// SPDX-License-Identifier: Apache-2.0

#include "usdAssetLocal/LocalAssetReader.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

#include "usdAssetIo/RangeMath.h"
#include "usdAssetLocal/Testing.h"
#include "PlatformFile.h"

namespace usdasset {
namespace local {

// --- Impl --------------------------------------------------------------------

class LocalAssetReader::Impl {
public:
    Impl(detail::NativeHandle handle,
         AssetMetadata metadata,
         detail::FileIdentity identity,
         testing::ReadFault fault)
        : handle(handle),
          metadata(std::move(metadata)),
          openIdentity(identity),
          fault(std::move(fault)),
          metrics(this->metadata.resolvedIdentifier) {
        metrics.SetAssetSize(this->metadata.size);
    }

    ~Impl() { detail::Close(handle); }

    detail::NativeHandle handle;
    AssetMetadata metadata;         ///< Immutable for the reader's lifetime.
    detail::FileIdentity openIdentity;
    testing::ReadFault fault;       ///< Null outside a test.
    ReaderMetrics metrics;
};

LocalAssetReader::LocalAssetReader(std::unique_ptr<Impl> impl)
    : _impl(std::move(impl)) {}

LocalAssetReader::~LocalAssetReader() = default;

const AssetMetadata& LocalAssetReader::Metadata() const { return _impl->metadata; }

const ReaderMetrics& LocalAssetReader::Metrics() const noexcept {
    return _impl->metrics;
}

ReadResult LocalAssetReader::Read(std::uint64_t offset, void* dst, std::size_t size) {
    ScopedLatency readTimer(_impl->metrics.ReadLatency());
    _impl->metrics.AddBytesRequested(size);

    const ReadRange range = ResolveReadRange(offset, size, _impl->metadata.size);
    if (range.outcome == ReadRangeOutcome::Overflow) {
        return ReadResult{0, OverflowStatus(offset, size)};
    }
    // Before the empty check, not after it. A null buffer with a non-zero
    // length is a caller bug wherever the offset happens to land, and a check
    // that only fires when the range turns out to be non-empty leaves that bug
    // invisible until one day an offset lands inside the asset.
    if (size > 0 && dst == nullptr) {
        return ReadResult{0, Status::Error(StatusCode::InvalidArgument,
                                           "read destination buffer is null")
                                 .WithRange(offset, size)};
    }
    if (range.outcome == ReadRangeOutcome::Empty) {
        // Nothing to transfer and nothing wrong. No request is issued, which is
        // a counter assertion and not only a comment -- and it is why this
        // return does not revalidate: see the note on the revision check below.
        return ReadResult{0, Status::Ok()};
    }

    unsigned char* out = static_cast<unsigned char*>(dst);
    std::size_t done = 0;
    Status transport = Status::Ok();

    while (done < range.length) {
        std::size_t want = range.length - done;
        if (_impl->fault) {
            want = (std::min)(want, _impl->fault(offset + done, want));
        }
        if (want == 0) {
            break;  // An injected stall. No progress is a short read, below.
        }

        std::size_t delivered = 0;
        {
            ScopedLatency requestTimer(_impl->metrics.RequestLatency());
            _impl->metrics.AddRequest();
            transport = detail::PositionalRead(_impl->handle, out + done, want,
                                               offset + done, &delivered);
        }
        _impl->metrics.AddBytesTransferred(delivered);
        done += delivered;

        if (!transport.IsOk()) {
            break;
        }
        if (delivered == 0) {
            break;  // The file ended below the size captured at open.
        }
    }

    // The revision check, after the copy. A file that was republished
    // underneath this reader is reported as AssetChanged before anything else,
    // including before the truncation that a shrinking rewrite causes: the
    // cause is the republish, and reporting InvalidResponse would send a reader
    // to look for a corrupt file that does not exist.
    //
    // Only reads that reach the transport get here, which is deliberate and is
    // not a hole. AssetChanged exists to stop one reader composing bytes from
    // two revisions, and a read that returns no bytes cannot do that. A read at
    // or past the size captured at open is past the end *of the revision this
    // reader is bound to*, so `0, Ok` is that revision's truthful answer rather
    // than a stale one. Revalidating there would also make the rule
    // unimplementable for a range transport without a wasted round trip on
    // every read that transfers nothing, which is the amplification this
    // project exists to avoid.
    detail::FileIdentity current;
    const Status identityStatus = detail::Stat(_impl->handle, &current);
    _impl->metrics.AddMetadataRequest();
    if (identityStatus.IsOk() && current != _impl->openIdentity) {
        // bytesRead is zero rather than `done`. Those bytes may span two
        // revisions, and reporting them as read invites exactly the
        // composition this guarantee exists to prevent.
        return ReadResult{0,
                          Status::Error(StatusCode::AssetChanged,
                                        "asset was republished while it was open (" +
                                            ElideSecrets(
                                                _impl->metadata.resolvedIdentifier) +
                                            ")")
                              .WithRange(offset, size)};
    }

    if (!transport.IsOk()) {
        return ReadResult{done, transport};
    }
    if (done < range.length) {
        return ReadResult{done,
                          ShortReadStatus(offset, range.length, done,
                                          _impl->metadata.resolvedIdentifier)};
    }
    return ReadResult{done, Status::Ok()};
}

// --- Open --------------------------------------------------------------------

struct LocalReaderFactory {
    static LocalOpenResult Open(const std::string& path, testing::ReadFault fault) {
        // Timed from here rather than with a ScopedLatency, because the
        // histogram being timed into does not exist until the reader does, and
        // openLatency has to measure what the caller waited for.
        const std::chrono::steady_clock::time_point started =
            std::chrono::steady_clock::now();

        LocalOpenResult result;

        detail::NativeHandle handle = detail::InvalidHandle();
        Status status = detail::OpenForRead(path, &handle);
        if (!status.IsOk()) {
            result.status = std::move(status);
            return result;
        }

        detail::FileIdentity identity;
        status = detail::Stat(handle, &identity);
        if (!status.IsOk()) {
            detail::Close(handle);
            result.status = std::move(status);
            return result;
        }

        AssetMetadata metadata;
        metadata.resolvedIdentifier = path;
        metadata.size = identity.size;
        // A local file serves any byte range by construction. There is no
        // capability to discover and none to fail on.
        metadata.supportsRandomAccess = true;
        metadata.validator.value = detail::FormatIdentity(identity);
        metadata.validator.kind = ValidatorKind::Derived;
        // Strong, and this backend is the one entitled to say so: it holds the
        // handle open and re-derives the identity on every read, so equal
        // values mean the same bytes for as long as the reader lives. The
        // limit of that claim -- a filesystem whose timestamp resolution is
        // coarse enough to hide a same-size rewrite -- is in the module README.
        metadata.validator.strength = ValidatorStrength::Strong;
        metadata.stability = ClassifyStability(metadata.validator);

        std::unique_ptr<LocalAssetReader::Impl> impl(new LocalAssetReader::Impl(
            handle, std::move(metadata), identity, std::move(fault)));
        // The open and the stat above are the metadata round trip, and no
        // content was read.
        impl->metrics.AddMetadataRequest();
        impl->metrics.OpenLatency().Record(
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - started)
                                           .count()));

        result.reader.reset(new LocalAssetReader(std::move(impl)));
        return result;
    }
};

LocalOpenResult Open(const std::string& path) {
    return LocalReaderFactory::Open(path, nullptr);
}

OpenResult OpenAsset(const std::string& path) {
    LocalOpenResult local = Open(path);
    OpenResult result;
    result.status = std::move(local.status);
    result.reader = std::move(local.reader);
    return result;
}

namespace testing {

LocalOpenResult OpenWithReadFault(const std::string& path, ReadFault fault) {
    return LocalReaderFactory::Open(path, std::move(fault));
}

}  // namespace testing

}  // namespace local
}  // namespace usdasset
