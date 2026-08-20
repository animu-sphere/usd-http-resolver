// SPDX-License-Identifier: Apache-2.0

#include "usdAssetCache/BlockCache.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace usdasset {
namespace cache {

namespace {

/// The interned form of a key: two integers, because that is what a lookup on
/// the read path can afford. The string form is `CacheKey`, and the mapping
/// between them is the identity table below.
struct ShardKey {
    std::uint64_t identityId = 0;
    std::uint64_t blockIndex = 0;
};

bool operator==(const ShardKey& lhs, const ShardKey& rhs) noexcept {
    return lhs.identityId == rhs.identityId && lhs.blockIndex == rhs.blockIndex;
}

std::uint64_t Mix(std::uint64_t value) noexcept {
    // SplitMix64's finalizer. Used to choose a stripe, so that the blocks of
    // one asset land in different stripes and two threads reading two blocks of
    // one asset do not queue behind each other on one mutex.
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

struct ShardKeyHash {
    std::size_t operator()(const ShardKey& key) const noexcept {
        return static_cast<std::size_t>(Mix(key.identityId ^ Mix(key.blockIndex)));
    }
};

struct IdentityHash {
    std::size_t operator()(const AssetIdentity& identity) const noexcept {
        return HashAssetIdentity(identity);
    }
};

struct Entry {
    BlockPtr block;  ///< Null exactly while `pending`.
    bool pending = false;
    std::uint64_t bytes = 0;
    std::list<ShardKey>::iterator lruIt{};
    bool inLru = false;  ///< A pending entry is not evictable.
};

/// One stripe of the store. Everything that needs a lock is in here, and there
/// is no lock outside it on the read path -- which is the whole of what "no
/// global lock" means in section 7 of the design policy.
struct Shard {
    mutable std::mutex mutex;
    std::condition_variable published;
    std::unordered_map<ShardKey, Entry, ShardKeyHash> entries;
    std::list<ShardKey> lru;  ///< Front is most recently used.
    std::uint64_t residentBytes = 0;
    std::uint64_t evictions = 0;
    std::uint64_t peakResidentBytes = 0;
};

std::uint32_t ChooseShardCount(std::uint64_t budgetBytes, std::uint64_t blockSize) noexcept {
    // Enough stripes that unrelated reads rarely collide, but never so many
    // that a stripe's share of the budget cannot hold a working set. Below
    // eight blocks per stripe the eviction order stops resembling LRU and
    // starts resembling a coin toss, so the count falls back toward one -- and
    // at one stripe the eviction order is exact, which is what a test that
    // fills a small budget wants.
    const std::uint64_t desired = blockSize > 0 ? budgetBytes / (blockSize * 8) : 1;
    std::uint32_t count = 1;
    while (count < 64 && static_cast<std::uint64_t>(count) * 2 <= desired) {
        count *= 2;
    }
    return count;
}

}  // namespace

// --- BlockCache::Impl --------------------------------------------------------

class BlockCache::Impl {
public:
    explicit Impl(const CacheOptions& requested) { Reset(requested); }

    void Reset(const CacheOptions& requested) {
        options = requested.Normalized();
        const std::uint32_t count = ChooseShardCount(options.budgetBytes, options.blockSize);
        shards.clear();
        shards.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            shards.emplace_back(new Shard());
        }
        shardMask = count - 1;
        shardBudget = (std::max)(options.budgetBytes / count, options.blockSize);
        residentTotal.store(0, std::memory_order_relaxed);
        const std::lock_guard<std::mutex> lock(identityMutex);
        identities.clear();
    }

    Shard& ShardFor(const ShardKey& key) noexcept {
        return *shards[ShardKeyHash()(key) & shardMask];
    }

    CacheOptions options;
    std::vector<std::unique_ptr<Shard>> shards;
    std::size_t shardMask = 0;
    std::uint64_t shardBudget = 0;

    /// The stripes' resident totals, summed, maintained relaxed so that the
    /// read path can read it without taking a lock.
    std::atomic<std::uint64_t> residentTotal{0};

    mutable std::mutex identityMutex;
    std::unordered_map<AssetIdentity, std::uint64_t, IdentityHash> identities;
    std::uint64_t nextIdentityId = 1;
    std::uint64_t liveBindings = 0;
};

namespace {

/// Drops least-recently-used blocks until the stripe is inside its budget.
///
/// Called with the stripe locked. Eviction is invisible to correctness: an
/// evicted block is re-fetched, and it is never served stale and never served
/// zero-filled (CACHE.md section 7). The last resident block is never evicted,
/// so a budget smaller than one block degrades to a one-block cache rather than
/// to a cache that stores every block and immediately drops it again.
std::uint64_t EvictLocked(Shard& shard, std::uint64_t budget, std::atomic<std::uint64_t>& total) {
    std::uint64_t evicted = 0;
    while (shard.residentBytes > budget && shard.lru.size() > 1) {
        const ShardKey victim = shard.lru.back();
        shard.lru.pop_back();
        auto it = shard.entries.find(victim);
        if (it != shard.entries.end()) {
            shard.residentBytes -= it->second.bytes;
            total.fetch_sub(it->second.bytes, std::memory_order_relaxed);
            shard.entries.erase(it);
        }
        ++shard.evictions;
        ++evicted;
    }
    return evicted;
}

void TouchLocked(Shard& shard, Entry& entry) {
    if (!entry.inLru) {
        return;
    }
    shard.lru.splice(shard.lru.begin(), shard.lru, entry.lruIt);
    entry.lruIt = shard.lru.begin();
}

}  // namespace

// --- BlockCache::Binding::Impl ----------------------------------------------

class BlockCache::Binding::Impl {
public:
    BlockCache::Impl* store = nullptr;
    AssetIdentity identity;
    std::uint64_t identityId = 0;
    bool isPrivate = false;
};

// --- BlockCache --------------------------------------------------------------

BlockCache::BlockCache(const CacheOptions& options) : _impl(new Impl(options)) {}

BlockCache::~BlockCache() = default;

BlockCache& BlockCache::Process() {
    // Constructed on first use with the shipped defaults. A host that wants
    // other numbers calls ConfigureProcess before it opens anything.
    static BlockCache instance{CacheOptions()};
    return instance;
}

bool BlockCache::ConfigureProcess(const CacheOptions& options) {
    BlockCache& store = Process();
    {
        const std::lock_guard<std::mutex> lock(store._impl->identityMutex);
        if (store._impl->liveBindings != 0) {
            // Refused rather than applied. Reconfiguring rebuilds the stripes,
            // and a binding that held a stripe index across that rebuild would
            // be reading a container that no longer exists.
            return false;
        }
    }
    store._impl->Reset(options);
    return true;
}

const CacheOptions& BlockCache::Options() const noexcept { return _impl->options; }

std::uint64_t BlockCache::ResidentBytes() const noexcept {
    return _impl->residentTotal.load(std::memory_order_relaxed);
}

BlockCache::Stats BlockCache::Snapshot() const {
    Stats stats;
    stats.shardCount = static_cast<std::uint32_t>(_impl->shards.size());
    stats.shardBudgetBytes = _impl->shardBudget;
    for (const auto& shard : _impl->shards) {
        const std::lock_guard<std::mutex> lock(shard->mutex);
        stats.residentBytes += shard->residentBytes;
        stats.evictions += shard->evictions;
        // Summed rather than maximized: each stripe holds a disjoint set of
        // blocks, so the sum of the stripe peaks is the closest thing to a
        // store-wide high-water mark that costs no lock to maintain. It is an
        // upper bound, and it is reported as one.
        stats.peakResidentBytes += shard->peakResidentBytes;
        for (const auto& entry : shard->entries) {
            if (entry.second.pending) {
                ++stats.pendingCount;
            } else {
                ++stats.blockCount;
            }
        }
    }
    const std::lock_guard<std::mutex> lock(_impl->identityMutex);
    stats.identityCount = _impl->identities.size();
    return stats;
}

void BlockCache::ClearForTesting() {
    for (const auto& shard : _impl->shards) {
        const std::lock_guard<std::mutex> lock(shard->mutex);
        shard->entries.clear();
        shard->lru.clear();
        shard->residentBytes = 0;
        shard->evictions = 0;
        shard->peakResidentBytes = 0;
        shard->published.notify_all();
    }
    _impl->residentTotal.store(0, std::memory_order_relaxed);
    const std::lock_guard<std::mutex> lock(_impl->identityMutex);
    _impl->identities.clear();
}

std::shared_ptr<BlockCache::Binding> BlockCache::Bind(const std::string& resolvedIdentifier,
                                                      const Validator& validator,
                                                      std::uint64_t blockSize) {
    std::unique_ptr<Binding::Impl> impl(new Binding::Impl());
    impl->store = _impl.get();
    impl->identity.resolvedIdentifier = resolvedIdentifier;
    impl->identity.validator = validator.value;
    impl->identity.blockSize = blockSize;
    impl->isPrivate = !IsShareable(validator);

    {
        const std::lock_guard<std::mutex> lock(_impl->identityMutex);
        ++_impl->liveBindings;
        if (impl->isPrivate) {
            // A private identity is never interned, so nothing can look it up
            // and no second reader can collide with it -- which is exactly the
            // property a weak or absent validator buys (CacheKey.h).
            impl->identityId = _impl->nextIdentityId++;
        } else {
            auto found = _impl->identities.find(impl->identity);
            if (found != _impl->identities.end()) {
                impl->identityId = found->second;
            } else {
                impl->identityId = _impl->nextIdentityId++;
                _impl->identities.emplace(impl->identity, impl->identityId);
            }
        }
    }

    return std::shared_ptr<Binding>(new Binding(std::move(impl)));
}

// --- BlockCache::Binding -----------------------------------------------------

BlockCache::Binding::Binding(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

BlockCache::Binding::~Binding() {
    BlockCache::Impl& store = *_impl->store;
    if (_impl->isPrivate) {
        // Private entries die with the reader that made them. Nothing else can
        // reach them, so leaving them resident would spend the budget on bytes
        // that are unreachable by construction.
        const std::uint64_t id = _impl->identityId;
        for (const auto& shard : store.shards) {
            const std::lock_guard<std::mutex> lock(shard->mutex);
            for (auto it = shard->entries.begin(); it != shard->entries.end();) {
                if (it->first.identityId != id) {
                    ++it;
                    continue;
                }
                if (it->second.inLru) {
                    shard->lru.erase(it->second.lruIt);
                    shard->residentBytes -= it->second.bytes;
                    store.residentTotal.fetch_sub(it->second.bytes,
                                                  std::memory_order_relaxed);
                }
                it = shard->entries.erase(it);
            }
            shard->published.notify_all();
        }
    }
    const std::lock_guard<std::mutex> lock(store.identityMutex);
    --store.liveBindings;
}

const AssetIdentity& BlockCache::Binding::Identity() const noexcept {
    return _impl->identity;
}

bool BlockCache::Binding::IsPrivate() const noexcept { return _impl->isPrivate; }

BlockCache::Binding::AcquireResult BlockCache::Binding::Acquire(std::uint64_t blockIndex) {
    const ShardKey key{_impl->identityId, blockIndex};
    Shard& shard = _impl->store->ShardFor(key);

    AcquireResult result;
    const std::lock_guard<std::mutex> lock(shard.mutex);
    auto found = shard.entries.find(key);
    if (found == shard.entries.end()) {
        Entry entry;
        entry.pending = true;
        shard.entries.emplace(key, entry);
        result.outcome = Acquisition::Owned;
        return result;
    }
    if (found->second.pending) {
        result.outcome = Acquisition::Busy;
        return result;
    }
    TouchLocked(shard, found->second);
    result.outcome = Acquisition::Hit;
    result.block = found->second.block;
    return result;
}

BlockPtr BlockCache::Binding::Await(std::uint64_t blockIndex) {
    const ShardKey key{_impl->identityId, blockIndex};
    Shard& shard = _impl->store->ShardFor(key);

    std::unique_lock<std::mutex> lock(shard.mutex);
    for (;;) {
        auto found = shard.entries.find(key);
        if (found == shard.entries.end()) {
            // The owner abandoned it, or the store was cleared. Not an error
            // here; the caller acquires again and fetches it itself.
            return BlockPtr();
        }
        if (!found->second.pending) {
            TouchLocked(shard, found->second);
            return found->second.block;
        }
        shard.published.wait(lock);
    }
}

std::uint64_t BlockCache::Binding::Publish(std::uint64_t blockIndex,
                                           const unsigned char* bytes,
                                           std::size_t length) {
    // Built outside the stripe lock. Copying a block while holding the mutex
    // that every other block in the stripe waits on is the one allocation on
    // this path that would be worth complaining about.
    BlockPtr block(new std::vector<unsigned char>(bytes, bytes + length));

    const ShardKey key{_impl->identityId, blockIndex};
    Shard& shard = _impl->store->ShardFor(key);

    std::uint64_t evicted = 0;
    {
        const std::lock_guard<std::mutex> lock(shard.mutex);
        auto found = shard.entries.find(key);
        if (found == shard.entries.end() || !found->second.pending) {
            // Ownership was taken away underneath us -- ClearForTesting, or a
            // private binding closing. The bytes still go to the caller that
            // fetched them; only the store forgets them.
            shard.published.notify_all();
            return 0;
        }
        Entry& entry = found->second;
        entry.block = block;
        entry.pending = false;
        entry.bytes = length;
        shard.lru.push_front(key);
        entry.lruIt = shard.lru.begin();
        entry.inLru = true;
        shard.residentBytes += length;
        _impl->store->residentTotal.fetch_add(length, std::memory_order_relaxed);
        shard.peakResidentBytes = (std::max)(shard.peakResidentBytes, shard.residentBytes);
        evicted = EvictLocked(shard, _impl->store->shardBudget, _impl->store->residentTotal);
        shard.published.notify_all();
    }
    return evicted;
}

void BlockCache::Binding::Abandon(std::uint64_t blockIndex) {
    const ShardKey key{_impl->identityId, blockIndex};
    Shard& shard = _impl->store->ShardFor(key);

    const std::lock_guard<std::mutex> lock(shard.mutex);
    auto found = shard.entries.find(key);
    if (found != shard.entries.end() && found->second.pending) {
        shard.entries.erase(found);
    }
    shard.published.notify_all();
}

}  // namespace cache
}  // namespace usdasset
