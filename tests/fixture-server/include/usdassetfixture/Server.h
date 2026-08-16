// SPDX-License-Identifier: Apache-2.0
//
// The fixture server: an HTTP/1.1 origin on loopback that can be told to
// misbehave in each of the ways named in Corpus.h.
//
// It exists before the HTTP backend on purpose (design policy §17, action 1). A
// backend written before its server is a backend whose bugs are
// indistinguishable from server behavior; written after, a wrong answer at EOF
// is either the backend's or it is a fixture the corpus already proves correct.
//
// Three properties are contract, not implementation detail:
//
//   loopback only, ephemeral port    §11.2 requires the corpus to run in CI
//                                    without network access, and two lanes on
//                                    one runner must not collide on a port
//   no third-party dependency        this repository takes exactly one, chosen
//                                    on stated criteria in ADR-0003, and it is
//                                    a *client*. A server acquired to test it
//                                    would be a second, unargued dependency
//   every request is logged          bounded redirects, absent requests for a
//                                    zero-length read, and `If-Range` on every
//                                    range are all properties of what was
//                                    *sent*, and are unassertable otherwise
//
// Nothing outside a test links this.

#ifndef USDASSETFIXTURE_SERVER_H
#define USDASSETFIXTURE_SERVER_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "usdassetfixture/Corpus.h"

namespace usdassetfixture {

/// One request, as the server received it.
///
/// The headers kept are the ones a test asserts on. `Range` and `If-Range` are
/// the two the consistency guarantee in §6 of the design policy lives or dies
/// by, and they are recorded verbatim rather than parsed, so that a test can
/// catch a backend that sends a syntactically wrong header the server was
/// lenient about.
struct RequestRecord {
    std::string method;
    std::string target;  ///< Path and query, exactly as sent.
    std::string range;
    std::string ifRange;
    std::string ifNoneMatch;
    std::string host;
    std::string userAgent;

    /// The status the server answered with. `0` when it deliberately never
    /// answered -- the reset and the still-stalling cases.
    int status = 0;
};

/// An origin server on 127.0.0.1, with an ephemeral port.
class Server {
public:
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Binds and starts accepting. Returns null with the reason in `*errorOut`
    /// on failure; a test that cannot bind loopback must say so rather than
    /// report a backend failure.
    static std::unique_ptr<Server> Start(std::string* errorOut);

    unsigned short Port() const noexcept;

    /// `http://127.0.0.1:<port>`, no trailing slash.
    std::string BaseUrl() const;

    /// `BaseUrl()` + `path`. `path` carries its own leading slash.
    std::string Url(const std::string& path) const;

    /// Registers an asset, replacing any already at `spec.path`. Legal while
    /// the server is running and while readers are open.
    void Serve(const AssetSpec& spec);

    /// Replaces content and validator underneath open readers.
    ///
    /// This is the HTTP backend's revision-simulation declaration in the
    /// boundary suite (BOUNDARY_SUITE.md §6). Returns false when `path` names
    /// no asset: a fixture that failed to change must not be reported as a
    /// backend that failed to notice, because those are different defects in
    /// different repositories.
    bool Republish(const std::string& path,
                   const std::vector<unsigned char>& content,
                   const std::string& etag);

    /// Every request answered so far, in the order the server finished them.
    std::vector<RequestRecord> Log() const;
    std::size_t RequestCount() const;
    void ClearLog();

    /// Stops accepting, wakes every stalled connection, and joins. Idempotent,
    /// and called by the destructor. A stalled SlowBody connection must not be
    /// able to hold a test process open past the end of its case.
    void Stop();

private:
    class Impl;
    explicit Server(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> _impl;
};

}  // namespace usdassetfixture

#endif  // USDASSETFIXTURE_SERVER_H
