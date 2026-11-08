"""Replace a 2D DXT1/DXT5 texture of a built frontend stream with a PNG, in place.

The texture keeps its name, format, size and mip count; every level is encoded again from
the PNG (resized to the texture's size). The stream is framed again with the texture data
recompressed, every other record is copied as it is and checked byte for byte.

    replace_frontend_texture.py --name lgo_ds2_02 --png FILE [--in FILE] [--out FILE] [--check]

The title logo is lgo_ds2_02 (1024x512 DXT5, two levels); lgo_ds2_blur and
lgo_ds2_02_scramble are its versions for the title's glitch transitions.
"""
import argparse
import hashlib
import io
import json
import struct
import sys
import zlib
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import apx_edit as ax  # noqa: E402
import frontend_str as fs  # noqa: E402


def layout(name, head, size):
    """(fourcc, block, [(offset, w, h, bytes)]) of a 2D texture."""
    if b'DXT5' in head:
        fourcc, block = 'DXT5', 16
    elif b'DXT1' in head:
        fourcc, block = 'DXT1', 8
    else:
        raise SystemExit('%s: not DXT1/DXT5' % name)
    if b'\0cube\0' in head:
        raise SystemExit('%s: a cube map' % name)
    declared, w, h = struct.unpack_from('<IHH', head, 0x1C)
    if declared != size:
        raise SystemExit('%s: header says %d bytes, data has %d' % (name, declared, size))
    levels, total = [], 0
    while total < size:
        n = ((w + 3) // 4) * ((h + 3) // 4) * block
        levels.append((total, w, h, n))
        total += n
        w, h = max(1, w // 2), max(1, h // 2)
    if total != size:
        raise SystemExit('%s: %d bytes is no mip chain' % (name, size))
    return fourcc, block, levels


def encode(im, w, h, fourcc):
    """DXT blocks of one level; sides below 4 are padded to a 4x4 block."""
    pw, ph = max(4, w), max(4, h)
    level = im.resize((w, h), Image.LANCZOS)
    if (pw, ph) != (w, h):
        pad = Image.new('RGBA', (pw, ph), (0, 0, 0, 0))
        pad.paste(level, (0, 0))
        level = pad
    buf = io.BytesIO()
    level.save(buf, format='DDS', pixel_format=fourcc)
    dds = buf.getvalue()
    if dds[:4] != b'DDS ' or dds[84:88] != fourcc.encode():
        raise SystemExit('Pillow did not write %s' % fourcc)
    block = 16 if fourcc == 'DXT5' else 8
    n = ((pw + 3) // 4) * ((ph + 3) // 4) * block
    return dds[128:128 + n]


def find(data, name):
    """(assets, tg4h payload, tg4d payload, index of the tg4d asset)."""
    assets = fs.group(fs.records(data))
    head = body = None
    at = -1
    for k, (shdr, bodies) in enumerate(assets):
        base, kind = fs.names(shdr)
        if base == name and kind == 'tg4h':
            head = fs.payload(bodies, fs.declared(shdr))
        elif base == name and kind == 'tg4d':
            if body is not None:
                raise SystemExit('%s: two texture data records' % name)
            body, at = fs.payload(bodies, fs.declared(shdr)), k
    if head is None or body is None:
        raise SystemExit('%s: not in the stream' % name)
    return assets, head, body, at


def replace(assets, at, name, new):
    """The stream framed again with asset `at` carrying `new`; every other asset checked byte for byte."""
    out_assets = [(s, fs.rpak(new) if k == at else b) for k, (s, b) in enumerate(assets)]
    out = fs.frame(out_assets)
    count = fs.validate(out)
    back = fs.group(fs.records(out))
    if len(back) != len(assets):
        raise SystemExit('asset count changed on read back')
    for k, ((s0, b0), (s1, b1)) in enumerate(zip(assets, back)):
        if s0 != s1:
            raise SystemExit('asset header %d changed' % k)
        if k == at:
            if fs.payload(b1, fs.declared(s1)) != new:
                raise SystemExit('%s differs on read back' % name)
        elif b0 != b1:
            raise SystemExit('asset %d (%s) changed' % (k, fs.names(s0)[0]))
    return out, count


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', required=True)
    ap.add_argument('--png', required=True)
    ap.add_argument('--in', dest='src', default=str(ROOT / 'build' / 'frontend_red_full' / 'ds2coop_frontend.str'))
    ap.add_argument('--out', default=str(ROOT / 'build' / 'frontend_title' / 'ds2coop_frontend.str'))
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()

    data = Path(args.src).read_bytes()
    assets, head, body, at = find(data, args.name)
    fourcc, block, levels = layout(args.name, head, len(body))
    im = Image.open(args.png).convert('RGBA')
    new = b''.join(encode(im, w, h, fourcc) for _, w, h, _ in levels)
    if len(new) != len(body):
        raise SystemExit('encoded %d bytes, the texture holds %d' % (len(new), len(body)))
    # read the top level back as a sanity check of the block layout
    ax.dxt_decode(new[:levels[0][3]], levels[0][1], levels[0][2], fourcc.encode())
    out, count = replace(assets, at, args.name, new)
    if not args.check:
        dst = Path(args.out)
        dst.parent.mkdir(parents=True, exist_ok=True)
        tmp = dst.with_suffix('.tmp')
        tmp.write_bytes(out)
        tmp.replace(dst)
    print(json.dumps({'texture': args.name, 'format': fourcc, 'levels': [(w, h) for _, w, h, _ in levels], 'png': args.png,
                      'png_size': im.size, 'in': args.src, 'in_crc32': '%08X' % zlib.crc32(data), 'out': args.out,
                      'size': len(out), 'crc32': '%08X' % zlib.crc32(out), 'sha256': hashlib.sha256(out).hexdigest(),
                      'records': count, 'written': not args.check}, indent=1))


if __name__ == '__main__':
    main()
