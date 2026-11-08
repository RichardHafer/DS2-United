"""Every class descriptor -> its primary vtable, via factory thunk -> factory -> ctor."""
import struct, sys, json
exec(open("live_probe_helpers.py").read().split("table = dw(")[0])
def hash_name(s):
    h = 0
    for ch in s.lower(): h = (h * 0x1003F + ord(ch)) & 0xFFFFFFFF
    return h
CODE_LO, CODE_HI = 0x00401000, 0x01A00000
RDATA_LO, RDATA_HI = 0x01A00000, 0x01C80000
descs = []
for start in range(0x01C80000, 0x02400000, 0x100000):
    raw = read(start, 0x100000)
    if not raw: continue
    for off in range(0, len(raw) - 12, 4):
        nameptr, h = struct.unpack_from("<II", raw, off + 4)
        if not (RDATA_LO < nameptr < RDATA_HI): continue
        n = text(nameptr, 64)
        if n and hash_name(n) == h and n[0].isalpha():
            descs.append((start + off, n))
print("descriptors:", len(descs))
def follow_jmp(a):
    for _ in range(4):
        c = read(a, 5)
        if c and c[0] == 0xE9: a = a + 5 + struct.unpack_from("<i", c, 1)[0]
        else: return a
    return a
def calls(fn, span=0x40):
    code = read(fn, span) or b""
    for i in range(len(code) - 5):
        if code[i] == 0xE8:
            t = fn + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
            if CODE_LO < t < CODE_HI: yield t
        if code[i] == 0xC3 or code[i] == 0xC2: break
def vtable_stores(ctor, span=0x100):
    code = read(ctor, span) or b""
    out = []
    for i in range(len(code) - 6):
        if code[i] == 0xC7 and code[i+1] in (0x00, 0x01, 0x03, 0x06, 0x07):
            imm = struct.unpack_from("<I", code, i + 2)[0]
            if RDATA_LO < imm < RDATA_HI: out.append(imm)
        if code[i] == 0xC3 and i > 0x10: break
    return out
result = {}
for d, n in descs:
    thunk = dw(d + 0x18)
    if not thunk or not (CODE_LO < thunk < CODE_HI): continue
    factory = follow_jmp(thunk)
    ctors = list(calls(factory))
    for ct in ctors:
        vts = vtable_stores(ct)
        if vts:
            result[n] = {"descriptor": d, "factory": factory, "ctor": ct, "vtables": vts, "size": dw(d + 0x40) & 0xFFFF}
            break
print("with vtable:", len(result))
json.dump(result, open("classmap.json", "w"), indent=1)
for want in (0x01B294D0, 0x01B5B628):
    hits = [n for n, r in result.items() if want in r["vtables"]]
    print(f"vtable {want:08X}: {hits}")
for n in ("Player", "TriggerBox", "Mover", "StandardDoor", "ScriptedSequence"):
    r = result.get(n); print(n, r and {k: (hex(v) if isinstance(v, int) else [hex(x) for x in v]) for k, v in r.items()})
