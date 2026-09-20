import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from package_research_pool import package, MAX_CHUNK, pad_stream


def header():
    data = bytearray(36)
    data[:4] = b'RDHS'
    struct.pack_into('<I', data, 12, 0xCCB51828)
    return data


class StreamTests(unittest.TestCase):
    def test_chunk_boundaries_and_alignment(self):
        for length in (1, MAX_CHUNK - 12, MAX_CHUNK, MAX_CHUNK * 2):
            payload = b'x' * length
            data = package([(header(), 'name', 'path', payload)])
            self.assertEqual(len(data) % MAX_CHUNK, 0)
            position, restored = 12, bytearray()
            while position < len(data):
                size = struct.unpack_from('<I', data, position + 4)[0]
                self.assertTrue(8 <= size <= MAX_CHUNK)
                body = data[position + 8:position + size]
                self.assertEqual(len(body), size - 8)
                if body[:4] == b'TADS':
                    restored.extend(body[4:])
                position += size
            self.assertEqual(position, len(data))
            self.assertEqual(restored, payload)

    def test_rejects_duplicate_identity(self):
        entry = (header(), 'name', 'path', b'x')
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            package([entry, entry])

    def test_padding_handles_short_header_remainders(self):
        for length in (720896, MAX_CHUNK, MAX_CHUNK - 8, *range(MAX_CHUNK - 7, MAX_CHUNK)):
            data = bytearray(length)
            pad_stream(data)
            self.assertEqual(len(data) % MAX_CHUNK, 0)
            position = length
            while position < len(data):
                tag, size = struct.unpack_from('<4sI', data, position)
                self.assertEqual(tag, b'LLIF')
                self.assertTrue(8 <= size <= MAX_CHUNK)
                position += size
            self.assertEqual(position, len(data))

    def test_rejects_unhandled_resource_identity(self):
        value = header()
        value[16] = 1
        with self.assertRaisesRegex(ValueError, 'resource GUID'):
            package([(value, 'name', 'path', b'x')])


if __name__ == '__main__':
    unittest.main()
