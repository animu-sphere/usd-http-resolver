// SPDX-License-Identifier: Apache-2.0

#include "Uri.h"

#include <cctype>
#include <string>
#include <vector>

namespace usdasset {
namespace http {
namespace {

char LowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string LowerAscii(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char c : text) lowered.push_back(LowerAscii(c));
    return lowered;
}

/// Splits off a fragment. It is dropped rather than carried: a fragment is a
/// client-side selector and is never sent to a server (RESOLVER.md §2.1).
std::string_view StripFragment(std::string_view text) {
    const std::size_t hash = text.find('#');
    return hash == std::string_view::npos ? text : text.substr(0, hash);
}

/// Parses the authority component: `[userinfo@]host[:port]`.
///
/// The last `@` separates userinfo from host, not the first: a userinfo
/// component may legally contain an encoded `@`, and splitting on the first one
/// hands part of a credential to the connection as a hostname.
bool ParseAuthority(std::string_view authority, Uri* out, std::string* errorOut) {
    std::string_view hostPort = authority;
    const std::size_t at = authority.rfind('@');
    if (at != std::string_view::npos) {
        out->userinfo = std::string(authority.substr(0, at));
        hostPort = authority.substr(at + 1);
    }

    if (hostPort.empty()) {
        *errorOut = "URI has no host";
        return false;
    }

    // An IPv6 literal is bracketed and contains colons of its own, so the port
    // separator is only the colon that follows the closing bracket.
    std::size_t colon = std::string_view::npos;
    if (hostPort.front() == '[') {
        const std::size_t close = hostPort.find(']');
        if (close == std::string_view::npos) {
            *errorOut = "URI has an unterminated IPv6 literal";
            return false;
        }
        if (close + 1 < hostPort.size()) {
            if (hostPort[close + 1] != ':') {
                *errorOut = "URI has trailing junk after an IPv6 literal";
                return false;
            }
            colon = close + 1;
        }
    } else {
        colon = hostPort.find(':');
    }

    if (colon == std::string_view::npos) {
        out->host = LowerAscii(hostPort);
        out->port = 0;
        return true;
    }

    out->host = LowerAscii(hostPort.substr(0, colon));
    const std::string_view portText = hostPort.substr(colon + 1);
    if (portText.empty()) {
        // `http://host:/path` is legal and means the default port.
        out->port = 0;
        return true;
    }

    int port = 0;
    for (const char c : portText) {
        if (c < '0' || c > '9') {
            *errorOut = "URI port is not a number";
            return false;
        }
        port = port * 10 + (c - '0');
        if (port > 65535) {
            *errorOut = "URI port is out of range";
            return false;
        }
    }
    if (port == 0) {
        *errorOut = "URI port is out of range";
        return false;
    }
    out->port = port;
    return true;
}

/// Fills path and query from everything after the authority, defaulting an
/// empty path to `/` -- which is what goes on the wire either way.
void SplitPathAndQuery(std::string_view rest, Uri* out) {
    const std::size_t question = rest.find('?');
    if (question == std::string_view::npos) {
        out->path = rest.empty() ? "/" : std::string(rest);
        out->query.clear();
        out->hasQuery = false;
        return;
    }
    const std::string_view path = rest.substr(0, question);
    out->path = path.empty() ? "/" : std::string(path);
    out->query = std::string(rest.substr(question + 1));
    out->hasQuery = true;
}

/// RFC 3986 §5.3, minus the fragment this type does not carry.
std::string Compose(const Uri& uri, bool includeUserinfo) {
    std::string text = uri.scheme;
    text += "://";
    if (includeUserinfo && !uri.userinfo.empty()) {
        text += uri.userinfo;
        text += '@';
    }
    text += uri.host;
    if (uri.port != 0 && uri.port != uri.DefaultPort()) {
        text += ':';
        text += std::to_string(uri.port);
    }
    text += uri.path;
    if (uri.hasQuery) {
        text += '?';
        text += uri.query;
    }
    return text;
}

}  // namespace

std::string Uri::Authority() const {
    std::string text = host;
    if (port != 0 && port != DefaultPort()) {
        text += ':';
        text += std::to_string(port);
    }
    return text;
}

std::string Uri::ToString() const { return Compose(*this, true); }

std::string Uri::ToIdentity() const { return Compose(*this, false); }

bool ParseUri(std::string_view text, Uri* out, std::string* errorOut) {
    std::string ignored;
    if (errorOut == nullptr) errorOut = &ignored;
    *out = Uri();

    text = StripFragment(text);

    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        *errorOut = "identifier is not an absolute URI";
        return false;
    }
    const std::string scheme = LowerAscii(text.substr(0, colon));
    if (scheme != "http" && scheme != "https") {
        // Named rather than generic. A caller that handed this backend an
        // `s3://` identifier has a routing bug, and "unsupported scheme" sends
        // it to the right place; "malformed URI" does not.
        *errorOut = "unsupported URI scheme '" + scheme + "'";
        return false;
    }
    out->scheme = scheme;

    std::string_view rest = text.substr(colon + 1);
    if (rest.size() < 2 || rest[0] != '/' || rest[1] != '/') {
        *errorOut = "URI has no authority";
        return false;
    }
    rest.remove_prefix(2);

    const std::size_t authorityEnd = rest.find_first_of("/?");
    const std::string_view authority =
        authorityEnd == std::string_view::npos ? rest : rest.substr(0, authorityEnd);
    if (!ParseAuthority(authority, out, errorOut)) return false;

    SplitPathAndQuery(authorityEnd == std::string_view::npos
                          ? std::string_view()
                          : rest.substr(authorityEnd),
                      out);
    out->path = RemoveDotSegments(out->path);
    return true;
}

std::string RemoveDotSegments(std::string_view path) {
    // The RFC's algorithm, written as a segment stack rather than as its
    // literal string rewriting: the two agree, and this form cannot leave a
    // half-consumed segment behind.
    //
    // An **empty segment is a segment**. `/a//b` has three of them and stays
    // `/a//b`; RFC 3986 §5.2.4 removes `.` and `..` and nothing else. Collapsing
    // `//` looks like tidying and is a rename: an object-storage key may
    // legitimately contain an empty path segment, so the collapsed form names a
    // different object, and a pre-signed URL whose signature covers the
    // canonical path stops verifying. That is the same reason the query string
    // is preserved verbatim two functions up.
    const bool absolute = !path.empty() && path.front() == '/';

    std::vector<std::string_view> segments;
    // Whether the final segment was `.` or `..`, which the RFC's algorithm
    // replaces with an empty segment -- `/a/b/..` is `/a/`, not `/a`.
    bool endedWithDotSegment = false;

    std::size_t i = absolute ? 1 : 0;
    for (;;) {
        const std::size_t end = path.find('/', i);
        const std::string_view segment =
            end == std::string_view::npos ? path.substr(i) : path.substr(i, end - i);

        if (segment == ".") {
            endedWithDotSegment = true;
        } else if (segment == "..") {
            // Pop one segment. A `..` that would escape the root is discarded
            // rather than honored: it cannot name anything on the server, and
            // an origin that emits one in a `Location` must not be able to walk
            // a client's path arithmetic backwards past `/`.
            if (!segments.empty()) segments.pop_back();
            endedWithDotSegment = true;
        } else {
            segments.push_back(segment);
            endedWithDotSegment = false;
        }

        if (end == std::string_view::npos) break;
        i = end + 1;
    }

    if (endedWithDotSegment) segments.emplace_back();

    std::string output;
    for (std::size_t k = 0; k < segments.size(); ++k) {
        if (k > 0 || absolute) output += '/';
        output.append(segments[k]);
    }
    if (output.empty()) return absolute ? "/" : std::string();
    return output;
}

bool ResolveReference(const Uri& base,
                      std::string_view reference,
                      Uri* out,
                      std::string* errorOut) {
    std::string ignored;
    if (errorOut == nullptr) errorOut = &ignored;

    reference = StripFragment(reference);
    if (reference.empty()) {
        *errorOut = "redirect Location is empty";
        return false;
    }

    // An absolute URI: it carries its own scheme, and RFC 3986 §5.2.2 uses it
    // whole. Anything with a scheme this backend does not serve fails here
    // rather than being silently rewritten into one that it does.
    const std::size_t colon = reference.find(':');
    const std::size_t firstDelimiter = reference.find_first_of("/?#");
    if (colon != std::string_view::npos &&
        (firstDelimiter == std::string_view::npos || colon < firstDelimiter)) {
        return ParseUri(reference, out, errorOut);
    }

    // A network-path reference inherits only the scheme.
    if (reference.size() >= 2 && reference[0] == '/' && reference[1] == '/') {
        std::string absolute = base.scheme;
        absolute += ':';
        absolute.append(reference);
        return ParseUri(absolute, out, errorOut);
    }

    *out = Uri();
    out->scheme = base.scheme;
    out->userinfo = base.userinfo;
    out->host = base.host;
    out->port = base.port;

    if (reference.front() == '?') {
        // A query-only reference keeps the base path and replaces the query.
        out->path = base.path;
        out->query = std::string(reference.substr(1));
        out->hasQuery = true;
        return true;
    }

    std::string_view rest = reference;
    const std::size_t question = rest.find('?');
    std::string_view referencePath = rest;
    if (question != std::string_view::npos) {
        referencePath = rest.substr(0, question);
        out->query = std::string(rest.substr(question + 1));
        out->hasQuery = true;
    }

    if (!referencePath.empty() && referencePath.front() == '/') {
        out->path = RemoveDotSegments(referencePath);
    } else {
        // Merge, per §5.3: everything up to and including the base's last `/`.
        std::string merged = base.path;
        const std::size_t slash = merged.rfind('/');
        merged.erase(slash == std::string::npos ? 0 : slash + 1);
        merged.append(referencePath);
        out->path = RemoveDotSegments(merged);
    }
    if (out->path.empty() || out->path.front() != '/') out->path.insert(0, "/");
    return true;
}

bool IsSchemeDowngrade(const Uri& base, const Uri& target) noexcept {
    return base.IsSecure() && !target.IsSecure();
}

}  // namespace http
}  // namespace usdasset
