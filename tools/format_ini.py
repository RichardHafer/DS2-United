"""Rewrite DS2Randomizer.ini in a readable layout without changing any value.

Reads the current values, emits the documented layout below, then verifies that
every section, key and value survived. It refuses to write if anything moved.
"""
import argparse
import re
from collections import OrderedDict
from pathlib import Path

RULE = ";" + "-" * 74


def parse(text):
    sections = OrderedDict()
    current = None
    for raw in text.split("\n"):
        line = raw.strip().lstrip("﻿")
        found = re.match(r"^\[([^\]]+)\]$", line)
        if found:
            current = found.group(1)
            sections.setdefault(current, OrderedDict())
            continue
        if line.startswith(";"):
            # A commented-out setting, not prose. Prose such as
            # "; Requires CrossFamily=1." must not be read as a key, so the
            # whole line has to be exactly one bare assignment.
            found = re.fullmatch(r";\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(\S*)\s*", line)
            if found and current is not None:
                sections[current][found.group(1)] = (found.group(2), True)
            continue
        found = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$", line)
        if found and current is not None:
            sections[current][found.group(1)] = (found.group(2).strip(), False)
    return sections


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ini", type=Path)
    args = parser.parse_args()

    original = args.ini.read_text(encoding="utf-8-sig")
    values = parse(original)

    # Keys a build added after the INI was written are emitted commented out
    # with their plugin default, so an old INI formats without a KeyError and
    # the owner sees the new switch.
    NEW_KEY_DEFAULTS = {("Loot", "AmmoPercent"): "100", ("Loot", "CreditsPercent"): "100",
                        ("DS2Randomizer", "SwarmHorde"): "6", ("DS2Randomizer", "PackHorde"): "4", ("DS2Randomizer", "FpsCap"): "60", ("DS2Randomizer", "PlacementHotkey"): "121", ("DS2Randomizer", "ExtraSpawns"): "1",
                        ("DS2Randomizer", "WatchdogReleaseDeathMessage"): "1"}
    def get(section, key):
        entry = values.get(section, {}).get(key)
        if entry is None:
            return NEW_KEY_DEFAULTS.get((section, key), ""), True
        return entry[0], entry[1]

    out = []
    def title(text):
        out.extend([RULE, "; " + text, RULE])
    def blank():
        if out and out[-1] != "":
            out.append("")
    def note(*lines):
        out.extend(("; " + line) if line else ";" for line in lines)
    def setting(section, key, *comment):
        value, commented = get(section, key)
        if comment:
            note(*comment)
        out.append(("; " if commented else "") + f"{key}={value}")
        blank()
    def block(section, keys, *comment):
        if comment:
            note(*comment)
        for key in keys:
            value, commented = get(section, key)
            out.append(("; " if commented else "") + f"{key}={value}")
        blank()

    title("Dead Space 2 Randomizer")
    note("Experimental. Back up your saves before changing anything here.",
         "Restart the game after editing this file; nothing is re-read at runtime.",
         "Every switch is 0 for off and 1 for on unless a range is given.")
    blank()

    out.append("[DS2Randomizer]")
    blank()
    note("--- Main switch -------------------------------------------------------")
    blank()
    setting("DS2Randomizer", "Enabled",
            "Enemy replacement. With this off the plugin only observes.")
    setting("DS2Randomizer", "Seed",
            "Any number. The same seed always produces the same run.")
    setting("DS2Randomizer", "Chance",
            "Percent chance that an eligible spawn is replaced. Range: 0 to 100.")

    note("--- What may replace what --------------------------------------------")
    blank()
    setting("DS2Randomizer", "CrossFamily",
            "Allow a different enemy species, not just another variant of the same",
            "one. Needs more in-game testing than same-family replacement.")
    setting("DS2Randomizer", "VentVariants",
            "Same-family variants that share the exact authored vent and spawn",
            "settings.")
    setting("DS2Randomizer", "CrossFamilyVents",
            "Authored same-vent pairs across families. Requires CrossFamily=1.")
    setting("DS2Randomizer", "VentRebinding",
            "Point a donor at the vent the level authored, instead of requiring it",
            "to be authored on that same vent. Only donors that are themselves vent",
            "arrivals qualify, because the climb-out belongs to the species, and",
            "the rest of the arrival contract (mode and activation flags) still",
            "has to match. Raises the vent spawns that have a donor in the same",
            "room from 196 to 282 of 392. Requires VentVariants=1. Cross-family",
            "pairs also require both cross-family switches above.",
            "",
            "With this off, vent spawns are still replaced, but only through the",
            "exact authored contract, so nothing is written into the spawning",
            "component. Floor and ceiling vents then lose most of their donors.")
    setting("DS2Randomizer", "VentMessageContract",
            "Let two vent spawns match when only their spawn-done messages differ.",
            "",
            "That message is what a level script waits for: a door, the next wave,",
            "the end of a quarantine. So it was part of what makes two spawns",
            "interchangeable. It is also the only thing keeping 26 of the 35 vent",
            "spawns no donor can reach: three leapers stand together in the med",
            "storage and two of them are unreachable for this one field.",
            "",
            "Safe only because the rebind hands the replacement the level's own",
            "spawning component, so the message comes with it rather than being",
            "written afterwards. Requires VentRebinding and is ignored without it.",
            "Watch `relaxed=` next to `missed=` in the vent_binding line.")
    setting("DS2Randomizer", "PoolActorsFollowLevel",
            "A replaced enemy keeps the original spawn's stream handle instead of",
            "the donor's, so it belongs to the level and is torn down together with",
            "the state tables it indexes. The same goes for anything a replaced",
            "enemy spawns by itself, such as the swarm a Pregnant releases: those",
            "packets get the handle of the last spawner lookup, so they die with",
            "the level too instead of outliving a checkpoint restart on the pool's",
            "handle. See analysis/STATE_TABLE_GENERATIONS.md.")
    setting("DS2Randomizer", "ReleasePoolAtLevelEnd",
            "Release our resident pool root already at level release. Off: the root",
            "is held until the stream manager is destroyed, because with",
            "PoolActorsFollowLevel the level teardown runs after that point and would",
            "otherwise pull the resources out from under enemies that have not been",
            "destroyed yet.")
    setting("DS2Randomizer", "PoolHoldForever",
            "Never release the pool. Every release point tried crashed inside the",
            "teardown of our own stream, and a resident pool does not need one: the",
            "root keeps its lock for the life of the process. See",
            "analysis/POOL_IDENTITY_COLLISION.md.")
    setting("DS2Randomizer", "GuardServiceRelease",
            "Skip handing a resource handle back to the streaming service while",
            "that service is null. It is only null between its destruction and the",
            "end of the level teardown, and the cleanup that runs in between used to",
            "fault on it. See analysis/CRASH_39616_MENU_HAVOK.md.")
    setting("DS2Randomizer", "ScriptedArrivals",
            "Spawns that arrive through an authored animation instead of a vent,",
            "plus the play-dead lifecycle. The donor must be authored somewhere with",
            "an identical spawning component: mode, both animation names, animation",
            "options, activation flags and every activation and completion message.",
            "Same family only. See analysis/SCRIPTED_ARRIVALS.md.",
            "Experimental, not tested in game.")
    setting("DS2Randomizer", "InfectedArrivals",
            "Corpses that arrive through the infection lifecycle. Same contract as",
            "ScriptedArrivals, kept separate because these also depend on infector",
            "target selection and corpse resources.")
    setting("DS2Randomizer", "MaxDonorLookups",
            "How many template lookups one replacement attempt may make.",
            "Range: 1 to 16.")

    note("--- Global template pool ----------------------------------------------")
    blank()
    setting("DS2Randomizer", "GlobalTemplates",
            "Load private enemy templates across levels, so a donor can be used in a",
            "chapter that does not ship it. Experimental. Requires the matching STR",
            "and TOC next to the executable.")
    setting("DS2Randomizer", "PinPoolPrerequisites",
            "Hold a reference on the shared character streams the pool needs, so a",
            "room change cannot free the state tables of enemies built from it.",
            "This is the experiment for the crash in",
            "analysis/STATE_TABLE_LIFETIME_20260909.md. It changes engine",
            "reference counts, which is the riskiest thing this plugin does.")
    setting("DS2Randomizer", "GlobalStreamProbe",
            "Loading probe for that pool. It does not enable replacement on its",
            "own, and GlobalTemplates=1 turns it on anyway, so this switch only",
            "matters when the pool is off.")
    setting("DS2Randomizer", "DirectDonors",
            "The pack boy stand-in. It is the only donor that was added by hand",
            "and the only one that replaces a pool root. Two of them were on screen",
            "when a game crashed, and for both the component walk reported two",
            "components of every class on one actor. Off until that is understood.")
    setting("DS2Randomizer", "Variants",
            "Stat variants: copies of pool enemies with more health, more damage",
            "or other authored numbers changed, built by tools/build_variants.py",
            "from variants/ds2_variants.json. They look like the base enemy; only",
            "the fight tells. Each one competes only in the share of spawns its",
            "chance in [Variants] below allows.")
    setting("DS2Randomizer", "VariantRuntime",
            "Let variants also be faster or bigger. The playback factor from the",
            "spec is written into the enemy's animation view when it is created",
            "(movement follows the animation, so it walks and attacks faster) and",
            "folded into every later change the game makes, stasis included. The",
            "size is the same visual only scale DS1 had: the collision capsule does",
            "not grow, so keep it modest. Requires Variants=1.")

    setting("DS2Randomizer", "VariantBias",
            "Test aid: the name of one variant (as in [Variants]) that wins every",
            "draw it is a candidate in, so you meet it right away instead of one",
            "time in twenty. Empty for the normal seeded draw. Not for real runs.")
    setting("DS2Randomizer", "PlanDump",
            "Diagnostic: the GUID of one source record; its replacement plan (the",
            "donor order) is written to the log at start. Empty for none.")

    note("--- Loading screen ----------------------------------------------------")
    blank()
    setting("DS2Randomizer", "MinimumLoadingSeconds",
            "How long every loading screen stays up at least, in seconds (0-60).",
            "The randomizer requests its pool while a level loads; MarkerPatch's",
            "SkipArtificialLoadingDelay ends the screen as soon as the level's own",
            "streams are in, before the pool can be. This minimum replaces the",
            "game's authored one, with or without MarkerPatch. 0 = no minimum.")
    setting("DS2Randomizer", "LoadingWaitsForPool",
            "Keep the loading screen up while the pool (and a skin's model",
            "stream) has been requested but is not ready yet, so the first spawns",
            "of a level can already be replaced. Capped by MaximumLoadingSeconds.")
    setting("DS2Randomizer", "MaximumLoadingSeconds",
            "Upper bound for the pool wait above, in seconds (0-120). A pool that",
            "never becomes ready does not hold the loading screen longer than this.")

    note("--- More enemies ------------------------------------------------------")
    blank()
    setting("DS2Randomizer", "MoreEnemies",
            "Spawn multiplier. It repeats an authored arrival that already",
            "succeeded, using the same message the engine uses for its own queued",
            "retry. It never picks a donor and never touches the global template",
            "pool, so it is independent of the state-table problem described in",
            "analysis/STATE_TABLE_LIFETIME_20260909.md.",
            "Experimental, not tested in game. See analysis/MORE_ENEMIES_20260909.md.")
    setting("DS2Randomizer", "FpsCap",
            "Frame cap the plugin enforces itself, in frames per second. Above",
            "60 the game's scripts misbehave (elevators, doors). 0 = off. 30 to 240.")
    setting("DS2Randomizer", "ExtraSpawns",
            "Reads plugins\DS2ExtraSpawns.tsv: enemies a spawner brings along, placed",
            "where you stood when you pressed the placement key. A row whose",
            "source_guid is near:20 needs no spawner: it fires once when you come",
            "within 20 m of its x y z (for rooms where nothing spawns at the door).",
            "1 = on.")
    setting("DS2Randomizer", "PlacementHotkey",
            "Key that prints your position, facing and the nearest spawners to the",
            "log, for extra-spawn rules. Virtual-key code, 121 = F10, 0 = off.")
    setting("DS2Randomizer", "MoreEnemiesVents",
            "Repeat vent arrivals as well. A repeat starts a second climb-out",
            "animation in the same vent while the first one is still playing.")
    setting("DS2Randomizer", "MoreEnemiesVentDelayMs",
            "Spacing between repeated vent spawns, in milliseconds. A vent carries",
            "one enemy at a time, so two in the same frame block each other. 2000 is",
            "what the game uses for its own retries. 0 restores the old behaviour",
            "and repeats them at once.")
    setting("DS2Randomizer", "MoreEnemiesHideCompletion",
            "Hide the duplicated per-spawn completion message from level logic.",
            "With 0 a level that counts spawn completions also counts the extra",
            "copies. When the message cannot be hidden the multiplier does nothing.")
    setting("DS2Randomizer", "MoreEnemiesMaxPerTrigger",
            "Hard ceiling on the total per trigger, whatever [MoreEnemies] asks",
            "for. Range: 1 to 8.")
    setting("DS2Randomizer", "MoreEnemiesCrossFamily",
            "When the randomizer replaced the enemy with another family, the",
            "multiplier uses the SMALLER of the two families' totals (1), or does",
            "not multiply such a replacement at all (0). A swarmer slot is never",
            "multiplied at all, whatever stands there: a room authored for a",
            "dozen swarmers, each replaced by a stalker and repeated four times,",
            "is how a session ends. 1 = on.")
    setting("DS2Randomizer", "SwarmHorde",
            "How many swarmers appear when a swarmer replaces another kind of",
            "enemy. One swarmer is no threat, so the spawn is repeated until this",
            "many stand there. Only the first of them carries the room's kill",
            "count. A swarmer replacing a swarmer stays single (the room has its",
            "dozen already), and MoreEnemies never stacks on a horde.",
            "Range: 1 to 12, 1 = single.")
    setting("DS2Randomizer", "PackHorde",
            "Same for pack boys: how many appear when a pack boy replaces another",
            "kind of enemy. Range: 1 to 12, 1 = single.")
    setting("DS2Randomizer", "MoreEnemiesPerSecond",
            "How many extra spawns a window of SIXTEEN arrivals may hold, all",
            "families together. Range: 0 to 112, 0 stops them. Rule of thumb:",
            "16 x (your largest [MoreEnemies] total - 1), so 48 for totals of 4;",
            "12 lets only four of sixteen arrivals be multiplied. (Arrivals, not",
            "seconds: a clock made runs depend on frame rate and load times.)")

    note("--- Engine limits ------------------------------------------------------")
    blank()
    setting("DS2Randomizer", "NavPowerPoolKiB",
            "The pathfinding library (NavPower) gets ONE fixed memory pool from",
            "the game: 576 KiB in retail, room for about 160 navigating enemies.",
            "One more and the game stops itself (\"System ran out of available",
            "heap memory\"). This hands it a larger pool. Range: 0 to 65536 KiB,",
            "0 = retail. 4608 = eight times retail.")
    blank()
    note("--- Runaway watchdog ---------------------------------------------------")
    blank()
    setting("DS2Randomizer", "Watchdog",
            "Watch the enemies the randomizer created. One that outlives the",
            "timeout below is reported to the log, so a replacement that ended up",
            "somewhere it cannot be reached can be found instead of quietly holding",
            "a room closed. Reporting only, unless WatchdogKill below is on.")
    setting("DS2Randomizer", "WatchdogRepairTables",
            "The real fix for the state-table crash. The game rebuilds its state",
            "tables into a fresh block and frees the old one; enemies the",
            "randomizer created survive that boundary and keep pointing into the",
            "freed block, which is what crashes. This points them back at the table",
            "their own class is using now. See analysis/STATE_TABLE_GENERATIONS.md.")
    setting("DS2Randomizer", "WatchdogKill",
            "Actually kill an enemy the watchdog reported, through the engine's own",
            "zero-health path so the death events, the drop and any door counter",
            "fire normally. Only families enabled in [WatchdogFamilies] below are",
            "ever eligible, and bosses, anchored actors and owned parts are refused",
            "in code whatever that section says.")
    setting("DS2Randomizer", "WatchdogDeathProxy",
            "When a replacement could not be given the original enemy's death",
            "message (the log's death_message refused= count), the watchdog says",
            "that message the moment the replacement dies, so a room that counts",
            "deaths still completes. 1 = on.")
    setting("DS2Randomizer", "WatchdogReleaseDeathMessage",
            "A scripted room counts death messages, not corpses, so a replacement",
            "that never came alive holds the door shut forever. Measured in the",
            "hospital lobby: the level adds one to a variable per dead slasher and",
            "opens up at five, four had died, and the fifth was a donor that built",
            "no state machines at all. With this on, the watchdog says that one",
            "message on the actor's behalf when it gives up on it. Only for an",
            "actor with no state machines after the full timeout, or one still",
            "stuck in its vent - never for an enemy the player merely has not",
            "walked to yet.")
    setting("DS2Randomizer", "WatchdogSeconds",
            "How long a replaced enemy may live before it is reported.",
            "Range: 10 to 3600. An enemy the player has simply not",
            "reached yet is perfectly normal, so do not read a report as a fault.")

    note("--- Diagnostics -------------------------------------------------------")
    blank()
    setting("DS2Randomizer", "WatchdogVentSeconds",
            "A shorter leash for enemies that arrive through a rebound vent. Those",
            "are the ones that get stuck: in one player's session 10 of 13 stranded",
            "enemies had come out of such a vent, and a quarantine waits for every",
            "one of them. 0 uses the normal leash.")
    setting("DS2Randomizer", "WatchdogStateProbeSeconds",
            "After how many seconds a replaced enemy is asked once what it is",
            "doing. The leash alone only says it is still alive. The state name",
            "says whether it is still stuck in its arrival. When",
            "WatchdogVentStuckSeconds is later than this, the probe waits for",
            "that instead, so the vent check gets its turn. 0 turns the probe off.")
    setting("DS2Randomizer", "WatchdogVentStuckSeconds",
            "After how many seconds a replaced enemy is suspected of being stuck",
            "in its vent. The test is not the time alone but whether one of the",
            "vent state machines is still running. At this age that is only",
            "reported. The verdict comes at the leash: if the same vent machine is",
            "still running after the full timeout, the donor is written to",
            "DS2VentDonorBlocklist.tsv and the next start no longer offers it for",
            "vents. It stays available for ordinary walk-in spawns, so that file",
            "cannot change what two machines on the same seed see away from",
            "vents. The enemy itself is killed only if WatchdogKill is on.",
            "0 turns the suspicion off.")
    setting("DS2Randomizer", "ObserveSpawns",
            "Write a line per spawn to the log.")
    setting("DS2Randomizer", "DetailedSpawnLimit",
            "How many spawn lines can queue up between two passes of the log",
            "thread. It is a ring: once it is full the oldest unwritten line is",
            "replaced, and the log says so. Range: 0 to 4096, 0 turns spawn",
            "lines off.")
    setting("DS2Randomizer", "StreamRegistryDiagnostics",
            "Read-only registry snapshots every five seconds, up to 60 reports.",
            "Requires Enabled=1 or ObserveSpawns=1.")
    setting("DS2Randomizer", "StreamWatchDiagnostics",
            "Continuous read-only stream changes and pending-state samples.")
    setting("DS2Randomizer", "MessageTrace",
            "One log line per script message the engine sends: hash, name where",
            "the plugin knows it, sender, whether the name is registered and how",
            "many listeners it has. Repeats inside a quarter second fold into one",
            "line, and more than 1500 a second are dropped and counted. This is",
            "the tool for a door or elevator that never comes: the last messages",
            "before the wait are in the log. Off unless something hangs; a busy",
            "room writes a lot.")
    setting("DS2Randomizer", "TextureProbe",
            "Skin diagnostics: one line per texture the loader resolves and per",
            "figure the game assembles. Test builds only; leave it 0.")
    setting("DS2Randomizer", "SpawnerTools",
            "Per-spawner rules and a rolling recent-spawn list for the companion",
            "editor. The rules live in DS2SpawnerRules.tsv next to the log. One",
            "spawner may carry several rows, each for its own set of",
            "difficulties (zealot;hardcore) plus one general row; the game uses",
            "the row for the difficulty being played, else the general one. The",
            "difficulty is read from the game's own settings file whenever that",
            "file changes.")

    title("Which families take part")
    note("Zombie means Slasher, TubeBeetle means Crawler, Hunter means Ubermorph.")
    blank()

    out.append("[Weights]")
    blank()
    block("Weights", list(values["Weights"]),
          "Relative weights, not percentages. 0 removes a family from selection.",
          "Only supported classes and arrival contexts can be picked at all.",
          "The species is drawn first by these weights, then a model of it, so a",
          "family at 3 next to ten families at 100 shows up about once in three",
          "hundred replacements. That is where the Ubermorph belongs: a mini boss",
          "that cannot die, once or twice in a whole run.")

    out.append("[Sources]")
    blank()
    block("Sources", list(values["Sources"]),
          "1 allows this family to be replaced. It does not bypass missing",
          "resources or an unsupported arrival.")

    out.append("[Targets]")
    blank()
    block("Targets", list(values["Targets"]),
          "1 allows this family to appear as a replacement.")

    out.append("[VentFamilies]")
    blank()
    note("1 allows this family to arrive through a vent as a replacement. 0 keeps",
         "it to ordinary walk-in spawns only.",
         "",
         "This is the blunt version of DS2VentDonorBlocklist.tsv. That file learns",
         "one record at a time, and every lesson costs a playthrough: a donor that",
         "never becomes an NPC holds its spawn point, and a room counting deaths",
         "then waits for one that can never come. Lurker is off because exactly",
         "that happened in the hospital lobby on 2026-09-16, and thirteen more",
         "vent-capable Lurker records sit behind the one that failed.",
         "",
         "A family switched off here loses nothing but vents.")
    for key in values["Targets"]:
        entry = values.get("VentFamilies", {}).get(key)
        value, commented = entry if entry else (("0" if key == "Lurker" else "1"), False)
        out.append(("; " if commented else "") + f"{key}={value}")
    blank()

    out.append("[ExperimentalClasses]")
    blank()
    block("ExperimentalClasses", list(values["ExperimentalClasses"]),
          "Extra candidates that need conditions the game does not guarantee.",
          "Infector needs infection targets. Stalker needs suitable navigation and",
          "cover. SwarmEnemy is the Swarmer. Vent and scripted arrivals stay",
          "excluded for these.",
          "DividerCore is the whole Divider; when it dies it releases its five",
          "parts from the pool's own copies. DividerPart is one crawling head,",
          "hand or foot. Hunter is the Ubermorph, which cannot be killed: it is",
          "only ever put where the source has no death message, so no door can",
          "wait for it, and it is off unless you want that kind of run.")

    out.append("[Variants]")
    blank()
    block("Variants", list(values.get("Variants", {})),
          "Per variant: its share of its family's spawns, 0 takes it out. When",
          "a spawn of that family is replaced and the variant is available, 25",
          "makes it the pick one time in four and 100 every time, decided by the",
          "seed. Requires Variants=1.")

    title("How many of each enemy")
    note("The number is the total, not a surcharge: 3 means three where the level",
         "placed one, and 1 leaves that kind exactly as it shipped. Requires",
         "MoreEnemies=1 above, and MoreEnemiesMaxPerTrigger caps every value here.",
         "",
         "Hunter, DividerCore, DividerPart, Nest, NestProjectile, Guardian,",
         "GuardianPod, GuardianTentacle, Landmine, Tripod, WindowTripod,",
         "BossTripod, TripodProjectile, Kong, NicoleBoss, Polyp and the tentacles",
         "are held at one in code. A value here does not lift them.")
    blank()

    out.append("[MoreEnemies]")
    blank()
    block("MoreEnemies", list(values["MoreEnemies"]),
          "Default applies to any family without a line of its own.")

    title("Which enemies the watchdog may kill")
    note("An enemy the player has simply not reached yet is perfectly normal, so",
         "keep this to the ordinary roaming kinds. Hunter, DividerCore, the",
         "Guardian family, Nest, Landmine, the Tripods, Kong, NicoleBoss, Polyp",
         "and every tentacle are refused in code and cannot be enabled here.")
    blank()

    out.append("[WatchdogFamilies]")
    blank()
    block("WatchdogFamilies", list(values["WatchdogFamilies"]),
          "Default applies to any family without a line of its own.")

    title("Per-enemy watchdog timers")
    note("Seconds before the watchdog acts on that kind. 0 uses the global",
         "WatchdogSeconds above. Only families enabled in [WatchdogFamilies] are",
         "affected at all; a value below 10 is raised to 10.",
         "",
         "The Lurker starts at 45 because it clings to walls and ceilings and gets",
         "stuck on geometry more readily than the enemies that walk.")
    blank()

    out.append("[WatchdogTimers]")
    blank()
    block("WatchdogTimers", list(values["WatchdogTimers"]))

    title("Schematic randomizer")
    note("Shuffles which upgrade schematic each of the 28 campaign pickups gives,",
         "including the Force Gun schematic that comes out of a container.",
         "Completion-gated elite suits and Hacker content stay out of the pool.",
         "Experimental, not tested in game.")
    blank()

    title("Loot amounts")
    out.append("[Loot]")
    setting("Loot", "AmmoPercent",
            "Size of every ammo pickup in percent of what the game authored.",
            "100 = as is, 150 = half again as much. Range 25 to 500. The drop",
            "chances stay as they are, only the quantity per pickup changes.")
    setting("Loot", "CreditsPercent",
            "Same for credit pickups. Shop prices and sales are not touched.")
    blank()
    out.append("[SchematicRandomizer]")
    blank()
    block("SchematicRandomizer", list(values["SchematicRandomizer"]))

    title("Bench upgrade randomizer")
    note("The game works out what an upgrade node awards from the node's name,",
         "every time it needs it. Only that answer is redirected, so the grid, the",
         "authored connections, the navigation and your save stay untouched.",
         "",
         "Do not change the seed part-way through a playthrough: nodes you already",
         "paid for would start awarding something else.")
    blank()

    out.append("[BenchRandomizer]")
    blank()
    setting("BenchRandomizer", "StatShuffle",
            "Trade stats between upgrade nodes. A trade is only kept when every",
            "tree it touches still offers exactly the stats it offered before, each",
            "as often as before: a fully upgraded weapon is exactly as strong as",
            "retail, Width stays on the Line Gun, and stasis never offers it.",
            "A cell always awards the amount the game authored for the stat it now",
            "carries, so no node shows +0 and no reload node takes a damage amount.")
    setting("BenchRandomizer", "NodeShuffle",
            "Move stats to different cells within one tree, filler nodes included,",
            "so an upgrade can sit where there used to be a plain connector.",
            "Only affects node names that belong to exactly one tree, because the",
            "game's name-to-stat mapping is global.")
    setting("BenchRandomizer", "ProtectStartPoints",
            "Keep a real stat on the cells a tree opens with, so it never starts",
            "on an empty node.")
    setting("BenchRandomizer", "Seed",
            "Defaults to the [DS2Randomizer] seed above.")

    while out and out[-1] == "":
        out.pop()
    text = "\r\n".join(out) + "\r\n"

    rewritten = parse(text.replace("\r\n", "\n"))
    # Order carries no meaning in an INI, so only membership and values matter.
    if set(rewritten) != set(values):
        raise SystemExit(f"section set changed: {sorted(values)} -> {sorted(rewritten)}")
    for section, keys in values.items():
        if set(rewritten[section]) != set(keys):
            missing = set(keys) - set(rewritten[section])
            added = set(rewritten[section]) - set(keys)
            raise SystemExit(f"[{section}] keys changed; missing={sorted(missing)} added={sorted(added)}")
        for key, entry in keys.items():
            if rewritten[section][key] != entry:
                raise SystemExit(f"[{section}] {key} changed: {entry} -> {rewritten[section][key]}")

    args.ini.write_bytes(text.encode("utf-8"))
    print(f"{args.ini.name}: {sum(len(v) for v in values.values())} keys in "
          f"{len(values)} sections, all values unchanged")


if __name__ == "__main__":
    main()
