// SPDX-License-Identifier: Apache-2.0
//
// The `HTTPxxx` projection: DIAGNOSTICS.md §5's table and §6's message form.
//
// The codes are a compatibility surface -- an operator writes an alert on
// `HTTP003`, a consumer greps a log for it -- so the table is asserted here
// rather than trusted to a switch statement nobody reads. What this cannot do
// is assert the OpenUSD channel each one goes down; that needs a runtime, and
// it is `httpResolver_test_stage`'s job.

#include <string>
#include <utility>
#include <vector>

#include "Check.h"
#include "Diagnostics.h"
#include "usdAssetIo/Diagnostics.h"

namespace {

using usdasset::Status;
using usdasset::StatusCode;
using usdhttpresolver::HttpCode;
using usdhttpresolver::Render;

/// DIAGNOSTICS.md §5, transcribed. A code that changes meaning is a breaking
/// change, and this table is what makes that statement enforceable.
void TestProjectionTable() {
    const std::vector<std::pair<StatusCode, const char*>> expected = {
        {StatusCode::Ok, ""},
        {StatusCode::NotFound, "HTTP001"},
        {StatusCode::AccessDenied, "HTTP002"},
        {StatusCode::RangeNotSupported, "HTTP003"},
        {StatusCode::InvalidResponse, "HTTP004"},
        {StatusCode::NetworkError, "HTTP005"},
        {StatusCode::Timeout, "HTTP006"},
        {StatusCode::AssetChanged, "HTTP007"},
        {StatusCode::Cancelled, "HTTP008"},
        {StatusCode::InvalidArgument, "HTTP009"},
        {StatusCode::Unsupported, "HTTP010"},
    };

    std::vector<std::string> seen;
    for (const auto& row : expected) {
        const std::string code = HttpCode(row.first);
        if (code != row.second) {
            std::fprintf(stderr, "FAIL %s:%d: %s projected to '%s', not '%s'\n",
                         __FILE__, __LINE__, usdasset::StatusCodeName(row.first),
                         code.c_str(), row.second);
            ++::usdassettest::FailureCount();
        }
        // Distinct, so that two conditions a caller would act on differently
        // cannot be reported as one.
        if (!code.empty()) {
            for (const std::string& other : seen) {
                CHECK(other != code);
            }
            seen.push_back(code);
        }
    }
    CHECK_EQ(seen.size(), std::size_t{10});
}

/// §6: the code first, the framing second, the identifier last.
void TestMessageForm() {
    Status status = Status::Error(StatusCode::InvalidResponse,
                                  "partial response did not cover the "
                                  "requested range")
                        .WithRange(1258291, 65536)
                        .WithTransportStatus(206);

    const std::string message =
        Render(status, "https://example.org/data/survey.copc");

    CHECK(message.rfind("HTTP004: ", 0) == 0);
    CHECK(message.find("1258291") != std::string::npos);
    CHECK(message.find("65536") != std::string::npos);
    CHECK(message.find("206") != std::string::npos);
    CHECK(message.find("https://example.org/data/survey.copc") !=
          std::string::npos);

    // A status with no range attached says so by omission rather than by
    // printing zeros, which would be a measurement nobody made.
    const std::string plain =
        Render(Status::Error(StatusCode::NotFound, "no such asset"),
               "https://example.org/a.usda");
    CHECK(plain.rfind("HTTP001: ", 0) == 0);
    CHECK(plain.find("offset") == std::string::npos);

    // A success is not a diagnostic.
    CHECK(std::string(HttpCode(StatusCode::Ok)).empty());
}

/// The negative this project cares most about: no rendered message repeats a
/// credential, whether it was hidden in the query string or in the authority.
void TestNoSecretSurvives() {
    const char* hostile =
        "https://alice:s3cr3t@example.org/a.usda?X-Amz-Signature=deadbeef";

    const std::vector<std::string> messages = {
        Render(Status::Error(StatusCode::AccessDenied, "403 from origin"),
               hostile),
        Render(Status::Error(StatusCode::Timeout, "response deadline elapsed")
                   .WithRange(0, 4096),
               hostile),
        usdhttpresolver::RenderRetryWarning(2, hostile),
    };

    for (const std::string& message : messages) {
        CHECK(message.find("s3cr3t") == std::string::npos);
        CHECK(message.find("deadbeef") == std::string::npos);
        CHECK(message.find("alice") == std::string::npos);
        CHECK(message.find("X-Amz-Signature") == std::string::npos);
        // What survives is the part that identifies the asset.
        CHECK(message.find("example.org/a.usda") != std::string::npos);
    }
}

void TestRetryWarning() {
    const std::string one =
        usdhttpresolver::RenderRetryWarning(1, "https://example.org/a.usda");
    CHECK(one.rfind("HTTP101: ", 0) == 0);
    CHECK(one.find(" 1 retry") != std::string::npos);

    const std::string many =
        usdhttpresolver::RenderRetryWarning(3, "https://example.org/a.usda");
    CHECK(many.find(" 3 retries") != std::string::npos);
}

}  // namespace

int main() {
    TestProjectionTable();
    TestMessageForm();
    TestNoSecretSurvives();
    TestRetryWarning();
    return usdassettest::Report("httpResolver diagnostics");
}
