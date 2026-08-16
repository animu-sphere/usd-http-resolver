// SPDX-License-Identifier: Apache-2.0

#include "usdAssetIo/Validator.h"

#include <string>

#include "Check.h"

using namespace usdasset;

namespace {

Validator Make(std::string value, ValidatorKind kind, ValidatorStrength strength) {
    Validator validator;
    validator.value = std::move(value);
    validator.kind = kind;
    validator.strength = strength;
    return validator;
}

void DefaultValidatorIsUnusable() {
    const Validator validator;
    CHECK(!validator.IsUsable());
    CHECK(ClassifyStability(validator) == IdentityStability::Unavailable);
}

void ClassificationFollowsStrength() {
    CHECK(ClassifyStability(Make("\"abc\"", ValidatorKind::EntityTag,
                                 ValidatorStrength::Strong)) ==
          IdentityStability::Stable);
    CHECK(ClassifyStability(Make("W/\"abc\"", ValidatorKind::EntityTag,
                                 ValidatorStrength::Weak)) ==
          IdentityStability::Unstable);
    // Last-Modified cannot distinguish two revisions published inside one
    // second, which is exactly why it is Unstable and not Stable.
    CHECK(ClassifyStability(Make("Tue, 16 Aug 2026 10:00:00 GMT",
                                 ValidatorKind::HttpDate,
                                 ValidatorStrength::Weak)) ==
          IdentityStability::Unstable);
    CHECK(ClassifyStability(Make("", ValidatorKind::None,
                                 ValidatorStrength::None)) ==
          IdentityStability::Unavailable);
}

void StrengthWithoutKindResolvesTowardTheWeakerAnswer() {
    // A producer bug. Trusting the strength here would hand a consumer a
    // Stable identity built out of nothing.
    const Validator broken = Make("anything", ValidatorKind::None,
                                  ValidatorStrength::Strong);
    CHECK(ClassifyStability(broken) == IdentityStability::Unavailable);
}

void EqualityIsOverTheWholeIdentity() {
    const Validator a = Make("v1", ValidatorKind::Derived, ValidatorStrength::Strong);
    CHECK(a == Make("v1", ValidatorKind::Derived, ValidatorStrength::Strong));
    CHECK(a != Make("v2", ValidatorKind::Derived, ValidatorStrength::Strong));
    // Two backends can produce the same byte string and mean different things
    // by it, so kind participates.
    CHECK(a != Make("v1", ValidatorKind::EntityTag, ValidatorStrength::Strong));
    CHECK(a != Make("v1", ValidatorKind::Derived, ValidatorStrength::Weak));
}

void NamesAreStable() {
    CHECK(std::string(ValidatorKindName(ValidatorKind::EntityTag)) == "EntityTag");
    CHECK(std::string(ValidatorStrengthName(ValidatorStrength::Strong)) == "Strong");
    CHECK(std::string(IdentityStabilityName(IdentityStability::Unavailable)) ==
          "Unavailable");
}

}  // namespace

int main() {
    DefaultValidatorIsUnusable();
    ClassificationFollowsStrength();
    StrengthWithoutKindResolvesTowardTheWeakerAnswer();
    EqualityIsOverTheWholeIdentity();
    NamesAreStable();
    return usdassettest::Report("usdAssetIo validator");
}
