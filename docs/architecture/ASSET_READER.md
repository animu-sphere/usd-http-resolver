# Asset reader contract

This document fixes the internal random-access read contract. Every backend
implements it, the cache decorates it, and the `ArAsset` adapter consumes it.

It is an **internal** contract. It is not a public SDK, and a consumer plugin
never sees it; see [ADR-0001](../adr/0001-consumer-interface.md). What a
consumer sees is `pxr::ArAsset`, described in [RESOLVER.md](RESOLVER.md).

Sections marked **Planned** are direction, not shipped behavior. What the tree
implements today is in
[CAPABILITY_MATRIX.md](../reference/CAPABILITY_MATRIX.md).

Status: §1 through §7 are implemented, for both backends. The types are in
`libs/usd-asset-io` (`usdAssetIo/AssetReader.h`, `Validator.h`, `RangeMath.h`);
`libs/usd-asset-local` and `libs/usd-asset-http` each satisfy §3 and §4 at every
boundary, and the suite in §5 is `tests/boundary`, which both are admitted by
unchanged. The revision binding in §2.1 now holds where it was written to
matter: every row of that section's table is implemented by the HTTP backend,
which is the release that could first violate the guarantee. §8 is unimplemented
by design.

## 1. Shape

The contract is deliberately small. It is the narrowest surface that supports
range reads, and every addition to it must justify itself against a transport
that cannot be expressed without it.

```cpp
namespace usdasset {

// Transport-derived identity of the exact bytes an asset had when it was
// opened. `value` is opaque above the backend that produced it; `kind` and
// `strength` are transport-level metadata that only the producing backend
// interprets. See §7.
enum class ValidatorKind {
    None,
    EntityTag,   // HTTP ETag
    HttpDate,    // HTTP Last-Modified
    Derived,     // synthesized by the backend, e.g. size + mtime + inode
};

enum class ValidatorStrength {
    None,
    Weak,        // equal values imply equivalent, not identical, bytes
    Strong,      // equal values imply byte-identical content
};

struct Validator {
    std::string       value;
    ValidatorKind     kind     = ValidatorKind::None;
    ValidatorStrength strength = ValidatorStrength::None;

    bool IsUsable() const { return kind != ValidatorKind::None; }
};

// The consumer-facing summary of the above. This, and never the struct, is
// what crosses the resolver boundary.
enum class IdentityStability {
    Stable,       // strength == Strong
    Unstable,     // strength == Weak
    Unavailable,  // strength == None, or no usable validator exists
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

### 2.1 Revision binding

The rule above is the contract's central consistency guarantee, and it is worth
stating on its own line:

> One `AssetReader` is bound, for its whole lifetime, to one asset revision.

A reader that is not bound composes a byte sequence that never existed:

```text
GET bytes=0-65535        -> revision A      header
GET bytes=1M-2M          -> revision B      index
GET bytes=50M-51M        -> revision B      records
```

The header describes a layout the records no longer have. Nothing in that
exchange fails: three requests succeeded, three `206` responses were correctly
framed, and the bytes handed upward are a blend of two files. The format plugin
above reports a corrupt asset, and the asset is intact.

This failure needs no cache to occur. It is a property of issuing more than one
request for one logical read, which is what a range backend does by definition.
Binding is therefore an obligation of **the first HTTP backend** — `v0.2.0` —
and not a consistency feature layered on later:

| Layer | Obligation |
| --- | --- |
| Backend, at open | Capture a validator and classify its strength |
| Backend, per request | Carry the validator on every subsequent range request |
| Backend, on mismatch | Fail the read with `AssetChanged`; never rebind, never retry into the new revision |
| Cache (`v0.3.0`) | Key on the validator, so an entry from revision A cannot serve revision B |

The first row of that table is where the word *observes* earns its place. A
reader that answers from bytes it captured under its own binding — a block cache
serving a hit — observes nothing, and what it returns is the revision it is
bound to. That is the guarantee holding, not an exception to it: the failure
§2.1 exists to prevent is one reader composing bytes from two revisions, and a
reader that never leaves revision A cannot. `AssetChanged` is reported by the
layer that reaches the transport, on the reads that reach it. The boundary suite
states both halves; see
[BOUNDARY_SUITE.md](../contributing/BOUNDARY_SUITE.md) §3.

A backend that cannot obtain a usable validator is still bound for its
lifetime — see §7.3 — but the binding is best-effort, and it says so through
`IdentityStability::Unavailable`.

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
path is expressed as an equivalence against it — over all three fields of the
result, not over the bytes alone:

```text
for every (offset, size) in the boundary set:
    expected = LocalReader(asset).Read(offset, size)
    actual   = BackendUnderTest(asset).Read(offset, size)

    expected.bytes     == actual.bytes
    expected.bytesRead == actual.bytesRead
    expected.status    == actual.status
```

Comparing `status` matters as much as comparing bytes: a backend that returns
the right bytes and the wrong code has still broken the EOF distinction in §3,
and that is a defect the byte comparison cannot see.

The suite is written so the backend under test is a parameter. Adding a
transport means adding a row, not writing a test suite. It is the primary
deliverable of `v0.1.0`, and its full case list, property-test generators, and
sanitizer requirements are fixed in the
[boundary suite contract](../contributing/BOUNDARY_SUITE.md).

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

## 7. Validator semantics

A validator carries two audiences' worth of information, and separating them is
what keeps transport semantics out of the layers above.

### 7.1 Who may interpret what

```text
consumer          never interprets a validator; sees only IdentityStability
resolver          never interprets a validator; projects stability outward
cache             treats validator.value as an opaque identity byte string,
                  and reads validator.strength only to decide persistence
backend           understands its transport's validator semantics completely
```

`value` is a byte string everywhere above the backend that produced it. No
layer above compares it to an `ETag`, parses a date out of it, or infers
recency from it — and the cache never concatenates two different backends'
values, because the `resolvedIdentifier` in the key already separates them.

`kind` and `strength` exist so that the backend does not have to re-derive, on
every request, what it already learned at open. They are not a widening of the
consumer surface: nothing above the cache reads them.

The rule this replaces is the tempting one — "let the cache decide whether an
`ETag` is weak." That decision needs `W/` prefix parsing, and the moment the
cache can parse an HTTP construct it has become an HTTP cache, and the local
backend stops being a usable oracle for the cached path.

### 7.2 What each validator is good for

The three questions are genuinely different, and one validator can answer some
and not others:

| Validator | Conditional range (`If-Range`) | Content identity | Persistent cache key |
| --- | --- | --- | --- |
| Strong `EntityTag` | yes | yes | yes |
| Weak `EntityTag` | no — RFC 9110 forbids it in `If-Range` | no | no |
| `HttpDate` (`Last-Modified`) | yes, with one-second granularity | no | no |
| `Derived` | backend-defined | backend-defined | only if the backend declares it strong |
| `None` | no | no | no |

Two consequences follow, and both are contract:

- A weak validator is still worth capturing. It cannot gate a conditional
  request, but a re-fetched metadata response whose weak validator has changed
  is still positive evidence of `AssetChanged`. Weak means "cannot prove
  sameness", not "carries no information".
- `Last-Modified` at one-second granularity cannot distinguish two revisions
  published inside the same second. That is exactly why it maps to `Unstable`
  rather than `Stable`, and why nothing keyed on it may outlive the reader.

### 7.3 Classification and the no-validator case

`stability` is derived at open, once, from the validator the backend captured:

```text
ValidatorStrength::Strong   -> IdentityStability::Stable
ValidatorStrength::Weak     -> IdentityStability::Unstable
ValidatorStrength::None     -> IdentityStability::Unavailable
```

When no usable validator exists, reads still work. The reader remains bound to
one revision for its lifetime as far as it can observe, in-memory caching still
functions for that lifetime, and nothing survives the reader: no persistent
entry, and no identity a consumer may reuse. The consumer is told
`Unavailable` so that it can disable its own generated-cache reuse rather than
guess.

## 8. Asynchrony — Planned

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
