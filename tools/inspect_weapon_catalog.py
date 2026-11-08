"""Cross-check resident Quartermaster GUIDs against extracted GunProxy data."""
import argparse
import json
from collections import defaultdict
from pathlib import Path


def guid_text(raw):
    data = bytes.fromhex(raw)
    return "-".join(f"{int.from_bytes(data[i:i+4], 'little'):08X}" for i in range(0, 16, 4))


def name_hash(name):
    value = 0
    for byte in name.encode("ascii"):
        if 65 <= byte <= 90:
            byte += 32
        value = (value * 0x1003F + byte) & 0xFFFFFFFF
    return value


def inspect(catalog, report):
    records = {guid_text(r["gun_proxy_guid"]): r for r in report["stream_record_details"]}
    candidates = defaultdict(list)
    matched = set()
    mismatches = []
    for line in catalog.open(encoding="utf-8"):
        entity = json.loads(line)
        if entity["guid"] not in records or entity.get("class") != "GunProxy":
            continue
        fields = {field.get("name"): field for image in entity["images"]
                  if image.get("class") == "GunProxy" for field in image["fields"]}
        name = fields.get("CMD_SetName", {}).get("string_candidate")
        streamed = fields.get("CMD_SetStreamedGunGUID", {}).get("guid_candidate")
        record = records[entity["guid"]]
        if not name or streamed != guid_text(record["template_guid"]):
            mismatches.append({"guid": entity["guid"], "archive": entity["archive"],
                               "name": name, "streamed_guid": streamed})
            continue
        matched.add(entity["guid"])
        candidates[f"{name_hash(name):08X}"].append({
            "name": name, "proxy_guid": entity["guid"], "streamed_guid": streamed,
            "record_index": record["index"], "archive": entity["archive"]})
    groups = {}
    for key, values in sorted(candidates.items()):
        guids = sorted({entry["proxy_guid"] for entry in values})
        groups[key] = {"status": "unique" if len(guids) == 1 else "ambiguous",
                       "proxy_guids": guids, "sources": values}
    observed = []
    for player in report["players"]:
        for weapon in player["weapons"]:
            key = f"{weapon['weapon_hash']:08X}"
            group = groups.get(key)
            matching = [entry for entry in (group or {}).get("sources", [])
                        if entry["streamed_guid"] == guid_text(weapon["template_guid"])]
            observed.append({"player_slot": player["slot"], "hash": key,
                             "status": (group or {}).get("status", "missing"),
                             "matching_proxy_guids": sorted({e["proxy_guid"] for e in matching})})
    return {"dump": report["dump"], "resident_records": len(records),
            "matched_records": len(matched), "unmatched_guids": sorted(set(records)-matched),
            "metadata_mismatches": mismatches, "hash_groups": groups,
            "observed_weapons": observed,
            "scope": "Offline catalog candidates; resident attributes and peer loadout are not validated."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("dump_report", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = inspect(args.catalog, json.loads(args.dump_report.read_text(encoding="utf-8")))
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Matched {result['matched_records']}/{result['resident_records']} records; "
          f"{sum(g['status'] == 'ambiguous' for g in result['hash_groups'].values())} ambiguous hashes; "
          f"{len(result['metadata_mismatches'])} metadata mismatches.")
