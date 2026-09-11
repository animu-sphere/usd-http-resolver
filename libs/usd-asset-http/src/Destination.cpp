// SPDX-License-Identifier: Apache-2.0

#include "Destination.h"

#include <cstddef>

namespace usdasset {
namespace http {
namespace {

constexpr std::size_t kNpos = std::string_view::npos;

int HexValue(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Parses colon-separated hexadecimal groups into `groups`, returning how many
/// sixteen-bit groups were written, or -1.
///
/// A dotted quad is admitted as the final part when `allowTrailingIPv4` is set,
/// and counts as two groups. It is admitted nowhere else: `1.2.3.4::` is not an
/// address, and a parser that read it as one would be classifying a string no
/// resolver connects to.
int ParseGroups(std::string_view text, std::uint16_t* groups, int capacity,
                bool allowTrailingIPv4) noexcept {
    if (text.empty()) return 0;

    int count = 0;
    std::size_t at = 0;
    for (;;) {
        const std::size_t colon = text.find(':', at);
        const std::string_view part =
            colon == kNpos ? text.substr(at) : text.substr(at, colon - at);

        if (part.find('.') != kNpos) {
            if (!allowTrailingIPv4 || colon != kNpos) return -1;
            std::array<std::uint8_t, 4> v4{};
            if (!ParseIPv4(part, &v4) || count + 2 > capacity) return -1;
            groups[count++] = static_cast<std::uint16_t>((v4[0] << 8) | v4[1]);
            groups[count++] = static_cast<std::uint16_t>((v4[2] << 8) | v4[3]);
            return count;
        }

        // An empty part is a stray colon -- `:1::2`, `1:2:` -- since the one
        // legal pair of adjacent colons was split off by the caller.
        if (part.empty() || part.size() > 4 || count + 1 > capacity) return -1;
        unsigned value = 0;
        for (const char c : part) {
            const int digit = HexValue(c);
            if (digit < 0) return -1;
            value = value * 16 + static_cast<unsigned>(digit);
        }
        groups[count++] = static_cast<std::uint16_t>(value);

        if (colon == kNpos) return count;
        at = colon + 1;
    }
}

std::array<std::uint8_t, 4> TrailingIPv4(const std::array<std::uint8_t, 16>& address) noexcept {
    return {address[12], address[13], address[14], address[15]};
}

bool AllZero(const std::array<std::uint8_t, 16>& address, std::size_t first,
             std::size_t last) noexcept {
    for (std::size_t i = first; i < last; ++i) {
        if (address[i] != 0) return false;
    }
    return true;
}

}  // namespace

const char* AddressClassName(AddressClass addressClass) noexcept {
    switch (addressClass) {
        case AddressClass::Public: return "public";
        case AddressClass::Private: return "private";
        case AddressClass::Loopback: return "loopback";
        case AddressClass::LinkLocal: return "link-local";
    }
    return "unknown";
}

bool DestinationPolicy::Permits(AddressClass addressClass) const noexcept {
    switch (addressClass) {
        case AddressClass::Public: return publicAddresses;
        case AddressClass::Private: return privateNetworks;
        case AddressClass::Loopback: return loopback;
        case AddressClass::LinkLocal: return linkLocal;
    }
    return false;
}

AddressClass ClassifyIPv4(const std::array<std::uint8_t, 4>& address) noexcept {
    // 0.0.0.0/8 is "this host on this network". A connect to 0.0.0.0 reaches
    // the local machine on Linux and macOS alike, so it is loopback for the
    // purpose this classification serves, whatever the registry calls it.
    if (address[0] == 127 || address[0] == 0) return AddressClass::Loopback;
    if (address[0] == 169 && address[1] == 254) return AddressClass::LinkLocal;
    if (address[0] == 10) return AddressClass::Private;
    if (address[0] == 172 && (address[1] & 0xf0) == 0x10) return AddressClass::Private;
    if (address[0] == 192 && address[1] == 168) return AddressClass::Private;
    // RFC 6598's shared address space. Carrier-grade NAT, and in practice the
    // inside of a good many corporate and cloud networks: private for every
    // purpose a request-forgery policy has.
    if (address[0] == 100 && (address[1] & 0xc0) == 0x40) return AddressClass::Private;
    return AddressClass::Public;
}

AddressClass ClassifyIPv6(const std::array<std::uint8_t, 16>& address) noexcept {
    if (AllZero(address, 0, 12)) {
        // `::` and `::1`. The unspecified address reaches this host for the
        // same reason 0.0.0.0 does.
        if (address[12] == 0 && address[13] == 0 && address[14] == 0 &&
            (address[15] == 0 || address[15] == 1)) {
            return AddressClass::Loopback;
        }
        // IPv4-compatible, `::a.b.c.d`. Deprecated, and still an address a
        // stack may route to the IPv4 one it spells.
        return ClassifyIPv4(TrailingIPv4(address));
    }
    // IPv4-mapped, `::ffff:a.b.c.d`: a connection to it *is* a connection to
    // the IPv4 address, on every dual-stack socket. Classifying it by its IPv6
    // prefix would let `[::ffff:169.254.169.254]` walk past a policy that
    // refuses 169.254.169.254.
    if (AllZero(address, 0, 10) && address[10] == 0xff && address[11] == 0xff) {
        return ClassifyIPv4(TrailingIPv4(address));
    }
    // The NAT64 well-known prefix, 64:ff9b::/96, where a translator forwards
    // to the IPv4 address in the low bits.
    if (address[0] == 0x00 && address[1] == 0x64 && address[2] == 0xff &&
        address[3] == 0x9b && AllZero(address, 4, 12)) {
        return ClassifyIPv4(TrailingIPv4(address));
    }
    if (address[0] == 0xfe && (address[1] & 0xc0) == 0x80) return AddressClass::LinkLocal;
    if (address[0] == 0xfe && (address[1] & 0xc0) == 0xc0) return AddressClass::Private;
    if ((address[0] & 0xfe) == 0xfc) return AddressClass::Private;
    return AddressClass::Public;
}

bool ParseIPv4(std::string_view text, std::array<std::uint8_t, 4>* out) noexcept {
    std::array<std::uint8_t, 4> bytes{};
    std::size_t at = 0;
    for (std::size_t part = 0; part < 4; ++part) {
        if (part > 0) {
            if (at >= text.size() || text[at] != '.') return false;
            ++at;
        }
        const std::size_t start = at;
        unsigned value = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            value = value * 10 + static_cast<unsigned>(text[at] - '0');
            // Checked per digit, so that a long run of digits cannot overflow
            // `value` before the length check below sees it.
            if (value > 255) return false;
            ++at;
        }
        const std::size_t digits = at - start;
        if (digits == 0 || digits > 3) return false;
        if (digits > 1 && text[start] == '0') return false;
        bytes[part] = static_cast<std::uint8_t>(value);
    }
    if (at != text.size()) return false;
    *out = bytes;
    return true;
}

bool ParseIPv6(std::string_view text, std::array<std::uint8_t, 16>* out) noexcept {
    std::uint16_t head[8] = {};
    std::uint16_t tail[8] = {};
    int headCount = 0;
    int tailCount = 0;

    const std::size_t gap = text.find("::");
    if (gap == kNpos) {
        headCount = ParseGroups(text, head, 8, true);
        if (headCount != 8) return false;
    } else {
        // One `::` at most. A second -- including the overlapping one in
        // `:::` -- would make the number of zero groups ambiguous.
        if (text.find("::", gap + 1) != kNpos) return false;
        headCount = ParseGroups(text.substr(0, gap), head, 8, false);
        tailCount = ParseGroups(text.substr(gap + 2), tail, 8, true);
        if (headCount < 0 || tailCount < 0) return false;
        // The gap stands for at least one group of zeros.
        if (headCount + tailCount > 7) return false;
    }

    std::uint16_t groups[8] = {};
    for (int i = 0; i < headCount; ++i) groups[i] = head[i];
    for (int i = 0; i < tailCount; ++i) groups[8 - tailCount + i] = tail[i];

    std::array<std::uint8_t, 16> bytes{};
    for (int i = 0; i < 8; ++i) {
        bytes[2 * i] = static_cast<std::uint8_t>(groups[i] >> 8);
        bytes[2 * i + 1] = static_cast<std::uint8_t>(groups[i] & 0xff);
    }
    *out = bytes;
    return true;
}

bool ClassifyHostLiteral(std::string_view host, AddressClass* out) noexcept {
    if (!host.empty() && host.front() == '[') {
        if (host.size() < 2 || host.back() != ']') return false;
        std::string_view inner = host.substr(1, host.size() - 2);
        // RFC 6874 writes a zone as `%25` followed by its name. The zone
        // chooses an interface and not an address, so it plays no part in the
        // class; a bare `%` is taken the same way, since that is how a system
        // resolver reads it.
        const std::size_t zone = inner.find('%');
        if (zone != kNpos) inner = inner.substr(0, zone);

        std::array<std::uint8_t, 16> v6{};
        if (!ParseIPv6(inner, &v6)) return false;
        *out = ClassifyIPv6(v6);
        return true;
    }

    std::array<std::uint8_t, 4> v4{};
    if (!ParseIPv4(host, &v4)) return false;
    *out = ClassifyIPv4(v4);
    return true;
}

}  // namespace http
}  // namespace usdasset
