# Development Policy

Last updated: 2026-09-04

This document is the standing development policy for `usd-http-resolver`. The
roadmap and architecture documents refine it; they do not override it.
[DIRECTION.md](DIRECTION.md) sits beside it and states where the project is
going over a longer horizon; it is direction rather than policy, and where the
two disagree this document wins.

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

### 1.2 The name is narrower than the architecture

```text
Repository            usd-http-resolver
Internal concept      remote asset access
```

The repository keeps its name for as long as HTTP is the only transport anybody
uses; renaming a repository is a cost with no engineering payoff. What the
distinction buys is a rule that applies to every commit: no module below
`plugins/` may be written in a way that only HTTP could satisfy. That is
invariants 1, 3, and 12 below, and it is what leaves room to lift a common layer
out later — for S3, for content-addressed storage, for a `fetch` backend in a
browser — without a rewrite. Extracting it is a possible consequence of having a
second and third transport, never a prerequisite for building them. See
[DIRECTION.md](DIRECTION.md) §1.

## 2. Current Assessment

The read contract, the local backend, the shared boundary suite, the
hostile-server corpus, the HTTP backend, the `ArResolver` bundle, the block
cache, identity exposed to consumers, the on-disk cache under it, and the
packaged product that composes the bundle as a runtime component are implemented
and passing, and released through `v0.5.0`. What is not is the first consumer
integration, the configuration and authentication seams, adaptive read-ahead,
and every transport after HTTP. The contracts under
[architecture/](../architecture/) were written before their implementation,
which is deliberate: the boundary is the product, and it is cheaper to fix here
than in five consumers — and every one of those implementations has since landed
against a contract that did not have to move to accept it.

The measurement this repository still cannot make is the one that matters most
to everything left: every number in [BASELINE.md](../reference/BASELINE.md) is a
loopback number. The trade this architecture makes is bytes for round trips, and
so far only the numerator has been measured. Distance arrives with the first
consumer integration, and several deferred decisions — read-ahead above all —
are deferred precisely because they cannot be tuned before it.

Invariant 11 below is worth checking against the tree rather than assuming, and
it holds: the cache's block size and coalescing gap come from a recorded sweep
(`tests/cache-tuning`, [BLOCK_POLICY.md](../reference/BLOCK_POLICY.md)), and the
two constants in that set that were *not* measured are labelled there as the
bounds they are rather than presented as tuned values.

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

The cache arrives in levels, each measurable before the next is built:

| Level | What it does | Status |
| --- | --- | --- |
| 1 | Request de-duplication: one missing region, one request, however many readers want it | `v0.3.0`, as per-block single-flight |
| 2 | Block cache with alignment, expansion, and coalescing | `v0.3.0` |
| 3 | Adaptive read-ahead: prefetch ahead of a sequential pass, suppress it under a random one | Not implemented |
| 4 | Persistence across processes, keyed by identifier, validator, and block | `v0.4.0`, `Strong` validator only |

Level 3 is last despite being simpler than level 4, and the reason is the last
rule in the list above rather than difficulty. Read-ahead trades bytes for round
trips, and a loopback fixture prices a round trip at nearly zero; a policy tuned
there is tuned against the wrong cost. It waits for a measurement taken over
real distance. See [DIRECTION.md](DIRECTION.md) §10.

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

Two things are untrusted here, and they are untrusted for different reasons.

### 10.1 The server is untrusted input

- Never allocate from a server-declared length without a bound.
- Validate that a `Content-Range` response actually covers the requested range
  before copying it into a caller's buffer.
- A `206` that returns the wrong range is `InvalidResponse`, not data.
- Bound redirect chains and reject scheme downgrades from `https` to `http`.
- Cap retries and total time; never retry unboundedly on a non-idempotent
  condition.
- Bound the response header block and the total response size, not only the
  body the caller asked for.
- Decode nothing. Every request carries `Accept-Encoding: identity`, which is
  two rules at once: the byte accounting in
  [METRICS.md](../architecture/METRICS.md) describes the asset rather than the
  wire, and a client that decompresses is a client with an unbounded output for
  a bounded input. A decompression bomb is not a case to defend against here
  because there is no decompressor to feed.
- Cache files are written to a path the process owns, atomically, with no
  attacker-controllable filename component.

### 10.2 The URL is untrusted input

A resolved identifier can arrive from a USD layer that a user did not author,
which makes this project a request-forgery primitive if it is careless. In a DCC
or a render farm that is the more consequential of the two risks, because the
process is usually inside a network the caller is not.

The policy is that reach is bounded by declared policy rather than by whatever
the host's resolver stack happens to permit:

- The scheme set is an allowlist. This resolver claims `http` and `https` and
  refuses everything else, including after a redirect — a redirect to `file:`,
  `ftp:`, or a scheme it does not register is a refusal, not a follow.
- Redirect targets are re-checked against the same policy as the original
  request. A policy applied only at the first hop is not a policy.
- Whether loopback and private-network destinations are reachable is a
  deliberate, configurable decision with a documented default, not an accident
  of what the resolver happened to allow. The fixture server makes loopback a
  *tested* destination, so the setting has to distinguish a test from a
  deployment rather than forbid one to protect the other.
- The number of requests one resolution or one read may cause is bounded, so a
  redirect chain, a retry budget, and a resume loop cannot compose into an
  unbounded one.

Most of that is implemented and recorded in
[CAPABILITY_MATRIX.md](../reference/CAPABILITY_MATRIX.md): the redirect bound,
the `https` → `http` refusal, the retry budget shared with the resume loop rather
than nested inside it, and the scheme allowlist, which is applied at every hop
because a redirect target is parsed by the same parser as an original identifier
and that parser accepts two schemes. A `Location` naming `file:` is an unusable
location, not a followed one.

Two things are named here as scope rather than as shipped properties. The
destination policy — whether loopback and private-network addresses are
reachable — does not exist, and its difficulty is that the hostile-server corpus
*is* loopback, so the setting has to distinguish a fixture from a deployment
rather than forbid one to protect the other. Nor is the response header block
separately bounded; today the caller's buffer bounds the body and nothing bounds
what precedes it. Both land with the configuration surface, because a policy with
no way to state it is a default nobody can override.

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

### 11.6 Fuzzing

Every parser in this repository consumes bytes an attacker may choose, and each
one is small enough that a fuzzer covers it properly rather than superficially.
The targets, in the order their inputs are least trustworthy:

```text
Content-Range and Content-Length parsing
HTTP response metadata: validators, Accept-Ranges, Location
URI normalization and RFC 3986 reference resolution
the persistent cache entry header
```

The last is not a network input and is on the list anyway: a cache directory is
a file a different process wrote, and `v0.4.0` already treats a corrupt entry as
something to discard rather than trust. A fuzzer is how that claim stops being a
claim.

This is CI work rather than release work — a corpus that grows, run under
AddressSanitizer and UndefinedBehaviorSanitizer, with any crash committed as a
regression case in the ordinary suite. It gates no release, and it is listed
here so that "the parsers are small" is recorded as a reason to fuzz them rather
than a reason not to.

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

### 15.1 The admission test

Applied to anything proposed from here on, including things not on either list
above.

| Easy to accept | Needs an argument, usually an ADR |
| --- | --- |
| Reusable from more than one FileFormat Plugin | Logic specific to one file format |
| Expressible without naming HTTP | HTTP detail pushed into USD schema or metadata |
| Contributes to random access rather than to whole-asset transfer | A bespoke index format |
| Consistent with OpenUSD's own conventions | A requirement for a cooperating, non-static server |
| Requires no special preparation of the data | libcurl types on a public surface |
| Portable to OpenStrata composition and to a Wasm build | The resolver understanding a format's internal structure |

The right-hand column is not a prohibition list. It is the set of proposals that
have to justify themselves against §2's invariants before they are built, and
the record of that justification is an ADR rather than a commit message.

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

The standing instruction, which outranks the list: do not widen the feature set.
Everything below is either a correctness property of what already exists or the
one integration that decides whether the abstraction is real.

1. **The first consumer integration.** `usd-pointcloud-plugins` opening a remote
   COPC asset through this resolver with no HTTP code of its own, and the
   recorded amplification baseline that goes with it. It needs a fixture of at
   least a gigabyte and somewhere to host it. See
   [consumer integration](../roadmap/consumer-integration.md).
2. **A measurement taken over distance.** Every recorded number is a loopback
   number, which prices a round trip at nearly zero and therefore cannot price
   the trade this architecture makes. This is not a separate task from 1 so much
   as the reason 1 is first.
3. **The configuration surface and the network policy that needs it**, per §10.2
   and [CONFIGURATION.md](../reference/CONFIGURATION.md): the transport bounds
   resolved from `ArResolverContext` as well as the environment, and the scheme
   and destination policy stated somewhere a host can override.
4. **The authentication interception point**, per §4.3 — the seam, and no
   provider.
5. **Adaptive read-ahead**, cache level 3 in §5, *after* 2 and not before it.

Deliberately not on this list, and each for a stated reason: freezing the
internal API before the consumer integration has argued with it (§3.3);
read-ahead tuned on loopback (§5); an async surface before the synchronous one is
measured (§15); and any second transport before the first has a consumer (§4.4).

Done and no longer pending:

- The packaged, composable product. `v0.5.0` publishes the workspace as an
  aggregate product with a component-owned acceptance probe that runs from the
  installed artifact rather than from a producer build directory, which is the
  half of §7 of [DIRECTION.md](DIRECTION.md) that this repository owes
  OpenStrata on its own. Formation-level composition is the other half and is
  not done.
- Phase 3, the block cache, whose definition of success was the table in
  [BASELINE.md](../reference/BASELINE.md) § *What the next release has to move*.
  It moved: the clustered index read went from 18 requests to 3, eight parallel
  readers from 152 to 25, `bytesOverFetched` is recorded rather than hidden, and
  the full sequential read is byte for byte and request for request what it was.
- Phase 4, identity exposure and persistence, under one rule that governs both
  halves — `Strong` yes, `Weak` no, `None` no.

- The `v0.2.0` release gate is walked and
  [its record](../releases/v0.2.0.md) is written. Gates 4 and 6 bound for the
  first time and both pass. Gate 9 did not bind after all: it binds a release
  that publishes a binary package, and `v0.2.0` is a source tag. It was measured
  anyway, because this is the first release whose install rules produce a bundle
  — two independent builds agree on 24 of 28 installed files, and the four that
  differ differ only in embedded build timestamps.
- Validator capture, conditional range requests, and `AssetChanged` shipped with
  the first HTTP backend rather than after it, which is what makes it a correct
  range backend rather than a fast one.

- The release's I/O baseline is recorded. `tests/baseline` runs the five
  scenarios §6 of [METRICS.md](../architecture/METRICS.md) names against a
  128 MiB synthetic asset on loopback, and
  [the record](../reference/BASELINE.md) states what a bounded query costs: a
  quarter of one percent of the asset, with `amplification` at exactly 1.0.
  The instrumentation was never what was missing; the fixture was, and it is
  synthesized rather than committed.

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
- `openstrata.ci.yaml` is written and its workflow is generated. It was action 2
  here and could not have been done for `v0.1.0`: no cell can name a workspace
  that contains no bundle. The runtime-free lanes stayed hand-authored in
  `.github/workflows/core-ci.yml`, as this action said they would, and one lane
  more than expected joined them — the Windows plugin lane, because libcurl
  there comes from vcpkg and a generated cell can neither install it nor hand
  CMake a prefix. See
  [report 01](../reports/ost/01-2026-08-16-v0.1.0-ci-without-a-support-matrix.md)
  and
  [report 03](../reports/ost/03-2026-08-18-a-support-matrix-with-one-hand-authored-lane.md).
- The fixture server is up, and it was action 1 here. `tests/fixture-server`
  serves the corpus in §11.2 over loopback, and its own self-test proves each
  behavior puts on the wire what its name claims — checked against deliberately
  broken servers before it was trusted, because an unchecked corpus is not an
  oracle but a second unknown. Nothing consumes it yet, and that is the expected
  state: it exists so that the backend has something to be wrong against.
