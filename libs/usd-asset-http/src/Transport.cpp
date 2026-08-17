// SPDX-License-Identifier: Apache-2.0
//
// The parts of the seam that are not the client: the header table every
// transport fills in, and the spelling of a transport error.

#include "Transport.h"

namespace usdasset {
namespace http {
namespace {

bool EqualsIgnoringCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

}  // namespace

void HeaderTable::Add(std::string name, std::string value) {
    _entries.emplace_back(std::move(name), std::move(value));
}

const std::string* HeaderTable::Find(std::string_view name) const {
    // Last wins. A response that repeats a header is malformed or hostile, and
    // where a decision has to be made anyway, taking the value nearest the body
    // is the one an intermediary is least likely to have prepended.
    const std::string* found = nullptr;
    for (const auto& entry : _entries) {
        if (EqualsIgnoringCase(entry.first, name)) found = &entry.second;
    }
    return found;
}

std::string HeaderTable::Get(std::string_view name) const {
    const std::string* value = Find(name);
    return value == nullptr ? std::string() : *value;
}

const char* TransportErrorName(TransportError error) noexcept {
    switch (error) {
        case TransportError::None: return "None";
        case TransportError::ConnectFailed: return "ConnectFailed";
        case TransportError::ConnectTimeout: return "ConnectTimeout";
        case TransportError::ResponseTimeout: return "ResponseTimeout";
        case TransportError::TransferTimeout: return "TransferTimeout";
        case TransportError::ConnectionLost: return "ConnectionLost";
        case TransportError::IncompleteBody: return "IncompleteBody";
        case TransportError::Malformed: return "Malformed";
        case TransportError::Internal: return "Internal";
    }
    return "Unknown";
}

}  // namespace http
}  // namespace usdasset
