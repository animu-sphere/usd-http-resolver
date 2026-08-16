// SPDX-License-Identifier: Apache-2.0
//
// The local backend's row in the shared boundary suite.
//
// This file is what "adding a transport adds a row, not a suite" means in
// practice. It declares four things -- a factory, fixture provisioning, whether
// cancellation is admitted, and whether a revision change can be simulated --
// and then hands the row to the suite. The HTTP backend's row in v0.2.0 is a
// file of about this length beside this one, and the suite it runs is
// byte-identical.

#include <memory>
#include <string>
#include <vector>

#include "usdAssetLocal/LocalAssetReader.h"
#include "usdAssetLocal/Testing.h"
#include "usdassetboundary/Backend.h"
#include "usdassetboundary/Fixture.h"
#include "usdassetboundary/Suite.h"

namespace {

usdasset::OpenResult ToSharedResult(usdasset::local::LocalOpenResult local) {
    usdasset::OpenResult result;
    result.status = std::move(local.status);
    result.reader = std::move(local.reader);
    return result;
}

/// A transport that accepts the range, delivers half of it once, and then
/// stalls. The reader must call that a short read below EOF rather than hand
/// back a buffer with a hole in it.
///
/// State lives in the closure, so every open gets a fresh fault and the
/// misbehaving fixture cannot disturb the readers running beside it.
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

usdassetboundary::BackendUnderTest MakeLocalBackend() {
    usdassetboundary::BackendUnderTest backend;
    backend.name = "local";

    backend.open = [](const std::string& identifier) {
        return usdasset::local::OpenAsset(identifier);
    };

    backend.provision = [](const usdassetboundary::FixtureRequest& request) {
        usdassetboundary::ProvisionedAsset asset;
        // A local asset is served from the same file the oracle reads. That is
        // the one thing this backend gets for free and every remote one has to
        // arrange.
        asset.identifier = request.oraclePath;

        if (request.behavior == usdassetboundary::FixtureBehavior::ShortReadBelowEof) {
            const std::string path = request.oraclePath;
            asset.open = [path] {
                return ToSharedResult(usdasset::local::testing::OpenWithReadFault(
                    path, MakeShortReadFault()));
            };
        }
        return asset;
    };

    // The read contract carries no cancellation token, so a backend admits
    // cancellation only if its transport can be told out of band. A file
    // descriptor cannot be.
    backend.admitsCancellation = false;

    // It can: rewriting the file underneath an open handle is exactly what a
    // republished remote asset does, and it is why the local backend is a
    // usable oracle for revision binding as well as for bytes.
    backend.simulatesRevisionChange = true;
    backend.republish = [](const std::string& identifier,
                           const std::vector<unsigned char>& content) {
        return usdassetboundary::RepublishFile(identifier, content);
    };

    return backend;
}

}  // namespace

int main(int argc, char** argv) {
    return usdassetboundary::RunBoundarySuite(MakeLocalBackend(), argc, argv);
}
