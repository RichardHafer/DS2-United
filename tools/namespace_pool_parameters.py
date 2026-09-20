"""Assign private parameter IDs to isolated research groups."""
import argparse
import hashlib
import json
from pathlib import Path
from ds2_assets import simgroup
from inventory_parameter_ids import image_ids
from remap_parameter_images import remap_images


def allocate(groups, reserved):
    used = set(reserved) | {0, 0xFFFFFFFF}
    used.update(value for ids in groups.values() for value in ids)
    result = {}
    for group, ids in sorted(groups.items()):
        mapping = {}
        for old in sorted(ids):
            for nonce in range(100000):
                key = f"DS2Randomizer:parameters:v1:{group}:{old:08X}:{nonce}".encode()
                value = int.from_bytes(hashlib.sha256(key).digest()[:4], "little")
                if value not in used:
                    break
            else:
                raise ValueError("Parameter ID allocation exhausted")
            used.add(value)
            mapping[old] = value
        result[group] = mapping
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pool", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    inventory_bytes = args.inventory.read_bytes()
    inventory = json.loads(inventory_bytes)
    if not inventory.get("catalog_complete") or inventory.get("errors"):
        raise ValueError("Complete parameter inventory required")
    report_bytes = (args.pool / "report.json").read_bytes()
    report = json.loads(report_bytes)
    if report.get("errors"):
        raise ValueError("Dependency extraction has errors")
    payloads, ids = {}, {}
    for group in report["groups"]:
        name = group["file"]
        if Path(name).name != name or name in payloads:
            raise ValueError("Invalid or duplicate group filename")
        data = (args.pool / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != group["sha256"]:
            raise ValueError("Dependency payload hash mismatch")
        records, base = simgroup(data)
        payloads[name] = data
        ids[name] = image_ids(data, base)
    mappings = allocate(ids, inventory["image_ids"])
    outputs, audits = {}, []
    for name, data in payloads.items():
        output, audit = remap_images(data, mappings[name])
        outputs[name] = output
        audits.append({"file": name, "source_sha256": hashlib.sha256(data).hexdigest(),
            "sha256": hashlib.sha256(output).hexdigest(), "images": audit["images"],
            "references": audit["references"], "mapping": mappings[name]})
    # Do not leave a partially validated set on disk.
    args.output.mkdir(parents=True, exist_ok=True)
    if any(args.output.iterdir()):
        raise ValueError("Output directory must be empty")
    for name, data in outputs.items():
        (args.output / name).write_bytes(data)
    manifest = {"runtime_ready": False, "scope": "Parameter namespace only; entity and resource identities unchanged",
        "inventory_sha256": hashlib.sha256(inventory_bytes).hexdigest(),
        "pool_report_sha256": hashlib.sha256(report_bytes).hexdigest(), "groups": audits}
    (args.output / "report.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"Namespaced {sum(a['images'] for a in audits)} images in {len(audits)} groups")


if __name__ == "__main__":
    main()
