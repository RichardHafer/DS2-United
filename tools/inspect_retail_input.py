"""Record retail DirectInput enumeration and keyboard creation from a dump."""
import argparse
import hashlib
import json
from pathlib import Path
import capstone
from inspect_inventory_persistence import Dump

RANGES = {
    "direct_input_import_thunk": (0x148DF6C, 6),
    "direct_input_create_and_enumerate": (0x140FA2A, 0x36),
    "device_enumeration_callback": (0x140F7B0, 0x6C),
    "keyboard_creation": (0x140F310, 0x29E),
}

def inspect(dump):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    result = {"dump": str(dump.path), "import_slot": 0x2158A34,
              "import_target": dump.u32(0x2158A34),
              "packed_import_target": dump.u32(0x375028C),
              "device_type_jump_table": [dump.u32(0x140F81C+i*4) for i in range(7)],
              "functions": {}}
    for name, (va, size) in RANGES.items():
        raw = dump.read(va, size)
        instructions = list(md.disasm(raw or b"", va))
        if raw is None or sum(i.size for i in instructions) != size:
            raise ValueError(f"Incomplete code range: {name}")
        result["functions"][name] = {"va": va, "bytes": raw.hex(),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "disassembly": [f"{i.address:08X} {i.mnemonic} {i.op_str}" for i in instructions]}
    return result

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    dump = Dump(args.dump)
    try:
        args.output.write_text(json.dumps(inspect(dump), indent=2)+"\n", encoding="utf-8")
    finally:
        dump.close()
