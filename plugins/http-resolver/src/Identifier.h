// SPDX-License-Identifier: Apache-2.0
//
// Identifier creation: normalization, anchoring, and the extension a file
// format is chosen by.
//
// This is RESOLVER.md §2.1 and §2.2 in code, and it is the resolver's half of
// the answer to "what is this asset called". The backend's `src/Uri.h` is
// deliberately *not* this: what it owns is reference resolution against the URL
// it just requested, because a `Location` header may be relative, and it says
// so in its own header. Normalization belongs here, where identifiers are made.
//
// Nothing in this file includes an OpenUSD header. That is not incidental: it
// is the part of the bundle that is pure arithmetic over strings, it is where a
// subtle mistake is invisible from the outside -- two spellings of one asset
// producing two opens and two revisions -- and a test for it should not need a
// USD runtime to run. `httpResolver_test_identifier` links this translation
// unit alone.

#ifndef USDHTTPRESOLVER_IDENTIFIER_H
#define USDHTTPRESOLVER_IDENTIFIER_H

#include <string>
#include <string_view>

namespace usdhttpresolver {

/// Whether `assetPath` begins with a scheme this bundle claims.
///
/// Answers a syntactic question and not an existence one: `https://` followed
/// by nonsense is claimed, and then rejected by `CreateIdentifier`. The
/// distinction matters because a path this bundle does not claim must be left
/// alone rather than failed -- it belongs to the host's primary resolver, and
/// installing this plugin never changes how a local asset opens
/// (RESOLVER.md §1).
bool IsClaimedScheme(std::string_view assetPath) noexcept;

/// The normalized absolute URI for `assetPath`, anchored to `anchorAssetPath`.
///
/// Returns an empty string when the result would not be an `http` or `https`
/// URI this bundle serves -- an unclaimed scheme, a relative reference with no
/// remote anchor, or a URI that does not parse. An empty return is "not mine",
/// never "broken": `ArResolver` has no other way to say so, and a resolver that
/// guesses at a malformed URI is a resolver that opens the wrong asset.
///
/// Normalization, per RESOLVER.md §2.1:
///
///   - the scheme and host are lowercased;
///   - a default port (`80` for `http`, `443` for `https`) is removed, as is
///     an empty one;
///   - the path is dot-segment-resolved and an empty path becomes `/`;
///   - percent-encoding is normalized to uppercase hex and unreserved
///     characters are decoded;
///   - the query string is preserved **verbatim**, in its original order;
///   - the fragment is removed and never sent to a server;
///   - the userinfo component is removed.
///
/// The last two rules are the ones worth stating twice. The query is never
/// reordered because a pre-signed object-storage URL carries its signature
/// there and sorting the parameters invalidates it. The userinfo is dropped
/// because §4.3 of the design policy says a credential never enters the
/// resolver API, and an identifier is the resolver API: `https://user:t@h/a`
/// hides one in the part a query-string rule keeps. A URL that needs
/// credentials therefore fails at the server with `AccessDenied` rather than
/// succeeding with a secret in every log line; authentication is the
/// interception point in `v0.6.0`, not a URL component.
///
/// Anchoring is RFC 3986 §5.2 reference resolution against `anchorAssetPath`,
/// which is what makes a remote scene work at all: a layer published to a CDN
/// references its neighbours relatively and none of those references mentions a
/// host. A relative reference anchored to a *local* layer returns empty and is
/// left to the primary resolver.
std::string CreateIdentifier(std::string_view assetPath,
                             std::string_view anchorAssetPath);

/// The extension of `assetPath`, without the leading dot, ignoring the query
/// and the fragment.
///
/// `ArResolver`'s default implementation takes the text after the last `.` in
/// the last path component, which for
/// `https://example.org/scenes/main.usda?token=abc` is `usda?token=abc` -- no
/// file format matches that, and a signed URL becomes an asset OpenUSD cannot
/// identify. The query is not part of the name of the thing.
std::string ExtensionOf(std::string_view assetPath);

}  // namespace usdhttpresolver

#endif  // USDHTTPRESOLVER_IDENTIFIER_H
