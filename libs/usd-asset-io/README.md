# usdAssetIo

## Purpose

The transport-independent core. It fixes the random-access read contract every
backend implements, the typed diagnostic vocabulary every layer reports in, and
the counters that make this project's central claim checkable.

It is the one module that knows nothing about anything. It has no transport, no
cache, no asset format, and no OpenUSD, and it is the only module in the
repository that every other one is allowed to depend on.

**OpenUSD is not required.** This module builds and tests with plain CMake on a
machine with no OpenUSD installation present, and it includes no OpenUSD header.
That is invariant 2 of the [workspace contract](../../docs/architecture/WORKSPACE.md),
and the build graph enforces it: there is no `find_package` in this module's
`CMakeLists.txt` at all.

## Responsibilities

- The `AssetReader` random-access contract, and the `AssetMetadata` a reader
  reports.
- Validator value types, and the one classification -- `Stable`, `Unstable`,
  `Unavailable` -- that crosses the resolver boundary.
- The `StatusCode` vocabulary, `Status`, and the elision rule that keeps a
  credential out of a message.
- Offset and size arithmetic, in `ResolveReadRange`, so that the EOF boundary
  and the overflow check exist once rather than once per backend.
- Counter definitions, per-reader counter storage, latency histograms, and the
  process aggregate they fold into.

## Non-responsibilities

- No transport. Nothing here opens a file, a socket, or a connection.
- No caching, alignment, or coalescing. That is the decorator's job.
- No asset format. Bytes are opaque here and everywhere else in this
  repository.
- No OpenUSD types, and no plugin registration.
- No interpretation of a validator's contents. `Validator::value` is an opaque
  byte string; only the backend that produced it knows what it means.

## Public API

| Header | Contains |
| --- | --- |
| `usdAssetIo/AssetReader.h` | `AssetReader`, `AssetMetadata`, `ReadResult`, `OpenResult` |
| `usdAssetIo/Validator.h` | `Validator`, `ValidatorKind`, `ValidatorStrength`, `IdentityStability`, `ClassifyStability` |
| `usdAssetIo/Diagnostics.h` | `StatusCode`, `Severity`, `Status`, `ToString`, `ElideSecrets` |
| `usdAssetIo/RangeMath.h` | `ResolveReadRange`, `OverflowStatus`, `ShortReadStatus` |
| `usdAssetIo/Metrics.h` | `ReaderMetrics`, `MetricsSnapshot`, `LatencyHistogram`, `ScopedLatency`, `MetricsRegistry` |

`AssetReader` deliberately has two methods. Every addition to it has to justify
itself against a transport that cannot be expressed without it, and metrics are
the worked example: `ReaderMetrics` is reachable from a concrete backend, not
from the interface, because a counter accessor on `AssetReader` would widen the
one surface this project keeps narrow.

## Dependencies

The C++17 standard library and `Threads::Threads`. Nothing else, and nothing
third-party. The single third-party decision this repository makes is the HTTP
client, and it belongs to `usdAssetHttp` in `v0.2.0`.

## Data flow

```text
backend                       -> populates AssetMetadata at open
backend                       -> ResolveReadRange(offset, size, assetSize)
                                 -> Empty | Transfer(length) | Overflow
backend                       -> ReaderMetrics counters, per read
reader closes                 -> MetricsRegistry process aggregate
```

## Error and diagnostic behavior

Codes are the caller's branch point and messages are for humans; no caller
parses a message. The vocabulary and its `HTTPxxx` projection are in
[DIAGNOSTICS.md](../../docs/architecture/DIAGNOSTICS.md). Two rules are enforced
here rather than left to producers:

- `Severity` is derived from the code by `DefaultSeverity`, so two backends
  cannot disagree about whether cancellation is a fault.
- Every identifier that reaches a message or a metrics dump goes through
  `ElideSecrets`, which removes the query string **and** the userinfo component
  of an authority. `https://user:token@host/a` hides a credential in the part a
  query-only rule keeps.

## Threading and ownership

- `Status`, `Validator`, `AssetMetadata`, and `MetricsSnapshot` are plain
  values. They are as thread-safe as any value: safe to read from many threads,
  not safe to mutate while another thread reads.
- One `ReaderMetrics` may have every mutator and `Snapshot()` called
  concurrently from any number of threads. Its counters are relaxed atomics on
  the per-reader structure, not a shared global, and nothing on the read path
  allocates.
- `MetricsRegistry::Instance()` may be used from any thread. `Fold`,
  `Aggregate`, `TopAssetsByBytesTransferred`, and `Dump` are internally locked;
  `ResetForTesting` is for a single-threaded test and nothing else.
- `LatencyHistogram::Record` and `Snapshot` may be called concurrently.
  Quantiles are bucket-resolution estimates, never exact order statistics.
- `ResolveReadRange` and `ElideSecrets` are pure.

This module allocates no caller buffers and holds no reference to one.

## Network behavior

**None.** This module issues no request of any kind, over any transport, under
any condition. It has no code that could.

## Build and test

```sh
cmake -S . -B build/core -DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF
cmake --build build/core
ctest --test-dir build/core -R usdAssetIo
```

Under sanitizers, which are contract rather than an optional lane:

```sh
cmake --preset core-asan && cmake --build build/core-asan && ctest --preset core-asan
cmake --preset core-tsan && cmake --build build/core-tsan && ctest --preset core-tsan
```

| Test | Covers |
| --- | --- |
| `usdAssetIo_range_math` | The EOF boundary, truncation, and the overflow check, without a reader in the way |
| `usdAssetIo_diagnostics` | Code names, severity mapping, and the elision rules |
| `usdAssetIo_validator` | Classification, including a strength claimed without a kind |
| `usdAssetIo_metrics` | Counter monotonicity, concurrent recording, folding, and the derived ratios |

## Known limitations

- Latency quantiles are power-of-two bucket upper bounds. They never
  under-report and may over-report by up to a factor of two, which is the price
  of a histogram that neither allocates nor stores samples on the read path.
- `MetricsSnapshot::Add` cannot merge quantiles, because quantiles are not
  recoverable from two summaries. It zeroes them rather than averaging them
  into a number that would look like a measurement. The aggregate in
  `MetricsRegistry` folds the histograms themselves and does carry real
  quantiles.
- `MetricsRegistry` tracks at most 4096 distinct identifiers for its per-asset
  table. Beyond that, readers fold into the aggregate and are not listed
  individually.
- No performance property is claimed here. This repository's value proposition
  is a ratio, and the first recorded baseline arrives with the first release
  that moves bytes over a network.

## Planned work

| Item | Release |
| --- | --- |
| HTTP counters populated, including validator capture and conditional requests | `v0.2.0` |
| Cache counters populated, including `bytesOverFetched` | `v0.3.0` |
| Metrics dump surfaced through `USD_HTTP_RESOLVER_METRICS_DUMP` in a real host | `v0.2.0` |
| Asynchronous read surface | Not planned in v0.x; see §8 of the reader contract |

## Contracts implemented

- [ASSET_READER.md](../../docs/architecture/ASSET_READER.md)
- [DIAGNOSTICS.md](../../docs/architecture/DIAGNOSTICS.md)
- [METRICS.md](../../docs/architecture/METRICS.md)
- [WORKSPACE.md](../../docs/architecture/WORKSPACE.md)

When this README and one of those disagrees, the contract wins and this file is
the bug.
