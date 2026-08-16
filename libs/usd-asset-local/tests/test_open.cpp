// SPDX-License-Identifier: Apache-2.0
//
// What opening a local asset establishes, and how it fails.
//
// The read semantics themselves are not tested here: they are the shared
// boundary suite's subject, and duplicating them would let this module drift
// from the contract every other backend is held to.

#include "usdAssetLocal/LocalAssetReader.h"

#include <string>

#include "Check.h"
#include "TempAsset.h"

using namespace usdasset;
using usdassettest::PositionalContent;
using usdassettest::TempDirectory;
using usdassettest::WriteFile;

namespace {

void OpenReportsSizeAndRandomAccess() {
    TempDirectory directory("open");
    const std::string path = directory.File("asset.bin");
    CHECK(WriteFile(path, PositionalContent(4096)));

    local::LocalOpenResult result = local::Open(path);
    CHECK(result.status.IsOk());
    if (!result.reader) {
        return;
    }
    const AssetMetadata& metadata = result.reader->Metadata();
    CHECK_EQ(metadata.size, std::uint64_t{4096});
    CHECK(metadata.supportsRandomAccess);
    CHECK(metadata.resolvedIdentifier == path);
}

void AnEmptyAssetOpensSuccessfully() {
    TempDirectory directory("empty");
    const std::string path = directory.File("empty.bin");
    CHECK(WriteFile(path, {}));

    local::LocalOpenResult result = local::Open(path);
    CHECK(result.status.IsOk());
    if (result.reader) {
        CHECK_EQ(result.reader->Metadata().size, std::uint64_t{0});
    }
}

void AMissingAssetIsNotFoundAndNotATransportFault() {
    TempDirectory directory("missing");
    local::LocalOpenResult result = local::Open(directory.File("absent.bin"));
    CHECK(result.status.code == StatusCode::NotFound);
    CHECK(result.reader == nullptr);
    // An unreachable transport is not an absent asset, and the reverse holds
    // too: this must never surface as NetworkError.
    CHECK(result.status.code != StatusCode::NetworkError);
}

void ADirectoryIsAMalformedIdentifier() {
    TempDirectory directory("directory");
    // The identifier is legal and the target exists; it is simply not an
    // asset. That is the caller's mistake, not a permission problem.
    local::LocalOpenResult result = local::Open(directory.Path());
    CHECK(result.status.code == StatusCode::InvalidArgument);
    CHECK(result.reader == nullptr);
}

void AFailedOpenReturnsNoReader() {
    // A reader is never returned in a state where its size or range support is
    // unknown, which in practice means never returned at all on failure.
    TempDirectory directory("failed");
    local::LocalOpenResult result = local::Open(directory.File("absent.bin"));
    CHECK(!result.status.IsOk());
    CHECK(result.reader == nullptr);

    OpenResult shared = local::OpenAsset(directory.File("absent.bin"));
    CHECK(!shared.status.IsOk());
    CHECK(shared.reader == nullptr);
}

void TheValidatorIsDerivedAndStrong() {
    TempDirectory directory("validator");
    const std::string path = directory.File("asset.bin");
    CHECK(WriteFile(path, PositionalContent(1024)));

    local::LocalOpenResult result = local::Open(path);
    if (!result.reader) {
        CHECK(false);
        return;
    }
    const AssetMetadata& metadata = result.reader->Metadata();
    CHECK(metadata.validator.kind == ValidatorKind::Derived);
    CHECK(metadata.validator.strength == ValidatorStrength::Strong);
    CHECK(metadata.validator.IsUsable());
    CHECK(!metadata.validator.value.empty());
    // Stability is derived from strength, once, at open.
    CHECK(metadata.stability == IdentityStability::Stable);
}

void TwoAssetsDoNotShareAnIdentity() {
    TempDirectory directory("distinct");
    const std::string first = directory.File("a.bin");
    const std::string second = directory.File("b.bin");
    CHECK(WriteFile(first, PositionalContent(1024, 1)));
    CHECK(WriteFile(second, PositionalContent(1024, 2)));

    local::LocalOpenResult a = local::Open(first);
    local::LocalOpenResult b = local::Open(second);
    if (a.reader && b.reader) {
        // Same size, same directory, same second: the identity has to come
        // from more than the size.
        CHECK(a.reader->Metadata().validator != b.reader->Metadata().validator);
    } else {
        CHECK(false);
    }
}

void MetadataIsImmutableForTheReaderLifetime() {
    TempDirectory directory("immutable");
    const std::string path = directory.File("asset.bin");
    CHECK(WriteFile(path, PositionalContent(2048)));

    local::LocalOpenResult result = local::Open(path);
    if (!result.reader) {
        CHECK(false);
        return;
    }
    const std::uint64_t sizeAtOpen = result.reader->Metadata().size;
    const Validator validatorAtOpen = result.reader->Metadata().validator;

    // Republishing underneath the reader changes the file, and changes nothing
    // the reader reports about the revision it is bound to.
    CHECK(WriteFile(path, PositionalContent(9000, 7)));

    CHECK_EQ(result.reader->Metadata().size, sizeAtOpen);
    CHECK(result.reader->Metadata().validator == validatorAtOpen);
}

}  // namespace

int main() {
    OpenReportsSizeAndRandomAccess();
    AnEmptyAssetOpensSuccessfully();
    AMissingAssetIsNotFoundAndNotATransportFault();
    ADirectoryIsAMalformedIdentifier();
    AFailedOpenReturnsNoReader();
    TheValidatorIsDerivedAndStrong();
    TwoAssetsDoNotShareAnIdentity();
    MetadataIsImmutableForTheReaderLifetime();
    return usdassettest::Report("usdAssetLocal open");
}
