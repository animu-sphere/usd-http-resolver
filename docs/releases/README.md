# Release records

Each tagged version receives an immutable record here: what shipped, the
supported behavior, the recorded I/O baseline, build requirements, known
limitations, and licensing notes. Release records are history and are not
rewritten after publication.

| Version | Date | Record |
| --- | --- | --- |
| [`v0.1.0`](v0.1.0.md) | 2026-08-16 | Read contract, local backend, and the shared boundary suite. No network code. |

The rest of the planned sequence is in the [roadmap](../roadmap/README.md).

Prepare the record in the release commit immediately before creating its tag.
The tag pins the source commit and the record pins the release scope.

Unreleased work on `main` is tracked in the root `CHANGELOG.md` and, at task
granularity, in
[roadmap/implementation-status.md](../roadmap/implementation-status.md).

## Release gate

A release record is created only after:

1. `VERSION`, `openstrata.toml`, the plugin manifest, the plugin CMake project,
   the tag, and the finalized changelog version agree;
2. every declared CI cell passes on Windows, Linux, and macOS arm64 — the
   hand-authored runtime-free lanes in `.github/workflows/core-ci.yml`, and from
   `v0.2.0` the generated cells in `openstrata.ci.yaml` as well;
3. the [shared boundary suite](../contributing/BOUNDARY_SUITE.md) passes against
   every backend, unchanged;
4. the hostile-server corpus passes;
5. sanitizer builds (ASan, UBSan, TSan) pass over the core path, which requires
   no OpenUSD runtime;
6. **an I/O baseline is recorded** for every scenario in
   [METRICS.md](../architecture/METRICS.md) §6, and any regression against the
   previous release is explained in the record;
7. no diagnostic, log line, metrics dump, or persisted artifact contains a
   credential, token, or signed-URL query string;
8. third-party dependencies and their licenses are recorded;
9. package digests are reproducible for an unchanged build.

Gate 6 is specific to this project. A resolver release that ships correct
behavior with a silent doubling of transferred bytes has regressed the only
property it exists to provide, and no functional test would catch it.

Gate 7 is checked mechanically, not by reading. A grep over test output,
diagnostics, and the metrics dump for `Authorization`, `token`, `X-Amz-`,
`Signature`, and `sig=` is part of the release run.

## Gates before a transport exists

Gates 4, 6, and 9 describe a release that moves bytes over a network or ships a
package. `v0.1.0` does neither, and it is the only release planned that does
neither. Rather than argue the point once per record, the rule is stated here:

- **Gate 4** binds any release in which a backend can talk to a server. Until
  one can, there is no corpus to pass and no fixture server to pass it against.
- **Gate 6** follows [METRICS.md](../architecture/METRICS.md) §6, which binds a
  release that *changes I/O behavior*. A release with no transport and no cache
  changes none, and a scenario table whose rows measure requests, cache hits,
  and selectivity cannot be filled in by a `pread`. A record claiming a baseline
  it did not measure would be worse than one stating it had nothing to measure.
- **Gate 9** binds any release that publishes a binary package. A source tag is
  pinned by the tag.

A record marks such a gate *not applicable* and says why in the same row. It
never marks one *waived*, and it never marks one *pass* on the strength of an
argument. From the first release that can meet a gate, that gate is binding
forever after; a gate that stopped applying would be a change to this document,
argued on its own.

## Record contents

Each record states:

- what shipped, and what explicitly did not;
- the capability delta against the previous release;
- the recorded baseline table, with the fixture named;
- the OpenUSD and OpenStrata versions validated against;
- third-party dependencies and licenses;
- known limitations, including any open ADR that constrains behavior;
- migration notes when a diagnostic code, configuration name, or default
  changes.
