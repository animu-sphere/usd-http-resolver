# Building

This repository is an OpenStrata project. It builds either through `ost`, which
resolves and composes a certified OpenUSD runtime, or through plain CMake — and,
for everything under `libs/`, with no OpenUSD at all.

Status: there is nothing to build yet — no libraries and no bundles exist. The
root build graph, including the OpenUSD-free path, is in place and configures
today. This guide documents the workflow the first modules land into.

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

ost build                                  # libraries
ost test

ost plugin build http-resolver             # once the bundle exists
ost plugin test  http-resolver
ost plugin test --workspace --up-to 4
```

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

This path is a contract, not a convenience — it is invariant 2 of the
[workspace contract](../architecture/WORKSPACE.md). If it stops working, a
`libs/` module has acquired an OpenUSD dependency, and the failure is the point:
the boundary is checked by the build rather than by review.

## Tests and the fixture server

The hostile-server corpus runs against a local fixture server started by the
test harness. It requires no network access and no external service, so CI runs
it on every platform without a hosting dependency.

Sanitizer builds are part of the contract rather than an optional extra: the
concurrency properties in §7 of the
[design policy](../design/DESIGN_POLICY.md) are only actually verified under
ThreadSanitizer. They run over the core path, so they need no USD runtime — see
the [boundary suite contract](../contributing/BOUNDARY_SUITE.md).

```sh
cmake -S . -B build-asan -DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
cmake -S . -B build-tsan -DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread"
```

## CI

The support matrix is `openstrata.ci.yaml`, and workflows are generated from
it:

```sh
ost ci validate
ost ci generate github
```

Never hand-edit the generated workflow YAML. A host package requirement is
declared in `openstrata.ci.yaml` so regeneration re-renders it instead of
dropping a hand-added step.
