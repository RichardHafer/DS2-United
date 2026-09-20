"""Choose the play-dead donors the pool should carry.

A play-dead slasher is the third way an enemy arrives, after walking in and
climbing out of a vent, and the pool never carried one: it holds 34 direct
arrivals and 74 vent arrivals, so a corpse that gets up had nothing to be
replaced with and the spawner tool reported "no compatible candidate".

The contract is strict and stays strict. `compatibleScripted` demands the same
family, the same context and the same scriptedKey - the hash over the whole
authored spawning component, both animation names, the options, the activation
flags and every message. Two corpses only swap when they were authored to play
exactly the same lying-down and getting-up animations, which is the only
evidence that the donor's model can perform them.

Across the game that leaves 15 play-dead entities, all slashers, in twelve
contracts. Ten contracts hold a single entity and can never swap with anything.
The two that hold more are what this plans for.
"""
import argparse
import csv
import collections
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLAYDEAD = "SPAWNMODE_ZOMBIE_PLAYDEAD"

# The generated catalog is the runtime's own source of truth for the contract, so
# the plan reads the key from there rather than recomputing it.
ROW = re.compile(
    r"\{\{0x([0-9A-Fa-f]{8})u,0x([0-9A-Fa-f]{8})u,0x([0-9A-Fa-f]{8})u,0x([0-9A-Fa-f]{8})u\},"
    r"\{[^}]*\},\"([^\"]*)\",\"([^\"]*)\","
    r"(true|false),(true|false),(true|false),"
    r"\"([^\"]*)\",\"([^\"]*)\",\"([^\"]*)\",\"([^\"]*)\"")


def catalog_contracts(path):
    out = {}
    for match in ROW.finditer(path.read_text(encoding="utf-8")):
        guid = "%s-%s-%s-%s" % tuple(match.group(i).upper() for i in (1, 2, 3, 4))
        out[guid] = {"family": match.group(5), "context": match.group(6),
                     "ordinary": match.group(7) == "true", "scripted": match.group(12)}
    return out


def resident_packages(path):
    """The character streams the plugin already forces resident."""
    names = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        if "char_str" in line:
            names.add(line.split('"')[1].replace("\\\\", "\\").split("\\")[-1])
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, default=ROOT / "src/generated_enemy_catalog.h")
    parser.add_argument("--targets", type=Path, default=ROOT / "analysis/catalog/enemy_targets.tsv")
    parser.add_argument("--requirements", type=Path,
                        default=ROOT / "src/generated_stream_requirements.h")
    parser.add_argument("--out", type=Path, default=ROOT / "analysis/playdead_pool_plan.json")
    args = parser.parse_args()

    contracts = catalog_contracts(args.catalog)
    resident = resident_packages(args.requirements)
    with args.targets.open(newline="", encoding="utf-8") as file:
        rows = [r for r in csv.DictReader(file, delimiter="\t")
                if r["mode"] == PLAYDEAD and r["multiplayer"] == "False"]

    groups = collections.defaultdict(list)
    for row in rows:
        entry = contracts.get(row["guid"])
        if not entry or not entry["ordinary"] or not entry["scripted"]:
            continue
        groups[entry["scripted"]].append((row, entry))

    plan, refused = [], collections.Counter()
    for key, members in sorted(groups.items()):
        if len(members) < 2:
            refused["contract has no partner"] += len(members)
            continue
        taken = set()
        for row, entry in members:
            package = row["package_candidates"].split("|")[0]
            if not package:
                refused["no character package"] += 1
                continue
            if package not in resident:
                refused["stream not forced resident"] += 1
                continue
            # Two entities of the same package under the same contract are the
            # same model playing the same animations. One of them is the donor;
            # a second would only add weight to a swap that changes nothing.
            if package in taken:
                refused["package already covered"] += 1
                continue
            taken.add(package)
            plan.append({"contract": key, "package": package, "family": entry["family"],
                         "guid": row["guid"], "archive": row["archive"], "asset": row["asset"],
                         "entity_index": row["entity_index"], "script_name": row["script_name"],
                         "context": entry["context"],
                         "spawns_of_contract": len(members)})
    # Deterministic order, and the most useful contracts first.
    plan.sort(key=lambda p: (-p["spawns_of_contract"], p["contract"], p["guid"]))
    args.out.write_text(json.dumps(plan, indent=1))

    print("play-dead entities: %d in %d contracts" % (len(rows), len(groups)))
    print("donors planned: %d" % len(plan))
    for reason, count in refused.most_common():
        print("   refused, %-28s %d" % (reason + ":", count))
    for key, members in sorted(groups.items()):
        if len(members) < 2:
            continue
        chosen = [p["package"] for p in plan if p["contract"] == key]
        print("   contract %s: %s" % (key[:16], ", ".join(chosen) or "nothing usable"))


if __name__ == "__main__":
    main()
