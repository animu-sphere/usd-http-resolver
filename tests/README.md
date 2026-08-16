# tests

Cross-module tests. A module's own tests live with the module; what lives here
belongs to no single module.

| Directory | Contains |
| --- | --- |
| `boundary/` | The shared boundary suite: the executable form of the read contract, and the thing every backend is admitted by |

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

`backends/boundary_local_main.cpp` is the whole of it. The HTTP backend's row in
`v0.2.0` is a file of about that length beside it, running a byte-identical
suite, and `tests/boundary/CMakeLists.txt` gains one line.

A backend that needs a case relaxed has either found a defect in the read
contract -- in which case
[ASSET_READER.md](../docs/architecture/ASSET_READER.md) changes first, for every
backend -- or it is not admissible. A per-backend exception is how a contract
stops being one.

### Fixture behavior is part of provisioning

One case needs a transport that misbehaves: "short read below EOF ->
`InvalidResponse`, never a hole". That is expressed as a fixture *behavior* a
backend must be able to provision, not as a fifth declaration and not as a case
a backend may decline. For a hostile HTTP server it is a truncated body; for a
local file it is the read fault in `usdAssetLocal/Testing.h`.

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
