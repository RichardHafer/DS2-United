"""Build the release DS2United.ini from a played INI and the repository template.

    python -I tools/merge_release_ini.py --played <game>\\plugins\\DS2United.ini --out <file>

The played INI gives the values (it holds the merged randomizer and survival sections the
template does not). Sections and keys only the template has are added with their comments,
and the release overrides below are applied. make_release_package.py then clears the
personal values and drops what is not in the release.
"""
import argparse
import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# (section, key) -> value; lower case section and key
OVERRIDES = {
    # diagnostics nobody needs in a release
    ("coop", "lasercleanup"): "1",
    ("bot", "altfiretrace"): "0",
    ("ds2randomizer", "soundprobe"): "0",
    ("ds2randomizer", "globalstreamprobe"): "0",
    ("ds2randomizer", "streamregistrydiagnostics"): "0",
    ("ds2randomizer", "streamwatchdiagnostics"): "0",
    # off by default in the release
    ("necro", "enabled"): "0",
    # the release look and the normal dementia pace (the played INI ran it fast for tests)
    ("menu", "frontendcolours"): "red",
    ("menu", "redingamemenus"): "1",
    ("dementia", "riseseconds"): "150",
    ("dementia", "pace"): "100",
    ("shop", "key"): "86",
}
# trace and probe switches anywhere: off
TRACE_KEYS = re.compile(r"^(?:\w*Trace|\w*Probe)$", re.I)
TRACE_KEEP = {("difficulty", "probe")}
# keys the template sets that a played test INI changed only for experiments
FROM_TEMPLATE = {("necro", "sixthtypeguid"), ("necro", "leaperrecordstream")}


def parse(text):
    """Sections in order: name -> list of lines (header excluded); '' is the part before any section."""
    order, body, current = [""], {"": []}, ""
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            current = s[1:-1]
            if current.lower() not in {o.lower() for o in order}:
                order.append(current)
                body[current] = []
            else:
                current = next(o for o in order if o.lower() == current.lower())
            continue
        body[current].append(line)
    return order, body


def key_of(line):
    s = line.strip()
    if not s or s.startswith(";") or "=" not in s:
        return None
    return s.split("=", 1)[0].strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--played", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    played = Path(args.played).read_bytes().decode("utf-8").replace("\r\n", "\n")
    template = (REPO / "DS2United.ini").read_bytes().decode("utf-8").replace("\r\n", "\n")
    porder, pbody = parse(played)
    torder, tbody = parse(template)
    tvalues = {(sec.lower(), key_of(l).lower()): l.split("=", 1)[1] for sec in torder for l in tbody[sec] if key_of(l)}
    added_keys = added_sections = 0
    lower_p = {s.lower(): s for s in porder}
    for sec in torder:
        if not sec:
            continue
        if sec.lower() not in lower_p:
            porder.append(sec)
            pbody[sec] = list(tbody[sec])
            lower_p[sec.lower()] = sec
            added_sections += 1
            continue
        target = pbody[lower_p[sec.lower()]]
        have = {key_of(l).lower() for l in target if key_of(l)}
        pending = []   # comment lines right above a template key
        for line in tbody[sec]:
            k = key_of(line)
            if k is None:
                pending.append(line) if line.strip().startswith(";") else pending.clear()
                continue
            if k.lower() not in have:
                while target and not target[-1].strip():
                    target.pop()
                target.extend(pending + [line])
                target.append("")
                added_keys += 1
            pending = []
    changed = []
    for sec in porder:
        for i, line in enumerate(pbody[sec]):
            k = key_of(line)
            if not k:
                continue
            ident = (sec.lower(), k.lower())
            want = OVERRIDES.get(ident)
            if want is None and ident in FROM_TEMPLATE and ident in tvalues:
                want = tvalues[ident].strip()
            if want is None and TRACE_KEYS.match(k) and ident not in TRACE_KEEP and line.split("=", 1)[1].strip() not in ("0", ""):
                want = "0"
            if want is not None and line.split("=", 1)[1].strip() != want:
                indent = line[: len(line) - len(line.lstrip())]
                pbody[sec][i] = "%s%s=%s" % (indent, k, want)
                changed.append("[%s] %s=%s" % (sec, k, want))
    out = []
    for sec in porder:
        if sec:
            out.append("[%s]" % sec)
        out.extend(pbody[sec])
    Path(args.out).write_bytes("\r\n".join(out).encode("utf-8"))
    print("added %d sections and %d keys from the template; %d values set:" % (added_sections, added_keys, len(changed)))
    for c in changed:
        print("  " + c)


if __name__ == "__main__":
    main()
