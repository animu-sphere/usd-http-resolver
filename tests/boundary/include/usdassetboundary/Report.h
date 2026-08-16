// SPDX-License-Identifier: Apache-2.0
//
// Failure reporting.
//
// The suite runs every case before it reports, rather than aborting on the
// first: when a backend gets the EOF boundary wrong, a dozen rows break at
// once, and the shape of which dozen is the diagnosis.

#ifndef USDASSETBOUNDARY_REPORT_H
#define USDASSETBOUNDARY_REPORT_H

#include <cstdint>
#include <string>

namespace usdassetboundary {

class Reporter {
public:
    Reporter(std::string backend, std::string group);

    void Pass();
    void Fail(const std::string& caseName, const std::string& detail);

    /// Counts without printing. The property cases run every candidate through
    /// a quiet reporter first, because a generated case that fails is about to
    /// be shrunk, and printing the unshrunk one alongside the minimal one
    /// buries the reproducer that matters.
    void Quiet(bool quiet) { _quiet = quiet; }

    /// A case the backend declared it does not admit. Printed, never silent: a
    /// skipped test that says nothing is indistinguishable from a passing one.
    void NotAdmitted(const std::string& caseName, const std::string& reason);

    /// Prints the summary and returns the process exit code.
    int Finish() const;

    std::uint64_t Failures() const { return _failures; }

private:
    std::string _backend;
    std::string _group;
    std::uint64_t _passes = 0;
    std::uint64_t _failures = 0;
    std::uint64_t _notAdmitted = 0;
    bool _quiet = false;
};

/// "fixture=several-blocks offset=196608 size=65536"
std::string DescribeRead(const std::string& fixture,
                         std::uint64_t offset,
                         std::size_t size);

}  // namespace usdassetboundary

#endif  // USDASSETBOUNDARY_REPORT_H
