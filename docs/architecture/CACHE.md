# Cache contract

This document fixes block caching, request coalescing, single-flight
de-duplication, and cache identity. It is the contract for `usdAssetCache`,
which is a decorator over `AssetReader` and knows no transport concept.

Status: implemented in `libs/usd-asset-cache`. The block model, coalescing,
single-flight, the key, and eviction landed in `v0.3.0`; §8, persistence, landed
in `v0.4.0`. The constants in §3 and §4 are measured and the measurement is
[BLOCK_POLICY.md](../reference/BLOCK_POLICY.md); the counters in §9 populate and
are recorded in [BASELINE.md](../reference/BASELINE.md).

The architecture below is unchanged by the `v0.2.0` reordering; what changed is
that the validator it keys on already existed when the cache landed. There is no
interim URL-keyed cache and no migration away from one.

Two things the implementation makes more specific than this document did, and
both are narrower rather than wider:

- **A read *at least* as large as the bypass threshold bypasses**, where §3 says
  "larger than". The boundary had to fall somewhere and it falls on the side
  that caches less.
- **Sharing an entry between two readers requires a strong validator.** §6 keys
  every entry on the validator, and §8 makes strength the test for outliving a
  reader; the implementation applies the same test one level earlier, to
  outliving *this* reader. A weak or absent validator still caches, privately,
  for the reader's lifetime, which is what §6 promises.

## 1. Which cache this is

Two caches exist in the whole system, and confusing them is the failure this
section prevents:

```text
raw byte / range cache          -> this repository        (this document)
generated USD / payload cache   -> the consuming plugin   (their contract)
```

This cache stores **bytes at offsets**. It has no idea whether those bytes are
a header, an index, or a point record. The consumer's cache stores generated
USD and is keyed by generation parameters this repository has never heard of.
The two never merge, never share a key, and never invalidate each other.

## 2. Why a block cache

A FileFormat Plugin reading a structured asset issues many small, clustered
reads: a header, then a directory, then a scatter of records. Served naively,
each becomes an HTTP request, and the per-request latency dominates by orders
of magnitude.

```text
requested:  1_258_291 .. 1_270_000        (11 KB, 4 reads)
fetched:    1_048_576 .. 2_097_152        (1 block, 1 request)
```

Alignment converts request count into transferred bytes, which is the right
trade on a high-latency link and the wrong trade on a local file. That is why
the cache is a decorator: the local backend is used without it.

## 3. Block model

```text
blockSize        a power of two; 65536 by default, chosen by measurement
blockIndex       offset / blockSize
blockRange       [blockIndex * blockSize, (blockIndex + 1) * blockSize)
```

Rules:

- A read is expanded to cover whole blocks, then satisfied from them.
- The final block of an asset is short and is cached at its true length. A
  cache that pads it produces a read past EOF that returns zeros — a silent
  corruption that looks exactly like valid data.
- Block size is fixed per reader for its lifetime. A read larger than a
  configured threshold bypasses the cache entirely rather than evicting the
  whole working set to store one streaming pass.

## 4. Coalescing

Adjacent and near-adjacent missing blocks become one request:

```text
missing blocks:  [5] [6] [8]        gap of one block between 6 and 8
one request:     blocks 5..8        (fetches block 7 unnecessarily)
```

The policy is a maximum gap and a maximum merged length, both measured
constants:

- merge across a gap when the gap is smaller than the threshold, because
  transferring the gap costs less than a second round trip;
- never merge beyond the maximum length, because one enormous request defeats
  cancellation and stalls every other read on the connection.

Both numbers are recorded with the measurement that produced them, per
[METRICS.md](METRICS.md). A tuned constant without a recorded measurement is a
guess with a decimal point.

They are, and the record is [BLOCK_POLICY.md](../reference/BLOCK_POLICY.md): a
maximum gap of one block and a maximum merged length of 8 MiB. The record also
says the thing a tuning document is most tempted to leave out — that at the
shipped block size the gap does not currently bind on any measured pattern. It
is 1 because that is the entire measured benefit where the benefit exists, and
because 2 and 4 were measured and bought nothing anywhere.

## 5. Single-flight

Concurrent readers that miss the same block issue **one** request. The second
arrival waits on the first rather than starting its own.

This is correctness-adjacent, not merely an optimization: N Hydra threads
opening one asset produce N identical requests, N times the bytes, and a
metrics report that is off by a factor of N. It is also where a naive
implementation deadlocks, so it is tested under ThreadSanitizer with readers
racing on overlapping ranges.

Locking is per block. A global lock over the cache serializes every read in the
stage and is forbidden by §7 of the [design policy](../design/DESIGN_POLICY.md).

## 6. Cache identity

The key is:

```text
CacheKey = resolvedIdentifier + validator + blockSize + blockIndex
```

`resolvedIdentifier` is the normalized absolute URI after redirects, per
[RESOLVER.md](RESOLVER.md). `validator` is `Validator::value` — the opaque
token captured at open, treated here as a byte string and nothing more.

The cache never parses that value, never compares it to an `ETag`, and never
infers recency from it. It reads exactly one other field, `strength`, and
exactly once: to decide whether an entry may outlive the reader (§8). Every
other validator question belongs to the backend, per §7.1 of
[ASSET_READER.md](ASSET_READER.md). The moment this layer can parse an HTTP
construct it has become an HTTP cache, and the local backend stops being a
usable oracle for the cached path.

The key's identity half — identifier, validator, block size — is interned by the
store, so a per-block lookup costs two integers rather than two string
comparisons. That is an implementation detail of where the strings live and not
of what the key is: two readers whose identities compare equal share entries,
and two whose identities differ never do, which is what this section is about.

The rule that follows is the whole point:

```text
equal identifiers never imply equal content
```

A URL match alone is never a hit. Two revisions published at one URL are two
cache identities, and an entry from revision A must never serve a read of
revision B. When the validator is absent — `IdentityStability::Unavailable` —
the in-memory cache still functions for the reader's lifetime, because the
reader is bound to one revision by `If-Range`, but nothing persists beyond that
reader.

## 7. Eviction

- The cache has a bounded memory budget. It never grows to the asset size.
  128 MiB by default.
- Eviction is LRU by default, per process, with the budget shared across
  assets so one enormous asset cannot starve the rest of the stage.
- Eviction is invisible to correctness. An evicted block is re-fetched; it is
  never served stale, and never served zero-filled.

The implementation stripes the store, and the eviction order is therefore LRU
within a stripe rather than globally. That is a consequence of §5's ban on a
global lock — a store-wide LRU order needs a store-wide lock on every hit — and
it is admissible precisely because of the third rule above: eviction is
invisible to correctness, so an approximate order costs a re-fetch and nothing
else. The stripe count falls back toward one for a small budget, which makes the
order exact where a test can see it.

## 8. Persistence

Implemented in `v0.4.0`, as `DiskBlockStore`.

An on-disk cache is admitted only after validators land, because a persistent
cache without a validator is a stale-data generator that survives restarts.

Persistence is admitted per asset, not per deployment, and the test is validator
strength:

| Validator strength | In-memory, for the reader's lifetime | Persistent across opens |
| --- | --- | --- |
| `Strong` | yes | yes |
| `Weak` | yes | no |
| `None` | yes | no |

A weak validator cannot prove two responses are byte-identical — that is what
weak means — and `Last-Modified` at one-second granularity cannot separate two
revisions published inside the same second. Within one reader's lifetime the
binding in §2.1 of [ASSET_READER.md](ASSET_READER.md) carries the guarantee, so
in-memory caching is safe regardless. Across opens there is no binding left, and
a weak match becomes a guess written to disk.

The requirements, and what each one is in the tree:

| Requirement | As implemented |
| --- | --- |
| Entries keyed by the same `CacheKey`, validator included | The file name is half a SHA-256 digest of identifier, validator, block size, and block index; the whole digest is written inside the entry and compared before a cached byte is used, so a name collision costs a miss and can never serve one asset's bytes for another's |
| Written only when the reader's identity is `Stable` | `Persistable`, answered once at open from the validator captured at open. A weak or absent one neither writes nor reads: it is not that the entry would be missing, it is that this tier is not consulted at all |
| Atomic writes | The entry is built whole in memory, written to a temporary in the destination's own directory, and renamed into place. A rename that loses a race drops the temporary and reports nothing — the loser's bytes were identical to the winner's |
| A directory the process owns, no attacker-controllable filename component | Every component is hexadecimal. A URL is digested, never spelled; `http://host/../../etc/passwd` is thirty-two hex characters like every other identifier |
| A corrupt entry discarded, never trusted | Header and body are checksummed separately. A structurally broken entry is deleted on the way out rather than re-read and re-rejected forever; one that is intact but names another identity is left where it is, because it is somebody's valid entry |
| Deletable at any time, costing time only | Every operation is best effort and its worst outcome is `false`, which is a miss. The directory vanishing under a live reader is a case with a test |

Three things the implementation adds that this section did not ask for. The
first is a bound, and the other two are the same refusal read twice:

- **The directory has a budget**, 1 GiB by default. An unbounded cache is a disk
  nobody can plan for. It is enforced by a sweep on an interval rather than on
  every write, so the ceiling is the budget plus one interval's writes, and the
  eviction order is oldest-written first rather than least-recently-used —
  refreshing a timestamp on every hit would turn a read into a write. Eviction
  here is invisible to correctness for the reason §7 gives in memory, so an
  approximate order costs a re-fetch and nothing else.
- **Persistence is off unless a directory is named.** There is no default
  location. A resolver that started writing to a disk nobody named would be a
  surprise, and the variables that turn it on are in
  [CONFIGURATION.md](../reference/CONFIGURATION.md) §2.
- **Nothing under the cache directory is reversible to a URL.** The identity is
  written as a digest and never as itself, because a resolved identifier can be
  a signed URL and [gate 7](../releases/README.md) forbids a credential in a
  persisted artifact. A cache entry is the first artifact this project
  persists, and it is the one place `ElideSecrets` cannot run — an entry has no
  message to elide, it has a key. So the key is not written down.

Where the tier sits is between owning a block and fetching it, in
`CachedAssetReader` rather than in `BlockCache`. The memory store answers under
a stripe mutex, and a file read under that mutex would make every unrelated
block of the same stripe wait on a disk seek — the objection §5 makes to a
global lock, where the wait is shorter. Single-flight already puts exactly one
reader per block at that point, so the disk is read once per block and everyone
waiting on it is served by the publish that follows.

## 9. Statistics

The cache populates the counters in [METRICS.md](METRICS.md): hits, misses,
partial hits, bytes served from cache, bytes fetched, requests issued, requests
saved by coalescing, requests saved by single-flight, and evictions.

These are not diagnostics. They are the evidence for the project's central
claim, and a cache change without a before-and-after number is not reviewable.

`v0.3.0` records its before and after in one table:
[BASELINE.md](../reference/BASELINE.md) measures every scenario twice, with the
cache and without it, in one run of one harness.

The counters are per reader, and a decorated stack has one reader as far as the
counters are concerned — the outermost. The two ends of a stack disagree about
what `bytesRequested` means, so the decorator keeps what the caller asked for
and takes from the reader underneath only what crossed the transport
(`ReaderMetrics::AbsorbTransport`). A stack that folded both would compute
`amplification` over a denominator that is two different measurements added
together.
