"""Frame loose PC resources for the native 128 KiB reader."""
import struct

BLOCK_SIZE = 131072
HEADER = b'3slo\x0c\x00\x00\x00\x02\x00\x03\x01'


def frame_stream(data):
    if data[:12] != HEADER:
        raise ValueError('Unsupported stream header')
    output = bytearray(HEADER)

    def padding():
        remaining = (-len(output)) % BLOCK_SIZE
        if remaining:
            if remaining < 8:
                raise ValueError('No room for padding header')
            output.extend(struct.pack('<4sI', b'LLIF', remaining))
            output.extend(bytes(remaining - 8))

    offset = 12
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError('Truncated chunk header')
        tag, size = struct.unpack_from('<4sI', data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError('Invalid chunk bounds')
        body = data[offset + 8:offset + size]
        offset += size
        if tag == b'LLIF':
            continue
        if tag != b'COHS' or len(body) < 4:
            raise ValueError('Unsupported chunk')
        if body[:4] == b'TADS':
            position = 4
            while position < len(body):
                remaining = BLOCK_SIZE - len(output) % BLOCK_SIZE
                if remaining <= 12:
                    padding()
                    remaining = BLOCK_SIZE
                count = min(len(body) - position, remaining - 12)
                tail = remaining - 12 - count
                if 0 < tail < 8:
                    if count <= 8 - tail:
                        padding()
                        continue
                    count -= 8 - tail
                output.extend(struct.pack('<4sI4s', b'COHS', count + 12, b'TADS'))
                output.extend(body[position:position + count])
                position += count
        else:
            # Compressed records cannot be split without recompression.
            if size > BLOCK_SIZE - 8:
                raise ValueError('Unsplittable record is too large')
            remaining = BLOCK_SIZE - len(output) % BLOCK_SIZE
            if size > remaining or 0 < remaining - size < 8:
                padding()
            output.extend(struct.pack('<4sI', tag, size))
            output.extend(body)
    padding()
    validate_framing(output)
    return bytes(output)


def validate_framing(data):
    if data[:12] != HEADER or len(data) % BLOCK_SIZE:
        raise ValueError('Invalid loose stream framing')
    offset = 12
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError('Truncated chunk header')
        tag, size = struct.unpack_from('<4sI', data, offset)
        if tag not in (b'COHS', b'LLIF') or size < 8 or offset + size > len(data):
            raise ValueError('Invalid chunk bounds')
        if offset // BLOCK_SIZE != (offset + size - 1) // BLOCK_SIZE:
            raise ValueError(f'Chunk crosses a read block at {offset:#x}')
        offset += size
