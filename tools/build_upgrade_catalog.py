"""Decode the authored bench upgrade trees from player_upgrades.lua.

The bench is not packed data: every node and base value is plain Lua text that
UpgradeManager executes at load. This reads it without running it, so a node it
cannot classify is reported rather than guessed.
"""
import argparse
import hashlib
import json
import re
from collections import Counter, defaultdict
from pathlib import Path

# EARS::Modules::UpgradeNodeType, from the Xbox PDB. NODE_BLANK is index 0 and
# is what every filler node collapses to.
NODE_TYPES = ["Blank", "Damage", "Speed", "Capacity", "Width", "Reload", "AltDamage",
              "HP", "Air", "Energy", "Duration", "Range", "Charge", "Special"]
STAT_NAMES = set(NODE_TYPES[1:])

# Items whose tree exists but carries no stat node, so nothing can be permuted.
INERT = {"FlashLight", "finger_gun", "MP_Human"}

UPGRADE = re.compile(r'u\s*=\s*um:Upgrade\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\)')
AT = re.compile(r'u:At\(\s*(-?\d+)\s*,\s*(-?\d+)\s*\)')
BASE = re.compile(r'um:BaseValue\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(-?[\d.]+)\s*\)')


def node_kind(name):
    """A node name is its stat plus an optional ordinal: Capacity3 -> Capacity.

    Anything else is filler that only exists to connect the grid.
    """
    stem = re.sub(r"\d+$", "", name)
    return stem if stem in STAT_NAMES else None


def parse(text):
    items = defaultdict(lambda: {"nodes": [], "baseValues": {}})
    current = None
    for raw in text.split("\n"):
        line = raw.split("--")[0].strip()
        if not line:
            continue
        found = UPGRADE.match(line)
        if found:
            item, name = found.group(1), found.group(2)
            items[item]["nodes"].append({"name": name, "kind": node_kind(name),
                                         "x": None, "y": None, "startPoint": False})
            current = item
            continue
        found = AT.match(line)
        if found and current:
            node = items[current]["nodes"][-1]
            node["x"], node["y"] = int(found.group(1)), int(found.group(2))
            continue
        if line.startswith("u:StartPoint") and current:
            items[current]["nodes"][-1]["startPoint"] = True
            continue
        found = BASE.match(line)
        if found:
            items[found.group(1)]["baseValues"][found.group(2)] = found.group(3)
    return items


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lua", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    raw = args.lua.read_bytes()
    items = parse(raw.decode("utf-8", errors="ignore"))

    report = {"source": args.lua.name,
              "sha256": hashlib.sha256(raw).hexdigest(),
              "nodeTypes": NODE_TYPES,
              "items": {}}
    problems = []
    for item, data in sorted(items.items()):
        nodes = data["nodes"]
        placed = [n for n in nodes if n["x"] is not None]
        if len(placed) != len(nodes):
            problems.append(f"{item}: {len(nodes) - len(placed)} node(s) without a grid position")
        cells = Counter((n["x"], n["y"]) for n in placed)
        for cell, count in cells.items():
            if count > 1:
                problems.append(f"{item}: {count} nodes share grid cell {cell}")
        stat = [n for n in nodes if n["kind"]]
        report["items"][item] = {
            "nodes": nodes,
            "baseValues": data["baseValues"],
            "statNodes": len(stat),
            "blankNodes": len(nodes) - len(stat),
            "startPoints": sum(1 for n in nodes if n["startPoint"]),
            "kinds": dict(Counter(n["kind"] for n in stat)),
            # Needs at least two placed stat nodes to permute at all, and a
            # start point, or the tree is never entered from the bench UI.
            # KinesisModule is the case that forces this: one unplaced Range
            # node carrying only a base value.
            "permutable": (item not in INERT and
                           sum(1 for n in stat if n["x"] is not None) >= 2 and
                           any(n["startPoint"] for n in nodes)),
        }

    report["totals"] = {
        "items": len(report["items"]),
        "permutableItems": sum(1 for v in report["items"].values() if v["permutable"]),
        "nodes": sum(len(v["nodes"]) for v in report["items"].values()),
        "statNodes": sum(v["statNodes"] for v in report["items"].values()),
        "blankNodes": sum(v["blankNodes"] for v in report["items"].values()),
    }
    report["problems"] = problems

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(f"{report['totals']['items']} items, "
          f"{report['totals']['permutableItems']} permutable, "
          f"{report['totals']['nodes']} nodes "
          f"({report['totals']['statNodes']} stat, {report['totals']['blankNodes']} blank)")
    for line in problems:
        print("problem:", line)
    if not problems:
        print("no unplaced nodes and no grid collisions")


if __name__ == "__main__":
    main()
