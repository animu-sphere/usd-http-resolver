// SPDX-License-Identifier: Apache-2.0
//
// HTTP response framing: the headers a range backend is required to read, and
// the rules for reading them.
//
// This is the "transport, not content" exception in §3.5 of the design policy
// -- status codes, `Content-Range`, `Content-Length`, `Accept-Ranges`, and
// validators. It is separated from the reader because it is pure function of a
// header table, which makes it testable without a request, and because §10 of
// the design policy treats every one of these values as untrusted input from a
// remote server.
//
// Internal to usdAssetHttp. No header outside src/ includes this.

#ifndef USDASSETHTTP_FRAMING_H
#define USDASSETHTTP_FRAMING_H

#include <cstdint>
#include <string>
#include <string_view>

#include "usdAssetIo/Validator.h"
#include "Transport.h"

namespace usdasset {
namespace http {

/// A parsed `Content-Range: bytes <first>-<last>/<complete-length>`.
struct ContentRange {
    /// The `bytes */<complete-length>` form, which is what a `416` carries.
    bool unsatisfied = false;
    std::uint64_t first = 0;
    std::uint64_t last = 0;  ///< Inclusive, as the header is.
    bool hasCompleteLength = false;
    std::uint64_t completeLength = 0;

    /// Only meaningful when `!unsatisfied`.
    std::uint64_t Length() const noexcept { return last - first + 1; }
};

/// Parses a `Content-Range` value. Returns false for anything that is not
/// exactly the grammar in RFC 9110 §14.4 -- including `first > last`, which is
/// syntactically well-formed and semantically impossible.
///
/// Only `bytes` is accepted. A response in some other range unit is not a
/// response to a request this backend issued.
bool ParseContentRange(std::string_view value, ContentRange* out);

/// Parses a `Content-Length` value. Returns false on anything that is not a
/// single run of digits, which includes the duplicated-value form a request
/// smuggling attempt uses.
bool ParseContentLength(std::string_view value, std::uint64_t* out);

/// Whether the response advertises byte ranges.
///
/// Absence is false, not unknown. §4.2 of the design policy makes range support
/// a capability discovered from `Accept-Ranges` and from the actual response
/// status, and the corpus's `NoAcceptRanges` row -- "the honest server that
/// simply cannot do this" -- is the absent case. Treating absence as "probably
/// fine" is how a 10 GB download starts.
bool AdvertisesByteRanges(const HeaderTable& headers);

/// What a metadata response yielded about the asset's identity.
///
/// Two fields rather than one because the reader contract asks two different
/// questions of a validator (ASSET_READER.md §7.2): what identity to bind to,
/// and what may legally go in an `If-Range`. A weak `ETag` answers the first
/// and not the second, so a design with one field either sends a header RFC
/// 9110 forbids or throws away identity it was entitled to keep.
struct CapturedValidator {
    Validator validator;

    /// The `If-Range` value to send on every subsequent range request, or empty
    /// when no header may be sent. Empty for a weak `ETag`: RFC 9110 §13.1.5
    /// admits only a strong validator there, because the whole point of the
    /// header is that the two halves of a partial response are byte-identical.
    std::string ifRangeValue;
};

/// Captures identity from a response's headers, preferring in the order the
/// contract's table ranks them: a strong `ETag`, then a weak one, then
/// `Last-Modified`, then nothing.
CapturedValidator CaptureValidator(const HeaderTable& headers);

/// Whether a later response contradicts the identity captured at open.
///
/// Returns false only on positive evidence of a change: a response that carries
/// a validator of the captured kind, whose value differs. A response that
/// carries no validator at all proves nothing and is not treated as a change --
/// the guarantee is enforced by `If-Range` and by this check together, and
/// inventing a change out of a missing header would fail every read against a
/// server that simply stopped repeating itself.
bool ContradictsCapturedValidator(const HeaderTable& headers,
                                  const Validator& captured);

/// `bytes=<offset>-<offset + length - 1>`. `length` must be non-zero: there is
/// no byte-range spelling for an empty range, and the read contract issues no
/// request for one.
std::string FormatByteRange(std::uint64_t offset, std::uint64_t length);

}  // namespace http
}  // namespace usdasset

#endif  // USDASSETHTTP_FRAMING_H
