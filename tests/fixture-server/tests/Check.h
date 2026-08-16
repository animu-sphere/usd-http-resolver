// SPDX-License-Identifier: Apache-2.0
//
// The same twenty-line check macro the module suites use, with one addition:
// a labelled variant, because every case here runs against a named behavior and
// "FAIL Server.cpp:412" without the behavior's name is a bisect rather than a
// diagnosis.
//
// Still deliberately not a test framework. This repository takes exactly one
// third-party dependency, chosen on stated criteria in ADR-0003; a framework
// acquired for the tests would be a second one, admitted without an argument.

#ifndef USDASSETFIXTURE_TESTS_CHECK_H
#define USDASSETFIXTURE_TESTS_CHECK_H

#include <cstdio>

namespace usdassetfixturetest {

inline int& FailureCount() {
    static int failures = 0;
    return failures;
}

inline const char*& CurrentCase() {
    static const char* label = "";
    return label;
}

/// Names the behavior every subsequent CHECK belongs to, until the next one.
struct CaseScope {
    explicit CaseScope(const char* label) { CurrentCase() = label; }
    ~CaseScope() { CurrentCase() = ""; }
};

inline int Report(const char* suite) {
    const int failures = FailureCount();
    if (failures == 0) {
        std::printf("%s: ok\n", suite);
        return 0;
    }
    std::printf("%s: %d failure(s)\n", suite, failures);
    return 1;
}

}  // namespace usdassetfixturetest

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n",                    \
                         ::usdassetfixturetest::CurrentCase(), __FILE__,     \
                         __LINE__, #expr);                                   \
            ++::usdassetfixturetest::FailureCount();                         \
        }                                                                    \
    } while (false)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        const auto _actual = (actual);                                       \
        const auto _expected = (expected);                                   \
        if (!(_actual == _expected)) {                                        \
            std::fprintf(stderr, "FAIL [%s] %s:%d: %s == %s\n",              \
                         ::usdassetfixturetest::CurrentCase(), __FILE__,     \
                         __LINE__, #actual, #expected);                      \
            ++::usdassetfixturetest::FailureCount();                         \
        }                                                                    \
    } while (false)

/// For a status code, where the actual value is what a reader needs to see.
#define CHECK_STATUS(response, expected)                                     \
    do {                                                                     \
        const auto& _response = (response);                                  \
        if (_response.status != (expected)) {                                 \
            std::fprintf(stderr,                                             \
                         "FAIL [%s] %s:%d: status %d, expected %d (end=%s)\n", \
                         ::usdassetfixturetest::CurrentCase(), __FILE__,     \
                         __LINE__, _response.status, (expected),             \
                         ::usdassetfixturetest::ResponseEndName(_response.end)); \
            ++::usdassetfixturetest::FailureCount();                         \
        }                                                                    \
    } while (false)

#endif  // USDASSETFIXTURE_TESTS_CHECK_H
