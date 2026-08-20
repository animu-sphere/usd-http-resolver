// SPDX-License-Identifier: Apache-2.0
//
// The environment-variable surface, CONFIGURATION.md §2.
//
// Against a lookup function rather than against `getenv`, which is what makes
// the interesting cases reachable at all: an `ArResolver` is constructed once
// per process, so a test that set variables and hoped about ordering would
// assert one row and skip the rest.

#include <map>
#include <string>
#include <vector>

#include "Check.h"
#include "Configuration.h"

namespace {

using usdhttpresolver::ConfigurationProblem;
using usdhttpresolver::OptionsFrom;

usdhttpresolver::EnvironmentLookup From(
    const std::map<std::string, std::string>& environment) {
    return [environment](const char* name, std::string* valueOut) {
        const auto found = environment.find(name);
        if (found == environment.end()) return false;
        valueOut->assign(found->second);
        return true;
    };
}

/// Nothing set is the shipped default, and the shipped default is what the
/// release is measured with.
void TestDefaults() {
    std::vector<ConfigurationProblem> problems;
    const usdasset::http::HttpOptions options =
        OptionsFrom(From({}), &problems);
    const usdasset::http::HttpOptions defaults;

    CHECK(problems.empty());
    CHECK_EQ(options.connectTimeoutMs, defaults.connectTimeoutMs);
    CHECK_EQ(options.responseTimeoutMs, defaults.responseTimeoutMs);
    CHECK_EQ(options.transferTimeoutMs, defaults.transferTimeoutMs);
    CHECK_EQ(options.maxAttempts, defaults.maxAttempts);
    CHECK_EQ(options.maxRedirects, defaults.maxRedirects);
}

void TestEachVariable() {
    std::vector<ConfigurationProblem> problems;
    const usdasset::http::HttpOptions options = OptionsFrom(
        From({{"USD_HTTP_RESOLVER_CONNECT_TIMEOUT_MS", "1500"},
              {"USD_HTTP_RESOLVER_READ_TIMEOUT_MS", "2500"},
              {"USD_HTTP_RESOLVER_TOTAL_TIMEOUT_MS", "45000"},
              {"USD_HTTP_RESOLVER_MAX_RETRIES", "5"},
              {"USD_HTTP_RESOLVER_MAX_REDIRECTS", "2"}}),
        &problems);

    CHECK(problems.empty());
    CHECK_EQ(options.connectTimeoutMs, 1500);
    CHECK_EQ(options.responseTimeoutMs, 2500);
    CHECK_EQ(options.transferTimeoutMs, 45000);
    // Retries, not attempts: what an operator sets is how many times a request
    // may be tried *again*.
    CHECK_EQ(options.maxAttempts, 6);
    CHECK_EQ(options.maxRedirects, 2);

    // Zero is a legal value for both counters and means "do not".
    std::vector<ConfigurationProblem> none;
    const usdasset::http::HttpOptions off =
        OptionsFrom(From({{"USD_HTTP_RESOLVER_MAX_RETRIES", "0"},
                          {"USD_HTTP_RESOLVER_MAX_REDIRECTS", "0"}}),
                    &none);
    CHECK(none.empty());
    CHECK_EQ(off.maxAttempts, 1);
    CHECK_EQ(off.maxRedirects, 0);
}

/// "An unparseable value is a diagnostic at first use, not a silent fallback."
/// The default is still used -- refusing to resolve anything because one
/// variable has a typo would be worse than the typo -- and it is used loudly.
void TestRejectedValues() {
    const std::vector<std::string> bad = {
        "30s",      // a unit, which would read as 30
        "1e6",      // scientific, which would read as 1
        "-1",       // negative
        " 30",      // padded
        "",         // set and empty
        "0",        // no deadline at all, which is the one value §10 forbids
        "99999999", // beyond the hour ceiling
        "abc",
    };
    for (const std::string& value : bad) {
        std::vector<ConfigurationProblem> problems;
        const usdasset::http::HttpOptions options = OptionsFrom(
            From({{"USD_HTTP_RESOLVER_CONNECT_TIMEOUT_MS", value}}), &problems);
        if (problems.size() != 1) {
            std::fprintf(stderr, "FAIL %s:%d: '%s' produced %zu problem(s)\n",
                         __FILE__, __LINE__, value.c_str(), problems.size());
            ++::usdassettest::FailureCount();
            continue;
        }
        CHECK(problems[0].variable == "USD_HTTP_RESOLVER_CONNECT_TIMEOUT_MS");
        CHECK(problems[0].value == value);
        CHECK(!problems[0].reason.empty());
        CHECK_EQ(options.connectTimeoutMs,
                 usdasset::http::HttpOptions().connectTimeoutMs);
    }
}

/// One bad value does not discard the other four. A configuration that is
/// mostly right stays mostly in force.
void TestIndependence() {
    std::vector<ConfigurationProblem> problems;
    const usdasset::http::HttpOptions options = OptionsFrom(
        From({{"USD_HTTP_RESOLVER_CONNECT_TIMEOUT_MS", "nonsense"},
              {"USD_HTTP_RESOLVER_MAX_REDIRECTS", "1"}}),
        &problems);

    CHECK_EQ(problems.size(), std::size_t{1});
    CHECK_EQ(options.maxRedirects, 1);
    CHECK_EQ(options.connectTimeoutMs,
             usdasset::http::HttpOptions().connectTimeoutMs);
}

/// The set this version reads. Asserted rather than restated, so that adding a
/// variable to the code without adding it to CONFIGURATION.md is visible.
void TestVariableSet() {
    const std::vector<const char*>& variables =
        usdhttpresolver::ConfiguredVariables();
    // Five transport bounds from `v0.2.0` and four cache variables from
    // `v0.3.0`, which is the whole of CONFIGURATION.md §2 except the metrics
    // dump -- that one is read by usdAssetIo and not by this resolver.
    CHECK_EQ(variables.size(), std::size_t{9});
    for (const char* name : variables) {
        CHECK(std::string(name).rfind("USD_HTTP_RESOLVER_", 0) == 0);
        std::vector<ConfigurationProblem> problems;
        usdhttpresolver::ConfigurationFrom(From({{name, "not a number"}}),
                                           &problems);
        // Every variable this version claims to read is actually read.
        CHECK_EQ(problems.size(), std::size_t{1});
    }
}

/// The cache variables, one at a time, and the two adjustments that are
/// reported rather than made silently.
void TestCacheVariables() {
    std::vector<ConfigurationProblem> problems;
    const usdasset::cache::CacheOptions defaults =
        usdhttpresolver::CacheOptionsFrom(From({}), &problems);
    CHECK_EQ(problems.size(), std::size_t{0});
    CHECK_EQ(defaults.blockSize, usdasset::cache::kDefaultBlockSize);
    CHECK_EQ(defaults.budgetBytes, usdasset::cache::kDefaultBudgetBytes);

    problems.clear();
    const usdasset::cache::CacheOptions set = usdhttpresolver::CacheOptionsFrom(
        From({{"USD_HTTP_RESOLVER_BLOCK_SIZE", "16384"},
              {"USD_HTTP_RESOLVER_CACHE_BUDGET", "1048576"},
              {"USD_HTTP_RESOLVER_COALESCE_GAP", "0"},
              {"USD_HTTP_RESOLVER_MAX_REQUEST_BYTES", "65536"}}),
        &problems);
    CHECK_EQ(problems.size(), std::size_t{0});
    CHECK_EQ(set.blockSize, std::uint64_t{16384});
    CHECK_EQ(set.budgetBytes, std::uint64_t{1048576});
    CHECK_EQ(set.coalesceGapBlocks, std::uint32_t{0});
    CHECK_EQ(set.maxRequestBytes, std::uint64_t{65536});

    // A block size that is not a power of two is rounded down, and the rounding
    // is a diagnostic: an operator who set 100000 and got 65536 should learn it
    // from a log rather than from a byte count.
    problems.clear();
    const usdasset::cache::CacheOptions rounded = usdhttpresolver::CacheOptionsFrom(
        From({{"USD_HTTP_RESOLVER_BLOCK_SIZE", "100000"}}), &problems);
    CHECK_EQ(problems.size(), std::size_t{1});
    CHECK_EQ(rounded.Normalized().blockSize, std::uint64_t{65536});

    // Below the floor and above the ceiling are refused, not clamped.
    problems.clear();
    usdhttpresolver::CacheOptionsFrom(
        From({{"USD_HTTP_RESOLVER_BLOCK_SIZE", "512"}}), &problems);
    CHECK_EQ(problems.size(), std::size_t{1});

    problems.clear();
    usdhttpresolver::CacheOptionsFrom(
        From({{"USD_HTTP_RESOLVER_CACHE_BUDGET", "12"}}), &problems);
    CHECK_EQ(problems.size(), std::size_t{1});

    // And one bad cache value does not discard the transport configuration, or
    // the other three cache values.
    problems.clear();
    const usdhttpresolver::ResolverConfiguration mixed =
        usdhttpresolver::ConfigurationFrom(
            From({{"USD_HTTP_RESOLVER_BLOCK_SIZE", "nonsense"},
                  {"USD_HTTP_RESOLVER_CACHE_BUDGET", "1048576"},
                  {"USD_HTTP_RESOLVER_MAX_REDIRECTS", "1"}}),
            &problems);
    CHECK_EQ(problems.size(), std::size_t{1});
    CHECK_EQ(mixed.cache.blockSize, usdasset::cache::kDefaultBlockSize);
    CHECK_EQ(mixed.cache.budgetBytes, std::uint64_t{1048576});
    CHECK_EQ(mixed.transport.maxRedirects, 1);
}

}  // namespace

int main() {
    TestDefaults();
    TestEachVariable();
    TestRejectedValues();
    TestIndependence();
    TestVariableSet();
    TestCacheVariables();
    return usdassettest::Report("httpResolver configuration");
}
