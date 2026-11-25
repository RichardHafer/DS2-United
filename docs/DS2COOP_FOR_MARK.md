# Dead Space 2 United: networking and survival modes

**16.11.2026, issue #9 diagnostics:** `teleport::traintrace` uses existing verified hooks at Player::StartScriptedSequence and Entity::SetTeleporting, plus the frame poll. It matches only `ss_se_02_14`, `ss_teleport_player_for_crash` and `ss_start_hanging_se_temp` by full GUID. It records actor identity, local/bot identity, sequence GUID, read validity, player +0xC0, destination +0x1F0, move type +0x200, active sequence +0x238, flags +0x240, master state and the original start return. The destination layout was checked against retail TeleportTargetToDest at 0x00EB87F0 (sequence transform +0x1C0) and authored catalogue records. No game state is changed. Enable `[Coop] TrainTrace=1` for a reproduction; retain all normal settings otherwise. A missing start record remains distinct from a start rejected by the native function. The issue is not yet fixed.

**16.11.2026, DS2PCVR compatibility:** `src/vr_compat.h` identifies the game-folder proxy using three DS2PCVR strings, without loading or executing it. Detection runs once during United's early menu setup and once during MarkerPatch configuration. NativeModeRow and red menu archive redirects are bypassed on detection. MarkerPatch retains its existing FixSuitChangeTiming exclusion and also forces FixHazardSuitIcon, RawMouseInput, AutoHideMouseCursor, ImprovedAntiAliasingMode and AchievementSupport off before applying features. INI files are unchanged. The helper is copied into the MarkerPatch build by build_markerpatch_united.cmd. Detection tests cover the Steam, EA and pinned DS2PCVR 0.3.0 binaries; this does not establish United support for the EA or pinned game executables. In-game VR validation remains pending.

**16.11.2026, issue #8 (not installed):** `ghosttrigger::OnDoorOccupant` filters companion bots from `StandardDoor::UpdateEntitiesInRange(bool)` at retail PC `0x00DD20C2`. ESI holds the resolved entity, EDI the door. Skipping to `0x00DD214B` advances the native iterator without resetting occupancy already recorded for another actor. Both sites are byte-checked. The native caller at `0x00DD9CB1` uses the result to defer closing; this path does not use the TriggerPhysical filter. `door_occupancy checked/bots_skipped` and bounded door GUID logs distinguish an inactive hook from actual exclusions. `tests/door_occupancy_retail_tests.py <unpacked PC image>` exercises 27 combinations in Unicorn, plus the unfiltered bot case. Gameplay confirmation remains pending.

**Renamed 08.11.2026:** the mod is Dead Space 2 United (before: Co-op Mod / DS2Coop from 17.10.2026, HaferSprawl before that). Files: `DS2United.asi`, `DS2United.ini`, `DS2UnitedConfig.exe`, `DS2United.log`. Internal names (`ds2coop` namespace, `DS2Coop_*` exports, `runtime_coop_*.inc`) are unchanged. The plugin reads a merged `DS2Coop.ini` until a `DS2United.ini` with the randomizer's section exists, and stays idle when the old `DS2Coop.asi` or `HaferSprawl.asi` is loaded too (named mutex per process plus a module check in `ds2coop_main.cpp`).

A technical write-up for Mark. State of the repository on 14.10.2026. Everything below is taken from the source (`DeadSpace2Randomizer\src`), the analysis notes (`DeadSpace2Randomizer\analysis`), `FEATURES.md` and the prototype PDB notes in `Fable\coop_pdb`. Where the code says "untested" or the notes disagree with the code, this document follows the code and says so.

Conventions:

- Addresses are absolute retail PC addresses with the image base 0x00400000 (Steam `deadspace2.exe`, build 21478, 48,444,416 bytes, sha1 653c8332...). The source mostly stores RVAs and adds the module base at run time; they are converted here. Other exe builds (Origin/EA app) were never checked.

**Update 14.10.2026:** everything marked "built, not installed" up to 14.10. is installed now (plugin `DS2United.asi` CRC 86050441, menu stream CRC D2AEEB3A) and waits for a playtest.
- Every hook checks the retail bytes at its site first and refuses to install on a mismatch. The exe is packed on disk; both halves wait until the unpacked bytes appear at 0x00ECB2B0 before touching anything.
- Status words follow `FEATURES.md`: "Works (date)" = seen working in a playtest that day, "Partial", "Untested" (built, not played), "built, not installed" (compiled 13.10, not in the build that was played).

## 1. Overview

### 1.1 What the mod is

The Dead Space 2 United (by Hafer) is one ASI plugin, `plugins\DS2United.asi`, loaded by MarkerPatch's `dinput8.dll` (`LoadASIPlugins=1`). It contains two halves that used to be two plugins and still keep separate worker threads, logs and INI sections:

| Half | Entry | What it does |
|---|---|---|
| Randomizer | `src/observer.cpp` (`DS2Coop_RandomizerMain`) | enemy randomizer and the cross-level enemy pool, MoreEnemies, extra spawns, loot, schematic and bench randomizers, the campaign watchdog, the NavPower pool resize, and the **survival director** (`runtime_survival.inc`) for all wave and clock modes |
| Co-op | `src/coop_plugin.cpp` (`DS2Coop_CoopMain`) | the network session (TNL reused in single player) and side channel, story co-op, the main menu and lobby, MP maps in single player, Versus and necro players, the companion bots, the supply terminal, HUD/overlay/waypoints, music |

`src/ds2coop_main.cpp` is the module's DllMain and calls both. Each half creates its own start thread; nothing runs in DllMain itself.

The split matters for survival: the director that spawns the waves lives in the randomizer half (it needs the pool and the spawn hooks), while everything a player sees or that crosses the network lives in the co-op half. They talk only through C exports.

### 1.2 How the halves talk

Each half finds the other's exports with `GetProcAddress` on `ds2coop::PartnerModule(...)` (`src/ds2coop_config.h`). In the combined build that is the module itself; in the old separate builds it was `DS2United.asi` / `DS2RandomizerObserver.asi` by name. All exports are `extern "C" __cdecl`.

Co-op half exports (`src/coop_plugin.cpp`), called by the director (`runtime_survival.inc`, looked up once around lines 500 and 690):

| Export | Purpose |
|---|---|
| `int DS2Coop_NetRole()` | 0 no network (solo, bots only), 1 host, 2 client. A client runs no waves |
| `int DS2Coop_GameMode()` | the mode picked in the map picker: 0 survival, 1 mercenaries, 2 objective, 3 hold the zone, 4 versus; a running campaign arena answers the picked mode only when it has that mode's points (`[Arena] Modes`), else survival |
| `void DS2Coop_SurvivalEvent(kind, value)` | kind 2: a wave ended (host: queued to the partners as a reward event); kind 3: director state for the music, `(wave << 8) \| state` (played here and sent to partners) |
| `int DS2Coop_QueueNotice(text)` | RIG notice through the co-op plugin's one text slot; a host also broadcasts it (`survnet` kind 1) |
| `int DS2Coop_HudReady()`, `void DS2Coop_SurvivalHud(line, flash)` | the overlay status line; a host sends it on (kind 4) |
| `int DS2Coop_RespawnState()` | `(respawn epoch << 2) \| (armed << 1) \| pending`, so the director keeps its run through a respawn and adopts the new Isaac |
| `int DS2Coop_PartnerPositions(float* out, int max)` | partners' Isaac positions from fresh PlayerState packets, for spawn distance rules |
| `int DS2Coop_RewardEvents()`, `int DS2Coop_LastRewardWave()` | on a client: how many wave-end events the host sent and for which wave |
| `void DS2Coop_SurvivalCapsuleSend(kind, id, pos, golden)`, `int DS2Coop_SurvivalCapsule(...)` | Mercenaries time capsules: host announces placed (5) / gone (6, id 0 = all); a client's director takes them one by one and keeps local copies |
| `void DS2Coop_SurvivalMarks(xyz, golden, count)` | capsule waypoints |
| `void DS2Coop_SurvivalEnemyMarks(xyz, count)` | last enemies of a wave, marked and sent (kind 8) |
| `void DS2Coop_SurvivalZone(state, id, pos, radius, percent)` | Hold the Zone display; host sends it (kind 7) |
| `int DS2Coop_ZoneObjectives()` | the map-objective opener: 0 nothing, 1 opening, 2 opened, 3 not possible here |
| `int DS2Coop_DefendTick(enemies, count, seconds, dps, radius, lowest)` | Objective mode defend phases: enemies near a vulnerable target damage it |
| `int DS2Coop_VersusRound(int* out, count)` | Versus round state for the AI slashers: stage, round, kills by humans/necros |
| `int DS2Coop_HumanDeaths()`, `int DS2Coop_TeamDown()` | Mercenaries time penalty; game over when every human is down with no timed respawn |
| `int DS2Coop_RestartCheckpoint()` | Run end on an MP map (game over, Objective/Mercenaries/Zone result, host restart): reload the map from its checkpoint, partners follow |
| `int DS2Coop_ArrivalsReady()` | MP map arrivals whose Isaac is spawned and equipped; the director arms the next run 2 s after it moves following a checkpoint restart |
| `int DS2Coop_Arena(name, size)`, `DS2Coop_ArenaInside(point)`, `DS2Coop_ArenaSpots(out, max)` | running campaign arena, its rooms and recorded spawn spots |
| `int DS2Coop_ArenaPoints(kind, out, max)` | the running arena's zone (0), capsule (1) or respawn (2) points as x, y, z, radius (generated_arena_points.h) |

Randomizer half exports (`src/observer.cpp`), called by the co-op half:

| Export | Caller | Purpose |
|---|---|---|
| `int DS2Survival_Ready(map, mode, why, whySize, fix, fixSize)` | menu, lobby | before a launch: 1 ready, 2 ready with a warning (Versus without AI slashers), 0 no, -1 still starting; `why`/`fix` become an overlay hint card |
| `int DS2Survival_State(int* wave)` | shop, music | 0 off, 1 arming, 2 wave, 3 intermission, 4 dead |
| `void DS2Survival_NoteMessage(hash)` | world mirror | a message the co-op plugin replayed for a partner; its replays bypass the randomizer's dispatch hook, so the vent windows and objective count are fed here |
| `int DS2Survival_EscalationFor(wave, out, count)` | escalation, shop, bots | escalation level, bot damage %, price %, boss stasis resist %, enhanced chance % for wave n |

The observer also asks `DS2Coop_NetRole` itself (`CoopClient()`, observer.cpp:176): a client repeats no MoreEnemies spawns because the host's enemies reach it as ghosts.

### 1.3 Ticks and threads

- Co-op: one inline hook on the per-frame site 0x007701C0 (`mov eax,[0x0204D414] / sub esp,0xC / cmp eax,[0x0204D418] / je`) calls `coop::Poll()` and the uniform poll on the game thread. The randomizer's frame cap wants the same site; `hook_site.h` installs both under a lock and the second one chains behind the first. If the site cannot be taken, the co-op half drives itself from its local-game-player hook, throttled to about 16 ms (`g_selfDrive`).
- Randomizer: its own worker thread for logging, a game-thread driver for queued spawns and the director (FEATURES: installed even with `Watchdog=0` since 13.10).
- Co-op start-up order: unpack wait (up to 300 s), the native mode row's file redirect (the frontend opens about 3 s after start), then `[Coop] InstallDelayMs` (3000) so the DS2 Debug Menu can verify its retail bytes first, then frame tick, `coop::Install`, uniform install.
- A second or third copy on one PC: the co-op half takes the `FindWindowW` import slot (0x02158F34) the instant the packer resolves it, so the game's single-instance check does not quit the copy (`DS2COOP` or `DS2COOP_PORT` environment variable). `[Coop] UnpinThreads`, `BackgroundFullSpeed` and `FocusInputOnly` make two copies on one PC playable.

### 1.4 Configuration

`plugins\DS2United.ini`, written by `DS2UnitedConfig.exe` (`--migrate` merges the legacy files and keeps the co-op half's own DS2United.ini as DS2Coop.legacy.ini). It counts as merged when it has the `[DS2Randomizer]` section, else `HaferSprawl.ini` from before the rename is used; while one of them exists no other INI is read (`ds2coop::ConfigFile`). Without it each half falls back to its legacy file: `DS2United.ini` (co-op), `DS2Randomizer.ini` (randomizer), `DS2Survival.ini` (director, read at the start of every run so edits apply to the next run), `DS2BotProfiles.ini`. The repo templates of those files are the documented defaults; `FEATURES.md` lists where the installed INI differs. Shared settings must match between machines; `[Coop] ConfigCheck` exchanges a fingerprint of the INI and the build and lists differences in the log (warning only).

Relevant sections: `[Coop]` (network, ports, sync switches), `[Gate]`, `[Checkpoint]`, `[Level]`, `[Mirror]`, `[Streams]`, `[Loading]`, `[Menu]`, `[NativeLobby]`, `[Loadout]`, `[MpMap]`, `[Respawn]`, `[Versus]`, `[VersusPoints]`, `[Necro]`, `[Bot]`, `[Shop]`, `[Hud]`, `[Zone]` (display), `[Marks]`, `[SurvivalMusic]`, `[Arena]`, `[Visuals]`, `[CameraTools]` in the co-op half; `[Survival]`, `[Wave1..10]`, `[Escalation]`, `[Mercenaries]`, `[Objective]`, `[Zone]` (rules), `[VersusSlashers]`, `[VersusSkins]`, `[Cleanup]`, `[ProgressXp]` in the director's file; `[Progress]` (profile XP, weapon levels and the MP upgrades they unlock, runtime_coop_progress.inc) and `[ExitGuard]` (level exit guard, runtime_exit_guard.inc in the randomizer half) since 15.10.

### 1.5 Logs

All next to the plugin:

| Log | Content |
|---|---|
| `DS2United.log` | co-op half: install lines (every signature ok/MISMATCH), session, side channel, gate, mirror, streams, once-a-second diagnostics. The last three runs survive as `DS2United.prev.log`, `.prev2.log` and `.prev3.log` (`src/log_rotation.h`, for every log here); a second copy on the same PC writes `DS2United.1.log` |
| `DS2Versus.log` | lobby, menu, MP map visit and Versus lines (`mpmap::Event`) |
| `DS2Bots.log` | bots |
| `DS2Randomizer.log` | randomizer half; director lines start with `survival` |
| `C:\DS2CrashDumps\` or the game folder | `deadspace2.crash.<pid>.dmp` + `.txt` + `.log` (the session's log) from `[Coop] CrashDumps` |
| `C:\DS2CrashDumps\bug_<time>_<id>_m<machine>_<pid>\` | the bug key (Ctrl+F12, `[Coop] BugReport`, `runtime_coop_bugreport.inc`): logs, INIs, `screen.bmp`, a dump of every thread, `note.txt`. A press sends `BUGR` (12 bytes: magic, token, id) and every partner saves its own folder under the same id. The key is read on its own thread, so it works while the game thread hangs |

## 2. Networking architecture

Short version: the co-op half builds the engine's own `NetSession` in single player, without Blaze, puts TNL on its own UDP socket, registers each machine's Isaac (and, on the host, the enemies) as NetObjects so TNL ghosts them, and runs everything the engine does not replicate in campaign mode over a second, plain UDP side channel with its own small protocols. Every machine dials every other one, because TNL does not forward ghosts. Main file: `src/runtime_coop.inc`.

Defaults: the values below are the repo template (`DS2United.ini`), which is what ships. The code default when a key is missing is often 0 (off), e.g. `HostNPCs`, `ReportHits`, `PeerTimeout`, `[Loading] SyncWithPartners`, `[Bot] GhostToClients`; `analysis/ONLINE_PLAYTEST_SYNC_AUDIT_2026-10-13.md` §0.4 lists the differences.

### 2.1 The session in single player

Single player never constructs a NetSession (the singleton `[0x020216DC]` is 0 in every SP dump), but the TNL class table is registered in the campaign and every online object except the BlazeHub (`OnlineServices [0x020213DC] + 0x94`, created only by `OnlineServices::StartUp` 0x01375720) already exists. `StartSession` builds it the way 0x015EFDD0 does:

| Step | Engine | Note |
|---|---|---|
| allocator | `[0x020213DC] + 0x8C` | |
| allocate 0x90 bytes | `GameNew` 0x00403CB0 (cdecl) | |
| construct | `NetSession::NetSession(allocator, OnlinePlayer*)` 0x0082A520 (thiscall) | the interface sits at session + 0x1C on PC |
| dial each partner | `NetInterface::CreateConnection(OnlinePlayer*)` 0x008578C0 (thiscall) | one call per dialled address |

The `OnlinePlayer` is a fake: the engine dereferences a 4-byte handle to an impl and calls slot 3 of the object at impl + 0x50 for the address (`(port << 32) | ip`) and slot 1 for the machine index that NetObjSync and the sub-key allocator fold into every key. There is one static fake per dialled partner (the engine keeps the pointer). The local player's address is `127.0.0.1:Port`; whether the engine ever sends that address to the other side was not verified.

`NetObjSync` 0x00829940 asks the online manager's local-game-player getter (vtable slot 26, 0x013778E0, hooked) for an object whose +0x210 holds the local impl. Offline that object is null. The hook answers a fake only for the call site inside NetObjSync (return address 0x00829984); a non-null answer to ordinary callers (about 30 a second) sent the animation transform 0x0106B1D0 down a Blaze path and crashed both instances. The whole-level object sync this would enable is off (`[Coop] SyncAllObjects=0`): it registered every level object and crashed in the same place. Objects are registered by hand instead (2.3).

Transport: the four `TNL::Socket` methods are inline-hooked so TNL runs on one plugin-owned, non-blocking UDP socket bound to `[Coop] Port`:

| Method | Address | Detour |
|---|---|---|
| ctor | 0x01417EA0 | creates and binds the shared socket (ref-counted) |
| dtor | 0x01417EE0 | closes it at refcount 0 |
| sendto | 0x01417F00 | plain `sendto`, TNL NetError codes back |
| recvfrom | 0x01417F50 | skips `WSAECONNRESET`/`WSAEMSGSIZE` (up to 16), because the engine's receive loop stops at any non-zero answer |

### 2.2 The tick

`coop::Poll` runs once per frame from the frame hook (1.3). A session starts when `network` is on and either `[Coop] AutoStart=1` and the level has settled (the slot 0 body stable for 4 s; on an MP map `[MpMap] SettleMs`, 1500) or the start key is pressed. Then:

```
connected        = any connection in state 7 (connection + 0x1B0)
replicationReady = connected && settled && (!RequirePeerReady || AnyPeerReady)
tickMode         = replicationReady ? [Coop] TickMode (2) : 1
```

Mode 2 calls the engine's `NetSession::Tick` 0x0082B300; mode 1 calls only `NetInterface::checkIncomingPackets` 0x0141A340 and `processConnections` 0x0141B400 (so connecting works before any object is synced; the first test showed the engine's own tick never ran, and a full tick with no connection crashed in an entity tick). The session is ticked at most every 10 ms, inside SEH; a fault stops ticking and logs the address. No TNL tick runs while the frontend's Apt director `[0x0208BE90]` exists (a partner still in a level would push ghosts into the frontend world). Once `replicationReady`, the local Isaac is registered and (host) the NPC scan runs.

One role per game start: a session is never torn down; a client cannot become a host, and the role cannot change while a session exists (`RoleChangeAllowed`). A host back in the menu keeps its session and hosts the next match in it (`HostSessionRunning`).

### 2.3 Players as NetObjects

`RegisterLocalPlayer` does by hand what NetObjSync would do. The NetReflectable sits at Player + 0x5C; its vtable must hold exactly these targets or registration is refused:

| Call | Address |
|---|---|
| slot 2 `SetSubKey(id)`, written at entity + 0x40 | 0x007D1D10 |
| slot 12 `netInit` | 0x007D1DC0 |
| `NetReflectable::AddToSession(this, 0)`, builds the NetProxy at reflectable + 0x0C | 0x007E10E0 |

The sub-key is `(machine << 24) | ++[session + 0x18]` (the engine's own counter; refused past 0xFFFFFF or machine > 255). The top byte is the machine number from the lobby (3.3): that is why two machines must never share one, and why `MachineOf(entity) = [entity + 0x40] >> 24` identifies the owner of any ghost. Bots use machine bytes 0xF0 + i.

A partner's Isaac arrives through `Player::NetCreate` as a ghost Player (proxy + 0x30: 0x8 local, 0x2 ghost) and registers itself in the next free PlayerManager slot (`[0x0201C0D4]`, capacity 8). It is dressed from the partner's suit GUID through the native `ChangeToUniform` 0x00EE0610 (`SuitSync`, `SuitStream`); the uniform hooks (ChangeToUniform, attach 0x00EB4E10, LoadUniformStream 0x00EC6310) go in only when the first partner connects, because solo they would change bytes the DS2 Debug Menu verifies. Ghost pose updates (slot 15 0x01091790, slot 14 0x01091A00) are skipped while a ghost's AnimView has no bone table yet.

Scope: `GhostConnection::objectInScope` 0x014160A0 is hooked and filters by NetProxy (vtable 0x01B22C0C) class name. Always in scope: `Player`, `NecroPlayer`, `PairedAttackCoordinator`, `NecroPlayerRespawnManager`; with `[Coop] HostNPCs=1` the host also scopes `DeadSpaceNPC`, after a template check (2.6). A client scopes no NPC to anybody.

Two Isaacs, one GUID: the ghost Player carries the level Player's GUID `36CAB03F-2C39E63A-45544150-3230314C`, and because it registered later, the registry's `FindByGuid` 0x00776CA0 returned the ghost to every script node asking for "the Player" (cutscene cameras, a double crash at 0x00892690). `[Coop] GhostGuidRedirect=1` mid-hooks its three epilogues (0x00776D20, 0x00776D4E, 0x00776D91; the prologue is left for the Debug Menu's byte check) and swaps a ghost answer for the local twin found on the NetReflectable list `[0x0205B744]` (`runtime_coop_guidfix.inc`).

### 2.4 Full mesh and re-ghosting

TNL does not forward ghosts. Measured 17.09 with four local instances: the host had three connections and three ghost bodies, each client had one connection and saw only the host, even though the side channel relayed fine. A ghost the host holds belongs to someone else and TNL will not hand it to a third connection. So every machine dials every partner it was given (`AddDial`, `Connect`); the first side to connect wins and the other accepts. The client gets the full roster in the Launch packet; the host dials every joiner at the launch (`ConfigureHostDials`). Every 12 s each dialled address without a live or pending connection is dialled again. 4 and 8 local instances have been meshed this way (17.09).

Re-ghosting (`runtime_coop_reghost.inc`, `[Coop] ReghostOnRebirth`, code default 1): after a checkpoint restart or death the client's level is torn down (ghosts deleted with it) while the TNL connection survives; the server side of that connection still believes the ghosts are in place and never sends them again. When a partner's readiness epoch moves (its Player was replaced) and it is ready again, every machine calls `GhostConnection::resetGhosting` 0x01416850 and `activateGhosting` 0x01416A20 on its connection to that partner (matched by IP at connection + 0x108 and port at + 0x10C), once per epoch. The client answers `rpcReadyForNormalGhosts` and gets every in-scope object afresh. Both calls return at once on the client side of a connection, so calling them everywhere is safe. A first epoch more than 20 s after the local level settled (a late joiner) also triggers it. On MP maps only the first epoch change per arrival resets; resetting on every respawn put partners back in the default suit with empty hands.

### 2.5 The side channel

One non-blocking UDP socket on `Port + 1000`, opened at install whenever `[Coop] Enabled=1`, polled from `Poll` (up to 16 datagrams per poll, 512-byte buffer). Packets are dispatched by datagram size (each packet type has its own size, checked at compile time); a few are matched by magic first.

Peers (`src/coop_peers.h`, 8 slots): a host accepts strangers and learns peers from their first packet; a client accepts only configured peers (the host and its roster). Each peer is identified by its readiness token (`GetTickCount() ^ pid * 2654435761`), which every packet except PlayerState carries. Learned slots are recycled after 120 s of silence; configured ones never.

Topology: the TNL bodies are a mesh, the side channel may be a star. The host relays: PlayerState with the relay bit `0x80000000` in the machine field, most other packets via `RelayToOthers` (only what it received directly), filed under the author's token (`AcceptRelayed`). Not relayed: RSST, RMVT, REGN, TKEV/TKPO, MPNV, VRSS (host-only senders or host-addressed asks for the last two; whether the rest matters with the mesh is unclear).

Readiness (`coop_peer_readiness.h`, per peer, every 250 ms): `{token, epoch, ackToken, ackEpoch, ready flag, sequence}`. A peer is ready only when it reports ready, acknowledged this machine's token and epoch, and was heard within 3 s. The epoch moves whenever the local body or ready flag changes, which is the trigger for re-ghosting.

Packet catalogue ("repeat" = blind resend without ack):

| Magic | Bytes | Direction | Rate / reliability | Content | Source |
|---|---|---|---|---|---|
| `DKP2` PlayerState | 52 | all, host relays | 33 ms; older sequence dropped | position, yaw, forward, helmet bit; presence only, the body moves through TNL | runtime_coop.inc |
| readiness | 32 | all, per peer | 250 ms | token, epoch, acks | coop_peer_readiness.h |
| `CPNG` ping | 28 | all | 500 ms, lost after 3 s | RTT, jitter (RTP style), loss | coop_ping_protocol.h |
| suit | 36 | all, relayed | on change ×5, then 5 s; epoch-ordered | worn suit GUID | coop_suit_protocol.h |
| loadout | 44 | all, relayed | on change 3 sweeps, then every 10 s | weapon GUIDs, one slot per packet | coop_loadout_sync.h |
| `CLDB` / `BSUT` | 80 / 32 | host | on change + periodic | host bots' weapons / suits | coop_loadout_sync.h, runtime_coop.inc |
| `SVNT` kind 9 | 156 | host | per director payment, 3 sends | profile XP {run, wave, xp, flags}: each client books its own, dedup by (run, wave, event) | runtime_coop.inc survnet, coop_progress.h |
| death / `ADCK` | 24 / 20 | dying machine / every receiver | 250 ms until every partner acks, give up 15 s | death epoch; the only acknowledged packet | coop_death_protocol.h |
| `CHIT` | 48 | non-owner | 3 copies (0/60/180 ms), (token, seq) dedupe of 256 | hit on a host-owned or world object | coop_hit_protocol.h |
| `WDMG` / `KONG` | 200 / 40 | any, relayed | repeat | prop hit as raw NetDamageData_t / Tormentor arm health | runtime_coop_hits.inc |
| template | 60 | host ↔ primary partner | ask 500 ms, valid 4 s | "is this NPC template resident and identical" | coop_template_protocol.h |
| `VOTE` | 40 | all, host relays | 400 ms for gate timeout + margin | progression vote with position | coop_gate_protocol.h |
| `MIRR` | 68 | host (world) / any (sequences) | 3 copies 150 ms | mirrored message, direct send, world call, journal | runtime_coop_mirror.inc |
| `CKPT` | 56 | host; client restart request | 500 ms; request 6× | death checkpoint GUID, restart epoch | runtime_coop_checkpoint.inc |
| `LEVL` | 168 | host | 1 s | map, stream list, checkpoint, epoch | runtime_coop_level.inc |
| `LDSY` | 96 | all | 250 ms | loading / held / ready / playing, map | runtime_coop_loadsync.inc |
| `STRM` / `VISI` | 72 / 244 | all | 1 s | stream set held and pending / room visibility intent | runtime_coop_streams.inc |
| `TKEV` / `TKPO` | 64 / 112 | holder | event / 33 ms | kinesis event / held-object pose | runtime_coop_tk.inc |
| `RSST` | 240 | any | 1 s | rest poses of props and corpses | runtime_coop_restsync.inc |
| `RMVT` / `REGN` | 156 / 76 | host | 3× | removals / Hunter limb regrowth | runtime_coop_removals.inc, runtime_coop_regen.inc |
| `INPT` / `AIMS` | 224 / 24 | scene leader | on change + 100 ms / 33 ms | leader's pad state / aim in shared scenes | runtime_coop_cutscene.inc |
| `SVNT` survnet | 156 | host | 3 copies 100 ms | survival notices, rewards, music, HUD, capsules, zone, marks (4.9) | runtime_coop.inc |
| `SVNT` kinds 13 / 14 | 156 | host → all / client → host | 3 copies on a change, refresh every 2 s / 3 copies | the host's difficulty `{generation, difficulty, locked, answered request}` / a client's pause-menu change `{token, request, base generation}` (4.13) | runtime_coop_difficulty.inc, coop_difficulty.h |
| `NAME` | 132 | all | 2 s | nameplate names by machine | runtime_coop_nameplate.inc |
| `LIFE` | 18 | all | on change, else 500 ms | MP-map life state, wave | runtime_coop_respawn.inc |
| `USEL` | 32 | all | on start, every 400 ms while held, release | machine claim `{holding, GUID}` for hacking panels, power node doors and the eye poke machine; a partner's start at a claimed machine is refused (`[Coop] PanelLock`, 14.10.). Kinesis: a body a partner holds is no cast target (`[Kinesis] PartnerHoldLock`, hook on GetTargetPositionFromCast 0x008A3530) | runtime_coop_uselock.inc |
| `NPOL` | 16 | any | event | necro pool demand / loaded | runtime_coop.inc |
| `MPNV` / `VRSS` | 272 / 72 | client → host, host → all | 2 s / 3 copies | Versus roster / match events | coop_mpnative.h, coop_versus.h |
| `CNFG` | 504 | all | 0 s, +3, +10, then 60 s | INI and build fingerprint (warns only) | coop_configcheck.h |

Latency-aware windows (`netclock`): `Rtt()`/`Jitter()` are the worst smoothed values over peers heard within 10 s; `Window(lan, cap) = lan + min(2·rtt + 4·jitter, cap)` stretches the cutscene, mirror spawn-wait, stream hold, kinesis and respawn timers. The template check is not stretched.

### 2.6 What is replicated, and who owns it

| System | How | Authority |
|---|---|---|
| Player bodies, animation | TNL ghost of each machine's registered Isaac; update period 32 ms / 12000 B/s (`[Coop] NetRate`, below) | owner |
| Ghost timing | `[Coop] GhostTimeline=1`: hook on `imatrix44_t::AuthoritativeUpdate` 0x007D0530 puts each received transform on the sender's clock (min offset over 16 samples), so `Entity::GhostTick` 0x007D1A70 extrapolates on the sender's spacing instead of arrival jitter | owner |
| Helmet | PlayerState bit → `Player::SetFlag(0x17)` 0x0085F7E0 on the ghost | owner |
| Suit, weapons | suit/loadout packets → native `ChangeToUniform`, `Quartermaster::RequisitionGun` 0x00DDD5F0 (a ghost with empty hands is re-handed its last gun, `GhostGunReequip`) | owner |
| Enemies | `[Coop] HostNPCs=1`: the host registers every `DeadSpaceNPC` (and `PairedAttackCoordinator`) by hand (`npcs::Scan`, every 100 ms, ≤ 8 per pass, machine byte 0); clients suppress their own NPC spawns in `ShouldSpawn` 0x0040DB20. Exceptions spawn locally on every machine: `LocalNPCClasses` (NicoleBoss), `NeutralNPCsLocal`, spawns within `ScriptedSpawnWindowMs` (3000) after a scripted sequence cue (ghosts do not act in sequences). `NearestTarget` mid hook 0x00FFF608 in `DeadSpaceNPC::UpdateTarget` makes enemies pick the nearest player | host |
| Template check | before an NPC is scoped to a client, the host asks whether template hash `[object + 0x48]` resolves to the same `{GUID, class, flags}` there. A client without the same pool would get ghosts it cannot build. Since 02.11. (`[Lifecycle] StickyTemplates`, `runtime_coop_templates_sticky.inc`) per partner, asked at his own address, and an answer lasts while his level stands (revoked when he reports not ready, is silent 10 s or has a new token); before, a 4 s validity and a wipe on every readiness epoch change dropped every enemy ghost on each respawn. Per connection once the scope hook passes it (`TemplateScopeAllowedFor`); until then the primary partner's answer | host |
| Damage | see below | host for enemies, everyone for world objects |
| Dismemberment | engine-replicated with host-owned NPCs (`SetDismemberableState`; components have `HandleDamage` and `GhostHandleDamage`). Severed parts and kinesis objects do not replicate | host |
| World logic | each machine runs its own level logic; the mirror repeats world-changing messages (2.7) | host for counters/objectives, any for sequences |
| Progression | gate votes (2.8) | symmetric, no arbiter |
| Level start | load sync barrier (2.9) | symmetric |
| Checkpoint, level | host broadcasts; clients follow (2.10) | host |
| Rooms | stream hold and visibility union (2.11) | host is the reference |
| Kinesis | holder streams the pose, receiver applies `applyHardKeyFrame` 0x014A27E0; no authority transfer (the engine's `RPC_AuthorityTransfer*` are only observed) | holder |
| Pickups, loot | not ghosted; enemy loot exists on the host only in the campaign; survival rewards and capsules are dropped per machine | per machine |
| Cutscenes | leader/follower: the follower's partners are hidden, its input blocked, it is invincible; `LeaderInputForAll`/`LeaderAimForAll` stream the leader's pad and aim (INPT/AIMS); two leaders: host wins | leader |
| Survival director | host only; clients follow survnet (4.9) | host |
| Versus match | host (4.12) | host |
| Bots | host only; ghosted to clients with `[Bot] GhostToClients=1` | host |

**Damage.** `EARS::DirectSendDamageEvent` 0x010BD030 (cdecl) is called on every hit. It returns when `[damageData + 0x10] & 0x40000000`, when the target has no net proxy (0x007E1120, proxy at reflectable + 0x0C), and otherwise tests bit 1 of proxy + 0x30 (0x007E1150). The earlier reading of that test as IsAuthoritative is in `Fable/coop_pdb/DS2_COOP_PHYSICS_2026-09-17.md`; the later reading in `runtime_coop_hits.inc` (from a client dump) is that bit 1 is the ghost bit, so the engine only tries to send for ghost targets, and whether that arrives is open. In practice a client's hits on host-owned enemies were applied locally only (different limbs off on each machine). The plugin path (`[Coop] ReportHits=1`, `ApplyHits=1`):

1. A pass-through hook on 0x010BD030 notices foreign targets; the amount is measured as the health that went away locally in the hooked `HealthComponent::HandleDamage` 0x00F65B80 / `DismemberableHealthComponent::HandleDamage` 0x00F73600, not parsed from the damage struct.
2. The client sends `CHIT {subKey, guid, amount, zone}`.
3. The host resolves the sub-key on the reflectable list, cross-checks the GUID (pool enemies built from one record share a GUID), refuses if it does not own the object, and takes the amount off with the engine's `SetHealth` 0x00F84C60 (only downward).

World objects with no proxy (the decompression button) are named by authored GUID and every machine takes the same health off its own copy (`WorldHits`). Props, vent covers, containers and destructibles keep health in `IDamageable`, not a HealthComponent: `IDamageable::ProcessDamage` 0x00DC5260 is hooked, the hit goes out as the engine's own `NetDamageData_t` (ctor 0x00EAE280) and is replayed through the engine's receiver sequence (`DamageData` ctor 0x00ECFD30, `ToDamageData` 0x00ECFF90, `INetDamage::HandleReflectedDamage`), so a replay is reflected and never sent back (`WorldPropHits`). With `LocalNPCClasses=NicoleBoss` each machine fights its own Nicole and her hits are shared through `NicoleBoss::ApplyDamageModifiers` 0x0107D930; a lethal one sets her zero flag on every machine. The Tormentor's arm health is shared the same way (`KONG`).

**Net rate.** TNL's measured defaults are a 96 ms send period at 2500 B/s with 240-byte packets (the console lobby numbers). `runtime_coop_netrate.inc` calls `NetConnection::setFixedRateParameters` 0x01418350 once per established connection with `[Coop] NetSendPeriodMs` (32, about 30 Hz) and `NetBandwidth` (12000 B/s), which clears the adaptive bit and renegotiates.

### 2.7 The world mirror

`runtime_coop_mirror.inc`, `[Mirror]`. Every scripted event goes through one dispatch, `bool __cdecl(message*, flags)` at 0x00756C60, where the message is 12 bytes `{name hash, flag byte, source handle}` and listeners come from the registry at `[0x0204CD8C]`. The gate hooks it once (shared with the randomizer through `hook_site.h`), and the mirror runs inside that hook.

Each machine repeats to every other each message whose listeners move the world, and the receiver replays it unless it fired the same message itself within `DedupeMs` (5000). The asymmetry that decides every rule: a door opening twice is harmless, a quarantine counter counting one death twice is a door that never opens.

- World classes (doors, movers, damageable and scriptable props, VentMarker): mirrored by the host, and by clients with `ClientsMirror=1` (template 1).
- Counters and objectives (`VariableOperator`, `VariableWatcher`, `ObjectiveEntry`, `LogEntry`): host only, because they count things (enemy deaths above all) that exist only there; a count must have one author.
- Saves held (campaign joiner, debug jump, MP visit, arena): `SaveManager::ChooseSaveSlot` 0x00B99FE0 writes nothing, and since 14.10. the hold reports the write as finished through the device callback 0x00BAE3A0 (manager+0x24), or a save kiosk keeps the logic paused.
- Scripted sequences: from any machine. With `[Mirror] StoryCuesOnce=1` (built 14.10.) a sequence or scene dialogue cue and a neutral NPC spawn play once per machine per level life, from whoever fires first; a later copy from another origin is dropped on both the sender and the receiver (`mirror::OnceBook`). A cue one origin fires twice counts as an authored repeat and stays shared. Without it a player behind the group re-sent old scenes and the partners' NPCs restarted them.
- Never: FuncSpawn (enemies stay the host's), StreamSet/StreamScripter, Checkpoint/LevelChange (own modules), player commands, and the pure relays and gates (MultiManager, OrEventGate, Lua nodes): their outputs are mirrored, and mirroring the input too would fire the output twice. `analysis/COOP_SYNC_NEVER.md` has the reasons per class.
- Direct sends (`SendMessageTo` 0x00756A00) are mirrored by target GUID; world calls (`[Mirror] WorldCalls=1`): `VentMarker::SetCoverState` 0x012F5AB0, `Door::TransitionToState` 0x00DD2950 with doors waiting for partners on both sides and door votes (`DoorsWaitForPartners`, `DoorsVote`, `DoorWaitMs` 180000).
- A relayed MultiManager sends only its safe targets on the receiver (`OnSendMessages`, `[Mirror] MultiManagerDropClasses/Nodes`). Since 14.10. (`SplitDroppedTargets=1`) a target dropped for a dangerous listener is still delivered straight to its MultiManager listeners (`SendMessageTo`), whose own targets are filtered again; the ch02 tram station hallucination never ended without it.
- Removals (`MultiRemoveTarget::HandleEvents` 0x007E5E50 hooked, `SharedRemovals`) go as `RMVT`.
- Journal: the host keeps every message it mirrored since its last checkpoint (256 entries with counts); a client asks once per host epoch and map and gets it replayed (8 entries per tick, each sent twice). That is how a player who arrives later gets the doors, vents and counters the others already changed.
- A replay is only applied with a loaded level, the local entity present and the message name registered here (the dispatch has no not-found branch).
- Replays call the gate's trampoline, so they bypass the randomizer's dispatch detour; `DS2Survival_NoteMessage` feeds them to the director's vent windows and objective count.

On MP maps the objective chain has its own once-per-round rule (4.12).

### 2.8 The progression gate

`runtime_coop_gate.inc`, `coop_gate.h`, `coop_gate_protocol.h`, `[Gate]`. "Nobody rides the elevator alone." A message whose listeners would take a player somewhere is held, a `VOTE` goes out, and the message is replayed once every player's own copy of the level fired it too (each still standing near where he voted), or after the timeout. The gate can delay progress, never lose it.

- Classification asks the listeners what they are: every engine object answers a class query through vtable slot 4 (`bool QueryInterface(classHash, void** out)`, inheritance aware), so the rule is a list of class names (`Classes=SimpleOneTimeDoor,Mover,MoverController`); script nodes, which are not entities, are matched by vtable (`Nodes=Checkpoint,LevelChange`). The hash is the engine's `h = h*0x1003F + lowercase(c)`.
- Quorum: live votes ≥ 1 + every partner heard within `PeerGraceMs` (60000). A voter who moved more than `DriftLimit` (6 m) from his vote no longer counts. `TimeoutMs` 180000 lets it through regardless. With partners configured but none ever connected, nothing is held.
- Ghosts fire trigger volumes too, so for a trigger source your own vote counts only once your Isaac is within `TriggerReach` (10 m).
- `RideTriggers=1`: boxes that start a ride nothing else holds (from `tools/coop_ride_triggers.py`, `generated_ride_triggers.h`) wait until everybody stands in the box. `[Coop] LiftWait=1` holds lift buttons until every partner stands in the cabin (fixed 13.10, untested).
- `PullToWaitingPartnerMs` (20000): a player the group waited for that long at a progression point is moved to where the partner voted.
- Votes repeat every 400 ms for the timeout plus a margin and keep going 3 s after release, because a vote that reached one side but not the other otherwise left the other waiting out the full timeout.

### 2.9 Loading together

`runtime_coop_loadsync.inc`, `[Loading] SyncWithPartners=1`. Every tick of the loading screen `LevelManager::OkToStart` 0x0042C970 (thunk 0x0040233D) asks whether to start; `CheckpointLoadComplete` 0x0042D5D0 only asks once the checkpoint's own StreamSet has loaded, and while the answer is no the logic stays paused under the native loading screen. The native MP mechanisms behind it (the `LevelManagerWait` list at `[0x02021574]`, WAIT_FOR_REFLECTABLES with the host's vent count and spawn point request) need the MP flags during the load and exist in no campaign level, so the hook answers the question itself.

Since 02.11. it is a barrier the host decides (`HostBarrier=1`): the host numbers every coordinated load (its restart, a level change, a load from the menu) and sends the number in `LDSY` (v3: `gen`, `releasedGen`, flags, the machines it still waits for). Each machine holds until it is READY for that load (loaded, the rooms of the set completed since the load at status 3 (`WaitForRooms`, `RoomsWaitMs` 8000), a story joiner's suit resident (`SuitWaitMs` 8000), with `PartnersDuringLoad=1` the partners' Isaacs or `GhostWaitMs` 10000), and starts when the host releases it. The host releases once every side-channel peer and every lobby joiner it is pulling in is READY or left out: silent after being ready (`MemberSilenceMs` 20000), in the main menu, never reloading (`RestartGraceMs` 15000), past `SyncTimeoutMs` (60000) or, for a joiner, `JoinTimeoutMs` (120000). A left-out machine starts on its own when it is done. A client whose host went silent starts too. Every member's way through the barrier is logged (`coop_barrier gen=N member machine=M ... READY|LEFT OUT|waited for`). `HostBarrier=0` is the older rule (each machine waits for every partner heard from in the last 3 s; played online 13.10: loaded together 3 of 3). Design note: `analysis/COOP_LIFECYCLE_STORY_2026-11-02.md`.

### 2.10 Checkpoints, level, death

- `[Checkpoint]`: every change of the death restart point goes through `Checkpoint::SetDeathRestart` 0x00BA1F20 (death GUID `[0x0208FCC4]`). The host broadcasts its GUID every 500 ms; a client refuses its own changes once the host's is applied (`Checkpoint::Find` 0x00B86C30). The restart implementation 0x0040EC10 is wrapped: a host restart is counted into `CKPT` and clients follow; a client restart is sent as a request (deduplicated per client on the host) and the host restarts everybody; the client's loading screen waits for the host's new load number. A restart resolves the death GUID through `LevelManager::GetCheckpoint` (`Find(&guid)` by the object registry, else `Find(null)`: the override GUID `[0x02091820]` in the static checkpoint array `0x02090B18`, else the default), so before a client restarts, a host checkpoint that was never found here is written into `[0x0208FCC4]` (`HostCheckpointOnRestart=1`).
- `[Level]`: the host sends `{map, stream, checkpoint}` every second; a client on a different map replays the level-change recipe: `SetOverrideCheckpoint` 0x00B757B0, `ClearPersistent` 0x007B6950, `StoreLevel` 0x00B75880, `GameMainLoop::RequestLevelChange` 0x0040F870. Same-map reloads are not followed (one hung). MP maps are not followed this way; they go through the lobby.
- Death: `Player::ProcessDeath` 0x00878B40 is hooked. A local death sends the acknowledged death packet; with `MirrorDeath=1` a receiver runs the native campaign death on its own Isaac (through the trampoline, so it cannot cascade). On MP maps with lives left there is no death packet; the MP respawn takes over (`[Respawn]`, `Player::RespawnPlayer` 0x00891D70, 4.9).

### 2.11 Streams and visibility

`runtime_coop_streams.inc`, `[Streams]`. The unit is the StreamSet: activating one loads its members and drops everything else, and every activation (script, door leave/open set, lift, level start) ends in `StreamSet::Activate` 0x00759D00.

- Hold rule: an activation that would drop a stream a partner's set (or pending set) still holds is deferred until every partner's set fits inside it, retried every 250 ms, let through after `HoldTimeoutMs`. Each machine sends its set and pending set once a second (`STRM`). The host is the reference and is never held. (Template: `HoldForPartners=0`; the pinning below is the active mechanism.)
- Pinning (`PinPartnerStreams=1`): `StreamSetManager::Set::SetDesired` 0x00758C80 keeps rooms a partner still holds in the desired list. The keep list fills at 46 entries (FEATURES: host pop-in).
- Visibility: a loaded room is drawn only when the game's visibility flag says so (`VisibilityManager::Show/Hide` 0x007C1270 / 0x007C12F0 on `[0x02021498]`). A player carried into the next room by a cutscene never crosses the triggers that switch it and stands in a black room; `VISI` (1 s) lets the carried player take the rooms of a partner who walked. A union of every machine's intended rooms exists (`VisibilityUnion`, template 0).
- On MP maps there is no stream movement: everything is one level-start set (10.13 analysis).

### 2.12 Necro players in Versus

`runtime_coop_necro.inc`, `[Necro]`. A human necromorph is a `NecroPlayer` owned by that player's machine:

1. Isaac is unregistered from the PlayerManager (0x0077BC00);
2. a NecroPlayer is spawned with Isaac's sub-key and registered with the manager (`NecroPlayer::RegisterWithManager` 0x0108C310);
3. it is registered with the session by the same SetSubKey/netInit/AddToSession triple (`RegisterNet=1`), and the partners see it through the NecroPlayer scope whitelist;
4. Isaac is deleted. With `[Versus] NecroWithoutIsaac=1` the round starts directly as the ghost.

The host builds the `NecroPlayerRespawnManager` (new 0xE0, ctor 0x010A7E00, singleton `[0x02021628]`) and registers it with the session so it ghosts to clients. Type pick goes through `RequestNecroPlayer` 0x0108EAE0 (hooked); no mod packet carries the type or vent pick, and whether a client's pick reaches the host is not established. A client ghost safety patch at 0x010A57DD (`HandlePostSpawnInit`, `je` → nops) and a `GetPlayerByRef` answer of 0 without a joined game keep ghosts from reaching Blaze paths. Native nameplates are forced off around necros (MPLevelSettings plus nameplate crashed at 0x0086AEDE). `NPOL` coordinates loading the necro pool (`LoadPool=2`).

Cross-machine necro grabs were never observed in a test (coordinator ghost updates 0).

A dead human in story co-op can watch instead of restarting everybody (`[Spectate]`, off by default, `runtime_coop_spectate.inc`): his dead Player stays in slot 0, the engine's MP ObserverCamera (spawned and pushed without the ObserverPlayer wrapper) follows a living partner, and NROL role 4 makes every partner treat him like a necro at the vents. He comes back through the MP branch of ProcessDeath (`Player::RespawnPlayer`) when the next checkpoint is reached: a client by the host's checkpoint epoch, the host by a client's CKPT reach report (flags 2). The new body stands on the checkpoint's matrix (Checkpoint+0x60); the checkpoint is applied only after his inventory is restored.

In the story (`[Necro] Invasion`) a necro's next vent after a death opens `RespawnBaseSeconds x necros / humans` seconds later (at most `RespawnMaxSeconds`; NecroPlayerMasterSM transition id 6 is refused until then, the same hold as during a human's scene), with a countdown on the necro's own HUD line only. The heartbeat radar (`[Necro] Radar`, `RadarRange`) draws a beating heart over every human within range through walls for a necro alive or at the vents (`overlay::kMarkHeart`, measured from the ghost proxy at the vents).

### 2.13 Bots and their seats

`runtime_coop_bot.inc`. Bots are full local `Player` instances on the host (`[Bot] Count` up to 3): PlayerManager slot 1 + i (registered with `Register` 0x0077BB60, id at +0x420), controller pad 14 − i (Isaac uses 15), and their own CameraManager viewport. Seats: `min(4 − humans, 3)`, humans = the Versus human side or 1 + ready partners; a bot gives its seat up when a human joins. With partners, a human ghost that lands in a bot's slot is moved to slot 4 and up (`[Bot] MoveGhostsOut`). `[Bot] GhostToClients=1` registers bots as NetObjects with machine byte 0xF0 + i and sends their weapons (`CLDB`) and suits (`BSUT`); clients find them as ghosts of that machine. The lobby subtracts bots from the joiner seats (8 PlayerManager slots in total). Note from the 12.10 audit: bots and their ghosts also run in story co-op, where they compete with human ghosts for slots 1..3 (no session-mode gate yet).

Because bots are real Players, `Player::UpdateAimingTarget` threatens whatever a bot aims at (`ThreatenedComponent::SetThreatened`), exactly as for a human. That matters for the Severed twitchers, which dodge 0.25 s into an aim. The bot senses mark a twitcher by its pool GUID (`Enemy::dodger`), and with `[Bot] TwitcherSnapShots=1` the brain shoots at it without the human aim pause and settle (18.10.).

### 2.14 Disconnects and timeouts

`runtime_coop_disconnect.inc`, `[Coop] PeerTimeout=1` (template; code default 0), `PeerTimeoutMs` 30000. The engine already has the mechanism: `NetConnection::checkTimeout` 0x01419F50 (called from processConnections every 1500 ms) ends a connection after `mPingTimeout (+0xB0) × (mPingRetryCount (+0xB4) + 1)` of silence, removes it and its ghosts (and so the partner's body). Live values read out were 60000 ms × 30 on one side and 5000 × 16 on the other ("in practice never"). The plugin writes timeout/4 and 3 retries into every connection and drops the side-channel peer slot on the same deadline, which is what releases the gate, checkpoint and readiness quorums. A local stall (a loading screen) is credited back to every peer and to the engine (`clearPingTimer` 0x01418280), so a long load is not a departure. "CO-OP PARTNER LOST / CONNECTED / TIMED OUT" notices are shown.

### 2.15 High ping and the Internet

Notes in `analysis/COOP_HIGH_PING_2026-09-25.md`, `COOP_REMOTE_PLAY_2026-09-17.md`, `DS2_COOP_ONLINE_READINESS_2026-10-09.md`, `ONLINE_PLAYTEST_SYNC_AUDIT_2026-10-13.md`. What is done: ghost timeline (2.6), acked deaths, epoch ordering for suit/loadout/death, triple sends with dedupe for hits, latency-stretched windows. What is open: the template check's fixed window (one lost reply can drop that template's ghosts for about one ping), no `{level, restart}` generation in MIRR/VOTE/CHIT (a pre-restart packet can act after a restart), the journal cut-over can deliver twice, no build check on the direct launcher path, the seed is not sent. The first Internet tests (Hamachi, 13.10) were story co-op ch02, survival on mp_solar/mp_gov and Versus on Titan Mines.

## 3. How players connect

### 3.1 The menu flow

Everything is reached without the EA login. `runtime_coop_menu.inc` swaps vtable slots and label pointers only (no code bytes), each slot checked against its retail value first. It needs `[Menu] Enabled=1` and `[MpMap] Enabled=1`.

| Screen | What we changed | Address |
|---|---|---|
| PC01 main menu | item 1 ("multiplayer", relabelled "gamemodes") pushes FE03 with `Apt3dDirector::PushScreen(0xBE64F23A)` instead of `UIFrontendManager::Transition(3)` | OnInput_Select 0x00AA1C30, slot 0x01B392B0; PushScreen 0x00A879D0; director `[0x0208BE90]` |
| PC01 main menu | item 0 ("single player", relabelled "story") opens SP01 as usual | label table 0x01F5FFCC |
| SP01 (story / co-op) | any start (continue, new game, load, new game+) opens a campaign lobby as host and then runs the native select; item 4 ("previously on Dead Space") becomes "join co-op" | OnInput_Select 0x00AABE20, slot 0x01B38198 ([Menu] CampaignCoop=1) |
| FE03 Multiplayer | item 0 "start survival" (solo: `ConfigureSolo`, lobby closed, native select pushes MP20); item 1 "host co-op" (`lobby::StartHost` + `ConfigureAsHost`, then MP20); item 2 "join co-op" (`lobby::StartJoin([Coop] JoinAddress)`, stays on FE03, the label shows progress); item 3 "records" (overlay page from DS2SurvivalBest.txt); item 4 character setup FE16 when `[Loadout] Enabled=1`, else swallowed | OnInput_Select 0x00AA9DD0, slot 0x01B39540; labels 0x01F5FFEC |
| FE03 input | code 10 (X, "EA Account") and 12 ("log out") reach Blaze natively (NU04WelcomeBack reads the null hub, dump 2328); swallowed and turned into "how to play" / "about" overlay pages | HandleInput 0x00AAF9F0, slot 0x01B394E0 |
| MP20 map picker | Select never calls `BeginMatchMaking`; it reads the picked entry, asks the director whether the map/mode can run (`DS2Survival_Ready`), and queues the launch for the next game tick, or opens the native lobby screen (3.4) | OnInput_Select 0x00A67B50, slot 0x01B3A368 |
| MP20 map list | the Outbreak maps (if `DS2DAT12.DAT` is mounted) and the recorded campaign arenas are appended with `Array<MultiPlayerMapInfo>::Add` and `String::Set`, the way `LoadMapInfo` 0x00A99800 builds the list | Add 0x00A91CB0, String::Set 0x0073AAE0; DGM `[0x020213FC]`, list +0x22C, count +0x230, entry 0x54 bytes |
| MP20 mode row | up/down (or left/right on Mark's two-row movie with `[Menu] NativeModeRow=1`) steps the game mode; see section 4.10 | `runtime_coop_gamemode.inc` |
| SP02 difficulty | with `[Difficulty] Pick=1` the MP20 select pushes the game's difficulty screen (hash 0xE037EC08) first; its accept runs the launch above, back returns to MP20; see section 4.13 | Select 0x00AAC060, slot 0x01B382D0; Cancel/Show/InitMenuParams slots 0x01B382D4/0x01B3829C/0x01B382CC |

The labels "gamemodes"/"story" and the records, how-to and about pages are from 14.10 and installed 14.10., untested; the installed build still says "survival"/"co-op". The text changes go through our LCH2 override table (`runtime_coop_notice.inc`, `notice::AddOverride`): screen titles, the NAT warning lines (`[Menu] NatLine1/NatLine2`), the title screen line. The picker's launch itself runs from `menu::Tick` on the game thread (`coop::Poll`), never inside the menu's input dispatch: `mpmap::StartVisitFromMenu(map)` for an MP map, `arena::Start` for an arena. `StartVisitFromMenu` is our single player route into an MP map (override checkpoint, `GameMainLoop::RequestLevelChange`, game mode stays 0); it is described in section 4.11.

Credits block (`runtime_coop_credits.inc`, `[Credits]`, 18.10.): UICredits::Initialize 0x00A589C0 reads the section count from the text `CREDITS_Num` (307), and UICredits::Update 0x00A71450 builds each section from `CREDITS_%d` → name, `%s_Num`, `%s_Cols`, `%s_%d`. Every text key goes through HashString_SDBM 0x007423C0 (lower case, h × 0x1003F + c) and GetLocalizedTextStatic 0x01371280 (cdecl, char* or null). The table cannot grow, so the seven lookup calls inside those two functions are pointed at our `Text`: it adds our sections to CREDITS_Num, answers CREDITS_1..K with our section names and shifts the original CREDITS_i to i + K. The original credits stay unchanged; nothing outside the credits is affected.

### 3.2 The lobby protocol (`src/coop_lobby.h`, `src/runtime_coop_lobby.inc`)

Plain UDP on `[Coop] Port + 2000` (default 29015), one non-blocking socket that exists only while this machine hosts or joins from the menu. Version 4 (20.10.: the difficulty byte). All structs are `#pragma pack(1)`; a datagram is accepted only with exactly the right size, magic and version. A version 3 hello gets the Mismatch in its own 120-byte layout, so an older joiner shows the build refusal instead of waiting.

`Packet` ('LOBY' 0x59424F4C, 121 bytes):

| Field | Meaning |
|---|---|
| magic, version (4), type | type 1 Hello, 2 Welcome, 3 Launch, 4 Bye, 5 Full, 6 Mismatch |
| machine | Welcome/Launch: the joiner's machine number (1..7) |
| map | Launch: index into `coop_mpmap::kMapTable`, 0xFF none |
| token | the host's lobby id (`GetTickCount()*2654435761 ^ pid`), so a stale launch from an older lobby is never taken |
| seq | per sender |
| gamePort | the sender's TNL port (`[Coop] Port`) |
| members | Welcome/Launch: joiners known to the host |
| flags | Launch: `0x80 \| game mode` (0 survival, 1 mercenaries, 2 objective, 3 hold the zone, 4 versus) |
| build | the co-op build id: the PE link timestamp of the module (`BuildId`) |
| randomizer | the randomizer build id (`RandomizerId`; in DS2United.asi both halves are one module, so both are the same stamp) |
| reserved | Hello: bit mask of the maps this joiner can load; Welcome/Launch: the seq of the hello it answers (for the joiner's RTT) |
| roster[7] | Launch: `{ip, gamePort, machine}` of every joiner as the host sees it |
| profile | Hello: `{name[24], xp, pingMs}` |
| difficulty | Launch: the host's difficulty 0..4 (0xFF none, `[Difficulty] Sync=0`); the joiner writes it right before its level request |

`StatePacket` ('LOBS', 308 bytes): the lobby list, host to every joiner every 250 ms while no map is launched: map index (0xFF = host not on the lobby screen), mode flags, player count, countdown ms (-1 waiting), and 8 rows `{machine, team, flags (used, host), xp, pingMs, name[24]}`. Rows are in the native MP02 order: host first, then joiners by machine; in Versus the first `[Versus] Humans` players (0 = half, rounded up) are humans and fill rows 0..3, necromorphs rows 4..7, because the native list has no team column.

`CampaignPacket` ('LOBC', 228 bytes): the story co-op launch: token, machine, build ids, game port, the host's difficulty + 1 (0 none; the former pad), the death/override checkpoint GUID, map name and stream list name (64 chars each), roster.

Sequence:

1. Joiner: Hello every 500 ms to `JoinAddress` host IP, port `hostGamePort + 2000`, carrying its build ids, its map mask (`mpmap::AvailableMask()`, the Outbreak maps need `DS2DAT12.DAT`) and its profile card. After 10 s without an answer an overlay hint names the usual causes.
2. Host: a Hello with a different `build` or `randomizer` gets Mismatch. Otherwise `Host::Admit` gives the lowest free machine number, keyed by ip:port; a hello from a known ip:port refreshes it. The cap is `7 - bots` (`JoinerSeats`: the host's bots take PlayerManager slots too, 8 in total). A full lobby answers Full. The reply is Welcome before the host launched, Launch after. Joiners silent for 6 s are expired before the launch.
3. Host picks a map: `HostLaunched` builds the roster, calls `ConfigureHostDials` (every joiner goes into the TNL dial list), and sends Launch to every joiner every 300 ms for 60 s. After that the lobby socket stays open while the host is on the map, so a late joiner's Hello is answered directly with Launch.
4. Joiner on Launch: refuses the map if it is not in its own mounted archives, then `ConfigureAsClient(hostIp, hostGamePort, machine, roster)`, adopts the host's game mode (`gamemode::mode`), asks its own director for readiness (a warning only: a client runs no waves, but its director drops its own wave rewards and capsule copies), and calls `mpmap::StartVisitFromMenu(map)`. The lobby socket closes; from here on the side channel and TNL own the connection.
5. Bye (either side, when leaving) frees the seat or closes the joiner's lobby screen.

Joiner refusals are mapped to overlay hint cards ("the host runs a different DS2Coop build", "lobby is full", "host closed the lobby", map missing). The ping shown in the lobby is the joiner's own Hello to Welcome round trip, smoothed 3:1; the signal bars follow the native `GetPlayerSignalBar` 0x00A53230 thresholds (133/233/333/500 ms, +9 on the host row).

### 3.3 Machine numbers

Machine 0 is always the host (`IsHost()` is `machineIndex == 0`, `runtime_coop.inc`). Joiners get 1..7 from the lobby. The number is not cosmetic: it is the top byte of every NetObject sub-key this machine mints (section 2.3), so two machines must never share one. When the host hosts again inside a running session, `HeldMachines` marks the numbers of partners the session still hears (heard within 15 s) as taken, so a new joiner cannot get a ghost-subkey that is still alive. With the direct launcher path (`DS2COOP` environment variable) the number comes from the launcher instead.

### 3.4 The native pre-game lobby screen (`[NativeLobby]`, off by default, untested)

`runtime_coop_nativelobby.inc` puts MP00 (`MP00LobbyBase`, director hash 0x57808EE7) with its MP02 tab (8 player rows) in front of our lobby. The full evidence is in `analysis/DS2_NATIVE_LOBBY_2026-10-11.md`; the parts in short:

- Host: MP20 Select calls `nativelobby::OpenFromPicker` instead of launching. It writes the end-of-match byte `[0x02089C29] = 0` (pre-game), calls `IGameManager::SetStreamList` 0x013780C0 with the entry's exact path text (so MP02 OnShow resolves the map itself), and pushes MP00 with `PushScreen`. Never `Transition(6)` or 0x00A92EF0, which rebuild the stack with NU00.
- Joiner: on the FE03 or MP20 screen, once the host's `StatePacket` names a map it can load, the game tick opens the same screen.
- Hooks: mid hook 0x00A6559C (MP00 OnShow takes the online branch), mid hook 0x00A49B12 (MP00 Tick draws the countdown without a joined game), inline 0x00A658B0 (MP02 Tick fills the 16-byte rows from our `StatePacket`, names in static buffers because Apt reads the pointers on every redraw), inline 0x00A49C30 (`ConditionalChangeTab` refuses the stats/settings tabs, which reach the BlazeHub), slot swaps for MP00 item state (0x01B37524), MP02 input (0x01B375B8, swallows X invite and Y mute), MP02 FSCommand (0x01B37628, `invi`/`mute`), MP02 select (0x01B37618, host "start now") and the quit popup result (0x01B2EBB8, Yes leaves our lobby and pops back to FE03).
- Campaign arenas (16.10., `[Arena] Modes`): `OpenFromPicker` takes an arena row too; the map byte (state and launch packet) is `0x80 | arena index`, `EntryFor` finds the arena's list entry for the joiner, and the weapon/suit tabs use the mode the arena plays. MP02 `SetInfo` 0x00A49CF0 only bounds the index against the list count, so an arena shows its texts and possibly no or a wrong picture.
- Countdown: `MPLobbyManager [0x020213E8] + 0x18` is written every tick by us; it waits at -1 until the host clicks LAUNCH (our MP02 help bar button `'lnch'`, as in your RIG-LINK) or presses select, then runs `LaunchSeconds` (3); with `AutoStartSeconds` above 0 it also starts once `MinPlayers` are in. At zero the host calls `StopLobby` 0x00A54010 and launches through the normal picker route (`menu::launchPending`). Joiners follow the Launch packet. A refused launch (a joiner lacks the map) puts the host back on the lobby screen.

### 3.5 Story (campaign) co-op

`[Menu] CampaignCoop=1`. Whoever starts or loads a game in SP01 hosts (`StartHost(Mode::Campaign)`); the network switches on when the first joiner says Hello (`ConfigureAsHost`) and the host gets the notice "PLAYER n WANTS TO JOIN - RESTART A CHECKPOINT TO LET THEM IN". There is no late join into a running campaign level: at the host's next level load or checkpoint restart (level manager leaves RUNNING), `CampaignHostTick` reads the map, stream list and checkpoint (the override GUID if a save was loaded, else the death checkpoint) and sends `CampaignPacket` every 300 ms until 10 s after it runs again. The joiner then calls `ConfigureAsClient`, arms the suit hooks and his join suit (the chapter's start suit for that checkpoint, or the suit in his client progress file for this host once the host's id arrives; `Player::Init`'s own dress at 0x008A866B puts it on), adopts the host's seed and difficulty from the packet and calls `mpmap::StartCampaignFromMenu(map, stream, checkpoint)`. The host's loading screen holds for him (section 2.9). The joiner's own saves are not touched. A client in the main menu whose session still runs and who chooses join co-op again waits there for the host's next restart (`[Level] JoinOnHostRestart=1`, the host gets the same notice) and loads with it. Once in, `[Level] FollowHost` and `[Checkpoint] FollowHost` keep the client on the host's map and restart point (section 2.8).

### 3.6 Addresses, LAN, virtual networks, Internet

IPv4 only. Three UDP ports per machine: `Port` (TNL, default 27015), `Port + 1000` (side channel, 28015), `Port + 2000` (menu lobby, 29015). The +1000 offset replaced +1 after two copies on one PC collided (host data 27016 == client TNL 27016). `JoinAddress=ip:port` names the host's game port. Tested over LAN, loopback (two copies on one PC with `DS2COOP_PORT`) and Hamachi (13.10, story co-op ch02 and survival on mp_solar/mp_gov, Versus on Titan Mines). There is no NAT traversal and no relay server: the host must be reachable on all three ports (Tailscale/ZeroTier/Hamachi or forwarding), and because TNL bodies need a full mesh every joiner must also reach every other joiner's TNL port. The side channel can fall back to the host relaying most packets (section 2.5). `ParsePeer` takes IPv4 literals only, no host names. The menu's NAT lines say "Host or join over LAN (or virtual network)".

### 3.7 Late join, rejoin, hosting again

| Case | What happens | Status |
|---|---|---|
| Late join into a running MP map | the lobby stays open while the host's visit runs; Hello is answered with Launch; the host's `HostTick` logs `late_joins`. The joiner dials the host and every roster member it was given; the host adds the new joiner to its dial list only through `ConfigureHostDials`, which runs at the launch | Partial. Objective steps reach a late joiner through the host's journal (`runtime_coop_objonce.inc`); "Late joiner spawns at a point near a partner" is a 13.10 fix, untested |
| Rejoin after leaving the host's MP match | the client's session keeps running in the main menu; "join co-op" becomes "rejoin co-op: <map>" and calls `StartVisitFromMenu` on the host's map without a lobby | New 13.10, untested |
| Client back in the menu while the host loads a new level | `[Level] FollowFromMenu=1`: the client follows the host's next level load | Unclear (template on, code default off; FEATURES only rates checkpoint and level follow as a whole: Works 17.09) |
| Host hosts again after a match | `HostSessionRunning()`: the same TNL session continues, partners still connected stay, new joiners are added to the dial list and connected by the redial loop | Built 13.10, installed 14.10. |
| Client wants to host | refused: one TNL session per game start, a client session cannot turn into a host session; restart the game | By design |
| Campaign joiner after the level started | waits for the host's next checkpoint restart; the host's loading screen holds for him (`JoinTimeoutMs`) | By design; barrier 02.11., untested |
| Arena joiners | the launch's map byte carries 0x80 \| arena index (`coop_lobby::ArenaCode`), the joiner runs `arena::Start`; a rejoin from the main menu still loads the level as plain campaign | Built 16.10., untested |

### 3.8 What happens at launch on each machine

Host: lobby announces → `StartVisitFromMenu` → level change to the MP map in single player → `[MpMap]` blocks the online-only MP logic, spawns Isaac at an MP spawn point (no MP map carries a player) → after `[MpMap] SettleMs` (1500) on the map the co-op session ticks (players see each other, the host builds the necro respawn manager) → the director arms (section 4.1).

Client: same level route; its session dials the host and the roster; `[Loading] SyncWithPartners` makes it wait at the loading screen for the others (section 2.9); its director does not run waves (`DS2Coop_NetRole() == 2`), but still takes reward and capsule events from the host.

## 4. The survival director

Source: `src/runtime_survival.inc` (namespace `survival_rt`, the engine side) and `src/survival.h` (pure logic: config, wave plans, families, queue, spawn rules, stuck verdict, scoring, records). Settings: `DS2Survival.ini` sections, or the same sections in `DS2United.ini`. Design note: `analysis/DS2_SURVIVAL_2026-10-09.md` (partly superseded: its wave 10, alive caps and reward drop point have changed; the code is current).

The director runs in the randomizer half because it needs the global enemy pool, the forced-donor lookup hook and the FuncSpawn repeat machinery. Every enemy it makes is a pool donor driven through one of the map's own FuncSpawns; nothing is created from scratch.

### 4.1 Installation, arming, solo/host/client

Install (`survival_rt::Install`, end of the file) parses the config, checks 20 signature sites (10 required), installs the cleanup hooks, builds the donor tables, and only then sets `installed`. It requires the selection lookup and hook, `[DS2Randomizer] GlobalTemplates=1` (the pool), the FuncSpawn constructor registry hook and the spawn multiplier hook. `[Survival] Enabled` (code default 0, template 1) is read before the randomizer's hooks go in; with the randomizer master switch off and survival on, the randomizer runs "survival-only" (chance 0; MoreEnemies, hordes, extra spawns, loot, bench/schematic randomizers and the campaign watchdog stay off). Whether the director can run is published through `DS2Survival_Ready` (1.2) with a human-readable reason and fix.

Who runs it: `DS2Coop_NetRole()`. Solo (0) and host (1) run waves. A client (2) never arms; it logs "co-op client on <map>: the host runs the waves", drops its own wave rewards when the host's reward event arrives, and keeps local copies of the host's capsules (4.9).

A run arms (`TickInner`, Off branch) when all hold: the current map is allowed (`[Survival] Map`, `any` = every `mp_*`/`pdlc_mp_*` map and every `arena_*`), the level manager `[0x0201C0A4]+0x18` is 16 (RUNNING), a local Isaac stands in PlayerManager slot 0 with health > 0, and no Objective checkpoint restart was asked in the last 30 s. The mode comes from `DS2Coop_GameMode()` (a campaign arena plays Mercenaries or Hold the Zone with its built-in points, `[Arena] Modes`, everything else as survival).

State machine (`enum State { Off, Arming, Wave, Intermission, Dead }`):

| State | What happens |
|---|---|
| Arming | waits for the pool (`GlobalTemplates`, the stream thread is the current thread, donors allowed), probes which families the map can field, then starts after `StartDelaySeconds` (survival 20 s, clock modes 3 s) or on `StartKey` (107, Numpad +) |
| Wave | spawn step, `PollTracked` every 500 ms, last-enemy marks, timed end; ends when the queue is empty and nothing is alive (the Ubermorph does not count) |
| Intermission | countdown (`PauseSeconds` 20, shortened by escalation), notices at 10 and 5 s |
| Dead | On an MP map every run end reloads the map from its checkpoint: Objective result; Zone/Mercenaries result (`RestartAfterResult=1`); game over after `GameOverSeconds` (10) (`[Survival] RestartOnGameOver=1`); the host's restart. The next run arms on the reloaded map. Campaign arenas reload their arena checkpoint the same way since 16.10. (the override set at the launch); a refused restart: end the run, next run arms on the respawned Isaac |

Each state change is reported to the co-op half (`DS2Coop_SurvivalEvent(3, (wave << 8) | state)`, state 5 for a running clock mode) for music and the shop. A run also ends when the map changes, the level manager leaves RUNNING, a new Isaac appears without a pending respawn (checkpoint restart), or the stream probe calls the level-release hook (`survivalLevelEnding`). Respawns are tolerated through `DS2Coop_RespawnState` (the run adopts the new body). `DS2Coop_TeamDown()` during a wave means game over.

Driver: `FrameTick` from the frame hook 0x007701C0 (`runtime_frame_cap.inc`); `MachineTick` from the StateMachine::Update hook at 0x013E2CF0 stands in when the frame tick has not driven for 1 s. Both only act on the pool's stream thread, because `GlobalDonorAllowed` refuses pool donors on any other thread. The director ticks at most every 100 ms. If the game clock `[0x0204D3C0]` stands still while a run is on (pause menu), the director does not tick and subtracts the paused time (fix for a crash after a 4-minute pause, 13.10). Faults inside the tick are caught (SEH) and logged.

### 4.2 Waves and the queue

`[WaveN] Enemies=Family:count[:path[:pack]]`, path `any` (default) / `vent` / `direct`, pack 1..6 arrive together at one spot; optional `MaxAlive`, `SpawnIntervalMs`, `TimeLimit` (the wave ends on the timer, `TimedEndKill=1` kills the rest), `Pause`, `Enhanced` (% of enhanced variants). Up to 50 waves; a wave with `Hunter` is a boss wave (90 s default limit). Excluded families: Guardian (and parts), Nest, Tripod, Kong, NicoleBoss, DragTentacle, DividerPart, Landmine.

Template waves:

| Wave | Enemies |
|---|---|
| 1 | Zombie:6 |
| 2 | Zombie:8 |
| 3 | Zombie:10, PackEnemy:4 |
| 4 | Zombie:12, Leaper:4, Puker:3 |
| 5 | Brute:1:direct, Exploder:6, Zombie:4 |
| 6 | Lurker:3, Infector:2, Stalker:2:direct:2, Zombie:6 |
| 7 | Lurker:4, Infector:3, Stalker:3:direct:3, Zombie:8 |
| 8 | Pregnant:2:direct, Puker:4, Exploder:4, Zombie:6 |
| 9 | Brute:2:direct, DividerCore:1:direct, Zombie:8, Leaper:4, Puker:3 |
| 10 (boss) | Hunter:1:direct, DividerCore:2:direct, Pregnant:1:direct, Stalker:4:direct:2, Lurker:3, Puker:3, Exploder:4, Leaper:4, Zombie:10; Enhanced=50, MaxAlive=14, SpawnIntervalMs=2500, TimeLimit=120 |

After the authored waves (`WaveFor`): every `BossEvery`-th wave (10) repeats the boss wave, the others cycle `EndlessCycleFrom..EndlessCycleTo` (4..9) with counts × (1 + `EndlessGrowthPercent` 20 % × waves past the last), capped at 60; the Ubermorph is never multiplied and never doubled (it stays tracked across runs on a level that keeps running).

Queue (`BuildQueue`): heavies and the boss first, whole packs together; the rest interleaved by the share of each entry still left. Pack followers spawn 250 ms after their head, 1.3 m around it. Pacing: nothing before `WaveStartDelayMs` (2500); one spawn per `SpawnIntervalMs` (1500) while alive < cap; an item that cannot be placed for 30 s is dropped so the wave can end.

Families, roles and substitution (`kFamilies`, `Substitute` in `survival.h`): a family the map cannot field now is replaced by its first available stand-in, else any available swarm family; a heavy or boss replaced by a non-heavy comes ×3. Availability (`Probe`): a family is available when a direct donor resolves on the pool stream or a live vent spawner has a donor for it. `StalkerNeedsCover=1` removes the Stalker on maps without CoverNodes (all MP maps, every arena except ch04): it only hides and peeks from cover nodes and stood around without them.

| Family | Role | Stand-ins in order |
|---|---|---|
| Zombie (slasher) | swarm | PackEnemy, Leaper, Infector |
| PackEnemy | swarm | Zombie, SwarmEnemy, Leaper |
| SwarmEnemy | swarm | PackEnemy, Zombie |
| Leaper | rusher | Stalker, Zombie, PackEnemy |
| Stalker | rusher | Leaper, Zombie |
| Puker | ranged | Lurker, Exploder, Zombie |
| Lurker | ranged | Puker, Leaper, Zombie |
| Exploder | explosive | Puker, Zombie |
| Infector | special | TubeBeetle, Leaper, Zombie |
| TubeBeetle | special | Infector, PackEnemy, Zombie |
| Brute | heavy (pool donors with both packages forced again since 14.10.; before that always the stand-in) | DividerCore, Pregnant, Stalker, Zombie |
| Pregnant | heavy | Brute, Puker, Zombie |
| DividerCore | heavy | Brute, Pregnant, Zombie |
| Hunter (Ubermorph) | boss | Brute, DividerCore, Stalker, Zombie |

### 4.3 Spawn paths

Path choice (`WantsVent`): `vent` items try a vent, `direct` never, `any` tries a vent with `VentShare` (40 %) chance. The vent path needs `[Survival] VentSpawns=1`, `[DS2Randomizer] VentRebinding=1` and the two spawning-component binding mid hooks (0x00818F60, 0x00F64B20). A vent item without a free vent for `VentWaitMs` (15 s) goes direct.

Both paths end in the same engine call. `DriveForcedSpawn` (`runtime_more_enemies.inc`) checks the FuncSpawn vtable 0x01B59C58 and its HandleEvents 0x00ECB1D0, parks the spawner's count with a sentinel, hides its completion message and calls HandleEvents with the trigger message. The selection mid hook `OnLookup` at 0x00ECB2B5 swaps in the forced pool donor; `OnCreated` at 0x00ECB2D0 reports the actor (and repairs templates without nav routing). A spawn `OnLookup` cannot serve has its SpawnInfo zeroed, so the spawner builds nothing.

**Vents.** `src/generated_survival_vents.h` (built by `tools/build_survival_vents.py` from the level catalog and the Outbreak images) holds per map the `spawn_slasher_N` FuncSpawns (bombRun 5, detonate 5, escape 5, gov 5, solar 4, mall 5, school 3) with their vent, context (`SPAWNMODE_VENT|Wall` or floor) and the message window in which the map has them selectable; all 177 VentMarkers (wall flag, position, window); the prop FuncSpawns used as direct-spawn engines; and 74 pool vent donors with their arrival key. A pool record serves a slasher vent when context and arrival key match (same family, or any with `VentCrossFamily=1`). The map's own slashers are blocked from spawning, but `[MpMap] KeepSlasherRecords=1` lets their spawn records register so these FuncSpawns exist.

Per candidate marker (`TryVent`):

- the spawner is usable and its target resolves; spawner and marker each rest `VentGapMs` (3500) between uses;
- the vent is open: `vent_gate::Open(marker)` replays the map's own window logic from the messages seen at the dispatch (`runtime_vent_gate.inc`); vents behind doors the map has not opened stay shut; in Hold the Zone/Mercenaries, once the objectives are opened, every vent counts as open;
- distance to the nearest living viewer within `VentMinDistance..VentMaxDistance` (0..30 m);
- height rule: a wall vent more than `VentMaxAbove` (2.5 m) above the walkable floor below it is left to the necro players (measured once per marker from a short NavPower path that snaps onto the mesh);
- reachability (`VentMustReach=1`): a NavPower walk from the nearest player's feet must end within 2.5 m of the vent's floor (catwalk perches stay shut); cached 20 s;
- optional line of sight (`VentLineOfSight=0`);
- the least recently used marker wins.

With `VentAnyMarker=1` an enemy can use any open marker of the right kind, not only the five the map's slashers use. For the spawn the director sets `ventRedirect`, and `OnLookup` rebinds the donor to the slasher's spawning component and writes the chosen vent GUID into the source spawning header (+0x18), so the donor climbs out of that MP vent. 3 s later the arrival is checked (within 6 m of the chosen vent).

**Direct spawns** (`TryDirect`, `SearchStep`): seeds around a rotating anchor (each living viewer in turn, bots included): 12-spoke rings at Min+2, Ideal and Max−1.5 m behind the anchor first, rings ±4.5 m in height, the map's `MultiplayerSpawnPoint`s (static list 0x020218A0, count 0x02021920), arena spots, and up to 24 remembered good spots. Per candidate (3 per tick):

1. NavPower walk anchor → seed (`bfx::CreatePolylinePath` 0x005801B0 on the level nav space); the path end is the candidate;
2. within `FloorBand` (12 m) height, `MinDistance..MaxDistance` (12..25 m, `BotMinDistance` 8 m from bots), path length ≤ distance × `MaxPathFactor` (2.2) + 4;
3. headroom (vertical cast 0.3 → 2.0 m), wall clearance (`WallClearance` 0.3 m: 8 waist-height rays, pushed out and re-snapped, else rejected);
4. unseen: casts from every viewer's eye to the spot at +1.0 and +1.8 m (`CollisionCamera::CastingTest` 0x00F5B180); a faulted cast counts as seen.

Score: closeness to `IdealDistance` (16 m), behind the anchor, `OtherFloorBonus` for another floor, away from recent spots. When one item waits long, the rules relax: 4 s distances ×0.6; 8 s 3 m from everyone, unseen; 14 s unseen only by viewers facing it; 22 s sight ignored. The engine is the first usable prop FuncSpawn of the map's main stream; the donor takes the prop's place and `ApplyPlacement` moves it (entity vtable +0x20/+0x24, movement component 0x00876E00, rotate 0x00F63540, position 0x00F638A0). An enemy that lands more than 5 m from its spot is placed again (4 checks in 2 s).

### 4.4 The enemy pool

The pool is the randomizer's own loose stream `randomizer\ds2_randomizer_pool.str/.toc` (size and FNV-1a fingerprint checked), requested through the stream probe and kept resident across levels; its records are copies of retail spawn records from every chapter with the last two GUID words replaced by the pool tag `4D444E52-4C4F4F50` (`global_pool.h`, `runtime_stream_probe.inc`, `runtime_guid_redirect.inc`). Direct donors are pool records with a private template, a model and a direct context, by family; super (`super_*` package) variants go to the enhanced list unless `SuperVariants=1`. One direct super slasher record (48CECFBF-FD12B2B2-...) is skipped because it authors health 0. Donors are taken round-robin per family, skipping any that failed three times this session. Escalation picks an enhanced donor with its chance (4.6).

This matters for co-op: a client renders the host's enemies only if it has the same templates resident, which is why the lobby compares the randomizer build and FEATURES lists "same pool files" for clients.

### 4.5 Caps

| Cap | Value | Source |
|---|---|---|
| Alive | wave `MaxAlive` or `[Survival] MaxAlive` (15), + `[Escalation] MaxAlivePerPlayer` (3) per further body (partners and bots), up to `MaxAliveCap` (23) | `WaveCap`, `MaxAliveFor` (13.10, installed 14.10., untested; before: 5 per player, cap 30) |
| NavPower | alive capped at 10 when the NavPower pool was not enlarged | `EffectiveCap` |
| NavPower headroom | no spawn while top chunk + dv chunk of NavPower's dlmalloc < `NavPowerReserveKiB` (96); the gate switches itself off after 30 s low with nothing alive | `NavHeadroom`, `[0x0202F4CC]` |
| Chargers | global charger max `[0x01F612AC]` raised to `MaxChargers` (2) for the run, restored after | `ApplyChargeMax` |
| Versus slashers | `Alive` 5 (Academy 3) + pressure | 4.12 |
| Pickups | 40 soft, 80 hard (4.7) | `[Cleanup]` |

The NavPower pool: retail creates a 576 KiB (0x90000) block; at about 160 nav entries it hits an int3 (dump 2924, 0x005B480D). `runtime_navpower.inc` inline-hooks `bfx::SystemCreate` 0x005B4620 and hands it a `[DS2Randomizer] NavPowerPoolKiB` block (4608 KiB). It must be installed before the system is created; otherwise the first level keeps retail and the director falls back to 10 alive.

### 4.6 Escalation

From `[Escalation] FromWave` (11) each wave is one step. Applied per enemy 400 ms after its spawn (`Escalate`):

| Key (default) | Effect |
|---|---|
| `EnhancedPerWave` (10) | % per step that an enemy with an enhanced variant comes as one (100 at step 10); a wave's own `Enhanced` wins when higher |
| `HealthPerWave` (6) | health and every limb health % per step (HealthComponent +0xE4/+0xE8, the 11 zone healths of the dismemberable component) |
| `SpeedPerWave` (2), `SpeedMax` (40) | animation speed through the SetAnimSpeed mid hook 0x00751F40 |
| `PauseStepSeconds` (1), `PauseMinSeconds` (8) | shorter breaks |
| `RewardStepPercent` (5), `RewardMinPercent` (40) | smaller rewards |
| `PriceStepPercent` (5), `PriceMaxPercent` (200) | higher shop prices |
| `BotDamageStepPercent` (3), `BotDamageMinPercent` (50) | weaker bot hits |
| `BossStasisResistPerBoss` (20), `BossStasisResistMax` (80) | stasis resistance of Ubermorph and Brute per boss wave reached (hook on `StasisManager::StasisEntityInternal` 0x00BB1B40) |

The co-op half reads the per-wave values through `DS2Survival_EscalationFor`; on a client the wave comes from the host's music state.

Hunting (`Awareness`): every enemy is forced aware every `AwareRefreshMs` (3000) by setting flag 4 at AwarenessComponent +0x724; templates that track nobody get the player group (`RepairBlindTemplates`); slasher templates get a charge floor (`ZombieChargeFloor` 0.9). Since 17.10. a charge starts as in the retail game: `Zombie::SetChargingMode` 0x012C9AE0 needs the NPC on its target's screen (`IsOnTargetsScreen` 0x00FE4960: the local player's view frustum, a cone along a remote player's facing) unless the template's ChargeIgnoreVis (+0x1CC) is set, and `UpdateApproachTarget` 0x00FFD800 needs the target in the NPC's sight; `ZombieChargeIgnoreVis` and `ChargeUnseen` (which lifted these) now default to 0. Template edits are undone at run end where our value is still in place.

### 4.7 Watchdog, catch-up and cleanup

`PollTracked` every 500 ms per tracked actor (`survival.h` `Observe`):

| Verdict | Rule | Action |
|---|---|---|
| vanished | vtable or GUID changed | counted |
| killed | state machine in the dead table, or health 0 | counted |
| inert | no state machine after `InertSeconds` (20) | written off, no kill |
| vent_stuck | a vent state machine (DeadSpaceNPCVentSM, EnterVentSM, ExitVentSM, VentIdleSM, NavVentLinkSM) still running after `VentStuckSeconds` (30) | killed |
| stuck | no progress (2 m closer, or within `EngageMeters` 10) for `StuckSeconds` (60) | killed; never the Ubermorph |

Kill = `HealthComponent::SetHealth(0)` (0x00F84C60, vtable slot +0x20, the call the campaign watchdog has used in production); at most 3 kills per poll.

Catch-up (13.10, installed 14.10., untested): during a wave, not in Versus, not for Brutes or the Ubermorph, not in a vent, not in the first 4 s: an enemy farther than the catch-up distance from every living player is removed at once (no reward) and its item re-queued as `any`, so it comes back near the players. The distance is 50 m solo down to 40 m with four real players (bots do not count), never below max spawn distance + 2 m.

`[Cleanup]` (every machine, MP maps, game time): watchdog corpses are deleted (`Base::Delete` 0x007568B0) 3 s after the kill unless a player is within 10 m; stalled delete-fades are finished (`Entity::FadeOut` mid hook 0x007D3220); dynamic pickups above 40 are faded oldest first (not younger than 60 s, not within 8 m of a player below the hard cap of 80, capsules never). Added after crash 16592 and the FPS loss in long runs; untested.

### 4.8 Rewards, shop, music, records

Wave rewards (`DropRewards`): at the local Isaac's feet (the INI comment still says "nearest spawn point"; the code drops at the player): `RewardAmmoPerGun` (2) ammo packs per carried gun, `RewardMedPacks` (1) of size `RewardMedPackSize` (medium), `RewardStasis` (1), `RewardCredits` (1000) as credit chips, each scaled by the escalation reward percent. Engine path: `GetGlobalPickupItem` 0x00DE7560, `Item::Drop` 0x00DD7660 (vtable +0x1F0). A downed player gets his share when back. In co-op every machine drops its own share next to its own Isaac (pickups are not ghosted): the host's director calls `DS2Coop_SurvivalEvent(2, wave)` → `survnet` kind 2 → each client's director drops locally.

Supply terminal (`runtime_coop_shop.inc`, `coop_shop.h`, panel drawn by our D3D9 overlay `runtime_coop_overlay.inc`; `[Shop]` in the co-op INI): key B; open before the first wave and between waves (`DuringWaves=0`), any time in Objective and Zone, never in Versus or Mercenaries. Tabs: supplies (ammo per carried gun, med packs, stasis, supply drop), weapons (Plasma Cutter 1000 … Contact Beam 4000), upgrades (nodes), suits, gadgets (turret, stasis mine, heal field). Prices scale with escalation. Paid with the local Isaac's credits; guns are delivered through a GunProxy pickup and `Quartermaster::RequisitionGun` (refunded if the stream never arrives), suits through `ChangeToUniform` 0x00EE0610. A client's terminal follows the host's wave state (`survmusic` state from survnet kind 3).

Music (`runtime_coop_survmusic.inc`, `[SurvivalMusic]`): driven by the director state (kind 3 events; the host repeats it every 10 s). While a wave runs: a combat track, then action and combat alternating as in an MP round, stopped `StopDelaySeconds` (2) after the wave. Source `auto`: the eight MP round streams (`aud_play_stream_mus_01..08`, stop `aud_stop_current_music`; need `[MpMap] AllowMpMusic=1`) when the map has them, else the campaign's `mus_action_01..04` / `mus_tension_01..06` sections. The messages are sent through the dispatch 0x00756C60. The source notes that the MP streams never spawned in a visit (13.10), so the campaign source is what actually plays. Untested.

Records: `plugins\DS2SurvivalBest.txt`, `key value` per line, only raised: `<map>` best wave, `mercenaries:<map>` best score, `objective:<map>` seconds left on a win, `zone:<map>` zones held. The Multiplayer screen's "records" page reads it (installed 14.10., untested).

### 4.9 Co-op: what the client sees

The host's director spawns everything; the enemies reach clients as TNL ghosts of host-owned NPCs (`[Coop] HostNPCs`, section 2.6). Everything else crosses the side channel as `survnet` packets ('SVNT', 156 bytes, host only, `runtime_coop.inc` around line 860):

```
#pragma pack(1)
struct Packet { uint32 magic; uint8 version /*1*/, kind; uint16 repeat; uint32 token, seq; char text[140]; };
// a uint32 value rides in the last 4 bytes of text
```

| Kind | Content | Sending |
|---|---|---|
| 1 | notice text (`DS2Coop_QueueNotice`) | 3× 100 ms apart |
| 2 | wave ended, value = wave: every machine drops its own rewards | 3× |
| 3 | `(wave << 8) \| state` for music and shop | 3×, repeated every 10 s |
| 4 | HUD line and event line, `line\x1Fflash` | on change and once a second |
| 5 / 6 | Mercenaries capsule placed / gone: `{u32 id; float pos[3]; u8 golden}` (id 0 = all) | 3× |
| 7 | Hold the Zone `{u32 id; float pos[3]; float radius; u8 state; u8 percent}` | change 3×, 1 s refresh 1× |
| 8 | last enemies of a wave: count + up to 8 × xyz | on change and once a second, 1× |
| 12 | host tool for every machine (debug menu HOST tab, 14.10.): `{i32 command; i32 value; float hostPos[3]}`, commands 1 heal, 2 respawn the downed, 3 supply drop + stasis, 4 credits +value, 5 gather at hostPos, 6 god mode value | 3× |

The director's host tools do not cross the wire: the debug menu asks `DS2Survival_Admin(command, value)` (1 start now, 2 jump to wave, 3 end wave / skip break, 4 spawn kind index next, 5 hold, 6 max alive, 7 restart run), the director runs them on its tick (`survival_rt::AdminStep`), and `DS2Survival_AdminInfo` returns its state line and the last result. A run a tool shaped saves no best (`UpdateBestFor`).

Receivers deduplicate by seq (ring of 16). There is no acknowledgement: a lost "capsule gone" leaves a stale capsule on a client. The client's own director runs only these follower duties (rewards, capsule copies; when the host sees a partner within 2 m of a capsule it counts it as taken). The overlay draws the HUD line (`[Hud]`), waypoints for capsules, the zone and the last enemies (`[Marks]`).

HUD lines: `WAVE n - k LEFT`, `WAVE n - SURVIVE n S`, `WAVE n CLEARED - NEXT WAVE IN n S`, `GAME OVER - WAVE n - BEST b - NEW RUN IN n S`; Mercenaries `m:ss | KILLS | COMBO | SCORE | CAPSULE n M`; Objective `OBJECTIVE k/5 | m:ss | ...`; Zone `ZONE n | m:ss | HELD | p % ...`.

Respawn in co-op (`[Respawn]`, template `Enabled=0`, installed on; `coop_respawn_rules.h`, `runtime_coop_respawn.inc`): solo, `Lives` respawns per wave; with another human up, back when the wave is cleared; everybody down = game over (`DS2Coop_TeamDown`), the run restarts from wave 1 without reloading the map; in clock modes back after `DelayMs`. LIFE packets (18 bytes) carry each machine's up/down state; `DS2Coop_HumanDeaths` counts deaths for the clock penalties.

### 4.10 Mode selection

The mode is picked in the map picker (`runtime_coop_gamemode.inc`): MP20's one row leaves up/down free, so up/down cycles the mode and the row reads "titan mines [mercenaries]" (MP20 HandleInput 0x00A7E7D0 via slot 0x01B3A308, SetInfo 0x00A4BA90 hooked). With `[Menu] NativeModeRow=1` and Mark's MP20 movie (from a frontend override `randomizer\ds2coop_frontend.str` that the plugin serves instead of `frontend\frontend.str` through the file-open function 0x0056F3C0; the archives are untouched), a real "Game Mode" row sits above the map. The joiner takes the host's mode from the launch packet flags. Modes: 0 survival, 1 mercenaries, 2 objective, 3 hold the zone, 4 versus.

The difficulty row (20.10., built, untested; `analysis/MP20_THIRD_ROW_2026-10-20.md`): with `[Difficulty] Pick=2` the four MP20 ctor immediates (0x00AAB7B5, 0x00AAB830, 0x00AAB836, 0x00AAB84A) get 3 instead of 2, and the SetInfo tail fills entry 2 = {difficulty name, difficulty} after {mode text, mode} and {map label, map}. Left/right on row 2 calls `difficulty::SetPick`, so the row holds its pick and the select skips SP02. Up/down needs nothing: the shared list input (0x00A50BB0 up, 0x00A50D10 down) wraps over the item count, which the menu build 0x00A55960 hands to the scroll iterator. The movie side is `tools/mp20_third_row.py`: Mark's InitMenu builds `mMenuArray` from the clip names (`UpdateMenuItem` draws `mMenuArray[mai]`), so it gets a clip m2 (a copy of m1 labelled `$UIMenu_Difficulty`, depth 129, one row pitch of 67.6 lower, starting on an empty stopped frame), InitMenu rewritten in place to `[m0, m1, m2]` with two new constants, the lower half moved down, and the marker `DS2Coop:MP20:m2` the plugin looks for. A movie without the marker keeps two rows.

### 4.11 MP maps in single player

The MP map is loaded through the single player route (`runtime_coop_mpmap.inc`, `analysis/DS2_MP_MAPS_IN_SP_2026-10-09.md`): override checkpoint and `GameMainLoop::RequestLevelChange` 0x0040F870, game mode stays 0, no `LaunchMPGame`. Pieces:

- Saves are held while away from the campaign (`ChooseSaveSlot` 0x00B99FE0, `SavePoint::Store` 0x015F0440), `[MpMap] HoldSaves=1`.
- The MP start checkpoint asks for five character streams that exist only in `global_mp_assets`; they are dropped from `Set::SetDesired` 0x00758C80.
- MP-only logic is kept from spawning by GUID groups (`generated_mpmap_block.h`): end of round, end-game camera, round timers, objective markers, team relays, the map's own slashers, MP globals (UI popups, player templates, MPLevelSettings, cameras). Gates: the creator 0x00777730 and the spawn filter `ShouldSpawn` 0x0040DB20. `KeepSlasherRecords` and `AllowMpMusic` punch the two holes survival needs.
- No MP map carries a player: Isaac is spawned from global_assets template `4BC78C36-9D70FEE7-544E4543-53574F4B` at a MultiplayerSpawnPoint after `PlayerWaitMs`, given a plasma cutter and ammo, dressed if bare, moved onto the map if he arrives > 60 m from every spawn point. An arrival curtain hides the void camera.
- Outbreak maps (Concourse, Academy, from `DS2DAT12.DAT`) need three fixes: registering the PCM codec the PC build carries but never registers (`runtime_coop_pcmcodec.inc`), an overflow for the 96 MB sound bank heap (`runtime_coop_bankheap.inc`), and an Xbox strip-format mesh conversion (`OutbreakRender`, after Mark's approach).
- Objective props are tuned for four players: `[MpMap] ObjectiveDamage=1` scales each hit by 4 / real team size (bots only with `ObjectiveTeamCountsBots`).

### 4.12 The clock modes and Versus

**Mercenaries** (mode 1, `[Mercenaries]`, `MercsBegin/MercsTick`): no waves or breaks; the next wave's enemies join the queue whenever it runs dry ("stages"), escalation by stage. A clock (game time) starts at `StartSeconds` 150, max 600. Kills add `KillMs` 1000 (elite 5000); kills within `ComboWindowMs` 3000 chain and every 10th adds `ComboBonusMs` 10000; score = family points × elite × combo multiplier (up to ×5). Time capsules: 2 at once, 8 per run, stasis packs (15 % golden credit chips), placed far from the players (≥ 20 m, reachable, 12 m apart) at recorded spots (`DS2Capsules.txt`) or near open vents; +30 s (golden +60 s); moved after 75 s. All bonuses fade to 20 % over 480 s; a run ends at `MaxRunSeconds` 900. Each human death costs 15 s. `KillTarget` 150 ends the run as cleared with a time bonus; the last 60 s bring `FinalEnemies` (Hunter, 4 Stalkers, a Pregnant) once. Ranks D..SS. The map's objectives are opened like Hold the Zone. No terminal.

**Objective** (mode 2, `[Objective]`, only MP maps): the map's own objective chain against the hordes. `generated_objective_table.h` gives per map the round time (Titan Mines and Escape 240 s, others 180 s, Outbreak maps 120 s), the TimerSet bonuses, `obj_N_new` hashes and win/loss messages, all counted at the dispatch (`runtime_objective_watch.inc`). The clock starts at round time × `StartPercent`; map bonuses count × `BonusPercent`; each step adds `StepBonusSeconds` 30, supplies and one more stage. Win message = cleared; loss message, clock out or team down = failed. After `ResultSeconds` the host restarts the map from its checkpoint (`DS2Coop_RestartCheckpoint` → `GameMainLoop::CheckpointRestart` 0x0040F840) and the partners follow. Defend phases (Titan bench 1200 HP, Fuel Core fuses 500..600 and detonator 1000): AI enemies never hit props by themselves, so every enemy within `DefendRadius` 4 m of a vulnerable target deals `DefendDamagePerSecond` 12 through `IDamageable::ProcessDamage` 0x00DC5260 (`DS2Coop_DefendTick`). `PropHealthFactor` 3 divides every hit on an objective prop (`DamageableProp::HandleEvents` 0x00ED9750 hook) and `HoldPerPlayerPercent` 50 lengthens hold switches per extra real player (`IUsable::EnterUse` 0x00E994B0 hook); both built 13.10, installed 14.10.. The whole mode is untested.

**Hold the Zone** (mode 3, `[Zone]` in both INIs): one zone at a time at recorded spots only (`DS2Zones.txt`, `map x y z [radius]`, recorded in game with PgUp / deleted with PgDn; 7 to 9 spots per map for all 7 maps exist). A cylinder of 4.5 m radius from 0.5 m below to 1 m above the spot. Standing inside fills it in `CaptureSeconds` 30, +50 % speed per extra body (bots count); 2 enemies inside contest it; empty, it drains at 50 %. A full zone adds 60 s (fading to 20 % over 600 s), supplies and a stage, then moves. Clock 150 s start, 600 max, `MaxRunSeconds` 720. The ring is a circle of resident pickup glows tinted with `VFXHandle::SetTint` 0x0069E080, the locator goes through ObjectiveManager `AddObjective` 0x010C0940 / `RemoveObjective` 0x010A4460. Played on Concourse 12.10 (partial).

**Opening the MP map's objectives** (Zone and Mercenaries, `runtime_coop_zoneobj.inc`, `generated_zone_objectives.h` from `tools/build_zone_objectives.py`, `analysis/HOLD_THE_ZONE_OBJECTIVES_2026-10-12.md`): with `[Zone] OpenObjectives=1`, every machine completes the map's objectives itself on arrival, one step every `OpenObjectivesStepMs` (2500), by sending each objective's completion messages in map order through the dispatch 0x00756C60. That opens every door and vent area. The final objective's start/completion and every round-end message are dropped at the dispatch, so the round never ends. `DS2Coop_ZoneObjectives` reports 0 nothing / 1 opening / 2 opened / 3 not possible; at 2 the director treats every vent as open. Needs `[Gate] Enabled=1` (the opener sends through the gate's dispatch hook).

**Objective steps in co-op** (`runtime_coop_objonce.inc`, `[Mirror] MpObjectivesOnce`): every machine runs the whole map logic. On 13.10 the client's completions never reached the host (each `obj_N_complete` is also heard by a MultiRemoveTarget, so the mirror kept it local; `analysis/MP_OBJECTIVE_SYNC_2026-10-13.md`). Now the five announcements, the plan's step messages, the completions and every message the round clock adds time on run once per round on every machine, from whoever fires first; later copies are swallowed. The host journals each step; a late joiner replays them in plan order. Not on Zone visits (every machine opens the map itself). Built 13.10, untested.

**Versus** (mode 4). Two parts:

- The match (co-op half, `runtime_coop_versus.inc`, `runtime_coop_mpnative.inc`, `runtime_coop_necro.inc`, `[Versus]`, `[Necro]`): humans against human necromorph players on the native MP systems, BlazeHub left null. The roster ('MPNV', 272 bytes: per player machine, team, weapon, suit, XP, name; host merges and splits teams, `[Versus] Humans`, 0 = half) answers the native identity seams with 64-bit refs `0x4453324D00000000 | (machine + 1)`: `IStatsManager::HasStats` 0x0137DAC0 / `GetStat` 0x0137DB00, `DGM::GetPlayerTeamIndex` 0x00A3A270, `GetTeamCount` 0x00A3A2E0, local team at DGM+0xF2. The level manager MP bit (0x2, host 0x4) is set after load (`MpBits`). Native scoring through a ScoreManager with our point table (`[VersusPoints]`, `GetMPTuningValue` 0x0042CAA0 answered), native respawn waves with the host issuing spawn points (`IssueSpawnPoint` 0x004331F0), killer card (`SetKillerInfo` 0x00A68BC0), two rounds with a side swap (`MPEndOfGameSequencer::Activate` 0x00B936C0 hooked; swap after 12 s, results 20 s). Match traffic is 'VRSS' (72 bytes): score_ask/score, round_end, swap, match_over, respawn_ask/respawn_delay/spawn_point; host-only kinds are taken from machine 0 only, each (sender, seq) once. A necro player starts the round as the ghost (type menu, vent pick) without an Isaac (`NecroWithoutIsaac`).
- The AI slashers (director, `[VersusSlashers]`, `[VersusSkins]`): on the host, `Alive` (5; Academy 3) slashers replace the map's own five, each with the next skin from the map's list (shuffled bag, only skins the pool can field), through the map's vents where a vent donor of that skin exists, else direct. First spawns 10 s into the round, replacements 25..40 s after a death (as the maps do). Health 80 (super 200). Pressure: a fast objective (within 45 s) or a lead of 4 kills raises it one level (max 3, decays after 60 s); each level makes 34 % of new slashers super and keeps one more alive. A slasher that targets a necro player gets its template switched to the human team (`RetargetFromNecros`). `DS2Coop_VersusRound` ends them all between rounds. No catch-up, no shop.

Versus was played online 13.10 on Titan Mines (two rounds with a swap); several fixes from that day are installed since 14.10. but not played yet (FEATURES "Versus").

**Campaign arenas** (`runtime_coop_arena.inc`, `generated_arenas.h` from recordings): nine of them (`arena_ch01` .. `arena_ch11`), loaded at their checkpoint with saves held, rooms kept loaded and visible, exit doors locked (`StandardDoor::SetIsLocked` 0x00DA1740, rechecked every second). The director uses `DS2Coop_ArenaInside/ArenaSpots/ArenaPoints` and the arena vent rows; with `[Arena] Modes=1` an arena plays survival, mercenaries or hold the zone and co-op joiners are launched with the host. Per arena file: `exclude=`, `maxalive=`, `heights=`, `spawns=`, door marks. Held doors are checked against the nav graph (`UnblockHeldDoors`) and necro-only Havok blockers near usable doors lose their NPC bit (`OpenNpcClips`).

Scene kinds (17.10.): `runtime_coop_scenekind.inc` classifies the local Isaac every frame from PlayerMasterSM `[P+0x24C]` state (+0xC), the PlayerLowerBodySM child's state name, `[P+0x240]`/`[P+0x238]` sequence flags, the AlertVFXComponent mash count (+0x18), behaviour flags `[P+0x298]` (bits 14, 45), the stationary marker `[P+0x1764]`, player flags (29, 32, 38, 39) and the viewport's top camera vtable. Order: QTE, stationary, cinematic/short by camera when the master state is 1, playable when a sequence runs under master state 0 (or strait jacket, dementia nav help), else none. VISI version 5 carries it; `scenekind::PartnerKind`, `HoldsNecros`, `Watchable` and `LocalHidesPartners` drive the necro scene hold, the watch and the cutscene module's hiding. Full table: analysis/SCENE_KIND_2026-10-17.md.

The partner in cutscenes (19.10., `runtime_coop_scenepartner.inc`, `[ScenePartner]`, off by default):
- **Scene and mode.** At a new sequence `[P+0x238]` the scene stem comes from the scene table: the sequence GUID at +0x24 in `coop_scenes::kSequences`, the anim name cut at `_player`. `PuppetScenes`/`HiddenScenes`/`DefaultMode` decide the mode.
- **When.** Puppets stand while the scene kind is cinematic and a partner is connected. They are released 600 ms after that ends.
- **A puppet** is a partner's Isaac ghost (`GhostForMachine`) with the same ChrInfo `[+0x188]` as the local Isaac. `Animated::IsValidStateId` 0x0074D500 must accept the clip id, sdbm of the name.
  - Start:
    1. Save `GetCurrentState` 0x0074D250.
    2. `Sentient::SetLocalAnimControlAndCacheNetRPCs` 0x00EA51E0 (true).
    3. Set the view `[+0x194]` byte +0xAE (bone mods) to 0.
    4. Collision mode 0 on the movement component (getter 0x00EB8D40, `SetCollisionMode` 0x00F712A0; old mode at +0x224).
    5. Entity::SetMatrix (vtable +0x2C) with Isaac's matrix (+0x90 rows) moved to the side and back.
    6. Player::SetAnimation (vtable +0x150: id, blend, flag800, noMerge), then SetAnimSpeed 1.
  - While it stands:
    - The hook on `Player::EntityGhostTick` 0x0086B940 skips its ghost tick, so the network history neither moves nor aims it.
    - It is placed again for the first 2 s of the scene.
    - The clip restarts with a hard cut when `IsAnimDone` 0x0074D420.
  - Release: bone mods and collision back, then the anim control switch (false) replays the caches. When both caches (+0x2D4, +0x2F4) and +0x292 are empty, the saved state plays. Then the ghost tick runs again.
- **The cutscene module** keeps a puppet's body and helmet shown (`Hidden::bodyShown`, balanced against the hide counter +0x16D), keeps its gun hidden, and hides it again when the puppet ends.
- **Never a sequence on a ghost**: `Player::EndScriptedSequence` 0x00892690 reads the local player table with the ghost's index.

Animation banks (19.10., `tools\ds2_bnk.py`):
- **Header.** +0 body end, +4 version 9, +8 0x805E58C4 (every player bank), +0xC the character hash (sdbm("player") = 0x383225A1), +0x20 the largest node count of one action, +0x24/+0x28 actions, +0x2C/+0x30 owners, +0x34/+0x38 transitions. The relocation list follows the body, descending, with a terminating zero.
- **Owner.** A marker 0x0C owner holds frames, extra count, rig mask and layout. Its +0x30 must be 0 in the file. The game stores an owner's size as the next table entry minus this one (the last up to the first transition block or the relocation list) and never reads it back.
- **Layout 0x80000004** is seven channels per bone (quaternion log x/y/z, zero, translation x/y/z), then the extra channels. A camera clip's extra channel is the FOV in degrees. Each channel is one record: u16 count<<4|family.
- **The camera.** A cutscene camera is a BoneBoundCamera on Isaac's bone 0x39C5E697 m_camera (bone 4). `UpdateAnimControlCameraFOV` 0x00F6BB70 turns the FOV channel into radians.
- **Codecs.** DecompressFrame 0x013C1430 takes codecs 0, 1, 3 and 4 for any character; bit 31 means no scale.
- **Into the game** (`tools\build_anim_banks.py`, `build_variants.py`): our banks are BNK resources in the pool stream.
  - SHDR: "RDHS", memory policy 0x3D074740, u16 alignment 16, u16 1, type 0x30B00ADF, then the GUID (id, id, id, check) and the size.
  - The streamer creates and loads them like retail. `CCTDataManager::LoadResource` 0x007C6F00 needs bank +4 = 9, relocates, registers, and runs `HookUpAllCharacters` 0x007C67F0, which attaches the bank to every ChrInfo whose +8 is the bank's +0xC.
  - An RCB that loads later picks up the registered banks itself (`HookUpAllBanks` 0x007C6970), so load order does not matter.
  - A second resource with the same GUID is never loaded: the first one wins. So an edited scene bank takes a GUID of its own and priority 1 at its edited action's root parameter block (`AddStateRef` 0x013B91D0 / `_ResolveStateOverride` 0x013B8FE0: higher priority wins, equal goes to the bank attached last).
  - Never more than two banks with one state name per character: a third copy loops the state list, and `RemoveStateRef` 0x013B8E50 then never finishes.
- **Synced partner clips.** The scene table gives the running part's Isaac clip. `coop_partner_<that clip>`, when `IsValidStateId` knows it on Isaac, plays on each puppet with a hard start when the part begins. The puppet stands on Isaac's matrix as it was over the part's first 300 ms (after the scene's teleport) and is not restarted.

Birth suits (17.10.):
- coop_uniform::ArmBirth/DisarmBirth wrap Finalize in our RespawnPlayer and in the bot's EngineSteps.
- OnChange swaps the GUID when `_ReturnAddress() == 0x008A8670` (Player::Init's dress after Restore at 0x008A864A). It writes the GUID to Player+0x5A0 and calls the original with that field.
- coop_uniform::Preload wraps PlayerUniformManager::PreloadUniformStreams 0x00ED5E90 (thiscall(guids, count), ret 8; FindUniform 0x00EA0B90, LoadUniformStream 0x00EC6310, flag [mgr+0x28]|=2, then 0x00EC61C0).
- Caveat: ChangeToUniform's first activation of a non-resident suit unloads every uniform with 0 activations.

Teleports and lights (17.10.):
- `runtime_coop_teleport.inc` hooks Entity::SetTeleporting 0x007D0A80 (12 bytes, plain `ret`; it writes the frame number to entity+0x178). The callers are SimpleTeleport::Teleport 0x010B82B0, ScriptedSequenceSM::TeleportTargetToDest 0x00EB87F0 and NicoleBossSM::TeleportNicole 0x010B8900.
- For the local Isaac, a move of 10 m or more within 500 ms (read at Player+0xC0) raises a serial, and the destination goes in VISI v6. The necro's WatchTeleports recentres the ghost camera on it.
- `runtime_coop_lights.inc` hooks AnimateLight::HandleEvents 0x00BADC50 (vtable 0x01B413A4).
  - Message slots in the attributes: on +0x19C.., off +0x1CC.., fade on +0x1FC.., fade off +0x22C.. (4 each, stride 0xC).
  - The light list is at +0x58 (entries at +8, 0x18 each, light at +0x10, count at +0x188). A light's flags are at +0xE8 (in world 0x80), +0xE9 (stream) and +0xEA (script).
  - On/off states travel by GUID in 'LGHT' packets (180 bytes, 8 entries). The receiver calls its own HandleEvents with the same message id.

MP screens (17.10., verified live): show = 0x0071C500(tag) (ensure screen, find by tag, virtual +0x4C), reached from 0x0108F550/70 and from 0x01085C90/B0 (identical copies, wrongly taken for hides until 17.10.); hide = 0x00A38A40(tag) cdecl -> [0x02042830]+0x74 -> 0x0071A3A0 (find by tag, virtual +0x50), reached from the by-tag wrappers 0x01072DE0 (MP12) / 0x01072DF0 (MP13). Screen objects in the MP UI block [0x02089C40]: MP12 +0x9FF0, MP13 +0xA328; open = state (+4) != -1 and movie (+0x2C) != 0.

Life state machine (17.10., evening): `LifeObserve()` derives the state from slot 0, `ghostProxy`/`LocalGhost()`, `necroEntity` (+`NecroDead`), `watch` and the direct spawn; `LifeTick()` (Poll, invasion story) logs every change through `gate::Event`, checks the expectations set by `LifeExpect()` (Transform, Revert, EscapeGhost, the first sighting of a proxy, OnMasterCheck's forced re-entry) and the invariants with their heals (`EscapeGhost` + `JoinAsNecro`, `DropGhost`, `JoinAsNecro`, `DropWatch`, forgetting a stale necro, `PlaceBody` for a moved direct spawn). All results go out as `test <scenario> <RESULT>: ...` lines, which `tools/coop_test_report.py` aggregates per session.

Direct spawn (17.10., evening): `DirectSpawnTick` (Poll, invasion story only) edge-detects `[Necro] DirectSpawnKey` while the local role is 2 with a live ghost proxy. `DirectSpotCheck`: `PaceLeft`, `SceneHold`, a cast 20 m down from the proxy (+0x90 matrix row 3) for the floor, `EntityStreamShown(floor) != 0`, a 1.8 m cast up and four 0.5 m side casts at 1 m, the nearest human between Min and Max distance, and `EyeReaches` (cast from 0.6 m in front of each human's eye at +1.6 m to the spot at +0.4/+1.1/+1.7) false for all. Then `EscapeGhost(matrix)` (rows right/up/forward/position, facing the nearest human) and `Transform()` every 300 ms for up to 4 s with `type` set to `DirectSpawnType()` (respawn manager +0x30 if spawnable). `LocalRole()` answers 2 while it runs, so the role packet and the checkpoint deferral do not see the moment as a human. Finale: `FinaleArena()` = `mirror::finaleActive` or a status-3, flag-0x100 stream whose name contains "dementia" (StreamManager handle table +0x5C/+0x60), checked once a second; it lifts `SceneHold`, the scene `signalForce` and `AskWatch`.

Teleport follow (17.10., evening): `WatchTeleports` (from `WatchHumanRooms`) compares each human's position with the last sample; more than 25 m within 1.5 s sets `recentreWanted` with `lastRecentre = 0`, so the next proxy SM message 6 drops the selection and the engine re-picks through `OnAutoSelectVent`. A pass-3 pick beyond `VentHumanRange` sets `viewAtHuman`, and `OnMoveProxyToRoomView` then places the proxy over that human (identity rotation, `kRoomViewUp`) instead of the vent's room view. HUD: `hudSweeps` (2 passes, 3 s apart, once the level is settled with no ghost) re-run `HideNecroHud` after a restart or level change. `WatchCuesHeld()` includes Watching so cues wait for `WatchReady()`.

Watch suit (17.10., evening): `WatchDressTick` (Watching state) dresses the watch's Isaac in `peerRuntime[watchAuthor].suit` with `coop_uniform::BotDress` once `BotBody` reports the body whole; `WatchReady()` (which releases the queued scene cues in the mirror) waits for `watchDress == Done`. Worn suit read at Player+0x1688 (uniform vtable 0x0174FCB0, GUID +0x24).

Ghost speed (17.10., evening): `TuneGhost` multiplies the RespawnProxy template's walk/run (+0x60/+0x64, retail 4/7 m/s) by `GhostSpeed` (default 6, 1..10) once per template; the first speeds seen are the base, values above 12/15 are taken as tuned already.

Ghost camera to the human (17.10., late): `OnAutoSelectVent` reads the followed human's room as the stream handle (+0x38) of the floor under him (`FloorStream`, a 6 m `bot_sense::CastRaw` down that excludes his ghost body) and each candidate's as the floor 1.2 m in front of its opening (`VentInRoom`, cached 1.5 s). In the first two passes a vent in his room within `GhostHumanViewRange` wins; otherwise the first valid vent (the old pick) is kept as the fallback. `TakeAutoVent` does the bookkeeping and sets `viewAtHuman` when the vent is in another room or beyond the range; `OnMoveProxyToRoomView` honours the flag for 3 s only.

Severed twitchers (17.10., late): a retargeted variant. `tools/build_variants.py` takes `"retarget": {"package": ...}` (with `contexts`, `require_fields`, `exclude_sources`): `retarget_targets` reads the package's MetaModel and rig from its template record, its model stream from the root SimGroup name (`root_guid`) and its DismemberableProp, maps the source's four references onto them, drops the base package's records from the closure, and forces the package resident in the pool TOC (`variants.forced_packages` in the manifest, removed again on the next restore). `VariantDonorRow` gained `package` and `model`; the rig is the package DismemberableProp's (the plain twitcher template names the slasher rig), `retarget.rcb` overrides it. Three packs (`Twitcher`, `TwitcherClassic`, `TwitcherOracle`) share the runtime through `EnemyRecord::retargeted`. Runtime: `appendVariantDonors(pool, statVariants, retargets)` gives retargeted rows `row.model` as the model key and skips a misfit row instead of failing the stage; `runtime_selection.inc` gates them with `[DS2Randomizer] Twitchers` and `StreamForced(row.package)`. `buildPlan` has `PoolSettings::twitcherChance`: a Zombie source that misses `Chance` may still roll a twitcher plan (`plan.twitcherOnly`, only retargeted candidates). Survival: `twitcherDirect` and `VentSpot::twitchers` (keys from `build_survival_vents.py`, which now also reads retargeted rows), `MaybeTwitcher` with `[Survival] TwitcherPercent`, never for super items.

Parameter image ids (18.10.): the loose ParamImageTOC mount (0x0085D020) registers images globally by id and skips ids AttributePacket::Find (0x007CEB30) already knows, so an edited copy must give its changed images new ids (`renumber_changed_images` in build_variants.py: CRC32 of type and fields with resolved GUIDs; refs and image tables rewritten together).

Overlay back buffer (17.10., late): (disabled 18.10.: every frame ends on an offscreen target the game copies after EndScene; `OverlayBackBuffer` defaults to 0) `DrawFrame` compares `GetRenderTarget(0)` with `GetBackBuffer(0, 0, MONO)`; when they differ it binds the back buffer (`BindBackBuffer`, depth surface null) and `UnbindBackBuffer` restores target and depth before the state block applies (SetRenderTarget resets the viewport, the block restores it). `RestoreAfterFault` unbinds too. `panelFrames` counts frames that drew the panel; the shop and debug menu draw checks use it.

Necro vent filter (17.10., evening): `OnCanSpawnAtVent` refuses vents whose room is not shown here, but the vent the auto pick last handed out (`keptVent`) always passes and the game's own pick (`AutoSelectRespawnVent` fallback) runs with the filter off. With every vent refused, the selected vent failed every frame and the game re-selected without end (the necro could barely move or spawn). The auto pick has a third pass that ignores `VentHumanRange` but still needs the vent's room shown (`VentRoomHere(vent, now, anyRange)`; the probe cache keeps the distance and the room result apart). The MP12 type menu and MP13 vent HUD are hidden whenever the ghost goes (`HideNecroHud`: EscapeGhost, restart) and by `HudHeal` when either stays up 1.5 s without a ghost. A restart or level change during a scene watch rejoins a necro by choice. Checkpoints: a necro client defers the host's checkpoint; a restart followed with one waiting sets `reloadAfterApply`, and after the apply as Isaac the client pulls one more local restart so the world is the host's; `necro::Poll`'s join waits on `checkpoint::ReloadWaiting()` (60 s cap).

Corpse limits (17.10., `runtime_coop_lifetime.inc`, `[Lifetime]`): EnemyLifetimeManager ([0x02021350], vtable 0x01B6A840) keeps Bucket[5] at +0x14 (stride 0x18, limit at +0x10; eLifetime 0 CORPSE, 1 BODYPART, 2 TRASH, 3 MULTIPLAYER_ITEM, 4 MULTIPLAYER_JAVELIN). ResetPopLimit 0x01003BF0 (thiscall(bucket, size), ret 8) fades the oldest above the limit and sets it; the game asks 5/10/20, MarkerPatch's DINPUT8.dll detours the entry and swaps in its IncreasedEntityPersistence values. The plugin calls the original past the detour (stub: 8B 44 24 04 83 EC 14, jmp 0x01003BF7) once a second when a limit differs from `Mode*` (MP maps, arenas) or `Story*`.

Since 17.10. a run on an arena owns its enemies: `arenaRunOwnsSpawns` (set in `SetState`, cleared at run end, on a loading screen or another map) makes `OnLookup` empty the packet of every level spawn whose target is a catalog enemy and that is not the director's (`survivalSpawnActive`), so the level's own spawners, more_enemies copies and extra spawns create nothing. Direct engines whose vent contract refuses a donor (bind result 201..207) are retired for the run, donor failures count per run, an item without a direct donor or engine goes to a vent at once, and the arena vent cap (20 m) holds only for an item's first 4 s.

### 4.13 The game's difficulty (`[Difficulty]`, 20.10., built, untested)

Source: `src/runtime_coop_difficulty.inc` (co-op half), `src/coop_difficulty.h` (pure rules, `tests/coop_difficulty_tests.cpp`). Evidence and every address: `analysis/DIFFICULTY_NATIVE_2026-10-20.md`.

- Storage: the current difficulty is one dword, `Options.difficulty` 0x01F6031C (0 casual, 1 normal, 2 survivalist, 3 zealot, 4 hard core); 0x01F60320 is the lowest of the run. We write both as `LaunchNewGame` does, on the game thread. `Game.PDiff` in settings.txt is only the best finished campaign. The spawner rules read the live dword now.
- Picker (`Pick`, 0 off, 1 the SP02 screen, 2 the difficulty row in MP20, section 4.10): the MP20 select pushes SP02 (hash 0xE037EC08). Accept writes the globals and runs the rest of the MP20 select (the lobby screen or the next tick's launch); back pops to MP20.
  - The SP02 vtable 0x01B38250 is shared with the in-game instance (UIComponentManager+0x9C4). Select 0x01B382D0, Cancel 0x01B382D4, Show 0x01B3829C and InitMenuParams 0x01B382CC are swapped and branch on the front end test (`uifm+0x38` bit 2 and `[0x02089C38]`).
  - A pick we own never reaches the native Select 0x00AAC060: it acts on the stale `LoadFlowState` reason and could load a save.
  - Show forces `+0x228 = 0` (no New Game+ lock) and, with `AllowHardcore`, `+0x234 = 1`; row 4 is drawn again through `UpdateItem` 0x015E9800.
  - Any UI can hold a pick with `difficulty::SetPick(row)`; a held pick skips SP02. `PickName()` gives CASUAL ... HARD CORE for a row label.
- Lock (`LockInMatch`): during survival, the clock modes and Versus the in-game SP02 Select is refused on every machine. A hint card shows and the native Cancel goes back to the options.
- Sync (`Sync`): survnet kind 13, host to all, `{generation, difficulty, locked, answered token and request}`, value word `(generation << 8) | d`. Three copies on a change and one every 2 s.
  - A client takes only a newer generation. While its own request waits, only the host's answer counts, for at most 10 s.
  - A client's pause-menu change (native popup) goes to the host as kind 14 `{token, request, base generation}`. The host applies it (or refuses it in a match) and answers with its next generation.
  - Any other change on a client (a save's restore) is put back to the host's value.
- Before the level: the lobby Launch byte (version 4) and `CampaignPacket.difficulty` (+1) are written right before `RequestLevelChange` in both raw launches, so the level's SpareAmmo is scaled with them on its first tick. A rejoin uses the last kind 13 value.
- A survival visit from the campaign keeps the campaign's value and writes it back on the way back.
- HUD (`Hud`): the director reads the dword itself (`DS2Coop_DifficultyHud` says whether) and ends its line with the name.
- Probe (`Probe=1`): mid hooks 0x0087D0A4/0x0087D0BE log the damage taken ratio per hit, an inline hook on 0x00DB3FC0 the SpareAmmo scale.

### 4.14 Player lifecycle in the match modes (`[Lifecycle]`, 02.11., built, untested)

Source `src/runtime_coop_lifecycle.inc` (included from `runtime_coop_respawn.inc`); design, evidence and every failure line in `analysis/COOP_LIFECYCLE_MODES_2026-11-02.md`.

- **Native placement.** Player::Init (0x008A8270) moves every Player without the MP bit (Player+0x3D8 bit 13) to the level's start checkpoint: 0x008A84FA `mov ecx,[0x0201C0A4]; call 0x00402658` (-> 0x00429B80), `cmp eax,ebx` (ebx = 0), `call 0x00B75740` (movaps copy of checkpoint+0x60), entity SetMatrix +0x2C and CMC::SetMatrix 0x00F72DC0. On an MP map that is the origin. A mid hook at 0x008A8505 replaces eax with a 16-aligned stand-in whose +0x60 holds the spawn matrix when a placement is armed (`mpmap::SpawnPlayer`, `respawn::OnRespawnPlayer`), and with 0 for a later Init on a body we placed. The hook goes in at the first match-mode visit, never in story.
- **Born dressed.** The same spawns arm `coop_uniform::ArmBirth` with the suit rule (`mpmap::RuleSuit`): the Engineering Suit in survival, Objective and Hold the Zone (a respawn too, whatever was bought), the setup's suit (`loadout::SuitRow`) in Mercenaries and Versus. Not in story co-op and never over a pending story join suit.
- **Hook from the start, level-time arms.** The hook goes in at plugin start and is idle unless armed. `ArmLoadPlacement` (load only, checkpoint facing, 180 s) and `lifecycle::birthLoadOnly` serve arms made before a level request: only a body made while the level manager is not RUNNING, or while a checkpoint restart lands (`lifecycle::RestartBodyWindow`: the restart's Isaac is made before the level manager leaves RUNNING), takes them, never a bot.
- **Campaign arenas.** `arena::ArmNativeStart` before `StartCampaignFromMenu` and in `arena::BeforeRestart` (from `lifecycle::OnRestartLanded`): a spot of the recording's start set (solo random, co-op by machine number), the kit's suit (`arena::KitSuit`: setup's in Mercenaries, else `[Arena] Suit`), facing the middle of the arena's walk. `NativeArrival` in the first tick the level runs gives the kit and logs `arena spawn: ...`; after a restart it waits (kWait) until the level ran again after the reload; a miss falls back to the old kit change, teleport and curtain.
- **Facing and camera.** Init calls AimingData::UpdateSource (0x00F5A550, thiscall(matrix, flag), ret 8) on [Player+0x6A0]+0x20 and +0xC0 with CMC+0xD0 right after the checkpoint move, so a native placement turns the camera too. A move by hand after Init repeats those two calls (`mpmap::AimFromBody`, `[Lifecycle] AimFromBody`).
- **Restart teardown.** The checkpoint restart's bookkeeping calls `DS2Survival_BeforeTeardown(1)`: every machine of every living director enemy parked on the watchdog's idle table, the enemy deleted (Base::Delete 0x007568B0) while its tables exist; enemy machines older than that mark that still tick are parked unread. A host restart heard on another map is not followed (`checkpoint::restartWantedMap`).
- **MP map arrivals.** `mpmap::EarlySpawn` spawns the Isaac while the level manager waits at WAIT_FOR_COMMAND (15) under the loading screen (`[Lifecycle] EarlySpawn`); without that moment the spawn comes in the arrival tick. The arrival curtain is raised only when Init did not place him (`spawn summary: ...`).
- **Phases.** off, loading, spawning, syncing, alive, dying, down, respawning, game_over, restarting, leaving; logged per change, sent as `LFCY` (18 bytes, the LIFE size: magic, version, machine, phase, kind, token, seq, generation, enemies). Kinds 0 status, 1 prepare, 2 ack, 3 go, 4 resync ask, 5 cancel.
- **Restart together.** `gamemode::PollRestart` -> `lifecycle::BeginRestart`: prepare every 150 ms, partners hold their downed players (`HoldReturns`) and ack, `CheckpointRestart` when all acked or after `RestartAckMs`; refusal cancels; `RestartTimeoutMs` releases holds.
- **Join timeline.** Per arrival: body, session, link, ghosting, first ghost, partner Isaacs, first enemy, enemies in (80 % of the host's LFCY count), wave state, from TNL's GhostConnection (+0x1FC pending, +0x200 in use, +0x204 ghosting, +0x20C 1024 received ghosts classified by NetProxy class name). Host: one line per partner link until its first ghosts are out. A joiner without ghosts asks for `reghost::ForceReset`.

## 5. Engine addresses

Retail PC addresses (image base 0x00400000). H = inline or mid hook, S = vtable slot swap, C = called, P = bytes patched, R = read/verified only. Names are the ones the source uses (most from the Xbox prototype PDB, verified on PC by signature).

### 5.1 Plumbing, session, transport

| Function / data | Address | Use | What for | Source |
|---|---|---|---|---|
| unpacked-image check | 0x00ECB2B0 | R | wait until the packer is done | coop_plugin.cpp |
| per-frame site | 0x007701C0 | H | co-op `Poll`, director frame tick (shared, chained) | coop_plugin.cpp, runtime_frame_cap.inc |
| `FindWindowW` import slot | 0x02158F34 | P | several game copies on one PC | runtime_coop.inc |
| OnlineServices | `[0x020213DC]` | R | allocator +0x8C, BlazeHub +0x94 (null) | runtime_coop.inc |
| NetSession singleton | `[0x020216DC]` | R | 0 in SP | runtime_coop.inc |
| GameNew | 0x00403CB0 | C | session memory | runtime_coop.inc |
| NetSession ctor | 0x0082A520 | C | build the session | runtime_coop.inc |
| NetSession::Tick | 0x0082B300 | C | tick mode 2 | runtime_coop.inc |
| NetInterface::CreateConnection | 0x008578C0 | C | dial each partner | runtime_coop.inc |
| NetInterface::checkIncomingPackets / processConnections | 0x0141A340 / 0x0141B400 | C | tick mode 1 | runtime_coop.inc |
| TNL::Socket ctor / dtor / sendto / recvfrom | 0x01417EA0 / 0x01417EE0 / 0x01417F00 / 0x01417F50 | H | TNL on our UDP socket | runtime_coop.inc |
| online manager local-game-player getter | 0x013778E0 | H | fake answer for NetObjSync only; self-drive fallback | runtime_coop.inc |
| NetObjSync (getter call returns to 0x00829984) | 0x00829940 | R | | runtime_coop.inc |
| GhostConnection::objectInScope | 0x014160A0 | H | scope filter | runtime_coop.inc |
| GhostConnection::resetGhosting / activateGhosting | 0x01416850 / 0x01416A20 | C | re-ghosting | runtime_coop_reghost.inc |
| NetConnection::setFixedRateParameters | 0x01418350 | C | 32 ms / 12000 B/s | runtime_coop_netrate.inc |
| NetConnection::checkTimeout / clearPingTimer | 0x01419F50 / 0x01418280 | R / C | partner timeout, stall forgiveness | runtime_coop_disconnect.inc |
| NetReflectable SetSubKey / netInit / AddToSession | 0x007D1D10 / 0x007D1DC0 / 0x007E10E0 | C | register players, NPCs, bots, necros | runtime_coop.inc, runtime_coop_npcs.inc |
| GetNetObj / proxy ghost test (damage gates) | 0x007E1120 / 0x007E1150 | R | | runtime_coop_hits.inc |
| RPC_AuthorityTransfer* | 0x007E19D0 … 0x007E2280 | H | observe only | runtime_coop_authority.inc |
| PlayerManager | `[0x0201C0D4]` | R | 8 slots | runtime_coop_state.inc |
| PlayerManager Register / Unregister | 0x0077BB60 / 0x0077BC00 | C | bots, necro transform | runtime_coop_bot.inc, runtime_coop_necro.inc |
| ghost anim update slot 15 / 14 | 0x01091790 / 0x01091A00 | H | skip ghosts without a bone table | runtime_coop.inc |
| FindByGuid epilogues | 0x00776D20 / 0x00776D4E / 0x00776D91 (fn 0x00776CA0) | H | ghost GUID redirect | runtime_coop_guidfix.inc |
| NetReflectable list | `[0x0205B744]` | R | NPC scan, sub-key lookup | runtime_coop_npcs.inc, runtime_coop_hits.inc |
| imatrix44_t::AuthoritativeUpdate | 0x007D0530 | H | ghost timeline | runtime_coop_ghostclock.inc |

### 5.2 Replication

| Function | Address | Use | What for | Source |
|---|---|---|---|---|
| message dispatch `bool(msg*, flags)` | 0x00756C60 | H | gate, mirror, vent windows, objective watch, zone opener, music (shared) | runtime_coop_gate.inc, runtime_more_enemies.inc |
| message registry / source handle table | `[0x0204CD8C]` / `[0x0201C0F4]` | R | | runtime_coop_gate.inc |
| SendMessageTo | 0x00756A00 | H | mirror direct sends | runtime_coop_mirror.inc |
| Door::TransitionToState | 0x00DD2950 | H | door waits and votes | runtime_coop_worldcalls.inc |
| VentMarker::SetCoverState | 0x012F5AB0 | H / C | vent covers | runtime_coop_worldcalls.inc |
| MultiRemoveTarget::HandleEvents | 0x007E5E50 | H | shared removals | runtime_coop_removals.inc |
| MultiManager::SendMessages | 0x00801A50 | H | relay mirroring (`MultiManagers=1`) | runtime_coop_mirror.inc |
| TriggerPhysical touch / CheckEntityPassesFilter | 0x015F15E0 / 0x00EA76F0 | H | ghost and bot trigger filters | runtime_coop_ghosttrigger.inc |
| DirectSendDamageEvent | 0x010BD030 | H (pass-through) | notice dropped client hits | runtime_coop_hits.inc |
| HealthComponent / DismemberableHealthComponent ::HandleDamage | 0x00F65B80 / 0x00F73600 | H | measure damage | runtime_coop_hits.inc |
| HealthComponent::SetHealth (vtable +0x20) | 0x00F84C60 | C | apply client hits, watchdog kills | runtime_coop_hits.inc, runtime_survival.inc |
| IDamageable::ProcessDamage | 0x00DC5260 | H / C | prop hits, defend phases | runtime_coop_hits.inc, runtime_coop_objectives.inc |
| NetDamageData_t ctor / ToDamageData / DamageData ctor | 0x00EAE280 / 0x00ECFF90 / 0x00ECFD30 | C | replay prop hits the engine's way | runtime_coop_hits.inc |
| NicoleBoss::ApplyDamageModifiers | 0x0107D930 | H | per-machine Nicole | runtime_coop_hits.inc |
| DeadSpaceSpawnFilter::ShouldSpawn | 0x0040DB20 | H | client NPC suppression, MP map blocks | runtime_coop_npcs.inc, runtime_coop_mpmap.inc |
| class-dispatch creator | 0x00777730 | H | MP map blocks | runtime_coop_mpmap.inc |
| DeadSpaceNPC::UpdateTarget site | 0x00FFF608 | H | nearest target | runtime_coop_npcs.inc |
| Player::ProcessDeath | 0x00878B40 | H | death sync, MP respawn | runtime_coop.inc |
| Player::RespawnPlayer | 0x00891D70 | H / C | MP-map respawn | runtime_coop_respawn.inc |
| Player::Init checkpoint move (block / hook) | 0x008A84FA / 0x008A8505 | R / H (mid) | native placement of spawned and respawned bodies | runtime_coop_lifecycle.inc |
| AimingData::UpdateSource (Init's calls at 0x008A8852 / 0x008A8884) | 0x00F5A550 | C / R | the camera's aim after a spawn moved by hand | runtime_coop_mpmap.inc |
| Player::SetFlag | 0x0085F7E0 | C | ghost helmet | runtime_coop.inc |
| ChangeToUniform / attach / LoadUniformStream | 0x00EE0610 / 0x00EB4E10 / 0x00EC6310 | H (armed on connect) / C | ghost suits, shop suits | runtime_coop_uniform.inc |
| Quartermaster::RequisitionGun / cancel | 0x00DDD5F0 / 0x00DD6C40 | C | ghost and bot weapons, shop | runtime_coop.inc |
| LevelManager::OkToStart | 0x0042C970 | H | loading barrier | runtime_coop_loadsync.inc |
| CheckpointLoadComplete / LoadFromCheckpoint | 0x0042D5D0 / 0x0042DA00 | R | where the barrier sits; the checkpoint set is loaded before the first ask | runtime_coop_loadsync.inc |
| Player::Init dress (ChangeToUniform call, returns to 0x008A8670) | 0x008A866B | R (via the ChangeToUniform hook) | story joiner born in his suit | runtime_coop_uniform.inc |
| StreamSet::Activate | 0x00759D00 | H | stream hold | runtime_coop_streams.inc |
| StreamSetManager::Set::SetDesired | 0x00758C80 | H | pin partner rooms, drop foreign MP streams | runtime_coop_streams.inc, runtime_coop_mpmap.inc |
| VisibilityManager::Show / Hide | 0x007C1270 / 0x007C12F0 | H / C | room visibility | runtime_coop_streams.inc |
| Checkpoint::SetDeathRestart / Find | 0x00BA1F20 / 0x00B86C30 | H / C | checkpoint follow | runtime_coop_checkpoint.inc |
| checkpoint restart implementation | 0x0040EC10 | H | restart follow | runtime_coop_checkpoint.inc |
| SetOverrideCheckpoint / ClearPersistent / StoreLevel | 0x00B757B0 / 0x007B6950 / 0x00B75880 | C | level follow, menu launches | runtime_coop_level.inc |
| GetDeathCheckpointId / death GUID | 0x00B75810 / `[0x0208FCC4]` | R / P | the host's checkpoint for a client restart | runtime_coop_checkpoint.inc |
| GameMainLoop::RequestLevelChange / CheckpointRestart | 0x0040F870 / 0x0040F840 | C | level follow, MP map visit, Objective restart | runtime_coop_level.inc, runtime_coop_mpmap.inc, runtime_coop_gamemode.inc |
| PlayerFireTKSM Grab / Drop / Launch notify | 0x0088A500 / 0x0088A2B0 / 0x0088A390 | H | kinesis sharing | runtime_coop_tk.inc |
| applyHardKeyFrame | 0x014A27E0 | C | kinesis pose, rest sync | runtime_coop_tk.inc, runtime_coop_restsync.inc |
| ItemDropListComponent::DropItem | 0x010B44E0 | H (observe) | drops | runtime_coop_drops.inc |

### 5.3 Menus, lobby, MP maps

| Function / data | Address | Use | Source |
|---|---|---|---|
| PC01 / FE03 / MP20 / SP01 OnInput_Select | 0x00AA1C30 / 0x00AA9DD0 / 0x00A67B50 / 0x00AABE20 | S (slots 0x01B392B0 / 0x01B39540 / 0x01B3A368 / 0x01B38198) | runtime_coop_menu.inc |
| FE03 HandleInput | 0x00AAF9F0 | S (0x01B394E0) | runtime_coop_menu.inc |
| Apt3dDirector::PushScreen / director | 0x00A879D0 / `[0x0208BE90]` | C / R | runtime_coop_menu.inc |
| DGM map list Add / String::Set; DGM | 0x00A91CB0 / 0x0073AAE0; `[0x020213FC]` | C | runtime_coop_menu.inc |
| MP20 HandleInput / SetInfo | 0x00A7E7D0 (slot 0x01B3A308) / 0x00A4BA90 | S / H | runtime_coop_gamemode.inc |
| file open (frontend override) | 0x0056F3C0 | H | runtime_coop_gamemode.inc |
| MP20 ctor item counts / SetInfo tail / HandleInput right, left | 0x00AAB7B5, 0x00AAB830, 0x00AAB836, 0x00AAB84A (1 -> 2, or 3 with `[Difficulty] Pick=2`) / 0x00A4BB50 / 0x00A7E83D, 0x00A7E8B6 | P / H (mid) / H (mid) | runtime_coop_gamemode.inc |
| list up / down, menu build, scroll iterator cursor / UpdateItem | 0x00A50BB0 / 0x00A50D10, 0x00A55960, 0x015E8C50 / 0x015E9800 | R / R / C | runtime_coop_gamemode.inc |
| MP00 OnShow branch / MP00 Tick game test | 0x00A6559C / 0x00A49B12 | H (mid) | runtime_coop_nativelobby.inc |
| MP02 Tick / ConditionalChangeTab | 0x00A658B0 / 0x00A49C30 | H | runtime_coop_nativelobby.inc |
| MPLobbyManager / StopLobby / IGameManager::SetStreamList | `[0x020213E8]` / 0x00A54010 / 0x013780C0 | R,W / C / C | runtime_coop_nativelobby.inc |
| GetPlayerSignalBar | 0x00A53230 | R (thresholds copied) | coop_lobby.h |
| SaveManager::ChooseSaveSlot / SavePoint::Store | 0x00B99FE0 / 0x015F0440 | H | runtime_coop_mpmap.inc |
| PCM codec descriptor / byte swap | 0x01C6D57C / 0x00562CF0 | R / registered | runtime_coop_pcmcodec.inc |
| sound bank heap | `[0x0201C130]` (registered at 0x00411AA0) | H | runtime_coop_bankheap.inc |
| StandardDoor::SetIsLocked | 0x00DA1740 | C | runtime_coop_arena.inc |
| TX_ReplaceTextureContents | 0x013C9030 (cdecl(texture, size, data, flag), from TM_DownloadTextureData 0x013DC06B) | H | runtime_coop_rigcolor.inc |
| RWS::MainLoop::Render::Poll | 0x00770C00 (cdecl(int), plain ret; callers 0x0042F8BB, 0x0042F8ED) | H | runtime_coop_camtools.inc |
| CameraManager / SetRenderCamera | `[0x0202131C]` (blenders +0x10, 0x2A0 each, count +0x164, RenderContext +0x14) / 0x00793470 (thiscall(vp, pos*, rot*, fov, near, far), ret 0x18) | R,W / C | runtime_coop_camtools.inc |
| game speed | float 0x01EC51F0 (read at 0x0076FA14), never below 0.01 | W | runtime_coop_camtools.inc |
| TFXSequencer::PatchInit | 0x006DA2C0 (cdecl(effect buffer), AL; only caller VFXManager::LoadInit 0x006A0B66) | H | runtime_coop_fxcolor.inc |
| SP02 Select / Cancel / Show / InitMenuParams | 0x00AAC060 / 0x00A8B110 / 0x00A98AF0 / 0x00A68FA0 (thiscall; Select `ret 4`, the rest plain `ret`) | S (slots 0x01B382D0 / 0x01B382D4 / 0x01B3829C / 0x01B382CC) | runtime_coop_difficulty.inc |
| Apt3dDirector::PopScreen / current screen | 0x00A87A80 / 0x00A34F00 (thiscall, plain `ret`) | C | runtime_coop_difficulty.inc |
| Options.difficulty / lowestDifficulty | 0x01F6031C / 0x01F60320 (verified through GetDifficulty 0x00A34070, SetLowestDifficulty 0x00A34080, LaunchNewGame's writes 0x00A40D58) | R,W | runtime_coop_difficulty.inc, runtime_survival.inc, runtime_spawner_tools.inc |
| ApplyDamageModifiers SP scale / ScaleAmountForDifficulty | 0x0087D0A4, 0x0087D0BE / 0x00DB3FC0 | H (`[Difficulty] Probe=1` only) | runtime_coop_difficulty.inc |

### 5.4 Survival director and modes

| Function / data | Address | Use | What for | Source |
|---|---|---|---|---|
| ServiceRegistry::Find, registry | 0x00417DB0, `[0x0201C09C]` | C | current map from GameMainLoop | runtime_survival.inc |
| LevelManager (+0x18 state, 16 RUNNING; +0x1C MP bits) | `[0x0201C0A4]` | R / W | arming; Versus MP bits | runtime_survival.inc, runtime_coop_versus.inc |
| SimManager / GetSpawnInfo | `[0x0201C0A0]` / 0x00776440 | C | does a donor resolve | runtime_survival.inc |
| FuncSpawn::HandleEvents (vtable 0x01B59C58) | 0x00ECB1D0 | C | drive a forced spawn | runtime_more_enemies.inc |
| selection `OnLookup` / `OnCreated` | 0x00ECB2B5 / 0x00ECB2D0 | H (mid) | donor swap, vent rebind / actor report, nav repair | runtime_selection.inc, observer.cpp |
| FuncSpawn ctor | 0x00EA5A40 | H (mid) | spawner registry | runtime_more_enemies.inc |
| spawning-component binding owner / constructor | 0x00818F60 / 0x00F64B20 | H (mid) | vent rebinding | runtime_selection.inc |
| placement: movement component / rotate / position | 0x00876E00 / 0x00F63540 / 0x00F638A0 | C | direct-spawn placement | runtime_more_enemies.inc |
| CollisionCamera::CastingTest | 0x00F5B180 | C | sight, headroom, walls | runtime_survival.inc |
| bfx::CreatePolylinePath (+ segment getters 0x0057B9A0..0x0057BAC0, ~RCPtr 0x0057F180) | 0x005801B0 | C | NavPower path checks | runtime_survival.inc |
| NavManager / NavPower system | `[0x020AD0C0]` / `[0x0202F4CC]` | R | nav space, pool headroom | runtime_survival.inc |
| bfx::SystemCreate | 0x005B4620 | H | enlarge the NavPower pool | runtime_navpower.inc |
| MultiplayerSpawnPoint list / count | 0x020218A0 / 0x02021920 | R | spawn seeds | runtime_survival.inc |
| AwarenessComponent forced-aware flag (+0x724), fn | 0x00B73260 | R / W | hunting | runtime_survival.inc |
| charger count / max | `[0x01F612A8]` / `[0x01F612AC]` | W | MaxChargers | runtime_survival.inc |
| SetAnimSpeed | 0x00751F40 | H (mid) | escalation speed | runtime_variants.inc |
| StateMachine::Update | 0x013E2CF0 | H (mid) | fallback director driver | runtime_watchdog.inc |
| GetGlobalPickupItem / Item::Drop (vtable +0x1F0) / Base::Delete | 0x00DE7560 / 0x00DD7660 / 0x007568B0 | C | rewards, capsules, cleanup | runtime_survival.inc |
| Entity::FadeOut; PickupItem register / unregister branches | 0x007D3220; 0x00DB433F / 0x00DB43CD | H (mid) | cleanup | runtime_survival.inc |
| game clock | `[0x0204D3C0]` | R | pause detection | runtime_survival.inc |
| StasisManager::StasisEntityInternal | 0x00BB1B40 | H | boss stasis resistance | runtime_coop_escalation.inc |
| DamageableProp::HandleEvents | 0x00ED9750 | H | objective damage scaling, PropHealthFactor | runtime_coop_objectives.inc |
| IUsable::EnterUse | 0x00E994B0 | H | hold switch length | runtime_coop_objectives.inc |
| VFXHandle::SetTint | 0x0069E080 | C | zone ring | runtime_coop_zone.inc |
| ObjectiveManager / AddObjective / RemoveObjective | `[0x02021430]` / 0x010C0940 / 0x010A4460 | C | zone locator | runtime_coop_zone.inc |
| Item::Pickup / Player::CalcUpgradedValues / Gun::MoveFromInventoryToSafe | 0x00DEF170 / 0x00879B80 / 0x00DE0EB0 | C | shop | runtime_coop_shop.inc |

### 5.5 Versus and necro players

| Function / data | Address | Use | Source |
|---|---|---|---|
| IStatsManager::HasStats / GetStat | 0x0137DAC0 / 0x0137DB00 | H (answer from roster, no hub) | runtime_coop_mpnative.inc, runtime_coop_necro.inc |
| DGM::GetPlayerTeamIndex(int64) / GetTeamCount | 0x00A3A270 / 0x00A3A2E0 | H | runtime_coop_mpnative.inc |
| LM::GetMPTuningValue | 0x0042CAA0 | H (points table) | runtime_coop_versus.inc |
| GetVictimInfo / GetAttackerInfo | 0x01086670 / 0x010865A0 | H | runtime_coop_versus.inc |
| UIKilledByInfo::SetKillerInfo | 0x00A68BC0 | C | runtime_coop_versus.inc |
| MPEndOfGameSequencer::Activate | 0x00B936C0 | H | runtime_coop_versus.inc |
| MPSpawnPointSelector ctor / QueuePlayerForRespawn / IssueSpawnPoint / RequestRespawnPoint | 0x00433630 / 0x00433C60 / 0x004331F0 / 0x00433F20 | C / C / H / H | runtime_coop_versus.inc |
| ScoreListener_Open / ScoreBoardEntryImpl ctor / LogScoringEvent | 0x010BD320 / 0x010868A0 / 0x010D9F00 | C / C / H | runtime_coop_versus.inc |
| RequestNecroPlayer / approved type | 0x0108EAE0 / 0x010C2760 | H | runtime_coop_necro.inc |
| NecroPlayer::RegisterWithManager | 0x0108C310 | C | runtime_coop_necro.inc |
| NecroPlayerRespawnManager ctor / singleton | 0x010A7E00 / `[0x02021628]` | C / R | runtime_coop_necro.inc |
| CreateRespawnProxy | 0x010D2740 | C | runtime_coop_necro.inc |
| HandlePostSpawnInit branch | 0x010A57DD | P (`je` → nops) | runtime_coop_necro.inc |
| GetPlayerByRef | 0x005F2CE0 | H | runtime_coop_necro.inc |

The full Versus signature list (about 40 entries: scoreboard UI, round restart, MP pause menu, loot swap) is in `runtime_coop_versus.inc` (`kSigs`), the native lobby list in `runtime_coop_nativelobby.inc` (`kSigs`).

## 6. Known limits and open problems

From `FEATURES.md` ("Known issues and open", state 13.10.2026) plus what the source and the audits say. "Built, not installed" means compiled on 13.10 but not in the build that was played.

### Networking and co-op

| Problem | Detail |
|---|---|
| Internet robustness | The enemy template check on a client has a 2 s window; one lost packet can drop ghosts for about one ping. Tested over Hamachi only. IPv4 only, no NAT traversal, every machine must reach every other (full mesh) |
| Unacknowledged survival traffic | Notices, wave rewards and capsules go out three times without an ack (`survnet`); a lost capsule "gone" leaves a stale capsule on a client. Prop hits (`SendPropHit`) go out once and are never repeated, so one lost packet is a permanent HP difference on that prop |
| Client hits "mostly stale" | 13.10 story run: 1578 received, 1025 stale. Each CHIT report is sent three times and the receiver counts repeats of a seen (token, sequence) as `stale`, so roughly two thirds stale is what the triple send alone produces; whether real hits were lost is not established (my reading of `coop_hit_protocol.h`, not measured) |
| No session-mode variable | `gamemode::mode` is the map picker's pick and is never reset for a campaign session; bots, `GhostToClients`, the necro MP UI set and the necro respawn manager also run in story co-op. A split (`StoryCoop()/SurvivalMode()/VersusMode()`) is proposed in `analysis/COOP_MODE_SPLIT_AUDIT_2026-10-12.md` and not implemented |
| Seed not sent | Schematics, bench and client-local spawns differ when the seeds differ |
| Direct launcher path | `DS2COOP` launchers compare no build; the config check (`[Coop] ConfigCheck`) only warns and cannot see a failed hook beyond survival readiness |
| Ghost trigger hooks | `[Coop] GhostsIgnoreTriggers` and `[Streams] GhostsIgnoreCullTriggers` never worked (they read the message source wrong); partner ghosts fire trigger volumes unless `[Coop] GhostTriggerFilter=1` (experimental, off) |
| Story streams | the partner keep list is full at 46 entries (host pop-in); relayed managers drop cull targets. A client crash (12.10) came from a ghost animating out of a stream block the client had just freed |
| World state | no world state transfer to a client that joins; a client never stores its own restart point; enemy loot exists on the host only in the campaign |
| Lifts | bots count as players in a lift cabin; a bot left outside holds the ride up to 60 s |
| Template check with several clients | per partner since 02.11., but the scope hook still asks for the primary partner until it passes the connection (`TemplateScopeAllowedFor`, a request in analysis/COOP_LIFECYCLE_MODES_2026-11-02.md) |
| No generation field | MIRR, VOTE and CHIT carry no `{level, restart}` generation; a packet from before a restart can act after it |
| Ghosting activation | the plugin never calls `NetSession::AcceptClient` or `EnableGhosting` (listed as gaps in the 16.09 PDB notes); ghosting works per the logs (17.09 onward), but how it gets switched on without them is not documented in the source |
| No session teardown | one role per game start; changing role or recovering from a broken session means restarting the game |
| Necro picks | no packet carries a client necro player's type or vent pick; whether the host learns it is not established |
| Not replicated | severed limbs and kinesis-held objects (no authority transfer); enemy loot in the campaign |
| MP-map respawn | the 13.10 playtest showed new bodies at the map origin, a use-after-free of a vanished body, a client restart restarting the host's run and a stale spawn-window table (`analysis/MP_RESPAWN_STREAM_DESYNC_2026-10-13.md`); fixes were built 13.10, not played |
| Seat arithmetic | the lobby admits 7 minus bots, the bot module seats `4 − humans`; with many humans plus bots the 8 PlayerManager slots can run out, and moving a human ghost out of a bot slot fails when slots 4..7 are full |
| Old notes | some notes describe older states (lobby packet v1 with 32 bytes, 3 joiners, ref base `0x44533200...`, template match 2 s, death sent as blind repeats); the code is current |
| Engine message handle table | 512 slots overflow in long survival runs; the Quartermaster then crashed on a gun stream (dump 7320). Guard and census built 13.10, installed 14.10. |
| 32-bit address space | the game is not large-address-aware (2 GB); host FPS falls over long survival runs |
| Blank menu text | after leaving a map with the pool held, the language pack was purged; fix built 13.10, installed 14.10. (`analysis/TEXT_PACK_HOLD_2026-10-13.md`). It made the menu reuse the held text_assets_global copy, which never sends iMsgStreamLoadComplete, so the level manager hung in LOAD_TEXT_ASSETS (black screen); built 14.10.: after a held pool, the level manager's handler 0x0042DEC0 gets that message once the queues are idle (`text_packs::NudgeTextState`). That nudge sat on the stream update, which does not run with idle queues, and failed in game on 15.10. Replaced by `exit_guard` (runtime_exit_guard.inc, `[ExitGuard]`): the frame tick sends iMsgStreamLoadComplete in every state that waits for it (1, 2, 5, 7, 8) after 1.5 s of idle queues; a load state stuck for 60 s or a main loop stopped for 30 s restarts the game after a small dump |

### Survival and modes

| Problem | Detail |
|---|---|
| Versus | cross-machine necro grabs never observed (coordinator ghost updates 0); Ubermorph crashes as a Versus necro; the client round clock ran out 390 s early (fix 13.10, needs a round) |
| Arenas | the arena wave 2 stall fix is not confirmed |
| Silent pool enemies | cause open |
| Fuel Core with bots | rooms behind open doors stayed black; cause open, logged now |
| Crash at level exit on mp_solar | dump 29868, 0x00790624, not followed up |
| Untested 13.10 work | catch-up, VentMustReach, the new alive cap, StalkerNeedsCover, Mercenaries capsule spread and bonus fade, Objective prop health and hold switch scaling, bot kit progression, wave music sources, rejoin, host-again |
| Boss donors | Divider and Ubermorph as campaign donors: softlock or unkillable cases; Stalker and Brute never work as replacements |
| Variants | Ghost and Gold Leaper crash in ItemDropListComponent; the build ships none |

## 7. Where to look in the source

All paths relative to `DeadSpace2Randomizer\`. `.h` files with a `coop_` prefix are pure logic (no game memory, most have tests under `tests\`); the `.inc` files touch the game.

### Entry and plumbing

| File | What |
|---|---|
| `src/ds2coop_main.cpp` | DllMain of DS2United.asi, calls both halves |
| `src/ds2coop_config.h` | INI lookup (merged DS2United.ini, HaferSprawl.ini from before the rename, or the legacy file), `PartnerModule` for the cross-half exports |
| `src/observer.cpp` | randomizer half: worker thread, includes every `runtime_*.inc` of the randomizer, `DS2Survival_*` exports |
| `src/coop_plugin.cpp` | co-op half: guarded memory reads/writes, frame tick hook, start-up, every `DS2Coop_*` export |
| `src/hook_site.h` | shared hook installation under a lock, chaining behind an existing hook (both halves want 0x007701C0 and the message dispatch 0x00756C60) |

### Networking core

| File | What |
|---|---|
| `src/runtime_coop.inc` | the TNL session (construction, tick, dial list, fake OnlinePlayer), roles (`ConfigureSolo/AsHost/AsClient`), the side channel socket and receive loop, PlayerState, readiness, `survnet`, bot ghosts (`botnet`), player registration |
| `src/coop_peers.h` | the peer table (8 slots, tokens, relayed peers) |
| `src/coop_peer_readiness.h`, `coop_ping_protocol.h`, `coop_death_protocol.h`, `coop_hit_protocol.h`, `coop_suit_protocol.h`, `coop_loadout_sync.h`, `coop_template_protocol.h`, `coop_gate_protocol.h` | side channel packet formats and their sender/receiver state machines |
| `src/runtime_coop_reghost.inc` | re-ghosting after level changes |
| `src/runtime_coop_guidfix.inc` | two Isaacs, one GUID (ghost GUID redirect) |
| `src/runtime_coop_ghostclock.inc`, `runtime_coop_netrate.inc` | ghost timeline, TNL send period and bandwidth |
| `src/runtime_coop_disconnect.inc` | partner timeout, seat and vote release |
| `src/runtime_coop_authority.inc`, `runtime_coop_ownership.inc` | kinesis authority transfer (observation only), ownership snapshots for the log |
| `src/runtime_coop_hits.inc` | client hits, world hits, prop hits, Tormentor arm |
| `src/runtime_coop_mirror.inc`, `runtime_coop_worldcalls.inc`, `runtime_coop_removals.inc`, `runtime_coop_objonce.inc` | the world mirror, door votes, removals, MP objective steps once per round |
| `src/runtime_coop_gate.inc`, `coop_gate.h` | the progression gate |
| `src/runtime_coop_loadsync.inc` | loading together |
| `src/runtime_coop_streams.inc` | stream pinning, visibility union (STRM, VISI) |
| `src/runtime_coop_level.inc`, `runtime_coop_checkpoint.inc`, `runtime_coop_respawn.inc` | level follow, checkpoint follow, MP-map respawn |
| `src/runtime_coop_cutscene.inc`, `runtime_coop_npcs.inc`, `runtime_coop_tk.inc`, `runtime_coop_restsync.inc`, `runtime_coop_regen.inc`, `runtime_coop_uniform.inc`, `runtime_coop_loadout.inc` | scenes, NPC policy, kinesis, physics rest, Hunter regrowth, suits, weapons |
| `src/runtime_coop_configcheck.inc`, `coop_configcheck.h` | INI fingerprint exchange |

### Lobby, menus, MP maps

| File | What |
|---|---|
| `src/coop_lobby.h`, `src/runtime_coop_lobby.inc` | lobby protocol and its socket |
| `src/runtime_coop_menu.inc` | PC01/FE03/MP20/SP01 slot swaps, extra maps, records/how-to/about pages |
| `src/runtime_coop_nativelobby.inc` | MP00/MP02 pre-game lobby |
| `src/runtime_coop_gamemode.inc` | mode row in MP20, frontend override file, checkpoint restart for Objective |
| `src/runtime_coop_difficulty.inc`, `src/coop_difficulty.h` | the difficulty: SP02 picker after MP20, lock in matches, host to client sync (survnet 13/14), probes (`[Difficulty]`, 4.13) |
| `src/coop_mpmap.h`, `src/runtime_coop_mpmap.inc`, `generated_mpmap_block.h`, `generated_mp_autospawn_block.h` | MP map visit in single player, block groups |
| `src/runtime_coop_spawnwin.inc`, `generated_mp_spawn_windows.h` | MP spawn point activation windows |
| `src/runtime_coop_pcmcodec.inc`, `runtime_coop_bankheap.inc` | Outbreak map audio |
| `src/runtime_coop_arena.inc`, `runtime_coop_arenarec.inc`, `coop_arena.h`, `generated_arenas.h`, `generated_arena_points.h` | campaign arenas (tools/build_arenas.py: arena file options `streaming=sets`, `block=<GUID>`, `doors=open`, `exclude=<Family>`, `maxalive=<n>`: the one-player cap there instead of `[Survival] MaxAlive`); their mode points and door marks from tools/build_arena_points.py |
| `src/runtime_coop_rigcolor.inc` | RIG, visor and blood texture colours (`[Visuals]`), recoloured at the texture upload |
| `src/runtime_coop_camtools.inc`, `coop_camtools.h` | trailer camera: free camera on the pad, slow motion, camera path recorder (`[CameraTools]`) |
| `src/runtime_coop_fxcolor.inc`, `generated_fx_colors.h` | stasis, kinesis, laser, weapon and blood effect colours (`[Visuals]`), recoloured when an effect loads |

### Versus and necro players

| File | What |
|---|---|
| `src/runtime_coop_necro.inc` | necro players (type menu, vent pick, respawn manager, pool, grabs) |
| `src/runtime_coop_mpnative.inc`, `coop_mpnative.h` | roster, 64-bit refs, team split, Blaze seams |
| `src/runtime_coop_versus.inc`, `coop_versus.h` | match flow, scoring, rounds, respawn points |
| `src/runtime_coop_mpquiet.inc` | our item calls without the MP bits |

### Survival director (randomizer half)

| File | What |
|---|---|
| `src/runtime_survival.inc` | the director |
| `src/survival.h` | its pure logic: config, wave plans, families and substitution, queue, spawn rules, stuck verdict, rewards, records |
| `src/generated_survival_vents.h`, `generated_vent_donors.h` | vent markers per map, vent donors |
| `src/runtime_navpower.inc` | NavPower pool size |
| `src/runtime_vent_gate.inc`, `runtime_objective_watch.inc` | which vents a map has opened, objective count |
| `src/runtime_watchdog.inc`, `spawn_watchdog.h` | campaign watchdog (survival has its own stuck logic) |
| `src/global_pool.h`, `runtime_stream_probe.inc`, `runtime_pool_ownership.inc` | the enemy pool stream |
| `src/generated_zone_objectives.h`, `generated_objective_table.h` | MP map objective plans and clock tables |

### Survival on the co-op side

| File | What |
|---|---|
| `src/runtime_coop_shop.inc`, `coop_shop.h`, `runtime_coop_overlay.inc` | supply terminal and the D3D9 overlay |
| `src/runtime_coop_hud.inc`, `runtime_coop_notice.inc`, `runtime_coop_marks.inc`, `coop_marks.h` | status line, RIG notices, waypoints |
| `src/runtime_coop_zone.inc`, `coop_zone.h`, `runtime_coop_zoneobj.inc`, `coop_zoneobj.h` | Hold the Zone display, objective opener |
| `src/runtime_coop_objectives.inc` | Objective mode: defend phases, prop damage scaling |
| `src/runtime_coop_survmusic.inc`, `coop_survmusic.h` | wave music |
| `src/runtime_coop_escalation.inc` | escalation on the co-op side |
| `src/runtime_coop_drops.inc` | loot drops |
| `src/runtime_coop_bot*.inc`, `coop_bot_*.h` | companion bots |

### Notes worth reading

`analysis/DS2_MP_INFRASTRUCTURE_2026-10-11.md` (how native MP is layered and where Blaze sits), `analysis/DS2_NATIVE_LOBBY_2026-10-11.md`, `analysis/DS2_COOP_ONLINE_READINESS_2026-10-09.md`, `analysis/COOP_SYNC_MUST.md` / `COOP_SYNC_NEVER.md`, `analysis/MP_OBJECTIVE_SYNC_2026-10-13.md`, `analysis/HOLD_THE_ZONE_OBJECTIVES_2026-10-12.md`, `analysis/ONLINE_PLAYTEST_SYNC_AUDIT_2026-10-13.md`, `analysis/COOP_MODE_SPLIT_AUDIT_2026-10-12.md`, and in `..\Fable\coop_pdb\` the PDB reference `DS2_COOP_PDB_REFERENCE.md` and `DS2_COOP_EFFECTIVE_PATH.md` (Xbox prototype PDB names; addresses there are architectural evidence, the PC addresses in the source are the verified ones).
