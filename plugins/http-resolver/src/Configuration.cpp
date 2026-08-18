// SPDX-License-Identifier: Apache-2.0

#include "Configuration.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace usdhttpresolver {
namespace {

constexpr const char* kConnectTimeout = "USD_HTTP_RESOLVER_CONNECT_TIMEOUT_MS";
constexpr const char* kReadTimeout = "USD_HTTP_RESOLVER_READ_TIMEOUT_MS";
constexpr const char* kTotalTimeout = "USD_HTTP_RESOLVER_TOTAL_TIMEOUT_MS";
constexpr const char* kMaxRetries = "USD_HTTP_RESOLVER_MAX_RETRIES";
constexpr const char* kMaxRedirects = "USD_HTTP_RESOLVER_MAX_REDIRECTS";

/// Parses a non-negative integer with no leading sign, no whitespace, and no
/// trailing text.
///
/// Strict on purpose. `strtol` would read `30s` as 30 and `1e6` as 1, and a
/// deadline that is a millionth of what an operator wrote is a configuration
/// error that presents as a network fault.
bool ParseCount(const std::string& text, long long min, long long max,
                long long* out, std::string* reasonOut) {
    if (text.empty()) {
        *reasonOut = "empty";
        return false;
    }
    long long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            *reasonOut = "not a non-negative integer";
            return false;
        }
        value = value * 10 + (c - '0');
        if (value > max) {
            *reasonOut = "out of range [" + std::to_string(min) + ", " +
                         std::to_string(max) + "]";
            return false;
        }
    }
    if (value < min) {
        *reasonOut = "out of range [" + std::to_string(min) + ", " +
                     std::to_string(max) + "]";
        return false;
    }
    *out = value;
    return true;
}

/// `getenv`, and the pragma MSVC needs to allow it.
///
/// `_dupenv_s` exists on one toolchain and allocates; this runs once, at
/// resolver construction, and reaching for the "safe" variant would put a
/// platform branch in the one function here that has no platform behaviour.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
const char* ReadEnvironment(const char* name) { return std::getenv(name); }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

void ReadInto(const EnvironmentLookup& lookup, const char* name, long long min,
              long long max, int* target,
              std::vector<ConfigurationProblem>* problemsOut) {
    std::string text;
    if (!lookup(name, &text)) return;

    long long value = 0;
    std::string reason;
    if (!ParseCount(text, min, max, &value, &reason)) {
        if (problemsOut) {
            problemsOut->push_back({name, text, reason});
        }
        return;
    }
    *target = static_cast<int>(value);
}

}  // namespace

usdasset::http::HttpOptions OptionsFrom(
    const EnvironmentLookup& lookup,
    std::vector<ConfigurationProblem>* problemsOut) {
    usdasset::http::HttpOptions options;

    // A deadline of zero is "no deadline" to most transports, which is the one
    // value this project must not accept: §10 of the design policy exists to
    // bound the pathological case, and an unbounded read is the pathological
    // case. The ceiling is an hour, which is not a service level -- it is the
    // point past which a configured value is more likely a typo than a
    // deliberate patience.
    ReadInto(lookup, kConnectTimeout, 1, 3600000, &options.connectTimeoutMs,
             problemsOut);
    ReadInto(lookup, kReadTimeout, 1, 3600000, &options.responseTimeoutMs,
             problemsOut);
    ReadInto(lookup, kTotalTimeout, 1, 3600000, &options.transferTimeoutMs,
             problemsOut);

    // Retries, not attempts. The variable an operator sets is "how many times
    // may this be tried again", and the backend counts total attempts; 0 is a
    // legal value and means "do not retry".
    int retries = options.maxAttempts - 1;
    ReadInto(lookup, kMaxRetries, 0, 100, &retries, problemsOut);
    options.maxAttempts = retries + 1;

    // Zero redirects is legal and means "refuse to follow any".
    ReadInto(lookup, kMaxRedirects, 0, 100, &options.maxRedirects, problemsOut);

    return options;
}

usdasset::http::HttpOptions OptionsFromEnvironment(
    std::vector<ConfigurationProblem>* problemsOut) {
    const EnvironmentLookup lookup = [](const char* name, std::string* valueOut) {
        const char* value = ReadEnvironment(name);
        if (value == nullptr) return false;
        valueOut->assign(value);
        return true;
    };
    return OptionsFrom(lookup, problemsOut);
}

const std::vector<const char*>& ConfiguredVariables() {
    static const std::vector<const char*> variables = {
        kConnectTimeout, kReadTimeout, kTotalTimeout, kMaxRetries,
        kMaxRedirects};
    return variables;
}

}  // namespace usdhttpresolver
