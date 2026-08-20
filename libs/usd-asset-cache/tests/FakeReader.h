// SPDX-License-Identifier: Apache-2.0
//
// An in-memory reader to decorate, and a log of what the cache asked it for.
//
// The cache's whole job is to change how many requests reach the reader
// underneath and how many bytes they move. Asserting that against a real
// backend would be asserting the backend as well; against this it is one
// question with one answer. It implements the same read semantics through the
// same `ResolveReadRange`, so a case that passes here is a case about the
// cache.

#ifndef USDASSETCACHE_TESTS_FAKEREADER_H
#define USDASSETCACHE_TESTS_FAKEREADER_H

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "usdAssetIo/AssetReader.h"
#include "usdAssetIo/Metrics.h"
#include "usdAssetIo/RangeMath.h"

namespace usdassetcachetest {

/// Every byte a function of its own offset, so a read that landed at the wrong
/// offset cannot compare equal to what was asked for.
inline unsigned char ContentByte(std::uint64_t offset) {
    std::uint64_t mixed = offset * 0x9E3779B97F4A7C15ull;
    mixed ^= mixed >> 29;
    return static_cast<unsigned char>(mixed & 0xFF);
}

inline std::vector<unsigned char> MakeContent(std::uint64_t size) {
    std::vector<unsigned char> content(static_cast<std::size_t>(size));
    for (std::uint64_t i = 0; i < size; ++i) {
        content[static_cast<std::size_t>(i)] = ContentByte(i);
    }
    return content;
}

/// A latch a test can hold every reader inside `Read` on, so that N threads are
/// provably concurrent at the moment single-flight has to work.
class Gate {
public:
    void Arrive() {
        std::unique_lock<std::mutex> lock(_mutex);
        ++_arrived;
        _changed.notify_all();
        _changed.wait(lock, [this] { return _open; });
    }

    void WaitForArrivals(int count) {
        std::unique_lock<std::mutex> lock(_mutex);
        _changed.wait(lock, [this, count] { return _arrived >= count; });
    }

    void Open() {
        const std::lock_guard<std::mutex> lock(_mutex);
        _open = true;
        _changed.notify_all();
    }

    int Arrived() const {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _arrived;
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    int _arrived = 0;
    bool _open = false;
};

class FakeReader final : public usdasset::AssetReader {
public:
    struct Call {
        std::uint64_t offset = 0;
        std::size_t size = 0;
    };

    FakeReader(std::string identifier,
               std::vector<unsigned char> content,
               usdasset::Validator validator)
        : _content(std::move(content)), _metrics(identifier) {
        _metadata.resolvedIdentifier = std::move(identifier);
        _metadata.size = _content.size();
        _metadata.supportsRandomAccess = true;
        _metadata.validator = std::move(validator);
        _metadata.stability = usdasset::ClassifyStability(_metadata.validator);
        _metrics.SetAssetSize(_metadata.size);
    }

    const usdasset::AssetMetadata& Metadata() const override { return _metadata; }

    usdasset::ReadResult Read(std::uint64_t offset, void* dst, std::size_t size) override {
        _metrics.AddBytesRequested(size);
        const usdasset::ReadRange range =
            usdasset::ResolveReadRange(offset, size, _metadata.size);
        if (range.outcome == usdasset::ReadRangeOutcome::Overflow) {
            return usdasset::ReadResult{0, usdasset::OverflowStatus(offset, size)};
        }
        if (range.outcome == usdasset::ReadRangeOutcome::Empty) {
            return usdasset::ReadResult{0, usdasset::Status::Ok()};
        }

        {
            const std::lock_guard<std::mutex> lock(_mutex);
            _calls.push_back(Call{range.offset, range.length});
        }
        if (_gate) {
            _gate->Arrive();
        }

        usdasset::Status failure;
        {
            const std::lock_guard<std::mutex> lock(_mutex);
            if (_failuresRemaining > 0) {
                --_failuresRemaining;
                failure = _failure;
            }
        }
        if (!failure.IsOk()) {
            _metrics.AddRequest();
            return usdasset::ReadResult{0, failure};
        }

        _metrics.AddRequest();
        _metrics.AddBytesTransferred(range.length);
        std::memcpy(dst, _content.data() + range.offset, range.length);
        return usdasset::ReadResult{range.length, usdasset::Status::Ok()};
    }

    usdasset::ReaderMetrics& Metrics() noexcept { return _metrics; }

    std::size_t CallCount() const {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _calls.size();
    }

    std::vector<Call> Calls() const {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _calls;
    }

    std::uint64_t BytesRead() const {
        const std::lock_guard<std::mutex> lock(_mutex);
        std::uint64_t total = 0;
        for (const Call& call : _calls) {
            total += call.size;
        }
        return total;
    }

    void SetGate(Gate* gate) { _gate = gate; }

    /// Makes the next `count` reads fail with `status`, delivering nothing.
    void FailNext(int count, usdasset::Status status) {
        const std::lock_guard<std::mutex> lock(_mutex);
        _failuresRemaining = count;
        _failure = std::move(status);
    }

private:
    std::vector<unsigned char> _content;
    usdasset::AssetMetadata _metadata;
    usdasset::ReaderMetrics _metrics;

    mutable std::mutex _mutex;
    std::vector<Call> _calls;
    int _failuresRemaining = 0;
    usdasset::Status _failure;
    Gate* _gate = nullptr;
};

inline usdasset::Validator StrongValidator(std::string value) {
    usdasset::Validator validator;
    validator.value = std::move(value);
    validator.kind = usdasset::ValidatorKind::EntityTag;
    validator.strength = usdasset::ValidatorStrength::Strong;
    return validator;
}

inline usdasset::Validator WeakValidator(std::string value) {
    usdasset::Validator validator;
    validator.value = std::move(value);
    validator.kind = usdasset::ValidatorKind::EntityTag;
    validator.strength = usdasset::ValidatorStrength::Weak;
    return validator;
}

inline usdasset::Validator NoValidator() { return usdasset::Validator(); }

}  // namespace usdassetcachetest

#endif  // USDASSETCACHE_TESTS_FAKEREADER_H
