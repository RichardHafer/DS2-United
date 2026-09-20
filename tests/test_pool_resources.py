import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from resolve_pool_resources import header_guid


class HeaderTests(unittest.TestCase):
    def header(self):
        data = bytearray(44)
        data[:4], data[8:12] = b"COHS", b"RDHS"
        struct.pack_into("<I", data, 4, 80)
        struct.pack_into("<4I", data, 24, 1, 2, 3, 4)
        struct.pack_into("<I", data, 40, 50)
        return data

    def test_complete_identity(self):
        self.assertEqual(header_guid(self.header(), dict(resource_id=1, size=50)),
                         "00000001-00000002-00000003-00000004")

    def test_remaining_words_matter(self):
        first, second = self.header(), self.header()
        struct.pack_into("<I", second, 36, 5)
        meta = dict(resource_id=1, size=50)
        self.assertNotEqual(header_guid(first, meta), header_guid(second, meta))

    def test_reject_stale_catalog(self):
        with self.assertRaises(ValueError):
            header_guid(self.header(), dict(resource_id=1, size=51))

    def test_reject_truncated(self):
        with self.assertRaises(ValueError):
            header_guid(self.header()[:40], dict(resource_id=1, size=50))


if __name__ == "__main__":
    unittest.main()
