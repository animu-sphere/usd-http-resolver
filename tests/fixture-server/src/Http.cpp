// SPDX-License-Identifier: Apache-2.0

#include "Http.h"

#include <cstddef>

namespace usdassetfixture {
namespace detail {
namespace {

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

/// Parses a run of ASCII digits. Rejects anything else, including a sign and a
/// leading '+': RFC 9110's range grammar is 1*DIGIT, and `strtoull` would
/// accept forms the grammar does not.
bool ParseDigits(const std::string& text, std::uint64_t* out) {
    if (text.empty()) return false;
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        // Saturate rather than wrap. A `Range` naming 2^70 is out of the
        // asset either way, and this file must not be the place an overflow
        // is introduced into a project whose UBSan lane exists for exactly
        // that arithmetic.
        if (value > (UINT64_MAX - digit) / 10) {
            *out = UINT64_MAX;
            return true;
        }
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

}  // namespace

std::string Request::Header(const std::string& name) const {
    for (const auto& field : headers) {
        if (EqualsIgnoringCase(field.first, name)) return field.second;
    }
    return std::string();
}

bool Request::WantsKeepAlive() const {
    const std::string connection = Header("Connection");
    if (EqualsIgnoringCase(Trim(connection), "close")) return false;
    if (version == "HTTP/1.0") {
        return EqualsIgnoringCase(Trim(connection), "keep-alive");
    }
    return true;
}

bool ParseRequest(const std::string& raw, Request* out) {
    if (!out) return false;
    *out = Request();

    std::size_t lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos) return false;

    const std::string requestLine = raw.substr(0, lineEnd);
    const std::size_t firstSpace = requestLine.find(' ');
    if (firstSpace == std::string::npos) return false;
    const std::size_t secondSpace = requestLine.find(' ', firstSpace + 1);
    if (secondSpace == std::string::npos) return false;

    out->method = requestLine.substr(0, firstSpace);
    out->target = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    out->version = requestLine.substr(secondSpace + 1);

    const std::size_t queryStart = out->target.find('?');
    if (queryStart == std::string::npos) {
        out->path = out->target;
    } else {
        out->path = out->target.substr(0, queryStart);
        out->query = out->target.substr(queryStart + 1);
    }

    std::size_t cursor = lineEnd + 2;
    while (cursor < raw.size()) {
        lineEnd = raw.find("\r\n", cursor);
        if (lineEnd == std::string::npos) return false;
        if (lineEnd == cursor) break;  // the blank line

        const std::string field = raw.substr(cursor, lineEnd - cursor);
        const std::size_t colon = field.find(':');
        if (colon == std::string::npos) return false;
        out->headers.emplace_back(Trim(field.substr(0, colon)),
                                  Trim(field.substr(colon + 1)));
        cursor = lineEnd + 2;
    }

    return true;
}

RangeParse ParseRange(const std::string& value, std::uint64_t size, ByteRange* out) {
    if (value.empty()) return RangeParse::Absent;

    const std::string spec = Trim(value);
    const std::string unit = "bytes=";
    if (spec.size() <= unit.size() ||
        !EqualsIgnoringCase(spec.substr(0, unit.size()), unit)) {
        return RangeParse::Invalid;
    }

    const std::string set = Trim(spec.substr(unit.size()));
    if (set.find(',') != std::string::npos) return RangeParse::Invalid;

    const std::size_t dash = set.find('-');
    if (dash == std::string::npos) return RangeParse::Invalid;

    const std::string firstText = Trim(set.substr(0, dash));
    const std::string lastText = Trim(set.substr(dash + 1));

    if (firstText.empty()) {
        // Suffix form, `bytes=-N`: the last N bytes.
        std::uint64_t suffix = 0;
        if (!ParseDigits(lastText, &suffix)) return RangeParse::Invalid;
        if (size == 0 || suffix == 0) return RangeParse::Unsatisfiable;
        const std::uint64_t length = suffix < size ? suffix : size;
        out->first = size - length;
        out->last = size - 1;
        return RangeParse::Ok;
    }

    std::uint64_t first = 0;
    if (!ParseDigits(firstText, &first)) return RangeParse::Invalid;

    std::uint64_t last = 0;
    if (lastText.empty()) {
        // Open form, `bytes=N-`: to the end.
        if (size == 0 || first >= size) return RangeParse::Unsatisfiable;
        last = size - 1;
    } else {
        if (!ParseDigits(lastText, &last)) return RangeParse::Invalid;
        if (last < first) return RangeParse::Invalid;
        if (size == 0 || first >= size) return RangeParse::Unsatisfiable;
        if (last >= size) last = size - 1;
    }

    out->first = first;
    out->last = last;
    return RangeParse::Ok;
}

const char* ReasonPhrase(int status) noexcept {
    switch (status) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 302: return "Found";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 416: return "Range Not Satisfiable";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

std::string BuildHead(
    int status, const std::vector<std::pair<std::string, std::string>>& headers) {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " +
                       ReasonPhrase(status) + "\r\n";
    for (const auto& field : headers) {
        head += field.first;
        head += ": ";
        head += field.second;
        head += "\r\n";
    }
    head += "\r\n";
    return head;
}

}  // namespace detail
}  // namespace usdassetfixture
