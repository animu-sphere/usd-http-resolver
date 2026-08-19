// SPDX-License-Identifier: Apache-2.0
//
// The fixture set every backend is driven over.

#ifndef USDASSETBOUNDARY_FIXTURE_H
#define USDASSETBOUNDARY_FIXTURE_H

#include <cstdint>
#include <string>
#include <vector>

namespace usdassetboundary {

/// A nominal alignment, used only to choose interesting offsets.
///
/// It is not the cache's block size. That is a measured constant chosen in
/// v0.3.0 against a realistic access pattern; this is a stand-in that exists so
/// the cache inherits passing cases at power-of-two offsets and their
/// neighbours rather than acquiring them afterwards
/// (docs/contributing/BOUNDARY_SUITE.md §3).
constexpr std::uint64_t kNominalBlockSize = 65536;

/// Content whose every byte is a function of its position, so that a read
/// landing one byte off returns data that is obviously from elsewhere rather
/// than plausibly correct.
std::vector<unsigned char> PositionalContent(std::size_t size, std::uint64_t seed = 0);

/// The required sizes: an empty asset, a one-byte asset, one smaller than a
/// block, one exactly a block, and one spanning several blocks with a short
/// final block. Chosen so that the boundary cases above are distinct from each
/// other rather than accidentally the same read.
struct FixtureSize {
    const char* name;
    std::uint64_t size;
};

const std::vector<FixtureSize>& RequiredFixtureSizes();

/// A directory of fixture content, removed when the suite finishes.
///
/// The directory is named for the row *and* for the process that owns it. The
/// tag alone is not unique: two runs of the same row -- an ASan build and a
/// TSan build on one machine, or `ctest -j` over two configurations -- would
/// otherwise share one path, and the constructor's cleanup of a stale directory
/// would delete fixtures the other run was still reading. That failure arrives
/// as `oracle NotFound` against a backend that returned correct bytes, which
/// reads exactly like a defect in the backend and is not one.
class FixtureWorkspace {
public:
    explicit FixtureWorkspace(const std::string& tag);
    ~FixtureWorkspace();

    FixtureWorkspace(const FixtureWorkspace&) = delete;
    FixtureWorkspace& operator=(const FixtureWorkspace&) = delete;

    /// Writes `content` and returns the path the oracle will read it from.
    std::string Write(const std::string& name, const std::vector<unsigned char>& content);

    bool Valid() const;

private:
    std::string _path;
};

/// Replaces a file's content, and forces its modification timestamp to differ
/// from the one it had.
///
/// The timestamp is set explicitly rather than left to the clock: a rewrite
/// completing inside one filesystem timestamp tick is exactly the case a
/// derived validator cannot see, and a revision-change test that depends on
/// wall-clock timing to avoid it is a test that fails once a month in CI for
/// reasons nobody can reproduce.
bool RepublishFile(const std::string& path, const std::vector<unsigned char>& content);

}  // namespace usdassetboundary

#endif  // USDASSETBOUNDARY_FIXTURE_H
