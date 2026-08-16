// SPDX-License-Identifier: Apache-2.0
//
// The oracle: a deliberately naive local reader.
//
// When the backend under test is the local backend, the first row of the
// equivalence table would otherwise be a tautology. So this shares no code with
// usdAssetLocal -- not its platform layer, and deliberately not even
// ResolveReadRange. The arithmetic below is duplicated on purpose: two
// independent expressions of the same rule disagreeing is a signal, and one
// expression checked against itself is not.
//
// docs/contributing/BOUNDARY_SUITE.md §2.

#ifndef USDASSETBOUNDARY_ORACLE_H
#define USDASSETBOUNDARY_ORACLE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "usdAssetIo/Diagnostics.h"

namespace usdassetboundary {

struct OracleResult {
    /// Exactly `bytesRead` long.
    std::vector<unsigned char> bytes;
    std::size_t bytesRead = 0;
    usdasset::StatusCode code = usdasset::StatusCode::Ok;
};

/// Open, seek, read, close. Nothing else.
OracleResult NaiveRead(const std::string& path, std::uint64_t offset, std::size_t size);

/// The whole file, for the concurrency cases, which compare many threads'
/// results against one expected image rather than re-reading per assertion.
std::vector<unsigned char> NaiveReadAll(const std::string& path);

}  // namespace usdassetboundary

#endif  // USDASSETBOUNDARY_ORACLE_H
