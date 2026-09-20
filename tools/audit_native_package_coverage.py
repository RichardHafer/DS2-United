"""Check which direct template dependencies already exist in stock character streams."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "analysis"


def classify(guid, primary, resources, models, verified_archives):
    assets = resources.get(guid, []) + models.get(guid, [])
    archives = sorted({a["archive"] for a in assets})
    if primary in archives:
        status = "primary_character_package"
    elif any(a in verified_archives for a in archives):
        status = "other_character_package"
    elif archives:
        status = "external_archive"
    else:
        status = "unresolved"
    return {"guid": guid, "status": status, "candidate_archives": archives,
            "verified_character_candidates": [verified_archives[a] for a in archives
                                               if a in verified_archives]}


def main():
    def read(name):
        return json.loads((ROOT / name).read_text(encoding="utf-8"))
    streams = read("native_character_streams.json")["packages"]
    packages = {p["package"]: p for p in streams}
    verified = {p["archive"]: p["toc_name"] for p in streams
                if p["toc_name"] and p["archive_exists"]}
    resources = {r["guid"]: r["candidates"]
                 for r in read("pool_resource_matches.json")["resources"]}
    models = read("model_stream_matches.json")["matches"]
    result = []
    for root in read("pool_dependency_closure.json")["roots"]:
        primary = packages[root["package"]]["archive"]
        guids = sorted({e["target"] for e in root["edges"]
                        if e["status"] == "resource_or_unresolved"})
        dependencies = [classify(g, primary, resources, models, verified) for g in guids]
        counts = {status: sum(d["status"] == status for d in dependencies)
                  for status in ("primary_character_package", "other_character_package",
                                 "external_archive", "unresolved")}
        result.append({"package": root["package"], "primary_toc_name": verified.get(primary),
                       "dependencies": dependencies, "counts": counts, "runtime_ready": False})
        print(root["package"], counts)
    report = {"scope": "Direct typed template resource references only; not transitive readiness",
              "roots": result}
    (ROOT / "native_package_coverage.json").write_text(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
