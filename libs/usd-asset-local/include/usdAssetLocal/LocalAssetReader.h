// SPDX-License-Identifier: Apache-2.0
//
// The local-file backend.
//
// This module exists to be correct and boring. Every property of every later
// transport is expressed as an equivalence against it -- over bytes, byte
// count, and status code alike -- so a subtlety introduced here is a subtlety
// every backend inherits as "correct".
//
// Normative contracts:
//   docs/architecture/ASSET_READER.md   read semantics and backend obligations
//   docs/architecture/DIAGNOSTICS.md    the typed vocabulary this maps onto
//   docs/architecture/METRICS.md        the counters it populates

#ifndef USDASSETLOCAL_LOCALASSETREADER_H
#define USDASSETLOCAL_LOCALASSETREADER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "usdAssetIo/AssetReader.h"
#include "usdAssetIo/Metrics.h"

namespace usdasset {
namespace local {

/// A reader over one open file handle, bound to the revision that file had when
/// it was opened.
///
/// Positional reads only: the file pointer is never relied on, so any number of
/// threads may call `Read` concurrently on one reader without a lock.
class LocalAssetReader final : public AssetReader {
public:
    ~LocalAssetReader() override;

    const AssetMetadata& Metadata() const override;

    /// Reads up to `size` bytes at `offset`. See `AssetReader::Read` for the
    /// semantics; this backend implements them exactly, and the shared boundary
    /// suite is what says so.
    ///
    /// The revision check happens after the bytes are copied rather than
    /// before. An identity captured before a copy proves nothing about the
    /// bytes copied after it, and the check that runs afterwards catches
    /// everything an earlier one would have, plus the race between them.
    ReadResult Read(std::uint64_t offset, void* dst, std::size_t size) override;

    /// This reader's counters, readable while it lives. They fold into the
    /// process aggregate when it closes.
    ///
    /// Exposed on the concrete backend rather than on `AssetReader`: metrics
    /// are not part of the read contract, and widening that contract to carry
    /// them would put an accessor on the one interface this project keeps
    /// deliberately narrow.
    const ReaderMetrics& Metrics() const noexcept;

private:
    class Impl;
    explicit LocalAssetReader(std::unique_ptr<Impl> impl);

    friend struct LocalReaderFactory;

    std::unique_ptr<Impl> _impl;
};

/// The result of opening a local asset, typed to the concrete reader.
struct LocalOpenResult {
    std::unique_ptr<LocalAssetReader> reader;  ///< Null exactly when `status` fails.
    Status status;
};

/// Opens `path`.
///
/// Performs exactly one metadata round trip -- an open and a single stat of the
/// resulting handle -- and never reads content. A reader is never returned in a
/// state where its size or range support is unknown.
///
/// `path` is a filesystem path, not a URI. Turning a `file://` URI into one is
/// the resolver's job, not this module's.
LocalOpenResult Open(const std::string& path);

/// The same open, in the shape every backend returns.
///
/// This is what the shared boundary suite and, from v0.2.0, the resolver call.
/// Entering a backend into the suite is a row, and this is that row's factory.
OpenResult OpenAsset(const std::string& path);

}  // namespace local
}  // namespace usdasset

#endif  // USDASSETLOCAL_LOCALASSETREADER_H
