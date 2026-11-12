# Dead Space 2 United features

Every feature of Dead Space 2 United for Dead Space 2 (by Hafer) in one place: per game mode what is on or off and whether it works, then a catalog by area.

How the systems behind them work (networking, connecting, the survival director and its modes, engine addresses, source map): `docs/DS2COOP_FOR_MARK.md`. Both files are kept current with every change.

**Last updated: 2026-11-12**

**Renamed 08.11.2026:** the mod is Dead Space 2 United now. Before that it was the Co-op Mod (DS2Coop, from 17.10.2026) and HaferSprawl (Hafer's Sprawl). `DS2Coop.asi` is `DS2United.asi`, the merged `DS2Coop.ini` is `DS2United.ini` (the configurator copies it over on first start, the plugin reads the old file until then), `DS2CoopConfig.exe` is `DS2UnitedConfig.exe` and `DS2Coop.log` is `DS2United.log`. With the old plugin still in plugins\ the new one stays idle and says why in DS2United.log.

**Update 12.11.2026, evening (installed):** configurator page "Look & Extras": colours (moved from Co-op), menu colours (`[Menu] RedInGameMenus`, new `[Menu] FrontendColours=red|stock`), game mode row, map pictures, turret and gadgets with their keys, new `[Gadgets] Story` (0 = turret, stasis mine and heal field only in the game modes), voice packs, Dementia (blackouts and flashes), the difficulty question and the debug menu; shop and status line sizes on the Survival page. `randomizer\ds2coop_frontend_stock.str` is the same frontend chain without the recolour steps (build_loadout_frontend --stock-colours, fix_mp20_previews, build_map_pictures, mp20_third_row, build_title_logo). Crash dumps: full memory only into C:\DS2CrashDumps or with `[Coop] CrashDumpFull=1`, otherwise a small dump, and the game folder keeps the last three crashes. Main menu title "/United" (was "/Co-op"), lobby refusal text renamed.

**Update 12.11.2026 (installed):** the plugin's queued game-thread work (More Enemies repeats, area extra spawns, vent queue, placement key, watchdog kills and releases) now runs from the frame tick after the frame's logic instead of from inside an NPC's StateMachine::Update. Run there it could spawn or tear down actors while that NPC's update was half done; the update then read a freed state array (crashes 0x013E2D18 and 0x012DAF9D, survival and co-op). The state machine tick only stands in while the frame tick is missing; log line `game-thread work from frame= from state machine=`.

**Update 11.11.2026 (installed):** co-op heap crash fixed at the root: the skin bone scale pass 0x010741F0 (deferred skin queue 0x010767C0) wrote bone matrices into a ghost's pose buffer after a detach or suit change had freed it, and the next heap consolidation crashed at 0x013FC421/0x013FC442 (nine crashes 19.10. to 05.11.). A hook skips that pass for a ghost skin whose pose pointer no longer names its Player's live buffer; log line `coop_uniform skin scale guard runs= skipped=`. Brutes stay multiplied; the configurator's More Enemies table drops the swarmer line (never multiplied).

**Update 08.11.2026 (built, not installed):** F1 ([Menu] HelpKey) shows the how to play page with every key in menus and levels, bot order keys included; the supply terminal defaults to V; Ellie's two vent slashers in the street scene, the two transport hub slashers that run at Ellie and the crawler of the se_04_09 vision count as story scene actors (no randomizing, no copies; tools/find_scene_actors.py now also takes targets a script kills or animates in sync), and MoreEnemiesSkipScripted defaults to on in the code too; the title screen logo reads UNITED (build/frontend_united/ds2coop_frontend.str, only lgo_ds2_02 changed against the installed 21.10. frontend), the credits block says Dead Space 2 United / Created by Richard Hafer and old INI default texts (NatLine, TitleLine, credits) map to the new ones; the configurator has the game modes first, an About dialog (F1) and the Severed, vent jam guard, quiet vent and bot order settings.

**Update 14.10.2026:** everything marked "built, not installed" up to 14.10. is installed now (plugin `DS2United.asi` CRC 86050441, menu stream CRC D2AEEB3A) and waits for a playtest.

- The mod is one plugin, `plugins\DS2United.asi`: the randomizer half (`src\observer.cpp`) and the co-op half (`src\coop_plugin.cpp`). Settings live in `plugins\DS2United.ini`, written by `DS2UnitedConfig.exe`. It counts as the merged file when it has the `[DS2Randomizer]` section (the co-op half's own file before the merge had the same name); otherwise `HaferSprawl.ini` from before the rename is read, and without both the legacy files: `DS2Randomizer.ini`, `DS2United.ini`, `DS2Survival.ini`, `DS2BotProfiles.ini`. The configurator's migration keeps the co-op half's own file as `DS2Coop.legacy.ini`.
- **Default** is the value in the repo templates, which is also the configurator's recommended value. "code N" is what the plugin uses when the key is missing, given only where it differs.
- **Switch** is `[Section] Key` in DS2United.ini. Dates are 2026, DD.MM.

| Status | Meaning |
|---|---|
| Works (date) | Verified in a playtest on that date |
| Partial (date) | Works with known issues, named in the note |
| Untested | Built, not played yet |
| Broken | Known not to work |
| Off | Experimental, off by default |
| Removed | Taken out of the code |
| Unclear | No test evidence found either way |

"Fix 13.10" in a note means a fix was built on 13.10 and not played yet.

## Installed INI vs template

`D:\SteamLibrary\steamapps\common\Dead Space 2\plugins\DS2United.ini` differs from the templates here:

| Switch | Template | Installed |
|---|---|---|
| [DS2Randomizer] SoundProbe | 0 | 1 |
| [Coop] JoinAddress | empty | 127.0.0.1:27015 |
| [Coop] ProvisionCreate (no template key, code 0) | | 1 |
| [Coop] NecroRecon, LiftReach, SecondIsaac (no template keys) | | 1, 5, 0 |
| [Necro] SixthTypeGuid, LeaperRecordStream, SixthCharStream | Lurker GUID, ds2_leaper | Ubermorph (139C0E59-..., ds2_ubermorph, ubermorph_cct) |
| [Necro] Key, Type, RegisterNet, SpawnSettings (no template keys) | | 122 (F11), 0, 1, 1 |
| [Bot] AltFireTrace | 0 | 1 |
| [Bot] FollowIntoTubes | removed 13.10 | 0 (ignored) |
| [Shop] DuringWaves | 0 | 1 |
| [Menu] NativeModeRow | 1 (since 13.11.) | 1 |
| [NativeLobby] Enabled | 0 | 1 |
| [Loadout] Enabled | 0 | 1 |
| [Respawn] Enabled, DelayMs, WaveClearedDelayMs | 0, 4000, 1000 | 1, 5000, 2000 |
| [Bot1..3] Antics / HarmfulAntics | 0 / 0 | 1, 3, 1 / 1, 1, 0 |
| [Bot1] PreferredDistance, [Bot3] ReactionMinMs..MaxMs | 4.5, 350..800 | 2.5, 400..600 |

- About 150 template keys are missing from the installed file, among them the whole [Turret], [Gadgets], [Marks] and [Cleanup] sections, `[Bot] NecroGrabEscape` and the new Mercenaries/Zone limits. The code defaults then apply; they equal the template for every key checked.

## Modes at a glance

### What each mode needs

| Mode | Needs to work at all |
|---|---|
| Solo campaign | Nothing. Every randomizer feature is optional |
| Campaign co-op | `[Coop] Enabled=1` (Expert page) and the identical DS2United.asi on every PC. Menu path: `[Menu] Enabled=1`, `CampaignCoop=1`, `[MpMap] Enabled=1`. Direct path: the `DS2_Coop_*` launchers or `[Coop] Network=1` + `Peer`. Clients need `GlobalTemplates=1` and the same pool files, else the host's pool enemies are invisible |
| Survival, Mercenaries, Objective, Hold the Zone | Co-op half (`[Coop] Enabled`, `[Menu] Enabled`, `[MpMap] Enabled`), plus the director chain: `[DS2Randomizer] GlobalTemplates=1` with the pool files in `randomizer\`, `[Survival] Enabled=1`, `[Survival] Map=any` (or that map). Since 13.10 the randomizer master may be off (survival-only mode), and the picker refuses a launch the director cannot play |
| Objective, extra | An objective table for the map (all 7 MP maps have one) |
| Hold the Zone, extra | `[Gate] Enabled=1` (the objective opener sends through the gate hook), spots in `plugins\DS2Zones.txt` (7 to 9 per map, all 7 maps) |
| Versus | Co-op half, `[Versus] Enabled=1`, `[Necro] Enabled=1` (else the necro team spawns as Isaac). AI slashers also need the director chain and `[VersusSlashers] Enabled=1`; `[Survival] Map` does not matter |
| Campaign arenas | Co-op half, `[Arena] Arenas=1`, director chain with `Map=any` or the arena name. Survival, Mercenaries and Hold the Zone with `[Arena] Modes=1` (Objective and Versus play survival there); respawn needs `[Respawn] Enabled=1` |
| MP-map key visit | Co-op half, `[MpMap] Enabled=1`. Without the mode row every visit plays survival (13.10) |

### Solo campaign

| Feature | Default | Switch | Status | Notes |
|---|---|---|---|---|
| Enemy replacement | off (Chance=0) | [DS2Randomizer] Enabled, Chance | Works (15.–29.09) | Master is on, but at Chance=0 nothing is replaced. Raise Chance to randomize |
| More enemies (total 4) | on | MoreEnemies, [MoreEnemies] | Works (07.10) | Swarmer slots never multiplied; story scene actors skipped (MoreEnemiesSkipScripted, untested) |
| Hordes | on (6 / 4) | SwarmHorde, PackHorde | Partial (15.09) | Only when a swarmer or pack boy replaces another kind, so never at Chance=0 |
| Extra spawns | on | ExtraSpawns, DS2ExtraSpawns.tsv | Works (15.09) | Area rows untested |
| Loot scaling | ammo 134 %, credits 100 % | [Loot] | Untested | |
| Schematic randomizer | on | [SchematicRandomizer] Enabled | Works (05.10) | |
| Bench randomizer | on | [BenchRandomizer] StatShuffle, NodeShuffle | Partial (13.09) | Wrong values once; fixes untested |
| Watchdog | on | Watchdog | Partial (21.09) | |
| Companion bots | on, 3 | [Bot] Enabled, Count | Works (09.10, 13.10) | |
| Turret, stasis mine, heal field | on | [Turret], [Gadgets] | Untested | |
| Debug menu | on | [Debug] Enabled (Insert) | Works (13.10) | Used for the ch02 jump |

### Campaign co-op (host and client)

Played online 13.10 (Hamachi, ch02, 3 bots): no crash, loading together 3 of 3, the gate never timed out. Lifts left without the partner, story NPCs stayed on one machine, and the loadout was lost after each restart; all three fixed 13.10.

| Feature | Default | Switch | Status | Notes |
|---|---|---|---|---|
| Lobby and connection | on | [Menu] CampaignCoop, [Coop] JoinAddress | Works (13.10) | Lobby refuses another build; the direct path only warns |
| Player ghosts | on | [Coop] GhostPlayer | Works (17.09) | |
| Host-owned enemies | on | [Coop] HostNPCs | Works (13.10) | Internet risk: 2 s template check window |
| Client hits on enemies | on | [Coop] ReportHits, ApplyHits | Works (13.10) | Each report goes out three times; the 1025 "stale" of 1578 were the folded copies (log field now `repeat_copies`) |
| Progression gate | on | [Gate] Enabled | Works (17.09, 13.10) | |
| Lifts wait for everybody | on | [Coop] LiftWait, [Gate] RideTriggers | Untested | Broken until 13.10 (lift check never matched); fix 13.10 |
| Doors wait, door votes | on | [Mirror] DoorsWaitForPartners, DoorsVote | Works (13.10) | Floods the log |
| World mirror | on | [Mirror] Enabled | Partial (13.10) | NPC removals fixed 13.10; drops, crates, lockers not shared |
| Checkpoint and level follow | on | [Checkpoint], [Level] | Works (17.09) | Empty inventory after restart fixed 13.10; 02.11.: a client restart loads the host's newest checkpoint (HostCheckpointOnRestart), untested |
| Start levels together | on | [Loading] SyncWithPartners | Works (13.10) | |
| Host load barrier | on | [Loading] HostBarrier, SyncTimeoutMs, JoinTimeoutMs, MemberSilenceMs | Untested | New 02.11.: the host numbers every load and releases it for everybody at once; 0 = the older rule |
| Rooms loaded before the start | on | [Loading] WaitForRooms, RoomsWaitMs | Untested | New 02.11.: under the native loading screen; replaces the black curtain (CurtainUntilRoomsLoaded=0) |
| Join only at a host restart | on | [Level] JoinOnHostRestart | Untested | New 02.11.: lobby joiners and menu clients load with the host's next checkpoint restart; the host's loading screen waits for them |
| Joiner born in his suit | on | [Loading] SuitWaitMs | Untested | New 02.11.: chapter start suit, or the client progress file's suit; no patient suit, no change 5 s later |
| Die together | on | [Coop] DeathSync, MirrorDeath | Unclear | Acks per partner since 25.09 |
| Suit, gun, helmet sync | on | [Coop] SuitSync, LoadoutSync, HelmetSync | Partial (13.10) | Ripper, Flamethrower fixed 13.10 |
| Cutscene leader | on | [Cutscene] LeaderDecides | Partial (24.09) | |
| Bots online | on | [Bot] GhostToClients | Works (13.10) | Bots exist on the host only |
| Config check | on | [Coop] ConfigCheck | Untested | Warns only |

### Survival (waves)

| Feature | Default | Switch | Status | Notes |
|---|---|---|---|---|
| Waves 1 to 10, endless after | on | [Survival] Enabled, [Wave1..10] | Works (09.10, 13.10) | 13.10 online, mp_gov to wave 15 |
| Boss wave 10, escalation from 11 | on | [Wave10], [Escalation] | Works (13.10) | Ran in the 15-wave run; balance not reviewed |
| Vent spawns | on | [Survival] VentSpawns + [DS2Randomizer] VentRebinding | Partial (13.10) | VentMaxAbove now measures a wall vent against the floor below it (13.10) |
| Wave rewards | on | [Survival] Reward* | Works (09.10) | |
| Respawn and game over | off (installed on) | [Respawn] Enabled | Partial (12.10, 13.10) | Origin falls fixed 13.10 |
| Spawn and respawn by the game's own placement, born in the worn suit | on | [Lifecycle] NativePlacement, EarlySpawn | Untested | New 02.11.: Player::Init puts the body on our point (hook 0x008A8505, in from the start); MP arrivals spawn under the loading screen; the old curtain and guards stay as logged fallbacks |
| Game over restart together | on | [Lifecycle] SyncedRestart, RestartAckMs | Untested | New 02.11.: the host announces, downed players wait for the reload, partners ack, then the restart |
| Late join sync | on | [Lifecycle] StickyTemplates, JoinTimeline, JoinResync | Untested | New 02.11.: enemy scope kept per partner (no vanishing enemies on respawns or stalls), join timeline in the log, re-ghost on request |
| Supply terminal | on | [Shop] Enabled (V) | Works (13.10) | Key hint (notice + card) when the first round arms, new 13.10 |
| Bot kit by waves | on (after waves 3, 5, 7, 9) | [Bot] Progression, ProgressionWaves, ProgressionStagger | Untested | New 13.10: cheaper gun, suit, expensive gun, fourth gun, the bots on different waves; no gun purchases with credits here any more; installed 14.10., untested |
| Wave music | on | [SurvivalMusic] Enabled | Untested | MP tracks never played on a visit; campaign sections since 13.10 |
| Status line | on | [Hud] Enabled, [Survival] Hud | Partial (13.10) | Client did not show it; fix 13.10 |
| Last enemies marked | on (3) | [Survival] ShowLastEnemies, [Marks] Enemies | Untested | New 13.10 |
| Cleanup | on | [Cleanup] | Untested | New 13.10 |
| Online survival | on | [Menu] Enabled (host / join co-op) | Partial (13.10) | mp_solar, mp_gov; fixes 13.10 |

### Mercenaries

| Feature | Default | Switch | Status | Notes |
|---|---|---|---|---|
| Clock, kills add time, combos | on | [Mercenaries] StartSeconds, KillMs, Combo* | Partial (13.10) | Crash after a 4 min pause; fix 13.10 |
| Time capsules | 2 at once, 8 per run, 20 m+ | Capsules, CapsuleTotal, CapsuleMinMeters | Untested | Spread across the map since 13.10 |
| Bonus fade, run limit | 480 s to 20 %, 900 s | BonusFadeSeconds, BonusFloorPercent, MaxRunSeconds | Untested | New 13.10 |
| Kill target, final enemies, ranks | 150 kills | KillTarget, FinalEnemies, Rank* | Unclear | |
| Shop | none | | | Mercenaries has no terminal |
| Bot kit at the start | on | [Bot] ModeLoadout | Untested | New 13.10: each bot its own suit, the Pulse Rifle and one more gun, like the character setup; installed 14.10., untested |
| Whole map open on arrival | on | [Zone] OpenObjectives | Untested | New 13.10: objectives opened like Hold the Zone, the last one never starts |

### Objective

| Feature | Default | Switch | Status | Notes |
|---|---|---|---|---|
| MP map objectives against hordes | on | [Objective] | Untested | Built 11.10 |
| Defend phases | 12 dmg/s within 4 m | DefendDamagePerSecond, DefendRadius | Untested | |
| Objective chain once per round | on | [Mirror] MpObjectivesOnce | Untested | Fix 13.10 for client completions |
| Objective damage scaled to team | on | [MpMap] ObjectiveDamage | Untested | |
| Objective props last longer | x3 | [Objective] PropHealthFactor | Untested | New 13.10, any team size, defend hits too; installed 14.10., untested |
| Hold switches longer per player | +50 % per human | [Objective] HoldPerPlayerPercent | Untested | New 13.10, bots do not count; installed 14.10., untested |
| Objective again after the restart | always | | Untested | Fix 16.10: the checkpoint restart cleared nothing, the next run played survival ("objectives are spent") |
| Clock floor after each objective | 90 s | [Objective] StepFloorSeconds | Untested | New 16.10: Marker Lab has one map bonus, the clock ran too short |
| One notice per objective with all its time | always | | Untested | Fix 16.10: showed "+30 S" while the map's own +60 s came 100 ms earlier |
| Contact Beam on objective props | 50 % | [Objective] ContactBeamPercent | Untested | New 16.10, Objective and Versus visits, inflictor bit 0x200 |
| Bot kit by round time | on (2, 4, 6, 8 min) | [Bot] Progression, ProgressionMinutes, ProgressionStaggerSeconds | Untested | New 13.10: as in survival, by game time; installed 14.10., untested |

### Hold the Zone

| Feature | Default | Switch | Status | Notes |
|---|---|---|---|---|
| Moving capture zone | on | [Zone] (survival half) | Partial (12.10) | Played on Concourse; ring and HUD fixed same day |
| Open every objective on arrival | on | [Zone] OpenObjectives | Untested | Needs [Gate] |
| Bonus fade, run limit | 600 s to 20 %, 720 s | [Zone] BonusFadeSeconds, MaxRunSeconds | Untested | New 13.10 (12 min) |
| Ring, locator, waypoint | on | [Zone] Ring, Locator, [Marks] Zone | Unclear | |
| Bot kit by round time | on (2, 4, 6, 8 min) | [Bot] Progression, ProgressionMinutes, ProgressionStaggerSeconds | Untested | New 13.10: as in survival, by game time; installed 14.10., untested |

### Versus

Played online 13.10 (Titan Mines, two rounds with a side swap). Fixed the same day: the partner's Ripper and Flamethrower were invisible, the partner came back in the Engineering suit with empty hands after every respawn, the other machine switched off an objective, the client's round clock ran out 390 s early, no necro vision, no points for killing a bot, stray guns at 0,0,0.

| Feature | Default | Switch | Status | Notes |
|---|---|---|---|---|
| Match on the MP systems | on | [Versus] MpBits, NativeRespawn, Scoring, KillerCard, Rounds | Partial (12.10, 13.10) | |
| Necro players | on | [Necro] Enabled | Works (21.–22.09) | Pack, Leaper |
| AI slashers | on, 5 | [VersusSlashers] | Partial (13.10) | Client loot fixed 13.10 |
| Bots die and respawn | on | [Bot] VersusDeaths | Unclear | |
| Bot grab escape | on | [Bot] NecroGrabEscape | Untested | New 13.10 |
| Character setup | off (installed on) | [Loadout] Enabled | Works (13.10) | Picks used in the match; joiner's lobby tabs fixed 13.10 (installed 14.10., untested) |
| Bot kit at the start | on | [Bot] ModeLoadout | Untested | New 13.10: own suit, Pulse Rifle and one more gun (campaign guns, not the MP ones); installed 14.10., untested |

### Campaign arenas

| Feature | Default | Switch | Status | Notes |
|---|---|---|---|---|
| arena_ch04 (stalker room), arena_ch10 (checkpoint) | on | [Arena] Arenas | Partial (09.10) | ch04 launched, doors locked, waves ran |
| arena_ch03 (church crypt, 55 rooms, streaming by stream sets, doors held open, no Brutes) | on | [Arena] Arenas | Untested | New 16.10 |
| arena_ch08 (ishimura, ch08_ish, 34 rooms, 19 doors: 12 held open, the boundary locked) | on | [Arena] Arenas | Untested | New 16.10; two recordings combined |
| arena_ch03b (church sanctuary), arena_ch05 (sprawl hub), arena_ch09 (mining entry hall), arena_ch11 (marker floor) | on | [Arena] Arenas | Untested | New 16.10 from the user's recordings |
| arena_ch01 (tripod room: chapter 1, the tripod boss fight's one room, vents only; the boss stays out) | on | [Arena] Arenas | Untested | New 16.10, wrongly dropped first (one stream set, so the recorder saw no activation) |
| Versus picked plays survival | always | | Untested | Fix 13.10; Objective too since 16.10 |
| Mode lock on the map row | always | | Untested | Fix 16.10: the map row skips arenas that cannot play the picked mode; "any map" too; an arena launch commits the mode it plays |
| Mercenaries and Hold the Zone in arenas | on | [Arena] Modes | Untested | New 16.10: built-in capsule and zone points |
| Respawn in arenas | on | [Arena] Modes, [Respawn] Enabled | Untested | New 16.10: arena point nearest to the team |
| Start kit | on | [Arena] Kit, Suit | Unclear | |
| Native start: born on the start spot in the kit's suit, kit in the first tick, no curtain | on | [Lifecycle] NativePlacement | Untested | New 02.11. (after the arena_ch01 test); also after arena restarts; old kit change, teleport and curtain only as logged fallbacks |
| Bot kit by waves | on | [Bot] Progression, ProgressionWaves | Untested | New 13.10, as in survival; installed 14.10., untested |
| Co-op joiners | on | [Arena] Modes | Untested | Fix 16.10: the launch carries an arena code, joiners start the same arena |

### MP-map key visit

| Feature | Default | Switch | Status | Notes |
|---|---|---|---|---|
| Visit an MP map from the campaign | on | [MpMap] Enabled, Key (Numpad . twice), Map | Works (09.10) | |
| Saves held during the visit | on | [MpMap] HoldSaves | Works (09.10) | |
| Outbreak maps | on | RegisterPcmCodec, SoundBankOverflow, OutbreakRender | Works (09.10) | Concourse; Academy unclear |

## Feature catalog

### Randomizer (`[DS2Randomizer]` unless named)

| Feature | Switch | Default | Status | Note |
|---|---|---|---|---|
| Master switch | Enabled | on | Works (13.10) | Off: frame cap and NavPower only, or survival-only mode |
| Seed | Seed | 69 | Works (15.–29.09) | Never change mid playthrough |
| Replacement chance | Chance | 0 | Works (15.–29.09) | 0 = nothing replaced |
| Cross-family swaps | CrossFamily | on | Works (15.–29.09) | |
| Vent swaps and rebinding | VentVariants, CrossFamilyVents, VentRebinding | on | Works (15.–29.09) | Lurker kept out of vents ([VentFamilies]) |
| Relaxed vent matching | VentMessageContract | off | Off | |
| Scripted and infected arrivals | ScriptedArrivals, InfectedArrivals | on | Unclear | Template says not tested in game |
| Direct donors | DirectDonors | on | Unclear | Only removes the broken pack boy |
| Cross-level enemy pool | GlobalTemplates, GlobalStreamProbe | on | Works (15.09, 13.10) | Needs `randomizer\ds2_randomizer_pool.*` |
| Pool lifetime | PoolActorsFollowLevel, ReleasePoolAtLevelEnd, GuardServiceRelease | on | Works (15.09) | PoolHoldForever, PinPoolPrerequisites off |
| Loading waits for pool | LoadingWaitsForPool, MaximumLoadingSeconds | on, 30 s | Unclear | |
| Zone switch fix | ZoneUnloadHeal | on | Unclear | High FPS lift hangs |
| Frame cap | FpsCap | 0 | Unclear | 0 = uncapped; whether a cap is still needed is open |
| NavPower pool | NavPowerPoolKiB | 4608 | Partial (20.09) | Retail 576 |
| Families, weights | [Weights], [Sources], [Targets], [VentFamilies], [ExperimentalClasses] | see template | Works (15.–29.09) | Brute, Stalker never work as replacements |
| Boss donors | [ExperimentalClasses] DividerCore, Hunter | on | Broken | Divider softlock or unkillable; fixes untested |
| Stat variants | Variants, VariantRuntime | off | Off | Build carries no variants; Ghost and Gold Leaper crashed. Since 15.10. one folder per variant (variants/packs, tools/variant_pack.py, variants/README.md) |
| More enemies | MoreEnemies, MoreEnemiesVents, [MoreEnemies] | on, 4 | Works (07.10) | MaxPerTrigger 4, PerSecond 48 |
| Vent repeat gap | MoreEnemiesVentDelayMs, MoreEnemiesVentGapMs | 2000, 4000 | Untested | Two slashers in one vent crashed |
| Skip scene actors | MoreEnemiesSkipScripted | on | Untested | 12.10 |
| Hordes | SwarmHorde, PackHorde | 6, 4 | Partial (15.09) | Kill-count fix untested |
| Extra spawns | ExtraSpawns, PlacementHotkey (F10) | on | Works (15.09) | |
| Spawner rules | SpawnerTools, DS2SpawnerRules.tsv | on | Works (20.09) | Spawner Editor writes it |
| Loot scaling | [Loot] AmmoPercent, CreditsPercent | 134, 100 | Untested | |
| Schematic randomizer | [SchematicRandomizer] Enabled, StoreFollowsSchematics | on | Works (05.10) | Store follow untested |
| Bench randomizer | [BenchRandomizer] StatShuffle, NodeShuffle | on | Partial (13.09) | |
| Watchdog | Watchdog, WatchdogKill, WatchdogRepairTables | on | Partial (21.09) | |
| Death message stand-in | WatchdogDeathProxy, WatchdogReleaseDeathMessage | on | Works (21.09) | |
| Game-thread driver | | always | Untested | New 13.10: runs with Watchdog=0 |
| Diagnostics | ObserveSpawns, Stream*Diagnostics, MessageTrace, TextureProbe, SoundProbe | mixed | Works (13.10) | SoundProbe 1 installed |

### Survival director (`DS2Survival.ini` sections)

| Feature | Switch | Default | Status | Note |
|---|---|---|---|---|
| Director | [Survival] Enabled, Map | on, any | Works (13.10) | code: off, mp_bombRun |
| Survival-only mode | [DS2Randomizer] Enabled=0 + [Survival] Enabled=1 | | Untested | New 13.10 |
| Readiness check | DS2Survival_Ready export | always | Untested | New 13.10 |
| Spawn placement | MinDistance..MaxDistance, FloorBand, WallClearance | 12–25 m | Partial (09.10) | Floor issue fixed 09.10 |
| Vent spawns | VentSpawns, VentShare, VentAnyMarker, VentMaxAbove | on, 40 %, 2.5 m | Partial (13.10) | VentMaxAbove new 13.10 |
| Hunting | ForceAware, RepairBlindTemplates, MaxChargers, ZombieChargeIgnoreVis, ChargeUnseen | on, 2, 0, 0 | Untested | 17.10.: charges only on screen and in sight again |
| Nav repair for blind templates | | always | Untested | New 13.10 |
| Stuck watchdog | StuckSeconds, VentStuckSeconds, InertSeconds | 60, 30, 20 | Works (13.10) | Its kills left corpses; Cleanup 13.10 |
| Rewards | RewardAmmoPerGun, RewardMedPacks, RewardCredits | on | Works (09.10) | |
| Escalation | [Escalation] FromWave | 11 | Works (13.10) | |
| Endless cycle | EndlessCycleFrom..To, BossEvery | 4..9, 10 | Works (13.10) | |
| Pause stops its clock | | always | Untested | New 13.10 |
| Cleanup | [Cleanup] StuckCorpseDelete, FadeSweeper, PickupCap | on, 40 items | Untested | New 13.10 |
| Last enemies marked | [Survival] ShowLastEnemies | 3 | Untested | New 13.10 |
| Notices, status line | Notices, Hud | on | Partial (13.10) | |
| Mercenaries | [Mercenaries] | | Partial (13.10) | |
| Objective | [Objective] | | Untested | |
| Hold the Zone | [Zone] | | Partial (12.10) | |
| Versus AI slashers | [VersusSlashers], [VersusSkins] | on, 5 | Partial (13.10) | |
| Wave music | [SurvivalMusic] (co-op half) | on, auto | Untested | Fix 13.10 |
| Best wave per map | DS2SurvivalBest.txt | | Unclear | |

### Co-op (`[Coop]` unless named)

| Feature | Switch | Default | Status | Note |
|---|---|---|---|---|
| Co-op half | Enabled | on | Works (13.10) | Off removes menu, MP maps, bots, shop |
| Network session | Network, Port, Peer, JoinAddress | off, 27015 | Works (15.09, 13.10 online) | Ports Port, +1000, +2000 UDP; IPv4 only |
| The host leaving ends the match for everybody | EndWithHost | on | Untested | New 16.10: 3 s "ended" level packets, partners back to the main menu, sessions torn down, lobby closed, join item reset; host or join again without a restart |
| Survival lobby | [Menu] Enabled | on | Works (13.10) | Build check |
| Rejoin from the main menu | | always | Untested | New 13.10 |
| Native lobby screen | [NativeLobby] Enabled | off (installed on) | Untested | Campaign arenas too since 16.10 ([Arena] Modes); host LAUNCH button since 16.10 |
| Player ghosts, ghost weapons | GhostPlayer, GhostGunReequip | on | Works (17.09, 24.09) | |
| Host-owned enemies | HostNPCs | on | Works (13.10) | |
| Local NPC classes | LocalNPCClasses, NeutralNPCsLocal, ScriptedSpawnsLocal | NicoleBoss, on | Unclear | |
| Hits and world hits | ReportHits, ApplyHits, WorldHits, WorldPropHits | on | Partial (13.10) | Ghost prop hits counted twice, fixed 13.10 |
| Progression gate | [Gate] Enabled | on | Works (17.09) | |
| Lifts | LiftWait, [Gate] RideTriggers | on | Untested | Fix 13.10 |
| World mirror | [Mirror] Enabled | on | Partial | |
| Doors | [Mirror] DoorsWaitForPartners, DoorsVote, DoorsStayOpen | on | Works (13.10) | |
| Streams | [Streams] PinPartnerStreams, FollowPartners, DoorLeaveFix | on | Partial (13.10) | Keep list full at 46 |
| Checkpoint, level follow | [Checkpoint], [Level] | on | Works (17.09) | |
| Loading together | [Loading] SyncWithPartners | on, 60 s | Works (13.10) | |
| Death sync | DeathSync, MirrorDeath | on | Unclear | |
| Suit sync | SuitSync, SuitStream | on | Partial | Fixes 12.10, 13.10 |
| Weapon sync | LoadoutSync, ProvisionWeapons | on | Partial | Fixes 13.10 |
| Helmet sync | HelmetSync | on | Unclear | |
| Cutscenes | [Cutscene] | on | Partial (24.09) | |
| Partner in cutscenes (puppet) | [ScenePartner] Enabled, DefaultMode, PuppetScenes, HiddenScenes, Clip, SideOffset, BackOffset, Follow, PuppetCollision | off | Stopped (19.10) | Ended by the user; code stays, off |
| Dementia mutator (sanity) | [Dementia] Enabled, Modes, AloneMeters, RiseSeconds, FallSeconds, BotsCount, Pace, Flashes, PackChildren, ForceStage | off | Untested | New 19.10; this machine only; sounds in plugins\dementia |
| Kinesis objects | [Kinesis] StreamHeld | on | Unclear | |
| Physics rest sync | [Physics] RestSync | on | Unclear | |
| Pause keeps running | PauseKeepsRunning | on | Unclear | |
| Catch-up key | CatchUpKey (F8) | on | Unclear | |
| Partner timeout | PeerTimeout, PeerTimeoutMs | on, 30 s | Unclear | |
| Respawn on MP maps | [Respawn] Enabled | off (installed on) | Partial (13.10) | 3 of 4 respawns fine |
| Nameplates | [Nameplate] Enabled, Overlay, Bots | on | Partial | No plate over necros, fix 13.10 |
| RIG notices | [Notice] Enabled | on | Works (17.09) | |
| Hint cards | [Notice] OverlayHints | on | Untested | New 13.10 |
| Status line | [Hud] Enabled | on | Partial (13.10) | |
| Status line colour | [Hud] Color, FlashColor | stock | Untested | New 15.10 |
| Waypoints | [Marks] Zone, Capsules, Enemies | on | Unclear | Enemies new 13.10 |
| Config check | ConfigCheck | on | Untested | Severity sorting 13.10 |
| Crash dumps | CrashDumps | on | Works (13.10) | C:\DS2CrashDumps |
| Crash guards | CrashGuards (code key) | on | Untested | Submix, Lurker guards 12.10; vent state flag 0x00E93CCF 16.10 |
| Two copies on one PC | BackgroundFullSpeed, UnpinThreads, FocusInputOnly | on | Works (17.09) | |
| Turret | [Turret] Enabled (Numpad 6) | on | Untested | Owner only |
| Stasis mine, heal field | [Gadgets] (Numpad 7, 8) | on | Untested | Owner only |
| Supply terminal | [Shop] Enabled (V) | on | Untested | Clients follow host state since 13.10 |
| Debug menu | [Debug] Enabled (Insert) | on | Works (13.10) | Holds saves after a jump; HOST tab built 14.10., untested |

Story co-op coordinates catalogued player scenes using the level's own movement commands. The host grants one initiator for the current cue. Each participating game checks the load generation, local player, registered cue and required streams before voting. Gather scenes also require arrival or proximity to the initiator. Pull scenes run the authored movement on each local Isaac. A managed scene keeps waiting while a required vote is missing. The two cosmetic helmet cues stay local.

The church and Psych Ward sequences also identify their immediate actor and camera cues by their parent sequence. Native and mirrored copies of one such step share one execution per start. Unclassified cues, older peers and non-story modes use their existing synchronization paths.

### Bots (`[Bot]`, profiles in `[Bot1..3]`)

| Feature | Switch | Default | Status | Note |
|---|---|---|---|---|
| Companion bots | Enabled, Count | on, 3 | Works (09.10, 13.10) | Cannot die outside Versus |
| Send away, call | Key (F9), CallKey (Numpad 0) | on | Unclear | |
| Orders | FollowHoldKey..VanguardKey (Numpad 1–5) | on | Unclear | |
| Mark key | MarkKey (Numpad 9) | on | Unclear | |
| Partner pose | PartnerPose | on | Unclear | |
| Fighting, aim | HumanAim, HumanSenses, AimLead | on | Partial | Slow fire 07.10, fixes untested |
| Snap shots at twitchers | TwitcherSnapShots | on | Untested | New 18.10; twitchers dodge any aim after 0.25 s |
| Stasis first on twitchers and charging stalkers | StasisPriority | on | Untested | New 18.10; user: "definitely more stasis" |
| Kinesis throws | Kinesis | on | Partial (27.09) | Grabs only; throw fixes untested |
| Rescue when grabbed | RescueLeader | on | Unclear | |
| Bench, store, kiosks | Bench, Store, UseKiosks, StoreRooms | on | Partial (28.09) | KioskEnter off |
| Buy guns, credit share | BuyGuns, CreditShare, OwnGunsOnly | on, 100 % | Partial (13.10) | Survival 13.10: first guns only before wave 6 (credits too low at campaign prices); outside the campaign the kit below chooses the guns now |
| Kit outside the campaign | ModeLoadout, Progression, ProgressionWaves, ProgressionMinutes, ProgressionStagger, ProgressionStaggerSeconds, ProgressionSeed | on | Untested | New 13.10: free through the store's gun path and the suit change; never in the campaign; installed 14.10., untested |
| Own suits | Suit, FavoriteSuits | on | Untested | |
| Per-save bot gear | PerSaveInventory, DS2BotSaves\ | on | Partial (06.10) | |
| Node doors, stasis stations | UseNodeDoors, UseStasisStations | on | Untested | |
| Loot corpses and crates | StompCorpses, LootContainers | on | Unclear | |
| Teleport catch-up | AutoTeleport | on | Unclear | |
| Zero-G flight, shared air | ZeroGFlight, ShareAir | on | Untested | |
| Vent tubes | FollowIntoTubes | | Removed (13.10) | Tube guard keeps bots out |
| Bots ignore trigger volumes | IgnoreTriggers | on (template) | Untested | 13.10: before this, bots fired TriggerPhysical volumes (door locks, checkpoints, cull/stream sets, ambushes); now the trigger's own entity filter refuses them |
| Bot use press guard | (always with bots) | on | Untested | 13.10: a bot's use starts only bench, store, locker, node door, stasis station; no switches, hacking stations, save kiosks |
| Logs left for the leader | (always) | on | Untested | 13.10: bots no longer pick up audio and text logs |
| Wheel guard, own aim light | WheelGuard, OwnAimLight | on | Untested | |
| Antics | AnticsEnabled, [BotN] Antics | on, 0 | Unclear | Installed profiles 1/3/1 |
| Grab escape from necro players | NecroGrabEscape | on | Untested | New 13.10 |
| Versus deaths | VersusDeaths, VersusRespawnMs | on, 5 s | Unclear | |

### Versus and necro players (`[Versus]`, `[Necro]`)

| Feature | Switch | Default | Status | Note |
|---|---|---|---|---|
| Versus mode | [Versus] Enabled | on | Partial (12.10, 13.10) | |
| Necro players | [Necro] Enabled | on (code 0) | Works (21.–22.09) | Template key and configurator row new 13.10 |
| Necro type menu, vent pick | [Necro] TypeMenu (code key) | on | Works (22.09) | |
| Sixth necro type (Lurker or Leaper) | [Necro] SixthType, SixthTypeGuid, Leaper | on, Lurker GUID, Leaper 0 | Works (22.09) | Leaper verified |
| Sixth type as Ubermorph | [Necro] SixthTypeGuid, SixthCharStream, LeaperRecordStream | installed INI only | Partial (22.09) | Animations work; crashes as a Versus necro |
| Necro pool on F11 | [Necro] LoadPool | 2 | Works (21.09) | |
| Necro vent fall rescue | [Necro] VentFallRescue (code key) | on | Untested | Under-map check 13.10 |
| MP death, respawn, scoring | MpBits, NativeRespawn, Scoring, KillerCard | on | Partial (13.10) | |
| Two rounds, side swap | Rounds, SwapSeconds | on, 12 s | Works (13.10) | |
| Team groups | TeamGroups | on | Unclear | |
| MP inventory rule | Inventory | 0 | Unclear | |
| Scoreboard | ScoreKey (Tab) | on | Unclear | |
| Points | [VersusPoints] | | Unclear | |
| Native MP audio | NativeAudio | on | Untested | |
| Round music | Music | on | Untested | Fix 13.10 |
| MP loot | MpLoot | on | Partial | Client drop list fixed 13.10 |
| Necro vision | [Versus] NecroVision (code key) | on | Untested | New 13.10; crash on a rebuilt body fixed 13.10 (installed 14.10., untested) |
| Necro points for bot kills | | always | Untested | New 13.10 |
| Necro nameplates | [Nameplate], [Marks] | on | Untested | New 13.10 |
| AI slashers, pressure | [VersusSlashers] | on, 5 | Partial (13.10) | |
| Necro Pack bots | | | Off | Design only (NECRO_BOT_PACK_DESIGN_2026-10-13.md) |

### Menu and frontend (`[Menu]` unless named)

| Feature | Switch | Default | Status | Note |
|---|---|---|---|---|
| Survival in the main menu | Enabled | on | Works (09.10) | No EA login |
| Co-op campaign in the main menu | CampaignCoop | on | Works (13.10) | |
| Outbreak maps in the picker | ExtraMaps | on | Works (09.10) | Needs DS2DAT12.DAT |
| Arenas in the picker | [Arena] Arenas | on | Works (09.10) | |
| Mode switch in the picker | Modes, Mode | on, 0 | Works (12.10) | Modes=0 means survival since 13.10 |
| Native "Game Mode" row | NativeModeRow | on (since 13.11.) | Works (12.10) | Needs randomizer\ds2coop_frontend.str |
| Lower-case mode and label text | | always | Unclear | "game mode" label, row texts |
| Frontend colours (Mark's MP20 movies) | NativeModeRow | | Untested | |
| Character setup screens | [Loadout] Enabled | off (installed on) | Works (13.10) | |
| Launch refusal with hint | | always | Untested | New 13.10 |
| RIG and visor colour | [Visuals] RigColor, VisorColor | stock | Untested | New 15.10, any suit |
| Trailer camera (free camera on the pad, slow motion, path recorder) | [CameraTools] Enabled | off | Black picture and crash 16.10, fixed (game HUD stays) | New 16.10 |
| Stasis, kinesis, laser, weapon and blood colours | [Visuals] StasisColor, KinesisColor, LaserColor, WeaponColor (+ one key per gun), BloodColor | stock | Untested | New 16.10 |
| Mod block at the start of the credits | [Credits] Enabled, Title, Role, Names, BlankLines | on | Untested | New 18.10; original credits follow unchanged |

### Configurator (`DS2UnitedConfig.exe`, `tools\RandomizerConfig\`)

| Feature | Status | Note |
|---|---|---|
| Pages: General, Enemies & Families, More Enemies, Loot, Schematics & Bench, Loading & Performance, Watchdog & Advanced, Diagnostics, Co-op, Bots, Survival, Versus, Expert | Works (13.10) | Expert lists every other key |
| Save with backup, reset to recommended | Works (13.10) | Two newest backups kept |
| `--migrate` (legacy INIs into DS2United.ini) | Works (12.10) | Built the installed DS2United.ini |
| `--selftest` | Unclear | |
| Warnings that name what breaks and the fix | Untested | New 13.10 |
| Rows for [Necro] Enabled and Mercenaries/Zone limits | Untested | New 13.10 |
| Survival map validation | Untested | any, 7 MP maps, arena_ch04, arena_ch10 |

### Other tools

| Tool | Status | Note |
|---|---|---|
| Spawner Editor (`StartSpawnerEditor.cmd`, `tools\spawner_editor.py`) | Works (20.09) | Writes DS2SpawnerRules.tsv, DS2ExtraSpawns.tsv |
| Arena recorder | Works (09.10) | [Arena] Recorder, Home/End; door marks with DoorKey (Delete) new 16.10, untested |
| Zone spot recorder | Works (12.10) | [Zone] RecordKey PgUp, DeleteKey PgDn |
| Crash dump switch (`tools\DS2_enable_crash_minidumps.cmd`) | Works (12.10) | Dumps in C:\DS2CrashDumps, Windows keeps 5 |
| DS2ModLoader.asi (loose-file mods) | Unclear | Separate plugin, not installed |
| Animation banks (`tools\ds2_bnk.py`, `tools\ds2_anim_json.py`, `tools\ue5\ds2_export_anim.py`) | Works offline (19.10) | Read, write, append, replace and new banks; JSON and UE5 bridge; round-trip tests `tools\test_ds2_bnk.py`, `tools\test_ds2_anim_json.py`; not yet loaded in game |
| Scene list (`tools\list_coop_scenes.py`) | Works (19.10) | Every story scene with chapter, parts, cast and a proposed partner mode |
| Scene export (`tools\export_scene.py`, `tools\blender\ds2_scene.py`, `tools\ue5\ds2_export_anim.py`) | Works in Blender (19.10) | A cutscene as glTF: every actor's skeleton and clips, the camera on its bone and free. Blender import and export round-trip exactly (tested headless, Blender 5.2). UE5 export reads the import's axis map from the rest pose (math tested, UE not run). Guide: docs\CUTSCENE_EDITING.md |

## Changed on 2026-10-13 (untested)

Randomizer and survival
- Survival-only mode: master off with `[Survival] Enabled=1` keeps the director; campaign replacement, MoreEnemies, hordes, extras, loot, schematic and bench randomizers and the watchdog stay off (observer.cpp, runtime_selection.inc).
- Game-thread driver installed without the watchdog (`Watchdog=0` no longer stalls queued vent repeats, area extras, placement key).
- Readiness export `DS2Survival_Ready`; the map picker refuses a mode that cannot run, Versus without AI slashers launches with a warning, a joiner is warned when its own survival is off.
- `[Menu] Modes=0` now means every visit plays survival; a campaign arena plays survival when Versus was picked.
- Mercenaries: 2 capsules at a time, 8 per run, spread across the map 20 m+ from players; time bonuses fade (BonusFadeSeconds 480, BonusFloorPercent 20); MaxRunSeconds 900.
- Hold the Zone: bonus fade (600 s to 20 %), MaxRunSeconds 1200.
- `[Survival] ShowLastEnemies=3`: the last enemies of a wave are marked on screen, on clients too (survnet kind 8, `[Marks] Enemies`).
- `[Cleanup]`: stuck corpses deleted, stalled fades finished, pickup cap 40 (crash 16592, FPS loss in long runs).
- The director's timers stop during the pause.
- VentMaxAbove 2.5 m: a wall vent more than 2.5 m above the walkable floor below it is left to the necro players (measured per marker on the nav mesh, no longer against the nearest player).
- VentMustReach=1: a vent is used only when the living can walk to its floor (NavPower path from the nearest player); catwalk vents with no way down stay shut (13.10., Solar Array). Installed 14.10., untested.
- Catch-up (`CatchUpMeters=50`, `CatchUpMetersFour=40`, `CatchUpSeconds=0`): an enemy farther than the catch-up distance from every living player is taken away at once and queued again, so it comes back near them by vent or direct spawn. 50 m solo, 40 m with four real players (linear, bots do not count, never below the spawn distance + 2 m). Not in Versus, not for Brutes or the Ubermorph, not inside a vent, not in the first 4 s; no kill reward, no Mercenaries bonus. Installed 14.10., untested.
- The Ubermorph stays tracked from one run into the next on a level that keeps running, so a new run brings no second one (three appeared after three Mercenaries runs on Fuel Core). Installed 14.10., untested.
- Alive cap: MaxAlive 15 for one player, +3 per further body (bots count), cap 23 (`[Escalation] MaxAlivePerPlayer=3`, `MaxAliveCap=23`, were 5 and 30). Installed 14.10., untested.
- `[Survival] StalkerNeedsCover=1`: no Stalker on maps without CoverNodes (MP maps, arenas except ch04); its substitutes come instead (they only stood around there). Installed 14.10., untested.
- Mercenaries, Objective and Hold the Zone start 3 s after loading in (were 10/15/15); the countdown does not wait for anybody, so nobody can run ahead. The bots are let in 0.3 s after the level stands when it came from the main menu (1.5 s after a restart, as before). Installed 14.10., untested.
- Supply terminal key hint when the first round of a visit arms (overlay card 7.5 s).
- Respawn notice says the lives left only when the death took a life ("RESPAWNED" otherwise).
- Mercenaries opens the whole map like Hold the Zone (objectives completed in order, the last never starts).
- Hold the Zone MaxRunSeconds default 720 (12 min).
- Objective fixes (user, 16.10., Objective on Marker Lab): (1) after the cleared run and the checkpoint restart the next run played survival: the restart reloads the map without a level release, so objectiveSpent, the objective watch and the vent windows were never cleared; NoteRestart clears them at the ask (the reloaded map announces obj_1_new before the new Isaac stands), a result message of the old map after the ask is dropped at the settle. (2) The step notice said "+30 S" while the map's own +60 s (obj_3_complete) had come 100 ms before: map bonus and step now go into one notice ("OBJECTIVE 3 DONE  +90 S"), a lone map bonus shows after 1.5 s. (3) `StepFloorSeconds=90`: after each objective the clock holds at least 90 s. (4) `ContactBeamPercent=50`: the Contact Beam's hits on objective props count half in Objective and Versus (DamageableProp hook, DamageData+0x94 inflictor bit 0x200; the hit log shows the weapon bits, `objectives contact_beam_hits=`).
- Objective: the objective props (Marker Lab node and markers, Titan bench, Fuel Core fuses and detonator, Concourse glass, Academy breaker box and scoreboards) last 3x as long (`[Objective] PropHealthFactor=3`, every hit divided in the DamageableProp hook, the defend hits too), and hold switches take 50 % longer per real player beyond the first (`HoldPerPlayerPercent=50`: x1.5 with two, x2 with three, x2.5 with four; bots never count). Every machine applies both to itself, so keep the values alike. Installed 14.10., untested.
- Frontend colours: tools\recolor_frontend.py shifts all 62 menu movies to any colour (--hue / --rgb) and also the additive colour of PlaceObject transforms, where the row and description text tints and the hover fills live (--text-white, --highlight). tools\recolor_frontend_scene.py does the 3D scene (--style red | dark). Built 13.10. (build\frontend_final, CRC 7CECBD9A, installed 14.10.): red menu D8443C, white text tints, orange FF8A2A hover, dark scene (dimmed red glow, grey nerve reflection, near white lights, dark wine red nebula) and the title logo. Installed for the test before: the all-red scene (E0A7164D).
- Title screen: tools\build_title_logo.py keeps the shipped DEAD SPACE letters, turns the 2 red and adds a steel CO-OP MOD line (lgo_ds2_02, edited block by block). `[Menu] TitleLine` adds our line after EA's copyright text, which stays. Both built 13.10., installed 14.10..
- Menu names (14.10., installed 14.10., untested): main menu title "/Co-op" (until 17.10. "/Hafer's Sprawl"), "co-op" is "story", "survival" is "gamemodes" (Multiplayer screen title "//gamemodes"; the mode row still says survival), join description capitalised. Save slot and chapter pictures keep their colours (recolor_frontend.py CONTENT_REGIONS, grey tints; build\frontend_final2, CRC DCD651B0).
- Multiplayer screen pages (14.10., installed 14.10., untested): "records" (item 3: best wave, Mercenaries score, zones held, Objective time left per map from DS2SurvivalBest.txt), "how to play" (the EA Account button: every key read from the INI, the modes in one line each) and "about" (the log out button: build id and date, what is new). The same button closes the page.
- Multiplayer screens: the NAT warning lines ("Your NAT Type: Strict" / "See www.support.ea.com ...") show our own text, [Menu] NatLine1 / NatLine2 (built 13.10., installed 14.10.).
- Nav routing repair for templates without nav settings (enemies that stood still).
- Stream teardown: pool prerequisites and text_assets_global records deregistered (dumps 50544, 26848, 40672), whatever their refs; language packs locked while the pool is held past a level (blank menu text).

Co-op
- Client stray guns at 0,0,0 fixed: the requisition is cancelled when the ghost has no placeholder.
- Ghost guns: Ripper and Flamethrower recognised; swapped-out guns trimmed; resident guns handed over after 1.5 s.
- Partner suit re-dressed when a body is rebuilt at the same address; re-ghosting only once per arrival on MP maps.
- Lobby: rejoin of the host's running MP match from the main menu; stale "loading" status cleared.
- Lobby: a host can host again after a match: the next match runs in the session it already has, new joiners are dialled into it and never get the machine number of a partner still connected (built 13.10., installed 14.10.).
- Lifts: Mover and MoverController recognised; lift managers generated offline (generated_ride_triggers.h).
- Story NPCs: a partner's sequence cue waits for its NPC spawn, so removals at its end run.
- Loadout after restart: restart points are held only on MP maps and arenas, not after a debug jump or a campaign join.
- MP maps: a client's restart reloads only its own world; objective completions and clock additions run once per round from whoever fires first; proximity switch messages stay local; ghost prop hits and iMsgDamage are not mirrored twice.
- Respawn: fall rescue from the map origin, respawned body placed directly, fresh body when the old one was deleted, spawn windows reset on arrival, anchor ignores falling partners, arrival curtain lifts when the body stands.
- Late joiner spawns at a point near a partner.
- Client HUD line no longer hidden by its own empty director; client shop follows the host's wave state.
- Key item carry icon only on our own Isaac (a client crashed when the host picked one up).
- Hint cards for refusals and warnings (`[Notice] OverlayHints=1`), in menus too.
- Config check: differences sorted by severity, "N MUST MATCH" notice, survival readiness in the fingerprint.

Bots
- Grab escape from necro players (`[Bot] NecroGrabEscape=1`).
- Bots no longer crawl vent tubes: FollowIntoTubes removed, tube guard refuses a bot's own entry.
- Bots ignore the level's trigger volumes for real: `DS_EntityFilterComponent::CheckEntityPassesFilter` refuses bots for TriggerPhysical (`[Bot] IgnoreTriggers=1`). The old SendMessageTo/touch hooks never saw these triggers (BOT_TRIGGER_AUDIT_2026-10-13.md).
- Bot use press guard (`PlayerUseObjectSM::CheckUseObjectStart`): only the usables the bot module means; bots skip audio and text logs.
- `[Coop] GhostTriggerFilter=0` (experimental): the same filter for partner ghosts, off.
- Bot kit outside the campaign (`[Bot] ModeLoadout=1`, `Progression=1`): Mercenaries and Versus start each bot with its own suit, the Pulse Rifle and one more gun; survival, the arenas, Objective and Hold the Zone hand out a cheaper gun, a suit, an expensive gun and a fourth gun after waves 3/5/7/9 or minutes 2/4/6/8, the bots on different waves. Tiers from the terminal's prices, picks seeded per bot, favourites weigh most. Before, the bots kept the plasma cutter all match (no store break outside survival; survival credits bought the first gun before wave 6). Built 13.10 (build\DS2Coop_bots.asi), installed 14.10..

Versus
- Necro vision on human bodies (`[Versus] NecroVision`, code key, on).
- Necro vision crash fixed: the attribute block now has the engine's 16-byte header (type, reference count pinned high), so a human body rebuilt by a suit change no longer releases our counters as a header (client crash at 0x007CE939 as the Pack spawned, 18:28). Built 13.10 (build\DS2Coop_versus.asi), installed 14.10..
- Lobby screen: a joiner's weapon and suit tabs follow the host's mode from the lobby state; before, its own mode stayed survival until the launch and the tabs were shut. Built 13.10 (build\DS2Coop_versus.asi), installed 14.10..
- Necro points for killing a bot.
- Nameplates over partners' necro players, overlay names for necros.
- Round music through the survival music module; MP loot drop list on clients.
- Necro fall rescue under the whole map.

Templates and configurator
- `[Necro] Enabled=1` in DS2United.ini and a configurator row.
- Configurator warnings name what breaks and the fix; rows for the Mercenaries and Zone limits; survival map validated.

## Changed on 2026-11-02 evening (story co-op test fixes; in build 921B03DB, not installed, untested)

- Black room for the player behind: the partner's ghost counts for the cull triggers here, and they hid the rooms around our own Isaac. In co-op the room under our Isaac and 3 m around him is never hidden, and one hidden before we walked in is shown again outside scenes (`[Streams] KeepOwnRoom=1`).
- QTE on the wrong machine: the host's scene waited for the client's streams, meanwhile his ghost entered the same trigger on the client and the client led the scene. A local scene started by a partner ghost's trigger entry that our Isaac did not make waits up to 20 s for that partner's own copy (`[Mirror] GhostSequenceHoldMs=20000`, 0 = off).
- The follower dies with the leader: a scripted death arriving with the leader's replayed failure is no longer refused. The follower's own keyboard actions are dropped while it plays with the leader's controller, and a follower behind the leader catches up on stick-only states instead of dropping button presses (queue 128).
- Scene chains: a player who followed a scene follows the same leader into the next scene that begins within 8 s (the Tormentor: the client's own copy began 170 ms early and the client led it).
- Save kiosk softlock: the kiosk runs on iMsgPausedTick only, so its PauseLogic now pauses that world again; pause pushes and pops pair up last in, first out.
- Scene NPCs: the host's removal of a NeutralNPC (the two soldiers at the start) reaches the clients, after the client's own sequence ends (`[Mirror] RemovalNpcClasses=NeutralNPC`).
- Log flood: the broken bone index line named only one ghost and logged 37000 lines when two took turns; now once per ghost, 32 at most. That flood had wiped every other event on the client.

## Changed on 2026-11-02 (network core, seed, journal, diagnostics, overlay recovery, start wait, menu fixes; in build 6EC8582B, not installed, untested)

- Menu fixes from the first local test (02.11.): the gamemodes screen's join row no longer shows the story screen's `< human / necromorph >` choice (left/right never reached it there); hint cards stay 5 s instead of 15 (`[Notice] OverlayHintSeconds=5`); `[Difficulty] AllowHardcore` is 0 by default, so hard core is locked in the game modes' difficulty screen and stays a story difficulty.

- First wave after a map restart (players, 24.10.: the countdown reached 0 S in Survival, Mercenaries and the other modes and nothing started; not in the logs at hand): once the start delay is over and the run still cannot start, the HUD says `STARTING: WAITING FOR THE ENEMY POOL` or `... FOR SPAWN POINTS` instead of `0 S`, and the log says why every 10 s (`run N: the start delay is over for N s, the first wave waits: ...`) and how long it waited when it starts. The spawner probe no longer gives up after 15 tries in 30 s: it goes on every 5 s while the run is arming.

- Overlay that vanished for good (reported by players, not in the 24.10. logs: there it drew every frame and F10 was never pressed): three draw faults or three failed font builds anywhere in a session switched it off until the game restarted. Now only three within 10 s pause it, and it draws again 30 s later. When the panel, the status line or the marks should show but no frame is drawn for 3 s, one `shop overlay STALL` line lists every reason counter (end scene calls, device lost, mismatch, render target, state block, font, faults, F10 hide); `shop overlay drawing again after N ms` when it is back.

- Up to 8 players: the host sends every machine its partner table (`[Coop] MeshRoster=1`, packet 'MRST' every 5 s, every 0.5 s for 10 s after a change); a client dials every machine it did not know. Before, a lobby joiner only learned the players of its own batch and TNL hands no body on through the host, so with three or more players some partners stayed invisible. Log: `mesh: machine N: ... direct, ... relayed only, ... with a body connection`, `mesh: the host's table ... names machine`.
- Seed: clients play the host's `[DS2Randomizer] Seed` while in its session (`[Coop] SeedSync=1`): enemy plans and blueprints are rebuilt through the randomizer export `DS2Randomizer_AdoptSeed`; the bench keeps the own seed (its trees are read at start). The seed rides in the lobby launch (lobby v5) and in the host's table; leaving the session goes back to the INI seed. The config check no longer counts the seed as a difference. Log: `mesh: seed adopted`, randomizer `seed: the host's seed ... is used`.
- `[Mirror] Journal=2`: the host's journal and its history replays run in the match modes only. Story partners all load in together at the host's checkpoint restart, so nobody needs the group's history there (audit 16.10. items 5, 7, 14 no longer apply in story).
- Per-partner enemy template check in the scope hook (`TemplateScopeAllowedFor`); a partner heard for the first time gets the suit, the loadout and the host's wave state (survnet kinds 3 and 4) at once. The disconnect line names the partner's last lifecycle phase.
- `[DS2Randomizer] PerfDiag=1`: every 30 s and at every wave change, `perfdiag ...` with frame times (average, 95th percentile, worst), hitches over 50/100/250 ms, private bytes, working set, free address space and its largest block, handles, GDI/USER objects, free texture memory and the survival state; each hitch over 100 ms on its own line; a warning when the largest free block is under 64 MB.

## Changed on 2026-11-02 (story co-op lifecycle, built and compile-checked, not installed, untested)

Design note analysis/COOP_LIFECYCLE_STORY_2026-11-02.md.
- Loading barrier with a host decision (`[Loading] HostBarrier=1`): the host numbers every coordinated load (restart, level change, load from the menu) and sends it in LDSY v3. Every machine holds at the native `OkToStart` until it is ready (loaded, the checkpoint set's rooms in, a joiner's suit on, partners' Isaacs if asked) and starts when the host releases that load. The host releases once every member is ready or left out: silent after being ready (`MemberSilenceMs` 20 s), in the main menu, never reloading (`RestartGraceMs`), past `SyncTimeoutMs` (60 s) or, joiners, `JoinTimeoutMs` (120 s). Lobby joiners count as members before they connect. `HostBarrier=0` keeps the older rule.
- Rooms before the start (`[Loading] WaitForRooms=1`, `RoomsWaitMs=8000`): the rooms check moved under the native loading screen; the black client curtain is off by default (`CurtainUntilRoomsLoaded=0`). A void watch logs a free fall in the first 15 s after the release (`coop_barrier VOID`).
- Story joins only through the host's next checkpoint restart (`[Level] JoinOnHostRestart=1`): a client in the main menu that chose join co-op waits there, the host gets "PLAYER n WANTS TO JOIN - RESTART A CHECKPOINT", and the client loads with the host's restart.
- Joiner born in his suit (`[Loading] SuitWaitMs=8000`): the chapter's start suit for the joined checkpoint, or the client progress file's suit for that host, put on by `Player::Init`'s own dress.
- Restart: before a client's restart its death checkpoint is set to the host's newest one if that one was never applied here (`[Checkpoint] HostCheckpointOnRestart=1`); the host dedupes restart requests per client (two clients' copies used to interleave).
- Lobby packets version 5: the launch and campaign packets carry the host's randomizer seed; a joiner adopts it before the level request (lead's mesh seed sync).
- Log: `coop_barrier ...` lines for every load, member, release and void; counters `coop_barrier role=...`, `coop_barrier join_suits ...`.

## Changed on 2026-11-02 (match mode lifecycle, built and compile-checked, not installed, untested)

Design note analysis/COOP_LIFECYCLE_MODES_2026-11-02.md. New `[Lifecycle]` section, all on, `Enabled=0` restores the old path.
- Native placement (`NativePlacement`): Player::Init's checkpoint move (0x008A84FA) is handed our spawn point through a mid hook at 0x008A8505, so arrival and respawn bodies are placed by the game itself inside the spawn and never pass the map origin; a later Init on our body skips the move. Installed at the first match-mode visit, never in story.
- Born dressed: the arrival and every respawn are born in the suit the rule picks (see the evening fixes below), through uniform's birth suit; story join suits keep priority.
- Restart together (`SyncedRestart`): the director's restart is announced first (LFCY prepare/ack/go/cancel), downed players stay down for the reload, the host restarts when everybody acked or after `RestartAckMs`.
- Late join (`StickyTemplates`, `JoinTimeline`, `JoinResync`): the host's enemy template check keeps its answer per partner while his level stands (before, every respawn or a 3 s stall dropped every enemy ghost); a join timeline per arrival and per link in the log; a joiner without ghosts asks the host for a re-ghost; a client reads the host's wave state from its LIFE packet until survnet's arrives.
- Phases per machine (`lifecycle m<n>: a -> b`) with timeouts that say what is missing; partners' phases in the log.
- Story fix in `mpmap::JoinSuitTick`: the chapter suit by the joined checkpoint GUID (the stream name always picked the last chapter), skipped with `[Loading] HostBarrier=1`.
- Later on 02.11., after the arena_ch01 test of 81C9F06A (patient suit at the checkpoint, kit change and teleport 2 s later, 3.5 s curtain): the placement hook is in from the plugin start (idle until a mode spawn is armed, never in story). A campaign arena arms this machine's start spot (spots of the start set; solo random, co-op by machine number) with the checkpoint's facing and the kit's suit before its level request and again before each arena restart, so the level's own first Isaac is born on the spot and dressed; the kit (guns, stasis, kinesis, start credits) comes in the first tick the level runs. No start curtain, no teleport; the old ones run only when a part did not take (`arena spawn: ... native placement NO (...)`). MP maps spawn the arrival's Isaac under the loading screen while the level waits at WAIT_FOR_COMMAND (`[Lifecycle] EarlySpawn=1`), otherwise in the arrival tick; the arrival curtain only when Init did not place him. Respawns raise no curtain when Init places the body. One line per spawn: `arena spawn:`, `spawn summary:`, `respawn: Player::Init placed`.

## Changed on 2026-11-02 (match mode fixes after the evening tests: restart crash, arena restart spawn, facing, suit rule; compile-checked, not installed, untested)

Design note analysis/COOP_LIFECYCLE_MODES_2026-11-02.md, section 9c.
- Host crash after a pause-menu restart mid-wave (dump 75636, 0x013E2D18 in StateMachine::Update: seven director enemies of the resident pool lived through the restart, their DeadSpaceNPCMainSM table was freed and refilled, state 3 read 0x1C): every checkpoint restart first parks all state machines of the director's living enemies on the watchdog's idle table and deletes them with Base::Delete while their tables still exist (`DS2Survival_BeforeTeardown`, called from the restart bookkeeping; a level release parks and deletes once a level runs again). Enemy machines older than that teardown that still tick are parked unread (the watchdog's rank mark); the mark is only set when enemies of ours were alive and is taken back when no loading screen follows within 10 s. The table probe now checks every state up to the current one (a refilled array reads as text or odd numbers). A host's own restart in a match mode with partners (the pause menu's) goes through the synced restart, so the partners follow. Log: `teardown (...): N enemies of ours still alive, N state machines parked, N deleted now`, `watchdog survivors_parked=`, `watchdog teardowns=... mark_rank=`, `teardown mark N dropped`, `coop_checkpoint local restart held: the partners are told first`.
- Arena restart without the native spawn (`native placement NO (Init never took the arm)` on host and client, then curtain, teleport and kit change): a checkpoint restart makes the new Isaac while the level manager still reads RUNNING, and the restart's arm only took a body made while the level loads. Arms made before a level request now also take the body made while a restart lands (`lifecycle::RestartBodyWindow`), the birth suit as well; the arena waits with the kit until the level runs again after the reload (`arena restart: Isaac X stands before the reload`) instead of judging the body at once. Log: `lifecycle placement: Player::Init puts X on our point ... (a restart is landing)`, every kept checkpoint move with its reason, counters `load_arms_refused=`, `restart_arms_taken=`, `arena native_start ... restart_waits=`.
- Partner in A-pose and big heads on the client after that: the client had pulled a host restart heard on another map 0.3 s after its first arena Isaac was born in [Arena] Suit, while that suit still streamed; the body was deleted mid-stream, the partner's ghost then wore that suit with bone indices 0x8080 (pose not bound, A-pose) and the local Isaac changed into it in the old kit change. A host restart heard on another map is no longer followed (`coop_checkpoint the host's restart N was heard on another map`). Log for the rest: `test birth_suit WARN: X was deleted while its suit ... was still streaming`, `coop_uniform ghost X animates with broken bone indices ...: it wears ...`.
- Facing: Player::Init takes the camera's aim from the body matrix it places (AimingData::UpdateSource at 0x008A8852/0x008A8884), so native spawns face the point's way; arena spots carry no facing and face the middle of the arena's walk, the old arena start and arena respawns by hand too. A move by hand after Init (the old paths) turns the camera with the same two calls (`[Lifecycle] AimFromBody=1`). Facing in every spawn line (`spawn summary`, `arena spawn`, `respawn: Player::Init placed`, `respawn placement`, `aim from body`).
- Suit rule: Survival, Objective and Hold the Zone always respawn in the Engineering Suit, whatever was bought or worn; Mercenaries and Versus keep the character setup's suit. The first spawn of a run follows the same rule ([Arena] Suit where the arena kit applies; the old arena kit no longer keeps a bought suit). The rule is named in every suit line.

## Known issues and open

- Blank main-menu text after leaving a map with the pool held (survival bodies): the level's purge took the language pack (LCH2 table) while the held text_assets_global kept the TextReference that owns it, so the pack was never loaded again (dump 40672: 1 reference, pack handle 8 gone, 0 text tables). The refs > 1 rule of the earlier build did not help and brought crash A back (dump 40672, 0x007767E6). Built 13.10. (build\DS2Coop_text.asi, CRC D988C063), installed 14.10.: the packs are locked for the hold and unlocked after our sweep; every prerequisite copy's records are deregistered at the teardown again; `text_state` lines at the level end, the teardown, LOAD_TEXT_ASSETS and 5 s later. analysis/TEXT_PACK_HOLD_2026-10-13.md.
- 4 GB copy of the game (15.10., works: the user started it, 15.10.): deadspace2.exe is not large-address-aware (2 GB for game, levels, pool and plugins; the pool alone keeps all 41 enemy packages, about 289 MB, resident). tools/make_laa_copy.py writes deadspace2_4gb.exe next to it with only the flag set (1 byte differs, characteristics 0102 -> 0122, the same as Mark's MP exe); the original stays for Steam. DS2_Start_4GB.cmd starts it (warns when the original is newer, after an update); every DS2_Coop_*/Survival launcher prefers the copy when it exists; DS2_enable_crash_dumps.cmd also writes the WER key for deadspace2_4gb.exe (run it once more as administrator); DS2CoopConfig warns about a running game under either name. The copy-protection part accepts the other file name.
- Enemy variant packs (15.10., tools only, nothing in the build): each variant is a folder variants/packs/<Name>/ with variant.json, skin/ (DDS), sounds/ (WAV) and model/ (a character package rebuilt with 4Kodda's Blender toolkit; the skin copy takes meshes, textures and banks from it). tools/variant_pack.py: new (picks package and an authored record per family), fields (every number the record authors), check (fields on the model, files, tags, unstable features), skin export/check/apply, sounds export/setup (private banks, same-length event rename), model check (differences to the retail package), build (dry run on a scratch copy, --write for real). The 16 variants of ds2_variants.json migrated, all disabled; six fields of four of them were on no record of their model and never took effect (kept as not_authored). build_variants.py no longer restores the stale base of 13.09. (it would have dropped boss donors, chapter dependencies and field patches): a variant-free pool refreshes the base first; its dry run works on a copy. Dry run with a test Brute (tint, x1.5 charge damage, model file) built clean. Untested in game; drop_list and skin.ghost stay refused (crash).
- Machines one player at a time (`[Coop] PanelLock=1`) and kinesis holds (`[Kinesis] PartnerHoldLock=1`), built 14.10./15.10., CRC 8975C425, not installed: both players at the train's hacking panel crashed the partner (13.10.). Whoever starts a hacking panel (IUsable +0x280, vtable 0x01B44F68), power node door (+0x230, 0x01B5DB00) or eye poke machine (+0x230, 0x01B772F0) claims its GUID (USEL every 400 ms while he stays within 4 m, at most 180 s); a partner's use start of that machine is refused with "A PARTNER IS USING THIS" while the claim is fresh (1.5 s). The first build of 14.10. looked at +0x230 for the panel, which is another interface: it would never have matched. Kinesis: PlayerFireTKSM::GetTargetPositionFromCast 0x008A3530 answers 0 for a body a partner holds (fresh TKPO pose, phase held), so ExecuteAttack gives up before the grab ("A PARTNER HOLDS THIS"). Log: `machine lock ...`, `coop_tk grab of body ... refused`, counters `machine lock claims_sent=...`, `coop_tk partner_hold_lock grabs_refused=`.
- Nicole hallucination at the ch02 tram station never ended in story co-op (reported 14.10. for 13.10., both players). Offline from the level data: the end chain mm_spawn_mov_tram_car_fire -> spawn_mov_tram_car_fire is heard by four FuncSpawns (the burning tram) and the MultiManager 37CAED6F that sends msg_vfx_tram_dementia_flicker_off/screen_off. On the partner who got the chain relayed, spawn_mov_tram_car_fire was dropped as dangerous (FuncSpawn), while the relayed trigger message already deactivated his own end triggers (TriggerLOS SetInactive, TriggerPhysical Deactivate): stuck for good. His still active start trigger (disable_dementia_triggers never reached him) then re-sent mm_se_tram_station_nicole, which put the first player back into it with his end triggers spent. Fix `[Mirror] SplitDroppedTargets=1` (built 14.10., in CRC 8975C425, not installed): a dropped target is delivered straight to its MultiManager listeners (SendMessageTo), whose targets are filtered again; delayed targets are queued. 223 campaign messages have this listener mix. Log: `dropped target ... delivered to n MultiManager listener(s) only`, `coop_mirror split_targets ...`.
- Debug menu HOST tab (14.10., installed as CRC D781E03F, untested): for the host or solo, a client sees it locked. Director: start the run now, jump to wave N (Q/E, Shift 5; kills the wave, no rewards), end the wave (rewards) / skip the break, spawn a kind next (Q/E through the wave kinds, stand-in when it cannot come), director hold (survival waves: no spawns, start delay, wave timer and break frozen), max alive (0 = INI, NavPower guard stays), restart the run. Kill all enemies (every hostile NPC except bosses and the Ubermorph, SetHealth 0). For every machine over SVNT kind 12: heal everyone (and bots), respawn the downed now (MP maps), supply drop + full stasis, credits, everyone to me (catch-up pull), god mode for everyone. Restart at the checkpoint (partners follow). Wave jump, restart, kill all and checkpoint restart need a second press. A run a director tool shaped saves no best ("NO RECORD" in the tab line). Log: `host tool ...` lines in DS2United.log and the director's `host tool done/refused` lines, counters in the `debug menu` line.
- Brute as a direct spawn (14.10., installed with pool TOC 4AB250DD, untested): the pool always carried two Brute donors (F9FD13D3-6ACD386C from the ch08 tram tunnel brute 58CACF4C, super D8D5110F-1FFD0984 from the ch10 checkpoint brute 5ACAB0FE) with their projectiles, item drop list and dismemberable parts, but `trim_pool_toc.py` had dropped the brute and super_brute packages, so the family counted as unpackaged and every Brute donor was target-disabled (survival: "Brute: none (package trimmed)", waves used the DividerCore stand-in). Both packages are forced resident again (TOC 6144 bytes, CRC 4AB250DD, 45 forced, 0 unresolved links; plugin CRC 8D652010). The Brute shows up in survival waves (`Brute:n:direct`) and through DS2ExtraSpawns.tsv (kind `brute`, `super_brute` or `Brute`); still never as a randomizer replacement (catalog rows are constrained, [Weights] Brute=0). Cost: about 20 MB more forced resident for the whole game. Backups `analysis/runtime_stream/ds2_randomizer_pool.toc(.json).bak-before-brute`.
- Save kiosk froze the player while saves are held (campaign joiner, debug menu jump, MP visit, arena; reported 14.10.): the hold answered SaveManager::ChooseSaveSlot with 0x00A33D10 only (takes back IncNumTimesSaved), so the kiosk never heard the write finish and kept the logic paused (SaveKiosk::BeginSave 0x00BAE040) until damage force-quit it. Installed 14.10. (CRC D781E03F), untested: the hold now also calls the device callback (manager+0x24, vtable 0x01B42B68, slot +0x44 = 0x00BAE3A0 (the 14.10. build compared 0x01B42BA4 and never reported it; fixed 16.10.), result 0) like a finished write: slot menu refreshed, manager back to ready, nothing written. A kiosk save shows "NOT SAVED - ..." (the reason). Log: `save hold: from a save kiosk, write completion reported`, summary `saves_completed=`.
- Story cues once per machine ([Mirror] StoryCuesOnce=1, installed 14.10. (CRC D781E03F), untested): sequence and scene dialogue cues are shared from any machine, but the receiver only folded its own copy for 5 s (DedupeMs) and neutral NPC spawns for 120 s. A player behind the group who walked into a scene the group played earlier sent its cue again, every partner replayed it (NPCs restarted an old scene, T-pose in unloaded rooms) and his own NPCs ran it twice. Now the first copy, local or a partner's, plays and every later copy from another origin is dropped on both sides; a cue one origin fires twice is a repeat and stays shared. Cleared with the world queues (restart, level exit). Log: `coop_mirror story cue ... dropped`, `coop_mirror story_once ...`, `neutral_once ...`.
- Black screen on the way back to the main menu (14.10., after the text fix): the held text_assets_global copy is reused by the menu's LoadTextAssets, nothing loads, and LOAD_TEXT_ASSETS only advances on iMsgStreamLoadComplete with idle queues (0x0042DF0A, 0x00748250), so the level manager sat in state 1 (live, PID 49880). Built and installed 14.10. (CRC F9B60DC0), untested: after a held pool, once state 1 has waited 1.5 s with both queues empty, the level manager's own handler 0x0042DEC0 gets that message (byte check first, at most 3 tries). Log: `text_nudge ...` and the `text_nudge nudges=` line 5 s after LOAD_TEXT_ASSETS. **Failed in game 15.10.:** the nudge ran from the stream update, which the game only calls while it has streaming work, so with idle queues it never ran (log `text_nudge nudges=0 armed=1`; live PID 52736: state 1, queues 0/0).
- Level exit guard (15.10., built, now in CRC 84AA966B with everything since 8975C425, not installed): `[ExitGuard]`, on by default, replaces the text nudge. It runs from the main loop's frame tick (0x007701C0, every frame, menu included) and covers every level manager state that waits for iMsgStreamLoadComplete (1, 2, 5, 7, 8), whatever kept a copy alive: after 1.5 s with both queues idle it hands the handler 0x0042DEC0 that message, at most 5 times per state. Proven live on the stuck game of 15.10.: the message moved state 1 to 2 (the test then crashed in the per-thread allocator at 0x0045244C because it ran from a foreign thread; the guard runs on the game thread). Last resort: a load state that does not move for 60 s with idle queues, or a main loop without a frame for 30 s while the game is in front, writes `deadspace2.stuck.<pid>.dmp` and restarts the game with its own command line (DS2United.restarted.txt tells the next run why). DS2Randomizer.log: `exit_guard lm A -> B after N ms`, `exit_guard nudge ...`, `exit_guard running: ... from randomizer\`, `exit_guard summary`, `exit_guard RESTART`. Checkpoint nudge (16.10.: three debug-menu chapter jumps from a level, CH04 -> CH05 sprawl hub, CH05 -> CH08 Chem Lab and one at 14:01, sat 60 s in LOAD_CHECKPOINT with idle queues and every sampled stream at status 3, each time with the randomizer pool requested during that state, then the guard restarted the game): LOAD_CHECKPOINT (11) leaves on the checkpoint set's loading-completed message ([0x0204D3A0], sent by StreamSet::StreamSetLoadingCompleted, handled at 0x0042E0E5 into CheckpointLoadComplete 0x0042D5D0 without a state check). After `CheckpointNudgeSeconds=8` with idle queues the guard logs what the load waits for (`exit_guard checkpoint wait:` stream status counts, the zone set's unload list, streams not loaded) and sends that message (twice at most, `exit_guard checkpoint nudge`); the restart stays the last resort. The root cause (why the set never reports complete) is open; that wait line is meant to show it.
- Two NPCs in one wall vent crash at 0x00E93CCF (08.10. dumps 22484, 15744; again 16.10. dump 44176, ch2 wall vent 31CAF23B-818D595D: a MoreEnemies repeat 4.03 s after the spawner's last spawn, then the game's own next spawn 938 ms later). A state handler (0x00E93CC0, thiscall(msg), ret 4) sets flag 2 on its sub state machine E8B1C0E8 without a null check. Guard 16.10. (crashguards, mid hook at the or): a missing one gets a scratch block. Log `state machine E8B1C0E8 missing`, diagnostics `vent_state_flags_dropped=`. The NPC may stand in the vent afterwards (vent_queue long_inside shows it).
- Engine message handle table (512 slots) overflows in long survival runs; the Quartermaster then crashed on a gun stream (dump 7320). Built 13.10., installed 14.10.: Quartermaster lookup guard (finds the stream by its handle or leaves the message alone) and a once-a-second census that names the burst ([Coop] CrashGuards). Restart the game between long survival tests until the burst is found.
- Versus: cross-machine necro grabs never observed (coordinator ghost updates 0); Ubermorph crashes as a Versus necro.
- Versus round clock desync (client 390 s early): fix 13.10, needs a round to confirm.
- Internet: enemy template check has a 2 s window; one lost packet can drop ghosts for about one ping.
- The seed is now sent with the lobby launch and the host's table (02.11., untested).
- Story joiners get their guns and items from the client progress file 1 s after the level starts (`[Coop] ClientProgressFirstApplyMs`, 02.11., untested).
- A story joiner's TNL session starts only after his first settle, so the host barrier's ghost wait can add up to 10 s to a join (story agent request 3, not done: starting the session at the barrier's ready is unverified).
- Network hardening (join code, filter in front of TNL, ports only while hosting, damage and report limits, 12 handlers unaudited) is deferred by the user (02.11.); play with friends only.
- Direct launcher path compares no build; the config check only warns and cannot see a failed hook beyond survival readiness.
- Notices, wave rewards and capsules are sent three times without ack; a lost capsule "gone" leaves a stale copy.
- Enemy loot exists on the host only in the campaign.
- Story streams: partner keep list full at 46 entries (host pop-in); relayed managers drop cull targets.
- Bots count as players in a lift cabin; a bot left outside holds the ride up to 60 s.
- `[Coop] GhostsIgnoreTriggers` and `[Streams] GhostsIgnoreCullTriggers` never worked: their hooks read the message source wrong (bot trigger audit 13.10). Partner ghosts still fire trigger volumes unless `GhostTriggerFilter=1`.
- Silent pool enemies: cause open (SoundProbe on in the installed INI).
- Host FPS falls over long survival runs, and from about wave 27 the inventory loses its hologram panel and item icons. `[DS2Randomizer] PerfDiag` (02.11.) logs frame times, hitches, memory, free address space and texture memory per wave to tell whether it is memory.
- Arena wave 2 stall fix not confirmed. (Joiners are launched with the host since `[Arena] Modes=1`.)
- Divider and Ubermorph as campaign donors: softlock or unkillable cases; Stalker and Brute never work as replacements.
- Variants crash (Ghost and Gold Leaper in ItemDropListComponent); the build ships none.
- No world state transfer to clients; a client never stores its own restart point.
- Crash at level exit on mp_solar (dump 29868, 0x00790624, 09.10): not followed up.
- Stale docs: README.md (07.10) still says replacement is on by default; README_COOP.txt names DS2United.ini and F9; the DS2Survival.ini header still says the randomizer must be on.

## Changed on 2026-10-15 (installed 15.10.: DS2United.asi CRC 50EFE8B5, ds2coop_frontend.str 152061B6 = the red build/frontend_final3 with the new FE11 movie recoloured the same way, ds2coop_mpskins.lua 8CA0B987; untested)

- Level exit guard (`[ExitGuard]`): see "Known issues and open" above for the full entry.
- Profile level and weapon levels (`[Progress]` in DS2United.ini, `[ProgressXp]` in DS2Survival.ini, src/coop_progress.h, src/runtime_coop_progress.inc, tests/progress_tests.cpp). User decisions 15.10.: profile level plus a level per gun (Battlefield / Deep Rock style), MP upgrades also in Survival, a start bonus, about 30 h to level 60, Unlocks=0 as the template default.
  - XP on the retail MP scale (deadspace.fandom.com Dead Space 2 Multiplayer: necromorph kill 100, human kill 300, round won 1000, both rounds +2000): the host's (or a solo) director pays per cleared wave 100 x wave (cap wave 40; wave 10 = a won round) plus the wave's kills (100, Brute/Pregnant/Divider 300, swarm babies 25, x2 enhanced), a boss wave +2000, the unfinished wave's kills once at a run end; Mercenaries pays score / 8, Objective 500 per step and 3000 when cleared, Hold the Zone 500 per zone. Estimated pace from the 13.10. logs about 55-65k XP/h, level 60 after roughly 35 h. DS2Coop_SurvivalProgress books it on the host and sends SVNT kind 9; each client books its own (dedup by run, wave and event). Versus pays the match score with the retail points ([VersusPoints] defaults now the wiki's: necro's kill 300, assists 25/150, objective 400, heal and rescue 20, team kill -20, suicide -300/-100) plus 1000 per round won and 2000 for both. A run with a host tool (director `adminUsed`, the co-op HOST tools) or a debug cheat or free delivery pays nothing until the next run. Each booking shows "+N XP (wave 5) - LEVEL 12, N XP TO LEVEL 13" or "LEVEL UP!".
  - Weapon levels: each machine counts its own kills (the local Isaac's hit that zeroes an enemy or a necro player, the gun named by DamageData+0x94), in DS2Profile.ini `[WeaponKills]`. Levels at 25/75/150/250/400/600/850/1150/1500 kills; magazine x1.25 at level 3 and x1.5 at 7, damage x1.10 at 5 and x1.20 at 10 (all INI). Only in Survival, Mercenaries and Versus, never in the story. Clip: Gun::CalcUpgradedValues 0x00DBAFF0 after the original (refill rule of 0x00DBB167 kept; a new tier recalculates the gun in hand). Damage: the health thunk scales the local Isaac's hit for the call and puts it back (explosions reuse the data).
  - Versus: GetMultiplayerUpgrade 0x00A6C140 answers the non-gun rows (stasis, stasis energy, necro melee/ranged/paired/health, pack stasis resistance) by profile level from the roster. Without it the local body was level 0, and stasis never fired once the MP bits were on (row 29 must be != 1.0, 0x00873450). Values: closed beta 1.5 for stasis, 1.10 necro damage, 1.20 necro health, 2.0 energy flags, 1.25 pack resistance (no retail values exist offline; they were Blaze title settings).
  - Weapon select (FE11): unlock levels for the extra rows (Flamethrower 4, Ripper 14, Detonator 22, Contact Beam 35, Hand Cannon 60), a description with weapon level, kills and bonuses on every row, and the two upgrade marks by weapon level (row setup 0x00A48240). Confirmed by the user 15.10.: XP booked, level badge and XP bar show the real profile (level 2, 10000/10500), weapon level text shows. Scrolling to the 11th row did not work: FE11 HandleInput gives up/down to the helper at screen+0x1F8, which stays inside the 10 visible rows; FE12 skips it for codes 8/9. Fixed in CRC F0AF78B8 (not installed): the same skip for FE11.
  - Suit select (FE12): the 23 campaign suits unlock between level 0 and 58 (SUIT_LEVELS in tools/build_loadout_frontend.py).
  - Start bonus: 50 credits per profile level on a Survival map or arena arrival (not Mercenaries, not Versus).
  - Hand Cannon (the Foam Finger, "Finger Gun Plasma Cutter" 38CB6750-72BC9767, no ammo use): profile level 60. Survival shop row `PriceHandCannon=25000`; FE11 row 11 (plain-text name, icon inv_fingergun) for Mercenaries; a pick in Versus gives the plasma cutter.
  - `[Loadout] Unlocks=1` stays the test switch: level 60 and weapon level 10 everywhere.
- Ripper icon in the weapon select: the real inventory icon inv_yoyo (the Ripper's prototype name) instead of the cropped pickup picture (tools/build_loadout_frontend.py).
- MP suits (`[Loadout] ApplySuit=1`, src/runtime_coop_mpsuit.inc, analysis/MP_SUITS_APPLY_2026-10-15.md): every MP row of the suit select wore the campaign Security suit as a stand-in. Now the MP security suit 45CAC626 is registered by hand per visit (RegisterUniform 0x00ED5DD0; its record passes the creator gate and its run-time entity the spawn filter), the row's base skin stream (polivedrab, urbancamo1, desertcamo, red, black) is loaded before the dress, and our slot 0x01B29484 (GetMaterialSwapList) adds the swap pairs and the tint the Blaze-only SetupSkinOnPlayer would. Partners' skins come from the roster, now also sent in Mercenaries. A ghost is never dressed in an unregistered suit (it would be stripped bare); bots skip the MP suit. Every failure path falls back to the stand-in, logged as `mpsuit ...`.

## Changed on 2026-10-15, later (built, not installed)

- Weapon select scroll (CRC F0AF78B8 and later): up/down reach the 11th row (see the progress entry above).
- Shop and gadget locks by profile level (user, 15.10.): `[Unlocks]` in DS2United.ini holds one line per weapon, gadget and suit (defaults: Flamethrower 4, Line Gun 7, Force Gun 11, Ripper 14, Seeker 17, Detonator 22, Javelin 28, Contact Beam 35, Hand Cannon 60; turret 8, stasis mine 15, heal field 25; the 16 MP suits at their stock levels; the 23 campaign suits 0 to 58). The same values drive the weapon select (native rows and ours), the suit select (written into PlayerSkinDB +0x1C at every frontend feed) and the survival shop (gun and suit rows show "LEVEL n", gadget refills "LOCKED"). The gadget keys refuse below their level with "TURRET UNLOCKS AT LEVEL 8" (not in the story). Bots keep buying everything (user). Read again at every menu or shop visit. `[Loadout] Unlocks=1` unlocks all.
- Real restart at the end of a run (user, 15.10.): on an MP map the Survival game over (`[Survival] RestartOnGameOver=1`), the Mercenaries and Hold the Zone results (`RestartAfterResult=1` in their sections, as Objective already had) and the HOST tab's "Restart the run" reload the map from its checkpoint (DS2Coop_RestartCheckpoint, GameMainLoop::CheckpointRestart; the partners follow through the checkpoint module) instead of re-arming on the used map. The next run arms on the reloaded map 2 s after the co-op plugin has put the new Isaac down there (`DS2Coop_ArrivalsReady`, at most 30 s after the ask; log `restart: our Isaac stands on the reloaded map N ms after the ask`). Refused or not on an MP map (campaign arenas): the old soft re-arm. Log: `<why>: checkpoint restart asked, the map reloads`.
- Spawn guard (user, 15.10.: after a real restart Isaac stood at the map origin): the game's setup of the new Isaac moved him to (0.1 -2.0 -0.2) about 1.5 s after our placement, on a floor there, so the fall rescue did not see it. For 10 s after an arrival spawn a jump of more than 8 m to within 2 m of the origin puts him back on his spawn point (at most 3 times). Log `spawn guard: ...`, diagnostics `mpmap spawn guard moves= quiet= faults= | ready arrivals=`.
- Weapon select mouse wheel (user, 15.10.: the arrow keys scrolled, the wheel did not): FE11 has no PC list helper (only FE12's Init makes one), and the base menu mouse handler 0x00A6AFC0 gives the wheel only to that helper. A wheel notch on FE11 is now sent to FE11 HandleInput as up or down. The old "skip the helper for up/down" hook did nothing (FE11 never had one) and only counts now. Diagnostics `FE11 up/down= wheel notches= wheel skipped=`.
- Trailer camera (user, 16.10.; analysis/DS2_TRAILER_CAMERA_2026-10-16.md, src/runtime_coop_camtools.inc, coop_camtools.h with tests): `[CameraTools] Enabled=1`. RWS::MainLoop::Render::Poll 0x00770C00 is wrapped: only for the render, blender 0 of the CameraManager holds the trailer pose and SetRenderCamera 0x00793470 gets it, then both get the gameplay camera back, so mouse aim, laser and movement stay on the gameplay camera while occlusion and culling follow the trailer view. The pad flies it (left stick move, right stick look, triggers down/up, LB/RB zoom, D-pad roll and speed, B reset) while the game sees an idle pad (focusrate::GatePad); keys F6 free camera, F5 slow motion (SlowFactor), F4 time almost stopped (0.01), F2 record a path, F3 play the last one, F10 HUD; pad: both sticks, A, Back, X, Y. Paths go to plugins\CameraPaths\ (path_<time>.txt and last.txt). Slow motion writes the game speed float 0x01EC51F0, never below 0.01. While flying the 2D overlay's draws (0x0077073B, 0x00770756; its list clear at 0x0077075C keeps running), the hologram UI layer (0x00770A38) and our status line and marks are hidden. Fix 15.10. after the first test: skipping the whole 2D render call (0x00770DF7) left its display list uncleared and the game crashed in an Apt sequence at the wave notice (dump 29468); the pad is searched on XInput users 0..3 (user 0 never answered). Flying and paths run on the real clock. Log `camtools: ...` once a second with trailer vs gameplay pose. Not done: the audio listener stays at the gameplay camera; Y-axis billboards face it too. Fix 16.10. (black picture, then a crash 13:56 in AptNewSequence::Run, dumps C:\DS2CrashDumps\*.26544.dmp): 0x007706F0 is the display list renderer of both passes, Render::Poll calls it for the 3D world (0x00770DA8, argument 0) and for the 2D overlay (0x00770DF7, argument 1), so the two NOPs blanked the world and left Apt sequences unrun. No engine draw is patched any more; the free camera and F10 hide only our own overlay, the game's HUD stays visible. The camera flies only with an XInput pad (the log of that run: no pad on users 0..3, error 1167).
- Arenas in every mode (user, 16.10., the Stalker room first; analysis/ARENA_ALL_MODES_2026-10-16.md): `[Arena] Modes=1`.
  - Points: tools/build_arena_points.py spreads zone, capsule and respawn points over each recorded walk (floor samples, 9 m apart; zone=, capsule=, respawn= lines in arenas/*.txt override) into src/generated_arena_points.h. ch04: 7 zones (the landing 3.5 m), 6 capsule points, 7 respawn points.
  - Modes: an arena plays Mercenaries or Hold the Zone when picked, Objective and Versus as survival. The picker shows "[mode]" on arena rows and steps only through the modes the arena has; the map row keeps the picked mode (user, 16.10.: Versus picked on an MP map, then the map row reached every arena): it skips the arenas that cannot play it (gamemode::StepMap on both the native row and the one-row picker, `lock_skips` in the gamemode line), "any map" picks only maps that can, and an arena launch writes the mode it really plays, so lobby, loadout and joiners follow it; DS2Coop_GameMode, the launch check, the director, shop, bot kits and the character setup's picks follow. The director takes the built-in points when DS2Capsules.txt / DS2Zones.txt have none for the arena (export DS2Coop_ArenaPoints); capsules must land inside the arena. The spot keys record under the arena's name.
  - Respawn: a death respawns at the arena point nearest to the team (the MP respawn path, placed inside RespawnPlayer's hook) instead of the level reload; lives refill per arena start. A respawned body gets no second kit or start bonus; the start bonus comes once per arena start.
  - Wave music, escalation and the item/corpse clean-up run in arenas; the real restart and the game over reload take the arena checkpoint (DS2Coop_ArrivalsReady counts the arena's new Isaac).
  - Co-op: the host announces the arena (launch map byte 0x80 | arena index), the joiner runs the same arena start; a client's restart stays on its machine, level follow is skipped while an arena runs, the host lobby stays open for late joiners.
  - Logs: `arena ... modes`, `arena respawn point`, `run N on campaign arena`, `capsule spots: N built in`, `zone spots: N built in`, `lobby: the host launched arena`.
  - Open: a partner rejoining from the main menu still loads the level as a plain campaign; Objective and Versus need own arena data.
  - Church crypt (user, 16.10.; recording ds_2_20261009_214553 -> arenas/arena_ch03.txt): chapter 3 (ch03_chr), a row of crypt rooms, 9 stream sets, 55 rooms, from the entry next to spots 1-4 down to the lift room. Only the entrance 3938D073 is locked, every other door stays usable (arenas/doors/arena_ch03.txt; the far door 39252F74, locked by script, is unlocked). The lift (Mover 3926B250-8B976F99) stays, its switch 3926B250-E8A51117 is not created (`block=` line). A second door in the double doorway at z -375 (341078E1) only a story spawner creates; build_arenas.py now leaves such doors out of the door table.
  - Heights in the arenas (user, 16.10.), arena file `heights=`: any (default) lets the director spawn at any height in the
    arena's rooms (FloorBand 30 m, no other-floor preference, the campaign's own vents at any height); same (Ishimura) only on a
    floor a player stands on (2.5 m; a vent by the floor below it, players on two floors get enemies on both); notabove
    (Sprawl hub) nothing more than 2.5 m above the anchor player (the ledges the campaign's packs jump down from; its three
    vents sit at floor level). MP maps keep FloorBand and VentMaxAbove.
  - The picker shows an arena by its title alone ("church crypt", no "arena: "), and so does the arrival notice. arena_ch10 is titled "security checkpoint" now (the Government Sector's checkpoints 2 and 3; was "ch10 checkpoint").
  - Pack on foot in survival again (user, 16.10.: "why is the Pack off?"): the pack boy stand-in 30CA2DAB-7FED4260 had been
    withdrawn after three friend logs (18.-22.09.) showed pack boys without state machines. It is a plain copy of an authored
    pack boy; of 154 probed it had 2-4 state machines 145 times, and every miss came from one scripted skybox spawner in chapter 2
    (31CB2AAA, a background actor). The campaign keeps it withdrawn; the survival director takes it (withdrawnForCampaign).
    Log: availability PackEnemy AVAILABLE direct=1/1. Research: session scratchpad pack_template/PACK_TEMPLATE.md.
  - Vents in the arenas (user, 16.10.): until now the survival director knew vents only on the MP maps (every arena logged
    "0 live vents"). tools/build_survival_vents.py now writes arena rows like an MP map's: the campaign's vent spawners whose
    target arrives through a VentMarker in the arena's rooms, and every marker in the walk's box within 9 m of the walk (open
    from the start); the arrival goes through any open marker of the same kind (VentAnyMarker). A vent behind a door that stays
    locked is not used (coop_arena::BehindLockedDoor). Rows per arena: crypt 0, church 16, stalker room 2, Sprawl hub 1,
    Ishimura 14, mining hall 19, ch10 33, marker floor 4, tripod room 2 (borrowed).
  - Tripod room vents only (`spawns=vents`, user): the room has 7 wall vents but no vent spawner of its own. All chapter 1 vent
    spawners sit in the always loaded main stream; their templates live in NPC streams. `npc_stream=npcs_surg_ward` loads that
    stream with the arena (its neutral NPC and corpse props are not created), so the surgery ward's two wall vent spawners drive
    the room's vents. With spawns=vents a family needs a vent donor there, the director never falls back to the direct path,
    and families without one are substituted (Swarmer, Stalker, Brute, Pregnant, Divider never come through vents; donors on
    the room's wall contract: slashers, leapers, pack, exploders, infector, lurkers).
  - World triggers stay (user, 16.10.: "the lifts on the Ishimura must work"): the story-logic block also took the triggers
    that switch lights and shadows (lt_*, *_shad_on/off), culling (cull_set_*), a lift's switch (can_use_switch: the switch
    works only with the player in the car), the chase camera and audio. A trigger whose every message is such plumbing is
    created now (41 crypt, 37 church, 19 Ishimura, ...); culling stays blocked with doors=open. Story triggers (ambushes,
    spawns, checkpoints, stream set switches, combat boxes) stay blocked.
  - Arena file options `allow=<GUID>` (an entity the story rules would block stays) and `skip=<from>-<to>` (seconds of the
    recording that are no arena). Mining hall: the lift switch 5A68DC42-4FFE350A is also the target of the story spawner
    that replaces it after the malfunction, so the filter refused the original; allowed. Its recording had a checkpoint
    load behind the locked door 5AE5BB72 (t=129-177), skipped: enemies spawned there.
  - No spawns behind a locked door: the door table now carries each door's crossing axis (its forward row; every door a
    walk went through crossed along row 8) and the side the arena is on; within 6 m of a door that stays locked, a point
    on the other side is not inside (coop_arena::BehindLockedDoor, diagnostics `inside_no= (behind a locked door N)`).
    ch10: the original checkpoint's room behind door 5AC7CAE4 was outside already (5.2 m and more from the walk).
  - Survival never started in the second arena of a session (16.10. night, church sanctuary and Sprawl hub): the enemy pool's
    verdict stayed "dispatched, unverified" (church: the brute model stream came 4 s after the last check) or "loading" (Sprawl hub).
    RecheckStreamGraph, the per-tick recheck meant for exactly that, asked GlobalStreamLifetime::usable(), which holds only once the
    verdict is "ready", so it never ran; with every arena room loaded the game had no streaming work and sent no update either. It
    now checks the manager and the handle, and also runs while the pool's root is loading. Log `stream_graph_recheck hits=`.
  - Stores (ShipStore) and workbenches (UpgradeBench) within 15 m of an arena walk are not created either (user, 16.10.: no store
    and no bench in survival; the store has no lock of its own). Sprawl hub: door 37C1513E-BD1FF60A locked by the user's wish (arenas/doors/arena_ch05.txt).
  - Spawn rules (user, 16.10.): the recorded spots (End key) are the players' spawn points, nothing else. Arrival and checkpoint
    restart: solo a random spot; in co-op the free spot nearest to a partner who already stands on a spot (the host never waits, a joiner
    whose game loaded first waits up to 4 s for one), else random; with more players than spots one is shared. Respawn: the free spot
    nearest to where he died (a fall: the team's place), not the generated respawn points.
    Fixed the same night (user: "the streaming is broken, it does not even load the right areas"): the random start put the Isaac on the
    crypt's lift room spot 1 s after arrival while only the entrance sets were loaded, he fell through, and the respawn then picked random
    spots in unloaded rooms again. Now a start spot's stream set is wanted first (streaming=sets: PickSets' forced set) and the Isaac goes
    there only once every room of that set that has a TOC is loaded (20 s at most, then the nearest loaded spot); a respawn takes only
    spots whose rooms are loaded, toward the place of death, else where he last stood inside the arena. Log `goes to spot N ... load first`,
    `rooms loaded after N ms`, `N spot(s) with loaded rooms`. Log `arena ... start: ... onto spot N of M, ...`
    and `arena respawn: spot N of M`, diagnostics `start_placements= (random, beside a partner, waits) respawn_spots=`.
  - Recordings of 15./16.10. sorted by me at the user's request ("work out what is what; drop the short ones and those with no rooms or no
    spots"): arena_ch03b church sanctuary (ds_2_20261015_232648), arena_ch05 sprawl hub (ds_2_20261015_234751, cut at t=144.8 where the
    recording ran on through later checkpoint loads into other chapters), arena_ch09 mining entry hall (ds_2_20261016_002428), arena_ch11 marker
    floor (ds_2_20261016_003147). Dropped: ds_2_20261015_230723 and _230733 (ch01, no rooms saved), ds_2_20261016_001925 (no spots, its one
    door mark is in _002428 too), _002421 (empty), _003630 (ch11 end area, no rooms saved). Door rules as marked in the game. Start spots are
    now spot 1 and the spots within 10 m of it in recorded order (the church's spots lay 12 m from where its recording began).
  - Ishimura (user, 16.10.; arenas/arena_ch08.txt, label "ishimura"): two recordings combined at the user's request. ds_2_20261016_001044
    gives the start, its checkpoint 58CAF76A and the 8 spots (1 and 2 are the start spots); ds_2_20261016_000313 gives the rooms and the door
    marks (12 held open, 368E67D5-C22EEB24 locked). 15 sets, 34 rooms, 19 doors (door table limit 16 -> 32, build_arenas.py MAX_DOORS). Spot 3
    lies 4.7 m from the rooms' walk, the others closer.
  - Crypt test 16.10. (user), fixed the same evening:
    - Save stations: every SaveKiosk within 15 m of an arena walk is not created (ch03, ch04 and ch10 each had one).
    - The start: the level loads at the arena checkpoint (ch03: in front of the save station, 4 m behind the recorded spots). Every new
      Isaac of the arena (arrival, checkpoint restart; not a respawned body) is put on his start spot, the recorded spots within 8 m of
      where the recording began, one per machine (host spot 1, joiners the next), facing as the checkpoint had him. Log `arena ... start:`
      and `arena start check:` 3 s later, diagnostics `start_placements= start_place_failures=`.
    - The hallucination's fake pieces stood in the 2nd/3rd room: the level loads `crypt_south_storage_02_horror` hidden for the story
      event and the arena showed every member. build_arenas.py now leaves streams named `_horror` to the game (not arena rooms).
    - Rooms loaded and unloaded in view: at a set border the wanted set swapped every 2 s. Now a player keeps his own set plus every set
      the walk had within 12 m, nearest first while at most 40 rooms fit the switch's list, and a set stays 10 s after the players left.
    - The lift room was dark: crypt_z01's own TriggerAutos switch its lights off when it loads, and only door 39252F74's open-start
      message (its script sends lt_crypt_on) turns them on. The door tick sends that message only when a door opens after loading
      (0x00DD264C); a held door opened from closed skipped it. Held doors now send their open-start messages (StandardDoor::SendDoorMsg
      0x00DADEF0, types 7 and 8) when opened and again 2 s after arena rooms loaded, for held doors within 40 m. Side effect: the crypt
      doors' cooling sounds and one roll_object play again then. Log `open-start messages`, `rooms loaded, open-start messages again`
      (with the lt_crypt_on listener count), diagnostics `door_messages= door_resends=`.
    - Item pickup holograms were red: our `[Menu] RedInGameMenus` recolour also took `pickup_ui_*` effects and `ui_pickup*`
      textures. tools/recolor_global_assets.py now leaves them stock (134 pickup assets stock again); the red menus stay.
  - Doors open and no Brutes in the crypt (user, 16.10.): arena file options `doors=open` (every door that is not locked is held open: opened straight into
    "opening" past the co-op door wait, its wish to close or to shut is refused in Door::TransitionToState while the arena runs; log `held open`,
    diagnostics `door_holds= door_closes_refused=`) and `exclude=Brute` (the survival director treats the family as unavailable there and plays its
    substitutes; availability line "the arena leaves it out"). Held doors used to be reopened only after they had closed, through TryToActivateDoor,
    which also switched to the door's open set; they are opened directly now. The crypt entrance 3938D073 stays locked.
  - Streaming by stream sets (`streaming=sets` in an arena file, user, 16.10.): the level already streams in sets that hold a room and its neighbours. build_arenas.py writes each set's members and the set active at every walk sample; the runtime keeps only the sets the local Isaac and the partners stand in (nearest walk sample), activates the local set again when they change (also where a blocked script would have switched), lets the game unload the rest, and the director spawns only inside those sets (DS2Coop_ArenaInside). Arena member limit 46 -> 96. Log `arena ... streaming: sets ... wanted`, diagnostics `sets_wanted= set_activations=`.
  - Door marks (user, 16.10.): the recorder's `[Arena] DoorKey` (Delete) steps the door nearest to the player (8 m, bot_sense's entity list) through auto, open (never locked), forced locked and held open (closed: TryToActivateDoor 0x00DDA380 again once a second). In a running arena the mark works at once and goes to `plugins\DS2Arenas\doors\<arena>.txt`; during a recording into the recording. tools/build_arena_points.py reads both (arenas/doors/<arena>.txt in the repo) into generated_arena_points.h; a mark wins over the door table, marked doors outside the table are handled too. Logs `arenarec: door ...`, `arena door ... (locked|inner|held, ...)`, diagnostics `door_unlocks= door_holds= door_marks_live=`. Saved marks count again (user, 16.10. evening: "live door changes should be kept for the arena"): each arena start reads its `plugins\DS2Arenas\doors\<arena>.txt` (last line per door wins, coop_arena::ParseSavedMarks, tests), so a mark from an earlier run works without a rebuild; log `arena ...: N saved door mark(s) from ...` and one line per mark. tools/sync_door_marks.py copies the game's marks into arenas/doors (16.10.: one ch09 and four ch10 marks had never reached the repo; now in generated_arena_points.h).
  - Host LAUNCH (user, 16.10.: a LAN co-op lobby should start when the host says so; research
    analysis in the session scratchpad LOBBY_LAUNCH_BUTTON.md): MP02's help bar (vtable 0x01B37528 slot 6, 0x00A49DF0) is ours while
    armed: quit for everybody, LAUNCH (`'lnch'`, stock label 0x68686F1E, as Mark's RIG-LINK) for the host. A click or select (A, Enter)
    starts the match `LaunchSeconds` (3) later; until then the countdown is -1, the screen's own "waiting for players...".
    `AutoStartSeconds` (0) above 0 brings back a countdown once `MinPlayers` are in (3..5999 s); `CountdownSeconds` is gone. A click on a
    player row injects select too and no longer starts the match (300 ms guard). Log `MP02 help bar built`, `the host launched (...)`,
    `native lobby launch:` with the input codes seen.
  - Lobby screen (user, 16.10.: "for arenas, not story"): with `[NativeLobby] Enabled=1` a co-op host who picks an arena row goes to the game's lobby screen as for an MP map; the state packet carries the arena code, the joiners open the same screen, the countdown launches the arena on every machine. MP02 shows the arena's list entry ("arena: ..."); its picture may be wrong or empty (MP02 SetInfo only bounds the index against the list). The weapon and suit tabs follow the mode the arena plays. Story co-op keeps its own route.
- Effect colours (user, 16.10.): `[Visuals] StasisColor`, `KinesisColor` (`KinesisThrowTrail=1` also the trail of thrown objects, shared with enemy blades), `LaserColor` (aiming laser of every gun, the red empty-clip laser stays), `WeaponColor` plus `PlasmaCutterColor` ... `RivetGunColor` (muzzle, shots, beams, impacts), `BloodColor` (sprays, drips, gore, decals; acid and bile stay). Same choices as the RIG. Every effect passes TFXSequencer::PatchInit 0x006DA2C0 once when its stream loads; before it runs, the listed effects (458, tools/build_fx_colors.py from fxcolors/fx_color_plugin_tables.json) get each chromatic colour turned about the grey axis onto the chosen hue. The 33 red blood textures go through the RIG texture hook (DXT1 and DXT5). Offline: all 458 effects parse in the game data, no version or table problem. Logs `fx colours loads= ...` and `| blood candidates= ...`. Details: analysis/DS2_FX_COLORS_2026-10-15.md.
- Status line colour (user, 15.10.): `[Hud] Color` (stock, red, orange, yellow, lime, green, blue, purple, pink, white, gray or RRGGBB) colours the survival status line, `FlashColor` the event line under it (empty: follows Color). Also on the configurator's Survival page. Log `hud colours: ...`.
- RIG and visor colours (user, 15.10., after the TexMod colour packs in "Dead Space 2 Texture Mods"): `[Visuals] RigColor` and `VisorColor` take stock, red, orange, yellow, lime, green, blue, purple, pink, white or gray. The textures those packs replace are found by TexMod's own hash (CRC32 of the top level without the final inversion) when TX_ReplaceTextureContents 0x013C9030 uploads them, and their DXT1 endpoints take the chosen hue on the way, for any suit: RIG healthbargradient_c; visor player*helmetscreen_c and the lens flares lf01, lf_burst_01, lf_glow_01, which lamps share (as in the pack). Read at the start; the configurator offers both on its Co-op page. Diagnostics `rig/visor colours uploads= candidates= faults= recoloured: ...`.
- Vent guard (a friend's report, 15.10.: stasis on an enemy climbing out of a vent jammed it with MoreEnemies, softlock): a vent's queue (VentMarker +0x374, 8 places) only spaces out the starts, and a ninth NPC waits inside for good. A MoreEnemies vent repeat now waits until no enemy from that vent is still inside and its queue is empty (dropped after 90 s), and a dead or vanished enemy left first in a queue is taken out. `[DS2Randomizer] VentQueueGuard=1`; log `vent_queue ...`. Details: analysis/VENT_QUEUE_STASIS_2026-10-15.md.
- Red in-game menus (user, 15.10.): the pause menu (sp10pausemenu.apx), the settings screens, popups and the button libraries (sharedlibpc, libshared, libsingleplayer) live in global_assets.str, not frontend.str. tools/recolor_global_assets.py recolours its 23 movies with the frontend's values (D8443C, white text, highlight FF8A2A); only colour bytes change, so global_assets.toc stays valid. The plugin reads randomizer\ds2coop_global_assets.str through the frontend's file open hook (`[Menu] RedInGameMenus=1`, the file must be there). Log `in-game menus: ...`.
- Arena start curtain (user, 16.10.: in an arena you first stood at its checkpoint for a few seconds, then jumped to the spot and changed suits): from the new Isaac's first frame the overlay's black LOADING curtain is up until he stands on his start spot (+1.5 s for the kit suit to stream in), at most 32 s; a respawned body gets none. [Arena] StartCurtain=1. Log: arena start curtain lifted after N ms: <why>.
- MP suits in the arenas (user, 16.10.; analysis/MPSUIT_ARENA_2026-10-16.md): `[Loadout] ArenaSuit=1`. In a campaign arena playing Mercenaries mpsuit asks the necro module for the pool root alone (necro::DemandForSuit: global_mp_assets, about 77 MB, no character streams, no MP UI, no respawn manager; needs [Necro] Enabled=1 and LoadPool not 0; its creator gate keeps the MP autospawns out), registers the MP Uniform 45CAC626 once the pool is in (checked live per level), and dresses the pick once the skin stream is in; the kit's suit stays on until then (30 s, then the stand-in). A respawn dresses the pick again (mpmap::GiveLoadoutSuit). The lobby's picks are stored in DS2Profile.ini now even when the hub pointer is set (they were lost before). A pending dress is no longer dropped when DressNow could not dress yet. Risks: 77 MB more per arena start; the September level-exit crashes with this pool in campaign levels were never confirmed fixed. Logs: `necro pool asked for the MP suit`, `pool for the MP suit in after N ms`, `mpsuit arena: MP suit wanted`, `dress deferred (arena)`, `dress ... after the pool and the skin stream`, `respawn: the character setup's suit again`.
- Show all enemies (user, 16.10.: a debug overlay for every enemy): debug menu, CHEATS tab, "Show all enemies" (toggle; `[Marks] AllEnemies=1` starts it on). Five times a second the SimManager's objects are scanned (as Kill all enemies does), every kind bot_sense knows, bosses included, alive; the nearest 60 get a small diamond with kind, health and distance, on the screen only (overlay kMarkDebugEnemy, 80 marks now). Log: `marks ... all enemies on scans= most_alive= shown_now=`.
- Vent redirect in the arenas (user, 16.10.: no enemies in the tripod room): the chosen vent was written into the spawner's header only when the spawner's target was unknown to the catalog (the MP spawners); a campaign arena's spawner, known to the catalog, kept its own vent, so the tripod room's borrowed surgery ward spawners sent every enemy out of their own vents 20 to 50 m away ("header=NOT WRITTEN", "somewhere else"). OnLookup now also redirects when the catalog's record has the forced source's GUID. The room has one more VentMarker, a wall vent 17 m up (35CAB654-E1DD14E0), that no campaign spawn uses; it stays out for now.
- Character setup labels (user, 16.10.): "character setup", "weapon select" and "suit select" say "mercs & versus" (text overrides, LCH2 ids 0C9541E1/A491E93A, 9EEAD300, D2ECD356); the picks count only in those modes.
- Map pictures for the arenas (user, 16.10.; analysis/ARENA_MAP_PICTURES_2026-10-16.md): the picker (MP20) and the lobby (MP02) pick their picture with gotoAndStop("map" + mMapIndex), the list index, so the arenas past index 6 had none. tools/build_map_pictures.py (from the installed final5, CRC 87FEF2BD; own RefPack and DXT1 code) adds one 512x256 DXT1c page, shape and frame "map<arena name>" per arena to both movies from arenas/pictures/<arena>.jpg|png (centre crop 2:1; a missing picture gets a card with the label) and MP02 the Outbreak frames map5/map6: build/frontend_final6, CRC 10A5838B, MP20 686,320 -> 1,279,824 bytes, MP02 341,680 -> 1,067,200; review sheets in build/map_pictures_review. The plugin (runtime_coop_mappictures.inc, `[Menu] MapPictures=1`) points the movie variable mMapIndex (AptSubscriberVariable entry, type 6 = string) at the arena name, "5"/"6" for Concourse/Academy, else the index as text, before MP20's SetInfo (game mode hook) and MP02's SetInfo 0x00A49CF0 (own hook); the frontend size check accepts MP20 from 686,320 up to 4x. Log `mappictures ...` (binds, frames found in the loaded movie, key per row). The lobby shows only rows 74..209, so the bottom of Concourse's lettering is cut there.
- Match end with the host (user, 16.10.: after leaving his own match "join co-op" said "restart the game first", the item still showed hosting, nobody was thrown out). runtime_coop_matchend.inc, `[Coop] EndWithHost=1`: the host back in the main menu sends level packets with kFlagEnded for 3 s; a partner that hears it (or whose host, machine 0, timed out in the disconnect module) gets "THE HOST ENDED THE MATCH" and goes to the main menu the way the pause menu's quit does. In the menu every machine tears its session down the way GameManager::DestroyNetSession does (UseScopeQuery 0, DisableGhosting, scalar deleting destructor 0x0082B610 with flag 1; signatures checked at install): live connections get the engine's "Shutdown" disconnect, ghosts and socket go, the singleton is cleared, our session caches are reset, and the next StartSession builds a new session on the same port. Guards: main menu without a level, every connection in state 7, no listeners on the session, not inside the engine's network code (the self-drive getter's caller), plugin not faulted. A guard that refuses for 10 s, or host/join picked meanwhile, drops the connections instead (TNL disconnect 0x0141B980, removePendingConnection 0x0141B1C0) and keeps the idle session, which then accepts a new role. The lobby's own "host left the map" now closes with byes and resets the join item. Evidence: analysis/COOP_SESSION_TEARDOWN_2026-10-16.md. Logs: `matchend: ...`, `matchend teardown: session ... iface ...`, the `matchend phase=` diagnostics line.
- Red MP screens (user, 16.10.: the online pause menu with its timer and objective list stayed blue): they live in global_mp_assets\global_mp_assets.str (DS2DAT9, 39 movies: mp10pausemenuhuman, scoring, spectator, vent pick, necro alerts, and the stream's own blue copies of libshared and sharedlibpc). recolor_global_assets.py has a third stream `mp` (-> ds2coop_global_mp_assets.str, 51.5 MB from 43.6 MB; global and loading outputs unchanged, same CRCs as installed); the MP health and stasis gauges (ui_mp_wep_health/stasis/h/s) stay cyan, the necro control pages keep their art. The plugin redirects exactly `global_mp_assets\global_mp_assets.str` under `[Menu] RedInGameMenus` (file check: at least 32 MB, 128 KiB blocks, stream header) and logs every other open naming global_mp_assets (first 8), so one run confirms the path. Log `in-game menus: global_mp_assets opens=...`. The header and timer turn pale pink like the campaign pause menu; the objective states all near white.
- Red RIG holograms and loading screens (user, 15.10. evening): the same tool now also shifts the RIG holograms in global_assets.str (inventory, store, bench, text log and notice holograms, the weapon select tabs, locator line; the item pickup holograms are red again with their item pictures stock blue since 16.10. evening (user: "the item picture stays blue"; build/global_red_pickup2: global 9A30323F, mp 8CCB4EBD, loading unchanged 4F7B0AE0; the mp_inv_* pictures of the MP ammo card stay too; not installed), and the ammo counter comes from each weapon's stream and was never red): 547 effects' colour keys and 76 chrome textures with recolor_frontend_scene's red shift; item pictures, storeicon_* and the suit's health and stasis gauges (player_*_ui) stay, flares (clf., 38 records) are not handled. The holo textures are mostly white; the cyan sits in the effects. The loading screens live in loading_screen\loading_screen_global.str (DS2DAT6/7/8; the movies' pages are all chrome and turn red too since the first test, 13 textures, 4 effects), read as randomizer\ds2coop_loading_screen.str. Both under `[Menu] RedInGameMenus`.
- Arena fixes (user, 16.10. evening, crypt, church sanctuary, Marker floor; built, now in CRC A37081AD with the slasher fixes below, not installed, untested):
  - Rooms with enemies stay loaded: an enemy belongs to its spawner's stream and went with it (crypt: seven at once when the player walked to the lift). The director exports DS2Survival_KeepPoints (every living enemy and the spawner it came from, 500 ms, arenas only); the arena's WantedSets adds their sets to the players'. Logs `arena: the director's keep points found`.
  - Crypt lift: spots 5 and 6 moved off the lift (z -468.90). The lift has no navigation, so it is a closed place (NoGo box around Mover 3926B250): a player who steps on it is put back where he last stood outside, and the director's inside test leaves it out. Log `stepped onto a closed place`.
  - Church sanctuary: the two gates by the checkpoint (37CAD81B-456985A7, 37CAD81A-ECA51EC2) are blocked by hand. The clipping geometry there is not found yet.
  - Valuables and logs: placed semiconductors (PowerUps whose template is a "... Treasure", Peng too) and text and audio logs (LogEntry TEXT/AUDIO) in an arena's rooms or within 15 m of its walk are not created; the six valuable and two log templates that drop lists, containers and a spawner make at run time are refused at the spawn filter in every arena (a refused drop falls through to the next item). ch03 1 gold + 2 text logs, ch08 2, ch09 1, ch10 2 audio logs, ch11 1 text log. A semiconductor carried into an arena is not restored there (arena saves are held). build_arenas.py no longer imports ds2str from the Modding Tools folder.
  - The church hall went dark: the campaign darkens a room it does not expect the player back in. build_arenas.py now leaves out (kind 1) a one-shot trigger (broadcast limit) that switches lights off or culls, and a door's close manager (`*_mm_front_close`, `*_mm_back_close`) that does, since only the other side's open manager switches it on again. Shadow and post effect switches stay. Sanctuary: vestry door 58355A68 front close (church hall lights, culling), back close (vestry), trigger 58CB6A64 (hall off, once) and two one-shot vestry culls; 23 in all arenas, listed by the build (`darkeners not created`).
  - Frozen enemies: one that has not moved a metre for `[Survival] FrozenSeconds` (15, 0 = off, at least 8 s old, not in a vent; Lurkers, Pukers and Stalkers excepted) is taken away and its item queued again. The progress watchdog never caught a slasher stuck in a wall next to a player (Marker floor: 65 s, the player within the engage range kept it fresh). Log `watchdog frozen: ...`, `frozen_kills=` in the summary.
  - Winding vents (user: "the Marker Test-Bed is a zig-zag, slashers come out in the corridors beside us and take ages"): a vent's walk from the nearest player (NavPower, the reach check) now counts. A walk longer than MaxPathFactor (2.2) times the distance plus 4 m is used only when no other vent is left (the shortest of those); in the log a vent 21 m away was a 104.5 m walk. The reach cache is walked again when that player moved more than 6 m (6 extra walks per vent search at most). Wave line `winding= winding_used=`, log `... m away but a ... m walk, kept back`.
- Slashers that play dead, creep or never come (user, 16.10.: "on the campaign maps they are less aggressive and play dead", "on the Marker Test-Bed every slasher sneaks"; analysis/ARENA_SLASHER_BEHAVIOUR_2026-10-16.md, signatures checked in the 16:29 dump; the exe on disk is encrypted):
  - Play dead: spawn copies CMD_SetPlayDeadEnabled into the NPC flag word (+0x6F4 bit 0x10, 0x00FECEF8); a limb shot off from more than 3 m rolls 15 % (0x010152F6). Nearly every donor has it, on the MP maps too; bots shooting limbs from range make it common. `[Survival] NoPlayDead=1` clears the bit once per enemy after its spawn (atomic AND). Wave line `play_dead_off=`.
  - Never a target: Ellie's two street_combat slashers (31CAF23A-84B8CF8F, 31CAF23B-818D595D, in the vent pool of ch11 and ch04) track only "Human NPCs"; forced awareness cannot help. Left out of every donor list; a template that tracks others only gets GroupPlayer in a free slot for the run (put back at the run's end). `groups_added=`.
  - Creeping: a charge (UpdateApproachTarget 0x00FFD800, once a second) needs the target in sight (IsEntityVisible, call 0x00FFD960), and the walking gait is kept until a charge decision. Forced awareness gives a target, never sight, so in corridors they walked. `[Survival] ChargeUnseen` (0 off, 1 campaign arenas, default; 2 everywhere) turns the je at 0x00FFD967 into jo for the run (one byte; restored at the run's end and when the level ends, like the charger maximum). MaxChargers stays 2 and VentMinDistance stays as it is in the arenas (user: 4 chargers and an 8 m vent minimum were too much). Logs `chargers: charge without sight on`, `a charge needs sight again`, wave line `sight_gate=`.
- Voice packs (user, 16.10.: "dynamic character banter like CoD Zombies"; first reactions to events only, no conversations, no RIG radio; the user and friends record the lines). `[Voices]` in DS2United.ini, src/coop_voices.h (tests: build_voices_tests.cmd), src/runtime_coop_voices.inc, voices/README.md and voices/_template/pack.ini. Not installed, untested in game.
  - Packs: `plugins\voices\<pack>\pack.ini`, one section per event (death, pain, grab, grab_<enemy>, reload, stomp, melee, test), each line `file.wav|subtitle`; a random line, never the same twice in a row; chance and gap per event (`Pain=100,2500`). WAV PCM 8/16/24/32 bit or float, any rate, stereo mixed to mono; loaded on a worker thread, 24 MB cache.
  - Who speaks with which pack: `[Voices] Pack` (personal, the config check leaves it out), else `Defaults` in join order: humans by machine index (the host first), then the host's bots; more characters than packs wrap round; the same pack twice is fine. Each machine sends its pick every 2 s in a 112-byte 'VOXP' packet (dispatched by its magic, whatever else has that size); the host adds its bots' packs and its human count and passes the clients' on.
  - Sound: our own XAudio2 engine (xaudio2_9.dll loaded at start, X3DAudio from the same DLL, the world's x mirrored for its left-handed frame), panned from the camera (CameraManager player 0, as the marks). Volume falls off past 3 m and is gone at MaxDistance (60). A wall between the local Isaac's head and the speaker's mouth (CastingTest through bot_sense::CastRaw, every 150 ms) muffles it (low pass); then the NavPower walk around (bot_nav::Query, every 500 ms) sets the volume by its length and the direction by its first turn 2.5 m away. Behind the camera a little duller. Changes glide. The engine stops while the game pauses (turret::SimMs).
  - Subtitles: up to three rows at the bottom centre, the pack's name in the speaker's colour (`Subtitles=1`).
  - Placeholders: tools/make_voice_placeholders.py speaks four test packs (test_a..d, 72 lines) with the Windows voices into build/voices.
  - Isaac muted, events from his own vocals (analysis/ISAAC_VOICE_SOUNDS_2026-10-16.md; `[Voices] MuteIsaac=1`, only while packs are there): every Player vocal comes from animation events and ends in Entity::PlaySound(hash) (Player vtable slot 0x01B29574, 0x007D1610, thiscall ret 8; slot checked and swapped at start). The 54 vocal hashes (sdbm32 of the name, tested against the names; breathing and asphyxiation stay) are not played and become events: hit reacts pain, struggle and panic grab, grunts a grab while grabbed (else nothing: zero-g, aim walks), attack_lrg stomp, attack_lrg_short melee, crushed death. The grabber comes from the paired coordinator (Player+0x2A8, attacker +0x88, defender +0x90; its vtable names the kind for `grab_<kind>`); one grab speaks once. A mid hook in LuaScriptComponent::HandleCCTEvent (0x00FF6B1B) reports `pc_death` and skips the 2D stomp yelling stream and the shock scream (pain). PlayerReloadSM::PlayReloadAnim (0x00874470) reports reloads of the own Isaac and the host's bots. Deaths and reloads also go out in a 12-byte 'VOXE' packet (the host relays); the rest fires on every machine from the ghosts' animations (hypothesis from the research, the diagnostics count local/bot/ghost). Caveat: the MP maps' hip melee uses the stomp grunt.
  - Logs `voices pack ...`, `voices: ... speaks with ...`, `voices: ... says ...`, `voices: Player PlaySound slot ... ours`, diagnostics `voices asked= played= ... casts open= blocked= walks= found= packets sent= in=` and `voices isaac slot= cct= reload= muted= (pain struggle grunt stomp melee death quiet; local bot ghost other) grabs= grabber_unknown= cct deaths= stomp_streams= shocks= reloads= events sent= in=`.
- Necro players in story co-op, doors and story triggers (user, 16.10.: "a spawned necro should force the door between two rooms open like a co-op human, until everybody is in one room"; the user could not say what happens now, so from the code). The door wait and the progression quorum counted every partner: a necro's machine, whose body reaches neither doors nor triggers and which at the vents sends no position at all, held the humans up to 180 s at every door ("WAITING FOR PARTNER - DOOR") and every gated trigger; at the vents its machine also cast early votes that could complete a human quorum before the last human arrived. Now every machine says its role once a second (12-byte 'NROL' packet, relayed by the host; necro::LocalRole: human, living necro, necro at the vents; silent 10 s = human):
  - the humans never wait for a necro at a door (PartnersVoted, PartnersAtDoor) or a trigger (gate Required); a necro's gate votes are ignored;
  - a living necro holds a door between it and the humans open like a human (PlayersOnBothSides counts it), one at the vents does not;
  - a living necro opens doors without waiting for the humans (mirrored as usual);
  - the necro's machine holds no progression and casts no votes (like Versus).
  Diagnostics `coop_worldcalls necro doors: votes_skipped= forced= | roles sent= received= changes= local= necro_partner=`, `coop_gate ... necro_machine= necro_votes_ignored=`, logs `roles: ...`. Built, not installed, untested.
- Invasion: necro players in story co-op (user, 16.10.; analysis/NECRO_GHOST_MODE_2026-10-16.md; `[Necro] Invasion=1`, `GhostFollowRadius=25`). Built, not installed, untested.
  - Join as a necromorph: the story screen's "join co-op" row shows `< human >` / `< necromorph >`; left/right switch it (SP01's input, vtable slot 0x01B38138 = 0x00A989D0, hands only 8..13 to the list input 0x00A6AEF0, so 6/7 were free), the row is drawn again (UIExternScrollMenuIterator::UpdateItem 0x015E9800) with the menu's navigation sound, the description says so, DS2Profile.ini `[Coop] JoinAs` keeps it. Native arrow art was looked for: SP01's btn_left/btn_right are the PC help bar's buttons and the help bar has no arrow glyph, so the arrows are the game font's in the row. A necro joiner turns once the host's level runs 3 s here, then goes straight into the vents.
  - Back into the vents during a human's scene: MP's re-entry path (NecroPlayerMasterSM::CheckTransition 0x0108D990, transition 0xD, forced true while a human partner is in a scene by his VISI packet; not dead, not in a grab, only the local necro): body hidden, RespawnProxy created with the necro's id. A ghost cannot accept a vent until 2 s after the last scene signal (proxy CheckTransition id 6).
  - Ghost camera near the humans: AutoSelectRespawnVent (0x010C2B20) found no human team in the campaign (the team group 0x91E21583 does not exist) and took the vent nearest the death spot. Our stand-in takes the nearest spawnable vent (FindNearestVents 0x012FFB20, CanSpawnAtVent 0x01072B40) within GhostFollowRadius of a human whose stream is loaded and shown here; the room view looks from the human's side. When a human's completed stream set (VISI area) changes, the proxy SM drops its selection in its own update (SetCurrentlySelectedVent 0x0108F230 with 0, message 6, at most every 2 s) and the engine picks and moves again, then the considered list is refreshed (0x010A8280).
  - Streaming: a necro at the vents takes the first human partner as its reference and stands "next to him" (NearTick adopts his set), never reverts to its own set, and has no side at a door. Its free camera (RespawnProxy) no longer counts for TriggerPhysical filters in story co-op.
  - Doors: StandardDoor::UpdateEntitiesInRange counts only Players and NPCs, so doors never opened for a necro. A living necro within 2.5 m of a closed, unlocked door opens it (TryToActivateDoor, as a partner's replayed opening); the door it stands in stays open on its machine. Its machine replays no human scene cue (unknown with a necro body in slot 0).
  - Diagnostics `necro invasion forced_ghosts= refused local= grab= dead= accepts_held= room_changes= recentres= auto_picks= fallbacks= vents_rejected spawn= stream= doors_opened=`, `coop_ghosttrigger` proxy refusals, logs `invasion: ...`, `join: ...`.
- Co-op audit fixes (analysis/COOP_LOOP_DESYNC_AUDIT_2026-10-16.md, 21 findings; built, untested): lift holds are cleared with the world and released only with a body (a restart could run a deleted MultiManager); a level exit clears the queues, the host's journal, a partner death not played yet and pending restart requests; a partner death within 6 s of our own restart or outside a level is acked but not played; a dropped configured peer keeps its seat and address; the host no longer sends its copy of a client's scene back to him (direct sends while following); the 32-byte readiness branch checks its magic before identifying the sender; a restart drops a pending respawn; the checkpoint follow forgets the old host's wishes on a new host token and gives up a checkpoint not found in 3 min; roles belong to the token that sent them. Not yet: host relays of the seven client packet types and roster pushes for later joiners (finding 4), the journal keyed by source (5), journal replays of scene cues (7), vote retirement (9, would cut the repeats a loading partner needs).
- Invasion and co-op fixes after the first invasion test (16.10. evening; plugin F9493F35+, not installed, untested):
  - Host stuck at the loading screen after a checkpoint restart: GameMainLoop::UpdateQuickRestart (0x00410200) state 1 waits until stream set 0 has nothing left to unload (0x007578E0: +0x99C, +0xC8, +0x240), but nothing runs Set::ProcessPendingLoads while it waits. slasherclergy (a stock character package the randomizer pool holds through its parent reference) sat in the unload list when the restart began, and the zone_unload heal only works inside ProcessPendingLoads. Read live from the stuck host (restart state 1, set+0xC8 = 1, handle 0x3D). Now the frame tick takes pool-held packages off the unload lists once the status test has been silent for a second (`zone_unload swept ...`, `zone_unload sweeps= swept= faults=`).
  - Necro machine crash in a human's QTE (crash 9712, analysis/NECRO_QTE_CRASH_2026-10-16.md): TestInputEvent binds its player once (GetPlayerFromPlayerManager(0), NULL with a necro body in slot 0) and Activate (0x010AECB0) uses it unchecked; the host ghost's animation event started the QTE there. Activate is skipped without a player or without the player's AlertVFX component (`necro story_guards qte_activations= skipped no_player= no_alert=`, `qte ... not activated`).
  - MP universal ammo seen only on the necro's machine: NecroPlayerMasterSM::DeathDropItem (0x0108DBD0) dropped the NecroPlayer's MP list 37CA4774 (large MultiAmmo 75 / Health Kit 25) locally. No death drop in the story (`death_drops skipped= passed=`).
  - Partner gun missing on the client (analysis/PARTNER_GUN_INVISIBLE_2026-10-16.md): every role change rebuilt the host's ghost, often at the same address, the game deleted the guns of the old body, and the eight provisioning attempts were spent on the first body. The per-body reset now runs before the budget check, an armed body that holds none of its guns any more counts as rebuilt, and a gun stream still loading costs no attempt (`bodies new= rebuilt_in_place= budget_stops= stream_waits=`, `coop_ghost_gun slot N: new/rebuilt body`). Open: the host never sees the client's gun (the client sends weapon hash 0).
  - Ghost camera in black rooms: the ghost's free camera is refused by the show/hide triggers, so the humans' rooms stayed hidden on its machine. A necro at the vents now takes the humans' stream picture from their VISI packets (shown when any human shows it, hidden when the humans who have it hide it; `coop_streams necro_ghost_picture passes= shown= hidden=`); it also adopts a set a human followed into.
  - Vents: a necro at the vents cannot pick a vent in a room its machine does not show (CanSpawnAtVent 0x01072B40 hooked; `hidden_refused=`); the stand-in vent pick first looks within GhostFollowRadius, then for the nearest shown vent anywhere (`far=`), before the game's own pick.
  - Scenes: the scene hold is 4 s (QTE scenes run as several sequences with gaps); the necro key or a necro join during a human's scene waits for its end (`transforms_held=`, `after_scene=`).
  - Story co-op draws names with the overlay only; the native MP plates had appeared because the necro pool brings global_mp_assets in.
  - Voice packs: the Player PlaySound slot write failed on the read-only vtable page (`WRITE FAILED`), so Isaac was not muted; the slot write now lifts the page protection.
- Necro players watch the humans' scenes as Isaac (user, 16.10.; design and every situation in analysis/NECRO_SCENE_WATCH_2026-10-16.md; `[Necro] WatchScenes=1`). Built, not installed, untested.
  - A partner's scene cue on a necro machine is queued (mirror), the machine sends role 3 and, 300 ms later, turns back into Isaac (Revert for a living necro, EscapeGhost for a ghost) where the scene's author stood about a second earlier (his trail, every 250 ms). The cue then plays; the cutscene module makes him a follower (leader's input, invincible, cues not sent), and input and damage stay blocked between the scene's parts.
  - Waits: a paired attack, a vent exit or a vent request in flight (up to 6 s, then the watch is dropped and the cue skipped as before); no watch during a restart or level change.
  - End: no local scene, no human scene signal and no scene cue for 4 s (shooting parts count as scene), at least 3 s in; a scene that never started here ends it after 4 s; 5 min fail-safe. Back via Transform and the forced vent re-entry, role 3 until the ghost is up; 10 s of failures leave him human (key turns him back).
  - Humans' machines: every body of a role-3 machine stays hidden (cutscene hide bookkeeping, also outside scenes), no overlay name, no enemy target (nearest-target mid hook, now also installed for invasion), no voice events; his world's messages are not sent.
  - QTEs built while slot 0 held the necro are bound to the new Isaac at Activate (TestInputEvent SafePtr +0x5C, the constructor's way, 0x010C4E3D).
  - A necro by choice (key or join) is a necro again after a checkpoint restart or level change (join path, outside scenes); before, a restart left him human.
  - Diagnostics `necro watch=`, `coop_mirror necro_scenes skipped= watch_cues queued= played= dropped=`, `coop_cutscene ... necro watchers hidden= shown=`, `nearest_target ... watcher_refused=`, `necro story_guards ... rebound=`, `coop_ghosttrigger invasion ... local_necro_filters passed= refused=` (does a NecroPlayer body count for story triggers?).
- Grab sync (analysis/GRAB_SYNC_2026-10-16.md): a partner's grab copy waiting for its defender never resolved (retail: PairedAttackCoordinator::OnReflectableAvailable 0x010E2A60 compares the arriving NetID with the attacker's twice); the wait bit is cleared for the defender's NetID (`[Coop] PairDefenderWaitFix=1`, `pair_defender_waits_fixed=`). The ghosting reset for a partner's new body waits while a grab object exists, at most 12 s (`[Coop] ReghostWaitsForGrabs=1`, `deferred_for_grab=`); invasion body switches had reset mid-grab. The coordinator trace labels copies by the proxy's ghost bit. Built, untested.
- Save stations (analysis/COOP_SAVE_STATIONS_2026-10-16.md): the held kiosk save's completion was never reported since 14.10. (vtable compared with 0x01B42BA4; the object at SaveManager+0x24 has vtable 0x01B42B68, the callback 0x00BAE3A0 is its slot +0x44): fixed, logged with the manager state before and after. The host announces the death checkpoint the game holds after a save load (`host_taken_from_game=`). Kiosk restore scripts (`*_mm_save_kiosk`) are never sent or replayed (`[Mirror] AlwaysLocalNames`). A client that plays a necro keeps the host's checkpoint for later (`applies_deferred_necro=`). Open, needs the user's decision: the client's own progress is never saved (every client save is held). Built, untested.
- Scene watch, first test (16.10. evening, installed BDC2D7D2): cues held and played (3/3), Isaac behind the host, leader input applied. Fixed after it (not installed):
  - the role packet refused role 3 (`p.role > 2`), so the host never knew the client watched (and after 10 s took him for a human);
  - the Tormentor: a forced vent re-entry and the way back to Isaac 300 ms later faulted in the respawn (0x0084A9D4), the client died a moment later in a dispatch (0x0084A94B, crash 25052); later cues of the long scene started nothing on the watcher, so he went back and forth seven times. Now the way back waits 2.5 s for a fresh ghost proxy; the scene signal alone forces the vents only 3 s after it began without a watch (the cue decides); a watch whose scene never started here (6 s) or that failed sits the rest of that scene out (`sit_out=`, `sat_out=`); the end waits 8 s of quiet; a human's stationary shooting part counts as scene in his VISI signal;
  - a watcher's world reports no hits (`watcher_hits_kept=`). The necro's yellow-orange screen stays while watching (user: a reminder).
- Client progress (user, 16.10.; analysis/COOP_SAVE_STATIONS_2026-10-16.md F4; `[Coop] ClientProgress=1`, src/coop_client_progress.h, tests: build_client_progress_tests.cmd, src/runtime_coop_clientprogress.inc). Built, untested.
  - A client keeps credits, power nodes (Player+0x42C), guns, general inventory stacks, suit and filled bench nodes per host in `plugins\DS2ClientProgress\host_<id>.ds2client` (FNV-1a checked, previous file kept as .bak). Written on a held kiosk or autosave, when the host saved (his save key, SaveManager+0x2C, rides in the checkpoint packet's reserved[1]; his id, DS2Profile.ini [Coop] HostId, in reserved[2..3]), before a level change and every 2 min, only while Isaac lives, plays no scene and no necro.
  - Given back once per session with that host, 5 s after the level runs (credits and nodes only rise, missing guns and items are given, bench nodes filled and recalculated, suit put on), then the host's checkpoint is stored again so a restart keeps it.
  - Every machine (host too) keeps Isaac's state across a necro transform: the respawned Isaac is the level's template (four items, no credits); what he carried comes back 1.5 s after the necro key brings him back.
  - Log `client progress applied (...)`, `client progress written (...)`, summary `client progress host= captures= writes= loads= applies= carries= given guns= stacks= bench_nodes=`.
- Voice packs: `voices\_example_pack` shows a pack with one folder per event (grab kinds in `grab\<kind>`), a RECORD_HERE.txt in each; pack.ini lines may name a file in a subfolder of the pack.
- Client freeze in a watched scene (16.10. ~21:15, stuck dump 37972, exit guard restart): FindByGuid(guid, after) also walks every entity with a GUID (the scripted sequence's actor loop 0x00EB8300..0x00EB8396 calls it with its last result); guidfix's redirect answered the ghost Isaac with the local one, the walk came back to the ghost and never ended. In a walk (`after` set, [esp+0x10] at the pop edi / pop esi / ret 8 epilogues) nothing is redirected any more: ghosts and bots are stepped over (`coop_guidfix ... walks= stepped_over= walk_ends=`). A crash now also copies the session's co-op log next to its dump (`deadspace2.crash.<pid>.log`); the restarts had rotated the crashed host's log away. Plugin 9A4962E0, not installed.
- New features (user, 16.10. night; built, untested; plugin 872015A5 with the guidfix walk fix, installed 17.10. (backup .bak-20261017a = 891B4510)):
  - Log history: DS2Coop, DS2Versus, DS2Bots and the co-op copies' DS2Randomizer.coop<port>.log keep three old runs (.prev.log, .prev2.log, .prev3.log; src/log_rotation.h). A log another running copy still writes is left alone and the next copy number is used. A crash copies its log with read/write sharing (the old CopyFileA could not open our deny-write log).
  - Bug key (`[Coop] BugReport=1`, `BugReportKey=123`, `BugReportCtrl=1`: Ctrl+F12): one folder `bug_<time>_<id>_m<machine>_<pid>` in C:\DS2CrashDumps (else plugins\DS2BugReports) with all logs and INIs, screen.bmp, a dump of every thread (`BugReportFullDump=1`: whole memory) and note.txt (role, map, position, peers, how long the game thread has been silent). Every partner gets 'BUGR' and saves his own under the same id. Its own thread reads the key, so it works in a hang. Log `bug report <id> saved to ...`, summary `coop_bugreport pressed= partner_requests= written= files= shots= dumps=`.
  - Necro respawn pace (`[Necro] RespawnBaseSeconds=8`, `RespawnMaxSeconds=30`): after a necro death the next vent opens base x necros / humans seconds later (CheckTransition id 6 held, like the scene hold); a countdown `RESPAWN IN n` on the necro's own HUD line (hud::SetOwn, never sent on by a host). Log `respawn pace: ...`, summary `necro respawn_pace deaths= vents_held= opened= last_wait_ms=`.
  - Heartbeat radar (`[Necro] Radar=1`, `RadarRange=40`): a necro player, alive or at the vents (measured from the ghost proxy), sees a red heart over every human within range through walls; a double beat whose period shrinks from 2 s to 0.5 s as he comes closer, an arrow at the screen edge when off screen. Not in Versus. Summary `marks radar ticks= from_ghost= hearts= out_of_range= no_body= drawn=`.
  - Voice packs phase 2, situation lines (`[Voices] Situations=1`, analysis/VOICE_SITUATIONS_2026-10-16.md): `low_health` (below 25 %, re-armed above 50 %, 1.2 s after the hit, waits out a grab), `low_ammo` (clip + spare at most one magazine, `low_ammo_empty` at none and always spoken; Player::CountSpareAmmo checked by its bytes), `new_area` (a stream set first completed on this map, asked for by a door, lift, script or follow, still current 2 s later; detail the chapter `ch04`; not when a partner already stands in it), `big_enemy` (NetReflectable chain, brute/stalker/tripod/hunter/tormentor/nicole/guardian within 25 m, on screen and a clear cast; once per enemy, 60 s per kind), `multi_kill` (three victims in 4 s by the own Isaac or a host bot, from the health thunk next to the kill credit), `necro_spotted` (a partner's living necro within 20 m in clear sight, story invasion; again after 20 s unseen). Detail code in the VOXE spare byte; a partner's room, big enemy or necro line holds ours back 8 s. Example pack folders, template sections and README rows added. Summary `voices situations fired ... team_held= | ... health_hooks=`.
  - Spectator after death (`[Spectate] Enabled=0`, experimental; analysis/SPECTATOR_AFTER_DEATH_2026-10-16.md, src/runtime_coop_spectate.inc): in story co-op a death while another human lives restarts nobody. Decided at PlayerDeathSM 0x18 (story, local Isaac, no necro, not in a scene unless `InScenes=1`, a living human partner: role 0, heard within 10 s, ghost not dead); 0x20 and 0x1E swallowed; ProcessDeath returns without the SP branch; the dead Player stays in slot 0, role 4 in NROL (no voter, no target, no stream reference, no necro either). Camera: DefaultSpawn<ObserverCameraInfo> 0x0109D6D0 + SetSubject 0x01085A20 on the partner's ghost + PushCameraInfo 0x00792530 on viewport 0 (pushed on the next frame, five tries; offset (0, Height, -Distance), orbit off); mouse buttons switch partners; `CameraMode=1` keeps the death camera. Back: a client when the host's checkpoint epoch moves past the one he died with; the host when a client reports a reached checkpoint (CKPT flags 2, sent while a peer has role 4; clients now ignore every flagged CKPT); a partner's scene cue (held, Isaac back at the author's trail spot, then replayed like the necro watch); a partner's LevelChange vote (the living machine's LevelChange holds count role-4 peers, coop_gate TakeEach). The respawn: ProcessDeath MP branch (respawn::CallOriginal), watchdogs ScheduleDirect/RespawnDirect, the new body placed on Checkpoint+0x60 (the checkpoint's matrix) or beside the nearest living human (40 m rule, 8 s Find wait); then respawn::Restore and only then the checkpoint (host ApplyTo, client checkpoint::Tick), curtain up meanwhile. Corpse gone: a fresh Isaac from the level template (necro::RespawnPlayer with the dead body's GUID and sub-key). Nobody alive for 2 s: host pulls the restart, a client dies natively. Refused while watching: death-path restarts, catch-up (F8 and the gate's pull), the necro key. Reghost skips the reset for a role-4 peer and 15 s after his role 4 -> 0. The respawn module's death, RespawnPlayer and death-state hooks moved into respawn::InstallHooks, shared with this module. Diagnostics: `spectate: ...` lines (decision with every peer, camera spawn/attributes/push/poses, triggers, placement, finish) and the `spectate stage= ...` summary.
  - Client progress check: the capture reads the right values (host 16.10.: credits 43200, 4 nodes, guns 207, 7 stacks); the client really had 0 credits and got its three guns later.
- Necro test 17.10. 00:33 (client crash 39376, plugin 872015A5), fixed in 8E8DF0CC (not installed):
  - Ghost camera far away and stuck: a campaign vent's room view is all zero; the mover took the world origin (46.8 m from the ch02 vent, under the 60 m limit) as authored. Zero views now count as missing (the synthesized view in front of the vent), the authored limit is 30 m (`zero_room_views=`).
  - Crash 0x0108C313 (NecroPlayer::RegisterWithManager on null): the type menu approved the spitter, whose character stream a sixth-type setup never loads (only the configured type's: pack); GetSpawnInfo invalid, SpawnEntity null. The menu now approves only a type with instance data here (else the configured one, `menu_type_fallbacks=`) and asks for the picked type's stream for the next time while TOC slots stay below 29 (`necro menu_streams`); NecroPlayer::Spawn falls back the same way and never spawns a type without instance data (`spawn_type_fallbacks=`, `spawns_refused_no_template=`).
  - Doors (user: a necro opens no door, he only holds an open one open like a player in the room next to it): NecroDoorAssist (TryActivateDoor on a closed, unlocked door within 2.5 m) is off, `[Necro] OpensDoors=0`; KeepOpen and PlayersOnBothSides still count a living necro.
  - Vents in unloaded rooms were selectable: every campaign vent sits in the level's root stream (dump 39376: ch05_sol\ch05_sol, handle at vent+0x38), so the stream test passed them all. Now a vent needs a human within `[Necro] VentHumanRange=45` m and its room's collision: from 0.6 m out of the opening (normal vent+0x310) a cast back onto the surface behind it, or 3 of 6 axis casts within 12 m (CanSpawnAtVent and the auto pick; cached 1.5 s; `necro vent_rooms probes= empty= too_far= refused=`, the first 40 probes logged).
  - The necro turned into Isaac now and then for a few seconds (user): five watches in the 00:39-00:59 session, every one ended `no_scene`, none had a human scene signal (scene_hold 0). Their cues (A1EA1DCF, 9F51B32E, BA9A9AAC) are ScriptedSequence world events that never take the partner's Isaac. A cue now only holds (Watch::Confirming, role stays 1/2) and the necro turns into Isaac when the author's own scene signal (cutscene::PartnerInScene, the VISI sequence age) follows within 2.5 s; otherwise the cues are dropped as before the watch (`cues confirmed= world_events=`).
  - The respawn pace missed the second death (the ghost proxy reported it before the death poll): PaceStart from both (`pace_from_proxy=`).
- Test 17.10. 01:00-01:16 (plugin 872015A5), fixed in 8BC6AC1C (installed 17.10., backup .bak-20261017b = 872015A5; build maps kept in build\maps\ from now on):
  - Slashers died suddenly (survival director, 19 `frozen_kills`): NoteMoved stamped `now | 1`, an even tick became now+1 and `now - movedAt` in the same tick read 4294967 s; Frozen compares signed now. The same stamp (`Stamp(now)`, never 0, no +1) replaces `now | 1` in necro, spectate, voices, hud and marks: the watch confirmation, the ghost settle and the scene ask could have passed at once the same way. Survival test added.
  - Wall vent cameras beside or behind the vent: the ghost camera took the edge matrix's row 2 (along the opening) as the way out; both the room view and the exit camera now stand on the vent normal (VentMarker+0x310, dump 39376: normal (-0.71 0 0.71) vs the old "front" (0.71 0 0.71) along axis A), the normal turned round when it runs into geometry, the camera pulled in short of any wall (`necro vent_cameras`).
  - Client crash 29076 (0x013E2D21, after a checkpoint restart, necro HUD in the loading screen): the ghost's RespawnProxy comes from global_mp_assets and outlived the restart; its state machine (0x01B785EC) ran on the freed level. Deleted before the restart (shutdown + Base::Delete), MP12/MP13 hidden.
  - The necro did not see the tripod appear in the watched scene: a held watch cue replayed without opening the scripted-spawn window (gate::lastSequenceTick); it does now.
  - Arena start curtain 23.5 s (arena_ch10 spot 6's rooms never counted as loaded): a first Isaac takes a spot whose rooms are already loaded, at most 6 s wait for another.
  - Enemies invisible for seconds after a team switch: the reghost reset (built for rebuilt worlds) ran for every new body; with `[Coop] ReghostOnTeamSwitch=0` a role change within 15 s skips it (the decision waits 1.5 s for the role packet).
  - Host audio gone twice: the game mutes itself when its window loses focus (byte 0x02021F69, two instances on one PC); live the flag was 0 the second time, cause open. Host stuck in a quick restart (state 1, set 0 +0x240 = 1, not the unload list this time) and host crash 50284 (0x00DB4204, after a new load): open.
- Test 17.10. ~01:30 (plugin 8BC6AC1C), fixed in F0D05342 (not installed):
  - Enemies spawned far away on the Ishimura arena (user, 02:15: wave 2's direct spawns all at its west end, 17-23 m off, walks allowed up to 2.2 times that; vents up to 30 m): in every campaign arena the director's window is pulled in to 18 m at most (vents 20 m), 13 m ideal, a walk of at most 1.6 times the distance; outside the arenas unchanged.
  - Mining arena (arena_ch09): the repair room behind door 5AD21A73-5135E74C (-117.2 -8 -31.3) counts now (user): the door has lock fields, so the generator always locked it; new arena option `open=<door GUID>` keeps such a door inner; the user's second recording (ds_2_20261017_020248, 10 samples) is appended to arenas/arena_ch09.txt (+300 s). Only the first 3 m of the room are walked: the director's spawn area and "inside" follow the walk (5 m around a sample), so the rest of the room needs a longer walk.
  - Marker Test Bed arena (arena_ch11, ch11_mtb): its lift does not work any more (user): the switch in the car (583959ED-B0165515) and both call controllers (583959ED-F69A297A top, 583959ED-24EBF26E bottom, mtb_room_lower_03, car at 300.8 -98.8 126.7) are `block=` lines in arenas/arena_ch11.txt, not created; the car stays as floor.
  - A respawned arena body wore the patient suit (user): the body comes from the player template; the arena still gives it no second kit, but changes the patient suit into [Arena] Suit.
  - No Brute in the campaign arenas (user): every Brute item of a wave, a stage or the final enemies (after the stand-in resolution) becomes the Ubermorph there (`Hunter`, one hunts at a time, a second is skipped), or is dropped on a map without one (director log `arena <map>: n Brute(s) become the Ubermorph`).
  - Crash 52780 at 0x0108C313 again (the lurker from the type menu): the template records of all four types sit in the pool root, so GetSpawnInfo answered for the lurker although its character stream was not loaded. A type now counts as spawnable only with its character stream in (the configured type's model stream, a menu type's extra stream, the Leaper's for the sixth); otherwise the menu approves the configured type and asks for the picked one's stream for the next time.
  - The living necro no longer loaded the rooms the humans opened: he used to open doors himself, which loaded them; since `OpensDoors=0` the humans' opening reaches him only mirrored. An open door within 6 m of the living necro now has its open set (door attributes +0xC0/+0xE0 for his side) activated here, both rooms of the doorway (`necro door_rooms loaded=`).
- Scene kinds, 17.10. ~22:35: a QTE counts only inside a scripted sequence or event (master state 1/2 or a running sequence). A combat grab (paired anim with a mash prompt, no sequence) is no scene: it no longer starts a watch from the kind, holds the vents or keeps a watch alive (test 22:30: a 91 s "watch" through the Tiedemann fight's combat grabs; the leader's-suit births were all clean, 5 of 5 resident).
- Full review 17.10. evening (A48B9739, not installed). The user stopped feature work after many regressions. Three reviewers and the lead each took one part of the source, fixed bugs and added fail-safes:
  - Crash fail-safes:
    - The game-thread work (survival tick, stream graph, placement, vent queue, repeats) runs SEH-guarded inside the watchdog hook.
    - The stream probe hooks are guarded, with the lock taken outside the guard.
    - NecroPlayerExitVentSM's destructor and its messages 0x18/0x19 get a zeroed MPLevelSettings stand-in while the singleton is null at teardown (crash 15156).
    - The NecroPlayer::Spawn access violation at 0x0108C313 is caught.
    - The held lift ride is sent guarded.
    - The spectate camera pops at once when its subject is gone.
  - Timestamp wrap family (a fresh stamp newer than a tick's now read as 49 days, `now | 1` stamps): fixed across all parts with Stamp(now) and the signed coop::TicksSince.
  - Necro and scenes:
    - The scene watch uses the author's scene kind: a short sequence no longer cancels it, it ends after 8 s of quiet, and a necro at the vents starts it from a watchable kind alone.
    - The return waits for the ghost.
    - Teleport follow keeps the event and has the 25 m / 1.5 s jump as a fallback (events expire after 5 s).
    - The Q-spawn sight rule counts bots.
    - Ghost-attacker checks read DamageData's SafePtr correctly (partner hits on Nicole and the Tormentor no longer count twice).
  - Bots:
    - Killable whenever any machine plays a necro (bot::MortalNow); Versus as before.
    - They also fight back then (user): a necro player is a target in a story game with a necro in it, as in Versus (bot_sense::Scan via bot::NecroInGame). Their hits on the partner's necro reach its owner through the usual hit report.
    - Bot ghosts' death messages are always swallowed outside Versus.
    - The suit stream wait is 12 s for a new body; a body stuck at 0 health counts as dead after 8 s.
    - Client hits on bots respect the floor and friendly fire; necro executions kill mortal bots.
  - Network core:
    - A necro never holds a door (the door desync).
    - The light sync shares only on/fade on, debounced 1.5 s, and ignores animated lights.
    - The necro reload restart really happens, after the level runs.
    - The launcher host may "host co-op"; a launcher client gets a clear card; "join co-op" no longer waits forever.
    - A client's "in menu" packet is no longer read as the host's level.
  - UI: the debug menu and the supply terminal close themselves when nothing is drawn for 1 s after opening (an open panel mutes the game's input), with a NOT DRAWN log line. A closed menu releases a stray input hold.
- Invisible debug menu (user 17.10. late, "doesn't open on either player"; CE60FBD8, not installed): the logs showed it opened and the overlay drew the panel every frame (visible=1, draw calls rising), yet nothing showed, so Insert was mashed. When the frame ends on a render target other than the swap chain's back buffer, the overlay now binds the back buffer for its draws (no depth surface) and puts the game's target and depth surface back before the state block ([Shop] OverlayBackBuffer=1). The 1 s check counts frames that drew the panel itself (the HUD and marks drew too) and logs the panel rect and whether this game window was in front. Log: `shop overlay panel_frames= offscreen_frames= back_buffer_draws= back_buffer_failures=`, `the frame ends on ... instead of the back buffer` (8 lines at most), `open for 1 s: panel drawn in N frame(s) at ...`. Whether an offscreen target was the cause shows in the next log.
- Ghost camera to the human (user 17.10. late; CE60FBD8, not installed; [Necro] GhostHumanViewRange=50, 0 = off): when the vent selection starts and the followed human's room has no vent, or no vent is within 50 m of him, the camera goes to him and the vent stays selected. A room is the stream (+0x38) of the floor: a cast down under the human (past his ghost body here) and one in front of the vent's opening; unknown counts as his room. A vent in his room within the range is preferred over a nearer one behind a wall; otherwise the pick is the old one. The camera flag expires after 3 s, so a stale one cannot pull a later camera move. Log: `necro ghost_human_view range= other_room= far= shown= expired= same_room_preferred= | vent rooms same= other= unknown=, human room unknown=`, the first 30 `vent ...: floor stream ... -> his room/ANOTHER room` lines, and the pick note's "the camera goes to him".
- Severed twitchers (user 17.10. late, "build them in completely", all three looks 18.10.; C346EC5D with a new pool, not installed): the retail game ships the twitcher packages (zomb_cct/twitcher_security, twitcher, twitcher_oracle; analysis/SEVERED_TWITCHER_2026-10-17.md) but no record that uses them.
  - Pool: variant packs `Twitcher` (security look), `TwitcherClassic` (plain look) and `TwitcherOracle` (oracle look, 350 health as in Severed), each `"retarget": {"package": ...}`. Per look 26 slasher records (direct and vent; scripted entrances, sources without CMD_SetPlayDeadEnabled and Ellie's two NPC-tracking slashers are left out) are copied with their model, rig, model stream and dismemberment prop pointed at the twitcher package, plus the Severed tuning: 200 health, dodge 0.9 after 0.25 s, back-up 2 s, no charge, no grab, no play dead, 8 damage. The plain package's template still names the slasher rig, its prop the twitcher rig; the copies take the twitcher rig like the other two. The three packages are forced resident by the pool TOC (C290FA51, 422EAC26, 098D1FC6, about 27 MB for the whole game). Pool 2.75 -> 4.5 MB, 78 copies.
  - Own switch `[DS2Randomizer] Twitchers=1`, independent of `Variants` (the stat variants stay off). A twitcher row that does not fit is left out on its own (`twitchers enabled= rows= skipped=`); with the package missing from the TOC all stay out (`twitchers REFUSED`). The copies of a look share its MetaModel as the model key, so the family draw rolls each look's chance once (`[Variants] Twitcher/TwitcherClassic/TwitcherOracle=15`; 0 also keeps that look out of TwitcherChance and survival).
  - Campaign: `[DS2Randomizer] TwitcherChance=15` makes that share of slasher sources twitchers even with `Chance=0` (only twitcher candidates for those plans; `campaign_plans=`). With Chance above 0 they also compete in the ordinary draw. More enemies copy the plan, so hordes follow.
  - Survival (MP maps, arenas, Objective, Versus non-super slashers): `[Survival] TwitcherPercent=20` turns that share of slasher spawns into twitchers, direct through their own donor list, vents through twitcher copies with the spawner's survival key (build_survival_vents.py computes their keys from their sources). Log `survival twitchers percent= direct_copies= vent_spots= vent_copies= rolls= direct_picks= vent_picks= no_copy=`.
  - Real Severed looks (18.10., 98FDC9E7): the retail oracle package is an old dev version (twitcheroracle.mm on the slasher doctor's textures, older banks, no twitcher sounds). The security and oracle twitchers now come from the Severed packages themselves (the port's PC conversion, copied from its DS2DAT12 into variants/packs/*/model/), shipped as loose streams `randomizer\ds2_twitcher_security` and `ds2_twitcher_oracle` (model GUIDs from the assets' SHDR, baked records, the oracle with its own prop 58CB8A8B). The plain look is the retail package copied as `ds2_twitcher_classic` with the Severed banks (bank resource ids are shared, so no retail twitcher package may load next to them; none is forced any more). The plugin requests the three streams after the pool (kModelStreams).
  - Final set (18.10., AC4EE5E0, user: pregnant, the two twitchers, Gabe's patrol helmet): packs Twitcher (security) and TwitcherOracle from the Severed packages; TwitcherClassic (the retail look on the Severed banks) stays at the user's wish (8E850E36). SeveredPregnant: 15 direct pregnant sources (the two MP necro-rush ones left out) on pdlc_pregnant_sag (own model 2E810DEF and rig pregnantsag.rcb, the retail pregnant banks byte-identical) as `randomizer\ds2_severed_pregnant`. Severed chances per family: `[DS2Randomizer] TwitcherChance` / `SeveredPregnantChance` (campaign, only copies of the source's own family), `[Survival] TwitcherPercent` / `SeveredPregnantPercent`. The port's converted texture headers are the format the Outbreak maps in our own DS2DAT12 use, which the retail engine renders.
  - Gabe Weller's patrol helmet: the retail Patrol Suit (Uniform 31CB7561-1B516549, `unisecuritywellerisaac`) names helmet template 31CB7561-7F92A9B3. The Severed suit package uni_pdlcsecurityweller (stream-only pack SeveredPatrolHelmet, `randomizer\ds2_severed_patrol`) holds Gabe's template 31CB97F2-4601A667 with its own model, helmet rig and motions bank (different resource ids; all shared assets byte-identical). Player::AttachHelmet (0x0087D210) gets Gabe's template instead when it is registered (`[Coop] PatrolWellerHelmet=1`; log `patrol helmet attach_calls= patrol= weller_helmet= not_registered=`). Isaac's head and the suit body stay.
  - Limb loss (18.10., 514F11DB): a severed limb takes a fixed amount of health (slasher: head 30, arms 40, legs 30). At 200/350 health twitchers lived with every limb off, so their limb damages scale with the health (x2, oracle x3.5) and they die from the same cuts as a slasher. The oracle look is the twitcher mesh with the slasher doctor's textures (retail package), so it reads as a hospital slasher.
  - Classification by vtable (bots, marks, voices, progress) counts a twitcher as a slasher. Risk: the retail twitcher banks are an older set than Severed's; a missing animation shows only in the game.
  - Install together: plugin, `randomizer\ds2_randomizer_pool.str` and `.toc` (the plugin checks the pool's fingerprint and refuses another one).
- Twitchers came as slashers, overlay gone (18.10., first test of F1F942E7; fixed in 6017FF43, not installed):
  - Parameter images are shared by id. The loose image mount (0x0085D020) skips any image whose id AttributePacket::Find (0x007CEB30) already knows. The twitcher copies kept the slasher images' ids, so they got the slashers' model, rig, sounds and 100 health (npcwatch: our GUIDs, health 100).
  - build_variants now gives every changed image an id of its own, a CRC of its content (1266 images in 81 groups). Stat variants and in-place pool field patches had the same flaw.
  - Overlay: the frame always ends on an offscreen 1920x1080 target that the game copies to the screen after EndScene. Drawing into the back buffer hid the whole overlay. `[Shop] OverlayBackBuffer` is now 0 by default; the invisible debug menu stays unexplained.
- Bots and twitchers (18.10., user: "the bots don't really see twitchers as enemies"; F95C57E9, not installed): the bots did pick twitchers as slasher targets (log `target ... vtable 01783948`), but the Severed tuning makes a twitcher dodge 0.25 s into an aim, nine times in ten, and not again for 1.5 to 3 s (slasher: 1.5 s, 0.6). Player::UpdateAimingTarget threatens whatever a Player aims at (Xbox recomp, ThreatenedComponent::SetThreatened), so a bot's aim makes it dodge too. A bot needed about 0.8 s from a new target to its first shot (pause, flick, settle; `acquire_ms_avg=784`), so a twitcher was gone before every shot. The bots now know a twitcher by its pool GUID (the Twitcher* rows of generated_variant_donors.h) and, with `[Bot] TwitcherSnapShots=1`, react as if it were on them, skip the aim pause and settling, shoot within a 1.6x wider cone and do not turn the cutter blade first (unit test: first shot after 80 ms instead of 624 ms). Log `bot twitchers rows= scanned= targeted= shots= in_sight= fast= snaps= | slashers targeted= shots= in_sight=` compares the two.
- Crash 19:32 (18.10., dump 8396, build 8E850E36; user had sent the bots away with F9; code written, NOT built yet):
  - Where: 0x00742A04, a SafePtr unlink reading through 0x30000A31.
  - Chain: FuncSpawn::HandleEvents → post-spawn init → DeadSpaceNPC::UpdateTarget → SetTarget → Player::RegisterAttacker → Player::ProcessAttackerSlots, on the human's Isaac 3D3FF010.
  - Cause: a Player keeps waiting attackers as 8-byte SafePtrs from +0x7C0 (room for 48, count byte at +0x940). RegisterAttacker checks only that list. ProcessAttackerSlots moves the three attack-slot holders (+0x730, 0x30 apart) back into it without a check, so with more than 48 in all it writes onto the count byte (49 = 0x31).
  - Trigger: a horde (118 pack enemies in that session) turned to the human at once when the bots were gone.
  - Fix: coop_crashguards hooks Player::RegisterAttacker (0x00892AD0) and refuses one more attacker when the list and the slots hold 48 together (log `coop_crashguards attackers registered= refused_list_full= peak_on_one_player=`).
  - The bots' `attack_slots` line read the count as 4 bytes ("attackers 936065282"); it reads the byte now.
- No bot damage to humans (18.10., user: "the bots must not hurt any real player, in any way"; code written, NOT built yet; `[Bot] NoDamageToHumans=1`):
  - What counts as a bot's hit: DamageData+0x80 m_attacker (Xbox PDB `EARS::WeaponUtils::DamageData`) is a bot, its gun (+0x240 owner), a bot's ghost on a partner's machine (machine 0xF0 + bot) or its gun, or an object a bot held or threw with kinesis in the last 6 s.
  - Such a hit on a human's Isaac (the local one or a partner's ghost) is refused at Player::ShouldProcessDamage. The hook now goes in on every machine, not only with bots. HealthComponent::HandleDamage, where the bots run, is a second net.
  - Stasis carries no shooter: a stasis on a human within 2.5 s of a bot's cast is dropped.
  - A hit on a human shortly after a bot acted that names no bot is logged with its attacker (30 lines), so a missed path (a mine, a blade, a blast) shows in the next test.
  - Log `bot human_guard on= hits_refused= unattributed_after_bot_action= stasis_dropped=`. Necro players in Versus can still be hit.
- Oracle twitcher as the enhanced slasher's counterpart (18.10., user: "treat it like an Enhanced Slasher, it had extremely much health in Severed"; plugin 1BE75F93 with a new pool, not installed):
  - Severed record 58CB8B68-A5A78204 (landing_bay), read from the port's baked image:
    - 350 health, limb health 40/40/40/50/50 (head, arms, legs), limb damage 90/120/120/90/90, attacks 10, dodge 0.2 s after an aim, gun pain delay 3, stasis component value 0.1 (security 0.25).
    - For comparison: plain slasher 100 health, legs 15, limb damage 30/40/40/30/30, attacks 10. Enhanced slasher 200 health, limbs 30, limb damage 50/70/70/50/50, attacks 30/30/15. Security twitcher 200 health, limbs 30/30/30/40/40, limb damage 50/70/70/50/50, attacks 8.
  - Pack TwitcherOracle now copies the 87 enhanced slasher records (model EC703B0F, direct and vent) instead of the plain ones. They author every limb field, so all Severed numbers are set exactly, no longer scaled (was x3.5 on plain records). PlayDeadEnabled is not authored there; its engine default is 0 (Xbox DeadSpaceNPC attributes). The stasis value is not authored either and stays the enhanced slasher's.
  - Campaign: EnemyRecord::enhanced (the model of a super_* pool target, inherited by copies). A Severed copy stands only where its own tier stood: oracle for an enhanced slasher, security and classic for a plain one (selection test).
  - Survival: the oracle copies are enhanced donors (superGuids, directEnhanced or the vent's donors). They come when the wave's escalation asks for an enhanced enemy, not through TwitcherPercent. Log `survival severed ... | oracle (an enhanced enemy) direct_copies= vent_copies= picks=`, randomizer `twitchers ... (oracle N, for the M enhanced sources)`.
  - Pool 6.8 MB (was 4.85 MB, 154 variants). Install plugin + `randomizer\ds2_randomizer_pool.str`; the TOC and the five loose streams are unchanged.
- More bot stasis on twitchers and stalkers (18.10., user: "definitely more stasis", stalkers too; 28CDD897, not installed):
  - Before, a bot cast stasis only on its current target with at least 200 max health or within 3.5 m, with 25 % energy, at most every 8 s. Every cast then blocked its target for the squad for 6 s, even when it missed.
  - Now `[Bot] StasisPriority=1` gives priority to a twitcher, or a stalker within 7 m or coming at the bot or the leader at 2.5 m/s or more, anywhere within 14 m.
  - Such an enemy is the target 5 m earlier while the bot can cast, and gets stasis with any energy for one cast, 2.5 s after the bot's last cast, even within swing range (before the melee).
  - A cast holds its target for the squad only 1.5 s while it flies. After that an enemy counts as stasised while its time multiplier (+0x84, 1.0 to 0.05 under stasis, measured 09.10.) is below 0.5. A twitcher that dodged the cast gets the next one.
  - Log `bot stasis priority casts twitchers= stalkers= waits no_energy= cooldown= player_in_burst= | all casts= slowed_seen=`. Energy is the limit: the bots often stood at 0 to 10 of 40.
- Credits (18.10., user; 358A9398, not installed): the mod's block comes first in the credits, in the main menu and at the end of the game: "Dead Space 2 Dead Space 2 United" as a big line like the game's own title, then "Created by" / "Hafer" and two blank lines; the original 307 sections follow unchanged. The seven text lookups inside UICredits::Initialize and Update go to our function, which adds our sections to CREDITS_Num and shifts the original ones (details in docs/DS2COOP_FOR_MARK.md 3.1). The plugin patches all seven sites or none (log `credits block on: sites 7/7`, at the start `credits start: CREDITS_Num 307 -> 311`, then `credits starts= lookups= our_texts= shifted=`). Custom credits music comes next with a song the user picks; the mod ships no copyrighted music.
- Survival after a game over (18.10., 4236088D, not installed):
  - The first wave never came after a checkpoint reload in arena_ch01: the run probed 2 s after the reload, before the npc_stream's vent spawners were registered, found no family and never probed again. While arming it now probes again every 2 s (at most 15 times) while vent spawners of the map are missing or nothing is available (`probe again #n: x of y vent spawners live, families available: ...`).
  - Vent spawns came in pairs every ~24 s after a few game overs: survival passes its clock without paused time to VentQuiet, whose stamps are GetTickCount. Since the review's signed age (17.10.) every vent waited the whole paused time after each spawn. VentQuiet now takes the age on its own clock.
- Enemy watch (18.10., F1F942E7, not installed; [Debug] NpcWatch=1): a foreign mod's menu spawns an unused zero-G tripod that half works and sometimes crashes. Twice a second the SimManager's objects are walked; every enemy that appears gets a line `npcwatch: new <class> <entity> (vtable) guid ... at (...) N m from Isaac, health, Isaac in ZERO-G/in gravity`. Tripods, bosses, brutes, guardians and the Ubermorph are followed every 2 s (position, metres moved, health) and get a `gone` line. At most 400 lines, 30 per ordinary class. The scan runs under SEH. Summary `npcwatch scans= new= gone= tracked= ... | <class counts>`. Note: bot_sense knows neither the plain Tripod (vtable 01B87970) nor the WindowTripod, so bots, marks and Kill all ignore them.
- The partner in cutscenes (19.10., user: "integrate a second player fully into the cutscenes", stages A to C; plugin 4EAF57CE, not installed; research analysis/COOP_CUTSCENES_2026-10-18.md):
  - A, `[ScenePartner]` (off by default): in a cinematic whose scene is a puppet scene, each partner's Isaac ghost is shown instead of hidden. It is parked beside your Isaac (SideOffset to his right, the next partner on the other side, BackOffset behind) and plays `Clip` (default unarmed_s_baseidle) on your screen only. Its gun stays hidden; nothing is sent.
  - How: the engine's own ghost switch for paired attacks (SetLocalAnimControlAndCacheNetRPCs 0x00EA51E0) caches the network animation, Player::EntityGhostTick 0x0086B940 is skipped for the puppet (no network position, no aim bones), SetMatrix parks it, Player::SetAnimation plays the clip, bone mods off, its collision off (PuppetCollision=0). At the end everything goes back and the network moves the ghost home.
  - The mode is per scene, by its name stem: `PuppetScenes`, `HiddenScenes`, `DefaultMode=hidden`. The template lists the 18 scenes without a confrontation. analysis/COOP_SCENE_LIST.md has all 38 with chapter, parts, cast and a proposal (tools/list_coop_scenes.py). Scenes like igc_03_03 (Daina's betrayal) stay hidden until they are reworked.
  - Log `scene_partner sequence X scene igc_..: partners stand by as puppets (why)`, `scene_partner puppet on/off ...`, summary `scene_partner scenes ... puppets started= released= gone= state_reapplied= restarts= ghost_ticks_skipped= faults= rejected ...`, `coop_cutscene ... puppet bodies shown= hidden_again=`.
  - The camera pull-back was built and taken out again (user: they edit the scene cameras themselves).
  - B, bank writer `tools\ds2_bnk.py` (our own code; the friend's DS2Import plugin only served as a cross-check). It reads and writes version 9 banks, appends actions, repoints a leaf to a new clip and makes new banks. Channels it writes: raw floats, u16/u8 quantized, constants. Channels it reads and does not change keep their bytes. `tools\test_ds2_bnk.py`: 96 player cutscene clips round-trip bit for bit, and the external decoder reads every bank we wrote. The exe takes every layout for any character, the appended layout is safe (owner sizes are stored, never read).
  - C, JSON and UE5 bridge `tools\ds2_anim_json.py` + `tools\ue5\ds2_export_anim.py`:
    - clips to ds2anim.v1 JSON and back, in native or UE coordinates
    - resampling to 30 fps; bones a document leaves out keep the bind pose
    - with `--replace` they keep the edited scene clip's own bones and its FOV, so a document with only m_camera is a camera edit; tested: body and FOV stay bit for bit
    - The cutscene camera is bone 4 (m_camera) of Isaac's clip; the FOV is the clip's extra channel in degrees.
  - C, into the game: `variants\anims\` (README there). `partner\coop_partner_<Isaac's clip>.json` go into one new bank. `scenes\<retail bank>\<action>.json` make an edited copy of a retail scene bank, with the edited action at priority 1.
    - `tools\variant_pack.py build` runs `tools\build_anim_banks.py` and appends the banks to the pool stream as BNK resources: retail header values, GUIDs of their own, character "player".
    - The engine attaches them to every player character by itself (the state table keeps the higher priority, so an edited scene clip wins whatever loads first).
    - In a puppet scene part with a `coop_partner_<clip>` state, the partner plays it in sync from the part's start, on Isaac's matrix at that moment, once (`scene_partner partner N: coop_partner_... plays`, `own_clips played= absent=`).
    - Tested offline: the probe clips in `variants\anims_samples\` (a partner copying Isaac in igc_01_03, a camera 0.5 higher) build, read back with both decoders, and sit in a scratch pool as two BNK assets. The real pool is unchanged; `variants\anims\` is empty.
    - Open in game: whether the clip's root motion moves a ghost (it is not authoritative), and the transitions of a scene copy being added twice.
- Supply Terminal fix (17.10. ~21:20, in the build after 61FE5F53): an order placed in a tick started a few ms after that tick's time, the age wrapped to 49 days, and every first gun was refunded at once as "its stream never came in 30 s". The stream kept loading, so the second order arrived after 0 ms (user: every gun had to be bought twice). Suits had the same wrap. Order ages are clamped now (shop::Age).
- Born in the right suit (17.10. ~21:10, 61FE5F53, not installed; [Necro] BirthSuits=1):
  - Player::Init (0x008A8270) dresses every new non-ghost Player after Restore with ChangeToUniform(this+0x5A0); that call returns to 0x008A8670.
  - Our spawns arm the wanted GUID right before Finalize. The ChangeToUniform hook swaps it in at exactly that call and writes Player+0x5A0 too. The body never wears the record's suit and needs no second change.
  - Who gets which suit: the watching Isaac gets the scene author's suit (no 3 s dress gate any more); Isaac back from the necro gets his own suit, the last one he wore as a human; a bot after its first body gets its own suit.
  - While a necro, his own suit is kept resident with the game's PlayerUniformManager::PreloadUniformStreams (0x00ED5E90; only the LevelManager listens for its message, and only during an MP load), checked every 10 s.
  - Report line `test birth_suit PASS/INFO/FAIL`; counter line `coop_uniform births ... preloads ...`.
  - The hooks only run once a partner is connected (as before), so solo bots keep the old way.
- Teleports and room lights (17.10. ~20:50, 58F328B5, installed ~20:55; analysis/SIMGROUP_INTERCEPT_2026-10-17.md, second pass):
  - Teleport follow ([Coop] TeleportFollow=1). Every machine watches Entity::SetTeleporting (0x007D0A80) on its own Isaac. That covers SimpleTeleport and scripted sequences.
    - A move of 10 m or more raises a serial, and the destination travels in the VISI packet (version 6, sent at once).
    - A necro's ghost camera follows that event and aims at the destination. The old 25 m position-jump guess is gone.
    - Report line `test teleport INFO`.
  - Light sync ([Coop] LightSync=1). AnimateLight::HandleEvents (0x00BADC50) reports each on, off, fade on or fade off message its lights take, by GUID.
    - Deduplicated by state, at most every 500 ms per light, 8 lights per 'LGHT' packet.
    - The partner hands the same message to its own copy, unless that copy already shows the state.
    - A light whose room is not loaded yet keeps the wanted state for up to 2 minutes and gets it once the room is there.
    - This is meant for the client's dark rooms: the trigger only answers to the local Isaac, and the mirror leaves AnimateLight alone.
    - Report line `test lights INFO`; counter line `lights taken=... applied=... dark=...`.
- Fixes 17.10. ~20:20 (302EA0CB, not installed):
  - Bot ghosts no longer die on a partner's machine. The host owns its bots' lives, so their ghosts get the same health floor and death-message swallow (a necro's spit had made them vanish on the client). Report line `test bot_ghost INFO`.
  - Nicole target guard. One of Nicole's states (message 0x22) placed her by her target's camera without a null check. A necro machine's own Nicole has no target, so that message is skipped until she has one (client crash 20:01, dump 11920). Report line `test nicole_target INFO`.
  - A necro partner no longer takes a bot's seat in an invasion (each necro spawn dropped bot 3 and the next vent visit brought it back).
- Scene kinds from the game itself (17.10. evening, 9F2CCA76, not installed; analysis/SCENE_KIND_2026-10-17.md):
  - Every machine reads what kind of scripted moment its own Isaac is in: cinematic, short sequence (the kinesis panel), QTE, stationary aiming (Tiedemann, the Tormentor), playable sequence (Nicole's dementia fight, the strait jacket) or none. It uses PlayerMasterSM and PlayerLowerBodySM states, the sequence flags, the button-mash prompt count, the behaviour and player flags and the active camera (gameplay or taken over). No timers, no names, no per-scene lists. Each change is a `test scene_kind INFO: <kind> (...)` line, and the report lists the kinds per copy.
  - The kind travels in the VISI packet (version 5) and goes out at once when it changes. A human in a cinematic, short sequence, QTE or aiming part holds the necros' vents; a playable sequence does not (the finale exception). A necro watches only a cinematic and its QTE/aiming parts (a cue that starts a short or playable sequence leaves him a necro), and the watch ends when the author's scene becomes playable. The 40 s / 25 m rules are gone.
  - Normal co-op too: the cutscene module hides partners only in a cinematic, QTE or aiming part; a short sequence hides nobody, a playable sequence shows them again.
  - Suicide row (user): a living necro in an invasion story game has "suicide" in place of the pause menu's friends list (the text table entry of `$ui_viewfriends_mc`, sdbm 0xFF1A4390, points at our word while he is one). Selecting it sends him into the vents the MP way (the forced re-entry), refused in a paired attack; `test suicide PASS/REFUSED/FAIL`.
- Test 17.10. ~19:00 (DC9FA10D: the menus hide now), fixed in 1ED15B09, F7C09372, 2088CD8F and 169360BB (not installed; 2088CD8F also counts a scene cue played more than 1.5 s late as `test watch_cue FAIL`):
  - Aiming and shooting at Tiedemann did not count as a scene (user): the ending (a sequence, then shooting from one spot) ran past 40 s, the 40 s rule took it for Nicole's fight, ended the watch and let necros spawn. The played-scene exception now also needs the human to have walked 25 m in the scene (sampled once a second, a scene's teleport not counted); the mirror's finale flag alone no longer lifts the scene hold or ends a watch (169360BB).
  - The scene watch broke with the leader's suit (user): the respawned Isaac's body never counted as whole, so every watch waited the full 2.5 s before its cues and the scene started about 3 s late on the necro's machine (cues played 2953 and 3203 ms after they came, 330 ms before the suit). `[Necro] WatchSuit` is off by default now; with it off nothing waits.
  - The human was invisible on the client during the insider fight: the necro's machine watched it as Isaac and followed it as a scene, and a follower hides its partners. "insider" was first added to the finale scope, but the level data (17.10. evening) shows "insider" is the Tiedemann fight (bossInsider, mtb_insider_fight); Nicole's fight is the dementia part. The addition was taken out again before any install. Kept: the finale scope is checked on every dispatched message (a replayed one and a watching necro's too). A follower hiding its partner for a whole boss fight is a job for the scene classifier (in work).
  - The direct spawn's position test ran 1.5 s after the spawn and put back a necro the player had walked 5 m: it runs after 0.4 s now and acts only on a jump of more than 6 m (the game's own placing jumped 10 m and more).
- The necro HUD's "hide" was a show (17.10. ~18:55, found live, fixed in DC9FA10D, installed ~19:00): 0x01085C90/B0, taken for the MP12/MP13 hides since 06.10., are byte for byte the show wrappers (GetUIManager 0x0070D2F0, the tag, 0x0071C500 = make the screen if needed and its virtual +0x4C). Every hide the plugin did (out of the ghost, before a restart, the HUD heal, the sweeps after a load) showed the type menu and the vent HUD again: the menus on Isaac, after restarts and on the direct-spawned necro, with their input. The real hide is the by-tag one (0x01072DE0/F0 -> 0x00A38A40 -> 0x0071A3A0, virtual +0x50); `HideNecroHud` uses it now (through the by-tag wrappers' originals, else directly after a byte check). Whether a screen is open is read from the screen itself (state at +4 not -1 and its movie at +0x2C), not from the bytes at +0xA03C/+0xA374 (0 with the menus open).
- Test session 17.10. ~18:40 (B263DEEB), report `build/reports/coop_test_20261017-1842.md`, fixed in E54CA74D (not installed):
  - The direct-spawned necro kept the vent HUD and its input ("sees the vent menu although spawned, stuck"): the HUD rule asked for Isaac (`!playerGone`), but a living necro has `playerGone` set too, so it never fired. Now the HUD (our mask or the screens' own shown bytes) may only stand with the ghost proxy; the hide runs before the shown bytes are cleared (it may test them).
  - Scene watches were skipped in all of ch11 (`watch_skips=11`): the finale test took any shown "dementia" stream, and that stream is shown in the whole chapter. The finale exception is now the mirror's finale flag or a human scene that has run 40 s on end (the dementia fight is one sequence of minutes); a watch of such a scene ends there ("played, not watched").
  - The respawned Isaac of a direct spawn never reached the spot (he stood beside the host 1.5 s later, in view): the transform runs at once with the spot's matrix again.
  - The life machine observes without the level's settle timer (it restarts at every body change and blinded the machine for 4 to 5 s); its acting rules still wait for a settled level, and an empty slot 0 before a level exit is no BROKEN.
- Necro life state machine and the test report (17.10. evening, B263DEEB, installed ~18:40; user: "one state machine instead of scattered transitions" and "an automatic test report"):
  - Every tick the necro machine's state is read from the game itself: human, living necro, dying necro, ghost at the vents, watching as Isaac, direct spawning, or BROKEN (nothing to control). Every change is one `life: A -> B after N ms` line; the action paths (transform, revert, out of the ghost, the ghost appearing, forced into the vents, direct spawn) announce what they expect, and an expectation not met within 6 s is a failure.
  - Rules checked every tick, healed at once and reported: nothing to control for 6 s (Isaac comes back where the necro was, a necro by choice joins again); a ghost proxy beside Isaac for 2 s (dropped, HUD hidden); a necro by choice standing as a human for 10 s (joins again); a watch without Isaac in slot 0 for 6 s (dropped); a cached necro that is no necro (forgotten); a dead necro without a ghost for 20 s (reported); the necro HUD without a ghost (hidden).
  - The direct spawn waits for the respawned Isaac at the spot (or 1.5 s) before the transform, and 1.5 s later checks that the necro still stands within 3 m of the spot; moved away, it is put back once.
  - Machine-readable result lines `test <scenario> PASS|FAIL|FAIL healed|REFUSED|INFO: detail` for vent_spawn, direct_spawn, death_to_ghost, rejoin, watch_cue, watch, hud, spawn_cue, life and finale; the log's second line is the session's start time.
  - `python -I tools/coop_test_report.py` reads every copy's log of the session, prints PASS/FAIL per scenario, the failures with detail, refusals by reason, each copy's role and life timeline, the last counters, problem lines and the crash dumps since the session began, and saves it under `build/reports/`. Log `necro life state= changes= expect_misses= | heals ...`.
- Direct necro spawn and the finale exception (17.10. evening, 55723D43, installed ~18:15):
  - Versus-like direct spawn (user: "like L4D2 versus, spawn around the corner, not in view, at least 7 m away"): the ghost at the vents flies to a spot and presses `[Necro] DirectSpawnKey` (81 = Q). The floor below the ghost is the spawn when the respawn timer is over, no human plays a scene, the floor's room is shown here, a necro fits (2 m up, not boxed in at the sides), no human is nearer than `DirectSpawnMinDistance` (7), one is within `DirectSpawnMaxDistance` (60) and no human's eyes reach the spot's feet, chest or head. Isaac comes back there for a moment (EscapeGhost, as for a scene watch, while the role stays "at the vents") and turns into the necro (Transform) on the next ticks: no vent, no exit animation, facing the nearest human, the type the type menu approved (else [Necro] Type; a type whose stream is missing falls back). The HUD line says `[Q] SPAWN HERE (UNSEEN, 7 M+)` or why not (`NO SPAWN HERE: SEEN`). `DirectSpawn=1` by default (asked for). Log `direct spawn: ...`, `necro direct_spawn asked= done= failed= refused timer= scene= floor= room= tight= close= seen= far= no_los=`.
  - The finale's dementia arena is an exception to the scene hold (user): each human's dementia fight is one long sequence on his Isaac, so necros waited in the vents the whole fight. With the mirror's finale flag or a shown stream named *dementia* here, the humans' scenes neither hold the vents, nor force a living necro back into them, nor start a scene watch. `finale arena entered/left`, `| finale= stream_hits= watch_skips=`.
  - Fixed in 705C85CF (not installed): the first direct spawn put the necro at the checkpoint's spawn point and he could not move (the respawned Isaac still stood there, RespawnPlayer places him later, and Transform read his matrix 16 ms after): the spot's matrix now goes to `Transform(at)` itself; the done line logs where the necro really stands.
  - Loud spawn cue (705C85CF): a partner's role turning "living necro" from the vents or from a human makes his necro body on the other machines roar through the entity's PlaySound(hash, NULL opts) (vtable +0xA4, 0x007D1610): pack `pack_attack_grondo.emx`, lurker `vx_lurker_attack_grondo.emx` (retail's "enemy spotted" roar), spitter `vo_slasher_aggro_attack.emx`, puker `puker_attack_01.emx`, all from the MP necro character banks; `[Necro] SpawnSound` overrides, `SpawnCue=0` turns it off. Log `spawn cue: ... roars X: playing`, `necro spawn_cue asked= played= silent= no_body=`.
- Two-copy test 17.10. ~17:40 (plugin A4212912), fixed in 816EF3E9 (in 55723D43):
  - Scene cues that came while the watch's Isaac stood but was not ready yet (net proxy still flagged, suit changing) were dropped at once, the watch ended after 6 s without a scene and the necro went back into the vents mid-scene: they now wait while the watch runs (`WatchCuesHeld` includes Watching; the queue and drop lines say why it is not ready).
  - The vent HUD stood on Isaac after a checkpoint restart: the hide before the restart is lost with the world, so it is done again twice once the next level is settled and no ghost is up (`hud: ... hidden after the load`, `hud_sweeps=`).
  - A necro made by the pool-on-demand way (the join, the key with PoolMode 2) was not "by choice", so a restart left him a human: it is now.
  - Teleports (ch11's insider scene, the Tiedemann fight, puts Isaac in an arena far away): a human further than 25 m between two samples recentres the ghost camera at once, without the cooldown; when the only vent with its room shown is beyond VentHumanRange, the camera goes to the human and the vent stays selected (`invasion: partner N jumped X m`, `teleports= teleport_recentres= views_at_human=`).
- Two-copy test 17.10. ~17:00 (plugin AB4BD692), fixed in 819357FF, 9DAF0573 and A4212912 (installed 17:45):
  - A necro watching a scene as Isaac wore the patient suit (the respawned body comes from the player template): once the body is whole he changes into the suit the scene's author announced (suit sync; another partner's if the author's is unknown) through the native ChangeToUniform, and the scene's cues wait for it (2.5 s per step at most). Skipped when the suit is not registered here or is an MP suit whose skin is missing. `watch: Isaac X changes from suit A into partner N's B`, `necro watch_suit dressed= kept= skipped= timeouts=`.
  - The ghost's speed never changed: the retail RespawnProxy template has 4/7 m/s, not the 4/6 the tune expected ("template speeds 4.00/7.00 ... left alone"). The speeds found on a template are now its base (a value above 12/15 m/s is never taken as one), and `[Necro] GhostSpeed` defaults to 6 (user: "much faster, speed 6"; at most 10): 24/42 m/s.
  - The necro at the vents could barely act: no vent passed our filter (the humans were more than VentHumanRange from every vent), so the vent the game had selected failed its check every frame, AutoSelectRespawnVent ran again and again (23653 fallbacks), the camera jumped and the spawn key found no vent. Now the vent the auto pick hands out stays allowed (`keptVent`), the game's own pick runs unfiltered, and a third pass takes the nearest vent whose room is shown here when none is near a human. Log `necro vent_pick kept= kept_passes= game_picks=`; the pick note comes once per 2 s.
  - Isaac with the necro type menu and the vent HUD on screen, which took his input: the ghost's own teardown leaves both up, and the way back to Isaac for a scene (EscapeGhost, also F11) did not hide them. It does now, and any other way out is caught after 1.5 s (`hud: ... stayed up without a ghost`, `hud_orphans_hidden=`).
  - A host restart during a scene watch left the necro by choice a human for good: a restart or level change during a watch brings him back to the necro team afterwards.
  - The necro got no checkpoints: a necro defers the host's checkpoint (slot 0 holds no Isaac), so the host's restart loaded our older one. A restart followed while a checkpoint waits marks a reload; once Isaac is back and the host's checkpoint is applied, the client restarts once more to load it, and the necro join waits for that (a minute at most). `coop_checkpoint necro_reloads marked= done= expired=`.
- Two-copy test 17.10. ~16:50 (plugin AB4BD692), fixed in 9C7EFF28 (not installed):
  - A client started by a DS2COOP launcher was pulled into the host's story level on its own, before its intro, and could not choose to come as a necro: a client follows the host from the main menu (and a launcher's client in its own game, and its session start) only after "join co-op" (`level::joinArmed`); once chosen it also follows a host who starts later. A launcher's client may now press "join co-op" (it waits for the host's level instead of being refused); a necro join waits in the menu as long as the join does. Log `coop_level the host plays X: not followed until join co-op`.
  - "Settings differ from a partner: state survival" while the second copy was still in its intro: a copy sends its settings list only once its own survival state is known (at most 60 s after start).
  - The host's door waited for a partner who had quit to the main menu (his side channel still lived): a client in the main menu says so once a second (level packet flag 2), and gate::Required does not count him (`coop_level partner X is in the main menu`).
  - A necro could climb out into a room that was loaded but not shown on his machine (user: "the unloaded room had collision"): every cast hit of the vent probe now has its entity's stream checked (+0x38, status 3 and visible flag 0x100); a hidden stream in front of the opening (the floor below the way out, the wall ahead) refuses the vent. `necro vent_rooms ... front_hidden= hits_hidden= hits_unknown_stream=` and per vent `hit streams shown= hidden= unknown=` show whether the stream of static geometry is found.
  - Stream sync for the living necro (user: "synced only after the necro respawned"): only the ghost at the vents followed the humans' rooms. Now a living necro's machine merges the humans' current sets (area and pending, whole, shown) into every switch of its own zone set (pin::OnSetDesired), and switches its own set again when a human's set changes and is not covered (UnionTick, at most every 2 s). Log `coop_streams living necro: N room(s) of the humans' sets loaded`, `... our set switched again`; a list beyond 46 rooms logs FULL. Plugin E583E0CD.
  - Open: lights missing on the client in one room (streams of both copies were the same afterwards; probably light switches fired before the client arrived, MultiManagers are not mirrored); the living necro's machine follows only its own rooms, the humans' only as a ghost at the vents ("synced only after a respawn").
- Necro types in the story (user 17.10.: with the sixth type configured only the configured necro's character stream was loaded, so another type picked in the menu was refused the first time): while a necro is in the game all four MP necro character streams (pack, lurker, spitter, puker) are loaded, the sixth type's two streams first, then the others one at a time, each only while at most 28 of the stream manager's 32 TOC slots are taken (the rest stay on demand; the menu approves only loaded types, crash 52780). Log `model stream X asked for: a necro is in the game` with the TOC count, `necro menu_streams ... all_four_asked= models=`. Plugin AB4BD692, not installed.
- Necro ghost outside the MP maps (user 17.10.): `[Necro] GhostSpeed=2.5` multiplies the RespawnProxy template's walk/run speed (+0x60/+0x64, authored 4/6 m/s; written only over the authored values), `GhostNoclip=1` sets the ghost's CMC to COLLMODE_NONE (CharacterMovementComponent::SetCollisionMode 0x00F712A0) while its movement mode has no gravity (MOVEMODE_NOGRAVITY 2 or 6), so it flies through walls. Log `ghost X: speed`, `ghost X: noclip`. Plugin AB4BD692.
- Corpse and body part limits (`[Lifetime]`, user 17.10.: survival on Marker Labs fell from 143 to 31 fps): the frame rate followed the bodies in the world (25 in a wave break with none alive: ~55 fps). The game keeps 5 corpses, 10 body parts and 20 trash pieces (EnemyLifetimeManager [0x02021350], ResetPopLimit 0x01003BF0); MarkerPatch's DINPUT8.dll detours ResetPopLimit and swaps in its IncreasedEntityPersistence values (25/96 here). The plugin calls the original past the detour (its 7 entry bytes rebuilt in a stub that jumps to 0x01003BF7) and sets the limits again whenever they differ: Mode* on MP maps and in campaign arenas (10 corpses, 32 body parts), Story* in the campaign (0 = MarkerPatch or the game decides). Log `lifetime <kind>: limit 25 -> 10`, `lifetime sets= reaped=`. Plugin BF32A6DE, not installed.
- Direct spawns behind a locked door (user 17.10., Marker Labs: an Exploder 6 m away behind a door): engine 32CB336D-B95CF854 put its enemies back onto its own spot behind the door; after two such placements it is "not used again this run", but PickEngine asked that list only for the area rule picker, so the named engine was used for five more. Named engines skip avoided ones now. Plugin BF32A6DE.
- Debug menu straitjacket (user 17.10.): a jump past chapter 1's straitjacket rooms kept Isaac and the bots in PLAYERSTATE_STRAIT_JACKET (player flag 0x26 at Player+0x3DC, PlayerChangeState::Activate 0x00864380 case 2); the flag outlives a level load. After a jump to any other checkpoint the plugin clears it with ClearPlayerFlag 0x0085F790 on every player for 15 s (`debug straitjacket jumps= kept= cleared=`). Built since 39D2E1EE, not installed.
- Bots (user, 17.10. 14:25, ch11 story): they kept walking to the same spots and did nothing there: one corpse 13 visits, two lockers 6 each, 0 stomps and 0 presses (a locker needs the game's use prompt on the bot, which never came; each bot gave up after 9 s and came back after its own minute). A spot is now given up for the level after 15 s of squad visits without a stomp or a press (`bot: <kind> X given up for this level`, `given_up=` in the loot line). The heal decision never got the med pack count (always "unknown"), so a bot below its heal mark pressed the med pack key every minute without one and dropped a kinesis object for it; it reads its med packs every 500 ms now. Plugin 9C9C9402, not installed.
- Rename (user, 17.10.): HaferSprawl is Dead Space 2 United now, files and identifiers DS2Coop (plugin `DS2United.asi`, settings `DS2United.ini`, configurator `DS2UnitedConfig.exe` with a new CO-OP MOD logo, build `build_ds2coop.cmd`, macro `DS2COOP_MODULE`, namespace `ds2coop`, sources `ds2coop_main.cpp`/`ds2coop_config.h`). In game: main menu title "/Co-op", title screen line `[Menu] TitleLine=Dead Space 2 United`, the logo's steel line CO-OP MOD (frontend build/frontend_final7, CRC 5867D82D, made with `tools\build_title_logo.py --restock` from the installed final6; only lgo_ds2_02 changed). Every file in `src\` names Hafer as the owner in its first line; `tools\stamp_owner.py` (run by the build) puts the line back into generated headers. The standalone co-op half now builds into `build\coop\`. Installed 17.10. (tag 20261017c: plugin 4DBFD421, configurator ACBDABF0, frontend 5867D82D, backups .bak-20261017c; DS2United.ini CRC BA7BDE13 with ZombieChargeIgnoreVis=0 and ChargeUnseen=0). The install renamed `HaferSprawl.asi`/`.ini`/`Config.exe`, keeps the old co-op-only `DS2United.ini` as `DS2Coop.legacy.ini` and sets the `[Menu]` title and NAT lines.
- Fair charging (user, 17.10.: enemies charged him while he was not even looking at them): our own switches did it. `ZombieChargeIgnoreVis=1` set the slasher templates' ChargeIgnoreVis byte, so `Zombie::SetChargingMode` skipped its "on the target's screen" test (IsOnTargetsScreen 0x00FE4960, the view frustum), and `ChargeUnseen=1` lifted the sight test of UpdateApproachTarget in the arenas. Both default to 0 now (plugin 041D8165, installed 17.10. in 4DBFD421; DS2Survival.ini template too): the enemies still know where the players are (ForceAware) and walk up, a charge starts only on screen and in sight, as in the game. Kept: `ZombieChargeFloor=0.9` (only slasher_doctor/hospital had 0 and never ran) and `MaxChargers=2` (the game allows 1). The installed settings file holds `ZombieChargeIgnoreVis=0` and `ChargeUnseen=0` since the install.
- Test 17.10. ~02:20 (plugin 8BC6AC1C), fixed in F3515098 (installed 17.10. in 4DBFD421; contains F0D05342):
  - Transport Hub (arena_ch05): enemies came in the wave break (user). The level's own vent spawners (npcs_sprawl_hub_return, a Puker spawner) fired, and more_enemies copied one three times. While a run holds a campaign arena only the director spawns: OnLookup empties the packet of every level spawn whose target is a catalog enemy (props and pickups still come), so no entity and no more_enemies copy; extra spawns and deferred repeats are refused the same way. The flag clears at run end, on a loading screen and on another map. Log: `arena run N: the level's own enemy spawners are refused`, `arena run: level spawner X refused its own <family>`.
  - Stalker Room (arena_ch04): nothing spawned for long stretches (user: "vents preferred?"). Not the vents: the direct path had no slasher left. In the Transport Hub the area rule picker had taken an arena vent spawner as the direct engine; its vent contract refused all 15 slasher donors (bind 206 WrongVent), three times each, and the donor failure count was kept for the whole session. Now a refused vent contract retires that engine for the run instead of blaming the donor (`engine X: its vent contract refused donor`), donor failures count per run, an item whose direct path has no donor or engine goes to a vent at once (not only on the 40 % vent roll), and the arena vent cap of 20 m holds only for an item's first 4 s (the Stalker Room's two vents are 29 m from its far end).
- Open: host crash 19376 (21:14:49): its own Isaac's iMsgPostSpawnInitialize (0x008A8270) found the PlayerSettings singleton [0x02087C38] (entity 22351260, vtable 01B271B0) with GUID 0 and an empty attribute +0x1F4, looked that GUID up and read through the null result (0x008A833A). The session's log was rotated away before it could be read.
- Crash at 0x004EAFB2 again (16.10., dump 21076, ch09 wave 46): Havok's refill found no memory, 45 MB free address space, largest block 0.8 MB. The game ran as deadspace2.exe, not the large-address-aware deadspace2_4gb.exe (DS2_Start_4GB.cmd).
- Map picker pictures of the Outbreak maps (user, 15.10.): Mark's two pictures (MP20 pages 4/5, DXT1 256x256) are a card from his UI (photo in a cyan outline with a tab, rows 36..219) on the stock 2:1 quad, so they showed twice as wide and did not match the frame. tools/fix_mp20_previews.py crops the photo inside the outline to 2:1 and stores it squeezed into the square page; only those 65536 bytes of MP20 change (frontend final5 87FEF2BD from final4 152061B6).
- Pause menu locks (user, 15.10.): tutorials and the friends list are always locked; the checkpoint restart is the host's (and a solo player's), a partner sees it locked (SP10PauseMenu DoUpdateItemAndLabel 0x00A69210 gives the Hardcore lock state, OnInput_Select 0x00A98D90 refuses). `[Coop] PauseMenuLocks=1`. Fix 15.10. evening: the row is the third argument of DoUpdateItemAndLabel (data, row, state); the first build locked no row, only the select.
- Crash at the level exit (15.10., dump 26564, after five checkpoint restarts on mp_bombRun): the Apt movie registry (AnimationManager, 192 slots of 0xE0 at [0x0204199C]) had been rebuilt while 56 loaded movies kept pointers into the old, freed one; an unload (AptResourceHandler::UnloadResource 0x007090F0, InitAptInfo(-1)) wrote -1 into the freed block and broke the general heap's free list. coop_crashguards now leaves such a stale slot alone and logs every registry rebuild.
- Last enemy on Concourse (user, 15.10.): a direct Exploder was put back four times by its engine onto the engine's own spot (-5.5 -3.5 32, out of reach) and stood there 54 s as the wave's last until the stuck watchdog. On an MP map such an enemy is now killed out of sight and queued again after the fourth placement when no living player is within 12 m (as in the arenas). The last-enemy marks log every change with the reasons an enemy is not marked (`wave N: L left, M marked (... not marked: ...)`).

## Changed on 2026-10-20 (build D9AA66FE, installed 20.10. 23:25)

From the 20.10. evening survival run in the ch09 mining entry hall arena (user reports and DS2Bots.log):
- Bots, combat leash in three bands:
  - Within 6-10 m it walks back shooting, as before.
  - From 10 m to 22 m it comes back aimed: a 3 m step toward the leader and 1.4 m to a side that changes every 0.9-1.8 s, away from its target first and around other enemies. It shoots all the while (user: "shoot while running back, backwards, sideways").
  - Beyond 22 m it runs with the gun down until 16 m, even with an enemy on it, unless one holds it.
  - Pack children had kept the bots swinging 40 m behind him. A fight also gives more patience before a walk counts as stuck (8 s instead of 4, 10 s instead of 7).
- Bots, catch-ups: the spot by the leader is snapped onto the NavPower mesh and taken out of the camera's view when there is one. Before, a spot beside him off the mesh got the bot stuck again at once, and it popped up in plain sight. Log: `bot leash ... catch_up spots hidden= in_view= off_mesh_fallback=`.
- Bots, the Ubermorph: one down and regrowing is no target and no threat. No gun is raised at it until it stands again.
- Bots, desperate mode (user: "if the bot itself is badly surrounded, the rules for being too close to enemies or mates fall away, except when the player is near"):
  - It triggers with three or more enemies within 3.5 m and no human within 5 m. Other bots no longer count.
  - Order: the contact beam's slam, the force gun's push, the pulse rifle's grenade at its own feet, a detonator mine at the feet of the one in front, the flamethrower.
  - The stasis and the line of fire only check humans then.
  - Its own blast is refused on the bot for 4 s, and the mates are spared as before (NoBotFriendlyFire).
  - Log: `desperate frames= blasts= held_for_human= self_spared=`.
- Bots, no gun swapping in a crowd: surrounded, a gun in hand that can clear the ring stays (contact beam slam or charge, force gun push or beam, flamethrower, pulse grenade, detonator, ripper). Any change waits 2.5 s. The push takes a 20 degree cone. Harlan had swapped contact beam and force gun about 1000 times with three twitchers on him. Log: `crowd_gun_kept=`.
- Arena matches block the campaign's own hallucinations: a message whose name holds dementia, nicole or hallucin is dropped while an arena runs (`[Arena] BlockHallucinations=1`, log `arena: campaign hallucination ... blocked`). One had started in the church crypt; the ch03 set pieces lock a door while Nicole shows.
- `[MapFamilies]` (user 21.10.): per map the enemy kinds that may come.
  - `<map>=slasher, puker` allows only these; `<map>=-brute, -ubermorph` allows all but these.
  - Player names work (slasher, pack, swarmer, leaper, stalker, puker, lurker, exploder, infector, tube, brute, pregnant, divider, ubermorph), and so do the game's names.
  - Waves fall back to the stand-ins. The availability log says `[MapFamilies] leaves it out on this map`.
- Map descriptions in the map picker follow the mode: Objective keeps the game's versus texts; the other modes get a line about the place and one about the mode (arenas and Outbreak maps a 63-character line). Log: `gamemode descriptions ...`.
- Doors held open by an arena were checked (analysis/DOOR_NAV_BLOCK_2026-10-20.md): UnblockGraph runs when they reach state 5, so the hold is not the necro-only wall. The likely cause is a necromorph standing still in the doorway: after 1 s it becomes a NavPower obstacle (NavSmartObstacleComponent).
- Survival escalation, the user's new values (20.10.), now the code and template defaults:
  - `EnhancedPerWave=5`, `HealthPerWave=3`, `SpeedPerWave=1` (max 40).
  - `PauseStepSeconds=0.5` (a second shorter every two waves, min 8 s); the key now takes decimals.
  - `RewardStepPercent=2.5` (min 40 %); the key now takes decimals.
  - `PriceStepPercent=0`: shop prices no longer scale.
  - `BotDamageStepPercent=1` (min 50 %).
  - The user's own INI got the same values; its backup is `DS2United.ini.bak-20261020-escalation`.
- Survival escalation caps (wave 23 measured live: every slasher an oracle twitcher with 623 health, 22 per cutter hit): `[Escalation]`.
  - `EnhancedMax=60`: the enhanced share stops at 60 %.
  - `HealthMax=60`: health and limb health stop at +60 %.
  - Oracle twitchers make at most `OracleShare=35` % of the enhanced slashers and `OracleMaxAlive=3` at once; super slashers take the rest.
  - Log: `survival severed ... oracle ... capped= share= max_alive=`.
  - Mercenaries, Objective and Hold the Zone (26.10.): `ClockStagesPerWave=3` stages count as one escalation wave and their enhanced chance stays at or below `ClockEnhancedMax=25` %, the boss stage's `Enhanced=50` included. Before, every stage counted as a wave, so a few minutes in most slashers came as super slashers or oracle twitchers. Rewards and health use the same escalation wave; the stage log line prints it.
- Survival, vents: a vent whose arrivals reach nobody is retired for the run after its second failure. In the ch09 mining arena the vent 5ACA0989-B86CF919 dropped Pukers and slashers into a pocket behind the arena's locked door 5A4AF1F3, where only their marks showed (analysis/NAVPOWER_FORMAT_2026-10-20.md).
- Survival, nav report: when the watchdog writes an enemy off, it asks NavPower where the walk toward the nearest player ends (DS2Coop_NavPathEnd) and logs the gap. Log: `nav ... m short`, `survival placements ... nav reports= reached= short= no_path=`.

Later on 20.10. (build E0D09616, installed 21.10. in 1912821E, untested): the game's own difficulty for the game modes and co-op, `[Difficulty]` (src/runtime_coop_difficulty.inc, src/coop_difficulty.h, analysis/DIFFICULTY_NATIVE_2026-10-20.md).
- Picker (`Pick=1`): select in the map picker opens the game's difficulty screen (SP02). Accept starts the match with casual, normal, survivalist, zealot or hard core; back returns to the picker. With the native lobby the difficulty screen closes first and the lobby screen opens from the picker.
- `Pick=0` does not ask; `Pick=2` is kept for a difficulty row in the map picker (not built, the screen asks until then). A picker UI calls `difficulty::SetPick(row)`; a held pick skips the screen, `difficulty::PickName()` gives its label.
- Hard Core (`AllowHardcore=1`): the row is open in this route. The game only syncs the unlock on the story screen, so it was always locked here.
- Matches (`LockInMatch=1`): in survival and the other modes the pause menu's difficulty screen refuses a change on every machine, the host too. A hint card says why and the screen goes back to the options.
- Story co-op (`Sync=1`): the pause menu still changes it. The host's value goes to everybody on survnet kind 13 with a generation number, three times on a change and every 2 s. A partner's change goes to the host as kind 14 and comes back as the host's next generation, so a refresh never takes back a newer change.
- Before the level loads: the lobby launch (lobby version 4, an older joiner gets the build refusal) and the campaign join packet carry the host's value. It is written right before the level request, so the new level's ammo pickups are scaled with it.
- A survival visit from the campaign writes the campaign's difficulty back on the way back.
- HUD (`Hud=1`): `WAVE 4  -  11 LEFT  -  ZEALOT`, `SURVIVAL  -  ZEALOT  -  FIRST WAVE IN 10 S`; the other modes' lines too when it fits. The host panel line gets `D3`.
- Spawner rules: the difficulty scoped rules now follow the live value (0x01F6031C), not `Game.PDiff` in settings.txt, which only records the best finished campaign.
- `Probe=0`: with 1 the damage taken scale of every hit and the ammo scale of every pickup are logged (`difficulty: probe`).
- Log: `difficulty sig`, `menu slot SP02 ... (difficulty)`, `difficulty: SP02 push/show/pick`, `in-game SP02 pick ... REFUSED`, `host change`, `host sends`, `client change ... request`, `launch value from`, and the `difficulty global=` line when anything changes.

Later on 20.10. (build CC6E42F3, in 1912821E with `Pick=2` in the user's INI; frontend 7C877270 crashed the game on opening the map picker and was rolled back to 5867D82D on 21.10., the fixed frontend BC3CC980 is not installed): the difficulty row in the map picker, `[Difficulty] Pick=2` with `[Menu] NativeModeRow=1` (analysis/MP20_THIRD_ROW_2026-10-20.md).
- The game setup screen gets a third row "difficulty" under game mode and game map. Left/right step casual, normal, survivalist, zealot and hard core (hard core with `AllowHardcore=1`); up/down move over three rows.
- The row holds the pick it shows, so select starts the match with it and the difficulty screen (SP02) is skipped.
- It needs the frontend with row m2: `tools/mp20_third_row.py` turns the installed `randomizer\ds2coop_frontend.str` into `build\frontend\ds2coop_frontend.str` (only the MP20 movie changes; the description, picture and bottom bracket move down one row, `--lift` recentres the panel). `build_frontend_override.py --third-row` does the same on Mark's movie.
- Without m2 in the movie, or with `Pick` other than 2, the screen keeps its two rows and SP02 asks as before. With `Pick` other than 2 the m2 slot stays empty.
- Crash 21.10. (dump 2272): the movie loader asserts `nConstTableIndex == *pnCurrentConstantIndex` (AptActionInterpreter.cpp:314): the push lists and constant pools must name the constant table in order, and the two new entries sat at the end. `tools/mp20_third_row.py` now renumbers the table to load order and refuses a movie that is out of order (analysis/MP20_THIRD_ROW_2026-10-20.md, section 6).
- Log: `native row: movie scan ... with the third row m2`, `native row: screen ... -> native rows with difficulty`, `native row: right on row 2 of 3, ... difficulty normal -> survivalist`, `native_mode_row=... | rows= ... difficulty_steps=`.

21.10. (installed 21.10. in 1912821E, untested): the invisible wall for necros in arena_ch10 (user: "something invisible blocks live necros, corpses come through").
- **What the wall is:** a ScriptableProp, 5ACB6EF0-E1930623 from the security_checkpoint_3 stream, stands in held door 5A9A7D82. Its Havok filter blocks NPCs only.
- **How EA's filter works:** the filter info points to a DetailedFilterInfo (type, flags). Two objects collide when each one's type is in the other's flags.
  - Isaac and the bots are type 0x10000.
  - Live necros are type 0x20000.
  - Ragdolls and body parts are props, type 0x04000000.
  - This prop has flags 00420101: the necro bit is set, the player bit is not.
  - Read live with the game running, not from the files. The same arena has three more such bodies, all away from its doors.
- **The fix, `[Arena] OpenNpcClips=1`:**
  - Every 3 s the arena walks all Havok bodies.
  - A body that blocks necros but not players loses the necro bit, if it stands within 2.5 m of a held or inner arena door.
  - Locked doors are skipped, and so is a filter record that several bodies share.
  - Havok keeps overlaps it already has: a necro standing inside the blocker at that moment stays blocked.
- **Log:**
  - `arena ...: necro-only collision opened at door ...` gives the body, owner GUID, filter and flags before and after.
  - `... left alone, N m from the nearest usable door` and `... NOT opened, its filter record is shared`.
  - `arena npc_clips on= passes= bodies_walked= npc_only_now= opened= reopened= shared_skipped= away_from_doors= faults=`.
- **The navmesh was not the cause:** all four held doors of that arena have the same 15-30 cm strip, which is the door width shrunk by the agent radius.
- **Held doors and the nav graph, `[Arena] UnblockHeldDoors=1`:** the door's own open entry (4 -> 5) already reconnects its links and calls UnblockGraph (verified 20.10., analysis/DOOR_NAV_BLOCK_2026-10-20.md). The arena now reads every held door's state, links (flag 0x200) and NavPower obstacle each pass and logs every change (`arena door ... nav: state 5/5, links connected, obstacle gone`). A door open 5/5 for two passes with its obstacle live or links cut gets OperateOnGraphConnections(false) and UnblockGraph (0x00DC5950, 0x00DB7E30, both signature-checked). Log `arena door_nav unblock= sigs= held_checks= open_but_blocked= unblocked= unblock_failed= no_component=`.

21.10. (installed 21.10. in 1912821E, untested): enemies alive at once per arena (user: "15 is too many for one player in the security checkpoint; on the MP maps it is enough").
- **The new line:** `maxalive=<1-30>` in `arenas\arena_*.txt` replaces `[Survival] MaxAlive` there.
  - Each further player, bots included, still adds `MaxAlivePerPlayer` (3), up to `MaxAliveCap` (23).
  - A wave's own `MaxAlive` counts only when it is lower (wave 10's 14 becomes the arena's value).
  - The debug menu's Max alive still overrides everything.
- **Starting values,** estimated from each recording's walk (3 m cells walked): tripod room 10, church crypt 13, church sanctuary 14, stalker room 9, sprawl hub 10, Ishimura 15, mining entry hall 12, security checkpoint 10, marker floor 12. A rebuild is needed after a change (`tools\build_arenas.py`, then the plugin).
- **Log:** the wave start line shows `cap=N (one player M from the arena's maxalive=|the wave's MaxAlive|MaxAlive, P players)`.
- **The same rebuild also takes in `exclude=` lines that were never built before:**
  - Brute and Leaper are left out in church crypt, Ishimura and security checkpoint.
  - Brute is left out in church sanctuary and the mining entry hall.
  - Until now only the church crypt left out Brute.

## Changed on 2026-10-19 (installed 20.10. 23:25 with D9AA66FE)

- Overlay audio (user, 20.10.: "nothing from the overlay while the game is not focused, and the volume should follow the game's settings"; build 0BE51A48, not installed):
  - Our XAudio2 sound (voice packs, Dementia) fades out in 150 ms while the game window is not focused (`[Voices] MuteUnfocused=1`).
  - On top of `[Voices] Volume` it follows the game's own sliders from settings.txt (`FollowGameVolume=1`): voice lines and Nicole on the Voice slider, Dementia whispers and ambience on the SFX slider. The file is checked once a second and re-read when the game rewrites it. Before this every line played at full volume.
  - Phantom sounds no longer take `Volume` twice. Log: `voices: the game's sliders ...`, `voices output ...`.
- Fixes after the 19.10. evening survival test on mp_escape (build C53060A6, not installed):
  - Client crash 42200 (and very likely host crash 7908): a ghost's suit detach frees its pose buffer while Player+0x1E0 keeps the pointer; the animation update then wrote into freed memory. Ghosts between a detach and their next attach are no longer animated (`coop_uniform detached ghosts ...`).
  - Survival misplacement check: an enemy is only "put back by its engine" when it stands farther off than a run could carry it (5 m, then 2 m + 7 m/s). Runners were teleported back to their spawn and 17 engines were retired, so a Brute was refused 116 times and the waves starved.
  - When every engine is retired, they are all forgiven instead of refusing the rest of the run. An enemy written off on its engine's spot comes back through a vent when it has one (a Puker looped six times). Log: `survival placements ...`.
  - Bots against twitchers: a stasis aim that does not close within 0.6 s is given up on that target for 4 s, and the gun takes over. A twitcher dodges every aim, so the bot aimed for stasis without ever shooting. The dodger cone also widens the stasis aim. Log: `aim_frames= aim_given_up=`.
  - Dementia only with a human partner (user, 19.10.: "solo there should be no dementia, and bots should not get it either"). Solo, with bots or without, it stays off (`solo=` in the meter line); `SoloPercent` is gone. Bots never had a meter, and the phantoms target only the local Isaac.
  - Dementia: Pack children come more often (cooldown 22 s, weight 2.5, times stage-1 from stage 3, three at once at stage 4). Nicole's appearances (stage 3 of the plan) are not built yet; she is only heard.

- Dementia mutator, stage 1 (user, 19.10.: "a sanity system: players who fight alone instead of staying with the partner start to hallucinate, weakly first, then worse"; `[Dementia]`, off by default; src/runtime_coop_dementia.inc, src/coop_dementia.h, tests build_dementia_tests.cmd):
  - A meter per player, this machine only. It rises while no partner or bot is within `AloneMeters` (0 to 100 in `RiseSeconds`, 1.5 times as fast under 35 % health). It falls near company or in the store (`FallSeconds`). A med pack takes 20 off. It holds during scenes and while dead.
  - Stages at 25/50/75/90, each ending 8 points below its threshold. A rising stage gives a cue at once.
  - Events come every 40-70 s at stage 1 down to 9-15 s at stage 4 (`Pace`), weighted, each with a cooldown:
    - whispers beside the head;
    - mild, normal and heavy flicker, and glitch bands;
    - Nicole's lines with subtitles;
    - false RIG notices: a large necromorph incoming, vitals critical, partner signal lost (co-op only);
    - a dementia ambience bed;
    - a freakout.
  - The stage also sets a lasting vignette and tint, teal and dark red at stage 4, with a heartbeat swell from stage 3.
  - Screen: a veil in our D3D9 overlay (rects only, drawn first). `Flashes=0` caps blackouts at 50 % and white flashes at 25 %.
  - The game's own dementia effects:
    - dementia_screen (LUT, blur pulse, distortion, glitch) from stage 2, held 5 s;
    - dementia_flicker_on (dark flicker, small camera shake) from stage 3;
    - the screen effect kept up at stage 4.
  - Both are resident on all seven survival maps, made with Create_ByGuid without a parent at our Isaac; their camera bit puts them in front of the camera.
  - The veil stands in when one is not resident. Both are killed in scenes, on death, in the store and below stage 2.
  - Research: analysis/DEMENTIA_SCREEN_EFFECTS_2026-10-19.md. Log `dementia dementia_screen created= not_resident= ended_by_itself= (last after N ms)`.
  - Sound pack: `tools\export_dementia_audio.py` exports 254 WAVs from the game (variants/dementia/audio, manifest.json; the 4-channel EALayer3 streams are two interleaved stereo substreams, split and decoded each).
  - `tools\build_dementia_pack.py` picks 34 of them into variants/build/dementia (24.8 MB, to be installed as plugins\dementia):
    - 21 whispers;
    - 10 Nicole lines that work outside their scene, with timed ASCII subtitles (`ms|text` parts of up to 90 characters);
    - 3 ambience beds.
  - Sounds: WAV files in `plugins\dementia` (whisper_*, nicole_*, amb_*, a .txt next to a file holds its subtitle), played as phantoms through the voices module's XAudio2 (`voices::PlayPhantom`, needs the voices installed).
  - Test aid: `ForceStage=0..4`.
  - Log: `dementia stage a -> b`, `dementia <event> fired/REFUSED`, the summary line `dementia meter= stage= ... events fired/refused`, `voices phantoms`.
- Dementia stage 2, the phantom Pack children (`[Dementia] PackChildren=1`; analysis/DEMENTIA_PHANTOM_PACK_2026-10-19.md):
  - Chapter 11's Dementia Land Pack (PackEnemy 36CB5AC0-E992D12F and siblings, own package pack_boy_dementia, already forced resident).
  - Four records are in the pool as copies: tools/append_dementia_records.py, manifest section `dementia_records`, src/generated_dementia_records.h. They are no randomizer donors, so they never spawn on their own.
  - Spawned like the bot bodies: GetSpawnInfo, SetTransform, SpawnByInfo, placement, spawning mode 0 (no rail climb), Finalize, forced awareness.
  - Where: 8-15 m away on the mesh, out of view, behind first. One at a time at stage 2, two at stage 3, three at stage 4.
  - On this machine only:
    - the host's npcs::Register keeps it out of the ghosting;
    - a client's spawn filter lets it through (`phantom::spawning`);
    - the PackEnemy FilterTarget slot (0x01B870F0) accepts only the local Isaac for a phantom;
    - its execution grab (CheckInitGrapple slot 0x01B8703C, forced at Isaac health 30 or less) is refused for phantoms.
  - Its hits on our Isaac are real. Hits on anybody else are zeroed. Hits on it never become world hits.
  - Bots, turret, mines, Kill-all, drops, weapon XP, multi-kill voices, corpse rest sync and its death message all leave it out.
  - Death: the game's dementia flicker plus a glitch, then FadeOut with delete (Delete as the fallback).
  - It goes away on a stage drop, in a scene, on death, in the store, and when the level stops running.
  - Log `dementia pack child ... born/gone` and the summary line `dementia pack children alive= spawned= ... targets ours= refused= grabs_refused= ...`.
  - Next: Nicole's appearances (analysis/IDEAS_SURVIVAL_COOP_2026-10-18.md).
- Scene cues of a partner replay in the order they came (mirror NpcCueTick kept order; a later cue queues behind a waiting one, `npc_cues ... queued_behind=`). On 19.10. the client's igc_03_03 got start_player before pull_player_to_start; the pull replaced part 010. Installed in 2AFDDE22.
- Debug menu checkpoint jumps are synchronised. The host's jump goes to every partner as a host tool (the campaign is one map, so the level follow never takes it). A connected client's own jump is refused. Log `debug jump_sync`.
- A launcher client that loads the host's map by itself counts as joined (user, 19.10., variant A: both loaded the same checkpoint and never saw each other). Log `coop_level ... joined without the menu`, `same_map_joins=`.
- The partner in cutscenes was ended by the user (19.10.): user INI `[ScenePartner] Enabled=0`, and the partner clip moved to variants/anims_disabled. Two heap crashes (host 7908, client 13176) came on the machine that played our partner clip. Lead in the memory notes: its 786 top-level quantised records of 3130 samples are longer than anything in the retail clips.
- `voices listener` line: the engine's microphones, the camera and Isaac (for "the host hears almost nothing").

## Keeping this file current

- With every added, removed or changed feature, update its rows here (mode table and catalog) and add a line to the "Changed on" section of that day.
- After a playtest, move tested items from Untested to Works, Partial or Broken with the date, and drop fixed entries from Known issues.
- Keep one line per feature; link an analysis note for detail instead of writing it here.
- Update "Last updated" and the installed INI table when the user's DS2United.ini changes.

- Fuel Core (mp_detonate): rooms behind open doors stayed black with bots along. Cause open; every room the game hides on an MP visit is now logged with the player position and the calling code (`coop_streams our game hides`). FOUND 15.10.: an MP map shows one stream set at the arrival (StreamSetManager::Set::SetDesired_i hides the rest, return 0x00758B7D) and the next sets come with the objectives; survival runs none, so 32 of Fuel Core's zonerenders stayed hidden (1 of 6 sets ever desired). On an MP map those hides are now refused (`[MpMap] ShowAllRooms=1`, log `coop_streams mp_map hides refused=`).