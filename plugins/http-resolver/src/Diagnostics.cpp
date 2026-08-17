// SPDX-License-Identifier: Apache-2.0

#include "Diagnostics.h"

#include <string>

namespace usdhttpresolver {

const char* HttpCode(usdasset::StatusCode code) noexcept {
    switch (code) {
        case usdasset::StatusCode::Ok:                return "";
        case usdasset::StatusCode::NotFound:          return "HTTP001";
        case usdasset::StatusCode::AccessDenied:      return "HTTP002";
        case usdasset::StatusCode::RangeNotSupported: return "HTTP003";
        case usdasset::StatusCode::InvalidResponse:   return "HTTP004";
        case usdasset::StatusCode::NetworkError:      return "HTTP005";
        case usdasset::StatusCode::Timeout:           return "HTTP006";
        case usdasset::StatusCode::AssetChanged:      return "HTTP007";
        case usdasset::StatusCode::Cancelled:         return "HTTP008";
        case usdasset::StatusCode::InvalidArgument:   return "HTTP009";
        case usdasset::StatusCode::Unsupported:       return "HTTP010";
    }
    // Unreachable for a value of the enumeration. Reached only by a cast of an
    // out-of-range integer, which is a caller defect rather than a condition:
    // naming it beats returning a code that means something else.
    return "HTTP000";
}

std::string Render(const usdasset::Status& status,
                   std::string_view identifier) {
    std::string out = HttpCode(status.code);
    if (!status.message.empty()) {
        out += ": ";
        out += status.message;
    }

    // The range, when the producer knew it. "Read failed at offset 1258291,
    // length 65536" is actionable; "read failed" is not.
    if (status.byteOffset.has_value()) {
        out += " (offset ";
        out += std::to_string(*status.byteOffset);
        if (status.byteLength.has_value()) {
            out += ", length ";
            out += std::to_string(*status.byteLength);
        }
        out += ")";
    }

    if (status.transportStatus.has_value()) {
        out += " [HTTP ";
        out += std::to_string(*status.transportStatus);
        out += "]";
    }

    if (!identifier.empty()) {
        out += " ";
        out += usdasset::ElideSecrets(identifier);
    }
    return out;
}

std::string RenderRetryWarning(std::uint64_t retryCount,
                               std::string_view identifier) {
    std::string out = kRetryOccurred;
    out += ": request succeeded after ";
    out += std::to_string(retryCount);
    out += retryCount == 1 ? " retry" : " retries";
    if (!identifier.empty()) {
        out += " ";
        out += usdasset::ElideSecrets(identifier);
    }
    return out;
}

}  // namespace usdhttpresolver
