// SPDX-License-Identifier: Apache-2.0
//
// Identifier creation: RESOLVER.md §2.1 and §2.2 as a table.
//
// This executable links `src/Identifier.cpp` and nothing else -- no OpenUSD, no
// backend, no sockets. That is the point of having kept the normalization pure:
// the mistakes it can make are invisible from the outside (two spellings of one
// asset become two opens, two readers, and two revisions) and a test that would
// catch them should not need a USD runtime to run.

#include <string>
#include <vector>

#include "Check.h"
#include "Identifier.h"

namespace {

using usdhttpresolver::CreateIdentifier;

/// A string comparison that prints both sides. `CHECK_EQ` reports that two
/// strings differed, which for a URL is the least useful half of the answer.
#define CHECK_ID(assetPath, anchor, expected)                                 \
    do {                                                                      \
        const std::string _actual = CreateIdentifier((assetPath), (anchor));  \
        const std::string _expected = (expected);                             \
        if (_actual != _expected) {                                           \
            std::fprintf(stderr,                                              \
                         "FAIL %s:%d\n  asset    %s\n  anchor   %s\n"         \
                         "  expected %s\n  actual   %s\n",                    \
                         __FILE__, __LINE__, (assetPath), (anchor),           \
                         _expected.c_str(), _actual.c_str());                 \
            ++::usdassettest::FailureCount();                                 \
        }                                                                     \
    } while (false)

/// §2.1: one asset has one identifier, however it was spelled.
void TestNormalization() {
    // Scheme and host are case-insensitive; the path is not. A resolver that
    // lowercased the path would open the wrong object on any origin whose
    // storage is case-sensitive, which is all of them.
    CHECK_ID("HTTPS://Example.ORG/Scenes/Main.usda", "",
             "https://example.org/Scenes/Main.usda");

    // The default port is not part of the name of the thing.
    CHECK_ID("https://example.org:443/a.usda", "", "https://example.org/a.usda");
    CHECK_ID("http://example.org:80/a.usda", "", "http://example.org/a.usda");
    CHECK_ID("http://example.org:/a.usda", "", "http://example.org/a.usda");
    // A non-default port is.
    CHECK_ID("http://example.org:8080/a.usda", "",
             "http://example.org:8080/a.usda");
    CHECK_ID("https://example.org:80/a.usda", "",
             "https://example.org:80/a.usda");

    // Dot segments are resolved, including the ones that would climb above the
    // authority. `..` at the root is discarded, never followed.
    CHECK_ID("https://example.org/a/b/../c/./d.usda", "",
             "https://example.org/a/c/d.usda");
    CHECK_ID("https://example.org/../../etc/passwd", "",
             "https://example.org/etc/passwd");

    // An empty path is `/`.
    CHECK_ID("https://example.org", "", "https://example.org/");
    CHECK_ID("https://example.org?q=1", "", "https://example.org/?q=1");

    // Percent-encoding: uppercase hex, unreserved decoded.
    CHECK_ID("https://example.org/a%7Eb%2fc%20d.usda", "",
             "https://example.org/a~b%2Fc%20d.usda");
    // A character that cannot appear literally is encoded rather than passed
    // through, and an escape that is already there is not doubly encoded.
    CHECK_ID("https://example.org/tree bark.png", "",
             "https://example.org/tree%20bark.png");
    CHECK_ID("https://example.org/tree%20bark.png", "",
             "https://example.org/tree%20bark.png");
    CHECK_ID("https://example.org/100%.usda", "",
             "https://example.org/100%25.usda");

    // An encoded dot segment is decoded before the dot segments are removed,
    // so the identifier and the origin agree on which asset was named. An
    // encoded separator is not: `%2F` is not unreserved and stays a character
    // in a segment rather than becoming one between segments.
    CHECK_ID("https://example.org/a/%2E%2E/b.usda", "",
             "https://example.org/b.usda");
    CHECK_ID("https://example.org/a%2F%2E%2E%2Fb.usda", "",
             "https://example.org/a%2F..%2Fb.usda");

    // The query is verbatim: a pre-signed URL carries its signature there, and
    // sorting or re-encoding the parameters invalidates it.
    CHECK_ID("https://example.org/a.usda?b=2&a=1&sig=x%2fy", "",
             "https://example.org/a.usda?b=2&a=1&sig=x%2fy");

    // The fragment is removed and never sent to a server.
    CHECK_ID("https://example.org/a.usda#/World/Mesh", "",
             "https://example.org/a.usda");
    CHECK_ID("https://example.org/a.usda?t=1#frag", "",
             "https://example.org/a.usda?t=1");

    // The userinfo is removed: design policy §4.3 keeps a credential out of the
    // resolver API, and an identifier is the resolver API.
    CHECK_ID("https://user:token@example.org/a.usda", "",
             "https://example.org/a.usda");
    CHECK_ID("https://user@example.org:443/a.usda", "",
             "https://example.org/a.usda");

    // An IPv6 literal keeps its brackets and its own colons.
    CHECK_ID("http://[::1]:8080/a.usda", "", "http://[::1]:8080/a.usda");
    CHECK_ID("http://[::1]:80/a.usda", "", "http://[::1]/a.usda");
}

/// §2.2: what makes a remote scene work at all.
void TestAnchoring() {
    const char* anchor = "https://example.org/scenes/shot_010/main.usda";

    CHECK_ID("tree.usda", anchor, "https://example.org/scenes/shot_010/tree.usda");
    CHECK_ID("./tree.usda", anchor,
             "https://example.org/scenes/shot_010/tree.usda");
    CHECK_ID("../assets/tree.usda", anchor,
             "https://example.org/scenes/assets/tree.usda");
    CHECK_ID("../../assets/tree.usda", anchor,
             "https://example.org/assets/tree.usda");
    // Above the root, and still inside the authority.
    CHECK_ID("../../../../assets/tree.usda", anchor,
             "https://example.org/assets/tree.usda");

    // An absolute path anchors to the scheme and authority.
    CHECK_ID("/assets/tree.usda", anchor, "https://example.org/assets/tree.usda");

    // An absolute URI passes through, anchor or no anchor.
    CHECK_ID("https://cdn.example.net/a.usda", anchor,
             "https://cdn.example.net/a.usda");

    // A network-path reference keeps the scheme and replaces the authority.
    CHECK_ID("//cdn.example.net/a.usda", anchor,
             "https://cdn.example.net/a.usda");

    // A query-only reference is the same document with a different query; an
    // empty reference is the same document, query included.
    CHECK_ID("?v=2", "https://example.org/a.usda?v=1",
             "https://example.org/a.usda?v=2");
    CHECK_ID("#frag", "https://example.org/a.usda?v=1",
             "https://example.org/a.usda?v=1");

    // The anchor's query does not survive onto a sibling: it belongs to the
    // anchor, not to the directory.
    CHECK_ID("tree.usda", "https://example.org/scenes/main.usda?token=abc",
             "https://example.org/scenes/tree.usda");

    // A trailing-slash anchor is a directory; a bare host anchor is the root.
    CHECK_ID("tree.usda", "https://example.org/scenes/",
             "https://example.org/scenes/tree.usda");
    CHECK_ID("tree.usda", "https://example.org", "https://example.org/tree.usda");
}

/// What this bundle declines to answer for. An empty identifier is "not mine",
/// and the host's primary resolver takes it from there.
void TestNotClaimed() {
    // A relative reference with no anchor, or with a local one, is the primary
    // resolver's business. Answering for it would change how a local asset
    // opens, which RESOLVER.md §1 forbids.
    CHECK_ID("tree.usda", "", "");
    CHECK_ID("tree.usda", "/home/user/scenes/main.usda", "");
    CHECK_ID("tree.usda", "C:/scenes/main.usda", "");
    CHECK_ID("../tree.usda", "file:///home/user/main.usda", "");

    // A scheme this bundle does not serve.
    CHECK_ID("ftp://example.org/a.usda", "", "");
    CHECK_ID("s3://bucket/a.usda", "", "");
    CHECK_ID("s3://bucket/a.usda", "https://example.org/main.usda", "");

    // Malformed, and therefore refused rather than guessed at.
    CHECK_ID("", "", "");
    CHECK_ID("https://", "", "");
    CHECK_ID("https:///a.usda", "", "");
    CHECK_ID("https:a.usda", "", "");
    CHECK_ID("https://example.org:99999/a.usda", "", "");
    CHECK_ID("https://example.org:8o80/a.usda", "", "");

    CHECK(usdhttpresolver::IsClaimedScheme("http://example.org/a"));
    CHECK(usdhttpresolver::IsClaimedScheme("HTTPS://example.org/a"));
    CHECK(!usdhttpresolver::IsClaimedScheme("s3://bucket/a"));
    CHECK(!usdhttpresolver::IsClaimedScheme("relative/a.usda"));
    // A relative path whose first segment contains a colon is not a scheme.
    CHECK(!usdhttpresolver::IsClaimedScheme("sub/dir:name.usda"));
    CHECK_ID("sub/dir:name.usda", "https://example.org/scenes/main.usda",
             "https://example.org/scenes/sub/dir:name.usda");
}

/// Normalizing an identifier again must not change it. Without this, two
/// resolutions of one asset -- one through `_CreateIdentifier`, one through
/// `_Resolve` on the result -- could produce two identifiers and two opens.
void TestIdempotence() {
    const std::vector<std::string> inputs = {
        "HTTPS://Example.ORG:443/a/../b/c%7Ed.usda?q=1#f",
        "http://user@example.org:80/tree bark.png",
        "https://[::1]/a.usda",
        "http://example.org",
        "https://example.org/100%.usda",
    };
    for (const std::string& input : inputs) {
        const std::string once = CreateIdentifier(input, "");
        CHECK(!once.empty());
        const std::string twice = CreateIdentifier(once, "");
        if (once != twice) {
            std::fprintf(stderr,
                         "FAIL %s:%d not idempotent\n  once  %s\n  twice %s\n",
                         __FILE__, __LINE__, once.c_str(), twice.c_str());
            ++::usdassettest::FailureCount();
        }
    }
}

/// The extension a file format is chosen by, which the query string must not
/// take part in.
void TestExtension() {
    CHECK(usdhttpresolver::ExtensionOf("https://example.org/a/main.usda") ==
          "usda");
    CHECK(usdhttpresolver::ExtensionOf(
              "https://example.org/a/main.usda?X-Amz-Signature=abc.def") ==
          "usda");
    CHECK(usdhttpresolver::ExtensionOf("https://example.org/a/main.usdc#p") ==
          "usdc");
    CHECK(usdhttpresolver::ExtensionOf("https://example.org/a.b/main") == "");
    CHECK(usdhttpresolver::ExtensionOf("https://example.org/a/.hidden") == "");
    CHECK(usdhttpresolver::ExtensionOf("https://example.org/") == "");
}

}  // namespace

int main() {
    TestNormalization();
    TestAnchoring();
    TestNotClaimed();
    TestIdempotence();
    TestExtension();
    return usdassettest::Report("httpResolver identifier");
}
