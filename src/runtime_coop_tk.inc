// Dead Space 2 United by Hafer.
// Kinesis for partners.
//
// The player's kinesis is a state machine on the local Player only,
// PlayerFireTKSM (ctor 0x0088A120, vtable 0x01B27FB8). A ghost Player has
// none, so what the partner holds was invisible here beyond the arm animation
// (DS2_COOP_PHYSICS_2026-09-17.md). The machine's three notifications:
//     DropNotify    0x0088A2B0   thiscall()     -> TKComponent::Drop   0x00FE1CF0
//     GrabNotify    0x0088A500   thiscall()     -> TKComponent::Grab   0x00FE1B80
//     LaunchNotify  0x0088A390   thiscall()     -> TKComponent::Launch 0x00FE1C40
// (Grab and Launch were swapped here until 28.09.; the HSM calls 0x0088A500 on
// MESSAGE_GRAB and 0x0088A390 on MESSAGE_LAUNCH, DS2_BOT_KINESIS_2026-09-28.md.)
// The target entity is [sm+0x104] - 0x10 (a SafePtr to the entity's +0x10),
// its TKComponent [sm+0x10C] - 0x1C.
//
// Stage one sends each notification to the partner, who logs whether he has
// that object. The first run: every grabbed thing (a DismemberableProp, two
// enemy corpses) existed on both machines under the same GUID.
namespace tk {
bool enabled = false, installed = false;
constexpr uint32_t kGrabNotifyRva = 0x48A500, kDropNotifyRva = 0x48A2B0, kLaunchNotifyRva = 0x48A390;
constexpr uint32_t kSmTarget = 0x104, kSmTargetComponent = 0x10C;
constexpr uint32_t kMagic = 0x56454B54;   // 'TKEV'
constexpr uint32_t kVersion = 1;
enum : uint32_t { kEventGrab = 1, kEventDrop = 2, kEventLaunch = 3 };

struct Packet {
    uint32_t magic, version, token, sequence;
    uint32_t event;
    uint32_t guid[4];
    float pos[3];
    uint32_t vtable;
    uint32_t reserved[3];
};
static_assert(sizeof(Packet) == 64, "Unexpected kinesis wire layout");
bool Valid(const Packet& p) { return p.magic == kMagic && p.version == kVersion && p.token && p.event >= 1 && p.event <= 3; }

// Stage three (24.09.). Stage two wrote the entity's matrix rows onto the
// partner's copy: a physics prop ignores that, Havok puts it back on the next
// step, and the stream hardly ever started because it waited for GrabNotify
// (grabs=1 against drops=11 and launches=7 in one run). Now:
//   Holder: the machine is polled. Its held body is [sm+0xC0] (hkpRigidBody*,
//   released in DetachFromTarget 0x0088B570), the owner [sm+0x50]. While a body
//   is held, and after it is let go until it rests (a throw or a drop), its
//   state goes out thirty times a second: position (body+0x120, the transform's
//   translation), rotation (+0x160, sweptTransform.rotation1), linear and
//   angular velocity (+0x1B0 / +0x1C0, the motion at +0xE0 plus 0xD0 / 0xE0).
//   Receiver: its copy is the same body of the same entity. HavokComponent
//   (component type byte 0x020A4662, lookup 0x007CF080 thiscall(entity, type))
//   keeps the bodies at [comp+0x18], count [comp+0x1C], and the holder sends
//   the index. The copy is steered the way DynamicObjectBehavior::SetMatrix
//   (0x0138DF80) moves keyframed bodies: hkpKeyFrameUtility::applyHardKeyFrame
//   (0x014A27E0, cdecl(pos*, quat*, invDt, body*), activates the body) gives
//   it the velocities that reach the holder's pose one packet ahead, so it
//   flies, collides and lands under its own physics in between. At rest it
//   is put on the exact spot and its velocities are cleared (motion vtable
//   +0x40 setLinearVelocity, +0x44 setAngularVelocity, after activate 0x00484660).
constexpr uint32_t kPoseMagic = 0x4F504B54;   // 'TKPO'
constexpr uint32_t kPoseVersion = 2;
enum : uint32_t { kPhaseHeld = 1, kPhaseFlight = 2, kPhaseRest = 3 };
struct PosePacket {
    uint32_t magic, version, token, sequence;
    uint32_t guid[4];
    float pos[4], rot[4], linear[4], angular[4];
    uint32_t phase, bodyIndex, bodyCount, reserved;
};
static_assert(sizeof(PosePacket) == 112, "Unexpected kinesis pose wire layout");
bool ValidPose(const PosePacket& p) {
    if (p.magic != kPoseMagic || p.version != kPoseVersion || !p.token || !(p.guid[0] | p.guid[1] | p.guid[2] | p.guid[3]) ||
        p.phase < kPhaseHeld || p.phase > kPhaseRest || p.bodyCount > 64 ||
        (p.bodyIndex != UINT32_MAX && p.bodyIndex >= p.bodyCount)) return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(p.pos[i]) || !std::isfinite(p.linear[i]) || std::fabs(p.pos[i]) > 1.0e7f || std::fabs(p.linear[i]) > 1.0e6f) return false;
    float norm = 0.0f;
    for (float v : p.rot) norm += v * v;
    return norm > 0.98f && norm < 1.02f;
}

constexpr uint32_t kSmVtableRva = 0x1727FB8, kSmCtorRva = 0x48A120, kSmOwner = 0x50, kSmBody = 0xC0;
constexpr uint32_t kComponentLookupRva = 0x3CF080, kHavokComponentType = 0x1CA4662;
constexpr uint32_t kHardKeyFrameRva = 0x10A27E0, kActivateRva = 0x84660, kSimFrameTime = 0x1C4D3E4;
constexpr uint32_t kCompBodies = 0x18, kCompBodyCount = 0x1C;
constexpr uint32_t kBodyMotion = 0xE0, kBodyMotionType = 0xE8, kBodyPos = 0x120, kBodyRot = 0x160, kBodyLinear = 0x1B0, kBodyAngular = 0x1C0;
constexpr unsigned char kMotionKeyframed = 6, kMotionFixed = 7;
constexpr uint32_t kNoIndex = 0xFFFFFFFF, kMaxBodies = 64;
constexpr DWORD kPoseIntervalMs = 33, kFlightMaxMs = 8000;
constexpr float kMaxWireLead = 0.5f;
constexpr float kLeadSeconds = 0.033f, kRestSpeed2 = 0.04f, kRestSpin2 = 0.25f;
constexpr unsigned kRestSamples = 4, kRestRepeats = 3;
bool streamPose = false;

safetyhook::InlineHook grabHook, dropHook, launchHook, ctorHook;
uint32_t sequence = 0, poseSequence = 0;
volatile LONG grabs = 0, drops = 0, launches = 0, sent = 0, received = 0, partnerFound = 0, partnerMissing = 0, noTarget = 0;
volatile LONG smSeen = 0, ownerMismatch = 0, holds = 0, flights = 0, flightTimeouts = 0, flightLost = 0, restsSent = 0, noIndex = 0;
volatile LONG posesSent = 0, posesReceived = 0, posesApplied = 0, posesGhost = 0, posesNotFound = 0, posesNoBody = 0, posesFixed = 0,
              posesStale = 0, posesOwnHold = 0, settles = 0, engineFaults = 0, eventLines = 0;
uint32_t lastSm = 0;   // the local machine, from its ctor or a notification

int Fault(unsigned) { InterlockedIncrement(&engineFaults); return EXCEPTION_EXECUTE_HANDLER; }

// The streamed body: held now, or let go and not yet at rest.
struct Track {
    uint32_t entity = 0, body = 0, guid[4]{}, index = kNoIndex, count = 0;
    uint32_t phase = 0;
    DWORD releasedAt = 0, lastSend = 0;
    unsigned quiet = 0, restLeft = 0;
    bool launched = false;
    float startPos[3]{};
};
Track track{};
DWORD launchedAt = 0;

// The body was replaced (necromorph hand-over, restart): its kinesis state
// machine and anything it held are gone with it.
void OnBodyReplaced() { lastSm = 0; track = Track{}; }

const char* EventName(uint32_t e) { return e == kEventGrab ? "grab" : e == kEventDrop ? "drop" : e == kEventLaunch ? "launch" : "?"; }
const char* PhaseName(uint32_t p) { return p == kPhaseHeld ? "held" : p == kPhaseFlight ? "flight" : p == kPhaseRest ? "rest" : "?"; }

uint32_t HavokComponent(uint32_t entity) {
    unsigned char type = 0;
    if (!entity || !Read(base + kHavokComponentType, &type, 1)) return 0;
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t)>(base + kComponentLookupRva)(reinterpret_cast<void*>(entity), type);
    } __except (Fault(GetExceptionCode())) {
        return 0;
    }
}
// Index of body among the entity's Havok bodies, and how many it has.
uint32_t BodyIndex(uint32_t entity, uint32_t body, uint32_t* count) {
    const uint32_t comp = HavokComponent(entity);
    uint32_t bodies = 0, n = 0;
    *count = 0;
    if (!comp || !Read(comp + kCompBodies, &bodies, 4) || !Read(comp + kCompBodyCount, &n, 4) || !bodies || n > kMaxBodies) return kNoIndex;
    *count = n;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t b = 0;
        if (Read(bodies + i * 4, &b, 4) && b == body) return i;
    }
    return kNoIndex;
}
uint32_t BodyAt(uint32_t entity, uint32_t index, uint32_t* count) {
    const uint32_t comp = HavokComponent(entity);
    uint32_t bodies = 0, n = 0, body = 0;
    *count = 0;
    if (!comp || !Read(comp + kCompBodies, &bodies, 4) || !Read(comp + kCompBodyCount, &n, 4) || !bodies || n > kMaxBodies) return 0;
    *count = n;
    if (index == kNoIndex && n == 1) index = 0;
    if (index >= n || !Read(bodies + index * 4, &body, 4)) return 0;
    return body;
}
uint32_t LocalHeldBody() {
    uint32_t vtable = 0, body = 0, owner = 0;
    if (!lastSm || !Read(lastSm, &vtable, 4) || vtable != base + kSmVtableRva ||
        !Read(lastSm + kSmOwner, &owner, 4) || !owner || owner != LocalSlotZeroEntity() ||
        !Read(lastSm + kSmBody, &body, 4)) return 0;
    return body;
}

// A companion bot's machine (runtime_coop_bot.inc, kinesis): its own, never "the
// local one", so nothing it holds is streamed. Its events are counted and logged.
volatile LONG botGrabs = 0, botDrops = 0, botLaunches = 0, botMachines = 0;
bool BotMachine(uint32_t sm) {
    uint32_t owner = 0;
    return Read(sm + kSmOwner, &owner, 4) && IsBotEntity(owner);
}
void NoteBotEvent(uint32_t sm, uint32_t event) {
    InterlockedIncrement(event == kEventGrab ? &botGrabs : event == kEventDrop ? &botDrops : &botLaunches);
    uint32_t owner = 0, handle = 0, vtable = 0;
    Read(sm + kSmOwner, &owner, 4);
    const uint32_t entity = Read(sm + kSmTarget, &handle, 4) && handle > 0x10010 ? handle - 0x10 : 0;
    if (entity) Read(entity, &vtable, 4);
    if (InterlockedIncrement(&eventLines) <= 200)
        gate::Event("coop_tk bot %08X: %s of %08X (vtable RVA %08X)", owner, EventName(event), entity, vtable ? vtable - static_cast<uint32_t>(base) : 0u);
}

void NoteMachine(uint32_t sm) {
    if (sm == lastSm) return;
    lastSm = sm;
    InterlockedIncrement(&smSeen);
    uint32_t owner = 0;
    Read(sm + kSmOwner, &owner, 4);
    const uint32_t local = LocalSlotZeroEntity();
    if (local && owner != local) InterlockedIncrement(&ownerMismatch);
    gate::Event("coop_tk kinesis machine %08X, owner %08X, local player %08X%s", sm, owner, local,
                local && owner != local ? " (not ours? streaming anyway, ghosts have no machine)" : "");
}

// Compare the cell's native attachment and the socket's occupant on both machines.
void TraceCell(const char* side, uint32_t event, uint32_t entity, uint32_t vtable, const uint32_t* guid, DWORD age = 0) {
    if (!gate::trace || vtable != base + 0x1743A98) return;
    static LONG lines = 0;
    if (InterlockedIncrement(&lines) > 160) return;
    uint32_t state = UINT32_MAX, parentRef = 0, parentVt = 0, socketState = UINT32_MAX, occupant = 0, parentGuid[4]{};
    Read(entity + 0x2F0, &state, 4);
    if (Read(entity + 0x6C, &parentRef, 4) && parentRef > 0x10) {
        const uint32_t parent = parentRef - 0x10;
        if (Read(parent, &parentVt, 4) && parentVt == base + 0x1743798) {
            Read(parent + 0x24, parentGuid, sizeof(parentGuid));
            Read(parent + 0x244, &socketState, 4);
            Read(parent + 0x238, &occupant, 4);
        }
    }
    gate::Event("coop_tk cell %s %s guid=%08X-%08X-%08X-%08X state=%u parent_vt=%08X socket=%08X-%08X-%08X-%08X socket_state=%u occupant_matches=%d after_ms=%lu",
                side, EventName(event), guid[0], guid[1], guid[2], guid[3], state, parentVt,
                parentGuid[0], parentGuid[1], parentGuid[2], parentGuid[3], socketState, occupant == entity + 0x10, age);
}

struct CellWatch {
    uint32_t guid[4], event, map, generation;
    DWORD since;
    bool remote, sampled;
};
CellWatch cellWatches[8]{};
unsigned cellWatchNext = 0;
void WatchCell(bool remote, uint32_t event, uint32_t vtable, const uint32_t* guid) {
    if (!gate::trace || vtable != base + 0x1743A98 || !levelSettled) return;
    auto& w = cellWatches[cellWatchNext++ % std::size(cellWatches)];
    std::memcpy(w.guid, guid, sizeof(w.guid));
    w.event = event;
    w.map = loadsync::SceneMapHash();
    w.generation = loadsync::SceneGeneration();
    w.since = GetTickCount();
    w.remote = remote;
    w.sampled = false;
}
void SampleCells(DWORD now) {
    if (!gate::trace || !cellWatchNext) return;
    for (auto& w : cellWatches) {
        if (!w.event) continue;
        const DWORD age = now - w.since;
        if (age < (w.sampled ? 1200u : 250u)) continue;
        if (age <= 3000 && levelSettled && w.map && w.generation &&
            w.map == loadsync::SceneMapHash() && w.generation == loadsync::SceneGeneration()) {
            const uint32_t cell = mirror::ObjectByGuid(w.guid);
            uint32_t vt = 0;
            if (cell && Read(cell, &vt, 4)) TraceCell(w.remote ? "partner" : "local", w.event, cell, vt, w.guid, age);
        }
        if (age >= 1200) w.event = 0;
        else w.sampled = true;
    }
}

void Report(uint32_t sm, uint32_t event) {
    uint32_t handle = 0, entity = 0, vtable = 0, guid[4]{};
    float pos[3]{};
    NoteMachine(sm);
    if (event == kEventLaunch) launchedAt = GetTickCount();
    if (!Read(sm + kSmTarget, &handle, 4) || !handle) { InterlockedIncrement(&noTarget); return; }
    entity = handle - 0x10;
    if (!Read(entity, &vtable, 4)) { InterlockedIncrement(&noTarget); return; }
    Read(entity + 0x24, guid, 16);
    TraceCell("local", event, entity, vtable, guid);
    WatchCell(false, event, vtable, guid);
    Read(entity + placement::kPlayerPosition, pos, 12);
    if (gate::trace)
        gate::Event("coop_tk %s entity=%08X vtable=%08X guid=%08X-%08X-%08X-%08X pos=(%.1f %.1f %.1f)", EventName(event), entity, vtable,
                    guid[0], guid[1], guid[2], guid[3], pos[0], pos[1], pos[2]);
    if (!peerTable.Count()) return;
    Packet p{kMagic, kVersion, peerReadiness.token, ++sequence, event, {guid[0], guid[1], guid[2], guid[3]}, {pos[0], pos[1], pos[2]}, vtable, {}};
    if (SendToPeers(&p, sizeof(p))) InterlockedIncrement(&sent);
}

void Notify(void* sm, uint32_t event, volatile LONG* counter) {
    const uint32_t machine = reinterpret_cast<uint32_t>(sm);
    if (BotMachine(machine)) { NoteBotEvent(machine, event); return; }
    InterlockedIncrement(counter);
    if (enabled) Report(machine, event);
}
void __fastcall OnGrab(void* sm, void*) { Notify(sm, kEventGrab, &grabs); grabHook.thiscall<void>(sm); }
void __fastcall OnDrop(void* sm, void*) { Notify(sm, kEventDrop, &drops); dropHook.thiscall<void>(sm); }
void __fastcall OnLaunch(void* sm, void*) { Notify(sm, kEventLaunch, &launches); launchHook.thiscall<void>(sm); }
void* __fastcall OnCtor(void* sm, void*, uint32_t a, uint32_t b) {
    void* result = ctorHook.thiscall<void*>(sm, a, b);
    const uint32_t machine = reinterpret_cast<uint32_t>(sm);
    if (BotMachine(machine)) InterlockedIncrement(&botMachines);
    else if (enabled) NoteMachine(machine);
    return result;
}

// The partner did something with kinesis: does that object exist here at all?
void OnPacket(const Packet& p) {
    if (!installed || !enabled) return;
    InterlockedIncrement(&received);
    const uint32_t object = (p.guid[0] | p.guid[1] | p.guid[2] | p.guid[3]) ? mirror::ObjectByGuid(p.guid) : 0;
    uint32_t vtable = 0;
    if (object) Read(object, &vtable, 4);
    TraceCell("partner", p.event, object, vtable, p.guid);
    WatchCell(true, p.event, vtable, p.guid);
    InterlockedIncrement(object ? &partnerFound : &partnerMissing);
    if (gate::trace)
        gate::Event("coop_tk partner %s guid=%08X-%08X-%08X-%08X at (%.1f %.1f %.1f) - here: %s%08X (vtable %08X, partner's %08X)",
                    EventName(p.event), p.guid[0], p.guid[1], p.guid[2], p.guid[3], p.pos[0], p.pos[1], p.pos[2],
                    object ? "object " : "NOT FOUND ", object, vtable, p.vtable);
}

bool SendPose(uint32_t phase) {
    PosePacket p{kPoseMagic, kPoseVersion, peerReadiness.token, ++poseSequence, {}, {}, {}, {}, {}, phase, track.index, track.count, 0};
    std::memcpy(p.guid, track.guid, 16);
    if (!Read(track.body + kBodyPos, p.pos, 16) || !Read(track.body + kBodyRot, p.rot, 16) ||
        !Read(track.body + kBodyLinear, p.linear, 16) || !Read(track.body + kBodyAngular, p.angular, 16)) return false;
    if (ValidPose(p) && SendToPeers(&p, sizeof(p))) { InterlockedIncrement(&posesSent); return true; }
    return false;
}
float Length2(const float* v) { return v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; }

// The flight is over: the partner's copy goes to where the local one lies.
void FinishTrack(const char* why) {
    if (!track.body) return;
    float pos[4]{};
    Read(track.body + kBodyPos, pos, 12);
    const float dx = pos[0] - track.startPos[0], dy = pos[1] - track.startPos[1], dz = pos[2] - track.startPos[2];
    if (InterlockedIncrement(&eventLines) <= 200)
        gate::Event("coop_tk %08X-%08X-%08X-%08X comes to rest (%s) after %lu ms, %.1f m from where it was taken",
                    track.guid[0], track.guid[1], track.guid[2], track.guid[3], why,
                    track.releasedAt ? GetTickCount() - track.releasedAt : 0ul, std::sqrt(dx * dx + dy * dy + dz * dz));
    track.phase = kPhaseRest;
    track.restLeft = kRestRepeats;
}

// Game thread, from Poll: what the local Isaac holds, or has let go of, goes out.
void ReceiverTick(DWORD now);
void Tick(DWORD now) {
    if (!installed || !enabled || !streamPose || !peerTable.Count()) return;
    SampleCells(now);
    ReceiverTick(now);
    if (!inLevel || !levelSettled) { track = Track{}; return; }
    if (now - track.lastSend < kPoseIntervalMs) return;
    const uint32_t held = LocalHeldBody();
    uint32_t handle = 0;
    const uint32_t heldEntity = held && Read(lastSm + kSmTarget, &handle, 4) && handle ? handle - 0x10 : 0;
    if (held && heldEntity && held != track.body) {
        uint32_t oldCount = 0;
        if (track.body && track.phase != kPhaseRest && mirror::ObjectByGuid(track.guid) == track.entity &&
            BodyAt(track.entity, track.index, &oldCount) == track.body) {
            FinishTrack("a new grab");
            SendPose(kPhaseRest);
        }
        track = Track{};
        track.entity = heldEntity;
        track.body = held;
        track.phase = kPhaseHeld;
        Read(heldEntity + 0x24, track.guid, 16);
        track.index = BodyIndex(heldEntity, held, &track.count);
        Read(held + kBodyPos, track.startPos, 12);
        InterlockedIncrement(&holds);
        if (track.index == kNoIndex) InterlockedIncrement(&noIndex);
        uint32_t vtable = 0;
        unsigned char motion = 0;
        Read(heldEntity, &vtable, 4);
        Read(held + kBodyMotionType, &motion, 1);
        if (InterlockedIncrement(&eventLines) <= 200)
            gate::Event("coop_tk holding %08X (vtable %08X) guid=%08X-%08X-%08X-%08X body %08X index %d of %u, motion type %u",
                        heldEntity, vtable, track.guid[0], track.guid[1], track.guid[2], track.guid[3], held,
                        track.index == kNoIndex ? -1 : static_cast<int>(track.index), track.count, motion);
    }
    if (!track.body) return;
    if (!(track.guid[0] | track.guid[1] | track.guid[2] | track.guid[3])) { track = Track{}; return; }
    track.lastSend = now;
    if (track.phase != kPhaseHeld && held == track.body) track.phase = kPhaseHeld;   // caught again mid-flight
    if (track.phase == kPhaseHeld && held != track.body) {
        // Let go: thrown or dropped, it is followed until it lies still.
        track.phase = kPhaseFlight;
        track.releasedAt = now;
        track.launched = launchedAt && now - launchedAt < 500;
        InterlockedIncrement(&flights);
    }
    if (track.phase != kPhaseHeld) {
        // Still the same body of the same object? It can explode or unload mid-flight.
        uint32_t count = 0;
        if (track.index == kNoIndex || mirror::ObjectByGuid(track.guid) != track.entity || BodyAt(track.entity, track.index, &count) != track.body) {
            InterlockedIncrement(&flightLost);
            track = Track{};
            return;
        }
    }
    if (track.phase == kPhaseRest) {
        SendPose(kPhaseRest);
        if (track.restLeft && --track.restLeft == 0) { InterlockedIncrement(&restsSent); track = Track{}; }
        return;
    }
    if (track.phase == kPhaseFlight) {
        float linear[4]{}, angular[4]{};
        Read(track.body + kBodyLinear, linear, 12);
        Read(track.body + kBodyAngular, angular, 12);
        track.quiet = Length2(linear) < kRestSpeed2 && Length2(angular) < kRestSpin2 ? track.quiet + 1 : 0;
        if (track.quiet >= kRestSamples) { FinishTrack(track.launched ? "thrown" : "dropped"); SendPose(kPhaseRest); return; }
        if (now - track.releasedAt > kFlightMaxMs) {
            InterlockedIncrement(&flightTimeouts);
            FinishTrack("still moving after 8 s");
            SendPose(kPhaseRest);
            return;
        }
    }
    SendPose(track.phase);
}

// Receiver: the partners' objects steered here, one per partner at most.
struct Driven {
    uint32_t token = 0, sequence = 0, entity = 0, body = 0, guid[4]{}, phase = 0;
    DWORD lastAt = 0, settleAt = 0;
    bool logged = false;
};
Driven driven[coop_peers::kMaxPeers]{};

bool HardKeyFrame(uint32_t body, const float* pos, const float* rot, float invDt) {
    alignas(16) float p[4] = {pos[0], pos[1], pos[2], 1.0f};
    alignas(16) float q[4] = {rot[0], rot[1], rot[2], rot[3]};
    __try {
        reinterpret_cast<void(__cdecl*)(const float*, const float*, float, void*)>(base + kHardKeyFrameRva)(p, q, invDt, reinterpret_cast<void*>(body));
        return true;
    } __except (Fault(GetExceptionCode())) {
        return false;
    }
}
bool StopBody(uint32_t body) {
    alignas(16) float zero[4]{};
    uint32_t motionVtable = 0, setLinear = 0, setAngular = 0;
    if (!Read(body + kBodyMotion, &motionVtable, 4) || !motionVtable || !Read(motionVtable + 0x40, &setLinear, 4) ||
        !Read(motionVtable + 0x44, &setAngular, 4)) return false;
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(base + kActivateRva)(reinterpret_cast<void*>(body));
        reinterpret_cast<void(__thiscall*)(void*, const float*)>(setLinear)(reinterpret_cast<void*>(body + kBodyMotion), zero);
        reinterpret_cast<void(__thiscall*)(void*, const float*)>(setAngular)(reinterpret_cast<void*>(body + kBodyMotion), zero);
        return true;
    } __except (Fault(GetExceptionCode())) {
        return false;
    }
}
float SimStep() {
    float step = 0.0f;
    return Read(base + kSimFrameTime, &step, 4) && step > 0.001f && step < 0.2f ? step : 1.0f / 60.0f;
}

// Game thread (the receive loop runs in Poll): the partner's held object, steered to where he has it.
void OnPose(const PosePacket& p, unsigned slot) {
    if (!installed || !enabled || !streamPose || !inLevel || !levelSettled || slot >= coop_peers::kMaxPeers) return;
    InterlockedIncrement(&posesReceived);
    Driven& d = driven[slot];
    if (d.token != p.token) d = Driven{};
    if (d.token && static_cast<int32_t>(p.sequence - d.sequence) <= 0) { InterlockedIncrement(&posesStale); return; }
    d.token = p.token;
    d.sequence = p.sequence;
    d.lastAt = 0;
    d.settleAt = 0;
    d.phase = 0;
    const uint32_t object = mirror::ObjectByGuid(p.guid);
    if (!object) { InterlockedIncrement(&posesNotFound); return; }
    if (guidfix::IsGhostEntity(object)) { InterlockedIncrement(&posesGhost); return; }   // TNL moves ghosts, not this module
    uint32_t count = 0;
    const uint32_t body = BodyAt(object, p.bodyIndex, &count);
    if (!body || (p.bodyCount && p.bodyCount != count)) {
        InterlockedIncrement(&posesNoBody);
        if (!d.logged && InterlockedIncrement(&eventLines) <= 200)
            gate::Event("coop_tk partner's object %08X guid=%08X-%08X-%08X-%08X: no body %d here (%u bodies, partner has %u)", object,
                        p.guid[0], p.guid[1], p.guid[2], p.guid[3], p.bodyIndex == kNoIndex ? -1 : static_cast<int>(p.bodyIndex), count, p.bodyCount);
        d.logged = true;
        return;
    }
    unsigned char motion = 0;
    if (!Read(body + kBodyMotionType, &motion, 1) || motion == kMotionFixed) { InterlockedIncrement(&posesFixed); return; }
    if (body == LocalHeldBody()) { InterlockedIncrement(&posesOwnHold); return; }   // both hold it: each keeps his own
    d.lastAt = Stamp(GetTickCount());
    d.phase = p.phase;
    if (d.body != body) {
        d.entity = object;
        d.body = body;
        std::memcpy(d.guid, p.guid, 16);
        d.logged = false;
        uint32_t vtable = 0;
        Read(object, &vtable, 4);
        if (InterlockedIncrement(&eventLines) <= 200)
            gate::Event("coop_tk steering partner's object %08X (vtable %08X) guid=%08X-%08X-%08X-%08X body %08X index %u of %u, motion type %u, %s",
                        object, vtable, p.guid[0], p.guid[1], p.guid[2], p.guid[3], body, p.bodyIndex, count, motion, PhaseName(p.phase));
    }
    bool ok;
    if (p.phase == kPhaseRest) {
        // Onto the spot within one step, then still (Tick clears the velocities).
        ok = HardKeyFrame(body, p.pos, p.rot, 1.0f / SimStep());
        if (ok) d.settleAt = Stamp(GetTickCount());
    } else {
        // Where the partner's body will be a packet from now. In flight the
        // path is ballistic, so it is also carried over the time the packet
        // spent on the wire; a held body follows his aim and is not.
        float lead = kLeadSeconds;
        if (p.phase == kPhaseFlight) {
            const float wire = static_cast<float>(netclock::OneWay()) / 1000.0f;
            lead += wire > kMaxWireLead ? kMaxWireLead : wire;
        }
        const float ahead[3] = {p.pos[0] + p.linear[0] * lead, p.pos[1] + p.linear[1] * lead, p.pos[2] + p.linear[2] * lead};
        ok = HardKeyFrame(body, ahead, p.rot, 1.0f / kLeadSeconds);
        d.settleAt = 0;
    }
    if (ok) InterlockedIncrement(&posesApplied);
}

// A body a partner holds is no kinesis target here ([Kinesis] PartnerHoldLock).
// PlayerFireTKSM::GetTargetPositionFromCast (0x008A3530, thiscall(sm, mode, out*, x),
// ret 0xC) answers the cast with the body it hit; ExecuteAttack (0x008A48A0) gives up
// on 0, before any grab. Both holding one prop left each machine steering its own copy.
bool holdLock = false;
safetyhook::InlineHook castHook;
volatile LONG castRefused = 0, castLines = 0;
DWORD castNoticeAt = 0;
bool PartnerHolds(uint32_t body, DWORD now) {
    const DWORD fresh = netclock::Window(500, 1500);
    for (unsigned i = 0; i < coop_peers::kMaxPeers; ++i) {
        const auto& d = driven[i];
        if (d.body == body && d.phase == kPhaseHeld && d.lastAt && now - d.lastAt < fresh && peerTable.peers[i].token == d.token) return true;
    }
    return false;
}
uint32_t __fastcall OnCastTarget(void* sm, void*, int mode, void* out, int extra) {
    const uint32_t hit = castHook.thiscall<uint32_t>(sm, mode, out, extra);
    if (!hit || !holdLock) return hit;
    const DWORD now = GetTickCount();
    if (!PartnerHolds(hit, now)) return hit;
    InterlockedIncrement(&castRefused);
    if (mode == 0 && now - castNoticeAt > 2000) {
        castNoticeAt = now;
        notice::Show("A PARTNER HOLDS THIS");
        if (InterlockedIncrement(&castLines) <= 40) gate::Event("coop_tk grab of body %08X refused: a partner holds it", hit);
    }
    return 0;
}

void ReceiverTick(DWORD now) {
    for (unsigned slot = 0; slot < coop_peers::kMaxPeers; ++slot) {
        Driven& d = driven[slot];
        if (!inLevel || !levelSettled || d.token != peerTable.peers[slot].token) { d = Driven{}; continue; }
        if (!d.settleAt || now - d.settleAt < 40) continue;
        d.settleAt = 0;
        // The object may be gone by now; only a body its entity still has is touched.
        uint32_t count = 0, index = 0;
        if (mirror::ObjectByGuid(d.guid) != d.entity || (index = BodyIndex(d.entity, d.body, &count)) == kNoIndex) continue;
        if (d.body != LocalHeldBody() && StopBody(d.body)) InterlockedIncrement(&settles);
    }
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Kinesis", "Observe", 0, ini.c_str()) != 0;
    streamPose = enabled && GetPrivateProfileIntA("Kinesis", "StreamHeld", 0, ini.c_str()) != 0;
    static const unsigned char kGrab[] = {0x51,0x55,0x8B,0xE9,0x8B,0x85,0x04,0x01,0x00,0x00};
    static const unsigned char kDrop[] = {0x51,0x56,0x57,0x8B,0xF9,0x8B,0x87,0x04,0x01,0x00,0x00};
    static const unsigned char kLaunch[] = {0x83,0xEC,0x14,0x53,0x56,0x57,0x8B,0xF9,0x8B,0x87,0x04,0x01,0x00,0x00};
    // push ebp / mov ebp,esp / and esp,-16 / mov eax,[ebp+0xC] / sub esp,0x18 / push ebx / push esi / mov esi,ecx
    static const unsigned char kCtor[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x8B,0x45,0x0C,0x83,0xEC,0x18,0x53,0x56,0x8B,0xF1};
    bool ok = hook_site::Install(log, "coop_tk GrabNotify", base + kGrabNotifyRva, kGrab, sizeof(kGrab), reinterpret_cast<void*>(OnGrab), grabHook);
    ok = hook_site::Install(log, "coop_tk DropNotify", base + kDropNotifyRva, kDrop, sizeof(kDrop), reinterpret_cast<void*>(OnDrop), dropHook) && ok;
    ok = hook_site::Install(log, "coop_tk LaunchNotify", base + kLaunchNotifyRva, kLaunch, sizeof(kLaunch), reinterpret_cast<void*>(OnLaunch), launchHook) && ok;
    ok = hook_site::Install(log, "coop_tk PlayerFireTKSM ctor", base + kSmCtorRva, kCtor, sizeof(kCtor), reinterpret_cast<void*>(OnCtor), ctorHook) && ok;
    installed = ok;
    std::fprintf(log, "coop_tk observe=%d stream_held=%d installed=%d (the held body and its flight steer the partner's copy)\n",
                 enabled, streamPose, installed);
    if (installed && streamPose && GetPrivateProfileIntA("Kinesis", "PartnerHoldLock", 1, ini.c_str()) != 0) {
        // push ebp / mov ebp,esp / and esp,-16 / sub esp,464h / push ebx / push esi / mov esi,ecx
        static const unsigned char kCast[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x81,0xEC,0x64,0x04,0x00,0x00,0x53,0x56,0x8B,0xF1};
        holdLock = hook_site::Install(log, "coop_tk PlayerFireTKSM::GetTargetPositionFromCast", base + 0x4A3530, kCast, sizeof(kCast),
                                      reinterpret_cast<void*>(OnCastTarget), castHook);
    }
    std::fprintf(log, "coop_tk partner_hold_lock=%d (a body a partner holds is no kinesis target here)\n", holdLock ? 1 : 0);
    return ok;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG activity = InterlockedCompareExchange(&grabs, 0, 0) + InterlockedCompareExchange(&drops, 0, 0) +
                          InterlockedCompareExchange(&botGrabs, 0, 0) + InterlockedCompareExchange(&botLaunches, 0, 0) +
                          InterlockedCompareExchange(&botDrops, 0, 0) + InterlockedCompareExchange(&botMachines, 0, 0) +
                          InterlockedCompareExchange(&launches, 0, 0) + InterlockedCompareExchange(&received, 0, 0) +
                          InterlockedCompareExchange(&holds, 0, 0) + InterlockedCompareExchange(&flights, 0, 0) +
                          InterlockedCompareExchange(&posesSent, 0, 0) / 30 + InterlockedCompareExchange(&posesReceived, 0, 0) / 30 +
                          InterlockedCompareExchange(&settles, 0, 0) + InterlockedCompareExchange(&engineFaults, 0, 0);
    if (activity == last) return;
    last = activity;
    std::fprintf(log, "coop_tk grabs=%ld drops=%ld launches=%ld no_target=%ld sent=%ld received=%ld partner_found=%ld partner_missing=%ld "
                      "machine=%08X machines_seen=%ld owner_mismatch=%ld\n",
                 InterlockedCompareExchange(&grabs, 0, 0), InterlockedCompareExchange(&drops, 0, 0),
                 InterlockedCompareExchange(&launches, 0, 0), InterlockedCompareExchange(&noTarget, 0, 0),
                 InterlockedCompareExchange(&sent, 0, 0), InterlockedCompareExchange(&received, 0, 0),
                 InterlockedCompareExchange(&partnerFound, 0, 0), InterlockedCompareExchange(&partnerMissing, 0, 0), lastSm,
                 InterlockedCompareExchange(&smSeen, 0, 0), InterlockedCompareExchange(&ownerMismatch, 0, 0));
    std::fprintf(log, "coop_tk bots machines=%ld grabs=%ld launches=%ld drops=%ld\n", InterlockedCompareExchange(&botMachines, 0, 0),
                 InterlockedCompareExchange(&botGrabs, 0, 0), InterlockedCompareExchange(&botLaunches, 0, 0), InterlockedCompareExchange(&botDrops, 0, 0));
    std::fprintf(log, "coop_tk holder holds=%ld no_index=%ld flights=%ld rests=%ld timeouts=%ld lost=%ld poses_sent=%ld tracking=%08X phase=%s\n",
                 InterlockedCompareExchange(&holds, 0, 0), InterlockedCompareExchange(&noIndex, 0, 0),
                 InterlockedCompareExchange(&flights, 0, 0), InterlockedCompareExchange(&restsSent, 0, 0),
                 InterlockedCompareExchange(&flightTimeouts, 0, 0), InterlockedCompareExchange(&flightLost, 0, 0),
                 InterlockedCompareExchange(&posesSent, 0, 0), track.entity, track.body ? PhaseName(track.phase) : "-");
    std::fprintf(log, "coop_tk receiver poses=%ld applied=%ld settled=%ld stale=%ld not_found=%ld ghost=%ld no_body=%ld fixed=%ld own_hold=%ld faults=%ld\n",
                 InterlockedCompareExchange(&posesReceived, 0, 0), InterlockedCompareExchange(&posesApplied, 0, 0),
                 InterlockedCompareExchange(&settles, 0, 0), InterlockedCompareExchange(&posesStale, 0, 0),
                 InterlockedCompareExchange(&posesNotFound, 0, 0), InterlockedCompareExchange(&posesGhost, 0, 0),
                 InterlockedCompareExchange(&posesNoBody, 0, 0), InterlockedCompareExchange(&posesFixed, 0, 0),
                 InterlockedCompareExchange(&posesOwnHold, 0, 0), InterlockedCompareExchange(&engineFaults, 0, 0));
    if (holdLock) std::fprintf(log, "coop_tk partner_hold_lock grabs_refused=%ld\n", InterlockedCompareExchange(&castRefused, 0, 0));
}
}  // namespace tk
