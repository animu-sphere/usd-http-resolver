# Capability matrix

This document describes what the current tree implements. It is not a plan.
Intent lives in the [roadmap](../roadmap/README.md); contracts live in
[architecture/](../architecture/).

Last updated: 2026-08-16, against `main`.

## Summary

**Nothing is implemented.** The repository contains an OpenStrata project root
and this documentation set. There is no resolver, no backend, no cache, and no
plugin bundle.

A reader arriving from the design documents should read them as specifications
written before their implementation, which is deliberate: the boundary is this
project's product, and it is cheaper to settle in a document than across five
consumer repositories.

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
| Local file random access | planned (`v0.1.0`) | The correctness oracle; see [ASSET_READER.md](../architecture/ASSET_READER.md) |
| HTTP / HTTPS `GET` | planned (`v0.2.0`) | |
| HTTP range requests | planned (`v0.2.0`) | Single ranges only |
| Metadata request (`HEAD` or equivalent) | planned (`v0.2.0`) | Size, range support, validator |
| Redirects | planned (`v0.2.0`) | Bounded; no `https` to `http` downgrade |
| Timeouts | planned (`v0.2.0`) | Connect, read, and total |
| Bounded retry | planned (`v0.2.0`) | Counted in metrics, never silent |
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
| `ArAsset` range reads | planned (`v0.2.0`) | `GetBuffer()` returns null by design |
| Asset info and identity stability | planned (`v0.4.0`) | `Stable` / `Unstable` / `Unavailable` |
| `ArResolverContext` configuration | planned (`v0.6.0`) | Environment variables first |
| Write support | not planned | Fails explicitly |

## Cache and consistency

| Capability | Status | Notes |
| --- | --- | --- |
| In-memory block cache | planned (`v0.3.0`) | See [CACHE.md](../architecture/CACHE.md) |
| Request coalescing | planned (`v0.3.0`) | Measured gap and length thresholds |
| Single-flight de-duplication | planned (`v0.3.0`) | Tested under ThreadSanitizer |
| Bounded eviction | planned (`v0.3.0`) | Process-wide budget |
| Validator capture and `If-Range` | planned (`v0.4.0`) | `ETag`, else `Last-Modified` plus size |
| `AssetChanged` detection | planned (`v0.4.0`) | Never repaired silently |
| On-disk persistence | planned (`v0.4.0`), may defer | Requires validators to be safe |
| Content-addressed identity | not planned in v0.x | Revisited only for cross-stage sharing |
| Generated USD caching | not planned, ever | Owned by the consuming plugin repository |

## Diagnostics and metrics

| Capability | Status | Notes |
| --- | --- | --- |
| Typed status vocabulary | planned (`v0.1.0`) | See [DIAGNOSTICS.md](../architecture/DIAGNOSTICS.md) |
| `HTTPxxx` plugin codes | planned (`v0.2.0`) | Allocated in the diagnostics contract |
| Per-asset I/O counters | planned (`v0.1.0`) | Definitions; populated per backend |
| Cache counters | planned (`v0.3.0`) | Including `bytesOverFetched` |
| Latency distributions | planned (`v0.2.0`) | p50 / p90 / p99 / max |
| Recorded baselines | planned (`v0.2.0` onward) | A release changing I/O records one |

## Consumers

| Consumer | Status | Notes |
| --- | --- | --- |
| `usd-pointcloud-plugins` (COPC) | planned (`v0.5.0`) | First integration; see [consumer integration](../roadmap/consumer-integration.md) |
| `usd-3dgs-plugins` | deferred | Validates generality under camera-driven access |
| `usd-vrm-plugins` / containers | deferred | Nested random access |

No consumer is a build-time or test-time dependency of this repository, in any
release, per [ADR-0001](../adr/0001-consumer-interface.md).
