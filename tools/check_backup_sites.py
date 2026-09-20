"""Compare backup-only PC discovery signatures. Never approves runtime hooks."""
import argparse
import hashlib
import json
from pathlib import Path
import pefile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("executable", type=Path)
args = parser.parse_args()
data = args.executable.read_bytes()
pe = pefile.PE(data=data, fast_load=True)
sites = (
    ("FuncSpawn constructor", 0xEA5A40,
     "55 8b ec 83 e4 f0 51 8b 45 08 53 56 57 50 8b f1 e8 eb 97 92 ff 33 db 53 53 c7 06 58 9c b5 01 c7"),
    ("FuncSpawn HandleEvents", 0xECB1D0,
     "8b 44 24 04 8b 00 83 ec 44 53 55 56 57 8b f9 8b 6f 0c 3b 05 f8 4f 05 02 75 0f e8 b1 a9 fd ff 5f"),
    ("FuncSpawn SpawnInit", 0xEBA6F0,
     "8b 44 24 04 56 8b f1 8b 48 04 51 8b ce e8 4e 9e fd ff 8b 46 0c 8b 50 2c 83 c9 ff 85 d2 0f 45 ca"),
    ("FuncSpawn QueryInterface", 0xEA5B60,
     "81 7c 24 04 17 06 59 4b 75 0b 8b 44 24 08 89 08 b0 01 c2 08 00 8b 4c 24 08 c7 01 00 00 00 00 32"),
)
report = {"sha256": hashlib.sha256(data).hexdigest(),
          "runtime_hook_approved": False, "sites": []}
for name, address, signature in sites:
    expected = bytes.fromhex(signature)
    actual = pe.get_data(address - 0x400000, len(expected))
    report["sites"].append({"name": name, "backup_va": hex(address),
                            "matches_backup": actual == expected,
                            "whole_file_occurrences": data.count(expected),
                            "actual_bytes": actual.hex(" ")})
print(json.dumps(report, indent=2))
