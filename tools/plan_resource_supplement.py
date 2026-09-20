"""Subtract stock coverage from the typed private-template resource closure."""
import json
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "analysis"


def main():
    def read(name):
        return json.loads((ROOT / name).read_text(encoding="utf-8"))
    streams = read("native_character_streams.json")["packages"]
    stock = {r["archive"] for r in streams if r["archive_exists"] and r["toc_name"]}
    stock.add("ds2dat10\\global_assets\\global_assets.str")
    stock.add("ds2dat10\\global_assets\\global_assets_global.str")
    closure = read("metamodel_resource_closure.json")
    if closure["errors"]:
        raise ValueError("Resource closure contains errors")
    resources = {r["guid"]: r for r in closure["resources"]}
    edges = defaultdict(set)
    for edge in closure["edges"]:
        edges[edge["source"]].add(edge["target"])
    coverage = read("native_package_coverage.json")
    origins = defaultdict(set)
    for root in coverage["roots"]:
        for dependency in root["dependencies"]:
            if dependency["status"] in ("external_archive", "unresolved"):
                origins[dependency["guid"]].add(root["package"])
    pending = list(origins)
    included, covered, unresolved = {}, {}, {}
    visited = set()
    while pending:
        guid = pending.pop()
        if guid in visited:
            continue
        visited.add(guid)
        row = resources.get(guid)
        if not row or row["status"] != "header_matched" or not row["candidates"]:
            unresolved[guid] = "missing_full_guid_match"
            continue
        supplied = sorted({r["archive"] for r in row["candidates"]} & stock)
        if supplied:
            covered[guid] = supplied
            continue
        types = {r["type"] for r in row["candidates"]}
        if len(types) != 1:
            raise ValueError(f"Conflicting resource classes: {guid}")
        included[guid] = row
        pending.extend(edges[guid])
    report = {
        "runtime_ready": False,
        "scope": "Typed references only; opaque VFX dependencies are not proven complete",
        "resources": [included[g] for g in sorted(included)],
        "stock_covered": covered,
        "unresolved": unresolved,
        "root_packages": {g: sorted(v) for g, v in origins.items()},
        "type_counts": dict(Counter(r["candidates"][0]["type"] for r in included.values())),
    }
    (ROOT / "resource_supplement_plan.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Supplement: {len(included)} resources; stock covers {len(covered)}; unresolved {len(unresolved)}")
    print(report["type_counts"])


if __name__ == "__main__":
    main()
