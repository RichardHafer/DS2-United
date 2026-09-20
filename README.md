<p align="center">
  <img src="assets/DS2Randomizer_Logo.png" alt="Dead Space 2 Randomizer" width="70%">
</p>

# Dead Space 2 Randomizer

A randomizer for the PC port of Dead Space 2. Every enemy spawn in the
campaign can become a different enemy, built by the game from its own data, in
the spot and through the vent the level authored, with the room's scripting
still working. On top of that: more enemies per spawn, enemies placed by hand,
sixteen new enemy variants, shuffled loot, schematics and workbench trees.

Nothing in the game's files is changed. The plugin runs beside the
[Marker Patch](https://github.com/Wemino/MarkerPatch), whose ASI loader it
uses, and everything happens in memory while you play.

## What it does

**On by default**

| | |
|---|---|
| Enemy replacement | Slashers, Leapers, Lurkers, Pukers, Exploders, Crawlers, Pregnants, Pack boys, Swarmers, Infectors, Stalkers, Cysts and the Brute take part. Seeded: the same `Seed` gives the same run. |
| Cross-family swaps | A spawn may become a different species, not just another look of the same one. Vent spawns keep their vent, climb-out and arrival flags. |
| Per-spawner rules | Blacklist a spawn, force a family on it, or keep it as authored, per difficulty if you like. The included Spawner Editor writes the file. |
| Extra spawns | A Guardian on that wall, a Tripod in that corridor. Stand there, press the readout key, add the row. Works tied to a spawner or on walking into the area. |
| Scaled loot | Ammo and credit pickups in percent of what the game authored. |
| Schematic shuffle | Which of the 28 schematic pickups gives what. |
| Workbench shuffle | Stats trade places between upgrade nodes; the amounts follow the stat. |
| Hordes | A swarmer or pack boy that replaces another enemy arrives as a horde (`SwarmHorde`, `PackHorde`). Only one of them counts for the room. |
| Runaway watchdog | Replacements that never die or never leave their vent are reported with their position, their death message is said for them, and after a leash they are killed through the engine's own zero-health path (`WatchdogKill`, on in the shipped INI). |

**Off by default, turn on in the INI**

| | |
|---|---|
| `MoreEnemies` | Repeats authored spawns: two, three, four where the level placed one. Per family, with a per-trigger ceiling and a budget. Swarmer slots are never multiplied. |
| `Variants` | Sixteen enemies that do not exist in the original: Veteran and Berserker Slashers, a Juggernaut, a Glass Slasher, a Rabid Leaper, a Kamikaze Exploder, a Brood Pregnant, a Golden Leaper, a Ghost Pack Boy and more, each with its own chance and look. |
| `VariantRuntime` | Lets variants also be faster or bigger than their base enemy. |

Every hook checks a byte signature before it is placed, and the log says which
ones went in. An unsupported build is left alone.

## Install

### Windows

**[Download the latest release](https://github.com/RichardHafer/DeadSpace2Randomizer/releases/latest)**

You need the [Marker Patch](https://github.com/Wemino/MarkerPatch) installed
first; it brings the ASI loader (`dinput8.dll`) that starts this plugin.

Close the game and extract the archive into the Dead Space 2 folder, next to
`deadspace2.exe`. The installed files should look like this:

```text
Dead Space 2/
|-- dinput8.dll                         (from the Marker Patch)
|-- Spawner-Rules.cmd
|-- plugins/
|   |-- DS2RandomizerObserver.asi
|   |-- DS2Randomizer.ini
|   |-- DS2SpawnerRules.tsv
|   |-- DS2ExtraSpawns.tsv
|   `-- DS2VentDonorBlocklist.tsv
|-- randomizer/
|   |-- ds2_randomizer_pool.str
|   |-- ds2_randomizer_pool.toc
|   |-- ds2_randomizer_models.str
|   `-- ds2_randomizer_models.toc
`-- tools/
    |-- spawner_editor.py
    `-- funcspawn_targets.tsv
```

Settings live in `plugins\DS2Randomizer.ini`. Every key is explained in the
file. Set a `Seed`, set `Chance`, pick the families under `[Sources]`,
`[Targets]` and `[Weights]`, save, play.

Loading screens are a little longer than usual. The plugin keeps every enemy
template resident so any enemy can appear anywhere, and it holds the loading
screen until that pool is ready. That is on purpose.

Cap your frame rate at 60. Dead Space 2 above 60 FPS has its own problems,
and with more enemies around they get worse. `FpsCap` in the INI does it if
you have nothing else.

The plugin writes `plugins\DS2Randomizer.log` while the game runs. If it
crashes, or a door does not open, send that file with the seed and where you
were. Every softlock so far had a cause in the log that could be fixed.

The DS2 Debug Menu works alongside it.

---

## How the bigger pieces work

Not required reading. This is here so the next person does not have to redo
the research. The comments in the source carry the evidence: addresses,
dumps, and the runs that proved or disproved an idea.

### Replacing an enemy

Every authored spawn goes through one engine function, `FuncSpawn::HandleEvents`.
It looks the spawn packet up by the target's GUID and hands it to the entity
factory. A hook between those two steps swaps the packet for one from a pool
of every enemy record in the game, so the factory builds the replacement
exactly the way it builds the original: same transform, same group, same
stream ownership.

The pool is a stream of the plugin's own, built offline from the game's
archives with every enemy record, its drop lists, dismemberment props and the
character packages they need. It is loaded once and stays resident, which is
why any enemy can appear in any chapter and why the loading screen waits for
it.

Vent spawns are the hard case. A vent arrival is a contract between the spawn
point and the spawning component of the enemy: which vent, which animation,
which spawn-done message. A replacement gets the source's spawning header
bound to its own component, so a Puker climbs out of the Leaper's ceiling vent
with the Leaper's arrival rules. Pairs that cannot be bound are refused rather
than guessed.

### Keeping the scripting alive

A scripted room does not count corpses. It counts death messages, and every
authored enemy carries the name of the message its room waits for. A
replacement carries its donor's name instead, from another chapter, which the
engine's message table does not even know. So the source's message is written
into the replacement's health component after creation, with the reference the
engine would have taken. When that write is impossible, the watchdog registers
the actor and says the message itself the first time the actor's state machine
shows the dead table.

The watchdog also tracks every replacement from creation. One that outlives
its leash is reported with its position; one that never became an NPC, or is
still running a vent state machine after the whole leash, has its death
message said for it; and with `WatchdogKill` on, the rest are killed through
the engine's own `SetHealth(0)`, on the game thread, with vtable and GUID
re-checked first.

One field is deliberately left alone: `MSG_AWARE`. It is the only one of the
four an actor listens for rather than sends, and rewriting it after creation
left listener nodes registered under the donor's hash with nobody behind them.
That was a dispatch crash six level segments later.

### More enemies

A repeat is produced exactly the way the engine produces its own queued retry:
a message carrying the spawner's trigger id, dispatched through the spawner's
own `HandleEvents`. The spawner's count is parked far below its authored
maximum for the duration and put back afterwards, and the per-spawn completion
message is hidden so level logic does not count the copies.

Vent copies are spaced two seconds apart, because a vent carries one enemy at
a time. That collided with retirement: a spawner whose authored maximum is one
is retired by the arrival's own `SpawnDone` and deleted, and two seconds later
there was nothing left to drive. So a hook just before `SpawnDone` parks the
count first, and the last copy lets the spawner retire the retail way.

A room that counts kills counts one per authored spawn: the first death of a
repeated group passes, the copies' deaths are swallowed at the dispatch.

The hard limit is not the multiplier. NavPower, the pathfinding library, gets
one fixed 576 KiB pool from the game, and past roughly 160 navigating enemies
it stops the process with "System ran out of available heap memory". The
plugin hands it a larger pool at creation (`NavPowerPoolKiB`).

### Extra spawns

A spawner is driven once more with the donor forced in the selection hook, and
the actor is moved to the spot the readout printed, the way `FuncSpawn` itself
moves a spawn onto its location entity. A spawner whose authored target is a
prop works too; the packet is swapped like any other. Area rules (`near:20`)
need no spawner at all: they fire on proximity and borrow whichever live
spawner still resolves its own target, preferring ones that spawn props.

### Workbench shuffle

The game does not store an upgrade node's type on the node. Every consumer
turns the node's name hash into a type through one pure function, and one
inline hook on that function moves what a node awards while the grid, the
connections and the save data stay as authored. The amounts have to follow:
a Capacity cell that now awards Reload would otherwise hand you its 25 rounds
as 25 seconds. The trees are read once at install and every cell is rewritten
with the amount the tree authored for its new stat.

---

## Planned

- A trigger on the room's stream set for area rules, instead of metres
- Damage and death cause in the watchdog report
- Variants for more families

## Building

Visual Studio 2022 with the x86 toolchain and Python 3, then
`build_observer.cmd` for the plugin and `build_tests.cmd` for the tests. `tools\vcvars32.cmd` finds whichever
edition is installed. safetyhook needs `std::expected`, hence `/std:c++latest`.

The `src/generated_*.h` tables are built from the extracted game data under
`analysis/`, which is not part of the repository; the headers are committed,
so the plugin builds without it. The enemy pool and the variants' model
stream are game-derived as well and ship with the release, not with the code.

Addresses are specific to the Steam build. Signatures are matched at runtime,
so an unsupported build fails safely and reports what it could not find.

## Credits

- [safetyhook](https://github.com/cursey/safetyhook) and [Zydis](https://github.com/zyantific/zydis) for hooking
- [Marker Patch](https://github.com/Wemino/MarkerPatch) for the ASI loader and the fixes this runs on top of
- Everyone who played test runs and sent logs

## Licence

MIT, see `LICENSE`. Not affiliated with EA or Visceral Games.
