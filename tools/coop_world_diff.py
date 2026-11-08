"""Compare the live worlds of two running Dead Space 2 co-op instances.

Every level entity (a vtable with a known class and a level GUID at +0x24) is
read from both processes. Shown for everything near any player:
entities only one world has, entities with the same GUID more than a metre
apart, a different hide count (+0x16D, Entity::Hide/Show) and a different door
state (StandardDoor +0x234), and the level variables (the 0x18-byte rows
{name hash, second hash, value, 0, flags, ptr} that VariableOperators set and
VariableSwitches read; hashes from analysis/coop_sweep/variables.tsv). What the
co-op leaves unshared turns up here as a
difference (25.09.: a withheld MultiRemoveTarget left the client two props the
host had deleted). Match the GUIDs against the coop_gap lines of DS2United*.log.

--watch records a whole play session: a snapshot every N seconds, a difference
counts once it is there in two snapshots in a row, and each one is tagged with
its level, class and gap family from the offline pass
(tools/coop_campaign_sweep.py, analysis/coop_sweep/coverage.tsv). Ctrl+C ends
it with a summary.

usage: python tools/coop_world_diff.py [pid pid] [--radius 30]
       python tools/coop_world_diff.py --watch 15 [--minutes 120] [--out FILE]
       (default: the two running deadspace2.exe, the older one first)
"""
import argparse
import collections
import ctypes
import ctypes.wintypes as w
import json
import math
import struct
import subprocess
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
VTABLE_NAMES = Path(__file__).with_name("vt2n.json")
COVERAGE = ROOT / "analysis" / "coop_sweep" / "coverage.tsv"
VARIABLES = ROOT / "analysis" / "coop_sweep" / "variables.tsv"
kernel = ctypes.windll.kernel32
SKIP = {"FOVAspectCameraModifierInfo", "Player", "PlayerMX9Inventory", "AchievementAward", "MultiManager"}
HIDE, DOOR_STATE = 0x16D, 0x234
# The hide count is an Entity field; compared only for things a player sees.
VISIBLE = {"ScriptableProp", "DamageableProp", "DismemberableProp", "DrivenScriptableProp", "LinearScriptableProp", "SecondaryAnimScriptableProp",
           "DistortedDamageableProp", "StandardContainer", "BreakableContainer", "PowerUp", "SpareAmmo", "Item", "KeyItem", "Schematic", "GunProxy",
           "MultiAmmo", "Destructible", "StandardDoor", "SimpleOneTimeDoor", "Mover", "TKReceptacle", "TKReceptacleTarget", "SolarPuzzle",
           "LinearHandle", "Switch", "PowerNodeSwitch", "SimplePressAndHoldSwitch", "HackingStationMiniGame", "VentMarker"}


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", w.DWORD),
                ("PartitionId", w.WORD), ("RegionSize", ctypes.c_size_t), ("State", w.DWORD), ("Protect", w.DWORD), ("Type", w.DWORD)]


def read(handle, address, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    ok = kernel.ReadProcessMemory(handle, ctypes.c_void_p(address), buf, size, ctypes.byref(got))
    return buf.raw[:got.value] if ok else None


def load_variables():
    """name hash -> readable name (or the hash) of every level variable."""
    table = {}
    if VARIABLES.exists():
        with VARIABLES.open(encoding="utf-8") as f:
            next(f, None)
            for line in f:
                h, name = line.rstrip("\n").split("\t")[:2]
                table[int(h, 16)] = name or h
    return table


VAR_NAMES = load_variables()
VAR_HASHES = np.array(sorted(VAR_NAMES), dtype="<u4")


def scan(pid, names, vtables):
    """GUID -> (class, position, hide count, door state or None); the Players' positions;
    variable hash -> the values of the rows that look like the live table's."""
    handle = kernel.OpenProcess(0x0410, False, pid)
    if not handle:
        raise SystemExit(f"cannot open process {pid}")
    entities, players, variables = {}, [], {}
    address = 0x10000
    try:
        while address < 0x7FFF0000:
            mbi = MBI()
            if not kernel.VirtualQueryEx(handle, ctypes.c_void_p(address), ctypes.byref(mbi), ctypes.sizeof(mbi)):
                break
            start, size = mbi.BaseAddress or 0, mbi.RegionSize
            if mbi.State == 0x1000 and mbi.Type == 0x20000 and mbi.Protect in (0x04, 0x40) and size < 0x10000000:
                raw = read(handle, start, size)
                if raw and len(raw) > 0x240:
                    words = np.frombuffer(raw[:len(raw) & ~3], dtype="<u4")
                    for index in np.nonzero(np.isin(words, VAR_HASHES))[0].tolist():
                        if index + 6 > len(words):
                            continue
                        h, second, value, zero, _, ptr = (int(x) for x in words[index:index + 6])
                        if second and zero == 0 and (ptr == 0 or 0x10000 <= ptr < 0x7FFF0000):
                            variables.setdefault(h, set()).add(value)
                    hits = np.nonzero(np.isin(words, vtables))[0] * 4
                    for off in hits.tolist():
                        if off + 0x240 > len(raw):
                            continue
                        tag = raw[off + 0x2C:off + 0x34]
                        if not all(48 <= c <= 90 for c in tag):   # level GUIDs end in an ASCII content tag
                            continue
                        name = names[str(struct.unpack_from("<I", raw, off)[0])]
                        pos = struct.unpack_from("<3f", raw, off + 0xC0)
                        guid = "%08X-%08X-%08X-%08X" % struct.unpack_from("<4I", raw, off + 0x24)
                        if name == "Player":
                            players.append(pos)
                        door = struct.unpack_from("<I", raw, off + DOOR_STATE)[0] if name == "StandardDoor" else None
                        entities.setdefault(guid, (name, pos, raw[off + HIDE], door))
            address = start + size
    finally:
        kernel.CloseHandle(handle)
    return entities, players, {h: tuple(sorted(v)) for h, v in variables.items()}


def running_pids():
    """The running games, the one started first (normally the host) first."""
    # bytes: a German "no tasks" line is not valid cp1252
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq deadspace2.exe", "/FO", "CSV", "/NH"], capture_output=True).stdout.decode("latin-1")
    pids = [int(line.split('","')[1]) for line in out.splitlines() if line.startswith('"deadspace2')]

    def started(pid):
        handle = kernel.OpenProcess(0x1000, False, pid)
        times = [w.FILETIME() for _ in range(4)]
        ok = handle and kernel.GetProcessTimes(handle, *[ctypes.byref(t) for t in times])
        if handle:
            kernel.CloseHandle(handle)
        return (times[0].dwHighDateTime << 32 | times[0].dwLowDateTime) if ok else 0
    return sorted(pids, key=started)


def show(value):
    f = struct.unpack("<f", struct.pack("<I", value))[0]
    return f"{value}" if value < 0x10000 or not math.isfinite(f) else f"{value} ({f:g})"


def differences(a, b, players, radius, va=None, vb=None):
    """(kind, guid, class, detail) for everything near a player that differs."""
    near = lambda p: any(math.dist(p, q) < radius for q in players)
    out = []
    for h in (va or {}).keys() & (vb or {}).keys():
        if va[h] != vb[h]:
            out.append(("variable", f"{h:08X}", VAR_NAMES.get(h, ""), {"first": [show(x) for x in va[h]], "second": [show(x) for x in vb[h]]}))
    for kind, mine, other in (("only_first", a, b), ("only_second", b, a)):
        for guid, (name, pos, _, _) in mine.items():
            if guid not in other and name not in SKIP and near(pos):
                out.append((kind, guid, name, {"pos": [round(x, 1) for x in pos]}))
    for guid in a.keys() & b.keys():
        (na, p1, h1, d1), (nb, p2, h2, d2) = a[guid], b[guid]
        if na != nb or na in SKIP or not (near(p1) or near(p2)):
            continue
        if math.dist(p1, p2) > 1.0:
            out.append(("moved", guid, na, {"d": round(math.dist(p1, p2), 1), "first": [round(x, 1) for x in p1], "second": [round(x, 1) for x in p2]}))
        if na in VISIBLE and (h1 > 0) != (h2 > 0):
            out.append(("hidden", guid, na, {"first": h1, "second": h2, "pos": [round(x, 1) for x in p1]}))
        if d1 is not None and d1 != d2:
            out.append(("door_state", guid, na, {"first": d1, "second": d2, "pos": [round(x, 1) for x in p1]}))
    return out


def load_coverage():
    table = {}
    if COVERAGE.exists():
        with COVERAGE.open(encoding="utf-8") as f:
            next(f, None)
            for line in f:
                guid, level, cls, coverage, family = line.rstrip("\n").split("\t")
                table[guid] = (level, coverage, family)
    return table


def one_shot(pids, names, vtables, radius):
    (a, pa, va), (b, pb, vb) = scan(pids[0], names, vtables), scan(pids[1], names, vtables)
    print(f"{pids[0]}: {len(a)} entities, {len(va)} variables; {pids[1]}: {len(b)} entities, {len(vb)} variables; {len(pa) + len(pb)} players")
    coverage = load_coverage()
    rows = sorted(differences(a, b, pa + pb, radius, va, vb), key=lambda r: (r[0], r[2], r[1]))
    for kind, guid, name, detail in rows:
        level, cov, family = coverage.get(guid, ("?", "?", ""))
        print(f"  {kind:11} {name:28} {guid} {level} {family or cov} {json.dumps(detail)}")
    print(f"{len(rows)} differences")


def watch(pids, names, vtables, args):
    coverage = load_coverage()
    out_path = args.out or ROOT / "analysis" / "coop_sweep" / time.strftime("watch_%Y%m%d_%H%M%S.jsonl")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    seen_once, open_diffs = set(), {}
    tally = collections.Counter()
    started = time.time()
    snapshots = 0
    print(f"watching {pids[0]} (first) and {pids[1]} (second) every {args.watch} s into {out_path}", flush=True)
    with out_path.open("a", encoding="utf-8") as out:
        def emit(event):
            out.write(json.dumps(event) + "\n")
            out.flush()
        try:
            while not args.minutes or time.time() - started < args.minutes * 60:
                tick = time.time()
                try:
                    (a, pa, va), (b, pb, vb) = scan(pids[0], names, vtables), scan(pids[1], names, vtables)
                except SystemExit as gone:
                    print(f"stopped: {gone}", flush=True)
                    break
                snapshots += 1
                now = {(k, g): (n, d) for k, g, n, d in differences(a, b, pa + pb, args.radius, va, vb)}
                fresh = 0
                for key, (name, detail) in now.items():
                    if key in open_diffs:
                        continue
                    if key not in seen_once:
                        continue   # needs a second snapshot in a row
                    level, cov, family = coverage.get(key[1], ("?", "unknown", "logic_state" if key[0] == "variable" else ""))
                    event = {"t": time.strftime("%H:%M:%S"), "event": "diff", "kind": key[0], "guid": key[1], "class": name, "level": level,
                             "coverage": cov, "family": family, "detail": detail, "players": [[round(x, 1) for x in p] for p in pa + pb]}
                    emit(event)
                    open_diffs[key] = event
                    tally[(family or ("covered:" + cov), name, key[0])] += 1
                    fresh += 1
                for key in [k for k in open_diffs if k not in now]:
                    emit({"t": time.strftime("%H:%M:%S"), "event": "resolved", "kind": key[0], "guid": key[1], "class": open_diffs.pop(key)["class"]})
                seen_once = set(now)
                print(f"{time.strftime('%H:%M:%S')} snapshot {snapshots}: {len(a)}/{len(b)} entities, {len(va)}/{len(vb)} variables, {len(pa) + len(pb)} players, "
                      f"{len(now)} differences, {fresh} new, {len(open_diffs)} open ({time.time() - tick:.1f} s)", flush=True)
                time.sleep(max(0.0, args.watch - (time.time() - tick)))
        except KeyboardInterrupt:
            pass
        summary = [{"family": f, "class": c, "kind": k, "count": n} for (f, c, k), n in tally.most_common()]
        emit({"t": time.strftime("%H:%M:%S"), "event": "summary", "snapshots": snapshots, "rows": summary})
    print(f"\n{snapshots} snapshots; differences by family, class and kind:")
    for row in summary:
        print(f"  {row['count']:5}  {row['family']:28} {row['class']:28} {row['kind']}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pids", nargs="*", type=int)
    parser.add_argument("--radius", type=float, default=30.0)
    parser.add_argument("--watch", type=float, default=0.0, help="seconds between snapshots; 0 = one comparison")
    parser.add_argument("--minutes", type=float, default=0.0, help="stop the watch after this long (0 = until Ctrl+C)")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    pids = args.pids or running_pids()
    if len(pids) != 2:
        raise SystemExit(f"need two game processes, found {pids}")
    names = json.loads(VTABLE_NAMES.read_text())
    vtables = np.array(sorted(int(k) for k in names), dtype="<u4")
    if args.watch > 0:
        watch(pids, names, vtables, args)
    else:
        one_shot(pids, names, vtables, args.radius)


if __name__ == "__main__":
    main()
