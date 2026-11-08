// Dead Space 2 United by Richard Hafer.
// The host grants one owner for a shared scene. The owner starts after every
// current participant has voted for this grant in the same loaded world.
namespace agreement {
constexpr uint32_t kMagic = 0x31414753;  // SGA1
enum Kind : uint32_t { Hello = 1, Offer, Grant, Vote };
struct Packet {
    uint32_t magic, version, sender, kind, map, generation;
    uint32_t hash, attempt, owner, ownerGeneration, hostGeneration, set[4];
    uint32_t ready, serial;
};
static_assert(sizeof(Packet) == sizeof(mirror::Packet), "Scene agreement uses the mirror receive branch");

constexpr DWORD kRetry = 250, kLease = 4000, kCapabilityAge = 10000;
constexpr float kGatherRadius = 12.0f;
constexpr uint32_t kChurchHandoff = 1, kCampaignSceneUpdate = 2, kCommitReceipt = 4;
constexpr uint32_t kEllieDepartCue = 0xB7491B11;
constexpr uint32_t kEllieAimCue = 0xDBC76F4B;
struct Capability { uint32_t token, map, generation, serial; DWORD seen; bool storyHuman; uint32_t features; };
struct VoteState { uint32_t token, generation, serial; DWORD at; bool ready; };
struct Reservation {
    Packet offer{};
    DWORD electedAt = 0, lastOffer = 0, lastGrant = 0, grantSeen = 0;
    DWORD lastVote = 0, lastFollow = 0, votedAt = 0, firstRoomCheck = 0;
    uint32_t votedGeneration = 0;
    DWORD readyAt = 0;
    DWORD lastPlacement = 0;
    bool placementFailed = false;
    uint32_t readyGeneration = 0;
    bool granted = false;
    const char* lastReason = nullptr;
    VoteState votes[coop_peers::kMaxPeers]{};
};
struct Candidate { uint32_t hash, attempt, map, generation; DWORD at, sent; bool arrived = false; };
Capability capabilities[coop_peers::kMaxPeers]{};
Reservation reservations[32]{};
Candidate candidates[32]{};
struct Finished { uint32_t hash, owner, attempt, map, generation; DWORD at; };
Finished finished[64]{};
unsigned nextFinished = 0;
uint32_t serial = 0;
uint32_t grantSender = 0, grantSerial = 0;
DWORD lastHello = 0;
unsigned logLines = 0;

bool Recent(DWORD now, DWORD at, DWORD age) { return at && TicksSince(now, at) <= age; }
bool SameGeneration(uint32_t a, uint32_t b) { return !a || !b || a == b; }
bool Shared(uint32_t hash) {
    const auto p = scene_catalog::PolicyFor(hash);
    return p == scene_catalog::Policy::Pull || p == scene_catalog::Policy::Gather;
}
const uint32_t* StartSet(uint32_t hash, const uint32_t* current) {
    // The church door fires the player pull before its scripted set finishes loading.
    static constexpr uint32_t churchIGC[] = {0x34CB2202, 0x703E5DB9, 0x54544F43, 0x33363053};
    return hash == 0x39C2AFA6 ? churchIGC : current;
}
bool HumanRoute() {
    return loadsync::SceneSyncCapable() && !play::SurvivalMode() && !play::VersusMode() &&
           !mpmap::InVisit() && !spectate::Active() && necro::LocalRole() == 0;
}
bool LivePeer(unsigned slot, DWORD now) {
    const auto& peer = peerTable.peers[slot];
    return peer.used && peer.token && Recent(now, peer.lastSeen, kCapabilityAge);
}
unsigned Slot(uint32_t token) {
    const auto* peer = peerTable.FindByToken(token);
    return peer ? PeerSlot(peer) : coop_peers::kMaxPeers;
}
bool World(const Packet& p) {
    return p.map && p.map == loadsync::SceneMapHash() &&
           SameGeneration(p.generation, loadsync::SceneGeneration()) &&
           SameGeneration(p.ownerGeneration, loadsync::SceneGeneration());
}
bool Same(const Packet& a, const Packet& b) {
    return a.hash == b.hash && a.owner == b.owner && a.attempt == b.attempt && a.map == b.map &&
           a.ownerGeneration == b.ownerGeneration && streams::GuidEqual(a.set, b.set);
}
bool Done(const Packet& p, DWORD now) {
    for (const auto& f : finished)
        if (f.hash == p.hash && f.owner == p.owner && f.attempt == p.attempt && f.map == p.map &&
            f.generation == p.ownerGeneration && Recent(now, f.at, 15000)) return true;
    return false;
}
Reservation* Find(uint32_t hash, bool create = false) {
    Reservation* free = nullptr;
    for (auto& r : reservations) {
        if (r.offer.hash == hash) return &r;
        if (!r.offer.hash && !free) free = &r;
    }
    return create ? free : nullptr;
}
Candidate* Local(uint32_t hash, bool create = false) {
    Candidate* free = nullptr;
    for (auto& c : candidates) {
        if (c.hash == hash) return &c;
        if (!c.hash && !free) free = &c;
    }
    return create ? free : nullptr;
}
void Clear() {
    std::fill(std::begin(reservations), std::end(reservations), Reservation{});
    std::fill(std::begin(candidates), std::end(candidates), Candidate{});
    std::fill(std::begin(finished), std::end(finished), Finished{});
    // Capabilities describe the connection and survive a map change.
    lastHello = 0;
    logLines = 0;
}
bool Available(DWORD now, uint32_t required = 0) {
    if (!HumanRoute()) return false;
    bool any = false;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        if (!peerTable.peers[s].used) continue;
        if (!LivePeer(s, now) || !capabilities[s].storyHuman || capabilities[s].token != peerTable.peers[s].token || !Recent(now, capabilities[s].seen, kCapabilityAge)) return false;
        if ((capabilities[s].features & required) != required) return false;
        any = true;
    }
    return any;
}
bool SupportsCue(uint32_t hash, DWORD now) {
    const bool newRoute = hash == kEllieDepartCue || hash == kEllieAimCue;
    return Available(now, newRoute ? kCampaignSceneUpdate : 0);
}
void StampPacket(Packet& p, Kind kind) {
    p.magic = kMagic; p.version = 1; p.kind = kind; p.sender = peerReadiness.token;
    p.generation = loadsync::SceneGeneration();
    p.serial = ++serial;
}
void Send(Packet p, Kind kind) {
    StampPacket(p, kind);
    SendToPeers(&p, sizeof(p));
}
void Propose(const Packet& p, DWORD now) {
    if (Done(p, now)) return;
    Reservation* r = Find(p.hash, true);
    if (!r) return;
    if (Same(r->offer, p)) {
        r->lastOffer = Stamp(now);
        if (p.ready < r->offer.ready) r->offer.ready = p.ready;
        return;
    }
    const bool better = p.ready < r->offer.ready || (p.ready == r->offer.ready && p.owner < r->offer.owner);
    if (r->offer.hash && Recent(now, r->lastOffer, kLease) && (r->granted || !better)) return;
    const DWORD first = r->offer.hash && !r->granted && Recent(now, r->lastOffer, kLease) ? r->electedAt : Stamp(now);
    *r = Reservation{};
    r->offer = p;
    r->offer.hostGeneration = loadsync::SceneGeneration();
    r->lastOffer = Stamp(now);
    r->electedAt = first;
}
void Request(uint32_t hash, uint32_t attempt, const uint32_t* set, DWORD now, uint32_t rank = 1) {
    if (!hash || !attempt || !set || streams::GuidZero(set) || !Shared(hash)) return;
    Candidate* c = Local(hash, true);
    if (!c) return;
    const uint32_t map = loadsync::SceneMapHash(), gen = loadsync::SceneGeneration();
    if (!map) return;
    if (c->attempt != attempt || c->map != map || c->generation != gen) *c = Candidate{hash, attempt, map, gen};
    if (!rank) c->arrived = true;
    c->at = Stamp(now);
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s)
        if (LivePeer(s, now) && cutscene::FollowsSlot(s)) return;
    if (Recent(now, c->sent, kRetry)) return;
    c->sent = Stamp(now);
    Packet p{};
    p.hash = hash; p.attempt = attempt; p.map = map;
    p.owner = peerReadiness.token; p.ownerGeneration = gen; p.ready = rank;
    std::memcpy(p.set, set, sizeof(p.set));
    StampPacket(p, Offer);
    if (IsHost()) Propose(p, now);
    else SendToPeers(&p, sizeof(p));
}
// Both retail igc_3_03 player sequences have this world position.
constexpr float churchStart[] = {-3.33f, -51.998f, -434.066f};
bool MayPrepareRooms(const Packet& p, DWORD now) {
    if (p.owner == peerReadiness.token || scene_catalog::PolicyFor(p.hash) == scene_catalog::Policy::Pull) return true;
    const unsigned owner = Slot(p.owner);
    if (owner >= coop_peers::kMaxPeers) return false;
    if (cutscene::FollowsSlot(owner)) return true;
    const auto* c = Local(p.hash);
    if (c && c->arrived && Recent(now, c->at, kLease) && c->map == p.map && c->generation == loadsync::SceneGeneration()) return true;
    float here[3]{};
    return Recent(now, peerRuntime[owner].lastState, 2000) && gate::LocalPosition(here) &&
           coop_gate::DistanceSquared(here, peerRuntime[owner].pos) <= kGatherRadius * kGatherRadius;
}
bool PreparingRooms(DWORD now) {
    for (const auto& r : reservations) {
        if (r.granted && (IsHost() || r.offer.serial == grantSerial) &&
            Recent(now, r.grantSeen, kLease) && peerTable.Count() && World(r.offer) && HumanRoute() && MayPrepareRooms(r.offer, now))
            return true;
    }
    return false;
}
const char* Blocked(Reservation& r, DWORD now, bool prepare = false) {
    const Packet& p = r.offer;
    const uint32_t body = LocalSlotZeroEntity();
    if (!inLevel || !levelSettled || !HumanRoute() || !body || nameplate::Dead(body)) return "player unavailable";
    if (!World(p)) return "different world";
    if ((p.hash == kEllieDepartCue || p.hash == kEllieAimCue) && !SupportsCue(p.hash, now)) return "partner scene support pending";
    const bool ours = p.owner == peerReadiness.token;
    const unsigned owner = Slot(p.owner);
    if (!ours && owner >= coop_peers::kMaxPeers) return "unknown owner";
    const bool follows = !ours && cutscene::FollowsSlot(owner);
    if (!ours && !follows && cutscene::SequenceRunningNow()) return "another scene";
    if (!MayPrepareRooms(p, now)) return "gather at scene";
    if (!streams::Covers(p.set)) {
        if (prepare && !Recent(now, r.lastFollow, 2000)) {
            r.lastFollow = Stamp(now);
            streams::Follow(p.set);
        }
        return "loading scene set";
    }
    if (!gate::Registered(p.hash)) return "cue unavailable";
    if (p.hash == 0x39C2AFA6) {
        if (!r.firstRoomCheck) r.firstRoomCheck = Stamp(now);
        if (TicksSince(now, r.firstRoomCheck) < 15000 && ChurchActorsSettling(now)) return "scene actors settling";
        if (!ours) {
            if (!streams::GuidEqual(p.set, StartSet(p.hash, nullptr))) return "wrong church scene set";
            float pos[3]{};
            if (!gate::LocalPosition(pos)) return "church position unavailable";
            const float gapSq = coop_gate::DistanceSquared(pos, churchStart);
            if (!std::isfinite(gapSq)) return "invalid church position";
            if (gapSq > 1.0f || r.placementFailed) {
                if (prepare && !cutscene::SequenceRunningNow() && !Recent(now, r.lastPlacement, 1000)) {
                    r.lastPlacement = Stamp(now);
                    const char* why = catchup::StageChurchIsaac(body, churchStart);
                    r.placementFailed = why != nullptr;
                    if (why) return why;
                }
                return "waiting for church placement";
            }
        }
    }
    return nullptr;
}
void Cast(Reservation& r, DWORD now) {
    if (!r.granted || !Recent(now, r.grantSeen, kLease) || Recent(now, r.lastVote, kRetry)) return;
    r.lastVote = Stamp(now);
    const char* why = Blocked(r, now, true);
    if ((!r.votedAt || r.lastReason != why) && logLines < 100) {
        ++logLines;
        gate::Event("scene agreement cue=%08X attempt=%u owner=%08X set=%08X-%08X status=%s",
                    r.offer.hash, r.offer.attempt, r.offer.owner, r.offer.set[0], r.offer.set[1], why ? why : "ready");
    }
    r.lastReason = why;
    r.votedAt = Stamp(now);
    r.votedGeneration = loadsync::SceneGeneration();
    if (!why) {
        r.readyAt = Stamp(now);
        r.readyGeneration = r.votedGeneration;
    }
    if (!IsHost()) {
        Packet p = r.offer; p.ready = !why;
        Send(p, Vote);
    }
}
bool AwaitingLeader(uint32_t hash, DWORD now) {
    const auto* r = Find(hash);
    return r && r->granted && r->offer.owner != peerReadiness.token && Recent(now, r->grantSeen, kLease) && World(r->offer);
}
bool GroupReady(Reservation& r, DWORD now) {
    if (Blocked(r, now)) return false;
    bool any = false;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        if (!peerTable.peers[s].used) continue;
        const auto& v = r.votes[s];
        const auto& cap = capabilities[s];
        if (!LivePeer(s, now) || !v.ready || v.token != peerTable.peers[s].token || !Recent(now, v.at, kLease / 2) ||
            !cap.storyHuman || cap.token != v.token || cap.generation != v.generation || cap.map != r.offer.map) return false;
        any = true;
    }
    return any;
}
bool Ready(uint32_t hash, uint32_t attempt, DWORD now) {
    auto* r = Find(hash);
    if (!r || !r->granted || r->offer.owner != peerReadiness.token || r->offer.attempt != attempt ||
        r->offer.ownerGeneration != loadsync::SceneGeneration() || !Recent(now, r->grantSeen, kLease)) return false;
    return IsHost() ? GroupReady(*r, now) : r->offer.serial == grantSerial && r->offer.ready && !Blocked(*r, now);
}
bool AcceptCommit(uint32_t hash, uint32_t attempt, uint32_t owner, DWORD now) {
    auto* r = Find(hash);
    if (!r || !r->granted || r->offer.owner != owner || r->offer.attempt != attempt || !World(r->offer) ||
        r->readyGeneration != loadsync::SceneGeneration() || !Recent(now, r->readyAt, kLease) ||
        !Recent(now, r->grantSeen, kLease)) return false;
    const uint32_t body = inLevel && levelSettled && HumanRoute() ? LocalSlotZeroEntity() : 0;
    if (!body || nameplate::Dead(body)) return false;
    if (r->votedGeneration == loadsync::SceneGeneration() && !r->lastReason && Recent(now, r->votedAt, kLease)) return true;
    const char* why = Blocked(*r, now);
    return !why || std::strcmp(why, "loading scene set") == 0;
}
void Consume(uint32_t hash, uint32_t owner) {
    auto* r = Find(hash);
    if (!r || r->offer.owner != owner) return;
    const auto& p = r->offer;
    finished[nextFinished++ % std::size(finished)] = Finished{hash, owner, p.attempt, p.map, p.ownerGeneration, Stamp(GetTickCount())};
    *r = Reservation{};
    if (auto* c = Local(hash)) *c = Candidate{};
}
void Tick(DWORD now) {
    if (!peerTable.Count() || !peerReadiness.token) return;
    if (!Recent(now, lastHello, 1000)) {
        lastHello = Stamp(now);
        Packet p{}; p.map = loadsync::SceneMapHash(); p.ready = HumanRoute(); p.hash = kChurchHandoff | kCampaignSceneUpdate | kCommitReceipt;
        Send(p, Hello);
    }
    for (auto& c : candidates) if (c.hash && !Recent(now, c.at, kLease)) c = Candidate{};
    Reservation* next = nullptr;
    for (auto& r : reservations) {
        if (!r.offer.hash) continue;
        if (!Recent(now, IsHost() ? r.lastOffer : r.grantSeen, kLease) ||
            (r.offer.owner != peerReadiness.token && !KnownPeerToken(r.offer.owner))) {
            r = Reservation{};
            continue;
        }
        if (!IsHost()) {
            if (r.offer.serial == grantSerial) Cast(r, now);
            continue;
        }
        // An issued grant owns the slot until commit or expiry. Ties go to the smaller cue hash.
        if (!next || r.granted || (!next->granted &&
            (static_cast<LONG>(r.electedAt - next->electedAt) < 0 ||
             (r.electedAt == next->electedAt && r.offer.hash < next->offer.hash)))) next = &r;
    }
    if (next && TicksSince(now, next->electedAt) >= netclock::Window(120, 500)) {
        if (!Recent(now, next->lastGrant, kRetry)) {
            next->granted = true;
            next->grantSeen = next->lastGrant = Stamp(now);
            Packet grant = next->offer;
            grant.ready = GroupReady(*next, now);
            // A client may commit this grant after our room sample changes.
            if (grant.ready) {
                next->readyAt = Stamp(now);
                next->readyGeneration = loadsync::SceneGeneration();
            }
            Send(grant, Grant);
        }
        Cast(*next, now);
    }
}
bool OnPacket(const Packet& p, unsigned from, DWORD now) {
    if (p.magic != kMagic || p.version != 1 || p.kind < Hello || p.kind > Vote || !p.sender ||
        from >= coop_peers::kMaxPeers || !KnownPeerToken(p.sender)) return false;
    if (IsHost() && peerTable.peers[from].token != p.sender) return false;
    const unsigned slot = Slot(p.sender);
    if (slot >= coop_peers::kMaxPeers) return false;
    if (p.kind == Hello) {
        auto& cap = capabilities[slot];
        if (cap.token != p.sender || static_cast<LONG>(p.serial - cap.serial) > 0) cap = Capability{p.sender, p.map, p.generation, p.serial, Stamp(now), p.ready == 1, p.hash};
        if (IsHost()) RelayToOthers(&p, sizeof(p), from);
        return true;
    }
    if (!p.attempt || streams::GuidZero(p.set) || (!KnownPeerToken(p.owner) && p.owner != peerReadiness.token) || !Shared(p.hash) || !World(p)) return false;
    if (p.kind == Offer) {
        if (!IsHost() || p.sender != p.owner || p.generation != p.ownerGeneration || p.ready > 2) return false;
        Propose(p, now);
    } else if (p.kind == Grant) {
        if (IsHost() || peerTable.peers[from].machine != 0 || p.sender != peerTable.peers[from].token || p.hostGeneration != p.generation || Done(p, now)) return false;
        auto* r = Find(p.hash, true);
        if (!r) return false;
        if (grantSender == p.sender && static_cast<LONG>(p.serial - grantSerial) <= 0) return true;
        grantSender = p.sender;
        grantSerial = p.serial;
        if (!Same(r->offer, p)) *r = Reservation{};
        r->offer = p;
        r->granted = true;
        r->grantSeen = Stamp(now);
        Cast(*r, now);
    } else {
        if (!IsHost()) return false;
        auto* r = Find(p.hash);
        if (!r || !r->granted || !Same(r->offer, p) || p.hostGeneration != r->offer.hostGeneration ||
            p.ready > 1) return false;
        auto& v = r->votes[slot];
        if (v.token != p.sender || static_cast<LONG>(p.serial - v.serial) > 0)
            v = VoteState{p.sender,p.generation,p.serial,Stamp(now),p.ready != 0};
    }
    return true;
}
}  // namespace agreement
