# Contributing

Thanks for helping improve `usd-http-resolver`. Contributions should preserve
the project's central boundary: it resolves remote assets and serves bytes;
it does not learn what those bytes mean.

Before opening an issue or pull request, read the
[Code of Conduct](CODE_OF_CONDUCT.md) and check the existing documentation in
[`docs/`](docs/README.md). For architecture or policy changes, the canonical
documents in `docs/design/` and `docs/architecture/` are part of the change,
not background reading.

## Start with an issue

Use the repository's issue templates for bugs and feature proposals. A useful
bug report includes the smallest reproduction, the expected and actual result,
the platform and compiler, the CMake preset or command used, and the complete
failure output. A proposal should explain the problem, the proposed contract,
and how it fits the existing module boundaries.

Do not put credentials, private URLs, customer data, or vulnerability details
in an issue. Follow [SECURITY.md](SECURITY.md) for security reports.

## Local setup

The normal contributor path builds the core without OpenUSD:

```sh
cmake --preset core
cmake --build --preset core
ctest --preset core
```

On Windows outside a Visual Studio developer prompt, use the generator that
matches CI:

```sh
cmake --preset core-msvc
cmake --build --preset core-msvc
ctest --preset core-msvc
```

Requirements and the OpenStrata workflow are documented in
[docs/guides/BUILDING.md](docs/guides/BUILDING.md).

## Tests and evidence

Run the narrowest relevant test while iterating, then run the complete core
suite before requesting review. Changes to a backend must pass the shared
[boundary suite](docs/contributing/BOUNDARY_SUITE.md) unchanged. Changes to
HTTP behavior should also cover the hostile-server corpus; changes to cache or
I/O behavior should update the relevant baseline or measurement record.

For changes that affect concurrency, offset arithmetic, or buffer handling,
run the sanitizer lanes when the local toolchain supports them:

```sh
cmake --preset core-asan
cmake --build --preset core-asan
ctest --preset core-asan

cmake --preset core-tsan
cmake --build --preset core-tsan
ctest --preset core-tsan
```

Record the commands you ran and any platform-specific limitation in the pull
request. A passing test is evidence for the behavior it exercises; it is not a
reason to weaken a contract or silently widen scope.

## Making a change

1. Keep the change focused and preserve existing public APIs unless the issue
   requires a contract change.
2. Add or update tests before treating a behavioral change as complete. Prefer
   an independent oracle or fixture over duplicating the implementation in a
   test.
3. Update the owning documentation when behavior, configuration, compatibility,
   security policy, or measured I/O changes.
4. Do not add network access, format knowledge, or OpenUSD dependencies to the
   core libraries without documenting the boundary and its consequences.
5. Keep generated workflow files consistent with their source configuration.
   The core workflow is intentionally hand-authored; do not replace its
   runtime-free or sanitizer lanes with a less specific check.

Pull requests should explain the motivation, the behavior changed, the tests
run, and any compatibility or performance effect. Small, reviewable commits
are helpful, but the project does not require a particular commit-message
format.

## Review and merge

Maintainers may request tests, documentation, design clarification, or a
smaller scope before merging. Review comments are part of the technical record:
resolve them explicitly, or explain why the current behavior is intentional.
CI must be green unless a maintainer has documented an exception.
