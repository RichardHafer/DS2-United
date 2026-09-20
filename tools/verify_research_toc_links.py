"""Resolve private TOC references against stock descriptors using native identity rules."""
import json
import argparse
from collections import defaultdict
from pathlib import Path
from read_stock_toc import parse

ROOT = Path(__file__).resolve().parents[1] / "analysis"


def key(identity):
    value = int(identity["hash"], 16)
    return ("hash", value) if value else ("name", identity["name"])


def resolve(identity, index):
    candidates = index.get(key(identity), [])
    if not candidates:
        return "missing", []
    if any(c["entry"]["identity"]["name"] != identity["name"] for c in candidates):
        return "identity_collision", candidates
    signatures = {
        (c["entry"]["content_size"], c["entry"]["flags"], c["entry"].get("dlc_index", 0), c["entry"]["leading_count"],
         tuple(key(r) for r in c["entry"]["leading_references"]),
         tuple(key(r) for r in c["entry"]["dependencies"])) for c in candidates}
    return ("resolved" if len(signatures) == 1 else "variant_descriptors"), candidates


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=ROOT / "research_stream")
    args = parser.parse_args()
    stock = json.loads((ROOT / "stock_toc_dependencies.json").read_text())
    index = defaultdict(list)
    for toc in stock["tocs"]:
        for entry in toc["entries"]:
            index[key(entry["identity"])].append({"toc": toc["file"], "entry": entry})
    root = parse((args.directory / "ds2_randomizer_pool.toc").read_bytes())["entries"][0]
    pending = root["leading_references"] + root["dependencies"]
    visited, results = set(), []
    while pending:
        identity = pending.pop(0)
        marker = (key(identity), identity["name"])
        if marker in visited:
            continue
        visited.add(marker)
        status, candidates = resolve(identity, index)
        results.append({"identity": identity, "status": status,
                        "candidate_tocs": sorted({c["toc"] for c in candidates})})
        if status == "resolved":
            entry = candidates[0]["entry"]
            pending.extend(entry["leading_references"] + entry["dependencies"])
    errors = [r for r in results if r["status"] != "resolved"]
    report = {"scope": "Stock corpus availability, not the active manager registry",
              "runtime_ready": False, "references": results, "unresolved": errors}
    (args.directory / "toc_links.json").write_text(json.dumps(report, indent=2))
    print(f"Recursive references: {len(results)}; unresolved or conflicting: {len(errors)}")
    for error in errors:
        print(error["status"], error["identity"]["name"])
    if errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
