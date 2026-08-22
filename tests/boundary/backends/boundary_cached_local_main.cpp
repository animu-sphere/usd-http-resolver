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
//
// The row itself is in CachedLocalRow.h, shared with `persisted_local`, which
// runs the same cases with the on-disk tier underneath. Sharing it is what
// makes the two comparable.

#include "CachedLocalRow.h"
#include "usdassetboundary/Suite.h"

namespace {

/// The store this row uses. Its own rather than the process one, so that the
/// row's budget is the budget in force.
usdasset::cache::BlockCache& RowStore() {
    static usdasset::cache::BlockCache store{usdassetboundaryrows::RowOptions()};
    return store;
}

}  // namespace

int main(int argc, char** argv) {
    return usdassetboundary::RunBoundarySuite(
        usdassetboundaryrows::MakeCachedLocalBackend("cached-local", RowStore(), nullptr),
        argc, argv);
}
