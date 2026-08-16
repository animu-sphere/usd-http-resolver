// SPDX-License-Identifier: Apache-2.0

#include "usdassetboundary/Oracle.h"

#include <cstdio>
#include <limits>

namespace usdassetboundary {
namespace {

// Sixty-four-bit seek, spelled the way each platform spells it. This is the one
// piece of platform knowledge the oracle has, and it is here rather than shared
// with the backend for the reason stated in the header.
bool SeekTo(std::FILE* file, std::uint64_t offset) {
#if defined(_WIN32)
    return _fseeki64(file, static_cast<long long>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

bool SizeOf(std::FILE* file, std::uint64_t* sizeOut) {
#if defined(_WIN32)
    if (_fseeki64(file, 0, SEEK_END) != 0) {
        return false;
    }
    const long long end = _ftelli64(file);
#else
    if (fseeko(file, 0, SEEK_END) != 0) {
        return false;
    }
    const off_t end = ftello(file);
#endif
    if (end < 0) {
        return false;
    }
    *sizeOut = static_cast<std::uint64_t>(end);
    return true;
}

}  // namespace

OracleResult NaiveRead(const std::string& path, std::uint64_t offset, std::size_t size) {
    using usdasset::StatusCode;

    OracleResult result;

    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        result.code = StatusCode::NotFound;
        return result;
    }

    std::uint64_t fileSize = 0;
    if (!SizeOf(file, &fileSize)) {
        std::fclose(file);
        result.code = StatusCode::NetworkError;
        return result;
    }

    const std::uint64_t wanted = static_cast<std::uint64_t>(size);

    // Overflow first, as a subtraction. Deliberately written out again rather
    // than calling the shared helper.
    if (offset > (std::numeric_limits<std::uint64_t>::max)() - wanted) {
        std::fclose(file);
        result.code = StatusCode::InvalidArgument;
        return result;
    }

    if (wanted == 0 || offset >= fileSize) {
        std::fclose(file);
        result.code = StatusCode::Ok;
        return result;
    }

    const std::uint64_t remaining = fileSize - offset;
    const std::size_t available =
        static_cast<std::size_t>(remaining < wanted ? remaining : wanted);

    if (!SeekTo(file, offset)) {
        std::fclose(file);
        result.code = StatusCode::NetworkError;
        return result;
    }

    result.bytes.resize(available);
    const std::size_t delivered =
        std::fread(result.bytes.data(), 1, available, file);
    std::fclose(file);

    result.bytes.resize(delivered);
    result.bytesRead = delivered;
    // Truncation at the end of the file was already accounted for by clamping
    // to `available`. Anything short of that is a fault, not an end.
    result.code = delivered == available ? StatusCode::Ok : StatusCode::InvalidResponse;
    return result;
}

std::vector<unsigned char> NaiveReadAll(const std::string& path) {
    std::vector<unsigned char> content;

    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return content;
    }
    std::uint64_t fileSize = 0;
    if (!SizeOf(file, &fileSize) || !SeekTo(file, 0)) {
        std::fclose(file);
        return content;
    }
    content.resize(static_cast<std::size_t>(fileSize));
    if (fileSize > 0) {
        const std::size_t delivered =
            std::fread(content.data(), 1, content.size(), file);
        content.resize(delivered);
    }
    std::fclose(file);
    return content;
}

}  // namespace usdassetboundary
