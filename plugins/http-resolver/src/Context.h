// SPDX-License-Identifier: Apache-2.0
//
// The resolver context: per-stage configuration, CONFIGURATION.md §1 and §4.
//
// The environment is a bootstrap. A host that opens two stages against two
// servers under two policies cannot be served by a process-global, and the
// `ArResolverContext` a stage is opened with is where OpenUSD already keeps
// what differs between stages. This is the object that goes in it.
//
// It is created from a string and from nothing else:
//
//   ArGetResolver().CreateContextFromString(
//       "https", "USD_HTTP_RESOLVER_DESTINATIONS=public; "
//                "USD_HTTP_RESOLVER_TOTAL_TIMEOUT_MS=60000")
//
// No header of this repository reaches a host that way, which is the same
// property ADR-0001 holds consumers to, extended to the hosts that configure
// them: the names are the environment's, and the entry point is OpenUSD's.
//
// What it carries is the overrides as written, after validation. It does not
// carry the configuration they produce, because that depends on the
// environment the resolver snapshot at construction, and a context is a value
// that two resolvers -- or two runs -- must compare equal on the strength of
// what it says rather than what it happened to combine with.

#ifndef USDHTTPRESOLVER_CONTEXT_H
#define USDHTTPRESOLVER_CONTEXT_H

#include <cstddef>
#include <map>
#include <string>

#include "pxr/pxr.h"
#include "pxr/usd/ar/defineResolverContext.h"
#include "pxr/usd/ar/resolverContext.h"

PXR_NAMESPACE_OPEN_SCOPE

class HttpResolverContext {
public:
    /// No overrides: a stage bound to this is configured by the environment,
    /// exactly as one bound to no context at all.
    HttpResolverContext() = default;

    /// `overrides` as `OverridesFrom` admitted them: names from
    /// `ContextVariables()`, values the environment's parser accepts.
    explicit HttpResolverContext(std::map<std::string, std::string> overrides);

    const std::map<std::string, std::string>& GetOverrides() const noexcept {
        return _overrides;
    }

    /// The canonical spelling: sorted, `;`-separated `NAME=value` entries. Two
    /// contexts that say the same thing print the same, whatever order and
    /// whitespace they were written in.
    std::string GetAsString() const;

    bool operator<(const HttpResolverContext& other) const {
        return _overrides < other._overrides;
    }
    bool operator==(const HttpResolverContext& other) const {
        return _overrides == other._overrides;
    }
    bool operator!=(const HttpResolverContext& other) const {
        return !(*this == other);
    }

private:
    std::map<std::string, std::string> _overrides;
};

size_t hash_value(const HttpResolverContext& context);

/// Found by argument-dependent lookup from `ArResolverContext`'s own debug
/// string, which would otherwise print a type name and an address.
std::string ArGetDebugString(const HttpResolverContext& context);

/// Makes a context readable from Python.
///
/// `Ar.ResolverContext` hands its objects to Python one at a time, through a
/// to-Python conversion, and an object with none cannot be handed. Measured
/// without this: `ctx.Get()` raises `TypeError: No to_python (by-value)
/// converter found`, and `Usd.Stage.__repr__`, which includes its resolver
/// context, prints `pathResolverContext=<invalid repr>` for any stage opened
/// with an `http` context -- in usdview's interpreter, in a pipeline script,
/// wherever Python looks at the stage. The conversion is to the canonical
/// string, which is what the object *is*; a Python class for it would be a
/// binding module this bundle does not otherwise need.
///
/// Registered once, and only once Python is running: a C++ host that never
/// starts an interpreter never pays for it, and one that does gets it at the
/// first context created afterwards. A no-op in a build without Python.
void HttpResolverContextEnsurePythonConversion();

AR_DECLARE_RESOLVER_CONTEXT(HttpResolverContext);

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // USDHTTPRESOLVER_CONTEXT_H
