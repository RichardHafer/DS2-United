"""Puts the owner line at the top of every source file in src/ that lacks it.

Run by build_ds2coop.cmd before the compile. A generated header loses the line
whenever its tool rewrites it; the next build puts it back.
"""
from pathlib import Path

LINE = b"// Dead Space 2 United by Richard Hafer."
SRC = Path(__file__).resolve().parent.parent / "src"
BOM = b"\xef\xbb\xbf"

added, skipped = [], []
for path in sorted(list(SRC.rglob("*")) + list((SRC.parent / "tests").glob("*"))):
    if not path.is_file() or path.suffix.lower() not in (".h", ".inc", ".cpp", ".c"):
        continue
    data = path.read_bytes()
    if b"Richard Hafer" in data:
        continue
    if b"\0" in data:
        skipped.append(path.name)
        continue
    bom = BOM if data.startswith(BOM) else b""
    body = data[len(bom):]
    newline = b"\r\n" if b"\r\n" in body[:4096] else b"\n"
    path.write_bytes(bom + LINE + newline + body)
    added.append(path.name)

print("owner line added to %d source file(s)%s" % (len(added), ": " + ", ".join(added[:8]) + (" ..." if len(added) > 8 else "") if added else ""))
if skipped:
    print("NOT stamped (binary or UTF-16): " + ", ".join(skipped))
