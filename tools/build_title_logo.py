"""The title logo with our line: DEAD SPACE 2 as shipped, a red 2 and a steel UNITED line.

lgo_ds2_02 (1024x512 DXT5, two levels) is edited block by block so the shipped letters keep
their compression: the cyan 2 and its spill on the E only get new colour endpoints, and only
the block rows under the new line are encoded again. The transition copies (lgo_ds2_blur,
lgo_ds2_02_scramble) stay as they are.

    build_title_logo.py [--in FILE] [--out FILE] [--text TEXT] [--preview FILE] [--check] [--restock]

--restock starts from the shipped logo when the input already carries an older line (the
rename of 17.10.2026 on a finished frontend); everything else in the input stays.
"""
import argparse
import colorsys
import hashlib
import io
import json
import struct
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import apx_edit as ax  # noqa: E402
import replace_frontend_texture as rt  # noqa: E402

NAME = 'lgo_ds2_02'
TWO_X, TWO_Y, SPILL_X = 840, 290, 680     # level 0 pixels: the 2, and the E next to it
LINE_Y, LINE_H = 392, 112                  # level 0 rows of the new line
MAX_LINE_W = 860                           # the logo spans about 20..950
RED_HUE = 3.0 / 360
FONT = 'C:/Windows/Fonts/bahnschrift.ttf'


def rgb565(v):
    r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
    return (r << 3 | r >> 2) / 255, (g << 2 | g >> 4) / 255, (b << 3 | b >> 2) / 255


def to565(rgb):
    r, g, b = (min(1.0, max(0.0, c)) for c in rgb)
    return round(r * 31) << 11 | round(g * 63) << 5 | round(b * 31)


def cyan(rgb):
    h, s, v = colorsys.rgb_to_hsv(*rgb)
    return s > 0.25 and 150 <= h * 360 <= 230, s, v


def endpoints(level, w, x0, y0, x1, recolour):
    """Map the colour endpoints of the blocks from (x0, y0) on, left of x1 (level pixels)."""
    bw = (w + 3) // 4
    n = 0
    for by in range(y0 // 4, len(level) // 16 // bw):
        for bx in range(x0 // 4, min(bw, (x1 + 3) // 4)):
            at = (by * bw + bx) * 16 + 8
            c0, c1 = struct.unpack_from('<HH', level, at)
            new = []
            for c in (c0, c1):
                rgb = rgb565(c)
                hit, s, v = cyan(rgb)
                new.append(to565(recolour(s, v)) if hit else c)
            if new != [c0, c1]:
                # BC3 colour blocks are always four colour: endpoint order needs no care
                struct.pack_into('<HH', level, at, *new)
                n += 1
    return n


def steel_line(logo, text):
    """The new line at level 0 size: steel letters from SPACE's own colours, a soft dark shadow."""
    w = logo.size[0]
    band = np.asarray(logo.crop((20, 215, 820, 330))).astype(np.float32)
    opaque = band[..., 3] > 200
    rows = np.array([band[r][opaque[r], :3].mean(0) if opaque[r].any() else band[..., :3][opaque].mean(0)
                     for r in range(band.shape[0])])
    grad = np.array([rows[int(i * (len(rows) - 1) / (LINE_H - 1))] for i in range(LINE_H)])
    streaks = np.random.default_rng(7).normal(0, 9, (LINE_H, w)).astype(np.float32)
    metal = Image.fromarray((grad[:, None, :] + streaks[..., None]).clip(0, 255).astype(np.uint8), 'RGB')
    mask = Image.new('L', (w, LINE_H), 0)
    d = ImageDraw.Draw(mask)
    # the largest size up to 76 px whose spaced line stays under the logo's width
    for size in range(76, 39, -2):
        font = ImageFont.truetype(FONT, size)
        try:
            font.set_variation_by_name('Bold')
        except (OSError, ValueError):
            pass
        spacing = max(6, size * 14 // 76)
        widths = [round(font.getlength(ch)) for ch in text]
        total = sum(widths) + spacing * (len(text) - 1)
        if total <= MAX_LINE_W:
            break
    x = (w - total) // 2 - 30
    for ch, cw in zip(text, widths):
        d.text((x, 10), ch, font=font, fill=255)
        x += cw + spacing
    shadow = Image.new('RGBA', (w, LINE_H), (0, 0, 0, 0))
    shadow.putalpha(mask.filter(ImageFilter.GaussianBlur(5)).point(lambda p: int(p * 0.7)))
    shadow = shadow.transform(shadow.size, Image.AFFINE, (1, 0, -3, 0, 1, -4))
    letters = Image.new('RGBA', (w, LINE_H), (0, 0, 0, 0))
    letters.paste(metal, (0, 0), mask)
    return Image.alpha_composite(shadow, letters)


def encode_rows(level_img, y0, y1):
    """BC3 blocks of the level's rows y0..y1 (multiples of 4), row-major like the texture."""
    strip = level_img.crop((0, y0, level_img.size[0], y1))
    buf = io.BytesIO()
    strip.save(buf, format='DDS', pixel_format='DXT5')
    dds = buf.getvalue()
    if dds[84:88] != b'DXT5':
        raise SystemExit('Pillow did not write DXT5')
    n = (strip.size[0] // 4) * ((y1 - y0) // 4) * 16
    return dds[128:128 + n]


def build(body, levels, text):
    out = bytearray(body)
    report = []
    top = ax.dxt_decode(body[:levels[0][3]], levels[0][1], levels[0][2], b'DXT5').convert('RGBA')
    line0 = steel_line(top, text)
    for k, (off, w, h, size) in enumerate(levels):
        scale = w / levels[0][1]
        level = bytearray(out[off:off + size])
        red = endpoints(level, w, int(TWO_X * scale), int(TWO_Y * scale), w,
                        lambda s, v: colorsys.hsv_to_rgb(RED_HUE, min(1.0, s * 1.4), v))
        steel = endpoints(level, w, int(SPILL_X * scale), int(TWO_Y * scale), int(TWO_X * scale),
                          lambda s, v: (v * 0.80, v * 0.90, v * 0.95))
        y0 = int(LINE_Y * scale) // 4 * 4
        y1 = min(h, (int((LINE_Y + LINE_H) * scale) + 3) // 4 * 4)
        img = ax.dxt_decode(bytes(level), w, h, b'DXT5').convert('RGBA')
        layer = line0.resize((w, max(1, round(LINE_H * scale))), Image.LANCZOS)
        img.alpha_composite(layer, (0, int(LINE_Y * scale)))
        bw = (w + 3) // 4
        level[y0 // 4 * bw * 16:y1 // 4 * bw * 16] = encode_rows(img, y0, y1)
        out[off:off + size] = level
        report.append({'level': '%dx%d' % (w, h), 'red_blocks': red, 'steel_blocks': steel, 'line_rows': [y0, y1]})
    return bytes(out), report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--in', dest='src', default=str(ROOT / 'build' / 'frontend_red_dark' / 'ds2coop_frontend.str'))
    ap.add_argument('--out', default=str(ROOT / 'build' / 'frontend_united' / 'ds2coop_frontend.str'))
    ap.add_argument('--text', default="UNITED")
    ap.add_argument('--preview', default=str(ROOT / 'build' / 'title_logo' / 'title_logo_final_preview.png'))
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--restock', action='store_true')
    args = ap.parse_args()

    data = Path(args.src).read_bytes()
    assets, head, body, at = rt.find(data, NAME)
    stock = rt.find((ROOT / 'build' / 'frontend_stock' / 'ds2coop_frontend.str').read_bytes(), NAME)[2]
    if body != stock:
        if not args.restock or len(body) != len(stock):
            raise SystemExit('%s in the input is not the shipped logo (--restock replaces an older line)' % NAME)
        body = stock
    fourcc, block, levels = rt.layout(NAME, head, len(body))
    if fourcc != 'DXT5' or len(levels) != 2 or levels[0][1:3] != (1024, 512):
        raise SystemExit('%s: unexpected layout %s %s' % (NAME, fourcc, levels))
    new, report = build(body, levels, args.text)
    out, count = rt.replace(assets, at, NAME, new)
    if not args.check:
        dst = Path(args.out)
        dst.parent.mkdir(parents=True, exist_ok=True)
        tmp = dst.with_suffix('.tmp')
        tmp.write_bytes(out)
        tmp.replace(dst)
        im = ax.dxt_decode(new[:levels[0][3]], 1024, 512, b'DXT5').convert('RGBA')
        bg = Image.new('RGBA', im.size, (28, 10, 9, 255))
        bg.alpha_composite(im)
        Path(args.preview).parent.mkdir(parents=True, exist_ok=True)
        bg.convert('RGB').save(args.preview)
    print(json.dumps({'texture': NAME, 'text': args.text, 'levels': report, 'in': args.src, 'in_crc32': '%08X' % zlib.crc32(data),
                      'out': args.out, 'size': len(out), 'crc32': '%08X' % zlib.crc32(out),
                      'sha256': hashlib.sha256(out).hexdigest(), 'records': count, 'preview': args.preview,
                      'written': not args.check}, indent=1))


if __name__ == '__main__':
    main()
