# Implementation status

Task-level tracking of what is done, in progress, and outstanding. Behavior
belongs in [capability matrix](../reference/CAPABILITY_MATRIX.md); this file
tracks work.

Last updated: 2026-08-20.

Phases 0 and 1 are complete and `v0.1.0` is released. The read contract, the
local backend, and the shared boundary suite are in the tree and passing; the
core lane builds and tests on Windows, Linux, and macOS arm64 with no OpenUSD
present; and the sanitizer lanes run green. The release gate and what it found
are in [the release record](../releases/v0.1.0.md).

Phase 2 is complete, and the order it was built in is why. Both prerequisites
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

`openstrata.ci.yaml` has now landed on top of that, which it could not before:
every cell shape names a bundle or a workspace containing one, and until
`plugins/http-resolver` existed there was neither. Six cells run on pull
requests; the Windows lane is hand-authored, because libcurl there comes from
vcpkg and no generated cell can hand CMake a prefix.

The release's I/O baseline is recorded. `tests/baseline` runs the five scenarios
METRICS.md §6 requires against a 128 MiB synthetic asset on loopback, asserts the
byte and request counts exactly, and reports the ratios; the record is
[BASELINE.md](../reference/BASELINE.md). The headline is `selectivity` on a
bounded query: 0.0025 of a 128 MiB asset moved to answer it, with `amplification`
at exactly 1.0 because there is no cache to over-fetch.

Phase 3 is implemented and unreleased. `libs/usd-asset-cache` is in the tree,
entered into the shared boundary suite as its own row, wired into the resolver,
and measured: the constants come from a sweep rather than from a decimal point,
and the release's before-and-after is one run of one harness. What it has not
had is a release gate walked over it.

**`v0.2.0` is released.** The gate is walked and
[its record](../releases/v0.2.0.md) is written. Gates 4 and 6 bound for the first
time and both pass; gate 9 turned out not to bind, because it binds a release
that publishes a binary package and this is a source tag — it was measured
anyway, and what that found is in §5 of the record. Walking the gate found one
defect, in the suite rather than in the product: two concurrent runs of the same
boundary row shared one temporary workspace and deleted each other's fixtures,
which reads as a backend failure and is not one. Phase 2 ships with one row of
the table below declined rather than left undone: a metadata fallback for a
server that refuses `HEAD`, which the corpus has no case for and which would
therefore ship unexercised.

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
| `openstrata.ci.yaml` and its generated cells, once a bundle exists to name | Done — six `pull_request` cells and `.github/workflows/ost-source-ci.yml`; the Windows lane is hand-authored because no generated cell can reach vcpkg. [Report 03](../reports/ost/03-2026-08-18-a-support-matrix-with-one-hand-authored-lane.md) |
| `plugins/http-resolver`: registration, normalization, anchoring | Done — one type, two schemes; RFC 3986 reference resolution; normalization asserted including idempotence |
| `ArAsset` adapter, with `GetBuffer()` null by contract | Done — `Read` and `GetSize` are the whole path, and a failed read returns no bytes |
| `HTTPxxx` projection and OpenUSD diagnostics | Done — the table asserted rather than restated, and every rendering elides credentials |
| Environment configuration for the five transport bounds | Done — CONFIGURATION.md §2; a bad value warns and takes the default, and does not discard the other four |
| Bundle through `ost`: inspect, doctor, build, and the verification pyramid | Done, with one recorded failure — L0/L1/L3/L4/L5 pass; L2 asserts that `Resolve` returned a path, which a network resolver cannot satisfy without an origin. [Report 02](../reports/ost/02-2026-08-18-resolver-bundle-under-the-pyramid.md) |
| Remote stage opened end to end, over a socket | Done — `httpResolver_test_stage`, against the fixture corpus: a relative reference followed to a second remote layer, and a 4 KiB window out of 1 MiB with the `Range` header asserted from the server's log |
| `v0.2.0` release gate walked, record written, tag created | Done — [record](../releases/v0.2.0.md); gates 4 and 6 bind for the first time and pass, gate 9 does not bind and was measured anyway |
| Metadata request where `HEAD` is unavailable | Declined for this release, and deliberately not guessed — reported as `Unsupported`. The corpus has no row that refuses `HEAD`, so a fallback would ship unexercised |
| Recorded I/O baseline for the release | Done — `tests/baseline`, all five scenarios of METRICS.md §6 against a 128 MiB loopback fixture, registered as `usdAssetHttp_io_baseline` so a byte count that moves fails a lane. Record: [BASELINE.md](../reference/BASELINE.md) |
| A fixture large enough for `selectivity` to mean something | Done — 128 MiB, synthesized rather than committed, every byte a hash of its own offset so no scenario can count bytes it did not verify |
| A plain-download comparator that is not the client under test | Done — the fixture server's own raw client, one `GET`, no `Range`, no HTTP code shared with `usdAssetHttp` |
| Cross-platform CI cells (Windows, Linux, macOS arm64) | Done — `core-ci.yml` for the runtime-free lane, and the plugin lane on all three: generated cells on Linux and macOS arm64, `plugin-windows-ci.yml` on Windows. Each asserts `httpResolver_stage` by name rather than trusting a green suite |

## Phase 3 — cache (`v0.3.0`)

| Task | Status |
| --- | --- |
| `libs/usd-asset-cache`: alignment, expansion, eviction | Done — a decorator over `AssetReader` that links `usdAssetIo` and nothing else. Reads expand to whole blocks, the final block is stored at its true length, and eviction is LRU under a process-wide budget shared across assets |
| Validator-keyed `CacheKey` from the first commit | Done — identifier, validator, block size, block index. Two revisions at one URL are two identities, and the suite proves it rather than the code asserting it |
| Coalescing with measured thresholds | Done — a one-block gap and an 8 MiB ceiling, both from `tests/cache-tuning`. [BLOCK_POLICY.md](../reference/BLOCK_POLICY.md) also records that the gap does not bind at the shipped block size, which is the part a tuning document is most tempted to leave out |
| Single-flight, tested under TSan | Done — per block, across readers as well as threads: eight threads missing one block issue one request, and eight readers of one revision issue one between them, with a latch that makes the moment single-flight has to work happen rather than hoping for it. Run under `core-tsan` and under `core-asan`, 25 of 25 tests green in each, GCC 15.2 |
| Cache counters, including `bytesOverFetched` | Done — every counter in METRICS.md §2.2, and one counter set per decorated stack rather than two |
| Block size and gap threshold measurement, recorded | Done — `tests/cache-tuning`, sixty-six runs over a real socket, recorded in [BLOCK_POLICY.md](../reference/BLOCK_POLICY.md) |
| The boundary suite passing against `cache over local`, unchanged | Done — a third row, 244 fixed cases, the property cases, and the concurrency cases. Not one line of the suite relaxed |
| A read large enough to be a streaming pass bypasses the cache | Done — and it is what keeps the full sequential read byte for byte and request for request what it was, which the baseline asserts rather than observes |
| The resolver takes the cache | Done — every `ArAsset` the bundle hands out is decorated and bound into the process store, and `httpResolver_stage` asserts from the server's log that a 4 KiB window out of a megabyte costs a block and not the megabyte |
| The four cache variables in CONFIGURATION.md | Done — read once at resolver construction; a bad value warns and takes the default, an adjusted one warns and takes the adjustment |
| Recorded before-and-after baseline | Done — [BASELINE.md](../reference/BASELINE.md), every scenario measured twice in one run of one harness |
| On-disk persistence | Deferred to `v0.4.0` by the roadmap, deliberately: the validator exists, and what does not yet exist is a release's worth of evidence that capture is correct |

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
   anything — those lanes are hand-authored in `.github/workflows/core-ci.yml`
   and are green — but it is why the two runtime-free lanes stay outside
   `openstrata.ci.yaml` now that the matrix exists. Every `SupportCell` requires
   a `runtime_artifact` and materializes it before building, which would remove
   the very property the core lane demonstrates; and a `kind: workspace` cell's
   build step takes no preset, `--intent`, or cache variable, so the sanitizer
   presets are unreachable. One third of this resolved itself: `verify: graph`,
   the one runtime-free rung, no longer fails with `PRECONDITION_FAILED: no
   plugin bundles found in the workspace member set`, because
   `plugins/http-resolver` is the bundle that precondition was waiting for, and
   it is now a cell. Full account and the two upstream asks:
   [report 01](../reports/ost/01-2026-08-16-v0.1.0-ci-without-a-support-matrix.md);
   what the matrix does with what remains:
   [report 03](../reports/ost/03-2026-08-18-a-support-matrix-with-one-hand-authored-lane.md).
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
   L2 cannot be satisfied by a network resolver.** Neither blocks the release,
   and the first now costs more than it did. Setting `CMAKE_PREFIX_PATH` in the
   environment before invoking `ost` works on a developer's machine and is
   invisible in the descriptor; in CI there is no step to set it in, because a
   generated cell renders a host-package installer for `apt` and `brew` only and
   `ost build` takes no prefix, no toolchain file, and no `-D`. So the Windows
   plugin lane is hand-authored in `.github/workflows/plugin-windows-ci.yml`,
   reading its pins back out of the matrix rather than copying them. The second
   is a disagreement about what the rung asserts: L2 runs
   `Ar.GetResolver().Resolve("<scheme>:<fixture>")` and requires a non-empty
   path, which for this resolver requires an origin to be listening, and the
   alternatives — a local-file branch in the resolver, or a fixture that is a
   URL — are both worse than a recorded failure; the two bundle cells are capped
   at `up_to: 1` because of it. Full account, with the probe and the three asks:
   [report 02](../reports/ost/02-2026-08-18-resolver-bundle-under-the-pyramid.md);
   what each costs in the matrix, and a fourth ask:
   [report 03](../reports/ost/03-2026-08-18-a-support-matrix-with-one-hand-authored-lane.md).

4. **A generated cell cannot be given a step timeout.** `openstrata.ci.yaml` has
   no field for one and `ost ci generate` emits none, so every generated cell —
   including the `host_packages` installer that runs the same `apt-get` that
   held this repository's lanes for six hours on 2026-08-18 — inherits GitHub's
   six-hour job default. The hand-authored lanes now bound their apt steps in
   minutes; the generated ones cannot, and the ask is a per-step or per-cell
   timeout in the matrix. It blocks nothing: a stalled cell fails eventually,
   expensively.

No longer blocking: ADR-0002, resolved as a hard error for `v0.2.0`. Also no
longer blocking: the sanitizer runs, which now happen; and the HTTP client
dependency, resolved as libcurl in
[ADR-0003](../adr/0003-http-client-dependency.md), which unblocks phase 2.

## Next

1. The `v0.3.0` release gate, and its record. Everything the release is defined
   by is in the tree and green; what has not happened is walking
   [the gate](../releases/README.md) over it and writing down what that found.
   Gate 6 is the interesting one this time, because it is the first release in
   which the byte counts moved on purpose and the gate has to be satisfied by a
   *recorded* change rather than by an unchanged table.
2. Phase 4, whose scope is what a persistent entry has to prove before it is
   written: identity exposed through `GetAssetInfo`, and the strong-validator
   rule for reuse across opens. The rows it is measured against are
   [BASELINE.md](../reference/BASELINE.md) § *What the next release has to
   move*.

Done, and no longer next: the sanitizer lanes over the cache. Both are green
over the whole core tree — 25 of 25 under `address,undefined` and 25 of 25 under
`thread`, GCC 15.2 — and the ones that matter for this release are the three the
cache added: `usdAssetCache_singleflight`, the `cached-local` boundary row, and
the block-policy sweep, which drives the cache over a real socket at five block
sizes. TSan is not optional for this module, because single-flight is where a
naive implementation deadlocks and asserting a concurrency property in prose
asserts nothing.

Done, and no longer next: Phase 3 itself. What building it surfaced was a
disagreement between two documents that had never been made to disagree before,
and it was worth resolving in the contract rather than in the code. The boundary
suite's mid-read revision case asserted `AssetChanged` on a re-read of a range
the reader had already read; a block cache answers that read from bytes it
captured under the same binding, observes nothing, and returns the revision the
reader is bound to. §2.1 of ASSET_READER.md already said *observes*, and
CACHE.md §6 already admitted in-memory caching for the reader's lifetime, so the
suite was asserting something narrower than the contract it is the executable
form of. The case now asserts `AssetChanged` at an offset the reader has not
read, and for a repeated range asserts either `AssetChanged` or byte-for-byte
the bound revision — never the new one. That is a strengthening: the byte
comparison is new, and it would have caught a backend that rebound *and*
reported `AssetChanged`, which the old case would have passed.

Two smaller things the work surfaced, both recorded where they matter. The
first is that a decorated stack cannot have two counter sets: the two ends
disagree about what `bytesRequested` means, and summing them makes
`amplification` a ratio over two different measurements added together. The
second is that the coalescing gap does not bind at the block size the
measurement chose, which is written into
[BLOCK_POLICY.md](../reference/BLOCK_POLICY.md) rather than left for a reader to
infer from a table of identical rows.

Done, and no longer next: the `v0.2.0` release gate and its record. Two of the
three gates that had been not-applicable in `v0.1.0` bound and passed; the third,
gate 9, turned out not to bind at all, because it binds a release that publishes
a binary package and this one is a source tag. That was worth measuring rather
than arguing: two independent builds of the same tree agree on 24 of 28 installed
files, and the four that differ — three static libraries and the bundle's shared
library — differ only in embedded build timestamps, two bytes of it in the DLL.
Closing that is a link flag, and it belongs to `v0.6.0` with the packaging rather
than to a release commit after the lanes are green.

What walking the gate surfaced was a defect in the harness rather than the
product, and of a shape worth naming: the boundary suite's temporary workspace
was unique per row but not per process, so running the ASan and the TSan build of
one row at once meant each deleted the other's fixtures. It presented as
`oracle NotFound` against a backend that had returned correct bytes. CI never saw
it — each sanitizer lane is its own runner — and it took walking both lanes on
one machine to produce it.

Done, and no longer next: the recorded I/O baseline. What it surfaced was not a
defect but a division worth writing down — byte counts are asserted and ratios
are reported, because a ratio is a byte count divided by a fixture size and a
gate on one would move when the fixture did. It also cost a fourth reverse edge
onto the fixture server, and the argument for it is the same one the fourth row
of METRICS.md §6 makes: "must not be worse than a plain download" needs a plain
download performed by a client that is not the one under test.

Also done, and no longer next: `plugins/http-resolver`, and before it
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

Also done, and no longer next: `openstrata.ci.yaml`. What it surfaced was a
third thing the same shape as the other two — that the arguments a build needs
can be real and still be inexpressible. `ost build` composes a runtime, a
toolchain, and a library closure correctly, and has nowhere to put
`CMAKE_PREFIX_PATH`; on Linux and macOS `host_packages` hides that, and on
Windows it is the difference between a generated cell and a hand-authored lane.
See [report 03](../reports/ost/03-2026-08-18-a-support-matrix-with-one-hand-authored-lane.md).
