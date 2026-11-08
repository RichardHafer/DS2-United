"""Vent spawners of the multiplayer maps for the survival director.

Each MP map authors a few `spawn_slasher_N` FuncSpawns whose Zombie target
arrives through a VentMarker. The survival director drives those spawners with a
pool vent donor and rebinds the donor to the slasher's spawning component, the
same rebind the randomizer does for campaign vents. The catalog embed skips MP
rows, so their arrival keys are computed here with the same rules as
tools/embed_enemy_catalog.py, plus one looser key (see keys_for) that is also
computed for every pool vent donor.

The Outbreak maps (pdlc_mp_mall, pdlc_mp_school) have no field lists; their
rows come from the baked images (tools/mp_baked.py, see outbreak_tables).

The campaign arenas (arenas/arena_*.txt) get the same rows from the campaign:
every FuncSpawn of the arena's chapter whose target arrives through a VentMarker
that lies in the arena's rooms (tools/build_arenas.py's area test), open from
the start (see arena_tables). The arrival goes through any open marker of the
arena of the same kind (VentAnyMarker), so an arena whose rooms have vents but
no vent spawner of a kind also gets up to four spawners of that kind from the
chapter's main stream (always loaded) whose templates live in the main stream or
in an npc_stream= of the arena file (loaded with the arena; the ch01 tripod room).
A marker counts when it lies in the walk's box and within 9 m of the walk (wall
vents of a big room sit farther from where the player walked than doors do).

Writes src/generated_survival_vents.h and prints the donor match report.
"""
import csv
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
catalog = root / "analysis/catalog"
MAPS = {"mp_bombrun": "mp_bombRun", "mp_detonate": "mp_detonate", "mp_escape": "mp_escape",
        "mp_gov": "mp_gov", "mp_solar": "mp_solar"}
# targets of the direct path's engines (see below)
ENGINE_CLASSES = {"DamageableProp", "ScriptableProp"}


def map_of(archive):
    for key, name in MAPS.items():
        if "\\" + key + "\\" in archive.lower():
            return name
    return None


def decoded_fields(image):
    # Same rules as embed_enemy_catalog.decoded_fields.
    fields = []
    for field in image.get("fields", []):
        flags = field["flags"]
        if flags in (1, 16, 17) and "string_candidate" in field:
            value = field["string_candidate"]
        elif flags == 12 and "guid_candidate" in field:
            value = field["guid_candidate"]
        elif flags == 0:
            value = field["value"]
        elif flags == 7 and field["value"] in (0, 1):
            value = field["value"]
        else:
            return None
        fields.append((field["field_id"], flags, value))
    return sorted(fields)


def keys_for(entity):
    """(ventFree, ventOpen, survival) or None when the contract cannot be decoded."""
    parts = []
    spawning = 0
    for image in entity["images"]:
        name = image.get("class", "")
        if name != "DSNPCSpawningComponent" and not name.endswith("VentComponent"):
            continue
        spawning += name == "DSNPCSpawningComponent"
        fields = decoded_fields(image)
        if fields is None:
            return None
        parts.append((name, fields))
    if spawning != 1 or sum(name.endswith("VentComponent") for name, _ in parts) != 1:
        return None
    spawn = [part for part in parts if part[0] == "DSNPCSpawningComponent"]
    stripped = [(name, [f for f in fields if f[1] != 12]) for name, fields in spawn]
    if sum(len(f) - len(k) for (_, f), (_, k) in zip(spawn, stripped)) != 1:
        return None
    free = hashlib.sha256(json.dumps(["vent-free", stripped]).encode()).hexdigest()
    message_ids = {f["field_id"] for image in entity["images"]
                   if image.get("class") == "DSNPCSpawningComponent"
                   for f in image.get("fields", []) if f.get("name") == "CMD_SetSpawnDoneSendMessage"}
    opened = [(name, [f for f in fields if f[0] not in message_ids]) for name, fields in stripped]
    open_key = hashlib.sha256(json.dumps(["vent-open", opened]).encode()).hexdigest()
    # The survival key also drops the arrival animation options. The rebind hands
    # the donor the source's whole spawning component, so those options travel
    # with it like the vent and the message do; they are generic NPC spawn flags
    # (no collision, no pain interrupt, take damage without dying), not species
    # data. What is left is the mode and the activation flags.
    option_ids = {f["field_id"] for image in entity["images"]
                  if image.get("class") == "DSNPCSpawningComponent"
                  for f in image.get("fields", []) if f.get("name") == "CMD_SetSpawningAnimOptions"}
    survival = [(name, [f for f in fields if f[0] not in option_ids]) for name, fields in opened]
    survival_key = hashlib.sha256(json.dumps(["vent-survival", survival]).encode()).hexdigest()
    return free, open_key, survival_key


def words(text):
    return [int(w, 16) for w in text.split("-")]


def cguid(text):
    return "{" + ",".join(f"0x{w:08X}u" for w in words(text)) + "}"


with (catalog / "enemy_targets.tsv").open(encoding="utf-8", newline="") as stream:
    targets = list(csv.DictReader(stream, delimiter="\t"))
with (catalog / "funcspawn_targets.tsv").open(encoding="utf-8", newline="") as stream:
    spawners = list(csv.DictReader(stream, delimiter="\t"))

wanted = {}
for row in targets:
    if row["multiplayer"] != "True" or row["mode"] != "SPAWNMODE_VENT" or not map_of(row["archive"]):
        continue
    # an unresolved vent has no rebind target
    if not row["vent_guid"]:
        continue
    wanted[(row["archive"], row["asset"], int(row["entity_index"]))] = row

# The pool vent donors and the authored entity each was copied from.
donors = []
text = (root / "src/generated_vent_donors.h").read_text(encoding="utf-8")
pattern = re.compile(r"\{\{(0x[0-9A-F]+)u,(0x[0-9A-F]+)u,(0x[0-9A-F]+)u,(0x[0-9A-F]+)u\},"
                     r"\{(0x[0-9A-F]+)u,(0x[0-9A-F]+)u,(0x[0-9A-F]+)u,(0x[0-9A-F]+)u\}")
for match in pattern.finditer(text):
    donor = "-".join(f"{int(match.group(i), 16):08X}" for i in range(1, 5))
    source = "-".join(f"{int(match.group(i), 16):08X}" for i in range(5, 9))
    donors.append({"donor": donor, "source": source, "survival": ""})
# Retargeted variants (the twitcher, 17.10.) keep their source's spawning component, so they
# get the source's survival key and can come through any vent its contract fits.
variant_text = (root / "src/generated_variant_donors.h").read_text(encoding="utf-8")
variant_pattern = re.compile(r"\{\{(0x[0-9A-F]+)u,(0x[0-9A-F]+)u,(0x[0-9A-F]+)u,(0x[0-9A-F]+)u\},"
                             r"\{(0x[0-9A-F]+)u,(0x[0-9A-F]+)u,(0x[0-9A-F]+)u,(0x[0-9A-F]+)u\},"
                             r'"[A-Za-z]+","[A-Za-z0-9]+",\d+,\d+,\d+,\{[^}]*\},"([^"]+)"')
for match in variant_pattern.finditer(variant_text):
    donor = "-".join(f"{int(match.group(i), 16):08X}" for i in range(1, 5))
    source = "-".join(f"{int(match.group(i), 16):08X}" for i in range(5, 9))
    donors.append({"donor": donor, "source": source, "survival": "", "retarget": match.group(9)})
source_rows = {row["guid"].upper(): row for row in targets if row["multiplayer"] == "False"}
donor_sources = {}
for d in donors:
    row = source_rows.get(d["source"])
    d["family"] = row["class"] if row else "?"
    d["context"] = row["context"] if row else "?"
    if row:
        donor_sources.setdefault((row["archive"], row["asset"], int(row["entity_index"])), []).append(d)

def strings_of(entity, names):
    out = {}
    for image in entity.get("images", []):
        for field in image.get("fields", []):
            if field.get("name") in names and field.get("string_candidate"):
                out[field["name"]] = field["string_candidate"]
    return out


def name_hash(text):
    # the engine's message name hash: h * 0x1003F + lower-case char
    h = 0
    for c in text.lower():
        h = (h * 0x1003F + ord(c)) & 0xFFFFFFFF
    return h


# The vents' own selection window: a VentMarker takes part from
# m_activateSelection until m_deactivateSelection (the MP necro player's vent
# choice; the map's own slashers only spawn there). A vent without either
# shares the window of the vents in its culling group.
SELECT = ("m_activateSelection", "m_deactivateSelection", "m_activateLocalCulling", "CMD_SetVentType")
vent_select = {}
vent_where = {}
round_start = {}
keys = {}
with (catalog / "entities.jsonl").open(encoding="utf-8") as stream:
    for line in stream:
        entity = json.loads(line)
        key = entity["archive"], entity["asset"], entity["index"]
        if key in wanted:
            keys[key] = keys_for(entity)
        elif key in donor_sources:
            computed = keys_for(entity)
            for d in donor_sources[key]:
                d["survival"] = computed[2] if computed else ""
        name = map_of(entity["archive"])
        images = entity.get("images", [])
        if not name or not images:
            continue
        cls = images[0].get("class")
        if cls == "VentMarker":
            vent_select[entity["guid"].upper()] = (name, strings_of(entity, SELECT))
            vent_where[entity["guid"].upper()] = (entity["archive"], entity["asset"], entity.get("transform_offset", 0))
        elif cls == "MultiManager":
            targets = [f.get("string_candidate") for image in images for f in image.get("fields", [])
                       if (f.get("name") or "").startswith("CMD_SetTarget") and f.get("string_candidate")]
            # the round start: whatever is sent together with the first objective's attach
            if "obj_1_attach" in targets:
                round_start.setdefault(name, set()).update(targets)
for name in MAPS.values():
    round_start.setdefault(name, set()).add("obj_1_attach")
group_window = {}
for guid, (name, fields) in vent_select.items():
    window = (fields.get("m_activateSelection", ""), fields.get("m_deactivateSelection", ""))
    if any(window) and fields.get("m_activateLocalCulling"):
        group_window.setdefault((name, fields["m_activateLocalCulling"]), {}).setdefault(window, 0)
        group_window[(name, fields["m_activateLocalCulling"])][window] += 1


def window_of(vent):
    """(on, off, on at the round start, inherited) for a vent GUID."""
    name, fields = vent_select.get(vent.upper(), (None, {}))
    on, off = fields.get("m_activateSelection", ""), fields.get("m_deactivateSelection", "")
    inherited = False
    if not on and not off and fields.get("m_activateLocalCulling"):
        windows = group_window.get((name, fields["m_activateLocalCulling"]))
        if windows:
            on, off = max(windows.items(), key=lambda kv: kv[1])[0]
            inherited = True
    return on, off, bool(on) and on in round_start.get(name, ()), inherited


# --- the Outbreak maps --------------------------------------------------------------------
# Their entities are baked images in runtime layout (tools/mp_baked.py). A slasher
# is taken only when its spawning data equals the stock MP slashers' byte for
# byte apart from the vent (checked against the live mp_bombRun spawn_slasher_1
# packet, 12.10.2026). Its keys are then rebuilt from the stock field values,
# which reproduces the stock keys exactly. A vent's start state is its own
# !CMD_SetStartInactive: on all 120 stock vents that equals the round start rule
# above, and VentMarker::SpawnInit (0x012D5460) sets it the same way.
OUTBREAK_MAPS = {"pdlc_mp_mall": "pdlc_mp_mall", "pdlc_mp_school": "pdlc_mp_school"}
BAKED_MODE = {1: "SPAWNMODE_VENT"}
BAKED_FLAGS = {1: "SPAWNACTIVATIONFLAG_IMMEDIATE"}
BAKED_OPTIONS = {0x19: "SPAWNANIMFLAG_NOCOLLISION|SPAWNANIMFLAG_NOPAININTERRUPT|SPAWNANIMFLAG_TAKEDAMAGEDONTDIE"}
# the rest of the stock slashers' spawning data: side bytes, both anims -1, then
# no zones or messages, proximity 3.0, no exit play dead
BAKED_SPAWNING_REST = {0x18: bytes.fromhex("00010000ffffffffffffffff"),
                       0x28: bytes(0x2C) + struct.pack("<f", 3.0) + bytes(4)}
BAKED_VENT_KINDS = {0: "default_FLOOR_descriptor", 1: "Wall"}


def baked_message(mp_baked, blob, offset, what, skipped):
    """(hash, name) of a window message; None when its name cannot be read back."""
    h, name = mp_baked.message_at(blob, offset)
    if h and not name:
        skipped.append(f"{what}: message {h:08X} has no readable name")
        return None
    return h, name


def baked_window(mp_baked, vent, skipped):
    """(on, off, open at the start) names of a baked VentMarker, or None."""
    on = baked_message(mp_baked, vent["blob"], mp_baked.VENT_SELECT_ON, f"vent {vent['guid']}", skipped)
    off = baked_message(mp_baked, vent["blob"], mp_baked.VENT_SELECT_OFF, f"vent {vent['guid']}", skipped)
    if on is None or off is None:
        return None
    start_open = vent["blob"][mp_baked.VENT_START_INACTIVE] == 0
    if not start_open and not on[0]:
        skipped.append(f"vent {vent['guid']}: starts inactive and nothing opens it")
        return None
    return on[1], off[1], start_open


def outbreak_tables():
    """Spawner rows, markers and engines of the Outbreak maps, plus what was left out."""
    sys.path.insert(0, str(root / "tools"))
    import mp_baked  # noqa: E402
    out = {"rows": [], "markers": [], "engines": [], "skipped": []}
    skipped = out["skipped"]
    vents, spawners, enemies = {}, [], {}
    for e in mp_baked.iter_baked_entities({"VentMarker", "FuncSpawn", "Zombie"}):
        if e["class"] == "VentMarker":
            vents[e["guid"]] = e
        elif e["class"] == "FuncSpawn":
            spawners.append(e)
        else:
            enemies[e["guid"]] = e
    for vent in vents.values():
        name = OUTBREAK_MAPS[vent["map"]]
        kind = mp_baked.u32_at(vent["blob"], mp_baked.VENT_TYPE)
        if kind not in BAKED_VENT_KINDS:
            skipped.append(f"vent {vent['guid']}: vent type {kind} has no survival kind")
            continue
        if not vent["matrix"]:
            skipped.append(f"vent {vent['guid']}: no transform")
            continue
        window = baked_window(mp_baked, vent, skipped)
        if not window:
            continue
        on, off, start_open = window
        m = vent["matrix"]
        out["markers"].append({"map": name, "vent": vent["guid"], "wall": kind == 1, "pos": (m[12], m[13], m[14]),
                               "on": on, "off": off, "at_start": start_open, "inherited": False})
    classes = mp_baked.entity_classes()
    for spawner in spawners:
        name = OUTBREAK_MAPS[spawner["map"]]
        blob = spawner["blob"]
        target = mp_baked.guid_at(blob, mp_baked.FUNCSPAWN_TARGET)
        trigger = mp_baked.message_at(blob, mp_baked.FUNCSPAWN_TARGET_NAME)[1]
        maximum = struct.unpack_from("<i", blob, mp_baked.FUNCSPAWN_MAX)[0]
        if spawner["main"] and classes.get(target) in ENGINE_CLASSES and maximum != 1:
            out["engines"].append({"map": name, "spawner": spawner["guid"], "target": target, "cls": classes[target], "name": trigger})
        enemy = enemies.get(target)
        if not enemy:
            continue
        what = f"{name} {trigger or spawner['guid']} -> {target}"
        spawning = enemy["components"].get("DSNPCSpawningComponent")
        vent_components = [c for c in enemy["components"] if c.endswith("VentComponent")]
        if enemy["archive"].lower() != spawner["archive"].lower():
            skipped.append(f"{what}: target in another stream")
            continue
        if not spawning or len(spawning) != mp_baked.SPAWNING_SIZE or len(vent_components) != 1:
            skipped.append(f"{what}: no single spawning and vent component")
            continue
        mode = mp_baked.u32_at(spawning, mp_baked.SPAWNING_MODE)
        flags = mp_baked.u32_at(spawning, mp_baked.SPAWNING_FLAGS)
        options = mp_baked.u32_at(spawning, mp_baked.SPAWNING_OPTIONS)
        if mode not in BAKED_MODE or flags not in BAKED_FLAGS or options not in BAKED_OPTIONS:
            skipped.append(f"{what}: mode {mode}, flags {flags}, options 0x{options:X} not proven against the stock slashers")
            continue
        if any(spawning[at:at + len(want)] != want for at, want in BAKED_SPAWNING_REST.items()):
            skipped.append(f"{what}: spawning data differs from the stock slashers' defaults")
            continue
        vent_guid = mp_baked.guid_at(spawning, mp_baked.SPAWNING_VENT)
        vent = vents.get(vent_guid)
        if not vent or vent["map"] != spawner["map"]:
            skipped.append(f"{what}: vent {vent_guid or 'none'} is no VentMarker of the map")
            continue
        kind = mp_baked.u32_at(vent["blob"], mp_baked.VENT_TYPE)
        window = baked_window(mp_baked, vent, skipped) if kind in BAKED_VENT_KINDS else None
        if not window:
            skipped.append(f"{what}: its vent {vent_guid} has no usable kind or window")
            continue
        on, off, start_open = window
        contract = {"images": [
            {"class": "DSNPCSpawningComponent", "fields": [
                {"name": "CMD_SetSpawningMode", "flags": 1, "string_candidate": BAKED_MODE[mode]},
                {"name": "CMD_SetActivationFlags", "flags": 1, "string_candidate": BAKED_FLAGS[flags]},
                {"name": "CMD_SetSpawningVent", "flags": 12, "guid_candidate": vent_guid},
                {"name": "CMD_SetSpawningAnimOptions", "flags": 1, "string_candidate": BAKED_OPTIONS[options]}]},
            {"class": vent_components[0], "fields": []}]}
        for image in contract["images"]:
            for field in image["fields"]:
                field["field_id"] = name_hash(field["name"])
        computed = keys_for(contract)
        if not computed:
            skipped.append(f"{what}: keys not computable")
            continue
        out["rows"].append({
            "map": name, "spawner": spawner["guid"], "name": trigger, "target": target, "vent": vent_guid,
            "context": "SPAWNMODE_VENT|" + BAKED_VENT_KINDS[kind], "family": enemy["class"],
            "free": computed[0], "open": computed[1], "survival": computed[2],
            "on": on, "off": off, "at_start": start_open, "inherited": False,
        })
    return out


rows = []
seen = set()
for spawner in spawners:
    target = spawner["target_guid"].upper()
    for key, row in wanted.items():
        if row["guid"].upper() != target or (spawner["source_guid"], target) in seen:
            continue
        if row["archive"].lower() != spawner["archive"].lower():
            continue
        seen.add((spawner["source_guid"], target))
        computed = keys.get(key) or ("", "", "")
        on, off, at_start, inherited = window_of(row["vent_guid"])
        rows.append({
            "map": map_of(row["archive"]), "spawner": spawner["source_guid"], "name": spawner["source_name"],
            "target": row["guid"], "vent": row["vent_guid"], "context": row["context"],
            "family": row["class"], "free": computed[0], "open": computed[1], "survival": computed[2],
            "on": on, "off": off, "at_start": at_start, "inherited": inherited,
        })
outbreak = outbreak_tables()
rows += outbreak["rows"]


# --- the campaign arenas -----------------------------------------------------------------------
def arena_tables():
    """Vent rows and markers of the campaign arenas: map = the arena's name."""
    sys.path.insert(0, str(root / "tools"))
    import build_arenas as arenas  # noqa: E402
    sets, _, _ = arenas.load_catalog()
    recs = []
    for path in sorted((root / "arenas").glob("arena_*.txt")):
        rec = arenas.parse_recording(path)
        rec["chapter"] = arenas.chapter_of(sets, rec["sets"]) or ""
        if rec["chapter"]:
            recs.append(rec)
    chapters = {r["chapter"] for r in recs}

    def marker_in(rec, pos):
        xs, ys, zs = zip(*rec["walk"])
        if not (min(xs) - 1.5 <= pos[0] <= max(xs) + 1.5 and min(zs) - 1.5 <= pos[2] <= max(zs) + 1.5 and min(ys) - 4.0 <= pos[1] <= max(ys) + 8.0):
            return False
        return any(((pos[0] - q[0]) ** 2 + (pos[2] - q[2]) ** 2) ** 0.5 <= 9.0 and abs(pos[1] - q[1]) <= 8.0 for q in rec["walk"])

    def chapter_of_archive(archive):
        return next((c for c in chapters if "\\" + c + "\\" in archive.lower()), None)

    with (catalog / "enemy_targets.tsv").open(encoding="utf-8", newline="") as stream:
        all_targets = list(csv.DictReader(stream, delimiter="\t"))   # `targets` is reused above
    vent_rows = [row for row in all_targets if row["multiplayer"] == "False" and row["mode"] == "SPAWNMODE_VENT" and row["vent_guid"]
                 and chapter_of_archive(row["archive"])]
    by_key = {(row["archive"], row["asset"], int(row["entity_index"])): row for row in vent_rows}
    keys_here, vents_here = {}, {}
    with (catalog / "entities.jsonl").open(encoding="utf-8") as stream:
        for line in stream:
            if not any(c in line[:400] for c in chapters):
                continue
            entity = json.loads(line)
            key = entity["archive"], entity["asset"], entity["index"]
            if key in by_key:
                keys_here[key] = keys_for(entity)
            images = entity.get("images", [])
            if images and images[0].get("class") == "VentMarker" and entity.get("transform_offset"):
                kind = strings_of(entity, ("CMD_SetVentType",)).get("CMD_SetVentType", "")
                vents_here.setdefault(entity["guid"].upper(), (entity, kind))
    places = {}
    for guid, (entity, kind) in vents_here.items():
        m = arenas.door_matrix(entity)
        if m:
            places[guid] = ((m[12], m[13], m[14]), kind == "Wall")
    out = {"rows": [], "markers": [], "report": []}
    for rec in recs:
        name = rec["name"]
        mine = 0
        for (archive, asset, index), row in by_key.items():
            if chapter_of_archive(archive) != rec["chapter"]:
                continue
            place = places.get(row["vent_guid"].upper())
            if not place or not arenas.in_area(rec, place[0]):
                continue
            computed = keys_here.get((archive, asset, index))
            if not computed:
                out["report"].append(f"{name}: {row['guid']} keys not computable")
                continue
            for spawner in spawners:
                if spawner["target_guid"].upper() != row["guid"].upper() or chapter_of_archive(spawner["archive"]) != rec["chapter"]:
                    continue
                out["rows"].append({
                    "map": name, "spawner": spawner["source_guid"], "name": spawner["source_name"], "target": row["guid"],
                    "vent": row["vent_guid"], "context": row["context"], "family": row["class"],
                    "free": computed[0], "open": computed[1], "survival": computed[2],
                    "on": "", "off": "", "at_start": True, "inherited": False,
                })
                mine += 1
        marked, kinds = 0, set()
        for guid, (pos, wall) in places.items():
            entity, _ = vents_here[guid]
            if chapter_of_archive(entity["archive"]) != rec["chapter"] or not marker_in(rec, pos):
                continue
            out["markers"].append({"map": name, "vent": guid, "wall": wall, "pos": pos, "on": "", "off": "", "at_start": True, "inherited": False})
            kinds.add(wall)
            marked += 1
        # a vent kind the arena has markers of but (almost) no spawner: spawners of the chapter's main stream
        main = "\\" + rec["chapter"] + "\\" + rec["chapter"] + ".str"
        homes = (main,) + tuple("\\" + s + ".str" for s in rec.get("npc_streams", []))
        borrowed = 0
        for wall in sorted(kinds):
            have = sum(1 for r in out["rows"] if r["map"] == name and r["context"].endswith("|Wall") == wall)
            for (archive, asset, index), row in sorted(by_key.items()):
                if have >= 4:
                    break
                if not archive.lower().endswith(homes) or row["context"].endswith("|Wall") != wall:
                    continue
                computed = keys_here.get((archive, asset, index))
                spawner = next((s for s in spawners if s["target_guid"].upper() == row["guid"].upper() and s["archive"].lower().endswith(main)), None)
                if not computed or not spawner or any(r["map"] == name and r["spawner"] == spawner["source_guid"] for r in out["rows"]):
                    continue
                out["rows"].append({
                    "map": name, "spawner": spawner["source_guid"], "name": spawner["source_name"], "target": row["guid"],
                    "vent": row["vent_guid"], "context": row["context"], "family": row["class"],
                    "free": computed[0], "open": computed[1], "survival": computed[2],
                    "on": "", "off": "", "at_start": True, "inherited": False,
                })
                have += 1
                borrowed += 1
        out["report"].append(f"{name}: {mine} vent spawner rows in its rooms, {borrowed} from the chapter's main stream, {marked} vent markers in its rooms")
    return out


arena = arena_tables()
rows += arena["rows"]
rows.sort(key=lambda r: (r["map"], words(r["spawner"])))

lines = [
    "// Generated by tools/build_survival_vents.py from analysis/catalog and, for the",
    "// Outbreak maps, their baked images (tools/mp_baked.py).",
    "// The vent FuncSpawns of the multiplayer maps: spawner, its authored target,",
    "// the vent it arrives through and the arrival keys (computed as in",
    "// tools/embed_enemy_catalog.py). `survival` also leaves out the arrival",
    "// animation options, which the rebind carries over with the component.",
    "// The vent's selection window: onName opens it, offName closes it, and the",
    "// later of the two wins (VentMarker::HandleEvents). Before either it is open",
    "// without an onName or with onAtStart (onName sent at the round start; on the",
    "// Outbreak maps the vent's own !CMD_SetStartInactive). A vent with neither",
    "// message takes its culling group's (inherited).",
    "#pragma once",
    "namespace ds2 {",
    "struct SurvivalVentRow { const char* map; Guid spawner, target, vent; const char* family; const char* context;"
    " const char* ventFree; const char* ventOpen; const char* survival; const char* name;"
    " uint32_t selectOn, selectOff; bool onAtStart, inherited; const char* onName; const char* offName; };",
    "inline constexpr SurvivalVentRow kSurvivalVents[] = {",
]
for r in rows:
    on_hash = f'0x{name_hash(r["on"]):08X}u' if r["on"] else "0u"
    off_hash = f'0x{name_hash(r["off"]):08X}u' if r["off"] else "0u"
    lines.append(f'    {{"{r["map"]}",{cguid(r["spawner"])},{cguid(r["target"])},{cguid(r["vent"])},'
                 f'"{r["family"]}","{r["context"]}","{r["free"]}","{r["open"]}","{r["survival"]}","{r["name"]}",'
                 f'{on_hash},{off_hash},{"true" if r["at_start"] else "false"},{"true" if r["inherited"] else "false"},'
                 f'"{r["on"]}","{r["off"]}"}},')
lines += ["};"]

# Every VentMarker of the maps with its place (the simgroup transform, as
# tools/build_arenas.py reads it) and window: the director sends a slasher
# spawner's arrival through any open vent of the same kind (wall or floor) by
# writing the vent's GUID into the spawning header.
sys.path.insert(0, str(root / "tools"))
from ds2_paths import MODDING_TOOLS, WORK  # noqa: E402
sys.path.insert(0, str(MODDING_TOOLS))
import ds2str  # noqa: E402
from ds2_assets import assets  # noqa: E402
payloads = {}


def place_of(vent):
    archive, asset, offset = vent_where[vent]
    key = (archive, asset)
    if key not in payloads:
        stream = ds2str.StreamSet(str(WORK / archive))
        payloads[key] = next((stream.asset_payload(c, m["size"]) for m, c in assets(stream) if m["path"] == asset), None)
    data = payloads[key]
    if not data or not offset or offset + 64 > len(data):
        return None
    m = struct.unpack_from("<16f", data, offset)
    return m[12], m[13], m[14]


markers = []
for vent, (name, fields) in vent_select.items():
    place = place_of(vent)
    if not place:
        print(f"marker {name} {vent}: no transform, left out")
        continue
    on, off, at_start, inherited = window_of(vent)
    markers.append({"map": name, "vent": vent, "wall": fields.get("CMD_SetVentType", "") == "Wall", "pos": place,
                    "on": on, "off": off, "at_start": at_start, "inherited": inherited})
markers += outbreak["markers"]
markers += arena["markers"]
markers.sort(key=lambda m: (m["map"], words(m["vent"])))
lines += ["// Every VentMarker of the maps: kind, place and selection window (as above).",
          "struct SurvivalVentMarker { const char* map; Guid vent; bool wall; float x, y, z;"
          " uint32_t selectOn, selectOff; bool onAtStart, inherited; const char* onName; const char* offName; };",
          "inline constexpr SurvivalVentMarker kSurvivalVentMarkers[] = {"]
for m in markers:
    on_hash = f'0x{name_hash(m["on"]):08X}u' if m["on"] else "0u"
    off_hash = f'0x{name_hash(m["off"]):08X}u' if m["off"] else "0u"
    x, y, z = m["pos"]
    lines.append(f'    {{"{m["map"]}",{cguid(m["vent"])},{"true" if m["wall"] else "false"},{x:.3f}f,{y:.3f}f,{z:.3f}f,'
                 f'{on_hash},{off_hash},{"true" if m["at_start"] else "false"},{"true" if m["inherited"] else "false"},'
                 f'"{m["on"]}","{m["off"]}"}},')
lines += ["};"]

# Engines for direct spawns: FuncSpawns in a map's main stream (always loaded)
# whose own target is a plain prop. Such a spawner has no arrival logic of its
# own, so a forced donor simply takes the prop's place and is then moved.
engines = []
for spawner in spawners:
    name = map_of(spawner["archive"])
    if not name or spawner["target_classes"] not in ENGINE_CLASSES:
        continue
    main = "\\" + name.lower() + "\\" + name.lower() + ".str"
    if not spawner["archive"].lower().endswith(main) or spawner["max_spawn"] == "1":
        continue
    engines.append({"map": name, "spawner": spawner["source_guid"], "target": spawner["target_guid"],
                    "cls": spawner["target_classes"], "name": spawner["source_name"]})
engines += outbreak["engines"]
engines.sort(key=lambda e: (e["map"], words(e["spawner"])))
lines += ["// FuncSpawns in a map's main stream whose target is a plain prop: engines",
          "// for direct spawns (the forced donor takes the prop's place, then moves).",
          "struct SurvivalEngineRow { const char* map; Guid spawner, target; const char* targetClass; const char* name; };",
          "inline constexpr SurvivalEngineRow kSurvivalEngines[] = {"]
for e in engines:
    lines.append(f'    {{"{e["map"]}",{cguid(e["spawner"])},{cguid(e["target"])},"{e["cls"]}","{e["name"]}"}},')
lines += ["};",
          "// The same survival key for every pool vent donor (generated_vent_donors.h) and every",
          "// retargeted variant (generated_variant_donors.h, the twitcher).",
          "struct SurvivalDonorKey { Guid donor; const char* survival; };",
          "inline constexpr SurvivalDonorKey kSurvivalDonorKeys[] = {"]
for d in sorted(donors, key=lambda d: words(d["donor"])):
    if d["survival"]:
        lines.append(f'    {{{cguid(d["donor"])},"{d["survival"]}"}},  // {d["family"]} {d["context"]}{" retarget" if d.get("retarget") else ""}')
lines += ["};", "}", ""]
out = root / "src/generated_survival_vents.h"
out.write_bytes("\n".join(lines).encode("utf-8"))

print(f"wrote {out.relative_to(root)} rows={len(rows)} donors_in_pool={len(donors)} "
      f"donor_keys={sum(1 for d in donors if d['survival'])} engines={len(engines)}")
for name in sorted({e["map"] for e in engines}):
    print(f"engines {name}: {sum(1 for e in engines if e['map'] == name)}")
for r in rows:
    by = {}
    for d in donors:
        if r["survival"] and d["survival"] == r["survival"] and d["context"] == r["context"]:
            by[d["family"]] = by.get(d["family"], 0) + 1
    print(f"{r['map']} {r['name']} spawner={r['spawner']} vent={r['vent']} context={r['context']} "
          f"donors_same_contract={by or 'none'} window={r['on'] or 'start'}{' (round start)' if r['at_start'] else ''}"
          f"..{r['off'] or 'end'}{' inherited' if r['inherited'] else ''}")
for name in sorted({m["map"] for m in markers}):
    ms = [m for m in markers if m["map"] == name]
    print(f"markers {name}: {len(ms)} ({sum(m['wall'] for m in ms)} wall, {sum(not m['wall'] for m in ms)} floor)")
for name in sorted(round_start):
    print(f"round start {name}: {sorted(round_start[name])}")
stock_keys = {(r["free"], r["open"], r["survival"]) for r in rows if r["map"] not in OUTBREAK_MAPS.values()}
for name in OUTBREAK_MAPS.values():
    rs = [r for r in rows if r["map"] == name]
    ms = [m for m in markers if m["map"] == name]
    es = [e for e in engines if e["map"] == name]
    messages = {x[k] for x in rs + ms for k in ("on", "off") if x[k]}
    walls = sum(r["context"].endswith("|Wall") for r in rs)
    print(f"outbreak {name}: spawner rows={len(rs)} ({walls} wall, {len(rs) - walls} floor) markers={len(ms)} "
          f"({sum(m['wall'] for m in ms)} wall, {sum(not m['wall'] for m in ms)} floor, {sum(m['at_start'] for m in ms)} open at the start) "
          f"engines={len(es)} window messages={len(messages)} keys_as_stock={all((r['free'], r['open'], r['survival']) in stock_keys for r in rs)}")
for line in outbreak["skipped"]:
    print("outbreak left out:", line)
for line in arena["report"]:
    print("arena", line)
watched = {x[k] for x in rows + markers for k in ("on", "off") if x[k]}
print(f"vent gate watches {len(watched)} window message(s) (runtime_vent_gate.inc kMax 48)")
