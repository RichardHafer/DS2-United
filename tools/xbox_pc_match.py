"""Name the functions of the PC deadspace2.exe after the Xbox 360 build.

Sources (all local, nothing is downloaded or run):
  - the Xbox recomp ("Dead Space 2 Decompiled", ReXGlue): every retail Xbox function
    with its PDB name, and its calls, constants and global references read from the
    PPC disassembly in the comments;
  - the Xbox debug build ("Dead Space 2 Research"): which strings each named function
    references (the recomp carries no data section);
  - an unpacked snapshot of the PC image (work/pc_image): functions, calls, constants,
    globals and strings from x86.

Matching: strings and constants that occur in exactly one function on both sides
seed pairs. Aligned call lists, unique globals and unique callers spread them until
nothing new is found. Every pair keeps how it was found.

    xbox_pc_match.py build                 parse all three sources, match, write the map
    xbox_pc_match.py name 0x00A4BA90       the Xbox name of a PC function
    xbox_pc_match.py find SetInfo          PC addresses of Xbox names containing the text
    xbox_pc_match.py check                 precision and recall on tools/xbox_pc_known.tsv

Output: <game>/work/xbox_pc_match/map.tsv and a cache of the parsed sources.
"""
import argparse
import bisect
import collections
import ctypes
import csv
import json
import os
import pickle
import re
import struct
import sys
import time
from pathlib import Path

import capstone
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ds2_paths import GAME_DIR  # noqa: E402

RECOMP = GAME_DIR / "Dead Space 2 Decompiled"
RESEARCH = GAME_DIR / "Dead Space 2 Research"
PC_IMAGE = GAME_DIR / "work" / "pc_image" / "ds2_pc_image_2026-10-09.bin"
OUT = GAME_DIR / "work" / "xbox_pc_match"
KNOWN = Path(__file__).resolve().parent / "xbox_pc_known.tsv"

PC_BASE = 0x400000
PC_TEXT = (0x401000, 0x1AA1000)
PC_RDATA = (0x1AA1000, 0x1C30000)
PC_DATA = (0x1C30000, 0x2158000)
X_IMAGE = (0x82000000, 0x84000000)
DBG_BASE = 0x82000000
DBG_RDATA = (DBG_BASE + 0x400, DBG_BASE + 0x400 + 0x186D1B8)
DBG_TEXT = (DBG_BASE + 0x1930000, DBG_BASE + 0x1930000 + 0x156F4B8)
HELPERS = re.compile(r"^__(save|rest)(gpr|fpr|vmx)")


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# --------------------------------------------------------------------------- Xbox recomp
HEADER = re.compile(r"^// 0x([0-9A-F]{8})  (.+?)(?:  \[.*)?$")
ASM = re.compile(r"^\t// ([a-z][a-z0-9.+-]*)\s*(.*)$")
STORES = ("st", "cmp", "b", "mt", "dcb", "tw", "sync", "isync", "eieio", "lwsync")
MEMOP = re.compile(r"^(r\d+|f\d+),(-?\d+)\(r(\d+)\)$")


def parse_recomp():
    """{va: {name, sig, file, calls, tails, consts, globals, n}} from the recomp sources."""
    funcs = {}
    folders = ["DeadSpace", "EA", "EARS", "Havok", "XDK", "BFX"]
    files = [p for f in folders for p in (RECOMP / "src" / f).rglob("*.cpp")]
    log("recomp: %d files" % len(files))
    for path in files:
        cur = None
        lis = {}
        rel = str(path.relative_to(RECOMP / "src"))
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if line.startswith("// 0x"):
                    m = HEADER.match(line.rstrip("\n"))
                    if m:
                        va = int(m.group(1), 16)
                        cur = funcs.setdefault(va, {"name": m.group(2), "sig": "", "file": rel, "calls": [], "tails": [], "consts": [], "globals": [], "n": 0})
                        lis = {}
                    continue
                if cur is None:
                    continue
                if line.startswith("//   sig: "):
                    cur["sig"] = line[10:].strip()
                    continue
                m = ASM.match(line)
                if not m:
                    continue
                op, args = m.group(1), m.group(2).replace(" ", "")
                cur["n"] += 1
                parts = args.split(",")
                if op == "bl" and args.startswith("0x"):
                    cur["calls"].append(int(args, 16))
                    continue
                if op == "b" and args.startswith("0x"):
                    cur["tails"].append(int(args, 16))
                    continue
                if op == "lis" and len(parts) == 2 and parts[0].startswith("r"):
                    lis[parts[0]] = (int(parts[1]) & 0xFFFF) << 16
                    continue
                if op in ("ori", "oris") and len(parts) == 3 and parts[1] in lis:
                    v = lis[parts[1]] | (int(parts[2]) & 0xFFFF) if op == "ori" else lis[parts[1]] | ((int(parts[2]) & 0xFFFF) << 16)
                    (cur["globals"] if X_IMAGE[0] <= v < X_IMAGE[1] else cur["consts"]).append(v)
                elif op == "addi" and len(parts) == 3 and parts[1] in lis:
                    v = (lis[parts[1]] + int(parts[2])) & 0xFFFFFFFF
                    (cur["globals"] if X_IMAGE[0] <= v < X_IMAGE[1] else cur["consts"]).append(v)
                elif len(parts) == 2:
                    mm = MEMOP.match(args)
                    if mm and ("r" + mm.group(3)) in lis:
                        v = (lis["r" + mm.group(3)] + int(mm.group(2))) & 0xFFFFFFFF
                        if X_IMAGE[0] <= v < X_IMAGE[1]:
                            cur["globals"].append(v)
                # the destination of anything but a store, compare or branch is overwritten
                if parts and parts[0].startswith("r") and not op.startswith(STORES):
                    lis.pop(parts[0], None)
    log("recomp: %d functions" % len(funcs))
    return funcs


# --------------------------------------------------------------------------- Xbox debug build
def undecorate(names):
    dbghelp = ctypes.WinDLL("dbghelp")
    fn = dbghelp.UnDecorateSymbolName
    fn.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32]
    fn.restype = ctypes.c_uint32
    buf = ctypes.create_string_buffer(2048)
    out = {}
    for n in names:
        if not n.startswith("?"):
            out[n] = n
            continue
        r = fn(n.encode("latin-1", "replace"), buf, 2048, 0x1000)  # UNDNAME_NAME_ONLY
        out[n] = buf.value.decode("latin-1") if r else n
    return out


def norm(name):
    name = re.sub(r"\b(class|struct|enum|union) ", "", name)
    return name.replace(" ", "")


def debug_strings():
    """{normalized name: set(strings)} for every function of the Xbox debug build."""
    img = np.fromfile(RESEARCH / "executables" / "deadspace_d_basic_image_v1.bin", dtype=np.uint8)
    csv.field_size_limit(1 << 30)
    syms = {}
    with open(RESEARCH / "executables" / "deadspace_d_pub32_runtime_va_v2.csv", newline="", encoding="utf-8", errors="replace") as f:
        for row in csv.DictReader(f):
            va = row["runtime_va"]
            if va.startswith("0x"):
                v = int(va, 16)
                if DBG_TEXT[0] <= v < DBG_TEXT[1]:
                    syms.setdefault(v, row["symbol"])
    order = np.array(sorted(syms), dtype=np.int64)
    names = undecorate([syms[v] for v in order.tolist()])
    log("debug: %d text symbols" % len(order))
    t0 = DBG_TEXT[0] - DBG_BASE
    words = img[t0:t0 + ((DBG_TEXT[1] - DBG_TEXT[0]) // 4) * 4].view(">u4").astype(np.int64)
    opc = words >> 26
    rd = (words >> 21) & 31
    ra = (words >> 16) & 31
    imm = words & 0xFFFF
    simm = np.where(imm >= 0x8000, imm - 0x10000, imm)
    is_lis = (opc == 15) & (ra == 0)
    is_addi = (opc == 14) & (ra != 0)
    idx = np.nonzero(is_addi)[0]
    addr = np.full(idx.shape, -1, dtype=np.int64)
    for k in range(1, 9):
        j = idx - k
        ok = (j >= 0) & (addr < 0)
        jj = np.where(j >= 0, j, 0)
        hit = ok & is_lis[jj] & (rd[jj] == ra[idx])
        addr = np.where(hit, (imm[jj] << 16) + simm[idx], addr)
    sel = (addr >= DBG_RDATA[0]) & (addr < DBG_RDATA[1])
    idx, addr = idx[sel], addr[sel]
    raw = img.tobytes()
    out = collections.defaultdict(set)
    fnidx = np.searchsorted(order, DBG_TEXT[0] + idx * 4, side="right") - 1
    for fi, a in zip(fnidx.tolist(), addr.tolist()):
        if fi < 0:
            continue
        s = cstring(raw, a - DBG_BASE)
        if s:
            out[norm(names[syms[int(order[fi])]])].add(s)
    log("debug: %d named functions with strings" % len(out))
    return out


def debug_vtables():
    """[(vtable symbol, [slot names])] from the Xbox debug build; slot order is the same on PC."""
    raw = (RESEARCH / "executables" / "deadspace_d_basic_image_v1.bin").read_bytes()
    csv.field_size_limit(1 << 30)
    syms = {}
    with open(RESEARCH / "executables" / "deadspace_d_pub32_runtime_va_v2.csv", newline="", encoding="utf-8", errors="replace") as f:
        for row in csv.DictReader(f):
            va = row["runtime_va"]
            if va.startswith("0x"):
                syms.setdefault(int(va, 16), row["symbol"])
    order = sorted(syms)
    tables = []
    for va in order:
        name = syms[va]
        if not name.startswith("??_7") or not (DBG_RDATA[0] <= va < DBG_RDATA[1]):
            continue
        nxt = order[bisect.bisect_right(order, va)] if bisect.bisect_right(order, va) < len(order) else va + 4096
        slots = []
        for a in range(va, min(nxt, va + 4 * 512), 4):
            p = struct.unpack_from(">I", raw, a - DBG_BASE)[0]
            if not (DBG_TEXT[0] <= p < DBG_TEXT[1]):
                break
            slots.append(syms.get(p))
        if len(slots) >= 2:
            tables.append((name, slots))
    names = undecorate({s for _, slots in tables for s in slots if s})
    out = [(t, [norm(names[s]) if s else None for s in slots]) for t, slots in tables]
    log("debug: %d vtables" % len(out))
    return out


def pc_vtables(pf):
    """[(address, [function starts])]: .rdata addresses code refers to that start a run of function pointers."""
    raw = PC_IMAGE.read_bytes()
    referenced = sorted({g for f in pf.values() for g in f["globals"] if PC_RDATA[0] <= g < PC_RDATA[1]})
    refset = set(referenced)
    out = []
    for va in referenced:
        slots = []
        a = va
        while a < PC_RDATA[1]:
            p = struct.unpack_from("<I", raw, a - PC_BASE)[0]
            if p not in pf or (a != va and a in refset):
                break
            slots.append(p)
            a += 4
        if len(slots) >= 2:
            out.append((va, slots))
    log("pc: %d vtable candidates" % len(out))
    return out


def cstring(raw, off, limit=160):
    if off <= 0 or off >= len(raw):
        return None
    end = raw.find(b"\0", off, off + limit)
    if end < 0 or end - off < 4:
        return None
    s = raw[off:end]
    if any(b < 0x20 or b > 0x7E for b in s):
        return None
    return s.decode("latin-1")


# --------------------------------------------------------------------------- PC image
HEX = re.compile(r"0x[0-9a-f]+")


def parse_pc():
    raw = PC_IMAGE.read_bytes()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    lo, hi = PC_TEXT
    text = raw[lo - PC_BASE:hi - PC_BASE]
    arr = np.frombuffer(text, dtype=np.uint8)
    # MSVC pads between functions with int3 and aligns to 16
    cc = arr == 0xCC
    pos = np.nonzero((~cc[1:]) & cc[:-1])[0] + 1
    pos = pos[((pos + lo) % 16) == 0]
    # a single 0xCC can be an operand byte (call rel32): one int3 only counts after a ret or jmp
    run2 = cc[np.maximum(pos - 2, 0)]
    q = pos - 1
    after_end = (arr[np.maximum(q - 1, 0)] == 0xC3) | (arr[np.maximum(q - 3, 0)] == 0xC2) | (arr[np.maximum(q - 5, 0)] == 0xE9) | (arr[np.maximum(q - 2, 0)] == 0xEB)
    pos = pos[run2 | after_end]
    starts = set((pos + lo).tolist())
    # vtable entries: pointers from .rdata into .text
    rd = np.frombuffer(raw[PC_RDATA[0] - PC_BASE:PC_RDATA[1] - PC_BASE], dtype="<u4")
    vt = rd[(rd >= lo) & (rd < hi)]
    starts.update(int(v) for v in vt.tolist() if text[int(v) - lo] != 0xCC and (text[int(v) - lo - 1] == 0xCC or int(v) % 16 == 0))
    funcs = {}
    for rnd in range(3):
        funcs = {}  # ranges change once new starts split them
        order = sorted(starts)
        new = set()
        for i, s in enumerate(order):
            if s in funcs:
                continue
            e = order[i + 1] if i + 1 < len(order) else hi
            f = {"calls": [], "tails": [], "consts": [], "globals": [], "n": 0, "end": e}
            for addr, size, mnem, ops in md.disasm_lite(text[s - lo:e - lo], s):
                f["n"] += 1
                if mnem == "int3":
                    break
                vals = [int(h, 16) for h in HEX.findall(ops)]
                if mnem == "call" and vals and lo <= vals[0] < hi and ops.startswith("0x"):
                    f["calls"].append(vals[0])
                    if vals[0] not in starts:
                        new.add(vals[0])
                    continue
                if mnem == "jmp" and vals and ops.startswith("0x") and not (s <= vals[0] < e):
                    f["tails"].append(vals[0])
                    continue
                for v in vals:
                    if PC_RDATA[0] <= v < PC_DATA[1]:
                        f["globals"].append(v)
                    elif v >= 0x10000 and not (PC_BASE <= v < PC_DATA[1]) and v < 0xFFFF0000:
                        f["consts"].append(v)
            funcs[s] = f
        log("pc: round %d, %d functions, %d new call targets" % (rnd, len(funcs), len(new)))
        if not new or rnd == 2:
            break
        starts |= new
    strings = {}
    for f in funcs.values():
        for g in f["globals"]:
            if g < PC_RDATA[1] and g not in strings:
                strings[g] = cstring(raw, g - PC_BASE)
    for f in funcs.values():
        f["strings"] = sorted({strings[g] for g in f["globals"] if strings.get(g)})
    return funcs


# --------------------------------------------------------------------------- matching
class Matcher:
    def __init__(self, xf, pf, dstr, dvt, pvt, use_strings=True):
        self.xf, self.pf = xf, pf
        self.use_strings = use_strings
        self.dvt, self.pvt = dvt, pvt
        self.x2p, self.p2x, self.how = {}, {}, {}
        self.conflicts = self.rejected = 0
        helper = {va for va, f in xf.items() if HELPERS.match(f["name"])}
        for f in xf.values():
            f["calls"] = [c for c in f["calls"] if c not in helper and c in xf]
            f["tails"] = [t for t in f["tails"] if t in xf and t not in helper]
        # Xbox names -> retail VAs; only unique names bridge to the debug build
        self.byname = collections.defaultdict(list)
        for va, f in xf.items():
            self.byname[norm(f["name"])].append(va)
        self.xstr = collections.defaultdict(set)
        self.strs_of = {}
        for name, ss in dstr.items():
            vas = self.byname.get(name)
            if vas and len(vas) == 1:
                self.strs_of[vas[0]] = ss
                for s in ss:
                    self.xstr[s].add(vas[0])
        self.pstr = collections.defaultdict(set)
        for va, f in pf.items():
            for s in f["strings"]:
                self.pstr[s].add(va)
        self.xcallers = collections.defaultdict(set)
        for va, f in xf.items():
            for c in f["calls"] + f["tails"]:
                self.xcallers[c].add(va)
        self.pcallers = collections.defaultdict(set)
        for va, f in pf.items():
            for c in f["calls"] + f["tails"]:
                if c in pf:
                    self.pcallers[c].add(va)

    def similar(self, x, p, loose=False):
        """Size, call count and strings must agree; seeds from unique strings get a looser size test."""
        fx, fp = self.xf[x], self.pf[p]
        nx, np_ = fx["n"], fp["n"]
        lo, hi = (0.3, 3.5) if loose else (0.5, 2.2)
        if min(nx, np_) < 8:
            if abs(nx - np_) > (6 if loose else 4):
                return False
        elif not lo <= np_ / nx <= hi:
            return False
        if not loose:
            cx, cp = len(set(fx["calls"])), len(set(fp["calls"]))
            if max(cx, cp) >= 3 and min(cx, cp) < 0.5 * max(cx, cp):
                return False
            if max(cx, cp) < 3 and abs(cx - cp) > 2:
                return False
        sx, sp = self.strs_of.get(x), fp["strings"]
        if self.use_strings and sx and sp and not (sx & set(sp)):
            return False
        return True

    def pair(self, x, p, how, loose=False):
        if x in self.x2p or p in self.p2x:
            if self.x2p.get(x) != p:
                self.conflicts += 1
            return False
        if not self.similar(x, p, loose):
            self.rejected += 1
            return False
        self.x2p[x], self.p2x[p], self.how[x] = p, x, how
        return True

    def unique_votes(self, xmap, pmap, how, loose=False):
        votes = collections.Counter()
        for key, xs in xmap.items():
            ps = pmap.get(key)
            if ps and len(xs) == 1 and len(ps) == 1:
                votes[(next(iter(xs)), next(iter(ps)))] += 1
        byx, byp = collections.defaultdict(set), collections.defaultdict(set)
        for (x, p) in votes:
            byx[x].add(p)
            byp[p].add(x)
        n = 0
        for (x, p), v in votes.most_common():
            if len(byx[x]) == 1 and len(byp[p]) == 1 and self.pair(x, p, "%s:%d" % (how, v), loose):
                n += 1
        return n

    def distance(self, x, p):
        """0 for identical shape; size, call, global and constant counts on a log scale."""
        fx, fp = self.xf[x], self.pf[p]

        def d(a, b, k):
            return abs(np.log((a + k) / (b + k)))
        return (d(fx["n"], fp["n"], 6) + 0.7 * d(len(set(fx["calls"])), len(set(fp["calls"])), 2) +
                0.4 * d(len(set(fx["globals"])), len(set(fp["globals"])), 2) + 0.2 * d(len(set(fx["consts"])), len(set(fp["consts"])), 2))

    def score(self, x, p):
        """distance plus what the graph says: matched callees and callers that agree pull a pair
        together, ones that point elsewhere push it apart; a shared rare constant helps too."""
        fx, fp = self.xf[x], self.pf[p]
        s = self.distance(x, p)
        pcalls = set(fp["calls"] + fp["tails"])
        agree = clash = 0
        for c in set(fx["calls"] + fx["tails"]):
            q = self.x2p.get(c)
            if q is not None:
                if q in pcalls:
                    agree += 1
                else:
                    clash += 1
        pcallers = self.pcallers.get(p, set())
        for c in self.xcallers.get(x, ()):
            q = self.x2p.get(c)
            if q is not None:
                if q in pcallers:
                    agree += 1
                else:
                    clash += 1
        if agree + clash:
            s += 0.6 * clash / (agree + clash) - 0.1 * min(agree, 3)
        cx, cp = set(fx["consts"]), set(fp["consts"])
        if cx & cp:
            s -= 0.15
        elif len(cx) >= 2 and len(cp) >= 2:
            s += 0.2
        return s

    def graph(self, x, p):
        """(agree, clash) over the matched callees and callers of a pair."""
        fx, fp = self.xf[x], self.pf[p]
        pn = set(fp["calls"] + fp["tails"]) | self.pcallers.get(p, set())
        agree = clash = 0
        for c in set(fx["calls"] + fx["tails"]) | self.xcallers.get(x, set()):
            q = self.x2p.get(c)
            if q is not None:
                if q in pn:
                    agree += 1
                else:
                    clash += 1
        return agree, clash

    def vtable_slot(self, s):
        vas = self.byname.get(s) if s else None
        return vas[0] if vas and len(vas) == 1 else None

    def vtables(self):
        """A PC vtable is the debug vtable of the same length whose slots look most alike;
        slots already matched must agree. Its slot names then name the PC functions."""
        bylen = collections.defaultdict(list)
        for name, slots in self.dvt:
            xs = [self.vtable_slot(s) for s in slots]
            if sum(1 for x in xs if x) >= max(2, len(xs) // 2):
                bylen[len(slots)].append((name, xs))
        cand = collections.defaultdict(set)
        for va, ps in self.pvt:
            scored = []
            for name, xs in bylen.get(len(ps), ()):
                total, used, clash, agree = 0.0, 0, 0, 0
                for p, x in zip(ps, xs):
                    if x is None:
                        continue
                    if p in self.p2x or x in self.x2p:
                        if self.p2x.get(p) == x:
                            agree += 1
                        else:
                            clash += 1
                        continue
                    total += self.distance(x, p)
                    used += 1
                if clash > agree:
                    continue
                score = (total / used if used else 0.0) - 0.3 * agree + 0.3 * clash
                scored.append((score, name, xs))
            if not scored:
                continue
            scored.sort(key=lambda t: t[0])
            best = scored[0]
            if best[0] > 0.35 or (len(scored) > 1 and scored[1][0] - best[0] < 0.15):
                continue
            for p, x in zip(ps, best[2]):
                if x is not None and p not in self.p2x and x not in self.x2p:
                    cand[p].add(x)
        n = 0
        byx = collections.Counter(next(iter(xs)) for xs in cand.values() if len(xs) == 1)
        for p, xs in cand.items():
            # a PC function in slots of different names was folded by the linker
            if len(xs) == 1:
                x = next(iter(xs))
                if byx[x] == 1 and self.pair(x, p, "vtable", loose=True):
                    n += 1
        return n

    def neighbours(self):
        """Unmatched callees, tails and callers of every pair: the mutually closest shapes pair up."""
        best_x, best_p = {}, {}

        def offer(x, p, dist):
            for table, key, other in ((best_x, x, p), (best_p, p, x)):
                cur = table.get(key)
                if cur is None:
                    table[key] = [dist, other, 9.0]
                elif other == cur[1]:
                    cur[0] = min(cur[0], dist)
                elif dist < cur[0]:
                    table[key] = [dist, other, cur[0]]
                elif dist < cur[2]:
                    cur[2] = dist
        for x, p in list(self.x2p.items()):
            fx, fp = self.xf[x], self.pf[p]
            for xs, ps in (([c for c in fx["calls"] + fx["tails"]], [c for c in fp["calls"] + fp["tails"] if c in self.pf]),
                           (list(self.xcallers.get(x, ())), list(self.pcallers.get(p, ())))):
                xs = [c for c in dict.fromkeys(xs) if c not in self.x2p]
                ps = [c for c in dict.fromkeys(ps) if c not in self.p2x]
                if not xs or not ps or len(xs) * len(ps) > 1600:
                    continue
                for a in xs:
                    for b in ps:
                        offer(a, b, self.score(a, b))
        n = 0
        for x, (dist, p, second) in best_x.items():
            back = best_p.get(p)
            if back and back[1] == x and dist <= 0.25 and second - dist >= 0.15 and back[2] - dist >= 0.15:
                if self.pair(x, p, "near:%.2f" % dist):
                    n += 1
        return n

    def seed(self):
        if self.use_strings:
            n = self.unique_votes(self.xstr, self.pstr, "string", loose=True)
            log("seed strings: %d" % n)
        xc, pc = collections.defaultdict(set), collections.defaultdict(set)
        for va, f in self.xf.items():
            for c in set(f["consts"]):
                xc[c].add(va)
        for va, f in self.pf.items():
            for c in set(f["consts"]):
                pc[c].add(va)
        # small or round values say nothing
        keep = {c for c in xc if c > 0xFFFF and bin(c).count("1") >= 6 and c & 0xFFFF}
        n = self.unique_votes({c: xc[c] for c in keep}, pc, "const")
        log("seed constants: %d" % n)

    def align(self, xs, ps, how):
        """Pair two ordered callee lists between already matched anchors."""
        n = 0
        xs = list(dict.fromkeys(xs))
        ps = list(dict.fromkeys(ps))
        anchors = [(i, ps.index(self.x2p[x])) for i, x in enumerate(xs) if x in self.x2p and self.x2p[x] in ps]
        # keep only increasing anchors
        mono, last = [], -1
        for i, j in anchors:
            if j > last:
                mono.append((i, j))
                last = j
        bounds = [(-1, -1)] + mono + [(len(xs), len(ps))]
        for (a0, b0), (a1, b1) in zip(bounds, bounds[1:]):
            gx, gp = xs[a0 + 1:a1], ps[b0 + 1:b1]
            # only a single unmatched function between two anchors is safe to pair by position
            if len(gx) == 1 and len(gp) == 1:
                for x, p in zip(gx, gp):
                    if x not in self.x2p and p not in self.p2x and self.pair(x, p, how):
                        n += 1
        return n

    def spread(self):
        for rnd in range(30):
            n = 0
            for x, p in list(self.x2p.items()):
                n += self.align(self.xf[x]["calls"], self.pf[p]["calls"], "calls")
                n += self.align(self.xf[x]["tails"], [t for t in self.pf[p]["tails"] if t in self.pf], "tails")
            n += self.neighbours()
            n += self.globals_round()
            n += self.vtables()
            log("spread round %d: +%d (total %d, conflicts %d, rejected %d)" % (rnd, n, len(self.x2p), self.conflicts, self.rejected))
            if not n:
                break

    def string_agreement(self):
        """Pairs not found through strings, where both sides carry strings: how often do they share one?"""
        stats = collections.defaultdict(lambda: [0, 0])
        for x, p in self.x2p.items():
            sx, sp = self.strs_of.get(x), self.pf[p]["strings"]
            if not sx or not sp:
                continue
            how = self.how[x].split(":")[0]
            stats[how][0 if sx & set(sp) else 1] += 1
        return {k: "%d agree / %d differ" % tuple(v) for k, v in stats.items()}

    def globals_round(self):
        # matched pairs with equally long global lists teach the global map
        gvotes = collections.Counter()
        for x, p in self.x2p.items():
            gx = list(dict.fromkeys(self.xf[x]["globals"]))
            gp = list(dict.fromkeys(self.pf[p]["globals"]))
            if gx and len(gx) == len(gp):
                for a, b in zip(gx, gp):
                    gvotes[(a, b)] += 1
        gmap, seen = {}, set()
        for (a, b), v in gvotes.most_common():
            if a not in gmap and b not in seen:
                gmap[a] = b
                seen.add(b)
        self.gmap = gmap
        xg, pg = collections.defaultdict(set), collections.defaultdict(set)
        for va, f in self.xf.items():
            for g in set(f["globals"]):
                if g in gmap:
                    xg[gmap[g]].add(va)
        for va, f in self.pf.items():
            for g in set(f["globals"]):
                pg[g].add(va)
        return self.unique_votes(xg, pg, "global")


# --------------------------------------------------------------------------- commands
def cache(name, fn):
    OUT.mkdir(parents=True, exist_ok=True)
    p = OUT / (name + ".pickle")
    if p.exists():
        return pickle.loads(p.read_bytes())
    v = fn()
    p.write_bytes(pickle.dumps(v, protocol=4))
    return v


def cmd_build(args):
    if args.fresh:
        for p in OUT.glob("*.pickle"):
            p.unlink()
    xf = cache("xbox_recomp", parse_recomp)
    dstr = cache("xbox_debug_strings", debug_strings)
    pf = cache("pc_image", parse_pc)
    dvt = cache("xbox_debug_vtables", debug_vtables)
    pvt = cache("pc_vtables", lambda: pc_vtables(pf))
    m = Matcher(xf, pf, dstr, dvt, pvt)
    m.seed()
    m.spread()
    log("string confirmation of pairs found otherwise: %s" % m.string_agreement())
    rows = []
    suspect = 0
    for x, p in sorted(m.x2p.items(), key=lambda t: t[1]):
        f = xf[x]
        agree, clash = m.graph(x, p)
        suspect += clash > agree
        rows.append("0x%08X\t0x%08X\t%s\t%d\t%d\t%s\t%d\t%d\t%s" % (p, x, m.how[x], agree, clash, f["name"], pf[p]["n"], f["n"], f["file"]))
    (OUT / "map.tsv").write_text("pc\txbox\thow\tagree\tclash\tname\tpc_instr\txbox_instr\tsource\n" + "\n".join(rows) + "\n", encoding="utf-8")
    log("graph check: %d pairs where more matched neighbours disagree than agree" % suspect)
    hows = collections.Counter(h.split(":")[0] for h in m.how.values())
    log("map: %d of %d PC functions named (%s); written %s" % (len(rows), len(pf), dict(hows), OUT / "map.tsv"))
    if KNOWN.exists():
        cmd_check(args)


def cmd_validate(args):
    """Match without any string evidence, then score the result on the pairs strings alone give."""
    xf = cache("xbox_recomp", parse_recomp)
    dstr = cache("xbox_debug_strings", debug_strings)
    pf = cache("pc_image", parse_pc)
    dvt = cache("xbox_debug_vtables", debug_vtables)
    pvt = cache("pc_vtables", lambda: pc_vtables(pf))
    truth = Matcher(xf, pf, dstr, dvt, pvt)
    truth.unique_votes(truth.xstr, truth.pstr, "string", loose=True)
    blind = Matcher(xf, pf, dstr, dvt, pvt, use_strings=False)
    blind.seed()
    blind.spread()
    per = collections.defaultdict(lambda: [0, 0])
    for x, p in truth.x2p.items():
        q = blind.x2p.get(x)
        if q is None:
            continue
        per[blind.how[x].split(":")[0]][0 if q == p else 1] += 1
    for how, (good, bad) in sorted(per.items()):
        print("%-8s %5d right %4d wrong  precision %.3f" % (how, good, bad, good / max(1, good + bad)))
    good = sum(v[0] for v in per.values())
    bad = sum(v[1] for v in per.values())
    print("all      %5d right %4d wrong  precision %.3f; %d of %d string pairs re-found" % (good, bad, good / max(1, good + bad), good + bad, len(truth.x2p)))


def load_map():
    out = []
    with open(OUT / "map.tsv", encoding="utf-8") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            out.append(row)
    return out


def cmd_name(args):
    va = int(args.what, 16)
    pf = cache("pc_image", parse_pc)
    starts = sorted(pf)
    i = bisect.bisect_right(starts, va) - 1
    s = starts[i] if i >= 0 else None
    rows = {int(r["pc"], 16): r for r in load_map()}
    if s in rows:
        r = rows[s]
        print("0x%08X%s = %s  (xbox %s, %s, graph %s/%s, %s)" % (s, "+0x%X" % (va - s) if va != s else "", r["name"], r["xbox"], r["how"], r["agree"], r["clash"], r["source"]))
    else:
        print("0x%08X: no name (function starts at %s)" % (va, "0x%08X" % s if s is not None else "?"))


def cmd_find(args):
    for r in load_map():
        if args.what.lower() in r["name"].lower():
            print(r["pc"], r["name"], r["how"], "graph %s/%s" % (r["agree"], r["clash"]))


def cmd_check(args):
    rows = {int(r["pc"], 16): r for r in load_map()}
    good = bad = missing = 0
    for line in KNOWN.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        pc, name = line.split("\t")[:2]
        r = rows.get(int(pc, 16))
        if not r:
            missing += 1
            print("  missing %s %s" % (pc, name))
        elif norm(r["name"]) == norm(name):
            good += 1
        else:
            bad += 1
            print("  WRONG   %s %s -> %s (%s)" % (pc, name, r["name"], r["how"]))
    total = good + bad + missing
    print("known pairs: %d right, %d wrong, %d not named (of %d)" % (good, bad, missing, total))


def main():
    global RECOMP, RESEARCH, PC_IMAGE, OUT, KNOWN
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["build", "name", "find", "check", "validate"])
    ap.add_argument("what", nargs="?")
    ap.add_argument("--fresh", action="store_true", help="parse the sources again")
    ap.add_argument("--recomp", help="the recomp folder (default %s)" % RECOMP)
    ap.add_argument("--research", help="the Xbox debug research folder (default %s)" % RESEARCH)
    ap.add_argument("--pc-image", help="flat unpacked PC image, file offset = VA - 0x400000 (default %s)" % PC_IMAGE)
    ap.add_argument("--out", help="map and cache folder (default %s)" % OUT)
    ap.add_argument("--known", help="known pairs for check (default %s)" % KNOWN)
    args = ap.parse_args()
    RECOMP = Path(args.recomp) if args.recomp else RECOMP
    RESEARCH = Path(args.research) if args.research else RESEARCH
    PC_IMAGE = Path(args.pc_image) if args.pc_image else PC_IMAGE
    OUT = Path(args.out) if args.out else OUT
    KNOWN = Path(args.known) if args.known else KNOWN
    {"build": cmd_build, "name": cmd_name, "find": cmd_find, "check": cmd_check, "validate": cmd_validate}[args.cmd](args)


if __name__ == "__main__":
    main()
