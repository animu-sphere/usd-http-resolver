// SPDX-License-Identifier: Apache-2.0

#include "Identifier.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace usdhttpresolver {
namespace {

/// A URI reference, split into the parts §5.2 of RFC 3986 operates on.
///
/// The fragment is not a member. It is removed at split and never reaches a
/// server: it addresses a location *inside* a representation, which is the
/// consumer's business and not the origin's.
struct Parts {
    std::string scheme;  ///< Lowercased. Empty for a relative reference.
    bool hasAuthority = false;
    std::string authority;  ///< As written, userinfo included. Trimmed later.
    std::string path;
    std::string query;
    bool hasQuery = false;  ///< Distinguishes `?` from no query at all.
};

char LowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IsAlpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsDigit(char c) noexcept { return c >= '0' && c <= '9'; }

bool IsHex(char c) noexcept {
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int HexValue(char c) noexcept {
    if (IsDigit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

/// RFC 3986 §2.3. These are the characters that must be *decoded* when they
/// appear percent-encoded: `%7E` and `~` name the same asset, and leaving both
/// spellings alive gives one asset two identifiers and two opens.
bool IsUnreserved(unsigned char c) noexcept {
    return IsAlpha(static_cast<char>(c)) || IsDigit(static_cast<char>(c)) ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

/// What may appear literally in a path segment: unreserved, sub-delims, and
/// the two path-legal delimiters. Everything else -- a space, a control byte,
/// a UTF-8 continuation byte, a stray `%` -- is percent-encoded rather than
/// passed through.
///
/// Encoding rather than passing through matters because a layer authored by a
/// human contains `../textures/tree bark.png`, and that is a valid *reference*
/// and not a valid URI. Handing it to a transport verbatim produces a request
/// line with a space in it, which a strict origin answers with `400`. This is
/// the one place normalization adds characters rather than removing them, and
/// it is idempotent: an already-encoded `%20` is recognized as an escape and
/// left as `%20`, never doubly encoded to `%2520`.
bool IsPathSafe(unsigned char c) noexcept {
    if (IsUnreserved(c)) return true;
    switch (c) {
        case '!': case '$': case '&': case '\'': case '(': case ')':
        case '*': case '+': case ',': case ';': case '=':
        case ':': case '@': case '/':
            return true;
        default:
            return false;
    }
}

/// What may appear literally in a host: the path set, plus the three
/// characters an IPv6 literal is written with. `http://[::1]/a` is a legal
/// authority and `http://%5B::1%5D/a` is a different one that resolves to
/// nothing -- encoding the brackets does not make the URL safer, it makes it
/// wrong.
bool IsHostSafe(unsigned char c) noexcept {
    return c == '[' || c == ']' || c == ':';
}

void AppendPercent(std::string* out, unsigned char c) {
    static const char kHex[] = "0123456789ABCDEF";
    out->push_back('%');
    out->push_back(kHex[c >> 4]);
    out->push_back(kHex[c & 0x0F]);
}

/// Uppercases every escape, decodes every unreserved one, and encodes anything
/// that cannot appear literally in the component `hostSafe` selects.
std::string NormalizePercentEncoding(std::string_view text,
                                     bool hostSafe = false) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '%' && i + 2 < text.size() && IsHex(text[i + 1]) &&
            IsHex(text[i + 2])) {
            const unsigned char value = static_cast<unsigned char>(
                (HexValue(text[i + 1]) << 4) | HexValue(text[i + 2]));
            if (IsUnreserved(value)) {
                out.push_back(static_cast<char>(value));
            } else {
                AppendPercent(&out, value);
            }
            i += 2;
            continue;
        }
        if (IsPathSafe(c) || (hostSafe && IsHostSafe(c))) {
            out.push_back(static_cast<char>(c));
        } else {
            AppendPercent(&out, c);
        }
    }
    return out;
}

/// RFC 3986 §5.2.4.
///
/// Exactly the algorithm in the RFC rather than a segment vector, because the
/// two differ on inputs that occur in practice: a trailing `..` must leave a
/// trailing `/`, and `..` at the root must be discarded rather than escaping
/// it. A resolver that lets `../../../../etc/passwd` climb above the authority
/// has turned a relative reference into a different asset.
std::string RemoveDotSegments(std::string_view path) {
    std::string in(path);
    std::string out;
    while (!in.empty()) {
        if (in.rfind("../", 0) == 0) {
            in.erase(0, 3);
        } else if (in.rfind("./", 0) == 0) {
            in.erase(0, 2);
        } else if (in.rfind("/./", 0) == 0) {
            in.replace(0, 3, "/");
        } else if (in == "/.") {
            in = "/";
        } else if (in.rfind("/../", 0) == 0) {
            in.replace(0, 4, "/");
            const std::size_t slash = out.find_last_of('/');
            out.erase(slash == std::string::npos ? 0 : slash);
        } else if (in == "/..") {
            in = "/";
            const std::size_t slash = out.find_last_of('/');
            out.erase(slash == std::string::npos ? 0 : slash);
        } else if (in == "." || in == "..") {
            in.clear();
        } else {
            const std::size_t start = (in[0] == '/') ? 1 : 0;
            const std::size_t slash = in.find('/', start);
            out.append(in, 0, slash == std::string::npos ? in.size() : slash);
            in.erase(0, slash == std::string::npos ? in.size() : slash);
        }
    }
    return out;
}

/// RFC 3986 §5.3, assembled from parts that are already normalized.
std::string Compose(const std::string& scheme,
                    const std::string& authority,
                    const std::string& path,
                    const std::string& query,
                    bool hasQuery) {
    std::string out = scheme;
    out += "://";
    out += authority;
    out += path;
    if (hasQuery) {
        out += '?';
        out += query;
    }
    return out;
}

/// Splits a URI reference. Accepts anything RFC 3986 calls one, including a
/// relative reference and a scheme this bundle does not serve; deciding what to
/// do about those is the caller's job.
bool SplitUriReference(std::string_view text, Parts* out) {
    if (text.empty()) return false;

    const std::size_t hash = text.find('#');
    if (hash != std::string_view::npos) text = text.substr(0, hash);

    const std::size_t question = text.find('?');
    if (question != std::string_view::npos) {
        out->query.assign(text.substr(question + 1));
        out->hasQuery = true;
        text = text.substr(0, question);
    }

    // A scheme is ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":", and only
    // before the first "/". `sub/dir:name.usda` is a relative path, not a
    // scheme, and misreading it as one loses the asset.
    if (!text.empty() && IsAlpha(text[0])) {
        std::size_t i = 1;
        while (i < text.size() &&
               (IsAlpha(text[i]) || IsDigit(text[i]) || text[i] == '+' ||
                text[i] == '-' || text[i] == '.')) {
            ++i;
        }
        if (i < text.size() && text[i] == ':') {
            out->scheme.reserve(i);
            for (std::size_t j = 0; j < i; ++j) {
                out->scheme.push_back(LowerAscii(text[j]));
            }
            text = text.substr(i + 1);
        }
    }

    if (text.rfind("//", 0) == 0) {
        text = text.substr(2);
        const std::size_t slash = text.find('/');
        out->hasAuthority = true;
        out->authority.assign(
            text.substr(0, slash == std::string_view::npos ? text.size() : slash));
        text = slash == std::string_view::npos ? std::string_view()
                                               : text.substr(slash);
    }

    out->path.assign(text);
    return true;
}

/// Lowercases the host, drops the userinfo, and drops a port that is the
/// scheme's default. Returns false for an authority with no host, which is not
/// a remote asset however it is spelled.
bool NormalizeAuthority(std::string_view authority,
                        bool secure,
                        std::string* out) {
    // Everything through the last "@" is userinfo. Dropped, not preserved:
    // design policy §4.3 keeps a credential out of the resolver API, and an
    // identifier is the resolver API.
    const std::size_t at = authority.rfind('@');
    if (at != std::string_view::npos) authority = authority.substr(at + 1);

    std::string_view host = authority;
    std::string_view port;
    if (!authority.empty() && authority.front() == '[') {
        // An IPv6 literal keeps its brackets and its colons; only a colon
        // *after* the closing bracket introduces a port.
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) return false;
        host = authority.substr(0, close + 1);
        const std::string_view rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return false;
            port = rest.substr(1);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        }
    }

    if (host.empty()) return false;

    std::string normalizedHost;
    normalizedHost.reserve(host.size());
    for (const char c : NormalizePercentEncoding(host, /*hostSafe=*/true)) {
        normalizedHost.push_back(LowerAscii(c));
    }

    // An empty port (`host:`) means the default, and so does the default
    // written out. Both are removed so that three spellings of one origin are
    // one identifier.
    std::string normalizedPort;
    if (!port.empty()) {
        unsigned long value = 0;
        for (const char c : port) {
            if (!IsDigit(c)) return false;
            value = value * 10 + static_cast<unsigned long>(c - '0');
            if (value > 65535) return false;
        }
        if (value != (secure ? 443u : 80u)) {
            normalizedPort = ":" + std::to_string(value);
        }
    }

    *out = normalizedHost + normalizedPort;
    return true;
}

bool IsClaimed(const std::string& scheme) noexcept {
    return scheme == "http" || scheme == "https";
}

/// Normalizes parts that are already absolute: a claimed scheme and an
/// authority. RESOLVER.md §2.1 in full.
std::string NormalizeAbsolute(const Parts& parts) {
    if (!IsClaimed(parts.scheme) || !parts.hasAuthority) return std::string();

    std::string authority;
    if (!NormalizeAuthority(parts.authority, parts.scheme == "https",
                            &authority)) {
        return std::string();
    }

    // Percent-encoding first, dot segments second. The order is load-bearing:
    // `%2E%2E` is an encoded `..`, and removing dot segments before decoding
    // leaves it in the identifier, where the *server* resolves it -- which
    // means the resolver's idea of the asset and the origin's differ. Decoding
    // an encoded separator does not have the mirror-image problem, because
    // `%2F` is not unreserved and is never decoded to a `/`.
    std::string path = RemoveDotSegments(NormalizePercentEncoding(parts.path));
    if (path.empty()) path = "/";
    if (path.front() != '/') return std::string();

    return Compose(parts.scheme, authority, path, parts.query, parts.hasQuery);
}

/// RFC 3986 §5.3's merge, for a base that always has an authority.
std::string MergePath(const Parts& base, const std::string& reference) {
    if (base.path.empty()) return "/" + reference;
    const std::size_t slash = base.path.find_last_of('/');
    if (slash == std::string::npos) return "/" + reference;
    return base.path.substr(0, slash + 1) + reference;
}

}  // namespace

bool IsClaimedScheme(std::string_view assetPath) noexcept {
    Parts parts;
    if (!SplitUriReference(assetPath, &parts)) return false;
    return IsClaimed(parts.scheme);
}

std::string CreateIdentifier(std::string_view assetPath,
                             std::string_view anchorAssetPath) {
    Parts reference;
    if (!SplitUriReference(assetPath, &reference)) return std::string();

    // An absolute URI ignores the anchor entirely, whatever the anchor is.
    if (!reference.scheme.empty()) {
        return NormalizeAbsolute(reference);
    }

    // A relative reference needs a remote anchor. Without one this is not this
    // resolver's business: a relative path anchored to a local layer belongs to
    // the primary resolver, and answering for it would change how local assets
    // resolve, which RESOLVER.md §1 forbids.
    if (anchorAssetPath.empty()) return std::string();

    Parts base;
    if (!SplitUriReference(anchorAssetPath, &base)) return std::string();
    if (!IsClaimed(base.scheme) || !base.hasAuthority) return std::string();

    Parts target;
    target.scheme = base.scheme;
    if (reference.hasAuthority) {
        // A network-path reference (`//other.example/a`) keeps the scheme and
        // replaces the authority. It is rare and it is legal.
        target.hasAuthority = true;
        target.authority = reference.authority;
        target.path = reference.path;
        target.query = reference.query;
        target.hasQuery = reference.hasQuery;
    } else {
        target.hasAuthority = true;
        target.authority = base.authority;
        if (reference.path.empty()) {
            // `?query` alone, or the empty reference: the same document, and
            // the base's query survives only when the reference has none.
            target.path = base.path;
            if (reference.hasQuery) {
                target.query = reference.query;
                target.hasQuery = true;
            } else {
                target.query = base.query;
                target.hasQuery = base.hasQuery;
            }
        } else {
            target.path = reference.path.front() == '/'
                              ? reference.path
                              : MergePath(base, reference.path);
            target.query = reference.query;
            target.hasQuery = reference.hasQuery;
        }
    }

    return NormalizeAbsolute(target);
}

std::string ExtensionOf(std::string_view assetPath) {
    // Split rather than trimmed, so that the authority is not mistaken for a
    // path segment. `https://example.org` has no path at all, and reading the
    // text after its last `/` gives `org` -- which `_GetExtension` would then
    // hand OpenUSD as a file format. The trailing-slash spelling of the same URL
    // gives the right answer, so trimming makes the result depend on whether
    // normalization has run.
    //
    // The fragment and the query are removed by the split. Neither is part of
    // the name of the thing, and a pre-signed URL always carries one.
    Parts parts;
    if (!SplitUriReference(assetPath, &parts)) return std::string();

    const std::size_t slash = parts.path.find_last_of('/');
    const std::string_view segment =
        slash == std::string::npos ? std::string_view(parts.path)
                                   : std::string_view(parts.path).substr(slash + 1);

    const std::size_t dot = segment.find_last_of('.');
    // A leading dot is a hidden name, not an extension, which is what
    // `ArResolver`'s own default says too.
    if (dot == std::string_view::npos || dot == 0) return std::string();
    return std::string(segment.substr(dot + 1));
}

}  // namespace usdhttpresolver
