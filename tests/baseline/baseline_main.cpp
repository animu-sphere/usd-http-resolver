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
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "usdAssetCache/BlockCache.h"
#include "usdAssetCache/CacheOptions.h"
#include "usdAssetCache/CachedAssetReader.h"
#include "usdAssetCache/DiskBlockStore.h"
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
using usdasset::cache::BlockCache;
using usdasset::cache::CacheOptions;
using usdasset::cache::CachedAssetReader;
using usdasset::http::HttpAssetReader;
using usdasset::http::HttpOpenResult;
using usdassetfixture::RequestRecord;
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

/// And above this, refused for a different reason. The whole fixture is resident
/// three times over at the peak -- the harness builds it, the server holds it,
/// and the plain-download comparator buffers a copy of it -- so a size that
/// cannot fit is a machine swapping or an allocation throwing, neither of which
/// is a measurement. The bound is stated rather than discovered: a number that
/// large is a typo or a mistaken unit far more often than it is a request.
constexpr std::uint64_t kMaxAssetBytes = 4ull * 1024 * 1024 * 1024;

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

/// Digits only, deliberately.
///
/// `strtoull` accepts a leading `-` and returns the two's-complement wrap of it,
/// so `-1` parses as `18446744073709551615` with nothing to test afterwards --
/// and the vector that size then throws `length_error` out of a function that
/// promised to refuse a bad size rather than die of one. It also accepts a
/// leading `+`, whitespace, and `0x`, none of which is a byte count anybody
/// meant. So the digits are checked first and `strtoull` only converts what has
/// already been established to be a number.
bool ParseDigits(const char* text, std::uint64_t* out) {
    if (text == nullptr || text[0] == 0) return false;
    for (const char* c = text; *c != 0; ++c) {
        if (*c < '0' || *c > '9') return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno == ERANGE || end == nullptr || end[0] != 0) return false;
    *out = static_cast<std::uint64_t>(parsed);
    return true;
}

/// False, having already said why. A bad size is refused rather than replaced
/// with the default: a run asked to measure one thing and quietly given another
/// prints a report that is correct about a fixture nobody chose.
bool AssetBytesFromEnvironment(std::uint64_t* out) {
    const char* raw = std::getenv("USD_ASSET_BASELINE_ASSET_BYTES");
    if (raw == nullptr || raw[0] == 0) {
        *out = kDefaultAssetBytes;
        return true;
    }

    std::uint64_t parsed = 0;
    if (!ParseDigits(raw, &parsed)) {
        std::fprintf(stderr,
                     "FAIL: USD_ASSET_BASELINE_ASSET_BYTES is not a count of "
                     "bytes: %s\n",
                     raw);
        return false;
    }
    if (parsed < kMinAssetBytes || parsed > kMaxAssetBytes) {
        std::fprintf(stderr,
                     "FAIL: a baseline asset of %llu bytes is outside the "
                     "%llu..%llu bytes this layout and this machine allow\n",
                     static_cast<unsigned long long>(parsed),
                     static_cast<unsigned long long>(kMinAssetBytes),
                     static_cast<unsigned long long>(kMaxAssetBytes));
        return false;
    }
    *out = parsed;
    return true;
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

/// One reader, with or without the cache over it.
///
/// Every scenario is measured both ways, because METRICS.md section 6 asks a
/// release that changes I/O behavior to record "the counter values before and
/// after" -- and `v0.3.0` is the release that changes them on purpose. A record
/// carrying only the after would leave gate 6 comparing this release's table
/// against a document rather than against a run.
struct Stack {
    std::unique_ptr<BlockCache> store;  ///< Null when uncached, or when shared.
    std::unique_ptr<HttpAssetReader> http;
    std::unique_ptr<CachedAssetReader> cached;
    AssetReader* reader = nullptr;

    MetricsSnapshot Snapshot() const {
        return cached ? cached->SnapshotMetrics() : http->Metrics().Snapshot();
    }
};

/// The cache's shipped defaults, for the reason the transport's are used: a
/// baseline measured with a block size no caller gets is a baseline about a
/// configuration that does not ship. What chose them is
/// docs/reference/BLOCK_POLICY.md.
CacheOptions BaselineCacheOptions() { return CacheOptions().Normalized(); }

/// Opens into `stack`, sharing `store` when one is given -- which is what the
/// parallel-readers scenario needs and what every other scenario must not have.
///
/// `persistent` is null for every scenario but the reopened one. A baseline in
/// which some rows quietly read a disk another row wrote would be a table whose
/// numbers are a function of the order it happens to be written in.
bool OpenStack(const Fixture& fixture, bool cached, BlockCache* shared, Stack* stack,
               usdasset::cache::DiskBlockStore* persistent = nullptr) {
    std::unique_ptr<HttpAssetReader> http = OpenOrReport(fixture);
    if (!http) return false;

    if (!cached) {
        stack->http = std::move(http);
        stack->reader = stack->http.get();
        return true;
    }

    BlockCache* store = shared;
    if (store == nullptr) {
        // A store per scenario, so that no scenario is warmed by the one above
        // it. The process store would make this table a function of the order
        // the rows happen to be written in.
        stack->store.reset(new BlockCache(BaselineCacheOptions()));
        store = stack->store.get();
    }

    usdasset::ReaderMetrics* innerMetrics = &http->Metrics();
    usdasset::cache::CachedOpenResult wrapped = usdasset::cache::Wrap(
        std::unique_ptr<AssetReader>(http.release()), innerMetrics,
        BaselineCacheOptions(), store, persistent);
    if (!wrapped.reader) {
        std::fprintf(stderr, "FAIL: wrap: %s\n",
                     usdasset::ToString(wrapped.status).c_str());
        ++usdassettest::FailureCount();
        return false;
    }
    stack->cached = std::move(wrapped.reader);
    stack->reader = stack->cached.get();
    return true;
}

/// The content bytes the server was asked for, read off its own log.
///
/// The independent witness for a cached run. With no cache, "n bytes requested
/// is n bytes transferred" was itself the check; with one, the expected
/// transfer is a function of the block policy, and asserting the backend's
/// counter against a number this file computed from the same policy would be
/// asserting the policy against itself. The server logged the ranges it
/// answered, and that is a different measurement of the same fact.
std::uint64_t RangeBytesFromLog(const std::vector<RequestRecord>& log) {
    std::uint64_t total = 0;
    for (const RequestRecord& record : log) {
        if (record.range.empty()) continue;  // The HEAD an open costs.
        const std::size_t equals = record.range.find('=');
        if (equals == std::string::npos) continue;
        const std::size_t dash = record.range.find('-', equals + 1);
        if (dash == std::string::npos) continue;
        const std::uint64_t first =
            std::strtoull(record.range.c_str() + equals + 1, nullptr, 10);
        const std::uint64_t last =
            std::strtoull(record.range.c_str() + dash + 1, nullptr, 10);
        if (last < first) continue;
        total += last - first + 1;
    }
    return total;
}

/// The counter assertions the cached runs share.
///
/// Deliberately not the uncached ones with a tolerance added. With a cache the
/// rule "a read of n bytes moves exactly n bytes" is gone by design, so what is
/// asserted here is what is still exactly true: the caller's ask, the metadata
/// cost, that nothing retried or redirected, and that the backend and the
/// server agree about both the request count and the byte count. The rest of
/// the release's claim -- fewer requests than the uncached run -- is asserted
/// per scenario against that scenario's own uncached row, because that
/// comparison is what the claim is made of.
void CheckCachedShape(const MetricsSnapshot& metrics,
                      const std::vector<RequestRecord>& log,
                      std::uint64_t serverRequests,
                      std::uint64_t expectedBytes,
                      std::uint64_t readers) {
    CHECK_EQ(metrics.bytesRequested, expectedBytes);
    CHECK_EQ(metrics.metadataRequestCount, readers);
    CHECK_EQ(metrics.retryCount, std::uint64_t{0});
    CHECK_EQ(metrics.redirectCount, std::uint64_t{0});
    CHECK_EQ(serverRequests, metrics.requestCount);
    CHECK_EQ(metrics.bytesTransferred, RangeBytesFromLog(log));
    CHECK(metrics.bytesFromCache <= metrics.bytesRequested);
}

/// Every counter in METRICS.md §2.2, not the two a scenario happens to think
/// about.
///
/// The report states that all of them are zero. That sentence is a claim, and
/// this file's whole premise is that a claim must be a counter -- so it is
/// checked here, in the one place that would otherwise let `BASELINE.md` assert
/// something true today and false the first time `libs/usd-asset-cache` fills
/// a field nobody was watching. The day that happens this fails, which is the
/// correct outcome: a release that changes I/O behavior records a new baseline
/// rather than inheriting one.
bool NoCacheCounterMoved(const MetricsSnapshot& metrics) {
    return metrics.bytesFromCache == 0 && metrics.blockHits == 0 &&
           metrics.blockMisses == 0 && metrics.partialHits == 0 &&
           metrics.requestsSavedByCoalescing == 0 &&
           metrics.requestsSavedBySingleFlight == 0 &&
           metrics.bytesOverFetched == 0 && metrics.evictions == 0 &&
           metrics.peakResidentBytes == 0 && metrics.persistedHits == 0 &&
           metrics.persistedWrites == 0;
}

/// The counter assertions every scenario shares.
///
/// With no cache, a read of n bytes is one request that moves exactly n bytes.
/// Stating that once means each scenario declares only what is specific to it,
/// and it means the day `v0.3.0` changes the rule, one place says so.
///
/// `serverRequests` is the fixture server's own count, and it is what makes the
/// rest of these worth anything. Every other number here is the backend's
/// account of what the backend did, and gate 6 is exactly the gate that account
/// can be wrong about: a request issued outside the metrics sink costs a round
/// trip and counts nothing, and a lane watching only the sink stays green while
/// the wire traffic doubles. The server logged what it answered, so the two are
/// independent expressions of one fact -- the same separation the plain-download
/// comparator keeps from the client under test, and the boundary suite's oracle
/// keeps from `usdAssetLocal`.
void CheckUncachedShape(const MetricsSnapshot& metrics,
                        std::uint64_t serverRequests,
                        std::uint64_t expectedBytes,
                        std::uint64_t expectedReadRequests,
                        std::uint64_t readers) {
    CHECK_EQ(metrics.bytesRequested, expectedBytes);
    CHECK_EQ(metrics.bytesTransferred, expectedBytes);
    CHECK_EQ(metrics.requestCount, expectedReadRequests + readers);
    CHECK_EQ(metrics.metadataRequestCount, readers);
    CHECK_EQ(metrics.retryCount, std::uint64_t{0});
    CHECK_EQ(metrics.redirectCount, std::uint64_t{0});
    CHECK(NoCacheCounterMoved(metrics));
    CHECK_EQ(serverRequests, metrics.requestCount);
}

/// What the server has answered since its log was last cleared. Read after the
/// reads it is meant to account for, and before anything else on this server
/// issues one -- the plain-download comparator in particular, which is a request
/// the backend did not make and must not be charged for.
std::uint64_t ServerRequests(const Server& server) {
    return static_cast<std::uint64_t>(server.RequestCount());
}

// --- the scenarios -----------------------------------------------------------
//
// Each one runs twice: once against the transport alone, and once with the
// block cache over it. The uncached run keeps the assertions `v0.2.0` recorded
// -- a read of n bytes is one request that moves exactly n bytes -- and is
// still the regression gate for the transport. The cached run asserts what is
// still exactly true with a cache in the stack, and then asserts the release's
// actual claim against the uncached run beside it: fewer requests, and a worst
// case that did not move.

/// The name a row carries in the record. The suffix is part of the name rather
/// than a column, so that a release record's table can be pasted and read
/// without a legend.
std::string RowName(const char* base, bool cached) {
    return cached ? std::string(base) + " (cached)" : std::string(base);
}

/// METRICS.md §6, row 1: the cost of merely resolving.
ScenarioRecord MetadataOnlyOpen(const Fixture& fixture,
                                Server& server,
                                bool cached,
                                const MetricsSnapshot* uncached) {
    ScenarioRecord record;
    record.name = RowName("metadata-only open", cached);
    record.cached = cached;
    record.exercises =
        "`openLatency`, `metadataRequestCount` — the cost of merely resolving";

    server.ClearLog();
    const Clock::time_point started = Clock::now();
    Stack stack;
    if (!OpenStack(fixture, cached, nullptr, &stack)) {
        record.wallMs = ElapsedMs(started);
        return record;
    }
    record.wallMs = ElapsedMs(started);
    record.metrics = stack.Snapshot();

    CHECK_EQ(record.metrics.assetSize, fixture.size);
    CHECK(stack.reader->Metadata().supportsRandomAccess);
    if (cached) {
        CheckCachedShape(record.metrics, server.Log(), ServerRequests(server), 0, 1);
        // A cache changes what a read costs and must not change what an open
        // costs. Wrapping a reader issues no request of its own, and a stack
        // that bound its store by asking the server something would show up
        // here as a second request.
        CHECK_EQ(record.metrics.requestCount, std::uint64_t{1});
        if (uncached != nullptr) {
            CHECK_EQ(record.metrics.requestCount, uncached->requestCount);
        }
    } else {
        CheckUncachedShape(record.metrics, ServerRequests(server), 0, 0, 1);
    }

    // The one request, named by the server rather than by the counter that
    // classified it. "No content byte crosses the transport" is a property of
    // the method that went out, and the wire is the only place to read it off.
    const std::vector<usdassetfixture::RequestRecord> log = server.Log();
    CHECK_EQ(log.size(), std::size_t{1});
    if (log.size() == 1) {
        CHECK(log[0].method == "HEAD");
        CHECK(log[0].range.empty());
    }
    CHECK_EQ(record.metrics.openLatency.count, std::uint64_t{1});

    record.note =
        cached ? "One `HEAD`, unchanged. Binding a store costs no request, which "
                 "is why this row is the one row the cache does not move"
               : "One `HEAD`. No content byte crosses the transport, and the "
                 "reader is bound to a revision before any read is issued";
    return record;
}

/// METRICS.md §6, row 2: the clustered small-read pattern the block cache exists
/// for.
ScenarioRecord HeaderAndIndexRead(const Fixture& fixture,
                                  Server& server,
                                  bool cached,
                                  const MetricsSnapshot* uncached) {
    ScenarioRecord record;
    record.name = RowName("header and index read", cached);
    record.cached = cached;
    record.exercises = "The clustered small-read pattern the block cache exists for";

    server.ClearLog();
    const Clock::time_point started = Clock::now();
    Stack stack;
    if (!OpenStack(fixture, cached, nullptr, &stack)) {
        record.wallMs = ElapsedMs(started);
        return record;
    }

    std::vector<unsigned char> buffer;
    bool ok = ReadChecked(*stack.reader, 0, kHeaderBytes, &buffer);
    for (int i = 0; ok && i < kIndexReads; ++i) {
        const std::uint64_t offset = fixture.size - kIndexBytes +
                                     kIndexReadBytes * static_cast<std::uint64_t>(i);
        ok = ReadChecked(*stack.reader, offset, kIndexReadBytes, &buffer);
    }
    record.wallMs = ElapsedMs(started);
    record.metrics = stack.Snapshot();
    if (!ok) return record;

    if (cached) {
        CheckCachedShape(record.metrics, server.Log(), ServerRequests(server),
                         kHeaderBytes + kIndexBytes, 1);
        // The release's claim, on the pattern it is made about.
        if (uncached != nullptr) {
            CHECK(record.metrics.requestCount < uncached->requestCount);
        }
        CHECK(record.metrics.blockHits > 0);
        record.note =
            "The same seventeen reads, collapsed onto " +
            std::to_string(record.metrics.requestCount - 1) +
            " block fetches. The bytes the alignment moved beyond the reads are "
            "`bytesOverFetched`, and the reads that never reached the transport "
            "are `blockHits`";
        return record;
    }

    CheckUncachedShape(record.metrics, ServerRequests(server),
                       kHeaderBytes + kIndexBytes,
                       1 + static_cast<std::uint64_t>(kIndexReads), 1);
    record.note = "One 4 KiB header read and " + std::to_string(kIndexReads) +
                  " adjacent 4 KiB index reads, each its own request";
    return record;
}

/// METRICS.md §6, row 3: `selectivity`, the headline claim.
ScenarioRecord BoundedSpatialQuery(const Fixture& fixture,
                                   Server& server,
                                   bool cached,
                                   const MetricsSnapshot* uncached) {
    ScenarioRecord record;
    record.name = RowName("bounded spatial query", cached);
    record.cached = cached;
    record.exercises = "`selectivity` — the headline claim";

    server.ClearLog();
    const Clock::time_point started = Clock::now();
    Stack stack;
    if (!OpenStack(fixture, cached, nullptr, &stack)) {
        record.wallMs = ElapsedMs(started);
        return record;
    }

    const bool ok = ReadBoundedQuery(*stack.reader, fixture);
    record.wallMs = ElapsedMs(started);
    record.metrics = stack.Snapshot();
    if (!ok) return record;

    if (cached) {
        CheckCachedShape(record.metrics, server.Log(), ServerRequests(server),
                         kBoundedQueryBytes, 1);
        if (uncached != nullptr) {
            CHECK(record.metrics.requestCount <= uncached->requestCount);
        }
        // The bound this row is gated on now that alignment is allowed to move
        // more than was asked for: one block per read, and no more. A coalescing
        // window that widened, or a read-ahead nobody asked for, is a byte count
        // above this.
        const std::uint64_t blockSize = BaselineCacheOptions().blockSize;
        CHECK(record.metrics.bytesTransferred <=
              kBoundedQueryBytes + blockSize * (kBoundedQueryReads + 1));
        record.note =
            "The same query, with every read expanded to whole blocks: " +
            std::to_string(record.metrics.bytesTransferred) +
            " bytes moved against an asset of " + std::to_string(fixture.size) +
            ". `selectivity` is worse than the uncached row on purpose -- that is "
            "what alignment costs, and `bytesOverFetched` is the counter for it";
        return record;
    }

    CheckUncachedShape(record.metrics, ServerRequests(server), kBoundedQueryBytes,
                       kBoundedQueryReads, 1);
    CHECK(record.metrics.bytesTransferred <= kBoundedQueryBytes);
    record.note = "A header, a tail index, and " + std::to_string(kChunkReads) +
                  " scattered 16 KiB chunks: " + std::to_string(kBoundedQueryBytes) +
                  " bytes moved to answer a query against an asset of " +
                  std::to_string(fixture.size);
    return record;
}

/// The row `v0.4.0` adds: the same bounded query, paid for a second time.
///
/// [BASELINE.md](../../docs/reference/BASELINE.md) named this row before the
/// release that would move it -- "every scenario, on a second open of the same
/// asset: the whole cost again, a new process starts cold" -- and this is where
/// it stops being true for a `Stable` identity.
///
/// The uncached half is the honest comparator: a second reader with no cache
/// pays the query again, exactly. The cached half is a *third* reader, over a
/// block store with nothing in it and a cache directory a previous reader
/// filled. That first reader's numbers are not recorded, because what is being
/// measured is what the second one costs, and a row that averaged the two would
/// be measuring the warm-up.
///
/// A fresh `BlockCache` and not a fresh process, for the reason the module test
/// gives: the property is that nothing in memory carries the answer, and a
/// store with nothing resident is exactly that condition. What a real process
/// boundary additionally proves -- that the file survives one -- is proven where
/// it can be isolated, in `httpResolver_stage`, which re-invokes itself.
ScenarioRecord ReopenedBoundedQuery(const Fixture& fixture,
                                    Server& server,
                                    bool cached,
                                    const MetricsSnapshot* uncached) {
    namespace fs = std::filesystem;

    ScenarioRecord record;
    record.name = RowName("bounded query, reopened", cached);
    record.cached = cached;
    record.exercises = "What a second open costs (CACHE.md §8)";

    if (!cached) {
        server.ClearLog();
        const Clock::time_point started = Clock::now();
        Stack stack;
        if (!OpenStack(fixture, false, nullptr, &stack)) {
            record.wallMs = ElapsedMs(started);
            return record;
        }
        const bool ok = ReadBoundedQuery(*stack.reader, fixture);
        record.wallMs = ElapsedMs(started);
        record.metrics = stack.Snapshot();
        if (!ok) return record;

        CheckUncachedShape(record.metrics, ServerRequests(server), kBoundedQueryBytes,
                           kBoundedQueryReads, 1);
        record.note =
            "A second reader with no cache pays the query again, exactly: " +
            std::to_string(record.metrics.bytesTransferred) +
            " bytes and " + std::to_string(record.metrics.requestCount) +
            " requests, the same as the first";
        return record;
    }

    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    const fs::path directory =
        fs::temp_directory_path() /
        ("usd-http-resolver-baseline-persist-" + std::to_string(now));
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) {
        std::fprintf(stderr, "FAIL: no cache directory: %s\n",
                     error.message().c_str());
        ++usdassettest::FailureCount();
        return record;
    }

    usdasset::cache::DiskCacheOptions persistence;
    persistence.directory = directory.string();
    persistence.budgetBytes = 256ull * 1024 * 1024;
    usdasset::cache::DiskBlockStore disk{persistence};
    if (!disk.IsEnabled()) {
        std::fprintf(stderr, "FAIL: the persistent tier did not open %s\n",
                     persistence.directory.c_str());
        ++usdassettest::FailureCount();
        fs::remove_all(directory, error);
        return record;
    }

    // The warm-up. Its numbers are the cached bounded-query row's numbers and
    // are not recorded again here.
    {
        BlockCache warming{BaselineCacheOptions()};
        Stack stack;
        if (!OpenStack(fixture, true, &warming, &stack, &disk)) {
            fs::remove_all(directory, error);
            return record;
        }
        if (!ReadBoundedQuery(*stack.reader, fixture)) {
            fs::remove_all(directory, error);
            return record;
        }
        CHECK(stack.cached->PersistsBlocks());
        CHECK(stack.Snapshot().persistedWrites > 0);
    }

    server.ClearLog();
    const Clock::time_point started = Clock::now();
    BlockCache cold{BaselineCacheOptions()};
    Stack stack;
    if (!OpenStack(fixture, true, &cold, &stack, &disk)) {
        record.wallMs = ElapsedMs(started);
        fs::remove_all(directory, error);
        return record;
    }
    const bool ok = ReadBoundedQuery(*stack.reader, fixture);
    record.wallMs = ElapsedMs(started);
    record.metrics = stack.Snapshot();
    fs::remove_all(directory, error);
    if (!ok) return record;

    CheckCachedShape(record.metrics, server.Log(), ServerRequests(server),
                     kBoundedQueryBytes, 1);
    // The claim, in three counters. Nothing crossed the wire for the bytes; the
    // one request is the metadata request, which is *supposed* to happen again
    // -- a persistent cache that skipped it would be reusing an identity it had
    // not revalidated; and every byte the caller asked for came from a cache.
    CHECK_EQ(record.metrics.bytesTransferred, std::uint64_t{0});
    CHECK_EQ(record.metrics.requestCount, std::uint64_t{1});
    CHECK_EQ(record.metrics.bytesFromCache, kBoundedQueryBytes);
    CHECK(record.metrics.persistedHits > 0);
    if (uncached != nullptr) {
        CHECK(record.metrics.requestCount < uncached->requestCount);
        CHECK(record.metrics.bytesTransferred < uncached->bytesTransferred);
    }
    record.note =
        "A reader with an empty block store over a cache directory an earlier "
        "reader filled: 0 bytes moved and 1 request, which is the metadata "
        "request. The row above is what the same query costs without it. "
        "`persistedWrites` is 0 here because the writing was done by the "
        "warm-up reader, whose numbers are the cached bounded-query row's. This "
        "holds for a `Stable` identity only -- a `Weak` or absent one neither "
        "writes to that directory nor reads from it";
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
/// gated. Its *byte* count flatters nobody, and that is what is gated.
///
/// This is the row a cache is most able to damage, and the one `BASELINE.md`
/// says must not regress: a cache that turns the worst case into a worse case
/// has the wrong policy. The bypass rule in CACHE.md §3 is what keeps it, and
/// the cached run is where that is checked rather than asserted.
ScenarioRecord FullSequentialRead(const Fixture& fixture,
                                  Server& server,
                                  bool cached,
                                  const MetricsSnapshot* uncached) {
    ScenarioRecord record;
    record.name = RowName("full sequential read", cached);
    record.cached = cached;
    record.exercises = "The worst case; must not be worse than a plain download";

    server.ClearLog();
    const Clock::time_point started = Clock::now();
    Stack stack;
    if (!OpenStack(fixture, cached, nullptr, &stack)) {
        record.wallMs = ElapsedMs(started);
        return record;
    }

    std::vector<unsigned char> buffer;
    bool ok = true;
    std::uint64_t offset = 0;
    std::uint64_t reads = 0;
    while (ok && offset < fixture.size) {
        const std::uint64_t size =
            std::min(kSequentialChunkBytes, fixture.size - offset);
        ok = ReadChecked(*stack.reader, offset, size, &buffer);
        offset += size;
        ++reads;
    }
    record.wallMs = ElapsedMs(started);
    record.metrics = stack.Snapshot();
    if (!ok) return record;

    // Read before the comparator issues its own request, which the backend did
    // not make.
    if (cached) {
        CheckCachedShape(record.metrics, server.Log(), ServerRequests(server),
                         fixture.size, 1);
        if (uncached != nullptr) {
            // Not "no worse by some margin" -- identical. Every read here is
            // larger than `bypassThresholdBytes` and never reaches the store, so
            // a request count or a byte count that differs from the uncached row
            // at all means the bypass stopped applying.
            CHECK_EQ(record.metrics.requestCount, uncached->requestCount);
            CHECK_EQ(record.metrics.bytesTransferred, uncached->bytesTransferred);
        }
        CHECK_EQ(record.metrics.bytesOverFetched, std::uint64_t{0});
        CHECK_EQ(record.metrics.blockMisses, std::uint64_t{0});
    } else {
        CheckUncachedShape(record.metrics, ServerRequests(server), fixture.size,
                           reads, 1);
    }

    // The plain download, over a client that is not the one under test.
    const Clock::time_point downloadStarted = Clock::now();
    const usdassetfixturetest::RawResponse plain = usdassetfixturetest::FetchOnce(
        server.Port(), usdassetfixturetest::GetRequest(kAssetPath), 300000);
    const double downloadMs = ElapsedMs(downloadStarted);

    CHECK_EQ(plain.status, 200);
    CHECK_EQ(static_cast<std::uint64_t>(plain.body.size()), fixture.size);
    CHECK(record.metrics.bytesTransferred <=
          static_cast<std::uint64_t>(plain.body.size()));

    char note[512];
    std::snprintf(note, sizeof(note),
                  "%llu reads of %llu MiB against one plain `GET` of the whole "
                  "asset over the fixture server's own raw client: identical "
                  "content bytes, %llu requests against 1, %.1f ms against "
                  "%.1f ms.%s",
                  static_cast<unsigned long long>(reads),
                  static_cast<unsigned long long>(kSequentialChunkBytes /
                                                  (1024 * 1024)),
                  static_cast<unsigned long long>(record.metrics.requestCount),
                  record.wallMs, downloadMs,
                  cached ? " Every read bypassed the cache, so this row is the "
                           "uncached row and is asserted to be"
                         : " The comparator reads 4 KiB at a time, so the times "
                           "are recorded and not gated");
    record.note = note;
    return record;
}

/// METRICS.md §6, row 5: parallel readers on one asset.
ScenarioRecord ParallelReaders(const Fixture& fixture,
                               Server& server,
                               bool cached,
                               const MetricsSnapshot* uncached) {
    ScenarioRecord record;
    record.name = RowName("parallel readers", cached);
    record.cached = cached;
    record.exercises = "`requestsSavedBySingleFlight`, contention";

    server.ClearLog();

    // The process aggregate rather than one reader's counters, because there are
    // eight of them. Folding is what `~ReaderMetrics` does, so the readers are
    // destroyed before the aggregate is read -- and the registry is cleared
    // first, so nothing an earlier scenario folded lands in this row.
    MetricsRegistry::Instance().ResetForTesting();

    // One store between the eight of them, which is the whole question this row
    // asks. A store per reader would make eight readers of one revision eight
    // times the traffic, which is the number `v0.2.0` recorded and this release
    // exists to move.
    std::unique_ptr<BlockCache> shared;
    if (cached) shared.reset(new BlockCache(BaselineCacheOptions()));

    const Clock::time_point started = Clock::now();
    std::vector<std::thread> threads;
    threads.reserve(kParallelReaders);
    for (int i = 0; i < kParallelReaders; ++i) {
        threads.emplace_back([&fixture, cached, &shared] {
            Stack stack;
            if (!OpenStack(fixture, cached, shared.get(), &stack)) return;
            ReadBoundedQuery(*stack.reader, fixture);
        });
    }
    for (std::thread& thread : threads) thread.join();
    record.wallMs = ElapsedMs(started);

    record.metrics = MetricsRegistry::Instance().Aggregate();
    // The aggregate summed eight readers' `assetSize`, which is eight times one
    // asset rather than a size anything has. There is one asset here, and
    // `selectivity` is about it.
    record.metrics.assetSize = fixture.size;

    if (cached) {
        CheckCachedShape(record.metrics, server.Log(), ServerRequests(server),
                         kBoundedQueryBytes * kParallelReaders, kParallelReaders);
        if (uncached != nullptr) {
            CHECK(record.metrics.requestCount < uncached->requestCount);
        }
        // The counter the row is named after. Zero here would mean eight readers
        // each fetched their own copy and the store's identity did not match --
        // which is the failure this release's cache key exists to prevent.
        CHECK(record.metrics.requestsSavedBySingleFlight +
                  record.metrics.blockHits >
              0);
        record.note =
            std::to_string(kParallelReaders) +
            " readers running the bounded query at once, each with its own "
            "revision binding and all of them sharing one store. What they no "
            "longer share is the traffic: " +
            std::to_string(record.metrics.requestCount) + " requests against " +
            (uncached != nullptr ? std::to_string(uncached->requestCount)
                                 : std::string("the uncached row")) +
            ". `requestsSavedBySingleFlight` is " +
            std::to_string(record.metrics.requestsSavedBySingleFlight) +
            " and `blockHits` is " + std::to_string(record.metrics.blockHits) +
            ": those count blocks a reader did not have to fetch, not requests, "
            "so they do not subtract to the difference above and are not meant to";
        return record;
    }

    CheckUncachedShape(record.metrics, ServerRequests(server),
                       kBoundedQueryBytes * kParallelReaders,
                       kBoundedQueryReads * kParallelReaders, kParallelReaders);
    record.note = std::to_string(kParallelReaders) +
                  " readers running the bounded query at once, each with its own "
                  "revision binding. Every request is issued " +
                  std::to_string(kParallelReaders) +
                  " times, because nothing is shared between readers";
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

    std::uint64_t assetBytes = 0;
    if (!AssetBytesFromEnvironment(&assetBytes)) return 1;

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
        // The one allocation here big enough to fail. A machine that cannot hold
        // the fixture must say which allocation it could not make, rather than
        // terminate on an uncaught `bad_alloc` and leave a lane reporting that
        // the baseline crashed.
        try {
            spec.content = MakeContent(assetBytes);
        } catch (const std::exception& failure) {
            std::fprintf(stderr,
                         "FAIL: could not allocate the %llu-byte fixture: %s\n",
                         static_cast<unsigned long long>(assetBytes),
                         failure.what());
            server->Stop();
            return 1;
        }
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

    // Uncached first, then the same scenario cached, so that each pair sits
    // together in the table and the cached run has the uncached row to assert
    // against. That ordering is the record's whole shape: METRICS.md section 6
    // asks a release that changes I/O behavior for the values before and after,
    // and this release changes them on purpose.
    std::vector<ScenarioRecord> records;
    // Reserved so that the pointer each cached run is given into the row above
    // it cannot be invalidated by the push that follows.
    records.reserve(12);
    records.push_back(MetadataOnlyOpen(fixture, *server, false, nullptr));
    records.push_back(MetadataOnlyOpen(fixture, *server, true, &records[0].metrics));
    records.push_back(HeaderAndIndexRead(fixture, *server, false, nullptr));
    records.push_back(HeaderAndIndexRead(fixture, *server, true, &records[2].metrics));
    records.push_back(BoundedSpatialQuery(fixture, *server, false, nullptr));
    records.push_back(BoundedSpatialQuery(fixture, *server, true, &records[4].metrics));
    records.push_back(ReopenedBoundedQuery(fixture, *server, false, nullptr));
    records.push_back(ReopenedBoundedQuery(fixture, *server, true, &records[6].metrics));
    records.push_back(FullSequentialRead(fixture, *server, false, nullptr));
    records.push_back(FullSequentialRead(fixture, *server, true, &records[8].metrics));
    records.push_back(ParallelReaders(fixture, *server, false, nullptr));
    records.push_back(ParallelReaders(fixture, *server, true, &records[10].metrics));

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
