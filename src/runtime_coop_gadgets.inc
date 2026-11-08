// Dead Space 2 United by Richard Hafer.
// Two quick gadgets for the local player ([Gadgets], arithmetic in coop_gadgets.h,
// evidence in analysis/DS2_GADGETS_2026-10-09.md).
//
// Stasis mine: one key puts it on the floor (the turret's placement rules), the
// same key near it takes it back. Armed, it waits for a live necromorph within
// TriggerMeters and then slows every enemy within RadiusMeters once, through
// the player's own stasis hit: FireStasisFunction::Hit (PC 0x00889730,
// thiscall(fn, entity, collidable), ret 8) with the object PlayerFireStasisSM::
// ProcessAttackCollision (0x00889180) builds: vtable 0x01B27338, +4 the
// multiplayer bit of [0x0201C0A4]+0x1C, +8 the PlayerSettings stasis strength
// ([[0x02087C38]+0xC]+0x414, percent), +0xC the owner's stasis duration
// (Player+0xBC8), +0x10 the owner. Hit calls StasisManager::StasisEntity
// (0x00BB6D00, manager [0x02021348]), which honours the enemy's immunity and
// scales the duration by its resistance, then makes the enemy aware of the owner.
//
// Heal field: one key puts it down; for DurationSeconds every local Isaac (the
// player and the bots) inside regains RatePercent of max health per second.
// The med pack ends in HealthComponent::AddHealth, which is SetHealth (vtable
// +0x20, PC 0x00F84C60) with min(current + amount, max) on a living component;
// the field calls SetHealth the same way (hits::CallSetHealth checks the slot),
// and the RIG follows the ratio SetHealth writes at +0xEC.
//
// Bodies: global PowerUps (a stasis pack, a small med pack) spawned and made
// unpickable exactly like the turret's ammo box.
namespace gadgets {
using coop_gadgets::V3;

bool mineWanted = false, fieldWanted = false, installed = false;
bool stasisReady = false, healReady = false, bodyReady = false, castOn = false, bodyWanted = true;
int mineKey = 103, fieldKey = 104;   // Numpad 7, Numpad 8
coop_gadgets::MineConfig mineCfg;
coop_gadgets::FieldConfig fieldCfg;
uint32_t mineGuid[4] = {0x4BC806EA, 0x40D9D6F7, 0x544E4543, 0x53574F4B};    // stasis pack, global_assets
uint32_t fieldGuid[4] = {0x4BC78C36, 0x9D5B89BA, 0x544E4543, 0x53574F4B};   // small med pack (25), global_assets

// --- engine ------------------------------------------------------------------
constexpr uint32_t kStasisHitRva = 0x489730, kStasisFnVtableRva = 0x1727338, kStasisEntityRva = 0x7B6D00, kProcessCollisionRva = 0x489180;
constexpr uint32_t kStasisManagerRva = 0x1C21348, kPlayerSettingsRva = 0x1C87C38, kGameFlagsRva = 0x1C1C0A4;
constexpr uint32_t kSettingsAttributes = 0xC, kStasisStrength = 0x414, kPlayerStasisDuration = 0xBC8, kGameFlagsByte = 0x1C;
constexpr uint32_t kTimeMultiplier = 0x84, kStasisTypeRva = 0x1CA4072, kComponentLookupRva = 0x3CF080;
constexpr uint32_t kComponentParent = 0x1C, kStasisAttributes = 0xC, kStasisFlags = 0x14;
constexpr uint32_t kHealthFlags = 0xC4, kSetHealthRva = 0xB84C60;
constexpr uint32_t kPlayerVtableRva = 0x17294D0;

// --- state -------------------------------------------------------------------
struct Placed {
    bool on = false;
    V3 spot;
    uint32_t owner = 0;
    uint32_t body = 0, bodyVtable = 0, bodyGuid[4]{};
    DWORD placedAt = 0;
    DWORD bodyCheckAt = 0;
};
Placed mine, field;
coop_gadgets::Charge mineCharge, fieldCharge;
coop_gadgets::Countdown armLeft, fieldLeft;
coop_gadgets::Clock clock;
coop_gadgets::Edge mineEdge, fieldEdge;
uint32_t scanAt = 0, healAcc = 0;
LONG startsSeen = 0, clearsSeen = 0;
bool mineActive = false, fieldActive = false;

// A stasis hit on its way: did the enemy's time multiplier drop?
struct Pending { uint32_t entity = 0, vtable = 0; float before = 1.0f; uint32_t sentAt = 0; };
Pending pending[16];
unsigned pendingNext = 0;

volatile LONG minePresses = 0, minePlaced = 0, minePickedUp = 0, mineTooFar = 0, mineRecharging = 0, mineNotNow = 0, mineForgotten = 0, mineRefunds = 0;
volatile LONG mineRefused[7]{};   // by coop_turret::Place
volatile LONG mineTriggers = 0, mineStunned = 0, mineSlowed = 0, mineAlready = 0, mineNoEffect = 0, mineRejected = 0, mineScans = 0, mineReadyAgain = 0, noManager = 0;
volatile LONG fieldPresses = 0, fieldPlaced = 0, fieldRunning = 0, fieldRecharging = 0, fieldNotNow = 0, fieldEnded = 0, fieldForgotten = 0, fieldReadyAgain = 0;
volatile LONG fieldRefused[7]{};
volatile LONG healSteps = 0, healApplied = 0, healNoEffect = 0, healFull = 0, healDead = 0, healPartners = 0, healOutside = 0;
volatile LONG healedCenti = 0;   // health points healed, times 100
volatile LONG cutsceneFrames = 0, pausedFrames = 0;
volatile LONG stasisFaults = 0, healFaults = 0, bodyFaults = 0, bodySpawned = 0, bodyRemoved = 0, bodyGone = 0, bodyRearmed = 0, castCalls = 0, castFaults = 0, tickFaults = 0;
volatile LONG eventLines = 0, faultLines = 0, hitLines = 0, effectLines = 0;
constexpr LONG kEventLines = 80, kFaultLines = 20, kHitLines = 24, kEffectLines = 16;

void Line(const char* format, ...) {
    if (InterlockedIncrement(&eventLines) > kEventLines) return;
    char buffer[240];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    gate::Event("gadgets: %s", buffer);
}
void Fault(const char* what, uint32_t a, uint32_t b) {
    if (InterlockedIncrement(&faultLines) <= kFaultLines) gate::Event("gadgets FAULT %s (%08X %08X)", what, a, b);
}
bool Notice(const char* format, ...) {
    if (!notice::enabled) return false;
    char text[96];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return notice::Show(text);
}
V3 ToV3(const coop_turret::V3& v) { return {v.x, v.y, v.z}; }
V3 ToV3(const coop_bot::Vec3& v) { return {v.x, v.y, v.z}; }
coop_turret::V3 ToT(const V3& v) { return {v.x, v.y, v.z}; }
coop_bot::Vec3 ToB(const V3& v) { return {v.x, v.y, v.z}; }
bool Pos(uint32_t entity, V3& out) {
    coop_turret::V3 at;
    if (!turret::Pos(entity, at)) return false;
    out = ToV3(at);
    return true;
}

// --- the camera cast, as the turret calls it -----------------------------------
DWORD castFaultTimes[5]{};
unsigned castFaultNext = 0;
__declspec(noinline) bool CastRaw(const float* from, const float* to, uint32_t exclude, float& fraction, uint32_t& hit, float* at) {
    using CastFn = float(__cdecl*)(const float*, const float*, const void*, const uint32_t*, uint32_t, float*, uint32_t*);
    __try {
        fraction = reinterpret_cast<CastFn>(base + turret::kCastingTestRva)(from, to, reinterpret_cast<const void*>(static_cast<uintptr_t>(exclude)), nullptr, 0, at, &hit);
        return std::isfinite(fraction);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// False on a fault. Five faults within ten seconds switch the cast off until the next level.
bool Cast(const V3& a, const V3& b, uint32_t exclude, float& fraction, uint32_t& hit, V3* point) {
    if (!castOn) return false;
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
        castOn = false;
        Line("the camera cast faulted five times in ten seconds: no placing until the next level");
    }
    return false;
}
bool Clear(const V3& a, const V3& b, uint32_t exclude) {
    const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    if (dx * dx + dy * dy + dz * dz < 0.09f) return true;
    float fraction = 1.0f;
    uint32_t hit = 0;
    return Cast(a, b, exclude, fraction, hit, nullptr) && fraction >= 0.999f;
}

// --- bodies ------------------------------------------------------------------
bool BodyIsOurs(const Placed& p) {
    uint32_t vtable = 0, guid[4]{}, owner = 1;
    return p.body && Read(p.body, &vtable, 4) && vtable == p.bodyVtable && Read(p.body + turret::kEntityGuid, guid, 16) &&
           std::memcmp(guid, p.bodyGuid, 16) == 0 && Read(p.body + turret::kItemOwner, &owner, 4) && owner == 0;
}
bool IsBody(uint32_t entity) { return entity && ((mine.on && entity == mine.body) || (field.on && entity == field.body)); }
void SpawnBody(Placed& p, const uint32_t* guid, const char* what) {
    static const uint32_t kPickupHash = gate::NameHash("PickupItem");
    if (!bodyWanted || !bodyReady) return;
    const uint32_t item = turret::SpawnRaw(guid);
    uint32_t vtable = 0, slot = 0, owner = 1;
    if (!item) {
        InterlockedIncrement(&bodyFaults);
        Line("%s body: GetGlobalPickupItem gave nothing for %08X-%08X-%08X-%08X", what, guid[0], guid[1], guid[2], guid[3]);
        return;
    }
    if (!Read(item, &vtable, 4) || vtable < base + gate::kRdataLo || vtable >= base + gate::kRdataHi || !gate::IsA(item, vtable, kPickupHash) ||
        !Read(vtable + turret::kItemDropSlot, &slot, 4) || slot != base + turret::kItemDropRva || !Read(item + turret::kItemOwner, &owner, 4) || owner) {
        InterlockedIncrement(&bodyFaults);
        Line("%s body: %08X (vtable RVA %08X, drop slot %08X, owner %08X) is no droppable pickup; deleted", what, item, vtable - static_cast<uint32_t>(base), slot, owner);
        turret::DeleteRaw(item);
        return;
    }
    alignas(16) float pos[4] = {p.spot.x, p.spot.y + 0.15f, p.spot.z, 1.0f};
    if (!turret::DropRaw(item, pos)) {
        InterlockedIncrement(&bodyFaults);
        Fault("body drop", item, vtable);
        turret::DeleteRaw(item);
        return;
    }
    // Better no body than a free stasis pack or med pack.
    if (!turret::MakeUnpickable(item)) {
        InterlockedIncrement(&bodyFaults);
        Line("%s body: %08X stayed pickable after the registration update; deleted", what, item);
        turret::DeleteRaw(item);
        return;
    }
    p.body = item;
    p.bodyVtable = vtable;
    Read(item + turret::kEntityGuid, p.bodyGuid, 16);
    InterlockedIncrement(&bodySpawned);
}
void RemoveBody(Placed& p, const char* why) {
    if (!p.body) return;
    if (BodyIsOurs(p)) {
        if (turret::DeleteRaw(p.body)) InterlockedIncrement(&bodyRemoved);
        else { InterlockedIncrement(&bodyFaults); Fault("body delete", p.body, p.bodyVtable); }
    } else {
        InterlockedIncrement(&bodyGone);
        Line("body %08X is no longer ours at %s: left alone", p.body, why);
    }
    p.body = 0;
}
void WatchBody(Placed& p, DWORD wall) {
    if (!p.body || wall - p.bodyCheckAt <= 1000) return;
    p.bodyCheckAt = wall;
    uint32_t flags = 0;
    if (!BodyIsOurs(p)) { InterlockedIncrement(&bodyGone); Line("body %08X is gone (the engine took it)", p.body); p.body = 0; return; }
    if (Read(p.body + turret::kItemFlags, &flags, 4) && (flags & 1)) {
        InterlockedIncrement(&bodyRearmed);
        const bool off = turret::MakeUnpickable(p.body);
        Line("body %08X was pickable again (flags %08X): %s", p.body, flags, off ? "switched off" : "switch-off failed, body removed");
        if (!off) RemoveBody(p, "a pickable body");
    }
}

// --- the radius rings ----------------------------------------------------------
// The game has no ground decal to borrow, so each radius is a ring of resident
// Alchemy effects on the floor, created the way the turret creates its hologram
// (turret::CreateRaw: Create_ByGuid, 0xFFFF, flags 0, no parent) into handle slots
// held here. A slot is touched only while its effect still points back at it;
// the level's kill-all zeroes slot and back pointer alike.
coop_gadgets::RingConfig ringCfg;
bool ringReady = false;
// pickup_stasis.vfx (global_assets): the blinking glow under a stasis pack on the floor.
uint32_t mineRingGuid[4] = {0x1B884B0E, 0x1B884B0E, 0x1B884B0E, 0xA18B4273};
uint32_t burstRingGuid[4] = {0x1B884B0E, 0x1B884B0E, 0x1B884B0E, 0xA18B4273};
// pickup_health_single.vfx (global_assets): the med pack's blinking glow.
uint32_t fieldRingGuid[4] = {0xA3791C0E, 0xA3791C0E, 0xA3791C0E, 0xC849DE8D};
// Stand-ins when a glow is not resident: the small pickup holograms.
const uint32_t kStasisHologram[4] = {0xC91BAD4C, 0xC91BAD4C, 0xC91BAD4C, 0x107BC2CC};   // pickup_ui_stasis_pack_sm
const uint32_t kHealHologram[4] = {0x9F8CECFC, 0x9F8CECFC, 0x9F8CECFC, 0x2247FDD8};     // pickup_ui_health_single_sm
constexpr DWORD kRingRefillMs = 500;

struct RingSlot {
    uint32_t handle = 0;
    bool held = false, skip = false;
    DWORD born = 0, retryAt = 0;
    V3 at;
};
struct Ring {
    const char* name;
    const uint32_t* primary;
    const uint32_t* fallback;
    bool on = false, pulseKnown = false, upLogged = false, endLogged = false;
    int n = 0, pulseNext = 0;
    float radius = 0.0f;
    V3 center;
    const uint32_t* guid = nullptr;   // chosen at the first creation
    DWORD until = 0, chooseAt = 0;    // until: wall time a burst ring ends, 0 = held
    uint32_t pulseAt = 0;
    RingSlot slots[coop_gadgets::kRingMaxPoints];
};
Ring mineRing{"mine", mineRingGuid, kStasisHologram}, burstRing{"burst", burstRingGuid, kStasisHologram}, fieldRing{"field", fieldRingGuid, kHealHologram};
Ring* const rings[] = {&burstRing, &fieldRing, &mineRing};   // creation priority under the cap
coop_turret::RateLimit ringRate;
LONG ringBuilt = 0, ringPlayed = 0, ringAbsent = 0, ringRated = 0, ringCapped = 0, ringEmpty = 0, ringWalls = 0, ringDrops = 0, ringEnded = 0, ringPulses = 0,
     ringReleased = 0, ringFaults = 0, ringLines = 0;
constexpr LONG kRingLines = 24;

void RingLine(const char* format, ...) {
    if (++ringLines > kRingLines) return;
    char buffer[240];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    gate::Event("gadgets ring: %s", buffer);
}
void RingFault(const char* what, uint32_t a) {
    ++ringFaults;
    Fault(what, a, static_cast<uint32_t>(ringFaults));
    if (ringFaults >= 3 && ringReady) {
        ringReady = false;
        Line("three faulted ring effects: the rings are off for the session");
    }
}
bool Ours(const uint32_t* slot) {
    uint32_t back = 0;
    return *slot && Read(*slot + turret::kHandleOwner, &back, 4) && back == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(slot));
}
void ReleaseSlot(RingSlot& s) {
    if (Ours(&s.handle)) {
        if (turret::SoftKillRaw(&s.handle)) ++ringReleased;
        else RingFault("ring soft kill", s.handle);
    }
    s.handle = 0;
    s.held = false;
}
int HideRing(Ring& r) {
    int released = 0;
    for (RingSlot& s : r.slots)
        if (s.held || s.handle) { ++released; ReleaseSlot(s); }
    return released;
}
int ReleaseRing(Ring& r) {
    const int released = HideRing(r);
    r.on = false;
    return released;
}
// 1 resident, 0 not loaded, -1 the lookup faulted.
int RingResident(const uint32_t* guid) {
    uint32_t resource = 0;
    if (turret::FindVfxRaw(guid, resource)) return resource ? 1 : 0;
    RingFault("ring lookup", guid[0]);
    return -1;
}

// Points on the radius at the gadget's floor height. With the wall check a point
// is left out when the camera cast from the middle hits something on the way, or
// finds no floor under it within 0.8 m (a ledge).
void BuildRing(Ring& r, const V3& center, float radius, uint32_t exclude, DWORD until) {
    ReleaseRing(r);
    if (!ringCfg.enabled || !ringReady) return;
    r.on = true;
    r.n = ringCfg.points;
    r.radius = radius;
    r.center = center;
    r.guid = nullptr;
    r.until = until;
    r.chooseAt = 0;
    r.pulseKnown = r.upLogged = r.endLogged = false;
    r.pulseNext = 0;
    int walls = 0, drops = 0;
    for (int i = 0; i < r.n; ++i) {
        RingSlot& s = r.slots[i];
        s.retryAt = 0;
        s.skip = false;
        s.at = coop_gadgets::RingPoint(center, radius, i, r.n, ringCfg.lift);
        if (!ringCfg.wallCheck || !castOn) continue;
        float fraction = 1.0f;
        uint32_t hit = 0;
        const float high = center.y + 0.5f;
        if (Cast({center.x, high, center.z}, {s.at.x, high, s.at.z}, exclude, fraction, hit, nullptr) && fraction < 0.999f) {
            s.skip = true;
            ++walls;
        } else if (Cast({s.at.x, high, s.at.z}, {s.at.x, center.y - 0.8f, s.at.z}, exclude, fraction, hit, nullptr) && !(fraction < 0.999f)) {
            s.skip = true;
            ++drops;
        }
    }
    ++ringBuilt;
    ringWalls += walls;
    ringDrops += drops;
    RingLine("%s ring: %d points on %.1f m around %.2f %.2f %.2f, %d behind a wall, %d over a drop%s", r.name, r.n, radius, center.x, center.y, center.z, walls, drops,
             !ringCfg.wallCheck ? " (no wall check)" : castOn ? "" : " (no camera cast: unchecked)");
}

// The configured glow, else the hologram, looked up once a second until one is resident.
bool ChooseEffect(Ring& r, DWORD wall) {
    if (r.guid) return true;
    if (static_cast<LONG>(wall - r.chooseAt) < 0) return false;
    r.chooseAt = wall + 1000;
    const int primary = RingResident(r.primary);
    if (primary == 1) { r.guid = r.primary; return true; }
    const int fallback = primary < 0 ? -1 : RingResident(r.fallback);
    if (fallback == 1) {
        r.guid = r.fallback;
        RingLine("%s ring: %08X-%08X not resident, the hologram %08X-%08X stands in", r.name, r.primary[0], r.primary[3], r.fallback[0], r.fallback[3]);
        return true;
    }
    ++ringAbsent;
    if (ringAbsent <= 3)
        RingLine("%s ring: neither %08X-%08X nor %08X-%08X is resident (lookup %d/%d)", r.name, r.primary[0], r.primary[3], r.fallback[0], r.fallback[3], primary,
                 fallback);
    return false;
}

// Creates the shown points that are missing and lets the rest go. pulseStep renews
// one point after the other in game time, so the ring shimmers around once per pulse.
void ShowRing(Ring& r, DWORD wall, uint32_t now, int visible, uint32_t pulseStep, int& live) {
    if (!r.on || !ringReady || !turret::VfxRunning() || !ChooseEffect(r, wall)) return;
    if (pulseStep) {
        if (!r.pulseKnown || static_cast<int32_t>(r.pulseAt - now) > 60000) { r.pulseAt = now + pulseStep; r.pulseKnown = true; }
        if (static_cast<int32_t>(now - r.pulseAt) >= 0) {
            r.pulseAt = now + pulseStep;
            for (int k = 0; k < r.n; ++k) {
                const int i = (r.pulseNext + k) % r.n;
                if (!r.slots[i].held) continue;
                ReleaseSlot(r.slots[i]);
                --live;
                r.slots[i].retryAt = 0;
                r.pulseNext = i + 1;
                ++ringPulses;
                break;
            }
        }
    }
    int shown = 0;
    for (int i = 0; i < r.n && ringReady; ++i) {
        RingSlot& s = r.slots[i];
        if (s.skip || !coop_gadgets::PointShown(i, r.n, visible)) {
            if (s.held) { ReleaseSlot(s); --live; }
            continue;
        }
        if (s.held) { ++shown; continue; }
        if (static_cast<LONG>(wall - s.retryAt) < 0) continue;
        if (coop_gadgets::RingBudget(1, live, ringCfg.maxEffects) < 1) { ++ringCapped; s.retryAt = wall + kRingRefillMs; continue; }
        if (!ringRate.Allow(wall, ringCfg.perSecond)) { ++ringRated; s.retryAt = wall + 100; continue; }
        alignas(16) const float m[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, s.at.x, s.at.y, s.at.z, 1.0f};
        s.handle = 0;
        if (!turret::CreateRaw(&s.handle, r.guid, m)) {
            s.handle = 0;
            s.retryAt = wall + 1000;
            RingFault("ring create", r.guid[0]);
            continue;
        }
        if (!s.handle) { ++ringEmpty; s.retryAt = wall + 1000; continue; }
        s.held = true;
        s.born = wall;
        s.retryAt = wall + kRingRefillMs;
        ++live;
        ++shown;
        ++ringPlayed;
    }
    if (!r.upLogged && shown) {
        r.upLogged = true;
        RingLine("%s ring up: %d of %d points, effect %08X-%08X, %d ring effects live", r.name, shown, r.n, r.guid[0], r.guid[3], live);
    }
}

void UpdateRings(DWORD wall, uint32_t now, bool scene, bool paused) {
    if (burstRing.on && static_cast<LONG>(wall - burstRing.until) >= 0) ReleaseRing(burstRing);
    const bool armed = mine.on && stasisReady && !armLeft.left;
    if (mineRing.on && !mine.on) ReleaseRing(mineRing);
    else if (mineRing.on && !armed) HideRing(mineRing);
    if (fieldRing.on && !field.on) ReleaseRing(fieldRing);
    if (!ringReady || !scene) {
        for (Ring* r : rings) HideRing(*r);
        return;
    }
    // Effects that ended on their own: refilled at most every kRingRefillMs.
    int live = 0;
    for (Ring* r : rings)
        for (int i = 0; i < coop_gadgets::kRingMaxPoints; ++i) {
            RingSlot& s = r->slots[i];
            if (!s.held) continue;
            if (Ours(&s.handle)) { ++live; continue; }
            s.handle = 0;
            s.held = false;
            ++ringEnded;
            if (!r->endLogged) {
                r->endLogged = true;
                RingLine("%s ring: point %d ended on its own after %lu ms (refilled)", r->name, i, static_cast<unsigned long>(wall - s.born));
            }
        }
    if (paused) return;   // nothing new while the game stands
    ShowRing(burstRing, wall, now, burstRing.n, 0, live);
    ShowRing(fieldRing, wall, now, coop_gadgets::VisibleCount(fieldRing.n, fieldLeft.left, coop_gadgets::Ms(ringCfg.fadeSeconds)), 0, live);
    if (armed) ShowRing(mineRing, wall, now, mineRing.n, coop_gadgets::PulseStepMs(mineRing.n, ringCfg.pulseSeconds), live);
}
void ReleaseRings(const char* why) {
    int released = 0;
    for (Ring* r : rings) released += ReleaseRing(*r);
    if (released) RingLine("released %d ring effects at %s", released, why);
}
int RingsLive() {
    int live = 0;
    for (const Ring* r : rings)
        for (const RingSlot& s : r->slots) live += s.held ? 1 : 0;
    return live;
}

// --- placing -----------------------------------------------------------------
// The turret's rules: the nav mesh (or the floor under the spot), a clear line
// from the eye and nobody standing there.
coop_turret::Place FindSpot(uint32_t player, float distance, V3& spot, char* detail, size_t detailSize) {
    coop_turret::V3 feet, forward, candidate;
    detail[0] = 0;
    if (!turret::Pos(player, feet) || !turret::Facing(player, forward) || !coop_turret::Candidate(feet, forward, distance, candidate)) {
        std::snprintf(detail, detailSize, "no position or facing");
        return coop_turret::Place::NoPlayer;
    }
    coop_turret::Config pc;
    pc.placeDistance = distance;
    pc = coop_turret::Sanitized(pc);
    coop_turret::PlaceInput in;
    in.feet = feet;
    in.candidate = candidate;
    in.meshKnown = bot_nav::ready && bot_nav::LevelSpace() != 0;
    if (in.meshKnown) {
        coop_bot::Vec3 end;
        int count = 0;
        in.meshHit = bot_nav::PathEnd({feet.x, feet.y, feet.z}, {candidate.x, candidate.y, candidate.z}, end, count);
        in.meshEnd = {end.x, end.y, end.z};
    } else {
        float fraction = 1.0f;
        uint32_t hit = 0;
        V3 point;
        const V3 from = {candidate.x, feet.y + 1.0f, candidate.z}, to = {candidate.x, feet.y - 1.5f, candidate.z};
        if (Cast(from, to, player, fraction, hit, &point) && fraction < 0.999f) {
            in.floorHit = true;
            in.floorY = from.y + (to.y - from.y) * fraction;   // the cast's output vector is the hit normal, not the point
        }
    }
    const coop_turret::PlaceResult r = coop_turret::SpotFor(in, pc);
    std::snprintf(detail, detailSize, "feet %.2f %.2f %.2f, candidate %.2f %.2f %.2f, mesh %d/%d end %.2f %.2f %.2f, floor %d %.2f", feet.x, feet.y, feet.z,
                  candidate.x, candidate.y, candidate.z, in.meshKnown ? 1 : 0, in.meshHit ? 1 : 0, in.meshEnd.x, in.meshEnd.y, in.meshEnd.z, in.floorHit ? 1 : 0, in.floorY);
    if (r.verdict != coop_turret::Place::Ok) return r.verdict;
    const V3 eye = {feet.x, feet.y + 1.5f, feet.z};
    const bool line = castOn && Clear(eye, {r.spot.x, r.spot.y + 0.3f, r.spot.z}, player);
    coop_turret::V3 others[coop_bot::kMaxPlayers];
    const int n = turret::Friends(player, others, coop_bot::kMaxPlayers);
    float nearest = 1000.0f;
    for (int i = 0; i < n; ++i) {
        const float d = coop_turret::Flat(others[i], r.spot);
        if (d < nearest && std::fabs(others[i].y - r.spot.y) < 2.0f) nearest = d;
    }
    const coop_turret::Place verdict = coop_turret::Finish(r.verdict, line, nearest, pc);
    if (verdict == coop_turret::Place::Ok) spot = ToV3(r.spot);
    else if (!castOn) std::snprintf(detail, detailSize, "no camera cast: the spot cannot be checked");
    return verdict;
}
void Refuse(const char* name, volatile LONG* counters, coop_turret::Place why, const char* detail) {
    InterlockedIncrement(&counters[static_cast<int>(why)]);
    Line("%s not placed: %s (%s)", name, coop_turret::PlaceName(why), detail);
    switch (why) {
    case coop_turret::Place::Blocked: Notice("%s: NO CLEAR LINE TO THE SPOT", name); break;
    case coop_turret::Place::Occupied: Notice("%s: SOMEONE STANDS THERE", name); break;
    case coop_turret::Place::TooHigh: Notice("%s: FLOOR TOO HIGH OR LOW", name); break;
    default: Notice("%s: NO ROOM HERE", name); break;
    }
}
const char* NotNowWhy(uint32_t player) {
    return !player ? "no player (menu, loading or a necromorph body)" : turret::CutsceneNow() ? "a cutscene runs" : "the level is not ready";
}

// --- the stasis mine -----------------------------------------------------------
struct StasisFn { uint32_t vtable; uint8_t multiplayer, pad[3]; float strength, duration; uint32_t owner; };
static_assert(sizeof(StasisFn) == 0x14, "FireStasisFunction layout");
__declspec(noinline) bool StasisRaw(const StasisFn* fn, uint32_t entity) {
    __try {
        reinterpret_cast<void(__thiscall*)(const void*, uint32_t, const void*)>(base + kStasisHitRva)(fn, entity, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) uint32_t ComponentRaw(uint32_t entity, unsigned char type) {
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t)>(base + kComponentLookupRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(entity)), type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// The enemy's stasis resistance (duration factor, 0 = immune) and immunity flag, for the log.
bool Resistance(uint32_t entity, float& resistance, bool& immune) {
    unsigned char type = 0;
    if (!Read(base + kStasisTypeRva, &type, 1)) return false;
    uint32_t c = ComponentRaw(entity, type), parent = 0, attributes = 0, flags = 0;
    for (int i = 0; c && i < 8 && Read(c + kComponentParent, &parent, 4) && parent; ++i) c = parent;
    if (!c || !Read(c + kStasisAttributes, &attributes, 4) || !attributes || !Read(attributes, &resistance, 4) || !Read(c + kStasisFlags, &flags, 4)) return false;
    immune = (flags & 1) != 0;
    return true;
}
coop_gadgets::StasisValues StasisNow(uint32_t owner) {
    uint32_t settings = 0, attributes = 0;
    float strength = -1.0f, duration = -1.0f;
    if (Read(base + kPlayerSettingsRva, &settings, 4) && settings && Read(settings + kSettingsAttributes, &attributes, 4) && attributes)
        Read(attributes + kStasisStrength, &strength, 4);
    Read(owner + kPlayerStasisDuration, &duration, 4);
    return coop_gadgets::Stasis(mineCfg, strength, duration);
}
bool Multiplayer() {
    uint32_t flags = 0;
    unsigned char b = 0;
    return Read(base + kGameFlagsRva, &flags, 4) && flags && Read(flags + kGameFlagsByte, &b, 1) && ((b >> 1) & 1);
}
float TimeMultiplier(uint32_t entity) {
    float tm = -1.0f;
    return Read(entity + kTimeMultiplier, &tm, 4) && std::isfinite(tm) ? tm : -1.0f;
}
bool Slow(uint32_t entity, const StasisFn& fn, uint32_t now) {
    uint32_t vtable = 0;
    // Never anything a player is: the scan leaves them out, this makes sure.
    if (!entity || !Read(entity, &vtable, 4) || vtable == base + kPlayerVtableRva || IsPlayerOrNecroEntity(entity) || IsBotEntity(entity) ||
        guidfix::IsGhostEntity(entity)) {
        InterlockedIncrement(&mineRejected);
        return false;
    }
    const float before = TimeMultiplier(entity);
    if (!StasisRaw(&fn, entity)) {
        InterlockedIncrement(&stasisFaults);
        Fault("stasis hit", entity, vtable);
        if (InterlockedCompareExchange(&stasisFaults, 0, 0) >= 3) {
            stasisReady = false;
            Line("three faulted stasis hits: the mine is off for the session");
        }
        return false;
    }
    InterlockedIncrement(&mineStunned);
    pending[pendingNext++ % std::size(pending)] = Pending{entity, vtable, before, now};
    if (InterlockedIncrement(&hitLines) <= kHitLines) {
        float resistance = -1.0f;
        bool immune = false;
        const bool known = Resistance(entity, resistance, immune);
        gate::Event("gadgets: stasis on %08X (vtable RVA %08X) time multiplier %.2f, resistance %s%.2f%s", entity, vtable - static_cast<uint32_t>(base), before,
                    known ? "" : "unknown ", resistance, immune ? ", immune" : "");
    }
    return true;
}
// Whether the stasis arrived: the time multiplier went down within 1.5 s.
void FollowPending(uint32_t now) {
    for (Pending& p : pending) {
        if (!p.entity) continue;
        uint32_t vtable = 0;
        const float tm = Read(p.entity, &vtable, 4) && vtable == p.vtable ? TimeMultiplier(p.entity) : -1.0f;
        if (tm >= 0.0f && tm < p.before - 0.05f) {
            InterlockedIncrement(&mineSlowed);
            p = Pending{};
        } else if (p.before >= 0.0f && p.before < 0.5f && tm >= 0.0f && tm < 0.5f && now - p.sentAt > 1500) {
            InterlockedIncrement(&mineAlready);   // in stasis before the burst: nothing to drop
            p = Pending{};
        } else if (now - p.sentAt > 1500 || tm < 0.0f) {
            InterlockedIncrement(&mineNoEffect);
            if (InterlockedIncrement(&effectLines) <= kEffectLines)
                gate::Event("gadgets: stasis on %08X changed nothing in 1.5 s (time multiplier %.2f -> %.2f; immune, resistance 0 or gone)", p.entity, p.before, tm);
            p = Pending{};
        }
    }
}

bot_sense::Snapshot snap;
void Burst(uint32_t now) {
    coop_gadgets::Body bodies[coop_bot::kMaxEnemies];
    int n = 0;
    for (int i = 0; i < snap.enemyCount && i < coop_bot::kMaxEnemies; ++i) {
        const coop_bot::Enemy& e = snap.enemies[i];
        coop_gadgets::Body& b = bodies[n++];
        b = coop_gadgets::Body{};
        b.id = snap.enemyEntity[i];
        b.pos = ToV3(e.pos);
        b.health = e.health;
        b.objective = e.objective;
        b.player = IsPlayerOrNecroEntity(b.id);
        b.bot = IsBotEntity(b.id);
        b.ghost = guidfix::IsGhostEntity(b.id);
    }
    if (!coop_gadgets::Triggered(mine.spot, bodies, n, mineCfg)) return;
    int hit[coop_bot::kMaxEnemies];
    const int m = coop_gadgets::InBurst(mine.spot, bodies, n, mineCfg, hit, coop_bot::kMaxEnemies);
    const coop_gadgets::StasisValues v = StasisNow(mine.owner);
    uint32_t manager = 0;
    if (!Read(base + kStasisManagerRva, &manager, 4) || !manager) {
        if (InterlockedIncrement(&noManager) <= 3) Line("mine triggered but there is no StasisManager: held");
        return;
    }
    StasisFn fn{static_cast<uint32_t>(base + kStasisFnVtableRva), static_cast<uint8_t>(Multiplayer() ? 1 : 0), {}, v.slowPercent, v.durationSeconds, mine.owner};
    int slowed = 0;
    for (int k = 0; k < m; ++k)
        if (Slow(bodies[hit[k]].id, fn, now)) ++slowed;
    InterlockedIncrement(&mineTriggers);
    Line("mine went off at %.2f %.2f %.2f: %d enemies in %.1f m (%d in the scan), %d slowed, strength %.0f%% for %.1f s (%s), multiplayer %d", mine.spot.x, mine.spot.y,
         mine.spot.z, m, mineCfg.radiusMeters, n, slowed, v.slowPercent, v.durationSeconds, v.fromGame ? "the owner's own" : "INI or fallback", fn.multiplayer);
    ReleaseRing(mineRing);
    BuildRing(burstRing, mine.spot, mineCfg.radiusMeters, mine.owner, GetTickCount() + coop_gadgets::Ms(ringCfg.burstSeconds));
    RemoveBody(mine, "going off");
    mine = Placed{};
    mineCharge.Spend(coop_gadgets::Ms(mineCfg.rechargeSeconds));
    Notice("STASIS MINE TRIGGERED (%d %s)", slowed, slowed == 1 ? "ENEMY" : "ENEMIES");
}

void PressMine(uint32_t player, bool ready) {
    InterlockedIncrement(&minePresses);
    V3 feet;
    const float d = mine.on && player && Pos(player, feet) ? coop_gadgets::Flat(feet, mine.spot) : 1000.0f;
    switch (coop_gadgets::MineAction(mine.on, d, ready, mineCharge.state, mineCfg)) {
    case coop_gadgets::MinePress::PickUp:
        ReleaseRing(mineRing);
        RemoveBody(mine, "pick up");
        mine = Placed{};
        mineCharge.Refund();
        InterlockedIncrement(&minePickedUp);
        Line("mine picked up %.1f m away", d);
        Notice("STASIS MINE PICKED UP");
        return;
    case coop_gadgets::MinePress::TooFar:
        InterlockedIncrement(&mineTooFar);
        Line("mine not picked up: %.1f m away (PickupRange %.1f)", d, mineCfg.pickupRange);
        Notice("STASIS MINE: %.0f M AWAY, WALK CLOSER", d);
        return;
    case coop_gadgets::MinePress::Recharging:
        InterlockedIncrement(&mineRecharging);
        Notice("STASIS MINE RECHARGING (%d S)", mineCharge.SecondsLeft());
        return;
    case coop_gadgets::MinePress::NotNow:
        InterlockedIncrement(&mineNotNow);
        Line("mine not placed: %s", NotNowWhy(player));
        Notice("STASIS MINE: NOT NOW");
        return;
    case coop_gadgets::MinePress::Place: break;
    }
    if (!stasisReady) { InterlockedIncrement(&mineNotNow); Line("mine not placed: the stasis path is off"); Notice("STASIS MINE: NOT NOW"); return; }
    char detail[200];
    V3 spot;
    const coop_turret::Place why = FindSpot(player, mineCfg.placeDistance, spot, detail, sizeof(detail));
    if (why != coop_turret::Place::Ok) { Refuse("STASIS MINE", mineRefused, why, detail); return; }
    mineCharge.Take();
    mine = Placed{};
    mine.on = true;
    mine.spot = spot;
    mine.owner = player;
    mine.placedAt = GetTickCount();
    SpawnBody(mine, mineGuid, "mine");
    BuildRing(mineRing, spot, mineCfg.triggerMeters, player, 0);
    armLeft.Start(coop_gadgets::Ms(mineCfg.armSeconds));
    scanAt = 0;
    InterlockedIncrement(&minePlaced);
    Line("mine placed at %.2f %.2f %.2f (%s), body %08X, arms in %.1f s", spot.x, spot.y, spot.z, detail, mine.body, mineCfg.armSeconds);
    Notice("STASIS MINE PLACED");
}

// --- the heal field ------------------------------------------------------------
// Every local Isaac in the field one heal step; partners' copies are only counted.
void HealStep(uint32_t dt) {
    InterlockedIncrement(&healSteps);
    uint32_t node = 0;
    if (!Read(base + bot_sense::kReflectableListRva, &node, 4)) return;
    const uint32_t local = LocalCampaignPlayer();
    for (unsigned steps = 0; node && steps < 8192; ++steps) {
        const uint32_t entity = node - bot_sense::kEntityReflectable;
        uint32_t vtable = 0, next = 0;
        if (Read(entity, &vtable, 4) && vtable == base + kPlayerVtableRva) {
            coop_gadgets::Patient p;
            p.id = entity;
            p.local = entity == local || IsBotEntity(entity);
            const bool at = Pos(entity, p.pos);
            if (at && coop_gadgets::InField(field.spot, p, fieldCfg)) {
                if (!p.local) {
                    InterlockedIncrement(&healPartners);
                } else if (const uint32_t c = hits::FindHealthComponent(entity)) {
                    unsigned char flags = 0;
                    if (Read(c + hits::kHealthCurrent, &p.health, 4) && Read(c + hits::kHealthMax, &p.max, 4) && Read(c + kHealthFlags, &flags, 1)) {
                        p.dead = (flags & 2) || (!(p.health > 0.0f) && !(flags & 1));
                        const float amount = coop_gadgets::HealAmount(p, fieldCfg.ratePercent, dt);
                        if (p.dead || !(p.health > 0.0f)) InterlockedIncrement(&healDead);
                        else if (!(amount > 0.0f)) InterlockedIncrement(&healFull);
                        else if (!hits::CallSetHealth(c, p.health + amount)) {
                            InterlockedIncrement(&healFaults);
                            Fault("SetHealth", entity, c);
                        } else {
                            float after = 0.0f;
                            if (Read(c + hits::kHealthCurrent, &after, 4) && after > p.health + 0.0001f) {
                                InterlockedIncrement(&healApplied);
                                InterlockedExchangeAdd(&healedCenti, static_cast<LONG>((after - p.health) * 100.0f + 0.5f));
                            } else {
                                InterlockedIncrement(&healNoEffect);
                                if (InterlockedIncrement(&effectLines) <= kEffectLines)
                                    gate::Event("gadgets: heal on %08X changed nothing (%.1f + %.2f of %.1f -> %.1f)", entity, p.health, amount, p.max, after);
                            }
                        }
                    }
                } else {
                    InterlockedIncrement(&healFaults);
                    Fault("no health component", entity, vtable);
                }
            } else if (at && p.local) {
                InterlockedIncrement(&healOutside);
            }
        }
        if (!Read(node + 4, &next, 4) || next == node) break;
        node = next;
    }
}
void EndField(const char* why, bool notice) {
    ReleaseRing(fieldRing);
    RemoveBody(field, why);
    field = Placed{};
    fieldLeft.left = 0;
    fieldCharge.Spend(coop_gadgets::Ms(fieldCfg.rechargeSeconds));
    InterlockedIncrement(&fieldEnded);
    Line("heal field ended (%s), %.1f health healed so far, recharge %.0f s", why, static_cast<float>(InterlockedCompareExchange(&healedCenti, 0, 0)) / 100.0f,
         fieldCfg.rechargeSeconds);
    if (notice) Notice("HEAL FIELD ENDED");
}
void PressField(uint32_t player, bool ready) {
    InterlockedIncrement(&fieldPresses);
    switch (coop_gadgets::FieldAction(field.on, ready, fieldCharge.state)) {
    case coop_gadgets::FieldPress::Running:
        InterlockedIncrement(&fieldRunning);
        Notice("HEAL FIELD ACTIVE (%d S LEFT)", fieldLeft.SecondsLeft());
        return;
    case coop_gadgets::FieldPress::Recharging:
        InterlockedIncrement(&fieldRecharging);
        Notice("HEAL FIELD RECHARGING (%d S)", fieldCharge.SecondsLeft());
        return;
    case coop_gadgets::FieldPress::NotNow:
        InterlockedIncrement(&fieldNotNow);
        Line("field not placed: %s", NotNowWhy(player));
        Notice("HEAL FIELD: NOT NOW");
        return;
    case coop_gadgets::FieldPress::Place: break;
    }
    if (!healReady) { InterlockedIncrement(&fieldNotNow); Line("field not placed: the heal path is off"); Notice("HEAL FIELD: NOT NOW"); return; }
    char detail[200];
    V3 spot;
    const coop_turret::Place why = FindSpot(player, fieldCfg.placeDistance, spot, detail, sizeof(detail));
    if (why != coop_turret::Place::Ok) { Refuse("HEAL FIELD", fieldRefused, why, detail); return; }
    fieldCharge.Take();
    field = Placed{};
    field.on = true;
    field.spot = spot;
    field.owner = player;
    field.placedAt = GetTickCount();
    SpawnBody(field, fieldGuid, "field");
    BuildRing(fieldRing, spot, fieldCfg.radiusMeters, player, 0);
    fieldLeft.Start(coop_gadgets::Ms(fieldCfg.durationSeconds));
    healAcc = 0;
    InterlockedIncrement(&fieldPlaced);
    Line("heal field placed at %.2f %.2f %.2f (%s), body %08X, %.0f s, %.1f m, %.1f%%/s", spot.x, spot.y, spot.z, detail, field.body, fieldCfg.durationSeconds,
         fieldCfg.radiusMeters, fieldCfg.ratePercent);
    Notice("HEAL FIELD PLACED (%d S)", fieldLeft.SecondsLeft());
}

// --- the tick ------------------------------------------------------------------
// The world they stood in is gone: an unused mine comes back, a running field is spent.
// Ring effects still ours are soft-killed like the turret's hologram.
void Forget(const char* why, bool worldKept) {
    ReleaseRings(why);
    if (mine.on) {
        InterlockedIncrement(&mineForgotten);
        InterlockedIncrement(&mineRefunds);
        Line("mine removed with %s, its charge is back (body %08X%s)", why, mine.body, worldKept ? " removed" : " left to the engine");
        if (worldKept) RemoveBody(mine, why);
        mine = Placed{};
        mineCharge.Refund();
    }
    if (field.on) {
        InterlockedIncrement(&fieldForgotten);
        Line("heal field removed with %s (body %08X%s)", why, field.body, worldKept ? " removed" : " left to the engine");
        if (!worldKept) field.body = 0;
        EndField(why, false);
    }
    for (Pending& p : pending) p = Pending{};
}

void TickInner() {
    const DWORD wall = GetTickCount();
    const LONG starts = InterlockedCompareExchange(&loadsync::levelStarts, 0, 0);
    const LONG clears = InterlockedCompareExchange(&gate::restartClears, 0, 0);
    if (starts != startsSeen) {
        Forget("a level start", false);
        castOn = bot_sense::losVerified;   // a fault switch-off lasts one level
        for (DWORD& when : castFaultTimes) when = 0;
        startsSeen = starts;
    }
    if (clears != clearsSeen) { Forget("a restart or level exit", false); clearsSeen = clears; }
    const uint32_t player = inLevel ? LocalCampaignPlayer() : 0;
    if (mine.on || field.on) {
        if (!inLevel) Forget("the level going away", false);
        else if (turret::FrontendNow()) Forget("the main menu", false);
        else if (player && ((mine.on && player != mine.owner) || (field.on && player != field.owner))) Forget("a new player body", true);
    }

    // Game time: a pause stops arming, triggering, healing and recharging alike.
    const uint32_t now = turret::SimMs();
    const uint32_t dt = clock.Step(now);
    const bool paused = clock.paused;
    if (mineCharge.Advance(dt)) { InterlockedIncrement(&mineReadyAgain); Line("mine recharged"); Notice("STASIS MINE READY"); }
    if (fieldCharge.Advance(dt)) { InterlockedIncrement(&fieldReadyAgain); Line("heal field recharged"); Notice("HEAL FIELD READY"); }

    const bool focused = necro::GameWindowFocused();
    const bool ready = player && turret::WorldReady() && !turret::CutsceneNow();
    if (mineWanted && mineEdge.Pressed(mineKey && focused && !shop::HoldsKey(mineKey, coop_shop::Guard::StasisMine) && (GetAsyncKeyState(mineKey) & 0x8000) != 0))
        if (progress::GadgetAllowed("StasisMine", "STASIS MINE")) PressMine(player, ready);
    if (fieldWanted && fieldEdge.Pressed(fieldKey && focused && !shop::HoldsKey(fieldKey, coop_shop::Guard::HealField) && (GetAsyncKeyState(fieldKey) & 0x8000) != 0))
        if (progress::GadgetAllowed("HealField", "HEAL FIELD")) PressField(player, ready);

    WatchBody(mine, wall);
    WatchBody(field, wall);
    FollowPending(now);
    UpdateRings(wall, now, turret::WorldReady() && !turret::CutsceneNow(), paused);
    mineActive = fieldActive = false;
    if (!mine.on && !field.on) return;
    if (paused) { InterlockedIncrement(&pausedFrames); return; }
    if (!turret::WorldReady()) return;
    if (turret::CutsceneNow()) { InterlockedIncrement(&cutsceneFrames); return; }

    if (mine.on && stasisReady) {
        mineActive = true;
        armLeft.Advance(dt);
        if (static_cast<int32_t>(scanAt - now) > 1000) scanAt = now;   // the clock went back
        // A look every 150 ms once armed.
        if (!armLeft.left && static_cast<int32_t>(now - scanAt) >= 0) {
            scanAt = now + 150;
            InterlockedIncrement(&mineScans);
            V3 feet;
            const V3 owner = Pos(mine.owner, feet) ? feet : mine.spot;
            bot_sense::Scan(snap, 0, ToB(mine.spot), ToB(owner));
            Burst(now);
        }
    }
    if (field.on) {
        fieldActive = true;
        const uint32_t step = dt < fieldLeft.left ? dt : fieldLeft.left;
        const bool over = fieldLeft.Advance(dt);
        healAcc += step;
        if (healAcc >= fieldCfg.tickMs || (over && healAcc)) {
            if (healReady) HealStep(healAcc);
            healAcc = 0;
        }
        if (over) EndField("its time ran out", true);
    }
}

// Switched off: no ring outlives the gadgets.
__declspec(noinline) void DropRings() {
    __try {
        ReleaseRings("the gadgets switching off");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ringReady = false;
    }
}
int TickFilter(unsigned code) {
    InterlockedIncrement(&tickFaults);
    if (InterlockedIncrement(&faultLines) <= kFaultLines) gate::Event("gadgets FAULT tick code %08X", code);
    return EXCEPTION_EXECUTE_HANDLER;
}
void Tick() {
    if (!installed) return;
    __try {
        TickInner();
    } __except (TickFilter(GetExceptionCode())) {
        if (InterlockedCompareExchange(&tickFaults, 0, 0) >= 5) {
            installed = false;
            gate::Event("gadgets: five faulted ticks, switched off for the session");
        }
    }
    if (!installed) DropRings();
}

// --- setup -------------------------------------------------------------------
bool Bytes(uint32_t rva, const unsigned char* bytes, size_t size) {
    unsigned char actual[48]{};
    return size <= sizeof(actual) && Read(base + rva, actual, size) && std::memcmp(actual, bytes, size) == 0;
}
float IniFloat(const std::string& ini, const char* name, float fallback) {
    char text[32]{}, def[32]{};
    std::snprintf(def, sizeof(def), "%g", fallback);
    GetPrivateProfileStringA("Gadgets", name, def, text, sizeof(text), ini.c_str());
    const float v = static_cast<float>(std::atof(text));
    return std::isfinite(v) ? v : fallback;
}
void IniGuid(FILE* log, const std::string& ini, const char* name, uint32_t* guid) {
    char text[64]{}, def[64]{};
    std::snprintf(def, sizeof(def), "%08X-%08X-%08X-%08X", guid[0], guid[1], guid[2], guid[3]);
    GetPrivateProfileStringA("Gadgets", name, def, text, sizeof(text), ini.c_str());
    if (!turret::ParseGuid(text, guid)) std::fprintf(log, "gadgets %s '%s' unreadable; %s stays\n", name, text, def);
}

bool Install(FILE* log, const std::string& ini) {
    mineWanted = GetPrivateProfileIntA("Gadgets", "StasisMineEnabled", 1, ini.c_str()) != 0;
    fieldWanted = GetPrivateProfileIntA("Gadgets", "HealFieldEnabled", 1, ini.c_str()) != 0;
    if (!mineWanted && !fieldWanted) { std::fprintf(log, "gadgets off ([Gadgets] StasisMineEnabled=0 HealFieldEnabled=0)\n"); return false; }
    mineKey = GetPrivateProfileIntA("Gadgets", "StasisMineKey", 103, ini.c_str());
    fieldKey = GetPrivateProfileIntA("Gadgets", "HealFieldKey", 104, ini.c_str());
    bodyWanted = GetPrivateProfileIntA("Gadgets", "Body", 1, ini.c_str()) != 0;
    IniGuid(log, ini, "StasisMineBodyGuid", mineGuid);
    IniGuid(log, ini, "HealFieldBodyGuid", fieldGuid);
    coop_gadgets::MineConfig m;
    m.triggerMeters = IniFloat(ini, "StasisMineTriggerMeters", m.triggerMeters);
    m.radiusMeters = IniFloat(ini, "StasisMineRadiusMeters", m.radiusMeters);
    m.rechargeSeconds = IniFloat(ini, "StasisMineRechargeSeconds", m.rechargeSeconds);
    m.armSeconds = IniFloat(ini, "StasisMineArmSeconds", m.armSeconds);
    m.durationSeconds = IniFloat(ini, "StasisMineDurationSeconds", m.durationSeconds);
    m.slowPercent = IniFloat(ini, "StasisMineSlowPercent", m.slowPercent);
    m.pickupRange = IniFloat(ini, "StasisMinePickupRange", m.pickupRange);
    mineCfg = coop_gadgets::Sanitized(m);
    coop_gadgets::FieldConfig f;
    f.durationSeconds = IniFloat(ini, "HealFieldDurationSeconds", f.durationSeconds);
    f.radiusMeters = IniFloat(ini, "HealFieldRadiusMeters", f.radiusMeters);
    f.ratePercent = IniFloat(ini, "HealFieldRatePercent", f.ratePercent);
    f.rechargeSeconds = IniFloat(ini, "HealFieldRechargeSeconds", f.rechargeSeconds);
    fieldCfg = coop_gadgets::Sanitized(f);
    coop_gadgets::RingConfig rc;
    rc.enabled = GetPrivateProfileIntA("Gadgets", "RadiusRing", 1, ini.c_str()) != 0;
    rc.points = GetPrivateProfileIntA("Gadgets", "RingPoints", rc.points, ini.c_str());
    rc.maxEffects = GetPrivateProfileIntA("Gadgets", "RingMaxEffects", rc.maxEffects, ini.c_str());
    rc.perSecond = GetPrivateProfileIntA("Gadgets", "RingEffectsPerSecond", rc.perSecond, ini.c_str());
    rc.pulseSeconds = IniFloat(ini, "RingPulseSeconds", rc.pulseSeconds);
    rc.fadeSeconds = IniFloat(ini, "RingFadeSeconds", rc.fadeSeconds);
    rc.burstSeconds = IniFloat(ini, "RingBurstSeconds", rc.burstSeconds);
    rc.lift = IniFloat(ini, "RingLift", rc.lift);
    rc.wallCheck = GetPrivateProfileIntA("Gadgets", "RingWallCheck", 1, ini.c_str()) != 0;
    ringCfg = coop_gadgets::Sanitized(rc);
    IniGuid(log, ini, "StasisMineRingVfxGuid", mineRingGuid);
    IniGuid(log, ini, "StasisMineBurstVfxGuid", burstRingGuid);
    IniGuid(log, ini, "HealFieldRingVfxGuid", fieldRingGuid);

    // The stasis hit (prologue, then the StasisEntity call with the 0.01 scale and the
    // manager singleton), StasisEntity itself, the FireStasisFunction vtable, and
    // ProcessAttackCollision's reads that give the object its values.
    static const unsigned char kHit[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xB4, 0x00, 0x00, 0x00, 0x53, 0x56, 0x8B, 0x75};
    static const unsigned char kHitCall[] = {0xF3, 0x0F, 0x10, 0x47, 0x08, 0xF3, 0x0F, 0x59, 0x05, 0x04, 0x8A, 0xAA, 0x01, 0x8B, 0x0D, 0x48, 0x13, 0x02, 0x02,
                                             0x83, 0xEC, 0x08, 0xD9, 0x5C, 0x24, 0x04, 0xF3, 0x0F, 0x11, 0x04, 0x24, 0x56, 0xE8, 0x05, 0xD5, 0x32, 0x00};
    static const unsigned char kEntity[] = {0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x8B, 0xE9, 0x85, 0xFF, 0x0F, 0x84, 0xB3, 0x00, 0x00};
    static const unsigned char kSettingsRead[] = {0x8B, 0x2D, 0x38, 0x7C, 0x08, 0x02};
    static const unsigned char kFnVtableStore[] = {0xC7, 0x44, 0x24, 0x1C, 0x38, 0x73, 0xB2, 0x01};
    static const unsigned char kFlagsRead[] = {0x8B, 0x0D, 0xA4, 0xC0, 0x01, 0x02};
    static const unsigned char kValueReads[] = {0x8B, 0x4D, 0x0C, 0xF3, 0x0F, 0x10, 0x81, 0x14, 0x04, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0x44, 0x24, 0x24,
                                                0x3B, 0xC3, 0x74, 0x0A, 0xF3, 0x0F, 0x10, 0x80, 0xC8, 0x0B, 0x00, 0x00};
    uint32_t slot = 0;
    const bool hitOk = Bytes(kStasisHitRva, kHit, sizeof(kHit)) && Bytes(kStasisHitRva + 0xA6, kHitCall, sizeof(kHitCall));
    const bool entityOk = Bytes(kStasisEntityRva, kEntity, sizeof(kEntity));
    const bool vtableOk = Read(base + kStasisFnVtableRva + 4, &slot, 4) && slot == base + kStasisHitRva;
    const bool valuesOk = Bytes(kProcessCollisionRva + 5, kSettingsRead, sizeof(kSettingsRead)) && Bytes(kProcessCollisionRva + 0x90, kFnVtableStore, sizeof(kFnVtableStore)) &&
                          Bytes(kProcessCollisionRva + 0xB9, kFlagsRead, sizeof(kFlagsRead)) && Bytes(kProcessCollisionRva + 0xDA, kValueReads, sizeof(kValueReads));
    stasisReady = mineWanted && hitOk && entityOk && vtableOk && valuesOk;

    // SetHealth, through the slot hits::CallSetHealth checks on every call.
    static const unsigned char kSetHealth[] = {0xF3, 0x0F, 0x10, 0x1D, 0xD0, 0x5E, 0xAA, 0x01, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x04, 0x0F, 0x57};
    const bool setHealthOk = Bytes(kSetHealthRva, kSetHealth, sizeof(kSetHealth)) && kSetHealthRva == hits::kSetHealthRva;
    healReady = fieldWanted && setHealthOk;

    // The bodies: the turret's spawn, Item::Drop, registration update and Base::Delete.
    static const unsigned char kSpawn[] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x28, 0x53, 0x8B, 0x1D, 0xA0, 0xC0, 0x01, 0x02};
    static const unsigned char kDrop[] = {0x51, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x40, 0x02, 0x00, 0x00, 0x57, 0x85, 0xC0};
    static const unsigned char kRegistration[] = {0x83, 0xEC, 0x28, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x40, 0x02, 0x00, 0x00, 0xBB, 0x01, 0x00, 0x00, 0x00};
    static const unsigned char kDelete[] = {0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x06, 0x8B, 0x50, 0x08, 0x8B, 0xCE, 0xFF, 0xD2, 0x83, 0x3D};
    unsigned char deleteHead[16]{};
    const bool deleteOk = Read(base + turret::kBaseDeleteRva, deleteHead, sizeof(deleteHead)) &&
                          (std::memcmp(deleteHead, kDelete, sizeof(kDelete)) == 0 || (deleteHead[0] == 0xE9 && std::memcmp(deleteHead + 5, kDelete + 5, sizeof(kDelete) - 5) == 0));
    const bool spawnOk = Bytes(turret::kGlobalPickupRva, kSpawn, sizeof(kSpawn)), dropOk = Bytes(turret::kItemDropRva, kDrop, sizeof(kDrop));
    const bool registrationOk = Bytes(turret::kUpdateRegistrationRva, kRegistration, sizeof(kRegistration));
    bodyReady = spawnOk && dropOk && registrationOk && deleteOk;

    if (!bot_sense::losVerified) bot_sense::InstallLineOfSight(log);
    castOn = bot_sense::losVerified;
    if (!bot_nav::ready) bot_nav::Install(log);
    startsSeen = InterlockedCompareExchange(&loadsync::levelStarts, 0, 0);
    clearsSeen = InterlockedCompareExchange(&gate::restartClears, 0, 0);
    installed = stasisReady || healReady;
    std::fprintf(log, "gadgets %s | mine wanted=%d ready=%d key=%d trigger=%.1f radius=%.1f rise=%.1f recharge_s=%.0f arm_s=%.1f duration_s=%.1f slow=%.0f pickup=%.1f "
                      "(hit=%d stasis_entity=%d fn_vtable=%d values=%d) | field wanted=%d ready=%d key=%d duration_s=%.0f radius=%.1f rate=%.1f%%/s recharge_s=%.0f "
                      "tick_ms=%u (set_health=%d) | body wanted=%d ready=%d (spawn=%d drop=%d registration=%d delete=%d%s) mine_guid=%08X-%08X-%08X-%08X "
                      "field_guid=%08X-%08X-%08X-%08X | cast=%d nav=%d notice=%d\n",
                 installed ? "installed" : "OFF (no signature path matched)", mineWanted, stasisReady, mineKey, mineCfg.triggerMeters, mineCfg.radiusMeters,
                 mineCfg.maxRise, mineCfg.rechargeSeconds, mineCfg.armSeconds, mineCfg.durationSeconds, mineCfg.slowPercent, mineCfg.pickupRange, hitOk, entityOk,
                 vtableOk, valuesOk, fieldWanted, healReady, fieldKey, fieldCfg.durationSeconds, fieldCfg.radiusMeters, fieldCfg.ratePercent,
                 fieldCfg.rechargeSeconds, fieldCfg.tickMs, setHealthOk, bodyWanted, bodyReady, spawnOk, dropOk, registrationOk, deleteOk,
                 deleteHead[0] == 0xE9 ? ", detoured by another plugin" : "", mineGuid[0], mineGuid[1], mineGuid[2], mineGuid[3], fieldGuid[0], fieldGuid[1],
                 fieldGuid[2], fieldGuid[3], castOn, bot_nav::ready, notice::enabled && notice::installed);
    if (installed && !castOn) std::fprintf(log, "gadgets: without the camera cast nothing can be placed\n");

    // The rings use the turret's effect calls; their bytes are checked here too, so
    // they work with the turret off.
    bool createOk = false, findOk = false, softOk = false;
    const bool fxOk = turret::FxCoreOk(createOk, findOk, softOk);
    ringReady = installed && ringCfg.enabled && fxOk;
    std::fprintf(log, "gadgets ring wanted=%d ready=%d points=%d max_effects=%d per_second=%d pulse_s=%.1f fade_s=%.1f burst_s=%.1f lift=%.2f wall_check=%d "
                      "(create=%d find=%d soft_kill=%d) mine %08X-%08X burst %08X-%08X field %08X-%08X, stand-ins %08X-%08X %08X-%08X\n",
                 ringCfg.enabled, ringReady, ringCfg.points, ringCfg.maxEffects, ringCfg.perSecond, ringCfg.pulseSeconds, ringCfg.fadeSeconds, ringCfg.burstSeconds,
                 ringCfg.lift, ringCfg.wallCheck, createOk, findOk, softOk, mineRingGuid[0], mineRingGuid[3], burstRingGuid[0], burstRingGuid[3], fieldRingGuid[0],
                 fieldRingGuid[3], kStasisHologram[0], kStasisHologram[3], kHealHologram[0], kHealHologram[3]);
    return installed;
}

void WriteDiagnostics(FILE* log) {
    if (!mineWanted && !fieldWanted) return;
    static LONG last = -1;
    const LONG sum = minePresses + minePlaced + minePickedUp + mineForgotten + mineTriggers + mineStunned + mineSlowed + mineNoEffect + mineReadyAgain + fieldPresses +
                     fieldPlaced + fieldEnded + fieldReadyAgain + healSteps + healApplied + stasisFaults + healFaults + bodyFaults + tickFaults + castFaults + bodyGone +
                     bodyRearmed + ringBuilt + ringPlayed + ringAbsent + ringRated + ringCapped + ringEmpty + ringEnded + ringPulses + ringReleased + ringFaults;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "gadgets mine on=%d active=%d charge=%s %ds presses=%ld placed=%ld picked_up=%ld too_far=%ld recharging=%ld not_now=%ld forgotten=%ld "
                      "refunds=%ld refused[no_player=%ld no_floor=%ld too_high=%ld wall=%ld blocked=%ld occupied=%ld] scans=%ld triggers=%ld stunned=%ld slowed=%ld "
                      "already=%ld no_effect=%ld rejected=%ld no_manager=%ld ready_again=%ld stasis_faults=%ld\n",
                 mine.on ? 1 : 0, mineActive ? 1 : 0, coop_gadgets::ChargeName(mineCharge.state), mineCharge.SecondsLeft(), minePresses, minePlaced, minePickedUp,
                 mineTooFar, mineRecharging, mineNotNow, mineForgotten, mineRefunds, mineRefused[1], mineRefused[2], mineRefused[3], mineRefused[4], mineRefused[5],
                 mineRefused[6], mineScans, mineTriggers, mineStunned, mineSlowed, mineAlready, mineNoEffect, mineRejected, noManager, mineReadyAgain, stasisFaults);
    std::fprintf(log, "gadgets field on=%d active=%d charge=%s %ds left=%ds presses=%ld placed=%ld running=%ld recharging=%ld not_now=%ld forgotten=%ld ended=%ld "
                      "refused[no_player=%ld no_floor=%ld too_high=%ld wall=%ld blocked=%ld occupied=%ld] steps=%ld applied=%ld healed=%.1f full=%ld dead=%ld "
                      "partners=%ld outside=%ld no_effect=%ld ready_again=%ld heal_faults=%ld | body spawned=%ld removed=%ld gone=%ld rearmed=%ld faults=%ld | "
                      "casts=%ld cast_faults=%ld paused=%ld cutscene=%ld tick_faults=%ld | ring %s on=%d%d%d live=%d built=%ld played=%ld absent=%ld rated=%ld "
                      "capped=%ld empty=%ld walls=%ld drops=%ld ended=%ld pulses=%ld released=%ld faults=%ld\n",
                 field.on ? 1 : 0, fieldActive ? 1 : 0, coop_gadgets::ChargeName(fieldCharge.state), fieldCharge.SecondsLeft(), fieldLeft.SecondsLeft(), fieldPresses,
                 fieldPlaced, fieldRunning, fieldRecharging, fieldNotNow, fieldForgotten, fieldEnded, fieldRefused[1], fieldRefused[2], fieldRefused[3],
                 fieldRefused[4], fieldRefused[5], fieldRefused[6], healSteps, healApplied, static_cast<float>(healedCenti) / 100.0f, healFull, healDead,
                 healPartners, healOutside, healNoEffect, fieldReadyAgain, healFaults, bodySpawned, bodyRemoved, bodyGone, bodyRearmed, bodyFaults, castCalls,
                 castFaults, pausedFrames, cutsceneFrames, tickFaults, ringReady ? "ready" : "off", mineRing.on ? 1 : 0, burstRing.on ? 1 : 0, fieldRing.on ? 1 : 0,
                 RingsLive(), ringBuilt, ringPlayed, ringAbsent, ringRated, ringCapped, ringEmpty, ringWalls, ringDrops, ringEnded, ringPulses, ringReleased,
                 ringFaults);
}
}  // namespace gadgets
