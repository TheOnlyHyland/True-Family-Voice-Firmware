import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMPARE = ROOT / "scripts" / "compare-yaml.py"


def find_yaml_python() -> str | None:
    candidates = (Path(sys.executable), ROOT / ".venv" / "bin" / "python3")
    for candidate in candidates:
        if not candidate.is_file():
            continue
        available = subprocess.run(
            [str(candidate), "-c", "import yaml"],
            check=False,
            capture_output=True,
        )
        if available.returncode == 0:
            return str(candidate)
    return None


YAML_PYTHON = find_yaml_python()


class CompareYamlTest(unittest.TestCase):
    def compare(self, left: str, right: str) -> subprocess.CompletedProcess[str]:
        python = YAML_PYTHON
        if python is None:
            self.skipTest("PyYAML is installed with ESPHome")
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            left_path = root / "left.yaml"
            right_path = root / "right.yaml"
            left_path.write_text(left, encoding="utf-8")
            right_path.write_text(right, encoding="utf-8")
            return subprocess.run(
                [python, str(COMPARE), str(left_path), str(right_path)],
                check=False,
                capture_output=True,
                text=True,
            )

    def test_mapping_order_is_not_semantic(self) -> None:
        result = self.compare(
            "root:\n  tagged: !lambda return true;\n  value: 1\n",
            "root:\n  value: 1\n  tagged: !lambda return true;\n",
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_tags_values_and_sequence_order_remain_semantic(self) -> None:
        left = (
            "root:\n"
            "  tagged: !lambda return true;\n"
            "  value: 1\n"
            "  sequence:\n"
            "    - 1\n"
            "    - 2\n"
        )
        for right in (
            left.replace("!lambda", "!secret"),
            left.replace("return true", "return false"),
            left.replace("value: 1", "value: 2"),
            left.replace("    - 1\n    - 2", "    - 2\n    - 1"),
        ):
            result = self.compare(left, right)
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
