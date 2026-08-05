#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import importlib.util
import json
import re
import sys
from pathlib import Path
from types import ModuleType
from typing import NoReturn


REPOSITORY = "TheOnlyHyland/True-Family-Voice-Firmware"
PACKAGE_FILES = frozenset(
    {
        "true-family-voice-esp32s3.factory.bin",
        "true-family-voice-esp32s3.ota.bin",
        "true-family-voice-esp32s3.elf",
        "SHA256SUMS",
        "manifest.json",
    }
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


def unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate release-intent key: {key}")
        result[key] = value
    return result


def package_verifier() -> ModuleType:
    path = Path(__file__).resolve().with_name("verify-release-package.py")
    spec = importlib.util.spec_from_file_location("verify_release_package", path)
    if spec is None or spec.loader is None:
        fail("unable to load release package verifier")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def verify(
    version: str,
    source_commit: str,
    source_tree: str,
    prerelease_raw: str,
    root: Path,
) -> None:
    if VERSION_PATTERN.fullmatch(version) is None:
        fail(f"invalid release version: {version}")
    if OBJECT_ID_PATTERN.fullmatch(source_commit) is None:
        fail("invalid expected source commit")
    if OBJECT_ID_PATTERN.fullmatch(source_tree) is None:
        fail("invalid expected source tree")
    if prerelease_raw not in {"true", "false"}:
        fail("expected prerelease must be exactly true or false")
    if not root.is_dir() or root.is_symlink():
        fail("release-intent root must be one regular directory")

    root_entries = list(root.iterdir())
    if {entry.name for entry in root_entries} != {"release-intent.json", "package"}:
        fail("release-intent root file set is not canonical")
    if any(entry.is_symlink() for entry in root_entries):
        fail("release-intent root must not contain symlinks")

    intent_path = root / "release-intent.json"
    package_root = root / "package"
    if not intent_path.is_file() or not package_root.is_dir():
        fail("release-intent layout is not canonical")
    package_versions = list(package_root.iterdir())
    if (
        len(package_versions) != 1
        or package_versions[0].name != version
        or not package_versions[0].is_dir()
        or package_versions[0].is_symlink()
    ):
        fail("release-intent package version directory is not canonical")
    package = package_versions[0]

    try:
        intent = json.loads(
            intent_path.read_text(encoding="ascii"),
            object_pairs_hook=unique_object,
        )
    except (TypeError, ValueError, UnicodeError, json.JSONDecodeError) as err:
        fail(f"invalid release intent: {err}")
    if not isinstance(intent, dict) or set(intent) != {
        "schema",
        "repository",
        "tag",
        "version",
        "source_commit",
        "source_tree",
        "prerelease",
        "package_sha256",
    }:
        fail("release-intent schema is not canonical")
    expected_prerelease = prerelease_raw == "true"
    expected_scalars = {
        "schema": 1,
        "repository": REPOSITORY,
        "tag": version,
        "version": version,
        "source_commit": source_commit,
        "source_tree": source_tree,
        "prerelease": expected_prerelease,
    }
    for key, expected in expected_scalars.items():
        if intent.get(key) != expected:
            fail(f"release-intent {key} mismatch")

    hashes = intent.get("package_sha256")
    if not isinstance(hashes, dict) or set(hashes) != PACKAGE_FILES:
        fail("release-intent package hash set is not canonical")
    for filename in PACKAGE_FILES:
        expected = hashes.get(filename)
        if not isinstance(expected, str) or re.fullmatch(r"[0-9a-f]{64}", expected) is None:
            fail(f"release-intent SHA-256 is invalid: {filename}")
        if digest(package / filename) != expected:
            fail(f"release-intent SHA-256 mismatch: {filename}")

    package_verifier().verify(version, package)


def main() -> None:
    if len(sys.argv) != 6:
        fail(
            f"usage: {sys.argv[0]} VERSION SOURCE_COMMIT SOURCE_TREE "
            "PRERELEASE INTENT_DIRECTORY"
        )
    verify(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], Path(sys.argv[5]))
    print(f"Verified immutable source and package intent for {sys.argv[1]}.")


if __name__ == "__main__":
    main()
