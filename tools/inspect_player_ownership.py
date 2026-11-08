"""Record retail player slots and ownership gates from a process dump."""
import argparse
import hashlib
import json
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "trainer"))
from analyze_minidump import Dump


FUNCTIONS = {
    "local_authority_predicate": (0x7E1130, 0x1D),
    "ghost_predicate": (0x7E1150, 0x16),
    "ghost_constructor_context": (0x7E17B1, 0x2C),
    "player_slot_registration": (0x86C1C0, 0x85),
    "controller_getter_base_player": (0x8A7950, 4),
    "controller_feedback_ghost_gate": (0x86A030, 0x4B),
    "camera_modifier_ghost_gate": (0x879950, 0xF8),
    "camera_creation_candidate": (0x8792F0, 0x400),
    "inventory_getter": (0x85F910, 0x11),
    "inventory_const_getter": (0x85F930, 0x11),
    "inventory_ui_init": (0x89E660, 0x48),
    "inventory_ui_owner_assignment": (0x8A83CB, 0x2D),
    "controller_config_authority_gate": (0x8694E0, 0x67),
    "player_state_machine_selection": (0x869870, 0xD2),
    "ghost_state_machine_registration": (0x8A0BCE, 0x18),
    "scope_native_local_flag_gate": (0x14160BE, 0x2A),
    "skin_native_owner_pose_binding": (0x1091AD6, 0x1E),
}


def inspect(dump):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    result = {"dump": str(dump.path), "players": [], "functions": {}}
    manager = dump.u32(0x201C0D4)
    slots = dump.u32(manager + 4) if manager else 0
    capacity = dump.u32(manager + 8) if manager else 0
    if slots and capacity and capacity <= 8:
        for slot in range(capacity):
            bp = dump.u32(slots + slot * 4)
            if not bp:
                continue
            player = bp - 0x3C0
            proxy = dump.u32(player + 0x68)
            flags = dump.u32(proxy + 0x30) if proxy else None
            ui_ref = dump.u32(player + 0x1660)
            ui = ui_ref - 0x10 if ui_ref else 0
            sm = dump.u32(player + 0x24C)
            result["players"].append({
                "slot": slot, "player": player,
                "primary_vtable": dump.u32(player),
                "player_id": dump.u32(player + 0x420),
                "controller_id": dump.u32(player + 0x41C),
                "proxy": proxy, "proxy_flags": flags,
                "ghost": bool(flags & 2) if flags is not None else None,
                "camera_safe_refs": {hex(off): dump.u32(player + off)
                                     for off in (0x548, 0x550, 0x558, 0x560)},
                "inventory_ui": {"entity": ui, "vtable": dump.u32(ui) if ui else None,
                                 "owner": dump.u32(ui + 0x184) if ui else None},
                "state_machine": {"root": sm, "vtable": dump.u32(sm) if sm else None,
                                  "owner": dump.u32(sm + 0x50) if sm else None},
                "inventories": [
                    {"address": player + 0xBD0 + i * 0x34,
                     **{name: dump.u32(player + 0xBD0 + i * 0x34 + off)
                        for name, off in [("vtable", 0), ("entries", 4), ("count", 8),
                                          ("allocation", 12), ("capacity", 16),
                                          ("currency", 0x28), ("type", 0x2C)]}}
                    for i in range(4)],
            })
    for name, (va, size) in FUNCTIONS.items():
        raw = dump.read(va, size)
        result["functions"][name] = {
            "va": va, "size": size,
            "sha256": hashlib.sha256(raw).hexdigest() if raw else None,
            "head": raw[:16].hex() if raw else None,
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
