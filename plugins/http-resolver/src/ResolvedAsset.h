// SPDX-License-Identifier: Apache-2.0
//
// The `ArAsset` over an `AssetReader`: RESOLVER.md §4.
//
// It has no policy of its own (WORKSPACE.md §4). It owns a reader, forwards
// three calls, converts a typed failure into an OpenUSD diagnostic, and returns
// null for the two entry points that ask for whole-asset materialization. When
// this class starts assembling ranges or interpreting responses, the boundary
// has moved.

#ifndef USDHTTPRESOLVER_RESOLVEDASSET_H
#define USDHTTPRESOLVER_RESOLVEDASSET_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include "pxr/pxr.h"
#include "pxr/usd/ar/asset.h"

#include "usdAssetIo/AssetReader.h"
#include "usdAssetIo/Metrics.h"

PXR_NAMESPACE_OPEN_SCOPE

/// One open remote asset, bound to the revision it had when it was opened.
///
/// The binding is the reader's, not this class's, and it is why an `ArAsset`
/// here is not interchangeable with re-resolving the same URL: two
/// `HttpResolvedAsset`s over one URL may be reading two revisions, and each is
/// individually consistent. That is the guarantee ASSET_READER.md §2.1 makes
/// and the reason it is made below this layer.
class HttpResolvedAsset final : public ArAsset {
public:
    /// `reader` must be non-null: an asset that cannot state its size cannot
    /// serve random access, and a backend never returns a reader in that state.
    ///
    /// `metrics` is the reader's own counter block, or null for a reader that
    /// does not publish one. It is taken as a pointer rather than reached
    /// through `reader` because metrics are deliberately not part of the read
    /// contract -- `AssetReader` has no accessor for them, and widening the one
    /// interface this project keeps narrow in order to emit a warning would be
    /// the wrong trade. It is what makes `HTTP101` reportable: a retry that
    /// succeeded is invisible in the return value and still cost the latency
    /// somebody is investigating.
    HttpResolvedAsset(std::unique_ptr<usdasset::AssetReader> reader,
                      const usdasset::ReaderMetrics* metrics);

    ~HttpResolvedAsset() override;

    size_t GetSize() const override;

    /// Null, permanently, per RESOLVER.md §4.1.
    ///
    /// This is a compatibility contract rather than an omission. `GetBuffer`
    /// asks for the whole asset in memory, and honouring it for a 10 GB remote
    /// asset is precisely the transfer this project exists to avoid, so
    /// implementing it would contradict the project's purpose rather than
    /// complete its API surface. A `SdfFileFormat` that reads through `Read` at
    /// offsets it computes itself is unaffected; one that requires a whole
    /// buffer is not remote-capable through this resolver, which RESOLVER.md
    /// §4.2 states as a bounded claim rather than a gap to close.
    std::shared_ptr<const char> GetBuffer() const override;

    /// Reads `count` bytes at `offset`, with the semantics of §3 of
    /// ASSET_READER.md.
    ///
    /// `ArAsset::Read` can only say "0" for a failure, so the distinction that
    /// matters -- truncation at EOF is success, truncation below EOF is a
    /// failure -- survives as a diagnostic rather than as a return value. A
    /// short read at EOF returns the available remainder and posts nothing; a
    /// failure posts its `HTTPxxx` code and returns 0, including when the
    /// backend had already placed bytes in the buffer. Reporting those bytes as
    /// read is what would let a caller compose two revisions.
    size_t Read(void* buffer, size_t count, size_t offset) const override;

    /// `{nullptr, 0}`: there is no file. RESOLVER.md §4.1.
    std::pair<FILE*, size_t> GetFileUnsafe() const override;

private:
    /// Posts `HTTP101` for retries that have happened since the last time it
    /// was called. Called after every read, and once at construction, so that
    /// a retry is reported when it happened rather than at close.
    void _ReportNewRetries() const;

    std::unique_ptr<usdasset::AssetReader> _reader;
    const usdasset::ReaderMetrics* _metrics;
    std::string _identifier;  ///< Passed through `ElideSecrets` before use.

    /// Retries already reported. Mutable and atomic because `Read` is `const`
    /// and concurrent, and because over-reporting a retry is noise while
    /// under-reporting one is the silence DIAGNOSTICS.md §3 refuses.
    mutable std::atomic<std::uint64_t> _reportedRetries{0};
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // USDHTTPRESOLVER_RESOLVEDASSET_H
