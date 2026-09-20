import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from scan_resource_links import candidates


class LinkTests(unittest.TestCase):
    def test_full_identity_and_offset(self):
        data = bytes(4) + struct.pack('<4I', 42, 2, 3, 4)
        self.assertEqual(candidates(data, {42}), [(4, '0000002A-00000002-00000003-00000004')])

    def test_short_unaligned_or_unknown_not_selected(self):
        data = struct.pack('<4I', 42, 2, 3, 4)
        self.assertEqual(candidates(data[:-1], {42}), [])
        self.assertEqual(candidates(b'x' + data, {42}), [])
        self.assertEqual(candidates(data, {11}), [])


if __name__ == '__main__':
    unittest.main()
