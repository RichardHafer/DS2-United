"""Recover message names: hash every string the process holds, match against registry keys."""
import struct, sys, json, re, ctypes as c
from ctypes import wintypes as w
exec(open("live_probe_helpers.py").read().split("table = dw(")[0])
def hash_name(s):
    h = 0
    for ch in s.lower(): h = (h * 0x1003F + ord(ch)) & 0xFFFFFFFF
    return h
table = dw(BASE + MESSAGE_TABLE_RVA)
keys = set()
for b in range(BUCKETS):
    e = dw(table + b * 4)
    while e:
        keys.add(dw(e + 0x10)); e = dw(e)
print("keys", len(keys))
class MBI(c.Structure):
    _fields_ = [("BaseAddress", c.c_void_p), ("AllocationBase", c.c_void_p), ("AllocationProtect", w.DWORD),
                ("RegionSize", c.c_size_t), ("State", w.DWORD), ("Protect", w.DWORD), ("Type", w.DWORD)]
kernel.VirtualQueryEx.argtypes = [w.HANDLE, c.c_void_p, c.POINTER(MBI), c.c_size_t]
kernel.VirtualQueryEx.restype = c.c_size_t
found = {}
a = 0x10000
pat = re.compile(rb"[A-Za-z0-9_\-\.]{4,64}")
scanned = 0
while a < 0x7FFF0000:
    mbi = MBI()
    if not kernel.VirtualQueryEx(handle, a, c.byref(mbi), c.sizeof(mbi)): break
    size = mbi.RegionSize
    if mbi.State == 0x1000 and mbi.Protect in (0x02, 0x04, 0x20, 0x40) and size < 0x20000000:
        raw = read(a, size)
        if raw:
            scanned += size
            for mt in pat.finditer(raw):
                s = mt.group().decode()
                h = hash_name(s)
                if h in keys and h not in found: found[h] = s
    a += size
print(f"scanned {scanned>>20} MB, named {len(found)} of {len(keys)}")
json.dump({f"{h:08X}": s for h, s in found.items()}, open("message_names.json", "w"), indent=0)
