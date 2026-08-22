// SPDX-License-Identifier: Apache-2.0

#include "usdAssetIo/Metrics.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <ostream>
#include <unordered_map>

#include "usdAssetIo/Diagnostics.h"

namespace usdasset {
namespace {

constexpr std::memory_order kRelaxed = std::memory_order_relaxed;

/// Bucket 0 holds exactly zero; bucket i > 0 holds [2^(i-1), 2^i). The top
/// bucket saturates rather than wrapping.
std::size_t BucketFor(std::uint64_t microseconds) noexcept {
    if (microseconds == 0) {
        return 0;
    }
    std::size_t bucket = 1;
    std::uint64_t bound = 1;
    while (bucket + 1 < LatencyHistogram::kBucketCount && microseconds >= bound * 2) {
        bound *= 2;
        ++bucket;
    }
    return bucket;
}

std::uint64_t BucketUpperBound(std::size_t bucket) noexcept {
    if (bucket == 0) {
        return 0;
    }
    std::uint64_t bound = 1;
    for (std::size_t i = 1; i < bucket; ++i) {
        bound *= 2;
    }
    return bound * 2 - 1;
}

void AddRelaxed(std::atomic<std::uint64_t>& counter, std::uint64_t delta) noexcept {
    counter.fetch_add(delta, kRelaxed);
}

void MaxRelaxed(std::atomic<std::uint64_t>& counter, std::uint64_t value) noexcept {
    std::uint64_t previous = counter.load(kRelaxed);
    while (value > previous &&
           !counter.compare_exchange_weak(previous, value, kRelaxed, kRelaxed)) {
    }
}

double Ratio(std::uint64_t numerator, std::uint64_t denominator) noexcept {
    if (denominator == 0) {
        return 0.0;
    }
    return static_cast<double>(numerator) / static_cast<double>(denominator);
}

bool EnvironmentFlagSet(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return false;
    }
    const bool set = length > 1;  // length counts the terminating null.
    std::free(value);
    return set;
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0';
#endif
}

}  // namespace

// --- LatencyHistogram --------------------------------------------------------

LatencyHistogram::LatencyHistogram() noexcept : _count(0), _sum(0), _max(0) {
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        _buckets[i].store(0, kRelaxed);
    }
}

void LatencyHistogram::Record(std::uint64_t microseconds) noexcept {
    AddRelaxed(_buckets[BucketFor(microseconds)], 1);
    AddRelaxed(_count, 1);
    AddRelaxed(_sum, microseconds);
    MaxRelaxed(_max, microseconds);
}

LatencyHistogram::Summary LatencyHistogram::Snapshot() const noexcept {
    Summary summary;
    summary.count = _count.load(kRelaxed);
    summary.sum = _sum.load(kRelaxed);
    summary.max = _max.load(kRelaxed);
    if (summary.count == 0) {
        return summary;
    }

    // Quantiles are bucket upper bounds. They over-report by at most a factor
    // of two, and they never under-report, which is the correct direction for
    // a latency number somebody is about to act on.
    const std::uint64_t rank50 = (summary.count * 50 + 99) / 100;
    const std::uint64_t rank90 = (summary.count * 90 + 99) / 100;
    const std::uint64_t rank99 = (summary.count * 99 + 99) / 100;

    // Explicit "found" flags rather than testing for a zero quantile: bucket 0
    // has an upper bound of zero, which is the honest answer for
    // sub-microsecond work and must not read as "not yet assigned".
    bool found50 = false;
    bool found90 = false;
    bool found99 = false;

    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        cumulative += _buckets[i].load(kRelaxed);
        const std::uint64_t bound = BucketUpperBound(i);
        if (!found50 && cumulative >= rank50) {
            summary.p50 = bound;
            found50 = true;
        }
        if (!found90 && cumulative >= rank90) {
            summary.p90 = bound;
            found90 = true;
        }
        if (!found99 && cumulative >= rank99) {
            summary.p99 = bound;
            found99 = true;
        }
    }

    // A bucket's upper bound can exceed every sample it holds. Reporting a p99
    // above the observed maximum is not a rounding artifact a reader should
    // have to explain.
    summary.p50 = (std::min)(summary.p50, summary.max);
    summary.p90 = (std::min)(summary.p90, summary.max);
    summary.p99 = (std::min)(summary.p99, summary.max);
    return summary;
}

void LatencyHistogram::Reset() noexcept {
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        _buckets[i].store(0, kRelaxed);
    }
    _count.store(0, kRelaxed);
    _sum.store(0, kRelaxed);
    _max.store(0, kRelaxed);
}

void LatencyHistogram::Fold(const LatencyHistogram& other) noexcept {
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        AddRelaxed(_buckets[i], other._buckets[i].load(kRelaxed));
    }
    AddRelaxed(_count, other._count.load(kRelaxed));
    AddRelaxed(_sum, other._sum.load(kRelaxed));
    MaxRelaxed(_max, other._max.load(kRelaxed));
}

// --- MetricsSnapshot ---------------------------------------------------------

double MetricsSnapshot::Amplification() const noexcept {
    return Ratio(bytesTransferred, bytesRequested);
}

double MetricsSnapshot::OverFetchRatio() const noexcept {
    return Ratio(bytesOverFetched, bytesTransferred);
}

double MetricsSnapshot::CacheHitRatio() const noexcept {
    return Ratio(bytesFromCache, bytesRequested);
}

double MetricsSnapshot::RequestEfficiency() const noexcept {
    return Ratio(bytesTransferred, requestCount);
}

double MetricsSnapshot::Selectivity() const noexcept {
    return Ratio(bytesTransferred, assetSize);
}

void MetricsSnapshot::AbsorbTransport(const MetricsSnapshot& inner) {
    // The asset is one asset however many readers are stacked over it, so its
    // size is taken and not summed.
    assetSize = (std::max)(assetSize, inner.assetSize);

    bytesTransferred += inner.bytesTransferred;
    requestCount += inner.requestCount;
    metadataRequestCount += inner.metadataRequestCount;
    retryCount += inner.retryCount;
    redirectCount += inner.redirectCount;

    // Not taken: bytesRequested, bytesFromCache, and every cache counter. Those
    // belong to the layer that faces the caller, and the inner reader's
    // bytesRequested is the expanded ask this cache made of it -- already
    // visible, exactly once, as bytesTransferred.
    openLatency = inner.openLatency;
    requestLatency = inner.requestLatency;
}

void MetricsSnapshot::Add(const MetricsSnapshot& other) {
    assetSize += other.assetSize;
    bytesRequested += other.bytesRequested;
    bytesTransferred += other.bytesTransferred;
    bytesFromCache += other.bytesFromCache;
    requestCount += other.requestCount;
    metadataRequestCount += other.metadataRequestCount;
    retryCount += other.retryCount;
    redirectCount += other.redirectCount;

    blockHits += other.blockHits;
    blockMisses += other.blockMisses;
    partialHits += other.partialHits;
    requestsSavedByCoalescing += other.requestsSavedByCoalescing;
    requestsSavedBySingleFlight += other.requestsSavedBySingleFlight;
    bytesOverFetched += other.bytesOverFetched;
    evictions += other.evictions;
    peakResidentBytes = (std::max)(peakResidentBytes, other.peakResidentBytes);

    const auto mergeLatency = [](LatencyHistogram::Summary& into,
                                 const LatencyHistogram::Summary& from) {
        into.count += from.count;
        into.sum += from.sum;
        into.max = (std::max)(into.max, from.max);
        into.p50 = 0;
        into.p90 = 0;
        into.p99 = 0;
    };
    mergeLatency(openLatency, other.openLatency);
    mergeLatency(requestLatency, other.requestLatency);
    mergeLatency(readLatency, other.readLatency);
}

// --- ReaderMetrics -----------------------------------------------------------

ReaderMetrics::ReaderMetrics(std::string identifier)
    : _identifier(ElideSecrets(identifier)) {
    // Touch the registry here so that it exists before this object does, and so
    // that the opt-in dump is armed before any counter can be folded into it.
    //
    // It is not what makes `~ReaderMetrics` safe. Construction order cannot,
    // because a reader is not required to be destroyed before the objects that
    // were constructed before it: a resolver holding a retained open destroys
    // its reader from a static destructor of its own, arbitrarily late. What
    // makes it safe is that the aggregate is never destroyed at all -- see
    // `MetricsRegistry::Instance`.
    (void)MetricsRegistry::Instance();
}

ReaderMetrics::~ReaderMetrics() {
    if (_detached.load(kRelaxed)) {
        return;
    }
    MetricsRegistry::Instance().Fold(*this);
}

void ReaderMetrics::SetAssetSize(std::uint64_t bytes) noexcept {
    _assetSize.store(bytes, kRelaxed);
}

void ReaderMetrics::AddBytesRequested(std::uint64_t bytes) noexcept {
    AddRelaxed(_bytesRequested, bytes);
}

void ReaderMetrics::AddBytesTransferred(std::uint64_t bytes) noexcept {
    AddRelaxed(_bytesTransferred, bytes);
}

void ReaderMetrics::AddBytesFromCache(std::uint64_t bytes) noexcept {
    AddRelaxed(_bytesFromCache, bytes);
}

void ReaderMetrics::AddRequest(std::uint64_t count) noexcept {
    AddRelaxed(_requestCount, count);
}

void ReaderMetrics::AddMetadataRequest(std::uint64_t count) noexcept {
    AddRelaxed(_metadataRequestCount, count);
    AddRelaxed(_requestCount, count);
}

void ReaderMetrics::AddRetry(std::uint64_t count) noexcept {
    AddRelaxed(_retryCount, count);
}

void ReaderMetrics::AddRedirect(std::uint64_t count) noexcept {
    AddRelaxed(_redirectCount, count);
}

void ReaderMetrics::AddBlockHit(std::uint64_t count) noexcept {
    AddRelaxed(_blockHits, count);
}

void ReaderMetrics::AddBlockMiss(std::uint64_t count) noexcept {
    AddRelaxed(_blockMisses, count);
}

void ReaderMetrics::AddPartialHit(std::uint64_t count) noexcept {
    AddRelaxed(_partialHits, count);
}

void ReaderMetrics::AddRequestsSavedByCoalescing(std::uint64_t count) noexcept {
    AddRelaxed(_requestsSavedByCoalescing, count);
}

void ReaderMetrics::AddRequestsSavedBySingleFlight(std::uint64_t count) noexcept {
    AddRelaxed(_requestsSavedBySingleFlight, count);
}

void ReaderMetrics::AddBytesOverFetched(std::uint64_t bytes) noexcept {
    AddRelaxed(_bytesOverFetched, bytes);
}

void ReaderMetrics::AddEviction(std::uint64_t count) noexcept {
    AddRelaxed(_evictions, count);
}

void ReaderMetrics::ObserveResidentBytes(std::uint64_t bytes) noexcept {
    MaxRelaxed(_peakResidentBytes, bytes);
}

void ReaderMetrics::AbsorbTransport(const ReaderMetrics& inner) noexcept {
    MaxRelaxed(_assetSize, inner._assetSize.load(kRelaxed));
    AddRelaxed(_bytesTransferred, inner._bytesTransferred.load(kRelaxed));
    // Deliberately not through AddMetadataRequest, which also bumps the request
    // total: the inner reader already counted its metadata requests there, and
    // routing them through that helper would count each one twice.
    AddRelaxed(_requestCount, inner._requestCount.load(kRelaxed));
    AddRelaxed(_metadataRequestCount, inner._metadataRequestCount.load(kRelaxed));
    AddRelaxed(_retryCount, inner._retryCount.load(kRelaxed));
    AddRelaxed(_redirectCount, inner._redirectCount.load(kRelaxed));

    // Folded bucket by bucket rather than merged as summaries, which is the
    // only way the quantiles survive.
    _openLatency.Fold(inner._openLatency);
    _requestLatency.Fold(inner._requestLatency);
}

void ReaderMetrics::DetachFromRegistry() noexcept { _detached.store(true, kRelaxed); }

bool ReaderMetrics::IsDetached() const noexcept { return _detached.load(kRelaxed); }

MetricsSnapshot ReaderMetrics::Snapshot() const {
    MetricsSnapshot snapshot;
    snapshot.identifier = _identifier;
    snapshot.assetSize = _assetSize.load(kRelaxed);
    snapshot.bytesRequested = _bytesRequested.load(kRelaxed);
    snapshot.bytesTransferred = _bytesTransferred.load(kRelaxed);
    snapshot.bytesFromCache = _bytesFromCache.load(kRelaxed);
    snapshot.requestCount = _requestCount.load(kRelaxed);
    snapshot.metadataRequestCount = _metadataRequestCount.load(kRelaxed);
    snapshot.retryCount = _retryCount.load(kRelaxed);
    snapshot.redirectCount = _redirectCount.load(kRelaxed);
    snapshot.blockHits = _blockHits.load(kRelaxed);
    snapshot.blockMisses = _blockMisses.load(kRelaxed);
    snapshot.partialHits = _partialHits.load(kRelaxed);
    snapshot.requestsSavedByCoalescing = _requestsSavedByCoalescing.load(kRelaxed);
    snapshot.requestsSavedBySingleFlight = _requestsSavedBySingleFlight.load(kRelaxed);
    snapshot.bytesOverFetched = _bytesOverFetched.load(kRelaxed);
    snapshot.evictions = _evictions.load(kRelaxed);
    snapshot.peakResidentBytes = _peakResidentBytes.load(kRelaxed);
    snapshot.openLatency = _openLatency.Snapshot();
    snapshot.requestLatency = _requestLatency.Snapshot();
    snapshot.readLatency = _readLatency.Snapshot();
    return snapshot;
}

// --- ScopedLatency -----------------------------------------------------------

namespace {

std::uint64_t NowNanoseconds() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

ScopedLatency::ScopedLatency(LatencyHistogram& histogram) noexcept
    : _histogram(histogram), _startNanoseconds(NowNanoseconds()) {}

ScopedLatency::~ScopedLatency() {
    const std::uint64_t end = NowNanoseconds();
    const std::uint64_t elapsed = end > _startNanoseconds ? end - _startNanoseconds : 0;
    _histogram.Record(elapsed / 1000);
}

// --- MetricsRegistry ---------------------------------------------------------

class MetricsRegistry::Impl {
public:
    /// Keeps std::cerr alive for the opt-in dump, which runs from `std::atexit`
    /// during static destruction and would otherwise race the standard streams'
    /// own teardown.
    std::ios_base::Init ioInit;

    mutable std::mutex mutex;
    MetricsSnapshot aggregate;
    LatencyHistogram openLatency;
    LatencyHistogram requestLatency;
    LatencyHistogram readLatency;
    std::unordered_map<std::string, MetricsSnapshot> perAsset;
};

MetricsRegistry::MetricsRegistry()
    : _impl(new Impl()),
      _dumpAtExit(EnvironmentFlagSet("USD_HTTP_RESOLVER_METRICS_DUMP")) {}

// Never runs. Defined here rather than in the header because `Impl` is
// incomplete there, and left non-trivial-looking on purpose: see `Instance`.
MetricsRegistry::~MetricsRegistry() = default;

MetricsRegistry& MetricsRegistry::Instance() {
    // Constructed on first use and never destroyed.
    //
    // The leak is the point. A `ReaderMetrics` folds into this aggregate from
    // its destructor, and a reader can be destroyed arbitrarily late: the
    // resolver retains an open reader for an identifier that was resolved and
    // never opened (RESOLVER.md §2.3), and that reader is destroyed when the
    // resolver is, during static destruction. A function-local static aggregate
    // is destroyed in reverse order of construction -- which is to say *before*
    // the resolver that was constructed before it -- so the fold would reach a
    // destroyed object. That is a crash at exit, after every test has passed
    // and every byte has been served, which is the least debuggable moment a
    // process has.
    //
    // The opt-in dump therefore runs from `std::atexit` rather than from a
    // destructor. It is armed at first use, so it runs before the static
    // destructors registered earlier than that -- the same moment the old
    // destructor ran, and with the same consequence: counters folded after it,
    // by a reader that outlived it, are not in the dump. Missing a counter in
    // an opt-in diagnostic is a cost; reaching a destroyed aggregate is not.
    static MetricsRegistry* const instance = [] {
        MetricsRegistry* const created = new MetricsRegistry();
        if (created->DumpAtExitEnabled()) {
            std::atexit([] { MetricsRegistry::Instance().Dump(std::cerr); });
        }
        return created;
    }();
    return *instance;
}

void MetricsRegistry::Fold(const ReaderMetrics& reader) {
    const MetricsSnapshot snapshot = reader.Snapshot();
    const std::lock_guard<std::mutex> lock(_impl->mutex);

    _impl->aggregate.Add(snapshot);
    _impl->openLatency.Fold(reader.OpenLatency());
    _impl->requestLatency.Fold(reader.RequestLatency());
    _impl->readLatency.Fold(reader.ReadLatency());

    auto existing = _impl->perAsset.find(snapshot.identifier);
    if (existing != _impl->perAsset.end()) {
        // Two readers over one asset transferred bytes twice but did not make
        // the asset twice as large, and a per-asset selectivity computed from
        // a doubled size would understate exactly the number this table
        // exists to show.
        const std::uint64_t knownSize = existing->second.assetSize;
        existing->second.Add(snapshot);
        existing->second.assetSize = (std::max)(knownSize, snapshot.assetSize);
    } else if (_impl->perAsset.size() < kMaxTrackedIdentifiers) {
        _impl->perAsset.emplace(snapshot.identifier, snapshot);
    }
}

MetricsSnapshot MetricsRegistry::Aggregate() const {
    const std::lock_guard<std::mutex> lock(_impl->mutex);
    MetricsSnapshot snapshot = _impl->aggregate;
    snapshot.identifier.clear();
    // Quantiles come from the folded histograms, not from summed summaries.
    snapshot.openLatency = _impl->openLatency.Snapshot();
    snapshot.requestLatency = _impl->requestLatency.Snapshot();
    snapshot.readLatency = _impl->readLatency.Snapshot();
    return snapshot;
}

std::vector<MetricsSnapshot> MetricsRegistry::TopAssetsByBytesTransferred(
    std::size_t n) const {
    std::vector<MetricsSnapshot> assets;
    {
        const std::lock_guard<std::mutex> lock(_impl->mutex);
        assets.reserve(_impl->perAsset.size());
        for (const auto& entry : _impl->perAsset) {
            assets.push_back(entry.second);
        }
    }
    std::sort(assets.begin(), assets.end(),
              [](const MetricsSnapshot& lhs, const MetricsSnapshot& rhs) {
                  if (lhs.bytesTransferred != rhs.bytesTransferred) {
                      return lhs.bytesTransferred > rhs.bytesTransferred;
                  }
                  return lhs.identifier < rhs.identifier;
              });
    if (assets.size() > n) {
        assets.resize(n);
    }
    return assets;
}

void MetricsRegistry::Dump(std::ostream& out) const {
    const MetricsSnapshot aggregate = Aggregate();
    out << "usd-http-resolver metrics\n"
        << "  assetSize            " << aggregate.assetSize << "\n"
        << "  bytesRequested       " << aggregate.bytesRequested << "\n"
        << "  bytesTransferred     " << aggregate.bytesTransferred << "\n"
        << "  bytesFromCache       " << aggregate.bytesFromCache << "\n"
        << "  requestCount         " << aggregate.requestCount << "\n"
        << "  metadataRequestCount " << aggregate.metadataRequestCount << "\n"
        << "  retryCount           " << aggregate.retryCount << "\n"
        << "  redirectCount        " << aggregate.redirectCount << "\n"
        // The cache block. Printed unconditionally, including the zeroes a
        // release with no cache produces: a dump that hid them would make
        // "no cache ran" and "the cache never hit" the same output.
        << "  blockHits            " << aggregate.blockHits << "\n"
        << "  blockMisses          " << aggregate.blockMisses << "\n"
        << "  partialHits          " << aggregate.partialHits << "\n"
        << "  savedByCoalescing    " << aggregate.requestsSavedByCoalescing << "\n"
        << "  savedBySingleFlight  " << aggregate.requestsSavedBySingleFlight << "\n"
        << "  bytesOverFetched     " << aggregate.bytesOverFetched << "\n"
        << "  evictions            " << aggregate.evictions << "\n"
        << "  peakResidentBytes    " << aggregate.peakResidentBytes << "\n"
        << "  amplification        " << aggregate.Amplification() << "\n"
        << "  selectivity          " << aggregate.Selectivity() << "\n"
        << "  cacheHitRatio        " << aggregate.CacheHitRatio() << "\n"
        << "  overFetchRatio       " << aggregate.OverFetchRatio() << "\n"
        << "  openLatency us       p50 " << aggregate.openLatency.p50 << " p90 "
        << aggregate.openLatency.p90 << " p99 " << aggregate.openLatency.p99 << " max "
        << aggregate.openLatency.max << "\n"
        // Per transport request, which is the distribution an operator
        // investigating a slow stage is actually looking for: a caller-visible
        // read can hide several requests, and their tail is where the stall is.
        << "  requestLatency us    p50 " << aggregate.requestLatency.p50 << " p90 "
        << aggregate.requestLatency.p90 << " p99 " << aggregate.requestLatency.p99
        << " max " << aggregate.requestLatency.max << "\n"
        << "  readLatency us       p50 " << aggregate.readLatency.p50 << " p90 "
        << aggregate.readLatency.p90 << " p99 " << aggregate.readLatency.p99 << " max "
        << aggregate.readLatency.max << "\n";

    const std::vector<MetricsSnapshot> top = TopAssetsByBytesTransferred(10);
    if (!top.empty()) {
        out << "  top assets by bytesTransferred\n";
        for (const MetricsSnapshot& asset : top) {
            out << "    " << asset.bytesTransferred << "  " << asset.identifier << "\n";
        }
    }
    out.flush();
}

void MetricsRegistry::ResetForTesting() {
    const std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->aggregate = MetricsSnapshot();
    _impl->openLatency.Reset();
    _impl->requestLatency.Reset();
    _impl->readLatency.Reset();
    _impl->perAsset.clear();
}

}  // namespace usdasset
