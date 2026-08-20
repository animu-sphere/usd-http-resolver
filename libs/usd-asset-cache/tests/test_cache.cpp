// SPDX-License-Identifier: Apache-2.0
//
// What the cache does to the reader underneath it.
//
// Every case here is a statement about requests and bytes, because that is the
// only thing the cache changes: correctness of the bytes themselves is the
// shared boundary suite's question, and the suite runs against `cache over
// local` unchanged. What cannot be asked there is "how many requests did that
// cost", and that is what this file is.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "usdAssetCache/BlockCache.h"
#include "usdAssetCache/CacheOptions.h"
#include "usdAssetCache/CachedAssetReader.h"

#include "Check.h"
#include "FakeReader.h"

using namespace usdasset;
using namespace usdasset::cache;
using usdassetcachetest::ContentByte;
using usdassetcachetest::FakeReader;
using usdassetcachetest::MakeContent;

namespace {

constexpr std::uint64_t kBlock = 4096;

CacheOptions TestOptions() {
    CacheOptions options;
    options.blockSize = kBlock;
    options.budgetBytes = 64 * kBlock;
    options.coalesceGapBlocks = 1;
    options.maxRequestBytes = 16 * kBlock;
    options.bypassThresholdBytes = 8 * kBlock;
    return options.Normalized();
}

/// A cached reader over a fake one, with the fake still reachable so that a
/// test can ask what the cache actually requested.
struct Stack {
    FakeReader* inner = nullptr;
    std::unique_ptr<CachedAssetReader> reader;
};

Stack MakeStack(BlockCache& store,
                const CacheOptions& options,
                const std::string& identifier,
                std::uint64_t size,
                const Validator& validator) {
    std::unique_ptr<FakeReader> fake(
        new FakeReader(identifier, MakeContent(size), validator));
    Stack stack;
    stack.inner = fake.get();
    ReaderMetrics* innerMetrics = &fake->Metrics();
    CachedOpenResult opened =
        Wrap(std::unique_ptr<AssetReader>(fake.release()), innerMetrics, options, &store);
    stack.reader = std::move(opened.reader);
    return stack;
}

bool ContentMatches(const std::vector<unsigned char>& buffer,
                    std::uint64_t offset,
                    std::size_t length) {
    for (std::size_t i = 0; i < length; ++i) {
        if (buffer[i] != ContentByte(offset + i)) {
            return false;
        }
    }
    return true;
}

/// Reads and checks the bytes, returning the result so a case can assert on it.
ReadResult ReadChecked(AssetReader& reader,
                       std::uint64_t offset,
                       std::size_t length,
                       const char* what) {
    std::vector<unsigned char> buffer(length + 8, 0xEE);
    const ReadResult result = reader.Read(offset, buffer.data(), length);
    if (result.status.IsOk() && !ContentMatches(buffer, offset, result.bytesRead)) {
        std::fprintf(stderr, "FAIL %s: wrong bytes at offset %llu\n", what,
                     static_cast<unsigned long long>(offset));
        ++::usdassettest::FailureCount();
    }
    for (std::size_t i = 0; i < 8; ++i) {
        if (buffer[length + i] != 0xEE) {
            std::fprintf(stderr, "FAIL %s: wrote past the buffer it was given\n", what);
            ++::usdassettest::FailureCount();
            break;
        }
    }
    return result;
}

void ClusteredSmallReadsBecomeOneRequest() {
    // The headline of the release, in one case: sixteen adjacent 256-byte reads
    // inside one block are one fetch, not sixteen.
    const CacheOptions options = TestOptions();
    BlockCache store(options);
    Stack stack = MakeStack(store, options, "test://clustered", kBlock * 8,
                            usdassetcachetest::StrongValidator("etag-1"));

    for (int i = 0; i < 16; ++i) {
        const ReadResult result =
            ReadChecked(*stack.reader, static_cast<std::uint64_t>(i) * 256, 256,
                        "clustered read");
        CHECK(result.status.IsOk());
        CHECK_EQ(result.bytesRead, std::size_t{256});
    }

    CHECK_EQ(stack.inner->CallCount(), std::size_t{1});
    CHECK_EQ(stack.inner->BytesRead(), kBlock);

    const MetricsSnapshot snapshot = stack.reader->SnapshotMetrics();
    CHECK_EQ(snapshot.bytesRequested, std::uint64_t{16 * 256});
    CHECK_EQ(snapshot.bytesTransferred, kBlock);
    CHECK_EQ(snapshot.requestCount, std::uint64_t{1});
    CHECK_EQ(snapshot.blockMisses, std::uint64_t{1});
    // Fifteen of the sixteen reads were answered without touching the reader.
    CHECK_EQ(snapshot.blockHits, std::uint64_t{15});
    CHECK_EQ(snapshot.bytesFromCache, std::uint64_t{15 * 256});
    // The two counters that look contradictory and are not, which is why this
    // case asserts both. `bytesOverFetched` is charged when the block is
    // fetched -- 4096 moved for a 256-byte read -- and it is never refunded
    // when the other fifteen reads consume the rest. `amplification` is the
    // whole read pattern against the whole transfer, and here it lands at
    // exactly 1.0 because the caller did eventually ask for every byte the
    // block alignment moved. The refund lives in `cacheHitRatio`.
    CHECK_EQ(snapshot.bytesOverFetched, kBlock - 256);
    CHECK_EQ(snapshot.Amplification(), 1.0);
    CHECK_EQ(snapshot.CacheHitRatio(), 15.0 / 16.0);
}

void ARereadOfTheSameRangeIssuesNoRequest() {
    const CacheOptions options = TestOptions();
    BlockCache store(options);
    Stack stack = MakeStack(store, options, "test://reread", kBlock * 4,
                            usdassetcachetest::StrongValidator("etag-1"));

    ReadChecked(*stack.reader, kBlock, 100, "first");
    const std::size_t afterFirst = stack.inner->CallCount();
    ReadChecked(*stack.reader, kBlock, 100, "second");
    CHECK_EQ(stack.inner->CallCount(), afterFirst);

    const MetricsSnapshot snapshot = stack.reader->SnapshotMetrics();
    CHECK_EQ(snapshot.blockHits, std::uint64_t{1});
    CHECK_EQ(snapshot.CacheHitRatio(), 0.5);
}

void AReadSpanningACachedAndAnUncachedBlockIsAPartialHit() {
    const CacheOptions options = TestOptions();
    BlockCache store(options);
    Stack stack = MakeStack(store, options, "test://partial", kBlock * 4,
                            usdassetcachetest::StrongValidator("etag-1"));

    ReadChecked(*stack.reader, 0, 16, "warm block 0");
    ReadChecked(*stack.reader, kBlock - 8, 16, "straddle");

    const MetricsSnapshot snapshot = stack.reader->SnapshotMetrics();
    CHECK_EQ(snapshot.partialHits, std::uint64_t{1});
    CHECK_EQ(snapshot.blockMisses, std::uint64_t{2});
}

void AGapIsMergedIntoOneRequestAndCounted() {
    CacheOptions options = TestOptions();
    options.coalesceGapBlocks = 1;
    options = options.Normalized();

    BlockCache store(options);
    Stack stack = MakeStack(store, options, "test://gap", kBlock * 16,
                            usdassetcachetest::StrongValidator("etag-1"));

    // Block 1 is made resident first, so the read that follows wants blocks 0
    // and 2 and has a one-block gap between them -- which is the shape of the
    // example in CACHE.md section 4, and the only way a gap can arise: every
    // block of a contiguous read is wanted unless something already holds it.
    ReadChecked(*stack.reader, kBlock, 16, "warm the gap block");
    CHECK_EQ(stack.inner->CallCount(), std::size_t{1});

    std::vector<unsigned char> buffer(kBlock * 2 + 16, 0);
    const ReadResult result = stack.reader->Read(0, buffer.data(), kBlock * 2 + 16);
    CHECK(result.status.IsOk());
    CHECK_EQ(result.bytesRead, static_cast<std::size_t>(kBlock * 2 + 16));

    // One request for blocks 0..2, which fetches block 1 again although it was
    // already resident. That is the trade the policy names: transferring the
    // gap costs less than a second round trip.
    CHECK_EQ(stack.inner->CallCount(), std::size_t{2});
    const std::vector<FakeReader::Call> calls = stack.inner->Calls();
    CHECK_EQ(calls[1].offset, std::uint64_t{0});
    CHECK_EQ(calls[1].size, static_cast<std::size_t>(kBlock * 3));

    const MetricsSnapshot snapshot = stack.reader->SnapshotMetrics();
    CHECK_EQ(snapshot.blockMisses, std::uint64_t{3});
    CHECK_EQ(snapshot.partialHits, std::uint64_t{1});
    // Two owned blocks that would have been two requests became one.
    CHECK_EQ(snapshot.requestsSavedByCoalescing, std::uint64_t{1});
}

void ALargeReadBypassesTheCacheAndStoresNothing() {
    const CacheOptions options = TestOptions();
    BlockCache store(options);
    Stack stack = MakeStack(store, options, "test://bypass", kBlock * 64,
                            usdassetcachetest::StrongValidator("etag-1"));

    const std::size_t size = static_cast<std::size_t>(options.bypassThresholdBytes);
    const ReadResult result = ReadChecked(*stack.reader, 0, size, "bypass");
    CHECK(result.status.IsOk());
    CHECK_EQ(result.bytesRead, size);

    // Exactly what was asked for, from one request, and nothing resident.
    CHECK_EQ(stack.inner->CallCount(), std::size_t{1});
    CHECK_EQ(stack.inner->BytesRead(), static_cast<std::uint64_t>(size));
    CHECK_EQ(store.Snapshot().blockCount, std::uint64_t{0});

    const MetricsSnapshot snapshot = stack.reader->SnapshotMetrics();
    CHECK_EQ(snapshot.bytesOverFetched, std::uint64_t{0});
    CHECK_EQ(snapshot.Amplification(), 1.0);
}

void TheFinalShortBlockIsServedAtItsTrueLength() {
    const CacheOptions options = TestOptions();
    BlockCache store(options);
    const std::uint64_t assetSize = kBlock * 2 + 17;
    Stack stack = MakeStack(store, options, "test://short-tail", assetSize,
                            usdassetcachetest::StrongValidator("etag-1"));

    // Straddling EOF: the remainder, and Ok. Never seventeen bytes and 4079
    // zeros, which is what a padded final block would produce.
    const ReadResult straddle = ReadChecked(*stack.reader, kBlock * 2, 4096, "straddle EOF");
    CHECK(straddle.status.IsOk());
    CHECK_EQ(straddle.bytesRead, std::size_t{17});

    // And again, from cache this time, with the same answer.
    const ReadResult again = ReadChecked(*stack.reader, kBlock * 2, 4096, "straddle again");
    CHECK(again.status.IsOk());
    CHECK_EQ(again.bytesRead, std::size_t{17});
    CHECK_EQ(stack.inner->CallCount(), std::size_t{1});

    // At EOF: an absence, not an error, and no request.
    unsigned char scratch[8];
    const ReadResult atEnd = stack.reader->Read(assetSize, scratch, sizeof(scratch));
    CHECK(atEnd.status.IsOk());
    CHECK_EQ(atEnd.bytesRead, std::size_t{0});
    CHECK_EQ(stack.inner->CallCount(), std::size_t{1});
}

void EvictionIsInvisibleToCorrectness() {
    CacheOptions options = TestOptions();
    options.budgetBytes = 4 * kBlock;  // one stripe, four blocks
    options = options.Normalized();

    BlockCache store(options);
    Stack stack = MakeStack(store, options, "test://evict", kBlock * 64,
                            usdassetcachetest::StrongValidator("etag-1"));

    for (int i = 0; i < 16; ++i) {
        const ReadResult result = ReadChecked(
            *stack.reader, static_cast<std::uint64_t>(i) * kBlock, 64, "walk");
        CHECK(result.status.IsOk());
    }
    CHECK(store.Snapshot().evictions > 0);
    CHECK(store.Snapshot().residentBytes <= options.budgetBytes);

    // The block that was evicted first is re-fetched rather than served stale
    // or zero-filled, and the bytes are still right.
    const std::size_t before = stack.inner->CallCount();
    const ReadResult again = ReadChecked(*stack.reader, 0, 64, "re-read evicted");
    CHECK(again.status.IsOk());
    CHECK(stack.inner->CallCount() > before);

    const MetricsSnapshot snapshot = stack.reader->SnapshotMetrics();
    CHECK(snapshot.evictions > 0);
    CHECK(snapshot.peakResidentBytes > 0);
}

void TwoReadersOfOneRevisionShareBlocks() {
    const CacheOptions options = TestOptions();
    BlockCache store(options);

    Stack first = MakeStack(store, options, "test://shared", kBlock * 8,
                            usdassetcachetest::StrongValidator("etag-1"));
    ReadChecked(*first.reader, 0, 64, "first reader");
    CHECK_EQ(first.inner->CallCount(), std::size_t{1});

    Stack second = MakeStack(store, options, "test://shared", kBlock * 8,
                             usdassetcachetest::StrongValidator("etag-1"));
    const ReadResult result = ReadChecked(*second.reader, 0, 64, "second reader");
    CHECK(result.status.IsOk());
    // The second reader moved no bytes at all. This is the figure the parallel
    // readers row of the baseline exists to move.
    CHECK_EQ(second.inner->CallCount(), std::size_t{0});
    CHECK_EQ(second.reader->SnapshotMetrics().bytesFromCache, std::uint64_t{64});
    CHECK_EQ(store.Snapshot().identityCount, std::uint64_t{1});
}

void AnEntryFromOneRevisionNeverServesAnother() {
    // The rule the key exists for. Same URL, different validator, and the
    // second reader is not allowed to see the first one's bytes.
    const CacheOptions options = TestOptions();
    BlockCache store(options);

    Stack revisionA = MakeStack(store, options, "test://published", kBlock * 4,
                                usdassetcachetest::StrongValidator("etag-A"));
    ReadChecked(*revisionA.reader, 0, 64, "revision A");
    CHECK_EQ(revisionA.inner->CallCount(), std::size_t{1});

    Stack revisionB = MakeStack(store, options, "test://published", kBlock * 4,
                                usdassetcachetest::StrongValidator("etag-B"));
    ReadChecked(*revisionB.reader, 0, 64, "revision B");
    CHECK_EQ(revisionB.inner->CallCount(), std::size_t{1});
    CHECK_EQ(store.Snapshot().identityCount, std::uint64_t{2});
}

void AWeakValidatorCachesPrivatelyAndDropsOnClose() {
    const CacheOptions options = TestOptions();
    BlockCache store(options);

    {
        Stack weak = MakeStack(store, options, "test://weak", kBlock * 4,
                               usdassetcachetest::WeakValidator("W/etag"));
        CHECK(weak.reader->Binding().IsPrivate());
        ReadChecked(*weak.reader, 0, 64, "weak first");
        ReadChecked(*weak.reader, 0, 64, "weak second");
        // It still caches within the reader's own lifetime: the binding in
        // ASSET_READER.md section 2.1 carries the guarantee there.
        CHECK_EQ(weak.inner->CallCount(), std::size_t{1});

        Stack other = MakeStack(store, options, "test://weak", kBlock * 4,
                                usdassetcachetest::WeakValidator("W/etag"));
        ReadChecked(*other.reader, 0, 64, "weak other reader");
        // But it is not shared: a weak validator cannot prove the two readers
        // are looking at the same bytes.
        CHECK_EQ(other.inner->CallCount(), std::size_t{1});
    }

    // And nothing survives the readers that made it.
    CHECK_EQ(store.Snapshot().blockCount, std::uint64_t{0});
    CHECK_EQ(store.Snapshot().residentBytes, std::uint64_t{0});
    CHECK_EQ(store.ResidentBytes(), std::uint64_t{0});
}

void NoValidatorCachesPrivatelyToo() {
    const CacheOptions options = TestOptions();
    BlockCache store(options);
    Stack none = MakeStack(store, options, "test://novalidator", kBlock * 4,
                           usdassetcachetest::NoValidator());
    CHECK(none.reader->Binding().IsPrivate());
    CHECK_EQ(none.reader->Metadata().stability, IdentityStability::Unavailable);
    ReadChecked(*none.reader, 0, 64, "no validator");
    ReadChecked(*none.reader, 0, 64, "no validator again");
    CHECK_EQ(none.inner->CallCount(), std::size_t{1});
}

void AFailedFetchLeavesNothingPendingAndIsReported() {
    const CacheOptions options = TestOptions();
    BlockCache store(options);
    Stack stack = MakeStack(store, options, "test://failure", kBlock * 4,
                            usdassetcachetest::StrongValidator("etag-1"));

    stack.inner->FailNext(1, Status::Error(StatusCode::NetworkError, "connection reset"));

    std::vector<unsigned char> buffer(64, 0);
    const ReadResult failed = stack.reader->Read(0, buffer.data(), 64);
    CHECK_EQ(failed.status.code, StatusCode::NetworkError);
    CHECK_EQ(failed.bytesRead, std::size_t{0});

    // Nothing is left pending: a block that stayed claimed would make every
    // later reader wait for a fetch that is not happening.
    CHECK_EQ(store.Snapshot().pendingCount, std::uint64_t{0});

    const ReadResult recovered = ReadChecked(*stack.reader, 0, 64, "after failure");
    CHECK(recovered.status.IsOk());
    CHECK_EQ(recovered.bytesRead, std::size_t{64});
}

void CountersAreTheStacksAndNotOneLayers() {
    const CacheOptions options = TestOptions();
    BlockCache store(options);
    Stack stack = MakeStack(store, options, "test://counters", kBlock * 4,
                            usdassetcachetest::StrongValidator("etag-1"));
    ReadChecked(*stack.reader, 0, 100, "counters");

    const MetricsSnapshot snapshot = stack.reader->SnapshotMetrics();
    // The caller's ask, from the cache; the transport's move, from the reader
    // underneath. A snapshot with one and not the other is a ratio between two
    // layers rather than a measurement of a stack.
    CHECK_EQ(snapshot.bytesRequested, std::uint64_t{100});
    CHECK_EQ(snapshot.bytesTransferred, kBlock);
    CHECK_EQ(snapshot.requestCount, std::uint64_t{1});
    CHECK_EQ(snapshot.assetSize, kBlock * 4);
    CHECK(stack.inner->Metrics().IsDetached());
    CHECK(snapshot.identifier.find("test://counters") != std::string::npos);
}

void AnOverflowingRequestIsRefusedBeforeAnythingIsFetched() {
    const CacheOptions options = TestOptions();
    BlockCache store(options);
    Stack stack = MakeStack(store, options, "test://overflow", kBlock * 4,
                            usdassetcachetest::StrongValidator("etag-1"));

    unsigned char scratch[16];
    const ReadResult result = stack.reader->Read(0xFFFFFFFFFFFFFFF0ull, scratch, 32);
    CHECK_EQ(result.status.code, StatusCode::InvalidArgument);
    CHECK_EQ(result.bytesRead, std::size_t{0});
    CHECK_EQ(stack.inner->CallCount(), std::size_t{0});
}

void AZeroLengthReadIssuesNoRequest() {
    const CacheOptions options = TestOptions();
    BlockCache store(options);
    Stack stack = MakeStack(store, options, "test://zero", kBlock * 4,
                            usdassetcachetest::StrongValidator("etag-1"));

    const ReadResult result = stack.reader->Read(0, nullptr, 0);
    CHECK(result.status.IsOk());
    CHECK_EQ(result.bytesRead, std::size_t{0});
    CHECK_EQ(stack.inner->CallCount(), std::size_t{0});
}

void AStoreCannotBeReconfiguredUnderneathALiveBinding() {
    // The process store, deliberately: this is the one call that rebuilds the
    // stripes, and a binding that held a stripe index across the rebuild would
    // be reading a container that no longer exists.
    CacheOptions options = TestOptions();
    CHECK(BlockCache::ConfigureProcess(options));

    std::unique_ptr<FakeReader> fake(new FakeReader(
        "test://process", MakeContent(kBlock), usdassetcachetest::StrongValidator("e")));
    ReaderMetrics* metrics = &fake->Metrics();
    CachedOpenResult opened =
        Wrap(std::unique_ptr<AssetReader>(fake.release()), metrics, options, nullptr);
    CHECK(opened.reader != nullptr);
    CHECK(!BlockCache::ConfigureProcess(options));
    opened.reader.reset();
    CHECK(BlockCache::ConfigureProcess(options));
    BlockCache::Process().ClearForTesting();
}

void WrappingNothingFailsRatherThanCrashing() {
    const CacheOptions options = TestOptions();
    BlockCache store(options);
    CachedOpenResult empty = Wrap(nullptr, nullptr, options, &store);
    CHECK(empty.reader == nullptr);
    CHECK_EQ(empty.status.code, StatusCode::InvalidArgument);

    OpenResult failedOpen;
    failedOpen.status = Status::Error(StatusCode::NotFound, "no such asset");
    OpenResult passedThrough = WrapAsset(std::move(failedOpen), nullptr, options, &store);
    CHECK(passedThrough.reader == nullptr);
    // The backend's status survives; replacing it would erase the only useful
    // thing the result carries.
    CHECK_EQ(passedThrough.status.code, StatusCode::NotFound);
}

}  // namespace

int main() {
    ClusteredSmallReadsBecomeOneRequest();
    ARereadOfTheSameRangeIssuesNoRequest();
    AReadSpanningACachedAndAnUncachedBlockIsAPartialHit();
    AGapIsMergedIntoOneRequestAndCounted();
    ALargeReadBypassesTheCacheAndStoresNothing();
    TheFinalShortBlockIsServedAtItsTrueLength();
    EvictionIsInvisibleToCorrectness();
    TwoReadersOfOneRevisionShareBlocks();
    AnEntryFromOneRevisionNeverServesAnother();
    AWeakValidatorCachesPrivatelyAndDropsOnClose();
    NoValidatorCachesPrivatelyToo();
    AFailedFetchLeavesNothingPendingAndIsReported();
    CountersAreTheStacksAndNotOneLayers();
    AnOverflowingRequestIsRefusedBeforeAnythingIsFetched();
    AZeroLengthReadIssuesNoRequest();
    AStoreCannotBeReconfiguredUnderneathALiveBinding();
    WrappingNothingFailsRatherThanCrashing();
    return usdassettest::Report("usdAssetCache_cache");
}
