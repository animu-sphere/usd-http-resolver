// SPDX-License-Identifier: Apache-2.0
//
// The biased generator behind the property cases.
//
// Generation is biased, not uniform. Uniform random offsets over a large asset
// almost never hit an interesting value: the boundaries where range readers
// actually break are a vanishing fraction of the space, so a uniform generator
// spends a thousand iterations proving that the middle of a file is easy.
//
// docs/contributing/BOUNDARY_SUITE.md §4.

#ifndef USDASSETBOUNDARY_RANDOM_H
#define USDASSETBOUNDARY_RANDOM_H

#include <cstddef>
#include <cstdint>

namespace usdassetboundary {

/// splitmix64. Deterministic, seedable, and small enough that a reported seed
/// reproduces a failure on another machine and another compiler.
class Generator {
public:
    explicit Generator(std::uint64_t seed) : _state(seed) {}

    std::uint64_t Next();

    /// Uniform in [0, bound). Zero when `bound` is zero.
    std::uint64_t Below(std::uint64_t bound);

    bool Chance(int percent);

private:
    std::uint64_t _state;
};

struct ReadCase {
    std::uint64_t offset = 0;
    std::size_t size = 0;
};

/// One case, weighted toward: zero; the three offsets around the end of the
/// asset; block-aligned offsets and their neighbours; offsets near the top of
/// the 64-bit range; and pairs chosen so that offset + size overflows.
ReadCase GenerateReadCase(Generator& generator, std::uint64_t assetSize);

}  // namespace usdassetboundary

#endif  // USDASSETBOUNDARY_RANDOM_H
