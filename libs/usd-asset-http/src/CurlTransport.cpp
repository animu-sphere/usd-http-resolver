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

#if defined(_WIN32)
// `curl.h` has already included Winsock on Windows, which is where
// `sockaddr_in6` and `socket` live there. `windows.h` is for
// `SetHandleInformation`.
#include <windows.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Destination.h"
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

    /// Header bytes delivered so far on this exchange, interim responses
    /// included, and whether they ran past `kMaxResponseHeaderBytes`.
    std::size_t headerBytes = 0;
    bool headersTooLarge = false;

    /// The destination policy, and what it decided about each address libcurl
    /// offered. Counted rather than flagged, because a name can resolve to
    /// several addresses and libcurl tries them in turn: only an exchange on
    /// which *every* address was refused is a refusal. One on which a refused
    /// IPv6 address was followed by a permitted IPv4 one that did not answer
    /// is a connection failure, and saying otherwise would send an operator to
    /// the wrong setting.
    DestinationPolicy destinations;
    int addressesRefused = 0;
    int addressesAdmitted = 0;
    std::optional<AddressClass> refusedClass;

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

    // Counted before the line is copied or stored, so that the bound holds for
    // the allocation it exists to prevent rather than for the one after it.
    // Returning anything other than `bytes` aborts the transfer, which libcurl
    // reports as a write error; `headersTooLarge` is what tells that apart from
    // the body bound's own abort below.
    if (bytes > kMaxResponseHeaderBytes - exchange.headerBytes) {
        exchange.headersTooLarge = true;
        return 0;
    }
    exchange.headerBytes += bytes;

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

/// Reads the numeric address out of what libcurl is about to connect to. False
/// for a family the destination policy cannot classify.
///
/// Copied out rather than cast in place. `curl_sockaddr::addr` is declared as a
/// plain `sockaddr`, sixteen bytes, and libcurl stores a `sockaddr_in6` there
/// in storage it sized for one; `addrlen` is the length that is actually
/// valid, and it is checked before a byte past the declared member is read.
bool ClassifySocketAddress(const curl_sockaddr& address, AddressClass* out) {
    const unsigned char* raw = reinterpret_cast<const unsigned char*>(&address.addr);
    if (address.family == AF_INET && address.addrlen >= sizeof(sockaddr_in)) {
        sockaddr_in v4;
        std::memcpy(&v4, raw, sizeof(v4));
        std::array<std::uint8_t, 4> bytes{};
        std::memcpy(bytes.data(), &v4.sin_addr, bytes.size());
        *out = ClassifyIPv4(bytes);
        return true;
    }
    if (address.family == AF_INET6 && address.addrlen >= sizeof(sockaddr_in6)) {
        sockaddr_in6 v6;
        std::memcpy(&v6, raw, sizeof(v6));
        std::array<std::uint8_t, 16> bytes{};
        std::memcpy(bytes.data(), &v6.sin6_addr, bytes.size());
        *out = ClassifyIPv6(bytes);
        return true;
    }
    return false;
}

/// The destination policy's connect-time half.
///
/// libcurl calls this with each address it is about to connect to -- after the
/// name was resolved, before a socket exists -- which is the one point at which
/// the address is both known and not yet reached. That is what makes the policy
/// hold for a name that resolves to a refused address, for a name whose answer
/// changed between two lookups, and for every legacy spelling of an address a
/// system resolver accepts: none of those is visible in the URL, and all of them
/// are visible here.
///
/// Refusing is returning `CURL_SOCKET_BAD`, which libcurl treats as a failed
/// connect and moves on to the next address, if there is one. The counts on the
/// exchange are what tell a refusal apart from a network that did not answer.
curl_socket_t OnOpenSocket(void* userdata, curlsocktype purpose, curl_sockaddr* address) {
    Exchange& exchange = *static_cast<Exchange*>(userdata);

    AddressClass addressClass = AddressClass::Public;
    const bool classified = purpose == CURLSOCKTYPE_IPCXN && address != nullptr &&
                            ClassifySocketAddress(*address, &addressClass);
    if (!classified || !exchange.destinations.Permits(addressClass)) {
        ++exchange.addressesRefused;
        exchange.refusedClass =
            classified ? std::optional<AddressClass>(addressClass) : std::nullopt;
        return CURL_SOCKET_BAD;
    }
    ++exchange.addressesAdmitted;

    // Created here, which means created without whatever libcurl would have
    // done itself -- so the one property a host depends on is set here too. A
    // DCC that forks render workers or shell tools while a reader holds a
    // connection must not hand that socket to every child it starts.
#if defined(_WIN32)
    const curl_socket_t created =
        socket(address->family, address->socktype, address->protocol);
    if (created != CURL_SOCKET_BAD) {
        SetHandleInformation(reinterpret_cast<HANDLE>(created), HANDLE_FLAG_INHERIT, 0);
    }
#elif defined(SOCK_CLOEXEC)
    // Atomically, where the platform can: a fork between `socket` and `fcntl`
    // would inherit the descriptor anyway.
    const curl_socket_t created =
        socket(address->family, address->socktype | SOCK_CLOEXEC, address->protocol);
#else
    const curl_socket_t created =
        socket(address->family, address->socktype, address->protocol);
    if (created != CURL_SOCKET_BAD) fcntl(created, F_SETFD, FD_CLOEXEC);
#endif
    return created;
}

bool HasNonAscii(const std::string& text) noexcept {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) >= 0x80) return true;
    }
    return false;
}

/// The destination policy's pre-flight half, as the client will see the host.
///
/// The protocol layer judges a literal it can read, and reads canonical
/// spellings only. That is not enough through a proxy, where the connect-time
/// check sees the proxy's address and the host goes out as text: libcurl reads
/// `2852039166`, `0xa9fea9fe`, `169.254.43518`, and `%31%36%39.254.169.254` as
/// 169.254.169.254, and normalizes each to it before the proxy ever sees the
/// request. So the host is taken from libcurl's own URL parser -- the one
/// `curl_easy_perform` will use on the same string -- and judged as that.
///
/// A host that is not ASCII is asked for in the ASCII form the client would put
/// on the wire, where this libcurl can produce one; a compatibility mapping can
/// turn look-alike digits into an address. A libcurl without IDN support sends
/// such a host as written, and what a proxy then makes of it is the proxy's.
///
/// Returns false, with the refused class, when the policy refuses the host.
/// Anything this cannot read -- a URL libcurl will itself refuse, an allocation
/// that failed -- is left to the transfer and the connect-time check.
bool PermittedByClient(const std::string& url, const DestinationPolicy& policy,
                       std::optional<AddressClass>* refusedOut) {
    CURLU* parsed = curl_url();
    if (parsed == nullptr) return true;

    std::string host;
    if (curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK) {
        char* text = nullptr;
        if (curl_url_get(parsed, CURLUPART_HOST, &text, 0) == CURLUE_OK && text != nullptr) {
            host = text;
        }
        curl_free(text);
#if LIBCURL_VERSION_NUM >= 0x075800
        if (HasNonAscii(host)) {
            char* ascii = nullptr;
            if (curl_url_get(parsed, CURLUPART_HOST, &ascii, CURLU_PUNYCODE) == CURLUE_OK &&
                ascii != nullptr) {
                host = ascii;
            }
            curl_free(ascii);
        }
#endif
    }
    curl_url_cleanup(parsed);

    AddressClass addressClass = AddressClass::Public;
    if (!host.empty() && ClassifyHostLiteral(host, &addressClass) &&
        !policy.Permits(addressClass)) {
        *refusedOut = addressClass;
        return false;
    }
    return true;
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

#if LIBCURL_VERSION_NUM >= 0x080600
        case CURLE_TOO_LARGE:
            // libcurl's own ceilings -- one header line past 100 KiB, or a
            // block past its total -- fire before `OnHeader` ever sees the
            // line, so the refusal is the library's rather than this file's.
            // It is the same fact about the server, and it is reported as one.
            // Older libcurl names it something vaguer, and that is classified
            // below as a transport fault: failed closed either way, and only
            // the words differ.
            return TransportError::HeadersTooLarge;
#endif

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

        // Before a handle, a connection, or a byte: a host the client would
        // send to a refused address is not sent anywhere, proxy or not.
        std::optional<AddressClass> refused;
        if (!PermittedByClient(request.url, request.destinations, &refused)) {
            response.error = TransportError::DestinationRefused;
            response.refusedClass = refused;
            return response;
        }

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
        exchange.destinations = request.destinations;

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

        // The scheme allowlist, a second time. The parser above this seam is
        // what enforces it, for an identifier and for every `Location`; this
        // makes the client refuse too, so that a parser that ever widened
        // would widen into a refusal rather than into a `file:` read.
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(handle, CURLOPT_PROTOCOLS,
                         static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

        curl_easy_setopt(handle, CURLOPT_OPENSOCKETFUNCTION, &OnOpenSocket);
        curl_easy_setopt(handle, CURLOPT_OPENSOCKETDATA, &exchange);

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

        if (code != CURLE_OK && exchange.addressesRefused > 0 &&
            exchange.addressesAdmitted == 0) {
            // Every address was the policy's to refuse, and it refused them
            // all. Whatever code libcurl chose for "no connection could be
            // made" is beside the point; nothing was attempted.
            response.error = TransportError::DestinationRefused;
            response.refusedClass = exchange.refusedClass;
        } else if (code == CURLE_WRITE_ERROR && exchange.headersTooLarge) {
            // Abandoned by `OnHeader`, at the bound. The status line may well
            // have arrived and been perfectly ordinary; what the response did
            // not do was finish describing itself within the space it was
            // given, and nothing it said is worth acting on.
            //
            // So none of it is handed up. The table holds the first 64 KiB of
            // a block that did not end, and a `Content-Length` or an
            // `Accept-Ranges` found in it would be a fact read out of a
            // response nobody finished receiving.
            response.error = TransportError::HeadersTooLarge;
            response.headers.Clear();
        } else if (code == CURLE_WRITE_ERROR && exchange.overflowed) {
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
