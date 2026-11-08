"""Fit Mark's two Outbreak map pictures in MP20 (the map picker) to the picture window.

The stock map pictures (LibFrontend mp_load_01..05) are full 2:1 photos on a 512x256 quad.
Mark's pictures for the Outbreak maps (MP20 pages 4 and 5, DXT1 256x256) sit on the same quad
but hold a card from his own UI: the photo inside a cyan outline with a tab, rows 36..219 only,
the rest transparent. Spread over the 2:1 quad the card shows twice as wide as it is, its
outline does not match the frame, and the bands above and below stay empty.

This crops the photo inside the outline to 2:1 and stores it squeezed into the square page, so
the quad shows it full and undistorted like the stock pictures. Only the bytes of those two
pages change (same DXT1 size); every other record of the stream is copied as it is.

    fix_mp20_previews.py [--in FILE] [--out FILE] [--check]

--in   the built frontend stream, default the installed randomizer\\ds2coop_frontend.str
--out  default build/frontend_final5/ds2coop_frontend.str
"""
import argparse
import io
import json
import sys
import zlib
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from ds2_paths import GAME_DIR  # noqa: E402
import apx_edit as ax  # noqa: E402
import frontend_str as fs  # noqa: E402
import recolor_frontend as rf  # noqa: E402

MP20 = 'mp20gamesettings.apx'
PAGES = (4, 5)
# inside Mark's outline (x 1..254, y 36..219 drawn; the tab hangs below y 205)
INSIDE = (4, 40, 252, 204)


def dxt1(im):
    buf = io.BytesIO()
    im.convert('RGBA').save(buf, format='DDS', pixel_format='DXT1')
    dds = buf.getvalue()
    if dds[:4] != b'DDS ' or dds[84:88] != b'DXT1':
        raise SystemExit('Pillow did not write DXT1')
    size = im.size[0] * im.size[1] // 2
    return dds[128:128 + size]


def fitted(page_img):
    x0, y0, x1, y1 = INSIDE
    w = x1 - x0
    h = w // 2                       # a 2:1 window of the photo, centred
    top = y0 + ((y1 - y0) - h) // 2
    crop = page_img.crop((x0, top, x1, top + h))
    # the quad draws the page twice as wide as tall: store the 2:1 photo squeezed to a square
    return crop.resize(page_img.size, Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--in', dest='src', default=str(Path(GAME_DIR) / 'randomizer' / 'ds2coop_frontend.str'))
    ap.add_argument('--out', default=str(ROOT / 'build' / 'frontend_final5' / 'ds2coop_frontend.str'))
    ap.add_argument('--preview', default=None)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()

    data = Path(args.src).read_bytes()
    assets = fs.group(fs.records(data))
    hits = [k for k, (shdr, _) in enumerate(assets) if fs.names(shdr) == (MP20, 'APX')]
    if len(hits) != 1:
        raise SystemExit('%s found %d times' % (MP20, len(hits)))
    k = hits[0]
    shdr, bodies = assets[k]
    apx = fs.payload(bodies, fs.declared(shdr))
    movie = rf.Movie(MP20, apx)
    out = bytearray(apx)
    report = {'pages': {}}
    previews = []
    for no in PAGES:
        p = movie.page(no)
        if p['format'] not in ('DXT1a', 'DXT1c') or (p['w'], p['h'], p['mips']) != (256, 256, 1):
            raise SystemExit('page %d is %s %dx%d with %d mips, not Mark\'s 256x256 DXT1' % (no, p['format'], p['w'], p['h'], p['mips']))
        old = apx[p['off']:p['off'] + p['size']]
        img = ax.dxt_decode(old, p['w'], p['h'], b'DXT1')
        new_img = fitted(img)
        new = dxt1(new_img)
        if len(new) != len(old):
            raise SystemExit('page %d: %d bytes encoded for %d' % (no, len(new), len(old)))
        out[p['off']:p['off'] + p['size']] = new
        report['pages'][no] = {'bytes': len(new), 'crop_inside': INSIDE}
        previews.append((img.resize((512, 256)), ax.dxt_decode(new, 256, 256, b'DXT1').resize((512, 256))))
    if len(out) != len(apx):
        raise SystemExit('movie size changed')
    changed = sum(1 for a, b in zip(apx, out) if a != b)
    allowed = sum(movie.page(no)['size'] for no in PAGES)
    if changed > allowed:
        raise SystemExit('bytes changed outside the two pages')
    out_assets = [(s, fs.rpak(bytes(out)) if i == k else b) for i, (s, b) in enumerate(assets)]
    stream = fs.frame(out_assets)
    fs.validate(stream)
    back = fs.group(fs.records(stream))
    for i, ((s0, b0), (s1, b1)) in enumerate(zip(assets, back)):
        if s0 != s1:
            raise SystemExit('asset header %d changed' % i)
        if i == k:
            if fs.payload(b1, fs.declared(s1)) != bytes(out):
                raise SystemExit('MP20 differs on read back')
        elif b0 != b1:
            raise SystemExit('asset %d changed' % i)
    if args.preview:
        sheet = Image.new('RGBA', (1034, 266 * len(previews)), (30, 30, 30, 255))
        for i, (a, b) in enumerate(previews):
            sheet.paste(a, (0, i * 266))
            sheet.paste(b, (522, i * 266))
        sheet.save(args.preview)
    target = Path(args.out)
    if not args.check:
        target.parent.mkdir(parents=True, exist_ok=True)
        tmp = target.with_suffix('.tmp')
        tmp.write_bytes(stream)
        tmp.replace(target)
    report.update({'in': args.src, 'in_crc32': '%08X' % zlib.crc32(data), 'out': str(target), 'size': len(stream),
                   'crc32': '%08X' % zlib.crc32(stream), 'mp20_size': len(out), 'bytes_changed': changed, 'written': not args.check})
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
