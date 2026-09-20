"""Extract dependency-aware research groups without installing them."""
import argparse
import hashlib
import json
import sys
from collections import defaultdict
from pathlib import Path
from build_isolated_pool import isolate_many
from ds2_assets import assets

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--tool-dir", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, args.tool_dir)
    import ds2str
    graph_path = ROOT / "analysis/pool_dependency_closure.json"
    graph = json.loads(graph_path.read_text(encoding="utf-8"))
    groups = defaultdict(dict)
    consumers = defaultdict(set)
    external = {}
    for root in graph["roots"]:
        if root["truncated"]:
            raise ValueError("Dependency graph was truncated")
        for node in root["nodes"]:
            key = node["archive"], node["asset"], node["index"]
            consumers[key].add(root["package"])
            if node["spawn_flags"]:
                external[key] = node
            else:
                groups[key[:2]][node["index"]] = node
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"runtime_ready": False, "graph_sha256": hashlib.sha256(graph_path.read_bytes()).hexdigest(),
              "groups": [], "external_entities": [], "errors": []}
    for key, node in sorted(external.items()):
        report["external_entities"].append({**node, "consumers": sorted(consumers[key]),
            "requirement": "Native registration and lifetime must be verified; authored flags preserved by omission"})
    by_archive = defaultdict(list)
    for (archive, asset), nodes in groups.items():
        by_archive[archive].append((asset, nodes))
    for archive, selected_assets in sorted(by_archive.items()):
        wanted = dict(selected_assets)
        found = set()
        try:
            stream = ds2str.StreamSet(args.work / archive)
            if not stream.pc:
                raise ValueError("Not a PC stream")
            for meta, chunks in assets(stream):
                if meta["type"] != "SimGroup" or meta["path"] not in wanted:
                    continue
                asset = meta["path"]
                if asset in found:
                    raise ValueError("Duplicate source asset")
                found.add(asset)
                nodes = wanted[asset]
                data = stream.asset_payload(chunks, meta["size"])
                payload, selected, unused = isolate_many(data, nodes)
                if any(row["guid"] != nodes[row["index"]]["guid"] for row in selected):
                    raise ValueError("Dependency graph does not match source GUIDs")
                filename = hashlib.sha256((archive + "|" + asset).encode()).hexdigest()[:24] + ".simgroup.bin"
                (args.output / filename).write_bytes(payload)
                report["groups"].append({"archive": archive, "asset": asset, "file": filename,
                    "sha256": hashlib.sha256(payload).hexdigest(), "source_sha256": hashlib.sha256(data).hexdigest(),
                    "entities": [nodes[i] for i in sorted(nodes)], "unused_images": unused,
                    "consumers": sorted(set().union(*(consumers[archive, asset, i] for i in nodes)))})
            if found != set(wanted):
                raise ValueError("Source assets missing")
        except (ValueError, OSError) as error:
            report["errors"].append({"archive": archive, "error": str(error)})
    (args.output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Groups: {len(report['groups'])}; entities: {sum(len(g['entities']) for g in report['groups'])}")
    print(f"External flagged entities: {len(external)}; errors: {len(report['errors'])}")
    print("Research payloads only. Identity remapping, resource closure and runtime registration remain required.")
    if report["errors"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
