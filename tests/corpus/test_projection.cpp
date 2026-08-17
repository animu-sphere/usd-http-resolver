// SPDX-License-Identifier: Apache-2.0
//
// Which corpus behavior produces which `StatusCode`.
//
// This is the file the two halves of `v0.2.0` were built to meet in. The
// hostile server stood up first and proved, over a raw socket, that each named
// behavior puts on the wire what its name claims; the backend was written
// against that. Neither knows the other: nothing in `tests/fixture-server` has
// ever heard of `StatusCode`, and nothing in `usdAssetHttp` has heard of
// `Behavior`. A disagreement here is therefore evidence rather than a
// tautology, which is the whole reason the order in §17 of the design policy
// was not negotiable.
//
// The table below is the release's central claim in executable form: every
// condition §11.2 names, projected onto a code a caller can act on. It is not
// the read contract -- that is `tests/boundary`, over an oracle, and no server
// is involved in it.

#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "usdAssetHttp/HttpAssetReader.h"
#include "usdAssetIo/Diagnostics.h"
#include "usdassetfixture/Corpus.h"
#include "usdassetfixture/Server.h"

#include "Check.h"

namespace {

using usdasset::ReadResult;
using usdasset::StatusCode;
using usdasset::StatusCodeName;
using usdasset::http::HttpOpenResult;
using usdasset::http::HttpOptions;
using usdassetfixture::AssetSpec;
using usdassetfixture::Behavior;
using usdassetfixture::BehaviorName;
using usdassetfixture::RequestRecord;
using usdassetfixture::Server;

/// Which behaviors a case has actually exercised. Checked against
/// `AllBehaviors()` at the end, so that a behavior added to the corpus without
/// a projection added beside it fails the run rather than silently widening
/// what this file claims to cover.
std::set<Behavior> g_exercised;

constexpr std::size_t kSize = 4096;

/// Deadlines short enough that a stall is a test that takes half a second
/// rather than one that takes half a minute. They are options rather than
/// constants precisely so that this file can say so.
HttpOptions FastOptions() {
    HttpOptions options;
    options.connectTimeoutMs = 2000;
    options.responseTimeoutMs = 400;
    options.transferTimeoutMs = 600;
    options.maxRedirects = 3;
    options.maxAttempts = 3;
    return options;
}

/// Positional content: every byte is a function of its offset, so a read
/// landing one byte off returns data that is obviously from elsewhere rather
/// than plausibly correct.
std::vector<unsigned char> MakeContent(std::size_t size) {
    std::vector<unsigned char> content(size);
    for (std::size_t i = 0; i < size; ++i) {
        content[i] = static_cast<unsigned char>((i * 31u + 7u) & 0xffu);
    }
    return content;
}

AssetSpec MakeSpec(const std::string& path, Behavior behavior) {
    AssetSpec spec;
    spec.path = path;
    spec.content = MakeContent(kSize);
    spec.behavior = behavior;
    spec.etag = "\"rev-a\"";
    spec.lastModified = "Sat, 16 Aug 2026 10:00:00 GMT";
    return spec;
}

const char* Name(StatusCode code) { return StatusCodeName(code); }

void ReportCode(Behavior behavior, const char* stage, StatusCode actual,
                StatusCode expected) {
    std::fprintf(stderr, "FAIL [%s] %s: %s, expected %s\n", BehaviorName(behavior),
                 stage, Name(actual), Name(expected));
    ++usdassettest::FailureCount();
}

/// `HTTP006` is required to name which deadline elapsed: a caller staring at
/// "timeout" cannot tell a firewall from a slow origin. The message is printed
/// on failure rather than only asserted, because "the substring was absent" is
/// not enough to tell which deadline the backend thought had elapsed.
void ExpectDeadlineNamed(Behavior behavior, const usdasset::Status& status,
                         const char* which) {
    if (status.message.find(which) != std::string::npos) return;
    std::fprintf(stderr, "FAIL [%s] expected the message to name the %s: %s\n",
                 BehaviorName(behavior), which, status.message.c_str());
    ++usdassettest::FailureCount();
}

/// Asserts that opening this behavior's asset fails with `expected`, and
/// nothing is handed back.
void ExpectOpenFails(Server& server, Behavior behavior, const std::string& path,
                     StatusCode expected) {
    g_exercised.insert(behavior);
    server.Serve(MakeSpec(path, behavior));

    const HttpOpenResult opened =
        usdasset::http::Open(server.Url(path), FastOptions());
    if (opened.status.code != expected) {
        ReportCode(behavior, "open", opened.status.code, expected);
        return;
    }
    // A reader is never returned in a state where its size or range support is
    // unknown, so a failing open returns none at all.
    CHECK(opened.reader == nullptr);
}

/// Asserts that opening succeeds and that a read of `length` at `offset` then
/// fails with `expected`, having handed back no complete data.
void ExpectReadFails(Server& server, Behavior behavior, const std::string& path,
                     StatusCode expected, std::uint64_t offset = 0,
                     std::size_t length = 256) {
    g_exercised.insert(behavior);
    server.Serve(MakeSpec(path, behavior));

    HttpOpenResult opened = usdasset::http::Open(server.Url(path), FastOptions());
    if (!opened.reader) {
        std::fprintf(stderr, "FAIL [%s] open failed: %s\n", BehaviorName(behavior),
                     usdasset::ToString(opened.status).c_str());
        ++usdassettest::FailureCount();
        return;
    }

    std::vector<unsigned char> buffer(length, 0);
    const ReadResult read = opened.reader->Read(offset, buffer.data(), length);
    if (read.status.code != expected) {
        ReportCode(behavior, "read", read.status.code, expected);
        return;
    }
    // The property that matters more than the code: a failed read never
    // reports a complete transfer. Whatever went wrong, the caller is not
    // handed a buffer it believes is full.
    CHECK(read.bytesRead < length);
}

// --- the behaviors that read correctly ---------------------------------------

void TestNormal(Server& server) {
    g_exercised.insert(Behavior::Normal);
    const std::string path = "/normal";
    const AssetSpec spec = MakeSpec(path, Behavior::Normal);
    server.Serve(spec);

    HttpOpenResult opened = usdasset::http::Open(server.Url(path), FastOptions());
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    CHECK_EQ(opened.reader->Metadata().size, static_cast<std::uint64_t>(kSize));
    CHECK(opened.reader->Metadata().supportsRandomAccess);
    // A strong `ETag` is what this fixture offers, so the identity is Stable.
    CHECK_EQ(opened.reader->Metadata().stability, usdasset::IdentityStability::Stable);

    // A read from the middle, checked byte for byte against the content the
    // fixture was given.
    std::vector<unsigned char> buffer(300, 0);
    const ReadResult read = opened.reader->Read(1000, buffer.data(), 300);
    CHECK_EQ(read.status.code, StatusCode::Ok);
    CHECK_EQ(read.bytesRead, std::size_t(300));
    for (std::size_t i = 0; i < 300; ++i) {
        CHECK_EQ(buffer[i], spec.content[1000 + i]);
    }

    // Truncation at EOF is success, not a short read. The single most important
    // distinction in DIAGNOSTICS.md, asserted here against a real server as
    // well as against the oracle.
    std::vector<unsigned char> tail(64, 0);
    const ReadResult atEnd = opened.reader->Read(kSize - 10, tail.data(), 64);
    CHECK_EQ(atEnd.status.code, StatusCode::Ok);
    CHECK_EQ(atEnd.bytesRead, std::size_t(10));

    // Every range request after open carries the conditional guard. This is a
    // property of what was *sent*, and the request log is the only place it can
    // be read.
    const std::vector<RequestRecord> log = server.Log();
    int ranged = 0;
    for (const RequestRecord& record : log) {
        if (record.method != "GET" || record.range.empty()) continue;
        ++ranged;
        CHECK_EQ(record.ifRange, spec.etag);
    }
    CHECK(ranged >= 2);
    // And the metadata request was one round trip that read no content.
    CHECK_EQ(log.front().method, std::string("HEAD"));
}

void TestRedirectChain(Server& server) {
    g_exercised.insert(Behavior::RedirectChain);
    const std::string path = "/chain";
    AssetSpec spec = MakeSpec(path, Behavior::RedirectChain);
    spec.redirectHops = 2;
    server.Serve(spec);
    server.ClearLog();

    HttpOpenResult opened = usdasset::http::Open(server.Url(path), FastOptions());
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    // Followed by this repository's own counter, never by the client library's:
    // a chain the library follows silently is a chain this test cannot see.
    CHECK_EQ(opened.reader->Metrics().Snapshot().redirectCount, 2u);
    // The identity is where the chain ended, not where it started.
    CHECK(opened.reader->Metadata().resolvedIdentifier.find(".hop2") !=
          std::string::npos);

    std::vector<unsigned char> buffer(128, 0);
    CHECK_EQ(opened.reader->Read(0, buffer.data(), 128).status.code, StatusCode::Ok);
}

void TestTransientServerError(Server& server) {
    g_exercised.insert(Behavior::TransientServerError);
    const std::string path = "/flaky";
    AssetSpec spec = MakeSpec(path, Behavior::TransientServerError);
    spec.transientFailures = 1;
    server.Serve(spec);
    server.ClearLog();

    HttpOpenResult opened = usdasset::http::Open(server.Url(path), FastOptions());
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    // Bounded retry is in scope per §4.1, and a retry that is not visible in
    // metrics is a defect per DIAGNOSTICS.md §3. A silent retry that succeeded
    // still cost a round trip.
    CHECK_EQ(opened.reader->Metrics().Snapshot().retryCount, 1u);
    CHECK_EQ(opened.reader->Metrics().Snapshot().requestCount, 2u);

    std::vector<unsigned char> buffer(64, 0);
    CHECK_EQ(opened.reader->Read(0, buffer.data(), 64).status.code, StatusCode::Ok);
}

// --- the behaviors that fail, and how ----------------------------------------

void TestRangeSupport(Server& server) {
    // The honest server that simply cannot serve ranges: caught at open, from
    // the absence of `Accept-Ranges`.
    ExpectOpenFails(server, Behavior::NoAcceptRanges, "/no-ranges",
                    StatusCode::RangeNotSupported);

    // Nastier: it advertises byte ranges and then ignores them. Not catchable
    // at open without a second round trip that ASSET_READER.md §2 forbids, so
    // it surfaces on the first read -- with the same code, because ADR-0002
    // makes both terminal and the caller does the same thing about each.
    ExpectReadFails(server, Behavior::IgnoresRange, "/ignores-range",
                    StatusCode::RangeNotSupported);

    // A `416` for a range that lies inside the size the server itself reported
    // at open. The response is not usable and the request was not wrong.
    ExpectReadFails(server, Behavior::RangeNotSatisfiable, "/unsatisfiable",
                    StatusCode::InvalidResponse);
}

void TestFraming(Server& server) {
    // Honest headers, a body that stops early, an orderly close. Never a hole
    // in the caller's buffer.
    ExpectReadFails(server, Behavior::TruncatedBody, "/truncated",
                    StatusCode::InvalidResponse);

    // Internally consistent and wrong against the request. At least two bytes,
    // because `Content-Range` cannot describe a range shorter than one and the
    // corpus states that floor beside the enumerator.
    ExpectReadFails(server, Behavior::ContentRangeTooShort, "/short-range",
                    StatusCode::InvalidResponse, 0, 256);

    // The right length at the wrong offset, caught by a different check.
    ExpectReadFails(server, Behavior::ContentRangeShifted, "/shifted-range",
                    StatusCode::InvalidResponse, 0, 256);

    // No `Content-Length` and no `Transfer-Encoding`: the size is unknowable
    // without downloading the asset, which is what a range backend must not do.
    // ADR-0003 required a client that can refuse this rather than one that
    // helpfully accumulates until EOF.
    ExpectOpenFails(server, Behavior::UnknownContentLength, "/unknown-length",
                    StatusCode::InvalidResponse);
}

void TestValidatorChange(Server& server) {
    g_exercised.insert(Behavior::ValidatorChangeMidRead);
    const std::string path = "/moving";
    AssetSpec spec = MakeSpec(path, Behavior::ValidatorChangeMidRead);
    // The metadata request at open is the one request answered from the first
    // revision; everything after it sees the second.
    spec.changeAfterRequests = 1;
    // Left empty on purpose: the new revision reuses the current bytes, so it
    // differs only in identity. A backend comparing bytes rather than
    // validators sees nothing here.
    spec.revisedContent.clear();
    server.Serve(spec);
    server.ClearLog();

    HttpOpenResult opened = usdasset::http::Open(server.Url(path), FastOptions());
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    std::vector<unsigned char> buffer(256, 0);
    const ReadResult read = opened.reader->Read(0, buffer.data(), 256);
    if (read.status.code != StatusCode::AssetChanged) {
        ReportCode(Behavior::ValidatorChangeMidRead, "read", read.status.code,
                   StatusCode::AssetChanged);
        return;
    }
    // No bytes. Those belong to a revision this reader is not bound to.
    CHECK_EQ(read.bytesRead, std::size_t(0));

    // It stays changed. A reader that recovers on the next call has rebound to
    // the new revision, which is the failure the code exists to prevent.
    const ReadResult again = opened.reader->Read(512, buffer.data(), 256);
    CHECK_EQ(again.status.code, StatusCode::AssetChanged);
    // And the metadata it reports is still the revision it bound to.
    CHECK_EQ(opened.reader->Metadata().size, static_cast<std::uint64_t>(kSize));

    // The guard was actually on the wire. Without this the case above could
    // pass for the wrong reason.
    bool sawConditional = false;
    for (const RequestRecord& record : server.Log()) {
        if (!record.range.empty() && !record.ifRange.empty()) sawConditional = true;
    }
    CHECK(sawConditional);
}

void TestRedirectLoop(Server& server) {
    g_exercised.insert(Behavior::RedirectLoop);
    const std::string path = "/loop";
    server.Serve(MakeSpec(path, Behavior::RedirectLoop));
    server.ClearLog();

    const HttpOptions options = FastOptions();
    const HttpOpenResult opened = usdasset::http::Open(server.Url(path), options);
    CHECK_EQ(opened.status.code, StatusCode::InvalidResponse);

    // The server never stops redirecting; what stops it is this repository's
    // counter, and the request log is how that counter is proven to exist. One
    // request per hop, plus the one that discovered the hop past the bound.
    CHECK_EQ(static_cast<int>(server.RequestCount()), options.maxRedirects + 1);
}

void TestDeadlines(Server& server) {
    // The connect deadline succeeded and the response deadline did not.
    {
        g_exercised.insert(Behavior::SlowHeaders);
        AssetSpec spec = MakeSpec("/slow-headers", Behavior::SlowHeaders);
        spec.delayMs = 1500;
        server.Serve(spec);

        const HttpOpenResult opened =
            usdasset::http::Open(server.Url("/slow-headers"), FastOptions());
        CHECK_EQ(opened.status.code, StatusCode::Timeout);
        ExpectDeadlineNamed(Behavior::SlowHeaders, opened.status, "response deadline");
    }

    // Headers, one byte, then a stall. A separate row because a backend with
    // only a total deadline cannot tell it from the one above.
    {
        g_exercised.insert(Behavior::SlowBody);
        AssetSpec spec = MakeSpec("/slow-body", Behavior::SlowBody);
        spec.delayMs = 1500;
        server.Serve(spec);

        HttpOpenResult opened =
            usdasset::http::Open(server.Url("/slow-body"), FastOptions());
        CHECK(opened.reader != nullptr);
        if (!opened.reader) return;

        std::vector<unsigned char> buffer(256, 0);
        const ReadResult read = opened.reader->Read(0, buffer.data(), 256);
        CHECK_EQ(read.status.code, StatusCode::Timeout);
        ExpectDeadlineNamed(Behavior::SlowBody, read.status, "transfer deadline");
    }
}

void TestConnectionResets(Server& server) {
    // Nothing came back at all: a transport fault, and not an absent asset.
    ExpectOpenFails(server, Behavior::ConnectionResetBeforeResponse, "/reset-early",
                    StatusCode::NetworkError);

    // Headers, some body, then `RST`.
    //
    // The code is not pinned to one value, and that is deliberate rather than
    // lax. What an `RST` destroys is the platform's decision: Linux delivers
    // buffered body bytes and Winsock discards them, and whether a peer sees
    // `ECONNRESET` or an orderly EOF follows from the same decision. The corpus
    // records this in its own known gaps. What is portable -- and what a
    // backend must get right -- is that the promise in the headers was not
    // kept, the read fails, and no complete buffer is handed back.
    g_exercised.insert(Behavior::ConnectionResetMidBody);
    const std::string path = "/reset-mid";
    AssetSpec spec = MakeSpec(path, Behavior::ConnectionResetMidBody);
    spec.deliverBytes = 8;
    server.Serve(spec);

    HttpOpenResult opened = usdasset::http::Open(server.Url(path), FastOptions());
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;

    std::vector<unsigned char> buffer(256, 0);
    const ReadResult read = opened.reader->Read(0, buffer.data(), 256);
    const bool acceptable = read.status.code == StatusCode::NetworkError ||
                            read.status.code == StatusCode::InvalidResponse;
    if (!acceptable) {
        ReportCode(Behavior::ConnectionResetMidBody, "read", read.status.code,
                   StatusCode::NetworkError);
        return;
    }
    CHECK(read.bytesRead < 256);
}

void TestAbsenceAndRefusal(Server& server) {
    // Separate codes, because one is a scene problem and the other is a
    // configuration problem, and an unreachable server is neither
    // (DIAGNOSTICS.md §4.4).
    ExpectOpenFails(server, Behavior::NotFound, "/missing", StatusCode::NotFound);
    ExpectOpenFails(server, Behavior::AccessDenied, "/forbidden",
                    StatusCode::AccessDenied);
}

// --- credentials --------------------------------------------------------------

void TestNoCredentialInAnyMessage(Server& server) {
    // §4.3 of the design policy, asserted rather than assumed. A failing open
    // is where a URL is most likely to be pasted into a message whole.
    const std::string path = "/missing-secret";
    server.Serve(MakeSpec(path, Behavior::NotFound));

    const std::string url = "http://user:s3cret@127.0.0.1:" +
                            std::to_string(server.Port()) + path + "?token=abc123";
    const HttpOpenResult opened = usdasset::http::Open(url, FastOptions());
    CHECK_EQ(opened.status.code, StatusCode::NotFound);

    const std::string rendered = usdasset::ToString(opened.status);
    CHECK(rendered.find("s3cret") == std::string::npos);
    CHECK(rendered.find("abc123") == std::string::npos);
    CHECK(rendered.find("user") == std::string::npos);
}

void TestCoverage() {
    // The projection claims to cover the corpus. This is what makes the claim
    // checkable: a behavior added to the enum without a projection added beside
    // it fails the run instead of silently widening what this file asserts.
    for (const Behavior behavior : usdassetfixture::AllBehaviors()) {
        if (g_exercised.find(behavior) == g_exercised.end()) {
            std::fprintf(stderr, "FAIL [coverage] behavior '%s' has no projection\n",
                         BehaviorName(behavior));
            ++usdassettest::FailureCount();
        }
    }
}

}  // namespace

int main() {
    std::string error;
    std::unique_ptr<Server> server = Server::Start(&error);
    if (!server) {
        // Reported as what it is. A test that cannot bind loopback must say so
        // rather than report a backend failure.
        std::fprintf(stderr, "FAIL: the fixture server could not bind loopback: %s\n",
                     error.c_str());
        return 1;
    }

    TestNormal(*server);
    TestRedirectChain(*server);
    TestTransientServerError(*server);
    TestRangeSupport(*server);
    TestFraming(*server);
    TestValidatorChange(*server);
    TestRedirectLoop(*server);
    TestDeadlines(*server);
    TestConnectionResets(*server);
    TestAbsenceAndRefusal(*server);
    TestNoCredentialInAnyMessage(*server);
    TestCoverage();

    server->Stop();
    return usdassettest::Report("usdAssetHttp/corpus-projection");
}
