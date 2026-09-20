"""Match dependency GUIDs against complete PC SHDR resource identities."""
import argparse
import json
import struct
from collections import defaultdict, Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def header_guid(header, meta):
    if len(header) != 44 or header[:4] != b"COHS" or header[8:12] != b"RDHS":
        raise ValueError("Unsupported resource header")
    if struct.unpack_from("<I", header, 4)[0] < 44:
        raise ValueError("Short SHDR chunk")
    words = struct.unpack_from("<4I", header, 24)
    if words[0] != meta["resource_id"] or struct.unpack_from("<I", header, 40)[0] != meta["size"]:
        raise ValueError("Catalog/header mismatch")
    return "-".join(f"{word:08X}" for word in words)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args()
    graph = json.loads((ROOT / "analysis/pool_dependency_closure.json").read_text(encoding="utf-8"))
    required = {e["target"] for root in graph["roots"] for e in root["edges"]
                if e["status"] == "resource_or_unresolved"}
    prefixes = {int(guid.split("-")[0], 16) for guid in required}
    archives = defaultdict(list)
    with (ROOT / "analysis/catalog/assets.jsonl").open(encoding="utf-8") as stream:
        for line in stream:
            asset = json.loads(line)
            if asset["resource_id"] in prefixes:
                archives[asset["archive"]].append(asset)
    matches, errors = defaultdict(list), []
    checked = 0
    for archive, entries in sorted(archives.items()):
        try:
            with (args.work / archive).open("rb") as stream:
                if stream.read(4) != b"3slo":
                    raise ValueError("Not a PC stream")
                for asset in entries:
                    stream.seek(asset["chunk_offset"])
                    try:
                        guid = header_guid(stream.read(44), asset)
                        checked += 1
                        if guid in required:
                            matches[guid].append(asset)
                    except ValueError as error:
                        errors.append({"archive": archive, "offset": asset["chunk_offset"], "error": str(error)})
        except (OSError, ValueError) as error:
            errors.append({"archive": archive, "error": str(error)})
    report = {"scope": "Exact header GUID matches only; payload closure and residency not verified",
              "checked_headers": checked, "errors": errors,
              "resources": [{"guid": guid, "status": "header_matched" if matches[guid] else "unresolved",
                             "candidates": matches[guid]} for guid in sorted(required)], "roots": []}
    for root in graph["roots"]:
        guids = {e["target"] for e in root["edges"] if e["status"] == "resource_or_unresolved"}
        matched = sorted(g for g in guids if matches[g])
        unresolved = sorted(guids - set(matched))
        report["roots"].append({"package": root["package"], "header_matched": matched, "unresolved": unresolved,
                                "runtime_ready": False})
    output = ROOT / "analysis/pool_resource_matches.json"
    output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Required GUIDs: {len(required)}; exact header matches: {sum(bool(matches[g]) for g in required)}")
    print(f"Checked headers: {checked}; errors: {len(errors)}")
    types = Counter(a["type"] for g in required for a in matches[g])
    print("Matching asset occurrences:", dict(sorted(types.items())))
    for root in report["roots"]:
        print(f"{root['package']}: matched={len(root['header_matched'])} unresolved={len(root['unresolved'])}")


if __name__ == "__main__":
    main()
