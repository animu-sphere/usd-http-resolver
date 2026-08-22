// SPDX-License-Identifier: Apache-2.0
//
// The persistent tier: CACHE.md §8, one case per requirement.
//
// The read semantics of a cache with this tier under it are the shared boundary
// suite's business, and `boundary_persisted_local` runs every case of it with
// persistence on. What is here is what that suite cannot ask, because it asks
// only for bytes: whether a second process pays for what the first one already
// fetched, whether a weak validator ever reaches the disk, whether an entry from
// one revision can be served for another, what a scribbled file costs, and what
// the directory a URL was hashed into actually contains.
//
// The "second process" throughout is a second `BlockCache` and a second reader
// over the same directory. It is not a fork, and it does not need to be: the
// property under test is that nothing in memory carries the answer, and a
// second store with nothing resident is exactly that condition.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "Check.h"
#include "FakeReader.h"
#include "usdAssetCache/BlockCache.h"
#include "usdAssetCache/CacheOptions.h"
#include "usdAssetCache/CachedAssetReader.h"
#include "usdAssetCache/DiskBlockStore.h"

// Internal, and reached the way `test_plan` reaches `BlockPlan.h`: a digest
// whose collisions must be hard to produce is not something a round trip can
// check, because a function that is subtly not SHA-256 round-trips perfectly.
#include "Sha256.h"

namespace fs = std::filesystem;

using usdasset::Validator;
using usdasset::ValidatorKind;
using usdasset::ValidatorStrength;
using usdasset::cache::AssetIdentity;
using usdasset::cache::BlockCache;
using usdasset::cache::CachedAssetReader;
using usdasset::cache::CacheOptions;
using usdasset::cache::DiskBlockStore;
using usdasset::cache::DiskCacheOptions;
using usdasset::cache::Persistable;
using usdassetcachetest::FakeReader;
using usdassetcachetest::MakeContent;
using usdassetcachetest::NoValidator;
using usdassetcachetest::StrongValidator;
using usdassetcachetest::WeakValidator;

namespace {

constexpr std::uint64_t kBlockSize = 4096;

/// A directory this run owns, removed when it goes out of scope.
///
/// Named with a counter and a clock rather than only a counter: two runs of this
/// binary in parallel is the normal state of a `ctest -j`, and the defect the
/// `v0.2.0` gate found was two runs of one suite deleting each other's fixtures.
class TempDirectory {
public:
    explicit TempDirectory(const std::string& label) {
        static int counter = 0;
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        _path = fs::temp_directory_path() /
                ("usdAssetCache-persist-" + label + "-" + std::to_string(now) + "-" +
                 std::to_string(++counter));
        std::error_code error;
        fs::create_directories(_path, error);
    }

    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(_path, error);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    const fs::path& Path() const { return _path; }
    std::string String() const { return _path.string(); }

    /// Where the store actually puts entries. Known here so that a case can
    /// look at what a URL became on a filesystem, which is a requirement rather
    /// than an implementation detail.
    fs::path EntryRoot() const { return _path / "blocks-v1"; }

private:
    fs::path _path;
};

CacheOptions RowOptions() {
    CacheOptions options;
    options.blockSize = kBlockSize;
    options.budgetBytes = 64 * kBlockSize;
    options.coalesceGapBlocks = 1;
    options.maxRequestBytes = 32 * kBlockSize;
    // Above every read these cases issue, so the cached path is the path under
    // test rather than the bypass.
    options.bypassThresholdBytes = 1024 * kBlockSize;
    return options.Normalized();
}

DiskCacheOptions DiskOptions(const TempDirectory& directory, std::uint64_t budget) {
    DiskCacheOptions options;
    options.directory = directory.String();
    options.budgetBytes = budget;
    return options;
}

AssetIdentity IdentityOf(const std::string& identifier, const std::string& validator) {
    AssetIdentity identity;
    identity.resolvedIdentifier = identifier;
    identity.validator = validator;
    identity.blockSize = kBlockSize;
    return identity;
}

std::vector<unsigned char> Bytes(std::uint64_t length, unsigned char seed) {
    std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<unsigned char>(seed + i);
    }
    return bytes;
}

/// Every byte of every file under `root`, concatenated.
///
/// For asking the one question a per-file check cannot: whether a string
/// appears *anywhere* under the cache directory, in a name or in a payload.
std::string EveryByteUnder(const std::filesystem::path& root);

/// Every regular file under `root`, entries and temporaries alike.
std::vector<fs::path> FilesUnder(const fs::path& root) {
    std::vector<fs::path> files;
    std::error_code error;
    if (!fs::exists(root, error)) {
        return files;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root, error)) {
        std::error_code entryError;
        if (entry.is_regular_file(entryError) && !entryError) {
            files.push_back(entry.path());
        }
    }
    return files;
}

std::string EveryByteUnder(const fs::path& root) {
    std::string all;
    for (const fs::path& file : FilesUnder(root)) {
        all += file.string();
        std::ifstream input(file, std::ios::binary);
        all.append(std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>());
    }
    return all;
}

bool IsHex(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

/// SHA-256 against the vectors everybody publishes.
///
/// The file comment claims FIPS 180-4, and a claim in this project is supposed
/// to be an assertion. What would go wrong without this is not a wrong answer
/// today -- any consistent function names files and compares equal to itself --
/// but a digest whose collision resistance nobody has ever established, holding
/// up the sentence "a name collision costs a miss and can never serve one
/// asset's bytes for another's".
void TestDigestIsSha256() {
    const auto hex = [](const std::string& input) {
        usdasset::cache::detail::Sha256 sha;
        sha.Update(input.data(), input.size());
        unsigned char digest[usdasset::cache::detail::Sha256::kDigestBytes] = {};
        sha.Finish(digest);
        static const char kHex[] = "0123456789abcdef";
        std::string out;
        for (unsigned char byte : digest) {
            out.push_back(kHex[(byte >> 4) & 0xF]);
            out.push_back(kHex[byte & 0xF]);
        }
        return out;
    };

    CHECK_EQ(hex(""),
             std::string("e3b0c44298fc1c149afbf4c8996fb924"
                         "27ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(hex("abc"),
             std::string("ba7816bf8f01cfea414140de5dae2223"
                         "b00361a396177a9cb410ff61f20015ad"));
    // Two blocks and a length that straddles the padding boundary, which is
    // where a hand-written implementation goes wrong if it is going to.
    CHECK_EQ(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039"
                         "a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ(hex(std::string(1000000, 'a')),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67"
                         "f1809a48a497200e046d39ccc7112cd0"));
}

// --- the strength rule -------------------------------------------------------

void TestStrengthRule() {
    // CACHE.md §8's table, read out.
    CHECK(Persistable(StrongValidator("\"v1\"")));
    CHECK(!Persistable(WeakValidator("W/\"v1\"")));
    CHECK(!Persistable(NoValidator()));

    // A strength without a kind is a producer bug, and it resolves toward the
    // weaker answer here for the same reason `ClassifyStability` does.
    Validator claimed;
    claimed.value = "something";
    claimed.kind = ValidatorKind::None;
    claimed.strength = ValidatorStrength::Strong;
    CHECK(!Persistable(claimed));
}

// --- the store on its own ----------------------------------------------------

void TestDisabledStoreDoesNothing() {
    DiskBlockStore store{DiskCacheOptions()};
    CHECK(!store.IsEnabled());

    const std::vector<unsigned char> block = Bytes(kBlockSize, 7);
    const AssetIdentity identity = IdentityOf("http://host/a", "\"v1\"");
    CHECK(!store.Store(identity, 0, true, block.data(), block.size()));

    std::vector<unsigned char> out;
    CHECK(!store.Load(identity, 0, kBlockSize, &out));

    const DiskBlockStore::Stats stats = store.Snapshot();
    CHECK(!stats.enabled);
    CHECK_EQ(stats.writes, 0u);
    CHECK_EQ(stats.hits, 0u);
}

void TestRoundTrip() {
    TempDirectory directory("roundtrip");
    DiskBlockStore store{DiskOptions(directory, 16 * 1024 * 1024)};
    CHECK(store.IsEnabled());

    const AssetIdentity identity = IdentityOf("http://host/a.usdc", "\"v1\"");
    const std::vector<unsigned char> block = Bytes(kBlockSize, 3);
    CHECK(store.Store(identity, 11, true, block.data(), block.size()));

    std::vector<unsigned char> out;
    CHECK(store.Load(identity, 11, kBlockSize, &out));
    CHECK(out == block);

    // A short final block is stored at its true length and comes back at it. A
    // tier that padded would produce a read past EOF that returns zeros, which
    // CACHE.md §3 names as the silent corruption that looks like valid data.
    const std::vector<unsigned char> tail = Bytes(17, 200);
    CHECK(store.Store(identity, 12, true, tail.data(), tail.size()));
    CHECK(store.Load(identity, 12, 17, &out));
    CHECK(out == tail);
    CHECK(!store.Load(identity, 12, kBlockSize, &out));
}

void TestKeyIsTheWholeKey() {
    TempDirectory directory("key");
    DiskBlockStore store{DiskOptions(directory, 16 * 1024 * 1024)};

    const AssetIdentity identity = IdentityOf("http://host/a", "\"v1\"");
    const std::vector<unsigned char> block = Bytes(kBlockSize, 5);
    CHECK(store.Store(identity, 4, true, block.data(), block.size()));

    std::vector<unsigned char> out;

    // The rule this tier exists to keep: equal identifiers never imply equal
    // content. A second revision at one URL is a second identity, and the entry
    // written for the first one is not reachable from it.
    CHECK(!store.Load(IdentityOf("http://host/a", "\"v2\""), 4, kBlockSize, &out));
    CHECK(!store.Load(IdentityOf("http://host/b", "\"v1\""), 4, kBlockSize, &out));
    CHECK(!store.Load(identity, 5, kBlockSize, &out));

    AssetIdentity otherBlockSize = identity;
    otherBlockSize.blockSize = kBlockSize * 2;
    CHECK(!store.Load(otherBlockSize, 4, kBlockSize, &out));

    // And the one it was written under still answers, so the four above failed
    // for their own reasons rather than because nothing was ever stored.
    CHECK(store.Load(identity, 4, kBlockSize, &out));
    CHECK(out == block);
}

void TestWeakValidatorNeverReachesDisk() {
    TempDirectory directory("weak");
    DiskBlockStore store{DiskOptions(directory, 16 * 1024 * 1024)};

    const AssetIdentity identity = IdentityOf("http://host/a", "W/\"v1\"");
    const std::vector<unsigned char> block = Bytes(kBlockSize, 9);
    CHECK(!store.Store(identity, 0, /*persistable=*/false, block.data(), block.size()));

    CHECK_EQ(store.Snapshot().rejected, 1u);
    CHECK_EQ(store.Snapshot().writes, 0u);
    CHECK(FilesUnder(directory.EntryRoot()).empty());
}

void TestNoUrlBecomesAPath() {
    TempDirectory directory("paths");
    DiskBlockStore store{DiskOptions(directory, 16 * 1024 * 1024)};

    // Three identifiers a filesystem would object to, one of which is a
    // traversal and one of which names a device on Windows. None of them may
    // appear anywhere under the root, in any component.
    const std::vector<std::string> hostile = {
        "http://host/../../../../etc/passwd",
        "http://host/CON",
        "http://host/a?token=secret&x=" + std::string(300, 'z'),
    };
    const std::vector<unsigned char> block = Bytes(kBlockSize, 1);
    for (const std::string& identifier : hostile) {
        CHECK(store.Store(IdentityOf(identifier, "\"v1\""), 0, true, block.data(),
                          block.size()));
    }

    const std::vector<fs::path> files = FilesUnder(directory.EntryRoot());
    CHECK_EQ(files.size(), hostile.size());
    for (const fs::path& file : files) {
        CHECK_EQ(file.extension().string(), std::string(".blk"));
        const std::string stem = file.stem().string();
        CHECK_EQ(stem.size(), std::size_t(32));
        CHECK(IsHex(stem));

        // Two components between the root and the file: the fan-out directory
        // and the file itself, and the fan-out directory is the name's first
        // two characters.
        const fs::path shard = file.parent_path();
        CHECK_EQ(shard.parent_path(), directory.EntryRoot());
        CHECK_EQ(shard.filename().string(), stem.substr(0, 2));
    }

    // And every one of them is still reachable by its own key, so the hashing
    // did not merely throw the identifiers away.
    std::vector<unsigned char> out;
    for (const std::string& identifier : hostile) {
        CHECK(store.Load(IdentityOf(identifier, "\"v1\""), 0, kBlockSize, &out));
        CHECK(out == block);
    }
}

/// Gate 7, at the one artifact this project persists.
///
/// "No diagnostic, log line, metrics dump, or persisted artifact contains a
/// credential, token, or signed-URL query string" is a release gate
/// (docs/releases/README.md), and until `v0.4.0` nothing here persisted
/// anything. A cache entry is keyed by the resolved identifier, a resolved
/// identifier can be a signed URL, and an entry that wrote it down would put a
/// credential on a disk that outlives the process -- in the one place
/// `ElideSecrets` cannot run, because an entry has no message to elide.
///
/// So the identity is written as a digest, and this is the case that says so.
/// It looks at every byte of every file rather than at the format, because what
/// the gate forbids is the string being *there* and not the field being absent.
void TestNoCredentialReachesTheDisk() {
    TempDirectory directory("credentials");
    DiskBlockStore store{DiskOptions(directory, 16 * 1024 * 1024)};

    const std::string signature = "AKIAIOSFODNN7EXAMPLEd34db33fc0ffee";
    const std::string identifier =
        "https://bucket.example.com/scene/asset.usdc"
        "?X-Amz-Credential=" + signature + "&X-Amz-Signature=" + signature;
    const std::string token = "\"etag-" + signature + "\"";

    const std::vector<unsigned char> block = Bytes(kBlockSize, 12);
    CHECK(store.Store(IdentityOf(identifier, token), 3, true, block.data(),
                      block.size()));

    const std::string written = EveryByteUnder(directory.Path());
    CHECK(!written.empty());
    CHECK(written.find(signature) == std::string::npos);
    CHECK(written.find("X-Amz-Signature") == std::string::npos);
    CHECK(written.find("bucket.example.com") == std::string::npos);
    CHECK(written.find("asset.usdc") == std::string::npos);
    CHECK(written.find("etag-") == std::string::npos);

    // And the entry is still reachable by its own key, so the identity was
    // digested rather than discarded.
    std::vector<unsigned char> out;
    CHECK(store.Load(IdentityOf(identifier, token), 3, kBlockSize, &out));
    CHECK(out == block);

    // A neighbouring key that differs only inside the elided query string is a
    // different entry, which is what would break if the identity had been
    // elided instead of digested.
    CHECK(!store.Load(IdentityOf(identifier + "x", token), 3, kBlockSize, &out));
}

void TestPublishLeavesNoTemporary() {
    TempDirectory directory("atomic");
    DiskBlockStore store{DiskOptions(directory, 16 * 1024 * 1024)};

    const std::vector<unsigned char> block = Bytes(kBlockSize, 2);
    for (std::uint64_t i = 0; i < 8; ++i) {
        CHECK(store.Store(IdentityOf("http://host/a", "\"v1\""), i, true, block.data(),
                          block.size()));
    }
    for (const fs::path& file : FilesUnder(directory.EntryRoot())) {
        CHECK(file.extension().string() != ".tmp");
    }
}

void TestCorruptEntryIsDiscarded() {
    TempDirectory directory("corrupt");
    DiskBlockStore store{DiskOptions(directory, 16 * 1024 * 1024)};

    const AssetIdentity identity = IdentityOf("http://host/a", "\"v1\"");
    const std::vector<unsigned char> block = Bytes(kBlockSize, 4);
    CHECK(store.Store(identity, 0, true, block.data(), block.size()));

    const std::vector<fs::path> files = FilesUnder(directory.EntryRoot());
    CHECK_EQ(files.size(), std::size_t(1));
    if (files.empty()) {
        return;
    }

    // A byte flipped in the middle of the payload. Nothing about the file's
    // length changes, so only the checksum can catch it -- which is what the
    // checksum is for.
    {
        std::fstream file(files[0], std::ios::binary | std::ios::in | std::ios::out);
        CHECK(static_cast<bool>(file));
        file.seekp(static_cast<std::streamoff>(200));
        const char scribble = '\xAB';
        file.write(&scribble, 1);
    }

    std::vector<unsigned char> out;
    CHECK(!store.Load(identity, 0, kBlockSize, &out));
    CHECK_EQ(store.Snapshot().discarded, 1u);
    // Discarded, not left to be re-read and re-rejected forever.
    CHECK(FilesUnder(directory.EntryRoot()).empty());

    // A truncated file: the write that the rename exists to hide, made visible.
    CHECK(store.Store(identity, 1, true, block.data(), block.size()));
    const std::vector<fs::path> second = FilesUnder(directory.EntryRoot());
    CHECK_EQ(second.size(), std::size_t(1));
    if (second.empty()) {
        return;
    }
    std::error_code error;
    fs::resize_file(second[0], 40, error);
    CHECK(!error);
    CHECK(!store.Load(identity, 1, kBlockSize, &out));
    CHECK_EQ(store.Snapshot().discarded, 2u);
}

void TestDeletingTheDirectoryCostsTimeOnly() {
    TempDirectory directory("deletable");
    DiskBlockStore store{DiskOptions(directory, 16 * 1024 * 1024)};

    const AssetIdentity identity = IdentityOf("http://host/a", "\"v1\"");
    const std::vector<unsigned char> block = Bytes(kBlockSize, 6);
    CHECK(store.Store(identity, 0, true, block.data(), block.size()));

    // The whole thing, out from under a live store, which is what a user with a
    // full disk actually does.
    std::error_code error;
    fs::remove_all(directory.EntryRoot(), error);
    CHECK(!error);

    std::vector<unsigned char> out;
    CHECK(!store.Load(identity, 0, kBlockSize, &out));

    // And the next write rebuilds what it needs rather than failing for the
    // life of the process.
    CHECK(store.Store(identity, 0, true, block.data(), block.size()));
    CHECK(store.Load(identity, 0, kBlockSize, &out));
    CHECK(out == block);
}

void TestBudgetIsEnforced() {
    TempDirectory directory("budget");
    const std::uint64_t budget = usdasset::cache::kMinPersistentBudgetBytes;
    DiskBlockStore store{DiskOptions(directory, budget)};

    const AssetIdentity identity = IdentityOf("http://host/big", "\"v1\"");
    const std::vector<unsigned char> block = Bytes(kBlockSize, 8);
    // Four times the budget's worth, so trimming has to happen more than once.
    const std::uint64_t blocks = 4 * budget / kBlockSize;
    for (std::uint64_t i = 0; i < blocks; ++i) {
        store.Store(identity, i, true, block.data(), block.size());
    }

    const DiskBlockStore::Stats stats = store.Snapshot();
    CHECK(stats.trimmed > 0);

    std::uint64_t onDisk = 0;
    for (const fs::path& file : FilesUnder(directory.EntryRoot())) {
        std::error_code error;
        onDisk += fs::file_size(file, error);
    }
    // The sweep runs on an interval rather than on every write, so the ceiling
    // is the budget plus one interval's worth of writes -- which is what
    // `sweepInterval` is, and it is an eighth of the budget with a floor.
    CHECK(onDisk <= budget + 2 * usdasset::cache::kMinPersistentBudgetBytes);

    // Whatever survived is still correct. An eviction that corrupted what it
    // kept would be a far worse failure than one that kept too much.
    std::vector<unsigned char> out;
    for (std::uint64_t i = 0; i < blocks; ++i) {
        if (store.Load(identity, i, kBlockSize, &out)) {
            CHECK(out == block);
        }
    }
}

// --- through a reader --------------------------------------------------------

/// One reader over `content`, with its own block store, so that the only thing
/// two of these share is the directory.
struct Process {
    Process(const std::string& identifier,
            const std::vector<unsigned char>& content,
            Validator validator,
            DiskBlockStore& disk)
        : store(RowOptions()) {
        auto fake = std::make_unique<FakeReader>(identifier, content, std::move(validator));
        reader = fake.get();
        usdasset::ReaderMetrics* innerMetrics = &fake->Metrics();
        usdasset::cache::CachedOpenResult wrapped = usdasset::cache::Wrap(
            std::move(fake), innerMetrics, RowOptions(), &store, &disk);
        cached = std::move(wrapped.reader);
    }

    BlockCache store;
    FakeReader* reader = nullptr;
    std::unique_ptr<CachedAssetReader> cached;
};

/// Reads `length` at `offset` and returns what came back, checking the status
/// and the length on the way.
std::vector<unsigned char> ReadRange(CachedAssetReader& reader,
                                     std::uint64_t offset,
                                     std::size_t length) {
    std::vector<unsigned char> out(length, 0);
    const usdasset::ReadResult result = reader.Read(offset, out.data(), length);
    CHECK(result.status.IsOk());
    CHECK_EQ(result.bytesRead, length);
    return out;
}

void TestSecondProcessPaysNothing() {
    TempDirectory directory("second-process");
    DiskBlockStore disk{DiskOptions(directory, 16 * 1024 * 1024)};

    const std::vector<unsigned char> content = MakeContent(20 * kBlockSize + 33);
    const std::uint64_t offset = 3 * kBlockSize + 11;
    const std::size_t length = 2 * kBlockSize;

    std::vector<unsigned char> first;
    {
        Process cold("http://host/a.usdc", content, StrongValidator("\"v1\""), disk);
        CHECK(cold.cached->PersistsBlocks());
        first = ReadRange(*cold.cached, offset, length);
        CHECK(cold.reader->CallCount() > 0);
        CHECK(cold.cached->SnapshotMetrics().persistedWrites > 0);
    }

    {
        Process warm("http://host/a.usdc", content, StrongValidator("\"v1\""), disk);
        const std::vector<unsigned char> second = ReadRange(*warm.cached, offset, length);
        CHECK(second == first);
        CHECK_EQ(warm.reader->CallCount(), std::size_t(0));

        const usdasset::MetricsSnapshot metrics = warm.cached->SnapshotMetrics();
        CHECK(metrics.persistedHits > 0);
        CHECK_EQ(metrics.blockMisses, 0u);
        CHECK_EQ(metrics.bytesTransferred, 0u);
        CHECK_EQ(metrics.bytesFromCache, static_cast<std::uint64_t>(length));
    }

    // And the bytes are the file's bytes, compared against the content the fake
    // was built from rather than against the first read.
    CHECK(std::equal(first.begin(), first.end(), content.begin() + static_cast<long>(offset)));
}

void TestRevisionANeverServesRevisionB() {
    TempDirectory directory("revision");
    DiskBlockStore disk{DiskOptions(directory, 16 * 1024 * 1024)};

    const std::vector<unsigned char> revisionA = MakeContent(8 * kBlockSize);
    std::vector<unsigned char> revisionB = revisionA;
    // Same size, same URL, different bytes and a different validator: the exact
    // shape of a republish, and the one a URL-keyed cache gets wrong.
    for (unsigned char& byte : revisionB) {
        byte = static_cast<unsigned char>(byte ^ 0xFF);
    }

    {
        Process first("http://host/a.usdc", revisionA, StrongValidator("\"v1\""), disk);
        const std::vector<unsigned char> bytes = ReadRange(*first.cached, 0, 2 * kBlockSize);
        CHECK(std::equal(bytes.begin(), bytes.end(), revisionA.begin()));
    }
    {
        Process second("http://host/a.usdc", revisionB, StrongValidator("\"v2\""), disk);
        const std::vector<unsigned char> bytes = ReadRange(*second.cached, 0, 2 * kBlockSize);
        CHECK(std::equal(bytes.begin(), bytes.end(), revisionB.begin()));
        // Cold, because the entries on disk belong to the other revision.
        CHECK(second.reader->CallCount() > 0);
        CHECK_EQ(second.cached->SnapshotMetrics().persistedHits, 0u);
    }
    // And revision A is still there and still itself, so the second open added
    // an identity rather than replacing one.
    {
        Process again("http://host/a.usdc", revisionA, StrongValidator("\"v1\""), disk);
        const std::vector<unsigned char> bytes = ReadRange(*again.cached, 0, 2 * kBlockSize);
        CHECK(std::equal(bytes.begin(), bytes.end(), revisionA.begin()));
        CHECK_EQ(again.reader->CallCount(), std::size_t(0));
    }
}

void TestWeakAndAbsentIdentitiesStayCold() {
    const std::vector<unsigned char> content = MakeContent(8 * kBlockSize);

    for (int weak = 0; weak < 2; ++weak) {
        TempDirectory directory(weak ? "weak-reader" : "none-reader");
        DiskBlockStore disk{DiskOptions(directory, 16 * 1024 * 1024)};
        const Validator validator = weak ? WeakValidator("W/\"v1\"") : NoValidator();

        {
            Process first("http://host/a.usdc", content, validator, disk);
            CHECK(!first.cached->PersistsBlocks());
            ReadRange(*first.cached, 0, 2 * kBlockSize);
            CHECK_EQ(first.cached->SnapshotMetrics().persistedWrites, 0u);
        }
        CHECK(FilesUnder(directory.EntryRoot()).empty());

        {
            Process second("http://host/a.usdc", content, validator, disk);
            const std::vector<unsigned char> bytes =
                ReadRange(*second.cached, 0, 2 * kBlockSize);
            CHECK(std::equal(bytes.begin(), bytes.end(), content.begin()));
            // The whole point: no binding survives the first reader, so the
            // second one pays again rather than guessing.
            CHECK(second.reader->CallCount() > 0);
        }
    }
}

void TestPersistenceOffChangesNothing() {
    DiskBlockStore disabled{DiskCacheOptions()};
    const std::vector<unsigned char> content = MakeContent(8 * kBlockSize);

    Process first("http://host/a.usdc", content, StrongValidator("\"v1\""), disabled);
    CHECK(!first.cached->PersistsBlocks());
    ReadRange(*first.cached, 0, 2 * kBlockSize);
    const std::size_t coldCalls = first.reader->CallCount();
    CHECK(coldCalls > 0);

    Process second("http://host/a.usdc", content, StrongValidator("\"v1\""), disabled);
    ReadRange(*second.cached, 0, 2 * kBlockSize);
    CHECK_EQ(second.reader->CallCount(), coldCalls);
}

void TestPartialDiskCoverage() {
    TempDirectory directory("partial");
    DiskBlockStore disk{DiskOptions(directory, 16 * 1024 * 1024)};

    const std::vector<unsigned char> content = MakeContent(16 * kBlockSize);

    // Warm exactly the middle block of a three-block span, so the second reader
    // has to fetch around a disk hit rather than through one. That is the case
    // that settles ownership out of order.
    {
        Process warmer("http://host/a.usdc", content, StrongValidator("\"v1\""), disk);
        ReadRange(*warmer.cached, 5 * kBlockSize, static_cast<std::size_t>(kBlockSize));
    }

    Process reader("http://host/a.usdc", content, StrongValidator("\"v1\""), disk);
    const std::vector<unsigned char> bytes =
        ReadRange(*reader.cached, 4 * kBlockSize, static_cast<std::size_t>(3 * kBlockSize));
    CHECK(std::equal(bytes.begin(), bytes.end(),
                     content.begin() + static_cast<long>(4 * kBlockSize)));

    const usdasset::MetricsSnapshot metrics = reader.cached->SnapshotMetrics();
    CHECK_EQ(metrics.persistedHits, 1u);
    CHECK_EQ(metrics.blockMisses, 2u);

    // One request, not two, and the middle block is inside it. Coalescing
    // merges across the disk hit exactly as it merges across a block that was
    // already resident in memory -- the gap is one block and the threshold is
    // one block -- so the wire moves a block the caller took from the store.
    // That is the cost of the merge and it is charged, which is the whole
    // reason `bytesOverFetched` exists.
    CHECK_EQ(reader.reader->CallCount(), std::size_t(1));
    CHECK_EQ(metrics.bytesOverFetched, kBlockSize);
}

void TestCacheDeletedUnderALiveReader() {
    TempDirectory directory("deleted-live");
    DiskBlockStore disk{DiskOptions(directory, 16 * 1024 * 1024)};

    const std::vector<unsigned char> content = MakeContent(16 * kBlockSize);
    {
        Process warmer("http://host/a.usdc", content, StrongValidator("\"v1\""), disk);
        ReadRange(*warmer.cached, 0, static_cast<std::size_t>(4 * kBlockSize));
    }

    Process reader("http://host/a.usdc", content, StrongValidator("\"v1\""), disk);
    // Half the read from a warm cache, then the cache is gone.
    const std::vector<unsigned char> before =
        ReadRange(*reader.cached, 0, static_cast<std::size_t>(2 * kBlockSize));
    std::error_code error;
    fs::remove_all(directory.Path(), error);

    const std::vector<unsigned char> after =
        ReadRange(*reader.cached, 8 * kBlockSize, static_cast<std::size_t>(2 * kBlockSize));
    CHECK(std::equal(before.begin(), before.end(), content.begin()));
    CHECK(std::equal(after.begin(), after.end(),
                     content.begin() + static_cast<long>(8 * kBlockSize)));
}

}  // namespace

int main() {
    TestDigestIsSha256();
    TestStrengthRule();
    TestDisabledStoreDoesNothing();
    TestRoundTrip();
    TestKeyIsTheWholeKey();
    TestWeakValidatorNeverReachesDisk();
    TestNoUrlBecomesAPath();
    TestNoCredentialReachesTheDisk();
    TestPublishLeavesNoTemporary();
    TestCorruptEntryIsDiscarded();
    TestDeletingTheDirectoryCostsTimeOnly();
    TestBudgetIsEnforced();

    TestSecondProcessPaysNothing();
    TestRevisionANeverServesRevisionB();
    TestWeakAndAbsentIdentitiesStayCold();
    TestPersistenceOffChangesNothing();
    TestPartialDiskCoverage();
    TestCacheDeletedUnderALiveReader();

    return usdassettest::Report("usdAssetCache persistence");
}
