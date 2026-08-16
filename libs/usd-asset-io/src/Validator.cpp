// SPDX-License-Identifier: Apache-2.0

#include "usdAssetIo/Validator.h"

namespace usdasset {

bool operator==(const Validator& lhs, const Validator& rhs) noexcept {
    return lhs.kind == rhs.kind && lhs.strength == rhs.strength &&
           lhs.value == rhs.value;
}

bool operator!=(const Validator& lhs, const Validator& rhs) noexcept {
    return !(lhs == rhs);
}

IdentityStability ClassifyStability(const Validator& validator) noexcept {
    if (!validator.IsUsable()) {
        return IdentityStability::Unavailable;
    }
    switch (validator.strength) {
        case ValidatorStrength::Strong: return IdentityStability::Stable;
        case ValidatorStrength::Weak: return IdentityStability::Unstable;
        case ValidatorStrength::None: return IdentityStability::Unavailable;
    }
    return IdentityStability::Unavailable;
}

const char* ValidatorKindName(ValidatorKind kind) noexcept {
    switch (kind) {
        case ValidatorKind::None: return "None";
        case ValidatorKind::EntityTag: return "EntityTag";
        case ValidatorKind::HttpDate: return "HttpDate";
        case ValidatorKind::Derived: return "Derived";
    }
    return "<unknown>";
}

const char* ValidatorStrengthName(ValidatorStrength strength) noexcept {
    switch (strength) {
        case ValidatorStrength::None: return "None";
        case ValidatorStrength::Weak: return "Weak";
        case ValidatorStrength::Strong: return "Strong";
    }
    return "<unknown>";
}

const char* IdentityStabilityName(IdentityStability stability) noexcept {
    switch (stability) {
        case IdentityStability::Stable: return "Stable";
        case IdentityStability::Unstable: return "Unstable";
        case IdentityStability::Unavailable: return "Unavailable";
    }
    return "<unknown>";
}

}  // namespace usdasset
