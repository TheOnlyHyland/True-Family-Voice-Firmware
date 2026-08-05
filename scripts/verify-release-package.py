#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path
from typing import NoReturn


BASE = "true-family-voice-esp32s3"
PRODUCT_NAME = "True Tech Solutions.True Family Voice Realtime"
FACTORY = f"{BASE}.factory.bin"
OTA = f"{BASE}.ota.bin"
ELF = f"{BASE}.elf"
MANIFEST = "manifest.json"
SUMS = "SHA256SUMS"
HASHED_FILES = (FACTORY, OTA, ELF, MANIFEST)
PACKAGE_FILES = frozenset((*HASHED_FILES, SUMS))
VERSION_PATTERN = re.compile(
    r"^(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)"
    r"(?:-(?:alpha|beta|rc)(?:\.(?:0|[1-9]\d*))?)?$"
)


def digest(path: Path, algorithm: str) -> str:
    hasher = hashlib.new(algorithm)
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def fail(message: str) -> NoReturn:
    raise SystemExit(message)


def unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate manifest key: {key}")
        result[key] = value
    return result


def verify(version: str, package: Path) -> None:
    if VERSION_PATTERN.fullmatch(version) is None:
        fail(f"invalid release version: {version}")
    if not package.is_dir():
        fail(f"release package directory is missing: {package}")

    entries = list(package.iterdir())
    actual_files = {entry.name for entry in entries}
    if actual_files != PACKAGE_FILES:
        missing = sorted(PACKAGE_FILES - actual_files)
        extra = sorted(actual_files - PACKAGE_FILES)
        fail(f"release package file set mismatch; missing={missing}, extra={extra}")
    if any(entry.is_symlink() or not entry.is_file() for entry in entries):
        fail("release package must contain regular files only")

    expected_sums: dict[str, str] = {}
    lines = (package / SUMS).read_text(encoding="ascii").splitlines()
    if len(lines) != len(HASHED_FILES):
        fail("SHA256SUMS must contain exactly four entries")
    for line, expected_name in zip(lines, HASHED_FILES, strict=True):
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9._-]+)", line)
        if match is None or match.group(2) != expected_name:
            fail(f"non-canonical SHA256SUMS entry: {line}")
        expected_sums[expected_name] = match.group(1)
    for filename in HASHED_FILES:
        if digest(package / filename, "sha256") != expected_sums[filename]:
            fail(f"SHA-256 mismatch: {filename}")

    try:
        manifest = json.loads(
            (package / MANIFEST).read_text(encoding="utf-8"),
            object_pairs_hook=unique_object,
        )
    except (TypeError, ValueError, json.JSONDecodeError) as err:
        fail(f"invalid release manifest: {err}")

    if not isinstance(manifest, dict):
        fail("release manifest root must be an object")
    if set(manifest) != {
        "name",
        "version",
        "home_assistant_domain",
        "new_install_prompt_erase",
        "builds",
    }:
        fail("release manifest root schema is not canonical")
    if manifest.get("name") != PRODUCT_NAME:
        fail("manifest product name is not canonical")
    if manifest.get("version") != version:
        fail("manifest version does not match the release version")
    if manifest.get("home_assistant_domain") != "esphome":
        fail("manifest Home Assistant domain is not canonical")
    if manifest.get("new_install_prompt_erase") is not False:
        fail("manifest erase policy is not canonical")

    builds = manifest.get("builds")
    if not isinstance(builds, list) or len(builds) != 1:
        fail("manifest must contain exactly one build")
    build = builds[0]
    if not isinstance(build, dict) or set(build) != {
        "chipFamily",
        "ota",
        "parts",
    }:
        fail("manifest build schema is not canonical")
    parts = build.get("parts")
    ota = build.get("ota")
    if not isinstance(parts, list) or len(parts) != 1:
        fail("manifest must contain exactly one build and one factory part")
    part = parts[0]
    if not isinstance(part, dict) or set(part) != {
        "path",
        "offset",
        "md5",
        "sha256",
    }:
        fail("manifest factory schema is not canonical")
    if not isinstance(ota, dict) or set(ota) != {"path", "md5", "sha256"}:
        fail("manifest OTA schema is not canonical")
    if build.get("chipFamily") != "ESP32-S3" or part.get("offset") != 0:
        fail("manifest target or factory offset is not canonical")

    for entry, filename in ((part, FACTORY), (ota, OTA)):
        if entry.get("path") != filename:
            fail(f"manifest path mismatch: {filename}")
        if entry.get("sha256") != digest(package / filename, "sha256"):
            fail(f"manifest SHA-256 mismatch: {filename}")
        if entry.get("md5") != digest(package / filename, "md5"):
            fail(f"manifest MD5 mismatch: {filename}")


def main() -> None:
    if len(sys.argv) != 3:
        fail(f"usage: {sys.argv[0]} VERSION PACKAGE_DIRECTORY")
    verify(sys.argv[1], Path(sys.argv[2]))
    print(f"Verified immutable release package {sys.argv[1]}.")


if __name__ == "__main__":
    main()
