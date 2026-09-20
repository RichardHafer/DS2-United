"""Write the public playtest DS2Randomizer.ini: every key the plugin reads, but
laid out for someone who has never seen the project. The few settings a player
will touch come first with one or two plain lines each; everything else sits
under "Advanced" with a single line and the value the packer plays with.

Values are explicit here, not read from anybody's private INI, so the shipped
file never carries a tester's experiments. Variants are off in this build.

Usage: python tools/casual_ini.py [output path]
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FAMILIES = ["Zombie", "Exploder", "Leaper", "Lurker", "PackEnemy", "Pregnant", "Puker", "TubeBeetle", "Brute", "Infector",
            "Stalker", "Hunter", "Guardian", "GuardianPod", "GuardianTentacle", "Nest", "NestProjectile", "Landmine",
            "DividerCore", "DividerPart", "SwarmEnemy", "Tripod", "WindowTripod", "BossTripod", "TripodProjectile", "Kong",
            "NicoleBoss", "DragTentacle", "LeviathanTentacle", "Polyp", "SlamDragTentacle", "SlugTentacle", "StabTentacle"]
COMMON = ["Zombie", "Exploder", "Leaper", "Lurker", "PackEnemy", "Pregnant", "Puker", "TubeBeetle", "Infector", "Stalker", "SwarmEnemy"]
PLAIN = {"Zombie": "Slasher", "PackEnemy": "Pack", "TubeBeetle": "Crawler", "SwarmEnemy": "Swarm"}


def build():
    out = []
    rule = ";" + "=" * 70

    def title(text):
        out.extend(["", rule, "; " + text, rule, ""])

    def note(*lines):
        out.extend(("; " + line) if line else ";" for line in lines)

    def key(name, value, *comment):
        if comment:
            note(*comment)
        out.append("%s=%s" % (name, value))
        out.append("")

    def flat(name, value):
        out.append("%s=%s" % (name, value))

    out.extend([rule, "; Dead Space 2 Randomizer  (public playtest)", rule, "",
                "; Every switch is 0 = off, 1 = on. Restart the game after editing.",
                "; Back up your saves. Cap the game at 60 FPS (see README).",
                "; Keep your own settings between the two rulers below; leave the",
                "; Advanced part alone unless you know what a line does.", ""])

    out.append("[DS2Randomizer]")
    out.append("")
    note("--- Settings you may want to change ------------------------------")
    out.append("")
    key("Enabled", 1, "Master switch. 0 turns the whole mod off without uninstalling it.")
    key("Seed", 50, "Which enemies replace which. Same seed = same game every time.",
        "Change it for a different run. Do not change it mid-playthrough.")
    key("Chance", 100, "Percent of spawns that get replaced. 100 = all, 50 = half, 0 = none.")
    key("CrossFamily", 1, "1: a slasher can become a puker, a leaper a pack, and so on.",
        "0: enemies only swap with other looks of their own kind (much tamer).")
    key("FpsCap", 60, "The mod caps the game at this many frames per second. Above 60 the",
        "game's own scripts break (elevators, doors). 0 = no cap, use your own.")
    key("MoreEnemies", 0, "1: spawn more than one enemy per spawn point. How many is set",
        "per enemy type in [MoreEnemies] at the bottom. Off for the playtest.")
    key("SwarmHorde", 6, "When a swarm replaces an enemy, this many swarmers come at once.",
        "1 = a single swarmer. Only the first one counts for the room's kill count.")
    key("PackHorde", 4, "Same for pack boys: this many at once. 1 = a single pack boy.",
        "Hordes are off while MoreEnemies=1.")
    out.append("")
    note("--- Advanced: leave as is ----------------------------------------")
    out.append("")
    for name, value, text in [
        ("VentVariants", 1, "Vent spawns may become other looks of the same kind."),
        ("CrossFamilyVents", 1, "Vent spawns may become other kinds too (needs CrossFamily)."),
        ("VentRebinding", 1, "Rebinds vent spawns to the new enemy's climb-out."),
        ("VentMessageContract", 0, "Stricter vent matching; off, it hid replacements."),
        ("PoolActorsFollowLevel", 1, "Replaced enemies belong to the level (fixes checkpoint restarts)."),
        ("ReleasePoolAtLevelEnd", 1, "Frees the enemy pool when a level ends."),
        ("PoolHoldForever", 0, "Debug only. Leave 0."),
        ("GuardServiceRelease", 1, "Safety guard for level teardown."),
        ("ScriptedArrivals", 1, "Scripted spawns (doors, ambushes) may be replaced too."),
        ("InfectedArrivals", 1, "Corpses raised by an infector may be replaced too."),
        ("MaxDonorLookups", 8, "How many candidates are tried per spawn."),
        ("GlobalTemplates", 1, "Enemies are built from the shared pool templates."),
        ("PinPoolPrerequisites", 0, "Debug only. Leave 0."),
        ("GlobalStreamProbe", 1, "Loads the enemy pool at the start of every level."),
        ("DirectDonors", 1, "Enemies placed directly in rooms may be replaced too."),
        ("Variants", 0, "Stat variants (faster, tougher, recoloured). Not in this build."),
        ("VariantRuntime", 0, "Speed and size of variants. Not in this build."),
        ("VariantBias", "", "Test aid. Leave empty."),
        ("PlanDump", "", "Test aid. Leave empty."),
        ("MinimumLoadingSeconds", 4, "The loading screen stays at least this long."),
        ("LoadingWaitsForPool", 1, "The loading screen waits for the enemy pool."),
        ("MaximumLoadingSeconds", 30, "Upper bound for that wait."),
        ("MoreEnemiesVents", 0, "MoreEnemies also for vent spawns."),
        ("MoreEnemiesVentDelayMs", 2000, "Gap between extra vent spawns."),
        ("MoreEnemiesHideCompletion", 1, "Doors wait for the extra enemies too."),
        ("MoreEnemiesMaxPerTrigger", 3, "Cap on extra enemies per spawn point."),
        ("MoreEnemiesPerSecond", 12, "Rate limit for extra spawns."),
        ("Watchdog", 1, "Repairs enemies stuck in vents or walls."),
        ("WatchdogRepairTables", 1, "Lets the watchdog fix internal tables."),
        ("WatchdogKill", 0, "Watchdog may remove stuck enemies. Off."),
        ("WatchdogSeconds", 180, "Seconds before an enemy counts as stuck."),
        ("WatchdogVentSeconds", 0, "Same for vents (0 = default)."),
        ("WatchdogStateProbeSeconds", 6, "How often the watchdog looks."),
        ("WatchdogVentStuckSeconds", 8, "Seconds in a vent before it helps."),
        ("ObserveSpawns", 1, "Writes every spawn to the log. Needed for bug reports."),
        ("DetailedSpawnLimit", 2048, "How many spawns the log keeps in detail."),
        ("StreamRegistryDiagnostics", 1, "More log lines for bug reports."),
        ("StreamWatchDiagnostics", 1, "More log lines for bug reports."),
        ("MessageTrace", 0, "Very noisy log. Only when asked for."),
        ("TextureProbe", 0, "Debug only. Leave 0."),
        ("SpawnerTools", 1, "Reads plugins\\DS2SpawnerRules.tsv (per-spawner rules)."),
    ]:
        key(name, value, text)

    title("Which enemy kinds take part")
    note("Sources: spawn points of this kind may be replaced.",
         "Targets: this kind may appear as a replacement.",
         "Weights: how often a kind is picked as a replacement (100 = normal, 0 = never).",
         "Lurker stays 0 as a target: its files are not loaded outside its own rooms.")
    out.append("")
    out.append("[Weights]")
    for f in COMMON:
        flat(f, 100)
    out.append("")
    out.append("[Sources]")
    for f in FAMILIES:
        flat(f, 1)
    out.append("")
    out.append("[Targets]")
    for f in FAMILIES:
        flat(f, 0 if f == "Lurker" else 1)
    out.append("")
    out.append("[ExperimentalClasses]")
    note("Kinds that only take part when you say so. Infector and Swarm work;",
         "Stalker, the Divider and the Hunter are experiments. A Divider or the",
         "Hunter never replaces an enemy whose death a door is waiting for.")
    for f, v in [("Infector", 1), ("Stalker", 0), ("SwarmEnemy", 1), ("DividerCore", 0), ("DividerPart", 0), ("Hunter", 0)]:
        flat(f, v)
    out.append("")

    title("How many of each kind (only with MoreEnemies=1)")
    note("The number is the total per spawn point: 2 = two where the level placed one.",
         "Default applies to every kind without its own line. Bosses are always one.")
    out.append("")
    out.append("[MoreEnemies]")
    flat("Default", 1)
    for f in ["Zombie", "PackEnemy", "TubeBeetle", "Leaper", "Lurker", "Puker", "Exploder", "Pregnant", "Infector", "Stalker", "SwarmEnemy", "Brute"]:
        flat(f, 2)
    out.append("")

    title("Watchdog (leave as is)")
    note("Which kinds the watchdog may repair, and per-kind timers (0 = default).")
    out.append("")
    out.append("[WatchdogFamilies]")
    flat("Default", 0)
    for f in ["Zombie", "Exploder", "Leaper", "Lurker", "PackEnemy", "Pregnant", "Puker", "TubeBeetle", "Brute", "Infector", "Stalker", "SwarmEnemy"]:
        flat(f, 1)
    out.append("")
    out.append("[WatchdogTimers]")
    for f in ["Zombie", "Exploder", "Leaper", "Lurker", "PackEnemy", "Pregnant", "Puker", "TubeBeetle", "Brute", "Infector", "Stalker", "SwarmEnemy"]:
        flat(f, 45 if f == "Lurker" else 0)
    out.append("")

    title("Loot amounts")
    out.append("[Loot]")
    key("AmmoPercent", 100, "Size of every ammo pickup, in percent. 100 = as the game made it,",
        "150 = half again as much. Drop chances do not change.")
    key("CreditsPercent", 100, "Same for credit pickups. Shop prices stay as they are.")
    title("Schematics and workbench")
    out.append("[SchematicRandomizer]")
    key("Enabled", 1, "Schematics you find unlock a different item in the store.")
    key("FirstIsWeapon", 1, "The first schematic of the story is always a weapon.")
    key("SecondIsSuit", 1, "The second is always the Security or the Steampunk Suit.")
    out.append("[BenchRandomizer]")
    key("StatShuffle", 1, "Bench upgrade nodes trade stats. A weapon keeps the same stats",
        "and the same number of each, only where they sit on the grid moves.")
    key("NodeShuffle", 1, "Stats may also sit on cells that used to be plain connectors.")
    key("ProtectStartPoints", 1, "A tree always opens on a real upgrade.")
    key("Seed", 50, "Bench seed. Keep it equal to the seed above. Never change it",
        "mid-playthrough: nodes you already bought would give something else.")

    out.append("")
    return "\r\n".join(out) + "\r\n"


def main():
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "dist/public/plugins/DS2Randomizer.ini"
    target.parent.mkdir(parents=True, exist_ok=True)
    text = build()
    target.write_text(text, encoding="ascii", newline="")
    keys = sum(1 for line in text.splitlines() if "=" in line and not line.startswith(";"))
    print("wrote %s (%d keys)" % (target, keys))


if __name__ == "__main__":
    main()
