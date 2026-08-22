// SPDX-License-Identifier: Apache-2.0

#include "Configuration.h"

#include <cstdint>
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

constexpr const char* kBlockSize = "USD_HTTP_RESOLVER_BLOCK_SIZE";
constexpr const char* kCacheBudget = "USD_HTTP_RESOLVER_CACHE_BUDGET";
constexpr const char* kCoalesceGap = "USD_HTTP_RESOLVER_COALESCE_GAP";
constexpr const char* kMaxRequestBytes = "USD_HTTP_RESOLVER_MAX_REQUEST_BYTES";

constexpr const char* kPersistentDirectory = "USD_HTTP_RESOLVER_PERSISTENT_CACHE_DIR";
constexpr const char* kPersistentBudget = "USD_HTTP_RESOLVER_PERSISTENT_CACHE_BUDGET";

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

/// The 64-bit form of `ReadInto`, for the variables that are byte counts.
///
/// A block size and a budget do not fit in the `int` the transport bounds are,
/// and a budget that silently wrapped at two gigabytes would be a cache that
/// held nothing on the machines large enough to want one.
void ReadBytesInto(const EnvironmentLookup& lookup, const char* name,
                   long long min, long long max, std::uint64_t* target,
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
    *target = static_cast<std::uint64_t>(value);
}

}  // namespace

usdasset::cache::DiskCacheOptions PersistenceOptionsFrom(
    const EnvironmentLookup& lookup,
    std::vector<ConfigurationProblem>* problemsOut) {
    usdasset::cache::DiskCacheOptions options;

    std::string directory;
    if (lookup(kPersistentDirectory, &directory)) {
        if (directory.empty()) {
            // A set-but-empty variable is a problem and not an absence. An
            // operator who wrote `export ...CACHE_DIR=` meant to turn something
            // on, and leaving persistence off without saying so would make the
            // typo indistinguishable from the feature not existing.
            //
            // The default this falls back to is no persistent cache, which is
            // what makes the reporter's "using the default" true here: unset is
            // off, and there is no location to fall back to.
            if (problemsOut != nullptr) {
                problemsOut->push_back({kPersistentDirectory, directory, "empty"});
            }
        } else {
            options.directory = directory;
        }
    }

    // The floor is one block of the smallest size this cache will ever use;
    // below that the tier would evict what it just wrote. The ceiling is a
    // terabyte, which is not a recommendation -- it is the point past which the
    // value is more likely a unit error than a disk.
    ReadBytesInto(lookup, kPersistentBudget,
                  static_cast<long long>(usdasset::cache::kMinPersistentBudgetBytes),
                  1024LL * 1024 * 1024 * 1024, &options.budgetBytes, problemsOut);

    return options;
}

usdasset::cache::CacheOptions CacheOptionsFrom(
    const EnvironmentLookup& lookup,
    std::vector<ConfigurationProblem>* problemsOut) {
    usdasset::cache::CacheOptions options;

    // The bounds are the module's own, so that a value this function accepts is
    // a value the cache can use. Below the floor the per-block bookkeeping costs
    // more than the block; above the ceiling one miss transfers more than most
    // assets are worth.
    ReadBytesInto(lookup, kBlockSize,
                  static_cast<long long>(usdasset::cache::kMinBlockSize),
                  static_cast<long long>(usdasset::cache::kMaxBlockSize),
                  &options.blockSize, problemsOut);

    // The floor here is the smallest block this module will ever use, not one
    // block of the size *this* configuration asked for -- the two differ
    // whenever the block size is raised, and the normalizer then lifts the
    // budget to one block. That lift is reported below rather than applied
    // quietly, on the same principle as the rounding: an operator who set a
    // number and got another one should learn it from a log.
    ReadBytesInto(lookup, kCacheBudget,
                  static_cast<long long>(usdasset::cache::kMinBlockSize),
                  64LL * 1024 * 1024 * 1024, &options.budgetBytes, problemsOut);

    std::uint64_t gap = options.coalesceGapBlocks;
    ReadBytesInto(lookup, kCoalesceGap, 0, 1024, &gap, problemsOut);
    options.coalesceGapBlocks = static_cast<std::uint32_t>(gap);

    ReadBytesInto(lookup, kMaxRequestBytes,
                  static_cast<long long>(usdasset::cache::kMinBlockSize),
                  4LL * 1024 * 1024 * 1024, &options.maxRequestBytes,
                  problemsOut);

    // CONFIGURATION.md §2 says the block size is "rounded to a power of two",
    // and rounding is an adjustment the operator did not ask for. Reported, so
    // that a deployment that set 100000 and got 65536 can find out from a log
    // rather than from a byte count.
    const usdasset::cache::CacheOptions normalized = options.Normalized();
    if (problemsOut != nullptr && normalized.blockSize != options.blockSize) {
        problemsOut->push_back({kBlockSize, std::to_string(options.blockSize),
                                "rounded down to the power of two " +
                                    std::to_string(normalized.blockSize)});
    }
    if (problemsOut != nullptr &&
        normalized.coalesceGapBlocks != options.coalesceGapBlocks) {
        problemsOut->push_back(
            {kCoalesceGap, std::to_string(options.coalesceGapBlocks),
             "capped at " + std::to_string(normalized.coalesceGapBlocks) +
                 ", the widest gap that can fit under "
                 "USD_HTTP_RESOLVER_MAX_REQUEST_BYTES"});
    }
    if (problemsOut != nullptr && normalized.budgetBytes != options.budgetBytes) {
        problemsOut->push_back(
            {kCacheBudget, std::to_string(options.budgetBytes),
             "raised to " + std::to_string(normalized.budgetBytes) +
                 ", one block: a budget that cannot hold a block does not "
                 "cache nothing, it fetches a block and drops it"});
    }
    if (problemsOut != nullptr && normalized.maxRequestBytes != options.maxRequestBytes) {
        problemsOut->push_back(
            {kMaxRequestBytes, std::to_string(options.maxRequestBytes),
             "raised to " + std::to_string(normalized.maxRequestBytes) +
                 ", one block: a merged request that cannot carry a block "
                 "cannot carry the block it was merging"});
    }

    return options;
}

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

ResolverConfiguration ConfigurationFrom(
    const EnvironmentLookup& lookup,
    std::vector<ConfigurationProblem>* problemsOut) {
    ResolverConfiguration configuration;
    configuration.transport = OptionsFrom(lookup, problemsOut);
    configuration.cache = CacheOptionsFrom(lookup, problemsOut);
    configuration.persistence = PersistenceOptionsFrom(lookup, problemsOut);
    return configuration;
}

ResolverConfiguration ConfigurationFromEnvironment(
    std::vector<ConfigurationProblem>* problemsOut) {
    const EnvironmentLookup lookup = [](const char* name, std::string* valueOut) {
        const char* value = ReadEnvironment(name);
        if (value == nullptr) return false;
        valueOut->assign(value);
        return true;
    };
    return ConfigurationFrom(lookup, problemsOut);
}

const std::vector<const char*>& ConfiguredVariables() {
    static const std::vector<const char*> variables = {
        kBlockSize,
        kCacheBudget,
        kCoalesceGap,
        kMaxRequestBytes,
        kPersistentDirectory,
        kPersistentBudget,
        kConnectTimeout,
        kReadTimeout,
        kTotalTimeout,
        kMaxRetries,
        kMaxRedirects};
    return variables;
}

}  // namespace usdhttpresolver
