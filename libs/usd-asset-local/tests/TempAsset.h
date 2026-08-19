// SPDX-License-Identifier: Apache-2.0
//
// A file on disk with a known content, removed when the test ends.

#ifndef USDASSETLOCAL_TESTS_TEMPASSET_H
#define USDASSETLOCAL_TESTS_TEMPASSET_H

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace usdassettest {

/// The owning process, spelled the way each platform spells it.
inline unsigned long CurrentProcessId() {
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

/// A directory under the platform temporary directory, removed on destruction.
///
/// Named for the owning process as well as the tag. The tag and a per-process
/// counter are not unique across processes: two runs of the same test -- an
/// ASan build and a TSan build on one machine, or `ctest -j` over two
/// configurations -- would share one path, and the constructor's cleanup would
/// delete the other run's asset out from under an open reader.
class TempDirectory {
public:
    explicit TempDirectory(const std::string& tag) {
        static int counter = 0;
        std::error_code error;
        _path = std::filesystem::temp_directory_path(error) /
                ("usd-http-resolver-" + tag + "-" +
                 std::to_string(CurrentProcessId()) + "-" +
                 std::to_string(++counter));
        std::filesystem::remove_all(_path, error);
        std::filesystem::create_directories(_path, error);
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(_path, error);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    std::string File(const std::string& name) const {
        return (_path / name).string();
    }

    /// The directory itself, with no trailing separator: a path that names a
    /// directory rather than one that merely lives in it.
    std::string Path() const { return _path.string(); }

private:
    std::filesystem::path _path;
};

/// Content that makes a wrong offset visible: every byte is a function of its
/// position, so a read that lands one byte off returns data that is obviously
/// from somewhere else rather than plausibly correct.
inline std::vector<unsigned char> PositionalContent(std::size_t size,
                                                    std::uint64_t seed = 0) {
    std::vector<unsigned char> content(size);
    std::uint64_t state = seed + 0x9E3779B97F4A7C15ull;
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

inline bool WriteFile(const std::string& path,
                      const std::vector<unsigned char>& content) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const bool ok = content.empty() ||
                    std::fwrite(content.data(), 1, content.size(), file) ==
                        content.size();
    std::fclose(file);
    return ok;
}

}  // namespace usdassettest

#endif  // USDASSETLOCAL_TESTS_TEMPASSET_H
