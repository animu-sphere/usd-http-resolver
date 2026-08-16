# ADR-0003: The HTTP client dependency

## Status

Accepted, 2026-08-16, for `v0.2.0`: **libcurl**, acquired through a private
`find_package(CURL)` in `libs/usd-asset-http` and installed by CI per platform.
No libcurl type appears in a `usdAssetHttp` header.

The Wasm criterion in §13 of the [design policy](../design/DESIGN_POLICY.md) is
satisfied at the contract level rather than by the dependency: libcurl does not
build for Wasm, and the reserved `usdAssetWasm` backend in §1 of the
[workspace contract](../architecture/WORKSPACE.md) is the answer. That is a
real cost, and §"The Wasm criterion, answered honestly" below states it as one
rather than as a technicality.

## Context

§13 of the design policy names this as the one real third-party decision in the
repository and fixes the criteria before features are discussed:

> A dependency whose license constrains redistribution of a plugin binary is
> disqualified; a dependency that cannot build for the Wasm target is a
> strategic liability.

§17 makes it the first action of phase 2, ahead of code, and
[implementation status](../roadmap/implementation-status.md) records it as
blocking item 1. Nothing in `v0.1.0` touches a network, so the decision could
wait; nothing in `v0.2.0` can start without it.

The choice is narrower than "pick an HTTP library" because the tree has already
fixed what the client must expose. Four constraints come from documents that
are already accepted:

- **Raw status and header access.** [ADR-0002](0002-range-unsupported-policy.md)
  rules that a `200` in answer to a `Range` request is `RangeNotSupported`
  (`HTTP003`), not `InvalidResponse`. Framing validation must additionally
  check that `Content-Range` covers what was asked for, and refuse an unknown
  `Content-Length`. A client that helpfully normalizes a partial response into
  "here are your bytes" cannot implement either rule.
- **Bounded, not automatic, redirects.** §11.2 of the design policy puts a
  redirect chain in the hostile corpus. Bounding a chain requires the client to
  let redirects be refused and counted, not merely followed.
- **Concurrency the boundary suite already exercises.** `tests/boundary` runs
  many threads on one reader and many readers on one asset, under
  ThreadSanitizer. The client's threading model has to survive that as-is,
  because the suite passes against the HTTP backend *unchanged* or the release
  does not ship.
- **Connection reuse.** `v0.3.0` records an amplification ratio. A client that
  reconnects per range request makes that measurement about TCP setup rather
  than about the cache.

Two further facts constrain acquisition. The core CI lane builds every `libs/`
module on Windows, Linux, and macOS arm64 with no OpenUSD present, so whatever
is chosen must install cleanly on all three runners. And `usdAssetHttp` is the
only module permitted to name a client at all, per §2 of the workspace
contract — the dependency is private, and the seam is not optional.

## Options

### A. libcurl

The de-facto standard C HTTP client, under the curl license — an MIT/X11
derivative with no redistribution constraint on a plugin binary.

- Exact control over every constraint above. `CURLOPT_FOLLOWLOCATION` stays
  off, so redirects are read from `Location` and bounded by this repository's
  own counter rather than by the library's. Response status, `Content-Range`,
  `Accept-Ranges`, and `Content-Length` are all available raw.
- Timeouts are separable — connect, transfer, and low-speed — which is what
  `Timeout` (`HTTP006`) needs in order to name which deadline elapsed.
- The threading model is well-defined and matches the reader: one easy handle
  per reader, with a share handle carrying the connection pool. This is a model
  with a decade of TSan exposure behind it rather than one being invented here.
- TLS is the platform's: Schannel on Windows, and OpenSSL where it is already
  present. No certificate store is vendored.
- Costs: a large API surface, of which this project uses a narrow slice; and
  acquisition on Windows is genuine work rather than a header drop.
- Does not build for Wasm. Emscripten support requires a WebSocket proxy, which
  is not a browser deployment.

### B. cpp-httplib

MIT, single header, trivially acquired on all three platforms.

- The smallest CI story available by a wide margin, and the license is beyond
  question.
- Range requests work through manually-set headers, and the response object
  exposes status and headers raw, so the framing rules are implementable.
- But redirect and retry handling are thin enough that the bounded versions get
  written here anyway, which removes much of the reason to take a dependency.
  Timeouts are coarser than the three-way split `HTTP006` wants.
- TLS still requires OpenSSL, so the "single header" acquisition advantage
  mostly evaporates on Windows — the hard part of option A was never libcurl
  itself.
- Connection reuse is weak, which lands directly on the `v0.3.0` measurement.
- Also does not build for Wasm, so it pays option A's strategic cost without
  buying option A's control.

### C. Platform-native stacks

WinHTTP on Windows, NSURLSession or CFNetwork on macOS, libcurl on Linux.

- No third-party license question exists at all, and the binary footprint is
  the smallest possible. TLS and the certificate store are the operating
  system's on every platform.
- Three implementations behind one seam. Each needs the full hostile-server
  corpus green, each has its own redirect and timeout semantics to bound, and
  each is a separate TSan surface.
- The failure modes differ per stack, so the projection onto `HTTP001`–`HTTP006`
  is written three times and is three times as likely to disagree with itself.
- This is a widening of a release whose scope is one sentence. It is the right
  end state only if a licensing or footprint problem forces it, and neither
  does.

### D. Boost.Beast

- Boost license, permissive, and the asio foundation would serve the async
  research track in `v0.5.0`+.
- But Beast is an HTTP *protocol* library, not a client: no redirects, no
  retry, no connection pool, no TLS without OpenSSL wired by hand. Choosing it
  means writing the client and then still owning every hostile case.
- Rejected without further comparison. The project needs a client, and building
  one is not in scope for the release that has to prove byte-equivalence.

## Considerations

- **License, decided first.** Options A, B, and D are all permissive and none
  constrains redistribution of a plugin binary. The criterion the design policy
  leads with turns out not to discriminate between the candidates, which is
  worth recording: it is a disqualifier that disqualified nobody, because the
  GPL and LGPL clients were never seriously in the running for a `.dll` that
  ships inside a bundle.
- **Footprint is not binary size alone.** libcurl's surface is large, but the
  slice this project uses is small and the linkage is static and private. The
  footprint that matters more is the acquisition burden on a contributor's
  machine and on three CI runners, which is why acquisition is decided in this
  ADR rather than left to the build.
- **TLS is the real cross-platform cost, not HTTP.** Every option except C
  needs a TLS backend and a certificate store. libcurl is the only option that
  resolves this to the platform's own on Windows without additional work.
- **The seam matters more than the choice.** Whatever is chosen, the
  `AssetReader` contract is what `usdAssetHttp` implements, and the client is
  behind a narrow internal transport interface. That is what makes this ADR
  cheap to supersede, and it is also what makes `usdAssetWasm` possible without
  one.
- **Credential elision already exists.** `v0.1.0` elides the query string and
  the userinfo component of an authority in every message and metrics dump. A
  client whose error strings embed the effective URL must be projected onto the
  typed vocabulary rather than passed through, or that work is undone. libcurl's
  error strings are the reason this is called out.

## Decision

**A, libcurl, acquired through `find_package`.**

```text
libs/usd-asset-http
    -> find_package(CURL REQUIRED)      private, never in a public header
    -> an internal transport seam       the only thing usdAssetHttp calls
```

The reasoning that settles it: three of the four constraints in the context
above are about *refusing* the library's convenience. Bounded redirects need
`FOLLOWLOCATION` off. ADR-0002's range rule needs the raw status of a response
whose body is perfectly valid. Framing validation needs `Content-Range` before
anything has interpreted it. libcurl is chosen not because it does the most,
but because it is the option that most reliably does exactly what it is told
and reports exactly what happened — which is the property a backend written
against a hostile-server corpus needs.

Option B loses on the second look rather than the first: its acquisition
advantage is real only until TLS enters, and it pushes bounded redirect and
retry back into this repository while offering worse connection reuse into the
release that has to measure amplification. Option C is deferred rather than
rejected, and its trigger is stated below.

Acquisition is `find_package` with CI installing per platform, matching how the
tree already treats OpenUSD — a guarded `find_package` whose absence is a
configure-time failure rather than a review comment:

```text
windows   vcpkg install curl
linux     apt-get install libcurl4-openssl-dev
macos     brew install curl
```

Vendoring was rejected because it puts third-party source into an Apache-2.0
tree that `NOTICE` must then track precisely, and turns an upgrade into a
commit. Pinned `FetchContent` was rejected because it slows every CI lane's
cold build to buy hermeticity this project does not yet need; it remains the
fallback if runner-provided versions drift far enough apart to matter.

### The Wasm criterion, answered honestly

The design policy calls a dependency that cannot build for Wasm a strategic
liability. libcurl cannot, and this decision accepts that liability rather than
arguing it away.

What makes it acceptable is that the mitigation was architected before the
question was asked. §1 of the workspace contract already reserves
`usdAssetWasm` as a sibling backend implementing the unchanged `AssetReader`
contract, and §2 already permits `any backend -> usdAssetIo, and nothing else
in this workspace`. The Wasm path was therefore never "recompile the HTTP
backend"; it was always "a second backend over `fetch`", sharing the contract,
the boundary suite, and the diagnostics, and sharing no transport code.

The claim this makes falsifiable: if `usdAssetWasm` is ever written and needs a
change to `AssetReader`, this ADR is implicated along with the contract, because
the reasoning above is the thing that will have been wrong.

What is genuinely lost is the ability to reach a browser by rebuilding the
existing backend. That is a real cost, paid knowingly, for exact control over
redirects, framing, and timeouts in the release that must establish that HTTP
range reads are byte-equivalent to a local file.

### Recorded consequences

- **`usdAssetHttp` is the only module that names libcurl**, per §2 of the
  workspace contract, and it names it privately. `curl.h` never appears in an
  installed header, so no consumer of the static library inherits it.
- **The transport seam is internal and narrow.** `usdAssetHttp` calls a small
  interface — issue a request, get status, headers, and bytes — and libcurl
  sits behind it. The seam exists so this ADR is cheap to supersede and so
  `tests/boundary`'s misbehaving-transport provisioning has somewhere to attach.
- **`CURLOPT_FOLLOWLOCATION` stays off.** Redirects are read from `Location`
  and bounded by this repository's counter, because a chain that the library
  follows silently is a chain the hostile corpus cannot test.
- **libcurl error strings are never passed through.** Every failure is
  projected onto the typed vocabulary in
  [DIAGNOSTICS.md](../architecture/DIAGNOSTICS.md) — `NetworkError`
  (`HTTP005`), `Timeout` (`HTTP006`), and the rest — before it reaches a
  message, so credential elision continues to hold.
- **One easy handle per reader, with a share handle for the connection pool.**
  This is the configuration the boundary suite's concurrency cases run against
  under ThreadSanitizer.
- **CI installs libcurl on all three runners.** The core lane's contract is that
  it needs no *OpenUSD* runtime; it was never that it needs no dependencies.
  The assertion made from the configure log stays as it is.
- **`NOTICE` records the curl license** when the first code lands, replacing the
  paragraph that currently says this release links no third-party library.
- **The Wasm target is `usdAssetWasm`**, not a rebuild of `usdAssetHttp`. Phase
  9 inherits this decision as a constraint rather than as an open question.

## When this is revisited

- **A licensing or footprint problem in a deployment that matters**, which is
  the trigger for option C. Native stacks become correct when a target refuses
  a third-party client, not before — and the seam is what makes that a
  replacement rather than a rewrite.
- **`usdAssetWasm` is actually attempted.** If the fetch backend cannot express
  the contract, the finding belongs against `AssetReader` and this ADR both.
- **HTTP/3, or async.** libcurl's multi interface is the assumed route for the
  `v0.5.0`+ research track. If it proves unsuitable, that is a new ADR
  superseding this one, not an edit to it.

## Open questions

1. Does the runner-provided libcurl version differ enough across Windows,
   Linux, and macOS arm64 to affect behavior the hostile corpus tests? If it
   does, `find_package` acquires a minimum version and pinned `FetchContent`
   returns to the table.
2. Does the share handle's connection pool interact with `v0.3.0`'s
   single-flight in a way that makes the amplification measurement ambiguous —
   that is, is a reused connection counted as the cache working?
