// SPDX-License-Identifier: Apache-2.0

#include "usdAssetCache/CachedAssetReader.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "usdAssetIo/RangeMath.h"
#include "BlockPlan.h"

namespace usdasset {
namespace cache {

namespace {

using detail::BlockExtent;
using detail::BlockSpan;
using detail::ExtentOf;
using detail::FetchRun;
using detail::OverFetchedBytes;
using detail::PlanRuns;

/// How many times a read will go round the acquire / fetch / wait loop before
/// it stops cooperating and fetches what is left itself.
///
/// A bound rather than a spin. A block this reader waited for can come back
/// unpublished -- the owner's fetch failed, or its binding closed -- and the
/// honest response is to acquire it again and do the work. Twice is enough to
/// absorb that; a third time means something is churning, and serving the read
/// directly is better than joining the churn.
constexpr int kMaxCooperativePasses = 3;

/// Copies the part of one block that the caller's range covers.
///
/// Returns the number of bytes copied, which is zero for a block that turned
/// out not to overlap -- a case the arithmetic above should make impossible,
/// and which is handled rather than asserted because the cost of being wrong
/// here is a buffer overrun.
std::size_t CopyOverlap(unsigned char* dst,
                        std::uint64_t rangeOffset,
                        std::uint64_t rangeLength,
                        std::uint64_t blockOffset,
                        const unsigned char* blockBytes,
                        std::uint64_t blockLength) {
    const std::uint64_t rangeEnd = rangeOffset + rangeLength;
    const std::uint64_t blockEnd = blockOffset + blockLength;
    const std::uint64_t begin = (std::max)(rangeOffset, blockOffset);
    const std::uint64_t end = (std::min)(rangeEnd, blockEnd);
    if (end <= begin) {
        return 0;
    }
    const std::size_t length = static_cast<std::size_t>(end - begin);
    std::memcpy(dst + (begin - rangeOffset), blockBytes + (begin - blockOffset), length);
    return length;
}

/// The blocks one read owns and has not published yet.
///
/// `BlockCache::Binding::Abandon` documents the invariant this exists to keep:
/// every `Owned` acquisition ends in exactly one `Publish` or one `Abandon`,
/// because a block left pending is one that every later reader of it waits on
/// for a fetch that is not happening -- and `Await` has no deadline, so "waits"
/// means forever.
///
/// Hand-written release paths kept that invariant across every `return` and
/// across no `throw`. Both of the allocations on this path can throw: the
/// transfer buffer is sized by the run and may be `maxRequestBytes`, and
/// `Publish` copies the block. A `bad_alloc` out of either used to leave the
/// remaining blocks pending for the life of the process. So ownership is held
/// by a destructor now, which runs either way.
class OwnedBlocks {
public:
    explicit OwnedBlocks(BlockCache::Binding& binding) noexcept : _binding(&binding) {}
    ~OwnedBlocks() { AbandonAll(); }

    OwnedBlocks(const OwnedBlocks&) = delete;
    OwnedBlocks& operator=(const OwnedBlocks&) = delete;

    void Add(std::uint64_t blockIndex) { _blocks.push_back(blockIndex); }

    std::size_t Size() const noexcept { return _blocks.size(); }
    bool Empty() const noexcept { return _blocks.empty(); }
    const std::vector<std::uint64_t>& Blocks() const noexcept { return _blocks; }

    /// The next block still owned, in fetch order.
    std::uint64_t Next() const noexcept { return _blocks[_settled]; }
    bool AllSettled() const noexcept { return _settled >= _blocks.size(); }

    /// Records that `Publish` took the next block. `Publish` allocates before it
    /// touches the store, so a throw from it leaves the block still owned and
    /// the destructor still correct.
    void MarkPublished() noexcept { ++_settled; }

    /// Hands back every block still owned. Idempotent, so the destructor
    /// running after an explicit call costs nothing.
    void AbandonAll() noexcept {
        while (_settled < _blocks.size()) {
            _binding->Abandon(_blocks[_settled++]);
        }
        _blocks.clear();
        _settled = 0;
    }

private:
    BlockCache::Binding* _binding;
    std::vector<std::uint64_t> _blocks;
    std::size_t _settled = 0;
};

}  // namespace

// --- Impl --------------------------------------------------------------------

class CachedAssetReader::Impl {
public:
    Impl(std::unique_ptr<AssetReader> reader,
         ReaderMetrics* readerMetrics,
         const CacheOptions& requested,
         BlockCache& blockStore)
        : inner(std::move(reader)),
          innerMetrics(readerMetrics),
          options(requested.Normalized()),
          store(blockStore),
          metrics(inner->Metadata().resolvedIdentifier) {
        metrics.SetAssetSize(inner->Metadata().size);
        if (innerMetrics != nullptr) {
            // One counter set per stack. The inner reader's own fold would
            // otherwise report the cache's expanded asks as a second reader's
            // caller-side demand.
            innerMetrics->DetachFromRegistry();
        }
        binding = store.Bind(inner->Metadata().resolvedIdentifier,
                             inner->Metadata().validator, options.blockSize);
    }

    ~Impl() {
        // While the inner reader is still alive, and before `metrics` is
        // destroyed and folds. Both halves of that sentence are why this is a
        // destructor body and not a member initializer order comment.
        if (innerMetrics != nullptr) {
            metrics.AbsorbTransport(*innerMetrics);
        }
    }

    ReadResult ReadCached(const ReadRange& range, unsigned char* dst);

    /// Reads part of one block straight from the transport, without claiming it
    /// in the store. The fallback for a block this reader could neither own nor
    /// wait for; it asks for exactly the bytes the caller wants, so it cannot
    /// over-fetch and has nothing to publish.
    Status ReadDirect(std::uint64_t offset, unsigned char* dst, std::size_t length);

    std::unique_ptr<AssetReader> inner;
    ReaderMetrics* innerMetrics = nullptr;
    CacheOptions options;
    BlockCache& store;
    ReaderMetrics metrics;
    std::shared_ptr<BlockCache::Binding> binding;
};

Status CachedAssetReader::Impl::ReadDirect(std::uint64_t offset,
                                           unsigned char* dst,
                                           std::size_t length) {
    const ReadResult result = inner->Read(offset, dst, length);
    if (!result.status.IsOk()) {
        return result.status;
    }
    if (result.bytesRead != length) {
        // Below EOF by construction: the extent was clamped to the asset size
        // captured at open. A transfer that stopped short of it is the short
        // read the contract forbids turning into a hole.
        return ShortReadStatus(offset, length, result.bytesRead,
                               inner->Metadata().resolvedIdentifier);
    }
    return Status::Ok();
}

ReadResult CachedAssetReader::Impl::ReadCached(const ReadRange& range, unsigned char* dst) {
    const std::uint64_t assetSize = inner->Metadata().size;
    const std::uint64_t blockSize = options.blockSize;
    const BlockSpan span = detail::CoveringBlocks(range.offset, range.length, blockSize);
    const std::size_t blockCount = static_cast<std::size_t>(span.Count());

    std::vector<bool> resolved(blockCount, false);
    std::vector<unsigned char> transfer;

    /// Blocks this read got without fetching them: resident on arrival, or
    /// published by whoever owned them while this read waited. Both are bytes
    /// that came out of the store, which is what classifies the read below.
    std::uint64_t cacheServed = 0;
    std::uint64_t served = 0;

    for (int pass = 0; pass < kMaxCooperativePasses; ++pass) {
        OwnedBlocks owned(*binding);
        std::vector<std::size_t> busy;

        for (std::size_t i = 0; i < blockCount; ++i) {
            if (resolved[i]) {
                continue;
            }
            const std::uint64_t blockIndex = span.first + i;
            const BlockExtent extent = ExtentOf(blockIndex, blockSize, assetSize);
            BlockCache::Binding::AcquireResult acquired = binding->Acquire(blockIndex);

            if (acquired.outcome == BlockCache::Binding::Acquisition::Hit) {
                if (acquired.block && acquired.block->size() == extent.length) {
                    const std::size_t copied =
                        CopyOverlap(dst, range.offset, range.length, extent.offset,
                                    acquired.block->data(), extent.length);
                    metrics.AddBytesFromCache(copied);
                    served += copied;
                    ++cacheServed;
                    resolved[i] = true;
                    continue;
                }
                // A resident block whose length disagrees with the asset size
                // this reader was opened against. Nothing can make that block
                // correct for this reader, so it is not used and not trusted --
                // the bytes are fetched instead. The ownerships this pass has
                // already taken are handed back first, because the direct read
                // below can fail and return, and a block left pending is a
                // block every later reader waits on for a fetch that is not
                // happening. The next pass acquires them again.
                owned.AbandonAll();

                const std::uint64_t begin = (std::max)(range.offset, extent.offset);
                const std::uint64_t end =
                    (std::min)(range.offset + range.length, extent.offset + extent.length);
                const Status direct =
                    ReadDirect(begin, dst + (begin - range.offset),
                               static_cast<std::size_t>(end - begin));
                if (!direct.IsOk()) {
                    return ReadResult{0, direct};
                }
                metrics.AddBlockMiss();
                served += (end - begin);
                resolved[i] = true;
                continue;
            }
            if (acquired.outcome == BlockCache::Binding::Acquisition::Owned) {
                owned.Add(blockIndex);
            } else {
                busy.push_back(i);
            }
        }

        if (owned.Empty() && busy.empty()) {
            break;
        }

        // --- fetch what this reader owns ------------------------------------
        if (!owned.Empty()) {
            const std::vector<FetchRun> runs = PlanRuns(owned.Blocks(), blockSize, assetSize,
                                                        options.coalesceGapBlocks,
                                                        options.maxRequestBytes);
            // Every owned block would have been its own request without the
            // merge. What the merge saved is the difference, and it is counted
            // here rather than inferred from a request total that a hit also
            // moves.
            metrics.AddRequestsSavedByCoalescing(owned.Size() - runs.size());

            for (const FetchRun& run : runs) {
                transfer.assign(static_cast<std::size_t>(run.length), 0);
                const ReadResult fetched =
                    inner->Read(run.offset, transfer.data(),
                                static_cast<std::size_t>(run.length));

                if (!fetched.status.IsOk() || fetched.bytesRead != run.length) {
                    // Give every block this read still owns back to the store
                    // before returning. A block left pending is a block every
                    // later reader waits on for a fetch that is not happening.
                    owned.AbandonAll();
                    if (!fetched.status.IsOk()) {
                        return ReadResult{0, fetched.status};
                    }
                    return ReadResult{0, ShortReadStatus(
                                             run.offset,
                                             static_cast<std::size_t>(run.length),
                                             fetched.bytesRead,
                                             inner->Metadata().resolvedIdentifier)};
                }

                const std::uint64_t runLast = run.firstBlock + run.blockCount - 1;
                std::uint64_t takenFromRun = 0;
                while (!owned.AllSettled() && owned.Next() <= runLast) {
                    const std::uint64_t blockIndex = owned.Next();
                    const BlockExtent extent = ExtentOf(blockIndex, blockSize, assetSize);
                    const unsigned char* bytes =
                        transfer.data() + (extent.offset - run.offset);

                    metrics.AddEviction(binding->Publish(
                        blockIndex, bytes, static_cast<std::size_t>(extent.length)));
                    owned.MarkPublished();
                    metrics.AddBlockMiss();

                    const std::size_t copied =
                        CopyOverlap(dst, range.offset, range.length, extent.offset,
                                    bytes, extent.length);
                    takenFromRun += copied;
                    served += copied;
                    resolved[static_cast<std::size_t>(blockIndex - span.first)] = true;
                }

                // Charged against what the caller took *out of this transfer*,
                // not against the caller's byte range. The two differ exactly
                // where coalescing merged across a block that was already
                // resident: those bytes sit inside the range, so a range-based
                // charge called them wanted, and the wire moved them anyway
                // while the caller read that block from the store. They are the
                // cost of the merge, which is the thing this counter is for.
                metrics.AddBytesOverFetched(OverFetchedBytes(run, takenFromRun));
            }
        }

        // --- wait for what somebody else owns -------------------------------
        for (const std::size_t i : busy) {
            const std::uint64_t blockIndex = span.first + i;
            const BlockExtent extent = ExtentOf(blockIndex, blockSize, assetSize);
            const BlockPtr block = binding->Await(blockIndex);
            if (!block || block->size() != extent.length) {
                // The owner failed or abandoned it. Not this reader's failure;
                // the next pass acquires it again and fetches it.
                continue;
            }
            const std::size_t copied = CopyOverlap(dst, range.offset, range.length,
                                                   extent.offset, block->data(),
                                                   extent.length);
            // Served without this reader issuing a request, which is what
            // single-flight buys and what the counter is for.
            metrics.AddBytesFromCache(copied);
            metrics.AddRequestsSavedBySingleFlight();
            // And counted as a block this read did not fetch, which it had not
            // been. `blockMisses` counts blocks *this* reader pulled over the
            // transport and this is not one; the classification below then saw
            // neither a hit nor a miss, so a read served entirely by
            // single-flight landed in none of `blockHits`, `blockMisses`, or
            // `partialHits` -- invisible in the three counters METRICS.md §2.2
            // defines the cache by.
            ++cacheServed;
            served += copied;
            resolved[i] = true;
        }
    }

    // --- anything the loop could not settle cooperatively --------------------
    for (std::size_t i = 0; i < blockCount; ++i) {
        if (resolved[i]) {
            continue;
        }
        const BlockExtent extent = ExtentOf(span.first + i, blockSize, assetSize);
        const std::uint64_t begin = (std::max)(range.offset, extent.offset);
        const std::uint64_t end =
            (std::min)(range.offset + range.length, extent.offset + extent.length);
        const Status direct = ReadDirect(begin, dst + (begin - range.offset),
                                         static_cast<std::size_t>(end - begin));
        if (!direct.IsOk()) {
            return ReadResult{0, direct};
        }
        metrics.AddBlockMiss();
        served += (end - begin);
        resolved[i] = true;
    }

    if (cacheServed == static_cast<std::uint64_t>(blockCount)) {
        metrics.AddBlockHit();
    } else if (cacheServed > 0) {
        metrics.AddPartialHit();
    }
    // One relaxed load, not a snapshot: `Snapshot` locks every stripe, and a
    // counter that locks the whole store on every read is the instrumentation
    // METRICS.md section 4 forbids.
    metrics.ObserveResidentBytes(store.ResidentBytes());

    if (served != range.length) {
        // Unreachable by the arithmetic above, and checked anyway: the failure
        // it would otherwise be is a buffer with a hole in it reported as a
        // complete read, which is the one outcome this project treats as worse
        // than an error.
        return ReadResult{0, ShortReadStatus(range.offset,
                                             static_cast<std::size_t>(range.length),
                                             static_cast<std::size_t>(served),
                                             inner->Metadata().resolvedIdentifier)};
    }
    return ReadResult{static_cast<std::size_t>(range.length), Status::Ok()};
}

// --- CachedAssetReader -------------------------------------------------------

CachedAssetReader::CachedAssetReader(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

CachedAssetReader::~CachedAssetReader() = default;

const AssetMetadata& CachedAssetReader::Metadata() const { return _impl->inner->Metadata(); }

const CacheOptions& CachedAssetReader::Options() const noexcept { return _impl->options; }

const ReaderMetrics& CachedAssetReader::Metrics() const noexcept { return _impl->metrics; }

const BlockCache::Binding& CachedAssetReader::Binding() const noexcept {
    return *_impl->binding;
}

MetricsSnapshot CachedAssetReader::SnapshotMetrics() const {
    MetricsSnapshot snapshot = _impl->metrics.Snapshot();
    if (_impl->innerMetrics != nullptr) {
        snapshot.AbsorbTransport(_impl->innerMetrics->Snapshot());
    }
    return snapshot;
}

ReadResult CachedAssetReader::Read(std::uint64_t offset, void* dst, std::size_t size) {
    ScopedLatency readTimer(_impl->metrics.ReadLatency());
    _impl->metrics.AddBytesRequested(size);

    const ReadRange range = ResolveReadRange(offset, size, _impl->inner->Metadata().size);
    if (range.outcome == ReadRangeOutcome::Overflow) {
        return ReadResult{0, OverflowStatus(offset, size)};
    }
    // Before the empty check, for the reason the local backend gives: a null
    // buffer with a non-zero length is a caller bug wherever the offset lands.
    if (size > 0 && dst == nullptr) {
        return ReadResult{0, Status::Error(StatusCode::InvalidArgument,
                                           "read destination buffer is null")
                                 .WithRange(offset, size)};
    }
    if (range.outcome == ReadRangeOutcome::Empty) {
        return ReadResult{0, Status::Ok()};
    }

    if (range.length >= _impl->options.bypassThresholdBytes) {
        // A streaming pass. Served straight through and stored nowhere: keeping
        // it would evict the working set to hold bytes that will not be read
        // twice, and the read after it would pay for the privilege.
        return _impl->inner->Read(offset, dst, size);
    }

    return _impl->ReadCached(range, static_cast<unsigned char*>(dst));
}

// --- Wrap --------------------------------------------------------------------

struct CachedReaderFactory {
    /// Builds the whole reader rather than taking an `Impl` as a parameter:
    /// the private nested type stays inside the one scope that is a friend,
    /// which is also how the backends' factories are shaped.
    static std::unique_ptr<CachedAssetReader> Make(std::unique_ptr<AssetReader> inner,
                                                   ReaderMetrics* innerMetrics,
                                                   const CacheOptions& options,
                                                   BlockCache& store) {
        std::unique_ptr<CachedAssetReader::Impl> impl(new CachedAssetReader::Impl(
            std::move(inner), innerMetrics, options, store));
        return std::unique_ptr<CachedAssetReader>(new CachedAssetReader(std::move(impl)));
    }
};

CachedOpenResult Wrap(std::unique_ptr<AssetReader> inner,
                      ReaderMetrics* innerMetrics,
                      const CacheOptions& options,
                      BlockCache* store) {
    CachedOpenResult result;
    if (!inner) {
        result.status = Status::Error(StatusCode::InvalidArgument,
                                      "the cache was given no reader to decorate");
        return result;
    }

    BlockCache& blockStore = store != nullptr ? *store : BlockCache::Process();
    result.reader =
        CachedReaderFactory::Make(std::move(inner), innerMetrics, options, blockStore);
    return result;
}

OpenResult WrapAsset(OpenResult inner,
                     ReaderMetrics* innerMetrics,
                     const CacheOptions& options,
                     BlockCache* store) {
    OpenResult result;
    if (!inner.reader) {
        // Nothing to decorate. The backend's status is the useful thing the
        // result carries, and replacing it with one of this module's would
        // erase it.
        result.status = std::move(inner.status);
        return result;
    }
    if (!inner.reader->Metadata().supportsRandomAccess) {
        // A reader that cannot seek would store the one block it managed to
        // read and miss forever after. Passed through undecorated rather than
        // failed: the reader works, and a cache is an optimization.
        result.reader = std::move(inner.reader);
        result.status = std::move(inner.status);
        return result;
    }

    CachedOpenResult wrapped =
        Wrap(std::move(inner.reader), innerMetrics, options, store);
    result.reader = std::move(wrapped.reader);
    result.status = std::move(wrapped.status);
    return result;
}

}  // namespace cache
}  // namespace usdasset
