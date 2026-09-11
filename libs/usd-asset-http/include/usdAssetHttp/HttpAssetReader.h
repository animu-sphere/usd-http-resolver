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

/// What kind of network an address belongs to, for the destination policy.
///
/// Four classes, and only the four a decision about request forgery turns on
/// (§10.2 of the design policy). An address is classified by its numeric value
/// and never by a name, so a hostname that resolves to a loopback address is a
/// loopback destination however it is spelled.
///
///   Loopback    127.0.0.0/8, 0.0.0.0/8, ::1, and the unspecified address ::,
///               which a connect on the common stacks treats as this host
///   LinkLocal   169.254.0.0/16 and fe80::/10 -- which is where cloud
///               instance-metadata services live, 169.254.169.254 among them
///   Private     10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, the shared address
///               space 100.64.0.0/10, the unique-local fc00::/7, and the
///               deprecated site-local fec0::/10
///   Public      everything else
///
/// An IPv6 address that carries an IPv4 one -- mapped (`::ffff:a.b.c.d`),
/// compatible (`::a.b.c.d`), or behind the NAT64 well-known prefix
/// (`64:ff9b::a.b.c.d`) -- is the class of the address it carries, because
/// that is where the connection ends up.
enum class AddressClass {
    Public,
    Private,
    Loopback,
    LinkLocal,
};

/// The stable lowercase spelling of a class: `public`, `private`, `loopback`,
/// `link-local`. These are also the words the resolver's configuration takes,
/// so that a message and the setting that would change it use one vocabulary.
const char* AddressClassName(AddressClass addressClass) noexcept;

/// Which classes of address a reader may connect to.
///
/// §10.2 of the design policy: an identifier can arrive from a layer the user
/// did not author, which makes a resolver a request-forgery primitive unless
/// its reach is bounded by declared policy rather than by whatever the host's
/// network happens to allow. The default is that declaration, and it is a
/// deliberate middle rather than either end:
///
///   public, private, loopback   permitted -- `http` is registered for local
///                               fixture servers and intranet hosts
///                               (RESOLVER.md §1), and refusing either would
///                               break the uses the scheme exists for
///   link-local                  refused -- nothing legitimate serves USD from
///                               a link-local address, and the one thing that
///                               reliably lives there is the credential
///                               endpoint of a cloud instance
///
/// A deployment that wants a narrower reach says so; a render farm that must
/// never reach its own intranet from a layer it did not author sets `public`
/// alone.
///
/// Judged twice, because each judgement covers what the other cannot. The
/// address a connection is made to is judged at connect time, which is what
/// makes the policy hold for a name that resolves to a refused address and for
/// every spelling of an address a system resolver accepts. A literal address in
/// the URL is also judged before any request is issued, at every redirect hop,
/// which is what makes the policy hold through a proxy -- where the address this
/// process connects to is the proxy's, and the destination is the proxy's to
/// resolve.
struct DestinationPolicy {
    bool publicAddresses = true;
    bool privateNetworks = true;
    bool loopback = true;
    bool linkLocal = false;

    bool Permits(AddressClass addressClass) const noexcept;

    bool operator==(const DestinationPolicy& other) const noexcept {
        return publicAddresses == other.publicAddresses &&
               privateNetworks == other.privateNetworks &&
               loopback == other.loopback && linkLocal == other.linkLocal;
    }
    bool operator!=(const DestinationPolicy& other) const noexcept {
        return !(*this == other);
    }
};

/// Transport policy, all of it bounded.
///
/// These are the knobs §10 of the design policy requires to exist -- "bound
/// redirect chains", "cap retries and total time" -- and they are parameters
/// rather than constants so that a test can make a deadline elapse in
/// milliseconds instead of waiting out a production one. The defaults are what
/// a caller that passes nothing gets, and they are the values the release is
/// measured with.
///
/// Nothing here reads the environment. Resolving these from a deployment's
/// settings is the resolver's configuration surface (CONFIGURATION.md), which
/// is a policy about where values come from; this module is a mechanism, and a
/// caller of it passes values or takes the defaults.
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

    /// Which classes of address this reader may connect to, at the first hop
    /// and at every redirect. A refusal is `AccessDenied` and issues no
    /// request: it is a configuration decision, which is what `403` is too,
    /// and a caller does the same thing about both.
    DestinationPolicy destinations;

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
