# Implementation status

Task-level tracking of what is done, in progress, and outstanding. Behavior
belongs in [capability matrix](../reference/CAPABILITY_MATRIX.md); this file
tracks work.

Last updated: 2026-08-18.

Phases 0 and 1 are complete and `v0.1.0` is released. The read contract, the
local backend, and the shared boundary suite are in the tree and passing; the
core lane builds and tests on Windows, Linux, and macOS arm64 with no OpenUSD
present; and the sanitizer lanes run green. The release gate and what it found
are in [the release record](../releases/v0.1.0.md).

Phase 2 is under way and its centre of gravity has landed. Both prerequisites
were done first, in the order §17 of the
[design policy](../design/DESIGN_POLICY.md) fixed — the client chosen and
recorded, then the hostile corpus standing up — and `libs/usd-asset-http` was
then written against two things that already passed. It passes the boundary
suite unchanged, every corpus behavior is projected onto a typed code, and both
sanitizer lanes are green over the HTTP path.

The bundle has now landed on top of it. `plugins/http-resolver` registers
`http` and `https`, normalizes and anchors identifiers, hands out an `ArAsset`
over the backend's reader, emits the `HTTPxxx` codes, and reads the five
transport bounds from the environment. A `UsdStage` opens over HTTP against the
hostile fixture corpus, over a real socket, in `httpResolver_test_stage`.

What remains in phase 2 is `openstrata.ci.yaml` — which now, for the first time,
has a bundle to name — and the release's recorded I/O baseline.

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
| Release gate walked, record written, `v0.1.0` tagged | Done — [record](../releases/v0.1.0.md); gates 4, 6, and 9 not applicable before a transport exists, per [the gate](../releases/README.md) |

## Phase 2 — HTTP backend, resolver bundle, revision binding (`v0.2.0`)

| Task | Status |
| --- | --- |
| HTTP client dependency decision, recorded as an ADR | Accepted — libcurl, private `find_package`, [ADR-0003](../adr/0003-http-client-dependency.md) |
| Hostile-server fixture corpus, standing up before the backend | Done — `tests/fixture-server`, 18 behaviors, self-checked over a raw socket |
| Corpus covers all nine conditions in §11.2 of the design policy | Done — plus three from ADR-0003, §4.1, and DIAGNOSTICS.md §4.4; coverage asserted at runtime, not claimed |
| `libs/usd-asset-http`: range, metadata, redirect, timeout, retry | Done — behind the internal transport seam ADR-0003 requires; libcurl in one translation unit |
| Response framing validation (`Content-Range` covers the request) | Done — against the request, not against the response itself |
| Range-unsupported hard error per ADR-0002, with no fallback path | Done — at open from `Accept-Ranges`, and at the first read for a server that advertised and then ignored |
| Validator capture at open, with kind and strength classified | Done — strong `ETag`, weak `ETag`, `Last-Modified`, or none |
| `If-Range` on every range request after open | Done — where the captured validator admits one; asserted from the fixture server's request log |
| `AssetChanged` detection and reporting | Done — two independent detectors: a `200` answering a conditional range, and a response contradicting the captured validator or length |
| Boundary test forbidding revision mixing within one reader | Done — the suite's own republish case, now with an HTTP row |
| Backend run against the hostile corpus, every behavior projected onto the typed vocabulary | Done — `tests/corpus`; coverage asserted against `AllBehaviors()` at runtime |
| Boundary suite passing against the HTTP backend, unchanged | Done — 243 fixed cases, 10,000 property cases, and the concurrency cases; not one line of the suite changed |
| Sanitizer lanes over the HTTP path | Done — ASan, UBSan, and TSan green, including the boundary row and the corpus projection |
| `openstrata.ci.yaml` and its generated cells, once a bundle exists to name | Outstanding — the bundle now exists to name |
| `plugins/http-resolver`: registration, normalization, anchoring | Done — one type, two schemes; RFC 3986 reference resolution; normalization asserted including idempotence |
| `ArAsset` adapter, with `GetBuffer()` null by contract | Done — `Read` and `GetSize` are the whole path, and a failed read returns no bytes |
| `HTTPxxx` projection and OpenUSD diagnostics | Done — the table asserted rather than restated, and every rendering elides credentials |
| Environment configuration for the five transport bounds | Done — CONFIGURATION.md §2; a bad value warns and takes the default, and does not discard the other four |
| Bundle through `ost`: inspect, doctor, build, and the verification pyramid | Done, with one recorded failure — L0/L1/L3/L4/L5 pass; L2 asserts that `Resolve` returned a path, which a network resolver cannot satisfy without an origin. [Report 02](../reports/ost/02-2026-08-18-resolver-bundle-under-the-pyramid.md) |
| Remote stage opened end to end, over a socket | Done — `httpResolver_test_stage`, against the fixture corpus: a relative reference followed to a second remote layer, and a 4 KiB window out of 1 MiB with the `Range` header asserted from the server's log |
| Metadata request where `HEAD` is unavailable | Outstanding, and deliberately not guessed — reported as `Unsupported`. The corpus has no row that refuses `HEAD`, so a fallback would ship unexercised |
| Recorded I/O baseline for the release | Outstanding — `v0.2.0` is the first release that can record one |
| Cross-platform CI cells (Windows, Linux, macOS arm64) | Done for the core lane — `core-ci.yml` installs libcurl per platform and still asserts from the configure log that OpenUSD was never reached |

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

1. **`ost ci` cannot express a lane that pins no runtime.** Not blocking
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
2. **`ost library build` cannot resolve a library-to-library edge on its own.**
   `libs/usd-asset-local` declares `requires.libraries: [usdAssetIo]` and builds
   through plain CMake, `ost library build libs/usd-asset-io`, and the root
   tree; `ost library build libs/usd-asset-local` fails to find the
   `usdAssetIo` package because nothing has installed it into a shared prefix
   first. The same is reproducible in `usd-vrm-plugins`, so it is an `ost`
   workflow question rather than a defect in these descriptors. It blocks
   nothing in `v0.1.0` — the path the release is defined by is plain CMake —
   and it is worth resolving before the bundle in `v0.2.0` consumes the closure.

3. **`ost … build` cannot reach a third-party dependency, and `ost plugin test`
   L2 cannot be satisfied by a network resolver.** Neither blocks the release.
   The first is worked around by setting `CMAKE_PREFIX_PATH` in the environment
   before invoking `ost`, which is invisible in the descriptor and is therefore
   an upstream ask. The second is a disagreement about what the rung asserts:
   L2 runs `Ar.GetResolver().Resolve("<scheme>:<fixture>")` and requires a
   non-empty path, which for this resolver requires an origin to be listening,
   and the alternatives — a local-file branch in the resolver, or a fixture that
   is a URL — are both worse than a recorded failure. Full account, with the
   probe and the three asks:
   [report 02](../reports/ost/02-2026-08-18-resolver-bundle-under-the-pyramid.md).

No longer blocking: ADR-0002, resolved as a hard error for `v0.2.0`. Also no
longer blocking: the sanitizer runs, which now happen; and the HTTP client
dependency, resolved as libcurl in
[ADR-0003](../adr/0003-http-client-dependency.md), which unblocks phase 2.

## Next

1. `openstrata.ci.yaml` and its generated cells. The bundle is the first thing
   in this repository a cell can name, which is what was missing; the two
   runtime-free lanes stay hand-authored and are never absorbed into it, for the
   reasons in
   [report 01](../reports/ost/01-2026-08-16-v0.1.0-ci-without-a-support-matrix.md).
   The cells that matter are the bundle build and `httpResolver_stage` on
   Windows, Linux, and macOS arm64 — the exit criterion that a remote layer
   opens on all three.
2. The recorded I/O baseline. `v0.2.0` is the first release that can produce
   one, and §6 of [METRICS.md](../architecture/METRICS.md) names the five
   scenarios it has to cover. The counters exist and are populated, and the
   resolver now drives them from a stage rather than from a test harness; what
   does not exist yet is a fixture large enough for `selectivity` to mean
   anything.
3. Gates 4, 6, and 9 of [the release gate](../releases/README.md) bind for the
   first time in this release, having been not-applicable in `v0.1.0`.

Done, and no longer next: `plugins/http-resolver`, and before it
`libs/usd-asset-http` and the corpus projection that was to be its first test
file. Both things the backend was written against were already passing when it
started — the boundary suite and the hostile corpus — which is the whole point
of having built them first, and it is why the arguments about failing range
reads during that work were short ones. Two defects found that way are recorded
in the [changelog](../../CHANGELOG.md).

The bundle inherited the same advantage and it showed: what its work surfaced
was not a range-read defect but two integration facts that no amount of contract
reading would have produced — that `ArResolver`'s default `GetExtension` cannot
name a signed URL's format, and that copying OpenUSD's DLLs beside a test
executable leaves `PlugRegistry` with nothing registered and kills the process
before `main` with no output at all.
