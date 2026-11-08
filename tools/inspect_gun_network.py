"""Record retail Gun network callbacks and inventory weapon ownership offline."""
import argparse
import hashlib
import json
from pathlib import Path

import capstone
from inspect_inventory_persistence import Dump

RANGES = {
    "item_name_hash_assignment": (0xDCF047, 0x38),
    "case_insensitive_name_hash": (0x7423C0, 0x30),
    "player_base_destructor_chain_1": (0xE93DB0, 0xD0),
    "player_base_destructor_chain_2": (0x754580, 0x1CB),
    "player_base_destructor_chain_3": (0x7D4910, 0x11F),
    "entity_destructor_invalidates_safe_pointers": (0x7CF870, 0xC0),
    "gun_proxy_constructor": (0xDDD770, 0x5A),
    "gun_proxy_scalar_destructor": (0xDE1940, 0x48),
    "gun_proxy_handle_event": (0xDEF140, 0x23),
    "gun_handle_event": (0xDEF050, 0xE1),
    "entity_notify_and_destroy": (0x7567B0, 0x6B),
    "player_scalar_destructor": (0x8AB5C0, 0x23),
    "player_destructor": (0x8A7AB0, 0x74C),
    "inventory_destructor": (0xDD7060, 0x17),
    "inventory_clear_items": (0xDCDB20, 0x50),
    "entity_request_delete": (0x7568B0, 0x41),
    "player_deactivate": (0x7CF040, 0x15),
    "weapon_owner_cleanup": (0xDD7660, 0xB9),
    "quartermaster_cancel_requisition": (0xDD6C40, 0x79),
    "quartermaster_release_gun": (0xDBED80, 0x83),
    "quartermaster_stream_event": (0xDDD490, 0x15C),
    "quartermaster_handle_unload": (0xDD6E60, 0xC8),
    "quartermaster_check_waiting": (0xDCD260, 0xB4),
    "gun_proxy_cancel_owned_request": (0xDD6F30, 0x6A),
    "gun_proxy_owner_cleanup_candidate": (0xDDD860, 0x6E),
    "quartermaster_remove_request": (0xDD16C0, 0x75),
    "gun_proxy_pickup_candidate": (0xDF1320, 0xE2),
    "item_pickup_wrapper": (0xDEF170, 0x99),
    "item_inventory_add": (0xDEC9D0, 0x2D1),
    "item_pickup_owner_transition": (0xDEA560, 0xE8),
    "quartermaster_requisition_gun": (0xDDD5F0, 0x176),
    "quartermaster_issue_gun": (0xDD6CC0, 0x199),
    "quartermaster_find_proxy_guid": (0xDB28D0, 0x57),
    "player_equip_weapon": (0x86BC80, 0x1DC),
    "ghost_state_message_handler": (0x8A0C00, 0x8D),
    "ghost_flashlight_update_candidate": (0x893560, 0x8C),
    "ghost_inventory_visual_update_candidate": (0x87D5A0, 0xA5),
    "player_update_ghost_gun_state": (0x86BAD0, 0xD8),
    "player_unpack_gun_state": (0x87BE0A, 0x6A),
    "player_pack_gun_values": (0x87B756, 0x90),
    "inventory_find_weapon_hash": (0xDBF300, 0x49),
    "item_owner_pack": (0xDB40F0, 0xF3),
    "item_owner_unpack": (0xDC0A20, 0x1E0),
    "weapon_equipped_unpack": (0xDCDF10, 0xD8),
    "gun_owner_setter": (0xDC8960, 0x32),
    "item_owner_setter": (0xDC09B0, 0x68),
    "net_reference_resolve": (0x827310, 0x22),
    "player_ghost_gun_reference_setter": (0x869EB0, 0x3B),
    "stream_handle_lookup": (0x40DA50, 0x3E),
    "gun_class_getter": (0xDA7030, 6),
    "gun_net_create": (0xDD46A0, 0x1B),
    "gun_manager_network_creation": (0xDCD000, 0x128),
    "gun_on_ghost_add": (0xDB0F90, 0x43),
    "gun_pack_update": (0xDC89A0, 0x52),
    "gun_unpack_update": (0xDD4A20, 0x68),
    "gun_parent_pack_update": (0xDBFEB0, 0xCA),
    "gun_stream_query": (0xDCF210, 0x7E),
}


def safe_pointer_chain(dump, entity):
    node = dump.u32(entity + 0x14)
    nodes = []
    seen = set()
    while node:
        if node in seen or len(nodes) >= 65536:
            return {"status": "cycle_or_limit", "nodes": nodes, "stopped_at": node}
        seen.add(node)
        raw = dump.read(node, 8)
        if raw is None or len(raw) != 8:
            return {"status": "unreadable", "nodes": nodes, "stopped_at": node}
        target = int.from_bytes(raw[:4], "little")
        if target != entity + 0x10:
            return {"status": "wrong_target", "nodes": nodes,
                    "stopped_at": node, "target": target}
        nodes.append(node)
        node = int.from_bytes(raw[4:], "little")
    return {"status": "valid", "nodes": nodes}


def stream_status(dump, handle):
    if not handle:
        return {"status": "no_handle"}
    manager = dump.u32(0x201C098)
    buckets, capacity = dump.u32(manager + 0x5C), dump.u32(manager + 0x60)
    if not buckets or not 0 < capacity <= 65536:
        return {"status": "invalid_table"}
    node = dump.u32(buckets + 4 * (handle % capacity))
    seen = set()
    while node:
        if node in seen or len(seen) >= 4096:
            return {"status": "invalid_chain"}
        seen.add(node)
        if dump.u32(node) == handle:
            stream = dump.u32(node + 4)
            return {"status": "found" if stream else "null_stream", "stream": stream,
                    "state": dump.u32(stream + 0x24) if stream else None}
        node = dump.u32(node + 8)
    return {"status": "not_found"}


def inspect(dump):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    info = dump.u32(0x1F60798)
    manager = dump.u32(0x201C0D0)
    result = {"dump": str(dump.path), "class_info": info,
              "class_name": dump.read(info + 0x20, 32).split(b"\0")[0].decode(),
              "net_create": dump.u32(info + 0x74), "gun_manager": manager,
              "stream_records": dump.u32(manager + 0x14),
              "stream_record_count": dump.u32(manager + 0x18),
              "manager_flags": dump.read(manager + 0x10, 1)[0],
              "stream_budget": dump.read(manager + 0x11, 1)[0],
              "loaded_stream_count": dump.read(manager + 0x12, 1)[0],
              "request_buffer": dump.u32(manager + 0x20),
              "request_count": dump.u32(manager + 0x24),
              "request_capacity": dump.u32(manager + 0x28),
              "players": [], "functions": {}}
    result["weapon_vtables"] = {
        f"{vt:08X}": {f"{offset:X}": dump.u32(vt + offset)
                        for offset in (0, 4, 8, 0x1F0, 0x1F8, 0x1FC)}
        for vt in (0x1B51E20, 0x1B51A00, 0x1B515D8)}
    result["delete_event_names"] = {
        f"{address:08X}": dump.read(address, 32).split(b"\0")[0].decode("ascii")
        for address in (0x1B19584, 0x1B19598, 0x1B195AC)}
    if not 0 <= result["stream_record_count"] <= 4096:
        raise ValueError("Invalid weapon stream record count")
    records = []
    for i in range(result["stream_record_count"]):
        address = result["stream_records"] + i * 0x3C
        handle = dump.u32(address + 0x34)
        records.append({"index": i, "address": address,
                        "gun_proxy_guid": dump.read(address, 16).hex(),
                        "template_guid": dump.read(address + 0x24, 16).hex(),
                        "reference_count": dump.read(address + 0x39, 1)[0],
                        "handle": handle, "stream": stream_status(dump, handle)})
    result["stream_record_details"] = records
    if not 0 <= result["request_count"] <= result["request_capacity"] <= 65536:
        raise ValueError("Invalid Quartermaster request count")
    requests = []
    for i in range(result["request_count"]):
        address = result["request_buffer"] + i * 0x10
        owner_ref, index = dump.u32(address), dump.u32(address + 8)
        if index >= len(records):
            raise ValueError("Invalid Quartermaster request record index")
        requests.append({"index": i, "address": address, "owner_ref": owner_ref,
                         "player": owner_ref - 0x10 if owner_ref else 0,
                         "record_index": index,
                         "force_active": dump.read(address + 0xC, 1)[0]})
    result["requests"] = requests
    pm = dump.u32(0x201C0D4)
    slots, capacity = dump.u32(pm + 4), dump.u32(pm + 8)
    if not 0 < capacity <= 8:
        raise ValueError("Invalid player capacity")
    for slot in range(capacity):
        bp = dump.u32(slots + slot * 4)
        if not bp:
            continue
        p = bp - 0x3C0
        if dump.u32(p) != 0x1B294D0:
            raise ValueError("Unexpected Player vtable")
        buffer, count = dump.u32(p + 0xC08), dump.u32(p + 0xC0C)
        if count > 256:
            raise ValueError("Invalid weapon inventory count")
        player = {"player": p, "slot": slot, "selected_ref": dump.u32(p + 0xCEC),
                  "pending_gun_ref": dump.u32(p + 0x424),
                  "weapons": []}
        chain = safe_pointer_chain(dump, p)
        player["safe_pointer_chain"] = chain
        player["queued_request_links"] = [
            {"request_index": request["index"],
             "linked": request["address"] in chain["nodes"]}
            for request in requests if request["player"] == p]
        for i in range(count):
            ref = dump.u32(buffer + i * 12 + 4)
            if not ref:
                continue
            gun = ref - 0x10
            rvt, proxy = dump.u32(gun + 0x5C), dump.u32(gun + 0x68)
            guid = dump.read(gun + 0x24, 16).hex()
            player["weapons"].append({
                "slot": i, "entity": gun, "vtable": dump.u32(gun),
                "reflectable_vtable": rvt, "proxy": proxy,
                "owner_ref": dump.u32(gun + 0x240),
                "owner_link_in_player_chain": gun + 0x240 in chain["nodes"],
                "weapon_hash": dump.u32(gun + 0x25C),
                "weapon_name": dump.read(dump.u32(gun + 0x24C), 64).split(b"\0")[0].decode("ascii"),
                "gun_proxy_guid": dump.read(gun + 0x404, 16).hex(),
                "ammo_field": dump.u32(gun + 0x39C),
                "template_guid": guid,
                "matching_stream_records": [r["index"] for r in records
                                            if r["template_guid"] == guid],
                "class_getter": dump.u32(rvt + 0x78),
                "pack": dump.u32(rvt + 0x20), "unpack": dump.u32(rvt + 0x24),
            })
        result["players"].append(player)
    for name, (va, size) in RANGES.items():
        raw = dump.read(va, size)
        if raw is None or len(raw) != size:
            raise ValueError(f"Missing {name}")
        instructions = list(md.disasm(raw, va))
        if sum(i.size for i in instructions) != size:
            raise ValueError(f"Incomplete {name}")
        result["functions"][name] = {
            "va": va, "bytes": raw.hex(), "sha256": hashlib.sha256(raw).hexdigest(),
            "disassembly": [f"{i.address:08X} {i.mnemonic} {i.op_str}" for i in instructions]}
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    dump = Dump(args.dump)
    try:
        args.output.write_text(json.dumps(inspect(dump), indent=2) + "\n", encoding="utf-8")
    finally:
        dump.close()
