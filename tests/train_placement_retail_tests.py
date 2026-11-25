"""Run retail sequence placement code with mocked movement-component methods.

Usage: python tests/train_placement_retail_tests.py <mapped retail image>
This checks native dispatch and matrix selection, not the reported failure.
"""
import struct
import sys
from pathlib import Path

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP

image = Path(sys.argv[1]).read_bytes()
assert image[0xACAB41:0xACAB47].hex() == '8b8000020000'
assert image[0xAA4C3C:0xAA4C43].hex() == '0f2880c0010000'


def run(move, position):
    u = Uc(UC_ARCH_X86, UC_MODE_32)
    u.mem_map(0x400000, (len(image) + 4095) & ~4095)
    u.mem_write(0x400000, image)
    u.mem_map(0x100000, 0x10000)
    sm, seq, entity, cmc, metadata = 0x102000, 0x103000, 0x104000, 0x105000, 0x106000
    stack, stop, owner, vtable = 0x10F000, 0x100000, 0x107000, 0x108000

    def put(a, v):
        u.mem_write(a, struct.pack('<I', v))

    def get(a):
        return struct.unpack('<I', u.mem_read(a, 4))[0]

    for a, v in [(sm + 0x54, seq + 0x10), (sm + 0x48, entity + 0x10),
                 (sm + 0x88, cmc), (sm + 0x50, owner), (owner, vtable),
                 (vtable + 0x28, stop + 0x10), (seq + 0xC, metadata),
                 (seq + 0x200, move), (stack, stop)]:
        put(a, v)
    matrix = struct.pack('<16f', -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, *position, 1)
    u.mem_write(seq + 0x190, matrix)
    # A distinct value exposes an incorrect diagnostic destination offset.
    u.mem_write(seq + 0x1F0, struct.pack('<3f', 900, 901, 902))
    calls = []

    def step(uc, address, size, data):
        cleanup = {0x876760: 0, 0xF72DC0: 4, 0xF638A0: 4,
                   0xF53730: 4, 0x7D0A80: 0, stop + 0x10: 0}
        if address not in cleanup:
            return
        sp = u.reg_read(UC_X86_REG_ESP)
        if address in (0xF72DC0, 0xF638A0):
            assert u.reg_read(UC_X86_REG_ECX) == cmc
            count = 64 if address == 0xF72DC0 else 12
            calls.append((address, bytes(u.mem_read(get(sp + 4), count))))
        elif address == 0x7D0A80:
            calls.append((address, None))
        u.reg_write(UC_X86_REG_EAX, 0)
        u.reg_write(UC_X86_REG_EIP, get(sp))
        u.reg_write(UC_X86_REG_ESP, sp + 4 + cleanup[address])

    u.hook_add(UC_HOOK_CODE, step)
    u.reg_write(UC_X86_REG_ESP, stack)
    u.reg_write(UC_X86_REG_ECX, sm)
    u.emu_start(0xECAB10, stop, count=2000)
    assert u.reg_read(UC_X86_REG_EIP) == stop
    assert u.reg_read(UC_X86_REG_ESP) == stack + 4
    expected = ([(0xF72DC0, matrix)] if move == 4 else
                [(0xF638A0, struct.pack('<3f', *position)), (0x7D0A80, None)])
    assert calls == expected, calls


for position in [(109.046997, 25.549999, -148.990997),
                 (108.310997, -31.381001, -222.996994)]:
    run(3, position)
    run(4, position)
print('4 retail placement cases passed: move 4 sets the matrix without SetTeleporting; move 3 marks the teleport.')
