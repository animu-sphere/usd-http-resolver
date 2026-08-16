// SPDX-License-Identifier: Apache-2.0

#include "PlatformFile.h"

#include <algorithm>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace usdasset {
namespace local {
namespace detail {
namespace {

std::string WithPath(const char* what, const std::string& path) {
    // The path is elided even here: a local backend will not normally see a
    // credential, but the rule is that every identifier reaching a message goes
    // through one function, and an exception is how the rule stops holding.
    return std::string(what) + ": " + ElideSecrets(path);
}

}  // namespace

bool operator==(const FileIdentity& lhs, const FileIdentity& rhs) noexcept {
    return lhs.size == rhs.size && lhs.deviceId == rhs.deviceId &&
           lhs.fileId == rhs.fileId && lhs.modifiedTicks == rhs.modifiedTicks;
}

bool operator!=(const FileIdentity& lhs, const FileIdentity& rhs) noexcept {
    return !(lhs == rhs);
}

std::string FormatIdentity(const FileIdentity& identity) {
    // Opaque above this module. Nothing outside parses it; the only operation
    // any other layer performs on a validator value is byte comparison.
    return std::to_string(identity.deviceId) + ":" + std::to_string(identity.fileId) +
           ":" + std::to_string(identity.size) + ":" +
           std::to_string(identity.modifiedTicks);
}

#if defined(_WIN32)

namespace {

std::wstring WidenUtf8(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          &wide[0], needed);
    return wide;
}

Status MapOpenError(DWORD error, const std::string& path) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_PATHNAME:
            return Status::Error(StatusCode::NotFound, WithPath("no such asset", path));
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION: {
            // A directory opens with ERROR_ACCESS_DENIED without
            // FILE_FLAG_BACKUP_SEMANTICS, and reporting that as a permission
            // problem sends a user to look at an ACL that is not the issue.
            // The extra call costs nothing on the path that succeeds.
            const DWORD attributes = ::GetFileAttributesW(WidenUtf8(path).c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                return Status::Error(StatusCode::InvalidArgument,
                                     WithPath("asset identifier names a directory",
                                              path));
            }
            return Status::Error(StatusCode::AccessDenied,
                                 WithPath("asset is not readable by this process",
                                          path));
        }
        default:
            return Status::Error(StatusCode::NetworkError,
                                 WithPath("filesystem error opening asset", path))
                .WithTransportStatus(static_cast<int>(error));
    }
}

}  // namespace

NativeHandle InvalidHandle() noexcept { return INVALID_HANDLE_VALUE; }

bool IsValidHandle(NativeHandle handle) noexcept {
    return handle != INVALID_HANDLE_VALUE && handle != nullptr;
}

Status OpenForRead(const std::string& path, NativeHandle* handleOut) {
    *handleOut = InvalidHandle();

    const std::wstring wide = WidenUtf8(path);
    if (wide.empty() && !path.empty()) {
        return Status::Error(StatusCode::InvalidArgument,
                             WithPath("asset identifier is not valid UTF-8", path));
    }

    const HANDLE handle = ::CreateFileW(
        wide.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return MapOpenError(::GetLastError(), path);
    }

    *handleOut = handle;
    return Status::Ok();
}

void Close(NativeHandle handle) noexcept {
    if (IsValidHandle(handle)) {
        ::CloseHandle(handle);
    }
}

Status Stat(NativeHandle handle, FileIdentity* identityOut) {
    BY_HANDLE_FILE_INFORMATION information;
    if (!::GetFileInformationByHandle(handle, &information)) {
        return Status::Error(StatusCode::NetworkError,
                             "filesystem error reading asset metadata")
            .WithTransportStatus(static_cast<int>(::GetLastError()));
    }

    identityOut->size = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) |
                        information.nFileSizeLow;
    identityOut->deviceId = information.dwVolumeSerialNumber;
    identityOut->fileId = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) |
                          information.nFileIndexLow;
    identityOut->modifiedTicks =
        (static_cast<std::uint64_t>(information.ftLastWriteTime.dwHighDateTime) << 32) |
        information.ftLastWriteTime.dwLowDateTime;
    return Status::Ok();
}

Status PositionalRead(NativeHandle handle,
                      void* dst,
                      std::size_t size,
                      std::uint64_t offset,
                      std::size_t* readOut) {
    *readOut = 0;

    // One call reads at most a DWORD's worth. The caller loops, which it has to
    // do regardless: a partial read is a legal outcome of any single call.
    const DWORD chunk = static_cast<DWORD>(
        (std::min<std::uint64_t>)(size, static_cast<std::uint64_t>(1) << 30));

    OVERLAPPED overlapped;
    ZeroMemory(&overlapped, sizeof(overlapped));
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);

    DWORD delivered = 0;
    if (!::ReadFile(handle, dst, chunk, &delivered, &overlapped)) {
        const DWORD error = ::GetLastError();
        // Reading at or past the end of the file through an explicit offset
        // reports EOF rather than success with zero bytes.
        if (error == ERROR_HANDLE_EOF) {
            return Status::Ok();
        }
        return Status::Error(StatusCode::NetworkError, "filesystem error reading asset")
            .WithRange(offset, size)
            .WithTransportStatus(static_cast<int>(error));
    }

    *readOut = static_cast<std::size_t>(delivered);
    return Status::Ok();
}

#else  // POSIX

namespace {

Status MapErrno(int error, const std::string& path) {
    switch (error) {
        case ENOENT:
        case ENOTDIR:
        case ENAMETOOLONG:
            return Status::Error(StatusCode::NotFound, WithPath("no such asset", path));
        case EACCES:
        case EPERM:
            return Status::Error(StatusCode::AccessDenied,
                                 WithPath("asset is not readable by this process",
                                          path));
        case EISDIR:
            return Status::Error(StatusCode::InvalidArgument,
                                 WithPath("asset identifier names a directory", path));
        default:
            return Status::Error(StatusCode::NetworkError,
                                 WithPath("filesystem error opening asset", path))
                .WithTransportStatus(error);
    }
}

}  // namespace

NativeHandle InvalidHandle() noexcept { return -1; }

bool IsValidHandle(NativeHandle handle) noexcept { return handle >= 0; }

Status OpenForRead(const std::string& path, NativeHandle* handleOut) {
    *handleOut = InvalidHandle();

    int fd = -1;
    do {
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);

    if (fd < 0) {
        return MapErrno(errno, path);
    }

    // A directory opens successfully here and only fails on read, which would
    // surface as a transport fault rather than as the wrong identifier it is.
    struct stat information;
    if (::fstat(fd, &information) == 0 && S_ISDIR(information.st_mode)) {
        ::close(fd);
        return Status::Error(StatusCode::InvalidArgument,
                             WithPath("asset identifier names a directory", path));
    }

    *handleOut = fd;
    return Status::Ok();
}

void Close(NativeHandle handle) noexcept {
    if (IsValidHandle(handle)) {
        ::close(handle);
    }
}

Status Stat(NativeHandle handle, FileIdentity* identityOut) {
    struct stat information;
    if (::fstat(handle, &information) != 0) {
        return Status::Error(StatusCode::NetworkError,
                             "filesystem error reading asset metadata")
            .WithTransportStatus(errno);
    }

    identityOut->size = static_cast<std::uint64_t>(information.st_size);
    identityOut->deviceId = static_cast<std::uint64_t>(information.st_dev);
    identityOut->fileId = static_cast<std::uint64_t>(information.st_ino);
#if defined(__APPLE__)
    identityOut->modifiedTicks =
        static_cast<std::uint64_t>(information.st_mtimespec.tv_sec) * 1000000000ull +
        static_cast<std::uint64_t>(information.st_mtimespec.tv_nsec);
#elif defined(__linux__)
    identityOut->modifiedTicks =
        static_cast<std::uint64_t>(information.st_mtim.tv_sec) * 1000000000ull +
        static_cast<std::uint64_t>(information.st_mtim.tv_nsec);
#else
    identityOut->modifiedTicks =
        static_cast<std::uint64_t>(information.st_mtime) * 1000000000ull;
#endif
    return Status::Ok();
}

Status PositionalRead(NativeHandle handle,
                      void* dst,
                      std::size_t size,
                      std::uint64_t offset,
                      std::size_t* readOut) {
    *readOut = 0;

    ssize_t delivered = 0;
    do {
        delivered = ::pread(handle, dst, size, static_cast<off_t>(offset));
    } while (delivered < 0 && errno == EINTR);

    if (delivered < 0) {
        return Status::Error(StatusCode::NetworkError, "filesystem error reading asset")
            .WithRange(offset, size)
            .WithTransportStatus(errno);
    }

    *readOut = static_cast<std::size_t>(delivered);
    return Status::Ok();
}

#endif

}  // namespace detail
}  // namespace local
}  // namespace usdasset
