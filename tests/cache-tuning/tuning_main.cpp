// SPDX-License-Identifier: Apache-2.0
//
// The measurement that chooses the block policy.
//
// Section 5 of the design policy says cache behavior is measured before it is
// tuned, and CACHE.md section 4 says both coalescing numbers are recorded with
// the measurement that produced them, because "a tuned constant without a
// recorded measurement is a guess with a decimal point". This is that
// measurement: a sweep of block size against coalescing gap over the access
// patterns METRICS.md section 6 names, against a real socket, with every byte
// verified.
//
// It is a sweep and not a benchmark, and the difference matters. What it
// reports -- requests, bytes moved, amplification -- are counts, exact and the
// same on every machine. What it also reports, wall clock, is a fact about
// loopback on the runner that drew the job, and no default is chosen from it:
// loopback has no round-trip time worth the name, and the whole argument for
// merging small reads is about a round-trip time this harness cannot produce.
// So the numbers that choose the defaults are the request counts and the byte
// counts, and the reasoning from them is written down beside the table in
// docs/reference/BLOCK_POLICY.md.
//
// What it asserts, rather than reports, is that every configuration in the
// sweep returns the right bytes. That is the part of this file that is a test:
// the cache runs over a real transport at five block sizes and four gaps, and a
// block boundary that is wrong at one of them fails the lane.
//
// Normative contracts:
//   docs/architecture/CACHE.md      sections 3 and 4, the model and the policy
//   docs/architecture/METRICS.md    section 6, the access patterns
//   docs/design/DESIGN_POLICY.md    section 5, measured before tuned

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "usdAssetCache/BlockCache.h"
#include "usdAssetCache/CacheOptions.h"
#include "usdAssetCache/CachedAssetReader.h"
#include "usdAssetHttp/HttpAssetReader.h"
#include "usdAssetIo/Metrics.h"
#include "usdassetfixture/Server.h"

#include "Check.h"

namespace {

using usdasset::AssetReader;
using usdasset::MetricsSnapshot;
using usdasset::ReadResult;
using usdasset::cache::BlockCache;
using usdasset::cache::CacheOptions;
using usdasset::cache::CachedAssetReader;
using usdasset::http::HttpOpenResult;
using usdassetfixture::AssetSpec;
using usdassetfixture::Behavior;
using usdassetfixture::Server;

using Clock = std::chrono::steady_clock;

/// The same layout the recorded baseline uses, so the two tables are about one
/// fixture and a row here can be read against a row there.
constexpr std::uint64_t kDefaultAssetBytes = 128ull * 1024 * 1024;
constexpr std::uint64_t kMinAssetBytes = 4ull * 1024 * 1024;
constexpr std::uint64_t kHeaderBytes = 4 * 1024;
constexpr std::uint64_t kIndexBytes = 64 * 1024;
constexpr std::uint64_t kIndexReadBytes = 4 * 1024;
constexpr std::uint64_t kChunkBytes = 16 * 1024;
constexpr int kChunkReads = 16;
constexpr std::uint64_t kSequentialChunkBytes = 4ull * 1024 * 1024;

const char kAssetPath[] = "/tuning/asset.bin";

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

bool ReadChecked(AssetReader& reader,
                 std::uint64_t offset,
                 std::uint64_t size,
                 std::vector<unsigned char>* buffer) {
    buffer->assign(static_cast<std::size_t>(size), 0);
    const ReadResult result =
        reader.Read(offset, buffer->data(), static_cast<std::size_t>(size));
    if (!result.status.IsOk() || result.bytesRead != size) {
        std::fprintf(stderr, "FAIL: read at %llu+%llu: %s (%llu bytes)\n",
                     static_cast<unsigned long long>(offset),
                     static_cast<unsigned long long>(size),
                     usdasset::ToString(result.status).c_str(),
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

std::uint64_t ChunkOffset(const Fixture& fixture, int index) {
    const std::uint64_t body = fixture.size - kHeaderBytes - kIndexBytes;
    const std::uint64_t stride = (body - kChunkBytes) / kChunkReads;
    return kHeaderBytes + stride * static_cast<std::uint64_t>(index);
}

/// The access patterns, named the way METRICS.md section 6 names them.
enum class Pattern {
    HeaderAndIndex,
    BoundedQuery,
    /// The one pattern here that METRICS.md section 6 does not name, and the
    /// only one that can measure the coalescing gap at all.
    ///
    /// A gap exists when a read wants blocks that straddle blocks something
    /// already holds, and none of the three patterns above ever produces one:
    /// every block of a contiguous read is wanted unless a previous read left
    /// one resident, and those patterns never revisit a region at a wider
    /// granularity. A format that reads an index in pieces and then re-reads
    /// the region does, so this reads every other piece of the index and then
    /// reads the whole of it. Without that row the gap constant would be
    /// recorded against a sweep in which it provably could not matter, which is
    /// a measurement of nothing presented as a measurement.
    InterleavedIndex,
    FullSequential,
};

const char* PatternName(Pattern pattern) {
    switch (pattern) {
        case Pattern::HeaderAndIndex: return "header and index";
        case Pattern::BoundedQuery: return "bounded query";
        case Pattern::InterleavedIndex: return "interleaved index re-read";
        case Pattern::FullSequential: return "full sequential";
    }
    return "unknown";
}

bool RunPattern(AssetReader& reader, const Fixture& fixture, Pattern pattern) {
    std::vector<unsigned char> buffer;
    switch (pattern) {
        case Pattern::HeaderAndIndex: {
            if (!ReadChecked(reader, 0, kHeaderBytes, &buffer)) return false;
            const int reads = static_cast<int>(kIndexBytes / kIndexReadBytes);
            for (int i = 0; i < reads; ++i) {
                const std::uint64_t offset =
                    fixture.size - kIndexBytes +
                    kIndexReadBytes * static_cast<std::uint64_t>(i);
                if (!ReadChecked(reader, offset, kIndexReadBytes, &buffer)) return false;
            }
            return true;
        }
        case Pattern::BoundedQuery: {
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
        case Pattern::InterleavedIndex: {
            const std::uint64_t base = fixture.size - kIndexBytes;
            const int reads = static_cast<int>(kIndexBytes / kIndexReadBytes);
            for (int i = 0; i < reads; i += 2) {
                const std::uint64_t offset =
                    base + kIndexReadBytes * static_cast<std::uint64_t>(i);
                if (!ReadChecked(reader, offset, kIndexReadBytes, &buffer)) return false;
            }
            // And now the whole region, which wants what it already holds and
            // what it does not, alternating.
            return ReadChecked(reader, base, kIndexBytes, &buffer);
        }
        case Pattern::FullSequential: {
            std::uint64_t offset = 0;
            while (offset < fixture.size) {
                const std::uint64_t size =
                    (std::min)(kSequentialChunkBytes, fixture.size - offset);
                if (!ReadChecked(reader, offset, size, &buffer)) return false;
                offset += size;
            }
            return true;
        }
    }
    return false;
}

struct Row {
    Pattern pattern = Pattern::HeaderAndIndex;
    std::uint64_t blockSize = 0;
    std::uint32_t gap = 0;
    bool cached = false;
    MetricsSnapshot metrics;
    std::uint64_t serverRequests = 0;
    double wallMs = 0.0;
};

/// One run: a fresh reader, a fresh store, and a server log cleared beforehand.
///
/// A fresh store for every row is the point. A sweep that let one configuration
/// warm the next would be measuring the order the rows happen to be in.
Row Measure(const Fixture& fixture,
            Server& server,
            Pattern pattern,
            const CacheOptions& options,
            bool cached) {
    Row row;
    row.pattern = pattern;
    row.blockSize = options.blockSize;
    row.gap = options.coalesceGapBlocks;
    row.cached = cached;

    server.ClearLog();
    const Clock::time_point started = Clock::now();

    HttpOpenResult opened = usdasset::http::Open(fixture.url);
    if (!opened.reader) {
        std::fprintf(stderr, "FAIL: open: %s\n",
                     usdasset::ToString(opened.status).c_str());
        ++usdassettest::FailureCount();
        return row;
    }

    if (!cached) {
        RunPattern(*opened.reader, fixture, pattern);
        row.wallMs =
            std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        row.metrics = opened.reader->Metrics().Snapshot();
        row.serverRequests = static_cast<std::uint64_t>(server.RequestCount());
        return row;
    }

    BlockCache store(options);
    usdasset::ReaderMetrics* innerMetrics = &opened.reader->Metrics();
    usdasset::cache::CachedOpenResult wrapped = usdasset::cache::Wrap(
        std::unique_ptr<AssetReader>(opened.reader.release()), innerMetrics, options,
        &store);
    if (!wrapped.reader) {
        std::fprintf(stderr, "FAIL: wrap: %s\n",
                     usdasset::ToString(wrapped.status).c_str());
        ++usdassettest::FailureCount();
        return row;
    }

    RunPattern(*wrapped.reader, fixture, pattern);
    row.wallMs =
        std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    row.metrics = wrapped.reader->SnapshotMetrics();
    row.serverRequests = static_cast<std::uint64_t>(server.RequestCount());

    // The independent witness, the same one the recorded baseline keeps: the
    // backend's account of what it did, against the server's account of what it
    // answered. A request issued outside the metrics sink costs a round trip and
    // counts nothing, and a sweep watching only the sink would choose a block
    // size from numbers that were wrong in the same direction everywhere.
    CHECK_EQ(row.serverRequests, row.metrics.requestCount);
    return row;
}

std::string HumanBytes(std::uint64_t bytes) {
    char text[64];
    if (bytes >= 1024 * 1024) {
        std::snprintf(text, sizeof(text), "%llu MiB",
                      static_cast<unsigned long long>(bytes / (1024 * 1024)));
    } else {
        std::snprintf(text, sizeof(text), "%llu KiB",
                      static_cast<unsigned long long>(bytes / 1024));
    }
    return text;
}

void PrintTable(const std::vector<Row>& rows, std::uint64_t assetBytes) {
    std::printf("\n### Sweep against a %llu-byte fixture on loopback\n\n",
                static_cast<unsigned long long>(assetBytes));
    std::printf("| Pattern | block | gap | requests | bytes moved | amplification |");
    std::printf(" over-fetch | wall ms |\n");
    std::printf("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n");
    for (const Row& row : rows) {
        if (!row.cached) {
            std::printf("| %s | none | — | %llu | %llu | %.6f | %llu | %.1f |\n",
                        PatternName(row.pattern),
                        static_cast<unsigned long long>(row.metrics.requestCount),
                        static_cast<unsigned long long>(row.metrics.bytesTransferred),
                        row.metrics.Amplification(),
                        static_cast<unsigned long long>(row.metrics.bytesOverFetched),
                        row.wallMs);
            continue;
        }
        std::printf("| %s | %s | %u | %llu | %llu | %.6f | %llu | %.1f |\n",
                    PatternName(row.pattern), HumanBytes(row.blockSize).c_str(),
                    row.gap,
                    static_cast<unsigned long long>(row.metrics.requestCount),
                    static_cast<unsigned long long>(row.metrics.bytesTransferred),
                    row.metrics.Amplification(),
                    static_cast<unsigned long long>(row.metrics.bytesOverFetched),
                    row.wallMs);
    }
}

bool ParseDigits(const char* text, std::uint64_t* out) {
    if (text == nullptr || text[0] == 0) return false;
    for (const char* c = text; *c != 0; ++c) {
        if (*c < '0' || *c > '9') return false;
    }
    *out = std::strtoull(text, nullptr, 10);
    return true;
}

}  // namespace

int main() {
    std::uint64_t assetBytes = kDefaultAssetBytes;
    const char* raw = std::getenv("USD_ASSET_TUNING_ASSET_BYTES");
    if (raw != nullptr && raw[0] != 0) {
        if (!ParseDigits(raw, &assetBytes) || assetBytes < kMinAssetBytes) {
            std::fprintf(stderr,
                         "FAIL: USD_ASSET_TUNING_ASSET_BYTES is not a usable byte "
                         "count: %s\n",
                         raw);
            return 1;
        }
    }

    std::string error;
    std::unique_ptr<Server> server = Server::Start(&error);
    if (!server) {
        std::fprintf(stderr, "FAIL: the fixture server could not bind loopback: %s\n",
                     error.c_str());
        return 1;
    }

    {
        AssetSpec spec;
        spec.path = kAssetPath;
        try {
            spec.content = MakeContent(assetBytes);
        } catch (const std::exception& failure) {
            std::fprintf(stderr, "FAIL: could not allocate the %llu-byte fixture: %s\n",
                         static_cast<unsigned long long>(assetBytes), failure.what());
            server->Stop();
            return 1;
        }
        spec.behavior = Behavior::Normal;
        // Strong, because sharing between readers turns on it and a sweep run
        // with a weak one would be measuring the private-cache path.
        spec.etag = "\"tuning-rev-1\"";
        spec.lastModified = "Thu, 20 Aug 2026 09:00:00 GMT";
        spec.revisedContent.assign(1, 0);
        server->Serve(spec);
    }

    Fixture fixture;
    fixture.url = server->Url(kAssetPath);
    fixture.size = assetBytes;

    const std::vector<std::uint64_t> blockSizes{4ull * 1024, 16ull * 1024,
                                                64ull * 1024, 256ull * 1024,
                                                1024ull * 1024};
    const std::vector<std::uint32_t> gaps{0, 1, 2, 4};

    std::vector<Row> rows;

    // The two clustered patterns, swept. These are the ones the block cache
    // exists for and the ones the defaults are chosen from.
    for (const Pattern pattern : {Pattern::HeaderAndIndex, Pattern::BoundedQuery,
                                 Pattern::InterleavedIndex}) {
        CacheOptions uncached;
        rows.push_back(Measure(fixture, *server, pattern, uncached, false));
        for (const std::uint64_t blockSize : blockSizes) {
            for (const std::uint32_t gap : gaps) {
                CacheOptions options;
                options.blockSize = blockSize;
                options.coalesceGapBlocks = gap;
                options.budgetBytes = 128ull * 1024 * 1024;
                options.maxRequestBytes = 8ull * 1024 * 1024;
                options.bypassThresholdBytes = 1024ull * 1024;
                rows.push_back(
                    Measure(fixture, *server, pattern, options.Normalized(), true));
            }
        }
    }

    // The worst case, swept over block size only. The gap cannot matter here:
    // every read is a streaming read above the bypass threshold, which is the
    // policy this row exists to check has not quietly stopped applying.
    {
        CacheOptions uncached;
        rows.push_back(Measure(fixture, *server, Pattern::FullSequential, uncached,
                               false));
        for (const std::uint64_t blockSize : blockSizes) {
            CacheOptions options;
            options.blockSize = blockSize;
            options.coalesceGapBlocks = 1;
            options.budgetBytes = 128ull * 1024 * 1024;
            options.maxRequestBytes = 8ull * 1024 * 1024;
            options.bypassThresholdBytes = 1024ull * 1024;
            rows.push_back(Measure(fixture, *server, Pattern::FullSequential,
                                   options.Normalized(), true));
        }
    }

    PrintTable(rows, assetBytes);
    server->Stop();
    return usdassettest::Report("usdAssetCache/block-policy-sweep");
}
