// SPDX-License-Identifier: Apache-2.0

#include "Identity.h"

#include "usdAssetIo/Diagnostics.h"
#include "usdAssetIo/Validator.h"

namespace usdhttpresolver {

PublishedIdentity PublishIdentity(const usdasset::AssetMetadata& metadata,
                                  bool contradicted) {
    PublishedIdentity published;
    published.resolvedIdentifier =
        usdasset::ElideSecrets(metadata.resolvedIdentifier);
    published.size = metadata.size;

    // Read from the validator, and the only thing read from it. Nothing here
    // parses an `ETag`, compares two values for recency, or looks at `kind`:
    // that is the transport's business, and the moment this file knows what a
    // `W/` prefix means the boundary in ASSET_READER.md §7.1 has moved.
    if (metadata.validator.IsUsable()) {
        published.validationToken = metadata.validator.value;
    }

    usdasset::IdentityStability stability = metadata.stability;

    // A validator that contradicted itself in this process cannot key anything
    // beyond the open that captured it, whatever strength it claims.
    if (contradicted && stability == usdasset::IdentityStability::Stable) {
        stability = usdasset::IdentityStability::Unstable;
    }

    published.stability = usdasset::IdentityStabilityName(stability);

    // A token with no identity to attach it to is not an identity. The metadata
    // a backend returns should never be in that state -- a usable validator is
    // what `Stable` is derived from -- and resolving the disagreement toward the
    // weaker answer is the only safe direction, which is the same rule
    // `ClassifyStability` applies one layer down.
    published.reusable = stability == usdasset::IdentityStability::Stable &&
                         !published.validationToken.empty() &&
                         !published.resolvedIdentifier.empty();
    if (!published.reusable &&
        stability == usdasset::IdentityStability::Stable) {
        published.stability =
            usdasset::IdentityStabilityName(
                usdasset::IdentityStability::Unavailable);
    }

    return published;
}

}  // namespace usdhttpresolver
