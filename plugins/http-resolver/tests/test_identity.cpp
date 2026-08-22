// SPDX-License-Identifier: Apache-2.0
//
// What may be published about an asset's identity, and what may not:
// RESOLVER.md §3.
//
// Offline, and linking one translation unit, because the mistakes this code can
// make are silent ones. A token published for a validator that cannot prove a
// revision does not fail here; it fails in a consumer, weeks later, as a
// generated cache that served the wrong bytes -- and by then the resolver looks
// correct, because every read it performed was correct. The rules are therefore
// asserted as a table rather than exercised through a stage.

#include <cstdint>
#include <string>

#include "Check.h"
#include "Identity.h"
#include "usdAssetIo/AssetReader.h"
#include "usdAssetIo/Validator.h"

namespace {

using usdasset::AssetMetadata;
using usdasset::IdentityStability;
using usdasset::Validator;
using usdasset::ValidatorKind;
using usdasset::ValidatorStrength;
using usdhttpresolver::PublishedIdentity;
using usdhttpresolver::PublishIdentity;

/// One asset as a backend would have described it at open. `ClassifyStability`
/// derives the stability rather than the test stating it, so that a case cannot
/// assert a combination the contract forbids one layer down.
AssetMetadata Opened(const std::string& identifier, std::uint64_t size,
                     const std::string& value, ValidatorKind kind,
                     ValidatorStrength strength) {
    AssetMetadata metadata;
    metadata.resolvedIdentifier = identifier;
    metadata.size = size;
    metadata.supportsRandomAccess = true;
    metadata.validator = Validator{value, kind, strength};
    metadata.stability = usdasset::ClassifyStability(metadata.validator);
    return metadata;
}

/// A strong `ETag` is the whole point: it is the only validator that may key
/// something outliving the open that captured it (ASSET_READER.md §7.2).
void TestStrongValidatorIsReusable() {
    const PublishedIdentity published = PublishIdentity(
        Opened("https://example.org/a.usdc", 4096, "\"rev-7\"",
               ValidatorKind::EntityTag, ValidatorStrength::Strong),
        false);

    CHECK(published.resolvedIdentifier == "https://example.org/a.usdc");
    CHECK_EQ(published.size, std::uint64_t{4096});
    CHECK(published.validationToken == "\"rev-7\"");
    CHECK(published.stability == "Stable");
    CHECK(published.reusable);
}

/// A weak validator is still worth publishing -- it is positive evidence of
/// change -- and is still not allowed to key reuse. Both halves matter, and the
/// second is the one a consumer's fail-safe depends on.
void TestWeakValidatorIsVisibleAndNotReusable() {
    const PublishedIdentity weakTag = PublishIdentity(
        Opened("https://example.org/a.usdc", 10, "W/\"rev-7\"",
               ValidatorKind::EntityTag, ValidatorStrength::Weak),
        false);
    CHECK(weakTag.validationToken == "W/\"rev-7\"");
    CHECK(weakTag.stability == "Unstable");
    CHECK(!weakTag.reusable);

    // `Last-Modified` at one-second granularity cannot separate two revisions
    // published inside the same second, which is why it classifies weak.
    const PublishedIdentity date = PublishIdentity(
        Opened("https://example.org/a.usdc", 10,
               "Wed, 21 Oct 2026 07:28:00 GMT", ValidatorKind::HttpDate,
               ValidatorStrength::Weak),
        false);
    CHECK(date.stability == "Unstable");
    CHECK(!date.reusable);
}

/// No usable validator: readable, and no identity anybody may reuse. The
/// consumer is told so rather than left to guess.
void TestNoValidatorIsUnavailable() {
    const PublishedIdentity published = PublishIdentity(
        Opened("https://example.org/a.usdc", 10, std::string(),
               ValidatorKind::None, ValidatorStrength::None),
        false);

    CHECK(published.validationToken.empty());
    CHECK(published.stability == "Unavailable");
    CHECK(!published.reusable);
    // The rest of the identity is still published: a consumer that cannot reuse
    // still gets the size and the identifier it asked about.
    CHECK(published.resolvedIdentifier == "https://example.org/a.usdc");
    CHECK_EQ(published.size, std::uint64_t{10});
}

/// A strength without a kind is a producer bug, and it resolves toward the
/// weaker answer rather than toward the useful one.
void TestStrengthWithoutAKindIsNotAnIdentity() {
    AssetMetadata metadata =
        Opened("https://example.org/a.usdc", 10, std::string(),
               ValidatorKind::None, ValidatorStrength::Strong);
    // Stated rather than derived: this is the case where a backend contradicts
    // itself, so the test has to build the contradiction by hand.
    metadata.stability = IdentityStability::Stable;

    const PublishedIdentity published = PublishIdentity(metadata, false);
    CHECK(published.validationToken.empty());
    CHECK(published.stability == "Unavailable");
    CHECK(!published.reusable);
}

/// The republish case. `ArAssetInfo` is keyed by path and not by open asset, so
/// once two opens of one identifier have disagreed there is no way to tell
/// which revision a caller is holding -- and the answer stops being reusable
/// for the rest of the process.
void TestContradictedIdentityStopsBeingReusable() {
    const AssetMetadata metadata =
        Opened("https://example.org/a.usdc", 4096, "\"rev-8\"",
               ValidatorKind::EntityTag, ValidatorStrength::Strong);

    const PublishedIdentity published = PublishIdentity(metadata, true);
    CHECK(published.stability == "Unstable");
    CHECK(!published.reusable);
    // The token stays visible. It is evidence of change, which is exactly what
    // a consumer holding an older entry needs in order to invalidate it.
    CHECK(published.validationToken == "\"rev-8\"");

    // A contradiction cannot promote anything: an identity that was already not
    // reusable is not made worse or better by it.
    const PublishedIdentity absent = PublishIdentity(
        Opened("https://example.org/a.usdc", 4096, std::string(),
               ValidatorKind::None, ValidatorStrength::None),
        true);
    CHECK(absent.stability == "Unavailable");
    CHECK(!absent.reusable);
}

/// §3, last line: no credential appears in asset info. The identifier is the
/// one field that can carry one, and it carries two shapes of it.
void TestCredentialsAreElided() {
    const PublishedIdentity signedUrl = PublishIdentity(
        Opened("https://example.org/a.usdc?X-Amz-Signature=abc123", 10,
               "\"rev-9\"", ValidatorKind::EntityTag,
               ValidatorStrength::Strong),
        false);
    CHECK(signedUrl.resolvedIdentifier.find("abc123") == std::string::npos);
    CHECK(signedUrl.resolvedIdentifier == "https://example.org/a.usdc?<elided>");

    const PublishedIdentity userinfo = PublishIdentity(
        Opened("https://user:t0ken@example.org/a.usdc", 10, "\"rev-9\"",
               ValidatorKind::EntityTag, ValidatorStrength::Strong),
        false);
    CHECK(userinfo.resolvedIdentifier.find("t0ken") == std::string::npos);
    CHECK(userinfo.resolvedIdentifier == "https://<elided>@example.org/a.usdc");

    // And an elided identifier is still an identity: the token is what
    // distinguishes revisions, and the reuse rule does not depend on the URL
    // having survived intact.
    CHECK(signedUrl.reusable);
}

/// An identifier that elided to nothing has nothing to publish. Unreachable
/// from a backend that opened an asset, and asserted anyway, because `reusable`
/// is the field that must never be true by accident.
void TestEmptyIdentifierIsNotReusable() {
    const PublishedIdentity published = PublishIdentity(
        Opened(std::string(), 10, "\"rev-9\"", ValidatorKind::EntityTag,
               ValidatorStrength::Strong),
        false);
    CHECK(!published.reusable);
    CHECK(published.stability == "Unavailable");
}

}  // namespace

int main() {
    TestStrongValidatorIsReusable();
    TestWeakValidatorIsVisibleAndNotReusable();
    TestNoValidatorIsUnavailable();
    TestStrengthWithoutAKindIsNotAnIdentity();
    TestContradictedIdentityStopsBeingReusable();
    TestCredentialsAreElided();
    TestEmptyIdentifierIsNotReusable();
    return usdassettest::Report("httpResolver identity");
}
