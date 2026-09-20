"""Read-only PE evidence inventory. Printed references are not hook signatures."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
import pefile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("executable", type=Path)
args = parser.parse_args()
data = args.executable.read_bytes()
pe = pefile.PE(data=data, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
report = {
    "path": str(args.executable), "sha256": hashlib.sha256(data).hexdigest(),
    "size": len(data), "machine": hex(pe.FILE_HEADER.Machine),
    "image_base": hex(base), "timestamp": pe.FILE_HEADER.TimeDateStamp,
    "sections": [{"name": s.Name.rstrip(b"\0").decode(),
                  "rva": hex(s.VirtualAddress), "size": s.Misc_VirtualSize}
                 for s in pe.sections], "strings": [],
    "verified_hooks": [],
}
for name in (b"FuncSpawn", b"FuncSpawnOptions", b"FuncSpawnRetryMessage"):
    needle = b"\0" + name + b"\0"
    offset = data.find(needle)
    if offset < 0:
        continue
    address = base + pe.get_rva_from_offset(offset + 1)
    packed = struct.pack("<I", address)
    references = []
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:
            continue
        code = section.get_data()
        start = 0
        while (index := code.find(packed, start)) >= 0:
            references.append(hex(base + section.VirtualAddress + index))
            start = index + 1
    report["strings"].append({"name": name.decode(), "va": hex(address),
                               "raw_absolute_references": references})
print(json.dumps(report, indent=2))
