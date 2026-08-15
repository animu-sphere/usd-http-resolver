# OpenUSD compatibility

This document states which runtimes this project targets and what it requires
from a host application.

Status: no bundle exists yet. The values below are the declared target, which
the first bundle's manifest is required to match.

## Declared contract

The plugin manifest will declare:

```yaml
runtime:
  openusd: ">=26.08,<27.0"
```

| Item | Value |
| --- | --- |
| Target OpenUSD version | 26.08 |
| Accepted range | `>=26.08,<27.0` |
| OpenStrata CLI | 0.22.2 |
| OpenStrata platform / profile | `cy2026` / `usd` |
| C++ standard | C++17 |
| CMake | 3.23 or newer |

The range matches the first consumer's, deliberately. Two bundles composed into
one runtime must agree on it, and a mismatch is discovered at composition time
rather than at load time.

A 27.x runtime is outside the declared range. Raising the upper bound requires
rebuilding and re-running the integration tests against that runtime.

## Bundle kind

```yaml
plugin:
  kind: usd-asset-resolver
provides:
  - usd-resolver:http
  - usd-resolver:https
requires:
  capabilities: [usd-stage-read]
```

## Target platforms

| Platform | Runner | Coverage |
| --- | --- | --- |
| Windows x86_64 | `windows-2022` | Bundle build and integration tests |
| Linux x86_64 | `ubuntu-24.04` | Bundle build and integration tests |
| macOS arm64 | `macos-15` | Bundle build and integration tests |

Everything under `libs/` builds and tests with plain CMake and **no OpenUSD
runtime**. Only `plugins/http-resolver` requires one. This is a structural
invariant, not a convenience: a range-read test that needs a USD runtime is a
test that gets skipped.

## OpenUSD surface used

The dependency is deliberately narrow — three libraries and a handful of types:

- `ArResolver` and `ArDefineResolver` for URI-scheme registration
- `ArResolvedPath`, `ArAsset`, `ArAssetInfo`, `ArTimestamp`
- `ArResolverContext` for per-stage configuration (`v0.6.0`)
- `TfType` registration through `TF_REGISTRY_FUNCTION`, and `TfDiagnostic` for
  diagnostic projection
- `ArchGetFileLength` and related `arch` facilities in the local backend

Linked components: `arch`, `tf`, `ar`. Notably absent: `sdf`, `usd`,
`usdGeom`, and `hd`. This resolver authors nothing and reads no scene
description; if a scene-description header appears in an include list, the
boundary has moved.

## Host expectations

- The host's primary resolver is unchanged. Installing this bundle never alters
  how a local asset resolves.
- Composition is `PXR_PLUGINPATH_NAME` or an OpenStrata formation. No host
  rebuild, no consumer rebuild.
- The bundle is thread-safe under concurrent Hydra access, without a global
  lock.

## OpenStrata compatibility

| Item | Value |
| --- | --- |
| Project template | `usd-plugin-workspace` 1.1.0 |
| Manifest schema | `openstrata.plugin/v1alpha1` |
| CI contract | `openstrata.ci.yaml`, workflows generated with `ost ci generate github` |

Workflow YAML is generated, never hand-edited. A host requirement is declared
in `openstrata.ci.yaml` so that regeneration re-renders it instead of dropping
it.
