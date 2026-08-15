# ADR-0001: The consumer interface is `ArAsset`, not a project C++ API

## Status

Accepted (2026-08-16).

## Context

The founding strategy for this project sketches a "Remote Asset Access API":

```cpp
AssetHandle OpenAsset(const std::string& assetPath);
uint64_t    GetSize(const AssetHandle&);
size_t      Read(const AssetHandle&, uint64_t offset, size_t size, void* dst);
```

Read one way, that is an internal contract between this repository's layers.
Read another way, it is a public SDK that FileFormat Plugins link against. The
two readings produce very different projects, and the difference has to be
settled before the first consumer integrates, because it determines whether
consumers acquire a build-time dependency on this repository.

The first consumer has already fixed its side of the boundary. Its
resolver-backed source contract states that no build-time dependency on any
resolver implementation exists — "no CMake dependency, submodule, vendored
transport library, resolver-specific include, or link dependency" — and that
resolvers are runtime composition. It consumes byte access through `ArAsset`
and adapts that to its own internal source interface.

So the question is not what would be pleasant to publish. It is whether this
repository publishes a second, redundant integration path that its own first
consumer has already forbidden itself from using.

## Options

### A. Publish the C++ API as the consumer interface

Consumers link `usdAssetIo` and open assets directly.

- Gives a consumer a richer surface than `ArAsset`: explicit prefetch,
  metadata, statistics, and cancellation.
- Makes every consumer depend on this repository at build time, at a pinned
  version, on every platform. Five consumers means five version matrices.
- Bypasses OpenUSD asset resolution entirely. A consumer that calls this API
  directly stops working when the asset is local, packaged, or served by a
  different resolver — the exact generality the project exists to provide.
- Contradicts the first consumer's own contract, which would have to be
  rewritten to permit it.

### B. `ArAsset` is the only consumer interface

Consumers see `pxr::ArAsset::Read(buffer, count, offset)`. The C++ contract
stays internal to this repository, between the bundle, the cache, and the
backends.

- One integration surface, already standardized, already implemented by every
  other resolver. A consumer written against it works with a local file, a
  package, this resolver, or a resolver nobody has written yet.
- Zero build-time coupling, in either direction. Composition is
  `PXR_PLUGINPATH_NAME` or an OpenStrata formation.
- The internal contract stays free to change, because nothing outside this
  repository depends on it.
- Loses expressiveness: `ArAsset` has no prefetch, no cancellation, no
  statistics, and no per-read policy. A consumer that genuinely needs those has
  no sanctioned route.

### C. `ArAsset` primarily, with an opt-in public API later

B, plus a documented escape hatch for a consumer that demonstrably cannot be
served by `ArAsset`.

- Keeps the default correct while admitting that `ArAsset` may prove too
  narrow — prefetch is the plausible case.
- Risks the escape hatch becoming the norm through convenience rather than
  necessity.

## Decision

**B, with C's door explicitly closed until a concrete case opens it.**

The consumer interface is `pxr::ArAsset` and nothing else. No consumer includes
a header from this repository, links a library from it, or names it in CMake.

The internal `AssetReader` contract described in
[ASSET_READER.md](../architecture/ASSET_READER.md) is an implementation
contract between this repository's own layers. Publishing it is a separate
decision that requires a named consumer with a need `ArAsset` provably cannot
meet — not a consumer that would merely find it more convenient.

The reasoning that settles it: this project's value proposition is that a
FileFormat Plugin does not know how its bytes arrived. A public C++ API is a
way for a plugin to find out. Every consumer that takes it becomes coupled to
HTTP-shaped assumptions again, one layer lower, which is precisely the failure
the architecture was designed to prevent.

## Consequences

- The public surface of this repository is a plugin bundle and a URI scheme.
  There is no SDK, no exported headers, and no consumer-facing package.
- `ArAsset::GetBuffer()` returns null, because honoring it would defeat range
  access. Consumers must read ranges. See
  [RESOLVER.md](../architecture/RESOLVER.md).
- Capabilities that do not fit `ArAsset` — identity stability, in particular —
  are exposed through `ArResolver::GetAssetInfo` rather than through a
  side-channel API.
- Prefetch has no delivery route today. That is a known, accepted limitation,
  and it is the most likely reason this ADR is superseded.
- Integration testing with a consumer is runtime composition, in its own CI
  lane, and is never a gate on this repository's own test suite.
- The reciprocal rule lives in the consumer: this repository must equally never
  depend on a consumer, for tests or fixtures.

## Open questions

1. If `usd-3dgs-plugins` needs camera-driven prefetch, is the answer an
   `ArResolver` extension, an `ArResolverContext` hint, or a superseding ADR?
2. Does identity stability belong in `GetAssetInfo`, or does OpenUSD's
   asset-info surface prove too weak to carry it?
