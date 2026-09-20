import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from read_stock_toc import parse


def fixture():
    data = bytearray(112)
    struct.pack_into("<IIHHHH", data, 0, 0x9CBA7B28, 10, 1, 3, 2, 0)
    struct.pack_into("<II4BII", data, 16, 36, 0, 0, 1, 1, 0, 0, 60)
    for i, (value, offset) in enumerate(((1, 72), (2, 80), (3, 88))):
        struct.pack_into("<II", data, 36 + i * 8, value, offset)
    struct.pack_into("<II", data, 60, 44, 52)
    data[72:77] = b"root\0"
    data[80:87] = b"global\0"
    data[88:94] = b"enemy\0"
    return data


class StockToc(unittest.TestCase):
    def test_reference_ranges(self):
        entry = parse(fixture())["entries"][0]
        self.assertEqual(entry["identity"]["name"], "root")
        self.assertEqual(entry["leading_references"][0]["name"], "global")
        self.assertEqual(entry["dependencies"][0]["name"], "enemy")

    def test_reference_outside_file(self):
        data = fixture()
        struct.pack_into("<I", data, 64, 0xFFFFFFFC)
        with self.assertRaises(ValueError):
            parse(data)

    def test_short_descriptor_table(self):
        with self.assertRaises(ValueError):
            parse(fixture()[:30])

    def test_wrong_format(self):
        data = fixture()
        data[0] = 0
        with self.assertRaises(ValueError):
            parse(data)

    def test_in_file_but_outside_identity_table(self):
        data = fixture()
        struct.pack_into("<I", data, 16, 40)
        with self.assertRaises(ValueError):
            parse(data)


if __name__ == "__main__":
    unittest.main()
