// SPDX-License-Identifier: Apache-2.0
//
// The HTTP backend's row in the shared boundary suite.
//
// The claim `v0.1.0` made was that adding a transport adds a row, not a suite.
// This file is that claim being cashed: it declares the same four things the
// local backend's row declares -- a factory, fixture provisioning, whether
// cancellation is admitted, and whether a revision change can be simulated --
// and then runs a byte-identical suite against a real server over a real
// socket. Nothing in `tests/boundary` was changed to admit it.
//
// The one thing this row has to arrange that the local backend gets for free is
// that its bytes exist somewhere the transport can reach. The suite writes the
// content to a file for its own oracle; provisioning copies that content onto
// the fixture server, so the oracle and the backend are reading the same bytes
// from two independent places.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "usdAssetHttp/HttpAssetReader.h"
#include "usdassetboundary/Backend.h"
#include "usdassetboundary/Fixture.h"
#include "usdassetboundary/Oracle.h"
#include "usdassetboundary/Suite.h"
#include "usdassetfixture/Corpus.h"
#include "usdassetfixture/Server.h"

namespace {

usdassetfixture::Server* g_server = nullptr;

/// Deadlines generous enough to survive a sanitizer build.
///
/// The suite issues tens of thousands of requests, and under ThreadSanitizer
/// each one is slow. A deadline tuned to an optimized loopback run turns this
/// row into a lane that fails for reasons that have nothing to do with the read
/// contract -- which is the failure mode the fixture server's own request
/// timeout was made generous for, and the same argument applies here.
usdasset::http::HttpOptions SuiteOptions() {
    usdasset::http::HttpOptions options;
    options.connectTimeoutMs = 10000;
    options.responseTimeoutMs = 30000;
    options.transferTimeoutMs = 60000;
    return options;
}

/// The server path a fixture name maps to. One path per fixture, so that two
/// fixtures cannot share a revision counter or a request log entry.
std::string PathFor(const std::string& name) { return "/" + name; }

/// The reverse, for `republish`: the suite hands back the identifier it was
/// given, and the server is addressed by path.
std::string PathOf(const std::string& identifier) {
    const std::string base = g_server->BaseUrl();
    if (identifier.compare(0, base.size(), base) != 0) return std::string();
    return identifier.substr(base.size());
}

/// A new strong validator per revision. Monotonic rather than random: a fixture
/// whose identity differs between runs cannot be reasoned about from a log.
std::string NextEtag() {
    static int revision = 0;
    return "\"rev-" + std::to_string(++revision) + "\"";
}

usdassetboundary::BackendUnderTest MakeHttpBackend() {
    usdassetboundary::BackendUnderTest backend;
    backend.name = "http";

    backend.open = [](const std::string& identifier) {
        return usdasset::http::OpenAsset(identifier, SuiteOptions());
    };

    backend.provision = [](const usdassetboundary::FixtureRequest& request) {
        usdassetboundary::ProvisionedAsset asset;

        // Read back through the suite's own oracle reader rather than
        // regenerated here. The bytes the server serves are then provably the
        // bytes the oracle will compare against, and a mistake in this file
        // cannot make the two agree on content neither was given.
        const std::vector<unsigned char> content =
            usdassetboundary::NaiveReadAll(request.oraclePath);

        usdassetfixture::AssetSpec spec;
        spec.path = PathFor(request.name);
        spec.content = content;
        spec.etag = NextEtag();
        spec.behavior = usdassetfixture::Behavior::Normal;

        if (request.behavior == usdassetboundary::FixtureBehavior::ShortReadBelowEof) {
            // "A transport that accepts the range, delivers fewer bytes than it
            // covers, and stops below the end of the asset" is not a mock here.
            // It is a server that really does it, on a real socket, and the
            // corpus already proved over a raw socket that it does.
            spec.behavior = usdassetfixture::Behavior::TruncatedBody;
        }

        g_server->Serve(spec);
        asset.identifier = g_server->Url(spec.path);
        return asset;
    };

    // The read contract carries no cancellation token, so a backend admits
    // cancellation only if its transport can be told out of band. This one
    // cannot: declared by the row rather than discovered by a test that quietly
    // skips itself.
    backend.admitsCancellation = false;

    // It can, and this is the case the whole release turns on: an asset
    // republished underneath an open reader must produce AssetChanged and never
    // a byte sequence composed from two revisions.
    backend.simulatesRevisionChange = true;
    backend.republish = [](const std::string& identifier,
                           const std::vector<unsigned char>& content) {
        const std::string path = PathOf(identifier);
        if (path.empty()) return false;
        // Content and validator move together, because they are one fact. A
        // fixture that could publish bytes without publishing an identity would
        // be able to produce a state no origin server can.
        return g_server->Republish(path, content, NextEtag());
    };

    return backend;
}

}  // namespace

int main(int argc, char** argv) {
    std::string error;
    std::unique_ptr<usdassetfixture::Server> server =
        usdassetfixture::Server::Start(&error);
    if (!server) {
        // Reported as what it is. A row that cannot bind loopback must say so
        // rather than report a backend that failed the contract.
        std::fprintf(stderr, "FAIL: the fixture server could not bind loopback: %s\n",
                     error.c_str());
        return 1;
    }
    g_server = server.get();

    const int failures = usdassetboundary::RunBoundarySuite(MakeHttpBackend(), argc, argv);

    // Before the server is destroyed, and while the readers the suite opened are
    // already gone. A stalled connection must not be able to hold this process
    // open past the end of its run.
    server->Stop();
    g_server = nullptr;
    return failures;
}
