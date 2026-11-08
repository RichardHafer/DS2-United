// Dead Space 2 United by Hafer.
// A placeable turret for the local player ([Turret], arithmetic in coop_turret.h,
// evidence in analysis/DS2_TURRET_2026-10-08.md).
//
// One key puts it on the floor in front of the player (the NavPower mesh and
// the camera cast decide whether there is room), the same key takes it back.
// While it stands it picks the nearest fighting necromorph the bot senses see
// (bot_sense::Scan), aims at a limb's ragdoll body and hurts it the way a
// projectile does: Projectile::ProcessImpactDamage builds a DamageData (PC ctor
// 0x00ECFDE0, thiscall, 9 args, ret 0x24) with the shooter as attacker, MOD_GUN
// and the hit bone, sets its impact direction (0x00EAE130, thiscall, ret 8) and
// queues it with SendDelayedDamageEvent (0x00EAE1E0, cdecl(entity, data)); the
// DamageData destructor (0x00877540) follows. The attacker is the player, so
// kills, scoring and loot are his. The DismemberableHealthComponent maps the
// bone back to its zone (0x00B94320), so a limb shot can take the limb off.
//
// The body is a pickup: a global SpareAmmo spawned with GetGlobalPickupItem and
// dropped with Item::Drop (vtable +0x1F0), the game's own enemy-drop path, then
// made unpickable (flag bit 0 at +0x248 cleared, UpdatePickupRegistrationState
// 0x00DB42E0 takes it off the targeting list) so nobody can collect it.
//
// Feedback is the game's own Alchemy effects: the pulse rifle's muzzle effect
// (with its authored shot sound) at the muzzle, the Seeker Rifle's tracer through
// BulletAttack::CreateTracer, the impact the projectile itself would make
// (VFX::PlayImpactEffect with the hit body's collidable and the pulse rifle's
// ammo type) and a hologram over the box while it is ready to fire.
namespace turret {
using coop_turret::V3;

bool wanted = false, installed = false, damageReady = false, bodyWanted = true, bodyReady = false, losReady = false;
int key = 102;   // Numpad 6
coop_turret::Config cfg;
uint32_t bodyGuid[4] = {0x4BC78C36, 0x9D67757C, 0x544E4543, 0x53574F4B};   // Pulse Rifle ammo, global_assets

// --- engine ------------------------------------------------------------------
constexpr uint32_t kDamageCtorRva = 0xACFDE0, kImpactDirRva = 0xAAE130, kSendDelayedRva = 0xAAE1E0, kDamageDtorRva = 0x477540;
constexpr uint32_t kDamageVtableStore = 0x39, kSendMsgPush = 0x82;   // vtable store in the ctor, push of iMsgDamage in the sender
constexpr uint32_t kModGun = 4;                                     // meansOfDeath_t MOD_GUN (Xbox _index_ table, type 4 on live gun hits)
constexpr uint32_t kGlobalPickupRva = 0x9E7560, kItemDropSlot = 0x1F0, kItemDropRva = 0x9D7660, kUpdateRegistrationRva = 0x9B42E0,
                   kBaseDeleteRva = 0x3568B0;
constexpr uint32_t kItemOwner = 0x240, kItemFlags = 0x248, kItemRegistered = 0x298, kHiddenCount = 0x16D, kEntityGuid = 0x24;
constexpr uint32_t kCastingTestRva = 0xB5B180, kSimMsRva = 0x1C4D3C0;
// The ragdoll's joint maps (bot_sense, measured 27./28.09.): one per physics body.
constexpr uint32_t kRagdollVtableRva = 0x174491C, kJointMaps = 0x2C, kJointMapCount = 0x30, kJointMapSize = 0x2B0, kJointBone = 0xD0,
                   kJointBody = 0x1B0, kJointZone = 0x1B4, kMaxJoints = 48;
constexpr uint32_t kBodyFrame = 0xF0, kBodyShape = 0x10, kShapeType = 0x0C, kShapeA = 0x20, kShapeB = 0x30, kShapeCapsule = 5, kShapeTranslated = 11;

// --- state -------------------------------------------------------------------
struct Placed {
    bool on = false;
    V3 spot, muzzle;
    float yaw = 0.0f;
    uint32_t owner = 0;
    uint32_t body = 0, bodyVtable = 0;
    DWORD placedAt = 0;
};
Placed t;
coop_turret::Magazine mag;
coop_turret::Pacer pacer;
coop_turret::Edge keyEdge;
uint32_t target = 0, targetVtable = 0;
coop_turret::Part aim;
uint32_t lastSim = 0, scanAt = 0, bodyCheckAt = 0;
bool simKnown = false;
LONG startsSeen = 0, clearsSeen = 0;
DWORD overheatNoticeAt = 0, emptyNoticeAt = 0;
bool wasOverheated = false, wasEmpty = false;
bool active = false;   // placed and allowed to fire this tick

// Shots on their way: did the health go down?
struct Pending { uint32_t entity = 0; float before = 0.0f; uint32_t sentAt = 0; int bone = -1, zone = -1; };
Pending pending[8];
unsigned pendingNext = 0;
// Targets killed lately, counted once each.
uint32_t killedRecently[16]{};
unsigned killedNext = 0;

volatile LONG presses = 0, placedCount = 0, pickedUp = 0, forgotten = 0, tooFar = 0;
volatile LONG refused[7]{};   // by coop_turret::Place
volatile LONG refusedState = 0;   // cutscene, loading, no player
volatile LONG scans = 0, candidates = 0, targetsChosen = 0, targetSwitches = 0, noTarget = 0;
volatile LONG shots = 0, sent = 0, applied = 0, noEffect = 0, kills = 0, blockedRays = 0, friendHolds = 0, turning = 0;
volatile LONG overheats = 0, empties = 0, refreshLost = 0, cutsceneFrames = 0, pausedFrames = 0;
volatile LONG damageFaults = 0, castFaults = 0, castCalls = 0, bodyFaults = 0, bodySpawned = 0, bodyRemoved = 0, bodyGone = 0, bodyRearmed = 0, tickFaults = 0;
volatile LONG eventLines = 0, shotLines = 0, killLines = 0, effectLines = 0, faultLines = 0;
constexpr LONG kEventLines = 80, kShotLines = 12, kKillLines = 20, kEffectLines = 12, kFaultLines = 20;

V3 ToV3(const coop_bot::Vec3& v) { return {v.x, v.y, v.z}; }
coop_bot::Vec3 ToVec3(const V3& v) { return {v.x, v.y, v.z}; }
bool Pos(uint32_t entity, V3& out) {
    float p[3]{};
    if (!entity || !Read(entity + 0xC0, p, 12)) return false;
    out = {p[0], p[1], p[2]};
    return coop_turret::Finite(out) && std::fabs(out.x) < 100000.0f && std::fabs(out.y) < 100000.0f && std::fabs(out.z) < 100000.0f;
}
bool Facing(uint32_t entity, V3& out) {
    float f[3]{};
    if (!Read(entity + 0xB0, f, 12)) return false;
    out = {f[0], f[1], f[2]};
    return coop_turret::Finite(out);
}
uint32_t SimMs() {
    uint32_t ms = 0;
    return Read(base + kSimMsRva, &ms, 4) ? ms : 0;
}
bool Notice(const char* text) { return notice::enabled && notice::Show(text); }
void Line(const char* format, ...) {
    if (InterlockedIncrement(&eventLines) > kEventLines) return;
    char buffer[240];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    gate::Event("turret: %s", buffer);
}
void Fault(const char* what, uint32_t a, uint32_t b) {
    if (InterlockedIncrement(&faultLines) <= kFaultLines) gate::Event("turret FAULT %s (%08X %08X)", what, a, b);
}

// --- the camera cast (CollisionCamera::CastingTest, as bot_sense calls it) -----
DWORD castFaultTimes[5]{};
unsigned castFaultNext = 0;
__declspec(noinline) bool CastRaw(const float* from, const float* to, uint32_t exclude, float& fraction, uint32_t& hit, float* at) {
    using Cast = float(__cdecl*)(const float*, const float*, const void*, const uint32_t*, uint32_t, float*, uint32_t*);
    __try {
        fraction = reinterpret_cast<Cast>(base + kCastingTestRva)(from, to, reinterpret_cast<const void*>(static_cast<uintptr_t>(exclude)), nullptr, 0, at, &hit);
        return std::isfinite(fraction);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// False on a fault. Five faults within ten seconds switch the cast off until the next level.
bool Cast(const V3& a, const V3& b, uint32_t exclude, float& fraction, uint32_t& hit, V3* point) {
    if (!losReady) return false;
    alignas(16) float from[4] = {a.x, a.y, a.z, 1.0f}, to[4] = {b.x, b.y, b.z, 1.0f}, at[4] = {};
    fraction = 1.0f;
    hit = 0;
    InterlockedIncrement(&castCalls);
    if (CastRaw(from, to, exclude, fraction, hit, at)) {
        if (point) *point = {at[0], at[1], at[2]};
        return true;
    }
    InterlockedIncrement(&castFaults);
    const DWORD now = GetTickCount();
    castFaultTimes[castFaultNext++ % 5] = now ? now : 1u;
    bool all = true;
    for (DWORD when : castFaultTimes) all = all && when && now - when <= 10000;
    if (all) {
        losReady = false;
        Line("the camera cast faulted five times in ten seconds: the turret holds fire until the next level");
    }
    return false;
}
// Free from a to b; a hit on `goal` itself counts when it is at the far end.
bool Clear(const V3& a, const V3& b, uint32_t exclude, uint32_t goal) {
    const float len = coop_turret::Distance(a, b);
    if (len < 0.3f) return true;
    float fraction = 1.0f;
    uint32_t hit = 0;
    if (!Cast(a, b, exclude, fraction, hit, nullptr)) return false;
    if (fraction >= 0.999f) return true;
    return goal && hit == goal && fraction * len >= len - 0.45f;
}
// The same for a part, keeping the contact point when the cast met the goal.
bool ClearPart(coop_turret::Part& p, uint32_t exclude, uint32_t goal) {
    p.hitKnown = false;
    const float len = coop_turret::Distance(t.muzzle, p.at);
    if (len < 0.3f) return p.clear = true;
    float fraction = 1.0f;
    uint32_t hit = 0;
    V3 point;
    if (!Cast(t.muzzle, p.at, exclude, fraction, hit, &point)) return p.clear = false;
    if (fraction >= 0.999f) return p.clear = true;
    p.clear = goal && hit == goal && fraction * len >= len - 0.45f;
    // the contact point from the fraction: the cast's output vector is the hit normal
    if (p.clear) {
        p.hit = {t.muzzle.x + (p.at.x - t.muzzle.x) * fraction, t.muzzle.y + (p.at.y - t.muzzle.y) * fraction, t.muzzle.z + (p.at.z - t.muzzle.z) * fraction};
        p.hitKnown = true;
    }
    (void)point;
    return p.clear;
}

// --- hit parts ---------------------------------------------------------------
struct RagdollSlot { uint32_t vtable = 0, offset = 0; DWORD retryAt = 0; };
RagdollSlot ragdollSlots[32];
uint32_t Ragdoll(uint32_t actor) {
    uint32_t vtable = 0;
    if (!Read(actor, &vtable, 4) || !vtable) return 0;
    RagdollSlot& slot = ragdollSlots[(vtable >> 3) % 32];
    const uint32_t rag = static_cast<uint32_t>(base + kRagdollVtableRva);
    auto at = [&](uint32_t offset) -> uint32_t {
        uint32_t c = 0, cv = 0, owner = 0;
        return Read(actor + offset, &c, 4) && c >= 0x10000 && Read(c, &cv, 4) && cv == rag && Read(c + 0x10, &owner, 4) && owner == actor ? c : 0;
    };
    const DWORD now = GetTickCount();
    if (slot.vtable == vtable) {
        if (slot.offset) { if (const uint32_t c = at(slot.offset)) return c; }
        else if (static_cast<LONG>(now - slot.retryAt) < 0) return 0;
    }
    for (uint32_t offset = 4; offset < hits::kActorScanBytes; offset += 4)
        if (const uint32_t c = at(offset)) { slot = RagdollSlot{vtable, offset, 0}; return c; }
    slot = RagdollSlot{vtable, 0, now + 2000};
    return 0;
}
// The middle of a ragdoll body's hit shape in the world (Havok 6.5: rotation columns
// at body+0xF0/+0x100/+0x110, origin at +0x120; capsule ends or a translated sphere).
bool BodyCenter(uint32_t body, const V3& around, V3& out) {
    alignas(16) float m[16];
    uint32_t shape = 0, type = 0;
    if (!body || !Read(body + kBodyFrame, m, sizeof(m))) return false;
    const V3 c0 = {m[0], m[1], m[2]}, c1 = {m[4], m[5], m[6]}, c2 = {m[8], m[9], m[10]}, origin = {m[12], m[13], m[14]};
    auto world = [&](const float* q) {
        return coop_turret::Add(origin, coop_turret::Add(coop_turret::Scale(c0, q[0]), coop_turret::Add(coop_turret::Scale(c1, q[1]), coop_turret::Scale(c2, q[2]))));
    };
    out = origin;
    if (Read(body + kBodyShape, &shape, 4) && shape && Read(shape + kShapeType, &type, 4)) {
        float a[4]{}, b[4]{};
        if (type == kShapeCapsule && Read(shape + kShapeA, a, 16) && Read(shape + kShapeB, b, 16))
            out = coop_turret::Scale(coop_turret::Add(world(a), world(b)), 0.5f);
        else if (type == kShapeTranslated && Read(shape + kShapeA, a, 16))
            out = world(a);
    }
    return coop_turret::Finite(out) && coop_turret::Distance(out, around) < 6.0f;
}
// Every hit body of an actor: zone, bone (BoneInfo >> 2, what DamageData+0x58
// holds on live gun hits) and the middle of its shape. Severed limbs are gone from
// the list already.
int Joints(uint32_t actor, const V3& around, coop_turret::Part* out, int cap) {
    const uint32_t ragdoll = Ragdoll(actor);
    uint32_t maps = 0, count = 0;
    if (!ragdoll || !Read(ragdoll + kJointMaps, &maps, 4) || !maps || !Read(ragdoll + kJointMapCount, &count, 4) || !count || count > kMaxJoints) return 0;
    int n = 0;
    for (uint32_t j = 0; j < count && n < cap; ++j) {
        const uint32_t map = maps + j * kJointMapSize;
        uint32_t bone = 0, body = 0, zone = 0;
        V3 center;
        if (!Read(map + kJointBone, &bone, 4) || !Read(map + kJointBody, &body, 4) || !Read(map + kJointZone, &zone, 4) || zone > 10) continue;
        if (!BodyCenter(body, around, center)) continue;
        out[n] = coop_turret::Part{center, static_cast<int>(zone), static_cast<int>(bone >> 2), false};
        out[n++].body = body;
    }
    return n;
}
// The parts in sight, best ranks first, at most `rays` casts.
int SightParts(uint32_t entity, coop_turret::Part* parts, int n, int rays) {
    int order[kMaxJoints];
    for (int i = 0; i < n; ++i) order[i] = i;
    for (int a = 1; a < n; ++a)
        for (int b = a; b > 0 && coop_turret::ZoneRank(parts[order[b]].zone) < coop_turret::ZoneRank(parts[order[b - 1]].zone); --b) std::swap(order[b], order[b - 1]);
    int clear = 0;
    for (int k = 0; k < n && rays > 0; ++k) {
        coop_turret::Part& p = parts[order[k]];
        // One body per zone is enough: the first ranked of each zone gets a ray.
        bool zoneDone = false;
        for (int q = 0; q < k; ++q) zoneDone = zoneDone || (parts[order[q]].zone == p.zone && parts[order[q]].clear);
        if (zoneDone) continue;
        --rays;
        ClearPart(p, t.body, entity);
        if (p.clear) ++clear; else InterlockedIncrement(&blockedRays);
        if (p.clear && coop_turret::ZoneRank(p.zone) == 0) break;   // an arm in sight: nothing beats it
    }
    return clear;
}

// --- damage ------------------------------------------------------------------
__declspec(noinline) int SendHitRaw(uint32_t entity, uint32_t attacker, float amount, int bone, const float* at, const float* from) {
    using Ctor = void*(__thiscall*)(void*, uint32_t, uint32_t, float, uint32_t, const float*, int, uint32_t, uint32_t, float);
    using Dir = void(__thiscall*)(void*, const float*, const float*);
    using Send = void(__cdecl*)(uint32_t, const void*);
    using Dtor = void(__thiscall*)(void*);
    alignas(16) unsigned char data[0xB0];
    std::memset(data, 0, sizeof(data));
    volatile bool built = false;
    int result = 0;
    __try {
        reinterpret_cast<Ctor>(base + kDamageCtorRva)(data, attacker, 0, amount, kModGun, at, bone, 0, 0, 0.0f);
        built = true;
        reinterpret_cast<Dir>(base + kImpactDirRva)(data, from, at);
        reinterpret_cast<Send>(base + kSendDelayedRva)(entity, data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = 1;
    }
    if (built) {
        __try {
            reinterpret_cast<Dtor>(base + kDamageDtorRva)(data);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            result |= 2;
        }
    }
    return result;
}
float Health(uint32_t entity) {
    const uint32_t hc = hits::FindHealthComponent(entity);
    float current = -1.0f;
    return hc && Read(hc + hits::kHealthCurrent, &current, 4) && std::isfinite(current) ? current : -1.0f;
}
bool Hit(uint32_t entity, const coop_turret::Part& part) {
    // Never anything a player is: the scan leaves them out, this makes sure.
    if (!entity || entity == t.owner || IsPlayerOrNecroEntity(entity) || IsBotEntity(entity)) return false;
    const float before = Health(entity);
    alignas(16) float at[4] = {part.at.x, part.at.y, part.at.z, 1.0f}, from[4] = {t.muzzle.x, t.muzzle.y, t.muzzle.z, 1.0f};
    const int result = SendHitRaw(entity, t.owner, cfg.damage, part.bone, at, from);
    if (result) {
        InterlockedIncrement(&damageFaults);
        Fault(result & 1 ? "damage send" : "damage release", entity, static_cast<uint32_t>(part.bone));
        if (InterlockedCompareExchange(&damageFaults, 0, 0) >= 3) {
            damageReady = false;
            Line("three faulted hits: the turret holds fire for the rest of the session");
        }
        return false;
    }
    InterlockedIncrement(&sent);
    pending[pendingNext++ % std::size(pending)] = Pending{entity, before, lastSim, part.bone, part.zone};
    if (InterlockedIncrement(&shotLines) <= kShotLines) {
        uint32_t vtable = 0;
        Read(entity, &vtable, 4);
        gate::Event("turret: shot %ld at %08X (vtable RVA %08X) zone %d bone %d health %.1f, %.1f m, rounds %d heat %.2f", shots, entity,
                    vtable - static_cast<uint32_t>(base), part.zone, part.bone, before, coop_turret::Distance(t.muzzle, part.at), mag.Rounds(cfg), mag.heat);
    }
    return true;
}
// Whether the queued hits arrived: the health went down, or the target died.
void FollowPending() {
    for (Pending& p : pending) {
        if (!p.entity) continue;
        const float now = Health(p.entity);
        if (now >= 0.0f && now < p.before - 0.001f) {
            InterlockedIncrement(&applied);
            if (!(now > 0.0f)) {
                bool counted = false;
                for (uint32_t k : killedRecently) counted = counted || k == p.entity;
                if (!counted) {
                    killedRecently[killedNext++ % std::size(killedRecently)] = p.entity;
                    InterlockedIncrement(&kills);
                    if (InterlockedIncrement(&killLines) <= kKillLines) gate::Event("turret: kill %ld, %08X (last hit zone %d bone %d)", kills, p.entity, p.zone, p.bone);
                }
            }
            p = Pending{};
        } else if (lastSim - p.sentAt > 1500) {
            InterlockedIncrement(&noEffect);
            if (InterlockedIncrement(&effectLines) <= kEffectLines)
                gate::Event("turret: hit on %08X zone %d bone %d changed nothing in 1.5 s (health %.1f -> %.1f)", p.entity, p.zone, p.bone, p.before, now);
            p = Pending{};
        }
    }
}

// --- the body ----------------------------------------------------------------
__declspec(noinline) uint32_t SpawnRaw(const uint32_t* guid) {
    __try {
        return reinterpret_cast<uint32_t(__cdecl*)(const void*, int)>(base + kGlobalPickupRva)(guid, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
__declspec(noinline) bool DropRaw(uint32_t item, const float* pos) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, const float*)>(base + kItemDropRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(item)), pos);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool RegistrationRaw(uint32_t item) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(base + kUpdateRegistrationRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(item)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool DeleteRaw(uint32_t object) {
    __try {
        reinterpret_cast<void(__cdecl*)(uint32_t)>(base + kBaseDeleteRva)(object);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// PickupItem::SetPickupable(false) as Drop does the opposite inline: bit 0 of the flags, then the registration update.
bool MakeUnpickable(uint32_t item) {
    uint32_t flags = 0;
    if (!Read(item + kItemFlags, &flags, 4)) return false;
    flags &= ~1u;
    if (!WriteBytes(item + kItemFlags, &flags, 4)) return false;
    if (!RegistrationRaw(item)) return false;
    unsigned char registered = 1;
    return Read(item + kItemRegistered, &registered, 1) && registered == 0;
}
// For the bot senses: the turret's body is no item to collect (not wired yet, see the analysis note).
bool IsBody(uint32_t entity) { return entity && t.on && entity == t.body; }
// Still the body put down: same class, same GUID, nobody's.
bool BodyIsOurs() {
    uint32_t vtable = 0, guid[4]{}, owner = 1;
    return t.body && Read(t.body, &vtable, 4) && vtable == t.bodyVtable && Read(t.body + kEntityGuid, guid, 16) && std::memcmp(guid, bodyGuid, 16) == 0 &&
           Read(t.body + kItemOwner, &owner, 4) && owner == 0;
}
uint32_t SpawnBody(const V3& spot) {
    static const uint32_t kPickupHash = gate::NameHash("PickupItem");
    const uint32_t item = SpawnRaw(bodyGuid);
    uint32_t vtable = 0, slot = 0, owner = 1;
    if (!item) { InterlockedIncrement(&bodyFaults); Line("body: GetGlobalPickupItem gave nothing for %08X-%08X-%08X-%08X", bodyGuid[0], bodyGuid[1], bodyGuid[2], bodyGuid[3]); return 0; }
    if (!Read(item, &vtable, 4) || vtable < base + gate::kRdataLo || vtable >= base + gate::kRdataHi || !gate::IsA(item, vtable, kPickupHash) ||
        !Read(vtable + kItemDropSlot, &slot, 4) || slot != base + kItemDropRva || !Read(item + kItemOwner, &owner, 4) || owner) {
        InterlockedIncrement(&bodyFaults);
        Line("body: %08X (vtable RVA %08X, drop slot %08X, owner %08X) is no droppable pickup; deleted", item, vtable - static_cast<uint32_t>(base), slot, owner);
        DeleteRaw(item);
        return 0;
    }
    alignas(16) float pos[4] = {spot.x, spot.y + 0.15f, spot.z, 1.0f};
    if (!DropRaw(item, pos)) {
        InterlockedIncrement(&bodyFaults);
        Fault("body drop", item, vtable);
        DeleteRaw(item);
        return 0;
    }
    if (!MakeUnpickable(item)) {
        // Better no body than a free ammo pickup.
        InterlockedIncrement(&bodyFaults);
        Line("body: %08X stayed pickable after the registration update; deleted", item);
        DeleteRaw(item);
        return 0;
    }
    t.bodyVtable = vtable;
    InterlockedIncrement(&bodySpawned);
    return item;
}
void RemoveBody(const char* why) {
    if (!t.body) return;
    if (BodyIsOurs()) {
        if (DeleteRaw(t.body)) InterlockedIncrement(&bodyRemoved);
        else { InterlockedIncrement(&bodyFaults); Fault("body delete", t.body, t.bodyVtable); }
    } else {
        InterlockedIncrement(&bodyGone);
        Line("body %08X is no longer ours at %s: left alone", t.body, why);
    }
    t.body = 0;
}

// --- feedback effects ----------------------------------------------------------
// Alchemy (PC, read in code and the full dumps): Create_ByGuid 0x006A0430
// cdecl(VFXHandle* out, VFXGUID*, matrix, 0xFFFF, flags, VFXParent*) writes 0 when
// the GUID is not resident ([0x02032C70] lookup 0x0069F230) or the system is not
// running ([0x02032C68] != 7); the instance keeps a pointer back to its handle at
// +8, and SoftKill 0x0069DF90 / Kill / the level's kill-all zero both. One-shots
// are left to end on their own exactly like Gun::Use and PlayImpactEffect do; a
// handle this file keeps is soft-killed after kFxLifeMs at the latest.
constexpr uint32_t kCreateByGuidRva = 0x2A0430, kVfxFindRva = 0x29F230, kVfxManagerRva = 0x1C32C70, kVfxStateRva = 0x1C32C68, kVfxRunning = 7;
constexpr uint32_t kSoftKillRva = 0x29DF90, kSetMatrixRva = 0x29E040, kImpactFxRva = 0x7ABC20, kImpactMatrixRva = 0x1C213B8, kCreateTracerRva = 0x794650;
constexpr uint32_t kHandleOwner = 8;                                        // instance -> its VFXHandle
constexpr uint32_t kTracerMatrix = 0x20, kTracerGuid = 0xB8, kTracerAttackSize = 0xF0;   // BulletAttack fields CreateTracer reads
constexpr uint32_t kCollidable = 0x10, kCollidableType = 0x18, kCollidableFilter = 0x1C;  // hkpRigidBody -> hkpCollidable
constexpr uint32_t kPulseAmmoType = 0x8C042160;                             // Pulse Rifle CMD_SetAmmoType, a row of the impact table
constexpr DWORD kFxLifeMs = 3000;
// Pulse Rifle UseItemVFX pulserifle_muzzle (weapon stream, flash plus five sound elements).
const uint32_t kPulseMuzzleGuid[4] = {0xDD6EC6DF, 0xDD6EC6DF, 0xDD6EC6DF, 0x70DF2A01};
// wep_child_sparks (global_assets, flash and sparks, no sound element).
const uint32_t kSilentFlashGuid[4] = {0x64A366B4, 0x64A366B4, 0x64A366B4, 0x51A990B6};
// pickup_ui_pulse_ammo_sm (global_assets): the small pulse ammo hologram pickups show.
const uint32_t kArmedGuid[4] = {0xC4AA180B, 0xC4AA180B, 0xC4AA180B, 0xEFA40D8C};
// dsspark3 (global_assets): sparks with sound, for overheat and empty.
const uint32_t kTroubleGuid[4] = {0x742782E6, 0x742782E6, 0x742782E6, 0xDABECCFC};
// Seeker Rifle CMD_TracerVFX seeker_tracer, the only tracer element in the data (its weapon stream).
uint32_t tracerGuid[4] = {0x4052B175, 0x4052B175, 0x4052B175, 0x240F5183};

struct Piece {
    const char* name;
    bool wanted = true, ready = false;
    LONG played = 0, absent = 0, rated = 0, refused = 0, empty = 0, faults = 0;
    coop_turret::RateLimit rate;
};
Piece fxMuzzle{"muzzle"}, fxTracer{"tracer"}, fxImpact{"impact"}, fxSound{"sound"}, fxArmed{"armed"};
bool fxSigsOk = false;
int fxPerSecond = 8;
LONG fxStale = 0, fxReleased = 0, fxExpired = 0, fxLines = 0, troubleCues = 0;
constexpr LONG kFxLines = 30;
void FxLine(const char* format, ...) {
    if (++fxLines > kFxLines) return;
    char buffer[240];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    gate::Event("turret fx: %s", buffer);
}
void FxFault(Piece& p, const char* what, uint32_t a) {
    ++p.faults;
    Fault(what, a, static_cast<uint32_t>(p.faults));
    if (p.faults >= 3 && p.ready) {
        p.ready = false;
        Line("three faults in %s effects: %s is off for the session", p.name, p.name);
    }
}

__declspec(noinline) bool FindVfxRaw(const uint32_t* guid, uint32_t& resource) {
    resource = 0;
    uint32_t manager = 0, node = 0, buckets = 0, count = 0;
    // The lookup divides by the bucket count at +4: no table, nothing resident.
    if (!Read(base + kVfxManagerRva, &manager, 4) || !manager || !Read(manager, &buckets, 4) || !buckets || !Read(manager + 4, &count, 4) || !count) return true;
    __try {
        node = reinterpret_cast<uint32_t(__thiscall*)(void*, const void*)>(base + kVfxFindRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(manager)), guid);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return !node || Read(node, &resource, 4);
}
bool VfxRunning() {
    uint32_t state = 0;
    return Read(base + kVfxStateRva, &state, 4) && state == kVfxRunning;
}
// 1 resident, 0 not loaded, -1 the lookup faulted (five faults switch every effect off).
LONG fxLookupFaults = 0;
int Resident(const uint32_t* guid) {
    uint32_t resource = 0;
    if (FindVfxRaw(guid, resource)) return resource ? 1 : 0;
    Fault("fx lookup", guid[0], static_cast<uint32_t>(++fxLookupFaults));
    if (fxLookupFaults >= 5 && fxSigsOk) {
        fxSigsOk = false;
        Line("five faulted effect lookups: all turret effects are off for the session");
    }
    return -1;
}
__declspec(noinline) bool CreateRaw(uint32_t* slot, const uint32_t* guid, const float* matrix) {
    alignas(16) uint32_t parent[4] = {};   // VFXParent type 0: no parent
    __try {
        reinterpret_cast<void*(__cdecl*)(uint32_t*, const void*, const float*, uint32_t, int, const void*)>(base + kCreateByGuidRva)(slot, guid, matrix, 0xFFFF, 0, parent);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool SoftKillRaw(uint32_t* slot) {
    __try {
        reinterpret_cast<void(__thiscall*)(uint32_t*)>(base + kSoftKillRva)(slot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool SetMatrixRaw(uint32_t* slot, const float* matrix) {
    __try {
        reinterpret_cast<void(__thiscall*)(uint32_t*, const float*)>(base + kSetMatrixRva)(slot, matrix);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool ImpactRaw(const float* pos, const float* normal, uint32_t collidable, const float* from) {
    using Play = void(__cdecl*)(const float*, const float*, uint32_t, const uint32_t*, uint32_t, const float*, const float*, uint32_t, uint32_t, uint32_t);
    __try {
        reinterpret_cast<Play>(base + kImpactFxRva)(pos, normal, collidable, nullptr, kPulseAmmoType, nullptr, from, t.owner, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
alignas(16) unsigned char tracerAttack[kTracerAttackSize];
__declspec(noinline) bool TracerRaw(const float* from, const float* to) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, const float*, const float*)>(base + kCreateTracerRva)(tracerAttack, from, to);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A handle is ours while its effect still points back at it; anything else is
// dropped without being touched.
bool HandleLive(uint32_t* slot) {
    const uint32_t h = *slot;
    uint32_t back = 0;
    if (!h) return false;
    if (Read(h + kHandleOwner, &back, 4) && back == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(slot))) return true;
    *slot = 0;
    ++fxStale;
    return false;
}
void Release(uint32_t* slot, Piece& p) {
    if (!HandleLive(slot)) return;
    if (SoftKillRaw(slot)) ++fxReleased;
    else FxFault(p, "fx soft kill", *slot);
    *slot = 0;
}
struct FxSlot { uint32_t handle = 0; DWORD born = 0; Piece* owner = nullptr; };
FxSlot fxRing[16];
unsigned fxNext = 0;
uint32_t armedHandle = 0;
DWORD armedRetryAt = 0;
bool armedShown = false, cueHold = false;

// A one-shot at a matrix, kept in the ring until it ends or kFxLifeMs pass.
bool PlayOneShot(Piece& p, const uint32_t* guid, const float* matrix) {
    FxSlot& s = fxRing[fxNext++ % std::size(fxRing)];
    if (s.handle) Release(&s.handle, s.owner ? *s.owner : p);   // the slot is empty afterwards: Create must not overwrite a live handle
    if (!CreateRaw(&s.handle, guid, matrix)) { s.handle = 0; FxFault(p, "fx create", guid[0]); return false; }
    if (!s.handle) { ++p.empty; return false; }
    s.born = GetTickCount();
    s.owner = &p;
    ++p.played;
    return true;
}
void SweepFx() {
    const DWORD now = GetTickCount();
    for (FxSlot& s : fxRing)
        if (s.handle && HandleLive(&s.handle) && now - s.born > kFxLifeMs) {
            Release(&s.handle, s.owner ? *s.owner : fxMuzzle);
            ++fxExpired;
        }
}
void ReleaseAllFx(const char* why) {
    LONG live = 0;
    for (FxSlot& s : fxRing)
        if (s.handle) { ++live; Release(&s.handle, s.owner ? *s.owner : fxMuzzle); }
    if (armedHandle) { ++live; Release(&armedHandle, fxArmed); }
    armedShown = false;
    if (live) FxLine("released %ld effect handles at %s", live, why);
}

// Muzzle (with the authored shot sound), tracer and impact for one shot.
void ShotEffects(const coop_turret::Part& part, uint32_t victim) {
    if (!fxSigsOk || !VfxRunning()) return;
    const V3 hit = coop_turret::ImpactPoint(part.at, part.hit, part.hitKnown, 0.6f);
    const V3 line = coop_turret::Sub(hit, t.muzzle);
    alignas(16) float m[16];
    if (!coop_turret::Basis(line, t.muzzle, m)) { ++fxMuzzle.refused; ++fxTracer.refused; ++fxImpact.refused; return; }
    const DWORD now = GetTickCount();

    const bool flash = fxMuzzle.wanted && fxMuzzle.ready, sound = fxSound.wanted && fxSound.ready;
    const int pulse = flash || sound ? Resident(kPulseMuzzleGuid) : 0;
    const coop_turret::ShotFx fx = coop_turret::ChooseShotFx(flash, sound, pulse == 1);
    if (sound && fx != coop_turret::ShotFx::PulseMuzzle) {
        if (pulse != 1) ++fxSound.absent;
        else ++fxSound.refused;   // FireSound without MuzzleFlash: the sound only comes with the muzzle effect
    }
    if (fx != coop_turret::ShotFx::None) {
        if (!fxMuzzle.rate.Allow(now, fxPerSecond)) {
            ++fxMuzzle.rated;
            if (fx == coop_turret::ShotFx::PulseMuzzle) ++fxSound.rated;
        } else if (fx == coop_turret::ShotFx::PulseMuzzle) {
            if (PlayOneShot(fxMuzzle, kPulseMuzzleGuid, m)) {
                ++fxSound.played;
                if (fxMuzzle.played <= 2) FxLine("muzzle pulserifle_muzzle (flash and shot sound) at %.2f %.2f %.2f toward %.2f %.2f %.2f", t.muzzle.x, t.muzzle.y,
                                                 t.muzzle.z, hit.x, hit.y, hit.z);
            }
        } else if (Resident(kSilentFlashGuid) == 1) {
            if (PlayOneShot(fxMuzzle, kSilentFlashGuid, m) && fxMuzzle.played <= 2)
                FxLine("muzzle wep_child_sparks (no sound%s)", flash && sound ? ": the pulse rifle stream is not loaded" : "");
        } else {
            ++fxMuzzle.absent;
        }
    }

    if (fxTracer.wanted && fxTracer.ready) {
        const int resident = Resident(tracerGuid);
        if (resident != 1) {
            ++fxTracer.absent;
            if (fxTracer.absent == 1) FxLine("tracer %08X-%08X not loaded (the Seeker Rifle stream carries it)", tracerGuid[0], tracerGuid[3]);
        } else if (!fxTracer.rate.Allow(now, fxPerSecond)) {
            ++fxTracer.rated;
        } else {
            std::memset(tracerAttack, 0, sizeof(tracerAttack));
            std::memcpy(tracerAttack + kTracerMatrix, m, sizeof(m));
            std::memcpy(tracerAttack + kTracerGuid, tracerGuid, 16);
            const float from[3] = {t.muzzle.x, t.muzzle.y, t.muzzle.z}, to[3] = {hit.x, hit.y, hit.z};
            if (TracerRaw(from, to)) {
                ++fxTracer.played;
                if (fxTracer.played == 1) FxLine("tracer from %.2f %.2f %.2f to %.2f %.2f %.2f (%.1f m)", from[0], from[1], from[2], to[0], to[1], to[2],
                                                 coop_turret::Length(line));
            } else {
                FxFault(fxTracer, "fx tracer", tracerGuid[0]);
            }
        }
    }

    if (fxImpact.wanted && fxImpact.ready) {
        uint32_t matrixTable = 0, shape = 0;
        unsigned char type = 0;
        const uint32_t collidable = part.body ? part.body + kCollidable : 0;
        if (!collidable) {
            ++fxImpact.refused;   // an actor without ragdoll: no body to hit
        } else if (!Read(base + kImpactMatrixRva, &matrixTable, 4) || !matrixTable || !Read(collidable, &shape, 4) || !shape ||
                   !Read(collidable + kCollidableType, &type, 1) || type != 1) {
            ++fxImpact.refused;
            if (fxImpact.refused <= 3) FxLine("impact refused: table %08X collidable %08X shape %08X type %u", matrixTable, collidable, shape, type);
        } else if (!fxImpact.rate.Allow(now, fxPerSecond)) {
            ++fxImpact.rated;
        } else {
            const V3 back = coop_turret::Scale(line, -1.0f / coop_turret::Length(line));
            alignas(16) float pos[4] = {hit.x, hit.y, hit.z, 1.0f}, normal[4] = {back.x, back.y, back.z, 0.0f}, from[4] = {t.muzzle.x, t.muzzle.y, t.muzzle.z, 1.0f};
            if (ImpactRaw(pos, normal, collidable, from)) {
                ++fxImpact.played;
                if (fxImpact.played <= 3) {
                    uint32_t filter = 0;
                    Read(collidable + kCollidableFilter, &filter, 4);
                    FxLine("impact on %08X zone %d bone %d collidable %08X filter %08X at %.2f %.2f %.2f (%s)", victim, part.zone, part.bone, collidable, filter,
                           hit.x, hit.y, hit.z, part.hitKnown ? "cast contact" : "shape middle");
                }
            } else {
                FxFault(fxImpact, "fx impact", collidable);
            }
        }
    }
}

// Sparks with their sound at the muzzle when it overheats or runs dry.
void TroubleCue(const char* why) {
    if (!fxSigsOk || !fxArmed.wanted || !fxArmed.ready || !t.on || !VfxRunning()) return;
    alignas(16) float m[16];
    const V3 ahead = {std::sin(t.yaw), 0.0f, std::cos(t.yaw)};
    if (!coop_turret::Basis(ahead, t.muzzle, m)) return;
    if (Resident(kTroubleGuid) != 1) { ++fxArmed.absent; return; }
    if (PlayOneShot(fxArmed, kTroubleGuid, m)) {
        ++troubleCues;
        FxLine("%s cue: dsspark3 at the muzzle", why);
    }
}

// The hologram over the box while the turret is ready to fire, turned like the
// player as PlayerItemPickupSM::RefreshVFX turns its prompts.
void ArmedCue(DWORD wall, bool ready, uint32_t player) {
    const bool want = fxSigsOk && fxArmed.wanted && fxArmed.ready && t.on && ready;
    if (!want) {
        if (armedHandle) Release(&armedHandle, fxArmed);
        if (armedShown) { armedShown = false; FxLine("armed hologram off"); }
        return;
    }
    // Rows right, up, forward of the player's world matrix (entity+0xD0, four floats each).
    alignas(16) float m[16];
    if (!player || !Read(player + 0xD0, m, 48)) return;
    m[3] = m[7] = m[11] = 0.0f;
    m[12] = t.spot.x;
    m[13] = t.spot.y + cfg.muzzleHeight + 0.35f;
    m[14] = t.spot.z;
    m[15] = 1.0f;
    for (float v : m)
        if (!std::isfinite(v)) return;
    if (HandleLive(&armedHandle)) {
        if (!SetMatrixRaw(&armedHandle, m)) FxFault(fxArmed, "fx armed matrix", armedHandle);
        return;
    }
    if (armedShown) { armedShown = false; FxLine("armed hologram ended by itself; shown again within a second"); }
    if (static_cast<LONG>(wall - armedRetryAt) < 0 || !VfxRunning()) return;
    armedRetryAt = wall + 1000;
    if (Resident(kArmedGuid) != 1) { ++fxArmed.absent; return; }
    if (!CreateRaw(&armedHandle, kArmedGuid, m)) { armedHandle = 0; FxFault(fxArmed, "fx armed create", kArmedGuid[0]); return; }
    if (!armedHandle) { ++fxArmed.empty; return; }
    ++fxArmed.played;
    armedShown = true;
    FxLine("armed hologram on (handle %08X) at %.2f %.2f %.2f", armedHandle, m[12], m[13], m[14]);
}

LONG FxSum() {
    LONG sum = troubleCues + fxReleased + fxExpired + fxStale + fxLookupFaults;
    for (const Piece* p : {&fxMuzzle, &fxSound, &fxTracer, &fxImpact, &fxArmed}) sum += p->played + p->absent + p->rated + p->refused + p->empty + p->faults;
    return sum;
}

// What a placement can show: which effects are resident in this level.
void LogFxResidency() {
    if (!fxSigsOk) return;
    uint32_t state = 0, table = 0;
    Read(base + kVfxStateRva, &state, 4);
    Read(base + kImpactMatrixRva, &table, 4);
    FxLine("resident at placement: vfx_state=%u pulse_muzzle=%d silent_flash=%d tracer=%d armed=%d trouble=%d impact_table=%08X", state, Resident(kPulseMuzzleGuid),
           Resident(kSilentFlashGuid), Resident(tracerGuid), Resident(kArmedGuid), Resident(kTroubleGuid), table);
}

// --- placing and taking ------------------------------------------------------
bool FrontendNow() {
    char name[16]{};
    const uint32_t loop = level::GameMainLoop();
    return loop && Read(loop + level::kMapOffset, name, sizeof(name) - 1) && _strnicmp(name, "frontend", 8) == 0;
}
bool CutsceneNow() { return cutscene::SequenceRunningNow() || cutscene::Following(); }
bool WorldReady() {
    return inLevel && levelSettled && !FrontendNow() && (!loadsync::installed || loadsync::state == loadsync::kPlaying);
}
bool rescan = false;
void ClearTarget() {
    if (target) rescan = true;
    target = 0;
    targetVtable = 0;
    aim = coop_turret::Part{};
}

// Every other Isaac (bots, partners' ghosts) and neutral people, for the clearance checks.
int Friends(uint32_t exclude, V3* out, int cap) {
    int n = 0;
    uint32_t node = 0;
    if (!Read(base + bot_sense::kReflectableListRva, &node, 4)) return 0;
    for (unsigned steps = 0; node && steps < 8192 && n < cap; ++steps) {
        const uint32_t entity = node - bot_sense::kEntityReflectable;
        uint32_t vtable = 0, next = 0;
        if (entity != exclude && Read(entity, &vtable, 4) && vtable == base + placement::kPlayerPrimaryVtableRva) {
            V3 at;
            if (Pos(entity, at)) out[n++] = at;
        }
        if (!Read(node + 4, &next, 4) || next == node) break;
        node = next;
    }
    return n;
}

void Refuse(coop_turret::Place why, const char* detail) {
    InterlockedIncrement(&refused[static_cast<int>(why)]);
    Line("not placed: %s (%s)", coop_turret::PlaceName(why), detail);
    switch (why) {
    case coop_turret::Place::Blocked: Notice("TURRET: NO CLEAR LINE TO THE SPOT"); break;
    case coop_turret::Place::Occupied: Notice("TURRET: SOMEONE STANDS THERE"); break;
    case coop_turret::Place::TooHigh: Notice("TURRET: FLOOR TOO HIGH OR LOW"); break;
    default: Notice("TURRET: NO ROOM HERE"); break;
    }
}

void TryPlace(uint32_t player) {
    V3 feet, forward, candidate;
    if (!Pos(player, feet) || !Facing(player, forward) || !coop_turret::Candidate(feet, forward, cfg.placeDistance, candidate)) {
        Refuse(coop_turret::Place::NoPlayer, "no position or facing");
        return;
    }
    coop_turret::PlaceInput in;
    in.feet = feet;
    in.candidate = candidate;
    in.meshKnown = bot_nav::ready && bot_nav::LevelSpace() != 0;
    if (in.meshKnown) {
        coop_bot::Vec3 end;
        int count = 0;
        in.meshHit = bot_nav::PathEnd(ToVec3(feet), ToVec3(candidate), end, count);
        in.meshEnd = ToV3(end);
    } else {
        // No mesh: the floor under the candidate, cast from a metre above it.
        float fraction = 1.0f;
        uint32_t hit = 0;
        V3 point;
        const V3 from = {candidate.x, feet.y + 1.0f, candidate.z}, to = {candidate.x, feet.y - 1.5f, candidate.z};
        if (Cast(from, to, player, fraction, hit, &point) && fraction < 0.999f) {
            in.floorHit = true;
            in.floorY = from.y + (to.y - from.y) * fraction;   // the cast's output vector is the hit normal, not the point
        }
    }
    const coop_turret::PlaceResult spot = coop_turret::SpotFor(in, cfg);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "feet %.2f %.2f %.2f, candidate %.2f %.2f %.2f, mesh %d/%d end %.2f %.2f %.2f, floor %d %.2f", feet.x, feet.y, feet.z,
                  candidate.x, candidate.y, candidate.z, in.meshKnown ? 1 : 0, in.meshHit ? 1 : 0, in.meshEnd.x, in.meshEnd.y, in.meshEnd.z, in.floorHit ? 1 : 0, in.floorY);
    if (spot.verdict != coop_turret::Place::Ok) { Refuse(spot.verdict, detail); return; }
    // The player must see the spot, and the muzzle must be in the open: one cast each.
    const V3 eye = {feet.x, feet.y + 1.5f, feet.z};
    const V3 muzzle = {spot.spot.x, spot.spot.y + cfg.muzzleHeight, spot.spot.z};
    const bool line = losReady && Clear(eye, {spot.spot.x, spot.spot.y + 0.4f, spot.spot.z}, player, 0) && Clear(eye, muzzle, player, 0);
    V3 others[coop_bot::kMaxPlayers];
    const int n = Friends(player, others, coop_bot::kMaxPlayers);
    float nearest = 1000.0f;
    for (int i = 0; i < n; ++i) {
        const float d = coop_turret::Flat(others[i], spot.spot);
        if (d < nearest && std::fabs(others[i].y - spot.spot.y) < 2.0f) nearest = d;
    }
    const coop_turret::Place verdict = coop_turret::Finish(spot.verdict, line, nearest, cfg);
    if (verdict != coop_turret::Place::Ok) { Refuse(verdict, losReady ? detail : "no camera cast: the spot cannot be checked"); return; }
    t = Placed{};
    t.on = true;
    t.spot = spot.spot;
    t.muzzle = muzzle;
    t.yaw = coop_turret::YawTo(feet, spot.spot);
    t.owner = player;
    t.placedAt = GetTickCount();
    if (bodyWanted && bodyReady) t.body = SpawnBody(spot.spot);
    pacer.Reset();
    ClearTarget();
    scanAt = 0;
    InterlockedIncrement(&placedCount);
    Line("placed at %.2f %.2f %.2f (%s), body %08X, rounds %d, nearest friend %.1f m", t.spot.x, t.spot.y, t.spot.z, detail, t.body, mag.Rounds(cfg), nearest);
    char text[64];
    if (mag.Rounds(cfg) >= 0) std::snprintf(text, sizeof(text), "TURRET PLACED (%d ROUNDS)", mag.Rounds(cfg));
    else std::snprintf(text, sizeof(text), "TURRET PLACED");
    Notice(text);
    LogFxResidency();
}

void TakeBack(uint32_t player) {
    V3 feet;
    const float d = Pos(player, feet) ? coop_turret::Distance(feet, t.spot) : 1000.0f;
    if (cfg.pickupRange > 0.0f && d > cfg.pickupRange) {
        InterlockedIncrement(&tooFar);
        Line("not picked up: %.1f m away (PickupRange %.1f)", d, cfg.pickupRange);
        char text[64];
        std::snprintf(text, sizeof(text), "TURRET: %.0f M AWAY, WALK CLOSER", d);
        Notice(text);
        return;
    }
    ReleaseAllFx("pick up");
    RemoveBody("pick up");
    t.on = false;
    ClearTarget();
    InterlockedIncrement(&pickedUp);
    Line("picked up %.1f m away after %lu s, rounds %d", d, static_cast<unsigned long>((GetTickCount() - t.placedAt) / 1000), mag.Rounds(cfg));
    Notice("TURRET PICKED UP");
}

// The world it stood in is gone: let go without touching anything of it. Effect
// handles are the exception: the level's kill-all has zeroed them already, and one
// that is still ours (its effect points back at it) is soft-killed so no hologram
// outlives the turret.
void Forget(const char* why) {
    ReleaseAllFx(why);
    if (!t.on) return;
    InterlockedIncrement(&forgotten);
    Line("removed with %s (body %08X left to the engine)", why, t.body);
    t = Placed{};
    ClearTarget();
    for (Pending& p : pending) p = Pending{};
}

// --- shooting ------------------------------------------------------------------
// Fresh targets from the bot senses: the fighting necromorphs near the turret or
// the player, the few nearest looked at closely.
bot_sense::Snapshot snap;
void Choose(uint32_t player) {
    InterlockedIncrement(&scans);
    V3 feet;
    if (!Pos(player, feet)) return;
    bot_sense::Scan(snap, 0, ToVec3(t.muzzle), ToVec3(feet));
    struct Option { int index; float distance; };
    Option options[coop_bot::kMaxEnemies];
    int n = 0;
    for (int i = 0; i < snap.enemyCount && i < coop_bot::kMaxEnemies; ++i) {
        const coop_bot::Enemy& e = snap.enemies[i];
        const float d = coop_turret::Distance(t.muzzle, ToV3(e.pos));
        if (e.objective || e.regenerating || !(e.health > 0.0f) || d > cfg.range + 2.0f) continue;
        options[n++] = Option{i, d};
    }
    for (int a = 1; a < n; ++a)
        for (int b = a; b > 0 && options[b].distance < options[b - 1].distance; --b) std::swap(options[b], options[b - 1]);
    constexpr int kLooked = 4;
    coop_turret::Target targets[kLooked];
    coop_turret::Part best[kLooked];
    int m = 0;
    for (int k = 0; k < n && m < kLooked; ++k) {
        const coop_bot::Enemy& e = snap.enemies[options[k].index];
        const uint32_t entity = snap.enemyEntity[options[k].index];
        coop_turret::Part parts[kMaxJoints];
        int count = Joints(entity, ToV3(e.pos), parts, kMaxJoints);
        if (!count) {   // no ragdoll: the middle of the body, no bone
            const float h = bot_sense::KindAimHeight(e.kind);
            parts[0] = coop_turret::Part{{e.pos.x, e.pos.y + (h < 1.2f ? h : 1.0f), e.pos.z}, 10, -1, false};
            count = 1;
        }
        SightParts(entity, parts, count, 4);
        const int pick = coop_turret::ChoosePart(parts, count, t.muzzle);
        coop_turret::Target& tg = targets[m];
        tg = coop_turret::Target{};
        tg.id = entity;
        tg.distance = pick >= 0 ? coop_turret::Distance(t.muzzle, parts[pick].at) : options[k].distance;
        tg.health = e.health;
        tg.visible = pick >= 0;
        tg.onFriend = e.victim && (IsPlayerOrNecroEntity(e.victim) || IsBotEntity(e.victim));
        tg.regenerating = e.regenerating;
        tg.objective = e.objective;
        best[m] = pick >= 0 ? parts[pick] : coop_turret::Part{};
        ++m;
    }
    InterlockedExchangeAdd(&candidates, m);
    const int chosen = coop_turret::ChooseTarget(targets, m, target, cfg);
    if (chosen < 0) {
        if (target) InterlockedIncrement(&refreshLost);
        ClearTarget();
        InterlockedIncrement(&noTarget);
        return;
    }
    if (targets[chosen].id != target) {
        InterlockedIncrement(target ? &targetSwitches : &targetsChosen);
        uint32_t vtable = 0;
        Read(targets[chosen].id, &vtable, 4);
        if (InterlockedCompareExchange(&targetsChosen, 0, 0) + InterlockedCompareExchange(&targetSwitches, 0, 0) <= 40)
            gate::Event("turret: target %08X (vtable RVA %08X) %.1f m, health %.0f, zone %d bone %d%s", targets[chosen].id, vtable - static_cast<uint32_t>(base),
                        targets[chosen].distance, targets[chosen].health, best[chosen].zone, best[chosen].bone, targets[chosen].onFriend ? ", it goes for a player" : "");
        target = targets[chosen].id;
        targetVtable = vtable;
    }
    aim = best[chosen];
}
// Between scans: the target still there, its parts where they are now.
bool Refresh() {
    uint32_t vtable = 0;
    unsigned char hidden = 0;
    V3 at;
    if (!target || !Read(target, &vtable, 4) || vtable != targetVtable || !Pos(target, at) || (Read(target + kHiddenCount, &hidden, 1) && hidden) ||
        !(Health(target) > 0.0f) || coop_turret::Distance(t.muzzle, at) > cfg.range + 2.0f) {
        InterlockedIncrement(&refreshLost);
        ClearTarget();
        return false;
    }
    coop_turret::Part parts[kMaxJoints];
    const int count = Joints(target, at, parts, kMaxJoints);
    if (!count) {
        if (aim.bone >= 0) { ClearTarget(); return false; }
        const float h = coop_turret::Distance(aim.at, at) < 3.0f ? aim.at.y - at.y : 1.0f;
        aim.at = {at.x, at.y + h, at.z};
        return ClearPart(aim, t.body, target);
    }
    // The same zone first (one ray), else the best part in sight.
    for (int i = 0; i < count; ++i)
        if (parts[i].zone == aim.zone && parts[i].bone == aim.bone) {
            if (ClearPart(parts[i], t.body, target)) { aim = parts[i]; return true; }
            InterlockedIncrement(&blockedRays);
            break;
        }
    SightParts(target, parts, count, 3);
    const int pick = coop_turret::ChoosePart(parts, count, t.muzzle);
    if (pick < 0) { ClearTarget(); return false; }
    aim = parts[pick];
    return true;
}

void Fire(uint32_t now, uint32_t dt) {
    if (!target) return;
    const float bearing = coop_turret::YawTo(t.muzzle, aim.at);
    t.yaw = coop_turret::Turn(t.yaw, bearing, dt, cfg);
    if (!coop_turret::Aligned(t.yaw, bearing, cfg.aimToleranceDeg * coop_turret::kPi / 180.0f)) { InterlockedIncrement(&turning); return; }
    if (!pacer.Ready(now)) return;
    if (!mag.CanFire(cfg)) return;
    if (!Refresh()) return;
    // Who could stand in the line: the last scan's Isaacs and neutral people, the
    // player and the bots where they are now.
    V3 others[coop_bot::kMaxPlayers + 1 + kMaxBots];
    int n = 0;
    for (int i = 0; i < snap.playerCount && i < coop_bot::kMaxPlayers; ++i) others[n++] = ToV3(snap.players[i]);
    if (Pos(t.owner, others[n])) ++n;
    for (uint32_t bot : botEntities)
        if (bot && Pos(bot, others[n])) ++n;
    if (coop_turret::FriendInLine(t.muzzle, aim.at, others, n, cfg)) {
        InterlockedIncrement(&friendHolds);
        pacer.Fired(now, cfg.fireIntervalMs);   // wait a beat, do not cast every frame
        return;
    }
    InterlockedIncrement(&shots);
    if (!Hit(target, aim)) { pacer.Fired(now, cfg.fireIntervalMs); return; }
    mag.Fire(cfg);
    pacer.Fired(now, cfg.fireIntervalMs);
    ShotEffects(aim, target);
}

void Feedback(DWORD wall) {
    if (mag.overheated && !wasOverheated) {
        InterlockedIncrement(&overheats);
        Line("overheated after shot %ld", shots);
        TroubleCue("overheat");
        if (wall - overheatNoticeAt > 5000) { overheatNoticeAt = wall; Notice("TURRET OVERHEATED"); }
    }
    wasOverheated = mag.overheated;
    const bool empty = !mag.Unlimited(cfg) && mag.rounds < 1.0f;
    if (empty && !wasEmpty) {
        InterlockedIncrement(&empties);
        Line("out of ammo after shot %ld", shots);
        TroubleCue("empty");
        if (wall - emptyNoticeAt > 5000) { emptyNoticeAt = wall; Notice("TURRET OUT OF AMMO"); }
    }
    wasEmpty = empty;
}

void TickInner() {
    const DWORD wall = GetTickCount();
    // The world it stood in: a new level start, a restart, the menu, another body.
    const LONG starts = InterlockedCompareExchange(&loadsync::levelStarts, 0, 0);
    const LONG clears = InterlockedCompareExchange(&gate::restartClears, 0, 0);
    if (starts != startsSeen) {
        Forget("a level start");
        mag.Refill(cfg);
        losReady = bot_sense::losVerified;   // a fault switch-off lasts one level
        for (DWORD& when : castFaultTimes) when = 0;
        startsSeen = starts;
    }
    if (clears != clearsSeen) { Forget("a restart or level exit"); clearsSeen = clears; }
    const uint32_t player = inLevel ? LocalCampaignPlayer() : 0;
    if (t.on) {
        if (!inLevel) Forget("the level going away");
        else if (FrontendNow()) Forget("the main menu");
        else if (player && player != t.owner) Forget("a new player body");
    }

    // The clock: game time, so a pause stops the turret and its cooling with it.
    const uint32_t now = SimMs();
    const uint32_t dt = simKnown && now - lastSim < 60000 ? now - lastSim : 0;
    const bool paused = simKnown && now == lastSim;
    lastSim = now;
    simKnown = now != 0;
    mag.Advance(dt, cfg);

    const bool down = key && necro::GameWindowFocused() && !shop::HoldsKey(key, coop_shop::Guard::Turret) && (GetAsyncKeyState(key) & 0x8000) != 0;
    if (keyEdge.Pressed(down)) {
        InterlockedIncrement(&presses);
        if (!t.on && !progress::GadgetAllowed("Turret", "TURRET")) return;   // [Unlocks]
        if (t.on) {
            if (player) TakeBack(player);
            else Forget("a key press without a player");
        } else if (!player || !WorldReady() || CutsceneNow()) {
            InterlockedIncrement(&refusedState);
            Line("not placed: %s", !player ? "no player (menu, loading or a necromorph body)" : CutsceneNow() ? "a cutscene runs" : "the level is not ready");
            Notice("TURRET: NOT NOW");
        } else {
            TryPlace(player);
        }
    }

    active = false;
    if (fxSigsOk) SweepFx();
    if (!t.on) {
        if (armedHandle || armedShown) ArmedCue(wall, false, player);
        return;
    }
    if (t.body && wall - bodyCheckAt > 1000) {
        bodyCheckAt = wall;
        uint32_t flags = 0;
        if (!BodyIsOurs()) { InterlockedIncrement(&bodyGone); Line("body %08X is gone (the engine took it)", t.body); t.body = 0; }
        else if (Read(t.body + kItemFlags, &flags, 4) && (flags & 1)) {
            // Something made it pickable again: off once more, and say so.
            InterlockedIncrement(&bodyRearmed);
            const bool off = MakeUnpickable(t.body);
            Line("body %08X was pickable again (flags %08X): %s", t.body, flags, off ? "switched off" : "switch-off failed, body removed");
            if (!off) RemoveBody("a pickable body");
        }
    }
    FollowPending();
    // The hologram goes out when it overheats or runs dry and comes back once it is
    // cool with a few rounds, so a slow refill does not blink it every second.
    if (mag.overheated || (!mag.Unlimited(cfg) && mag.rounds < 1.0f)) cueHold = true;
    else if (cueHold && (mag.Unlimited(cfg) || mag.rounds >= std::min(10.0f, static_cast<float>(cfg.ammo)))) cueHold = false;
    ArmedCue(wall, WorldReady() && !CutsceneNow() && damageReady && losReady && !cueHold, player);
    if (paused) { InterlockedIncrement(&pausedFrames); return; }
    if (!WorldReady()) return;
    if (CutsceneNow()) { InterlockedIncrement(&cutsceneFrames); ClearTarget(); pacer.Reset(); return; }
    if (!damageReady || !losReady) return;
    active = true;
    // A scan every 250 ms without a target, 400 ms with one, and once at once when it is lost.
    if (rescan || static_cast<int32_t>(now - scanAt) >= 0) {
        rescan = false;
        Choose(t.owner);
        scanAt = now + (target ? 400u : 250u);
    }
    Fire(now, dt);
    Feedback(wall);
}

int TickFilter(unsigned code) {
    InterlockedIncrement(&tickFaults);
    if (InterlockedIncrement(&faultLines) <= kFaultLines) gate::Event("turret FAULT tick code %08X", code);
    return EXCEPTION_EXECUTE_HANDLER;
}
void Tick() {
    if (!installed) return;
    __try {
        TickInner();
    } __except (TickFilter(GetExceptionCode())) {
        if (InterlockedCompareExchange(&tickFaults, 0, 0) >= 5) {
            installed = false;
            gate::Event("turret: five faulted ticks, switched off for the session");
        }
    }
}

// --- setup -------------------------------------------------------------------
bool Bytes(uint32_t rva, const unsigned char* bytes, size_t size) {
    unsigned char actual[40]{};
    return size <= sizeof(actual) && Read(base + rva, actual, size) && std::memcmp(actual, bytes, size) == 0;
}
float IniFloat(const std::string& ini, const char* name, float fallback) {
    char text[32]{}, def[32]{};
    std::snprintf(def, sizeof(def), "%g", fallback);
    GetPrivateProfileStringA("Turret", name, def, text, sizeof(text), ini.c_str());
    const float v = static_cast<float>(std::atof(text));
    return std::isfinite(v) ? v : fallback;
}
// "4BC78C36-9D67757C-544E4543-53574F4B": four dwords as the log prints GUIDs.
bool ParseGuid(const char* text, uint32_t out[4]) {
    uint32_t v[4]{};
    const char* p = text;
    for (int i = 0; i < 4; ++i) {
        char* end = nullptr;
        const unsigned long part = std::strtoul(p, &end, 16);
        if (end == p || end - p > 8 || (i < 3 ? *end != '-' : (*end != 0 && *end != ' '))) return false;
        v[i] = static_cast<uint32_t>(part);
        p = end + 1;
    }
    for (int i = 0; i < 4; ++i) out[i] = v[i];
    return true;
}

// Create_ByGuid, the resident lookup and SoftKill: what every effect needs. The
// gadgets' radius rings check the same bytes through this.
bool FxCoreOk(bool& createOk, bool& findOk, bool& softOk) {
    // mov eax,[esp+8] / cmp dword [eax],0 / jnz ... / mov ecx,[VFX manager]
    static const unsigned char kCreate[] = {0x8B, 0x44, 0x24, 0x08, 0x83, 0x38, 0x00, 0x75, 0x12, 0x83, 0x78, 0x04, 0x00, 0x75, 0x0C, 0x83,
                                            0x78, 0x08, 0x00, 0x75, 0x06, 0x83, 0x78, 0x0C, 0x00, 0x74, 0x16, 0x8B, 0x0D, 0x70, 0x2C, 0x03, 0x02};
    // push esi / push edi / mov edi,[esp+0Ch] / push 10h / push edi / mov esi,ecx
    static const unsigned char kFind[] = {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x6A, 0x10, 0x57, 0x8B, 0xF1};
    // mov ecx,[ecx] / test ecx,ecx / jz / jmp instance soft kill / ret
    static const unsigned char kSoftKill[] = {0x8B, 0x09, 0x85, 0xC9, 0x74, 0x05, 0xE9, 0x85, 0xA0, 0x03, 0x00, 0xC3};
    createOk = Bytes(kCreateByGuidRva, kCreate, sizeof(kCreate));
    findOk = Bytes(kVfxFindRva, kFind, sizeof(kFind));
    softOk = Bytes(kSoftKillRva, kSoftKill, sizeof(kSoftKill));
    return createOk && findOk && softOk;
}

// The effect functions, each checked against the bytes read in the live game and
// in the full dumps (identical in both). Any mismatch switches every effect off.
void InstallFx(FILE* log, const std::string& ini) {
    fxMuzzle.wanted = GetPrivateProfileIntA("Turret", "MuzzleFlash", 1, ini.c_str()) != 0;
    fxTracer.wanted = GetPrivateProfileIntA("Turret", "Tracer", 1, ini.c_str()) != 0;
    fxImpact.wanted = GetPrivateProfileIntA("Turret", "ImpactFx", 1, ini.c_str()) != 0;
    fxSound.wanted = GetPrivateProfileIntA("Turret", "FireSound", 1, ini.c_str()) != 0;
    fxArmed.wanted = GetPrivateProfileIntA("Turret", "ArmedCue", 1, ini.c_str()) != 0;
    fxPerSecond = GetPrivateProfileIntA("Turret", "EffectsPerSecond", 8, ini.c_str());
    if (fxPerSecond < 1) fxPerSecond = 1;
    if (fxPerSecond > 20) fxPerSecond = 20;
    char guidText[64]{};
    GetPrivateProfileStringA("Turret", "TracerVfxGuid", "4052B175-4052B175-4052B175-240F5183", guidText, sizeof(guidText), ini.c_str());
    if (!ParseGuid(guidText, tracerGuid)) std::fprintf(log, "turret TracerVfxGuid '%s' unreadable; the Seeker Rifle tracer stays\n", guidText);

    // aligned movaps copy of four rows to instance+0x30, ret 4
    static const unsigned char kSetMatrix[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x01, 0x85, 0xC0, 0x74, 0x24, 0x8B, 0x4D, 0x08, 0x0F, 0x28, 0x01, 0x83, 0xC0, 0x30};
    static const unsigned char kImpact[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x45, 0x14, 0x81, 0xEC, 0x04, 0x01, 0x00, 0x00, 0x53, 0x56, 0x8B, 0x75, 0x10, 0x57, 0x85, 0xC0, 0x74, 0x10};
    static const unsigned char kImpactTable[] = {0x8B, 0x0D, 0xB8, 0x13, 0x02, 0x02};   // mov ecx,[ImpactSurfaceMatrix] at +0x6E
    static const unsigned char kImpactRet[] = {0x5F, 0x5E, 0x5B, 0x8B, 0xE5, 0x5D, 0xC3};   // plain ret at +0x7B1: cdecl
    static const unsigned char kTracer[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x1C, 0x53, 0x8D, 0x81, 0xB8, 0x00, 0x00, 0x00, 0x33, 0xDB};
    static const unsigned char kTracerVtable[] = {0xC7, 0x00, 0x34, 0xFE, 0xB3, 0x01};   // VFXContextTracer vtable store at +0xA5
    static const unsigned char kTracerRet[] = {0x5B, 0x8B, 0xE5, 0x5D, 0xC2, 0x08, 0x00};   // ret 8 at +0x134: thiscall(start, end)
    bool createOk = false, findOk = false, softOk = false;
    FxCoreOk(createOk, findOk, softOk);
    const bool matrixOk = Bytes(kSetMatrixRva, kSetMatrix, sizeof(kSetMatrix));
    const bool impactOk = Bytes(kImpactFxRva, kImpact, sizeof(kImpact)) && Bytes(kImpactFxRva + 0x6E, kImpactTable, sizeof(kImpactTable)) &&
                          Bytes(kImpactFxRva + 0x7B1, kImpactRet, sizeof(kImpactRet));
    const bool tracerOk = Bytes(kCreateTracerRva, kTracer, sizeof(kTracer)) && Bytes(kCreateTracerRva + 0xA5, kTracerVtable, sizeof(kTracerVtable)) &&
                          Bytes(kCreateTracerRva + 0x134, kTracerRet, sizeof(kTracerRet));
    fxSigsOk = createOk && findOk && softOk;
    fxMuzzle.ready = fxSigsOk;
    fxSound.ready = fxSigsOk;
    fxTracer.ready = fxSigsOk && tracerOk;
    fxImpact.ready = fxSigsOk && impactOk;
    fxArmed.ready = fxSigsOk && matrixOk;
    std::fprintf(log, "turret fx muzzle=%d/%d sound=%d/%d tracer=%d/%d impact=%d/%d armed=%d/%d (wanted/ready) per_second=%d | create=%d find=%d soft_kill=%d "
                      "set_matrix=%d impact_fn=%d tracer_fn=%d | muzzle %08X-%08X silent %08X-%08X tracer %08X-%08X armed %08X-%08X trouble %08X-%08X "
                      "impact ammo %08X\n",
                 fxMuzzle.wanted, fxMuzzle.ready, fxSound.wanted, fxSound.ready, fxTracer.wanted, fxTracer.ready, fxImpact.wanted, fxImpact.ready, fxArmed.wanted,
                 fxArmed.ready, fxPerSecond, createOk, findOk, softOk, matrixOk, impactOk, tracerOk, kPulseMuzzleGuid[0], kPulseMuzzleGuid[3], kSilentFlashGuid[0],
                 kSilentFlashGuid[3], tracerGuid[0], tracerGuid[3], kArmedGuid[0], kArmedGuid[3], kTroubleGuid[0], kTroubleGuid[3], kPulseAmmoType);
    if (fxSound.wanted && !fxMuzzle.wanted)
        std::fprintf(log, "turret fx: FireSound=1 with MuzzleFlash=0 plays nothing; the pulse rifle's shot sound is part of its muzzle effect\n");
}

bool Install(FILE* log, const std::string& ini) {
    wanted = GetPrivateProfileIntA("Turret", "Enabled", 1, ini.c_str()) != 0;
    if (!wanted) { std::fprintf(log, "turret off ([Turret] Enabled=0)\n"); return false; }
    key = GetPrivateProfileIntA("Turret", "Key", 102, ini.c_str());
    bodyWanted = GetPrivateProfileIntA("Turret", "Body", 1, ini.c_str()) != 0;
    char guidText[64]{};
    GetPrivateProfileStringA("Turret", "BodyGuid", "4BC78C36-9D67757C-544E4543-53574F4B", guidText, sizeof(guidText), ini.c_str());
    if (!ParseGuid(guidText, bodyGuid)) std::fprintf(log, "turret BodyGuid '%s' unreadable; the Pulse Rifle ammo box stays\n", guidText);
    coop_turret::Config c;
    c.range = IniFloat(ini, "Range", c.range);
    c.fireIntervalMs = static_cast<uint32_t>(GetPrivateProfileIntA("Turret", "FireRateMs", static_cast<int>(c.fireIntervalMs), ini.c_str()));
    c.damage = IniFloat(ini, "DamagePerShot", c.damage);
    c.ammo = GetPrivateProfileIntA("Turret", "Ammo", c.ammo, ini.c_str());
    c.ammoRegenPerSec = IniFloat(ini, "AmmoRegenPerSecond", c.ammoRegenPerSec);
    c.heatSeconds = IniFloat(ini, "HeatSeconds", c.heatSeconds);
    c.coolSeconds = IniFloat(ini, "CoolSeconds", c.coolSeconds);
    c.turnDegPerSec = IniFloat(ini, "TurnDegreesPerSecond", c.turnDegPerSec);
    c.placeDistance = IniFloat(ini, "PlaceDistance", c.placeDistance);
    c.pickupRange = IniFloat(ini, "PickupRange", c.pickupRange);
    cfg = coop_turret::Sanitized(c);
    mag.Refill(cfg);

    // The damage path: ctor (with its vtable store), impact direction, the sender (with its iMsgDamage push) and the destructor.
    static const unsigned char kCtor[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x08, 0x53, 0x56, 0x8B, 0xF1, 0xE8};
    static const unsigned char kCtorVtable[] = {0xC7, 0x06, 0x4C, 0xC7, 0xB5, 0x01};
    static const unsigned char kDir[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x10, 0x8B, 0x45, 0x08, 0x8B, 0x55, 0x0C, 0x0F, 0x28, 0x00};
    static const unsigned char kSend[] = {0x51, 0x56, 0x8B, 0x35, 0x24, 0x0E, 0x05, 0x02, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x85, 0xFF, 0x74, 0x7D};
    static const unsigned char kSendPush[] = {0x68, 0x68, 0xEE, 0x09, 0x02};
    static const unsigned char kDtor[] = {0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0x80, 0x00, 0x00, 0x00, 0x8D, 0x86, 0x80, 0x00, 0x00, 0x00, 0x85, 0xC9};
    const bool ctorOk = Bytes(kDamageCtorRva, kCtor, sizeof(kCtor)) && Bytes(kDamageCtorRva + kDamageVtableStore, kCtorVtable, sizeof(kCtorVtable));
    const bool dirOk = Bytes(kImpactDirRva, kDir, sizeof(kDir));
    const bool sendOk = Bytes(kSendDelayedRva, kSend, sizeof(kSend)) && Bytes(kSendDelayedRva + kSendMsgPush, kSendPush, sizeof(kSendPush));
    const bool dtorOk = Bytes(kDamageDtorRva, kDtor, sizeof(kDtor));
    damageReady = ctorOk && dirOk && sendOk && dtorOk;

    // The body: spawn, Item::Drop (and the slot every item uses for it), the registration update, Base::Delete.
    static const unsigned char kSpawn[] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x28, 0x53, 0x8B, 0x1D, 0xA0, 0xC0, 0x01, 0x02};
    static const unsigned char kDrop[] = {0x51, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x40, 0x02, 0x00, 0x00, 0x57, 0x85, 0xC0};
    static const unsigned char kRegistration[] = {0x83, 0xEC, 0x28, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x40, 0x02, 0x00, 0x00, 0xBB, 0x01, 0x00, 0x00, 0x00};
    static const unsigned char kDelete[] = {0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x06, 0x8B, 0x50, 0x08, 0x8B, 0xCE, 0xFF, 0xD2, 0x83, 0x3D};
    unsigned char deleteHead[16]{};
    // Another plugin's 5-byte detour at Base::Delete's entry (seen live) still leads to the same function.
    const bool deleteOk = Read(base + kBaseDeleteRva, deleteHead, sizeof(deleteHead)) &&
                          (std::memcmp(deleteHead, kDelete, sizeof(kDelete)) == 0 || (deleteHead[0] == 0xE9 && std::memcmp(deleteHead + 5, kDelete + 5, sizeof(kDelete) - 5) == 0));
    const bool spawnOk = Bytes(kGlobalPickupRva, kSpawn, sizeof(kSpawn)), dropOk = Bytes(kItemDropRva, kDrop, sizeof(kDrop));
    const bool registrationOk = Bytes(kUpdateRegistrationRva, kRegistration, sizeof(kRegistration));
    bodyReady = spawnOk && dropOk && registrationOk && deleteOk;

    // The cast and the mesh are the bots' (their Install only checks signatures; safe to ask twice).
    if (!bot_sense::losVerified) bot_sense::InstallLineOfSight(log);
    losReady = bot_sense::losVerified;
    if (!bot_nav::ready) bot_nav::Install(log);
    startsSeen = InterlockedCompareExchange(&loadsync::levelStarts, 0, 0);
    clearsSeen = InterlockedCompareExchange(&gate::restartClears, 0, 0);
    installed = damageReady;
    std::fprintf(log, "turret %s key=%d range=%.1f fire_ms=%u damage=%.1f ammo=%d regen=%.2f heat_s=%.1f cool_s=%.1f turn_dps=%.0f place=%.2f pickup_range=%.1f "
                      "| damage ctor=%d dir=%d send=%d dtor=%d | body wanted=%d ready=%d (spawn=%d drop=%d registration=%d delete=%d%s) guid=%08X-%08X-%08X-%08X "
                      "| cast=%d nav=%d notice=%d\n",
                 installed ? "installed" : "OFF (damage path signatures do not match)", key, cfg.range, cfg.fireIntervalMs, cfg.damage, cfg.ammo,
                 cfg.ammoRegenPerSec, cfg.heatSeconds, cfg.coolSeconds, cfg.turnDegPerSec, cfg.placeDistance, cfg.pickupRange, ctorOk, dirOk, sendOk, dtorOk,
                 bodyWanted, bodyReady, spawnOk, dropOk, registrationOk, deleteOk, deleteHead[0] == 0xE9 ? ", detoured by another plugin" : "", bodyGuid[0],
                 bodyGuid[1], bodyGuid[2], bodyGuid[3], losReady, bot_nav::ready, notice::enabled && notice::installed);
    if (installed && !losReady) std::fprintf(log, "turret: without the camera cast it can be placed nowhere and fires at nothing\n");
    InstallFx(log, ini);
    return installed;
}

void WriteDiagnostics(FILE* log) {
    if (!wanted) return;
    static LONG last = -1;
    const LONG sum = presses + placedCount + pickedUp + forgotten + scans + shots + sent + applied + noEffect + kills + damageFaults + bodyFaults + tickFaults +
                     castFaults + refusedState + tooFar + friendHolds + overheats + empties + bodyGone + bodyRearmed + FxSum();
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "turret on=%d active=%d presses=%ld placed=%ld picked_up=%ld too_far=%ld forgotten=%ld refused[no_player=%ld no_floor=%ld too_high=%ld wall=%ld "
                      "blocked=%ld occupied=%ld state=%ld] | scans=%ld looked_at=%ld chosen=%ld switches=%ld lost=%ld none=%ld | shots=%ld sent=%ld applied=%ld "
                      "no_effect=%ld kills=%ld blocked_rays=%ld friend_holds=%ld turning=%ld | rounds=%d heat=%.2f overheats=%ld empties=%ld paused=%ld cutscene=%ld "
                      "| body spawned=%ld removed=%ld gone=%ld rearmed=%ld faults=%ld | casts=%ld cast_faults=%ld damage_faults=%ld tick_faults=%ld\n",
                 t.on ? 1 : 0, active ? 1 : 0, presses, placedCount, pickedUp, tooFar, forgotten, refused[1], refused[2], refused[3], refused[4], refused[5],
                 refused[6], refusedState, scans, candidates, targetsChosen, targetSwitches, refreshLost, noTarget, shots, sent, applied, noEffect, kills,
                 blockedRays, friendHolds, turning, mag.Rounds(cfg), mag.heat, overheats, empties, pausedFrames, cutsceneFrames, bodySpawned, bodyRemoved,
                 bodyGone, bodyRearmed, bodyFaults, castCalls, castFaults, damageFaults, tickFaults);
    // Per piece: played, not loaded, over the rate, refused, created nothing, faults.
    char pieces[400];
    int used = 0;
    pieces[0] = 0;
    for (const Piece* p : {&fxMuzzle, &fxSound, &fxTracer, &fxImpact, &fxArmed}) {
        if (used < 0 || used >= static_cast<int>(sizeof(pieces))) break;
        used += std::snprintf(pieces + used, sizeof(pieces) - used, " %s=%s%ld/%ld/%ld/%ld/%ld/%ld", p->name, p->wanted && p->ready ? "" : "off:", p->played, p->absent,
                              p->rated, p->refused, p->empty, p->faults);
    }
    std::fprintf(log, "turret fx (played/absent/rated/refused/empty/faults)%s | trouble_cues=%ld hologram=%d released=%ld expired=%ld stale=%ld lookup_faults=%ld\n",
                 pieces, troubleCues, armedHandle ? 1 : 0, fxReleased, fxExpired, fxStale, fxLookupFaults);
}
}  // namespace turret
