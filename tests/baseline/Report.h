// SPDX-License-Identifier: Apache-2.0
//
// The shape of a recorded baseline.
//
// This is a separate translation unit from the measurement for one reason: what
// a release record pastes is a *table*, and gate 6 compares one release's table
// against the previous one's. A format that drifts between releases makes that
// comparison a reading exercise. The columns are therefore fixed here, once,
// rather than printed inline wherever a scenario happens to finish.
//
// Nothing here measures anything. It is given counters and it renders them.

#ifndef USDASSETHTTP_BASELINE_REPORT_H
#define USDASSETHTTP_BASELINE_REPORT_H

#include <cstdint>
#include <string>
#include <vector>

#include "usdAssetIo/Metrics.h"

namespace usdassetbaseline {

/// What was measured, and on what. Recorded beside the numbers because a
/// counter without its fixture is not a baseline: METRICS.md §6 requires the
/// fixture, the access pattern, and the values, and a table carrying only the
/// third is the marketing statement §9 of the design policy refuses.
struct RunContext {
    std::uint64_t assetBytes = 0;
    std::uint64_t headerBytes = 0;
    std::uint64_t indexBytes = 0;

    std::string assetPath;   ///< Server path. The port is ephemeral and omitted.
    std::string config;      ///< Build configuration, from CMake.
    std::string toolchain;   ///< Compiler id and version, from CMake.
    std::string platform;    ///< System name and processor, from CMake.
};

/// One scenario's numbers.
struct ScenarioRecord {
    std::string name;
    std::string exercises;  ///< The METRICS.md §6 column, in that table's words.
    usdasset::MetricsSnapshot metrics;

    /// Wall clock for the whole scenario, open included. Reported and never
    /// asserted: it is a loopback number on whatever runner drew the job, and a
    /// gate on it would fail for reasons that are not this repository's.
    double wallMs = 0.0;

    /// Anything the numbers alone do not say -- the plain-download comparison,
    /// the reader count, what the next release is expected to move.
    std::string note;
};

/// The whole record, as GitHub-flavored markdown: a header naming the fixture,
/// the counter table, the latency table, and the per-scenario notes.
std::string FormatBaseline(const RunContext& context,
                           const std::vector<ScenarioRecord>& records);

}  // namespace usdassetbaseline

#endif  // USDASSETHTTP_BASELINE_REPORT_H
