"""Record static input API call sites in the installed MarkerPatch image."""
import argparse
import hashlib
import json
from pathlib import Path

import capstone
import pefile


def inspect(path):
    data = path.read_bytes()
    pe = pefile.PE(data=data)
    base = pe.OPTIONAL_HEADER.ImageBase
    names = {entry.address: entry.name.decode("ascii")
             for dll in pe.DIRECTORY_ENTRY_IMPORT for entry in dll.imports
             if entry.name and entry.name.decode("ascii") in (
                 "GetForegroundWindow", "GetAsyncKeyState",
                 "RegisterRawInputDevices", "GetRawInputData")}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.skipdata = True
    calls = []
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:
            continue
        for insn in md.disasm(section.get_data(), base + section.VirtualAddress):
            if insn.mnemonic not in ("call", "jmp"):
                continue
            for address, name in names.items():
                if insn.op_str == f"dword ptr [{hex(address)}]":
                    calls.append({"rva": insn.address-base, "api": name,
                                  "instruction": f"{insn.mnemonic} {insn.op_str}"})
    ranges = {}
    # These are bounded instruction windows, not whole function boundaries.
    for name, rva, size in (("foreground_comparison", 0x3077E, 0x12),
                            ("async_key_argument", 0x5E351, 0xA),
                            ("raw_registration", 0x9783F, 0x22),
                            ("raw_unregistration", 0x9797F, 0x23)):
        raw = pe.get_data(rva, size)
        instructions = list(md.disasm(raw, base+rva))
        if sum(i.size for i in instructions) != size or any(i.mnemonic == ".byte" for i in instructions):
            raise ValueError(f"Invalid instruction window: {name}")
        ranges[name] = {"rva": rva, "bytes": raw.hex(),
                        "instructions": [f"{i.address-base:08X} {i.mnemonic} {i.op_str}"
                                         for i in instructions]}
    return {"file": str(path), "sha256": hashlib.sha256(data).hexdigest(),
            "preferred_image_base": base, "api_calls": calls, "windows": ranges,
            "scope": "Static PE evidence only; addresses are module RVAs, not live hook targets."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = inspect(args.image)
    args.output.write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    print(f"Recorded {len(result['api_calls'])} API call sites and {len(result['windows'])} instruction windows.")
