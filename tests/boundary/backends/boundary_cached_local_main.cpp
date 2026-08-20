// SPDX-License-Identifier: Apache-2.0
//
// The block cache's row in the shared boundary suite, over the local backend.
//
// The cache is not a transport, so this row is not a fourth backend: it is the
// same local backend with a decorator on top, and entering it here is what
// makes "byte-for-byte equivalence with the uncached path over the full suite"
// an assertion rather than a claim. Every case runs unchanged, and the oracle
// is the same independent naive reader the `local` row is compared against --
// so a block boundary that is off by one, a short final block that got padded,
// or an EOF answer the expansion moved shows up as a byte mismatch against a
// file, not as an argument about caching.
//
// It is the local backend underneath rather than the HTTP one deliberately. A
// cached row over a socket would be measuring two things at once, and the cache
// knows no transport concept -- if this row needed one, the decorator would
// have acquired knowledge WORKSPACE.md invariant 5 forbids it.

#include <memory>
#include <string>
#include <vector>

#include "usdAssetCache/BlockCache.h"
#include "usdAssetCache/CacheOptions.h"
#include "usdAssetCache/CachedAssetReader.h"
#include "usdAssetLocal/LocalAssetReader.h"
#include "usdAssetLocal/Testing.h"
#include "usdassetboundary/Backend.h"
#include "usdassetboundary/Fixture.h"
#include "usdassetboundary/Suite.h"

namespace {

/// Deliberately smaller than the shipped default, and smaller than the suite's
/// nominal alignment.
///
/// The suite's interesting offsets are powers of two around 65536, and a block
/// size of 65536 would put every one of them exactly on a block boundary --
/// which is the one case block arithmetic never gets wrong. At 4096 the suite's
/// offsets land inside blocks, across blocks, and on boundaries, and the tail
/// of every fixture is a short final block.
const std::uint64_t kRowBlockSize = 4096;

usdasset::cache::CacheOptions RowOptions() {
    usdasset::cache::CacheOptions options;
    options.blockSize = kRowBlockSize;
    // Small enough that eviction runs during the suite rather than sitting
    // unexercised: the property cases walk assets larger than this, so blocks
    // are dropped and re-fetched while the oracle comparison is watching.
    options.budgetBytes = 64 * kRowBlockSize;
    options.coalesceGapBlocks = 1;
    options.maxRequestBytes = 32 * kRowBlockSize;
    // Above the largest whole-asset read the suite issues, so the cached path
    // is the path under test rather than the bypass.
    options.bypassThresholdBytes = 4 * usdassetboundary::kNominalBlockSize;
    return options.Normalized();
}

/// The store this row uses. Its own rather than the process one, so that the
/// budget above is the budget in force.
usdasset::cache::BlockCache& RowStore() {
    static usdasset::cache::BlockCache store{RowOptions()};
    return store;
}

usdasset::OpenResult Decorate(usdasset::local::LocalOpenResult local) {
    usdasset::OpenResult result;
    if (!local.reader) {
        result.status = std::move(local.status);
        return result;
    }
    // The inner reader's counters, so the stack reports one set of numbers.
    usdasset::ReaderMetrics* innerMetrics = &local.reader->Metrics();
    usdasset::cache::CachedOpenResult cached = usdasset::cache::Wrap(
        std::unique_ptr<usdasset::AssetReader>(local.reader.release()), innerMetrics,
        RowOptions(), &RowStore());
    result.reader = std::move(cached.reader);
    result.status = cached.reader ? std::move(local.status) : std::move(cached.status);
    return result;
}

/// The same injected fault the `local` row uses: half of the first request, and
/// then nothing. Through the cache it is a fetch of a whole block that stops
/// below the end of the asset, which is the same condition the contract names.
usdasset::local::testing::ReadFault MakeShortReadFault() {
    auto delivered = std::make_shared<bool>(false);
    return [delivered](std::uint64_t, std::size_t size) -> std::size_t {
        if (*delivered) {
            return 0;
        }
        *delivered = true;
        return size / 2;
    };
}

usdassetboundary::BackendUnderTest MakeCachedLocalBackend() {
    usdassetboundary::BackendUnderTest backend;
    backend.name = "cached-local";

    backend.open = [](const std::string& identifier) {
        return Decorate(usdasset::local::Open(identifier));
    };

    backend.provision = [](const usdassetboundary::FixtureRequest& request) {
        usdassetboundary::ProvisionedAsset asset;
        asset.identifier = request.oraclePath;
        if (request.behavior == usdassetboundary::FixtureBehavior::ShortReadBelowEof) {
            const std::string path = request.oraclePath;
            asset.open = [path] {
                return Decorate(usdasset::local::testing::OpenWithReadFault(
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

}  // namespace

int main(int argc, char** argv) {
    return usdassetboundary::RunBoundarySuite(MakeCachedLocalBackend(), argc, argv);
}
