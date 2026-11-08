"""Read-only: watch every StandardDoor of a running deadspace2.exe and report
which fields change (open/close state hunting).

usage: python tools/live_probe_doors.py <pid> [seconds=30]
Doors are found through the object registry ([0x0201C0A0] + 0x4074, 4096
buckets, chain at +0x4C) by vtable 0x01B4D5D8; the first 0x3D0 bytes are
sampled at 5 Hz and every dword that changes is listed per door.
"""
import ctypes as c
from ctypes import wintypes as w
import struct
import sys
import time

BASE = 0x400000
REGISTRY_RVA = 0x1C1C0A0
DOOR_VTABLE = 0x01B4D5D8
DOOR_SIZE = 0x3D0
kernel = c.WinDLL("kernel32", use_last_error=True)
kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel.OpenProcess.restype = w.HANDLE
kernel.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
kernel.ReadProcessMemory.restype = w.BOOL
handle = kernel.OpenProcess(0x0410, False, int(sys.argv[1]))
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0


def read(a, n):
    buf = c.create_string_buffer(n)
    got = c.c_size_t()
    if not kernel.ReadProcessMemory(handle, a, buf, n, c.byref(got)) or got.value != n:
        return None
    return buf.raw


def dw(a):
    b = read(a, 4)
    return struct.unpack("<I", b)[0] if b else None


LIST_RVA = 0x1C5B744   # NetReflectable chain: node-0x5C = entity, next at node+4
registry = dw(BASE + LIST_RVA)
doors = []
node = dw(BASE + LIST_RVA)
steps = 0
while node and steps < 8192:
    entity = node - 0x5C
    if dw(entity) == DOOR_VTABLE:
        doors.append(entity)
    node = dw(node + 4)
    steps += 1
print("registry=%08X doors=%d" % (registry, len(doors)))
if not doors:
    sys.exit(0)
base_samples = {d: read(d, DOOR_SIZE) for d in doors}
changes = {}
t0 = time.time()
while time.time() - t0 < seconds:
    time.sleep(0.2)
    for d in doors:
        now = read(d, DOOR_SIZE)
        before = base_samples[d]
        if not now or not before:
            continue
        for off in range(0, DOOR_SIZE, 4):
            if now[off:off + 4] != before[off:off + 4]:
                a = struct.unpack("<I", before[off:off + 4])[0]
                b = struct.unpack("<I", now[off:off + 4])[0]
                changes.setdefault(d, {}).setdefault(off, []).append((round(time.time() - t0, 1), a, b))
        base_samples[d] = now
for d, offs in changes.items():
    guid = struct.unpack("<4I", read(d + 0x24, 16))
    print("door %08X guid=%08X-%08X-%08X-%08X" % ((d,) + guid))
    for off, hist in sorted(offs.items()):
        if len(hist) > 40:
            print("   +%03X changed %d times (continuous)" % (off, len(hist)))
            continue
        print("   +%03X: %s" % (off, " ".join("%.1fs:%08X->%08X" % h for h in hist[:8])))
