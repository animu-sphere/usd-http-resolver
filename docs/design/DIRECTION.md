# Long-range direction

Last updated: 2026-09-04.

This document states where the project is going over a horizon longer than the
release sequence. It is direction, not contract. The
[design policy](DESIGN_POLICY.md) is the standing policy and outranks this file;
the [roadmap](../roadmap/README.md) states the order of work; this file states
why that order is what it is, and what the work is eventually for.

Nothing here is a commitment. Every item is admitted by §12 below, and an item
that fails that test is dropped rather than accommodated.

## 1. The repository name is narrower than the architecture

The repository is called `usd-http-resolver` and will keep that name for as long
as HTTP is the only transport anybody uses. The name is accurate about the
deliverable and inaccurate about the design:

```text
Repository            usd-http-resolver
Internal concept      remote asset access
```

The practical consequence is a rule, not a rename: no module below the plugin
bundle may be written in a way that only HTTP could satisfy. `usdAssetIo` names
offsets and validators; `usdAssetCache` names blocks and keys; neither has heard
of an `ETag`, a status code, or a socket. That separation is what leaves room to
extract a common layer later — for S3, for content-addressed storage, for a
`fetch` backend in a browser — without a rewrite, and it is the reason the
invariants in §2 of the [design policy](DESIGN_POLICY.md) are worded the way
they are.

Extracting that layer into its own repository is a possible consequence of
having a second and third transport, never a prerequisite for building them.

## 2. The layer that is actually the product

An `ArResolver` that opens `http://` is the visible part. The product is the
layer underneath it:

```text
USD / ArResolver
      |
      v
HTTP asset resolver adapter        plugins/http-resolver
      |
      v
random access / cache / transport  libs/usd-asset-io, libs/usd-asset-cache
      |
      v
transport backend                  libs/usd-asset-http, libs/usd-asset-local
  libcurl
  browser fetch                    (research)
  object storage                   (deferred)
```

The value of resolution alone is small: a URL becomes a path. The value is in
what is available *after* resolution — size, validator, byte ranges, seek,
cache, conditional requests, retry, connection reuse, integrity, persistence —
and that is a file-like random-access surface, not a URL-handling surface.

This is why the internal contract is
[`AssetReader`](../architecture/ASSET_READER.md) and not a URL type, and why
that contract is expressed in offsets and lengths rather than in requests:

```text
GetMetadata()                      -> size, validator, range support
Read(offset, buffer)               -> bytes, bytesRead, status
```

HTTP is one implementation of that shape. So is a local file, which is why the
local backend can be the correctness oracle for the remote one.

## 3. Random access is the thesis

The technical claim of this project is not that a URL can be opened. It is that
a range request can be carried all the way to a FileFormat Plugin without the
plugin knowing what a range request is.

```text
download the whole asset
```

versus

```text
read the header
      -> read the index
      -> decide which region is needed
      -> read only those byte ranges
```

The difference is the difference between a table scan and an index lookup, and
it has the same shape of payoff: it grows with the asset and with the
selectivity of the query. A 64 KiB header out of a gigabyte is the case the
architecture exists for.

The recorded form of that claim is [BASELINE.md](../reference/BASELINE.md), and
§9 of the design policy is why it is recorded rather than asserted: an
unmeasured ratio is a marketing statement.

## 4. Formats bring their own index

The temptation, once ranges work, is to define a sidecar index so the resolver
can be clever. It is refused, permanently, by §3.4 of the design policy.

The formats worth streaming already carry a random-access-friendly structure,
and the right move is to map that structure onto ranges rather than to invent a
parallel one:

```text
chunk table            offset table           table of contents
spatial index          page index             ZIP central directory
USD package metadata
```

Where a format carries nothing usable, that is the format's problem, solved in
the format's own plugin. Where extra information genuinely has to travel with
the data, it travels in a structure that already exists — USD metadata, package
metadata, the format specification — never in a `usd-http-resolver`-specific
header or manifest. A design that needs a cooperating server is rejected
outright.

## 5. Division of labour with FileFormat plugins

```text
resolver         makes a remote asset readable
format plugin    decides which bytes to read
```

The resolver never learns what a byte means; the plugin never learns how a byte
arrived. The whole of the plugin's side is:

```text
asset = resolver.OpenAsset(resolvedPath)
asset->Read(buffer, count, offset)
```

which is `pxr::ArAsset` and nothing else — no header from this repository, no
library, no CMake edge, per [ADR-0001](../adr/0001-consumer-interface.md). A
consumer that needs an exception to that is a defect here, not a special case
there.

The cost of getting this wrong is paid once per plugin: every plugin that grew
its own libcurl call is a place where retry, validators, credentials, and cache
policy have to be re-argued. The single source of truth for remote access is
this repository, and consumers reach it through runtime composition rather than
through a dependency.

## 6. Consumers, in order

Each consumer is chosen for what it tests that the previous one could not.

| Order | Consumer | What it validates |
| --- | --- | --- |
| 1 | `usd-pointcloud-plugins` (COPC) | A real internal index; bounded queries over a large asset; the header-index-scatter pattern that punishes a naive range reader |
| 2 | `usd-3dgs-plugins` | Camera-driven access; LOD and progressive loading; whether caching survives locality that changes as the view moves |
| 3 | `usd-vrm-plugins` / container formats | Nested random access: a range inside a package inside a URL |
| 4 | Material libraries | Many small assets rather than few large ones; the opposite request profile, where per-asset open cost dominates |

The first is the one that decides whether the abstraction is real, and it is
detailed in [consumer integration](../roadmap/consumer-integration.md). The
discipline stated there is worth repeating: the API is not frozen before that
integration. Fixing an abstraction from the outside, against a real consumer, is
cheaper than fixing it from the inside against an imagined one.

## 7. OpenStrata's position

OpenStrata is not a consumer of this library. It is the system that composes
this resolver with runtimes, format plugins, and renderers:

```text
OpenStrata
   |
   +-- usd-http-resolver
   +-- usd-pointcloud-plugins
   +-- usd-3dgs-plugins
   +-- usd-vrm-plugins
   +-- Hydra renderer
```

So the shape this repository owes it is:

```text
a standalone library and bundle
+
a native OpenStrata integration
```

and not a dependency on OpenStrata APIs anywhere below `plugins/`. `v0.5.0`
built the first half of that: the workspace publishes an aggregate product with
a component-owned acceptance probe that runs from the installed artifact rather
than from a producer build directory. Formation-level composition — a consumer
workspace pinning this resolver by digest against one certified runtime — is the
second half.

## 8. WebAssembly

The transport seam exists so that this is a backend question rather than a port:

```text
HttpClient
 |-- CurlHttpClient        native
 +-- WasmFetchHttpClient   browser: fetch(), ReadableStream, Range, Cache API
```

libcurl does not build for the Wasm target, and
[ADR-0003](../adr/0003-http-client-dependency.md) recorded that as an accepted
cost with a named answer: a separate `usdAssetWasm` backend over `fetch`, not a
rebuild of `usdAssetHttp`. Everything above the transport seam — the read
contract, the cache, the resolver — is already free of libcurl and does not
move.

The second Wasm question is composition rather than compilation. A single
monolithic runtime is the wrong artifact; independently composable ones are the
right shape, and it is the same shape OpenStrata already wants:

```text
USD core (wasm)  +  resolver (wasm)  +  format plugin (wasm)  +  renderer (wasm)
```

This is research. It gates no release.

## 9. USD as a data model over distributed storage

The furthest-out framing, and the one that explains why the cache work looks
like database work.

A USD layer that references remote point clouds, remote splats, remote VRM
assets, remote textures, and remote metadata is a schema and a namespace over
data that lives on object storage. USD supplies composition, references,
dependency structure, and typing; the resolver supplies the I/O that
materializes a region of it on demand.

| Database | USD + resolver |
| --- | --- |
| row / record | prim / asset |
| index | format index / package index |
| query planner | composition, plus the consumer's own logic |
| block read | HTTP range request |
| buffer cache | block cache |
| remote storage | HTTP / CDN / object storage |
| schema | USD schema |
| view | composed stage |

The correspondence is a source of technique, not a goal. Nothing here is an
argument for making USD a query engine, and no SQL surface is planned. What it
does argue is that block sizing, cache keying, read-ahead, and eviction are
solved problems in a neighbouring field, and that this project should borrow
those answers rather than rediscover them — which is what
[BLOCK_POLICY.md](../reference/BLOCK_POLICY.md) already is.

## 10. The cache, in four levels

Random access is only practical with a cache, and the levels arrive in an order
where each one is measurable before the next is built.

| Level | What it does | Status |
| --- | --- | --- |
| 1 | Request de-duplication: the same missing region is not fetched twice concurrently | Implemented — subsumed by per-block single-flight, `v0.3.0` |
| 2 | Block cache: reads aligned and expanded to fixed blocks, adjacent blocks coalesced into one request | Implemented — `v0.3.0`; 64 KiB blocks and an 8 MiB coalescing ceiling, both measured |
| 3 | Adaptive read-ahead: sequential access prefetches the next block; random access suppresses it | Not implemented — the first level whose benefit cannot be argued from a loopback fixture |
| 4 | Persistent cache: entries keyed by identifier, validator, and block index survive the process | Implemented — `v0.4.0`, `Strong` validator only |

Level 3 is deliberately last even though it is conceptually simpler than level
4, and the reason is measurement rather than difficulty. Read-ahead trades bytes
for round trips, and every number in [BASELINE.md](../reference/BASELINE.md) is
a loopback number, where a round trip is nearly free. A read-ahead policy tuned
against a fixture with no latency is a policy tuned against the wrong cost, so
it waits for the first measurement taken over real distance. Invariant 11 — a
performance parameter is decided by measurement, not by choice — is what makes
that a rule rather than a preference.

## 11. Async

The public path is synchronous and correct, which is the right order: §15 of the
design policy forbids an async API before synchronous reads are measured. What
the internals must not do is foreclose it.

```text
ReadAsync(offset, buffer)
Prefetch(offset, size)
```

are the shapes that would eventually appear. The value is highest exactly where
this project is heading — point clouds, splats, a browser event loop, a renderer
that would rather not block a frame on a socket — so the constraint on today's
code is that no layer assume a read completes on the calling thread's stack.
Prefetch hints, when they arrive, are supplied by the caller and never inferred
from content, per §3.5 of the design policy.

## 12. What a new feature has to be

The admission test for anything proposed from here on.

**Easy to accept:**

- reusable from more than one FileFormat Plugin
- expressible without naming HTTP
- contributing to random access rather than to whole-asset transfer
- consistent with OpenUSD's own conventions
- requiring no special preparation of the data
- portable to OpenStrata composition and to a Wasm build

**Treat with suspicion:**

- logic specific to one file format
- HTTP details pushed into USD schema or metadata
- a bespoke index format
- a requirement for a cooperating, non-static server
- libcurl types on any public surface
- the resolver understanding a format's internal structure

The second list is not a list of prohibitions; it is a list of things that need
an argument and usually an ADR. The failure mode to avoid is the inverted one:
adding a backend-specific concept to the core contract because a backend did not
fit. A backend that does not fit is evidence about the contract, and it is
investigated as that before it is accommodated.

## 13. The whole picture

```text
                     USD Stage
                        |
                Asset resolution
                        |
              Remote asset interface
                        |
          +-------------+-------------+
          |             |             |
        HTTP           S3       other backend
          |
      range access
          |
    cache / prefetch
          |
   FileFormat plugins
     /      |       \
PointCloud 3DGS     VRM
     \      |       /
         Hydra
          |
   native / WebGPU
          |
 desktop / browser
```

Short term, `usd-http-resolver` is a resolver that lets OpenUSD open an HTTP
asset. Medium term, it is the shared I/O substrate that lets any FileFormat
Plugin randomly access remote data. Long term, it is the layer that connects USD
composition to remote object storage, range access, streaming, and — if the Wasm
research pays off — a browser.

The near-term work does not change on account of any of that. It is written down
so that the near-term work is not accidentally built in a way that forecloses
it.
