// SPDX-License-Identifier: Apache-2.0
//
// The protocol rules, over a scripted transport.
//
// Deliberately not the hostile-corpus cases: those run against a real server in
// `tests/corpus`, because a mock that truncates a body on request would be this
// module asserting its own fiction. What is here is what no server can be asked
// to demonstrate -- a scheme downgrade over a plaintext fixture, a redirect
// bound counted exactly, a retry budget spent -- and the shape of what the
// backend *sends*, which is unassertable from a response.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Check.h"
#include "ScriptedTransport.h"
#include "TestSupport.h"
#include "usdAssetHttp/HttpAssetReader.h"

namespace {

using usdasset::IdentityStability;
using usdasset::ReadResult;
using usdasset::StatusCode;
using usdasset::ValidatorStrength;
using usdasset::http::HttpOpenResult;
using usdasset::http::HttpOptions;
using usdasset::http::Method;
using usdasset::http::TransportError;
using usdasset::http::TransportRequest;
using usdasset::http::TransportResponse;
using usdassethttptest::Factory;
using usdassethttptest::MetadataResponse;
using usdassethttptest::PartialResponse;
using usdassethttptest::RedirectResponse;
using usdassethttptest::Script;
using usdassethttptest::TransportFailure;

constexpr char kUrl[] = "https://example.org/data/survey.copc";
constexpr std::uint64_t kSize = 4096;

std::shared_ptr<Script> MakeScript(usdassethttptest::Responder responder) {
    auto script = std::make_shared<Script>();
    script->responder = std::move(responder);
    return script;
}

HttpOpenResult OpenWith(const std::shared_ptr<Script>& script,
                        const HttpOptions& options = HttpOptions()) {
    return usdasset::http::testing::OpenWithTransport(kUrl, options, Factory(script));
}

/// The ordinary server: a metadata response, then correct partial responses.
usdassethttptest::Responder Wellbehaved(const std::string& etag) {
    return [etag](const TransportRequest& request, int) {
        if (request.method == Method::Head) return MetadataResponse(kSize, etag);
        // The offset is recovered from the request rather than tracked, so the
        // script cannot quietly disagree with what the backend asked for.
        const std::size_t dash = request.range.find('-');
        const std::uint64_t first =
            std::strtoull(request.range.c_str() + 6, nullptr, 10);
        (void)dash;
        return PartialResponse(request, first, kSize, etag, 0xA5);
    };
}

// --- what the backend sends --------------------------------------------------

void TestRequestShape() {
    auto script = MakeScript(Wellbehaved("\"v1\""));
    HttpOpenResult opened = OpenWith(script);
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    // Open is one metadata round trip, and it reads no content.
    CHECK_EQ(script->Count(), 1);
    CHECK_EQ(script->sent[0].method, Method::Head);
    CHECK(script->sent[0].range.empty());
    CHECK(script->sent[0].ifRange.empty());
    CHECK_EQ(script->sent[0].capacity, std::size_t(0));

    std::vector<unsigned char> buffer(256);
    const ReadResult read = opened.reader->Read(1000, buffer.data(), 256);
    CHECK_EQ(read.status.code, StatusCode::Ok);
    CHECK_EQ(read.bytesRead, std::size_t(256));

    CHECK_EQ(script->Count(), 2);
    CHECK_EQ(script->sent[1].method, Method::Get);
    CHECK_EQ(script->sent[1].range, std::string("bytes=1000-1255"));
    // The conditional guard, on every range request after open, carrying the
    // captured validator verbatim. This is the mechanism §6 of the design
    // policy's consistency guarantee rests on.
    CHECK_EQ(script->sent[1].ifRange, std::string("\"v1\""));
    // The bound §10 requires: the transport is never allowed to write more than
    // the caller asked for, whatever the server declares.
    CHECK_EQ(script->sent[1].capacity, std::size_t(256));

    // A zero-length read and a read past EOF issue no request at all.
    unsigned char scratch[16];
    CHECK_EQ(opened.reader->Read(0, scratch, 0).status.code, StatusCode::Ok);
    CHECK_EQ(opened.reader->Read(kSize, scratch, 16).status.code, StatusCode::Ok);
    CHECK_EQ(opened.reader->Read(kSize + 1, scratch, 16).bytesRead, std::size_t(0));
    CHECK_EQ(script->Count(), 2);
}

void TestMetadata() {
    auto script = MakeScript(Wellbehaved("\"v1\""));
    HttpOpenResult opened = OpenWith(script);
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    const usdasset::AssetMetadata& metadata = opened.reader->Metadata();
    CHECK_EQ(metadata.size, kSize);
    CHECK(metadata.supportsRandomAccess);
    CHECK_EQ(metadata.validator.strength, ValidatorStrength::Strong);
    CHECK_EQ(metadata.stability, IdentityStability::Stable);
    CHECK_EQ(metadata.resolvedIdentifier, std::string(kUrl));
    CHECK_EQ(metadata.contentType, std::string("application/octet-stream"));
}

// --- open refusals -----------------------------------------------------------

void TestOpenRefusals() {
    {
        // No `Accept-Ranges`: terminal, per ADR-0002, with no whole-asset
        // fallback. Silently downloading a 10 GB asset because a header was
        // missing is the failure that ADR exists to prevent.
        auto script = MakeScript([](const TransportRequest&, int) {
            TransportResponse response;
            response.status = 200;
            response.connected = true;
            response.headers.Add("Content-Length", "4096");
            return response;
        });
        CHECK_EQ(OpenWith(script).status.code, StatusCode::RangeNotSupported);
    }
    {
        // No `Content-Length`: the size is unknowable without downloading the
        // asset, which is exactly what a range backend must not do.
        auto script = MakeScript([](const TransportRequest&, int) {
            TransportResponse response;
            response.status = 200;
            response.connected = true;
            response.headers.Add("Accept-Ranges", "bytes");
            return response;
        });
        CHECK_EQ(OpenWith(script).status.code, StatusCode::InvalidResponse);
    }
    {
        auto script = MakeScript([](const TransportRequest&, int) {
            TransportResponse response;
            response.status = 404;
            response.connected = true;
            return response;
        });
        const HttpOpenResult opened = OpenWith(script);
        CHECK_EQ(opened.status.code, StatusCode::NotFound);
        CHECK(opened.reader == nullptr);
    }
    {
        auto script = MakeScript([](const TransportRequest&, int) {
            TransportResponse response;
            response.status = 403;
            response.connected = true;
            return response;
        });
        // Separate from NotFound because one is a configuration problem and the
        // other is a scene problem (DIAGNOSTICS.md §4.4).
        CHECK_EQ(OpenWith(script).status.code, StatusCode::AccessDenied);
    }
    {
        // A server that refuses HEAD. Named as unsupported rather than
        // approximated with a range probe this backend does not implement.
        auto script = MakeScript([](const TransportRequest&, int) {
            TransportResponse response;
            response.status = 405;
            response.connected = true;
            return response;
        });
        CHECK_EQ(OpenWith(script).status.code, StatusCode::Unsupported);
    }
    {
        // Not a URL this backend serves.
        HttpOptions options;
        auto script = MakeScript(Wellbehaved("\"v1\""));
        const HttpOpenResult opened = usdasset::http::testing::OpenWithTransport(
            "s3://bucket/key", options, Factory(script));
        CHECK_EQ(opened.status.code, StatusCode::InvalidArgument);
        // And no request was issued for it.
        CHECK_EQ(script->Count(), 0);
    }
}

// --- redirects ---------------------------------------------------------------

void TestBoundedRedirects() {
    HttpOptions options;
    options.maxRedirects = 3;

    {
        // Within the bound: followed, counted, and the resolved identifier is
        // where the chain ended rather than where it started.
        auto script = MakeScript([](const TransportRequest& request, int index) {
            if (index < 3) {
                return RedirectResponse("/hop" + std::to_string(index + 1));
            }
            (void)request;
            return MetadataResponse(kSize, "\"v1\"");
        });
        const HttpOpenResult opened = OpenWith(script, options);
        CHECK(opened.reader != nullptr);
        if (opened.reader) {
            CHECK_EQ(opened.reader->Metadata().resolvedIdentifier,
                     std::string("https://example.org/hop3"));
            CHECK_EQ(opened.reader->Metrics().Snapshot().redirectCount, 3u);
            CHECK_EQ(opened.reader->Metrics().Snapshot().requestCount, 4u);
        }
    }
    {
        // One hop past the bound. The chain is refused, and the count is the
        // configured number plus the request that discovered the hop -- not
        // whatever the client library would have done on its own.
        auto script = MakeScript([](const TransportRequest&, int index) {
            return RedirectResponse("/hop" + std::to_string(index + 1));
        });
        const HttpOpenResult opened = OpenWith(script, options);
        CHECK_EQ(opened.status.code, StatusCode::InvalidResponse);
        CHECK_EQ(script->Count(), 4);
    }
    {
        auto script = MakeScript([](const TransportRequest&, int) {
            TransportResponse response;
            response.status = 302;
            response.connected = true;
            return response;  // No Location.
        });
        CHECK_EQ(OpenWith(script, options).status.code, StatusCode::InvalidResponse);
    }
    {
        // §10 of the design policy: reject a scheme downgrade. This is the case
        // the hostile corpus cannot carry, because a plaintext fixture server
        // has no `https` to be downgraded from.
        auto script = MakeScript([](const TransportRequest&, int) {
            return RedirectResponse("http://example.org/data/survey.copc");
        });
        const HttpOpenResult opened = OpenWith(script, options);
        CHECK_EQ(opened.status.code, StatusCode::InvalidResponse);
        CHECK(opened.status.message.find("https to http") != std::string::npos);
        // Refused before the request was issued, not after it came back.
        CHECK_EQ(script->Count(), 1);
    }
    {
        // The reverse is not a downgrade and is followed.
        auto script = MakeScript([](const TransportRequest&, int index) {
            if (index == 0) return RedirectResponse("https://cdn.example.net/x");
            return MetadataResponse(kSize, "\"v1\"");
        });
        const HttpOpenResult opened = usdasset::http::testing::OpenWithTransport(
            "http://example.org/a", options, Factory(script));
        CHECK(opened.reader != nullptr);
    }
}

// --- retry -------------------------------------------------------------------

void TestBoundedRetry() {
    HttpOptions options;
    options.maxAttempts = 3;

    {
        // A transient failure, then service. The retry is visible in metrics
        // rather than only in a debug log: a silent retry that succeeded still
        // cost latency (DIAGNOSTICS.md §3).
        auto script = MakeScript([](const TransportRequest&, int index) {
            if (index == 0) {
                TransportResponse response;
                response.status = 503;
                response.connected = true;
                return response;
            }
            return MetadataResponse(kSize, "\"v1\"");
        });
        const HttpOpenResult opened = OpenWith(script, options);
        CHECK(opened.reader != nullptr);
        if (opened.reader) {
            CHECK_EQ(opened.reader->Metrics().Snapshot().retryCount, 1u);
            // Two requests, because that is what the network saw. A counter
            // that reset on success would report what it wished had happened.
            CHECK_EQ(opened.reader->Metrics().Snapshot().requestCount, 2u);
        }
    }
    {
        // A budget spent. The status carries the server's own code as
        // diagnostic sugar, and the vocabulary has no server-is-unwell entry:
        // what a caller does about a spent 503 is what it does about a reset.
        auto script = MakeScript([](const TransportRequest&, int) {
            TransportResponse response;
            response.status = 503;
            response.connected = true;
            return response;
        });
        const HttpOpenResult opened = OpenWith(script, options);
        CHECK_EQ(opened.status.code, StatusCode::NetworkError);
        CHECK(opened.status.transportStatus.has_value());
        CHECK_EQ(opened.status.transportStatus.value(), 503);
        CHECK_EQ(script->Count(), 3);
    }
    {
        // A deadline is not retried. Re-spending it two more times triples the
        // wait the caller already declared too long.
        auto script = MakeScript([](const TransportRequest&, int) {
            return TransportFailure(TransportError::ResponseTimeout);
        });
        const HttpOpenResult opened = OpenWith(script, options);
        CHECK_EQ(opened.status.code, StatusCode::Timeout);
        CHECK_EQ(script->Count(), 1);
        // And it names which deadline elapsed, which `HTTP006` requires.
        CHECK(opened.status.message.find("response deadline") != std::string::npos);
    }
    {
        // A connection that never happened is retried: a `GET` is idempotent
        // and nothing reached the server.
        auto script = MakeScript([](const TransportRequest&, int) {
            return TransportFailure(TransportError::ConnectFailed);
        });
        const HttpOpenResult opened = OpenWith(script, options);
        CHECK_EQ(opened.status.code, StatusCode::NetworkError);
        CHECK_EQ(script->Count(), 3);
    }
    {
        // Retry can be turned off entirely.
        HttpOptions once;
        once.maxAttempts = 1;
        auto script = MakeScript([](const TransportRequest&, int) {
            return TransportFailure(TransportError::ConnectFailed);
        });
        OpenWith(script, once);
        CHECK_EQ(script->Count(), 1);
    }
}

// --- revision binding --------------------------------------------------------

void TestRevisionBinding() {
    {
        // The mechanism: an `If-Range` that did not match makes the `Range`
        // inapplicable and the whole representation is returned (RFC 9110
        // §13.1.5). That is the asset having moved, not data.
        auto script = MakeScript([](const TransportRequest& request, int index) {
            if (index == 0) return MetadataResponse(kSize, "\"v1\"");
            TransportResponse response;
            response.status = 200;
            response.connected = true;
            response.headers.Add("Content-Length", "8192");
            response.headers.Add("ETag", "\"v2\"");
            response.bodyBytes = request.bodyCapacity;
            response.bodyOverflowed = true;
            return response;
        });
        HttpOpenResult opened = OpenWith(script);
        CHECK(opened.reader != nullptr);
        if (!opened.reader) return;

        std::vector<unsigned char> buffer(256);
        const ReadResult read = opened.reader->Read(0, buffer.data(), 256);
        CHECK_EQ(read.status.code, StatusCode::AssetChanged);
        // Zero, not what arrived. Those bytes belong to a revision this reader
        // is not bound to, and reporting them as read invites exactly the
        // composition the guarantee exists to prevent.
        CHECK_EQ(read.bytesRead, std::size_t(0));

        // And it stays changed. A reader that recovers on the next call has
        // rebound to the new revision.
        const ReadResult again = opened.reader->Read(512, buffer.data(), 256);
        CHECK_EQ(again.status.code, StatusCode::AssetChanged);
        // The metadata it reports is still the revision it bound to.
        CHECK_EQ(opened.reader->Metadata().size, kSize);
    }
    {
        // The harder case, which `If-Range` alone does not cover: a `206` that
        // is correctly framed and carries a different validator. A backend
        // comparing bytes rather than identities sees nothing here.
        auto script = MakeScript([](const TransportRequest& request, int index) {
            if (index == 0) return MetadataResponse(kSize, "\"v1\"");
            return PartialResponse(request, 0, kSize, "\"v2\"", 0x11);
        });
        HttpOpenResult opened = OpenWith(script);
        CHECK(opened.reader != nullptr);
        if (!opened.reader) return;

        std::vector<unsigned char> buffer(256);
        const ReadResult read = opened.reader->Read(0, buffer.data(), 256);
        CHECK_EQ(read.status.code, StatusCode::AssetChanged);
        CHECK_EQ(read.bytesRead, std::size_t(0));
    }
    {
        // A representation whose length moved. Stronger evidence than a
        // validator, and it holds for an asset that supplied none at all.
        auto script = MakeScript([](const TransportRequest& request, int index) {
            if (index == 0) return MetadataResponse(kSize, std::string());
            TransportResponse response;
            response.status = 206;
            response.connected = true;
            response.headers.Add("Content-Range", "bytes 0-255/8192");
            response.bodyBytes = request.bodyCapacity;
            return response;
        });
        HttpOpenResult opened = OpenWith(script);
        CHECK(opened.reader != nullptr);
        if (!opened.reader) return;
        CHECK_EQ(opened.reader->Metadata().stability, IdentityStability::Unavailable);

        std::vector<unsigned char> buffer(256);
        CHECK_EQ(opened.reader->Read(0, buffer.data(), 256).status.code,
                 StatusCode::AssetChanged);
    }
}

void TestWeakValidatorSendsNoConditional() {
    auto script = MakeScript(Wellbehaved("W/\"v1\""));
    HttpOpenResult opened = OpenWith(script);
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    CHECK_EQ(opened.reader->Metadata().stability, IdentityStability::Unstable);

    std::vector<unsigned char> buffer(64);
    CHECK_EQ(opened.reader->Read(0, buffer.data(), 64).status.code, StatusCode::Ok);
    // Captured but not sent: RFC 9110 admits only a strong validator in
    // `If-Range`. The response-validator comparison is what guards this reader.
    CHECK_EQ(script->sent[1].ifRange, std::string());
}

void TestNoValidatorStillReads() {
    auto script = MakeScript(Wellbehaved(std::string()));
    HttpOpenResult opened = OpenWith(script);
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    // Reads work, the reader is bound as far as it can observe, and the
    // consumer is told the identity is not stable so it can disable its own
    // generated-cache reuse rather than guess (ASSET_READER.md §7.3).
    CHECK_EQ(opened.reader->Metadata().stability, IdentityStability::Unavailable);
    std::vector<unsigned char> buffer(64);
    const ReadResult read = opened.reader->Read(0, buffer.data(), 64);
    CHECK_EQ(read.status.code, StatusCode::Ok);
    CHECK_EQ(read.bytesRead, std::size_t(64));
    CHECK_EQ(script->sent[1].ifRange, std::string());
}

// --- framing against the request ---------------------------------------------

void TestFramingIsCheckedAgainstTheRequest() {
    {
        // Internally consistent, and wrong against the request. This is the
        // case DIAGNOSTICS.md §6 names: a backend validating framing against
        // itself passes it and copies short data into the caller's buffer.
        auto script = MakeScript([](const TransportRequest& request, int index) {
            if (index == 0) return MetadataResponse(kSize, "\"v1\"");
            TransportResponse response;
            response.status = 206;
            response.connected = true;
            response.headers.Add("Content-Range", "bytes 0-127/4096");
            response.headers.Add("ETag", "\"v1\"");
            response.bodyBytes = 128;
            return response;
        });
        HttpOpenResult opened = OpenWith(script);
        CHECK(opened.reader != nullptr);
        if (!opened.reader) return;

        std::vector<unsigned char> buffer(256);
        const ReadResult read = opened.reader->Read(0, buffer.data(), 256);
        CHECK_EQ(read.status.code, StatusCode::InvalidResponse);
        // No retry: a server that answered the wrong question is not a
        // transient, and asking again three times does not make it one.
        CHECK_EQ(script->Count(), 2);
    }
    {
        // A window at the wrong offset. Caught by a different check than the
        // one above -- start, not length -- which is why the corpus has two
        // rows for it.
        auto script = MakeScript([](const TransportRequest&, int index) {
            if (index == 0) return MetadataResponse(kSize, "\"v1\"");
            TransportResponse response;
            response.status = 206;
            response.connected = true;
            response.headers.Add("Content-Range", "bytes 1-256/4096");
            response.headers.Add("ETag", "\"v1\"");
            response.bodyBytes = 256;
            return response;
        });
        HttpOpenResult opened = OpenWith(script);
        CHECK(opened.reader != nullptr);
        if (!opened.reader) return;
        std::vector<unsigned char> buffer(256);
        CHECK_EQ(opened.reader->Read(0, buffer.data(), 256).status.code,
                 StatusCode::InvalidResponse);
    }
    {
        // A `206` with no `Content-Range` at all.
        auto script = MakeScript([](const TransportRequest& request, int index) {
            if (index == 0) return MetadataResponse(kSize, "\"v1\"");
            TransportResponse response;
            response.status = 206;
            response.connected = true;
            response.bodyBytes = request.bodyCapacity;
            return response;
        });
        HttpOpenResult opened = OpenWith(script);
        CHECK(opened.reader != nullptr);
        if (!opened.reader) return;
        std::vector<unsigned char> buffer(256);
        CHECK_EQ(opened.reader->Read(0, buffer.data(), 256).status.code,
                 StatusCode::InvalidResponse);
    }
    {
        // A server that stopped honoring ranges after open, with no guard sent
        // to make the whole body mean anything else. ADR-0002 makes this
        // terminal, and the body was cut off at the requested length rather
        // than downloaded.
        auto script = MakeScript([](const TransportRequest& request, int index) {
            if (index == 0) return MetadataResponse(kSize, std::string());
            TransportResponse response;
            response.status = 200;
            response.connected = true;
            response.headers.Add("Content-Length", "4096");
            response.bodyBytes = request.bodyCapacity;
            response.bodyOverflowed = true;
            return response;
        });
        HttpOpenResult opened = OpenWith(script);
        CHECK(opened.reader != nullptr);
        if (!opened.reader) return;
        std::vector<unsigned char> buffer(256);
        const ReadResult read = opened.reader->Read(0, buffer.data(), 256);
        CHECK_EQ(read.status.code, StatusCode::RangeNotSupported);
    }
}

// --- resume ------------------------------------------------------------------

void TestPartialBodyIsResumed() {
    // Honest framing, a body that stops early, then an orderly close. The read
    // contract admits a retry within policy; what it forbids is returning a
    // hole. Here the remainder is re-requested from where it stopped, which is
    // a resume rather than a re-fetch of what already arrived.
    auto script = MakeScript([](const TransportRequest& request, int index) {
        if (index == 0) return MetadataResponse(kSize, "\"v1\"");
        const std::uint64_t first =
            std::strtoull(request.range.c_str() + 6, nullptr, 10);
        TransportResponse response;
        response.status = 206;
        response.connected = true;
        response.headers.Add("Content-Range",
                             "bytes " + std::to_string(first) + "-" +
                                 std::to_string(first + request.bodyCapacity - 1) +
                                 "/" + std::to_string(kSize));
        response.headers.Add("ETag", "\"v1\"");
        const std::size_t half = request.bodyCapacity / 2;
        if (index == 1 && half > 0) {
            std::memset(request.body, 0x22, half);
            response.bodyBytes = half;
            response.error = TransportError::IncompleteBody;
            return response;
        }
        std::memset(request.body, 0x22, request.bodyCapacity);
        response.bodyBytes = request.bodyCapacity;
        return response;
    });

    HttpOpenResult opened = OpenWith(script);
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    std::vector<unsigned char> buffer(256, 0);
    const ReadResult read = opened.reader->Read(0, buffer.data(), 256);
    CHECK_EQ(read.status.code, StatusCode::Ok);
    CHECK_EQ(read.bytesRead, std::size_t(256));
    CHECK_EQ(script->Count(), 3);
    CHECK_EQ(script->sent[1].range, std::string("bytes=0-255"));
    // The second request asks for the remainder, not for the whole range again.
    CHECK_EQ(script->sent[2].range, std::string("bytes=128-255"));
    CHECK_EQ(opened.reader->Metrics().Snapshot().retryCount, 1u);
    for (const unsigned char byte : buffer) CHECK_EQ(byte, 0x22);
}

void TestResumeIsBounded() {
    // A server that always delivers half. The budget is spent and the read
    // fails with a hole reported as a failure rather than handed back as data.
    HttpOptions options;
    options.maxAttempts = 3;

    auto script = MakeScript([](const TransportRequest& request, int index) {
        if (index == 0) return MetadataResponse(kSize, "\"v1\"");
        const std::uint64_t first =
            std::strtoull(request.range.c_str() + 6, nullptr, 10);
        TransportResponse response;
        response.status = 206;
        response.connected = true;
        response.headers.Add("Content-Range",
                             "bytes " + std::to_string(first) + "-" +
                                 std::to_string(first + request.bodyCapacity - 1) +
                                 "/" + std::to_string(kSize));
        response.headers.Add("ETag", "\"v1\"");
        const std::size_t half = request.bodyCapacity / 2;
        std::memset(request.body, 0x33, half);
        response.bodyBytes = half;
        response.error = TransportError::IncompleteBody;
        return response;
    });

    HttpOpenResult opened = OpenWith(script, options);
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    std::vector<unsigned char> buffer(256, 0);
    const ReadResult read = opened.reader->Read(0, buffer.data(), 256);
    CHECK_EQ(read.status.code, StatusCode::InvalidResponse);
    CHECK(read.bytesRead < 256);
    // One metadata request plus the attempt budget, and not one request more.
    CHECK_EQ(script->Count(), 4);
}

void TestDestroyedConnectionIsNotAShortRead() {
    // The same shape as the resume above, ended differently: a connection that
    // was destroyed rather than closed. The corpus has a row for each, and they
    // are reported differently because a caller does different things about
    // them.
    HttpOptions options;
    options.maxAttempts = 2;

    auto script = MakeScript([](const TransportRequest& request, int index) {
        if (index == 0) return MetadataResponse(kSize, "\"v1\"");
        const std::uint64_t first =
            std::strtoull(request.range.c_str() + 6, nullptr, 10);
        TransportResponse response;
        response.status = 206;
        response.connected = true;
        response.headers.Add("Content-Range",
                             "bytes " + std::to_string(first) + "-" +
                                 std::to_string(first + request.bodyCapacity - 1) +
                                 "/" + std::to_string(kSize));
        response.headers.Add("ETag", "\"v1\"");
        response.bodyBytes = 8;
        std::memset(request.body, 0x44, 8);
        response.error = TransportError::ConnectionLost;
        return response;
    });

    HttpOpenResult opened = OpenWith(script, options);
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    std::vector<unsigned char> buffer(256, 0);
    const ReadResult read = opened.reader->Read(0, buffer.data(), 256);
    CHECK_EQ(read.status.code, StatusCode::NetworkError);
    CHECK(read.bytesRead < 256);
}

// --- overflow and argument checking -------------------------------------------

void TestRetryBudgetIsSharedAcrossOneRead() {
    // The budget is per logical operation, and the resume loop draws from the
    // same pool the transport-level retry does. Two loops each bounded by
    // `maxAttempts` would be jointly bounded by its square: 3 becomes 9, and a
    // caller who asked for three requests gets nine.
    HttpOptions options;
    options.maxAttempts = 3;

    auto script = MakeScript([](const TransportRequest& request, int index) {
        if (index == 0) return MetadataResponse(kSize, "\"v1\"");
        const std::uint64_t first =
            std::strtoull(request.range.c_str() + 6, nullptr, 10);
        // Correctly framed, and then the connection dies every single time --
        // which is retryable at the transport level *and* resumable at the read
        // level, so it draws from both loops at once.
        TransportResponse response;
        response.status = 206;
        response.connected = true;
        response.headers.Add("Content-Range",
                             "bytes " + std::to_string(first) + "-" +
                                 std::to_string(first + request.bodyCapacity - 1) +
                                 "/" + std::to_string(kSize));
        response.headers.Add("ETag", "\"v1\"");
        response.bodyBytes = 1;
        std::memset(request.body, 0x55, 1);
        response.error = TransportError::ConnectionLost;
        return response;
    });

    HttpOpenResult opened = OpenWith(script, options);
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    std::vector<unsigned char> buffer(256, 0);
    const ReadResult read = opened.reader->Read(0, buffer.data(), 256);
    CHECK_EQ(read.status.code, StatusCode::NetworkError);
    // One metadata request, then the attempt budget for the read and not one
    // request more: 1 + 3, never 1 + 9.
    CHECK_EQ(script->Count(), 4);
    CHECK_EQ(opened.reader->Metrics().Snapshot().retryCount, 2u);
}

void TestRefusedRangeThatMeansTheAssetMoved() {
    // A `416` for a range that lies inside the size captured at open is the one
    // refusal that is usually true: the representation is no longer the one
    // that size came from. Reporting `InvalidResponse` there attaches a message
    // that is factually false.
    {
        auto script = MakeScript([](const TransportRequest&, int index) {
            if (index == 0) return MetadataResponse(kSize, "\"v1\"");
            TransportResponse response;
            response.status = 416;
            response.connected = true;
            response.headers.Add("Content-Range", "bytes */64");
            return response;
        });
        HttpOpenResult opened = OpenWith(script);
        CHECK(opened.reader != nullptr);
        if (!opened.reader) return;

        std::vector<unsigned char> buffer(256, 0);
        const ReadResult read = opened.reader->Read(0, buffer.data(), 256);
        CHECK_EQ(read.status.code, StatusCode::AssetChanged);
        CHECK_EQ(read.bytesRead, std::size_t(0));
    }
    {
        // A different validator says the same thing without a length.
        auto script = MakeScript([](const TransportRequest&, int index) {
            if (index == 0) return MetadataResponse(kSize, "\"v1\"");
            TransportResponse response;
            response.status = 416;
            response.connected = true;
            response.headers.Add("ETag", "\"v2\"");
            response.headers.Add("Content-Range",
                                 "bytes */" + std::to_string(kSize));
            return response;
        });
        HttpOpenResult opened = OpenWith(script);
        CHECK(opened.reader != nullptr);
        if (!opened.reader) return;
        std::vector<unsigned char> buffer(256, 0);
        CHECK_EQ(opened.reader->Read(0, buffer.data(), 256).status.code,
                 StatusCode::AssetChanged);
    }
    {
        // A `416` that contradicts nothing is still a server refusing a range
        // it had already sized, and that is a malformed exchange.
        auto script = MakeScript([](const TransportRequest&, int index) {
            if (index == 0) return MetadataResponse(kSize, "\"v1\"");
            TransportResponse response;
            response.status = 416;
            response.connected = true;
            response.headers.Add("ETag", "\"v1\"");
            response.headers.Add("Content-Range",
                                 "bytes */" + std::to_string(kSize));
            return response;
        });
        HttpOpenResult opened = OpenWith(script);
        CHECK(opened.reader != nullptr);
        if (!opened.reader) return;
        std::vector<unsigned char> buffer(256, 0);
        CHECK_EQ(opened.reader->Read(0, buffer.data(), 256).status.code,
                 StatusCode::InvalidResponse);
    }
}

void TestConflictingContentLengthIsRefused() {
    // RFC 9110 §8.6. An intermediary that believes the first and an origin that
    // believes the last disagree about where this message ends and the next
    // begins, which is the whole mechanism of request smuggling.
    {
        auto script = MakeScript([](const TransportRequest&, int) {
            TransportResponse response;
            response.status = 200;
            response.connected = true;
            response.headers.Add("Accept-Ranges", "bytes");
            response.headers.Add("Content-Length", "4096");
            response.headers.Add("Content-Length", "8192");
            return response;
        });
        CHECK_EQ(OpenWith(script).status.code, StatusCode::InvalidResponse);
    }
    {
        // Repeated identically is redundant, not hostile, and opens fine.
        auto script = MakeScript([](const TransportRequest&, int) {
            TransportResponse response;
            response.status = 200;
            response.connected = true;
            response.headers.Add("Accept-Ranges", "bytes");
            response.headers.Add("Content-Length", "4096");
            response.headers.Add("Content-Length", "4096");
            return response;
        });
        CHECK(OpenWith(script).reader != nullptr);
    }
}

void TestCallerErrors() {
    auto script = MakeScript(Wellbehaved("\"v1\""));
    HttpOpenResult opened = OpenWith(script);
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    unsigned char scratch[16];
    const ReadResult overflow =
        opened.reader->Read((std::numeric_limits<std::uint64_t>::max)(), scratch, 16);
    CHECK_EQ(overflow.status.code, StatusCode::InvalidArgument);
    CHECK_EQ(overflow.bytesRead, std::size_t(0));

    const ReadResult nullBuffer = opened.reader->Read(0, nullptr, 16);
    CHECK_EQ(nullBuffer.status.code, StatusCode::InvalidArgument);

    // Neither issued a request.
    CHECK_EQ(script->Count(), 1);
}

}  // namespace

int main() {
    TestRequestShape();
    TestMetadata();
    TestOpenRefusals();
    TestBoundedRedirects();
    TestBoundedRetry();
    TestRevisionBinding();
    TestWeakValidatorSendsNoConditional();
    TestNoValidatorStillReads();
    TestFramingIsCheckedAgainstTheRequest();
    TestPartialBodyIsResumed();
    TestResumeIsBounded();
    TestDestroyedConnectionIsNotAShortRead();
    TestRetryBudgetIsSharedAcrossOneRead();
    TestRefusedRangeThatMeansTheAssetMoved();
    TestConflictingContentLengthIsRefused();
    TestCallerErrors();
    return usdassettest::Report("usdAssetHttp/protocol");
}
