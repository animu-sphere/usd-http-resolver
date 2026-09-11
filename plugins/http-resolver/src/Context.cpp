// SPDX-License-Identifier: Apache-2.0

#include "Context.h"

#include <functional>
#include <mutex>
#include <utility>

#include "Configuration.h"

#ifdef PXR_PYTHON_SUPPORT_ENABLED
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pyUtils.h"
#include "pxr/external/boost/python/object.hpp"
#include "pxr/external/boost/python/str.hpp"
#include "pxr/external/boost/python/to_python_converter.hpp"
#endif

PXR_NAMESPACE_OPEN_SCOPE

HttpResolverContext::HttpResolverContext(std::map<std::string, std::string> overrides)
    : _overrides(std::move(overrides)) {}

std::string HttpResolverContext::GetAsString() const {
    return usdhttpresolver::CanonicalContextString(_overrides);
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
    // A flag under a mutex rather than `std::call_once`, because the question
    // is not "has this been attempted" but "has this been done": a first call
    // made before the host started Python must not use up the only chance.
    static std::mutex mutex;
    static bool registered = false;

    std::lock_guard<std::mutex> lock(mutex);
    if (registered || !TfPyIsInitialized()) return;

    TfPyLock python;
    pxr_boost::python::to_python_converter<HttpResolverContext,
                                           HttpResolverContextToPython>();
    registered = true;
#endif
}

PXR_NAMESPACE_CLOSE_SCOPE
