"""Recolour the Apt movies of a built frontend stream (ds2coop_frontend.str).

Only colour values change. Every movie keeps its size and layout; the bytes that differ are
the colour fields listed in the change file. The stream is framed again with the changed
movies recompressed, every other record is copied as it is.

Pass 1, always: MP20GameSettings back to the stock colours. Mark's copy of the movie carries
his orange in four EditText colours, two flat geometry colours and the colour endpoints of
texture pages 1-3 (alpha blocks and colour indices are the stock ones). The stock values come
from the stock movie in DS2DAT6.DAT; his game mode label (character 79) gets the stock colour
of the map label. Pages 4 and 5 are his map pictures and stay.

Pass 2, with --hue or --rgb: the blue theme of every movie turns to another hue. Colours
within the theme band (stock hue 206, full weight up to --band degrees away, fading out over
20 more) move, other colours (red, yellow and green art, greys) stay. EditText colours, flat
geometry colours, PlaceObject colour transforms and the colour endpoints of DXT1/DXT5 pages
follow; CONTENT_PAGES (suit and gun renders, screenshots, controller art) stay. Not covered:
the YCoCg/L8 pages of the chapter select, the 3D background, VFX and colours set by code.

Additive colours of the PlaceObject colour transforms follow the hue too. They carry the
blue of row and description texts (multiply 0, add blue) and of hover fills and flashes;
--text-white turns the text tints grey (the text reads white), --highlight gives the others
a colour of their own (the hover fill of SharedLibPC's buttons, menu row glows).

    recolor_frontend.py [--game DIR] [--in FILE] [--out FILE] [--hue DEG | --rgb RRGGBB]
                        [--band DEG] [--text-white] [--highlight RRGGBB] [--changes FILE] [--check]

--in       built stream with Mark's MP20 on the stock libraries (build_frontend_override.py
           or build_loadout_frontend.py with --stock-colours); default build/frontend_stock
--out      default build/frontend_blue/ds2coop_frontend.str, build/frontend_hue/... with --hue/--rgb
--hue DEG  rotate the theme hue by DEG degrees (-80 = green, 180 = orange)
--rgb HEX  turn the theme colour #3C7BB5 into this colour (hue and saturation, brightness kept)
--text-white   additive text tints of the theme band become grey of the same brightness
--highlight HEX  additive highlight colours of the theme band (hover fills, glows) take this colour
--changes  change list (TSV), default next to --out
--check    build and verify in memory, write nothing
"""
import argparse
import colorsys
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from ds2_paths import GAME_DIR  # noqa: E402
import apx_edit as ax  # noqa: E402
import build_frontend_override as fo  # noqa: E402
import frontend_str as fs  # noqa: E402

MP20 = fo.MOVIE
# EditText character: (Mark's BGRA, stock BGRA, the stock character the colour comes from)
MP20_TEXT = {46: ('3e7bc6ff', 'c6b087ff', 46),   # "game map" label
             60: ('4e9bfaff', 'fae6bfff', 60),   # map description
             68: ('4e9bfaff', 'fae6bfff', 68),   # screen title
             79: ('3e7bc6ff', 'c6b087ff', 46)}   # Mark's "game mode" label
MP20_GEOMETRY = (1, 62)
MP20_PAGES = (1, 2, 3)
MARK_LIBS = ('libfrontemp.apx', 'libsharmp.apx', 'sharedlibmp.apx')

THEME_HUE = 206.0
THEME_RGB = (60, 123, 181)   # median of the theme pixels on the stock chrome pages
MIN_SAT = 0.05
FADE = 20.0
DXT = {'DXT5': 16, 'DXT1a': 8, 'DXT1c': 8}
CONTENT_PAGES = {
    'fe11setupweapons.apx': {3},                 # gun icons added by build_loadout_frontend
    'fe12setupsuit.apx': 'from 2',               # suit renders
    'fe22unlocks.apx': {1},                      # screenshots
    'libfrontend.apx': {1, 5, 6},                # controller art, screenshots
    'mp19endofmatchrewards.apx': {2, 3},         # suit and gun renders
    'mp20gamesettings.apx': {4, 5},              # Mark's map pictures
    'platform.apx': 'all', 'platform_720p.apx': 'all',   # controller glyphs, EA logo
}
# Pixel areas of mixed pages that keep their colours (x0, y0, x1, y1): the chapter pictures of
# the save slot screen share their pages with a little chrome.
CONTENT_REGIONS = {
    'sp11saveslot.apx': {1: [(0, 0, 2048, 60), (236, 0, 2048, 128)], 2: [(0, 0, 272, 72)]},
}
# Movies whose additive tints of the theme band turn grey instead of taking the highlight:
# they dim the chapter pictures of unselected rows, which should not change colour.
NEUTRAL_TINTS = {'sp11saveslot.apx', 'sp03chapterselect.apx', 'sp04dlcselect.apx'}


# ---------------------------------------------------------------- movie layout

def apx_chunks(data):
    """[(tag, data offset, size)] of an apx container, alig chunks included."""
    if data[:4][::-1] != b'apxf' or struct.unpack_from('<I', data, 4)[0] != len(data):
        raise ValueError('not an apx container')
    out, off = [], 8
    while off < len(data):
        tag, size = data[off:off + 4][::-1], struct.unpack_from('<I', data, off + 4)[0]
        if size < 8 or off + size > len(data):
            raise ValueError('bad chunk at %#x' % off)
        out.append((tag, off + 8, size - 8))
        off += size
    return out


def page_table(tx):
    """[(format, w, h, mips, size)] of a tx4h body; the format string hangs off the second block."""
    _, ver, count, _, table_off, tag = struct.unpack_from('<IHHIII', tx, 0)
    if ver != 4 or tag != ax.TX_SET_TAG:
        raise ValueError('unexpected tx4h header')
    out = []
    for i in range(count):
        _, size, w, h, _, _, mips, _, _, so = struct.unpack_from('<IIHHBBBBII', tx, table_off + 24 * i)
        f = struct.unpack_from('<I', tx, so + 1)[0]
        out.append((tx[f:tx.index(b'\0', f)].decode('ascii'), w, h, mips, size))
    return out


def dxt_size(w, h, mips, block):
    total = 0
    for level in range(max(1, mips)):
        lw, lh = max(1, w >> level), max(1, h >> level)
        total += ((lw + 3) // 4) * ((lh + 3) // 4) * block
    return total


class Movie:
    """Colour fields of one apx payload, as absolute offsets into it."""

    def __init__(self, name, data):
        self.name = name
        self.data = bytes(data)
        chunks = apx_chunks(self.data)
        one = {}
        for tag, off, size in chunks:
            if tag in (b'apti', b'cons', b'rubi', b'tx4h'):
                if tag in one:
                    raise ValueError('%s: %r twice' % (name, tag))
                one[tag] = (off, size)
        self.texts, self.places, self.backgrounds, self.geometry, self.pages, self.skipped = {}, [], [], {}, [], []
        self.adds = []      # (offset of the additive colour, 'text' when it only tints text)
        if b'apti' in one:
            self._apt(one)
        if b'rubi' in one:
            self._rubi(*one[b'rubi'])
        if b'tx4h' in one:
            self._pages(chunks, *one[b'tx4h'])

    def body(self, at):
        off, size = at
        return self.data[off:off + size]

    def _apt(self, one):
        base = one[b'apti'][0]
        apt = ax.Apt(self.body(one[b'apti']), self.body(one[b'cons']))
        self.apt = apt
        table = apt.char_table()
        for i, o in enumerate(table):
            if o and apt.type_of(o) == 2:
                self.texts[i] = base + o + 32
        owners = [apt.movie] + [o for o in table if o and apt.type_of(o) == 5]
        for o in owners:
            held = {}   # depth -> character, for places that only move or recolour
            for fr in apt.frames_of(o):
                for io in apt.items_of(fr):
                    if apt.u32(io) == 3:
                        flags, depth = apt.u32(io + 4), apt.i32(io + 8)
                        if flags & 2:
                            held[depth] = apt.i32(io + 12)
                        if flags & 8:
                            # colour transform: multiply BGRA at +40, add BGRA at +44
                            self.places.append(base + io + 40)
                            kinds = self._content(apt, table, held.get(depth, -1), ())
                            self.adds.append((base + io + 44, 'text' if kinds == {'text'} else 'other'))
                    elif apt.u32(io) == 5:
                        self.backgrounds.append(base + io + 4)

    def _content(self, apt, table, cid, seen):
        """What a character draws in the end: text, shape, import, ..."""
        if cid < 0 or cid >= len(table) or not table[cid]:
            return {'import'}
        o = table[cid]
        t = apt.type_of(o)
        if t == 2:
            return {'text'}
        if t != 5 or cid in seen:
            return {'shape' if t == 1 else 'other'}
        out = set()
        for fr in apt.frames_of(o):
            for io in apt.items_of(fr):
                if apt.u32(io) == 3 and apt.u32(io + 4) & 2:
                    out |= self._content(apt, table, apt.i32(io + 12), seen + (cid,))
        return out or {'empty'}

    def _rubi(self, off, size):
        p, end = off, off + size
        while p + 8 <= end:
            gid, rsize = struct.unpack_from('<II', self.data, p)
            q, subs = p + 8, []
            while q < p + rsize:
                ssize = struct.unpack_from('<I', self.data, q)[0]
                if ssize < 64 or q + ssize > p + rsize:
                    raise ValueError('%s: bad geometry record %d' % (self.name, gid))
                subs.append((q + 4, struct.unpack_from('<I', self.data, q + 52)[0]))
                q += ssize
            self.geometry[gid] = subs
            p += rsize
        if p != end:
            raise ValueError('%s: rubi has stray bytes' % self.name)

    def _pages(self, chunks, off, size):
        table = page_table(self.data[off:off + size])
        # tyuv chunks carry the YCoCg pages, tx4t the others, each in page order
        tx = [(o, s) for t, o, s in chunks if t == b'tx4t']
        yuv = [(o, s) for t, o, s in chunks if t == b'tyuv']
        plain = [k for k, p in enumerate(table) if p[0] != 'DXT5_YCOCG']
        if len(plain) != len(tx) or len(table) - len(plain) != len(yuv):
            raise ValueError('%s: %d pages, %d tx4t, %d tyuv' % (self.name, len(table), len(tx), len(yuv)))
        for k, (o, s) in zip(plain, tx):
            fmt, w, h, mips, want = table[k]
            if s != want:
                raise ValueError('%s: page %d holds %d bytes, declares %d' % (self.name, k + 1, s, want))
            if fmt in DXT and dxt_size(w, h, mips, DXT[fmt]) == s:
                self.pages.append({'no': k + 1, 'format': fmt, 'w': w, 'h': h, 'mips': mips, 'off': o, 'size': s})
            else:
                self.skipped.append('page %d %s %dx%d' % (k + 1, fmt, w, h))
        for k, p in enumerate(table):
            if p[0] == 'DXT5_YCOCG':
                self.skipped.append('page %d %s %dx%d' % (k + 1, p[0], p[1], p[2]))

    def page(self, no):
        hits = [p for p in self.pages if p['no'] == no]
        if len(hits) != 1:
            raise ValueError('%s: page %d is not a DXT page' % (self.name, no))
        return hits[0]


# ---------------------------------------------------------------- colours

def rgb565(v):
    r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
    return (r << 3 | r >> 2) / 255, (g << 2 | g >> 4) / 255, (b << 3 | b >> 2) / 255


def to565(rgb):
    r, g, b = (min(1.0, max(0.0, c)) for c in rgb)
    return round(r * 31) << 11 | round(g * 63) << 5 | round(b * 31)


def band_weight(h, band):
    d = abs((h - THEME_HUE + 180) % 360 - 180)
    if d <= band:
        return 1.0
    return max(0.0, 1 - (d - band) / FADE)


class Tints:
    """Additive colours (PlaceObject +44) of the theme band. The movies use them for
    text tints (row and description text: mul 0 + add) and for highlights (hover
    fills, flashes). Text tints turn grey at the same brightness, so the text reads
    white; the others take the highlight colour, scaled by their own brightness."""

    def __init__(self, shift, band, text_white, highlight):
        self.shift, self.band, self.text_white = shift, band, text_white
        self.highlight = colorsys.rgb_to_hsv(*highlight) if highlight else None

    def __call__(self, raw, cls):
        b, g, r, a = raw
        h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
        if s < MIN_SAT or v <= 0 or not band_weight(h * 360, self.band):
            return bytes(raw)
        if (cls == 'text' and self.text_white) or cls == 'neutral':
            new = (v, v, v)
        elif cls != 'text' and self.highlight:
            new = colorsys.hsv_to_rgb(self.highlight[0], self.highlight[1], v * self.highlight[2])
        elif self.shift:
            return self.shift.bgra(raw)
        else:
            return bytes(raw)
        r2, g2, b2 = (round(c * 255) for c in new)
        return bytes((b2, g2, r2, a))


class Shift:
    """Band limited hue rotation (HSV): brightness and alpha stay, greys stay."""

    def __init__(self, degrees, saturation=1.0, band=30.0):
        self.degrees, self.saturation, self.band = degrees, saturation, band
        lut = np.arange(65536, dtype=np.uint16)
        for v in range(65536):
            rgb = rgb565(v)
            new = self(rgb)
            if new != rgb:
                lut[v] = to565(new)
        self.lut = lut

    def weight(self, h):
        return band_weight(h, self.band)

    def __call__(self, rgb):
        h, s, v = colorsys.rgb_to_hsv(*rgb)
        if s < MIN_SAT or v <= 0:
            return tuple(rgb)
        w = self.weight(h * 360)
        if not w:
            return tuple(rgb)
        h = (h * 360 + w * self.degrees) % 360 / 360
        s = min(1.0, s * (1 + w * (self.saturation - 1)))
        return colorsys.hsv_to_rgb(h, s, v)

    def bgra(self, raw):
        b, g, r, a = raw
        new = self((r / 255, g / 255, b / 255))
        r2, g2, b2 = (round(c * 255) for c in new)
        return bytes((b2, g2, r2, a))


def index_swap(idx, four):
    """Indices of a DXT colour block after its two endpoints swap places."""
    if four:
        return idx ^ 0x55555555
    hi = (idx >> 1) & 0x55555555
    return idx ^ (~hi & 0x55555555)


# ---------------------------------------------------------------- edits

class Edit:
    def __init__(self, movie):
        self.movie = movie
        self.out = bytearray(movie.data)
        self.rows = []      # (kind, offset, detail, old, new)
        self.ranges = []    # (offset, length) of every field written

    def put(self, kind, off, new, detail, log=True):
        old = bytes(self.out[off:off + len(new)])
        if old == new:
            return False
        self.out[off:off + len(new)] = new
        self.ranges.append((off, len(new)))
        if log:
            self.rows.append((kind, off, detail, old.hex(), new.hex()))
        return True


def restore_mp20(edit, stock):
    """Mark's orange in MP20 back to the stock values; returns the number of fields written."""
    m = edit.movie
    n = 0
    for cid, (mark, want, source) in MP20_TEXT.items():
        off = m.texts.get(cid)
        if off is None or m.data[off:off + 4].hex() != mark:
            raise SystemExit('MP20 EditText %d is not the colour this was made for' % cid)
        new = stock.data[stock.texts[source]:stock.texts[source] + 4]
        if new.hex() != want:
            raise SystemExit('stock MP20 EditText %d is %s, expected %s' % (source, new.hex(), want))
        n += edit.put('text', off, new, 'EditText %d BGRA' % cid)
    for gid in MP20_GEOMETRY:
        subs, ssubs = m.geometry[gid], stock.geometry[gid]
        if len(subs) != 1 or len(ssubs) != 1 or subs[0][1] or ssubs[0][1]:
            raise SystemExit('MP20 geometry %d is not one flat sub-record' % gid)
        new = stock.data[ssubs[0][0]:ssubs[0][0] + 16]
        n += edit.put('geometry', subs[0][0], new, 'geometry %d RGBA float' % gid)
    for no in MP20_PAGES:
        p, s = m.page(no), stock.page(no)
        if (p['format'], p['w'], p['h'], p['size']) != (s['format'], s['w'], s['h'], s['size']):
            raise SystemExit('MP20 page %d differs from the stock page in layout' % no)
        block = DXT[p['format']]
        for k in range(p['size'] // block):
            c = p['off'] + k * block + block - 8
            sc = s['off'] + k * block + block - 8
            if block == 16 and m.data[c - 8:c] != stock.data[sc - 8:sc]:
                raise SystemExit('MP20 page %d block %d: alpha differs from stock' % (no, k))
            if m.data[c + 4:c + 8] != stock.data[sc + 4:sc + 8]:
                raise SystemExit('MP20 page %d block %d: colour indices differ from stock' % (no, k))
            n += edit.put('dxt', c, stock.data[sc:sc + 4], 'page %d block %d endpoints' % (no, k))
    # what is left must match the stock movie field for field
    for cid, off in m.texts.items():
        if cid in stock.texts and edit.out[off:off + 4] != stock.data[stock.texts[cid]:stock.texts[cid] + 4]:
            raise SystemExit('MP20 EditText %d still differs from stock' % cid)
    for gid, subs in m.geometry.items():
        if gid in stock.geometry:
            for (o, _), (so, _) in zip(subs, stock.geometry[gid]):
                if edit.out[o:o + 16] != stock.data[so:so + 16]:
                    raise SystemExit('MP20 geometry %d still differs from stock' % gid)
    for no in MP20_PAGES:
        p, s = m.page(no), stock.page(no)
        if edit.out[p['off']:p['off'] + p['size']] != stock.data[s['off']:s['off'] + s['size']]:
            raise SystemExit('MP20 page %d still differs from stock' % no)
    return n


def content(name, no):
    rule = CONTENT_PAGES.get(name)
    if rule == 'all':
        return True
    if rule == 'from 2':
        return no >= 2
    return bool(rule) and no in rule


def tint_movie(edit, tints):
    """Additive colours; the controller glyph movies keep theirs."""
    m = edit.movie
    if CONTENT_PAGES.get(m.name) == 'all':
        return
    neutral = m.name in NEUTRAL_TINTS
    for off, cls in m.adds:
        edit.put('cxform add', off, tints(edit.out[off:off + 4], 'neutral' if neutral else cls), 'PlaceObject add BGRA (%s)' % cls)


def shift_movie(edit, shift, summary):
    m = edit.movie
    for cid, off in sorted(m.texts.items()):
        edit.put('text', off, shift.bgra(edit.out[off:off + 4]), 'EditText %d BGRA' % cid)
    for off in m.places:
        edit.put('cxform', off, shift.bgra(edit.out[off:off + 4]), 'PlaceObject colour BGRA')
    for off in m.backgrounds:
        edit.put('background', off, shift.bgra(edit.out[off:off + 4]), 'background colour')
    for gid, subs in sorted(m.geometry.items()):
        for k, (off, page) in enumerate(subs):
            r, g, b, a = struct.unpack_from('<4f', edit.out, off)
            new = shift((r, g, b))
            if new != (r, g, b):
                packed = struct.pack('<4f', *new, a)
                if struct.unpack('<4f', packed) != (r, g, b, a):
                    edit.put('geometry', off, packed, 'geometry %d sub %d RGBA float' % (gid, k))
    for p in m.pages:
        if content(m.name, p['no']):
            summary.append((m.name, p['no'], p['format'], 'content page, kept'))
            continue
        block = DXT[p['format']]
        n = p['size'] // block
        raw = np.frombuffer(bytes(edit.out[p['off']:p['off'] + p['size']]), dtype=np.uint8).reshape(n, block)
        words = raw[:, block - 8:].copy().view('<u2').reshape(n, 4)
        c0, c1 = words[:, 0].copy(), words[:, 1].copy()
        idx = raw[:, block - 4:].copy().view('<u4').reshape(n)
        n0, n1 = shift.lut[c0], shift.lut[c1]
        if block == 8:
            # DXT1: keep each block in its mode (c0 > c1 four colours, else three and transparent)
            four = c0 > c1
            swap = (four & (n0 < n1)) | (~four & (n0 > n1))
            a, b = np.where(swap, n1, n0), np.where(swap, n0, n1)
            idx = np.where(swap & four, index_swap(idx, True), np.where(swap & ~four, index_swap(idx, False), idx))
            flat = four & (a == b)
            m3 = (idx & (idx >> 1)) & 0x55555555
            idx = np.where(flat, idx & ~(m3 | (m3 << 1)), idx)
            n0, n1 = a, b
        changed = (n0 != c0) | (n1 != c1)
        words[:, 0], words[:, 1] = n0, n1
        new = raw.copy()
        new[:, block - 8:block - 4] = words[:, :2].copy().view(np.uint8).reshape(n, 4)
        new[:, block - 4:] = idx.astype('<u4').view(np.uint8).reshape(n, 4)
        keep = CONTENT_REGIONS.get(m.name, {}).get(p['no'], [])
        if keep:
            if p['mips'] > 1:
                raise SystemExit('%s page %d: content regions need a single level' % (m.name, p['no']))
            bw = (p['w'] + 3) // 4
            ks = np.arange(n)
            bx, by = (ks % bw) * 4, (ks // bw) * 4
            inside = np.zeros(n, dtype=bool)
            for x0, y0, x1, y1 in keep:
                inside |= (bx >= x0) & (bx + 4 <= x1) & (by >= y0) & (by + 4 <= y1)
            changed &= ~inside
        for k in np.nonzero(changed)[0]:
            at = p['off'] + int(k) * block + block - 8
            edit.put('dxt', at, new[k, block - 8:].tobytes(), '', log=False)
        summary.append((m.name, p['no'], p['format'], '%d of %d blocks%s' % (int(changed.sum()), n, ', picture areas kept' if keep else '')))


# ---------------------------------------------------------------- checks

def verify_movie(edit):
    """Same container, same characters and frames, and only the written fields differ."""
    old, new = edit.movie.data, bytes(edit.out)
    if len(old) != len(new):
        raise SystemExit('%s changed size' % edit.movie.name)
    if [(t, o, s) for t, o, s in apx_chunks(old)] != [(t, o, s) for t, o, s in apx_chunks(new)]:
        raise SystemExit('%s: chunk layout changed' % edit.movie.name)
    allowed = np.zeros(len(old), dtype=bool)
    for off, ln in edit.ranges:
        allowed[off:off + ln] = True
    diff = np.frombuffer(old, np.uint8) != np.frombuffer(new, np.uint8)
    stray = np.nonzero(diff & ~allowed)[0]
    if len(stray):
        raise SystemExit('%s: %d bytes changed outside colour fields, first at %#x' % (edit.movie.name, len(stray), stray[0]))
    again = Movie(edit.movie.name, new)
    if hasattr(edit.movie, 'apt'):
        geos = set(again.geometry)
        if sorted(edit.movie.apt.check(set(edit.movie.geometry))) != sorted(again.apt.check(geos)):
            raise SystemExit('%s: movie check differs after the edit' % edit.movie.name)
    if {g: [p for _, p in s] for g, s in edit.movie.geometry.items()} != {g: [p for _, p in s] for g, s in again.geometry.items()}:
        raise SystemExit('%s: geometry changed' % edit.movie.name)
    return int(diff.sum())


# ---------------------------------------------------------------- main

def recolor(data, stock_frontend, shift=None, tints=None):
    """New stream bytes, change rows and the page summary."""
    assets = fs.group(fs.records(data))
    by_name = {}
    for k, (shdr, _) in enumerate(assets):
        base, kind = fs.names(shdr)
        if kind == 'APX':
            if base in by_name:
                raise SystemExit('%s found twice' % base)
            by_name[base] = k
    present = [n for n in MARK_LIBS if n in by_name]
    if present:
        raise SystemExit('the input carries Mark\'s orange libraries (%s); build it with --stock-colours' % ', '.join(present))
    if MP20 not in by_name:
        raise SystemExit('the input holds no %s' % MP20)
    if hashlib.sha256(stock_frontend).hexdigest() != fo.STOCK_SHA:
        raise SystemExit('the stock frontend.str is not the Steam retail file')
    stock = Movie(MP20, fs.find_assets(stock_frontend, {(MP20, 'APX')})[(MP20, 'APX')])

    rows, summary, new_payloads, stats = [], [], {}, {}
    for base, k in sorted(by_name.items()):
        shdr, bodies = assets[k]
        movie = Movie(base, fs.payload(bodies, fs.declared(shdr)))
        edit = Edit(movie)
        if base == MP20:
            sha = hashlib.sha256(movie.data).hexdigest()
            if sha != fo.MOVIE_SHA:
                raise SystemExit("the input's MP20 is not Mark's movie on the stock libraries (sha %s)" % sha[:16])
            restore_mp20(edit, stock)
            rows.extend(('mp20 stock', base) + r for r in edit.rows)
            stock_fields = len(edit.ranges)
        if shift is not None:
            before = len(edit.rows)
            shift_movie(edit, shift, summary)
            rows.extend(('hue', base) + r for r in edit.rows[before:])
        if tints is not None:
            before = len(edit.rows)
            tint_movie(edit, tints)
            rows.extend(('tint', base) + r for r in edit.rows[before:])
        if movie.skipped:
            summary.extend((base, s, '', 'not handled') for s in movie.skipped)
        if edit.ranges:
            changed = verify_movie(edit)
            new_payloads[k] = bytes(edit.out)
            stats[base] = {'fields': len(edit.ranges), 'bytes': changed}
            if base == MP20:
                stats[base]['mp20_stock_fields'] = stock_fields

    out_assets = []
    for k, (shdr, bodies) in enumerate(assets):
        if k in new_payloads:
            out_assets.append((shdr, fs.rpak(new_payloads[k])))
        else:
            out_assets.append((shdr, bodies))
    out = fs.frame(out_assets)
    count = fs.validate(out)
    back = fs.group(fs.records(out))
    if len(back) != len(assets):
        raise SystemExit('asset count changed on read back')
    for k, ((s0, b0), (s1, b1)) in enumerate(zip(assets, back)):
        if s0 != s1:
            raise SystemExit('asset header %d changed' % k)
        if k in new_payloads:
            if fs.payload(b1, fs.declared(s1)) != new_payloads[k]:
                raise SystemExit('%s differs on read back' % fs.names(s1)[0])
        elif b0 != b1:
            raise SystemExit('asset %d (%s) changed' % (k, fs.names(s0)[0]))
    return out, rows, summary, {'records': count, 'assets': len(back), 'movies': stats}


def parse_rgb(text, option):
    t = text.lstrip('#')
    if len(t) != 6:
        raise SystemExit('%s takes RRGGBB' % option)
    return tuple(int(t[i:i + 2], 16) / 255 for i in (0, 2, 4))


def tints_from_args(args, shift):
    if not args.text_white and not args.highlight:
        return None, 'none'
    highlight = parse_rgb(args.highlight, '--highlight') if args.highlight else None
    if highlight and colorsys.rgb_to_hsv(*highlight)[1] < MIN_SAT:
        raise SystemExit('--highlight %s has no hue' % args.highlight)
    label = ', '.join(x for x in ('text tints white' if args.text_white else '',
                                  'highlights %s' % args.highlight.lstrip('#') if highlight else '') if x)
    return Tints(shift, args.band, args.text_white, highlight), label


def shift_from_args(args):
    if args.hue is not None:
        return Shift(args.hue, 1.0, args.band), 'hue %+g' % args.hue
    if args.rgb:
        t = args.rgb.lstrip('#')
        rgb = parse_rgb(t, '--rgb')
        h, s, _ = colorsys.rgb_to_hsv(*rgb)
        _, s0, _ = colorsys.rgb_to_hsv(*(c / 255 for c in THEME_RGB))
        if s < MIN_SAT:
            raise SystemExit('--rgb %s has no hue' % t)
        degrees = (h * 360 - THEME_HUE + 180) % 360 - 180
        return Shift(degrees, min(1.5, s / s0), args.band), 'rgb %s (hue %+.1f, saturation x%.2f)' % (t, degrees, s / s0)
    return None, 'none'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=str(GAME_DIR))
    ap.add_argument('--in', dest='src', default=str(ROOT / 'build' / 'frontend_stock' / 'ds2coop_frontend.str'))
    ap.add_argument('--out', default=None)
    group = ap.add_mutually_exclusive_group()
    group.add_argument('--hue', type=float, default=None)
    group.add_argument('--rgb', default=None)
    ap.add_argument('--band', type=float, default=30.0)
    ap.add_argument('--text-white', action='store_true')
    ap.add_argument('--highlight', default=None)
    ap.add_argument('--changes', default=None)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()

    shift, label = shift_from_args(args)
    tints, tint_label = tints_from_args(args, shift)
    if tints is None and shift is not None:
        tints, tint_label = Tints(shift, args.band, False, None), 'with the theme'
    folder = 'frontend_hue' if shift or tints else 'frontend_blue'
    out = Path(args.out) if args.out else ROOT / 'build' / folder / 'ds2coop_frontend.str'
    changes = Path(args.changes) if args.changes else out.with_name('recolor_changes.tsv')
    src = Path(args.src)
    data = src.read_bytes()
    stock = fs.big_entry(Path(args.game) / 'DS2DAT6.DAT', fs.FRONTEND)
    new, rows, summary, report = recolor(data, stock, shift, tints)

    lines = ['pass\tmovie\tfield\toffset\tdetail\told\tnew']
    lines += ['%s\t%s\t%s\t0x%06X\t%s\t%s\t%s' % (p, m, kind, off, d, o, n) for p, m, kind, off, d, o, n in rows]
    lines += ['', 'movie\tpage\tformat\tresult'] + ['%s\t%s\t%s\t%s' % s for s in summary]
    text = ('\r\n'.join(lines) + '\r\n').encode('ascii')
    if not args.check:
        out.parent.mkdir(parents=True, exist_ok=True)
        tmp = out.with_suffix('.tmp')
        tmp.write_bytes(new)
        tmp.replace(out)
        changes.write_bytes(text)
    report.update({'in': str(src), 'in_size': len(data), 'in_crc32': '%08X' % zlib.crc32(data), 'shift': label, 'tints': tint_label,
                   'out': str(out), 'size': len(new), 'crc32': '%08X' % zlib.crc32(new),
                   'sha256': hashlib.sha256(new).hexdigest(), 'changes': str(changes), 'rows': len(rows),
                   'written': not args.check})
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
