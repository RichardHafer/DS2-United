"""Export a character package's textures as DDS files for editing.

Each tg4h/tg4d pair becomes <out>/<name>.dds with the full mip chain, the same
bytes the game holds plus a 128 byte DDS header. Edited files go back through
`files` in variants/ds2_variants.json (same format, same size, saved with
mipmaps: Intel Texture Works, BC1/BC3 to match, Mip Maps = Auto Generate).
"""
import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, r"D:/SteamLibrary/steamapps/common/Dead Space 2/Dead Space 2 Modding Tools")
import ds2str  # noqa: E402
import skin_assets  # noqa: E402

CHARACTER_STREAMS = Path(r"D:/SteamLibrary/steamapps/common/Dead Space 2/work/ds2dat10/global_assets/global_assets/ds_assets/char_str/npc")


def dds(data, width, height, fourcc, mips):
    header = bytearray(124)
    struct.pack_into("<I", header, 0, 124)
    struct.pack_into("<I", header, 4, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000 | 0x20000)
    struct.pack_into("<II", header, 8, height, width)
    struct.pack_into("<I", header, 16, len(data))
    struct.pack_into("<I", header, 24, mips)
    struct.pack_into("<I", header, 72, 32)
    struct.pack_into("<I", header, 76, 0x4)
    header[80:84] = fourcc
    struct.pack_into("<I", header, 104, 0x1000 | 0x400000 | 0x8)
    return b"DDS " + bytes(header) + data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", help="e.g. exp_cct/exploder.str")
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument("--only", default="", help="comma separated stems, default all")
    args = parser.parse_args()
    stream = ds2str.StreamSet(str(CHARACTER_STREAMS / args.package))
    items = skin_assets.load_package(stream)
    out = args.out or (ROOT / "variants/skins" / Path(args.package).stem)
    out.mkdir(parents=True, exist_ok=True)
    only = {s for s in args.only.split(",") if s}
    pairs = {}
    for it in items:
        if it["meta"]["type"] in ("tg4h", "tg4d"):
            pairs.setdefault(it["meta"]["name"], {})[it["meta"]["type"]] = it
    written = 0
    for name, pair in sorted(pairs.items()):
        if only and name not in only:
            continue
        if "tg4h" not in pair or "tg4d" not in pair:
            continue
        head = pair["tg4h"]["payload"]
        fmt = re.search(rb"DXT[135]", head)
        if not fmt:
            print("skip %s: not DXT" % name); continue
        width, height = struct.unpack_from("<HH", head, 0x20)
        data = pair["tg4d"]["payload"]
        block = 8 if fmt.group() == b"DXT1" else 16
        mips, w, h, total = 0, width, height, 0
        while total < len(data) and w and h:
            total += max(1, w // 4) * max(1, h // 4) * block; mips += 1; w //= 2; h //= 2
        (out / (name + ".dds")).write_bytes(dds(data, width, height, fmt.group(), mips))
        print("%-22s %4dx%-4d %s mips=%d bytes=%d" % (name, width, height, fmt.group().decode(), mips, len(data)))
        written += 1
    print("wrote %d files to %s" % (written, out))


if __name__ == "__main__":
    main()
