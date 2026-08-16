# Release records

Each tagged version receives an immutable record here: what shipped, the
supported behavior, the recorded I/O baseline, build requirements, known
limitations, and licensing notes. Release records are history and are not
rewritten after publication.

| Version | Date | Record |
| --- | --- | --- |
| — | — | No release yet. The planned sequence is in the [roadmap](../roadmap/README.md). |

Prepare the record in the release commit immediately before creating its tag.
The tag pins the source commit and the record pins the release scope.

Unreleased work on `main` is tracked in the root `CHANGELOG.md` and, at task
granularity, in
[roadmap/implementation-status.md](../roadmap/implementation-status.md).

## Release gate

A release record is created only after:

1. `VERSION`, `openstrata.toml`, the plugin manifest, the plugin CMake project,
   the tag, and the finalized changelog version agree;
2. every declared CI cell in `openstrata.ci.yaml` passes on Windows, Linux, and
   macOS arm64;
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
