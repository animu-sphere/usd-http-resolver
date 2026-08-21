// SPDX-License-Identifier: Apache-2.0

#include "usdAssetCache/CacheOptions.h"

#include <algorithm>

namespace usdasset {
namespace cache {

namespace {

/// The largest power of two not greater than `value`, inside the block bounds.
///
/// Rounds *down* rather than to the nearest. A caller that asked for 100000
/// bytes gets 65536 and not 131072: the risk this cache is exposed to is
/// over-fetch, and rounding a request for a block size upward would silently
/// double the bytes moved by every miss.
std::uint64_t RoundDownToPowerOfTwo(std::uint64_t value) noexcept {
    if (value < kMinBlockSize) {
        return kMinBlockSize;
    }
    if (value >= kMaxBlockSize) {
        return kMaxBlockSize;
    }
    std::uint64_t power = kMinBlockSize;
    while ((power << 1) <= value) {
        power <<= 1;
    }
    return power;
}

}  // namespace

CacheOptions CacheOptions::Normalized() const noexcept {
    CacheOptions normalized = *this;

    normalized.blockSize =
        RoundDownToPowerOfTwo(blockSize == 0 ? kDefaultBlockSize : blockSize);

    // Every other bound is expressed in blocks somewhere, so each one has to be
    // able to hold at least one. A budget that cannot hold a block, or a merged
    // request that cannot carry one, does not mean "cache nothing" -- it means
    // "fetch a block and immediately drop it", which is the worst of both.
    normalized.maxRequestBytes = (std::max)(normalized.maxRequestBytes, normalized.blockSize);
    normalized.budgetBytes = (std::max)(normalized.budgetBytes, normalized.blockSize);
    normalized.bypassThresholdBytes =
        (std::max)(normalized.bypassThresholdBytes, normalized.blockSize);

    // A gap wide enough that merging across it could never fit under the
    // request ceiling is not a policy, it is a number that never applies. Cap
    // it where it stops meaning anything, so that a reader of the resolved
    // options sees the gap that is actually in force.
    // Two, not one. Merging across a gap of G blocks puts G + 2 blocks in the
    // request -- the block before the gap, the gap, and the block after it --
    // so the widest gap a request ceiling of N blocks can carry is N - 2. At
    // N - 1 the normalizer advertised a gap `PlanRuns` can never take: with a
    // 4 KiB block and a 16 KiB ceiling it resolved to 3, and a gap of 3 needs
    // 20 KiB. A resolved option that never applies is the thing this cap exists
    // to prevent.
    const std::uint64_t blocksPerRequest = normalized.maxRequestBytes / normalized.blockSize;
    const std::uint64_t gapCeiling = blocksPerRequest >= 2 ? blocksPerRequest - 2 : 0;
    if (normalized.coalesceGapBlocks > gapCeiling) {
        normalized.coalesceGapBlocks = static_cast<std::uint32_t>(
            (std::min)(gapCeiling, static_cast<std::uint64_t>(0xFFFFFFFFu)));
    }

    return normalized;
}

bool CacheOptions::IsNormalized() const noexcept {
    const CacheOptions normalized = Normalized();
    return normalized.blockSize == blockSize && normalized.budgetBytes == budgetBytes &&
           normalized.coalesceGapBlocks == coalesceGapBlocks &&
           normalized.maxRequestBytes == maxRequestBytes &&
           normalized.bypassThresholdBytes == bypassThresholdBytes;
}

}  // namespace cache
}  // namespace usdasset
