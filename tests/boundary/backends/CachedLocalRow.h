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
//
// One thing follows from that argument rather than sitting beside it: the
// persisted row also relabels the validator's *kind*, because the tier declines
// to write an entry for an identity a backend synthesized. `OriginValidator`
// below is that, and says why it is narrow enough to keep the two rows one
// experiment.

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
#include "usdAssetIo/AssetReader.h"
#include "usdAssetIo/Validator.h"
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

/// Presents the local backend's identity as one an origin issued.
///
/// `Persistable` writes an entry only for a `ValidatorKind::EntityTag`. An
/// identity a *backend* synthesized is strong for as long as that backend holds
/// the asset open -- which is what `usdAssetLocal` claims about its own, in as
/// many words -- and that is a claim about a reader's lifetime rather than a
/// directory's. The rule is right, and this row is not arguing with it. But a
/// row about persistence has to put bytes on a disk to be one, and the local
/// backend is the only one the boundary suite can provision a fixture for.
///
/// So the relabel is as narrow as it can be made. The validator's *value* is
/// untouched -- still device, file index, size, and mtime -- so a republished
/// fixture is still a different identity and every revision case still fails
/// exactly the way it is meant to. One field changes, on one row, inside a test
/// binary that provisions, reads, and removes its own fixtures in one process:
/// the window the derived claim is valid in anyway.
class OriginValidator : public usdasset::AssetReader {
public:
    explicit OriginValidator(std::unique_ptr<usdasset::AssetReader> inner)
        : _inner(std::move(inner)), _metadata(_inner->Metadata()) {
        _metadata.validator.kind = usdasset::ValidatorKind::EntityTag;
    }

    const usdasset::AssetMetadata& Metadata() const override { return _metadata; }

    usdasset::ReadResult Read(std::uint64_t offset, void* dst, std::size_t size) override {
        return _inner->Read(offset, dst, size);
    }

private:
    std::unique_ptr<usdasset::AssetReader> _inner;
    usdasset::AssetMetadata _metadata;
};

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
        // Taken before the reader is handed on, and still the local reader's
        // own however many decorators end up between it and the cache.
        usdasset::ReaderMetrics* innerMetrics = &local.reader->Metrics();
        std::unique_ptr<usdasset::AssetReader> inner(local.reader.release());
        if (persistent != nullptr) {
            usdasset::AssetReader* relabelled = new OriginValidator(std::move(inner));
            inner.reset(relabelled);
        }
        usdasset::cache::CachedOpenResult cached = usdasset::cache::Wrap(
            std::move(inner), innerMetrics, RowOptions(), &store, persistent);
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
