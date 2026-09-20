import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from audit_native_package_coverage import classify


class PackageCoverage(unittest.TestCase):
    def test_primary_wins_over_duplicate(self):
        result = classify("g", "own", {"g": [{"archive": "other"}, {"archive": "own"}]},
                          {}, {"own": "native/own", "other": "native/other"})
        self.assertEqual(result["status"], "primary_character_package")

    def test_model_stream_named_group_counts(self):
        result = classify("g", "own", {}, {"g": [{"archive": "own"}]}, {"own": "own"})
        self.assertEqual(result["status"], "primary_character_package")

    def test_foreign_level_is_not_a_character_request(self):
        result = classify("g", "own", {"g": [{"archive": "chapter"}]}, {}, {"own": "own"})
        self.assertEqual(result["status"], "external_archive")
        self.assertEqual(result["verified_character_candidates"], [])

    def test_missing_reference_stays_unresolved(self):
        self.assertEqual(classify("g", "own", {}, {}, {})["status"], "unresolved")


if __name__ == "__main__":
    unittest.main()
