"""Build src/generated_arenas.h from the arena recordings in arenas/*.txt.

A recording comes from the co-op plugin's arena recorder ([Arena] Recorder,
plugins\\DS2Arenas\\*.txt), copied here as arenas/<name>.txt with a first line
"label=<text>" (the name is the file name, it must start with "arena_").

For each arena:
  - stream sets: the start area and every set activated during the walk;
    members come from the catalog (CMD_Set1..n, CMD_IncludedStreamSet followed)
    and from the recording;
  - doors: every StandardDoor of the chapter within 5 m of the walk. A door is
    inner (left alone) only when the walk passed both of its sides, it has no
    lock fields and its stream sets are arena sets; every other one is locked;
  - spots (End key) and the walk samples, for the survival director's spawn
    area and the respawn;
  - rooms the level loads hidden for a story event (a hallucination's "_horror"
    copy of a room) are no arena rooms: the game keeps them hidden (user, 16.10.:
    the fake pieces of the ch03 hallucination stood in the room);
  - save stations (SaveKiosk), stores (ShipStore) and workbenches (UpgradeBench) within
    15 m of the walk are not created (user, 16.10.: none of them in an arena, none in
    view behind a locked door; a store has no switch of its own to lock it);
  - semiconductors (the "... Treasure" PowerUps and Peng) and text and audio logs
    (LogEntry) in the arena's rooms or within 15 m of the walk are not created, and
    their templates are refused at the spawn filter (user, 16.10.);
  - the story logic inside the arena (the walk's rooms): triggers, scripted
    sequences, story spawners and hacking stations are not created at all,
    enemies placed there and the targets of those spawners are refused at the
    spawn filter (their spawn records stay);
  - per stream set the indices of its members, and per walk sample the set that
    was active there (the last one activated), for "streaming=sets".
Optional lines in an arena file: "streaming=sets" (only the stream sets the
players stand in are kept loaded; each set already holds its neighbour rooms),
"block=<GUID>" (an entity not created, e.g. a lift's switch), "doors=open"
(every door that is not locked is opened and never closes), "exclude=<Family>[,...]"
(survival families the director never spawns there, e.g. Brute in narrow rooms),
"open=<GUID>" (a door that stays an inner door although it has lock fields or was not
walked through, user 17.10.: the mining hall's repair room), "allow=<GUID>" (an entity the
story rules would block that stays, e.g. a lift's switch
that is also a story spawner's target), "skip=<from>-<to>" (seconds of the recording that
are no arena: a checkpoint load put the player behind a door that is locked now),
"npc_stream=<name>" (a chapter NPC stream loaded with the arena so the main stream's
vent spawners whose templates live there work for the survival director; its
neutral NPCs and corpse props are not created), "spawns=vents" (the director
spawns only through vents there) and "heights=any|same|notabove" (where the director
may spawn relative to the players: any height in the arena's rooms, the default;
only on a floor a player stands on; nothing above the players) and "maxalive=<1-30>"
(enemies alive at once for one player in this arena, instead of [Survival] MaxAlive;
each further player still adds MaxAlivePerPlayer, and a wave's own lower MaxAlive counts).

usage: python tools/build_arenas.py
"""
import collections
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from ds2_paths import WORK  # noqa: E402
from ds2_assets import assets  # noqa: E402


# A PC .str reader of its own: nothing from the modding tools folder may run (it was ds2str).
def refpack_decompress(data):
    hdr = struct.unpack_from(">H", data, 0)[0]
    p = 2
    if hdr & 0x0100:
        p += 4 if hdr & 0x8000 else 3
    if hdr & 0x8000:
        size = struct.unpack_from(">I", data, p)[0]
        p += 4
    else:
        size = data[p] << 16 | data[p + 1] << 8 | data[p + 2]
        p += 3
    out = bytearray()
    while p < len(data):
        b0 = data[p]
        if b0 < 0x80:
            b1 = data[p + 1]
            p += 2
            lit, ln, dist = b0 & 3, ((b0 >> 2) & 7) + 3, ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:
            b1, b2 = data[p + 1], data[p + 2]
            p += 3
            lit, ln, dist = b1 >> 6, (b0 & 0x3F) + 4, ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:
            b1, b2, b3 = data[p + 1], data[p + 2], data[p + 3]
            p += 4
            lit, ln, dist = b0 & 3, ((b0 & 0x0C) << 6) + b3 + 5, ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:
            p += 1
            lit, ln, dist = ((b0 & 0x1F) << 2) + 4, 0, 0
        else:
            p += 1
            out += data[p:p + (b0 & 3)]
            break
        out += data[p:p + lit]
        p += lit
        if ln:
            s = len(out) - dist
            if s < 0:
                raise ValueError("RefPack distance before the start")
            if dist >= ln:
                out += out[s:s + ln]
            else:
                for i in range(ln):
                    out.append(out[s + i])
    if len(out) != size:
        raise ValueError("RefPack size %d, header says %d" % (len(out), size))
    return bytes(out)


class StreamFile:
    """The parts of a PC stream set that ds2_assets.assets and door_matrix use."""
    end = "<"

    def __init__(self, path):
        self.data = Path(path).read_bytes()
        magic = self.data[:4]
        if magic == b"3slo":
            self.start = struct.unpack_from("<I", self.data, 4)[0]
        elif magic == b"COHS":
            self.start = 0
        else:
            raise SystemExit("%s: no PC stream set (%r)" % (path, magic))

    @staticmethod
    def tag(raw):
        return raw[::-1]

    def chunks(self):
        pos, d = self.start, self.data
        while pos + 8 <= len(d):
            size = struct.unpack_from("<I", d, pos + 4)[0]
            if size < 8 or pos + size > len(d):
                break
            yield pos, self.tag(d[pos:pos + 4]), d[pos + 8:pos + size]
            pos += size

    def asset_payload(self, chunks, declared=None):
        # SDAT carries no size, Rpak does; the last piece is padded past the declared size
        out = bytearray()
        for body in chunks:
            inner = self.tag(body[:4])
            if inner == b"Rpak":
                n = struct.unpack_from("<I", body, 4)[0]
                out += refpack_decompress(body[8:8 + n])
            elif inner == b"SDAT":
                out += body[4:]
            else:
                raise SystemExit("unexpected %r piece in a PC stream set" % inner)
        if declared is not None and declared <= len(out):
            del out[declared:]
        return bytes(out)

CATALOG = ROOT / "analysis" / "catalog" / "entities.jsonl"
OUT = ROOT / "src" / "generated_arenas.h"
DOOR_REACH = 5.0       # a door this close to the walk belongs to the arena
DOOR_HEIGHT = 3.0
BOTH_SIDES = 2.5       # walk samples this close on both sides: the walk went through
LOCK_FIELDS = ("CMD_DoorStartLocked", "CMD_LockDoor", "CMD_UnlockDoor")
SET_FIELDS = ("CMD_OpenFrontStreamSet", "CMD_OpenBackStreamSet", "CMD_LeaveFrontStreamSet", "CMD_LeaveBackStreamSet")
GUID = re.compile(r"[0-9A-F]{8}-[0-9A-F]{8}-[0-9A-F]{8}-[0-9A-F]{8}")
# Not created inside an arena (kind 1): the story logic.
BLOCK_CREATE = {"TriggerPhysical", "TriggerAuto", "TriggerLOS", "ScriptedSequence", "HackingStationMiniGame"}
# World plumbing among those triggers stays (user, 16.10.: the Ishimura lifts did not work):
# a trigger whose every message switches lights or shadows, culling, a lift's switch
# (can_use_switch: the switch works only with the player in the car), the chase camera
# or audio is created. Culling stays blocked with doors=open: the campaign culls rooms
# behind doors that close, and a held door would look into nothing.
PLUMBING = re.compile(r"(^|_)(lt|lut|lights?|shad|shadow|chasecam|aud)(_|$)|_shad_|use_switch|^aud_|lights_(on|off)")
CULLING = re.compile(r"cull")
# The campaign darkens a room it does not expect the player back in (user, 16.10.: the
# church sanctuary went dark). A one-shot trigger (a broadcast limit) that switches
# lights off is story, not plumbing; a door's close managers (*_mm_front_close,
# *_mm_back_close) switch the room behind the door off and cull it, and only the other
# door side's open manager switches it on again, which the campaign never needed.
# Both are not created: in an arena every room is walked both ways.
DARKENS = re.compile(r"(^|_)(off|hide|fade_out)_*$")
DOOR_CLOSE = re.compile(r"_mm_(front|back)_close$")
DARK_CLASSES = {"MultiManager"}
# Created, but their own targets are refused (kind 2): with the triggers gone they
# idle, and the survival director needs them as engines inside the arena. Without
# them the area rule picker took an elevator spawner outside (ch04, 09.10.).
ENGINE_CLASSES = {"FuncSpawn"}
KIOSK_CLASSES = {"SaveKiosk", "ShipStore", "UpgradeBench"}
# Valuables and logs (user, 16.10.: no semiconductors, text or audio logs in an arena).
# A placed one points at its global template (CMD_SetSharedPickupItemGUID), which carries
# the name and the log type. The templates get kind 2 in every arena: special enemies'
# drop lists (Brute, Guardian, Nest, Divider core, Tripod), two containers and a spawner
# make them at run time; a refused drop falls through to the next item or to nothing.
LOOT_CLASSES = {"PowerUp", "LogEntry"}
VALUABLE_NAME = re.compile(r"Treasure$|^Peng$")
LOG_TYPES = {"TEXT": "text log", "AUDIO": "audio log"}
DROPPERS = {"ItemDropList", "StandardContainer", "BreakableContainer"}
DROP_FIELDS = re.compile(r"CMD_Item\d+|CMD_SetDropItemGUID|CMD_SetSpawnTarget")
NPC_STREAM_BLOCK = {"NeutralNPC", "DismemberableProp"}   # placed things of an npc_stream= stream
HIDDEN_STREAMS = ("_horror",)
TOCS = ROOT / "analysis" / "stock_toc_dependencies.json"
# the runtime's tables (runtime_coop_arena.inc kMaxMembers, kMaxDoors; set masks are 32 bit)
MAX_MEMBERS, MAX_DOORS, MAX_SETS = 96, 32, 32
KIOSK_REACH, KIOSK_HEIGHT = 15.0, 6.0
ENEMY_CLASSES = {line.split("\t", 1)[0] for line in (ROOT / "analysis" / "catalog" / "enemy_classes.tsv").read_text(encoding="utf-8").splitlines()[1:] if line}
# The arena area for the story logic: the walk's box plus a margin, near a walk sample.
BLOCK_REACH, BLOCK_HEIGHT, BLOCK_MARGIN = 7.0, 3.5, 2.0


def survival_families():
    """The family names of survival::kFamilies in src/survival.h."""
    text = (ROOT / "src" / "survival.h").read_text(encoding="utf-8")
    return re.findall(r'\{"(\w+)", Role::', text)


def parse_recording(path):
    rec = {"name": path.stem, "label": path.stem, "sets": [], "members": [], "spots": [], "walk": [], "walk_sets": [], "streaming": "all", "hand_blocks": [],
           "doors": "auto", "exclude": [], "allow": [], "skips": [], "npc_streams": [], "spawns": "any", "heights": "any", "nogo": [], "open": [], "maxalive": 0}
    label = None
    for line in path.read_text(encoding="utf-8").splitlines():
        stamp = re.match(r"t=([\d.]+) ", line)
        if stamp and any(a <= float(stamp.group(1)) <= b for a, b in rec["skips"]):
            continue
        if "=" in line and not line.startswith(("t=", "#", "set=", "member=")):
            key, _, value = line.partition("=")
            if key in ("label", "map", "stream", "checkpoint"):
                rec[key] = value.strip()
            elif key == "area_at_start":
                rec["start"] = value.split()[0]
                label = rec["start"]
            elif key == "streaming":
                if value.strip() not in ("all", "sets"):
                    raise SystemExit("%s: streaming= takes all or sets" % path)
                rec["streaming"] = value.strip()
            elif key == "block":
                if not GUID.fullmatch(value.strip()):
                    raise SystemExit("%s: block= needs a GUID: %s" % (path, line))
                rec["hand_blocks"].append(value.strip())
            elif key == "npc_stream":
                if not re.fullmatch(r"[a-z0-9_]+", value.strip()):
                    raise SystemExit("%s: npc_stream= takes a stream name: %s" % (path, line))
                rec["npc_streams"].append(value.strip())
            elif key == "heights":
                if value.strip() not in ("any", "same", "notabove"):
                    raise SystemExit("%s: heights= takes any, same or notabove" % path)
                rec["heights"] = value.strip()
            elif key == "maxalive":
                if not re.fullmatch(r"\s*\d+\s*", value) or not 1 <= int(value) <= 30:
                    raise SystemExit("%s: maxalive= takes a number from 1 to 30: %s" % (path, line))
                rec["maxalive"] = int(value)
            elif key == "spawns":
                if value.strip() not in ("any", "vents"):
                    raise SystemExit("%s: spawns= takes any or vents" % path)
                rec["spawns"] = value.strip()
            elif key == "skip":
                span = re.fullmatch(r"\s*([\d.]+)\s*-\s*([\d.]+)\s*", value)
                if not span:
                    raise SystemExit("%s: skip= takes <from>-<to> in seconds: %s" % (path, line))
                rec["skips"].append((float(span.group(1)), float(span.group(2))))
            elif key == "allow":
                if not GUID.fullmatch(value.strip()):
                    raise SystemExit("%s: allow= needs a GUID: %s" % (path, line))
                rec["allow"].append(value.strip())
            elif key == "open":
                if not GUID.fullmatch(value.strip()):
                    raise SystemExit("%s: open= needs a door GUID: %s" % (path, line))
                rec["open"].append(value.strip())
            elif key == "nogo":
                rec["nogo"].append(value.strip())   # read by other tools, nothing here
                continue
            elif key == "doors":
                if value.strip() not in ("auto", "open"):
                    raise SystemExit("%s: doors= takes auto or open" % path)
                rec["doors"] = value.strip()
            elif key == "exclude":
                known = survival_families()
                for family in (v.strip() for v in value.split(",") if v.strip()):
                    match = [k for k in known if k.lower() == family.lower()]
                    if not match:
                        raise SystemExit("%s: exclude=%s is no survival family (%s)" % (path, family, ", ".join(known)))
                    if match[0] not in rec["exclude"]:
                        rec["exclude"].append(match[0])
        if " activate set=" in line:
            label = re.search(r"set=(" + GUID.pattern + ")", line).group(1)
        m = re.search(r"\(([-\d.]+) ([-\d.]+) ([-\d.]+)\)", line)
        point = tuple(float(v) for v in m.groups()) if m else None
        if " spot " in line and point:
            rec["spots"].append(point)
        if point and (" pos " in line or " spot " in line or "player=" in line or "player_at_start" in line):
            rec["walk"].append(point)
            rec["walk_sets"].append(label)
        if " activate set=" in line:
            s = re.search(r"set=(" + GUID.pattern + ")", line).group(1)
            if s not in rec["sets"]:
                rec["sets"].append(s)
            tail = line.split("members=", 1)[1].split(" ", 1)
            for g in GUID.findall(tail[1] if len(tail) > 1 else ""):
                if g not in rec["members"]:
                    rec["members"].append(g)
    if rec.get("start") and rec["start"] not in rec["sets"]:
        rec["sets"].insert(0, rec["start"])
    for key in ("map", "stream", "checkpoint"):
        if key not in rec:
            raise SystemExit("%s: no %s= line" % (path, key))
    if not rec["name"].startswith("arena_"):
        raise SystemExit("%s: the file name must start with arena_" % path)
    return rec


def fields(entity):
    out = []
    for im in entity["images"]:
        for f in im.get("fields", []):
            out.append((f.get("name") or "", f.get("string_candidate") or f.get("guid_candidate") or f.get("value")))
    return out


loot = {}           # GUID: "semiconductor", "text log" or "audio log" (placed ones and templates)
runtime_loot = {}   # template GUID: label, refused at the spawn filter in every arena


def loot_kind(e, logic):
    f = dict(fields(e))
    shared = logic.get(f.get("CMD_SetSharedPickupItemGUID"))
    sf = dict(fields(shared)) if shared else {}
    if e["class"] == "PowerUp":
        name = sf.get("CMD_SetName") or f.get("CMD_SetName")
        return "semiconductor" if isinstance(name, str) and VALUABLE_NAME.search(name) else None
    return LOG_TYPES.get(f.get("CMD_SetType") or sf.get("CMD_SetType"))


def load_catalog():
    sets, doors, logic = {}, {}, {}
    wanted = BLOCK_CREATE | ENGINE_CLASSES | ENEMY_CLASSES | KIOSK_CLASSES | NPC_STREAM_BLOCK | LOOT_CLASSES | DARK_CLASSES
    quoted = ['"%s"' % c for c in wanted | DROPPERS | {"StreamSet", "StandardDoor"}]
    dropped = set()
    for line in open(CATALOG, encoding="utf-8"):
        if not any(q in line for q in quoted):
            continue
        e = json.loads(line)
        cls = e.get("class")
        if cls == "StreamSet" and e["guid"] not in sets:
            sets[e["guid"]] = e
        elif cls == "StandardDoor" and e["guid"] not in doors:
            doors[e["guid"]] = e
        elif cls in wanted and e["guid"] not in logic:
            logic[e["guid"]] = e
        if cls in DROPPERS or cls in ENGINE_CLASSES:
            dropped.update(v for k, v in fields(e) if DROP_FIELDS.fullmatch(k) and isinstance(v, str) and GUID.fullmatch(v))
    loot.clear()
    runtime_loot.clear()
    for guid, e in logic.items():
        kind = loot_kind(e, logic) if e["class"] in LOOT_CLASSES else None
        if not kind:
            continue
        loot[guid] = kind
        # every valuable template; a log template only where a list, container or spawner names it
        if "global_assets" in e["archive"].lower() and (kind == "semiconductor" or guid in dropped):
            runtime_loot[guid] = kind
    return sets, doors, logic


def set_members(sets, guid, seen=None):
    seen = seen if seen is not None else set()
    if guid in seen or guid not in sets:
        return []
    seen.add(guid)
    out = []
    for name, value in fields(sets[guid]):
        if re.fullmatch(r"CMD_Set\d+", name) and isinstance(value, str) and GUID.fullmatch(value):
            out.append(value)
        elif name == "CMD_IncludedStreamSet" and isinstance(value, str) and GUID.fullmatch(value):
            out.extend(set_members(sets, value, seen))
    return out


payloads = {}


def door_matrix(e):
    key = (e["archive"], e["asset"])
    if key not in payloads:
        stream = StreamFile(WORK / e["archive"])
        payloads[key] = next((stream.asset_payload(c, m["size"]) for m, c in assets(stream) if m["path"] == e["asset"]), None)
    data, off = payloads[key], e.get("transform_offset", 0)
    if not data or not off or off + 64 > len(data):
        return None
    return struct.unpack_from("<16f", data, off)


def dist(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def chapter_of(sets, guids):
    for g in guids:
        if g in sets:
            m = re.search(r"\\(ch\d+_[a-z0-9]+)\\", sets[g]["archive"].lower())
            if m:
                return m.group(1)
    return None


def doors_for(rec, doors, arena_sets, chapter):
    walk = rec["walk"]
    out = []
    for guid, e in sorted(doors.items()):
        if chapter not in e["archive"].lower():
            continue
        m = door_matrix(e)
        if not m:
            continue
        p = (m[12], m[13], m[14])
        near = min(dist(p, q) for q in walk)
        if near > DOOR_REACH or min(abs(p[1] - q[1]) for q in walk) > DOOR_HEIGHT:
            continue
        f = dict(fields(e))
        both = False
        for row in (0, 8):
            ax = (m[row], m[row + 1], m[row + 2])
            if abs(ax[1]) > 0.5:
                continue
            front = (p[0] + 2.0 * ax[0], p[1] + 1.0, p[2] + 2.0 * ax[2])
            back = (p[0] - 2.0 * ax[0], p[1] + 1.0, p[2] - 2.0 * ax[2])
            if min(dist(front, q) for q in walk) <= BOTH_SIDES and min(dist(back, q) for q in walk) <= BOTH_SIDES:
                both = True
        # the axis a player crosses the door along: its forward row (every door the walks went
        # through crossed along row 8; walks past a door spread along row 0, 16.10.) and the
        # side the arena is on: +1/-1 when three quarters of the walk near it is on one side
        axis, inside = (0.0, 0.0), 0
        close = [q for q in walk if ((q[0] - p[0]) ** 2 + (q[2] - p[2]) ** 2) ** 0.5 <= 6.0 and abs(q[1] - p[1]) <= DOOR_HEIGHT]
        length = (m[8] ** 2 + m[10] ** 2) ** 0.5
        if abs(m[9]) <= 0.5 and length >= 0.5:
            axis = (m[8] / length, m[10] / length)
            dots = [(q[0] - p[0]) * axis[0] + (q[2] - p[2]) * axis[1] for q in close]
            plus, minus = sum(1 for d in dots if d > 0.5), sum(1 for d in dots if d < -0.5)
            inside = 1 if plus and plus >= 3 * minus else -1 if minus and minus >= 3 * plus else 0
        lock = [k for k in LOCK_FIELDS if k in f]
        foreign = [f[k] for k in SET_FIELDS if k in f and f[k] not in arena_sets]
        if guid in rec.get("open", []):
            why = None
        elif lock:
            why = "lock fields " + "/".join(k[4:] for k in lock)
        elif foreign:
            why = "opens sets outside the arena"
        elif not both:
            why = "the walk did not pass both sides"
        else:
            why = None
        opened = guid in rec.get("open", [])
        out.append({"guid": guid, "pos": p, "lock": why is not None, "why": why or ("open by hand" if opened else "walked through"), "near": near, "axis": axis,
                    "inside": inside})
    return out


def in_area(rec, p):
    xs, ys, zs = zip(*rec["walk"])
    if not (min(xs) - BLOCK_MARGIN <= p[0] <= max(xs) + BLOCK_MARGIN and min(zs) - BLOCK_MARGIN <= p[2] <= max(zs) + BLOCK_MARGIN and
            min(ys) - BLOCK_HEIGHT <= p[1] <= max(ys) + BLOCK_HEIGHT):
        return False
    return any(((p[0] - q[0]) ** 2 + (p[2] - q[2]) ** 2) ** 0.5 <= BLOCK_REACH and abs(p[1] - q[1]) <= BLOCK_HEIGHT for q in rec["walk"])


# kind 1: not created; kind 2: its record stays, the spawn filter refuses the entity
def plumbing(entity, cull_ok):
    """True for a trigger whose every message is world plumbing (lights, culling, lift switch, camera, audio)."""
    msgs = [v for k, v in fields(entity) if k.endswith("Message") and isinstance(v, str) and not GUID.fullmatch(v)]
    if not msgs:
        return False
    for msg in msgs:
        core = re.sub(r"^X[0-9A-Z]+_[0-9A-F]+_[0-9A-F]+_v\d+_", "", msg).lower()
        if CULLING.search(core):
            if not cull_ok:
                return False
            continue
        if not PLUMBING.search(core):
            return False
    return True


def core_name(msg):
    return re.sub(r"^X[0-9A-Z]+_[0-9A-F]+_[0-9A-F]+_v\d+_", "", msg).lower()


def darkens(msg):
    """Lights off or a room culled; a shadow or post effect switch only costs detail (several shadows at once cost frames)."""
    core = core_name(msg)
    return bool((PLUMBING.search(core) or CULLING.search(core)) and DARKENS.search(core) and "shad" not in core and "fx" not in core)


def one_shot_dark(entity):
    """A trigger broadcast that switches lights off once (CMD_SetBroadcast<n>Limit)."""
    f = dict(fields(entity))
    for n in range(1, 9):
        msg, limit = f.get("CMD_SetBroadcast%dMessage" % n), f.get("CMD_SetBroadcast%dLimit" % n)
        if isinstance(msg, str) and not GUID.fullmatch(msg) and darkens(msg) and isinstance(limit, int) and limit > 0:
            return msg
    return None


def door_close_dark(entity):
    """A door's close manager that switches lights off or culls: its darkening targets."""
    f = fields(entity)
    name = dict(f).get("CMD_SetTargetName")
    if not isinstance(name, str) or not DOOR_CLOSE.search(name.lower()):
        return []
    return [v for k, v in f if re.fullmatch(r"CMD_SetTarget\d+", k) and isinstance(v, str) and not GUID.fullmatch(v) and darkens(v)]


def near_walk(rec, p):
    return any(((p[0] - q[0]) ** 2 + (p[2] - q[2]) ** 2) ** 0.5 <= KIOSK_REACH and abs(p[1] - q[1]) <= KIOSK_HEIGHT for q in rec["walk"])


def blocks_for(rec, logic, chapter, rooms=frozenset()):
    """rooms: the TOC names (lower case) of the arena's member streams."""
    out, targets = {}, {}
    rec["kept_plumbing"] = 0
    rec["loot"] = []
    rec["dark"] = []
    npc_archives = tuple("\\" + s + ".str" for s in rec.get("npc_streams", []))
    for guid, e in sorted(logic.items()):
        if chapter not in e["archive"].lower():
            continue
        cls = e["class"]
        if cls in LOOT_CLASSES:
            if guid not in loot:
                continue
            room = e["archive"].lower().split("\\", 1)[-1][:-4] in rooms
            m = door_matrix(e)
            p = (m[12], m[13], m[14]) if m else None
            if room or (p and near_walk(rec, p)):
                out[guid] = (1, loot[guid])
                rec["loot"].append((guid, loot[guid], p, "arena room" if room else "near the walk", e["spawn_flags"]))
            continue
        if npc_archives and e["archive"].lower().endswith(npc_archives) and cls in NPC_STREAM_BLOCK:
            out[guid] = (1, cls)
            continue
        m = door_matrix(e)
        if not m:
            continue
        if cls in KIOSK_CLASSES:
            if near_walk(rec, (m[12], m[13], m[14])):
                out[guid] = (1, cls)
            continue
        if not in_area(rec, (m[12], m[13], m[14])):
            continue
        if cls in BLOCK_CREATE:
            if cls.startswith("Trigger") and plumbing(e, rec["doors"] != "open"):
                once = one_shot_dark(e)
                if not once:
                    rec["kept_plumbing"] += 1
                    continue
                rec["dark"].append((guid, "one-shot trigger", (m[12], m[13], m[14]), [once]))
            out[guid] = (1, cls)
        elif cls in DARK_CLASSES:
            dark = door_close_dark(e)
            if dark:
                out[guid] = (1, "door close darkens")
                rec["dark"].append((guid, "door close manager", (m[12], m[13], m[14]), dark))
        elif cls in ENGINE_CLASSES:
            t = dict(fields(e)).get("CMD_SetSpawnTarget")
            if isinstance(t, str) and GUID.fullmatch(t):
                targets[t] = "target of " + guid[:17]
        elif cls in ENEMY_CLASSES:
            out[guid] = (2, cls)
    for g, kind in runtime_loot.items():
        out.setdefault(g, (2, kind + " template"))
    for t, why in targets.items():
        if t not in out:
            out[t] = (2, "spawn target")
    for g in rec["hand_blocks"]:
        out[g] = (1, "blocked by hand")
    for g in rec["allow"]:
        if out.pop(g, None):
            print("   allowed by hand: %s" % g)
        else:
            print("   allow=%s: not blocked anyway" % g)
    return out


def stream_hash(g):
    """coop_mpmap::StreamHash: the TOC identity hash of a stream GUID."""
    words = [int(w, 16) for w in g.split("-")]
    h = words[0]
    for w in words[1:]:
        h = (h * 33 + w) & 0xFFFFFFFF
    return h


stream_name_cache = {}


def stream_name(g):
    if not stream_name_cache:
        for toc in json.loads(TOCS.read_text(encoding="utf-8"))["tocs"]:
            for e in toc["entries"]:
                stream_name_cache.setdefault(int(e["identity"]["hash"], 16), e["identity"]["name"])
    return stream_name_cache.get(stream_hash(g), "")


def c_guid(g):
    return "{" + ",".join("0x%sU" % w for w in g.split("-")) + "}"


def c_float(v):
    return "%.2ff" % v


def main():
    recordings = sorted((ROOT / "arenas").glob("arena_*.txt"))
    if not recordings:
        raise SystemExit("no arenas/arena_*.txt")
    sets, doors, logic = load_catalog()
    blocks, rows = [], []
    for path in recordings:
        rec = parse_recording(path)
        members = []
        for s in rec["sets"]:
            for g in set_members(sets, s):
                if g not in members:
                    members.append(g)
        only_recorded = [g for g in rec["members"] if g not in members]
        members += only_recorded
        # npc_stream=: the stream's GUID from the chapter's stream sets, by its TOC name
        npc_members = []
        if rec["npc_streams"]:
            chapter_sets = [g for g, e in sets.items() if chapter_of(sets, [g]) == chapter_of(sets, rec["sets"])]
            for name_wanted in rec["npc_streams"]:
                found = next((g for s in chapter_sets for g in set_members(sets, s) if stream_name(g).lower().endswith("\\" + name_wanted)), None)
                if not found:
                    raise SystemExit("%s: npc_stream=%s is in none of the chapter's stream sets" % (rec["name"], name_wanted))
                if found not in members:
                    members.append(found)
                npc_members.append(found)
                print("   npc stream %s (%s) loaded with the arena" % (name_wanted, found))
        hidden = [g for g in members if any(p in stream_name(g).lower() for p in HIDDEN_STREAMS)]
        for g in hidden:
            print("   room %s (%s) left to the game: it loads hidden for a story event" % (g, stream_name(g)))
        members = [g for g in members if g not in hidden]
        chapter = chapter_of(sets, rec["sets"])
        door_rows = doors_for(rec, doors, set(rec["sets"]), chapter) if chapter else []
        rooms = frozenset(stream_name(g).lower() for g in members) - {""}
        block_rows = blocks_for(rec, logic, chapter, rooms) if chapter else {}
        # a door only a story spawner creates never appears in the arena (ch03: a second door in a double doorway)
        spawned_doors = [d["guid"] for d in door_rows if d["guid"] in block_rows]
        if spawned_doors:
            print("   doors left out, only a story spawner creates them: %s" % ", ".join(spawned_doors))
        door_rows = [d for d in door_rows if d["guid"] not in block_rows]
        if len(members) > MAX_MEMBERS or len(door_rows) > MAX_DOORS or len(rec["sets"]) > MAX_SETS:
            raise SystemExit("%s: %d rooms, %d doors, %d sets; the runtime holds %d, %d and %d" % (
                rec["name"], len(members), len(door_rows), len(rec["sets"]), MAX_MEMBERS, MAX_DOORS, MAX_SETS))
        xs, ys, zs = zip(*rec["walk"])
        box = (min(xs), min(ys), min(zs), max(xs), max(ys), max(zs))
        n = rec["name"]
        print("%s (%s): chapter %s, %d sets, %d members (%d only in the recording), %d spots, %d walk samples" % (
            n, rec["label"], chapter, len(rec["sets"]), len(members), len(only_recorded), len(rec["spots"]), len(rec["walk"])))
        for d in door_rows:
            print("   door %s at (%.1f %.1f %.1f) %.1f m from the walk: %s (%s)" % (d["guid"], *d["pos"], d["near"], "LOCK" if d["lock"] else "inner", d["why"]))
        print("   world triggers kept (lights, culling, lift switches, camera, audio): %d" % rec.get("kept_plumbing", 0))
        print("   darkeners not created: %d" % len(rec.get("dark", [])))
        for g, what, p, msgs in rec.get("dark", []):
            print("     %s %s at (%.1f %.1f %.1f): %s" % (what, g, *p, ", ".join(core_name(v) for v in msgs)))
        found = collections.Counter(k for _, k, _, _, _ in rec.get("loot", []))
        print("   valuables and logs not created: %d semiconductor(s), %d text log(s), %d audio log(s); %d template(s) refused at the filter" % (
            found["semiconductor"], found["text log"], found["audio log"], sum(1 for _, c in block_rows.values() if c.endswith(" template"))))
        for g, k, p, where, flags in rec.get("loot", []):
            at = "(%.1f %.1f %.1f) %.1f m from the walk" % (*p, min(dist(p, q) for q in rec["walk"])) if p else "no transform"
            print("     %s %s at %s, %s%s" % (k, g, at, where, "" if flags else ", spawned by a script"))
        by_kind = collections.Counter((k, c) for k, c in block_rows.values())
        print("   story logic: " + ", ".join("%s %d (%s)" % (c, v, "not created" if k == 1 else "refused at the filter") for (k, c), v in sorted(by_kind.items())))
        ident = n.upper()
        blocks.append("inline constexpr uint32_t k%sSets[][4] = {%s};" % (ident, ",".join(c_guid(g) for g in rec["sets"])))
        blocks.append("inline constexpr uint32_t k%sMembers[][4] = {%s};" % (ident, ",".join(c_guid(g) for g in members)))
        blocks.append("inline constexpr Door k%sDoors[] = {\n%s\n};" % (ident, "\n".join(
            "    {%s, {%s}, %s, \"%s\", {%s, %s}, %d}," % (c_guid(d["guid"]), ", ".join(c_float(v) for v in d["pos"]), "true" if d["lock"] else "false", d["why"],
                                                     c_float(d["axis"][0]), c_float(d["axis"][1]), d["inside"])
            for d in door_rows) or "    {}"))
        spots = rec["spots"] or rec["walk"][:1]
        blocks.append("inline constexpr float k%sSpots[][3] = {%s};" % (ident, ",".join("{%s}" % ",".join(c_float(v) for v in s) for s in spots)))
        blocks.append("inline constexpr float k%sWalk[][3] = {%s};" % (ident, ",".join("{%s}" % ",".join(c_float(v) for v in s) for s in rec["walk"])))
        ordered = sorted(block_rows.items(), key=lambda kv: tuple(int(w, 16) for w in kv[0].split("-")))
        blocks.append("inline constexpr Block k%sBlocks[] = {\n%s\n};" % (ident, "\n".join(
            "    {%s, %d, \"%s\"}," % (c_guid(g), k, c) for g, (k, c) in ordered) or "    {}"))
        # each set's members as indices into the member list, and the set of each walk sample
        index_of = {g: i for i, g in enumerate(members)}
        starts, indices = [0], []
        for s in rec["sets"]:
            mine = [index_of[g] for g in set_members(sets, s) if g in index_of] + [index_of[g] for g in npc_members if g in index_of]
            indices.extend(sorted(set(mine)))
            starts.append(len(indices))
        walk_sets = [rec["sets"].index(s) if s in rec["sets"] else 255 for s in rec["walk_sets"]]
        blocks.append("inline constexpr uint16_t k%sSetStart[] = {%s};" % (ident, ",".join(str(v) for v in starts)))
        blocks.append("inline constexpr uint16_t k%sSetMember[] = {%s};" % (ident, ",".join(str(v) for v in indices) or "0"))
        blocks.append("inline constexpr uint8_t k%sWalkSet[] = {%s};" % (ident, ",".join(str(v) for v in walk_sets)))
        print("   streaming %s; set sizes %s; walk samples per set %s" % (rec["streaming"], [starts[i + 1] - starts[i] for i in range(len(rec["sets"]))],
                                                                     [walk_sets.count(i) for i in range(len(rec["sets"]))]))
        print("   doors %s; families left out: %s; max alive for one player %s" % (rec["doors"], ", ".join(rec["exclude"]) or "none",
                                                                             rec["maxalive"] or "from [Survival] MaxAlive"))
        rows.append("    {\"%s\", \"%s\", \"%s\", \"%s\", %s, k%sSets, %d, k%sMembers, %d, k%sDoors, %d, k%sSpots, %d, k%sWalk, %d, {%s}, k%sBlocks, %d, k%sSetStart, k%sSetMember, k%sWalkSet, %d, \"%s\", %d, %d, %d, %d}," % (
            n, rec["label"], rec["map"], rec["stream"], c_guid(rec["checkpoint"]), ident, len(rec["sets"]), ident, len(members), ident,
            len(door_rows), ident, len(spots), ident, len(rec["walk"]), ", ".join(c_float(v) for v in box), ident, len(ordered),
            ident, ident, ident, 1 if rec["streaming"] == "sets" else 0, ",".join(rec["exclude"]), 1 if rec["doors"] == "open" else 0,
            1 if rec["spawns"] == "vents" else 0, ("any", "same", "notabove").index(rec["heights"]), rec["maxalive"]))
    text = [
        "// Generated by tools/build_arenas.py from arenas/*.txt and analysis/catalog.",
        "// Do not edit; record the arena again or change the tool.",
        "#pragma once",
        "#include <cstdint>",
        "namespace ds2arena {",
        "// axis: the floor-plan direction a player crosses the door along; inside: the side the arena is on (+1/-1, 0 both or unknown)",
        "struct Door { uint32_t guid[4]; float pos[3]; bool lock; const char* why; float axis[2]; int8_t inside; };",
        "// kind 1: not created; kind 2: the record stays, the spawn filter refuses the entity. Sorted by GUID.",
        "struct Block { uint32_t guid[4]; uint8_t kind; const char* cls; };",
        "struct Arena {",
        "    const char* name; const char* label; const char* level; const char* stream; uint32_t checkpoint[4];",
        "    const uint32_t (*sets)[4]; unsigned setCount;",
        "    const uint32_t (*members)[4]; unsigned memberCount;",
        "    const Door* doors; unsigned doorCount;",
        "    const float (*spots)[3]; unsigned spotCount;",
        "    const float (*walk)[3]; unsigned walkCount;",
        "    float box[6];   // min x y z, max x y z of the walk",
        "    const Block* blocks; unsigned blockCount;",
        "    // set i's members: members[setMember[setStart[i]] .. setMember[setStart[i + 1] - 1]]; walkSet: the set active at each walk sample (255 none)",
        "    const uint16_t* setStart; const uint16_t* setMember; const uint8_t* walkSet;",
        "    uint8_t streaming;   // 0 every arena room stays loaded, 1 only the sets the players stand in",
        "    const char* exclude;   // survival families never spawned here, comma separated",
        "    uint8_t doorsOpen;     // 1: every door that is not locked is held open",
        "    uint8_t ventsOnly;     // 1: the survival director spawns only through vents (spawns=vents)",
        "    uint8_t heights;       // where it spawns: 0 any height in the rooms, 1 only on a player's floor, 2 nothing above the players",
        "    uint8_t maxAlive;      // enemies alive at once for one player (maxalive=), 0: [Survival] MaxAlive",
        "};",
        *blocks,
        "inline constexpr Arena kArenas[] = {",
        *rows,
        "};",
        "inline constexpr unsigned kArenaCount = sizeof(kArenas) / sizeof(kArenas[0]);",
        "}  // namespace ds2arena",
        "",
    ]
    OUT.write_text("\n".join(text), encoding="utf-8", newline="\n")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
