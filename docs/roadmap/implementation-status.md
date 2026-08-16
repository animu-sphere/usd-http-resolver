# Implementation status

Task-level tracking of what is done, in progress, and outstanding. Behavior
belongs in [capability matrix](../reference/CAPABILITY_MATRIX.md); this file
tracks work.

Last updated: 2026-08-16.

Phases 0 and 1 are complete. The read contract, the local backend, and the
shared boundary suite are in the tree and passing; the core lane builds and
tests on Windows, Linux, and macOS arm64 with no OpenUSD present; and the
sanitizer lanes run green. `v0.1.0` has no unmet exit criterion.

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
| CI: the runtime-free lanes, on three platforms | Done — `.github/workflows/core-ci.yml`, hand-authored |
| `openstrata.ci.yaml` and generated workflows | Moved to phase 2 — no `ost` cell can name a workspace without a bundle, or decline to pin a runtime |
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
| ASan, UBSan, and TSan **test cells** actually run | Done — the `sanitizers` job in `core-ci.yml`, and locally under GCC 15.2 |
| A UBSan report fails the run rather than printing | Done — `-fno-sanitize-recover=all`; it did not, before |
| Core build and test on a machine with no OpenUSD, in CI | Done — the `core` job, three platforms, asserted from the configure log |
| Module READMEs for both libraries | Done |

## Phase 2 — HTTP backend, resolver bundle, revision binding (`v0.2.0`)

| Task | Status |
| --- | --- |
| HTTP client dependency decision, recorded as an ADR | Outstanding |
| `openstrata.ci.yaml` and its generated cells, once a bundle exists to name | Outstanding |
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
2. **`ost ci` cannot express a lane that pins no runtime.** Not blocking
   anything — the lanes landed hand-authored in `.github/workflows/core-ci.yml`
   and are green — but it is why `openstrata.ci.yaml` does not exist yet and why
   two cells will stay outside it after it does. Every `SupportCell` requires a
   `runtime_artifact` and materializes it before building, which would remove
   the very property the core lane demonstrates; a `kind: workspace` cell's
   build step takes no preset, `--intent`, or cache variable, so the sanitizer
   presets are unreachable; and `verify: graph`, the one runtime-free rung,
   fails with `PRECONDITION_FAILED: no plugin bundles found in the workspace
   member set`. Full account and the two upstream asks:
   [report 01](../reports/ost/01-2026-08-16-v0.1.0-ci-without-a-support-matrix.md).
3. **`ost library build` cannot resolve a library-to-library edge on its own.**
   `libs/usd-asset-local` declares `requires.libraries: [usdAssetIo]` and builds
   through plain CMake, `ost library build libs/usd-asset-io`, and the root
   tree; `ost library build libs/usd-asset-local` fails to find the
   `usdAssetIo` package because nothing has installed it into a shared prefix
   first. The same is reproducible in `usd-vrm-plugins`, so it is an `ost`
   workflow question rather than a defect in these descriptors. It blocks
   nothing in `v0.1.0` — the path the release is defined by is plain CMake —
   and it is worth resolving before the bundle in `v0.2.0` consumes the closure.

No longer blocking: ADR-0002, resolved as a hard error for `v0.2.0`. Also no
longer blocking: the sanitizer runs, which now happen.

## Next

1. Tag `v0.1.0`: finalize the changelog, write the release record, and confirm
   the gate in [docs/releases/README.md](../releases/README.md). Every technical
   criterion is met; what remains is the first green CI run on the pull request
   that carries these lanes, which is gate 2's evidence.
2. Then phase 2, which starts with the HTTP client decision and its ADR rather
   than with code. The boundary suite the backend will be written against is
   already passing, which is the whole point of having built it first.
3. `openstrata.ci.yaml` lands within phase 2, once `plugins/http-resolver`
   exists to name, and never absorbs the two runtime-free lanes.
