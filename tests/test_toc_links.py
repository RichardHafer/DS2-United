import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from verify_research_toc_links import key, resolve


def entry(name, value=1):
    return {"toc": "stock", "entry": {"identity": {"hash": f"{value:08X}", "name": name},
            "content_size": 10, "flags": 0, "leading_count": 0,
            "leading_references": [], "dependencies": []}}


class TocLinks(unittest.TestCase):
    def test_hash_collision_is_not_a_name_match(self):
        wanted = entry("wanted")["entry"]["identity"]
        self.assertEqual(resolve(wanted, {key(wanted): [entry("other")]})[0], "identity_collision")

    def test_zero_uses_name(self):
        self.assertNotEqual(key(entry("a", 0)["entry"]["identity"]),
                            key(entry("b", 0)["entry"]["identity"]))

    def test_duplicate_identical_descriptors_are_resolved(self):
        candidate = entry("wanted")
        wanted = candidate["entry"]["identity"]
        self.assertEqual(resolve(wanted, {key(wanted): [candidate, candidate]})[0], "resolved")

    def test_missing_is_not_ready(self):
        self.assertEqual(resolve(entry("missing")["entry"]["identity"], {})[0], "missing")


if __name__ == "__main__":
    unittest.main()
