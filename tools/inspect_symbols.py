"""Print Xbox PDB architecture using the existing registration-free DIA helper."""
import argparse
import hashlib
import json
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("pdb", type=Path)
parser.add_argument("--dia-helper", required=True, type=Path)
args = parser.parse_args()
sys.path.insert(0, str(args.dia_helper))
import dia

session = dia.open_pdb(str(args.pdb))
scope = session.globalScope
report = {"name": scope.name, "machine": hex(scope.machineType),
          "guid": str(scope.guid), "age": scope.age,
          "sha256": hashlib.sha256(args.pdb.read_bytes()).hexdigest(),
          "address_scope": "Xbox only; not PC hook addresses", "types": []}
for name in ("EARS::Modules::FuncSpawn", "EARS::Modules::FuncSpawn::Attributes",
             "EARS::Framework::SpawnInfo"):
    seen = set()
    for udt in dia.udts(session, name):
        if udt.length in seen:
            continue
        seen.add(udt.length)
        fields = []
        for raw in udt.findChildren(7, None, 0):
            member = dia.sym(raw)
            if member.dataKind != 7:
                continue
            fields.append({"name": member.name, "offset": hex(member.offset),
                           "size": member.type.length})
        report["types"].append({"name": name, "size": udt.length, "fields": fields})
print(json.dumps(report, indent=2))
