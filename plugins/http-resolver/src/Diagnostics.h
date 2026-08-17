// SPDX-License-Identifier: Apache-2.0
//
// The `HTTPxxx` projection: DIAGNOSTICS.md §5's table and §6's message form.
//
// The bundle owns these codes (WORKSPACE.md §1). They are a compatibility
// surface in the same sense a schema field is -- a consumer greps for `HTTP003`
// in a log, an operator writes an alert on it -- so the mapping lives in one
// total function rather than at each site that reports, and the table is
// asserted against the vocabulary rather than restated in prose.
//
// No OpenUSD header: rendering a message is arithmetic over a `Status`, and the
// test for it should not need a USD runtime. Emitting one is in `Report.h`,
// which does.

#ifndef USDHTTPRESOLVER_DIAGNOSTICS_H
#define USDHTTPRESOLVER_DIAGNOSTICS_H

#include <cstdint>
#include <string>
#include <string_view>

#include "usdAssetIo/Diagnostics.h"

namespace usdhttpresolver {

/// The stable code for a typed status, per DIAGNOSTICS.md §5.
///
/// Total over `StatusCode`: every code has a projection, including `Ok`, which
/// projects to the empty string because a success is not a diagnostic. A new
/// vocabulary code without a row here fails to compile rather than rendering as
/// an unnamed number.
const char* HttpCode(usdasset::StatusCode code) noexcept;

/// The codes allocated for conditions that are not a `StatusCode`. Both are
/// warnings about how an operation went, not about whether it succeeded.
///
/// `HTTP102` has no constant here on purpose: DIAGNOSTICS.md allocates it for
/// the bounded whole-asset fallback that ADR-0002 defers, and nothing in this
/// release may emit it. Holding the number in the table keeps a later feature
/// from renumbering the codes around it; holding it in code would invite
/// somebody to use it.
constexpr const char* kRetryOccurred = "HTTP101";

/// One line, per DIAGNOSTICS.md §6: the code, then the framing, then the
/// identifier with everything that can carry a credential elided.
///
/// The identifier goes through `usdasset::ElideSecrets` here rather than at the
/// call sites, so that a site that forgets cannot leak: every path from a
/// `Status` to a human passes through this function.
std::string Render(const usdasset::Status& status,
                   std::string_view identifier);

/// The `HTTP101` line, for a request that succeeded after being retried.
///
/// A silent retry that succeeded still cost latency, and DIAGNOSTICS.md §3
/// requires it to be visible. It is a warning: the operation produced its
/// bytes.
std::string RenderRetryWarning(std::uint64_t retryCount,
                               std::string_view identifier);

}  // namespace usdhttpresolver

#endif  // USDHTTPRESOLVER_DIAGNOSTICS_H
