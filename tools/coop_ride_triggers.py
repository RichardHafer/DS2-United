"""Find the campaign's ride triggers: a box the player steps into that starts a ride.

12.10., the drill (ch09_ch10_trans): the small lift on the drill machine rises as
soon as one player stands in TriggerPhysical 34CAE8A9 (mm_drill_machine_elevator_up).
Only a MultiManager hears that message, so the gate never held it (it holds what
Doors, Movers and MoverControllers hear) and the relayed MultiManager sent the
partner's lift up empty. The lift itself is an animation of the drill prop.

A ride trigger here is a TriggerPhysical broadcast (or direct message) whose
listeners, followed through MultiManagers for up to three steps, reach
  - a MoverController's MSG_CALL whose Mover is a lift, elevator or machine, or
  - a message named like a ride (elevator, lift, tram, gondola, ride, platform),
and is not already a gate class's message itself. The gate holds these until
every player's own machine fires them, i.e. everybody stands in the box.

    python tools/coop_ride_triggers.py [--write]

--write regenerates src/generated_ride_triggers.h (hash, level, name).

It also lists the lift managers: MultiManagers whose targets reach a lift's or
elevator's MoverController MSG_CALL within three steps through MultiManagers,
VariableSwitches and event gates. 13.10., ch02's public sector elevator: its
cabin manager reaches the ride through a VariableSwitch, so the runtime's own
walk (MultiManagers only) never held it. Their GUIDs go to the same header,
with the cabins' MoverControllers: only those hold a ride, a machine or a
platform never waits for a partner.
"""
import argparse
import collections
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import coop_message_graph as g  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "analysis" / "catalog" / "entities.jsonl"
HEADER = ROOT / "src" / "generated_ride_triggers.h"
LEVEL = re.compile(r"levels\\(ch\d\d_[a-z0-9_]+)", re.I)
RIDE_NAME = re.compile(r"(elevator|(^|_)lift|tram|gondola|platform).*(up|down|start|go|ride|move)|(^|_)ride_", re.I)
# Lights, sound, culling, VFX and AI hints carry the same words and move nobody
# (also behind an authored prefix like xvora7769x_cad6bba_85cdb12_v3_).
NOT_RIDE = re.compile(r"(^|_v3_)(lt_|aud_|enable_cull|disable_cull|cull|vfx_|retreat|debug|var_op|var_switch|obj_|equip_|log_)", re.I)
RIDE_MOVERS = {"MTT_LIFT", "MTT_ELEVATOR", "MTT_MACHINE", "MTT_TRAM"}
GATE_CLASSES = {"SimpleOneTimeDoor", "Mover", "MoverController"}


def name_hash(s):
    v = 0
    for c in s.lower():
        v = (v * 0x1003F + ord(c)) & 0xFFFFFFFF
    return v


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    listeners, senders, inputs = g.load_graph()
    # Mover types, and which Mover a MoverController drives (same GUID prefix: the pair is authored together)
    mover_type = {}
    controllers = {}
    attached = {}
    for line in CATALOG.open(encoding="utf-8"):
        if '"Mover' not in line and '"Attacher"' not in line:
            continue
        r = json.loads(line)
        im = (r.get("images") or [{}])[0]
        if im.get("class") == "Mover":
            for f in im.get("fields", []):
                if f.get("name") == "CMD_SetMoverType" and f.get("string_candidate"):
                    mover_type[r["guid"]] = f["string_candidate"]
        elif im.get("class") == "Attacher":
            links = {f["name"]: f.get("guid_candidate") for f in im.get("fields", [])}
            child, parent = links.get("CMD_SetChildEntity"), links.get("CMD_SetParentEntity")
            if child and parent:
                attached.setdefault(child, set()).add(parent)
        elif im.get("class") == "MoverController":
            controllers[r["guid"]] = r
    prefix_type = {}
    for guid, t in mover_type.items():
        prefix_type.setdefault(guid.split("-")[0], t)

    def ride_reason(key, depth=0, seen=None):
        seen = seen or set()
        if key in seen or depth > 3:
            return None
        seen = seen | {key}
        if RIDE_NAME.search(key[1]) and not NOT_RIDE.search(key[1]) and not key[1].startswith("//"):   # "//" = a target commented out
            # an animation event on a prop: no authored listener at all
            if not listeners.get(key):
                return f"ride animation {key[1]}"
        for guid, owner, part, field, enemy in listeners.get(key, []):
            if owner == "MoverController" and field == "MSG_CALL":
                t = mover_type.get(guid) or prefix_type.get(guid.split("-")[0], "")
                if t in RIDE_MOVERS:
                    return f"MoverController {guid} ({t})"
            if owner in g.MULTI:
                for out_key, out_who in ((k, w) for k, w in senders.items() if any(x[0] == guid for x in w)):
                    why = ride_reason(out_key, depth + 1, seen)
                    if why:
                        return f"{key[1]} -> {why}"
        return None

    sends = collections.defaultdict(list)
    owner_of = {}
    for key, who in senders.items():
        for guid, owner, part, field, enemy in who:
            sends[guid].append(key)
            owner_of[guid] = owner
    follow = g.MULTI | {"VariableSwitch", "OrEventGate", "AndEventGate", "MessageRelay"}

    def lift_controller(guid, call):
        t = mover_type.get(guid) or prefix_type.get(guid.split("-")[0], "")
        # an untyped Mover is a cabin when its call is named like one; machines and trams have no cabin to wait in
        return t in ("MTT_LIFT", "MTT_ELEVATOR") or (not t and re.search(r"elevator|lift", call))

    lift_controllers = {}
    for key, who in listeners.items():
        for lg, owner, part, field, enemy in who:
            if owner == "MoverController" and field == "MSG_CALL" and lift_controller(lg, key[1]):
                lift_controllers[lg] = key[1]

    def lift_reason(guid, depth=0, seen=frozenset()):
        if depth > 3 or guid in seen:
            return None
        seen = seen | {guid}
        for key in sends.get(guid, []):
            for lg, owner, part, field, enemy in listeners.get(key, []):
                if owner == "MoverController" and field == "MSG_CALL" and lg in lift_controllers:
                    return f"{key[1]} -> MoverController {lg}"
                if owner in follow:
                    why = lift_reason(lg, depth + 1, seen)
                    if why:
                        return f"{key[1]} -> {owner} {lg}: {why}"
        return None

    lift_managers = {}
    for guid, owner in owner_of.items():
        if owner in g.MULTI:
            why = lift_reason(guid)
            if why:
                lift_managers[guid] = why

    cabin_movers = set()
    for guid in lift_controllers:
        record = controllers.get(guid)
        if record:
            cabin_movers.update(f.get("guid_candidate") for f in record["images"][0]["fields"]
                                if f["name"] == "CMD_SetMover" and f.get("guid_candidate"))
    cabin_buttons = {}
    for guid, owner in owner_of.items():
        parents = attached.get(guid, set())
        if owner == "Switch" and len(parents) == 1 and parents <= cabin_movers and lift_reason(guid):
            cabin_buttons[guid] = next(iter(parents))
    print(f"{len(cabin_buttons)} attached ride switches in {len(set(cabin_buttons.values()))} cabins")

    found = {}
    for key, who in senders.items():
        for guid, owner, part, field, enemy in who:
            if owner != "TriggerPhysical" or not ("Broadcast" in field or "DirectMessage" in field) or NOT_RIDE.search(key[1]):
                continue
            ls = listeners.get(key, [])
            if any(o in GATE_CLASSES for _, o, _, _, _ in ls):
                continue   # the gate holds it already
            why = ride_reason(key)
            if why:
                found[key] = (guid, field, why)
    by_level = collections.Counter(k[0] for k in found)
    for (level, name), (guid, field, why) in sorted(found.items()):
        print(f"{level:18} {name_hash(name):08X} {name:48} {guid} {field} :: {why[:120]}")
    print(f"\n{len(found)} ride triggers: " + ", ".join(f"{lv} {n}" for lv, n in sorted(by_level.items())))
    for guid, why in sorted(lift_managers.items()):
        print(f"lift manager {guid} :: {why[:140]}")
    print(f"{len(lift_managers)} lift managers, {len(lift_controllers)} lift controllers")
    if args.write:
        lines = ["// Generated by tools/coop_ride_triggers.py: messages of the boxes a player steps into",
                 "// that start a ride (the gate holds them until every player stands in the box).",
                 "#pragma once", "#include <cstdint>", "namespace coop_rides {",
                 "struct Ride { uint32_t hash; const char* level; const char* name; };",
                 "constexpr Ride kRides[] = {"]
        for (level, name), _ in sorted(found.items()):
            lines.append(f'    {{0x{name_hash(name):08X}u, "{level}", "{name}"}},')
        lines += ["};",
                  "// MultiManagers that move a lift or elevator cabin (GUID words, entity+0x24), sorted.",
                  "constexpr uint32_t kLiftManagers[][4] = {"]
        for guid in sorted(lift_managers, key=lambda s: tuple(int(w, 16) for w in s.split("-"))):
            words = ", ".join(f"0x{int(w, 16):08X}u" for w in guid.split("-"))
            lines.append(f"    {{{words}}},")
        lines += ["};",
                  "// The cabins' MoverControllers (MSG_CALL of a lift or elevator Mover), sorted.",
                  "constexpr uint32_t kLiftControllers[][4] = {"]
        for guid in sorted(lift_controllers, key=lambda s: tuple(int(w, 16) for w in s.split("-"))):
            words = ", ".join(f"0x{int(w, 16):08X}u" for w in guid.split("-"))
            lines.append(f"    {{{words}}},")
        lines += ["};", "struct CabinButton { uint32_t button[4], mover[4]; };",
                  "constexpr CabinButton kCabinButtons[] = {"]
        for guid, mover in sorted(cabin_buttons.items()):
            pair = ["{" + ", ".join("0x" + w + "u" for w in key.split("-")) + "}" for key in (guid, mover)]
            lines.append("    {" + ", ".join(pair) + "},")
        lines += ["};", "}  // namespace coop_rides", ""]
        HEADER.write_text("\n".join(lines), encoding="utf-8", newline="\n")
        print(f"wrote {HEADER}")


if __name__ == "__main__":
    main()
