// SPDX-License-Identifier: Apache-2.0

#include "Report.h"

#include <cstdio>

namespace usdassetbaseline {
namespace {

std::string Unsigned(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llu",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

/// Six decimal places, because `selectivity` is the headline number and three
/// of them is where an interesting one lives. A ratio with no denominator
/// prints as an em dash rather than as zero: nobody measured it, and a zero
/// there would read as a measurement.
std::string Ratio(double value, bool defined) {
    if (!defined) return "—";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6f", value);
    return std::string(buffer);
}

std::string Millis(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    return std::string(buffer);
}

std::string Quantiles(const usdasset::LatencyHistogram::Summary& summary) {
    if (summary.count == 0) return "—";
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%llu / %llu / %llu / %llu",
                  static_cast<unsigned long long>(summary.p50),
                  static_cast<unsigned long long>(summary.p90),
                  static_cast<unsigned long long>(summary.p99),
                  static_cast<unsigned long long>(summary.max));
    return std::string(buffer);
}

std::string Mebibytes(std::uint64_t bytes) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
    return std::string(buffer);
}

}  // namespace

std::string FormatBaseline(const RunContext& context,
                           const std::vector<ScenarioRecord>& records) {
    std::string out;

    // No heading. What this renders is a fragment that a release record pastes
    // under its own "Recorded baseline" section and reference/BASELINE.md
    // pastes under its own, and a fragment that carried a heading would
    // duplicate one of them.
    out += "Fixture: a synthetic asset of " + Unsigned(context.assetBytes) +
           " bytes (" + Mebibytes(context.assetBytes) +
           " MiB) at `" + context.assetPath +
           "` on the loopback fixture server, ephemeral port, `Behavior::Normal`,"
           " strong `ETag`. Every byte is a hash of its own offset, so a read"
           " landing at the wrong offset returns data that is obviously from"
           " elsewhere, and every scenario below verifies the bytes it counted.\n\n";
    out += "Layout: a " + Unsigned(context.headerBytes) +
           "-byte header at offset 0, a " + Unsigned(context.indexBytes) +
           "-byte index in the tail, body between them.\n\n";
    out += "Measured with the shipped transport defaults, on " + context.platform +
           ", " + context.toolchain + ", " + context.config + ".\n\n";
    out += "| Scenario | requests | metadata | retries | redirects | bytes requested |"
           " bytes transferred | amplification | selectivity | wall ms |\n";
    out += "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n";

    for (const ScenarioRecord& record : records) {
        const usdasset::MetricsSnapshot& m = record.metrics;
        out += "| " + record.name;
        out += " | " + Unsigned(m.requestCount);
        out += " | " + Unsigned(m.metadataRequestCount);
        out += " | " + Unsigned(m.retryCount);
        out += " | " + Unsigned(m.redirectCount);
        out += " | " + Unsigned(m.bytesRequested);
        out += " | " + Unsigned(m.bytesTransferred);
        out += " | " + Ratio(m.Amplification(), m.bytesRequested != 0);
        out += " | " + Ratio(m.Selectivity(), m.assetSize != 0);
        out += " | " + Millis(record.wallMs);
        out += " |\n";
    }

    out += "\nLatency, in microseconds. Quantiles are bucket upper bounds, not"
           " exact order statistics (METRICS.md §4), and the request and read"
           " columns are p50 / p90 / p99 / max.\n\n";
    out += "| Scenario | open | request | read |\n";
    out += "| --- | ---: | ---: | ---: |\n";
    for (const ScenarioRecord& record : records) {
        const usdasset::MetricsSnapshot& m = record.metrics;
        out += "| " + record.name;
        out += " | " + (m.openLatency.count == 0 ? std::string("—")
                                                 : Unsigned(m.openLatency.max));
        out += " | " + Quantiles(m.requestLatency);
        out += " | " + Quantiles(m.readLatency);
        out += " |\n";
    }

    // Stated only when it is true of the run that printed it. The harness
    // asserts the same thing, so a report carrying the second sentence comes
    // from a failing run -- but a document is read long after the run that made
    // it, and this one is the place a claim without a counter behind it would
    // be least excusable.
    bool everyCacheCounterIsZero = true;
    std::uint64_t cached = 0;
    std::uint64_t overFetched = 0;
    for (const ScenarioRecord& record : records) {
        const usdasset::MetricsSnapshot& m = record.metrics;
        cached += m.bytesFromCache;
        overFetched += m.bytesOverFetched;
        everyCacheCounterIsZero =
            everyCacheCounterIsZero && m.bytesFromCache == 0 && m.blockHits == 0 &&
            m.blockMisses == 0 && m.partialHits == 0 &&
            m.requestsSavedByCoalescing == 0 && m.requestsSavedBySingleFlight == 0 &&
            m.bytesOverFetched == 0 && m.evictions == 0 && m.peakResidentBytes == 0;
    }

    if (everyCacheCounterIsZero) {
        out += "\nEvery cache counter in METRICS.md §2.2 is zero, and"
               " `bytesFromCache` with it, because no cache exists in this release"
               " -- not because none hit. `v0.3.0` is where these rows are expected"
               " to move, and the request counts above are what it has to move.\n";
    } else {
        out += "\nThe cache counters in METRICS.md §2.2 are no longer all zero: "
               "this run served " + Unsigned(cached) +
               " bytes from a cache and over-fetched " + Unsigned(overFetched) +
               ". A release whose cache moved is a release that records a new"
               " baseline rather than inheriting this one.\n";
    }

    out += "\n| Scenario | What it exercises | Notes |\n";
    out += "| --- | --- | --- |\n";
    for (const ScenarioRecord& record : records) {
        out += "| " + record.name + " | " + record.exercises + " | " +
               (record.note.empty() ? std::string("—") : record.note) + " |\n";
    }

    return out;
}

}  // namespace usdassetbaseline
