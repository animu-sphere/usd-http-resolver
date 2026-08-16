// SPDX-License-Identifier: Apache-2.0
//
// Test-only fault injection.
//
// This header exists because of one row of the boundary suite: "short read
// below EOF -> InvalidResponse, never a hole". Every backend runs that case
// unchanged, and a backend proves it by making a fixture whose transport
// misbehaves -- for the HTTP backend that is a hostile server serving a
// truncated body, and for a local file it is this. Provisioning a fixture with
// a declared behavior is part of a backend's suite row
// (docs/contributing/BOUNDARY_SUITE.md §6); a per-backend exemption from the
// case would not be.
//
// Nothing outside a test calls anything here.

#ifndef USDASSETLOCAL_TESTING_H
#define USDASSETLOCAL_TESTING_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "usdAssetLocal/LocalAssetReader.h"

namespace usdasset {
namespace local {
namespace testing {

/// Caps what one positional read is allowed to deliver.
///
/// Called with the offset and the number of bytes the reader still needs, and
/// returns the number of bytes the underlying read may yield. Returning zero
/// stalls the transfer, which is how a genuine short read below EOF is
/// produced: the reader makes no progress, and it must report that rather than
/// hand back a buffer with a hole in it.
using ReadFault = std::function<std::size_t(std::uint64_t offset, std::size_t size)>;

/// Opens `path` with `fault` installed on this reader alone.
///
/// Per reader rather than per process, so that a fixture with a misbehaving
/// transport does not disturb the concurrency cases running beside it.
LocalOpenResult OpenWithReadFault(const std::string& path, ReadFault fault);

}  // namespace testing
}  // namespace local
}  // namespace usdasset

#endif  // USDASSETLOCAL_TESTING_H
