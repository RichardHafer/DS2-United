import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from build_research_toc import build
from read_stock_toc import parse


class ResearchToc(unittest.TestCase):
    def test_cross_toc_references_preserved(self):
        data = build("pool", 12345, [(99, "global")], [(17, "enemy")])
        root = parse(data)["entries"][0]
        self.assertEqual(root["identity"]["hash"], "00000000")
        self.assertEqual(root["leading_references"][0]["hash"], "00000063")
        self.assertEqual(root["dependencies"][0]["hash"], "00000011")
        self.assertEqual(root["content_size"], 12345)
        self.assertEqual(len(data) % 2048, 0)

    def test_duplicate_rejected(self):
        with self.assertRaises(ValueError):
            build("pool", 1, [(1, "enemy")], [(1, "enemy")])

    def test_reference_limit(self):
        with self.assertRaises(ValueError):
            build("pool", 1, [], [(i, str(i)) for i in range(256)])

    def test_embedded_nul_rejected(self):
        with self.assertRaises(ValueError):
            build("bad\0name", 1, [], [])


if __name__ == "__main__":
    unittest.main()
