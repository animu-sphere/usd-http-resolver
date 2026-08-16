// SPDX-License-Identifier: Apache-2.0
//
// The hostile-server corpus: the named set of server behaviors the HTTP backend
// is required to survive.
//
// §11.2 of the design policy fixes the list -- no `Accept-Ranges`, a `200` in
// answer to a `Range` request, a truncated body, a wrong `Content-Range`, a
// mid-read `ETag` change, a redirect chain, a slow response, a connection reset,
// and a `416` -- and §17 requires the corpus to exist *before* the backend, so
// that the backend is written against a passing oracle rather than debugged
// against a server.
//
// A behavior is named here rather than assembled inline in a test, for the same
// reason the boundary cases are a table: a corpus whose membership is implicit
// in test bodies cannot be shown to cover the list it claims to.
//
// Nothing outside a test links this.

#ifndef USDASSETFIXTURE_CORPUS_H
#define USDASSETFIXTURE_CORPUS_H

#include <cstdint>
#include <string>
#include <vector>

namespace usdassetfixture {

/// How a fixture's server misbehaves. One enumerator per condition a backend
/// must distinguish; two conditions that a backend would handle identically do
/// not get separate enumerators, on the same test the diagnostic vocabulary
/// uses (DIAGNOSTICS.md §1).
enum class Behavior {
    /// Honors `Range`, `If-Range`, and `HEAD` exactly as RFC 9110 requires.
    /// The corpus needs a correct row: a suite where every server is hostile
    /// cannot tell a backend that is careful from one that fails everything.
    Normal,

    /// Never emits `Accept-Ranges`, and answers a ranged `GET` with `200` and
    /// the whole body. The honest server that simply cannot do this.
    NoAcceptRanges,

    /// Advertises `Accept-Ranges: bytes`, then answers a ranged `GET` with
    /// `200` and the whole body anyway. Nastier than NoAcceptRanges, because a
    /// backend that trusted `HEAD` has already committed. ADR-0002 makes both
    /// `RangeNotSupported`, and in `v0.2.0` both are terminal: the whole body
    /// arriving is not permission to use it.
    IgnoresRange,

    /// `206` with an honest `Content-Range` and `Content-Length`, then fewer
    /// body bytes than either promises, then an orderly close. The short read
    /// below EOF: `InvalidResponse`, never a hole in the caller's buffer.
    TruncatedBody,

    /// `206` whose `Content-Range` accurately describes a range *shorter* than
    /// the one requested -- the response is internally consistent and wrong only
    /// against the request. This is the example in DIAGNOSTICS.md §6, and it is
    /// the case a backend that validates framing against itself will pass and a
    /// backend that validates against the request will catch.
    ContentRangeTooShort,

    /// `206` whose `Content-Range` describes a range at a different offset.
    /// Detected by a different check than the row above -- start, not length --
    /// which is why it is a separate row.
    ContentRangeShifted,

    /// `200` with neither `Content-Length` nor `Transfer-Encoding`, the body
    /// delimited by connection close. ADR-0003 requires a client that can refuse
    /// this rather than one that helpfully accumulates until EOF.
    UnknownContentLength,

    /// The validator and the content change after `changeAfterRequests`
    /// requests. A later request carrying the old `If-Range` gets `200` and the
    /// whole *new* body, per RFC 9110 §13.1.5 -- which is `AssetChanged`, and
    /// never data. A backend that omits `If-Range` gets a `206` of the new
    /// revision instead, with nothing to report: the silent corruption §6 of the
    /// design policy exists to prevent.
    ValidatorChangeMidRead,

    /// `redirectHops` `302`s and then the asset, served normally.
    RedirectChain,

    /// A `302` whose `Location` is the request's own path. Unbounded by the
    /// server on purpose: what stops it is the backend's own counter, and the
    /// request log is how a test proves the counter exists.
    RedirectLoop,

    /// Waits `delayMs` before the status line. The connect deadline succeeded
    /// and the response deadline did not.
    SlowHeaders,

    /// Sends the headers and one body byte, waits `delayMs`, then the rest. A
    /// separate row because a backend with only a total deadline cannot tell it
    /// from SlowHeaders, and `Timeout` (`HTTP006`) is required to name which
    /// deadline elapsed.
    SlowBody,

    /// Reads the request, then resets the connection -- `RST`, not `FIN`.
    ConnectionResetBeforeResponse,

    /// Headers, `deliverBytes` of body, then `RST`. Distinct from TruncatedBody:
    /// one connection ended, the other was destroyed, and a client reports them
    /// differently.
    ConnectionResetMidBody,

    /// `416` with `Content-Range: bytes */size` for every ranged request,
    /// whatever was asked for.
    RangeNotSatisfiable,

    /// `404`.
    NotFound,

    /// `403`.
    AccessDenied,

    /// `503` for the first `transientFailures` requests, then normal service.
    /// Bounded retry is in scope per §4.1 of the design policy, and a retry that
    /// is not visible in metrics is a defect per DIAGNOSTICS.md §3.
    TransientServerError,
};

/// Every behavior, in declaration order. The corpus is enumerable so that a
/// test can assert it covered all of it, rather than covering what someone
/// remembered to write a case for.
const std::vector<Behavior>& AllBehaviors();

const char* BehaviorName(Behavior behavior) noexcept;

/// One line, for a test report and for the README's coverage table.
const char* BehaviorDescription(Behavior behavior) noexcept;

/// One asset, and how the server is to misbehave about it.
struct AssetSpec {
    /// Absolute path, leading slash. The query string is ignored for routing
    /// and preserved in the request log, so a test can assert that a backend
    /// neither drops nor mangles it -- and that no message it emits repeats it.
    std::string path;

    std::vector<unsigned char> content;

    Behavior behavior = Behavior::Normal;

    /// Written verbatim into `ETag`, quoting included. Empty emits no `ETag`;
    /// with `lastModified` also empty that is the no-usable-validator fixture,
    /// which must still read correctly and must report `Unavailable`.
    std::string etag;

    /// Emits `W/` before `etag`. A weak validator is legal for `If-Range` only
    /// under RFC 9110's narrow rule, and the backend classifies it `Unstable`.
    bool weakValidator = false;

    /// Written verbatim into `Last-Modified`. Not synthesized from a clock:
    /// a fixture whose headers differ between runs cannot pin a validator.
    std::string lastModified;

    /// Body bytes actually delivered by TruncatedBody and
    /// ConnectionResetMidBody. `-1` means half of what the response promised,
    /// rounded down; `0` is a legal explicit value and means the headers
    /// arrived and no body did.
    std::int64_t deliverBytes = -1;

    /// Hops before the asset, for RedirectChain.
    int redirectHops = 1;

    /// Milliseconds of stall, for SlowHeaders and SlowBody.
    int delayMs = 1000;

    /// Requests answered before the revision changes, for
    /// ValidatorChangeMidRead. The default changes it after the first, which is
    /// the case that catches a backend that captured a validator at open and
    /// then never sent it.
    int changeAfterRequests = 1;

    /// The revision ValidatorChangeMidRead switches to. When `revisedContent`
    /// is empty the current content is reused with the new validator, which is
    /// the harder case: the bytes are identical and only the identity moved, so
    /// a backend comparing bytes rather than validators sees nothing.
    std::vector<unsigned char> revisedContent;
    std::string revisedEtag = "\"revised\"";

    /// Requests answered with `503` before service resumes, for
    /// TransientServerError.
    int transientFailures = 1;
};

}  // namespace usdassetfixture

#endif  // USDASSETFIXTURE_CORPUS_H
