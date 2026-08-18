# Capability matrix

This document describes what the current tree implements. It is not a plan.
Intent lives in the [roadmap](../roadmap/README.md); contracts live in
[architecture/](../architecture/).

Last updated: 2026-08-19, against `main`.

## Summary

**The read contract, the local backend, the shared boundary suite, the
hostile-server corpus, the HTTP backend, and the `ArResolver` bundle are
implemented. A `UsdStage` opens over HTTP. There is still no cache.**

`libs/usd-asset-io` fixes the `AssetReader` contract, the typed diagnostic
vocabulary, the validator value types, and the metrics counters.
`libs/usd-asset-local` implements all of it over a local file, including a
filesystem-derived validator and `AssetChanged` on a mid-read republish.
`tests/boundary` is the shared suite that admits every later backend, and the
local backend passes it: the required boundary cases, biased property cases
against an independent naive oracle, and the concurrency cases.

`libs/usd-asset-http` is the newest thing here, and the first thing in this
repository that touches a network. It serves byte ranges over `http` and
`https`, validates response framing against the request rather than against
itself, bounds its redirects and retries, and binds each reader to the revision
it opened — validator capture, a conditional guard on every range request, and
`AssetChanged`, from its first commit. It passes the `v0.1.0` boundary suite
**unchanged**, which was the point of building the suite first, and it is
byte-equivalent to the local backend over every fixture size and 10,000
generated cases.

`tests/fixture-server` is no longer a passing oracle waiting for a subject.
`tests/corpus` is the subject: every one of the 18 named behaviors is projected
onto a `StatusCode`, and the coverage is asserted at runtime rather than
claimed. Neither side knows the other — nothing in the fixture server has heard
of `StatusCode`, and nothing in the backend has heard of `Behavior` — so a
disagreement between them is evidence rather than a tautology.

`plugins/http-resolver` is what closed the gap between that backend and a
consumer. It registers `http` and `https` as URI schemes, normalizes an
identifier once, anchors a relative reference to the remote layer it was
authored in, hands out an `ArAsset` whose `GetBuffer` is null by contract, and
emits the `HTTPxxx` codes the diagnostics contract allocated. A consumer opens a
remote stage with no HTTP code of its own; `httpResolver_test_stage` does
exactly that against the hostile fixture corpus, over a real socket.

What is still missing is the cache: every read is a request, deliberately, so
that the request pattern is visible before it is optimized. Identity is captured
and used but not yet exposed to consumers, which is `v0.4.0`. The release's I/O
baseline is recorded: `tests/baseline` runs the five scenarios METRICS.md §6
requires against a 128 MiB fixture on loopback, and the numbers are in
[BASELINE.md](BASELINE.md). A bounded query moves a quarter of one percent of
that asset and every byte of it is a byte the caller asked for.

The whole tree still builds and tests with
`-DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF` on a machine with no OpenUSD
installation present, which is the path `v0.1.0` was defined by and which
`v0.2.0` preserves — libcurl is a `find_package`, not a USD runtime, and the one
directory that needs a runtime is the bundle.

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
| HTTP / HTTPS `GET` | implemented | `libs/usd-asset-http`, reached from `plugins/http-resolver` through `ArResolver` and from nowhere else |
| HTTP range requests | implemented | Single ranges only. Framing validated against the request, not against itself |
| Metadata request (`HEAD`) | implemented | One round trip; size, range support, validator, content type |
| Metadata request where `HEAD` is unavailable | not implemented | A `405` or `501` is reported as `Unsupported`. The fallback §4.1 admits would ship unexercised: the corpus has no row that refuses `HEAD` |
| Redirects | implemented | Bounded by `maxRedirects` (default 5); `301`, `302`, `307`, `308`. `303` is not followed. An `https` → `http` `Location` is refused |
| Timeouts | implemented | Connect, response, and transfer, separately, and the failure names which elapsed |
| Bounded retry | implemented | `maxAttempts` (default 3), on `429`/`502`/`503`/`504` and on connection failures where nothing came back. Never on a deadline. One budget per logical operation, shared with the resume loop rather than nested inside it. Counted in metrics |
| Malformed-framing refusals | implemented | Conflicting duplicate `Content-Length` or `Content-Range` lines, per RFC 9110 §8.6; a `Content-Range` describing bytes past the representation it claims to be part of; a length that would wrap |
| Resume of a short transfer | implemented | The remainder is re-requested from where it stopped, bounded by the same budget; past it, `InvalidResponse` |
| Range unsupported → hard error | implemented | `RangeNotSupported`, at open when `Accept-Ranges` is absent and at the first read when it was advertised and then ignored. No whole-asset fallback, per [ADR-0002](../adr/0002-range-unsupported-policy.md) |
| Response body bounded by the request | implemented | The caller's buffer is the bound. A `200` answering a 64 KiB range request moves 64 KiB and is cut off |
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
| `http` / `https` URI scheme registration | implemented | One type, two schemes. A scheme resolver: the host's primary resolver is unchanged, and a local asset opens exactly as it did |
| URI normalization | implemented | Query preserved verbatim, fragment and userinfo removed, encoding normalized, dot segments resolved after decoding. See [RESOLVER.md](../architecture/RESOLVER.md) §2.1 |
| Relative asset resolution against a remote anchor | implemented | RFC 3986 §5.2 reference resolution. A relative path anchored to a *local* layer is left to the primary resolver |
| `ArAsset` range reads | implemented | `Read` and `GetSize` are the whole path. A failed read returns 0 bytes and a diagnostic, never the bytes it had |
| Extension detection through a query string | implemented | `GetExtension` ignores the query and the fragment; the default would return `usda?X-Amz-Signature=…` and match no file format |
| One metadata request per resolution, reused by the open | implemented | The reader opened by `Resolve` is retained and handed to the next `OpenAsset`, once. Concurrent resolutions of one identifier make one request |
| Absence distinguished from failure | implemented | A `404` is an empty path and no diagnostic; a transport fault is an empty path *and* an `HTTPxxx` error |
| `GetBuffer()` whole-asset materialization | not planned, ever | Returns null by contract; see §4.1 of [RESOLVER.md](../architecture/RESOLVER.md) |
| Interoperability with whole-buffer FileFormat Plugins | not planned, ever | Incompatible with the remote random-access path by construction |
| Asset info and identity stability | planned (`v0.4.0`) | `Stable` / `Unstable` / `Unavailable` exposed to consumers |
| Environment-variable configuration | implemented | The five transport bounds in [CONFIGURATION.md](CONFIGURATION.md); a bad value warns and takes the default |
| `ArResolverContext` configuration | planned (`v0.6.0`) | Per stage; the environment form is a process-wide bootstrap |
| Write support | not planned | Fails explicitly |

## Cache and consistency

| Capability | Status | Notes |
| --- | --- | --- |
| Validator value types and stability classification | implemented | `libs/usd-asset-io`; `Stable` / `Unstable` / `Unavailable` derived once, at open |
| Validator capture, local | implemented | Derived from device, file id, size, and mtime; declared strong, with the caveat in the [module README](../../libs/usd-asset-local/README.md) |
| Validator capture, HTTP | implemented | Strong `ETag`, else weak `ETag`, else `Last-Modified`, else none. Classified once, at open |
| Conditional range requests (`If-Range`) | implemented | On every range request after open, where the captured validator admits one. Not sent for a weak `ETag`, per RFC 9110 §13.1.5 |
| `If-Range` with a `Last-Modified` validator | implemented, not covered by the corpus | The fixture server compares `If-Range` only against its `ETag`, so the server-side half cannot be exercised there. Unit-tested against a scripted transport |
| Revision binding, one reader to one revision | implemented | Both backends. A correctness property of range reads, not of the cache |
| `AssetChanged` detection | implemented | Local: the file identity re-derived after every transferring read. HTTP: two independent detectors — a `200` answering a conditional range, and a response whose validator or complete length contradicts the capture, including a `416` whose `bytes */<length>` does. Never repaired silently, never rebound |
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
| `HTTPxxx` plugin codes | implemented | Every code in the table except `HTTP102`, which ADR-0002 defers. Errors as `TF_RUNTIME_ERROR`, cancellation as `TF_WARN`, an impossible request as `TF_CODING_ERROR` |
| Per-asset I/O counters | implemented | Defined in `usdAssetIo`, populated by both backends, folded into a process aggregate. The HTTP backend populates `requestCount`, `metadataRequestCount`, `retryCount`, `redirectCount`, `bytesRequested`, `bytesTransferred`, and all three latency histograms |
| Cache counters | defined, not populated | Fields exist and stay at zero until `libs/usd-asset-cache` in `v0.3.0` |
| Latency distributions | implemented | p50 / p90 / p99 / max, as power-of-two bucket estimates |
| Metrics dump on `USD_HTTP_RESOLVER_METRICS_DUMP` | implemented | Aggregate plus top assets, at process exit, to stderr |
| Recorded baselines | implemented | `tests/baseline`, the five scenarios in METRICS.md §6 against a 128 MiB loopback fixture. The record is [BASELINE.md](BASELINE.md); byte and request counts are asserted, ratios and durations are reported |

## Testing and build

| Capability | Status | Notes |
| --- | --- | --- |
| Libs-first root, OpenUSD-optional | implemented | `-DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF`; the `core` CMake preset, and `core-msvc` for the same build through the Visual Studio generator |
| Shared boundary suite | implemented | `tests/boundary`; parameterized over backends, one row per transport |
| Property-based read tests | implemented | Biased generators over `assetSize`, `offset`, `readSize`, with shrinking and a reported seed |
| Concurrency tests | implemented | Concurrent reads on one reader, and concurrent readers on one asset |
| Local revision-change simulation | implemented | The suite rewrites the fixture underneath an open reader |
| ASan / UBSan / TSan builds of `libs/` | implemented | `USD_HTTP_RESOLVER_SANITIZER`, the `core-asan` / `core-tsan` presets, and the `sanitizers` job in `core-ci.yml`. Halting on a UBSan report is `-fno-sanitize-recover=all`; without it the lane reports and passes |
| CI: core build and test, three platforms, no OpenUSD | implemented | `core-ci.yml`, `core` job: Windows, Linux, macOS arm64. Asserts from the configure log that `find_package(pxr)` was never reached |
| CI: sanitizer cells | implemented | `core-ci.yml`, `sanitizers` job, Linux. A sanitizer is a property of the compiler, and MSVC implements only `address` — unverified at that |
| CI: generated OpenStrata support matrix | implemented | `openstrata.ci.yaml`, six `pull_request` cells, and `ost-source-ci.yml` generated from it: the dependency graph on Linux and Windows, `ost build` + `ost test` on Linux and macOS arm64, and the bundle through the pyramid to L1 on the same two. The L1 cap is [report 02](../reports/ost/02-2026-08-18-resolver-bundle-under-the-pyramid.md) §2 |
| CI: plugin lane on Windows | implemented | `plugin-windows-ci.yml`, hand-authored: no generated cell can hand CMake the vcpkg prefix libcurl needs. It reads its pins back out of `openstrata.ci.yaml` and asserts `httpResolver_stage` by name from the `ctest` log; see [report 03](../reports/ost/03-2026-08-18-a-support-matrix-with-one-hand-authored-lane.md) |
| Hostile-server fixture corpus | implemented | `tests/fixture-server`; 18 behaviors covering all nine conditions in §11.2 of the design policy. Additional to the boundary suite, not a substitute |
| Fixture-server self-test | implemented | Asserts over a raw socket that each behavior puts on the wire what its name claims, with a client that shares no HTTP code with the server |
| Corpus projection onto the typed vocabulary | implemented | `tests/corpus`; every behavior maps to a `StatusCode`, and coverage against `AllBehaviors()` is asserted at runtime rather than claimed |
| Boundary suite against the HTTP backend | implemented | `tests/boundary/backends/boundary_http_main.cpp`, one row, running the suite unchanged over a real server and a real socket |
| Remote stage opened end to end | implemented | `httpResolver_test_stage`: a `UsdStage` over loopback HTTP, a relative reference followed to a second remote layer, and the `Range` header the server actually received |
| Resolver logic tested without a USD runtime | implemented | Normalization, configuration, and the `HTTPxxx` projection each link one translation unit and nothing else |
| Redirect scheme-downgrade rejection | implemented | Not in the corpus and cannot be: the fixture server speaks plaintext HTTP, so there is no `https` to downgrade from. Tested in `usdAssetHttp` against a scripted `Location` |
| Mid-read revision-change tests, HTTP | implemented | Both halves: `ValidatorChangeMidRead` in the corpus projection, and the boundary suite's own republish-underneath-an-open-reader case |
| No credential in a message, asserted | implemented | The corpus projection opens a failing URL carrying userinfo and a query token and checks the rendered status for both |
| Amplification baselines | implemented | `tests/baseline`, registered as `usdAssetHttp_io_baseline` so that a byte count which moves fails a lane rather than waiting for a release run. `amplification` is exactly 1.0 in every scenario that moves a byte, because there is no cache to over-fetch |

## Consumers

| Consumer | Status | Notes |
| --- | --- | --- |
| `usd-pointcloud-plugins` (COPC) | planned (`v0.5.0`) | First integration; see [consumer integration](../roadmap/consumer-integration.md) |
| `usd-3dgs-plugins` | deferred | Validates generality under camera-driven access |
| `usd-vrm-plugins` / containers | deferred | Nested random access |

No consumer is a build-time or test-time dependency of this repository, in any
release, per [ADR-0001](../adr/0001-consumer-interface.md).
