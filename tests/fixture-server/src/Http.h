// SPDX-License-Identifier: Apache-2.0
//
// Request parsing and range arithmetic for the fixture server.
//
// Deliberately strict about what it accepts and deliberately literal about what
// it emits. A fixture server that is lenient where a real origin is strict lets
// a backend ship a request no CDN would answer; a fixture server that
// normalizes what it emits cannot express the malformed responses it exists to
// produce. So `ParseRange` implements RFC 9110 §14.1.1 as written -- including
// the rule that an unparseable `Range` is *ignored*, which is a case a backend
// author does not expect and therefore does not handle.
//
// Internal to the fixture server. The corpus self-test does not use this: it
// writes request bytes by hand and parses responses with its own code, so that
// the framing assertions are two independent expressions of one rule rather
// than one expression checked against itself -- the same separation the
// boundary suite's oracle keeps from `usdAssetLocal`.

#ifndef USDASSETFIXTURE_HTTP_H
#define USDASSETFIXTURE_HTTP_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace usdassetfixture {
namespace detail {

struct Request {
    std::string method;
    std::string target;  ///< Path and query, exactly as sent.
    std::string path;    ///< Target with the query removed.
    std::string query;   ///< Query without the '?', empty when absent.
    std::string version;
    std::vector<std::pair<std::string, std::string>> headers;

    /// Case-insensitive lookup, empty when absent. Field names are
    /// case-insensitive per RFC 9110 §5.1, and a backend that sends `range:`
    /// must be answered, not quietly ignored into a passing test.
    std::string Header(const std::string& name) const;

    bool WantsKeepAlive() const;
};

/// Parses a complete header block: the request line, the fields, and the blank
/// line. `raw` ends at the blank line; request bodies are out of scope, because
/// nothing in §4.1 of the design policy sends one.
bool ParseRequest(const std::string& raw, Request* out);

struct ByteRange {
    std::uint64_t first = 0;
    std::uint64_t last = 0;  ///< Inclusive.

    std::uint64_t Length() const noexcept { return last - first + 1; }
};

enum class RangeParse {
    Absent,         ///< No `Range` header.
    Invalid,        ///< Syntactically bad, or a form out of scope. Ignore it.
    Unsatisfiable,  ///< Well-formed and outside the asset. `416`.
    Ok,
};

/// Parses a single-range `Range` header against an asset of `size` bytes.
///
/// Multi-range is reported `Invalid` rather than implemented: §4.1 of the
/// design policy puts multipart ranges out of v0.x scope, and a fixture that
/// answers a request the backend must never send would hide the day it sent
/// one.
RangeParse ParseRange(const std::string& value, std::uint64_t size, ByteRange* out);

const char* ReasonPhrase(int status) noexcept;

/// Serializes a status line and header block, blank line included. Fields are
/// emitted in the order given, and nothing is added: a header the caller did
/// not ask for is a header the corpus cannot prove absent.
std::string BuildHead(int status,
                      const std::vector<std::pair<std::string, std::string>>& headers);

}  // namespace detail
}  // namespace usdassetfixture

#endif  // USDASSETFIXTURE_HTTP_H
