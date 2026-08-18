# OST 03: a support matrix, and the one lane it could not express

| Field | Value |
| --- | --- |
| Date | 2026-08-18 |
| Subject | `openstrata.ci.yaml` lands; which rungs its cells run, which lanes stay outside it, and what the first runs found |
| `ost` version | 0.22.2 for every local walk; **0.21.0 pinned in CI**, for the reason in §4 |
| Runtimes pinned | `openstrata-runtime-cy2026-usd`, OpenUSD 26.08, on Linux, macOS arm64, and Windows |
| Platform (local walk) | Windows 11 (26200), MSVC 19.34, CMake 4.4, Ninja |
| Result | Six cells generated; one lane hand-authored; two rung caps recorded; three defects found by contact with CI and fixed; five upstream asks, two of them new |

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

Not because of a rung, but because the build cannot start; §5.

## 4. What contact with CI found

Three things, none of which a local walk could have found, and each fixed in the
place it belongs.

### 4.1 A version skew, and the pin moved down

Everything in §1 through §3 was walked locally with `ost` 0.22.2 and passed. The
first run on hosted runners failed all four cells that materialize a runtime,
in the same step, in under 40 seconds:

```text
ost runtime validate cy2026 --profile usd --json
  [FAIL] manifest-schema   — manifest schema 3 != expected 7
  [FAIL] digest-integrity  — recomputed sha256:f75ed71a… != stored sha256:552de36f…
Result: FAILED                                            (exit 5)
```

The two graph cells passed, because a `verify: graph` job stops after the
checkout and never materializes anything — which is the first time that
property has been worth anything.

This is not a CI fact. It reproduces on a developer machine, against a runtime
that had already built and tested this workspace an hour earlier:

```sh
ost --version                          # ost 0.22.2
ost runtime validate cy2026 --profile usd
#   [FAIL] manifest-schema — manifest schema 3 != expected 7
#   Result: FAILED
```

`ost artifact show` names the cause:

```text
  producer:    ost 0.20.0
  imported by: ost 0.20.0
```

The published `openstrata-runtime-cy2026-usd` artifacts on all three platforms
were built by `ost` 0.20.0 and carry runtime manifest schema 3. `ost` 0.22.2's
`runtime validate` requires schema 7, and it is a step every generated cell runs
before it builds anything. `usd-vrm-plugins` pins the same three digests and its
`ost source ci` is green, which was the clue: it bootstraps 0.21.0.

The fix is `bootstrap.ost.version: "0.21.0"` — the newest CLI that accepts these
runtimes — checked rather than assumed, against the same materialized runtime:

```sh
ost-0.21.0 runtime validate cy2026 --profile usd
#   [ok  ] manifest-schema
#   [ok  ] digest-integrity
#   Result: passed
```

Nothing else in the matrix changed. 0.21.0 accepts the file unchanged
(`6 cell(s), structure OK`), its `ci matrix --lane pull_request --json` emits the
same fields the Windows lane reads, and every `ost` invocation the generated
workflow makes exists in 0.21.0 with the same flags — `artifact pull
--expect-artifact --require-kind`, `artifact verify --minimum-trust
--require-sbom --require-provenance`, `runtime pull --from-artifact`,
`plugin test --workspace --graph-only`, `ci matrix`.

The workflow is regenerated **by 0.21.0**, so the YAML and the CLI that runs it
are the same version. The only difference in the output is the registry cache:
0.21.0 emits a single `actions/cache` step where 0.22.2 emits a
`restore`/`save` pair. The hand-authored Windows lane keeps the split, which is
its own code and version-independent.

What this costs: local development stays on 0.22.2 and CI runs 0.21.0, so the
two can disagree again. What makes that survivable is that the disagreement is
observable in one command — `ost runtime validate` — and the pin is one line
with the reason attached to it.

#### Ask 5 — a runtime that a current `ost` will not validate

The artifacts are fine: `ost artifact verify --require-sbom
--require-provenance` passes on all three, in CI, under 0.22.2. What fails is
the materialized runtime's manifest schema, and there is no path forward from
inside a consumer repository — a workspace cannot re-produce a runtime it
consumes.

Either resolves it:

- republish the `cy2026`/`usd` runtimes with a current `ost`, so the newest CLI
  and the newest artifacts agree;
- or have `ost runtime validate` accept an older manifest schema it can still
  read, and say which CLI produced it, so the failure names the fix instead of
  reporting `3 != 7`.

The second is the more valuable of the two, because the first is a treadmill:
every CLI release that bumps the schema strands every published runtime until
someone rebuilds it.

### 4.2 `host_python` is not only about schema tooling

With the pin fixed, the Linux workspace cell got as far as its own test suite
and failed one test out of twenty-one:

```text
21/21 Test #21: httpResolver_stage ...............***Failed    0.00 sec
  httpResolver_test_stage: error while loading shared libraries:
  libpython3.13.so.1.0: cannot open shared object file: No such file or directory
```

`0.00 sec` is the tell: the process died before `main`. `httpResolver_stage` is
the only test here that links OpenUSD, and therefore the only one that links
Python; the other twenty link `libs/` and a socket and passed.

The cell had no `host_python`, and that was a considered omission — the field is
documented for a runtime that ships no interpreter but whose profile still needs
`usdGenSchema`, and this workspace generates no schema. That reasoning was about
*build* time and the failure is at *run* time. The step `host_python` renders is
a pinned `actions/setup-python`, which on Linux also puts a
`libpython3.13.so.1.0` on the loader path. `usd-vrm-plugins` declares it on
every Linux and macOS cell, which is the second time in this report that reading
the neighbouring workspace was faster than reading the documentation.

Declared on both `verify: test` cells now. macOS never needed it — it resolved
Python through the runtime's own rpath and passed — and it is declared there
anyway, because a lane that passes for a reason its neighbour does not share is
a lane that breaks on a runner image change and takes an afternoon to explain.

### 4.3 The ZLIB failure `core-ci.yml` predicted, in the lane that cannot use its fix

The Windows lane failed at configure, and the failure was already written down
in this repository — in the comment on `core-ci.yml`'s configure step, which
explains why that lane uses the vcpkg toolchain file and not a bare prefix:

```text
CMake Error at FindPackageHandleStandardArgs.cmake:233 (message):
  Could NOT find ZLIB (missing: ZLIB_LIBRARY) (found suitable version "1.3.2",
  minimum required is "1")
Call Stack (most recent call first):
  C:/vcpkg/installed/x64-windows-static-md/share/curl/CURLConfig.cmake:80 (find_dependency)
  libs/usd-asset-http/CMakeLists.txt:60 (find_package)
```

`CMAKE_PREFIX_PATH` worked — `CURLConfig.cmake` was found under the vcpkg
prefix, which is the whole reason the trace reaches `find_dependency(ZLIB)`.
What did not work is CMake's own `FindZLIB` guessing the name of a *static*
vcpkg zlib: it finds `zlib.h`, reports the version it read out of it, and misses
the library. `core-ci.yml` avoids this by configuring through the vcpkg
toolchain file, which activates vcpkg's wrapper and supplies the release and
debug paths explicitly — and that file is exactly what `ost build` will not
accept.

`ZLIB_ROOT` was already exported and was not sufficient, and neither were
`CMAKE_LIBRARY_PATH` and `CMAKE_INCLUDE_PATH` when they were added. None of them
could be, and the reason took a directory listing rather than an argument:

```text
$ ls C:/vcpkg/installed/x64-windows-static-md/lib
libcurl.lib
pkgconfig
zs.lib
```

vcpkg's zlib port installs its release library as **`zs.lib`**. CMake's
`FindZLIB` searches for a library named one of `z`, `zlib`, `zdll`, `zlib1`,
`zlibstatic`, `zlibwapi`. `zs` is not among them, and no amount of pointing the
search at the right directory helps when the name it is looking for is not
there. vcpkg solves this with a wrapper that supplies the release and debug
paths explicitly, activated by its toolchain file — which is what `core-ci.yml`
uses, and what `ost build` will not accept.

So the lane gives the file the name the module looks for: `zs.lib` is copied to
`zlib.lib` (and `zsd.lib` to `zlibd.lib`), conditional on the canonical name
being absent, so the day vcpkg renames the output back this step stops doing
anything rather than starting to do something wrong. It copies bytes and changes
nothing about what is linked — the same static zlib either way.

The diagnostic that found it is worth its own sentence, because the first
version of it did not. It grepped the listing for `zlib|curl` and printed one
line, `libcurl.lib`, which hid the file that was the entire problem — a listing
that only shows what you already suspected is not a listing. It prints the
directory unfiltered now.

This is ask 3 from report 02 collecting interest. Every one of these is a
consequence of a build argument that is real and cannot be expressed.

## 5. The lane `ost` could not express

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

### 5.1 The lane declares no pins of its own

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

### 5.2 What the lane asserts, beyond a green suite

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

### 5.3 Walked locally first

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

## 6. Why the workspace cells carry the release's claim

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

## 7. Upstream asks

Ask 1 and ask 2 from report 02 (a dispatch-shaped L2, and the empty failure
message) are unchanged and still open; §3.1 above is what living with them
costs. Ask 3 from report 02 — no way to reach a third-party dependency from
`ost … build` — is now the reason an entire lane is hand-authored rather than an
inconvenience in a shell, and it is restated here at that weight. Ask 5 — a
published runtime a current `ost` will not validate — is stated where it was
found, in §4.1.

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

## 8. When this changes

- `ost` gains a Windows host-package installer, or a `--cmake-arg` /
  `cmake.find_package_hints` passthrough: `plugin-windows-ci.yml` is deleted,
  `workspace-graph-pr-windows` becomes `workspace-pr-windows` with
  `verify: test`, and this report gains a successor.
- `ost` gains a dispatch-shaped L2: the two bundle cells go from `up_to: 1` to
  `up_to: 5` and pick up L3, L4, and L5.
- `ost ci` gains a non-generated cell: ask 4 above; the thin Windows graph cell
  becomes an honest declaration of the lane it is standing in for.
- The `cy2026`/`usd` runtimes are republished by a current `ost`, or
  `runtime validate` learns to read an older manifest schema: ask 5 in §4.1;
  `bootstrap.ost.version` moves back up from 0.21.0 and the workflow is
  regenerated by the version it pins.
- A cell runs green on a real pull request. This report records local walks, a
  generated workflow, and one CI run that failed for a reason outside this
  repository. A fully green matrix is evidence it does not have yet, and it
  belongs in a successor rather than in an edit here.
