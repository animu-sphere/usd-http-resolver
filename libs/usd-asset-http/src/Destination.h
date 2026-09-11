// SPDX-License-Identifier: Apache-2.0
//
// Address classification for the destination policy: §10.2 of the design
// policy, and `DestinationPolicy` in the public header.
//
// Pure functions over bytes and text. No socket header, no resolver, and no
// libcurl: the transport hands this file the address it is about to connect to
// as sixteen or four bytes, and the protocol layer hands it a URI host. Keeping
// the arithmetic here is what makes it testable as a table and fuzzable as a
// parser (§11.6), and what lets a second transport reuse it without inheriting
// the first one's client.
//
// Internal to usdAssetHttp. No header outside src/ includes this.

#ifndef USDASSETHTTP_DESTINATION_H
#define USDASSETHTTP_DESTINATION_H

#include <array>
#include <cstdint>
#include <string_view>

#include "usdAssetHttp/HttpAssetReader.h"

namespace usdasset {
namespace http {

/// The class of an IPv4 address, in network byte order.
AddressClass ClassifyIPv4(const std::array<std::uint8_t, 4>& address) noexcept;

/// The class of an IPv6 address, in network byte order. An address that
/// carries an IPv4 one is the class of the address it carries.
AddressClass ClassifyIPv6(const std::array<std::uint8_t, 16>& address) noexcept;

/// Parses canonical dotted-quad IPv4 text: four decimal parts, each 0 to 255,
/// with no leading zeros and nothing else.
///
/// Strict on purpose, and the strictness is safe rather than merely tidy. The
/// legacy forms a system resolver also accepts -- `127.1`, `0x7f.0.0.1`,
/// `017700000001` -- are not literals here, so they reach the connect-time
/// check as names and are judged there by the address they resolve to. Reading
/// `010.0.0.1` as decimal would be worse than not reading it: a resolver that
/// honours the leading zero connects to 8.0.0.1, and a pre-flight that judged
/// 10.0.0.1 would be judging an address nobody connects to.
bool ParseIPv4(std::string_view text, std::array<std::uint8_t, 4>* out) noexcept;

/// Parses IPv6 text in RFC 4291 §2.2 form: hexadecimal groups, at most one
/// `::`, and an optional trailing dotted quad. No brackets and no zone.
bool ParseIPv6(std::string_view text, std::array<std::uint8_t, 16>* out) noexcept;

/// Classifies a URI host when it is a literal address: a dotted quad, or a
/// bracketed IPv6 literal with or without an RFC 6874 zone (`[fe80::1%25en0]`).
///
/// Returns false for a name, which is not a failure: a name is judged at
/// connect time, by the address it resolves to.
bool ClassifyHostLiteral(std::string_view host, AddressClass* out) noexcept;

}  // namespace http
}  // namespace usdasset

#endif  // USDASSETHTTP_DESTINATION_H
