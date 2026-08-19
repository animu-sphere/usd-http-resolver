# Recorded I/O baseline

This file holds the current record for the five scenarios in
[METRICS.md](../architecture/METRICS.md) §6. It describes what the tree measures
today, not what it intends to measure later, which is why it lives beside
[CAPABILITY_MATRIX.md](CAPABILITY_MATRIX.md).

Last recorded: 2026-08-19, against `main`.

A release record copies this table at its tag and never rewrites it; this file
is rewritten whenever I/O behavior changes, which is what
[gate 6](../releases/README.md) binds a release to. The two are not redundant:
the record is history and this is the present, and a release that finds them
disagreeing has found the regression the gate exists for.

Until this file existed, this project made no performance claim. `v0.2.0` is the
first release that could produce one, because it is the first release in which
bytes cross a network.

## What is measured, and by what

`tests/baseline` is the harness. It stands up the loopback fixture server, serves
one large synthetic asset, and runs the five scenarios against it through the
HTTP backend with the shipped transport defaults — the deadlines, the redirect
bound, and the attempt count a caller gets when it passes nothing, because a
baseline measured with a configuration that does not ship is a baseline about
something else.

Every scenario verifies the bytes it counted. Each byte of the fixture is a hash
of its own offset, so a read that landed at the wrong offset cannot compare
equal, and a number produced over unchecked bytes would be a measurement of the
wrong thing arriving quickly.

The harness is registered as a test rather than kept as a tool a release run
remembers to invoke:

```sh
ctest --test-dir build/core -R usdAssetHttp_io_baseline
```

Gate 6 is a regression gate. A cache that over-fetches, a retry nobody asked
for, or a redirect that starts being followed is a byte count that moves, and
every functional test in this repository passes straight through it — the
boundary suite compares bytes against an oracle, and all of those bytes would
still be right.

## What is gated and what is reported

| Quantity | Treatment | Why |
| --- | --- | --- |
| Bytes requested, bytes transferred, request counts, retries, redirects | Asserted exactly | With no cache, a read of *n* bytes is one request that moves exactly *n* bytes. Anything else is over-fetch, a retry, or a redirect, and each of those is a defect until a release says otherwise |
| `amplification`, `selectivity`, and the other derived ratios | Recorded | They are the byte counts divided by the fixture size, and a gate on them would move when the fixture did |
| Latency and wall clock | Recorded | Loopback has no bandwidth-delay product. These are numbers about this process on this machine, and a lane that failed on them would fail for reasons that are not this repository's |

Every request count is asserted twice: once against the backend's own counter,
and once against the number of requests the fixture server logged answering.
That second check is what makes the first worth anything. Gate 6 is precisely
the gate a self-report can be wrong about — a request issued outside the metrics
sink costs a round trip and counts nothing — and a lane watching only the sink
stays green while the wire traffic doubles. The server's log is the independent
witness, the same separation the plain-download comparator keeps from the client
under test.

The one comparison in the table that is not a counter is the fourth scenario's.
"Must not be worse than a plain download" needs a plain download, performed by
something that is not the client under test, so it is performed by the fixture
server's own raw client — a naive socket, one `GET`, no `Range`, and no HTTP
code shared with the backend. Its byte count is the gate. Its wall clock is not:
it reads 4 KiB at a time, which flatters the backend, and a comparison that
flatters is recorded rather than asserted.

## The record

Fixture: a synthetic asset of 134217728 bytes (128.0 MiB) at `/baseline/asset.bin` on the loopback fixture server, ephemeral port, `Behavior::Normal`, strong `ETag`. Every byte is a hash of its own offset, so a read landing at the wrong offset returns data that is obviously from elsewhere, and every scenario below verifies the bytes it counted.

Layout: a 4096-byte header at offset 0, a 65536-byte index in the tail, body between them.

Measured with the shipped transport defaults, on Windows AMD64, MSVC 19.34.31937.0, Release.

| Scenario | requests | metadata | retries | redirects | bytes requested | bytes transferred | amplification | selectivity | wall ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| metadata-only open | 1 | 1 | 0 | 0 | 0 | 0 | — | 0.000000 | 1.4 |
| header and index read | 18 | 1 | 0 | 0 | 69632 | 69632 | 1.000000 | 0.000519 | 1.4 |
| bounded spatial query | 19 | 1 | 0 | 0 | 331776 | 331776 | 1.000000 | 0.002472 | 1.5 |
| full sequential read | 33 | 1 | 0 | 0 | 134217728 | 134217728 | 1.000000 | 1.000000 | 131.0 |
| parallel readers | 152 | 8 | 0 | 0 | 2654208 | 2654208 | 1.000000 | 0.019775 | 3.5 |

Latency, in microseconds. Quantiles are bucket upper bounds, not exact order statistics (METRICS.md §4), and the request and read columns are p50 / p90 / p99 / max.

| Scenario | open | request | read |
| --- | ---: | ---: | ---: |
| metadata-only open | 1350 | 632 / 632 / 632 / 632 | — |
| header and index read | 327 | 63 / 127 / 322 / 322 | 63 / 88 / 88 / 88 |
| bounded spatial query | 257 | 63 / 127 / 253 / 253 | 63 / 84 / 84 / 84 |
| full sequential read | 264 | 1455 / 1455 / 1455 / 1455 | 1458 / 1458 / 1458 / 1458 |
| parallel readers | 854 | 127 / 255 / 840 / 840 | 127 / 249 / 249 / 249 |

Every cache counter in METRICS.md §2.2 is zero, and `bytesFromCache` with it, because no cache exists in this release -- not because none hit. `v0.3.0` is where these rows are expected to move, and the request counts above are what it has to move.

| Scenario | What it exercises | Notes |
| --- | --- | --- |
| metadata-only open | `openLatency`, `metadataRequestCount` — the cost of merely resolving | One `HEAD`. No content byte crosses the transport, and the reader is bound to a revision before any read is issued |
| header and index read | The clustered small-read pattern the block cache exists for | One 4 KiB header read and 16 adjacent 4 KiB index reads. Every one of them is its own request today, which is the number `v0.3.0` exists to collapse |
| bounded spatial query | `selectivity` — the headline claim | A header, a tail index, and 16 scattered 16 KiB chunks: 331776 bytes moved to answer a query against an asset of 134217728 |
| full sequential read | The worst case; must not be worse than a plain download | 32 reads of 4 MiB against one plain `GET` of the whole asset over the fixture server's own raw client: identical content bytes, 33 requests against 1, 131.0 ms against 120.0 ms. The comparator reads 4 KiB at a time, so the times are recorded and not gated |
| parallel readers | `requestsSavedBySingleFlight`, contention | 8 readers running the bounded query at once, each with its own revision binding. Every request is issued 8 times, because nothing is shared between readers yet; that is the figure `requestsSavedBySingleFlight` has to move in `v0.3.0` |

## What the numbers say

**The headline is `selectivity` on the bounded query: 0.0025.** A query that
read a header, an index, and sixteen scattered chunks moved 324 KiB of a 128 MiB
asset — a quarter of one percent of it — and every byte it moved was a byte the
caller asked for. That is the sentence §1 of METRICS.md says the architecture is
made of, in the form §14 of the [design policy](../design/DESIGN_POLICY.md)
requires it to be made in: a counter on a named fixture rather than a claim.

**`amplification` is exactly 1.000000 in every scenario that moved a byte**, and
that is the more interesting number, because it is the one that can only get
worse. There is no cache, so a read of *n* bytes is one request for exactly *n*
bytes: no block alignment, no read-ahead, no coalescing window, and nothing
over-fetched. `v0.3.0` will trade that number for a smaller request count, and
this row is what the trade is measured against.

**The request counts are the cost of having no cache, stated plainly.** Sixteen
adjacent 4 KiB index reads are sixteen requests; eight readers doing identical
work do it eight times over. Neither is a defect — every read is a request in
this release, deliberately, so that the request pattern is visible before it is
optimized — but both are the numbers `v0.3.0` exists to move, and neither could
have been argued about before it was counted.

**The full sequential read does not lose to a plain download.** Thirty-three
requests against one moved the same content bytes in a comparable time on
loopback, which is what row 4 of METRICS.md §6 asks: a range-based reader that
reads an entire asset must not lose badly to `curl`. The per-request overhead is
visible in the request count and not in the byte count, which is where it should
be.

**Nothing here is a network measurement.** Loopback has no round-trip time worth
the name, so the latency columns describe this process rather than a CDN. What
loopback does measure exactly is how many bytes and how many requests a pattern
costs, and that is the whole of what gate 6 is about. A measurement over real
distance arrives with the first consumer integration in `v0.5.0`, against a
fixture of at least a gigabyte; see
[consumer integration](../roadmap/consumer-integration.md).

## Reproducing it

The harness takes the fixture size from the environment and writes its report
wherever it is told:

```sh
cmake --preset core && cmake --build --preset core
./build/core/tests/baseline/usdAssetHttp_baseline --output baseline.md
USD_ASSET_BASELINE_ASSET_BYTES=536870912 ./build/core/tests/baseline/usdAssetHttp_baseline
```

The default is 128 MiB, chosen so that the bounded query lands near the ratio
the architecture's argument is stated as rather than at a number that only says
the fixture was small, and so that a CI cell moves it in about a second. The
report states the size it used, so no run of it can be mistaken for another.

The byte counts are the same on every platform and in every configuration; the
times are not, and a run reproduced on other hardware is expected to agree on
the first and disagree on the second. That is not an aspiration: three
consecutive runs on the machine above produced byte-identical counter tables and
a different latency table each time, which is the property that makes the
counters a gate and the durations a note. The sanitizer lanes run the same scenarios
against an 8 MiB fixture — 128 MiB of instrumented `memcpy` is a lane that times
out rather than a lane that measures — and they are there for the counter
assertions under a data race, not for the numbers.

## What the next release has to move

`v0.3.0` is the first release that will change these numbers on purpose, and the
direction each one moves is the release's own definition of success:

| Row | Now | `v0.3.0` |
| --- | --- | --- |
| Header and index read, requests | 18 | Fewer: sixteen adjacent 4 KiB reads inside one 64 KiB region are one or two block fetches |
| Bounded query, `amplification` | 1.000000 | Above 1.0, by the block size — and `bytesOverFetched` becomes non-zero, which is the honest counter for it |
| Parallel readers, requests | 152 | Fewer, by `requestsSavedBySingleFlight`, which is 0 here because nothing is shared between readers yet |
| Full sequential read | 33 requests, `amplification` 1.000000 | Must not regress. A cache that turns the worst case into a worse case has the wrong policy |

A release that improves the first three and quietly damages the fourth has not
improved anything, which is why all five scenarios are recorded together and why
the fourth is in the list at all.
