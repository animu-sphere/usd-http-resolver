# usdAssetLocal

## Purpose

The local-file backend, and the correctness oracle every other backend is
compared against.

Its job is to be correct and boring. Every property of the remote path is
expressed as an equivalence against this module -- over bytes, byte count, and
status code alike -- so a subtlety introduced here becomes a subtlety every
later transport inherits as "correct".

**OpenUSD is not required.** This module builds and tests with plain CMake on a
machine with no OpenUSD installation present, and it includes no OpenUSD header.

## Responsibilities

- Positional reads against an open file handle.
- Size and range-support discovery at open.
- A filesystem-derived validator, and the revision binding it makes possible: a
  file republished underneath an open reader is reported as `AssetChanged`.
- Mapping filesystem failures onto the typed vocabulary.
- Populating the per-asset counters.

## Non-responsibilities

- No caching, alignment, or coalescing. Every read is a read.
- No URI handling. This module takes a filesystem path; turning a `file://` URI
  into one is the resolver's job.
- No knowledge of any other backend, of the cache, or of OpenUSD.
- No asset format.

## Public API

| Header | Contains |
| --- | --- |
| `usdAssetLocal/LocalAssetReader.h` | `LocalAssetReader`, `Open`, `OpenAsset` |
| `usdAssetLocal/Testing.h` | `testing::OpenWithReadFault` -- test-only fault injection |

`Open` returns the concrete reader, which exposes `Metrics()`. `OpenAsset`
returns the shared `usdasset::OpenResult` shape, and is what the boundary suite
and, from `v0.2.0`, the resolver call.

`Testing.h` exists for one row of the boundary suite: "short read below EOF ->
`InvalidResponse`, never a hole". Every backend runs that case, and each proves
it by provisioning a fixture whose transport misbehaves -- a truncated body from
a hostile server, or an injected read fault here. Nothing outside a test calls
it.

## Dependencies

`usdasset::io`, the C++17 standard library, and the platform's file API
(`CreateFileW` / `ReadFile` with an explicit offset on Windows, `open` /
`pread` elsewhere). No third-party library.

## Data flow

```text
Open(path)
    open the file, sharing it with writers and deleters
    stat the handle           -> size, volume/device, file index/inode, mtime
    derive the validator      -> ValidatorKind::Derived, ValidatorStrength::Strong
    classify stability        -> IdentityStability::Stable
    -> LocalAssetReader

Read(offset, dst, size)
    ResolveReadRange          -> Empty | Transfer(length) | Overflow
    positional reads until length is delivered, or no progress is made
    re-stat the handle        -> AssetChanged when the identity moved
    -> ReadResult
```

## Transport

The local filesystem, through positional reads on one handle opened at
construction. The handle is opened `GENERIC_READ` with full sharing
(`FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`) on Windows, so an
asset can be republished underneath an open reader. That is deliberate: a
backend that cannot have its asset replaced while it is open is a backend whose
revision-change behavior can never be tested, and this is the module the suite
gets that case from.

**No network request is issued, ever, under any condition.**

## Range support behavior

A local file serves any byte range by construction. `supportsRandomAccess` is
always `true` and `RangeNotSupported` is never produced -- there is no
capability to discover and none to fail on.

## Failure mapping

| Condition | Code |
| --- | --- |
| `ENOENT`, `ENOTDIR`, `ENAMETOOLONG`, `ERROR_FILE_NOT_FOUND`, `ERROR_PATH_NOT_FOUND`, `ERROR_INVALID_NAME` | `NotFound` |
| `EACCES`, `EPERM`, `ERROR_ACCESS_DENIED`, `ERROR_SHARING_VIOLATION` | `AccessDenied` |
| The identifier names a directory | `InvalidArgument` |
| The identifier is not valid UTF-8 (Windows) | `InvalidArgument` |
| `offset + size` overflows | `InvalidArgument` |
| `dst` is null with a non-zero length | `InvalidArgument`, wherever the offset lands |
| A read stops below the size captured at open | `InvalidResponse` |
| The file's identity changed while the reader was open | `AssetChanged` |
| Any other filesystem error | `NetworkError` |

Two rows are worth their reasons.

**A directory is `InvalidArgument`, not `AccessDenied` or `NotFound`.** The
target exists and the identifier is well formed; it simply is not an asset. That
is the caller's mistake, and reporting it as a permission problem sends somebody
to inspect an ACL that is not the issue. On Windows a directory opens with
`ERROR_ACCESS_DENIED`, so the mapping checks the file attributes before
concluding anything -- on the failing path only.

**A generic filesystem error is `NetworkError`.** The name reads oddly for a
local file, and the code is right: the vocabulary is transport-generic, and the
condition is "the transport failed" -- the same thing a caller would do
something about. Inventing a `FilesystemError` for one backend would give two
backends two codes for one caller behavior, which is exactly the test
[DIAGNOSTICS.md](../../docs/architecture/DIAGNOSTICS.md) applies to a new code.

## Retry, timeout, and redirect policy

None of the three. There is no retry, no timeout, and no redirect: a local read
either returns bytes or fails, and a backend does not invent a retry policy of
its own. `retryCount` and `redirectCount` are therefore always zero here, and
that is a fact about the transport rather than a gap.

## Validator and revision binding

The validator is `ValidatorKind::Derived`, rendered from the device or volume
identifier, the file index or inode, the size, and the last-write timestamp at
the platform's full resolution. It is declared `ValidatorStrength::Strong`,
which classifies the asset as `IdentityStability::Stable`.

This module is entitled to that claim in a way a remote backend is not: it holds
the handle open and re-derives the identity on every read that transfers bytes,
so equal values mean the same bytes for as long as the reader lives. The check
runs **after** the copy, not before -- an identity captured before a copy proves
nothing about the bytes copied after it, and the later check catches everything
an earlier one would have plus the race between them.

When the identity has moved, the read reports `AssetChanged` with `bytesRead`
of zero. Zero rather than what was copied: those bytes may span two revisions,
and reporting them as read invites exactly the composition the guarantee exists
to prevent. The reader never rebinds — every subsequent read that reaches the
filesystem fails the same way — and `Metadata()` continues to describe the
revision the reader was bound to.

Reads that transfer nothing are the exception, and deliberately so. A
zero-length read, and a read at or past the size captured at open, return
`0, Ok` after a republish just as they did before one. `AssetChanged` exists to
stop one reader composing bytes from two revisions, and a read that returns no
bytes cannot do that; an offset past the captured size is past the end *of the
revision this reader is bound to*, whatever the file on disk now looks like, so
`0, Ok` is that revision's truthful answer rather than a stale one. The
zero-length case has a second reason: the read contract requires that it issue
no request at all, so it is the one read that is forbidden from looking. The
shared boundary suite pins both, so no backend has to guess.

`AssetChanged` also takes precedence over the truncation a shrinking rewrite
causes. The cause is the republish, and `InvalidResponse` would send a reader
looking for a corrupt file that does not exist.

## Metrics populated

| Counter | Populated with |
| --- | --- |
| `assetSize` | The size discovered at open |
| `bytesRequested` | What the caller asked for, including bytes past the end that were never transferred |
| `bytesTransferred` | What the filesystem actually delivered, including on a read that then failed |
| `requestCount` | Positional reads plus metadata stats |
| `metadataRequestCount` | One at open, and one per read that transfers bytes |
| `openLatency`, `requestLatency`, `readLatency` | Measured; quantiles are bucket estimates |
| `retryCount`, `redirectCount`, `bytesFromCache` | Always zero; this transport has no such thing |

The revalidation stat is counted, and counting it costs this backend a
comparison against HTTP that it does not have to make. It is counted anyway:
the syscall really happens, and a backend that hid it would be reporting what it
wishes it did. A read that transfers no bytes -- zero length, or at or past the
end of the asset -- issues no request at all, and the counters say so.

## Threading and ownership

- Any number of threads may call `Read` concurrently on one reader, with
  overlapping ranges, without external synchronization. The guarantee comes
  from the syscall rather than from a lock above it: `pread` and `ReadFile`
  with an explicit `OVERLAPPED` offset neither consult nor advance a shared file
  pointer.
- `Metadata()` may be called from any thread at any time. What it returns is
  fixed at open and never changes, including after the asset is republished.
- `Metrics()` may be called from any thread while reads are in flight.
- There is no global lock anywhere in this module.
- `dst` is caller-owned. The reader writes only within `[dst, dst + size)`,
  never past it, and retains no reference to it after returning. Bytes inside
  that range beyond `bytesRead` are unspecified on failure; a caller acts on
  `bytesRead`.
- The reader owns its file handle and closes it on destruction. Destroying a
  reader while another thread is inside `Read` is a use-after-free, exactly as
  it would be for any object.

## Build and test

```sh
cmake -S . -B build/core -DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF
cmake --build build/core
ctest --test-dir build/core -R "usdAssetLocal|boundary_local"
```

| Test | Covers |
| --- | --- |
| `usdAssetLocal_open` | Size, range support, validator derivation, open failures, metadata immutability |
| `usdAssetLocal_metrics` | Which counters this backend populates, and which stay at zero |
| `boundary_local_fixed` | The required boundary cases, the short read, and the revision change |
| `boundary_local_property` | Biased generated cases against the naive oracle |
| `boundary_local_concurrent` | Concurrent reads on one reader, and concurrent readers on one asset |

The read semantics are not tested in this module. They belong to the
[shared boundary suite](../../tests/README.md), and duplicating them here would
let this backend drift from the contract every other backend is held to.

## Third-party dependencies and licenses

None. This module links the C++ standard library and the platform's own file
API. Project code is Apache-2.0.

## Known limitations

- **A same-size rewrite that restores the timestamp is invisible.** The derived
  validator is size, file identity, and modification time; a filesystem with
  coarse timestamp granularity -- FAT's two seconds, or a tool that
  deliberately restores an mtime -- can hide a revision change that preserves
  the size. On NTFS, ext4, and APFS the timestamp resolution makes this
  practically unreachable, which is why the validator is declared strong; on a
  filesystem where it is not, the claim is weaker than the label.
- **Revalidation costs one stat per read that transfers bytes.** Correctness
  before speed: this module is the oracle, and an oracle that is fast and
  occasionally wrong is worse than useless.
- **Cancellation is not admitted.** The read contract carries no cancellation
  token, and a file descriptor cannot be told out of band. The backend's suite
  row declares this rather than skipping the case quietly.
- **A read is not atomic against a republish that reverts.** A file changed and
  changed back between the copy and the check is undetectable. No validator
  scheme detects it, and the supported editing model is publishing a new
  revision at a new path rather than mutating one in place.

## Planned work

Nothing. This module is complete for what it exists to do, and it changes only
when the read contract does -- in which case every backend changes with it.

## Contracts implemented

- [ASSET_READER.md](../../docs/architecture/ASSET_READER.md)
- [DIAGNOSTICS.md](../../docs/architecture/DIAGNOSTICS.md)
- [METRICS.md](../../docs/architecture/METRICS.md)
- [BOUNDARY_SUITE.md](../../docs/contributing/BOUNDARY_SUITE.md)

When this README and one of those disagrees, the contract wins and this file is
the bug.
