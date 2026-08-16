// SPDX-License-Identifier: Apache-2.0
//
// The shared boundary suite.
//
// This is the deliverable, not the local file reader that arrives with it. It
// is the executable form of the read contract in ASSET_READER.md, and every
// later backend -- HTTP, and after it S3, package-internal, and Wasm -- is
// admitted by it unchanged. The exit criterion it exists to satisfy is that
// swapping the backend under test is a one-line change.
//
// docs/contributing/BOUNDARY_SUITE.md.

#ifndef USDASSETBOUNDARY_SUITE_H
#define USDASSETBOUNDARY_SUITE_H

#include <cstdint>

#include "usdassetboundary/Backend.h"

namespace usdassetboundary {

struct SuiteOptions {
    /// Fixed by default, so that a CI failure is a real failure rather than a
    /// seed nobody can reproduce. Overridden by USD_ASSET_BOUNDARY_SEED, and
    /// reported on every failure so that a run can be repeated exactly.
    std::uint64_t seed = 0x5EEDB00DB0554Eull;
    int propertyIterations = 2000;
    int concurrencyThreads = 8;
    int concurrencyReadsPerThread = 250;
};

int RunFixedCases(const BackendUnderTest& backend, const SuiteOptions& options);
int RunPropertyCases(const BackendUnderTest& backend, const SuiteOptions& options);
int RunConcurrencyCases(const BackendUnderTest& backend, const SuiteOptions& options);

/// Entry point for a backend's suite executable.
///
/// `argv[1]` selects a group -- fixed, property, or concurrent -- so that a CI
/// failure names which part of the contract broke instead of one opaque red
/// test. With no argument, runs all three.
int RunBoundarySuite(const BackendUnderTest& backend, int argc, char** argv);

}  // namespace usdassetboundary

#endif  // USDASSETBOUNDARY_SUITE_H
