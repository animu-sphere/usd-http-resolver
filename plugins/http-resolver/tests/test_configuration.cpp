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
    CHECK(options.destinations == defaults.destinations);
}

/// The destination policy of §10.2, as a set of address class names.
void TestDestinations() {
    using usdasset::http::DestinationPolicy;

    struct Row {
        const char* value;
        bool publicAddresses;
        bool privateNetworks;
        bool loopback;
        bool linkLocal;
    };
    const Row accepted[] = {
        {"public", true, false, false, false},
        {"private,loopback", false, true, true, false},
        // How a person writes a list.
        {"public, private , loopback", true, true, true, false},
        {"link-local", false, false, false, true},
        {"public,private,loopback,link-local", true, true, true, true},
        // Repetition is redundant, not contradictory.
        {"public,public", true, false, false, false},
    };
    for (const Row& row : accepted) {
        std::vector<ConfigurationProblem> problems;
        const usdasset::http::HttpOptions options = OptionsFrom(
            From({{"USD_HTTP_RESOLVER_DESTINATIONS", row.value}}), &problems);
        if (!problems.empty()) {
            std::fprintf(stderr, "FAIL %s:%d: '%s' was refused: %s\n", __FILE__,
                         __LINE__, row.value, problems[0].reason.c_str());
            ++::usdassettest::FailureCount();
            continue;
        }
        CHECK_EQ(options.destinations.publicAddresses, row.publicAddresses);
        CHECK_EQ(options.destinations.privateNetworks, row.privateNetworks);
        CHECK_EQ(options.destinations.loopback, row.loopback);
        CHECK_EQ(options.destinations.linkLocal, row.linkLocal);
    }

    // Refused whole, and the default kept, loudly. An unknown name is not
    // skipped: a policy that dropped the word it did not recognize is a
    // different policy from the one that was written.
    const char* const refused[] = {
        "",
        "public,,private",
        "public,",
        "Public",
        "everything",
        "public,internet",
        "none",
        "127.0.0.1",
    };
    for (const char* value : refused) {
        std::vector<ConfigurationProblem> problems;
        const usdasset::http::HttpOptions options = OptionsFrom(
            From({{"USD_HTTP_RESOLVER_DESTINATIONS", value}}), &problems);
        if (problems.size() != 1) {
            std::fprintf(stderr, "FAIL %s:%d: '%s' produced %zu problem(s)\n",
                         __FILE__, __LINE__, value, problems.size());
            ++::usdassettest::FailureCount();
            continue;
        }
        CHECK(problems[0].variable == "USD_HTTP_RESOLVER_DESTINATIONS");
        CHECK(options.destinations == DestinationPolicy());
    }
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
    // Five transport bounds from `v0.2.0`, four cache variables from `v0.3.0`,
    // two persistence variables from `v0.4.0`, and the destination policy from
    // `v0.7.0`, which is the whole of CONFIGURATION.md §2 except the metrics
    // dump -- that one is read by usdAssetIo and not by this resolver.
    CHECK_EQ(variables.size(), std::size_t{12});
    for (const char* name : variables) {
        CHECK(std::string(name).rfind("USD_HTTP_RESOLVER_", 0) == 0);
        // Every variable is a byte count or a bound except the cache directory,
        // whose values are paths and for which the unusable value is the empty
        // one. Asserting `not a number` against it would assert that a path
        // cannot contain a space.
        const bool isPath =
            std::string(name) == "USD_HTTP_RESOLVER_PERSISTENT_CACHE_DIR";
        std::vector<ConfigurationProblem> problems;
        usdhttpresolver::ConfigurationFrom(
            From({{name, isPath ? std::string() : std::string("not a number")}}),
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

/// The persistence variables of CACHE.md §8, which are the only two in this
/// surface whose default is "off" rather than a number.
void TestPersistenceVariables() {
    std::vector<ConfigurationProblem> problems;
    const usdasset::cache::DiskCacheOptions unset =
        usdhttpresolver::PersistenceOptionsFrom(From({}), &problems);
    CHECK(problems.empty());
    // Off, and off is an empty directory rather than a flag: a second way to
    // say the same thing is a second thing to get wrong.
    CHECK(unset.directory.empty());
    CHECK_EQ(unset.budgetBytes, usdasset::cache::kDefaultPersistentBudgetBytes);

    problems.clear();
    const usdasset::cache::DiskCacheOptions set =
        usdhttpresolver::PersistenceOptionsFrom(
            From({{"USD_HTTP_RESOLVER_PERSISTENT_CACHE_DIR", "/var/tmp/usd cache"},
                  {"USD_HTTP_RESOLVER_PERSISTENT_CACHE_BUDGET", "2097152"}}),
            &problems);
    CHECK(problems.empty());
    CHECK_EQ(set.directory, std::string("/var/tmp/usd cache"));
    CHECK_EQ(set.budgetBytes, std::uint64_t{2097152});

    // Set and empty is a typo, not an absence, and it is reported.
    problems.clear();
    const usdasset::cache::DiskCacheOptions blank =
        usdhttpresolver::PersistenceOptionsFrom(
            From({{"USD_HTTP_RESOLVER_PERSISTENT_CACHE_DIR", ""}}), &problems);
    CHECK_EQ(problems.size(), std::size_t{1});
    CHECK(blank.directory.empty());

    // A budget below the floor is refused rather than clamped, like every other
    // out-of-range value in this surface.
    problems.clear();
    usdhttpresolver::PersistenceOptionsFrom(
        From({{"USD_HTTP_RESOLVER_PERSISTENT_CACHE_BUDGET", "4096"}}), &problems);
    CHECK_EQ(problems.size(), std::size_t{1});

    // A budget with no directory is read, reported if bad, and then unused.
    // Persistence stays off because nothing named a directory, which is the
    // rule that keeps "on" a single decision.
    problems.clear();
    const usdhttpresolver::ResolverConfiguration budgetOnly =
        usdhttpresolver::ConfigurationFrom(
            From({{"USD_HTTP_RESOLVER_PERSISTENT_CACHE_BUDGET", "8388608"}}),
            &problems);
    CHECK(problems.empty());
    CHECK(budgetOnly.persistence.directory.empty());
    CHECK_EQ(budgetOnly.persistence.budgetBytes, std::uint64_t{8388608});
}

// --- the context form ----------------------------------------------------------

using usdhttpresolver::OverridesFrom;

/// What a context may set, and what it may not. The four it may not are the
/// ones the process shares -- the block store, its block size, and the
/// persistent tier -- and a stage that set them would be setting them for
/// every other stage too.
void TestContextVariableSet() {
    const std::vector<const char*>& configured = usdhttpresolver::ConfiguredVariables();
    const std::vector<const char*>& context = usdhttpresolver::ContextVariables();
    CHECK_EQ(context.size(), std::size_t{8});

    for (const char* name : context) {
        bool known = false;
        for (const char* candidate : configured) {
            if (std::string(name) == candidate) known = true;
        }
        CHECK(known);
    }
    const char* const processWide[] = {
        "USD_HTTP_RESOLVER_BLOCK_SIZE",
        "USD_HTTP_RESOLVER_CACHE_BUDGET",
        "USD_HTTP_RESOLVER_PERSISTENT_CACHE_DIR",
        "USD_HTTP_RESOLVER_PERSISTENT_CACHE_BUDGET",
    };
    for (const char* name : processWide) {
        for (const char* candidate : context) {
            CHECK(std::string(name) != candidate);
        }
    }
}

void TestContextStrings() {
    {
        // How a host writes one: across lines, with spaces, and with the
        // trailing separator concatenation leaves behind.
        std::vector<ConfigurationProblem> problems;
        const std::map<std::string, std::string> overrides = OverridesFrom(
            "  USD_HTTP_RESOLVER_MAX_RETRIES = 0 ;\n"
            "  USD_HTTP_RESOLVER_DESTINATIONS = public, private ;\n",
            &problems);
        CHECK(problems.empty());
        CHECK_EQ(overrides.size(), std::size_t{2});
        CHECK_EQ(overrides.at("USD_HTTP_RESOLVER_MAX_RETRIES"), std::string("0"));
        CHECK_EQ(overrides.at("USD_HTTP_RESOLVER_DESTINATIONS"),
                 std::string("public, private"));
        // Canonical: sorted by name, whatever order it was written in.
        CHECK_EQ(usdhttpresolver::CanonicalContextString(overrides),
                 std::string("USD_HTTP_RESOLVER_DESTINATIONS=public, private;"
                             "USD_HTTP_RESOLVER_MAX_RETRIES=0"));
    }
    {
        // Nothing is not a problem.
        std::vector<ConfigurationProblem> problems;
        CHECK(OverridesFrom("", &problems).empty());
        CHECK(OverridesFrom(" ; ;", &problems).empty());
        CHECK(problems.empty());
    }

    // Each of these is refused, reported as the context's, and absent from
    // what the context carries -- so that what it compares and hashes by is
    // what is in force.
    struct Refused {
        const char* text;
        const char* variable;
    };
    const Refused refused[] = {
        {"USD_HTTP_RESOLVER_BLOCK_SIZE=16384", "USD_HTTP_RESOLVER_BLOCK_SIZE"},
        {"USD_HTTP_RESOLVER_CACHE_BUDGET=1048576", "USD_HTTP_RESOLVER_CACHE_BUDGET"},
        {"USD_HTTP_RESOLVER_PERSISTENT_CACHE_DIR=/var/tmp/x",
         "USD_HTTP_RESOLVER_PERSISTENT_CACHE_DIR"},
        {"USD_HTTP_RESOLVER_PERSISTENT_CACHE_BUDGET=8388608",
         "USD_HTTP_RESOLVER_PERSISTENT_CACHE_BUDGET"},
        {"USD_HTTP_RESOLVER_METRICS_DUMP=1", "USD_HTTP_RESOLVER_METRICS_DUMP"},
        {"USD_HTTP_RESOLVER_NO_SUCH_THING=1", "USD_HTTP_RESOLVER_NO_SUCH_THING"},
        {"usd_http_resolver_max_retries=1", "usd_http_resolver_max_retries"},
        {"MAX_RETRIES=1", "MAX_RETRIES"},
        {"USD_HTTP_RESOLVER_MAX_RETRIES=abc", "USD_HTTP_RESOLVER_MAX_RETRIES"},
        {"USD_HTTP_RESOLVER_CONNECT_TIMEOUT_MS=0", "USD_HTTP_RESOLVER_CONNECT_TIMEOUT_MS"},
        {"USD_HTTP_RESOLVER_DESTINATIONS=everything", "USD_HTTP_RESOLVER_DESTINATIONS"},
        {"USD_HTTP_RESOLVER_DESTINATIONS=", "USD_HTTP_RESOLVER_DESTINATIONS"},
        {"=1", ""},
        {"USD_HTTP_RESOLVER_MAX_RETRIES", ""},
    };
    for (const Refused& row : refused) {
        std::vector<ConfigurationProblem> problems;
        const std::map<std::string, std::string> overrides =
            OverridesFrom(row.text, &problems);
        if (problems.size() != 1 || !overrides.empty()) {
            std::fprintf(stderr,
                         "FAIL %s:%d: '%s' gave %zu problem(s) and %zu override(s)\n",
                         __FILE__, __LINE__, row.text, problems.size(),
                         overrides.size());
            ++::usdassettest::FailureCount();
            continue;
        }
        CHECK(problems[0].fromContext);
        CHECK(!problems[0].adjusted);
        CHECK_EQ(problems[0].variable, std::string(row.variable));
    }

    {
        // One bad entry does not discard its neighbours.
        std::vector<ConfigurationProblem> problems;
        const std::map<std::string, std::string> overrides = OverridesFrom(
            "USD_HTTP_RESOLVER_MAX_REDIRECTS=nonsense;USD_HTTP_RESOLVER_MAX_RETRIES=1",
            &problems);
        CHECK_EQ(problems.size(), std::size_t{1});
        CHECK_EQ(overrides.size(), std::size_t{1});
        CHECK(overrides.count("USD_HTTP_RESOLVER_MAX_RETRIES") == 1);
    }
    {
        // Set twice: the last wins, as an environment assignment would, and
        // the repetition is reported as an adjustment rather than a refusal.
        std::vector<ConfigurationProblem> problems;
        const std::map<std::string, std::string> overrides = OverridesFrom(
            "USD_HTTP_RESOLVER_MAX_RETRIES=1; USD_HTTP_RESOLVER_MAX_RETRIES=2",
            &problems);
        CHECK_EQ(problems.size(), std::size_t{1});
        if (!problems.empty()) CHECK(problems[0].adjusted);
        CHECK_EQ(overrides.at("USD_HTTP_RESOLVER_MAX_RETRIES"), std::string("2"));
    }
    {
        // An adjusted value is kept and reported: a gap wider than a merged
        // request can carry is capped wherever it is applied.
        std::vector<ConfigurationProblem> problems;
        const std::map<std::string, std::string> overrides =
            OverridesFrom("USD_HTTP_RESOLVER_COALESCE_GAP=1024", &problems);
        CHECK_EQ(problems.size(), std::size_t{1});
        if (!problems.empty()) {
            CHECK(problems[0].adjusted);
            CHECK(problems[0].fromContext);
        }
        CHECK_EQ(overrides.size(), std::size_t{1});
    }
}

/// CONFIGURATION.md §4, as a function: context over environment over default,
/// one variable at a time.
void TestPrecedence() {
    const std::map<std::string, std::string> environment = {
        {"USD_HTTP_RESOLVER_MAX_RETRIES", "5"},
        {"USD_HTTP_RESOLVER_CONNECT_TIMEOUT_MS", "1500"},
    };
    const std::map<std::string, std::string> overrides = {
        {"USD_HTTP_RESOLVER_MAX_RETRIES", "0"},
        {"USD_HTTP_RESOLVER_DESTINATIONS", "public"},
    };

    std::vector<ConfigurationProblem> problems;
    const usdasset::http::HttpOptions layered = OptionsFrom(
        usdhttpresolver::Layered(overrides, usdhttpresolver::LookupIn(environment)),
        &problems);
    CHECK(problems.empty());
    CHECK_EQ(layered.maxAttempts, 1);             // the context's
    CHECK_EQ(layered.connectTimeoutMs, 1500);     // the environment's
    CHECK_EQ(layered.maxRedirects,                // the default
             usdasset::http::HttpOptions().maxRedirects);
    CHECK(layered.destinations.publicAddresses);
    CHECK(!layered.destinations.loopback);

    // A snapshot takes what the lookup has of the variables this version
    // reads, and nothing else.
    const std::map<std::string, std::string> snapshot = usdhttpresolver::Snapshot(
        From({{"USD_HTTP_RESOLVER_MAX_RETRIES", "3"}, {"PATH", "/usr/bin"}}));
    CHECK_EQ(snapshot.size(), std::size_t{1});
    CHECK_EQ(snapshot.at("USD_HTTP_RESOLVER_MAX_RETRIES"), std::string("3"));
}

/// The key a retained reader is handed out by. Equal exactly when a reader
/// opened under one configuration may serve a caller under the other.
void TestTransportFingerprint() {
    using usdhttpresolver::TransportFingerprint;
    const usdasset::http::HttpOptions defaults;
    CHECK_EQ(TransportFingerprint(defaults), TransportFingerprint(defaults));

    usdasset::http::HttpOptions narrower;
    narrower.destinations.loopback = false;
    CHECK(TransportFingerprint(narrower) != TransportFingerprint(defaults));

    usdasset::http::HttpOptions impatient;
    impatient.transferTimeoutMs = 1000;
    CHECK(TransportFingerprint(impatient) != TransportFingerprint(defaults));

    usdasset::http::HttpOptions persistent;
    persistent.maxAttempts = 1;
    CHECK(TransportFingerprint(persistent) != TransportFingerprint(defaults));
}

}  // namespace

int main() {
    TestDefaults();
    TestDestinations();
    TestContextVariableSet();
    TestContextStrings();
    TestPrecedence();
    TestTransportFingerprint();
    TestEachVariable();
    TestRejectedValues();
    TestIndependence();
    TestVariableSet();
    TestCacheVariables();
    TestPersistenceVariables();
    return usdassettest::Report("httpResolver configuration");
}
