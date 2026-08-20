// SPDX-License-Identifier: Apache-2.0
//
// The block store: residency, eviction under a budget, and single-flight.
//
// This is the half of the cache that holds bytes. The half that decides which
// bytes to ask for is CachedAssetReader; the two are separated because the
// arithmetic is worth testing without a reader and the residency is worth
// testing without a transport.
//
// The store is shared between readers on purpose. Eight Hydra threads opening
// one asset are eight readers over one revision, and a store that were private
// to each of them would issue every request eight times, move eight times the
// bytes, and report a metrics table that is off by a factor of eight
// (CACHE.md §5). Sharing is admitted by identity, and identity includes the
// validator, so it is never a URL match alone (CACHE.md §6, CacheKey.h).
//
// Locking is per block, striped: there is no lock in this file that a read of
// an unrelated block can be made to wait on, because a global lock over a
// network cache serializes the entire stage and §7 of the design policy
// forbids it.
//
// Normative contract: docs/architecture/CACHE.md §5, §6, §7.

#ifndef USDASSETCACHE_BLOCKCACHE_H
#define USDASSETCACHE_BLOCKCACHE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "usdAssetIo/Validator.h"
#include "usdAssetCache/CacheKey.h"
#include "usdAssetCache/CacheOptions.h"

namespace usdasset {
namespace cache {

/// One cached block. Immutable once published, which is what lets a reader copy
/// out of it after it has left the map: eviction drops the store's reference,
/// and the bytes live exactly as long as the last reader looking at them.
using BlockPtr = std::shared_ptr<const std::vector<unsigned char>>;

class BlockCache {
public:
    class Binding;

    struct Stats {
        std::uint64_t residentBytes = 0;
        std::uint64_t blockCount = 0;
        std::uint64_t pendingCount = 0;
        std::uint64_t evictions = 0;
        std::uint64_t peakResidentBytes = 0;
        std::uint64_t identityCount = 0;
        std::uint32_t shardCount = 0;
        std::uint64_t shardBudgetBytes = 0;
    };

    explicit BlockCache(const CacheOptions& options);
    ~BlockCache();

    BlockCache(const BlockCache&) = delete;
    BlockCache& operator=(const BlockCache&) = delete;

    /// The process-wide store. The budget is process-wide and shared across
    /// assets, per CACHE.md §7, so there is one of these and readers bind into
    /// it rather than each carrying their own.
    static BlockCache& Process();

    /// Rebuilds the process store with `options`.
    ///
    /// For a host that resolves the budget from its environment before opening
    /// anything (CONFIGURATION.md §2). Discards everything resident, so it is
    /// called once, at resolver construction, and never with readers open --
    /// a binding that outlived its store would be reading freed memory, so the
    /// call is refused while any binding is alive and reports that it was.
    static bool ConfigureProcess(const CacheOptions& options);

    const CacheOptions& Options() const noexcept;
    Stats Snapshot() const;

    /// Bytes resident right now, as one relaxed load.
    ///
    /// `Snapshot` locks every stripe, which is fine for a test and wrong for
    /// the read path: a counter that locks the whole store on every read is
    /// exactly the instrumentation METRICS.md section 4 forbids. This is the
    /// number a reader records its high-water mark from, and it is approximate
    /// under concurrent publishes -- which a high-water mark can afford to be.
    std::uint64_t ResidentBytes() const noexcept;

    /// Drops every block and every interned identity. Tests only: there is no
    /// production reason to throw away a cache that is correct by construction.
    void ClearForTesting();

    /// Binds a reader to an identity.
    ///
    /// `validator` is read for exactly two things and never parsed: its value
    /// becomes part of the key, and its strength decides whether the entries
    /// stored under it may be shared with another reader or are private to this
    /// one (CacheKey.h, `IsShareable`).
    std::shared_ptr<Binding> Bind(const std::string& resolvedIdentifier,
                                  const Validator& validator,
                                  std::uint64_t blockSize);

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

/// One reader's handle on the store.
///
/// Holds the interned identity, so a lookup costs two integers rather than two
/// string comparisons, and owns the private entries of a reader whose validator
/// is not strong enough to share: those are dropped when this handle dies.
class BlockCache::Binding {
public:
    ~Binding();

    Binding(const Binding&) = delete;
    Binding& operator=(const Binding&) = delete;

    enum class Acquisition {
        /// The block was resident. `block` holds it.
        Hit,
        /// It was absent, and this caller is now the one obliged to fetch it.
        /// Exactly one caller is ever told this for a given block, which is
        /// what single-flight *is*.
        Owned,
        /// It was absent and another caller is already fetching it. `Await`.
        Busy,
    };

    struct AcquireResult {
        Acquisition outcome = Acquisition::Owned;
        BlockPtr block;
    };

    AcquireResult Acquire(std::uint64_t blockIndex);

    /// Waits for the owner of a `Busy` block to publish it.
    ///
    /// Returns null when the owner failed or abandoned the block rather than
    /// publishing it. A null is not an error to report: the fetch that failed
    /// was somebody else's, against somebody else's transport, and reporting it
    /// here would fail one reader for another's network. The caller acquires
    /// again and does the work itself.
    BlockPtr Await(std::uint64_t blockIndex);

    /// Publishes bytes for a block this binding owns, and evicts under the
    /// budget to make room for it. Returns the number of blocks evicted.
    ///
    /// A publish for a block this binding no longer owns -- because the store
    /// was cleared underneath it -- stores nothing and returns zero. It is not
    /// an error: the bytes are still handed to the caller that fetched them.
    std::uint64_t Publish(std::uint64_t blockIndex,
                          const unsigned char* bytes,
                          std::size_t length);

    /// Gives up ownership without publishing, and wakes everyone waiting. Every
    /// `Owned` acquisition ends in exactly one `Publish` or one `Abandon`; a
    /// fetch that failed and did neither would leave every later reader of that
    /// block waiting on a fetch that is not happening.
    void Abandon(std::uint64_t blockIndex);

    const AssetIdentity& Identity() const noexcept;

    /// True when this binding's entries are private to it and are dropped when
    /// it closes -- which is what a weak or absent validator buys.
    bool IsPrivate() const noexcept;

private:
    friend class BlockCache;
    class Impl;
    explicit Binding(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> _impl;
};

}  // namespace cache
}  // namespace usdasset

#endif  // USDASSETCACHE_BLOCKCACHE_H
