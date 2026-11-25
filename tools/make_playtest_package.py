"""Pack what a second player needs for an online Dead Space 2 United playtest.

Reads the host's installed game folder (never writes to it) and builds one zip
with exactly the files listed in FILES, relative paths kept, plus a manifest
(sha1 per file) and a short install note. Logs, backups, retired plugins, crash
dumps and per-player state are never packed; the leftovers of plugins\\ and
randomizer\\ are printed with the reason they stay out.

    python -I tools/make_playtest_package.py --game "<Dead Space 2>" --out <zip>
        [--no-outbreak] [--no-markerpatch] [--join-address HOST:PORT]
        [--repo <repo>] [--asi <DS2United.asi>] [--list] [--force]

--no-outbreak     leave out DS2DAT12.DAT (1.3 GB); the host must then not pick
                  the Outbreak maps, the lobby refuses the join for them
--no-markerpatch  leave out MarkerPatch (dinput8.dll, MarkerPatch.ini,
                  achievements\\) when the partner has the identical files
--join-address    written to [Coop] JoinAddress in the packed DS2United.ini
--repo            repo to take REPO_FILES from (default: this script's repo);
                  also compares the installed plugin with <repo>\\build
--asi             pack this DS2United.asi instead of the installed one (a build not
                  installed yet; the host must install the same file to play)
--list            print the plan and the checks, write nothing
"""
import argparse
import datetime
import fnmatch
import hashlib
import os
import re
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

# deadspace2.exe of Steam app 47780, build 21478 (depot 47781 manifest 6851344156015013660).
STEAM_EXE_SHA1 = "653c83327049e4c6076c38c53ae552b0507910d2"
STEAM_EXE_SIZE = 48444416
POOL_FILES = (r"randomizer\ds2_randomizer_pool.str", r"randomizer\ds2_randomizer_pool.toc")
UDP_PORTS = "UDP Port, Port+1000, Port+2000 (27015, 28015, 29015 by default)"

# (path or glob relative to the game folder, group, required, why)
FILES = [
    (r"plugins\DS2United.asi", "core", True, "the mod (randomizer + co-op); the lobby refuses any other build"),
    (r"plugins\DS2United.ini", "core", True, "all settings; read instead of the old per-plugin INIs"),
    (r"DS2UnitedConfig.exe", "core", True, "settings window; looks for plugins\\DS2United.ini next to itself"),
    (r"randomizer\ds2_randomizer_pool.str", "core", True, "enemy pool stream, size and FNV-1a checked by the plugin"),
    (r"randomizer\ds2_randomizer_pool.toc", "core", True, "enemy pool TOC, size and FNV-1a checked by the plugin"),
    (r"randomizer\ds2_ubermorph.str", "core", True, "[Necro] LeaperRecordStream record (sixth necro type)"),
    (r"randomizer\ds2_ubermorph.toc", "core", True, "TOC of the record above"),
    (r"randomizer\ds2_leaper.str", "core", True, "built-in default LeaperRecordStream"),
    (r"randomizer\ds2_leaper.toc", "core", True, "TOC of the record above"),
    (r"randomizer\ds2coop_frontend.str", "core", True, "[Menu] NativeModeRow + [Loadout]: frontend copy with Mark's MP20 movie and the suit pictures"),
    (r"randomizer\ds2coop_mpskins.lua", "core", True, "[Loadout]: mpskins.lua with the campaign suit rows"),
    (r"plugins\DS2Zones.txt", "core", True, "Hold the Zone spots recorded in game"),
    (r"plugins\DS2Capsules.txt", "core", False, "Mercenaries capsule spots recorded in game"),
    (r"plugins\DS2SpawnerRules.tsv", "core", False, "per-spawner block/allow rules"),
    (r"plugins\DS2ExtraSpawns.tsv", "core", False, "hand-placed extra enemies"),
    (r"plugins\DS2VentDonorBlocklist.tsv", "core", False, "vent donors known to get stuck"),
    (r"randomizer\ds2_twitcher_*.str", "core", False, "Twitcher looks (TwitcherPercent, TwitcherChance)"),
    (r"randomizer\ds2_twitcher_*.toc", "core", False, "TOCs of the Twitcher looks"),
    (r"randomizer\ds2_severed_pregnant.str", "core", False, "Severed records: the Severed pregnant"),
    (r"randomizer\ds2_severed_pregnant.toc", "core", False, "TOCs of the Severed records"),
    (r"plugins\dementia\*", "core", False, "[Dementia] sounds and Nicole's lines"),
    (r"plugins\voices\**\*", "core", False, "[Voices] situation voice packs"),
    (r"randomizer\ds2coop_global_assets.str", "core", False, "global_assets copy the plugin loads"),
    (r"randomizer\ds2coop_global_mp_assets.str", "core", False, "MP global_assets copy the plugin loads"),
    (r"randomizer\ds2coop_loading_screen.str", "core", False, "loading screen the plugin loads"),
    (r"randomizer\ds2coop_partner_paths.bin", "core", False, "recorded partner paths"),
    (r"dinput8.dll", "markerpatch", True, "MarkerPatch: the ASI loader for plugins\\*.asi; the plugin works around its hooks"),
    (r"MarkerPatch.ini", "markerpatch", True, "MarkerPatch settings (LoadASIPlugins=1, CheckLAAPatch=0)"),
    (r"achievements\img\*.png", "markerpatch", False, "MarkerPatch achievement overlay (AchievementSupport=1)"),
    (r"achievements\txt\*.txt", "markerpatch", False, "MarkerPatch achievement texts"),
    (r"DS2DAT12.DAT", "outbreak", True, "Outbreak maps (Concourse, Academy) from Mark's multiplayer install"),
]

# (path in the repo, path in the package, why); packed from the repo, not the game folder.
# deadspace2_4gb.exe itself is never shipped, the partner makes it from their own exe.
REPO_FILES = [
    (r"coop_launchers\DS2_Make_4GB_Copy.cmd", r"DS2_Make_4GB_Copy.cmd", "makes deadspace2_4gb.exe (4 GB flag) from deadspace2.exe"),
    (r"coop_launchers\DS2_Make_4GB_Copy.ps1", r"DS2_Make_4GB_Copy.ps1", "the patcher the .cmd runs"),
    (r"coop_launchers\DS2_Start_4GB.cmd", r"DS2_Start_4GB.cmd", "starts deadspace2_4gb.exe"),
]

# Never packed, whatever FILES says (logs, backups, dumps, per-player state).
NEVER = ["*.log", "*.bak", "*.bak-*", "*.bak_*", "*.off-*", "*.inuse-*", "*.previous", "*.wrong", "*.user-backup",
         "*.dmp", "deadspace2.crash.*", "deadspace2.hang.*", "*.tmp", "DS2SurvivalBest.txt", "DS2Profile.ini",
         "DS2SpawnerRecent.tsv", "*.ds2bots"]

# Why the rest of plugins\ and randomizer\ stays out.
LEFT_OUT = [
    ("*.log", "log"),
    ("*.bak*", "backup"), ("*.previous", "backup"), ("*.wrong", "backup"), ("*.user-backup", "backup"),
    ("*.inuse-*", "retired plugin"), ("*.off-*", "retired plugin"),
    ("DS2SurvivalBest.txt", "personal high scores"),
    ("DS2Profile.ini", "personal loadout pick and XP, written by the plugin"),
    ("DS2SpawnerRecent.tsv", "runtime spawn list for the Spawner Editor"),
    ("DS2BotSaves", "bot save sidecars of the host's own save slots"),
    ("DS2Arenas", "arena recorder output, written only"),
    ("DS2Randomizer.ini", "legacy, superseded by DS2United.ini"),
    ("DS2Survival.ini", "legacy, superseded by DS2United.ini"),
    ("DS2BotProfiles.ini", "legacy, superseded by DS2United.ini"),
    ("DS2_Runtime_Text.bin", "old text dump, not read by the plugin"),
    ("CameraPaths", "camera paths recorded by the host"),
    ("DS2ClientProgress", "the host's own client progress"),
    ("DS2Coop.legacy.ini", "the co-op half's old INI, kept by the migration"),
    ("Spezielle_Spawns_Pruefen.txt", "personal notes"),
    ("achievements", "old plugin's assets (MarkerPatch now reads the game folder's achievements)"),
    ("fonts", "old plugin's assets"),
    ("presets", "another mod's presets"),
    ("ds2_randomizer_models.*", "skin stream, not loaded by this build (no model streams compiled in)"),
    ("ds2coop_loadout_rows.tsv", "build by-product, compiled into the plugin"),
]
# Other DLLs a loader could hide behind; a second ASI loader loads every plugin twice.
LOADER_NAMES = {"d3d9.dll", "d3d8.dll", "d3d11.dll", "dinput.dll", "dsound.dll", "winmm.dll", "version.dll",
                "wininet.dll", "winhttp.dll", "xinput1_3.dll", "binkw32.dll", "vorbisfile.dll", "msacm32.dll"}


def fnv1a64(path):
    h = 14695981039346656037
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            for b in block:
                h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def sha1_file(path):
    s = hashlib.sha1()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 22), b""):
            s.update(block)
    return s.hexdigest()


def link_stamp(path):
    data = path.read_bytes()[:4096]
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    return struct.unpack_from("<I", data, pe + 8)[0]


def never(name):
    return any(fnmatch.fnmatch(name.lower(), p.lower()) for p in NEVER)


def game_running():
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq deadspace2*", "/FO", "CSV", "/NH"], capture_output=True).stdout
    except OSError:
        return None
    # the output is in the OEM code page; only the image name matters
    out = (out or b"").lower()
    return b'"deadspace2.exe"' in out or b'"deadspace2_4gb.exe"' in out


def expand(game, groups):
    """[(rel, abs, group, why)] and the missing required entries."""
    plan, missing = [], []
    for pattern, group, required, why in FILES:
        if group not in groups:
            continue
        if "\\**\\" in pattern:
            # a whole folder tree: folder\\**\\mask
            folder, mask = pattern.split("\\**\\", 1)
            hits = sorted(p for p in (game / folder).glob("**/" + mask) if p.is_file() and not never(p.name)) if (game / folder).is_dir() else []
            if not hits and required:
                missing.append(pattern)
            for p in hits:
                plan.append((str(p.relative_to(game)), p, group, why))
        elif any(c in pattern for c in "*?["):
            folder, mask = os.path.split(pattern)
            hits = sorted(p for p in (game / folder).glob(mask) if p.is_file()) if (game / folder).is_dir() else []
            if not hits and required:
                missing.append(pattern)
            for p in hits:
                plan.append((str(p.relative_to(game)), p, group, why))
        else:
            p = game / pattern
            if p.is_file():
                plan.append((pattern, p, group, why))
            elif required:
                missing.append(pattern)
            else:
                print("note: optional %s not present, nothing to pack" % pattern)
    return plan, missing


def edit_ini(raw, join_address):
    """[Nameplate] Name blanked, [Coop] JoinAddress set when given. Returns (bytes, notes)."""
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = (raw[3:] if bom else raw).decode("utf-8", "surrogateescape")
    notes, out, section = [], [], ""
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        m = re.match(r"\[([^\]]+)\]", stripped)
        if m:
            section = m.group(1).strip().lower()
        key = re.match(r"([A-Za-z0-9_]+)\s*=(.*?)(\r?\n)?$", line)
        if key and section == "nameplate" and key.group(1).lower() == "name" and key.group(2).strip():
            line = "Name=" + (key.group(3) or "")
            notes.append("[Nameplate] Name blanked (was %r)" % key.group(2).strip())
        if key and section == "coop" and key.group(1).lower() == "joinaddress":
            current = key.group(2).strip()
            if join_address:
                line = "JoinAddress=" + join_address + (key.group(3) or "")
                notes.append("[Coop] JoinAddress %s -> %s" % (current, join_address))
            elif current.startswith("127.") or not current:
                notes.append("[Coop] JoinAddress is %r: the partner must set the host's address" % current)
        out.append(line)
    data = "".join(out).encode("utf-8", "surrogateescape")
    return (b"\xef\xbb\xbf" + data) if bom else data, notes


def left_out(game, planned):
    rows = []
    for folder in ("plugins", "randomizer"):
        base = game / folder
        if not base.is_dir():
            continue
        for p in sorted(base.iterdir(), key=lambda x: x.name.lower()):
            rel = str(p.relative_to(game))
            if rel.lower() in planned or (p.is_dir() and any(x.startswith(rel.lower() + "\\") for x in planned)):
                continue
            reason = next((r for pat, r in LEFT_OUT if fnmatch.fnmatch(p.name.lower(), pat.lower())), None)
            if reason is None and p.is_file() and p.suffix.lower() == ".asi":
                reason = "OTHER PLUGIN - it would load next to DS2United.asi"
            rows.append((rel + ("\\" if p.is_dir() else ""), reason or "UNKNOWN - check whether the partner needs it"))
    return rows


def readme(stamp, exe_ok, groups, ini_notes):
    lines = [
        "Dead Space 2 United online playtest package",
        "===================================",
        "",
        "Built %s. Lobby build id %08X: both players must run this exact DS2United.asi," % (datetime.date.today().isoformat(), stamp),
        "the lobby refuses any other build. DS2Coop_playtest_manifest.txt lists every file with its sha1.",
        "",
        "Before installing",
        "  * Dead Space 2 from Steam (English), unmodified. In PowerShell, in the game folder:",
        "        Get-FileHash -Algorithm SHA1 deadspace2.exe",
        "    must print %s. Origin/EA app copies are not supported." % STEAM_EXE_SHA1.upper(),
        "  * Remove plugins\\HaferSprawl.asi and plugins\\DS2RandomizerObserver.asi if you have them,",
        "    and any other .asi or second ASI loader (winmm.dll, d3d9.dll ...) in the game folder.",
    ]
    if "markerpatch" not in groups:
        lines.append("  * MarkerPatch (dinput8.dll + MarkerPatch.ini) must be the same version as the host's.")
    lines += [
        "",
        "Install",
        "  1. Unzip into the Dead Space 2 folder (where deadspace2.exe is) and overwrite.",
        "  2. plugins\\DS2United.ini: [Coop] JoinAddress=<host address>:27015, optionally",
        "     [Nameplate] Name=<your name>. Change nothing else; both machines need the same settings.",
        "  3. Cap the frame rate at 60. Turn off RivaTuner and the Discord overlay for the test.",
        "  4. Double-click DS2_Make_4GB_Copy.cmd once. It writes deadspace2_4gb.exe next to deadspace2.exe:",
        "     the same game with 4 GB of memory instead of 2, which avoids out-of-memory crashes with the",
        "     bigger enemy pool. deadspace2.exe itself is not changed. Run it again after every game update.",
        "  5. With Steam running, start the game with DS2_Start_4GB.cmd. Starting through Steam also works,",
        "     but then the game only gets 2 GB.",
        "",
        "Play",
        "  Campaign: the host picks \"co-op\" in the main menu (it replaces single player) and starts or loads a game.",
        "            The friend picks \"join co-op\" (it replaces \"previously on Dead Space\"). The friend is pulled",
        "            in at the host's next checkpoint restart: the host restarts from the pause menu once the friend",
        "            shows up as joined.",
        "  Survival: main menu > survival > host co-op / join co-op; the friend joins before the host picks the map.",
        "  On joining, both screens say \"INI MATCHES ...\" or \"INI DIFFERS ...\"; plugins\\DS2United.log lists",
        "  every setting or game file that differs.",
        "",
        "Network: the host must be reachable on %s," % UDP_PORTS,
        "through an overlay VPN (Tailscale, ZeroTier) or port forwarding.",
    ]
    if "outbreak" not in groups:
        lines.append("Outbreak maps: DS2DAT12.DAT is not in this package; the host must not pick Concourse or Academy.")
    lines += [
        "",
        "After the test, send: plugins\\DS2United*.log, DS2Randomizer*.log, DS2Versus*.log, DS2Bots*.log",
        "(with their .prev/.previous copies) and any deadspace2.crash.* or deadspace2.hang.* files",
        "from the game folder or C:\\DS2CrashDumps.",
        "",
    ]
    if ini_notes:
        lines += ["Packed DS2United.ini: " + "; ".join(ini_notes), ""]
    if not exe_ok:
        lines += ["WARNING: the host's own deadspace2.exe did not match the Steam hash when this was built.", ""]
    return "\r\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", required=True, help="host's Dead Space 2 folder")
    ap.add_argument("--out", required=True, help="zip to write")
    ap.add_argument("--no-outbreak", action="store_true")
    ap.add_argument("--no-markerpatch", action="store_true")
    ap.add_argument("--join-address", default="")
    ap.add_argument("--repo", default="")
    ap.add_argument("--asi", default="", help="DS2United.asi to pack instead of the installed one")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--force", action="store_true", help="overwrite an existing zip")
    args = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)

    game = Path(args.game).resolve()
    out = Path(args.out).resolve()
    if not (game / "deadspace2.exe").is_file():
        print("error: no deadspace2.exe in %s" % game)
        return 2
    try:
        out.relative_to(game)
        print("error: --out must not be inside the game folder")
        return 2
    except ValueError:
        pass
    if args.join_address and not re.fullmatch(r"[A-Za-z0-9.\-]+:\d{1,5}", args.join_address):
        print("error: --join-address must look like HOST:PORT")
        return 2
    groups = {"core", "launchers"} | (set() if args.no_markerpatch else {"markerpatch"}) | (set() if args.no_outbreak else {"outbreak"})
    errors, warnings = [], []

    running = game_running()
    print("game running: %s" % {True: "YES", False: "no", None: "unknown"}[running])
    if running:
        warnings.append("deadspace2.exe is running: DS2United.ini and the recorded spots may change while packing")

    exe = game / "deadspace2.exe"
    exe_sha = sha1_file(exe)
    exe_ok = exe_sha == STEAM_EXE_SHA1 and exe.stat().st_size == STEAM_EXE_SIZE
    print("deadspace2.exe %d bytes sha1 %s: %s" % (exe.stat().st_size, exe_sha, "Steam build 21478" if exe_ok else "NOT the Steam exe"))
    if not exe_ok:
        warnings.append("deadspace2.exe differs from the Steam build the plugin's signatures were taken from")

    plan, missing = expand(game, groups)
    repo = Path(args.repo).resolve() if args.repo else Path(__file__).resolve().parent.parent
    for src, rel, why in REPO_FILES:
        p = repo / src
        if p.is_file():
            plan.append((rel, p, "launchers", why))
        else:
            missing.append("repo " + src)
    for m in missing:
        errors.append("required file missing: " + m)
    for rel, _, _, _ in plan:
        if never(os.path.basename(rel)):
            errors.append("refusing to pack %s (log/backup/personal pattern)" % rel)

    asi = game / "plugins" / "DS2United.asi"
    if args.asi:
        asi = Path(args.asi).resolve()
        if not asi.is_file():
            errors.append("--asi %s not found" % asi)
        else:
            plan = [(rel, asi if rel.lower() == r"plugins\ds2coop.asi" else p, group, why) for rel, p, group, why in plan]
            warnings.append("packing %s, not the installed DS2United.asi: install the same file before playing together" % asi)
    stamp = 0
    if asi.is_file():
        stamp = link_stamp(asi)
        blob = asi.read_bytes()
        print("DS2United.asi lobby build id %08X (%s UTC)" % (stamp, datetime.datetime.fromtimestamp(stamp, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")))
        for rel in POOL_FILES:
            p = game / rel
            if not p.is_file():
                continue
            fp = fnv1a64(p)
            found = blob.find(struct.pack("<Q", fp)) >= 0
            print("%s fnv1a64 %016X %s" % (rel, fp, "expected by this DS2United.asi" if found else "NOT expected by this DS2United.asi"))
            if not found:
                errors.append("%s does not match the fingerprint compiled into DS2United.asi" % rel)
        if args.repo and not args.asi:
            built = Path(args.repo) / "build" / "DS2United.asi"
            if built.is_file():
                b = link_stamp(built)
                print("repo build\\DS2United.asi build id %08X: %s" % (b, "same as installed" if b == stamp else "DIFFERENT from installed"))
                if b != stamp:
                    warnings.append("the repo has a different DS2United.asi build than the game folder; install it first if that is the one the host will run")

    for name in ("HaferSprawl.asi", "DS2RandomizerObserver.asi"):
        if (game / "plugins" / name).exists():
            warnings.append("plugins\\%s is active on the host next to DS2United.asi" % name)
    for p in sorted(game.glob("*.asi")):
        warnings.append("%s sits in the game folder itself (loaded twice?)" % p.name)
    for p in sorted(game.glob("*.dll")):
        if p.name.lower() in LOADER_NAMES:
            warnings.append("%s in the game folder could be a second ASI loader" % p.name)

    ini_bytes, ini_notes = None, []
    ini = game / "plugins" / "DS2United.ini"
    if ini.is_file():
        ini_bytes, ini_notes = edit_ini(ini.read_bytes(), args.join_address)
        for n in ini_notes:
            print("DS2United.ini: " + n)

    planned = {rel.lower() for rel, _, _, _ in plan}
    print("\nleft out of plugins\\ and randomizer\\:")
    by_reason = {}
    for rel, reason in left_out(game, planned):
        by_reason.setdefault(reason, []).append(rel)
        if reason.startswith(("UNKNOWN", "OTHER PLUGIN")):
            warnings.append("%s: %s" % (rel, reason))
    for reason, rels in sorted(by_reason.items(), key=lambda kv: (len(kv[1]) > 3, kv[0])):
        shown = rels if len(rels) <= 3 else rels[:2] + ["... %d more" % (len(rels) - 2)]
        print("  %-52s %s" % (reason, ", ".join(shown)))

    total = sum(p.stat().st_size for _, p, _, _ in plan)
    print("\nplan: %d files, %.1f MB uncompressed, groups %s" % (len(plan), total / 1e6, ", ".join(sorted(groups))))
    for w in warnings:
        print("WARNING: " + w)
    for e in errors:
        print("ERROR: " + e)
    if errors:
        return 1
    if args.list:
        for rel, p, group, why in plan:
            print("  %-44s %12d  [%s] %s" % (rel, p.stat().st_size, group, why))
        print("--list: nothing written")
        return 0
    if out.exists() and not args.force:
        print("error: %s exists (use --force)" % out)
        return 2

    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".part")
    manifest = []
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, allowZip64=True, compresslevel=6) as z:
        for rel, p, group, why in plan:
            arc = rel.replace("\\", "/")
            if ini_bytes is not None and rel.lower() == r"plugins\ds2coop.ini":
                info = zipfile.ZipInfo.from_file(p, arc)
                info.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(info, ini_bytes)
                digest, size = hashlib.sha1(ini_bytes).hexdigest(), len(ini_bytes)
                note = "edited copy" if ini_bytes != p.read_bytes() else ""
            else:
                info = zipfile.ZipInfo.from_file(p, arc)
                info.compress_type = zipfile.ZIP_DEFLATED
                s, size = hashlib.sha1(), 0
                with open(p, "rb") as src, z.open(info, "w", force_zip64=info.file_size >= 1 << 31) as dst:
                    for block in iter(lambda: src.read(1 << 22), b""):
                        s.update(block)
                        dst.write(block)
                        size += len(block)
                digest, note = s.hexdigest(), ""
            manifest.append("%s  %12d  %s  [%s]%s" % (digest, size, rel, group, " " + note if note else ""))
            print("packed %s" % manifest[-1])
        header = ["# Dead Space 2 United playtest package, %s" % datetime.datetime.now().isoformat(timespec="seconds"),
                  "# lobby build id %08X; deadspace2.exe must be sha1 %s" % (stamp, STEAM_EXE_SHA1),
                  "# sha1  size  path (relative to the Dead Space 2 folder)  [group]"]
        z.writestr("DS2Coop_playtest_manifest.txt", "\r\n".join(header + manifest) + "\r\n")
        z.writestr("DS2Coop_playtest_README.txt", readme(stamp, exe_ok, groups, ini_notes))
    if out.exists():
        out.unlink()
    tmp.rename(out)
    with zipfile.ZipFile(out) as z:
        bad = z.testzip()
        count = len(z.infolist())
    print("\nwrote %s: %d entries (%d game files + manifest + readme), %.1f MB%s" % (
        out, count, len(plan), out.stat().st_size / 1e6, "" if bad is None else ", CRC ERROR in " + bad))
    return 0 if bad is None else 1


if __name__ == "__main__":
    sys.exit(main())
