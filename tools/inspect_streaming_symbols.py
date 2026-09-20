"""Extract Xbox streaming architecture, never PC hook addresses."""
import argparse
import json
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("pdb")
parser.add_argument("--dia-helper", required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
sys.path.insert(0, args.dia_helper)
import dia
session = dia.open_pdb(args.pdb)
names = ["EARS::Framework::StreamManager", "EARS::Framework::Stream",
         "EARS::Framework::SimManager", "EARS::Framework::Modules::StreamSetManager",
         "EARS::Framework::Modules::StreamSetManager::Set::LoadEntry"]
report = {"address_scope": "Xbox architecture only", "types": []}
for name in names:
    for udt in dia.udts(session, name):
        entry = {"name": name, "size": udt.length, "members": [], "methods": []}
        for raw in udt.findChildren(7, None, 0):
            member = dia.sym(raw)
            if member.dataKind == 7:
                entry["members"].append({"name": member.name, "offset": member.offset,
                                         "type": member.type.name})
        for raw in udt.findChildren(5, None, 0):
            method = dia.sym(raw)
            parameters = []
            if method.name in ("Load", "Unload", "GetStatus", "StreamFromHandle"):
                for child in method.findChildren(7, None, 0):
                    parameter = dia.sym(child)
                    if parameter.dataKind == 3:
                        parameters.append({"name": parameter.name, "type": parameter.type.name,
                                           "size": parameter.type.length})
            entry["methods"].append({"name": method.name, "xbox_rva": hex(method.relativeVirtualAddress),
                                     "length": method.length, "parameters": parameters})
        report["types"].append(entry)
args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
for entry in report["types"]:
    print(entry["name"], "size", entry["size"])
    for method in entry["methods"]:
        if entry["name"].endswith("SimManager") and not any(s in method["name"] for s in ("Stream", "Resource", "Unload", "Remove")):
            continue
        print(method)
