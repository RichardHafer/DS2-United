"""Offline co-op pass over the campaign levels: which world objects our sync covers.

Every entity of the ch01..ch11 levels (analysis/catalog/entities.jsonl) is sorted
by its class into what the co-op does with it today, read from the installed
DS2United.ini ([Mirror] Classes/HostNodes/AnyClasses/VoiceClasses/DeleteClasses,
[Physics] RestClasses) plus the modules that cover a class by code (host NPCs,
gate, loadsync, removals, world calls). What is left are the gap families the
live pass (tools/coop_world_diff.py --watch) has to confirm or rule out.

    python tools/coop_campaign_sweep.py [--ini PATH]

Writes analysis/coop_sweep/candidates.tsv (GUID, level, class, family),
coverage.tsv (every campaign entity) and summary.json; prints the per-level table.
"""
import argparse
import collections
import configparser
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "analysis" / "catalog" / "entities.jsonl"
OUT = ROOT / "analysis" / "coop_sweep"
DEFAULT_INI = Path(r"D:\SteamLibrary\steamapps\common\Dead Space 2\plugins\DS2United.ini")
LEVEL = re.compile(r"levels\\(ch\d\d_[a-z0-9_]+)", re.I)

# Covered by code, not by an INI list.
HOST_NPC_COMPONENTS = {"AwarenessComponent", "DSNPCSpawningComponent"}   # every enemy: HostNPCs=1, ghosted to clients
BY_MODULE = {
    "Checkpoint": "gate/loadsync", "LevelChange": "gate/loadsync", "StreamSet": "gate/loadsync", "StreamScripter": "gate/loadsync",
    "PlayerSetChapter": "gate/loadsync", "FuncSpawn": "host NPCs", "MultiRemoveTarget": "removals", "NeutralNPC": "local by design",
    "NicoleBoss": "local by design",
}
LOGIC = {"MultiManager", "RandomMultiManager", "OrEventGate", "AndEventGate", "MessageRelay", "RelayMessageByTeam", "MessageGate",
         "TriggerPhysical", "TriggerAuto", "TriggerLOS", "TriggerRandom", "ProximitySwitch", "PlayerAndGate", "TestInputEvent",
         "GraphLinkOperator", "GraphActivator", "State"}
LOGIC_STATE = {"VariableSwitch", "TimerUtil", "TimerSet", "CounterUtil"}   # keep state per machine: may drift
PRESENTATION = {"ExecuteVFX", "NetExecuteVFX", "AnimateLight", "MoveLight", "TriggerShadowLight", "PlaySound", "PlayLightSound", "ZoneRender",
                "DebugText", "SetAudioMix", "SetAudioConfig", "LoadMixPatch", "SetDspEffect", "LuaAudioScript", "FadeScreenFx", "CounterVFX",
                "VFXAnalogMeter", "VFXTimer", "FXObject", "SetOcclusion", "BoxOccluder", "OcclusionWatcher", "DirectionalShadowParams",
                "PanoSettings", "PlayShockProfile", "FearSegmentEmitter", "PlayerFearReact", "RASAnnouncement", "RigAnnouncementSystem",
                "AchievementAward", "AchievementSwitch", "TrainingTip", "TrainingTipStage", "UIPopup", "MenuPanel", "Apt3dScreenTransition",
                "Apt3dScreenMount", "VidScreen", "UILoadingScreenData", "BlankScreenData", "SetCurrentDisc", "MovieSubtitleManager"}
AI_DATA = {"WayPoint", "NavFolder", "NavObstacle", "NavLinkStrip", "NavLink", "NavSmartObstacle", "NavAnimSettings", "NavMoveableField",
           "NavRoutingSettings", "CoverNode", "RetreatPoint", "EditBreadcrumbs", "FlowField", "EnemyLifetimeModifier", "PairedAttackCoordinator",
           "StationaryShootingMarker", "HealthComponentTuningRelay", "NecroAIPopulationSettings"}
PER_PLAYER = {"SaveKiosk", "UpgradeBench", "ShipStore", "ItemStoreList", "RechargeStation", "PlayerAir", "AirMeter", "PlayerChangeState",
              "SimpleTeleport", "PlayerSwapItem", "PlayerDecompressionInfo", "PlayerSettings", "PlayerUISettings", "PlayerMX9Inventory",
              "MX9InventorySettings", "ObjectivesSubscreenSettings", "VideoSubscreenSettings", "TestDifficultyMode", "EquipCommand",
              "PlayerHelmetCommand", "LogEntryList", "EyePokeMachine", "TubeMarker"}
CAMERA = re.compile(r"Camera|PushCamera|PopCamera|BoomOperator|CameraModifier|ModifierOp|PostBlendModifier")
# The gap families. Loot: what lies, drops or comes out of a container on one machine.
LOOT = {"PowerUp", "SpareAmmo", "Item", "KeyItem", "Schematic", "GunProxy", "MultiAmmo"}
KINESIS_PUZZLE = {"TKReceptacle", "TKReceptacleTarget", "SolarPuzzle", "LinearHandle", "KeyLock"}
DESTRUCTION = {"Destructible", "DistortedDamageableProp", "CuttingShape"}
ENVIRONMENT = {"GravityField", "ImpulseField", "DrainField", "EnvironmentalHazard", "DecompressionTarget", "WindowMarker", "CorruptionBrushEntity",
               "CorruptionEntity", "ZoneWorld", "StasisCaster", "ProjectileLauncher", "Landmine"}
WORLD_LISTENERS = {"Rotate", "Scaler", "MetaModelStateChanger", "Attacher", "NetBinaryState", "Entity", "TentacleControl",
                   "PropsScriptingCoordinator", "BoneMovementWatcher", "HaloMarker", "RagdollConstraint", "BallAndSocketConstraint",
                   "PrismaticConstraint", "ImpalingProjectile"}

FAMILY_NOTE = {
    "enemy_drops": "an enemy with a drop list: its drop is spawned by the host only (HostNPCs), nothing reaches a client",
    "loot": "pickups, enemy drops and container contents exist on one machine (drops.inc is stage one: observe only)",
    "container_state": "a container opened or broken on one machine; the other may still show it closed or whole",
    "kinesis_puzzle": "receptacles, handles and the solar puzzle: solved by one player's kinesis, the state may stay local",
    "destruction": "Destructible and friends are in neither the mirror's classes nor SharedDeletes",
    "environment": "fields, hazards, decompression and corruption: switched by messages to classes the mirror does not share",
    "world_listener": "Rotate/Scaler/MetaModelStateChanger/Attacher...: correct only when the chain that drives them runs on both machines",
    "logic_state": "variable switches, timers and counters keep their own state on every machine",
}


def ini_list(cp, section, key, default=""):
    try:
        raw = cp.get(section, key)
    except (configparser.NoSectionError, configparser.NoOptionError):
        raw = default
    return {x.strip() for x in raw.replace(";", ",").split(",") if x.strip()}


def load_rules(ini):
    cp = configparser.ConfigParser(strict=False, interpolation=None, comment_prefixes=(";", "#"), inline_comment_prefixes=None)
    cp.optionxform = str
    cp.read_string(ini.read_bytes().decode("utf-8-sig", errors="replace"))
    return {
        "mirrored": ini_list(cp, "Mirror", "Classes"),
        "host_nodes": ini_list(cp, "Mirror", "HostNodes"),
        "sequence": ini_list(cp, "Mirror", "AnyClasses") | ini_list(cp, "Mirror", "VoiceClasses"),
        "deleted": ini_list(cp, "Mirror", "DeleteClasses") | ini_list(cp, "Mirror", "RemovalClasses"),
        "rest": ini_list(cp, "Physics", "RestClasses"),
    }


def classify(cls, components, rules):
    """(coverage, family): coverage says why it is fine, family names the gap."""
    if cls in BY_MODULE:   # NeutralNPCs run on every machine: each stomps its own copy's loot
        return BY_MODULE[cls], None
    if components & HOST_NPC_COMPONENTS:
        return "host NPC", "enemy_drops" if "ItemDropListComponent" in components else None
    if cls == "ItemDropList":
        return "data (drop table)", None
    if cls in rules["host_nodes"]:
        return "host only (mirror)", None
    if cls in rules["sequence"]:
        return "sequence (mirror)", None
    if cls == "BreakableContainer":
        return "deleted+rest", "loot"   # the break is shared, its loot is not
    if cls == "StandardContainer":
        return "-", "container_state"
    if cls in LOOT or "ItemDropListComponent" in components and cls not in rules["mirrored"]:
        return "-", "loot"
    if cls in rules["mirrored"]:
        return "mirrored", None
    if cls in rules["deleted"] or cls in rules["rest"]:
        return "deleted/rest", None
    if cls in KINESIS_PUZZLE:
        return "-", "kinesis_puzzle"
    if cls in DESTRUCTION:
        return "-", "destruction"
    if cls in ENVIRONMENT:
        return "-", "environment"
    if cls in WORLD_LISTENERS:
        return "-", "world_listener"
    if cls in LOGIC_STATE:
        return "-", "logic_state"
    if cls in LOGIC:
        return "logic (relayed)", None
    if cls in PRESENTATION:
        return "local presentation", None
    if cls in AI_DATA:
        return "AI data (host)", None
    if cls in PER_PLAYER or CAMERA.search(cls):
        return "per player", None
    return "unclassified", None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ini", type=Path, default=DEFAULT_INI)
    args = parser.parse_args()
    rules = load_rules(args.ini)
    per_level = collections.defaultdict(collections.Counter)
    families = collections.defaultdict(collections.Counter)
    unclassified = collections.Counter()
    rows, coverage_rows = [], []
    variables = {}   # name hash -> what sets and reads it, for the live comparison
    for line in CATALOG.open(encoding="utf-8"):
        r = json.loads(line)
        m = LEVEL.search(r.get("asset", ""))
        images = r.get("images") or []
        if not m or not images:
            continue
        level = m.group(1).lower()
        cls = images[0]["class"]
        components = {x["class"] for x in images[1:]}
        coverage, family = classify(cls, components, rules)
        coverage_rows.append((r["guid"], level, cls, coverage, family or ""))
        if cls in ("VariableOperator", "VariableWatcher", "VariableSwitch"):
            fields = {f["name"]: f for f in images[0].get("fields", [])}
            if "CMD_SetVariable" in fields:
                v = variables.setdefault(fields["CMD_SetVariable"]["value"], {"name": "", "levels": set(), "operators": 0, "readers": 0})
                v["levels"].add(level)
                v["operators" if cls == "VariableOperator" else "readers"] += 1
                if fields.get("CMD_SetVariableName", {}).get("string_candidate"):
                    v["name"] = fields["CMD_SetVariableName"]["string_candidate"]
        per_level[level][family or coverage] += 1
        if family:
            families[family][cls] += 1
            rows.append((r["guid"], level, cls, family))
        elif coverage == "unclassified":
            unclassified[cls] += 1
    OUT.mkdir(parents=True, exist_ok=True)
    with (OUT / "candidates.tsv").open("w", encoding="utf-8", newline="\n") as f:
        f.write("guid\tlevel\tclass\tfamily\n")
        for row in sorted(rows, key=lambda x: (x[1], x[3], x[2], x[0])):
            f.write("\t".join(row) + "\n")
    with (OUT / "variables.tsv").open("w", encoding="utf-8", newline="\n") as f:
        f.write("hash\tname\tlevels\toperators\treaders\n")
        for h, v in sorted(variables.items()):
            f.write(f"{h:08X}\t{v['name']}\t{','.join(sorted(v['levels']))}\t{v['operators']}\t{v['readers']}\n")
    with (OUT / "coverage.tsv").open("w", encoding="utf-8", newline="\n") as f:
        f.write("guid\tlevel\tclass\tcoverage\tfamily\n")
        for row in coverage_rows:
            f.write("\t".join(row) + "\n")
    columns = ["enemy_drops", "loot", "container_state", "kinesis_puzzle", "destruction", "environment", "world_listener", "logic_state"]
    summary = {"rules": {k: sorted(v) for k, v in rules.items()},
               "levels": {lv: dict(c) for lv, c in sorted(per_level.items())},
               "families": {f: dict(c.most_common()) for f, c in families.items()},
               "unclassified": dict(unclassified.most_common()), "notes": FAMILY_NOTE}
    (OUT / "summary.json").write_text(json.dumps(summary, indent=1), encoding="utf-8")
    print("level".ljust(18) + "".join(c[:10].rjust(11) for c in columns) + "    covered")
    for level, c in sorted(per_level.items()):
        covered = sum(v for k, v in c.items() if k not in columns)
        print(level.ljust(18) + "".join(str(c.get(col, 0)).rjust(11) for col in columns) + str(covered).rjust(11))
    for fam in columns:
        print(f"\n{fam}: {FAMILY_NOTE[fam]}")
        print("  " + ", ".join(f"{k} {v}" for k, v in families[fam].most_common()))
    print("\nunclassified classes: " + ", ".join(f"{k} {v}" for k, v in unclassified.most_common()))
    print(f"\nwrote {OUT / 'candidates.tsv'} ({len(rows)} rows), coverage.tsv ({len(coverage_rows)} rows) and summary.json")


if __name__ == "__main__":
    main()
