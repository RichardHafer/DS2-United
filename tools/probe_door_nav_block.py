"""Read-only: the NavPower obstacle state of every StandardDoor.

    python -I tools/probe_door_nav_block.py --pid 23040 [door ...]
    python -I tools/probe_door_nav_block.py --dump C:/DS2CrashDumps/x.dmp [door ...]

Live mode opens the process with PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
(0x0410) and only calls ReadProcessMemory. Doors come from the NetReflectable
chain ([0x0205B744], node - 0x5C = entity, next at node + 4) plus any address
given on the command line.

Per door: state +0x234, previous +0x238, flags +0x240 (0x200 graph links cut),
locked +0x258, and its NavObstacleComponent (Base::GetComponent with the index
byte at 0x020B79EA): owner +0x10, ObstacleHandle +0x24 (valid when the handle
points at a nonzero link), NavSpace +0x28. A valid handle means BlockGraph's
obstacle is in the NavPower space.
"""
import argparse
import bisect
import ctypes as c
import struct
import sys
from ctypes import wintypes as w
from pathlib import Path

DOOR_VTABLE = 0x01B4D5D8
LIST_HEAD = 0x0205B744
NOC_INDEX = 0x020B79EA
SMART_INDEX = 0x020B7A5A

SIGS = [
    ("StandardDoor::BlockGraph", 0x00DB7DA0, "558bec83e4f083ec68568bf15785f6746c"),
    ("StandardDoor::UnblockGraph", 0x00DB7E30, "85c974180fb605ea790b0250e83f72a1ff85c07407"
                                               "8bc8e9343e5b00c3"),
    ("NavObstacleComponent::AddObstacle", 0x0136BD50, "8b44240c8b542408508b442408528b511050"
                                                      "5283c114e88584ffffc20c00"),
    ("NavObstacleComponent::RemoveObstacle", 0x0136BC80, "83c114e96884ffff"),
    ("NavInternalObstacle::RemoveObstacle", 0x013640F0, "83ec0c56578bf98d77108bcee86f8823ff84c07449"),
    ("NavInternalObstacle::AddObstacle", 0x013641F0, "83ec3c568bf1578d7e108bcfe86f8723ff84c07407"),
    ("tick: closed entry calls BlockGraph", 0x00DD286D, "8bcee82c55feff"),
    ("tick: open entry calls UnblockGraph", 0x00DD28D0, "8bcee85955feff"),
    ("SpawnInit: closed start calls BlockGraph", 0x00DC5DB1, "8bcee8e81fffff"),
    ("SpawnInit: ignition door calls BlockGraph", 0x00DC5DE4, "e8b71fffff"),
]


class Live:
    def __init__(self, pid):
        k = c.WinDLL("kernel32", use_last_error=True)
        k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        k.OpenProcess.restype = w.HANDLE
        k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
        k.ReadProcessMemory.restype = w.BOOL
        self.k = k
        self.h = k.OpenProcess(0x0410, False, pid)
        if not self.h:
            raise OSError("OpenProcess(0x0410, %d) failed: %d" % (pid, c.get_last_error()))
        self.name = "pid %d" % pid

    def read(self, a, n):
        buf = c.create_string_buffer(n)
        got = c.c_size_t()
        if not self.k.ReadProcessMemory(self.h, c.c_void_p(a), buf, n, c.byref(got)) or got.value != n:
            return None
        return buf.raw


class Dump:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.name = str(path)
        sig, _ver, count, rva = struct.unpack("<IIII", self.f.read(16))
        if sig != 0x504D444D:
            raise ValueError("not a minidump")
        streams = {}
        self.f.seek(rva)
        for _ in range(count):
            typ, size, srva = struct.unpack("<III", self.f.read(12))
            streams[typ] = (size, srva)
        self.ranges = []
        if 9 in streams:
            self.f.seek(streams[9][1])
            n, cursor = struct.unpack("<QQ", self.f.read(16))
            for _ in range(n):
                start, size = struct.unpack("<QQ", self.f.read(16))
                self.ranges.append((start, start + size, cursor))
                cursor += size
        self.ranges.sort()
        self.starts = [r[0] for r in self.ranges]

    def read(self, a, n):
        out = bytearray()
        while n > 0:
            i = bisect.bisect_right(self.starts, a) - 1
            if i < 0:
                return None
            start, end, off = self.ranges[i]
            if a >= end:
                return None
            take = min(n, end - a)
            self.f.seek(off + a - start)
            out += self.f.read(take)
            a += take
            n -= take
        return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--pid", type=int)
    g.add_argument("--dump", type=Path)
    ap.add_argument("doors", nargs="*")
    a = ap.parse_args()
    mem = Live(a.pid) if a.pid else Dump(a.dump)

    def dw(x):
        b = mem.read(x, 4)
        return struct.unpack("<I", b)[0] if b else None

    print("source:", mem.name)
    for name, va, hexbytes in SIGS:
        want = bytes.fromhex(hexbytes)
        got = mem.read(va, len(want))
        state = "match" if got == want else ("unreadable" if got is None else "MISMATCH " + got.hex())
        print("sig %-42s %08X %s" % (name, va, state))
    raw = mem.read(NOC_INDEX, 1)
    idx = raw[0] if raw else None
    print("NavObstacleComponent index byte [%08X] = %s" % (NOC_INDEX, "?" if idx is None else idx))

    doors = []
    node = dw(LIST_HEAD)
    steps = 0
    while node and steps < 16384:
        if dw(node - 0x5C) == DOOR_VTABLE:
            doors.append(node - 0x5C)
        node = dw(node + 4)
        steps += 1
    print("NetReflectable chain: %d nodes, %d StandardDoor(s)" % (steps, len(doors)))
    for extra in a.doors:
        d = int(extra, 16)
        if d not in doors:
            doors.append(d)

    def component(entity):
        if idx is None:
            return None
        arr, desc = dw(entity + 0x48), dw(entity + 0x44)
        if not arr or not desc:
            return 0
        slot = mem.read(desc + idx + 0x0C, 1)
        return dw(arr + slot[0] * 4) if slot else None

    blocked_open = 0
    for d in doors:
        vt = dw(d)
        if vt != DOOR_VTABLE:
            print("door %08X: vtable %s, not a StandardDoor (gone or reused)" % (d, "unreadable" if vt is None else "%08X" % vt))
            continue
        guid = struct.unpack("<4I", mem.read(d + 0x24, 16))
        pos = struct.unpack("<3f", mem.read(d + 0xC0, 12))
        state, last, flags = dw(d + 0x234), dw(d + 0x238), dw(d + 0x240)
        locked = mem.read(d + 0x258, 1)[0]
        noc = component(d)
        line = "door %08X %08X-%08X state %u/%u flags %08X%s locked %u at (%.1f %.1f %.1f)" % (
            d, guid[0], guid[1], state, last, flags, " links_cut" if flags & 0x200 else "", locked, *pos)
        if not noc:
            print(line + " | no NavObstacleComponent")
            continue
        owner, handle, space = dw(noc + 0x10), dw(noc + 0x24), dw(noc + 0x28)
        link = dw(handle) if handle else 0
        valid = bool(handle and link)
        if valid and state in (4, 5):
            blocked_open += 1
        print(line + " | noc %08X vt %08X owner %s handle %08X link %s space %08X -> %s" % (
            noc, dw(noc) or 0, "self" if owner == d else "%08X" % (owner or 0), handle or 0,
            "%08X" % link if link is not None else "?", space or 0, "BLOCKED" if valid else "unblocked"))
    print("open or opening doors with a live obstacle: %d" % blocked_open)

    # NavSmartObstacleComponent (index byte 0x020B7A5A, Update 0x0136C2F0): a woken NPC
    # that moved less than 0.1 m for 1 s becomes an obstacle; handle +0x2C, owner +0x10,
    # owner position +0x100.
    raw = mem.read(SMART_INDEX, 1)
    sidx = raw[0] if raw else None
    print("NavSmartObstacleComponent index byte [%08X] = %s" % (SMART_INDEX, "?" if sidx is None else sidx))
    if sidx is None:
        return
    node = dw(LIST_HEAD)
    steps = enabled = total = 0
    door_pos = []
    for d in doors:
        if dw(d) == DOOR_VTABLE:
            door_pos.append((d, struct.unpack("<3f", mem.read(d + 0xC0, 12)), dw(d + 0x234)))
    while node and steps < 16384:
        entity = node - 0x5C
        node = dw(node + 4)
        steps += 1
        arr, desc = dw(entity + 0x48), dw(entity + 0x44)
        if not arr or not desc:
            continue
        slot = mem.read(desc + sidx + 0x0C, 1)
        comp = dw(arr + slot[0] * 4) if slot else None
        if not comp:
            continue
        total += 1
        handle = dw(comp + 0x2C)
        on = bool(handle and dw(handle))
        enabled += on
        pos = struct.unpack("<3f", mem.read(entity + 0x100, 12))
        timer = struct.unpack("<f", mem.read(comp + 0x58, 4))[0]
        near = min(((sum((a - b) ** 2 for a, b in zip(pos, p)) ** 0.5, dd, st) for dd, p, st in door_pos), default=None)
        where = " nearest door %08X (state %u) %.1f m" % (near[1], near[2], near[0]) if near else ""
        print("npc %08X vt %08X at (%.1f %.1f %.1f) smart %08X obstacle %s timer %.2f%s" % (
            entity, dw(entity) or 0, *pos, comp, "ON" if on else "off", timer, where))
    print("smart obstacles: %d of %d components enabled" % (enabled, total))


if __name__ == "__main__":
    sys.stdout.reconfigure(errors="replace")
    main()
