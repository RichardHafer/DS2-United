"""PC .str stream helpers shared by the frontend builders (read and write, no game access).

A stream is the 12 byte '3slo' header and then records of (tag, size) that never cross
a 128 KiB read block: COHS records carry an asset header (RDHS) or its payload pieces
(kapR = RefPack, TADS = raw), LLIF records fill to the next block.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ds2_paths import MODDING_TOOLS  # noqa: E402
sys.path.insert(0, str(MODDING_TOOLS))
import ds2str  # noqa: E402

BLOCK = 0x20000
HEADER = b'3slo\x0c\x00\x00\x00\x02\x00\x03\x01'
RAW_PIECE = 129024
FRONTEND = 'frontend\\frontend.str'


def sdbm(name):
    """Archive name hash (BIGH tables): lower case, forward slashes as backslashes."""
    h = 0
    for c in name.lower().replace('/', '\\').encode():
        h = (c + (h << 6) + (h << 16) - h) & 0xFFFFFFFF
    return h


def hash_string(text):
    """EARS::Common::HashString_SDBM (0x007423C0): ASCII lower case, h * 0x1003F + c."""
    h = 0
    for c in text.encode('ascii'):
        if 0x41 <= c <= 0x5A:
            c += 0x20
        h = (h * 0x1003F + c) & 0xFFFFFFFF
    return h


def big_entry(dat, name):
    """One file out of a BIGH archive (big endian table of offset, size, hash)."""
    want = sdbm(name)
    with open(dat, 'rb') as f:
        head = f.read(16)
        if head[:4] != b'BIGH':
            raise SystemExit('%s is not a BIGH archive' % dat)
        _, count, _ = struct.unpack('>III', head[4:16])
        table = f.read(count * 12)
        for i in range(count):
            offset, size, h = struct.unpack_from('>III', table, i * 12)
            if h == want:
                f.seek(offset)
                data = f.read(size)
                if len(data) != size:
                    raise SystemExit('%s: short read for %s' % (dat, name))
                return data
    raise SystemExit('%s holds no %s (hash 0x%08X)' % (dat, name, want))


def records(data):
    off = 12
    out = []
    while off + 8 <= len(data):
        tag, size = struct.unpack_from('<4sI', data, off)
        if size < 8 or off + size > len(data):
            raise ValueError('bad record at %#x' % off)
        out.append((tag, data[off + 8:off + size]))
        off += size
    return out


def names(shdr):
    parts = shdr[36:].split(b'\x00')
    return parts[0].decode('latin1'), parts[2].decode('latin1')


def declared(shdr):
    return struct.unpack_from('<I', shdr, 32)[0]


def with_declared(shdr, size):
    out = bytearray(shdr)
    struct.pack_into('<I', out, 32, size)
    return bytes(out)


def group(recs):
    """[(shdr, [payload records])] in file order; fill records dropped."""
    assets, cur = [], None
    for tag, body in recs:
        if tag == b'LLIF':
            continue
        if tag != b'COHS':
            raise ValueError('unexpected tag %r' % tag)
        if body[:4] == b'RDHS':
            cur = [body, []]
            assets.append(cur)
        elif cur is None:
            raise ValueError('payload before the first header')
        else:
            cur[1].append(body)
    return assets


def payload(bodies, size):
    out = bytearray()
    for b in bodies:
        if b[:4] == b'kapR':
            n = struct.unpack_from('<I', b, 4)[0]
            out += ds2str.refpack_decompress(b[8:8 + n])
        elif b[:4] == b'TADS':
            out += b[4:]
        else:
            raise ValueError('unknown payload kind %r' % b[:4])
    return bytes(out[:size])


def find_assets(data, wanted):
    """{(base name, kind): payload} for the (base, kind) pairs in wanted; first one wins."""
    out = {}
    for shdr, bodies in group(records(data)):
        key = names(shdr)
        if key in wanted and key not in out:
            out[key] = payload(bodies, declared(shdr))
    return out


def rpak(data):
    out = []
    for i in range(0, len(data), RAW_PIECE):
        stream = ds2str.refpack_compress(data[i:i + RAW_PIECE])
        out.append(b'kapR' + struct.pack('<I', len(stream)) + stream)
    return out


def frame(assets):
    """No record crosses a 128 KiB read block, no 1..7 byte gaps, LLIF fill, ends on a block.

    tools/stream_framing.py splits large TADS records, but stock carries records of
    up to a full block, which it rejects, so this keeps every record as it is."""
    out = bytearray(HEADER)

    def fill():
        rem = (-len(out)) % BLOCK
        if rem:
            if rem < 8:
                raise ValueError('no room for a fill header')
            out.extend(struct.pack('<4sI', b'LLIF', rem))
            out.extend(bytes(rem - 8))

    def put(body):
        size = 8 + len(body)
        if size > BLOCK:
            raise ValueError('record of %d bytes cannot fit a block' % size)
        rem = BLOCK - len(out) % BLOCK
        if size > rem or 0 < rem - size < 8:
            fill()
        out.extend(struct.pack('<4sI', b'COHS', size))
        out.extend(body)

    for shdr, bodies in assets:
        put(shdr)
        for b in bodies:
            put(b)
    fill()
    return bytes(out)


def validate(data):
    if data[:12] != HEADER or len(data) % BLOCK:
        raise ValueError('header or length')
    off, n = 12, 0
    while off < len(data):
        tag, size = struct.unpack_from('<4sI', data, off)
        if tag not in (b'COHS', b'LLIF') or size < 8:
            raise ValueError('bad record at %#x' % off)
        if off // BLOCK != (off + size - 1) // BLOCK:
            raise ValueError('record crosses a block at %#x' % off)
        off += size
        n += 1
    return n
