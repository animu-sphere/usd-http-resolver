// SPDX-License-Identifier: Apache-2.0
//
// A twenty-line check macro, deliberately not a test framework. Same reasoning
// as every other Check.h in this tree: this repository takes exactly one
// third-party dependency, chosen on stated criteria in ADR-0003, and a test
// framework beside it would be a second one acquired without an argument.
//
// One divergence from the others, and it is the reason this copy is not a copy:
// the counter is atomic. The parallel-readers scenario checks from eight
// threads, and a plain `int` incremented from eight threads is a data race
// however unlikely a failure is -- and the day one happened, ThreadSanitizer
// would report the race rather than the failure, which buries the finding under
// its own symptom.

#ifndef USDASSETHTTP_BASELINE_CHECK_H
#define USDASSETHTTP_BASELINE_CHECK_H

#include <atomic>
#include <cstdio>

namespace usdassettest {

inline std::atomic<int>& FailureCount() {
    static std::atomic<int> failures{0};
    return failures;
}

inline int Report(const char* suite) {
    const int failures = FailureCount().load();
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
            std::fprintf(stderr, "FAIL %s:%d: %s == %s (%llu vs %llu)\n",      \
                         __FILE__, __LINE__, #actual, #expected,               \
                         static_cast<unsigned long long>(_actual),             \
                         static_cast<unsigned long long>(_expected));          \
            ++::usdassettest::FailureCount();                                  \
        }                                                                      \
    } while (false)

#endif  // USDASSETHTTP_BASELINE_CHECK_H
