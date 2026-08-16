// SPDX-License-Identifier: Apache-2.0

#include "usdAssetIo/Metrics.h"

#include <string>
#include <thread>
#include <vector>

#include "Check.h"

using namespace usdasset;

namespace {

void CountersStartAtZero() {
    const ReaderMetrics metrics("file:///a");
    const MetricsSnapshot snapshot = metrics.Snapshot();
    CHECK_EQ(snapshot.bytesRequested, std::uint64_t{0});
    CHECK_EQ(snapshot.bytesTransferred, std::uint64_t{0});
    CHECK_EQ(snapshot.requestCount, std::uint64_t{0});
}

void IdentifiersAreStoredElided() {
    const ReaderMetrics metrics("https://example.org/a.copc?X-Amz-Signature=deadbeef");
    CHECK(metrics.Snapshot().identifier.find("deadbeef") == std::string::npos);
}

void AMetadataRequestIsAlsoARequest() {
    // requestCount counts transport requests "including metadata requests";
    // a metadata request that did not appear in the total would make the
    // request count a lie about what the network saw.
    ReaderMetrics metrics("file:///a");
    metrics.AddMetadataRequest();
    const MetricsSnapshot snapshot = metrics.Snapshot();
    CHECK_EQ(snapshot.metadataRequestCount, std::uint64_t{1});
    CHECK_EQ(snapshot.requestCount, std::uint64_t{1});
}

void RatiosAreZeroRatherThanInfiniteWhenNothingWasMeasured() {
    const MetricsSnapshot empty;
    CHECK(empty.Amplification() == 0.0);
    CHECK(empty.Selectivity() == 0.0);
    CHECK(empty.CacheHitRatio() == 0.0);
    CHECK(empty.RequestEfficiency() == 0.0);
    CHECK(empty.OverFetchRatio() == 0.0);
}

void RatiosComputeTheHeadlineNumbers() {
    MetricsSnapshot snapshot;
    snapshot.assetSize = 10ull * 1024 * 1024 * 1024;  // 10 GB
    snapshot.bytesRequested = 30ull * 1024 * 1024;    // 30 MB
    snapshot.bytesTransferred = 30ull * 1024 * 1024;
    snapshot.requestCount = 30;

    CHECK(snapshot.Amplification() == 1.0);
    CHECK(snapshot.Selectivity() < 0.003);
    CHECK(snapshot.RequestEfficiency() == 1024.0 * 1024.0);
}

void CountersAreMonotonicAcrossAFailure() {
    // A conditional request the server refused still cost a round trip. A
    // reader whose counters shrink after a failure is reporting what it wishes
    // had happened.
    ReaderMetrics metrics("file:///a");
    metrics.AddRequest();
    metrics.AddBytesTransferred(4096);
    const std::uint64_t before = metrics.Snapshot().bytesTransferred;
    metrics.AddRetry();
    metrics.AddRequest();
    const MetricsSnapshot after = metrics.Snapshot();
    CHECK(after.bytesTransferred >= before);
    CHECK_EQ(after.requestCount, std::uint64_t{2});
    CHECK_EQ(after.retryCount, std::uint64_t{1});
}

void HistogramReportsADistributionNotAMean() {
    LatencyHistogram histogram;
    for (int i = 0; i < 99; ++i) {
        histogram.Record(10);
    }
    histogram.Record(1000000);  // one second, the tail that matters

    const LatencyHistogram::Summary summary = histogram.Snapshot();
    CHECK_EQ(summary.count, std::uint64_t{100});

    // Ninety-nine of a hundred requests really were fast, and the quantiles say
    // so. What a mean would destroy is the other one, which is still visible.
    CHECK(summary.p50 < 100);
    CHECK(summary.p99 < 100);
    CHECK_EQ(summary.max, std::uint64_t{1000000});

    const double mean = static_cast<double>(summary.sum) /
                        static_cast<double>(summary.count);
    CHECK(mean > 1000.0);  // a latency no single request experienced
}

void HistogramFoldPreservesTheDistribution() {
    LatencyHistogram left;
    LatencyHistogram right;
    left.Record(5);
    left.Record(5);
    right.Record(500000);

    left.Fold(right);
    const LatencyHistogram::Summary summary = left.Snapshot();
    CHECK_EQ(summary.count, std::uint64_t{3});
    CHECK_EQ(summary.max, std::uint64_t{500000});
}

void ConcurrentRecordingLosesNothing() {
    ReaderMetrics metrics("file:///a");
    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                metrics.AddBytesRequested(16);
                metrics.AddRequest();
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    const MetricsSnapshot snapshot = metrics.Snapshot();
    CHECK_EQ(snapshot.requestCount, std::uint64_t{kThreads * kPerThread});
    CHECK_EQ(snapshot.bytesRequested, std::uint64_t{kThreads * kPerThread * 16});
}

void ClosingAReaderFoldsIntoTheProcessAggregate() {
    MetricsRegistry& registry = MetricsRegistry::Instance();
    registry.ResetForTesting();

    {
        ReaderMetrics metrics("file:///big.copc");
        metrics.SetAssetSize(1024);
        metrics.AddBytesRequested(256);
        metrics.AddBytesTransferred(256);
        metrics.AddRequest();
        // Still open: the aggregate has not seen it yet.
        CHECK_EQ(registry.Aggregate().bytesTransferred, std::uint64_t{0});
    }

    const MetricsSnapshot aggregate = registry.Aggregate();
    CHECK_EQ(aggregate.bytesTransferred, std::uint64_t{256});
    CHECK_EQ(aggregate.assetSize, std::uint64_t{1024});
    CHECK(aggregate.identifier.empty());
    CHECK(aggregate.Selectivity() == 0.25);

    const std::vector<MetricsSnapshot> top = registry.TopAssetsByBytesTransferred(4);
    CHECK_EQ(top.size(), std::size_t{1});
    if (!top.empty()) {
        CHECK(top.front().identifier == "file:///big.copc");
    }
    registry.ResetForTesting();
}

void TwoReadersOverOneAssetDoNotDoubleItsSize() {
    MetricsRegistry& registry = MetricsRegistry::Instance();
    registry.ResetForTesting();

    for (int i = 0; i < 2; ++i) {
        ReaderMetrics metrics("file:///same.copc");
        metrics.SetAssetSize(1000);
        metrics.AddBytesTransferred(100);
    }

    const std::vector<MetricsSnapshot> top = registry.TopAssetsByBytesTransferred(4);
    CHECK_EQ(top.size(), std::size_t{1});
    if (!top.empty()) {
        CHECK_EQ(top.front().bytesTransferred, std::uint64_t{200});
        CHECK_EQ(top.front().assetSize, std::uint64_t{1000});
    }
    registry.ResetForTesting();
}

}  // namespace

int main() {
    CountersStartAtZero();
    IdentifiersAreStoredElided();
    AMetadataRequestIsAlsoARequest();
    RatiosAreZeroRatherThanInfiniteWhenNothingWasMeasured();
    RatiosComputeTheHeadlineNumbers();
    CountersAreMonotonicAcrossAFailure();
    HistogramReportsADistributionNotAMean();
    HistogramFoldPreservesTheDistribution();
    ConcurrentRecordingLosesNothing();
    ClosingAReaderFoldsIntoTheProcessAggregate();
    TwoReadersOverOneAssetDoNotDoubleItsSize();
    return usdassettest::Report("usdAssetIo metrics");
}
