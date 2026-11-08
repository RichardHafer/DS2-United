"""Add the campaign arena map pictures to the MP map picker (MP20) and the MP lobby (MP02).

Input is the installed final frontend stream (red pipeline, final5). Per arena of
src/generated_arenas.h (kArenas order) both movies get one 512x256 DXT1c page, one shape and
one frame labelled "map" + arena name ("maparena_ch03"), placed like Mark's map5 picture.
MP02 also gets the frames map5/map6 with the Outbreak pictures of MP20 (pages 4/5). Only
mp20gamesettings.apx and mp02lobby.apx change; every other asset is copied as it is.
Plan: analysis/ARENA_MAP_PICTURES_2026-10-16.md, option B.

Pictures: arenas/pictures/<arena name>.png|.jpg|.jpeg, any size; centre crop to 2:1, Lanczos
to 512x256, a light sharpen. An arena without a picture gets a placeholder card with its label.

    build_map_pictures.py [--in FILE] [--force] [--out FILE] [--pictures DIR] [--arenas FILE]
                          [--review DIR] [--numbered] [--check] [--self-test]

--in        default <game>/randomizer/ds2coop_frontend.str (must be final5, CRC 87FEF2BD)
--force     accept another input
--out       default build/frontend_final6/ds2coop_frontend.str (never inside the game folder)
--review    review sheets, default build/map_pictures_review (with --check only when given)
--numbered  label the arena frames map7.. (list index fallback) instead of by name
--check     build and verify in memory, write nothing
--self-test RefPack and DXT1 round trips, then exit

The plugin rejects the frontend unless it accepts the new MP20 size (gamemode::native::CheckFile).
If recolor_frontend.py ever runs after this, add the new pages to its CONTENT_PAGES.
"""
import argparse
import json
import re
import struct
import sys
import types
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageOps

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from ds2_paths import GAME_DIR, MODDING_TOOLS  # noqa: E402
import apx_edit as ax  # noqa: E402

INPUT_CRC = 0x87FEF2BD
MP20 = 'mp20gamesettings.apx'
MP02 = 'mp02lobby.apx'
LIBFRONTEND = 'libfrontend.apx'
QUAD_EXPORT = 'mp_load_05'          # stock picture sprite whose 512x256 quad is the template
W, H = 512, 256
PAGE_SIZE = W * H // 2
SAFE = (72, 74, 450, 209)           # visible in both screens, page pixels
FIRST_NUMBERED = 7                  # list index of the first arena (stock 0..4, mall 5, school 6)
PICTURE_EXTS = ('.png', '.jpg', '.jpeg')
OUTBREAK_PAGES = {'map5': 4, 'map6': 5}

# sprite with the map frames, its labels in the input, the frame whose items are copied,
# the picture depth and the placement the template must have (scale, x, y)
MOVIES = {
    MP20: {'key': 'mp20', 'sprite': 41, 'template': 'map5', 'depth': 3, 'place': (0.668, 135.2, 88.3),
           'labels': ['init', 'map_Any'] + ['map%d' % k for k in range(7)]},
    MP02: {'key': 'mp02', 'sprite': 26, 'template': 'map4', 'depth': 2, 'place': (1.0, 257.3, 95.6),
           'labels': ['map%d' % k for k in range(5)]},
}
STOP_ACTION = b'\x07\x00'           # ActionStop, ActionEnd
PLACE_FLAGS = 0x16                  # character, matrix, ratio
CLIP_FLAG = 0x40


# ---------------------------------------------------------------- RefPack

def refpack_decompress(data):
    hdr = struct.unpack_from('>H', data, 0)[0]
    p = 2
    if hdr & 0x0100:
        p += 4 if hdr & 0x8000 else 3
    if hdr & 0x8000:
        size = struct.unpack_from('>I', data, p)[0]
        p += 4
    else:
        size = data[p] << 16 | data[p + 1] << 8 | data[p + 2]
        p += 3
    out = bytearray()
    while True:
        b0 = data[p]
        if b0 < 0x80:
            b1 = data[p + 1]
            p += 2
            lit, ln, dist = b0 & 3, ((b0 >> 2) & 7) + 3, ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:
            b1, b2 = data[p + 1], data[p + 2]
            p += 3
            lit, ln, dist = b1 >> 6, (b0 & 0x3F) + 4, ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:
            b1, b2, b3 = data[p + 1], data[p + 2], data[p + 3]
            p += 4
            lit, ln, dist = b0 & 3, ((b0 & 0x0C) << 6) + b3 + 5, ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:
            p += 1
            lit, ln, dist = ((b0 & 0x1F) << 2) + 4, 0, 0
        else:
            p += 1
            out += data[p:p + (b0 & 3)]
            break
        out += data[p:p + lit]
        p += lit
        if ln:
            s = len(out) - dist
            if s < 0:
                raise ValueError('RefPack distance before the start')
            if dist >= ln:
                out += out[s:s + ln]
            else:
                for i in range(ln):
                    out.append(out[s + i])
    if len(out) != size:
        raise ValueError('RefPack size %d, header says %d' % (len(out), size))
    return bytes(out)


def _min_len(dist):
    return 3 if dist <= 1024 else 4 if dist <= 16384 else 5


def _match(data, a, b, limit):
    ln = 0
    while ln + 32 <= limit and data[a + ln:a + ln + 32] == data[b + ln:b + ln + 32]:
        ln += 32
    while ln < limit and data[a + ln] == data[b + ln]:
        ln += 1
    return ln


def refpack_compress(data, max_chain=32, good=258):
    """Greedy hash chain LZ, header 10 FB + 3 byte size like the stock pieces."""
    data = bytes(data)
    n = len(data)
    if n >= 1 << 24:
        raise ValueError('RefPack piece too large')
    out = bytearray(b'\x10\xfb' + n.to_bytes(3, 'big'))
    head, prev = {}, [-1] * n
    pos = start = 0

    def literals(upto):
        nonlocal start
        while upto - start >= 4:
            run = min(112, (upto - start) & ~3)
            out.append(0xE0 | (run - 4) >> 2)
            out.extend(data[start:start + run])
            start += run

    while pos < n:
        best, bdist = 0, 0
        if pos + 3 <= n:
            key = data[pos:pos + 3]
            cand = head.get(key, -1)
            prev[pos] = cand
            head[key] = pos
            limit = min(1028, n - pos)
            chain = max_chain
            while cand >= 0 and chain:
                dist = pos - cand
                if dist > 131072:
                    break
                if best < limit and data[cand + best] == data[pos + best]:
                    ln = _match(data, cand, pos, limit)
                    if ln > best and ln >= _min_len(dist):
                        best, bdist = ln, dist
                        if ln >= limit or ln >= good:
                            break
                cand = prev[cand]
                chain -= 1
        if not best:
            pos += 1
            continue
        literals(pos)
        lit = pos - start
        d, ln = bdist - 1, best
        if ln <= 10 and d < 1024:
            out += bytes(((d >> 8) << 5 | (ln - 3) << 2 | lit, d & 0xFF))
        elif ln <= 67 and d < 16384:
            out += bytes((0x80 | (ln - 4), lit << 6 | d >> 8, d & 0xFF))
        else:
            out += bytes((0xC0 | (d >> 16) << 4 | ((ln - 5) >> 8) << 2 | lit, (d >> 8) & 0xFF, d & 0xFF, (ln - 5) & 0xFF))
        out += data[start:pos]
        for k in range(pos + 1, min(pos + ln, n - 2)):
            key = data[k:k + 3]
            prev[k] = head.get(key, -1)
            head[key] = k
        pos += ln
        start = pos
    literals(n)
    out.append(0xFC | (n - start))
    out += data[start:]
    return bytes(out)


# frontend_str imports ds2str from the modding tools folder; nothing from there may run, so it
# gets this codec instead and the folder leaves sys.path again.
sys.modules['ds2str'] = types.SimpleNamespace(refpack_compress=refpack_compress, refpack_decompress=refpack_decompress)
import frontend_str as fs  # noqa: E402
sys.path[:] = [p for p in sys.path if Path(p).resolve() != Path(MODDING_TOOLS).resolve()]


# ---------------------------------------------------------------- pictures

def font(size, bold=True):
    names = ('arialbd.ttf', 'DejaVuSans-Bold.ttf') if bold else ('arial.ttf', 'DejaVuSans.ttf')
    for name in names:
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default(size)


def fitted_font(draw, text, width, size, bold):
    while size > 10:
        f = font(size, bold)
        if draw.textlength(text, font=f) <= width:
            return f
        size -= 1
    return font(size, bold)


def placeholder(name, label):
    im = Image.new('RGB', (W, H), (18, 20, 24))
    d = ImageDraw.Draw(im)
    for y in range(0, H, 8):
        d.line([(0, y), (W, y)], fill=(24, 27, 32))
    x0, y0, x1, y1 = SAFE
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    big = fitted_font(d, label.upper(), x1 - x0 - 24, 34, True)
    small = fitted_font(d, name, x1 - x0 - 24, 18, False)
    d.text((cx, cy - 12), label.upper(), font=big, fill=(216, 68, 60), anchor='mm')
    d.text((cx, cy + 26), name, font=small, fill=(170, 170, 170), anchor='mm')
    return im


def load_picture(path):
    """Centre 2:1 crop, Lanczos to 512x256, light sharpen; returns (image, note)."""
    im = ImageOps.exif_transpose(Image.open(path))
    note = '%dx%d' % im.size
    if im.mode in ('RGBA', 'LA', 'PA') or 'transparency' in im.info:
        im = im.convert('RGBA')
        if im.getchannel('A').getextrema()[0] < 255:
            flat = Image.new('RGBA', im.size, (0, 0, 0, 255))
            flat.alpha_composite(im)
            im = flat
            note += ', transparency flattened on black'
    im = im.convert('RGB')
    w, h = im.size
    if w < W or h < H:
        note += ', upscaled'
    if w / h > 2:
        box = ((w - 2 * h) / 2, 0, (w + 2 * h) / 2, h)
    else:
        box = (0, (h - w / 2) / 2, w, (h + w / 2) / 2)
    im = im.resize((W, H), Image.LANCZOS, box=box)
    return im.filter(ImageFilter.UnsharpMask(radius=1.0, percent=50, threshold=2)), note


def find_picture(folder, name):
    hits = [folder / (name + ext) for ext in PICTURE_EXTS if (folder / (name + ext)).is_file()]
    if len(hits) > 1:
        raise SystemExit('%s: more than one picture (%s)' % (name, ', '.join(p.name for p in hits)))
    return hits[0] if hits else None


# ---------------------------------------------------------------- DXT1

def _to565(c):
    c = np.clip(np.rint(c), 0, 255).astype(np.int64)
    return (c[..., 0] * 31 + 127) // 255 << 11 | (c[..., 1] * 63 + 127) // 255 << 5 | (c[..., 2] * 31 + 127) // 255


def _from565(v):
    r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
    return np.stack([r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2], -1).astype(np.float64)


def dxt1_encode(im, refine=2):
    """Opaque DXT1 (four colour blocks only): principal axis fit, then least squares endpoints.
    Pillow's encoder is 2-6 dB worse on the delivered pictures."""
    a = np.asarray(im.convert('RGB'), dtype=np.float64)
    h, w, _ = a.shape
    if w % 4 or h % 4:
        raise ValueError('page size must be a multiple of 4')
    px = a.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3)
    n = len(px)
    mean = px.mean(1)
    cen = px - mean[:, None]
    cov = np.einsum('nki,nkj->nij', cen, cen)
    axis = np.full((n, 3), 3 ** -0.5)
    for _ in range(8):
        axis = np.einsum('nij,nj->ni', cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-9)
    t = np.einsum('nki,ni->nk', cen, axis)
    e0 = mean + t.max(1)[:, None] * axis
    e1 = mean + t.min(1)[:, None] * axis
    weights = np.array([[1, 0], [0, 1], [2 / 3, 1 / 3], [1 / 3, 2 / 3]])
    for step in range(refine + 1):
        q0, q1 = _from565(_to565(e0)), _from565(_to565(e1))
        pal = np.stack([q0, q1, (2 * q0 + q1) / 3, (q0 + 2 * q1) / 3], 1)
        idx = ((px[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1).argmin(2)
        if step == refine:
            break
        ws = weights[idx]
        m = np.einsum('nki,nkj->nij', ws, ws)
        rhs = np.einsum('nki,nkc->nic', ws, px)
        det = m[:, 0, 0] * m[:, 1, 1] - m[:, 0, 1] * m[:, 1, 0]
        ok = np.abs(det) > 1e-6
        safe = np.where(ok, det, 1.0)
        inv = np.stack([np.stack([m[:, 1, 1], -m[:, 0, 1]], -1), np.stack([-m[:, 1, 0], m[:, 0, 0]], -1)], 1) / safe[:, None, None]
        sol = np.einsum('nij,njc->nic', inv, rhs)
        e0 = np.where(ok[:, None], sol[:, 0], e0)
        e1 = np.where(ok[:, None], sol[:, 1], e1)
    c0, c1 = _to565(e0), _to565(e1)
    swap = c0 < c1                       # four colour mode needs c0 > c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    idx = np.where(swap[:, None], np.array([1, 0, 3, 2])[idx], idx)
    idx = np.where((c0 == c1)[:, None], 0, idx)
    bits = (idx.astype(np.uint64) << (2 * np.arange(16, dtype=np.uint64))).sum(1)
    out = np.zeros((n, 8), np.uint8)
    out[:, 0], out[:, 1], out[:, 2], out[:, 3] = c0 & 255, c0 >> 8, c1 & 255, c1 >> 8
    for k in range(4):
        out[:, 4 + k] = (bits >> np.uint64(8 * k)) & np.uint64(255)
    return out.tobytes()


def dxt1_decode(data, w, h):
    return ax.dxt_decode(data, w, h, b'DXT1')      # fourcc must be exactly DXT1


def psnr(a, b):
    err = np.asarray(a.convert('RGB'), np.float64) - np.asarray(b.convert('RGB'), np.float64)
    mse = (err ** 2).mean()
    return 99.0 if mse == 0 else 10 * np.log10(255 ** 2 / mse)


# ---------------------------------------------------------------- arenas

def read_arenas(header):
    text = Path(header).read_text(encoding='utf-8')
    m = re.search(r'inline constexpr Arena kArenas\[\] = \{\n(.*?)\n\};', text, re.S)
    if not m:
        raise SystemExit('%s has no kArenas table' % header)
    out = []
    for line in m.group(1).splitlines():
        row = re.match(r'\s*\{"([^"]+)", "([^"]*)"', line)
        if not row:
            raise SystemExit('%s: unexpected kArenas row %r' % (header, line[:60]))
        name, label = row.groups()
        if not re.fullmatch(r'[A-Za-z0-9_]+', name) or not all(32 <= ord(c) < 127 for c in label):
            raise SystemExit('%s: arena name or label is not plain ASCII: %r %r' % (header, name, label))
        out.append((name, label))
    if not out:
        raise SystemExit('%s: kArenas is empty' % header)
    if len({n.lower() for n, _ in out}) != len(out):
        raise SystemExit('%s: duplicate arena names' % header)
    return out


# ---------------------------------------------------------------- movie data

def open_apt(chunks):
    return ax.Apt(chunks[ax.chunk(chunks, b'apti')][1], chunks[ax.chunk(chunks, b'cons')][1])


def read_pages(body):
    """tx4h: set name, header fields and per page name, size, w, h, mips, keys, values, id."""
    name_off, ver, count, ids_off, table_off, tag = struct.unpack_from('<IHHIII', body, 0)
    if ver != 4 or tag != ax.TX_SET_TAG:
        raise ValueError('unexpected tx4h header')

    def cstr(o):
        return body[o:body.index(b'\0', o)].decode('ascii')
    pages = []
    for i in range(count):
        no, size, w, h, z0, z1, mips, nkeys, fo, so = struct.unpack_from('<IIHHBBBBII', body, table_off + 24 * i)
        values = []
        for k in range(nkeys):
            kind, v = struct.unpack_from('<BI', body, so + 5 * k)
            values.append(cstr(v) if kind == 0 else v)
        pages.append({'name': cstr(no), 'size': size, 'w': w, 'h': h, 'z': (z0, z1), 'mips': mips,
                      'keys': body[fo:fo + 4 * nkeys], 'values': values,
                      'id': struct.unpack_from('<I', body, ids_off + 4 * i)[0],
                      'entry': body[table_off + 24 * i:table_off + 24 * i + 24]})
    return cstr(name_off), pages, (name_off, count, ids_off, table_off)


def append_pages(body, count_new):
    """Mark's layout: the old body stays, then per page a key block, a value block and the page
    name, a new id array and a new page table with the old entries copied."""
    name_off, ver, count, ids_off, table_off, tag = struct.unpack_from('<IHHIII', body, 0)
    set_name, pages, _ = read_pages(body)
    out = bytearray(body)

    def put(blob, align=1):
        out.extend(bytes((-len(out)) % align))
        at = len(out)
        out.extend(blob)
        return at
    fmt, wrap = put(b'DXT1c\0'), put(b'clamp\0')
    ids = [p['id'] for p in pages]
    entries = [p['entry'] for p in pages]
    for k in range(count_new):
        name = str(count + 1 + k)
        fo = put(ax.TX_FORMAT_BLOCK)              # format, wrap, size
        so = put(struct.pack('<BIBIBI', 0, fmt, 0, wrap, 1, PAGE_SIZE))
        no = put(name.encode('ascii') + b'\0')
        entries.append(struct.pack('<IIHHBBBBII', no, PAGE_SIZE, W, H, 0, 0, 1, 3, fo, so))
        ids.append(zlib.crc32((set_name + name).encode('ascii')))
    new_ids = put(b''.join(struct.pack('<I', i) for i in ids), 4)
    new_table = put(b''.join(entries), 4)
    struct.pack_into('<IHHIII', out, 0, name_off, ver, count + count_new, new_ids, new_table, tag)
    return bytes(out)


def exports(apt):
    n, p = apt.u32(apt.movie + 48), apt.u32(apt.movie + 52)
    return {apt.s(apt.u32(p + 8 * i)): apt.u32(p + 8 * i + 4) for i in range(n)}


def check_quad(body, what):
    page, u0, v0, u1, v1, x0, y0, x1, y1 = ax.quad_info(body)
    if max(abs(u0), abs(v0), abs(u1 - 1), abs(v1 - 1)) > 1e-3 or \
            max(abs(x0 + 256), abs(y0 + 128), abs(x1 - 256), abs(y1 - 128)) > 0.05:
        raise SystemExit('%s is not a 512x256 quad over a whole page (%s)' % (what, ax.quad_info(body)))
    if struct.unpack_from('<4f', body, 4) != (0.5, 0.5, 0.5, 1.0):
        raise SystemExit('%s is not drawn in the neutral colour' % what)
    return page


def on_page(geo, page):
    """The stock quad as it is, drawing another page (it covers the whole page already)."""
    out = bytearray(geo)
    struct.pack_into('<I', out, 52, page)
    return bytes(out)


def quad_template(lib_apx):
    """Shape and geometry of LibFrontend's mp_load_05, the quad every stock map picture uses."""
    chunks = ax.parse_apx(lib_apx)
    apt = open_apt(chunks)
    geos = dict(ax.parse_rubi(chunks[ax.chunk(chunks, b'rubi')][1]))
    cid = exports(apt).get(QUAD_EXPORT)
    if cid is None:
        raise SystemExit('%s exports no %s' % (LIBFRONTEND, QUAD_EXPORT))
    frames = apt.frames_of(apt.sprite(cid))
    places = [io for io in apt.items_of(frames[0]) if apt.u32(io) == 3] if len(frames) == 1 else []
    if len(places) != 1:
        raise SystemExit('%s.%s is not a one frame, one shape sprite' % (LIBFRONTEND, QUAD_EXPORT))
    shape_id = apt.i32(places[0] + 12)
    shape = apt.shape(shape_id)
    if struct.unpack_from('<4f', shape, 8) != (-256.0, -128.0, 256.0, 128.0):
        raise SystemExit('%s shape %d bounds' % (LIBFRONTEND, shape_id))
    geo = geos[struct.unpack_from('<I', shape, 24)[0]]
    check_quad(geo, '%s geometry %d' % (LIBFRONTEND, shape_id))
    return shape, geo


def frame_items(apt, frame):
    return [(apt.u32(io), io) for io in apt.items_of(frame)]


def template_frame(apt, cfg, what):
    """Items of the template frame: remove, label, action (stop), place at the expected spot."""
    sprite = apt.sprite(cfg['sprite'])
    got = [name for _, name, _, _ in apt.frame_labels(sprite)]
    if got != cfg['labels']:
        raise SystemExit('%s: sprite %d labels %s, expected %s' % (what, cfg['sprite'], got, cfg['labels']))
    k = [k for k, name, _, _ in apt.frame_labels(sprite) if name == cfg['template']][0]
    items = frame_items(apt, apt.frames_of(sprite)[k])
    if [t for t, _ in items] != [4, 2, 1, 3]:
        raise SystemExit('%s: frame %s items %s, expected remove, label, action, place' % (what, cfg['template'], [t for t, _ in items]))
    remove, action, place = items[0][1], items[2][1], items[3][1]
    depth = cfg['depth']
    if apt.i32(remove + 4) != depth or apt.i32(place + 8) != depth:
        raise SystemExit('%s: frame %s does not work on depth %d' % (what, cfg['template'], depth))
    code = apt.u32(action + 4)
    if bytes(apt.d[code:code + 2]) != STOP_ACTION:
        raise SystemExit('%s: frame %s action is not a plain stop' % (what, cfg['template']))
    if apt.u32(place + 4) != PLACE_FLAGS:
        raise SystemExit('%s: frame %s place flags %#x' % (what, cfg['template'], apt.u32(place + 4)))
    a, b, c, d, tx, ty = struct.unpack_from('<6f', apt.d, place + 16)
    scale, x, y = cfg['place']
    if b or c or abs(a - scale) > 0.002 or abs(d - scale) > 0.002 or abs(tx - x) > 0.1 or abs(ty - y) > 0.1:
        raise SystemExit('%s: frame %s places at %.3f/%.3f %.2f/%.2f, expected %s' % (what, cfg['template'], a, d, tx, ty, cfg['place']))
    return sprite, bytes(apt.d[action:action + 8]), place, (a, tx, ty)


def extend_movie(apx, cfg, entries, shape_tpl, geo_tpl, what):
    """Append one page, shape and frame per (label, page data)."""
    chunks = ax.parse_apx(apx)
    realigned = ax.build_apx(chunks) != apx
    if realigned and ax.parse_apx(ax.build_apx(chunks)) != chunks:
        raise SystemExit('%s does not round trip through the container writer' % what)
    apt = open_apt(chunks)
    geos = ax.parse_rubi(chunks[ax.chunk(chunks, b'rubi')][1])
    baseline = set(apt.check({g for g, _ in geos}))
    sprite, action, tplace, placed = template_frame(apt, cfg, what)
    tx_i = ax.chunk(chunks, b'tx4h')
    _, old_pages, _ = read_pages(chunks[tx_i][1])
    labels = {name.lower() for _, name, _, _ in apt.frame_labels(sprite)}
    table = apt.char_table()
    frames = apt.frames_of(sprite)
    for k, (label, page) in enumerate(entries):
        if label.lower() in labels:
            raise SystemExit('%s: label %s exists already' % (what, label))
        if len(page) != PAGE_SIZE:
            raise SystemExit('%s: page for %s has %d bytes' % (what, label, len(page)))
        labels.add(label.lower())
        cid, index = len(table), len(frames)
        if any(g == cid for g, _ in geos):      # geometry id = character id, as in the stock movies
            raise SystemExit('%s: geometry %d exists already' % (what, cid))
        geos.append((cid, on_page(geo_tpl, len(old_pages) + 1 + k)))
        table.append(apt.add_shape(shape_tpl, cid))
        place = bytearray(apt.place_copy(tplace, character=cid))
        struct.pack_into('<f', place, 48, index / 65536)   # ratio: frame / 65536 like the stock frames
        frames.append(apt.new_frame([apt.remove_item(cfg['depth']), apt.label_item(label, index), action, bytes(place)]))
    apt.set_frames(sprite, frames)
    apt.set_char_table(table)
    out = []
    for tag, body in chunks:
        if tag == b'apti':
            body = bytes(apt.d)
        elif tag == b'rubi':
            body = ax.build_rubi(geos)
        elif tag == b'tx4h':
            body = append_pages(body, len(entries))
        out.append((tag, body))
    last = max(i for i, (t, _) in enumerate(out) if t == b'tx4t')
    out[last + 1:last + 1] = [(b'tx4t', page) for _, page in entries]
    return ax.build_apx(out), {'baseline': baseline, 'realigned': realigned, 'placed': placed, 'shape': shape_tpl, 'geo': geo_tpl}


# ---------------------------------------------------------------- checks

def verify_movie(old, new, cfg, entries, info, what):
    """The written movie against its input: old parts unchanged, new parts exactly as planned."""
    oc, nc = ax.parse_apx(old), ax.parse_apx(new)
    if ax.build_apx(nc) != new:
        raise SystemExit('%s does not round trip' % what)
    n = len(entries)
    otags, ntags = [t for t, _ in oc], [t for t, _ in nc]
    last = max(i for i, t in enumerate(otags) if t == b'tx4t')
    if ntags != otags[:last + 1] + [b'tx4t'] * n + otags[last + 1:]:
        raise SystemExit('%s: chunk order %s' % (what, ntags))
    body = {t: b for t, b in oc if t != b'tx4t'}
    nbody = {t: b for t, b in nc if t != b'tx4t'}
    for tag in body:
        if tag not in (b'apti', b'rubi', b'tx4h') and body[tag] != nbody[tag]:
            raise SystemExit('%s: chunk %r changed' % (what, tag))
    oa, na = open_apt(oc), open_apt(nc)
    osprite, nsprite = oa.sprite(cfg['sprite']), na.sprite(cfg['sprite'])
    # apti: only the character table and the sprite's frame list fields move
    allowed = set(range(oa.movie + 20, oa.movie + 28)) | set(range(osprite + 8, osprite + 16))
    od, nd = np.frombuffer(bytes(oa.d), np.uint8), np.frombuffer(bytes(na.d[:len(oa.d)]), np.uint8)
    stray = [int(i) for i in np.nonzero(od != nd)[0] if int(i) not in allowed]
    if stray:
        raise SystemExit('%s: %d old movie bytes changed, first at %#x' % (what, len(stray), stray[0]))
    otable, ntable = oa.char_table(), na.char_table()
    if ntable[:len(otable)] != otable or len(ntable) != len(otable) + n:
        raise SystemExit('%s: character table' % what)
    ogeo, ngeo = ax.parse_rubi(body[b'rubi']), ax.parse_rubi(nbody[b'rubi'])
    if ngeo[:len(ogeo)] != ogeo or len(ngeo) != len(ogeo) + n:
        raise SystemExit('%s: geometry records' % what)
    new_problems = sorted(set(na.check({g for g, _ in ngeo})) - info['baseline'])
    if new_problems:
        raise SystemExit('%s: %s' % (what, '; '.join(new_problems[:8])))
    # pages: old header body kept, old entries and data unchanged, new ones as stock DXT1c pages
    oset, opages, _ = read_pages(body[b'tx4h'])
    nset, npages, nhead = read_pages(nbody[b'tx4h'])
    tx = nbody[b'tx4h']
    if nset != oset or tx[20:len(body[b'tx4h'])] != body[b'tx4h'][20:] or nhead[0] != struct.unpack_from('<I', body[b'tx4h'])[0]:
        raise SystemExit('%s: page table header' % what)
    odata = [b for t, b in oc if t == b'tx4t']
    ndata = [b for t, b in nc if t == b'tx4t']
    if len(npages) != len(opages) + n or len(ndata) != len(npages):
        raise SystemExit('%s: %d pages, %d page chunks' % (what, len(npages), len(ndata)))
    for k, (p, d) in enumerate(zip(npages, ndata)):
        if p['id'] != zlib.crc32((nset + p['name']).encode('ascii')) or len(d) != p['size']:
            raise SystemExit('%s: page %d id or size' % (what, k + 1))
        if k < len(opages):
            if p != opages[k] or d != odata[k]:
                raise SystemExit('%s: old page %d changed' % (what, k + 1))
            continue
        if (p['name'], p['size'], p['w'], p['h'], p['z'], p['mips'], p['keys'], p['values']) != \
                (str(k + 1), PAGE_SIZE, W, H, (0, 0), 1, ax.TX_FORMAT_BLOCK, ['DXT1c', 'clamp', PAGE_SIZE]):
            raise SystemExit('%s: new page %d entry %s' % (what, k + 1, p))
        if d != entries[k - len(opages)][1]:
            raise SystemExit('%s: new page %d data' % (what, k + 1))
        if dxt1_decode(d, W, H).getchannel('A').getextrema()[0] < 255:
            raise SystemExit('%s: new page %d has transparent texels' % (what, k + 1))
    # frames: old ones unchanged, then one per entry
    oframes, nframes = oa.frames_of(osprite), na.frames_of(nsprite)
    if nframes[:len(oframes)] != oframes or len(nframes) != len(oframes) + n:
        raise SystemExit('%s: frame list' % what)
    labels = na.frame_labels(nsprite)
    want = cfg['labels'] + [label for label, _ in entries]
    fresh = [(k, fl, idx) for k, _, fl, idx in labels if k >= len(oframes)]
    if [name for _, name, _, _ in labels] != want or len(fresh) != n or any(k != idx or fl != ax.LABEL_FLAGS for k, fl, idx in fresh):
        raise SystemExit('%s: labels %s' % (what, [name for _, name, _, _ in labels]))
    if len({name.lower() for _, name, _, _ in labels}) != len(labels):
        raise SystemExit('%s: labels repeat' % what)
    _, action, tplace, _ = template_frame(oa, cfg, what)
    tbytes = bytes(oa.d[tplace:tplace + 64])
    for j in range(n):
        k = len(oframes) + j
        items = frame_items(na, nframes[k])
        if [t for t, _ in items] != [4, 2, 1, 3]:
            raise SystemExit('%s: frame %d items' % (what, k))
        rem, act, place = items[0][1], items[2][1], items[3][1]
        cid = na.i32(place + 12)
        pb = bytes(na.d[place:place + 64])
        expect = tbytes[:12] + struct.pack('<i', cid) + tbytes[16:48] + struct.pack('<f', k / 65536) + tbytes[52:]
        if na.i32(rem + 4) != cfg['depth'] or bytes(na.d[act:act + 8]) != action or pb != expect:
            raise SystemExit('%s: frame %d remove, action or place differs from the template' % (what, k))
        if cid != len(otable) + j or na.type_of(ntable[cid]) != 1:
            raise SystemExit('%s: frame %d places character %d' % (what, k, cid))
        shape = bytes(na.d[ntable[cid]:ntable[cid] + 28])
        if shape != info['shape'][:24] + struct.pack('<I', cid):
            raise SystemExit('%s: shape %d is not the template shape' % (what, cid))
        geo = dict(ngeo)[cid]
        if check_quad(geo, '%s geometry %d' % (what, cid)) != len(opages) + 1 + j or geo != on_page(info['geo'], len(opages) + 1 + j):
            raise SystemExit('%s: shape %d is not the template quad on page %d' % (what, cid, len(opages) + 1 + j))
    return {'size': len(new), 'frames': len(nframes), 'characters': len(ntable), 'pages': len(npages),
            'new_labels': [label for label, _ in entries], 'container_realigned': info['realigned'],
            'placement': 'depth %d, scale %.3f at %.2f/%.2f' % ((cfg['depth'],) + info['placed'])}


# ---------------------------------------------------------------- review

def strip_triangles(body):
    """Triangles of every sub-record of a geometry (vertices form a strip)."""
    tris = []
    for sub in ax.sub_records(body):
        nv = struct.unpack_from('<I', sub, 60)[0]
        v = struct.unpack_from('<%df' % (2 * nv), sub, 64)
        pts = list(zip(v[0::2], v[1::2]))
        tris += [pts[i:i + 3] for i in range(nv - 2)]
    return tris


def resolve(apt, geos, cid, m, depth=0):
    """Triangles a character draws, in the parent's space (scale and translate only)."""
    o = apt.char_table()[cid]
    t = apt.type_of(o)
    if t == 1:
        a, d, tx, ty = m
        return [[(a * x + tx, d * y + ty) for x, y in tri] for tri in strip_triangles(geos[apt.u32(o + 24)])]
    if t != 5 or depth > 4:
        raise SystemExit('mask character %d is neither a shape nor a sprite' % cid)
    out = []
    for typ, io in frame_items(apt, apt.frames_of(o)[0]):
        if typ == 3 and apt.u32(io + 4) & 2:
            a, b, c, d, tx, ty = struct.unpack_from('<6f', apt.d, io + 16) if apt.u32(io + 4) & 4 else (1, 0, 0, 1, 0, 0)
            if b or c:
                raise SystemExit('rotated mask parts are not drawn')
            out += resolve(apt, geos, apt.i32(io + 12), (m[0] * a, m[1] * d, m[0] * tx + m[2], m[1] * ty + m[3]), depth + 1)
    return out


def mask_of(apt, geos, sprite, depth):
    """The clip layer over the picture depth in the sprite's frames."""
    for fr in apt.frames_of(sprite):
        for typ, io in frame_items(apt, fr):
            if typ == 3 and apt.u32(io + 4) & CLIP_FLAG and apt.i32(io + 8) < depth <= apt.i32(io + 56):
                a, b, c, d, tx, ty = struct.unpack_from('<6f', apt.d, io + 16)
                return apt.i32(io + 12), resolve(apt, geos, apt.i32(io + 12), (a, d, tx, ty))
    raise SystemExit('no clip layer over depth %d' % depth)


def review(apx, cfg, labels, notes, path):
    """Each new frame's page (safe area outlined) and the picture as placed under the mask."""
    chunks = ax.parse_apx(apx)
    apt = open_apt(chunks)
    geos = dict(ax.parse_rubi(chunks[ax.chunk(chunks, b'rubi')][1]))
    _, pages, _ = read_pages(chunks[ax.chunk(chunks, b'tx4h')][1])
    datas = [b for t, b in chunks if t == b'tx4t']
    sprite = apt.sprite(cfg['sprite'])
    mask_id, tris = mask_of(apt, geos, sprite, cfg['depth'])
    xs = [x for t in tris for x, _ in t]
    ys = [y for t in tris for _, y in t]
    rows = []
    for k, name, _, _ in apt.frame_labels(sprite):
        if name not in labels:
            continue
        place = [io for t, io in frame_items(apt, apt.frames_of(sprite)[k]) if t == 3][0]
        a, _, _, d, tx, ty = struct.unpack_from('<6f', apt.d, place + 16)
        shape = apt.char_table()[apt.i32(place + 12)]
        page, u0, v0, u1, v1, x0, y0, x1, y1 = ax.quad_info(geos[apt.u32(shape + 24)])
        p = pages[page - 1]
        img = dxt1_decode(datas[page - 1], p['w'], p['h']).convert('RGB')
        crop = img.crop((round(u0 * p['w']), round(v0 * p['h']), round(u1 * p['w']), round(v1 * p['h'])))
        rect = (a * x0 + tx, d * y0 + ty, a * x1 + tx, d * y1 + ty)
        sx, sy = (x1 - x0) / (crop.width or 1), (y1 - y0) / (crop.height or 1)
        safe = (a * (x0 + SAFE[0] * sx) + tx, d * (y0 + SAFE[1] * sy) + ty, a * (x0 + SAFE[2] * sx) + tx, d * (y0 + SAFE[3] * sy) + ty)
        rows.append((name, img, crop, rect, safe))
    if not rows:
        raise SystemExit('nothing to review')
    bx0 = min(min(xs), min(r[3][0] for r in rows)) - 6
    by0 = min(min(ys), min(r[3][1] for r in rows)) - 6
    bx1 = max(max(xs), max(r[3][2] for r in rows)) + 6
    by1 = max(max(ys), max(r[3][3] for r in rows)) + 6
    scale = 600 / (bx1 - bx0)
    pw, ph = 600, round((by1 - by0) * scale)

    def to_px(x, y):
        return (x - bx0) * scale, (y - by0) * scale
    mask = Image.new('L', (pw, ph), 0)
    md = ImageDraw.Draw(mask)
    for t in tris:
        md.polygon([to_px(x, y) for x, y in t], fill=255)
    edge = mask.filter(ImageFilter.FIND_EDGES).point(lambda v: 255 if v else 0)
    gap, top, head = 16, 22, 30
    rh = max(H, ph) + top + gap
    sheet = Image.new('RGB', (W + pw + 3 * gap, head + rh * len(rows)), (34, 34, 38))
    sd = ImageDraw.Draw(sheet)
    small = font(13, False)
    sd.text((gap, 8), 'page as stored (safe area yellow)', font=small, fill=(180, 180, 180))
    sd.text((W + 2 * gap, 8), 'as drawn: mask ch%d (cyan), clipped part dimmed, safe area (yellow) x %d..%d y %d..%d'
            % ((mask_id,) + SAFE[0::2] + SAFE[1::2]), font=small, fill=(180, 180, 180))
    for i, (name, img, crop, rect, safe) in enumerate(rows):
        y = head + i * rh
        sd.text((gap, y), '%s   %s' % (name, notes.get(name, '')), font=small, fill=(235, 235, 235))
        page = img.copy()
        ImageDraw.Draw(page).rectangle(SAFE, outline=(255, 214, 0))
        sheet.paste(page, (gap, y + top))
        canvas = Image.new('RGB', (pw, ph), (20, 20, 20))
        r0, r1 = to_px(rect[0], rect[1]), to_px(rect[2], rect[3])
        canvas.paste(crop.resize((max(1, round(r1[0] - r0[0])), max(1, round(r1[1] - r0[1]))), Image.LANCZOS), (round(r0[0]), round(r0[1])))
        dim = Image.blend(Image.new('RGB', (pw, ph), (20, 20, 20)), canvas, 0.25)
        canvas = Image.composite(canvas, dim, mask)
        canvas.paste((0, 200, 255), (0, 0), edge)
        s0, s1 = to_px(safe[0], safe[1]), to_px(safe[2], safe[3])
        ImageDraw.Draw(canvas).rectangle((s0, s1), outline=(255, 214, 0))
        sheet.paste(canvas, (W + 2 * gap, y + top))
    path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(path)
    return len(rows)


# ---------------------------------------------------------------- stream

def find_apx(assets, name):
    hits = [k for k, (shdr, _) in enumerate(assets) if fs.names(shdr) == (name, 'APX')]
    if len(hits) != 1:
        raise SystemExit('%s found %d times in the input' % (name, len(hits)))
    return hits[0]


def outbreak_pages(mp20):
    """MP20 pages 4/5 (2:1 photos squeezed into 256x256) stretched back to 512x256."""
    chunks = ax.parse_apx(mp20)
    _, pages, _ = read_pages(chunks[ax.chunk(chunks, b'tx4h')][1])
    datas = [b for t, b in chunks if t == b'tx4t']
    out, notes = {}, {}
    for label, no in OUTBREAK_PAGES.items():
        p = pages[no - 1]
        if p['values'][0] not in ('DXT1a', 'DXT1c') or (p['w'], p['h'], p['mips'], p['size']) != (256, 256, 1, 32768):
            raise SystemExit('%s page %d is %s %dx%d, not the fixed Outbreak picture' % (MP20, no, p['values'][0], p['w'], p['h']))
        img = dxt1_decode(datas[no - 1], 256, 256)
        clear = int((np.asarray(img)[..., 3] < 255).sum())
        flat = Image.new('RGBA', img.size, (0, 0, 0, 255))
        flat.alpha_composite(img)
        out[label] = flat.convert('RGB').resize((W, H), Image.LANCZOS)
        notes[label] = 'Outbreak picture, %s page %d stretched to 512x256%s' % (MP20, no, ', %d clear texels on black' % clear if clear else '')
    return out, notes


def build(data, arenas, pictures, numbered):
    assets = fs.group(fs.records(data))
    k20, k02, klib = find_apx(assets, MP20), find_apx(assets, MP02), find_apx(assets, LIBFRONTEND)
    movies = {k: fs.payload(assets[k][1], fs.declared(assets[k][0])) for k in (k20, k02, klib)}
    shape_tpl, geo_tpl = quad_template(movies[klib])

    images, notes, real = [], {}, []
    for i, (name, label) in enumerate(arenas):
        frame_label = 'map%d' % (FIRST_NUMBERED + i) if numbered else 'map' + name
        src = find_picture(pictures, name)
        if src:
            im, note = load_picture(src)
            notes[frame_label] = '%s (%s): %s, %s' % (name, label, src.name, note)
            real.append(name)
        else:
            im = placeholder(name, label)
            notes[frame_label] = '%s (%s): placeholder' % (name, label)
        images.append((frame_label, im))
    known = {n.lower() for n, _ in arenas}
    strays = sorted(p.name for p in pictures.glob('*') if p.suffix.lower() in PICTURE_EXTS and p.stem.lower() not in known) if pictures.is_dir() else []
    outbreak, onotes = outbreak_pages(movies[k20])
    notes.update(onotes)

    quality = {}

    def encode(items):
        out = []
        for label, im in items:
            page = dxt1_encode(im)
            quality[label] = round(float(psnr(im, dxt1_decode(page, W, H))), 2)
            out.append((label, page))
        return out
    arena_pages = encode(images)
    entries = {MP20: arena_pages, MP02: encode(list(outbreak.items())) + arena_pages}
    new, reports = {}, {}
    for name, k in ((MP20, k20), (MP02, k02)):
        cfg = MOVIES[name]
        apx, info = extend_movie(movies[k], cfg, entries[name], shape_tpl, geo_tpl, name)
        reports[name] = verify_movie(movies[k], apx, cfg, entries[name], info, name)
        reports[name]['declared_before'] = fs.declared(assets[k][0])
        new[k] = apx

    out_assets = [(fs.with_declared(shdr, len(new[k])), fs.rpak(new[k])) if k in new else (shdr, bodies)
                  for k, (shdr, bodies) in enumerate(assets)]
    stream = fs.frame(out_assets)
    records = fs.validate(stream)
    if len(stream) % fs.BLOCK:
        raise SystemExit('stream size is not a multiple of 0x20000')
    back = fs.group(fs.records(stream))
    if len(back) != len(assets):
        raise SystemExit('asset count changed on read back')
    for k, ((s0, b0), (s1, b1)) in enumerate(zip(assets, back)):
        if k in new:
            if s1 != fs.with_declared(s0, len(new[k])) or fs.declared(s1) != len(new[k]):
                raise SystemExit('%s: header differs beyond the declared size' % fs.names(s0)[0])
            if fs.payload(b1, fs.declared(s1)) != new[k]:
                raise SystemExit('%s differs on read back' % fs.names(s0)[0])
        elif s0 != s1 or b0 != b1:
            raise SystemExit('asset %d (%s) changed' % (k, fs.names(s0)[0]))
    for name, k in ((MP20, k20), (MP02, k02)):
        reports[name]['declared'] = fs.declared(back[k][0])
    return stream, {'movies': reports, 'real': real, 'notes': notes, 'quality': quality, 'strays': strays,
                    'records': records, 'assets': len(back), 'apx': {MP20: new[k20], MP02: new[k02]},
                    'labels': {MP20: [lb for lb, _ in entries[MP20]], MP02: [lb for lb, _ in entries[MP02]]}}


# ---------------------------------------------------------------- self test

def self_test(data):
    rng = np.random.default_rng(7)
    samples = [b'', b'a', b'ab', b'abc', b'abcd' * 3, bytes(rng.integers(0, 256, 5000, np.uint8)),
               bytes(70000) + b'xyz' * 20000, bytes(rng.integers(0, 4, 129024, np.uint8))]
    for s in samples:
        if refpack_decompress(refpack_compress(s)) != s:
            raise SystemExit('self test: RefPack round trip failed for %d bytes' % len(s))
    pieces = 0
    for shdr, bodies in fs.group(fs.records(data)):
        if fs.names(shdr)[1] == 'APX':
            fs.payload(bodies, fs.declared(shdr))
            pieces += sum(1 for b in bodies if b[:4] == b'kapR')
    gx, gy = np.meshgrid(np.linspace(0, 255, W), np.linspace(0, 255, H))
    grad = Image.fromarray(np.dstack([gx, gy, np.full((H, W), 90.0)]).astype(np.uint8))
    page = dxt1_encode(grad)
    dec = dxt1_decode(page, W, H)
    if len(page) != PAGE_SIZE or dec.getchannel('A').getextrema()[0] < 255 or psnr(grad, dec) < 35:
        raise SystemExit('self test: DXT1 encoder')
    if zlib.crc32(b'ui_mp20gamesettings4') != 0xDF6418C9:
        raise SystemExit('self test: page id')
    print('self test passed: RefPack %d samples, %d input pieces decoded, DXT1 %.1f dB' % (len(samples), pieces, psnr(grad, dec)))


# ---------------------------------------------------------------- main

def inside(path, folder):
    try:
        path.resolve().relative_to(Path(folder).resolve())
        return True
    except ValueError:
        return False


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_bytes(data)
    tmp.replace(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--in', dest='src', default=str(Path(GAME_DIR) / 'randomizer' / 'ds2coop_frontend.str'))
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--out', default=str(ROOT / 'build' / 'frontend_final6' / 'ds2coop_frontend.str'))
    ap.add_argument('--pictures', default=str(ROOT / 'arenas' / 'pictures'))
    ap.add_argument('--arenas', default=str(ROOT / 'src' / 'generated_arenas.h'))
    ap.add_argument('--review', default=None)
    ap.add_argument('--numbered', action='store_true')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()

    src, out = Path(args.src), Path(args.out)
    if inside(out, GAME_DIR):
        raise SystemExit('--out %s is inside the game folder; build elsewhere and install separately' % out)
    data = src.read_bytes()
    crc = zlib.crc32(data)
    if crc != INPUT_CRC and not args.force:
        raise SystemExit('%s has CRC %08X, not final5 (%08X); pass --force to use it anyway' % (src, crc, INPUT_CRC))
    if args.self_test:
        self_test(data)
        return
    arenas = read_arenas(args.arenas)
    stream, rep = build(data, arenas, Path(args.pictures), args.numbered)

    review_dir = Path(args.review) if args.review else (None if args.check else ROOT / 'build' / 'map_pictures_review')
    sheets = {}
    if review_dir:
        for name in (MP20, MP02):
            path = review_dir / ('%s_review.png' % MOVIES[name]['key'])
            review(rep['apx'][name], MOVIES[name], set(rep['labels'][name]), rep['notes'], path)
            sheets[MOVIES[name]['key']] = str(path)
    if not args.check:
        write(out, stream)

    for name, label in arenas:
        key = 'map%d' % (FIRST_NUMBERED + arenas.index((name, label))) if args.numbered else 'map' + name
        print('%-16s %-22s %s  (DXT1 %.1f dB)' % (key, name, 'picture' if name in rep['real'] else 'PLACEHOLDER', rep['quality'][key]))
    for label in OUTBREAK_PAGES:
        print('%-16s %s  (DXT1 %.1f dB)' % (label, rep['notes'][label], rep['quality'][label]))
    if rep['strays']:
        print('pictures without an arena (ignored): %s' % ', '.join(rep['strays']))
    m20, m02 = rep['movies'][MP20], rep['movies'][MP02]
    print('%s declared %d -> %d bytes (the plugin checks this size)' % (MP20, m20['declared_before'], m20['declared']))
    print('%s declared %d -> %d bytes' % (MP02, m02['declared_before'], m02['declared']))
    summary = {
        'in': str(src), 'in_crc32': '%08X' % crc, 'out': str(out), 'written': not args.check,
        'size': len(stream), 'crc32': '%08X' % zlib.crc32(stream), 'records': rep['records'], 'assets': rep['assets'],
        'arenas': len(arenas), 'real_pictures': rep['real'], 'placeholders': [n for n, _ in arenas if n not in rep['real']],
        'labels': 'numbered' if args.numbered else 'by name', 'mp20': m20, 'mp02': m02, 'review': sheets,
    }
    print(json.dumps(summary, indent=1))


if __name__ == '__main__':
    main()
