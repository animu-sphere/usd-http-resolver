// SPDX-License-Identifier: Apache-2.0

#include "Report.h"

#include <string>

#include "pxr/base/tf/diagnostic.h"
#include "pxr/pxr.h"

#include "Diagnostics.h"

// The `TF_*` macros expand to helpers inside OpenUSD's namespace, and this
// bundle's own code is outside it. The alternative -- moving these three
// functions into `PXR_NAMESPACE` -- would put non-OpenUSD types there, which is
// how a namespace stops meaning anything.
PXR_NAMESPACE_USING_DIRECTIVE

namespace usdhttpresolver {

void Report(const usdasset::Status& status, std::string_view identifier) {
    if (status.IsOk()) return;

    const std::string message = Render(status, identifier);
    switch (status.severity) {
        case usdasset::Severity::Warning:
            TF_WARN("%s", message.c_str());
            return;
        case usdasset::Severity::CodingError:
            TF_CODING_ERROR("%s", message.c_str());
            return;
        case usdasset::Severity::Error:
            break;
    }
    TF_RUNTIME_ERROR("%s", message.c_str());
}

void ReportRetries(std::uint64_t retryCount, std::string_view identifier) {
    if (retryCount == 0) return;
    TF_WARN("%s", RenderRetryWarning(retryCount, identifier).c_str());
}

void ReportConfigurationProblem(const ConfigurationProblem& problem) {
    // Four sentences rather than one with blanks in it, because the four end
    // differently and the ending is the part an operator acts on. An adjusted
    // value was used; a refused one was not; and a refused *context* value
    // leaves its stage on the environment's value, which is a different
    // fallback from the environment's own.
    if (problem.fromContext) {
        if (problem.variable.empty()) {
            TF_WARN("a resolver context has an entry '%s', which is %s; it is "
                    "ignored",
                    problem.value.c_str(), problem.reason.c_str());
        } else if (problem.adjusted) {
            TF_WARN("a resolver context sets %s to '%s': %s",
                    problem.variable.c_str(), problem.value.c_str(),
                    problem.reason.c_str());
        } else {
            TF_WARN("a resolver context sets %s to '%s', which is %s; stages "
                    "bound to it use the environment's value or the default",
                    problem.variable.c_str(), problem.value.c_str(),
                    problem.reason.c_str());
        }
        return;
    }
    if (problem.adjusted) {
        TF_WARN("%s is set to '%s': %s", problem.variable.c_str(),
                problem.value.c_str(), problem.reason.c_str());
        return;
    }
    TF_WARN("%s is set to '%s', which is %s; using the default",
            problem.variable.c_str(), problem.value.c_str(),
            problem.reason.c_str());
}

}  // namespace usdhttpresolver
