# Building

This repository is an OpenStrata project. It builds either through `ost`, which
resolves and composes a certified OpenUSD runtime, or through plain CMake
against an OpenUSD installation you supply.

Status: there is nothing to build yet — no libraries and no bundles exist. This
guide documents the workflow the first modules land into.

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

The root is dual-mode: it resolves OpenUSD once and adds each bundle, so a
plain CMake user can build everything.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=<your-openusd-install>
cmake --build build
ctest --test-dir build
```

## Building the libraries without OpenUSD

Everything under `libs/` builds with no OpenUSD at all, and this is the normal
way to work on the read contract, the backends, and the cache:

```sh
cmake -S libs -B build-core
cmake --build build-core
ctest --test-dir build-core
```

If this stops working, a `libs/` module has acquired an OpenUSD dependency,
which the [workspace contract](../architecture/WORKSPACE.md) forbids.

## Tests and the fixture server

The hostile-server corpus runs against a local fixture server started by the
test harness. It requires no network access and no external service, so CI runs
it on every platform without a hosting dependency.

Sanitizer builds are part of the contract rather than an optional extra: the
concurrency properties in §7 of the
[design policy](../design/DESIGN_POLICY.md) are only actually verified under
ThreadSanitizer.

```sh
cmake -S libs -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
cmake -S libs -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
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
