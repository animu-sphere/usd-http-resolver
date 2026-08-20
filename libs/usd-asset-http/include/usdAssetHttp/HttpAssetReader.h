// SPDX-License-Identifier: Apache-2.0
//
// The HTTP backend.
//
// It implements the same `AssetReader` contract as the local backend and is
// admitted by the same boundary suite, unchanged. What it adds is everything a
// range reader needs in order to be correct against a server it does not
// control: response framing validation, bounded redirects, bounded retry,
// separable deadlines, and -- from its first commit rather than after it --
// validator capture and a conditional guard on every range request.
//
// The last of those is not a feature. A range reader without revision binding
// can compose a header from one revision and records from another with every
// request succeeding and nothing to report, which is the corruption §6 of the
// design policy exists to prevent. See ASSET_READER.md §2.1.
//
// No HTTP client type appears in this header, or in any header this module
// installs. libcurl is reached through a narrow internal seam and named in
// exactly one translation unit (ADR-0003).
//
// Normative contracts:
//   docs/architecture/ASSET_READER.md   read semantics, revision binding,
//                                       validator semantics
//   docs/architecture/DIAGNOSTICS.md    the typed vocabulary this maps onto
//   docs/architecture/METRICS.md        the counters it populates
//   docs/adr/0002-range-unsupported-policy.md
//   docs/adr/0003-http-client-dependency.md

#ifndef USDASSETHTTP_HTTPASSETREADER_H
#define USDASSETHTTP_HTTPASSETREADER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "usdAssetIo/AssetReader.h"
#include "usdAssetIo/Metrics.h"

namespace usdasset {
namespace http {

/// Transport policy, all of it bounded.
///
/// These are the knobs §10 of the design policy requires to exist -- "bound
/// redirect chains", "cap retries and total time" -- and they are parameters
/// rather than constants so that a test can make a deadline elapse in
/// milliseconds instead of waiting out a production one. The defaults are what
/// a caller that passes nothing gets, and they are the values the release is
/// measured with.
///
/// They are not yet resolved from the environment or from an
/// `ArResolverContext`: that is the configuration surface in `v0.6.0`
/// (RESOLVER.md §6). Until then a caller passes them or takes the defaults.
struct HttpOptions {
    /// Establishing a connection. Its own deadline because a connect that
    /// never completes and a server that never answers are different faults,
    /// and `Timeout` (`HTTP006`) is required to say which one elapsed.
    int connectTimeoutMs = 10000;

    /// Connection established to status line received. This is the deadline a
    /// server that accepts and then thinks about it for a minute elapses.
    int responseTimeoutMs = 30000;

    /// The whole exchange, headers and body. A large range over a slow link is
    /// meant to fit inside this; it is a bound on the pathological case, not a
    /// service-level expectation.
    int transferTimeoutMs = 300000;

    /// Redirect hops before a chain is refused. Bounded by this counter and
    /// never by the client library's, so that the chain is visible to the
    /// backend and testable by the corpus (ADR-0003).
    int maxRedirects = 5;

    /// HTTP requests per logical operation, retries included. `1` disables
    /// retry entirely. A retried request is counted twice in metrics, because
    /// that is what the network saw (METRICS.md §3).
    int maxAttempts = 3;

    /// Sent as `User-Agent`. Empty takes the module's default.
    std::string userAgent;
};

/// A reader over one remote asset, bound to the revision that asset had when it
/// was opened.
///
/// Thread-safe: any number of threads may call `Read` concurrently. Each read
/// is an independent request and shares no mutable state with another, which is
/// what lets the boundary suite's concurrency cases run against it unchanged.
class HttpAssetReader final : public AssetReader {
public:
    ~HttpAssetReader() override;

    const AssetMetadata& Metadata() const override;

    /// Reads up to `size` bytes at `offset`. See `AssetReader::Read` for the
    /// semantics, which this backend implements identically to the local one --
    /// the same EOF boundary, the same overflow rule, and the same refusal to
    /// return a hole.
    ///
    /// Every request carries the conditional guard captured at open, where the
    /// captured validator admits one. A response that contradicts that identity
    /// fails the read with `AssetChanged` and returns no bytes: the bytes may
    /// span two revisions, and reporting them as read invites exactly the
    /// composition the guarantee exists to prevent.
    ReadResult Read(std::uint64_t offset, void* dst, std::size_t size) override;

    /// This reader's counters, readable while it lives. They fold into the
    /// process aggregate when it closes.
    ///
    /// On the concrete backend rather than on `AssetReader`, for the reason the
    /// local backend gives: metrics are not part of the read contract, and
    /// widening that contract to carry them would put an accessor on the one
    /// interface this project keeps deliberately narrow.
    const ReaderMetrics& Metrics() const noexcept;

    /// The same counters, writable, for a decorator that composes this reader
    /// into a stack and folds the whole stack once. See the local backend's
    /// note and `ReaderMetrics::AbsorbTransport`.
    ReaderMetrics& Metrics() noexcept;

private:
    class Impl;
    explicit HttpAssetReader(std::unique_ptr<Impl> impl);

    friend struct HttpReaderFactory;

    std::unique_ptr<Impl> _impl;
};

/// The result of opening a remote asset, typed to the concrete reader.
struct HttpOpenResult {
    std::unique_ptr<HttpAssetReader> reader;  ///< Null exactly when `status` fails.
    Status status;
};

/// Opens `url`.
///
/// Performs one metadata request -- a `HEAD`, plus whatever redirect hops and
/// bounded retries that `HEAD` costs -- and never reads content. A reader is
/// never returned in a state where its size or range support is unknown.
///
/// `url` is an absolute `http` or `https` URI. Normalizing an identifier into
/// one, and anchoring a relative reference against a layer, is the resolver's
/// job and not this module's (RESOLVER.md §2.1).
///
/// Open fails with `RangeNotSupported` when the server does not advertise byte
/// ranges. It cannot fail there for a server that advertises them and then
/// ignores them, because catching that would take a second round trip that
/// ASSET_READER.md §2 forbids; that server's first read fails with the same
/// code instead, and ADR-0002 makes both terminal.
HttpOpenResult Open(const std::string& url);
HttpOpenResult Open(const std::string& url, const HttpOptions& options);

/// The same open, in the shape every backend returns. This is what the shared
/// boundary suite and, from `v0.2.0`, the resolver call.
OpenResult OpenAsset(const std::string& url);
OpenResult OpenAsset(const std::string& url, const HttpOptions& options);

}  // namespace http
}  // namespace usdasset

#endif  // USDASSETHTTP_HTTPASSETREADER_H
