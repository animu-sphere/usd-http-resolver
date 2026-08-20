// SPDX-License-Identifier: Apache-2.0
//
// The cache's policy constants, in one struct.
//
// Every value here is a measured constant rather than a guessed one, per §5 of
// the design policy: "a tuned constant without a recorded measurement is a
// guess with a decimal point". The measurement that produced the defaults is
// tests/cache-tuning, and it is recorded in docs/reference/BLOCK_POLICY.md.
//
// They are parameters rather than compile-time constants for the reason the
// transport bounds are: a test has to be able to make a budget overflow in
// kilobytes instead of provisioning a gigabyte, and the environment surface in
// CONFIGURATION.md §2 has to have something to set.
//
// Normative contract: docs/architecture/CACHE.md.

#ifndef USDASSETCACHE_CACHEOPTIONS_H
#define USDASSETCACHE_CACHEOPTIONS_H

#include <cstdint>

namespace usdasset {
namespace cache {

/// The smallest and largest block this cache will use, whatever it is asked
/// for. Below the floor the per-block bookkeeping costs more than the block;
/// above the ceiling one miss transfers more than most assets are worth.
inline constexpr std::uint64_t kMinBlockSize = 4096;
inline constexpr std::uint64_t kMaxBlockSize = 64ull * 1024 * 1024;

/// Defaults. See BLOCK_POLICY.md for the runs these came from.
inline constexpr std::uint64_t kDefaultBlockSize = 64ull * 1024;
inline constexpr std::uint64_t kDefaultBudgetBytes = 128ull * 1024 * 1024;
inline constexpr std::uint32_t kDefaultCoalesceGapBlocks = 1;
inline constexpr std::uint64_t kDefaultMaxRequestBytes = 8ull * 1024 * 1024;
inline constexpr std::uint64_t kDefaultBypassThresholdBytes = 1ull * 1024 * 1024;

struct CacheOptions {
    /// A power of two. Fixed per reader for its lifetime, because it is part
    /// of the cache key: a reader that changed it mid-flight would be looking
    /// up blocks that were stored under a different arithmetic.
    std::uint64_t blockSize = kDefaultBlockSize;

    /// The resident ceiling, process-wide and shared across assets, so that one
    /// enormous asset cannot starve the rest of the stage (CACHE.md §7).
    std::uint64_t budgetBytes = kDefaultBudgetBytes;

    /// The largest run of blocks this reader will fetch *through* in order to
    /// merge the fetches on either side of it. Zero merges nothing.
    ///
    /// The trade is stated in CACHE.md §4: transferring the gap costs less than
    /// a second round trip, up to some width, and past that width it is just
    /// bytes nobody asked for.
    std::uint32_t coalesceGapBlocks = kDefaultCoalesceGapBlocks;

    /// The ceiling on one merged request. A merge is never taken past this,
    /// because one enormous request defeats cancellation and stalls every other
    /// read on the connection.
    std::uint64_t maxRequestBytes = kDefaultMaxRequestBytes;

    /// A read at least this large bypasses the cache entirely: it is served
    /// straight from the reader underneath, and nothing it moved is stored.
    ///
    /// Not an optimization -- a correctness-of-policy rule. A streaming pass
    /// over a large asset would otherwise evict the whole working set to store
    /// bytes that will never be read twice, and the read that follows it would
    /// pay for the privilege (CACHE.md §3).
    std::uint64_t bypassThresholdBytes = kDefaultBypassThresholdBytes;

    /// The same options with every field made legal: `blockSize` rounded down
    /// to a power of two inside [kMinBlockSize, kMaxBlockSize], and the rest
    /// clamped so that a merged request can always hold at least one block and
    /// the budget can always hold at least one.
    ///
    /// Clamps rather than fails. A caller reaches this with values from an
    /// environment variable, and CONFIGURATION.md §2 already fixes what a bad
    /// value costs -- a diagnostic at the point it is read, and the default in
    /// force -- so a second failure mode here would only make the first
    /// unreachable.
    CacheOptions Normalized() const noexcept;

    /// True when `blockSize` is a power of two within the bounds and every
    /// other field is consistent with it. `Normalized()` is idempotent, which
    /// is what this predicate is for in a test.
    bool IsNormalized() const noexcept;
};

}  // namespace cache
}  // namespace usdasset

#endif  // USDASSETCACHE_CACHEOPTIONS_H
