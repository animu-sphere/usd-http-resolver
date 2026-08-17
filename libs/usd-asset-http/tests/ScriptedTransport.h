// SPDX-License-Identifier: Apache-2.0
//
// A transport that answers from a script, and records what it was asked.
//
// This is not a substitute for the hostile corpus, and it is deliberately not
// used for anything the corpus can produce. `tests/corpus` runs the backend
// against a real server that really truncates a body and really resets a
// connection; a mock asserting a short read would be this module checking its
// own fiction.
//
// What it is for is the rules no server can be asked to demonstrate:
//
//   a scheme downgrade         the fixture server speaks plaintext HTTP, so
//                              there is no `https` to be downgraded *from*.
//                              The CHANGELOG names this as absent from the
//                              corpus and belonging here, and this is here
//   a redirect chain, counted  the corpus proves a bound exists; this proves
//                              the bound is the configured number and that the
//                              hop after it is refused
//   a client that never
//   connects                   a socket that fails before the server sees it
//   the exact request shape    that `If-Range` carries the captured validator
//                              verbatim, and that no header is sent that the
//                              backend did not intend

#ifndef USDASSETHTTP_TESTS_SCRIPTEDTRANSPORT_H
#define USDASSETHTTP_TESTS_SCRIPTEDTRANSPORT_H

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "TestSupport.h"
#include "Transport.h"

namespace usdassethttptest {

using usdasset::http::HeaderTable;
using usdasset::http::Method;
using usdasset::http::Transport;
using usdasset::http::TransportError;
using usdasset::http::TransportRequest;
using usdasset::http::TransportResponse;

/// One request, as the transport received it. The shape of what was *sent* is
/// half of what this module has to get right, and it is unassertable from a
/// response.
struct SentRequest {
    std::string url;
    Method method = Method::Get;
    std::string range;
    std::string ifRange;
    std::size_t capacity = 0;
};

/// The script: a function of the request and how many have come before it.
using Responder = std::function<TransportResponse(const TransportRequest&, int)>;

/// Shared between the transport the backend owns and the test that scripted it.
/// The reader takes ownership of its transport, so the log has to outlive it.
struct Script {
    Responder responder;
    std::vector<SentRequest> sent;
    std::mutex mutex;

    int Count() {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<int>(sent.size());
    }
};

class ScriptedTransport final : public Transport {
public:
    explicit ScriptedTransport(std::shared_ptr<Script> script)
        : _script(std::move(script)) {}

    TransportResponse Perform(const TransportRequest& request) override {
        int index = 0;
        {
            std::lock_guard<std::mutex> lock(_script->mutex);
            SentRequest sent;
            sent.url = request.url;
            sent.method = request.method;
            sent.range = request.range;
            sent.ifRange = request.ifRange;
            sent.capacity = request.bodyCapacity;
            _script->sent.push_back(std::move(sent));
            index = static_cast<int>(_script->sent.size()) - 1;
        }
        return _script->responder(request, index);
    }

private:
    std::shared_ptr<Script> _script;
};

inline usdasset::http::testing::MakeTransport Factory(std::shared_ptr<Script> script) {
    return [script] {
        return std::unique_ptr<Transport>(new ScriptedTransport(script));
    };
}

/// A well-formed metadata response for an asset of `size` bytes.
inline TransportResponse MetadataResponse(std::uint64_t size, const std::string& etag) {
    TransportResponse response;
    response.status = 200;
    response.connected = true;
    response.headers.Add("Accept-Ranges", "bytes");
    response.headers.Add("Content-Length", std::to_string(size));
    if (!etag.empty()) response.headers.Add("ETag", etag);
    response.headers.Add("Content-Type", "application/octet-stream");
    return response;
}

/// A well-formed partial response covering exactly what was asked for, filled
/// with `fill` so that a caller can tell delivered bytes from an untouched
/// buffer.
inline TransportResponse PartialResponse(const TransportRequest& request,
                                         std::uint64_t first,
                                         std::uint64_t assetSize,
                                         const std::string& etag,
                                         unsigned char fill) {
    TransportResponse response;
    response.status = 206;
    response.connected = true;
    const std::size_t length = request.bodyCapacity;
    response.headers.Add("Content-Range",
                         "bytes " + std::to_string(first) + "-" +
                             std::to_string(first + length - 1) + "/" +
                             std::to_string(assetSize));
    response.headers.Add("Content-Length", std::to_string(length));
    if (!etag.empty()) response.headers.Add("ETag", etag);
    if (request.body != nullptr && length > 0) {
        std::memset(request.body, fill, length);
    }
    response.bodyBytes = length;
    return response;
}

inline TransportResponse RedirectResponse(const std::string& location) {
    TransportResponse response;
    response.status = 302;
    response.connected = true;
    response.headers.Add("Location", location);
    response.headers.Add("Content-Length", "0");
    return response;
}

inline TransportResponse TransportFailure(TransportError error) {
    TransportResponse response;
    response.error = error;
    return response;
}

}  // namespace usdassethttptest

#endif  // USDASSETHTTP_TESTS_SCRIPTEDTRANSPORT_H
