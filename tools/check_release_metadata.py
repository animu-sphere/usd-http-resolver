#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Gate 1 of the release gate, mechanized.

`docs/releases/README.md` requires that "`VERSION`, `openstrata.toml`, the
plugin manifest, the plugin CMake project, the tag, and the finalized changelog
version agree" before a release record is created. Six places is more than a
reader reliably checks, and the failure mode is silent: a release whose manifest
says `0.2.0` and whose `VERSION` says `0.1.0` builds, tests, and tags green, and
is discovered by whoever installs it.

So the gate is a script rather than a reading. It is deliberately not a
formatting check -- it asserts agreement between files that have no other reason
to agree, and says which pair disagreed.

Run it with no arguments to check the tree against itself:

    python tools/check_release_metadata.py

Run it with a tag to check the tree against what is about to be tagged, which is
what `release.yml` does:

    python tools/check_release_metadata.py --tag v0.2.0

Exits 0 when everything agrees, 1 when anything does not. Reads no dependency
outside the standard library, because it runs before anything is installed.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# The bundle whose manifest and CMake project the gate names. A second bundle
# would be a second entry here rather than a second copy of the checks.
BUNDLE = Path("plugins/http-resolver")

SEMVER = re.compile(r"^\d+\.\d+\.\d+$")


class Failure(Exception):
    """A disagreement worth failing the release for."""


def read(relative: str) -> str:
    path = REPO_ROOT / relative
    if not path.is_file():
        raise Failure(f"{relative} does not exist")
    return path.read_text(encoding="utf-8")


def find(pattern: str, text: str, relative: str, what: str) -> str:
    match = re.search(pattern, text, re.MULTILINE)
    if match is None:
        raise Failure(f"{relative} does not state {what}")
    return match.group(1).strip()


def version_from_version_file() -> str:
    version = read("VERSION").strip()
    if not SEMVER.match(version):
        raise Failure(f"VERSION is {version!r}, which is not a semantic version")
    return version


def version_from_openstrata_toml() -> str:
    # Anchored to the [project] table. A bare `version =` would also match a
    # version key in any table added later, and would then agree by accident.
    text = read("openstrata.toml")
    project = re.search(r"^\[project\]$(.*?)(?=^\[|\Z)", text, re.MULTILINE | re.DOTALL)
    if project is None:
        raise Failure("openstrata.toml has no [project] table")
    return find(r'^\s*version\s*=\s*"([^"]+)"', project.group(1),
                "openstrata.toml", "a [project] version")


def version_from_plugin_manifest() -> str:
    # Anchored to the `plugin:` block for the same reason: `runtime:` and the
    # `requires.libraries` entries carry versions of their own, and one of them
    # matching would be meaningless.
    relative = str(BUNDLE / "openstrata.plugin.yaml").replace("\\", "/")
    text = read(relative)
    block = re.search(r"^plugin:$(.*?)(?=^\S|\Z)", text, re.MULTILINE | re.DOTALL)
    if block is None:
        raise Failure(f"{relative} has no plugin: block")
    return find(r'^\s*version:\s*"?([0-9][^"\s]*)"?\s*$', block.group(1),
                relative, "a plugin version")


def version_from_plugin_cmake() -> str:
    # The bundle's CMakeLists reads the repo-root VERSION when it is present and
    # falls back to a literal when it is not, so that a bundle exported on its
    # own still configures. The literal is the thing that can rot: nothing
    # builds through it in this repository, so nothing notices when it is stale.
    # That is exactly what this check is for.
    relative = str(BUNDLE / "CMakeLists.txt").replace("\\", "/")
    text = read(relative)
    return find(r'^\s*set\(_http_resolver_version\s+"([^"]+)"\)', text,
                relative, "a standalone version fallback")


def changelog_versions() -> list[str]:
    """Every released section heading, newest first."""
    text = read("CHANGELOG.md")
    return re.findall(r"^##\s+`v(\d+\.\d+\.\d+)`", text, re.MULTILINE)


def check_release_record(version: str) -> None:
    relative = f"docs/releases/v{version}.md"
    text = read(relative)

    tag = find(r"^-\s*Tag:\s*`([^`]+)`", text, relative, "the tag it records")
    if tag != f"v{version}":
        raise Failure(f"{relative} records tag {tag}, but VERSION is {version}")

    # The index is how anyone finds the record. A record that exists and is not
    # linked is a record nobody reads.
    index = read("docs/releases/README.md")
    if f"(v{version}.md)" not in index:
        raise Failure(f"docs/releases/README.md has no row linking v{version}.md")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--tag",
        help="the tag about to be created, e.g. v0.2.0. Checked against VERSION.",
    )
    args = parser.parse_args()

    try:
        version = version_from_version_file()

        # Every source that is required to agree, and where it comes from, so
        # that a failure names the file to edit rather than the fact of
        # disagreement.
        sources = {
            "VERSION": version,
            "openstrata.toml [project].version": version_from_openstrata_toml(),
            f"{BUNDLE.as_posix()}/openstrata.plugin.yaml plugin.version":
                version_from_plugin_manifest(),
            f"{BUNDLE.as_posix()}/CMakeLists.txt standalone fallback":
                version_from_plugin_cmake(),
        }

        disagreeing = {name: value for name, value in sources.items() if value != version}
        if disagreeing:
            lines = "\n".join(f"    {name}: {value}" for name, value in disagreeing.items())
            raise Failure(
                f"VERSION is {version}, and these do not agree:\n{lines}"
            )

        released = changelog_versions()
        if version not in released:
            raise Failure(
                f"CHANGELOG.md has no `v{version}` section. The Unreleased "
                f"section is finalized into one as part of the release commit."
            )
        if released[0] != version:
            raise Failure(
                f"CHANGELOG.md's newest released section is v{released[0]}, "
                f"but VERSION is {version}. The newest section is the release."
            )

        check_release_record(version)

        if args.tag is not None and args.tag != f"v{version}":
            raise Failure(f"tag is {args.tag}, but VERSION is {version}")

    except Failure as failure:
        print(f"release metadata: {failure}", file=sys.stderr)
        return 1

    for name, value in sources.items():
        print(f"  {value}  {name}")
    print(f"  v{version}  CHANGELOG.md newest released section")
    print(f"  v{version}  docs/releases/v{version}.md, linked from the index")
    if args.tag is not None:
        print(f"  {args.tag}  tag")
    print(f"release metadata: everything agrees on {version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
