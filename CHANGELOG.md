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

## Unreleased — `v0.1.0`

The release that decides whether the rest of the project is buildable. It ships
no network code, and its centre of gravity is the test suite rather than the
reader.

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
- `VERSION`, `LICENSE`, `NOTICE`, and OpenStrata plain-library descriptors for
  both `libs/` modules.

### Known gaps

- No CI matrix exists, so no sanitizer run has happened. The `v0.1.0` exit
  criterion that the local backend passes the suite *under* ASan, UBSan, and
  TSan is therefore not met; the configuration is in place and the cells are
  not. Tracked in
  [implementation status](docs/roadmap/implementation-status.md).
- The MSVC AddressSanitizer lane is unverified and the build says so. Sanitizer
  evidence is a clang or GCC lane.
- No I/O baseline is recorded. `v0.1.0` moves no bytes over a network, so there
  is nothing yet for the ratio this project claims to be measured against.
