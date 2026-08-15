# Module README contract

Every directory under `libs/` and `plugins/` contains a `README.md`. That
README is part of the module contract, not optional supplementary
documentation: it is where the module states what it owns, what it refuses to
own, and what a caller may rely on.

This is invariant 10 of the [workspace contract](../architecture/WORKSPACE.md).

## Ownership

A code change that modifies a module contract updates that module's `README.md`
in the same pull request. Examples:

- adding or removing a public API;
- changing read, EOF, or short-read semantics;
- changing ownership or lifetime rules for buffers;
- changing retry, timeout, or redirect policy;
- changing what a cache key contains;
- adding or removing a third-party dependency;
- changing thread-safety guarantees;
- changing license obligations.

A reviewer treats a contract change without a README change as incomplete.

## Required sections: library README

```text
# Module name

## Purpose
## Responsibilities
## Non-responsibilities
## Public API
## Dependencies
## Data flow
## Error and diagnostic behavior
## Threading and ownership
## Build and test
## Known limitations
## Planned work
```

A library README in this repository must additionally:

- state that OpenUSD is not required, and that the module tests without a USD
  runtime — if that is not true, the module is in the wrong directory;
- state its buffer ownership and lifetime rules explicitly;
- state its thread-safety guarantee in terms of what may be called
  concurrently, not as the word "thread-safe";
- state whether it issues network requests, and under what conditions;
- link the architecture contracts it implements.

## Required sections: backend README

A backend adds:

```text
## Transport
## Range support behavior
## Failure mapping
## Retry, timeout, and redirect policy
## Metrics populated
## Third-party dependencies and licenses
```

The failure mapping section is a table from the transport's failures to the
codes in [DIAGNOSTICS.md](../architecture/DIAGNOSTICS.md). A backend that
cannot produce that table has not finished mapping its failures, and its
callers cannot branch on them.

## Required sections: plugin README

```text
# Plugin name

## Purpose
## Registered URI schemes
## Resolution behavior
## ArAsset behavior
## Configuration
## Plugin discovery and installation
## Diagnostics
## Build and test
## Runtime dependencies
## Licensing
## Known limitations
## Compatibility
```

The plugin README must be explicit about what it does **not** do, because a
resolver's failure modes are invisible until they bite: it does not change how
local assets resolve, it does not support writing, `GetBuffer()` returns null
by design, and it caches bytes but never generated USD.

## Relationship to the shared documentation

A module README describes that module. It does not restate a shared contract;
it links to it. The normative sources are:

| Topic | Document |
| --- | --- |
| Structure, identities, dependency directions | [WORKSPACE.md](../architecture/WORKSPACE.md) |
| Read semantics and backend obligations | [ASSET_READER.md](../architecture/ASSET_READER.md) |
| Resolution, URI handling, `ArAsset` | [RESOLVER.md](../architecture/RESOLVER.md) |
| Block cache, coalescing, cache identity | [CACHE.md](../architecture/CACHE.md) |
| Typed errors and `HTTPxxx` codes | [DIAGNOSTICS.md](../architecture/DIAGNOSTICS.md) |
| Counters and baselines | [METRICS.md](../architecture/METRICS.md) |
| Configuration | [CONFIGURATION.md](../reference/CONFIGURATION.md) |
| What is implemented today | [CAPABILITY_MATRIX.md](../reference/CAPABILITY_MATRIX.md) |

When a module README and one of those documents disagree, the shared document
wins and the README is the bug.

## Status language

Use status words that separate capability from reachability:

```text
implemented                   in this module, tested
implemented, not connected    exists in a library, nothing reaches it
planned                       has a contract, no implementation
not planned                   explicitly out of scope
```

"Not implemented" without qualification is only correct when no code in the
repository performs the behavior.

## Performance claims

A README may not claim a performance property without a link to a recorded
measurement. "Fetches only what is needed" is a design intent; a recorded
selectivity number on a named fixture is a fact. This repository's entire value
proposition is a ratio, so an unsupported claim about it is the one kind of
documentation error that matters most here.
