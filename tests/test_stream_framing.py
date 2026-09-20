import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from stream_framing import HEADER, BLOCK_SIZE, frame_stream, validate_framing


def record(body):
    return struct.pack('<4sI', b'COHS', len(body) + 8) + body


def payload(data):
    output = bytearray()
    offset = 12
    while offset < len(data):
        tag, size = struct.unpack_from('<4sI', data, offset)
        if tag == b'COHS' and data[offset + 8:offset + 12] == b'TADS':
            output.extend(data[offset + 12:offset + size])
        offset += size
    return bytes(output)


class FramingTests(unittest.TestCase):
    def test_crash_boundary_and_payload_identity(self):
        # Reproduce the failing payload's position modulo the read block.
        prefix = record(b'RDHS' + bytes(0x1F524 - len(HEADER) - 24))
        data = bytes((i % 251 for i in range(11259)))
        output = frame_stream(HEADER + prefix + record(b'TADS' + data))
        validate_framing(output)
        self.assertEqual(payload(output), data)
        self.assertEqual(frame_stream(output), output)

    def test_all_short_tail_cases(self):
        for remainder in range(1, 32):
            data = b'x' * (BLOCK_SIZE - 24 - remainder)
            output = frame_stream(HEADER + record(b'TADS' + data) + record(b'TADS' + b'next'))
            validate_framing(output)
            self.assertEqual(payload(output), data + b'next')

    def test_many_blocks(self):
        data = b'z' * (BLOCK_SIZE * 3 + 19)
        output = frame_stream(HEADER + record(b'TADS' + data))
        validate_framing(output)
        self.assertEqual(payload(output), data)

    def test_rejects_unframed_input(self):
        with self.assertRaises(ValueError):
            validate_framing(HEADER + record(b'TADS' + bytes(BLOCK_SIZE)))


if __name__ == '__main__':
    unittest.main()
