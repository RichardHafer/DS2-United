"""Show what actually differs between two vent spawn contracts.

The catalog hashes the whole DSNPCSpawningComponent into `crossArrivalKey`, so
two vent spawns that differ in a single authored message id count as
incompatible. This prints the field-level difference for pairs that share their
orientation and activation flags, which is what decides whether the strictness is
buying anything.
"""
import argparse
import collections
import csv
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROW = re.compile(
    r'\{\{(0x[0-9A-Fa-f]+u),(0x[0-9A-Fa-f]+u),(0x[0-9A-Fa-f]+u),(0x[0-9A-Fa-f]+u)\},'
    r'\{[^}]*\},"([^"]*)","([^"]*)",(\w+),(\w+),(\w+),"([^"]*)","([^"]*)","([^"]*)"')


def guid(m, *idx):
    return "-".join("%08X" % int(m.group(i)[2:-1], 16) for i in idx)


def load():
    catalog = {}
    for m in ROW.finditer((ROOT / "src/generated_enemy_catalog.h").read_text(encoding="utf-8")):
        catalog[guid(m, 1, 2, 3, 4)] = dict(family=m.group(5), context=m.group(6),
                                            arrival=m.group(10), cross=m.group(11))
    with (ROOT / "analysis/catalog/enemy_targets.tsv").open(newline="", encoding="utf-8") as f:
        targets = {r["guid"]: r for r in csv.DictReader(f, delimiter="\t")}
    return catalog, targets


def spawning_fields():
    """(archive, asset, index) -> the decoded DSNPCSpawningComponent fields."""
    out = {}
    with (ROOT / "analysis/catalog/entities.jsonl").open(encoding="utf-8") as f:
        for line in f:
            entity = json.loads(line)
            for image in entity.get("images", []):
                if image.get("class") != "DSNPCSpawningComponent":
                    continue
                fields = {}
                for field in image.get("fields", []):
                    flags = field["flags"]
                    if flags in (1, 16, 17) and "string_candidate" in field:
                        value = field["string_candidate"]
                    elif flags == 12 and "guid_candidate" in field:
                        value = field["guid_candidate"]
                    elif flags in (0, 7):
                        value = field["value"]
                    else:
                        value = "<%d>" % flags
                    fields[(field["field_id"], flags)] = value
                out[(entity["archive"], entity["asset"], entity["index"])] = fields
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pairs", type=int, default=6)
    args = parser.parse_args()
    catalog, targets = load()
    fields = spawning_fields()

    def key_of(g):
        r = targets.get(g)
        return None if not r else (r["archive"], r["asset"], int(r["entity_index"]))

    vent = [g for g, c in catalog.items() if c["context"].startswith("SPAWNMODE_VENT") and c["cross"]]
    groups = collections.defaultdict(list)
    for g in vent:
        r = targets.get(g, {})
        groups[(catalog[g]["context"], r.get("activation_flags", ""))].append(g)

    differing = collections.Counter()
    shown = 0
    for (context, flags), members in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        species = collections.defaultdict(list)
        for g in members:
            species[catalog[g]["family"]].append(g)
        if len(species) < 2:
            continue
        families = sorted(species)
        for a in species[families[0]]:
            for b in species[families[1]]:
                fa, fb = fields.get(key_of(a)), fields.get(key_of(b))
                if not fa or not fb:
                    continue
                diff = {k for k in set(fa) | set(fb) if fa.get(k) != fb.get(k)}
                for k in diff:
                    differing[k] += 1
                if shown < args.pairs and catalog[a]["cross"] != catalog[b]["cross"]:
                    print("%s  %s vs %s" % (context, catalog[a]["family"], catalog[b]["family"]))
                    print("   same fields: %d, differing: %d" % (len(set(fa) & set(fb)) - len(diff), len(diff)))
                    for k in sorted(diff):
                        print("      field %-6s flags=%-3s %r -> %r" %
                              (k[0], k[1], fa.get(k), fb.get(k)))
                    shown += 1
                break
            if shown >= args.pairs:
                break

    print("\nfields that differ across species inside one orientation/flags group:")
    for (field_id, flags), n in differing.most_common(12):
        print("   field %-8s flags=%-3s in %d comparisons" % (field_id, flags, n))


if __name__ == "__main__":
    main()
