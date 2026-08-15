# OST reports

These reports are append-only records of OpenStrata (`ost`) adoption in this
repository. They preserve the commands that were run, the CI evidence that was
observed, repository-side fixes, and any follow-up asks for OpenStrata.

## Reading order

| Report | Date | Subject | OST version | Result |
| --- | --- | --- | --- | --- |
| — | — | No report yet | — | — |

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

This project exercises a part of the OpenStrata surface the earlier
point-cloud work did not: the `usd-asset-resolver` bundle kind, rather than
`usd-fileformat`. Whether the verification pyramid's Level 4 and Level 6 checks
express "a resolver loaded and claimed its URI schemes" as naturally as they
express "a file format opened a fixture" is an open question, and the answer
belongs in the first report rather than in a design document.

The second likely subject is fixture hosting. Every other bundle kind tests
against a file on disk; this one needs a server. How that composes inside an
`ost plugin test` run — and whether it composes at all — is worth recording
early.
