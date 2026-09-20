"""Inventory common-enemy stream candidates without claiming runtime readiness."""
import csv
import json
from collections import Counter, defaultdict
from pathlib import Path

root = Path(__file__).resolve().parents[1]
catalog = root / "analysis/catalog"
common = {"Zombie", "Exploder", "Leaper", "Lurker", "PackEnemy", "Pregnant",
          "Puker", "TubeBeetle", "Stalker", "Infector", "Brute", "SwarmEnemy",
          "DividerCore", "DividerPart", "Hunter"}
def rows(name):
    with (catalog / name).open(encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream, delimiter="\t"))

packages = {row["package"]: row for row in rows("npc_packages.tsv")}
assets = defaultdict(lambda: {"types": Counter(), "banks": set()})
with (catalog / "assets.jsonl").open(encoding="utf-8") as stream:
    for line in stream:
        asset = json.loads(line)
        entry = assets[asset["archive"]]
        entry["types"][asset["type"]] += 1
        if asset["type"] == "SBK": entry["banks"].add(asset["name"])
counts = Counter()
with (catalog / "entities.jsonl").open(encoding="utf-8") as stream:
    for line in stream:
        entity = json.loads(line)
        counts[entity["archive"]] += 1
enemies = rows("enemy_targets.tsv")
result = []
for package, info in sorted(packages.items()):
    matches = [e for e in enemies if e["class"] in common and e["multiplayer"] == "False"
               and package in e["package_candidates"].split("|")]
    if not matches: continue
    audio_candidates = sorted(a for a in assets if a.replace("\\", "/").split("/")[-1] == package + "_audio.str")
    entry = assets[info["archive"]]
    result.append({"package": package, "classes": sorted({e["class"] for e in matches}),
        "character_archive": info["archive"], "asset_types": dict(entry["types"]),
        "embedded_banks": sorted(entry["banks"]), "audio_archive_name_candidates": audio_candidates,
        "donors": [{"guid": e["guid"], "archive": e["archive"], "context": e["context"],
                    "archive_entity_count": counts[e["archive"]]} for e in matches],
        "runtime_ready": False})
output = root / "analysis/common_enemy_streams.json"
output.write_text(json.dumps({"scope": "Offline candidates, not a load order or dependency closure",
                             "packages": result}, indent=2), encoding="utf-8")
print("Packages:", len(result))
for entry in result:
    print(entry["package"], "donors",len(entry["donors"]), "banks",len(entry["embedded_banks"]),
          "audio candidates",len(entry["audio_archive_name_candidates"]))
