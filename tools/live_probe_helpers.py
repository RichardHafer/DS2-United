"""Walk the listeners of hospital_lobby_slasher_dead and look for the counter."""
import ctypes as c
from ctypes import wintypes as w
import struct
import sys

BASE = 0x400000
MESSAGE_TABLE_RVA = 0x01C4CD8C
BUCKETS = 0x1000

kernel = c.WinDLL("kernel32", use_last_error=True)
kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel.OpenProcess.restype = w.HANDLE
kernel.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t,
                                     c.POINTER(c.c_size_t)]
kernel.ReadProcessMemory.restype = w.BOOL
handle = kernel.OpenProcess(0x1010, False, int(sys.argv[1]))


def read(a, n):
    b = c.create_string_buffer(n)
    d = c.c_size_t()
    if not kernel.ReadProcessMemory(handle, a, b, n, c.byref(d)) or d.value != n:
        return None
    return b.raw


def dw(a):
    r = read(a, 4)
    return None if r is None else struct.unpack("<I", r)[0]


def text(a, limit=90):
    out = bytearray()
    for i in range(limit):
        ch = read(a + i, 1)
        if ch is None or ch == b"\0":
            break
        out.extend(ch)
    s = out.decode("ascii", "replace")
    return s if len(s) > 2 and all(32 <= ord(x) < 127 for x in s) else None


def hexdump(address, span, indent="      "):
    raw = read(address, span)
    if raw is None:
        print(indent + "unreadable")
        return
    for off in range(0, span, 4):
        v = struct.unpack_from("<I", raw, off)[0]
        f = struct.unpack_from("<f", raw, off)[0]
        extra = ""
        n = text(v) if 0x00400000 < v < 0x7F000000 else None
        if n:
            extra += f'  "{n}"'
        elif 0.0001 < abs(f) < 1e6:
            extra += f"  f={f:g}"
        elif 0 < v < 256:
            extra += f"  ={v}"
        print(f"{indent}+{off:02X}: {v:08X}{extra}")


table = dw(BASE + MESSAGE_TABLE_RVA)

for label, wanted in (("hospital_lobby_slasher_dead", 0x9F2E78E7),):
    entry = dw(table + (wanted & (BUCKETS - 1)) * 4)
    while entry and dw(entry + 0x10) != wanted:
        entry = dw(entry)
    print(f"{label}: entry={entry:08X} refcount={dw(entry + 0x14) & 0xFFFF}")
    lst = dw(entry + 4)
    lists = 0
    while lst and lists < 8:
        node = dw(lst + 8)
        print(f"  list {lst:08X} head={node:08X}")
        nodes = 0
        while node and nodes < 64:
            owner_slots = read(node, 0x20)
            print(f"    node {node:08X}: " +
                  " ".join(f"{struct.unpack_from('<I', owner_slots, o)[0]:08X}"
                           for o in range(0, 0x20, 4)))
            # A listener node usually carries the object it belongs to; try each
            # word as an object and print anything that looks like a named one.
            for o in range(0, 0x20, 4):
                cand = struct.unpack_from("<I", owner_slots, o)[0]
                if not (0x00100000 < cand < 0x7F000000):
                    continue
                vt = dw(cand)
                if vt is None or not (0x00400000 < vt < 0x02000000):
                    continue
                print(f"      word+{o:02X} -> object {cand:08X} vtable={vt:08X}"
                      f" (rva {vt - BASE:07X})")
                hexdump(cand, 0x60, "        ")
            node = dw(node + 4)
            nodes += 1
        lst = dw(lst)
        lists += 1
