// SPDX-License-Identifier: Apache-2.0

#include "RawClient.h"

#include <chrono>
#include <cstddef>

namespace usdassetfixturetest {
namespace {

using usdassetfixture::detail::NativeSocket;
using usdassetfixture::detail::ReceiveState;

char LowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoringCase(const std::string& lhs, const std::string& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (LowerAscii(lhs[i]) != LowerAscii(rhs[i])) return false;
    }
    return true;
}

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t')) ++begin;
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t')) --end;
    return value.substr(begin, end - begin);
}

using Clock = std::chrono::steady_clock;

long ElapsedMs(Clock::time_point from) {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - from)
            .count());
}

int RemainingMs(Clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now());
    if (remaining.count() <= 0) return 0;
    return static_cast<int>(remaining.count());
}

/// Parses the status line and the field block. Written here rather than reused
/// from the server: the point of this file is that the two disagree when one is
/// wrong.
bool ParseHead(const std::string& head, RawResponse* out) {
    const std::size_t lineEnd = head.find("\r\n");
    if (lineEnd == std::string::npos) return false;

    const std::string statusLine = head.substr(0, lineEnd);
    const std::size_t firstSpace = statusLine.find(' ');
    if (firstSpace == std::string::npos) return false;
    const std::size_t secondSpace = statusLine.find(' ', firstSpace + 1);

    const std::string code =
        secondSpace == std::string::npos
            ? statusLine.substr(firstSpace + 1)
            : statusLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    if (code.size() != 3) return false;
    for (const char c : code) {
        if (c < '0' || c > '9') return false;
    }
    out->status = (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');
    if (secondSpace != std::string::npos) {
        out->reason = statusLine.substr(secondSpace + 1);
    }

    std::size_t cursor = lineEnd + 2;
    while (cursor < head.size()) {
        const std::size_t fieldEnd = head.find("\r\n", cursor);
        if (fieldEnd == std::string::npos) return false;
        if (fieldEnd == cursor) break;

        const std::string field = head.substr(cursor, fieldEnd - cursor);
        const std::size_t colon = field.find(':');
        if (colon == std::string::npos) return false;
        out->headers.emplace_back(Trim(field.substr(0, colon)),
                                  Trim(field.substr(colon + 1)));
        cursor = fieldEnd + 2;
    }
    return true;
}

bool ParseContentLength(const std::string& text, std::size_t* out) {
    if (text.empty()) return false;
    std::size_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<std::size_t>(c - '0');
    }
    *out = value;
    return true;
}

}  // namespace

const char* ResponseEndName(ResponseEnd end) noexcept {
    switch (end) {
        case ResponseEnd::Complete: return "Complete";
        case ResponseEnd::Closed: return "Closed";
        case ResponseEnd::Reset: return "Reset";
        case ResponseEnd::TimedOut: return "TimedOut";
        case ResponseEnd::Error: return "Error";
        case ResponseEnd::NoResponse: return "NoResponse";
    }
    return "Unknown";
}

std::string RawResponse::Header(const std::string& name) const {
    for (const auto& field : headers) {
        if (EqualsIgnoringCase(field.first, name)) return field.second;
    }
    return std::string();
}

bool RawResponse::HasHeader(const std::string& name) const {
    for (const auto& field : headers) {
        if (EqualsIgnoringCase(field.first, name)) return true;
    }
    return false;
}

// --- RawClient ---------------------------------------------------------------

RawClient::RawClient(unsigned short port) {
    _socket = usdassetfixture::detail::Connect(port, &_error);
}

RawClient::~RawClient() { Close(); }

bool RawClient::Connected() const noexcept {
    return usdassetfixture::detail::IsValidSocket(_socket);
}

void RawClient::Close() {
    if (Connected()) {
        usdassetfixture::detail::Close(_socket);
        _socket = usdassetfixture::detail::InvalidSocket();
    }
}

bool RawClient::Send(const std::string& raw) {
    if (!Connected()) return false;
    return usdassetfixture::detail::SendAll(_socket, raw.data(), raw.size());
}

RawResponse RawClient::ReadResponse(int timeoutMs, bool expectBody) {
    RawResponse response;
    if (!Connected()) {
        response.end = ResponseEnd::Error;
        return response;
    }

    const Clock::time_point started = Clock::now();
    const Clock::time_point deadline =
        started + std::chrono::milliseconds(timeoutMs);

    // --- the head ---
    std::size_t headEnd = std::string::npos;
    for (;;) {
        headEnd = _buffered.find("\r\n\r\n");
        if (headEnd != std::string::npos) break;

        const int remaining = RemainingMs(deadline);
        if (remaining == 0) {
            response.end = ResponseEnd::TimedOut;
            response.headElapsedMs = ElapsedMs(started);
            return response;
        }

        char chunk[4096];
        std::size_t received = 0;
        const ReceiveState state = usdassetfixture::detail::Receive(
            _socket, chunk, sizeof(chunk), remaining, &received);
        if (state == ReceiveState::Data) {
            _buffered.append(chunk, received);
            continue;
        }
        response.headElapsedMs = ElapsedMs(started);
        switch (state) {
            case ReceiveState::Closed: response.end = ResponseEnd::NoResponse; break;
            case ReceiveState::Reset: response.end = ResponseEnd::Reset; break;
            case ReceiveState::TimedOut: response.end = ResponseEnd::TimedOut; break;
            default: response.end = ResponseEnd::Error; break;
        }
        return response;
    }

    const std::string head = _buffered.substr(0, headEnd + 4);
    _buffered.erase(0, headEnd + 4);
    response.headElapsedMs = ElapsedMs(started);

    if (!ParseHead(head, &response)) {
        response.end = ResponseEnd::Error;
        return response;
    }
    response.gotHead = true;

    if (!expectBody) {
        response.end = ResponseEnd::Complete;
        response.bodyElapsedMs = response.headElapsedMs;
        return response;
    }

    // --- the body ---
    //
    // Two framings, and the difference is the whole point of the
    // UnknownContentLength row: with a Content-Length the body is complete or
    // it is not, and without one there is no such thing as incomplete.
    std::size_t declared = 0;
    const bool hasLength =
        ParseContentLength(response.Header("Content-Length"), &declared);

    for (;;) {
        if (hasLength) {
            const std::size_t wanted = declared - response.body.size();
            if (wanted == 0) {
                response.end = ResponseEnd::Complete;
                break;
            }
            if (_buffered.size() >= wanted) {
                response.body.insert(response.body.end(), _buffered.begin(),
                                     _buffered.begin() +
                                         static_cast<std::ptrdiff_t>(wanted));
                _buffered.erase(0, wanted);
                response.end = ResponseEnd::Complete;
                break;
            }
        }

        if (!_buffered.empty()) {
            response.body.insert(response.body.end(), _buffered.begin(),
                                 _buffered.end());
            _buffered.clear();
        }

        const int remaining = RemainingMs(deadline);
        if (remaining == 0) {
            response.end = ResponseEnd::TimedOut;
            break;
        }

        char chunk[4096];
        std::size_t received = 0;
        const ReceiveState state = usdassetfixture::detail::Receive(
            _socket, chunk, sizeof(chunk), remaining, &received);
        if (state == ReceiveState::Data) {
            _buffered.append(chunk, received);
            continue;
        }
        switch (state) {
            case ReceiveState::Closed: response.end = ResponseEnd::Closed; break;
            case ReceiveState::Reset: response.end = ResponseEnd::Reset; break;
            case ReceiveState::TimedOut: response.end = ResponseEnd::TimedOut; break;
            default: response.end = ResponseEnd::Error; break;
        }
        break;
    }

    response.bodyElapsedMs = ElapsedMs(started);
    return response;
}

// --- request construction ----------------------------------------------------

namespace {

std::string BuildRequest(
    const char* method,
    const std::string& target,
    const std::vector<std::pair<std::string, std::string>>& headers) {
    std::string raw = std::string(method) + " " + target + " HTTP/1.1\r\n";
    raw += "Host: 127.0.0.1\r\n";
    for (const auto& field : headers) {
        raw += field.first;
        raw += ": ";
        raw += field.second;
        raw += "\r\n";
    }
    raw += "\r\n";
    return raw;
}

}  // namespace

std::string GetRequest(
    const std::string& target,
    const std::vector<std::pair<std::string, std::string>>& headers) {
    return BuildRequest("GET", target, headers);
}

std::string HeadRequest(
    const std::string& target,
    const std::vector<std::pair<std::string, std::string>>& headers) {
    return BuildRequest("HEAD", target, headers);
}

RawResponse FetchOnce(unsigned short port,
                      const std::string& raw,
                      int timeoutMs,
                      bool expectBody) {
    RawClient client(port);
    if (!client.Connected() || !client.Send(raw)) {
        RawResponse response;
        response.end = ResponseEnd::Error;
        return response;
    }
    return client.ReadResponse(timeoutMs, expectBody);
}

}  // namespace usdassetfixturetest
