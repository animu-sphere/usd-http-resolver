// SPDX-License-Identifier: Apache-2.0
//
// The consumer-facing projection of an asset's identity: RESOLVER.md §3.
//
// What crosses this boundary is four neutral values -- a resolved identifier, a
// size, an opaque validation token, and a stability class -- and never a
// `Validator`. §7.1 of ASSET_READER.md keeps `kind` and `strength` below the
// resolver, so this file reads `stability`, which is the projection of strength
// that is allowed out, and `value`, which is opaque here and stays opaque all
// the way to the consumer.
//
// No OpenUSD header, for the reason `Identifier.h` gives: deciding *what* may
// be published is arithmetic over a metadata struct, the mistakes it can make
// are invisible from the outside -- a token published for a revision that
// cannot prove it is one -- and a test for it should not need a USD runtime.
// `HttpResolver.cpp` turns the result into an `ArAssetInfo`; nothing else does.

#ifndef USDHTTPRESOLVER_IDENTITY_H
#define USDHTTPRESOLVER_IDENTITY_H

#include <cstdint>
#include <string>

#include "usdAssetIo/AssetReader.h"

namespace usdhttpresolver {

/// One asset's identity, in the form a consumer reads it.
///
/// Every field is safe to log: the identifier has been through
/// `usdasset::ElideSecrets`, and the token is a validator, which is a public
/// property of a response rather than a credential.
struct PublishedIdentity {
    /// The normalized absolute URI, after redirects, with everything that can
    /// carry a credential elided.
    ///
    /// Elision costs a distinction: two assets that differ only in their query
    /// strings elide to one string. That is why this is not, on its own, a
    /// cache key -- the token is the identity, and a consumer that keys on a URL
    /// keys on the resolved path it already holds. RESOLVER.md §3 says so next
    /// to the field list.
    std::string resolvedIdentifier;

    /// Byte size at open.
    std::uint64_t size = 0;

    /// The backend's captured validator value, verbatim and opaque. Empty when
    /// the backend captured no usable validator.
    ///
    /// Published for a weak validator too, because it is still positive
    /// evidence of change (ASSET_READER.md §7.2) and because it never travels
    /// without `stability` beside it saying what it is worth.
    std::string validationToken;

    /// `Stable`, `Unstable`, or `Unavailable`, as
    /// `usdasset::IdentityStabilityName` spells them.
    std::string stability;

    /// Whether this identity may key something that outlives the open.
    ///
    /// True only for a strong validator that this process has never seen
    /// contradicted at this identifier. It is the one field with a rule rather
    /// than a value, and it exists because `ArAssetInfo::version` travels with
    /// no stability beside it: a consumer that finds a token there treats the
    /// identity as reusable, so a token that is not reusable must not be put in
    /// it. See `HttpResolver::_GetAssetInfo`.
    bool reusable = false;
};

/// Projects `metadata` for publication.
///
/// `contradicted` says that this process has seen two different validators at
/// this identifier -- the asset was republished underneath it. `ArAssetInfo` is
/// keyed by path and not by open asset, so a consumer holding the earlier
/// revision would be handed the later revision's token and would file bytes
/// from A under the identity of B. There is no version of that surface that can
/// distinguish them, so a contradicted identifier stops publishing a reusable
/// identity for the rest of the process: `Stable` degrades to `Unstable`, and
/// the token stays visible as evidence of change while `reusable` goes false.
PublishedIdentity PublishIdentity(const usdasset::AssetMetadata& metadata,
                                  bool contradicted);

}  // namespace usdhttpresolver

#endif  // USDHTTPRESOLVER_IDENTITY_H
