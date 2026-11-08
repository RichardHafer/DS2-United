"""Compare resident template prerequisites; does not certify gameplay synchronization."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re

from ds2_assets import sdbm

REQUIRED_CODE = ("EntityNetCreate", "SentientNetCreate", "DeadSpaceNPCNetCreate",
                 "FindSpawnInfo", "SpawnInfoFromInstance", "CreateEntityFromSpawnInfo")


def word(value):
    return type(value) is int and 0 <= value <= 0xFFFFFFFF


def validate(snapshot):
    errors = []
    index = defaultdict(list)
    if snapshot.get("errors") != []:
        errors.append("incomplete_traversal")
    templates = snapshot.get("templates")
    if not isinstance(templates, list) or not templates:
        return index, errors + ["empty_or_missing_templates"]
    for ordinal, item in enumerate(templates):
        guid = item.get("guid")
        if not isinstance(guid, list) or len(guid) != 4 or not all(word(w) for w in guid):
            errors.append(f"invalid_guid:{ordinal}")
            continue
        calculated = 0
        for w in guid:
            calculated = (calculated * 33 + w) & 0xFFFFFFFF
        if not word(item.get("hash")) or calculated != item["hash"]:
            errors.append(f"invalid_hash:{ordinal}")
            continue
        name = item.get("name")
        if not isinstance(name, str) or not name or not word(item.get("class_id")) or sdbm(name) != item["class_id"]:
            errors.append(f"invalid_class:{ordinal}")
            continue
        if not word(item.get("flags")):
            errors.append(f"invalid_flags:{ordinal}")
            continue
        index[calculated].append(item)
    for key, entries in index.items():
        if len(entries) != 1:
            errors.append(f"ambiguous_hash:{key:08X}")
    return index, errors


def identity(item):
    return dict(hash=item["hash"], guid=item["guid"], class_id=item["class_id"],
                name=item["name"], flags=item["flags"])


def fingerprint(items):
    canonical = sorted((x["hash"], *x["guid"], x["class_id"], x["flags"]) for x in items)
    return hashlib.sha256(json.dumps(canonical, separators=(",", ":")).encode()).hexdigest()


def compare(host, client, classes=()):
    host_index, host_errors = validate(host)
    client_index, client_errors = validate(client)
    code_errors = []
    for name in REQUIRED_CODE:
        a = host.get("functions", {}).get(name, {}).get("sha256")
        b = client.get("functions", {}).get(name, {}).get("sha256")
        if not isinstance(a, str) or not re.fullmatch("[0-9a-f]{64}", a) or a != b:
            code_errors.append(name)
    required = [x for rows in host_index.values() for x in rows if not classes or x["name"] in classes]
    selection_errors = [f"class_not_present:{name}" for name in sorted(set(classes) - {x["name"] for x in required})]
    if not required:
        selection_errors.append("no_required_templates")
    missing, conflicts, matches = [], [], 0
    for item in required:
        candidates = client_index.get(item["hash"], [])
        if not candidates:
            missing.append(identity(item))
        elif len(candidates) != 1:
            conflicts.append(dict(reason="ambiguous_client_hash", host=identity(item)))
        else:
            peer = candidates[0]
            changed = [k for k in ("guid", "class_id", "flags") if item[k] != peer[k]]
            if changed:
                conflicts.append(dict(reason="template_mismatch", fields=changed,
                                      host=identity(item), client=identity(peer)))
            else:
                matches += 1
    ready = not (host_errors or client_errors or code_errors or selection_errors or missing or conflicts)
    return dict(status="template_prerequisites_match" if ready else "template_prerequisites_not_met",
        host_source=host.get("source"), client_source=client.get("source"), selected_classes=sorted(classes),
        required_count=len(required), matched_count=matches, host_fingerprint=fingerprint(required),
        host_errors=host_errors, client_errors=client_errors, code_mismatches=code_errors,
        selection_errors=selection_errors, missing_on_client=missing, conflicts=conflicts,
        limitations=["Snapshot comparison is not a live replication gate.",
                     "Model, animation, component payload and resource residency are not compared.",
                     "Authority, world events, combat and checkpoint synchronization remain unverified."])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", type=Path)
    parser.add_argument("client", type=Path)
    parser.add_argument("--class", dest="classes", action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = compare(json.loads(args.host.read_text()), json.loads(args.client.read_text()), args.classes)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({k: result[k] for k in ("status", "required_count", "matched_count")}))
    return 0 if result["status"] == "template_prerequisites_match" else 2


if __name__ == "__main__":
    raise SystemExit(main())
