// SPDX-License-Identifier: Apache-2.0
//
// Block alignment and coalescing, as pure arithmetic.
//
// Internal to the module and deliberately not installed: it is the part of the
// cache that has no state, no lock, and no reader, and it is separated for the
// reason ResolveReadRange is separated in usdAssetIo -- this is where the
// off-by-one lives, and it should be checkable without provisioning an asset.
//
// Normative contract: docs/architecture/CACHE.md §3, §4.

#ifndef USDASSETCACHE_BLOCKPLAN_H
#define USDASSETCACHE_BLOCKPLAN_H

#include <cstdint>
#include <vector>

namespace usdasset {
namespace cache {
namespace detail {

/// The inclusive range of blocks a resolved read touches.
struct BlockSpan {
    std::uint64_t first = 0;
    std::uint64_t last = 0;

    std::uint64_t Count() const noexcept { return last - first + 1; }
};

/// Expands a resolved read to whole blocks.
///
/// `length` must be non-zero and `offset + length` must already have been
/// resolved against the asset size by ResolveReadRange: this function does no
/// EOF reasoning and no overflow checking, because doing either here would be
/// a second copy of the arithmetic the whole project keeps in one place.
BlockSpan CoveringBlocks(std::uint64_t offset,
                         std::uint64_t length,
                         std::uint64_t blockSize) noexcept;

/// The byte extent of one block, clamped to the asset.
///
/// The final block of an asset is short and is stored at its true length. A
/// cache that padded it would answer a read past EOF with zeros, which is a
/// silent corruption that looks exactly like valid data (CACHE.md §3).
struct BlockExtent {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

BlockExtent ExtentOf(std::uint64_t blockIndex,
                     std::uint64_t blockSize,
                     std::uint64_t assetSize) noexcept;

/// One merged fetch: a contiguous byte range covering `blockCount` blocks
/// starting at `firstBlock`, some of which the caller may not want.
struct FetchRun {
    std::uint64_t firstBlock = 0;
    std::uint64_t blockCount = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

/// Merges the blocks a caller must fetch into as few requests as the policy
/// allows.
///
/// `blocks` is ascending and without duplicates. Two blocks separated by no
/// more than `coalesceGapBlocks` blocks the caller does *not* need are merged
/// into one request that fetches the gap too, because transferring the gap
/// costs less than a second round trip; a merge is never taken past
/// `maxRequestBytes`, because one enormous request defeats cancellation and
/// stalls every other read on the connection (CACHE.md §4).
///
/// The bound is never applied so tightly that a single block cannot be
/// fetched: a run of one block is emitted whatever its length, since splitting
/// a block would store a partial one and the store has no way to say so.
std::vector<FetchRun> PlanRuns(const std::vector<std::uint64_t>& blocks,
                               std::uint64_t blockSize,
                               std::uint64_t assetSize,
                               std::uint32_t coalesceGapBlocks,
                               std::uint64_t maxRequestBytes);

/// The bytes of `run` that fall outside `[wantedOffset, wantedOffset + wantedLength)`.
///
/// This is `bytesOverFetched` for one fetch: the cost of block alignment and of
/// merging across a gap, charged where it is incurred. METRICS.md §2.2 calls it
/// the honest counter, and it is charged at fetch time and never refunded when
/// a later read hits those bytes -- that refund is what `cacheHitRatio` is.
std::uint64_t OverFetchedBytes(const FetchRun& run,
                               std::uint64_t wantedOffset,
                               std::uint64_t wantedLength) noexcept;

}  // namespace detail
}  // namespace cache
}  // namespace usdasset

#endif  // USDASSETCACHE_BLOCKPLAN_H
