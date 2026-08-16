// SPDX-License-Identifier: Apache-2.0
//
// Offset and size arithmetic, in one place.
//
// This is small enough to look like it belongs inline in each backend, and that
// is exactly why it is here. Offset arithmetic is where this project's overflow
// lives (docs/design/DESIGN_POLICY.md §11.4), and a per-backend copy is a
// per-backend chance to get the EOF boundary wrong by one. Every backend
// resolves a caller's request through `ResolveReadRange` and then reads what it
// says to read.
//
// Normative contract: docs/architecture/ASSET_READER.md §3.

#ifndef USDASSETIO_RANGEMATH_H
#define USDASSETIO_RANGEMATH_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "usdAssetIo/Diagnostics.h"

namespace usdasset {

/// What a caller's (offset, size) request resolves to against a known asset
/// size.
enum class ReadRangeOutcome {
    /// Nothing to transfer, and not an error: a zero-length request, or one at
    /// or past EOF. No request is issued for these.
    Empty,
    /// `length` bytes are available and must all be delivered. `length` is less
    /// than the requested size when the request straddles EOF, which is
    /// success, not a short read.
    Transfer,
    /// `offset + size` overflows. The caller is wrong, and the sum is never
    /// evaluated.
    Overflow,
};

struct ReadRange {
    ReadRangeOutcome outcome = ReadRangeOutcome::Empty;
    std::uint64_t offset = 0;
    /// Bytes that must be delivered for the read to be complete. Zero unless
    /// `outcome` is `Transfer`.
    std::size_t length = 0;
};

/// Resolves a read request against the asset size captured at open.
///
/// Overflow is checked before anything else, so an overflowing request past EOF
/// is `Overflow` rather than `Empty`: the caller's arithmetic is wrong whatever
/// the asset looks like, and reporting the asset's shape back to a caller that
/// cannot compute an offset is not useful.
ReadRange ResolveReadRange(std::uint64_t offset,
                           std::size_t size,
                           std::uint64_t assetSize) noexcept;

/// The `InvalidArgument` status for an overflowing request, with the offending
/// range attached.
Status OverflowStatus(std::uint64_t offset, std::size_t size);

/// The `InvalidResponse` status for a transfer that stopped below EOF, with the
/// shortfall attached. A backend calls this rather than returning a hole.
Status ShortReadStatus(std::uint64_t offset,
                       std::size_t expected,
                       std::size_t actual,
                       std::string_view identifier);

}  // namespace usdasset

#endif  // USDASSETIO_RANGEMATH_H
