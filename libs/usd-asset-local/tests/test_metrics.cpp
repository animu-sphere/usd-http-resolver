// SPDX-License-Identifier: Apache-2.0
//
// The counters this backend populates.
//
// v0.1.0's exit criterion is that counters are populated, not that they exist:
// a counter set that compiles and stays at zero is a definition, and the
// amplification argument this project rests on cannot be built out of
// definitions.

#include "usdAssetLocal/LocalAssetReader.h"

#include <string>
#include <vector>

#include "Check.h"
#include "TempAsset.h"

using namespace usdasset;
using usdassettest::PositionalContent;
using usdassettest::TempDirectory;
using usdassettest::WriteFile;

namespace {

void OpenCostsOneMetadataRequestAndNoContent() {
    TempDirectory directory("metrics-open");
    const std::string path = directory.File("asset.bin");
    CHECK(WriteFile(path, PositionalContent(8192)));

    local::LocalOpenResult result = local::Open(path);
    if (!result.reader) {
        CHECK(false);
        return;
    }

    const MetricsSnapshot snapshot = result.reader->Metrics().Snapshot();
    CHECK_EQ(snapshot.assetSize, std::uint64_t{8192});
    CHECK_EQ(snapshot.metadataRequestCount, std::uint64_t{1});
    CHECK_EQ(snapshot.requestCount, std::uint64_t{1});
    CHECK_EQ(snapshot.bytesTransferred, std::uint64_t{0});
    CHECK_EQ(snapshot.bytesRequested, std::uint64_t{0});
    CHECK_EQ(snapshot.openLatency.count, std::uint64_t{1});
}

void AReadPopulatesTheTransferCounters() {
    TempDirectory directory("metrics-read");
    const std::string path = directory.File("asset.bin");
    CHECK(WriteFile(path, PositionalContent(8192)));

    local::LocalOpenResult result = local::Open(path);
    if (!result.reader) {
        CHECK(false);
        return;
    }

    std::vector<unsigned char> buffer(4096);
    const ReadResult read = result.reader->Read(1024, buffer.data(), buffer.size());
    CHECK(read.status.IsOk());
    CHECK_EQ(read.bytesRead, std::size_t{4096});

    const MetricsSnapshot snapshot = result.reader->Metrics().Snapshot();
    CHECK_EQ(snapshot.bytesRequested, std::uint64_t{4096});
    CHECK_EQ(snapshot.bytesTransferred, std::uint64_t{4096});
    CHECK_EQ(snapshot.readLatency.count, std::uint64_t{1});
    CHECK(snapshot.requestLatency.count >= 1);
    // Nothing here is cached, and a counter that reports otherwise would make
    // the cache's own numbers unreadable in v0.3.0.
    CHECK_EQ(snapshot.bytesFromCache, std::uint64_t{0});
    CHECK_EQ(snapshot.retryCount, std::uint64_t{0});
    CHECK_EQ(snapshot.redirectCount, std::uint64_t{0});
    CHECK(snapshot.Amplification() == 1.0);
    CHECK(snapshot.Selectivity() == 0.5);
}

void AnEmptyReadIssuesNoRequest() {
    TempDirectory directory("metrics-empty");
    const std::string path = directory.File("asset.bin");
    CHECK(WriteFile(path, PositionalContent(1024)));

    local::LocalOpenResult result = local::Open(path);
    if (!result.reader) {
        CHECK(false);
        return;
    }
    const std::uint64_t requestsAfterOpen =
        result.reader->Metrics().Snapshot().requestCount;

    unsigned char byte = 0;
    // Zero length, and past EOF: both are "nothing to transfer", and neither
    // may touch the transport.
    CHECK(result.reader->Read(0, &byte, 0).status.IsOk());
    CHECK(result.reader->Read(4096, &byte, 1).status.IsOk());

    const MetricsSnapshot snapshot = result.reader->Metrics().Snapshot();
    CHECK_EQ(snapshot.requestCount, requestsAfterOpen);
    CHECK_EQ(snapshot.bytesTransferred, std::uint64_t{0});
    // The caller did ask for a byte, and that is counted even though no
    // request was issued for it.
    CHECK_EQ(snapshot.bytesRequested, std::uint64_t{1});
}

void CountersSurviveIntoTheProcessAggregate() {
    MetricsRegistry& registry = MetricsRegistry::Instance();
    registry.ResetForTesting();

    TempDirectory directory("metrics-aggregate");
    const std::string path = directory.File("asset.bin");
    CHECK(WriteFile(path, PositionalContent(65536)));

    {
        local::LocalOpenResult result = local::Open(path);
        if (!result.reader) {
            CHECK(false);
            return;
        }
        std::vector<unsigned char> buffer(1024);
        CHECK(result.reader->Read(0, buffer.data(), buffer.size()).status.IsOk());
    }

    const MetricsSnapshot aggregate = registry.Aggregate();
    CHECK_EQ(aggregate.bytesTransferred, std::uint64_t{1024});
    CHECK_EQ(aggregate.assetSize, std::uint64_t{65536});
    // The headline number, on a fixture small enough to check by hand.
    CHECK(aggregate.Selectivity() == 1024.0 / 65536.0);
    registry.ResetForTesting();
}

}  // namespace

int main() {
    OpenCostsOneMetadataRequestAndNoContent();
    AReadPopulatesTheTransferCounters();
    AnEmptyReadIssuesNoRequest();
    CountersSurviveIntoTheProcessAggregate();
    return usdassettest::Report("usdAssetLocal metrics");
}
