"""Print an Xbox function from the recomp as PPC with call names.

    xbox_read.py EARS::Modules::TimerUtil::HandleEvents
    xbox_read.py 0x826A2A68
    xbox_read.py TimerUtil::HandleEvents --calls     # only calls

Each `bl`/`b` target is shown by name; the header gives the PDB signature and
the source line. The PC address comes from xbox_pc_match.py's map when known.
"""
import argparse
import csv
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xbox_pc_match as m  # noqa: E402

HEADER = re.compile(r"^// 0x([0-9A-F]{8})  (.+?)(?:  \[.*)?$")


def load_names():
    names = {}
    with open(m.RECOMP / "src" / "symbols.tsv", encoding="utf-8", errors="replace") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            names[int(row["addr"], 16)] = row["symbol"]
    return names


def find(xf, what):
    if what.lower().startswith("0x"):
        va = int(what, 16)
        return [va] if va in xf else []
    exact = [va for va, f in xf.items() if f["name"] == what]
    if exact:
        return exact
    return [va for va, f in xf.items() if f["name"].endswith(what) or what in f["name"]][:12]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what")
    ap.add_argument("--calls", action="store_true")
    ap.add_argument("--recomp")
    ap.add_argument("--out", help="the xbox_pc_match folder (for PC addresses)")
    args = ap.parse_args()
    if args.recomp:
        m.RECOMP = Path(args.recomp)
    if args.out:
        m.OUT = Path(args.out)
    xf = m.cache("xbox_recomp", m.parse_recomp)
    hits = find(xf, args.what)
    if not hits:
        print("no Xbox function matches %r" % args.what)
        return
    if len(hits) > 1:
        for va in hits:
            print("0x%08X %s" % (va, xf[va]["name"]))
        print("%d matches; give the exact name or the address" % len(hits))
        return
    va = hits[0]
    f = xf[va]
    names = load_names()
    pc = ""
    try:
        for r in m.load_map():
            if int(r["xbox"], 16) == va:
                pc = "PC %s (%s, graph %s/%s)" % (r["pc"], r["how"], r["agree"], r["clash"])
                break
    except OSError:
        pass
    path = m.RECOMP / "src" / f["file"]
    want = "// 0x%08X  " % va
    out, inside = [], False
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith(want):
                inside = True
                out.append(line.rstrip("\n"))
                continue
            if not inside:
                continue
            if line.startswith("// 0x") and HEADER.match(line.rstrip("\n")):
                break
            if line.startswith("//   "):
                out.append(line.rstrip("\n"))
                continue
            s = line.strip()
            if s.startswith("// ") and line.startswith("\t"):
                asm = s[3:]
                t = re.match(r"^(bl|b)\s+0x([0-9a-f]+)$", asm)
                if t:
                    target = int(t.group(2), 16)
                    name = names.get(target, "")
                    asm = "%s %s" % (t.group(1), name or ("0x%08X" % target))
                    if HEADER and target in xf:
                        asm = "%s %s" % (t.group(1), xf[target]["name"])
                elif args.calls:
                    continue
                out.append("    " + asm)
            elif s.startswith("loc_") and not args.calls:
                out.append(s)
    print("\n".join(out))
    if pc:
        print("// " + pc)


if __name__ == "__main__":
    main()
