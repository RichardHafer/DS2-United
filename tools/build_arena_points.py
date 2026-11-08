"""Build src/generated_arena_points.h: per campaign arena the points the game modes need.

Reads only arenas/arena_*.txt (no catalog, no game data). An arena file may list points by
hand, one per line:
    zone=x y z [radius]      Hold the Zone
    capsule=x y z            Mercenaries time capsules
    respawn=x y z            where a dead player comes back
Without them the points are spread over the recorded walk: floor samples only (a height
seen at least three times; ramps are left out), farthest point sampling from the start,
at least MIN_GAP metres apart on the same floor. The start is the first point, the
checkpoint landing; its zone is smaller (START_RADIUS).
Zones: the first ZONE_COUNT points. Capsules: all points but the start. Respawn: all.
Modes (bit = game mode, gamemode::Mode): survival always, Mercenaries with 3 capsule
points or more, Hold the Zone with 3 zones or more. Objective and Versus have no arena
data yet.
Door marks (the recorder's DoorKey): "doormark <GUID> state=<auto|inner|locked|held>"
lines in the recording, or in arenas/doors/<arena>.txt (marked while the arena ran); the
last mark of a door wins. inner: never locked; locked: always locked; held: kept open;
auto: the door table of build_arenas.py decides.

usage: python -I tools/build_arena_points.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "src" / "generated_arena_points.h"
MIN_GAP = 9.0
MAX_POINTS = 10
ZONE_COUNT = 7
START_RADIUS = 3.5
FLOOR_STEP = 0.25     # heights are grouped in steps of this size
FLOOR_SEEN = 3        # samples a floor height needs
FLOOR_REACH = 0.15    # a sample this close to a floor height stands on it (ramps are 0.3 off and more)
OTHER_FLOOR = 2.5     # a height difference beyond this is another floor
POINT = re.compile(r"\(([-\d.]+) ([-\d.]+) ([-\d.]+)\)")
DOORMARK = re.compile(r"doormark ([0-9A-F]{8}-[0-9A-F]{8}-[0-9A-F]{8}-[0-9A-F]{8}) state=(auto|inner|locked|held)")
DOOR_STATES = {"auto": 0, "inner": 1, "locked": 2, "held": 3}
MODE_SURVIVAL, MODE_MERCENARIES, MODE_OBJECTIVE, MODE_ZONE, MODE_VERSUS = range(5)


def parse(path):
    samples, authored = [], {"zone": [], "capsule": [], "respawn": []}
    skips = []   # skip=<from>-<to>: seconds of the recording that are no arena (build_arenas.py)
    for line in path.read_text(encoding="utf-8").splitlines():
        key, _, value = line.partition("=")
        if key == "skip":
            span = re.fullmatch(r"\s*([\d.]+)\s*-\s*([\d.]+)\s*", value)
            if span:
                skips.append((float(span.group(1)), float(span.group(2))))
            continue
        stamp = re.match(r"t=([\d.]+) ", line)
        if stamp and any(a <= float(stamp.group(1)) <= b for a, b in skips):
            continue
        if key in authored and not line.startswith("t="):
            nums = [float(v) for v in value.replace(",", " ").split()]
            if len(nums) not in (3, 4):
                raise SystemExit("%s: %s= needs x y z [radius]: %s" % (path, key, line))
            authored[key].append((tuple(nums[:3]), nums[3] if len(nums) == 4 else 0.0))
            continue
        if " pos " in line or " spot " in line or "player_at_start=" in line:
            m = POINT.search(line)
            if m:
                p = tuple(round(float(v), 2) for v in m.groups())
                if p not in samples:
                    samples.append(p)
    if not samples:
        raise SystemExit("%s: no walk samples" % path)
    return samples, authored


def door_marks(paths):
    marks = {}   # GUID -> (state, pos); the last one wins
    for path in paths:
        if not path.exists():
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            m = DOORMARK.search(line)
            if not m:
                continue
            p = POINT.search(line[m.end():])
            pos = tuple(float(v) for v in p.groups()) if p else (0.0, 0.0, 0.0)
            marks[m.group(1)] = (DOOR_STATES[m.group(2)], pos)
    return {g: v for g, v in marks.items() if v[0]}


def floor_samples(samples):
    counts = {}
    for p in samples:
        k = round(p[1] / FLOOR_STEP)
        counts[k] = counts.get(k, 0) + 1
    floors = [k * FLOOR_STEP for k, n in counts.items() if n >= FLOOR_SEEN]
    return [p for p in samples if any(abs(p[1] - f) <= FLOOR_REACH for f in floors)]


def gap(a, b):
    if abs(a[1] - b[1]) > OTHER_FLOOR:
        return 1e9
    return ((a[0] - b[0]) ** 2 + (a[2] - b[2]) ** 2) ** 0.5


def spread(points):
    chosen = [points[0]]
    while len(chosen) < MAX_POINTS:
        best = max(points, key=lambda p: min(gap(p, c) for c in chosen))
        if min(gap(best, c) for c in chosen) < MIN_GAP:
            break
        chosen.append(best)
    return chosen


def c_points(rows):
    return ",".join("{{%.2ff,%.2ff,%.2ff},%.2ff}" % (p[0], p[1], p[2], r) for p, r in rows) or "{{0,0,0},0}"


def main():
    arenas = sorted((ROOT / "arenas").glob("arena_*.txt"))
    if not arenas:
        raise SystemExit("no arenas/arena_*.txt")
    blocks, rows = [], []
    for path in arenas:
        name = path.stem
        samples, authored = parse(path)
        floor = floor_samples(samples)
        if samples[0] not in floor:
            floor.insert(0, samples[0])
        points = spread(floor)
        zones = authored["zone"] or [(p, START_RADIUS if i == 0 else 0.0) for i, p in enumerate(points[:ZONE_COUNT])]
        capsules = authored["capsule"] or [(p, 0.0) for p in points[1:]]
        respawns = authored["respawn"] or [(p, 0.0) for p in points]
        modes = 1 << MODE_SURVIVAL
        if len(capsules) >= 3:
            modes |= 1 << MODE_MERCENARIES
        if len(zones) >= 3:
            modes |= 1 << MODE_ZONE
        ident = name.upper()
        marks = door_marks([path, ROOT / "arenas" / "doors" / (name + ".txt")])
        mark_rows = ",".join("{{%s}, %d, {%.2ff, %.2ff, %.2ff}}" % (",".join("0x%sU" % w for w in g.split("-")), s, *pos)
                             for g, (s, pos) in sorted(marks.items()))
        blocks.append("inline constexpr DoorMark k%sDoorMarks[] = {%s};" % (ident, mark_rows or "{{0, 0, 0, 0}, 0, {0, 0, 0}}"))
        blocks.append("inline constexpr Point k%sZones[] = {%s};" % (ident, c_points(zones)))
        blocks.append("inline constexpr Point k%sCapsules[] = {%s};" % (ident, c_points(capsules)))
        blocks.append("inline constexpr Point k%sRespawns[] = {%s};" % (ident, c_points(respawns)))
        rows.append('    {"%s", k%sZones, %d, k%sCapsules, %d, k%sRespawns, %d, 0x%02Xu, k%sDoorMarks, %d},' % (
            name, ident, len(zones), ident, len(capsules), ident, len(respawns), modes, ident, len(marks)))
        for g, (s, pos) in sorted(marks.items()):
            print("   door mark %s: %s at (%.1f %.1f %.1f)" % (g, [k for k, v in DOOR_STATES.items() if v == s][0], *pos))
        print("%s: %d samples, %d on floors, %d points (%s); zones %d%s, capsules %d%s, respawn %d%s; modes %02X" % (
            name, len(samples), len(floor), len(points), " ".join("(%.1f %.1f %.1f)" % p for p in points),
            len(zones), " authored" if authored["zone"] else "", len(capsules), " authored" if authored["capsule"] else "",
            len(respawns), " authored" if authored["respawn"] else "", modes))
    text = [
        "// Generated by tools/build_arena_points.py from arenas/*.txt. Do not edit; add zone=,",
        "// capsule= or respawn= lines to the arena file instead.",
        "#pragma once",
        "#include <cstdint>",
        "namespace ds2arena {",
        "struct Point { float pos[3]; float radius; };   // radius 0: the mode's default",
        "// a door marked in the game: state 1 inner (never locked), 2 locked, 3 held open. Sorted by GUID.",
        "struct DoorMark { uint32_t guid[4]; uint8_t state; float pos[3]; };",
        "// modes: bit = gamemode::Mode (0 survival, 1 mercenaries, 2 objective, 3 zone, 4 versus)",
        "struct ArenaPoints { const char* name; const Point* zones; unsigned zoneCount; const Point* capsules; unsigned capsuleCount;",
        "                     const Point* respawns; unsigned respawnCount; uint8_t modes; const DoorMark* doorMarks; unsigned doorMarkCount; };",
        *blocks,
        "inline constexpr ArenaPoints kArenaPoints[] = {",
        *rows,
        "};",
        "inline constexpr unsigned kArenaPointsCount = sizeof(kArenaPoints) / sizeof(kArenaPoints[0]);",
        "}  // namespace ds2arena",
        "",
    ]
    OUT.write_bytes("\n".join(text).encode("ascii"))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
