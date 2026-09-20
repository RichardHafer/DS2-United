"""Drop character packages the randomizer can never select from the pool TOC.

Every name in the pool TOC is a stream the loader references for as long as our
root is held, so each one stays resident for the whole game.  That residency is
not free: the game evicts what it can when it needs room, and the only entries
it may evict are the ones nobody references, which includes the actors of a
scripted scene that has not played yet.  Chapter 4 lost the Nicole scene that
way, and with it the message that unlocks the transport hub elevator.

So the pool should force only what it can actually use.  A family is selectable
when at least one catalog row is `ordinary`, or when it is `extended` and the
matching ExperimentalClasses switch turns it on.  A family with neither can
never be picked no matter how the INI is written, so its packages are dead
weight in every session.  Those are dropped here.

Families that are only selectable through an ExperimentalClasses switch are
kept by default: the switch lives in the INI and can be turned on after the
pool was built.  Pass --drop-experimental to trim those too when building a
package for a configuration that leaves them off.

A package trimmed for one configuration can be wanted by the next: Stalker
went off on 12.09. and on again on 15.09., and its three packages stayed
dropped, so every Stalker record in the pool was refused at start while the
INI said 1. Pass --restore with package names (stalker, lurker ...) to put
dropped packages back into the forced list; their identities come from the
stock TOCs like every other forced stream.

The TOC is rebuilt with build_research_toc.build, which round-trips its own
output, and the manifest is updated so embed_stream_requirements.py stays in
step.  Run that afterwards. The plugin verifies the file it loads against the
size and fingerprint recorded there and refuses a pool it does not recognise.
"""
import argparse
import hashlib
import json
import re
from collections import defaultdict
from pathlib import Path

from build_research_toc import build, pool_root_name
from append_boss_donors import stock_identity

ROOT = Path(__file__).resolve().parents[1]
STREAM = ROOT / "analysis/runtime_stream"
EXPERIMENTAL = ("Infector", "Stalker", "SwarmEnemy", "DividerCore", "DividerPart", "Hunter")


def catalog_families():
    """Per family: how many rows are ordinary, how many are extended."""
    text = (ROOT / "src/generated_enemy_catalog.h").read_text(encoding="latin1")
    rows = re.findall(r'\},"([A-Za-z_]+)","[^"]*",(true|false),(true|false),(true|false),', text)
    families = defaultdict(lambda: {"rows": 0, "ordinary": 0, "extended": 0})
    for family, ordinary, _ambiguous, extended in rows:
        families[family]["rows"] += 1
        families[family]["ordinary"] += ordinary == "true"
        families[family]["extended"] += extended == "true"
    return families


def package_families():
    """Per TOC stream name: the family whose models it carries."""
    manifest = json.loads((ROOT / "analysis/native_character_streams.json").read_text(encoding="utf-8"))
    out = {}
    for package in manifest["packages"]:
        classes = package.get("classes") or []
        name = package["toc_name"]
        # A package can carry more than one family (the divider package holds
        # the core and its parts); the first one decides for the trim.
        out[name] = classes[0] if classes else ""
    return out


def disabled_families(path):
    """Families a configuration turns off: [Targets]=0, or an experimental class
    left off. Those can never be selected while that INI is in use, so their
    packages are dead weight for a build shipped with it."""
    section, off = "", set()
    for raw in path.read_text(encoding="latin1").splitlines():
        line = raw.split(";", 1)[0].strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            continue
        if "=" not in line or section not in ("Targets", "ExperimentalClasses"):
            continue
        key, _, value = line.partition("=")
        if value.strip() == "0":
            off.add(key.strip())
    return off


def restore(manifest, manifest_path, names, apply):
    dropped = list(manifest.get("trimmed", {}).get("dropped", []))
    wanted, missing = [], []
    for short in names:
        match = [name for name in dropped if name.rsplit("\\", 1)[-1] == short]
        (wanted if match else missing).append(match[0] if match else short)
    if missing:
        raise SystemExit("not among the dropped packages: " + ", ".join(missing))
    forced = [tuple(entry) for entry in manifest["forced"]]
    for name in wanted:
        if all(existing != name for _identity, existing in forced):
            forced.append((stock_identity(name), name))
        print("%-30s restored" % name.rsplit("\\", 1)[-1][:30])
    print("\nForced afterwards: %d streams." % len(forced))
    if not apply:
        print("Dry run. Pass --apply to write.")
        return
    data = build(pool_root_name(manifest), manifest["content_size"],
                 [tuple(entry) for entry in manifest["shared"]], forced)
    (STREAM / "ds2_randomizer_pool.toc").write_bytes(data)
    manifest["forced"] = [list(entry) for entry in forced]
    manifest["toc_bytes"] = len(data)
    manifest["toc_sha256"] = hashlib.sha256(data).hexdigest()
    manifest["trimmed"]["dropped"] = [name for name in dropped if name not in wanted]
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print("TOC rebuilt: %d bytes, %d forced streams." % (len(data), len(forced)))
    print("Now run tools/embed_stream_requirements.py and rebuild.")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--drop-experimental", action="store_true",
                        help="also drop families that only an ExperimentalClasses switch enables")
    parser.add_argument("--ini", type=Path,
                        help="drop families this INI turns off in [Targets] or [ExperimentalClasses]; "
                             "the switches stop working for them, so use it for a shipped build")
    parser.add_argument("--restore", nargs="+", metavar="PACKAGE", default=[],
                        help="put these dropped packages back (short names such as stalker); "
                             "everything else is left alone")
    parser.add_argument("--apply", action="store_true",
                        help="write the trimmed TOC and manifest; without it nothing is changed")
    args = parser.parse_args()

    manifest_path = STREAM / "ds2_randomizer_pool.toc.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    families, owners = catalog_families(), package_families()

    off = disabled_families(args.ini) if args.ini else set()

    if args.restore:
        restore(manifest, manifest_path, args.restore, args.apply)
        return

    def selectable(family):
        counts = families.get(family)
        if not counts:
            return True          # unknown family: never guess it away
        if family in off:
            return False
        if counts["ordinary"]:
            return True
        if counts["extended"] and family in EXPERIMENTAL:
            return not args.drop_experimental
        return False

    keep, drop = [], []
    for identity, name in manifest["forced"]:
        family = owners.get(name, "")
        (keep if not family or selectable(family) else drop).append((identity, name, family))

    print("%-30s %-12s %s" % ("Package", "Family", "Decision"))
    for _identity, name, family in sorted(drop, key=lambda row: row[1]):
        print("%-30s %-12s dropped, never selectable" % (name.rsplit("\\", 1)[-1][:30], family))
    print("\nKept: %d  dropped: %d  of %d" % (len(keep), len(drop), len(manifest["forced"])))
    if not drop:
        print("Nothing to do.")
        return
    if not args.apply:
        print("Dry run. Pass --apply to write.")
        return

    forced = [[identity, name] for identity, name, _family in keep]
    data = build(pool_root_name(manifest), manifest["content_size"],
                 [tuple(entry) for entry in manifest["shared"]],
                 [(identity, name) for identity, name in forced])
    (STREAM / "ds2_randomizer_pool.toc").write_bytes(data)
    manifest["forced"] = forced
    manifest["toc_bytes"] = len(data)
    manifest["toc_sha256"] = hashlib.sha256(data).hexdigest()
    # A second pass trims further; keep the whole record so the manifest still
    # says which packages the pool once carried.
    previous = manifest.get("trimmed", {}).get("dropped", [])
    manifest["trimmed"] = {"dropped": previous + [name for _identity, name, _family in drop],
                           "reason": "family cannot be selected by the configuration this pool was built for"}
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print("\nTOC rebuilt: %d bytes, %d forced streams." % (len(data), len(forced)))
    print("Now run tools/embed_stream_requirements.py and rebuild.")


if __name__ == "__main__":
    main()
