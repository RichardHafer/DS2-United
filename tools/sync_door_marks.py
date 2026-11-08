"""Copy the door marks made in running arenas into the repo.

The door key writes every press to <game>\\plugins\\DS2Arenas\\doors\\<arena>.txt; the
game reads that file again at the next start of the arena. The build reads
arenas/doors/<arena>.txt (tools/build_arena_points.py), so a mark only reaches the
generated header once it is here. For each door the game's last mark is appended
when it differs from the mark the repo file ends with; hand comments stay.

usage: python -I tools/sync_door_marks.py [--dry]
then:  python -I tools/build_arena_points.py
"""
import datetime
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ds2_paths import GAME_DIR  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
MARK = re.compile(r"^doormark ([0-9A-F]{8}-[0-9A-F]{8}-[0-9A-F]{8}-[0-9A-F]{8}) state=(auto|inner|locked|held)\b(.*)$")


def last_marks(path):
    """GUID -> (state, rest of line), the last line of each door, in first-seen order."""
    out = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = MARK.match(line.strip())
            if m:
                out[m.group(1)] = (m.group(2), m.group(3).strip())
    return out


def main():
    dry = "--dry" in sys.argv[1:]
    game = GAME_DIR / "plugins" / "DS2Arenas" / "doors"
    if not game.is_dir():
        raise SystemExit("no %s" % game)
    today = datetime.date.today().strftime("%d.%m.%Y")
    changed = 0
    for src in sorted(game.glob("arena_*.txt")):
        theirs = last_marks(src)
        dst = ROOT / "arenas" / "doors" / src.name
        ours = last_marks(dst)
        # no line in the repo is the same as auto: the door table decides
        new = [(g, s, rest) for g, (s, rest) in theirs.items() if ours.get(g, ("auto", ""))[0] != s]
        if not new:
            print("%s: %d door(s), nothing new" % (src.stem, len(theirs)))
            continue
        lines = ["# marked in the running arena (door key), copied %s" % today]
        lines += ["doormark %s state=%s %s" % (g, s, rest) if rest else "doormark %s state=%s" % (g, s) for g, s, rest in new]
        for g, s, _ in new:
            print("%s: %s %s -> %s" % (src.stem, g, ours[g][0] if g in ours else "(none)", s))
        changed += len(new)
        if dry:
            continue
        old = dst.read_bytes() if dst.exists() else b""
        nl = b"\r\n" if b"\r\n" in old else b"\n"
        if old and not old.endswith(nl):
            old += nl
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(old + nl.join(line.encode("utf-8") for line in lines) + nl)
    print("%d mark(s) %s" % (changed, "would be copied (dry run)" if dry else "copied; now run tools/build_arena_points.py" if changed else "copied"))


if __name__ == "__main__":
    main()
