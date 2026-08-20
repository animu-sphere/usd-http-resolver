// SPDX-License-Identifier: Apache-2.0

#include "usdAssetCache/CacheKey.h"

#include <functional>

namespace usdasset {
namespace cache {

namespace {

/// The 64-bit mixing constant of FNV-1a, used only to combine two hashes. The
/// key is never persisted and never leaves the process, so this needs to spread
/// bits and nothing else; it is not a checksum and must never become one.
void HashCombine(std::size_t& seed, std::size_t value) noexcept {
    seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2);
}

}  // namespace

bool operator==(const AssetIdentity& lhs, const AssetIdentity& rhs) noexcept {
    return lhs.blockSize == rhs.blockSize &&
           lhs.resolvedIdentifier == rhs.resolvedIdentifier &&
           lhs.validator == rhs.validator;
}

bool operator!=(const AssetIdentity& lhs, const AssetIdentity& rhs) noexcept {
    return !(lhs == rhs);
}

std::size_t HashAssetIdentity(const AssetIdentity& identity) noexcept {
    std::size_t seed = std::hash<std::string>()(identity.resolvedIdentifier);
    // The validator is hashed as bytes, like every other use of it at this
    // layer. Nothing here knows whether it is an ETag, a date, or a synthesized
    // triple, and the moment it did this module would have become an HTTP
    // cache (CACHE.md §6).
    HashCombine(seed, std::hash<std::string>()(identity.validator));
    HashCombine(seed, std::hash<std::uint64_t>()(identity.blockSize));
    return seed;
}

bool operator==(const CacheKey& lhs, const CacheKey& rhs) noexcept {
    return lhs.blockIndex == rhs.blockIndex && lhs.identity == rhs.identity;
}

bool operator!=(const CacheKey& lhs, const CacheKey& rhs) noexcept {
    return !(lhs == rhs);
}

std::size_t HashCacheKey(const CacheKey& key) noexcept {
    std::size_t seed = HashAssetIdentity(key.identity);
    HashCombine(seed, std::hash<std::uint64_t>()(key.blockIndex));
    return seed;
}

bool IsShareable(const Validator& validator) noexcept {
    return validator.IsUsable() && validator.strength == ValidatorStrength::Strong;
}

}  // namespace cache
}  // namespace usdasset
