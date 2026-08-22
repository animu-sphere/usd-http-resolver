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

**`v0.4.0` is released: a `UsdStage` opens over HTTP, a clustered read of a
remote asset costs three requests where it cost eighteen, and a process that
starts cold pays nothing for a window an earlier one fetched. The read contract,
the local backend, the shared boundary suite, the hostile-server corpus, the
HTTP backend, the `ArResolver` bundle, the block cache, identity exposure, and
the on-disk cache tier are in the tree and passing.**

That ordering is the point. `v0.1.0` shipped a local file reader, which is not
interesting; what was interesting is that it arrived with the harness that makes
every later transport cheap to verify and impossible to fake. An HTTP backend
written before that harness is an HTTP backend whose bugs are indistinguishable
from server behavior. `v0.2.0` cashed that: the HTTP backend passes the `v0.1.0`
boundary suite **unchanged**, against an independent oracle, and separately
against 18 hostile-server behaviors on a real socket.

It also made this project's first performance claim, and it is a counter on a
named fixture rather than a sentence: **a bounded query moved 324 KiB of a
128 MiB asset — 0.0025 of it — and every byte moved was a byte the caller asked
for.**

`v0.3.0` is the release that changes those numbers on purpose, and it changes
them in both directions. Seventeen clustered reads of a header and an index went
from 18 requests to 3; eight parallel readers went from 152 to 25 and now move
one reader's worth of bytes between them; the full sequential read did not move
at all. The bounded query's `selectivity` got *worse*, 0.0025 to 0.0112, because
alignment converts request count into transferred bytes, and 1191936 bytes of
what it moved are `bytesOverFetched` — reported beside the saving rather than
instead of it. Both records are counters on a named fixture:
[BASELINE.md](docs/reference/BASELINE.md) for what the shipped configuration
costs, and [BLOCK_POLICY.md](docs/reference/BLOCK_POLICY.md) for why it is that
configuration.

`v0.4.0` lets both of those outlive the thing that produced them, under one
rule: a `Strong` validator issued by the origin, and nothing weaker, may be
reused after the reader that captured it is gone. `GetAssetInfo` publishes an
asset's identity — a resolved identifier, a size, an opaque validation token,
and a stability class — so a consumer can decide whether *its own* generated
cache may be reused, and `ArAssetInfo::version` carries the token only when that
answer is yes. Under the same rule, blocks reach a disk: the bounded query that
costs 19 requests and 331776 bytes on a first open costs 1 request and 0 bytes
on a second, in a second process, and the one request is the metadata `HEAD`
that revalidates the identity the entries are keyed on. A weak or absent
validator is never written and never read back, because a guess that survives a
restart is exactly what an on-disk cache was held back until validators landed
in order not to become.

What the tree actually does is in
[docs/reference/CAPABILITY_MATRIX.md](docs/reference/CAPABILITY_MATRIX.md); what
each release shipped is in
[docs/releases/](docs/releases/README.md).

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
- **One reader, one revision.** An asset that changes underneath an open reader
  fails with `AssetChanged`. It never silently produces bytes from two
  revisions, and that guarantee ships with the first HTTP backend, not after
  the cache.
- **Measured, not asserted.** The claim is a ratio, so the ratio is a counter
  and a test assertion ([METRICS.md](docs/architecture/METRICS.md)).

## What this resolver does not give you

`ArAsset::Read` and `ArAsset::GetSize` are the whole surface. `GetBuffer()`
returns null, permanently and by contract: it asks for the entire asset in
memory, which is the exact transfer this project exists to avoid.

So the interoperability claim is bounded, and worth stating plainly:

> This resolver interoperates with random-access-compatible FileFormat Plugins.

A plugin that computes offsets and reads them streams a remote asset. A plugin
that requires whole-buffer access does not — not because it is broken, but
because it is asking for something else. It keeps working against local assets
through the primary resolver, which this bundle never changes. The details, and
the per-format compatibility matrix, are in §4 of
[docs/architecture/RESOLVER.md](docs/architecture/RESOLVER.md).

## First consumer

`usd-pointcloud-plugins` reads COPC over this resolver in `v0.5.0` — with no
HTTP code of its own, no build dependency, and no change to its COPC reader. If
it needs one, the abstraction leaked and the fix belongs here.

## Building

The build graph is libs-first: everything under `libs/` and `tests/` builds and
tests with no OpenUSD installation present, and OpenUSD is resolved only for the
plugin bundle. This is the path every release so far is defined by, and it is
the normal way to work on the read contract, the backends, and the boundary
suite. Since `v0.2.0` it needs libcurl, which is the only third-party dependency
this project has.

```sh
cmake -S . -B build-core -DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF
cmake --build build-core
ctest --test-dir build-core
```

On Windows, use `cmake --preset core-msvc` unless you are in a Visual Studio
developer command prompt: the Ninja presets need `cl.exe` on `PATH` already.

Under sanitizers, which are contract rather than an optional lane — clang or
GCC, since MSVC implements only `address`:

```sh
cmake --preset core-asan && cmake --build --preset core-asan && ctest --preset core-asan
cmake --preset core-tsan && cmake --build --preset core-tsan && ctest --preset core-tsan
```

Both lanes, and the core build on Windows, Linux, and macOS arm64, run on every
pull request in [`.github/workflows/core-ci.yml`](.github/workflows/core-ci.yml).

With `ost`, which resolves and composes a certified OpenUSD runtime:

```sh
ost runtime pull cy2026 --profile usd
ost build
ost test
```

See [docs/guides/BUILDING.md](docs/guides/BUILDING.md).

## License

Apache-2.0.
