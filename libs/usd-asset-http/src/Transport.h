// SPDX-License-Identifier: Apache-2.0
//
// The narrow internal transport seam ADR-0003 requires.
//
// Everything above this interface is protocol: framing validation, bounded
// redirects, bounded retry, validator capture, `If-Range`, and the projection
// onto the typed vocabulary. Everything below it is one HTTP client. libcurl
// sits behind this and appears nowhere else -- not in a public header, not in
// `HttpAssetReader.cpp`, and not in a test.
//
// Two properties are the reason the seam is here rather than implied:
//
//   the ADR stays cheap to supersede    a native-stack or `fetch` transport is
//                                       a second implementation of this file's
//                                       interface, not a second backend
//   the protocol is testable offline    the redirect, retry, framing, and
//                                       revision-binding rules are exercised
//                                       against a scripted transport, with no
//                                       socket and no server involved
//
// The interface is shaped by what the hostile corpus requires a caller to be
// able to see, which is more than a convenience client exposes: the raw status
// of a response whose body is perfectly valid (ADR-0002), `Content-Range`
// before anything has interpreted it, the *absence* of `Content-Length`, and a
// redirect that was not followed.
//
// Internal to usdAssetHttp. No header outside src/ includes this.

#ifndef USDASSETHTTP_TRANSPORT_H
#define USDASSETHTTP_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace usdasset {
namespace http {

/// Response headers, looked up case-insensitively.
///
/// A vector rather than a map: a response carries a handful of headers, the
/// lookups are few and happen once per request, and preserving arrival order
/// makes a failing test's dump readable.
class HeaderTable {
public:
    void Add(std::string name, std::string value);

    /// Null when absent. Absence is a first-class answer here -- an unknown
    /// `Content-Length` is a corpus behavior, not an oversight.
    const std::string* Find(std::string_view name) const;

    /// The value, or an empty string when absent. For the cases where empty
    /// and absent lead to the same decision.
    std::string Get(std::string_view name) const;

    bool Has(std::string_view name) const { return Find(name) != nullptr; }

    void Clear() { _entries.clear(); }

    const std::vector<std::pair<std::string, std::string>>& Entries() const {
        return _entries;
    }

private:
    std::vector<std::pair<std::string, std::string>> _entries;
};

/// What went wrong below HTTP, when something did.
///
/// These are transport conditions, not diagnostics: the projection onto
/// `StatusCode` happens above, in one place, so that credential-bearing client
/// error strings never reach a message (ADR-0003, recorded consequences).
enum class TransportError {
    None,

    /// DNS, refusal, TLS handshake -- the connection was never established.
    ConnectFailed,
    /// The connect deadline elapsed before a connection existed.
    ConnectTimeout,
    /// A connection existed and the status line never arrived in time.
    ResponseTimeout,
    /// Headers arrived and the body did not finish in time.
    TransferTimeout,
    /// The peer destroyed the connection. Distinct from `IncompleteBody`
    /// because a reset and an orderly close say different things about the
    /// server, and the corpus has a row for each.
    ConnectionLost,
    /// The connection closed cleanly with fewer body bytes than the response
    /// framing promised.
    IncompleteBody,
    /// The response could not be parsed as HTTP at all.
    Malformed,
    /// The transport itself failed -- out of memory, a handle that would not
    /// initialize. Never a property of the server.
    Internal,
};

const char* TransportErrorName(TransportError error) noexcept;

enum class Method {
    Get,
    Head,
};

struct Timeouts {
    /// Establishing a connection.
    int connectMs = 10000;
    /// Connection established to status line received.
    int responseMs = 30000;
    /// The whole exchange, headers and body.
    int transferMs = 300000;
};

/// One request, fully specified. There is no implicit state: a transport does
/// not remember the last URL, does not follow a redirect on its own, and does
/// not add a conditional header the caller did not ask for.
struct TransportRequest {
    std::string url;
    Method method = Method::Get;

    /// `bytes=a-b`, verbatim. Empty means no `Range` header at all, which is
    /// not the same as `bytes=0-` and the corpus can tell the difference.
    std::string range;

    /// `If-Range` value, verbatim. Empty means the header is not sent.
    std::string ifRange;

    std::string userAgent;

    Timeouts timeouts;

    /// Where the body goes, and the bound §10 of the design policy requires:
    /// "never allocate from a server-declared length without a bound". The
    /// caller sizes this from what it asked for, so a server answering a 64 KiB
    /// range request with a 10 GB body is cut off after 64 KiB rather than
    /// buffered -- which is the exact transfer this project exists to avoid.
    void* body = nullptr;
    std::size_t bodyCapacity = 0;
};

struct TransportResponse {
    /// The status line's code, raw. `0` when no response was received.
    ///
    /// Raw is the requirement: ADR-0002 makes a `200` answering a `Range`
    /// request `RangeNotSupported`, and a client that normalizes a partial
    /// response into "here are your bytes" cannot implement that rule.
    int status = 0;

    HeaderTable headers;

    /// Bytes written into `TransportRequest::body`.
    std::size_t bodyBytes = 0;

    /// The server had more to send than `bodyCapacity` allowed, and the
    /// transfer was cut off. Not an error at this layer: whether it is one
    /// depends on what the caller asked for, which only the caller knows.
    bool bodyOverflowed = false;

    TransportError error = TransportError::None;

    /// True once a connection was established. The only thing that separates a
    /// connect deadline from a response deadline, which `Timeout` (`HTTP006`)
    /// is required to name.
    bool connected = false;
};

/// The seam itself.
///
/// One instance per reader. Implementations are required to be safe for
/// concurrent `Perform` calls on one instance, because the boundary suite runs
/// many threads on one reader and that suite passes unchanged or the release
/// does not ship (ADR-0003, context).
class Transport {
public:
    virtual ~Transport() = default;

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    /// Issues exactly one request. Never follows a redirect, never retries,
    /// and never interprets a status code. All three are policy, and policy
    /// lives above this line.
    virtual TransportResponse Perform(const TransportRequest& request) = 0;

protected:
    Transport() = default;
};

/// How a reader acquires its transport. Injectable so that the protocol rules
/// can be tested without a socket, and so that `tests/boundary`'s misbehaving
/// transport provisioning has somewhere to attach (ADR-0003).
using TransportFactory = std::unique_ptr<Transport> (*)();

/// The libcurl transport. Defined in CurlTransport.cpp, which is the only
/// translation unit in this repository that includes `curl.h`.
std::unique_ptr<Transport> MakeCurlTransport();

}  // namespace http
}  // namespace usdasset

#endif  // USDASSETHTTP_TRANSPORT_H
