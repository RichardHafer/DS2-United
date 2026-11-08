"""Record retail death-state and MP respawn-selector evidence without mutation."""
import argparse
import hashlib
import json
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "trainer"))
from analyze_minidump import Dump

RANGES = {
    "player_on_proxy_deleted": (0x861B80, 0x37),
    "register_delete_listener": (0x82B5A0, 0x67),
    "remove_delete_listener": (0x82AA30, 0x7B),
    "dispatch_proxy_deleted": (0x82A9B0, 0x80),
    "process_pending_proxy_deletes": (0x82ADA0, 0x70),
    "queue_proxy_delete": (0x82AE10, 0x69),
    "proxy_delete_ready": (0x1412F60, 0x34),
    "mark_proxy_ghosts_for_delete": (0x1412FA0, 0x1B),
    "reflectable_remove_from_session": (0x7E1060, 0x19),
    "session_remove_reflectable": (0x82AE80, 0x76),
    "player_delete_listener_cleanup": (0x8AB684, 0x4D),
    "respawn_player": (0x891D70, 0x1B7),
    "pending_respawn_event_branch": (0x8ABFB3, 0xA1),
    "process_death_candidate": (0x878B40, 0x19B),
    "death_state_registration": (0x896EE1, 0x18),
    "death_state_factory": (0x893A90, 0x48),
    "death_state_constructor_head": (0x87ED80, 0x70),
    "death_state_id": (0x87EEE0, 6),
    "death_state_respawn_ready_branch": (0x87EFE8, 0x26),
    "selector_constructor": (0x433630, 0xA4),
    "selector_net_create": (0x433C30, 0x23),
    "selector_net_create_thunk": (0x406E06, 5),
    "selector_constructor_thunk": (0x4061C2, 5),
    "selector_class_getter": (0x4309D0, 6),
    "selector_ready_query": (0x433A70, 0x2E),
    "selector_ready_query_thunk": (0x4067BC, 5),
    "selector_creation_mode_gate": (0x42D5D0, 0xA0),
    "reset_initial_spawn_points": (0x431D80, 0x28),
}


def inspect(dump):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    selector = dump.u32(0x2021620)
    class_info = dump.u32(0x1C305B8)
    name = dump.read(class_info + 0x20, 64) if class_info else None
    result = {"dump": str(dump.path), "selector": selector,
              "registered_spawn_points": dump.u32(0x2021920),
              "selector_vtable": dump.u32(selector) if selector else None,
              "class_info": class_info,
              "class_name": name.split(b"\0")[0].decode("ascii") if name else None,
              "net_create_callback": dump.u32(class_info + 0x74) if class_info else None,
              "death_vtable": [dump.u32(0x1B277C0 + i * 4) for i in range(5)],
              "player_on_proxy_deleted_slot": dump.u32(0x1B293F0 + 0x38),
              "functions": {}}
    for name, (va, size) in RANGES.items():
        raw = dump.read(va, size)
        result["functions"][name] = {
            "va": va, "size": size,
            "sha256": hashlib.sha256(raw).hexdigest() if raw else None,
            "bytes": raw.hex() if raw else None,
            "disassembly": [f"{i.address:08X} {i.mnemonic} {i.op_str}"
                            for i in md.disasm(raw or b"", va)],
        }
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    dump = Dump(args.dump)
    try:
        args.output.write_text(json.dumps(inspect(dump), indent=2) + "\n")
    finally:
        dump.close()
