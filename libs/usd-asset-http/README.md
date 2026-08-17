# usdAssetHttp

The HTTP backend: random access over `http` and `https` by byte range, bound
for a reader's lifetime to the asset revision it opened.

## Purpose

To make a remote asset readable the way a local file is — a caller asks for
bytes at an offset and gets exactly those bytes or a typed failure — over a
server it does not control and cannot trust. It is the first transport in this
repository and the first thing in it that touches a network.

The difficult part is not issuing a range request. It is that a remote asset can
change between two reads of it, and a range reader issues more than one request
per logical read. This module therefore ships validator capture, a conditional
guard on every range request, and `AssetChanged` from its first commit rather
than after it; see [ASSET_READER.md](../../docs/architecture/ASSET_READER.md)
§2.1.

OpenUSD is not required to build or test this module, and no OpenUSD header is
included anywhere in it. The whole of `libs/` builds and tests with no USD
runtime present, and this module's tests run in that configuration.

## Responsibilities

- Metadata discovery at open: size, range support, validator, content type.
- Range requests, and validation of the response framing **against the
  request**.
- Bounded redirects, with a scheme-downgrade refusal.
- Bounded retry, and separable connect, response, and transfer deadlines.
- Validator capture, classification, and the conditional guard derived from it.
- Detecting a mid-read revision change and reporting it as `AssetChanged`.
- Projecting every transport failure onto the typed vocabulary in
  [DIAGNOSTICS.md](../../docs/architecture/DIAGNOSTICS.md).
- Populating the counters in
  [METRICS.md](../../docs/architecture/METRICS.md).

## Non-responsibilities

- **Caching of any kind.** Every read is a request, deliberately, so that the
  request pattern is visible before `v0.3.0` optimizes it.
- **Coalescing, alignment, or read expansion.** The cache decorator's job.
- **URI normalization and anchoring.** The resolver's job
  ([RESOLVER.md](../../docs/architecture/RESOLVER.md) §2.1). This module parses
  a URL only as far as following a redirect requires, and takes an absolute
  `http` or `https` URI as given.
- **Authentication.** Out of `v0.x` scope. Credentials in a URL's userinfo are
  carried to the socket and removed from every identity, message, and counter.
- **Interpreting bytes.** No format knowledge exists here or anywhere in this
  repository.
- **A whole-asset fallback when a server will not serve ranges.** Terminal, per
  [ADR-0002](../../docs/adr/0002-range-unsupported-policy.md).

## Public API

```cpp
namespace usdasset::http {

struct HttpOptions {                 // all of it bounded
    int connectTimeoutMs  = 10000;
    int responseTimeoutMs = 30000;
    int transferTimeoutMs = 300000;
    int maxRedirects      = 5;
    int maxAttempts       = 3;       // requests per logical operation, retries included
    std::string userAgent;           // empty takes the default
};

class HttpAssetReader final : public AssetReader {
    const AssetMetadata& Metadata() const override;
    ReadResult Read(std::uint64_t offset, void* dst, std::size_t size) override;
    const ReaderMetrics& Metrics() const noexcept;
};

HttpOpenResult Open(const std::string& url);
HttpOpenResult Open(const std::string& url, const HttpOptions& options);
OpenResult     OpenAsset(const std::string& url);                        // the suite's row
OpenResult     OpenAsset(const std::string& url, const HttpOptions& options);
}
```

`HttpOptions` is a parameter rather than a set of constants so that a test can
make a deadline elapse in milliseconds. It is not yet resolved from the
environment or from an `ArResolverContext`; that is the configuration surface in
`v0.6.0`.

## Dependencies

```text
usdAssetHttp -> usdasset::io          public
usdAssetHttp -> CURL::libcurl         private, and named nowhere else
```

This is the only module in the repository permitted to name an HTTP client
([WORKSPACE.md](../../docs/architecture/WORKSPACE.md) §2, invariant 4). The
dependency is private in both senses CMake distinguishes: it is linked
`PRIVATE`, so no consumer of this static library inherits it, and `curl.h`
appears in exactly one translation unit — `src/CurlTransport.cpp` — and in no
installed header.

## Data flow

```text
Open(url)
  -> parse the URL                      absolute http/https, or InvalidArgument
  -> HEAD, following bounded redirects  one metadata round trip, no content
  -> size from Content-Length           absent is a refusal, not a guess
  -> range support from Accept-Ranges   absent is RangeNotSupported, terminal
  -> capture validator, classify        Stable | Unstable | Unavailable
  -> AssetReader

Read(offset, size)
  -> ResolveReadRange                   shared with every other backend
  -> empty or past EOF: 0, Ok           no request is issued
  -> GET Range: bytes=a-b               with If-Range where one may be sent
  -> validate framing against the request
  -> copy, or fail; never a hole
```

The read loop asks for the remainder rather than the whole range again when a
transfer stops early, so a body that ends below its declared length is resumed
rather than re-fetched — bounded by `maxAttempts`, after which it is
`InvalidResponse`.

## Transport

HTTP/1.1 over libcurl, with almost every convenience turned off:

| Setting | Why |
| --- | --- |
| `CURLOPT_FOLLOWLOCATION` off | A chain the library follows silently is a chain this repository cannot bound and the hostile corpus cannot test |
| Raw response status | ADR-0002 makes a `200` answering a `Range` request `RangeNotSupported`, and a client that normalizes a partial response into "here are your bytes" cannot implement that |
| `Accept-Encoding: identity` | A compressed range response would make the byte accounting describe the wire rather than the asset |
| Bounded write callback | The caller's buffer is the bound. A server answering a 64 KiB range request with a 10 GB body moves 64 KiB and is then cut off |
| `CURLOPT_NOSIGNAL` | libcurl's alarm-based DNS timeout is not safe to use from a thread |

No libcurl error string ever reaches a `Status::message`. They embed the
effective URL, and a message built from one would undo the credential elision
this repository already ships.

## Range support behavior

Range support is a capability, discovered and then re-checked, never assumed.

| Server | Detected | Result |
| --- | --- | --- |
| Advertises `Accept-Ranges: bytes`, honors it | At open | Reader is returned |
| No `Accept-Ranges`, or `none` | At open | `RangeNotSupported`, no reader |
| Advertises byte ranges, then ignores `Range` | At the first read | `RangeNotSupported` |
| `206` whose `Content-Range` does not cover the request | At that read | `InvalidResponse` |

The third row cannot be caught at open. Doing so would take a second round trip,
and ASSET_READER.md §2 admits only one; the corpus's `IgnoresRange` row exists
precisely because a backend that trusted `HEAD` has already committed.
[ADR-0002](../../docs/adr/0002-range-unsupported-policy.md) makes both
`RangeNotSupported` cases terminal in `v0.2.0`, so where it is detected changes
the message and not the outcome.

**A `200` answering a range request is two different servers.** One ignored the
`Range`; the other honored a stale `If-Range` and correctly returned the whole
new representation. They are separated by the response itself — a server that
merely ignored the range is still serving the revision this reader bound to, so
its validator and its length still match. Branching on "did this reader send
`If-Range`" instead would report every range-ignoring server as a changed asset.

## Revision binding

| Moment | Obligation |
| --- | --- |
| Open | Capture a validator, classify its strength, derive the conditional guard |
| Every range request | Send `If-Range`, where the captured validator admits one |
| Every range response | Compare its validator, and its complete length, against what was captured |
| A refused range (`416`) | The same comparison, against `bytes */<complete-length>` |
| On any mismatch | `AssetChanged`, `bytesRead == 0`, and never a rebind |

The `416` row is easy to miss and is the one status where the obvious reading is
backwards. A range this reader already sized cannot lie outside the asset, so a
server refusing it is evidence that the representation is no longer the one that
size came from — and a `416` is required to carry `bytes */<complete-length>`,
which says so outright. Reporting it as a malformed response attaches a message
that is factually false.

Two independent detectors, because neither covers the other. `If-Range` makes
the *server* refuse, which is the cheaper and more atomic answer; the response
comparison catches a revision whose bytes are unchanged and whose identity
moved, and is the whole guard for an asset whose validator may not be sent
conditionally.

`bytesRead` is zero on `AssetChanged` rather than what arrived: those bytes
belong to a revision this reader is not bound to, and reporting them as read
invites exactly the composition the guarantee exists to prevent.

| Validator captured | `stability` | `If-Range` sent |
| --- | --- | --- |
| Strong `ETag` | `Stable` | yes, verbatim |
| Weak `ETag` | `Unstable` | no — RFC 9110 §13.1.5 admits only a strong validator |
| `Last-Modified` | `Unstable` | yes, under its own rule; one-second granularity |
| None | `Unavailable` | no |

A weak validator is still captured. It cannot gate a conditional request, but a
response whose weak validator has changed is still positive evidence of
`AssetChanged`.

## Failure mapping

| Condition | `StatusCode` |
| --- | --- |
| `404`, `410` | `NotFound` |
| `401`, `403` | `AccessDenied` |
| No `Accept-Ranges` at open; `200` answering a `Range` | `RangeNotSupported` |
| `Content-Range` that does not cover the request | `InvalidResponse` |
| Missing or unparseable `Content-Length` at open | `InvalidResponse` |
| Body shorter than the framing declared, after the retry budget | `InvalidResponse` |
| Conflicting duplicate `Content-Length` or `Content-Range` lines | `InvalidResponse` |
| `416` for a range inside the size reported at open, with nothing contradicted | `InvalidResponse` |
| `416` whose `bytes */<length>` or validator contradicts the capture | `AssetChanged` |
| Redirect chain past `maxRedirects`; no `Location`; unusable `Location` | `InvalidResponse` |
| `https` → `http` redirect | `InvalidResponse` |
| Response that is not HTTP | `InvalidResponse` |
| DNS, refusal, TLS handshake, reset connection | `NetworkError` |
| `5xx` or `429` after the retry budget | `NetworkError`, with the status attached |
| Connect, response, or transfer deadline | `Timeout`, naming which |
| Validator or complete length contradicting the capture | `AssetChanged` |
| `405`, `501` to the metadata request | `Unsupported` |
| Overflowing `offset + size`; null buffer with non-zero length | `InvalidArgument` |

A `5xx` maps to `NetworkError` because the vocabulary has no server-is-unwell
code and does not need one: what a caller does about a spent `503` is what it
does about a reset connection. The status itself travels in
`Status::transportStatus`, which is diagnostic sugar no caller branches on.

A scheme downgrade also lands on `InvalidResponse` — it is a response this
backend refuses to act on. It is called out here because it is a security
refusal rather than a framing fault, and a future code for it would be a
`DIAGNOSTICS.md` change first.

## Retry, timeout, and redirect policy

| Policy | Value | Rule |
| --- | --- | --- |
| Redirects | `maxRedirects`, default 5 | `301`, `302`, `307`, `308`. `303` is not followed: it means "fetch a different resource", which would substitute one asset for another |
| Retry | `maxAttempts`, default 3 | `429`, `502`, `503`, `504`, and connection-level failures where nothing came back |
| Not retried | — | Any deadline; `500`; any response whose headers arrived and whose status is final |
| Resumed | — | A body that stopped early, from where it stopped — not re-fetched whole |
| Total work | bounded | `maxRedirects + maxAttempts` requests, each with its own deadline |

The retry budget is **per logical operation and shared**, not per loop. One
`Read` allocates `maxAttempts - 1` retries once, and both the transport-level
retry and the resume of a short body draw from that one pool; a resume is a
retry however it is spelled. Two nested loops each bounded by `maxAttempts`
would be jointly bounded by its square, which is not what the option means and
is nine requests where the caller asked for three. Redirect hops draw from
`maxRedirects` instead, because a hop is not a retry: a chain longer than the
retry budget must still be followed to its end.

Deadlines are separable because `HTTP006` is required to name which one elapsed:
a caller staring at "timeout" cannot tell a firewall from a slow origin. A
timeout is never retried — the caller has already said how long it is prepared
to wait, and asking again spends that budget a second time.

## Metrics populated

`requestCount`, `metadataRequestCount`, `retryCount`, `redirectCount`,
`bytesRequested`, `bytesTransferred`, `assetSize`, `openLatency`,
`requestLatency`, `readLatency`.

`bytesFromCache` and every counter in METRICS.md §2.2 stay at zero: this module
has no cache, and `v0.3.0` is what populates them.

Counters are never retracted by a failure. A conditional request the server
refused still cost a round trip; a read that then fails with `AssetChanged` does
not take it back.

## Error and diagnostic behavior

Every failure is typed, carries the byte range it applies to where one is known,
and quotes an identifier that has been through `ElideSecrets`. No message
contains an `Authorization` value, a userinfo component, or a signed-URL query
string, and the corpus test asserts that rather than assuming it.

## Threading and ownership

- **`Read` may be called concurrently on one reader, by any number of threads.**
  No caller's bytes are interleaved into another caller's buffer. Each read is
  an independent request and shares no mutable state with another.
- **`Metadata()` may be called concurrently with anything.** It is fixed at open
  and immutable for the reader's lifetime.
- **Two readers on one asset share nothing.** Each owns its transport.
- `dst` is caller-owned. The reader writes only within `[dst, dst + size)`,
  whatever the transport delivered, and retains no reference to it after
  returning. Bytes inside that range but beyond `bytesRead` are unspecified on
  failure.
- Opening is not required to happen on the thread that reads.

Internally, a reader owns a small pool of libcurl easy handles, one checked out
per in-flight request. [ADR-0003](../../docs/adr/0003-http-client-dependency.md)
records "one easy handle per reader"; that is one handle short of what the
boundary suite requires, because an easy handle may not be used by two threads
at once and the suite runs many threads on *one* reader. A single handle would
be either a data race or a mutex serializing every concurrent read of an asset.
The pool is still per reader, is never shared between readers, and still reuses
connections, because a checked-in handle keeps its connection for the next
request that checks it out.

## Build and test

```sh
cmake --preset core            # no OpenUSD; libcurl via find_package
cmake --build --preset core
ctest --preset core -R usdAssetHttp
ctest --preset core -R boundary_http
```

libcurl is acquired the way ADR-0003 names — a `find_package`, with CI
installing per platform:

```text
windows   vcpkg install curl:x64-windows-static-md
linux     apt-get install libcurl4-openssl-dev
macos     brew install curl
```

On Windows, configure through vcpkg's toolchain file rather than by pointing
`CMAKE_PREFIX_PATH` at the installed tree:

```sh
cmake --preset core-msvc \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
```

The difference is not cosmetic. vcpkg's `CURLConfig.cmake` calls
`find_dependency(ZLIB)`, and a bare prefix leaves CMake's own `FindZLIB` to
locate a *static* vcpkg zlib by guessing library names — it finds the header,
does not find the library, and the configure fails with "Could NOT find ZLIB
(missing: `ZLIB_LIBRARY`)" while reporting a version it read out of `zlib.h`.
The toolchain activates vcpkg's own wrapper, which supplies the release and
debug paths explicitly. Naming the triplet matters too: the default is the
dynamic one.

Three suites, and they are not interchangeable:

| Suite | Where | Asserts |
| --- | --- | --- |
| Module tests | `tests/` | URI arithmetic, framing as a pure function of headers, and the protocol policies over a scripted transport |
| Corpus projection | `tests/corpus/` | Which hostile server behavior produces which `StatusCode`, against a real server |
| Boundary suite | `tests/boundary/` | The read contract, byte-equivalent to the local backend over an independent oracle |

The scripted transport in the module tests is deliberately never used for
anything the corpus can produce. A mock that truncates a body on request would
be this module asserting its own fiction; `tests/corpus` uses a server that
really truncates one, and the corpus proved over a raw socket that it does.

## Third-party dependencies and licenses

| Dependency | License | Linkage |
| --- | --- | --- |
| libcurl | curl license (MIT/X11 derivative) | Static, private |

No redistribution constraint applies to a plugin binary. Recorded in
[NOTICE](../../NOTICE) and argued in
[ADR-0003](../../docs/adr/0003-http-client-dependency.md).

## Known limitations

- **No fallback when a server refuses `HEAD`.** §4.1 of the design policy admits
  "a minimal metadata request where `HEAD` is unavailable"; this module does not
  implement one, and reports `Unsupported` instead. The hostile corpus has no
  row that refuses `HEAD`, so the fallback would ship unexercised. Named rather
  than approximated.
- **`If-Range` with a `Last-Modified` validator is not covered by the corpus.**
  The fixture server compares `If-Range` only against its `ETag`, so an asset
  with a date and no entity tag cannot exercise the conditional path there. The
  behavior is implemented per RFC 9110 and unit-tested; what is missing is the
  server-side half. A corpus fixture with a date and no `ETag` would fail for
  the fixture's reason, not this module's.
- **Nothing exposes identity upward yet.** `stability` is captured and
  classified; the resolver surface that hands it to a consumer is `v0.4.0`, and
  deliberately waits until capture has been correct for a release.
- **No cancellation.** The read contract carries no cancellation token, and this
  transport has no out-of-band way to be told. The boundary suite is told so by
  the row rather than discovering it from a test that skips itself.
- **Multipart range responses are not accepted.** Out of `v0.x` scope; a request
  that would need one is split.
- **A `206` covering less than was asked for is refused, which is stricter than
  RFC 9110.** An origin may answer a single-range request with a prefix of it,
  and the resume loop could accept that and ask for the rest. It does not,
  because §10 of the design policy says to validate that a `Content-Range`
  covers the requested range before copying it, DIAGNOSTICS.md §6 uses precisely
  this response as its `InvalidResponse` example, and the corpus keeps
  `ContentRangeTooShort` as a hostile row on that basis. Relaxing it is a change
  to those documents first, for every backend. The cost, named rather than
  hidden: an origin that caps the size of a range response is refused instead of
  being read in pieces, and nothing in the corpus behaves that way, so nothing
  here would notice if the rule were wrong.
- **`RST` against `FIN` is distinguished only where the platform distinguishes
  it.** A destroyed connection is `NetworkError` and a clean close below the
  declared length is `InvalidResponse`, but which of the two a peer observes is
  the operating system's decision. The corpus asserts the portable half: the
  promise in the headers was not kept, and no complete buffer is handed back.

## Planned work

- `v0.3.0`: nothing in this module. The block cache decorates it from outside,
  keyed on the validator captured here.
- `v0.4.0`: no change expected. Identity exposure is the resolver's surface.
- `v0.6.0`: `HttpOptions` resolved from the environment and from
  `ArResolverContext`, and the request interception point authentication will
  attach to.

## Contracts implemented

- [ASSET_READER.md](../../docs/architecture/ASSET_READER.md) — read semantics,
  revision binding, validator semantics
- [DIAGNOSTICS.md](../../docs/architecture/DIAGNOSTICS.md) — the typed vocabulary
- [METRICS.md](../../docs/architecture/METRICS.md) — the counters
- [WORKSPACE.md](../../docs/architecture/WORKSPACE.md) — the dependency boundary
- [ADR-0002](../../docs/adr/0002-range-unsupported-policy.md) — range-unsupported
  policy
- [ADR-0003](../../docs/adr/0003-http-client-dependency.md) — the client
  dependency and the seam
