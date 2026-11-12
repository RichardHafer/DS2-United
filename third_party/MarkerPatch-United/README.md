# MarkerPatch, Dead Space 2 United edition

Dead Space 2 United ships its own build of [MarkerPatch](https://github.com/Wemino/MarkerPatch)
by Wemino: version 3.2.0 with the small change in `united.patch`. It is the same `dinput8.dll`,
reads the same `MarkerPatch.ini` and keeps every fix and option, except the ones below, which
stay off whatever the INI says because they work on the same game functions as the mod.

MarkerPatch is licensed under the GNU GPL version 2 (`LICENSE` in this folder), and so is this
edition. `build_markerpatch_united.cmd` in the repository root clones MarkerPatch 3.2.0, applies
the patch and builds `build\markerpatch-united\dinput8.dll`.

## Off in this edition

| Option | Why |
| --- | --- |
| `FixSuitChangeTiming` | Hooks `PlayerUniformManager::AttachUniformToPlayer` (0x00EB4E10) before any plugin loads. United's suit code needs that function untouched and switches itself off when it finds the hook, which takes partner suits and the suit changes of bots and necro players with it. |
| `MultiplayerSuit` | Hooks `PlayerUniformManager::LoadUniformStream` (0x00EC6310) and `Player::AttachHelmet` (0x0087D210), which United hooks for partner suits and the patrol helmet, and recolours the suit through the same material swap United uses for the multiplayer suit colours. United's own suit select replaces it. |
| `AlwaysSucceedQTE` | Hooks `PairedAttackCoordinator::Update` (0x010C36E0) and `TestInputEvent::TestInput`. United hooks the first for the bots' grab escapes, and in co-op the scene leader decides every prompt for all players, so a prompt that always succeeds on one PC splits the scene. |
| `FixMainLoopSpin` | Hooks `Time_GetCurTimeInMSec` and the sound provider's frame wait, which retimes the whole main loop. United paces its own per-frame work on that loop. Suspected in the hang of two copies on one PC (the window stops responding while the game keeps running). |
| `FixAudioSyncStall` | Changes the audio mixer's wait and latency. Also on in that hang, and United's voice packs mix on top of the game's audio. |
| `FixStreamingBudget` | Hooks the time slicer and `EntityPartSpawner::Spawn`. United streams its enemy pool during loads and spawns through the same spawner. Also on in that hang. |

`LoadASIPlugins` is always on (United is an ASI plugin), and the loader skips any
`plugins\MarkerPatch*.asi`, so an official MarkerPatch cannot be loaded a second time on top.

The edition exports `MarkerPatchUnitedEdition`. When the official MarkerPatch is loaded instead
(as `dinput8.dll` or as `plugins\MarkerPatch.asi`), United stays off for that session and says why.

## Kept, although they touch the same places

- `IncreasedEntityPersistence`: United's `[Lifetime]` sets the corpse limits again where it wants
  its own (game modes); in the story `0` leaves them to MarkerPatch.
- `SkipArtificialLoadingDelay`: United puts the skipped branch back while its enemy pool loads
  and releases the loading screen itself afterwards.
- `RawMouseInput`, `FOVScale`, `HavokPhysicsFix`: United hooks behind them on the same functions
  (the bots' aim and camera, the bots' kinesis) and was played with them on.
