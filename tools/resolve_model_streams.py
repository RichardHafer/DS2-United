"""Resolve model stream GUIDs through named SimGroup assets."""
import argparse
import json
import struct
import uuid
from pathlib import Path
from resolve_pool_resources import header_guid

ROOT = Path(__file__).resolve().parents[1]


def guid_name(name):
    if not (name.startswith("{") and name.endswith("}")):
        return None
    try:
        value = uuid.UUID(name)
    except ValueError:
        return None
    return "-".join(f"{word:08X}" for word in struct.unpack("<4I", value.bytes_le))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args()
    resources = json.loads((ROOT / "analysis/pool_resource_matches.json").read_text(encoding="utf-8"))
    required = {r["guid"] for r in resources["resources"] if r["status"] == "unresolved"}
    matches = {guid: [] for guid in required}
    errors = []
    with (ROOT / "analysis/catalog/assets.jsonl").open(encoding="utf-8") as catalog:
        for line in catalog:
            asset = json.loads(line)
            if asset["type"] != "SimGroup":
                continue
            guid = guid_name(asset["name"])
            if guid not in required:
                continue
            try:
                with (args.work / asset["archive"]).open("rb") as stream:
                    if stream.read(4) != b"3slo":
                        raise ValueError("Not a PC stream")
                    stream.seek(asset["chunk_offset"])
                    header = stream.read(44)
                    header_guid(header, asset)
                    expected = asset["name"].encode("latin1") + b"\0"
                    if struct.unpack_from("<I", header, 4)[0] < 44 + len(expected):
                        raise ValueError("Name exceeds SHDR bounds")
                    if stream.read(len(expected)) != expected:
                        raise ValueError("Catalog name does not match SHDR")
                matches[guid].append(asset)
            except (OSError, ValueError) as error:
                errors.append({"asset": asset, "error": str(error)})
    report = {"scope": "Full GUID match to verified SimGroup asset name; native stream resolution not yet tested",
              "errors": errors, "matches": matches,
              "unresolved": sorted(g for g, candidates in matches.items() if not candidates)}
    (ROOT / "analysis/model_stream_matches.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Model stream GUIDs: {len(required)}; matched: {len(required)-len(report['unresolved'])}; errors: {len(errors)}")
    for guid, candidates in sorted(matches.items()):
        print(guid, len(candidates), ", ".join(sorted({Path(a['archive']).stem for a in candidates})))


if __name__ == "__main__":
    main()
