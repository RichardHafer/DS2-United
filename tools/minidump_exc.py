import struct, sys, mmap
p = sys.argv[1]
f = open(p, "rb"); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
sig, ver, nstreams, diroff = struct.unpack_from("<4sIII", m, 0)
assert sig == b"MDMP", sig
streams = {}
for i in range(nstreams):
    t, sz, off = struct.unpack_from("<III", m, diroff + 12 * i)
    streams.setdefault(t, []).append((off, sz))
# modules (4)
mods = []
off, sz = streams[4][0]
n, = struct.unpack_from("<I", m, off)
for i in range(n):
    base, size, cks, ts, nameoff = struct.unpack_from("<QIIII", m, off + 4 + 108 * i)
    ln, = struct.unpack_from("<I", m, nameoff)
    name = m[nameoff + 4: nameoff + 4 + ln].decode("utf-16-le")
    mods.append((base, size, name.split("\\")[-1]))
def owner(a):
    for b, s, nm in mods:
        if b <= a < b + s: return "%s+%X" % (nm, a - b)
    return None
# memory64 (9) for reading
mem = []
if 9 in streams:
    off, sz = streams[9][0]
    n, base = struct.unpack_from("<QQ", m, off)
    pos = base
    for i in range(n):
        a, s = struct.unpack_from("<QQ", m, off + 16 + 16 * i)
        mem.append((a, s, pos)); pos += s
def rd(a, n):
    for b, s, fo in mem:
        if b <= a and a + n <= b + s: return m[fo + (a - b): fo + (a - b) + n]
    # stitch across adjacent ranges (large reads of .text)
    out = bytearray(); cur = a
    while cur < a + n:
        hit = None
        for b, s, fo in mem:
            if b <= cur < b + s: hit = (b, s, fo); break
        if not hit: return None
        b, s, fo = hit; take = min(a + n - cur, b + s - cur)
        out += m[fo + (cur - b): fo + (cur - b) + take]; cur += take
    return bytes(out)
# exception (6)
off, sz = streams[6][0]
tid, _, code, flags, rec, addr = struct.unpack_from("<IIIIQQ", m, off)
nparams, = struct.unpack_from("<I", m, off + 32)
params = struct.unpack_from("<15Q", m, off + 40)
ctxsz, ctxoff = struct.unpack_from("<II", m, off + 32 + 8 + 120)
print("thread=%d code=%08X addr=%08X (%s) params=%s" % (tid, code, addr, owner(addr), ["%X" % x for x in params[:nparams]]))
# x86 CONTEXT: ContextFlags @0, Dr0-7, FloatSave(112), SegGs,Fs,Es,Ds @ 140, Edi@156 Esi Ebx Edx Ecx Eax, Ebp@180 Eip@184 SegCs Efl Esp@196 SegSs
ctx = m[ctxoff: ctxoff + ctxsz]
edi, esi, ebx, edx, ecx, eax, ebp, eip, cs, efl, esp = struct.unpack_from("<IIIIIIIIIII", ctx, 156)
print("eip=%08X (%s) esp=%08X ebp=%08X eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X" % (eip, owner(eip), esp, ebp, eax, ebx, ecx, edx, esi, edi))
code_bytes = rd(eip, 16)
print("code at eip:", code_bytes.hex() if code_bytes else None)
try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    if code_bytes:
        for ins in Cs(CS_ARCH_X86, CS_MODE_32).disasm(code_bytes, eip): print("   %08X %s %s" % (ins.address, ins.mnemonic, ins.op_str))
except Exception as e: print("capstone:", e)
print("-- stack (return addresses into modules), top 200 dwords")
st = rd(esp, 800)
if st:
    for i in range(0, len(st), 4):
        v, = struct.unpack_from("<I", st, i)
        o = owner(v)
        if o and (o.startswith("deadspace2") or ".asi" in o):
            # only plausible return addresses: preceded by a call (E8 / FF)
            back = rd(v - 5, 5)
            back2 = rd(v - 6, 6)
            iscall = (back and back[0] == 0xE8) or (back2 and back2[0] == 0xFF) or (back and back[-2] == 0xFF) or (back and back[-3] == 0xFF)
            print("  esp+%03X = %08X %s%s" % (i, v, o, "  <call>" if iscall else ""))
print("-- modules of interest")
for b, s, nm in mods:
    if nm.lower().endswith(".asi") or nm.lower().startswith("deadspace2"): print("  %08X %08X %s" % (b, s, nm))
