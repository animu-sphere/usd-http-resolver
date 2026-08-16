// SPDX-License-Identifier: Apache-2.0
//
// The socket layer, kept to the seven operations the fixture server needs.
// Internal to the fixture server; the corpus self-test reaches it too, because
// the thing under test there is HTTP framing and not `send`.
//
// Two operations are here for reasons worth stating. `AcceptWithTimeout` exists
// so that shutdown never closes a descriptor another thread is blocked in --
// a race whose symptom is an occasional hang in CI and a green run locally.
// `CloseAbortive` exists because `RST` and `FIN` are different events: a client
// reports one as a connection reset and the other as a short body, and the
// corpus needs the difference to be real rather than simulated.

#ifndef USDASSETFIXTURE_SOCKET_H
#define USDASSETFIXTURE_SOCKET_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace usdassetfixture {
namespace detail {

#if defined(_WIN32)
using NativeSocket = std::uintptr_t;  // SOCKET
#else
using NativeSocket = int;             // file descriptor
#endif

NativeSocket InvalidSocket() noexcept;
bool IsValidSocket(NativeSocket socket) noexcept;

/// Winsock needs process-wide initialization, and a test binary may hold a
/// server and a client at once. Refcounted, so neither has to know about the
/// other's lifetime.
class Platform {
public:
    Platform();
    ~Platform();

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
};

/// Binds `127.0.0.1:0` and listens. `*portOut` receives the assigned port.
///
/// Loopback rather than `INADDR_ANY`: the corpus runs in CI without network
/// access, and a fixture server that is reachable from off the machine is a
/// fixture server that has to think about who is calling.
NativeSocket Listen(unsigned short* portOut, std::string* errorOut);

/// Waits up to `timeoutMs` for a connection. Returns `InvalidSocket()` with
/// `*timedOut` set when nothing arrived, which is how the accept loop stays
/// responsive to `Stop()` without another thread closing the listener.
NativeSocket AcceptWithTimeout(NativeSocket listener, int timeoutMs, bool* timedOut);

NativeSocket Connect(unsigned short port, std::string* errorOut);

/// Result of one receive. `bytes > 0` delivered; `Closed` is an orderly `FIN`;
/// `Reset` is an `RST`, which the corpus distinguishes; `TimedOut` is neither.
enum class ReceiveState { Data, Closed, Reset, TimedOut, Error };

ReceiveState Receive(NativeSocket socket,
                     void* dst,
                     std::size_t size,
                     int timeoutMs,
                     std::size_t* readOut);

/// Writes all of `size`, or returns false because the peer went away. A
/// partial write is never reported as success: a fixture that half-sent a body
/// it meant to send whole is a fixture defect, not a hostile case.
bool SendAll(NativeSocket socket, const void* src, std::size_t size);

void Close(NativeSocket socket) noexcept;

/// Closes with `SO_LINGER{1, 0}`, which sends `RST` instead of `FIN`.
void CloseAbortive(NativeSocket socket) noexcept;

}  // namespace detail
}  // namespace usdassetfixture

#endif  // USDASSETFIXTURE_SOCKET_H
