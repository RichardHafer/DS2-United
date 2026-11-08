"""Inventory resident spawn templates required by retail Entity ghost creation."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct

from inspect_uniform_runtime import ROOT
from analyze_minidump import Dump

FUNCTIONS = {
    "EntityNetCreate": (0x7D1F00, 0x58),
    "SentientNetCreate": (0xE94490, 0x20),
    "DeadSpaceNPCNetCreate": (0xFE4020, 0x64),
    "FindSpawnInfo": (0x7763F0, 0x47),
    "SpawnInfoFromInstance": (0x84A530, 0x47),
    "CreateEntityFromSpawnInfo": (0x7CF3B0, 0xB9),
}


def inspect(read):
    def u32(address):
        raw = read(address, 4)
        return struct.unpack("<I", raw)[0] if raw and len(raw) == 4 else 0

    manager = u32(0x201C0A0)
    result = dict(manager=manager, templates=[], errors=[], functions={})
    for name, (address, size) in FUNCTIONS.items():
        raw = read(address, size)
        result["functions"][name] = dict(address=address, size=size,
            head=raw[:16].hex() if raw else None,
            sha256=hashlib.sha256(raw).hexdigest() if raw else None)
    buckets = read(manager + 0x70, 4096 * 4) if manager else None
    if not buckets or len(buckets) != 4096 * 4:
        result["errors"].append("spawn_table_unavailable")
        return result
    visited = set()
    for bucket, (node,) in enumerate(struct.iter_unpack("<I", buckets)):
        chain = set()
        while node:
            if node in chain or node in visited:
                result["errors"].append(dict(reason="repeated_node", bucket=bucket, node=node))
                break
            if len(visited) >= 100000:
                result["errors"].append("node_limit")
                return result
            chain.add(node)
            visited.add(node)
            raw = read(node, 0x38)
            if not raw or len(raw) != 0x38:
                result["errors"].append(dict(reason="unreadable_node", node=node))
                break
            words = struct.unpack("<14I", raw)
            guid = list(words[3:7])
            hash_value = 0
            for word in guid:
                hash_value = (hash_value * 33 + word) & 0xFFFFFFFF
            packet = u32(words[2]) if words[2] else 0
            descriptor = u32(packet) if packet else 0
            name_ptr = u32(descriptor + 4) if descriptor else 0
            name_raw = read(name_ptr, 120) if name_ptr else None
            name = name_raw.split(b"\0")[0].decode("ascii", "replace") if name_raw else None
            valid = hash_value == words[7] and (words[7] & 4095) == bucket
            result["templates"].append(dict(node=node, bucket=bucket, guid=guid,
                hash=words[7], hash_valid=valid, descriptor=descriptor,
                class_id=u32(descriptor + 8) if descriptor else 0, name=name,
                component_list=words[8], resource_list=words[9], lifetime=words[10],
                flags=words[11], transform=words[12]))
            node = words[13]
    result["class_counts"] = dict(Counter(t["name"] for t in result["templates"]))
    result["invalid_hashes"] = sum(not t["hash_valid"] for t in result["templates"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.dump:
        dump = Dump(args.dump)
        read, close, source = dump.read, dump.close, str(args.dump)
    else:
        from memscan import find_pid, read_mem, k32
        pid = find_pid("deadspace2")
        if not pid:
            parser.error("deadspace2 is not running; no process was started")
        handle = k32.OpenProcess(0x410, False, pid)
        if not handle:
            raise OSError("OpenProcess failed")
        read = lambda a, n: read_mem(handle, a, n)
        close = lambda: k32.CloseHandle(handle)
        source = f"live PID {pid}"
    try:
        result = inspect(read)
        result["source"] = source
    finally:
        close()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(dict(source=source, templates=len(result["templates"]),
        invalid_hashes=result.get("invalid_hashes"), errors=result["errors"][:5])))


if __name__ == "__main__":
    main()
