# Capability matrix

This document describes what the current tree implements. It is not a plan.
Intent lives in the [roadmap](../roadmap/README.md); contracts live in
[architecture/](../architecture/).

Last updated: 2026-08-16, against `main`.

## Summary

**The read contract, the local backend, and the shared boundary suite are
implemented. No network code exists.**

`libs/usd-asset-io` fixes the `AssetReader` contract, the typed diagnostic
vocabulary, the validator value types, and the metrics counters.
`libs/usd-asset-local` implements all of it over a local file, including a
filesystem-derived validator and `AssetChanged` on a mid-read republish.
`tests/boundary` is the shared suite that admits every later backend, and the
local backend passes it: the required boundary cases, biased property cases
against an independent naive oracle, and the concurrency cases.

There is still no resolver, no HTTP, no cache, and no plugin bundle. The whole
tree builds and tests with `-DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF` on a machine
with no OpenUSD installation present, which is the path `v0.1.0` is defined by.

A reader arriving from the design documents should read the parts still marked
*planned* as specifications written before their implementation, which is
deliberate: the boundary is this project's product, and it is cheaper to settle
in a document than across five consumer repositories.

## Status language

```text
implemented                   in the tree, tested
implemented, not connected    exists in a library, nothing reaches it
planned                       has a contract, no implementation
not planned                   explicitly out of scope
```

## Transport

| Capability | Status | Notes |
| --- | --- | --- |
| Local file random access | implemented | `libs/usd-asset-local`; the correctness oracle. Positional reads, no lock, no network |
| HTTP / HTTPS `GET` | planned (`v0.2.0`) | |
| HTTP range requests | planned (`v0.2.0`) | Single ranges only |
| Metadata request (`HEAD` or equivalent) | planned (`v0.2.0`) | Size, range support, validator |
| Redirects | planned (`v0.2.0`) | Bounded; no `https` to `http` downgrade |
| Timeouts | planned (`v0.2.0`) | Connect, read, and total |
| Bounded retry | planned (`v0.2.0`) | Counted in metrics, never silent |
| Range unsupported → hard error | planned (`v0.2.0`) | `RangeNotSupported`; no whole-asset fallback, per [ADR-0002](../adr/0002-range-unsupported-policy.md) |
| Bounded whole-asset fallback | deferred | Its own residency model; needs a new ADR and a demonstrated need |
| Multipart ranges | not planned in v0.x | Admitted only if measured to beat coalescing |
| Authentication | not planned in v0.x | An interception point arrives in `v0.6.0`; no provider |
| S3-compatible backend | not planned in v0.x | Deferred; must fit the contract unchanged |
| Package-internal ranges | not planned in v0.x | Deferred |
| Wasm `fetch` backend | research | Constrains the HTTP client choice made in `v0.2.0` |
| Writing or upload | not planned | Assets are immutable |

## Resolver

| Capability | Status | Notes |
| --- | --- | --- |
| `http` / `https` URI scheme registration | planned (`v0.2.0`) | Scheme resolver; the primary resolver is unchanged |
| URI normalization | planned (`v0.2.0`) | Query string preserved verbatim; see [RESOLVER.md](../architecture/RESOLVER.md) |
| Relative asset resolution against a remote anchor | planned (`v0.2.0`) | RFC 3986 reference resolution |
| `ArAsset` range reads | planned (`v0.2.0`) | `Read` and `GetSize` are the whole path |
| `GetBuffer()` whole-asset materialization | not planned, ever | Returns null by contract; see §4.1 of [RESOLVER.md](../architecture/RESOLVER.md) |
| Interoperability with whole-buffer FileFormat Plugins | not planned, ever | Incompatible with the remote random-access path by construction |
| Asset info and identity stability | planned (`v0.4.0`) | `Stable` / `Unstable` / `Unavailable` exposed to consumers |
| `ArResolverContext` configuration | planned (`v0.6.0`) | Environment variables first |
| Write support | not planned | Fails explicitly |

## Cache and consistency

| Capability | Status | Notes |
| --- | --- | --- |
| Validator value types and stability classification | implemented | `libs/usd-asset-io`; `Stable` / `Unstable` / `Unavailable` derived once, at open |
| Validator capture, local | implemented | Derived from device, file id, size, and mtime; declared strong, with the caveat in the [module README](../../libs/usd-asset-local/README.md) |
| Validator capture, HTTP | planned (`v0.2.0`) | Kind and strength; `ETag`, else `Last-Modified` plus size |
| Conditional range requests (`If-Range`) | planned (`v0.2.0`) | On every range request after open |
| Revision binding, one reader to one revision | implemented for the local backend | A correctness property of range reads, not of the cache. The HTTP half lands in `v0.2.0` |
| `AssetChanged` detection | implemented for the local backend | Re-derived after every transferring read; never repaired silently, never rebound |
| In-memory block cache | planned (`v0.3.0`) | See [CACHE.md](../architecture/CACHE.md); validator-keyed from the start |
| Request coalescing | planned (`v0.3.0`) | Measured gap and length thresholds |
| Single-flight de-duplication | planned (`v0.3.0`) | Tested under ThreadSanitizer |
| Bounded eviction | planned (`v0.3.0`) | Process-wide budget |
| On-disk persistence | planned (`v0.4.0`), may defer | Strong validator only |
| Content-addressed identity | not planned in v0.x | Revisited only for cross-stage sharing |
| Generated USD caching | not planned, ever | Owned by the consuming plugin repository |

## Diagnostics and metrics

| Capability | Status | Notes |
| --- | --- | --- |
| Typed status vocabulary | implemented | `StatusCode`, `Severity`, `Status`; see [DIAGNOSTICS.md](../architecture/DIAGNOSTICS.md) |
| Credential elision in messages and dumps | implemented | Query string and authority userinfo both removed, visibly |
| `HTTPxxx` plugin codes | planned (`v0.2.0`) | Allocated in the diagnostics contract |
| Per-asset I/O counters | implemented | Defined in `usdAssetIo`, populated by the local backend, folded into a process aggregate |
| Cache counters | defined, not populated | Fields exist and stay at zero until `libs/usd-asset-cache` in `v0.3.0` |
| Latency distributions | implemented | p50 / p90 / p99 / max, as power-of-two bucket estimates |
| Metrics dump on `USD_HTTP_RESOLVER_METRICS_DUMP` | implemented | Aggregate plus top assets, at process exit, to stderr |
| Recorded baselines | planned (`v0.2.0` onward) | A release changing I/O records one; `v0.1.0` moves no bytes over a network |

## Testing and build

| Capability | Status | Notes |
| --- | --- | --- |
| Libs-first root, OpenUSD-optional | implemented | `-DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF`; the `core` CMake preset |
| Shared boundary suite | implemented | `tests/boundary`; parameterized over backends, one row per transport |
| Property-based read tests | implemented | Biased generators over `assetSize`, `offset`, `readSize`, with shrinking and a reported seed |
| Concurrency tests | implemented | Concurrent reads on one reader, and concurrent readers on one asset |
| Local revision-change simulation | implemented | The suite rewrites the fixture underneath an open reader |
| ASan / UBSan / TSan builds of `libs/` | implemented as build configuration | `USD_HTTP_RESOLVER_SANITIZER`, and the `core-asan` / `core-tsan` presets. Not yet run by a CI cell; see [implementation status](../roadmap/implementation-status.md) |
| Hostile-server fixture corpus | planned (`v0.2.0`) | Additional to the boundary suite, not a substitute |
| Mid-read revision-change tests | planned (`v0.2.0`) | Local backend and fixture server both simulate it |
| Amplification baselines | planned (`v0.2.0` onward) | A release changing I/O records one |

## Consumers

| Consumer | Status | Notes |
| --- | --- | --- |
| `usd-pointcloud-plugins` (COPC) | planned (`v0.5.0`) | First integration; see [consumer integration](../roadmap/consumer-integration.md) |
| `usd-3dgs-plugins` | deferred | Validates generality under camera-driven access |
| `usd-vrm-plugins` / containers | deferred | Nested random access |

No consumer is a build-time or test-time dependency of this repository, in any
release, per [ADR-0001](../adr/0001-consumer-interface.md).
