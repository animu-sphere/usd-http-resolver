// SPDX-License-Identifier: Apache-2.0
//
// SHA-256, FIPS 180-4, for the persistent tier's entry identity.
//
// Written out rather than taken from a library because this module takes no
// third-party dependency: ADR-0003 admits exactly one for the whole project and
// it is an HTTP client, which a block cache may not name. Nothing here is
// secret and nothing is authenticated. What is wanted is a digest whose
// collisions cannot be produced on purpose, so that a cache entry's identity
// can be *checked* without the identity being written down -- and it may not be
// written down, because a resolved identifier can be a signed URL and gate 7 of
// docs/releases/README.md forbids one in a persisted artifact.
//
// Internal to the module. It is in `src/` rather than in an installed header
// for the reason `BlockPlan.h` is: a hash function in the public surface is a
// hash function somebody depends on, and this one exists to serve one file
// format. It is tested directly, against the published vectors, because a
// digest that is subtly not SHA-256 is still a perfectly consistent function
// and would never fail a round trip.

#ifndef USDASSETCACHE_SHA256_H
#define USDASSETCACHE_SHA256_H

#include <cstddef>
#include <cstdint>

namespace usdasset {
namespace cache {
namespace detail {

class Sha256 {
public:
    static constexpr std::size_t kDigestBytes = 32;

    void Update(const void* data, std::size_t length) noexcept {
        const unsigned char* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < length; ++i) {
            _buffer[_pending++] = bytes[i];
            if (_pending == 64) {
                Compress(_buffer);
                _pending = 0;
            }
        }
        _length += static_cast<std::uint64_t>(length) * 8;
    }

    /// Writes `kDigestBytes`. Single use: the state is consumed.
    void Finish(unsigned char* out) noexcept {
        const std::uint64_t bits = _length;
        const unsigned char one = 0x80;
        Update(&one, 1);
        const unsigned char zero = 0;
        while (_pending != 56) {
            Update(&zero, 1);
        }
        for (int i = 7; i >= 0; --i) {
            _buffer[_pending++] = static_cast<unsigned char>((bits >> (8 * i)) & 0xFF);
        }
        Compress(_buffer);
        for (int i = 0; i < 8; ++i) {
            out[4 * i + 0] = static_cast<unsigned char>((_state[i] >> 24) & 0xFF);
            out[4 * i + 1] = static_cast<unsigned char>((_state[i] >> 16) & 0xFF);
            out[4 * i + 2] = static_cast<unsigned char>((_state[i] >> 8) & 0xFF);
            out[4 * i + 3] = static_cast<unsigned char>(_state[i] & 0xFF);
        }
    }

private:
    static std::uint32_t Rotr(std::uint32_t value, int bits) noexcept {
        return (value >> bits) | (value << (32 - bits));
    }

    void Compress(const unsigned char* block) noexcept {
        static const std::uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
            0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
            0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
            0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
            0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
            0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
            0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
            0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
            0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24) |
                   (static_cast<std::uint32_t>(block[4 * i + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[4 * i + 2]) << 8) |
                   static_cast<std::uint32_t>(block[4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 =
                Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = _state[0], b = _state[1], c = _state[2], d = _state[3];
        std::uint32_t e = _state[4], f = _state[5], g = _state[6], h = _state[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        _state[0] += a;
        _state[1] += b;
        _state[2] += c;
        _state[3] += d;
        _state[4] += e;
        _state[5] += f;
        _state[6] += g;
        _state[7] += h;
    }

    std::uint32_t _state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                               0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    unsigned char _buffer[64] = {};
    std::size_t _pending = 0;
    std::uint64_t _length = 0;
};

}  // namespace detail
}  // namespace cache
}  // namespace usdasset

#endif  // USDASSETCACHE_SHA256_H
