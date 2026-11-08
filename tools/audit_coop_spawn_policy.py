"""Compare retail MP spawn rules with extracted campaign entities. Read-only."""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct

from inspect_coop_runtime import inspect
from analyze_minidump import Dump
from ds2_assets import sdbm


def class_descriptors(dump):
    result = {}
    # This vtable occurs on the retail Player/Entity/Zombie ClassDesc objects.
    signature = struct.pack("<I", 0x1B1E648)
    for start, end, _ in dump.ranges:
        lo, hi = max(start, 0x400000), min(end, 0x2200000)
        if hi <= lo:
            continue
        data = dump.read(lo, hi - lo)
        at = data.find(signature)
        while at >= 0:
            address = lo + at
            raw = dump.read(address, 76)
            if raw and len(raw) == 76:
                name_ptr, class_id = struct.unpack_from("<II", raw, 4)
                name_raw = dump.read(name_ptr, 160) if 0x400000 <= name_ptr < 0x2200000 else None
                if name_raw:
                    name_raw = name_raw.split(b"\0")[0]
                    if name_raw and all(32 <= c < 127 for c in name_raw):
                        name = name_raw.decode("ascii")
                        if sdbm(name) == class_id:
                            result[class_id] = dict(name=name, class_id=class_id, address=address,
                                factory=struct.unpack_from("<I", raw, 24)[0], raw=raw.hex())
            at = data.find(signature, at + 4)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", type=Path, required=True)
    parser.add_argument("--entities", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    dump = Dump(args.dump)
    try:
        classes = class_descriptors(dump)
        runtime = inspect(dump.read)
    finally:
        dump.close()
    registry = {e["class_id"]: e for e in runtime["spawn_registry"]}
    counts, archives = Counter(), {}
    unresolved = 0
    with args.entities.open(encoding="utf-8") as source:
        for line in source:
            entity = json.loads(line)
            images = entity.get("images", [])
            class_id = images[0].get("type_id") if images else None
            if class_id is None:
                unresolved += 1
                continue
            counts[class_id] += 1
            archives.setdefault(class_id, Counter())[entity["archive"]] += 1
    rows = []
    for class_id, count in counts.most_common():
        rule = registry.get(class_id)
        rows.append(dict(class_id=class_id, name=classes.get(class_id, {}).get("name"),
            descriptor=classes.get(class_id, {}).get("address"), entity_count=count,
            mp_registered=rule is not None,
            spawn_on_clients=bool(rule["spawn_on_clients"]) if rule else None,
            allow_spawn_flag=bool(rule["allow_spawn_flag"]) if rule else None,
            archives=dict(archives[class_id])))
    output = dict(source_dump=str(args.dump), source_entities=str(args.entities),
        classes=list(classes.values()), registry=[dict(e, name=classes.get(i, {}).get("name")) for i, e in registry.items()],
        campaign=rows, unresolved_entities=unresolved,
        total_entities=sum(counts.values()),
        unregistered_entities=sum(r["entity_count"] for r in rows if not r["mp_registered"]),
        note="Counts describe extracted records, not simultaneous live objects. Stream residency and the special spawn flag are not evaluated.")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2), encoding="utf-8")
    print(json.dumps({k: output[k] for k in ("total_entities", "unregistered_entities", "unresolved_entities")}))
    print(f"Retail classes: {len(classes)}; named MP rules: {sum(e['name'] is not None for e in output['registry'])}/{len(registry)}")


if __name__ == "__main__":
    main()
