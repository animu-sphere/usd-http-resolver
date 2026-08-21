// SPDX-License-Identifier: Apache-2.0
//
// Cache identity.
//
//   CacheKey = resolvedIdentifier + validator + blockSize + blockIndex
//
// The rule the key exists to enforce is one line of CACHE.md §6, and it is the
// whole point of the module:
//
//   equal identifiers never imply equal content
//
// A URL match alone is never a hit. Two revisions published at one URL are two
// cache identities, and an entry from revision A must never serve a read of
// revision B.
//
// The validator is an opaque byte string here and nothing more. This layer
// never parses it, never compares it to an ETag, and never infers recency from
// it; it reads exactly one other field of the validator, `strength`, and
// exactly once -- to decide whether an entry may be shared with a reader that
// is not the one that stored it. Every other validator question belongs to the
// backend (ASSET_READER.md §7.1).
//
// Normative contract: docs/architecture/CACHE.md §6.

#ifndef USDASSETCACHE_CACHEKEY_H
#define USDASSETCACHE_CACHEKEY_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "usdAssetIo/Validator.h"

namespace usdasset {
namespace cache {

/// The part of the key that is constant for one reader: everything except the
/// block index.
///
/// Split out because it is what the store interns. A key carrying two strings
/// per cached block would spend more memory on identity than on bytes for a
/// small block size, and would hash two strings on every lookup; interning
/// turns the per-block key into two integers and leaves this struct as the
/// thing the contract is stated over.
struct AssetIdentity {
    /// The normalized absolute URI after redirects, per RESOLVER.md.
    std::string resolvedIdentifier;

    /// `Validator::value`, treated here as bytes. Empty is legal and means the
    /// backend captured nothing usable; see `IsShareable`.
    std::string validator;

    /// Part of the identity, not a parameter beside it: blocks stored under one
    /// block size cannot answer a lookup made under another.
    std::uint64_t blockSize = 0;
};

bool operator==(const AssetIdentity& lhs, const AssetIdentity& rhs) noexcept;
bool operator!=(const AssetIdentity& lhs, const AssetIdentity& rhs) noexcept;

std::size_t HashAssetIdentity(const AssetIdentity& identity) noexcept;

/// The key itself, as CACHE.md §6 states it.
struct CacheKey {
    AssetIdentity identity;
    std::uint64_t blockIndex = 0;
};

bool operator==(const CacheKey& lhs, const CacheKey& rhs) noexcept;
bool operator!=(const CacheKey& lhs, const CacheKey& rhs) noexcept;

std::size_t HashCacheKey(const CacheKey& key) noexcept;

/// Whether an entry stored under this validator may be served to a reader that
/// is not the one that stored it.
///
/// Only a strong validator admits it. The argument is the one CACHE.md §8 makes
/// about persistence, applied one level earlier: a weak validator cannot prove
/// two responses are byte-identical -- that is what weak means -- so two readers
/// that opened the same URL and got the same weak token may be holding two
/// different files, and serving one's blocks to the other composes exactly the
/// byte sequence §2.1 of ASSET_READER.md exists to prevent.
///
/// Within one reader the binding carries the guarantee whatever the strength
/// is, which is why a weak or absent validator still caches -- privately, for
/// that reader's lifetime, and dropped when it closes.
bool IsShareable(const Validator& validator) noexcept;

}  // namespace cache
}  // namespace usdasset

#endif  // USDASSETCACHE_CACHEKEY_H
