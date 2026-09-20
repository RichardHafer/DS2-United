"""Compare stock TOC content sizes against catalogued asset payload sizes."""
import json
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "analysis"


def main():
    packages = json.loads((ROOT / "native_character_streams.json").read_text())["packages"]
    archives = {p["archive"] for p in packages}
    totals = defaultdict(lambda: {"bytes": 0, "aligned16": 0, "count": 0})
    with (ROOT / "catalog/assets.jsonl").open(encoding="utf-8") as source:
        for line in source:
            asset = json.loads(line)
            if asset["archive"] not in archives:
                continue
            entry = totals[asset["archive"]]
            size = asset["size"]
            entry["bytes"] += size
            entry["aligned16"] += (size + 15) & ~15
            entry["count"] += 1
    descriptors = defaultdict(set)
    for toc in json.loads((ROOT / "stock_toc_dependencies.json").read_text())["tocs"]:
        for entry in toc["entries"]:
            descriptors[entry["identity"]["name"]].add(entry["content_size"])
    rows = []
    for package in packages:
        sizes = sorted(descriptors[package["toc_name"]])
        total = totals[package["archive"]]
        row = {"package": package["package"], "archive": package["archive"],
               "toc_sizes": sizes, "assets": total,
               "exact_payload_sum": sizes == [total["bytes"]],
               "delta": [size - total["bytes"] for size in sizes]}
        rows.append(row)
        print(row["package"], "TOC", sizes, "payload", total["bytes"], "delta", row["delta"])
    (ROOT / "toc_content_size_audit.json").write_text(json.dumps({"packages": rows}, indent=2))


if __name__ == "__main__":
    main()
