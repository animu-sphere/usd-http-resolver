// SPDX-License-Identifier: Apache-2.0
//
// The bundle against a real origin: a stage composed over HTTP, a range read
// that moves a window rather than an asset, and the failure modes a consumer
// would otherwise discover in production.
//
// This is the one test in the repository that links OpenUSD *and* the hostile
// fixture corpus, and it is the first time in this project that a URL becomes a
// `UsdStage`. Everything below it is already proven -- the boundary suite has
// admitted the backend, and `tests/corpus` has projected every hostile
// behaviour onto a typed code -- so what is left to assert here is exactly what
// only the bundle can be wrong about: registration, normalization, anchoring,
// the `ArAsset` surface, and which OpenUSD channel a failure comes out of.
//
// The origin is stood up by the test. There is no network access, no fixture
// hosted anywhere, and no port anybody has to reserve.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "pxr/base/tf/errorMark.h"
#include "pxr/base/vt/dictionary.h"
#include "pxr/base/vt/value.h"
#include "pxr/pxr.h"
#include "pxr/usd/ar/asset.h"
#include "pxr/usd/ar/assetInfo.h"
#include "pxr/usd/ar/resolvedPath.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/ar/timestamp.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/stage.h"

#include "Check.h"
#include "usdassetfixture/Corpus.h"
#include "usdassetfixture/Server.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

usdassetfixture::Server* g_server = nullptr;

std::vector<unsigned char> Bytes(const std::string& text) {
    return std::vector<unsigned char>(text.begin(), text.end());
}

/// A deterministic pattern, so that a wrong window is a wrong byte rather than
/// a plausible one.
std::vector<unsigned char> Pattern(std::size_t size) {
    std::vector<unsigned char> content(size);
    for (std::size_t i = 0; i < size; ++i) {
        content[i] = static_cast<unsigned char>((i * 31u + (i >> 8)) & 0xFF);
    }
    return content;
}

void Serve(const std::string& path, std::vector<unsigned char> content,
           const std::string& etag,
           usdassetfixture::Behavior behavior =
               usdassetfixture::Behavior::Normal) {
    usdassetfixture::AssetSpec spec;
    spec.path = path;
    spec.content = std::move(content);
    spec.etag = etag;
    spec.behavior = behavior;
    g_server->Serve(spec);
}

/// A remote scene that references a sibling *relatively*, which is how a layer
/// published to a CDN actually refers to its neighbours: nothing in the
/// reference mentions a host, and it only resolves if the anchoring in
/// RESOLVER.md §2.2 is right.
void ServeScene() {
    Serve("/scenes/shot_010/main.usda", Bytes(R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def "Tree" (
        prepend references = @../../assets/tree.usda@
    )
    {
    }
}
)"),
          "\"scene-1\"");

    Serve("/assets/tree.usda", Bytes(R"(#usda 1.0
(
    defaultPrim = "Tree"
)

def Xform "Tree"
{
    double3 xformOp:translate = (1, 2, 3)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
)"),
          "\"tree-1\"");
}

/// The claim the release is about: a remote stage opens, and a relative
/// reference inside it resolves against the layer's URL.
void TestStageOpens() {
    TfErrorMark mark;
    const std::string url = g_server->Url("/scenes/shot_010/main.usda");

    const UsdStageRefPtr stage = UsdStage::Open(url);
    if (!stage) {
        std::fprintf(stderr, "FAIL %s:%d: stage did not open: %s\n", __FILE__,
                     __LINE__, url.c_str());
        ++::usdassettest::FailureCount();
        mark.Clear();
        return;
    }

    // The root layer is the URL, not a local copy of it.
    CHECK(stage->GetRootLayer()->GetIdentifier() == url);

    // The referenced prim came from `../../assets/tree.usda`, which only
    // resolves if the reference was anchored to the layer's own URL.
    const UsdPrim tree = stage->GetPrimAtPath(SdfPath("/World/Tree"));
    CHECK(tree.IsValid());
    if (tree.IsValid()) {
        const UsdAttribute translate = tree.GetAttribute(
            TfToken("xformOp:translate"));
        CHECK(translate.IsValid());
    }

    CHECK(mark.IsClean());
    mark.Clear();
}

/// Parses a `Range` header's `bytes=first-last`, as the fixture server logged it.
///
/// The test parses the header itself rather than asking the backend what it
/// sent, for the reason the baseline harness does: the server's log is the
/// independent witness, and a request issued outside the metrics sink counts
/// nothing there and still costs a round trip.
bool ParseByteRange(const std::string& header, std::uint64_t* first,
                    std::uint64_t* last) {
    const std::size_t equals = header.find('=');
    if (equals == std::string::npos) return false;
    const std::size_t dash = header.find('-', equals + 1);
    if (dash == std::string::npos) return false;
    *first = std::strtoull(header.c_str() + equals + 1, nullptr, 10);
    *last = std::strtoull(header.c_str() + dash + 1, nullptr, 10);
    return *last >= *first;
}

/// §4: the `ArAsset` surface, and the reason this project exists -- a window
/// out of an asset costs the window.
void TestRangeRead() {
    const std::size_t size = 1u << 20;  // 1 MiB
    const std::vector<unsigned char> content = Pattern(size);
    Serve("/data/blob.bin", content, "\"blob-1\"");

    const std::string url = g_server->Url("/data/blob.bin");
    g_server->ClearLog();

    TfErrorMark mark;
    const std::shared_ptr<ArAsset> asset =
        ArGetResolver().OpenAsset(ArResolvedPath(url));
    if (!asset) {
        std::fprintf(stderr, "FAIL %s:%d: OpenAsset returned null\n", __FILE__,
                     __LINE__);
        ++::usdassettest::FailureCount();
        mark.Clear();
        return;
    }

    CHECK_EQ(asset->GetSize(), size);

    // §4.1: null, permanently. A consumer that dereferences without checking
    // has a bug that a local file happened to hide.
    CHECK(asset->GetBuffer() == nullptr);
    CHECK(asset->GetFileUnsafe().first == nullptr);

    const std::size_t offset = 500000;
    const std::size_t count = 4096;
    std::vector<unsigned char> window(count, 0);
    const std::size_t read = asset->Read(window.data(), count, offset);
    CHECK_EQ(read, count);
    CHECK(std::equal(window.begin(), window.end(), content.begin() + offset));

    // A read past EOF is truncation, not a failure, and truncation at EOF is
    // success (ASSET_READER.md §3, DIAGNOSTICS.md §4.1).
    std::vector<unsigned char> tail(count, 0);
    const std::size_t tailRead = asset->Read(tail.data(), count, size - 10);
    CHECK_EQ(tailRead, std::size_t{10});
    const std::size_t past = asset->Read(tail.data(), count, size + 1);
    CHECK_EQ(past, std::size_t{0});

    CHECK(mark.IsClean());
    mark.Clear();

    // What was actually asked of the network. A `Range` header on every read, a
    // request that covers the window, and nothing that fetched the whole
    // megabyte.
    //
    // The covering request is no longer the window itself. `v0.3.0` puts a
    // block cache under this asset, so the window is expanded to the block that
    // holds it, and CACHE.md section 3 trades the exact-bytes property away on
    // purpose. What the release still claims -- and what this checks -- is that
    // a 4 KiB window out of a megabyte costs a block and not the megabyte. The
    // bound is stated here rather than imported from the cache's constants: a
    // test that knew the block size would agree with the cache by construction,
    // and the question being asked is whether the cache ran away.
    const std::uint64_t windowEnd = offset + count;
    bool sawWindow = false;
    std::uint64_t movedByGets = 0;
    for (const usdassetfixture::RequestRecord& record : g_server->Log()) {
        if (record.method != "GET") continue;
        CHECK(!record.range.empty());
        std::uint64_t first = 0;
        std::uint64_t last = 0;
        if (!ParseByteRange(record.range, &first, &last)) {
            std::fprintf(stderr, "FAIL %s:%d: unparseable Range: %s\n",
                         __FILE__, __LINE__, record.range.c_str());
            ++::usdassettest::FailureCount();
            continue;
        }
        movedByGets += last - first + 1;
        if (first <= offset && last + 1 >= windowEnd) sawWindow = true;
        // No single request took a quarter of the asset.
        CHECK(last - first + 1 <= size / 4);
    }
    CHECK(sawWindow);
    // And neither did all of them together. Three reads of a megabyte-sized
    // asset moved a fraction of it, which is the sentence the whole project is
    // made of.
    CHECK(movedByGets <= size / 4);
}

/// §2.3: one metadata request per identifier, reused by the open that follows.
void TestResolveIsNotRepeated() {
    const std::string url = g_server->Url("/data/blob.bin");
    g_server->ClearLog();

    const ArResolvedPath resolved = ArGetResolver().Resolve(url);
    CHECK(!resolved.empty());
    const std::size_t afterResolve = g_server->RequestCount();
    CHECK(afterResolve >= 1);

    const std::shared_ptr<ArAsset> asset = ArGetResolver().OpenAsset(resolved);
    CHECK(asset != nullptr);
    // Opening a resolved asset does not repeat the round trip.
    CHECK_EQ(g_server->RequestCount(), afterResolve);
}

/// §2.1 end to end: two spellings of one asset are one identifier, and
/// therefore one entry and one open.
void TestNormalizationThroughAr() {
    const std::string url = g_server->Url("/data/blob.bin");
    const std::string ugly =
        g_server->Url("/data/./x/../blob.bin") + "#/World";

    const std::string canonical = ArGetResolver().CreateIdentifier(url);
    CHECK(canonical == url);
    CHECK(ArGetResolver().CreateIdentifier(ugly) == url);

    // And the extension survives a query string, which is what a pre-signed
    // URL always has.
    CHECK(ArGetResolver().GetExtension(
              g_server->Url("/scenes/shot_010/main.usda?X-Amz-Signature=a.b")) ==
          "usda");
}

/// §2.3 and DIAGNOSTICS.md §4.4: an absent asset is not a fault, and a fault is
/// not an absence.
void TestAbsenceIsNotAFailure() {
    Serve("/missing.usda", Bytes("nothing"), "\"gone\"",
          usdassetfixture::Behavior::NotFound);

    TfErrorMark mark;
    const ArResolvedPath resolved =
        ArGetResolver().Resolve(g_server->Url("/missing.usda"));
    CHECK(resolved.empty());
    // A `404` is an answer, not a diagnostic. Posting an error here is what
    // fills a consumer's log with noise for every speculative existence check.
    CHECK(mark.IsClean());
    mark.Clear();
}

/// A transport failure *is* a diagnostic, and it names the condition rather
/// than the client library.
void TestFailureIsReported() {
    Serve("/denied.usda", Bytes("secret"), "\"denied\"",
          usdassetfixture::Behavior::AccessDenied);

    TfErrorMark mark;
    const ArResolvedPath resolved =
        ArGetResolver().Resolve(g_server->Url("/denied.usda"));
    CHECK(resolved.empty());
    CHECK(!mark.IsClean());

    bool sawCode = false;
    for (const TfError& error : mark) {
        if (error.GetCommentary().find("HTTP002") != std::string::npos) {
            sawCode = true;
        }
    }
    CHECK(sawCode);
    mark.Clear();
}

/// A server that will not serve ranges is a hard error, with no whole-asset
/// fallback (ADR-0002). The whole body arriving is not permission to use it.
void TestRangeUnsupportedIsTerminal() {
    Serve("/whole.bin", Pattern(4096), "\"whole-1\"",
          usdassetfixture::Behavior::NoAcceptRanges);

    TfErrorMark mark;
    const std::shared_ptr<ArAsset> asset =
        ArGetResolver().OpenAsset(ArResolvedPath(g_server->Url("/whole.bin")));
    CHECK(asset == nullptr);

    bool sawCode = false;
    for (const TfError& error : mark) {
        if (error.GetCommentary().find("HTTP003") != std::string::npos) {
            sawCode = true;
        }
    }
    CHECK(sawCode);
    mark.Clear();
}

/// One field out of the `resolverInfo` dictionary, or the empty string.
///
/// The test reads the dictionary the way a consumer would rather than through
/// a helper the bundle exports, because there is no such helper: the surface is
/// `ArAssetInfo` and nothing else, which is what ADR-0001 means by keeping the
/// consumer interface at OpenUSD's own types.
std::string InfoField(const ArAssetInfo& info, const std::string& key) {
    if (!info.resolverInfo.IsHolding<VtDictionary>()) return std::string();
    const VtDictionary& fields = info.resolverInfo.UncheckedGet<VtDictionary>();
    const VtDictionary::const_iterator found = fields.find(key);
    if (found == fields.end()) return std::string();
    if (!found->second.IsHolding<std::string>()) return std::string();
    return found->second.UncheckedGet<std::string>();
}

std::uint64_t InfoSize(const ArAssetInfo& info) {
    if (!info.resolverInfo.IsHolding<VtDictionary>()) return 0;
    const VtDictionary& fields = info.resolverInfo.UncheckedGet<VtDictionary>();
    const VtDictionary::const_iterator found = fields.find("size");
    if (found == fields.end()) return 0;
    if (!found->second.IsHolding<std::uint64_t>()) return 0;
    return found->second.UncheckedGet<std::uint64_t>();
}

/// §3: the identity a consumer keys its own generated cache on, for the asset
/// that has the validator that admits it.
void TestAssetInfoIsPublished() {
    const std::size_t size = 4096;
    Serve("/identity/strong.bin", Pattern(size), "\"strong-1\"");
    const std::string url = g_server->Url("/identity/strong.bin");

    const ArResolvedPath resolved = ArGetResolver().Resolve(url);
    CHECK(!resolved.empty());

    // Asked after the resolve, which is where a consumer asks it -- and after
    // the open, below, which is where the reader that knows the answer is gone.
    const std::size_t afterResolve = g_server->RequestCount();
    const std::shared_ptr<ArAsset> asset = ArGetResolver().OpenAsset(resolved);
    CHECK(asset != nullptr);

    const ArAssetInfo info = ArGetResolver().GetAssetInfo(url, resolved);

    // The four values of RESOLVER.md §3, under the names the consumer contract
    // uses.
    CHECK(InfoField(info, "resolvedIdentifier") == url);
    CHECK_EQ(InfoSize(info), std::uint64_t{size});
    CHECK(InfoField(info, "validationToken") == "\"strong-1\"");
    CHECK(InfoField(info, "stability") == "Stable");

    // And the field a consumer reads first: a token there means "this identity
    // may key something that outlives the open".
    CHECK(info.version == "\"strong-1\"");

    // It cost nothing. The identity came from the open this process already
    // performed, which is both cheaper and *more correct* than a fresh `HEAD`:
    // a `HEAD` issued now would describe whatever is published now, which is
    // not necessarily what the asset above is reading.
    CHECK_EQ(g_server->RequestCount(), afterResolve);
}

/// The two identities that must not be reusable, end to end: a weak validator
/// and no validator at all. Both read correctly; neither may key anything.
void TestAssetInfoStabilityClasses() {
    usdassetfixture::AssetSpec weak;
    weak.path = "/identity/weak.bin";
    weak.content = Pattern(512);
    weak.etag = "\"weak-1\"";
    weak.weakValidator = true;
    g_server->Serve(weak);

    const std::string weakUrl = g_server->Url("/identity/weak.bin");
    const ArResolvedPath weakResolved = ArGetResolver().Resolve(weakUrl);
    CHECK(!weakResolved.empty());
    const ArAssetInfo weakInfo = ArGetResolver().GetAssetInfo(weakUrl,
                                                              weakResolved);
    CHECK(weakInfo.version.empty());
    CHECK(InfoField(weakInfo, "stability") == "Unstable");
    // Still published, because a weak validator that changed is still evidence
    // that the asset changed (ASSET_READER.md §7.2). It just cannot prove that
    // one that did *not* change means the bytes are identical.
    CHECK(!InfoField(weakInfo, "validationToken").empty());

    usdassetfixture::AssetSpec anonymous;
    anonymous.path = "/identity/none.bin";
    anonymous.content = Pattern(512);
    g_server->Serve(anonymous);

    const std::string anonymousUrl = g_server->Url("/identity/none.bin");
    const ArResolvedPath anonymousResolved =
        ArGetResolver().Resolve(anonymousUrl);
    CHECK(!anonymousResolved.empty());
    const ArAssetInfo anonymousInfo =
        ArGetResolver().GetAssetInfo(anonymousUrl, anonymousResolved);
    CHECK(anonymousInfo.version.empty());
    CHECK(InfoField(anonymousInfo, "stability") == "Unavailable");
    CHECK(InfoField(anonymousInfo, "validationToken").empty());
    // Readable all the same: an asset with no identity is still an asset.
    CHECK_EQ(InfoSize(anonymousInfo), std::uint64_t{512});

    const std::shared_ptr<ArAsset> asset =
        ArGetResolver().OpenAsset(anonymousResolved);
    CHECK(asset != nullptr);
}

/// A republish underneath a running process. `ArAssetInfo` is keyed by path,
/// so once two opens of one identifier have disagreed, this resolver can no
/// longer say which revision a caller is holding -- and stops publishing an
/// identity anybody may reuse.
void TestAssetInfoAfterRepublishIsNotReusable() {
    Serve("/identity/moving.bin", Pattern(256), "\"moving-1\"");
    const std::string url = g_server->Url("/identity/moving.bin");

    const ArResolvedPath resolved = ArGetResolver().Resolve(url);
    CHECK(!resolved.empty());
    CHECK(ArGetResolver().GetAssetInfo(url, resolved).version ==
          "\"moving-1\"");

    // The open consumes the reader `Resolve` retained, so the resolve below is
    // a real second open rather than the first one's answer again. That is also
    // the realistic order: something opened the asset, and the asset moved.
    const std::shared_ptr<ArAsset> first = ArGetResolver().OpenAsset(resolved);
    CHECK(first != nullptr);

    CHECK(g_server->Republish("/identity/moving.bin", Pattern(300),
                              "\"moving-2\""));

    const ArResolvedPath again = ArGetResolver().Resolve(url);
    CHECK(!again.empty());

    const ArAssetInfo info = ArGetResolver().GetAssetInfo(url, again);
    CHECK(info.version.empty());
    CHECK(InfoField(info, "stability") == "Unstable");
    // The current token is still visible, which is what lets a consumer
    // invalidate what it filed under the old one.
    CHECK(InfoField(info, "validationToken") == "\"moving-2\"");
}

/// §3, last line: no credential appears in asset info. The query string of a
/// pre-signed URL is the shape that actually occurs.
void TestAssetInfoElidesCredentials() {
    Serve("/identity/signed.bin", Pattern(128), "\"signed-1\"");
    const std::string url =
        g_server->Url("/identity/signed.bin") + "?X-Amz-Signature=deadbeef";

    const ArResolvedPath resolved = ArGetResolver().Resolve(url);
    CHECK(!resolved.empty());

    const ArAssetInfo info = ArGetResolver().GetAssetInfo(url, resolved);
    const std::string identifier = InfoField(info, "resolvedIdentifier");
    CHECK(identifier.find("deadbeef") == std::string::npos);
    CHECK(identifier.find("?<elided>") != std::string::npos);
    // The identity is still usable: what distinguishes revisions is the token,
    // not the part of the URL that had to be removed.
    CHECK(info.version == "\"signed-1\"");
}

/// The timestamp is invalid, deliberately and permanently. A fabricated one
/// would be read by a consumer's fallback as a durable identity, which is
/// exactly what an asset without a strong validator must not have.
void TestModificationTimestampIsInvalid() {
    const std::string url = g_server->Url("/identity/strong.bin");
    const ArResolvedPath resolved = ArGetResolver().Resolve(url);
    CHECK(!resolved.empty());
    CHECK(!ArGetResolver().GetModificationTimestamp(url, resolved).IsValid());
}

/// Asking for an identity that nothing has opened costs one metadata request,
/// and the open that follows reuses it rather than issuing a second.
void TestAssetInfoRetainsItsOpen() {
    Serve("/identity/cold.bin", Pattern(64), "\"cold-1\"");
    const std::string url = g_server->Url("/identity/cold.bin");
    g_server->ClearLog();

    const ArAssetInfo info = ArGetResolver().GetAssetInfo(url,
                                                          ArResolvedPath(url));
    CHECK(info.version == "\"cold-1\"");
    const std::size_t afterInfo = g_server->RequestCount();
    CHECK(afterInfo >= 1);

    const std::shared_ptr<ArAsset> asset =
        ArGetResolver().OpenAsset(ArResolvedPath(url));
    CHECK(asset != nullptr);
    CHECK_EQ(g_server->RequestCount(), afterInfo);
}

/// A reader retained past the end of the test, deliberately.
///
/// §2.3 lets `Resolve` keep the reader it opened for the `OpenAsset` that
/// usually follows, and nothing requires one to follow: a host probing for
/// existence resolves constantly and opens rarely. Those readers are destroyed
/// when the resolver is, during static destruction, arbitrarily late and after
/// every other test here has passed.
///
/// This case asserts nothing in a `CHECK`. What it asserts is the exit code,
/// which is the only place the failure it exists for can appear -- and it did
/// appear: a reader folding its counters into a process aggregate that had
/// already been destroyed crashed the process after the suite reported "ok".
void TestRetainedOpenSurvivesProcessExit() {
    Serve("/identity/retained.bin", Pattern(64), "\"retained-1\"");
    const ArResolvedPath resolved =
        ArGetResolver().Resolve(g_server->Url("/identity/retained.bin"));
    CHECK(!resolved.empty());
    // And then nothing. The open stays in the resolver's table until the
    // process ends, which is the point.
}

/// §5: writing is unsupported, and says so.
void TestWritingIsRefused() {
    const std::string url = g_server->Url("/data/blob.bin");

    CHECK(ArGetResolver().CreateIdentifierForNewAsset(url).empty());
    CHECK(ArGetResolver().ResolveForNewAsset(url).empty());

    std::string whyNot;
    CHECK(!ArGetResolver().CanWriteAssetToPath(ArResolvedPath(url), &whyNot));
    CHECK(!whyNot.empty());
    CHECK(whyNot.find("HTTP010") != std::string::npos);
}

/// The property this bundle is most likely to break, and the one nobody would
/// notice until it broke: registering a URI-scheme resolver must not change how
/// a local asset opens.
void TestLocalResolutionIsUnchanged(const std::string& localLayer) {
    TfErrorMark mark;
    const ArResolvedPath resolved = ArGetResolver().Resolve(localLayer);
    CHECK(!resolved.empty());

    const UsdStageRefPtr stage = UsdStage::Open(localLayer);
    CHECK(stage != nullptr);
    if (stage) {
        CHECK(stage->GetPrimAtPath(SdfPath("/Local")).IsValid());
    }
    CHECK(mark.IsClean());
    mark.Clear();
}

/// Writes the local fixture beside the test executable's working directory, so
/// that the local-resolution case does not depend on an installed fixture path.
std::string WriteLocalLayer() {
    const std::string path = "httpResolver_local_unchanged.usda";
    std::ofstream out(path, std::ios::binary);
    out << "#usda 1.0\n\ndef Xform \"Local\"\n{\n}\n";
    out.close();
    return path;
}

}  // namespace

int main() {
    std::string error;
    const std::unique_ptr<usdassetfixture::Server> server =
        usdassetfixture::Server::Start(&error);
    if (!server) {
        // A test that cannot bind loopback must say so rather than report a
        // resolver failure.
        std::fprintf(stderr, "fixture server did not start: %s\n",
                     error.c_str());
        return 2;
    }
    g_server = server.get();

    // The plugin has to have been found. Without this check a misconfigured
    // PXR_PLUGINPATH_NAME reports as "every remote asset is missing", which
    // looks exactly like a broken resolver.
    if (!ArGetResolver().CreateIdentifier(g_server->Url("/a.usda")).empty()) {
        ServeScene();
        TestStageOpens();
        TestRangeRead();
        TestResolveIsNotRepeated();
        TestNormalizationThroughAr();
        TestAbsenceIsNotAFailure();
        TestFailureIsReported();
        TestRangeUnsupportedIsTerminal();
        TestAssetInfoIsPublished();
        TestAssetInfoStabilityClasses();
        TestAssetInfoAfterRepublishIsNotReusable();
        TestAssetInfoElidesCredentials();
        TestModificationTimestampIsInvalid();
        TestAssetInfoRetainsItsOpen();
        TestRetainedOpenSurvivesProcessExit();
        TestWritingIsRefused();
        TestLocalResolutionIsUnchanged(WriteLocalLayer());
    } else {
        std::fprintf(stderr,
                     "FAIL: no resolver claimed %s -- is the bundle's "
                     "plugInfo.json on PXR_PLUGINPATH_NAME?\n",
                     g_server->Url("/a.usda").c_str());
        ++usdassettest::FailureCount();
    }

    return usdassettest::Report("httpResolver stage");
}
