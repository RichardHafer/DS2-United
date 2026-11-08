"""Build src/generated_zone_objectives.h: Hold the Zone opens the MP maps' doors.

Per map, the messages that complete each objective step in the map's own order,
the door messages of the final step (sent directly, the step itself never is),
the messages of the round's end, which the dispatch drops while a Zone visit
runs, and every message the plan reaches through the map's relays, which stay
local to each machine (no gate hold, no mirror).

The plan below is curated from the level data (analysis/HOLD_THE_ZONE_OBJECTIVES_2026-10-12.md).
This script checks it against the data: every message it sends must have a
listener on that map, every dropped message must occur there, and no round-end
message may be reachable from the plan through the map's relays undropped.

Sources: analysis/catalog/entities.jsonl (field lists) for the five shipped
maps, the baked images (tools/mp_baked.py) for the two Outbreak maps.

usage: python -B tools/build_zone_objectives.py [--report]
(--report prints the per-map tables of the analysis document instead of writing the header)
"""
import collections
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mp_baked  # noqa: E402

CATALOG = ROOT / "analysis" / "catalog" / "entities.jsonl"
OUT = ROOT / "src" / "generated_zone_objectives.h"
# the order of coop_mpmap::kMapTable
MAPS = ["mp_bombRun", "mp_detonate", "mp_escape", "mp_gov", "mp_solar", "pdlc_mp_mall", "pdlc_mp_school"]
STOCK = {m.lower(): m for m in MAPS if not m.startswith("pdlc")}


def name_hash(text):
    return mp_baked.name_hash(text)


# --- the message graph ------------------------------------------------------------------
# Fields that send; every other message field listens.
SEND = {
    "AndEventGate": {"CMD_SetOutputEvent"}, "OrEventGate": {"CMD_SetOutputEvent"},
    "DamageableProp": {"CMD_SetDamageMsg1", "CMD_SetDamageMsg2", "CMD_SetDamageMsg3", "CMD_SetDamageMsg4", "CMD_SetDeathTarget"},
    "KeyItem": {"CMD_OnDropMsg", "CMD_OnPickupMsg"}, "KeyLock": {"CMD_UnlockMessage"},
    "MPEndOfGameSequencer": {"m_humanLossMsg", "m_humanWinMsg", "m_necroLossMsg", "m_necroWinMsg"},
    "MultiManager": {"CMD_SetTarget%d" % i for i in range(1, 17)}, "TriggerRandom": {"CMD_SetTarget%d" % i for i in range(1, 5)},
    "MultiplayerSpawnPoint": {"m_onUsageMessage"}, "NetBinaryState": {"m_msgStateOnOutput"},
    "ObjectiveEntry": {"MSG_BroadcastNewObjective"}, "PlayerAndGate": {"CMD_SetOutput"},
    "ProximitySwitch": {"CMD_SetEntered", "CMD_SetExited"}, "RelayMessageByTeam": {"m_msgOutputHuman", "m_msgOutputNecro"},
    "SimplePressAndHoldSwitch": {"CMD_SendCompleteAllNet", "CMD_SetCompleted"}, "StreamSet": {"CMD_SetCompleteMessage"},
    "Switch": {"MSG_ON"}, "TimerUtil": {"CMD_LowTime", "CMD_ThreshMsg1", "CMD_ThreshMsg2", "CMD_TimeAdded", "CMD_TimesUp"},
    "TriggerAuto": {"CMD_SetTarget"},
    "TriggerPhysical": {"CMD_SetBroadcast1Message", "CMD_SetBroadcast2Message", "CMD_SetBroadcast3Message", "CMD_SetBroadcast4Message",
                        "CMD_SetDirectMessage"},
    "VariableSwitch": {"CMD_SetTarget1", "CMD_SetTarget2"}, "VentMarker": {"m_activateLocalCulling"},
    "DismemberableHealthComponent": {"CMD_SetOnDeathMessage"},
}
# The Outbreak maps' baked images: (class, offset) -> (field, direction), from the
# stock field order and value matching (analysis/DS2_MP_MAPS_BUILDING_BLOCKS_2026-10-10.md).
BAKED = {
    ("MultiManager", 0): ("CMD_SetTargetName", "L"), ("TriggerRandom", 0): ("CMD_SetTargetName", "L"),
    ("FuncSpawn", 0): ("CMD_SetTargetName", "L"), ("MultiRemoveTarget", 0): ("CMD_SetTargetName", "L"),
    ("DebugText", 0): ("CMD_SetTargetName", "L"), ("SetAudioMix", 0): ("CMD_SetTargetName", "L"),
    ("VariableOperator", 0): ("CMD_SetTargetName", "L"), ("VariableSwitch", 0): ("CMD_SetTargetName", "L"),
    ("VariableSwitch", 0x28): ("CMD_SetTarget1", "S"), ("VariableSwitch", 0x3C): ("CMD_SetTarget2", "S"),
    ("DamageableProp", 0x64): ("CMD_SetDeathTarget", "S"), ("DamageableProp", 0x9C): ("m_invincibleOffMsg", "L"),
    ("DamageableProp", 0xA8): ("CMD_SetDamageMsg1", "S"), ("DamageableProp", 0xB8): ("CMD_SetDamageMsg2", "S"),
    ("DamageableProp", 0xC8): ("CMD_SetDamageMsg3", "S"), ("DamageableProp", 0xD8): ("CMD_SetDamageMsg4", "S"),
    ("DamageableProp", 0x10C): ("m_showHealthMeterMsg", "L"), ("DamageableProp", 0x118): ("m_hideHealthMeterMsg", "L"),
    ("KeyLock", 0x88): ("CMD_UnlockMessage", "S"), ("KeyItem", 0x84): ("CMD_OnPickupMsg", "S"), ("KeyItem", 0x90): ("CMD_OnDropMsg", "S"),
    ("MultiplayerSpawnPoint", 4): ("m_onUsageMessage", "S"), ("MultiplayerSpawnPoint", 0x14): ("m_activateMessage", "L"),
    ("MultiplayerSpawnPoint", 0x20): ("m_deactivateMessage", "L"),
    ("ProximitySwitch", 0x14): ("CMD_SetEnabled", "L"), ("ProximitySwitch", 0x20): ("CMD_Reset", "L"),
    ("ProximitySwitch", 0x2C): ("CMD_SetEntered", "S"), ("ProximitySwitch", 0x38): ("CMD_SetExited", "S"),
    ("ProximitySwitch", 0x44): ("CMD_SetCompleted", "S"),
    ("SimplePressAndHoldSwitch", 0x94): ("CMD_SetEnabled", "L"), ("SimplePressAndHoldSwitch", 0xA0): ("CMD_SetCompleted", "S"),
    ("SimplePressAndHoldSwitch", 0x120): ("CMD_SendCompleteAllNet", "S"),
    ("TriggerPhysical", 0x40): ("CMD_SetBroadcast1Message", "S"), ("TriggerPhysical", 0x58): ("CMD_SetBroadcast2Message", "S"),
    ("TriggerPhysical", 0x70): ("CMD_SetBroadcast3Message", "S"), ("TriggerPhysical", 0x88): ("CMD_SetBroadcast4Message", "S"),
    ("VentMarker", 0x170): ("m_activateLocalCulling", "S"), ("VentMarker", 0x1D4): ("m_activateSelection", "L"),
    ("VentMarker", 0x1E0): ("m_deactivateSelection", "L"),
    ("SimpleOneTimeDoor", 0x50): ("CMD_OpenDoor", "L"), ("TimerSet", 0): ("CMD_Activate", "L"),
    ("StreamSet", 0x0C): ("CMD_SetCompleteMessage", "S"), ("TriggerAuto", 0): ("CMD_SetTarget", "S"),
    ("AndEventGate", 0): ("CMD_SetInputEvent01", "L"), ("AndEventGate", 0x0C): ("CMD_SetInputEvent02", "L"),
    ("AndEventGate", 0x30): ("CMD_SetOutputEvent", "S"),
    ("OrEventGate", 0): ("CMD_SetInputEvent01", "L"), ("OrEventGate", 0x0C): ("CMD_SetInputEvent02", "L"),
    ("OrEventGate", 0x18): ("CMD_SetInputEvent03", "L"), ("OrEventGate", 0x24): ("CMD_SetInputEvent04", "L"),
    ("OrEventGate", 0x30): ("CMD_SetOutputEvent", "S"),
    ("TimerUtil", 0): ("CMD_StartTimer", "L"), ("TimerUtil", 0x48): ("CMD_TimesUp", "S"), ("TimerUtil", 0x15C): ("CMD_LowTime", "S"),
    ("TimerUtil", 0x26C): ("CMD_TimeAdded", "S"), ("TimerUtil", 0x38C): ("CMD_ThreshMsg1", "S"), ("TimerUtil", 0x39C): ("CMD_ThreshMsg2", "S"),
    ("RelayMessageByTeam", 0): ("m_msgInput", "L"), ("RelayMessageByTeam", 0x0C): ("m_msgOutputHuman", "S"),
    ("RelayMessageByTeam", 0x18): ("m_msgOutputNecro", "S"),
    ("NetBinaryState", 0): ("m_msgStateOn", "L"), ("NetBinaryState", 0x0C): ("m_msgStateOnOutput", "S"),
    ("ObjectiveEntry", 0xB0): ("MSG_RcvNewObjective", "L"), ("ObjectiveEntry", 0xBC): ("MSG_BroadcastNewObjective", "S"),
    ("ObjectiveEntry", 0xEC): ("MSG_ForceCompleteObjective", "L"), ("ObjectiveEntry", 0x18C): ("MSG_AttachSub1", "L"),
    ("MPEndOfGameSequencer", 0): ("m_msgActivate", "L"), ("MPEndOfGameSequencer", 0x0C): ("m_msgNecroWin", "L"),
    ("MPEndOfGameSequencer", 0x18): ("m_msgHumanWin", "L"),
    ("PlayerAndGate", 0): ("CMD_SetActivate", "L"), ("PlayerAndGate", 0x0C): ("CMD_SetOutput", "S"),
    ("DismemberableHealthComponent", 4): ("CMD_SetOnDeathMessage", "S"),
}
# Listeners [MpMap] keeps from spawning in a Zone visit (generated_mpmap_block.h
# groups 1 to 4: end of round, round clock, objective markers, team relays).
ABSENT = {"OnlineEndRound", "MPEndOfGameSequencer", "UIMPRoundTimerHub", "UIMPObjectiveIndicator", "RelayMessageByTeam", "MPDamageFilter"}
# Listeners that only show or play something and send nothing on.
SHOW = {"DebugText", "AnimateLight", "MoveLight", "TriggerShadowLight", "ExecuteVFX", "NetExecuteVFX", "PlaySound", "PlayAudioStream",
        "SetAudioMix", "LuaAudioScript", "StreamScripter", "VFXAnalogMeter"}


class Entity:
    __slots__ = ("map", "guid", "cls", "zone", "sf", "msgs", "values", "spawns")

    def __init__(self, map_, guid, cls, zone, sf):
        self.map, self.guid, self.cls, self.zone, self.sf = map_, guid, cls, zone, sf
        self.msgs = []      # (field, name, hash, direction, delay)
        self.values = {}    # field -> value (stock maps)
        self.spawns = ""    # FuncSpawn target GUID

    def short(self):
        return "%s@%s#%s" % (self.cls, self.zone, self.guid[9:17])


def stock_map(archive):
    parts = archive.lower().split("\\")
    return STOCK.get(parts[1]) if len(parts) > 2 and parts[0] == "ds2dat10" else None


def f32(v):
    return struct.unpack("<f", struct.pack("<I", v & 0xFFFFFFFF))[0]


def load_stock():
    ents = {}
    with open(CATALOG, encoding="utf-8") as f:
        for line in f:
            if "ds2dat10" not in line[:400].lower():
                continue
            e = json.loads(line)
            m = stock_map(e["archive"])
            if not m:
                continue
            key = (m, e["guid"].upper())
            if key in ents:
                continue
            zone = e["archive"].split("\\")[-1].lower().replace(".str", "")
            ent = Entity(m, e["guid"].upper(), e.get("class", "?"), zone, e.get("spawn_flags", 0))
            for i, im in enumerate(e.get("images", [])):
                cls = im.get("class", "?")
                fields = im.get("fields", [])
                if i == 0:
                    for fl in fields:
                        n = fl.get("name") or "0x%08X" % fl["field_id"]
                        ent.values[n] = fl.get("string_candidate") or fl.get("guid_candidate") or fl.get("value")
                    ent.spawns = (next((fl.get("guid_candidate") for fl in fields if fl.get("name") == "CMD_SetSpawnTarget"), "") or "").upper()
                for fl in fields:
                    name = fl.get("string_candidate")
                    if fl.get("flags") != 17 or not name:
                        continue
                    field = fl.get("name") or "0x%08X" % fl["field_id"]
                    sends = field in SEND.get(cls, ())
                    delay = 0.0
                    if sends and ent.cls == "MultiManager" and field.startswith("CMD_SetTarget"):
                        raw = ent.values.get("CMD_SetDelay" + field[len("CMD_SetTarget"):])
                        delay = f32(raw) if isinstance(raw, int) else 0.0
                    ent.msgs.append(((cls + "." if i else "") + field, name, name_hash(name), "S" if sends else "L", delay))
            ents[key] = ent
    return ents


def baked_messages(blob):
    """(offset, hash, name) of every message field: {u32 hash, u32, u32 self-relative string offset}."""
    out = []
    n = len(blob)
    for i in range(0, n - 11, 4):
        h = struct.unpack_from("<I", blob, i)[0]
        if not h:
            continue
        p = struct.unpack_from("<I", blob, i + 8)[0]
        if not p or p >= 0x10000 or i + 8 + p >= n:
            continue
        end = blob.find(b"\0", i + 8 + p)
        text = blob[i + 8 + p:end if end >= 0 else n]
        if text and all(32 <= c < 127 for c in text) and name_hash(text.decode()) == h:
            out.append((i, h, text.decode()))
    return out


def load_baked():
    ents = {}
    classes = set(mp_baked._type_classes().values())
    for e in mp_baked.iter_baked_entities(classes):
        m = next(x for x in MAPS if x.lower() == e["map"])
        key = (m, e["guid"])
        if key in ents:
            continue
        zone = e["archive"].split("\\")[-1].lower().replace(".str", "")
        ent = Entity(m, e["guid"], e["class"], zone, 1)
        images = [(e["class"], e["blob"])] + [(c, b) for c, b in e["components"].items()]
        for i, (cls, blob) in enumerate(images):
            for off, h, name in baked_messages(blob):
                if cls == "MultiManager" and off >= 0x14 and (off - 0x14) % 16 == 0:
                    field, d = "CMD_SetTarget%d" % ((off - 0x14) // 16 + 1), "S"
                    delay = struct.unpack_from("<f", blob, off + 12)[0] if off + 16 <= len(blob) else 0.0
                else:
                    field, d = BAKED.get((cls, off), ("+0x%X" % off, "?"))
                    delay = 0.0
                ent.msgs.append(((cls + "." if i else "") + field, name, h, d, delay))
            if i == 0 and cls == "FuncSpawn":
                ent.spawns = mp_baked.guid_at(blob, mp_baked.FUNCSPAWN_TARGET)
        ents[key] = ent
    return ents


def load_all():
    ents = load_stock()
    ents.update(load_baked())
    return ents


def graph(ents, m):
    """name -> {"S": [(entity, field, delay)], "L": [(entity, field)], "?": [...]}"""
    g = collections.defaultdict(lambda: {"S": [], "L": [], "?": []})
    for (mm, _), e in ents.items():
        if mm != m:
            continue
        for field, name, h, d, delay in e.msgs:
            g[name.lower()][d].append((e, field, delay) if d == "S" else (e, field))
    return g


# --- the plan ----------------------------------------------------------------------------
# Per map: the steps in the map's own order (each step's messages are what the
# map sends when the players finish that objective), then "open": what the final
# objective's start would open (doors, vent groups), sent without that start.
# "block": the final objective's start and completion and anything only its end
# plays; the round's end messages (END) are added for every map that has them.
END = ["online_end_round_human_win", "online_end_round_necro_win", "round_end_humanwin", "round_end_necrowin", "var_switch_round_end",
       "var_op_human_win", "end_round", "mp_endofgamesequencer_human_win", "mp_endofgamesequencer_necro_win"]
PLAN = {
    "mp_bombRun": {
        # bomb parts 1..3 delivered to the bench; the dispensers open as when their timer ran out
        "steps": [("bomb part 1", ["mp_simpledoor_obj_1_open", "mm_bench_component_1"]),
                  ("bomb part 2", ["mp_simpledoor_obj_2_open", "mm_bench_component_2"]),
                  ("bomb part 3", ["mp_simpledoor_obj_3_open", "mm_bench_component_3"])],
        # the blast door to the escape hall opens only in the human win's sequence:
        # the hall's visibility, the door's explosion and its kill
        "open": ["_culling_exitLoad", "msg_vfx_spawn_door_explosion", "mm_prop_damage_door"],
        # part 3's manager also starts the defence (objective 4): the bench swap,
        # its timer, health bar and locator; spawn_base_3 would remove the bench
        # parts for the defence bench, so it stays out too
        "block": ["obj_4_new", "mm_start_bench_timer", "spawn_base_3", "spawn_locator_bench_protect", "timerset_obj4_reset_time",
                  "timer_defend_start", "timer_defend_end", "prop_damage_bench_destroyed"],
    },
    "mp_detonate": {
        "steps": [("fuse repaired", ["mm_obj_1_complete"]),
                  ("workshop switch", ["simple_switch_obj2_complete"]),
                  ("data drive", ["mm_obj_4"]),
                  ("core switch", ["mm_obj_5"])],
        "open": [],
        # objective 5 defends the detonator; the core's flame jets start with
        # objective 4 and would cycle for the rest of the run
        "block": ["obj_5_new", "mm_start_obj5_detonator", "timer_obj5_detonator_start", "prop_damage_obj5_destroyed", "mm_flamejet_initial"],
    },
    "mp_escape": {
        "steps": [("server room switches", ["custom_switch1_obj1_complete", "custom_switch2_obj1_complete"]),
                  ("control room switches", ["custom_switch3_obj2_complete", "custom_switch4_obj2_complete"]),
                  ("atrium switches", ["custom_switch5_obj3_complete", "custom_switch6_obj3_complete"]),
                  ("pod switch", ["mm_obj_4_complete"])],
        "open": [],
        # objective 5 arms the four pod triggers: a player inside is put into
        # observer mode (a direct send the dispatch never sees)
        "block": ["obj_5_new", "player_and_gate_player_escaped", "timerset_obj5_reset"],
    },
    "mp_gov": {
        "steps": [("power node", ["obj_1_complete"]),
                  ("power switch", ["obj_2_complete"]),
                  ("marker 1", ["obj_3_complete"]),
                  ("containment switch", ["obj_4_complete"])],
        "open": [],
        "block": ["obj_5_new", "obj_5_activate", "obj_5_complete"],
    },
    "mp_solar": {
        "steps": [("power switch", ["mm_power_complete"]),
                  ("security switch", ["mm_security_complete"]),
                  ("disk delivered", ["mm_disk_complete"]),
                  ("control switches", ["mm_control_01_complete", "mm_control_02_complete"])],
        "open": [],
        "block": ["mm_coordinates_start", "obj_05_new", "obj_coordinates_start", "spawn_coordinates", "mm_coordinates_complete"],
    },
    "pdlc_mp_mall": {
        "steps": [("keycard 1", ["mm_key_lock_obj_1_received"]),
                  ("glass and keycard 2", ["mm_obj_2_safe_destroyed", "mm_key_lock_obj_2_received"]),
                  ("amplifier", ["mm_key_lock_obj_3_received"]),
                  ("south room switch", ["mm_obj_4_complete"])],
        # objective 5's start opens the lower hall door and the atrium vents
        "open": ["open_doors_to_objective_5", "activate_vents_area_2", "activate_vents_area_3", "activate_vents_atrium_lower",
                 "activate_vents_atrium_upper"],
        # the human win's gunship and window shutters are its cinematic
        "block": ["obj_5_new", "mm_obj_5_complete", "gunship_spawn", "gunship_animate", "mp_mall_shutters_open"],
    },
    "pdlc_mp_school": {
        "steps": [("cables delivered", ["key_lock_obj1_cables_complete"]),
                  ("breaker and circuit", ["prop_damage_obj2_door_destroyed", "key_lock_obj2_circuit_complete"]),
                  ("class hall switch", ["switch_obj3_complete"]),
                  ("monitor delivered", ["key_lock_obj4_monitor_complete"])],
        # objective 5's start opens the gym's centre platform
        "open": ["mp_simpleDoor_vent_cover_open", "vent_spawn_gym_activate", "vent_spawn_obj2_activate", "vent_spawn_obj2_deliver_activate",
                 "vent_spawn_obj4_activate"],
        "block": ["obj_5_new", "mm_obj5_start", "prop_damage_obj5_invincible_off", "obj5_destroy_complete"],
    },
}
ROUND_STARTS = {"start_roundtimer", "round_timer_start"}


def absent(e):
    if e.cls in ABSENT:
        return True
    if e.cls == "TimerUtil":   # the round clock ([MpMap] group 2)
        return any(f.endswith("CMD_StartTimer") and n.lower() in ROUND_STARTS for f, n, h, d, _ in e.msgs)
    return False


def inert(e):
    return e.cls in SHOW or absent(e)


def by_guid(ents, m):
    return {g: e for (mm, g), e in ents.items() if mm == m}


def fan_out(e, field, guids):
    """What a listener sends by itself once it hears a message on `field`:
    [(message name, delay s)]. Players and enemies are not assumed."""
    sends = [(f, n, delay) for f, n, h, d, delay in e.msgs if d == "S"]
    if e.cls in ("MultiManager", "TriggerRandom", "AndEventGate", "OrEventGate", "VariableSwitch", "NetBinaryState"):
        return [(n, delay) for f, n, delay in sends]
    if e.cls == "ObjectiveEntry" and field == "MSG_RcvNewObjective":
        return [(n, 0.0) for f, n, delay in sends if f == "MSG_BroadcastNewObjective"]
    if e.cls == "TimerUtil" and field == "CMD_StartTimer":
        time = e.values.get("CMD_SetTime")
        return [(n, f32(time) if isinstance(time, int) else 0.0) for f, n, delay in sends if f == "CMD_TimesUp"]
    if e.cls == "DamageableProp" and field == "CMD_SetKillMessage":
        return [(n, 0.0) for f, n, delay in sends if f == "CMD_SetDeathTarget"]
    if e.cls == "FuncSpawn" and e.spawns in guids and guids[e.spawns].cls == "TriggerAuto":
        t = guids[e.spawns]
        d = t.values.get("CMD_SetDelay")
        return [(n, f32(d) if isinstance(d, int) else 0.0) for f, n, h, dd, _ in t.msgs if dd == "S"]
    return []


def arms(e, field, guids):
    """What players or enemies can make a listener send once it heard `field`:
    a switch enabled, a trigger armed, a prop made damageable or spawned."""
    sends = [n for f, n, h, d, _ in e.msgs if d == "S"]
    if e.cls in ("SimplePressAndHoldSwitch", "ProximitySwitch", "Switch") and field in ("CMD_SetEnabled", "MSG_ENABLE"):
        return sends
    if e.cls == "TriggerPhysical" and field == "CMD_SetActivateMessage":
        return sends
    if e.cls == "DamageableProp" and field == "m_invincibleOffMsg":
        return sends
    if e.cls == "FuncSpawn" and e.spawns in guids:
        return [n for f, n, h, d, _ in guids[e.spawns].msgs if d == "S"]
    return []


def closure(ents, m, seeds, blocked):
    """Every message the seeds reach through the map's relays: name -> (first
    arrival s, path). Blocked messages are reached but do not spread."""
    g = graph(ents, m)
    guids = by_guid(ents, m)
    seen = {}
    queue = [(0.0, s.lower(), s) for s in seeds]
    while queue:
        queue.sort(key=lambda x: x[0])
        at, name, path = queue.pop(0)
        if name in seen:
            continue
        seen[name] = (at, path)
        if name in blocked:
            continue
        for e, field in g[name]["L"] + g[name]["?"]:
            if inert(e):
                continue
            for n, d in fan_out(e, field, guids):
                if n.lower() not in seen:
                    queue.append((at + max(d, 0.0), n.lower(), path + " > " + n))
    return seen


def interactions(ents, m, reached, blocked):
    """What the reached messages arm for players and enemies: name -> armed by."""
    g = graph(ents, m)
    guids = by_guid(ents, m)
    out = {}
    for name in reached:
        if name in blocked:
            continue
        for e, field in g[name]["L"] + g[name]["?"]:
            if inert(e):
                continue
            for n in arms(e, field, guids):
                out.setdefault(n.lower(), "%s armed by %s" % (e.short(), name))
    return out


def live_listeners(g, name):
    s = g.get(name.lower())
    return [(e, f) for e, f in s["L"] + s["?"] if not absent(e)] if s else []


def check(ents):
    """The plan against the data: one row per map, and the errors."""
    rows, errors = [], []
    for m in MAPS:
        p = PLAN[m]
        g = graph(ents, m)
        sent = [x for _, step in p["steps"] for x in step] + p["open"]
        block = list(p["block"])
        for n in END:
            s = g.get(n.lower())
            if s and (s["L"] or s["S"] or s["?"]) and n.lower() not in {b.lower() for b in block}:
                block.append(n)
        blocked = {b.lower() for b in block}
        for n in sent:
            if not live_listeners(g, n):
                errors.append("%s: %s has no listener on the map" % (m, n))
            if n.lower() in blocked:
                errors.append("%s: %s is both sent and blocked" % (m, n))
        for n in p["block"]:
            if n.lower() not in g:
                errors.append("%s: blocked %s does not occur on the map" % (m, n))
        reach = closure(ents, m, sent, blocked)
        for n in END:
            if n.lower() in reach and n.lower() not in blocked:
                errors.append("%s: the plan reaches %s (%s) unblocked" % (m, n, reach[n.lower()][1]))
        # each batch's own relays, so the next one waits until they have played
        batch_tails = []
        for msgs in [step for _, step in p["steps"]] + ([p["open"]] if p["open"] else []):
            own = closure(ents, m, msgs, blocked)
            batch_tails.append(max([a for n, (a, _) in own.items() if live_listeners(g, n)] or [0.0]))
        rows.append({"map": m, "plan": p, "block": block, "reach": reach, "hit": sorted(n for n in reach if n in blocked),
                     "armed": interactions(ents, m, reach, blocked), "graph": g, "batch_tails": batch_tails})
    return rows, errors


# --- the report ---------------------------------------------------------------------------
def effects(ents, row, seeds):
    """What a step's messages do on their own: a short text per kind."""
    m, g = row["map"], row["graph"]
    blocked = {b.lower() for b in row["block"]}
    reach = closure(ents, m, seeds, blocked)
    out = collections.defaultdict(list)
    counts = collections.Counter()
    for name in reach:
        if name in blocked:
            out["blocked"].append(name)
            continue
        for e, field in g[name]["L"] + g[name]["?"]:
            if absent(e):
                continue
            if e.cls == "SimpleOneTimeDoor":
                out["doors"].append("%s %s (%s)" % (e.zone, e.guid[9:17], name))
            elif e.cls == "DamageableProp" and field == "CMD_SetKillMessage":
                out["doors"].append("%s %s killed (%s)" % (e.zone, e.guid[9:17], name))
            elif e.cls == "ObjectiveEntry" and field in ("MSG_RcvNewObjective", "MSG_ForceCompleteObjective"):
                out["objectives"].append(("new " if field == "MSG_RcvNewObjective" else "done ") + name)
            elif e.cls == "VentMarker":
                counts["vents on" if "activate" in field and "deactivate" not in field else "vents off"] += 1
            elif e.cls == "MultiplayerSpawnPoint":
                counts["spawn points on" if field == "m_activateMessage" else "spawn points off"] += 1
            elif e.cls == "TimerSet":
                counts["round clock bonus (no clock in Zone)"] += 1
            elif e.cls == "FuncSpawn":
                t = by_guid(ents, m).get(e.spawns)
                counts["spawns " + (t.cls if t else "?")] += 1
            elif e.cls in ("SimplePressAndHoldSwitch", "Switch", "ProximitySwitch") and field in ("CMD_SetEnabled", "MSG_ENABLE"):
                counts["switch enabled"] += 1
            elif e.cls == "TriggerPhysical":
                counts["trigger " + ("armed" if "Activate" in field else "disarmed")] += 1
            elif e.cls == "TimerUtil" and field == "CMD_StartTimer":
                counts["timer started"] += 1
            elif e.cls == "DamageableProp":
                counts["prop " + field.replace("m_", "").replace("Msg", "")] += 1
            elif e.cls == "MultiRemoveTarget":
                counts["removal"] += 1
            elif e.cls in ("PlayAudioStream", "PlaySound", "LuaAudioScript"):
                counts["audio"] += 1
            elif e.cls in ("ExecuteVFX", "NetExecuteVFX"):
                counts["effects"] += 1
            elif e.cls == "StreamScripter":
                counts["zone visibility"] += 1
    for k, v in sorted(counts.items()):
        out["other"].append("%s x%d" % (k, v))
    return out


def report(ents, rows):
    lines = []
    for row in rows:
        m, p, g = row["map"], row["plan"], row["graph"]
        lines.append("### %s" % m)
        lines.append("")
        lines.append("| step | sent (hash) | objectives | doors | other | blocked on the way |")
        lines.append("|---|---|---|---|---|---|")
        steps = list(p["steps"]) + ([("open (final step's doors)", p["open"])] if p["open"] else [])
        for label, msgs in steps:
            ef = effects(ents, row, msgs)
            sent = "<br>".join("`%s` %08X" % (n, name_hash(n)) for n in msgs)
            lines.append("| %s | %s | %s | %s | %s | %s |" % (label, sent, "<br>".join(sorted(set(ef["objectives"]))) or "-",
                                                         "<br>".join(sorted(set(ef["doors"]))) or "-", "<br>".join(ef["other"]) or "-",
                                                         "<br>".join("`%s`" % b for b in sorted(set(ef["blocked"]))) or "-"))
        lines.append("")
        lines.append("Dropped while the Zone visit runs:")
        lines.append("")
        lines.append("| message | hash | sent by |")
        lines.append("|---|---|---|")
        for n in row["block"]:
            s = g.get(n.lower(), {"S": []})
            senders = sorted({"%s %s" % (e.cls, f.replace("CMD_Set", "").replace("CMD_", "")) for e, f, d in s["S"]})
            lines.append("| `%s` | %08X | %s |" % (n, name_hash(n), ", ".join(senders) or "(no sender)"))
        lines.append("")
        lines.append("Local window: %d messages, the last one heard %.1f s after its step." % (len(row["reach"]), tail_s(row)))
        armed = sorted(n for n in row["armed"] if n in {b.lower() for b in row["block"]})
        if armed:
            lines.append("Players or enemies can still produce: %s (dropped)." % ", ".join("`%s`" % a for a in armed))
        lines.append("")
    return "\n".join(lines)


def tail_s(row):
    """The latest first arrival of a reached message that something hears."""
    return max([a for n, (a, _) in row["reach"].items() if live_listeners(row["graph"], n)] or [0.0])


# --- the header -----------------------------------------------------------------------------
def c_messages(names):
    return ", ".join('{0x%08Xu, "%s"}' % (name_hash(n), n) for n in names)


def ms(seconds):
    """Seconds to milliseconds, rounded up to 100 ms."""
    return int(seconds * 10 + 0.999) * 100


def write_header(rows):
    lines = ["// Generated by tools/build_zone_objectives.py from the entity catalogs - DO NOT EDIT",
             "// Hold the Zone opens the MP maps: per map the objective steps in the map's own",
             "// order (the messages the map sends when that objective is done), what the final",
             "// objective's start would open, the messages dropped while a Zone visit runs",
             "// (the final objective and the round's end) and every message the plan reaches",
             "// through the map's relays (kept local to each machine). Index = coop_mpmap::kMapTable.",
             "// analysis/HOLD_THE_ZONE_OBJECTIVES_2026-10-12.md",
             "#pragma once", "#include <cstdint>", "", "namespace coop_zoneobj {",
             "struct Message { uint32_t hash; const char* name; };",
             "struct Step { const char* label; const Message* messages; unsigned count; uint32_t tailMs; };   // tailMs: its relays' last heard message",
             "struct MapPlan {",
             "    const char* map;",
             "    const Step* steps; unsigned stepCount;",
             "    const Message* open; unsigned openCount;",
             "    const Message* block; unsigned blockCount;",
             "    const uint32_t* local; unsigned localCount;   // sorted",
             "    uint32_t tailMs;       // the latest first arrival of a heard local message after its step",
             "    uint32_t openTailMs;   // the open batch's relays",
             "};"]
    for row in rows:
        m, p = row["map"], row["plan"]
        for i, (label, msgs) in enumerate(p["steps"]):
            lines.append("inline constexpr Message k_%s_step%d[] = {%s};" % (m, i + 1, c_messages(msgs)))
        lines.append("inline constexpr Step k_%s_steps[] = {%s};" % (m, ", ".join(
            '{"%s", k_%s_step%d, %d, %du}' % (label, m, i + 1, len(msgs), ms(row["batch_tails"][i])) for i, (label, msgs) in enumerate(p["steps"]))))
        lines.append("inline constexpr Message k_%s_open[] = {%s};" % (m, c_messages(p["open"]) or '{0u, ""}'))
        lines.append("inline constexpr Message k_%s_block[] = {%s};" % (m, c_messages(row["block"])))
        local = sorted({name_hash(n) for n in row["reach"]})
        lines.append("inline constexpr uint32_t k_%s_local[] = {" % m)
        for i in range(0, len(local), 8):
            lines.append("    " + ", ".join("0x%08Xu" % h for h in local[i:i + 8]) + ",")
        lines.append("};")
    lines.append("inline constexpr MapPlan kPlans[] = {")
    for row in rows:
        m, p = row["map"], row["plan"]
        tail = int(tail_s(row) * 1000 + 999) // 1000 * 1000
        open_tail = ms(row["batch_tails"][-1]) if p["open"] else 0
        lines.append('    {"%s", k_%s_steps, %d, k_%s_open, %d, k_%s_block, %d, k_%s_local, %d, %du, %du},' % (
            m, m, len(p["steps"]), m, len(p["open"]), m, len(row["block"]), m, len({name_hash(n) for n in row["reach"]}), tail, open_tail))
    lines.append("};")
    lines.append("constexpr unsigned kPlanCount = sizeof(kPlans) / sizeof(kPlans[0]);")
    lines.append("}  // namespace coop_zoneobj")
    OUT.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))


def main():
    args = sys.argv[1:]
    ents = load_all()
    rows, errors = check(ents)
    if args and args[0] == "--report":
        print(report(ents, rows))
        return 1 if errors else 0
    for e in errors:
        print("error:", e)
    if errors:
        return 1
    # the local lists are searched by hash: two names of one hash would be one entry
    for row in rows:
        hashes = collections.defaultdict(set)
        for n in row["reach"]:
            hashes[name_hash(n)].add(n)
        for h, ns in hashes.items():
            if len(ns) > 1:
                print("note: %s: %08X is %s" % (row["map"], h, ", ".join(sorted(ns))))
    write_header(rows)
    for row in rows:
        p = row["plan"]
        print("%-15s %d steps, %d messages, %d open, %d blocked, %d local, tail %.1f s; reached and dropped: %s" % (
            row["map"], len(p["steps"]), sum(len(s) for _, s in p["steps"]), len(p["open"]), len(row["block"]), len(row["reach"]),
            tail_s(row), ", ".join(row["hit"]) or "-"))
    print("written", OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
