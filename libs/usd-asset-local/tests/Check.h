// SPDX-License-Identifier: Apache-2.0
//
// A twenty-line check macro, deliberately not a test framework.
//
// This repository takes exactly one third-party dependency -- the HTTP client
// chosen in v0.2.0 on license, footprint, and Wasm viability. A test framework
// pulled in ahead of it would be a second one, acquired without that argument
// ever being made. `CHECK` reports every failure rather than aborting on the
// first, which is what makes a boundary table readable when several rows break
// at once.

#ifndef USDASSETLOCAL_TESTS_CHECK_H
#define USDASSETLOCAL_TESTS_CHECK_H

#include <cstdio>

namespace usdassettest {

inline int& FailureCount() {
    static int failures = 0;
    return failures;
}

inline int Report(const char* suite) {
    const int failures = FailureCount();
    if (failures == 0) {
        std::printf("%s: ok\n", suite);
        return 0;
    }
    std::printf("%s: %d failure(s)\n", suite, failures);
    return 1;
}

}  // namespace usdassettest

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++::usdassettest::FailureCount();                                    \
        }                                                                        \
    } while (false)

#define CHECK_EQ(actual, expected)                                             \
    do {                                                                       \
        const auto _actual = (actual);                                         \
        const auto _expected = (expected);                                     \
        if (!(_actual == _expected)) {                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, \
                         #actual, #expected);                                  \
            ++::usdassettest::FailureCount();                                  \
        }                                                                      \
    } while (false)

#endif  // USDASSETLOCAL_TESTS_CHECK_H
