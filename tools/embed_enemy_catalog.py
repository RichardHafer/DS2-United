"""Embed reviewed PC spawn metadata so installation does not depend on TSV files."""
import csv
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = root / "analysis/catalog/enemy_targets.tsv"
records = {}
# Records that never attack the player. They stay sources and are never donors.
passive_guids = set()
with source.open(encoding="utf-8", newline="") as stream:
    catalog_rows = list(csv.DictReader(stream, delimiter="\t"))
vent_keys = {(r["archive"], r["asset"], int(r["entity_index"])) for r in catalog_rows
             if r["mode"] == "SPAWNMODE_VENT" and r["multiplayer"] == "False"
             and int(r["vent_matches"]) > 0 and r["vent_type"] in
             {"Wall", "Ceiling", "Floor", "default_FLOOR_descriptor"}}
# Scripted arrivals carry an authored animation/lifecycle contract instead of a vent.
# They never reference a spawning vent, so their contract is placement independent.
SCRIPTED_MODES = {"SPAWNMODE_ANIM", "SPAWNMODE_ZOMBIE_PLAYDEAD", "SPAWNMODE_INFECTED"}
scripted_keys_wanted = {(r["archive"], r["asset"], int(r["entity_index"])) for r in catalog_rows
                        if r["mode"] in SCRIPTED_MODES and r["multiplayer"] == "False"}
arrival_keys = {}
cross_arrival_keys = {}
# The same contract with the vent reference taken out. A vent spawn's
# DSNPCSpawningComponent holds only the mode, the activation flags and a GUID
# pointing at the vent it uses - verified on all 427 extracted vent spawns, that
# GUID is the row's own vent_guid. So two vent spawns of different species differ
# in nothing but which vent they were placed on, and the runtime can point a
# donor at the source's vent. This key is what is left once that pointer is
# removed, and it is the honest test for "may this species use that vent".
vent_free_keys = {}
vent_open_keys = {}
scripted_keys = {}


def chapter_of(name):
    """The chapter an archive belongs to, e.g. ds2dat7\\ch01_hos\\... -> ch01_hos.

    A chapter's NPC design streams are loaded together - a session log shows
    npcs_observation, npcs_psycho_dev, npcs_surg_ward and npcs_tube_crawl
    resident at the same time - so a donor from the same chapter is a reasonable
    second choice when the source's own archive has none.
    """
    parts = name.split("\\")
    return parts[1] if len(parts) > 1 else ""


def archive_id(name):
    """A stable 32-bit name for the archive a record was authored in.

    Two entities from the same archive are loaded together, so a donor from the
    source's own archive is the one that is actually there when the spawn fires.
    The plan uses this to decide which donors are worth a lookup.
    """
    value = 0
    for char in name.lower():
        value = (value * 0x1003F + ord(char)) & 0xFFFFFFFF
    return value


def decoded_fields(image):
    """Return the comparable (field_id, flags, value) triples, or None when unknown."""
    fields = []
    for field in image.get("fields", []):
        flags = field["flags"]
        # 1/16/17 are self-relative string pointers and 12 is a self-relative GUID;
        # their raw value is a file offset, so only the resolved target may be compared.
        if flags in (1, 16, 17) and "string_candidate" in field:
            value = field["string_candidate"]
        elif flags == 12 and "guid_candidate" in field:
            value = field["guid_candidate"]
        # 0 is an inline dword. 7 is an inline boolean: across the whole extracted
        # corpus every flags-7 field holds 0 or 1 with value_index 255 and no
        # descriptor, and inside the spawn contract it is only the authored
        # m_ventSpawnSelectPlayerSide and CMD_SetInitialAIVentUsage switches.
        elif flags == 0:
            value = field["value"]
        elif flags == 7 and field["value"] in (0, 1):
            value = field["value"]
        else:
            return None
        # Enum descriptor indices are local to a serialized image table.
        fields.append((field["field_id"], flags, value))
    return sorted(fields)


with (root / "analysis/catalog/entities.jsonl").open(encoding="utf-8") as stream:
    for line in stream:
        entity = json.loads(line)
        key = entity["archive"], entity["asset"], entity["index"]
        vent_wanted = key in vent_keys
        scripted_wanted = key in scripted_keys_wanted
        if not vent_wanted and not scripted_wanted:
            continue
        parts = []
        valid = True
        spawning_count = 0
        for image in entity["images"]:
            name = image.get("class", "")
            if name != "DSNPCSpawningComponent" and not name.endswith("VentComponent"):
                continue
            spawning_count += name == "DSNPCSpawningComponent"
            fields = decoded_fields(image)
            if fields is None:
                valid = False
                break
            parts.append((name, fields))
        if not valid or spawning_count != 1:
            continue
        spawn = [part for part in parts if part[0] == "DSNPCSpawningComponent"]
        if vent_wanted:
            arrival_keys[key] = hashlib.sha256(json.dumps(sorted(parts)).encode()).hexdigest()
            if sum(name.endswith("VentComponent") for name, fields in parts) == 1:
                cross_arrival_keys[key] = hashlib.sha256(json.dumps(spawn).encode()).hexdigest()
                # Drop every self-relative GUID from the spawning component. On a
                # vent spawn the only one is the vent itself; a component that
                # carried a second GUID would be a different shape than the 427
                # inspected here, so it keeps its vent-bound key and nothing else.
                stripped = [(name, [f for f in fields if f[1] != 12]) for name, fields in spawn]
                if sum(len(fields) - len(kept) for (_, fields), (_, kept)
                       in zip(spawn, stripped)) == 1:
                    vent_free_keys[key] = hashlib.sha256(
                        json.dumps(["vent-free", stripped]).encode()).hexdigest()
                    # The same contract once more without the spawn-done message.
                    #
                    # That message is what a level script waits for, so it had to
                    # be part of the contract: only a donor authored with the very
                    # same message could be trusted to send it. The runtime now
                    # writes the source's message onto the replacement instead,
                    # which is what makes a looser key defensible - 26 of the 35
                    # vent spawns that no donor can reach are held back by this one
                    # field alone. It is the only message field a spawning
                    # component ever carries.
                    message_ids = {f["field_id"] for image in entity["images"]
                                   if image.get("class") == "DSNPCSpawningComponent"
                                   for f in image.get("fields", [])
                                   if f.get("name") == "CMD_SetSpawnDoneSendMessage"}
                    opened = [(name, [f for f in fields if f[0] not in message_ids])
                              for name, fields in stripped]
                    vent_open_keys[key] = hashlib.sha256(
                        json.dumps(["vent-open", opened]).encode()).hexdigest()
        if scripted_wanted:
            # The species vent components are inert for a scripted arrival, so the
            # contract is the spawning component alone: mode, both animation names,
            # animation options, activation flags and every authored message.
            scripted_keys[key] = hashlib.sha256(
                json.dumps(["scripted", spawn]).encode()).hexdigest()
with source.open(encoding="utf-8", newline="") as stream:
    for row in csv.DictReader(stream, delimiter="\t"):
        if row["multiplayer"] == "True":
            continue
        try:
            guid = tuple(int(w, 16) for w in row["guid"].split("-"))
            model = tuple(int(w, 16) for w in row["model_guid"].split("-"))
        except ValueError:
            continue
        if len(guid) != 4 or len(model) != 4 or not any(guid) or not any(model):
            continue
        ordinary = row["eligibility"] == "ordinary_candidate_unverified_swap" and not (
            row["inactive_animation"] or row["active_animation"])
        if row["eligibility"] == "non_hostile_scripted":
            passive_guids.add(guid)
        extended = row["class"] in {"Infector", "Stalker", "SwarmEnemy", "DividerCore", "DividerPart", "Hunter"} and not (
            row["inactive_animation"] or row["active_animation"])
        occurrence = row["archive"], row["asset"], int(row["entity_index"])
        arrival = arrival_keys.get(occurrence, "")
        cross_arrival = cross_arrival_keys.get(occurrence, "")
        # A scripted arrival stays refused unless the class itself is an ordinary
        # candidate. Constrained classes (Brute, Stalker, Hunter, ...) never qualify.
        scripted = scripted_keys.get(occurrence, "") if (
            row["eligibility"] == "ordinary_candidate_unverified_swap") else ""
        vent_free = vent_free_keys.get(occurrence, "")
        vent_open = vent_open_keys.get(occurrence, "")
        value = (model, row["class"], row["context"], ordinary, extended, arrival,
                 cross_arrival, scripted, vent_free, vent_open)
        # The archive is metadata, not identity: two occurrences that agree on
        # everything else are still one record. A GUID authored in two archives
        # has no single home, so its locality is dropped rather than guessed.
        archive = archive_id(row["archive"])
        chapter = archive_id(chapter_of(row["archive"]))
        if guid in records:
            prior, ambiguous, prior_archive, prior_chapter = records[guid]
            records[guid] = (prior, ambiguous or prior != value,
                             prior_archive if prior_archive == archive else 0,
                             prior_chapter if prior_chapter == chapter else 0)
        else:
            records[guid] = value, False, archive, chapter

def words(value):
    return "{" + ",".join(f"0x{word:08X}u" for word in value) + "}"

lines = ["// Generated by tools/embed_enemy_catalog.py.", "#pragma once",
         "namespace ds2 {",
         "struct CatalogRow { Guid guid, model; const char* family; const char* context; bool ordinary, ambiguous, extended; const char* arrivalKey; const char* crossArrivalKey; const char* scriptedKey; const char* ventFreeKey; const char* ventOpenKey; unsigned archive, chapter; bool passive; };",
         'inline constexpr const char* kCatalogHash = "' + hashlib.sha256(source.read_bytes()).hexdigest() + '";',
         "inline constexpr CatalogRow kEnemyCatalog[] = {"]
for guid, ((model, family, context, ordinary, extended, arrival, cross_arrival, scripted, vent_free, vent_open), ambiguous, archive, chapter) in sorted(records.items()):
    lines.append("    {" + ",".join((words(guid), words(model), json.dumps(family),
                                    json.dumps(context), str(ordinary).lower(), str(ambiguous).lower(),
                                    str(extended).lower(), json.dumps(arrival), json.dumps(cross_arrival),
                                    json.dumps(scripted), json.dumps(vent_free),
                                    json.dumps(vent_open),
                                    f"0x{archive:08X}u", f"0x{chapter:08X}u",
                                    str(guid in passive_guids).lower())) + "},")
lines += ["};", "struct VentGuidRow { Guid target, vent; };", "inline constexpr VentGuidRow kVentGuids[] = {"]
vent_guids = {}
for row in catalog_rows:
    if row["multiplayer"] == "True" or row["mode"] != "SPAWNMODE_VENT":
        continue
    try:
        guid = tuple(int(w, 16) for w in row["guid"].split("-"))
        vent = tuple(int(w, 16) for w in row["vent_guid"].split("-"))
    except ValueError:
        continue
    if len(vent) == 4 and any(vent) and guid in records and not records[guid][1]:
        vent_guids[guid] = vent
for guid, vent in sorted(vent_guids.items()):
    lines.append("    {" + words(guid) + "," + words(vent) + "},")
lines += ["};", "}", ""]
target = root / "src/generated_enemy_catalog.h"
target.write_text("\n".join(lines), encoding="utf-8")
print(f"Embedded {len(records)} records; {sum(v[1] for v in records.values())} ambiguous GUIDs; "
      f"{sum(1 for v in records.values() if not v[2])} without a single home archive; "
      f"{sum(1 for v in records.values() if not v[3])} without a single chapter")
matrix = []
for guid, (src, ambiguous, _, _unused) in records.items():
    model, family, context, ordinary, extended, arrival, cross, scripted, vent_free, vent_open = src
    if ambiguous or not (ordinary or extended) or not arrival:
        continue
    targets = []
    for donor_guid, (dst, donor_ambiguous, _, _unused_donor) in records.items():
        dm, df, dc, ordinary_d, extended_d, da, dx, ds, dv, do = dst
        if donor_ambiguous or not (ordinary_d or extended_d) or model == dm or context != dc:
            continue
        if donor_guid in passive_guids:
            continue
        if (family == df and arrival == da) or (family != df and cross and cross == dx):
            targets.append({"guid": "-".join(f"{w:08X}" for w in donor_guid), "family": df})
    if targets:
        matrix.append({"guid": "-".join(f"{w:08X}" for w in guid), "family": family,
                       "context": context, "targets": targets})
report = {"scope": "Authored same-vent candidates; runtime residency and behavior unverified",
          "source_count": len(matrix), "pairs": sum(len(r["targets"]) for r in matrix), "sources": matrix}
(root / "analysis/vent_pair_candidates.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print(f"Same-vent matrix: {report['source_count']} source records, {report['pairs']} directed pairs")
scripted_matrix = []
for guid, (src, ambiguous, _, _unused) in records.items():
    model, family, context, ordinary, extended, arrival, cross, scripted, vent_free, vent_open = src
    if ambiguous or not scripted:
        continue
    targets = []
    for donor_guid, (dst, donor_ambiguous, _, _unused_donor) in records.items():
        dm, df, dc, ordinary_d, extended_d, da, dx, ds, dv, do = dst
        # Same family only. Authored spawn animations are species specific and no
        # cross-species scripted arrival is demonstrated anywhere in the corpus.
        if donor_ambiguous or not ds or ds != scripted or df != family or dc != context or dm == model:
            continue
        if donor_guid in passive_guids:
            continue
        targets.append({"guid": "-".join(f"{w:08X}" for w in donor_guid), "family": df})
    if targets:
        scripted_matrix.append({"guid": "-".join(f"{w:08X}" for w in guid), "family": family,
                                "context": context, "targets": targets})
scripted_report = {
    "scope": "Identical authored spawning-component contracts for scripted arrivals; "
             "runtime residency and behavior unverified",
    "source_count": len(scripted_matrix),
    "pairs": sum(len(r["targets"]) for r in scripted_matrix), "sources": scripted_matrix}
(root / "analysis/scripted_pair_candidates.json").write_text(json.dumps(scripted_report, indent=2), encoding="utf-8")
print(f"Scripted-arrival matrix: {scripted_report['source_count']} source records, "
      f"{scripted_report['pairs']} directed pairs")
