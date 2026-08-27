// SPDX-License-Identifier: Apache-2.0

#include "usdAssetCache/DiskBlockStore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <system_error>
#include <utility>

#include "usdAssetCache/CacheOptions.h"
#include "Sha256.h"

namespace usdasset {
namespace cache {

namespace {

namespace fs = std::filesystem;

/// The directory under the configured root that holds entries.
///
/// Versioned in the name rather than only in the header, so that a future
/// format change is a directory nobody reads instead of a corpus of entries
/// every process has to open before rejecting. The old one is then deletable by
/// exactly the same rule as the whole cache: it costs time only.
constexpr const char* kEntryRoot = "blocks-v1";

constexpr const char* kEntrySuffix = ".blk";
constexpr const char* kTempSuffix = ".tmp";

/// The fixed part of an entry. Everything variable-length follows it, and every
/// length that describes the variable part is inside this header and covered by
/// `headerHash`, so a truncated header can never be used to size an allocation.
///
///   0   8   magic
///   8   4   format version
///   12  4   reserved, zero
///   16  8   block size
///   24  8   block index
///   32  8   payload length
///   40  32  identity digest (SHA-256 over the key image)
///   72  8   payload hash    (FNV-1a over the payload)
///   80  8   header hash     (FNV-1a over bytes 0..80)
///   88      payload
///
/// The identity is a *digest* and never the identity itself, and that is a
/// release-gate requirement rather than a size optimization. A resolved
/// identifier is a URL, and a URL can be a signed one: gate 7 of
/// docs/releases/README.md forbids a credential, a token, or a signed-URL query
/// string in any persisted artifact, and an entry that wrote
/// `?X-Amz-Signature=...` into a file would put one on a disk that outlives the
/// process, in the one place `ElideSecrets` never runs.
///
/// A digest keeps the property the identity was there for. Two entries belong
/// to the same key exactly when their digests agree, so a name collision is
/// still caught and still costs a miss rather than serving one asset's bytes
/// for another's; it is only the *inspectability* of an entry that is lost, and
/// what an entry is for is not being read by a person.
constexpr std::size_t kHeaderBytes = 88;
constexpr std::size_t kDigestBytes = detail::Sha256::kDigestBytes;
constexpr std::uint32_t kFormatVersion = 1;
const unsigned char kMagic[8] = {'U', 'S', 'D', 'B', 'L', 'K', '\0', '1'};

/// A ceiling on what a header may claim, so that a corrupt length is refused
/// before it becomes an allocation. Nothing this store holds is larger than one
/// block, and a block is bounded by `kMaxBlockSize`.
constexpr std::uint64_t kMaxEntryPayload = kMaxBlockSize;

/// How many of the largest entry it has written a sweep's budget must admit
/// before the sweep is allowed to enforce it.
///
/// `kMinPersistentBudgetBytes` is a floor in bytes, and a block size is a
/// runtime choice, so the two can be set against each other: a legal 8 MiB
/// block under the minimum budget would make every sweep trim away the entry
/// whose write triggered it -- two I/Os spent to achieve nothing, which is the
/// outcome that floor was introduced to prevent and cannot prevent on its own,
/// because `DiskCacheOptions` never learns a block size. Raising the byte floor
/// to a few of the largest legal block instead would force a quarter of a
/// gigabyte on a host that asked for a megabyte, so the relation is stated
/// here, where the size of an entry is a fact rather than a bound.
constexpr std::uint64_t kMinBudgetEntries = 4;

/// FNV-1a, 64 bit.
///
/// A checksum and nothing more: it catches a truncated or scribbled entry. It
/// does not name a file and it does not decide whether an entry belongs to a
/// key -- both of those are the SHA-256 digest below, because both of those are
/// questions an adversary would like to answer wrongly.
std::uint64_t Fnv1a(const void* data, std::size_t length, std::uint64_t seed) noexcept {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    std::uint64_t hash = seed;
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 0x00000100000001B3ull;
    }
    return hash;
}

constexpr std::uint64_t kFnvOffsetBasis = 0xCBF29CE484222325ull;

void AppendLittleEndian64(std::string& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

void AppendLittleEndian32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

std::uint64_t ReadLittleEndian64(const unsigned char* bytes) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
}

std::uint32_t ReadLittleEndian32(const unsigned char* bytes) noexcept {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
    }
    return value;
}

/// The bytes a key hashes over. Every field of `CacheKey`, with the lengths
/// mixed in so that moving a byte from the identifier to the validator is a
/// different key -- which it is.
std::string KeyImage(const AssetIdentity& identity, std::uint64_t blockIndex) {
    std::string image;
    image.reserve(identity.resolvedIdentifier.size() + identity.validator.size() + 32);
    AppendLittleEndian64(image, identity.resolvedIdentifier.size());
    image.append(identity.resolvedIdentifier);
    AppendLittleEndian64(image, identity.validator.size());
    image.append(identity.validator);
    AppendLittleEndian64(image, identity.blockSize);
    AppendLittleEndian64(image, blockIndex);
    return image;
}

/// The identity of one block, as 32 bytes. This is what is written down, and it
/// is the only thing about the key that is.
struct IdentityDigest {
    unsigned char bytes[kDigestBytes] = {};
};

IdentityDigest DigestOf(const AssetIdentity& identity, std::uint64_t blockIndex) {
    const std::string image = KeyImage(identity, blockIndex);
    detail::Sha256 sha;
    sha.Update(image.data(), image.size());
    IdentityDigest digest;
    sha.Finish(digest.bytes);
    return digest;
}

/// The entry's file name stem: 32 hexadecimal characters and nothing else.
///
/// This is the whole of "no filename component is attacker-controllable". The
/// identifier can be any URL a stage names, including one whose path is `..`
/// repeated and one carrying a signed query string, and none of it reaches the
/// filesystem: it is digested, and the digest is rendered from a
/// sixteen-character alphabet.
///
/// Half the digest, because a file name is a bucket rather than a proof. The
/// other half is in the entry, and the whole of it is compared before a byte is
/// used.
std::string NameFor(const IdentityDigest& digest) {
    static const char kHex[] = "0123456789abcdef";
    std::string name(32, '0');
    for (int i = 0; i < 16; ++i) {
        name[static_cast<std::size_t>(2 * i)] = kHex[(digest.bytes[i] >> 4) & 0xF];
        name[static_cast<std::size_t>(2 * i + 1)] = kHex[digest.bytes[i] & 0xF];
    }
    return name;
}

/// A value unique to this process, for temporary file names.
///
/// Not the pid: reaching for one means a `<windows.h>` in a module whose whole
/// point is that it knows nothing about a platform. A random 64-bit value drawn
/// once collides with another live process at a rate nothing in this system is
/// sensitive to, and a collision costs one lost temporary file.
std::uint64_t ProcessNonce() {
    static const std::uint64_t nonce = [] {
        std::random_device device;
        std::uint64_t value = static_cast<std::uint64_t>(device()) << 32;
        value |= static_cast<std::uint64_t>(device());
        value ^= static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return value;
    }();
    return nonce;
}

std::string HexOf(std::uint64_t value) {
    static const char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 0; i < 16; ++i) {
        out[static_cast<std::size_t>(i)] = kHex[(value >> (60 - 4 * i)) & 0xF];
    }
    return out;
}

/// The serialized entry, header and body in one buffer.
///
/// Built whole and written whole. A header written first and a body written
/// second is two chances to be interrupted, and the rename this is published by
/// only makes the *appearance* of the file atomic -- not its assembly.
std::string EncodeEntry(const AssetIdentity& identity,
                        const IdentityDigest& digest,
                        std::uint64_t blockIndex,
                        const unsigned char* bytes,
                        std::size_t length) {
    std::string entry;
    entry.reserve(kHeaderBytes + length);
    entry.append(reinterpret_cast<const char*>(kMagic), sizeof(kMagic));
    AppendLittleEndian32(entry, kFormatVersion);
    AppendLittleEndian32(entry, 0);
    AppendLittleEndian64(entry, identity.blockSize);
    AppendLittleEndian64(entry, blockIndex);
    AppendLittleEndian64(entry, static_cast<std::uint64_t>(length));
    entry.append(reinterpret_cast<const char*>(digest.bytes), kDigestBytes);
    AppendLittleEndian64(entry, Fnv1a(bytes, length, kFnvOffsetBasis));
    AppendLittleEndian64(entry, Fnv1a(entry.data(), entry.size(), kFnvOffsetBasis));
    entry.append(reinterpret_cast<const char*>(bytes), length);
    return entry;
}

/// Why a load did not produce bytes.
///
/// The distinction that matters is between the first two: a structurally broken
/// entry is this store's own litter and is deleted, and an entry belonging to
/// another identity is somebody's valid data and is left where it is. The third
/// is separate only so that the collision counter counts collisions -- an entry
/// whose digest agrees and whose length does not is not another key's, it is
/// this key's under an asset size this reader was not opened against.
enum class DecodeOutcome {
    Ok,
    Corrupt,     ///< Truncated, mis-hashed, or not one of ours.
    OtherKey,    ///< Intact, and carries another identity's digest.
    WrongShape,  ///< This identity, and not the block this reader asked for.
};

DecodeOutcome DecodeEntry(const std::string& raw,
                          const AssetIdentity& identity,
                          const IdentityDigest& digest,
                          std::uint64_t blockIndex,
                          std::uint64_t expectedLength,
                          std::vector<unsigned char>* out) {
    if (raw.size() < kHeaderBytes) {
        return DecodeOutcome::Corrupt;
    }
    const unsigned char* header = reinterpret_cast<const unsigned char*>(raw.data());

    if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
        return DecodeOutcome::Corrupt;
    }
    if (Fnv1a(header, kHeaderBytes - 8, kFnvOffsetBasis) !=
        ReadLittleEndian64(header + 80)) {
        return DecodeOutcome::Corrupt;
    }
    if (ReadLittleEndian32(header + 8) != kFormatVersion) {
        return DecodeOutcome::Corrupt;
    }

    const std::uint64_t payloadLength = ReadLittleEndian64(header + 32);
    if (payloadLength > kMaxEntryPayload) {
        return DecodeOutcome::Corrupt;
    }
    if (raw.size() != kHeaderBytes + payloadLength) {
        // A short file is a write that was interrupted before the rename could
        // hide it, or a file somebody truncated. A long one is not ours.
        return DecodeOutcome::Corrupt;
    }
    const unsigned char* payload = header + kHeaderBytes;
    if (Fnv1a(payload, static_cast<std::size_t>(payloadLength), kFnvOffsetBasis) !=
        ReadLittleEndian64(header + 72)) {
        return DecodeOutcome::Corrupt;
    }

    // Structurally sound from here. Everything below is "is this ours", and the
    // answer being no leaves the entry where it is.
    if (std::memcmp(header + 40, digest.bytes, kDigestBytes) != 0) {
        // The name matched and the digest does not, which is exactly the
        // collision the in-entry digest exists to catch.
        return DecodeOutcome::OtherKey;
    }
    if (ReadLittleEndian64(header + 16) != identity.blockSize ||
        ReadLittleEndian64(header + 24) != blockIndex) {
        // Unreachable while the digest covers both, and checked anyway: the
        // digest is the proof and these are two of the fields it is a proof
        // *about*, so a disagreement means one of the two is wrong.
        return DecodeOutcome::WrongShape;
    }
    if (payloadLength != expectedLength) {
        // The one field the digest does *not* cover, because it is not part of
        // the key: a block's length is a function of the asset's size, and the
        // size is not in the key. The reader asking is opened against a size,
        // and a block stored at another one cannot be made correct for it.
        return DecodeOutcome::WrongShape;
    }

    out->assign(payload, payload + static_cast<std::size_t>(payloadLength));
    return DecodeOutcome::Ok;
}

/// Reads a whole file. Returns false for anything that is not a complete read
/// of a regular file, without distinguishing why: every reason is a miss.
bool ReadWholeFile(const fs::path& path, std::string* out) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error || size > kHeaderBytes + kMaxEntryPayload) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    out->resize(static_cast<std::size_t>(size));
    if (size > 0) {
        input.read(&(*out)[0], static_cast<std::streamsize>(size));
        if (static_cast<std::uintmax_t>(input.gcount()) != size) {
            return false;
        }
    }
    return true;
}

}  // namespace

// --- options -----------------------------------------------------------------

DiskCacheOptions DiskCacheOptions::Normalized() const {
    DiskCacheOptions normalized = *this;
    normalized.budgetBytes = (std::max)(normalized.budgetBytes, kMinPersistentBudgetBytes);
    return normalized;
}

bool Persistable(const Validator& validator) noexcept {
    // Deliberately spelled out rather than delegated to `IsShareable`, and
    // stricter than it. The two are the same question at two lifetimes, and the
    // longer lifetime asks for one thing more.
    //
    // `Strong` is necessary. It is not sufficient, because strength is a claim
    // and a claim has an author. An entity tag was issued by the origin against
    // the bytes it served, and it means the same thing to whoever reads the
    // entry next -- in another process, after a restart, tomorrow. A `Derived`
    // identity was synthesized by a backend out of what it could see while it
    // held the asset open, and `usdAssetLocal` is the case that shows what that
    // costs: it renders device, file index, size, and mtime, and declares the
    // result `Strong` because it holds the handle and re-derives the identity
    // on every read -- so equal values mean equal bytes *for as long as the
    // reader lives*, which is what its own comment says. On a filesystem whose
    // timestamp resolution is coarse enough to hide a same-size rewrite in
    // place, that claim is already at its limit; writing it to disk would move
    // the limit from "until this reader closes" to "until the entry is
    // evicted", and a stale read a restart cannot clear is exactly what an
    // on-disk cache was not admitted until validators landed in order to avoid.
    return validator.strength == ValidatorStrength::Strong &&
           validator.kind == ValidatorKind::EntityTag;
}

// --- Impl --------------------------------------------------------------------

class DiskBlockStore::Impl {
public:
    explicit Impl(const DiskCacheOptions& requested) { Configure(requested); }

    /// Prepares the directory and adopts the options, or leaves the store
    /// disabled. Returns whether persistence is on afterwards.
    bool Configure(const DiskCacheOptions& requested) {
        const std::lock_guard<std::mutex> lock(mutex);
        options = requested.Normalized();
        enabled = false;
        root.clear();
        swept = false;
        knownBytes = 0;
        bytesSinceSweep = 0;
        largestEntryBytes = 0;
        // Before every early return below, so that a sweep already walking the
        // old directory cannot publish its count into the new one.
        ++epoch;

        if (options.directory.empty()) {
            return false;
        }

        std::error_code error;
        fs::path candidate = fs::path(options.directory) / kEntryRoot;
        fs::create_directories(candidate, error);
        if (error || !fs::is_directory(candidate, error)) {
            return false;
        }
        // Best effort, and only on a platform that has the notion. Applied to
        // the directory this store created and not to the one the host named:
        // a host may well have pointed this at a directory it shares for other
        // reasons, and tightening somebody else's directory as a side effect of
        // enabling a cache is not this module's decision to make.
        std::error_code permissionError;
        fs::permissions(candidate, fs::perms::owner_all, fs::perm_options::replace,
                        permissionError);

        root = std::move(candidate);
        enabled = true;
        return true;
    }

    fs::path PathFor(const std::string& name) const {
        return root / name.substr(0, 2) / (name + kEntrySuffix);
    }

    /// What a sweep needs out of the lock, and the stamp that says whether its
    /// answer still describes the directory when it comes back.
    struct SweepPlan {
        fs::path root;
        std::uint64_t budget = 0;
        std::uint64_t epoch = 0;
    };

    /// The budget a sweep actually enforces. Called with `mutex` held.
    ///
    /// The host's number, unless that number cannot hold `kMinBudgetEntries` of
    /// the largest entry this store has written -- in which case enforcing it
    /// would delete the entry whose write triggered the sweep.
    std::uint64_t EffectiveBudgetLocked() const {
        return (std::max)(options.budgetBytes, largestEntryBytes * kMinBudgetEntries);
    }

    /// How many bytes may be published between sweeps, so that the ceiling is
    /// the budget plus one interval's writes. Called with `mutex` held.
    std::uint64_t SweepIntervalLocked() const {
        return (std::max)(EffectiveBudgetLocked() / 8, kMinPersistentBudgetBytes);
    }

    /// Claims the right to sweep and records that one happened. Called with
    /// `mutex` held.
    ///
    /// Returns false when another thread is already walking the directory:
    /// there is nothing for this caller to do, and a second concurrent walk
    /// would race the first one over the same deletes.
    bool BeginSweepLocked(SweepPlan* plan) {
        if (sweeping || !enabled) {
            return false;
        }
        sweeping = true;
        swept = true;
        bytesSinceSweep = 0;
        ++stats.sweeps;
        plan->root = root;
        plan->budget = EffectiveBudgetLocked();
        plan->epoch = epoch;
        return true;
    }

    /// Walks the directory, deletes stale temporaries, and trims to the budget.
    ///
    /// Called with `mutex` NOT held, and that is the whole reason a sweep is
    /// three functions rather than one. It is a recursive directory walk, a
    /// sort, and a run of deletes; under the lock it would make every concurrent
    /// `Load` wait behind it -- the exact serialization `Load` goes out of its
    /// way to avoid by reading its file outside the lock. `sweeping` is what
    /// keeps two of these from running at once, so the walk is unlocked without
    /// being unguarded.
    void RunSweep(const SweepPlan& plan) {
        struct Candidate {
            fs::path path;
            std::uintmax_t size = 0;
            fs::file_time_type written{};
        };
        std::vector<Candidate> entries;
        std::uint64_t total = 0;
        std::uint64_t trimmed = 0;
        std::uint64_t trimmedBytes = 0;

        std::error_code error;
        fs::recursive_directory_iterator it(plan.root,
                                            fs::directory_options::skip_permission_denied, error);
        // A directory that would not open leaves `total` at zero, which is the
        // right account of a cache somebody deleted: nothing is resident, and
        // the next write recreates what it needs.
        if (!error) {
            const fs::recursive_directory_iterator end;
            // The error_code increment: a directory another process is deleting
            // entries out of will make an iteration step fail, and a throw out
            // of a sweep would turn "the cache is being tidied" into a failed
            // read.
            for (; it != end; it.increment(error)) {
                if (error) {
                    break;
                }
                std::error_code entryError;
                if (!it->is_regular_file(entryError) || entryError) {
                    continue;
                }
                const std::uintmax_t size = it->file_size(entryError);
                if (entryError) {
                    continue;
                }
                const fs::file_time_type written = it->last_write_time(entryError);
                if (entryError) {
                    continue;
                }

                const std::string filename = it->path().filename().string();
                const bool isTemporary =
                    filename.size() > 4 &&
                    filename.compare(filename.size() - 4, 4, kTempSuffix) == 0;
                if (isTemporary) {
                    // A temporary this old belongs to a process that is not
                    // going to finish it. An hour is far longer than writing one
                    // block takes and far shorter than a cache directory's life.
                    const auto age = fs::file_time_type::clock::now() - written;
                    if (age > std::chrono::hours(1)) {
                        std::error_code removeError;
                        fs::remove(it->path(), removeError);
                        continue;
                    }
                }

                total += size;
                entries.push_back(Candidate{it->path(), size, written});
            }
        }

        if (total > plan.budget) {
            // Oldest first, by write time. Not a true LRU: refreshing a
            // timestamp on every hit would turn a read of a cached block into a
            // write, which is the trade this tier exists to avoid. Eviction is
            // invisible to correctness here for exactly the reason CACHE.md §7
            // gives in memory -- an evicted block is re-fetched -- so an
            // approximate order costs a re-fetch and nothing else.
            std::sort(entries.begin(), entries.end(),
                      [](const Candidate& lhs, const Candidate& rhs) {
                          return lhs.written < rhs.written;
                      });
            const std::uint64_t target = plan.budget - plan.budget / 8;
            for (const Candidate& candidate : entries) {
                if (total <= target) {
                    break;
                }
                std::error_code removeError;
                if (fs::remove(candidate.path, removeError) && !removeError) {
                    total -= candidate.size;
                    ++trimmed;
                    trimmedBytes += candidate.size;
                }
                // A remove that failed is a file another process has open. It
                // stays, and the next sweep tries again.
            }
        }

        const std::lock_guard<std::mutex> lock(mutex);
        sweeping = false;
        stats.trimmed += trimmed;
        stats.trimmedBytes += trimmedBytes;
        if (epoch == plan.epoch) {
            // Only when nothing reconfigured or emptied the store while the walk
            // was running: a count of a directory that is no longer the one
            // being written to is worse than no count at all. An entry published
            // during the walk is counted twice for one interval -- once here and
            // once in `bytesSinceSweep` -- which errs toward sweeping sooner,
            // the harmless direction.
            knownBytes = total;
        }
    }

    mutable std::mutex mutex;
    DiskCacheOptions options;
    fs::path root;
    bool enabled = false;
    bool swept = false;
    std::uint64_t knownBytes = 0;
    std::uint64_t bytesSinceSweep = 0;
    /// The largest entry this store has published, which is what relates a
    /// budget to a block size nobody told it; see `kMinBudgetEntries`.
    std::uint64_t largestEntryBytes = 0;
    /// Bumped whenever the directory this store owns changes identity or is
    /// emptied, so a sweep in flight can tell that its count is stale.
    std::uint64_t epoch = 0;
    bool sweeping = false;
    std::atomic<std::uint64_t> tempCounter{0};
    Stats stats;
};

// --- DiskBlockStore ----------------------------------------------------------

DiskBlockStore::DiskBlockStore(const DiskCacheOptions& options) : _impl(new Impl(options)) {}

DiskBlockStore::~DiskBlockStore() = default;

DiskBlockStore& DiskBlockStore::Process() {
    // Disabled on construction. Persistence is opt-in, so a host that never
    // names a directory behaves exactly as it did before this tier existed.
    static DiskBlockStore instance{DiskCacheOptions()};
    return instance;
}

bool DiskBlockStore::ConfigureProcess(const DiskCacheOptions& options) {
    // No liveness check, unlike `BlockCache::ConfigureProcess`. Nothing holds a
    // pointer into this store across a call: a reader keeps the store itself,
    // and every operation takes the lock and re-reads the root.
    return Process()._impl->Configure(options);
}

bool DiskBlockStore::IsEnabled() const noexcept {
    const std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->enabled;
}

const DiskCacheOptions& DiskBlockStore::Options() const noexcept { return _impl->options; }

DiskBlockStore::Stats DiskBlockStore::Snapshot() const {
    const std::lock_guard<std::mutex> lock(_impl->mutex);
    Stats stats = _impl->stats;
    stats.enabled = _impl->enabled;
    stats.residentBytes = _impl->knownBytes + _impl->bytesSinceSweep;
    return stats;
}

bool DiskBlockStore::Load(const AssetIdentity& identity,
                          std::uint64_t blockIndex,
                          std::uint64_t expectedLength,
                          std::vector<unsigned char>* out) {
    if (out == nullptr || expectedLength == 0 || expectedLength > kMaxEntryPayload) {
        return false;
    }

    const IdentityDigest digest = DigestOf(identity, blockIndex);
    fs::path path;
    {
        const std::lock_guard<std::mutex> lock(_impl->mutex);
        if (!_impl->enabled) {
            return false;
        }
        path = _impl->PathFor(NameFor(digest));
    }

    // The file read happens outside the lock. This tier is consulted on the
    // read path, and a mutex held across a disk seek would serialize every
    // reader in the stage behind the slowest one -- the same objection §5 makes
    // to a global lock in memory, where the wait is shorter.
    std::string raw;
    const bool read = ReadWholeFile(path, &raw);

    DecodeOutcome outcome = DecodeOutcome::Corrupt;
    if (read) {
        outcome = DecodeEntry(raw, identity, digest, blockIndex, expectedLength, out);
    }

    if (outcome == DecodeOutcome::Corrupt && read) {
        // Read, and not usable: this store's own litter, and it is deleted
        // rather than left to be re-read on every miss for the life of the
        // directory. A file that could not be read at all is left alone, since
        // "could not read" includes "does not exist".
        std::error_code error;
        fs::remove(path, error);
        const std::lock_guard<std::mutex> lock(_impl->mutex);
        ++_impl->stats.discarded;
    }

    const std::lock_guard<std::mutex> lock(_impl->mutex);
    if (outcome == DecodeOutcome::Ok) {
        ++_impl->stats.hits;
        _impl->stats.bytesRead += expectedLength;
        return true;
    }
    if (outcome == DecodeOutcome::OtherKey) {
        ++_impl->stats.collisions;
    }
    ++_impl->stats.misses;
    return false;
}

bool DiskBlockStore::Store(const AssetIdentity& identity,
                           std::uint64_t blockIndex,
                           bool persistable,
                           const unsigned char* bytes,
                           std::size_t length) {
    if (bytes == nullptr || length == 0 || length > kMaxEntryPayload) {
        return false;
    }

    const IdentityDigest digest = DigestOf(identity, blockIndex);
    fs::path path;
    {
        const std::lock_guard<std::mutex> lock(_impl->mutex);
        if (!_impl->enabled) {
            return false;
        }
        if (!persistable) {
            // CACHE.md §8's table, and the only place it is enforced. Counted
            // rather than silent, because "the cache is not filling up" is a
            // question a deployment will ask and the answer is usually this.
            ++_impl->stats.rejected;
            return false;
        }
        path = _impl->PathFor(NameFor(digest));
    }

    const std::string encoded = EncodeEntry(identity, digest, blockIndex, bytes, length);

    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        const std::lock_guard<std::mutex> lock(_impl->mutex);
        ++_impl->stats.writeFailures;
        return false;
    }

    // The temporary lives in the destination's own directory, so the rename
    // cannot cross a filesystem and therefore cannot degrade into a copy.
    fs::path temporary = path;
    temporary += "." + HexOf(ProcessNonce()) + "." +
                 HexOf(_impl->tempCounter.fetch_add(1, std::memory_order_relaxed)) + kTempSuffix;

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            const std::lock_guard<std::mutex> lock(_impl->mutex);
            ++_impl->stats.writeFailures;
            return false;
        }
        output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        output.flush();
        if (!output) {
            output.close();
            std::error_code removeError;
            fs::remove(temporary, removeError);
            const std::lock_guard<std::mutex> lock(_impl->mutex);
            ++_impl->stats.writeFailures;
            return false;
        }
    }

    fs::rename(temporary, path, error);
    if (error) {
        // Lost the race, or the destination is held open by a reader on a
        // platform that will not rename over one. Either way the bytes are
        // already in the caller's hands, and the temporary goes.
        std::error_code removeError;
        fs::remove(temporary, removeError);

        // Whether that was a *failure* is what the destination says, and asking
        // is what keeps the header's promise that neither half of a lost race
        // is reported as one. An entry sitting there is the winner: byte for
        // byte this entry, because both were built from the same key. Nothing
        // there is a filesystem that refused the write, and that is the case
        // `writeFailures` exists to show. On Windows the difference is not
        // hypothetical -- a concurrent `Load` holding the destination open
        // fails this rename on a cache that is working exactly as intended.
        std::error_code existsError;
        const bool published = fs::exists(path, existsError) && !existsError;
        const std::lock_guard<std::mutex> lock(_impl->mutex);
        if (!published) {
            ++_impl->stats.writeFailures;
        }
        return false;
    }

    Impl::SweepPlan plan;
    bool sweep = false;
    {
        const std::lock_guard<std::mutex> lock(_impl->mutex);
        ++_impl->stats.writes;
        _impl->stats.bytesWritten += encoded.size();
        _impl->bytesSinceSweep += encoded.size();
        _impl->largestEntryBytes =
            (std::max)(_impl->largestEntryBytes, static_cast<std::uint64_t>(encoded.size()));
        if (!_impl->swept || _impl->bytesSinceSweep >= _impl->SweepIntervalLocked()) {
            sweep = _impl->BeginSweepLocked(&plan);
        }
    }
    // Outside the lock, and after the entry this call was asked for is already
    // published: a caller waiting on `Store` waits for its own write, never for
    // somebody else's directory walk.
    if (sweep) {
        _impl->RunSweep(plan);
    }
    return true;
}

void DiskBlockStore::Clear() {
    const std::lock_guard<std::mutex> lock(_impl->mutex);
    if (!_impl->enabled) {
        return;
    }
    std::error_code error;
    fs::remove_all(_impl->root, error);
    fs::create_directories(_impl->root, error);
    _impl->knownBytes = 0;
    _impl->bytesSinceSweep = 0;
    _impl->largestEntryBytes = 0;
    _impl->swept = false;
    ++_impl->epoch;
}

}  // namespace cache
}  // namespace usdasset
