"""Save reproducible schematic code evidence from the PC backup and a crash dump."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

import capstone
import pefile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'trainer'))
from analyze_minidump import Dump

SITES = {
    'registration': (0x1A82020, 45),
    'factory': (0x41D550, 38),
    'constructor': (0xDD8040, 113),
    'spawn_init': (0xDD8130, 24),
    'query_interface': (0xDB4980, 51),
    'init_from_parent': (0xDC1430, 114),
    'reward_guid_accessor': (0xDA9020, 7),
    'completion_gate_event': (0xDED450, 54),
}


def inspect(backup, dump_path):
    pe = pefile.PE(str(backup), fast_load=True)
    preferred = pe.OPTIONAL_HEADER.ImageBase
    dump = Dump(dump_path)
    try:
        bases = [m[0] for m in dump.modules() if Path(m[2]).name.lower() == 'deadspace2.exe']
        if len(bases) != 1 or bases[0] != preferred:
            raise ValueError('Expected one module at preferred base; relocation comparison not implemented')
        code = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        sites = []
        for name, (address, size) in SITES.items():
            expected = pe.get_data(address - preferred, size)
            actual = dump.read(address, size)
            sites.append(dict(name=name, preferred_va=hex(address), rva=hex(address-preferred),
                              backup_hex=expected.hex(), dump_equal=actual == expected,
                              dump_hex=actual.hex() if actual is not None else None,
                              assembly=[f'{i.address:08X} {i.mnemonic} {i.op_str}'
                                        for i in code.disasm(expected, address)]))
        table = pe.get_data(0x1B4F560-preferred, 24)
        return dict(backup=str(backup), backup_sha256=hashlib.sha256(backup.read_bytes()).hexdigest(),
                    dump=str(dump_path), module_base=hex(preferred),
                    scope='Historical PC code evidence only; runtime integration is not approved',
                    vtable=dict(address='0x1B4F560', first_six=[hex(v) for v in struct.unpack('<6I', table)],
                                dump_equal=dump.read(0x1B4F560, 24) == table), sites=sites)
    finally:
        dump.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backup', type=Path, required=True)
    parser.add_argument('--dump', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = inspect(args.backup, args.dump)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print({r['name']: r['dump_equal'] for r in report['sites']})
