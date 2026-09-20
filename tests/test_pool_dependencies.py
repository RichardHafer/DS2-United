import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from resolve_pool_dependencies import choose, closure


def node(guid, refs=(), archive="a", signature="s"):
    return dict(guid=guid, archive=archive, asset="asset", index=0, signature=signature,
                **{"class": "Enemy"}, references=[dict(guid=r, component="c", field="f") for r in refs])


class DependencyTests(unittest.TestCase):
    def test_cycle(self):
        result = closure(dict(guid="a", source_archive="a", package="test"),
                         {"a": [node("a", ["b"])], "b": [node("b", ["a"])]})
        self.assertEqual(len(result["nodes"]), 2)
        self.assertFalse(result["truncated"])

    def test_ambiguity(self):
        self.assertEqual(choose([node("a"), node("a", signature="other")], "a")[1], "ambiguous_entity")

    def test_local_precedence(self):
        selected, status = choose([node("a"), node("a", archive="b", signature="other")], "b")
        self.assertEqual(status, "entity")
        self.assertEqual(selected["archive"], "b")

    def test_missing_and_bound(self):
        root = dict(guid="a", source_archive="a", package="test")
        index = {"a": [node("a", ["missing", "b"])], "b": [node("b")]}
        result = closure(root, index, limit=1)
        self.assertTrue(result["truncated"])
        self.assertEqual(result["unresolved"], 1)


if __name__ == "__main__":
    unittest.main()
