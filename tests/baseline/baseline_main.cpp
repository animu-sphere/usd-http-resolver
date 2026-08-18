// SPDX-License-Identifier: Apache-2.0
//
// The release's recorded I/O baseline: the five scenarios §6 of METRICS.md
// requires, measured against a fixture large enough for `selectivity` to mean
// something.
//
// This is the last outstanding item of `v0.2.0` and it is a release gate, not a
// benchmark. Gate 6 exists because a resolver that ships correct behavior with
// a silent doubling of transferred bytes has regressed the only property it
// exists to provide, and no functional test would catch it: the boundary suite
// compares bytes against an oracle and would pass every one of them.
//
// So this file asserts *byte counts* and reports *ratios*. The byte counts are
// exact and are the regression gate -- with no cache, a read of n bytes moves
// exactly n bytes in exactly one request, and anything else is either
// over-fetch or a retry nobody asked for. The ratios depend on the fixture size
// and are what a release record quotes. Wall clock is reported and never
// gated: it is a loopback number on whatever runner drew the job.
//
// What is deliberately *not* claimed here: nothing in this file measures a
// network. Loopback has no bandwidth-delay product, and a latency figure from
// it is a figure about this process. What loopback does measure exactly is how
// many bytes and how many requests a pattern costs, which is what gate 6 is
// about and what the architecture's central claim is made of.
//
// Normative contracts:
//   docs/architecture/METRICS.md   §6, the five scenarios and the recording rule
//   docs/releases/README.md        gate 6
//   docs/design/DESIGN_POLICY.md   §9 measurement, §11.3 amplification

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "usdAssetHttp/HttpAssetReader.h"
#include "usdAssetIo/Metrics.h"
#include "usdassetfixture/Corpus.h"
#include "usdassetfixture/Server.h"

#include "Check.h"
#include "RawClient.h"
#include "Report.h"

namespace {

using usdasset::AssetReader;
using usdasset::MetricsRegistry;
using usdasset::MetricsSnapshot;
using usdasset::ReadResult;
using usdasset::http::HttpAssetReader;
using usdasset::http::HttpOpenResult;
using usdassetbaseline::RunContext;
using usdassetbaseline::ScenarioRecord;
using usdassetfixture::AssetSpec;
using usdassetfixture::Behavior;
using usdassetfixture::Server;

using Clock = std::chrono::steady_clock;

// --- the fixture -------------------------------------------------------------

/// The default asset size.
///
/// Chosen so that the bounded query below lands near the ratio §1 of METRICS.md
/// states the architecture's argument as -- three thousandths of the asset --
/// rather than at a number that only says the fixture was small. It is also
/// small enough that a CI cell moves it in about a second over loopback and
/// holds one copy of it in the server. `USD_ASSET_BASELINE_ASSET_BYTES`
/// overrides it, and the sanitizer lanes do exactly that: 128 MiB of
/// instrumented memcpy is a lane that times out rather than a lane that
/// measures.
constexpr std::uint64_t kDefaultAssetBytes = 128ull * 1024 * 1024;

/// Below this the tail index and the scattered chunks stop being distinguishable
/// and the layout means nothing. A run that asks for less is refused rather than
/// quietly resized: a baseline recorded against a fixture nobody chose is worse
/// than no baseline.
constexpr std::uint64_t kMinAssetBytes = 4ull * 1024 * 1024;

constexpr std::uint64_t kHeaderBytes = 4 * 1024;
constexpr std::uint64_t kIndexBytes = 64 * 1024;

/// The index read as the clustered small reads a format actually issues, rather
/// than as one convenient slab. This is the pattern the block cache exists for,
/// and the request count it produces is the number `v0.3.0` has to collapse.
constexpr std::uint64_t kIndexReadBytes = 4 * 1024;
constexpr int kIndexReads = static_cast<int>(kIndexBytes / kIndexReadBytes);

/// The bounded spatial query: a header, an index, and the chunks the index
/// pointed at. Sixteen scattered reads is a query that touched a region, not a
/// query that read the asset.
constexpr std::uint64_t kChunkBytes = 16 * 1024;
constexpr int kChunkReads = 16;

/// The full sequential read's granularity. Large enough that the request count
/// is not the story, small enough that no caller allocates the asset.
constexpr std::uint64_t kSequentialChunkBytes = 4ull * 1024 * 1024;

constexpr int kParallelReaders = 8;

const char kAssetPath[] = "/baseline/asset.bin";

/// Every byte is a hash of its own offset. Positional content with a short
/// period would let a read that landed a block away still compare equal; this
/// one cannot, and it is why every scenario below verifies what it counted. A
/// baseline measured over bytes nobody checked is a measurement of the wrong
/// thing arriving quickly.
unsigned char ByteAt(std::uint64_t offset) noexcept {
    std::uint64_t x = offset + 0x9E3779B97F4A7C15ull;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return static_cast<unsigned char>(x & 0xffull);
}

std::vector<unsigned char> MakeContent(std::uint64_t size) {
    std::vector<unsigned char> content(static_cast<std::size_t>(size));
    for (std::uint64_t i = 0; i < size; ++i) {
        content[static_cast<std::size_t>(i)] = ByteAt(i);
    }
    return content;
}

std::uint64_t AssetBytesFromEnvironment() {
    const char* raw = std::getenv("USD_ASSET_BASELINE_ASSET_BYTES");
    if (raw == nullptr || raw[0] == 0) return kDefaultAssetBytes;

    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (end == raw || end == nullptr || end[0] != 0) {
        std::fprintf(stderr,
                     "FAIL: USD_ASSET_BASELINE_ASSET_BYTES is not a number: %s\n",
                     raw);
        ++usdassettest::FailureCount();
        return kDefaultAssetBytes;
    }
    return static_cast<std::uint64_t>(parsed);
}

// --- reading -----------------------------------------------------------------

double ElapsedMs(Clock::time_point from) {
    return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

/// One read, checked. Returns false having already reported which read failed
/// and how, because "the baseline is wrong" and "the read was wrong" are
/// different findings and only one of them is about I/O volume.
bool ReadChecked(AssetReader& reader,
                 std::uint64_t offset,
                 std::uint64_t size,
                 std::vector<unsigned char>* buffer) {
    buffer->assign(static_cast<std::size_t>(size), 0);
    const ReadResult result =
        reader.Read(offset, buffer->data(), static_cast<std::size_t>(size));

    if (!result.status.IsOk()) {
        std::fprintf(stderr, "FAIL: read at %llu+%llu: %s\n",
                     static_cast<unsigned long long>(offset),
                     static_cast<unsigned long long>(size),
                     usdasset::ToString(result.status).c_str());
        ++usdassettest::FailureCount();
        return false;
    }
    if (result.bytesRead != size) {
        std::fprintf(stderr, "FAIL: read at %llu wanted %llu bytes, got %llu\n",
                     static_cast<unsigned long long>(offset),
                     static_cast<unsigned long long>(size),
                     static_cast<unsigned long long>(result.bytesRead));
        ++usdassettest::FailureCount();
        return false;
    }
    for (std::uint64_t i = 0; i < size; ++i) {
        if ((*buffer)[static_cast<std::size_t>(i)] == ByteAt(offset + i)) continue;
        std::fprintf(stderr,
                     "FAIL: read at %llu+%llu: byte %llu is not the byte that "
                     "belongs at that offset\n",
                     static_cast<unsigned long long>(offset),
                     static_cast<unsigned long long>(size),
                     static_cast<unsigned long long>(i));
        ++usdassettest::FailureCount();
        return false;
    }
    return true;
}

struct Fixture {
    std::string url;
    std::uint64_t size = 0;
};

/// Where the chunk reads land: deterministic, and spread across the body between
/// the header and the tail index, so the pattern is scattered rather than
/// sequential and is the same pattern on every run and every platform.
std::uint64_t ChunkOffset(const Fixture& fixture, int index) {
    const std::uint64_t body = fixture.size - kHeaderBytes - kIndexBytes;
    const std::uint64_t stride = (body - kChunkBytes) / kChunkReads;
    return kHeaderBytes + stride * static_cast<std::uint64_t>(index);
}

/// A header read, an index read, and the chunks the index pointed at. The
/// bounded spatial query, and the shape two of the five scenarios are built out
/// of.
bool ReadBoundedQuery(AssetReader& reader, const Fixture& fixture) {
    std::vector<unsigned char> buffer;
    if (!ReadChecked(reader, 0, kHeaderBytes, &buffer)) return false;
    if (!ReadChecked(reader, fixture.size - kIndexBytes, kIndexBytes, &buffer)) {
        return false;
    }
    for (int i = 0; i < kChunkReads; ++i) {
        if (!ReadChecked(reader, ChunkOffset(fixture, i), kChunkBytes, &buffer)) {
            return false;
        }
    }
    return true;
}

constexpr std::uint64_t kBoundedQueryBytes =
    kHeaderBytes + kIndexBytes + kChunkBytes * kChunkReads;
constexpr std::uint64_t kBoundedQueryReads = 2 + kChunkReads;

std::unique_ptr<HttpAssetReader> OpenOrReport(const Fixture& fixture) {
    // The shipped defaults, deliberately. A baseline measured with deadlines and
    // an attempt count no caller gets is a baseline about a configuration that
    // does not ship.
    HttpOpenResult opened = usdasset::http::Open(fixture.url);
    if (!opened.reader) {
        std::fprintf(stderr, "FAIL: open: %s\n",
                     usdasset::ToString(opened.status).c_str());
        ++usdassettest::FailureCount();
        return nullptr;
    }
    return std::move(opened.reader);
}

/// The counter assertions every scenario shares.
///
/// With no cache, a read of n bytes is one request that moves exactly n bytes.
/// Stating that once means each scenario declares only what is specific to it,
/// and it means the day `v0.3.0` changes the rule, one place says so.
void CheckUncachedShape(const MetricsSnapshot& metrics,
                        std::uint64_t expectedBytes,
                        std::uint64_t expectedReadRequests,
                        std::uint64_t readers) {
    CHECK_EQ(metrics.bytesRequested, expectedBytes);
    CHECK_EQ(metrics.bytesTransferred, expectedBytes);
    CHECK_EQ(metrics.requestCount, expectedReadRequests + readers);
    CHECK_EQ(metrics.metadataRequestCount, readers);
    CHECK_EQ(metrics.retryCount, std::uint64_t{0});
    CHECK_EQ(metrics.redirectCount, std::uint64_t{0});
    CHECK_EQ(metrics.bytesFromCache, std::uint64_t{0});
    CHECK_EQ(metrics.bytesOverFetched, std::uint64_t{0});
}

// --- the scenarios -----------------------------------------------------------

/// METRICS.md §6, row 1: the cost of merely resolving.
ScenarioRecord MetadataOnlyOpen(const Fixture& fixture) {
    ScenarioRecord record;
    record.name = "metadata-only open";
    record.exercises =
        "`openLatency`, `metadataRequestCount` — the cost of merely resolving";

    const Clock::time_point started = Clock::now();
    std::unique_ptr<HttpAssetReader> reader = OpenOrReport(fixture);
    record.wallMs = ElapsedMs(started);
    if (!reader) return record;

    record.metrics = reader->Metrics().Snapshot();

    CHECK_EQ(record.metrics.assetSize, fixture.size);
    CHECK(reader->Metadata().supportsRandomAccess);
    CheckUncachedShape(record.metrics, 0, 0, 1);
    // The one request an open costs is the metadata request, and it moves no
    // content. A backend that read a byte to discover a size would show up here
    // and nowhere else.
    CHECK_EQ(record.metrics.openLatency.count, std::uint64_t{1});

    record.note =
        "One `HEAD`. No content byte crosses the transport, and the reader is "
        "bound to a revision before any read is issued";
    return record;
}

/// METRICS.md §6, row 2: the clustered small-read pattern the block cache exists
/// for.
ScenarioRecord HeaderAndIndexRead(const Fixture& fixture) {
    ScenarioRecord record;
    record.name = "header and index read";
    record.exercises = "The clustered small-read pattern the block cache exists for";

    const Clock::time_point started = Clock::now();
    std::unique_ptr<HttpAssetReader> reader = OpenOrReport(fixture);
    if (!reader) return record;

    std::vector<unsigned char> buffer;
    bool ok = ReadChecked(*reader, 0, kHeaderBytes, &buffer);
    for (int i = 0; ok && i < kIndexReads; ++i) {
        const std::uint64_t offset = fixture.size - kIndexBytes +
                                     kIndexReadBytes * static_cast<std::uint64_t>(i);
        ok = ReadChecked(*reader, offset, kIndexReadBytes, &buffer);
    }
    record.wallMs = ElapsedMs(started);
    record.metrics = reader->Metrics().Snapshot();
    if (!ok) return record;

    CheckUncachedShape(record.metrics, kHeaderBytes + kIndexBytes,
                       1 + static_cast<std::uint64_t>(kIndexReads), 1);

    record.note = "One 4 KiB header read and " + std::to_string(kIndexReads) +
                  " adjacent 4 KiB index reads. Every one of them is its own "
                  "request today, which is the number `v0.3.0` exists to collapse";
    return record;
}

/// METRICS.md §6, row 3: `selectivity`, the headline claim.
ScenarioRecord BoundedSpatialQuery(const Fixture& fixture) {
    ScenarioRecord record;
    record.name = "bounded spatial query";
    record.exercises = "`selectivity` — the headline claim";

    const Clock::time_point started = Clock::now();
    std::unique_ptr<HttpAssetReader> reader = OpenOrReport(fixture);
    if (!reader) return record;

    const bool ok = ReadBoundedQuery(*reader, fixture);
    record.wallMs = ElapsedMs(started);
    record.metrics = reader->Metrics().Snapshot();
    if (!ok) return record;

    CheckUncachedShape(record.metrics, kBoundedQueryBytes, kBoundedQueryReads, 1);

    // The amplification gate, and the reason this file exists. An absolute byte
    // budget rather than a ratio: the ratio moves with the fixture size, and a
    // gate that moved with it would stop catching the thing it is for.
    CHECK(record.metrics.bytesTransferred <= kBoundedQueryBytes);

    record.note = "A header, a tail index, and " + std::to_string(kChunkReads) +
                  " scattered 16 KiB chunks: " + std::to_string(kBoundedQueryBytes) +
                  " bytes moved to answer a query against an asset of " +
                  std::to_string(fixture.size);
    return record;
}

/// METRICS.md §6, row 4: the worst case, which must not be worse than a plain
/// download.
///
/// The comparison is against the fixture server's own raw client -- a naive
/// socket, one `GET`, no `Range` -- rather than against `curl`. That comparator
/// shares no HTTP code with the backend, which is the property that makes the
/// comparison mean anything, and it is already in this repository for that
/// reason. What it is not is a performance-matched client: it reads 4 KiB at a
/// time, so its wall clock flatters the backend and is recorded rather than
/// gated. Its *byte* count flatters nobody, and that is what is gated: a ranged
/// full read must put no more content on the wire than a plain download of the
/// same asset.
ScenarioRecord FullSequentialRead(const Fixture& fixture, unsigned short port) {
    ScenarioRecord record;
    record.name = "full sequential read";
    record.exercises = "The worst case; must not be worse than a plain download";

    const Clock::time_point started = Clock::now();
    std::unique_ptr<HttpAssetReader> reader = OpenOrReport(fixture);
    if (!reader) return record;

    std::vector<unsigned char> buffer;
    bool ok = true;
    std::uint64_t offset = 0;
    std::uint64_t reads = 0;
    while (ok && offset < fixture.size) {
        const std::uint64_t size =
            std::min(kSequentialChunkBytes, fixture.size - offset);
        ok = ReadChecked(*reader, offset, size, &buffer);
        offset += size;
        ++reads;
    }
    record.wallMs = ElapsedMs(started);
    record.metrics = reader->Metrics().Snapshot();
    if (!ok) return record;

    CheckUncachedShape(record.metrics, fixture.size, reads, 1);

    // The plain download, over a client that is not the one under test.
    const Clock::time_point downloadStarted = Clock::now();
    const usdassetfixturetest::RawResponse plain = usdassetfixturetest::FetchOnce(
        port, usdassetfixturetest::GetRequest(kAssetPath), 300000);
    const double downloadMs = ElapsedMs(downloadStarted);

    CHECK_EQ(plain.status, 200);
    CHECK_EQ(static_cast<std::uint64_t>(plain.body.size()), fixture.size);
    // The gate: ranged reading moved no more content than downloading the whole
    // asset in one request did. Headers are outside both sides of it -- the
    // counter is a content counter -- and the request count is where the
    // per-request overhead is visible instead.
    CHECK(record.metrics.bytesTransferred <=
          static_cast<std::uint64_t>(plain.body.size()));

    char note[512];
    std::snprintf(note, sizeof(note),
                  "%llu reads of %llu MiB against one plain `GET` of the whole "
                  "asset over the fixture server's own raw client: identical "
                  "content bytes, %llu requests against 1, %.1f ms against "
                  "%.1f ms. The comparator reads 4 KiB at a time, so the times "
                  "are recorded and not gated",
                  static_cast<unsigned long long>(reads),
                  static_cast<unsigned long long>(kSequentialChunkBytes /
                                                  (1024 * 1024)),
                  static_cast<unsigned long long>(record.metrics.requestCount),
                  record.wallMs, downloadMs);
    record.note = note;
    return record;
}

/// METRICS.md §6, row 5: parallel readers on one asset.
ScenarioRecord ParallelReaders(const Fixture& fixture) {
    ScenarioRecord record;
    record.name = "parallel readers";
    record.exercises = "`requestsSavedBySingleFlight`, contention";

    // The process aggregate rather than one reader's counters, because there are
    // eight of them. Folding is what `~ReaderMetrics` does, so the readers are
    // destroyed before the aggregate is read -- and the registry is cleared
    // first, so nothing an earlier scenario folded lands in this row.
    MetricsRegistry::Instance().ResetForTesting();

    const Clock::time_point started = Clock::now();
    std::vector<std::thread> threads;
    threads.reserve(kParallelReaders);
    for (int i = 0; i < kParallelReaders; ++i) {
        threads.emplace_back([&fixture] {
            std::unique_ptr<HttpAssetReader> reader = OpenOrReport(fixture);
            if (!reader) return;
            ReadBoundedQuery(*reader, fixture);
        });
    }
    for (std::thread& thread : threads) thread.join();
    record.wallMs = ElapsedMs(started);

    record.metrics = MetricsRegistry::Instance().Aggregate();
    // The aggregate summed eight readers' `assetSize`, which is eight times one
    // asset rather than a size anything has. There is one asset here, and
    // `selectivity` is about it.
    record.metrics.assetSize = fixture.size;

    CheckUncachedShape(record.metrics, kBoundedQueryBytes * kParallelReaders,
                       kBoundedQueryReads * kParallelReaders, kParallelReaders);
    CHECK_EQ(record.metrics.requestsSavedBySingleFlight, std::uint64_t{0});

    record.note = std::to_string(kParallelReaders) +
                  " readers running the bounded query at once, each with its own "
                  "revision binding. Every request is issued " +
                  std::to_string(kParallelReaders) +
                  " times, because nothing is shared between readers yet; that is "
                  "the figure `requestsSavedBySingleFlight` has to move in `v0.3.0`";
    return record;
}

// --- output ------------------------------------------------------------------

bool WriteReport(const std::string& path, const std::string& text) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
    return written == text.size();
}

}  // namespace

int main(int argc, char** argv) {
    std::string outputPath;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--output" && i + 1 < argc) {
            outputPath = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [--output <path>]\n", argv[0]);
            return 2;
        }
    }
    if (outputPath.empty()) {
        const char* fromEnvironment = std::getenv("USD_ASSET_BASELINE_OUTPUT");
        if (fromEnvironment != nullptr) outputPath = fromEnvironment;
    }

    const std::uint64_t assetBytes = AssetBytesFromEnvironment();
    if (assetBytes < kMinAssetBytes) {
        std::fprintf(stderr,
                     "FAIL: a baseline asset of %llu bytes is below the %llu-byte "
                     "minimum this layout needs\n",
                     static_cast<unsigned long long>(assetBytes),
                     static_cast<unsigned long long>(kMinAssetBytes));
        return 1;
    }

    std::string error;
    std::unique_ptr<Server> server = Server::Start(&error);
    if (!server) {
        // Reported as what it is. A run that cannot bind loopback must say so
        // rather than report a backend that moved no bytes.
        std::fprintf(stderr,
                     "FAIL: the fixture server could not bind loopback: %s\n",
                     error.c_str());
        return 1;
    }

    {
        AssetSpec spec;
        spec.path = kAssetPath;
        spec.content = MakeContent(assetBytes);
        spec.behavior = Behavior::Normal;
        spec.etag = "\"baseline-rev-1\"";
        spec.lastModified = "Tue, 18 Aug 2026 09:00:00 GMT";
        // Nothing here republishes, and an empty `revisedContent` would make the
        // server hold a second copy of the whole asset for a revision no
        // scenario reaches. One byte says the same thing and costs one byte.
        spec.revisedContent.assign(1, 0);
        server->Serve(spec);
    }

    Fixture fixture;
    fixture.url = server->Url(kAssetPath);
    fixture.size = assetBytes;

    std::vector<ScenarioRecord> records;
    records.push_back(MetadataOnlyOpen(fixture));
    records.push_back(HeaderAndIndexRead(fixture));
    records.push_back(BoundedSpatialQuery(fixture));
    records.push_back(FullSequentialRead(fixture, server->Port()));
    records.push_back(ParallelReaders(fixture));

    RunContext context;
    context.assetBytes = assetBytes;
    context.headerBytes = kHeaderBytes;
    context.indexBytes = kIndexBytes;
    context.assetPath = kAssetPath;
    context.config = USD_ASSET_BASELINE_CONFIG;
    context.toolchain = USD_ASSET_BASELINE_TOOLCHAIN;
    context.platform = USD_ASSET_BASELINE_PLATFORM;

    const std::string report = usdassetbaseline::FormatBaseline(context, records);
    std::fputs(report.c_str(), stdout);

    if (!outputPath.empty() && !WriteReport(outputPath, report)) {
        std::fprintf(stderr, "FAIL: could not write the report to %s\n",
                     outputPath.c_str());
        ++usdassettest::FailureCount();
    }

    server->Stop();
    return usdassettest::Report("usdAssetHttp/io-baseline");
}
