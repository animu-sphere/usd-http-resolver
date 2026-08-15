# Consumer integration

`usd-pointcloud-plugins` is the first consumer. This document states what that
integration is, what it proves, and the rules both repositories hold to.

Status: planned for `v0.5.0`. Nothing is integrated today.

## 1. Why this consumer first

COPC is the best available test of the architecture, and not because it is
convenient:

- The assets are large enough that downloading them is a real cost, so
  selectivity is measurable rather than theoretical.
- The format carries its own hierarchy and byte ranges. A COPC reader already
  knows which bytes it wants, which means the integration tests the transport
  rather than testing a query planner nobody has written.
- The access pattern is the hard one: a small header, then an index, then a
  scatter of chunk reads. That is precisely the pattern that punishes a naive
  range reader and rewards a block cache.
- The consumer has already written its side of the boundary and committed to
  it, so the integration is a test of two contracts meeting, not a negotiation.

## 2. The boundary, from both sides

```text
usd-pointcloud-plugins                    usd-http-resolver
──────────────────────                    ─────────────────
 knows COPC layout                         knows nothing about COPC
 decides which bytes it needs              serves the bytes
 owns generated USD cache                  owns the raw byte cache
 adapts ArAsset to its own source          implements ArAsset
 never implements transport                never parses a format

                    they meet here:
                    pxr::ArAsset::Read(buffer, count, offset)
```

Both sides forbid the build edge. This repository states it in the
[workspace contract](../architecture/WORKSPACE.md) §3 and
[ADR-0001](../adr/0001-consumer-interface.md); the consumer states it in its
[resolver-backed source contract](https://github.com/animu-sphere/usd-pointcloud-plugins/blob/main/docs/architecture/RESOLVER_SOURCE.md).

## 3. What the consumer already has

The consumer reached `v0.5.0` with resolver-backed COPC reads and does not need
new code to use this resolver:

```text
ArResolver -> ArAsset -> the consumer's RandomAccessSource -> usdCopc
```

It also carries a test double at `plugins/httpresolver` — an `ArResolver` that
serves a local fixture in memory for `http://memory.copc`. That double is
explicitly not a transport: it performs no network I/O and implements no
ranges, retries, or caching. Its own README says so.

The relationship between that double and this project is worth stating plainly,
because it is easy to misread as a starting point. It is a **shape** to match,
not code to extend. It demonstrates the registration surface — how a bundle
claims `http` and `https` and returns an `ArAsset` — and that surface is
reproduced here in `plugins/http-resolver`. Everything beneath it is new. The
consumer's `v0.10.0` removes or relocates the double once this project provides
equivalent external coverage, which makes this integration the event that
retires it.

## 4. The two caches

The most likely place for these two projects to collide is caching, so the
split is fixed:

```text
this repository                 the consumer
──────────────                  ────────────
raw bytes at offsets            generated USDC and payloads
keyed by identifier +           keyed by source identity + format arguments +
  validator + block               tiling arguments + planner version + ...
in-memory, bounded              on-disk, deterministic, deletable
```

The bridge between them is one value: identity stability. This resolver reports
whether the source identity is `Stable`, `Unstable`, or `Unavailable`; the
consumer enables its generated-cache reuse only for `Stable`. It never sees an
`ETag`, and this repository never sees a tiling argument.

That single value is the entire coupling, and keeping it to one value is the
design's success condition.

## 5. Composition

Runtime only. No CMake edge, no submodule, no vendored source:

```sh
export PXR_PLUGINPATH_NAME="/path/to/http-resolver/plugin/resources:$PXR_PLUGINPATH_NAME"
export PXR_PLUGINPATH_NAME="/path/to/pointcloud-copc/plugin/resources:$PXR_PLUGINPATH_NAME"

usdview https://example.org/data/survey.copc
```

Under OpenStrata the same composition is a formation that pins both bundles by
digest against one certified runtime, which is what makes the integration
reproducible in CI rather than a manual environment setup.

## 6. The measurement

The integration's deliverable is a number, not a screenshot. On a fixture of at
least 1 GB, served by a plain static HTTP server:

| Metric | Full-download baseline | Through this resolver |
| --- | --- | --- |
| Bytes transferred | asset size | to be recorded |
| Requests issued | 1 | to be recorded |
| Selectivity | 1.0 | to be recorded |
| Time to first authored prim | to be recorded | to be recorded |
| Peak resident bytes | to be recorded | to be recorded |

Scenarios, matching [METRICS.md](../architecture/METRICS.md) §6: metadata-only
open, header and hierarchy read, a bounded spatial query, a full read, and
parallel readers.

The full read matters as much as the bounded query. A range-based reader that
loses badly to `curl` when reading everything has a block or coalescing policy
that is wrong, and the bounded-query number would be hiding it.

## 7. Success criteria

1. The consumer opens a remote COPC asset with **no change that mentions
   HTTP**. If any change is needed, the abstraction leaked and the fix belongs
   in this repository.
2. Authored USD from the remote asset is identical to authored USD from the
   same asset locally.
3. Selectivity for a bounded query is far below 1.0, recorded.
4. Identity stability drives the consumer's cache reuse correctly: `Stable`
   enables it, a changed validator invalidates it, `Unavailable` disables it.
5. Neither repository appears in the other's build graph.
6. The full-read case is not materially worse than a plain download.

Criterion 1 is the one that decides whether this project is what it claims. The
others are how well it does it.

## 8. Later consumers

Each is admitted only if it needs no HTTP code of its own:

| Consumer | What it would test that COPC does not |
| --- | --- |
| `usd-3dgs-plugins` | Camera-driven access: the pattern changes as the view moves, so cache locality is no longer predictable from file layout |
| `usd-vrm-plugins` / containers | Nested random access: a range inside a package inside a URL |
| Material libraries | Many small assets instead of few large ones — the opposite request profile, where per-asset open cost dominates |

A consumer that needs an exception to the boundary is a defect in this
repository, and it is fixed here.
