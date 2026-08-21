// SPDX-License-Identifier: Apache-2.0
//
// Single-flight, and the concurrency around it.
//
// This is the file ThreadSanitizer exists for in this module. Section 5 of
// CACHE.md calls single-flight correctness-adjacent rather than an
// optimization, for two reasons that are both checked here: N Hydra threads
// opening one asset otherwise produce N identical requests, N times the bytes,
// and a metrics report off by a factor of N; and it is where a naive
// implementation deadlocks.
//
// Asserting concurrency properties in prose asserts nothing
// (BOUNDARY_SUITE.md section 5), so every case here races real threads and the
// lane that means anything is `core-tsan`.

#include <atomic>
#include <memory>
#include <string>
#include <thread>
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
using usdassetcachetest::Gate;
using usdassetcachetest::MakeContent;

namespace {

constexpr std::uint64_t kBlock = 4096;
constexpr int kThreads = 8;

CacheOptions TestOptions() {
    CacheOptions options;
    options.blockSize = kBlock;
    options.budgetBytes = 256 * kBlock;
    options.coalesceGapBlocks = 1;
    options.maxRequestBytes = 16 * kBlock;
    options.bypassThresholdBytes = 8 * kBlock;
    return options.Normalized();
}

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
    ReaderMetrics* metrics = &fake->Metrics();
    CachedOpenResult opened =
        Wrap(std::unique_ptr<AssetReader>(fake.release()), metrics, options, &store);
    stack.reader = std::move(opened.reader);
    return stack;
}

bool BytesAreRight(const std::vector<unsigned char>& buffer,
                   std::uint64_t offset,
                   std::size_t length) {
    for (std::size_t i = 0; i < length; ++i) {
        if (buffer[i] != ContentByte(offset + i)) {
            return false;
        }
    }
    return true;
}

void ManyThreadsMissingOneBlockIssueOneRequest() {
    const CacheOptions options = TestOptions();
    BlockCache store(options);
    Stack stack = MakeStack(store, options, "test://oneflight", kBlock * 8,
                            usdassetcachetest::StrongValidator("etag-1"));

    // The owner is held inside the transport, so every other thread that gets
    // as far as the store finds the block claimed and has to wait. Without the
    // latch the case would still be a race worth running; with it, the moment
    // single-flight has to work is guaranteed to happen.
    Gate gate;
    stack.inner->SetGate(&gate);

    std::atomic<int> wrongBytes{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            std::vector<unsigned char> buffer(512, 0);
            const ReadResult result = stack.reader->Read(64, buffer.data(), 512);
            if (!result.status.IsOk() || result.bytesRead != 512) {
                ++failures;
            } else if (!BytesAreRight(buffer, 64, 512)) {
                ++wrongBytes;
            }
        });
    }

    gate.WaitForArrivals(1);
    gate.Open();
    for (std::thread& thread : threads) {
        thread.join();
    }

    CHECK_EQ(failures.load(), 0);
    CHECK_EQ(wrongBytes.load(), 0);
    // The whole point: eight threads, one request, one block moved.
    CHECK_EQ(stack.inner->CallCount(), std::size_t{1});
    CHECK_EQ(stack.inner->BytesRead(), kBlock);

    const MetricsSnapshot snapshot = stack.reader->SnapshotMetrics();
    CHECK_EQ(snapshot.bytesTransferred, kBlock);
    CHECK_EQ(snapshot.requestCount, std::uint64_t{1});
    CHECK(snapshot.requestsSavedBySingleFlight + snapshot.blockHits >= 1);
}

void ManyReadersOfOneRevisionIssueOneRequestBetweenThem() {
    // Not threads on one reader -- separate readers, each with its own
    // transport, as eight parallel opens of one asset actually are. They share
    // because the identity says they may, and for no other reason.
    const CacheOptions options = TestOptions();
    BlockCache store(options);

    std::vector<Stack> stacks;
    stacks.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        stacks.push_back(MakeStack(store, options, "test://parallel", kBlock * 8,
                                   usdassetcachetest::StrongValidator("etag-1")));
    }

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::vector<unsigned char> buffer(256, 0);
            const ReadResult result = stacks[t].reader->Read(0, buffer.data(), 256);
            if (!result.status.IsOk() || !BytesAreRight(buffer, 0, 256)) {
                ++failures;
            }
        });
    }

    // No latch here, and none is needed: whichever reader wins the claim, the
    // others either wait on it or find the block resident, and both outcomes
    // are one request. Two requests would mean the identity did not match.
    for (std::thread& thread : threads) {
        thread.join();
    }

    CHECK_EQ(failures.load(), 0);
    std::size_t totalCalls = 0;
    for (const Stack& stack : stacks) {
        totalCalls += stack.inner->CallCount();
    }
    CHECK_EQ(totalCalls, std::size_t{1});
}

void OverlappingScatteredReadsAgreeOnEveryByte() {
    // The ThreadSanitizer case: threads on one reader over overlapping ranges,
    // crossing block boundaries, with eviction running underneath them because
    // the budget is smaller than the asset.
    CacheOptions options = TestOptions();
    options.budgetBytes = 8 * kBlock;
    options = options.Normalized();

    BlockCache store(options);
    const std::uint64_t assetSize = kBlock * 64 + 123;
    Stack stack = MakeStack(store, options, "test://scatter", assetSize,
                            usdassetcachetest::StrongValidator("etag-1"));

    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::uint64_t seed = 0x1234567ull + static_cast<std::uint64_t>(t);
            std::vector<unsigned char> buffer(9000, 0);
            for (int i = 0; i < 400; ++i) {
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                const std::uint64_t offset = (seed >> 17) % (assetSize + 64);
                const std::size_t size = static_cast<std::size_t>((seed >> 5) % 8192) + 1;
                const ReadResult result = stack.reader->Read(offset, buffer.data(), size);
                if (!result.status.IsOk()) {
                    ++wrong;
                    continue;
                }
                const std::uint64_t expected =
                    offset >= assetSize ? 0
                                        : (std::min)(static_cast<std::uint64_t>(size),
                                                     assetSize - offset);
                if (result.bytesRead != expected ||
                    !BytesAreRight(buffer, offset, result.bytesRead)) {
                    ++wrong;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK_EQ(wrong.load(), 0);
    CHECK(store.Snapshot().residentBytes <= options.budgetBytes);
    CHECK_EQ(store.Snapshot().pendingCount, std::uint64_t{0});
}

void AWaiterRecoversWhenTheOwnersFetchFails() {
    // A block claimed by a fetch that then fails must not fail every reader
    // that was waiting on it: the failure was somebody else's, against
    // somebody else's transport. They acquire again and do the work.
    const CacheOptions options = TestOptions();
    BlockCache store(options);
    Stack stack = MakeStack(store, options, "test://ownerfails", kBlock * 8,
                            usdassetcachetest::StrongValidator("etag-1"));

    Gate gate;
    stack.inner->SetGate(&gate);
    stack.inner->FailNext(1, Status::Error(StatusCode::NetworkError, "reset"));

    std::atomic<int> failures{0};
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            std::vector<unsigned char> buffer(128, 0);
            const ReadResult result = stack.reader->Read(0, buffer.data(), 128);
            if (!result.status.IsOk()) {
                ++failures;
            } else if (!BytesAreRight(buffer, 0, 128)) {
                ++wrong;
            }
        });
    }
    gate.WaitForArrivals(1);
    gate.Open();
    for (std::thread& thread : threads) {
        thread.join();
    }

    // Exactly one read saw the transport failure -- the one that issued it.
    CHECK_EQ(failures.load(), 1);
    CHECK_EQ(wrong.load(), 0);
    CHECK_EQ(store.Snapshot().pendingCount, std::uint64_t{0});
}

}  // namespace

int main() {
    ManyThreadsMissingOneBlockIssueOneRequest();
    ManyReadersOfOneRevisionIssueOneRequestBetweenThem();
    OverlappingScatteredReadsAgreeOnEveryByte();
    AWaiterRecoversWhenTheOwnersFetchFails();
    return usdassettest::Report("usdAssetCache_singleflight");
}
