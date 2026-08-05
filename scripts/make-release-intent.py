#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import NoReturn


REPOSITORY = "TheOnlyHyland/True-Family-Voice-Firmware"
PACKAGE_FILES = (
    "true-family-voice-esp32s3.factory.bin",
    "true-family-voice-esp32s3.ota.bin",
    "true-family-voice-esp32s3.elf",
    "SHA256SUMS",
    "manifest.json",
)
VERSION_PATTERN = re.compile(
    r"^(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)"
    r"(?:-(?:alpha|beta|rc)(?:\.(?:0|[1-9]\d*))?)?$"
)
OBJECT_ID_PATTERN = re.compile(r"^[0-9a-f]{40}$")


def fail(message: str) -> NoReturn:
    raise SystemExit(message)


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def main() -> None:
    if len(sys.argv) != 7:
        fail(
            f"usage: {sys.argv[0]} VERSION SOURCE_COMMIT SOURCE_TREE "
            "PRERELEASE PACKAGE_DIRECTORY INTENT_DIRECTORY"
        )

    version, source_commit, source_tree, prerelease_raw = sys.argv[1:5]
    package = Path(sys.argv[5]).resolve()
    destination = Path(sys.argv[6]).resolve()
    if VERSION_PATTERN.fullmatch(version) is None:
        fail(f"invalid release version: {version}")
    if OBJECT_ID_PATTERN.fullmatch(source_commit) is None:
        fail("source commit must be one exact lowercase 40-character object id")
    if OBJECT_ID_PATTERN.fullmatch(source_tree) is None:
        fail("source tree must be one exact lowercase 40-character object id")
    if prerelease_raw not in {"true", "false"}:
        fail("prerelease must be exactly true or false")
    if destination.exists():
        fail(f"refusing to replace existing release intent: {destination}")

    verifier = Path(__file__).resolve().with_name("verify-release-package.py")
    subprocess.run(
        [sys.executable, str(verifier), version, str(package)],
        check=True,
    )

    package_destination = destination / "package" / version
    package_destination.mkdir(parents=True)
    for filename in PACKAGE_FILES:
        source = package / filename
        target = package_destination / filename
        shutil.copyfile(source, target)
        if source.read_bytes() != target.read_bytes():
            fail(f"release intent copy mismatch: {filename}")

    intent = {
        "schema": 1,
        "repository": REPOSITORY,
        "tag": version,
        "version": version,
        "source_commit": source_commit,
        "source_tree": source_tree,
        "prerelease": prerelease_raw == "true",
        "package_sha256": {
            filename: digest(package_destination / filename)
            for filename in PACKAGE_FILES
        },
    }
    (destination / "release-intent.json").write_text(
        json.dumps(intent, indent=2, sort_keys=True) + "\n",
        encoding="ascii",
    )
    print(f"Prepared immutable release intent for {version} at {source_commit}.")


if __name__ == "__main__":
    main()
