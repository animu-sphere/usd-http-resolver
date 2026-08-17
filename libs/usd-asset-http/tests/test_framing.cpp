// SPDX-License-Identifier: Apache-2.0
//
// Response framing: the header values a range backend acts on, treated as the
// untrusted input §10 of the design policy says they are.
//
// Every rejection below is a header a server could send and a backend could
// believe. The interesting ones are not malformed: `bytes 0-999/500` parses
// cleanly and describes a range outside the representation it claims to be part
// of, and `bytes 100-50/500` is grammatical and impossible.

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

#include "Check.h"
#include "Framing.h"

namespace {

using usdasset::ValidatorKind;
using usdasset::ValidatorStrength;
using usdasset::http::AdvertisesByteRanges;
using usdasset::http::CaptureValidator;
using usdasset::http::CapturedValidator;
using usdasset::http::ContentRange;
using usdasset::http::ContradictsCapturedValidator;
using usdasset::http::FormatByteRange;
using usdasset::http::HeaderTable;
using usdasset::http::ParseContentLength;
using usdasset::http::ParseContentRange;

HeaderTable Headers(std::initializer_list<std::pair<const char*, const char*>> entries) {
    HeaderTable table;
    for (const auto& entry : entries) table.Add(entry.first, entry.second);
    return table;
}

void TestHeaderTable() {
    const HeaderTable table =
        Headers({{"Content-Length", "10"}, {"ETag", "\"a\""}});

    // Case-insensitive, because a header name is.
    CHECK(table.Find("content-length") != nullptr);
    CHECK(table.Find("CONTENT-LENGTH") != nullptr);
    CHECK_EQ(table.Get("Content-Length"), std::string("10"));
    CHECK(table.Find("Accept-Ranges") == nullptr);
    // Absence is a first-class answer here, not an empty string: an unknown
    // `Content-Length` is a corpus behavior and has to be distinguishable from
    // one that is present and zero.
    CHECK_EQ(table.Get("Accept-Ranges"), std::string());

    // Two lines that disagree is the request-smuggling shape RFC 9110 §8.6
    // requires a message to be rejected for. `Find` cannot express it -- it has
    // to return one value -- so it is a separate question.
    CHECK(Headers({{"Content-Length", "10"}, {"Content-Length", "20"}})
              .HasConflictingDuplicate("Content-Length"));
    CHECK(Headers({{"content-length", "10"}, {"Content-Length", "20"}})
              .HasConflictingDuplicate("Content-Length"));
    // Repeated identically is redundant and harmless. Refusing it would fail
    // against servers that merely repeat themselves.
    CHECK(!Headers({{"Content-Length", "10"}, {"Content-Length", "10"}})
               .HasConflictingDuplicate("Content-Length"));
    CHECK(!table.HasConflictingDuplicate("Content-Length"));
    CHECK(!table.HasConflictingDuplicate("Accept-Ranges"));
}

void TestContentRange() {
    ContentRange range;

    CHECK(ParseContentRange("bytes 0-255/1024", &range));
    CHECK_EQ(range.first, 0u);
    CHECK_EQ(range.last, 255u);
    CHECK_EQ(range.Length(), 256u);
    CHECK(range.hasCompleteLength);
    CHECK_EQ(range.completeLength, 1024u);
    CHECK(!range.unsatisfied);

    // The `416` form.
    CHECK(ParseContentRange("bytes */1024", &range));
    CHECK(range.unsatisfied);
    CHECK_EQ(range.completeLength, 1024u);

    // An unknown complete length is legal on a `206`.
    CHECK(ParseContentRange("bytes 0-9/*", &range));
    CHECK(!range.hasCompleteLength);

    CHECK(ParseContentRange("  bytes   0-9/100  ", &range));

    // Grammatical and impossible. Rejected here so that no caller has to
    // remember that `Length()` underflows on it.
    CHECK(!ParseContentRange("bytes 100-50/500", &range));
    // Describes bytes past the end of the representation it is part of.
    CHECK(!ParseContentRange("bytes 0-999/500", &range));
    CHECK(!ParseContentRange("bytes 500-500/500", &range));
    // An unsatisfied form without a length describes nothing.
    CHECK(!ParseContentRange("bytes */*", &range));
    // Some other range unit is not an answer to a request this backend issued.
    CHECK(!ParseContentRange("items 0-9/100", &range));
    CHECK(!ParseContentRange("bytes 0-9", &range));
    CHECK(!ParseContentRange("bytes 0/100", &range));
    CHECK(!ParseContentRange("", &range));
    CHECK(!ParseContentRange("bytes -9/100", &range));
    CHECK(!ParseContentRange("bytes 0-x/100", &range));
}

void TestContentLength() {
    std::uint64_t length = 0;
    CHECK(ParseContentLength("0", &length));
    CHECK_EQ(length, 0u);
    CHECK(ParseContentLength(" 4096 ", &length));
    CHECK_EQ(length, 4096u);

    // A length that wrapped is the one that turns an enormous hostile number
    // into a small plausible one.
    CHECK(!ParseContentLength("18446744073709551616", &length));
    // The duplicated-value form a request-smuggling attempt uses.
    CHECK(!ParseContentLength("10, 10", &length));
    CHECK(!ParseContentLength("-1", &length));
    CHECK(!ParseContentLength("0x10", &length));
    CHECK(!ParseContentLength("", &length));
}

void TestAcceptRanges() {
    CHECK(AdvertisesByteRanges(Headers({{"Accept-Ranges", "bytes"}})));
    CHECK(AdvertisesByteRanges(Headers({{"Accept-Ranges", "BYTES"}})));
    CHECK(AdvertisesByteRanges(Headers({{"Accept-Ranges", "none, bytes"}})));

    // Absence is false, not unknown: the corpus's `NoAcceptRanges` row is "the
    // honest server that simply cannot do this", and treating absence as
    // probably-fine is how a 10 GB download starts.
    CHECK(!AdvertisesByteRanges(Headers({{"Content-Length", "10"}})));
    CHECK(!AdvertisesByteRanges(Headers({{"Accept-Ranges", "none"}})));
    CHECK(!AdvertisesByteRanges(Headers({{"Accept-Ranges", ""}})));
}

void TestValidatorCapture() {
    {
        const CapturedValidator captured =
            CaptureValidator(Headers({{"ETag", "\"v1\""}}));
        CHECK_EQ(captured.validator.kind, ValidatorKind::EntityTag);
        CHECK_EQ(captured.validator.strength, ValidatorStrength::Strong);
        // Verbatim, quoting included: an origin compares `If-Range` as an
        // opaque byte string, so re-quoting produces a header that never
        // matches.
        CHECK_EQ(captured.validator.value, std::string("\"v1\""));
        CHECK_EQ(captured.ifRangeValue, std::string("\"v1\""));
    }
    {
        const CapturedValidator captured =
            CaptureValidator(Headers({{"ETag", "W/\"v1\""}}));
        CHECK_EQ(captured.validator.strength, ValidatorStrength::Weak);
        CHECK_EQ(captured.validator.value, std::string("W/\"v1\""));
        // Captured, and not sent. A weak validator is still worth having --
        // a changed one is positive evidence of `AssetChanged` -- but RFC 9110
        // §13.1.5 admits only a strong validator in `If-Range`.
        CHECK_EQ(captured.ifRangeValue, std::string());
    }
    {
        const CapturedValidator captured = CaptureValidator(
            Headers({{"Last-Modified", "Sat, 16 Aug 2026 10:00:00 GMT"}}));
        CHECK_EQ(captured.validator.kind, ValidatorKind::HttpDate);
        // Weak for arithmetic reasons, not policy ones: one-second granularity
        // cannot distinguish two revisions published inside one second.
        CHECK_EQ(captured.validator.strength, ValidatorStrength::Weak);
        // Legal in `If-Range` all the same, under its own rule. This is the one
        // place the weak/strong split does not follow `ValidatorStrength`.
        CHECK_EQ(captured.ifRangeValue, captured.validator.value);
    }
    {
        // A strong `ETag` outranks a date, so a server offering both is bound
        // to the one that can actually prove byte identity.
        const CapturedValidator captured = CaptureValidator(
            Headers({{"ETag", "\"v1\""},
                     {"Last-Modified", "Sat, 16 Aug 2026 10:00:00 GMT"}}));
        CHECK_EQ(captured.validator.kind, ValidatorKind::EntityTag);
    }
    {
        // The no-usable-validator fixture. Reads still work; the reader is
        // bound as far as it can observe and reports `Unavailable` upward.
        const CapturedValidator captured =
            CaptureValidator(Headers({{"Content-Length", "10"}}));
        CHECK_EQ(captured.validator.kind, ValidatorKind::None);
        CHECK(!captured.validator.IsUsable());
        CHECK_EQ(captured.ifRangeValue, std::string());
    }
}

void TestValidatorContradiction() {
    const CapturedValidator captured = CaptureValidator(Headers({{"ETag", "\"v1\""}}));

    CHECK(!ContradictsCapturedValidator(Headers({{"ETag", "\"v1\""}}),
                                        captured.validator));
    CHECK(ContradictsCapturedValidator(Headers({{"ETag", "\"v2\""}}),
                                       captured.validator));

    // A response that repeats nothing proves nothing. Inventing a change out of
    // a missing header would fail every read against a server that simply
    // stopped repeating itself on a `206`.
    CHECK(!ContradictsCapturedValidator(Headers({{"Content-Length", "10"}}),
                                        captured.validator));

    // A validator of a different kind is not evidence about this one.
    CHECK(!ContradictsCapturedValidator(
        Headers({{"Last-Modified", "Sat, 16 Aug 2026 10:00:00 GMT"}}),
        captured.validator));

    // Nothing was captured, so nothing can contradict it.
    const CapturedValidator none = CaptureValidator(Headers({{"Content-Length", "1"}}));
    CHECK(!ContradictsCapturedValidator(Headers({{"ETag", "\"v9\""}}), none.validator));
}

void TestByteRangeFormatting() {
    CHECK_EQ(FormatByteRange(0, 1), std::string("bytes=0-0"));
    CHECK_EQ(FormatByteRange(0, 256), std::string("bytes=0-255"));
    CHECK_EQ(FormatByteRange(1000, 24), std::string("bytes=1000-1023"));
}

}  // namespace

int main() {
    TestHeaderTable();
    TestContentRange();
    TestContentLength();
    TestAcceptRanges();
    TestValidatorCapture();
    TestValidatorContradiction();
    TestByteRangeFormatting();
    return usdassettest::Report("usdAssetHttp/framing");
}
