# Development Policy

Last updated: 2026-08-16

This document is the standing development policy for `usd-http-resolver`. The
roadmap and architecture documents refine it; they do not override it.

## 1. Purpose

`usd-http-resolver` is an OpenUSD asset resolver and the I/O substrate beneath
it. Its purpose is not to make `http://` openable. Its purpose is to give
OpenUSD **remote random access**: the ability for a FileFormat Plugin to read
the bytes it actually needs out of a large remote asset, without downloading the
asset and without knowing how the bytes arrived.

```text
USD Stage / Hydra
        |
        v
USD Composition                     logical structure
        |
        v
FileFormat Plugin                   "I need bytes [o, o+n)"
        |
        v
usd-http-resolver                   byte ranges, cache, consistency
        |
        v
HTTP / object storage / CDN         blocks
```

The value is the composition of three existing things — HTTP range requests,
OpenUSD asset resolution, and FileFormat Plugins — into one format-independent
remote random-access architecture. HTTP is the first transport, not the thesis.

### 1.1 What this repository is not

It is not a point-cloud project, a Gaussian-splat project, or a VRM project. It
never parses an asset's contents. It has no format knowledge, no octree, no
index model, and no idea what a chunk means.

It is not a server. Assets are served by ordinary static hosting, object
storage, or a CDN. A design that requires a `usd-http-resolver`-specific server
is rejected outright; see §4.

It is not a generic storage abstraction layer. Only the abstraction that HTTP
range access actually demands is built. S3, IPFS, and database backends are
possible later consequences of that abstraction, not inputs to it.

## 2. Current Assessment

The read contract, the local backend, and the shared boundary suite are
implemented and passing. No resolver, no transport, and no cache is. The
contracts under [architecture/](../architecture/) were written before their
implementation, which is deliberate: the boundary is the product, and it is
cheaper to fix here than in five consumers.

The properties to establish, in order, are in the
[roadmap](../roadmap/README.md). The invariants to preserve from the first
commit, through every reordering of that roadmap, are:

1. `usdAssetIo` does not know OpenUSD exists.
2. A backend does not know another backend exists.
3. The cache does not know a transport exists.
4. A consumer does not know the internal `AssetReader` API exists.
5. No build-time dependency exists between a consumer and this repository, in
   either direction.
6. Remote correctness is proven by comparison against the local backend.
7. No asset format is parsed anywhere in this repository.
8. No credential reaches a cache key, a diagnostic, a log, or a persisted
   artifact.
9. A whole-asset download is never a silent fallback.
10. Correctness is implemented before optimization.
11. A performance parameter is decided by measurement, not by choice.
12. A new transport is evaluated first against the existing `AssetReader`
    contract, and a transport that does not fit is a question about the
    contract before it is a request for an exception.

A change that violates one of these is a change to this document first.

## 3. Design Principles

### 3.1 Dependency direction

```text
ArResolver bundle -> backend selection -> cache -> backend -> transport
                                                        |
                                     local file  <------+------>  HTTP
```

Rules:

- Core contracts do not depend on the OpenUSD API. Only the plugin bundle
  includes OpenUSD headers.
- A backend never depends on another backend.
- The cache depends on the read contract, never on HTTP concepts. It stores
  bytes keyed by an opaque validator; it does not know what an `ETag` is.
- No module in this repository parses an asset format.
- No consumer repository is a build-time or test-time dependency.

The complete legal set is in the
[workspace contract](../architecture/WORKSPACE.md).

### 3.2 Resolution and byte access are two responsibilities

`ArResolver` answers *which asset*. A reader answers *which bytes*. They are
designed as two layers even though one plugin registers both:

```text
Asset Resolution            https://host/a/b.copc  ->  resolved identity
        +
Random Access Reader        (identity, offset, size) -> bytes
```

Collapsing them produces a resolver that must re-derive transport state on
every read, and a reader that cannot be tested without a URL. Keeping them
apart is what makes the local backend a usable correctness oracle.

### 3.3 The consumer interface is `ArAsset`

A FileFormat Plugin consumes `pxr::ArAsset::Read(buffer, count, offset)` and
nothing else from this project. It does not include a header from this
repository, link a library from it, or name it in CMake. Runtime composition
through `PXR_PLUGINPATH_NAME` is the entire integration surface.

This is not a stylistic preference. The first consumer's own contract forbids a
build-time dependency on any resolver, and every later consumer inherits that
rule. The decision and its consequences are recorded in
[ADR-0001](../adr/0001-consumer-interface.md).

The internal `AssetReader` abstraction described in
[ASSET_READER.md](../architecture/ASSET_READER.md) is therefore an
implementation contract between this repository's own layers, not a public SDK.
Publishing it as a public C++ API is a separate decision that requires a
concrete consumer that cannot be served by `ArAsset`.

### 3.4 Data side requires no special preparation

The following must be sufficient to use this project:

```text
a file on a static HTTP server that honors Range
```

No sidecar index, no manifest, no custom header, no registration step, and no
server extension. Formats that carry their own index — COPC, and most container
formats worth streaming — already contain everything needed. When a format does
not, that is the format's problem to solve in its own plugin, not a reason to
invent a protocol here.

### 3.5 Bytes are opaque

The resolver never inspects, decodes, validates, or reorders content. It has no
opinion about what offset is a header. Prefetch hints, when they exist, are
supplied by the caller; they are never inferred from content.

The single exception is HTTP-level framing — status codes, `Content-Range`,
`Content-Length`, redirects, and validators — which is transport, not content.

## 4. Transport Coverage

### 4.1 v0.x scope

```text
GET
HEAD, or a minimal metadata request where HEAD is unavailable
Range: bytes=a-b
Content-Length
Content-Range
Accept-Ranges
ETag
Last-Modified
If-Range
redirects
timeouts
bounded retry
```

Authentication, cloud-provider APIs, multipart ranges, and request signing are
out of v0.x scope. Multipart range responses are accepted only if a measurement
shows they beat coalesced single ranges; until then a request that would need
one is split.

### 4.2 Range support is a capability, not an assumption

A server may not honor `Range`. The resolver detects this from
`Accept-Ranges` and from the actual response status, and reports it as a
distinct, non-generic condition. Whether the fallback is a full download or a
hard error is a policy decision, not an implementation accident, and it is
decided in [ADR-0002](../adr/0002-range-unsupported-policy.md): in `v0.2.0` a
server without range support is a hard error, with no whole-asset fallback.

Silently downloading a 10 GB asset because a header was missing is the specific
failure that ADR exists to prevent. Bounded fallback for small assets is a
deferred feature with its own residency model, admitted on a demonstrated need
and a new ADR — never as a quiet widening of the first HTTP release.

### 4.3 Authentication is an extension point, not a parameter

Credentials never enter the resolver API, the cache key, a diagnostic message,
a log line, or any persisted artifact. When authentication arrives, it arrives
as a request-interception point resolved from the environment:

```text
request interceptor
credential provider
environment / session
```

`Authorization` headers, signed URLs, and SigV4 are then implementations of
that point. Public HTTP is the v0.x target.

### 4.4 Transports after HTTP

S3-compatible object storage, package-internal ranges, content-addressed
storage, and the Wasm `fetch` backend are candidates. Each is admitted only
when it can be expressed through the existing read contract without widening
it. A transport that requires a new concept in the core contract is a signal
that the abstraction is wrong, and it is treated as a design question rather
than a feature request.

## 5. Cache Policy

Two caches exist in the whole system, with one owner each:

```text
raw byte / range cache          -> this repository
generated USD / payload cache   -> the consuming plugin repository
```

They never merge and never share a key. The consumer's generated-cache contract
is its own; see the first consumer's
[resolver-backed source contract](https://github.com/animu-sphere/usd-pointcloud-plugins/blob/main/docs/architecture/RESOLVER_SOURCE.md).

Standing rules:

- The cache is a block cache. Reads are aligned and expanded to block
  boundaries so that many small reads become few large requests.
- The cache key includes the resolved identifier, the block index, and an
  opaque validator. An identifier match alone is never a hit.
- The cache is deletable and reproducible. Losing it costs time, never
  correctness.
- Cache behavior is measured before it is tuned. Block size is a measured
  constant, not a guessed one.

The full contract is in [CACHE.md](../architecture/CACHE.md).

## 6. Consistency Policy

Remote random access has a correctness problem that local file access does not:
the asset can change between two reads of the same asset. Mixing bytes from two
revisions produces corruption that looks like a decoder bug.

The policy is:

- An asset is bound to a validator at open time, and one reader is bound to one
  revision for its whole lifetime.
- Every subsequent range read carries that validator (`If-Range`, or an
  equivalent for the transport).
- A detected change is surfaced as a distinct `AssetChanged` condition. It is
  never repaired silently by re-reading, and never hidden behind a generic I/O
  error.
- Assets are treated as immutable. Publishing a new revision at a new path is
  the supported editing model; in-place mutation is not a use case this project
  optimizes for.

This is an obligation of the first HTTP backend, not of the cache. A reader
issuing three range requests without a validator can return a header from one
revision and records from another, with every request succeeding and nothing to
report — a corruption that appears at the format plugin as a malformed asset.
Validator capture therefore ships in `v0.2.0`, alongside the first backend that
can violate the guarantee. The mechanism is §2.1 and §7 of the
[asset reader contract](../architecture/ASSET_READER.md).

Validator strength is transport knowledge and stays in the backend. What
crosses the boundary upward is one classification — `Stable`, `Unstable`, or
`Unavailable` — which is what a consumer needs and all it may act on. When a
server supplies no usable validator, reads still work, in-memory caching works
for the reader's lifetime, nothing persists beyond it, and the consumer is told
the identity is not stable so it can disable its own generated-cache reuse.

## 7. Concurrency

OpenUSD and Hydra read in parallel, so every public path is thread-safe from
the first implementation, not retrofitted.

- No global lock. A global lock over a network cache serializes the entire
  stage.
- Lock granularity is per asset and per block.
- Concurrent readers requesting the same missing block issue one request, not
  N. Duplicate in-flight requests are a bug, not an inefficiency.
- Cancellation propagates. A closed stage must not keep sockets alive.

## 8. Diagnostics

Transport detail is neither leaked nor erased. A consumer must be able to
distinguish these without parsing a message string:

```text
NotFound
AccessDenied
RangeNotSupported
NetworkError
Timeout
InvalidResponse
AssetChanged
Cancelled
```

Requirements:

- Codes are stable and versioned.
- Messages are for humans and never contain credentials, tokens, signed-URL
  query strings, or `Authorization` values.
- The failing byte range is attached where available.
- Retries are visible in metrics, not silent.
- The plugin layer projects these onto stable `HTTPxxx` codes and OpenUSD
  diagnostics.

The full contract is in [DIAGNOSTICS.md](../architecture/DIAGNOSTICS.md).

## 9. Measurement

Measurement is a feature of this project, not instrumentation added later. The
entire claim of the architecture is a ratio, and an unmeasured ratio is a
marketing statement:

```text
asset size                       10 GB
bytes actually transferred       30 MB
```

The counters in [METRICS.md](../architecture/METRICS.md) — request count, bytes
requested, bytes transferred, bytes served from cache, coalescing ratio,
amplification ratio, retries, latency distribution — exist from the first HTTP
release, and a release that changes I/O behavior without a recorded baseline is
incomplete.

## 10. Security and Trust

A remote server is untrusted input.

- Never allocate from a server-declared length without a bound.
- Validate that a `Content-Range` response actually covers the requested range
  before copying it into a caller's buffer.
- A `206` that returns the wrong range is `InvalidResponse`, not data.
- Bound redirect chains and reject scheme downgrades from `https` to `http`.
- Cap retries and total time; never retry unboundedly on a non-idempotent
  condition.
- Cache files are written to a path the process owns, atomically, with no
  attacker-controllable filename component.

## 11. Testing

### 11.1 Local backend as oracle

Every correctness property is expressed as an equivalence, over the whole
result rather than the bytes alone:

```text
local.Read(offset, size).bytes     == http.Read(offset, size).bytes
local.Read(offset, size).bytesRead == http.Read(offset, size).bytesRead
local.Read(offset, size).status    == http.Read(offset, size).status
```

for all boundaries — start, end, block edges, past-EOF, zero-length, and
oversized reads — plus property-generated cases biased toward those same
boundaries. A remote result that cannot be compared to a local result is not a
test.

The shared suite that expresses this is the primary deliverable of `v0.1.0`,
not a by-product of it, and its contract is
[BOUNDARY_SUITE.md](../contributing/BOUNDARY_SUITE.md). Every backend passes it
unchanged; a backend that needs a case relaxed is either a defect in the read
contract or is not admissible.

### 11.2 Server behavior corpus

A test server reproduces the hostile cases deliberately: no `Accept-Ranges`,
`200` in response to a `Range` request, a truncated body, a wrong
`Content-Range`, a mid-read `ETag` change, a redirect chain, a slow response, a
connection reset, and a `416`. These are the cases that decide whether the
project is trustworthy, and they run in CI without network access.

### 11.3 Amplification tests

A test asserts the byte ratio, not only correctness: reading a 64 KB header out
of a large fixture transfers on the order of one block, not the asset.

### 11.4 Concurrency tests

Parallel readers over the same asset, with sanitizers, asserting single-flight
behavior on shared blocks. The core libraries are tested under
AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer from
`v0.1.0`; UBSan is named explicitly because offset and size arithmetic is where
this project's overflow lives.

### 11.5 Cross-repository integration

Integration with a real consumer is composed through OpenStrata, not through a
build dependency. It is a separate lane and never a gate on repository-local
tests. See [consumer integration](../roadmap/consumer-integration.md).

## 12. Repository Shape

```text
libs/
  usd-asset-io/          read contract, metadata, diagnostics, metrics
  usd-asset-cache/       block cache, coalescing, single-flight
  usd-asset-local/       local file backend
  usd-asset-http/        HTTP backend and its client dependency
plugins/
  http-resolver/         the ArResolver bundle; the only OpenUSD consumer
tests/
docs/
  architecture/
  reference/
  guides/
  design/
  adr/
  compatibility/
  contributing/
  roadmap/
  releases/
  reports/ost/
```

A directory is created when its first tested capability exists, not in advance.
Every directory under `libs/` and `plugins/` carries a `README.md`; see the
[module README contract](../contributing/MODULE_README_CONTRACT.md). The
binding layout is the [workspace contract](../architecture/WORKSPACE.md).

## 13. Licensing and Distribution

Project code is Apache-2.0.

The HTTP client is the one real third-party decision in this repository, and it
is made on license and footprint before features. A dependency whose license
constrains redistribution of a plugin binary is disqualified; a dependency that
cannot build for the Wasm target is a strategic liability. The choice is
recorded as an ADR when it is made, with the Wasm path considered at the time
of choosing rather than after.

It has been made: [ADR-0003](../adr/0003-http-client-dependency.md) selects
libcurl, under the curl license, linked privately and statically by
`usdAssetHttp` alone. No license criterion disqualified any serious candidate,
and the Wasm criterion is the one this decision pays for: libcurl does not
build for the Wasm target, and the answer is the reserved `usdAssetWasm`
backend over `fetch` rather than a rebuild of the HTTP one. The ADR records
that as an accepted cost.

## 14. Documentation

Documented support matches implemented behavior exactly.
[CAPABILITY_MATRIX.md](../reference/CAPABILITY_MATRIX.md) states what the tree
does; every other document may describe intent as long as it is labeled.

A performance claim requires a recorded measurement. "Only downloads what it
needs" is not a documentable property until a counter proves it on a named
fixture.

## 15. Deliberately Not Doing

- A custom HTTP server, a database server, or a custom network protocol.
- A custom URI scheme in v1. `https://` plus validators carries content
  identity adequately; `hash://` style content addressing is revisited only
  when cross-stage cache sharing is a demonstrated need.
- HTTP code in any consumer repository.
- A generic storage abstraction designed ahead of a second real backend.
- Asset mutation, upload, or server-side editing APIs.
- Format knowledge of any kind.
- Async and prefetch APIs before synchronous reads are correct and measured.

Not doing *yet*, and deliberately not designed until the `AssetReader` contract
and the HTTP implementation have actually run:

```text
concrete authentication provider
S3, package, and Wasm backend implementations
async API and speculative prefetch
persistent cache implementation
bounded whole-asset fallback
write and upload
content-addressed storage
generated USD caching
```

Each is a candidate, not a commitment, and each is admitted on the same test:
whether it fits the existing contract. The failure mode to avoid is the
inverted one — adding a backend-specific API because a backend does not fit.
A backend that does not fit is evidence about the contract's generality, and it
is investigated as that before it is accommodated.

## 16. Definition of Done

A transport backend counts as supported only when all of the following hold:

- It satisfies the read contract in
  [ASSET_READER.md](../architecture/ASSET_READER.md) at every boundary.
- It is byte-equivalent to the local backend over the shared fixture set.
- Its failure modes map onto the typed diagnostic vocabulary.
- The hostile-server corpus passes.
- Concurrency tests pass under sanitizers.
- Metrics counters are populated and a baseline is recorded.
- Its third-party dependencies and their licenses are recorded.
- Its module `README.md` states what it owns and refuses.

## 17. Immediate Actions

1. Stand up the fixture server before writing the HTTP backend, so the backend
   is written against a passing oracle rather than debugged against a server.
2. Ship validator capture, conditional range requests, and `AssetChanged` with
   the first HTTP backend rather than after it. A range backend without
   revision binding is not a correct range backend.
3. Write `openstrata.ci.yaml` with the first bundle, and generate its workflows.
   It could not be written for `v0.1.0`: every `ost` cell pins and materializes
   an OpenUSD runtime, which the core lanes must not, and no cell can name a
   workspace that contains no bundle. The runtime-free lanes stay hand-authored
   in `.github/workflows/core-ci.yml` even then. See
   [report 01](../reports/ost/01-2026-08-16-v0.1.0-ci-without-a-support-matrix.md).

Done and no longer pending:

- [ADR-0002](../adr/0002-range-unsupported-policy.md) is resolved — hard error
  in `v0.2.0`.
- [ADR-0003](../adr/0003-http-client-dependency.md) is resolved — libcurl,
  acquired through a private `find_package` and reached only through a narrow
  internal transport seam. It was action 1 here, and it precedes the backend.
- The sanitizer lanes run. ASan, UBSan, and TSan pass over the core path, in CI
  and locally, so the concurrency and overflow properties are verified rather
  than configured.
- The root build graph is libs-first, and the core path builds and tests with no
  OpenUSD installation present.
- The read contract, the validator value types, the diagnostic vocabulary, and
  the metrics counters are fixed in code, and the local backend implements them.
- The shared boundary suite exists, is parameterized over backends, and the
  local backend passes it. The HTTP backend is now written against a passing
  oracle, which is the whole reason this order was not negotiable.
