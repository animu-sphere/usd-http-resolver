// SPDX-License-Identifier: Apache-2.0
//
// The offset arithmetic every backend shares. These are the same boundaries the
// shared suite drives through a real reader; here they are checked without one,
// so that a failure points at the arithmetic rather than at a transport.

#include "usdAssetIo/RangeMath.h"

#include <cstdint>
#include <limits>
#include <string>

#include "Check.h"

using namespace usdasset;

namespace {

constexpr std::uint64_t kMax = (std::numeric_limits<std::uint64_t>::max)();

void ZeroLengthIsEmptyAtEveryOffset() {
    for (const std::uint64_t offset : {std::uint64_t{0}, std::uint64_t{1},
                                       std::uint64_t{4096}, kMax}) {
        const ReadRange range = ResolveReadRange(offset, 0, 8192);
        CHECK(range.outcome == ReadRangeOutcome::Empty);
        CHECK_EQ(range.length, std::size_t{0});
    }
}

void OffsetZeroTransfersWhatWasAsked() {
    const ReadRange range = ResolveReadRange(0, 16, 8192);
    CHECK(range.outcome == ReadRangeOutcome::Transfer);
    CHECK_EQ(range.length, std::size_t{16});
    CHECK_EQ(range.offset, std::uint64_t{0});
}

void ReadEndingExactlyAtEofIsFull() {
    const ReadRange range = ResolveReadRange(8192 - 16, 16, 8192);
    CHECK(range.outcome == ReadRangeOutcome::Transfer);
    CHECK_EQ(range.length, std::size_t{16});
}

void ReadStartingExactlyAtEofIsEmptyAndNotAnError() {
    const ReadRange range = ResolveReadRange(8192, 16, 8192);
    CHECK(range.outcome == ReadRangeOutcome::Empty);
    CHECK_EQ(range.length, std::size_t{0});
}

void ReadStartingPastEofIsEmptyAndNotAnError() {
    for (const std::uint64_t offset : {std::uint64_t{8193}, std::uint64_t{1} << 40}) {
        const ReadRange range = ResolveReadRange(offset, 16, 8192);
        CHECK(range.outcome == ReadRangeOutcome::Empty);
    }
}

void ReadStraddlingEofTruncatesToTheRemainder() {
    const ReadRange range = ResolveReadRange(8192 - 8, 64, 8192);
    CHECK(range.outcome == ReadRangeOutcome::Transfer);
    CHECK_EQ(range.length, std::size_t{8});
}

void WholeAssetReadIsOneTransfer() {
    const ReadRange range = ResolveReadRange(0, 8192, 8192);
    CHECK(range.outcome == ReadRangeOutcome::Transfer);
    CHECK_EQ(range.length, std::size_t{8192});
}

void EmptyAssetTransfersNothing() {
    CHECK(ResolveReadRange(0, 0, 0).outcome == ReadRangeOutcome::Empty);
    CHECK(ResolveReadRange(0, 1, 0).outcome == ReadRangeOutcome::Empty);
    CHECK(ResolveReadRange(1, 1, 0).outcome == ReadRangeOutcome::Empty);
}

void OneByteAssetHasExactlyOneReadableByte() {
    const ReadRange first = ResolveReadRange(0, 4, 1);
    CHECK(first.outcome == ReadRangeOutcome::Transfer);
    CHECK_EQ(first.length, std::size_t{1});
    CHECK(ResolveReadRange(1, 4, 1).outcome == ReadRangeOutcome::Empty);
}

void OverflowIsCheckedBeforeAnythingElse() {
    // The sum is never evaluated, so an overflowing request past EOF is still
    // an overflow rather than a quiet empty read.
    CHECK(ResolveReadRange(kMax, 1, 8192).outcome == ReadRangeOutcome::Overflow);
    CHECK(ResolveReadRange(kMax - 4, 16, 8192).outcome == ReadRangeOutcome::Overflow);
    CHECK(ResolveReadRange(kMax, 1, 0).outcome == ReadRangeOutcome::Overflow);

    // A sum of exactly kMax is legal, not an overflow. It is also past EOF, so
    // it reads nothing. One byte further along is the overflow.
    CHECK(ResolveReadRange(kMax - 16, 16, 8192).outcome == ReadRangeOutcome::Empty);
    CHECK(ResolveReadRange(kMax - 15, 16, 8192).outcome == ReadRangeOutcome::Overflow);

    // A zero-length request cannot overflow, whatever the offset.
    CHECK(ResolveReadRange(kMax, 0, 8192).outcome == ReadRangeOutcome::Empty);
}

void BlockBoundaryNeighboursAgree() {
    constexpr std::uint64_t kAssetSize = 65536 * 3 + 7;
    for (const std::uint64_t block : {std::uint64_t{512}, std::uint64_t{4096},
                                      std::uint64_t{65536}}) {
        for (const std::int64_t delta : {-1, 0, 1}) {
            const std::uint64_t offset = block + static_cast<std::uint64_t>(delta);
            const std::size_t size = static_cast<std::size_t>(block);
            const ReadRange range = ResolveReadRange(offset, size, kAssetSize);
            CHECK(range.outcome == ReadRangeOutcome::Transfer);
            const std::uint64_t expected =
                (offset + size <= kAssetSize) ? size : kAssetSize - offset;
            CHECK_EQ(static_cast<std::uint64_t>(range.length), expected);
        }
    }
}

void StatusHelpersCarryTheRange() {
    const Status overflow = OverflowStatus(kMax, 16);
    CHECK(overflow.code == StatusCode::InvalidArgument);
    CHECK(overflow.severity == Severity::CodingError);
    CHECK(overflow.byteOffset.has_value() && *overflow.byteOffset == kMax);
    CHECK(overflow.byteLength.has_value() && *overflow.byteLength == 16u);

    const Status shortRead = ShortReadStatus(1258291, 65536, 31709,
                                             "https://example.org/a.copc?sig=secret");
    CHECK(shortRead.code == StatusCode::InvalidResponse);
    CHECK(shortRead.message.find("sig=secret") == std::string::npos);
    CHECK(shortRead.message.find("31709") != std::string::npos);
}

}  // namespace

int main() {
    ZeroLengthIsEmptyAtEveryOffset();
    OffsetZeroTransfersWhatWasAsked();
    ReadEndingExactlyAtEofIsFull();
    ReadStartingExactlyAtEofIsEmptyAndNotAnError();
    ReadStartingPastEofIsEmptyAndNotAnError();
    ReadStraddlingEofTruncatesToTheRemainder();
    WholeAssetReadIsOneTransfer();
    EmptyAssetTransfersNothing();
    OneByteAssetHasExactlyOneReadableByte();
    OverflowIsCheckedBeforeAnythingElse();
    BlockBoundaryNeighboursAgree();
    StatusHelpersCarryTheRange();
    return usdassettest::Report("usdAssetIo range math");
}
