// SPDX-License-Identifier: Apache-2.0
//
// The platform layer, kept to four operations: open, stat, positional read, and
// close. Internal to usdAssetLocal and not installed.
//
// Positional is the point. `pread` and `ReadFile` with an explicit `OVERLAPPED`
// offset do not consult or advance a shared file pointer, which is what lets
// any number of threads read one reader concurrently without a lock -- the
// concurrency guarantee in §7 of the design policy, obtained from the syscall
// rather than bolted on above it.

#ifndef USDASSETLOCAL_PLATFORMFILE_H
#define USDASSETLOCAL_PLATFORMFILE_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "usdAssetIo/Diagnostics.h"

namespace usdasset {
namespace local {
namespace detail {

#if defined(_WIN32)
using NativeHandle = void*;  // HANDLE
#else
using NativeHandle = int;    // file descriptor
#endif

NativeHandle InvalidHandle() noexcept;
bool IsValidHandle(NativeHandle handle) noexcept;

/// Everything a filesystem can say about which bytes a file currently holds.
/// The derived validator is a rendering of exactly this and nothing else.
struct FileIdentity {
    std::uint64_t size = 0;
    std::uint64_t deviceId = 0;      ///< Volume serial, or st_dev.
    std::uint64_t fileId = 0;        ///< File index, or st_ino.
    std::uint64_t modifiedTicks = 0; ///< Platform ticks; monotonic, not a date.
};

bool operator==(const FileIdentity& lhs, const FileIdentity& rhs) noexcept;
bool operator!=(const FileIdentity& lhs, const FileIdentity& rhs) noexcept;

/// Opens `path` read-only, sharing it with writers and deleters.
///
/// Sharing is deliberate: an asset that cannot be republished underneath an
/// open reader is an asset whose revision-change behavior can never be tested,
/// and that behavior is the one the boundary suite most needs from this
/// backend.
Status OpenForRead(const std::string& path, NativeHandle* handleOut);

void Close(NativeHandle handle) noexcept;

Status Stat(NativeHandle handle, FileIdentity* identityOut);

/// Reads at most `size` bytes at `offset`. Sets `*readOut` to what was actually
/// delivered; zero means the file ended there, which the caller interprets
/// against the size it captured at open.
Status PositionalRead(NativeHandle handle,
                      void* dst,
                      std::size_t size,
                      std::uint64_t offset,
                      std::size_t* readOut);

/// The opaque byte string that becomes `Validator::value`.
std::string FormatIdentity(const FileIdentity& identity);

}  // namespace detail
}  // namespace local
}  // namespace usdasset

#endif  // USDASSETLOCAL_PLATFORMFILE_H
