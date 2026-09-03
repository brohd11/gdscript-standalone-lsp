#!/usr/bin/env python3
"""Apply a base64-encoded gzip patch idempotently to a Git checkout."""

from __future__ import annotations

import base64
import gzip
import pathlib
import subprocess
import sys


def git_apply(repository: pathlib.Path, arguments: list[str], patch: bytes) -> bool:
    result = subprocess.run(
        ["git", "-C", str(repository), "apply", *arguments, "-"],
        input=patch,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: apply_encoded_patch.py PATCH.gz.b64 REPOSITORY", file=sys.stderr)
        return 2

    encoded_path = pathlib.Path(sys.argv[1])
    repository = pathlib.Path(sys.argv[2])
    patch = gzip.decompress(base64.b64decode(encoded_path.read_bytes()))

    if git_apply(repository, ["--reverse", "--check"], patch):
        return 0
    if not git_apply(repository, ["--check"], patch):
        print(f"{encoded_path.name} does not apply cleanly to {repository}", file=sys.stderr)
        return 1
    if not git_apply(repository, [], patch):
        print(f"failed to apply {encoded_path.name} to {repository}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
