"""Read-only: list the stream manager's TOC register of a running deadspace2.exe.

usage: python tools/live_probe_tocs.py <pid>
Prints count, each TOC's entry count, first/last identity hash and first name:
the same fingerprint runtime_coop_streams.inc sends.
"""
import ctypes as c
from ctypes import wintypes as w
import struct
import sys

BASE = 0x400000
STREAM_MANAGER_RVA = 0x1C1C098
kernel = c.WinDLL("kernel32", use_last_error=True)
kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel.OpenProcess.restype = w.HANDLE
kernel.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
kernel.ReadProcessMemory.restype = w.BOOL
handle = kernel.OpenProcess(0x0410, False, int(sys.argv[1]))
if not handle:
    sys.exit("OpenProcess failed: %d" % c.get_last_error())


def read(a, n):
    buf = c.create_string_buffer(n)
    got = c.c_size_t()
    if not kernel.ReadProcessMemory(handle, a, buf, n, c.byref(got)) or got.value != n:
        return None
    return buf.raw


def dw(a):
    b = read(a, 4)
    return struct.unpack("<I", b)[0] if b else None


def text(a, limit=60):
    b = read(a, limit)
    if not b:
        return "?"
    return b.split(b"\0", 1)[0].decode("ascii", "replace")


manager = dw(BASE + STREAM_MANAGER_RVA)
print("manager=%08X" % (manager or 0))
count = dw(manager + 0x130)
print("count=%s" % count)
for i in range(count or 0):
    toc = dw(manager + 0xB0 + i * 4)
    hdr = read(toc, 16)
    magic, version, h2, h3 = struct.unpack("<4I", hdr)
    entries = h2 & 0xFFFF
    p0 = dw(toc + 16)
    pn = dw(toc + 16 + (entries - 1) * 20) if entries else 0
    id0 = struct.unpack("<2I", read(p0, 8)) if p0 else (0, 0)
    idn = struct.unpack("<2I", read(pn, 8)) if pn else (0, 0)
    fp = ((id0[0] * 0x9E3779B1) ^ ((idn[0] + 0x7F4A7C15) & 0xFFFFFFFF) ^ (entries << 20)) & 0xFFFFFFFF
    print("  %2d toc=%08X magic=%08X v=%d entries=%d refs=%d fp=%08X first=%08X %s | last=%08X %s" % (
        i, toc, magic, version, entries, h3 >> 16, fp, id0[0], text(id0[1]), idn[0], text(idn[1])))

if len(sys.argv) > 2:
    i = int(sys.argv[2])
    toc = dw(manager + 0xB0 + i * 4)
    entries = struct.unpack("<4I", read(toc, 16))[2] & 0xFFFF
    print("entries of toc %d:" % i)
    for e in range(entries):
        raw = read(toc + 16 + e * 20, 20)
        p, a, b, cc, d = struct.unpack("<5I", raw)
        ident = struct.unpack("<2I", read(p, 8)) if p else (0, 0)
        print("  %3d p=%08X %08X %08X %08X %08X  hash=%08X %s" % (e, p, a, b, cc, d, ident[0], text(ident[1], 80)))
