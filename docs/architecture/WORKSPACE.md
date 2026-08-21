# Workspace contract

This is the binding structural contract for `usd-http-resolver`. It fixes
module identities, dependency directions, root responsibilities, artifact
naming, and change invariants. A structural change that contradicts this
document must change this document first.

Status: everything in §1 exists and is tested except the reserved backends. A directory is created when its first
tested capability exists; see the [roadmap](../roadmap/README.md).

## 1. Components

| Identity | Directory | Kind | Status | Responsibility |
| --- | --- | --- | --- | --- |
| `usdAssetIo` | `libs/usd-asset-io` | plain CMake/OpenStrata static library | implemented (`v0.1.0`) | The transport-independent core: the `AssetReader` random-access contract, `AssetMetadata`, byte-range and validator value types, the typed diagnostic vocabulary, and the metrics counter definitions. Contains no transport, no cache, and no OpenUSD. |
| `usdAssetLocal` | `libs/usd-asset-local` | plain CMake/OpenStrata static library | implemented (`v0.1.0`) | The local-file backend: positional reads against a file handle, size discovery, and filesystem-derived validators. It is the correctness oracle every other backend is compared against. |
| `usdAssetHttp` | `libs/usd-asset-http` | plain CMake/OpenStrata static library | implemented (`v0.2.0`) | The HTTP backend: range requests, metadata requests, redirects, timeouts, bounded retry, response framing validation, and validator extraction. Owns the third-party HTTP client dependency; it is the only module that may name one, and it names it privately, in one translation unit, behind an internal transport seam. |
| `usdAssetCache` | `libs/usd-asset-cache` | plain CMake/OpenStrata static library | implemented (`v0.3.0`); persistence is `v0.4.0` | Aligned block caching, read expansion, request coalescing, single-flight de-duplication, eviction under a memory budget, and cache statistics. It is a decorator over `AssetReader`, keyed by an opaque validator. |
| `http-resolver` | `plugins/http-resolver` | OpenStrata plugin bundle (`usd-asset-resolver`) | implemented (`v0.2.0`); asset-info exposure is `v0.4.0` | The OpenUSD `ArResolver` implementation: URI scheme registration for `http` and `https`, URI normalization, relative and anchored resolution, asset-info exposure, and the `ArAsset` adapter over `AssetReader`. It is the only module that includes an OpenUSD header. Owns its `HTTPxxx` diagnostic codes. |
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
http-resolver   -> OpenUSD (ar, tf, arch, js, plug, vt)
```

What the bundle links today is `usdAssetHttp`, `usdAssetCache`, and the Ar
surface, and nothing else on either list. The cache edge is taken as of
`v0.3.0`: the resolver decorates every asset it opens, and binds it into the
process-wide block store by the identifier and the validator the backend
captured. `usdAssetLocal` is permitted and unused: a local path is
the primary resolver's business, and a URI-scheme resolver that reached for the
local backend would be answering for paths it does not claim.

A bundle's edges are declared twice and both declarations bind. The root build
graph resolves them as in-tree targets, and `plugins/http-resolver/openstrata.plugin.yaml`
declares them again under `requires.libraries` — which is the list
`ost plugin build` installs into the workspace prefix before it configures the
bundle standalone. The two are not redundant: a library the bundle links and
does not declare there builds perfectly in-tree, in every local lane and in
`core-ci.yml`, and fails only the bundle cells, with a `find_package` that
cannot be satisfied. `usdAssetHttp` carries `usdAssetIo` transitively;
`usdAssetCache` is its sibling over `usdAssetIo` and is carried by nothing, so
it has to be named. The `js`, `plug`,
and `vt` components are what `ar` itself needs in a non-monolithic build; a
resolver reads bytes and hands them over, so no `usd` or `usdGeom` component
appears.

The shared boundary suite is the one thing outside `libs/` that links a backend,
and the direction is one-way:

```text
tests/boundary (usdAssetBoundary)  -> usdAssetIo
tests/boundary/backends/*          -> usdAssetBoundary, and one backend each
```

The suite library itself must never link a backend. The moment it can name one
it can special-case one, and a per-backend exception is how a contract stops
being one. A backend is reached only from its own row executable.

The hostile-server corpus links nothing at all:

```text
tests/fixture-server (usdAssetFixtureServer)  -> the platform's sockets, and
                                                 the standard library
```

Not `usdAssetIo`, not a backend, and above all not the HTTP client. It is the
*server* side of the boundary `usdAssetHttp` sits on, and acquiring a second
third-party dependency to test the one §13 of the
[design policy](../design/DESIGN_POLICY.md) reasons about would be a dependency
admitted without an argument. Not knowing `usdAssetIo` is the more important
half: a corpus that could name `StatusCode` would start asserting the backend's
interpretation, and a disagreement between the two would stop being evidence.

Its reverse edges — a test linking both the fixture server and something that
reads from it — are legal, and there are exactly five:

```text
tests/boundary/backends/boundary_http_main.cpp  -> usdAssetHttp, fixture server
tests/corpus (usdAssetHttp_test_projection)     -> usdAssetHttp, fixture server
tests/baseline (usdAssetHttp_baseline)          -> usdAssetHttp, usdAssetCache,
                                                   fixture server
tests/cache-tuning (usdAssetCache_tuning)       -> usdAssetCache, usdAssetHttp,
                                                   fixture server
plugins/http-resolver/tests/test_stage.cpp      -> OpenUSD, fixture server
```

The first provisions remote fixtures, because a remote backend has to arrange
for its bytes to exist somewhere a transport can reach; that is what fixture
provisioning *means* for it. The second is the projection of each corpus
behavior onto the typed vocabulary, which is the one assertion neither side can
make alone.

The third is the recorded I/O baseline that gate 6 of
[the release gate](../releases/README.md) binds a release to: the five scenarios
in [METRICS.md](METRICS.md) §6, measured against an asset large enough for
`selectivity` to mean something. It needs a server for the reason the counters
do — nothing crosses a transport without one — and it compiles the fixture
server's own raw client besides, because "must not be worse than a plain
download" needs a plain download performed by a client that is not the one under
test.

The third also links the cache from `v0.3.0`, because that release is the first
to change the numbers on purpose and METRICS.md §6 asks a release that changes
I/O behavior for the counter values before *and* after. One harness, one
fixture, each scenario twice.

The fourth is the measurement that chose the cache's constants. It needs a
server for a reason of its own rather than the shared one: the constants are
about round trips, and a sweep over a local file would be a sweep over a cost
that does not exist there. It is also the only place that links the cache and a
backend at once, which is not the cache learning what a transport is — it holds
an `AssetReader` and nothing else — but a measurement of a stack, made where the
stack is.

The fifth is the only place in this repository that links OpenUSD and the
fixture server at once, and it is the one test that can assert the release's
actual claim: that a `UsdStage` opens over HTTP. It reaches the backend only
through `ArResolver`, which is the point — a test that linked `usdAssetHttp`
directly would be asserting the backend again rather than the bundle.

The first four live outside `libs/` rather than in a module's own tests, and
that placement is load-bearing rather than tidy: a module's tests must not
depend on anything outside `libs/`, or `ost library build libs/usd-asset-http` —
which builds the module alone — stops working.

The direction stays one-way in every case. The fixture server links none of them
and still does not know what a `StatusCode` is.

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

plugins/http-resolver/src/Identifier.cpp
plugins/http-resolver/src/Configuration.cpp
plugins/http-resolver/src/Diagnostics.cpp
    The parts of the bundle that are arithmetic rather than integration: URI
    normalization and anchoring, the environment surface, and the HTTPxxx
    projection with its message form. No OpenUSD header, and one test
    executable each, linking that translation unit and nothing else.

    They are separate files for that reason and not for tidiness. A wrong
    normalization is invisible from the outside -- two spellings of one asset
    become two opens and two revisions -- and a test that would catch it must
    not need a USD runtime to run.

plugins/http-resolver/src/Report.cpp
    The one place that talks to OpenUSD's diagnostic system. Splitting it from
    Diagnostics.cpp is what keeps that file testable offline, and it is why
    there is no path from a failure to a human that skips ElideSecrets.

libs/usd-asset-local/src/*.cpp
    The platform layer -- open, stat, positional read, close -- and the reader
    over it. No caching, no URI handling, no OpenUSD.

libs/usd-asset-http/src/*.cpp
    Request construction, response framing validation, retry, redirect,
    validator extraction. No caching, no ArAsset, no OpenUSD.

libs/usd-asset-cache/src/*.cpp
    Block alignment, coalescing, single-flight, eviction. No transport.

    BlockPlan.h is the arithmetic with no state, no lock, and no reader, and it
    is separated for the reason ResolveReadRange is separated in usdAssetIo:
    this is where the off-by-one lives, and it should be checkable without
    provisioning an asset. It is internal and not installed.

libs/usd-asset-io/include/**
    Contracts only. Header-heavy by design; an implementation that belongs to
    a backend must not appear here. The one thing here that is shared logic
    rather than a value type is ResolveReadRange, and it is shared for the
    reason it exists: the EOF boundary and the overflow check are where every
    backend gets it wrong, and one copy is one place to be right.

tests/boundary/src/**
    The shared suite. Names no backend, and duplicates the read arithmetic in
    its oracle on purpose.

tests/baseline/**
    The recorded I/O baseline: the five scenarios METRICS.md §6 requires,
    each measured with the cache and without it, the fixture they run against,
    and the shape a release record pastes. It asserts byte counts, which are
    exact, and reports ratios and wall clock, which are about the fixture and
    the runner. Measurement only -- it owns no read semantics, and a case it
    would be the first to catch belongs in tests/boundary instead.

tests/cache-tuning/**
    The block-policy measurement: a sweep of block size against coalescing gap
    over the access patterns METRICS.md §6 names, plus one pattern of its own,
    which exists because it is the only one in which the gap can bind at all. It
    chooses constants and asserts correctness; it chooses nothing from wall
    clock, and says so. Record: reference/BLOCK_POLICY.md.

tests/fixture-server/src/**
    The hostile corpus: a loopback origin, its socket layer, and the request
    parsing and range arithmetic it answers with. Stops at the wire. Nothing
    here knows what a StatusCode is, and its self-test parses responses with
    its own code rather than the server's, for the same reason the boundary
    suite's oracle duplicates ResolveReadRange.
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
| `openstrata.ci.yaml` | The CI support matrix, from `v0.2.0` and its first bundle. `.github/workflows/ost-source-ci.yml` is generated from it by `ost ci generate github` and is never hand-edited |
| `.github/workflows/plugin-windows-ci.yml` | The plugin lane on Windows, hand-authored: libcurl there comes from vcpkg and no generated cell can hand CMake a prefix. It declares no pins of its own — it reads them out of `openstrata.ci.yaml` at run time — so the matrix stays the single declaration |
| `CMakeLists.txt` | Libs-first root: always adds `libs/` and `tests/`, resolves OpenUSD and adds bundles only when `USD_HTTP_RESOLVER_BUILD_PLUGIN` is `ON`, so a plain CMake user can build with or without `ost` and with or without OpenUSD. Also owns `USD_HTTP_RESOLVER_SANITIZER`, because a sanitizer must cover the libraries and the suite that drives them with one switch |
| `CMakePresets.json` | The `default` (whole repo), `core` (libs only, no OpenUSD), `core-msvc` (the same, through the Visual Studio generator), `core-asan`, and `core-tsan` configure, build, and test presets |
| `VERSION` | The single source of the release version |
| `LICENSE`, `NOTICE` | Apache-2.0, and the third-party record the release gate checks |
| `tests/` | Cross-module tests: the shared boundary suite, which belongs to no single backend; the hostile-server fixture corpus, which belongs to no module because it is the other side of the boundary; the recorded I/O baseline, which belongs to no module because it measures the whole path; and the block-policy sweep, which belongs to no module because it measures a stack |
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
