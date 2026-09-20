"""Compare a historical .text capture with relocated backup code. Read only."""
import argparse
import json
import struct
from pathlib import Path

import pefile

SITES = {
    "FuncSpawn.constructor": 0xEA5A40,
    "FuncSpawn.HandleEvents": 0xECB1D0,
    "FuncSpawn.SpawnInit": 0xEBA6F0,
    "FuncSpawn.QueryInterface": 0xEA5B60,
    "FuncSpawn.SpawnDone_candidate": 0xEBA610,
    "manager.lookup_candidate": 0x776440,
    "manager.create_candidate": 0x776480,
    "manager.finalize_candidate": 0x7752A0,
}


def compare(backup, capture, runtime_base, text_rva):
    pe = pefile.PE(str(backup))
    data = Path(capture).read_bytes()
    preferred = pe.OPTIONAL_HEADER.ImageBase
    delta = runtime_base - preferred
    relocations = [entry.rva for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])
                   for entry in block.entries if entry.type == 3]
    results = []
    for name, address in SITES.items():
        rva = address - preferred
        expected = bytearray(pe.get_data(rva, 64))
        for relocation in relocations:
            offset = relocation - rva
            if 0 <= offset <= len(expected) - 4:
                value = struct.unpack_from("<I", expected, offset)[0]
                struct.pack_into("<I", expected, offset, (value + delta) & 0xFFFFFFFF)
        offset = rva - text_rva
        actual = data[offset:offset + len(expected)] if offset >= 0 else b""
        row = {"name": name, "rva": hex(rva), "capture_va": hex(runtime_base + rva),
               "equal_64_bytes": actual == expected,
               "expected": expected.hex(), "actual": actual.hex()}
        if len(actual) >= 5 and actual[0] == 0xE9:
            row["entry_jump_target"] = hex(runtime_base + rva + 5 +
                                            struct.unpack_from("<i", actual, 1)[0])
            row["equal_after_first_6"] = actual[6:] == expected[6:]
        results.append(row)
    return {"historical_capture_only": True, "runtime_hook_approved": False,
            "runtime_base": hex(runtime_base), "text_rva": hex(text_rva),
            "capture_size": len(data), "sites": results}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backup", type=Path)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--runtime-base", type=lambda s: int(s, 0), required=True)
    parser.add_argument("--text-rva", type=lambda s: int(s, 0), default=0x1000)
    args = parser.parse_args()
    print(json.dumps(compare(args.backup, args.capture, args.runtime_base, args.text_rva), indent=2))
