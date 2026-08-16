# Workspace contract

This is the binding structural contract for `usd-http-resolver`. It fixes
module identities, dependency directions, root responsibilities, artifact
naming, and change invariants. A structural change that contradicts this
document must change this document first.

Status: `usdAssetIo` and `usdAssetLocal` exist and are tested; everything else
in §1 is still a boundary the implementation is required to respect rather than
a directory that exists. A directory is created when its first tested capability
exists; see the [roadmap](../roadmap/README.md).

## 1. Components

| Identity | Directory | Kind | Status | Responsibility |
| --- | --- | --- | --- | --- |
| `usdAssetIo` | `libs/usd-asset-io` | plain CMake/OpenStrata static library | implemented (`v0.1.0`) | The transport-independent core: the `AssetReader` random-access contract, `AssetMetadata`, byte-range and validator value types, the typed diagnostic vocabulary, and the metrics counter definitions. Contains no transport, no cache, and no OpenUSD. |
| `usdAssetLocal` | `libs/usd-asset-local` | plain CMake/OpenStrata static library | implemented (`v0.1.0`) | The local-file backend: positional reads against a file handle, size discovery, and filesystem-derived validators. It is the correctness oracle every other backend is compared against. |
| `usdAssetHttp` | `libs/usd-asset-http` | plain CMake/OpenStrata static library | planned (`v0.2.0`) | The HTTP backend: range requests, metadata requests, redirects, timeouts, bounded retry, response framing validation, and validator extraction. Owns the third-party HTTP client dependency; it is the only module that may name one. |
| `usdAssetCache` | `libs/usd-asset-cache` | plain CMake/OpenStrata static library | planned (`v0.3.0`) | Aligned block caching, read expansion, request coalescing, single-flight de-duplication, eviction under a memory budget, and cache statistics. It is a decorator over `AssetReader`, keyed by an opaque validator. |
| `http-resolver` | `plugins/http-resolver` | OpenStrata plugin bundle (`usd-asset-resolver`) | planned (`v0.2.0`) | The OpenUSD `ArResolver` implementation: URI scheme registration for `http` and `https`, URI normalization, relative and anchored resolution, asset-info exposure, and the `ArAsset` adapter over `AssetReader`. It is the only module that includes an OpenUSD header. Owns its `HTTPxxx` diagnostic codes. |
| `usdAssetS3`, `usdAssetPackage`, `usdAssetWasm` | `libs/` | plain libraries | reserved, not implemented | Additional backends targeting the unchanged `AssetReader` contract. A backend that cannot be expressed through it is a design question, not a feature request. |

None of the `libs/` modules is a plugin: none has a `plugInfo.json`, none
performs plugin registration, and none exposes an OpenUSD type. Only
`plugins/http-resolver` requires an OpenUSD runtime to build or test.

## 2. Dependency directions

Allowed:

```text
usdAssetLocal   -> usdAssetIo
usdAssetHttp    -> usdAssetIo
usdAssetHttp    -> the HTTP client dependency (private)
usdAssetCache   -> usdAssetIo
http-resolver   -> usdAssetIo, usdAssetCache, usdAssetLocal, usdAssetHttp
http-resolver   -> OpenUSD (ar, tf, arch)
```

The shared boundary suite is the one thing outside `libs/` that links a backend,
and the direction is one-way:

```text
tests/boundary (usdAssetBoundary)  -> usdAssetIo
tests/boundary/backends/*          -> usdAssetBoundary, and one backend each
```

The suite library itself must never link a backend. The moment it can name one
it can special-case one, and a per-backend exception is how a contract stops
being one. A backend is reached only from its own row executable.

Reserved future directions:

```text
usdAssetS3      -> usdAssetIo
usdAssetPackage -> usdAssetIo
usdAssetWasm    -> usdAssetIo
any backend     -> usdAssetIo, and nothing else in this workspace
```

Forbidden:

```text
usdAssetIo      -> anything (OpenUSD, a transport, an HTTP client, a cache)
any backend     -> another backend
any backend     -> usdAssetCache
usdAssetCache   -> any backend, or any transport concept
any lib         -> OpenUSD
any lib         -> a consumer repository
any module      -> an asset format parser
any dependency cycle
```

Two of these deserve their reason stated:

`usdAssetCache -> a backend` is forbidden because the cache is a decorator. It
holds an `AssetReader` it did not construct. If it knew about HTTP it would
grow HTTP policy, and the local backend would stop being a usable oracle for
the cached path.

`any lib -> OpenUSD` is forbidden because it is what keeps the boundary
testable. The backends, the cache, and the contracts build and test with plain
CMake and no OpenUSD runtime; only the bundle needs one. A test that requires a
USD runtime to prove a range read is a test that will be skipped.

### 2.1 The build graph is the enforcement

A documented dependency direction that the build does not enforce is a
convention, and conventions decay silently. The root `CMakeLists.txt` therefore
has the same shape as this section:

```text
root
 |- libs/*                      always built, OpenUSD never resolved
 `- plugins/*                   built only when USD_HTTP_RESOLVER_BUILD_PLUGIN
                                is ON, and only then is find_package(pxr) called
```

This path is supported, tested, and required to keep working:

```sh
cmake -S . -B build-core -DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF
cmake --build build-core
ctest --test-dir build-core
```

on a machine with no OpenUSD installed at all. A `libs/` module that acquires an
OpenUSD dependency does not produce a review comment; it produces a configure
failure on every developer machine and CI cell that runs the core path.

`libs/` modules are added by an explicit ordered list rather than a directory
glob, because a directory is created when its first tested capability exists
(invariant 11) and the list is the landing order from the
[roadmap](../roadmap/README.md).

## 3. Consumer direction

The dependency edge toward a consumer does not exist, in either direction:

```text
usd-pointcloud-plugins   -/->   usd-http-resolver      (forbidden, build time)
usd-http-resolver        -/->   usd-pointcloud-plugins (forbidden, always)

usd-pointcloud-plugins   ---> OpenUSD ArResolver / ArAsset  (the only edge)
usd-http-resolver        ---> OpenUSD ArResolver            (registration)
```

Composition is runtime-only, through `PXR_PLUGINPATH_NAME` or an OpenStrata
formation. This repository must not appear in any consumer's CMake, submodules,
vendored sources, or include paths, and no consumer fixture is required to run
this repository's tests. The rationale is [ADR-0001](../adr/0001-consumer-interface.md);
the mirror-image rule lives in the first consumer's
[resolver-backed source contract](https://github.com/animu-sphere/usd-pointcloud-plugins/blob/main/docs/architecture/RESOLVER_SOURCE.md).

## 4. Source boundaries

```text
plugins/http-resolver/src/HttpResolver.cpp
    ArResolver implementation: identifier creation, resolution, anchoring,
    asset opening, asset info. No transport, no byte handling.

plugins/http-resolver/src/ResolvedAsset.cpp
    ArAsset over AssetReader: Read(), GetSize(), buffer lifetime.
    No policy of its own.

libs/usd-asset-local/src/*.cpp
    The platform layer -- open, stat, positional read, close -- and the reader
    over it. No caching, no URI handling, no OpenUSD.

libs/usd-asset-http/src/*.cpp
    Request construction, response framing validation, retry, redirect,
    validator extraction. No caching, no ArAsset, no OpenUSD.

libs/usd-asset-cache/src/*.cpp
    Block alignment, coalescing, single-flight, eviction. No transport.

libs/usd-asset-io/include/**
    Contracts only. Header-heavy by design; an implementation that belongs to
    a backend must not appear here. The one thing here that is shared logic
    rather than a value type is ResolveReadRange, and it is shared for the
    reason it exists: the EOF boundary and the overflow check are where every
    backend gets it wrong, and one copy is one place to be right.

tests/boundary/src/**
    The shared suite. Names no backend, and duplicates the read arithmetic in
    its oracle on purpose.
```

Read orchestration does not live in the plugin. The plugin normalizes a URI,
constructs a reader stack, adapts it to `ArAsset`, and converts diagnostics.
When plugin code starts assembling ranges or interpreting responses, the
boundary has moved and this document is out of date.

## 5. Artifact naming

| Identity | CMake package | CMake target | Bundle library |
| --- | --- | --- | --- |
| `usdAssetIo` | `usdAssetIo` | `usdasset::io` | — |
| `usdAssetLocal` | `usdAssetLocal` | `usdasset::local` | — |
| `usdAssetHttp` | `usdAssetHttp` | `usdasset::http` | — |
| `usdAssetCache` | `usdAssetCache` | `usdasset::cache` | — |
| `http-resolver` | — | `HttpResolver` | `HttpResolver` |

The bundle declares `kind: usd-asset-resolver` and
`provides: [usd-resolver:http, usd-resolver:https]` in its
`openstrata.plugin.yaml`.

## 6. Root responsibilities

| File | Owns |
| --- | --- |
| `openstrata.toml` | Project identity, version, platform, and profile |
| `.github/workflows/core-ci.yml` | The runtime-free lanes, hand-authored: the core build and test on three platforms with no OpenUSD present, and the sanitizer builds. No `ost` cell can express a lane that pins no runtime |
| `openstrata.ci.yaml` | The CI support matrix, from `v0.2.0` and its first bundle; workflows are generated from it and never hand-edited |
| `CMakeLists.txt` | Libs-first root: always adds `libs/` and `tests/`, resolves OpenUSD and adds bundles only when `USD_HTTP_RESOLVER_BUILD_PLUGIN` is `ON`, so a plain CMake user can build with or without `ost` and with or without OpenUSD. Also owns `USD_HTTP_RESOLVER_SANITIZER`, because a sanitizer must cover the libraries and the suite that drives them with one switch |
| `CMakePresets.json` | The `default` (whole repo), `core` (libs only, no OpenUSD), `core-msvc` (the same, through the Visual Studio generator), `core-asan`, and `core-tsan` configure, build, and test presets |
| `VERSION` | The single source of the release version |
| `LICENSE`, `NOTICE` | Apache-2.0, and the third-party record the release gate checks |
| `tests/` | Cross-module tests. Today the shared boundary suite, which belongs to no single backend |
| `docs/` | Contracts, plans, and records |

`openstrata.toml` gains a `[workspace] members` declaration once more than one
member descriptor exists.

## 7. Invariants

1. A structural change updates this document in the same change.
2. No `libs/` module includes an OpenUSD header, and the whole of `libs/`
   configures, builds, and tests with no OpenUSD installation present.
3. No module in this repository parses an asset format.
4. Only `usdAssetHttp` names an HTTP client dependency.
5. The cache is a decorator over `AssetReader` and knows no transport concept.
6. Every backend passes the same boundary suite, unchanged.
7. No consumer repository is a build-time or test-time dependency.
8. Credentials never reach a cache key, a log, a diagnostic, or a persisted
   artifact.
9. Every public path is thread-safe, without a global lock.
10. Every directory under `libs/` and `plugins/` carries a `README.md` that
    satisfies the [module README contract](../contributing/MODULE_README_CONTRACT.md).
11. A directory is created when its first tested capability exists.
12. A release that changes I/O behavior records a metrics baseline.
