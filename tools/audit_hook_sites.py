"""Check our hook sites against the named Xbox build (xbox_pc_match.py map).

For every row of a hook inventory (va, kind, our_name, assumed_signature, ...):
  - the PC function the address lies in, and its Xbox name and signature,
  - the stack bytes the PC function really pops (every `ret N` in it),
  - what our assumed signature implies, and a flag when the two disagree.

    audit_hook_sites.py <inventory.tsv> [--out audit.tsv] [--only high]
"""
import argparse
import bisect
import csv
import re
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xbox_pc_match as m  # noqa: E402

SIG = re.compile(r"^//   sig: (.*)$")


def xbox_sig(xva, source):
    path = m.RECOMP / "src" / source
    want = "// 0x%08X  " % xva
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            hit = False
            for line in f:
                if line.startswith(want):
                    hit = True
                elif hit:
                    s = SIG.match(line.rstrip("\n"))
                    if s:
                        return s.group(1)
                    if line.startswith("#if") or line.startswith("DEFINE"):
                        return ""
    except OSError:
        pass
    return ""


def sig_stack(sig):
    """(this, params) from an MSVC signature string, None when unknown."""
    if not sig or "(" not in sig:
        return None
    member = bool(re.match(r"^(public|private|protected):", sig)) and " static " not in sig
    inner = sig[sig.index("(") + 1:sig.rindex(")")].strip()
    depth, parts, cur = 0, [], ""
    for ch in inner:
        if ch in "<(":
            depth += 1
        elif ch in ">)":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    params = 0 if inner in ("", "void") else len(parts)
    return member, params


def pc_rets(raw, start, end):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    rets = set()
    code = raw[start - m.PC_BASE:end - m.PC_BASE]
    for addr, size, mnem, ops in md.disasm_lite(code, start):
        if mnem == "ret":
            rets.add(int(ops, 16) if ops else 0)
        if mnem == "int3":
            break
    return rets


def assumed_pop(sig):
    """Stack bytes our assumed signature pops, from an explicit 'ret N' or the argument count."""
    if not sig:
        return None
    r = re.search(r"\bret\s*(0x[0-9a-fA-F]+|\d+)", sig)
    if r:
        return int(r.group(1), 0)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inventory")
    ap.add_argument("--out")
    ap.add_argument("--only")
    ap.add_argument("--recomp", help="the recomp folder, as for xbox_pc_match.py")
    ap.add_argument("--pc-image", help="the PC image xbox_pc_match.py was built from")
    ap.add_argument("--map-dir", help="the folder with map.tsv and the cache")
    args = ap.parse_args()
    if args.recomp:
        m.RECOMP = Path(args.recomp)
    if args.pc_image:
        m.PC_IMAGE = Path(args.pc_image)
    if args.map_dir:
        m.OUT = Path(args.map_dir)
    pf = m.cache("pc_image", m.parse_pc)
    starts = sorted(pf)
    raw = m.PC_IMAGE.read_bytes()
    names = {int(r["pc"], 16): r for r in m.load_map()}
    out = ["va\tkind\tcriticality\tour_name\tpc_function\txbox_name\tgraph\tpc_ret\tassumed_ret\txbox_sig\tflag\tfile"]
    with open(args.inventory, encoding="utf-8", errors="replace") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    for row in rows:
        crit = (row.get("criticality") or "").strip().lower()
        if args.only and crit != args.only:
            continue
        try:
            va = int(row["va"].strip(), 16)
        except (ValueError, KeyError):
            continue
        kind = (row.get("kind") or "").strip()
        if kind == "global" or not (m.PC_TEXT[0] <= va < m.PC_TEXT[1]):
            continue
        i = bisect.bisect_right(starts, va) - 1
        s = starts[i]
        e = pf[s]["end"]
        r = names.get(s)
        xname = r["name"] if r else ""
        graph = "%s/%s" % (r["agree"], r["clash"]) if r else ""
        sig = xbox_sig(int(r["xbox"], 16), r["source"]) if r else ""
        rets = pc_rets(raw, s, e)
        mine = assumed_pop(row.get("assumed_signature", ""))
        flags = []
        if kind in ("inline", "call", "slot") and va != s:
            flags.append("not a function start (+0x%X)" % (va - s))
        if mine is not None and rets and mine not in rets:
            flags.append("ret %s on PC, we assume %d" % ("/".join(str(x) for x in sorted(rets)), mine))
        st = sig_stack(sig)
        if st and mine is not None and kind in ("inline", "call", "slot"):
            member, params = st
            conv = (row.get("assumed_signature") or "").lower()
            if member and "thiscall" not in conv and "fastcall" not in conv and "mid" not in kind:
                flags.append("Xbox says member function, we do not pass this in ecx")
            if not member and "thiscall" in conv:
                flags.append("Xbox says no this, we use thiscall")
            if rets and max(rets) and max(rets) // 4 != params:
                flags.append("Xbox has %d params, PC pops %d bytes" % (params, max(rets)))
        out.append("\t".join([
            "0x%08X" % va, kind, crit, (row.get("our_name") or "").strip(), "0x%08X%s" % (s, "+0x%X" % (va - s) if va != s else ""),
            xname, graph, "/".join(str(x) for x in sorted(rets)), "" if mine is None else str(mine), sig, "; ".join(flags), (row.get("file_line") or row.get("file:line") or row.get("file") or "").strip()]))
    text = "\n".join(out) + "\n"
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
