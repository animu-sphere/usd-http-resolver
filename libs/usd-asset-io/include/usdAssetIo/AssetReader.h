// SPDX-License-Identifier: Apache-2.0
//
// The random-access read contract. Every backend implements it, the cache
// decorates it, and the ArAsset adapter consumes it.
//
// This is an *internal* contract. It is not a public SDK and no consumer plugin
// sees it; see docs/adr/0001-consumer-interface.md. What a consumer sees is
// pxr::ArAsset.
//
// Normative contract: docs/architecture/ASSET_READER.md.

#ifndef USDASSETIO_ASSETREADER_H
#define USDASSETIO_ASSETREADER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "usdAssetIo/Diagnostics.h"
#include "usdAssetIo/Validator.h"

namespace usdasset {

/// Everything discovered at open, immutable for the reader's lifetime.
///
/// `Size()` is deliberately not a reader method: size is metadata, discovered
/// once at open, and a reader that cannot state its size cannot serve random
/// access.
struct AssetMetadata {
    std::string resolvedIdentifier;  ///< After redirects and normalization.
    std::uint64_t size = 0;
    bool supportsRandomAccess = false;
    Validator validator;
    IdentityStability stability = IdentityStability::Unavailable;
    std::string contentType;  ///< Informational only, never dispatched on.
};

/// The outcome of one read. `bytesRead` is meaningful on failure too: a backend
/// reports what it actually placed in the caller's buffer before it failed.
struct ReadResult {
    std::size_t bytesRead = 0;
    Status status;
};

/// A reader bound, for its whole lifetime, to one asset revision.
///
/// That binding is the contract's central consistency guarantee. A reader that
/// is not bound composes a byte sequence that never existed: a header from
/// revision A and records from revision B, with every request succeeding and
/// nothing to report. See ASSET_READER.md §2.1.
class AssetReader {
public:
    virtual ~AssetReader() = default;

    AssetReader(const AssetReader&) = delete;
    AssetReader& operator=(const AssetReader&) = delete;

    /// The metadata captured at open. Immutable, and safe to call from any
    /// thread at any time.
    virtual const AssetMetadata& Metadata() const = 0;

    /// Reads up to `size` bytes at `offset` into `dst`.
    ///
    /// Thread-safe. Concurrent calls on one reader are legal and expected, and
    /// no caller's bytes are ever interleaved into another caller's buffer.
    ///
    /// `dst` is caller-owned, and the reader retains no reference to it after
    /// returning. It writes only within `[dst, dst + size)` -- never past the
    /// length it was given, whatever the transport delivered. Bytes inside that
    /// range but beyond `bytesRead` are unspecified on failure: a read that
    /// fails partway may already have copied what it had, and a caller acts on
    /// `bytesRead` rather than on what it finds in the buffer.
    ///
    /// The semantics, which the shared boundary suite enforces identically for
    /// every backend:
    ///
    ///   size == 0                  -> bytesRead 0, Ok. No request is issued.
    ///   offset >= assetSize        -> bytesRead 0, Ok. Not an error.
    ///   offset + size > assetSize  -> the available remainder, Ok. Truncation
    ///                                 at EOF is normal, not a short read.
    ///   offset + size overflows    -> InvalidArgument, never evaluated
    ///                                 unchecked.
    ///   a partial read below EOF   -> InvalidResponse. Never a hole.
    ///
    /// The distinction that matters is the last two rows of that list against
    /// the third: truncation at EOF is success, truncation below EOF is a
    /// failure. A backend that conflates them turns a transport fault into
    /// silent data loss.
    virtual ReadResult Read(std::uint64_t offset, void* dst, std::size_t size) = 0;

protected:
    AssetReader() = default;
};

/// The shape every backend's open returns. `reader` is null exactly when
/// `status` is a failure: a reader is never returned in a state where its size
/// or range support is unknown.
struct OpenResult {
    std::unique_ptr<AssetReader> reader;
    Status status;
};

}  // namespace usdasset

#endif  // USDASSETIO_ASSETREADER_H
