// SPDX-License-Identifier: Apache-2.0
//
// The row a backend is entered into the suite as.
//
// Adding a transport adds a row, not a suite. Four things are declared and
// nothing else is negotiable (docs/contributing/BOUNDARY_SUITE.md §6): a
// factory, fixture provisioning, whether the backend admits cancellation, and
// whether it can change an asset underneath an open reader. A backend that
// needs a boundary case relaxed has either found a defect in the read contract
// -- in which case ASSET_READER.md changes first, for every backend -- or it is
// not admissible.

#ifndef USDASSETBOUNDARY_BACKEND_H
#define USDASSETBOUNDARY_BACKEND_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "usdAssetIo/AssetReader.h"

namespace usdassetboundary {

/// How a fixture's transport is required to behave.
///
/// Behavior is part of fixture provisioning rather than a fifth declaration,
/// because that is what it is: "an asset that short-reads" is a fixture a
/// backend must be able to make exist, in the same sense that "an asset of
/// 65536 bytes" is. For a hostile HTTP server it is a truncated body; for a
/// local file it is an injected read fault.
enum class FixtureBehavior {
    Normal,
    /// The transport accepts the range, delivers fewer bytes than it covers,
    /// and stops below the end of the asset. The reader must report
    /// InvalidResponse rather than hand back a buffer with a hole in it.
    ShortReadBelowEof,
};

/// What the suite asks a backend to make exist.
struct FixtureRequest {
    std::string name;
    FixtureBehavior behavior = FixtureBehavior::Normal;
    std::uint64_t size = 0;
    /// The suite's own copy of the content, already written to the local
    /// filesystem. The oracle reads this; a backend that serves bytes from
    /// somewhere else copies from it.
    std::string oraclePath;
};

/// What provisioning hands back.
struct ProvisionedAsset {
    std::string identifier;
    /// Per-asset factory, for a fixture the backend's ordinary factory cannot
    /// express -- a misbehaving transport, typically. Null means "open
    /// `identifier` with the backend's own factory".
    std::function<usdasset::OpenResult()> open;
};

struct BackendUnderTest {
    std::string name;

    /// Fixture provisioning.
    std::function<ProvisionedAsset(const FixtureRequest&)> provision;

    /// The factory.
    std::function<usdasset::OpenResult(const std::string& identifier)> open;

    /// Whether this backend admits cancellation. Declared rather than
    /// discovered: a case that a backend cannot run is announced by the row,
    /// not by a test that quietly skips itself.
    bool admitsCancellation = false;

    /// Whether this backend can change an asset underneath an open reader.
    bool simulatesRevisionChange = false;

    /// Required when `simulatesRevisionChange`. Replaces the asset's content
    /// while readers are open on it.
    std::function<void(const std::string& identifier,
                       const std::vector<unsigned char>& content)>
        republish;
};

}  // namespace usdassetboundary

#endif  // USDASSETBOUNDARY_BACKEND_H
