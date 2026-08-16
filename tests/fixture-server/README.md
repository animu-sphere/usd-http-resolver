# fixture-server

The hostile-server corpus: an HTTP/1.1 origin on loopback that can be told to
misbehave in each of the ways the HTTP backend has to survive.

It exists *before* the backend, which is the whole point. §17 of the
[design policy](../../docs/design/DESIGN_POLICY.md) makes standing it up the
first action of phase 2, "so that the backend is written against a passing
oracle rather than debugged against a server". A backend written first is a
backend whose bugs are indistinguishable from server behavior: a wrong answer at
EOF looks like a proxy, a short read looks like a network fault, and every
debugging session starts by arguing about which side is wrong.

```sh
ctest --test-dir build/core -R usdAssetFixture_corpus
```

## What it owns

- One origin server per test process, on `127.0.0.1` with an ephemeral port.
- The named behaviors in `include/usdassetfixture/Corpus.h`, one per condition a
  backend must distinguish.
- A request log — method, target, `Range`, `If-Range`, and the answered status —
  because bounded redirects, "no request was issued for a zero-length read", and
  "`If-Range` on every range request" are properties of what was *sent*, and are
  otherwise unassertable.
- Fixture provisioning for the HTTP backend's row in the
  [boundary suite](../../docs/contributing/BOUNDARY_SUITE.md), including the
  republish that the revision-simulation declaration requires.

## What it refuses

- **A third-party dependency.** This repository takes exactly one, chosen on
  license, footprint, and Wasm viability in
  [ADR-0003](../../docs/adr/0003-http-client-dependency.md) — and it is a
  *client*. A server acquired to test it would be a second dependency admitted
  without an argument ever being made. Sockets and the standard library are the
  whole of it.
- **Anything above the wire.** Nothing here knows what `InvalidResponse` is, or
  that `usdAssetIo` exists. The moment the corpus asserts the backend's
  interpretation, the corpus and the backend stop being independent and a
  disagreement between them stops being evidence.
- **Reachability from off the machine.** Loopback, never `INADDR_ANY`, and never
  a fixed port: §11.2 of the design policy requires the corpus to run in CI
  without network access, and two lanes on one runner must not collide.
- **Shipping.** It is under `tests/`, not `libs/`, because it is not a module of
  the product. It has no consumer and it is in no artifact.

## The corpus

§11.2 of the design policy names nine conditions. Each has a row, and three
more rows come from constraints fixed elsewhere:

| Behavior | Condition | Required by |
| --- | --- | --- |
| `Normal` | Honors `Range`, `If-Range`, and `HEAD` as RFC 9110 requires | The corpus needs a correct row, or it cannot tell a careful backend from one that fails everything |
| `NoAcceptRanges` | No `Accept-Ranges`; a ranged `GET` gets `200` and the whole body | §11.2 |
| `IgnoresRange` | Advertises `Accept-Ranges: bytes`, then answers a `Range` with `200` anyway | §11.2, and [ADR-0002](../../docs/adr/0002-range-unsupported-policy.md) |
| `TruncatedBody` | Honest `Content-Range` and `Content-Length`, fewer body bytes, orderly close | §11.2; also the boundary suite's short-read-below-EOF row |
| `ContentRangeTooShort` | `206` accurately describing a range shorter than the request | §11.2; the example in [DIAGNOSTICS.md](../../docs/architecture/DIAGNOSTICS.md) §6 |
| `ContentRangeShifted` | `206` describing a range at a different offset | §11.2; a different check from the row above — start, not length |
| `UnknownContentLength` | `200` with no `Content-Length`, body delimited by close | ADR-0003: the client must be able to refuse it |
| `ValidatorChangeMidRead` | The `ETag` and content change underneath an open reader | §11.2, and §6 of the design policy |
| `RedirectChain` | `redirectHops` `302`s, then the asset | §11.2 |
| `RedirectLoop` | A `302` pointing at its own path, unbounded by the server | ADR-0003: bounding a chain is this repository's counter, not the library's |
| `SlowHeaders` | `delayMs` before the status line | §11.2 |
| `SlowBody` | Headers and one byte, `delayMs`, then the rest | §11.2; `HTTP006` must name *which* deadline elapsed |
| `ConnectionResetBeforeResponse` | Reads the request, then `RST` | §11.2 |
| `ConnectionResetMidBody` | Headers, `deliverBytes` of body, then `RST` | §11.2 |
| `RangeNotSatisfiable` | `416` with `Content-Range: bytes */size` | §11.2 |
| `NotFound` | `404` | The `NotFound` / `NetworkError` distinction in DIAGNOSTICS.md §4.4 |
| `AccessDenied` | `403` | `AccessDenied` is a configuration problem and `404` is a scene problem |
| `TransientServerError` | `503` for `transientFailures` requests, then service | §4.1 bounded retry, and DIAGNOSTICS.md §3: a retry is visible in metrics |

The list is enumerable at runtime (`AllBehaviors()`), and the self-test walks it
and fails when a behavior has no case. That is what makes the coverage claim in
this table checkable rather than asserted.

### One row the redirect case does not carry

§10 of the design policy requires rejecting a scheme downgrade from `https` to
`http` in a redirect chain. This server speaks plaintext HTTP only, so it cannot
stage the downgrade end to end — the `https` half does not exist to be
downgraded *from*. Rather than fake it with a `Location` the test declares was
reached over TLS, the corpus leaves the case out and says so here. The rule is
redirect *policy*, not server behavior, and it belongs in `usdAssetHttp`'s own
tests against a synthetic `Location` — where it is a decision about a string,
and needs no server at all.

## The self-test is the point

`tests/test_corpus.cpp` asserts, over a raw socket, that each behavior puts on
the wire exactly what its name claims. A corpus that has not been checked this
way is not an oracle; it is a second unknown, and the first argument about a
failing range read is then about which of the two is wrong.

The client in `tests/RawClient.h` shares the socket layer with the server —
`send` is not the thing under test — and shares no HTTP code with it at all.
Requests are written as literal bytes and responses parsed independently, so a
framing assertion is two expressions of one rule rather than one expression
checked against itself. This is the same separation the boundary suite keeps
between its oracle and `usdAssetLocal`, and for the same reason: without it, a
server that serves a wrong `Content-Range` and a parser that agrees about what
"wrong" means would be wrong together and pass.

The corpus was checked against deliberately broken servers before it was
trusted. Seven mutations — a `Content-Range` that stops being short, an ignored
`If-Range` mismatch, a body that is no longer truncated, an off-by-one clamp at
EOF, a suffix range counted from the wrong end, a `Content-Length` that should
not be there, and a dropped log field — each produced a named failure. The
eighth, removing the redundant multi-range guard, changed no observable
behavior, and is recorded as an equivalent mutant rather than as coverage.

## Entering the HTTP backend into the boundary suite

The boundary suite asks a backend for fixtures with a declared *behavior*, not
only a size ([BOUNDARY_SUITE.md](../../docs/contributing/BOUNDARY_SUITE.md) §6).
Two of its declarations resolve here:

| Suite declaration | Served by |
| --- | --- |
| `FixtureBehavior::Normal` | `Behavior::Normal` |
| `FixtureBehavior::ShortReadBelowEof` | `Behavior::TruncatedBody` |
| Revision simulation | `Server::Republish` |

`Server::Republish` returns false when the path names nothing, and the suite is
required to treat that as a fixture that failed to change rather than as a
backend that failed to notice. Those are different defects, in different
repositories, and a suite that conflates them reports the wrong one.

## Portability

`src/Socket.cpp` is the only socket code in this repository and it carries three
platform branches, two of which no local lane on a Windows workstation compiles.
Both traps found so far are *header* facts rather than logic, so both are
invisible until the macos-arm64 cell runs:

| Trap | Linux | macOS |
| --- | --- | --- |
| Suppressing `SIGPIPE` on a write to a hung-up peer | `MSG_NOSIGNAL` per send | no such flag; `SO_NOSIGPIPE` per socket |
| `htonl`, `htons`, `ntohs` | functions, so `::htonl(x)` compiles | macros over a *statement expression*, so `::htonl(x)` does not parse |

The second cost a red CI run. The byte-order conversions are therefore wrapped
once each in the anonymous namespace and called unqualified from there, so there
is one place to be wrong instead of four call sites that each look correct on
two platforms out of three.

Either can be reproduced locally without a Mac by compiling the file against a
Linux toolchain wearing Darwin's macros — undefine `MSG_NOSIGNAL`, define
`SO_NOSIGPIPE`, and redefine the byte-order conversions as statement
expressions. The shape matters and not merely the macro-ness: an ordinary
function-like macro still parses after `::`, so a proxy that uses one passes on
code the real lane rejects.

## Threading

One thread accepts, one thread per connection. Two properties are contract:

- **Shutdown never hangs.** The accept loop polls rather than blocking, so
  stopping does not require closing a descriptor another thread is inside; and
  every deliberate stall waits on a condition variable that `Stop()` signals, so
  a `SlowBody` handler cannot hold a test process open past the end of its case.
- **Concurrent requests are correct.** The boundary suite runs concurrent
  readers over one asset under ThreadSanitizer, and every one of those cases
  will run against this server. A fixture server that were only correct
  single-threaded would turn the suite's concurrency rows into a test of the
  fixture. The self-test's concurrency case is what pins this, and it runs under
  TSan in the `core-tsan` preset like everything else.
