// SPDX-License-Identifier: Apache-2.0

#include "ResolvedAsset.h"

#include <cstdint>
#include <limits>

#include "Report.h"

PXR_NAMESPACE_OPEN_SCOPE

HttpResolvedAsset::HttpResolvedAsset(
    std::unique_ptr<usdasset::AssetReader> reader,
    const usdasset::ReaderMetrics* metrics)
    : _reader(std::move(reader)),
      _metrics(metrics),
      _identifier(_reader ? _reader->Metadata().resolvedIdentifier
                          : std::string()) {
    // The open itself may have been retried -- a redirect chain, a `503` that
    // cleared. Reported here rather than by the resolver, so that there is one
    // place that knows how many retries have already been named.
    _ReportNewRetries();
}

HttpResolvedAsset::~HttpResolvedAsset() = default;

size_t HttpResolvedAsset::GetSize() const {
    return static_cast<size_t>(_reader->Metadata().size);
}

std::shared_ptr<const char> HttpResolvedAsset::GetBuffer() const {
    return nullptr;
}

size_t HttpResolvedAsset::Read(void* buffer, size_t count,
                               size_t offset) const {
    const usdasset::ReadResult result =
        _reader->Read(static_cast<std::uint64_t>(offset), buffer, count);

    _ReportNewRetries();

    if (!result.status.IsOk()) {
        // A failed read reports zero bytes even when some were placed in the
        // buffer. The bytes may span two revisions, and returning them would
        // be the composition ASSET_READER.md §2.1 exists to prevent -- reported
        // as a partial success, which no caller checks.
        usdhttpresolver::Report(result.status, _identifier);
        return 0;
    }
    return result.bytesRead;
}

std::pair<FILE*, size_t> HttpResolvedAsset::GetFileUnsafe() const {
    return {nullptr, 0};
}

void HttpResolvedAsset::_ReportNewRetries() const {
    if (_metrics == nullptr) return;

    const std::uint64_t retries = _metrics->Snapshot().retryCount;
    std::uint64_t reported = _reportedRetries.load(std::memory_order_relaxed);
    while (retries > reported) {
        if (_reportedRetries.compare_exchange_weak(reported, retries,
                                                   std::memory_order_relaxed)) {
            usdhttpresolver::ReportRetries(retries - reported, _identifier);
            return;
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
