# Changelog

Unreleased work on `main`. Tagged versions get an immutable record under
[docs/releases/](docs/releases/README.md); this file is what has landed since
the last one.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
this project follows [semantic versioning](https://semver.org/) with the
diagnostic codes in
[DIAGNOSTICS.md](docs/architecture/DIAGNOSTICS.md) treated as a compatibility
surface: adding a code is a minor change, changing what one means is a breaking
one.

## `v0.5.0` - 2026-08-27

The resolver becomes an independently composable geospatial-runtime component.
The transport, cache, and public C++ contracts are unchanged from v0.4.0.

### Added

- A workspace aggregate product containing the exact resolver bundle and a
  component-owned packaged probe. The probe opens a remote USD stage with a
  relative layer through a loopback HTTP origin and proves byte-range reads.
- Product data mapping under `share/usd-http-resolver/probes`, so acceptance
  executes from an installed artifact without producer source paths.

### Changed

- Generated source CI and canonical OpenUSD 26.08 pins are refreshed for the
  OpenStrata v0.22.8 release line.
- Packaging records the resolver product as a runtime-composition provider;
  the v0.22.8 OpenStrata aggregate-product correction is required to consume
  the outer archive directly during composition.

## `v0.4.0` - 2026-08-22

Identity leaves the process, and so do the bytes. The release is two halves of
one rule — a `Strong` validator, and nothing weaker, may be reused after the
reader that captured it is gone. The first half decides what a consumer may
learn about an asset's identity and, the part that took the argument, what it
may not be told. The second writes blocks to a disk a later process reads back.

### Added

- **An on-disk block cache**, `usdasset::cache::DiskBlockStore`, which is
  [CACHE.md](docs/architecture/CACHE.md) §8. Blocks fetched for a `Stable`
  identity are written into a directory the host names, and a process that
  starts cold reads them instead of the network: a bounded query that costs 19
  requests and 331776 bytes on a first open costs 1 request and 0 bytes on a
  second, and the one request is the metadata `HEAD`, which is *supposed* to
  still happen. `bounded query, reopened` in
  [BASELINE.md](docs/reference/BASELINE.md) is that pair.

  A `Weak` or absent identity does not merely skip the write. The tier is never
  read for one, so a restart cannot turn a guess into a durable answer, and the
  write is refused inside the store rather than outside it, so that `rejected`
  counts what the rule turned away.

  Nor is `Strong` on its own enough. Strength is a claim and a claim has an
  author: an entity tag is the origin's and means the same thing to whoever
  reads the entry next, while a `Derived` identity — `usdAssetLocal`'s device,
  file index, size, and mtime — is strong for as long as that backend holds the
  file open, which is what it says about itself. Only an identity the origin
  issued crosses a process boundary.

- **The properties CACHE.md §8 asked for a release in advance**, each with a
  case: entries keyed by the whole key rather than by the URL; the identity
  written *inside* the entry and compared byte for byte, so a hash collision
  costs a miss and can never serve one asset's bytes for another's; publication
  by rename, so an interrupted process leaves a temporary nobody looks up rather
  than half an entry; a filename alphabet of sixteen hexadecimal characters, so
  a URL never becomes a path however it is spelled; separate header and body
  checksums, with a structurally broken entry discarded on the way out and an
  intact one belonging to another identity left alone; and a cache directory
  that can be deleted at any moment, under a live reader, at a cost of time.

- **Nothing under the cache directory is reversible to a URL.** An entry's
  identity is written as a SHA-256 digest and never as itself, because a
  resolved identifier can be a signed URL and gate 7 of
  [docs/releases/README.md](docs/releases/README.md) forbids a credential or a
  signed-URL query string in any persisted artifact. A cache entry is the first
  artifact this project persists, and it is the one place `ElideSecrets` cannot
  run: an entry has no message to elide, it has a key. So the key is not written
  down, and the digest that replaces it still catches a name collision — which
  is all the identity was in the entry for.

- **A bound the contract did not ask for and an implementation cannot avoid.**
  The directory has a budget, 1 GiB by default, swept on an interval rather than
  enforced per write, evicting oldest-written first. Not least-recently-used:
  refreshing a timestamp on every hit would turn a read of a cached block into a
  write. Eviction there is invisible to correctness for the reason it is in
  memory, so an approximate order costs a re-fetch and nothing else. The budget
  a sweep enforces is the host's, unless the host's number cannot hold a few of
  the largest entry the store has written: a byte floor cannot relate itself to
  a block size nobody told it, and a budget that evicts the entry whose write
  triggered the sweep is two I/Os spent to achieve nothing. The walk runs
  outside the store's lock, for the same reason a read of one entry does.

- **Two environment variables**, `USD_HTTP_RESOLVER_PERSISTENT_CACHE_DIR` and
  `USD_HTTP_RESOLVER_PERSISTENT_CACHE_BUDGET`. Naming a directory is the only
  switch, and there is no default location: a resolver that wrote to a disk
  nobody named would be a surprise, and where a cache belongs on a given machine
  is a deployment's decision about a disk this project cannot see
  ([CONFIGURATION.md](docs/reference/CONFIGURATION.md) §3).

- **`persistedHits` and `persistedWrites`**, in
  [METRICS.md](docs/architecture/METRICS.md) §2.2. Block counts and not byte
  counts: the bytes a persisted hit saved are already in `bytesFromCache`, which
  is where `cacheHitRatio` has to find them, and a second byte counter for the
  same bytes would be double counted by anything that summed the section.

- **`boundary_persisted_local`**, a fourth row in the shared boundary suite: the
  same row definition as `cached-local` with the tier underneath it, so the two
  are a comparison rather than two experiments. The one further difference is
  the validator's *kind*, relabelled so that the tier writes at all; the value
  is the local backend's own, so a republished fixture is still a different
  identity and every revision case still fails where it should. Every case
  unchanged, green under AddressSanitizer, UndefinedBehaviorSanitizer, and
  ThreadSanitizer.

- **`usdAssetCache_persistence`**, which asks what the suite cannot, because the
  suite asks only for bytes: whether a second process pays for what the first
  one fetched, whether a weak validator ever reaches the disk, whether an entry
  from revision A can serve a read of revision B, what a scribbled file costs,
  and what the directory a hostile URL was hashed into actually contains.

- **A sixth baseline scenario and a cross-process case.**
  `bounded query, reopened` in `tests/baseline` records what a second open
  costs; `httpResolver_stage` re-invokes its own executable with the cache
  directory in its environment and counts the fixture server's log, so "a second
  process pays nothing" is measured against a second process rather than
  simulated inside one.

- **Asset info and identity stability**, through `ArResolver::GetAssetInfo` and
  nothing else. `ArAssetInfo::resolverInfo` carries the four neutral values of
  [RESOLVER.md](docs/architecture/RESOLVER.md) §3 — `resolvedIdentifier`,
  `size`, `validationToken`, and `stability` — under the names the first
  consumer's own contract uses. The token is the backend's captured validator,
  opaque here and opaque all the way out: nothing above the backend parses it,
  compares it to an `ETag`, or infers a time from it.
- **The rule that decides what `version` may carry.** `ArAssetInfo::version`
  gets the token only for a `Stable` identity, because that field travels with
  nothing beside it — no stability, no qualification — and a consumer that finds
  a token there treats it as fit to key durable cache reuse on. A weak or absent
  validator leaves it empty and explains itself in `resolverInfo`, where
  `stability` is beside the token. The dictionary publishes a weak token anyway,
  because a weak validator that *changed* is still evidence the asset changed;
  it is proof of sameness that weak cannot supply.
- **A contradicted identifier stops publishing a reusable identity.**
  `ArAssetInfo` is keyed by asset path and this resolver hands out one reader
  per open, so two `ArAsset`s over one URL may be two revisions and asset info
  cannot say which one a caller holds. Harmless until the asset moves: once two
  opens of one identifier have captured two different validators, `Stable`
  degrades to `Unstable` and `version` goes empty for the rest of the process.
  Two records exist per identifier and only one of them is bounded: the metadata
  that answers asset info without a request may be dropped, because dropping it
  costs a request, and the validator a later open is compared against may not,
  because dropping that makes an asset which has already moved look like one
  being opened for the first time.
- **Identity answered from the open, not from a new request.** Asset info comes
  from the open this process already performed for that identifier. That is
  cheaper and, more to the point, *more correct*: the identity a consumer needs
  is the identity of the bytes it is holding, and a `HEAD` issued now describes
  whatever is published now. An identifier with a resolved path that nothing has
  opened is opened and retained, so the request costs what the `OpenAsset` after
  it would have cost.
- **Asset info never rediscovers a failure.** It does not open an identifier
  whose resolved path is empty — an empty resolved path is a resolution that
  already failed, and rediscovering that costs the round trip `Resolve` has just
  spent — and it posts no diagnostic of its own, because the operation that
  follows reports the same fault with the same code.
- **`httpResolver_identity`**, offline and linking one translation unit: the
  publication rules as a table — strong, weak, `Last-Modified`, absent,
  contradicted, a strength with no kind, and both credential shapes. The defect
  it exists to catch is silent, and it does not fail in this repository: a token
  published for a validator that cannot prove a revision fails in a consumer,
  weeks later, as a generated cache that served the wrong bytes.
- **Ten cases in `httpResolver_stage`**, over a real socket: the four fields for
  a strong validator, the `version` rule for each stability class, a republish
  that withdraws reusability, a signed URL whose credential reaches no published
  field, an invalid timestamp, an identity that costs no second metadata
  request, a republish detected after that identifier's metadata has aged out of
  the bounded table, eight threads resolving and opening one asset while asking
  for its identity, a failing origin that is neither reopened nor reported
  twice, and one case that asserts nothing in a `CHECK` at all — it resolves an
  asset it never opens and lets the exit code do the asserting.

  Those cases count requests per asset out of the fixture server's log rather
  than from its total, which is what makes them stable: a case that abandons a
  response mid-body leaves the server writing to a socket nobody is reading, and
  that request can be logged inside a later case's window.

### Changed

- **`GetModificationTimestamp` is now overridden to return an invalid
  timestamp**, which is what the default did, so that the decision is recorded
  where somebody would otherwise make the tempting mistake. A validator is not a
  time; reading `Last-Modified` as one parses an HTTP construct above the
  backend that captured it; and a synthesized number would be picked up by a
  consumer's fallback and turned into precisely the durable identity a weak or
  absent validator may not have. An invalid timestamp costs a `SdfLayer::Reload`
  a request, and never costs a wrong answer.

### Fixed

- **A crash at process exit when a resolved asset was never opened.** The
  resolver retains the reader `Resolve` opened, for the `OpenAsset` that usually
  follows (RESOLVER.md §2.3) — and nothing requires one to follow. Such a reader
  is destroyed with the resolver, during static destruction, and folded its
  counters into a process metrics aggregate that had been destroyed already: the
  aggregate is constructed at the *first* reader and so is destroyed before the
  resolver that was constructed before it. The aggregate is now never destroyed,
  and the opt-in `USD_HTTP_RESOLVER_METRICS_DUMP` runs from `std::atexit`
  instead. Reachable since `v0.2.0` by any host that probes for existence and
  exits; it took a test that left a retained open behind to surface it, and it
  presented as a segmentation fault *after* the suite printed `ok`.
- **The bundle README's configuration table**, which still listed five variables
  after `v0.3.0` added four cache values, and still said the bundle performed no
  caching.

The immutable release record is
[docs/releases/v0.4.0.md](docs/releases/v0.4.0.md).

## `v0.3.0` - 2026-08-21

The block-cache release. Its definition
of success was the table in [BASELINE.md](docs/reference/BASELINE.md) *What the
next release has to move*, and all four rows moved the way that table asked:
the clustered header-and-index read went from 18 requests to 3, the bounded
query's `amplification` went above 1.0 with `bytesOverFetched` to account for
it, eight parallel readers of one asset went from 152 requests to 25, and the
full sequential read did not move at all.

### Added

- **`libs/usd-asset-cache`**, the block cache, as a decorator over
  `AssetReader`. It links `usdAssetIo` and nothing else: no transport, no
  backend, no OpenUSD. Reads are expanded to whole blocks and served from them;
  the final block of an asset is stored at its true length and never padded;
  adjacent and near-adjacent fetches are merged into one request; concurrent
  readers that miss the same block issue one request and the rest wait; blocks
  are evicted LRU under a process-wide budget shared across assets; and a read
  large enough to be a streaming pass bypasses the whole thing.
  [CACHE.md](docs/architecture/CACHE.md) is the contract and the module
  [README](libs/usd-asset-cache/README.md) states what it refuses to own.
- **A validator-keyed `CacheKey` from the first commit.** The key is
  `resolvedIdentifier + validator + blockSize + blockIndex`, and the rule it
  exists for is that equal identifiers never imply equal content: two revisions
  published at one URL are two cache identities. The validator is an opaque byte
  string here — never parsed, never compared to an `ETag`, never read for
  recency — and exactly one other field of it is read, `strength`, exactly once.
- **Single-flight, across readers and not only across threads.** Eight threads
  missing one block issue one request; so do eight independent readers of one
  revision, because they share an identity and therefore share the store. That
  second case is the one that moved the parallel-readers baseline, and it is why
  the store is process-wide rather than per reader.
- **The cache counters of [METRICS.md](docs/architecture/METRICS.md) §2.2**,
  populated: hits, misses, partial hits, requests saved by coalescing and by
  single-flight, `bytesOverFetched`, evictions, and a resident high-water mark.
- **A third row in the shared boundary suite**, `cache over local`. The cache is
  not a transport, so it is not a fourth backend — it is the same local backend
  with a decorator on top, and entering it there is what makes "byte-for-byte
  equivalence with the uncached path over the full suite" an assertion rather
  than a claim. Every case runs unchanged.
- **`tests/cache-tuning`**, the measurement that chose the constants: a sweep of
  five block sizes against four coalescing gaps over four access patterns,
  against a real socket, with every byte verified. Recorded in
  [BLOCK_POLICY.md](docs/reference/BLOCK_POLICY.md), which also labels the two
  constants that were *not* measured as the bounds they are.
- **The four cache variables of
  [CONFIGURATION.md](docs/reference/CONFIGURATION.md) §2**, read once when the
  resolver is constructed. A block size that is not a power of two is rounded
  down and the rounding is reported; a value outside the bounds is refused
  rather than clamped.
- **`ReaderMetrics::AbsorbTransport` and `DetachFromRegistry`**, so that a
  decorated stack reports one counter set instead of two.
- **Sanitizer coverage over all of it**, which needs no new lane: the module
  tests, the boundary row, and the tuning sweep are `libs/` and `tests/`, which
  is what `core-asan` and `core-tsan` already cover. Both are green over the
  whole core tree, 25 of 25 each, under GCC 15.2.

### Changed

- **The resolver decorates every asset it opens.** `plugins/http-resolver` links
  `usdasset::cache`, which [WORKSPACE.md](docs/architecture/WORKSPACE.md) §2 has
  admitted since the workspace contract was written and this release is the
  first to take. `httpResolver_stage` now asserts from the fixture server's log
  that a 4 KiB window out of a megabyte costs a block and not the megabyte —
  a bound rather than an exact range, because the exact-bytes property is what
  CACHE.md §3 trades away on purpose.
- **The recorded I/O baseline holds every scenario twice**, with the cache and
  without it, in one run of one harness. METRICS.md §6 asks a release that
  changes I/O behavior for the counter values before *and* after, and this is
  the first release that changes them on purpose. `tests/baseline` gained the
  cache on its link line and a set of assertions for the cached rows: the
  server's log is the independent witness for the byte count, the request count
  is asserted to be below the uncached row, and the full sequential read is
  asserted to be *identical* rather than merely close.
- **The boundary suite's mid-read revision case now asserts bytes as well as a
  status**, and asserts them in the right place. It reports `AssetChanged` for a
  read at an offset the reader has not read before; for a range it has already
  read, it accepts either `AssetChanged` or byte-for-byte what the first read
  returned, and rejects the new revision's bytes. This is a strengthening rather
  than a relaxation — the byte comparison is new, and the old case would have
  passed a backend that rebound *and* reported `AssetChanged`. It is also what a
  reader with a cache under it can satisfy honestly: §2.1 of
  [ASSET_READER.md](docs/architecture/ASSET_READER.md) says a reader that
  *observes* a changed validator fails subsequent reads, and a hit observes
  nothing and returns the revision the reader is bound to.
- **`selectivity` on the bounded query went from 0.0025 to 0.0112**, on purpose.
  Alignment converts request count into transferred bytes, and one percent of a
  128 MiB asset to answer a query against it is still the sentence the
  architecture is made of. The cost is in `bytesOverFetched`, which is reported
  beside the saving rather than instead of it.
- The metrics dump prints the cache block and the two cache ratios, including
  the zeroes. A dump that hid them would make "no cache ran" and "the cache
  never hit" the same output.

### Fixed

Nothing. No defect in `v0.2.0` was found by this work.

The immutable release record is
[docs/releases/v0.3.0.md](docs/releases/v0.3.0.md).

## `v0.2.0` — 2026-08-20

The release in which bytes cross a network. Its scope is in the
[roadmap](docs/roadmap/README.md) and the immutable record is
[docs/releases/v0.2.0.md](docs/releases/v0.2.0.md).

It started with the HTTP client dependency decision rather than with code, and
then with the server the code would be tested against rather than the code. Both
prerequisites in §17 of the [design policy](docs/design/DESIGN_POLICY.md) were
done first, in the order it fixed, and neither ships in the release. The backend
they existed for landed against two suites that already passed, and so did the
bundle that makes it reachable: a `UsdStage` now opens over HTTP. With the I/O
baseline recorded, this release also makes its first performance claim — and it
is a counter on a named fixture rather than a sentence.

### Added

- **The recorded I/O baseline**, which is the last outstanding item of `v0.2.0`
  and the thing gate 6 of [the release gate](docs/releases/README.md) binds a
  release to. `tests/baseline` stands up the fixture server, serves one 128 MiB
  synthetic asset, and runs the five scenarios METRICS.md §6 names with the
  shipped transport defaults. The record is
  [BASELINE.md](docs/reference/BASELINE.md), and its headline is `selectivity`
  on a bounded query: **a header, an index, and sixteen scattered chunks moved
  324 KiB of a 128 MiB asset — 0.0025 of it — and every byte moved was a byte
  the caller asked for.** `amplification` is exactly 1.000000 in every scenario
  that moves a byte, because there is no cache to over-fetch; that number can
  now only get worse, which is the point of having written it down before
  `v0.3.0` trades it for a smaller request count.
- The division the harness keeps, which is a decision and not a detail: **byte
  and request counts are asserted exactly, and every ratio and every duration is
  recorded.** A ratio is a byte count divided by a fixture size, so a gate on one
  would move when the fixture did; a duration on loopback is a fact about the
  process that measured it. What that leaves gated is precisely what gate 6
  exists for — a cache that over-fetches, a retry nobody asked for, or a redirect
  that starts being followed — and none of it is visible to a functional test,
  because the boundary suite would still find every one of those bytes correct.
- The fixture that was missing rather than the instrumentation, which existed
  since `v0.1.0`: 128 MiB, synthesized rather than committed, every byte a hash
  of its own offset. That last part is what lets every scenario verify the bytes
  it counted — a read that landed a block away cannot compare equal — because a
  measurement over unchecked bytes is a measurement of the wrong thing arriving
  quickly. It is registered as a test rather than kept as a tool a release run
  remembers to invoke, so it runs on all three platforms and, at 8 MiB, under
  both sanitizers.
- Every request count asserted twice: against the backend's counter, and against
  the number of requests the fixture server logged answering. The second is what
  makes the first worth anything, because gate 6 is the one gate a self-report
  can be wrong about — a request issued outside the metrics sink costs a round
  trip and counts nothing, and a lane watching only the sink stays green while
  the wire traffic doubles. The metadata-only scenario reads the method off the
  wire too: "no content byte crosses the transport" is a property of the `HEAD`
  that went out, not of the counter that classified it.
- A plain download to compare the worst case against, performed by something
  that is not the client under test: the fixture server's own raw client, one
  `GET`, no `Range`, and no HTTP code shared with `usdAssetHttp` — the same
  separation the boundary suite keeps between its oracle and `usdAssetLocal`. A
  ranged full read of the whole asset moves the same content bytes as that
  download in a comparable time, which is what METRICS.md §6's fourth row asks
  and the one row that could have embarrassed the architecture. Its wall clock
  is recorded and not gated: the comparator reads 4 KiB at a time, which flatters
  the backend, and a comparison that flatters is not a gate.

- **A release lane, and the mechanical half of the gate inside it.**
  `.github/workflows/release.yml` runs on a `vX.Y.Z` tag: it validates that
  `VERSION`, `openstrata.toml`, the plugin manifest, the bundle's standalone
  CMake fallback, the newest changelog section, and the tag all agree
  (`tools/check_release_metadata.py`); re-runs gates 3, 4, and 5 by *calling*
  `core-ci.yml` and `plugin-windows-ci.yml` as reusable workflows rather than
  copying their steps; greps the whole verbose suite output for credentials with
  the metrics dump on; and assembles a draft release from the source tree, its
  checksums, and the release record as its notes. Six places is more than a
  reader reliably checks, and the failure is silent — a tree whose manifest says
  `0.2.0` and whose `VERSION` says `0.1.0` builds, tests, and tags green, and is
  discovered by whoever installs it.
- **It publishes source and no binary package, deliberately.** Gate 9 binds a
  release that publishes one, and the roadmap puts packaged cross-platform
  artifacts in `v0.6.0`; attaching plugin binaries here would make gate 9 bind
  on a build that, measured, is not yet reproducible. The file says where the
  binary matrix goes when it arrives and what has to be true first.
- The tag lane refuses a record that still carries a placeholder, which is the
  one thing only the tag can check: the gate requires the record to be prepared
  in the release commit *before* the tag, and a finished record is the evidence
  that happened.

- `plugins/http-resolver`, the `ArResolver` bundle, and the first module in this
  repository that includes an OpenUSD header. It registers `http` and `https` as
  URI schemes — not as the primary resolver, so installing it never changes how
  a local asset opens — and returns an `ArAsset` over the backend's reader. It
  is thin by construction: no byte handling, no request assembly, and 45 lines
  of it are the part that talks to OpenUSD's diagnostic system.
- **A remote stage, asserted rather than described.** `httpResolver_test_stage`
  stands up the hostile fixture corpus on loopback, opens a `UsdStage` over it,
  follows a relative reference to a second remote layer, reads a 4 KiB window
  out of a 1 MiB asset and checks the `Range` header the server actually
  received, and confirms that a `404` is silent, that a transport failure is
  not, that range-unsupported is terminal, and that a local stage still opens
  exactly as it did. It is the only test here that links OpenUSD and the fixture
  server at once, and it reaches the backend only through `ArResolver`.
- Identifier normalization that makes one asset one identifier, tested as a
  table and as a property: normalizing an identifier again must not change it.
  Two rules in it are decisions rather than mechanics. **Percent-decoding runs
  before dot segments are removed**, because `%2E%2E` is an encoded `..` and
  leaving it for the origin to resolve means the resolver's idea of which asset
  was named and the server's differ. **The userinfo is dropped**, because §4.3
  of the design policy keeps credentials out of the resolver API and an
  identifier *is* that API — so `https://user:token@host/a` fails at the origin
  with `HTTP002` rather than succeeding with a secret in every log line.
- Anchoring per RFC 3986 §5.2, which is what makes a remote scene work at all: a
  layer published to a CDN references its neighbours relatively and none of
  those references mentions a host. A relative path anchored to a *local* layer
  returns empty and is left to the primary resolver.
- One metadata request per resolution, reused by the open that follows. The
  reader `Resolve` opened is retained and handed to the next `OpenAsset` **once**
  — a second open for the same identifier opens again rather than sharing a
  reader already bound to a revision somebody else is mid-composition on. Two
  concurrent resolutions of one identifier make one request, no lock is held
  across a round trip, and the table of retained opens is bounded, because a
  resolve that is never followed by an open is normal and an unbounded table
  would hold one reader per asset the process ever asked about.
- The `HTTPxxx` projection, which the diagnostics contract allocated in `v0.1.0`
  and nothing emitted until now. Every code in the table except `HTTP102`, which
  ADR-0002 defers and which is deliberately absent from the code as well as
  unreachable. Errors arrive as `TF_RUNTIME_ERROR`, cancellation as `TF_WARN`,
  and an impossible request as `TF_CODING_ERROR`; `HTTP101` reports a retry that
  succeeded, which costs latency and is otherwise invisible in a return value.
- The five transport bounds in CONFIGURATION.md, read from the environment. A
  value that does not parse warns and takes the default rather than failing the
  process, and one bad value does not discard the other four. `0` is legal for
  the two counters and refused for the three deadlines, because to most
  transports a zero deadline means *no* deadline.
- Three of the bundle's four tests link one translation unit and nothing else —
  no OpenUSD, no sockets. Normalization, configuration, and the diagnostic
  projection are arithmetic, and a mistake in the first is invisible from the
  outside: two spellings of one asset become two opens and two revisions. A test
  that would catch that must not need a USD runtime to run.

- `libs/usd-asset-http`, the HTTP backend, and the first thing in this
  repository that touches a network. It serves byte ranges over `http` and
  `https`, and it is admitted by the `v0.1.0` boundary suite **unchanged** —
  243 fixed cases, 10,000 generated cases, and the concurrency cases, all
  byte-equivalent to the local backend over an independent oracle. Entering it
  was one row (`tests/boundary/backends/boundary_http_main.cpp`) and one line of
  CMake, which was the claim `v0.1.0` made and had not yet been asked to cash.
- **Revision binding, from the backend's first commit rather than after it.** A
  validator is captured at open and classified; a conditional guard rides every
  subsequent range request where the captured validator admits one; and a
  response that contradicts the capture fails the read with `AssetChanged` and
  `bytesRead == 0`. There are two detectors because neither covers the other:
  `If-Range` makes the server refuse, which is cheaper and more atomic, and the
  response comparison catches a revision whose bytes are identical and whose
  identity moved — the case a backend comparing bytes cannot see, and the whole
  guard for an asset whose validator may not be sent conditionally. A weak
  `ETag` is captured and never sent, per RFC 9110 §13.1.5.
- Response framing validated **against the request**, not against itself. A
  `206` whose `Content-Range` is internally consistent and describes a different
  window than the one asked for is `InvalidResponse` before a byte is accepted;
  §10 of the design policy requires exactly this, and DIAGNOSTICS.md §6 uses it
  as its example message.
- The write is bounded by the caller's buffer, so a server answering a 64 KiB
  range request with a 10 GB body moves 64 KiB and is then cut off. That is the
  transfer this project exists to avoid, arriving as a hostile case rather than
  as an accident.
- Bounded redirects with an `https` → `http` refusal, bounded retry that is
  never spent on a deadline, a short transfer resumed from where it stopped
  rather than re-fetched whole, and three separable deadlines — connect,
  response, and transfer — so that `Timeout` can name which one elapsed, which
  DIAGNOSTICS.md requires of `HTTP006`.
- The narrow internal transport seam ADR-0003 called for. libcurl is named in
  exactly one translation unit, `src/CurlTransport.cpp`, appears in no installed
  header, and is linked `PRIVATE` and statically. Everything above the seam —
  framing, redirects, retry, validator handling, the whole projection onto the
  typed vocabulary — is exercised offline against a scripted transport, and the
  seam is what makes a future `usdAssetWasm` a second implementation of one
  interface rather than a second backend.
- `tests/corpus`, the projection of each corpus behavior onto the typed
  vocabulary — which `Behavior` produces which `StatusCode`. All 18 behaviors
  are covered and the coverage is asserted against `AllBehaviors()` at runtime,
  so adding a behavior without adding a projection fails the run. Neither side
  knows the other: nothing in `tests/fixture-server` has heard of `StatusCode`
  and nothing in `usdAssetHttp` has heard of `Behavior`, which is what makes a
  disagreement between them evidence rather than a tautology. It also asserts
  the negative this project cares most about — that a URL carrying userinfo and
  a query token appears in no rendered message.
- Sanitizer coverage over the whole HTTP path. ASan, UBSan, and TSan are green
  across all 17 tests, including the boundary row and the corpus projection.
  TSan is the one that matters here: `v0.2.0` is the first release with many
  threads reading one asset over sockets, and asserting that in prose asserts
  nothing.

- `tests/fixture-server`, the hostile-server corpus, standing up **before** the
  HTTP backend rather than beside it. A loopback HTTP/1.1 origin on an ephemeral
  port, with 18 named behaviors covering all nine conditions §11.2 of the design
  policy requires — no `Accept-Ranges`, a `200` answering a `Range`, a truncated
  body, a wrong `Content-Range`, a mid-read `ETag` change, a redirect chain, a
  slow response, a connection reset, and a `416` — plus three more from
  constraints fixed elsewhere: an unknown `Content-Length` (ADR-0003), a
  transient `503` for bounded retry (§4.1), and `403` against `404`
  ([DIAGNOSTICS.md](docs/architecture/DIAGNOSTICS.md) §4.4). Behaviors are
  enumerable at runtime and the self-test fails when one has no case, so the
  coverage claim is checkable rather than asserted.
- A self-test for the corpus, which is the part that makes it an oracle rather
  than a second unknown. It asserts over a raw socket that each behavior puts on
  the wire exactly what its name claims, with a client that shares the socket
  layer with the server and no HTTP code at all — the same separation the
  boundary suite keeps between its oracle and `usdAssetLocal`. It was checked
  against deliberately broken servers before it was trusted: seven mutations
  each produced a named failure, and the eighth is recorded as an equivalent
  mutant rather than counted as coverage.
- A request log on the fixture server — method, target, `Range`, `If-Range`, and
  the answered status. Bounded redirects, "no request was issued for a
  zero-length read", and "`If-Range` on every range request after open" are
  properties of what was *sent*, and there is no other way to assert them.
- [ADR-0003](docs/adr/0003-http-client-dependency.md): the HTTP client
  dependency is **libcurl**, acquired through a private `find_package(CURL)` in
  `libs/usd-asset-http` and reached only through a narrow internal transport
  seam, so no libcurl type appears in a header and the choice stays cheap to
  supersede. It is chosen for exact control rather than for convenience:
  bounded redirects need `CURLOPT_FOLLOWLOCATION` off, ADR-0002's rule that a
  `200` answering a `Range` request is `RangeNotSupported` needs the raw status
  of a response whose body is valid, and framing validation needs
  `Content-Range` before anything has interpreted it. Vendoring and pinned
  `FetchContent` were considered and rejected, with the trigger for revisiting
  each recorded.
- The Wasm criterion in §13 of the [design policy](docs/design/DESIGN_POLICY.md)
  is answered rather than deferred. libcurl does not build for Wasm; the
  reserved `usdAssetWasm` backend over `fetch` is the answer, which the
  workspace contract had already architected as a sibling backend on the
  unchanged `AssetReader` contract. The ADR records this as an accepted cost
  and states what would falsify the reasoning.
- **`openstrata.ci.yaml`, and the workflow generated from it.** The house rule
  is that CI semantics live in the matrix and the workflow YAML is generated;
  at `v0.1.0` that rule could not apply, because no cell shape can name a
  workspace that contains no bundle. `plugins/http-resolver` is what it was
  waiting for. Six `pull_request` cells: the dependency graph on Linux and
  Windows, `ost build` plus `ost test` on Linux and macOS arm64 — which is where
  the release's remote claim lives, in `httpResolver_stage` — and the bundle
  itself through the verification pyramid on Linux and macOS arm64. One rung
  that could not run before now does: `ost plugin test --workspace --graph-only`
  reported `PRECONDITION_FAILED` at `v0.1.0` and now reports 1 bundle, 3
  libraries, 3 library edges, valid.
- **`.github/workflows/plugin-windows-ci.yml`, hand-authored, and why.** libcurl
  on Windows comes from vcpkg (ADR-0003); a generated cell renders a
  host-package installer for `apt` and `brew` only, and `ost build` accepts no
  prefix, no toolchain file, and no `-D`. A generated Windows cell therefore
  fails at `find_package(CURL)` before anything is compiled, and `v0.2.0`'s exit
  criterion names Windows. The lane installs libcurl itself and then runs the
  same two commands the generated workspace cells run — but it **declares no
  pins of its own**: the `ost` version, the runtime artifact digest, and its OCI
  reference are read back out of `openstrata.ci.yaml` at run time through
  `ost ci matrix --json`, so there is no second copy to drift. It also asserts
  `httpResolver_stage` by name from the `ctest` log, because a suite that
  skipped it reports the same "all tests passed" as one that ran it. Full
  account, with two further upstream asks:
  [report 03](docs/reports/ost/03-2026-08-18-a-support-matrix-with-one-hand-authored-lane.md).
- **CI pins `ost` 0.21.0, and the reason is a skew rather than a preference.**
  This repository is developed against 0.22.2, and its `ost runtime validate`
  requires runtime manifest schema 7. Every published `cy2026`/`usd` runtime
  artifact was produced by `ost` 0.20.0 and carries schema 3, so a step every
  generated cell runs before it builds anything fails with
  `manifest-schema` and `digest-integrity`. It is not a CI fact — it reproduces
  on a developer machine against a runtime that had just built and tested the
  workspace. 0.21.0 is the newest CLI that accepts these artifacts, it accepts
  this matrix unchanged, and the generated workflow is produced by it so the YAML
  and the CLI that runs it are the same version. The pin moves back up when a
  runtime built by a newer `ost` is published.
- **Two more things contact with CI found, both fixed where they belong.**
  `host_python: "3.13"` on the workspace cells: the field is documented for
  schema tooling, which this workspace has none of, and the step it renders also
  puts a `libpython3.13.so.1.0` on the Linux loader path — without it
  `httpResolver_stage`, the only test here that links OpenUSD, died before
  `main` while the other twenty passed — and the Windows lane, which renders no
  cell and so inherits nothing, needed the same `actions/setup-python` written
  out: without it the same test exited `0xC0000135`, `STATUS_DLL_NOT_FOUND`, for
  want of a `python313.dll` a developer machine has and a runner does not. On the
  Windows lane also,
  vcpkg's `zs.lib` aliased to `zlib.lib`. `find_dependency(ZLIB)` inside vcpkg's
  `CURLConfig.cmake` reaches CMake's own `FindZLIB`, which searches for `z`,
  `zlib`, `zdll`, `zlib1`, `zlibstatic`, or `zlibwapi`; vcpkg's zlib port
  installs `zs.lib`, so the module finds `zlib.h`, reports the version it read
  out of it, and misses the library. `core-ci.yml` avoids this through the vcpkg
  toolchain file, which is what `ost build` will not take. Copying the file to
  the name the module looks for changes nothing about what is linked, and the
  lane lists the directory unfiltered so the next mismatch of this kind is a
  diagnosis rather than a round trip.

### Changed

- `NOTICE` carries the curl copyright and license text, and no longer says this
  release links no third-party library. It does now, from one module.
- `.github/workflows/core-ci.yml` installs libcurl per platform, the way
  ADR-0003 names. The lane's contract is unchanged and still asserted from the
  configure log: it needs no *OpenUSD runtime*, and never claimed to need no
  dependencies.
- The HTTP client is no longer a blocking item in
  [implementation status](docs/roadmap/implementation-status.md), and neither is
  the fixture server, the backend, or the bundle. What remains in phase 2 is
  `openstrata.ci.yaml` — which now, for the first time, has a bundle to name —
  and the recorded baseline.
- [RESOLVER.md](docs/architecture/RESOLVER.md) §7 no longer requires the
  identifier table to be "a concurrent map, not a mutex-wrapped one". That was a
  statement about a data structure where the property that matters is the one
  beside it: what must not happen is a lock held across a network round trip,
  because that is what makes one slow origin stall every other asset's
  resolution. A short critical section around a hash lookup is not that, and the
  requirement is now written as the property so it can be met without acquiring
  a concurrency library for one map.
- [RESOLVER.md](docs/architecture/RESOLVER.md) §2.1 gains three rules the
  implementation forced into the open: characters that cannot appear literally
  in a URI are percent-encoded (a human-authored `tree bark.png` is a valid asset
  path and not a valid URI), decoding runs before dot-segment removal, and the
  userinfo is removed. §2.1.1 states why `GetExtension` is overridden at all —
  the default returns `usda?X-Amz-Signature=…` for a signed URL, matches no file
  format, and presents as an unsupported format rather than as the resolver bug
  it is.
- [docs/reports/ost/](docs/reports/ost/README.md) gains
  [report 02](docs/reports/ost/02-2026-08-18-resolver-bundle-under-the-pyramid.md):
  the first `usd-asset-resolver` bundle through `ost plugin build`, `doctor`, and
  the verification pyramid. L0, L1, L3, L4, and L5 pass; **L2 fails
  structurally**, because it asserts that `Resolve` returned a path and a network
  resolver cannot satisfy that without an origin listening. The failure is
  recorded rather than worked around: the two ways to make it green are a
  local-file branch in the resolver, which would change how local assets open,
  and a fixture that is a URL, which the on-disk fixture check would then fail.
  The reports README predicted this subject before the bundle existed, and its
  prediction is answered in place rather than rewritten.
- [CONFIGURATION.md](docs/reference/CONFIGURATION.md) stops saying nothing here
  is implemented. The five transport bounds are, with the defaults now written
  in the table rather than promised, and `READ_TIMEOUT_MS` is described as what
  the backend actually enforces — a deadline to the status line, not an
  inter-byte one.
- [WORKSPACE.md](docs/architecture/WORKSPACE.md) records a second legal reverse
  edge. There was one — the boundary row reaching the fixture server to
  provision remote fixtures — and `tests/corpus` is the other. Both live outside
  `libs/` rather than in the backend's own tests, and that placement is
  load-bearing: a module's tests must not depend on anything outside `libs/`, or
  `ost library build libs/usd-asset-http` stops working.
- The [workspace contract](docs/architecture/WORKSPACE.md) records the fixture
  server's dependency direction, which is that it has none: not `usdAssetIo`,
  not a backend, and not the HTTP client. It links the platform's sockets and
  the standard library. Not knowing `usdAssetIo` is the load-bearing half — a
  corpus that could name `StatusCode` would begin asserting the backend's
  interpretation, and a disagreement between the two would stop being evidence.

### Fixed

- **Two concurrent runs of one boundary row deleted each other's fixtures.**
  The suite's temporary workspace was named for the backend and the row —
  `usd-http-resolver-boundary-http-property` — and its constructor removes a
  stale directory of that name before creating it. That is correct for one
  process and wrong for two: running the ASan and the TSan build of the same row
  at once, which is what walking both sanitizer lanes on one machine does, meant
  whichever started second deleted the fixtures the first was still reading. It
  surfaced as `oracle NotFound` and `bytesRead 1, oracle 0` against a backend
  that had returned correct bytes — a harness defect wearing a backend defect's
  clothes, and intermittent, because it depended on which process reached the
  constructor first. The workspace path now carries the owning process id, so a
  run cleans up after itself and after nothing else; `usdAssetLocal`'s
  `TempDirectory` had the same shape and got the same treatment. It was found
  while walking the release gate and never reached CI, where each sanitizer lane
  is its own runner and the collision cannot happen.
- **A stalled apt mirror held the CI lanes for six hours.** On 2026-08-18 three
  Linux jobs of one run sat in `apt-get update` with nothing to show for it; two
  recovered when the run was re-fired and the third ran until the six-hour job
  limit killed it, having built nothing and tested nothing. apt has no deadline
  of its own, so it was given one — `Acquire::Retries` and per-protocol timeouts
  written once per job into `/etc/apt/apt.conf.d/`, covering the calls that exist
  and any a later step adds — with `timeout-minutes` on each apt step as the
  backstop for the hangs those options cannot see, the dpkg lock being the
  realistic one. It is the rule this repository already applies to its own
  fixtures, applied to the runner's package manager: a hung exchange fails the
  lane rather than holding it.
- **`usdAssetHttpConfig.cmake` did not find its own private dependency.** The
  bundle is the first thing to consume the *installed* `usdAssetHttp` package
  rather than the in-tree target, and it failed at generate time with "the link
  interface of target `usdasset::http` contains `CURL::libcurl` but the target
  was not found". The config file asserted, in a comment, that a `PRIVATE` link
  is invisible to a consumer. That is true of a shared library and false of a
  static one: CMake records a static library's private link libraries as
  `$<LINK_ONLY:…>` in the exported interface, because the archive carries no
  code for them and whoever links last has to supply them. ADR-0003's actual
  claim — no installed header includes `curl.h`, no consumer inherits curl's
  include directories or compile definitions — is unaffected, and the comment
  was corrected rather than deleted.

Two further defects in the HTTP backend, both caught by suites that already
passed before it existed — which is the return on having built them first.

- **A deadline that elapsed mid-body was resumed as though it were a short
  read.** The read loop treats a transfer that stopped early as resumable and
  asks for the remainder, which is right for a connection that ended and wrong
  for a clock that ran out: the caller has already said how long it is prepared
  to wait, and asking again spends that budget a second time. The failure then
  surfaced as `InvalidResponse` after the retries, naming the server for the
  caller's own deadline. Deadlines are now non-resumable and reported as
  `Timeout` immediately, with the bytes that did arrive. Found by the corpus's
  `SlowBody` row, which exists precisely because a backend with only a total
  deadline cannot tell it from `SlowHeaders`.
- **Every deadline on a reused connection was reported as a connect timeout.**
  `CURLINFO_CONNECT_TIME_T` measures a connection *this transfer established*
  and is zero when the transfer reused one from the pool — which, once
  connection reuse works, is most of them. Reading it as "was there a
  connection" named the one deadline that provably had not elapsed. It now reads
  pre-transfer time, which is non-zero however the connection was obtained.
  Found by the same row, and only after the first fix stopped masking it.

Six more found by review of the branch before it merged, four of them in
handling that only a hostile or unlucky server reaches:

- **`RemoveDotSegments` collapsed empty path segments.** `/a//b` became `/a/b`,
  and RFC 3986 §5.2.4 removes `.` and `..` and nothing else. It looks like
  tidying and is a rename: an object-storage key may legitimately contain an
  empty segment, so the collapsed form names a different object, and a
  pre-signed URL whose signature covers the canonical path stops verifying.
  Every path went through it on the way in.
- **A `416` never asked whether the asset had simply changed.** A range this
  reader already sized cannot lie outside the asset, so a server refusing it is
  evidence the representation moved — and a `416` is required to carry
  `bytes */<complete-length>`, which says so outright. It reported
  `InvalidResponse` with a message that was factually false. It was the one
  status with no coverage for the release's central guarantee.
- **The retry budget was nested rather than shared.** `maxAttempts` bounded the
  read's resume loop and the transport's retry loop independently, so one
  `Read` could cost `maxAttempts²` requests against a documented per-operation
  cap of `maxAttempts` — nine where the caller asked for three. One budget is
  now allocated per logical operation and both loops draw from it; redirect hops
  still draw from `maxRedirects`, because a hop is not a retry.
- **The response deadline was charged for the connect.** It was measured from
  the start of the exchange rather than from when the request went out, so it
  could fire up to `connectTimeoutMs` early — and name the wrong deadline, which
  is the one thing `HTTP006` is required to get right.
- **`curl_slist_append`'s return was unchecked.** It returns null on allocation
  failure and does not free what it was given, so the obvious idiom both leaks
  the list and drops every header. A dropped `Range` does not fail: it succeeds,
  as a `200` carrying the whole representation, which this backend would then
  correctly report as `RangeNotSupported` — a transient allocation failure
  wearing the name of a terminal server capability.
- **Conflicting duplicate `Content-Length` lines were accepted.** Last-wins,
  where RFC 9110 §8.6 requires the message to be rejected; an intermediary that
  believes the first and an origin that believes the last disagree about where
  one message ends and the next begins. Repeated *identical* values are still
  accepted, because that is redundant rather than hostile.

One defect in the metrics accounting, found by an assertion on counters rather
than on behavior:

- **Metadata requests were counted twice.** `AddMetadataRequest` already bumps
  `requestCount` — a metadata request is a request — and the backend called
  both. Nothing observable broke, which is the point: it would have inflated the
  denominator of `requestEfficiency` and understated the amplification this
  project claims to be measured by, in the release that first has a number to
  report.

Eight defects in the fixture server, found by reviewing it before the backend
started depending on it rather than after. None of them ships; all of them would
have been debugged as backend bugs.

- **`Stop()` hung on a client that stopped reading.** "Shutdown never hangs" was
  contract in `Server.h` and in the README and was not true: the condition
  variable `Stop()` signals reaches the deliberate stalls and does not reach a
  handler sitting in `send`. Accepted sockets are now non-blocking and every
  write is abandoned when the stop flag is set. The regression case hangs
  without the fix on Linux; Winsock buffers a whole 64 MiB response without
  blocking, so it proves nothing there and says so.
- **Connection threads were joined only at `Stop()`**, one unreaped stack per
  request. The accept loop now reaps as they finish, and `OpenConnections()`
  exists so the self-test can say it does.
- **`ContentRangeShifted` served a correct response** for any range covering the
  whole asset — `bytes=0-255`, `bytes=0-`, and `bytes=-256` alike. The window
  now moves up one and clamps at EOF.
- **`ContentRangeTooShort` was a no-op for a single-byte range.** It still is,
  because `Content-Range` cannot describe an empty range; the difference is that
  the floor is now stated beside the enumerator and pinned by a case, instead of
  being discovered by a backend test that leaned on it.
- **The `.hopN` redirect alias routed on half a match.** `/chain.hop` redirected
  to `/chain.hop.hop1` and then `404`ed, and `/normal.hop` served `/normal`'s
  body under a name nobody registered while counting the request against it. The
  suffix must now parse as digits and name a `RedirectChain` asset.
- **The accept loop spun at 100% of a core** on a persistent accept failure —
  reachable through the thread leak above. It backs off at the poll cadence.
- **A request was logged with a status that never reached the wire.** `Server.h`
  gives `0` the meaning "deliberately never answered"; the log is now written
  after the send, and records `0` when the send failed.
- **A reset-mid-body assertion was really an assertion about the runner.** What
  an `RST` destroys is measured, not assumed: Linux delivers the eight buffered
  body bytes and Winsock discards them, and the headers survive on both only
  because the reader gets to them first.

### Known gaps

- **No binary package is published, so gate 9 is not asserted.** This is a
  source tag. The install rules exist and were exercised: two independent builds
  of the same tree produce 24 of 28 installed files byte-identical, and the four
  that differ — three static libraries and the bundle's shared library — differ
  only in embedded build timestamps. The measurement and what it would take to
  close the gap are in §5 of the [record](docs/releases/v0.2.0.md).
- **The two bundle cells stop at L1, and the Windows cell at `graph`.** Neither
  cap is a workaround. `ost plugin test`'s L2 asserts that `Resolve` returned a
  path, which for a network resolver means an origin has to be listening, and
  there is no way to skip one rung and keep the ones above it — so L3, L4, and
  L5 pass and are not in a cell. The Windows cell cannot build at all, for the
  reason above. Both are recorded in
  [report 03](docs/reports/ost/03-2026-08-18-a-support-matrix-with-one-hand-authored-lane.md)
  with the upstream change that would lift each.
- **There is no fallback when a server refuses `HEAD`.** §4.1 of the design
  policy admits "a minimal metadata request where `HEAD` is unavailable"; a
  `405` or `501` is reported as `Unsupported` instead. The hostile corpus has no
  row that refuses `HEAD`, so a fallback would ship unexercised, and this
  repository's rule is to name a gap rather than fill it speculatively.
- **`If-Range` with a `Last-Modified` validator is implemented and not covered
  by the corpus.** The fixture server compares `If-Range` only against its
  `ETag`, so an asset with a date and no entity tag cannot exercise the
  conditional path there — a fixture shaped that way would fail for the
  fixture's reason rather than the backend's. The behavior is unit-tested
  against a scripted transport; the server-side half is what is missing.
- **A `206` covering less than was asked for is refused, which is stricter than
  RFC 9110 requires.** An origin may answer a single-range request with a prefix
  of it, and the resume loop could accept that and ask for the rest; §10 of the
  design policy, DIAGNOSTICS.md §6, and the corpus's `ContentRangeTooShort` row
  all say not to. Relaxing it is a change to those documents first, for every
  backend, and not a quiet accommodation in one. The cost: an origin that caps
  the size of a range response is refused rather than read in pieces, and
  nothing in the corpus behaves that way, so nothing would notice if the rule
  were wrong.
- **The sanitizer lanes do not instrument libcurl.** It is the runner's own
  package, so the evidence is about this repository's code and not about the
  client behind the seam. That is the intended scope — what needed proving is
  that many threads on one reader do not race, that the offset arithmetic does
  not overflow, and that nothing writes past a caller's buffer, all of which
  live above the seam.
- **The scheme-downgrade case is not in the corpus and cannot be.** §10 of the
  design policy requires rejecting an `https` to `http` redirect; the fixture
  server speaks plaintext HTTP, so there is no `https` to downgrade from.
  Faking it with a `Location` a test declares was reached over TLS would assert
  nothing, so the case is left out and named as absent. It is redirect policy
  rather than server behavior, and it is now tested where the previous release
  said it belonged — in `usdAssetHttp`'s own tests, against a scripted
  `Location`.
- **`RST` against `FIN` is asserted only where it is portable.** The reset
  behaviors close with `SO_LINGER{1, 0}`, which is a real reset; whether a peer
  observes `ECONNRESET` or an orderly EOF is the platform's decision, and so is
  how much of what was already sent survives it. The corpus asserts the fact a
  backend must handle — the promise in the headers was not kept — and not the
  errno, the byte count, or even the arrival of the headers, none of which the
  runner promises.

## `v0.1.0` — 2026-08-16

The release that decides whether the rest of the project is buildable. It ships
no network code, and its centre of gravity is the test suite rather than the
reader. The immutable record, including the release gate and what it found, is
[docs/releases/v0.1.0.md](docs/releases/v0.1.0.md).

### Added

- `libs/usd-asset-io`, the transport-independent core: the `AssetReader`
  random-access contract and `AssetMetadata`; the validator value types and the
  `Stable` / `Unstable` / `Unavailable` classification derived from them; the
  typed `StatusCode` vocabulary; the shared offset arithmetic in
  `ResolveReadRange`, so the EOF boundary and the overflow check exist once
  rather than once per backend; and the metrics counters, latency histograms,
  and process aggregate.
- Credential elision in every message and metrics dump, covering the query
  string **and** the userinfo component of an authority — recognized only
  where an authority can legally appear, so a filesystem path with a `//` in it
  is left alone.
- `libs/usd-asset-local`, the local-file backend and the correctness oracle:
  positional reads with no lock, size and range-support discovery at open, a
  filesystem-derived validator, and `AssetChanged` when an asset is republished
  underneath an open reader.
- `tests/boundary`, the shared boundary suite: the required boundary cases per
  fixture size, the short read below EOF, the mid-read revision change, biased
  property cases with shrinking and a reported seed, and the concurrency cases —
  all against an independent naive oracle that shares no code with the backend
  it checks. Entering a backend is a row, and the local backend's is
  `tests/boundary/backends/boundary_local_main.cpp`.
- Sanitizer build configuration: `USD_HTTP_RESOLVER_SANITIZER`, and the
  `core-asan` and `core-tsan` CMake presets.
- `.github/workflows/core-ci.yml`, the runtime-free CI lanes: the core build and
  test on Windows, Linux, and macOS arm64 with no OpenUSD present — asserted
  from the configure log, not inferred from a green build — and `core-asan` and
  `core-tsan` on Linux. Hand-authored rather than generated from
  `openstrata.ci.yaml`, because every `ost` cell pins and materializes an
  OpenUSD runtime and no cell can name a workspace that contains no bundle;
  the account and the upstream asks are in
  [report 01](docs/reports/ost/01-2026-08-16-v0.1.0-ci-without-a-support-matrix.md).
- `core-msvc`, the `core` build through the Visual Studio generator, so the
  Windows lane works outside a developer command prompt — in CI and on a
  contributor's machine.
- `VERSION`, `LICENSE`, `NOTICE`, and OpenStrata plain-library descriptors for
  both `libs/` modules.

### Fixed

- The UndefinedBehaviorSanitizer lane reported nothing it found.
  `-fno-sanitize-recover=all` now accompanies the sanitizer flags: UBSan's
  default is to print a violation and continue, so a signed overflow in the
  offset arithmetic exited `0` and CTest reported a pass. Measured both ways in
  [report 01](docs/reports/ost/01-2026-08-16-v0.1.0-ci-without-a-support-matrix.md)
  §4.

### Known gaps

- The MSVC AddressSanitizer lane is unverified and the build says so. Sanitizer
  evidence is a clang or GCC lane, and the CI cells are Linux for that reason.
- No I/O baseline is recorded. `v0.1.0` moves no bytes over a network, so there
  is nothing yet for the ratio this project claims to be measured against.
- `openstrata.ci.yaml` does not exist. It arrives in `v0.2.0` with the first
  bundle a cell can name; the runtime-free lanes stay outside it.
