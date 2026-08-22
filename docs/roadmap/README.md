# usd-http-resolver Roadmap

This directory breaks the [design policy](../design/DESIGN_POLICY.md) into
actionable milestones. The policy states the standing direction; this directory
states the order of work. What is implemented today is in
[implementation status](implementation-status.md) and, at the level of
behavior, in [capability matrix](../reference/CAPABILITY_MATRIX.md).

## Principles

- Establish the read contract before any transport. A backend is written
  against an existing, passing test suite.
- Make the local backend the correctness oracle. Remote correctness is defined
  as equivalence to local, not as "it opened".
- Ship synchronous, correct reads before asynchronous, fast reads.
- Measure before tuning. Block size, coalescing window, and concurrency are
  measured constants, not guessed ones.
- Keep the consumer interface at `ArAsset`. No consumer links this repository.
- Prove the abstraction with a second consumer before generalizing it further.
- Add a transport only when it fits the existing contract unchanged.

## Immediate direction

The first two releases build one vertical slice: a read contract with the
shared boundary suite that enforces it, a local backend that proves the suite
is satisfiable, and an HTTP backend that is byte-equivalent to the local one
over a hostile test server — and that is bound to a single asset revision for
each reader's lifetime. Only then does caching enter, because a cache in front
of an unproven reader hides its bugs.

Revision binding sits in `v0.2.0` and not later because it is a correctness
property of range reads themselves, not a cache feature: a reader that issues
three requests without a validator can compose three revisions into one byte
sequence with no request failing. The argument is in §2.1 of the
[asset reader contract](../architecture/ASSET_READER.md). Validators therefore
ship with the first backend that can violate the guarantee, and `v0.4.0` keeps
only what genuinely depends on them being trustworthy first: exposure to
consumers, and persistence.

`v0.5.0` is the first release with an external claim: `usd-pointcloud-plugins`
opens a remote COPC asset through this resolver, and the recorded byte ratio
demonstrates the architecture. Everything before it is infrastructure that must
be right; everything after it is reach.

| Release | Theme | Outcome |
| --- | --- | --- |
| `v0.1.0` | Read contract, local backend, shared boundary suite | One random-access contract, typed diagnostics, metrics counters, and the suite every later backend is admitted by |
| `v0.2.0` | HTTP range reads, the resolver bundle, and revision binding | `http`/`https` resolve and serve ranges; byte-equivalent to local against a hostile fixture server; one reader, one revision |
| `v0.3.0` | Block cache, coalescing, single-flight | Small scattered reads stop becoming small scattered requests |
| `v0.4.0` | Identity exposure and persistence | Stability metadata a consumer can act on, and a cache that may safely outlive a process |
| `v0.5.0` | First consumer integration | Remote COPC through `usd-pointcloud-plugins`, with a recorded amplification baseline |
| `v0.6.0` | Composition and extension points | OpenStrata formation composition, auth interception point, and configuration surface |
| Research | Async, prefetch, and Wasm | Investigated in parallel; no release gate |
| Later | Additional transports and consumers | S3, package-internal, and content-addressed backends; `usd-3dgs-plugins` |

The layering that every release preserves:

```text
         ArResolver bundle (plugins/http-resolver)   <- only OpenUSD consumer
                        |
                        v
              asset open / identity
                        |
                        v
                  block cache            <- v0.3.0
                        |
             +----------+----------+
             |                     |
             v                     v
      local backend          HTTP backend
      (v0.1.0, oracle)       (v0.2.0)
                                   |
                                   v
                        S3 / package / Wasm  (later)
```

### `v0.1.0` — read contract, local backend, and the shared boundary suite

The release that decides whether the rest of the project is buildable. It ships
no network code, and its centre of gravity is the test suite rather than the
reader.

Scope: the `AssetReader`, `AssetMetadata`, and validator value types fixed in
[ASSET_READER.md](../architecture/ASSET_READER.md); the typed diagnostic
vocabulary in [DIAGNOSTICS.md](../architecture/DIAGNOSTICS.md); the counter set
in [METRICS.md](../architecture/METRICS.md); a local file backend implementing
all of it, including a filesystem-derived validator; and the shared boundary
suite fixed in [BOUNDARY_SUITE.md](../contributing/BOUNDARY_SUITE.md) — the
fixed cases, the biased property-test generators, and the sanitizer builds —
that every later backend is admitted by.

The deliverable is not "a local file reader". It is a small, boring, reusable
core plus the harness that makes every subsequent transport cheap to verify.
This release adds implementation, not documentation: the contracts it
implements are already written.

Out of scope: HTTP, caching, `ArResolver` registration, async.

Exit criteria: the local backend passes the boundary suite under
AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer; the suite is
written so that swapping the backend under test is a one-line change; the whole
release builds and tests with `-DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF` on a
machine with no OpenUSD installed; counters are populated.

Met, and released on 2026-08-16: the suite exists and the local backend passes
it, entering a backend is a row rather than a suite, the OpenUSD-free path
builds and tests on all three platforms, the counters are populated, and the
sanitizer lanes run — ASan, UBSan, and TSan, green over the core path. The
release gate, including the three rows that could not apply before a transport
exists, is walked in [the record](../releases/v0.1.0.md).

The sanitizer cells are hand-authored in `.github/workflows/core-ci.yml` rather
than generated from `openstrata.ci.yaml`, which at `v0.1.0` did not exist and
could not: every `ost` cell pins and materializes an OpenUSD runtime, and a lane
whose contract is that it needs none must not. The matrix arrived in `v0.2.0`
with the first bundle, and those two lanes stayed where they are, for the same
reason. See
[report 01](../reports/ost/01-2026-08-16-v0.1.0-ci-without-a-support-matrix.md),
[report 03](../reports/ost/03-2026-08-18-a-support-matrix-with-one-hand-authored-lane.md),
and [implementation status](implementation-status.md).

### `v0.2.0` — HTTP range reads, the resolver bundle, and revision binding

The first release that touches a network, and the first that registers an
OpenUSD plugin.

Scope: an HTTP backend supporting `GET`, a metadata request, `Range`,
`Content-Length`, `Content-Range`, `Accept-Ranges`, bounded redirects,
timeouts, and bounded retry; validator capture at open with its kind and
strength classified, a conditional (`If-Range`) guard on every subsequent range
request, mid-read change detection reported as `AssetChanged`, and the boundary
test that forbids one reader from ever mixing revisions; the
`plugins/http-resolver` bundle registering the `http` and `https` URI schemes
and returning an `ArAsset` backed by that backend; URI normalization and
relative resolution per [RESOLVER.md](../architecture/RESOLVER.md); and the
fixture server that reproduces the hostile cases in §11.2 of the design policy.

Range-unsupported is a hard error here — `RangeNotSupported`, no whole-asset
fallback — per [ADR-0002](../adr/0002-range-unsupported-policy.md). Bounded
fallback is a separate, later feature with its own residency model, and
building it now would widen this release from "range reads are correct" to
"range reads are correct, plus a second storage path with a threshold, a
warning policy, a spill story, and its own metrics".

The HTTP client dependency is chosen in this release, on license, footprint,
and Wasm viability, and recorded as an ADR. It is decided:
[ADR-0003](../adr/0003-http-client-dependency.md) selects libcurl, acquired
through a private `find_package` and reached only through a narrow internal
transport seam. libcurl does not build for Wasm; the reserved `usdAssetWasm`
backend is the answer to that, and the ADR records it as a cost rather than a
technicality.

Out of scope: caching of any kind — every read is a request, deliberately, so
that the request pattern is visible before it is optimized. Also out of scope:
authentication; persistent identity; and exposure of stability to consumers,
which waits for `v0.4.0` because a consumer acting on it needs it to have been
right for a release first.

Exit criteria: the `v0.1.0` boundary suite passes against the HTTP backend
unchanged; the hostile-server corpus passes; a fixture mutated mid-read
produces `AssetChanged` and never mixed bytes; `usdcat` on a remote `.usda`
succeeds on Windows, Linux, and macOS in CI; no credential-shaped string
appears in any diagnostic.

### `v0.3.0` — block cache, coalescing, and single-flight

The release that makes the architecture perform rather than merely work.

Status: implemented and unreleased. All four exit criteria below are met, the
sanitizer lanes included. What has not happened is the release gate; see
[implementation status](implementation-status.md).

Scope: an aligned block cache; read expansion to block boundaries; coalescing
of adjacent and near-adjacent block fetches into one request; single-flight so
that N threads missing the same block issue one request; eviction with a
bounded memory budget; and the cache statistics in
[METRICS.md](../architecture/METRICS.md).

Block size, coalescing gap threshold, and budget are chosen from measurement
against a fixture with a realistic access pattern — a header read, an index
read, then scattered chunk reads — and the measurement is recorded.

The cache is keyed on the validator captured in `v0.2.0` from the start. There
is no interim URL-keyed cache to migrate away from, which is the practical
benefit of having moved validators forward.

Out of scope: on-disk cache persistence. The validator it would key on exists
by now; what does not yet exist is a release's worth of evidence that capture
is correct, which is what `v0.4.0` waits for before letting an entry outlive
the process that wrote it.

Exit criteria: byte-for-byte equivalence with the uncached path over the full
suite; a cached entry from revision A never serves a read of revision B at the
same URL; a recorded before/after request count and amplification ratio for the
representative access pattern; single-flight proven under ThreadSanitizer.

### `v0.4.0` — identity exposure and persistence

The release that lets identity leave the process — first to a consumer, then to
a disk. Both wait until validator capture has been correct for a release,
because both turn a wrong validator into a durable wrong answer.

Scope: identity classification as `Stable`, `Unstable`, or `Unavailable`
exposed to consumers through the resolver's asset-info surface, so a consumer
can decide whether its own generated-cache reuse is safe; the rule that
persistent reuse is admitted only for a strong validator, per §7.2 of the
[asset reader contract](../architecture/ASSET_READER.md); optional on-disk
cache persistence keyed on the same `CacheKey`, never on the URL alone; and the
cross-stage reuse rules that follow.

Out of scope: content-addressed identity and cross-stage cache sharing beyond
those rules.

Exit criteria: the identity classification matches what the first consumer's
contract expects; a weak or absent validator never produces a persistent entry;
a persisted entry from revision A never serves a read of revision B; deleting
the cache directory costs time and never correctness.

Status: both halves have landed. `GetAssetInfo` publishes the four neutral
values and `ArAssetInfo::version` carries a token only for a `Stable` identity —
a rule that came from reading the consumer's side, where a non-empty `version`
is by itself sufficient for generated-cache reuse and no stability field is
consulted. That asymmetry is what the first exit criterion turned out to mean.
`DiskBlockStore` is the second half: blocks fetched for a `Stable` identity are
written to a directory a host names, and a later process reads them back — one
request and no bytes for a bounded query a previous process paid nineteen
requests for, recorded in [BASELINE.md](../reference/BASELINE.md) as
`bounded query, reopened`. The same rule divides both halves, and it is the only
rule either of them has: `Strong` yes, `Weak` no, `None` no. See
[RESOLVER.md](../architecture/RESOLVER.md) §3,
[CACHE.md](../architecture/CACHE.md) §8, and
[implementation status](implementation-status.md).

### `v0.5.0` — first consumer integration

The release that tests whether the abstraction is real.

`usd-pointcloud-plugins` opens a remote COPC asset with no HTTP code of its
own, no build dependency on this repository, and no change to `usdCopc`. The
composition is runtime-only, through `PXR_PLUGINPATH_NAME` or an OpenStrata
formation.

The deliverable is a recorded measurement on a large fixture:

```text
asset size                       >= 1 GB
bytes transferred for a bounded query
requests issued
cache hit ratio
wall-clock time to first authored prim
```

compared against the full-download baseline. If the abstraction leaked — if the
consumer needed any change that mentions HTTP — that is the finding, and it is
fixed here rather than documented as a limitation.

See [consumer integration](consumer-integration.md).

### `v0.6.0` — composition and extension points

Scope: the configuration surface (block size, budgets, timeouts, retry policy)
resolved from environment and context rather than hard-coded; the request
interception point for authentication, with no credential reaching a cache key,
a log, or a diagnostic; OpenStrata formation composition so a consumer
workspace can pull this resolver as a pinned artifact; and the packaged
cross-platform release.

Out of scope: any concrete auth provider. The point is the seam, not SigV4.

## Phases

| Phase | Scope | Status | Notes |
| --- | --- | --- | --- |
| 0 | Project scaffolding, boundary documentation, and contracts | Complete | CI landed as `core-ci.yml`; `openstrata.ci.yaml` moves to phase 2, which brings the first bundle a cell can name |
| 1 | Read contract, diagnostics, metrics, local backend, shared boundary suite | Complete | The suite is in `tests/boundary`, the local backend passes it, and the core and sanitizer lanes run in CI |
| 2 | HTTP backend, resolver bundle, validator capture and revision binding | Complete for `v0.2.0` | The backend, the bundle, the support matrix, and the recorded I/O baseline have all landed; a stage opens over HTTP on all three platforms, and a bounded query moves 0.0025 of a 128 MiB asset. What remains is the release gate, and one deliberate omission: a metadata fallback for a server that refuses `HEAD`, which no corpus row exercises |
| 3 | Block cache, coalescing, single-flight | Complete for `v0.3.0` | Measured, not guessed; validator-keyed from the start |
| 4 | Identity exposure, persistent cache, stability metadata | Complete for `v0.4.0` | Everything that makes identity outlive a reader — and, with the disk tier, outlive the process |
| 5 | First consumer integration and amplification baseline | Planned for `v0.5.0` | The abstraction's real test |
| 6 | Configuration, auth seam, formation composition, packaging | Planned for `v0.6.0` | Seams only, no providers |
| 7 | Second consumer (`usd-3dgs-plugins`) | Deferred | Camera-driven streaming; validates generality |
| 8 | Additional transports | Deferred | S3, package-internal, content-addressed |
| 9 | Wasm and browser composition | Research | Constrained by ADR-0003: a `usdAssetWasm` backend over `fetch`, not a rebuild of `usdAssetHttp` |

## Workstreams

| Workstream | Scope | Phases | Status |
| --- | --- | --- | --- |
| W1 | Read contract, metadata, validator value types, typed diagnostics, metrics | 1 | Done |
| W2 | Local backend and the shared boundary suite | 1 | Done |
| W3 | HTTP transport, redirects, timeouts, retry | 2 | Done |
| W4 | `ArResolver` bundle, URI normalization, `ArAsset` surface | 2 | Done — `plugins/http-resolver`, and with it the `HTTPxxx` projection and the environment surface |
| W5 | Hostile-server fixture corpus | 2 | Done — and consumed, by `tests/corpus` |
| W6 | Validator capture, `If-Range`, `AssetChanged`, revision binding | 2 | Done — shipped with W3, not after it |
| W6a | The recorded I/O baseline and the fixture it needed | 2 | Done — `tests/baseline`, the five scenarios of METRICS.md §6; [the record](../reference/BASELINE.md) |
| W7 | Block cache, coalescing, single-flight, eviction | 3 | Done |
| W8 | Identity exposure, persistence, cross-stage reuse rules | 4 | Done — `GetAssetInfo`, `DiskBlockStore`, and the one rule that governs both |
| W9 | Consumer integration and amplification baselines | 5 | Planned |
| W10 | Configuration, auth seam, packaging, formation composition | 6 | Planned |
| W11 | Async, prefetch, Wasm research | Parallel | No release gate |

W1 and W2 exist to make W3 cheap and verifiable. The order is not negotiable:
an HTTP backend written before the boundary suite is an HTTP backend whose bugs
are indistinguishable from server behavior.

That claim has now been tested rather than asserted. W3 entered the suite as one
row and a single line of CMake, the suite itself was not touched, and the two
defects the work surfaced — a deadline mid-body being resumed as though it were
a short read, and a connection-reuse detail that misnamed which deadline had
elapsed — were both found by a suite that already passed rather than argued
about against a live server.

W6 is deliberately in phase 2 rather than alongside W8. It is the part of
validator work that a range backend cannot ship without; W8 is the part that
only matters once identity is handed to somebody else.

## Documents

- [Implementation status](implementation-status.md)
- [Consumer integration](consumer-integration.md)

Related documents outside this directory:

- [Design policy](../design/DESIGN_POLICY.md)
- [Workspace contract](../architecture/WORKSPACE.md)
- [Asset reader contract](../architecture/ASSET_READER.md)
- [Resolver contract](../architecture/RESOLVER.md)
- [Cache contract](../architecture/CACHE.md)
- [Diagnostics contract](../architecture/DIAGNOSTICS.md)
- [Metrics contract](../architecture/METRICS.md)
- [Boundary suite contract](../contributing/BOUNDARY_SUITE.md)
- [Capability matrix](../reference/CAPABILITY_MATRIX.md)
- [OpenUSD compatibility](../compatibility/OPENUSD.md)
- [ADR-0001: consumer interface](../adr/0001-consumer-interface.md)
- [ADR-0002: range-unsupported policy](../adr/0002-range-unsupported-policy.md)
- [ADR-0003: HTTP client dependency](../adr/0003-http-client-dependency.md)

## Consumer order

The order is chosen so each consumer tests something the previous one could
not:

| Order | Consumer | What it validates |
| --- | --- | --- |
| 1 | `usd-pointcloud-plugins` (COPC) | A format with a real internal index; bounded queries over a large asset |
| 2 | `usd-3dgs-plugins` | Camera-driven, changing access patterns; whether caching survives non-sequential locality |
| 3 | `usd-vrm-plugins` / container formats | Nested random access: a range inside a package inside a URL |
| 4 | Material libraries | Many small assets rather than few large ones; the opposite request profile |

Each consumer is admitted only if it needs no HTTP code of its own. A consumer
that requires an exception is a defect in this repository.
