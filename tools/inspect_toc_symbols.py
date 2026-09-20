"""Read Xbox TOC type names as architectural evidence, not PC addresses."""
import argparse
import json
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--pdb", required=True)
parser.add_argument("--dia-helper", required=True)
parser.add_argument("--type", action="append", dest="types", required=True)
args = parser.parse_args()
sys.path.insert(0, args.dia_helper)
import dia

session = dia.open_pdb(args.pdb)
result = []
for name in args.types:
    for udt in dia.udts(session, name):
        fields = []
        for raw in udt.findChildren(7, None, 0):
            member = dia.sym(raw)
            if member.dataKind == 7:
                fields.append({"name": member.name, "offset": member.offset,
                               "type": member.type.name, "size": member.type.length})
        result.append({"name": name, "size": udt.length, "fields": fields})
report = {"scope": "Xbox architecture only", "types": result}
output = Path(__file__).resolve().parents[1] / "analysis/toc_symbols.json"
output.write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2))
