// SPDX-License-Identifier: Apache-2.0
//
// The configuration surface of CONFIGURATION.md: the five transport bounds,
// which arrived in `v0.2.0`, the four cache variables, which arrived in
// `v0.3.0` with the cache they configure, the two persistence variables, which
// arrived in `v0.4.0` with the tier they turn on, and the destination policy,
// which arrived in `v0.7.0` -- read from the environment for the process, and
// from an `ArResolverContext` for a stage.
//
// Parsing is separated from reading the environment, and from reporting, on
// purpose. An `ArResolver` is constructed once per process by `Plug`, so the
// interesting cases -- a value that does not parse, a value out of range, a
// value that is zero -- are otherwise reachable only by a test that mutates the
// process environment and then hopes about ordering. Here they are a table.
// The context form is the same table read through a different lookup, which is
// what keeps it one vocabulary rather than two.
//
// No OpenUSD header. `httpResolver_test_configuration` links this translation
// unit alone.

#ifndef USDHTTPRESOLVER_CONFIGURATION_H
#define USDHTTPRESOLVER_CONFIGURATION_H

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "usdAssetCache/CacheOptions.h"
#include "usdAssetCache/DiskBlockStore.h"
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

    /// The value was used after an adjustment -- rounded, capped, raised --
    /// rather than refused. The two are reported differently because they end
    /// differently: an operator told "using the default" about a value that
    /// was in fact used, rounded, has been told something false.
    bool adjusted = false;

    /// Written in a resolver context rather than in the environment. A refused
    /// context value leaves that stage on the environment's value, which is not
    /// the same fallback as the environment's own, and the message says so.
    bool fromContext = false;
};

/// Reads one variable. Returns false when it is unset; an empty string is a
/// *set* variable whose value is empty, which is a problem rather than an
/// absence.
using EnvironmentLookup =
    std::function<bool(const char* name, std::string* valueOut)>;

/// Reads one variable from the process environment, and the only place this
/// bundle calls `getenv`. The resolver takes a `Snapshot` through it once, and
/// everything after that reads the snapshot.
bool ReadEnvironmentVariable(const char* name, std::string* valueOut);

/// The transport options `lookup` describes, starting from the defaults.
///
/// Every variable is independent: one bad value leaves the others in force
/// rather than discarding the whole configuration.
usdasset::http::HttpOptions OptionsFrom(
    const EnvironmentLookup& lookup,
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

/// The persistent cache policy `lookup` describes.
///
/// Persistence is off unless a directory is named, which is why the interesting
/// return value is `directory`: an unset variable is not a problem, and an empty
/// one is. A budget without a directory is read and reported like any other
/// variable and then goes unused, because a variable that is silently ignored
/// depending on another variable is a variable nobody can debug.
usdasset::cache::DiskCacheOptions PersistenceOptionsFrom(
    const EnvironmentLookup& lookup,
    std::vector<ConfigurationProblem>* problemsOut);

/// Everything one resolver is configured by, read in one pass.
struct ResolverConfiguration {
    usdasset::http::HttpOptions transport;
    usdasset::cache::CacheOptions cache;
    usdasset::cache::DiskCacheOptions persistence;
};

ResolverConfiguration ConfigurationFrom(
    const EnvironmentLookup& lookup,
    std::vector<ConfigurationProblem>* problemsOut);

/// The variables this version reads, in the order CONFIGURATION.md lists them.
/// Exposed so a test asserts the set rather than restating it.
const std::vector<const char*>& ConfiguredVariables();

// --- the context form ----------------------------------------------------------

/// The variables a resolver context may set: the ones that bind a reader or a
/// wrap, and are therefore a property of whoever opened the asset.
///
/// Not the others, and the reason is the same for all four. The block store and
/// the persistent tier are shared by every stage in the process -- one budget,
/// CACHE.md §7, and one directory -- and the store's stripes are sized for one
/// block size, eight blocks to a stripe. A stage that asked for blocks larger
/// than a stripe would fetch each one and watch it evicted on arrival. So the
/// block size, the two budgets, and the directory are the environment's alone.
const std::vector<const char*>& ContextVariables();

/// Parses a context string: `NAME=value` entries separated by `;`, each `NAME`
/// one of `ContextVariables()` spelled exactly as the environment spells it.
///
/// Returns the entries admitted, keyed by name, each value in its canonical
/// spelling -- the number the parser read, or the destination classes in a
/// fixed order -- so that two contexts that say the same thing are equal. One
/// vocabulary rather than two: a value is checked by the parser the
/// environment's value would go through, over `base` (the environment the
/// context will be layered on), and refused or adjusted for the same reasons.
/// Whitespace around an entry, a name, or a value is tolerated, and so is an
/// empty entry -- a trailing `;` is what concatenation leaves behind. A name
/// set twice considers only the last value, the way an environment assignment
/// would, and says so.
///
/// Everything not admitted is a problem marked `fromContext`, and the stage
/// that binds the context takes the environment's value for it instead.
std::map<std::string, std::string> OverridesFrom(
    const std::string& text,
    const EnvironmentLookup& base,
    std::vector<ConfigurationProblem>* problemsOut);

/// The canonical spelling of a set of overrides: `NAME=value` entries, sorted
/// by name, separated by `;`. Two contexts that set the same values print the
/// same, which is what a debug string and a `repr` are for.
std::string CanonicalContextString(const std::map<std::string, std::string>& overrides);

/// A lookup over a fixed set of values.
///
/// It refers to `values` rather than copying it -- a lookup is built per call
/// under a context and a copy of the environment per call is a cost with no
/// purpose -- so `values` must outlive it.
EnvironmentLookup LookupIn(const std::map<std::string, std::string>& values);

/// A lookup that answers from `overrides` first and from `base` after:
/// CONFIGURATION.md §4's precedence, context over environment over default,
/// as a function. Refers to `overrides`, which must outlive it.
EnvironmentLookup Layered(const std::map<std::string, std::string>& overrides,
                          EnvironmentLookup base);

/// The value of every variable this version reads that `lookup` has.
///
/// The resolver takes one of these of its environment at construction, and
/// resolves a context against it rather than against `getenv`. CONFIGURATION.md
/// §4: resolved at bind time, not per request -- a host that mutates its
/// environment mid-session must not change what a stage opened an hour ago is
/// configured by.
std::map<std::string, std::string> Snapshot(const EnvironmentLookup& lookup);

/// Equal for two option sets exactly when a reader opened under one may be
/// handed to a caller configured with the other.
///
/// Every field a reader carries for its lifetime is in it -- the deadlines, the
/// retry and redirect bounds, the destination policy -- because a reader keeps
/// the options it was opened with. Handing one opened under a permissive policy
/// to a stage whose context refuses that destination would let the stage read
/// from somewhere its own policy forbids.
std::string TransportFingerprint(const usdasset::http::HttpOptions& options);

}  // namespace usdhttpresolver

#endif  // USDHTTPRESOLVER_CONFIGURATION_H
