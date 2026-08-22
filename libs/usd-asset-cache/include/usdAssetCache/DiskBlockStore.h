// SPDX-License-Identifier: Apache-2.0
//
// The second tier of the block cache: blocks that outlive the process that
// fetched them.
//
// This is CACHE.md §8, and the reason it is a separate class rather than a
// mode of `BlockCache` is the one §5 gives about locks. The in-memory store
// answers under a stripe mutex, and a file read under that mutex would make
// every unrelated block of the same stripe wait on a disk seek. So residency
// here is consulted by the reader, between the acquisition that made it the
// owner of a block and the transport fetch it would otherwise do -- which is
// also where single-flight already protects it: exactly one caller per block
// reaches this store, and everybody else waits on that caller.
//
// What the tier is allowed to do is narrower than what the memory tier is
// allowed to do, and the difference is one rule:
//
//   an entry is written only for a `Strong` validator
//
// Within one reader's lifetime the revision binding of ASSET_READER.md §2.1
// carries the guarantee whatever the validator's strength is, which is why the
// memory tier caches for a weak or absent one -- privately, and dropped when
// the reader closes. Across processes there is no binding left, and a weak
// match becomes a guess written to disk. This store is handed the binding's
// answer to that question rather than re-deriving it; see `Persistable`.
//
// Nothing here parses a validator, and nothing here builds a path out of one.
// A URL never becomes a path: every filename component is hexadecimal, derived
// from a SHA-256 digest of the key, and the same digest is written inside the
// entry and compared on the way out. A name collision therefore costs a miss
// and can never serve one asset's bytes for another's.
//
// The digest is also the whole of what is written down about the identity, and
// that is a release-gate requirement rather than a size optimization. A
// resolved identifier is a URL and a URL can be a signed one; gate 7 of
// docs/releases/README.md forbids a credential or a signed-URL query string in
// any persisted artifact, and a cache entry is the first artifact this project
// persists. Nothing under the cache directory is reversible to a URL.
//
// Normative contract: docs/architecture/CACHE.md §8.

#ifndef USDASSETCACHE_DISKBLOCKSTORE_H
#define USDASSETCACHE_DISKBLOCKSTORE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "usdAssetIo/Validator.h"
#include "usdAssetCache/CacheKey.h"

namespace usdasset {
namespace cache {

/// The default ceiling on what the cache directory may hold: 1 GiB.
///
/// Larger than the memory budget by an order of magnitude, because the resource
/// it bounds is cheaper by more than that, and bounded all the same: a cache
/// nobody bounded is a disk nobody can plan for.
inline constexpr std::uint64_t kDefaultPersistentBudgetBytes = 1024ull * 1024 * 1024;

/// The floor a budget is raised to. Below a few blocks the store would evict
/// what it just wrote, which costs two I/Os to achieve nothing.
inline constexpr std::uint64_t kMinPersistentBudgetBytes = 1024ull * 1024;

struct DiskCacheOptions {
    /// The directory the store owns. Empty disables persistence entirely, which
    /// is the default: a resolver that started writing to a disk nobody named
    /// would be a surprise, and CONFIGURATION.md makes it opt-in.
    std::string directory;

    /// The ceiling on bytes under `directory`. Enforced by a sweep rather than
    /// on every write; see `DiskBlockStore::Stats::sweeps`.
    std::uint64_t budgetBytes = kDefaultPersistentBudgetBytes;

    /// The same options with the budget raised to something a block fits in.
    /// Clamps rather than fails, for the reason `CacheOptions::Normalized`
    /// gives: the diagnostic belongs where the value was read.
    DiskCacheOptions Normalized() const;
};

/// Whether the entries of an asset with this validator may be written to disk.
///
/// The same predicate as `IsShareable`, named separately because the two
/// answers are the same rule applied at two lifetimes and a future relaxation
/// of one must not silently relax the other. CACHE.md §8's table is this
/// function: `Strong` yes, `Weak` no, `None` no.
bool Persistable(const Validator& validator) noexcept;

/// A content-addressed directory of blocks.
///
/// Thread-safe and process-safe. Two processes sharing one directory is the
/// case this exists for, and the only shared mutation is a rename over a name
/// both of them computed from the same key -- so the loser of a race publishes
/// bytes identical to the winner's, or loses the rename and drops its temporary
/// file. Neither outcome is an error, and neither is reported as one.
///
/// Every operation is best effort. A full disk, a read-only directory, a
/// cache somebody deleted halfway through a read: all of them are a miss and a
/// transport fetch, never a failed read. "Deleting the cache directory costs
/// time and never correctness" is the exit criterion, and it is a property of
/// this class refusing to have a failure mode above `false`.
class DiskBlockStore {
public:
    struct Stats {
        bool enabled = false;
        std::uint64_t hits = 0;         ///< Blocks served from disk.
        std::uint64_t misses = 0;       ///< Lookups that found no usable entry.
        std::uint64_t writes = 0;       ///< Blocks published.
        std::uint64_t writeFailures = 0;///< Publishes the filesystem refused.
        std::uint64_t rejected = 0;     ///< Writes declined by the strength rule.
        std::uint64_t discarded = 0;    ///< Corrupt entries deleted on the way out.
        std::uint64_t collisions = 0;   ///< Valid entries that named another key.
        std::uint64_t bytesRead = 0;
        std::uint64_t bytesWritten = 0;
        std::uint64_t sweeps = 0;
        std::uint64_t trimmed = 0;      ///< Entries deleted under the budget.
        std::uint64_t trimmedBytes = 0;
        std::uint64_t residentBytes = 0;///< As of the last sweep, plus writes since.
    };

    explicit DiskBlockStore(const DiskCacheOptions& options);
    ~DiskBlockStore();

    DiskBlockStore(const DiskBlockStore&) = delete;
    DiskBlockStore& operator=(const DiskBlockStore&) = delete;

    /// The process-wide store. Disabled until a host configures it, so the
    /// default behavior of every existing caller is exactly what it was.
    static DiskBlockStore& Process();

    /// Points the process store at `options`, discarding nothing: the contents
    /// of a cache directory are the point of it, so reconfiguring changes where
    /// entries are looked up and never deletes what is there.
    ///
    /// Returns false when the directory could not be prepared, in which case
    /// the store is left disabled rather than half configured.
    static bool ConfigureProcess(const DiskCacheOptions& options);

    bool IsEnabled() const noexcept;
    const DiskCacheOptions& Options() const noexcept;
    Stats Snapshot() const;

    /// Reads one block.
    ///
    /// Returns true and fills `out` only when a structurally intact entry
    /// carries the digest of exactly this identity and this block index, and
    /// holds `expectedLength` bytes. Every other outcome is false: absent,
    /// unreadable, truncated, checksum mismatched, or written under another
    /// identity whose digest happened to share a name. A structurally corrupt
    /// entry is deleted on the way out; one that belongs to another identity is
    /// left alone, because it is somebody's valid entry and this call is the
    /// one in the wrong place.
    bool Load(const AssetIdentity& identity,
              std::uint64_t blockIndex,
              std::uint64_t expectedLength,
              std::vector<unsigned char>* out);

    /// Writes one block, if `persistable`.
    ///
    /// The write goes to a temporary file in the destination's own directory
    /// and is renamed into place, so a process killed mid-write leaves a
    /// temporary nobody looks up rather than an entry half of which is real.
    /// Returns whether an entry was published; false is never an error to
    /// report to a caller, whose bytes are in hand either way.
    bool Store(const AssetIdentity& identity,
               std::uint64_t blockIndex,
               bool persistable,
               const unsigned char* bytes,
               std::size_t length);

    /// Deletes every entry under the directory, leaving the directory itself.
    /// For a test, and for a host that wants the "deleting the cache costs time
    /// only" property without shelling out.
    void Clear();

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace cache
}  // namespace usdasset

#endif  // USDASSETCACHE_DISKBLOCKSTORE_H
