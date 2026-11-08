"""Disassemble a function from the dumped PC image (base 0x00400000).

    python -I tools/pc_disasm.py 0x0087ABD0 [--count 200] [--image PATH]

Stops after the first ret when no --count is given. Call targets that the
Xbox matcher named are printed next to the call.
"""
import argparse
import csv
import struct
from pathlib import Path

import capstone

GAME = Path(r"D:\SteamLibrary\steamapps\common\Dead Space 2")
IMAGE = GAME / "work" / "pc_image" / "ds2_pc_image_2026-10-09.bin"
MAP = GAME / "work" / "xbox_pc_match" / "map.tsv"
BASE = 0x00400000


def load_names(path):
    names = {}
    if not path.exists():
        return names
    with path.open(encoding="utf-8") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            try:
                names[int(row["pc"], 16)] = row["name"]
            except (KeyError, ValueError):
                continue
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("address")
    ap.add_argument("--count", type=int, default=0)
    ap.add_argument("--image", default=str(IMAGE))
    ap.add_argument("--map", default=str(MAP))
    a = ap.parse_args()
    data = Path(a.image).read_bytes()
    names = load_names(Path(a.map))
    start = int(a.address, 16)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    off = start - BASE
    limit = a.count or 2000
    n = 0
    for ins in md.disasm(data[off:off + 0x4000], start):
        note = ""
        if ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
            target = int(ins.op_str, 16)
            if target in names:
                note = "  ; " + names[target]
        print(f"{ins.address:08X}  {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}{note}")
        n += 1
        if n >= limit or (not a.count and ins.mnemonic == "ret"):
            break


if __name__ == "__main__":
    main()
