// SPDX-License-Identifier: Apache-2.0

#include "usdAssetHttp/HttpAssetReader.h"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "usdAssetIo/RangeMath.h"
#include "Framing.h"
#include "TestSupport.h"
#include "Transport.h"
#include "Uri.h"

namespace usdasset {
namespace http {
namespace {

constexpr char kDefaultUserAgent[] = "usd-http-resolver/0.2.0";

std::uint64_t ElapsedMicroseconds(std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - since)
            .count());
}

/// Where a request's counters go.
///
/// Open issues requests before a reader -- and therefore before a
/// `ReaderMetrics` -- exists, so the counting is behind this small interface
/// and the two phases are its two implementations. The alternative, constructing
/// the reader's metrics against the identifier the caller passed in, would key
/// every dump on the pre-redirect URL.
class RequestSink {
public:
    virtual ~RequestSink() = default;
    virtual void Request(bool metadata, std::uint64_t bytes, std::uint64_t microseconds) = 0;
    virtual void Retry() = 0;
    virtual void Redirect() = 0;
};

/// Counters accumulated during open and replayed once the reader exists.
class PendingSink final : public RequestSink {
public:
    void Request(bool metadata, std::uint64_t bytes, std::uint64_t microseconds) override {
        ++requests;
        if (metadata) ++metadataRequests;
        bytesTransferred += bytes;
        latencies.push_back(microseconds);
    }
    void Retry() override { ++retries; }
    void Redirect() override { ++redirects; }

    /// Replays into the reader's counters. A failed open replays into nothing,
    /// which matches the local backend: there is no reader to attribute the
    /// cost to, and the process aggregate accumulates only from readers.
    ///
    /// `metadataRequestCount` is a subset of `requestCount` and
    /// `AddMetadataRequest` bumps both, so only the difference is added as a
    /// plain request. Adding each total separately counts every metadata round
    /// trip twice, which would inflate the denominator of `requestEfficiency`
    /// and understate the amplification this project is measured by.
    void ReplayInto(ReaderMetrics& metrics) const {
        metrics.AddRequest(requests - metadataRequests);
        metrics.AddMetadataRequest(metadataRequests);
        metrics.AddRetry(retries);
        metrics.AddRedirect(redirects);
        metrics.AddBytesTransferred(bytesTransferred);
        for (const std::uint64_t sample : latencies) {
            metrics.RequestLatency().Record(sample);
        }
    }

    std::uint64_t requests = 0;
    std::uint64_t metadataRequests = 0;
    std::uint64_t retries = 0;
    std::uint64_t redirects = 0;
    std::uint64_t bytesTransferred = 0;
    std::vector<std::uint64_t> latencies;
};

class ReaderSink final : public RequestSink {
public:
    explicit ReaderSink(ReaderMetrics& metrics) : _metrics(metrics) {}

    void Request(bool metadata, std::uint64_t bytes, std::uint64_t microseconds) override {
        // One or the other. `AddMetadataRequest` bumps `requestCount` too --
        // a metadata request is a request, and counting it as both is counting
        // it twice.
        if (metadata) {
            _metrics.AddMetadataRequest();
        } else {
            _metrics.AddRequest();
        }
        _metrics.AddBytesTransferred(bytes);
        _metrics.RequestLatency().Record(microseconds);
    }
    void Retry() override { _metrics.AddRetry(); }
    void Redirect() override { _metrics.AddRedirect(); }

private:
    ReaderMetrics& _metrics;
};

// --- projection onto the typed vocabulary ------------------------------------
//
// One place, deliberately. ADR-0003 records that libcurl's error strings are
// never passed through: they embed the effective URL, and a message built from
// one would undo the credential elision v0.1.0 shipped. Every failure below is
// built from this repository's own words plus an already-sanitized identifier.

std::string Where(const Uri& uri) {
    return " (" + ElideSecrets(uri.ToIdentity()) + ")";
}

Status ProjectTransportError(TransportError error, const Uri& uri) {
    switch (error) {
        case TransportError::ConnectFailed:
            return Status::Error(StatusCode::NetworkError,
                                 "could not connect to the server" + Where(uri));
        case TransportError::ConnectionLost:
            return Status::Error(StatusCode::NetworkError,
                                 "the server closed the connection" + Where(uri));
        case TransportError::ConnectTimeout:
            // Which deadline elapsed, named. DIAGNOSTICS.md requires `HTTP006`
            // to distinguish connect, read, and total, and a caller staring at
            // "timeout" cannot tell a firewall from a slow origin.
            return Status::Error(StatusCode::Timeout,
                                 "the connect deadline elapsed" + Where(uri));
        case TransportError::ResponseTimeout:
            return Status::Error(
                StatusCode::Timeout,
                "the response deadline elapsed before the server sent a status line" +
                    Where(uri));
        case TransportError::TransferTimeout:
            return Status::Error(StatusCode::Timeout,
                                 "the transfer deadline elapsed while the body was "
                                 "still arriving" +
                                     Where(uri));
        case TransportError::IncompleteBody:
            return Status::Error(StatusCode::InvalidResponse,
                                 "the response body ended before the length it "
                                 "declared" +
                                     Where(uri));
        case TransportError::Malformed:
            return Status::Error(StatusCode::InvalidResponse,
                                 "the response could not be parsed as HTTP" + Where(uri));
        case TransportError::Internal:
            return Status::Error(StatusCode::NetworkError,
                                 "the HTTP client could not issue the request" +
                                     Where(uri));
        case TransportError::None:
            break;
    }
    return Status::Ok();
}

/// A non-2xx, non-redirect status, projected.
///
/// `403` and `404` stay separate because one is a configuration problem and the
/// other is a scene problem (DIAGNOSTICS.md §4.4), and a `5xx` that outlived its
/// retries is reported as a transport fault with the status attached: the
/// vocabulary has no server-is-unwell code, and what a caller does about a
/// spent `503` is what it does about a reset connection.
Status ProjectHttpStatus(int status, const Uri& uri) {
    if (status == 404 || status == 410) {
        return Status::Error(StatusCode::NotFound, "the asset does not exist" + Where(uri))
            .WithTransportStatus(status);
    }
    if (status == 401 || status == 403) {
        return Status::Error(StatusCode::AccessDenied,
                             "the server refused access to the asset" + Where(uri))
            .WithTransportStatus(status);
    }
    if (status == 405 || status == 501) {
        // A server that refuses `HEAD`. §4.1 of the design policy admits "a
        // minimal metadata request where HEAD is unavailable" as the fallback,
        // and this backend does not implement one: the hostile corpus has no
        // row that refuses `HEAD`, so the fallback would ship unexercised. Named
        // rather than approximated, so that the next contributor finds a gap
        // instead of a silent second round trip.
        return Status::Error(StatusCode::Unsupported,
                             "the server does not support the metadata request this "
                             "backend issues (HEAD)" +
                                 Where(uri))
            .WithTransportStatus(status);
    }
    if (status >= 500) {
        return Status::Error(StatusCode::NetworkError,
                             "the server reported an error and did not recover within "
                             "the retry budget" +
                                 Where(uri))
            .WithTransportStatus(status);
    }
    if (status == 429) {
        return Status::Error(StatusCode::NetworkError,
                             "the server is rate limiting and did not recover within "
                             "the retry budget" +
                                 Where(uri))
            .WithTransportStatus(status);
    }
    return Status::Error(StatusCode::InvalidResponse,
                         "the server answered with an unusable status" + Where(uri))
        .WithTransportStatus(status);
}

Status AssetChangedStatus(const Uri& uri, const char* how) {
    return Status::Error(StatusCode::AssetChanged,
                         std::string("the asset changed while it was open: ") + how +
                             Where(uri));
}

bool IsRedirect(int status) noexcept {
    // `303` is absent on purpose: it means "fetch a different resource with
    // GET", which is a semantic this backend has no use for on an asset read,
    // and following it would silently substitute one asset for another.
    return status == 301 || status == 302 || status == 307 || status == 308;
}

/// Statuses worth issuing the same idempotent request for again.
///
/// `500` is not among them: it is as likely to be deterministic as transient,
/// and retrying it three times turns one failure into three.
bool IsRetryableStatus(int status) noexcept {
    return status == 429 || status == 502 || status == 503 || status == 504;
}

/// Failures worth retrying when nothing at all came back.
///
/// Timeouts are excluded. A deadline that elapsed is a statement about how long
/// the caller is prepared to wait, and re-spending it two more times triples
/// the wait the caller already declared too long.
bool IsRetryableTransportError(TransportError error) noexcept {
    return error == TransportError::ConnectFailed ||
           error == TransportError::ConnectionLost;
}

/// Whether a body that stopped early is worth asking for the remainder of.
///
/// A transfer that ended -- cleanly short, or destroyed -- can be resumed from
/// where it stopped, and on a real network that is what retry is for. A
/// deadline cannot: the caller has already said how long it is prepared to
/// wait, and asking again spends that budget a second time while reporting the
/// eventual failure as a malformed response rather than as the timeout it was.
bool IsResumable(TransportError error) noexcept {
    return error == TransportError::IncompleteBody ||
           error == TransportError::ConnectionLost;
}

// --- one logical exchange ----------------------------------------------------

struct ExchangeResult {
    TransportResponse response;
    /// Failure means no usable response arrived. An `Ok` here says nothing about
    /// the HTTP status, which is the caller's to interpret -- ADR-0002 makes a
    /// `200` a failure in one context and a success in another.
    Status status;
    Uri finalUri;
};

/// Issues one request, following bounded redirects and applying bounded retry.
///
/// `retriesRemaining` is the budget for the whole logical operation, and is
/// consumed rather than reset. An open, or one `Read`, allocates
/// `maxAttempts - 1` retries once and every request under it draws from that
/// pool -- including the resume attempts the read loop makes, which are retries
/// however they are spelled.
///
/// Passing the budget rather than re-deriving it from `options` is the whole
/// point. Two nested loops each bounded by `maxAttempts` are jointly bounded by
/// its *square*, which is not what "requests per logical operation" means and
/// is 27 requests where the caller asked for 3. Redirect hops draw from
/// `maxRedirects` instead, because a hop is not a retry: a chain longer than the
/// retry budget must still be followed to its end.
///
/// Total work is therefore bounded by `maxRedirects + maxAttempts` requests,
/// each with its own deadline, which is how §10's "cap retries and total time"
/// is satisfied without a second clock.
ExchangeResult PerformExchange(Transport& transport,
                               const HttpOptions& options,
                               const Uri& start,
                               Method method,
                               const std::string& range,
                               const std::string& ifRange,
                               void* body,
                               std::size_t capacity,
                               int* retriesRemaining,
                               RequestSink& sink) {
    ExchangeResult result;
    result.finalUri = start;

    Uri current = start;
    int redirects = 0;

    for (;;) {
        TransportResponse response;

        for (;;) {
            TransportRequest request;
            request.url = current.ToString();
            request.method = method;
            request.range = range;
            request.ifRange = ifRange;
            request.userAgent =
                options.userAgent.empty() ? kDefaultUserAgent : options.userAgent;
            request.timeouts.connectMs = options.connectTimeoutMs;
            request.timeouts.responseMs = options.responseTimeoutMs;
            request.timeouts.transferMs = options.transferTimeoutMs;
            request.body = body;
            request.bodyCapacity = capacity;

            const std::chrono::steady_clock::time_point started =
                std::chrono::steady_clock::now();
            response = transport.Perform(request);
            sink.Request(method == Method::Head, response.bodyBytes,
                         ElapsedMicroseconds(started));

            if (*retriesRemaining <= 0) break;
            // Retried only when nothing usable came back. A response whose
            // headers arrived is the caller's to judge, and a body that stopped
            // early is resumed by the read loop rather than re-fetched whole.
            const bool retryable =
                response.status == 0 ? IsRetryableTransportError(response.error)
                                     : IsRetryableStatus(response.status);
            if (!retryable) break;
            --*retriesRemaining;
            sink.Retry();
        }

        result.finalUri = current;

        if (response.status == 0) {
            result.response = std::move(response);
            result.status = ProjectTransportError(result.response.error, current);
            return result;
        }
        if (!IsRedirect(response.status)) {
            result.response = std::move(response);
            result.status = Status::Ok();
            return result;
        }

        const std::string* location = response.headers.Find("Location");
        if (location == nullptr || location->empty()) {
            result.response = std::move(response);
            result.status = Status::Error(StatusCode::InvalidResponse,
                                          "the server redirected without a Location" +
                                              Where(current))
                                .WithTransportStatus(result.response.status);
            return result;
        }
        if (redirects >= options.maxRedirects) {
            result.response = std::move(response);
            result.status =
                Status::Error(StatusCode::InvalidResponse,
                              "the redirect chain exceeded " +
                                  std::to_string(options.maxRedirects) + " hops" +
                                  Where(current))
                    .WithTransportStatus(result.response.status);
            return result;
        }

        Uri target;
        std::string error;
        if (!ResolveReference(current, *location, &target, &error)) {
            result.response = std::move(response);
            result.status = Status::Error(StatusCode::InvalidResponse,
                                          "the server redirected to an unusable "
                                          "location: " +
                                              error + Where(current))
                                .WithTransportStatus(result.response.status);
            return result;
        }
        if (IsSchemeDowngrade(current, target)) {
            // §10 of the design policy. A server answering an `https` request
            // with an `http` Location is asking the client to re-issue it in
            // the clear; a client that obliges has removed the transport
            // security its caller asked for, silently.
            result.response = std::move(response);
            result.status = Status::Error(
                                StatusCode::InvalidResponse,
                                "the server redirected from https to http, which "
                                "would drop transport security" +
                                    Where(current))
                                .WithTransportStatus(result.response.status);
            return result;
        }

        ++redirects;
        sink.Redirect();
        current = target;
    }
}

}  // namespace

// --- Impl --------------------------------------------------------------------

class HttpAssetReader::Impl {
public:
    Impl(std::unique_ptr<Transport> transport,
         HttpOptions options,
         Uri uri,
         AssetMetadata metadata,
         std::string ifRangeValue)
        : transport(std::move(transport)),
          options(std::move(options)),
          uri(std::move(uri)),
          metadata(std::move(metadata)),
          ifRangeValue(std::move(ifRangeValue)),
          metrics(this->metadata.resolvedIdentifier) {
        metrics.SetAssetSize(this->metadata.size);
    }

    std::unique_ptr<Transport> transport;
    HttpOptions options;
    Uri uri;                    ///< Post-redirect, with credentials, for the wire.
    AssetMetadata metadata;     ///< Immutable for the reader's lifetime.
    std::string ifRangeValue;   ///< Empty when no conditional guard may be sent.
    ReaderMetrics metrics;
};

HttpAssetReader::HttpAssetReader(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

HttpAssetReader::~HttpAssetReader() = default;

const AssetMetadata& HttpAssetReader::Metadata() const { return _impl->metadata; }

const ReaderMetrics& HttpAssetReader::Metrics() const noexcept { return _impl->metrics; }

ReadResult HttpAssetReader::Read(std::uint64_t offset, void* dst, std::size_t size) {
    ScopedLatency readTimer(_impl->metrics.ReadLatency());
    _impl->metrics.AddBytesRequested(size);

    const ReadRange range = ResolveReadRange(offset, size, _impl->metadata.size);
    if (range.outcome == ReadRangeOutcome::Overflow) {
        return ReadResult{0, OverflowStatus(offset, size)};
    }
    // Before the empty check, for the reason the local backend gives: a null
    // buffer with a non-zero length is a caller bug wherever the offset lands,
    // and a check that only fires on a non-empty range leaves it invisible
    // until an offset happens to fall inside the asset.
    if (size > 0 && dst == nullptr) {
        return ReadResult{0, Status::Error(StatusCode::InvalidArgument,
                                           "read destination buffer is null")
                                 .WithRange(offset, size)};
    }
    if (range.outcome == ReadRangeOutcome::Empty) {
        // No request is issued. That is a counter assertion and an amplification
        // property, not only a comment: the fixture server's request log is what
        // proves a zero-length read costs nothing.
        //
        // Nor is the validator revalidated here. An offset at or past the size
        // captured at open is past the end *of the revision this reader is bound
        // to*, so `0, Ok` is that revision's truthful answer, and revalidating
        // would spend a round trip on every read that moves no bytes.
        return ReadResult{0, Status::Ok()};
    }

    unsigned char* out = static_cast<unsigned char*>(dst);
    std::size_t done = 0;
    ReaderSink sink(_impl->metrics);
    TransportError lastError = TransportError::None;

    // One budget for the whole read, shared with the retries `PerformExchange`
    // makes underneath. A resume is a retry however it is spelled, and two
    // loops each bounded by `maxAttempts` are jointly bounded by its square --
    // which is not what the option means and is not what a caller asked for.
    int retriesRemaining = _impl->options.maxAttempts > 0
                               ? _impl->options.maxAttempts - 1
                               : 0;
    bool firstAttempt = true;

    while (done < range.length) {
        if (!firstAttempt) {
            if (retriesRemaining <= 0) {
                // The budget is spent and the range is still short. Which
                // failure this is depends on how the last attempt ended: a
                // connection that was destroyed is a transport fault, and one
                // that closed cleanly below the length it promised is a
                // malformed response. The corpus has a row for each, and they
                // are reported differently because a caller does different
                // things about them.
                if (lastError == TransportError::ConnectionLost) {
                    return ReadResult{done, ProjectTransportError(lastError, _impl->uri)
                                                .WithRange(offset, size)};
                }
                return ReadResult{done,
                                  ShortReadStatus(offset, range.length, done,
                                                  _impl->metadata.resolvedIdentifier)};
            }
            --retriesRemaining;
            sink.Retry();
        }
        firstAttempt = false;

        const std::uint64_t at = offset + done;
        const std::size_t remaining = range.length - done;

        const ExchangeResult exchange = PerformExchange(
            *_impl->transport, _impl->options, _impl->uri, Method::Get,
            FormatByteRange(at, remaining), _impl->ifRangeValue, out + done, remaining,
            &retriesRemaining, sink);
        if (!exchange.status.IsOk()) {
            return ReadResult{done, Status(exchange.status).WithRange(offset, size)};
        }
        const TransportResponse& response = exchange.response;
        lastError = response.error;

        if (response.status == 200) {
            // Two very different servers answer a range request with the whole
            // representation, and the difference is not in the status.
            //
            // One ignores `Range` -- it advertised byte ranges at open and does
            // not honor them, which ADR-0002 makes `RangeNotSupported` and
            // terminal. The other honored the `If-Range` this reader sent,
            // found it stale, and correctly returned the whole *new*
            // representation per RFC 9110 §13.1.5, which is `AssetChanged`.
            //
            // What separates them is the response itself: a server that merely
            // ignored the range is still serving the revision this reader bound
            // to, so its validator and its length still match. Branching on
            // "did we send If-Range" instead would report every range-ignoring
            // server as a changed asset.
            if (ContradictsCapturedValidator(response.headers,
                                             _impl->metadata.validator)) {
                return ReadResult{0, AssetChangedStatus(exchange.finalUri,
                                                        "a range request was answered "
                                                        "with a whole representation "
                                                        "carrying a different validator")
                                         .WithRange(offset, size)};
            }
            std::uint64_t whole = 0;
            const std::string* length = response.headers.Find("Content-Length");
            if (length != nullptr && ParseContentLength(*length, &whole) &&
                whole != _impl->metadata.size) {
                // Length is evidence even where there is no validator to
                // compare, which is what makes the no-validator fixture's
                // revision change detectable at all.
                return ReadResult{0, AssetChangedStatus(exchange.finalUri,
                                                        "a range request was answered "
                                                        "with a whole representation of "
                                                        "a different length")
                                         .WithRange(offset, size)};
            }
            // Terminal, with no whole-asset fallback -- and the body was cut off
            // at the requested length rather than downloaded, which is the point
            // of bounding the write.
            return ReadResult{done,
                              Status::Error(StatusCode::RangeNotSupported,
                                            "the server answered a range request with "
                                            "the whole representation" +
                                                Where(exchange.finalUri))
                                  .WithTransportStatus(200)
                                  .WithRange(offset, size)};
        }
        if (response.status != 206) {
            if (response.status == 416) {
                // A `416` is the one refusal that is usually *true*. The range
                // this reader asked for lies inside the size it captured at
                // open, so if the server says it is unsatisfiable, the most
                // likely explanation is that the representation is no longer
                // the one that size came from -- and a `416` is required to
                // carry `Content-Range: bytes */<complete-length>`, which says
                // so outright.
                //
                // Checked before the refusal is reported, because reporting
                // `InvalidResponse` here would attach a message that is
                // factually false: the range does *not* lie inside the asset
                // any more. This is the same evidence the `200` branch uses,
                // and it is the reason a shrinking asset is `AssetChanged`
                // rather than a malformed server.
                if (ContradictsCapturedValidator(response.headers,
                                                 _impl->metadata.validator)) {
                    return ReadResult{0, AssetChangedStatus(
                                             exchange.finalUri,
                                             "a range this reader had already sized "
                                             "was refused, with a different validator")
                                             .WithRange(offset, size)};
                }
                ContentRange unsatisfiable;
                const std::string* refused = response.headers.Find("Content-Range");
                if (refused != nullptr &&
                    ParseContentRange(*refused, &unsatisfiable) &&
                    unsatisfiable.unsatisfied &&
                    unsatisfiable.completeLength != _impl->metadata.size) {
                    return ReadResult{0, AssetChangedStatus(
                                             exchange.finalUri,
                                             "the representation is now a different "
                                             "length, and no longer contains the range "
                                             "this reader asked for")
                                             .WithRange(offset, size)};
                }
                return ReadResult{
                    done, Status::Error(StatusCode::InvalidResponse,
                                        "the server refused a range that lies inside "
                                        "the size it reported at open" +
                                            Where(exchange.finalUri))
                              .WithTransportStatus(416)
                              .WithRange(offset, size)};
            }
            return ReadResult{done, ProjectHttpStatus(response.status, exchange.finalUri)
                                        .WithRange(offset, size)};
        }

        // The response's own identity, checked independently of `If-Range`.
        // This is what catches a revision whose bytes are unchanged and whose
        // validator moved, and it is the whole guard for an asset whose captured
        // validator admits no conditional header.
        if (ContradictsCapturedValidator(response.headers, _impl->metadata.validator)) {
            return ReadResult{0, AssetChangedStatus(exchange.finalUri,
                                                    "the response carried a different "
                                                    "validator")
                                     .WithRange(offset, size)};
        }

        const std::string* rangeHeader = response.headers.Find("Content-Range");
        ContentRange contentRange;
        // Same refusal as at open, for the two headers this branch's arithmetic
        // rests on: a response that says two different things about where its
        // bytes belong has not framed them.
        if (response.headers.HasConflictingDuplicate("Content-Range") ||
            response.headers.HasConflictingDuplicate("Content-Length") ||
            rangeHeader == nullptr || !ParseContentRange(*rangeHeader, &contentRange) ||
            contentRange.unsatisfied) {
            return ReadResult{done,
                              Status::Error(StatusCode::InvalidResponse,
                                            "the partial response had no usable "
                                            "Content-Range" +
                                                Where(exchange.finalUri))
                                  .WithTransportStatus(206)
                                  .WithRange(offset, size)};
        }
        if (contentRange.hasCompleteLength &&
            contentRange.completeLength != _impl->metadata.size) {
            // The representation is a different length than the one this reader
            // bound to. That is a change, and a stronger signal than a validator
            // -- it holds even for an asset that supplied no validator at all.
            return ReadResult{0, AssetChangedStatus(exchange.finalUri,
                                                    "the representation's length "
                                                    "differs from the size captured at "
                                                    "open")
                                     .WithRange(offset, size)};
        }
        // Validated against the *request*, not only against itself. A response
        // that is internally consistent and describes a different window than
        // the one asked for is the case DIAGNOSTICS.md §6 names, and copying it
        // into the caller's buffer is silent data loss.
        //
        // The length half of this is stricter than RFC 9110 requires, and
        // deliberately so. An origin may answer a single-range request with a
        // prefix of it, and the resume loop below could accept that and ask for
        // the rest -- but §10 of the design policy says to "validate that a
        // Content-Range response actually covers the requested range before
        // copying it into a caller's buffer", DIAGNOSTICS.md §6 uses precisely
        // this response as its `InvalidResponse` example, and the corpus keeps
        // `ContentRangeTooShort` as a hostile row on that basis. Relaxing it is
        // a change to those documents first, for every backend, and not a
        // quiet accommodation here.
        //
        // The cost is named rather than hidden: an origin that caps the size of
        // a range response is refused instead of being read in pieces. Nothing
        // in the corpus behaves that way, so nothing here would notice if the
        // rule were wrong, which is the other half of why it stays a documented
        // decision.
        if (contentRange.first != at || contentRange.Length() != remaining) {
            return ReadResult{
                done,
                Status::Error(StatusCode::InvalidResponse,
                              "the partial response did not cover the requested range "
                              "(requested bytes " +
                                  std::to_string(at) + "-" +
                                  std::to_string(at + remaining - 1) +
                                  ", server returned " +
                                  std::to_string(contentRange.first) + "-" +
                                  std::to_string(contentRange.last) + ")" +
                                  Where(exchange.finalUri))
                    .WithTransportStatus(206)
                    .WithRange(offset, size)};
        }
        if (response.bodyOverflowed) {
            return ReadResult{done,
                              Status::Error(StatusCode::InvalidResponse,
                                            "the server sent more body than its own "
                                            "framing declared" +
                                                Where(exchange.finalUri))
                                  .WithTransportStatus(206)
                                  .WithRange(offset, size)};
        }

        done += response.bodyBytes;

        // The framing was sound and the transfer was not. Whether to ask for
        // the remainder depends on how it ended: a connection that stopped can
        // be resumed, and an elapsed deadline is the caller's budget rather
        // than the server's fault. Reported with the bytes that did arrive,
        // which the caller may not treat as a complete read.
        if (response.error != TransportError::None && !IsResumable(response.error)) {
            return ReadResult{done, ProjectTransportError(response.error,
                                                          exchange.finalUri)
                                        .WithRange(offset, size)};
        }

        if (response.bodyBytes == 0 && response.error == TransportError::None) {
            // A correctly framed `206` that delivered nothing and reported no
            // fault. Nothing about the next attempt would differ, so the loop is
            // cut here rather than spun to the end of its budget.
            return ReadResult{done, ShortReadStatus(offset, range.length, done,
                                                    _impl->metadata.resolvedIdentifier)};
        }
    }

    return ReadResult{done, Status::Ok()};
}

// --- open --------------------------------------------------------------------

struct HttpReaderFactory {
    static HttpOpenResult Open(const std::string& url,
                               const HttpOptions& options,
                               const testing::MakeTransport& factory) {
        // Timed from here rather than with a ScopedLatency: the histogram being
        // timed into does not exist until the reader does, and `openLatency` has
        // to measure what the caller waited for.
        const std::chrono::steady_clock::time_point started =
            std::chrono::steady_clock::now();

        HttpOpenResult result;

        Uri uri;
        std::string error;
        if (!ParseUri(url, &uri, &error)) {
            result.status = Status::Error(StatusCode::InvalidArgument,
                                          error + " (" + ElideSecrets(url) + ")");
            return result;
        }

        std::unique_ptr<Transport> transport = factory ? factory() : MakeCurlTransport();
        if (!transport) {
            result.status = ProjectTransportError(TransportError::Internal, uri);
            return result;
        }

        PendingSink sink;
        int retriesRemaining = options.maxAttempts > 0 ? options.maxAttempts - 1 : 0;
        const ExchangeResult exchange =
            PerformExchange(*transport, options, uri, Method::Head, std::string(),
                            std::string(), nullptr, 0, &retriesRemaining, sink);
        if (!exchange.status.IsOk()) {
            result.status = exchange.status;
            return result;
        }
        const TransportResponse& response = exchange.response;

        if (response.status != 200) {
            result.status = ProjectHttpStatus(response.status, exchange.finalUri);
            return result;
        }

        // Size, from a length the server actually stated. ADR-0003 requires a
        // client that can refuse an unknown `Content-Length` rather than one
        // that helpfully accumulates until the connection closes -- a reader
        // that cannot state its size cannot serve random access, and guessing
        // one by downloading the asset is the transfer this project exists to
        // avoid.
        std::uint64_t size = 0;
        const std::string* length = response.headers.Find("Content-Length");
        // Two `Content-Length` lines that disagree is the request-smuggling
        // shape RFC 9110 §8.6 requires a message to be rejected for: an
        // intermediary that believes one and an origin that believes the other
        // disagree about where this message ends and the next begins. Asked as
        // its own question, because `Find` has to return a single value and so
        // cannot express the conflict.
        if (response.headers.HasConflictingDuplicate("Content-Length") ||
            length == nullptr || !ParseContentLength(*length, &size)) {
            result.status =
                Status::Error(StatusCode::InvalidResponse,
                              "the server did not state a usable Content-Length, so "
                              "the asset's size is unknown" +
                                  Where(exchange.finalUri))
                    .WithTransportStatus(200);
            return result;
        }

        if (!AdvertisesByteRanges(response.headers)) {
            // Terminal, per ADR-0002: no whole-asset fallback. Silently
            // downloading a 10 GB asset because a header was missing is the
            // specific failure that ADR exists to prevent.
            result.status = Status::Error(StatusCode::RangeNotSupported,
                                          "the server does not advertise byte ranges" +
                                              Where(exchange.finalUri))
                                .WithTransportStatus(200);
            return result;
        }

        const CapturedValidator captured = CaptureValidator(response.headers);

        AssetMetadata metadata;
        // The identity, with any credential removed. This is what a message
        // quotes, what the metrics dump keys on, and what `v0.3.0` will key a
        // cache entry on.
        metadata.resolvedIdentifier = exchange.finalUri.ToIdentity();
        metadata.size = size;
        // True because the alternative already returned above: a reader is never
        // handed back in a state where its range support is unknown.
        metadata.supportsRandomAccess = true;
        metadata.validator = captured.validator;
        metadata.stability = ClassifyStability(captured.validator);
        metadata.contentType = response.headers.Get("Content-Type");

        std::unique_ptr<HttpAssetReader::Impl> impl(new HttpAssetReader::Impl(
            std::move(transport), options, exchange.finalUri, std::move(metadata),
            captured.ifRangeValue));
        sink.ReplayInto(impl->metrics);
        impl->metrics.OpenLatency().Record(ElapsedMicroseconds(started));

        result.reader.reset(new HttpAssetReader(std::move(impl)));
        return result;
    }
};

HttpOpenResult Open(const std::string& url) { return Open(url, HttpOptions()); }

HttpOpenResult Open(const std::string& url, const HttpOptions& options) {
    return HttpReaderFactory::Open(url, options, nullptr);
}

OpenResult OpenAsset(const std::string& url) { return OpenAsset(url, HttpOptions()); }

OpenResult OpenAsset(const std::string& url, const HttpOptions& options) {
    HttpOpenResult opened = Open(url, options);
    OpenResult result;
    result.status = std::move(opened.status);
    result.reader = std::move(opened.reader);
    return result;
}

namespace testing {

HttpOpenResult OpenWithTransport(const std::string& url,
                                 const HttpOptions& options,
                                 MakeTransport factory) {
    return HttpReaderFactory::Open(url, options, factory);
}

}  // namespace testing

}  // namespace http
}  // namespace usdasset
