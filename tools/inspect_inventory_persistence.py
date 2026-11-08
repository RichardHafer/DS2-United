"""Extract retail inventory persistence evidence from an existing crash dump."""
import argparse
import hashlib
import json
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "trainer"))
from analyze_minidump import Dump

RANGES = {
    "inventory_constructor": (0xDCDA50, 0x3F),
    "inventory_initialize": (0xDDDB90, 0x49),
    "player_inventory_initialization": (0x8A7360, 0x6A),
    "drop_items_on_death_candidate": (0xDBF7F0, 0x53),
    "death_inventory_dispatch": (0x8A12D0, 0x2C),
    "death_inventory_branch": (0x8A169D, 0x38),
    "inventory_store_candidate": (0xDBF850, 0x265),
    "inventory_restore_candidate": (0xDE9480, 0x29B),
    "player_store_candidate": (0x87C7F0, 0x2E8),
    "player_restore_candidate": (0x87CAE0, 0x364),
    "player_restore_init_call": (0x8A862C, 0x4B),
    "player_init_before_restore": (0x8A8270, 0x3DF),
    "player_store_event_call": (0x8ABB35, 0x19),
    "persistence_lookup": (0x7B6350, 0x6A),
    "event_subscription_lookup": (0x756260, 0x96),
    "store_event_constructor_subscription": (0x8A7402, 0x1B),
    "event_subscribe": (0x756910, 0x45),
    "event_dispatch_eligibility": (0x756CD0, 0x29),
}


def subscription(dump, player, event_address):
    event_id = dump.u32(event_address)
    flags = dump.u32(player + 4)
    table = dump.u32(player + 8)
    mask = (255 if flags & 8 else 31) if flags & 4 else 3
    result = {"event_address": event_address, "event_id": event_id,
              "entity_flags": flags, "table": table, "mask": mask}
    if not table:
        return dict(result, status="absent_table")
    node = dump.u32(table + (event_id & mask) * 4)
    seen = set()
    while node:
        if node in seen or len(seen) >= 256:
            return dict(result, status="invalid_chain")
        seen.add(node)
        raw = dump.read(node, 24)
        if not raw:
            return dict(result, status="unreadable_node")
        if int.from_bytes(raw[16:20], "little") == event_id:
            return dict(result, status="subscribed", node=node,
                        owner=int.from_bytes(raw[12:16], "little"),
                        references_and_flags=int.from_bytes(raw[20:22], "little"),
                        priority=int.from_bytes(raw[22:24], "little"))
        node = int.from_bytes(raw[8:12], "little")
    return dict(result, status="absent")


def inspect(dump):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    result = {
        "dump": str(dump.path),
        "inventory_keys": [dump.u32(0x1B4B28C + 4 * i) for i in range(4)],
        "persistence_manager": dump.u32(0x2021300),
        "death_dispatch": {hex(0x17 + i): dump.u32(0x8A1750 + 4 * i)
                           for i in range(13)},
        "functions": {},
        "player_subscriptions": [],
    }
    manager = dump.u32(0x201C0D4)
    slots, capacity = dump.u32(manager + 4), dump.u32(manager + 8)
    if not 0 < capacity <= 8:
        raise ValueError("Invalid player slot capacity")
    for slot in range(capacity):
        base_player = dump.u32(slots + slot * 4)
        if not base_player:
            continue
        player = base_player - 0x3C0
        if dump.u32(player) != 0x1B294D0:
            raise ValueError("Unexpected Player vtable")
        proxy = dump.u32(player + 0x68)
        result["player_subscriptions"].append({
            "slot": slot, "player": player,
            "ghost": bool(proxy and dump.u32(proxy + 0x30) & 2),
            "events": {name: subscription(dump, player, address) for name, address in
                       [("store", 0x2054FF8), ("initialize", 0x204D5E4),
                        ("cleanup", 0x204CDBC), ("respawn_tick", 0x204D504)]},
        })
    for name, (va, size) in RANGES.items():
        raw = dump.read(va, size)
        if raw is None or len(raw) != size:
            raise ValueError(f"Missing range {name} at {va:08X}")
        instructions = list(md.disasm(raw, va))
        if sum(i.size for i in instructions) != size:
            raise ValueError(f"Incomplete disassembly for {name}")
        result["functions"][name] = {
            "va": va, "size": size, "bytes": raw.hex(),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "disassembly": [f"{i.address:08X} {i.mnemonic} {i.op_str}"
                            for i in instructions],
        }
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    dump = Dump(args.dump)
    try:
        args.output.write_text(json.dumps(inspect(dump), indent=2) + "\n",
                               encoding="utf-8")
    finally:
        dump.close()
