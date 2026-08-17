// SPDX-License-Identifier: Apache-2.0
//
// Just enough URI to follow a redirect safely.
//
// This is deliberately not the normalization in RESOLVER.md §2.1. That belongs
// to `plugins/http-resolver`, which owns identifier creation and anchoring, and
// duplicating it here would give the repository two answers to "what is this
// asset called". What the backend owns is narrower and is forced on it by
// redirects: a `Location` may be a relative reference (RFC 3986 §5), so the
// backend has to be able to resolve one against the URL it just requested, and
// §10 of the design policy requires it to refuse an `https` to `http` downgrade
// while doing so.
//
// Internal to usdAssetHttp. No header outside src/ includes this.

#ifndef USDASSETHTTP_URI_H
#define USDASSETHTTP_URI_H

#include <string>
#include <string_view>

namespace usdasset {
namespace http {

/// An absolute `http` or `https` URI, split into the parts a request needs.
///
/// The fragment is not a member: it is removed at parse and never sent to a
/// server, per RESOLVER.md §2.1. The query is kept verbatim and is never
/// reordered or re-encoded, because a pre-signed object-storage URL carries its
/// signature there and canonicalizing it invalidates the signature.
struct Uri {
    std::string scheme;    ///< Lowercased. `http` or `https`, nothing else.
    std::string userinfo;  ///< As written, without the `@`. Never logged.
    std::string host;      ///< Lowercased. Bracketed literal for IPv6.
    int port = 0;          ///< 0 means the scheme's default.
    std::string path;      ///< Always begins with `/`.
    std::string query;     ///< Without the `?`. Empty when absent.
    bool hasQuery = false; ///< Distinguishes `?` from no query at all.

    bool IsSecure() const noexcept { return scheme == "https"; }

    /// The scheme's default port, which `port == 0` stands for.
    int DefaultPort() const noexcept { return IsSecure() ? 443 : 80; }

    /// `host[:port]`, with the userinfo excluded. This is what goes on the wire
    /// as `Host`, and excluding the userinfo is not a stylistic choice: a
    /// credential in an authority must not be echoed into a header.
    std::string Authority() const;

    /// The full request URL, userinfo included, for handing to the transport.
    /// This value goes on a socket and nowhere else.
    std::string ToString() const;

    /// The same URL with the userinfo component removed.
    ///
    /// This, never `ToString()`, is what becomes `resolvedIdentifier`, what a
    /// message quotes, and what a metrics dump keys on. §4.3 of the design
    /// policy: credentials never enter the resolver API, a cache key, a
    /// diagnostic, or a log line, and `https://user:token@host/a` hides one in
    /// the part a query-string rule keeps.
    ///
    /// Two URLs that differ only in their credentials therefore share one
    /// identity, which is the right answer: they name the same bytes.
    std::string ToIdentity() const;
};

/// Parses an absolute `http` or `https` URI.
///
/// Returns false, with a reason in `*errorOut`, for anything else -- including
/// a scheme this backend does not serve. A relative reference is not an
/// absolute URI and is rejected here; `ResolveReference` is what accepts one.
bool ParseUri(std::string_view text, Uri* out, std::string* errorOut);

/// Resolves `reference` against `base`, per RFC 3986 §5.2.
///
/// `reference` is whatever a server put in `Location`, which is untrusted
/// input: it may be an absolute URI, a network-path reference (`//host/path`),
/// an absolute path, or a relative path, and it may be nonsense. Every form
/// that is not an `http` or `https` URI after resolution is rejected rather
/// than guessed at.
bool ResolveReference(const Uri& base,
                      std::string_view reference,
                      Uri* out,
                      std::string* errorOut);

/// Whether following `base` -> `target` would drop TLS.
///
/// §10 of the design policy: bound redirect chains and reject scheme
/// downgrades. A server that answers an `https` request with an `http`
/// `Location` is asking the client to re-issue the request in the clear, and
/// the client that obliges has quietly removed the transport security the
/// caller asked for.
bool IsSchemeDowngrade(const Uri& base, const Uri& target) noexcept;

/// Removes `.` and `..` segments, per RFC 3986 §5.2.4. Exposed for its own
/// test: it is the part of reference resolution that is easy to get subtly
/// wrong and impossible to notice from the outside.
std::string RemoveDotSegments(std::string_view path);

}  // namespace http
}  // namespace usdasset

#endif  // USDASSETHTTP_URI_H
