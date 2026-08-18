# OST 03: a support matrix, and the one lane it could not express

| Field | Value |
| --- | --- |
| Date | 2026-08-18 |
| Subject | `openstrata.ci.yaml` lands; which rungs its cells run, and which lanes stay outside it |
| `ost` version | 0.22.2 |
| Runtimes pinned | `openstrata-runtime-cy2026-usd`, OpenUSD 26.08, on Linux, macOS arm64, and Windows |
| Platform (local walk) | Windows 11 (26200), MSVC 19.34, CMake 4.4, Ninja |
| Result | Six cells generated; one lane hand-authored; two rung caps recorded; two upstream asks, one of them new |

[Report 02](02-2026-08-18-resolver-bundle-under-the-pyramid.md) closed by naming
the decision this report exists to record:

> `openstrata.ci.yaml` can name this bundle, which is what phase 2 was waiting
> for; whether its generated cells run `ost plugin test` with a known-failing
> rung, or run `ctest` instead, is the decision that report will record.

The answer is **both, split by what each can actually assert**, and it took a
third lane that `ost` cannot express at all.

## 1. What landed

```text
openstrata.ci.yaml                          6 cells, all pull_request
.github/workflows/ost-source-ci.yml         generated; 3 jobs
.github/workflows/plugin-windows-ci.yml     hand-authored; 1 job
.github/workflows/core-ci.yml               hand-authored; unchanged since v0.1.0
```

| Cell | Kind | Runner | Runs |
| --- | --- | --- | --- |
| `workspace-graph-pr-linux` | workspace | ubuntu-24.04 | `verify: graph` |
| `workspace-graph-pr-windows` | workspace | windows-2022 | `verify: graph` |
| `workspace-pr-linux` | workspace | ubuntu-24.04 | `ost build`, `ost test` |
| `workspace-pr-macos-arm64` | workspace | macos-15 | `ost build`, `ost test` |
| `http-resolver-pr-linux` | bundle | ubuntu-24.04 | pyramid to L1, then `ost plugin package` |
| `http-resolver-pr-macos-arm64` | bundle | macos-15 | pyramid to L1, then `ost plugin package` |

```sh
ost ci validate
#   Matrix openstrata.ci.yaml: 6 cell(s), structure OK
ost ci plan
#   lanes:            pull_request 6, main 0, scheduled 0, workflow_dispatch 0
#   hosted jobs:      6 (metered classes: linux-hosted, windows-hosted, macos-arm64-hosted)
#   publish-capable:  0 job(s)
ost ci generate github
#   Generated .github/workflows/ost-source-ci.yml from openstrata.ci.yaml
```

## 2. `verify: graph` runs now, and could not before

Blocking item 1 in [implementation status](../../roadmap/implementation-status.md)
recorded that the one runtime-free rung `ost` offers refused to run:

```text
PRECONDITION_FAILED: no plugin bundles found in the workspace member set
```

`plugins/http-resolver` is what that precondition was waiting for. The same
command, unchanged, in the same workspace:

```sh
ost plugin test --workspace --graph-only
#   Workspace dependency graph: 1 bundle(s), 0 bundle edge(s), 3 libraries,
#   3 library edge(s), valid
```

That is one third of blocking item 1 resolved by a commit rather than by an
upstream change. The other two thirds — that every `SupportCell` materializes a
runtime, and that a `kind: workspace` cell's build step takes no preset or cache
variable — are unchanged, which is why `core-ci.yml` is still hand-authored and
still the only lane that demonstrates the OpenUSD-free build.

## 3. Two rung caps, and why each is a cap rather than a workaround

### 3.1 The bundle cells stop at L1

L2 (`resolver.registration`) fails structurally for a network resolver, for the
reason established in report 02 §2: it runs
`Ar.GetResolver().Resolve("<scheme>:<fixture>")` and requires a non-empty path,
and `Resolve` returning non-empty *asserts the asset exists*. There is no origin
in an `ost plugin test` session, and `https:C:/…/local_unchanged.usda` has no
authority, so it is not an absolute `http` or `https` URI and normalizes to
nothing this bundle claims.

`up_to: 1` therefore, and the cap is asserted rather than assumed:

```sh
ost plugin test plugins/http-resolver --up-to 1
```

```text
  [PASS] L0 bundle.manifest            http-resolver 0.2.0 (usd-asset-resolver)
  [PASS] L0 bundle.plug_info           valid JSON at 'plugin/resources/httpResolver/plugInfo.json'
  [PASS] L0 bundle.plug_info.library_path 1 LibraryPath entry points under bundle lib/ (target windows)
  [PASS] L0 plugin.shared_library      found lib/libHttpResolver.dll
  [PASS] L0 bundle.fixtures            1 fixture(s) present
  [SKIP] L0 bundle.runtime_libs        no runtime library dirs declared
  [SKIP] L0 bundle.runtime_plugin_paths no staged provider plugin paths declared
  [PASS] L0 session.plugin_path        PXR_PLUGINPATH_NAME would include '…/plugin/resources/httpResolver'
  [PASS] L1 runtime.source             runtime source is 'artifact' (reproducible)
  [PASS] L1 runtime.openusd.version    runtime OpenUSD 26.08 satisfies '>=26.08,<27.0'
  [SKIP] L1 runtime.cxx_abi            plugin defers its C++ ABI to the runtime (inherit)
  [SKIP] L1 runtime.python_abi         plugin declares no Python ABI

Result: OK (8 pass, 0 fail, 4 skip)
```

L0 and L1 are not a consolation prize. They are the only checks in this
repository that look at the *shipped* shape rather than the build tree: that
`plugInfo.json` parses, that its `LibraryPath` points under the bundle's own
`lib/` for the target, and that the runtime came from a pinned artifact rather
than from whatever the host had. A `ctest` run loads the library out of the
build directory and can see none of it.

L3, L4, and L5 pass, and they are deliberately not in a cell, because they sit
on the far side of a failing rung and `ost plugin test` has no way to skip one.
They were walked by hand in report 02 and stay recorded there. Raising `up_to`
to 5 is a one-line change on the day `ost` asks L2 for dispatch rather than for
resolution.

### 3.2 The Windows cell stops at `graph`

Not because of a rung, but because the build cannot start; §4.

## 4. The lane `ost` could not express

`libs/usd-asset-http` resolves libcurl with `find_package(CURL)` — deliberately,
per [ADR-0003](../../adr/0003-http-client-dependency.md), and it is not an `ost`
library edge, because libcurl is not a member of this workspace. On Linux and
macOS the library is a host package and `host_packages` installs it. On Windows
it comes from vcpkg, and two facts collide:

1. the generated host-package step renders an installer for `apt` and `brew`
   only, and exits with *"host_packages has no installer for Windows; provision
   the dependency on the runner image instead"*;
2. `ost build` accepts no prefix, no toolchain file, and no `-D` passthrough
   (report 02, ask 3).

So a generated Windows cell cannot configure this workspace at all. It fails at
`find_package(CURL)`, before anything is compiled — which is not a Windows
problem, a vcpkg problem, or a curl problem. It is the absence of a way to say
"this workspace needs a prefix".

`v0.2.0`'s exit criterion names Windows explicitly, so the lane is hand-authored
in `.github/workflows/plugin-windows-ci.yml`. It installs
`curl:x64-windows-static-md` — the same acquisition and the same triplet as
`core-ci.yml`, so the two Windows lanes link the same library — exports
`CMAKE_PREFIX_PATH` into the environment, and then runs the same two commands
the generated workspace cell runs.

`ZLIB_ROOT` is exported alongside it, and that is not redundancy. vcpkg's
`CURLConfig.cmake` calls `find_dependency(ZLIB)`, which leaves CMake's own
`FindZLIB` to locate a *static* vcpkg zlib by guessing library names; it can
find the header, miss the library, and fail with `Could NOT find ZLIB (missing:
ZLIB_LIBRARY)` while reporting a version it read out of `zlib.h`. `core-ci.yml`
avoids this by configuring through the vcpkg toolchain file, which is exactly
what is unreachable from `ost build`.

### 4.1 The lane declares no pins of its own

A hand-authored lane beside a generated one is a place for two copies of a
digest to drift apart. `ost ci matrix` exists to prevent that — its own help
says it emits the resolved cells "so a workflow `ost ci generate` cannot express
can consume the same pins instead of copying them" — so the Windows lane reads
its runtime artifact, its OCI reference, its platform, and its profile out of
`openstrata.ci.yaml` at run time:

```sh
ost ci matrix --lane pull_request --json
```

That is what `workspace-graph-pr-windows` is for. The cell carries two things.
The thin one is real: the dependency graph is validated by a Windows `ost`,
where path separators, case folding, and `.strata/targets` naming are not the
same code path as on Linux. The load-bearing one is the pin — it is the reason
the Windows runtime is declared once, in the file that declares the other two,
rather than twice.

One field is still parsed by hand, and only one: `bootstrap.ost.version`, which
has to be read before `ost` exists to read it.

### 4.2 What the lane asserts, beyond a green suite

```sh
ost build --target cy2026 --profile usd
ost test  --target cy2026 --profile usd
grep -E "httpResolver_stage .*Passed" .ost-ci/ctest.log
```

The `grep` is the point. A suite that skipped `httpResolver_stage` — because the
bundle branch was off, or because the plugin failed to register and the test was
never generated — reports the same "all tests passed" as one that ran it. The
criterion is that a `UsdStage` opened over a socket on this platform, so the one
test the lane exists for is asserted by name, from the log `ctest` wrote. This
is the same discipline `core-ci.yml` applies when it asserts from the configure
log that OpenUSD was never reached.

### 4.3 Walked locally first

The whole cell shape was run on Windows before it was written down, with the
prefix supplied the way the lane supplies it:

```sh
CMAKE_PREFIX_PATH="C:/dev/vcpkg/installed/x64-windows-static-md" \
  ost build --target cy2026 --profile usd
#   Built target cy2026-windows-x86_64-py313-usd

CMAKE_PREFIX_PATH="…" ost test --target cy2026 --profile usd
#   21/21 Test #21: httpResolver_stage ...............   Passed    0.23 sec
#   100% tests passed out of 21
#   Tested target cy2026-windows-x86_64-py313-usd: 21 of 21 passed (Release)
```

So the hand-authored lane is a transcription of a walk that passed, not a guess
at a workflow.

## 5. Why the workspace cells carry the release's claim

A bundle cell's rungs open a path declared in `tests.smoke`, and a remote
fixture is not a file in a directory (report 02, §3). The claim `v0.2.0` exists
to make — a stage opened over HTTP, a relative reference followed to a second
remote layer, and a 4 KiB window out of 1 MiB with the `Range` header asserted
from the server's own request log — lives in `httpResolver_test_stage`, a CTest
executable that stands up `tests/fixture-server` on loopback with an ephemeral
port and stops it at the end.

`verify: test` is the cell shape that reaches it: `ost build` over the root
CMake tree, then `ost test` over its CTest suite. Nothing is hosted, no port is
reserved, and no cell needs network access beyond pulling its own runtime.

## 6. Upstream asks

Ask 1 and ask 2 from report 02 (a dispatch-shaped L2, and the empty failure
message) are unchanged and still open; §3.1 above is what living with them
costs. Ask 3 from report 02 — no way to reach a third-party dependency from
`ost … build` — is now the reason an entire lane is hand-authored rather than an
inconvenience in a shell, and it is restated here at that weight.

### Ask 4 — a cell that declares pins without being generated

`ost ci matrix` is the right tool for a workflow the generator cannot express,
and it works. What is missing is a way to say so in the matrix. Today a cell is
either generated into a job or absent, so a lane that must be hand-authored
either duplicates its pins or, as here, borrows a cheaper cell that happens to
name the same runtime.

The thin cell is defensible — a Windows `ost` really does validate the graph on
a different code path — but it is standing in for something the schema should be
able to state directly. Something in the shape of:

```yaml
  - name: workspace-pr-windows
    kind: workspace
    generate: false          # pins recorded; the job is hand-authored
    runner: windows-hosted
    runtime_artifact: sha256:…
```

would let the matrix stay the single declaration of every pin, and let
`ost ci plan` report honestly that one cell's job lives elsewhere — instead of a
reader having to notice that a `graph` cell on Windows is doing a second job.

## 7. When this changes

- `ost` gains a Windows host-package installer, or a `--cmake-arg` /
  `cmake.find_package_hints` passthrough: `plugin-windows-ci.yml` is deleted,
  `workspace-graph-pr-windows` becomes `workspace-pr-windows` with
  `verify: test`, and this report gains a successor.
- `ost` gains a dispatch-shaped L2: the two bundle cells go from `up_to: 1` to
  `up_to: 5` and pick up L3, L4, and L5.
- `ost ci` gains a non-generated cell: ask 4 above; the thin Windows graph cell
  becomes an honest declaration of the lane it is standing in for.
- A cell first runs on a real pull request: this report records local walks and
  a generated workflow. The first CI run is evidence this report does not yet
  have, and it belongs in a successor rather than in an edit here.
