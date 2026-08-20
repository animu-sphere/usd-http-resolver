# Recorded I/O baseline

This file holds the current record for the five scenarios in
[METRICS.md](../architecture/METRICS.md) §6. It describes what the tree measures
today, not what it intends to measure later, which is why it lives beside
[CAPABILITY_MATRIX.md](CAPABILITY_MATRIX.md).

Last recorded: 2026-08-20, against `main`, for `v0.3.0`.

A release record copies this table at its tag and never rewrites it; this file
is rewritten whenever I/O behavior changes, which is what
[gate 6](../releases/README.md) binds a release to. The two are not redundant:
the record is history and this is the present, and a release that finds them
disagreeing has found the regression the gate exists for.

`v0.3.0` is the first release to change these numbers on purpose. Every scenario
is therefore measured twice — once against the transport alone and once with the
block cache over it — because METRICS.md §6 asks a release that changes I/O
behavior to record *the counter values before and after*, and a table carrying
only the after would leave the next gate comparing a run against a document.
Why the cache's constants are the ones they are is a separate record:
[BLOCK_POLICY.md](BLOCK_POLICY.md).

## What is measured, and by what

`tests/baseline` is the harness. It stands up the loopback fixture server, serves
one large synthetic asset, and runs the five scenarios against it — twice each —
with the shipped transport defaults and the shipped cache defaults, because a
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

Gate 6 is a regression gate. A cache that over-fetches beyond its block size, a
retry nobody asked for, or a redirect that starts being followed is a byte count
that moves, and every functional test in this repository passes straight through
it — the boundary suite compares bytes against an oracle, and all of those bytes
would still be right.

## What is gated and what is reported

| Quantity | Treatment | Why |
| --- | --- | --- |
| Uncached rows: bytes requested, bytes transferred, request counts, retries, redirects | Asserted exactly | With no cache, a read of *n* bytes is one request that moves exactly *n* bytes. Anything else is over-fetch, a retry, or a redirect, and each of those is a defect until a release says otherwise |
| Cached rows: bytes transferred and request count | Asserted against the server's log, exactly | A cache makes the expected transfer a function of the block policy, and asserting the backend's counter against a number the harness computed from the same policy would be asserting the policy against itself |
| Cached rows: fewer requests than the row above | Asserted | This is the release's claim. It is checked against the uncached run of the same scenario in the same process, not against a number copied from a previous release |
| The full sequential read, cached against uncached | Asserted identical | Not "no worse by a margin". Every read in it is above the bypass threshold and never reaches the store, so any difference at all means the bypass stopped applying |
| `amplification`, `selectivity`, and the other derived ratios | Recorded | They are byte counts divided by the fixture size, and a gate on them would move when the fixture did |
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

Measured with the shipped transport defaults and the shipped cache defaults -- 64 KiB blocks, a gap of one block, a 1 MiB bypass threshold, a 128 MiB budget (see [BLOCK_POLICY.md](BLOCK_POLICY.md)) -- on Windows AMD64, MSVC 19.34.31937.0, Release.

| Scenario | requests | metadata | retries | redirects | bytes requested | bytes transferred | amplification | selectivity | wall ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| metadata-only open | 1 | 1 | 0 | 0 | 0 | 0 | — | 0.000000 | 2.0 |
| metadata-only open (cached) | 1 | 1 | 0 | 0 | 0 | 0 | — | 0.000000 | 0.9 |
| header and index read | 18 | 1 | 0 | 0 | 69632 | 69632 | 1.000000 | 0.000519 | 1.5 |
| header and index read (cached) | 3 | 1 | 0 | 0 | 69632 | 131072 | 1.882353 | 0.000977 | 0.6 |
| bounded spatial query | 19 | 1 | 0 | 0 | 331776 | 331776 | 1.000000 | 0.002472 | 1.5 |
| bounded spatial query (cached) | 18 | 1 | 0 | 0 | 331776 | 1507328 | 4.543210 | 0.011230 | 2.2 |
| full sequential read | 33 | 1 | 0 | 0 | 134217728 | 134217728 | 1.000000 | 1.000000 | 132.4 |
| full sequential read (cached) | 33 | 1 | 0 | 0 | 134217728 | 134217728 | 1.000000 | 1.000000 | 148.6 |
| parallel readers | 152 | 8 | 0 | 0 | 2654208 | 2654208 | 1.000000 | 0.019775 | 3.3 |
| parallel readers (cached) | 25 | 8 | 0 | 0 | 2654208 | 1507328 | 0.567901 | 0.011230 | 3.0 |


The cached parallel row is the one number in this record that is not identical
from run to run: it lands at 25 or 26 requests depending on which reader wins
each claim, because a reader that arrives while a block is in flight waits where
a reader arriving a microsecond later finds it resident. The harness therefore
asserts that it is *below* the uncached row rather than equal to a constant, and
this record states which run it came from rather than implying it is fixed.

Cache counters, for the rows that have them. Every one is zero on an uncached row, and those rows are omitted.

| Scenario | blockHits | blockMisses | partialHits | savedByCoalescing | savedBySingleFlight | bytesFromCache | bytesOverFetched | evictions | peakResidentBytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| metadata-only open (cached) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| header and index read (cached) | 15 | 2 | 0 | 0 | 0 | 61440 | 122880 | 0 | 131072 |
| bounded spatial query (cached) | 1 | 23 | 0 | 6 | 0 | 16384 | 1191936 | 0 | 1507328 |
| full sequential read (cached) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| parallel readers (cached) | 16 | 23 | 0 | 6 | 153 | 2338816 | 1191936 | 0 | 1507328 |

Latency, in microseconds. Quantiles are bucket upper bounds, not exact order statistics (METRICS.md §4), and the request and read columns are p50 / p90 / p99 / max.

| Scenario | open | request | read |
| --- | ---: | ---: | ---: |
| metadata-only open | 1979 | 1145 / 1145 / 1145 / 1145 | — |
| metadata-only open (cached) | 808 | 799 / 799 / 799 / 799 | — |
| header and index read | 444 | 63 / 255 / 437 / 437 | 63 / 127 / 143 / 143 |
| header and index read (cached) | 257 | 127 / 253 / 253 / 253 | 0 / 127 / 140 / 140 |
| bounded spatial query | 245 | 63 / 127 / 241 / 241 | 63 / 68 / 68 / 68 |
| bounded spatial query (cached) | 229 | 63 / 127 / 225 / 225 | 127 / 142 / 142 / 142 |
| full sequential read | 233 | 1309 / 1309 / 1309 / 1309 | 1312 / 1312 / 1312 / 1312 |
| full sequential read (cached) | 446 | 1023 / 2047 / 16627 / 16627 | 1023 / 2047 / 16630 / 16630 |
| parallel readers | 639 | 127 / 255 / 629 / 629 | 127 / 255 / 255 / 268 |
| parallel readers (cached) | 479 | 127 / 473 / 473 / 473 | 127 / 178 / 178 / 178 |

The cache counters in METRICS.md §2.2 are populated: the runs below served 2416640 bytes from a block store and over-fetched 2506752 to do it. Both halves belong in the record. A design that reported only the first would be selling something, which is what METRICS.md §2.2 calls `bytesOverFetched` the honest counter for.

| Scenario | What it exercises | Notes |
| --- | --- | --- |
| metadata-only open | `openLatency`, `metadataRequestCount` — the cost of merely resolving | One `HEAD`. No content byte crosses the transport, and the reader is bound to a revision before any read is issued |
| metadata-only open (cached) | `openLatency`, `metadataRequestCount` — the cost of merely resolving | One `HEAD`, unchanged. Binding a store costs no request, which is why this row is the one row the cache does not move |
| header and index read | The clustered small-read pattern the block cache exists for | One 4 KiB header read and 16 adjacent 4 KiB index reads, each its own request |
| header and index read (cached) | The clustered small-read pattern the block cache exists for | The same seventeen reads, collapsed onto 2 block fetches. The bytes the alignment moved beyond the reads are `bytesOverFetched`, and the reads that never reached the transport are `blockHits` |
| bounded spatial query | `selectivity` — the headline claim | A header, a tail index, and 16 scattered 16 KiB chunks: 331776 bytes moved to answer a query against an asset of 134217728 |
| bounded spatial query (cached) | `selectivity` — the headline claim | The same query, with every read expanded to whole blocks: 1507328 bytes moved against an asset of 134217728. `selectivity` is worse than the uncached row on purpose -- that is what alignment costs, and `bytesOverFetched` is the counter for it |
| full sequential read | The worst case; must not be worse than a plain download | 32 reads of 4 MiB against one plain `GET` of the whole asset over the fixture server's own raw client: identical content bytes, 33 requests against 1, 132.4 ms against 127.9 ms. The comparator reads 4 KiB at a time, so the times are recorded and not gated |
| full sequential read (cached) | The worst case; must not be worse than a plain download | 32 reads of 4 MiB against one plain `GET` of the whole asset over the fixture server's own raw client: identical content bytes, 33 requests against 1, 148.6 ms against 129.1 ms. Every read bypassed the cache, so this row is the uncached row and is asserted to be |
| parallel readers | `requestsSavedBySingleFlight`, contention | 8 readers running the bounded query at once, each with its own revision binding. Every request is issued 8 times, because nothing is shared between readers |
| parallel readers (cached) | `requestsSavedBySingleFlight`, contention | 8 readers running the bounded query at once, each with its own revision binding and all of them sharing one store. What they no longer share is the traffic: 25 requests against 152. `requestsSavedBySingleFlight` is 153 and `blockHits` is 16: those count blocks a reader did not have to fetch, not requests, so they do not subtract to the difference above and are not meant to |

## What the numbers say

**The clustered read is the release, and it is a factor of six.** Seventeen
reads of a header and an index cost eighteen requests in `v0.2.0` and cost three
here — one `HEAD` and two block fetches. Fifteen of the seventeen reads never
reached the transport at all. The price is 122880 bytes of alignment against
69632 bytes asked for, stated in `bytesOverFetched` rather than left to be
inferred, and it is the whole of what the row cost.

**Eight readers of one asset now move what one reader moves.** The parallel row
went from 152 requests to 25, and — the number worth stopping on — from 2654208
bytes to 1507328, which is *exactly the transfer of the single cached bounded
query above it*. Eight readers of one revision moved one reader's worth of
bytes. That is what the cache key buys: the eight have eight independent
revision bindings and one identity, so seven of them found the blocks resident
or waited on the flight that was already in the air.
`requestsSavedBySingleFlight` is 153, and it counts blocks rather than requests,
so it is not the arithmetic difference of the two request counts and is not
meant to be.

**`selectivity` got worse, on purpose, and is still the headline.** The bounded
query moved 0.0025 of the asset before and moves 0.0112 now. Alignment converts
request count into transferred bytes — §1 of [CACHE.md](../architecture/CACHE.md)
says so before any of this was built — and one percent of a 128 MiB asset to
answer a query against it is still the sentence the architecture is made of. The
request count barely moved on that row, 19 to 18, because sixteen chunks 8 MiB
apart are sixteen requests whatever the block size; that pattern is not what a
block cache is for, and the record shows it not being helped rather than
implying it was.

**The worst case did not move at all.** The full sequential read is 33 requests
and 134217728 bytes with the cache and without it, and the harness asserts the
two rows are *identical* rather than merely close. `BASELINE.md` said in
`v0.2.0` that a release which improved the first three rows and quietly damaged
the fourth had not improved anything. The bypass rule in CACHE.md §3 is what
keeps it, and this row is where it is checked.

**`bytesOverFetched` is 2506752 across the whole run.** It belongs in the
record. A design that reported only what the cache saved would be selling
something, and the honest form of "the clustered read went from 18 requests to
3" is "for 61440 bytes nobody asked for".

**Nothing here is a network measurement.** Loopback has no round-trip time worth
the name, so the latency columns describe this process rather than a CDN, and
the trade this release makes — bytes for round trips — is one loopback cannot
price. What loopback does measure exactly is how many bytes and how many
requests a pattern costs, and that is the whole of what gate 6 is about. A
measurement over real distance arrives with the first consumer integration in
`v0.5.0`, against a fixture of at least a gigabyte; see
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
the first and disagree on the second. The sanitizer lanes run the same scenarios
against an 8 MiB fixture — 128 MiB of instrumented `memcpy` is a lane that times
out rather than a lane that measures — and they are there for the counter
assertions under a data race, not for the numbers.

## What the next release has to move

`v0.4.0` adds persistence, and it changes what a hit is worth rather than what a
block is. The rows it is measured against are these:

| Row | Now | `v0.4.0` |
| --- | --- | --- |
| Every scenario, on a second open of the same asset | The whole cost again: a new process starts cold | Cheaper, for a `Stable` identity only. A `Weak` or `Unavailable` one must still start cold, and a row showing otherwise is the stale-data generator CACHE.md §8 refuses |
| `metadata-only open` | 1 request | Unchanged. A persistent cache that skipped the `HEAD` would be reusing an identity it had not revalidated |
| Full sequential read | 33 requests, `amplification` 1.000000 | Must not regress, again. The rule does not get weaker because the cache got bigger |

The scenario this file still cannot record is the one that would price the trade
it made. `v0.5.0` is where that arrives.
