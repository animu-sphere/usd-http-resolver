// SPDX-License-Identifier: Apache-2.0

#include "usdassetfixture/Corpus.h"

namespace usdassetfixture {

const std::vector<Behavior>& AllBehaviors() {
    // Declaration order, and the whole enum. The corpus self-test walks this
    // list and fails when a behavior has no case, so a behavior added to the
    // header without a case added beside it fails the run rather than joining
    // a coverage claim it does not support.
    static const std::vector<Behavior> behaviors = {
        Behavior::Normal,
        Behavior::NoAcceptRanges,
        Behavior::IgnoresRange,
        Behavior::TruncatedBody,
        Behavior::ContentRangeTooShort,
        Behavior::ContentRangeShifted,
        Behavior::UnknownContentLength,
        Behavior::ValidatorChangeMidRead,
        Behavior::RedirectChain,
        Behavior::RedirectLoop,
        Behavior::SlowHeaders,
        Behavior::SlowBody,
        Behavior::ConnectionResetBeforeResponse,
        Behavior::ConnectionResetMidBody,
        Behavior::RangeNotSatisfiable,
        Behavior::NotFound,
        Behavior::AccessDenied,
        Behavior::TransientServerError,
    };
    return behaviors;
}

const char* BehaviorName(Behavior behavior) noexcept {
    switch (behavior) {
        case Behavior::Normal: return "Normal";
        case Behavior::NoAcceptRanges: return "NoAcceptRanges";
        case Behavior::IgnoresRange: return "IgnoresRange";
        case Behavior::TruncatedBody: return "TruncatedBody";
        case Behavior::ContentRangeTooShort: return "ContentRangeTooShort";
        case Behavior::ContentRangeShifted: return "ContentRangeShifted";
        case Behavior::UnknownContentLength: return "UnknownContentLength";
        case Behavior::ValidatorChangeMidRead: return "ValidatorChangeMidRead";
        case Behavior::RedirectChain: return "RedirectChain";
        case Behavior::RedirectLoop: return "RedirectLoop";
        case Behavior::SlowHeaders: return "SlowHeaders";
        case Behavior::SlowBody: return "SlowBody";
        case Behavior::ConnectionResetBeforeResponse:
            return "ConnectionResetBeforeResponse";
        case Behavior::ConnectionResetMidBody: return "ConnectionResetMidBody";
        case Behavior::RangeNotSatisfiable: return "RangeNotSatisfiable";
        case Behavior::NotFound: return "NotFound";
        case Behavior::AccessDenied: return "AccessDenied";
        case Behavior::TransientServerError: return "TransientServerError";
    }
    return "Unknown";
}

const char* BehaviorDescription(Behavior behavior) noexcept {
    switch (behavior) {
        case Behavior::Normal:
            return "honors Range, If-Range, and HEAD as RFC 9110 requires";
        case Behavior::NoAcceptRanges:
            return "no Accept-Ranges, and 200 with the whole body for a Range";
        case Behavior::IgnoresRange:
            return "advertises Accept-Ranges, then answers a Range with 200";
        case Behavior::TruncatedBody:
            return "honest headers, fewer body bytes, orderly close";
        case Behavior::ContentRangeTooShort:
            return "206 whose Content-Range covers less than was requested";
        case Behavior::ContentRangeShifted:
            return "206 whose Content-Range starts at a different offset";
        case Behavior::UnknownContentLength:
            return "200 with no Content-Length, body delimited by close";
        case Behavior::ValidatorChangeMidRead:
            return "the ETag and content change underneath an open reader";
        case Behavior::RedirectChain:
            return "redirectHops 302s, then the asset";
        case Behavior::RedirectLoop:
            return "a 302 pointing at its own path, unbounded by the server";
        case Behavior::SlowHeaders:
            return "delayMs before the status line";
        case Behavior::SlowBody:
            return "headers and one byte, delayMs, then the rest";
        case Behavior::ConnectionResetBeforeResponse:
            return "reads the request, then RST";
        case Behavior::ConnectionResetMidBody:
            return "headers, deliverBytes of body, then RST";
        case Behavior::RangeNotSatisfiable:
            return "416 with Content-Range: bytes */size for any Range";
        case Behavior::NotFound: return "404";
        case Behavior::AccessDenied: return "403";
        case Behavior::TransientServerError:
            return "503 for transientFailures requests, then normal service";
    }
    return "unknown";
}

}  // namespace usdassetfixture
