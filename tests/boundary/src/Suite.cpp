// SPDX-License-Identifier: Apache-2.0

#include "usdassetboundary/Suite.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "usdassetboundary/Fixture.h"
#include "usdassetboundary/Oracle.h"
#include "usdassetboundary/Random.h"
#include "usdassetboundary/Report.h"

namespace usdassetboundary {
namespace {

using usdasset::AssetReader;
using usdasset::OpenResult;
using usdasset::ReadResult;
using usdasset::StatusCode;

constexpr std::uint64_t kMaxOffset = (std::numeric_limits<std::uint64_t>::max)();

/// Bytes appended past the requested length and checked afterwards. A backend
/// that writes past the range it was given fails here on every platform, not
/// only the ones where the overrun happens to land somewhere fatal.
constexpr std::size_t kGuardBytes = 64;
constexpr unsigned char kGuardFill = 0xCD;

struct Fixture {
    std::string name;
    std::uint64_t size = 0;
    std::string oraclePath;
    ProvisionedAsset asset;
};

bool ProvisionFixture(const BackendUnderTest& backend,
                      FixtureWorkspace& workspace,
                      const std::string& name,
                      std::uint64_t size,
                      FixtureBehavior behavior,
                      std::uint64_t contentSeed,
                      Fixture* fixtureOut) {
    const std::vector<unsigned char> content =
        PositionalContent(static_cast<std::size_t>(size), contentSeed);

    fixtureOut->name = name;
    fixtureOut->size = size;
    fixtureOut->oraclePath = workspace.Write(name + ".bin", content);
    if (fixtureOut->oraclePath.empty()) {
        return false;
    }

    FixtureRequest request;
    request.name = name;
    request.behavior = behavior;
    request.size = size;
    request.oraclePath = fixtureOut->oraclePath;

    fixtureOut->asset = backend.provision(request);
    return !fixtureOut->asset.identifier.empty() || fixtureOut->asset.open != nullptr;
}

OpenResult OpenFixture(const BackendUnderTest& backend, const Fixture& fixture) {
    if (fixture.asset.open) {
        return fixture.asset.open();
    }
    return backend.open(fixture.asset.identifier);
}

/// One equivalence, over the whole result rather than the bytes alone.
///
/// Comparing the status code is what catches the failure a byte comparison
/// cannot see: a backend that returns the right bytes with the wrong code has
/// broken the EOF distinction, and the layer above acts on the code.
bool CompareRead(Reporter& reporter,
                 const std::string& caseName,
                 AssetReader& reader,
                 const Fixture& fixture,
                 std::uint64_t offset,
                 std::size_t size) {
    const OracleResult expected = NaiveRead(fixture.oraclePath, offset, size);

    std::vector<unsigned char> buffer(size + kGuardBytes, kGuardFill);
    const ReadResult actual = reader.Read(offset, buffer.data(), size);

    const std::string where = DescribeRead(fixture.name, offset, size);

    if (actual.bytesRead != expected.bytesRead) {
        reporter.Fail(caseName, where + ": bytesRead " +
                                    std::to_string(actual.bytesRead) + ", oracle " +
                                    std::to_string(expected.bytesRead));
        return false;
    }
    if (actual.status.code != expected.code) {
        reporter.Fail(caseName,
                      where + ": status " + usdasset::StatusCodeName(actual.status.code) +
                          ", oracle " + usdasset::StatusCodeName(expected.code));
        return false;
    }
    if (actual.bytesRead > size) {
        reporter.Fail(caseName, where + ": reported more bytes than were requested");
        return false;
    }
    for (std::size_t i = 0; i < actual.bytesRead; ++i) {
        if (buffer[i] != expected.bytes[i]) {
            reporter.Fail(caseName, where + ": byte " + std::to_string(i) +
                                        " is " + std::to_string(buffer[i]) +
                                        ", oracle " +
                                        std::to_string(expected.bytes[i]));
            return false;
        }
    }
    for (std::size_t i = 0; i < kGuardBytes; ++i) {
        if (buffer[size + i] != kGuardFill) {
            reporter.Fail(caseName, where + ": wrote " + std::to_string(i + 1) +
                                        " byte(s) past the end of the buffer");
            return false;
        }
    }

    reporter.Pass();
    return true;
}

void AddCase(std::vector<ReadCase>& cases, std::uint64_t offset, std::uint64_t size) {
    ReadCase readCase;
    readCase.offset = offset;
    readCase.size = static_cast<std::size_t>(size);
    cases.push_back(readCase);
}

/// The required boundary cases of BOUNDARY_SUITE.md §3, for one fixture size.
std::vector<ReadCase> FixedCasesFor(std::uint64_t size) {
    std::vector<ReadCase> cases;

    // Zero-length, at offsets before, at, and past the end.
    AddCase(cases, 0, 0);
    AddCase(cases, size, 0);
    AddCase(cases, size + 1, 0);

    // Offset zero: the trivial case, and where off-by-one framing shows up.
    AddCase(cases, 0, 1);
    AddCase(cases, 0, 16);
    AddCase(cases, 0, kNominalBlockSize);

    // Ending exactly at the end of the asset.
    if (size >= 1) {
        AddCase(cases, size - 1, 1);
    }
    if (size >= 16) {
        AddCase(cases, size - 16, 16);
    }
    if (size >= kNominalBlockSize) {
        AddCase(cases, size - kNominalBlockSize, kNominalBlockSize);
    }

    // Starting exactly at the end, and past it. Both are absences, not errors.
    AddCase(cases, size, 1);
    AddCase(cases, size, 16);
    AddCase(cases, size + 1, 16);
    AddCase(cases, size + kNominalBlockSize, 16);

    // Straddling the end.
    if (size >= 8) {
        AddCase(cases, size - 8, 64);
    }
    AddCase(cases, size == 0 ? 0 : size - 1, kNominalBlockSize);

    // The whole asset in one read: the degenerate streaming case.
    AddCase(cases, 0, size);

    // Block-boundary-like offsets and lengths, before any cache exists, so the
    // cache inherits passing cases rather than acquiring them.
    const std::uint64_t blocks[] = {512, 4096, kNominalBlockSize};
    for (const std::uint64_t block : blocks) {
        for (int offsetDelta = -1; offsetDelta <= 1; ++offsetDelta) {
            for (int sizeDelta = -1; sizeDelta <= 1; ++sizeDelta) {
                // Spelled with a branch rather than by adding a cast of -1.
                // Unsigned wraparound would give the same answer and read like
                // a bug, and this file is where somebody looks to find out
                // what the suite claims to cover.
                const std::uint64_t offset =
                    offsetDelta < 0 ? block - 1
                                    : block + static_cast<std::uint64_t>(offsetDelta);
                const std::uint64_t length =
                    sizeDelta < 0 ? block - 1
                                  : block + static_cast<std::uint64_t>(sizeDelta);
                AddCase(cases, offset, length);
            }
        }
    }

    // Overflow, and the boundary immediately below it.
    AddCase(cases, kMaxOffset, 1);
    AddCase(cases, kMaxOffset - 4, 16);
    AddCase(cases, kMaxOffset - 15, 16);
    AddCase(cases, kMaxOffset - 16, 16);

    return cases;
}

void RunShortReadCase(const BackendUnderTest& backend,
                      FixtureWorkspace& workspace,
                      Reporter& reporter) {
    const std::string caseName = "short read below EOF";

    Fixture fixture;
    if (!ProvisionFixture(backend, workspace, "short-read", kNominalBlockSize * 2,
                          FixtureBehavior::ShortReadBelowEof, 91, &fixture)) {
        reporter.Fail(caseName, "the backend could not provision a short-reading asset");
        return;
    }

    OpenResult opened = OpenFixture(backend, fixture);
    if (!opened.reader) {
        reporter.Fail(caseName,
                      "open failed: " + usdasset::ToString(opened.status));
        return;
    }

    // Well inside the asset, so that a short delivery cannot be confused with
    // the end of the file.
    const std::size_t size = static_cast<std::size_t>(kNominalBlockSize);
    std::vector<unsigned char> buffer(size + kGuardBytes, kGuardFill);
    const ReadResult result = opened.reader->Read(0, buffer.data(), size);

    if (result.status.code != StatusCode::InvalidResponse) {
        reporter.Fail(caseName, std::string("status ") +
                                    usdasset::StatusCodeName(result.status.code) +
                                    ", expected InvalidResponse");
        return;
    }
    if (result.bytesRead >= size) {
        reporter.Fail(caseName,
                      "reported a complete read while the transport delivered less");
        return;
    }
    reporter.Pass();
}

void RunRevisionChangeCase(const BackendUnderTest& backend,
                           FixtureWorkspace& workspace,
                           Reporter& reporter) {
    const std::string caseName = "mid-read revision change";

    if (!backend.simulatesRevisionChange) {
        reporter.NotAdmitted(caseName,
                             "the backend cannot change an asset underneath an "
                             "open reader");
        return;
    }
    if (!backend.republish) {
        reporter.Fail(caseName,
                      "the row declares revision simulation but supplies no republish");
        return;
    }

    Fixture fixture;
    if (!ProvisionFixture(backend, workspace, "revision", kNominalBlockSize * 2,
                          FixtureBehavior::Normal, 17, &fixture)) {
        reporter.Fail(caseName, "could not provision");
        return;
    }

    OpenResult opened = OpenFixture(backend, fixture);
    if (!opened.reader) {
        reporter.Fail(caseName, "open failed: " + usdasset::ToString(opened.status));
        return;
    }

    const std::size_t size = 4096;
    std::vector<unsigned char> before(size + kGuardBytes, kGuardFill);
    const ReadResult first = opened.reader->Read(0, before.data(), size);
    if (!first.status.IsOk() || first.bytesRead != size) {
        reporter.Fail(caseName, "the read before the republish already failed");
        return;
    }

    // A new revision, of a different length and different content.
    const std::vector<unsigned char> republished =
        PositionalContent(static_cast<std::size_t>(kNominalBlockSize * 3), 42);
    if (!backend.republish(fixture.asset.identifier, republished)) {
        // Reported as what it is. A fixture that did not change, presented as
        // a backend that failed to notice, sends somebody to read the wrong
        // repository's code.
        reporter.Fail(caseName, "the backend could not republish the fixture");
        return;
    }

    // The read that has to observe the change: an offset this reader has not
    // read before, so nothing it already holds can answer it and the request
    // reaches the transport.
    std::vector<unsigned char> after(size + kGuardBytes, kGuardFill);
    const ReadResult second = opened.reader->Read(kNominalBlockSize, after.data(), size);

    if (second.status.code != StatusCode::AssetChanged) {
        reporter.Fail(caseName, std::string("status ") +
                                    usdasset::StatusCodeName(second.status.code) +
                                    ", expected AssetChanged");
        return;
    }
    reporter.Pass();

    // A range this reader already read, read again. Two answers are correct and
    // one is not.
    //
    // A backend that reaches its transport observes the change and reports
    // `AssetChanged`. A reader with a block cache over it answers from bytes it
    // captured *under this binding*, observes nothing, and hands back the
    // revision it is bound to -- which is the guarantee, not an exception to
    // it: the contract's wording is "a reader that observes a changed validator
    // fails subsequent reads", and CACHE.md section 6 admits in-memory caching
    // for the reader's lifetime for exactly this reason. What neither is
    // allowed to do is return the new revision's bytes, and that is what this
    // checks. A byte comparison, because a status comparison cannot see it.
    std::vector<unsigned char> reread(size + kGuardBytes, kGuardFill);
    const ReadResult repeat = opened.reader->Read(0, reread.data(), size);
    if (repeat.status.code == StatusCode::Ok) {
        if (repeat.bytesRead != first.bytesRead ||
            !std::equal(reread.begin(), reread.begin() + repeat.bytesRead,
                        before.begin())) {
            reporter.Fail(caseName + " (a repeated read serves the bound revision)",
                          "the bytes changed underneath a reader that reported Ok");
            return;
        }
    } else if (repeat.status.code != StatusCode::AssetChanged) {
        reporter.Fail(caseName + " (a repeated read)",
                      std::string("status ") +
                          usdasset::StatusCodeName(repeat.status.code) +
                          ", expected AssetChanged or the bound revision's bytes");
        return;
    }
    reporter.Pass();

    // And it stays changed. A reader that recovers on the next call has
    // rebound to the new revision, which is the failure the code exists to
    // prevent rather than a transient it survived. Another offset this reader
    // has not read, for the reason the first one was.
    std::vector<unsigned char> third(size + kGuardBytes, kGuardFill);
    const ReadResult retry =
        opened.reader->Read(kNominalBlockSize + size, third.data(), size);
    if (retry.status.code != StatusCode::AssetChanged) {
        reporter.Fail(caseName + " (does not rebind)",
                      std::string("a later read returned ") +
                          usdasset::StatusCodeName(retry.status.code));
        return;
    }
    reporter.Pass();

    // A read that transfers nothing still returns Ok, and this is pinned rather
    // than left to whichever branch a backend happens to take first.
    //
    // AssetChanged exists to stop one reader composing bytes from two
    // revisions, and a read that returns no bytes cannot do that. An offset at
    // or past the size captured at open is past the end *of the revision this
    // reader is bound to*, whatever the file on disk now looks like, so `0, Ok`
    // is that revision's truthful answer. Requiring AssetChanged here would
    // also oblige a range transport to spend a round trip on every read that
    // moves no bytes, which is the amplification this project exists to avoid.
    unsigned char scratch[16];
    const ReadResult atEnd =
        opened.reader->Read(fixture.size, scratch, sizeof(scratch));
    if (atEnd.status.code != StatusCode::Ok || atEnd.bytesRead != 0) {
        reporter.Fail(caseName + " (a read past the bound revision's end is Ok)",
                      std::string("status ") +
                          usdasset::StatusCodeName(atEnd.status.code) + ", bytesRead " +
                          std::to_string(atEnd.bytesRead));
        return;
    }
    reporter.Pass();

    const ReadResult empty = opened.reader->Read(0, scratch, 0);
    if (empty.status.code != StatusCode::Ok || empty.bytesRead != 0) {
        reporter.Fail(caseName + " (a zero-length read is Ok)",
                      std::string("status ") +
                          usdasset::StatusCodeName(empty.status.code));
        return;
    }
    reporter.Pass();

    // The metadata the reader reports is still the revision it was bound to.
    if (opened.reader->Metadata().size != fixture.size) {
        reporter.Fail(caseName + " (metadata is immutable)",
                      "the reader's reported size followed the new revision");
        return;
    }
    reporter.Pass();
}

}  // namespace

// --- fixed -------------------------------------------------------------------

int RunFixedCases(const BackendUnderTest& backend, const SuiteOptions& options) {
    (void)options;
    Reporter reporter(backend.name, "fixed");

    FixtureWorkspace workspace(backend.name + "-fixed");
    if (!workspace.Valid()) {
        reporter.Fail("workspace", "could not create a fixture directory");
        return reporter.Finish();
    }

    std::uint64_t contentSeed = 1;
    for (const FixtureSize& fixtureSize : RequiredFixtureSizes()) {
        Fixture fixture;
        if (!ProvisionFixture(backend, workspace, fixtureSize.name, fixtureSize.size,
                              FixtureBehavior::Normal, contentSeed++, &fixture)) {
            reporter.Fail(std::string("provision ") + fixtureSize.name,
                          "the backend could not provision this fixture");
            continue;
        }

        OpenResult opened = OpenFixture(backend, fixture);
        if (!opened.reader) {
            reporter.Fail(std::string("open ") + fixtureSize.name,
                          usdasset::ToString(opened.status));
            continue;
        }

        // What the reader reports about the asset, before any byte is read.
        if (opened.reader->Metadata().size != fixture.size) {
            reporter.Fail(std::string("size ") + fixtureSize.name,
                          "reported " +
                              std::to_string(opened.reader->Metadata().size) +
                              ", fixture is " + std::to_string(fixture.size));
        } else {
            reporter.Pass();
        }
        if (!opened.reader->Metadata().supportsRandomAccess) {
            reporter.Fail(std::string("random access ") + fixtureSize.name,
                          "a reader in the suite must serve ranges");
        } else {
            reporter.Pass();
        }

        for (const ReadCase& readCase : FixedCasesFor(fixture.size)) {
            CompareRead(reporter, "boundary", *opened.reader, fixture, readCase.offset,
                        readCase.size);
        }
    }

    RunShortReadCase(backend, workspace, reporter);
    RunRevisionChangeCase(backend, workspace, reporter);

    if (!backend.admitsCancellation) {
        // Declared by the row rather than discovered by a test that skips
        // itself. The read contract carries no cancellation token, so a
        // backend admits cancellation only if it has some other way to be
        // told; the local backend has none.
        reporter.NotAdmitted("cancellation",
                             "the backend declares that it does not admit "
                             "cancellation");
    }

    return reporter.Finish();
}

// --- property ----------------------------------------------------------------

namespace {

/// Reduces a failing case to a minimal one, so that the reproducer added to
/// the fixed set is the smallest thing that still breaks.
ReadCase Shrink(AssetReader& reader, const Fixture& fixture, ReadCase failing) {
    const auto stillFails = [&](const ReadCase& candidate) {
        Reporter attempt("shrink", "shrink");
        attempt.Quiet(true);
        return !CompareRead(attempt, "shrink", reader, fixture, candidate.offset,
                            candidate.size);
    };

    bool progressed = true;
    int rounds = 0;
    while (progressed && rounds++ < 64) {
        progressed = false;

        std::vector<ReadCase> candidates;
        if (failing.size > 1) {
            ReadCase half = failing;
            half.size = failing.size / 2;
            candidates.push_back(half);
            ReadCase one = failing;
            one.size = 1;
            candidates.push_back(one);
        }
        if (failing.offset > 0) {
            ReadCase zero = failing;
            zero.offset = 0;
            candidates.push_back(zero);
            ReadCase halved = failing;
            halved.offset = failing.offset / 2;
            candidates.push_back(halved);
            if (fixture.size > 0 && failing.offset > fixture.size) {
                ReadCase atEnd = failing;
                atEnd.offset = fixture.size;
                candidates.push_back(atEnd);
            }
        }

        for (const ReadCase& candidate : candidates) {
            if (stillFails(candidate)) {
                failing = candidate;
                progressed = true;
                break;
            }
        }
    }

    return failing;
}

}  // namespace

int RunPropertyCases(const BackendUnderTest& backend, const SuiteOptions& options) {
    Reporter reporter(backend.name, "property");

    FixtureWorkspace workspace(backend.name + "-property");
    if (!workspace.Valid()) {
        reporter.Fail("workspace", "could not create a fixture directory");
        return reporter.Finish();
    }

    std::uint64_t contentSeed = 1000;
    for (const FixtureSize& fixtureSize : RequiredFixtureSizes()) {
        Fixture fixture;
        if (!ProvisionFixture(backend, workspace, fixtureSize.name, fixtureSize.size,
                              FixtureBehavior::Normal, contentSeed++, &fixture)) {
            reporter.Fail(std::string("provision ") + fixtureSize.name,
                          "the backend could not provision this fixture");
            continue;
        }
        OpenResult opened = OpenFixture(backend, fixture);
        if (!opened.reader) {
            reporter.Fail(std::string("open ") + fixtureSize.name,
                          usdasset::ToString(opened.status));
            continue;
        }

        Generator generator(options.seed ^ (fixture.size * 0x9E3779B97F4A7C15ull));
        for (int i = 0; i < options.propertyIterations; ++i) {
            const ReadCase readCase = GenerateReadCase(generator, fixture.size);
            Reporter attempt(backend.name, "property");
            attempt.Quiet(true);
            if (CompareRead(attempt, "property", *opened.reader, fixture,
                            readCase.offset, readCase.size)) {
                reporter.Pass();
                continue;
            }

            // Report the seed, then the minimal reproducer. A property failure
            // fixed without gaining a fixed case can regress silently, so the
            // line below is written to be pasted into FixedCasesFor.
            const ReadCase minimal = Shrink(*opened.reader, fixture, readCase);
            CompareRead(reporter, "property", *opened.reader, fixture, minimal.offset,
                        minimal.size);
            reporter.Fail("property (reproducer)",
                          "seed=" + std::to_string(options.seed) + " iteration=" +
                              std::to_string(i) + " minimal: AddCase(cases, " +
                              std::to_string(minimal.offset) + ", " +
                              std::to_string(minimal.size) + ");");
            break;
        }
    }

    return reporter.Finish();
}

// --- concurrency -------------------------------------------------------------

int RunConcurrencyCases(const BackendUnderTest& backend, const SuiteOptions& options) {
    Reporter reporter(backend.name, "concurrent");

    FixtureWorkspace workspace(backend.name + "-concurrent");
    if (!workspace.Valid()) {
        reporter.Fail("workspace", "could not create a fixture directory");
        return reporter.Finish();
    }

    Fixture fixture;
    if (!ProvisionFixture(backend, workspace, "several-blocks", kNominalBlockSize * 3 + 7,
                          FixtureBehavior::Normal, 7, &fixture)) {
        reporter.Fail("provision", "the backend could not provision this fixture");
        return reporter.Finish();
    }

    OpenResult opened = OpenFixture(backend, fixture);
    if (!opened.reader) {
        reporter.Fail("open", usdasset::ToString(opened.status));
        return reporter.Finish();
    }

    // One expected image, read once. The property under test is that no
    // caller's bytes are interleaved into another caller's buffer, and that is
    // checked against the file's content rather than against a second reader
    // racing the first.
    const std::vector<unsigned char> expected = NaiveReadAll(fixture.oraclePath);
    if (expected.size() != fixture.size) {
        reporter.Fail("oracle", "could not read the fixture back");
        return reporter.Finish();
    }

    std::atomic<std::uint64_t> mismatches{0};
    std::atomic<std::uint64_t> comparisons{0};

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(options.concurrencyThreads));
    for (int t = 0; t < options.concurrencyThreads; ++t) {
        threads.emplace_back([&, t] {
            Generator generator(options.seed + static_cast<std::uint64_t>(t) * 7919);
            std::vector<unsigned char> buffer;
            for (int i = 0; i < options.concurrencyReadsPerThread; ++i) {
                // Overlapping ranges, deliberately: threads reading disjoint
                // regions would not detect one caller's bytes landing in
                // another caller's buffer.
                const std::size_t size = static_cast<std::size_t>(
                    generator.Below(kNominalBlockSize) + 1);
                const std::uint64_t offset = generator.Below(fixture.size);

                buffer.assign(size + kGuardBytes, kGuardFill);
                const ReadResult result =
                    opened.reader->Read(offset, buffer.data(), size);
                ++comparisons;

                if (!result.status.IsOk()) {
                    ++mismatches;
                    continue;
                }
                const std::uint64_t remaining = fixture.size - offset;
                const std::size_t wanted = static_cast<std::size_t>(
                    remaining < size ? remaining : size);
                if (result.bytesRead != wanted) {
                    ++mismatches;
                    continue;
                }
                if (std::memcmp(buffer.data(), expected.data() + offset, wanted) != 0) {
                    ++mismatches;
                    continue;
                }
                for (std::size_t g = 0; g < kGuardBytes; ++g) {
                    if (buffer[size + g] != kGuardFill) {
                        ++mismatches;
                        break;
                    }
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    if (mismatches.load() != 0) {
        reporter.Fail("concurrent reads on one reader",
                      std::to_string(mismatches.load()) + " of " +
                          std::to_string(comparisons.load()) +
                          " concurrent reads disagreed with the oracle");
    } else {
        reporter.Pass();
    }

    // Concurrent readers over one asset, each with its own reader. This is the
    // concurrency that actually matters today: Hydra opens many assets at once.
    std::atomic<std::uint64_t> openFailures{0};
    std::vector<std::thread> openers;
    openers.reserve(static_cast<std::size_t>(options.concurrencyThreads));
    for (int t = 0; t < options.concurrencyThreads; ++t) {
        openers.emplace_back([&] {
            OpenResult own = OpenFixture(backend, fixture);
            if (!own.reader) {
                ++openFailures;
                return;
            }
            std::vector<unsigned char> buffer(4096 + kGuardBytes, kGuardFill);
            const ReadResult result = own.reader->Read(0, buffer.data(), 4096);
            if (!result.status.IsOk() || result.bytesRead != 4096 ||
                std::memcmp(buffer.data(), expected.data(), 4096) != 0) {
                ++openFailures;
            }
        });
    }
    for (std::thread& thread : openers) {
        thread.join();
    }

    if (openFailures.load() != 0) {
        reporter.Fail("concurrent readers on one asset",
                      std::to_string(openFailures.load()) + " reader(s) failed");
    } else {
        reporter.Pass();
    }

    return reporter.Finish();
}

// --- entry point -------------------------------------------------------------

int RunBoundarySuite(const BackendUnderTest& backend, int argc, char** argv) {
    SuiteOptions options;

    if (const char* seed = std::getenv("USD_ASSET_BOUNDARY_SEED")) {
        options.seed = std::strtoull(seed, nullptr, 0);
    }

    const std::string group = argc > 1 ? argv[1] : "all";
    if (group != "fixed" && group != "property" && group != "concurrent" &&
        group != "all") {
        std::fprintf(stderr,
                     "unknown group '%s'; expected fixed, property, concurrent, "
                     "or all\n",
                     group.c_str());
        return 2;
    }

    int failures = 0;
    if (group == "fixed" || group == "all") {
        failures += RunFixedCases(backend, options);
    }
    if (group == "property" || group == "all") {
        failures += RunPropertyCases(backend, options);
    }
    if (group == "concurrent" || group == "all") {
        failures += RunConcurrencyCases(backend, options);
    }
    return failures == 0 ? 0 : 1;
}

}  // namespace usdassetboundary
