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
prerequisites in §17 of the [design policy](docs/design/DESIGN_POLICY.md) are
now done, in the order it fixed. Neither ships in the release.

### Added

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

- `NOTICE` names libcurl and its license, and no longer describes the client as
  an open question. No third-party code is bundled or linked yet; the license
  text arrives with the first `libs/usd-asset-http` commit.
- The HTTP client is no longer a blocking item in
  [implementation status](docs/roadmap/implementation-status.md), and neither is
  the fixture server. What remains in phase 2 is the backend itself.
- The [workspace contract](docs/architecture/WORKSPACE.md) records the fixture
  server's dependency direction, which is that it has none: not `usdAssetIo`,
  not a backend, and not the HTTP client. It links the platform's sockets and
  the standard library. Not knowing `usdAssetIo` is the load-bearing half — a
  corpus that could name `StatusCode` would begin asserting the backend's
  interpretation, and a disagreement between the two would stop being evidence.

### Fixed

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

- **Nothing consumes the corpus.** It is a passing oracle with no subject until
  `libs/usd-asset-http` lands, and
  [the capability matrix](docs/reference/CAPABILITY_MATRIX.md) says so rather
  than letting 18 green behaviors read as HTTP support.
- **The scheme-downgrade case is not in the corpus and cannot be.** §10 of the
  design policy requires rejecting an `https` to `http` redirect; the fixture
  server speaks plaintext HTTP, so there is no `https` to downgrade from.
  Faking it with a `Location` a test declares was reached over TLS would assert
  nothing, so the case is left out and named as absent. It is redirect policy
  rather than server behavior, and belongs in `usdAssetHttp`'s own tests.
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
