// SPDX-License-Identifier: Apache-2.0
//
// The destination policy, over a real socket.
//
// `usdAssetHttp_protocol` asserts the halves of the policy the protocol layer
// owns -- the pre-flight on a literal address, and the projection of a
// refusal -- against a scripted transport. What it cannot assert is the two
// halves that live in the client: that the host is judged as libcurl will send
// it, and that the address libcurl is about to connect to is judged after the
// name was resolved and before the socket exists. Those need a client, a
// resolver, and a listening socket, and the fixture server is the one listening
// socket this repository has.
//
// Loopback is the only destination a CI runner can offer without a network,
// so most cases here are about loopback: permitted by default, and refused when
// a policy says so -- whether the URL spells the address or names a host that
// resolves to it. The second of those is the case that matters. A policy that
// only read the URL would pass the first and let `localhost` through. The last
// case uses the fixture as a *proxy*, which is how a refused address that no
// socket here can reach is still asserted to be refused.

#include <cstdio>
#include <cstdlib>
#include <memory>
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
using usdasset::http::HttpOpenResult;
using usdasset::http::HttpOptions;
using usdassetfixture::AssetSpec;
using usdassetfixture::Behavior;
using usdassetfixture::RequestRecord;
using usdassetfixture::Server;

constexpr std::size_t kSize = 4096;

HttpOptions FastOptions() {
    HttpOptions options;
    options.connectTimeoutMs = 2000;
    options.responseTimeoutMs = 2000;
    options.transferTimeoutMs = 4000;
    return options;
}

HttpOptions RefusingLoopback() {
    HttpOptions options = FastOptions();
    options.destinations.loopback = false;
    return options;
}

std::string NamedUrl(const Server& server, const std::string& path) {
    return "http://localhost:" + std::to_string(server.Port()) + path;
}

void ExpectRefused(const HttpOpenResult& opened, const Server& server,
                   const char* what) {
    if (opened.status.code != StatusCode::AccessDenied) {
        std::fprintf(stderr, "FAIL [%s] expected AccessDenied, got %s\n", what,
                     usdasset::ToString(opened.status).c_str());
        ++usdassettest::FailureCount();
        return;
    }
    CHECK(opened.reader == nullptr);
    // Named, so that whoever reads it goes to their own policy rather than to
    // the origin's permissions.
    CHECK(opened.status.message.find("loopback") != std::string::npos);
    // And nothing reached the server. Not a request refused after it was
    // made: a connection never opened.
    CHECK_EQ(server.RequestCount(), std::size_t(0));
}

void TestLoopbackIsPermittedByDefault(Server& server) {
    // The default policy is what the whole corpus runs under, and this is the
    // one place that says so out loud: loopback is permitted, because `http`
    // is registered for local fixture servers and intranet hosts and the
    // default is not allowed to break the uses the scheme exists for.
    server.ClearLog();
    HttpOpenResult opened = usdasset::http::Open(server.Url("/normal"), FastOptions());
    CHECK(opened.reader != nullptr);
    if (!opened.reader) return;
    std::vector<unsigned char> buffer(128, 0);
    const ReadResult read = opened.reader->Read(0, buffer.data(), buffer.size());
    CHECK_EQ(read.status.code, StatusCode::Ok);

    // And through a name, which exercises the connect-time callback on the
    // admitting side: `localhost` commonly resolves to `::1` first, which this
    // fixture does not listen on, and the callback must admit that attempt and
    // the IPv4 one after it rather than stopping at the first.
    HttpOpenResult named =
        usdasset::http::Open(NamedUrl(server, "/normal"), FastOptions());
    if (!named.reader) {
        std::fprintf(stderr, "FAIL [named, permitted] %s\n",
                     usdasset::ToString(named.status).c_str());
        ++usdassettest::FailureCount();
    }
}

void TestLiteralIsRefusedBeforeConnecting(Server& server) {
    // The pre-flight half: `127.0.0.1` in the URL is judged as text.
    server.ClearLog();
    const HttpOpenResult opened =
        usdasset::http::Open(server.Url("/normal"), RefusingLoopback());
    ExpectRefused(opened, server, "literal");
}

void TestNameIsRefusedAtConnect(Server& server) {
    // The connect-time half, and the case the policy is worthless without. The
    // URL names no address at all; the pre-flight has nothing to judge, and
    // passes it. What refuses it is the transport, looking at the address the
    // name resolved to, before a socket for it exists.
    server.ClearLog();
    const HttpOpenResult opened =
        usdasset::http::Open(NamedUrl(server, "/normal"), RefusingLoopback());
    ExpectRefused(opened, server, "name");
}

void TestLegacySpellingIsRefusedAtConnect(Server& server) {
    // `127.1` is 127.0.0.1 to every resolver descended from `inet_aton`, and
    // to libcurl's own URL parser, and it is not a literal to the protocol
    // layer's pre-flight, which reads canonical dotted quads only. That is
    // deliberate -- a second parser for a notorious grammar would be a second
    // opinion about it -- and it is safe because the transport judges the
    // host as libcurl will send it, and the connect-time check sees what it
    // became regardless.
    const std::string url =
        "http://127.1:" + std::to_string(server.Port()) + "/normal";

    server.ClearLog();
    ExpectRefused(usdasset::http::Open(url, RefusingLoopback()), server, "127.1");

    // And under the default it is simply loopback, and opens.
    HttpOpenResult permitted = usdasset::http::Open(url, FastOptions());
    if (!permitted.reader) {
        std::fprintf(stderr, "FAIL [127.1, permitted] %s\n",
                     usdasset::ToString(permitted.status).c_str());
        ++usdassettest::FailureCount();
    }
}

/// Sets one environment variable for the life of a scope and puts back what was
/// there. An empty value removes the variable.
class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, const std::string& value) : _name(name) {
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
        if (const char* previous = std::getenv(name)) {
            _had = true;
            _previous = previous;
        }
        Set(value);
    }
    ~ScopedEnvironment() { Set(_had ? _previous : std::string()); }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    void Set(const std::string& value) {
#if defined(_WIN32)
        _putenv_s(_name, value.c_str());
#else
        if (value.empty()) {
            unsetenv(_name);
        } else {
            setenv(_name, value.c_str(), 1);
        }
#endif
    }

    const char* _name;
    bool _had = false;
    std::string _previous;
};

void TestLegacySpellingIsRefusedThroughAProxy(Server& server) {
    // Through a proxy the connect-time check sees the proxy's address, and the
    // destination goes out as text for the proxy to resolve. So a spelling the
    // client will normalize to a refused address has to be refused as the
    // client will send it -- and libcurl reads every one of these as
    // 169.254.169.254 before the proxy sees the request. The fixture server is
    // the proxy here: it logs whatever arrives, absolute-form targets included,
    // so "nothing reached the proxy" is a count of its log.
    ScopedEnvironment proxy("http_proxy", server.BaseUrl());
    ScopedEnvironment noProxy("no_proxy", std::string());
    ScopedEnvironment noProxyUpper("NO_PROXY", std::string());

    const char* const spellings[] = {
        "http://2852039166/latest/meta-data/",
        "http://0xa9fea9fe/latest/meta-data/",
        "http://169.254.43518/latest/meta-data/",
        "http://%31%36%39.254.169.254/latest/meta-data/",
        "http://169.254.169.254./latest/meta-data/",
    };
    for (const char* url : spellings) {
        server.ClearLog();
        const HttpOpenResult opened = usdasset::http::Open(url, FastOptions());
        if (opened.status.code != StatusCode::AccessDenied ||
            opened.status.message.find("metadata") == std::string::npos) {
            std::fprintf(stderr, "FAIL [proxy] %s: %s\n", url,
                         usdasset::ToString(opened.status).c_str());
            ++usdassettest::FailureCount();
        }
        CHECK_EQ(server.RequestCount(), std::size_t(0));
    }

    // The control, and the reason the case above is not vacuous: under a policy
    // that permits the metadata class, the same spelling does go to the proxy,
    // and the proxy is handed the address libcurl normalized it to. Without
    // the pre-flight, this is what every spelling above would have done.
    HttpOptions permissive = FastOptions();
    permissive.destinations.metadata = true;
    server.ClearLog();
    usdasset::http::Open("http://2852039166/latest/meta-data/", permissive);
    const std::vector<RequestRecord> log = server.Log();
    CHECK(!log.empty());
    if (!log.empty()) {
        CHECK(log.front().target.find("169.254.169.254") != std::string::npos);
    }
}

// Not here: a redirect that crosses from a permitted destination into a
// refused one. Staging it over a socket needs two origins in two classes, and a
// runner without a network has one. The rule is the protocol layer's -- each hop
// is judged as a new request, pre-flight and connect alike -- and it is asserted
// in `usdAssetHttp_protocol`, where a scripted `Location` can name any address.

}  // namespace

int main() {
    std::string error;
    std::unique_ptr<Server> server = Server::Start(&error);
    if (!server) {
        std::fprintf(stderr, "FAIL: the fixture server could not bind loopback: %s\n",
                     error.c_str());
        return 1;
    }

    AssetSpec normal;
    normal.path = "/normal";
    normal.content.assign(kSize, 0x5a);
    normal.behavior = Behavior::Normal;
    normal.etag = "\"rev-a\"";
    server->Serve(normal);

    TestLoopbackIsPermittedByDefault(*server);
    TestLiteralIsRefusedBeforeConnecting(*server);
    TestNameIsRefusedAtConnect(*server);
    TestLegacySpellingIsRefusedAtConnect(*server);
    TestLegacySpellingIsRefusedThroughAProxy(*server);

    server->Stop();
    return usdassettest::Report("usdAssetHttp/destination-policy");
}
