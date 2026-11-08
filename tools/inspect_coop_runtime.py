"""Read retail co-op lifecycle evidence without calling or patching game code."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from inspect_uniform_runtime import ROOT

FUNCTIONS = {
    "BasePlayerEntity": (0x8A7960, 7),
    "RegisterPlayer": (0x77BB60, 0x95),
    "UnregisterPlayer": (0x77BC00, 0x51),
    "EntityGetNetID": (0x7D1CD0, 0x32),
    "EntitySetSubKey": (0x7D1D10, 0x16),
    "NetObjSync": (0x829940, 0x232),
    "ShouldSpawn": (0x40DB20, 0x17C),
    "SpawnFilterDeletingDestructor": (0x40DAC0, 0x57),
}


def inspect(read):
    def u32(a):
        b = read(a, 4)
        return struct.unpack("<I", b)[0] if b and len(b) == 4 else 0

    def proxy(p):
        return dict(address=p, vtable=u32(p), flags=u32(p + 0x30),
                    reflectable=u32(p + 0x40), class_info=u32(p + 0x44),
                    net_id=[u32(p + 0x48), u32(p + 0x4C)], authority=u32(p + 0x50))

    result = dict(players=[], reflectables=[], proxies=[], spawn_registry=[], functions={})
    for name, (a, n) in FUNCTIONS.items():
        raw = read(a, n)
        result["functions"][name] = dict(va=a, size=n, head=raw[:16].hex() if raw else None,
                                          sha256=hashlib.sha256(raw).hexdigest() if raw else None)
    manager = u32(0x201C0D4)
    slots, capacity = u32(manager + 4), u32(manager + 8)
    result["player_manager"] = dict(address=manager, vtable=u32(manager), slots=slots,
                                    capacity=capacity, count=u32(manager + 12))
    if slots and 0 < capacity <= 8:
        for slot in range(capacity):
            bp = u32(slots + slot * 4)
            if not bp:
                continue
            entity = bp - 0x3C0
            result["players"].append(dict(slot=slot, base_player=bp, entity=entity,
                getter=u32(u32(bp) + 12), vtable=u32(entity), player_id=u32(entity + 0x420),
                subkey=u32(entity + 0x40), proxy=u32(entity + 0x68)))
    node = u32(0x205B744)
    seen = set()
    while node and node not in seen and len(seen) < 4096:
        seen.add(node)
        vt = u32(node)
        getter = u32(vt + 0x78)
        stub = read(getter, 6)
        class_info = u32(struct.unpack_from("<I", stub, 1)[0]) if stub and len(stub) == 6 and stub[0] == 0xA1 and stub[5] == 0xC3 else 0
        raw_name = read(class_info + 0x20, 80) if class_info else None
        name = raw_name.split(b"\0")[0].decode("ascii", "replace") if raw_name else None
        result["reflectables"].append(dict(address=node, vtable=vt, getter=getter,
            class_info=class_info, name=name, get_net_id=u32(vt + 4), set_subkey=u32(vt + 8),
            proxy=u32(node + 12)))
        node = u32(node + 4)
    result["reflectable_walk_end"] = node
    session = u32(0x20216DC)
    result["session"] = dict(address=session, counter=u32(session + 0x18) if session else None)
    if session:
        node = u32(session + 0x28)
        seen = set()
        while node and node not in seen and len(seen) < 4096:
            seen.add(node)
            result["proxies"].append(proxy(node))
            node = u32(node + 0x34)
    filt = u32(0x201C094)
    arr, count, cap = (u32(filt + off) for off in (0xC, 0x10, 0x14))
    result["spawn_filter"] = dict(address=filt, vtable=u32(filt), mode=u32(filt + 8),
        deleting_destructor=u32(u32(filt)), should_spawn_thunk=u32(u32(filt) + 4),
        array=arr, count=count, capacity=cap)
    if arr and 0 <= count <= cap <= 4096:
        for i in range(count):
            raw = read(arr + i * 8, 8)
            if not raw or len(raw) != 8:
                break
            result["spawn_registry"].append(dict(class_id=struct.unpack_from("<I", raw)[0],
                spawn_on_clients=raw[4], allow_spawn_flag=raw[5], raw=raw.hex()))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.dump:
        from analyze_minidump import Dump
        dump = Dump(args.dump)
        read, close, origin = dump.read, dump.close, str(args.dump)
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
        origin = f"live PID {pid}"
    try:
        result = inspect(read)
        result["source"] = origin
    finally:
        close()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(dict(source=origin, players=len(result["players"]),
        reflectables=len(result["reflectables"]), proxies=len(result["proxies"]),
        spawn_classes=len(result["spawn_registry"]))))


if __name__ == "__main__":
    main()
