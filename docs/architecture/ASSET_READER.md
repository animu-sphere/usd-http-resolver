# Asset reader contract

This document fixes the internal random-access read contract. Every backend
implements it, the cache decorates it, and the `ArAsset` adapter consumes it.

It is an **internal** contract. It is not a public SDK, and a consumer plugin
never sees it; see [ADR-0001](../adr/0001-consumer-interface.md). What a
consumer sees is `pxr::ArAsset`, described in [RESOLVER.md](RESOLVER.md).

Sections marked **Planned** are direction, not shipped behavior. What the tree
implements today is in
[CAPABILITY_MATRIX.md](../reference/CAPABILITY_MATRIX.md).

## 1. Shape

The contract is deliberately small. It is the narrowest surface that supports
range reads, and every addition to it must justify itself against a transport
that cannot be expressed without it.

```cpp
namespace usdasset {

// Opaque, transport-derived identity of the exact bytes an asset had when it
// was opened. The cache treats it as a byte string and nothing more.
using Validator = std::string;

enum class IdentityStability {
    Stable,       // a strong validator was supplied
    Unstable,     // a weak or derived validator was supplied
    Unavailable,  // no usable validator exists
};

struct AssetMetadata {
    std::string       resolvedIdentifier;  // after redirects and normalization
    std::uint64_t     size = 0;
    bool              supportsRandomAccess = false;
    Validator         validator;
    IdentityStability stability = IdentityStability::Unavailable;
    std::string       contentType;         // informational only, never dispatched on
};

struct ReadResult {
    std::size_t bytesRead = 0;
    Status      status;                    // see DIAGNOSTICS.md
};

class AssetReader {
public:
    virtual ~AssetReader() = default;

    virtual const AssetMetadata& Metadata() const = 0;

    // Reads up to `size` bytes at `offset` into `dst`.
    // Thread-safe. Concurrent calls on one reader are legal and expected.
    virtual ReadResult Read(std::uint64_t offset,
                            void*         dst,
                            std::size_t   size) = 0;
};

} // namespace usdasset
```

`Size()` is deliberately not a separate virtual: size is metadata, discovered
once at open, and a reader that cannot state its size cannot serve random
access.

## 2. Open

Opening is separate from reading, and it is where transport work is allowed to
happen eagerly:

```text
Open(identifier)
    -> resolve redirects
    -> discover size
    -> discover range support
    -> capture validator and classify stability
    -> AssetReader
```

Rules:

- Open performs at most one metadata round trip. It never reads content.
- Open failure is a typed failure. A reader is never returned in a state where
  its size or range support is unknown.
- The metadata captured at open is immutable for the reader's lifetime. A
  reader that observes a changed validator fails subsequent reads with
  `AssetChanged`; it does not silently rebind to the new content.

## 3. Read semantics

These are the semantics the boundary suite enforces, identically, for every
backend.

| Case | Required behavior |
| --- | --- |
| `size == 0` | Returns `bytesRead == 0` and `Ok`. No request is issued. |
| `offset >= assetSize` | Returns `bytesRead == 0` and `Ok`. Not an error. |
| `offset + size > assetSize` | Reads the available remainder; `bytesRead < size` with `Ok`. Truncation at EOF is normal, not a short read. |
| `offset + size` overflows | `InvalidArgument`. Never evaluated unchecked. |
| A partial read below EOF | Retried within policy; if it cannot be completed, `InvalidResponse`. A backend never returns a hole. |
| Concurrent reads | Legal on one reader. No interleaving of one caller's bytes into another's buffer. |
| Cancellation | Returns `Cancelled` promptly; in-flight transport work is abandoned, not awaited. |

The distinction in row 3 and row 5 is the one that matters: **truncation at EOF
is success, truncation below EOF is a failure**. A backend that conflates them
turns a network fault into silent data loss, and the format plugin above it
reports a corrupt file.

`dst` is caller-owned. The reader writes into it and retains no reference to it
after returning.

## 4. Backend obligations

A backend implements the read semantics above and nothing else. In particular a
backend does not:

- cache — that is the decorator's job;
- coalesce or align — that is the decorator's job;
- retry indefinitely, or apply a retry policy it invented;
- interpret bytes;
- log a credential, a signed URL, or a header value.

A backend does own:

- its transport's framing and its validation (§10 of the design policy);
- mapping its transport's failures onto the typed vocabulary in
  [DIAGNOSTICS.md](DIAGNOSTICS.md);
- populating the counters in [METRICS.md](METRICS.md).

## 5. The local backend is the oracle

`usdAssetLocal` exists to be correct and boring. Every property of the remote
path is expressed as an equivalence against it:

```text
for every (offset, size) in the boundary set:
    local.Read(offset, size).bytes == http.Read(offset, size).bytes
    local.Read(offset, size).bytesRead == http.Read(offset, size).bytesRead
```

The boundary set covers: zero length; offset zero; offset at `size - 1`; a read
straddling EOF; a read entirely past EOF; a read spanning a block boundary; a
read exactly one block; a read of the whole asset; and, once the cache exists,
each of those repeated to exercise hit, miss, and partial-hit paths.

The suite is written so the backend under test is a parameter. Adding a
transport means adding a row, not writing a test suite.

## 6. Composition

Readers compose as decorators, and the order is fixed:

```text
ArAsset adapter
      |
      v
 block cache            (single-flight, coalescing, alignment)
      |
      v
 transport backend      (local | http | ...)
```

The cache holds a reader it did not construct and does not know its type. This
is what allows the cached path to be tested against a local backend, with no
server involved, and it is why `usdAssetCache -> any backend` is forbidden in
the [workspace contract](WORKSPACE.md).

## 7. Asynchrony — Planned

An asynchronous surface is deliberately absent from v0.x:

```cpp
// Not implemented. Not designed. Recorded so that its shape is not
// accidentally precluded.
Future<ReadResult> ReadAsync(std::uint64_t offset, void* dst, std::size_t size);
void               Prefetch(std::uint64_t offset, std::size_t size);
```

`ArAsset::Read` is synchronous, so an async backend surface buys nothing until
a caller above it can express concurrency. The concurrency that matters today
is *between* readers — Hydra reading many assets at once — and that is served
by thread safety, not by futures.

`Prefetch` is admitted only when a consumer can supply a hint it actually has.
A prefetch inferred from access history is a guess, and a guess that transfers
bytes is a regression in the one number this project claims.
