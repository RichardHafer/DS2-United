// Dead Space 2 United by Hafer.
// Cutscenes in co-op.
//
// A scripted sequence cue is mirrored to every machine (runtime_coop_mirror),
// and on each of them the sequence takes the local Isaac as its actor. So a
// cutscene plays everywhere with the local body in the triggering player's
// role, and the partners' ghost bodies stand around in it. Two rules follow:
//
//  * While the local Isaac runs a sequence, every partner body is hidden (and
//    the gun in its hands): one Isaac on each screen, the one the cutscene moves.
//  * The player who triggered the cutscene decides it. On every other machine
//    (a follower: the local sequence started while a partner's cue was the
//    reference, see streams::SetReference) the local sequence cues are not sent
//    out, local controller input is blocked and the local Isaac takes no damage,
//    so the outcome (freed, killed, the next cue) comes from the leader's
//    mirrored cues alone. The valve gives input and damage back after
//    FollowerValveMs.
//
// Everything measured on PC (dump 62708, prologues identical in four others):
//  * Player's IScriptedSequenceTarget sits at player+0x230 (the Player ctor
//    stores vtable 0x01B293A8 there, 0x008A7AD9). Base Start 0x00E93C70 sets
//    iface+8 = sequence and iface+0x10 |= 5; End 0x00E93CE0 clears bit 2 and
//    iface+8. Running = [player+0x240] bit 2 and [player+0x238] set.
//  * Hide/Show are Player vtable +0x68 (0x008612C0) / +0x6C (0x00878EB0),
//    thiscall, no arguments: a counter at entity+0x16D, shared with the game
//    in balanced pairs, so every hide here is undone by exactly one show.
//  * The held item: [player+0xCEC] - 0x10, hidden through the same two slots
//    (the game's own active-item visibility handler, 0x008AB94E).
//  * Player::ShouldProcessDamage 0x00892D60 (vtable +0x20C, thiscall(damage),
//    ret 4), the gate that already tests the invincible flag [+0x3DC] bit 14.
//  * The controller filter 0x004132C0 (cdecl, returns true to block): the
//    ControllerManager ([0x0201C0AC]) answers every query with "no input"
//    for the frame when it says so (0x0077E7E0 stores !filter at +0x18D0).
namespace bot { bool MortalNow(); }   // runtime_coop_bot.inc
namespace scenepartner { bool IsPuppet(uint32_t ghost); }   // runtime_coop_scenepartner.inc
namespace cutscene {
bool installed = false, hidePartners = true, leaderOnly = true, blockInput = true, followerInvincible = true;
DWORD valveMs = 90000;
// The shooting part of an interactive scene is no sequence: the game hands
// Isaac to PlayerStationaryShootingSM (Tiedemann's javelin, the Tormentor's
// arm). Until 24.09. the scene ended with the sequence, so the leader stopped
// sending aim and fire button (leader_aim sent=0), the follower was his own
// again, missed his copy of the target and failed on the scene's timer while
// the leader succeeded. A stationary part (the aim hook saw the local Isaac within
// kStationaryFreshMs) now keeps the scene, and an ending sequence waits
// kEndGraceMs for one to begin.
DWORD stationaryAt = 0, endPendingSince = 0;
bool inStationary = false;
constexpr DWORD kStationaryFreshMs = 300, kEndGraceMs = 1000;
volatile LONG stationaryParts = 0;
constexpr DWORD kLeaderGoneMs = 5000;
DWORD leaderGoneSince = 0;
bool valveHeldLogged = false;
volatile LONG valvesHeld = 0;
constexpr uint32_t kSequence = 0x238, kSequenceFlags = 0x240, kActiveItem = 0xCEC;
constexpr uint32_t kHideSlot = 0x68, kShowSlot = 0x6C;
constexpr uint32_t kShouldProcessDamageRva = 0x492D60, kInputFilterRva = 0x0132C0;
safetyhook::InlineHook damageHook, filterHook;

bool running = false, following = false, valveOpen = false;
uint32_t sequenceNow = 0, localPlayer = 0;
unsigned leader = 0xFF;
DWORD startedAt = 0;
// What was hidden, per partner slot: his ghost body, the item in its hands and his helmet.
// A puppet ([ScenePartner]) keeps its body and helmet shown: bodyShown says the body's
// hide was undone (or never done) while the slot still tracks it for its gun.
struct Hidden { uint32_t body, bodyVtable, item, itemVtable, helmet, helmetVtable; bool bodyShown; };
// One per partner, then one per host bot: on a client the bots are ghosts too
// (machine 0xF0 + bot) and stood in its scenes (12.10.).
Hidden hidden[coop_peers::kMaxPeers + kMaxBots]{};
volatile LONG starts = 0, followerStarts = 0, ends = 0, bodiesHidden = 0, itemsHidden = 0, shown = 0, showSkipped = 0,
              damageBlocked = 0, inputBlockedFrames = 0, cuesSuppressed = 0, valves = 0, faults = 0;

// The finale's scenes are every player's own ([Cutscene] FinaleOwnScenes).
// 25.09. 20:17, the Nicole fight: each machine's dementia fight is itself one
// ScriptedSequence on its Isaac (36F33720 on the host ran 1148 s). The host
// "led" it and hid the client's ghost the whole fight; the client led his own,
// then the host's leader input arrived and he yielded ("both of us led this
// scene: partner 0 keeps the lead"), followed 30349C20 as "a partner's cutscene
// cue" and, after the checkpoint restart, played his own dementia intro with
// the host's controller for 15 s. From the first finale message on (the same
// scope as the mirror's local names) nobody follows, yields, hides or streams
// his controller: every player leads his own scenes.
bool finaleOwnScenes = false;
volatile LONG finaleOwnStarts = 0;
bool OwnScenes() { return finaleOwnScenes && mirror::finaleActive; }
bool Following() { return installed && leaderOnly && following && !valveOpen && !OwnScenes(); }
bool FollowsSlot(unsigned slot) { return running && Following() && leader == slot; }
bool Leading() { return installed && running && !following; }
DWORD FollowingSince() { return Following() ? startedAt : 0; }
// Two more ways to tell a partner's scene from a local one (23.09., the ch11 scene:
// the host's direct "gun_fire" to a ScriptableProp started a sequence on the
// client's Isaac, no sequence cue ever came, so the client "led", got the
// button prompts without the scene and died):
//   * a replay of a partner's message started it: the local sequence began inside
//     the replay call, or within kReplayWindowMs after a direct replay,
//   * a partner has been in a scene for more than kSceneFirstMs already
//     (his VISI packets report how long ago his Isaac last ran one), it
//     began at most kSceneRecentMs before the local one and he is in the same
//     stream set (a long scene of his elsewhere must not take the local one over).
// A sequence that follows straight on from a followed one is followed too.
constexpr DWORD kReplayWindowMs = 1000, kSceneFirstMs = 1000, kSceneRecentMs = 10000, kInScene = 1500;
DWORD replayStartAt = 0, directReplayAt = 0;
unsigned replayAuthor = 0xFF;
DWORD partnerSceneSince[coop_peers::kMaxPeers]{};
volatile LONG followByReplay = 0, followByPartnerScene = 0, followInherited = 0, leadKept = 0, followersIgnored = 0;
// The last scene led here. A partner who is "in a scene" because he follows it
// must not make this machine his follower (24.09.: the host led the insider fight, its
// next sequence took the client's scene as the earlier one, both followed,
// both were invincible and the failed prompts killed nobody).
DWORD ledSince = 0, ledEndedAt = 0;
bool SequenceRunningNow() {
    const uint32_t player = inLevel ? LocalCampaignPlayer() : 0;
    unsigned char flags = 0;
    uint32_t sequence = 0;
    return player && Read(player + 0x240, &flags, 1) && (flags & 4) && Read(player + 0x238, &sequence, 4) && sequence;
}
void NoteReplay(unsigned author, bool started, bool direct, DWORD now) {
    if (author >= coop_peers::kMaxPeers) return;
    replayAuthor = author;
    if (started) replayStartAt = now;
    if (direct) directReplayAt = now;
}
// A partner is in a scene now: his last VISI packet (at most kFreshMs old)
// said his Isaac ran a sequence at most kInScene before it was sent.
bool PartnerInScene(unsigned slot, DWORD now) {
    if (slot >= coop_peers::kMaxPeers || !partnerSceneSince[slot]) return false;
    const auto& peer = streams::vis::peers[slot];
    return peer.at && TicksSince(now, peer.at) <= streams::vis::kFreshMs;
}
void NotePartnerScene(unsigned slot, uint32_t sequenceAgoMs, DWORD now) {
    if (slot >= coop_peers::kMaxPeers) return;
    if (sequenceAgoMs == 0xFFFFFFFFu || sequenceAgoMs > kInScene) { partnerSceneSince[slot] = 0; return; }
    // His "ago" was measured when he sent it; the packet then spent a wire trip.
    if (!partnerSceneSince[slot]) partnerSceneSince[slot] = now - sequenceAgoMs - netclock::OneWay();
}
void NoteSuppressed(uint32_t hash) {
    if (InterlockedIncrement(&cuesSuppressed) > 64) return;   // the counter keeps going, the log does not
    gate::Event("coop_cutscene follower: our cue %08X (%s) not sent, the leader's cues decide", hash, coop_names::Name(hash));
}

int Filter(unsigned) { InterlockedIncrement(&faults); return EXCEPTION_EXECUTE_HANDLER; }
bool CallSlot(uint32_t entity, uint32_t slot) {
    uint32_t vtable = 0, fn = 0;
    if (!entity || !Read(entity, &vtable, 4) || !Read(vtable + slot, &fn, 4) || fn < base + 0x1000 || fn >= base + gate::kRdataLo) return false;
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(fn)(reinterpret_cast<void*>(entity));
        return true;
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}
bool Hide(uint32_t entity, uint32_t& vtableOut) {
    uint32_t vtable = 0;
    if (!entity || !Read(entity, &vtable, 4) || vtable < base + gate::kRdataLo || vtable >= base + gate::kRdataHi) return false;
    if (!CallSlot(entity, kHideSlot)) return false;
    vtableOut = vtable;
    return true;
}
// Shown only while the object is provably the one that was hidden: a body while it is
// still this partner's ghost, an item while it is still in that ghost's hands.
// Anything else was freed (level change, disconnect) and took its hide count
// with it; calling into it would be calling into freed memory.
void Show(uint32_t entity, uint32_t vtable) {
    uint32_t now = 0;
    if (entity && Read(entity, &now, 4) && now == vtable && CallSlot(entity, kShowSlot)) InterlockedIncrement(&shown);
    else InterlockedIncrement(&showSkipped);
}
// The helmet is its own prop (a LinearScriptableProp from the suit stream),
// held at Player+0x168C as a SafePtr (object - 0x10); hiding the body leaves
// it floating in the scene (24.09.). Xbox PDB: m_pHelmet +0x16AC.
constexpr uint32_t kHelmet = 0x168C;
uint32_t Helmet(uint32_t ghost) {
    uint32_t helmet = 0, vtable = 0;
    if (!Read(ghost + kHelmet, &helmet, 4) || helmet <= 0x10010) return 0;
    helmet -= 0x10;
    return Read(helmet, &vtable, 4) && vtable >= base + gate::kRdataLo && vtable < base + gate::kRdataHi ? helmet : 0;
}
volatile LONG helmetsHidden = 0;
uint32_t ActiveItem(uint32_t ghost) {
    uint32_t item = 0;
    return (Read(ghost + kActiveItem, &item, 4) && item > 0x10010) ? item - 0x10 : 0;
}
uint32_t SlotMachine(unsigned s) { return s < coop_peers::kMaxPeers ? peerTable.peers[s].machine : 0xF0u + (s - coop_peers::kMaxPeers); }
// A partner playing a necromorph has no Isaac ghost; his necro ghost sits in a
// PlayerManager slot of its own.
uint32_t NecroGhostFor(uint32_t machine) {
    uint32_t manager = 0, slots = 0, capacity = 0;
    if (!Read(base + kPlayerManagerRva, &manager, 4) || !manager || !Read(manager + 4, &slots, 4) || !slots || !Read(manager + 8, &capacity, 4) ||
        !capacity || capacity > 8)
        return 0;
    for (uint32_t i = 0; i < capacity; ++i) {
        uint32_t bp = 0;
        if (!Read(slots + 4 * i, &bp, 4) || bp < 0x103C0) continue;
        if (IsGhostNecro(bp - 0x3C0) && MachineOf(bp - 0x3C0) == machine) return bp - 0x3C0;
    }
    return 0;
}
uint32_t BodyFor(uint32_t machine) {
    const uint32_t ghost = GhostForMachine(machine);
    return ghost ? ghost : NecroGhostFor(machine);
}
// The ghost lookup can miss for a frame while the body is fine; then the body
// is recognised by itself: same vtable, still a ghost, still this machine's.
bool StillHisGhost(const Hidden& h, uint32_t machine) {
    uint32_t vtable = 0;
    return h.body && Read(h.body, &vtable, 4) && vtable == h.bodyVtable && (IsGhostPlayer(h.body) || IsGhostNecro(h.body)) &&
           MachineOf(h.body) == machine;
}
void ShowSlot(unsigned s, uint32_t liveGhost) {
    Hidden& h = hidden[s];
    const bool bodyOk = liveGhost ? liveGhost == h.body : StillHisGhost(h, SlotMachine(s));
    if (h.item) {
        if (bodyOk && ActiveItem(h.body) == h.item) Show(h.item, h.itemVtable);
        else InterlockedIncrement(&showSkipped);
    }
    if (h.helmet) {
        if (bodyOk && Helmet(h.body) == h.helmet) Show(h.helmet, h.helmetVtable);
        else InterlockedIncrement(&showSkipped);
    }
    if (h.body && !h.bodyShown) {
        if (bodyOk) Show(h.body, h.bodyVtable);
        else InterlockedIncrement(&showSkipped);
    }
    h = Hidden{};
}
void ShowAll(bool levelAlive) {
    const DWORD now = GetTickCount();
    for (unsigned s = 0; s < std::size(hidden); ++s) {
        if (!hidden[s].body && !hidden[s].item && !hidden[s].helmet) continue;
        if (levelAlive && s < coop_peers::kMaxPeers && roles::Watching(s, now)) continue;   // a necro watching as Isaac stays hidden
        uint32_t live = 0;
        if (levelAlive && (s >= coop_peers::kMaxPeers || peerTable.peers[s].used)) live = BodyFor(SlotMachine(s));
        ShowSlot(s, live);
    }
}
volatile LONG botGhostsHidden = 0, necroGhostsHidden = 0, watchersHidden = 0, watchersShown = 0, puppetBodiesShown = 0, puppetBodiesHidden = 0;
void HidePartners(bool watchersOnly = false) {
    const DWORD now = GetTickCount();
    for (unsigned s = 0; s < std::size(hidden); ++s) {
        if (watchersOnly && (s >= coop_peers::kMaxPeers || !roles::Watching(s, now))) continue;
        if (s < coop_peers::kMaxPeers) {
            const auto& peer = peerTable.peers[s];
            if (!peer.used || !peer.token || now - peer.lastSeen > 10000) continue;
        } else if (IsHost()) {
            continue;   // the host's own bots hide themselves ([Bot] HideInScenes)
        } else if (!bot::HidesInThisScene()) {
            continue;   // 12.10.: the client hid the bot ghosts in every scene, the host only in cinematics
        }
        const uint32_t ghost = BodyFor(SlotMachine(s));
        Hidden& h = hidden[s];
        if (h.body && ghost && h.body != ghost) ShowSlot(s, ghost);   // a new ghost for this slot: the old one is gone
        if (!ghost) continue;
        const bool puppet = s < coop_peers::kMaxPeers && IsGhostPlayer(ghost) && scenepartner::IsPuppet(ghost);
        if (puppet && !h.bodyShown) {
            // a puppet stands in the scene: its body and helmet show, its gun stays hidden
            if (h.body == ghost) Show(h.body, h.bodyVtable);
            else if (!h.body) { h.body = ghost; Read(ghost, &h.bodyVtable, 4); }
            if (h.helmet) { Show(h.helmet, h.helmetVtable); h.helmet = 0; }
            h.bodyShown = true;
            InterlockedIncrement(&puppetBodiesShown);
        } else if (!puppet && h.bodyShown && h.body == ghost && Hide(ghost, h.bodyVtable)) {
            h.bodyShown = false;   // the puppet is over: hidden again like every partner
            InterlockedIncrement(&puppetBodiesHidden);
        }
        if (!h.body && Hide(ghost, h.bodyVtable)) {
            h.body = ghost;
            InterlockedIncrement(&bodiesHidden);
            if (watchersOnly) InterlockedIncrement(&watchersHidden);
            if (s >= coop_peers::kMaxPeers) InterlockedIncrement(&botGhostsHidden);
            else if (IsGhostNecro(ghost)) InterlockedIncrement(&necroGhostsHidden);
        }
        if (!h.body || !IsGhostPlayer(ghost)) continue;   // helmet and gun: an Isaac's only
        const uint32_t helmet = h.bodyShown ? 0 : Helmet(ghost);
        if (helmet != h.helmet) {
            if (h.helmet) { Show(h.helmet, h.helmetVtable); h.helmet = 0; }
            if (helmet && Hide(helmet, h.helmetVtable)) { h.helmet = helmet; InterlockedIncrement(&helmetsHidden); }
        }
        const uint32_t item = ActiveItem(ghost);
        if (item == h.item) continue;
        // He switched weapons: the one he put away is his again (it is alive,
        // it was in his hands a frame ago), the new one is hidden.
        if (h.item) { Show(h.item, h.itemVtable); h.item = 0; }
        if (item && Hide(item, h.itemVtable)) { h.item = item; InterlockedIncrement(&itemsHidden); }
    }
}

// The last messages before the scene, newest first, at most 3 s old, three
// to a log line (the line buffer holds 192 characters). Broadcasts come by the
// thousand, so the direct sends (triggers, the sequence's own starts) are
// counted apart.
void LogWhatStartedIt(DWORD now) {
    char line[192]; size_t used = 0;
    unsigned shownDirect = 0, shownBroadcast = 0, inLine = 0;
    const LONG total = InterlockedCompareExchange(&gate::recentCount, 0, 0);
    for (LONG i = total - 1; i >= 0 && i >= total - static_cast<LONG>(gate::kRecentMessages); --i) {
        const gate::RecentMessage r = gate::recentMessages[static_cast<unsigned>(i) % gate::kRecentMessages];
        if (!r.hash || now - r.at > 3000) continue;
        if (r.direct ? shownDirect >= 9 : shownBroadcast >= 9) continue;
        ++(r.direct ? shownDirect : shownBroadcast);
        if (!inLine) used = static_cast<size_t>(std::snprintf(line, sizeof(line), "coop_cutscene before it:"));
        if (r.direct)
            used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, " [direct %.24s %08X vt %08X -%lu]", coop_names::Name(r.hash), r.hash, r.target, static_cast<unsigned long>(now - r.at)));
        else
            used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, " [%.24s %08X -%lu]", coop_names::Name(r.hash), r.hash, static_cast<unsigned long>(now - r.at)));
        if (used >= sizeof(line)) used = sizeof(line) - 1;
        if (++inLine == 3) { gate::Event("%s", line); inLine = 0; }
    }
    if (inLine) gate::Event("%s", line);
}
bool FollowedChain(DWORD now, unsigned& slot);   // below, with the roles of the scene that ended
volatile LONG followByChain = 0;
void Begin(uint32_t sequence, DWORD now, bool continued) {
    const bool wasFollowing = continued && following && !valveOpen;
    const unsigned wasLeader = leader;
    sequenceNow = sequence;
    if (!continued) mirror::ghostseq::BeginScene(sequence, now);
    if (!continued) { startedAt = now; leaderGoneSince = 0; valveHeldLogged = false; }
    valveOpen = false;
    const char* why = "ours, we lead";
    following = streams::referenceUntil && now < streams::referenceUntil && streams::referenceSlot < coop_peers::kMaxPeers;
    leader = following ? streams::referenceSlot : 0xFEu;
    if (following) why = "a partner's cutscene cue, we follow";
    // The next part of a locally led scene keeps the lead.
    const bool keepLead = continued && !wasFollowing && wasLeader == 0xFEu;
    if (keepLead) {
        following = false; leader = 0xFEu; why = "the next part of our scene, we still lead";
        InterlockedIncrement(&leadKept);
    }
    if (!following && wasFollowing) {
        following = true; leader = wasLeader; why = "the next part of the followed scene";
        InterlockedIncrement(&followInherited);
    }
    if (!following && !keepLead && replayAuthor < coop_peers::kMaxPeers &&
        ((replayStartAt && now - replayStartAt <= kReplayWindowMs) || (directReplayAt && now - directReplayAt <= kReplayWindowMs))) {
        following = true; leader = replayAuthor; why = "started by a partner's replayed message, we follow";
        InterlockedIncrement(&followByReplay);
    }
    // The next scene of a chain we followed: 02.11., the Tormentor, the client's own copy of the
    // second scene began 170 ms before the host's, the client led it and the camera went its own way.
    unsigned chainLeader = 0xFF;
    if (!following && !keepLead && FollowedChain(now, chainLeader)) {
        following = true; leader = chainLeader; why = "the next scene of the chain we followed";
        InterlockedIncrement(&followByChain);
    }
    if (!following && !keepLead) {
        for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
            if (!partnerSceneSince[s] || now - partnerSceneSince[s] <= kSceneFirstMs || now - partnerSceneSince[s] > kSceneRecentMs) continue;
            // His scene began with or after one led here that is barely over: he follows this one.
            if (ledEndedAt && now - ledEndedAt <= kSceneRecentMs && partnerSceneSince[s] + kSceneFirstMs >= ledSince) {
                InterlockedIncrement(&followersIgnored);
                continue;
            }
            if (!streams::vis::peers[s].at || !streams::GuidEqual(streams::vis::peers[s].packet.area, streams::area)) continue;
            following = true; leader = s; why = "the partner was in his scene first, we follow";
            InterlockedIncrement(&followByPartnerScene);
            break;
        }
    }
    if (OwnScenes()) {
        following = false; leader = 0xFEu; why = "the finale, every player's scenes are his own";
        InterlockedIncrement(&finaleOwnStarts);
    }
    if (following && leader < coop_peers::kMaxPeers) streams::SetReference(leader, now);
    InterlockedIncrement(&starts);
    if (following) InterlockedIncrement(&followerStarts);
    InterlockedExchange(&gate::sceneTraceUntil, static_cast<LONG>(now + 3000));
    if (gate::SceneTraceOn()) gate::SceneTrace("coop_scene t=%lu start sequence %08X %s leader=%u", static_cast<unsigned long>(now), sequence,
                                               following ? "followed" : "led", leader);
    gate::Event("coop_cutscene our Isaac runs sequence %08X: %s (leader %u)", sequence, why, leader);
    if (!continued) LogWhatStartedIt(now);
}
// The roles of the scene that just ended. 25.09., Tiedemann at the end of
// the game: the javelin part began more than a second after the sequence
// before it had ended, both machines had closed the scene by then, the leader
// aimed with nothing sent (leader_aim sent=0) and the follower was his own.
// A stationary part within kStationaryAfterMs of a scene takes it up again
// with the same leader.
namespace leadaim { extern DWORD leaderAt; }   // below
// A follower in a shooting part whose leader sends no angles is not held.
// 25.09., Tiedemann: the client went into the javelin part and took the
// host's controller, the host never did (no angles, stale=542): the client
// could neither aim nor fire. After kLeaderSilentMs without the leader's aim
// the follower's own hands come back for this part (the valve).
constexpr DWORD kLeaderSilentMs = 1500;
DWORD stationaryFrom = 0;
volatile LONG stationaryOwnHands = 0;
bool endedFollowing = false, endedWithPartner = false;
unsigned endedLeader = 0xFF;
DWORD endedAt = 0;
constexpr DWORD kChainMs = 8000;
bool FollowedChain(DWORD now, unsigned& slot) {
    if (running || OwnScenes() || !endedFollowing || !endedAt || now - endedAt > kChainMs || endedLeader >= coop_peers::kMaxPeers ||
        !peerTable.peers[endedLeader].used)
        return false;
    slot = endedLeader;
    return true;
}
bool InFollowedChain(DWORD now) { unsigned slot = 0xFF; return installed && leaderOnly && FollowedChain(now, slot); }
constexpr DWORD kStationaryAfterMs = 20000;
volatile LONG stationaryResumed = 0;
void End(DWORD now, bool alive) {
    endedFollowing = following && !valveOpen;
    endedLeader = leader;
    endedWithPartner = peerTable.Count() != 0;
    endedAt = now;
    if (!following || valveOpen) { ledSince = startedAt; ledEndedAt = now; }
    ShowAll(alive);
    InterlockedIncrement(&ends);
    gate::Event("coop_cutscene sequence %08X over after %u ms (%s)", sequenceNow, static_cast<unsigned>(now - startedAt),
                following ? "followed" : "led");
    running = following = valveOpen = false;
    leaderGoneSince = 0; valveHeldLogged = false;
    sequenceNow = 0;
    leader = 0xFF;
    // A stationary part belongs to this scene; a resumed one starts its own clock.
    inStationary = false;
    stationaryFrom = endPendingSince = 0;
}

// Game thread, from Poll.
void Tick(DWORD now) {
    if (!installed) return;
    const uint32_t player = inLevel ? LocalCampaignPlayer() : 0;
    if (!player || (localPlayer && player != localPlayer)) {
        if (running) End(now, inLevel);   // still in the level: the partners are still there to be shown
        localPlayer = player;
        if (!player) return;
    }
    localPlayer = player;
    // A partner's necro watching a scene as Isaac stays hidden here for the whole watch (his
    // Isaac, then the necro body on his way back); shown again once his role changes.
    if (peerTable.Count() && hidePartners && !OwnScenes()) {
        HidePartners(true);
        if (!running)
            for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s)
                if (hidden[s].body && !roles::Watching(s, now)) { ShowSlot(s, BodyFor(SlotMachine(s))); InterlockedIncrement(&watchersShown); }
    }
    unsigned char flags = 0;
    uint32_t sequence = 0;
    const bool inSequence = Read(player + kSequenceFlags, &flags, 1) && (flags & 4) && Read(player + kSequence, &sequence, 4) && sequence;
    if (!inSequence) {
        if (!running && stationaryAt && now - stationaryAt < kStationaryFreshMs && endedAt && endedWithPartner &&
            now - endedAt < kStationaryAfterMs && peerTable.Count() && (!endedFollowing || endedLeader < coop_peers::kMaxPeers)) {
            running = true;
            following = endedFollowing;
            leader = endedFollowing ? endedLeader : 0xFEu;
            startedAt = now;
            leaderGoneSince = 0;
            InterlockedIncrement(&stationaryResumed);
            gate::Event("coop_cutscene stationary shooting %u ms after the scene: it resumes (%s, leader %u)",
                        static_cast<unsigned>(now - endedAt), following ? "following" : "leading", leader);
        }
        if (!running) return;
        if (stationaryAt && now - stationaryAt < kStationaryFreshMs) {
            streams::vis::sequenceSeenAt = now;   // the partners' scene signal (a watching necro waits for the whole scene)
            if (!inStationary) {
                stationaryFrom = now;
                inStationary = true;
                InterlockedIncrement(&stationaryParts);
                gate::Event("coop_cutscene stationary shooting inside the scene: it goes on (%s, leader %u)", following ? "following" : "leading", leader);
            }
            endPendingSince = 0;
            InterlockedExchange(&gate::sceneTraceUntil, static_cast<LONG>(now + 3000));
            if (hidePartners && peerTable.Count() && !OwnScenes()) HidePartners();
            const DWORD silent = netclock::Window(kLeaderSilentMs);
            if (following && !valveOpen && stationaryFrom && now - stationaryFrom > silent &&
                (!leadaim::leaderAt || now - leadaim::leaderAt > silent)) {
                valveOpen = true;
                InterlockedIncrement(&stationaryOwnHands);
                gate::Event("coop_cutscene shooting part: leader %u sends no aim for %u ms, the controls are ours again", leader,
                            static_cast<unsigned>(leadaim::leaderAt ? now - leadaim::leaderAt : now - stationaryFrom));
            }
            return;
        }
        if (!endPendingSince) endPendingSince = now;
        // a shooting part may begin right after the sequence; a follower hears of it a wire trip late
        if (now - endPendingSince < (following ? netclock::Window(kEndGraceMs) : kEndGraceMs)) return;
        endPendingSince = 0;
        inStationary = false;
        End(now, true);
        return;
    }
    endPendingSince = 0;
    inStationary = false;
    if (!running || sequence != sequenceNow) {
        const bool continued = running;
        running = true;
        Begin(sequence, now, continued);
    }
    streams::vis::sequenceSeenAt = now;   // the visibility sync only ever acts right after a scene
    InterlockedExchange(&gate::sceneTraceUntil, static_cast<LONG>(now + 3000));
    if (OwnScenes()) {
        // Partners hidden before the finale began come back; a follow in progress ends.
        ShowAll(true);
        if (following) { following = false; leader = 0xFEu; gate::Event("coop_cutscene the finale began: we stop following, our scenes are our own"); }
        return;
    }
    // A short sequence (the kinesis panel) hides nobody, a playable one (the dementia fight) shows
    // whom an intro had hidden (scene kinds, 17.10.).
    if (hidePartners && peerTable.Count() && scenekind::LocalHidesPartners()) HidePartners();
    else if (scenekind::local == scenekind::kPlayable) ShowAll(true);
    // 24.09., the Tormentor: its sequence runs 99 s. The valve opened at 90 s
    // while the leader was still in it, the follower had his damage back, died
    // in the scene, and the death restarted both. The valve is for a leader
    // who has left the scene (or the game), not for a long scene: it waits
    // until he has been out of one for kLeaderGoneMs, or for four times
    // valveMs whatever he reports.
    if (following && !valveOpen) {
        const bool leaderIn = leader < coop_peers::kMaxPeers && PartnerInScene(leader, now);
        if (leaderIn) leaderGoneSince = 0;
        else if (!leaderGoneSince) leaderGoneSince = now;
        const DWORD elapsed = now - startedAt;
        const bool leaderGone = leaderGoneSince && now - leaderGoneSince >= netclock::Window(kLeaderGoneMs);
        if (elapsed >= valveMs && (leaderGone || elapsed >= valveMs * 4)) {
            valveOpen = true;
            InterlockedIncrement(&valves);
            gate::Event("coop_cutscene follower valve after %u ms: input and damage are ours again (%s)", static_cast<unsigned>(elapsed),
                        leaderGone ? "the leader is out of his scene" : "hard limit, the leader still reports a scene");
        } else if (elapsed >= valveMs && leaderIn && !valveHeldLogged) {
            valveHeldLogged = true;
            InterlockedIncrement(&valvesHeld);
            gate::Event("coop_cutscene follower valve held after %u ms: the leader is still in his scene", static_cast<unsigned>(elapsed));
        }
    }
}

// The leader's world decides how a shared scene ends ([Cutscene] LeaderDecidesDeaths).
// 25.09., the Tormentor: the client followed the host's scene into the
// part where the player shoots the arm holding him; the host's world went on,
// the client's own world ran its failure and started the kill sequence on his
// Isaac (B9682145), and his death restarted both. A scripted death is a
// ScriptedSequence whose settings flags (attributes +0x74) carry 0x10;
// Player::StartScriptedSequence (PC 0x008790D0, thiscall on the player's
// IScriptedSequenceTarget at +0x230, (sequence, bool), ret 8; Xbox
// 0x845E47B8) is where it takes the player. Its "true" is the engine's own
// refusal (the target is busy with another sequence): the sequence does not
// run on the local Isaac. It is refused while a partner's scene decides (this
// machine follows it, or the one it followed ended less than kDeathAuthorityMs
// ago) and while a partner's replayed message is being dispatched (his death
// is not this player's). If the leader fails too, his own death restarts everybody.
bool leaderDecidesDeaths = false;
constexpr uint32_t kStartSequenceRva = 0x4790D0, kSequenceTarget = 0x230, kSequenceSettings = 0x74, kScriptedDeathFlag = 0x10;
constexpr DWORD kDeathAuthorityMs = 20000;
safetyhook::InlineHook startSequenceHook;
volatile LONG deathsRefused = 0, deathsAllowed = 0, deathsWithLeader = 0;
const char* DeathDecidedElsewhere(DWORD now, unsigned& slot) {
    slot = leader;
    if (OwnScenes()) return nullptr;
    // The leader we follow failed his prompt: his death is ours too. 02.11.: the client mashed,
    // passed its own copy, refused the host's failure as "a partner's replayed message" and lived on alone.
    if (gate::mirroring > 0 && following && !valveOpen && leader < coop_peers::kMaxPeers && gate::mirroringFrom == leader) {
        InterlockedIncrement(&deathsWithLeader);
        return nullptr;
    }
    if (gate::replaying > 0 || gate::mirroring > 0) return "it came with a partner's replayed message";
    if (following && !valveOpen && leader < coop_peers::kMaxPeers) return "we follow the partner's scene";
    if (!running && endedFollowing && endedLeader < coop_peers::kMaxPeers && endedAt && now - endedAt < kDeathAuthorityMs &&
        peerTable.peers[endedLeader].used) {
        slot = endedLeader;
        return "right after the partner's scene we followed";
    }
    return nullptr;
}
volatile LONG botScriptedDeathsPassed = 0;
unsigned char __fastcall OnStartSequence(void* target, void*, uint32_t sequence, unsigned char flag) {
    uint32_t attributes = 0, flags = 0;
    const auto player = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target)) - kSequenceTarget;
    // The bot never plays a scripted death (a grab, a failed struggle): it would end in a level restart.
    // While bots are mortal (Versus, a necro player in the game) a necro's execution may kill one;
    // OnDeathSm in runtime_coop_bot.inc still swallows the level-wide death messages.
    if (IsBotEntity(player) && sequence && Read(sequence + 0x0C, &attributes, 4) && attributes &&
        Read(attributes + kSequenceSettings, &flags, 4) && (flags & kScriptedDeathFlag)) {
        if (!bot::MortalNow()) {
            bot::NoteDeathRefused();
            return 1;
        }
        if (InterlockedIncrement(&botScriptedDeathsPassed) == 1)
            gate::Event("test bot_scripted_death INFO: bot %08X's scripted death runs, the bots are mortal now", player);
    }
    if (leaderDecidesDeaths && sequence && peerTable.Count() && player == LocalCampaignPlayer() && Read(sequence + 0x0C, &attributes, 4) &&
        attributes && Read(attributes + kSequenceSettings, &flags, 4) && (flags & kScriptedDeathFlag)) {
        const DWORD now = GetTickCount();
        unsigned slot = 0xFF;
        uint32_t guid[4]{};
        Read(sequence + 0x24, guid, 16);
        if (const char* why = DeathDecidedElsewhere(now, slot)) {
            if (InterlockedIncrement(&deathsRefused) <= 40) {
                char who[24] = "a partner decides";   // a replayed message carries no slot (leader 0xFE/0xFF)
                if (slot < coop_peers::kMaxPeers) std::snprintf(who, sizeof(who), "partner %u decides", slot);
                gate::Event("coop_cutscene scripted death %08X-%08X (flags %X) refused on our Isaac: %s (%s)", guid[0], guid[1], flags,
                            why, who);
            }
            return 1;
        }
        if (InterlockedIncrement(&deathsAllowed) <= 40)
            gate::Event("coop_cutscene scripted death %08X-%08X (flags %X) is ours: %s", guid[0], guid[1], flags,
                        gate::mirroring > 0 ? "the leader we follow failed, we fail with him" : "no partner's scene decides for us");
    }
    return startSequenceHook.thiscall<unsigned char>(target, sequence, flag);
}

bool __fastcall OnShouldProcessDamage(void* self, void*, const void* damage) {
    const auto selfEntity = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    if (bot::RefuseHitOnHuman(selfEntity, damage, "the player's damage gate")) return false;
    if (IsBotEntity(selfEntity) && !bot::TakesDamage(selfEntity, damage)) return false;
    if (followerInvincible && (Following() || necro::WatchBlocks()) && self && static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)) == LocalCampaignPlayer()) {
        InterlockedIncrement(&damageBlocked);
        return false;
    }
    return damageHook.thiscall<bool>(self, damage);
}
// The leader's controller for everybody in his scene. The QTEs (TestInputEvent
// prompts, InputContextSystem, paired struggles, swarm shake-off) all ask the
// ControllerManager, which reads the raw pad state of the player's controller
// slot each frame: [0x0201C0A8] + 0x2C + slot * 0x44 (0x00780180), slot =
// Player+0x41C (15 on PC, measured live 23.09.). The update filter this
// module hooks is called first thing in ControllerManager::UpdateAll
// (0x0077E7E0), after the device manager has filled the slots and before they
// are read. So there the leader records his slot and a follower writes the
// leader's state into his own: the game derives pressed / just pressed from
// it as if the follower had pressed, and every machine passes or fails the
// same prompt. Changes are queued and played back one per frame, so a mash
// faster than the network tick is not lost.
namespace leadinput {
bool enabled = false;
constexpr uint32_t kMagic = 0x54504E49;   // 'INPT'
constexpr uint32_t kVersion = 1;
constexpr uint32_t kDeviceManager = 0x1C1C0A8, kSlotBase = 0x2C, kSlotSize = 0x44, kControllerOffset = 0x41C;
constexpr unsigned kPerPacket = 3, kQueue = 128, kCatchUpAfter = 4, kStickBytes = 0x10;
constexpr DWORD kHeartbeatMs = 100, kStaleMs = 1500, kLeaderTailMs = 3000;
struct Packet {
    uint32_t magic, version, token, firstSequence, count;
    unsigned char states[kPerPacket][kSlotSize];
};
static_assert(sizeof(Packet) == 224, "Unexpected input wire layout");
struct State { uint32_t sequence; unsigned char bytes[kSlotSize]; };
// Leader side: the last kPerPacket changes, resent with every packet.
State sentRing[kPerPacket]{};
uint32_t sentSequence = 0;
unsigned retainedStates = 0;
unsigned char lastCaptured[kSlotSize]{};
bool haveCaptured = false;
DWORD lastSendAt = 0, leadingUntil = 0;
uint16_t buttonsSeen = 0;   // every button the leader held during his scene, for the log
LONG capturedAtLeadStart = 0;   // captured when this lead began, for the per-scene count
// Follower side.
State queue[kQueue]{};
unsigned queueHead = 0, queueCount = 0;
uint32_t queuedUpTo = 0;
unsigned queueAuthor = 0xFF;
uint32_t queueToken = 0;
volatile LONG leaderRestarts = 0;
unsigned char applied[kSlotSize]{};
bool haveApplied = false, injecting = false;
DWORD lastPacketAt = 0;
std::mutex queueLock;   // packets arrive on the network poll, states leave on the game thread
volatile LONG captured = 0, packetsSent = 0, packetsReceived = 0, statesQueued = 0, statesApplied = 0, heldFrames = 0,
              queueOverflow = 0, staleStops = 0, wrongAuthor = 0, slotFaults = 0, statesSkipped = 0, ownKeysDropped = 0;

uint32_t SlotAddress() {
    const uint32_t player = inLevel ? LocalCampaignPlayer() : 0;
    uint32_t manager = 0, controller = ~0u;
    if (!player || !Read(player + kControllerOffset, &controller, 4) || controller >= 16) return 0;
    if (!Read(base + kDeviceManager, &manager, 4) || !manager) return 0;
    return manager + kSlotBase + controller * kSlotSize;
}
void Send() {
    Packet p{};
    p.magic = kMagic; p.version = kVersion; p.token = peerReadiness.token;
    const unsigned n = retainedStates;
    p.firstSequence = sentSequence - n + 1;
    p.count = n;
    for (unsigned i = 0; i < n; ++i) std::memcpy(p.states[i], sentRing[(p.firstSequence + i) % kPerPacket].bytes, kSlotSize);
    if (SendToPeers(&p, sizeof(p))) InterlockedIncrement(&packetsSent);
}
// Leader: record the local slot when it changed, send on change and as heartbeat.
void Capture(DWORD now) {
    const uint32_t slot = SlotAddress();
    unsigned char bytes[kSlotSize]{};
    if (!slot || !Read(slot, bytes, kSlotSize)) { InterlockedIncrement(&slotFaults); return; }
    if (!haveCaptured || std::memcmp(bytes, lastCaptured, kSlotSize) != 0) {
        std::memcpy(lastCaptured, bytes, kSlotSize);
        haveCaptured = true;
        uint16_t buttons = 0;
        std::memcpy(&buttons, bytes + 0x10, 2);
        buttonsSeen |= buttons;
        ++sentSequence;
        if (retainedStates < kPerPacket) ++retainedStates;
        State& s = sentRing[sentSequence % kPerPacket];
        s.sequence = sentSequence;
        std::memcpy(s.bytes, bytes, kSlotSize);
        InterlockedIncrement(&captured);
        Send();
        lastSendAt = now;
    } else if (now - lastSendAt >= kHeartbeatMs) {
        Send();
        lastSendAt = now;
    }
}
// Two leaders of one scene. 25.09., Tiedemann: both machines led parts of the
// same fight ("the next part of our scene, we still lead" on the client, "ours,
// we lead" on the host); the client counted its own QTE results, refused the
// host's successful ones and failed. The partner's leader input arriving while
// this machine leads and he is in a scene is that conflict: the host keeps the lead, a
// client gives it up to him (between two clients the lower token keeps it).
volatile LONG leadYields = 0;
bool YieldTo(unsigned slot, DWORD now) {
    if (slot >= coop_peers::kMaxPeers || !running || following || IsHost() || OwnScenes()) return false;
    if (!PartnerInScene(slot, now)) return false;   // his leftover heartbeats after his scene: not a conflict
    const auto& peer = peerTable.peers[slot];
    if (!peer.used || !peer.token) return false;
    const bool hostPeer = peer.machine == 0;   // the clients are meshed: only machine 0 is the host
    return hostPeer || peer.token < peerReadiness.token;
}
void OnPacket(const Packet& p, unsigned slot) {
    if (!enabled || p.count > kPerPacket) return;
    InterlockedIncrement(&packetsReceived);
    // The partner being followed, or whose scene reference arrived (his cue and
    // the local sequence may start a frame apart; his first states must not be lost).
    const DWORD now = GetTickCount();
    if (leader == 0xFEu && YieldTo(slot, now)) {
        following = true;
        leader = slot;
        streams::SetReference(slot, now);
        InterlockedIncrement(&leadYields);
        gate::Event("coop_cutscene both of us led this scene: partner %u keeps the lead, we follow from here", slot);
    }
    const unsigned expect = leader < coop_peers::kMaxPeers ? leader
        : (streams::referenceUntil && now < streams::referenceUntil ? streams::referenceSlot : 0xFFu);
    if (slot != expect) { InterlockedIncrement(&wrongAuthor); return; }
    std::lock_guard<std::mutex> guard(queueLock);
    // A leader whose game restarted comes back with a new token and counts from
    // one again: his states would all be older than queuedUpTo.
    if (queueAuthor != slot || queueToken != p.token) {
        if (queueAuthor == slot) InterlockedIncrement(&leaderRestarts);
        queueAuthor = slot; queueToken = p.token; queueCount = 0; queuedUpTo = p.firstSequence ? p.firstSequence - 1 : 0;
    }
    lastPacketAt = GetTickCount();
    for (unsigned i = 0; i < p.count; ++i) {
        const uint32_t sequence = p.firstSequence + i;
        if (sequence <= queuedUpTo) continue;
        if (queueCount == kQueue) { queueHead = (queueHead + 1) % kQueue; --queueCount; InterlockedIncrement(&queueOverflow); }
        State& s = queue[(queueHead + queueCount) % kQueue];
        s.sequence = sequence;
        std::memcpy(s.bytes, p.states[i], kSlotSize);
        ++queueCount;
        queuedUpTo = sequence;
        InterlockedIncrement(&statesQueued);
    }
}
// Follower: one queued state per frame, else the last one again.
void Inject(DWORD now) {
    std::lock_guard<std::mutex> guard(queueLock);
    if (!lastPacketAt || now - lastPacketAt > netclock::Window(kStaleMs)) {   // a slow link is not a stopped leader
        if (injecting) { injecting = false; InterlockedIncrement(&staleStops); gate::Event("coop_cutscene leader input stale, follower input blocked again"); }
        return;
    }
    // A follower behind the leader (a slower frame rate, a burst of packets) catches up on states
    // that differ only in the sticks and the mouse; every button change still gets its own frame.
    // 02.11.: overflow=154 on the client, and a mash lost its presses with the dropped states.
    while (queueCount > kCatchUpAfter &&
           std::memcmp(queue[queueHead].bytes + kStickBytes, queue[(queueHead + 1) % kQueue].bytes + kStickBytes, kSlotSize - kStickBytes) == 0) {
        queueHead = (queueHead + 1) % kQueue;
        --queueCount;
        InterlockedIncrement(&statesSkipped);
    }
    if (queueCount) {
        std::memcpy(applied, queue[queueHead].bytes, kSlotSize);
        queueHead = (queueHead + 1) % kQueue;
        --queueCount;
        haveApplied = true;
        InterlockedIncrement(&statesApplied);
    } else {
        InterlockedIncrement(&heldFrames);
    }
    if (!haveApplied) return;
    const uint32_t slot = SlotAddress();
    if (!slot || !WriteBytes(slot, applied, kSlotSize)) { InterlockedIncrement(&slotFaults); return; }
    if (!injecting) { injecting = true; gate::Event("coop_cutscene follower plays with the leader's controller (partner %u)", leader); }
}
void Reset() {
    std::lock_guard<std::mutex> guard(queueLock);
    haveCaptured = false; retainedStates = 0; haveApplied = false; injecting = false;
    queueCount = 0; queueHead = 0; queueAuthor = 0xFF; queueToken = 0; lastPacketAt = 0;
}
// From the update filter, game thread, once per ControllerManager update.
void Tick() {
    if (!enabled || !installed || !leaderOnly) return;
    const DWORD now = GetTickCount();
    if (OwnScenes()) {
        if (leadingUntil) { leadingUntil = 0; buttonsSeen = 0; }
        if (injecting || haveCaptured || queueCount) Reset();
        return;
    }
    // The leader keeps recording a moment past each sequence: a scene is a
    // chain of them, and a prompt can fall between two.
    if (running && !following && peerTable.Count()) {
        if (!leadingUntil) capturedAtLeadStart = InterlockedCompareExchange(&captured, 0, 0);
        leadingUntil = now + kLeaderTailMs;
    }
    if (leadingUntil && !following && static_cast<LONG>(leadingUntil - now) > 0) { Capture(now); return; }
    if (leadingUntil) {
        leadingUntil = 0;
        gate::Event("coop_cutscene leader input: scene over, %ld state change(s) sent, buttons held %04X",
                    InterlockedCompareExchange(&captured, 0, 0) - capturedAtLeadStart, buttonsSeen);
        buttonsSeen = 0;
        retainedStates = 0;
        haveCaptured = false;
    }
    if (running && following && !valveOpen && leader < coop_peers::kMaxPeers) { Inject(now); return; }
    if (injecting || haveCaptured || queueCount) Reset();
}
}  // namespace leadinput
void OnInputPacket(const leadinput::Packet& p, unsigned slot) { leadinput::OnPacket(p, slot); }
// Free aim inside a scene. The ch11 Tiedemann fight (StationaryShootingMarker
// 33CB5616-B8A33CCF) lets the player aim the javelin gun with the mouse; the
// streamed pad state only carries the mouse deltas, which each machine adds
// per frame at its own frame rate, so the follower aimed somewhere else
// (24.09.). PlayerStationaryShootingSM (vtable 0x01B778CC, ctor 0x010B7950,
// 0xCC bytes: +0x50 the player, +0x61 aiming, +0x64 pitch, +0x68 yaw) adds
// the input, clamps to the marker's limits, stores pitch and at 0x010B7FDB
// still holds the clamped yaw in xmm0 before storing and using both. A mid
// hook there sends the leader's angles and replaces the follower's.
namespace leadaim {
bool enabled = false;
constexpr uint32_t kMagic = 0x534D4941;   // 'AIMS'
constexpr uint32_t kVersion = 1;
constexpr uint32_t kSiteRva = 0xCB7FDB, kVtableRva = 0x17778CC, kOwner = 0x50, kPitch = 0x64;
constexpr DWORD kSendMs = 33, kFreshMs = 300;
struct Packet { uint32_t magic, version, token, sequence; float pitch, yaw; };
static_assert(sizeof(Packet) == 24, "Unexpected aim wire layout");
safetyhook::MidHook hook;
uint32_t sentSequence = 0, lastSequence = 0;
DWORD lastSendAt = 0, leaderAt = 0;
unsigned leaderSlot = 0xFF;
float leaderPitch = 0.0f, leaderYaw = 0.0f;
std::mutex lock;
volatile LONG reached = 0, notOurs = 0, frames = 0, sent = 0, received = 0, applied = 0, stale = 0, wrongAuthor = 0;
bool Plausible(float v) { return v == v && v > -360.0f && v < 360.0f; }
void OnAim(SafetyHookContext& ctx) {
    const uint32_t sm = static_cast<uint32_t>(ctx.edi);
    uint32_t vtable = 0, owner = 0;
    if (!enabled) return;
    InterlockedIncrement(&reached);
    if (!Read(sm, &vtable, 4) || vtable != base + kVtableRva || !Read(sm + kOwner, &owner, 4) ||
        !owner || owner != LocalCampaignPlayer()) { InterlockedIncrement(&notOurs); return; }
    InterlockedIncrement(&frames);
    const DWORD now = GetTickCount();
    stationaryAt = now;
    if (running && !following && peerTable.Count()) {
        if (now - lastSendAt < kSendMs) return;
        float pitch = 0.0f;
        Read(sm + kPitch, &pitch, 4);
        Packet p{kMagic, kVersion, peerReadiness.token, ++sentSequence, pitch, ctx.xmm0.f32[0]};
        if (SendToPeers(&p, sizeof(p))) InterlockedIncrement(&sent);
        lastSendAt = now;
        return;
    }
    if (!running || !following || valveOpen || leader >= coop_peers::kMaxPeers) return;
    std::lock_guard<std::mutex> guard(lock);
    if (leaderSlot != leader || !leaderAt || now - leaderAt > netclock::Window(kFreshMs, 1000)) { InterlockedIncrement(&stale); return; }
    ctx.xmm0.f32[0] = leaderYaw;
    WriteBytes(sm + kPitch, &leaderPitch, 4);
    InterlockedIncrement(&applied);
}
void OnPacket(const Packet& p, unsigned slot) {
    if (!enabled || !Plausible(p.pitch) || !Plausible(p.yaw)) return;
    InterlockedIncrement(&received);
    const DWORD now = GetTickCount();
    const unsigned expect = leader < coop_peers::kMaxPeers ? leader
        : (streams::referenceUntil && now < streams::referenceUntil ? streams::referenceSlot : 0xFFu);
    if (slot != expect) { InterlockedIncrement(&wrongAuthor); return; }
    std::lock_guard<std::mutex> guard(lock);
    if (slot == leaderSlot && p.sequence <= lastSequence && now - leaderAt < 2000) return;   // late or repeated
    leaderSlot = slot; lastSequence = p.sequence; leaderAt = now;
    leaderPitch = p.pitch; leaderYaw = p.yaw;
}
bool Install(FILE* log) {
    // mov ecx,[edi+0x50] / fld dword [0x01B72490]
    static const unsigned char kSite[] = {0x8B, 0x4F, 0x50, 0xD9, 0x05, 0x90, 0x24, 0xB7, 0x01};
    unsigned char actual[sizeof(kSite)]{};
    if (!Read(base + kSiteRva, actual, sizeof(actual)) || std::memcmp(actual, kSite, sizeof(kSite)) != 0) {
        std::fprintf(log, "coop_cutscene leader_aim site mismatch at %08X; aim stays per machine\n", static_cast<unsigned>(base + kSiteRva));
        return false;
    }
    hook = safetyhook::create_mid(reinterpret_cast<void*>(base + kSiteRva), OnAim);
    std::fprintf(log, "coop_cutscene leader_aim %s at %08X (stationary shooting takes the leader's angles)\n", hook ? "installed" : "FAILED",
                 static_cast<unsigned>(base + kSiteRva));
    return static_cast<bool>(hook);
}
}  // namespace leadaim
void OnAimPacket(const leadaim::Packet& p, unsigned slot) { leadaim::OnPacket(p, slot); }

namespace sceneaim {
constexpr uint32_t kMagic = 0x31415343; // CSA1
struct Packet { uint32_t magic, token, root, attempt, generation, sequence; float pitch, yaw; };
safetyhook::MidHook hook;
Packet sample{};
DWORD sampleAt = 0, sentAt = 0;
uint32_t sequence = 0;
unsigned appliedLines = 0, sentLines = 0;
bool Matches(const Packet& p) {
    return p.root && p.root == mirror::scenechain::root && p.attempt && p.attempt == mirror::scenechain::attempt &&
           p.token == mirror::scenechain::owner && p.generation == loadsync::SceneGeneration();
}
bool Receive(const void* data, unsigned slot, DWORD now) {
    Packet p{};
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != kMagic || !KnownPeerToken(p.token) || (IsHost() && peerTable.peers[slot].token != p.token) ||
        !p.root || !p.attempt || !leadaim::Plausible(p.pitch) || !leadaim::Plausible(p.yaw)) return false;
    if (!hook || !Matches(p) || OwnScenes() || !FollowsSlot(PeerSlot(peerTable.FindByToken(p.token)))) return true;
    if (sampleAt && Matches(sample) && static_cast<int32_t>(p.sequence - sample.sequence) <= 0) return true;
    sample = p; sampleAt = Stamp(now);
    return true;
}
void OnAngles(SafetyHookContext& ctx) {
    if (!running || !mirror::scenechain::attempt || OwnScenes() || !peerTable.Count()) return;
    uint32_t adc = 0, player = LocalCampaignPlayer();
    if (!player || !Read(player + 0x6A0, &adc, 4) || !adc || adc != ctx.ebx || adc + 0x20 != ctx.esi) return;
    float angles[2]{};
    if (!Read(ctx.esi, angles, sizeof(angles)) || !leadaim::Plausible(angles[0]) || !leadaim::Plausible(angles[1])) return;
    const DWORD now = GetTickCount();
    if (Leading() && mirror::scenechain::owner == peerReadiness.token && now - sentAt >= leadaim::kSendMs) {
        Packet p{kMagic, peerReadiness.token, mirror::scenechain::root, mirror::scenechain::attempt, loadsync::SceneGeneration(), ++sequence, angles[0], angles[1]};
        const int sent = SendToPeers(&p, sizeof(p)); sentAt = now;
        if (sentLines++ < 8) gate::Event("scene aim: shared angles root=%08X attempt=%u sent=%d", p.root, p.attempt, sent);
    } else if (Following() && !valveOpen && sampleAt && Matches(sample) && now - sampleAt <= netclock::Window(leadaim::kFreshMs, 1000)) {
        // Relative mode rebuilds both orientation matrices before the game copies the block.
        using Update = void(__thiscall*)(void*, float, float, float, uint32_t);
        reinterpret_cast<Update>(base + 0xB4E260)(reinterpret_cast<void*>(ctx.esi), sample.pitch - angles[0], sample.yaw - angles[1], 0, 0);
        if (appliedLines++ < 8) gate::Event("scene aim: took leader angles root=%08X attempt=%u", sample.root, sample.attempt);
    }
}
void Install(FILE* log) {
    const unsigned char copy[]{0x8D,0xBB,0xC0,0,0,0,0xB9,0x28,0,0,0,0xF3,0xA5};
    const unsigned char update[]{0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,0x54,0xF3,0x0F,0x10,0x45,0x08};
    unsigned char first[sizeof(copy)], second[sizeof(update)];
    if (Read(base + 0xCAE609, first, sizeof(first)) && !std::memcmp(copy, first, sizeof(first)) &&
        Read(base + 0xB4E260, second, sizeof(second)) && !std::memcmp(update, second, sizeof(second)))
        hook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0xCAE609), OnAngles);
    std::fprintf(log, "coop_cutscene special shooting aim hook=%d at %08X\n", hook ? 1 : 0, static_cast<unsigned>(base + 0xCAE609));
}
}

void OnWorldReset() {
    if (!installed) return;
    if (running) End(GetTickCount(), inLevel);
    endedFollowing = endedWithPartner = false;
    endedLeader = leader = replayAuthor = 0xFF;
    localPlayer = sequenceNow = 0;
    endedAt = startedAt = stationaryAt = stationaryFrom = endPendingSince = 0;
    replayStartAt = directReplayAt = ledSince = ledEndedAt = leaderGoneSince = 0;
    std::fill(std::begin(partnerSceneSince), std::end(partnerSceneSince), 0u);
    streams::referenceUntil = streams::vis::sequenceSeenAt = 0;
    streams::referenceSlot = 0xFF;
    leadinput::leadingUntil = leadinput::buttonsSeen = 0;
    leadinput::Reset();
    leadaim::leaderAt = 0;
    leadaim::leaderSlot = 0xFF;
    sceneaim::sampleAt = 0;
}

bool __cdecl OnInputFilter() {
    bot::WritePad();   // the bot's controller, pad 14, before the ControllerManager reads it
    leadinput::Tick();
    if (blockInput && (Following() || necro::WatchBlocks()) && !leadinput::injecting) { InterlockedIncrement(&inputBlockedFrames); return true; }
    // the curtain after a level start or restart holds the pad too (runtime_coop_streams.inc)
    if (streams::curtain::HoldsInput()) { InterlockedIncrement(&inputBlockedFrames); return true; }
    return filterHook.ccall<bool>();
}

bool Install(FILE* log, const std::string& ini) {
    hidePartners = GetPrivateProfileIntA("Cutscene", "HidePartners", 1, ini.c_str()) != 0;
    leaderOnly = GetPrivateProfileIntA("Cutscene", "LeaderDecides", 1, ini.c_str()) != 0;
    blockInput = GetPrivateProfileIntA("Cutscene", "FollowerInputBlocked", 1, ini.c_str()) != 0;
    followerInvincible = GetPrivateProfileIntA("Cutscene", "FollowerInvincible", 1, ini.c_str()) != 0;
    valveMs = static_cast<DWORD>(GetPrivateProfileIntA("Cutscene", "FollowerValveMs", 90000, ini.c_str()));
    finaleOwnScenes = GetPrivateProfileIntA("Cutscene", "FinaleOwnScenes", 0, ini.c_str()) != 0;
    leaderDecidesDeaths = leaderOnly && GetPrivateProfileIntA("Cutscene", "LeaderDecidesDeaths", 0, ini.c_str()) != 0;
    if (leaderDecidesDeaths || botWanted) {
        // push ebp / mov ebp,esp / and esp,-16 / sub esp,24h / push ebx / push esi / mov esi,ecx / mov eax,[esi+2A8h]
        static const unsigned char kStart[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x24, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0xA8, 0x02, 0x00, 0x00};
        if (!hook_site::Install(log, "coop_cutscene Player::StartScriptedSequence", base + kStartSequenceRva, kStart, sizeof(kStart),
                                reinterpret_cast<void*>(OnStartSequence), startSequenceHook))
            leaderDecidesDeaths = false;
    }
    std::fprintf(log, "coop_cutscene leader_decides_deaths=%d (a scripted death on our Isaac waits for the leader of the scene we follow)\n",
                 leaderDecidesDeaths ? 1 : 0);
    std::fprintf(log, "coop_cutscene finale_own_scenes=%d (in the finale nobody follows, yields, hides partners or streams his controller)\n",
                 finaleOwnScenes ? 1 : 0);
    // push ebx / push esi / push edi / mov edi,[esp+0x10] / mov eax,[edi+0x54]
    static const unsigned char kDamage[] = {0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x8B, 0x47, 0x54};
    // push esi / mov esi,[0x02021378] / test esi,esi
    static const unsigned char kFilter[] = {0x56, 0x8B, 0x35, 0x78, 0x13, 0x02, 0x02, 0x85, 0xF6};
    // [Bot] NoDamageToHumans: on every machine, a partner's too, where only the host's bots' ghosts are.
    const bool humanGuard = GetPrivateProfileIntA("Bot", "NoDamageToHumans", 1, ini.c_str()) != 0;
    bot::SetNoDamageToHumans(humanGuard);
    bool damageHooked = false;
    if ((leaderOnly && followerInvincible) || botWanted || humanGuard) {
        damageHooked = hook_site::Install(log, "coop_cutscene Player::ShouldProcessDamage", base + kShouldProcessDamageRva, kDamage, sizeof(kDamage),
                                          reinterpret_cast<void*>(OnShouldProcessDamage), damageHook);
        if (!damageHooked) {
            followerInvincible = false;
            bot::SetNoDamageToHumans(false);
        }
    }
    std::fprintf(log, "coop_cutscene no_damage_to_humans=%d (no hit whose attacker is a bot, its ghost, its gun or an object it threw reaches a human's Isaac)\n",
                 humanGuard && damageHooked ? 1 : 0);
    leadinput::enabled = GetPrivateProfileIntA("Cutscene", "LeaderInputForAll", 0, ini.c_str()) != 0;
    if (((leaderOnly && (blockInput || leadinput::enabled)) || botWanted) &&
        !hook_site::Install(log, "coop_cutscene controller filter", base + kInputFilterRva, kFilter, sizeof(kFilter),
                            reinterpret_cast<void*>(OnInputFilter), filterHook)) {
        blockInput = false;
        leadinput::enabled = false;
    }
    gate::sceneTraceEnabled = GetPrivateProfileIntA("Cutscene", "SceneTrace", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop_cutscene scene_trace=%d (every message around a scene, with its tick)\n", gate::sceneTraceEnabled ? 1 : 0);
    leadaim::enabled = leadinput::enabled && GetPrivateProfileIntA("Cutscene", "LeaderAimForAll", 0, ini.c_str()) != 0;
    if (leadaim::enabled) {
        sceneaim::Install(log);
        leadaim::enabled = leadaim::Install(log);
    }
    installed = true;
    std::fprintf(log, "coop_cutscene hide_partners=%d leader_decides=%d follower_input_blocked=%d follower_invincible=%d valve_ms=%lu leader_input_for_all=%d\n",
                 hidePartners ? 1 : 0, leaderOnly ? 1 : 0, blockInput ? 1 : 0, followerInvincible ? 1 : 0, static_cast<unsigned long>(valveMs),
                 leadinput::enabled ? 1 : 0);
    return true;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG activity = InterlockedCompareExchange(&starts, 0, 0) + InterlockedCompareExchange(&ends, 0, 0) +
                          InterlockedCompareExchange(&bodiesHidden, 0, 0) + InterlockedCompareExchange(&itemsHidden, 0, 0) +
                          InterlockedCompareExchange(&cuesSuppressed, 0, 0) + InterlockedCompareExchange(&valves, 0, 0) +
                          InterlockedCompareExchange(&faults, 0, 0) + (InterlockedCompareExchange(&damageBlocked, 0, 0) > 0 ? 1 : 0) +
                          InterlockedCompareExchange(&leadinput::captured, 0, 0) + InterlockedCompareExchange(&leadinput::statesApplied, 0, 0) +
                          InterlockedCompareExchange(&leadinput::staleStops, 0, 0) +
                          InterlockedCompareExchange(&leadaim::sent, 0, 0) + InterlockedCompareExchange(&leadaim::applied, 0, 0) +
                          InterlockedCompareExchange(&leadaim::received, 0, 0) + InterlockedCompareExchange(&leadaim::reached, 0, 0);
    if (activity == last) return;
    last = activity;
    std::fprintf(log, "coop_cutscene starts=%ld following=%ld ends=%ld bodies_hidden=%ld items_hidden=%ld shown=%ld show_skipped=%ld "
                      "damage_blocked=%ld input_blocked_frames=%ld cues_suppressed=%ld valves=%ld valves_held=%ld faults=%ld running=%d follower=%d leader=%u "
                      "by_replay=%ld by_partner_scene=%ld by_chain=%ld inherited=%ld lead_kept=%ld followers_ignored=%ld helmets_hidden=%ld\n",
                 InterlockedCompareExchange(&starts, 0, 0), InterlockedCompareExchange(&followerStarts, 0, 0), InterlockedCompareExchange(&ends, 0, 0),
                 InterlockedCompareExchange(&bodiesHidden, 0, 0), InterlockedCompareExchange(&itemsHidden, 0, 0), InterlockedCompareExchange(&shown, 0, 0),
                 InterlockedCompareExchange(&showSkipped, 0, 0), InterlockedCompareExchange(&damageBlocked, 0, 0),
                 InterlockedCompareExchange(&inputBlockedFrames, 0, 0), InterlockedCompareExchange(&cuesSuppressed, 0, 0),
                 InterlockedCompareExchange(&valves, 0, 0), InterlockedCompareExchange(&valvesHeld, 0, 0), InterlockedCompareExchange(&faults, 0, 0), running ? 1 : 0, following ? 1 : 0, leader,
                 InterlockedCompareExchange(&followByReplay, 0, 0), InterlockedCompareExchange(&followByPartnerScene, 0, 0),
                 InterlockedCompareExchange(&followByChain, 0, 0), InterlockedCompareExchange(&followInherited, 0, 0), InterlockedCompareExchange(&leadKept, 0, 0),
                 InterlockedCompareExchange(&followersIgnored, 0, 0), InterlockedCompareExchange(&helmetsHidden, 0, 0));
    std::fprintf(log, "coop_cutscene hidden in scenes: bot ghosts=%ld necro ghosts=%ld | necro watchers hidden=%ld shown=%ld | puppet bodies shown=%ld hidden_again=%ld\n",
                 InterlockedCompareExchange(&botGhostsHidden, 0, 0), InterlockedCompareExchange(&necroGhostsHidden, 0, 0),
                 InterlockedCompareExchange(&watchersHidden, 0, 0), InterlockedCompareExchange(&watchersShown, 0, 0),
                 InterlockedCompareExchange(&puppetBodiesShown, 0, 0), InterlockedCompareExchange(&puppetBodiesHidden, 0, 0));
    std::fprintf(log, "coop_cutscene stationary_resumed=%ld lead_yields=%ld stationary_own_hands=%ld\n", InterlockedCompareExchange(&stationaryResumed, 0, 0),
                 InterlockedCompareExchange(&leadinput::leadYields, 0, 0), InterlockedCompareExchange(&stationaryOwnHands, 0, 0));
    std::fprintf(log, "coop_cutscene stationary_parts=%ld in_stationary=%d\n", InterlockedCompareExchange(&stationaryParts, 0, 0), inStationary ? 1 : 0);
    if (leaderDecidesDeaths)
        std::fprintf(log, "coop_cutscene scripted_deaths refused=%ld ours=%ld with_leader=%ld finale_own_starts=%ld\n", InterlockedCompareExchange(&deathsRefused, 0, 0),
                     InterlockedCompareExchange(&deathsAllowed, 0, 0), InterlockedCompareExchange(&deathsWithLeader, 0, 0),
                     InterlockedCompareExchange(&finaleOwnStarts, 0, 0));
    if (leadaim::enabled)
        std::fprintf(log, "coop_cutscene leader_aim reached=%ld not_ours=%ld frames=%ld sent=%ld received=%ld applied=%ld stale=%ld wrong_author=%ld\n",
                     InterlockedCompareExchange(&leadaim::reached, 0, 0), InterlockedCompareExchange(&leadaim::notOurs, 0, 0),
                     InterlockedCompareExchange(&leadaim::frames, 0, 0), InterlockedCompareExchange(&leadaim::sent, 0, 0),
                     InterlockedCompareExchange(&leadaim::received, 0, 0), InterlockedCompareExchange(&leadaim::applied, 0, 0),
                     InterlockedCompareExchange(&leadaim::stale, 0, 0), InterlockedCompareExchange(&leadaim::wrongAuthor, 0, 0));
    if (!leadinput::enabled) return;
    std::fprintf(log, "coop_cutscene leader_input captured=%ld sent=%ld received=%ld wrong_author=%ld queued=%ld applied=%ld held_frames=%ld "
                      "overflow=%ld caught_up=%ld own_keys_dropped=%ld stale_stops=%ld slot_faults=%ld leader_restarts=%ld injecting=%d\n",
                 InterlockedCompareExchange(&leadinput::captured, 0, 0), InterlockedCompareExchange(&leadinput::packetsSent, 0, 0),
                 InterlockedCompareExchange(&leadinput::packetsReceived, 0, 0), InterlockedCompareExchange(&leadinput::wrongAuthor, 0, 0),
                 InterlockedCompareExchange(&leadinput::statesQueued, 0, 0), InterlockedCompareExchange(&leadinput::statesApplied, 0, 0),
                 InterlockedCompareExchange(&leadinput::heldFrames, 0, 0), InterlockedCompareExchange(&leadinput::queueOverflow, 0, 0),
                 InterlockedCompareExchange(&leadinput::statesSkipped, 0, 0), InterlockedCompareExchange(&leadinput::ownKeysDropped, 0, 0),
                 InterlockedCompareExchange(&leadinput::staleStops, 0, 0), InterlockedCompareExchange(&leadinput::slotFaults, 0, 0),
                 InterlockedCompareExchange(&leadinput::leaderRestarts, 0, 0),
                 leadinput::injecting ? 1 : 0);
}
}  // namespace cutscene
