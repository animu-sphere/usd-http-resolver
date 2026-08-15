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

The first three releases build one vertical slice: a read contract, a local
backend that proves it, and an HTTP backend that is byte-equivalent to the
local one over a hostile test server. Only then does caching enter, because a
cache in front of an unproven reader hides its bugs.

`v0.5.0` is the first release with an external claim: `usd-pointcloud-plugins`
opens a remote COPC asset through this resolver, and the recorded byte ratio
demonstrates the architecture. Everything before it is infrastructure that must
be right; everything after it is reach.

| Release | Theme | Outcome |
| --- | --- | --- |
| `v0.1.0` | Read contract and local backend | One random-access contract, typed diagnostics, metrics counters, and a local backend that satisfies them |
| `v0.2.0` | HTTP range reads and the resolver bundle | `http`/`https` resolve and serve ranges; byte-equivalent to local against a hostile fixture server |
| `v0.3.0` | Block cache, coalescing, single-flight | Small scattered reads stop becoming small scattered requests |
| `v0.4.0` | Validators and consistency | Validator-bound reads, `AssetChanged` detection, and identity classification exposed to consumers |
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

### `v0.1.0` — read contract and local backend

The release that decides whether the rest of the project is buildable. It ships
no network code.

Scope: the `AssetReader` and `AssetMetadata` contracts fixed in
[ASSET_READER.md](../architecture/ASSET_READER.md); the typed diagnostic
vocabulary in [DIAGNOSTICS.md](../architecture/DIAGNOSTICS.md); the counter set
in [METRICS.md](../architecture/METRICS.md); a local file backend implementing
all of it; and the boundary test suite — zero-length reads, reads at EOF, reads
straddling EOF, reads past EOF, oversized reads, and concurrent reads — that
every later backend is required to pass unchanged.

Out of scope: HTTP, caching, `ArResolver` registration, async.

Exit criteria: the local backend passes the boundary suite under
AddressSanitizer and ThreadSanitizer; the suite is written so that swapping the
backend under test is a one-line change; counters are populated.

### `v0.2.0` — HTTP range reads and the resolver bundle

The first release that touches a network, and the first that registers an
OpenUSD plugin.

Scope: an HTTP backend supporting `GET`, a metadata request, `Range`,
`Content-Length`, `Content-Range`, `Accept-Ranges`, bounded redirects,
timeouts, and bounded retry; the `plugins/http-resolver` bundle registering the
`http` and `https` URI schemes and returning an `ArAsset` backed by that
backend; URI normalization and relative resolution per
[RESOLVER.md](../architecture/RESOLVER.md); and the fixture server that
reproduces the hostile cases in §11.2 of the design policy.

The HTTP client dependency is chosen in this release, on license, footprint,
and Wasm viability, and recorded as an ADR.

Out of scope: caching of any kind — every read is a request, deliberately, so
that the request pattern is visible before it is optimized. Also out of scope:
authentication, and the range-unsupported fallback, which stays an explicit
diagnostic until [ADR-0002](../adr/0002-range-unsupported-policy.md) is
resolved.

Exit criteria: the `v0.1.0` boundary suite passes against the HTTP backend
unchanged; the hostile-server corpus passes; `usdcat` on a remote `.usda`
succeeds on Windows, Linux, and macOS in CI; no credential-shaped string
appears in any diagnostic.

### `v0.3.0` — block cache, coalescing, and single-flight

The release that makes the architecture perform rather than merely work.

Scope: an aligned block cache; read expansion to block boundaries; coalescing
of adjacent and near-adjacent block fetches into one request; single-flight so
that N threads missing the same block issue one request; eviction with a
bounded memory budget; and the cache statistics in
[METRICS.md](../architecture/METRICS.md).

Block size, coalescing gap threshold, and budget are chosen from measurement
against a fixture with a realistic access pattern — a header read, an index
read, then scattered chunk reads — and the measurement is recorded.

Out of scope: on-disk cache persistence, which requires the validator work in
`v0.4.0` to be safe.

Exit criteria: byte-for-byte equivalence with the uncached path over the full
suite; a recorded before/after request count and amplification ratio for the
representative access pattern; single-flight proven under ThreadSanitizer.

### `v0.4.0` — validators and consistency

The release that makes remote reads trustworthy rather than merely fast.

Scope: validator capture at open (`ETag`, else `Last-Modified` plus size);
`If-Range` on every subsequent range request; detection and typed reporting of
mid-read asset change as `AssetChanged`; identity classification as `Stable`,
`Unstable`, or `Unavailable`; and exposure of that classification to consumers
through the resolver's asset-info surface, so a consumer can decide whether its
own generated-cache reuse is safe.

Optional on-disk cache persistence lands here or is deferred, keyed on the
validator, never on the URL alone.

Out of scope: content-addressed identity and cross-stage cache sharing.

Exit criteria: a test that mutates the fixture mid-read produces `AssetChanged`
and never mixed bytes; a cached entry from revision A never serves a read of
revision B at the same URL; the identity classification matches what the first
consumer's contract expects.

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
| 0 | Project scaffolding, boundary documentation, and contracts | In progress | OpenStrata project initialized; architecture contracts written before implementation |
| 1 | Read contract, diagnostics, metrics, local backend | Planned for `v0.1.0` | The boundary suite defined here is what every backend is held to |
| 2 | HTTP backend and resolver bundle | Planned for `v0.2.0` | Includes the HTTP client dependency decision |
| 3 | Block cache, coalescing, single-flight | Planned for `v0.3.0` | Measured, not guessed |
| 4 | Validators, consistency, identity classification | Planned for `v0.4.0` | Prerequisite for any persistent cache |
| 5 | First consumer integration and amplification baseline | Planned for `v0.5.0` | The abstraction's real test |
| 6 | Configuration, auth seam, formation composition, packaging | Planned for `v0.6.0` | Seams only, no providers |
| 7 | Second consumer (`usd-3dgs-plugins`) | Deferred | Camera-driven streaming; validates generality |
| 8 | Additional transports | Deferred | S3, package-internal, content-addressed |
| 9 | Wasm and browser composition | Research | Depends on the client dependency choice made in phase 2 |

## Workstreams

| Workstream | Scope | Phases | Status |
| --- | --- | --- | --- |
| W1 | Read contract, metadata, typed diagnostics, metrics | 1 | Planned |
| W2 | Local backend and the shared boundary suite | 1 | Planned |
| W3 | HTTP transport, redirects, timeouts, retry | 2 | Planned |
| W4 | `ArResolver` bundle, URI normalization, `ArAsset` surface | 2 | Planned |
| W5 | Hostile-server fixture corpus | 2 | Planned |
| W6 | Block cache, coalescing, single-flight, eviction | 3 | Planned |
| W7 | Validators, `If-Range`, `AssetChanged`, identity classes | 4 | Planned |
| W8 | Consumer integration and amplification baselines | 5 | Planned |
| W9 | Configuration, auth seam, packaging, formation composition | 6 | Planned |
| W10 | Async, prefetch, Wasm research | Parallel | No release gate |

W1 and W2 exist to make W3 cheap and verifiable. The order is not negotiable:
an HTTP backend written before the boundary suite is an HTTP backend whose bugs
are indistinguishable from server behavior.

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
- [Capability matrix](../reference/CAPABILITY_MATRIX.md)
- [OpenUSD compatibility](../compatibility/OPENUSD.md)
- [ADR-0001: consumer interface](../adr/0001-consumer-interface.md)
- [ADR-0002: range-unsupported policy](../adr/0002-range-unsupported-policy.md)

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
