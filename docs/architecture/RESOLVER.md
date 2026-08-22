# Resolver contract

This document fixes what the `plugins/http-resolver` bundle owns: URI
normalization, resolution and anchoring, asset identity exposed to OpenUSD, and
the `ArAsset` surface handed to consumers.

It is the only part of the repository that consumers observe. Everything below
it is described in [ASSET_READER.md](ASSET_READER.md).

Sections marked **Planned** are direction, not shipped behavior.

Status: implemented in `v0.2.0`, except §3, which is `v0.4.0`, and §6, which is
`v0.6.0` apart from the environment variables named in
[CONFIGURATION.md](../reference/CONFIGURATION.md).

§3 has landed: asset info and identity stability are implemented, and what that
surface may and may not publish is stated there rather than left to the code.

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
- a character that cannot appear literally in a path — a space, a control byte,
  a non-ASCII byte, a stray `%` — is percent-encoded;
- the query string is preserved **verbatim**, in its original order;
- the fragment is removed from the identifier and never sent to a server;
- the userinfo component is removed from the identifier.

The query string is preserved verbatim and never reordered because signed URLs
carry their signature in it, and reordering invalidates the signature. A
"canonicalization" that sorts query parameters would break every pre-signed
object-storage URL. This is stated here so that a later cleanup does not
introduce it.

Encoding is the one rule that adds characters rather than removing them, and it
is what makes a human-authored reference resolvable: `../textures/tree bark.png`
is a valid asset path and not a valid URI, and handing it to a transport
verbatim produces a request line a strict origin answers with `400`. It is
idempotent — an escape that is already there is recognized as one and never
doubly encoded.

Decoding runs **before** dot segments are removed. `%2E%2E` is an encoded `..`,
and removing dot segments first leaves it in the identifier for the *origin* to
resolve, which means the resolver's idea of which asset was named and the
server's differ. The mirror image does not arise: `%2F` is not unreserved, is
never decoded, and therefore never becomes a segment separator.

The userinfo is removed because §4.3 of the
[design policy](../design/DESIGN_POLICY.md) keeps credentials out of the
resolver API, and an identifier *is* the resolver API — `https://user:t@host/a`
hides one in the part a query-string rule keeps. The consequence is deliberate
and worth stating plainly: a URL that needs credentials in its authority fails
at the origin with `AccessDenied` rather than succeeding with a secret in every
log line, cache key, and diagnostic. Two URLs that differ only in their
credentials share one identity, which is the right answer — they name the same
bytes.

### 2.1.1 The extension

`GetExtension` is part of normalization's job, not the default's. OpenUSD picks
a file format from it, and the default implementation returns the text after the
last `.` of the whole path — for `main.usda?X-Amz-Signature=abc.def` that is
`def`, and for a signature without a dot it is `usda?X-Amz-Signature=…`. Neither
matches a file format, so a signed remote layer cannot be identified at all: a
failure that presents as an unsupported format and is in fact a resolver bug.
The query and the fragment are not part of the name of the thing.

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

What is retained is the *open reader*, not a copy of its metadata: resolution
has to open the asset in order to answer the existence question at all, and
discarding that open would make every resolved asset cost two round trips. Three
properties follow, and each is a decision rather than a detail.

A reader is handed out **once**. A second `OpenAsset` for one identifier opens
again rather than sharing a reader that is already bound to a revision somebody
else is mid-composition on; two `ArAsset`s over one URL may therefore be reading
two revisions, and each is individually consistent, which is exactly the
guarantee §2.1 of [ASSET_READER.md](ASSET_READER.md) makes.

A failure is **not** retained. Caching one would turn a server that was
restarting into an asset that does not exist for the rest of the process.

The table of retained opens is **bounded**. A resolve that is never followed by
an open is legal and normal — a host probing for existence does it constantly —
and an unbounded table would hold one reader per asset the process ever asked
about. Dropping the oldest costs a later metadata request and never costs
correctness.

## 3. Asset info and identity

Validators are captured in `v0.2.0`, because a range backend cannot be correct
without them (§2.1 of [ASSET_READER.md](ASSET_READER.md)). What waited for
`v0.4.0` was *exposing* identity outward, which is a different commitment: a
consumer that keys its own generated cache on this surface turns a wrong
validator into a durable wrong answer, so the surface opened only after capture
had been correct for a release.

`GetAssetInfo` exposes what a consumer needs in order to decide whether *its
own* generated-cache reuse is safe. Four values, under the neutral names the
first consumer's contract uses, in `ArAssetInfo::resolverInfo` as a
`VtDictionary`:

```text
resolvedIdentifier   string    the normalized absolute URI, after redirects,
                               with credentials elided
size                 uint64    byte size at open
validationToken      string    opaque; the backend's captured validator value,
                               empty when there is no usable validator
stability            string    "Stable" | "Unstable" | "Unavailable"
```

The token is opaque by contract. A consumer must not parse it, compare it to an
`ETag`, or infer a timestamp from it. This is what lets a later S3 or
content-addressed backend supply a completely different token shape without any
consumer change — and it is the same neutrality the first consumer's contract
demands from its side.

`stability` is the field a consumer actually acts on, and it is the only
projection of validator strength that crosses this boundary. The resolver does
not expose, and does not itself read, the `ValidatorKind` and
`ValidatorStrength` that the backend used to build its conditional requests:
those are transport semantics, and §7.1 of the reader contract keeps them below
this layer. A consumer reading `Unstable` knows not to persist a derived
artifact against this asset; it does not know, and must not need to know, that
the reason was a one-second `Last-Modified` granularity.

### 3.1 `version` carries a token only when the token may be reused

`ArAssetInfo::version` carries the same token, and only when the identity is
`Stable`. That asymmetry with the dictionary is deliberate, and it is the rule
this whole surface turns on.

`version` travels alone. Nothing accompanies it — no stability, no
qualification — and a consumer that finds a token there has been handed an
identity with no way to ask what it is worth. The first consumer does exactly
that: it reads `assetInfo.version`, and treats a non-empty value as sufficient
for generated-cache reuse. So a token that may not key durable reuse must not
appear in that field, and a weak or absent validator leaves it empty and
explains itself in `resolverInfo` instead.

The dictionary is annotated, so it can afford to say more. `validationToken` is
published there for a weak validator too, because a weak validator that
*changed* is still positive evidence that the asset changed (§7.2 of
[ASSET_READER.md](ASSET_READER.md)) — it is only proof of *sameness* that weak
cannot supply. Beside it, `stability` says which of those two uses is admitted.

### 3.2 An identifier that contradicted itself stops being reusable

`ArAssetInfo` is keyed by asset path, and this resolver hands out one reader per
open (§2.3): two `ArAsset`s over one URL may be reading two revisions. Asset
info has no way to say which of them a caller is holding.

That ambiguity is harmless until the asset actually moves. When two opens of one
identifier capture two different validators — a republish underneath a running
process — a consumer holding the earlier revision could be handed the later
revision's token, and would file bytes from revision A under the identity of
revision B. Nothing downstream could detect it: the entry would validate, and it
would be wrong.

So a contradicted identifier stops publishing a reusable identity for the rest
of the process: `Stable` degrades to `Unstable`, and `version` goes empty. The
current token stays visible in the dictionary, because a consumer that filed
something under the old one needs to see that it changed.

Two things are remembered per identifier, and only one of them may be forgotten.
The *answer* — the metadata that lets asset info be returned without a request —
is held in a bounded table, because dropping it costs a request and nothing
else. The *validator a later open is compared against* is held for the life of
the process, because dropping it is not a cost, it is a wrong answer: an asset
that has already moved would look like an asset being opened for the first time,
and would publish a reusable token for a revision some consumer is not holding.
A bound on that second record is a bound on how far back a republish can be
noticed, and there is no such bound.

### 3.3 The identity is the open's, not a fresh request's

Asset info is answered from the open this process already performed for that
identifier, and never from a metadata request issued to answer the question.
Those are different answers, and the difference is not a matter of cost: the
identity a consumer needs is the identity of the bytes it is *holding*, and a
`HEAD` issued now describes whatever is published now.

An identifier nothing has opened is opened here, and the reader is retained
exactly as §2.3 describes, so the request it costs is the one the `OpenAsset`
that follows would have made rather than an extra one.

Two limits on that, and both are about an origin that is failing. Asset info
does not open an identifier whose resolved path is empty: an empty resolved path
is a resolution that failed or never happened, and asset info must not be the
call that discovers a `503` — for a layer being reloaded against a dead origin
that is a second identical round trip behind the one `Resolve` has just paid
for. And it posts no diagnostic of its own: this is a question about identity
rather than an operation on the asset, the operation that follows reports the
same fault with the same code, and one fault rendered twice is the noise
[DIAGNOSTICS.md](DIAGNOSTICS.md) §3 exists to avoid.

### 3.4 `GetModificationTimestamp` is invalid, permanently

A validator is not a time. `Last-Modified` sometimes is, but reading it as one
means parsing an HTTP construct above the backend that captured it, which §7.1
of the reader contract places below this layer — and a strong `ETag`, the
validator this resolver most wants to publish, carries no time at all.

Synthesizing one anyway would be worse than not answering. A consumer that finds
no token in asset info falls back to the timestamp and builds an identity out of
it, so a fabricated number would manufacture precisely the durable identity that
a weak or absent validator is not allowed to have. `GetAssetInfo` is the whole
identity surface, and an invalid timestamp is how this resolver says so.

What an invalid timestamp costs is a reload: `SdfLayer::Reload` re-reads a layer
whose timestamp is invalid rather than comparing it. That is a request, and
never a wrong answer.

### 3.5 Credentials

Credentials, `Authorization` values, and signed-URL query strings never appear
in asset info, in a timestamp, or in any string a consumer can read. The
identifier is published through the same elision every diagnostic goes through.

Elision costs a distinction, and the cost is stated rather than hidden: two
assets that differ only in their query strings elide to one `resolvedIdentifier`.
That field is therefore not, on its own, a cache key. The token is what
distinguishes revisions, and a consumer that wants the URL it asked about
already holds it — it passed it in.

## 4. The `ArAsset` surface

`OpenAsset` returns an `ArAsset` backed by the reader stack:

```text
ArAsset::Read(buffer, count, offset)  ->  AssetReader::Read(offset, buffer, count)
ArAsset::GetSize()                    ->  AssetMetadata::size
```

`ArAsset::Read`'s return semantics map exactly onto §3 of
[ASSET_READER.md](ASSET_READER.md): a short read at EOF is normal, a short read
below EOF is a failure, and the failure is reported as a diagnostic rather than
as fewer bytes.

### 4.1 `GetBuffer()` returns null, permanently

```text
GetBuffer()      -> nullptr
GetFileUnsafe()  -> {nullptr, 0}
```

`Read` and `GetSize` are the primary path, and they are the whole path.
`GetBuffer` asks for whole-asset materialization; honoring it on a 10 GB remote
asset is the exact transfer this project exists to avoid, so implementing it
would contradict the project's purpose rather than complete its API surface.
`GetFileUnsafe` asks for a local file descriptor, and there is no file.

This is a compatibility contract, not an omission, and it is stated as one so
that neither a future contributor nor a consumer treats it as a gap to close.

`GetDetachedAsset` is the one nearby entry point that is *not* refused, and the
asymmetry is deliberate. It is inherited from `ArAsset`, whose implementation
reads the whole asset into memory through `Read`. A host that turns detached
layers on has asked for a layer's bytes to be independent of the asset it came
from, and there is no way to provide that remotely except by transferring them;
refusing would break the feature rather than bound the transfer. It is off by
default, and a host that enables it globally has chosen a full download per
layer.

### 4.2 What interoperability is claimed

The claim this resolver makes is bounded and specific:

> interoperability with random-access-compatible FileFormat Plugins

A FileFormat Plugin that reads through `ArAsset::Read` at offsets it computes
itself is compatible. A FileFormat Plugin that requires whole-buffer access is
**not compatible with the remote random-access path** — not because it is
broken, but because it is asking for a different thing than this resolver
provides. Such a plugin still works against a local asset through the primary
resolver; what it cannot do is stream a remote one through this bundle.

Stating the boundary this way puts the incompatibility where it belongs. A
plugin that calls `GetBuffer` and dereferences without a null check has a bug
that a local file happened to hide, and it will fail identically against any
resolver that streams.

### 4.3 Compatibility matrix — Planned

As consumers are evaluated, they are recorded here rather than assessed
repeatedly in conversation:

| Consumer / format | Uses `ArAsset::Read` | Depends on `GetBuffer` | Remote capable |
| --- | --- | --- | --- |
| COPC (`usd-pointcloud-plugins`) | yes | no | yes |
| 3DGS (`usd-3dgs-plugins`) | yes | no | expected, unverified |
| Container formats (`usd-vrm-plugins`) | unknown | unknown | unknown |

The third row is the honest one. A row moves out of `unknown` when someone has
actually read the plugin's I/O path, and `expected` is never reported as `yes`
until a fixture has been opened.

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

- no lock is ever held across a request. The table of retained opens is guarded
  for lookup and insertion only; the round trip happens under a per-identifier
  lock that no other identifier can contend for;
- one asset's open does not block another asset's read;
- two concurrent resolutions of the same identifier perform one metadata
  request, not two: the second thread waits on that identifier's lock and then
  finds the first thread's answer.

An earlier version of this section required the table itself to be a concurrent
map rather than a mutex-guarded one. That was a statement about a data structure
where the property that matters is the one above it — a global lock held across
a network round trip is what would make one slow origin stall every other
asset's resolution, and a short critical section around a hash lookup is not
that. The requirement is stated as the property now, so that it can be satisfied
without acquiring a concurrency library for one map.

## 8. Diagnostics

The bundle projects the typed vocabulary onto stable `HTTPxxx` codes and
OpenUSD diagnostics; see [DIAGNOSTICS.md](DIAGNOSTICS.md). A resolver failure
must say which of the following it is, without the consumer parsing a string:
the asset does not exist, access was denied, the server does not support
ranges, the network failed, the response was malformed, or the asset changed
mid-read.
