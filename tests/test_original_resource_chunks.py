import io
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from package_direct_resources import original_payload_chunks


def chunk(body):
    return b'COHS' + struct.pack('<I', len(body) + 8) + body


class OriginalResourceTests(unittest.TestCase):
    def test_preserves_compressed_encoding(self):
        raw = chunk(b'kapR' + struct.pack('<I', 3) + b'abc')
        encoded, decoded = original_payload_chunks(io.BytesIO(raw), 5, lambda data: b'12345')
        self.assertEqual(encoded, raw)
        self.assertEqual(decoded, b'12345')

    def test_mixed_chunks(self):
        raw = chunk(b'TADS12') + chunk(b'kapR' + struct.pack('<I', 1) + b'x')
        encoded, decoded = original_payload_chunks(io.BytesIO(raw), 4, lambda data: b'34')
        self.assertEqual(encoded, raw)
        self.assertEqual(decoded, b'1234')

    def test_bad_lengths(self):
        for raw, size in [(chunk(b'TADS123'), 2), (chunk(b'TADS1')[:-1], 1),
                          (chunk(b'kapR' + struct.pack('<I', 50) + b'x'), 1)]:
            with self.assertRaises(ValueError):
                original_payload_chunks(io.BytesIO(raw), size, lambda data: b'1')

    def test_preserves_zero_alignment_padding(self):
        raw = chunk(b'TADS12\0\0')
        encoded, decoded = original_payload_chunks(io.BytesIO(raw), 2, lambda data: data)
        self.assertEqual(encoded, raw)
        self.assertEqual(decoded, b'12')
        with self.assertRaises(ValueError):
            original_payload_chunks(io.BytesIO(chunk(b'TADS12\0x')), 2, lambda data: data)
