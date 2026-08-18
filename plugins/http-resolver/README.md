# http-resolver

The OpenUSD `ArResolver` bundle for the `http` and `https` URI schemes, and the
only module in this repository that includes an OpenUSD header.

It is thin on purpose. Everything that decides what bytes to ask for and what a
response means is in `libs/usd-asset-http`, which was admitted by the shared
boundary suite before this bundle existed. What is here is registration, the
name of an asset, and the handover to `pxr::ArAsset`.

## Purpose

To let a consumer open a remote asset with no HTTP code of its own, no build
dependency on this repository, and no change to the consumer. A `UsdStage`
opened on `https://example.org/scenes/main.usda` composes its references over
range requests, and the consumer sees only `ArAsset::Read`.

The normative contract is [RESOLVER.md](../../docs/architecture/RESOLVER.md).
When this README and that document disagree, the document wins.

## Registered URI schemes

```json
"HttpResolver": {
    "bases": ["ArResolver"],
    "uriSchemes": ["http", "https"]
}
```

Two consequences that are contract rather than detail:

- **The host's primary resolver is unchanged.** This is a URI-scheme resolver,
  not the primary one. Local paths keep resolving exactly as they did, and
  installing this bundle never alters how a local asset opens. A relative path
  anchored to a *local* layer never reaches this code at all.
- **A scheme this bundle does not claim is not this bundle's concern.** `s3://`
  belongs to a future bundle, never to a special case inside this one.

`https` is the expected scheme; `http` exists for local fixture servers and
intranet hosts, and it is what this repository's own tests use.

## Resolution behavior

`CreateIdentifier` normalizes once, so that two spellings of one asset produce
one identifier, one entry, and one open:

| Rule | Example |
| --- | --- |
| scheme and host lowercased | `HTTPS://Example.ORG/A.usda` → `https://example.org/A.usda` |
| default port removed | `https://h:443/a` → `https://h/a` |
| dot segments resolved | `https://h/a/../b` → `https://h/b` |
| percent-encoding uppercased, unreserved decoded | `%7e` → `~` |
| characters that cannot appear literally encoded | `tree bark.png` → `tree%20bark.png` |
| query preserved **verbatim** | `?b=2&a=1` stays `?b=2&a=1` |
| fragment removed | never sent to a server |
| userinfo removed | `https://user:t@h/a` → `https://h/a` |

The query is never reordered because a pre-signed object-storage URL carries its
signature there, and sorting the parameters invalidates it. The userinfo is
removed because §4.3 of the
[design policy](../../docs/design/DESIGN_POLICY.md) keeps a credential out of the
resolver API, and an identifier *is* the resolver API — a URL that needs
credentials therefore fails at the origin with `HTTP002` rather than succeeding
with a secret in every log line. Authentication arrives as the interception
point in `v0.6.0`, not as a URL component.

Relative references anchor to the layer they were authored in, per RFC 3986
§5.2, which is what makes a remote scene work at all: a layer published to a CDN
references its neighbours relatively and none of those references mentions a
host.

```text
anchor:   https://example.org/scenes/shot_010/main.usda
asset:    ../../assets/tree.usda
result:   https://example.org/assets/tree.usda
```

`Resolve` performs one metadata request and **retains the reader it opened**, so
the `OpenAsset` that follows does not repeat the round trip. Two concurrent
resolutions of one identifier perform one request, not two. A reader is handed
out once: a second `OpenAsset` for the same identifier opens again rather than
sharing a reader that is already bound to a revision somebody else is
mid-composition on.

Absence and failure are different answers, and this is where a resolver most
often misleads:

| Condition | `Resolve` returns | Diagnostic |
| --- | --- | --- |
| `404` | empty path | none — absence is the answer, not a fault |
| timeout, reset, TLS failure, malformed response | empty path | `HTTP005`, `HTTP006`, `HTTP004`, … |

`GetExtension` ignores the query string. The default implementation would return
`usda?X-Amz-Signature=…` for a signed URL, which matches no file format — a
failure that presents as an unsupported format and is in fact a resolver bug.

## `ArAsset` behavior

```text
ArAsset::Read(buffer, count, offset)  ->  AssetReader::Read(offset, buffer, count)
ArAsset::GetSize()                    ->  AssetMetadata::size
ArAsset::GetBuffer()                  ->  nullptr, permanently
ArAsset::GetFileUnsafe()              ->  {nullptr, 0}
```

`GetBuffer` returning null is a compatibility contract and not an omission. It
asks for whole-asset materialization, and honouring it for a 10 GB remote asset
is exactly the transfer this project exists to avoid. A `SdfFileFormat` that
reads through `Read` at offsets it computes itself is unaffected; one that
requires a whole buffer is not remote-capable through this resolver, which
[RESOLVER.md §4.2](../../docs/architecture/RESOLVER.md) states as a bounded claim.

A short read at EOF returns the remainder and is success. A short read below EOF
is a failure, and a failed read returns **0**, not the bytes that had already
arrived: those bytes may span two revisions, and reporting them as read invites
the composition revision binding exists to prevent.

Writing is unsupported. `CreateIdentifierForNewAsset` and `ResolveForNewAsset`
return empty, `OpenAssetForWrite` returns null with `HTTP010`, and
`CanWriteAssetToPath` says why rather than failing silently.

`GetDetachedAsset` is inherited rather than overridden, and it is the one path
here that reads a whole asset: a host that turns detached layers on has asked
for the layer's bytes to be independent of the asset, and there is no way to
provide that remotely except by transferring them. It is off by default.

## Configuration

The five transport bounds in
[CONFIGURATION.md](../../docs/reference/CONFIGURATION.md), read once when the
resolver is constructed:

| Variable | Maps to | Default |
| --- | --- | --- |
| `USD_HTTP_RESOLVER_CONNECT_TIMEOUT_MS` | connect deadline | 10000 |
| `USD_HTTP_RESOLVER_READ_TIMEOUT_MS` | response deadline | 30000 |
| `USD_HTTP_RESOLVER_TOTAL_TIMEOUT_MS` | whole-transfer deadline | 300000 |
| `USD_HTTP_RESOLVER_MAX_RETRIES` | attempts, minus one | 2 |
| `USD_HTTP_RESOLVER_MAX_REDIRECTS` | redirect hops | 5 |

A value that does not parse is a warning at construction and then the default;
one bad value does not discard the other four. `0` is legal for the two counters
and means "do not", and is rejected for the three deadlines, because to most
transports a zero deadline means *no* deadline — the one value §10 of the design
policy exists to forbid.

Per-stage configuration through `ArResolverContext` is `v0.6.0`. A host that
opens two stages against two servers cannot be served by a process-global, and
that is the surface the environment variables are a bootstrap for.

## Plugin discovery and installation

The bundle is a directory, not a bare library:

```text
plugins/http-resolver/
  openstrata.plugin.yaml                             the bundle manifest
  plugin/resources/httpResolver/plugInfo.json.in     tracked; the source of truth
  plugin/resources/httpResolver/plugInfo.json        generated at configure time
  lib/libHttpResolver.{dll,so,dylib}                 built here, not in the build tree
```

`plugInfo.json`'s `LibraryPath` is relative to itself, which is why the library
is staged into the bundle rather than left in the build directory. Both the
generated `plugInfo.json` and the library are build products and neither is
tracked: `LibraryPath` names a different file on each platform, so a committed
copy is a file that every configure on another platform rewrites. Configure the
bundle — through CMake or `ost plugin build` — and both appear.

Composition is runtime-only:

```sh
export PXR_PLUGINPATH_NAME=/path/to/plugins/http-resolver/plugin/resources/httpResolver
usdcat https://example.org/scenes/main.usda
```

or, through OpenStrata, by pulling the bundle into a formation. No consumer
links this repository ([ADR-0001](../../docs/adr/0001-consumer-interface.md)).

## Diagnostics

The bundle owns the `HTTPxxx` codes and is the only thing that emits one. The
projection is total and one-way; the table is in
[DIAGNOSTICS.md §5](../../docs/architecture/DIAGNOSTICS.md) and is asserted in
`httpResolver_test_diagnostics` rather than restated here.

```text
HTTP004: partial response did not cover the requested range
         (offset 1258291, length 65536) [HTTP 206]
         https://example.org/data/survey.copc?<elided>
```

Every message goes through one rendering function, and that function elides the
query string and the userinfo. There is no path from a failure to a human that
can skip it.

Errors reach the host as `TF_RUNTIME_ERROR`, cancellation as `TF_WARN`, and a
caller's impossible request as `TF_CODING_ERROR`. `HTTP101` is a warning posted
when a request succeeded after being retried: it cost latency that is otherwise
invisible in a return value.

## Build and test

This is the only directory here that needs OpenUSD. Everything under `libs/`
builds and tests without it, and that path must keep working.

```sh
# the whole repository, bundle included
cmake -S . -B build/plugin -DCMAKE_PREFIX_PATH="<openusd-prefix>;<curl-prefix>"
cmake --build build/plugin
ctest --test-dir build/plugin

# the bundle alone, through ost, against the resolved runtime
ost plugin build plugins/http-resolver
```

On Windows outside a developer command prompt, use the `plugin-msvc` preset:
the Ninja presets need `cl.exe` already on `PATH`, and the Visual Studio
generator finds the toolchain itself.

Four tests. Three of them link one translation unit and nothing else — no
OpenUSD, no sockets — because identifier normalization, the configuration
surface, and the diagnostic projection are arithmetic, and a mistake in the
first is invisible from the outside:

| Test | Asserts |
| --- | --- |
| `httpResolver_identifier` | normalization, anchoring, what is not claimed, idempotence |
| `httpResolver_configuration` | the five variables, and what a bad value does |
| `httpResolver_diagnostics` | the `HTTPxxx` table, the message form, and that no secret survives |
| `httpResolver_stage` | a remote stage over a real socket, against the hostile fixture corpus |

The fourth is the release's claim: it stands up an origin on loopback, opens a
`UsdStage` over it, follows a relative reference to a second remote layer, reads
a 4 KiB window out of a 1 MiB asset and checks the `Range` header the server
actually received, and confirms that a `404` is silent, that a failure is not,
that range-unsupported is terminal, and that a local stage still opens exactly
as it did.

## Runtime dependencies

| Dependency | Kind | Notes |
| --- | --- | --- |
| OpenUSD `ar`, `tf`, `arch`, `js`, `plug`, `vt` | linked | Only the Ar surface. No `usd`, no `usdGeom`: a resolver reads bytes, it does not author stages |
| `usdasset::http` | linked, static | Carries `usdasset::io`, and libcurl privately |
| libcurl | **not** an edge of this bundle | Named in one translation unit inside the backend, linked `PRIVATE` and statically ([ADR-0003](../../docs/adr/0003-http-client-dependency.md)) |

The bundle links no HTTP client and includes no header that mentions one. That
is what makes a future `usdAssetWasm` a second implementation of one interface
rather than a second resolver.

## Licensing

Apache-2.0, as the repository. Third-party obligations belong to libcurl and are
recorded in [NOTICE](../../NOTICE); nothing in this bundle adds one.

## Known limitations

- **No caching.** Every read is a request, deliberately, so that the request
  pattern is visible before it is optimized. The block cache is `v0.3.0`, and it
  will key on the validator this release already captures.
- **Identity is not exposed.** `GetAssetInfo` and `GetModificationTimestamp` are
  the defaults. Validators are captured and used, but exposing them to a
  consumer turns a wrong validator into a durable wrong answer, so the surface
  opens in `v0.4.0` after capture has been correct for a release.
- **No authentication.** Public HTTP is the target. A URL carrying credentials
  has them removed from its identifier, and the request then fails at the origin
  rather than succeeding with a secret in the logs.
- **Range-unsupported is terminal.** No whole-asset fallback, per
  [ADR-0002](../../docs/adr/0002-range-unsupported-policy.md).
- **A metadata request is a `HEAD`.** A server that refuses `HEAD` is reported
  as `Unsupported` rather than guessed at; the corpus has no such row, and a
  fallback would ship unexercised.

## Compatibility

| Item | Value |
| --- | --- |
| OpenUSD | `>=26.08,<27.0` |
| C++ | 17 |
| Platforms | Windows x86_64, Linux x86_64, macOS arm64 |
| Bundle kind | `usd-asset-resolver` |
| Provides | `usd-resolver:http`, `usd-resolver:https` |

A 27.x runtime is outside the declared range; raising the bound means rebuilding
and re-running these tests against it. The range matches the first consumer's
deliberately, so that a mismatch is found at composition time rather than at
load time.
