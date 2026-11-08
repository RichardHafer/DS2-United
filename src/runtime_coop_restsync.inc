// Dead Space 2 United by Hafer.
// Where loose objects and corpses come to rest ([Physics] RestSync).
//
// Every machine simulates its own physics; only kinesis-held bodies were
// streamed. 25.09., ch01 psych hall: 28 props lay 1-25 m apart between host
// and client, kicked or thrown on one machine only. Now, once a second, each
// machine walks the SimManager's GUID table (buckets at +0x70, 4096 of them,
// node chain +0x34, the object at +0x40; the table FindBase 0x00776310 reads)
// for props (RestClasses) and corpses (every Sentient whose Havok bodies are
// all dynamic) within RestRadius of its own Isaac. A body set that has come to
// rest somewhere else than where it rested before is sent: 20 bytes a body,
// position and a packed rotation. The partner puts its copy there (one hard
// keyframe step, then still, the kinesis receiver's way) and takes that as
// its own resting pose, so nothing echoes back.
//
// Who wins: a prop both machines moved (a ghost's capsule pushes on both) goes
// where the host has it. A corpse's first resting pose is the host's (it owns
// the NPC); after that whoever moves it sends it.
namespace restsync {
bool enabled = false, installed = false, corpses = false;
constexpr uint32_t kSimManagerRva = 0x1C1C0A0, kBuckets = 0x70, kNodeNext = 0x34, kNodeObject = 0x40;
constexpr uint32_t kMagic = 0x54535352;   // 'RSST'
constexpr uint32_t kVersion = 1;
constexpr unsigned kPayload = 220, kMaxBodies = 24, kBodiesPerChunk = 10;
struct Packet {
    uint32_t magic, version, token, sequence;
    uint16_t used, chunks;
    unsigned char payload[kPayload];
};
static_assert(sizeof(Packet) == 240, "Unexpected rest sync wire layout");
struct ChunkHead { uint32_t guid[4]; uint8_t bodyCount, first, count, flags; };
struct WireBody { float pos[3]; int16_t rot[4]; };
static_assert(sizeof(ChunkHead) == 20 && sizeof(WireBody) == 20, "Unexpected rest sync chunk layout");
bool Valid(const Packet& p) { return p.magic == kMagic && p.version == kVersion && p.token && p.used <= kPayload && p.chunks; }

enum Kind : uint8_t { kNone, kProp, kCorpse };
struct Pose { float pos[3], rot[4]; };
struct Tracked {
    uint32_t vtable = 0, guid[4]{};
    uint8_t kind = kNone, bodies = 0, resends = 0;
    bool known = false, queued = false;
    Pose rest[kMaxBodies]{};
    DWORD seenAt = 0, movedAt = 0, appliedAt = 0;
};
std::unordered_map<uint32_t, Tracked> tracked;
volatile LONG trackedCount = 0;   // tracked.size() for the diagnostics thread, which must not touch the map
std::unordered_map<uint32_t, uint8_t> kinds;   // by vtable
std::vector<uint32_t> outgoing;
gate::RuleSet propRules;
uint32_t sentientHash = 0, sequence = 0;
float radius = 25.0f;
DWORD intervalMs = 1000, lastScan = 0;
unsigned maxPackets = 8;
struct Settle { uint32_t entity, body; DWORD at; };
Settle settles[96]{};
unsigned settleCount = 0;
volatile LONG scans = 0, candidates = 0, moved = 0, firstRests = 0, packetsSent = 0, bodiesSent = 0, deferred = 0, received = 0,
              applied = 0, appliedCorpses = 0, notFound = 0, countMismatch = 0, refused = 0, notDynamic = 0, heldHere = 0,
              hostKept = 0, stale = 0, faults = 0, events = 0, aliveHere = 0;
struct PeerSeq { uint32_t token, sequence; };
PeerSeq peers[8]{};

float Dist2(const float* a, const float* b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}
bool Differs(const Pose& a, const Pose& b) {
    if (Dist2(a.pos, b.pos) > 0.15f * 0.15f) return true;
    const float dot = a.rot[0] * b.rot[0] + a.rot[1] * b.rot[1] + a.rot[2] * b.rot[2] + a.rot[3] * b.rot[3];
    return (dot < 0 ? -dot : dot) < 0.996f;   // about 10 degrees
}
uint8_t KindOf(uint32_t object, uint32_t vtable) {
    if (phantom::Is(object)) { InterlockedIncrement(&phantom::restSkipped); return kNone; }   // a hallucination has no corpse to share
    auto it = kinds.find(vtable);
    if (it != kinds.end()) return it->second;
    uint8_t k = kNone;
    char cls[32] = "";
    if (vtable == base + placement::kPlayerPrimaryVtableRva || vtable == base + kNecroPrimaryVtableRva) k = kNone;
    else if (mirror::ClassOf(object, vtable, propRules, cls, sizeof(cls))) k = kProp;
    else if (corpses && gate::IsA(object, vtable, sentientHash)) k = kCorpse;
    kinds[vtable] = k;
    return k;
}
// A Sentient counts as a corpse only with its health component at zero: a
// living NPC's ragdoll bodies can be dynamic too.
bool Dead(uint32_t entity) {
    const uint32_t component = hits::FindHealthComponent(entity);
    float health = 1.0f;
    return component && Read(component + hits::kHealthCurrent, &health, 4) && !(health > 0.0f);
}
// The bodies of an entity, all dynamic, and their poses; false when any is keyframed, fixed or unreadable.
bool Bodies(uint32_t object, uint32_t* list, unsigned& n, bool& resting, Pose* poses) {
    const uint32_t comp = tk::HavokComponent(object);
    uint32_t array = 0, count = 0;
    if (!comp || !Read(comp + tk::kCompBodies, &array, 4) || !Read(comp + tk::kCompBodyCount, &count, 4) || !array || !count || count > kMaxBodies)
        return false;
    n = count;
    resting = true;
    for (unsigned i = 0; i < n; ++i) {
        uint32_t body = 0;
        unsigned char motion = 0;
        float linear[3]{}, angular[3]{};
        if (!Read(array + i * 4, &body, 4) || !body || !Read(body + tk::kBodyMotionType, &motion, 1)) return false;
        if (motion == tk::kMotionKeyframed || motion == tk::kMotionFixed) return false;
        list[i] = body;
        if (!Read(body + tk::kBodyPos, poses[i].pos, 12) || !Read(body + tk::kBodyRot, poses[i].rot, 16)) return false;
        Read(body + tk::kBodyLinear, linear, 12);
        Read(body + tk::kBodyAngular, angular, 12);
        if (tk::Length2(linear) > 0.01f || tk::Length2(angular) > 0.04f) resting = false;
    }
    return true;
}

void Consider(uint32_t object, const float* me, DWORD now) {
    uint32_t vtable = 0;
    float at[3]{};
    if (!Read(object, &vtable, 4) || vtable < base + gate::kRdataLo || vtable >= base + gate::kRdataHi) return;
    if (!Read(object + 0xC0, at, 12) || !(Dist2(at, me) < radius * radius)) return;
    const uint8_t kind = KindOf(object, vtable);
    if (kind == kNone || (kind == kCorpse && !Dead(object))) return;
    uint32_t list[kMaxBodies]{};
    Pose now_[kMaxBodies]{};
    unsigned n = 0;
    bool resting = false;
    if (!Bodies(object, list, n, resting, now_)) return;
    InterlockedIncrement(&candidates);
    Tracked& t = tracked[object];
    uint32_t guid[4]{};
    Read(object + 0x24, guid, 16);
    if (t.vtable != vtable || std::memcmp(t.guid, guid, 16) != 0 || t.bodies != n) {
        t = Tracked{};
        t.vtable = vtable; t.kind = kind; t.bodies = static_cast<uint8_t>(n);
        std::memcpy(t.guid, guid, 16);
    }
    t.seenAt = now;
    if (!resting) { t.movedAt = now; return; }
    if (!t.known) {
        // Level props start where the level put them, on every machine. A
        // corpse is news: the host, who owns the NPC, tells where it lies.
        t.known = true;
        std::memcpy(t.rest, now_, n * sizeof(Pose));
        if (kind == kCorpse && IsHost()) { InterlockedIncrement(&firstRests); t.resends = 2; if (!t.queued) { t.queued = true; outgoing.push_back(object); } }
        return;
    }
    bool changed = false;
    for (unsigned i = 0; i < n && !changed; ++i) changed = Differs(t.rest[i], now_[i]);
    std::memcpy(t.rest, now_, n * sizeof(Pose));
    if (!changed || now - t.appliedAt < 1500) return;   // settling where a partner put it: nothing new
    InterlockedIncrement(&moved);
    t.resends = 2;
    if (!t.queued) { t.queued = true; outgoing.push_back(object); }
}

bool Flush() {
    unsigned sent = 0;
    size_t next = 0;
    Packet p{};
    auto send = [&]() {
        if (!p.chunks) return;
        p.magic = kMagic; p.version = kVersion; p.token = peerReadiness.token; p.sequence = ++sequence;
        if (SendToPeers(&p, sizeof(p))) InterlockedIncrement(&packetsSent);
        ++sent;
        p = Packet{};
    };
    std::vector<uint32_t> keep;
    for (; next < outgoing.size(); ++next) {
        const uint32_t object = outgoing[next];
        auto it = tracked.find(object);
        if (it == tracked.end()) continue;
        Tracked& t = it->second;
        if (sent >= maxPackets) { keep.push_back(object); InterlockedIncrement(&deferred); continue; }
        for (unsigned first = 0; first < t.bodies; first += kBodiesPerChunk) {
            const unsigned count = t.bodies - first < kBodiesPerChunk ? t.bodies - first : kBodiesPerChunk;
            const unsigned bytes = sizeof(ChunkHead) + count * sizeof(WireBody);
            if (p.used + bytes > kPayload) send();
            ChunkHead head{{t.guid[0], t.guid[1], t.guid[2], t.guid[3]}, t.bodies, static_cast<uint8_t>(first), static_cast<uint8_t>(count),
                           static_cast<uint8_t>(t.kind)};
            std::memcpy(p.payload + p.used, &head, sizeof(head));
            p.used = static_cast<uint16_t>(p.used + sizeof(head));
            for (unsigned i = 0; i < count; ++i) {
                const Pose& pose = t.rest[first + i];
                WireBody b{{pose.pos[0], pose.pos[1], pose.pos[2]}, {}};
                for (unsigned c = 0; c < 4; ++c) b.rot[c] = static_cast<int16_t>(pose.rot[c] * 32767.0f);
                std::memcpy(p.payload + p.used, &b, sizeof(b));
                p.used = static_cast<uint16_t>(p.used + sizeof(b));
            }
            ++p.chunks;
            InterlockedExchangeAdd(&bodiesSent, static_cast<LONG>(count));
        }
        // Once more in the next seconds: a lost packet leaves nothing apart for long.
        if (t.resends && --t.resends) keep.push_back(object);
        else t.queued = false;
    }
    send();
    outgoing.swap(keep);
    return sent != 0;
}

void Scan(DWORD now) {
    const uint32_t local = LocalSlotZeroEntity();
    float me[3]{};
    uint32_t sim = 0;
    if (!local || !Read(local + 0xC0, me, 12) || !Read(base + kSimManagerRva, &sim, 4) || !sim) return;
    InterlockedIncrement(&scans);
    for (uint32_t b = 0; b < 4096; ++b) {
        uint32_t node = 0;
        if (!Read(sim + kBuckets + b * 4, &node, 4)) return;
        for (unsigned guard = 0; node && guard < 64; ++guard) {
            uint32_t object = 0;
            if (Read(node + kNodeObject, &object, 4) && object) Consider(object, me, now);
            if (!Read(node + kNodeNext, &node, 4)) break;
        }
    }
    for (auto it = tracked.begin(); it != tracked.end();)
        it = now - it->second.seenAt > 10000 && !it->second.queued ? tracked.erase(it) : std::next(it);
    InterlockedExchange(&trackedCount, static_cast<LONG>(tracked.size()));
}

void Tick(DWORD now) {
    if (!installed || !inLevel) {
        if (!inLevel && !tracked.empty()) { tracked.clear(); outgoing.clear(); settleCount = 0; InterlockedExchange(&trackedCount, 0); }
        return;
    }
    for (unsigned i = 0; i < settleCount;) {
        if (now - settles[i].at < 40) { ++i; continue; }
        const Settle s = settles[i];
        settles[i] = settles[--settleCount];
        uint32_t count = 0;
        if (tk::BodyIndex(s.entity, s.body, &count) != tk::kNoIndex) tk::StopBody(s.body);
    }
    if (!peerTable.Count() || now - lastScan < intervalMs) return;
    lastScan = now;
    __try {
        Scan(now);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
    Flush();
}

// Game thread, from the receive loop.
void OnPacket(const Packet& p) {
    if (!installed || !inLevel) return;
    InterlockedIncrement(&received);
    PeerSeq* peer = nullptr;
    for (auto& s : peers) if (s.token == p.token) peer = &s;
    if (!peer) { peer = &peers[p.token % std::size(peers)]; *peer = PeerSeq{p.token, 0}; }
    if (peer->sequence && static_cast<int32_t>(p.sequence - peer->sequence) <= 0) { InterlockedIncrement(&stale); return; }
    peer->sequence = p.sequence;
    const DWORD now = GetTickCount();
    const float step = tk::SimStep();
    const uint32_t held = tk::LocalHeldBody();
    unsigned at = 0;
    for (unsigned c = 0; c < p.chunks && at + sizeof(ChunkHead) <= p.used; ++c) {
        ChunkHead head{};
        std::memcpy(&head, p.payload + at, sizeof(head));
        at += sizeof(head);
        const unsigned bytes = head.count * sizeof(WireBody);
        if (at + bytes > p.used || head.count > kBodiesPerChunk || head.first + head.count > head.bodyCount) return;
        const unsigned char* bodies = p.payload + at;
        at += bytes;
        const uint32_t object = mirror::ObjectByGuid(head.guid);
        if (!object) { InterlockedIncrement(&notFound); continue; }
        uint32_t vtable = 0;
        if (!Read(object, &vtable, 4) || KindOf(object, vtable) != head.flags) { InterlockedIncrement(&refused); continue; }
        if (head.flags == kCorpse && !Dead(object)) { InterlockedIncrement(&aliveHere); continue; }   // never moves a living NPC
        uint32_t list[kMaxBodies]{};
        Pose poses[kMaxBodies]{};
        unsigned n = 0;
        bool resting = false;
        if (!Bodies(object, list, n, resting, poses)) { InterlockedIncrement(&notDynamic); continue; }
        if (n != head.bodyCount) { InterlockedIncrement(&countMismatch); continue; }
        Tracked& t = tracked[object];
        if (IsHost() && t.known && now - t.movedAt < 3000) { InterlockedIncrement(&hostKept); continue; }   // moved here too: the local pose stands
        bool touched = false;
        for (unsigned i = 0; i < head.count; ++i) {
            const unsigned index = head.first + i;
            if (list[index] == held) { InterlockedIncrement(&heldHere); touched = false; break; }
            WireBody w{};
            std::memcpy(&w, bodies + i * sizeof(WireBody), sizeof(w));
            float rot[4]{};
            float len = 0.0f;
            for (unsigned k = 0; k < 4; ++k) { rot[k] = w.rot[k] / 32767.0f; len += rot[k] * rot[k]; }
            if (len < 0.5f) continue;
            len = std::sqrt(len);
            for (float& r : rot) r /= len;
            if (!tk::HardKeyFrame(list[index], w.pos, rot, 1.0f / step)) continue;
            touched = true;
            if (settleCount < std::size(settles)) settles[settleCount++] = Settle{object, list[index], now};
        }
        if (!touched) continue;
        if (t.vtable != vtable || t.bodies != n) {
            t = Tracked{};
            t.vtable = vtable; t.kind = head.flags; t.bodies = static_cast<uint8_t>(n);
            std::memcpy(t.guid, head.guid, 16);
        }
        t.known = true;
        t.appliedAt = now;
        t.seenAt = now;
        InterlockedIncrement(head.flags == kCorpse ? &appliedCorpses : &applied);
        if (InterlockedIncrement(&events) <= 40)
            gate::Event("coop_restsync partner's %s %08X-%08X (%u bodies from %u) put where it rests there%s", head.flags == kCorpse ? "corpse" : "prop",
                        head.guid[0], head.guid[1], static_cast<unsigned>(head.count), static_cast<unsigned>(head.first),
                        guidfix::IsGhostEntity(object) ? " (a ghost here)" : "");
    }
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Physics", "RestSync", 0, ini.c_str()) != 0;
    corpses = GetPrivateProfileIntA("Physics", "RestSyncCorpses", 1, ini.c_str()) != 0;
    radius = static_cast<float>(GetPrivateProfileIntA("Physics", "RestRadius", 25, ini.c_str()));
    intervalMs = static_cast<DWORD>(GetPrivateProfileIntA("Physics", "RestSyncMs", 1000, ini.c_str()));
    if (intervalMs < 250) intervalMs = 250;
    maxPackets = static_cast<unsigned>(GetPrivateProfileIntA("Physics", "RestMaxPackets", 8, ini.c_str()));
    if (!maxPackets) maxPackets = 1;
    gate::ReadRuleSet(log, ini, propRules, "Physics", "RestClasses", "ScriptableProp,DamageableProp,DismemberableProp,BreakableContainer", "RestNodes", "");
    sentientHash = gate::NameHash("Sentient");
    installed = enabled && tk::installed;
    tracked.reserve(1024);
    outgoing.reserve(256);
    std::fprintf(log, "coop_restsync enabled=%d installed=%d corpses=%d radius=%.0f every=%lu ms max_packets=%u (tk=%d)\n", enabled ? 1 : 0,
                 installed ? 1 : 0, corpses ? 1 : 0, radius, intervalMs, maxPackets, tk::installed ? 1 : 0);
    return installed;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG n = moved + firstRests + received + faults;
    if (n == last) return;
    last = n;
    std::fprintf(log, "coop_restsync scans=%ld candidates=%ld tracked=%u moved=%ld first_rests=%ld packets_sent=%ld bodies_sent=%ld deferred=%ld | received=%ld applied=%ld corpses=%ld not_found=%ld count_mismatch=%ld refused=%ld not_dynamic=%ld held_here=%ld host_kept=%ld alive_here=%ld stale=%ld faults=%ld\n",
                 scans, candidates, static_cast<unsigned>(InterlockedCompareExchange(&trackedCount, 0, 0)), moved, firstRests, packetsSent, bodiesSent, deferred, received, applied,
                 appliedCorpses, notFound, countMismatch, refused, notDynamic, heldHere, hostKept, aliveHere, stale, faults);
}
}  // namespace restsync
