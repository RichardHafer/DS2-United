"""List the player's story scenes for the co-op scene modes.

    python -I tools/list_coop_scenes.py [dir of extracted player cutscene banks] > analysis/COOP_SCENE_LIST.md

One row per scene (igc_*/se_* sequences on the player, grouped by their name
stem): chapter, parts with their length, co-actors (named after their own clip,
e.g. igc_09_05_npcfNicoleCorpse_001 -> npcfNicoleCorpse) and the settings
flags. The proposed mode is a first guess for the user to correct:
  hidden  the partner stays hidden (the scene's story needs Isaac alone)
  puppet  the partner stands by with an idle clip
A scene goes to hidden when its co-actors grab, attack or confront Isaac by
name (fanatic, unitologist, nicole, tormentor, necro and enemy classes), when
it kills him (SSEQFL_PLAYER_DEATH) or when it is a QTE.
"""
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CATALOG = REPO / "analysis" / "catalog" / "entities.jsonl"
BANKS = Path(sys.argv[1]) if len(sys.argv) > 1 else None
STORY = re.compile(r"^(igc|se)_(\d\d)_(\d\d[a-z]?)_player")
HOSTILE = re.compile(r"fanatic|unitolog|cultist|nicole|tormentor|slasher|zombie|necro|lurker|puker|pack|stalker|brute|hunter|daina|ubermorph|tiedemann|soldier|security", re.I)
ENEMY_CLASSES = {"Zombie", "Leaper", "Lurker", "Puker", "Pack", "Stalker", "Brute", "Exploder", "Infector", "Spitter", "TubeBeetle", "Ubermorph", "NicoleBoss", "Tormentor"}


def fields(image):
    return {f["name"]: (f.get("string_candidate") or f.get("guid_candidate")) for f in image["fields"]}


def main():
    scenes, wanted = [], set()
    with CATALOG.open(encoding="utf-8") as f:
        for line in f:
            if '"ScriptedEventSequence"' not in line and '"ScriptedSequence"' not in line:
                continue
            record = json.loads(line)
            image = record["images"][0]
            if image["class"] not in ("ScriptedEventSequence", "ScriptedSequence"):
                continue
            values = fields(image)
            anim = values.get("CMD_SetActionAnimationString") or ""
            if not STORY.match(anim.lower()):
                continue
            actors = [values[k] for k in sorted(values) if k.startswith("CMD_SetSyncAnimated") and values[k]]
            wanted.update(a.upper() for a in actors)
            scenes.append(dict(anim=anim, guid=record["guid"], archive=record["archive"], actors=[a.upper() for a in actors],
                               flags=values.get("CMD_SetScriptedSequenceSettingsFlags") or "", move=values.get("CMD_SetScriptedSequenceMoveType") or ""))
    actors = {}
    with CATALOG.open(encoding="utf-8") as f:
        for line in f:
            match = re.search(r'"guid": "([0-9A-F-]+)"', line)
            if not match or match.group(1) not in wanted:
                continue
            record = json.loads(line)
            image = record["images"][0]
            values = fields(image)
            state = values.get("CMD_SetAnimState") or values.get("CMD_SetIdleAnim") or ""
            name = re.sub(r"^(igc|se)_\d\d_\d\d[a-z]?_|_\d+$", "", state) or image["class"]
            actors[record["guid"]] = (image["class"], name)
    # every actor of a scene has a bank of its own: igc_10_02_<actor>.bnk
    cast_banks = defaultdict(set)
    with (REPO / "analysis" / "catalog" / "assets.jsonl").open(encoding="utf-8") as f:
        for line in f:
            if '"BNK"' not in line:
                continue
            name = json.loads(line)["name"].lower()
            m = re.match(r"^((?:igc|se)_\d\d_\d\d[a-z]?)_(.+)\.bnk$", name)
            if m and not m.group(2).startswith("player"):
                cast_banks[m.group(1)].add(m.group(2))
    frames = {}
    if BANKS:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import ds2_bnk as bnk
        for path in BANKS.glob("*.bnk"):
            try:
                bank = bnk.read_bank(path.read_bytes())
            except bnk.BankError:
                continue
            for action in bank.actions:
                for leaf in action.leaves:
                    frames[action.name.lower()] = max(frames.get(action.name.lower(), 0), bnk.owner_info(bank.data, leaf.owner).frames)
    grouped = defaultdict(list)
    for scene in scenes:
        m = STORY.match(scene["anim"].lower())
        grouped["%s_%s_%s" % m.groups()].append(scene)
    print("| Scene | Ch | Parts (s) | Co-actors | Flags | Proposed |")
    print("|---|---|---|---|---|---|")
    for stem in sorted(grouped, key=lambda s: (s.split("_")[1], s.split("_")[2], s)):
        parts = {}
        for scene in grouped[stem]:
            parts.setdefault(scene["anim"].lower(), scene)
        cast, flags, hostile = set(cast_banks.get(stem, ())), set(), False
        for name in cast:
            hostile |= bool(HOSTILE.search(name))
        for scene in parts.values():
            for guid in scene["actors"]:
                cls, name = actors.get(guid, ("?", guid[:8]))
                if not re.fullmatch(r"npc[fm]_idle", name):
                    cast.add(name)
                hostile |= cls in ENEMY_CLASSES or bool(HOSTILE.search(name))
            flags.update(x.replace("SSEQFL_", "") for x in scene["flags"].split("|") if x)
        lengths = ", ".join("%s %.0f" % (anim[len(stem) + 8:] or "main", frames.get(anim, 0) / 30.0) for anim in sorted(parts))
        dies = any("DEATH" in f for f in flags)
        proposed = "hidden" if hostile or dies else "puppet"
        chapter = int(stem.split("_")[1])
        notable = sorted(f for f in flags if f not in ("REPEATABLE", "NEGATE_CHAR_PROXY_OFFSET", "NO_GRAVITY_COLLISION"))
        print("| %s | %d | %s | %s | %s | %s |" % (stem, chapter, lengths, ", ".join(sorted(cast)) or "-", " ".join(notable) or "-", proposed))


if __name__ == "__main__":
    main()
