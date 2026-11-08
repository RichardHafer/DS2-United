"""Which messages of the campaign reach a client, offline, from the level data.

Every string field of the ch01..ch11 entities (analysis/catalog/entities.jsonl)
names a message the entity listens to (an input: its target name, an on/off
message) or one it sends (an output: targets, broadcasts, death and kill
messages, notifications). Field directions come from the field names, see
OUTPUT below. For every gap candidate of tools/coop_campaign_sweep.py the inputs
are followed back to their senders and judged the way the co-op handles them:

    shared     a listener of the message is a mirrored class ([Mirror] Classes,
               AnyClasses, VoiceClasses): the mirror sends it to every machine
    relayed    sent by a MultiManager (relayed, MultiManagers=1)
    host       sent by a host node ([Mirror] HostNodes) or by an enemy (HostNPCs,
               EnemyMessages): the host's message reaches the clients
    both       sent by something that runs on every machine anyway (a door the
               mirror opens, a sequence, an automatic trigger, a stream set)
    logic      sent by a gate, relay, switch or timer: as good as its own inputs
               (followed up to five steps)
    trigger    sent by a trigger a player walks into: each machine fires its
               own only for its own Isaac (GhostsIgnoreTriggers)
    action     sent by a player action that happens on one machine only (a
               prop, destructible, container, pickup, receptacle, hack, switch
               result that is not mirrored)
    none       no sender in the level data (code or Lua sends it)

An entity is covered when every input reaches it as shared, relayed, host or
both; otherwise its worst input names the gap.

    python tools/coop_message_graph.py [--ini PATH]

Writes analysis/coop_sweep/message_verdicts.tsv and prints the table.
"""
import argparse
import collections
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from coop_campaign_sweep import DEFAULT_INI, load_rules  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "analysis" / "catalog" / "entities.jsonl"
CANDIDATES = ROOT / "analysis" / "coop_sweep" / "candidates.tsv"
OUT = ROOT / "analysis" / "coop_sweep" / "message_verdicts.tsv"
LEVEL = re.compile(r"levels\\(ch\d\d_[a-z0-9_]+)", re.I)

# Field names that send. Everything else that holds a message name is an input.
OUTPUT = re.compile(
    r"(SetTarget\d|SetTarget$|DefaultTarget|ReverseTarget|Broadcast\dMessage|DirectMessage|OutputEvent|OutputMessage|DeathTarget|KillMessage|"
    r"DamageMsg|Notify|Finished|Done|Complete|Loaded|Loading|Unloading|DetachMessage|Solved|Success|Failure|TimesUp|SndEnded|STOPPED|ARRIVAL|"
    r"DECELERATING|OnPickupMsg|Pulled|Startled|OnActionStart|SpawnDone|SendMessage|OnSleep|Playing|AudioFinished|ValueUpdate|PlayerEnter|"
    r"PlayerExit|Threshold\d|OnDeathMessage|OnHealthModified|ZoneHealthModifed|DismemberMessage|InterruptedMessage|OpenStart|CloseStart|"
    r"EnterStoreMessage|ExitStoreMessage|HasWeaponEver|ChangeUniform|EnterHackingStation|ExitHackingStation|OnAudioThreshold|SuccessState|"
    r"EnterTube|ExitTube|StartUseMessage|EndUseMessage|SpecialPullMessage|ActivatingMessage|ActiveMessage|OnDeletedMsg|"
    r"MSG_ON\d?$|MSG_OFF$|MSG_SWITCH_PRESSED|EnterPlayDead|ExitPlayDead|IntroStarted|IntroFinished|OutroStarted|OutroFinished|"
    r"StartMessage$|FinishMessage$|ONAWARE|AWARE$|Phase\d.*Msg$|SuccessMsgName|FailureMsgName|CompleteMessage|PrimeComplete|SetCompletedName)")
# Classes whose own StartMessage/FinishMessage are inputs, not outputs.
START_IS_INPUT = {"Scaler"}
MULTI = {"MultiManager", "RandomMultiManager"}
LOGIC = {"OrEventGate", "AndEventGate", "MessageRelay", "VariableSwitch", "TimerUtil", "TimerSet", "CounterUtil", "GraphLinkOperator",
         "TestInputEvent", "TriggerRandom", "MessageGate", "PlayerAndGate"}
BOTH = {"ScriptedSequence", "ScriptedEventSequence", "TriggerAuto", "StreamSet", "StreamScripter", "Checkpoint", "PlayerSetChapter",
        "StandardDoor", "SimpleOneTimeDoor", "MoverController", "Mover", "ScriptedSentence", "PlayAudioStream", "TubeMarker", "VidScreen"}
TRIGGERS = {"TriggerPhysical", "TriggerLOS", "ProximitySwitch"}
ENEMY_PARTS = {"HealthComponent", "DismemberableHealthComponent", "AwarenessComponent", "DSNPCSpawningComponent", "StasisComponent",
               "FearComponent", "AIAimingComponent"}
RANK = {"shared": 0, "relayed": 0, "host": 0, "both": 0, "logic": 1, "trigger": 2, "action": 3, "none": 4}


def is_output(cls, field):
    if cls in START_IS_INPUT and field in ("CMD_StartMessage",):
        return False
    return bool(OUTPUT.search(field))


def load_graph():
    listeners, senders = collections.defaultdict(list), collections.defaultdict(list)
    inputs = collections.defaultdict(list)
    for line in CATALOG.open(encoding="utf-8"):
        r = json.loads(line)
        m = LEVEL.search(r.get("asset", ""))
        images = r.get("images") or []
        if not m or not images:
            continue
        level = m.group(1).lower()
        components = {x["class"] for x in images[1:]}
        enemy = bool(components & {"AwarenessComponent", "DSNPCSpawningComponent"})
        owner = images[0]["class"]
        for im in images:
            for f in im.get("fields", []):
                name = f.get("string_candidate")
                if f.get("flags") != 17 or not name:
                    continue
                key = (level, name.lower())
                who = (r["guid"], owner, im["class"], f["name"], enemy)
                if is_output(im["class"], f["name"]):
                    senders[key].append(who)
                else:
                    listeners[key].append(who)
                    inputs[r["guid"]].append((key, f["name"]))
    return listeners, senders, inputs


def judge_sender(owner, part, enemy, rules):
    if owner in MULTI:
        return "relayed"
    if owner in rules["host_nodes"] or enemy or part in ENEMY_PARTS and enemy:
        return "host"
    if owner in BOTH or owner in rules["sequence"]:
        return "both"
    if owner in LOGIC:
        return "logic"
    if owner in TRIGGERS:
        return "trigger"
    return "action"


def message_verdict(key, listeners, senders, inputs, rules, depth=0, seen=None):
    """(verdict, detail) for one message: the best way it reaches a client."""
    seen = seen or set()
    if key in seen or depth > 5:
        return "logic", "cycle"
    seen = seen | {key}
    for guid, owner, part, field, enemy in listeners.get(key, []):
        if owner in rules["mirrored"] or owner in rules["sequence"]:
            return "shared", f"listened to by {owner}"
    best, detail = "none", "no sender in the level data"
    for guid, owner, part, field, enemy in senders.get(key, []):
        v = judge_sender(owner, part, enemy, rules)
        d = f"{owner}.{field}"
        if v == "logic":
            sub = [message_verdict(k, listeners, senders, inputs, rules, depth + 1, seen) for k, _ in inputs.get(guid, [])]
            v, d = min(sub, key=lambda x: RANK[x[0]]) if sub else ("none", f"{owner} without inputs")
            d = f"{owner} <- {d}"
        if RANK[v] < RANK[best]:
            best, detail = v, d
            if RANK[v] == 0:
                break
    return best, detail


# A player action happens on one machine. Its results reach the other one when the
# message is shared, or through a relayed MultiManager listening to it. A host node
# listening directly only hears the host's own actions; any other direct listener
# never hears the partner's.
ACTION_CLASSES = {"Destructible", "DistortedDamageableProp", "TKReceptacle", "TKReceptacleTarget", "SolarPuzzle", "LinearHandle", "KeyLock",
                  "BreakableContainer", "StandardContainer", "PowerUp", "Schematic", "SpareAmmo", "Item", "KeyItem", "DamageableProp",
                  "DismemberableProp", "ScriptableProp", "Switch", "PowerNodeSwitch", "SimplePressAndHoldSwitch", "HackingStationMiniGame",
                  "RechargeStation", "SaveKiosk", "UpgradeBench", "ShipStore"}


def action_results(listeners, senders, rules):
    """guid -> (class, level, verdict, detail) for every entity of an action class with outputs."""
    outputs = collections.defaultdict(list)
    for key, who in senders.items():
        for guid, owner, part, field, enemy in who:
            if owner in ACTION_CLASSES and not enemy:
                outputs[(guid, owner, key[0])].append((key, field))
    result = {}
    for (guid, owner, level), outs in outputs.items():
        missed, host_only, fine = collections.Counter(), 0, 0
        for key, field in outs:
            ls = [x for x in listeners.get(key, []) if x[0] != guid]
            if any(o in rules["mirrored"] or o in rules["sequence"] for _, o, _, _, _ in ls):
                fine += 1
                continue
            for _, o, part, f, enemy in ls:
                if o in MULTI:
                    fine += 1
                elif o in rules["host_nodes"]:
                    host_only += 1
                elif not enemy:
                    missed[f"{o}.{f}"] += 1
        if missed:
            verdict, detail = "missed", ", ".join(f"{k} {v}" for k, v in missed.most_common(4))
        elif host_only:
            verdict, detail = "host node only", f"{host_only} message(s) to VariableOperator/Watcher etc."
        else:
            verdict, detail = "covered", ""
        result[guid] = (owner, level, verdict, detail)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ini", type=Path, default=DEFAULT_INI)
    args = parser.parse_args()
    rules = load_rules(args.ini)
    listeners, senders, inputs = load_graph()
    actions = action_results(listeners, senders, rules)
    with (OUT.parent / "action_results.tsv").open("w", encoding="utf-8", newline="\n") as f:
        f.write("guid\tlevel\tclass\tverdict\tdetail\n")
        for guid, (owner, level, verdict, detail) in sorted(actions.items(), key=lambda kv: (kv[1][1], kv[1][0])):
            f.write(f"{guid}\t{level}\t{owner}\t{verdict}\t{detail}\n")
    by_class = collections.defaultdict(collections.Counter)
    missed_to = collections.defaultdict(collections.Counter)
    for owner, level, verdict, detail in actions.values():
        by_class[owner][verdict] += 1
        if verdict == "missed":
            for part in detail.split(", "):
                missed_to[owner][part.rsplit(" ", 1)[0]] += 1
    print("results of player actions (outputs of action classes):")
    print("class".ljust(26) + "".join(v.rjust(16) for v in ("covered", "host node only", "missed")))
    for owner, c in sorted(by_class.items(), key=lambda kv: -sum(kv[1].values())):
        print(owner.ljust(26) + "".join(str(c.get(v, 0)).rjust(16) for v in ("covered", "host node only", "missed")))
        if missed_to[owner]:
            print("    missed listeners: " + ", ".join(f"{k} {v}" for k, v in missed_to[owner].most_common(6)))
    print()
    rows = []
    with CANDIDATES.open(encoding="utf-8") as f:
        next(f)
        for line in f:
            guid, level, cls, family = line.rstrip("\n").split("\t")
            if family in ("enemy_drops", "loot", "container_state"):
                continue   # loot is decided by the drop sync, not by messages
            ins = inputs.get(guid, [])
            if not ins:
                rows.append((guid, level, cls, family, "no inputs", "", ""))
                continue
            judged = [(message_verdict(k, listeners, senders, inputs, rules), k[1], field) for k, field in ins]
            (verdict, detail), message, field = max(judged, key=lambda x: RANK[x[0][0]])
            if RANK[verdict] == 0:
                verdict = "covered"
            rows.append((guid, level, cls, family, verdict, f"{field}={message}", detail))
    with OUT.open("w", encoding="utf-8", newline="\n") as f:
        f.write("guid\tlevel\tclass\tfamily\tverdict\tworst_input\tdetail\n")
        for row in rows:
            f.write("\t".join(row) + "\n")
    table = collections.defaultdict(collections.Counter)
    for guid, level, cls, family, verdict, _, _ in rows:
        table[(family, cls)][verdict] += 1
    verdicts = ["covered", "logic", "trigger", "action", "none", "no inputs"]
    print("family / class".ljust(44) + "".join(v.rjust(10) for v in verdicts))
    for (family, cls), c in sorted(table.items(), key=lambda kv: (kv[0][0], -sum(kv[1].values()))):
        if sum(c.values()) < 3:
            continue
        print(f"{family} / {cls}".ljust(44) + "".join(str(c.get(v, 0)).rjust(10) for v in verdicts))
    print(f"\nwrote {OUT} ({len(rows)} rows)")


if __name__ == "__main__":
    main()
