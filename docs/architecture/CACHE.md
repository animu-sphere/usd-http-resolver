# Cache contract

This document fixes block caching, request coalescing, single-flight
de-duplication, and cache identity. It is the contract for `usdAssetCache`,
which is a decorator over `AssetReader` and knows no transport concept.

Status: planned for `v0.3.0`, with validator-keyed identity and optional
persistence in `v0.4.0`. Nothing here is implemented.

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
blockSize        a power of two, default chosen by measurement in v0.3.0
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
[RESOLVER.md](RESOLVER.md). `validator` is the opaque token captured at open.

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
- Eviction is LRU by default, per process, with the budget shared across
  assets so one enormous asset cannot starve the rest of the stage.
- Eviction is invisible to correctness. An evicted block is re-fetched; it is
  never served stale, and never served zero-filled.

## 8. Persistence — Planned (`v0.4.0`)

An on-disk cache is admitted only after validators land, because a persistent
cache without a validator is a stale-data generator that survives restarts.

Requirements when it lands:

- entries are keyed by the same `CacheKey`, with the validator included;
- writes are atomic (write to a temporary path, then rename) so an interrupted
  process cannot publish a partial block;
- the cache directory is owned by the process, and no filename component is
  attacker-controllable — a URL never becomes a path;
- a corrupt or unreadable entry is discarded and re-fetched, never trusted;
- the cache is deletable at any time, and deleting it costs time only.

## 9. Statistics

The cache populates the counters in [METRICS.md](METRICS.md): hits, misses,
partial hits, bytes served from cache, bytes fetched, requests issued, requests
saved by coalescing, requests saved by single-flight, and evictions.

These are not diagnostics. They are the evidence for the project's central
claim, and a cache change without a before-and-after number is not reviewable.
