# Dead Space 2 United experimental release review

Reviewed on 2026-11-06 against the local combined Steam PC build. The binary compiled and linked as an x86 DLL with matching private debugging symbols. The combined plugin has not been installed or played as part of this review.

| Area | Offline evidence | Remaining release check |
| --- | --- | --- |
| Campaign spawns | 2,227 FuncSpawn records in ten main archives and five transition groups. 1,299 have enemy targets, 895 have other targets, and 33 have an empty target GUID in the extract. The runtime checks any target assigned later. | Test a scripted kill and its downstream door with randomization, enhanced variants, hordes and extra enemies enabled. Archive groups do not map one-to-one to chapters on the HUD. |
| Scripted enemies and additional spawns | The review identified ten enemy targets directly bound to scenes by original GUID. These keep their identity. Failed forced replacements must not clone a prop. Catalog and policy checks cover 1,057 entries and 112 scripted sources. | Verify authored arrivals, vent exits and death-count messages on both peers, including after restarting the checkpoint. |
| Seed | Lobby and mesh changes preserve the full unsigned range, including zero. The native tests cover old senders and campaign startup. The workbench still uses the seed loaded at process start. | Reconnect with different local seeds and compare the same authored encounter and settings on both machines. |
| Elevators | 54 ride switches attached to 42 cabin movers. The switch interface offset was confirmed in a retail dump. Interior buttons check cabin occupancy before use. Repeated manager calls stay held. Unresolved cabin geometry falls back to the existing ride gate. | Try a landing call, repeatedly press the cabin switch with one player outside, then enter together. Repeat with the client operating it and with a partner disconnecting. Other lift scripts retain the existing gates and timeout. |
| Game modes | Wave Survival, Mercenaries, Objective and Hold the Zone now retry missing vent targets on the current map. Cleanup validates object identity before deletion. Offline coverage includes nine arenas, seven objective plans and vent windows. | Stream out and back during a wave, then restart with enemies and a Mercenaries capsule present. |
| Network and story | Peer admission, host updates and duplicate restart handling were corrected. Local UDP handshake, peer tests, three-machine scene agreement, Tormentor scene-chain and story-object tests passed. | Exercise a disconnect during loading, chapter transition and rejoin under packet loss. Mirrored events still use a trust model suited to known partners. Ordinary nonterminal prop damage can still diverge under loss. |

## Distribution

### Follow-up on client parity

- A distant host that receives a gather cue now keeps local door streaming active until arrival. A pull scene can still request its rooms immediately. The three-machine scene fixture covers both paths.
- Partner rooms retained in the previous zone request remain eligible on the next switch while the remote player still needs them. A native-code fixture covers two successive switches, departure and reset. The engine limit remains 46 entries. Room and visibility reports now reject duplicates, older sequence numbers and incorrect sender slots.
- The DS3 August prototype names the door RPC `RPC_PingRemotePlayer_BlockedByActiveStreaming`. The RPC name also appears in the local DS3 PC retail memory image. This supports a door-specific streaming message, but does not establish how retail prioritizes or unloads rooms.
- Clients now apply the configured player/bot collision exclusion to local bot ghosts. A retail client dump contained two such ghosts with the expected movement controller and valid collision IDs. The offline fixture covers three ghosts, removal and filtering out a non-bot.
- Switching hosts now clears the previous inventory before loading another host's save. Explicit blueprint seeds survive a session, and inherited workbench seeds participate in the settings comparison. Clients sharing one installation and host can still share a progress filename.
- Mercenaries clients receive the current set of standing capsules. Queued wave payments keep their own wave number. The experimental reward protocol now retries until acknowledged within the same host run and client load generation.

Prioritize a two-instance run with the client leading, a host two doors behind and a host bot. Cross two streaming boundaries, test a gather and a pull scene, then repeat after restarting the checkpoint. Resident geometry, collision and actual visibility require observation on each instance.

### Packet loss and streaming within an active session

| Path | Change and offline evidence | Remaining limit |
| --- | --- | --- |
| Scene start and control | A negotiated receipt covers acceptance of a specific commit into the scene queue. Retries are per link and bounded by a 1.5 to 4 second window. The three-machine test loses the first three commits on both hops and then an acknowledgement, with exactly one acceptance per receiver. Restart drops old receipts, queued spectator cues and input ownership. | Receipt confirms queue acceptance. Actor availability and successful native playback are distinct. Losing the whole window can still miss the scene. |
| Wall cysts | The existing checkpoint bitmap now also observes native retirement. Tests distinguish a living stream-out from a confirmed kill between scans and recover a lost first snapshot. | This change covers the wall body. Its ejected mine is a different entity. |
| Destructible props | Restricted to 4,162 unique authored campaign GUIDs. A verified lethal hit may be retried up to five times across nine seconds. The receiver checks the world generation, object identity, placement, owner and previous health before dispatch. | Bounded queue of 32. A room that streams in later, or divergent pre-hit health, can still miss the transition. |
| Panels | Sender and sequence now order claims and releases. Leases account for RTT and jitter. Two hundred seeded runs exercised loss, duplication and reordering. | Simultaneous first use before either side receives a claim remains a race. |
| Kinesis | Movement samples are validated before entering Havok. A deferred stop rechecks local ownership, and a new pose cannot extend an old grab on a different body. | Cell pose and socket state are separate. Two Courtyard cells stayed attached remotely. The retail detach path schedules a later script message; blindly replaying it could activate level logic twice. Trace mode samples attachment again after 250 and 1,200 ms. See analysis/COOP_POWER_CELL_SOCKETS_2026-11-06.md. |
| Wave payout | Capable peers retry per-player grants until acknowledged. Tested with the first three deliveries and acknowledgements lost, new runs and checkpoint changes. | Applies within the active run and peer load generation; legacy clients keep their former delivery method. |

The transport report now counts frames where a full receive batch still leaves bytes queued. Offline delay tests do not benchmark real frame times or guarantee playability at a specific ping.

### Packaging

The configured remote is RichardHafer/DS2-Randomizer. The tracked randomizer tree by itself cannot reproduce the combined build. A scan of the three plugin translation units, SafetyHook and Zydis found 250 source files via quoted includes, of which 60 are tracked, 161 are ignored and 29 are otherwise untracked. Prepare a reviewed source manifest and test from a clean checkout before tagging.

The current README describes the standalone randomizer. The older friends packer includes both ASIs. The newer playtest packer reads a host installation and defaults to including Outbreak and MarkerPatch. Prepare the public package using a clean configuration, exactly one combined ASI, and a reviewed asset list. Verify upgrade instructions against an installation containing the standalone observer. Release defaults remain the maintainer's decision.
