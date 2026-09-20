import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from resolve_model_streams import guid_name


class ModelStreamTests(unittest.TestCase):
    def test_brute_guid(self):
        self.assertEqual(guid_name("{36C83B5F-11A2-EB65-5752-454445323034}"),
                         "36C83B5F-EB6511A2-44455257-34303245")

    def test_reject_names(self):
        self.assertIsNone(guid_name("brute.simgroup.bin"))
        self.assertIsNone(guid_name("{not-a-guid}"))


if __name__ == "__main__":
    unittest.main()
