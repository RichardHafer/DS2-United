"""Trace entity references for isolated donors without loading foreign levels."""
import hashlib
import json
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ZERO = "00000000-00000000-00000000-00000000"


def compact(entity):
    refs, parts = [], []
    for image in entity["images"]:
        fields = []
        for field in image.get("fields", []):
            value = field.get("guid_candidate", field.get("string_candidate", field["value"]))
            fields.append((field["field_id"], field["flags"], value))
            guid = field.get("guid_candidate")
            if guid and guid != ZERO:
                refs.append({"guid": guid, "component": image["class"], "field": field["name"]})
        parts.append((image["class"], sorted(fields)))
    signature = hashlib.sha256(json.dumps([entity["spawn_flags"], sorted(parts)]).encode()).hexdigest()
    return {"guid": entity["guid"], "class": entity["class"], "archive": entity["archive"],
            "asset": entity["asset"], "index": entity["index"], "signature": signature,
            "spawn_flags": entity["spawn_flags"], "references": refs}


def choose(matches, archive):
    local = [m for m in matches if m["archive"] == archive]
    candidates = local or matches
    if not candidates:
        return None, "resource_or_unresolved"
    if len({m["signature"] for m in candidates}) != 1:
        return None, "ambiguous_entity"
    return min(candidates, key=lambda m: (m["archive"], m["asset"], m["index"])), "entity"


def closure(root, index, limit=512):
    pending = [(root["guid"], root["source_archive"])]
    nodes, edges, seen = [], [], set()
    truncated = False
    while pending:
        guid, archive = pending.pop()
        node, status = choose(index.get(guid, []), archive)
        if node is None:
            edges.append({"source": None, "target": guid, "status": status})
            continue
        key = node["guid"], node["archive"], node["asset"], node["index"]
        if key in seen:
            continue
        if len(seen) >= limit:
            truncated = True
            break
        seen.add(key)
        nodes.append({k: v for k, v in node.items() if k != "references"})
        for ref in node["references"]:
            target, status = choose(index.get(ref["guid"], []), node["archive"])
            edge = {"source": guid, "source_archive": node["archive"],
                    "target": ref["guid"], "component": ref["component"],
                    "field": ref["field"], "status": status}
            if target:
                edge["target_archive"] = target["archive"]
                pending.append((target["guid"], target["archive"]))
            edges.append(edge)
    return {"package": root["package"], "runtime_ready": False, "truncated": truncated,
            "nodes": nodes, "edges": edges,
            "unresolved": sum(e["status"] != "entity" for e in edges)}


def main():
    index = defaultdict(list)
    with (ROOT / "analysis/catalog/entities.jsonl").open(encoding="utf-8") as stream:
        for line in stream:
            node = compact(json.loads(line))
            index[node["guid"]].append(node)
    source = ROOT / "analysis/isolated_pool/report.json"
    roots = json.loads(source.read_text(encoding="utf-8"))["entries"]
    reports = [closure(root, index) for root in roots]
    report = {"scope": "Entity reference closure only; resources and script references remain unresolved",
              "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(), "roots": reports}
    (ROOT / "analysis/pool_dependency_closure.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    for row in reports:
        classes = sorted({node["class"] for node in row["nodes"]})
        print(f"{row['package']}: nodes={len(row['nodes'])} unresolved={row['unresolved']} truncated={row['truncated']} classes={','.join(classes)}")


if __name__ == "__main__":
    main()
