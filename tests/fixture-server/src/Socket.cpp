// SPDX-License-Identifier: Apache-2.0

#include "Socket.h"

#include <mutex>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace usdassetfixture {
namespace detail {
namespace {

#if defined(_WIN32)
int LastError() noexcept { return ::WSAGetLastError(); }

bool WouldBlock(int error) noexcept {
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
}

bool IsReset(int error) noexcept {
    return error == WSAECONNRESET || error == WSAECONNABORTED;
}
#else
int LastError() noexcept { return errno; }

bool WouldBlock(int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
}

bool IsReset(int error) noexcept { return error == ECONNRESET; }
#endif

std::string Describe(const char* what, int error) {
    return std::string(what) + " failed (" + std::to_string(error) + ")";
}

// The byte-order conversions, wrapped once each.
//
// Not decoration: on Darwin these are *macros* over a statement expression
// rather than functions, so a qualified `::htonl(x)` expands to something with
// no unqualified-id after the `::` and does not parse. A `using ::htonl;`
// declaration does not help either -- the preprocessor rewrites that too. Linux
// declares them as functions and accepts the qualifier happily, which is why
// writing `::htonl` is the natural mistake and why only the macos-arm64 lane
// finds it. Wrapping them here leaves exactly one place that can be wrong,
// instead of four call sites that each look fine on two platforms out of three.
std::uint32_t HostToNetwork32(std::uint32_t value) noexcept { return htonl(value); }
std::uint16_t HostToNetwork16(std::uint16_t value) noexcept { return htons(value); }
std::uint16_t NetworkToHost16(std::uint16_t value) noexcept { return ntohs(value); }

#if defined(_WIN32)
using SysSocket = SOCKET;
using SysLength = int;
#else
using SysSocket = int;
using SysLength = socklen_t;
#endif

/// The one place the descriptor changes type. Every call below goes through it,
/// so the platform difference is a single conversion rather than a cast at each
/// call site.
SysSocket Sys(NativeSocket socket) noexcept {
    return static_cast<SysSocket>(socket);
}

// Writing to a peer that hung up must produce an error, not kill the process.
// Three platforms, three spellings, and macOS is the one that has neither of
// the others: Linux takes MSG_NOSIGNAL per send, macOS takes SO_NOSIGPIPE per
// socket, and Windows raises no signal at all. A fixture server that died of
// SIGPIPE when a client disconnected early could not host the reset and
// truncation cases it exists to serve -- and every one of those cases ends with
// a client that has stopped reading.
#if defined(_WIN32)
constexpr int kSendFlags = 0;
#elif defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

/// Applies SO_NOSIGPIPE where that is how the platform spells it. A no-op
/// everywhere else.
void SuppressSigPipe(NativeSocket socket) noexcept {
#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
    const int on = 1;
    ::setsockopt(static_cast<SysSocket>(socket), SOL_SOCKET, SO_NOSIGPIPE, &on,
                 static_cast<SysLength>(sizeof(on)));
#else
    (void)socket;
#endif
}

/// One readiness wait, on the platform's own poll.
int PollReadable(NativeSocket socket, int timeoutMs) {
#if defined(_WIN32)
    WSAPOLLFD entry{};
    entry.fd = static_cast<SOCKET>(socket);
    entry.events = POLLRDNORM;
    return ::WSAPoll(&entry, 1, timeoutMs);
#else
    struct pollfd entry {};
    entry.fd = socket;
    entry.events = POLLIN;
    return ::poll(&entry, 1, timeoutMs);
#endif
}

/// The same wait, for room in the send buffer.
///
/// `WSAPoll`'s known defect is that it does not report a *failed connect* in
/// `POLLOUT`, and this is deliberately not that call: the socket here is already
/// connected and the question is only whether the peer has drained anything. A
/// spurious wakeup costs one extra `send` that returns `WSAEWOULDBLOCK`, which
/// the caller's loop already handles.
int PollWritable(NativeSocket socket, int timeoutMs) {
#if defined(_WIN32)
    WSAPOLLFD entry{};
    entry.fd = static_cast<SOCKET>(socket);
    entry.events = POLLWRNORM;
    return ::WSAPoll(&entry, 1, timeoutMs);
#else
    struct pollfd entry {};
    entry.fd = socket;
    entry.events = POLLOUT;
    return ::poll(&entry, 1, timeoutMs);
#endif
}

/// How long one wait for a writable peer lasts. The cadence the accept loop
/// polls at, and for the same reason: it bounds how long a caller that has been
/// told to stop keeps waiting on a client that stopped reading.
constexpr int kSendWaitMs = 25;

/// Puts a socket in non-blocking mode. Applied to accepted peers, so that a
/// write to a client that stopped reading is a wait this process can end.
/// `Receive` already polls before it reads, so nothing else changes shape.
void SetNonBlocking(NativeSocket socket) noexcept {
#if defined(_WIN32)
    u_long mode = 1;
    ::ioctlsocket(Sys(socket), FIONBIO, &mode);
#else
    const int flags = ::fcntl(Sys(socket), F_GETFL, 0);
    if (flags >= 0) ::fcntl(Sys(socket), F_SETFL, flags | O_NONBLOCK);
#endif
}

}  // namespace

NativeSocket InvalidSocket() noexcept {
#if defined(_WIN32)
    return static_cast<NativeSocket>(INVALID_SOCKET);
#else
    return -1;
#endif
}

bool IsValidSocket(NativeSocket socket) noexcept {
    return socket != InvalidSocket();
}

// --- Platform ---------------------------------------------------------------

namespace {

std::mutex& PlatformMutex() {
    static std::mutex mutex;
    return mutex;
}

int& PlatformRefCount() {
    static int count = 0;
    return count;
}

}  // namespace

Platform::Platform() {
    std::lock_guard<std::mutex> lock(PlatformMutex());
    if (PlatformRefCount()++ == 0) {
#if defined(_WIN32)
        WSADATA data{};
        ::WSAStartup(MAKEWORD(2, 2), &data);
#endif
    }
}

Platform::~Platform() {
    std::lock_guard<std::mutex> lock(PlatformMutex());
    if (--PlatformRefCount() == 0) {
#if defined(_WIN32)
        ::WSACleanup();
#endif
    }
}

// --- listen, accept, connect -------------------------------------------------

NativeSocket Listen(unsigned short* portOut, std::string* errorOut) {
    const NativeSocket listener =
        static_cast<NativeSocket>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!IsValidSocket(listener)) {
        if (errorOut) *errorOut = Describe("socket", LastError());
        return InvalidSocket();
    }

    // Deliberately no SO_REUSEADDR. The port is ephemeral, so there is nothing
    // to reuse, and on Windows SO_REUSEADDR permits two live binds to the same
    // address -- which would let one test's server silently steal another's
    // connections.

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = HostToNetwork32(INADDR_LOOPBACK);

    if (::bind(Sys(listener), reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0) {
        if (errorOut) *errorOut = Describe("bind", LastError());
        Close(listener);
        return InvalidSocket();
    }

    if (::listen(Sys(listener), 16) != 0) {
        if (errorOut) *errorOut = Describe("listen", LastError());
        Close(listener);
        return InvalidSocket();
    }

    sockaddr_in bound{};
    SysLength boundLength = static_cast<SysLength>(sizeof(bound));
    if (::getsockname(Sys(listener), reinterpret_cast<sockaddr*>(&bound),
                      &boundLength) != 0) {
        if (errorOut) *errorOut = Describe("getsockname", LastError());
        Close(listener);
        return InvalidSocket();
    }

    if (portOut) *portOut = NetworkToHost16(bound.sin_port);
    return listener;
}

NativeSocket AcceptWithTimeout(NativeSocket listener, int timeoutMs, bool* timedOut) {
    if (timedOut) *timedOut = false;

    const int ready = PollReadable(listener, timeoutMs);
    if (ready == 0) {
        if (timedOut) *timedOut = true;
        return InvalidSocket();
    }
    if (ready < 0) {
        if (WouldBlock(LastError())) {
            if (timedOut) *timedOut = true;
        }
        return InvalidSocket();
    }

    const NativeSocket peer =
        static_cast<NativeSocket>(::accept(Sys(listener), nullptr, nullptr));
    if (IsValidSocket(peer)) {
        SuppressSigPipe(peer);
        SetNonBlocking(peer);
    }
    return peer;
}

NativeSocket Connect(unsigned short port, std::string* errorOut) {
    const NativeSocket peer =
        static_cast<NativeSocket>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!IsValidSocket(peer)) {
        if (errorOut) *errorOut = Describe("socket", LastError());
        return InvalidSocket();
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = HostToNetwork16(port);
    address.sin_addr.s_addr = HostToNetwork32(INADDR_LOOPBACK);

    if (::connect(Sys(peer), reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) != 0) {
        if (errorOut) *errorOut = Describe("connect", LastError());
        Close(peer);
        return InvalidSocket();
    }

    SuppressSigPipe(peer);
    return peer;
}

// --- transfer ----------------------------------------------------------------

ReceiveState Receive(NativeSocket socket,
                     void* dst,
                     std::size_t size,
                     int timeoutMs,
                     std::size_t* readOut) {
    if (readOut) *readOut = 0;

    const int ready = PollReadable(socket, timeoutMs);
    if (ready == 0) return ReceiveState::TimedOut;
    if (ready < 0) {
        return WouldBlock(LastError()) ? ReceiveState::TimedOut : ReceiveState::Error;
    }

#if defined(_WIN32)
    const int received = ::recv(Sys(socket), static_cast<char*>(dst),
                                static_cast<int>(size), 0);
#else
    const long received = ::recv(Sys(socket), dst, size, 0);
#endif

    if (received > 0) {
        if (readOut) *readOut = static_cast<std::size_t>(received);
        return ReceiveState::Data;
    }
    if (received == 0) return ReceiveState::Closed;

    const int error = LastError();
    if (IsReset(error)) return ReceiveState::Reset;
    if (WouldBlock(error)) return ReceiveState::TimedOut;
    return ReceiveState::Error;
}

bool SendAll(NativeSocket socket,
             const void* src,
             std::size_t size,
             const std::atomic<bool>* abandon) {
    const char* cursor = static_cast<const char*>(src);
    std::size_t remaining = size;

    while (remaining > 0) {
        if (abandon && abandon->load()) return false;

#if defined(_WIN32)
        const int sent =
            ::send(Sys(socket), cursor, static_cast<int>(remaining), kSendFlags);
#else
        const long sent = ::send(Sys(socket), cursor, remaining, kSendFlags);
#endif
        if (sent > 0) {
            cursor += sent;
            remaining -= static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && WouldBlock(LastError())) {
            // The peer's window is full, or nothing was ready. Waiting in
            // bounded steps rather than in one unbounded call is what lets the
            // check at the top of this loop ever run again.
            const int ready = PollWritable(socket, kSendWaitMs);
            if (ready < 0 && !WouldBlock(LastError())) return false;
            continue;
        }
        return false;
    }
    return true;
}

void Close(NativeSocket socket) noexcept {
    if (!IsValidSocket(socket)) return;
#if defined(_WIN32)
    ::closesocket(Sys(socket));
#else
    ::close(Sys(socket));
#endif
}

void CloseAbortive(NativeSocket socket) noexcept {
    if (!IsValidSocket(socket)) return;

    struct linger option = {};
    option.l_onoff = 1;
    option.l_linger = 0;
    ::setsockopt(Sys(socket), SOL_SOCKET, SO_LINGER,
                 reinterpret_cast<const char*>(&option),
                 static_cast<SysLength>(sizeof(option)));
    Close(socket);
}

}  // namespace detail
}  // namespace usdassetfixture
