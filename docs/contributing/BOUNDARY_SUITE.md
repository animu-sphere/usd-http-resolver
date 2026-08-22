# Boundary suite contract

The shared boundary suite is the deliverable of `v0.1.0`. It is not a test file
that happens to accompany the local backend: it is the executable form of the
read contract in [ASSET_READER.md](../architecture/ASSET_READER.md), and it is
what every later backend — HTTP, and after it S3, package-internal, and
Wasm — is admitted by.

This document fixes what the suite must contain and how a backend is entered
into it.

Status: implemented in `tests/boundary`, with four rows entered: the local
backend, the HTTP backend, the block cache over the local backend, and the same
cache with its persistent tier underneath. The last two are not further
transports — the cache is a decorator — and they are what makes "byte-for-byte
equivalence with the uncached path over the full suite" an assertion rather than
a claim. The persisted row runs the same cases from the same row definition,
which is what makes the pair a comparison rather than two experiments: if a
block that came off a disk differed from one that came off the wire, it would
show up as a byte mismatch against a file. The fixed cases in §3, the property cases in §4, the concurrency
cases, and the sanitizer builds in §5 all pass — the last of these under the
`sanitizers` job in `.github/workflows/core-ci.yml`, and first recorded locally
in [report 01](../reports/ost/01-2026-08-16-v0.1.0-ci-without-a-support-matrix.md).
See [CAPABILITY_MATRIX.md](../reference/CAPABILITY_MATRIX.md) and
[tests/README.md](../../tests/README.md).

## 1. Why the suite is the product

`v0.1.0` ships a local file reader, which is not interesting. What is
interesting is that the local file reader arrives with the harness that makes
every subsequent transport cheap to verify and impossible to fake.

The order is not negotiable. An HTTP backend written before the suite exists is
an HTTP backend whose bugs are indistinguishable from server behavior: a wrong
answer at EOF looks like a proxy, a short read looks like a network fault, and
every debugging session starts by arguing about which side is wrong. Written
after the suite, the same backend is either byte-equivalent to a local file or
it is not, and the question is settled in one run.

So the exit criterion for `v0.1.0` is not "the local backend works". It is:

> swapping the backend under test is a one-line change, and the suite is the
> only thing a new transport has to satisfy.

## 2. The oracle model

Every case is an equivalence against the local backend over the whole result,
never over the bytes alone:

```text
expected = LocalReader(asset).Read(offset, size)
actual   = BackendUnderTest(asset).Read(offset, size)

expected.bytes     == actual.bytes
expected.bytesRead == actual.bytesRead
expected.status    == actual.status
```

Comparing `status` is what catches the failure the byte comparison cannot see:
a backend that returns correct bytes with the wrong code has broken the EOF
distinction in §3 of the reader contract, and the layer above will act on the
code.

When the backend under test *is* the local backend, the oracle is an
independent, deliberately naive reader — open, seek, read, close, no shared
code with `usdAssetLocal` — so that the first row of the table is a real
assertion rather than a tautology.

## 3. Required boundary cases

Every backend runs all of these, unchanged:

| Case | What it pins |
| --- | --- |
| Zero-length read | `bytesRead == 0`, `Ok`, and no request issued |
| Offset zero | The trivial case, which is where off-by-one framing shows up |
| Read ending exactly at EOF | The boundary a range backend most often gets wrong by one |
| Read starting exactly at EOF | `bytesRead == 0`, `Ok` — an absence, not an error |
| Read starting past EOF | Same as above; still not an error |
| Read straddling EOF | Truncation to the remainder, with `Ok` |
| Whole-asset read | One read of `size` bytes; the degenerate streaming case |
| Block-boundary-like offsets | Offsets and lengths at powers of two and at their neighbours, before any cache exists, so the cache inherits passing cases |
| `offset + size` overflow | `InvalidArgument`, never an unchecked evaluation |
| Concurrent reads on one reader | No interleaving of one caller's bytes into another's buffer |
| Short read below EOF | `InvalidResponse` — never a hole, never a silent truncation |
| Cancellation | `Cancelled` promptly, where the backend admits cancellation |
| Mid-read revision change | `AssetChanged` on a read that reaches the transport; never the new revision's bytes on one that does not. For backends that can simulate it |

The last two rows are conditional on the backend, and the condition is declared
by the backend's entry in the suite rather than discovered by a skipped test. A
backend that cannot simulate a revision change says so; the local backend can
(rewrite the file underneath an open reader) and does.

The revision row is stated in two halves because a decorator forced the
distinction, and the distinction was always there. §2.1 of
[ASSET_READER.md](../architecture/ASSET_READER.md) says a reader that
*observes* a changed validator fails subsequent reads. A read that reaches the
transport observes; a read a block cache answers from bytes it captured under
the same binding observes nothing, and what it hands back is the revision the
reader is bound to — which is the guarantee rather than an exception to it. So
the case asserts `AssetChanged` at an offset the reader has not read before, and
for a range it has already read it asserts the thing that is true of every
backend: either `AssetChanged`, or byte-for-byte what the first read returned.
Never revision B.

That is a strengthening and not a relaxation. Before `v0.3.0` the case compared
a status and nothing else, and a backend that had quietly rebound and returned
the new revision's bytes *with* an `AssetChanged` code would have passed it. The
byte comparison is new, and every row passes it unchanged.

Asset sizes are chosen so that these cases are distinct: at minimum an empty
asset, a one-byte asset, an asset smaller than one block, an asset exactly one
block, and an asset spanning several blocks with a short final block.

## 4. Property-based cases

Fixed boundary cases prove the cases someone thought of. The property tests
cover the ones nobody did, over the same oracle:

```text
generate  assetSize, offset, readSize
assert    BackendUnderTest agrees with LocalReader on bytes, bytesRead, status
```

Generation is biased, not uniform — uniform random offsets over a large asset
almost never hit an interesting value. The generator weights:

- `0`
- `assetSize - 1`, `assetSize`, `assetSize + 1`
- block-aligned offsets, and those offsets ±1
- large offsets near the top of the range
- values chosen so that `offset + size` overflows
- random overlapping pairs
- concurrent overlapping ranges issued from several threads

A failing case is reported with its seed and shrunk to a minimal reproducer,
and the reproducer is then added to §3 as a fixed case. A property failure that
is fixed without gaining a fixed case can regress silently.

## 5. Sanitizers

Sanitizer builds are part of the contract, not an optional lane. The
concurrency properties in §7 of the
[design policy](../design/DESIGN_POLICY.md) are only actually verified under
ThreadSanitizer; asserting them in prose asserts nothing.

| Sanitizer | Required for | Catches |
| --- | --- | --- |
| AddressSanitizer | `libs/`, every release | Buffer handling at the boundary cases above |
| UndefinedBehaviorSanitizer | `libs/`, every release | Offset and size arithmetic, which is where overflow lives |
| ThreadSanitizer | `libs/`, every release | Concurrent reads, and later single-flight and eviction |

Because the core libraries build without OpenUSD, these builds need no USD
runtime and run on every platform in CI:

```sh
cmake --preset core-asan     # -DUSD_HTTP_RESOLVER_SANITIZER=address,undefined
cmake --build --preset core-asan
ctest --preset core-asan
```

That is the practical argument for the libs-first build graph: a range-read
test that needs a USD runtime is a test that gets skipped, and a sanitizer
build that needs one is a sanitizer build that never runs.

A sanitizer lane must also fail the run it finds something in, which is not the
default. UBSan prints the violation and continues, so the process exits `0` and
CTest reports a pass; `-fno-sanitize-recover=all`, set with the sanitizer flags
in the root `CMakeLists.txt`, is what makes the report a failure. A lane that
finds overflow in the offset arithmetic and stays green is worse than no lane,
because it is believed.

## 6. Entering a backend into the suite

Adding a transport adds a row, not a suite. The row declares:

| Declaration | Meaning |
| --- | --- |
| Factory | How to open a reader for a fixture asset |
| Fixture provisioning | How an asset of a given size and content comes to exist for this backend |
| Cancellation | Whether the backend admits cancellation |
| Revision simulation | Whether the backend can change an asset underneath an open reader |

Nothing else is negotiable. A backend that needs a boundary case relaxed has
either found a defect in the contract — in which case
[ASSET_READER.md](../architecture/ASSET_READER.md) changes first, for every
backend — or it is not admissible. A per-backend exception is how a contract
stops being one.

Fixture provisioning is asked for a *behavior*, not only a size. One case in §3
needs a transport that misbehaves — the short read below EOF — and expressing
that as a fixture characteristic is what keeps it an unconditional case rather
than a fifth declaration a backend could decline. "An asset that short-reads"
is something a backend must be able to make exist, in the same sense that "an
asset of 65536 bytes" is: for a hostile server it is a truncated body, and for a
local file it is an injected read fault. A backend that cannot produce the
behavior has not finished implementing its transport's failure handling, and it
cannot claim the case passes.

## 7. Relationship to other suites

```text
boundary suite          every backend, every release       this document
hostile-server corpus   the HTTP backend only              tests/fixture-server
amplification tests     assert a ratio, not correctness    METRICS.md
consumer integration    a separate lane, never a gate      consumer-integration.md
```

The hostile-server corpus in §11.2 of the design policy is additional to this
suite, not a substitute: it covers conditions only a server can produce — a
`200` in answer to a `Range`, a wrong `Content-Range`, a truncated body, a
mid-read validator change, a redirect chain, a reset. Those cases have no local
analogue, which is exactly why they cannot carry the correctness argument on
their own.

It is implemented, and it landed before the backend rather than beside it, per
action 1 of §17 of the design policy: see
[tests/fixture-server](../../tests/fixture-server/README.md). Two of the four
declarations in §6 above are already served by it for the HTTP row —
`FixtureBehavior::ShortReadBelowEof` is `Behavior::TruncatedBody`, and revision
simulation is `Server::Republish` — so what `usdAssetHttp` still has to supply
when it lands is the factory and the ordinary provisioning.
