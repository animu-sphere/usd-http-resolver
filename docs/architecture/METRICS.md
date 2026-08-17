# Metrics contract

This document defines the I/O counters that make the project's central claim
checkable.

Status: the counters in §2.1 and §2.3, the derived ratios in §2.4, the per-reader
lifetime and process aggregate in §3, and the environment-keyed dump in §5 are
implemented in `libs/usd-asset-io` (`usdAssetIo/Metrics.h`) and populated by both
backends. The HTTP counters populate, including the requests issued by validator
capture and by conditional range requests, and `retryCount` and `redirectCount`
are asserted from tests rather than assumed. The cache counters in §2.2 are
defined and stay at zero until `v0.3.0`.

No baseline in §6 is recorded yet. `v0.2.0` is the first release that *can*
record one — bytes now cross a network — and until it does, this project makes
no performance claim. What is missing is not instrumentation but a fixture: the
loopback corpus assets are kilobytes, and `selectivity` measured against them
would be a number without a meaning.

## 1. Why this is a contract and not a debug feature

The entire argument for this architecture is a ratio:

```text
asset size                       10 GB
bytes actually transferred       30 MB
amplification                    0.003
```

Without a counter, that sentence is a claim. With one, it is a test assertion.
Every performance statement in this repository's documentation must be traceable
to a recorded run of these counters on a named fixture, per §14 of the
[design policy](../design/DESIGN_POLICY.md).

The counters also catch the failure mode this design is most exposed to:
accidental amplification. A cache that over-fetches, a coalescing window that is
too wide, or a lost single-flight turns a 30 MB read into a 300 MB read while
every functional test still passes. The functional suite cannot see it. These
counters can.

## 2. Counters

### 2.1 Per asset

| Counter | Meaning |
| --- | --- |
| `assetSize` | Byte size reported at open |
| `bytesRequested` | Bytes the caller above asked for, summed |
| `bytesTransferred` | Bytes that actually crossed the transport |
| `bytesFromCache` | Bytes served without a transport request |
| `requestCount` | Transport requests issued, including metadata requests |
| `metadataRequestCount` | Of those, requests that fetched no content |
| `retryCount` | Requests re-issued after a failure |
| `redirectCount` | Redirects followed |

### 2.2 Cache

| Counter | Meaning |
| --- | --- |
| `blockHits` | Reads fully satisfied from cache |
| `blockMisses` | Blocks fetched from the transport |
| `partialHits` | Reads satisfied by a mix of cached and fetched blocks |
| `requestsSavedByCoalescing` | Requests avoided by merging adjacent blocks |
| `requestsSavedBySingleFlight` | Duplicate concurrent requests avoided |
| `bytesOverFetched` | Bytes fetched inside blocks the caller never read |
| `evictions` | Blocks dropped under the memory budget |
| `peakResidentBytes` | High-water mark of cached bytes |

`bytesOverFetched` is the honest counter. It is the cost of block alignment and
coalescing, and a design that reports only its savings is selling something.

### 2.3 Latency

Recorded as a distribution, not a mean — a mean latency over a network hides
exactly the tail that makes an interactive session feel broken.

| Counter | Meaning |
| --- | --- |
| `openLatency` | Time from open to usable metadata |
| `requestLatency` | p50, p90, p99, and max per transport request |
| `readLatency` | p50, p90, p99, and max per caller-visible read |

### 2.4 Derived ratios

```text
amplification         = bytesTransferred / bytesRequested
overFetchRatio        = bytesOverFetched / bytesTransferred
cacheHitRatio         = bytesFromCache / bytesRequested
requestEfficiency     = bytesTransferred / requestCount     (mean payload size)
selectivity           = bytesTransferred / assetSize
```

`selectivity` is the headline number: the fraction of a remote asset that was
actually moved to answer a query.

## 3. Scope and lifetime

Counters are per reader, aggregated per process. A reader's counters are
readable while it lives and are folded into the process aggregate when it
closes, so that a stage-wide total survives assets being opened and closed
during composition.

Counters are monotonic within a scope and are never reset by an internal
retry — a retried request counts as two requests and two transfers, because
that is what the network saw.

The same rule covers revision binding. A conditional range request that the
server refuses because the asset changed still cost a round trip, and it is
counted; the read that then fails with `AssetChanged` does not retract it. A
reader whose counters shrink after a failure is reporting what it wishes had
happened.

## 4. Cost

Instrumentation that measurably slows the fast path defeats itself. Counters
are relaxed atomics on a per-reader structure, not a shared global; latency
distributions are fixed-bucket histograms, not stored samples; and no counter
allocates on the read path.

## 5. Exposure

- Programmatic, for tests: the assertion target for the amplification tests in
  §11.3 of the design policy.
- A dump on request, keyed by an environment variable, for a human
  investigating a slow stage. It reports the aggregate and the top assets by
  `bytesTransferred`.
- Never automatic logging on the read path. A resolver that logs per request
  becomes the slow thing.

Identifiers reported in any dump have their query strings elided, per the
diagnostics rule on secrets.

## 6. Baselines

A release that changes I/O behavior records a baseline in its release record:
the fixture, the access pattern, and the counter values before and after. The
required scenarios are:

| Scenario | What it exercises |
| --- | --- |
| Metadata-only open | `openLatency`, `metadataRequestCount` — the cost of merely resolving |
| Header and index read | The clustered small-read pattern the block cache exists for |
| Bounded spatial query on a large asset | `selectivity` — the headline claim |
| Full sequential read | The worst case; must not be worse than a plain download |
| Parallel readers on one asset | `requestsSavedBySingleFlight`, contention |

The fourth row is the one that keeps the project honest. A range-based reader
that reads an entire asset must not lose badly to `curl`; if it does, the block
and coalescing policy is wrong.
