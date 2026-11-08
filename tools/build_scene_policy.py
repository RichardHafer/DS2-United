"""Build the campaign scene participation table from authored message listeners."""

import json
import re
from collections import Counter, defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ISAAC = "36CAB03F-2C39E63A-45544150-3230314C"
SEQUENCES = {"ScriptedSequence", "ScriptedEventSequence"}
PLACEMENTS = {"RWS_MOVETYPE_TELEPORTALL", "RWS_MOVETYPE_TELEPORTPOS", "RWS_MOVETYPE_PULLTOPOS"}
HELMET_ANIMATIONS = {"pc_bib_helmet_close_advsuit", "pc_bib_helmet_open_advsuit"}
START_MANAGERS = {
    # Authored player pull -> completion broadcast -> scene's MultiManager.
    "31CB5378-99ACE7F1-534F5254-30314E49": ("31CB5113-00B68D4D-534F5254-30314E49", "se_01_01"),
    "39CB17CF-FDEDBB6C-4B4C4157-32345245": ("39CB17CF-FDF01DBA-4B4C4157-32345245", "igc_03_03"),
}
ELLIE_DEPART = "mm_ellie_depart"
ELLIE_ACTOR = "31CAF2D9-0A607A70-43495250-32303045"
ELLIE_PLAYER_SEQ = "31CB35A3-6C835DE4-43495250-32303045"
ELLIE_NPC_SEQ = "31CB350D-714216AF-43495250-32303045"
ELLIE_REFERENCES = {
    (ELLIE_PLAYER_SEQ, "ScriptedEventSequence", "CMD_SetTargetName"),
    (ELLIE_NPC_SEQ, "ScriptedEventSequence", "CMD_SetTargetName"),
    ("31CAF2E7-3DE763F1-43495250-32303045", "MultiManager", "CMD_SetTargetName"),
    ("58CB4A33-4B263030-30345346-58583031", "MultiManager", "CMD_SetTargetName"),
    ("34CB7D43-73547F83-46494853-30524546", "SetAudioMix", "CMD_SetTargetName"),
    ("31CB35CB-CB0F05CF-43495250-32303045", "PlayerHelmetCommand", "CMD_SetTargetName"),
    ("31CAE58D-6FCFBA3B-43524F57-31313048", "TriggerPhysical", "CMD_SetBroadcast1Message"),
}
ELLIE_SHOT = "ses_shoot_at_isaac"
SHOT_PLAYER_SEQ = "31CB4FA3-8741268A-43495250-32303045"
SHOT_NPC_SEQ = "31CB34FF-B544F8B3-43495250-32303045"
SHOT_REFERENCES = {
    (SHOT_PLAYER_SEQ, "ScriptedEventSequence", "CMD_SetTargetName"),
    (SHOT_NPC_SEQ, "ScriptedEventSequence", "CMD_SetTargetName"),
    ("52CB5460-E6739790-4D574F4C-454C4C49", "MultiManager", "CMD_SetTargetName"),
    ("46CB7CA1-197BC633-454C4C41-524F444E", "MultiManager", "CMD_SetTargetName"),
    ("31CAE567-52F55BB5-43524F57-31313048", "MultiManager", "CMD_SetTarget1"),
    ("32CB5EB0-6997FF52-41574445-30534452", "ExecuteVFX", "CMD_SetTargetName"),
    ("58CB6A7E-E4533195-4C4C4148-32393230", "DebugText", "CMD_SetTargetName"),
    *(("32CB5E16-D012A59" + n + "-41574445-30534452", "EnvironmentalHazard", "CMD_Enable") for n in "012"),
    *(("32CB5EB0-04F1712" + n + "-41574445-30534452", "Attacher", "MSG_ATTACH") for n in "012"),
}
PROMPT_CHAINS = (
    ("ses_igc_09_04_player_", "patient_struggle", "020", "030"),
    ("ss_igc_09_05_player_", "nicole_choke_struggle", "030", "020"),
)


def message_hash(name):
    result = 0
    for letter in name.lower():
        result = (result * 65599 + ord(letter)) & 0xffffffff
    return result


def classify(name, fields):
    if fields.get("CMD_SetScriptedSequenceTarget") != ISAAC:
        return "Unknown"
    animation = fields.get("CMD_SetActionAnimationString", "").lower()
    move = fields.get("CMD_SetScriptedSequenceMoveType", "")
    settings = fields.get("CMD_SetScriptedSequenceSettingsFlags", "").split("|")
    if (animation in HELMET_ANIMATIONS and "SSEQFL_REPEATABLE" in settings
            and not move and not fields.get("CMD_SetTarget")):
        return "Personal"
    # Individual deaths, hallucinations and disabled scene branches retain their existing handling.
    if name.startswith("null_") or re.search(r"(?:death|dementia|fail)", name + " " + animation):
        return "Unknown"
    # The church entry has a movement clip and an outgoing start message, but no action clip.
    entry = (not animation and move == "RWS_MOVETYPE_PULLTOPOS"
             and fields.get("CMD_SetPullToPosAnimationString")
             and fields.get("CMD_SetTarget") and re.search(r"(?:igc|se)_\d+_\d+", name))
    story_clip = re.match(r"(igc|se)_\d+_\d+_player(?:_|$)", animation)
    if move in PLACEMENTS and (story_clip or entry):
        return "Pull"
    if story_clip and story_clip[1] == "igc" and (not move or move == "RWS_MOVETYPE_INPLACEANGLES"):
        return "Gather"
    return "Unknown"


ACTOR_CLASSES = {
    "ScriptableProp", "LinearScriptableProp", "SecondaryAnimScriptableProp", "DamageableProp",
    "DismemberableProp", "NeutralNPC", "Zombie", "PackEnemy", "Leaper", "Lurker", "Puker",
    "Exploder", "Pregnant", "Stalker", "SwarmEnemy", "TubeBeetle", "Infector",
}
ACTOR_FIELDS = {
    "CMD_SetTargetName", "CMD_SetScriptedSequenceTarget", "CMD_SetActionAnimationString",
    "CMD_SetActionCount", "CMD_SetWaitAnimationString", "CMD_SetRunAnimationString",
    "CMD_SetIdleAnimationString", "CMD_SetPullToPosAnimationString", "CMD_PullTimeToPos",
    "CMD_SetArrivalDistance", "CMD_SetActionTurnSpeed", "CMD_SetScriptedSequenceMoveType",
    "CMD_SetScriptedSequenceSettingsFlags",
}
ACTOR_FLAGS = {
    "SSEQFL_REPEATABLE", "SSEQFL_NO_INTERRUPT", "SSEQFL_NO_GRAVITY_COLLISION", "SSEQFL_NO_DAMAGE",
    "SSEQFL_PERSIST", "SSEQFL_NEGATE_CHAR_PROXY_OFFSET", "SSEQFL_START_ACTION_ANIM_ON_MOVE",
}
ACTOR_MOVES = PLACEMENTS | {
    "RWS_MOVETYPE_PATHTOPOS", "RWS_MOVETYPE_INPLACEANGLES",
    "RWS_MOVETYPE_TURNTOSSEQ", "RWS_MOVETYPE_INPLACEROTANGLES",
}


def actor_target(image):
    fields = {f.get("name"): f for f in image["fields"]}
    if len(fields) != len(image["fields"]) or not fields.keys() <= ACTOR_FIELDS:
        return None
    if not (fields.get("CMD_SetActionAnimationString", {}).get("string_candidate")
            or fields.get("CMD_SetScriptedSequenceMoveType", {}).get("string_candidate")):
        return None
    if "CMD_SetScriptedSequenceSettingsFlags" in fields:
        flags = fields["CMD_SetScriptedSequenceSettingsFlags"].get("string_candidate", "").split("|")
        if not set(flags) <= ACTOR_FLAGS:
            return None
    if "CMD_SetScriptedSequenceMoveType" in fields:
        if fields["CMD_SetScriptedSequenceMoveType"].get("string_candidate") not in ACTOR_MOVES:
            return None
    target = fields.get("CMD_SetScriptedSequenceTarget", {}).get("guid_candidate")
    return target if target != ISAAC else None


def validate_ellie_departure(references, sequences):
    player = sequences.get(ELLIE_PLAYER_SEQ, {})
    npc = sequences.get(ELLIE_NPC_SEQ, {})
    if (references != ELLIE_REFERENCES or classify(ELLIE_DEPART, player) != "Pull"
            or player.get("CMD_SetScriptedSequenceMoveType") != "RWS_MOVETYPE_PULLTOPOS"
            or player.get("CMD_SetSyncAnimated0") != ELLIE_ACTOR
            or player.get("CMD_OnActionStartMessage") != "push_cam_player_bone_bound_cam_se_04_02"
            or player.get("CMD_SetTarget") != "mm_ellie_in_elevator"
            or npc.get("CMD_SetScriptedSequenceTarget") != ELLIE_ACTOR
            or npc.get("CMD_SetScriptedSequenceMoveType") != "RWS_MOVETYPE_TELEPORTALL"):
        raise ValueError("Unexpected Ellie departure graph")


def validate_ellie_shot(references, sequences, cameras):
    player = sequences.get(SHOT_PLAYER_SEQ, {})
    npc = sequences.get(SHOT_NPC_SEQ, {})
    camera = "31CB4FB0-35F4514A-43495250-32303045"
    if (references != SHOT_REFERENCES or player.get("CMD_SetScriptedSequenceTarget") != ISAAC
            or player.get("CMD_SetScriptedSequenceMoveType") or player.get("CMD_SetActionAnimationString") != "se_04_02_player_050"
            or player.get("CMD_SetLookAtEntity") != ELLIE_ACTOR or npc.get("CMD_SetScriptedSequenceTarget") != ELLIE_ACTOR
            or npc.get("CMD_SetLookAtEntity") != ISAAC or player.get("CMD_OnActionStartMessage") != "mm_se_04_02_push_cam"
            or player.get("CMD_SetTarget") != "pop_cam_se_04_02_hands_up"
            or cameras != {("MultiManager", "mm_se_04_02_push_cam", "push_cam_se_04_02_hands_up"),
                           ("PushCamera", "push_cam_se_04_02_hands_up", camera),
                           ("PopCamera", "pop_cam_se_04_02_hands_up", camera)}):
        raise ValueError("Unexpected Ellie shooting encounter graph")


def prompt_branches(nodes, prompts, results):
    branches = {}
    for prefix, outcome, success, failure in PROMPT_CHAINS:
        root = prefix + "010"
        group, entry = nodes[root]
        if ("SSEQFL_HAS_NEXT_SEQUENCE" not in entry.get("CMD_SetScriptedSequenceSettingsFlags", "").split("|")
                or entry.get("CMD_SetScriptedSequenceTarget") != ISAAC
                or (group, outcome + "_success", outcome + "_fail") not in prompts):
            raise ValueError(f"Unexpected prompt entry for {root}")
        steps = []
        for suffix, result in ((success, "_success"), (failure, "_fail")):
            cue = prefix + suffix
            sequence_group, step = nodes[cue]
            manager_group, outputs = nodes[outcome + result]
            targets = {v for k, v in outputs.items() if re.fullmatch(r"CMD_SetTarget\d+", k)}
            scripted_death = "SSEQFL_SCRIPTEDDEATH" in step.get("CMD_SetScriptedSequenceSettingsFlags", "").split("|")
            if (group != sequence_group or group != manager_group or targets & {prefix + success, prefix + failure} != {cue}
                    or step.get("CMD_SetScriptedSequenceTarget") != ISAAC or scripted_death != (result == "_fail")
                    or step.get("CMD_SetSyncAnimated0") != entry.get("CMD_SetSyncAnimated0")
                    or results[message_hash(cue)] != {"Gather"}):
                raise ValueError(f"Unexpected {result} branch for {root}")
            steps.append(cue)
        branches[root] = steps
    return branches


def is_actor_reference(cls, field):
    # Only a sequence's start input and verified message producers are permitted.
    if cls in SEQUENCES:
        return field in {"CMD_SetTargetName", "CMD_SetTarget", "CMD_OnActionStartMessage"}
    if cls == "TriggerPhysical":
        return bool(re.fullmatch(r"CMD_SetBroadcast\d+Message", field))
    if cls == "FuncSpawn":
        return field == "CMD_SetSpawnDoneMessage"
    if cls == "DSNPCSpawningComponent":
        return field == "CMD_SetSpawnDoneSendMessage"
    return False


def campaign_actor_cues(catalogs, reserved):
    classes = defaultdict(set)
    local_guids = set()
    bindings = defaultdict(set)
    for catalog in catalogs:
        with catalog.open(encoding="utf-8") as records:
            for line in records:
                record = json.loads(line)
                group = record["asset"].split("\\")[:2]
                campaign = len(group) == 2 and group[0] == "levels" and group[1].startswith("ch")
                level = (catalog, tuple(group))
                if record["images"]:
                    guid = record["guid"]
                    classes[guid].add(record["images"][0].get("class"))
                    local_guids.add((level, guid))
                for image in record["images"]:
                    if image.get("class") not in SEQUENCES:
                        continue
                    target = actor_target(image) if campaign else None
                    for field in image["fields"]:
                        if field.get("name") == "CMD_SetTargetName" and field.get("string_candidate"):
                            bindings[field["string_candidate"].lower()].add((level, target))
    candidates = {
        message_hash(name): name for name, receivers in bindings.items() if name not in reserved
        and all((level, guid) in local_guids and classes[guid] <= ACTOR_CLASSES for level, guid in receivers)
    }
    if not candidates:
        return []
    # Check every string field, including camera, trigger and player inputs with other names.
    hashes = {}
    for catalog in catalogs:
        with catalog.open(encoding="utf-8") as records:
            for line in records:
                for image in json.loads(line)["images"]:
                    for field in image["fields"]:
                        text = field.get("string_candidate")
                        if not text:
                            continue
                        name = text.lower()
                        if name not in hashes:
                            hashes[name] = message_hash(name)
                        key = hashes[name]
                        if key in candidates and (candidates[key] != name or
                                not is_actor_reference(image.get("class"), field.get("name") or "")):
                            del candidates[key]
    return sorted(candidates.items())


def main():
    results = defaultdict(set)
    names = defaultdict(set)
    graph_records = {}
    actors = set()
    church_exit = set()
    kong_ready = set()
    mixed_refs = defaultdict(set)
    mixed_sequences = {}
    cameras = set()
    prompt_names = {name for prefix, outcome, success, failure in PROMPT_CHAINS
                    for name in (prefix + "010", prefix + success, prefix + failure, outcome + "_success", outcome + "_fail")}
    prompt_nodes = {}
    prompts = set()
    graph_nodes = set(START_MANAGERS) | {manager for manager, _ in START_MANAGERS.values()}
    for folder in ("catalog", "catalog_dlc"):
        with (ROOT / "analysis" / folder / "entities.jsonl").open(encoding="utf-8") as records:
            for line in records:
                lowered = line.lower()
                check_ellie = ELLIE_DEPART in lowered or ELLIE_SHOT in lowered
                if "CMD_SetTargetName" not in line and not check_ellie and "TestInputEvent" not in line:
                    continue
                record = json.loads(line)
                group = (record["archive"], record["asset"])
                for image in record["images"]:
                    fields = {f["name"]: f.get("string_candidate") or f.get("guid_candidate") or ""
                              for f in image.get("fields", []) if f.get("name")}
                    name = fields.get("CMD_SetTargetName", "").lower()
                    cls = image.get("class")
                    if check_ellie:
                        for field, value in fields.items():
                            if value.lower() in {ELLIE_DEPART, ELLIE_SHOT}:
                                mixed_refs[value.lower()].add((record["guid"], cls, field))
                        if cls in SEQUENCES and name in {ELLIE_DEPART, ELLIE_SHOT}:
                            mixed_sequences[record["guid"]] = fields
                    if name in {"mm_se_04_02_push_cam", "push_cam_se_04_02_hands_up", "pop_cam_se_04_02_hands_up"}:
                        cameras.add((cls, name, fields.get("CMD_SetCameraTarget") or fields.get("CMD_SetTarget1")))
                    if cls == "TestInputEvent":
                        prompts.add((group, fields.get("CMD_SetSuccessMsgName"), fields.get("CMD_SetFailureMsgName")))
                    if name in prompt_names and cls in SEQUENCES | {"MultiManager"}:
                        if name in prompt_nodes:
                            raise ValueError(f"Multiple campaign branch nodes for {name}")
                        prompt_nodes[name] = group, fields
                    if not name:
                        continue
                    if (record["guid"] == "39CB17CF-FDEDBB67-4B4C4157-32345245"
                            and image.get("class") == "ScriptedEventSequence"
                            and fields.get("CMD_SetScriptedSequenceTarget") == ISAAC):
                        church_exit.add(fields.get("CMD_SetTarget", "").lower())
                    if (record["guid"] == "33CAC561-4F98F0A7-4E4F4F4E-32304E41"
                            and image.get("class") == "FuncSpawn" and name == "msg_spawn_kong_phase_1"):
                        kong_ready.add(fields.get("CMD_SetSpawnDoneMessage", "").lower())
                    key = message_hash(name)
                    names[key].add(name)
                    policy = classify(name, fields) if image.get("class") in SEQUENCES else "Unknown"
                    results[key].add(policy)
                    if record["guid"] in graph_nodes and image.get("class") in SEQUENCES | {"MultiManager"}:
                        graph_records[group, record["guid"]] = (name, fields, image)
                    if image.get("class") in SEQUENCES | {"PushCamera", "PopCamera"}:
                        actors.add((group, name))

    validate_ellie_departure(mixed_refs[ELLIE_DEPART], mixed_sequences)
    validate_ellie_shot(mixed_refs[ELLIE_SHOT], mixed_sequences, cameras)
    for name, expected, policy in ((ELLIE_DEPART, {"Unknown", "Pull"}, "Pull"), (ELLIE_SHOT, {"Unknown"}, "Gather")):
        key = message_hash(name)
        if names[key] != {name} or results[key] != expected:
            raise ValueError(f"Ambiguous Ellie encounter hash for {name}")
        results[key] = {policy}
    branches = prompt_branches(prompt_nodes, prompts, results)
    rows = [(key, next(iter(names[key])), next(iter(policies))) for key, policies in results.items()
            if len(names[key]) == 1 and len(policies) == 1 and "Unknown" not in policies]
    header = ["// Dead Space 2 United by Hafer.", "// Generated by tools/build_scene_policy.py.", "#pragma once", "#include <cstdint>", "",
              "namespace scene_catalog {",
              "enum class Policy { Unknown, Personal, Gather, Pull };",
              "inline Policy PolicyFor(uint32_t hash) {", "    switch (hash) {"]
    for key, name, policy in sorted(rows):
        header.append(f"        case 0x{key:08X}u: return Policy::{policy};  // {name}")
    header += ["        default: return Policy::Unknown;", "    }", "}", "",
               "inline bool IsCompanion(uint32_t sharedRootHash, uint32_t cueHash) {",
               "    switch (sharedRootHash) {"]
    companions = defaultdict(set)
    for (group, guid), (root, fields, _) in graph_records.items():
        if guid not in START_MANAGERS:
            continue
        manager_guid, stem = START_MANAGERS[guid]
        manager, outputs, image = graph_records[group, manager_guid]
        if fields.get("CMD_SetTarget", "").lower() != manager or results[message_hash(root)] != {"Pull"}:
            raise ValueError(f"Unexpected entry chain for {root} in {group}")
        delays = {f["name"].removeprefix("CMD_SetDelay") for f in image["fields"]
                  if f.get("name", "").startswith("CMD_SetDelay") and f.get("value")}
        for field, target in outputs.items():
            slot = field.removeprefix("CMD_SetTarget")
            target = target.lower()
            if slot.isdecimal() and slot not in delays and stem in target and (group, target) in actors:
                if names[message_hash(target)] == {target}:
                    companions[root].add(target)
    if len(companions) != len(START_MANAGERS):
        raise ValueError("A verified scene entry graph is missing")
    if church_exit != {"msg_end_se_03_03"} or kong_ready != {"bosskong_spawn_complete"}:
        raise ValueError("Unexpected church shaft exit or Tormentor spawn completion")
    companions["msg_pull_player_to_start_igc_03_03"].update(church_exit | kong_ready)
    for root, targets in sorted(companions.items()):
        header.append(f"        case 0x{message_hash(root):08X}u:  // {root}")
        header.append("            switch (cueHash) {")
        for target in sorted(targets):
            header.append(f"                case 0x{message_hash(target):08X}u:  // {target}")
        header += ["                    return true;", "                default: return false;", "            }"]
    prompt_roots = " || ".join(f"root == 0x{message_hash(root):08X}u" for root in sorted(branches))
    header += ["        default: return false;", "    }", "}", "", "inline bool IsPromptRoot(uint32_t root) {",
               f"    return {prompt_roots};", "}", "", "inline bool IsPromptBranch(uint32_t root, uint32_t cue) {",
               "    switch (root) {"]
    for root, (success, failure) in sorted(branches.items()):
        header += [f"        case 0x{message_hash(root):08X}u:  // {root}",
                   f"            return cue == 0x{message_hash(success):08X}u || cue == 0x{message_hash(failure):08X}u;"]
    header += ["        default: return false;", "    }", "}", "", "inline bool IsActorCue(uint32_t hash) {",
               "    switch (hash) {"]
    catalogs = [ROOT / "analysis" / folder / "entities.jsonl" for folder in ("catalog", "catalog_dlc")]
    reserved = set(companions).union(*companions.values(), {"msg_spawn_npcf_unitologist_igc_03_03_done"})
    actor_rows = campaign_actor_cues(catalogs, reserved)
    for key, name in actor_rows:
        header.append(f"        case 0x{key:08X}u:  // {name}")
    header += ["            return true;", "        default: return false;", "    }", "}", "}  // namespace scene_catalog", ""]
    (ROOT / "src" / "scene_catalog.h").write_text("\n".join(header), encoding="utf-8")
    print(dict(Counter(policy for _, _, policy in rows)))
    print(f"{len(actor_rows)} campaign actor cues")


if __name__ == "__main__":
    main()
