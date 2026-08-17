# OST 02: the first resolver bundle under the verification pyramid

| Field | Value |
| --- | --- |
| Date | 2026-08-18 |
| Subject | `plugins/http-resolver` under `ost plugin build`, `inspect`, `doctor`, and `test` |
| `ost` version | 0.22.2 |
| Runtime | `openstrata-cy2026-windows-x86_64-py313-usd`, OpenUSD 26.08, artifact source, validation passed |
| Platform | Windows 11 (26200), MSVC 19.34, CMake 4.4, Ninja |
| Bundle kind | `usd-asset-resolver` — the first in this repository, and the first this project has taken through the pyramid |
| Result | Bundle builds and runs; L0, L1, L3, L4, L5 pass; **L2 fails structurally**; two repository-side findings; three upstream asks |

The [reports README](README.md) predicted this report before the bundle
existed:

> Whether the verification pyramid's Level 4 and Level 6 checks express "a
> resolver loaded and claimed its URI schemes" as naturally as they express "a
> file format opened a fixture" is an open question, and the answer belongs in
> the first report rather than in a design document.

The answer is: Level 3, 4, and 5 express it fine, because what they open is a
*local* fixture and the property being asserted there is that the host's
resolution is undisturbed. **Level 2 does not**, and the reason is worth stating
precisely, because it is not a bug in either side.

## 1. What was run

```sh
ost plugin inspect plugins/http-resolver
ost plugin doctor  plugins/http-resolver
CMAKE_PREFIX_PATH="C:/dev/vcpkg/installed/x64-windows-static-md" \
  ost plugin build plugins/http-resolver
ost plugin test    plugins/http-resolver
```

`inspect` and `doctor` pass with no findings: 6 pass / 2 skip and 8 pass /
8 skip respectively, the skips being the rungs that need a session.

`build` composes the MSVC environment, resolves the library closure
(`usdAssetIo` 0.1.0 then `usdAssetHttp` 0.2.0) into the workspace prefix,
configures the bundle against the runtime, and links
`lib/libHttpResolver.dll`. The bundle's three offline tests build with it; the
end-to-end stage test does not, and is not meant to — it needs
`tests/fixture-server`, which a standalone bundle configure has no access to,
and the bundle's `tests/CMakeLists.txt` returns early rather than failing.

`test` produces 11 pass / 1 fail / 4 skip:

```text
[PASS] L0 bundle.manifest             http-resolver 0.2.0 (usd-asset-resolver)
[PASS] L0 bundle.plug_info            valid JSON
[PASS] L0 bundle.plug_info.library_path  points under bundle lib/ (target windows)
[PASS] L0 plugin.shared_library       found lib/libHttpResolver.dll
[PASS] L0 bundle.fixtures             1 fixture(s) present
[PASS] L0 session.plugin_path         PXR_PLUGINPATH_NAME would include the bundle
[PASS] L1 runtime.source              artifact (reproducible)
[PASS] L1 runtime.openusd.version     26.08 satisfies '>=26.08,<27.0'
[FAIL] L2 resolver.registration       resolver registration or dispatch failed:
[PASS] L3 usdcat.read                 usdcat read 'local_unchanged.usda' and emitted USDA
[PASS] L4 python.stage_open           Usd.Stage.Open() opened the fixture
[PASS] L5 golden.roundtrip            flattened output matches the golden
```

## 2. L2 asserts resolution, and a network resolver cannot satisfy it

The failure message is empty, which is what sent this investigation into the
`ost` binary rather than into the bundle. The Level 2 probe is:

```python
import sys
from pxr import Ar
p = Ar.GetResolver().Resolve("<scheme>:<fixture path>")
sys.exit(0 if p else 7)
```

and its pass message is "USD dispatched `<scheme>:` to the resolver and resolved
the fixture".

That check models the resolver the `usd-asset-resolver` template scaffolds,
which strips its scheme and treats the remainder as a path on the local
filesystem. For that skeleton, `Resolve("https:C:/…/basic.usda")` returns a
non-empty path and the rung passes.

A real network resolver cannot pass it, and should not:

- `https:C:/…/local_unchanged.usda` normalizes to nothing this bundle claims —
  it has no authority, so it is not an absolute `http` or `https` URI, and
  RESOLVER.md §2.1 makes that an empty identifier rather than a guess;
- even a well-formed `https://host/a.usda` would return an empty path unless an
  origin were listening, because `Resolve` returning non-empty *asserts that the
  asset exists* (RESOLVER.md §2.3). Making the rung pass would mean either
  making resolution lie or hosting a server for the duration of the check.

The distinction the rung wants is **dispatch**: did USD route this scheme to
this bundle's resolver? That is observable without a network. Inside the same
session the bundle is registered and dispatching correctly:

```sh
ost plugin run plugins/http-resolver -- python probe.py
```

```text
derived: ['HttpResolver', 'ArDefaultResolver']
https://example.org/a.usda -> 'https://example.org/a.usda'
http://example.org/a.usda  -> 'http://example.org/a.usda'
ext: 'usda'
```

Both schemes reach `HttpResolver::_CreateIdentifier`, the type is registered
under `ArResolver`, and the overridden `GetExtension` answers through a query
string. The bundle is fine; the rung is asking a different question than the one
it names.

The L2 failure is therefore **recorded and not worked around**. The alternatives
were to add a local-file branch to the resolver so that a synthetic
`https:<path>` resolves — which would make the plugin change how local assets
open, the one thing RESOLVER.md §1 forbids — or to declare a fixture that is a
URL, which the fixture checks at L0 would then fail to find on disk. Neither
trade is worth a green rung.

## 3. Fixture hosting, the second predicted subject

The README's second prediction was fixture hosting: "Every other bundle kind
tests against a file on disk; this one needs a server."

The answer this release takes is that the origin is stood up **by the test**,
not by the harness. `httpResolver_test_stage` starts `tests/fixture-server` on
loopback with an ephemeral port, serves the layers it is about to open, and
stops it at the end. Nothing is hosted, no port is reserved, and CI needs no
network access.

That works because the test is a CTest executable in the repository's own build,
where the fixture server exists. It does **not** compose into `ost plugin test`,
whose rungs open a path from `tests.smoke`. So the bundle's manifest declares one
fixture, and it is a *local* stage:

```yaml
tests:
  smoke:
    - tests/fixtures/local_unchanged.usda
```

which looks like it tests nothing and tests the property this bundle is most
likely to break — that registering a URI-scheme resolver does not disturb local
resolution. L3, L4, and L5 pass on it, which means `usdcat`, `Usd.Stage.Open`,
and a flatten round-trip all behaved identically with the bundle loaded. That is
a real assertion, and it is the one an on-disk fixture can carry for a resolver
of this kind.

The remote claim stays in `ctest`, where the server can exist.

## 4. Repository-side findings

### 4.1 `usdAssetHttpConfig.cmake` did not find its own private dependency

The bundle is the first thing to consume the *installed* `usdAssetHttp` package
rather than the in-tree target, and it failed at generate time:

```text
The link interface of target "usdasset::http" contains: CURL::libcurl
but the target was not found.
```

The config file asserted, in a comment, that libcurl "is deliberately absent
from this file … not part of this package's usage requirements — which is what
'the dependency is private' means in practice". That is true for a shared
library and false for a static one: CMake records a static library's `PRIVATE`
link libraries as `$<LINK_ONLY:CURL::libcurl>` in the exported interface,
because a static archive carries no code for its dependencies and whoever links
last must supply them.

Fixed by `find_dependency(CURL)` in the config, with the comment corrected
rather than deleted. ADR-0003's actual claim — no installed header includes
`curl.h`, and no consumer inherits curl's include directories or compile
definitions — is unaffected and still holds.

Nothing in the in-tree build could have caught this: there, `CURL::libcurl` is
already defined by `libs/usd-asset-http`'s own `find_package`.

### 4.2 A golden carries the generating machine's absolute path

`usdcat --flatten` writes `doc = "Generated from Composed Stage of root layer
<abs path>"` into the flattened layer, so the committed golden contains a path
from the machine that generated it. `ost`'s comparison normalizes it away and
the rung passes, but the file is not machine-independent on its face, and
deleting the block by hand fails the comparison — the normalizer neutralizes the
path inside the header rather than removing the header.

The fixture's own prose was moved from a `doc` field into `#` comments so that
at least the *authored* text does not appear in the golden, where editing a
paragraph would otherwise change a test artifact.

## 5. Upstream asks for OpenStrata

### Ask 1 — L2 for `usd-asset-resolver` should assert dispatch, not resolution

`Resolve` returning non-empty is an existence assertion. For every resolver
whose assets are not on the local filesystem it requires a live backing store,
which the rung cannot provide and should not need. A check that is satisfiable
by every resolver of this kind would be one of:

- `Ar.GetResolver().CreateIdentifier("<scheme>://host/a.usd")` returns non-empty
  — normalization is pure, needs no I/O, and returning empty is exactly how a
  resolver says "not mine";
- the registry route: a type derived from `ArResolver` whose `uriSchemes`
  contain the declared scheme is registered after the session's plugin path is
  applied.

Either distinguishes "the bundle loaded and USD routed the scheme to it" from
"the asset was there", which are different failures with different fixes.

### Ask 2 — the failure message is empty

`resolver registration or dispatch failed: ` with nothing after the colon gives
a bundle author nothing to act on; the probe's exit code (7) and its stderr are
both dropped. Surfacing the probe script, or at least its output, would have
turned a binary-strings investigation into a one-line diagnosis.

### Ask 3 — no way to reach a third-party dependency from `ost … build`

`ost plugin build` and `ost library build` accept no `-D` passthrough and no
prefix argument. `libs/usd-asset-http` resolves libcurl with `find_package(CURL)`
— deliberately, per ADR-0003, and it is not an `ost` library edge because
libcurl is not a workspace member — so on Windows, where libcurl comes from
vcpkg rather than from a system path, the configure fails with
`Could NOT find CURL`.

The workaround is to set `CMAKE_PREFIX_PATH` in the environment before invoking
`ost`, which CMake honours and the composed session does not overwrite:

```sh
CMAKE_PREFIX_PATH="C:/dev/vcpkg/installed/x64-windows-static-md" \
  ost plugin build plugins/http-resolver
```

It works, and it is invisible: nothing in the descriptor records that this
library needs a prefix, so the next person meets the same failure. A
`--cmake-arg`/`-D` passthrough, or a `cmake.find_package_hints` field in
`openstrata.library.yaml`, would make it a property of the workspace rather than
of a shell.

This is adjacent to blocking item 2 in
[implementation status](../../roadmap/implementation-status.md) — that
`ost library build` cannot resolve a library-to-library edge on its own — and it
is a different problem with the same shape: the closure `ost` builds is real,
and the arguments it needs to build it cannot be expressed.

## 6. What is not blocked

Nothing in `v0.2.0`. The bundle builds through plain CMake and through `ost`,
the release's actual claim is asserted in `ctest` where a server can exist, and
the L2 rung's failure is a disagreement about what the rung means rather than a
defect in what shipped. `openstrata.ci.yaml` can name this bundle, which is what
phase 2 was waiting for; whether its generated cells run `ost plugin test` with
a known-failing rung, or run `ctest` instead, is the decision that report will
record.

## 7. When this changes

- `ost` gains a dispatch-shaped L2 for `usd-asset-resolver`: re-run and record
  the result in a new report.
- `ost` gains a way to pass build arguments: the `CMAKE_PREFIX_PATH` workaround
  above is removed from this repository's instructions.
- `openstrata.ci.yaml` lands: a report records which rungs the cells run and
  which are excluded, with the reason attached to each exclusion rather than to
  the file.
