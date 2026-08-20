// SPDX-License-Identifier: Apache-2.0

#include "BlockPlan.h"

#include <algorithm>

namespace usdasset {
namespace cache {
namespace detail {

BlockSpan CoveringBlocks(std::uint64_t offset,
                         std::uint64_t length,
                         std::uint64_t blockSize) noexcept {
    BlockSpan span;
    span.first = offset / blockSize;
    // The last byte's block, not the block after the end. Computing it from
    // `offset + length` and rounding up would put a read that ends exactly on a
    // boundary into a block it never touches, and that block would be fetched,
    // stored, and counted as over-fetch that never happened.
    span.last = (offset + length - 1) / blockSize;
    return span;
}

BlockExtent ExtentOf(std::uint64_t blockIndex,
                     std::uint64_t blockSize,
                     std::uint64_t assetSize) noexcept {
    BlockExtent extent;
    extent.offset = blockIndex * blockSize;
    if (extent.offset >= assetSize) {
        extent.length = 0;
        return extent;
    }
    const std::uint64_t remaining = assetSize - extent.offset;
    extent.length = (std::min)(blockSize, remaining);
    return extent;
}

std::vector<FetchRun> PlanRuns(const std::vector<std::uint64_t>& blocks,
                               std::uint64_t blockSize,
                               std::uint64_t assetSize,
                               std::uint32_t coalesceGapBlocks,
                               std::uint64_t maxRequestBytes) {
    std::vector<FetchRun> runs;
    if (blocks.empty()) {
        return runs;
    }
    runs.reserve(blocks.size());

    const auto close = [&](std::uint64_t first, std::uint64_t last) {
        const BlockExtent begin = ExtentOf(first, blockSize, assetSize);
        const BlockExtent end = ExtentOf(last, blockSize, assetSize);
        FetchRun run;
        run.firstBlock = first;
        run.blockCount = last - first + 1;
        run.offset = begin.offset;
        run.length = (end.offset + end.length) - begin.offset;
        runs.push_back(run);
    };

    std::uint64_t first = blocks.front();
    std::uint64_t last = first;

    for (std::size_t i = 1; i < blocks.size(); ++i) {
        const std::uint64_t next = blocks[i];
        const std::uint64_t gap = next - last - 1;
        const BlockExtent begin = ExtentOf(first, blockSize, assetSize);
        const BlockExtent end = ExtentOf(next, blockSize, assetSize);
        const std::uint64_t merged = (end.offset + end.length) - begin.offset;

        if (gap <= coalesceGapBlocks && merged <= maxRequestBytes) {
            last = next;
            continue;
        }
        close(first, last);
        first = next;
        last = next;
    }
    close(first, last);
    return runs;
}

std::uint64_t OverFetchedBytes(const FetchRun& run,
                               std::uint64_t wantedOffset,
                               std::uint64_t wantedLength) noexcept {
    const std::uint64_t runEnd = run.offset + run.length;
    const std::uint64_t wantedEnd = wantedOffset + wantedLength;
    const std::uint64_t overlapBegin = (std::max)(run.offset, wantedOffset);
    const std::uint64_t overlapEnd = (std::min)(runEnd, wantedEnd);
    const std::uint64_t overlap = overlapEnd > overlapBegin ? overlapEnd - overlapBegin : 0;
    return run.length - overlap;
}

}  // namespace detail
}  // namespace cache
}  // namespace usdasset
