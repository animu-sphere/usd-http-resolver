# usd-http-resolver documentation

Documentation is organized by responsibility, following the same taxonomy as
`usd-pointcloud-plugins`, so current contracts, procedures, plans, and
historical records do not drift into one another.

When a summary disagrees with the implementation, the implementation wins and
the summary is a documentation bug. When a summary disagrees with
[architecture/WORKSPACE.md](architecture/WORKSPACE.md) about structure, the
workspace contract wins; structural changes must update that contract first.

This repository is at `v0.2.0`, released 2026-08-20; its
[record](releases/v0.2.0.md) states what shipped and what did not. The read
contract, the local backend, the shared boundary suite, the hostile-server
corpus, the HTTP backend, and the `ArResolver` bundle are implemented, and a
`UsdStage` opens over HTTP; no cache is. What the tree actually contains is
stated in [reference/CAPABILITY_MATRIX.md](reference/CAPABILITY_MATRIX.md), and
what a bounded query costs is in
[reference/BASELINE.md](reference/BASELINE.md); everything else here is contract
and plan.

| Category | Answers | Start here |
| --- | --- | --- |
| [design/](design/) | Why the project is built this way, and what it refuses to do. | [DESIGN_POLICY.md](design/DESIGN_POLICY.md) |
| [architecture/](architecture/) | How the workspace is structured, which dependency directions are legal, and what each cross-cutting contract requires. | [WORKSPACE.md](architecture/WORKSPACE.md) |
| [reference/](reference/) | What the tree implements today and how it is configured. | [CAPABILITY_MATRIX.md](reference/CAPABILITY_MATRIX.md) |
| [guides/](guides/) | How to build and test the workspace. | [BUILDING.md](guides/BUILDING.md) |
| [compatibility/](compatibility/) | Which OpenUSD and OpenStrata versions are supported. | [OPENUSD.md](compatibility/OPENUSD.md) |
| [roadmap/](roadmap/) | What remains incomplete and in what order it lands. | [README.md](roadmap/README.md) |
| [adr/](adr/) | Numbered, immutable architecture decision records. | [0001-consumer-interface.md](adr/0001-consumer-interface.md) |
| [contributing/](contributing/) | Contributor procedures that a code change must satisfy. | [BOUNDARY_SUITE.md](contributing/BOUNDARY_SUITE.md), [MODULE_README_CONTRACT.md](contributing/MODULE_README_CONTRACT.md) |
| [releases/](releases/) | Immutable records for tagged releases. | [README.md](releases/README.md) |
| [reports/ost/](reports/ost/) | Append-only OpenStrata adoption and CI evidence. | [README.md](reports/ost/README.md) |

## Canonical documents

- [design/DESIGN_POLICY.md](design/DESIGN_POLICY.md) defines the product
  intent, the transport boundary, the consumer boundary, cache policy,
  consistency policy, diagnostics, thread safety, measurement, and licensing.
- [architecture/WORKSPACE.md](architecture/WORKSPACE.md) is the binding
  structural contract for modules, bundles, dependency directions, and artifact
  naming. A structural change updates it first.
- [architecture/ASSET_READER.md](architecture/ASSET_READER.md) fixes the
  internal random-access read contract that every backend implements and every
  layer above consumes.
- [architecture/RESOLVER.md](architecture/RESOLVER.md) fixes what the
  `ArResolver` bundle owns: URI normalization, relative resolution, asset
  identity, and the `ArAsset` surface handed to consumers.
- [architecture/CACHE.md](architecture/CACHE.md) fixes block caching, request
  coalescing, and validator-keyed cache identity.
- [architecture/DIAGNOSTICS.md](architecture/DIAGNOSTICS.md) defines the typed
  error vocabulary and its projection onto stable `HTTPxxx` plugin codes.
- [architecture/METRICS.md](architecture/METRICS.md) defines the I/O counters
  that make byte amplification and cache effectiveness observable.
- [contributing/BOUNDARY_SUITE.md](contributing/BOUNDARY_SUITE.md) fixes the
  shared correctness suite that every backend is admitted by, and that `v0.1.0`
  exists to produce.
- [reference/CAPABILITY_MATRIX.md](reference/CAPABILITY_MATRIX.md) describes
  what the current tree implements, not what it intends to implement later.
- [reference/BASELINE.md](reference/BASELINE.md) is the current recorded I/O
  baseline: the five scenarios METRICS.md §6 requires, the fixture they ran
  against, and what is asserted rather than merely reported. A release record
  copies it at its tag; it is rewritten whenever I/O behavior changes.

## The one-sentence contract

> `usd-http-resolver` connects the OpenUSD composition graph to remote
> random-access storage. It resolves asset paths and serves byte ranges. It
> never learns what a byte means.

The complementary half of that boundary is owned by the first consumer and
stated in its
[resolver-backed source contract](https://github.com/animu-sphere/usd-pointcloud-plugins/blob/main/docs/architecture/RESOLVER_SOURCE.md):
no consumer implements transport, and no consumer takes a build-time dependency
on this repository.
