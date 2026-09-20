"""Offline evidence for the DS2 spawn multiplier.

Read-only. Finds every live FuncSpawn object in the given crash dumps and
reports the fields the multiplier depends on, cross-referenced against the
randomizer's embedded enemy catalog.
"""
import re
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, r"C:\Users\me\Programme\DeadSpace3-Mod\trainer")
from analyze_minidump import Dump  # noqa: E402

VTABLE = 0x01B59C58
VTABLE2 = 0x01B59C54
CATALOG = Path(r"C:\Users\me\Programme\DeadSpace3-Mod\DeadSpace2Randomizer"
               r"\src\generated_enemy_catalog.h")

ROW = re.compile(
    r"\{\{(0x[0-9A-Fa-f]+)u,(0x[0-9A-Fa-f]+)u,(0x[0-9A-Fa-f]+)u,(0x[0-9A-Fa-f]+)u\},"
    r"\s*\{[^}]*\},\s*\"([^\"]*)\",\s*\"([^\"]*)\"")


def load_catalog():
    text = CATALOG.read_text(encoding="utf-8", errors="replace")
    out = {}
    for m in ROW.finditer(text):
        out[tuple(int(m.group(i), 16) for i in range(1, 5))] = (m.group(5), m.group(6))
    return out


def objects(dump):
    needle = struct.pack("<I", VTABLE)
    for start, end, file_rva in dump.ranges:
        size = end - start
        dump.f.seek(file_rva)
        data = dump.f.read(size)
        pos = data.find(needle)
        while pos != -1:
            if pos % 4 == 0 and pos + 0x100 <= size and \
                    struct.unpack_from("<I", data, pos + 0x10)[0] == VTABLE2:
                yield start + pos, data[pos:pos + 0x100]
            pos = data.find(needle, pos + 4)


def main():
    catalog = load_catalog()
    print("enemy catalog rows:", len(catalog))
    total = 0
    maxs, opts, pops, counts = Counter(), Counter(), Counter(), Counter()
    fam, ctx, famctx = Counter(), Counter(), Counter()
    blocked_bit = 0
    unreadable = 0
    for path in sys.argv[1:]:
        d = Dump(Path(path))
        here = 0
        for _va, blob in objects(d):
            here += 1
            total += 1
            counts[struct.unpack_from("<i", blob, 0xC0)[0]] += 1
            attrs = struct.unpack_from("<I", blob, 0x0C)[0]
            araw = d.read(attrs, 0x70) if attrs else None
            if araw is None or len(araw) != 0x70:
                unreadable += 1
                continue
            maxs[struct.unpack_from("<i", araw, 0x1C)[0]] += 1
            option = struct.unpack_from("<I", araw, 0x30)[0]
            opts[option] += 1
            if option & 2:
                blocked_bit += 1
            pops[struct.unpack_from("<i", araw, 0x54)[0]] += 1
            guid = struct.unpack_from("<4I", araw, 0x0C)
            hit = catalog.get(guid)
            if hit:
                fam[hit[0]] += 1
                ctx[hit[1]] += 1
                famctx[hit] += 1
            else:
                fam["<unknown target>"] += 1
                ctx["<unknown target>"] += 1
        print("%-46s objects=%d" % (Path(path).name, here))
        d.close()

    print()
    print("total FuncSpawn objects:", total, " attributes unreadable:", unreadable)
    print("maxSpawn attrs+0x1C:", dict(sorted(maxs.items())))
    print("options  attrs+0x30:", {hex(k): v for k, v in sorted(opts.items())})
    print("  -> objects with the CheckBlocked bit (0x02) set:", blocked_bit)
    print("popThreshold attrs+0x54:", dict(sorted(pops.items())))
    print("count this+0xC0:", dict(sorted(counts.items())))
    print()
    print("authored target family (attrs+0x0C vs embedded catalog):")
    for k, v in fam.most_common():
        print("  %-28s %d" % (k, v))
    print()
    print("authored arrival context:")
    for k, v in ctx.most_common():
        print("  %-40s %d" % (k, v))
    print()
    print("family + context pairs that a multiplier would accept:")
    accept = ("default_NONE_descriptor", "SPAWNMODE_NONE")
    for (f, c), v in sorted(famctx.items()):
        mark = "direct" if c in accept else ("vent" if c.startswith("SPAWNMODE_VENT") else "refuse")
        print("  %-24s %-40s %-6s %d" % (f, c, mark, v))


if __name__ == "__main__":
    main()
