"""Read and extend the EA Apt movies (.apx) of frontend.str.

Container: an 'apxf' chunk holding titl, apti (the movie), cons (constants), rubi
(geometry), vers, tx4h (texture page table) and one tx4t per page. Every chunk's data
but vers (which follows rubi directly) starts on a 16 byte boundary; 'alig' chunks
(at least 8 bytes) fill the gaps.

apti: offsets are relative to the apti data. Characters are {u32 type, u32 0x09876543, ...}:
Shape {bounds 4f, u32 geometry id} (28 bytes), Sprite {u32 frames, u32 frames ptr, u32 0}.
A frame is {u32 items, u32 ptr to item pointers}; items: 2 FrameLabel {name, 0x70000,
frame index}, 3 PlaceObject (64 bytes: flags, depth, character, matrix, ...), 4 Remove {depth}.
The movie character's offset is in cons ('Apt constant file' + 20); its character table
is {count at +20, pointer at +24}.

rubi: records {u32 id, u32 size incl. header, sub-records}; a sub-record is {u32 size, colour
4f, UV matrix 8f (u = m0 x + m1 y + m2, v = m4 x + m5 y + m6), u32 page (0 = flat, n = tx4h
page n), u32, u32 vertex count, vertices}.

tx4h: {u32 set name, u16 4, u16 pages, u32 page ids, u32 page table, u32 0x27FF46B0}, page id =
CRC-32(set name + page name), page entry {name, size, u16 w, u16 h, 0, 0, mips,
format 3 = DXT5, format block, sampler block}. tx4t carries the raw DXT5 data of one page.
"""
import io
import struct
import zlib

from PIL import Image

SIG = 0x09876543
TYPES = {1: 'Shape', 2: 'EditText', 3: 'Font', 4: 'Button', 5: 'Sprite', 6: 'Sound', 7: 'Image',
         8: 'Morph', 9: 'Movie', 10: 'StaticText', 11: 'None', 12: 'Video'}
LABEL_FLAGS = 0x70000
TX_SET_TAG = 0x27FF46B0
TX_FORMAT_BLOCK = bytes.fromhex('df72bade66d306759f2a8b4f')


def _tag(raw):
    return raw[::-1]


def hash_string(text):
    h = 0
    for c in text.encode('ascii'):
        if 0x41 <= c <= 0x5A:
            c += 0x20
        h = (h * 0x1003F + c) & 0xFFFFFFFF
    return h


# ---------------------------------------------------------------- container

def parse_apx(data):
    """[(tag, body)] without the alig chunks; tags as read (b'apti', b'tx4t', ...)."""
    if _tag(data[:4]) != b'apxf' or struct.unpack_from('<I', data, 4)[0] != len(data):
        raise ValueError('not an apx container')
    out, off = [], 8
    while off < len(data):
        tag = _tag(data[off:off + 4])
        size = struct.unpack_from('<I', data, off + 4)[0]
        if size < 8 or off + size > len(data):
            raise ValueError('bad chunk at %#x' % off)
        if tag != b'alig':
            out.append((tag, data[off + 8:off + size]))
        off += size
    return out


def build_apx(chunks):
    out = bytearray(_tag(b'apxf') + bytes(4))
    for tag, body in chunks:
        pad = 0 if tag == b'vers' else (-(len(out) + 8)) % 16
        if pad:
            if pad < 8:
                pad += 16
            out += _tag(b'alig') + struct.pack('<I', pad) + bytes(pad - 8)
        out += _tag(tag) + struct.pack('<I', 8 + len(body)) + body
    struct.pack_into('<I', out, 4, len(out))
    return bytes(out)


def chunk(chunks, tag):
    hits = [i for i, (t, _) in enumerate(chunks) if t == tag]
    if len(hits) != 1:
        raise ValueError('%r found %d times' % (tag, len(hits)))
    return hits[0]


# ---------------------------------------------------------------- the movie

class Apt:
    def __init__(self, apti, cons):
        self.d = bytearray(apti)
        c = cons.find(b'Apt constant file')
        if c < 0:
            raise ValueError('no constant file header')
        self.movie = struct.unpack_from('<I', cons, c + 20)[0]
        if self.type_of(self.movie) != 9:
            raise ValueError('movie character not found')

    def u32(self, o):
        return struct.unpack_from('<I', self.d, o)[0]

    def i32(self, o):
        return struct.unpack_from('<i', self.d, o)[0]

    def f32(self, o):
        return struct.unpack_from('<f', self.d, o)[0]

    def put(self, o, value):
        struct.pack_into('<I', self.d, o, value)

    def s(self, o):
        e = self.d.find(b'\0', o)
        return self.d[o:e].decode('latin1')

    def type_of(self, o):
        if o <= 0 or o + 8 > len(self.d) or self.u32(o + 4) != SIG:
            return None
        return self.u32(o)

    # characters
    def char_table(self):
        n, p = self.u32(self.movie + 20), self.u32(self.movie + 24)
        return [self.u32(p + 4 * i) for i in range(n)]

    def set_char_table(self, offsets):
        p = self.alloc(b''.join(struct.pack('<I', o) for o in offsets))
        self.put(self.movie + 20, len(offsets))
        self.put(self.movie + 24, p)

    def alloc(self, blob, align=4):
        pad = (-len(self.d)) % align
        self.d += bytes(pad)
        at = len(self.d)
        self.d += blob
        return at

    def string(self, text):
        return self.alloc(text.encode('ascii') + b'\0', 1)

    def shape(self, char_id):
        o = self.char_table()[char_id]
        if self.type_of(o) != 1:
            raise ValueError('character %d is not a shape' % char_id)
        return bytes(self.d[o:o + 28])

    def add_shape(self, template, geometry):
        blob = bytearray(template)
        struct.pack_into('<I', blob, 24, geometry)
        return self.alloc(bytes(blob))

    # sprites and frames
    def frames_of(self, o):
        n, p = self.u32(o + 8), self.u32(o + 12)
        return [(self.u32(p + 8 * k), self.u32(p + 8 * k + 4)) for k in range(n)]

    def items_of(self, frame):
        n, p = frame
        return [self.u32(p + 4 * j) for j in range(n)]

    def sprite(self, char_id):
        o = self.char_table()[char_id]
        if self.type_of(o) != 5:
            raise ValueError('character %d is not a sprite' % char_id)
        return o

    def set_frames(self, sprite_off, frames):
        p = self.alloc(b''.join(struct.pack('<II', n, q) for n, q in frames))
        self.put(sprite_off + 8, len(frames))
        self.put(sprite_off + 12, p)

    def new_frame(self, items):
        ptrs = [self.alloc(it) for it in items]
        p = self.alloc(b''.join(struct.pack('<I', q) for q in ptrs))
        return (len(items), p)

    def label_item(self, name, index):
        return struct.pack('<IIII', 2, self.string(name), LABEL_FLAGS, index)

    @staticmethod
    def remove_item(depth):
        return struct.pack('<Ii', 4, depth)

    def place_copy(self, item_off, character=None, name=None):
        blob = bytearray(self.d[item_off:item_off + 64])
        if struct.unpack_from('<I', blob, 0)[0] != 3:
            raise ValueError('not a PlaceObject at %#x' % item_off)
        if character is not None:
            struct.pack_into('<i', blob, 12, character)
        if name is not None:
            struct.pack_into('<I', blob, 52, self.string(name))
        return bytes(blob)

    def frame_labels(self, sprite_off):
        out = []
        for k, fr in enumerate(self.frames_of(sprite_off)):
            for io in self.items_of(fr):
                if self.u32(io) == 2:
                    out.append((k, self.s(self.u32(io + 4)), self.u32(io + 8), self.u32(io + 12)))
        return out

    # checking
    def check(self, geometry_ids):
        """Problems found walking every character, frame and item (empty list = clean)."""
        bad = []
        size = len(self.d)
        table = self.char_table()
        n = len(table)

        def inside(o, ln, what):
            if not (0 < o and o + ln <= size):
                bad.append('%s at %#x outside the movie' % (what, o))
                return False
            return True

        def walk_frames(owner, o):
            labels = set()
            nf, fp = self.u32(o + 8), self.u32(o + 12)
            if nf and not inside(fp, 8 * nf, '%s frames' % owner):
                return
            for k in range(nf):
                cnt, ip = self.u32(fp + 8 * k), self.u32(fp + 8 * k + 4)
                if cnt and not inside(ip, 4 * cnt, '%s frame %d items' % (owner, k)):
                    continue
                for j in range(cnt):
                    io = self.u32(ip + 4 * j)
                    if not inside(io, 8, '%s frame %d item' % (owner, k)):
                        continue
                    t = self.u32(io)
                    if t == 3:
                        if not inside(io, 64, '%s PlaceObject' % owner):
                            continue
                        ch = self.i32(io + 12)
                        if ch >= n or ch < -1:
                            bad.append('%s frame %d places character %d of %d' % (owner, k, ch, n))
                        nm = self.u32(io + 52)
                        if nm and not inside(nm, 1, '%s place name' % owner):
                            pass
                    elif t == 2:
                        if inside(io, 16, '%s label' % owner) and inside(self.u32(io + 4), 1, '%s label name' % owner):
                            name = self.s(self.u32(io + 4))
                            if name in labels:
                                bad.append('%s label %s twice' % (owner, name))
                            labels.add(name)
                            if self.u32(io + 8) != LABEL_FLAGS or self.u32(io + 12) != k:
                                bad.append('%s label %s fields %#x/%d at frame %d' % (owner, name, self.u32(io + 8), self.u32(io + 12), k))
                    elif t not in (1, 4, 5, 8):
                        bad.append('%s frame %d unknown item type %d' % (owner, k, t))

        walk_frames('movie', self.movie)
        for i, o in enumerate(table):
            if not o:
                continue
            t = self.type_of(o)
            if t is None:
                bad.append('character %d at %#x has no signature' % (i, o))
                continue
            if t == 1:
                g = self.u32(o + 24)
                if g not in geometry_ids:
                    bad.append('shape %d uses geometry %d, not in rubi' % (i, g))
            elif t == 5:
                walk_frames('sprite %d' % i, o)
        return bad


# ---------------------------------------------------------------- geometry

def parse_rubi(body):
    out, p = [], 0
    while p + 8 <= len(body):
        gid, size = struct.unpack_from('<II', body, p)
        if size < 8 or p + size > len(body):
            raise ValueError('bad geometry record at %#x' % p)
        out.append((gid, body[p + 8:p + size]))
        p += size
    if p != len(body):
        raise ValueError('rubi has %d stray bytes' % (len(body) - p))
    return out


def build_rubi(recs):
    return b''.join(struct.pack('<II', gid, 8 + len(b)) + b for gid, b in recs)


def sub_records(body):
    out, p = [], 0
    while p + 4 <= len(body):
        size = struct.unpack_from('<I', body, p)[0]
        if size < 64 or p + size > len(body):
            raise ValueError('bad sub-record at %#x' % p)
        out.append(body[p:p + size])
        p += size
    return out


def quad_info(body):
    """(page, u0, v0, u1, v1, x0, y0, x1, y1) of a one-quad geometry body."""
    subs = sub_records(body)
    if len(subs) != 1 or len(subs[0]) != 96 or struct.unpack_from('<I', subs[0], 60)[0] != 4:
        raise ValueError('not a single textured quad')
    m = struct.unpack_from('<8f', body, 20)
    page = struct.unpack_from('<I', body, 52)[0]
    v = struct.unpack_from('<8f', body, 64)
    xs, ys = v[0::2], v[1::2]
    us = [m[0] * x + m[1] * y + m[2] for x, y in zip(xs, ys)]
    vs = [m[4] * x + m[5] * y + m[6] for x, y in zip(xs, ys)]
    return page, min(us), min(vs), max(us), max(vs), min(xs), min(ys), max(xs), max(ys)


def quad_on_page(template, page, rect, page_w, page_h):
    """The template quad mapped onto rect (x0, y0, x1, y1 in pixels) of a page."""
    _, _, _, _, _, x0, y0, x1, y1 = quad_info(template)
    u0, v0, u1, v1 = rect[0] / page_w, rect[1] / page_h, rect[2] / page_w, rect[3] / page_h
    m0 = (u1 - u0) / (x1 - x0)
    m5 = (v1 - v0) / (y1 - y0)
    m = list(struct.unpack_from('<8f', template, 20))
    m[0], m[1], m[2] = m0, 0.0, u0 - m0 * x0
    m[4], m[5], m[6] = 0.0, m5, v0 - m5 * y0
    out = bytearray(template)
    struct.pack_into('<8f', out, 20, *m)
    struct.pack_into('<I', out, 52, page)
    return bytes(out)


# ---------------------------------------------------------------- textures

def parse_tx4h(body):
    name_off, ver, count, ids_off, table_off, tag = struct.unpack_from('<IHHIII', body, 0)
    if ver != 4 or tag != TX_SET_TAG:
        raise ValueError('unexpected tx4h header')

    def cstr(o):
        return body[o:body.index(b'\0', o)].decode('ascii')

    pages = []
    for i in range(count):
        no, size, w, h, z0, z1, mips, fmt, fo, so = struct.unpack_from('<IIHHBBBBII', body, table_off + 24 * i)
        pages.append({'name': cstr(no), 'size': size, 'w': w, 'h': h, 'mips': mips, 'format': fmt,
                      'id': struct.unpack_from('<I', body, ids_off + 4 * i)[0]})
    return cstr(name_off), pages


def build_tx4h(set_name, pages):
    """Header in the stock layout: table, the format/sampler blocks (strings after the first),
    the set name and the page names."""
    n = len(pages)
    ids_off = 20
    table_off = ids_off + 4 * n
    blocks = bytearray()
    base = table_off + 24 * n
    fmt_off = wrap_off = None
    where = []
    for i, p in enumerate(pages):
        a = base + len(blocks)
        blocks += TX_FORMAT_BLOCK
        b = base + len(blocks)
        if i == 0:
            fmt_off = b + 15
            wrap_off = fmt_off + 5
        blocks += struct.pack('<BIBIBI', 0, fmt_off, 0, wrap_off, 1, p['size'])
        if i == 0:
            blocks += b'DXT5\0clamp\0'
        where.append((a, b))
    name_off = base + len(blocks)
    tail = bytearray(set_name.encode('ascii') + b'\0')
    page_name_offs = []
    for p in pages:
        page_name_offs.append(name_off + len(tail))
        tail += p['name'].encode('ascii') + b'\0'
    out = bytearray(struct.pack('<IHHIII', name_off, 4, n, ids_off, table_off, TX_SET_TAG))
    for p in pages:
        out += struct.pack('<I', zlib.crc32((set_name + p['name']).encode('ascii')))
    for p, (a, b), no in zip(pages, where, page_name_offs):
        if p.get('format', 3) != 3 or p.get('mips', 1) != 1:
            raise ValueError('only single level DXT5 pages are written')
        out += struct.pack('<IIHHBBBBII', no, p['size'], p['w'], p['h'], 0, 0, 1, 3, a, b)
    out += blocks + tail
    return bytes(out)


def dds_header(w, h, size, fourcc=b'DXT5'):
    hd = bytearray(124)
    struct.pack_into('<I', hd, 0, 124)
    struct.pack_into('<I', hd, 4, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000)
    struct.pack_into('<II', hd, 8, h, w)
    struct.pack_into('<I', hd, 16, size)
    struct.pack_into('<I', hd, 72, 32)
    struct.pack_into('<I', hd, 76, 0x4)
    hd[80:84] = fourcc
    struct.pack_into('<I', hd, 104, 0x1000)
    return b'DDS ' + bytes(hd)


def dxt_decode(data, w, h, fourcc=b'DXT5'):
    blocks = ((w + 3) // 4) * ((h + 3) // 4) * (8 if fourcc == b'DXT1' else 16)
    im = Image.open(io.BytesIO(dds_header(w, h, blocks, fourcc) + data[:blocks]))
    im.load()
    return im.convert('RGBA')


def dxt5_encode(im):
    """Top level DXT5 blocks of an RGBA image (sides multiples of 4)."""
    im = im.convert('RGBA')
    w, h = im.size
    if w % 4 or h % 4:
        raise ValueError('page size must be a multiple of 4')
    buf = io.BytesIO()
    im.save(buf, format='DDS', pixel_format='DXT5')
    dds = buf.getvalue()
    if dds[:4] != b'DDS ' or dds[84:88] != b'DXT5':
        raise ValueError('Pillow did not write DXT5')
    data = dds[128:128 + w * h]
    if len(data) != w * h:
        raise ValueError('short DXT5 data')
    return data


def texture_rgba(tg4h, tg4d):
    """A tg4h/tg4d texture pair as RGBA (top level)."""
    w, h = struct.unpack_from('<HH', tg4h, 0x20)
    fourcc = b'DXT5'
    for name, cc in ((b'DXT1', b'DXT1'), (b'DXT3', b'DXT3'), (b'DXT5', b'DXT5')):
        if name in tg4h:
            fourcc = cc
            break
    return dxt_decode(tg4d, w, h, fourcc)
