// SPDX-License-Identifier: Apache-2.0
//
// The persistent tier's row in the shared boundary suite.
//
// The same row as `cached-local`, with the on-disk tier underneath it. It is a
// separate executable rather than a flag on that one because the claim being
// made is a comparison: every case of the suite produces the same bytes with
// the tier and without it, and a comparison needs both halves to run.
//
// What the disk actually answers during a run is the row's budget doing its
// second job. The block store holds sixty-four blocks and the property cases
// walk assets larger than that, so blocks are evicted while the oracle
// comparison is watching -- and an evicted block is exactly where this tier is
// read from. The cross-*process* half of the claim is not here, because a
// suite row is one process; it is in `usdAssetCache_persistence`, where a
// second block store over one directory is the condition a second process is.
//
// The cache directory is this process's, named for the row and for the moment
// the process started, and removed when the suite finishes. The reason is the one
// FixtureWorkspace gives: two runs of one row -- `ctest -j` over two
// configurations, or an ASan and a TSan build on one machine -- must not share
// a path, or one deletes what the other is reading and the failure reads as a
// defect in the backend.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include "CachedLocalRow.h"
#include "usdassetboundary/Suite.h"

namespace {

namespace fs = std::filesystem;

/// The cache directory, created on first use and removed at destruction.
class CacheDirectory {
public:
    CacheDirectory() {
        const auto now = std::chrono::system_clock::now().time_since_epoch().count();
        _path = fs::temp_directory_path() /
                ("usd-http-resolver-boundary-persisted-" + std::to_string(now));
        std::error_code error;
        fs::create_directories(_path, error);
    }

    ~CacheDirectory() {
        std::error_code error;
        fs::remove_all(_path, error);
    }

    CacheDirectory(const CacheDirectory&) = delete;
    CacheDirectory& operator=(const CacheDirectory&) = delete;

    std::string String() const { return _path.string(); }

private:
    fs::path _path;
};

const CacheDirectory& Directory() {
    static const CacheDirectory directory;
    return directory;
}

/// The disk tier this row uses. Its own rather than the process one, so that a
/// row which is about persistence cannot pass because some other row left
/// something behind.
///
/// It reaches `Directory()` during its own initialization, which is what fixes
/// the order: the directory is constructed first and therefore destroyed last,
/// so nothing can write into a path that has already been removed.
usdasset::cache::DiskBlockStore& RowDiskStore() {
    static usdasset::cache::DiskBlockStore store{[] {
        usdasset::cache::DiskCacheOptions options;
        options.directory = Directory().String();
        // Comfortably larger than every fixture the suite provisions, so that
        // the disk tier is exercised as a cache and not as a trimmer. Its
        // trimming is `usdAssetCache_persistence`'s question.
        options.budgetBytes = 64ull * 1024 * 1024;
        return options;
    }()};
    return store;
}

usdasset::cache::BlockCache& RowStore() {
    static usdasset::cache::BlockCache store{usdassetboundaryrows::RowOptions()};
    return store;
}

}  // namespace

int main(int argc, char** argv) {
    return usdassetboundary::RunBoundarySuite(
        usdassetboundaryrows::MakeCachedLocalBackend("persisted-local", RowStore(),
                                                     &RowDiskStore()),
        argc, argv);
}
