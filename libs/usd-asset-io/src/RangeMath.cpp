// SPDX-License-Identifier: Apache-2.0

#include "usdAssetIo/RangeMath.h"

#include <limits>
#include <string>

namespace usdasset {

ReadRange ResolveReadRange(std::uint64_t offset,
                           std::size_t size,
                           std::uint64_t assetSize) noexcept {
    ReadRange range;
    range.offset = offset;

    const std::uint64_t wanted = static_cast<std::uint64_t>(size);

    // Overflow first, and expressed as a subtraction so the sum is never
    // evaluated. `offset + wanted` would be the bug this check exists to
    // prevent.
    if (offset > (std::numeric_limits<std::uint64_t>::max)() - wanted) {
        range.outcome = ReadRangeOutcome::Overflow;
        return range;
    }

    // A zero-length read, and a read at or past EOF, are the same answer: no
    // bytes, no error, and no request issued.
    if (wanted == 0 || offset >= assetSize) {
        range.outcome = ReadRangeOutcome::Empty;
        return range;
    }

    const std::uint64_t remaining = assetSize - offset;
    const std::uint64_t available = remaining < wanted ? remaining : wanted;

    range.outcome = ReadRangeOutcome::Transfer;
    range.length = static_cast<std::size_t>(available);
    return range;
}

Status OverflowStatus(std::uint64_t offset, std::size_t size) {
    std::string message = "read offset " + std::to_string(offset) + " plus length " +
                          std::to_string(size) + " overflows a 64-bit byte offset";
    return Status::Error(StatusCode::InvalidArgument, std::move(message))
        .WithRange(offset, size);
}

Status ShortReadStatus(std::uint64_t offset,
                       std::size_t expected,
                       std::size_t actual,
                       std::string_view identifier) {
    std::string message = "transfer stopped below end of asset (requested " +
                          std::to_string(expected) + " bytes at offset " +
                          std::to_string(offset) + ", delivered " +
                          std::to_string(actual) + ")";
    if (!identifier.empty()) {
        message += " ";
        message += ElideSecrets(identifier);
    }
    return Status::Error(StatusCode::InvalidResponse, std::move(message))
        .WithRange(offset, expected);
}

}  // namespace usdasset
