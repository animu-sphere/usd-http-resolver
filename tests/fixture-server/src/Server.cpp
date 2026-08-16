// SPDX-License-Identifier: Apache-2.0

#include "usdassetfixture/Server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include "Http.h"
#include "Socket.h"

namespace usdassetfixture {
namespace {

using detail::ByteRange;
using detail::NativeSocket;
using detail::RangeParse;
using detail::ReceiveState;
using detail::Request;

/// How long a connection waits for a request before giving up on the client.
/// Generous, because a backend under ThreadSanitizer is slow and a fixture that
/// times out under instrumentation is a flaky test rather than a hostile case.
constexpr int kRequestTimeoutMs = 30000;

/// How long the accept loop blocks before rechecking the stop flag.
constexpr int kAcceptPollMs = 25;

constexpr std::size_t kMaxRequestBytes = 64 * 1024;

/// One revision of one asset: the bytes and the identity of those bytes,
/// swapped together. They are one object because they are one fact -- a fixture
/// that could publish content without publishing its validator would be able to
/// produce a state no origin server can.
struct Revision {
    std::vector<unsigned char> content;
    std::string etag;
};

/// The knobs, split from the content so that dispatch can copy them under the
/// lock without copying a multi-megabyte body per request.
struct Knobs {
    Behavior behavior = Behavior::Normal;
    bool weakValidator = false;
    std::string lastModified;
    std::int64_t deliverBytes = -1;
    int redirectHops = 1;
    int delayMs = 1000;
    int changeAfterRequests = 1;
    int transientFailures = 1;
};

struct AssetState {
    Knobs knobs;
    std::shared_ptr<const Revision> current;
    std::shared_ptr<const Revision> pending;  ///< ValidatorChangeMidRead.
    int requestsSeen = 0;
};

/// What the server decided to do about one request, computed under the lock and
/// executed outside it.
struct Plan {
    bool found = false;
    Knobs knobs;
    std::shared_ptr<const Revision> revision;
    int requestIndex = 0;  ///< 1 for the first request to this asset.
    int hop = 0;           ///< RedirectChain: hops already taken.
    std::string assetPath;
};

std::string FormatEtag(const Knobs& knobs, const Revision& revision) {
    if (revision.etag.empty()) return std::string();
    return knobs.weakValidator ? "W/" + revision.etag : revision.etag;
}

/// The `.hopN` suffix RedirectChain routes through. A path suffix rather than a
/// query parameter, so that a chain can be followed without the fixture
/// depending on a backend preserving a query string -- which is a separate
/// property, asserted separately.
const char* kHopMarker = ".hop";

}  // namespace

class Server::Impl {
public:
    ~Impl() { Stop(); }

    bool Start(std::string* errorOut) {
        _listener = detail::Listen(&_port, errorOut);
        if (!detail::IsValidSocket(_listener)) return false;
        _acceptor = std::thread([this] { AcceptLoop(); });
        return true;
    }

    unsigned short Port() const noexcept { return _port; }

    void Serve(const AssetSpec& spec) {
        auto current = std::make_shared<Revision>();
        current->content = spec.content;
        current->etag = spec.etag;

        auto pending = std::make_shared<Revision>();
        // An empty `revisedContent` reuses the current bytes on purpose: the
        // revision then differs only in identity, which is the case a backend
        // comparing bytes instead of validators cannot see.
        pending->content =
            spec.revisedContent.empty() ? spec.content : spec.revisedContent;
        pending->etag = spec.revisedEtag;

        AssetState state;
        state.knobs.behavior = spec.behavior;
        state.knobs.weakValidator = spec.weakValidator;
        state.knobs.lastModified = spec.lastModified;
        state.knobs.deliverBytes = spec.deliverBytes;
        state.knobs.redirectHops = spec.redirectHops;
        state.knobs.delayMs = spec.delayMs;
        state.knobs.changeAfterRequests = spec.changeAfterRequests;
        state.knobs.transientFailures = spec.transientFailures;
        state.current = std::move(current);
        state.pending = std::move(pending);

        std::lock_guard<std::mutex> lock(_mutex);
        _assets[spec.path] = std::move(state);
    }

    bool Republish(const std::string& path,
                   const std::vector<unsigned char>& content,
                   const std::string& etag) {
        auto revision = std::make_shared<Revision>();
        revision->content = content;
        revision->etag = etag;

        std::lock_guard<std::mutex> lock(_mutex);
        const auto found = _assets.find(path);
        if (found == _assets.end()) return false;
        found->second.current = std::move(revision);
        return true;
    }

    std::vector<RequestRecord> Log() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _log;
    }

    std::size_t RequestCount() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _log.size();
    }

    void ClearLog() {
        std::lock_guard<std::mutex> lock(_mutex);
        _log.clear();
    }

    void Stop() {
        {
            // Under `_wakeMutex`, not merely before the notify. A stalling
            // handler that has evaluated the predicate and not yet blocked
            // would otherwise miss the wakeup and hold the process for its full
            // delay -- the kind of race that is invisible locally and a hang in
            // CI.
            std::lock_guard<std::mutex> lock(_wakeMutex);
            if (_stopping.exchange(true)) return;
        }
        _wake.notify_all();

        if (_acceptor.joinable()) _acceptor.join();

        std::vector<std::thread> connections;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            connections.swap(_connections);
        }
        for (auto& connection : connections) {
            if (connection.joinable()) connection.join();
        }

        detail::Close(_listener);
        _listener = detail::InvalidSocket();
    }

private:
    // --- accept ---------------------------------------------------------------

    void AcceptLoop() {
        while (!_stopping.load()) {
            bool timedOut = false;
            const NativeSocket peer =
                detail::AcceptWithTimeout(_listener, kAcceptPollMs, &timedOut);
            if (timedOut) continue;
            if (!detail::IsValidSocket(peer)) {
                if (_stopping.load()) break;
                continue;
            }
            if (_stopping.load()) {
                detail::Close(peer);
                break;
            }

            std::lock_guard<std::mutex> lock(_mutex);
            _connections.emplace_back([this, peer] { HandleConnection(peer); });
        }
    }

    /// Interruptible sleep. Returns false when `Stop()` cut it short, which
    /// every stalling behavior checks before it writes anything else.
    bool Sleep(int milliseconds) {
        if (milliseconds <= 0) return !_stopping.load();
        std::unique_lock<std::mutex> lock(_wakeMutex);
        return !_wake.wait_for(lock, std::chrono::milliseconds(milliseconds),
                               [this] { return _stopping.load(); });
    }

    // --- one connection -------------------------------------------------------

    /// `peer` is taken by reference all the way down, because the abortive
    /// paths close it themselves. Without that, this function's own close
    /// would be a second close of the same descriptor -- which on a busy
    /// process is not a no-op but a close of whatever descriptor the number
    /// has since been reused for.
    void HandleConnection(NativeSocket peer) {
        std::string buffered;

        while (!_stopping.load()) {
            std::string head;
            if (!ReadHead(peer, &buffered, &head)) break;

            Request request;
            if (!detail::ParseRequest(head, &request)) {
                WriteSimple(peer, 405, {});
                break;
            }

            const Disposition disposition = Dispatch(&peer, request);
            if (disposition != Disposition::KeepAlive) break;
            if (!request.WantsKeepAlive()) break;
        }

        detail::Close(peer);
    }

    /// Reads bytes until the blank line that ends a header block. Leftovers
    /// stay in `*buffered` for the next request on a kept-alive connection.
    bool ReadHead(NativeSocket peer, std::string* buffered, std::string* head) {
        int idleWaits = 0;
        for (;;) {
            const std::size_t end = buffered->find("\r\n\r\n");
            if (end != std::string::npos) {
                *head = buffered->substr(0, end + 4);
                buffered->erase(0, end + 4);
                return true;
            }
            if (buffered->size() > kMaxRequestBytes) return false;
            if (_stopping.load()) return false;

            char chunk[4096];
            std::size_t received = 0;
            const ReceiveState state = detail::Receive(
                peer, chunk, sizeof(chunk), kAcceptPollMs, &received);
            if (state == ReceiveState::Data) {
                buffered->append(chunk, received);
                continue;
            }
            if (state == ReceiveState::TimedOut) {
                // Poll rather than block for the whole request timeout, so
                // that Stop() is noticed by an idle kept-alive connection too.
                // The budget is per read, not per server: a connection that
                // waited out its idle time must not shorten the next one's.
                if (++idleWaits * kAcceptPollMs > kRequestTimeoutMs) return false;
                continue;
            }
            return false;  // Closed, Reset, or Error.
        }
    }

    enum class Disposition { KeepAlive, Close, Aborted };

    // --- routing --------------------------------------------------------------

    /// Resolves a request path to an asset and, for RedirectChain, to how far
    /// along the chain it is. Computed under the lock together with the
    /// per-asset counters, so that two concurrent requests cannot both see
    /// themselves as the first.
    Plan MakePlan(const std::string& path) {
        std::lock_guard<std::mutex> lock(_mutex);

        std::string assetPath = path;
        int hop = 0;

        auto found = _assets.find(assetPath);
        if (found == _assets.end()) {
            const std::size_t marker = path.rfind(kHopMarker);
            if (marker != std::string::npos) {
                const std::string base = path.substr(0, marker);
                const std::string index = path.substr(marker + 4);
                found = _assets.find(base);
                if (found != _assets.end() && !index.empty()) {
                    assetPath = base;
                    hop = std::atoi(index.c_str());
                }
            }
        }
        if (found == _assets.end()) return Plan();

        AssetState& state = found->second;
        ++state.requestsSeen;

        if (state.knobs.behavior == Behavior::ValidatorChangeMidRead &&
            state.requestsSeen > state.knobs.changeAfterRequests) {
            state.current = state.pending;
        }

        Plan plan;
        plan.found = true;
        plan.knobs = state.knobs;
        plan.revision = state.current;
        plan.requestIndex = state.requestsSeen;
        plan.hop = hop;
        plan.assetPath = assetPath;
        return plan;
    }

    void Record(const Request& request, int status) {
        RequestRecord record;
        record.method = request.method;
        record.target = request.target;
        record.range = request.Header("Range");
        record.ifRange = request.Header("If-Range");
        record.ifNoneMatch = request.Header("If-None-Match");
        record.host = request.Header("Host");
        record.userAgent = request.Header("User-Agent");
        record.status = status;

        std::lock_guard<std::mutex> lock(_mutex);
        _log.push_back(std::move(record));
    }

    // --- dispatch -------------------------------------------------------------

    Disposition Dispatch(NativeSocket* peer, const Request& request) {
        if (request.method != "GET" && request.method != "HEAD") {
            Record(request, 405);
            return WriteSimple(*peer, 405, {}) ? Disposition::Close
                                               : Disposition::Aborted;
        }

        const Plan plan = MakePlan(request.path);
        if (!plan.found) {
            Record(request, 404);
            return WriteSimple(*peer, 404, {}) ? Disposition::KeepAlive
                                               : Disposition::Aborted;
        }

        switch (plan.knobs.behavior) {
            case Behavior::NotFound:
                Record(request, 404);
                return WriteSimple(*peer, 404, {}) ? Disposition::KeepAlive
                                                   : Disposition::Aborted;

            case Behavior::AccessDenied:
                Record(request, 403);
                return WriteSimple(*peer, 403, {}) ? Disposition::KeepAlive
                                                   : Disposition::Aborted;

            case Behavior::TransientServerError:
                if (plan.requestIndex <= plan.knobs.transientFailures) {
                    Record(request, 503);
                    return WriteSimple(*peer, 503, {{"Retry-After", "0"}})
                               ? Disposition::KeepAlive
                               : Disposition::Aborted;
                }
                return ServeAsset(peer, request, plan, Behavior::Normal);

            case Behavior::ConnectionResetBeforeResponse:
                Record(request, 0);
                Abort(peer);
                return Disposition::Aborted;

            case Behavior::RedirectLoop: {
                Record(request, 302);
                const bool sent =
                    WriteSimple(*peer, 302, {{"Location", request.path}});
                return sent ? Disposition::KeepAlive : Disposition::Aborted;
            }

            case Behavior::RedirectChain: {
                if (plan.hop < plan.knobs.redirectHops) {
                    const std::string next = plan.assetPath + kHopMarker +
                                             std::to_string(plan.hop + 1);
                    Record(request, 302);
                    const bool sent = WriteSimple(*peer, 302, {{"Location", next}});
                    return sent ? Disposition::KeepAlive : Disposition::Aborted;
                }
                return ServeAsset(peer, request, plan, Behavior::Normal);
            }

            case Behavior::SlowHeaders:
                if (!Sleep(plan.knobs.delayMs)) {
                    Record(request, 0);
                    return Disposition::Aborted;
                }
                return ServeAsset(peer, request, plan, Behavior::Normal);

            default:
                return ServeAsset(peer, request, plan, plan.knobs.behavior);
        }
    }

    /// Resets the connection and gives up the descriptor, so that the caller's
    /// own close is a no-op rather than a second close.
    static void Abort(NativeSocket* peer) {
        detail::CloseAbortive(*peer);
        *peer = detail::InvalidSocket();
    }

    // --- the asset responses --------------------------------------------------

    /// `effective` is the behavior to apply, which is not always the asset's
    /// own: a RedirectChain that has arrived, and a TransientServerError that
    /// has exhausted its failures, both serve normally from here.
    Disposition ServeAsset(NativeSocket* peer,
                           const Request& request,
                           const Plan& plan,
                           Behavior effective) {
        const Revision& revision = *plan.revision;
        const std::uint64_t size = revision.content.size();
        const std::string etag = FormatEtag(plan.knobs, revision);

        const bool advertisesRanges = effective != Behavior::NoAcceptRanges;

        // Advertising and honoring are separate, and IgnoresRange is the row
        // that separates them: it says `Accept-Ranges: bytes` and then does
        // not. A server that does not honor ranges also does not answer `416`,
        // because it never looked at the header -- so the whole range branch
        // below is gated on this rather than on the header being present.
        const bool honorsRanges = effective != Behavior::NoAcceptRanges &&
                                  effective != Behavior::IgnoresRange;

        const std::string rangeHeader = request.Header("Range");
        const std::string ifRange = request.Header("If-Range");

        // RFC 9110 §13.1.5: an If-Range that does not match the current
        // validator makes the Range inapplicable, and the whole representation
        // is returned. This is not a hostile case -- it is the mechanism the
        // revision binding in §6 of the design policy depends on, and every
        // behavior below inherits it.
        const bool ifRangeMatches = ifRange.empty() || ifRange == etag;

        ByteRange range;
        RangeParse parsed = detail::ParseRange(rangeHeader, size, &range);
        if (!ifRangeMatches && parsed == RangeParse::Ok) {
            parsed = RangeParse::Absent;
        }

        const bool refuses =
            honorsRanges &&
            ((effective == Behavior::RangeNotSatisfiable &&
              parsed != RangeParse::Absent) ||
             parsed == RangeParse::Unsatisfiable);

        if (refuses) {
            Record(request, 416);
            const bool sent = WriteSimple(
                *peer, 416,
                {{"Content-Range", "bytes */" + std::to_string(size)},
                 {"Accept-Ranges", "bytes"}});
            return sent ? Disposition::KeepAlive : Disposition::Aborted;
        }

        const bool rangeApplies = honorsRanges && parsed == RangeParse::Ok;

        std::vector<std::pair<std::string, std::string>> headers;
        if (advertisesRanges) headers.emplace_back("Accept-Ranges", "bytes");
        if (!etag.empty()) headers.emplace_back("ETag", etag);
        if (!plan.knobs.lastModified.empty()) {
            headers.emplace_back("Last-Modified", plan.knobs.lastModified);
        }
        headers.emplace_back("Content-Type", "application/octet-stream");

        int status = 200;
        std::uint64_t bodyFirst = 0;
        std::uint64_t bodyLength = size;

        if (rangeApplies) {
            status = 206;
            bodyFirst = range.first;
            bodyLength = range.Length();

            std::uint64_t claimedFirst = range.first;
            std::uint64_t claimedLast = range.last;

            if (effective == Behavior::ContentRangeTooShort) {
                // Serve, and honestly describe, less than was asked for. The
                // response is self-consistent; only the request disagrees.
                claimedLast = range.first + (range.Length() - 1) / 2;
                bodyLength = claimedLast - claimedFirst + 1;
            } else if (effective == Behavior::ContentRangeShifted) {
                // Same length, wrong place. Shifted toward zero when there is
                // room below and away from it otherwise, so the case works at
                // offset zero too.
                const std::uint64_t shift = range.first > 0 ? 1 : 0;
                if (shift > 0) {
                    claimedFirst = range.first - 1;
                    claimedLast = range.last - 1;
                    bodyFirst = claimedFirst;
                } else if (range.last + 1 < size) {
                    claimedFirst = range.first + 1;
                    claimedLast = range.last + 1;
                    bodyFirst = claimedFirst;
                }
            }

            headers.emplace_back("Content-Range",
                                 "bytes " + std::to_string(claimedFirst) + "-" +
                                     std::to_string(claimedLast) + "/" +
                                     std::to_string(size));
        }

        const std::uint64_t available =
            bodyFirst < size ? std::min<std::uint64_t>(bodyLength, size - bodyFirst)
                             : 0;

        if (effective != Behavior::UnknownContentLength) {
            headers.emplace_back("Content-Length", std::to_string(available));
        } else {
            // No Content-Length and no Transfer-Encoding: the body ends when
            // the connection does, and a client that accepts this cannot tell a
            // complete response from a truncated one.
            headers.emplace_back("Connection", "close");
        }

        Record(request, status);

        const std::string head = detail::BuildHead(status, headers);
        if (!detail::SendAll(*peer, head.data(), head.size())) {
            return Disposition::Aborted;
        }

        if (request.method == "HEAD") {
            // A HEAD that announced a close-delimited body must still close,
            // or the next request on this connection would contradict it.
            return effective == Behavior::UnknownContentLength
                       ? Disposition::Close
                       : Disposition::KeepAlive;
        }

        // Never form `data() + n` on an empty vector: `data()` may be null, and
        // this repository runs a UBSan lane precisely so that pointer and offset
        // arithmetic is checked rather than assumed.
        const unsigned char* body =
            available > 0 ? revision.content.data() + bodyFirst : nullptr;
        return WriteBody(peer, body, available, plan.knobs, effective);
    }

    Disposition WriteBody(NativeSocket* peer,
                          const unsigned char* body,
                          std::uint64_t promised,
                          const Knobs& knobs,
                          Behavior effective) {
        const bool truncates = effective == Behavior::TruncatedBody ||
                               effective == Behavior::ConnectionResetMidBody;

        std::uint64_t deliver = promised;
        if (truncates) {
            deliver = knobs.deliverBytes >= 0
                          ? std::min<std::uint64_t>(
                                static_cast<std::uint64_t>(knobs.deliverBytes),
                                promised)
                          : promised / 2;
        }

        if (effective == Behavior::SlowBody) {
            const std::uint64_t head = promised > 0 ? 1 : 0;
            if (head > 0 && !detail::SendAll(*peer, body, head)) {
                return Disposition::Aborted;
            }
            if (!Sleep(knobs.delayMs)) return Disposition::Aborted;
            if (promised > head &&
                !detail::SendAll(*peer, body + head, promised - head)) {
                return Disposition::Aborted;
            }
            return Disposition::KeepAlive;
        }

        if (deliver > 0 && !detail::SendAll(*peer, body, deliver)) {
            return Disposition::Aborted;
        }

        if (effective == Behavior::ConnectionResetMidBody) {
            Abort(peer);
            return Disposition::Aborted;
        }
        if (effective == Behavior::TruncatedBody ||
            effective == Behavior::UnknownContentLength) {
            return Disposition::Close;
        }
        return Disposition::KeepAlive;
    }

    bool WriteSimple(
        NativeSocket peer,
        int status,
        const std::vector<std::pair<std::string, std::string>>& extra) {
        std::vector<std::pair<std::string, std::string>> headers = extra;
        headers.emplace_back("Content-Length", "0");
        const std::string head = detail::BuildHead(status, headers);
        return detail::SendAll(peer, head.data(), head.size());
    }

    // --- state ----------------------------------------------------------------

    detail::Platform _platform;
    NativeSocket _listener = detail::InvalidSocket();
    unsigned short _port = 0;

    std::atomic<bool> _stopping{false};

    std::thread _acceptor;

    mutable std::mutex _mutex;
    std::map<std::string, AssetState> _assets;
    std::vector<RequestRecord> _log;
    std::vector<std::thread> _connections;

    std::mutex _wakeMutex;
    std::condition_variable _wake;
};

// --- the public shell --------------------------------------------------------

Server::Server(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

Server::~Server() = default;

std::unique_ptr<Server> Server::Start(std::string* errorOut) {
    auto impl = std::unique_ptr<Impl>(new Impl());
    if (!impl->Start(errorOut)) return nullptr;
    return std::unique_ptr<Server>(new Server(std::move(impl)));
}

unsigned short Server::Port() const noexcept { return _impl->Port(); }

std::string Server::BaseUrl() const {
    return "http://127.0.0.1:" + std::to_string(_impl->Port());
}

std::string Server::Url(const std::string& path) const {
    return BaseUrl() + path;
}

void Server::Serve(const AssetSpec& spec) { _impl->Serve(spec); }

bool Server::Republish(const std::string& path,
                       const std::vector<unsigned char>& content,
                       const std::string& etag) {
    return _impl->Republish(path, content, etag);
}

std::vector<RequestRecord> Server::Log() const { return _impl->Log(); }

std::size_t Server::RequestCount() const { return _impl->RequestCount(); }

void Server::ClearLog() { _impl->ClearLog(); }

void Server::Stop() { _impl->Stop(); }

}  // namespace usdassetfixture
