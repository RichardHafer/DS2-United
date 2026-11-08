// Dead Space 2 United by Richard Hafer.
// World calls: state the host's world changes by C++ call, not by message.
//
// The vent. A necromorph leaving a vent does not send the VentMarker
// anything: DeadSpaceNPCExitVentSM::HandleCCTSetVentOpen calls
// VentMarker::SetCoverState(bool present, int) on it (Xbox 0x00944948; PC
// 0x012F5AB0, right after IsCoverPresent 0x012F5A70, both after the ctor
// 0x012F5850 as in the PDB order). SetCoverState flips bit 1 of +0x2F0,
// tells itself by message (vtable+0x150, hash 0x38B44447 for "on" /
// 0x7FD2A81B for "off") and starts the cover's animation (0x012E0DA0). None
// of that reaches a client: its necromorphs are ghosts that walk, not state
// machines that open vents (17:20: vents stayed intact on the clients).
//
// So the call is hooked on the host and sent through the mirror as a world
// call (kFlagCall): the vent's GUID, call id 1, the two arguments. The client
// finds its VentMarker by GUID and makes the same call through the
// trampoline. The journal keeps the latest state per vent for late joiners.
//
// The same shape serves any other host-side setter found later: add an id,
// a hook, and a case in ReplayCall.
//
// Doors (19:50). A StandardDoor is a state machine: +0x234 the state, +0x238
// the previous one (0 closed, 2 a player at the panel, 4 opening, 5 open,
// 6 closing), and every change goes through TransitionToState (PC
// 0x00DD2950, `thiscall(state)`, ret 4, twelve call sites; it marks the
// object dirty for TNL, stores the states, sends the door's message for the
// state (+0x1A8: 7FD2A81B opening, F8DAA2B8 open, 38B44447 closing) and, for
// a streaming door, activates its leave set). Measured live with
// tools/live_probe_doors.py across three doors. The
// door opens for the LOCAL player in its field only; a partner's ghost walks
// through it shut. So "opening" (4) is a world call from ANY machine: the
// partner's copy of the door, found by GUID, gets the same transition unless
// it is already opening or open; it then closes itself when nobody is in
// its field, as it always did.
namespace arena { bool RefusesClose(uint32_t door, uint32_t wanted); }   // runtime_coop_arena.inc
namespace worldcalls {
bool enabled = false, installed = false, doorsEnabled = true;
constexpr uint32_t kSetCoverStateRva = 0xEF5AB0;
constexpr uint32_t kVentMarkerVtableRva = 0x178C5F0;
constexpr uint32_t kDoorTransitionRva = 0x9D2950;
constexpr uint32_t kDoorState = 0x234;
constexpr uint32_t kDoorOpening = 4, kDoorOpen = 5;
constexpr uint32_t kCallSetCoverState = 1, kCallDoorTransition = 2;
safetyhook::InlineHook setCoverHook, doorHook;
volatile LONG coverCalls = 0, coverMirrored = 0, coverReplayed = 0, wrongClass = 0, faults = 0;
volatile LONG doorCalls = 0, doorMirrored = 0, doorReplayed = 0, doorSkipped = 0, doorHeldOpen = 0;
bool doorsStayOpen = true;
float doorReach = 30.0f;
int Filter(unsigned);
constexpr uint32_t kDoorClosing = 6;
// A partner's opening is repeated the way the local Isaac would open the door.
// 25.09., ch11 marker test bed: the client walked up, the host's copy was put
// straight into "opening" (TransitionToState(4) from 0) and opened on a room
// that was never loaded (mtb_room_lower missing on the host, black behind the
// door). The load is part of the native path: StandardDoor::TryToActivateDoor
// (PC 0x00DDA380, thiscall(actor), ret 4) notes the actor's side
// (+0x259 = EntityOnFrontSideOfDoor) and goes to state 1; the tick then loads
// that side's open set in state 2 (0x00DD24B0, StreamSet::Activate, waits for
// iMsgStreamSetLoadComplete) and only then opens. So a closed StandardDoor is
// activated with the local Isaac as the actor; a lock or anything else that
// keeps it shut falls back to the direct opening.
constexpr uint32_t kStandardDoorVtableRva = 0x174D5D8, kTryActivateDoorRva = 0x9DA380;
constexpr uint32_t kDoorClosed = 0, kDoorUsed = 1, kDoorLoading = 2;
volatile LONG doorNative = 0;
constexpr DWORD kHoldOpenCapMs = 300000;
// A closed door shows its lock hologram ("UNLOCKED"/"LOCKED", front and back).
// The door itself takes it down on the way CLOSED -> ACCESS_GRANTED (1): the
// tick (0x00DD23BF) calls StandardDoor::StopDoorAmbientVFX (0x00DAE520,
// thiscall, releases the handles at +0x264..) and plays its own. An opening
// started straight from CLOSED (0) or WAITING (3) skips that state, and the
// hologram stayed hanging in the open doorway (25.09., screenshot). So it is
// taken down first, the way ACCESS_GRANTED would have.
constexpr uint32_t kStopAmbientVfxRva = 0x9AE520;
volatile LONG hologramsCleared = 0;
void ClearLockHologram(uint32_t door) {
    uint32_t state = 0;
    if (!door || !Read(door + kDoorState, &state, 4) || (state != 0 && state != 3)) return;
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(base + kStopAmbientVfxRva)(reinterpret_cast<void*>(door));
        InterlockedIncrement(&hologramsCleared);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}

// A door between the players stays open (the companion bots count as players
// here too: a bot left behind a door keeps it open). While one of them is near a
// door and the players are on different sides of it, the door's own wish to close
// (state 6, asked again every tick while it is open, 0x00DD9C7D) is refused; once everybody
// is on one side it closes as it always did. The side is the sign of the
// player's offset along the door's forward (+0xB0) from its position (+0xC0);
// a player standing in the doorway counts with whichever side he leans to.
// Only one player has to be within DoorReach; the others count at any
// distance, so a partner far down the corridor still keeps the door open, up
// to kHoldOpenCapMs. Intended: a door closing between the players is what
// strands one of them in an unloaded room.
// Necromorphs never hold a door (17.10.: the necro client counted its own necro and kept a
// door open that the host had shut). As in MP they use the vents, and every machine must
// decide from the same bodies: humans and bots only. A dead human watching a partner
// (role 4) has no body here either.
struct OpenHold { uint32_t door; DWORD since; };
OpenHold openHolds[8]{};
volatile LONG necroHoldsSkipped = 0;
bool PlayersOnBothSides(uint32_t door) {
    float pos[3]{}, fwd[3]{};
    if (!Read(door + placement::kPlayerPosition, pos, 12) || !Read(door + placement::kPlayerForward, fwd, 12)) return false;
    const DWORD now = GetTickCount();
    int positive = 0, negative = 0;
    bool anyNear = false;
    auto consider = [&](const float* p) {
        const float d[3] = {p[0] - pos[0], p[1] - pos[1], p[2] - pos[2]};
        const float dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (dist2 <= doorReach * doorReach) anyNear = true;
        const float along = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        if (along >= 0.0f) ++positive; else ++negative;
    };
    const int localRole = spectate::Active() ? 4 : necro::LocalRole();
    if (localRole) {
        InterlockedIncrement(&necroHoldsSkipped);
    } else {
        float me[3]{};
        if (!gate::LocalPosition(me)) return false;
        consider(me);
    }
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        if (!peer.used || !peer.token || now - peer.lastSeen > 10000) continue;
        const auto& runtime = peerRuntime[s];
        if (!runtime.lastState || now - runtime.lastState > 5000) continue;
        if (roles::IsNecro(s, now)) continue;   // any necro role, or a spectator: no human body
        consider(runtime.pos);
    }
    for (uint32_t bot : botEntities) {
        float at[3]{};
        if (bot && Read(bot + placement::kPlayerPosition, at, 12)) consider(at);
    }
    return anyNear && positive && negative;
}
bool KeepOpen(uint32_t door) {
    if (!doorsStayOpen || (!peerTable.Count() && !AnyBot())) return false;
    const DWORD now = GetTickCount();
    OpenHold* mine = nullptr;
    for (auto& h : openHolds) if (h.door == door) { mine = &h; break; }
    if (!PlayersOnBothSides(door)) { if (mine) *mine = OpenHold{}; return false; }
    if (!mine) {
        for (auto& h : openHolds) if (!h.door) { mine = &h; break; }
        if (!mine) mine = &openHolds[0];
        *mine = OpenHold{door, now};
        if (gate::trace) gate::Event("coop_worldcalls door %08X kept open: players on both sides", door);
    }
    if (now - mine->since > kHoldOpenCapMs) return false;   // the valve
    return true;
}

// 22:40: "the first door opened without the partner". The Dead
// Space 3 door opens when everybody stands at it. Until 22:30 that came,
// by accident, from the gate holding the stream set's load-complete, which
// also left the host 95 s at an open door. So it is measured now: a door's
// wish to OPEN (state 4) from the local world waits while a partner is farther
// than DoorWaitReach from the door, retried every quarter second, let
// through after DoorWaitMs regardless, and mirrored on release like any
// opening. One door waits at a time; a newer wish replaces the older one. A
// partner whose position is unknown is not waited for.
bool doorsWait = true;
float doorWaitReach = 5.0f;
DWORD doorWaitMs = 180000;
struct DoorWait { uint32_t door; uint32_t state; DWORD since, lastTry, lastVote; bool noticed; };
DoorWait doorWait{};
volatile LONG doorWaits = 0, doorWaitReleased = 0, doorWaitTimedOut = 0, doorWaitReplaced = 0;

// 23:00: distance is not enough: some checkpoints spawn the group
// right in front of a door, and 5 m is 5 m. So the door takes VOTES: the local
// world asking a door to open (the local Isaac reached it) is this machine's vote for that
// door's GUID, sent to everybody as a world call (id 3, never journaled);
// the door opens once every partner has voted for the same GUID, and both
// sides release the moment the last vote arrives. Votes expire after
// DoorVoteTtlMs and are wiped by a checkpoint restart, so a door voted for
// before the restart is not open after it. DoorsVote=0 falls back to the
// distance rule.
constexpr uint32_t kCallDoorVote = 3;

// A door remembers which side its user stood on: +0x259, written when the
// door is USED (0x00DDA380: state 0 -> 1, `+0x259 = EntityOnFrontSideOfDoor(actor)`,
// PC 0x00DAE110, `thiscall(actor)` ret 4, the door's own plane test). The
// opening loads the open set of that side (+0xC0/+0xE0, 0x00DD24B0); the
// leave set after closing is chosen in streams::DoorActivation.
constexpr uint32_t kDoorSideRva = 0x9AE110;
constexpr uint32_t kDoorSideByte = 0x259;
bool doorSideFix = true, doorSideOk = false;
volatile LONG doorSidesChecked = 0;
bool DoorSide(uint32_t door, uint32_t actor, bool& out) {
    using Side = bool(__thiscall*)(void*, uint32_t);
    __try {
        out = reinterpret_cast<Side>(base + kDoorSideRva)(reinterpret_cast<void*>(door), actor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The byte is only logged. The leave set a shut door loads is chosen in
// streams::DoorActivation, from the activation itself (the door is its actor):
// +0xC0/+0xE0 are the door's OPEN sets, the leave sets are +0xD0/+0xF0.
constexpr uint32_t kDoorUser = 0x260;
void LogDoorSide(uint32_t door, const char* why) {
    if (!doorSideOk || !gate::trace) return;
    const uint32_t player = LocalCampaignPlayer();
    unsigned char stored = 0;
    uint32_t user = 0;
    bool side = false;
    if (!player || !Read(door + kDoorSideByte, &stored, 1) || !Read(door + kDoorUser, &user, 4) || !DoorSide(door, player, side)) return;
    InterlockedIncrement(&doorSidesChecked);
    gate::Event("coop_worldcalls door %08X %s: came_from=%u we_stand_on=%u user=%08X%s", door, why, stored, side ? 1 : 0, user, user == player ? " (us)" : "");
}
// While its user stands in the doorway, the door notes on which side
// (0x00DD2030: +0x240 bit 16 = side, bit 17 = noted). When it has shut, that
// note, not the door user's position, picks the "closed with the user on side X"
// output (slot 14/15, which the level wires to room visibility) and the leave
// set (0x00DD27C2). 23.09.: a scene carried the client from the doorway into
// the next room; the note still said the old side, so his game showed the old
// room and hid the one he stood in. At closing the door user is out of the
// doorway, so his side is asked again and the note replaced when it is stale.
constexpr uint32_t kDoorFlags = 0x240, kDoorSideNoted = 0x20000, kDoorNotedFront = 0x10000;
// Only after a scene: 12.10., with no scene near, a replaced note made door
// 3B902090 load the old intro set behind the host again (and the 25 enemies
// still alive in it).
// Outside a scene the game's own note stands.
volatile LONG storedSidesChecked = 0, storedSidesFixed = 0, storedSidesNoScene = 0;
void FreshenStoredSide(uint32_t door) {
    if (!doorSideOk || !doorSideFix) return;
    const DWORD now = GetTickCount();
    if (!cutscene::SequenceRunningNow() && !(streams::referenceUntil && now < streams::referenceUntil)) { InterlockedIncrement(&storedSidesNoScene); return; }
    uint32_t flags = 0, user = 0;
    if (!Read(door + kDoorFlags, &flags, 4) || !(flags & kDoorSideNoted) || !Read(door + kDoorUser, &user, 4) || !user) return;
    bool side = false;
    if (!DoorSide(door, user, side)) return;
    InterlockedIncrement(&storedSidesChecked);
    const bool noted = (flags & kDoorNotedFront) != 0;
    if (noted == side) return;
    const uint32_t fixed = side ? (flags | kDoorNotedFront) : (flags & ~kDoorNotedFront);
    if (!WriteBytes(door + kDoorFlags, &fixed, 4)) return;
    InterlockedIncrement(&storedSidesFixed);
    gate::Event("coop_worldcalls door %08X closing: its user %08X%s was noted on side %u but stands on side %u now: note replaced",
                door, user, user == LocalCampaignPlayer() ? " (us)" : "", noted ? 1u : 0u, side ? 1u : 0u);
}
// A shut door fires "closed with its user on side X" (door update 0x00DD2805:
// slot 14 when EntityOnFrontSideOfDoor(user), else 15; hashes at attributes
// +0x224 / +0x230, through the dispatch), and the level wires that output to
// hide the rooms on the other side. 12.10.: the client went ahead through the
// saferoom door, the host's copy of the door shut with his ghost as its user,
// and its back output (disable_cull_set_chase_hall_z01) hid the chase hall the
// host stood in. The output still runs (it also starts scares, removals and
// checkpoints); only its cull sets are dropped when our Isaac is on the other
// side ([Streams] DoorCullsFollowUs).
constexpr uint32_t kDoorShutFront = 0x224, kDoorShutBack = 0x230;
struct ClosingDoor { uint32_t door, front, back; DWORD at; };
ClosingDoor closingDoors[8]{};
bool doorCullsFollowUs = false;
volatile LONG doorShutsSeen = 0, doorShutsOurs = 0, doorShutsFiltered = 0, doorShutsDropped = 0, doorShutsNoSide = 0, doorShutLines = 0;
void NoteClosingDoor(uint32_t door, DWORD now) {
    uint32_t attributes = 0, front = 0, back = 0;
    if (!doorCullsFollowUs || !Read(door + 0x0C, &attributes, 4) || !attributes || !Read(attributes + kDoorShutFront, &front, 4) ||
        !Read(attributes + kDoorShutBack, &back, 4) || (!front && !back)) return;
    ClosingDoor* slot = nullptr;
    for (auto& c : closingDoors) if (c.door == door) { slot = &c; break; }
    if (!slot) {
        slot = &closingDoors[0];
        for (auto& c : closingDoors) if (now - c.at > now - slot->at) slot = &c;
    }
    *slot = ClosingDoor{door, front, back, now};
}
// From the dispatch: true drops the message itself (heard only by StreamScripters).
bool DoorCloseCull(uint32_t hash) {
    if (!doorCullsFollowUs || !peerTable.Count()) return false;
    const DWORD now = GetTickCount();
    for (auto& c : closingDoors) {
        if (!c.door || now - c.at > 15000 || (hash != c.front && hash != c.back)) continue;
        const ClosingDoor door = c;
        c = ClosingDoor{};
        InterlockedIncrement(&doorShutsSeen);
        const bool outputFront = hash == door.front;
        const uint32_t body = LocalSlotZeroEntity();
        bool weFront = false;
        if (!body || !doorSideOk || !DoorSide(door.door, body, weFront)) { InterlockedIncrement(&doorShutsNoSide); return false; }
        if (weFront == outputFront) { InterlockedIncrement(&doorShutsOurs); return false; }
        uint32_t user = 0;
        Read(door.door + kDoorUser, &user, 4);
        if (mirror::OnlyStreamScripters(hash)) {
            InterlockedIncrement(&doorShutsDropped);
            if (InterlockedIncrement(&doorShutLines) <= 100)
                gate::Event("coop_worldcalls door %08X shut with its user %08X on side %u, our Isaac on side %u: its cull set %08X (%s) dropped",
                            door.door, user, outputFront ? 1u : 0u, weFront ? 1u : 0u, hash, coop_names::Name(hash));
            return true;
        }
        const unsigned armed = mirror::ArmManagersOf(hash, now + 3000, true, true);
        InterlockedIncrement(&doorShutsFiltered);
        if (InterlockedIncrement(&doorShutLines) <= 100)
            gate::Event("coop_worldcalls door %08X shut with its user %08X on side %u, our Isaac on side %u: output %08X (%s) runs without its cull sets (%u managers)",
                        door.door, user, outputFront ? 1u : 0u, weFront ? 1u : 0u, hash, coop_names::Name(hash), armed);
        return false;
    }
    return false;
}
bool doorsVote = true;
DWORD doorVoteTtlMs = 30000;
constexpr unsigned kDoorVotes = 32;
struct DoorVote { uint32_t guid[4]; unsigned slot; DWORD at; };
DoorVote doorVotes[kDoorVotes]{};
volatile LONG votesSent = 0, votesReceived = 0, votesReset = 0, necroVotesSkipped = 0, necroDoorsForced = 0, menuVotesSkipped = 0;
bool VoteGuidEqual(const uint32_t* a, const uint32_t* b) { return std::memcmp(a, b, 16) == 0; }
void NoteVote(const uint32_t* guid, unsigned slot, DWORD now) {
    now = Stamp(now);   // 0 marks a free entry
    DoorVote* oldest = &doorVotes[0];
    for (auto& v : doorVotes) {
        if (v.at && VoteGuidEqual(v.guid, guid) && v.slot == slot) { v.at = now; return; }
        if (!v.at) { oldest = &v; break; }
        if (v.at < oldest->at) oldest = &v;
    }
    std::memcpy(oldest->guid, guid, 16);
    oldest->slot = slot;
    oldest->at = now;
}
volatile LONG doorNearCounted = 0;
// A partner is at the door when his world asked it to open, or when he stands
// within DoorWaitReach of it (24.09., school: the host stood 1-4 m from the
// door the client wanted open, never walked into its opening range, and the
// client waited behind a shut door). Only a door somebody asked for opens.
bool PartnerNearDoor(unsigned slot, const float* doorPos, DWORD now) {
    const auto& runtime = peerRuntime[slot];
    return doorPos && runtime.lastState && now - runtime.lastState <= 5000 &&
           coop_gate::DistanceSquared(runtime.pos, doorPos) <= doorWaitReach * doorWaitReach;
}
bool PartnersVoted(const uint32_t* guid, DWORD now, char* who, size_t whoSize, const float* doorPos = nullptr) {
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        if (!peer.used || !peer.token || now - peer.lastSeen > 10000) continue;
        if (roles::IsNecro(s, now)) { InterlockedIncrement(&necroVotesSkipped); continue; }   // never waited for
        // nor a partner in the main menu (his side channel lives on there; the gate skips him the same way)
        if (level::PeerInMenu(peer.token, now)) { InterlockedIncrement(&menuVotesSkipped); continue; }
        bool voted = false;
        for (const auto& v : doorVotes)
            if (v.at && v.slot == s && TicksSince(now, v.at) <= doorVoteTtlMs && VoteGuidEqual(v.guid, guid)) { voted = true; break; }
        if (voted) continue;
        if (PartnerNearDoor(s, doorPos, now)) { InterlockedIncrement(&doorNearCounted); continue; }
        std::snprintf(who, whoSize, "partner %u has not reached this door", s);
        return false;
    }
    return true;
}
// A vote is spent by the opening it produced: the same door a minute later
// needs everybody at it again (23:05 the host walked back through the first
// door on the client's ten-second-old vote).
void ConsumeVotes(const uint32_t* guid) {
    for (auto& v : doorVotes) if (v.at && VoteGuidEqual(v.guid, guid)) v = DoorVote{};
}
void ConsumeVotesFor(uint32_t door) {
    uint32_t guid[4]{};
    if (Read(door + 0x24, guid, 16)) ConsumeVotes(guid);
}
void SendVote(uint32_t door, DWORD) {
    uint32_t guid[4]{};
    if (!Read(door + 0x24, guid, 16)) return;
    mirror::OnLocalCall(guid, kCallDoorVote, 0, 0, "Door::Vote", true, false);
    InterlockedIncrement(&votesSent);
}
bool PartnersAtDoor(uint32_t door, DWORD now, char* who, size_t whoSize);
bool PartnersReady(uint32_t door, DWORD now, char* who, size_t whoSize) {
    if (!doorsVote) return PartnersAtDoor(door, now, who, whoSize);
    uint32_t guid[4]{};
    if (!Read(door + 0x24, guid, 16)) return true;
    float pos[3]{};
    const bool havePos = Read(door + placement::kPlayerPosition, pos, 12);
    return PartnersVoted(guid, now, who, whoSize, havePos ? pos : nullptr);
}
// Doors opened the native way for a partner (ReplayCall: TryToActivateDoor with
// the local Isaac as user). Once the room behind has loaded the door asks for
// state 4 itself; that is the partner's opening going on, not a new local
// wish, and the votes behind it were spent by the replay. Without this the
// door waited DoorWaitMs for votes that never came again.
struct PartnerOpening { uint32_t door; DWORD at; };
PartnerOpening partnerOpenings[8]{};
constexpr DWORD kPartnerOpeningTtlMs = 120000;
volatile LONG partnerOpeningsPassed = 0, partnerOpeningsExpired = 0;
void NotePartnerOpening(uint32_t door, DWORD now) {
    PartnerOpening* slot = &partnerOpenings[0];
    for (auto& p : partnerOpenings) {
        if (p.door == door || !p.door) { slot = &p; break; }
        if (p.at < slot->at) slot = &p;
    }
    *slot = PartnerOpening{door, now};
}
bool TakePartnerOpening(uint32_t door, DWORD now) {
    for (auto& p : partnerOpenings) {
        if (p.door != door) continue;
        const bool fresh = now - p.at <= kPartnerOpeningTtlMs;
        if (!fresh) InterlockedIncrement(&partnerOpeningsExpired);
        p = PartnerOpening{};
        return fresh;
    }
    return false;
}
void ForgetDoorState() {
    for (auto& p : partnerOpenings) p = PartnerOpening{};
    for (auto& h : openHolds) h = OpenHold{};
}
void OnRestart() {
    for (auto& v : doorVotes) v = DoorVote{};
    ForgetDoorState();
    streams::OnRestartLeave();
    if (doorWait.door) { if (doorWait.noticed) notice::Show("CONTINUING WITHOUT PARTNER"); doorWait = DoorWait{}; }
    InterlockedIncrement(&votesReset);
}
bool PartnersAtDoor(uint32_t door, DWORD now, char* who, size_t whoSize) {
    float pos[3]{};
    if (!Read(door + placement::kPlayerPosition, pos, 12)) return true;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        if (!peer.used || !peer.token || now - peer.lastSeen > 10000) continue;
        if (roles::IsNecro(s, now) || level::PeerInMenu(peer.token, now)) continue;
        const auto& runtime = peerRuntime[s];
        if (!runtime.lastState || now - runtime.lastState > 5000) continue;
        const float dist = std::sqrt(coop_gate::DistanceSquared(runtime.pos, pos));
        if (dist <= doorWaitReach) continue;
        std::snprintf(who, whoSize, "partner %u is %.0f m from the door", s, dist);
        return false;
    }
    return true;
}
bool doorStateHeld = false;
void ReplayWatchTick(DWORD now);
void DoorWaitTick(DWORD now) {
    ReplayWatchTick(now);
    if (!inLevel) {
        if (doorStateHeld) { ForgetDoorState(); doorStateHeld = false; }   // door addresses belong to the old level
    } else {
        doorStateHeld = true;
    }
    if (!doorWait.door) return;
    if (!inLevel) { doorWait = DoorWait{}; return; }
    if (!LocalSlotZeroEntity()) {
        // No body at all (mid hand-over or mid transition): the door already
        // swallowed its TransitionToState, so it is re-issued now rather than
        // dropped; a dropped one stays shut forever (audit 2026-09-21).
        const DoorWait w = doorWait;
        doorWait = DoorWait{};
        __try { ClearLockHologram(w.door); doorHook.thiscall<void>(reinterpret_cast<void*>(w.door), w.state); } __except (Filter(GetExceptionCode())) {}
        InterlockedIncrement(&doorWaitReleased);
        return;
    }
    if (TicksSince(now, doorWait.lastTry) < 250) return;
    doorWait.lastTry = now;
    char who[96] = "";
    const bool here = !peerTable.Count() || PartnersReady(doorWait.door, now, who, sizeof(who));
    // the wait was stamped in the door hook: a stamp a moment newer than this now is no timeout
    const DWORD waited = TicksSince(now, doorWait.since);
    const bool late = waited >= doorWaitMs;
    if (!here && !late) {
        if (doorsVote && TicksSince(now, doorWait.lastVote) >= 2000) { doorWait.lastVote = now; SendVote(doorWait.door, now); }   // in case the first one was lost
        if (!doorWait.noticed && waited >= 1500) { doorWait.noticed = true; notice::Show("WAITING FOR PARTNER - DOOR"); }
        return;
    }
    const DoorWait w = doorWait;
    doorWait = DoorWait{};
    uint32_t guid[4]{};
    if (Read(w.door + 0x24, guid, 16)) {
        ConsumeVotes(guid);
        mirror::OnLocalCall(guid, kCallDoorTransition, w.state, 0, "Door::TransitionToState", true);
        InterlockedIncrement(&doorMirrored);
    }
    __try {
        ClearLockHologram(w.door);
        doorHook.thiscall<void>(reinterpret_cast<void*>(w.door), w.state);
    } __except (Filter(GetExceptionCode())) {
    }
    InterlockedIncrement(late ? &doorWaitTimedOut : &doorWaitReleased);
    gate::Event("coop_worldcalls door %08X opens after %u ms %s", w.door, static_cast<unsigned>(waited), late ? "TIMEOUT" : "(partners at the door)");
    if (w.noticed) notice::Show(late ? "CONTINUING WITHOUT PARTNER" : "PARTNER HERE - CONTINUING");
}

void ReplayOpening(uint32_t door, const uint32_t* guid, uint32_t state);
void NoteReplayOutcome(uint32_t door, uint32_t wanted);
void __fastcall OnDoorTransition(void* self, void*, uint32_t state) {
    InterlockedIncrement(&doorCalls);
    if (self) NoteReplayOutcome(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)), state);
    // a campaign arena door held open (runtime_coop_arena.inc): still open, asked again next tick
    if ((state == kDoorClosing || state == kDoorClosed) && self && arena::RefusesClose(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)), state)) return;
    // A door whose leave set is held is being used again: decide the leave now.
    if (self && state != 0 && state != kDoorClosing) streams::OnDoorReuse(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)));
    if (self) streams::vis::NoteDoor(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)));
    // A living necro opens a door without waiting for the humans (they may be anywhere);
    // the opening is mirrored as usual below.
    const bool necroHere = state == kDoorOpening && roles::LocalNecro();
    if (necroHere && doorsWait) InterlockedIncrement(&necroDoorsForced);
    if (enabled && doorsWait && !necroHere && state == kDoorOpening && gate::mirroring == 0 && gate::replaying == 0 && self && peerTable.Count() &&
        inLevel && levelSettled) {
        const uint32_t door = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
        const DWORD now = GetTickCount();
        char who[96] = "";
        uint32_t current = 0;
        if (Read(door + kDoorState, &current, 4) && (current == kDoorOpening || current == kDoorOpen)) {
            doorHook.thiscall<void>(self, state);   // already on its way: not a new wish, no vote spent
            return;
        }
        if (TakePartnerOpening(door, now)) {
            InterlockedIncrement(&partnerOpeningsPassed);
            if (gate::trace) gate::Event("coop_worldcalls door %08X opens: the partner's opening, its room has loaded", door);
            doorHook.thiscall<void>(self, state);
            return;
        }
        // Every local opening says where everybody stood: the 22:45 run opened
        // the first door with door_waits=0 and nobody could tell why.
        static uint32_t loggedDoor = 0;
        static DWORD loggedAt = 0;
        if (gate::trace && (loggedDoor != door || now - loggedAt >= 2000)) {
            loggedDoor = door; loggedAt = now;
            float pos[3]{}, me[3]{};
            char where[256] = "";
            size_t used = 0;
            if (Read(door + placement::kPlayerPosition, pos, 12) && gate::LocalPosition(me))
                used += static_cast<size_t>(std::snprintf(where, sizeof(where), "me=%.0f m", std::sqrt(coop_gate::DistanceSquared(me, pos))));
            for (unsigned s = 0; s < coop_peers::kMaxPeers && used < sizeof(where) - 40; ++s) {
                const auto& peer = peerTable.peers[s];
                if (!peer.used || !peer.token || now - peer.lastSeen > 10000) continue;
                const auto& runtime = peerRuntime[s];
                if (!runtime.lastState || now - runtime.lastState > 5000) { used += static_cast<size_t>(std::snprintf(where + used, sizeof(where) - used, " partner%u=no position", s)); continue; }
                used += static_cast<size_t>(std::snprintf(where + used, sizeof(where) - used, " partner%u=%.0f m", s, std::sqrt(coop_gate::DistanceSquared(runtime.pos, pos))));
            }
            gate::Event("coop_worldcalls door %08X asked to open: %s (reach %.0f m)", door, where, doorWaitReach);
        }
        if (doorsVote && (doorWait.door != door || now - doorWait.lastVote >= 2000)) SendVote(door, now);   // the local Isaac is at this door
        if (!PartnersReady(door, now, who, sizeof(who))) {
            if (doorWait.door && doorWait.door != door) InterlockedIncrement(&doorWaitReplaced);
            if (doorWait.door != door) {
                doorWait = DoorWait{door, state, now, now, now, false};
                InterlockedIncrement(&doorWaits);
                gate::Event("coop_worldcalls door %08X waits to open: %s", door, who);
            }
            return;   // opens from DoorWaitTick
        }
        if (doorsVote) ConsumeVotesFor(door);   // everybody was here: spent
        if (doorWait.door == door) doorWait = DoorWait{};
    }
    if (enabled && state == kDoorClosing && gate::mirroring == 0 && gate::replaying == 0 && self &&
        KeepOpen(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)))) {
        InterlockedIncrement(&doorHeldOpen);
        return;   // still open, asked again next tick
    }
    if (enabled && state == kDoorClosing && self && peerTable.Count()) {
        const uint32_t door = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
        FreshenStoredSide(door);
        NoteClosingDoor(door, GetTickCount());
        LogDoorSide(door, "closing");   // the leave set it asks for later is judged in streams::DoorActivation
        doorHook.thiscall<void>(self, state);
        return;
    }
    if (enabled && doorsEnabled && state == kDoorOpening && gate::mirroring == 0 && gate::replaying == 0 && self && peerTable.Count()) {
        uint32_t guid[4]{};
        if (Read(reinterpret_cast<uintptr_t>(self) + 0x24, guid, 16)) {
            mirror::OnLocalCall(guid, kCallDoorTransition, state, 0, "Door::TransitionToState", true);
            InterlockedIncrement(&doorMirrored);
        }
    }
    doorHook.thiscall<void>(self, state);
}

bool __fastcall OnSetCoverState(void* self, void*, int present, int arg) {
    InterlockedIncrement(&coverCalls);
    if (enabled && IsHost() && gate::mirroring == 0 && gate::replaying == 0 && self) {
        uint32_t guid[4]{};
        if (Read(reinterpret_cast<uintptr_t>(self) + 0x24, guid, 16)) {
            mirror::OnLocalCall(guid, kCallSetCoverState, static_cast<uint32_t>(present), static_cast<uint32_t>(arg), "VentMarker::SetCoverState");
            InterlockedIncrement(&coverMirrored);
        }
    }
    return setCoverHook.thiscall<bool>(self, present, arg);
}

int Filter(unsigned) { InterlockedIncrement(&faults); return EXCEPTION_EXECUTE_HANDLER; }

// 24.10.: a refused TryToActivateDoor (one-way side, power, key) fell back to the direct
// opening, and the client looked through the open door into rooms it never asked for.
// Now a door with an open set always opens the native way ([Mirror] DoorReplayLoads):
// refused, our Isaac becomes its user (+0x260) on his side (+0x259) and the door goes to
// LOADING (2). Entering 2 (0x00DD24B0) activates that side's open set with the door as
// actor and keeps the node at +0x34C; the tick opens once LoadComplete cleared it
// (0x00DD9F9B), or goes back to 0 when nobody is left in its field (0x00DD9F01).
// Only a door with both open sets empty opens directly.
bool doorReplayLoads = true;
constexpr uint32_t kDoorLockedByte = 0x258, kDoorPendingSet = 0x34C, kDoorOpenSide1 = 0xC0, kDoorOpenSide0 = 0xE0;
volatile LONG replayNative = 0, replayLoading = 0, replayDirect = 0, replayDirectNoIsaac = 0, replayLines = 0;
volatile LONG replayOpened = 0, replayCancelled = 0, replayStuck = 0, replayOutcomeLines = 0;
bool OpenSets(uint32_t door, uint32_t out[2][4]) {
    uint32_t desc = 0;
    if (!door || !Read(door + 0x0C, &desc, 4) || !desc) return false;
    return Read(desc + kDoorOpenSide0, out[0], 16) && Read(desc + kDoorOpenSide1, out[1], 16);
}
bool ZeroGuid(const uint32_t* g) { return !(g[0] | g[1] | g[2] | g[3]); }
// What became of a door replayed through LOADING: opened, cancelled, or still waiting.
struct ReplayWatch { uint32_t door; DWORD at; bool stuckLogged; };
ReplayWatch replayWatches[8]{};
volatile LONG replayWatching = 0;
void WatchReplay(uint32_t door, DWORD now) {
    ReplayWatch* slot = &replayWatches[0];
    for (auto& w : replayWatches) {
        if (w.door == door || !w.door) { slot = &w; break; }
        if (w.at < slot->at) slot = &w;
    }
    *slot = ReplayWatch{door, Stamp(now), false};
    InterlockedExchange(&replayWatching, 1);
}
void NoteReplayOutcome(uint32_t door, uint32_t wanted) {
    if (!InterlockedCompareExchange(&replayWatching, 0, 0) || (wanted != kDoorOpening && wanted != kDoorClosed)) return;
    for (auto& w : replayWatches) {
        if (w.door != door) continue;
        uint32_t current = 0;
        Read(door + kDoorState, &current, 4);
        const DWORD ms = TicksSince(GetTickCount(), w.at);
        if (wanted == kDoorOpening) {
            InterlockedIncrement(&replayOpened);
            if (InterlockedIncrement(&replayOutcomeLines) <= 100)
                gate::Event("coop_worldcalls door %08X replay: opens %u ms later, its room loaded first (state %u -> 4)", door, static_cast<unsigned>(ms), current);
        } else if (current == kDoorLoading) {
            InterlockedIncrement(&replayCancelled);
            NotePartnerOpening(door, GetTickCount());   // our own opening there later needs no votes
            if (InterlockedIncrement(&replayOutcomeLines) <= 100)
                gate::Event("coop_worldcalls door %08X replay: CANCELLED by the door %u ms later (nobody left in its field while its room loaded): stays shut here "
                            "until our Isaac opens it",
                            door, static_cast<unsigned>(ms));
        } else {
            continue;   // a close from another state: not the replay's outcome
        }
        w = ReplayWatch{};
        return;
    }
}
void ReplayWatchTick(DWORD now) {
    if (!InterlockedCompareExchange(&replayWatching, 0, 0)) return;
    bool any = false;
    for (auto& w : replayWatches) {
        if (!w.door) continue;
        const DWORD age = TicksSince(now, w.at);
        if (!inLevel || age > 120000) { w = ReplayWatch{}; continue; }
        any = true;
        if (w.stuckLogged || age < 20000) continue;
        w.stuckLogged = true;
        uint32_t state = 0, pending = 0;
        Read(w.door + kDoorState, &state, 4);
        Read(w.door + kDoorPendingSet, &pending, 4);
        InterlockedIncrement(&replayStuck);
        if (InterlockedIncrement(&replayOutcomeLines) <= 100)
            gate::Event("coop_worldcalls door %08X replay: still state %u after %u ms (waiting for set node %08X)", w.door, state, static_cast<unsigned>(age), pending);
    }
    if (!any) InterlockedExchange(&replayWatching, 0);
}
// A partner opened this door (state 4 asked for by his world). `state` is ours now.
void ReplayOpening(uint32_t door, const uint32_t* guid, uint32_t state) {
    const DWORD now = GetTickCount();
    const uint32_t player = LocalCampaignPlayer();
    uint32_t sets[2][4]{}, flags = 0;
    unsigned char locked = 0, sideByte = 0;
    const bool noSets = !OpenSets(door, sets) || (ZeroGuid(sets[0]) && ZeroGuid(sets[1]));
    Read(door + kDoorFlags, &flags, 4);
    Read(door + kDoorLockedByte, &locked, 1);
    Read(door + kDoorSideByte, &sideByte, 1);
    float doorPos[3]{}, me[3]{};
    float distance = -1.0f;
    if (Read(door + placement::kPlayerPosition, doorPos, 12) && gate::LocalPosition(me)) distance = std::sqrt(coop_gate::DistanceSquared(me, doorPos));
    bool side = false;
    const bool sideKnown = player && doorSideOk && DoorSide(door, player, side);
    int tried = -1;   // TryToActivateDoor: -1 not asked
    uint32_t after = state;
    if (state == kDoorClosed && player) {
        tried = reinterpret_cast<bool(__thiscall*)(void*, uint32_t)>(base + kTryActivateDoorRva)(reinterpret_cast<void*>(door), player) ? 1 : 0;
        Read(door + kDoorState, &after, 4);
    }
    const char* path = "";
    if (after == kDoorUsed || after == kDoorLoading || after == kDoorOpening || after == kDoorOpen) {
        path = "native (TryToActivateDoor with our Isaac)";
        InterlockedIncrement(&doorNative);
        InterlockedIncrement(&replayNative);
        if (after == kDoorUsed || after == kDoorLoading) { NotePartnerOpening(door, now); WatchReplay(door, now); }
    } else if (!doorReplayLoads || noSets || !player || state == kDoorClosing) {
        path = noSets ? "direct (no open set)" : !player ? "direct (no Isaac here)" : state == kDoorClosing ? "direct (still closing, its rooms are in)"
                                                                                                         : "direct (DoorReplayLoads=0)";
        ClearLockHologram(door);
        doorHook.thiscall<void>(reinterpret_cast<void*>(door), kDoorOpening);
        Read(door + kDoorState, &after, 4);
        InterlockedIncrement(&doorReplayed);
        InterlockedIncrement(player ? &replayDirect : &replayDirectNoIsaac);
    } else {
        // Refused here: used by our Isaac from his side, the room of that side loads first.
        const unsigned char ourSide = sideKnown ? (side ? 1 : 0) : sideByte;
        const uint32_t noPending = 0;
        WriteBytes(door + kDoorUser, &player, 4);
        WriteBytes(door + kDoorSideByte, &ourSide, 1);
        WriteBytes(door + kDoorPendingSet, &noPending, 4);   // a leave set never activated must not be waited for
        ClearLockHologram(door);
        doorHook.thiscall<void>(reinterpret_cast<void*>(door), kDoorLoading);
        Read(door + kDoorState, &after, 4);
        NotePartnerOpening(door, now);
        WatchReplay(door, now);
        path = "state 2 (refused here: loads our side's open set, opens on its LoadComplete)";
        InterlockedIncrement(&replayLoading);
    }
    ConsumeVotes(guid);
    if (doorWait.door == door) { if (doorWait.noticed) notice::Show("PARTNER HERE - CONTINUING"); doorWait = DoorWait{}; }
    streams::vis::NoteDoor(door);
    if (InterlockedIncrement(&replayLines) <= 100)
        gate::Event("coop_worldcalls door %08X replay for the partner (%08X-%08X-%08X-%08X): %s; our Isaac %.0f m away on side %d, came_from=%u flags(+0x240)=%08X "
                    "locked(+0x258)=%u TryToActivateDoor=%s state %u -> %u, open sets side0=%08X-%08X side1=%08X-%08X%s",
                    door, guid[0], guid[1], guid[2], guid[3], path, distance, sideKnown ? (side ? 1 : 0) : -1, sideByte, flags, locked,
                    tried < 0 ? "not asked" : tried ? "accepted" : "REFUSED", state, after, sets[0][0], sets[0][1], sets[1][0], sets[1][1], noSets ? " (both empty)" : "");
}

// Client: the same call on the local copy of the object.
bool ReplayCall(uint32_t callId, const uint32_t* guid, const uint32_t* args, bool fromJournal, unsigned authorSlot) {
    if (!installed || !enabled) return false;
    if (callId == kCallDoorVote) {
        if (fromJournal || authorSlot >= coop_peers::kMaxPeers) return true;
        NoteVote(guid, authorSlot, GetTickCount());
        InterlockedIncrement(&votesReceived);
        if (gate::trace) gate::Event("coop_worldcalls door vote from partner %u for %08X-%08X-%08X-%08X", authorSlot, guid[0], guid[1], guid[2], guid[3]);
        return true;
    }
    const uint32_t object = mirror::ObjectByGuid(guid);
    if (!object) return false;
    uint32_t vtable = 0;
    if (!Read(object, &vtable, 4)) return false;
    bool ok = false;
    ++gate::mirroring;
    __try {
        switch (callId) {
        case kCallDoorTransition: {
            // StandardDoor is the only class with this state machine: its class
            // query (0x00DA13F0) sits in exactly one vtable and no subclass
            // chains to it (28.09. dump). A pool GUID resolving to anything else
            // is refused.
            if (vtable != base + kStandardDoorVtableRva) { InterlockedIncrement(&wrongClass); break; }
            uint32_t state = 0;
            if (!doorHook || !Read(object + kDoorState, &state, 4)) break;
            if (state == kDoorOpening || state == kDoorOpen) { InterlockedIncrement(&doorSkipped); ok = true; break; }
            LogDoorSide(object, "opened by a partner");
            if (state == kDoorUsed || state == kDoorLoading) { InterlockedIncrement(&doorSkipped); ok = true; break; }   // already on its native way
            if (args[0] == kDoorOpening) {
                ReplayOpening(object, guid, state);
                ok = true;
                break;
            }
            doorHook.thiscall<void>(reinterpret_cast<void*>(object), args[0]);
            InterlockedIncrement(&doorReplayed);
            if (gate::trace) gate::Event("coop_worldcalls door %08X opened for the partner (was state %u)", object, state);
            // The partner opened it for everybody: the local wait for this door
            // is over, and the votes behind it are spent.
            ConsumeVotes(guid);
            if (doorWait.door == object) { if (doorWait.noticed) notice::Show("PARTNER HERE - CONTINUING"); doorWait = DoorWait{}; }
            ok = true;
            break;
        }
        case kCallSetCoverState:
            if (vtable != base + kVentMarkerVtableRva) { InterlockedIncrement(&wrongClass); break; }
            if (setCoverHook) {
                setCoverHook.thiscall<bool>(reinterpret_cast<void*>(object), static_cast<int>(args[0]), static_cast<int>(args[1]));
                InterlockedIncrement(&coverReplayed);
                if (gate::trace) gate::Event("coop_worldcalls VentMarker::SetCoverState(%u, %u) replayed on %08X", args[0], args[1], object);
                ok = true;
            }
            break;
        default:
            break;
        }
    } __except (Filter(GetExceptionCode())) {
        ok = false;
    }
    --gate::mirroring;
    return ok;
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Mirror", "WorldCalls", 1, ini.c_str()) != 0;
    // The calls and the door votes travel through the mirror: without it a door would wait DoorWaitMs for nothing.
    if (!mirror::installed || !mirror::enabled) enabled = false;
    // push ebx / push esi / mov esi,ecx / xor bl,bl / call
    static const unsigned char kSig[] = {0x53,0x56,0x8B,0xF1,0x32,0xDB,0xE8};
    if (!hook_site::Install(log, "coop_worldcalls VentMarker::SetCoverState", base + kSetCoverStateRva, kSig, sizeof(kSig),
                            reinterpret_cast<void*>(OnSetCoverState), setCoverHook))
        return false;
    doorsEnabled = GetPrivateProfileIntA("Mirror", "DoorOpenings", 1, ini.c_str()) != 0;
    doorsStayOpen = GetPrivateProfileIntA("Mirror", "DoorsStayOpen", 1, ini.c_str()) != 0;
    doorReach = static_cast<float>(GetPrivateProfileIntA("Mirror", "DoorReach", 30, ini.c_str()));
    doorsWait = GetPrivateProfileIntA("Mirror", "DoorsWaitForPartners", 1, ini.c_str()) != 0;
    doorWaitReach = static_cast<float>(GetPrivateProfileIntA("Mirror", "DoorWaitReach", 5, ini.c_str()));
    doorWaitMs = static_cast<DWORD>(GetPrivateProfileIntA("Mirror", "DoorWaitMs", 180000, ini.c_str()));
    doorsVote = GetPrivateProfileIntA("Mirror", "DoorsVote", 1, ini.c_str()) != 0;
    doorSideFix = GetPrivateProfileIntA("Mirror", "DoorSideFix", 1, ini.c_str()) != 0;
    doorReplayLoads = GetPrivateProfileIntA("Mirror", "DoorReplayLoads", 1, ini.c_str()) != 0;
    std::fprintf(log, "coop_worldcalls door_replay_loads=%d (a partner's door opening loads our side's room first; only a door without open sets opens directly)\n",
                 doorReplayLoads ? 1 : 0);
    doorCullsFollowUs = GetPrivateProfileIntA("Streams", "DoorCullsFollowUs", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop_worldcalls door_culls_follow_us=%d (a shut door hides no room on our Isaac's side when its user was a partner)\n", doorCullsFollowUs ? 1 : 0);
    {
        // push ebp / mov ebp,esp / and esp,-16 / sub esp,0x58 / push ebx / push esi / mov esi,[ebp+8] / xor bl,bl
        static const unsigned char kSide[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,0x58,0x53,0x56,0x8B,0x75,0x08,0x32,0xDB};
        unsigned char actual[sizeof(kSide)]{};
        doorSideOk = Read(base + kDoorSideRva, actual, sizeof(actual)) && std::memcmp(actual, kSide, sizeof(kSide)) == 0;
        std::fprintf(log, "coop_worldcalls door_side_fix=%d EntityOnFrontSideOfDoor at %08X %s\n", doorSideFix ? 1 : 0,
                     static_cast<unsigned>(base + kDoorSideRva), doorSideOk ? "matches" : "SIGNATURE MISMATCH - side fix off");
    }
    doorVoteTtlMs = static_cast<DWORD>(GetPrivateProfileIntA("Mirror", "DoorVoteTtlMs", 30000, ini.c_str()));
    // sub esp,0x18 / push ebp / push esi / mov esi,ecx / mov eax,[esi+0x5C] / mov edx,[eax+0x2C]
    static const unsigned char kDoor[] = {0x83,0xEC,0x18,0x55,0x56,0x8B,0xF1,0x8B,0x46,0x5C,0x8B,0x50,0x2C};
    if (!hook_site::Install(log, "coop_worldcalls Door::TransitionToState", base + kDoorTransitionRva, kDoor, sizeof(kDoor),
                            reinterpret_cast<void*>(OnDoorTransition), doorHook))
        doorsEnabled = false;
    installed = true;
    std::fprintf(log, "coop_worldcalls enabled=%d doors=%d doors_wait=%d doors_vote=%d vote_ttl_ms=%lu wait_reach_m=%d wait_ms=%lu (vent covers follow the host; door openings follow whoever opened; a door opens when everybody reached it)\n",
                 enabled, doorsEnabled, doorsWait ? 1 : 0, doorsVote ? 1 : 0, static_cast<unsigned long>(doorVoteTtlMs), static_cast<int>(doorWaitReach), static_cast<unsigned long>(doorWaitMs));
    return true;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG activity = InterlockedCompareExchange(&coverCalls, 0, 0) + InterlockedCompareExchange(&coverReplayed, 0, 0) +
                          InterlockedCompareExchange(&doorMirrored, 0, 0) + InterlockedCompareExchange(&doorReplayed, 0, 0) +
                          InterlockedCompareExchange(&doorWaits, 0, 0) + InterlockedCompareExchange(&votesReceived, 0, 0) + InterlockedCompareExchange(&votesReset, 0, 0) +
                          InterlockedCompareExchange(&storedSidesChecked, 0, 0) + InterlockedCompareExchange(&partnerOpeningsPassed, 0, 0) +
                          InterlockedCompareExchange(&wrongClass, 0, 0) + InterlockedCompareExchange(&replayOpened, 0, 0) +
                          InterlockedCompareExchange(&replayCancelled, 0, 0) + InterlockedCompareExchange(&replayStuck, 0, 0);
    if (activity == last) return;
    last = activity;
    std::fprintf(log, "coop_worldcalls door_replays native=%ld state2=%ld direct=%ld direct_no_isaac=%ld opened_after_load=%ld cancelled=%ld stuck=%ld\n",
                 InterlockedCompareExchange(&replayNative, 0, 0), InterlockedCompareExchange(&replayLoading, 0, 0),
                 InterlockedCompareExchange(&replayDirect, 0, 0), InterlockedCompareExchange(&replayDirectNoIsaac, 0, 0),
                 InterlockedCompareExchange(&replayOpened, 0, 0), InterlockedCompareExchange(&replayCancelled, 0, 0),
                 InterlockedCompareExchange(&replayStuck, 0, 0));
    std::fprintf(log, "coop_worldcalls door_sides checked=%ld stored_sides_checked=%ld stored_sides_fixed=%ld partner_openings_passed=%ld partner_openings_expired=%ld\n",
                 InterlockedCompareExchange(&doorSidesChecked, 0, 0), InterlockedCompareExchange(&storedSidesChecked, 0, 0),
                 InterlockedCompareExchange(&storedSidesFixed, 0, 0), InterlockedCompareExchange(&partnerOpeningsPassed, 0, 0),
                 InterlockedCompareExchange(&partnerOpeningsExpired, 0, 0));
    std::fprintf(log, "coop_worldcalls stored_sides_left_alone=%ld (no scene within a minute: the game's own doorway note stands)\n",
                 InterlockedCompareExchange(&storedSidesNoScene, 0, 0));
    std::fprintf(log, "coop_worldcalls door_shuts seen=%ld ours=%ld filtered=%ld dropped=%ld side_unknown=%ld\n", InterlockedCompareExchange(&doorShutsSeen, 0, 0),
                 InterlockedCompareExchange(&doorShutsOurs, 0, 0), InterlockedCompareExchange(&doorShutsFiltered, 0, 0),
                 InterlockedCompareExchange(&doorShutsDropped, 0, 0), InterlockedCompareExchange(&doorShutsNoSide, 0, 0));
    std::fprintf(log, "coop_worldcalls door_waits=%ld door_wait_released=%ld door_wait_timed_out=%ld door_wait_replaced=%ld waiting=%08X votes_sent=%ld votes_received=%ld votes_reset=%ld near_counted=%ld\n",
                 InterlockedCompareExchange(&doorWaits, 0, 0), InterlockedCompareExchange(&doorWaitReleased, 0, 0),
                 InterlockedCompareExchange(&doorWaitTimedOut, 0, 0), InterlockedCompareExchange(&doorWaitReplaced, 0, 0), doorWait.door,
                 InterlockedCompareExchange(&votesSent, 0, 0), InterlockedCompareExchange(&votesReceived, 0, 0), InterlockedCompareExchange(&votesReset, 0, 0), InterlockedCompareExchange(&doorNearCounted, 0, 0));
    std::fprintf(log, "coop_worldcalls necro doors: votes_skipped=%ld forced=%ld local_holds_skipped=%ld menu_partners_skipped=%ld | roles sent=%ld received=%ld changes=%ld local=%d necro_partner=%d\n",
                 InterlockedCompareExchange(&necroVotesSkipped, 0, 0), InterlockedCompareExchange(&necroDoorsForced, 0, 0),
                 InterlockedCompareExchange(&necroHoldsSkipped, 0, 0), InterlockedCompareExchange(&menuVotesSkipped, 0, 0), InterlockedCompareExchange(&roles::sent, 0, 0),
                 InterlockedCompareExchange(&roles::received, 0, 0), InterlockedCompareExchange(&roles::changes, 0, 0), roles::lastSentRole,
                 roles::AnyNecroPeer(GetTickCount()) ? 1 : 0);
    std::fprintf(log, "coop_worldcalls cover_calls=%ld cover_mirrored=%ld cover_replayed=%ld wrong_class=%ld faults=%ld "
                      "door_calls=%ld door_mirrored=%ld door_replayed=%ld door_skipped=%ld door_held_open_ticks=%ld holograms_cleared=%ld doors_opened_natively=%ld\n",
                 InterlockedCompareExchange(&coverCalls, 0, 0), InterlockedCompareExchange(&coverMirrored, 0, 0),
                 InterlockedCompareExchange(&coverReplayed, 0, 0), InterlockedCompareExchange(&wrongClass, 0, 0),
                 InterlockedCompareExchange(&faults, 0, 0), InterlockedCompareExchange(&doorCalls, 0, 0),
                 InterlockedCompareExchange(&doorMirrored, 0, 0), InterlockedCompareExchange(&doorReplayed, 0, 0),
                 InterlockedCompareExchange(&doorSkipped, 0, 0), InterlockedCompareExchange(&doorHeldOpen, 0, 0), InterlockedCompareExchange(&hologramsCleared, 0, 0),
                 InterlockedCompareExchange(&doorNative, 0, 0));
}
}  // namespace worldcalls
