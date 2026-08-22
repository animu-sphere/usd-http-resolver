// SPDX-License-Identifier: Apache-2.0
//
// The cache's row over the local backend, shared by the two executables that
// enter it: once with the persistent tier off, and once with it on.
//
// The two rows have to be the same row. If the persisted one built its backend
// separately it could drift -- a different block size, a different budget, a
// different fault -- and then "byte-for-byte equivalent with and without
// persistence" would be a comparison between two different experiments. So the
// options, the fault, and the provisioning live here and the difference between
// the rows is one argument.

#ifndef USDASSETBOUNDARY_BACKENDS_CACHEDLOCALROW_H
#define USDASSETBOUNDARY_BACKENDS_CACHEDLOCALROW_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "usdAssetCache/BlockCache.h"
#include "usdAssetCache/CacheOptions.h"
#include "usdAssetCache/CachedAssetReader.h"
#include "usdAssetCache/DiskBlockStore.h"
#include "usdAssetLocal/LocalAssetReader.h"
#include "usdAssetLocal/Testing.h"
#include "usdassetboundary/Backend.h"
#include "usdassetboundary/Fixture.h"

namespace usdassetboundaryrows {

/// Deliberately smaller than the shipped default, and smaller than the suite's
/// nominal alignment.
///
/// The suite's interesting offsets are powers of two around 65536, and a block
/// size of 65536 would put every one of them exactly on a block boundary --
/// which is the one case block arithmetic never gets wrong. At 4096 the suite's
/// offsets land inside blocks, across blocks, and on boundaries, and the tail
/// of every fixture is a short final block.
inline constexpr std::uint64_t kRowBlockSize = 4096;

inline usdasset::cache::CacheOptions RowOptions() {
    usdasset::cache::CacheOptions options;
    options.blockSize = kRowBlockSize;
    // Small enough that eviction runs during the suite rather than sitting
    // unexercised: the property cases walk assets larger than this, so blocks
    // are dropped and re-fetched while the oracle comparison is watching. With
    // the persistent tier on it does a second job -- an evicted block is where
    // that tier is read from, so the same budget that exercises eviction is
    // what makes the disk answer inside one run.
    options.budgetBytes = 64 * kRowBlockSize;
    options.coalesceGapBlocks = 1;
    options.maxRequestBytes = 32 * kRowBlockSize;
    // Above the largest whole-asset read the suite issues, so the cached path
    // is the path under test rather than the bypass.
    options.bypassThresholdBytes = 4 * usdassetboundary::kNominalBlockSize;
    return options.Normalized();
}

/// The same injected fault the `local` row uses: half of the first request, and
/// then nothing. Through the cache it is a fetch of a whole block that stops
/// below the end of the asset, which is the same condition the contract names.
inline usdasset::local::testing::ReadFault MakeShortReadFault() {
    auto delivered = std::make_shared<bool>(false);
    return [delivered](std::uint64_t, std::size_t size) -> std::size_t {
        if (*delivered) {
            return 0;
        }
        *delivered = true;
        return size / 2;
    };
}

/// The row.
///
/// `store` is the block store the row binds into -- its own rather than the
/// process one, so that the budget above is the budget in force. `persistent`
/// is the disk tier, and null is the row that has none.
inline usdassetboundary::BackendUnderTest MakeCachedLocalBackend(
    std::string name,
    usdasset::cache::BlockCache& store,
    usdasset::cache::DiskBlockStore* persistent) {
    usdassetboundary::BackendUnderTest backend;
    backend.name = std::move(name);

    const auto decorate = [&store, persistent](usdasset::local::LocalOpenResult local) {
        usdasset::OpenResult result;
        if (!local.reader) {
            result.status = std::move(local.status);
            return result;
        }
        // The inner reader's counters, so the stack reports one set of numbers.
        usdasset::ReaderMetrics* innerMetrics = &local.reader->Metrics();
        usdasset::cache::CachedOpenResult cached = usdasset::cache::Wrap(
            std::unique_ptr<usdasset::AssetReader>(local.reader.release()), innerMetrics,
            RowOptions(), &store, persistent);
        result.reader = std::move(cached.reader);
        result.status = cached.reader ? std::move(local.status) : std::move(cached.status);
        return result;
    };

    backend.open = [decorate](const std::string& identifier) {
        return decorate(usdasset::local::Open(identifier));
    };

    backend.provision = [decorate](const usdassetboundary::FixtureRequest& request) {
        usdassetboundary::ProvisionedAsset asset;
        asset.identifier = request.oraclePath;
        if (request.behavior == usdassetboundary::FixtureBehavior::ShortReadBelowEof) {
            const std::string path = request.oraclePath;
            asset.open = [decorate, path] {
                return decorate(usdasset::local::testing::OpenWithReadFault(
                    path, MakeShortReadFault()));
            };
        }
        return asset;
    };

    // Unchanged from the row underneath: a decorator cannot add a cancellation
    // channel a file descriptor does not have.
    backend.admitsCancellation = false;

    backend.simulatesRevisionChange = true;
    backend.republish = [](const std::string& identifier,
                           const std::vector<unsigned char>& content) {
        return usdassetboundary::RepublishFile(identifier, content);
    };

    return backend;
}

}  // namespace usdassetboundaryrows

#endif  // USDASSETBOUNDARY_BACKENDS_CACHEDLOCALROW_H
