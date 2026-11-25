"""Exercise the retail door occupancy loop with mocked class queries (Unicorn)."""
import struct
import sys
from pathlib import Path

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import *

image = Path(sys.argv[1]).read_bytes()
assert image[0x9D20C2:0x9D20CF].hex() == '8b8f3c020000d1e9f6c1017433'
assert image[0x9D214B:0x9D215A].hex() == '83c5103b6c24180f8540ffffffeb3e'


def run(kinds, filtered, flags=3):
    u = Uc(UC_ARCH_X86, UC_MODE_32)
    u.mem_map(0xDD2000, 0x1000)
    u.mem_write(0xDD2000, image[0x9D2000:0x9D3000])
    u.mem_map(0x100000, 0x10000)
    stack, door, entries, query = 0x101000, 0x102000, 0x103000, 0x104000
    def put(a, v):
        u.mem_write(a, struct.pack('<I', v))
    def get(a):
        return struct.unpack('<I', u.mem_read(a, 4))[0]
    put(door + 0x23C, flags)
    put(stack + 0x18, entries + len(kinds) * 16)
    put(query + 0x10, query + 0x100)
    for n in range(len(kinds)):
        put(0x105000 + n * 0x100, query)
    for reg, value in [(UC_X86_REG_ESP, stack), (UC_X86_REG_EDI, door),
                       (UC_X86_REG_EBP, entries), (UC_X86_REG_EBX, 0)]:
        u.reg_write(reg, value)
    def step(uc, address, size, data):
        if address == 0xDD2098:
            n = (u.reg_read(UC_X86_REG_EBP) - entries) // 16
            u.reg_write(UC_X86_REG_ESI, 0x105000 + n * 0x100)
            u.reg_write(UC_X86_REG_EIP, 0xDD20C2)
        elif address == 0xDD20C2:
            n = (u.reg_read(UC_X86_REG_ESI) - 0x105000) // 0x100
            if filtered and kinds[n] == 'bot':
                u.reg_write(UC_X86_REG_EIP, 0xDD214B)
        elif address == query + 0x100:
            sp = u.reg_read(UC_X86_REG_ESP)
            n = (u.reg_read(UC_X86_REG_ECX) - 0x105000) // 0x100
            wanted = 'npc' if get(sp + 4) == 0x71BC20FF else 'player'
            match = kinds[n] == wanted or (kinds[n] == 'bot' and wanted == 'player')
            put(get(sp + 8), 1 if match else 0)
            u.reg_write(UC_X86_REG_EAX, int(match))
            u.reg_write(UC_X86_REG_EIP, get(sp))
            u.reg_write(UC_X86_REG_ESP, sp + 12)
    u.hook_add(UC_HOOK_CODE, step)
    u.emu_start(0xDD2098, 0xDD2198, count=2000)
    assert u.reg_read(UC_X86_REG_EIP) == 0xDD2198
    assert u.reg_read(UC_X86_REG_ESP) == stack
    return bool(u.mem_read(stack + 0x13, 1)[0])


cases = [('bot',), ('bot', 'bot'), ('player',), ('npc',), ('prop',),
         ('player', 'bot'), ('bot', 'player'), ('npc', 'bot'), ('bot', 'npc')]
for kinds in cases:
    for flags in (1, 2, 3):
        expected = any((k == 'player' and flags & 1) or (k == 'npc' and flags & 2) for k in kinds)
        assert run(kinds, True, flags) == bool(expected), (kinds, flags)
assert run(('bot',), False)
print('27 retail occupancy cases passed; unfiltered bot reproduces occupancy.')
