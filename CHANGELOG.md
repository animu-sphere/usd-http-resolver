# Changelog

Unreleased work on `main`. Tagged versions get an immutable record under
[docs/releases/](docs/releases/README.md); this file is what has landed since
the last one.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
this project follows [semantic versioning](https://semver.org/) with the
diagnostic codes in
[DIAGNOSTICS.md](docs/architecture/DIAGNOSTICS.md) treated as a compatibility
surface: adding a code is a minor change, changing what one means is a breaking
one.

## Unreleased

Work toward `v0.2.0`, whose scope is in the [roadmap](docs/roadmap/README.md).
It started with the HTTP client dependency decision rather than with code, and
then with the server the code will be tested against rather than the code. Both
prerequisites in §17 of the [design policy](docs/design/DESIGN_POLICY.md) were
done first, in the order it fixed, and neither ships in the release. The backend
they existed for has now landed.

### Added

- `libs/usd-asset-http`, the HTTP backend, and the first thing in this
  repository that touches a network. It serves byte ranges over `http` and
  `https`, and it is admitted by the `v0.1.0` boundary suite **unchanged** —
  243 fixed cases, 10,000 generated cases, and the concurrency cases, all
  byte-equivalent to the local backend over an independent oracle. Entering it
  was one row (`tests/boundary/backends/boundary_http_main.cpp`) and one line of
  CMake, which was the claim `v0.1.0` made and had not yet been asked to cash.
- **Revision binding, from the backend's first commit rather than after it.** A
  validator is captured at open and classified; a conditional guard rides every
  subsequent range request where the captured validator admits one; and a
  response that contradicts the capture fails the read with `AssetChanged` and
  `bytesRead == 0`. There are two detectors because neither covers the other:
  `If-Range` makes the server refuse, which is cheaper and more atomic, and the
  response comparison catches a revision whose bytes are identical and whose
  identity moved — the case a backend comparing bytes cannot see, and the whole
  guard for an asset whose validator may not be sent conditionally. A weak
  `ETag` is captured and never sent, per RFC 9110 §13.1.5.
- Response framing validated **against the request**, not against itself. A
  `206` whose `Content-Range` is internally consistent and describes a different
  window than the one asked for is `InvalidResponse` before a byte is accepted;
  §10 of the design policy requires exactly this, and DIAGNOSTICS.md §6 uses it
  as its example message.
- The write is bounded by the caller's buffer, so a server answering a 64 KiB
  range request with a 10 GB body moves 64 KiB and is then cut off. That is the
  transfer this project exists to avoid, arriving as a hostile case rather than
  as an accident.
- Bounded redirects with an `https` → `http` refusal, bounded retry that is
  never spent on a deadline, a short transfer resumed from where it stopped
  rather than re-fetched whole, and three separable deadlines — connect,
  response, and transfer — so that `Timeout` can name which one elapsed, which
  DIAGNOSTICS.md requires of `HTTP006`.
- The narrow internal transport seam ADR-0003 called for. libcurl is named in
  exactly one translation unit, `src/CurlTransport.cpp`, appears in no installed
  header, and is linked `PRIVATE` and statically. Everything above the seam —
  framing, redirects, retry, validator handling, the whole projection onto the
  typed vocabulary — is exercised offline against a scripted transport, and the
  seam is what makes a future `usdAssetWasm` a second implementation of one
  interface rather than a second backend.
- `tests/corpus`, the projection of each corpus behavior onto the typed
  vocabulary — which `Behavior` produces which `StatusCode`. All 18 behaviors
  are covered and the coverage is asserted against `AllBehaviors()` at runtime,
  so adding a behavior without adding a projection fails the run. Neither side
  knows the other: nothing in `tests/fixture-server` has heard of `StatusCode`
  and nothing in `usdAssetHttp` has heard of `Behavior`, which is what makes a
  disagreement between them evidence rather than a tautology. It also asserts
  the negative this project cares most about — that a URL carrying userinfo and
  a query token appears in no rendered message.
- Sanitizer coverage over the whole HTTP path. ASan, UBSan, and TSan are green
  across all 17 tests, including the boundary row and the corpus projection.
  TSan is the one that matters here: `v0.2.0` is the first release with many
  threads reading one asset over sockets, and asserting that in prose asserts
  nothing.

- `tests/fixture-server`, the hostile-server corpus, standing up **before** the
  HTTP backend rather than beside it. A loopback HTTP/1.1 origin on an ephemeral
  port, with 18 named behaviors covering all nine conditions §11.2 of the design
  policy requires — no `Accept-Ranges`, a `200` answering a `Range`, a truncated
  body, a wrong `Content-Range`, a mid-read `ETag` change, a redirect chain, a
  slow response, a connection reset, and a `416` — plus three more from
  constraints fixed elsewhere: an unknown `Content-Length` (ADR-0003), a
  transient `503` for bounded retry (§4.1), and `403` against `404`
  ([DIAGNOSTICS.md](docs/architecture/DIAGNOSTICS.md) §4.4). Behaviors are
  enumerable at runtime and the self-test fails when one has no case, so the
  coverage claim is checkable rather than asserted.
- A self-test for the corpus, which is the part that makes it an oracle rather
  than a second unknown. It asserts over a raw socket that each behavior puts on
  the wire exactly what its name claims, with a client that shares the socket
  layer with the server and no HTTP code at all — the same separation the
  boundary suite keeps between its oracle and `usdAssetLocal`. It was checked
  against deliberately broken servers before it was trusted: seven mutations
  each produced a named failure, and the eighth is recorded as an equivalent
  mutant rather than counted as coverage.
- A request log on the fixture server — method, target, `Range`, `If-Range`, and
  the answered status. Bounded redirects, "no request was issued for a
  zero-length read", and "`If-Range` on every range request after open" are
  properties of what was *sent*, and there is no other way to assert them.
- [ADR-0003](docs/adr/0003-http-client-dependency.md): the HTTP client
  dependency is **libcurl**, acquired through a private `find_package(CURL)` in
  `libs/usd-asset-http` and reached only through a narrow internal transport
  seam, so no libcurl type appears in a header and the choice stays cheap to
  supersede. It is chosen for exact control rather than for convenience:
  bounded redirects need `CURLOPT_FOLLOWLOCATION` off, ADR-0002's rule that a
  `200` answering a `Range` request is `RangeNotSupported` needs the raw status
  of a response whose body is valid, and framing validation needs
  `Content-Range` before anything has interpreted it. Vendoring and pinned
  `FetchContent` were considered and rejected, with the trigger for revisiting
  each recorded.
- The Wasm criterion in §13 of the [design policy](docs/design/DESIGN_POLICY.md)
  is answered rather than deferred. libcurl does not build for Wasm; the
  reserved `usdAssetWasm` backend over `fetch` is the answer, which the
  workspace contract had already architected as a sibling backend on the
  unchanged `AssetReader` contract. The ADR records this as an accepted cost
  and states what would falsify the reasoning.

### Changed

- `NOTICE` carries the curl copyright and license text, and no longer says this
  release links no third-party library. It does now, from one module.
- `.github/workflows/core-ci.yml` installs libcurl per platform, the way
  ADR-0003 names. The lane's contract is unchanged and still asserted from the
  configure log: it needs no *OpenUSD runtime*, and never claimed to need no
  dependencies.
- The HTTP client is no longer a blocking item in
  [implementation status](docs/roadmap/implementation-status.md), and neither is
  the fixture server or the backend. What remains in phase 2 is
  `plugins/http-resolver`, `openstrata.ci.yaml`, and the recorded baseline.
- [WORKSPACE.md](docs/architecture/WORKSPACE.md) records a second legal reverse
  edge. There was one — the boundary row reaching the fixture server to
  provision remote fixtures — and `tests/corpus` is the other. Both live outside
  `libs/` rather than in the backend's own tests, and that placement is
  load-bearing: a module's tests must not depend on anything outside `libs/`, or
  `ost library build libs/usd-asset-http` stops working.
- The [workspace contract](docs/architecture/WORKSPACE.md) records the fixture
  server's dependency direction, which is that it has none: not `usdAssetIo`,
  not a backend, and not the HTTP client. It links the platform's sockets and
  the standard library. Not knowing `usdAssetIo` is the load-bearing half — a
  corpus that could name `StatusCode` would begin asserting the backend's
  interpretation, and a disagreement between the two would stop being evidence.

### Fixed

Two defects in the HTTP backend, both caught by suites that already passed
before it existed — which is the return on having built them first.

- **A deadline that elapsed mid-body was resumed as though it were a short
  read.** The read loop treats a transfer that stopped early as resumable and
  asks for the remainder, which is right for a connection that ended and wrong
  for a clock that ran out: the caller has already said how long it is prepared
  to wait, and asking again spends that budget a second time. The failure then
  surfaced as `InvalidResponse` after the retries, naming the server for the
  caller's own deadline. Deadlines are now non-resumable and reported as
  `Timeout` immediately, with the bytes that did arrive. Found by the corpus's
  `SlowBody` row, which exists precisely because a backend with only a total
  deadline cannot tell it from `SlowHeaders`.
- **Every deadline on a reused connection was reported as a connect timeout.**
  `CURLINFO_CONNECT_TIME_T` measures a connection *this transfer established*
  and is zero when the transfer reused one from the pool — which, once
  connection reuse works, is most of them. Reading it as "was there a
  connection" named the one deadline that provably had not elapsed. It now reads
  pre-transfer time, which is non-zero however the connection was obtained.
  Found by the same row, and only after the first fix stopped masking it.

Six more found by review of the branch before it merged, four of them in
handling that only a hostile or unlucky server reaches:

- **`RemoveDotSegments` collapsed empty path segments.** `/a//b` became `/a/b`,
  and RFC 3986 §5.2.4 removes `.` and `..` and nothing else. It looks like
  tidying and is a rename: an object-storage key may legitimately contain an
  empty segment, so the collapsed form names a different object, and a
  pre-signed URL whose signature covers the canonical path stops verifying.
  Every path went through it on the way in.
- **A `416` never asked whether the asset had simply changed.** A range this
  reader already sized cannot lie outside the asset, so a server refusing it is
  evidence the representation moved — and a `416` is required to carry
  `bytes */<complete-length>`, which says so outright. It reported
  `InvalidResponse` with a message that was factually false. It was the one
  status with no coverage for the release's central guarantee.
- **The retry budget was nested rather than shared.** `maxAttempts` bounded the
  read's resume loop and the transport's retry loop independently, so one
  `Read` could cost `maxAttempts²` requests against a documented per-operation
  cap of `maxAttempts` — nine where the caller asked for three. One budget is
  now allocated per logical operation and both loops draw from it; redirect hops
  still draw from `maxRedirects`, because a hop is not a retry.
- **The response deadline was charged for the connect.** It was measured from
  the start of the exchange rather than from when the request went out, so it
  could fire up to `connectTimeoutMs` early — and name the wrong deadline, which
  is the one thing `HTTP006` is required to get right.
- **`curl_slist_append`'s return was unchecked.** It returns null on allocation
  failure and does not free what it was given, so the obvious idiom both leaks
  the list and drops every header. A dropped `Range` does not fail: it succeeds,
  as a `200` carrying the whole representation, which this backend would then
  correctly report as `RangeNotSupported` — a transient allocation failure
  wearing the name of a terminal server capability.
- **Conflicting duplicate `Content-Length` lines were accepted.** Last-wins,
  where RFC 9110 §8.6 requires the message to be rejected; an intermediary that
  believes the first and an origin that believes the last disagree about where
  one message ends and the next begins. Repeated *identical* values are still
  accepted, because that is redundant rather than hostile.

One defect in the metrics accounting, found by an assertion on counters rather
than on behavior:

- **Metadata requests were counted twice.** `AddMetadataRequest` already bumps
  `requestCount` — a metadata request is a request — and the backend called
  both. Nothing observable broke, which is the point: it would have inflated the
  denominator of `requestEfficiency` and understated the amplification this
  project claims to be measured by, in the release that first has a number to
  report.

Eight defects in the fixture server, found by reviewing it before the backend
started depending on it rather than after. None of them ships; all of them would
have been debugged as backend bugs.

- **`Stop()` hung on a client that stopped reading.** "Shutdown never hangs" was
  contract in `Server.h` and in the README and was not true: the condition
  variable `Stop()` signals reaches the deliberate stalls and does not reach a
  handler sitting in `send`. Accepted sockets are now non-blocking and every
  write is abandoned when the stop flag is set. The regression case hangs
  without the fix on Linux; Winsock buffers a whole 64 MiB response without
  blocking, so it proves nothing there and says so.
- **Connection threads were joined only at `Stop()`**, one unreaped stack per
  request. The accept loop now reaps as they finish, and `OpenConnections()`
  exists so the self-test can say it does.
- **`ContentRangeShifted` served a correct response** for any range covering the
  whole asset — `bytes=0-255`, `bytes=0-`, and `bytes=-256` alike. The window
  now moves up one and clamps at EOF.
- **`ContentRangeTooShort` was a no-op for a single-byte range.** It still is,
  because `Content-Range` cannot describe an empty range; the difference is that
  the floor is now stated beside the enumerator and pinned by a case, instead of
  being discovered by a backend test that leaned on it.
- **The `.hopN` redirect alias routed on half a match.** `/chain.hop` redirected
  to `/chain.hop.hop1` and then `404`ed, and `/normal.hop` served `/normal`'s
  body under a name nobody registered while counting the request against it. The
  suffix must now parse as digits and name a `RedirectChain` asset.
- **The accept loop spun at 100% of a core** on a persistent accept failure —
  reachable through the thread leak above. It backs off at the poll cadence.
- **A request was logged with a status that never reached the wire.** `Server.h`
  gives `0` the meaning "deliberately never answered"; the log is now written
  after the send, and records `0` when the send failed.
- **A reset-mid-body assertion was really an assertion about the runner.** What
  an `RST` destroys is measured, not assumed: Linux delivers the eight buffered
  body bytes and Winsock discards them, and the headers survive on both only
  because the reader gets to them first.

### Known gaps

- **No consumer can open a remote asset yet.** The backend works and nothing
  above it reaches it: there is no `ArResolver` registration, no `ArAsset`, and
  no `HTTPxxx` code is emitted. `plugins/http-resolver` is what remains of the
  release, and [the capability matrix](docs/reference/CAPABILITY_MATRIX.md) says
  so rather than letting a green transport read as a working resolver.
- **No I/O baseline is recorded.** `v0.2.0` is the first release that can record
  one, and it has not. The counters are populated and asserted; what is missing
  is a fixture large enough for `selectivity` to mean anything — the loopback
  corpus assets are kilobytes, and a ratio measured against them would be a
  number without a meaning. It belongs in the release record.
- **There is no fallback when a server refuses `HEAD`.** §4.1 of the design
  policy admits "a minimal metadata request where `HEAD` is unavailable"; a
  `405` or `501` is reported as `Unsupported` instead. The hostile corpus has no
  row that refuses `HEAD`, so a fallback would ship unexercised, and this
  repository's rule is to name a gap rather than fill it speculatively.
- **`If-Range` with a `Last-Modified` validator is implemented and not covered
  by the corpus.** The fixture server compares `If-Range` only against its
  `ETag`, so an asset with a date and no entity tag cannot exercise the
  conditional path there — a fixture shaped that way would fail for the
  fixture's reason rather than the backend's. The behavior is unit-tested
  against a scripted transport; the server-side half is what is missing.
- **A `206` covering less than was asked for is refused, which is stricter than
  RFC 9110 requires.** An origin may answer a single-range request with a prefix
  of it, and the resume loop could accept that and ask for the rest; §10 of the
  design policy, DIAGNOSTICS.md §6, and the corpus's `ContentRangeTooShort` row
  all say not to. Relaxing it is a change to those documents first, for every
  backend, and not a quiet accommodation in one. The cost: an origin that caps
  the size of a range response is refused rather than read in pieces, and
  nothing in the corpus behaves that way, so nothing would notice if the rule
  were wrong.
- **The sanitizer lanes do not instrument libcurl.** It is the runner's own
  package, so the evidence is about this repository's code and not about the
  client behind the seam. That is the intended scope — what needed proving is
  that many threads on one reader do not race, that the offset arithmetic does
  not overflow, and that nothing writes past a caller's buffer, all of which
  live above the seam.
- **The scheme-downgrade case is not in the corpus and cannot be.** §10 of the
  design policy requires rejecting an `https` to `http` redirect; the fixture
  server speaks plaintext HTTP, so there is no `https` to downgrade from.
  Faking it with a `Location` a test declares was reached over TLS would assert
  nothing, so the case is left out and named as absent. It is redirect policy
  rather than server behavior, and it is now tested where the previous release
  said it belonged — in `usdAssetHttp`'s own tests, against a scripted
  `Location`.
- **`RST` against `FIN` is asserted only where it is portable.** The reset
  behaviors close with `SO_LINGER{1, 0}`, which is a real reset; whether a peer
  observes `ECONNRESET` or an orderly EOF is the platform's decision, and so is
  how much of what was already sent survives it. The corpus asserts the fact a
  backend must handle — the promise in the headers was not kept — and not the
  errno, the byte count, or even the arrival of the headers, none of which the
  runner promises.

## `v0.1.0` — 2026-08-16

The release that decides whether the rest of the project is buildable. It ships
no network code, and its centre of gravity is the test suite rather than the
reader. The immutable record, including the release gate and what it found, is
[docs/releases/v0.1.0.md](docs/releases/v0.1.0.md).

### Added

- `libs/usd-asset-io`, the transport-independent core: the `AssetReader`
  random-access contract and `AssetMetadata`; the validator value types and the
  `Stable` / `Unstable` / `Unavailable` classification derived from them; the
  typed `StatusCode` vocabulary; the shared offset arithmetic in
  `ResolveReadRange`, so the EOF boundary and the overflow check exist once
  rather than once per backend; and the metrics counters, latency histograms,
  and process aggregate.
- Credential elision in every message and metrics dump, covering the query
  string **and** the userinfo component of an authority — recognized only
  where an authority can legally appear, so a filesystem path with a `//` in it
  is left alone.
- `libs/usd-asset-local`, the local-file backend and the correctness oracle:
  positional reads with no lock, size and range-support discovery at open, a
  filesystem-derived validator, and `AssetChanged` when an asset is republished
  underneath an open reader.
- `tests/boundary`, the shared boundary suite: the required boundary cases per
  fixture size, the short read below EOF, the mid-read revision change, biased
  property cases with shrinking and a reported seed, and the concurrency cases —
  all against an independent naive oracle that shares no code with the backend
  it checks. Entering a backend is a row, and the local backend's is
  `tests/boundary/backends/boundary_local_main.cpp`.
- Sanitizer build configuration: `USD_HTTP_RESOLVER_SANITIZER`, and the
  `core-asan` and `core-tsan` CMake presets.
- `.github/workflows/core-ci.yml`, the runtime-free CI lanes: the core build and
  test on Windows, Linux, and macOS arm64 with no OpenUSD present — asserted
  from the configure log, not inferred from a green build — and `core-asan` and
  `core-tsan` on Linux. Hand-authored rather than generated from
  `openstrata.ci.yaml`, because every `ost` cell pins and materializes an
  OpenUSD runtime and no cell can name a workspace that contains no bundle;
  the account and the upstream asks are in
  [report 01](docs/reports/ost/01-2026-08-16-v0.1.0-ci-without-a-support-matrix.md).
- `core-msvc`, the `core` build through the Visual Studio generator, so the
  Windows lane works outside a developer command prompt — in CI and on a
  contributor's machine.
- `VERSION`, `LICENSE`, `NOTICE`, and OpenStrata plain-library descriptors for
  both `libs/` modules.

### Fixed

- The UndefinedBehaviorSanitizer lane reported nothing it found.
  `-fno-sanitize-recover=all` now accompanies the sanitizer flags: UBSan's
  default is to print a violation and continue, so a signed overflow in the
  offset arithmetic exited `0` and CTest reported a pass. Measured both ways in
  [report 01](docs/reports/ost/01-2026-08-16-v0.1.0-ci-without-a-support-matrix.md)
  §4.

### Known gaps

- The MSVC AddressSanitizer lane is unverified and the build says so. Sanitizer
  evidence is a clang or GCC lane, and the CI cells are Linux for that reason.
- No I/O baseline is recorded. `v0.1.0` moves no bytes over a network, so there
  is nothing yet for the ratio this project claims to be measured against.
- `openstrata.ci.yaml` does not exist. It arrives in `v0.2.0` with the first
  bundle a cell can name; the runtime-free lanes stay outside it.
