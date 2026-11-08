"""Walk QueryInterface (vtable slot 4) chains: which class hashes does each class answer to?"""
import struct, sys, json
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
exec(open("live_probe_helpers.py").read().split("table = dw(")[0])
def hash_name(s):
    h = 0
    for ch in s.lower(): h = (h * 0x1003F + ord(ch)) & 0xFFFFFFFF
    return h
m = json.load(open("classmap.json"))
names = {hash_name(n): n for n in m}
for extra in ("Entity", "Base", "Component", "NetReflectable", "Door", "Trigger", "TriggerBase", "SimObject", "Actor", "Scripted"):
    names.setdefault(hash_name(extra), extra)
cs = Cs(CS_ARCH_X86, CS_MODE_32)
def chain(fn, depth=0, seen=None):
    seen = seen or set()
    if fn in seen or depth > 12: return
    seen.add(fn)
    code = read(fn, 0x200) or b""
    hashes, calls = [], []
    for ins in cs.disasm(code, fn):
        if ins.mnemonic == "cmp" and ins.op_str.startswith("edx, 0x"):
            hashes.append(int(ins.op_str.split(", ")[1], 16))
        if ins.mnemonic == "call":
            try: calls.append(int(ins.op_str, 16))
            except ValueError: pass
        if ins.mnemonic in ("ret", "int3") and ins.address > fn + 0x20 and ins.mnemonic == "int3": break
    print("  " * depth + f"{fn:08X}: " + ", ".join(names.get(h, f"{h:08X}") for h in hashes))
    for c in calls[-1:]:
        chain(c, depth + 1, seen)
for n in sys.argv[2:]:
    r = m.get(n)
    if not r: print(n, "?"); continue
    vt = r["vtables"][-1] if n == "Player" else r["vtables"][0]
    print(f"{n}: vtable {vt:08X}")
    chain(dw(vt + 0x10))
