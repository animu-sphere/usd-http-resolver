// SPDX-License-Identifier: Apache-2.0

#include "usdAssetIo/Diagnostics.h"

#include <string>

#include "Check.h"

using namespace usdasset;

namespace {

void EveryCodeHasAStableName() {
    const StatusCode codes[] = {
        StatusCode::Ok,           StatusCode::NotFound,
        StatusCode::AccessDenied, StatusCode::RangeNotSupported,
        StatusCode::InvalidResponse, StatusCode::NetworkError,
        StatusCode::Timeout,      StatusCode::AssetChanged,
        StatusCode::Cancelled,    StatusCode::InvalidArgument,
        StatusCode::Unsupported,
    };
    for (const StatusCode code : codes) {
        CHECK(std::string(StatusCodeName(code)) != "<unknown>");
    }
    CHECK(std::string(StatusCodeName(StatusCode::AssetChanged)) == "AssetChanged");
}

void SeverityFollowsTheProjectionTable() {
    // Cancellation is the caller getting what it asked for, not a fault.
    CHECK(DefaultSeverity(StatusCode::Cancelled) == Severity::Warning);
    // An overflowing offset is not a condition of the world; somebody computed it.
    CHECK(DefaultSeverity(StatusCode::InvalidArgument) == Severity::CodingError);
    CHECK(DefaultSeverity(StatusCode::NotFound) == Severity::Error);
    CHECK(DefaultSeverity(StatusCode::AssetChanged) == Severity::Error);
    CHECK(DefaultSeverity(StatusCode::RangeNotSupported) == Severity::Error);
}

void DefaultConstructedStatusIsSuccess() {
    const Status status;
    CHECK(status.IsOk());
    CHECK(Status::Ok().IsOk());
    CHECK(!Status::Error(StatusCode::Timeout, "elapsed").IsOk());
}

void AttachmentsSurviveChaining() {
    const Status status = Status::Error(StatusCode::InvalidResponse, "framing")
                              .WithRange(1258291, 65536)
                              .WithTransportStatus(206);
    CHECK(status.byteOffset.has_value() && *status.byteOffset == 1258291u);
    CHECK(status.byteLength.has_value() && *status.byteLength == 65536u);
    CHECK(status.transportStatus.has_value() && *status.transportStatus == 206);

    const std::string rendered = ToString(status);
    CHECK(rendered.find("InvalidResponse") != std::string::npos);
    CHECK(rendered.find("1258291") != std::string::npos);
    CHECK(rendered.find("206") != std::string::npos);
}

void QueryStringsAreElided() {
    CHECK(ElideSecrets("https://example.org/a.copc?X-Amz-Signature=deadbeef") ==
          "https://example.org/a.copc?<elided>");
    // A fragment after the query goes with it; there is no second parse.
    CHECK(ElideSecrets("https://example.org/a?sig=x#frag") ==
          "https://example.org/a?<elided>");
    // Nothing to elide is left exactly alone.
    CHECK(ElideSecrets("https://example.org/a.copc") == "https://example.org/a.copc");
    CHECK(ElideSecrets("/home/user/assets/a.copc") == "/home/user/assets/a.copc");
}

void UserinfoIsElided() {
    // The part a query-only rule keeps.
    CHECK(ElideSecrets("https://user:token@example.org/a.copc") ==
          "https://<elided>@example.org/a.copc");
    CHECK(ElideSecrets("https://user:token@example.org/a?sig=x") ==
          "https://<elided>@example.org/a?<elided>");
    // An '@' in a path is not an authority's userinfo.
    CHECK(ElideSecrets("https://example.org/mail@host/a.copc") ==
          "https://example.org/mail@host/a.copc");
    // Nor is one in a query string that has already been removed.
    CHECK(ElideSecrets("https://example.org/a?to=user@host") ==
          "https://example.org/a?<elided>");
}

void ElisionIsVisible() {
    // A trimmed URL has to read as trimmed. A silent truncation invites the
    // reader to believe the asset really is at that path.
    const std::string elided = ElideSecrets("https://example.org/a?sig=x");
    CHECK(elided.find("<elided>") != std::string::npos);
}

}  // namespace

int main() {
    EveryCodeHasAStableName();
    SeverityFollowsTheProjectionTable();
    DefaultConstructedStatusIsSuccess();
    AttachmentsSurviveChaining();
    QueryStringsAreElided();
    UserinfoIsElided();
    ElisionIsVisible();
    return usdassettest::Report("usdAssetIo diagnostics");
}
