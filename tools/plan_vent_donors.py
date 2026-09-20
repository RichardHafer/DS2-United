"""Pick one vent donor per (contract, package), the one that depends least on
the level it was authored in.

An entity carries GUID references to other entities: its loot table, its ranged
projectile, its nav routing settings, a grapple camera. Copy it into the pool and
those references stay pointed at the level it came from, which is not loaded
anywhere else. A player reported a spitter that could not spit, and the reference
counts name the reason exactly: `CMD_SetRangedAttackProjectile` on 54 Puker and
14 Zombie vent instances points at a level-local projectile.

Nothing can remove those references, but a lot can be avoided by choosing well:
the same species and contract is authored in many places, and the instances
differ in how much of their level they drag along. The reference to the vent
itself is ignored here, because the runtime replaces the whole spawning
component with the level's own.
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
    r'\{[^}]*\},"([^"]*)","([^"]*)",(\w+),(\w+),(\w+),"([^"]*)","([^"]*)","([^"]*)","([^"]*)"')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=ROOT / "analysis/vent_pool_plan.json")
    args = parser.parse_args()

    catalog = {}
    for m in ROW.finditer((ROOT / "src/generated_enemy_catalog.h").read_text(encoding="utf-8")):
        guid = "-".join("%08X" % int(m.group(i)[2:-1], 16) for i in (1, 2, 3, 4))
        catalog[guid] = {"family": m.group(5), "context": m.group(6), "vent_free": m.group(13)}

    with (ROOT / "analysis/catalog/enemy_targets.tsv").open(newline="", encoding="utf-8") as f:
        rows = [r for r in csv.DictReader(f, delimiter="\t")
                if r["multiplayer"] == "False" and r["mode"].startswith("SPAWNMODE_VENT")]

    where = collections.defaultdict(list)
    references = {}
    with (ROOT / "analysis/catalog/entities.jsonl").open(encoding="utf-8") as f:
        for line in f:
            entity = json.loads(line)
            where[entity["guid"]].append(entity["archive"])
            found = []
            for image in entity.get("images", []):
                for field in image.get("fields", []):
                    guid = field.get("guid_candidate")
                    if guid and guid != "00000000-00000000-00000000-00000000":
                        found.append((image.get("class", ""), field.get("name", ""), guid))
            references[(entity["archive"], entity["asset"], entity["index"])] = found

    def level_bound(row):
        """References the level owns and the pool cannot bring along."""
        key = row["archive"], row["asset"], int(row["entity_index"])
        out = []
        for owner, name, guid in references.get(key, []):
            if guid not in where:
                continue  # a resource, not an entity
            if any("global_assets" in archive for archive in where[guid]):
                continue  # loaded everywhere
            if owner == "DSNPCSpawningComponent":
                continue  # replaced wholesale by the level's own component
            out.append("%s.%s" % (owner, name))
        return out

    groups = collections.defaultdict(list)
    spawns = collections.Counter()
    for row in rows:
        entry = catalog.get(row["guid"])
        if not entry or not entry["vent_free"]:
            continue
        spawns[entry["vent_free"]] += 1
        groups[(entry["vent_free"], row["package_candidates"].split("|")[0])].append(row)

    plan = []
    kept = collections.Counter()
    for (contract, package), members in groups.items():
        members.sort(key=lambda r: (len(level_bound(r)), r["archive"], r["guid"]))
        best = members[0]
        missing = level_bound(best)
        kept[len(missing)] += 1
        plan.append({"contract": contract, "package": package,
                     "family": catalog[best["guid"]]["family"],
                     "guid": best["guid"], "archive": best["archive"], "asset": best["asset"],
                     "entity_index": best["entity_index"], "script_name": best["script_name"],
                     "spawns_of_contract": spawns[contract],
                     "level_bound_references": missing,
                     "candidates": len(members)})
    plan.sort(key=lambda p: (-p["spawns_of_contract"], p["package"]))
    args.out.write_text(json.dumps(plan, indent=1))
    print("donors planned: %d" % len(plan))
    print("level-bound references left, by count: %s" % dict(sorted(kept.items())))
    worst = collections.Counter()
    for entry in plan:
        for name in entry["level_bound_references"]:
            worst[name] += 1
    for name, n in worst.most_common(8):
        print("   %-46s on %d donors" % (name, n))
    print("wrote %s" % args.out)


if __name__ == "__main__":
    main()
