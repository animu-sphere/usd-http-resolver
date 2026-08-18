# OST reports

These reports are append-only records of OpenStrata (`ost`) adoption in this
repository. They preserve the commands that were run, the CI evidence that was
observed, repository-side fixes, and any follow-up asks for OpenStrata.

## Reading order

| Report | Date | Subject | OST version | Result |
| --- | --- | --- | --- | --- |
| [01](01-2026-08-16-v0.1.0-ci-without-a-support-matrix.md) | 2026-08-16 | The `v0.1.0` CI cells, and why they are hand-authored rather than generated | 0.22.2 | Landed, with two upstream asks |
| [02](02-2026-08-18-resolver-bundle-under-the-pyramid.md) | 2026-08-18 | The first `usd-asset-resolver` bundle through build, doctor, and the verification pyramid | 0.22.2 | Landed; L2 fails structurally, with three upstream asks |
| [03](03-2026-08-18-a-support-matrix-with-one-hand-authored-lane.md) | 2026-08-18 | `openstrata.ci.yaml` lands: which rungs its cells run, the Windows lane it could not express, and a CLI/runtime version skew | 0.22.2 local, 0.21.0 pinned in CI | Landed; six generated cells, one hand-authored lane, two new upstream asks |

Reports are historical evidence. When a later OpenStrata version changes an
observation, add a new report rather than rewriting the old one.

## What to record

A report is written when one of these happens:

- a bundle first builds, tests, or packages under a new `ost` version;
- a CI matrix lands or changes materially;
- a runtime is re-pinned;
- an `ost` behavior blocks work, and the workaround is worth preserving;
- an upstream ask is raised, and later when it is resolved.

Include the exact commands, the `ost` and runtime versions, the platform, and
the observed output. `--json --redact-paths` produces output suitable for
attaching to a public report.

## Expected first reports

Both predictions below were written before the bundle existed, and
[report 02](02-2026-08-18-resolver-bundle-under-the-pyramid.md) answers them.
They are kept here as written, because a prediction that was right about the
question and wrong about which rung is more useful intact than corrected.

This project exercises a part of the OpenStrata surface the earlier
point-cloud work did not: the `usd-asset-resolver` bundle kind, rather than
`usd-fileformat`. Whether the verification pyramid's Level 4 and Level 6 checks
express "a resolver loaded and claimed its URI schemes" as naturally as they
express "a file format opened a fixture" is an open question, and the answer
belongs in the first report rather than in a design document.

> Answered: Levels 3, 4, and 5 express it fine, because what they open is a
> *local* fixture and the property asserted there is that the host's resolution
> is undisturbed. Level 2 is the one that does not — it asserts that `Resolve`
> returned a path, which for a network resolver requires an origin to be
> listening.

The second likely subject is fixture hosting. Every other bundle kind tests
against a file on disk; this one needs a server. How that composes inside an
`ost plugin test` run — and whether it composes at all — is worth recording
early.

> Answered: it does not compose, and the origin is stood up by the test instead.
> The remote claim lives in `ctest`, where `tests/fixture-server` exists; the
> bundle's declared fixture is a local stage, asserting the property a URI-scheme
> resolver is most likely to break.
