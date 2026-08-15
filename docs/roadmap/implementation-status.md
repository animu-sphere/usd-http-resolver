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
| ADR-0001: consumer interface | Accepted |
| ADR-0002: range-unsupported policy | Open, blocks `v0.2.0` |
| `openstrata.ci.yaml` and generated workflows | Outstanding |
| `LICENSE`, `NOTICE`, `VERSION` | Outstanding |
| Module README contract applied to real modules | Outstanding (no modules yet) |

## Phase 1 — read contract and local backend (`v0.1.0`)

| Task | Status |
| --- | --- |
| `libs/usd-asset-io`: `AssetReader`, `AssetMetadata`, `Status` | Outstanding |
| Metrics counter definitions and per-reader storage | Outstanding |
| `libs/usd-asset-local`: positional reads, size, validator | Outstanding |
| Boundary suite, parameterized over backends | Outstanding |
| Concurrency tests under ASan and TSan | Outstanding |
| Module READMEs for both libraries | Outstanding |

## Phase 2 — HTTP backend and resolver bundle (`v0.2.0`)

| Task | Status |
| --- | --- |
| HTTP client dependency decision, recorded as an ADR | Outstanding |
| `libs/usd-asset-http`: range, metadata, redirect, timeout, retry | Outstanding |
| Response framing validation (`Content-Range` covers the request) | Outstanding |
| Resolve ADR-0002 and implement the chosen policy | Outstanding |
| `plugins/http-resolver`: registration, normalization, anchoring | Outstanding |
| `ArAsset` adapter | Outstanding |
| `HTTPxxx` projection and OpenUSD diagnostics | Outstanding |
| Hostile-server fixture corpus | Outstanding |
| Boundary suite passing against the HTTP backend, unchanged | Outstanding |
| Cross-platform CI cells (Windows, Linux, macOS arm64) | Outstanding |

## Phase 3 — cache (`v0.3.0`)

| Task | Status |
| --- | --- |
| `libs/usd-asset-cache`: alignment, expansion, eviction | Outstanding |
| Coalescing with measured thresholds | Outstanding |
| Single-flight, tested under TSan | Outstanding |
| Cache counters, including `bytesOverFetched` | Outstanding |
| Block size and gap threshold measurement, recorded | Outstanding |

## Phase 4 — consistency (`v0.4.0`)

| Task | Status |
| --- | --- |
| Validator capture and classification | Outstanding |
| `If-Range` on every range request | Outstanding |
| `AssetChanged` detection and reporting | Outstanding |
| Identity stability exposed through `GetAssetInfo` | Outstanding |
| Mid-read mutation test | Outstanding |
| On-disk persistence, or a recorded decision to defer | Outstanding |

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

1. **ADR-0002 is open** and blocks `v0.2.0`. See
   [0002-range-unsupported-policy.md](../adr/0002-range-unsupported-policy.md).
2. **The HTTP client dependency is unchosen.** It constrains licensing, binary
   footprint, and the Wasm research track, so it is decided on those grounds
   before features.
3. **No CI matrix exists.** `openstrata.ci.yaml` is written before the first
   bundle so that `v0.1.0` lands with cross-platform evidence rather than
   acquiring it later.
