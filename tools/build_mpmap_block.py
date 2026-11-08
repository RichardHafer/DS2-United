"""Build src/generated_mpmap_block.h from analysis/catalog/entities.jsonl and,
when present, analysis/catalog_dlc/entities.jsonl (the Outbreak maps).

Every entity placed on one of the multiplayer maps whose class talks to the
online game (end of round, end-game camera, round timer, objective markers, team
relays and filters) or needs global_mp_assets (the map's own slashers) gets a
group, so [MpMap] can keep it from spawning in single player. A few more classes
are listed as "watch" only: they spawn, and the log says when they do.

usage: python tools/build_mpmap_block.py
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CATALOG = os.path.join(ROOT, "analysis", "catalog", "entities.jsonl")
DLC_CATALOG = os.path.join(ROOT, "analysis", "catalog_dlc", "entities.jsonl")
OUT = os.path.join(ROOT, "src", "generated_mpmap_block.h")

# Bit i of an entity's map mask; the same order as coop_mpmap::kMapTable.
MAPS = ["mp_bombRun", "mp_detonate", "mp_escape", "mp_gov", "mp_solar", "pdlc_mp_mall", "pdlc_mp_school"]
# group numbers match coop_mpmap::Group
BLOCK = {
    "OnlineEndRound": 1, "MPEndOfGameSequencer": 1,
    "UIMPRoundTimerHub": 2,
    "UIMPObjectiveIndicator": 3,
    "RelayMessageByTeam": 4, "MPDamageFilter": 4,
    "Zombie": 5,
}
WATCH = {
    "ObjectiveEntry", "NetBinaryState", "KeyItem", "KeyLock", "Checkpoint", "MultiplayerSpawnPoint",
    "TimerUtil", "TimerSet", "PowerUp", "ItemDropList", "LuaAudioScript", "StreamSet", "ZoneWorld",
    "VentMarker", "FuncSpawn", "UILoadingScreenData",
}
# The map's round clock is a TimerUtil started by one of these messages. It joins
# group 2 with the ExecuteVFX displays it writes M:SS into (m_VFXGUID), so the
# survival and mercenaries time shows only on our overlay. Only the clock and
# TimerSets refer to it, and TimerSet skips a timer it cannot find. The
# objective timers (fuses, bomb, drills) keep running.
ROUND_TIMER_STARTS = {"start_roundtimer", "round_timer_start"}


def round_timer_guids():
    """{guid: "TimerUtil" or "ExecuteVFX"} for every map's round clock and its displays."""
    out = {}
    for line in catalog_lines():
        if '"TimerUtil"' not in line:
            continue
        e = json.loads(line)
        if not map_mask(e.get("archive", "")):
            continue
        for image in e.get("images", []):
            if image.get("class") != "TimerUtil":
                continue
            fields = image.get("fields", [])
            start = next((f.get("string_candidate", "") for f in fields if f.get("name") == "CMD_StartTimer"), "")
            if start.lower() not in ROUND_TIMER_STARTS:
                continue
            out[e["guid"].upper()] = "TimerUtil"
            for f in fields:
                if f.get("name") == "m_VFXGUID" and f.get("guid_candidate"):
                    out[f["guid_candidate"].upper()] = "ExecuteVFX"
    # The Outbreak maps' images are baked (no field list): read the start message
    # hash at +0x00 and the sixteen display GUIDs at +0x58 straight from them.
    try:
        import mp_baked
    except Exception as exc:
        print(f"note: the Outbreak maps' round clocks are not listed ({exc})")
        return out
    starts = {mp_baked.name_hash(s) for s in ROUND_TIMER_STARTS}
    for _, guid, _, blob in mp_baked.iter_baked({"TimerUtil"}):
        if struct.unpack_from("<I", blob, 0)[0] not in starts:
            continue
        out[guid] = "TimerUtil"
        for k in range(16):
            g = mp_baked.guid_at(blob, 0x58 + 16 * k)
            if g:
                out[g] = "ExecuteVFX"
    return out


def map_mask(archive):
    parts = archive.lower().replace("/", "\\").split("\\")
    mask = 0
    for i, name in enumerate(m.lower() for m in MAPS):
        if any(p == name or p.startswith(name + ".") or p.startswith(name + "_") for p in parts):
            mask |= 1 << i
    # the loading screens are not map content
    if "loading_screen" in parts:
        return 0
    return mask


MP_GLOBALS = "global_mp_assets\\streams\\global_mp_assets.simgroup"
SP_GLOBALS = "global_assets\\streams\\global_assets.simgroup"
ALL_MAPS = (1 << len(MAPS)) - 1


def catalog_lines():
    for path in (CATALOG, DLC_CATALOG):
        if not os.path.exists(path):
            if path == CATALOG:
                raise SystemExit(f"missing {path}")
            print(f"note: {path} not found, the Outbreak maps get no entries")
            continue
        with open(path, "r", encoding="utf-8") as f:
            yield from f


def main():
    entries = {}
    # The MP map's own checkpoint pulls in global_mp_assets.str, the multiplayer
    # player setup (UI popups, player templates, MPLevelSettings, cameras). Its
    # entities are group 6 on every map, except GUIDs the campaign's global_assets
    # also places (those are the single player's own and must keep spawning).
    sp_guids = set()
    mp_globals = {}
    with open(CATALOG, "r", encoding="utf-8") as f:
        for line in f:
            if "global_" not in line:
                continue
            e = json.loads(line)
            asset = e.get("asset", "").lower()
            if SP_GLOBALS in asset:
                sp_guids.add(e["guid"].upper())
            elif MP_GLOBALS in asset:
                cls = e.get("class") or ((e.get("images") or [{}])[0].get("class")) or "?"
                mp_globals[e["guid"].upper()] = (cls, e.get("spawn_flags", 0))
    for guid, (cls, flags) in mp_globals.items():
        if guid in sp_guids:
            continue
        words = tuple(int(x, 16) for x in guid.split("-"))
        entries[words] = {"cls": cls, "group": 6, "mask": ALL_MAPS, "flags": {flags}}
    clock = round_timer_guids()
    for line in catalog_lines():
        if "mp_" not in line[:700].lower():
            continue
        e = json.loads(line)
        mask = map_mask(e.get("archive", ""))
        if not mask:
            continue
        if MP_GLOBALS in e.get("asset", "").lower():
            continue
        cls = e.get("class") or ""
        if e["guid"].upper() in clock:
            cls = clock[e["guid"].upper()]
            group = 2
        elif cls in BLOCK:
            group = BLOCK[cls]
        elif cls in WATCH:
            group = 0
        else:
            continue
        words = tuple(int(x, 16) for x in e["guid"].split("-"))
        old = entries.get(words)
        if old and old["group"] == 6:
            continue
        if old:
            old["mask"] |= mask
            old["flags"].add(e.get("spawn_flags", 0))
        else:
            entries[words] = {"cls": cls, "group": group, "mask": mask, "flags": {e.get("spawn_flags", 0)}}
    rows = sorted(entries.items())
    counts = {}
    for words, info in rows:
        counts[(info["group"], info["cls"])] = counts.get((info["group"], info["cls"]), 0) + 1
    with open(OUT, "w", encoding="utf-8", newline="\r\n") as out:
        out.write("// Generated by tools/build_mpmap_block.py from analysis/catalog/entities.jsonl.\n")
        out.write("// Entities placed on the multiplayer maps, sorted by GUID for a binary search.\n")
        out.write("// group: 0 watch only, 1 end of round, 2 round timer (the clock, its displays and\n")
        out.write("// the scoreboard hub), 3 objective markers,\n")
        out.write("// 4 team logic, 5 the map's own slashers (their model is in global_mp_assets),\n")
        out.write("// 6 global_mp_assets entities the MP checkpoint pulls in (UI popups, player setup).\n")
        out.write("// maps: bit i = kBlockMaps[i], the order of coop_mpmap::kMapTable.\n")
        for (group, cls), n in sorted(counts.items()):
            out.write(f"//   group {group} {cls}: {n}\n")
        out.write("#pragma once\n#include <cstdint>\n#include \"coop_mpmap.h\"\n")
        out.write("namespace coop_mpmap {\n")
        out.write("constexpr const char* kBlockMaps[] = {%s};\n" % ", ".join('"%s"' % m for m in MAPS))
        out.write("constexpr MapEntity kMpMapEntities[] = {\n")
        for words, info in rows:
            spawn = 1 if 1 in info["flags"] else 0
            out.write("    {{0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu}, %d, 0x%08Xu, %d, \"%s\"},\n" %
                      (words[0], words[1], words[2], words[3], info["group"], info["mask"], spawn, info["cls"]))
        out.write("};\n}  // namespace coop_mpmap\n")
    print(f"{len(rows)} entities written to {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
