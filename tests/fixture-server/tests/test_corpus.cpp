// SPDX-License-Identifier: Apache-2.0
//
// The corpus, checked against itself before any backend is pointed at it.
//
// §17 of the design policy requires the fixture server to stand up before the
// HTTP backend, "so that the backend is written against a passing oracle rather
// than debugged against a server". This file is what makes that sentence mean
// something: it asserts, over a raw socket and an independent parser, that each
// named behavior puts on the wire exactly what its name claims. A corpus that
// has not been checked this way is not an oracle -- it is a second unknown, and
// the first argument about a failing range read is then about which of the two
// is wrong.
//
// Every assertion here is about *bytes on the wire*. Nothing in this file knows
// what `InvalidResponse` is, and it must stay that way: the moment the corpus
// starts asserting the backend's interpretation, the two stop being independent.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "usdassetfixture/Corpus.h"
#include "usdassetfixture/Server.h"

#include "Check.h"
#include "RawClient.h"

namespace {

using usdassetfixture::AssetSpec;
using usdassetfixture::Behavior;
using usdassetfixture::BehaviorName;
using usdassetfixture::Server;
using usdassetfixturetest::CaseScope;
using usdassetfixturetest::FetchOnce;
using usdassetfixturetest::GetRequest;
using usdassetfixturetest::HeadRequest;
using usdassetfixturetest::RawClient;
using usdassetfixturetest::RawResponse;
using usdassetfixturetest::ResponseEnd;

/// Which behaviors a case has actually exercised. Checked against
/// `AllBehaviors()` at the end, so that adding an enumerator without adding a
/// case fails the run instead of silently widening a coverage claim.
std::set<Behavior> g_exercised;

void Exercised(Behavior behavior) { g_exercised.insert(behavior); }

/// Deterministic, and not a constant: a fixture of one repeated byte cannot
/// catch a server that serves the right length from the wrong offset.
std::vector<unsigned char> MakeContent(std::size_t size) {
    std::vector<unsigned char> content(size);
    for (std::size_t i = 0; i < size; ++i) {
        content[i] = static_cast<unsigned char>((i * 31u + 7u) & 0xffu);
    }
    return content;
}

bool BodyEquals(const RawResponse& response,
                const std::vector<unsigned char>& content,
                std::size_t first,
                std::size_t length) {
    if (response.body.size() != length) return false;
    for (std::size_t i = 0; i < length; ++i) {
        if (response.body[i] != content[first + i]) return false;
    }
    return true;
}

constexpr std::size_t kSize = 256;

/// Log entries whose target begins with `prefix`.
///
/// Counted by target rather than taken from `RequestCount()`, because one
/// fixture is deliberately abandoned: the impatient half of the SlowHeaders
/// case gives up after 50ms and the server logs that request when its stall
/// ends, half a second later. A count of everything would be a count of when
/// that straggler happened to land.
std::size_t CountLogged(const Server& server, const std::string& prefix) {
    std::size_t count = 0;
    for (const auto& record : server.Log()) {
        if (record.target.compare(0, prefix.size(), prefix) == 0) ++count;
    }
    return count;
}

// --- the correct row ---------------------------------------------------------

void TestNormal(Server& server, const std::vector<unsigned char>& content) {
    CaseScope scope(BehaviorName(Behavior::Normal));
    Exercised(Behavior::Normal);

    const unsigned short port = server.Port();

    // HEAD states the size, advertises ranges, and carries the validator.
    {
        const RawResponse head =
            FetchOnce(port, HeadRequest("/normal"), 5000, /*expectBody=*/false);
        CHECK_STATUS(head, 200);
        CHECK_EQ(head.Header("Accept-Ranges"), std::string("bytes"));
        CHECK_EQ(head.Header("Content-Length"), std::to_string(kSize));
        CHECK_EQ(head.Header("ETag"), std::string("\"v1\""));
        CHECK(head.body.empty());
    }

    // A GET with no Range is the whole asset.
    {
        const RawResponse whole = FetchOnce(port, GetRequest("/normal"));
        CHECK_STATUS(whole, 200);
        CHECK(!whole.HasHeader("Content-Range"));
        CHECK(BodyEquals(whole, content, 0, kSize));
    }

    // The ordinary range.
    {
        const RawResponse ranged =
            FetchOnce(port, GetRequest("/normal", {{"Range", "bytes=10-19"}}));
        CHECK_STATUS(ranged, 206);
        CHECK_EQ(ranged.Header("Content-Range"),
                 std::string("bytes 10-19/") + std::to_string(kSize));
        CHECK_EQ(ranged.Header("Content-Length"), std::string("10"));
        CHECK(BodyEquals(ranged, content, 10, 10));
    }

    // The open form runs to EOF.
    {
        const RawResponse open =
            FetchOnce(port, GetRequest("/normal", {{"Range", "bytes=240-"}}));
        CHECK_STATUS(open, 206);
        CHECK_EQ(open.Header("Content-Range"),
                 std::string("bytes 240-255/") + std::to_string(kSize));
        CHECK(BodyEquals(open, content, 240, 16));
    }

    // The suffix form counts back from EOF.
    {
        const RawResponse suffix =
            FetchOnce(port, GetRequest("/normal", {{"Range", "bytes=-16"}}));
        CHECK_STATUS(suffix, 206);
        CHECK_EQ(suffix.Header("Content-Range"),
                 std::string("bytes 240-255/") + std::to_string(kSize));
        CHECK(BodyEquals(suffix, content, 240, 16));
    }

    // A range that straddles EOF is clamped and still 206. This is the
    // boundary a range backend most often gets wrong by one, and the server
    // must not be the thing that hides it.
    {
        const RawResponse straddle =
            FetchOnce(port, GetRequest("/normal", {{"Range", "bytes=250-999"}}));
        CHECK_STATUS(straddle, 206);
        CHECK_EQ(straddle.Header("Content-Range"),
                 std::string("bytes 250-255/") + std::to_string(kSize));
        CHECK(BodyEquals(straddle, content, 250, 6));
    }

    // A range starting at EOF is unsatisfiable, which is a 416 and not an
    // empty 206.
    {
        const RawResponse beyond =
            FetchOnce(port, GetRequest("/normal", {{"Range", "bytes=256-300"}}));
        CHECK_STATUS(beyond, 416);
        CHECK_EQ(beyond.Header("Content-Range"),
                 std::string("bytes */") + std::to_string(kSize));
    }

    // An unparseable Range is ignored, per RFC 9110 §14.2 -- a 200 with the
    // whole body, which a backend must not mistake for RangeNotSupported.
    {
        const RawResponse garbage =
            FetchOnce(port, GetRequest("/normal", {{"Range", "bytes=abc"}}));
        CHECK_STATUS(garbage, 200);
        CHECK(BodyEquals(garbage, content, 0, kSize));
    }

    // Multi-range is out of v0.x scope and answered as if unparseable, so that
    // a backend that starts sending one is caught rather than served.
    {
        const RawResponse multi =
            FetchOnce(port, GetRequest("/normal", {{"Range", "bytes=0-9,20-29"}}));
        CHECK_STATUS(multi, 200);
        CHECK(BodyEquals(multi, content, 0, kSize));
    }

    // A matching If-Range leaves the range in force.
    {
        const RawResponse matched = FetchOnce(
            port, GetRequest("/normal",
                             {{"Range", "bytes=10-19"}, {"If-Range", "\"v1\""}}));
        CHECK_STATUS(matched, 206);
        CHECK(BodyEquals(matched, content, 10, 10));
    }

    // A non-matching If-Range makes the range inapplicable and returns the
    // whole representation (RFC 9110 §13.1.5). This is not a hostile case: it
    // is the mechanism the revision binding in §6 of the design policy is
    // built on, and a backend detects AssetChanged by seeing exactly this.
    {
        const RawResponse stale = FetchOnce(
            port, GetRequest("/normal",
                             {{"Range", "bytes=10-19"}, {"If-Range", "\"gone\""}}));
        CHECK_STATUS(stale, 200);
        CHECK(!stale.HasHeader("Content-Range"));
        CHECK(BodyEquals(stale, content, 0, kSize));
    }

    // Field names are case-insensitive, and a backend that sends `range:` must
    // be answered rather than quietly served a whole body.
    {
        const RawResponse lower =
            FetchOnce(port, GetRequest("/normal", {{"range", "bytes=0-3"}}));
        CHECK_STATUS(lower, 206);
        CHECK(BodyEquals(lower, content, 0, 4));
    }

    // Keep-alive: two requests, one connection. Connection reuse is one of the
    // four constraints ADR-0003 chose the client on, and a fixture server that
    // closed after every response could not exercise it.
    {
        RawClient client(port);
        CHECK(client.Connected());
        CHECK(client.Send(GetRequest("/normal", {{"Range", "bytes=0-3"}})));
        const RawResponse first = client.ReadResponse(5000);
        CHECK_STATUS(first, 206);
        CHECK(client.Send(GetRequest("/normal", {{"Range", "bytes=4-7"}})));
        const RawResponse second = client.ReadResponse(5000);
        CHECK_STATUS(second, 206);
        CHECK(BodyEquals(second, content, 4, 4));
    }
}

// --- range support is a capability -------------------------------------------

void TestNoAcceptRanges(Server& server, const std::vector<unsigned char>& content) {
    CaseScope scope(BehaviorName(Behavior::NoAcceptRanges));
    Exercised(Behavior::NoAcceptRanges);
    const unsigned short port = server.Port();

    const RawResponse head =
        FetchOnce(port, HeadRequest("/no-accept-ranges"), 5000, false);
    CHECK_STATUS(head, 200);
    CHECK(!head.HasHeader("Accept-Ranges"));

    const RawResponse ranged = FetchOnce(
        port, GetRequest("/no-accept-ranges", {{"Range", "bytes=10-19"}}));
    CHECK_STATUS(ranged, 200);
    CHECK(!ranged.HasHeader("Content-Range"));
    CHECK(BodyEquals(ranged, content, 0, kSize));

    // Even an out-of-bounds range gets the whole body: this server never
    // looked at the header, so it has no 416 to give.
    const RawResponse beyond = FetchOnce(
        port, GetRequest("/no-accept-ranges", {{"Range", "bytes=999-1999"}}));
    CHECK_STATUS(beyond, 200);
    CHECK(BodyEquals(beyond, content, 0, kSize));
}

void TestIgnoresRange(Server& server, const std::vector<unsigned char>& content) {
    CaseScope scope(BehaviorName(Behavior::IgnoresRange));
    Exercised(Behavior::IgnoresRange);
    const unsigned short port = server.Port();

    // The trap: HEAD looks entirely correct.
    const RawResponse head = FetchOnce(port, HeadRequest("/ignores-range"), 5000, false);
    CHECK_STATUS(head, 200);
    CHECK_EQ(head.Header("Accept-Ranges"), std::string("bytes"));

    // And then the range is ignored anyway. ADR-0002 makes this
    // RangeNotSupported and terminal: the whole body arriving is not
    // permission to use it.
    const RawResponse ranged =
        FetchOnce(port, GetRequest("/ignores-range", {{"Range", "bytes=10-19"}}));
    CHECK_STATUS(ranged, 200);
    CHECK(!ranged.HasHeader("Content-Range"));
    CHECK_EQ(ranged.Header("Content-Length"), std::to_string(kSize));
    CHECK(BodyEquals(ranged, content, 0, kSize));
}

// --- framing -----------------------------------------------------------------

void TestTruncatedBody(Server& server, const std::vector<unsigned char>& content) {
    CaseScope scope(BehaviorName(Behavior::TruncatedBody));
    Exercised(Behavior::TruncatedBody);
    const unsigned short port = server.Port();

    const RawResponse response =
        FetchOnce(port, GetRequest("/truncated", {{"Range", "bytes=0-63"}}));
    CHECK_STATUS(response, 206);

    // The headers are honest and the body is not. Everything a backend has to
    // go on says 64 bytes are coming.
    CHECK_EQ(response.Header("Content-Range"),
             std::string("bytes 0-63/") + std::to_string(kSize));
    CHECK_EQ(response.Header("Content-Length"), std::string("64"));

    CHECK_EQ(response.body.size(), static_cast<std::size_t>(16));
    CHECK(BodyEquals(response, content, 0, 16));

    // The connection ended before the promise was kept, which is the only
    // signal that anything was wrong.
    CHECK(response.end != ResponseEnd::Complete);
}

void TestContentRangeTooShort(Server& server,
                              const std::vector<unsigned char>& content) {
    CaseScope scope(BehaviorName(Behavior::ContentRangeTooShort));
    Exercised(Behavior::ContentRangeTooShort);
    const unsigned short port = server.Port();

    const RawResponse response =
        FetchOnce(port, GetRequest("/short-range", {{"Range", "bytes=0-63"}}));
    CHECK_STATUS(response, 206);

    // 64 asked for, 32 described and 32 delivered. The response agrees with
    // itself perfectly -- Content-Range, Content-Length, and the body all say
    // 32 -- and disagrees only with the request. A backend that validates
    // framing internally passes this; one that validates against what it asked
    // for catches it, which is the distinction the case exists to force.
    CHECK_EQ(response.Header("Content-Range"),
             std::string("bytes 0-31/") + std::to_string(kSize));
    CHECK_EQ(response.Header("Content-Length"), std::string("32"));
    CHECK_EQ(response.end, ResponseEnd::Complete);
    CHECK(BodyEquals(response, content, 0, 32));

    // Two bytes is the shortest request this row can be hostile about: one is
    // half of two, and half of one is nothing.
    const RawResponse pair =
        FetchOnce(port, GetRequest("/short-range", {{"Range", "bytes=8-9"}}));
    CHECK_STATUS(pair, 206);
    CHECK_EQ(pair.Header("Content-Range"),
             std::string("bytes 8-8/") + std::to_string(kSize));
    CHECK_EQ(pair.Header("Content-Length"), std::string("1"));
    CHECK(BodyEquals(pair, content, 8, 1));

    // And a single-byte request is answered correctly, because `Content-Range`
    // has no way to describe an empty range. Pinned here rather than left to be
    // discovered: a backend test that leaned on this row for a one-byte read
    // would otherwise be asserting against a server that was behaving.
    const RawResponse single =
        FetchOnce(port, GetRequest("/short-range", {{"Range", "bytes=8-8"}}));
    CHECK_STATUS(single, 206);
    CHECK_EQ(single.Header("Content-Range"),
             std::string("bytes 8-8/") + std::to_string(kSize));
    CHECK_EQ(single.Header("Content-Length"), std::string("1"));
    CHECK(BodyEquals(single, content, 8, 1));
}

void TestContentRangeShifted(Server& server,
                             const std::vector<unsigned char>& content) {
    CaseScope scope(BehaviorName(Behavior::ContentRangeShifted));
    Exercised(Behavior::ContentRangeShifted);
    const unsigned short port = server.Port();

    // Right length, wrong place -- caught by a different check than the row
    // above, which is why it is a separate row.
    const RawResponse shifted =
        FetchOnce(port, GetRequest("/shifted-range", {{"Range", "bytes=10-19"}}));
    CHECK_STATUS(shifted, 206);
    CHECK_EQ(shifted.Header("Content-Range"),
             std::string("bytes 9-18/") + std::to_string(kSize));
    CHECK_EQ(shifted.end, ResponseEnd::Complete);
    CHECK(BodyEquals(shifted, content, 9, 10));

    // At offset zero there is no room below, so the shift goes the other way.
    // The case has to work at zero: offset zero is where off-by-one framing
    // shows up, and a hostile case that quietly becomes a correct response
    // there is a hole in the corpus.
    const RawResponse atZero =
        FetchOnce(port, GetRequest("/shifted-range", {{"Range", "bytes=0-9"}}));
    CHECK_STATUS(atZero, 206);
    CHECK_EQ(atZero.Header("Content-Range"),
             std::string("bytes 1-10/") + std::to_string(kSize));
    CHECK(BodyEquals(atZero, content, 1, 10));

    // A range covering the whole representation has room in neither direction,
    // and all three spellings of it parse to the same range. This is where the
    // row used to stop being hostile: `bytes 0-255/256` came back, correct in
    // every particular, from the fixture whose entire job is a wrong offset.
    // The window now moves up by one and clamps at EOF.
    const char* const whole[] = {"bytes=0-255", "bytes=0-", "bytes=-256"};
    for (const char* spec : whole) {
        const RawResponse all =
            FetchOnce(port, GetRequest("/shifted-range", {{"Range", spec}}));
        CHECK_STATUS(all, 206);
        CHECK_EQ(all.Header("Content-Range"),
                 std::string("bytes 1-255/") + std::to_string(kSize));
        CHECK_EQ(all.Header("Content-Length"), std::string("255"));
        CHECK(BodyEquals(all, content, 1, 255));
    }

    // Two bytes is the smallest asset with a second offset to shift to, and so
    // the smallest one this row can be carried by at all.
    const RawResponse tiny =
        FetchOnce(port, GetRequest("/shifted-tiny", {{"Range", "bytes=0-1"}}));
    CHECK_STATUS(tiny, 206);
    CHECK_EQ(tiny.Header("Content-Range"), std::string("bytes 1-1/2"));
    CHECK(BodyEquals(tiny, content, 1, 1));
}

void TestUnknownContentLength(Server& server,
                              const std::vector<unsigned char>& content) {
    CaseScope scope(BehaviorName(Behavior::UnknownContentLength));
    Exercised(Behavior::UnknownContentLength);
    const unsigned short port = server.Port();

    const RawResponse response = FetchOnce(port, GetRequest("/unknown-length"));
    CHECK_STATUS(response, 200);

    // No Content-Length and no Transfer-Encoding: the body ends when the
    // connection does, so "complete" is not a thing this response can be. A
    // client that accumulates until EOF and calls it a success cannot tell
    // this from a truncated transfer, which is why ADR-0003 requires one that
    // can refuse it.
    CHECK(!response.HasHeader("Content-Length"));
    CHECK(!response.HasHeader("Transfer-Encoding"));
    CHECK_EQ(response.end, ResponseEnd::Closed);
    CHECK(BodyEquals(response, content, 0, kSize));
}

// --- revision binding --------------------------------------------------------

void TestValidatorChangeMidRead(Server& server,
                                const std::vector<unsigned char>& content) {
    CaseScope scope(BehaviorName(Behavior::ValidatorChangeMidRead));
    Exercised(Behavior::ValidatorChangeMidRead);
    const unsigned short port = server.Port();

    // The first request sees the original revision and its validator.
    const RawResponse first =
        FetchOnce(port, GetRequest("/mid-read", {{"Range", "bytes=0-15"}}));
    CHECK_STATUS(first, 206);
    CHECK_EQ(first.Header("ETag"), std::string("\"v1\""));
    CHECK(BodyEquals(first, content, 0, 16));

    // The asset has now changed underneath the reader. A second range request
    // carrying the captured validator is answered with 200 and the whole *new*
    // representation: the range did not apply, because the thing it applied to
    // is gone. That is AssetChanged, and it is the only reason a reader ever
    // learns the world moved.
    const RawResponse conditional = FetchOnce(
        port, GetRequest("/mid-read",
                         {{"Range", "bytes=16-31"}, {"If-Range", "\"v1\""}}));
    CHECK_STATUS(conditional, 200);
    CHECK(!conditional.HasHeader("Content-Range"));
    CHECK_EQ(conditional.Header("ETag"), std::string("\"revised\""));

    // And the case that makes If-Range non-optional: the same request without
    // it succeeds, with a 206, from the new revision, with nothing to report.
    // Those bytes and the first response's bytes are a sequence that never
    // existed at any instant -- the silent corruption §6 of the design policy
    // exists to prevent, reproduced here so a backend can be shown not to
    // produce it.
    const RawResponse unconditional =
        FetchOnce(port, GetRequest("/mid-read", {{"Range", "bytes=16-31"}}));
    CHECK_STATUS(unconditional, 206);
    CHECK_EQ(unconditional.Header("ETag"), std::string("\"revised\""));
    CHECK_EQ(unconditional.body.size(), static_cast<std::size_t>(16));
}

void TestRepublish(Server& server) {
    CaseScope scope("Republish");
    const unsigned short port = server.Port();

    AssetSpec spec;
    spec.path = "/republished";
    spec.content = MakeContent(64);
    spec.etag = "\"before\"";
    server.Serve(spec);

    const RawResponse before = FetchOnce(port, GetRequest("/republished"));
    CHECK_STATUS(before, 200);
    CHECK_EQ(before.Header("ETag"), std::string("\"before\""));
    CHECK_EQ(before.body.size(), static_cast<std::size_t>(64));

    CHECK(server.Republish("/republished", MakeContent(128), "\"after\""));

    const RawResponse after = FetchOnce(port, GetRequest("/republished"));
    CHECK_STATUS(after, 200);
    CHECK_EQ(after.Header("ETag"), std::string("\"after\""));
    CHECK_EQ(after.body.size(), static_cast<std::size_t>(128));

    // A republish of an asset that does not exist reports that it did not
    // happen. A fixture that failed to change must not be reported as a
    // backend that failed to notice: those are different defects, in different
    // repositories.
    CHECK(!server.Republish("/never-registered", MakeContent(8), "\"x\""));
}

// --- redirects ---------------------------------------------------------------

void TestRedirectChain(Server& server, const std::vector<unsigned char>& content) {
    CaseScope scope(BehaviorName(Behavior::RedirectChain));
    Exercised(Behavior::RedirectChain);
    const unsigned short port = server.Port();

    std::string target = "/chain";
    int hops = 0;
    for (; hops < 10; ++hops) {
        const RawResponse response = FetchOnce(port, GetRequest(target));
        if (response.status == 302) {
            const std::string location = response.Header("Location");
            CHECK(!location.empty());
            target = location;
            continue;
        }
        CHECK_STATUS(response, 200);
        CHECK(BodyEquals(response, content, 0, kSize));
        break;
    }
    CHECK_EQ(hops, 3);

    // The `.hopN` suffix is the chain's own alias, and every part of it has to
    // be right before it routes anywhere. A bare `.hop` names no hop, so it
    // names no asset: it must not redirect to `/chain.hop.hop1` -- a chain that
    // walks away from its own asset and then 404s -- and a suffix that is not a
    // number must not be read as hop zero and start the chain over.
    CHECK_STATUS(FetchOnce(port, GetRequest("/chain.hop")), 404);
    CHECK_STATUS(FetchOnce(port, GetRequest("/chain.hopx")), 404);

    // Nor does an ordinary asset acquire a second path by suffix. `/normal.hop`
    // served `/normal`'s body under a name nobody registered, and counted the
    // request against `/normal` while doing it, which is a request log a test
    // cannot reason about.
    CHECK_STATUS(FetchOnce(port, GetRequest("/normal.hop")), 404);
    CHECK_STATUS(FetchOnce(port, GetRequest("/normal.hop1")), 404);
}

void TestRedirectLoop(Server& server) {
    CaseScope scope(BehaviorName(Behavior::RedirectLoop));
    Exercised(Behavior::RedirectLoop);
    const unsigned short port = server.Port();

    // The server never terminates this. Nothing in the corpus bounds it, on
    // purpose: what stops it must be the backend's own counter, and a fixture
    // that gave up after N hops would let a backend with no counter pass.
    for (int hop = 0; hop < 5; ++hop) {
        const RawResponse response = FetchOnce(port, GetRequest("/loop"));
        CHECK_STATUS(response, 302);
        CHECK_EQ(response.Header("Location"), std::string("/loop"));
    }
}

// --- deadlines ---------------------------------------------------------------

void TestSlowHeaders(Server& server) {
    CaseScope scope(BehaviorName(Behavior::SlowHeaders));
    Exercised(Behavior::SlowHeaders);
    const unsigned short port = server.Port();

    // A deadline shorter than the stall elapses with nothing received at all.
    // Asserted in the safe direction only: that 500ms of deliberate silence is
    // not answered within 50ms. The reverse -- that a fast response is fast --
    // is the assertion that flakes on a loaded runner, and it is not made.
    const RawResponse impatient =
        FetchOnce(port, GetRequest("/slow-headers"), 50);
    CHECK_EQ(impatient.end, ResponseEnd::TimedOut);
    CHECK(!impatient.gotHead);

    // Waited out, the response is ordinary.
    const RawResponse patient = FetchOnce(port, GetRequest("/slow-headers"), 10000);
    CHECK_STATUS(patient, 200);
    CHECK(patient.headElapsedMs >= 250);
    CHECK_EQ(patient.end, ResponseEnd::Complete);
}

void TestSlowBody(Server& server, const std::vector<unsigned char>& content) {
    CaseScope scope(BehaviorName(Behavior::SlowBody));
    Exercised(Behavior::SlowBody);
    const unsigned short port = server.Port();

    // The headers arrive, and then the transfer stalls. A backend with only a
    // total deadline cannot tell this from SlowHeaders, and HTTP006 is
    // required to name which deadline elapsed -- which is the whole reason
    // these are two rows.
    const RawResponse response = FetchOnce(port, GetRequest("/slow-body"), 10000);
    CHECK_STATUS(response, 200);
    CHECK(response.bodyElapsedMs >= 250);
    CHECK_EQ(response.end, ResponseEnd::Complete);
    CHECK(BodyEquals(response, content, 0, kSize));
}

// --- connection faults -------------------------------------------------------

void TestConnectionResetBeforeResponse(Server& server) {
    CaseScope scope(BehaviorName(Behavior::ConnectionResetBeforeResponse));
    Exercised(Behavior::ConnectionResetBeforeResponse);
    const unsigned short port = server.Port();

    const RawResponse response = FetchOnce(port, GetRequest("/reset-early"));

    // Whether the peer's RST surfaces as ECONNRESET or as an orderly EOF is
    // the platform's decision, and a corpus that asserted one would be
    // asserting a property of the runner. What is asserted is the fact a
    // backend must handle: the request was accepted and no response arrived.
    CHECK(!response.gotHead);
    CHECK_EQ(response.status, 0);
    CHECK(response.end == ResponseEnd::Reset ||
          response.end == ResponseEnd::NoResponse);

    // The server nonetheless logged the request, with no status -- which is
    // how a test tells "never answered" from "never asked".
    const auto log = server.Log();
    bool logged = false;
    for (const auto& record : log) {
        if (record.target == "/reset-early") {
            logged = true;
            CHECK_EQ(record.status, 0);
        }
    }
    CHECK(logged);
}

void TestConnectionResetMidBody(Server& server,
                                const std::vector<unsigned char>& content) {
    CaseScope scope(BehaviorName(Behavior::ConnectionResetMidBody));
    Exercised(Behavior::ConnectionResetMidBody);
    const unsigned short port = server.Port();

    const RawResponse response =
        FetchOnce(port, GetRequest("/reset-mid-body", {{"Range", "bytes=0-63"}}));

    // An RST destroys whatever is still sitting in the client's receive buffer,
    // and how much that is depends on the platform and on when this thread last
    // ran. Measured: Linux hands the reader the eight buffered body bytes
    // before it reports the error, and Winsock discards them -- the same
    // fixture, and `body.size()` is 8 on one cell and 0 on the other.
    //
    // The head survives on both, in every run of fifteen on Windows, because
    // the reader has already copied it out before the reset lands. That is a
    // race and not a guarantee: the headers, the eight bytes, and the RST leave
    // the server microseconds apart, and a reader descheduled across that
    // window loses all three together. So the framing is asserted only when it
    // arrived, exactly as the sibling case above does, and what is asserted
    // unconditionally is the one thing every platform promises -- that the
    // promise was not kept.
    CHECK(response.end != ResponseEnd::Complete);

    if (response.gotHead) {
        CHECK_STATUS(response, 206);
        CHECK_EQ(response.Header("Content-Length"), std::string("64"));
        CHECK(response.body.size() < 64u);
        if (!response.body.empty()) {
            CHECK(BodyEquals(response, content, 0, response.body.size()));
        }
    } else {
        CHECK(response.end == ResponseEnd::Reset ||
              response.end == ResponseEnd::NoResponse);
    }
}

// --- status codes ------------------------------------------------------------

void TestRangeNotSatisfiable(Server& server) {
    CaseScope scope(BehaviorName(Behavior::RangeNotSatisfiable));
    Exercised(Behavior::RangeNotSatisfiable);
    const unsigned short port = server.Port();

    // A 416 for a range that is entirely inside the asset: the server simply
    // refuses.
    const RawResponse response =
        FetchOnce(port, GetRequest("/always-416", {{"Range", "bytes=0-15"}}));
    CHECK_STATUS(response, 416);
    CHECK_EQ(response.Header("Content-Range"),
             std::string("bytes */") + std::to_string(kSize));

    // Without a Range there is nothing to refuse, and the asset is served.
    const RawResponse whole = FetchOnce(port, GetRequest("/always-416"));
    CHECK_STATUS(whole, 200);
}

void TestNotFoundAndAccessDenied(Server& server) {
    CaseScope scope("NotFound / AccessDenied");
    Exercised(Behavior::NotFound);
    Exercised(Behavior::AccessDenied);
    const unsigned short port = server.Port();

    CHECK_STATUS(FetchOnce(port, GetRequest("/missing")), 404);
    CHECK_STATUS(FetchOnce(port, GetRequest("/forbidden")), 403);

    // A path nobody registered is also a 404, and it must be: an unreachable
    // fixture and an absent asset are the same fact from the wire, and
    // DIAGNOSTICS.md §4.4 keeps them apart above this layer, not here.
    CHECK_STATUS(FetchOnce(port, GetRequest("/never-registered-at-all")), 404);
}

void TestTransientServerError(Server& server,
                              const std::vector<unsigned char>& content) {
    CaseScope scope(BehaviorName(Behavior::TransientServerError));
    Exercised(Behavior::TransientServerError);
    const unsigned short port = server.Port();

    CHECK_STATUS(FetchOnce(port, GetRequest("/flaky")), 503);
    CHECK_STATUS(FetchOnce(port, GetRequest("/flaky")), 503);

    const RawResponse recovered = FetchOnce(port, GetRequest("/flaky"));
    CHECK_STATUS(recovered, 200);
    CHECK(BodyEquals(recovered, content, 0, kSize));
}

// --- validators and the request log ------------------------------------------

void TestValidatorForms(Server& server) {
    CaseScope scope("validator forms");
    const unsigned short port = server.Port();

    // No usable validator at all. Reads must still work; what changes is the
    // stability a backend reports, and that is not this layer's business.
    const RawResponse none = FetchOnce(port, GetRequest("/no-validator"));
    CHECK_STATUS(none, 200);
    CHECK(!none.HasHeader("ETag"));
    CHECK(!none.HasHeader("Last-Modified"));

    // A weak validator is emitted in its `W/` form, verbatim.
    const RawResponse weak = FetchOnce(port, GetRequest("/weak-validator"));
    CHECK_STATUS(weak, 200);
    CHECK_EQ(weak.Header("ETag"), std::string("W/\"weak\""));
    CHECK_EQ(weak.Header("Last-Modified"),
             std::string("Wed, 21 Oct 2015 07:28:00 GMT"));
}

void TestRequestLog(Server& server) {
    CaseScope scope("request log");
    const unsigned short port = server.Port();

    server.ClearLog();
    CHECK_EQ(server.RequestCount(), static_cast<std::size_t>(0));

    // The query string is preserved in the log and ignored for routing. Both
    // halves matter: a backend must not mangle a signed URL, and no message it
    // emits may repeat one (design policy §4.3).
    const RawResponse response = FetchOnce(
        port, GetRequest("/normal?token=redacted",
                         {{"Range", "bytes=0-7"}, {"If-Range", "\"v1\""}}));
    CHECK_STATUS(response, 206);

    CHECK_EQ(CountLogged(server, "/normal"), static_cast<std::size_t>(1));

    const auto log = server.Log();
    bool found = false;
    for (const auto& record : log) {
        if (record.target.compare(0, 7, "/normal") != 0) continue;
        found = true;
        CHECK_EQ(record.method, std::string("GET"));
        CHECK_EQ(record.target, std::string("/normal?token=redacted"));
        CHECK_EQ(record.range, std::string("bytes=0-7"));
        CHECK_EQ(record.ifRange, std::string("\"v1\""));
        CHECK_EQ(record.status, 206);
    }
    CHECK(found);

    // The counter is what a test asserts "no request was issued" with -- the
    // zero-length read row of the boundary suite has no other evidence
    // available to it.
    server.ClearLog();
    CHECK_EQ(server.RequestCount(), static_cast<std::size_t>(0));
}

// --- degenerate sizes --------------------------------------------------------

void TestEmptyAsset(Server& server) {
    CaseScope scope("empty asset");
    const unsigned short port = server.Port();

    const RawResponse whole = FetchOnce(port, GetRequest("/empty"));
    CHECK_STATUS(whole, 200);
    CHECK_EQ(whole.Header("Content-Length"), std::string("0"));
    CHECK(whole.body.empty());

    // Every range over an empty asset is unsatisfiable, including `bytes=0-`.
    CHECK_STATUS(FetchOnce(port, GetRequest("/empty", {{"Range", "bytes=0-"}})), 416);
    CHECK_STATUS(FetchOnce(port, GetRequest("/empty", {{"Range", "bytes=0-0"}})), 416);
    CHECK_STATUS(FetchOnce(port, GetRequest("/empty", {{"Range", "bytes=-1"}})), 416);
}

void TestSingleByteAsset(Server& server) {
    CaseScope scope("one-byte asset");
    const unsigned short port = server.Port();

    const RawResponse only =
        FetchOnce(port, GetRequest("/one-byte", {{"Range", "bytes=0-0"}}));
    CHECK_STATUS(only, 206);
    CHECK_EQ(only.Header("Content-Range"), std::string("bytes 0-0/1"));
    CHECK_EQ(only.body.size(), static_cast<std::size_t>(1));

    CHECK_STATUS(FetchOnce(port, GetRequest("/one-byte", {{"Range", "bytes=1-1"}})),
                 416);
}

// --- concurrency -------------------------------------------------------------

void TestConcurrentRequests(Server& server,
                            const std::vector<unsigned char>& content) {
    CaseScope scope("concurrent requests");
    const unsigned short port = server.Port();

    // The boundary suite runs concurrent readers over one asset, under
    // ThreadSanitizer, and every one of those cases will run against this
    // server. A fixture server that is only correct single-threaded would turn
    // the suite's concurrency rows into a test of the fixture.
    server.ClearLog();

    constexpr int kThreads = 8;
    constexpr int kPerThread = 8;
    std::atomic<int> wrong{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&, thread] {
            for (int i = 0; i < kPerThread; ++i) {
                const std::size_t first =
                    static_cast<std::size_t>((thread * kPerThread + i) % 200);
                const std::string range =
                    "bytes=" + std::to_string(first) + "-" + std::to_string(first + 15);
                const RawResponse response =
                    FetchOnce(port, GetRequest("/normal", {{"Range", range}}));
                if (response.status != 206 ||
                    !BodyEquals(response, content, first, 16)) {
                    ++wrong;
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();

    CHECK_EQ(wrong.load(), 0);
    CHECK_EQ(CountLogged(server, "/normal"),
             static_cast<std::size_t>(kThreads * kPerThread));
    server.ClearLog();
}

void TestConnectionsAreReaped(Server& server) {
    CaseScope scope("connections are reaped");
    const unsigned short port = server.Port();

    // A finished connection thread is joined by the accept loop, not held until
    // Stop(). Held, the count is one unreaped stack per request -- a few
    // hundred sequential requests are already gigabytes of reserved address
    // space -- and the boundary suite will run far more than a few hundred
    // against one server. Nothing about the responses changes, which is exactly
    // why this needs asserting rather than eyeballing.
    server.ClearLog();

    constexpr int kRequests = 200;
    int wrong = 0;
    for (int i = 0; i < kRequests; ++i) {
        const RawResponse response =
            FetchOnce(port, GetRequest("/normal", {{"Range", "bytes=0-3"}}));
        if (response.status != 206) ++wrong;
    }
    CHECK_EQ(wrong, 0);

    // Reaping happens on the accept loop's own schedule, so the assertion is
    // "settles quickly", not "is already zero". The bound is generous: what
    // fails here is accumulation, and accumulation does not settle.
    std::size_t open = server.OpenConnections();
    for (int waited = 0; waited < 200 && open > 8; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        open = server.OpenConnections();
    }
    CHECK(open <= 8);

    server.ClearLog();
}

/// Its own server, because it stops one.
void TestStopWithAStalledReader() {
    CaseScope scope("stop with a stalled reader");

    std::string error;
    std::unique_ptr<Server> server = Server::Start(&error);
    CHECK(server != nullptr);
    if (!server) return;

    // Far larger than a socket buffer with a bound on it, so that the handler
    // is still inside the write when Stop() is called. On Linux that is
    // measured to be what happens -- with the abandon flag removed this case
    // does not fail, it hangs, and the process has to be killed.
    //
    // Windows is a different story and the size is not the reason. Winsock
    // buffers the whole response however large it is: 64 MiB went out without a
    // single `WSAEWOULDBLOCK`, so the handler never stalls there and this case
    // passes for a reason unrelated to what it tests. Sixteen rather than
    // sixty-four megabytes, then -- the larger figure buys nothing on the cell
    // that cannot use it, and costs a copy of it on every run of every lane.
    //
    // One repeated byte rather than MakeContent's arithmetic, because nothing
    // here reads the body.
    AssetSpec spec;
    spec.path = "/big";
    spec.content.assign(16u * 1024u * 1024u, 0xa5u);
    spec.etag = "\"big\"";
    server->Serve(spec);

    RawClient client(server->Port());
    CHECK(client.Connected());
    CHECK(client.Send(GetRequest("/big")));

    // Long enough for the handler to reach the write and stick there. The
    // assertion is about Stop() returning at all, so landing early costs
    // nothing but a weaker version of the same check.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const auto started = std::chrono::steady_clock::now();
    server->Stop();
    const long elapsedMs = static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count());

    // "Shutdown never hangs" is a contract in Server.h and in the README, and
    // it was not true: the condition variable Stop() signals reaches the
    // deliberate stalls and does not reach a thread sitting in `send` on a
    // client that stopped reading. That client is not misbehaving in any way
    // the corpus forbids -- it is what every reader that hits its own deadline
    // and walks away looks like from here.
    CHECK(elapsedMs < 5000);
}

// --- registration -------------------------------------------------------------

void RegisterFixtures(Server& server, const std::vector<unsigned char>& content) {
    auto base = [&content](const std::string& path, Behavior behavior) {
        AssetSpec spec;
        spec.path = path;
        spec.content = content;
        spec.behavior = behavior;
        spec.etag = "\"v1\"";
        return spec;
    };

    server.Serve(base("/normal", Behavior::Normal));
    server.Serve(base("/no-accept-ranges", Behavior::NoAcceptRanges));
    server.Serve(base("/ignores-range", Behavior::IgnoresRange));
    server.Serve(base("/short-range", Behavior::ContentRangeTooShort));
    server.Serve(base("/shifted-range", Behavior::ContentRangeShifted));
    server.Serve(base("/unknown-length", Behavior::UnknownContentLength));
    server.Serve(base("/loop", Behavior::RedirectLoop));
    server.Serve(base("/always-416", Behavior::RangeNotSatisfiable));
    server.Serve(base("/missing", Behavior::NotFound));
    server.Serve(base("/forbidden", Behavior::AccessDenied));
    server.Serve(base("/reset-early", Behavior::ConnectionResetBeforeResponse));

    {
        AssetSpec spec = base("/truncated", Behavior::TruncatedBody);
        spec.deliverBytes = 16;
        server.Serve(spec);
    }
    {
        AssetSpec spec = base("/reset-mid-body", Behavior::ConnectionResetMidBody);
        spec.deliverBytes = 8;
        server.Serve(spec);
    }
    {
        AssetSpec spec = base("/mid-read", Behavior::ValidatorChangeMidRead);
        spec.changeAfterRequests = 1;
        spec.revisedEtag = "\"revised\"";
        server.Serve(spec);
    }
    {
        AssetSpec spec = base("/chain", Behavior::RedirectChain);
        spec.redirectHops = 3;
        server.Serve(spec);
    }
    {
        AssetSpec spec = base("/slow-headers", Behavior::SlowHeaders);
        spec.delayMs = 500;
        server.Serve(spec);
    }
    {
        AssetSpec spec = base("/slow-body", Behavior::SlowBody);
        spec.delayMs = 500;
        server.Serve(spec);
    }
    {
        AssetSpec spec = base("/flaky", Behavior::TransientServerError);
        spec.transientFailures = 2;
        server.Serve(spec);
    }
    {
        AssetSpec spec = base("/no-validator", Behavior::Normal);
        spec.etag.clear();
        server.Serve(spec);
    }
    {
        AssetSpec spec = base("/weak-validator", Behavior::Normal);
        spec.etag = "\"weak\"";
        spec.weakValidator = true;
        spec.lastModified = "Wed, 21 Oct 2015 07:28:00 GMT";
        server.Serve(spec);
    }
    {
        AssetSpec spec = base("/empty", Behavior::Normal);
        spec.content.clear();
        server.Serve(spec);
    }
    {
        AssetSpec spec = base("/one-byte", Behavior::Normal);
        spec.content.assign(1, content[0]);
        server.Serve(spec);
    }
    {
        // The smallest asset ContentRangeShifted can be carried by: a shift
        // needs a second offset to shift to.
        AssetSpec spec = base("/shifted-tiny", Behavior::ContentRangeShifted);
        spec.content.assign(content.begin(), content.begin() + 2);
        server.Serve(spec);
    }
}

/// The corpus claims to cover §11.2 of the design policy. This is what makes
/// the claim checkable: a behavior added to the enum without a case added
/// beside it fails the run.
void TestCoverage() {
    CaseScope scope("corpus coverage");
    for (const Behavior behavior : usdassetfixture::AllBehaviors()) {
        if (g_exercised.find(behavior) == g_exercised.end()) {
            std::fprintf(stderr,
                         "FAIL [corpus coverage] behavior '%s' has no case\n",
                         BehaviorName(behavior));
            ++usdassetfixturetest::FailureCount();
        }
    }
}

}  // namespace

int main() {
    std::string error;
    std::unique_ptr<Server> server = Server::Start(&error);
    if (!server) {
        std::fprintf(stderr,
                     "FAIL: the fixture server could not bind loopback: %s\n",
                     error.c_str());
        return 1;
    }

    const std::vector<unsigned char> content = MakeContent(kSize);
    RegisterFixtures(*server, content);

    TestNormal(*server, content);
    TestNoAcceptRanges(*server, content);
    TestIgnoresRange(*server, content);
    TestTruncatedBody(*server, content);
    TestContentRangeTooShort(*server, content);
    TestContentRangeShifted(*server, content);
    TestUnknownContentLength(*server, content);
    TestValidatorChangeMidRead(*server, content);
    TestRepublish(*server);
    TestRedirectChain(*server, content);
    TestRedirectLoop(*server);
    TestSlowHeaders(*server);
    TestSlowBody(*server, content);
    TestConnectionResetBeforeResponse(*server);
    TestConnectionResetMidBody(*server, content);
    TestRangeNotSatisfiable(*server);
    TestNotFoundAndAccessDenied(*server);
    TestTransientServerError(*server, content);
    TestValidatorForms(*server);
    TestRequestLog(*server);
    TestEmptyAsset(*server);
    TestSingleByteAsset(*server);
    TestConcurrentRequests(*server, content);
    TestConnectionsAreReaped(*server);
    TestCoverage();

    // Last, and against a server of its own: it is the one case that stops one.
    TestStopWithAStalledReader();

    server->Stop();
    return usdassetfixturetest::Report("fixture_corpus");
}
