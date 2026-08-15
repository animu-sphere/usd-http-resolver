# Resolver contract

This document fixes what the `plugins/http-resolver` bundle owns: URI
normalization, resolution and anchoring, asset identity exposed to OpenUSD, and
the `ArAsset` surface handed to consumers.

It is the only part of the repository that consumers observe. Everything below
it is described in [ASSET_READER.md](ASSET_READER.md).

Sections marked **Planned** are direction, not shipped behavior.

## 1. Registration

The bundle registers a URI-scheme resolver, not the primary resolver:

```json
"Types": {
    "HttpResolver": {
        "bases": ["ArResolver"],
        "uriSchemes": ["http", "https"]
    }
}
```

Consequences that are contract, not detail:

- The host's primary resolver is unchanged. Local paths keep resolving exactly
  as they did, and installing this plugin never alters how a local asset opens.
- Both schemes are registered by one type. `https` is the expected one;
  `http` exists for local fixture servers and intranet hosts.
- A scheme this bundle does not claim is not this bundle's concern. `s3://` and
  friends belong to future bundles or future URI schemes, never to a special
  case inside this one.

## 2. Identifier and resolution

```text
CreateIdentifier(assetPath, anchorAssetPath)   -> normalized absolute URI
CreateIdentifierForNewAsset(...)               -> unsupported; assets are read-only
Resolve(identifier)                            -> ArResolvedPath (existence-checked)
```

### 2.1 Normalization

A URI is normalized once, at identifier creation, so that two spellings of one
asset produce one cache entry and one open:

- scheme and host are lowercased;
- a default port (`80` for `http`, `443` for `https`) is removed;
- the path is dot-segment-resolved (`.` and `..`);
- percent-encoding is normalized to uppercase hex, and unreserved characters
  are decoded;
- the query string is preserved **verbatim**, in its original order;
- the fragment is removed from the identifier and never sent to a server.

The query string is preserved verbatim and never reordered because signed URLs
carry their signature in it, and reordering invalidates the signature. A
"canonicalization" that sorts query parameters would break every pre-signed
object-storage URL. This is stated here so that a later cleanup does not
introduce it.

### 2.2 Anchoring

Relative asset paths inside a remote layer resolve against that layer's URL,
per RFC 3986 reference resolution:

```text
anchor:   https://example.org/scenes/shot_010/main.usda
asset:    ../assets/tree.usda
result:   https://example.org/scenes/assets/tree.usda
```

This is what makes a remote scene work at all: a USD layer published to a CDN
references its neighbors relatively, and none of those references mention a
host. An absolute-path asset (`/assets/tree.usda`) anchors to the scheme and
authority; an absolute URI passes through unchanged.

A relative path anchored to a **local** layer is not this resolver's business
and is left to the primary resolver.

### 2.3 Resolution and existence

`Resolve` returning a non-empty `ArResolvedPath` asserts that the asset exists.
For a remote asset that requires a round trip, so:

- resolution performs at most one metadata request per identifier;
- the result — size, range support, validator, and stability — is retained and
  reused by the subsequent `OpenAsset` for the same identifier, so that opening
  a resolved asset does not repeat the round trip;
- a `404` resolves to an empty path (the asset does not exist), while a
  network failure is a diagnostic, not an absence. Reporting a timeout as "file
  not found" sends every consumer down the wrong path.

## 3. Asset info and identity — Planned (`v0.4.0`)

`GetModificationTimestamp` and `GetAssetInfo` expose what a consumer needs to
decide whether *its own* generated-cache reuse is safe:

```text
resolved identifier      the normalized absolute URI, after redirects
size                     byte size at open
validation token         opaque; derived from ETag, else Last-Modified + size
stability                Stable | Unstable | Unavailable
```

The token is opaque by contract. A consumer must not parse it, compare it to an
`ETag`, or infer a timestamp from it. This is what lets a later S3 or
content-addressed backend supply a completely different token shape without any
consumer change — and it is the same neutrality the first consumer's contract
demands from its side.

Credentials, `Authorization` values, and signed-URL query strings never appear
in asset info, in a timestamp, or in any string a consumer can read.

## 4. The `ArAsset` surface

`OpenAsset` returns an `ArAsset` backed by the reader stack:

```text
ArAsset::Read(buffer, count, offset)  ->  AssetReader::Read(offset, buffer, count)
ArAsset::GetSize()                    ->  AssetMetadata::size
```

`GetBuffer()` returns null. It is the "give me the whole asset in memory" call,
and honoring it on a 10 GB remote asset defeats the entire project. A consumer
that needs whole-asset bytes gets them by reading ranges; one that calls
`GetBuffer` and dereferences without a null check has a bug that a local file
happened to hide.

`GetFileUnsafe()` returns `{nullptr, 0}`. There is no file.

`ArAsset::Read`'s return semantics map exactly onto §3 of
[ASSET_READER.md](ASSET_READER.md): a short read at EOF is normal, a short read
below EOF is a failure, and the failure is reported as a diagnostic rather than
as fewer bytes.

## 5. Writing

`OpenAssetForWrite` and `CreateIdentifierForNewAsset` are unsupported and fail
explicitly. Assets are immutable; publishing a new revision at a new path is
the supported editing model, per §6 of the
[design policy](../design/DESIGN_POLICY.md).

## 6. Context and configuration — Planned (`v0.6.0`)

`ArResolverContext` binding is where per-stage configuration belongs: cache
budget, timeouts, retry policy, and — later — a credential provider. It is
resolved at bind time, never read from a global on each request.

Environment variables are the v0.x mechanism and are documented in
[CONFIGURATION.md](../reference/CONFIGURATION.md). They are a bootstrap, not
the final surface: a host that opens two stages against two servers with two
credentials cannot be served by a process-global.

## 7. Thread safety

`ArResolver` methods are called concurrently. Every method here is thread-safe,
without a global lock:

- the identifier-to-metadata map is a concurrent map, not a mutex-wrapped one;
- one asset's open does not block another asset's read;
- two concurrent resolutions of the same identifier perform one metadata
  request, not two.

## 8. Diagnostics

The bundle projects the typed vocabulary onto stable `HTTPxxx` codes and
OpenUSD diagnostics; see [DIAGNOSTICS.md](DIAGNOSTICS.md). A resolver failure
must say which of the following it is, without the consumer parsing a string:
the asset does not exist, access was denied, the server does not support
ranges, the network failed, the response was malformed, or the asset changed
mid-read.
