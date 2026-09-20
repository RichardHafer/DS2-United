import sys
import struct
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from combine_runtime_stream import records
from package_research_pool import chunk


class CombinedStreamTests(unittest.TestCase):
    header = b'3slo\x0c\x00\x00\x00\x02\x00\x03\x01'

    def test_preserves_records_removes_terminal_padding(self):
        content = chunk(b'payload')
        self.assertEqual(records(self.header + content + chunk(bytes(8), b'LLIF')), content)

    def test_rejects_bad_boundaries(self):
        for content in (b'CO', b'COHS' + struct.pack('<I', 7),
                        b'COHS' + struct.pack('<I', 100), chunk(b'', b'NOPE')):
            with self.subTest(content=content), self.assertRaises(ValueError):
                records(self.header + content)

    def test_strips_interblock_padding(self):
        content = chunk(b'first') + chunk(b'', b'LLIF') + chunk(b'second')
        self.assertEqual(records(self.header + content), chunk(b'first') + chunk(b'second'))

    def test_rejects_wrong_platform(self):
        with self.assertRaises(ValueError):
            records(b'x' * 12)


if __name__ == '__main__':
    unittest.main()
