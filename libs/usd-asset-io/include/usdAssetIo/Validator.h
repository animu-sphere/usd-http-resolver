// SPDX-License-Identifier: Apache-2.0
//
// Validator value types: the transport-derived identity of the exact bytes an
// asset had when it was opened.
//
// Normative contract: docs/architecture/ASSET_READER.md §7.

#ifndef USDASSETIO_VALIDATOR_H
#define USDASSETIO_VALIDATOR_H

#include <string>

namespace usdasset {

/// What kind of identity the producing backend captured.
///
/// `kind` and `strength` exist so that a backend does not re-derive on every
/// request what it already learned at open. They are transport-level metadata:
/// only the backend that produced them interprets them. Nothing above the cache
/// reads either field.
enum class ValidatorKind {
    None,
    EntityTag,  ///< HTTP ETag.
    HttpDate,   ///< HTTP Last-Modified.
    Derived,    ///< Synthesized by the backend, e.g. size + mtime + file id.
};

/// What equality of two validator values proves.
enum class ValidatorStrength {
    None,
    Weak,    ///< Equal values imply equivalent, not identical, bytes.
    Strong,  ///< Equal values imply byte-identical content.
};

/// The identity itself. `value` is an opaque byte string everywhere above the
/// backend that produced it: no layer above compares it to an ETag, parses a
/// date out of it, or infers recency from it.
struct Validator {
    std::string value;
    ValidatorKind kind = ValidatorKind::None;
    ValidatorStrength strength = ValidatorStrength::None;

    bool IsUsable() const noexcept { return kind != ValidatorKind::None; }
};

/// Byte equality of the identity, used to detect a revision change. This is the
/// only comparison any layer performs on a validator.
bool operator==(const Validator& lhs, const Validator& rhs) noexcept;
bool operator!=(const Validator& lhs, const Validator& rhs) noexcept;

/// The consumer-facing summary of a validator. This, and never the struct, is
/// what crosses the resolver boundary.
enum class IdentityStability {
    Stable,       ///< strength == Strong.
    Unstable,     ///< strength == Weak.
    Unavailable,  ///< strength == None, or no usable validator exists.
};

/// Derives stability from a captured validator, once, at open.
///
/// An unusable validator is `Unavailable` whatever strength it claims: a
/// strength without a kind is a producer bug, and resolving it toward the
/// weaker answer is the only safe direction.
IdentityStability ClassifyStability(const Validator& validator) noexcept;

const char* ValidatorKindName(ValidatorKind kind) noexcept;
const char* ValidatorStrengthName(ValidatorStrength strength) noexcept;
const char* IdentityStabilityName(IdentityStability stability) noexcept;

}  // namespace usdasset

#endif  // USDASSETIO_VALIDATOR_H
