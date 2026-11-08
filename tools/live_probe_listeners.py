"""Every registered message: its listeners, classified by QueryInterface (vtable+0x10)."""
import struct, sys, json, collections
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
exec(open("live_probe_helpers.py").read().split("table = dw(")[0])
def hash_name(s):
    h = 0
    for ch in s.lower(): h = (h * 0x1003F + ord(ch)) & 0xFFFFFFFF
    return h
m = json.load(open("classmap.json"))
names = {hash_name(n): n for n in m}
cs = Cs(CS_ARCH_X86, CS_MODE_32)
qi_cache = {}
def answers(vt):
    """Hashes a class answers to, walking the base chain."""
    if vt in qi_cache: return qi_cache[vt]
    out, fn, seen = [], dw(vt + 0x10), set()
    while fn and fn not in seen and len(seen) < 12:
        seen.add(fn)
        code = read(fn, 0x200) or b""
        if not code.startswith(b"\x8b\x54\x24\x04"): break     # not QueryInterface-shaped
        nxt = None
        for ins in cs.disasm(code, fn):
            if ins.mnemonic == "cmp" and ins.op_str.startswith("edx, 0x"):
                out.append(int(ins.op_str.split(", ")[1], 16))
            if ins.mnemonic == "call":
                try: nxt = int(ins.op_str, 16)
                except ValueError: pass
            if ins.mnemonic == "int3": break
        fn = nxt
    qi_cache[vt] = out
    return out
table = dw(BASE + MESSAGE_TABLE_RVA)
stats = collections.Counter()
rows = []
for b in range(BUCKETS):
    entry = dw(table + b * 4)
    while entry:
        key = dw(entry + 0x10)
        lst = dw(entry + 4)
        listeners = []
        while lst and len(listeners) < 64:
            node = dw(lst + 8)
            while node and len(listeners) < 64:
                obj = dw(node + 0x0C)
                vt = dw(obj) if obj and 0x00100000 < obj < 0x7F000000 else None
                if vt and 0x01A00000 < vt < 0x01C80000:
                    a = answers(vt)
                    listeners.append(names.get(a[0], f"{a[0]:08X}") if a else f"vt{vt:08X}")
                else:
                    listeners.append("?")
                node = dw(node + 4)
            lst = dw(lst)
        for l in listeners: stats[l] += 1
        rows.append((key, listeners))
        entry = dw(entry)
print("messages:", len(rows), "listeners:", sum(stats.values()))
print("listener classes:", stats.most_common(40))
interesting = ("Mover", "MoverController", "StandardDoor", "SimpleOneTimeDoor", "ScriptedSequence", "LevelChange", "Checkpoint", "SimpleTeleport", "StreamScripter", "LoadStream", "Elevator")
for key, ls in rows:
    if any(l in interesting for l in ls):
        print(f"  {key:08X}: {collections.Counter(ls).most_common()}")
