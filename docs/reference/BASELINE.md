# Recorded I/O baseline

This file holds the current record for the six scenarios in
[METRICS.md](../architecture/METRICS.md) §6 — five from `v0.2.0`, and the sixth
that `v0.4.0` added when it gave a second open something to reuse. It describes what the tree measures
today, not what it intends to measure later, which is why it lives beside
[CAPABILITY_MATRIX.md](CAPABILITY_MATRIX.md).

Last recorded: 2026-08-22, against `main`, for `v0.4.0`.

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

`v0.4.0` changes them again, in one place and on purpose. Its sixth scenario,
`bounded query, reopened`, is the row this file named a release in advance: the
same query paid for a second time, once by a reader with nothing to reuse and
once by a reader whose block store is empty and whose cache directory an earlier
reader filled. Nothing else in the table moves, which is the other half of the
claim — a persistent cache that changed the metadata cost or the worst case
would have bought its row at the expense of two others.

## What is measured, and by what

`tests/baseline` is the harness. It stands up the loopback fixture server, serves
one large synthetic asset, and runs the six scenarios against it — twice each —
with the shipped transport defaults and the shipped cache defaults, because a
baseline measured with a configuration that does not ship is a baseline about
something else.

The one exception is the persistent tier, which ships off: there is no default
cache directory, and CONFIGURATION.md §3 says why. The reopened scenario creates
one for the length of that scenario and deletes it afterwards, so no row is
warmed by a directory another row left behind and no run of the harness is a
function of what a previous run wrote.

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

Measured with the shipped transport defaults and the shipped cache defaults -- 64 KiB blocks, a gap of one block, a 1 MiB bypass threshold, a 128 MiB budget (see [BLOCK_POLICY.md](BLOCK_POLICY.md)) -- and, for the reopened row, a persistent cache directory that run created and deleted, on Windows AMD64, MSVC 19.34.31937.0, Release.

| Scenario | requests | metadata | retries | redirects | bytes requested | bytes transferred | amplification | selectivity | wall ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| metadata-only open | 1 | 1 | 0 | 0 | 0 | 0 | — | 0.000000 | 1.4 |
| metadata-only open (cached) | 1 | 1 | 0 | 0 | 0 | 0 | — | 0.000000 | 0.4 |
| header and index read | 18 | 1 | 0 | 0 | 69632 | 69632 | 1.000000 | 0.000519 | 1.6 |
| header and index read (cached) | 3 | 1 | 0 | 0 | 69632 | 131072 | 1.882353 | 0.000977 | 0.7 |
| bounded spatial query | 19 | 1 | 0 | 0 | 331776 | 331776 | 1.000000 | 0.002472 | 1.5 |
| bounded spatial query (cached) | 18 | 1 | 0 | 0 | 331776 | 1507328 | 4.543210 | 0.011230 | 2.6 |
| bounded query, reopened | 19 | 1 | 0 | 0 | 331776 | 331776 | 1.000000 | 0.002472 | 1.5 |
| bounded query, reopened (cached) | 1 | 1 | 0 | 0 | 331776 | 0 | 0.000000 | 0.000000 | 4.4 |
| full sequential read | 33 | 1 | 0 | 0 | 134217728 | 134217728 | 1.000000 | 1.000000 | 152.6 |
| full sequential read (cached) | 33 | 1 | 0 | 0 | 134217728 | 134217728 | 1.000000 | 1.000000 | 137.0 |
| parallel readers | 152 | 8 | 0 | 0 | 2654208 | 2654208 | 1.000000 | 0.019775 | 3.6 |
| parallel readers (cached) | 26 | 8 | 0 | 0 | 2654208 | 1507328 | 0.567901 | 0.011230 | 3.7 |


The cached parallel row is the one row in this record that is not identical from
run to run: it lands at 25 or 26 requests depending on which reader wins each
claim, because a reader that arrives while a block is in flight waits where a
reader arriving a microsecond later finds it resident. `blockHits`,
`partialHits`, and `requestsSavedBySingleFlight` move with it, and for the same
reason. The harness therefore asserts that the request count is *below* the
uncached row rather than equal to a constant, and this record states which run
it came from rather than implying it is fixed.

Cache counters, for the rows that have them. Every one is zero on an uncached row, and those rows are omitted.

| Scenario | blockHits | blockMisses | partialHits | savedByCoalescing | savedBySingleFlight | bytesFromCache | bytesOverFetched | evictions | peakResidentBytes | persistedHits | persistedWrites |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| metadata-only open (cached) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| header and index read (cached) | 15 | 2 | 0 | 0 | 0 | 61440 | 122880 | 0 | 131072 | 0 | 0 |
| bounded spatial query (cached) | 1 | 23 | 0 | 6 | 0 | 16384 | 1191936 | 0 | 1507328 | 0 | 0 |
| bounded query, reopened (cached) | 18 | 0 | 0 | 0 | 0 | 331776 | 0 | 0 | 1507328 | 23 | 0 |
| full sequential read (cached) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| parallel readers (cached) | 126 | 23 | 2 | 5 | 159 | 2338816 | 1191936 | 0 | 1507328 | 0 | 0 |

Latency, in microseconds. Quantiles are bucket upper bounds, not exact order statistics (METRICS.md §4), and the request and read columns are p50 / p90 / p99 / max.

| Scenario | open | request | read |
| --- | ---: | ---: | ---: |
| metadata-only open | 1363 | 652 / 652 / 652 / 652 | — |
| metadata-only open (cached) | 315 | 311 / 311 / 311 / 311 | — |
| header and index read | 331 | 127 / 127 / 327 / 327 | 99 / 99 / 99 / 99 |
| header and index read (cached) | 290 | 255 / 286 / 286 / 286 | 0 / 170 / 170 / 170 |
| bounded spatial query | 279 | 63 / 127 / 274 / 274 | 63 / 77 / 77 / 77 |
| bounded spatial query (cached) | 262 | 127 / 127 / 258 / 258 | 127 / 179 / 179 / 179 |
| bounded query, reopened | 267 | 63 / 63 / 262 / 262 | 63 / 63 / 65 / 65 |
| bounded query, reopened (cached) | 529 | 521 / 521 / 521 / 521 | 255 / 351 / 351 / 351 |
| full sequential read | 393 | 2047 / 2047 / 13736 / 13736 | 2047 / 2047 / 13741 / 13741 |
| full sequential read (cached) | 480 | 1732 / 1732 / 1732 / 1732 | 1736 / 1736 / 1736 / 1736 |
| parallel readers | 630 | 127 / 255 / 622 / 622 | 127 / 226 / 226 / 226 |
| parallel readers (cached) | 551 | 127 / 511 / 544 / 544 | 255 / 255 / 255 / 261 |

The cache counters in METRICS.md §2.2 are populated: the runs below served 2748416 bytes from a block store and over-fetched 2506752 to do it. Both halves belong in the record. A design that reported only the first would be selling something, which is what METRICS.md §2.2 calls `bytesOverFetched` the honest counter for.

| Scenario | What it exercises | Notes |
| --- | --- | --- |
| metadata-only open | `openLatency`, `metadataRequestCount` — the cost of merely resolving | One `HEAD`. No content byte crosses the transport, and the reader is bound to a revision before any read is issued |
| metadata-only open (cached) | `openLatency`, `metadataRequestCount` — the cost of merely resolving | One `HEAD`, unchanged. Binding a store costs no request, which is why this row is the one row the cache does not move |
| header and index read | The clustered small-read pattern the block cache exists for | One 4 KiB header read and 16 adjacent 4 KiB index reads, each its own request |
| header and index read (cached) | The clustered small-read pattern the block cache exists for | The same seventeen reads, collapsed onto 2 block fetches. The bytes the alignment moved beyond the reads are `bytesOverFetched`, and the reads that never reached the transport are `blockHits` |
| bounded spatial query | `selectivity` — the headline claim | A header, a tail index, and 16 scattered 16 KiB chunks: 331776 bytes moved to answer a query against an asset of 134217728 |
| bounded spatial query (cached) | `selectivity` — the headline claim | The same query, with every read expanded to whole blocks: 1507328 bytes moved against an asset of 134217728. `selectivity` is worse than the uncached row on purpose -- that is what alignment costs, and `bytesOverFetched` is the counter for it |
| bounded query, reopened | What a second open costs (CACHE.md §8) | A second reader with no cache pays the query again, exactly: 331776 bytes and 19 requests, the same as the first |
| bounded query, reopened (cached) | What a second open costs (CACHE.md §8) | A reader with an empty block store over a cache directory an earlier reader filled: 0 bytes moved and 1 request, which is the metadata request. The row above is what the same query costs without it. `persistedWrites` is 0 here because the writing was done by the warm-up reader, whose numbers are the cached bounded-query row's. This holds for a `Stable` identity only -- a `Weak` or absent one neither writes to that directory nor reads from it |
| full sequential read | The worst case; must not be worse than a plain download | 32 reads of 4 MiB against one plain `GET` of the whole asset over the fixture server's own raw client: identical content bytes, 33 requests against 1, 152.6 ms against 123.1 ms. The comparator reads 4 KiB at a time, so the times are recorded and not gated |
| full sequential read (cached) | The worst case; must not be worse than a plain download | 32 reads of 4 MiB against one plain `GET` of the whole asset over the fixture server's own raw client: identical content bytes, 33 requests against 1, 137.0 ms against 130.1 ms. Every read bypassed the cache, so this row is the uncached row and is asserted to be |
| parallel readers | `requestsSavedBySingleFlight`, contention | 8 readers running the bounded query at once, each with its own revision binding. Every request is issued 8 times, because nothing is shared between readers |
| parallel readers (cached) | `requestsSavedBySingleFlight`, contention | 8 readers running the bounded query at once, each with its own revision binding and all of them sharing one store. What they no longer share is the traffic: 26 requests against 152. `requestsSavedBySingleFlight` is 159 and `blockHits` is 126: those count blocks a reader did not have to fetch, not requests, so they do not subtract to the difference above and are not meant to |

## What the numbers say

**The clustered read is the release, and it is a factor of six.** Seventeen
reads of a header and an index cost eighteen requests in `v0.2.0` and cost three
here — one `HEAD` and two block fetches. Fifteen of the seventeen reads never
reached the transport at all. The price is 122880 bytes of alignment against
69632 bytes asked for, stated in `bytesOverFetched` rather than left to be
inferred, and it is the whole of what the row cost.

**Eight readers of one asset now move what one reader moves.** The parallel row
went from 152 requests to 26, and — the number worth stopping on — from 2654208
bytes to 1507328, which is *exactly the transfer of the single cached bounded
query above it*. Eight readers of one revision moved one reader's worth of
bytes. That is what the cache key buys: the eight have eight independent
revision bindings and one identity, so seven of them found the blocks resident
or waited on the flight that was already in the air.
`requestsSavedBySingleFlight` is 159, and it counts blocks rather than requests,
so it is not the arithmetic difference of the two request counts and is not
meant to be.

**A second open now costs one request and no bytes.** This is what `v0.4.0`
adds, and it is the row `v0.3.0` said had to move. The bounded query is 19
requests and 331776 bytes for a second reader with no persistent cache — the
whole cost again, exactly — and 1 request and 0 bytes for a reader whose block
store is empty and whose cache directory an earlier reader filled. The one
request is the `HEAD`, and it is *supposed* to still happen: a persistent cache
that skipped it would be reusing an identity it had not revalidated. Twenty-three
blocks came off a disk instead of a socket, and `persistedHits` is the counter
that says so. This holds for a `Stable` identity and for no other: a `Weak` or
absent one never wrote to that directory and never reads from it, so its second
open is the row above, which is the point.

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
3" is "for 61440 bytes nobody asked for". The reopened row over-fetched nothing
at all, which is not a virtue of the persistent tier: the over-fetch was paid by
the warm-up reader whose numbers are two rows above, and a tier that reported
the saving without the row that paid for it would be selling the same thing.

**Nothing here is a network measurement.** Loopback has no round-trip time worth
the name, so the latency columns describe this process rather than a CDN, and
the trade this release makes — bytes for round trips — is one loopback cannot
price. What loopback does measure exactly is how many bytes and how many
requests a pattern costs, and that is the whole of what gate 6 is about. A
measurement over real distance arrives with the first consumer integration in
`v0.6.0`, against a fixture of at least a gigabyte; see
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

## What `v0.4.0` was measured against

The three rows the `v0.3.0` record named, and what happened to each:

| Row | At `v0.3.0` | At `v0.4.0` |
| --- | --- | --- |
| A second open of the same asset | The whole cost again: a new reader starts cold | 1 request and 0 bytes for a `Stable` identity, against 19 requests and 331776 bytes without the tier. `bounded query, reopened` is the pair |
| `metadata-only open` | 1 request | 1 request. Unchanged, and deliberately: the `HEAD` is how the identity the cache is keyed on gets revalidated |
| Full sequential read | 33 requests, `amplification` 1.000000 | 33 requests, `amplification` 1.000000, and still asserted *identical* to the uncached row rather than close to it |

The row that is not in this table is the one that would show a `Weak` or
`Unavailable` identity failing to reuse anything, because the fixture server
serves one asset with one strong `ETag` and the harness measures byte counts
rather than policy. That rule is tested where a test can vary the validator:
`usdAssetCache_persistence` opens the same identifier under a strong, a weak,
and an absent validator and asserts that only the first writes an entry and
only the first reads one back.

## What the next release has to move

`v0.6.0` is the first release with an external claim, and the scenario this file
still cannot record is the one that would price the trade every row above makes.
Loopback has no round-trip time, so "bytes for round trips" is a trade whose
numerator this harness measures exactly and whose denominator it cannot see. The
measurement over real distance arrives with the first consumer integration,
against a fixture of at least a gigabyte; see
[consumer integration](../roadmap/consumer-integration.md).
