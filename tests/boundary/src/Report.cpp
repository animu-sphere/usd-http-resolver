// SPDX-License-Identifier: Apache-2.0

#include "usdassetboundary/Report.h"

#include <cstdio>
#include <utility>

namespace usdassetboundary {

Reporter::Reporter(std::string backend, std::string group)
    : _backend(std::move(backend)), _group(std::move(group)) {}

void Reporter::Pass() { ++_passes; }

void Reporter::Fail(const std::string& caseName, const std::string& detail) {
    ++_failures;
    if (_quiet) {
        return;
    }
    std::fprintf(stderr, "FAIL [%s/%s] %s\n      %s\n", _backend.c_str(),
                 _group.c_str(), caseName.c_str(), detail.c_str());
}

void Reporter::NotAdmitted(const std::string& caseName, const std::string& reason) {
    ++_notAdmitted;
    std::printf("not admitted [%s/%s] %s: %s\n", _backend.c_str(), _group.c_str(),
                caseName.c_str(), reason.c_str());
}

int Reporter::Finish() const {
    std::printf("%s/%s: %llu passed, %llu failed, %llu not admitted\n",
                _backend.c_str(), _group.c_str(),
                static_cast<unsigned long long>(_passes),
                static_cast<unsigned long long>(_failures),
                static_cast<unsigned long long>(_notAdmitted));
    std::fflush(stdout);
    return _failures == 0 ? 0 : 1;
}

std::string DescribeRead(const std::string& fixture,
                         std::uint64_t offset,
                         std::size_t size) {
    return "fixture=" + fixture + " offset=" + std::to_string(offset) +
           " size=" + std::to_string(size);
}

}  // namespace usdassetboundary
