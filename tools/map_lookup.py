"""Name DS2United.asi offsets from build/DS2United.map: python tools/map_lookup.py 0x3981 0x246CB ..."""
import bisect, re, sys
from pathlib import Path
lines = (Path(__file__).resolve().parent.parent / "build" / "DS2United.map").read_text(encoding="latin1").splitlines()
base = int(next(re.search(r"is ([0-9a-fA-F]+)", l).group(1) for l in lines if "Preferred load address" in l), 16)
syms = sorted((int(m.group(2), 16) - base, m.group(1)) for l in lines
              if (m := re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{8})\s", l)))
keys = [s[0] for s in syms]
for arg in sys.argv[1:]:
    off = int(arg, 16)
    i = bisect.bisect_right(keys, off) - 1
    print(f"{arg}: {syms[i][1][:110]} +{off - syms[i][0]:#x}")
