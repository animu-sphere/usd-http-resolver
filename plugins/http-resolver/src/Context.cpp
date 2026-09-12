// SPDX-License-Identifier: Apache-2.0

#include "Context.h"

#include <atomic>
#include <functional>
#include <utility>

#include "Configuration.h"

#ifdef PXR_PYTHON_SUPPORT_ENABLED
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pyUtils.h"
#include "pxr/external/boost/python/object.hpp"
#include "pxr/external/boost/python/str.hpp"
#include "pxr/external/boost/python/to_python_converter.hpp"
#endif

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdhttpresolver {

HttpResolverContext::HttpResolverContext(std::map<std::string, std::string> overrides)
    : _overrides(std::move(overrides)) {}

std::string HttpResolverContext::GetAsString() const {
    return CanonicalContextString(_overrides);
}

size_t hash_value(const HttpResolverContext& context) {
    return std::hash<std::string>()(context.GetAsString());
}

std::string ArGetDebugString(const HttpResolverContext& context) {
    return "HttpResolverContext(" + context.GetAsString() + ")";
}

#ifdef PXR_PYTHON_SUPPORT_ENABLED
namespace {

struct HttpResolverContextToPython {
    static PyObject* convert(const HttpResolverContext& context) {
        return pxr_boost::python::incref(
            pxr_boost::python::str(context.GetAsString()).ptr());
    }
};

}  // namespace
#endif

void HttpResolverContextEnsurePythonConversion() {
#ifdef PXR_PYTHON_SUPPORT_ENABLED
    // Not `std::call_once`, because the question is not "has this been
    // attempted" but "has this been done": a first call made before the host
    // started Python must not use up the only chance.
    //
    // And no lock of its own. The flag is written only while the GIL is held,
    // so the GIL is what serializes registration; the atomic load before it is
    // only the fast path, so that a context created after registration does
    // not queue for the interpreter at all.
    static std::atomic<bool> registered{false};
    if (registered.load(std::memory_order_acquire) || !TfPyIsInitialized()) return;

    TfPyLock python;
    if (registered.load(std::memory_order_relaxed)) return;
    pxr_boost::python::to_python_converter<HttpResolverContext,
                                           HttpResolverContextToPython>();
    registered.store(true, std::memory_order_release);
#endif
}

}  // namespace usdhttpresolver
