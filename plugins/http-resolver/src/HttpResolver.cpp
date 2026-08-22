// SPDX-License-Identifier: Apache-2.0

#include "HttpResolver.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pxr/base/vt/dictionary.h"
#include "pxr/base/vt/value.h"
#include "pxr/usd/ar/defineResolver.h"
#include "pxr/usd/ar/writableAsset.h"

#include "Configuration.h"
#include "Diagnostics.h"
#include "Identifier.h"
#include "Identity.h"
#include "Report.h"
#include "ResolvedAsset.h"

#include "usdAssetCache/BlockCache.h"
#include "usdAssetCache/CachedAssetReader.h"
#include "usdAssetCache/DiskBlockStore.h"
#include "usdAssetIo/Diagnostics.h"

PXR_NAMESPACE_OPEN_SCOPE

// Registers the type named in plugInfo.json. Both schemes are served by one
// type (RESOLVER.md §1): `https` is the expected one and `http` exists for
// local fixture servers and intranet hosts.
AR_DEFINE_RESOLVER(HttpResolver, ArResolver);

namespace {

/// The status a legal-but-unimplemented operation fails with. Writing is the
/// only one in this release.
usdasset::Status UnsupportedWrite() {
    return usdasset::Status::Error(
        usdasset::StatusCode::Unsupported,
        "assets are read-only; publish a new revision at a new path");
}

}  // namespace

HttpResolver::HttpResolver() {
    std::vector<usdhttpresolver::ConfigurationProblem> problems;
    const usdhttpresolver::ResolverConfiguration configuration =
        usdhttpresolver::ConfigurationFromEnvironment(&problems);
    _options = configuration.transport;
    _cacheOptions = configuration.cache.Normalized();

    // The budget belongs to the process store rather than to this resolver, so
    // it is applied where it lives. Refused only when something is already bound
    // into that store, which for a resolver constructed by `Plug` before any
    // stage opens cannot happen -- and if it somehow does, the store keeps the
    // budget it has and says so rather than being rebuilt underneath a live
    // reader.
    if (!usdasset::cache::BlockCache::ConfigureProcess(_cacheOptions)) {
        problems.push_back(
            {"USD_HTTP_RESOLVER_CACHE_BUDGET",
             std::to_string(_cacheOptions.budgetBytes),
             "the process block store was already in use; its budget and block "
             "size were left as they were"});
    }

    // The persistent tier, second, because it is the one that can fail for a
    // reason outside this process: a directory that cannot be created, or one
    // this user may not write to. Off unless a directory was named, and off
    // again if the one that was named could not be prepared -- reported either
    // way, because a cache that silently did not turn on is a cache somebody
    // spends an afternoon looking for.
    if (!configuration.persistence.directory.empty() &&
        !usdasset::cache::DiskBlockStore::ConfigureProcess(configuration.persistence)) {
        problems.push_back(
            {"USD_HTTP_RESOLVER_PERSISTENT_CACHE_DIR",
             configuration.persistence.directory,
             "not a directory this process can create and write"});
    }

    for (const usdhttpresolver::ConfigurationProblem& problem : problems) {
        // At first use, per CONFIGURATION.md §2, which for a process-global
        // surface is when the resolver is constructed. A typo that silently
        // does nothing is worse than one that is reported.
        usdhttpresolver::ReportConfigurationProblem(problem);
    }
}

HttpResolver::~HttpResolver() = default;

std::string HttpResolver::_CreateIdentifier(
    const std::string& assetPath,
    const ArResolvedPath& anchorAssetPath) const {
    return usdhttpresolver::CreateIdentifier(assetPath,
                                             anchorAssetPath.GetPathString());
}

std::string HttpResolver::_CreateIdentifierForNewAsset(
    const std::string& assetPath,
    const ArResolvedPath& anchorAssetPath) const {
    (void)assetPath;
    (void)anchorAssetPath;
    return std::string();
}

ArResolvedPath HttpResolver::_Resolve(const std::string& assetPath) const {
    // Normalized again rather than assumed: `_Resolve` is reachable with a path
    // that never went through `_CreateIdentifier`, and resolving two spellings
    // of one asset to two paths would give it two readers and two revisions.
    const std::string identifier =
        usdhttpresolver::CreateIdentifier(assetPath, std::string());
    if (identifier.empty()) return ArResolvedPath();

    const std::shared_ptr<_Opened> entry = _GetOrCreate(identifier);

    std::lock_guard<std::mutex> lock(entry->mutex);
    if (!entry->opened) {
        entry->result = usdasset::http::Open(identifier, _options);
        entry->opened = true;
        if (entry->result.reader) {
            // Copied off the reader while it is still here. The reader leaves
            // -- `_OpenAsset` takes it, possibly on another thread and possibly
            // before this line is reached again -- and what it knew about the
            // asset's identity must not leave with it.
            entry->metadata = entry->result.reader->Metadata();
            entry->succeeded = true;
        }
    }

    if (entry->succeeded) {
        // `succeeded` and not `result.reader`: a concurrent `_OpenAsset` may
        // already have taken the reader out of this entry, and an asset whose
        // reader has been handed to somebody still exists.
        //
        // Remembered here rather than at the open in `_OpenAsset`, because that
        // reader is handed out once and the consumer that asks for its identity
        // asks after it is gone. RESOLVER.md §3.
        _RememberIdentity(identifier, entry->metadata);
        return ArResolvedPath(identifier);
    }

    // A failure is not retained. Caching it would turn a server that was
    // restarting into an asset that does not exist for the rest of the process.
    //
    // Forgotten by identity and not by name: a second thread that was waiting on
    // this same entry's mutex arrives here after the first has already removed
    // it and a third has opened the identifier successfully, and erasing by key
    // would throw away that third thread's reader.
    const usdasset::Status status = entry->result.status;
    _Forget(identifier, entry);

    if (status.code != usdasset::StatusCode::NotFound) {
        usdhttpresolver::Report(status, identifier);
    }
    return ArResolvedPath();
}

ArResolvedPath HttpResolver::_ResolveForNewAsset(
    const std::string& assetPath) const {
    (void)assetPath;
    return ArResolvedPath();
}

std::shared_ptr<ArAsset> HttpResolver::_OpenAsset(
    const ArResolvedPath& resolvedPath) const {
    const std::string identifier = usdhttpresolver::CreateIdentifier(
        resolvedPath.GetPathString(), std::string());
    if (identifier.empty()) return nullptr;

    std::unique_ptr<usdasset::http::HttpAssetReader> reader;

    if (const std::shared_ptr<_Opened> entry = _Take(identifier)) {
        std::lock_guard<std::mutex> lock(entry->mutex);
        reader = std::move(entry->result.reader);
    }

    if (!reader) {
        // Either nothing resolved this identifier in this process, or the
        // reader `_Resolve` captured has already been handed to somebody.
        usdasset::http::HttpOpenResult result =
            usdasset::http::Open(identifier, _options);
        if (!result.reader) {
            usdhttpresolver::Report(result.status, identifier);
            return nullptr;
        }
        reader = std::move(result.reader);
    }

    // Both paths, and not only the fresh open. A reader taken from the table
    // was already recorded by whatever put it there, and recording it again is
    // free; a reader opened here may never have been resolved through this
    // process at all, and this is the only point at which its identity is
    // known.
    _RememberIdentity(identifier, reader->Metadata());

    // Captured before the reader is moved from, and valid for as long as the
    // reader is: it is a member of the reader's own implementation, and the
    // decorator below owns the reader for the whole life of the asset.
    //
    // The *transport's* counter set, deliberately, and not the decorated
    // stack's. What this pointer is for is `HTTP101`, a retry that succeeded
    // and cost the latency somebody is investigating, and a retry is a
    // transport event: the cache neither issues one nor sees one.
    usdasset::ReaderMetrics* const metrics = &reader->Metrics();

    // The block cache goes on here rather than in `_Resolve`, because
    // `_Resolve` only has to establish that the asset exists and this is where
    // bytes start being asked for. The wrap binds into the process store by
    // identity -- the resolved identifier and the validator the reader captured
    // at open -- so two `ArAsset`s over one revision share blocks, and two over
    // two revisions never do (CACHE.md section 6).
    //
    // `WrapAsset` and not `Wrap`, which is what this comment used to say while
    // the line below said otherwise. The difference is the `supportsRandomAccess`
    // guard: `Wrap` returns a `CachedAssetReader` and therefore cannot decline
    // to decorate, and a reader that cannot seek would store the one block it
    // managed to read and miss forever after. ADR-0002 makes range support a
    // hard error at open, so every reader that reaches this line supports it and
    // the guard has never fired -- which is exactly how long a missing guard
    // stays invisible.
    usdasset::OpenResult opened;
    opened.reader = std::unique_ptr<usdasset::AssetReader>(reader.release());
    usdasset::OpenResult cached = usdasset::cache::WrapAsset(
        std::move(opened), metrics, _cacheOptions, nullptr);
    if (!cached.reader) {
        usdhttpresolver::Report(cached.status, identifier);
        return nullptr;
    }
    return std::make_shared<HttpResolvedAsset>(std::move(cached.reader), metrics);
}

std::shared_ptr<ArWritableAsset> HttpResolver::_OpenAssetForWrite(
    const ArResolvedPath& resolvedPath, WriteMode writeMode) const {
    (void)writeMode;
    usdhttpresolver::Report(UnsupportedWrite(), resolvedPath.GetPathString());
    return nullptr;
}

bool HttpResolver::_CanWriteAssetToPath(const ArResolvedPath& resolvedPath,
                                        std::string* whyNot) const {
    (void)resolvedPath;
    if (whyNot != nullptr) {
        *whyNot = usdhttpresolver::Render(UnsupportedWrite(), std::string());
    }
    return false;
}

ArAssetInfo HttpResolver::_GetAssetInfo(
    const std::string& assetPath, const ArResolvedPath& resolvedPath) const {
    // The resolved path when there is one, because that is the identifier the
    // open was performed under; the asset path otherwise, because `ArResolver`
    // permits asking about a path that has not been resolved.
    const bool resolved = !resolvedPath.GetPathString().empty();
    const std::string source = resolved ? resolvedPath.GetPathString()
                                        : assetPath;
    const std::string identifier =
        usdhttpresolver::CreateIdentifier(source, std::string());

    ArAssetInfo info;
    if (identifier.empty()) return info;

    usdasset::AssetMetadata metadata;
    bool contradicted = false;
    // An identifier this process has opened is answered either way. What
    // `resolved` decides is whether one it has *not* opened is worth a round
    // trip: an empty resolved path is a resolution that failed or never
    // happened, and asset info is not the call that should discover a dead
    // origin -- for a layer being reloaded against one, that is a second
    // identical round trip behind the one `_Resolve` has just paid for.
    if (!_IdentityFor(identifier, resolved, &metadata, &contradicted)) {
        return info;
    }

    const usdhttpresolver::PublishedIdentity published =
        usdhttpresolver::PublishIdentity(metadata, contradicted);

    // `version` is the field a consumer reads first, and it travels with
    // nothing beside it -- no stability, no explanation. A token found there is
    // treated as an identity fit to key a generated cache on, so only an
    // identity that is fit for that goes there. A weak or contradicted
    // validator leaves it empty and says why in `resolverInfo`.
    if (published.reusable) info.version = published.validationToken;

    // The four values of RESOLVER.md §3, under the neutral names the consumer
    // contract uses. `size` is a `uint64_t`, which is what a byte count is.
    VtDictionary resolverInfo;
    resolverInfo["resolvedIdentifier"] = VtValue(published.resolvedIdentifier);
    resolverInfo["size"] = VtValue(published.size);
    resolverInfo["validationToken"] = VtValue(published.validationToken);
    resolverInfo["stability"] = VtValue(published.stability);
    info.resolverInfo = VtValue(resolverInfo);

    // `assetName` stays empty. It names an asset in a studio asset system, this
    // resolver knows only URLs, and filling it with the last path segment would
    // publish a guess in a field a consumer may key on.
    return info;
}

ArTimestamp HttpResolver::_GetModificationTimestamp(
    const std::string& assetPath, const ArResolvedPath& resolvedPath) const {
    (void)assetPath;
    (void)resolvedPath;
    return ArTimestamp();
}

std::string HttpResolver::_GetExtension(const std::string& assetPath) const {
    return usdhttpresolver::ExtensionOf(assetPath);
}

bool HttpResolver::_RememberIdentity(
    const std::string& identifier,
    const usdasset::AssetMetadata& metadata) const {
    std::lock_guard<std::mutex> lock(_identityMutex);

    // The fingerprint first, because it is the half that decides an answer's
    // trustworthiness rather than its cost. It is compared before it is
    // overwritten, and it is never erased.
    _Fingerprint& fingerprint = _fingerprints[identifier];
    if (!fingerprint.seen) {
        fingerprint.validator = metadata.validator;
        fingerprint.seen = true;
    } else if (fingerprint.validator != metadata.validator) {
        // Equality of the validator, which is the only comparison any layer
        // performs on one (ASSET_READER.md §7.1). A difference is a republish
        // underneath this process, and it is remembered permanently.
        fingerprint.contradicted = true;
        fingerprint.validator = metadata.validator;
    }
    const bool contradicted = fingerprint.contradicted;

    // And then the answer, which is bounded: dropping one of these costs a
    // metadata request, and the line above is why it costs nothing else.
    const auto found = _identities.find(identifier);
    if (found != _identities.end()) {
        found->second.metadata = metadata;
    } else {
        _identities.emplace(identifier, _Identity{metadata});
        _identityOrder.push_back(identifier);
        while (_identityOrder.size() > kMaxRememberedIdentities) {
            _identities.erase(_identityOrder.front());
            _identityOrder.pop_front();
        }
    }

    return contradicted;
}

bool HttpResolver::_KnownIdentity(const std::string& identifier,
                                  usdasset::AssetMetadata* metadata,
                                  bool* contradicted) const {
    std::lock_guard<std::mutex> lock(_identityMutex);

    const auto found = _identities.find(identifier);
    if (found == _identities.end()) return false;

    const auto fingerprint = _fingerprints.find(identifier);

    *metadata = found->second.metadata;
    *contradicted = fingerprint != _fingerprints.end() &&
                    fingerprint->second.contradicted;
    return true;
}

bool HttpResolver::_IdentityFor(const std::string& identifier,
                                bool mayOpen,
                                usdasset::AssetMetadata* metadata,
                                bool* contradicted) const {
    if (_KnownIdentity(identifier, metadata, contradicted)) return true;
    if (!mayOpen) return false;

    // Nothing in this process has opened it, or the answer has aged out of the
    // bounded table. Opening it here goes through the same retained table
    // `_Resolve` fills, so the metadata request this costs is the one an
    // `_OpenAsset` that follows would have made rather than an extra one.
    const std::shared_ptr<_Opened> entry = _GetOrCreate(identifier);

    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (!entry->opened) {
            entry->result = usdasset::http::Open(identifier, _options);
            entry->opened = true;
            if (entry->result.reader) {
                entry->metadata = entry->result.reader->Metadata();
                entry->succeeded = true;
            }
        }
        if (entry->succeeded) {
            // From the entry and not from `result.reader`, which a concurrent
            // `_OpenAsset` may already have taken. Reading the reader here
            // would report an asset that opened perfectly well as an asset with
            // no identity, and the caller would hand a consumer an empty
            // `ArAssetInfo` for an asset it is about to read.
            *metadata = entry->metadata;
            *contradicted = _RememberIdentity(identifier, *metadata);
            return true;
        }
    }

    // A failure is not retained, exactly as in `_Resolve`. Unlike `_Resolve`,
    // nothing is posted: this is a question about identity rather than an
    // operation on the asset, and the operation that follows reports the same
    // failure with the same code. One fault rendered twice is noise.
    _Forget(identifier, entry);
    return false;
}

std::shared_ptr<HttpResolver::_Opened> HttpResolver::_GetOrCreate(
    const std::string& identifier) const {
    // Declared before the lock, and therefore destroyed after it is released.
    //
    // That ordering is the whole point of this vector. Dropping the last
    // reference to an evicted entry runs `~HttpAssetReader`, which tears down a
    // connection -- a socket close, and a TLS shutdown that can put bytes on the
    // wire. Doing that while holding the table lock would block every unrelated
    // identifier's resolution behind one eviction, which is exactly the "no lock
    // across a request" property RESOLVER.md §7 requires.
    std::vector<std::shared_ptr<_Opened>> evicted;

    std::lock_guard<std::mutex> lock(_tableMutex);

    const auto found = _table.find(identifier);
    if (found != _table.end()) return found->second;

    std::shared_ptr<_Opened> entry = std::make_shared<_Opened>();
    _table.emplace(identifier, entry);
    _order.push_back(identifier);

    while (_order.size() > kMaxRetainedOpens) {
        // The evicted entry may still be held by a thread that is opening it;
        // `shared_ptr` is what makes that safe. What it loses is the chance to
        // reuse that open, which costs one later metadata request.
        const auto oldest = _table.find(_order.front());
        if (oldest != _table.end()) {
            evicted.push_back(std::move(oldest->second));
            _table.erase(oldest);
        }
        _order.pop_front();
    }
    return entry;
}

std::shared_ptr<HttpResolver::_Opened> HttpResolver::_Take(
    const std::string& identifier) const {
    std::lock_guard<std::mutex> lock(_tableMutex);

    const auto found = _table.find(identifier);
    if (found == _table.end()) return nullptr;

    std::shared_ptr<_Opened> entry = found->second;
    _table.erase(found);
    for (auto it = _order.begin(); it != _order.end(); ++it) {
        if (*it == identifier) {
            _order.erase(it);
            break;
        }
    }
    return entry;
}

void HttpResolver::_Forget(const std::string& identifier,
                           const std::shared_ptr<_Opened>& entry) const {
    // Same ordering argument as `_GetOrCreate`: whatever this drops is dropped
    // after the lock is released. A forgotten entry is a failed open and so
    // usually holds no reader, but "usually" is not a reason to hold a lock
    // across a destructor.
    std::shared_ptr<_Opened> removed;

    std::lock_guard<std::mutex> lock(_tableMutex);

    const auto found = _table.find(identifier);
    if (found == _table.end() || found->second != entry) {
        // Somebody else has already replaced this entry. Theirs is newer than
        // the failure being forgotten, and is not ours to discard.
        return;
    }
    removed = std::move(found->second);
    _table.erase(found);

    for (auto it = _order.begin(); it != _order.end(); ++it) {
        if (*it == identifier) {
            _order.erase(it);
            break;
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
