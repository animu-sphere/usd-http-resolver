// SPDX-License-Identifier: Apache-2.0

#include "usdassetboundary/Fixture.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace usdassetboundary {
namespace {

/// The owning process, spelled the way each platform spells it.
///
/// It is part of the workspace path rather than a random suffix so that a
/// directory left behind by a crashed run can be attributed to the run that
/// left it.
unsigned long CurrentProcessId() {
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

}  // namespace

std::vector<unsigned char> PositionalContent(std::size_t size, std::uint64_t seed) {
    std::vector<unsigned char> content(size);
    std::uint64_t state = seed;
    for (std::size_t i = 0; i < size; ++i) {
        state += 0x9E3779B97F4A7C15ull;
        std::uint64_t mixed = state;
        mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
        mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
        mixed = mixed ^ (mixed >> 31);
        content[i] = static_cast<unsigned char>(mixed & 0xFF);
    }
    return content;
}

const std::vector<FixtureSize>& RequiredFixtureSizes() {
    static const std::vector<FixtureSize> sizes = {
        {"empty", 0},
        {"one-byte", 1},
        {"below-block", kNominalBlockSize - 1},
        {"exactly-one-block", kNominalBlockSize},
        // Several blocks with a short final block: the shape almost every real
        // asset has, and the one where an off-by-one at the tail hides.
        {"several-blocks", kNominalBlockSize * 3 + 7},
    };
    return sizes;
}

FixtureWorkspace::FixtureWorkspace(const std::string& tag) {
    std::error_code error;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(error) /
        ("usd-http-resolver-boundary-" + tag + "-" + std::to_string(CurrentProcessId()));
    // Of this process's own directory, which a previous run with the same
    // process id may have left behind. The process id is what makes that safe:
    // without it the tag is shared, and this line deletes the fixtures of
    // whichever concurrent run of the same row started first.
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    if (!error) {
        _path = root.string();
    }
}

FixtureWorkspace::~FixtureWorkspace() {
    if (_path.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(_path, error);
}

bool FixtureWorkspace::Valid() const { return !_path.empty(); }

std::string FixtureWorkspace::Write(const std::string& name,
                                    const std::vector<unsigned char>& content) {
    const std::string path = (std::filesystem::path(_path) / name).string();
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return std::string();
    }
    // A fixture that was not fully written would make every case over it fail
    // as though the backend were wrong.
    const bool written =
        content.empty() ||
        std::fwrite(content.data(), 1, content.size(), file) == content.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        return std::string();
    }
    return path;
}

bool RepublishFile(const std::string& path, const std::vector<unsigned char>& content) {
    std::error_code error;
    const std::filesystem::file_time_type before =
        std::filesystem::last_write_time(path, error);

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    // A short or failed write here would otherwise surface as the backend
    // failing to report AssetChanged, which is a fixture problem wearing a
    // defect's clothes.
    const bool written =
        content.empty() ||
        std::fwrite(content.data(), 1, content.size(), file) == content.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        return false;
    }

    if (!error) {
        std::filesystem::last_write_time(path, before + std::chrono::seconds(10),
                                         error);
        if (error) {
            return false;
        }
    }
    return true;
}

}  // namespace usdassetboundary
