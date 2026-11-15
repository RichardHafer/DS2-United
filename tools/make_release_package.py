"""Build the public release zip of Dead Space 2 United.

The plugin and the settings window come from build\\, the settings from a release INI
(default: the repository's DS2United.ini) with the personal values cleared, the game
data the plugin loads (enemy pool, Severed and Twitcher records, the frontend and menu
copies) from an installed game folder, and the United edition of MarkerPatch as dinput8.dll
(build_markerpatch_united.cmd). The Outbreak archive is not packed; INSTALL.txt links it. The zip unpacks straight into the Dead Space 2 folder.

    python -I tools/make_release_package.py --game "<Dead Space 2>" --out <zip>
        [--ini <release DS2United.ini>] [--version 0.1] [--list] [--force]
"""
import argparse
import collections
import datetime
import hashlib
import re
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_playtest_package as playtest  # noqa: E402

REPO = Path(__file__).resolve().parent.parent

# packed from the game folder: (path or glob, required, why)
GAME_FILES = [
    (r"randomizer\ds2_randomizer_pool.str", True, "enemy pool stream"),
    (r"randomizer\ds2_randomizer_pool.toc", True, "enemy pool TOC"),
    (r"randomizer\ds2_ubermorph.str", True, "sixth necro type record"),
    (r"randomizer\ds2_ubermorph.toc", True, "TOC of the record above"),
    (r"randomizer\ds2_leaper.str", True, "Leaper record"),
    (r"randomizer\ds2_leaper.toc", True, "TOC of the record above"),
    (r"randomizer\ds2_twitcher_*.str", True, "Twitcher looks"),
    (r"randomizer\ds2_twitcher_*.toc", True, "TOCs of the Twitcher looks"),
    (r"randomizer\ds2_severed_*.str", True, "Severed records: Gabe's patrol helmet, the Severed pregnant"),
    (r"randomizer\ds2_severed_*.toc", True, "TOCs of the Severed records"),
    (r"randomizer\ds2coop_frontend.str", True, "main menu copy: game mode row, title logo, map pictures, suit screens"),
    (r"randomizer\ds2coop_frontend_stock.str", True, "the same main menu in the game's own colours ([Menu] FrontendColours=stock)"),
    (r"randomizer\ds2coop_mpskins.lua", True, "suit list with the campaign suits"),
    (r"randomizer\ds2coop_global_assets.str", True, "red in-game menus"),
    (r"randomizer\ds2coop_global_mp_assets.str", True, "red multiplayer screens"),
    (r"randomizer\ds2coop_loading_screen.str", True, "red loading screens"),
    (r"randomizer\ds2coop_partner_paths.bin", False, "recorded partner paths"),
    (r"plugins\DS2Zones.txt", True, "Hold the Zone spots"),
    (r"plugins\DS2Capsules.txt", False, "Mercenaries capsule spots"),
    (r"plugins\DS2SpawnerRules.tsv", False, "per-spawner rules"),
    (r"plugins\DS2ExtraSpawns.tsv", False, "hand-placed extra enemies"),
    (r"plugins\DS2VentDonorBlocklist.tsv", False, "vent donors known to get stuck"),
    (r"plugins\dementia\*", False, "[Dementia] sounds and Nicole's lines"),
]

# packed from the repository: (source, path in the zip)
REPO_FILES = [
    (r"build\DS2United.asi", r"plugins\DS2United.asi"),
    (r"build\DS2UnitedConfig.exe", r"DS2UnitedConfig.exe"),
    (r"build\markerpatch-united\dinput8.dll", r"dinput8.dll"),
    (r"third_party\MarkerPatch-United\LICENSE", r"MarkerPatch_United_LICENSE.txt"),
    (r"third_party\MarkerPatch-United\README.md", r"MarkerPatch_United_README.md"),
    (r"third_party\MarkerPatch-United\MarkerPatch.ini", r"MarkerPatch.ini"),
    (r"README.md", r"DS2United_README.md"),
    (r"LICENSE", r"DS2United_LICENSE.txt"),
]

# values cleared in the packed INI: (section, key), lower case; Peer1..PeerN count as Peer
PERSONAL = {("nameplate", "name"), ("coop", "joinaddress"), ("coop", "peer"), ("coop", "machineindex"),
            ("coop", "hostid"), ("voices", "pack")}
# Versus is not in the release: its sections and keys leave the packed INI, with the comment
# lines right above a removed key
DROPPED_SECTIONS = {"versus", "versuspoints", "versusslashers", "versusskins"}
DROPPED_KEYS = {("bot", "versusdeaths"), ("bot", "versusrespawnms"), ("progress", "versusrows")}
# values the release forces: (section, key) -> (value, why)
FORCED = {("menu", "mode"): ("0", "versus is not in the release")}

INSTALL = """Dead Space 2 United {version}
{line}

A HIGHLY EXPERIMENTAL RELEASE. Expect crashes and bugs, and back up your save
games before you play.

1. Close the game and extract this archive into the Dead Space 2 folder,
   next to deadspace2.exe. Let it overwrite when asked.
2. dinput8.dll is United's own edition of the Marker Patch (by Wemino,
   https://github.com/Wemino/MarkerPatch). It replaces an official Marker
   Patch, reads the same MarkerPatch.ini and keeps its fixes, except a few
   that clash with the mod (MarkerPatch_United_README.md lists them). Do not
   put the official one back: with it the mod stays off and says so.
   MarkerPatch.ini holds its options with the MarkerPatch defaults. If you
   already have a MarkerPatch.ini with your own settings, keep yours: do not
   let the archive overwrite it.
3. Start DS2UnitedConfig.exe to look through the settings (optional).
4. Start the game. F1 in the game shows every key of the mod.

Updating from the Co-op Mod: delete plugins\\DS2Coop.asi (or HaferSprawl.asi).
Your old DS2Coop.ini is taken over the first time the settings window opens.
Keep your own plugins\\DS2United.ini when you update: extract everything except
that file, or let the settings window merge it.

Optional: the Outbreak maps (Concourse and Academy) need DS2DAT12.DAT from
Mark's Dead Space 2 Multiplayer: https://www.nexusmods.com/deadspace2/mods/181
Without it they are just not in the map list.

Co-op: every player needs this same release. Host co-op and join co-op are in
the main menu; the joiner enters the host's address in the settings window
(Co-op, Join address). The game uses UDP on the port, port + 1000 and port + 2000.

Problems: send plugins\\DS2United.log, DS2Randomizer.log and DS2Bots.log and
say where you were. Discord: https://discord.com/invite/sWMRkYJ5JV
"""


def sha1(path):
    s = hashlib.sha1()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 22), b""):
            s.update(block)
    return s.hexdigest()


def clean_ini(raw):
    """The release INI with the personal values cleared. Returns (bytes, notes)."""
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = (raw[3:] if bom else raw).decode("utf-8", "surrogateescape")
    out, notes, section, comments = [], [], "", []
    dropped = collections.Counter()
    for line in text.splitlines(keepends=True):
        m = re.match(r"\s*\[([^\]]+)\]", line)
        if m:
            section = m.group(1).strip().lower()
        if section in DROPPED_SECTIONS:
            dropped[section] += 1
            continue
        if line.lstrip().startswith(";"):
            comments.append(line)
            continue
        key = re.match(r"([A-Za-z0-9_]+)\s*=(.*?)(\r?\n)?$", line)
        if key:
            name = key.group(1).lower()
            base = "peer" if re.fullmatch(r"peer\d*", name) else name
            if (section, name) in DROPPED_KEYS:
                notes.append("[%s] %s removed (with %d comment line(s))" % (section, key.group(1), len(comments)))
                comments = []
                continue
            if (section, base) in PERSONAL and key.group(2).strip():
                notes.append("[%s] %s cleared (was %r)" % (section, key.group(1), key.group(2).strip()))
                line = key.group(1) + "=" + (key.group(3) or "")
            forced = FORCED.get((section, name))
            if forced and key.group(2).strip() != forced[0]:
                notes.append("[%s] %s set to %s (was %r): %s" % (section, key.group(1), forced[0], key.group(2).strip(), forced[1]))
                line = key.group(1) + "=" + forced[0] + (key.group(3) or "")
        out += comments
        comments = []
        out.append(line)
    out += comments
    for s, n in sorted(dropped.items()):
        notes.append("[%s] removed (%d lines)" % (s, n))
    data = "".join(out).encode("utf-8", "surrogateescape")
    return (b"\xef\xbb\xbf" + data) if bom else data, notes


def expand(game):
    plan, missing = [], []
    for pattern, required, why in GAME_FILES:
        if any(c in pattern for c in "*?["):
            folder, mask = pattern.rsplit("\\", 1)
            hits = sorted(p for p in (game / folder).glob(mask) if p.is_file() and not playtest.never(p.name)) if (game / folder).is_dir() else []
        else:
            hits = [game / pattern] if (game / pattern).is_file() else []
        if not hits and required:
            missing.append(pattern)
        plan += [(str(p.relative_to(game)), p, why) for p in hits]
    return plan, missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True, help="an installed Dead Space 2 folder with the mod's game data")
    ap.add_argument("--out", required=True, help="zip to write")
    ap.add_argument("--ini", default=str(REPO / "DS2United.ini"), help="release DS2United.ini")
    ap.add_argument("--version", default="0.1.2")
    ap.add_argument("--list", action="store_true", help="print the plan, write nothing")
    ap.add_argument("--force", action="store_true", help="overwrite an existing zip")
    args = ap.parse_args()

    game, out = Path(args.game), Path(args.out)
    plan, missing = expand(game)
    repo_plan = []
    for src, dst in REPO_FILES:
        p = REPO / src
        if p.is_file():
            repo_plan.append((dst, p))
        else:
            missing.append(src)
    ini_raw = Path(args.ini).read_bytes()
    ini_data, notes = clean_ini(ini_raw)

    mp = REPO / "build" / "markerpatch-united" / "dinput8.dll"
    if mp.is_file() and b"MarkerPatchUnitedEdition" not in mp.read_bytes():
        print("build\\markerpatch-united\\dinput8.dll has no MarkerPatchUnitedEdition export; run build_markerpatch_united.cmd")
        return 1
    installed = game / "plugins" / "DS2United.asi"
    built = REPO / "build" / "DS2United.asi"
    if installed.is_file() and built.is_file() and sha1(installed) != sha1(built):
        print("note: the installed DS2United.asi differs from build\\DS2United.asi; the zip takes the build")

    total = sum(p.stat().st_size for _, p, _ in plan) + sum(p.stat().st_size for _, p in repo_plan) + len(ini_data)
    for rel, p, why in plan:
        print("game  %-45s %11d  %s" % (rel, p.stat().st_size, why))
    for dst, p in repo_plan:
        print("repo  %-45s %11d" % (dst, p.stat().st_size))
    print("ini   %-45s %11d  from %s" % (r"plugins\DS2United.ini", len(ini_data), args.ini))
    for n in notes:
        print("      " + n)
    print("total %.1f MB in %d files" % (total / 1048576.0, len(plan) + len(repo_plan) + 2))
    if missing:
        print("MISSING: " + ", ".join(missing))
        return 1
    if args.list:
        return 0
    if out.exists() and not args.force:
        print("%s exists; --force overwrites it" % out)
        return 1

    stamp = datetime.date.today().isoformat()
    title = "Dead Space 2 United %s" % args.version
    manifest = []
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for rel, p, _ in plan:
            z.write(p, rel)
            manifest.append("%s  %s" % (sha1(p), rel))
        for dst, p in repo_plan:
            z.write(p, dst)
            manifest.append("%s  %s" % (sha1(p), dst))
        z.writestr(r"plugins\DS2United.ini", ini_data)
        manifest.append("%s  %s" % (hashlib.sha1(ini_data).hexdigest(), r"plugins\DS2United.ini"))
        z.writestr("DS2United_INSTALL.txt", INSTALL.format(version=args.version, line="=" * len(title) + " (" + stamp + ")").replace("\n", "\r\n"))
        z.writestr("DS2United_MANIFEST.txt", ("sha1 of every file, " + title + ", " + stamp + "\n" + "\n".join(manifest) + "\n").replace("\n", "\r\n"))
    print("wrote %s (%.1f MB)" % (out, out.stat().st_size / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
