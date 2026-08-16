# Building

This repository is an OpenStrata project. It builds either through `ost`, which
resolves and composes a certified OpenUSD runtime, or through plain CMake — and,
for everything under `libs/`, with no OpenUSD at all.

Status: `libs/usd-asset-io`, `libs/usd-asset-local`, and the shared boundary
suite in `tests/` build and test today. No bundle exists yet, so the `ost
plugin` steps below are the workflow the first bundle lands into rather than
something you can run.

## Requirements

| Tool | Version |
| --- | --- |
| `ost` | 0.22.2 or newer |
| CMake | 3.23 or newer |
| Ninja | any recent |
| A C++17 compiler | MSVC 143, Clang, or GCC |

The project targets platform `cy2026`, profile `usd`, per `openstrata.toml`.

## With `ost`

```sh
ost runtime pull cy2026 --profile usd     # or adopt one: --from-usd <path>
ost doctor

ost library build libs/usd-asset-io        # one descriptor-owned library
ost library test  libs/usd-asset-io

ost plugin build http-resolver             # once the bundle exists
ost plugin test  http-resolver
ost plugin test --workspace --up-to 4
```

Each `libs/` module carries an `openstrata.library.yaml`, so `ost` has a
workspace identity for it and the bundle can declare the edge as
`requires.libraries` in `v0.2.0`.

One caveat today: `ost library build libs/usd-asset-local` fails to find the
`usdAssetIo` package, because nothing has installed it into a shared prefix
first and `ost` does not resolve a library-to-library edge on its own for this
command. The plain CMake paths below build the same module without that step,
and they are what `v0.1.0` is defined by. See the blocking items in
[implementation status](../roadmap/implementation-status.md).

`ost plugin test` runs the verification pyramid. The levels this project cares
about:

| Level | What it proves |
| --- | --- |
| L0 | The bundle's structure and manifest are well formed |
| L1 | It builds against the resolved runtime |
| L4 | It loads, registers its URI schemes, and resolves a fixture |
| L6 | `usdview` opens a remote fixture |

## Without `ost`

The root is libs-first. `libs/` is always built and never resolves OpenUSD; the
plugin bundles are built only when `USD_HTTP_RESOLVER_BUILD_PLUGIN` is `ON`,
which is the default, and OpenUSD is resolved only inside that branch.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=<your-openusd-install>
cmake --build build
ctest --test-dir build
```

## Building the core without OpenUSD

This is the normal way to work on the read contract, the backends, the cache,
and the boundary suite. It requires no OpenUSD installation:

```sh
cmake -S . -B build-core -DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF
cmake --build build-core
ctest --test-dir build-core
```

or, equivalently, through the preset:

```sh
cmake --preset core
cmake --build --preset core
ctest --preset core
```

On Windows, use `core-msvc` instead of `core` unless you are already in a Visual
Studio developer command prompt. The `core` preset pins the Ninja generator,
which needs `cl.exe` on `PATH` already; `core-msvc` sets the same cache
variables through the Visual Studio generator, which finds the toolchain itself.
The symptom of getting this wrong is `fatal error C1083: cannot open include
file: 'atomic'` — a standard header, missing because the compiler was started
without its environment. `core-msvc` is what the Windows CI cell runs.

This path is a contract, not a convenience — it is invariant 2 of the
[workspace contract](../architecture/WORKSPACE.md). If it stops working, a
`libs/` module has acquired an OpenUSD dependency, and the failure is the point:
the boundary is checked by the build rather than by review.

## Tests

```sh
ctest --test-dir build-core                       # everything
ctest --test-dir build-core -R boundary_local     # the shared suite, one backend
```

The boundary suite registers three tests per backend — `fixed`, `property`, and
`concurrent` — so a failure names which part of the read contract broke. How to
run it, how to reproduce a property failure from its seed, and how to enter a
new backend into it are in [tests/README.md](../../tests/README.md).

The hostile-server corpus, which is additional to the boundary suite rather than
a substitute for it, arrives with the HTTP backend in `v0.2.0`. It will run
against a local fixture server started by the test harness, requiring no network
access and no external service.

## Sanitizers

Sanitizer builds are part of the contract rather than an optional extra: the
concurrency properties in §7 of the
[design policy](../design/DESIGN_POLICY.md) are only actually verified under
ThreadSanitizer, and asserting them in prose asserts nothing. They run over the
core path, so they need no USD runtime — see the
[boundary suite contract](../contributing/BOUNDARY_SUITE.md).

```sh
cmake --preset core-asan     # -DUSD_HTTP_RESOLVER_SANITIZER=address,undefined
cmake --build --preset core-asan
ctest --preset core-asan

cmake --preset core-tsan     # -DUSD_HTTP_RESOLVER_SANITIZER=thread
cmake --build --preset core-tsan
ctest --preset core-tsan
```

Use clang or GCC. MSVC implements only `address`, and the root
`CMakeLists.txt` fails the configure with a message rather than emitting flags
that would be ignored into a green build that checked nothing. The MSVC
AddressSanitizer branch is present but unverified: on MSVC 19.34 the
instrumented binaries build and then exit with `STATUS_DLL_INIT_FAILED` before
`main`, with the runtime both on `PATH` and beside the executable.

The sanitizer flags include `-fno-sanitize-recover=all`. Without it,
UndefinedBehaviorSanitizer prints a violation and continues, the process exits
`0`, and CTest reports a pass — a lane that reports nothing it finds. ASan
already aborts and TSan already sets a non-zero exit code when it has reported,
so the flag changes only the UBSan lane. `UBSAN_OPTIONS` and `TSAN_OPTIONS` are
set by the test presets, so a local run uses what CI uses.

## CI

Two workflows, with two different owners.

`.github/workflows/core-ci.yml` is hand-authored and runs the lanes above: the
core build and test on Windows, Linux, and macOS arm64 with no OpenUSD present,
and `core-asan` and `core-tsan` on Linux. It is the `v0.1.0` exit criterion in
executable form.

It is hand-authored because no `ost` cell shape can express it: every cell must
pin and materialize an OpenUSD runtime, which is the one thing this lane must
not do, and a workspace cell's build step accepts no preset or cache variable,
so the sanitizer lanes are unreachable from a matrix. The reasoning and the
upstream asks are in
[docs/reports/ost/01](../reports/ost/01-2026-08-16-v0.1.0-ci-without-a-support-matrix.md).

`openstrata.ci.yaml` arrives in `v0.2.0` with `plugins/http-resolver`, the first
thing here a cell can name. From then on it is the support matrix and its
workflows are generated:

```sh
ost ci validate
ost ci generate github
```

Never hand-edit the generated workflow YAML. A host package requirement is
declared in `openstrata.ci.yaml` so regeneration re-renders it instead of
dropping a hand-added step. `core-ci.yml` stays hand-authored and outside that
rule, because it stays runtime-free.
