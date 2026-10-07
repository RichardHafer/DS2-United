"""Every INI key the plugin reads must sit in the section the code reads it from.

A key in the wrong section is silently ignored (25.09.: LocalNamePatterns sat in
[Gate], so the finale rule never ran). usage: python tools/check_ini_sections.py [ini ...]
"""
import glob
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATTERNS = [r'GetPrivateProfile(?:Int|String)A\(\s*"(\w+)"\s*,\s*"(\w+)"', r'Config\w*\(ini,\s*"(\w+)",\s*"(\w+)"']
code = set()
for f in glob.glob(str(ROOT / "src" / "*.*")):
    text = open(f, encoding="utf-8", errors="ignore").read()
    for p in PATTERNS:
        code.update(re.findall(p, text))
    for m in re.finditer(r'ReadRuleSet\([^;]*?"(\w+)",\s*"(\w+)",\s*"[^"]*",\s*"(\w+)"', text):
        code.add((m.group(1), m.group(2)))
        code.add((m.group(1), m.group(3)))
bad = 0
for ini in sys.argv[1:] or [str(ROOT / "DS2Coop.ini")]:
    section, seen = None, {}
    for line in open(ini, encoding="utf-8", errors="ignore"):
        line = line.strip()
        if line.startswith("["):
            section = line[1:-1]
        elif "=" in line and not line.startswith(";"):
            seen.setdefault(line.split("=")[0].strip(), []).append(section)
    for key, sections in sorted(seen.items()):
        wanted = sorted({s for s, k in code if k == key})
        if wanted and not set(sections) & set(wanted):
            bad += 1
            print(f"{ini}: {key} is in {sections}, the plugin reads it from {wanted}")
print("ok" if not bad else f"{bad} misplaced key(s)")
sys.exit(1 if bad else 0)
