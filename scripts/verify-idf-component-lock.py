#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT / ".esphome" / "build" / "true-family-voice" / "dependencies.lock"
LOCK = ROOT / "idf-component-manager.lock"


def main() -> None:
    if not GENERATED.is_file() or GENERATED.is_symlink():
        raise SystemExit(f"ESP-IDF component-manager lock is missing: {GENERATED}")

    values: dict[str, str] = {}
    for raw_line in LOCK.read_text(encoding="ascii").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise SystemExit(f"invalid component lock line: {raw_line}")
        key, value = line.split("=", 1)
        if key in values:
            raise SystemExit(f"duplicate component lock key: {key}")
        values[key] = value
    if set(values) != {
        "schema",
        "target",
        "idf",
        "normalized_sha256",
        "improv_version",
        "improv_file_count",
        "improv_tree_sha256",
    }:
        raise SystemExit("component lock schema is not canonical")
    if values["schema"] != "1" or values["target"] != "esp32s3":
        raise SystemExit("component lock target is not canonical")
    if values["idf"] != "5.5.5":
        raise SystemExit("component lock ESP-IDF version is not canonical")
    if values["improv_version"] != "1.2.4" or values["improv_file_count"] != "12":
        raise SystemExit("component lock Improv version or file count is not canonical")
    if re.fullmatch(r"[0-9a-f]{64}", values["normalized_sha256"]) is None:
        raise SystemExit("component lock SHA-256 is invalid")
    if re.fullmatch(r"[0-9a-f]{64}", values["improv_tree_sha256"]) is None:
        raise SystemExit("component lock Improv tree SHA-256 is invalid")

    content = GENERATED.read_text(encoding="utf-8")
    content = re.sub(
        r"registry_url: https://components\.espressif\.com/?",
        "registry_url: https://components.espressif.com",
        content,
    )
    content, path_count = re.subn(
        r"(?m)^      path: .*/\.esphome/pio_components/idf/"
        r"[0-9a-f]+/improv/Improv$",
        "      path: <ESPHome bundled Improv>",
        content,
    )
    content, manifest_count = re.subn(
        r"(?m)^manifest_hash: [0-9a-f]{64}$",
        "manifest_hash: <generated>",
        content,
    )
    if path_count != 1 or manifest_count != 1:
        raise SystemExit("component lock normalization shape changed")
    actual = hashlib.sha256(content.encode("utf-8")).hexdigest()
    if actual != values["normalized_sha256"]:
        raise SystemExit(
            "ESP-IDF component-manager closure differs from "
            "idf-component-manager.lock"
        )
    if "    version: 5.5.5\n" not in content:
        raise SystemExit("generated component lock does not contain ESP-IDF 5.5.5")

    improv_roots = list(
        (ROOT / ".esphome" / "pio_components" / "idf").glob(
            "*/improv/Improv"
        )
    )
    if len(improv_roots) != 1 or not improv_roots[0].is_dir():
        raise SystemExit("expected one resolved Improv component tree")
    improv_root = improv_roots[0]
    if improv_root.is_symlink() or any(
        path.is_symlink() for path in improv_root.rglob("*")
    ):
        raise SystemExit("resolved Improv component tree contains a symlink")
    improv_files = sorted(
        path
        for path in improv_root.rglob("*")
        if path.is_file() and path.name != ".esphome_extracted"
    )
    if len(improv_files) != int(values["improv_file_count"]):
        raise SystemExit("resolved Improv component file set changed")
    improv_hasher = hashlib.sha256()
    for path in improv_files:
        relative = path.relative_to(improv_root).as_posix().encode("utf-8")
        improv_hasher.update(relative)
        improv_hasher.update(b"\0")
        improv_hasher.update(path.read_bytes())
        improv_hasher.update(b"\0")
    if improv_hasher.hexdigest() != values["improv_tree_sha256"]:
        raise SystemExit("resolved Improv component tree differs from the lock")
    print("Verified exact normalized ESP-IDF component-manager closure.")


if __name__ == "__main__":
    main()
