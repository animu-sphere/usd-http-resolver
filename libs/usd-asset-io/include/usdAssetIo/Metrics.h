// SPDX-License-Identifier: Apache-2.0
//
// The I/O counters that make this project's central claim checkable.
//
// The entire argument for this architecture is a ratio -- 10 GB of asset, 30 MB
// transferred. Without a counter that sentence is a claim; with one it is a
// test assertion. These also catch the failure this design is most exposed to,
// accidental amplification, which every functional test passes straight
// through.
//
// Normative contract: docs/architecture/METRICS.md.

#ifndef USDASSETIO_METRICS_H
#define USDASSETIO_METRICS_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace usdasset {

/// A fixed-bucket latency histogram.
///
/// Latency is recorded as a distribution and never as a mean: a mean latency
/// over a network hides exactly the tail that makes an interactive session feel
/// broken. Buckets are powers of two of microseconds, so quantiles are
/// bucket-resolution estimates rather than exact order statistics -- which is
/// the price of not allocating and not storing samples on the read path.
class LatencyHistogram {
public:
    /// Bucket i holds [2^(i-1), 2^i) microseconds; bucket 0 holds 0. The top
    /// bucket saturates, at roughly 2^40 us, which is a fortnight.
    static constexpr std::size_t kBucketCount = 41;

    LatencyHistogram() noexcept;

    LatencyHistogram(const LatencyHistogram&) = delete;
    LatencyHistogram& operator=(const LatencyHistogram&) = delete;

    void Record(std::uint64_t microseconds) noexcept;

    struct Summary {
        std::uint64_t count = 0;
        std::uint64_t sum = 0;  ///< Microseconds.
        std::uint64_t max = 0;
        std::uint64_t p50 = 0;  ///< Bucket upper bounds, not exact samples.
        std::uint64_t p90 = 0;
        std::uint64_t p99 = 0;
    };

    Summary Snapshot() const noexcept;

    /// Folds another histogram's buckets into this one, for process-wide
    /// aggregation.
    void Fold(const LatencyHistogram& other) noexcept;

    /// Discards every sample. For tests only.
    void Reset() noexcept;

private:
    std::atomic<std::uint64_t> _buckets[kBucketCount];
    std::atomic<std::uint64_t> _count;
    std::atomic<std::uint64_t> _sum;
    std::atomic<std::uint64_t> _max;
};

/// The plain-value form of a reader's counters: what a test asserts on and what
/// a dump prints. Reading it is a snapshot, never a lock.
struct MetricsSnapshot {
    std::string identifier;  ///< Already passed through `ElideSecrets`.

    // Per asset (METRICS.md §2.1).
    std::uint64_t assetSize = 0;
    std::uint64_t bytesRequested = 0;
    std::uint64_t bytesTransferred = 0;
    std::uint64_t bytesFromCache = 0;
    std::uint64_t requestCount = 0;
    std::uint64_t metadataRequestCount = 0;
    std::uint64_t retryCount = 0;
    std::uint64_t redirectCount = 0;

    // Cache (METRICS.md §2.2). Defined here in v0.1.0 and populated by
    // libs/usd-asset-cache in v0.3.0; a backend leaves them at zero.
    std::uint64_t blockHits = 0;
    std::uint64_t blockMisses = 0;
    std::uint64_t partialHits = 0;
    std::uint64_t requestsSavedByCoalescing = 0;
    std::uint64_t requestsSavedBySingleFlight = 0;
    std::uint64_t bytesOverFetched = 0;
    std::uint64_t evictions = 0;
    std::uint64_t peakResidentBytes = 0;

    // Latency (METRICS.md §2.3).
    LatencyHistogram::Summary openLatency;
    LatencyHistogram::Summary requestLatency;
    LatencyHistogram::Summary readLatency;

    // Derived ratios (METRICS.md §2.4). Each returns 0.0 when its denominator
    // is zero, because a ratio nobody measured is not an infinity.
    double Amplification() const noexcept;
    double OverFetchRatio() const noexcept;
    double CacheHitRatio() const noexcept;
    double RequestEfficiency() const noexcept;
    /// The headline number: the fraction of a remote asset that was actually
    /// moved to answer a query.
    double Selectivity() const noexcept;

    /// Takes the transport-side counters of an inner reader in a decorated
    /// stack, leaving this snapshot's own caller-side counters alone.
    ///
    /// A decorated stack has one counter set, and it is the outermost reader's,
    /// because the two ends of the stack disagree about what `bytesRequested`
    /// means: to the cache it is what the caller asked for, and to the reader
    /// underneath it is what the cache asked for expanded to whole blocks.
    /// Summing them would make `amplification` a ratio over a denominator that
    /// is two different measurements added together. So the outer set keeps
    /// what the caller asked for and the cache served, and takes from the inner
    /// set only what crossed the transport.
    void AbsorbTransport(const MetricsSnapshot& inner);

    /// Sums counters. Latency merges only `count`, `sum`, and `max`: quantiles
    /// cannot be recovered from two summaries, so they are zeroed rather than
    /// averaged into a number that looks like a measurement. The aggregate in
    /// `MetricsRegistry` folds the histograms themselves and does carry real
    /// quantiles.
    void Add(const MetricsSnapshot& other);
};

/// One reader's counters.
///
/// Counters are relaxed atomics on a per-reader structure, never a shared
/// global: instrumentation that measurably slows the fast path defeats itself.
/// Nothing here allocates on the read path.
///
/// Counters are monotonic within a reader and are never retracted by a failure.
/// A conditional request the server refused still cost a round trip, and a
/// reader whose counters shrink after a failure is reporting what it wishes had
/// happened.
class ReaderMetrics {
public:
    /// `identifier` is stored elided; a dump never prints a query string.
    explicit ReaderMetrics(std::string identifier);

    /// Folds this reader's counters into the process aggregate, so that a
    /// stage-wide total survives assets being opened and closed during
    /// composition.
    ~ReaderMetrics();

    ReaderMetrics(const ReaderMetrics&) = delete;
    ReaderMetrics& operator=(const ReaderMetrics&) = delete;

    void SetAssetSize(std::uint64_t bytes) noexcept;
    void AddBytesRequested(std::uint64_t bytes) noexcept;
    void AddBytesTransferred(std::uint64_t bytes) noexcept;
    void AddBytesFromCache(std::uint64_t bytes) noexcept;
    void AddRequest(std::uint64_t count = 1) noexcept;
    void AddMetadataRequest(std::uint64_t count = 1) noexcept;
    void AddRetry(std::uint64_t count = 1) noexcept;
    void AddRedirect(std::uint64_t count = 1) noexcept;

    // The cache counters of METRICS.md §2.2. Declared here rather than on a
    // cache-owned structure because a counter set that is per reader for the
    // transport and per decorator for the cache cannot be summed: the process
    // aggregate and the top-assets table both fold one `ReaderMetrics`, and a
    // cache hit that landed somewhere else would be invisible in exactly the
    // report it is evidence for.
    //
    // A backend never calls these; `libs/usd-asset-cache` does, on the reader
    // metrics of the reader it decorates.
    void AddBlockHit(std::uint64_t count = 1) noexcept;
    void AddBlockMiss(std::uint64_t count = 1) noexcept;
    void AddPartialHit(std::uint64_t count = 1) noexcept;
    void AddRequestsSavedByCoalescing(std::uint64_t count = 1) noexcept;
    void AddRequestsSavedBySingleFlight(std::uint64_t count = 1) noexcept;
    void AddBytesOverFetched(std::uint64_t bytes) noexcept;
    void AddEviction(std::uint64_t count = 1) noexcept;

    /// Raises the resident high-water mark to `bytes` if it is higher.
    ///
    /// A high-water mark is a maximum and not a sum, which is why it is
    /// observed rather than added: two readers sharing one block store did not
    /// make the store twice as large, and `MetricsSnapshot::Add` takes the
    /// maximum of this field for the same reason.
    void ObserveResidentBytes(std::uint64_t bytes) noexcept;

    LatencyHistogram& OpenLatency() noexcept { return _openLatency; }
    LatencyHistogram& RequestLatency() noexcept { return _requestLatency; }
    LatencyHistogram& ReadLatency() noexcept { return _readLatency; }

    const LatencyHistogram& OpenLatency() const noexcept { return _openLatency; }
    const LatencyHistogram& RequestLatency() const noexcept { return _requestLatency; }
    const LatencyHistogram& ReadLatency() const noexcept { return _readLatency; }

    const std::string& Identifier() const noexcept { return _identifier; }

    /// Absorbs an inner reader's transport counters into this set, histograms
    /// included, so that the decorator that owns this set folds the whole
    /// stack into the process aggregate exactly once.
    ///
    /// The inner set is detached first; see `DetachFromRegistry`.
    void AbsorbTransport(const ReaderMetrics& inner) noexcept;

    /// Stops this counter set folding into the process aggregate when it is
    /// destroyed.
    ///
    /// For the inner reader of a decorated stack, whose counters are folded by
    /// the decorator above it. A set that both is absorbed and folds itself
    /// would count every transferred byte twice.
    void DetachFromRegistry() noexcept;
    bool IsDetached() const noexcept;

    /// Readable while the reader lives.
    MetricsSnapshot Snapshot() const;

private:
    std::string _identifier;

    std::atomic<std::uint64_t> _assetSize{0};
    std::atomic<std::uint64_t> _bytesRequested{0};
    std::atomic<std::uint64_t> _bytesTransferred{0};
    std::atomic<std::uint64_t> _bytesFromCache{0};
    std::atomic<std::uint64_t> _requestCount{0};
    std::atomic<std::uint64_t> _metadataRequestCount{0};
    std::atomic<std::uint64_t> _retryCount{0};
    std::atomic<std::uint64_t> _redirectCount{0};

    std::atomic<std::uint64_t> _blockHits{0};
    std::atomic<std::uint64_t> _blockMisses{0};
    std::atomic<std::uint64_t> _partialHits{0};
    std::atomic<std::uint64_t> _requestsSavedByCoalescing{0};
    std::atomic<std::uint64_t> _requestsSavedBySingleFlight{0};
    std::atomic<std::uint64_t> _bytesOverFetched{0};
    std::atomic<std::uint64_t> _evictions{0};
    std::atomic<std::uint64_t> _peakResidentBytes{0};

    std::atomic<bool> _detached{false};

    LatencyHistogram _openLatency;
    LatencyHistogram _requestLatency;
    LatencyHistogram _readLatency;
};

/// Times a scope into a histogram. Used for open, per transport request, and
/// per caller-visible read.
class ScopedLatency {
public:
    explicit ScopedLatency(LatencyHistogram& histogram) noexcept;
    ~ScopedLatency();

    ScopedLatency(const ScopedLatency&) = delete;
    ScopedLatency& operator=(const ScopedLatency&) = delete;

private:
    LatencyHistogram& _histogram;
    std::uint64_t _startNanoseconds;
};

/// The process-wide aggregate that closed readers fold into.
class MetricsRegistry {
public:
    static MetricsRegistry& Instance();

    /// Called by `~ReaderMetrics`. Takes the reader rather than a snapshot so
    /// that the latency histograms are folded bucket by bucket, which is the
    /// only way the aggregate's quantiles mean anything.
    void Fold(const ReaderMetrics& reader);

    /// The sum over every reader that has closed, plus nothing that is still
    /// open. `identifier` is empty on the aggregate.
    MetricsSnapshot Aggregate() const;

    /// The `n` closed assets with the largest `bytesTransferred`, which is what
    /// a human investigating a slow stage actually wants. Rows carry counters
    /// only; see `MetricsSnapshot::Add`.
    ///
    /// The per-identifier table is bounded: past `kMaxTrackedIdentifiers`
    /// distinct assets, later readers fold into the aggregate and are not
    /// listed. A long-lived host opening a hundred thousand assets must not
    /// pay a hundred thousand strings for a diagnostic nobody asked for.
    static constexpr std::size_t kMaxTrackedIdentifiers = 4096;
    std::vector<MetricsSnapshot> TopAssetsByBytesTransferred(std::size_t n) const;

    /// Writes the aggregate and the top assets. Identifiers are already
    /// elided. Never called automatically on the read path: a resolver that
    /// logs per request becomes the slow thing.
    void Dump(std::ostream& out) const;

    /// Discards everything folded so far. For tests only; there is no
    /// production reason to reset a monotonic aggregate.
    void ResetForTesting();

    /// True when `USD_HTTP_RESOLVER_METRICS_DUMP` was set in the environment at
    /// first use, in which case the aggregate is dumped to stderr at exit.
    bool DumpAtExitEnabled() const noexcept { return _dumpAtExit; }

private:
    MetricsRegistry();
    ~MetricsRegistry();

    class Impl;
    std::unique_ptr<Impl> _impl;
    bool _dumpAtExit = false;
};

}  // namespace usdasset

#endif  // USDASSETIO_METRICS_H
