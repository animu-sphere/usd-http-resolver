# tests

Cross-module tests. A module's own tests live with the module; what lives here
belongs to no single module.

| Directory | Contains |
| --- | --- |
| `boundary/` | The shared boundary suite: the executable form of the read contract, and the thing every backend is admitted by |
| `fixture-server/` | The hostile-server corpus: a loopback HTTP origin that misbehaves on request, and the conditions only a server can produce |
| `corpus/` | The projection of each corpus behavior onto the typed vocabulary — which `Behavior` produces which `StatusCode` |
| `baseline/` | The recorded I/O baseline: the five scenarios METRICS.md §6 requires, measured against a fixture large enough for `selectivity` to mean something |

The first three are not the same suite and none substitutes for another. The boundary
suite carries the correctness argument, over an oracle, for every backend. The
corpus covers what has no local analogue — a `200` answering a `Range`, a wrong
`Content-Range`, a mid-read validator change, a reset — and it applies to the
HTTP backend alone. See §7 of
[BOUNDARY_SUITE.md](../docs/contributing/BOUNDARY_SUITE.md).

`corpus/` is where the other two meet, and it is the only place either one's
opinion about the other is written down. Nothing in `fixture-server/` knows what
a `StatusCode` is, and nothing in `libs/usd-asset-http` knows what a `Behavior`
is; the table lives here precisely so that a disagreement between the server and
the backend is evidence rather than a tautology. Its coverage is checked against
`AllBehaviors()` at runtime, so adding a behavior without adding a projection
fails the run.

```sh
ctest --test-dir build/core -R usdAssetHttp_corpus_projection
```

## The baseline

`baseline/` is not a fourth correctness suite. It answers gate 6 of
[the release gate](../docs/releases/README.md), which exists because a resolver
that ships correct behavior with a silent doubling of transferred bytes has
regressed the only property it exists to provide — and the boundary suite would
pass every byte of it.

It stands up the fixture server, serves one 128 MiB synthetic asset, and runs
METRICS.md §6's five scenarios through the HTTP backend with the shipped
transport defaults. What it asserts and what it merely records is a deliberate
split:

| Quantity | Treatment |
| --- | --- |
| Bytes requested, bytes transferred, requests, retries, redirects | Asserted exactly. With no cache, a read of *n* bytes is one request moving exactly *n* bytes |
| `amplification`, `selectivity`, the other ratios | Recorded. They are byte counts over a fixture size, and a gate on one would move when the fixture did |
| Latency and wall clock | Recorded. Loopback has no round-trip time worth the name |

Every scenario verifies the bytes it counted: each byte of the fixture is a hash
of its own offset, so a read that landed elsewhere cannot compare equal.

```sh
ctest --test-dir build/core -R usdAssetHttp_io_baseline
./build/core/tests/baseline/usdAssetHttp_baseline --output baseline.md
USD_ASSET_BASELINE_ASSET_BYTES=536870912 ./build/core/tests/baseline/usdAssetHttp_baseline
```

The current record is [BASELINE.md](../docs/reference/BASELINE.md); a release
record copies it at its tag. The fourth scenario's comparator — "must not be
worse than a plain download" — is `fixture-server/tests/RawClient.cpp`, one
`GET`, no `Range`, and no HTTP code shared with the backend, for the same reason
the boundary suite's oracle shares none with `usdAssetLocal`.

## The boundary suite

`v0.1.0` ships a local file reader, which is not interesting. What is
interesting is that it arrives with the harness that makes every subsequent
transport cheap to verify and impossible to fake. The contract for what the
suite must contain is
[BOUNDARY_SUITE.md](../docs/contributing/BOUNDARY_SUITE.md); this file is how to
run it and how to enter a backend into it.

```sh
ctest --test-dir build/core -R boundary_local
```

Three groups, registered separately so that a failure names which part of the
contract broke:

| Group | Asserts |
| --- | --- |
| `fixed` | The required boundary cases, per fixture size; the short read below EOF; the mid-read revision change |
| `property` | Biased generated cases, shrunk to a minimal reproducer on failure |
| `concurrent` | Concurrent reads on one reader, and concurrent readers on one asset |

## The oracle

Every case is an equivalence against a deliberately naive local reader -- open,
seek, read, close -- over the whole result:

```text
expected.bytes     == actual.bytes
expected.bytesRead == actual.bytesRead
expected.status    == actual.status
```

Comparing the status is what catches the failure a byte comparison cannot see. A
backend that returns the right bytes with the wrong code has broken the EOF
distinction, and the layer above acts on the code.

The oracle in `src/Oracle.cpp` shares no code with `usdAssetLocal` -- not its
platform layer, and deliberately not even `ResolveReadRange`. The arithmetic is
duplicated on purpose. Two independent expressions of one rule disagreeing is a
signal; one expression checked against itself is not, and without that
separation the local backend's own row would be a tautology.

## Entering a backend

Adding a transport adds a row, not a suite. A row declares four things:

| Declaration | For the local backend |
| --- | --- |
| Factory | `usdasset::local::OpenAsset` |
| Fixture provisioning | The oracle's own file; the suite writes the content |
| Cancellation | Not admitted -- declared, not skipped |
| Revision simulation | Admitted: rewrite the file underneath the open reader |

`backends/boundary_local_main.cpp` is the whole of it. The HTTP row is
`backends/boundary_http_main.cpp` beside it, and it is what the claim above
cost: one file, one line in `tests/boundary/CMakeLists.txt`, and no change to
the suite. Its four declarations:

| Declaration | For the HTTP backend |
| --- | --- |
| Factory | `usdasset::http::OpenAsset` |
| Fixture provisioning | The suite's content, read back through the oracle and served by the fixture server; `ShortReadBelowEof` is `Behavior::TruncatedBody` |
| Cancellation | Not admitted — declared, not skipped |
| Revision simulation | Admitted: `Server::Republish`, with content and validator moving together |

The short-read row is worth pointing at. It is not a mock: it is a server that
really accepts the range, really delivers fewer bytes than it covers, and really
closes below the end of the asset — and the corpus already proved over a raw
socket that it does.

A backend that needs a case relaxed has either found a defect in the read
contract -- in which case
[ASSET_READER.md](../docs/architecture/ASSET_READER.md) changes first, for every
backend -- or it is not admissible. A per-backend exception is how a contract
stops being one.

### Fixture behavior is part of provisioning

One case needs a transport that misbehaves: "short read below EOF ->
`InvalidResponse`, never a hole". That is expressed as a fixture *behavior* a
backend must be able to provision, not as a fifth declaration and not as a case
a backend may decline. For a hostile HTTP server it is a truncated body -- and
that server now exists, as `Behavior::TruncatedBody` in
[`fixture-server/`](fixture-server/README.md); for a local file it is the read
fault in `usdAssetLocal/Testing.h`.

## Seeds and reproduction

The property cases run from a fixed seed, so a CI failure is a real failure
rather than a seed nobody can reproduce. Override it to widen a search:

```sh
USD_ASSET_BOUNDARY_SEED=0x1234 ./build/core/tests/boundary/boundary_local property
```

A failure prints the seed, the iteration, and a shrunk minimal reproducer,
formatted as the line to paste into `FixedCasesFor` in `src/Suite.cpp`:

```text
FAIL [local/property] property (reproducer)
      seed=26719988210488654 iteration=3 minimal: AddCase(cases, 0, 2);
```

Paste it. A property failure fixed without gaining a fixed case can regress
silently.

## Sanitizers

Sanitizer builds are part of the contract, not an optional lane. The concurrency
properties are only actually verified under ThreadSanitizer; asserting them in
prose asserts nothing.

```sh
cmake --preset core-asan && cmake --build build/core-asan && ctest --preset core-asan
cmake --preset core-tsan && cmake --build build/core-tsan && ctest --preset core-tsan
```

Both need clang or GCC. MSVC implements `/fsanitize=address` and nothing else,
and the root `CMakeLists.txt` says so with an error rather than emitting flags
that would be ignored into a green build that checked nothing.

Because the core libraries build without OpenUSD, these builds need no USD
runtime and run on every platform. That is the practical argument for the
libs-first build graph: a range-read test that needs a USD runtime is a test
that gets skipped, and a sanitizer build that needs one is a sanitizer build
that never runs.
