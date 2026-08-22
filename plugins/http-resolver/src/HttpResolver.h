// SPDX-License-Identifier: Apache-2.0
//
// The `ArResolver` for `http` and `https`: RESOLVER.md.
//
// It is a URI-scheme resolver and not the primary one. Installing this bundle
// never changes how a local asset opens -- that is contract rather than
// detail, and it is why the registration in `plugInfo.json` names two schemes
// instead of claiming the resolver chair.
//
// What lives here is identifier creation, resolution, and asset opening. No
// byte handling and no request assembly: those are the backend's, behind
// `usdAssetHttp`, and the moment this file starts interpreting a response the
// boundary in WORKSPACE.md §4 has moved.

#ifndef USDHTTPRESOLVER_HTTPRESOLVER_H
#define USDHTTPRESOLVER_HTTPRESOLVER_H

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pxr/pxr.h"
#include "pxr/usd/ar/assetInfo.h"
#include "pxr/usd/ar/resolvedPath.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/ar/timestamp.h"

#include "usdAssetCache/CacheOptions.h"
#include "usdAssetHttp/HttpAssetReader.h"
#include "usdAssetIo/AssetReader.h"

PXR_NAMESPACE_OPEN_SCOPE

class ArAsset;
class ArWritableAsset;

class HttpResolver final : public ArResolver {
public:
    HttpResolver();
    ~HttpResolver() override;

protected:
    /// RESOLVER.md §2.1 and §2.2: normalization, and RFC 3986 anchoring
    /// against a remote layer.
    std::string _CreateIdentifier(
        const std::string& assetPath,
        const ArResolvedPath& anchorAssetPath) const override;

    /// Empty, always. Assets are immutable and are published at new paths;
    /// RESOLVER.md §5 and §6 of the design policy.
    std::string _CreateIdentifierForNewAsset(
        const std::string& assetPath,
        const ArResolvedPath& anchorAssetPath) const override;

    /// Existence, which for a remote asset costs a round trip.
    ///
    /// The metadata request is performed once per identifier and its result --
    /// the open reader, bound to the revision it found -- is retained for the
    /// `_OpenAsset` that follows, so that opening a resolved asset does not
    /// repeat it (RESOLVER.md §2.3).
    ///
    /// Absence and failure are different answers. A `404` returns an empty
    /// path and posts nothing: the asset is not there, which is a fact a
    /// consumer asked for. Anything else -- a timeout, a reset, a malformed
    /// response -- also returns an empty path, because `ArResolver` has no
    /// other channel, but posts its `HTTPxxx` diagnostic first. Reporting a
    /// timeout as "file not found" sends every consumer down the wrong path.
    ArResolvedPath _Resolve(const std::string& assetPath) const override;

    /// Empty, always. See `_CreateIdentifierForNewAsset`.
    ArResolvedPath _ResolveForNewAsset(
        const std::string& assetPath) const override;

    /// The `ArAsset` over the reader captured by `_Resolve`, or over a fresh
    /// open when this identifier was not resolved through this process.
    std::shared_ptr<ArAsset> _OpenAsset(
        const ArResolvedPath& resolvedPath) const override;

    /// Null, with `HTTP010`. Writing is unsupported, permanently.
    std::shared_ptr<ArWritableAsset> _OpenAssetForWrite(
        const ArResolvedPath& resolvedPath,
        WriteMode writeMode) const override;

    /// False, with a reason. Never a silent no.
    bool _CanWriteAssetToPath(const ArResolvedPath& resolvedPath,
                              std::string* whyNot) const override;

    /// The identity a consumer decides its own cache reuse from: RESOLVER.md
    /// §3, and the four neutral values `Identity.h` projects.
    ///
    /// Answered from the open this process already performed for this
    /// identifier, and not from a fresh metadata request, because those are
    /// different answers: the identity a consumer needs is the identity of the
    /// bytes it is holding, and a `HEAD` issued now can describe a revision
    /// published after the asset it is asking about was opened. An identifier
    /// with a resolved path that nothing has opened is opened here, and
    /// retained, so that the request it costs is the one the `_OpenAsset` that
    /// follows would have made.
    ///
    /// Two things it will not do, and both are about a failing origin. It does
    /// not open an identifier whose `resolvedPath` is empty: an empty resolved
    /// path is a resolution that failed or never happened, and asset info must
    /// not be the call that discovers a `503` -- that discovery costs a round
    /// trip, and `_Resolve` has just paid for it. And it posts no diagnostic of
    /// its own: this is a question about identity rather than an operation on
    /// the asset, the operation that follows reports its own failure, and one
    /// failure rendered twice is the noise DIAGNOSTICS.md §3 exists to avoid.
    ArAssetInfo _GetAssetInfo(const std::string& assetPath,
                              const ArResolvedPath& resolvedPath) const override;

    /// Invalid, always, and deliberately.
    ///
    /// A validator is not a time. `Last-Modified` sometimes is, but reading it
    /// as one means parsing an HTTP construct above the backend that captured
    /// it, which §7.1 of ASSET_READER.md places below this layer -- and a
    /// strong `ETag`, which is the validator this resolver most wants to
    /// publish, carries no time at all.
    ///
    /// Synthesizing one anyway is worse than not answering, and not by a
    /// little. A consumer that finds no token in `GetAssetInfo` falls back to
    /// this timestamp and builds an identity out of it, so a fabricated number
    /// would manufacture exactly the durable identity that a weak or absent
    /// validator is not allowed to have. The identity surface is `GetAssetInfo`
    /// alone, and this returns the invalid timestamp that says so.
    ///
    /// What an invalid timestamp costs is a reload: `SdfLayer::Reload` re-reads
    /// a layer whose timestamp is invalid rather than comparing it. That is a
    /// request, and never a wrong answer.
    ArTimestamp _GetModificationTimestamp(
        const std::string& assetPath,
        const ArResolvedPath& resolvedPath) const override;

    /// The extension, ignoring the query string.
    ///
    /// Overridden because the default implementation takes the text after the
    /// last `.` of the whole path, and for a pre-signed URL that is
    /// `usda?X-Amz-Signature=...`. No file format matches it, so the layer
    /// cannot be identified -- a failure that looks like an unsupported format
    /// and is in fact a resolver bug.
    std::string _GetExtension(const std::string& assetPath) const override;

private:
    /// One identifier's in-flight or completed open.
    ///
    /// The reader is the point. `_Resolve` has to open the asset in order to
    /// answer the existence question at all, and throwing that open away would
    /// make every resolved asset cost two metadata requests.
    struct _Opened {
        std::mutex mutex;  ///< Single-flight: the second thread waits, and
                           ///< then finds the first thread's answer.
        bool opened = false;
        usdasset::http::HttpOpenResult result;

        /// Whether the open succeeded, and what it found.
        ///
        /// Separate from `result.reader` because the reader leaves: a
        /// concurrent `_OpenAsset` takes it, and an entry with a null reader
        /// and an `Ok` status is then indistinguishable from a failed open by
        /// anything that looks at `result` alone. It is not the same answer --
        /// the asset exists, and its identity is known -- so the answer is kept
        /// here, where handing the reader out cannot erase it.
        bool succeeded = false;
        usdasset::AssetMetadata metadata;
    };

    /// Finds or creates the entry for `identifier`. The table lock is held for
    /// the lookup and never across a request.
    std::shared_ptr<_Opened> _GetOrCreate(const std::string& identifier) const;

    /// Removes an entry and returns it, so that a reader is handed out exactly
    /// once. A second `_OpenAsset` for one identifier opens again rather than
    /// sharing a reader that is already bound to a revision somebody else is
    /// mid-composition on.
    std::shared_ptr<_Opened> _Take(const std::string& identifier) const;

    /// Removes `identifier`'s entry, but only if it is still `entry`.
    ///
    /// By identity rather than by name, because a failed resolve is forgotten
    /// and two threads can be holding one failed entry: the second arrives after
    /// the first has removed it and a third has opened the identifier
    /// successfully, and erasing by key would discard that third thread's
    /// reader.
    void _Forget(const std::string& identifier,
                 const std::shared_ptr<_Opened>& entry) const;

    /// What one identifier's most recent successful open discovered.
    ///
    /// Kept because `_Resolve` hands its reader out exactly once and a consumer
    /// asks for asset info *after* opening the asset, by which time the reader
    /// that knows the answer belongs to the consumer. Remembering the metadata
    /// costs a few hundred bytes and keeps the answer describing the bytes the
    /// consumer actually holds.
    struct _Identity {
        usdasset::AssetMetadata metadata;
    };

    /// The validator one identifier has been seen with, and whether it has ever
    /// been seen with two.
    ///
    /// This is the record a contradiction is detected against, and it is why it
    /// is not the same structure as `_identities`. The two have different
    /// jobs and therefore different lifetimes: `_identities` caches an *answer*
    /// and may be dropped, because dropping it costs a metadata request; this
    /// caches the *question* -- has this identifier changed underneath us -- and
    /// may never be dropped, because dropping it makes the next open of an
    /// asset that has already moved look like the first one, and publish a
    /// reusable identity for a revision a live consumer is not holding.
    struct _Fingerprint {
        usdasset::Validator validator;
        bool seen = false;  ///< An absent validator is a legal value, so
                            ///< presence needs its own bit.
        bool contradicted = false;
    };

    /// Records `metadata` as the identity of `identifier`, and reports whether
    /// this identifier has ever contradicted itself in this process.
    ///
    /// A contradiction is a republish underneath a running process: two opens
    /// of one identifier that captured two different validators. It is
    /// remembered permanently, in `_fingerprints`, because the consequence is
    /// permanent -- see `PublishIdentity` -- and because forgetting it is
    /// indistinguishable, from the inside, from the asset never having moved.
    bool _RememberIdentity(const std::string& identifier,
                           const usdasset::AssetMetadata& metadata) const;

    /// The remembered identity for `identifier`, if there is one.
    bool _KnownIdentity(const std::string& identifier,
                        usdasset::AssetMetadata* metadata,
                        bool* contradicted) const;

    /// The remembered identity, or a fresh open's, or nothing.
    ///
    /// `mayOpen` decides whether an identifier nothing has opened is worth a
    /// round trip. The open it then performs is the retained kind: it goes
    /// through the same table `_Resolve` fills, so a following `_OpenAsset`
    /// finds the reader rather than issuing a second metadata request.
    ///
    /// A failure is silent here. It is still not retained -- the entry is
    /// forgotten exactly as `_Resolve` forgets one -- but nothing is posted:
    /// see `_GetAssetInfo`.
    bool _IdentityFor(const std::string& identifier,
                      bool mayOpen,
                      usdasset::AssetMetadata* metadata,
                      bool* contradicted) const;

    /// Unclaimed opens the table will hold before dropping the oldest.
    ///
    /// A bound rather than a policy: an entry holds an open reader, a resolve
    /// that is never followed by an open is legal and normal, and an unbounded
    /// table would accumulate one reader per asset a long-lived host ever
    /// probed for. Dropping the oldest costs a later metadata request and
    /// never costs correctness.
    static constexpr std::size_t kMaxRetainedOpens = 64;

    usdasset::http::HttpOptions _options;

    /// The block policy every asset this resolver opens is decorated with.
    ///
    /// Resolved once, at construction, from the environment. The blocks
    /// themselves live in the process-wide store rather than here, because the
    /// budget is process-wide and shared across assets (CACHE.md section 7) and
    /// a store per resolver would not be one budget.
    usdasset::cache::CacheOptions _cacheOptions;

    /// Remembered identities the process will hold before dropping the oldest.
    ///
    /// Larger than `kMaxRetainedOpens`, and for a different reason: an entry
    /// here holds no reader and no connection, only a metadata struct, and what
    /// is lost when one is dropped is the ability to answer `GetAssetInfo`
    /// without a request. Dropping one costs that request and nothing else,
    /// because the record a change is detected against is `_fingerprints`,
    /// which is not this table and is never dropped.
    static constexpr std::size_t kMaxRememberedIdentities = 512;

    mutable std::mutex _tableMutex;
    mutable std::unordered_map<std::string, std::shared_ptr<_Opened>> _table;
    mutable std::deque<std::string> _order;  ///< Insertion order, for eviction.

    mutable std::mutex _identityMutex;
    mutable std::unordered_map<std::string, _Identity> _identities;
    mutable std::deque<std::string> _identityOrder;

    /// One fingerprint per identifier this process has opened, kept for the
    /// life of the process.
    ///
    /// Unbounded, deliberately, and it is the one structure here that is. A
    /// bound on this table is a bound on how far back a republish can be
    /// noticed: past it, an asset that has already moved looks like an asset
    /// being opened for the first time, and asset info publishes a reusable
    /// token for a revision that some consumer is not the holder of. That is
    /// the failure this whole surface exists to prevent, and it is not worth
    /// trading for a table of validators -- an `ETag` and a URL per asset the
    /// process has actually opened, which is the same order as the identifiers
    /// a host is holding anyway.
    ///
    /// The bounded table above it is what keeps that affordable: the expensive
    /// part of an identity is the metadata and the answer, and those are the
    /// part that may be dropped.
    mutable std::unordered_map<std::string, _Fingerprint> _fingerprints;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // USDHTTPRESOLVER_HTTPRESOLVER_H
