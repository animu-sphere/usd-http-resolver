// SPDX-License-Identifier: Apache-2.0
//
// A deliberately naive HTTP client, for asserting what the fixture server
// actually puts on the wire.
//
// It shares the socket layer with the server -- `send` is not the thing under
// test -- and shares no HTTP code with it at all. Requests are written as
// literal bytes and responses are parsed here, so that a framing assertion is
// two independent expressions of one rule rather than one expression checked
// against itself. This is the same separation the boundary suite keeps between
// its oracle and `usdAssetLocal`, and for the same reason: without it, a
// corpus that claims to serve a wrong `Content-Range` and a parser that agrees
// about what "wrong" means would both be wrong together and pass.
//
// It is not a model for the HTTP backend. It has no retry, no redirect
// following, no validator handling, and no opinion about what a short body
// means -- it reports what arrived and stops.

#ifndef USDASSETFIXTURE_TESTS_RAWCLIENT_H
#define USDASSETFIXTURE_TESTS_RAWCLIENT_H

#include <string>
#include <utility>
#include <vector>

#include "Socket.h"

namespace usdassetfixturetest {

/// How the response ended. The corpus cares about the difference: a body that
/// stopped short of its own `Content-Length` is a different fact from one that
/// had no `Content-Length` to stop short of.
enum class ResponseEnd {
    Complete,     ///< Content-Length was satisfied.
    Closed,       ///< The peer sent FIN first.
    Reset,        ///< The peer sent RST first.
    TimedOut,     ///< The deadline elapsed with the response unfinished.
    Error,
    NoResponse,   ///< The connection ended before any header arrived.
};

const char* ResponseEndName(ResponseEnd end) noexcept;

struct RawResponse {
    bool gotHead = false;
    int status = 0;
    std::string reason;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<unsigned char> body;
    ResponseEnd end = ResponseEnd::NoResponse;

    /// Milliseconds from the request being sent to the blank line arriving,
    /// and to the body finishing. The two slow behaviors differ only in which
    /// of these grows, which is why `Timeout` is required to name the deadline.
    long headElapsedMs = 0;
    long bodyElapsedMs = 0;

    /// Case-insensitive; empty when absent. `HasHeader` is separate because
    /// "absent" and "present and empty" are different facts and the corpus
    /// asserts absence -- of `Accept-Ranges`, of `Content-Length`, of `ETag`.
    std::string Header(const std::string& name) const;
    bool HasHeader(const std::string& name) const;
};

/// One connection, reusable across requests so that keep-alive is testable.
class RawClient {
public:
    explicit RawClient(unsigned short port);
    ~RawClient();

    RawClient(const RawClient&) = delete;
    RawClient& operator=(const RawClient&) = delete;

    bool Connected() const noexcept;
    const std::string& Error() const noexcept { return _error; }

    bool Send(const std::string& raw);

    /// `expectBody` false for a `HEAD`, whose response carries `Content-Length`
    /// and no body. Told rather than inferred: this client does not remember
    /// what it sent, and a client that waited for a HEAD's body would hang on
    /// a correct server -- which would make the corpus's own timeouts
    /// meaningless.
    RawResponse ReadResponse(int timeoutMs, bool expectBody = true);

    void Close();

private:
    usdassetfixture::detail::Platform _platform;
    // Not in the constructor's initializer list: `Connect` writes into
    // `_error`, which is declared after this and would not yet be constructed.
    usdassetfixture::detail::NativeSocket _socket =
        usdassetfixture::detail::InvalidSocket();
    std::string _buffered;
    std::string _error;
};

/// `GET <target> HTTP/1.1` with a `Host` and whatever else is asked for.
std::string GetRequest(const std::string& target,
                       const std::vector<std::pair<std::string, std::string>>& headers = {});

std::string HeadRequest(const std::string& target,
                        const std::vector<std::pair<std::string, std::string>>& headers = {});

/// Connect, send one request, read one response, disconnect.
RawResponse FetchOnce(unsigned short port,
                      const std::string& raw,
                      int timeoutMs = 5000,
                      bool expectBody = true);

}  // namespace usdassetfixturetest

#endif  // USDASSETFIXTURE_TESTS_RAWCLIENT_H
