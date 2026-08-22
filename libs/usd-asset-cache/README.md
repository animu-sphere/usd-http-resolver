# usdAssetCache

## Purpose

An aligned block cache over `AssetReader`, so that the many small clustered
reads a format plugin issues stop becoming many small requests.

It is a decorator, not a backend. It holds a reader it did not construct, it
knows no transport concept, and it is keyed by an opaque validator. That is what
lets the local backend stay a usable oracle for the cached path: the shared
boundary suite runs against `local` and against `cache over local` and compares
the two, and it could not if this module knew what HTTP was.

Normative contract: [CACHE.md](../../docs/architecture/CACHE.md). The constants
it ships with, and the measurement that chose them, are
[BLOCK_POLICY.md](../../docs/reference/BLOCK_POLICY.md).

## Responsibilities

- Expanding a read to whole blocks, and serving it from them.
- Storing the final block of an asset at its true length, never padded.
- Merging adjacent and near-adjacent block fetches into one request, bounded by
  a maximum gap and a maximum length.
- Single-flight: concurrent readers that miss the same block issue one request,
  and the rest wait.
- Eviction under a bounded, process-wide memory budget, shared across assets.
- Bypassing itself entirely for a read large enough to be a streaming pass.
- Persisting blocks to a directory, for a `Strong` validator only, so that a
  later process pays nothing for what this one fetched.
- Populating the cache counters of
  [METRICS.md](../../docs/architecture/METRICS.md) §2.2, including
  `bytesOverFetched`, which is what block alignment costs.

## Non-responsibilities

- **No transport.** No URL, no header, no status code, no client library. It
  cannot tell what it is decorating.
- **No revalidation.** A reader is bound to one revision for its lifetime
  (ASSET_READER.md §2.1), the blocks this module holds for it were captured
  under that binding, and serving them is serving the bound revision.
  `AssetChanged` is reported by the reader underneath, on the reads that reach
  it. A hit reaches nothing and observes nothing.
- **No validator interpretation.** The validator's `value` is a byte string
  here and nothing else: never parsed, never compared to an `ETag`, never read
  for recency. Exactly one other field is read, `strength`, exactly once, to
  decide whether an entry may be shared with a reader that did not store it.
- **No persistence for a weak identity.** On-disk entries exist as of `v0.4.0`,
  and CACHE.md §8's table is the whole rule: `Strong` writes, `Weak` and `None`
  do not. Within one reader the revision binding carries the guarantee whatever
  the strength is, which is why those two still cache — privately, in memory,
  and dropped when the reader closes. Across processes there is no binding left
  and a weak match becomes a guess written to disk.
- **No cache directory of its own choosing.** Persistence is off unless a host
  names a directory. There is no default location.
- **No read-ahead.** A read is expanded to the blocks it touches and no
  further. Prefetch is research, not a feature of this release.
- **No format knowledge.** It stores bytes at offsets and has no idea whether
  they are a header, an index, or a point record.

## Public API

```text
usdAssetCache/CacheOptions.h        blockSize, budgetBytes, coalesceGapBlocks,
                                    maxRequestBytes, bypassThresholdBytes, and
                                    Normalized()
usdAssetCache/CacheKey.h            AssetIdentity, CacheKey, IsShareable
usdAssetCache/BlockCache.h          the block store, its bindings, and its stats
usdAssetCache/DiskBlockStore.h      the persistent tier, its options, Persistable
usdAssetCache/CachedAssetReader.h   the decorator, Wrap, and WrapAsset
```

`Wrap` takes the reader to decorate, that reader's `ReaderMetrics`, the options,
and the store. Passing the metrics is what makes a decorated stack report one
set of counters instead of two; the caller supplies it because only the caller
knows the concrete backend, and `AssetReader` deliberately carries no metrics
accessor.

`WrapAsset` is the same thing in the shape a backend's open returns. A failed
open passes through untouched, and so does a reader whose metadata says it
cannot serve random access — caching a reader that cannot seek would store one
block and miss forever after.

Both take a `DiskBlockStore*` as a last argument. Null takes the process store,
which is disabled until a host configures it, so a caller that says nothing gets
exactly the behavior it had before that tier existed.

## Dependencies

`usdAssetIo`, and the standard library's threading. Nothing else, ever:

- **OpenUSD is not required.** No file in this module includes an OpenUSD
  header, and the module builds and tests with plain CMake on a machine with no
  USD runtime installed. If that stops being true, the module is in the wrong
  directory (WORKSPACE.md invariant 2).
- No backend, no transport, no HTTP client, no third-party library at all.

## Data flow

```text
Read(offset, size)
  -> ResolveReadRange, once, in usdAssetIo
  -> size >= bypassThresholdBytes ?  straight to the reader underneath, stored
                                     nowhere
  -> otherwise: expand to whole blocks
       for each block: resident -> copy out
                       absent   -> claim it, and fetch
                       claimed  -> wait for whoever claimed it
       for each claimed block: on disk -> publish it, and copy out
       merge what is left into runs, bounded by gap and by length
       publish each fetched block, evict under the budget, and write it
         to disk when the identity is Stable
```

The disk is consulted after ownership and before the transport, which is where
single-flight has already reduced the readers asking for a block to one, and
outside the store's stripe locks — a file read under one would make every
unrelated block of that stripe wait on a seek.

## Error and diagnostic behavior

The same typed vocabulary every module uses, and nothing added to it. This
module produces exactly two statuses of its own:

| Condition | Code |
| --- | --- |
| `Wrap` given no reader to decorate | `InvalidArgument` |
| A fetch delivered fewer bytes than the block extent, below EOF | `InvalidResponse`, via the shared `ShortReadStatus` |

Everything else is the decorated reader's status, forwarded unchanged. A failed
read returns `bytesRead == 0`: the bytes a partial fetch left in the caller's
buffer are not reported as read.

A block claimed by a fetch that then fails is handed back rather than left
pending, and the readers waiting on it acquire it again and do the work
themselves. A waiter is never failed by another reader's transport.

## Threading and ownership

What may be called concurrently:

- Any number of threads may call `Read` on one `CachedAssetReader`, at any
  offsets, overlapping or not.
- Any number of `CachedAssetReader`s may share one `BlockCache`, and they do by
  default: the process store is the normal binding.
- `Metadata()`, `Options()`, `Metrics()`, and `SnapshotMetrics()` are callable
  from any thread at any time.

What may not: `BlockCache::ConfigureProcess` and `ClearForTesting` are not
concurrent with anything. The first is refused while any binding is alive and
says so by returning `false`; the second is for tests.

There is no global lock. The store is striped and every lock is per stripe, so
a read of one block never waits on a lock a read of an unrelated block holds
(§7 of the design policy). The number of stripes falls back toward one for a
small budget, which makes eviction order exact for a test that fills one.

Buffers: `dst` is caller-owned and this module retains no reference to it after
returning; it writes only within `[dst, dst + size)`. A cached block is
immutable once published and is held by `shared_ptr`, so eviction drops the
store's reference and the bytes live exactly as long as the last reader looking
at them.

**No network requests.** This module issues none and cannot: it has no
transport. Every byte it does not already hold, it asks the reader underneath
for.

## Build and test

```sh
cmake -S . -B build/core -DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF
cmake --build build/core
ctest --test-dir build/core -R usdAssetCache
```

| Test | What it is |
| --- | --- |
| `usdAssetCache_plan` | The arithmetic, with no reader and no thread: alignment, coalescing, the over-fetch charge, option normalization, key identity |
| `usdAssetCache_cache` | What the cache does to the reader underneath — requests, bytes, hits, eviction, identity sharing |
| `usdAssetCache_singleflight` | Threads. The ThreadSanitizer target for this module |
| `usdAssetCache_persistence` | The persistent tier: what a second process pays, what a weak validator may never write, what a scribbled entry costs, and what a hostile URL becomes on a filesystem |
| `boundary_cached_local_*` | The shared boundary suite, unchanged, over `cache over local` |
| `boundary_persisted_local_*` | The same suite over the same row with the disk tier underneath it |
| `usdAssetCache_block_policy` | The block-policy sweep, over a real socket, at five block sizes and four gaps |

The first four need nothing but a compiler. The last three live outside `libs/`,
because a module's tests must not depend on anything outside it.

All of them run under the sanitizer presets without a lane of their own, because
they are `libs/` and `tests/` and that is what `core-asan` and `core-tsan`
cover:

```sh
cmake --preset core-tsan && cmake --build --preset core-tsan
ctest --preset core-tsan
```

TSan is not optional for this module. Single-flight is where a naive
implementation deadlocks, and asserting a concurrency property in prose asserts
nothing.

## Known limitations

- **The coalescing gap does no work at the shipped block size.** Measured, and
  recorded rather than hidden: at 64 KiB blocks none of the recorded access
  patterns produces a gap to merge across, and gaps of 0, 1, 2 and 4 give
  identical numbers. It binds at 4 KiB blocks, where it is worth seven requests.
  See BLOCK_POLICY.md.
- **`bytesOverFetched` is charged at fetch time and never refunded.** A block
  fetched for one read and then consumed by fifteen more is still counted as
  over-fetch for the fourteen-fifteenths of it the first read did not want. The
  refund shows up in `cacheHitRatio` instead, and the two are reported together
  for that reason.
- **`peakResidentBytes` is an upper bound, not an exact high-water mark.** Each
  stripe tracks its own peak and the store sums them; the true peak of the sum
  is never higher and can be lower. Maintaining an exact one would take a lock
  across every stripe on every publish.
- **Sharing requires a strong validator.** A weak or absent one caches
  privately, for the reader's lifetime, and drops on close. That is stricter
  than CACHE.md §8's table, which admits in-memory caching at any strength; the
  strictness is about sharing between *two* readers, where the binding that
  makes weak safe no longer holds.
- **A read larger than the bypass threshold is never cached**, even when it is
  issued twice.

## Planned work

- `v0.4.0`: persistence, admitted per asset by validator strength, and the
  identity exposure that goes with it (CACHE.md §8).
- `v0.5.0`: the first measurement over real distance. The block size chosen
  here is chosen on request counts under a stated premise about round-trip cost
  that loopback cannot price; `v0.5.0` is where the premise gets tested.
- `v0.6.0`: cache configuration through `ArResolverContext`, so that two stages
  in one process can have two policies.
