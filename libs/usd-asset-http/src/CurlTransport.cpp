// SPDX-License-Identifier: Apache-2.0
//
// The libcurl transport.
//
// This is the only translation unit in the repository that includes `curl.h`,
// and `usdAssetHttp` is the only module permitted to name an HTTP client at all
// (WORKSPACE.md §2, invariant 4). Everything libcurl-shaped stops here: the
// layer above sees a status, a header table, bytes, and a `TransportError`.
//
// Almost every option set below turns a convenience *off*. That is the reason
// ADR-0003 chose this client: `FOLLOWLOCATION` stays off so redirects can be
// bounded and counted here; the raw status is read so that a `200` answering a
// `Range` can be `RangeNotSupported` rather than data; `Content-Range` is
// handed up before anything interprets it; and no error string libcurl produces
// ever reaches a message, because those embed the effective URL and would undo
// the credential elision this project already ships.

#include <curl/curl.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Transport.h"

namespace usdasset {
namespace http {
namespace {

/// Initializes libcurl once per process, before any handle exists.
///
/// There is no matching `curl_global_cleanup`. Cleanup is not thread-safe
/// against handles that still exist, and a static destructor cannot know that
/// no reader is mid-read: leaking a global at exit is strictly better than
/// tearing one out from under a thread that is still using it.
void EnsureGlobalInit() {
    static const bool initialized = [] {
        return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    }();
    (void)initialized;
}

/// Per-request state the callbacks write into.
struct Exchange {
    HeaderTable headers;
    /// Set once the blank line that ends the header block has arrived. This is
    /// what separates "the server never answered" from "the server answered and
    /// then stalled", which `Timeout` is required to name.
    bool headersComplete = false;

    unsigned char* body = nullptr;
    std::size_t capacity = 0;
    std::size_t written = 0;
    bool overflowed = false;

    /// Deadlines enforced by the progress callback rather than by libcurl,
    /// because libcurl has one transfer timeout and this backend owes its
    /// caller two: a server that accepts a connection and then says nothing,
    /// and a server that starts a body and stalls inside it, are different
    /// faults with different fixes.
    int responseMs = 0;
    int transferMs = 0;
    TransportError deadline = TransportError::None;
};

std::size_t OnHeader(char* data, std::size_t size, std::size_t count, void* userdata) {
    Exchange& exchange = *static_cast<Exchange*>(userdata);
    const std::size_t bytes = size * count;
    std::string line(data, bytes);

    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }

    if (line.empty()) {
        exchange.headersComplete = true;
        return bytes;
    }
    if (line.compare(0, 5, "HTTP/") == 0) {
        // A new status line. Anything already collected belonged to an earlier
        // response on this exchange -- a `100 Continue`, or an intermediate hop
        // if one were ever followed -- and keeping it would let a header from
        // the wrong response answer a framing question.
        exchange.headers.Clear();
        exchange.headersComplete = false;
        return bytes;
    }

    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) return bytes;  // Not a header. Ignored.

    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    exchange.headers.Add(std::move(name), value.substr(start));
    return bytes;
}

std::size_t OnBody(char* data, std::size_t size, std::size_t count, void* userdata) {
    Exchange& exchange = *static_cast<Exchange*>(userdata);
    const std::size_t bytes = size * count;

    const std::size_t room = exchange.capacity - exchange.written;
    const std::size_t take = bytes < room ? bytes : room;
    if (take > 0) {
        std::memcpy(exchange.body + exchange.written, data, take);
        exchange.written += take;
    }
    if (take < bytes) {
        // The bound §10 of the design policy requires: never allocate from a
        // server-declared length. Returning short aborts the transfer, so a
        // server answering a 64 KiB range request with a 10 GB body moves
        // 64 KiB and then stops -- rather than the whole-asset download this
        // project exists to avoid.
        exchange.overflowed = true;
        return take;
    }
    return bytes;
}

/// The progress callback needs the handle to read elapsed time from, so the
/// two travel together.
struct ProgressContext {
    Exchange* exchange = nullptr;
    CURL* handle = nullptr;
};

/// libcurl calls this on its own cadence while a transfer waits, which is what
/// makes it a usable clock for the two deadlines libcurl does not itself
/// implement. The easy handle carries the elapsed time, so nothing here keeps
/// one of its own.
int OnProgressWithClock(void* userdata,
                        curl_off_t /*downloadTotal*/,
                        curl_off_t /*downloadNow*/,
                        curl_off_t /*uploadTotal*/,
                        curl_off_t /*uploadNow*/) {
    ProgressContext& context = *static_cast<ProgressContext*>(userdata);
    Exchange& exchange = *context.exchange;

    curl_off_t totalUs = 0;
    if (curl_easy_getinfo(context.handle, CURLINFO_TOTAL_TIME_T, &totalUs) != CURLE_OK) {
        return 0;
    }
    const curl_off_t totalMs = totalUs / 1000;

    // The transfer deadline is the whole exchange, so it is measured from the
    // start of it.
    if (exchange.transferMs > 0 && totalMs > exchange.transferMs) {
        exchange.deadline = exchange.headersComplete ? TransportError::TransferTimeout
                                                     : TransportError::ResponseTimeout;
        return 1;
    }

    // The response deadline is not. It is "connection established to status
    // line received" -- the interval a *connect* deadline does not cover -- so
    // measuring it from the start of the exchange charges DNS, TCP, and the TLS
    // handshake against it, and fires up to `connectTimeoutMs` early with a
    // message naming the wrong deadline. Pre-transfer time is when the request
    // went out; before that there is nothing for this deadline to be about, and
    // `CURLOPT_CONNECTTIMEOUT_MS` is what bounds it.
    if (!exchange.headersComplete && exchange.responseMs > 0) {
        curl_off_t pretransferUs = 0;
        if (curl_easy_getinfo(context.handle, CURLINFO_PRETRANSFER_TIME_T,
                              &pretransferUs) == CURLE_OK &&
            pretransferUs > 0 && (totalUs - pretransferUs) / 1000 > exchange.responseMs) {
            exchange.deadline = TransportError::ResponseTimeout;
            return 1;
        }
    }
    return 0;
}

TransportError ClassifyCurlError(CURLcode code, const Exchange& exchange, bool connected) {
    switch (code) {
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CIPHER:
            return TransportError::ConnectFailed;

        case CURLE_OPERATION_TIMEDOUT:
            // libcurl reports one code for every deadline it owns. Which one
            // elapsed is recoverable from how far the exchange got, and
            // `HTTP006` is required to say.
            if (!connected) return TransportError::ConnectTimeout;
            return exchange.headersComplete ? TransportError::TransferTimeout
                                            : TransportError::ResponseTimeout;

        case CURLE_ABORTED_BY_CALLBACK:
            return exchange.deadline != TransportError::None
                       ? exchange.deadline
                       : TransportError::ConnectionLost;

        case CURLE_PARTIAL_FILE:
            // The connection closed cleanly with fewer body bytes than the
            // framing promised. A different fact about the server than a reset,
            // and the corpus has a row for each.
            return TransportError::IncompleteBody;

        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_GOT_NOTHING:
            return TransportError::ConnectionLost;

        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_URL_MALFORMAT:
        case CURLE_WEIRD_SERVER_REPLY:
            return TransportError::Malformed;

        case CURLE_OUT_OF_MEMORY:
            return TransportError::Internal;

        default:
            // Unrecognized rather than unclassified. Whether a connection was
            // ever established is the one thing that is always known, and it is
            // the distinction the layer above actually acts on.
            return connected ? TransportError::ConnectionLost
                             : TransportError::ConnectFailed;
    }
}

/// A pool of easy handles, one checked out per in-flight request.
///
/// ADR-0003 records "one easy handle per reader". That is one handle short of
/// what the boundary suite requires: an easy handle may not be used by two
/// threads at once, and the suite runs many threads on *one* reader, so a
/// single handle would either be a data race or a mutex that serializes every
/// concurrent read on an asset. The pool is that consequence's refinement --
/// still per reader, still never shared across readers, and still reusing
/// connections, because a checked-in handle keeps its connection for the next
/// request that checks it out.
class HandlePool {
public:
    ~HandlePool() {
        for (CURL* handle : _idle) curl_easy_cleanup(handle);
    }

    CURL* Acquire() {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_idle.empty()) {
                CURL* handle = _idle.back();
                _idle.pop_back();
                curl_easy_reset(handle);
                return handle;
            }
        }
        return curl_easy_init();
    }

    void Release(CURL* handle) {
        if (handle == nullptr) return;
        std::lock_guard<std::mutex> lock(_mutex);
        if (_idle.size() >= kMaxIdle) {
            // Bounded, so that a burst of concurrency does not leave a
            // long-lived reader holding a socket per thread forever.
            curl_easy_cleanup(handle);
            return;
        }
        _idle.push_back(handle);
    }

private:
    static constexpr std::size_t kMaxIdle = 16;

    std::mutex _mutex;
    std::vector<CURL*> _idle;
};

class CurlTransport final : public Transport {
public:
    CurlTransport() { EnsureGlobalInit(); }

    TransportResponse Perform(const TransportRequest& request) override {
        TransportResponse response;

        CURL* handle = _pool.Acquire();
        if (handle == nullptr) {
            response.error = TransportError::Internal;
            return response;
        }

        Exchange exchange;
        exchange.body = static_cast<unsigned char*>(request.body);
        exchange.capacity = request.body == nullptr ? 0 : request.bodyCapacity;
        exchange.responseMs = request.timeouts.responseMs;
        exchange.transferMs = request.timeouts.transferMs;

        ProgressContext progress;
        progress.exchange = &exchange;
        progress.handle = handle;

        // Every append is checked, and the whole request is abandoned if one
        // fails. `curl_slist_append` returns null on allocation failure and
        // does *not* free what it was given, so the obvious
        // `list = curl_slist_append(list, ...)` both leaks the list and drops
        // every header silently -- and a dropped `Range` does not fail. It
        // succeeds, as a `200` carrying the whole representation, which this
        // backend would then correctly report as `RangeNotSupported`: a
        // transient allocation failure wearing the name of a terminal server
        // capability.
        curl_slist* headers = nullptr;
        bool headersBuilt = true;
        const auto appendHeader = [&headers, &headersBuilt](const std::string& line) {
            if (!headersBuilt) return;
            curl_slist* appended = curl_slist_append(headers, line.c_str());
            if (appended == nullptr) {
                headersBuilt = false;
                return;
            }
            headers = appended;
        };

        if (!request.range.empty()) appendHeader("Range: " + request.range);
        if (!request.ifRange.empty()) appendHeader("If-Range: " + request.ifRange);
        // Identity encoding, always. A compressed range response would make the
        // byte accounting below describe the wire rather than the asset, and
        // every framing check in this backend is arithmetic about asset offsets.
        appendHeader("Accept-Encoding: identity");

        if (!headersBuilt) {
            curl_slist_free_all(headers);
            _pool.Release(handle);
            response.error = TransportError::Internal;
            return response;
        }

        curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(handle, CURLOPT_USERAGENT, request.userAgent.c_str());

        // Off, and this is the option the ADR turns on the choice of client: a
        // chain the library follows silently is a chain the hostile corpus
        // cannot test and this repository's counter cannot bound.
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);

        if (request.method == Method::Head) {
            curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
        } else {
            curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
        }

        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, &OnHeader);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &exchange);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &OnBody);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &exchange);

        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, &OnProgressWithClock);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &progress);

        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS,
                         static_cast<long>(request.timeouts.connectMs));
        // A backstop under the callback's deadlines, not a replacement for
        // them: libcurl's own timeout cannot distinguish which one elapsed.
        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS,
                         static_cast<long>(request.timeouts.transferMs));

        // Required for use from a thread: libcurl's alarm-based DNS timeout is
        // not thread-safe, and this backend is called from Hydra's threads.
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);

        const CURLcode code = curl_easy_perform(handle);

        long status = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);

        // Pre-transfer, not connect. `CURLINFO_CONNECT_TIME_T` measures a
        // connection this transfer established, and is zero when the transfer
        // reused one from the pool -- which, once connection reuse works, is
        // most of them. Reading it as "was there a connection" reports every
        // deadline on a reused connection as a *connect* timeout, which is the
        // one deadline that provably did not elapse.
        //
        // Pre-transfer time is non-zero once the request has been sent,
        // whichever way the connection was obtained, and zero when no
        // connection was ever had.
        curl_off_t pretransferTime = 0;
        curl_easy_getinfo(handle, CURLINFO_PRETRANSFER_TIME_T, &pretransferTime);

        response.status = static_cast<int>(status);
        response.headers = std::move(exchange.headers);
        response.bodyBytes = exchange.written;
        response.bodyOverflowed = exchange.overflowed;
        response.connected = pretransferTime > 0;

        if (code == CURLE_WRITE_ERROR && exchange.overflowed) {
            // Not a failure. The transfer was cut off deliberately, by this
            // file, because the server had more to send than the caller was
            // willing to receive. Whether that is an error depends on what the
            // caller asked for, and only the caller knows.
            response.error = TransportError::None;
        } else if (code != CURLE_OK) {
            response.error = ClassifyCurlError(code, exchange, response.connected);
        }

        curl_slist_free_all(headers);
        _pool.Release(handle);
        return response;
    }

private:
    HandlePool _pool;
};

}  // namespace

std::unique_ptr<Transport> MakeCurlTransport() {
    return std::unique_ptr<Transport>(new CurlTransport());
}

}  // namespace http
}  // namespace usdasset
