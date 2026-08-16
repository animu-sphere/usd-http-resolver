# Implementation status

Task-level tracking of what is done, in progress, and outstanding. Behavior
belongs in [capability matrix](../reference/CAPABILITY_MATRIX.md); this file
tracks work.

Last updated: 2026-08-16.

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
| `LICENSE`, `NOTICE`, `VERSION` | Outstanding |
| Module README contract applied to real modules | Outstanding (no modules yet) |

## Phase 1 — read contract, local backend, boundary suite (`v0.1.0`)

| Task | Status |
| --- | --- |
| `libs/usd-asset-io`: `AssetReader`, `AssetMetadata`, `Status` | Outstanding |
| Validator value types: `ValidatorKind`, `ValidatorStrength`, `Validator` | Outstanding |
| Metrics counter definitions and per-reader storage | Outstanding |
| `libs/usd-asset-local`: positional reads, size, derived validator | Outstanding |
| Boundary suite, parameterized over backends | Outstanding |
| Property tests with biased generators over size, offset, length | Outstanding |
| ASan, UBSan, and TSan build and test cells for `libs/` | Outstanding |
| Local revision-change simulation (rewrite underneath an open reader) | Outstanding |
| Module READMEs for both libraries | Outstanding |

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
2. **No CI matrix exists.** `openstrata.ci.yaml` is written before the first
   bundle so that `v0.1.0` lands with cross-platform evidence rather than
   acquiring it later. It must include a core cell that runs
   `-DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF` with no OpenUSD present, and the
   sanitizer cells, since both are contract rather than optional lanes.

No longer blocking: ADR-0002, resolved as a hard error for `v0.2.0`.

## Next

Phase 1, in this order: the read contract and validator value types, the local
backend, then the boundary suite and its property and sanitizer cells. Nothing
in it requires a network, an HTTP client decision, or OpenUSD — which is why it
is the work that can start immediately.
