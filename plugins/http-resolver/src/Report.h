// SPDX-License-Identifier: Apache-2.0
//
// The one place this bundle talks to OpenUSD's diagnostic system.
//
// Split from `Diagnostics.h` because the two halves have different testability:
// what a condition renders as is arithmetic and is asserted offline, while
// which OpenUSD channel it goes down needs a runtime. Keeping the split visible
// keeps the first half honest -- there is no path from a `Status` to a human
// that does not go through `Render`, and therefore none that skips
// `ElideSecrets`.

#ifndef USDHTTPRESOLVER_REPORT_H
#define USDHTTPRESOLVER_REPORT_H

#include <cstdint>
#include <string_view>

#include "Configuration.h"
#include "usdAssetIo/Diagnostics.h"

namespace usdhttpresolver {

/// Posts `status` as the OpenUSD diagnostic DIAGNOSTICS.md §5 assigns it: an
/// error for every transport failure, a warning for a cancellation, and a
/// coding error for a request that could never have been valid.
///
/// A successful status posts nothing.
void Report(const usdasset::Status& status, std::string_view identifier);

/// Posts `HTTP101` when `retryCount` is non-zero. A no-op otherwise, so that
/// the caller can hand it a counter unconditionally.
void ReportRetries(std::uint64_t retryCount, std::string_view identifier);

/// Posts a warning for an environment variable that was set and could not be
/// used, per CONFIGURATION.md §2. The variable's value is included because
/// these five carry no secret and because a typo is unfindable otherwise.
void ReportConfigurationProblem(const ConfigurationProblem& problem);

}  // namespace usdhttpresolver

#endif  // USDHTTPRESOLVER_REPORT_H
