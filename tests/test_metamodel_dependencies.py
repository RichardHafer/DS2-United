import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from metamodel_dependencies import dependencies


def fixture():
    data = bytearray(0x88)
    struct.pack_into('<III', data, 0, 0x4D4D646C, 14, len(data))
    struct.pack_into('<I', data, 0x34, 0x68)
    struct.pack_into('<H', data, 0x58, 1)
    struct.pack_into('<7I', data, 0x68, 1, 2, 3, 4, 5, 6, 7)
    return data


class MetaModelTests(unittest.TestCase):
    def test_explicit_asset_record(self):
        row = dependencies(fixture())[0]
        self.assertEqual(row['type_id'], 3)
        self.assertEqual(row['guid'], '00000004-00000005-00000006-00000007')
        self.assertEqual(row['offset'], 0x68)

    def test_version_and_bounds(self):
        for at, value in [(4, 13), (8, 0x1000), (0x34, 0x60), (0x34, 0x69), (0x34, 0x80)]:
            data = fixture()
            struct.pack_into('<I', data, at, value)
            with self.assertRaises(ValueError):
                dependencies(data)

    def test_no_assets_does_not_scan_header_guid(self):
        data = fixture()
        struct.pack_into('<H', data, 0x58, 0)
        self.assertEqual(dependencies(data), [])


if __name__ == '__main__':
    unittest.main()
