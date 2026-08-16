// SPDX-License-Identifier: Apache-2.0

#include "usdAssetIo/Diagnostics.h"

#include <string>
#include <utility>

namespace usdasset {

const char* StatusCodeName(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::Ok: return "Ok";
        case StatusCode::NotFound: return "NotFound";
        case StatusCode::AccessDenied: return "AccessDenied";
        case StatusCode::RangeNotSupported: return "RangeNotSupported";
        case StatusCode::InvalidResponse: return "InvalidResponse";
        case StatusCode::NetworkError: return "NetworkError";
        case StatusCode::Timeout: return "Timeout";
        case StatusCode::AssetChanged: return "AssetChanged";
        case StatusCode::Cancelled: return "Cancelled";
        case StatusCode::InvalidArgument: return "InvalidArgument";
        case StatusCode::Unsupported: return "Unsupported";
    }
    return "<unknown>";
}

const char* SeverityName(Severity severity) noexcept {
    switch (severity) {
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
        case Severity::CodingError: return "coding error";
    }
    return "<unknown>";
}

Severity DefaultSeverity(StatusCode code) noexcept {
    switch (code) {
        // Severity is not meaningful on success. The value mirrors the
        // declared default of the struct rather than inventing a fourth
        // meaning for it.
        case StatusCode::Ok:
            return Severity::Error;
        // Caller cancellation is not a fault; it is the caller getting what it
        // asked for.
        case StatusCode::Cancelled:
            return Severity::Warning;
        // An overflowing offset is not a condition of the world. Somebody
        // computed it.
        case StatusCode::InvalidArgument:
            return Severity::CodingError;
        default:
            return Severity::Error;
    }
}

Status Status::Error(StatusCode code, std::string message) {
    Status status;
    status.code = code;
    status.severity = DefaultSeverity(code);
    status.message = std::move(message);
    return status;
}

Status& Status::WithRange(std::uint64_t offset, std::size_t length) & {
    byteOffset = offset;
    byteLength = length;
    return *this;
}

Status&& Status::WithRange(std::uint64_t offset, std::size_t length) && {
    byteOffset = offset;
    byteLength = length;
    return std::move(*this);
}

Status& Status::WithTransportStatus(int status) & {
    transportStatus = status;
    return *this;
}

Status&& Status::WithTransportStatus(int status) && {
    transportStatus = status;
    return std::move(*this);
}

Status& Status::WithSeverity(Severity value) & {
    severity = value;
    return *this;
}

Status&& Status::WithSeverity(Severity value) && {
    severity = value;
    return std::move(*this);
}

std::string ToString(const Status& status) {
    std::string out = StatusCodeName(status.code);
    out += " (";
    out += SeverityName(status.severity);
    out += ")";
    if (!status.message.empty()) {
        out += ": ";
        out += status.message;
    }
    if (status.byteOffset) {
        out += " [offset ";
        out += std::to_string(*status.byteOffset);
        if (status.byteLength) {
            out += ", length ";
            out += std::to_string(*status.byteLength);
        }
        out += "]";
    }
    if (status.transportStatus) {
        out += " [transport ";
        out += std::to_string(*status.transportStatus);
        out += "]";
    }
    return out;
}

std::string ElideSecrets(std::string_view identifier) {
    std::string out(identifier);

    // Query string. Everything from the first '?' goes, including a fragment
    // that follows it -- a signed URL's signature is in there, and the
    // fragment after it is not worth a second parse.
    const std::string::size_type query = out.find('?');
    if (query != std::string::npos) {
        out.erase(query);
        out += "?<elided>";
    }

    // Userinfo. `scheme://user:token@host/path` hides a credential in the part
    // a query-only rule keeps. The authority ends at the first '/', '?', or
    // '#' after the scheme separator, so an '@' in a path is not mistaken for
    // one in an authority.
    const std::string::size_type schemeEnd = out.find("//");
    if (schemeEnd != std::string::npos) {
        const std::string::size_type authorityBegin = schemeEnd + 2;
        std::string::size_type authorityEnd = out.find_first_of("/?#", authorityBegin);
        if (authorityEnd == std::string::npos) {
            authorityEnd = out.size();
        }
        const std::string::size_type at = out.rfind('@', authorityEnd);
        if (at != std::string::npos && at >= authorityBegin && at < authorityEnd) {
            out.replace(authorityBegin, at + 1 - authorityBegin, "<elided>@");
        }
    }

    return out;
}

}  // namespace usdasset
