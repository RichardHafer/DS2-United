import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from spawner_editor import read_rules, write_rules

A = "00000001-00000002-00000003-00000004"
B = "00000005-00000006-00000007-00000008"


class RulesTests(unittest.TestCase):
    def test_preserve_other_sources_and_reset(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rules.tsv"
            write_rules(path, {A: "block"})
            write_rules(path, {B: "allow"})
            block = [{"mode": "block", "families": [], "difficulty": ""}]
            allow = [{"mode": "allow", "families": [], "difficulty": ""}]
            self.assertEqual(read_rules(path), {A: block, B: allow})
            self.assertEqual(read_rules(path.with_name(path.name + ".previous")), {A: block})
            write_rules(path, {A: "default"})
            self.assertEqual(read_rules(path), {B: allow})

    def test_invalid_existing_file_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rules.tsv"
            for content in ("broken", f"source_guid\tmode\n{A}\tblock\n{A}\tallow\n"):
                path.write_text(content, encoding="ascii")
                with self.assertRaises(ValueError):
                    write_rules(path, {B: "block"})
                self.assertEqual(path.read_text(encoding="ascii"), content)

    def test_invalid_requested_rule_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rules.tsv"
            with self.assertRaises(ValueError):
                write_rules(path, {A: "force"})
            self.assertFalse(path.exists())


if __name__ == "__main__":
    unittest.main()
