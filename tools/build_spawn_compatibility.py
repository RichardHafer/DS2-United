"""Separate authored placement evidence from adapters that still need validation."""
import csv
import hashlib
import json
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIRECT = {"SPAWNMODE_NONE", "default_NONE_descriptor"}
VENTS = {"Wall", "Ceiling", "Floor", "default_FLOOR_descriptor"}
EXTENDED = {"Infector", "Stalker", "SwarmEnemy"}


def arrival(row):
    if row["inactive_animation"] or row["active_animation"]:
        return "authored_animation"
    if row["context"] in DIRECT:
        return "direct"
    if (row["mode"] == "SPAWNMODE_VENT" and row["vent_type"] in VENTS
            and int(row["vent_matches"]) > 0 and row["vent_guid"]
            and len(row["vent_components"].split("|")) == 1
            and row["vent_components"].endswith("VentComponent")):
        return "vent:" + row["vent_type"]
    return "unverified:" + row["context"]


def eligible(row):
    return (row["eligibility"] == "ordinary_candidate_unverified_swap"
            or row["class"] in EXTENDED)


def classify(source, target, exact_pairs):
    if not eligible(source) or not eligible(target):
        return "protected_class"
    if not source["model_guid"] or not target["model_guid"]:
        return "missing_model"
    if source["model_guid"] == target["model_guid"]:
        return "same_model"
    sa, ta = arrival(source), arrival(target)
    if sa == "direct" and ta == "direct":
        return "direct_candidate"
    if not sa.startswith("vent:") or sa != ta:
        return "arrival_not_matched"
    if (source["guid"], target["guid"]) in exact_pairs:
        return "authored_same_vent_candidate"
    return "requires_source_vent_rebinding"


def main():
    catalog = ROOT / "analysis/catalog/enemy_targets.tsv"
    with catalog.open(encoding="utf-8", newline="") as stream:
        rows = [row for row in csv.DictReader(stream, delimiter="\t")
                if row["multiplayer"] == "False"]
    authored = json.loads((ROOT / "analysis/vent_pair_candidates.json").read_text(encoding="utf-8"))
    exact = {(source["guid"], target["guid"]) for source in authored["sources"]
             for target in source["targets"]}
    by_guid = defaultdict(list)
    for row in rows:
        by_guid[row["guid"]].append(row)
    keys = ("class", "model_guid", "context", "vent_guid", "vent_type", "vent_components",
            "inactive_animation", "active_animation", "animation_flags", "activation_flags")
    ambiguous = {guid for guid, group in by_guid.items()
                 if len({tuple(row[key] for key in keys) for row in group}) != 1}
    unique = [group[0] for guid, group in sorted(by_guid.items()) if guid not in ambiguous]
    capabilities = Counter((row["class"], row["model_assets"], arrival(row)) for row in unique)
    counts = Counter()
    pairs = []
    family_pairs = Counter()
    for source in unique:
        for target in unique:
            status = classify(source, target, exact)
            counts[status] += 1
            if status not in {"direct_candidate", "authored_same_vent_candidate",
                              "requires_source_vent_rebinding"}:
                continue
            pairs.append({"source": source["guid"], "target": target["guid"],
                          "source_class": source["class"], "target_class": target["class"],
                          "arrival": arrival(source), "status": status})
            family_pairs[status, arrival(source), source["class"], target["class"]] += 1
    report = {
        "scope": "Offline candidates, not confirmed swaps or resident assets",
        "catalog_sha256": hashlib.sha256(catalog.read_bytes()).hexdigest(),
        "unique_records": len(unique), "ambiguous_guids": sorted(ambiguous),
        "counts": dict(counts),
        "requirements": {
            "direct_candidate": ["Complete resident donor", "Native creation checks", "Behavior test"],
            "authored_same_vent_candidate": ["Complete resident donor", "Native creation checks", "Behavior test"],
            "requires_source_vent_rebinding": ["Component list lifetime validation", "Original vent transfer",
                "Complete resident donor", "Model entry animation and collision validation", "Behavior test"]},
        "capabilities": [{"class": family, "model": model, "arrival": context, "records": count}
                         for (family, model, context), count in sorted(capabilities.items())],
        "family_pairs": [{"status": status, "arrival": context, "source": source,
                          "target": target, "pairs": count}
                         for (status, context, source, target), count in sorted(family_pairs.items())],
        "pairs": pairs,
    }
    output = ROOT / "analysis/spawn_compatibility.json"
    output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Unique campaign targets: {len(unique)}; ambiguous GUIDs: {len(ambiguous)}")
    for status, count in sorted(counts.items()):
        print(f"{status}: {count}")
    for context in sorted({arrival(row) for row in unique if arrival(row).startswith('vent:')}):
        families = sorted({row['class'] for row in unique if eligible(row) and arrival(row) == context})
        print(f"{context}: {', '.join(families)}")
    print("No runtime gates were relaxed. No game files were changed.")


if __name__ == "__main__":
    main()
