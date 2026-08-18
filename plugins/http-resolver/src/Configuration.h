// SPDX-License-Identifier: Apache-2.0
//
// The environment-variable configuration surface, which CONFIGURATION.md §2
// schedules for `v0.2.0`: the five transport bounds, and nothing else. The
// cache variables arrive with the cache they configure, and the
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

/// The variables this version reads, in the order CONFIGURATION.md lists them.
/// Exposed so a test asserts the set rather than restating it.
const std::vector<const char*>& ConfiguredVariables();

}  // namespace usdhttpresolver

#endif  // USDHTTPRESOLVER_CONFIGURATION_H
