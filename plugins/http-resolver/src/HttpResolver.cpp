// SPDX-License-Identifier: Apache-2.0

#include "HttpResolver.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pxr/base/vt/dictionary.h"
#include "pxr/base/vt/value.h"
#include "pxr/usd/ar/defineResolver.h"
#include "pxr/usd/ar/writableAsset.h"

#include "Configuration.h"
#include "Context.h"
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

HttpResolver::HttpResolver() = default;

HttpResolver::~HttpResolver() = default;

void HttpResolver::_EnsureConfigured() const {
    std::call_once(_configureOnce, [this] { _Configure(); });
}

void HttpResolver::_Configure() const {
    // The environment, once. Everything this resolver is configured by -- the
    // base configuration here, and every context resolved later -- reads this
    // snapshot rather than `getenv`, so one process has one environment.
    _environment = usdhttpresolver::Snapshot(&usdhttpresolver::ReadEnvironmentVariable);

    std::vector<usdhttpresolver::ConfigurationProblem> problems;
    const usdhttpresolver::ResolverConfiguration configuration =
        usdhttpresolver::ConfigurationFrom(usdhttpresolver::LookupIn(_environment),
                                           &problems);
    _base.transport = configuration.transport;
    _base.cache = configuration.cache.Normalized();
    _base.fingerprint = usdhttpresolver::TransportFingerprint(_base.transport);

    // The budget belongs to the process store rather than to this resolver, so
    // it is applied where it lives. Refused only when something is already bound
    // into that store, which for a resolver constructed by `Plug` before any
    // stage opens cannot happen -- and if it somehow does, the store keeps the
    // budget it has and says so rather than being rebuilt underneath a live
    // reader.
    if (!usdasset::cache::BlockCache::ConfigureProcess(_base.cache)) {
        problems.push_back(
            {"USD_HTTP_RESOLVER_CACHE_BUDGET",
             std::to_string(_base.cache.budgetBytes),
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
        // At first use, per CONFIGURATION.md §2 -- which is exactly when this
        // runs. A typo that silently does nothing is worse than one that is
        // reported, and a report in a process that never used the resolver is
        // noise about a setting nothing read.
        usdhttpresolver::ReportConfigurationProblem(problem);
    }
}

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

    // Under the stage's own configuration, when one is bound. A resolve under a
    // context that refuses a destination fails here, and -- because the path is
    // context-dependent -- that failure is what OpenUSD's layer registry acts
    // on, even for a layer another stage has already loaded.
    const _Effective effective = _EffectiveConfiguration();
    const std::string key = _OpenKey(identifier, effective);

    // Inside a scope, the scope's answer for this identifier under this
    // configuration, if it has one. Looked up under the scope's lock and
    // resolved outside it: a round trip under a lock every thread of the scope
    // shares would serialize the composition the scope exists to speed up.
    // Two threads that miss together are single-flighted below, by the
    // retained entry, and the second insertion is a no-op.
    const std::shared_ptr<_ResolveCache> scope = _resolveCache.GetCurrentCache();
    if (scope) {
        std::lock_guard<std::mutex> lock(scope->mutex);
        const auto found = scope->resolved.find(key);
        if (found != scope->resolved.end()) return found->second;
    }

    const ArResolvedPath resolved = _ResolveOnce(identifier, key, effective);

    if (scope) {
        std::lock_guard<std::mutex> lock(scope->mutex);
        scope->resolved.emplace(key, resolved);
    }
    return resolved;
}

ArResolvedPath HttpResolver::_ResolveOnce(const std::string& identifier,
                                          const std::string& key,
                                          const _Effective& effective) const {
    const std::shared_ptr<_Opened> entry = _GetOrCreate(key);

    std::lock_guard<std::mutex> lock(entry->mutex);
    if (!entry->opened) {
        entry->result = usdasset::http::Open(identifier, effective.transport);
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
        _RememberIdentity(identifier, entry->metadata, effective.transport.destinations);
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
    _Forget(key, entry);

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

    // The reader `_Resolve` retained is taken only when it was opened the way
    // this call would open it. Under a different context -- a narrower
    // destination policy, a shorter deadline -- it is left for a caller it
    // fits, and this call opens its own.
    const _Effective effective = _EffectiveConfiguration();

    std::unique_ptr<usdasset::http::HttpAssetReader> reader;

    if (const std::shared_ptr<_Opened> entry = _Take(_OpenKey(identifier, effective))) {
        std::lock_guard<std::mutex> lock(entry->mutex);
        reader = std::move(entry->result.reader);
    }

    if (!reader) {
        // Either nothing resolved this identifier in this process, or the
        // reader `_Resolve` captured has already been handed to somebody.
        usdasset::http::HttpOpenResult result =
            usdasset::http::Open(identifier, effective.transport);
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
    _RememberIdentity(identifier, reader->Metadata(), effective.transport.destinations);

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
        std::move(opened), metrics, effective.cache, nullptr);
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
    if (!_IdentityFor(identifier, resolved, _EffectiveConfiguration(), &metadata,
                      &contradicted)) {
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

ArResolverContext HttpResolver::_CreateContextFromString(
    const std::string& contextStr) const {
    // Validated over the environment it will be layered on, which has to have
    // been read for that.
    _EnsureConfigured();

    std::vector<usdhttpresolver::ConfigurationProblem> problems;
    std::map<std::string, std::string> overrides = usdhttpresolver::OverridesFrom(
        contextStr, usdhttpresolver::LookupIn(_environment), &problems);

    // Reported here and nowhere else. A context is created once and bound many
    // times, often from worker threads, and a warning per bind would be one
    // typo rendered once per composed prim.
    for (const usdhttpresolver::ConfigurationProblem& problem : problems) {
        usdhttpresolver::ReportConfigurationProblem(problem);
    }

    // Before the context exists, so that the first `repr` of a stage opened
    // with it can already print it (Context.h).
    usdhttpresolver::HttpResolverContextEnsurePythonConversion();

    // A context even when nothing was admitted. An empty one configures a
    // stage exactly as the environment does, and returning no context at all
    // would be indistinguishable, to the host, from a resolver that does not
    // implement contexts -- when what happened is that it read the string and
    // said what was wrong with it.
    return ArResolverContext(usdhttpresolver::HttpResolverContext(std::move(overrides)));
}

void HttpResolver::_BeginCacheScope(VtValue* cacheScopeData) {
    _resolveCache.BeginCacheScope(cacheScopeData);
}

void HttpResolver::_EndCacheScope(VtValue* cacheScopeData) {
    _resolveCache.EndCacheScope(cacheScopeData);
}

bool HttpResolver::_IsContextDependentPath(const std::string& assetPath) const {
    // Every path that reaches this resolver is one of its own: the dispatching
    // resolver routes by scheme. See the header for why the answer is yes.
    (void)assetPath;
    return true;
}

HttpResolver::_Effective HttpResolver::_EffectiveConfiguration() const {
    _EnsureConfigured();

    const usdhttpresolver::HttpResolverContext* context =
        _GetCurrentContextObject<usdhttpresolver::HttpResolverContext>();
    if (context == nullptr || context->GetOverrides().empty()) return _base;

    // Resolved per call rather than cached per context. It is a dozen short
    // string parses against a request that crosses a network, and a cache
    // keyed by context would be a second table to bound, lock, and get wrong
    // for no measurable return. No problems are collected: the context's were
    // reported when it was created, and the environment's when this resolver
    // was.
    const usdhttpresolver::ResolverConfiguration configuration =
        usdhttpresolver::ConfigurationFrom(
            usdhttpresolver::Layered(context->GetOverrides(),
                                     usdhttpresolver::LookupIn(_environment)),
            nullptr);

    _Effective effective;
    effective.transport = configuration.transport;
    effective.cache = configuration.cache.Normalized();
    effective.fingerprint = usdhttpresolver::TransportFingerprint(effective.transport);
    return effective;
}

std::string HttpResolver::_OpenKey(const std::string& identifier,
                                   const _Effective& effective) {
    // A newline cannot appear in a normalized identifier -- it is a control
    // byte, and normalization encodes those -- so the two halves cannot run
    // into each other.
    return identifier + '\n' + effective.fingerprint;
}

bool HttpResolver::_RememberIdentity(
    const std::string& identifier,
    const usdasset::AssetMetadata& metadata,
    const usdasset::http::DestinationPolicy& reachedUnder) const {
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
        std::vector<usdasset::http::DestinationPolicy>& policies =
            found->second.reachedUnder;
        // A handful at most -- one per distinct policy that reached it -- so a
        // linear scan is the whole data structure.
        if (std::find(policies.begin(), policies.end(), reachedUnder) ==
            policies.end()) {
            policies.push_back(reachedUnder);
        }
    } else {
        _identities.emplace(identifier, _Identity{metadata, {reachedUnder}});
        _identityOrder.push_back(identifier);
        while (_identityOrder.size() > kMaxRememberedIdentities) {
            _identities.erase(_identityOrder.front());
            _identityOrder.pop_front();
        }
    }

    return contradicted;
}

bool HttpResolver::_KnownIdentity(const std::string& identifier,
                                  const usdasset::http::DestinationPolicy& policy,
                                  usdasset::AssetMetadata* metadata,
                                  bool* contradicted) const {
    std::lock_guard<std::mutex> lock(_identityMutex);

    const auto found = _identities.find(identifier);
    if (found == _identities.end()) return false;

    const std::vector<usdasset::http::DestinationPolicy>& policies =
        found->second.reachedUnder;
    const bool reachable =
        std::any_of(policies.begin(), policies.end(),
                    [&policy](const usdasset::http::DestinationPolicy& reached) {
                        return policy.Covers(reached);
                    });
    if (!reachable) return false;

    const auto fingerprint = _fingerprints.find(identifier);

    *metadata = found->second.metadata;
    *contradicted = fingerprint != _fingerprints.end() &&
                    fingerprint->second.contradicted;
    return true;
}

bool HttpResolver::_IdentityFor(const std::string& identifier,
                                bool mayOpen,
                                const _Effective& effective,
                                usdasset::AssetMetadata* metadata,
                                bool* contradicted) const {
    // Only an identity this caller could have reached itself. A stage whose
    // context refuses a destination is not told the size and token of an
    // asset another stage opened there -- it is told what it would be told had
    // nobody opened it, which is, with an empty resolved path, nothing.
    if (_KnownIdentity(identifier, effective.transport.destinations, metadata,
                       contradicted)) {
        return true;
    }
    if (!mayOpen) return false;

    // Nothing in this process has opened it, or the answer has aged out of the
    // bounded table. Opening it here goes through the same retained table
    // `_Resolve` fills, so the metadata request this costs is the one an
    // `_OpenAsset` that follows would have made rather than an extra one.
    const std::string key = _OpenKey(identifier, effective);
    const std::shared_ptr<_Opened> entry = _GetOrCreate(key);

    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (!entry->opened) {
            entry->result = usdasset::http::Open(identifier, effective.transport);
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
            *contradicted = _RememberIdentity(identifier, *metadata,
                                              effective.transport.destinations);
            return true;
        }
    }

    // A failure is not retained, exactly as in `_Resolve`. Unlike `_Resolve`,
    // nothing is posted: this is a question about identity rather than an
    // operation on the asset, and the operation that follows reports the same
    // failure with the same code. One fault rendered twice is noise.
    _Forget(key, entry);
    return false;
}

std::shared_ptr<HttpResolver::_Opened> HttpResolver::_GetOrCreate(
    const std::string& key) const {
    // Declared before the lock, and therefore destroyed after it is released.
    //
    // That ordering is the whole point of this vector. Dropping the last
    // reference to an evicted entry runs `~HttpAssetReader`, which tears down a
    // connection -- a socket close, and a TLS shutdown that can put bytes on the
    // wire. Doing that while holding the table lock would block every unrelated
    // key's resolution behind one eviction, which is exactly the "no lock
    // across a request" property RESOLVER.md §7 requires.
    std::vector<std::shared_ptr<_Opened>> evicted;

    std::lock_guard<std::mutex> lock(_tableMutex);

    const auto found = _table.find(key);
    if (found != _table.end()) return found->second;

    std::shared_ptr<_Opened> entry = std::make_shared<_Opened>();
    _table.emplace(key, entry);
    _order.push_back(key);

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
    const std::string& key) const {
    std::lock_guard<std::mutex> lock(_tableMutex);

    const auto found = _table.find(key);
    if (found == _table.end()) return nullptr;

    std::shared_ptr<_Opened> entry = found->second;
    _table.erase(found);
    for (auto it = _order.begin(); it != _order.end(); ++it) {
        if (*it == key) {
            _order.erase(it);
            break;
        }
    }
    return entry;
}

void HttpResolver::_Forget(const std::string& key,
                           const std::shared_ptr<_Opened>& entry) const {
    // Same ordering argument as `_GetOrCreate`: whatever this drops is dropped
    // after the lock is released. A forgotten entry is a failed open and so
    // usually holds no reader, but "usually" is not a reason to hold a lock
    // across a destructor.
    std::shared_ptr<_Opened> removed;

    std::lock_guard<std::mutex> lock(_tableMutex);

    const auto found = _table.find(key);
    if (found == _table.end() || found->second != entry) {
        // Somebody else has already replaced this entry. Theirs is newer than
        // the failure being forgotten, and is not ours to discard.
        return;
    }
    removed = std::move(found->second);
    _table.erase(found);

    for (auto it = _order.begin(); it != _order.end(); ++it) {
        if (*it == key) {
            _order.erase(it);
            break;
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
