// SPDX-License-Identifier: Apache-2.0
//
// The typed diagnostic vocabulary shared by every module.
//
// Normative contract: docs/architecture/DIAGNOSTICS.md. This header is that
// document in code; when the two disagree, the document wins.

#ifndef USDASSETIO_DIAGNOSTICS_H
#define USDASSETIO_DIAGNOSTICS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace usdasset {

/// How a caller is expected to treat a status.
///
/// Severity is a property of the code, not of the site that produced it: a
/// backend that degraded warns, a backend that failed errors, and a caller that
/// asked for something impossible gets a coding error. `DefaultSeverity` is the
/// mapping, and it is the one every constructor uses.
enum class Severity {
    Warning,      ///< Degraded, not failed. The operation still produced bytes.
    Error,        ///< The operation failed.
    CodingError,  ///< The caller asked for something that cannot be valid.
};

/// The stable error vocabulary. A code is a compatibility commitment: adding
/// one is a minor change, changing what one means is a breaking change.
///
/// The test of a code is whether a caller would do something different. Two
/// transport failures that lead to identical caller behavior share one code.
enum class StatusCode {
    Ok,

    NotFound,           ///< The asset does not exist.
    AccessDenied,       ///< It exists, or may, but this caller may not read it.
    RangeNotSupported,  ///< The transport will not serve partial content.
    InvalidResponse,    ///< Malformed framing, wrong range, truncated below EOF.
    NetworkError,       ///< Connection failure, DNS, TLS, reset; a transport fault.
    Timeout,            ///< A deadline elapsed.
    AssetChanged,       ///< The validator changed while the asset was open.
    Cancelled,          ///< The caller cancelled.
    InvalidArgument,    ///< An overflowing or malformed request from the caller.
    Unsupported,        ///< A legal operation this backend does not implement.
};

/// Stable spelling of a code, for logs and test failure output. Never parsed.
const char* StatusCodeName(StatusCode code) noexcept;

/// Stable spelling of a severity.
const char* SeverityName(Severity severity) noexcept;

/// The severity a code carries unless a producer deliberately overrides it.
/// Mirrors the OpenUSD column of the projection table in DIAGNOSTICS.md §5.
Severity DefaultSeverity(StatusCode code) noexcept;

/// A typed failure, with the location of the failure attached where it is
/// known.
///
/// Callers branch on `code` and never on `message`. Two backends reporting the
/// same condition are required to agree on the code and are free to differ in
/// the message; the boundary suite compares codes for exactly that reason.
struct Status {
    StatusCode code = StatusCode::Ok;
    Severity severity = Severity::Error;
    std::string message;                    ///< For humans. Never contains a secret.
    std::optional<std::uint64_t> byteOffset;///< Where, when known.
    std::optional<std::size_t> byteLength;
    std::optional<int> transportStatus;     ///< e.g. HTTP 503. Diagnostic sugar; no
                                            ///< caller branches on it.

    bool IsOk() const noexcept { return code == StatusCode::Ok; }

    /// Success. `Status()` is already success; this names it at call sites.
    static Status Ok() noexcept { return Status{}; }

    /// A failure carrying the code's default severity.
    static Status Error(StatusCode code, std::string message);

    /// Attach the byte range the failure applies to. "Read failed at offset
    /// 1258291, length 65536" is actionable; "read failed" is not.
    Status& WithRange(std::uint64_t offset, std::size_t length) &;
    Status&& WithRange(std::uint64_t offset, std::size_t length) &&;

    /// Attach the transport's own status, when the transport has one.
    Status& WithTransportStatus(int status) &;
    Status&& WithTransportStatus(int status) &&;

    /// Override the default severity, for a producer that genuinely degraded
    /// rather than failed.
    Status& WithSeverity(Severity severity) &;
    Status&& WithSeverity(Severity severity) &&;
};

/// A single-line rendering, for test output and for a human reading a log.
///
/// The `HTTPxxx` message form in DIAGNOSTICS.md §6 is the *plugin's* rendering
/// and lands with the bundle in v0.2.0; this is the transport-independent one
/// underneath it.
std::string ToString(const Status& status);

/// Removes the parts of an identifier that can carry a credential: the query
/// string, and the userinfo component of an authority.
///
/// Every identifier that reaches a message, a log line, or a metrics dump goes
/// through this first. Both components are elided rather than only the query
/// string, because `https://user:token@host/a` hides a credential in the part a
/// query-only rule keeps. Elision is marked, not silent — a reader has to be
/// able to tell that a URL was trimmed rather than authored short.
std::string ElideSecrets(std::string_view identifier);

}  // namespace usdasset

#endif  // USDASSETIO_DIAGNOSTICS_H
