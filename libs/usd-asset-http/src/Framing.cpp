// SPDX-License-Identifier: Apache-2.0

#include "Framing.h"

#include <limits>

namespace usdasset {
namespace http {
namespace {

constexpr char kEtag[] = "ETag";
constexpr char kLastModified[] = "Last-Modified";

bool IsDigit(char c) noexcept { return c >= '0' && c <= '9'; }

bool IsSpace(char c) noexcept { return c == ' ' || c == '\t'; }

std::string_view Trim(std::string_view text) {
    while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
    return text;
}

/// Digits to a `uint64_t`, refusing overflow rather than wrapping.
///
/// A server-declared length is untrusted input (§10 of the design policy), and
/// a length that wrapped is the one that produces a small, plausible number
/// from an enormous, hostile one.
bool ParseUnsigned(std::string_view text, std::uint64_t* out) {
    if (text.empty()) return false;
    std::uint64_t value = 0;
    for (const char c : text) {
        if (!IsDigit(c)) return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

/// Case-insensitive token search over a comma-separated list.
bool ListContainsToken(std::string_view list, std::string_view token) {
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t comma = list.find(',', start);
        const std::string_view item = Trim(
            comma == std::string_view::npos ? list.substr(start)
                                            : list.substr(start, comma - start));
        if (item.size() == token.size()) {
            bool same = true;
            for (std::size_t i = 0; i < item.size(); ++i) {
                char a = item[i];
                char b = token[i];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
                if (a != b) {
                    same = false;
                    break;
                }
            }
            if (same) return true;
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return false;
}

}  // namespace

bool ParseContentRange(std::string_view value, ContentRange* out) {
    *out = ContentRange();

    value = Trim(value);
    constexpr std::string_view kUnit = "bytes";
    if (value.size() <= kUnit.size()) return false;
    if (value.substr(0, kUnit.size()) != kUnit) return false;
    if (!IsSpace(value[kUnit.size()])) return false;
    value = Trim(value.substr(kUnit.size()));

    const std::size_t slash = value.find('/');
    if (slash == std::string_view::npos) return false;
    const std::string_view spec = Trim(value.substr(0, slash));
    const std::string_view complete = Trim(value.substr(slash + 1));

    if (complete == "*") {
        out->hasCompleteLength = false;
    } else if (!ParseUnsigned(complete, &out->completeLength)) {
        return false;
    } else {
        out->hasCompleteLength = true;
    }

    if (spec == "*") {
        // `bytes */<length>`: the unsatisfied form. A complete length is
        // mandatory here, and a `416` without one describes nothing.
        if (!out->hasCompleteLength) return false;
        out->unsatisfied = true;
        return true;
    }

    const std::size_t dash = spec.find('-');
    if (dash == std::string_view::npos) return false;
    if (!ParseUnsigned(spec.substr(0, dash), &out->first)) return false;
    if (!ParseUnsigned(spec.substr(dash + 1), &out->last)) return false;

    // Well-formed and impossible. Rejected here so that no caller has to
    // remember that `Length()` underflows on it.
    if (out->last < out->first) return false;
    // A range that claims to extend past the representation it is a range of is
    // internally inconsistent, whatever the request asked for.
    if (out->hasCompleteLength && out->last >= out->completeLength) return false;
    return true;
}

bool ParseContentLength(std::string_view value, std::uint64_t* out) {
    return ParseUnsigned(Trim(value), out);
}

bool AdvertisesByteRanges(const HeaderTable& headers) {
    const std::string* value = headers.Find("Accept-Ranges");
    if (value == nullptr) return false;
    return ListContainsToken(*value, "bytes");
}

CapturedValidator CaptureValidator(const HeaderTable& headers) {
    CapturedValidator captured;

    if (const std::string* etag = headers.Find(kEtag)) {
        const std::string_view value = Trim(*etag);
        if (!value.empty()) {
            captured.validator.value = std::string(value);
            captured.validator.kind = ValidatorKind::EntityTag;

            // `W/` is the weak prefix, and it is case-sensitive per RFC 9110.
            const bool weak = value.size() >= 2 && value[0] == 'W' && value[1] == '/';
            captured.validator.strength =
                weak ? ValidatorStrength::Weak : ValidatorStrength::Strong;
            if (!weak) {
                // Verbatim, quoting included. An `If-Range` is compared by the
                // origin as an opaque byte string, so re-quoting or unquoting
                // it here would produce a header that never matches.
                captured.ifRangeValue = captured.validator.value;
            }
            return captured;
        }
    }

    if (const std::string* modified = headers.Find(kLastModified)) {
        const std::string_view value = Trim(*modified);
        if (!value.empty()) {
            captured.validator.value = std::string(value);
            captured.validator.kind = ValidatorKind::HttpDate;
            // Weak, and the reason is arithmetic rather than policy: an HTTP
            // date has one-second granularity, so two revisions published
            // inside one second are indistinguishable. ASSET_READER.md §7.2
            // maps that to Unstable, and nothing keyed on it may outlive the
            // reader.
            captured.validator.strength = ValidatorStrength::Weak;
            // Still legal in `If-Range`, which is the one place the weak/strong
            // split does not follow `ValidatorStrength`: RFC 9110 §13.1.5
            // admits `Last-Modified` there under its own rule.
            captured.ifRangeValue = captured.validator.value;
            return captured;
        }
    }

    return captured;
}

bool ContradictsCapturedValidator(const HeaderTable& headers,
                                  const Validator& captured) {
    if (!captured.IsUsable()) return false;

    if (captured.kind == ValidatorKind::EntityTag) {
        const std::string* etag = headers.Find(kEtag);
        if (etag == nullptr) return false;
        return Trim(*etag) != std::string_view(captured.value);
    }
    if (captured.kind == ValidatorKind::HttpDate) {
        const std::string* modified = headers.Find(kLastModified);
        if (modified == nullptr) return false;
        return Trim(*modified) != std::string_view(captured.value);
    }
    return false;
}

std::string FormatByteRange(std::uint64_t offset, std::uint64_t length) {
    return "bytes=" + std::to_string(offset) + "-" +
           std::to_string(offset + length - 1);
}

}  // namespace http
}  // namespace usdasset
