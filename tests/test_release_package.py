import hashlib
import json
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VERIFY = ROOT / "scripts" / "verify-release-package.py"
MAKE_INTENT = ROOT / "scripts" / "make-release-intent.py"
VERIFY_INTENT = ROOT / "scripts" / "verify-release-intent.py"
BASE = "true-family-voice-esp32s3"
SOURCE_COMMIT = "a" * 40
SOURCE_TREE = "b" * 40


class ReleasePackageTest(unittest.TestCase):
    def refresh_sums(self, package: Path) -> None:
        names = (
            f"{BASE}.factory.bin",
            f"{BASE}.ota.bin",
            f"{BASE}.elf",
            "manifest.json",
        )
        sums = "".join(
            f"{hashlib.sha256((package / name).read_bytes()).hexdigest()}  {name}\n"
            for name in names
        )
        (package / "SHA256SUMS").write_text(sums, encoding="ascii")

    def make_package(self, root: Path, version: str = "0.19.0") -> Path:
        package = root / version
        package.mkdir()
        factory = package / f"{BASE}.factory.bin"
        ota = package / f"{BASE}.ota.bin"
        elf = package / f"{BASE}.elf"
        factory.write_bytes(b"factory-bytes")
        ota.write_bytes(b"ota-bytes")
        elf.write_bytes(b"elf-bytes")

        def file_hash(path: Path, algorithm: str) -> str:
            return hashlib.new(algorithm, path.read_bytes()).hexdigest()

        manifest = {
            "name": "True Tech Solutions.True Family Voice Realtime",
            "version": version,
            "home_assistant_domain": "esphome",
            "new_install_prompt_erase": False,
            "builds": [
                {
                    "chipFamily": "ESP32-S3",
                    "ota": {
                        "path": ota.name,
                        "md5": file_hash(ota, "md5"),
                        "sha256": file_hash(ota, "sha256"),
                    },
                    "parts": [
                        {
                            "path": factory.name,
                            "offset": 0,
                            "md5": file_hash(factory, "md5"),
                            "sha256": file_hash(factory, "sha256"),
                        }
                    ],
                }
            ],
        }
        manifest_path = package / "manifest.json"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        self.refresh_sums(package)
        return package

    def verify(self, version: str, package: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["python3", str(VERIFY), version, str(package)],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )

    def make_intent(
        self, package: Path, destination: Path
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                "python3",
                str(MAKE_INTENT),
                "0.19.0",
                SOURCE_COMMIT,
                SOURCE_TREE,
                "true",
                str(package),
                str(destination),
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )

    def verify_intent(self, destination: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                "python3",
                str(VERIFY_INTENT),
                "0.19.0",
                SOURCE_COMMIT,
                SOURCE_TREE,
                "true",
                str(destination),
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )

    def test_accepts_exact_package_and_rejects_byte_change(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            package = self.make_package(Path(temp))
            accepted = self.verify("0.19.0", package)
            self.assertEqual(accepted.returncode, 0, accepted.stderr)

            (package / f"{BASE}.ota.bin").write_bytes(b"different")
            rejected = self.verify("0.19.0", package)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("SHA-256 mismatch", rejected.stderr)

    def test_rejects_extra_asset_and_wrong_version(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            package = self.make_package(Path(temp))
            (package / "unexpected.bin").write_bytes(b"unexpected")
            extra = self.verify("0.19.0", package)
            self.assertNotEqual(extra.returncode, 0)
            self.assertIn("file set mismatch", extra.stderr)

            (package / "unexpected.bin").unlink()
            wrong_version = self.verify("0.19.1", package)
            self.assertNotEqual(wrong_version.returncode, 0)
            self.assertIn("manifest version", wrong_version.stderr)

    def test_rejects_wrong_product_name_target_and_extra_schema(self) -> None:
        for mutation, expected_error in (
            (lambda manifest: manifest.update(name="Wrong"), "product name"),
            (
                lambda manifest: manifest["builds"][0].update(
                    chipFamily="ESP32"
                ),
                "target",
            ),
            (lambda manifest: manifest.update(extra=True), "root schema"),
        ):
            with self.subTest(expected_error=expected_error):
                with tempfile.TemporaryDirectory() as temp:
                    package = self.make_package(Path(temp))
                    manifest_path = package / "manifest.json"
                    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                    mutation(manifest)
                    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
                    self.refresh_sums(package)
                    rejected = self.verify("0.19.0", package)
                    self.assertNotEqual(rejected.returncode, 0)
                    self.assertIn(expected_error, rejected.stderr)

    def test_release_intent_binds_source_version_and_all_package_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            package = self.make_package(root)
            destination = root / "prepared-release"
            made = self.make_intent(package, destination)
            self.assertEqual(made.returncode, 0, made.stderr)
            verified = self.verify_intent(destination)
            self.assertEqual(verified.returncode, 0, verified.stderr)

            copied_ota = destination / "package" / "0.19.0" / f"{BASE}.ota.bin"
            copied_ota.write_bytes(b"changed after preparation")
            rejected = self.verify_intent(destination)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("release-intent SHA-256 mismatch", rejected.stderr)

    def test_release_intent_rejects_source_mismatch_and_extra_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            package = self.make_package(root)
            destination = root / "prepared-release"
            made = self.make_intent(package, destination)
            self.assertEqual(made.returncode, 0, made.stderr)

            wrong_source = subprocess.run(
                [
                    "python3",
                    str(VERIFY_INTENT),
                    "0.19.0",
                    "c" * 40,
                    SOURCE_TREE,
                    "true",
                    str(destination),
                ],
                cwd=ROOT,
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(wrong_source.returncode, 0)
            self.assertIn("source_commit mismatch", wrong_source.stderr)

            (destination / "unexpected").write_text("unexpected", encoding="ascii")
            extra = self.verify_intent(destination)
            self.assertNotEqual(extra.returncode, 0)
            self.assertIn("root file set", extra.stderr)


if __name__ == "__main__":
    unittest.main()
