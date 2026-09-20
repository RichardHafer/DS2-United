"""Generate PDB-derived class and field hash candidates for offline inspection."""
import argparse
import json
import sys
from pathlib import Path
from ds2_assets import sdbm

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("pdb")
parser.add_argument("--dia-helper", required=True)
parser.add_argument("--output", required=True, type=Path)
args = parser.parse_args()
sys.path.insert(0,args.dia_helper)
import dia
s=dia.open_pdb(args.pdb)
names={}
bases={}
def add(name):
    names.setdefault(f"{sdbm(name):08X}",set()).add(name)
for u in dia.udts(s):
    name=u.name
    if not name.startswith(("EARS::", "EA::")) or "<" in name:
        continue
    add(name.rsplit("::",1)[-1])
    if name.endswith("::Attributes"):
        add(name.rsplit("::",2)[-2])
        for raw in u.findChildren(7,None,0):
            field=dia.sym(raw)
            if field.dataKind==7:
                add(field.name)
    if name.startswith("EARS::Modules::") and name.count("::")==2:
        bases[name]=[dia.sym(x).type.name for x in u.findChildren(18,None,0)]
args.output.write_text(json.dumps({"names":{k:sorted(v) for k,v in names.items()},
                                  "bases":bases},indent=2),encoding="utf-8")
print(f"Wrote {len(names)} hashes and {len(bases)} class inheritance records")
