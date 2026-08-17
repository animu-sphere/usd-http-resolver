// SPDX-License-Identifier: Apache-2.0
//
// Reference resolution and the refusals around it.
//
// A `Location` is untrusted input from a remote server, and this is the code
// that decides what URL the next request goes to. Every case below is either a
// hop a redirect chain legitimately needs or something a hostile origin could
// put in the header.

#include <string>

#include "Check.h"
#include "Uri.h"

namespace {

using usdasset::http::IsSchemeDowngrade;
using usdasset::http::ParseUri;
using usdasset::http::RemoveDotSegments;
using usdasset::http::ResolveReference;
using usdasset::http::Uri;

Uri Parse(const std::string& text) {
    Uri uri;
    std::string error;
    CHECK(ParseUri(text, &uri, &error));
    return uri;
}

std::string Resolve(const std::string& base, const std::string& reference) {
    Uri resolved;
    std::string error;
    if (!ResolveReference(Parse(base), reference, &resolved, &error)) return std::string();
    return resolved.ToString();
}

void TestParse() {
    const Uri uri = Parse("http://Example.ORG:8080/a/b.usda?x=1&y=2");
    CHECK_EQ(uri.scheme, std::string("http"));
    // Lowercased, because a host is case-insensitive and two spellings of one
    // asset must not become two identities.
    CHECK_EQ(uri.host, std::string("example.org"));
    CHECK_EQ(uri.port, 8080);
    CHECK_EQ(uri.path, std::string("/a/b.usda"));
    // Verbatim and unreordered. A pre-signed object-storage URL carries its
    // signature here, and canonicalizing it invalidates the signature.
    CHECK_EQ(uri.query, std::string("x=1&y=2"));

    // A default port is not spelled back out.
    CHECK_EQ(Parse("https://example.org:443/a").ToString(),
             std::string("https://example.org/a"));
    CHECK_EQ(Parse("http://example.org:80/a").ToString(),
             std::string("http://example.org/a"));

    // An empty path is `/` on the wire either way.
    CHECK_EQ(Parse("https://example.org").path, std::string("/"));
    CHECK_EQ(Parse("https://example.org?q=1").path, std::string("/"));

    // A fragment is a client-side selector and is never sent to a server.
    CHECK_EQ(Parse("https://example.org/a#frag").ToString(),
             std::string("https://example.org/a"));

    CHECK_EQ(Parse("https://[::1]:8080/a").host, std::string("[::1]"));
    CHECK_EQ(Parse("https://[::1]:8080/a").port, 8080);

    Uri uriOut;
    std::string error;
    CHECK(!ParseUri("s3://bucket/key", &uriOut, &error));
    // Named, not generic: a caller that routed an `s3://` identifier here has a
    // routing bug, and "malformed URI" sends it to the wrong place.
    CHECK(error.find("scheme") != std::string::npos);
    CHECK(!ParseUri("/just/a/path", &uriOut, &error));
    CHECK(!ParseUri("https:///nohost", &uriOut, &error));
    CHECK(!ParseUri("https://example.org:99999/a", &uriOut, &error));
    CHECK(!ParseUri("https://example.org:0/a", &uriOut, &error));
    CHECK(!ParseUri("https://example.org:8o8/a", &uriOut, &error));
}

void TestUserinfoNeverReachesIdentity() {
    const Uri uri = Parse("https://user:token@example.org/a?sig=abc");
    // On the wire, credentials included: that is what they are for.
    CHECK_EQ(uri.ToString(), std::string("https://user:token@example.org/a?sig=abc"));
    // Everywhere else, removed. This is the value that becomes
    // `resolvedIdentifier`, the key of a metrics dump, and -- in v0.3.0 -- part
    // of a cache key.
    CHECK_EQ(uri.ToIdentity(), std::string("https://example.org/a?sig=abc"));
    CHECK_EQ(uri.userinfo, std::string("user:token"));

    // The last `@` separates, not the first. Splitting on the first hands part
    // of a credential to the connection as a hostname.
    CHECK_EQ(Parse("https://user@name:pw@example.org/a").host, std::string("example.org"));

    // `Host` never carries the credential either.
    CHECK_EQ(uri.Authority(), std::string("example.org"));
}

void TestDotSegments() {
    CHECK_EQ(RemoveDotSegments("/a/b/c/./../../g"), std::string("/a/g"));
    CHECK_EQ(RemoveDotSegments("/a/./b/../b/c"), std::string("/a/b/c"));
    CHECK_EQ(RemoveDotSegments("/a/b/"), std::string("/a/b/"));
    CHECK_EQ(RemoveDotSegments("/"), std::string("/"));
    // A `..` that would escape the root is discarded rather than honored: it
    // names nothing on the server, and an origin must not be able to walk a
    // client's path arithmetic backwards past `/`.
    CHECK_EQ(RemoveDotSegments("/../../etc/passwd"), std::string("/etc/passwd"));
    CHECK_EQ(RemoveDotSegments("/a/../.."), std::string("/"));
}

void TestReferenceResolution() {
    const std::string base = "https://example.org/scenes/shot_010/main.usda";

    // The case that makes a remote scene work at all: a layer published to a
    // CDN references its neighbours relatively, and none of those references
    // mentions a host (RESOLVER.md §2.2).
    CHECK_EQ(Resolve(base, "../assets/tree.usda"),
             std::string("https://example.org/scenes/assets/tree.usda"));
    CHECK_EQ(Resolve(base, "sibling.usda"),
             std::string("https://example.org/scenes/shot_010/sibling.usda"));
    CHECK_EQ(Resolve(base, "/absolute/x.usda"),
             std::string("https://example.org/absolute/x.usda"));
    CHECK_EQ(Resolve(base, "https://cdn.example.net/x.usda"),
             std::string("https://cdn.example.net/x.usda"));

    // A network-path reference inherits the scheme and nothing else.
    CHECK_EQ(Resolve(base, "//cdn.example.net/x.usda"),
             std::string("https://cdn.example.net/x.usda"));

    // Query-only, which object storage uses to hand out a signed URL for the
    // same object.
    CHECK_EQ(Resolve(base, "?token=abc"),
             std::string("https://example.org/scenes/shot_010/main.usda?token=abc"));

    // A reference the backend must refuse rather than guess at.
    CHECK_EQ(Resolve(base, "mailto:someone@example.org"), std::string());
    CHECK_EQ(Resolve(base, ""), std::string());

    // The credential travels with the hop, because the hop is the same
    // exchange -- but only within the same origin's URL, and never into an
    // identity.
    Uri resolved;
    std::string error;
    CHECK(ResolveReference(Parse("https://user:pw@example.org/a/b"), "c",
                           &resolved, &error));
    CHECK_EQ(resolved.ToString(), std::string("https://user:pw@example.org/a/c"));
    CHECK_EQ(resolved.ToIdentity(), std::string("https://example.org/a/c"));
}

void TestSchemeDowngrade() {
    // §10 of the design policy. This is the case the hostile corpus cannot
    // carry: the fixture server speaks plaintext HTTP, so there is no `https`
    // to be downgraded from, and faking one would assert nothing.
    CHECK(IsSchemeDowngrade(Parse("https://example.org/a"),
                            Parse("http://example.org/a")));
    CHECK(IsSchemeDowngrade(Parse("https://example.org/a"),
                            Parse("http://elsewhere.example/a")));

    // Not downgrades: staying, and upgrading.
    CHECK(!IsSchemeDowngrade(Parse("https://example.org/a"),
                             Parse("https://elsewhere.example/a")));
    CHECK(!IsSchemeDowngrade(Parse("http://example.org/a"),
                             Parse("https://example.org/a")));
    // A plaintext origin redirecting to plaintext has no security to drop.
    CHECK(!IsSchemeDowngrade(Parse("http://example.org/a"),
                             Parse("http://elsewhere.example/a")));
}

}  // namespace

int main() {
    TestParse();
    TestUserinfoNeverReachesIdentity();
    TestDotSegments();
    TestReferenceResolution();
    TestSchemeDowngrade();
    return usdassettest::Report("usdAssetHttp/uri");
}
