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
#include <vector>

#include "usdAssetIo/AssetReader.h"
#include "usdAssetIo/Metrics.h"

namespace usdasset {
namespace http {

/// What kind of network an address belongs to, for the destination policy.
///
/// Five classes, and only the five a decision about request forgery turns on
/// (§10.2 of the design policy). An address is classified by its numeric value
/// and never by a name, so a hostname that resolves to a loopback address is a
/// loopback destination however it is spelled.
///
///   Metadata    the well-known instance-metadata and credential endpoints of
///               the major clouds, wherever they sit: 169.254.169.254,
///               169.254.170.2, 169.254.170.23, 169.254.0.23,
///               100.100.100.200, 168.63.129.16, fd00:ec2::254, and
///               fd00:ec2::23. Checked before the ranges below, because two of
///               them are inside the private ones and one is public
///   Loopback    127.0.0.0/8, 0.0.0.0/8, ::1, and the unspecified address ::,
///               which a connect on the common stacks treats as this host
///   LinkLocal   169.254.0.0/16 and fe80::/10, apart from the metadata
///               addresses in them
///   Private     10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, the shared address
///               space 100.64.0.0/10, the unique-local fc00::/7, and the
///               deprecated site-local fec0::/10, apart from the metadata
///               addresses in them
///   Public      everything else
///
/// `Metadata` is a class of its own rather than a part of `LinkLocal` because
/// the ranges do not contain it. An address range is a statement about routing,
/// and the property a request-forgery policy cares about -- that answering
/// this request hands a stranger the instance's credentials -- follows the
/// provider rather than the range: AWS's IPv6 endpoint is unique-local, and
/// Alibaba's sits in the shared address space.
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
    Metadata,
};

/// The stable lowercase spelling of a class: `public`, `private`, `loopback`,
/// `link-local`, `metadata`. These are also the words the resolver's
/// configuration takes, so that a message and the setting that would change it
/// use one vocabulary.
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
///   link-local, metadata        refused -- nothing legitimate serves USD from
///                               either, and a metadata address is where a
///                               cloud instance hands out its credentials
///
/// A deployment that wants a narrower reach says so; a render farm that must
/// never reach its own intranet from a layer it did not author sets `public`
/// alone. Permitting `link-local` does not permit `metadata`: the second is
/// never a side effect of the first.
///
/// Judged three times, because each judgement covers what the others cannot.
/// The address a connection is made to is judged at connect time, which is
/// what makes the policy hold for a name that resolves to a refused address.
/// The host the client will actually send is judged before each request, as
/// the client itself parses it -- decimal, octal, and hexadecimal spellings of
/// an address, and percent-encoded ones, normalized the way it will normalize
/// them -- which is what makes the policy hold through a proxy, where the
/// address this process connects to is the proxy's and the destination is the
/// proxy's to resolve. And a literal in the URL is judged at every redirect hop
/// before any transport sees it, so that the rule does not depend on which
/// client is underneath.
struct DestinationPolicy {
    bool publicAddresses = true;
    bool privateNetworks = true;
    bool loopback = true;
    bool linkLocal = false;
    bool metadata = false;

    bool Permits(AddressClass addressClass) const noexcept;

    /// True when every class `other` permits, this permits too. A
    /// destination reached under `other` is reachable under this.
    bool Covers(const DestinationPolicy& other) const noexcept {
        return (publicAddresses || !other.publicAddresses) &&
               (privateNetworks || !other.privateNetworks) &&
               (loopback || !other.loopback) && (linkLocal || !other.linkLocal) &&
               (metadata || !other.metadata);
    }

    bool operator==(const DestinationPolicy& other) const noexcept {
        return publicAddresses == other.publicAddresses &&
               privateNetworks == other.privateNetworks &&
               loopback == other.loopback && linkLocal == other.linkLocal &&
               metadata == other.metadata;
    }
    bool operator!=(const DestinationPolicy& other) const noexcept {
        return !(*this == other);
    }
};

/// One header a `RequestInterceptor` adds to a request.
struct RequestHeader {
    std::string name;
    std::string value;
};

/// What a `RequestInterceptor` is told about the request it is asked about.
///
/// Enough to scope a credential and to sign a request, and nothing about the
/// asset: no size, no validator, no bytes. An interceptor decides who is
/// asking, never what the answer is.
struct InterceptedRequest {
    /// `GET` or `HEAD`.
    std::string method;

    /// The URL this request is sent to, with any userinfo removed. The query is
    /// kept verbatim, because a request signer needs exactly what goes on the
    /// wire.
    std::string url;

    /// The origin, split out so that an interceptor scoping a credential to one
    /// does not have to parse `url` to do it. `scheme` and `host` are
    /// lowercase, `host` is bracketed for an IPv6 literal, and `port` is the
    /// effective port -- the scheme's default when the URL names none.
    std::string scheme;
    std::string host;
    int port = 0;

    /// Redirect hops already followed to reach this URL within the current
    /// open or read. `0` for the URL the operation started at.
    int redirectHop = 0;

    /// 1 for the first time this exact request is issued; higher for a retry.
    /// A provider whose token expired between attempts gets to mint another.
    int attempt = 1;
};

/// The authentication seam: §4.3 of the design policy.
///
///   request interceptor     <- this
///   credential provider     <- an implementation of it; none ships here
///   environment / session   <- where a provider gets what it presents
///
/// A credential never enters this module's API as a value. It enters as an
/// object that is asked, request by request, which headers to add -- and
/// everything that would make a credential leak is the backend's to refuse
/// rather than the interceptor's to remember:
///
///   origin scope     consulted only for requests to the origin the asset was
///                    opened at: same scheme, host, and effective port. A
///                    redirect to any other origin is followed with no
///                    intercepted header, as browsers and libcurl both do for
///                    `Authorization`, and so is a read whose open was
///                    redirected there. An `https` to `http` hop is already
///                    refused outright
///   policy first     the destination policy judges a hop before the
///                    interceptor is asked, so a hop the policy refuses never
///                    costs a credential lookup, and never carries one
///   protocol owned   a header this backend's correctness depends on --
///                    `Range`, `If-Range`, the other conditionals, `Host`,
///                    framing, encoding, connection management, `User-Agent`
///                    -- cannot be set, and a header that could split the
///                    request cannot be built (`IsInterceptableHeader`)
///   bounded          at most `kMaxInterceptedHeaderBytes` of added header per
///                    request
///   never repeated   no added value reaches a `Status` message, a metrics
///                    dump, a cache key, an identity, or anything persisted.
///                    A refused header is named in a message only when its
///                    name is a well-formed reserved one; a malformed name may
///                    be a value pasted into the wrong field
///
/// Called on whatever thread issued the request, concurrently, for the life of
/// every reader opened with it -- so it must be thread-safe and must outlive
/// none of them, which the `shared_ptr` in `HttpOptions` takes care of.
///
/// Returning false declines the request: it is not sent, and the operation
/// fails with `AccessDenied` and is not retried, because a retry would ask the
/// same provider the same question. An exception is a decline. The backend
/// never repeats anything the interceptor said about why.
class RequestInterceptor {
public:
    virtual ~RequestInterceptor() = default;

    /// Appends to `headersOut`, which arrives empty, the headers to add to
    /// `request`. Returns false to decline it.
    virtual bool Intercept(const InterceptedRequest& request,
                           std::vector<RequestHeader>* headersOut) const = 0;
};

/// The most bytes of header one interception may add, counted as
/// `name: value\r\n`. A bearer token is tens of bytes and a SigV4 header set a
/// few hundred; common origins refuse a request whose whole header block passes
/// 8 KiB, so 16 KiB is a bound on a defect rather than a limit on a credential.
constexpr std::size_t kMaxInterceptedHeaderBytes = 16 * 1024;

/// Whether an interceptor may add a header named `name` with value `value`.
///
/// The name is an RFC 9110 token and not one the protocol owns. The value is
/// non-empty and carries no control character but horizontal tab -- no CR, no
/// LF, no NUL -- so that it cannot end the header it is in and begin another.
/// An empty value is refused because the client underneath treats `Name:` with
/// nothing after it as an instruction to remove a header it would otherwise
/// send, which is not something a credential provider has any business doing.
///
/// Exposed so that a provider can check what it builds, and so that the rule
/// is a function with a test rather than a sentence.
bool IsInterceptableHeader(const std::string& name, const std::string& value) noexcept;

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

    /// Asked for headers to add to every request to the origin the asset is
    /// opened at -- the open's `HEAD`, every range `GET`, every retry, and
    /// every same-origin redirect hop. Null, the default, adds nothing: public
    /// HTTP is the target, and no provider ships with this module.
    ///
    /// A reader keeps this for its lifetime, as it keeps every other option. A
    /// caller that hands out readers opened under one interceptor to callers
    /// configured with another is handing out one caller's credentials.
    std::shared_ptr<const RequestInterceptor> interceptor;
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
