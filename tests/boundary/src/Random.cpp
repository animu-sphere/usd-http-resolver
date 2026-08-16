// SPDX-License-Identifier: Apache-2.0

#include "usdassetboundary/Random.h"

#include <limits>

#include "usdassetboundary/Fixture.h"

namespace usdassetboundary {
namespace {

constexpr std::uint64_t kMax = (std::numeric_limits<std::uint64_t>::max)();

/// Sizes stay small on purpose. The suite allocates a buffer per case, and the
/// bugs it hunts live at the ends of a range rather than in its middle: a
/// 64 MB read exercises the same two boundaries a 64 KB read does, at a
/// thousand times the cost.
std::size_t GenerateSize(Generator& generator) {
    if (generator.Chance(15)) {
        return 0;
    }
    if (generator.Chance(20)) {
        return 1;
    }
    if (generator.Chance(30)) {
        const std::uint64_t block = kNominalBlockSize;
        const std::uint64_t choice = generator.Below(3);
        return static_cast<std::size_t>(block + choice - 1);  // block-1, block, block+1
    }
    return static_cast<std::size_t>(generator.Below(kNominalBlockSize * 2) + 1);
}

std::uint64_t Perturb(Generator& generator, std::uint64_t value) {
    switch (generator.Below(3)) {
        case 0: return value == 0 ? 0 : value - 1;
        case 1: return value;
        default: return value + 1;
    }
}

}  // namespace

std::uint64_t Generator::Next() {
    _state += 0x9E3779B97F4A7C15ull;
    std::uint64_t mixed = _state;
    mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
    mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
    return mixed ^ (mixed >> 31);
}

std::uint64_t Generator::Below(std::uint64_t bound) {
    if (bound == 0) {
        return 0;
    }
    return Next() % bound;
}

bool Generator::Chance(int percent) {
    return static_cast<int>(Below(100)) < percent;
}

ReadCase GenerateReadCase(Generator& generator, std::uint64_t assetSize) {
    ReadCase readCase;
    readCase.size = GenerateSize(generator);

    // Overflow. Generated deliberately rather than hoped for: the odds of a
    // uniform 64-bit offset landing within `size` of the top are nil.
    if (generator.Chance(8)) {
        // A zero-length request cannot overflow whatever the offset, so this
        // branch insists on a length. Generating one that quietly could not
        // overflow would leave the case untested while looking covered.
        if (readCase.size == 0) {
            readCase.size = 16;
        }
        const std::uint64_t slack = generator.Below(4);
        readCase.offset =
            kMax - static_cast<std::uint64_t>(readCase.size) + 1 + slack;
        return readCase;
    }

    const std::uint64_t roll = generator.Below(100);
    if (roll < 12) {
        readCase.offset = 0;
    } else if (roll < 32) {
        // The three offsets around the end of the asset, which is where a
        // range backend is most often wrong by one.
        readCase.offset = Perturb(generator, assetSize);
    } else if (roll < 62) {
        // A block-aligned offset, and its neighbours.
        const std::uint64_t blocks = assetSize / kNominalBlockSize + 2;
        readCase.offset = Perturb(generator, generator.Below(blocks) * kNominalBlockSize);
    } else if (roll < 72) {
        // Near the top of the 64-bit range, without overflowing.
        readCase.offset = kMax - generator.Below(kNominalBlockSize * 4) -
                          static_cast<std::uint64_t>(readCase.size);
    } else if (assetSize > 0) {
        readCase.offset = generator.Below(assetSize + kNominalBlockSize);
    } else {
        readCase.offset = generator.Below(kNominalBlockSize);
    }

    return readCase;
}

}  // namespace usdassetboundary
