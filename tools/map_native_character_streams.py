"""Match character archives to exact null-terminated names in stock TOCs."""
import argparse
import hashlib
import json
from pathlib import Path, PureWindowsPath

ROOT = Path(__file__).resolve().parents[1]


def stream_name(archive):
    path = PureWindowsPath(archive)
    if len(path.parts) < 3 or not path.parts[0].startswith("ds2dat"):
        raise ValueError("Expected an extracted stock archive path")
    if path.suffix.lower() != ".str":
        raise ValueError("Expected a character STR")
    return str(PureWindowsPath(*path.parts[1:]).with_suffix(""))


def name_offsets(data, name):
    needle = name.encode("ascii") + b"\0"
    result = []
    offset = data.find(needle)
    while offset >= 0:
        if offset == 0 or data[offset - 1] == 0:
            result.append(offset)
        offset = data.find(needle, offset + 1)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    packages = json.loads((ROOT / "analysis/common_enemy_streams.json").read_text())["packages"]
    toc_data = [(p, p.read_bytes()) for p in sorted(args.work.rglob("*.toc"))]
    entries = []
    for package in packages:
        archive = package["character_archive"]
        name = stream_name(archive)
        matches = []
        for path, data in toc_data:
            offsets = name_offsets(data, name)
            if offsets:
                matches.append({"toc": str(path.relative_to(args.work)),
                                "sha256": hashlib.sha256(data).hexdigest(),
                                "name_offsets": offsets})
        file = args.work / archive
        entries.append({"package": package["package"], "classes": package["classes"],
                        "archive": archive, "archive_exists": file.is_file(),
                        "archive_bytes": file.stat().st_size if file.is_file() else None,
                        "toc_name": name if matches else None, "toc_matches": matches,
                        "runtime_ready": False})
    report = {"scope": "Exact stock TOC string evidence, not runtime dispatch verification",
              "strategy": "Load original character streams and a separate private template pool",
              "packages": entries}
    (ROOT / "analysis/native_character_streams.json").write_text(json.dumps(report, indent=2))
    matched = sum(bool(e["toc_matches"]) for e in entries)
    print(f"TOC name matches: {matched}/{len(entries)}; TOCs examined: {len(toc_data)}")
    for entry in entries:
        if not entry["toc_matches"] or not entry["archive_exists"]:
            print("Unresolved:", entry["package"], entry["archive"])


if __name__ == "__main__":
    main()
