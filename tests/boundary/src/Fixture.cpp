// SPDX-License-Identifier: Apache-2.0

#include "usdassetboundary/Fixture.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace usdassetboundary {

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
        ("usd-http-resolver-boundary-" + tag);
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
