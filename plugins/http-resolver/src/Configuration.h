// SPDX-License-Identifier: Apache-2.0
//
// The environment-variable configuration surface of CONFIGURATION.md §2: the
// five transport bounds, which arrived in `v0.2.0`, and the four cache
// variables, which arrive here in `v0.3.0` with the cache they configure. The
// `ArResolverContext` form arrives in `v0.6.0`.
//
// Parsing is separated from reading the environment, and from reporting, on
// purpose. An `ArResolver` is constructed once per process by `Plug`, so the
// interesting cases -- a value that does not parse, a value out of range, a
// value that is zero -- are otherwise reachable only by a test that mutates the
// process environment and then hopes about ordering. Here they are a table.
//
// No OpenUSD header. `httpResolver_test_configuration` links this translation
// unit alone.

#ifndef USDHTTPRESOLVER_CONFIGURATION_H
#define USDHTTPRESOLVER_CONFIGURATION_H

#include <functional>
#include <string>
#include <vector>

#include "usdAssetCache/CacheOptions.h"
#include "usdAssetHttp/HttpAssetReader.h"

namespace usdhttpresolver {

/// One variable that was set and could not be used.
///
/// CONFIGURATION.md §2: "An unparseable value is a diagnostic at first use, not
/// a silent fallback to the default." The default is still what gets used --
/// refusing to resolve anything because one environment variable has a typo
/// would be worse than the typo -- but it is used loudly.
struct ConfigurationProblem {
    std::string variable;
    std::string value;  ///< As set. These variables never carry a secret.
    std::string reason;
};

/// Reads one variable. Returns false when it is unset; an empty string is a
/// *set* variable whose value is empty, which is a problem rather than an
/// absence.
using EnvironmentLookup =
    std::function<bool(const char* name, std::string* valueOut)>;

/// The transport options `lookup` describes, starting from the defaults.
///
/// Every variable is independent: one bad value leaves the other four in force
/// rather than discarding the whole configuration.
usdasset::http::HttpOptions OptionsFrom(
    const EnvironmentLookup& lookup,
    std::vector<ConfigurationProblem>* problemsOut);

/// The same, against the process environment.
usdasset::http::HttpOptions OptionsFromEnvironment(
    std::vector<ConfigurationProblem>* problemsOut);

/// The cache policy `lookup` describes, starting from the shipped defaults.
///
/// The values the defaults are is a measured question and its answer is
/// docs/reference/BLOCK_POLICY.md; what this function does is let a deployment
/// override them, and refuse to do so silently when it asks for something that
/// is not a number.
///
/// The returned options are *not* normalized here. Normalization rounds and
/// clamps, and a value that had to be rounded is worth a diagnostic rather than
/// a silent adjustment -- so the rounding is reported as a problem and the
/// caller normalizes when it applies them.
usdasset::cache::CacheOptions CacheOptionsFrom(
    const EnvironmentLookup& lookup,
    std::vector<ConfigurationProblem>* problemsOut);

/// Everything one resolver is configured by, read in one pass.
struct ResolverConfiguration {
    usdasset::http::HttpOptions transport;
    usdasset::cache::CacheOptions cache;
};

ResolverConfiguration ConfigurationFrom(
    const EnvironmentLookup& lookup,
    std::vector<ConfigurationProblem>* problemsOut);

ResolverConfiguration ConfigurationFromEnvironment(
    std::vector<ConfigurationProblem>* problemsOut);

/// The variables this version reads, in the order CONFIGURATION.md lists them.
/// Exposed so a test asserts the set rather than restating it.
const std::vector<const char*>& ConfiguredVariables();

}  // namespace usdhttpresolver

#endif  // USDHTTPRESOLVER_CONFIGURATION_H
