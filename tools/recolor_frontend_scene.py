"""Recolour the 3D main menu scene of a built frontend stream (ds2coop_frontend.str) to red.

Works on any frontend stream built by build_frontend_override, build_loadout_frontend or
recolor_frontend: the Apt movies are never touched. Only colour values change; every changed
asset keeps its payload size and layout and is recompressed, every other record is copied as
it is.

What changes:
- tg4d cub_bridgeatruim_illum: incandescence cube map (6 faces, DXT1) of ui_mat_fe_branches
  (nerves) and ui_mat_fe_marker_a(_vfx) (spires and marker shards).
- tg4d propneuronsinside_ca: subsurface map (DXT5, all mips) of the nerves.
  DXT colour endpoints go through a 565 lookup; DXT1 blocks keep their mode, so punch-through
  alpha stays; DXT5 alpha blocks are not touched.
- LightTOC of the stream: light colours (RwRGBA, red first). The scene has ambient (white),
  ears_light_key and ears_light_fill (both blue).
- VFX colour keys of every effect that exists only in frontend.str (analysis/catalog):
  particles 'trp.' (RGBA0, RGBAMin, RGBAMax at +0x200/+0x1E0/+0x1F0 of the params, 0..255,
  params at record + 0x0C + header[3]), FX text 'tfb.' (colour +0x30, 0..1), light 'til.'
  (colour +0x44, shadow colour +0x84, 0..1) and screen 'rcs.' (colour +0x20, 0..1), params at
  record + 0x0C. Alpha never changes. A chromatic RGBA delta would be reported, none exists.

Colours whose hue lies in the band (default 60..300: green, teal, cyan, blue, violet) turn to the
hue of --rgb (default #D8443C); the band is compressed by --spread around 180 so cyan and blue
stay slightly apart. HSV value and alpha stay, saturation is scaled by --saturation. Greys
(saturation below 0.05), reds, oranges, yellows and magentas stay.

Not changed: cub_bridgeatruim (reflection cube, shared with 114 level streams), material
parameters (ui_mat_fe_branches params3 is not a colour: the pixel shader at exe offset 0x18442A8
uses x as lighting saturation, y as incandescence gain, z/w as a luminance lerp), flares 'clf.'
(their PC layout differs from the prototype PDB), effects also found in other streams
(human_fuzz*, ui_fe_arrow, credits titles), fog colour and g_Tint (set by code).

    recolor_frontend_scene.py [--in FILE] [--out FILE] [--rgb RRGGBB] [--spread F]
                              [--saturation F] [--style red|dark] [--catalog FILE] [--changes FILE]
                              [--preview DIR] [--check]

--style    red: everything takes the red; dark: red only as an accent (STYLES: glow and
           subsurface dimmed, the frontend's own reflection cube a warm grey, lights near white,
           nebula a dark wine red)

--in       built frontend stream, default build/frontend_red_ui/ds2coop_frontend.str
--out      default build/frontend_red_scene/ds2coop_frontend.str
--changes  change list (TSV), default scene_changes.tsv next to --out
--preview  before/after PNGs, default preview/ next to --out
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
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import apx_edit as ax  # noqa: E402
import frontend_str as fs  # noqa: E402

FRONTEND_ARCHIVE = 'frontend\\frontend.str'
CATALOGS = (ROOT / 'analysis' / 'catalog' / 'assets.jsonl', ROOT / 'analysis' / 'catalog_dlc' / 'assets.jsonl')
MATERIAL = 'ui_mat_fe_branches'
# texture -> (material sampler that names it, role, style part)
TEXTURES = {'cub_bridgeatruim_illum': ('incandescenceSampler', 'glow cube of nerves and spires', 'glow'),
            'propneuronsinside_ca': ('subSurfaceSampler', 'nerve subsurface', 'subsurface'),
            'cub_bridgeatruim': ('reflectionSampler', 'reflection cube of nerves and spires', 'reflection')}
# Level streams carry their own copy of the reflection cube; this edits only the frontend's.
SHARED_OK = {'cub_bridgeatruim'}
MIN_SAT = 0.05
LIGHT_SIZE = 0xA0
LIGHT_COLOR = 0x5C
LIGHT_NAME = 0x84
# record tag -> [(field, offset in params, scale, floats)]; RGBA fields keep their alpha
VFX_FIELDS = {b'trp.': [('RGBADelta', 0x1D0, 255.0, 4), ('RGBAMin', 0x1E0, 255.0, 4), ('RGBAMax', 0x1F0, 255.0, 4),
                        ('RGBA0', 0x200, 255.0, 4)],
              b'tfb.': [('colour', 0x30, 1.0, 4)],
              b'til.': [('colour', 0x44, 1.0, 3), ('shadowColour', 0x84, 1.0, 3)],
              b'rcs.': [('colour', 0x20, 1.0, 3)]}
# smallest record length that holds the fields (params start + struct size seen in retail)
VFX_MIN_LEN = {b'tfb.': 0x0C + 0xA0, b'til.': 0x0C + 0xA8, b'rcs.': 0x0C + 0xBC}
SKIPPED_TAGS = {b'clf.': 'flare: PC layout differs from the prototype PDB, colour fields unknown'}


# ---------------------------------------------------------------- colour map

class RedShift:
    """Hue band to red: value and alpha stay, saturation scales, other hues stay."""

    def __init__(self, rgb, spread, saturation, lo=60.0, hi=300.0, fade=15.0, centre=180.0, value=1.0):
        h, s, _ = colorsys.rgb_to_hsv(*(c / 255 for c in rgb))
        if s < MIN_SAT:
            raise SystemExit('--rgb %02X%02X%02X has no hue' % rgb)
        self.target = h * 360
        self.spread, self.saturation, self.value = spread, saturation, value
        self.lo, self.hi, self.fade, self.centre = lo, hi, fade, centre
        lut = np.arange(65536, dtype=np.uint16)
        for v in range(65536):
            old = rgb565(v)
            new = self(old)
            if new != old:
                lut[v] = to565(new)
        self.lut = lut

    def weight(self, h):
        if self.lo <= h <= self.hi:
            return 1.0
        d = min(abs(h - self.lo), abs(h - self.hi), abs(h + 360 - self.hi), abs(self.lo + 360 - h))
        return max(0.0, 1 - d / self.fade)

    def __call__(self, rgb):
        """rgb in 0..1; returns the input tuple when nothing changes."""
        rgb = tuple(rgb)
        h, s, v = colorsys.rgb_to_hsv(*(min(1.0, max(0.0, c)) for c in rgb))
        if s < MIN_SAT or v <= 0:
            return rgb
        w = self.weight(h * 360)
        if not w:
            return rgb
        nh = (self.target + (h * 360 - self.centre) * self.spread) % 360
        new = colorsys.hsv_to_rgb(nh / 360, min(1.0, s * self.saturation), v * self.value)
        return tuple(o + w * (n - o) for o, n in zip(rgb, new))

    def label(self):
        return 'hue %.1f, band %g..%g (+%g fade), spread %g, saturation x%g, value x%g' % (
            self.target, self.lo, self.hi, self.fade, self.spread, self.saturation, self.value)


# --style dark: red only as an accent. Glow and subsurface dimmed, the blue reflection cube of
# the nerves turned to a warm grey, lights near white, nebula and particles a dark wine red.
# (saturation, value) per part; reflection None in the red style (left blue).
STYLES = {'red': {'glow': (1.0, 1.0), 'subsurface': (1.0, 1.0), 'reflection': None, 'lights': (1.0, 1.0), 'vfx': (1.0, 1.0)},
          'dark': {'glow': (0.85, 0.55), 'subsurface': (0.6, 0.8), 'reflection': (0.12, 1.0), 'lights': (0.25, 1.0), 'vfx': (0.6, 0.55)}}


def rgb565(v):
    r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
    return (r << 3 | r >> 2) / 255, (g << 2 | g >> 4) / 255, (b << 3 | b >> 2) / 255


def to565(rgb):
    r, g, b = (min(1.0, max(0.0, c)) for c in rgb)
    return round(r * 31) << 11 | round(g * 63) << 5 | round(b * 31)


# ---------------------------------------------------------------- edits

class Edit:
    def __init__(self, name, kind, data):
        self.name, self.kind = name, kind
        self.data = data
        self.out = bytearray(data)
        self.rows = []      # (field, offset, old, new)
        self.ranges = []    # (offset, length) of every field written
        self.swatches = []  # (label, old rgb 0..1, new rgb 0..1)

    def put(self, field, off, new, old_text, new_text):
        if bytes(self.out[off:off + len(new)]) == new:
            return False
        self.out[off:off + len(new)] = new
        self.ranges.append((off, len(new)))
        self.rows.append((field, off, old_text, new_text))
        return True


def fmt(values, scale):
    return '(%s)' % ', '.join(('%g' % round(v, 2)) if scale > 1 else ('%.4g' % v) for v in values)


def shift_rgba(edit, field, off, scale, floats, shift, problems):
    """Map the RGB of a float RGB(A) at off; only the RGB floats are written. Returns the new RGB or None."""
    old = struct.unpack_from('<%df' % floats, edit.out, off)
    rgb = tuple(c / scale for c in old[:3])
    if any(c < -1e-3 or c > 1.0 + 1e-3 for c in rgb):
        problems.append((edit.name, field, '0x%X' % off, 'out of range %s, left' % fmt(old, scale)))
        return None
    new = shift(rgb)
    if new == rgb:
        return None
    if scale > 1:
        vals = tuple(float(round(c * scale)) for c in new)
    else:
        vals = tuple(round(c, 4) for c in new)
    raw = struct.pack('<3f', *vals)
    if edit.put(field, off, raw, fmt(old, scale), fmt(struct.unpack('<3f', raw) + old[3:], scale)):
        edit.swatches.append(('%s %s' % (edit.name, field), rgb, tuple(v / scale for v in vals)))
        return vals
    return None


def record_table(d):
    n = d[0x1C]
    offs = [struct.unpack_from('<I', d, 0x40 + 36 * i)[0] for i in range(n)]
    if offs != sorted(offs) or (offs and (offs[0] < 0x40 + 36 * n or offs[-1] >= len(d))):
        raise ValueError('record table out of order')
    return [(i, o, (offs[i + 1] if i + 1 < n else len(d)), d[o:o + 4]) for i, o in enumerate(offs)]


def shift_vfx(edit, shift, problems, skipped):
    d = edit.data
    for i, o, end, tag in record_table(d):
        if tag in SKIPPED_TAGS:
            skipped.append((edit.name, 'record %d %s' % (i, tag.decode()), '0x%X' % o, SKIPPED_TAGS[tag]))
            continue
        fields = VFX_FIELDS.get(tag)
        if not fields:
            continue
        if tag == b'trp.':
            h = struct.unpack_from('<5I', d, o + 4)
            if h[0] != 0x24 or h[4] - h[3] != 0x1C0 or h[3] not in (0x10, 0x18):
                problems.append((edit.name, 'record %d' % i, '0x%X' % o, 'particle header %s, left' % [hex(x) for x in h]))
                continue
            params = o + 0x0C + h[3]
            if params + 0x210 > end:
                problems.append((edit.name, 'record %d' % i, '0x%X' % o, 'particle record too short, left'))
                continue
        else:
            params = o + 0x0C
            if end - o < VFX_MIN_LEN[tag]:
                problems.append((edit.name, 'record %d' % i, '0x%X' % o, '%s record too short, left' % tag.decode()))
                continue
        if tag == b'trp.':
            delta = struct.unpack_from('<4f', d, params + 0x1D0)
            if len({round(c, 3) for c in delta[:3]}) > 1:
                problems.append((edit.name, 'record %d RGBADelta' % i, '0x%X' % (params + 0x1D0),
                                 'chromatic delta %s, left' % fmt(delta, 255)))
            for field, at, scale, floats in fields[1:]:
                shift_rgba(edit, 'rec %d %s %s' % (i, tag.decode(), field), params + at, scale, floats, shift, problems)
            mn = struct.unpack_from('<3f', edit.out, params + 0x1E0)
            mx = struct.unpack_from('<3f', edit.out, params + 0x1F0)
            c0 = struct.unpack_from('<3f', edit.out, params + 0x200)
            was = [struct.unpack_from('<3f', d, params + a) for a in (0x1E0, 0x1F0, 0x200)]
            inside_before = all(was[0][k] - 1e-3 <= was[2][k] <= was[1][k] + 1e-3 for k in range(3))
            inside_after = all(mn[k] - 1e-3 <= c0[k] <= mx[k] + 1e-3 for k in range(3))
            if inside_before and not inside_after:
                problems.append((edit.name, 'record %d' % i, '0x%X' % o, 'RGBA0 left the min/max range after the shift'))
        else:
            for field, at, scale, floats in fields:
                shift_rgba(edit, 'rec %d %s %s' % (i, tag.decode(), field), params + at, scale, floats, shift, problems)


def shift_lights(edit, shift, problems):
    d = edit.data
    magic, version, plights, pboxes, nlights, nboxes = struct.unpack_from('<HHIIHH', d, 0)
    if magic != 0x40F2 or version != 5 or plights != 0x10 or nboxes or len(d) != 0x10 + nlights * LIGHT_SIZE:
        problems.append((edit.name, 'LightTOC', '0', 'unexpected layout, left'))
        return
    for i in range(nlights):
        o = plights + LIGHT_SIZE * i
        name = d[o + LIGHT_NAME:o + LIGHT_SIZE].split(b'\0')[0].decode('latin1')
        at = o + LIGHT_COLOR
        r, g, b, a = d[at:at + 4]
        rgb = (r / 255, g / 255, b / 255)
        new = shift(rgb)
        if new == rgb:
            continue
        nb = bytes(min(255, max(0, round(c * 255))) for c in new) + bytes((a,))
        field = 'light %d %s colour RGBA' % (i, name)
        if edit.put(field, at, nb, d[at:at + 4].hex(), nb.hex()):
            edit.swatches.append(('LightTOC %s' % name, rgb, tuple(c / 255 for c in nb[:3])))


# ---------------------------------------------------------------- textures

class Texture:
    def __init__(self, name, head, data):
        self.name, self.head, self.data = name, head, data
        if b'DXT5' in head:
            self.fourcc, self.block = b'DXT5', 16
        elif b'DXT1' in head:
            self.fourcc, self.block = b'DXT1', 8
        else:
            raise SystemExit('%s: not DXT1/DXT5' % name)
        size, self.w, self.h = struct.unpack_from('<IHH', head, 0x1C)
        if size != len(data):
            raise SystemExit('%s: header says %d bytes, data has %d' % (name, size, len(data)))
        self.faces = 6 if b'\0cube\0' in head else 1
        if len(data) % self.faces:
            raise SystemExit('%s: size does not split into faces' % name)
        per_face, self.levels, w, h = len(data) // self.faces, [], self.w, self.h
        total = 0
        while total < per_face:
            size = ((w + 3) // 4) * ((h + 3) // 4) * self.block
            self.levels.append((total, w, h, size))
            total += size
            w, h = max(1, w // 2), max(1, h // 2)
        if total != per_face:
            raise SystemExit('%s: %d bytes is no %dx%d mip chain' % (name, len(data), self.w, self.h))

    def images(self, data):
        """[(face, level, RGBA image)] of every face and level."""
        out = []
        per_face = len(data) // self.faces
        for f in range(self.faces):
            for lv, (off, w, h, size) in enumerate(self.levels):
                at = f * per_face + off
                out.append((f, lv, ax.dxt_decode(data[at:at + size], w, h, self.fourcc)))
        return out


def shift_blocks(data, block, lut):
    """New DXT data with the colour endpoints mapped; returns (bytes, blocks changed)."""
    a = np.frombuffer(data, np.uint8).reshape(-1, block).copy()
    col = a[:, block - 8:block - 4].copy().view('<u2').reshape(-1, 2)
    idx = a[:, block - 4:block].copy().view('<u4').reshape(-1)
    c0, c1 = col[:, 0].copy(), col[:, 1].copy()
    n0, n1 = lut[c0], lut[c1]
    # DXT1: c0 > c1 means four colours, else three and transparent; DXT5 always four
    four = c0 > c1 if block == 8 else np.ones(len(a), bool)
    greater = c0 > c1
    swap = (greater & (n0 < n1)) | (~greater & (n0 > n1))
    hi = (idx >> 1) & 0x55555555
    flipped = np.where(four, idx ^ 0x55555555, idx ^ (~hi & 0x55555555))
    idx = np.where(swap, flipped, idx).astype(np.uint32)
    n0, n1 = np.where(swap, n1, n0), np.where(swap, n0, n1)
    # a four colour block whose endpoints became equal drops to three colours: index 3 -> 0
    flat = greater & (n0 == n1)
    m3 = idx & (idx >> 1) & 0x55555555
    idx = np.where(flat, idx & ~(m3 | (m3 << 1)), idx).astype(np.uint32)
    col[:, 0], col[:, 1] = n0, n1
    a[:, block - 8:block - 4] = col.astype('<u2').view(np.uint8).reshape(-1, 4)
    a[:, block - 4:block] = idx.astype('<u4').view(np.uint8).reshape(-1, 4)
    out = a.tobytes()
    changed = int((np.frombuffer(out, np.uint8).reshape(-1, block) != np.frombuffer(data, np.uint8).reshape(-1, block)).any(1).sum())
    return out, changed


def shift_texture(edit, tex, shift):
    new, changed = shift_blocks(tex.data, tex.block, shift.lut)
    edit.out[:] = new
    for k in range(0, len(new), tex.block):
        lo = k + tex.block - 8   # colour part; DXT5 alpha bytes stay out of the allowed range
        edit.ranges.append((lo, 8))
    edit.rows.append(('%s %dx%d, %d face(s), %d level(s): colour blocks' % (tex.fourcc.decode(), tex.w, tex.h, tex.faces, len(tex.levels)),
                      0, '%d blocks, sha %s' % (len(new) // tex.block, hashlib.sha256(tex.data).hexdigest()[:16]),
                      '%d blocks changed, sha %s' % (changed, hashlib.sha256(new).hexdigest()[:16])))
    return changed


# ---------------------------------------------------------------- checks

def exclusive_assets(catalogs):
    """{(name, kind)} that the catalog finds in frontend\\frontend.str and nowhere else."""
    where = {}
    for cat in catalogs:
        if not cat.exists():
            continue
        with open(cat, encoding='utf-8') as f:
            for line in f:
                r = json.loads(line)
                key = (r['name'], r['type'])
                arch = r['archive'].lower().split('\\', 1)[-1]
                where.setdefault(key, set()).add(arch)
    if not where:
        raise SystemExit('no asset catalog found (%s)' % ', '.join(str(c) for c in catalogs))
    return {k for k, v in where.items() if v == {FRONTEND_ARCHIVE}}


def material_samplers(d):
    count, toff, _, voff, _, soff, _ = struct.unpack_from('<7H', d, 0x56)
    out = {}
    for i in range(count):
        _, _, _, name, annot = struct.unpack_from('<IHHHH', d, toff + 12 * i)
        cstr = lambda o: d[o:d.index(b'\0', o)].decode('latin1')  # noqa: E731
        out[cstr(soff + name)] = cstr(soff + annot)
    return out


def verify_edit(edit):
    old, new = edit.data, bytes(edit.out)
    if len(old) != len(new):
        raise SystemExit('%s changed size' % edit.name)
    allowed = np.zeros(len(old), dtype=bool)
    for off, ln in edit.ranges:
        allowed[off:off + ln] = True
    diff = np.frombuffer(old, np.uint8) != np.frombuffer(new, np.uint8)
    stray = np.nonzero(diff & ~allowed)[0]
    if len(stray):
        raise SystemExit('%s: %d bytes changed outside colour fields, first at %#x' % (edit.name, len(stray), stray[0]))
    if edit.kind == 'VFX':
        if [(i, o, e, t) for i, o, e, t in record_table(old)] != [(i, o, e, t) for i, o, e, t in record_table(new)]:
            raise SystemExit('%s: record table changed' % edit.name)
        if old[:0x40 + 36 * old[0x1C]] != new[:0x40 + 36 * new[0x1C]]:
            raise SystemExit('%s: effect header changed' % edit.name)
    return int(diff.sum())


def verify_texture(tex, new):
    """Alpha of every face and level stays; returns the share of pixels whose colour changed."""
    moved, total = 0, 0
    for (f, lv, a), (_, _, b) in zip(tex.images(tex.data), tex.images(new)):
        a, b = np.asarray(a), np.asarray(b)
        if not np.array_equal(a[..., 3], b[..., 3]):
            raise SystemExit('%s face %d level %d: alpha changed' % (tex.name, f, lv))
        moved += int((a[..., :3] != b[..., :3]).any(-1).sum())
        total += a.shape[0] * a.shape[1]
    return moved / total


# ---------------------------------------------------------------- previews

def label(draw, xy, text):
    draw.text(xy, text, fill=(235, 235, 235))


def texture_sheet(pairs, path):
    """pairs: [(title, before image, after image)]"""
    width = 1240
    blocks = []
    for title, b, a in pairs:
        # RGB only: an RGBA resize premultiplies alpha and would hide colour under alpha 0
        b, a = b.convert('RGB'), a.convert('RGB')
        b.thumbnail((600, 340))
        a.thumbnail((600, 340))
        t = Image.new('RGB', (width, max(b.height, a.height) + 24), (28, 28, 28))
        t.paste(b, (0, 22))
        t.paste(a, (width // 2 + 10, 22))
        label(ImageDraw.Draw(t), (4, 4), title + '   (before | after)')
        blocks.append(t)
    sheet = Image.new('RGB', (width, sum(t.height for t in blocks)), (28, 28, 28))
    y = 0
    for t in blocks:
        sheet.paste(t, (0, y))
        y += t.height
    sheet.save(path)


def faces_strip(images, size):
    strip = Image.new('RGBA', (len(images) * (size + 4), size), (28, 28, 28, 255))
    for k, im in enumerate(images):
        strip.paste(im.resize((size, size)), (k * (size + 4), 0))
    return strip


def swatch_sheet(swatches, path):
    cols, cw, ch = 3, 420, 34
    rows = (len(swatches) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * cw, max(1, rows) * ch + 20), (28, 28, 28))
    draw = ImageDraw.Draw(sheet)
    label(draw, (4, 4), 'colour keys: before | after (alpha not shown)')
    for k, (text, old, new) in enumerate(swatches):
        x, y = (k % cols) * cw, 20 + (k // cols) * ch
        to8 = lambda c: tuple(min(255, max(0, round(v * 255))) for v in c)  # noqa: E731
        draw.rectangle((x + 4, y + 2, x + 40, y + ch - 4), fill=to8(old))
        draw.rectangle((x + 44, y + 2, x + 80, y + ch - 4), fill=to8(new))
        label(draw, (x + 86, y + 4), text[:52])
        label(draw, (x + 86, y + 17), '%s -> %s' % ('%d,%d,%d' % to8(old), '%d,%d,%d' % to8(new)))
    sheet.save(path)


# ---------------------------------------------------------------- main

def recolor(data, shifts, catalogs):
    """shifts: style part (glow, subsurface, reflection, lights, vfx) -> RedShift or None."""
    assets = fs.group(fs.records(data))
    exclusive = exclusive_assets(catalogs)
    index = {}
    for k, (shdr, _) in enumerate(assets):
        index.setdefault(fs.names(shdr), []).append(k)

    def one(name, kind):
        ks = index.get((name, kind), [])
        if len(ks) != 1:
            raise SystemExit('%s (%s) found %d times' % (name, kind, len(ks)))
        return ks[0]

    def load(k):
        shdr, bodies = assets[k]
        return fs.payload(bodies, fs.declared(shdr))

    edits, problems, skipped, textures, notes = {}, [], [], {}, []

    # the material tells which textures are its glow and subsurface maps
    samplers = material_samplers(load(one(MATERIAL, 'Material')))
    for tex_name, (sampler, role, part) in TEXTURES.items():
        if shifts.get(part) is None:
            continue
        path = samplers.get(sampler, '')
        if not path.endswith('/%s_tdf.xml' % tex_name):
            raise SystemExit('%s %s names %r, not %s' % (MATERIAL, sampler, path, tex_name))
        if (tex_name, 'tg4d') not in exclusive and tex_name not in SHARED_OK:
            raise SystemExit('%s is not exclusive to frontend.str' % tex_name)
        kh, kd = one(tex_name, 'tg4h'), one(tex_name, 'tg4d')
        tex = Texture(tex_name, load(kh), load(kd))
        edit = Edit(tex_name, 'tg4d', tex.data)
        changed = shift_texture(edit, tex, shifts[part])
        if changed:
            edits[kd] = edit
            textures[tex_name] = (tex, role)

    k = one('LightTOC', 'LightTOC')
    edit = Edit('LightTOC', 'LightTOC', load(k))
    shift_lights(edit, shifts['lights'], problems)
    if edit.ranges:
        edits[k] = edit

    shared_vfx = []
    for (name, kind), ks in sorted(index.items()):
        if kind != 'VFX':
            continue
        if (name, kind) not in exclusive:
            shared_vfx.append(name)
            continue
        for k in ks:
            edit = Edit(name, 'VFX', load(k))
            shift_vfx(edit, shifts['vfx'], problems, skipped)
            if edit.ranges:
                edits[k] = edit

    stats = {}
    for k, edit in edits.items():
        stats[edit.name] = {'kind': edit.kind, 'fields': len(edit.rows), 'bytes': verify_edit(edit)}
    for name, (tex, role) in textures.items():
        k = one(name, 'tg4d')
        stats[name]['pixels_changed'] = round(verify_texture(tex, bytes(edits[k].out)), 4)
        stats[name]['role'] = role
    notes.append('shared VFX left: ' + ', '.join(n for n in shared_vfx))
    notes.append('%s params3 left: lighting saturation, incandescence gain and luminance lerp, not a colour '
                 '(pixel shader at exe offset 0x18442A8)' % MATERIAL)
    if shifts.get('reflection') is None:
        notes.append('cub_bridgeatruim left: reflection cube shared with level streams')
    else:
        notes.append('cub_bridgeatruim changed in this stream only; level streams keep their own copy')

    out_assets = []
    for k, (shdr, bodies) in enumerate(assets):
        if k in edits:
            out_assets.append((shdr, fs.rpak(bytes(edits[k].out))))
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
        if k in edits:
            if fs.payload(b1, fs.declared(s1)) != bytes(edits[k].out):
                raise SystemExit('%s differs on read back' % fs.names(s1)[0])
        elif b0 != b1:
            raise SystemExit('asset %d (%s) changed' % (k, fs.names(s0)[0]))
    report = {'records': count, 'assets': len(back), 'changed_assets': len(edits),
              'unchanged_assets_identical': len(back) - len(edits), 'assets_changed': stats}
    return out, edits, textures, problems, skipped, notes, report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--in', dest='src', default=str(ROOT / 'build' / 'frontend_red_ui' / 'ds2coop_frontend.str'))
    ap.add_argument('--out', default=str(ROOT / 'build' / 'frontend_red_scene' / 'ds2coop_frontend.str'))
    ap.add_argument('--rgb', default='D8443C')
    ap.add_argument('--spread', type=float, default=0.08)
    ap.add_argument('--saturation', type=float, default=1.2)
    ap.add_argument('--style', choices=sorted(STYLES), default='red')
    ap.add_argument('--catalog', action='append', default=None)
    ap.add_argument('--changes', default=None)
    ap.add_argument('--preview', default=None)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()

    t = args.rgb.lstrip('#')
    if len(t) != 6:
        raise SystemExit('--rgb takes RRGGBB')
    rgb = tuple(int(t[i:i + 2], 16) for i in (0, 2, 4))
    shifts = {part: None if sv is None else RedShift(rgb, args.spread, args.saturation * sv[0], value=sv[1])
              for part, sv in STYLES[args.style].items()}
    out_path = Path(args.out)
    changes = Path(args.changes) if args.changes else out_path.with_name('scene_changes.tsv')
    preview = Path(args.preview) if args.preview else out_path.parent / 'preview'
    catalogs = [Path(c) for c in args.catalog] if args.catalog else list(CATALOGS)
    src = Path(args.src)
    data = src.read_bytes()
    new, edits, textures, problems, skipped, notes, report = recolor(data, shifts, catalogs)

    lines = ['asset\tkind\tfield\toffset\told\tnew']
    for k in sorted(edits, key=lambda k: (edits[k].kind, edits[k].name)):
        e = edits[k]
        lines += ['%s\t%s\t%s\t0x%05X\t%s\t%s' % (e.name, e.kind, f, off, o, n) for f, off, o, n in e.rows]
    lines += ['', 'asset\tfield\toffset\tnot changed']
    lines += ['%s\t%s\t%s\t%s' % p for p in problems + skipped]
    lines += ['', 'note'] + notes
    text = ('\r\n'.join(lines) + '\r\n').encode('ascii', 'replace')

    if not args.check:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        tmp = out_path.with_suffix('.tmp')
        tmp.write_bytes(new)
        tmp.replace(out_path)
        changes.write_bytes(text)
        preview.mkdir(parents=True, exist_ok=True)
        pairs = []
        for name, (tex, role) in textures.items():
            k = next(k for k in edits if edits[k].name == name)
            before, after = tex.images(tex.data), tex.images(bytes(edits[k].out))
            if tex.faces > 1:
                b = faces_strip([im for f, lv, im in before if lv == 0], 192)
                a = faces_strip([im for f, lv, im in after if lv == 0], 192)
                pairs.append(('%s (%s), 6 cube faces' % (name, role), b, a))
            else:
                pairs.append(('%s (%s), level 0 %dx%d' % (name, role, tex.w, tex.h), before[0][2], after[0][2]))
        texture_sheet(pairs, preview / 'scene_textures_before_after.png')
        swatches = [s for k in sorted(edits, key=lambda k: edits[k].name) for s in edits[k].swatches]
        swatch_sheet(swatches, preview / 'scene_keys_before_after.png')
    report.update({'in': str(src), 'in_size': len(data), 'in_crc32': '%08X' % zlib.crc32(data),
                   'style': args.style, 'shift': {p: s.label() if s else 'left' for p, s in shifts.items()}, 'out': str(out_path), 'size': len(new), 'crc32': '%08X' % zlib.crc32(new),
                   'sha256': hashlib.sha256(new).hexdigest(), 'changes': str(changes),
                   'rows': sum(len(e.rows) for e in edits.values()), 'not_changed': len(problems) + len(skipped),
                   'problems': len(problems), 'written': not args.check})
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
