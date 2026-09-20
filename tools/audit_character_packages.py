"""Inventory registration side effects before selecting global pool packages."""
import json
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    manifest = json.loads((ROOT / "analysis/common_enemy_streams.json").read_text(encoding="utf-8"))
    wanted = {p["character_archive"] for p in manifest["packages"]}
    groups, entities = defaultdict(list), defaultdict(list)
    with (ROOT / "analysis/catalog/assets.jsonl").open(encoding="utf-8") as stream:
        for line in stream:
            asset = json.loads(line)
            if asset["archive"] in wanted and asset["type"] == "SimGroup":
                groups[asset["archive"]].append(asset)
    with (ROOT / "analysis/catalog/entities.jsonl").open(encoding="utf-8") as stream:
        for line in stream:
            entity = json.loads(line)
            if entity["archive"] not in wanted:
                continue
            entities[entity["archive"]].append({k: entity[k] for k in
                ("guid", "class", "asset", "index", "spawn_flags", "transform_offset")})
    report = {"scope": "Catalog side-effect inventory, not a runtime loading approval", "packages": []}
    for package in manifest["packages"]:
        archive = package["character_archive"]
        rows = entities[archive]
        group_assets = groups[archive]
        report["packages"].append({"package": package["package"], "archive": archive,
            "groups": group_assets, "entities": rows,
            "classes": dict(Counter(row["class"] for row in rows)),
            "nonzero_spawn_flags": sum(row["spawn_flags"] != 0 for row in rows),
            "inventory_consistent": bool(group_assets) == bool(rows),
            "runtime_ready": False})
    (ROOT / "analysis/character_package_audit.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    for entry in report["packages"]:
        print(f"{entry['package']}: groups={len(entry['groups'])} entities={len(entry['entities'])} flagged={entry['nonzero_spawn_flags']} classes={entry['classes']}")


if __name__ == "__main__":
    main()
