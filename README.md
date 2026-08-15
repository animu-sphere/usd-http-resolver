# usd-http-resolver

An OpenUSD asset resolver for `http://` and `https://`, and the I/O substrate
beneath it.

The goal is not to make URLs openable. It is to give OpenUSD **remote random
access**: a FileFormat Plugin asks for the bytes it needs, and gets them out of
a large remote asset without downloading the asset and without knowing how they
arrived.

```text
USD Stage / Hydra
        |
        v
USD Composition                     logical structure
        |
        v
FileFormat Plugin                   "I need bytes [o, o+n)"
        |
        v
usd-http-resolver                   byte ranges, cache, consistency
        |
        v
HTTP / object storage / CDN         blocks
```

A 10 GB point cloud on a static HTTP server should cost a 64 KB header, a 1 MB
index, and the chunks actually in view — not 10 GB.

## Status

**Pre-implementation.** The repository contains the OpenStrata project root and
a complete documentation set. There is no resolver, no backend, and no cache
yet.

The contracts were written first on purpose: this project's product is a
boundary between repositories, and a boundary is cheaper to settle in a
document than across five consumers. What the tree actually does is in
[docs/reference/CAPABILITY_MATRIX.md](docs/reference/CAPABILITY_MATRIX.md).

## Start here

| If you want to know | Read |
| --- | --- |
| Why this project exists and what it refuses to do | [docs/design/DESIGN_POLICY.md](docs/design/DESIGN_POLICY.md) |
| What lands, in what order | [docs/roadmap/README.md](docs/roadmap/README.md) |
| How the modules are split | [docs/architecture/WORKSPACE.md](docs/architecture/WORKSPACE.md) |
| How a consumer integrates | [docs/roadmap/consumer-integration.md](docs/roadmap/consumer-integration.md) |
| Everything else | [docs/README.md](docs/README.md) |

## Design commitments

- **No format knowledge.** This project never parses an asset. Bytes are
  opaque.
- **No special server.** Static hosting, object storage, or a CDN that honors
  `Range` is the entire requirement. No sidecar index, no protocol.
- **No consumer coupling.** A FileFormat Plugin sees `pxr::ArAsset` and nothing
  else. No consumer links this repository, in either direction
  ([ADR-0001](docs/adr/0001-consumer-interface.md)).
- **Correctness before speed.** The local backend is the oracle; a remote read
  is correct when it is byte-equivalent to a local one at every boundary.
- **Measured, not asserted.** The claim is a ratio, so the ratio is a counter
  and a test assertion ([METRICS.md](docs/architecture/METRICS.md)).

## First consumer

`usd-pointcloud-plugins` reads COPC over this resolver in `v0.5.0` — with no
HTTP code of its own, no build dependency, and no change to its COPC reader. If
it needs one, the abstraction leaked and the fix belongs here.

## Building

Nothing to build yet. The workflow the first modules land into is in
[docs/guides/BUILDING.md](docs/guides/BUILDING.md).

```sh
ost runtime pull cy2026 --profile usd
ost build
ost test
```

## License

Apache-2.0.
