// SPDX-License-Identifier: Apache-2.0
//
// The block cache, as a decorator over `AssetReader`.
//
// It holds a reader it did not construct and knows no transport concept: no
// URL parsing, no header, no status code, no client library. That is not
// tidiness, it is what keeps the local backend a usable oracle for the cached
// path -- the boundary suite runs against `local` and against `cache over
// local` and compares the two, and it could not if this file knew what HTTP
// was (WORKSPACE.md section 2, invariant 5).
//
// What it does:
//
//   read (offset, size)
//     -> resolve against the asset size, exactly once, in usdAssetIo
//     -> expand to whole blocks
//     -> serve what is resident, fetch what is not, wait for what someone else
//        is already fetching
//     -> merge the fetches, bounded by a gap and by a length
//
// What it deliberately does not do: revalidate. A reader is bound to one
// revision for its lifetime (ASSET_READER.md section 2.1), and the blocks this
// cache holds for it were captured under that binding, so serving them is
// serving the bound revision. `AssetChanged` is reported by the reader
// underneath, on the reads that reach it; a hit reaches nothing and observes
// nothing, and the contract's wording is exactly that -- "a reader that
// observes a changed validator fails subsequent reads".
//
// Normative contract: docs/architecture/CACHE.md.

#ifndef USDASSETCACHE_CACHEDASSETREADER_H
#define USDASSETCACHE_CACHEDASSETREADER_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "usdAssetIo/AssetReader.h"
#include "usdAssetIo/Metrics.h"
#include "usdAssetCache/BlockCache.h"
#include "usdAssetCache/CacheOptions.h"

namespace usdasset {
namespace cache {

/// A reader that answers from blocks, over a reader that answers from a
/// transport.
///
/// Thread-safe, and with no lock of its own: everything mutable is either a
/// relaxed counter or lives in the store, whose locks are per block.
class CachedAssetReader final : public AssetReader {
public:
    ~CachedAssetReader() override;

    /// The decorated reader's metadata, verbatim. A cache that restated an
    /// asset's size or validator would be a second place for them to be wrong.
    const AssetMetadata& Metadata() const override;

    /// Reads up to `size` bytes at `offset`, with the semantics every backend
    /// implements -- the same EOF boundary, the same overflow rule, the same
    /// refusal to return a hole. The shared boundary suite runs against this
    /// reader unchanged, which is the only statement of equivalence worth
    /// making.
    ///
    /// A read of at least `CacheOptions::bypassThresholdBytes` goes straight to
    /// the reader underneath and stores nothing.
    ReadResult Read(std::uint64_t offset, void* dst, std::size_t size) override;

    /// The resolved options this reader is using -- normalized, so they are
    /// what is in force rather than what was asked for.
    const CacheOptions& Options() const noexcept;

    /// The whole stack's counters: this decorator's cache counters and caller
    /// side accounting, composed with the transport counters of the reader
    /// underneath.
    ///
    /// This, and not `Metrics()`, is what a test or a baseline harness asserts
    /// on: `Metrics()` is half of the stack, and `amplification` computed from
    /// half of a stack is a ratio between two layers.
    MetricsSnapshot SnapshotMetrics() const;

    /// This decorator's own counter set. It is the one that folds into the
    /// process aggregate, and it absorbs the inner reader's transport counters
    /// when this reader closes.
    const ReaderMetrics& Metrics() const noexcept;

    /// The store binding, for a test that wants to see identity sharing rather
    /// than infer it from a request count.
    const BlockCache::Binding& Binding() const noexcept;

private:
    class Impl;
    explicit CachedAssetReader(std::unique_ptr<Impl> impl);

    friend struct CachedReaderFactory;

    std::unique_ptr<Impl> _impl;
};

/// The result of decorating a reader, typed to the concrete reader.
struct CachedOpenResult {
    std::unique_ptr<CachedAssetReader> reader;  ///< Null exactly when `status` fails.
    Status status;
};

/// Wraps `inner` in a block cache.
///
/// `innerMetrics` is the counter set of the reader being wrapped, and passing
/// it is what makes the stack report one set of numbers instead of two. The
/// caller supplies it because only the caller knows the concrete backend: this
/// module may not name one, and `AssetReader` deliberately carries no metrics
/// accessor. Null is legal and means the stack reports the cache's counters
/// only, with the transport's folding separately.
///
/// `store` is the block store to bind into. Null takes the process store, which
/// is the normal case: the budget is process-wide and shared across assets
/// (CACHE.md section 7), so a store per reader would not be one budget.
///
/// Fails with `InvalidArgument` when `inner` is null.
///
/// It does **not** check `supportsRandomAccess`, and cannot: it returns a
/// `CachedAssetReader`, so it has no way to hand back an undecorated reader.
/// Caching a reader that cannot seek would store the one block it managed to
/// read and miss forever after, so a caller that cannot guarantee random access
/// wants `WrapAsset` below, which can pass one through.
CachedOpenResult Wrap(std::unique_ptr<AssetReader> inner,
                      ReaderMetrics* innerMetrics,
                      const CacheOptions& options,
                      BlockCache* store);

/// The same wrap, in the shape every backend's open returns, so that a caller
/// that has an `OpenResult` can decorate it in one line and hand the result
/// wherever an `AssetReader` goes.
///
/// A failed open passes through untouched: there is no reader to decorate, and
/// replacing the backend's status with one of this module's would erase the
/// only useful thing the result carries.
OpenResult WrapAsset(OpenResult inner,
                     ReaderMetrics* innerMetrics,
                     const CacheOptions& options,
                     BlockCache* store);

}  // namespace cache
}  // namespace usdasset

#endif  // USDASSETCACHE_CACHEDASSETREADER_H
