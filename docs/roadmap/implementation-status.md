# Implementation status

Task-level tracking of what is done, in progress, and outstanding. Behavior
belongs in [capability matrix](../reference/CAPABILITY_MATRIX.md); this file
tracks work.

Last updated: 2026-08-16.

Phase 1 is complete except for its CI cells. The read contract, the local
backend, and the shared boundary suite are in the tree and passing; the
sanitizer builds are configured but have not been run by any cell, which is
tracked as a blocking item below.

## Phase 0 — scaffolding and contracts

| Task | Status |
| --- | --- |
| OpenStrata project initialized (`usd-plugin-workspace`, `cy2026` / `usd`) | Done |
| Documentation taxonomy established | Done |
| Design policy | Done |
| Roadmap and release sequence | Done |
| Workspace contract | Done |
| Asset reader contract | Done |
| Resolver contract | Done |
| Cache contract | Done |
| Diagnostics contract and `HTTPxxx` allocation | Done |
| Metrics contract | Done |
| Boundary suite contract | Done |
| Libs-first, OpenUSD-optional root build graph | Done |
| ADR-0001: consumer interface | Accepted |
| ADR-0002: range-unsupported policy | Accepted — hard error in `v0.2.0` |
| `openstrata.ci.yaml` and generated workflows | Outstanding |
| `LICENSE`, `NOTICE`, `VERSION` | Done |
| Module README contract applied to real modules | Done — both `libs/` modules |
| OpenStrata plain-library descriptors for `libs/` modules | Done |

## Phase 1 — read contract, local backend, boundary suite (`v0.1.0`)

| Task | Status |
| --- | --- |
| `libs/usd-asset-io`: `AssetReader`, `AssetMetadata`, `Status` | Done |
| Validator value types: `ValidatorKind`, `ValidatorStrength`, `Validator` | Done |
| Shared offset arithmetic (`ResolveReadRange`), so the EOF and overflow rules exist once | Done |
| Metrics counter definitions and per-reader storage | Done |
| Metrics process aggregate and `USD_HTTP_RESOLVER_METRICS_DUMP` | Done |
| `libs/usd-asset-local`: positional reads, size, derived validator | Done |
| Local `AssetChanged` on a republish underneath an open reader | Done |
| Counters populated by the local backend | Done |
| Boundary suite, parameterized over backends | Done — `tests/boundary`, one row per backend |
| Independent naive oracle, sharing no code with `usdAssetLocal` | Done |
| Property tests with biased generators over size, offset, length | Done — with shrinking and a reported seed |
| Concurrency cases: many threads on one reader, many readers on one asset | Done |
| Short-read-below-EOF case, via a provisioned misbehaving transport | Done |
| Local revision-change simulation (rewrite underneath an open reader) | Done |
| ASan, UBSan, and TSan **build configuration** for `libs/` | Done — `USD_HTTP_RESOLVER_SANITIZER`, `core-asan` and `core-tsan` presets |
| ASan, UBSan, and TSan **test cells** actually run | Outstanding — needs the CI matrix below; see "Blocking items" |
| Module READMEs for both libraries | Done |

## Phase 2 — HTTP backend, resolver bundle, revision binding (`v0.2.0`)

| Task | Status |
| --- | --- |
| HTTP client dependency decision, recorded as an ADR | Outstanding |
| `libs/usd-asset-http`: range, metadata, redirect, timeout, retry | Outstanding |
| Response framing validation (`Content-Range` covers the request) | Outstanding |
| Range-unsupported hard error per ADR-0002, with no fallback path | Outstanding |
| Validator capture at open, with kind and strength classified | Outstanding |
| `If-Range` on every range request after open | Outstanding |
| `AssetChanged` detection and reporting | Outstanding |
| Boundary test forbidding revision mixing within one reader | Outstanding |
| `plugins/http-resolver`: registration, normalization, anchoring | Outstanding |
| `ArAsset` adapter, with `GetBuffer()` null by contract | Outstanding |
| `HTTPxxx` projection and OpenUSD diagnostics | Outstanding |
| Hostile-server fixture corpus, including mid-read validator change | Outstanding |
| Boundary suite passing against the HTTP backend, unchanged | Outstanding |
| Cross-platform CI cells (Windows, Linux, macOS arm64) | Outstanding |

## Phase 3 — cache (`v0.3.0`)

| Task | Status |
| --- | --- |
| `libs/usd-asset-cache`: alignment, expansion, eviction | Outstanding |
| Validator-keyed `CacheKey` from the first commit | Outstanding |
| Coalescing with measured thresholds | Outstanding |
| Single-flight, tested under TSan | Outstanding |
| Cache counters, including `bytesOverFetched` | Outstanding |
| Block size and gap threshold measurement, recorded | Outstanding |

## Phase 4 — identity exposure and persistence (`v0.4.0`)

| Task | Status |
| --- | --- |
| Identity stability exposed through `GetAssetInfo` | Outstanding |
| Strong-validator-only rule for persistent reuse | Outstanding |
| On-disk persistence, or a recorded decision to defer | Outstanding |
| Cross-stage reuse rules for consumers | Outstanding |

## Phase 5 — first consumer (`v0.5.0`)

| Task | Status |
| --- | --- |
| Runtime composition with `usd-pointcloud-plugins` | Outstanding |
| Large remote COPC fixture and hosting | Outstanding |
| Amplification baseline recorded | Outstanding |
| Confirmation that the consumer needed no HTTP-aware change | Outstanding |

## Phase 6 — composition and extension points (`v0.6.0`)

| Task | Status |
| --- | --- |
| Configuration surface (env, then `ArResolverContext`) | Outstanding |
| Request interception point for authentication | Outstanding |
| OpenStrata formation composition and pinned artifacts | Outstanding |
| Packaged cross-platform release | Outstanding |

## Blocking items

1. **The HTTP client dependency is unchosen.** It constrains licensing, binary
   footprint, and the Wasm research track, so it is decided on those grounds
   before features. It blocks `v0.2.0`, not `v0.1.0` — nothing in phase 1
   touches a network.
2. **No CI matrix exists, so no sanitizer run has happened.** This is the one
   `v0.1.0` exit criterion not met. The build configuration is in place —
   `-DUSD_HTTP_RESOLVER_SANITIZER=address,undefined` and `=thread`, wired to the
   `core-asan` and `core-tsan` presets — and it is a clang or GCC lane: MSVC
   implements only `address`, and on MSVC 19.34 the instrumented binaries die at
   startup with `STATUS_DLL_INIT_FAILED` before `main`, with the runtime both on
   `PATH` and beside the executable. `openstrata.ci.yaml` must therefore include
   a Linux cell that runs the sanitizer presets, alongside the core cell that
   runs `-DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF` with no OpenUSD present. Both are
   contract rather than optional lanes.
3. **`ost library build` cannot resolve a library-to-library edge on its own.**
   `libs/usd-asset-local` declares `requires.libraries: [usdAssetIo]` and builds
   through plain CMake, `ost library build libs/usd-asset-io`, and the root
   tree; `ost library build libs/usd-asset-local` fails to find the
   `usdAssetIo` package because nothing has installed it into a shared prefix
   first. The same is reproducible in `usd-vrm-plugins`, so it is an `ost`
   workflow question rather than a defect in these descriptors. It blocks
   nothing in `v0.1.0` — the path the release is defined by is plain CMake —
   and it is worth resolving before the bundle in `v0.2.0` consumes the closure.

No longer blocking: ADR-0002, resolved as a hard error for `v0.2.0`.

## Next

1. Write `openstrata.ci.yaml` with the core cell, the sanitizer cells, and the
   Windows, Linux, and macOS arm64 rows, and generate the workflows from it.
   Until it exists, `v0.1.0`'s sanitizer criterion is unmet, and everything else
   in phase 1 is done.
2. Then phase 2, which starts with the HTTP client decision and its ADR rather
   than with code. The boundary suite the backend will be written against is
   already passing, which is the whole point of having built it first.
