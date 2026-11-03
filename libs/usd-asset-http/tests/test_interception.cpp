// SPDX-License-Identifier: Apache-2.0
//
// The authentication seam, over a scripted transport.
//
// §4.3 of the design policy: a credential enters as an object that is asked,
// never as a value, and everything that would make it leak is the backend's to
// refuse. Each rule in `RequestInterceptor`'s comment has a case here, and each
// case is written so that removing the rule fails it -- a header sent to a
// second origin, a header sent past the destination policy, a reserved header
// replaced, a value that splits the request, a secret repeated in a message.
//
// A scripted transport rather than the fixture server, for the reason
// `test_protocol.cpp` gives: what is asserted is what the backend *sends*, and
// two origins, an `https` origin, and an instance-metadata literal are all
// things the loopback fixture cannot be. That the headers then reach a real
// socket through libcurl is `tests/corpus`'s to prove, and it does.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Check.h"
#include "ScriptedTransport.h"
#include "TestSupport.h"
#include "usdAssetHttp/HttpAssetReader.h"
#include "usdAssetIo/Diagnostics.h"

namespace {

using usdasset::ReadResult;
using usdasset::StatusCode;
using usdasset::http::HttpOpenResult;
using usdasset::http::HttpOptions;
using usdasset::http::InterceptedRequest;
using usdasset::http::IsInterceptableHeader;
using usdasset::http::kMaxInterceptedHeaderBytes;
using usdasset::http::Method;
using usdasset::http::RequestHeader;
using usdasset::http::RequestInterceptor;
using usdasset::http::TransportError;
using usdasset::http::TransportRequest;
using usdasset::http::TransportResponse;
using usdassethttptest::Factory;
using usdassethttptest::MetadataResponse;
using usdassethttptest::PartialResponse;
using usdassethttptest::RedirectResponse;
using usdassethttptest::Script;
using usdassethttptest::SentRequest;

constexpr char kUrl[] = "https://example.org/data/survey.copc";
constexpr std::uint64_t kSize = 4096;
constexpr char kSecret[] = "Bearer s3cret-token-0f9e";

/// An interceptor whose answer is a function, and which remembers every
/// request it was asked about.
class Recording final : public RequestInterceptor {
public:
    using Answer =
        std::function<bool(const InterceptedRequest&, std::vector<RequestHeader>*)>;

    explicit Recording(Answer answer) : _answer(std::move(answer)) {}

    bool Intercept(const InterceptedRequest& request,
                   std::vector<RequestHeader>* headersOut) const override {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _asked.push_back(request);
        }
        return _answer(request, headersOut);
    }

    std::vector<InterceptedRequest> Asked() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _asked;
    }

private:
    Answer _answer;
    mutable std::mutex _mutex;
    mutable std::vector<InterceptedRequest> _asked;
};

std::shared_ptr<Recording> Bearer() {
    return std::make_shared<Recording>(
        [](const InterceptedRequest&, std::vector<RequestHeader>* out) {
            out->push_back({"Authorization", kSecret});
            return true;
        });
}

std::shared_ptr<Recording> Adding(std::vector<RequestHeader> headers) {
    return std::make_shared<Recording>(
        [headers](const InterceptedRequest&, std::vector<RequestHeader>* out) {
            *out = headers;
            return true;
        });
}

HttpOptions With(std::shared_ptr<const RequestInterceptor> interceptor) {
    HttpOptions options;
    options.interceptor = std::move(interceptor);
    return options;
}

std::shared_ptr<Script> MakeScript(usdassethttptest::Responder responder) {
    auto script = std::make_shared<Script>();
    script->responder = std::move(responder);
    return script;
}

HttpOpenResult OpenWith(const std::shared_ptr<Script>& script,
                        const HttpOptions& options,
                        const std::string& url = kUrl) {
    return usdasset::http::testing::OpenWithTransport(url, options, Factory(script));
}

std::uint64_t FirstByte(const TransportRequest& request) {
    return std::strtoull(request.range.c_str() + 6, nullptr, 10);
}

usdassethttptest::Responder Wellbehaved() {
    return [](const TransportRequest& request, int) {
        if (request.method == Method::Head) return MetadataResponse(kSize, "\"v1\"");
        return PartialResponse(request, FirstByte(request), kSize, "\"v1\"", 0x5A);
    };
}

bool Carries(const SentRequest& sent, const std::string& name, const std::string& value) {
    for (const RequestHeader& header : sent.headers) {
        if (header.name == name && header.value == value) return true;
    }
    return false;
}

std::vector<SentRequest> Sent(const std::shared_ptr<Script>& script) {
    std::lock_guard<std::mutex> lock(script->mutex);
    return script->sent;
}

// --- the default -------------------------------------------------------------

void TestNoInterceptorAddsNothing() {
    // Public HTTP is the target: with nothing configured, nothing is added.
    auto script = MakeScript(Wellbehaved());
    HttpOpenResult opened = OpenWith(script, HttpOptions());
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;
    unsigned char buffer[64];
    CHECK_EQ(opened.reader->Read(0, buffer, sizeof buffer).status.code, StatusCode::Ok);
    for (const SentRequest& sent : Sent(script)) CHECK(sent.headers.empty());
}

// --- what an interceptor is told, and where its headers go -------------------

void TestEveryRequestToTheOriginCarriesTheHeaders() {
    auto interceptor = Bearer();
    auto script = MakeScript(Wellbehaved());
    HttpOpenResult opened = OpenWith(script, With(interceptor));
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    unsigned char buffer[128];
    CHECK_EQ(opened.reader->Read(0, buffer, 64).status.code, StatusCode::Ok);
    CHECK_EQ(opened.reader->Read(2048, buffer, 128).status.code, StatusCode::Ok);

    const std::vector<SentRequest> sent = Sent(script);
    CHECK_EQ(sent.size(), std::size_t(3));
    for (const SentRequest& request : sent) {
        CHECK(Carries(request, "Authorization", kSecret));
        CHECK_EQ(request.headers.size(), std::size_t(1));
        // Added beside the protocol's own, never instead of them.
        if (request.method == Method::Get) {
            CHECK(!request.range.empty());
            CHECK_EQ(request.ifRange, std::string("\"v1\""));
        }
    }

    const std::vector<InterceptedRequest> asked = interceptor->Asked();
    CHECK_EQ(asked.size(), std::size_t(3));
    if (asked.size() == 3) {
        CHECK_EQ(asked[0].method, std::string("HEAD"));
        CHECK_EQ(asked[1].method, std::string("GET"));
        CHECK_EQ(asked[0].url, std::string(kUrl));
        CHECK_EQ(asked[0].scheme, std::string("https"));
        CHECK_EQ(asked[0].host, std::string("example.org"));
        // The effective port, not the URL's absent one.
        CHECK_EQ(asked[0].port, 443);
        CHECK_EQ(asked[0].redirectHop, 0);
        CHECK_EQ(asked[0].attempt, 1);
    }
}

void TestUserinfoIsNotShownToTheInterceptor() {
    // The view is the identity, not the wire URL: a credential an interceptor
    // did not supply is not one it is handed.
    auto interceptor = Bearer();
    auto script = MakeScript(Wellbehaved());
    const HttpOpenResult opened =
        OpenWith(script, With(interceptor), "https://user:pa55@example.org:8443/a.usda");
    CHECK(opened.reader != nullptr);
    const std::vector<InterceptedRequest> asked = interceptor->Asked();
    CHECK_EQ(asked.size(), std::size_t(1));
    if (!asked.empty()) {
        CHECK(asked[0].url.find("pa55") == std::string::npos);
        CHECK(asked[0].url.find("user") == std::string::npos);
        CHECK_EQ(asked[0].port, 8443);
    }
}

void TestSameOriginRedirectIsIntercepted() {
    auto interceptor = Bearer();
    auto script = MakeScript([](const TransportRequest& request, int index) {
        if (index == 0) return RedirectResponse("/moved/survey.copc");
        if (request.method == Method::Head) return MetadataResponse(kSize, "\"v1\"");
        return PartialResponse(request, FirstByte(request), kSize, "\"v1\"", 0x5A);
    });
    HttpOpenResult opened = OpenWith(script, With(interceptor));
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;
    unsigned char buffer[32];
    CHECK_EQ(opened.reader->Read(0, buffer, sizeof buffer).status.code, StatusCode::Ok);

    const std::vector<SentRequest> sent = Sent(script);
    CHECK_EQ(sent.size(), std::size_t(3));
    for (const SentRequest& request : sent) CHECK(Carries(request, "Authorization", kSecret));

    const std::vector<InterceptedRequest> asked = interceptor->Asked();
    CHECK_EQ(asked.size(), std::size_t(3));
    if (asked.size() == 3) {
        CHECK_EQ(asked[0].redirectHop, 0);
        CHECK_EQ(asked[1].redirectHop, 1);
        CHECK_EQ(asked[1].url, std::string("https://example.org/moved/survey.copc"));
        // A read starts where the open ended, and that is hop 0 of the read.
        CHECK_EQ(asked[2].redirectHop, 0);
    }
}

void TestOtherOriginsAreSentBare() {
    // Each of these is a different origin from https://example.org: another
    // host, another port, and the same host on the scheme's non-default port.
    // The hop is followed -- refusing it would break every CDN in front of an
    // object store -- and it carries nothing the interceptor said.
    const char* const elsewhere[] = {
        "https://cdn.example.net/survey.copc",
        "https://example.org:8443/survey.copc",
        "https://www.example.org/survey.copc",
    };
    for (const char* target : elsewhere) {
        auto interceptor = Bearer();
        const std::string location = target;
        auto script = MakeScript([location](const TransportRequest& request, int index) {
            if (index == 0) return RedirectResponse(location);
            if (request.method == Method::Head) return MetadataResponse(kSize, "\"v1\"");
            return PartialResponse(request, FirstByte(request), kSize, "\"v1\"", 0x5A);
        });
        HttpOpenResult opened = OpenWith(script, With(interceptor));
        CHECK(opened.reader != nullptr);
        if (!opened.reader) continue;
        unsigned char buffer[32];
        CHECK_EQ(opened.reader->Read(0, buffer, sizeof buffer).status.code,
                 StatusCode::Ok);
        CHECK_EQ(opened.reader->Read(100, buffer, sizeof buffer).status.code,
                 StatusCode::Ok);

        const std::vector<SentRequest> sent = Sent(script);
        CHECK_EQ(sent.size(), std::size_t(4));
        if (sent.size() != 4) continue;
        // The first request went to the origin, and carried the header.
        CHECK(Carries(sent[0], "Authorization", kSecret));
        // The hop, and every read after it, went elsewhere and carried nothing.
        for (std::size_t i = 1; i < sent.size(); ++i) {
            CHECK_EQ(sent[i].url.rfind(location, 0), std::size_t(0));
            CHECK(sent[i].headers.empty());
        }
        // Not asked either: an interceptor is never consulted about a request
        // it may not decorate, so it cannot be talked into it.
        CHECK_EQ(interceptor->Asked().size(), std::size_t(1));
    }
}

void TestPlaintextOriginIsADifferentOrigin() {
    // http://example.org and https://example.org are two origins. An `http`
    // open redirected to `https` is an upgrade the backend follows, and the
    // first origin's credential does not follow it.
    auto interceptor = Bearer();
    auto script = MakeScript([](const TransportRequest&, int index) {
        if (index == 0) return RedirectResponse("https://example.org/survey.copc");
        return MetadataResponse(kSize, "\"v1\"");
    });
    const HttpOpenResult opened =
        OpenWith(script, With(interceptor), "http://example.org/survey.copc");
    CHECK(opened.reader != nullptr);
    const std::vector<SentRequest> sent = Sent(script);
    CHECK_EQ(sent.size(), std::size_t(2));
    if (sent.size() == 2) {
        CHECK(Carries(sent[0], "Authorization", kSecret));
        CHECK(sent[1].headers.empty());
    }
}

void TestEachAttemptIsAskedAgain() {
    // A `503` and its retry: the provider is asked twice, told which attempt
    // each is, and its second answer is the one sent the second time.
    auto interceptor = std::make_shared<Recording>(
        [](const InterceptedRequest& request, std::vector<RequestHeader>* out) {
            out->push_back({"Authorization", "Bearer t" + std::to_string(request.attempt)});
            return true;
        });
    auto script = MakeScript([](const TransportRequest&, int index) {
        if (index == 0) {
            TransportResponse busy;
            busy.status = 503;
            busy.connected = true;
            return busy;
        }
        return MetadataResponse(kSize, "\"v1\"");
    });
    const HttpOpenResult opened = OpenWith(script, With(interceptor));
    CHECK(opened.reader != nullptr);
    const std::vector<SentRequest> sent = Sent(script);
    CHECK_EQ(sent.size(), std::size_t(2));
    if (sent.size() == 2) {
        CHECK(Carries(sent[0], "Authorization", "Bearer t1"));
        CHECK(Carries(sent[1], "Authorization", "Bearer t2"));
    }
}

// --- declining ---------------------------------------------------------------

void TestDeclineSendsNothing() {
    auto declining = std::make_shared<Recording>(
        [](const InterceptedRequest&, std::vector<RequestHeader>*) { return false; });
    auto script = MakeScript(Wellbehaved());
    const HttpOpenResult opened = OpenWith(script, With(declining));
    CHECK(opened.reader == nullptr);
    CHECK_EQ(opened.status.code, StatusCode::AccessDenied);
    // Not sent, and not retried: a retry asks the same provider the same
    // question.
    CHECK_EQ(script->Count(), 0);
    CHECK_EQ(declining->Asked().size(), std::size_t(1));
}

void TestDeclineMidReadFailsTheRead() {
    // A provider that admits the open and then stops -- a token revoked while
    // the asset was open. The read fails as `AccessDenied`, with no request.
    std::atomic<int> calls{0};
    auto interceptor = std::make_shared<Recording>(
        [&calls](const InterceptedRequest&, std::vector<RequestHeader>* out) {
            if (calls.fetch_add(1) > 0) return false;
            out->push_back({"Authorization", kSecret});
            return true;
        });
    auto script = MakeScript(Wellbehaved());
    HttpOpenResult opened = OpenWith(script, With(interceptor));
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;
    unsigned char buffer[32];
    const ReadResult read = opened.reader->Read(0, buffer, sizeof buffer);
    CHECK_EQ(read.status.code, StatusCode::AccessDenied);
    CHECK_EQ(read.bytesRead, std::size_t(0));
    CHECK_EQ(script->Count(), 1);
    CHECK_EQ(calls.load(), 2);
}

void TestAnExceptionIsADecline() {
    auto throwing = std::make_shared<Recording>(
        [](const InterceptedRequest&, std::vector<RequestHeader>*) -> bool {
            throw std::runtime_error(std::string("token endpoint said ") + kSecret);
        });
    auto script = MakeScript(Wellbehaved());
    const HttpOpenResult opened = OpenWith(script, With(throwing));
    CHECK_EQ(opened.status.code, StatusCode::AccessDenied);
    CHECK_EQ(script->Count(), 0);
    // The exception's text is not repeated.
    CHECK(usdasset::ToString(opened.status).find("s3cret") == std::string::npos);
}

// --- what may be added -------------------------------------------------------

void TestTheHeaderRule() {
    // Admitted: what the seam is for.
    CHECK(IsInterceptableHeader("Authorization", "Bearer abc"));
    CHECK(IsInterceptableHeader("Cookie", "session=1"));
    CHECK(IsInterceptableHeader("X-Amz-Date", "20261103T000000Z"));
    CHECK(IsInterceptableHeader("x-amz-content-sha256", "UNSIGNED-PAYLOAD"));
    CHECK(IsInterceptableHeader("Proxy-Authorization", "Basic Zm9vOmJhcg=="));
    CHECK(IsInterceptableHeader("Authorization", "AWS4-HMAC-SHA256\tCredential=x"));

    // Refused: owned by the protocol, in any case.
    const char* const reserved[] = {
        "Range",           "If-Range",          "If-Match",
        "If-None-Match",   "If-Modified-Since", "If-Unmodified-Since",
        "Host",            "Accept-Encoding",   "TE",
        "Transfer-Encoding", "Content-Length",  "Content-Encoding",
        "Connection",      "Keep-Alive",        "Proxy-Connection",
        "Upgrade",         "Expect",            "Trailer",
        "User-Agent",      "RANGE",             "if-range",
    };
    for (const char* name : reserved) CHECK(!IsInterceptableHeader(name, "x"));

    // Refused: not a token, or a value that could split the request.
    CHECK(!IsInterceptableHeader("", "x"));
    CHECK(!IsInterceptableHeader("Authorization: Bearer", "x"));
    CHECK(!IsInterceptableHeader("Bad Name", "x"));
    CHECK(!IsInterceptableHeader("X-A\r\nX-B", "x"));
    CHECK(!IsInterceptableHeader("Authorization", ""));
    CHECK(!IsInterceptableHeader("Authorization", "a\r\nRange: bytes=0-"));
    CHECK(!IsInterceptableHeader("Authorization", "a\nb"));
    CHECK(!IsInterceptableHeader("Authorization", std::string("a\0b", 3)));
    CHECK(!IsInterceptableHeader("Authorization", "a\x7f"));
    // Prefixes and extensions of a reserved name are other names.
    CHECK(IsInterceptableHeader("Range-Token", "x"));
    CHECK(IsInterceptableHeader("Hos", "x"));
}

/// Opens with an interceptor that adds `headers`, and returns the status.
usdasset::Status OpenAdding(std::vector<RequestHeader> headers,
                            int* requestsOut = nullptr) {
    auto script = MakeScript(Wellbehaved());
    const HttpOpenResult opened = OpenWith(script, With(Adding(std::move(headers))));
    if (requestsOut != nullptr) *requestsOut = script->Count();
    return opened.status;
}

void TestRefusedHeadersAreNotSent() {
    {
        int requests = -1;
        const usdasset::Status status = OpenAdding({{"Range", "bytes=0-"}}, &requests);
        CHECK_EQ(status.code, StatusCode::InvalidArgument);
        CHECK_EQ(requests, 0);
        // A reserved name is ours, and is named.
        CHECK(status.message.find("range") != std::string::npos);
    }
    {
        // A malformed name may be a whole line pasted into the name field, and
        // it is not repeated.
        int requests = -1;
        const usdasset::Status status =
            OpenAdding({{std::string("Authorization: ") + kSecret, "x"}}, &requests);
        CHECK_EQ(status.code, StatusCode::InvalidArgument);
        CHECK_EQ(requests, 0);
        CHECK(usdasset::ToString(status).find("s3cret") == std::string::npos);
    }
    {
        // A value that would end its header and start another: refused whole,
        // and not repeated.
        int requests = -1;
        const usdasset::Status status = OpenAdding(
            {{"Authorization", std::string(kSecret) + "\r\nRange: bytes=0-"}}, &requests);
        CHECK_EQ(status.code, StatusCode::InvalidArgument);
        CHECK_EQ(requests, 0);
        CHECK(usdasset::ToString(status).find("s3cret") == std::string::npos);
    }
    {
        // Twice, in two spellings, is twice.
        int requests = -1;
        const usdasset::Status status = OpenAdding(
            {{"Authorization", "Bearer a"}, {"authorization", "Bearer b"}}, &requests);
        CHECK_EQ(status.code, StatusCode::InvalidArgument);
        CHECK_EQ(requests, 0);
    }
}

void TestTheByteBound() {
    // `name: value\r\n` is the unit. Exactly at the bound is admitted, and one
    // byte past it is not.
    const std::string name = "X-Token";
    const std::size_t overhead = name.size() + 4;
    {
        int requests = -1;
        const usdasset::Status status = OpenAdding(
            {{name, std::string(kMaxInterceptedHeaderBytes - overhead, 'a')}}, &requests);
        CHECK_EQ(status.code, StatusCode::Ok);
        CHECK_EQ(requests, 1);
    }
    {
        int requests = -1;
        const usdasset::Status status = OpenAdding(
            {{name, std::string(kMaxInterceptedHeaderBytes - overhead + 1, 'a')}},
            &requests);
        CHECK_EQ(status.code, StatusCode::InvalidArgument);
        CHECK_EQ(requests, 0);
    }
}

// --- ordering against the destination policy --------------------------------

void TestThePolicyJudgesFirst() {
    {
        // An instance-metadata literal at the first hop: refused by the policy,
        // and the interceptor is never asked.
        auto interceptor = Bearer();
        auto script = MakeScript(Wellbehaved());
        const HttpOpenResult opened = OpenWith(
            script, With(interceptor), "http://169.254.169.254/latest/meta-data/");
        CHECK_EQ(opened.status.code, StatusCode::AccessDenied);
        CHECK_EQ(script->Count(), 0);
        CHECK(interceptor->Asked().empty());
    }
    {
        // And a same-origin open whose host is refused at connect time: the
        // transport's half of the policy. The header was built, and it went
        // nowhere, because the transport refuses before any socket exists.
        auto interceptor = Bearer();
        auto script = MakeScript([](const TransportRequest&, int) {
            TransportResponse refused;
            refused.error = TransportError::DestinationRefused;
            return refused;
        });
        const HttpOpenResult opened = OpenWith(script, With(interceptor));
        CHECK_EQ(opened.status.code, StatusCode::AccessDenied);
        CHECK_EQ(script->Count(), 1);
    }
}

// --- never repeated ----------------------------------------------------------

void TestNoValueReachesAMessage() {
    // Every failure an open with a credential can end in, rendered, and
    // searched for the credential.
    const int statuses[] = {401, 403, 404, 500, 503};
    for (const int code : statuses) {
        auto script = MakeScript([code](const TransportRequest&, int) {
            TransportResponse response;
            response.status = code;
            response.connected = true;
            return response;
        });
        const HttpOpenResult opened = OpenWith(script, With(Bearer()));
        CHECK(!opened.status.IsOk());
        CHECK(usdasset::ToString(opened.status).find("s3cret") == std::string::npos);
    }
    {
        auto script = MakeScript(Wellbehaved());
        HttpOpenResult opened = OpenWith(script, With(Bearer()));
        CHECK(opened.reader != nullptr);
        if (opened.reader) {
            CHECK(opened.reader->Metadata().resolvedIdentifier.find("s3cret") ==
                  std::string::npos);
        }
    }
}

// --- concurrency -------------------------------------------------------------

void TestConcurrentReadsShareOneInterceptor() {
    // The interceptor is called from every reading thread at once; this is the
    // case the TSan lane is for.
    auto interceptor = Bearer();
    auto script = MakeScript(Wellbehaved());
    HttpOpenResult opened = OpenWith(script, With(interceptor));
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    constexpr int kThreads = 8;
    constexpr int kReads = 16;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            unsigned char buffer[32];
            for (int i = 0; i < kReads; ++i) {
                const std::uint64_t offset = static_cast<std::uint64_t>((t * kReads + i) * 32);
                if (!opened.reader->Read(offset, buffer, sizeof buffer).status.IsOk()) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& thread : threads) thread.join();

    CHECK_EQ(failures.load(), 0);
    CHECK_EQ(interceptor->Asked().size(), std::size_t(1 + kThreads * kReads));
    for (const SentRequest& request : Sent(script)) {
        CHECK(Carries(request, "Authorization", kSecret));
    }
}

}  // namespace

int main() {
    TestNoInterceptorAddsNothing();
    TestEveryRequestToTheOriginCarriesTheHeaders();
    TestUserinfoIsNotShownToTheInterceptor();
    TestSameOriginRedirectIsIntercepted();
    TestOtherOriginsAreSentBare();
    TestPlaintextOriginIsADifferentOrigin();
    TestEachAttemptIsAskedAgain();
    TestDeclineSendsNothing();
    TestDeclineMidReadFailsTheRead();
    TestAnExceptionIsADecline();
    TestTheHeaderRule();
    TestRefusedHeadersAreNotSent();
    TestTheByteBound();
    TestThePolicyJudgesFirst();
    TestNoValueReachesAMessage();
    TestConcurrentReadsShareOneInterceptor();
    return usdassettest::Report("usdAssetHttp/interception");
}
