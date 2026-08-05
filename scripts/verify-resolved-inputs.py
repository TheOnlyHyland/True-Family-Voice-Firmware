from __future__ import annotations

import hashlib
from pathlib import Path
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[1]


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cache_key(url: str) -> str:
    return hashlib.sha256(url.encode()).hexdigest()[:8]


def resolved_path(url: str) -> Path | None:
    filename = Path(urlparse(url).path).name
    if "/sounds/" in url:
        return ROOT / ".esphome" / "audio_file" / cache_key(url)
    if filename.endswith(".json") and (
        "micro-wake-word" in url or "micro-wake-word-models" in url
    ):
        return (
            ROOT
            / ".esphome"
            / "micro_wake_word"
            / cache_key(url)
            / "manifest.json"
        )
    if filename.endswith(".tflite"):
        manifest_url = url.removesuffix(".tflite") + ".json"
        return (
            ROOT
            / ".esphome"
            / "micro_wake_word"
            / cache_key(manifest_url)
            / filename
        )
    # The XMOS image is fetched and MD5-verified by voice_kit at runtime.
    return None


def main() -> None:
    checked = 0
    for raw_line in (ROOT / "external-inputs.lock").read_text().splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        expected, url = line.split(maxsplit=1)
        path = resolved_path(url)
        if path is None:
            continue
        if not path.is_file():
            raise SystemExit(f"resolved input is missing: {path}")
        if digest(path) != expected:
            raise SystemExit(f"resolved input checksum mismatch: {url}")
        checked += 1
    if checked != 23:
        raise SystemExit(f"expected 23 compile-time external inputs, checked {checked}")
    print("Verified exact external bytes consumed by ESPHome.")


if __name__ == "__main__":
    main()
