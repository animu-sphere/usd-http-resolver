# Recorded block policy

This file holds the measurement that chose the cache's constants, and the
reasoning from it. §5 of the [design policy](../design/DESIGN_POLICY.md) says
cache behavior is measured before it is tuned, and §4 of
[CACHE.md](../architecture/CACHE.md) says the coalescing numbers are recorded
with the measurement that produced them, because *a tuned constant without a
recorded measurement is a guess with a decimal point*. This is that record.

Last recorded: 2026-08-20, against `main`, for `v0.3.0`.

It sits beside [BASELINE.md](BASELINE.md) and answers a different question. The
baseline records what the shipped configuration costs; this records why the
shipped configuration is that one and not another. A change to either constant
rewrites this file, and a release that changes I/O behavior rewrites both.

## What produced the numbers

`tests/cache-tuning` is the harness. It stands up the loopback fixture server,
serves one synthetic asset of 134217728 bytes with a strong `ETag`, and runs
four access patterns through `usdAssetCache` over `usdAssetHttp` at five block
sizes and four coalescing gaps — sixty-six runs, each against a fresh store, so
that no row is warmed by the row above it.

```sh
ctest --test-dir build/core -R usdAssetCache_block_policy
./build/core/tests/cache-tuning/usdAssetCache_tuning
```

Every byte of the fixture is a hash of its own offset and every read is
verified, so a configuration that returns the wrong bytes fails the run rather
than contributing a fast row. That verification is the part of the harness that
is a test: the cache runs over a real socket at five block sizes, and a block
boundary that is wrong at one of them fails the lane.

Every request count is asserted twice, once against the backend's counter and
once against the number of requests the fixture server logged answering — the
same independent witness [BASELINE.md](BASELINE.md) keeps, and for the same
reason: a request issued outside the metrics sink costs a round trip and counts
nothing, and a sweep watching only the sink would choose a block size from
numbers that were wrong in the same direction everywhere.

## What is chosen from what

**The defaults are chosen from the request counts and the byte counts, and from
nothing else.** Those are exact and identical on every machine. The wall-clock
column is a fact about loopback on the runner that drew the job, and choosing a
block size from it would be choosing it on a link with no round-trip time —
which is precisely the cost the block cache exists to trade bytes against.

That leaves one premise the harness cannot measure and this file therefore
states outright:

> On any link this project targets, one round trip costs more than one block of
> bytes.

That is §2 of CACHE.md as an assumption rather than a result. It is why a
configuration with fewer requests and more bytes is preferred to one with more
requests and fewer bytes, and it is the sentence that gets its own measurement
in `v0.5.0`, when the first consumer integration puts real distance between the
reader and the origin. Until then the honest statement is that the *direction*
of the trade is assumed and its *magnitude* is measured.

## The chosen defaults

| Constant | Value | Chosen from |
| --- | ---: | --- |
| `blockSize` | 65536 | Measured. Six times fewer requests on the clustered pattern, at a bounded cost of 64 KiB per miss |
| `coalesceGapBlocks` | 1 | Measured. The whole of the observed benefit at small block sizes, and nothing beyond it |
| `maxRequestBytes` | 8388608 | Not measured — a safety bound. No merged run in the sweep comes near it |
| `budgetBytes` | 134217728 | Not measured — a residency policy. See below |
| `bypassThresholdBytes` | 1048576 | Measured, as a non-regression: it is what keeps the full sequential read at the uncached request count |

The last two are labelled as what they are. A number in a table under a heading
about measurement, which was not measured, is the failure this file exists to
prevent.

## The sweep

Fixture: 134217728 bytes at `/tuning/asset.bin` on the loopback fixture server,
ephemeral port, `Behavior::Normal`, strong `ETag`. Layout: a 4096-byte header at
offset 0, a 65536-byte index in the tail, body between them — the same layout
the recorded baseline uses, so a row here can be read against a row there.

Windows AMD64, MSVC 19.34.31937.0, Release. `requests` includes the one metadata
request every open costs.

### Header and index read

One 4 KiB header read, then sixteen adjacent 4 KiB index reads. This is the
clustered small-read pattern §2 of CACHE.md is written about.

| block | gap | requests | bytes moved | amplification | over-fetch |
| ---: | ---: | ---: | ---: | ---: | ---: |
| none | — | 18 | 69632 | 1.000000 | 0 |
| 4 KiB | 0–4 | 18 | 69632 | 1.000000 | 0 |
| 16 KiB | 0–4 | 6 | 81920 | 1.176471 | 61440 |
| **64 KiB** | **0–4** | **3** | **131072** | **1.882353** | **122880** |
| 256 KiB | 0–4 | 3 | 524288 | 7.529412 | 516096 |
| 1 MiB | 0–4 | 3 | 2097152 | 30.117647 | 2088960 |

### Bounded spatial query

A header, a tail index, and sixteen scattered 16 KiB chunks. The pattern
`selectivity` is claimed on, and the one that punishes a large block.

| block | gap | requests | bytes moved | amplification | over-fetch |
| ---: | ---: | ---: | ---: | ---: | ---: |
| none | — | 19 | 331776 | 1.000000 | 0 |
| 4 KiB | 0–4 | 19 | 393216 | 1.185185 | 61440 |
| 16 KiB | 0–4 | 19 | 589824 | 1.777778 | 270336 |
| **64 KiB** | **0–4** | **18** | **1507328** | **4.543210** | **1191936** |
| 256 KiB | 0–4 | 18 | 5242880 | 15.802469 | 4927488 |
| 1 MiB | 0–4 | 18 | 20971520 | 63.209877 | 20656128 |

### Interleaved index re-read

Every other 4 KiB piece of the index, and then the whole index region in one
read. The only pattern in the sweep in which the coalescing gap can bind at all,
and it is here for that reason alone; see below.

It is also the only pattern whose over-fetch comes from the *gap* rather than
from alignment: the merged request re-transfers blocks that were already
resident, and the caller reads those from the store while the wire moves them
again. That is a real cost and it is charged here.

| block | gap | requests | bytes moved | over-fetch |
| ---: | ---: | ---: | ---: | ---: |
| none | — | 10 | 98304 | 0 |
| 4 KiB | 0 | 17 | 65536 | 0 |
| **4 KiB** | **1–4** | **10** | **94208** | **28672** |
| 16 KiB | 0–4 | 5 | 65536 | 49152 |
| 64 KiB | 0–4 | 2 | 65536 | 61440 |
| 256 KiB | 0–4 | 2 | 262144 | 258048 |
| 1 MiB | 0–4 | 2 | 1048576 | 1044480 |

### Full sequential read

Thirty-two reads of 4 MiB over the whole asset. The worst case, and the row that
has to not move.

| block | gap | requests | bytes moved | amplification | over-fetch |
| ---: | ---: | ---: | ---: | ---: | ---: |
| none | — | 33 | 134217728 | 1.000000 | 0 |
| 4 KiB … 1 MiB | 1 | 33 | 134217728 | 1.000000 | 0 |

Identical at every block size, because every read in it is larger than
`bypassThresholdBytes` and never reaches the store. That is the policy working,
and it is checked here rather than argued: a cache that turned the worst case
into a worse case would show up in this table as a request count or a byte count
that moved.

## What the numbers say

**Block size buys request count and is paid for in bytes, and the exchange rate
gets worse fast.** From 4 KiB to 64 KiB, the clustered pattern goes from 18
requests to 3 — six times fewer — for 61440 extra bytes. From 64 KiB to 1 MiB it
buys nothing at all on that pattern, 3 requests either way, and costs a further
1966080 bytes. The knee is at 64 KiB and it is not close.

**The scattered pattern is where a large block is punished, and it agrees.** The
bounded query's request count barely moves across the whole sweep — 19 down to
18 — because sixteen chunks 8 MiB apart are sixteen requests whatever the block
size. All a larger block does there is multiply the bytes: at 64 KiB the query
moves 1507328 bytes, and at 1 MiB it moves 20971520 to answer the same question.
Both tables point at the same value from opposite directions, which is the
strongest thing this sweep produces.

**`selectivity` gets worse, on purpose, and stays the headline.** The bounded
query moved 0.0025 of the asset in `v0.2.0` and moves 0.0112 at the chosen block
size. That is the trade §1 of CACHE.md describes — alignment converts request
count into transferred bytes — and it is still one percent of a 128 MiB asset to
answer a query against it.

**The coalescing gap is real, and it does not bind at the chosen block size.**
This is the finding worth stating plainly rather than tidying away. On the
interleaved re-read at 4 KiB blocks, a gap of one block takes 17 requests down to
10 for 28672 extra bytes, which is exactly the trade §4 of CACHE.md predicts.
At 16 KiB and above the whole index region fits in one or two blocks, there is
no gap left to merge across, and gaps of 0, 1, 2 and 4 produce identical rows
everywhere in the sweep. So `coalesceGapBlocks` is 1 because that is the value
that captures the entire measured benefit where the benefit exists, and because
2 and 4 were measured and bought nothing anywhere. At the shipped block size the
constant currently does no work, and a reader of this table should know that
rather than infer that it does.

**Two of the five constants were not measured and are labelled.**
`maxRequestBytes` is a bound on the pathological case: no run in this sweep
produces a merged request within two orders of magnitude of it, so there was
nothing to measure, and its job is to stop one enormous request defeating
cancellation rather than to make a pattern faster. `budgetBytes` is a residency
policy rather than an I/O constant — 128 MiB is a ceiling a DCC process can
afford and roughly two thousand blocks at the chosen size — and measuring it
would take a working set that outlives one query, which is `v0.5.0`'s consumer
fixture and not this harness.

## What the next release has to move

`v0.4.0` adds persistence, which changes what a hit is worth and nothing about
what a block is. The constants here are expected to survive it.

`v0.5.0` is what tests the premise. The first consumer integration puts real
distance between the reader and the origin, and it is the first run in which a
round trip costs what this file assumes it costs. If 64 KiB is the wrong number,
that is where it will show, and this file is what the new measurement replaces.
