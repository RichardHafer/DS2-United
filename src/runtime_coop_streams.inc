// Dead Space 2 United by Richard Hafer.
// Areas: nobody's room is unloaded while they are still in it.
//
// Approach (2026-09-17 17:10): stop reasoning about who waited for whom
// and compare what is LOADED. The stream manager's TOC register is the wrong
// list for that (one package per CHAPTER, all eleven at once; probe 17:20,
// tools/live_probe_tocs.py). The right unit is the StreamSet: a level is a
// chain of stream sets, each one a list of streams; activating one loads its
// members and drops everything else. A door's "leave" set is the room behind
// it; a door's open set is both rooms. So the world a player is in IS the
// last stream set that completed for him (iMsgStreamSetLoadComplete
// 0x5FC7E489, source+8 -> the set's GUID).
//
// Corrected 19:35, after the first run with the symmetric rule: the client
// went ahead and held, the host wandered BACK several rooms (his old rooms
// were still loaded because of the holds) and was held at the first streaming
// door he reached; both waiting for the other, both stuck. The rule is
// now: everybody must be in the host's room. So the host is the
// reference and is never held, and a client is held only when it would drop
// a stream it currently SHARES with the host. Going ahead of the host: held
// at the first set that leaves him behind, the shared room stays loaded.
// Going back toward the host, or anywhere while the host is far away and
// nothing is shared: free, the notices say where the host is.
//
// Every activation, from a script message, a door or a lift, ends in
// StreamSet::Activate (PC 0x00759D00, `thiscall(actor)`, ret 4; six call
// sites: the level's first set 0x0042DA6A, HandleEvents 0x0075A143, the
// doors' leave set 0x00DC5940 and open set 0x00DD259E, the lifts 0x0100A30F /
// 0x0100A471). That call is the choke point, and this is the Dead Space 3
// rule, measured rather than voted:
//
//   an activation that would drop a stream a partner's set still holds is
//   deferred until every partner's set fits inside it.
//
// The leader walks through a door into B: his door's open set (A+B) goes
// through, because the partner's set A fits in it. The door closes behind
// him and asks for B alone: held, because the partner is still in A. The
// partner follows, his own door asks for B alone: now everybody wants B
// (a partner's PENDING set counts as his set), and both machines switch.
// Nothing unloads under anybody, the leader keeps his new room, and it works
// for any number of players, each judged against the same list.
//
// Each machine sends its set and its pending set once a second. A held
// activation is retried every quarter second and let through after
// [Streams] HoldTimeoutMs regardless, because a partner who never follows
// must not end the session.
namespace overlay { void Curtain(DWORD ms); void LiftCurtain(); }   // runtime_coop_overlay.inc
namespace focusrate { void PanelHolds(LONG bit, bool on); }         // runtime_coop_focus.inc
namespace mirror::agreement { bool PreparingRooms(DWORD now); }
namespace bot_sense { extern bool losReady; bool CastRaw(const float* from, const float* to, uint32_t exclude, float& fraction, uint32_t& hit); }   // runtime_coop_bot_sense.inc
namespace streams {
// No walk of its own here: a necro at the vents, or a dead human watching a partner. Both take
// the humans' rooms and pictures (the follow, the reference set, GhostPicture).
bool GhostFollower() { return necro::LocalRole() == 2 || spectate::Active(); }
// A living necro keeps the humans' rooms loaded next to its own (user, 17.10.: "the stream sync
// works only after the necro respawned": only the ghost at the vents took the humans' rooms).
// pin::OnSetDesired merges their sets into every switch of ours, UnionTick switches our own set
// again when a human's set changed, so the merge runs.
bool NecroUnion() { return necro::LocalRole() == 1; }
volatile LONG unionMerged = 0, unionRefreshes = 0, unionFull = 0;
bool enabled = false, installed = false, holdEnabled = true;
constexpr uint32_t kMagic = 0x4D525453;   // 'STRM'
constexpr uint32_t kVersion = 3;
constexpr uint32_t kActivateRva = 0x359D00;
constexpr uint32_t kNodeGuid = 0x24, kNodeMembers = 0x5C, kNodeMemberStride = 0x18, kNodeMemberCount = 0x4AC;
constexpr unsigned kMaxMembers = 46;
constexpr DWORD kSendMs = 1000, kStaleMs = 5000, kRetryMs = 250;
DWORD holdTimeoutMs = 180000;
// A script asked for the set (not a door or a lift): a story transition, not a
// player running ahead. 24.09. ch05 Halo jump: the client's next area was held
// for the host ("WAITING FOR PARTNER TO FOLLOW", up to 180 s) and the jump flew
// on into streams that never came. Such a hold ends after HoldScriptTimeoutMs.
DWORD holdScriptMs = 4000;
volatile LONG sceneActivations = 0, scriptHoldsEnded = 0;
// The local scene asks for its set: never held. 24.09. Tormentor, client leading:
// "holding set 34CB2202 ... host in 39CB17CC" two lines before the sequence
// started, and the scene played without its streams (the camera never started
// right). The partner follows it himself (SceneTick).
bool SceneActivation(DWORD now) {
    const DWORD cue = static_cast<DWORD>(InterlockedCompareExchange(&gate::lastSequenceTick, 0, 0));
    return cutscene::Leading() || cutscene::Following() || cutscene::SequenceRunningNow() || (cue && now - cue < 3000);
}

struct Packet {
    uint32_t magic, version, token, sequence, flags, epoch;
    uint32_t area[4];            // GUID of the sender's last completed stream set
    uint32_t activationHash;     // the message that activated it (0 = unknown)
    uint32_t activationFlags;
    uint32_t completedAgoMs;
    uint32_t pending[4];         // the set the sender is holding for the others (zero = none)
    uint32_t reserved;           // since 23:30: what activated the set, see kKind*
};
static_assert(sizeof(Packet) == 72, "Unexpected area wire layout");
// Who asked for the set, from the return address of StreamSet::Activate.
// 23:27 crash (dump 45712): the host, warped to a checkpoint by the debug
// menu, reported the LEVEL'S FIRST set (4F8B0CA8, activated by the level
// loader 0x0042DA6A) for as long as the load took, and the client, 0 m away,
// adopted it; a loading set activated mid-room drops the room. Only a set
// a door, a lift or the level script asked for is a room anybody stands in;
// the loader's set, the unknown and the local follows are never adopted.
enum : uint32_t { kKindUnknown = 0, kKindLevelStart = 1, kKindScript = 2, kKindDoor = 3, kKindLift = 4, kKindFollow = 5 };
uint32_t KindOfCaller(uint32_t ret) {
    switch (ret - base) {
    case 0x02DA6F: return kKindLevelStart;   // return addresses: the call sites + 5
    case 0x35A148: return kKindScript;
    case 0x9C5945: case 0x9D25A3: return kKindDoor;
    case 0xC0A314: case 0xC0A476: return kKindLift;
    default: return kKindUnknown;
    }
}
const char* KindName(uint32_t k) {
    static const char* names[] = {"unknown", "level_start", "script", "door", "lift", "follow"};
    return k < 6 ? names[k] : "?";
}
uint32_t lastActivationKind = kKindUnknown, areaKind = kKindUnknown;
bool Valid(const Packet& p) { return p.magic == kMagic && p.version == kVersion && p.token; }

// Local: the last completed set, and the activation that led to it.
uint32_t area[4]{};
DWORD areaAt = 0;
// The map the set was completed in. 23:21 crash (client, dump 44632): the
// host's first "area" of the run was the FRONTEND's stream set (ch04_asc,
// 31CB5374), still reported after ds_2 had loaded because no ds_2 set had
// completed yet; the client, spawned at 0 m from him, "adopted" it,
// activated a chapter-4 set inside chapter 1, and died in the animation
// system fifteen lines later on freed data. A set only counts while this machine is
// still on the map it was completed in.
char areaMap[64] = "";
volatile LONG areaWiped = 0;
bool GuidZero(const uint32_t* a);
bool AreaValid() {
    if (GuidZero(area)) return false;
    char map[64] = "", stream[64] = "";
    if (!level::CurrentMap(map, sizeof(map), stream, sizeof(stream))) return false;
    return _stricmp(map, areaMap) == 0;
}
void WipeStaleArea() {
    if (GuidZero(area) || AreaValid()) return;
    gate::Event("coop_streams area %08X-%08X-%08X-%08X belonged to map '%s': forgotten on the new map", area[0], area[1], area[2], area[3], areaMap);
    std::memset(area, 0, sizeof(area));
    areaAt = 0;
    areaMap[0] = 0;
    InterlockedIncrement(&areaWiped);
}
uint32_t activationHash = 0, activationFlags = 0;
uint32_t pendingHash = 0, pendingFlags = 0;   // a StreamSet activation seen, completion not yet
// What every peer reported, by slot.
struct PeerArea { uint32_t token, sequence; uint32_t area[4]; uint32_t pending[4]; DWORD at; uint32_t kind; uint32_t completedAgoMs; };
PeerArea peerAreas[coop_peers::kMaxPeers]{};
Packet host{};
DWORD hostAt = 0, lastSend = 0, mismatchSince = 0;
uint32_t sequence = 0, epoch = 0;
uint32_t lastLoggedArea[4]{};
bool noticeShown = false;
volatile LONG completions = 0, activations = 0, sent = 0, received = 0, mismatches = 0, matches = 0;

// The held activation (one at a time is the rule; a second request for a
// different set replaces the first: the newer wish is the player's).
safetyhook::InlineHook activateHook;
LONG releasing = 0;   // game thread only
struct Held { bool used; uint32_t node, actor, guid[4]; DWORD since, lastTry; unsigned members; uint32_t memberGuids[kMaxMembers][4]; uint32_t kind; };
Held held{};
volatile LONG activateCalls = 0, heldCount = 0, releasedCount = 0, timedOutCount = 0, passedCount = 0, replacedCount = 0, refusedMirrored = 0;
volatile LONG follows = 0, followsUnknown = 0;

bool GuidEqual(const uint32_t* a, const uint32_t* b) { return std::memcmp(a, b, 16) == 0; }
bool GuidZero(const uint32_t* a) { return !(a[0] | a[1] | a[2] | a[3]); }

// The member streams of a StreamSet node.
unsigned Members(uint32_t node, uint32_t out[kMaxMembers][4]) {
    uint32_t count = 0;
    if (!node || !Read(node + kNodeMemberCount, &count, 4) || count > kMaxMembers) return 0;
    unsigned n = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (Read(node + kNodeMembers + i * kNodeMemberStride, out[n], 16) && !GuidZero(out[n])) ++n;
    return n;
}
bool Contains(const uint32_t set[][4], unsigned count, const uint32_t* guid) {
    for (unsigned i = 0; i < count; ++i) if (GuidEqual(set[i], guid)) return true;
    return false;
}

// From the gate's dispatch hook, for every dispatch: a StreamSet activation
// (a message with a StreamSet listener) and a load completion.
void OnActivation(uint32_t hash, uint32_t flags) {
    pendingHash = hash;
    pendingFlags = flags;
    InterlockedIncrement(&activations);
}
void OnLoadComplete(uint32_t source) {
    uint32_t table = 0, object = 0, id = 0, guidPtr = 0, guid[4]{};
    if (!source || !Read(base + gate::kHandleTableRva, &table, 4) || !table) return;
    const uint32_t entry = table + (source >> 23) * 8;
    if (!Read(entry, &object, 4) || !Read(entry + 4, &id, 4) || id != source || !object) return;
    if (!Read(object + 8, &guidPtr, 4) || !guidPtr || !Read(guidPtr, guid, 16) || GuidZero(guid)) return;
    std::memcpy(area, guid, 16);
    areaAt = GetTickCount();
    {
        char map[64] = "", stream[64] = "";
        if (level::CurrentMap(map, sizeof(map), stream, sizeof(stream))) std::snprintf(areaMap, sizeof(areaMap), "%s", map);
        else areaMap[0] = 0;
    }
    activationHash = pendingHash;
    activationFlags = pendingFlags;
    areaKind = lastActivationKind;
    InterlockedIncrement(&completions);
    if (gate::trace) gate::Event("coop_streams area completed %08X-%08X-%08X-%08X activation=%08X asked_by=%s", guid[0], guid[1], guid[2], guid[3], activationHash, KindName(areaKind));
}

bool NewStreamReport(unsigned slot, uint32_t token, uint32_t seq, uint32_t previousToken, uint32_t previousSeq) {
    if (slot >= coop_peers::kMaxPeers || !peerTable.peers[slot].used || peerTable.peers[slot].token != token || !token) return false;
    const uint32_t advance = seq - previousSeq;
    return token != previousToken || (advance && advance < 0x80000000u);
}
void OnPacket(const Packet& p, unsigned slot) {
    if (!installed || !enabled || slot >= coop_peers::kMaxPeers) return;
    if (!NewStreamReport(slot, p.token, p.sequence, peerAreas[slot].token, peerAreas[slot].sequence)) return;
    InterlockedIncrement(&received);
    if (slot < coop_peers::kMaxPeers) {
        PeerArea& a = peerAreas[slot];
        a.token = p.token; a.sequence = p.sequence;
        std::memcpy(a.area, p.area, 16);
        std::memcpy(a.pending, p.pending, 16);
        a.at = Stamp(GetTickCount());
        a.kind = p.reserved;
        a.completedAgoMs = p.completedAgoMs;
    }
    // Clients are meshed: only machine 0's packet is the host's.
    if (!IsHost() && slot < coop_peers::kMaxPeers && peerTable.peers[slot].used && peerTable.peers[slot].machine == 0) {
        host = p;
        hostAt = Stamp(GetTickCount());
    }
}

// The host's current set, as he last reported it (machine 0 in the peer table).
bool HostArea(DWORD now, uint32_t out[4]) {
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        if (!peer.used || !peer.token || peer.machine != 0 || now - peer.lastSeen > 10000) continue;
        const PeerArea& a = peerAreas[s];
        if (!a.at || TicksSince(now, a.at) > kStaleMs || GuidZero(a.area)) return false;
        std::memcpy(out, a.area, 16);
        return true;
    }
    return false;
}

// A partner who asks for the same set right now (holding it himself, or
// already in it) moves along. 24.09.: the dementia land (4FCB505E) was
// asked for by both machines at the same moment of the followed scene; each
// held it because the other still reported the old set, both waited, the
// camera sat in the unloaded floor for 60 s (host timeout) and forever on
// the client. The hold is for a partner who stays behind, not one who comes.
volatile LONG movedTogether = 0;
bool PartnerComesToo(const uint32_t guid[4], DWORD now) {
    if (!guid || GuidZero(guid)) return false;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        if (!peer.used || !peer.token || now - peer.lastSeen > 10000) continue;
        const PeerArea& a = peerAreas[s];
        if (!a.at || TicksSince(now, a.at) > kStaleMs) continue;
        if (GuidEqual(a.pending, guid) || GuidEqual(a.area, guid)) return true;
    }
    return false;
}
// Client only: would activating `members` drop a stream that the current set
// and the host's set both hold (the room shared with him)?
volatile LONG refusedNear = 0;   // held next to the reference (the name is older than the hold)
bool NearReference(DWORD now, uint32_t set[4], float* distance);
bool Drops(const uint32_t members[][4], unsigned count, const uint32_t* set, uint32_t dropped[4]);
bool PartnersFit(const uint32_t members[][4], unsigned count, DWORD now, char* who, size_t whoSize) {
    // Next to the reference partner, an activation that would drop one of
    // his streams WAITS (23:37: it was refused outright, and the client's
    // door (its open set asked for a second before the host's own report
    // of the same set arrived) never asked again: state 2 forever, no vote,
    // both players at one door, nobody opening). Held, it is retried every
    // quarter second and goes through as soon as his set fits, which for a
    // door both players walk through is the next second.
    uint32_t referenceSet[4]{}, dropped[4]{};
    if (NearReference(now, referenceSet, nullptr) && count && Drops(members, count, referenceSet, dropped)) {
        InterlockedIncrement(&refusedNear);
        std::snprintf(who, whoSize, "next to the reference partner, his stream %08X-%08X would go", dropped[0], dropped[1]);
        return false;
    }
    if (IsHost()) return true;                          // the host is the reference, never held
    uint32_t hostSet[4]{};
    if (!HostArea(now, hostSet)) return true;           // no word from the host: nothing to judge
    if (GuidZero(area)) return true;                    // no local set yet
    const uint32_t hostNode = mirror::ObjectByGuid(hostSet), ourNode = mirror::ObjectByGuid(area);
    if (!hostNode || !ourNode) return true;
    uint32_t theirs[kMaxMembers][4]{}, ours[kMaxMembers][4]{};
    const unsigned n = Members(hostNode, theirs), m = Members(ourNode, ours);
    for (unsigned i = 0; i < n; ++i) {
        if (!Contains(ours, m, theirs[i])) continue;        // not shared here: nothing to keep
        if (Contains(members, count, theirs[i])) continue;  // kept: fine
        std::snprintf(who, whoSize, "host in %08X-%08X, shared stream %08X-%08X would go", hostSet[0], hostSet[1], theirs[i][0], theirs[i][1]);
        return false;
    }
    return true;
}

// Compare completed sets using the same membership rule on both machines.
bool SetCovers(const uint32_t* completed, const uint32_t* required) {
    if (GuidZero(required) || GuidEqual(completed, required)) return true;
    if (GuidZero(completed)) return false;
    const uint32_t ownNode = mirror::ObjectByGuid(completed);
    const uint32_t requiredNode = mirror::ObjectByGuid(required);
    if (!ownNode || !requiredNode) return false;
    uint32_t own[kMaxMembers][4]{}, need[kMaxMembers][4]{};
    const unsigned count = Members(ownNode, own), requiredCount = Members(requiredNode, need);
    uint32_t declared = 0;
    if (!Read(requiredNode + kNodeMemberCount, &declared, 4) || declared != requiredCount) return false;
    for (unsigned i = 0; i < requiredCount; ++i)
        if (!Contains(own, count, need[i])) return false;
    return true;
}
bool Covers(const uint32_t* required) { return SetCovers(area, required); }
bool PeerSet(unsigned slot, uint32_t out[4]) {
    if (slot >= coop_peers::kMaxPeers) return false;
    const PeerArea& a = peerAreas[slot];
    if (!a.at || GetTickCount() - a.at > kStaleMs || GuidZero(a.area)) return false;
    std::memcpy(out, a.area, 16);
    return true;
}
// The initiator and every connected peer must cover the set captured at the cue.
bool SceneReady(const uint32_t* required, DWORD now, char* why, size_t size) {
    if (GuidZero(required)) return true;
    if (!Covers(required)) {
        std::snprintf(why, size, "local set lacks scene set %08X-%08X", required[0], required[1]);
        return false;
    }
    for (unsigned slot = 0; slot < coop_peers::kMaxPeers; ++slot) {
        const auto& peer = peerTable.peers[slot];
        if (!peer.used || !peer.token || TicksSince(now, peer.lastSeen) > 10000) continue;
        uint32_t completed[4]{};
        if (PeerSet(slot, completed) && SetCovers(completed, required)) continue;
        std::snprintf(why, size, "peer %u set %08X-%08X needs %08X-%08X", slot, completed[0], completed[1], required[0], required[1]);
        return false;
    }
    return true;
}
// A partner's set worth adopting: asked for by a door, a lift or the level
// script (a room he walked into), and his for at least three seconds.
bool PeerRoom(unsigned slot, uint32_t out[4], const char** whyNot) {
    if (!PeerSet(slot, out)) { *whyNot = "no set"; return false; }
    const PeerArea& a = peerAreas[slot];
    // A necro at the vents also takes the room a human followed into (he has no walk of his own).
    const bool followed = a.kind == kKindFollow && GhostFollower();
    if (a.kind != kKindDoor && a.kind != kKindLift && a.kind != kKindScript && !followed) { *whyNot = KindName(a.kind); return false; }
    if (a.completedAgoMs + (GetTickCount() - a.at) < 3000) { *whyNot = "too fresh"; return false; }
    return true;
}
// Activate a partner's set here, so that a sequence that pulls the local player to him
// plays in a loaded room. StreamSet::Activate takes an optional actor and
// runs its own no-actor path (0x00759DD5) when it is 0; it returns at once
// when the set is already the active one (+0x58). Called through the
// original, so neither the partner rule nor the mirrored-activation refusal
// judges it: it IS the following.
// Set while a partner's set is activated for his scene: the zone set's switch
// then keeps the local rooms too (pin::OnSetDesired). 25.09.: the host up front
// had a small hallucination, the client behind him followed it, adopted the
// host's set and his own room unloaded under him; he fell through the map.
bool followingNow = false;
// The set a scene follow replaced, and where our Isaac stood then. 12.10.: the
// client opened a door into his scene far ahead, the host followed the scene's
// set without ever being carried there and stood in an empty room. When the
// scene is over and our Isaac has neither moved nor stands by a partner, the
// set from before comes back (RevertTick); a carried player keeps the new one.
uint32_t setBeforeFollow[4]{};
float posBeforeFollow[3]{};
DWORD followBeganAt = 0, followSceneEndAt = 0;
volatile LONG followsReverted = 0, followsKept = 0;
// A door's open set for a living necro, who opens no door himself (17.10.: the rooms the humans
// opened never loaded on his machine once he stopped opening doors). A plain activation: none of
// the follow's bookkeeping, nothing to give back after a scene.
volatile LONG necroDoorSets = 0;
// While set, the zone set's switch keeps the members of our current set (pin::OnSetDesired):
// a door's room comes in, the room our Isaac stands in stays. keptOwnLast counts them.
bool keepOwnNow = false;
unsigned keptOwnLast = 0;
bool farDoorGuard = true, doorApproach = true;   // [Streams] FarDoorGuard, DoorApproachLoads (below)
bool ActivateDoorSet(const uint32_t* guid, bool keepOwn = false) {
    const uint32_t node = mirror::ObjectByGuid(guid);
    if (!node) return false;
    bool activated = true;
    ++releasing;
    lastActivationKind = kKindDoor;
    keepOwnNow = keepOwn;
    keptOwnLast = 0;
    __try {
        activateHook.thiscall<void>(reinterpret_cast<void*>(node), 0u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        activated = false;
        gate::Event("coop_streams door set faulted node=%08X", node);
    }
    keepOwnNow = false;
    --releasing;
    if (!activated) return false;
    if (!keepOwn) InterlockedIncrement(&necroDoorSets);
    return true;
}
bool Follow(const uint32_t* guid) {
    const uint32_t node = mirror::ObjectByGuid(guid);
    if (!node) { InterlockedIncrement(&followsUnknown); return false; }
    if (GuidZero(setBeforeFollow) && !GuidZero(area) && !GuidEqual(area, guid) && gate::LocalPosition(posBeforeFollow)) {
        std::memcpy(setBeforeFollow, area, 16);
        followBeganAt = GetTickCount();
        followSceneEndAt = 0;
    }
    followingNow = true;
    bool activated = true;
    ++releasing;
    lastActivationKind = kKindFollow;   // the trampoline skips OnActivate, which would set it
    __try {
        activateHook.thiscall<void>(reinterpret_cast<void*>(node), 0u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        activated = false;
        gate::Event("coop_streams follow faulted node=%08X", node);
    }
    --releasing;
    followingNow = false;
    if (!activated) return false;
    InterlockedIncrement(&follows);
    gate::Event("coop_streams following a partner's set %08X-%08X-%08X-%08X for his sequence", guid[0], guid[1], guid[2], guid[3]);
    return true;
}

// 22:50, the teleport: a cutscene pulls a player into its room, the
// door shuts, and his game still holds the streams of the room he came from:
// it never saw him walk through a door, so nothing asked for the new
// room, and the door, closing behind nobody, may even ask for the OLD one.
// So the streams follow the BODY: whoever is standing next to the reference
// player has to hold the reference's streams. The reference is the host,
// or for a minute the partner whose sequence cue was just
// replayed here (he is where the cutscene is, whoever he is). Standing within
// FollowReach of him with a set that lacks streams of his, the local machine
// activates his set; and a local activation that would drop one of his streams
// while standing next to him is held (see PartnersFit): a door that remembers the
// wrong side is not the player's wish. A mirrored activation, on the other
// hand, is let through next to him: his cues are moving the local player, that
// is the point.
bool followPartners = true;
float followReach = 15.0f;
unsigned referenceSlot = 0xFF;     // 0xFE = this machine (it fired the sequence)
DWORD referenceUntil = 0, lastNearFollow = 0;
constexpr DWORD kReferenceMs = 60000, kNearFollowMs = 3000;
volatile LONG followsNear = 0, allowedNear = 0;
void SetReference(unsigned slot, DWORD now) { referenceSlot = slot; referenceUntil = now + kReferenceMs; }
// The reference partner's set, if he is one and has reported a set.
bool ReferenceSet(DWORD now, unsigned& slot, uint32_t out[4]) {
    // A necro at the vents has no body of its own here: the first human partner with a set
    // is the reference (invasion, 16.10.), whoever hosts.
    if (GhostFollower()) {
        // a dead human watching: the partner he watches first
        const unsigned watched = spectate::SubjectSlot();
        if (watched < coop_peers::kMaxPeers && PeerSet(watched, out)) { slot = watched; return true; }
        for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
            const auto& peer = peerTable.peers[s];
            if (!peer.used || !peer.token || now - peer.lastSeen > 10000 || roles::IsNecro(s, now)) continue;
            if (PeerSet(s, out)) { slot = s; return true; }
        }
        return false;
    }
    if (referenceUntil && now < referenceUntil) {
        if (referenceSlot == 0xFE) return false;   // this machine leads
        slot = referenceSlot;
        return PeerSet(slot, out);
    }
    if (IsHost()) return false;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        if (!peer.used || !peer.token || peer.machine != 0 || now - peer.lastSeen > 10000) continue;
        slot = s;
        return PeerSet(s, out);
    }
    return false;
}
bool NearReference(DWORD now, uint32_t set[4], float* distance) {
    unsigned slot = 0xFF;
    if (!followPartners || !ReferenceSet(now, slot, set) || slot >= coop_peers::kMaxPeers) return false;
    if (GhostFollower()) {   // the ghost stands where the humans are
        if (distance) *distance = 0.0f;
        return true;
    }
    const auto& runtime = peerRuntime[slot];
    if (!runtime.lastState || now - runtime.lastState > 5000) return false;
    float me[3]{};
    if (!gate::LocalPosition(me)) return false;
    const float d2 = coop_gate::DistanceSquared(me, runtime.pos);
    if (distance) *distance = std::sqrt(d2);
    return d2 <= followReach * followReach;
}
// Would `members` drop a stream of `set`?
bool Drops(const uint32_t members[][4], unsigned count, const uint32_t* set, uint32_t dropped[4]) {
    const uint32_t node = mirror::ObjectByGuid(set);
    uint32_t theirs[kMaxMembers][4]{};
    const unsigned n = node ? Members(node, theirs) : 0;
    for (unsigned i = 0; i < n; ++i)
        if (!Contains(members, count, theirs[i])) { std::memcpy(dropped, theirs[i], 16); return true; }
    return false;
}
// 24.09., the Tormentor (ch12): the leader's scene loads its own set
// (streamset_start_igc_03_0, asked_by=script) the moment it begins, and the
// follower played it in his level-start set without the scene's streams. A
// follower takes the leader's set at once, wherever his body stands and
// however fresh the set is: the scene is what it shows.
volatile LONG sceneFollows = 0;
uint32_t sceneFollowed[4]{};
void SceneTick(DWORD now) {
    if (!followPartners || !inLevel || !levelSettled || !cutscene::Following()) { std::memset(sceneFollowed, 0, 16); return; }
    unsigned slot = 0xFF;
    uint32_t set[4]{};
    if (!ReferenceSet(now, slot, set) || slot >= coop_peers::kMaxPeers) return;
    // A partner's level-start or unknown set is no room of his scene (dumps 44632, 45712).
    if (peerAreas[slot].kind == kKindLevelStart || peerAreas[slot].kind == kKindUnknown) return;
    if (GuidEqual(set, sceneFollowed) || Covers(set)) return;
    std::memcpy(sceneFollowed, set, 16);
    InterlockedIncrement(&sceneFollows);
    gate::Event("coop_streams following partner %u's scene: adopting his set %08X-%08X (asked by %s)", slot, set[0], set[1], KindName(peerAreas[slot].kind));
    Follow(set);
}
constexpr DWORD kLevelStartSettleMs = 10000;
void RevertTick(DWORD now) {
    if (GuidZero(setBeforeFollow)) return;
    if (GhostFollower()) { std::memset(setBeforeFollow, 0, 16); return; }   // a ghost's own set is no room to go back to
    if (!inLevel || !levelSettled) { std::memset(setBeforeFollow, 0, 16); return; }
    if (cutscene::Following() || cutscene::SequenceRunningNow() || now - followBeganAt < 5000) { followSceneEndAt = 0; return; }
    if (!followSceneEndAt) { followSceneEndAt = Stamp(now); return; }
    if (now - followSceneEndAt < 2000) return;
    float me[3]{};
    const bool here = gate::LocalPosition(me);
    const bool stayed = here && coop_gate::DistanceSquared(me, posBeforeFollow) < 6.0f * 6.0f;
    bool byPartner = false;
    for (unsigned s = 0; s < coop_peers::kMaxPeers && here; ++s) {
        const auto& peer = peerTable.peers[s];
        const auto& runtime = peerRuntime[s];
        if (!peer.used || !peer.token || !runtime.lastState || now - runtime.lastState > 5000) continue;
        byPartner = byPartner || coop_gate::DistanceSquared(me, runtime.pos) <= followReach * followReach;
    }
    uint32_t before[4]{};
    std::memcpy(before, setBeforeFollow, 16);
    std::memset(setBeforeFollow, 0, 16);
    if (!stayed || byPartner || GuidEqual(area, before)) {
        InterlockedIncrement(&followsKept);
        if (followsKept <= 50)
            gate::Event("coop_streams scene over: the followed set stays (%s)", !stayed ? "our Isaac was carried or walked on" : byPartner ? "we stand by a partner" : "ours again anyway");
        return;
    }
    const uint32_t node = mirror::ObjectByGuid(before);
    if (!node) { InterlockedIncrement(&followsUnknown); return; }
    ++releasing;
    lastActivationKind = kKindFollow;
    __try {
        activateHook.thiscall<void>(reinterpret_cast<void*>(node), 0u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gate::Event("coop_streams return to our set faulted node=%08X", node);
    }
    --releasing;
    InterlockedIncrement(&followsReverted);
    gate::Event("coop_streams scene over, our Isaac was not carried to it: back to our own set %08X-%08X", before[0], before[1]);
}
void NearTick(DWORD now) {
    SceneTick(now);
    RevertTick(now);
    if (!followPartners || !inLevel || !levelSettled || now - lastNearFollow < kNearFollowMs) return;
    if (GuidZero(area)) return;   // no local set on this map yet: nothing to compare, nothing to adopt
    // The local set is not a room yet (still loading). A level-start set stayed "not a
    // room" for good before, so a client that loaded into the level never
    // adopted anything (24.09., ch12).
    if (areaKind == kKindUnknown || (areaKind == kKindLevelStart && TicksSince(now, areaAt) < kLevelStartSettleMs)) return;
    uint32_t set[4]{};
    float distance = 0.0f;
    if (!NearReference(now, set, &distance) || Covers(set)) return;
    unsigned slot = 0xFF;
    uint32_t room[4]{};
    const char* whyNot = "";
    if (!ReferenceSet(now, slot, set) || !PeerRoom(slot, room, &whyNot)) {
        static DWORD lastSaid = 0;
        if (now - lastSaid > 10000) { lastSaid = now; gate::Event("coop_streams next to the reference partner (%.0f m) without his streams, but his set is not a room to adopt (%s)", distance, whyNot); }
        return;
    }
    lastNearFollow = now;
    InterlockedIncrement(&followsNear);
    gate::Event("coop_streams next to the reference partner (%.0f m) without his streams: adopting his set (asked by %s)", distance, KindName(peerAreas[slot].kind));
    Follow(room);
}

// Door leave sets. A StandardDoor carries four stream sets in its attributes
// (door+0x0C): open front/back at +0xC0/+0xE0 and LEAVE front/back at
// +0xD0/+0xF0. When it has shut, LoadLeaveStreamSet (PC 0x00DC5870, both call
// sites push the door as the actor of StreamSet::Activate) loads the leave set
// of the side its user stands on (EntityOnFrontSideOfDoor, 0x00DAE110); a
// cancelled opening (tick, 0x00DD9F53) loads the side its user came from
// (+0x259). Neither knows about a cutscene that pulled the player through the
// door: the room the cutscene plays in was unloaded under him (23.09., the
// old DoorSetFor never ran in any logged session). So the leave set is chosen
// here, from where the players are:
//   * a partner's cutscene is running (reference window) and he is on the
//     other side of this door: the leave waits, both rooms stay loaded, until
//     the game's own side test puts the local body on his side (the cutscene
//     pulled it through) or DoorLeaveHoldMs is over, then the local side;
//   * otherwise everybody near the door on one side: that side; players on
//     both sides: the local side by the game's own test. The room the local body is
//     in is never the one dropped.
namespace leave {
bool enabled = true;
DWORD holdMs = 15000;
constexpr uint32_t kDescriptor = 0x0C, kLeaveSide1 = 0xD0, kLeaveSide0 = 0xF0;
constexpr DWORD kRetryMs = 250;
int frontSign = 0;   // +1: a positive offset along the door's forward is the game's side 1
struct Hold { bool used; uint32_t door; uint32_t sets[2][4]; int requested; DWORD since, lastTry; };
Hold hold{};
bool reissuing = false;
volatile LONG seen = 0, kept = 0, swapped = 0, held = 0, released = 0, timedOut = 0, reused = 0, unreadable = 0, calibrations = 0;

bool Sets(uint32_t door, uint32_t out[2][4]) {
    uint32_t desc = 0;
    if (!door || !Read(door + kDescriptor, &desc, 4) || !desc) return false;
    return Read(desc + kLeaveSide1, out[1], 16) && Read(desc + kLeaveSide0, out[0], 16);
}
int SideOfSet(uint32_t node, const uint32_t sets[2][4]) {
    uint32_t guid[4]{};
    if (!node || !Read(node + kNodeGuid, guid, 16) || GuidZero(guid)) return -1;
    if (GuidEqual(guid, sets[1])) return 1;
    if (GuidEqual(guid, sets[0])) return 0;
    return -1;
}
bool Along(uint32_t door, const float* p, float& along) {
    float pos[3]{}, fwd[3]{};
    if (!Read(door + placement::kPlayerPosition, pos, 12) || !Read(door + placement::kPlayerForward, fwd, 12)) return false;
    along = (p[0] - pos[0]) * fwd[0] + (p[1] - pos[1]) * fwd[1] + (p[2] - pos[2]) * fwd[2];
    return true;
}
// The local side by the game's own plane test; also learns which sign of the plane
// offset that test calls side 1, so partners can be placed from positions.
int LocalSide(uint32_t door) {
    if (GhostFollower()) return -1;   // a necro at the vents stands on no side of a door
    const uint32_t body = LocalSlotZeroEntity();
    bool side = false;
    if (!body || !worldcalls::doorSideOk || !worldcalls::DoorSide(door, body, side)) return -1;
    float me[3]{}, along = 0.0f;
    if (gate::LocalPosition(me) && Along(door, me, along) && std::fabs(along) >= 0.75f) {
        const int sign = ((along > 0.0f) == side) ? 1 : -1;
        if (sign != frontSign) { frontSign = sign; InterlockedIncrement(&calibrations); }
    }
    return side ? 1 : 0;
}
// A partner's side: the game's own test on his ghost body when there is one
// (the test builds its plane from the door's own frame, door+0x348 and
// attributes+0x160, not from the entity's forward row); otherwise his reported
// position against the plane calibrated above.
volatile LONG ghostTests = 0, planeTests = 0;
int PeerSide(uint32_t door, unsigned slot, DWORD now) {
    if (slot >= coop_peers::kMaxPeers) return -1;
    const auto& runtime = peerRuntime[slot];
    if (!runtime.lastState || now - runtime.lastState > 5000) return -1;
    if (worldcalls::doorSideOk) {
        const uint32_t ghost = GhostForMachine(peerTable.peers[slot].machine);
        bool side = false;
        if (ghost && worldcalls::DoorSide(door, ghost, side)) { InterlockedIncrement(&ghostTests); return side ? 1 : 0; }
    }
    if (!frontSign) return -1;
    InterlockedIncrement(&planeTests);
    float along = 0.0f;
    if (!Along(door, runtime.pos, along) || std::fabs(along) < 0.25f) return -1;   // in the doorway: no side
    return ((along > 0.0f) == (frontSign > 0)) ? 1 : 0;
}
// The side whose leave set loads, -1 to wait, kNoOpinion to leave the
// door's own choice alone. `why` names the rule.
constexpr int kNoOpinion = -2;
int Decide(uint32_t door, DWORD now, bool mayWait, const char** why) {
    // The local body's side comes from the game's test and is never overruled:
    // the room the local player stands in is never the one dropped. During a partner's
    // cutscene a door between the two drops nothing until the cutscene has pulled
    // the local player to his side (or the wait is over).
    const int local = LocalSide(door);
    if (referenceUntil && now < referenceUntil && local >= 0) {
        if (referenceSlot == 0xFE) { *why = "our cutscene: our side"; return local; }
        const int theirs = PeerSide(door, referenceSlot, now);
        if (theirs == local) { *why = "cutscene partner on our side"; return local; }
        if (theirs >= 0 && mayWait) { *why = "cutscene partner on the other side: both rooms stay until we are pulled to him"; return -1; }
    }
    if (local < 0) { *why = "our side unknown: the door's own choice"; return kNoOpinion; }
    int sides[2] = {0, 0};
    if (local >= 0) ++sides[local];
    float doorPos[3]{};
    const bool havePos = Read(door + placement::kPlayerPosition, doorPos, 12);
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        if (!peer.used || !peer.token || now - peer.lastSeen > 10000) continue;
        if (havePos && coop_gate::DistanceSquared(peerRuntime[s].pos, doorPos) > worldcalls::doorReach * worldcalls::doorReach) continue;
        const int side = PeerSide(door, s, now);
        if (side >= 0) ++sides[side];
    }
    if (sides[0] && !sides[1]) { *why = "everybody near the door on side 0"; return 0; }
    if (sides[1] && !sides[0]) { *why = "everybody near the door on side 1"; return 1; }
    *why = "players on both sides: our side";
    return local;
}
volatile LONG decisionLines = 0;
void LogDecision(uint32_t door, int requested, int chosen, const char* why, const char* what) {
    if (InterlockedIncrement(&decisionLines) > 100) return;
    float doorPos[3]{}, me[3]{};
    const float distance = Read(door + placement::kPlayerPosition, doorPos, 12) && gate::LocalPosition(me) ? std::sqrt(coop_gate::DistanceSquared(me, doorPos)) : -1.0f;
    gate::Event("coop_streams door %08X leave set: asked side %d, loading side %d (%s)%s our Isaac %.0f m from the door, front_sign=%d reference=%u",
                door, requested, chosen, why, what, distance, frontSign, referenceSlot);
}
}  // namespace leave

// Activate a door's leave set on the door's behalf, through the Activate hook, so
// the partner rules still judge it; the door decision itself is skipped.
void ActivateLeave(uint32_t node, uint32_t door) {
    using Activate = void(__thiscall*)(void*, uint32_t);
    leave::reissuing = true;
    __try {
        reinterpret_cast<Activate>(base + kActivateRva)(reinterpret_cast<void*>(node), door);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gate::Event("coop_streams door leave activation faulted node=%08X door=%08X", node, door);
    }
    leave::reissuing = false;
}
bool ReleaseLeave(DWORD now, bool force, const char* cause) {
    if (!leave::hold.used) return false;
    const char* why = "";
    int side = leave::Decide(leave::hold.door, now, !force, &why);
    if (side == -1 && !force) return false;
    if (side < 0) side = leave::hold.requested;
    const leave::Hold h = leave::hold;
    leave::hold = leave::Hold{};
    const uint32_t node = mirror::ObjectByGuid(h.sets[side]);
    leave::LogDecision(h.door, h.requested, side, why, cause);
    if (node) ActivateLeave(node, h.door);
    return true;
}
// A checkpoint restart puts everybody back: a held leave, and a held set
// activation (its node is freed with the old world), belong to the old world.
namespace curtain { void MarkLoad(const char* why); }
namespace pin { void ForgetDesired(); }
void OnRestartLeave() { pin::ForgetDesired(); leave::hold = leave::Hold{}; held = Held{}; noticeShown = false; curtain::MarkLoad("a checkpoint restart"); }
// From worldcalls: a door whose leave is held is being used again. Somebody
// stands at it, so the cutscene is over for this door: the leave is decided
// now, before the opening asks for its open set.
void OnDoorReuse(uint32_t door) {
    if (!leave::hold.used || leave::hold.door != door) return;
    InterlockedIncrement(&leave::reused);
    ReleaseLeave(GetTickCount(), true, " [door used again]");
}
void LeaveTick(DWORD now) {
    if (!leave::hold.used) return;
    if (!inLevel) { leave::hold = leave::Hold{}; return; }
    if (now - leave::hold.lastTry < leave::kRetryMs) return;
    leave::hold.lastTry = now;
    if (now - leave::hold.since >= leave::holdMs) {
        InterlockedIncrement(&leave::timedOut);
        ReleaseLeave(now, true, " [TIMEOUT]");
        return;
    }
    if (ReleaseLeave(now, false, " [after waiting]")) InterlockedIncrement(&leave::released);
}
// Returns the node to activate for a door's activation, or 0 when the leave
// waits (see leave::Decide).
uint32_t DoorActivation(uint32_t node, uint32_t door) {
    if (!leave::enabled || leave::reissuing || !door || !peerTable.Count()) return node;
    uint32_t sets[2][4]{};
    if (!leave::Sets(door, sets)) { InterlockedIncrement(&leave::unreadable); return node; }
    const int requested = leave::SideOfSet(node, sets);
    if (requested < 0) return node;   // an open set, not a leave
    InterlockedIncrement(&leave::seen);
    const DWORD now = GetTickCount();
    const char* why = "";
    const int side = leave::Decide(door, now, true, &why);
    if (side == leave::kNoOpinion) { leave::LogDecision(door, requested, requested, why, " [the door's own choice]"); return node; }
    if (side < 0) {
        if (leave::hold.used && leave::hold.door != door) ReleaseLeave(now, true, " [replaced by another door]");
        leave::hold = leave::Hold{true, door, {}, requested, now, now};
        std::memcpy(leave::hold.sets, sets, sizeof(sets));
        InterlockedIncrement(&leave::held);
        leave::LogDecision(door, requested, -1, why, " [held, both rooms stay loaded]");
        return 0;
    }
    if (side == requested) { InterlockedIncrement(&leave::kept); leave::LogDecision(door, requested, side, why, " [kept]"); return node; }
    const uint32_t other = mirror::ObjectByGuid(sets[side]);
    if (!other) return node;
    InterlockedIncrement(&leave::swapped);
    leave::LogDecision(door, requested, side, why, " [swapped]");
    return other;
}

// Stream visibility follows the player who walked.
//
// A loaded stream can still be hidden: its IS_VISIBLE flag (stream+0x54 bit
// 0x100) decides whether the room is drawn, and the game flips it through
// ShowStream / HideStream (0x007C1270 / 0x007C12F0, thiscall on the
// visibility manager [0x02021498], (handle), ret 4: they tell the stream's
// entities (iMsgStreamVisible/Hidden) and set the flag through
// StreamManager::SetVisible 0x0074ACE0). The level script does that as the
// player walks from room to room. 23.09., twice, live on both machines after
// a cutscene had pulled the client into the next room: the same streams loaded
// everywhere, but the client still showed the room he came from
// (observation_hall_z01) and hid the two he now stood in (observation_z01,
// recreation_z02): a black room with the players in it. He never walked
// through the triggers that switch them. The pull is not a jump: in the second
// run no tick moved him more than 4 m, the players ended 0.2 m apart.
//
// So the one who was carried takes the rooms of the one who was not, once,
// right after the scene. "Carried" is decided with the doors, the same way the
// game decides it for the leave set: before a scene, for every door used
// this level, the side the local body stands on is noted (EntityOnFrontSideOfDoor).
// The local side of a door flipping means the player went through it; the flip
// counts as a carry when the door was open then, it happened in a scene (or just
// before one began), and the local picture has not changed since (a walker's triggers
// switch it). No distances: the test is an endless plane, but a closed door
// cannot be passed, so a far door's plane swept by the scene does not count.
// Then a partner who was in the scene too, was not carried himself and stands
// on the local player's new side of that door is in the same room, and his picture is taken
// (several: the nearest). Carried both or neither: nothing happens. His
// picture is followed while he reports from inside the scene (23.09.: at the
// carry it was still the old one, his triggers switched a moment later), and
// it is over when the local scene has ended or the local picture changes by itself.
namespace vis {
bool enabled = true;
constexpr uint32_t kMagic = 0x49534956;   // 'VISI'
constexpr uint32_t kVersion = 6;   // 5: the sender's scene kind (runtime_coop_scenekind.inc); 6: its last scripted teleport
constexpr unsigned kMax = 40;
constexpr uint32_t kShowRva = 0x3C1270, kHideRva = 0x3C12F0, kVisibilityManager = 0x1C21498, kVisibleFlag = 0x100;
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr DWORD kSceneTailMs = 3000, kPreSceneMs = 1000, kOpenGraceMs = 2000, kSettleMs = 3000, kInScenePacketMs = 1500;
constexpr uint32_t kDoorStateOffset = 0x234, kDoorOpeningState = 4, kDoorOpenState = 5;   // as in runtime_coop_worldcalls
constexpr DWORD kCarryWindowMs = 30000, kSendMs = 1000, kApplyMs = 500, kFreshMs = 3000;
struct Packet {
    uint32_t magic, version, token, sequence, sequenceAgoMs, carriedAgoMs;
    uint32_t area[4];
    uint32_t count;
    uint32_t hashes[kMax];
    unsigned char visible[kMax];
    uint32_t sceneKind;
    uint32_t teleportSerial;   // runtime_coop_teleport.inc
    float teleportTo[3];
};
static_assert(sizeof(Packet) == 264, "Unexpected visibility wire layout");
bool Valid(const Packet& p) { return p.magic == kMagic && p.version == kVersion && p.token && p.count <= kMax; }
struct PeerVis { DWORD at; Packet packet; };
PeerVis peers[coop_peers::kMaxPeers]{};
DWORD lastSend = 0, lastApply = 0;
DWORD sequenceSeenAt = 0;   // set by runtime_coop_cutscene while the local Isaac runs a scripted sequence
uint32_t sequence = 0, signature = 0;
// The carry: when, through which door, onto which side, and the local picture then.
DWORD carriedAt = 0;
bool carryOpen = false;
uint32_t carryDoor = 0, carrySignature = 0;
bool carrySide = false;
unsigned carryTook = 0;
volatile LONG changes = 0, sent = 0, received = 0, shown = 0, hidden = 0, faults = 0,
              carries = 0, adoptions = 0, alreadySame = 0, walkedAway = 0, expired = 0, partnerCarried = 0, partnerOtherSide = 0,
              flips = 0, flipsClosed = 0, flipsWalked = 0, badTeleports = 0;
// Every door used in this level (runtime_coop_worldcalls reports each
// transition), with the vtable it had, so a freed one is never asked, the side
// the local body stands on (-1 unknown), when it was last seen open, and the last
// local flip through it with the local picture from just before.
struct Door { uint32_t door, vtable; signed char side; DWORD openAt, flippedAt; uint32_t flipSignature; };
constexpr unsigned kDoors = 64;
Door doors[kDoors]{};
unsigned doorCount = 0;
DWORD sceneFrom = 0;
uint32_t watchedSignature = 0;
// A transition means the door is opening, open or just closing.
void NoteDoor(uint32_t door) {
    uint32_t vtable = 0;
    if (!door || !Read(door, &vtable, 4)) return;
    const DWORD now = GetTickCount();
    for (unsigned i = 0; i < doorCount; ++i) if (doors[i].door == door) { doors[i].vtable = vtable; doors[i].openAt = now; return; }
    const Door fresh{door, vtable, -1, now, 0, 0};
    if (doorCount < kDoors) doors[doorCount++] = fresh;
    else { std::memmove(doors, doors + 1, sizeof(Door) * (kDoors - 1)); doors[kDoors - 1] = fresh; }
}
bool DoorAlive(const Door& d) {
    uint32_t vtable = 0;
    return Read(d.door, &vtable, 4) && vtable == d.vtable;
}

uint32_t NameHash(const char* name) {
    uint32_t h = 2166136261u;
    for (; *name; ++name) h = (h ^ static_cast<unsigned char>(*name >= 'A' && *name <= 'Z' ? *name + 32 : *name)) * 16777619u;
    return h;
}
bool LevelStream(const char* name) {
    return name[0] && _strnicmp(name, "global_", 7) != 0 && _strnicmp(name, "text_", 5) != 0 &&
           _strnicmp(name, "randomizer\\", 11) != 0 && _strnicmp(name, "frontend", 8) != 0;
}
// The local level streams: {handle, stream, name hash}, from the StreamManager's
// handle table (+0x5C buckets, +0x60 count, nodes {handle, stream, next}).
struct Local { uint32_t handle, stream, hash; };
unsigned LocalStreams(Local* out, unsigned max) {
    uint32_t manager = 0, table = 0, buckets = 0;
    if (!Read(base + kStreamManager, &manager, 4) || !manager || !Read(manager + 0x5C, &table, 4) ||
        !Read(manager + 0x60, &buckets, 4) || !table || !buckets || buckets > 4096) return 0;
    unsigned n = 0;
    for (uint32_t b = 0; b < buckets && n < max; ++b) {
        uint32_t node = 0;
        if (!Read(table + 4 * b, &node, 4)) continue;
        for (unsigned hops = 0; node && hops < 64 && n < max; ++hops) {
            uint32_t handle = 0, stream = 0, desc = 0, identity = 0, namePtr = 0, next = 0;
            char name[96]{};
            Read(node, &handle, 4);
            Read(node + 4, &stream, 4);
            if (stream && Read(stream + 0x18, &desc, 4) && desc && Read(desc, &identity, 4) && identity &&
                Read(identity + 4, &namePtr, 4) && namePtr && Read(namePtr, name, sizeof(name) - 1) && LevelStream(name))
                out[n++] = Local{handle, stream, NameHash(name)};
            if (!Read(node + 8, &next, 4)) break;
            node = next;
        }
    }
    return n;
}
bool Visible(uint32_t stream) {
    uint32_t flags = 0;
    return Read(stream + 0x54, &flags, 4) && (flags & kVisibleFlag);
}
uint32_t Signature(const Local* local, unsigned n) {
    uint32_t sig = 0;
    for (unsigned i = 0; i < n; ++i) sig += (local[i].hash * 2654435761u) ^ (Visible(local[i].stream) ? 0x9E3779B9u : 0u);
    return sig;
}
int Filter(unsigned) { InterlockedIncrement(&faults); return EXCEPTION_EXECUTE_HANDLER; }
// Visibility union ([Streams] VisibilityUnion). Inside a level each player's
// own triggers show and hide rooms (VisibilityManager Show 0x007C1270 / Hide
// 0x007C12F0, thiscall(handle), ret 4: iMsgStreamVisible/Hidden to the room's
// entities, then StreamManager::SetVisible 0x0074ACE0, flags +0x54 bit 0x100).
// 24.09.: the host walked on through a big room made of several streams, his
// triggers hid the parts behind him, and the partner who stayed there stood in
// an invisible room on the host's screen. So a loaded room is shown while the local
// game wants it (the intent, recorded from the game's own Show/Hide calls) OR
// any partner's game wants it. The packets carry the intent, not what is on
// screen, so two machines never keep a room visible for each other.
bool unionEnabled = false, applying = false;
safetyhook::InlineHook showHook, hideHook;
struct Intent { uint32_t handle; bool visible; };
Intent intents[256]{};
unsigned intentCount = 0, intentNext = 0;
volatile LONG unionShown = 0, unionHidden = 0, intentsSeen = 0, unionLines = 0;
void NoteIntent(uint32_t handle, bool visible) {
    for (unsigned i = 0; i < intentCount; ++i) if (intents[i].handle == handle) { intents[i].visible = visible; return; }
    const unsigned slot = intentCount < std::size(intents) ? intentCount++ : intentNext++ % std::size(intents);
    intents[slot] = Intent{handle, visible};
}
bool IntentVisible(uint32_t handle, uint32_t stream) {
    for (unsigned i = 0; i < intentCount; ++i) if (intents[i].handle == handle) return intents[i].visible;
    const bool now = Visible(stream);
    NoteIntent(handle, now);   // first sight: what the game shows is what it wants
    return now;
}
bool __fastcall OnShow(void* self, void*, uint32_t handle) {
    if (!applying) { NoteIntent(handle, true); InterlockedIncrement(&intentsSeen); }
    return showHook.thiscall<bool>(self, handle);
}
bool PartnerShows(uint32_t hash, DWORD now);
// The name hash of a loaded stream, as LocalStreams computes it.
uint32_t HashOfHandle(uint32_t handle) {
    uint32_t manager = 0, stream = 0, desc = 0, identity = 0, namePtr = 0;
    char name[96]{};
    if (!Read(base + kStreamManager, &manager, 4) || !manager) return 0;
    __try {
        stream = reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t)>(base + 0x433B)(reinterpret_cast<void*>(manager), handle);   // StreamFromHandle
    } __except (Filter(GetExceptionCode())) {
        return 0;
    }
    if (!stream || !Read(stream + 0x18, &desc, 4) || !desc || !Read(desc, &identity, 4) || !identity ||
        !Read(identity + 4, &namePtr, 4) || !namePtr || !Read(namePtr, name, sizeof(name) - 1) || !LevelStream(name)) return 0;
    return NameHash(name);
}
volatile LONG hidesHeld = 0, arenaHidesRefused = 0;
// An MP map shows the rooms of one stream set at the arrival and hides the rest
// (StreamSetManager::Set::SetDesired_i, the hide returns to 0x00758B7D); the next
// sets come with the objectives. Survival runs none, so the rooms behind open doors
// stayed black (15.10. Fuel Core: 32 zonerenders hidden at the arrival, 1 of 6 sets
// ever desired). On an MP map those hides are refused ([MpMap] ShowAllRooms).
bool mpShowAll = true;
constexpr uint32_t kSetDesiredHideReturnRva = 0x358B7D;
volatile LONG mpHidesRefused = 0;
// A running campaign arena (runtime_coop_arena.inc): its rooms stay visible.
bool (*arenaKeepsVisible)(uint32_t handle) = nullptr;
bool hooked = false;
// The room our own Isaac stands in is never hidden in co-op ([Streams] KeepOwnRoom).
// 02.11.: the host walked ahead, his ghost counted for the cull triggers on the client
// (ghost_passed=97) and the client's game hid isaacs_cell and psych_hall around its own
// Isaac (callers 0x007C1401, hide by GUID, and 0x00758B7D, a stream set). The rooms are
// the streams (+0x38 of the hit entity) of the floor under Isaac and of four points
// 3 m around him, cast every 250 ms from the plugin tick.
bool keepOwnRoom = true;
constexpr unsigned kFloorPoints = 5, kHomePoints = kFloorPoints + 5;
constexpr uint32_t kHitStreamHandle = 0x38;
uint32_t home[kHomePoints]{};
DWORD homeAt = 0;
volatile LONG ownHidesRefused = 0, ownRoomsShown = 0, homeSamples = 0, homeUnknown = 0, homeChanges = 0, homeLines = 0;
uint32_t HomeSurface(float* a, float* b, uint32_t exclude) {
    float fraction = 1.0f;
    uint32_t hit = 0, handle = 0;
    if (!bot_sense::CastRaw(a, b, exclude, fraction, hit) || fraction >= 0.999f || !hit || !Read(hit + kHitStreamHandle, &handle, 4)) return 0;
    return handle;
}
bool IsHome(uint32_t handle) {
    if (!handle) return false;
    for (uint32_t h : home) if (h == handle) return true;
    return false;
}
uint32_t StreamOfHandle(uint32_t handle, char* name, size_t size) {
    uint32_t manager = 0, stream = 0, desc = 0, identity = 0, namePtr = 0;
    if (name && size) name[0] = 0;
    if (!Read(base + kStreamManager, &manager, 4) || !manager) return 0;
    __try {
        stream = reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t)>(base + 0x433B)(reinterpret_cast<void*>(manager), handle);   // StreamFromHandle
    } __except (Filter(GetExceptionCode())) {
        return 0;
    }
    if (stream && name && size > 1 && Read(stream + 0x18, &desc, 4) && desc && Read(desc, &identity, 4) && identity && Read(identity + 4, &namePtr, 4) && namePtr)
        Read(namePtr, name, size - 1);
    return stream;
}
bool SetVisible(uint32_t handle, bool show, bool asIntent);
void HomeTick(DWORD now) {
    if (!keepOwnRoom || !hooked || !peerTable.Count() || mpmap::InVisit() || !bot_sense::losReady) {
        for (uint32_t& h : home) h = 0;
        return;
    }
    if (homeAt && now - homeAt < 250) return;
    homeAt = now ? now : 1;
    const uint32_t body = LocalSlotZeroEntity();
    float me[3]{};
    if (!body || !Read(body + 0xC0, me, 12) || !std::isfinite(me[0]) || !std::isfinite(me[1]) || !std::isfinite(me[2])) {
        for (uint32_t& h : home) h = 0;
        return;
    }
    static const float kOffsets[kFloorPoints][2] = {{0.0f, 0.0f}, {3.0f, 0.0f}, {-3.0f, 0.0f}, {0.0f, 3.0f}, {0.0f, -3.0f}};
    const uint32_t before = home[0];
    for (unsigned i = 0; i < kFloorPoints; ++i) {
        alignas(16) float a[4] = {me[0] + kOffsets[i][0], me[1] + 0.5f, me[2] + kOffsets[i][1], 1};
        alignas(16) float b[4] = {a[0], me[1] - 5.5f, a[2], 1};
        home[i] = HomeSurface(a, b, body);
    }
    // Adjacent walls and ceilings can belong to different streams than the floor.
    static const float surfaces[][3] = {{4,0,0},{-4,0,0},{0,0,4},{0,0,-4},{0,4,0}};
    alignas(16) float origin[4] = {me[0], me[1] + 1, me[2], 1};
    for (unsigned i = 0; i < std::size(surfaces); ++i) {
        alignas(16) float end[4] = {origin[0]+surfaces[i][0], origin[1]+surfaces[i][1], origin[2]+surfaces[i][2], 1};
        home[kFloorPoints+i] = HomeSurface(origin, end, body);
    }
    InterlockedIncrement(&homeSamples);
    if (!home[0]) InterlockedIncrement(&homeUnknown);
    char name[96];
    if (home[0] && home[0] != before) {
        InterlockedIncrement(&homeChanges);
        if (InterlockedIncrement(&homeLines) <= 80) {
            StreamOfHandle(home[0], name, sizeof(name));
            gate::Event("coop_streams own room now %s (handle %08X) at (%.1f,%.1f,%.1f)", name[0] ? name : "?", home[0], me[0], me[1], me[2]);
        }
    }
    // A room hidden before we walked into it (a partner's trigger) is shown again, outside scenes,
    // which swap rooms on purpose.
    if (scenekind::local != scenekind::kNone) return;
    for (unsigned i = 0; i < kHomePoints; ++i) {
        if (!home[i]) continue;
        bool repeat = false;
        for (unsigned j = 0; j < i; ++j) repeat = repeat || home[j] == home[i];
        if (repeat) continue;
        const uint32_t stream = StreamOfHandle(home[i], name, sizeof(name));
        if (!stream || Visible(stream) || !SetVisible(home[i], true, true)) continue;
        InterlockedIncrement(&ownRoomsShown);
        if (InterlockedIncrement(&homeLines) <= 80)
            gate::Event("coop_streams own room %s (handle %08X) was hidden under our Isaac at (%.1f,%.1f,%.1f): shown again", name[0] ? name : "?", home[i], me[0], me[1],
                        me[2]);
    }
}
bool __fastcall OnHide(void* self, void*, uint32_t handle) {
    if (!applying && keepOwnRoom && IsHome(handle) && peerTable.Count() && !mpmap::InVisit()) {
        if (InterlockedIncrement(&ownHidesRefused) <= 100) {
            char name[96];
            StreamOfHandle(handle, name, sizeof(name));
            gate::Event("coop_streams our game hides %s (handle %08X), the room our Isaac stands in: refused (caller RVA %08X)", name[0] ? name : "?", handle,
                        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())) - static_cast<uint32_t>(base));
        }
        return true;
    }
    if (!applying && arenaKeepsVisible && arenaKeepsVisible(handle)) {
        InterlockedIncrement(&arenaHidesRefused);
        return true;
    }
    if (!applying && mpShowAll && mpmap::InVisit() &&
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())) - static_cast<uint32_t>(base) == kSetDesiredHideReturnRva) {
        if (InterlockedIncrement(&mpHidesRefused) == 1) gate::Event("coop_streams MP map: the stream set hides room handle %08X, refused (every room stays visible)", handle);
        return true;
    }
    if (!applying) {
        NoteIntent(handle, false);
        InterlockedIncrement(&intentsSeen);
        // A room a partner's game still shows is not hidden at all: no gap
        // until the next union pass, no Hidden/Visible pair to its entities.
        // UnionTick hides it once no game wants it any more.
        if (unionEnabled && peerTable.Count() && levelSettled) {
            const uint32_t hash = HashOfHandle(handle);
            if (hash && PartnerShows(hash, GetTickCount())) {
                if (InterlockedIncrement(&hidesHeld) <= 100)
                    gate::Event("coop_streams visibility union: our game hides stream %08X handle %08X, a partner still shows it: stays visible", hash, handle);
                return true;
            }
        }
        // Which room the local game hides, and where the player stands: 25.09. a player
        // standing in a large open area saw part of it go black when his
        // partner walked around a corner, and nothing in the log named it. On the
        // MP maps solo too, with the caller (13.10.: black rooms on Fuel Core with bots).
        static volatile LONG hideLines = 0;
        if ((peerTable.Count() || mpmap::InVisit()) && InterlockedIncrement(&hideLines) <= 200) {
            const uint32_t caller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())) - static_cast<uint32_t>(base);
            char name[96] = "";
            uint32_t manager = 0, stream = 0;
            if (Read(base + kStreamManager, &manager, 4) && manager) {
                __try {
                    stream = reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t)>(base + 0x433B)(reinterpret_cast<void*>(manager), handle);
                } __except (Filter(GetExceptionCode())) {
                    stream = 0;
                }
            }
            uint32_t desc = 0, identity = 0, namePtr = 0;
            if (stream && Read(stream + 0x18, &desc, 4) && desc && Read(desc, &identity, 4) && identity && Read(identity + 4, &namePtr, 4) && namePtr)
                Read(namePtr, name, sizeof(name) - 1);
            float me[3]{};
            const uint32_t local = LocalSlotZeroEntity();
            if (local) Read(local + 0xC0, me, 12);
            gate::Event("coop_streams our game hides %s (handle %08X) with our Isaac at (%.1f,%.1f,%.1f), caller RVA %08X", name[0] ? name : "?", handle,
                        me[0], me[1], me[2], caller);
        }
    }
    return hideHook.thiscall<bool>(self, handle);
}
// asIntent: the carry takes a partner's rooms as the local picture; the union
// only lends them.
bool SetVisible(uint32_t handle, bool show, bool asIntent = true) {
    if (!show && keepOwnRoom && IsHome(handle) && peerTable.Count() && !mpmap::InVisit()) {
        if (InterlockedIncrement(&ownHidesRefused) <= 100)
            gate::Event("coop_streams partner visibility hide refused for local surface %08X", handle);
        return false;
    }
    uint32_t manager = 0;
    if (!Read(base + kVisibilityManager, &manager, 4) || !manager) return false;
    if (asIntent) NoteIntent(handle, show);
    applying = true;
    bool ok = true;
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + (show ? kShowRva : kHideRva))(reinterpret_cast<void*>(manager), handle);
    } __except (Filter(GetExceptionCode())) {
        ok = false;
    }
    applying = false;
    return ok;
}
bool PartnerShows(uint32_t hash, DWORD now) {
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const PeerVis& peer = peers[s];
        if (!peer.at || TicksSince(now, peer.at) > kFreshMs) continue;
        for (unsigned i = 0; i < peer.packet.count && i < kMax; ++i)
            if (peer.packet.hashes[i] == hash) { if (peer.packet.visible[i]) return true; break; }
    }
    return false;
}
void UnionTick(const Local* local, unsigned n, DWORD now) {
    if (!unionEnabled || !peerTable.Count() || !levelSettled) return;
    for (unsigned i = 0; i < n; ++i) {
        const bool ours = IntentVisible(local[i].handle, local[i].stream);
        const bool want = ours || PartnerShows(local[i].hash, now);
        if (want == Visible(local[i].stream)) continue;
        if (!SetVisible(local[i].handle, want, false)) continue;
        InterlockedIncrement(want ? &unionShown : &unionHidden);
        if (InterlockedIncrement(&unionLines) <= 200)
            gate::Event("coop_streams visibility union: stream %08X handle %08X %s (%s)", local[i].hash, local[i].handle,
                        want ? "shown" : "hidden again", want ? "a partner's game shows it" : "no game wants it any more");
    }
}
// A necro at the vents has no body that walks through the level's show/hide triggers
// (its free camera is refused by them), so its rooms stayed black next to the host
// (16.10.). Its picture is the humans': a loaded room is shown when any human shows it
// and hidden when the humans who have it hide it.
volatile LONG ghostPasses = 0, ghostShown = 0, ghostHidden = 0, ghostLines = 0;
bool GhostPicture(const Local* local, unsigned n, DWORD now) {
    if (!GhostFollower()) return false;
    InterlockedIncrement(&ghostPasses);
    unsigned changed = 0, showing = 0;
    for (unsigned i = 0; i < n; ++i) {
        int want = -1;   // no human reports this stream
        for (unsigned s = 0; s < coop_peers::kMaxPeers && want != 1; ++s) {
            const PeerVis& peer = peers[s];
            if (!peer.at || TicksSince(now, peer.at) > kFreshMs || roles::IsNecro(s, now)) continue;
            for (unsigned j = 0; j < peer.packet.count && j < kMax; ++j)
                if (peer.packet.hashes[j] == local[i].hash) { want = peer.packet.visible[j] ? 1 : 0; break; }
        }
        if (want == 1) ++showing;
        if (want < 0 || (want == 1) == Visible(local[i].stream) || !SetVisible(local[i].handle, want == 1)) continue;
        ++changed;
        InterlockedIncrement(want == 1 ? &ghostShown : &ghostHidden);
    }
    if (changed) {
        signature = Signature(local, n);
        if (InterlockedIncrement(&ghostLines) <= 60)
            gate::Event("coop_streams visibility: necro at the vents takes the humans' rooms (%u stream(s) switched, %u shown by them)", changed, showing);
    }
    return true;
}
void OnPacket(const Packet& p, unsigned slot) {
    if (!enabled || slot >= coop_peers::kMaxPeers) return;
    if (!NewStreamReport(slot, p.token, p.sequence, peers[slot].packet.token, peers[slot].packet.sequence)) return;
    peers[slot].packet = p;
    peers[slot].at = Stamp(GetTickCount());
    InterlockedIncrement(&received);
    cutscene::NotePartnerScene(slot, p.sequenceAgoMs, peers[slot].at);
    scenekind::NotePartner(slot, p.sceneKind, peers[slot].at);
    // a destination that is no position never reaches the necro camera
    if (std::isfinite(p.teleportTo[0]) && std::isfinite(p.teleportTo[1]) && std::isfinite(p.teleportTo[2]))
        teleport::NotePartner(slot, p.token, p.teleportSerial, p.teleportTo);
    else if (InterlockedIncrement(&badTeleports) <= 5)
        gate::Event("coop_streams visibility: partner %u sent a teleport destination that is not finite (serial %u): ignored", slot, p.teleportSerial);
}
// The local side of every door, every kApplyMs; a flip through an open door during a
// scene (or kPreSceneMs before it) with the local picture unchanged is a carry.
void WatchDoors(DWORD now, uint32_t body) {
    const uint32_t before = watchedSignature;
    watchedSignature = signature;
    if (!worldcalls::doorSideOk || !body) return;
    const bool scene = sequenceSeenAt && TicksSince(now, sequenceSeenAt) <= kSceneTailMs;
    if (!scene) sceneFrom = 0;
    else if (!sceneFrom) sceneFrom = now;
    for (unsigned i = 0; i < doorCount; ++i) {
        Door& d = doors[i];
        if (!DoorAlive(d)) { d.side = -1; d.flippedAt = 0; continue; }
        uint32_t state = 0;
        if (Read(d.door + kDoorStateOffset, &state, 4) && (state == kDoorOpeningState || state == kDoorOpenState)) d.openAt = now;
        bool side = false;
        if (!worldcalls::DoorSide(d.door, body, side)) continue;
        if (d.side >= 0 && (d.side != 0) != side) {
            InterlockedIncrement(&flips);
            if (!d.openAt || now - d.openAt > kOpenGraceMs + kApplyMs) {
                InterlockedIncrement(&flipsClosed);   // an endless plane swept past a shut door
                d.flippedAt = 0;
            } else {
                d.flippedAt = now;
                d.flipSignature = before;
            }
        }
        d.side = side ? 1 : 0;
        if (!scene || !d.flippedAt || d.flippedAt + kPreSceneMs < sceneFrom) continue;
        const DWORD flippedAt = d.flippedAt;
        d.flippedAt = 0;
        if (d.flipSignature != signature) { InterlockedIncrement(&flipsWalked); continue; }   // the local triggers switched the rooms: the player walked
        carriedAt = now;
        carryOpen = true;
        carryDoor = d.door;
        carrySide = side;
        carrySignature = signature;
        carryTook = 0;
        InterlockedIncrement(&carries);
        gate::Event("coop_streams visibility: the scene carried our Isaac through open door %08X (side %u -> %u, %u ms ago)",
                    d.door, side ? 0u : 1u, side ? 1u : 0u, static_cast<unsigned>(now - flippedAt));
    }
}
void Tick(DWORD now) {
    if (!enabled || !inLevel) { signature = 0; doorCount = 0; sequenceSeenAt = 0; carriedAt = 0; carryOpen = false; intentCount = 0; for (uint32_t& h : home) h = 0; return; }
    HomeTick(now);
    Local local[64];
    const unsigned n = LocalStreams(local, 64);
    const uint32_t sig = Signature(local, n);
    if (sig != signature) {
        if (signature) InterlockedIncrement(&changes);
        signature = sig;
    }
    if (peerTable.Count() && (now - lastSend >= kSendMs || scenekind::sendNow)) {
        lastSend = now;
        scenekind::sendNow = false;
        Packet p{};
        p.magic = kMagic; p.version = kVersion; p.token = peerReadiness.token; p.sequence = ++sequence;
        p.sceneKind = scenekind::local;
        p.teleportSerial = teleport::serial;
        std::memcpy(p.teleportTo, teleport::to, 12);
        p.sequenceAgoMs = sequenceSeenAt ? TicksSince(now, sequenceSeenAt) : kNone;
        p.carriedAgoMs = carriedAt ? TicksSince(now, carriedAt) : kNone;
        std::memcpy(p.area, area, 16);
        for (unsigned i = 0; i < n && p.count < kMax; ++i) {
            p.hashes[p.count] = local[i].hash;
            p.visible[p.count] = (unionEnabled ? IntentVisible(local[i].handle, local[i].stream) : Visible(local[i].stream)) ? 1 : 0;
            ++p.count;
        }
        if (SendToPeers(&p, sizeof(p))) InterlockedIncrement(&sent);
    }
    if (now - lastApply < kApplyMs) return;
    lastApply = now;
    WatchDoors(now, LocalSlotZeroEntity());
    if (GhostPicture(local, n, now)) { carryOpen = false; return; }
    if (!carryOpen) { UnionTick(local, n, now); return; }
    if (!sequenceSeenAt || TicksSince(now, sequenceSeenAt) > kSceneTailMs + kSettleMs) {
        carryOpen = false;
        if (!carryTook) InterlockedIncrement(&expired);
        gate::Event("coop_streams visibility: carry over with the scene, partner's rooms taken %u time(s)", carryTook);
        return;
    }
    if (signature != carrySignature) {   // the local triggers switched the rooms: the game knows where the player is
        carryOpen = false;
        InterlockedIncrement(&walkedAway);
        gate::Event("coop_streams visibility: our rooms changed by themselves after the carry, following stopped");
        return;
    }
    if (GuidZero(area)) return;
    float me[3]{};
    const bool placed = gate::LocalPosition(me);
    int best = -1;
    float bestD2 = 0.0f;   // only picks among several partners, never refuses one
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const PeerVis& peer = peers[s];
        if (!peer.at || TicksSince(now, peer.at) > kFreshMs || !GuidEqual(peer.packet.area, area) || roles::IsNecro(s, now)) continue;
        const DWORD age = TicksSince(now, peer.at);
        if (peer.packet.sequenceAgoMs == kNone || peer.packet.sequenceAgoMs > kInScenePacketMs) continue;   // only his picture from inside the scene
        if (peer.packet.carriedAgoMs != kNone && peer.packet.carriedAgoMs + age <= kCarryWindowMs) { InterlockedIncrement(&partnerCarried); continue; }
        const auto& runtime = peerRuntime[s];
        if (!runtime.lastState || now - runtime.lastState > 5000) continue;
        const float d2 = placed ? coop_gate::DistanceSquared(me, runtime.pos) : 0.0f;
        if (best >= 0 && d2 >= bestD2) continue;
        const uint32_t ghost = GhostForMachine(peerTable.peers[s].machine);
        bool side = false;
        if (!ghost || !worldcalls::DoorSide(carryDoor, ghost, side) || side != carrySide) { InterlockedIncrement(&partnerOtherSide); continue; }
        best = static_cast<int>(s);
        bestD2 = d2;
    }
    if (best < 0) return;
    // His picture changes a moment after the scene starts, so it is followed
    // for as long as he reports from inside the scene, not taken once.
    const Packet& his = peers[best].packet;
    unsigned changed = 0;
    uint32_t expected = 0;
    for (unsigned i = 0; i < n; ++i) {
        bool visible = Visible(local[i].stream);
        for (unsigned j = 0; j < his.count; ++j) {
            if (his.hashes[j] != local[i].hash) continue;
            const bool want = his.visible[j] != 0;
            if (want != visible && SetVisible(local[i].handle, want)) {
                visible = Visible(local[i].stream);
                ++changed;
                InterlockedIncrement(want ? &shown : &hidden);
            }
            break;
        }
        expected += (local[i].hash * 2654435761u) ^ (visible ? 0x9E3779B9u : 0u);
    }
    if (!changed) { InterlockedIncrement(&alreadySame); return; }
    carrySignature = expected;   // this switch is not a walk
    signature = Signature(local, n);
    ++carryTook;
    InterlockedIncrement(&adoptions);
    gate::Event("coop_streams visibility: carried next to partner %d (%.1f m, same side of door %08X): took his rooms (%u stream(s) switched)",
                best, std::sqrt(bestD2), carryDoor, changed);
}
}  // namespace vis

// Partner rooms kept in the local set instead of holding the local switch
// ([Streams] PinPartnerStreams). 24.09. research (Xbox debug build + PC):
//   StreamSet::ActivateSet (PC 0x00759350, thiscall(setId, ?, count), ret 0xC)
//   copies the set's member GUIDs (46 at most, 16 bytes each, plus a
//   spawn-hidden byte each) and calls Set::SetDesired (PC 0x00758C80,
//   thiscall(Set*, guids*, bools*, count, activatorGuid*), ret 0x10) on the
//   zone set, [[[0x0201C0BC]+0x10]+0]. SetDesired leaves wanted streams that
//   are loaded or loading alone and unloads the rest of its own list; the set
//   then loads the new ones once its unload list is empty.
// So a stream both machines hold, that the partner's set still has and the local one
// is about to drop, is added to the desired list: it stays a member,
// nothing waits, and the next switch without it unloads it the normal way.
// An extra reference instead (tried first) would have kept the stream out of
// the unload list's reach and the set would never have finished.
namespace pin {
bool enabled = false, hooked = false;
// A partner this close walks through with us: his own game switches a moment
// later, and a room kept for him stays loaded here until our next switch.
// 12.10.: the client, 5 m behind the host, still reported the set he had taken
// over from the host's scene; the host kept his own old rooms for him and a
// video two rooms back was still heard. Only a partner farther away, who
// really is somewhere else, keeps rooms ([Streams] PinNearM).
// Never around a scene: 12.10. 20:29 the client's switch at the start of the
// host's IGC kept nothing for the host "0 m away", and the host's ghost, still
// animating from a dropped room, crashed the client in the anim tree (dump
// 34836). In a scene and for a minute after it everything is kept as before.
float nearM = 0.0f;
volatile LONG nearSkipped = 0, sceneKept = 0;
constexpr uint32_t kSetDesiredRva = 0x358C80, kFromActivateSetRva = 0x359410, kSetManager = 0x1C1C0BC;
constexpr unsigned kMaxDesired = 46;
// The engine's previous zone request also contains rooms retained for a partner.
uint32_t lastZone = 0, lastDesired[kMaxDesired][4]{};
unsigned lastDesiredCount = 0;
void ForgetDesired() { lastZone = 0; lastDesiredCount = 0; }

safetyhook::InlineHook setDesiredHook;
volatile LONG switches = 0, kept = 0, keptOwn = 0, full = 0, lines = 0, notZone = 0, fullLines = 0;
// A running campaign arena (runtime_coop_arena.inc) adds its rooms to every
// zone set switch; returns how many entries it changed.
unsigned (*arenaMerge)(uint32_t (*list)[4], unsigned char* hidden, unsigned& count, unsigned max, uint32_t caller) = nullptr;
// mpmap's SetDesired hook, installed later, sits in front of this one and
// passes the caller it saw (09.10.: without it no arena switch ever matched).
uint32_t outerCaller = 0;
uint32_t ZoneSet() {
    uint32_t manager = 0, sets = 0, zone = 0;
    return Read(base + kSetManager, &manager, 4) && manager && Read(manager + 0x10, &sets, 4) && sets && Read(sets, &zone, 4) ? zone : 0;
}
bool InList(const uint32_t (*list)[4], unsigned count, const uint32_t* guid) {
    for (unsigned i = 0; i < count; ++i) if (GuidEqual(list[i], guid)) return true;
    return false;
}
void __fastcall OnSetDesired(void* self, void*, const uint32_t (*guids)[4], const unsigned char* hidden, uint32_t count, const uint32_t* activator) {
    const auto set = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    const auto caller = outerCaller ? outerCaller : static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    // the partner pin only on ActivateSet's switch; an arena keeps its rooms on every zone set switch
    const bool partners = enabled && peerTable.Count() && caller == base + kFromActivateSetRva;
    const bool necroUnion = NecroUnion() && peerTable.Count() && caller == base + kFromActivateSetRva;
    const bool keepOwn = keepOwnNow && caller == base + kFromActivateSetRva;   // a far door's or an approached door's set
    if ((!partners && !arenaMerge && !necroUnion && !keepOwn) || count > kMaxDesired || !guids || !hidden) {
        if (lastZone == set) ForgetDesired();
        setDesiredHook.thiscall<void>(self, guids, hidden, count, activator);
        return;
    }
    if (set != ZoneSet()) { InterlockedIncrement(&notZone); setDesiredHook.thiscall<void>(self, guids, hidden, count, activator); return; }
    InterlockedIncrement(&switches);
    uint32_t list[kMaxDesired][4]{};
    unsigned char spawnHidden[kMaxDesired]{};
    unsigned n = count;
    std::memcpy(list, guids, count * 16);
    std::memcpy(spawnHidden, hidden, count);
    unsigned added = 0;
    if (partners || keepOwn) {
        uint32_t ours[kMaxMembers][4]{};
        const uint32_t ourNode = GuidZero(area) ? 0 : mirror::ObjectByGuid(area);
        const unsigned oursCount = ourNode ? Members(ourNode, ours) : 0;
        const DWORD now = GetTickCount();
        if ((partners && followingNow) || keepOwn) {
            // Following a partner's scene: his set comes in, the local one stays, the floor
            // under the local Isaac with it. The next local switch drops it as usual.
            for (unsigned i = 0; i < oursCount; ++i) {
                if (InList(list, n, ours[i])) continue;
                if (n >= kMaxDesired) {
                    InterlockedIncrement(&full);
                    if (InterlockedIncrement(&fullLines) <= 40)
                        gate::Event("coop_streams list FULL (%u rooms): our own %08X-%08X-%08X-%08X and %u more of ours not kept while following", kMaxDesired,
                                    ours[i][0], ours[i][1], ours[i][2], ours[i][3], oursCount - i - 1);
                    break;
                }
                std::memcpy(list[n], ours[i], 16);
                spawnHidden[n] = 0;
                ++n; ++added;
                InterlockedIncrement(&keptOwn);
            }
            if (keepOwn) keptOwnLast = added;
            else if (added && InterlockedIncrement(&lines) <= 200)
                gate::Event("coop_streams following a partner's scene: %u rooms of our own stay loaded", added);
        }
        for (unsigned slot = 0; slot < coop_peers::kMaxPeers && partners; ++slot) {
            const auto& peer = peerTable.peers[slot];
            if (!peer.used || !peer.token || now - peer.lastSeen > 10000) continue;
            const PeerArea& a = peerAreas[slot];
            if (!a.at || TicksSince(now, a.at) > kStaleMs) continue;
            const auto& runtime = peerRuntime[slot];
            float me[3]{};
            const bool scene = cutscene::SequenceRunningNow() || cutscene::Following() || cutscene::Leading() ||
                               (referenceUntil && now < referenceUntil);
            if (scene && nearM > 0.0f) InterlockedIncrement(&sceneKept);
            if (!scene && nearM > 0.0f && runtime.lastState && now - runtime.lastState <= 5000 && gate::LocalPosition(me)) {
                const float d2 = coop_gate::DistanceSquared(me, runtime.pos);
                if (d2 <= nearM * nearM) {
                    InterlockedIncrement(&nearSkipped);
                    if (InterlockedIncrement(&lines) <= 200)
                        gate::Event("coop_streams keep nothing for partner %u: %.0f m from us, he comes along (his set %08X-%08X)", slot, std::sqrt(d2), a.area[0], a.area[1]);
                    continue;
                }
            }
            for (const uint32_t* setGuid : {a.area, a.pending}) {
                if (GuidZero(setGuid)) continue;
                const uint32_t node = mirror::ObjectByGuid(setGuid);
                uint32_t theirs[kMaxMembers][4]{};
                const unsigned m = node ? Members(node, theirs) : 0;
                for (unsigned i = 0; i < m; ++i) {
                    // Only a room loaded now and about to be dropped: nothing new is loaded for the partner.
                    if (InList(list, n, theirs[i])) continue;
                    if (!InList(ours, oursCount, theirs[i]) && (lastZone != set || !InList(lastDesired, lastDesiredCount, theirs[i]))) continue;
                    if (n >= kMaxDesired) {
                        InterlockedIncrement(&full);
                        if (InterlockedIncrement(&fullLines) <= 40)
                            gate::Event("coop_streams list FULL (%u rooms): partner %u's %08X-%08X-%08X-%08X and up to %u more not kept (his set %08X-%08X)",
                                        kMaxDesired, slot, theirs[i][0], theirs[i][1], theirs[i][2], theirs[i][3], m - i - 1, setGuid[0], setGuid[1]);
                        break;
                    }
                    std::memcpy(list[n], theirs[i], 16);
                    spawnHidden[n] = 0;
                    ++n; ++added;
                    InterlockedIncrement(&kept);
                    if (InterlockedIncrement(&lines) <= 200)
                        gate::Event("coop_streams keep %08X-%08X-%08X-%08X in our set: partner %u still has it (his set %08X-%08X)",
                                    theirs[i][0], theirs[i][1], theirs[i][2], theirs[i][3], slot, setGuid[0], setGuid[1]);
                }
            }
        }
    }
    if (necroUnion) {
        const DWORD now = GetTickCount();
        unsigned merged = 0;
        for (unsigned slot = 0; slot < coop_peers::kMaxPeers; ++slot) {
            const auto& peer = peerTable.peers[slot];
            if (!peer.used || !peer.token || now - peer.lastSeen > 10000 || roles::IsNecro(slot, now)) continue;
            const PeerArea& a = peerAreas[slot];
            if (!a.at || TicksSince(now, a.at) > kStaleMs) continue;
            for (const uint32_t* setGuid : {a.area, a.pending}) {
                if (GuidZero(setGuid)) continue;
                const uint32_t node = mirror::ObjectByGuid(setGuid);
                uint32_t theirs[kMaxMembers][4]{};
                const unsigned m = node ? Members(node, theirs) : 0;
                for (unsigned i = 0; i < m; ++i) {
                    if (InList(list, n, theirs[i])) continue;
                    if (n >= kMaxDesired) {
                        InterlockedIncrement(&unionFull);
                        if (InterlockedIncrement(&fullLines) <= 40)
                            gate::Event("coop_streams list FULL (%u rooms): the living necro keeps no more of human %u's set %08X-%08X", kMaxDesired, slot, setGuid[0], setGuid[1]);
                        break;
                    }
                    std::memcpy(list[n], theirs[i], 16);
                    spawnHidden[n] = 0;
                    ++n; ++added; ++merged;
                }
            }
        }
        if (merged) {
            InterlockedExchangeAdd(&unionMerged, static_cast<LONG>(merged));
            if (InterlockedIncrement(&lines) <= 200) gate::Event("coop_streams living necro: %u room(s) of the humans' sets loaded beside our own (%u in the list)", merged, n);
        }
    }
    if (arenaMerge) added += arenaMerge(list, spawnHidden, n, kMaxDesired, caller);
    lastZone = set;
    lastDesiredCount = n;
    std::memcpy(lastDesired, list, n * sizeof(list[0]));
    setDesiredHook.thiscall<void>(self, added ? list : guids, added ? spawnHidden : hidden, n, activator);
}
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Streams", "PinPartnerStreams", 0, ini.c_str()) != 0;
    nearM = static_cast<float>(GetPrivateProfileIntA("Streams", "PinNearM", 0, ini.c_str()));
    const bool arenas = GetPrivateProfileIntA("Arena", "Arenas", 1, ini.c_str()) != 0;
    if (enabled || arenas || farDoorGuard || doorApproach) {
        // sub esp,0x30 / push ebx / push ebp / push esi / mov esi,ecx / push edi / lea ecx,[esi+0x9A0]
        static const unsigned char kSig[] = {0x83, 0xEC, 0x30, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x57, 0x8D, 0x8E, 0xA0, 0x09, 0x00, 0x00};
        hooked = hook_site::Install(log, "coop_streams StreamSetManager::Set::SetDesired", base + kSetDesiredRva, kSig, sizeof(kSig),
                                    reinterpret_cast<void*>(OnSetDesired), setDesiredHook);
        if (!hooked) enabled = false;
    }
    std::fprintf(log, "coop_streams pin_partner_streams=%d set_desired_hook=%d near_m=%.0f (a set switch keeps the rooms a partner's set still holds when he is farther away, or a campaign arena's rooms)\n",
                 enabled ? 1 : 0, hooked ? 1 : 0, nearM);
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    std::fprintf(log, "coop_streams keep switches=%ld kept=%ld kept_own=%ld list_full=%ld not_zone=%ld partners_near_skipped=%ld kept_for_scene=%ld\n",
                 InterlockedCompareExchange(&switches, 0, 0), InterlockedCompareExchange(&kept, 0, 0), InterlockedCompareExchange(&keptOwn, 0, 0),
                 InterlockedCompareExchange(&full, 0, 0), InterlockedCompareExchange(&notZone, 0, 0), InterlockedCompareExchange(&nearSkipped, 0, 0),
                 InterlockedCompareExchange(&sceneKept, 0, 0));
}
}  // namespace pin
void ForgetPeer(unsigned slot) {
    if (slot >= coop_peers::kMaxPeers) return;
    if (peerAreas[slot].token && host.token == peerAreas[slot].token) { hostAt = 0; host = {}; }
    peerAreas[slot] = {};
    vis::peers[slot] = {};
    if (referenceSlot == slot) { referenceSlot = 0xFF; referenceUntil = 0; }
}
void OnVisPacket(const vis::Packet& p, unsigned slot) { vis::OnPacket(p, slot); }

void Release(bool late) {
    if (!held.used) return;
    Held copy = held;
    held = Held{};
    ++releasing;
    __try {
        activateHook.thiscall<void>(reinterpret_cast<void*>(copy.node), copy.actor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gate::Event("coop_streams release faulted node=%08X", copy.node);
    }
    --releasing;
    InterlockedIncrement(late ? &timedOutCount : &releasedCount);
    gate::Event("coop_streams released set %08X-%08X-%08X-%08X after %u ms%s", copy.guid[0], copy.guid[1], copy.guid[2], copy.guid[3],
                static_cast<unsigned>(GetTickCount() - copy.since), late ? " TIMEOUT" : " (partners fit)");
    if (noticeShown) notice::Show(late ? "CONTINUING WITHOUT PARTNER" : "PARTNER HERE - CONTINUING");
    noticeShown = false;
}

// The arena recorder (runtime_coop_arenarec.inc) listens here: set node, the
// actor that asked and who asked (kKind*).
void (*recordActivation)(uint32_t node, uint32_t actor, uint32_t kind) = nullptr;

// A door's set asked for far from our Isaac ([Streams] FarDoorGuard, FarDoorM). 24.10.: a
// partner's door replayed here, or a door held open for him that shut minutes later, asked
// for its set while our Isaac stood rooms away, and the switch replaced our zone set: the
// room he stood in unloaded. Beyond FarDoorM an open set comes in with our current rooms kept
// (pin::OnSetDesired), a leave set is skipped (its door has shut, nothing waits for it).
// Every door-asked activation is logged here, near ones too. True: handled here.
float farDoorM = 30.0f;
constexpr uint32_t kStandardDoorVtableRva = 0x174D5D8, kDoorDescriptor = 0x0C, kDoorPendingSet = 0x34C;
constexpr uint32_t kDoorSetOffsets[4] = {0xE0, 0xC0, 0xF0, 0xD0};   // open side 0/1, leave side 0/1
volatile LONG farSeen = 0, farNear = 0, farMerged = 0, farSkipped = 0, farUnmerged = 0, farLines = 0, nearLines = 0;
bool FarDoorActivation(void* self, uint32_t node, uint32_t actor) {
    uint32_t vtable = 0, desc = 0;
    if (!farDoorGuard || !node || !actor || !peerTable.Count() || !levelSettled || GhostFollower() || necro::LocalRole() != 0 ||
        !Read(actor, &vtable, 4) || vtable != base + kStandardDoorVtableRva || !Read(actor + kDoorDescriptor, &desc, 4) || !desc) return false;
    uint32_t guid[4]{};
    Read(node + kNodeGuid, guid, 16);
    int which = -1;
    for (int i = 0; i < 4 && which < 0; ++i) {
        uint32_t set[4]{};
        if (Read(desc + kDoorSetOffsets[i], set, 16) && GuidEqual(set, guid)) which = i;
    }
    const bool leaveSet = which >= 2;
    const char* kind = which < 0 ? "door (none of its four)" : leaveSet ? (which == 3 ? "leave side 1" : "leave side 0") : (which == 1 ? "open side 1" : "open side 0");
    float doorPos[3]{}, me[3]{};
    const bool placed = Read(actor + placement::kPlayerPosition, doorPos, 12) && gate::LocalPosition(me);
    const float distance = placed ? std::sqrt(coop_gate::DistanceSquared(me, doorPos)) : -1.0f;
    uint32_t members[kMaxMembers][4]{}, ours[kMaxMembers][4]{};
    const unsigned count = Members(node, members);
    const uint32_t ourNode = AreaValid() ? mirror::ObjectByGuid(area) : 0;
    const unsigned oursCount = ourNode ? Members(ourNode, ours) : 0;
    unsigned drops = 0;
    for (unsigned i = 0; i < oursCount; ++i) if (!Contains(members, count, ours[i])) ++drops;
    InterlockedIncrement(&farSeen);
    const bool isFar = placed && distance > farDoorM;
    const char* verdict = "";
    bool handled = false;
    if (!isFar) {
        InterlockedIncrement(&farNear);
        verdict = placed ? "near: as the game asks" : "our position unknown: as the game asks";
    } else if (leaveSet) {
        uint32_t pending = 0;
        if (Read(actor + kDoorPendingSet, &pending, 4) && pending == node) { const uint32_t none = 0; WriteBytes(actor + kDoorPendingSet, &none, 4); }
        InterlockedIncrement(&farSkipped);
        verdict = "FAR: skipped, our rooms stay";
        handled = true;
    } else if (!pin::hooked) {
        InterlockedIncrement(&farUnmerged);
        verdict = "FAR: passes NOT merged (no SetDesired hook)";
    } else {
        keepOwnNow = true;
        keptOwnLast = 0;
        activateHook.thiscall<void>(self, actor);
        keepOwnNow = false;
        InterlockedIncrement(&farMerged);
        verdict = "FAR: loaded with our rooms kept";
        handled = true;
    }
    if (isFar ? InterlockedIncrement(&farLines) <= 100 : InterlockedIncrement(&nearLines) <= 50)   // near ones must not use up the far ones' lines
        gate::Event("coop_streams door %08X asks for its %s set %08X-%08X-%08X-%08X (%u streams): our Isaac %.0f m away (far beyond %.0f m), it would drop %u of our %u; %s%s",
                    actor, kind, guid[0], guid[1], guid[2], guid[3], count, distance, farDoorM, drops, oursCount, verdict,
                    handled && !leaveSet ? (keptOwnLast ? " (merged)" : " (nothing of ours to keep)") : "");
    return handled;
}

void __fastcall OnActivate(void* self, void*, uint32_t actor) {
    InterlockedIncrement(&activateCalls);
    lastActivationKind = releasing ? kKindFollow : KindOfCaller(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
    // A door's leave set is chosen from where the players are (see leave).
    if (leave::reissuing) {
        lastActivationKind = kKindDoor;
    } else if (lastActivationKind == kKindDoor && self) {
        const uint32_t chosen = DoorActivation(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)), actor);
        if (!chosen) return;   // held: the open set stays, both rooms loaded
        self = reinterpret_cast<void*>(chosen);
    }
    const uint32_t node = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    if (recordActivation && node) recordActivation(node, actor, lastActivationKind);
    if (lastActivationKind == kKindDoor && enabled && FarDoorActivation(self, node, actor)) return;
    if (pin::enabled && enabled && levelSettled && node) {
        // Nobody waits: what a partner's set still holds stays in the local one (pin::OnSetDesired).
        if (held.used) { held = Held{}; noticeShown = false; }
        InterlockedIncrement(&passedCount);
        activateHook.thiscall<void>(self, actor);
        return;
    }
    if (!enabled || !holdEnabled || releasing || !peerTable.Count() || !levelSettled || !node) {
        activateHook.thiscall<void>(self, actor);
        return;
    }
    uint32_t guid[4]{};
    uint32_t members[kMaxMembers][4]{};
    const unsigned count = Members(node, members);
    Read(node + kNodeGuid, guid, 16);
    const DWORD now = GetTickCount();
    char who[96] = "";
    // An activation that a MIRRORED message caused (a partner's sequence cue
    // replayed here, and its chain reached a stream set) must not take the
    // local room away: 21:43 the host's cues switched the client's streams to
    // wherever the host was. It goes through only if it keeps every stream of
    // the current local set; otherwise it is refused, and the player's own walk through
    // the door will ask for it again legitimately.
    uint32_t referenceSet[4]{};
    const bool nearReference = NearReference(now, referenceSet, nullptr);
    if (gate::mirroring > 0 && nearReference) {
        // his cues are moving the local player: through, not judged against his own set
        InterlockedIncrement(&allowedNear);
        InterlockedIncrement(&passedCount);
        activateHook.thiscall<void>(self, actor);
        return;
    } else if (gate::mirroring > 0 && !GuidZero(area)) {
        const uint32_t ourNode = mirror::ObjectByGuid(area);
        uint32_t ours[kMaxMembers][4]{};
        const unsigned m = ourNode ? Members(ourNode, ours) : 0;
        for (unsigned i = 0; i < m; ++i) {
            if (Contains(members, count, ours[i])) continue;
            InterlockedIncrement(&refusedMirrored);
            gate::Event("coop_streams refused mirrored activation of %08X-%08X-%08X-%08X (would drop our stream %08X-%08X)",
                        guid[0], guid[1], guid[2], guid[3], ours[i][0], ours[i][1]);
            return;
        }
    }
    if (count && SceneActivation(now)) {
        if (held.used && !GuidEqual(held.guid, guid)) { held = Held{}; InterlockedIncrement(&replacedCount); noticeShown = false; }
        InterlockedIncrement(&sceneActivations);
        InterlockedIncrement(&passedCount);
        gate::Event("coop_streams set %08X-%08X-%08X-%08X passes unheld: a scene of ours asks for it (asked by %s)",
                    guid[0], guid[1], guid[2], guid[3], KindName(lastActivationKind));
        activateHook.thiscall<void>(self, actor);
        return;
    }
    if (!count || PartnerComesToo(guid, now) || PartnersFit(members, count, now, who, sizeof(who))) {
        // A set everybody fits in: through. It also ends any hold for a
        // different set: this one is the newer wish.
        if (held.used && !GuidEqual(held.guid, guid)) { held = Held{}; InterlockedIncrement(&replacedCount); noticeShown = false; }
        InterlockedIncrement(&passedCount);
        activateHook.thiscall<void>(self, actor);
        return;
    }
    if (held.used && !GuidEqual(held.guid, guid)) InterlockedIncrement(&replacedCount);
    const bool fresh = !held.used || !GuidEqual(held.guid, guid);
    if (fresh) {
        held = Held{};
        held.used = true; held.node = node; held.actor = actor; held.since = now; held.kind = lastActivationKind;
        std::memcpy(held.guid, guid, 16);
        InterlockedIncrement(&heldCount);
        gate::Event("coop_streams holding set %08X-%08X-%08X-%08X (%u streams): %s", guid[0], guid[1], guid[2], guid[3], count, who);
    }
    held.lastTry = now;
    held.members = count;
    std::memcpy(held.memberGuids, members, sizeof(members));
}

// The living necro: a human's set changed and ours does not hold it, so our own set is switched
// again (a plain activation, as the door rooms do) and pin::OnSetDesired merges theirs in.
uint32_t unionSeen[coop_peers::kMaxPeers][4]{};
DWORD unionAt = 0;
void UnionTick(DWORD now) {
    if (!NecroUnion() || !inLevel || !levelSettled || !AreaValid() || !pin::hooked || now - unionAt < 2000) return;
    bool refresh = false;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        if (!peer.used || !peer.token || now - peer.lastSeen > 10000 || roles::IsNecro(s, now)) continue;
        const PeerArea& a = peerAreas[s];
        if (!a.at || TicksSince(now, a.at) > kStaleMs || GuidZero(a.area) || GuidEqual(a.area, unionSeen[s])) continue;
        std::memcpy(unionSeen[s], a.area, 16);
        if (!Covers(a.area)) refresh = true;
    }
    if (!refresh) return;
    unionAt = now;
    if (ActivateDoorSet(area)) {
        InterlockedIncrement(&unionRefreshes);
        if (InterlockedCompareExchange(&unionRefreshes, 0, 0) <= 60)
            gate::Event("coop_streams living necro: a human's set changed, our set %08X-%08X switched again to take theirs in", area[0], area[1]);
    }
}
// Doors a partner opened ([Streams] DoorApproachLoads, DoorApproachM). 24.10.: our Isaac walked
// through a door the partner had opened; open (5) and held open for him, it never ran a set
// here, and the room behind came only with NearTick's adoption much later. Within
// DoorApproachM of an open door whose open set for our side our set does not cover, that set
// is activated with our current rooms kept.
float approachM = 6.0f;
constexpr DWORD kApproachPassMs = 500, kApproachRetryMs = 8000;
constexpr uint32_t kDoorStateField = 0x234;
uint32_t approachDoor = 0, approachSet[4]{};
DWORD approachAt = 0, approachPassAt = 0;
volatile LONG approachPasses = 0, approachActivations = 0, approachCovered = 0, approachUnknown = 0, approachLines = 0;
void ApproachTick(DWORD now) {
    if (!doorApproach || !pin::hooked || !peerTable.Count() || !inLevel || !levelSettled || TicksSince(now, approachPassAt) < kApproachPassMs) return;
    approachPassAt = now;
    if (GhostFollower() || necro::LocalRole() != 0 || arena::RunningNow() || mpmap::InVisit() || !AreaValid() || !worldcalls::doorSideOk) return;
    if (cutscene::SequenceRunningNow() || cutscene::Following()) return;   // a scene brings its own rooms
    const uint32_t body = LocalSlotZeroEntity();
    float me[3]{};
    if (!body || !gate::LocalPosition(me)) return;
    InterlockedIncrement(&approachPasses);
    for (unsigned i = 0; i < vis::doorCount; ++i) {
        const vis::Door& d = vis::doors[i];
        uint32_t state = 0, desc = 0;
        float pos[3]{};
        if (!vis::DoorAlive(d) || d.vtable != base + kStandardDoorVtableRva || !Read(d.door + kDoorStateField, &state, 4) ||
            (state != vis::kDoorOpeningState && state != vis::kDoorOpenState)) continue;
        if (!Read(d.door + placement::kPlayerPosition, pos, 12) || coop_gate::DistanceSquared(me, pos) > approachM * approachM) continue;
        bool side = false;
        if (!Read(d.door + kDoorDescriptor, &desc, 4) || !desc || !worldcalls::DoorSide(d.door, body, side)) continue;
        uint32_t set[4]{};
        if (!Read(desc + kDoorSetOffsets[side ? 1 : 0], set, 16) || GuidZero(set)) continue;
        if (Covers(set)) { InterlockedIncrement(&approachCovered); continue; }
        if (approachDoor == d.door && GuidEqual(approachSet, set) && TicksSince(now, approachAt) < kApproachRetryMs) continue;   // still loading
        approachDoor = d.door;
        std::memcpy(approachSet, set, 16);
        approachAt = Stamp(now);
        const bool ok = ActivateDoorSet(set, true);
        InterlockedIncrement(ok ? &approachActivations : &approachUnknown);
        if (InterlockedIncrement(&approachLines) <= 100)
            gate::Event("coop_streams approaching open door %08X (%.1f m, our side %u, state %u): its open set %08X-%08X-%08X-%08X is not ours yet: %s",
                        d.door, std::sqrt(coop_gate::DistanceSquared(me, pos)), side ? 1u : 0u, state, set[0], set[1], set[2], set[3],
                        !ok ? "NO NODE for it here" : keptOwnLast ? "activated, our rooms kept (merged)" : "activated (nothing of ours to keep)");
        return;   // one door per pass
    }
}

// Black until the rooms are in ([Loading] CurtainUntilRoomsLoaded, CurtainHoldMs,
// CurtainShowHiddenRooms). 24.10.: after a client's level start, checkpoint load or restart
// the loading screen went away before the rooms around Isaac were in, and rooms the start set
// spawned hidden (member flag bit 0 at +0x10, hidden by SetDesired_i) stayed black around him.
// On a client the overlay's curtain and an input hold stay until every member of the first
// stream set completed since the load is loaded (status 3); hidden members the set itself hid,
// or that a partner's game shows, are shown. CurtainHoldMs at most; the log says why it lifted.
namespace curtain {
bool enabled = true, showHidden = true, lookupOk = false;
DWORD holdMs = 10000;
constexpr uint32_t kStreamLookupRva = 0x348A40, kStatusLoaded = 3, kMemberFlags = 0x10, kSpawnHidden = 1;
constexpr DWORD kPassMs = 200;
constexpr LONG kInputBit = 4;   // focusrate::PanelHolds: 1 the shop, 2 the debug menu
enum class Phase { Idle, Armed, Holding };
Phase phase = Phase::Idle;
DWORD loadMark = 0, armedAt = 0, since = 0, lastPass = 0;
const char* armedWhy = "";
volatile LONG holding = 0;   // read by the controller filter (runtime_coop_cutscene.inc)
volatile LONG holds = 0, liftedReady = 0, liftedTimeout = 0, liftedOther = 0, skipped = 0, shownRooms = 0, lines = 0, showLines = 0;
bool HoldsInput() { return InterlockedCompareExchange(&holding, 0, 0) != 0; }
void MarkLoad(const char* why) {
    loadMark = Stamp(GetTickCount());
    if (enabled && phase == Phase::Idle) { phase = Phase::Armed; armedAt = loadMark; armedWhy = why; }
}
// StreamManager's TOC lookup (0x00748A40, thiscall(hash), ret 4): the name pointer of the
// stream a member GUID names, [[stream+0x18]]+4 of the loaded stream.
uint32_t Lookup(uint32_t hash) {
    uint32_t manager = 0;
    if (!lookupOk || !Read(base + kStreamManager, &manager, 4) || !manager) return 0;
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t)>(base + kStreamLookupRva)(reinterpret_cast<void*>(manager), hash);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
uint32_t MemberHash(const uint32_t* g) {   // as Set::SetDesired builds it (0x00758CF9)
    uint32_t h = g[0];
    for (unsigned i = 1; i < 4; ++i) h = h * 33u + g[i];
    return h;
}
struct Loaded { uint32_t handle, stream, name; };
unsigned LoadedStreams(Loaded* out, unsigned max) {
    uint32_t manager = 0, table = 0, buckets = 0;
    if (!Read(base + kStreamManager, &manager, 4) || !manager || !Read(manager + 0x5C, &table, 4) || !Read(manager + 0x60, &buckets, 4) || !table ||
        !buckets || buckets > 4096) return 0;
    unsigned n = 0;
    for (uint32_t b = 0; b < buckets && n < max; ++b) {
        uint32_t node = 0;
        if (!Read(table + 4 * b, &node, 4)) continue;
        for (unsigned hops = 0; node && hops < 64 && n < max; ++hops) {
            uint32_t handle = 0, stream = 0, slot = 0, identity = 0, name = 0, next = 0;
            Read(node, &handle, 4);
            if (Read(node + 4, &stream, 4) && stream && Read(stream + 0x18, &slot, 4) && slot && Read(slot, &identity, 4) && identity &&
                Read(identity + 4, &name, 4) && name)
                out[n++] = Loaded{handle, stream, name};
            if (!Read(node + 8, &next, 4)) break;
            node = next;
        }
    }
    return n;
}
struct Rooms { bool fresh; unsigned members, registered, loaded, visible, shown, leftHidden; uint32_t missing[4]; };
Rooms Check(DWORD now, bool show) {
    Rooms r{};
    r.fresh = AreaValid() && (!loadMark || static_cast<LONG>(areaAt - loadMark) >= 0);
    const uint32_t node = r.fresh ? mirror::ObjectByGuid(area) : 0;
    uint32_t count = 0;
    if (!node || !Read(node + kNodeMemberCount, &count, 4) || count > kMaxMembers) { r.fresh = false; return r; }
    static Loaded table[512];
    const unsigned n = LoadedStreams(table, 512);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t guid[4]{}, flags = 0;
        if (!Read(node + kNodeMembers + i * kNodeMemberStride, guid, 16) || GuidZero(guid)) continue;
        Read(node + kNodeMembers + i * kNodeMemberStride + kMemberFlags, &flags, 4);
        ++r.members;
        const uint32_t name = Lookup(MemberHash(guid));
        if (!name) continue;   // in no mounted TOC: nothing to wait for
        ++r.registered;
        const Loaded* hit = nullptr;
        for (unsigned k = 0; k < n && !hit; ++k) if (table[k].name == name) hit = &table[k];
        uint32_t status = 0;
        if (!hit || !Read(hit->stream + 0x24, &status, 4) || status != kStatusLoaded) {
            if (GuidZero(r.missing)) std::memcpy(r.missing, guid, 16);
            continue;
        }
        ++r.loaded;
        if (vis::Visible(hit->stream)) { ++r.visible; continue; }
        char text[96]{};
        const bool partnerShows = Read(name, text, sizeof(text) - 1) && vis::PartnerShows(vis::NameHash(text), now);
        if (!show || !showHidden || (!(flags & kSpawnHidden) && !partnerShows) || !vis::SetVisible(hit->handle, true)) { ++r.leftHidden; continue; }
        ++r.shown;
        InterlockedIncrement(&shownRooms);
        if (InterlockedIncrement(&showLines) <= 60)
            gate::Event("coop_streams curtain: room %s (handle %08X) of set %08X-%08X was hidden (%s): shown", text[0] ? text : "?", hit->handle, area[0], area[1],
                        (flags & kSpawnHidden) ? "the set spawned it hidden" : "a partner's game shows it");
    }
    return r;
}
// For the loading barrier (runtime_coop_loadsync.inc), at the loading screen: are the rooms of the
// set completed since the load (the checkpoint's own set) in? 1 yes, 0 not yet, -1 not known
// (no set completed since the load, or the lookup did not match). Shows nothing.
int BarrierRooms(DWORD now, unsigned* loaded, unsigned* registered, uint32_t* missing) {
    if (!lookupOk) return -1;
    const Rooms r = Check(now, false);
    if (loaded) *loaded = r.loaded;
    if (registered) *registered = r.registered;
    if (missing) std::memcpy(missing, r.missing, 16);
    if (!r.fresh) return -1;
    return r.loaded == r.registered ? 1 : 0;
}
void Lift(DWORD now, const char* why, volatile LONG* counter, const Rooms* r) {
    if (phase == Phase::Holding) {
        overlay::LiftCurtain();
        focusrate::PanelHolds(kInputBit, false);
        InterlockedExchange(&holding, 0);
        InterlockedIncrement(counter);
        if (InterlockedIncrement(&lines) <= 100) {
            if (r)
                gate::Event("coop_streams curtain lifted after %lu ms: %s (set %08X-%08X asked by %s, fresh=%d: %u members, %u in a TOC, %u loaded, %u visible, %u shown, %u left hidden by the level%s%08X-%08X)",
                            static_cast<unsigned long>(TicksSince(now, since)), why, area[0], area[1], KindName(areaKind), r->fresh ? 1 : 0, r->members, r->registered,
                            r->loaded, r->visible, r->shown, r->leftHidden, GuidZero(r->missing) ? "; none missing " : "; first missing ", r->missing[0], r->missing[1]);
            else
                gate::Event("coop_streams curtain lifted after %lu ms: %s", static_cast<unsigned long>(TicksSince(now, since)), why);
        }
    }
    phase = Phase::Idle;
}
const char* Refusal() {
    if (IsHost()) return "the host";
    if (!inLevel) return "not in a level";
    if (mpmap::InVisit()) return "an MP map (its arrival curtain)";
    if (InterlockedCompareExchange(&arena::stateMirror, 0, 0) != 0) return "a campaign arena (its start curtain)";
    if (GhostFollower() || necro::LocalRole() != 0) return "a necro or a watcher";
    if (!lookupOk) return "the stream lookup did not match";
    return nullptr;
}
bool wasInLevel = false;
void Tick(DWORD now) {
    if (!enabled) return;
    // A load is a level coming in (from the menu, a level change) or a restart (MarkLoad).
    if (inLevel != wasInLevel) {
        wasInLevel = inLevel;
        if (!inLevel) {
            loadMark = Stamp(now);
            if (phase == Phase::Holding) Lift(now, "the level went away", &liftedOther, nullptr);
            phase = Phase::Idle;
        } else if (phase == Phase::Idle) {
            phase = Phase::Armed;
            armedAt = Stamp(now);
            armedWhy = "a level load";
        }
    }
    const uint32_t me = LocalCampaignPlayer();
    if (phase == Phase::Idle) return;
    if (phase == Phase::Holding && (!inLevel || !me)) { Lift(now, "the level went away", &liftedOther, nullptr); return; }
    if (TicksSince(now, lastPass) < kPassMs) return;
    lastPass = now;
    if (phase == Phase::Armed) {
        if (!me || !loadsync::LevelStarted()) {   // still at the loading screen
            if (TicksSince(now, armedAt) > 120000) phase = Phase::Idle;
            return;
        }
        if (const char* no = Refusal()) {
            phase = Phase::Idle;
            InterlockedIncrement(&skipped);
            if (InterlockedIncrement(&lines) <= 100) gate::Event("coop_streams curtain not held after %s: %s", armedWhy, no);
            return;
        }
        const Rooms r = Check(now, true);
        if (r.fresh && r.loaded == r.registered) {
            phase = Phase::Idle;
            InterlockedIncrement(&skipped);
            if (InterlockedIncrement(&lines) <= 100)
                gate::Event("coop_streams curtain not needed after %s: set %08X-%08X already in (%u of %u loaded, %u shown)", armedWhy, area[0], area[1], r.loaded,
                            r.registered, r.shown);
            return;
        }
        phase = Phase::Holding;
        since = Stamp(now);
        overlay::Curtain(holdMs + 1500);   // falls by itself should this module stop
        focusrate::PanelHolds(kInputBit, true);
        InterlockedExchange(&holding, 1);
        InterlockedIncrement(&holds);
        if (InterlockedIncrement(&lines) <= 100)
            gate::Event("coop_streams curtain held after %s: set %08X-%08X fresh=%d, %u of %u rooms loaded, %u visible (at most %lu ms)", armedWhy, area[0], area[1],
                        r.fresh ? 1 : 0, r.loaded, r.registered, r.visible, static_cast<unsigned long>(holdMs));
        return;
    }
    const Rooms r = Check(now, true);
    if (r.fresh && r.loaded == r.registered) { Lift(now, r.registered ? "the rooms around Isaac are loaded and visible" : "no member of the set is in a TOC", &liftedReady, &r); return; }
    if (TicksSince(now, since) >= holdMs) Lift(now, r.fresh ? "TIMEOUT, rooms still loading" : "TIMEOUT, no stream set completed since the load", &liftedTimeout, &r);
}
}  // namespace curtain

// Game thread, from Poll.
void Tick(DWORD now) {
    curtain::Tick(now);
    if (!inLevel || !levelSettled) pin::ForgetDesired();
    if (!installed || !enabled) return;
    const bool preparingScene = mirror::agreement::PreparingRooms(now);
    if (!preparingScene) ApproachTick(now);
    UnionTick(now);
    // A held activation never outlives its level: its node goes with it.
    if (held.used && !inLevel) { held = Held{}; noticeShown = false; }
    WipeStaleArea();
    LeaveTick(now);
    vis::Tick(now);
    if (!preparingScene) NearTick(now);
    if (held.used && now - held.lastTry >= kRetryMs) {
        held.lastTry = now;
        char who[96] = "";
        if (!peerTable.Count() || PartnersFit(held.memberGuids, held.members, now, who, sizeof(who))) Release(false);
        else if (PartnerComesToo(held.guid, now)) {
            InterlockedIncrement(&movedTogether);
            gate::Event("coop_streams set %08X-%08X: the partner asks for it too, released after %u ms", held.guid[0], held.guid[1],
                        static_cast<unsigned>(now - held.since));
            Release(false);
        }
        else if (held.kind == kKindScript && now - held.since >= netclock::Window(holdScriptMs)) {
            InterlockedIncrement(&scriptHoldsEnded);
            gate::Event("coop_streams set %08X-%08X asked by a script: released after %u ms without the partner", held.guid[0], held.guid[1],
                        static_cast<unsigned>(now - held.since));
            Release(true);
        }
        else if (now - held.since >= holdTimeoutMs) Release(true);
        else if (!noticeShown && now - held.since >= 1500) { noticeShown = true; notice::Show("WAITING FOR PARTNER TO FOLLOW"); }
    }
    if (!peerTable.Count() || now - lastSend < kSendMs) return;
    lastSend = now;
    if (AreaValid() || held.used) {
        Packet p{kMagic, kVersion, peerReadiness.token, ++sequence, 0, ++epoch,
                 {area[0], area[1], area[2], area[3]}, activationHash, activationFlags,
                 static_cast<uint32_t>(TicksSince(now, areaAt)), {}, areaKind};
        if (held.used) std::memcpy(p.pending, held.guid, 16);
        if (SendToPeers(&p, sizeof(p))) InterlockedIncrement(&sent);
    }
    if (IsHost()) return;
    if (!hostAt || TicksSince(now, hostAt) > kStaleMs || !inLevel || !levelSettled || GuidZero(area)) return;
    if (GuidEqual(area, host.area)) {
        if (mismatchSince) {
            InterlockedIncrement(&matches);
            gate::Event("coop_streams same area as the host again after %u ms", static_cast<unsigned>(now - mismatchSince));
        }
        mismatchSince = 0; std::memset(lastLoggedArea, 0, 16);
        return;
    }
    if (!mismatchSince) mismatchSince = now;
    if (!GuidEqual(lastLoggedArea, host.area)) {
        std::memcpy(lastLoggedArea, host.area, 16);
        InterlockedIncrement(&mismatches);
        gate::Event("coop_streams area differs: ours=%08X-%08X-%08X-%08X (%u ms ago) host=%08X-%08X-%08X-%08X (%u ms ago, activation=%08X registered_here=%d)",
                    area[0], area[1], area[2], area[3], static_cast<unsigned>(now - areaAt),
                    host.area[0], host.area[1], host.area[2], host.area[3], host.completedAgoMs, host.activationHash,
                    host.activationHash ? (gate::Registered(host.activationHash) ? 1 : 0) : -1);
    }
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Streams", "Compare", 1, ini.c_str()) != 0;
    holdEnabled = GetPrivateProfileIntA("Streams", "HoldForPartners", 1, ini.c_str()) != 0;
    holdTimeoutMs = static_cast<DWORD>(GetPrivateProfileIntA("Streams", "HoldTimeoutMs", 180000, ini.c_str()));
    holdScriptMs = static_cast<DWORD>(GetPrivateProfileIntA("Streams", "HoldScriptTimeoutMs", 4000, ini.c_str()));
    std::fprintf(log, "coop_streams hold_script_timeout_ms=%lu (a script-asked set waits at most this long for the partner; a scene's own set never waits)\n",
                 static_cast<unsigned long>(holdScriptMs));
    followPartners = GetPrivateProfileIntA("Streams", "FollowPartners", 1, ini.c_str()) != 0;
    followReach = static_cast<float>(GetPrivateProfileIntA("Streams", "FollowReach", 15, ini.c_str()));
    leave::enabled = GetPrivateProfileIntA("Streams", "DoorLeaveFix", 1, ini.c_str()) != 0;
    leave::holdMs = static_cast<DWORD>(GetPrivateProfileIntA("Streams", "DoorLeaveHoldMs", 15000, ini.c_str()));
    farDoorGuard = GetPrivateProfileIntA("Streams", "FarDoorGuard", 1, ini.c_str()) != 0;
    farDoorM = static_cast<float>(GetPrivateProfileIntA("Streams", "FarDoorM", 30, ini.c_str()));
    doorApproach = GetPrivateProfileIntA("Streams", "DoorApproachLoads", 1, ini.c_str()) != 0;
    approachM = static_cast<float>(GetPrivateProfileIntA("Streams", "DoorApproachM", 6, ini.c_str()));
    // The host barrier waits for the rooms under the native loading screen; the curtain is its old stand-in.
    const bool barrier = GetPrivateProfileIntA("Loading", "HostBarrier", 1, ini.c_str()) != 0;
    curtain::enabled = GetPrivateProfileIntA("Loading", "CurtainUntilRoomsLoaded", barrier ? 0 : 1, ini.c_str()) != 0;
    curtain::holdMs = static_cast<DWORD>(GetPrivateProfileIntA("Loading", "CurtainHoldMs", 10000, ini.c_str()));
    curtain::showHidden = GetPrivateProfileIntA("Loading", "CurtainShowHiddenRooms", 1, ini.c_str()) != 0;
    {
        // mov edx,[esp+4] / push esi / xor esi,esi / push esi / push ecx / mov eax,esp / mov [eax],edx / call
        static const unsigned char kLookup[] = {0x8B, 0x54, 0x24, 0x04, 0x56, 0x33, 0xF6, 0x56, 0x51, 0x8B, 0xC4, 0x89, 0x10, 0xE8};
        unsigned char actual[sizeof(kLookup)]{};
        curtain::lookupOk = Read(base + curtain::kStreamLookupRva, actual, sizeof(actual)) &&
                            hook_site::Inspect(actual, kLookup, sizeof(kLookup)) != hook_site::State::Foreign;
        std::fprintf(log, "coop_streams curtain_until_rooms_loaded=%d hold_ms=%lu show_hidden_rooms=%d stream lookup at %08X %s "
                          "(a client's level start or restart stays black, input held, until the rooms of the first set completed since are loaded)\n",
                     curtain::enabled ? 1 : 0, static_cast<unsigned long>(curtain::holdMs), curtain::showHidden ? 1 : 0,
                     static_cast<unsigned>(base + curtain::kStreamLookupRva), curtain::lookupOk ? "matches" : "SIGNATURE MISMATCH - curtain off");
    }
    vis::enabled = GetPrivateProfileIntA("Streams", "VisibilityFollowsWalker", 1, ini.c_str()) != 0;
    vis::unionEnabled = vis::enabled && GetPrivateProfileIntA("Streams", "VisibilityUnion", 0, ini.c_str()) != 0;
    vis::mpShowAll = GetPrivateProfileIntA("MpMap", "ShowAllRooms", 1, ini.c_str()) != 0;
    vis::keepOwnRoom = GetPrivateProfileIntA("Streams", "KeepOwnRoom", 1, ini.c_str()) != 0;
    if (vis::unionEnabled || vis::mpShowAll || GetPrivateProfileIntA("Arena", "Arenas", 1, ini.c_str()) != 0) {
        // sub esp,0x18 / push esi / mov esi,[esp+0x20] / xor al,al / test esi,esi
        static const unsigned char kVis[] = {0x83, 0xEC, 0x18, 0x56, 0x8B, 0x74, 0x24, 0x20, 0x32, 0xC0, 0x85, 0xF6};
        const bool a = hook_site::Install(log, "coop_streams VisibilityManager::Show", base + vis::kShowRva, kVis, sizeof(kVis),
                                          reinterpret_cast<void*>(vis::OnShow), vis::showHook);
        const bool b = hook_site::Install(log, "coop_streams VisibilityManager::Hide", base + vis::kHideRva, kVis, sizeof(kVis),
                                          reinterpret_cast<void*>(vis::OnHide), vis::hideHook);
        vis::hooked = a && b;
        if (!vis::hooked) vis::unionEnabled = false;   // without both the intent is unknown
    }
    std::fprintf(log, "coop_streams visibility_union=%d (a loaded room stays visible while any player's game shows it)\n",
                 vis::unionEnabled ? 1 : 0);
    std::fprintf(log, "coop_streams keep_own_room=%d (in co-op the room our Isaac stands in is never hidden, line of sight %s)\n", vis::keepOwnRoom && vis::hooked ? 1 : 0,
                 bot_sense::losReady ? "ready" : "not ready yet");
    std::fprintf(log, "coop_streams mp_show_all_rooms=%d (on an MP map the stream sets hide no room)\n", vis::mpShowAll && vis::hooked ? 1 : 0);
    std::fprintf(log, "coop_streams visibility_follows_walker=%d (a player a scene carried through a door takes the rooms of the partner it carried him to)\n", vis::enabled ? 1 : 0);
    std::fprintf(log, "coop_streams door_leave_fix=%d hold_ms=%lu (a door's leave set follows where the players are, a partner's cutscene room first)\n",
                 leave::enabled ? 1 : 0, static_cast<unsigned long>(leave::holdMs));
    // sub esp,0x18 / push esi / mov esi,ecx / cmp byte [esi+0x58],0
    static const unsigned char kSig[] = {0x83,0xEC,0x18,0x56,0x8B,0xF1,0x80,0x7E,0x58,0x00};
    if (!hook_site::Install(log, "coop_streams StreamSet::Activate", base + kActivateRva, kSig, sizeof(kSig),
                            reinterpret_cast<void*>(OnActivate), activateHook)) {
        holdEnabled = false;
        std::fprintf(log, "coop_streams hold off (hook failed)\n");
    }
    pin::Install(log, ini);
    std::fprintf(log, "coop_streams far_door_guard=%d far_door_m=%.0f door_approach_loads=%d door_approach_m=%.0f set_desired_hook=%d "
                      "(a door's set asked for far from our Isaac keeps our rooms or, a leave set, is skipped; an open door we approach loads our side's set)\n",
                 farDoorGuard ? 1 : 0, farDoorM, doorApproach ? 1 : 0, approachM, pin::hooked ? 1 : 0);
    installed = true;
    std::fprintf(log, "coop_streams compare=%d hold_for_partners=%d hold_timeout_ms=%lu follow_partners=%d follow_reach_m=%d (an activation that drops a partner's stream waits; next to the reference his streams are ours)\n",
                 enabled, holdEnabled, static_cast<unsigned long>(holdTimeoutMs), followPartners ? 1 : 0, static_cast<int>(followReach));
    return true;
}

void WriteDiagnostics(FILE* log) {
    if (!installed || !enabled) return;
    static LONG last = -1;
    const LONG activity = InterlockedCompareExchange(&completions, 0, 0) + InterlockedCompareExchange(&mismatches, 0, 0) +
                          InterlockedCompareExchange(&matches, 0, 0) + InterlockedCompareExchange(&heldCount, 0, 0) + InterlockedCompareExchange(&refusedMirrored, 0, 0) +
                          InterlockedCompareExchange(&releasedCount, 0, 0) + InterlockedCompareExchange(&timedOutCount, 0, 0) +
                          InterlockedCompareExchange(&follows, 0, 0) + InterlockedCompareExchange(&followsUnknown, 0, 0) +
                          InterlockedCompareExchange(&followsNear, 0, 0) + InterlockedCompareExchange(&refusedNear, 0, 0) + InterlockedCompareExchange(&allowedNear, 0, 0) +
                          InterlockedCompareExchange(&leave::seen, 0, 0) + InterlockedCompareExchange(&leave::held, 0, 0) + InterlockedCompareExchange(&leave::timedOut, 0, 0) +
                          InterlockedCompareExchange(&vis::carries, 0, 0) + InterlockedCompareExchange(&vis::changes, 0, 0) + InterlockedCompareExchange(&vis::flips, 0, 0) +
                          InterlockedCompareExchange(&farSeen, 0, 0) + InterlockedCompareExchange(&approachActivations, 0, 0) +
                          InterlockedCompareExchange(&curtain::holds, 0, 0) + InterlockedCompareExchange(&curtain::skipped, 0, 0);
    if (activity == last) return;
    std::fprintf(log, "coop_streams far_doors seen=%ld near=%ld merged=%ld skipped=%ld unmerged=%ld | door_approach passes=%ld activated=%ld covered=%ld no_node=%ld\n",
                 InterlockedCompareExchange(&farSeen, 0, 0), InterlockedCompareExchange(&farNear, 0, 0), InterlockedCompareExchange(&farMerged, 0, 0),
                 InterlockedCompareExchange(&farSkipped, 0, 0), InterlockedCompareExchange(&farUnmerged, 0, 0), InterlockedCompareExchange(&approachPasses, 0, 0),
                 InterlockedCompareExchange(&approachActivations, 0, 0), InterlockedCompareExchange(&approachCovered, 0, 0),
                 InterlockedCompareExchange(&approachUnknown, 0, 0));
    std::fprintf(log, "coop_streams curtain holds=%ld lifted_ready=%ld lifted_timeout=%ld lifted_other=%ld not_held=%ld rooms_shown=%ld holding=%ld\n",
                 InterlockedCompareExchange(&curtain::holds, 0, 0), InterlockedCompareExchange(&curtain::liftedReady, 0, 0),
                 InterlockedCompareExchange(&curtain::liftedTimeout, 0, 0), InterlockedCompareExchange(&curtain::liftedOther, 0, 0),
                 InterlockedCompareExchange(&curtain::skipped, 0, 0), InterlockedCompareExchange(&curtain::shownRooms, 0, 0),
                 InterlockedCompareExchange(&curtain::holding, 0, 0));
    std::fprintf(log, "coop_streams mp_map hides refused=%ld arena hides refused=%ld\n", InterlockedCompareExchange(&vis::mpHidesRefused, 0, 0),
                 InterlockedCompareExchange(&vis::arenaHidesRefused, 0, 0));
    std::fprintf(log, "coop_streams visibility changes=%ld doors_known=%u flips=%ld flips_closed_door=%ld flips_walked=%ld carries=%ld adoptions=%ld already_same=%ld walked_away=%ld expired=%ld partner_carried=%ld partner_other_side=%ld sent=%ld received=%ld shown=%ld hidden=%ld faults=%ld\n",
                 InterlockedCompareExchange(&vis::changes, 0, 0), vis::doorCount,
                 InterlockedCompareExchange(&vis::flips, 0, 0), InterlockedCompareExchange(&vis::flipsClosed, 0, 0), InterlockedCompareExchange(&vis::flipsWalked, 0, 0),
                 InterlockedCompareExchange(&vis::carries, 0, 0), InterlockedCompareExchange(&vis::adoptions, 0, 0),
                 InterlockedCompareExchange(&vis::alreadySame, 0, 0), InterlockedCompareExchange(&vis::walkedAway, 0, 0),
                 InterlockedCompareExchange(&vis::expired, 0, 0), InterlockedCompareExchange(&vis::partnerCarried, 0, 0),
                 InterlockedCompareExchange(&vis::partnerOtherSide, 0, 0),
                 InterlockedCompareExchange(&vis::sent, 0, 0), InterlockedCompareExchange(&vis::received, 0, 0),
                 InterlockedCompareExchange(&vis::shown, 0, 0), InterlockedCompareExchange(&vis::hidden, 0, 0),
                 InterlockedCompareExchange(&vis::faults, 0, 0));
    std::fprintf(log, "coop_streams necro_ghost_picture passes=%ld shown=%ld hidden=%ld\n", InterlockedCompareExchange(&vis::ghostPasses, 0, 0),
                 InterlockedCompareExchange(&vis::ghostShown, 0, 0), InterlockedCompareExchange(&vis::ghostHidden, 0, 0));
    std::fprintf(log, "coop_streams door_leave seen=%ld kept=%ld swapped=%ld held=%ld released=%ld timed_out=%ld reused=%ld unreadable=%ld calibrations=%ld ghost_tests=%ld plane_tests=%ld front_sign=%d holding=%08X\n",
                 InterlockedCompareExchange(&leave::seen, 0, 0), InterlockedCompareExchange(&leave::kept, 0, 0), InterlockedCompareExchange(&leave::swapped, 0, 0),
                 InterlockedCompareExchange(&leave::held, 0, 0), InterlockedCompareExchange(&leave::released, 0, 0), InterlockedCompareExchange(&leave::timedOut, 0, 0),
                 InterlockedCompareExchange(&leave::reused, 0, 0), InterlockedCompareExchange(&leave::unreadable, 0, 0), InterlockedCompareExchange(&leave::calibrations, 0, 0),
                 InterlockedCompareExchange(&leave::ghostTests, 0, 0), InterlockedCompareExchange(&leave::planeTests, 0, 0), leave::frontSign, leave::hold.used ? leave::hold.door : 0u);
    pin::WriteDiagnostics(log);
    if (vis::keepOwnRoom && vis::hooked)
        std::fprintf(log, "coop_streams own_room samples=%ld unknown=%ld changes=%ld hides_refused=%ld shown_again=%ld now=%08X\n",
                     InterlockedCompareExchange(&vis::homeSamples, 0, 0), InterlockedCompareExchange(&vis::homeUnknown, 0, 0),
                     InterlockedCompareExchange(&vis::homeChanges, 0, 0), InterlockedCompareExchange(&vis::ownHidesRefused, 0, 0),
                     InterlockedCompareExchange(&vis::ownRoomsShown, 0, 0), vis::home[0]);
    if (vis::unionEnabled)
        std::fprintf(log, "coop_streams visibility_union shown=%ld hidden_again=%ld hides_held=%ld game_calls=%ld intents=%u\n",
                     InterlockedCompareExchange(&vis::unionShown, 0, 0), InterlockedCompareExchange(&vis::unionHidden, 0, 0),
                     InterlockedCompareExchange(&vis::hidesHeld, 0, 0), InterlockedCompareExchange(&vis::intentsSeen, 0, 0), vis::intentCount);
    std::fprintf(log, "coop_streams scene follows given back=%ld kept=%ld (a followed set returns when our Isaac was not carried)\n",
                 InterlockedCompareExchange(&followsReverted, 0, 0), InterlockedCompareExchange(&followsKept, 0, 0));
    std::fprintf(log, "coop_streams scene_follows=%ld scene_activations=%ld script_holds_ended=%ld\n", InterlockedCompareExchange(&sceneFollows, 0, 0),
                 InterlockedCompareExchange(&sceneActivations, 0, 0), InterlockedCompareExchange(&scriptHoldsEnded, 0, 0));
    std::fprintf(log, "coop_streams near_reference follows=%ld refused=%ld mirrored_allowed=%ld area_wiped=%ld area_map='%s' reference_slot=%u for=%u ms\n",
                 InterlockedCompareExchange(&followsNear, 0, 0), InterlockedCompareExchange(&refusedNear, 0, 0), InterlockedCompareExchange(&allowedNear, 0, 0),
                 InterlockedCompareExchange(&areaWiped, 0, 0), areaMap, referenceSlot, referenceUntil && GetTickCount() < referenceUntil ? static_cast<unsigned>(referenceUntil - GetTickCount()) : 0u);
    last = activity;
    std::fprintf(log, "coop_streams completions=%ld activations=%ld activate_calls=%ld passed=%ld held=%ld released=%ld timed_out=%ld replaced=%ld refused_mirrored=%ld follows=%ld follows_unknown=%ld "
                      "sent=%ld received=%ld mismatches=%ld matches=%ld area=%08X-%08X-%08X-%08X pending=%08X-%08X host_area=%08X-%08X-%08X-%08X mismatch_for=%u ms\n",
                 InterlockedCompareExchange(&completions, 0, 0), InterlockedCompareExchange(&activations, 0, 0),
                 InterlockedCompareExchange(&activateCalls, 0, 0), InterlockedCompareExchange(&passedCount, 0, 0),
                 InterlockedCompareExchange(&heldCount, 0, 0), InterlockedCompareExchange(&releasedCount, 0, 0),
                 InterlockedCompareExchange(&timedOutCount, 0, 0), InterlockedCompareExchange(&replacedCount, 0, 0),
                 InterlockedCompareExchange(&refusedMirrored, 0, 0), InterlockedCompareExchange(&follows, 0, 0), InterlockedCompareExchange(&followsUnknown, 0, 0),
                 InterlockedCompareExchange(&sent, 0, 0), InterlockedCompareExchange(&received, 0, 0),
                 InterlockedCompareExchange(&mismatches, 0, 0), InterlockedCompareExchange(&matches, 0, 0),
                 area[0], area[1], area[2], area[3], held.used ? held.guid[0] : 0u, held.used ? held.guid[1] : 0u,
                 host.area[0], host.area[1], host.area[2], host.area[3],
                 mismatchSince ? static_cast<unsigned>(GetTickCount() - mismatchSince) : 0u);
}
}  // namespace streams
