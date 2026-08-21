// SPDX-License-Identifier: Apache-2.0
//
// The cache's arithmetic, with no reader, no store, and no thread: block
// alignment, coalescing, the over-fetch charge, option normalization, and key
// identity.
//
// Separated from the reader tests for the reason ResolveReadRange is separated
// from the backends: this is where the off-by-one lives, and it should be
// checkable without provisioning an asset.

#include <string>
#include <vector>

#include "usdAssetCache/CacheKey.h"
#include "usdAssetCache/CacheOptions.h"

#include "BlockPlan.h"
#include "Check.h"

using namespace usdasset;
using namespace usdasset::cache;
using namespace usdasset::cache::detail;

namespace {

constexpr std::uint64_t kBlock = 4096;

void AReadInsideOneBlockCoversOneBlock() {
    const BlockSpan span = CoveringBlocks(10, 20, kBlock);
    CHECK_EQ(span.first, std::uint64_t{0});
    CHECK_EQ(span.last, std::uint64_t{0});
    CHECK_EQ(span.Count(), std::uint64_t{1});
}

void AReadEndingExactlyOnABoundaryDoesNotTouchTheNextBlock() {
    // The case the naive `(offset + length) / blockSize` gets wrong. A read of
    // [0, 4096) ends where block 1 begins and touches none of it; rounding up
    // would fetch block 1, store it, and charge the caller for it.
    const BlockSpan span = CoveringBlocks(0, kBlock, kBlock);
    CHECK_EQ(span.first, std::uint64_t{0});
    CHECK_EQ(span.last, std::uint64_t{0});
}

void AReadStartingExactlyOnABoundaryStartsAtThatBlock() {
    const BlockSpan span = CoveringBlocks(kBlock, 1, kBlock);
    CHECK_EQ(span.first, std::uint64_t{1});
    CHECK_EQ(span.last, std::uint64_t{1});
}

void AReadStraddlingABoundaryCoversBoth() {
    const BlockSpan span = CoveringBlocks(kBlock - 1, 2, kBlock);
    CHECK_EQ(span.first, std::uint64_t{0});
    CHECK_EQ(span.last, std::uint64_t{1});
}

void TheFinalBlockIsShortAndIsNotPadded() {
    // A cache that padded it would answer a read past EOF with zeros, which is
    // a silent corruption that looks exactly like valid data.
    const std::uint64_t assetSize = kBlock * 2 + 17;
    const BlockExtent extent = ExtentOf(2, kBlock, assetSize);
    CHECK_EQ(extent.offset, kBlock * 2);
    CHECK_EQ(extent.length, std::uint64_t{17});

    const BlockExtent whole = ExtentOf(1, kBlock, assetSize);
    CHECK_EQ(whole.length, kBlock);

    const BlockExtent past = ExtentOf(9, kBlock, assetSize);
    CHECK_EQ(past.length, std::uint64_t{0});
}

void AdjacentBlocksBecomeOneRequest() {
    const std::vector<std::uint64_t> blocks{4, 5, 6};
    const std::vector<FetchRun> runs = PlanRuns(blocks, kBlock, kBlock * 100, 0, kBlock * 64);
    CHECK_EQ(runs.size(), std::size_t{1});
    CHECK_EQ(runs[0].firstBlock, std::uint64_t{4});
    CHECK_EQ(runs[0].blockCount, std::uint64_t{3});
    CHECK_EQ(runs[0].offset, kBlock * 4);
    CHECK_EQ(runs[0].length, kBlock * 3);
}

void AGapWiderThanThePolicyIsNotMerged() {
    const std::vector<std::uint64_t> blocks{5, 6, 8};
    const std::vector<FetchRun> split = PlanRuns(blocks, kBlock, kBlock * 100, 0, kBlock * 64);
    CHECK_EQ(split.size(), std::size_t{2});
    CHECK_EQ(split[0].blockCount, std::uint64_t{2});
    CHECK_EQ(split[1].firstBlock, std::uint64_t{8});
}

void AGapInsideThePolicyIsMergedAndFetchesTheGap() {
    // CACHE.md section 4, verbatim: blocks 5, 6 and 8 with a gap of one become
    // one request for blocks 5..8, which fetches block 7 unnecessarily.
    const std::vector<std::uint64_t> blocks{5, 6, 8};
    const std::vector<FetchRun> runs = PlanRuns(blocks, kBlock, kBlock * 100, 1, kBlock * 64);
    CHECK_EQ(runs.size(), std::size_t{1});
    CHECK_EQ(runs[0].firstBlock, std::uint64_t{5});
    CHECK_EQ(runs[0].blockCount, std::uint64_t{4});
    CHECK_EQ(runs[0].length, kBlock * 4);
}

void AMergeIsNeverTakenPastTheRequestCeiling() {
    const std::vector<std::uint64_t> blocks{0, 1, 2, 3};
    const std::vector<FetchRun> runs = PlanRuns(blocks, kBlock, kBlock * 100, 4, kBlock * 2);
    CHECK_EQ(runs.size(), std::size_t{2});
    CHECK_EQ(runs[0].blockCount, std::uint64_t{2});
    CHECK_EQ(runs[1].blockCount, std::uint64_t{2});
}

void ASingleBlockIsAlwaysEmittedWhateverTheCeiling() {
    const std::vector<std::uint64_t> blocks{7};
    const std::vector<FetchRun> runs = PlanRuns(blocks, kBlock, kBlock * 100, 4, 1);
    CHECK_EQ(runs.size(), std::size_t{1});
    CHECK_EQ(runs[0].length, kBlock);
}

void ARunAtTheEndOfTheAssetStopsAtTheEnd() {
    const std::uint64_t assetSize = kBlock * 3 + 100;
    const std::vector<std::uint64_t> blocks{2, 3};
    const std::vector<FetchRun> runs = PlanRuns(blocks, kBlock, assetSize, 0, kBlock * 64);
    CHECK_EQ(runs.size(), std::size_t{1});
    CHECK_EQ(runs[0].length, kBlock + 100);
    CHECK_EQ(runs[0].offset + runs[0].length, assetSize);
}

void OverFetchIsTheBytesTheCallerDidNotTake() {
    FetchRun run;
    run.offset = 0;
    run.length = kBlock * 4;

    // A caller that took 100 bytes out of the middle paid for four blocks.
    CHECK_EQ(OverFetchedBytes(run, 100), kBlock * 4 - 100);
    // A caller that took all of it paid for nothing extra.
    CHECK_EQ(OverFetchedBytes(run, kBlock * 4), std::uint64_t{0});
    // A run nothing was taken out of is over-fetch end to end. This is also the
    // shape of a merge across a resident gap, whose bytes the caller reads from
    // the store rather than out of the transfer that moved them.
    CHECK_EQ(OverFetchedBytes(run, 0), kBlock * 4);
    // Never underflows. `takenBytes` is a sum the caller accumulates, and a
    // counter that wrapped to 18 exabytes would be worse than one reporting 0.
    CHECK_EQ(OverFetchedBytes(run, kBlock * 8), std::uint64_t{0});
}

void OptionsRoundTheBlockSizeDownToAPowerOfTwo() {
    CacheOptions options;
    options.blockSize = 100000;
    const CacheOptions normalized = options.Normalized();
    CHECK_EQ(normalized.blockSize, std::uint64_t{65536});
    // Down, not to the nearest. The risk this cache is exposed to is
    // over-fetch, and rounding up would double the bytes every miss moves.
    CHECK(normalized.blockSize < 100000);
}

void OptionsClampToSomethingUsable() {
    CacheOptions options;
    options.blockSize = 1;
    options.budgetBytes = 1;
    options.maxRequestBytes = 1;
    options.bypassThresholdBytes = 1;
    options.coalesceGapBlocks = 1000;

    const CacheOptions normalized = options.Normalized();
    CHECK_EQ(normalized.blockSize, kMinBlockSize);
    CHECK(normalized.budgetBytes >= normalized.blockSize);
    CHECK(normalized.maxRequestBytes >= normalized.blockSize);
    CHECK(normalized.bypassThresholdBytes >= normalized.blockSize);
    // One block per request leaves no room to merge across anything.
    CHECK_EQ(normalized.coalesceGapBlocks, std::uint32_t{0});
}

void NormalizationIsIdempotent() {
    CacheOptions options;
    options.blockSize = 3 * 4096;
    options.budgetBytes = 7;
    options.coalesceGapBlocks = 99;
    const CacheOptions once = options.Normalized();
    CHECK(once.IsNormalized());
    const CacheOptions twice = once.Normalized();
    CHECK_EQ(twice.blockSize, once.blockSize);
    CHECK_EQ(twice.budgetBytes, once.budgetBytes);
    CHECK_EQ(twice.coalesceGapBlocks, once.coalesceGapBlocks);
    CHECK(CacheOptions().IsNormalized());
}

void EqualIdentifiersNeverImplyEqualContent() {
    // The one line CACHE.md section 6 is made of. Two revisions published at
    // one URL are two cache identities.
    AssetIdentity a;
    a.resolvedIdentifier = "https://example.org/asset.usd";
    a.validator = "etag-A";
    a.blockSize = 65536;

    AssetIdentity b = a;
    b.validator = "etag-B";

    CHECK(a != b);
    CHECK(HashAssetIdentity(a) != HashAssetIdentity(b));

    AssetIdentity same = a;
    CHECK(a == same);
    CHECK_EQ(HashAssetIdentity(a), HashAssetIdentity(same));
}

void TheBlockSizeIsPartOfTheIdentity() {
    AssetIdentity a;
    a.resolvedIdentifier = "https://example.org/asset.usd";
    a.validator = "etag";
    a.blockSize = 65536;

    AssetIdentity b = a;
    b.blockSize = 4096;
    CHECK(a != b);
}

void KeysDifferByBlockIndex() {
    CacheKey first;
    first.identity.resolvedIdentifier = "https://example.org/a";
    first.identity.validator = "etag";
    first.identity.blockSize = 4096;
    first.blockIndex = 3;

    CacheKey second = first;
    second.blockIndex = 4;

    CHECK(first != second);
    CHECK(first == first);
    CHECK(HashCacheKey(first) != HashCacheKey(second));
}

void OnlyAStrongValidatorAdmitsSharing() {
    Validator strong;
    strong.value = "abc";
    strong.kind = ValidatorKind::EntityTag;
    strong.strength = ValidatorStrength::Strong;
    CHECK(IsShareable(strong));

    Validator weak = strong;
    weak.strength = ValidatorStrength::Weak;
    CHECK(!IsShareable(weak));

    Validator none;
    CHECK(!IsShareable(none));

    // A strength without a kind is a producer bug, and resolving it toward the
    // weaker answer is the only safe direction -- the same rule
    // ClassifyStability applies.
    Validator strengthWithoutKind;
    strengthWithoutKind.value = "abc";
    strengthWithoutKind.strength = ValidatorStrength::Strong;
    CHECK(!IsShareable(strengthWithoutKind));
}

}  // namespace

int main() {
    AReadInsideOneBlockCoversOneBlock();
    AReadEndingExactlyOnABoundaryDoesNotTouchTheNextBlock();
    AReadStartingExactlyOnABoundaryStartsAtThatBlock();
    AReadStraddlingABoundaryCoversBoth();
    TheFinalBlockIsShortAndIsNotPadded();
    AdjacentBlocksBecomeOneRequest();
    AGapWiderThanThePolicyIsNotMerged();
    AGapInsideThePolicyIsMergedAndFetchesTheGap();
    AMergeIsNeverTakenPastTheRequestCeiling();
    ASingleBlockIsAlwaysEmittedWhateverTheCeiling();
    ARunAtTheEndOfTheAssetStopsAtTheEnd();
    OverFetchIsTheBytesTheCallerDidNotTake();
    OptionsRoundTheBlockSizeDownToAPowerOfTwo();
    OptionsClampToSomethingUsable();
    NormalizationIsIdempotent();
    EqualIdentifiersNeverImplyEqualContent();
    TheBlockSizeIsPartOfTheIdentity();
    KeysDifferByBlockIndex();
    OnlyAStrongValidatorAdmitsSharing();
    return usdassettest::Report("usdAssetCache_plan");
}
