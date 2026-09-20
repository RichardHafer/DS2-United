"""Extract single-instance research payloads. These are not installable streams."""
import argparse
import csv
import hashlib
import json
import struct
import sys
from collections import defaultdict
from pathlib import Path

from ds2_assets import assets, simgroup, loose_images, u32


def isolate(data, index):
    result, selected, unused = isolate_many(data, [index])
    return result, selected[0], unused


def isolate_many(data, indices):
    entities, base = simgroup(data)
    images = loose_images(data, base)
    indices = list(indices)
    if not indices or len(set(indices)) != len(indices):
        raise ValueError("Empty or duplicate instance selection")
    if any(not 0 <= index < len(entities) for index in indices):
        raise ValueError("Instance index out of range")
    selected = [entities[index] for index in sorted(indices)]
    if any(row["spawn_flags"] != 0 for row in selected):
        raise ValueError("Donor has nonzero spawn flags")
    required = {image for row in selected for image in [row["image_id"], *row["component_images"]]}
    if not required <= images.keys():
        raise ValueError("Unresolved parameter image")
    start = u32(data, 12)
    end = start + len(entities) * 68
    if end > base:
        raise ValueError("Instance table overlaps parameter images")
    records = b"".join(data[row["offset"]:row["offset"] + 68] for row in selected)
    # Keep offset-based tables in place. Only the instance table is reduced.
    result = bytearray(data)
    result[start:end] = bytes(end - start)
    result[start:start + len(records)] = records
    struct.pack_into("<H", result, 0x16, len(selected))
    restored, new_base = simgroup(result)
    if [row["guid"] for row in restored] != [row["guid"] for row in selected]:
        raise ValueError("Instance round trip failed")
    if new_base != base or result[base:] != data[base:]:
        raise ValueError("Parameter payload changed")
    if loose_images(result, base) != images:
        raise ValueError("Parameter image round trip failed")
    return bytes(result), selected, len(images) - len(required)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--tool-dir", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, args.tool_dir)
    import ds2str
    root = Path(__file__).resolve().parents[1]
    catalog = root / "analysis/catalog"
    manifest = json.loads((root / "analysis/common_enemy_streams.json").read_text())
    with (catalog / "enemy_targets.tsv").open(newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    keys = {(r["archive"], r["asset"], int(r["entity_index"])) for r in rows}
    entities, guid_locations = {}, defaultdict(list)
    with (catalog / "entities.jsonl").open(encoding="utf-8") as f:
        for line in f:
            entity = json.loads(line)
            guid_locations[entity["guid"]].append({"class": entity["class"],
                                                    "archive": entity["archive"]})
            key = entity["archive"], entity["asset"], entity["index"]
            if key in keys:
                entities[key] = entity
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"runtime_ready": False, "entries": [], "skipped": []}
    for package in manifest["packages"]:
        name = package["package"]
        candidates = [r for r in rows if r["multiplayer"] == "False"
                      and name in r["package_candidates"].split("|")
                      and r["mode"] in ("SPAWNMODE_NONE", "default_NONE_descriptor")
                      and not r["inactive_animation"] and not r["active_animation"]]
        if not candidates:
            report["skipped"].append({"package": name, "reason": "No unanimated ground donor"})
            continue
        # Prefer fewer chapter entity references, then stable catalog order.
        def chapter_refs(row):
            e = entities[row["archive"], row["asset"], int(row["entity_index"])]
            return sum(bool(guid_locations.get(f.get("guid_candidate")))
                       for im in e["images"] for f in im.get("fields", []))
        candidates.sort(key=lambda r: (chapter_refs(r), r["archive"], r["guid"]))
        row = candidates[0]
        entry = {"package": name, "class": row["class"], "guid": row["guid"],
                 "source_archive": row["archive"], "source_asset": row["asset"],
                 "character_archive": package["character_archive"], "runtime_ready": False}
        try:
            stream = ds2str.StreamSet(args.work / row["archive"])
            if not stream.pc:
                raise ValueError("Not a PC stream")
            matches = [(meta, chunks) for meta, chunks in assets(stream)
                       if meta["type"] == "SimGroup" and meta["path"] == row["asset"]]
            if len(matches) != 1:
                raise ValueError("Source asset is not unique")
            meta, chunks = matches[0]
            data = stream.asset_payload(chunks, meta["size"])
            candidate, instance, orphan_images = isolate(data, int(row["entity_index"]))
            if instance["guid"] != row["guid"]:
                raise ValueError("Catalog does not match source archive")
            entity = entities[row["archive"], row["asset"], int(row["entity_index"])]
            references = []
            for im in entity["images"]:
                for field in im.get("fields", []):
                    guid = field.get("guid_candidate")
                    if guid and guid != "00000000-00000000-00000000-00000000":
                        references.append({"component": im["class"], "field": field["name"],
                                           "guid": guid, "entity_matches": guid_locations.get(guid, []),
                                           "classification": "entity" if guid in guid_locations
                                           else "resource_or_unresolved"})
            filename = name + ".candidate.simgroup.bin"
            (args.output / filename).write_bytes(candidate)
            entry.update({"file": filename, "source_sha256": hashlib.sha256(data).hexdigest(),
                          "candidate_sha256": hashlib.sha256(candidate).hexdigest(),
                          "bytes": len(candidate), "retained_unreferenced_images": orphan_images,
                          "parameter_payload_unchanged": True, "instance_count": 1,
                          "spawn_flags": instance["spawn_flags"], "references": references,
                          "blockers": ["Original entity and group IDs are retained",
                                       "Unused parameter images and lists are retained",
                                       "Resource closure and dispatch semantics need retail verification",
                                       "Stream ownership and unload order are not implemented"]})
            report["entries"].append(entry)
        except (ValueError, OSError, struct.error) as error:
            report["skipped"].append({"package": name, "reason": str(error)})
    (args.output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("Single-instance candidates:", len(report["entries"]))
    print("Skipped:", len(report["skipped"]))
    print("Candidates with entity references:", sum(any(r["entity_matches"] for r in e["references"])
                                                    for e in report["entries"]))
    print("Runtime ready: false. No game files were changed.")


if __name__ == "__main__":
    main()
