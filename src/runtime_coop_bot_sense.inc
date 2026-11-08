// Dead Space 2 United by Hafer.
// What the companion bot knows about the world ([Bot], runtime_coop_bot.inc).
//
// One pass over the NetReflectable chain (0x0205B744, entity = node-0x5C,
// next at node+4; every enemy, item and door is on it) every kScanMs fills
// the brain's snapshot: enemies that are fighting (a target at npc+0x6F8, or
// an aware entry in the AwarenessComponent at [npc+0x71C]), the points on
// their limbs, pickups lying close to the group, and doors the leader is
// going through.
//
// Limbs come from the skeleton itself. Entity vtable +0xA0 is GetWorldBonePos
// (thiscall(float out[4], const BoneInfo&), ret 8; PC 0x0074DB90 for every
// Animated, it calls GetLocalBonePos at +0x9C and the entity's matrix), and a
// BoneInfo on PC is the bone index shifted left by two (Animated::GetBoneInfo,
// 0x0074D7A0, stores index*4; the two low bits are the locator and root
// flags). The bone count sits at AnimView (entity+0x194) +0xB0. Arms are the
// joints farthest out to either side between 45 and 95 per cent of the body's
// height, legs the ones between 15 and 40 per cent; the point aimed at is
// pulled a fifth of the way back toward the body, onto the forearm or the
// knee rather than a fingertip.
namespace turret { bool IsBody(uint32_t entity); }   // the placed turret's ammo box is no item for the bots
namespace gadgets { bool IsBody(uint32_t entity); }  // nor are the stasis mine's and heal field's packs
namespace mpnative { bool InVersus(); }              // runtime_coop_mpnative.inc: necromorph players are enemies there
namespace bot { bool NecroInGame(); }                // runtime_coop_bot.inc: a necro player in a story game
namespace bot_sense {
using coop_bot::Vec3;
constexpr uint32_t kReflectableListRva = 0x1C5B744, kEntityReflectable = 0x5C;
constexpr uint32_t kNpcTarget = 0x6F8, kNpcAwareness = 0x71C, kAwareCount = 0x720, kAwareFlags = 0x724, kAwareEntries = 0x30,
                   kAwareStride = 0x70, kAwareBit = 0x48;
constexpr uint32_t kWorldBonePosSlot = 0xA0, kBoneCount = 0xB0, kAnimView = 0x194;
constexpr uint32_t kStandardDoorVtableRva = 0x174D5D8, kDoorState = 0x234;
// A door counts as the way on only when the leader opened it: it started to open
// (state 1, 2 or 4) with him within kLeaderOpenRange. Doors that open on their
// own (scripts, enemies, a bot) once kept drawing the bots back to them.
constexpr float kLeaderOpenRange = 4.0f;
struct DoorSeen { uint32_t entity = 0; bool open = false, byLeader = false; };
DoorSeen doorsSeen[48];
unsigned doorsSeenNext = 0;
volatile LONG doorsByLeader = 0, doorsOnTheirOwn = 0, doorLogs = 0;
bool DoorOpenedByLeader(uint32_t entity, bool open, float leaderDistance) {
    DoorSeen* d = nullptr;
    for (auto& e : doorsSeen) if (e.entity == entity) { d = &e; break; }
    if (!d) {
        d = &doorsSeen[doorsSeenNext++ % std::size(doorsSeen)];
        *d = DoorSeen{entity, false, false};
    }
    if (open && !d->open) {
        d->byLeader = leaderDistance <= kLeaderOpenRange;
        InterlockedIncrement(d->byLeader ? &doorsByLeader : &doorsOnTheirOwn);
        if (InterlockedIncrement(&doorLogs) <= 40)
            gate::Event("bot: door %08X opens %s (leader %.1f m away)", entity, d->byLeader ? "for the leader" : "on its own: not the way on", leaderDistance);
    }
    if (!open) d->byLeader = false;
    d->open = open;
    return open && d->byLeader;
}
// PickupItem::m_pOwner (Xbox +0x260, PC +0x240; Item derives from PickupItem), a SafePtr: zero while it lies around.
constexpr uint32_t kItemOwner = 0x240, kHiddenCount = 0x16D;
// The nest (EARS::Modules::Nest, ch05/06/09/11): fixed to a wall, it fires volleys
// without a target of its own, so it never counts as engaged; it is taken at
// enemy range and the line of sight decides. Its pods are damage zones 1, 2 and 7.
constexpr uint32_t kNestVtableRva = 0x1785D40;
// The Guardian's tentacles are entities of their own (EARS::Modules::GuardianTentacle,
// 40 health each, no NPC), so the NPC walk never saw them and the bots shot the
// 30000-health body and its pods (06.10.). They are taken here as enemies.
constexpr uint32_t kGuardianTentacleVtableRva = 0x17859D8;
volatile LONG tentaclesSeen = 0;
volatile LONG nestsSeen = 0, nestSights = 0, nestPods = 0, nestLogLines = 0;
constexpr float kEnemyRange = 30.0f, kItemRange = 12.0f, kDoorRange = 14.0f, kLurkRange = 10.0f, kForcedRange = 45.0f;
// The leader's forced target ([Bot] AttackTargetKey): scanned whether it fights or not, a little farther out.
uint32_t forcedEnemy = 0;

bool Pos(uint32_t entity, Vec3& out) {
    float p[3]{};
    if (!entity || !Read(entity + 0xC0, p, 12)) return false;
    for (float v : p) if (!std::isfinite(v) || std::fabs(v) > 100000.0f) return false;
    out = {p[0], p[1], p[2]};
    return true;
}
float Yaw(uint32_t entity) {
    float f[3]{};
    if (!Read(entity + 0xB0, f, 12) || !std::isfinite(f[0]) || !std::isfinite(f[2])) return 0.0f;
    return std::atan2(f[0], f[2]);
}

volatile LONG boneExceptions = 0;   // of boneFaults, the ones that raised (the rest gave no finite position)
__declspec(noinline) bool WorldBonePos(uint32_t entity, uint32_t fn, uint32_t index, float out[4]) {
    uint32_t info = index << 2;
    __try {
        reinterpret_cast<void(__thiscall*)(void*, float*, const uint32_t*)>(fn)(reinterpret_cast<void*>(entity), out, &info);
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&boneExceptions);
        return false;
    }
}

volatile LONG boneFaults = 0, zoneReads = 0, guessedReads = 0, limbsKept = 0, headSkips = 0, volumeReads = 0, volumeMisses = 0, zoneSkips = 0,
              bonesFromVolume = 0, kindAimFallbacks = 0;
// Where to aim without a readable skeleton: the middle of a body of that kind.
// 1.2 m is over the head of an exploder (1.07 m) or a pack.
float KindAimHeight(coop_bot::EnemyKind kind) {
    using coop_bot::EnemyKind;
    switch (kind) {
    case EnemyKind::Swarm: return 0.2f;
    case EnemyKind::TubeBeetle: return 0.25f;
    case EnemyKind::Lurker: return 0.4f;
    case EnemyKind::Leaper: return 0.5f;
    case EnemyKind::Pack: return 0.55f;
    case EnemyKind::Exploder: return 0.6f;
    case EnemyKind::Infector: return 0.9f;
    default: return 1.2f;
    }
}
constexpr float kHeadClear = 0.35f;   // no aim point closer than this to the head
// Any part of the body, for when no limb point and not the chest has a clear
// line: the highest few first (a necromorph climbing over a railing shows its
// shoulders and head above it, 07.10.), the head last.
struct Parts {
    static constexpr int kMax = 6;
    Vec3 at[kMax];
    bool head[kMax]{};
    int count = 0;
    void Offer(const Vec3& p, bool isHead) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return;
        auto before = [&](int k) { return head[k] != isHead ? !isHead : p.y > at[k].y; };
        int slot = count < kMax ? count : -1;
        for (int k = 0; k < count; ++k)
            if (before(k)) { slot = k; break; }
        if (slot < 0) return;
        for (int k = (count < kMax ? count : kMax - 1); k > slot; --k) { at[k] = at[k - 1]; head[k] = head[k - 1]; }
        at[slot] = p;
        head[slot] = isHead;
        if (count < kMax) ++count;
    }
};

// The hit zones themselves. RagdollComponent (PC vtable RVA 0x174491C, owner at
// +0x10; found in a full dump, 27.09.) keeps one JointMap per physics body, the
// bodies bullets hit: array at +0x2C, count at +0x30, 0x2B0 bytes each (PC
// IsBoneDismemberable 0x007960C0 walks them). Per map: bone at +0xD0 (index << 2),
// parent map at +0x160, EA::DamageZone at +0x1B4 (0 head, 1/2 right/left front,
// 3/4 right/left back, 5 tail, 6 sac, 7-9 generic, 10 torso). Zone healths are
// the DismemberableHealthComponent's floats at +0x1F8, indexed by the zone.
constexpr uint32_t kRagdollVtableRva = 0x174491C, kJointMaps = 0x2C, kJointMapCount = 0x30, kJointMapSize = 0x2B0, kJointBone = 0xD0,
                   kJointParent = 0x160, kJointZone = 0x1B4;
constexpr uint32_t kZoneTorso = 10, kMaxJoints = 48, kZoneHealths = 0x1F8;
struct ComponentCache { uint32_t vtable = 0, ragdoll = 0, health = 0; DWORD retryAt = 0; };
ComponentCache componentCache[32];
// Offsets of the ragdoll and health pointers inside an actor, per actor class. A
// class without a ragdoll is looked for again after 2 s, not on every call (the
// 4 KB scan reads every pointer-like value in the actor).
bool ComponentOffsets(uint32_t entity, uint32_t vtable, uint32_t& ragdollAt, uint32_t& healthAt) {
    ComponentCache* slot = &componentCache[(vtable >> 3) % 32];
    const DWORD now = GetTickCount();
    if (slot->vtable == vtable && !slot->ragdoll && static_cast<LONG>(now - slot->retryAt) < 0) return false;
    if (slot->vtable != vtable || !slot->ragdoll) {
        ComponentCache found{vtable, 0, 0, 0};
        const uint32_t rag = static_cast<uint32_t>(base + kRagdollVtableRva);
        const uint32_t hp = static_cast<uint32_t>(base + hits::kHealthVtableRva), dis = static_cast<uint32_t>(base + hits::kDismemberableVtableRva);
        for (uint32_t offset = 4; offset < hits::kActorScanBytes && (!found.ragdoll || !found.health); offset += 4) {
            uint32_t candidate = 0, cv = 0, owner = 0;
            if (!Read(entity + offset, &candidate, 4) || candidate < 0x10000 || !Read(candidate, &cv, 4)) continue;
            if (cv != rag && cv != hp && cv != dis) continue;
            if (!Read(candidate + hits::kComponentOwner, &owner, 4) || owner != entity) continue;
            if (cv == rag) found.ragdoll = offset; else found.health = offset;
        }
        if (!found.ragdoll) {   // the ragdoll may come later; a good entry of another class is kept
            found.retryAt = now + 2000;
            if (!slot->ragdoll || slot->vtable == vtable) *slot = found;
            return false;
        }
        *slot = found;
    }
    ragdollAt = slot->ragdoll;
    healthAt = slot->health;
    return true;
}
// A component pointer at a cached offset, checked again (the class may share the slot with another).
uint32_t ComponentAt(uint32_t entity, uint32_t offset, uint32_t vtableRva) {
    uint32_t c = 0, v = 0, owner = 0;
    if (!offset || !Read(entity + offset, &c, 4) || c < 0x10000 || !Read(c, &v, 4) || !Read(c + hits::kComponentOwner, &owner, 4)) return 0;
    if (owner != entity || (vtableRva && v != base + vtableRva)) return 0;
    return c;
}

// The hit volume of a ragdoll body (Havok 6.5, read live): hkpRigidBody world rotation columns at +0xF0/+0x100/+0x110,
// origin at +0x120; shape at +0x10 with its type at +0x0C. A capsule (5): radius
// +0x10, ends +0x20 and +0x30 in body space; a translated sphere (11): offset
// +0x20, the sphere at +0x18 with its radius at +0x10. Living enemies' bodies are
// keyframed to the animation (a few cm behind), so this is what a bullet meets.
constexpr uint32_t kJointBody = 0x1B0, kBodyShape = 0x10, kBodyFrame = 0xF0, kShapeType = 0x0C, kShapeRadius = 0x10, kShapeA = 0x20, kShapeB = 0x30,
                   kShapeChild = 0x18;
constexpr uint32_t kShapeCapsule = 5, kShapeTranslated = 11;
bool HitVolume(uint32_t body, const Vec3& around, Vec3& center, float& size) {
    alignas(16) float m[16];
    uint32_t shape = 0, type = 0;
    if (!body || !Read(body + kBodyFrame, m, sizeof(m)) || !Read(body + kBodyShape, &shape, 4) || !shape || !Read(shape + kShapeType, &type, 4)) return false;
    const Vec3 c0 = {m[0], m[1], m[2]}, c1 = {m[4], m[5], m[6]}, c2 = {m[8], m[9], m[10]}, t = {m[12], m[13], m[14]};
    for (const Vec3& c : {c0, c1, c2})
        if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || std::fabs(coop_bot::Length(c) - 1.0f) > 0.05f) return false;
    if (!std::isfinite(t.x) || coop_bot::Distance(t, around) > 5.0f) return false;
    auto world = [&](const float* q) {
        return coop_bot::Add(t, coop_bot::Add(coop_bot::Scale(c0, q[0]), coop_bot::Add(coop_bot::Scale(c1, q[1]), coop_bot::Scale(c2, q[2]))));
    };
    float radius = 0.0f;
    if (type == kShapeCapsule) {
        float a[4]{}, b[4]{};
        if (!Read(shape + kShapeRadius, &radius, 4) || !Read(shape + kShapeA, a, 16) || !Read(shape + kShapeB, b, 16)) return false;
        const Vec3 wa = world(a), wb = world(b);
        center = coop_bot::Scale(coop_bot::Add(wa, wb), 0.5f);
        size = coop_bot::Distance(wa, wb) + 2.0f * radius;
    } else if (type == kShapeTranslated) {
        float off[4]{};
        uint32_t sphere = 0;
        if (!Read(shape + kShapeA, off, 16) || !Read(shape + kShapeChild, &sphere, 4) || !sphere || !Read(sphere + kShapeRadius, &radius, 4)) return false;
        center = world(off);
        size = 2.0f * radius;
    } else {
        return false;
    }
    return std::isfinite(center.x) && std::isfinite(size) && size > 0.0f && size < 4.0f && coop_bot::Distance(center, around) < 5.0f;
}

// Where a corpse really lies: the middle of its ragdoll bodies (their Havok
// frames, origin at body +0x120). A ragdoll may slide away from the entity's own
// matrix; without readable bodies the entity position stands in.
volatile LONG corpseCenters = 0, corpseCenterMisses = 0, corpseGapMaxCm = 0;
bool CorpseCenter(uint32_t entity, const Vec3& at, Vec3& out) {
    uint32_t vtable = 0, ragdollAt = 0, healthAt = 0, maps = 0, count = 0;
    out = at;
    if (!Read(entity, &vtable, 4) || !ComponentOffsets(entity, vtable, ragdollAt, healthAt)) { InterlockedIncrement(&corpseCenterMisses); return false; }
    const uint32_t ragdoll = ComponentAt(entity, ragdollAt, kRagdollVtableRva);
    if (!ragdoll || !Read(ragdoll + kJointMaps, &maps, 4) || !maps || !Read(ragdoll + kJointMapCount, &count, 4) || !count || count > kMaxJoints) {
        InterlockedIncrement(&corpseCenterMisses);
        return false;
    }
    Vec3 sum{};
    int n = 0;
    for (uint32_t j = 0; j < count && n < 16; ++j) {
        uint32_t body = 0;
        float t[3]{};
        if (!Read(maps + j * kJointMapSize + kJointBody, &body, 4) || !body || !Read(body + kBodyFrame + 0x30, t, sizeof(t))) continue;
        const Vec3 p = {t[0], t[1], t[2]};
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || coop_bot::Distance(p, at) > 8.0f) continue;
        sum = coop_bot::Add(sum, p);
        ++n;
    }
    if (!n) { InterlockedIncrement(&corpseCenterMisses); return false; }
    out = coop_bot::Scale(sum, 1.0f / static_cast<float>(n));
    InterlockedIncrement(&corpseCenters);
    const LONG gap = static_cast<LONG>(coop_bot::Flat(out, at) * 100.0f);
    if (gap > corpseGapMaxCm) InterlockedExchange(&corpseGapMaxCm, gap);
    return true;
}

// A class whose joint maps never gave a bone position (05.10.: zone_reads=0 and
// bone_faults=guessed_reads all session, every Zones call failed at its first
// joint) is not asked again for 30 s after three faults in a row: the skeleton
// guess follows anyway, and each fault may be an exception inside the game.
struct ZoneVerdict { uint32_t vtable = 0; unsigned faults = 0; bool worked = false; DWORD retryAt = 0; };
ZoneVerdict zoneVerdicts[16];
ZoneVerdict& ZoneVerdictOf(uint32_t vtable) {
    ZoneVerdict& v = zoneVerdicts[(vtable >> 3) % 16];
    if (v.vtable != vtable) v = ZoneVerdict{vtable};
    return v;
}
void NoteZoneFault(uint32_t vtable, uint32_t entity, uint32_t bone, uint32_t joint) {
    ZoneVerdict& v = ZoneVerdictOf(vtable);
    if (v.worked || ++v.faults < 3) return;
    v.faults = 0;
    v.retryAt = GetTickCount() + 30000;
    static volatile LONG lines = 0;
    if (InterlockedIncrement(&lines) <= 12)
        gate::Event("bot sense: hit zones of class %08X give no bone position (entity %08X joint %u bone value %08X, %ld exceptions so far): "
                    "limbs from the skeleton for 30 s", vtable - static_cast<uint32_t>(base), entity, joint, bone, boneExceptions);
}

// Up to four points on the hit zones that come off (arms, then tails, sacs and
// other appendages, then legs), each the middle of that zone's biggest body.
// Returns -1 when the actor has no readable ragdoll.
int Zones(uint32_t entity, uint32_t vtable, uint32_t fn, const Vec3& feet, Vec3* out, coop_bot::LimbKind* kinds, Vec3& chest, float& height, Parts& parts) {
    {
        const ZoneVerdict& v = ZoneVerdictOf(vtable);
        if (!v.worked && v.retryAt && static_cast<LONG>(GetTickCount() - v.retryAt) < 0) { InterlockedIncrement(&zoneSkips); return -1; }
    }
    uint32_t ragdollAt = 0, healthAt = 0;
    if (!ComponentOffsets(entity, vtable, ragdollAt, healthAt)) return -1;
    const uint32_t ragdoll = ComponentAt(entity, ragdollAt, kRagdollVtableRva);
    uint32_t maps = 0, count = 0;
    if (!ragdoll || !Read(ragdoll + kJointMaps, &maps, 4) || !Read(ragdoll + kJointMapCount, &count, 4) || !maps || count < 1 || count > kMaxJoints)
        return -1;
    float zoneHealth[10];
    bool healthKnown = false;
    if (const uint32_t health = ComponentAt(entity, healthAt, hits::kDismemberableVtableRva))
        healthKnown = Read(health + kZoneHealths, zoneHealth, sizeof(zoneHealth));
    struct Joint { uint32_t zone; int parent; float pos[4]; Vec3 volume; float size; bool haveVolume; };
    Joint joints[kMaxJoints];
    float top = feet.y;
    Vec3 torso{}, head{}, highest = feet;
    int torsoCount = 0, headCount = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t map = maps + i * kJointMapSize;
        uint32_t bone = 0, zone = 0;
        int32_t parent = -1;
        if (!Read(map + kJointBone, &bone, 4) || !Read(map + kJointZone, &zone, 4) || !Read(map + kJointParent, &parent, 4)) return -1;
        Joint& j = joints[i];
        j.zone = zone;
        j.parent = parent >= 0 && static_cast<uint32_t>(parent) < count ? parent : -1;
        uint32_t body = 0;
        j.haveVolume = Read(map + kJointBody, &body, 4) && HitVolume(body, feet, j.volume, j.size);
        if (j.haveVolume) InterlockedIncrement(&volumeReads); else InterlockedIncrement(&volumeMisses);
        // The bone query can fault (06.10.: exploders, slashers, pukers); the physics
        // body is a position too, and it is where the bullets land.
        if (!WorldBonePos(entity, fn, bone >> 2, j.pos)) {
            if (!j.haveVolume) { InterlockedIncrement(&boneFaults); NoteZoneFault(vtable, entity, bone, i); return -1; }
            j.pos[0] = j.volume.x; j.pos[1] = j.volume.y; j.pos[2] = j.volume.z; j.pos[3] = 1.0f;
            InterlockedIncrement(&bonesFromVolume);
        }
        const Vec3 mark = j.haveVolume ? j.volume : Vec3{j.pos[0], j.pos[1], j.pos[2]};
        if (j.pos[1] > top) { top = j.pos[1]; highest = mark; }
        if (zone == kZoneTorso) { torso = coop_bot::Add(torso, mark); ++torsoCount; }
        if (zone == 0) { head = coop_bot::Add(head, mark); ++headCount; }
    }
    height = top - feet.y;
    if (torsoCount) chest = coop_bot::Scale(torso, 1.0f / static_cast<float>(torsoCount));
    for (uint32_t i = 0; i < count; ++i)
        parts.Offer(joints[i].haveVolume ? joints[i].volume : Vec3{joints[i].pos[0], joints[i].pos[1], joints[i].pos[2]}, joints[i].zone == 0);
    // Never at the head, even through a limb that reaches up to it (a hand raised
    // before the face, an appendage on the head): the head zone's joints, else the
    // highest joint.
    head = headCount ? coop_bot::Scale(head, 1.0f / static_cast<float>(headCount)) : highest;
    // Per zone: the segment farthest out from the body (a forearm, a shin), so a
    // shot a little off still hits the limb and not the chest (28.09.: the longest
    // segment was often the upper arm right beside the chest). Each segment runs
    // from its bone to its child in the same zone; the last one in a chain reaches
    // on past its bone the way its parent points.
    struct Pick { float length = -1.0f, score = -1.0f; Vec3 point; };
    Pick best[10];
    for (uint32_t i = 0; i < count; ++i) {
        const Joint& j = joints[i];
        if (j.zone < 1 || j.zone > 9) continue;
        if (healthKnown && std::isfinite(zoneHealth[j.zone]) && zoneHealth[j.zone] <= 0.0f) continue;   // already off
        const Vec3 at = {j.pos[0], j.pos[1], j.pos[2]};
        Vec3 point = at;
        float length = 0.0f;
        bool child = j.haveVolume;   // with its body read, the body's own middle is the point
        if (j.haveVolume) { point = j.volume; length = j.size; }
        for (uint32_t k = 0; k < count && !j.haveVolume; ++k) {
            if (joints[k].parent != static_cast<int>(i) || joints[k].zone != j.zone) continue;
            const Vec3 next = {joints[k].pos[0], joints[k].pos[1], joints[k].pos[2]};
            const float l = coop_bot::Distance(at, next);
            if (l > length) { length = l; point = coop_bot::Scale(coop_bot::Add(at, next), 0.5f); }
            child = true;
        }
        if (!child && j.parent >= 0) {
            const Vec3 from = {joints[j.parent].pos[0], joints[j.parent].pos[1], joints[j.parent].pos[2]};
            const Vec3 along = coop_bot::Sub(at, from);
            point = coop_bot::Add(at, coop_bot::Scale(along, 0.4f));
            length = coop_bot::Length(along) * 0.8f;
        }
        if (coop_bot::Distance(point, head) < kHeadClear) { InterlockedIncrement(&headSkips); continue; }
        const float score = length >= 0.12f ? coop_bot::Distance(point, chest) : 0.0f;
        if (score > best[j.zone].score || (score == best[j.zone].score && length > best[j.zone].length)) {
            best[j.zone].length = length;
            best[j.zone].score = score;
            best[j.zone].point = point;
        }
    }
    // Small ones (packs, often crouched) are shot in the middle, not in the legs
    // or arms; what sticks out of them (a lurker's tentacles) still counts. A
    // nest's pods are its front zones: always taken, whatever its height.
    const bool small = height < 1.3f && vtable != base + kNestVtableRva;
    int n = 0;
    static const uint32_t kOrder[] = {1, 2, 5, 6, 7, 8, 9, 3, 4};
    for (uint32_t zone : kOrder) {
        if (n >= coop_bot::kMaxLimbs || best[zone].length < 0.0f) continue;
        const bool limb = zone <= 4;
        if (small && limb) continue;
        kinds[n] = zone >= 3 && zone <= 4 ? coop_bot::LimbKind::Leg : coop_bot::LimbKind::Arm;
        out[n++] = best[zone].point;
    }
    ZoneVerdictOf(vtable).worked = true;
    return n;
}

// Up to four limb points (arms first, then legs), best first. height is feet to
// the highest bone (0 when the skeleton cannot be read).
int Limbs(uint32_t entity, const Vec3& feet, Vec3* out, coop_bot::LimbKind* kinds, Vec3& chest, float& height, Parts& parts) {
    uint32_t vtable = 0, fn = 0, view = 0, count = 0;
    parts = Parts{};
    chest = {feet.x, feet.y + 1.2f, feet.z};
    height = 0.0f;
    if (!Read(entity, &vtable, 4) || !Read(vtable + kWorldBonePosSlot, &fn, 4) || fn < base + 0x1000 || fn >= base + gate::kRdataLo) return 0;
    const int zones = Zones(entity, vtable, fn, feet, out, kinds, chest, height, parts);
    if (zones >= 0) { InterlockedIncrement(&zoneReads); return zones; }
    parts = Parts{};
    InterlockedIncrement(&guessedReads);
    height = 0.0f;
    chest = {feet.x, feet.y + 1.2f, feet.z};
    if (!Read(entity + kAnimView, &view, 4) || !view || !Read(view + kBoneCount, &count, 4) || count < 4 || count > 200) return 0;
    alignas(16) float bones[200][4];
    unsigned have = 0;
    float top = feet.y;
    Vec3 head = feet;   // the highest bone stands in for the head here
    for (uint32_t i = 0; i < count; ++i) {
        if (!WorldBonePos(entity, fn, i, bones[have])) { InterlockedIncrement(&boneFaults); return 0; }
        if (std::fabs(bones[have][0] - feet.x) > 6.0f || std::fabs(bones[have][2] - feet.z) > 6.0f) continue;   // stray locator
        if (bones[have][1] > top) { top = bones[have][1]; head = {bones[have][0], bones[have][1], bones[have][2]}; }
        ++have;
    }
    if (!have) return 0;
    height = top - feet.y;
    for (unsigned i = 0; i < have; ++i) {
        const Vec3 bone = {bones[i][0], bones[i][1], bones[i][2]};
        parts.Offer(bone, coop_bot::Distance(bone, head) < kHeadClear);
    }
    // Small ones and crawlers (a lurker on a wall is not upright): the middle of
    // the skeleton itself, not a point above the feet.
    constexpr float kSmall = 1.3f, kMinLegHeight = 0.35f;
    if (height < kSmall) {
        Vec3 sum{};
        for (unsigned i = 0; i < have; ++i) sum = coop_bot::Add(sum, {bones[i][0], bones[i][1], bones[i][2]});
        chest = coop_bot::Scale(sum, 1.0f / static_cast<float>(have));
        return 0;
    }
    chest = {feet.x, feet.y + height * 0.65f, feet.z};
    const float yaw = Yaw(entity);
    const Vec3 right = {std::cos(yaw), 0.0f, -std::sin(yaw)};
    struct Best { float lateral = 0.0f; int index = -1; };
    Best arm[2], leg[2];
    for (unsigned i = 0; i < have; ++i) {
        const float h = (bones[i][1] - feet.y) / height;
        const Vec3 off = {bones[i][0] - feet.x, 0.0f, bones[i][2] - feet.z};
        const float side = coop_bot::Dot(off, right);
        const float lateral = std::fabs(side);
        Best* band = h >= 0.45f && h <= 0.95f ? arm : (h >= 0.15f && h <= 0.40f ? leg : nullptr);
        if (!band) continue;
        Best& b = band[side < 0.0f ? 0 : 1];
        if (lateral > b.lateral) { b.lateral = lateral; b.index = static_cast<int>(i); }
    }
    // Small ones (packs, often crouched) returned above with the middle of the body
    // as the target: their leg joints sit a hand above the floor, a shot a little
    // low hits the ground. Nobody else's leg point below kMinLegHeight counts either.
    int n = 0;
    for (const Best* group : {arm, leg})
        for (int s = 0; s < 2; ++s) {
            const Best& b = group[s];
            if (b.index < 0 || b.lateral < 0.15f) continue;
            const float* p = bones[b.index];
            if (group == leg && p[1] - feet.y < kMinLegHeight) continue;
            const Vec3 axis = {feet.x, p[1], feet.z};
            const Vec3 point = coop_bot::Add({p[0], p[1], p[2]}, coop_bot::Scale(coop_bot::Sub(axis, {p[0], p[1], p[2]}), 0.2f));
            if (coop_bot::Distance(point, head) < kHeadClear) { InterlockedIncrement(&headSkips); continue; }
            kinds[n] = group == arm ? coop_bot::LimbKind::Arm : coop_bot::LimbKind::Leg;
            out[n++] = point;
        }
    return n;
}

// Limbs shot off: DismemberableHealthComponent keeps one health per damage zone
// (+0x1F8, 10 floats; PC IsZoneDestroyed 0x00F640F0: destroyed at <= 0). Zones 1-4
// are the right and left front (arms) and back (legs); 0 is the head.
// A limb that grows back (the Ubermorph) drops to 0 when it comes off, restarts at
// a sliver and climbs (28.09.: arm 1 and leg 3 of 30). It counts as off from the
// moment it hits 0 until it has stopped growing for kRegrownMs. A limb only shot
// at, never off, is not off however low it is (28.09.: a quarter-of-full rule
// kept wounded limbs "off" for good, so after a full regrowth the bots never shot
// the Ubermorph again).
constexpr DWORD kRegrownMs = 1500;
struct ZoneMemory { uint32_t entity = 0; DWORD at = 0; float last[5]{}; DWORD grewAt[5]{}; bool severed[5]{}; };
ZoneMemory zoneMemory[8];
int LimbsLost(uint32_t entity, uint32_t health) {
    uint32_t vtable = 0;
    float zones[5]{};
    if (!health || !Read(health, &vtable, 4) || vtable != base + hits::kDismemberableVtableRva || !Read(health + kZoneHealths, zones, sizeof(zones)))
        return -1;
    const DWORD now = GetTickCount();
    ZoneMemory* m = nullptr;
    for (ZoneMemory& z : zoneMemory) if (z.entity == entity) { m = &z; break; }
    if (!m) {
        for (ZoneMemory& z : zoneMemory) if (!z.entity || now - z.at > 60000) { m = &z; break; }
        if (!m) m = &zoneMemory[entity % 8];
        *m = ZoneMemory{entity};
        for (int i = 1; i <= 4; ++i) m->last[i] = zones[i];
    }
    m->at = now;
    int lost = 0;
    for (int i = 1; i <= 4; ++i) {
        const float z = std::isfinite(zones[i]) ? zones[i] : 0.0f;
        if (z <= 0.0f) { m->severed[i] = true; m->grewAt[i] = now; }
        else if (z > m->last[i] + 0.01f) m->grewAt[i] = now;   // still growing back
        else if (m->severed[i] && now - m->grewAt[i] >= kRegrownMs) m->severed[i] = false;   // done growing
        m->last[i] = z;
        if (m->severed[i]) ++lost;
    }
    return lost;
}

// The Ubermorph (Hunter, vtable 0x01B6EAD8) down and regrowing. Its
// CadaverousRegenerationSM (vtable 0x01B6B568, id EB5658F1 at +8, NPC at +0x6C)
// sets bit 4 ("Dismembering") of npc+0x874 in its ctor (0x00FFDD1E) and its dtors
// clear it (0x0100EF66, 0x01014F56); its states (+0x0C) are 0 init, 1 dormantStart,
// 2 preRegenerate, 3 regenerate (Undismember on entry), 4 dormantFall, 5
// dormantEnd (the get-up), 6 exit. Regenerating = the bit, or that machine in a
// state below 6. Read from the Xbox build and the PC code; no dump holds a Hunter.
// Primary vtables of the enemy classes (analysis/ds2_classmap_live_2026-09-17.json):
// the brain lets more bots shoot the tough ones at once (FocusCap).
coop_bot::EnemyKind KindOf(uint32_t vtable) {
    using coop_bot::EnemyKind;
    static constexpr struct { uint32_t rva; EnemyKind kind; } kKinds[] = {
        {0x1783948u, EnemyKind::Slasher},     {0x1786DF0u, EnemyKind::Pack},        {0x17864F8u, EnemyKind::Swarm},
        {0x1788B10u, EnemyKind::TubeBeetle},  {0x176C400u, EnemyKind::Leaper},      {0x176CA40u, EnemyKind::Lurker},
        {0x1785378u, EnemyKind::Exploder},    {0x176BE80u, EnemyKind::Infector},    {0x1789018u, EnemyKind::Puker},
        {0x178CCA8u, EnemyKind::Stalker},     {0x176D428u, EnemyKind::Brute},       {0x176EAD8u, EnemyKind::Hunter},
        {0x1784410u, EnemyKind::DividerCore}, {0x1784930u, EnemyKind::DividerPart}, {0x1789F90u, EnemyKind::Guardian},
        {0x1773CE8u, EnemyKind::Boss},        {0x1774B78u, EnemyKind::Boss},        {kNestVtableRva, EnemyKind::Nest},
    };
    for (const auto& k : kKinds) if (vtable == base + k.rva) return k.kind;
    return EnemyKind::Unknown;
}
constexpr uint32_t kHunterVtableRva = 0x176EAD8, kRegenSmVtableRva = 0x176B568, kRegenSmId = 0xEB5658F1, kNpcFlags = 0x874, kDismembering = 4;
volatile LONG regenFlagOnly = 0, regenSmOnly = 0, regenBoth = 0, regenLines = 0;
struct RegenSeen { uint32_t entity = 0; bool on = false; DWORD since = 0; };
RegenSeen regenSeen[4];
bool Regenerating(uint32_t entity, uint32_t vtable) {
    if (vtable != base + kHunterVtableRva) return false;
    uint32_t flags = 0;
    const bool flag = Read(entity + kNpcFlags, &flags, 4) && (flags & kDismembering);
    int32_t state = -1;
    {
        uint32_t stack[64], rootSm = 0;
        int n = 0, visited = 0;
        if (Read(entity + 0x24C, &rootSm, 4) && rootSm) stack[n++] = rootSm;
        while (n && visited < 256) {
            const uint32_t sm = stack[--n];
            ++visited;
            uint32_t smVtable = 0, id = 0, npc = 0, child = 0, sibling = 0;
            if (!Read(sm, &smVtable, 4)) continue;
            if (smVtable == base + kRegenSmVtableRva && Read(sm + 8, &id, 4) && id == kRegenSmId && Read(sm + 0x6C, &npc, 4) && npc == entity) {
                if (!Read(sm + 0x0C, &state, 4)) state = -1;
                break;
            }
            if (Read(sm + 0x20, &child, 4) && child && n < 64) stack[n++] = child;
            if (Read(sm + 0x28, &sibling, 4) && sibling && n < 64) stack[n++] = sibling;
        }
    }
    const bool machine = state >= 0 && state <= 5;
    const bool on = flag || machine;
    if (on) InterlockedIncrement(flag && machine ? &regenBoth : (flag ? &regenFlagOnly : &regenSmOnly));
    // Each change per Hunter, logged with how long the last phase lasted.
    RegenSeen* slot = nullptr;
    for (RegenSeen& r : regenSeen) if (r.entity == entity) { slot = &r; break; }
    if (!slot) { slot = &regenSeen[entity % 4]; *slot = RegenSeen{entity, false, GetTickCount()}; }
    if (slot->on != on) {
        const DWORD now = GetTickCount();
        if (InterlockedIncrement(&regenLines) <= 60)
            gate::Event("bot sense: Hunter %08X %s (flag %d, machine state %d; %.1f s after the last change)", entity,
                        on ? "is down regrowing: no shots" : "stands again: shots allowed", flag ? 1 : 0, state,
                        static_cast<float>(now - slot->since) / 1000.0f);
        slot->on = on;
        slot->since = now;
    }
    return on;
}

// Fighting: it has a target, or is aware of anything.
bool Engaged(uint32_t npc) {
    uint32_t target = 0, comp = 0, count = 0;
    unsigned char flags = 0;
    if (Read(npc + kNpcTarget, &target, 4) && target > 0x10010) return true;
    if (!Read(npc + kNpcAwareness, &comp, 4) || !comp || !Read(comp + kAwareFlags, &flags, 1) || !(flags & 2) ||
        !Read(comp + kAwareCount, &count, 4) || !count || count > 32) return false;
    for (uint32_t i = 0; i < count; ++i) {
        unsigned char aware = 0;
        if (Read(comp + kAwareEntries + i * kAwareStride + kAwareBit, &aware, 1) && (aware & 1)) return true;
    }
    return false;
}

// Line of sight: CollisionCamera::CastingTest (PC 0x00F5B180, cdecl, returns
// the free fraction of the way in st0; Xbox ?CastingTest@CollisionCamera: from,
// to, excluded entity, excluded UIDs, their count, hit point out, hit entity
// out), the sphere cast the cameras use against the world. The bot's own body
// is excluded; a hit on the enemy itself counts as seen.
constexpr uint32_t kCastingTestRva = 0xB5B180;
bool losReady = false;
bool losVerified = false;   // the signature matched; only then may faults switch it off and a new level on again
volatile LONG losCasts = 0, losBlocked = 0, losFaults = 0, losDisables = 0, losReenables = 0;
bool InstallLineOfSight(FILE* log) {
    // push ebp / mov ebp,esp / and esp,-16 / sub esp,1ACh / fld dword [01B625A8h]
    static const unsigned char kSig[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xAC, 0x01, 0x00, 0x00, 0xD9, 0x05, 0xA8, 0x25, 0xB6, 0x01};
    unsigned char actual[sizeof(kSig)]{};
    losReady = Read(base + kCastingTestRva, actual, sizeof(actual)) && std::memcmp(actual, kSig, sizeof(kSig)) == 0;
    losVerified = losReady;
    std::fprintf(log, "bot line_of_sight=%d (CollisionCamera::CastingTest at %08X%s)\n", losReady ? 1 : 0, static_cast<unsigned>(base + kCastingTestRva),
                 losReady ? "" : ", signature mismatch: enemies count as seen when they are on our level");
    return losReady;
}
// One faulted cast is not the end of it (the review of 07.10.: a single fault
// turned the sight off for the session, after which every test passed). Off only
// after kLosFaultLimit faults within kLosFaultWindowMs; back on at the next level.
constexpr int kLosFaultLimit = 5;
constexpr DWORD kLosFaultWindowMs = 10000;
DWORD losFaultTimes[kLosFaultLimit]{};
unsigned losFaultNext = 0;
bool losFaultOff = false;
void NoteLosFault() {
    InterlockedIncrement(&losFaults);
    const DWORD now = GetTickCount();
    losFaultTimes[losFaultNext++ % kLosFaultLimit] = now ? now : 1u;
    for (DWORD at : losFaultTimes)
        if (!at || now - at > kLosFaultWindowMs) return;
    if (!losReady) return;
    losReady = false;
    losFaultOff = true;
    InterlockedIncrement(&losDisables);
    gate::Event("bot sense: line of sight off after %d faulted casts within %lu ms (%ld faults in all); enemies count as seen until the next level",
                kLosFaultLimit, static_cast<unsigned long>(kLosFaultWindowMs), losFaults);
}
void ReenableLineOfSight(const char* why) {
    if (!losVerified || !losFaultOff) return;
    losFaultOff = false;
    losReady = true;
    for (DWORD& at : losFaultTimes) at = 0;
    InterlockedIncrement(&losReenables);
    gate::Event("bot sense: line of sight on again (%s)", why);
}
__declspec(noinline) bool CastRaw(const float* from, const float* to, uint32_t exclude, float& fraction, uint32_t& hit) {
    using Cast = float(__cdecl*)(const float*, const float*, const void*, const uint32_t*, uint32_t, float*, uint32_t*);
    alignas(16) float at[4]{};
    __try {
        fraction = reinterpret_cast<Cast>(base + kCastingTestRva)(from, to, reinterpret_cast<const void*>(static_cast<uintptr_t>(exclude)), nullptr, 0,
                                                                   at, &hit);
        return std::isfinite(fraction);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Free from the bot's eye to a point on the target (a little short of it, so the
// target's own surface does not count as a wall).
bool Clear(const Vec3& eye, const Vec3& point, uint32_t self, uint32_t target) {
    if (!losReady) return true;   // CastingTest unverified or faulted: never call it
    const Vec3 d = coop_bot::Sub(point, eye);
    const float len = coop_bot::Length(d);
    if (len < 0.5f) return true;
    const Vec3 end = coop_bot::Add(eye, coop_bot::Scale(d, (len - 0.3f) / len));
    alignas(16) float from[4] = {eye.x, eye.y, eye.z, 1.0f}, to[4] = {end.x, end.y, end.z, 1.0f};
    float fraction = 1.0f;
    uint32_t hit = 0;
    InterlockedIncrement(&losCasts);
    if (!CastRaw(from, to, self, fraction, hit)) { NoteLosFault(); return true; }
    if (fraction >= 0.999f || (hit && hit == target)) return true;
    InterlockedIncrement(&losBlocked);
    return false;
}

// Which limbs the bot can really hit from where it stands. A ray that meets the
// enemy well before the limb has the enemy's own body in the way (an arm behind
// the chest counted as "seen" before, since the ray hit the enemy); and a limb on
// the far side of the body, seen from the bot, is the worse pick even when a ray
// scrapes past to it. So: the limbs on the side facing the bot (from the enemy's
// middle toward the bot) with a clear ray first, nearest side first; the others
// only when there is nothing better. Facing the bot, both sides are open; from its
// right, its right arm and leg.
enum class Sight : uint8_t { Clear, Body, Blocked };
constexpr float kBodyDepth = 0.4f, kFarSide = -0.08f;
volatile LONG limbsNear = 0, limbsFar = 0, limbsBehindBody = 0;
Sight LimbSight(const Vec3& eye, const Vec3& point, uint32_t self, uint32_t target) {
    const Vec3 d = coop_bot::Sub(point, eye);
    const float len = coop_bot::Length(d);
    if (!losReady || len < 0.5f) return Sight::Clear;
    alignas(16) float from[4] = {eye.x, eye.y, eye.z, 1.0f}, to[4] = {point.x, point.y, point.z, 1.0f};
    float fraction = 1.0f;
    uint32_t hit = 0;
    InterlockedIncrement(&losCasts);
    if (!CastRaw(from, to, self, fraction, hit)) { NoteLosFault(); return Sight::Clear; }
    if (fraction >= 0.999f) return Sight::Clear;
    const float at = fraction * len;
    if (hit && hit == target) return at >= len - kBodyDepth ? Sight::Clear : Sight::Body;
    if (at >= len - 0.3f) return Sight::Clear;
    InterlockedIncrement(&losBlocked);
    return Sight::Blocked;
}
void PickExposed(const Vec3& eye, uint32_t self, uint32_t target, const Vec3& chest, const Vec3* limbs, const coop_bot::LimbKind* kinds, int found,
                 coop_bot::Enemy& e) {
    struct Candidate { int index; float exposure; Sight sight; };
    Candidate c[coop_bot::kMaxLimbs];
    Vec3 toBot = {eye.x - chest.x, 0.0f, eye.z - chest.z};
    const float span = coop_bot::Length(toBot);
    toBot = span > 0.05f ? coop_bot::Scale(toBot, 1.0f / span) : Vec3{};
    int n = 0;
    for (int i = 0; i < found && n < coop_bot::kMaxLimbs; ++i) {
        const Vec3 out = {limbs[i].x - chest.x, 0.0f, limbs[i].z - chest.z};
        c[n++] = Candidate{i, coop_bot::Dot(out, toBot), LimbSight(eye, limbs[i], self, target)};
    }
    for (int a = 1; a < n; ++a)   // nearest side first
        for (int b = a; b > 0 && c[b].exposure > c[b - 1].exposure; --b) std::swap(c[b], c[b - 1]);
    e.limbCount = 0;
    for (int i = 0; i < n; ++i)
        if (c[i].sight == Sight::Clear && c[i].exposure >= kFarSide) {
            e.limbKind[e.limbCount] = kinds[c[i].index];
            e.limbs[e.limbCount++] = limbs[c[i].index];
            if (c[i].exposure > 0.05f) InterlockedIncrement(&limbsNear);
        } else if (c[i].sight == Sight::Body) {
            InterlockedIncrement(&limbsBehindBody);
        } else if (c[i].sight == Sight::Clear) {
            InterlockedIncrement(&limbsFar);
        }
    if (e.limbCount) return;
    for (int i = 0; i < n; ++i)   // nothing open: whatever is not behind a wall, nearest side first
        if (c[i].sight != Sight::Blocked) { e.limbKind[e.limbCount] = kinds[c[i].index]; e.limbs[e.limbCount++] = limbs[c[i].index]; }
}
// No limb point and not the chest in sight: the first parts of the body with a
// clear line become the points to shoot at (at most kPartRays rays, two points,
// within kPartRange: the rays cost frame time).
// The limb points sit on forearms and shins, the chest in the middle; over a
// railing only the shoulders or the head may show. Nests and the Guardian keep
// their own rules (pods and tentacles only).
constexpr int kPartRays = 3;
constexpr float kPartRange = 15.0f;
volatile LONG partTries = 0, partSights = 0, partRays = 0;
bool ExposedParts(const Vec3& eye, uint32_t self, uint32_t target, const Parts& parts, coop_bot::Enemy& e) {
    if (!losReady || !parts.count || e.kind == coop_bot::EnemyKind::Nest || e.kind == coop_bot::EnemyKind::Guardian ||
        coop_bot::Distance(eye, e.pos) > kPartRange)
        return false;
    InterlockedIncrement(&partTries);
    e.limbCount = 0;
    for (int k = 0; k < parts.count && k < kPartRays && e.limbCount < 2; ++k) {
        InterlockedIncrement(&partRays);
        if (LimbSight(eye, parts.at[k], self, target) != Sight::Clear) continue;
        e.limbKind[e.limbCount] = coop_bot::LimbKind::Arm;
        e.limbs[e.limbCount++] = parts.at[k];
    }
    if (!e.limbCount) return false;
    InterlockedIncrement(&partSights);
    return true;
}

// How fast an enemy walks, from where it stood on earlier scans (flat, m/s).
struct Track { uint32_t entity = 0; Vec3 pos; DWORD at = 0; Vec3 vel; };
Track tracks[32];
Vec3 Velocity(uint32_t entity, const Vec3& pos) {
    const DWORD now = GetTickCount();
    Track* slot = nullptr;
    for (Track& t : tracks) if (t.entity == entity) { slot = &t; break; }
    if (!slot) {
        for (Track& t : tracks) if (!t.entity || now - t.at > 5000) { slot = &t; break; }
        if (!slot) slot = &tracks[entity % 32];
        *slot = Track{entity, pos, now, {}};
        return {};
    }
    const float dt = static_cast<float>(now - slot->at) / 1000.0f;
    if (dt >= 0.15f) {
        if (dt <= 1.5f) {
            const Vec3 v = coop_bot::Scale(coop_bot::Sub(pos, slot->pos), 1.0f / dt);
            slot->vel = coop_bot::Add(coop_bot::Scale(slot->vel, 0.5f), coop_bot::Scale({v.x, 0.0f, v.z}, 0.5f));   // a little smoothing
        } else {
            slot->vel = {};
        }
        slot->pos = pos;
        slot->at = now;
    }
    return slot->vel;
}

// The finale's Marker heart. The Nicole boss (NPC vtable RVA 0x1774B78) runs a
// NicoleBossSM (vtable RVA 0x1774EC8, found 28.09. live: m_pNicoleBoss +0x6C,
// m_pMarkerHeart SafePtr +0xA0 = DamageableProp (vtable RVA 0x175E678) + 0x10)
// in its machine tree (root [actor+0x24C], child +0x20, sibling +0x28). It is no
// enemy the NPC scan knows, so it is handed to the brain as an objective.
constexpr uint32_t kNicoleBossVtableRva = 0x1774B78, kNicoleBossSmVtableRva = 0x1774EC8, kMarkerHeartRef = 0xA0, kDamageablePropVtableRva = 0x175E678;
constexpr float kObjectiveRange = 80.0f;   // the heart opens some 50 m from where Nicole fought
// A hidden entity (its hide count at +0x16D above zero) is not there for the
// player: Nicole between her phases, faded out while the heart is open (28.09.:
// the bots kept shooting the spot where she vanished). Nobody aims at it.
volatile LONG hiddenSkips = 0, nicoleShown = -1;
// A necromorph placed to play dead (SPAWNMODE_ZOMBIE_PLAYDEAD in the catalog, or
// one of the pool's play-dead donors; the entity carries that GUID at +0x24) is a
// corpse to the bots until it springs up: no target, no threat, no aim through a
// wall at it (06.10.). Up means it moved off the spot it was first seen on, or it
// lost health (somebody shot it).
constexpr float kWakeDistance = 0.75f;
volatile LONG playdeadSkips = 0, playdeadWakes = 0, playdeadLines = 0;
bool PlaydeadGuid(const uint32_t* g) {
    static std::vector<::ds2::Guid> guids;
    static bool built = false;
    if (!built) {
        built = true;
        for (const auto& row : ::ds2::kEnemyCatalog)
            if (row.context && std::strstr(row.context, "PLAYDEAD")) guids.push_back(row.guid);
        for (const auto& row : ::ds2::kPlaydeadDonors) guids.push_back(row.guid);
    }
    for (const auto& guid : guids)
        if (guid[0] == g[0] && guid[1] == g[1] && guid[2] == g[2] && guid[3] == g[3]) return true;
    return false;
}
// A Severed twitcher is one of the pool's slasher copies named Twitcher* (generated_variant_donors.h);
// the entity carries the copy's GUID at +0x24. It dodges an aim after 0.25 s (coop_bot_brain.h dodger).
volatile LONG twitcherScans = 0, twitcherRows = -1;
bool TwitcherGuid(uint32_t entity) {
    static std::vector<::ds2::Guid> guids;
    static bool built = false;
    if (!built) {
        built = true;
        for (const auto& row : ::ds2::kVariantDonors)
            if (row.name && std::strncmp(row.name, "Twitcher", 8) == 0) guids.push_back(row.guid);
        InterlockedExchange(&twitcherRows, static_cast<LONG>(guids.size()));
    }
    uint32_t g[4]{};
    if (guids.empty() || !Read(entity + 0x24, g, sizeof(g))) return false;
    for (const auto& guid : guids)
        if (guid[0] == g[0] && guid[1] == g[1] && guid[2] == g[2] && guid[3] == g[3]) { InterlockedIncrement(&twitcherScans); return true; }
    return false;
}
struct Lying { uint32_t entity = 0; Vec3 at; bool woke = false; DWORD seen = 0; };
Lying lying[24];
// True while it still lies where it was placed, untouched.
bool PlayingDead(uint32_t entity, const Vec3& at, float current, float max) {
    uint32_t guid[4]{};
    if (!Read(entity + 0x24, guid, sizeof(guid)) || !PlaydeadGuid(guid)) return false;
    const DWORD now = GetTickCount();
    Lying* slot = nullptr;
    Lying* oldest = &lying[0];
    for (Lying& l : lying) {
        if (l.entity == entity) { slot = &l; break; }
        if (l.seen < oldest->seen) oldest = &l;
    }
    if (!slot) { slot = oldest; *slot = Lying{entity, at, false, now}; }
    slot->seen = now;
    if (slot->woke) return false;
    if (coop_bot::Distance(at, slot->at) > kWakeDistance || (max > 0.0f && current < max - 0.01f)) {
        slot->woke = true;
        InterlockedIncrement(&playdeadWakes);
        if (InterlockedIncrement(&playdeadLines) <= 40)
            gate::Event("bot: %08X got up from playing dead (%.2f m off its spot, health %.0f/%.0f)", entity, coop_bot::Distance(at, slot->at), current, max);
        return false;
    }
    return true;
}
// Neutral people are no targets: the game's own DeadSpaceNPC::NoFriendlyFireFromPlayer
// (vtable +0x31C, thiscall, no arguments; the kinesis launch check 0x0086AD70
// asks it about the player's aim target). NeutralNPC (vtable 0x01B84EE8,
// 0x012CC5D0) says yes unless a scene marks it as fair game; necromorphs say
// no (0x00FFEE50). They count as players instead: no shot through them either.
constexpr uint32_t kNoFriendlyFireSlot = 0x31C, kNeutralNpcVtableRva = 0x1784EE8;
volatile LONG neutralSkips = 0, neutralFaults = 0, neutralSeen = 0;
bool Neutral(uint32_t entity, uint32_t vtable) {
    uint32_t fn = 0;
    if (!Read(vtable + kNoFriendlyFireSlot, &fn, 4) || fn < base + 0x1000 || fn >= base + gate::kRdataLo) return vtable == base + kNeutralNpcVtableRva;
    __try {
        return reinterpret_cast<bool(__thiscall*)(void*)>(fn)(reinterpret_cast<void*>(entity));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&neutralFaults);
        return vtable == base + kNeutralNpcVtableRva;
    }
}
bool Hidden(uint32_t entity) {
    unsigned char count = 0;
    return Read(entity + kHiddenCount, &count, 1) && count;
}
volatile LONG heartsSeen = 0;
uint32_t MarkerHeart(uint32_t boss) {
    uint32_t stack[64], rootSm = 0;
    int n = 0, visited = 0;
    if (!Read(boss + 0x24C, &rootSm, 4) || !rootSm) return 0;
    stack[n++] = rootSm;
    while (n && visited < 256) {
        const uint32_t sm = stack[--n];
        ++visited;
        uint32_t vtable = 0, child = 0, sibling = 0;
        if (!Read(sm, &vtable, 4)) continue;
        if (vtable == base + kNicoleBossSmVtableRva) {
            uint32_t ref = 0, heartVtable = 0;
            if (!Read(sm + kMarkerHeartRef, &ref, 4) || ref < 0x10010) return 0;
            return Read(ref - 0x10, &heartVtable, 4) && heartVtable == base + kDamageablePropVtableRva ? ref - 0x10 : 0;
        }
        if (Read(sm + 0x20, &child, 4) && child && n < 64) stack[n++] = child;
        if (Read(sm + 0x28, &sibling, 4) && sibling && n < 64) stack[n++] = sibling;
    }
    return 0;
}

// Dead necromorphs close to the bot or the leader ([Bot] StompCorpses).
constexpr int kMaxCorpses = 12;
constexpr float kCorpseRange = 14.0f;
struct Corpse { uint32_t entity = 0; Vec3 at; };
// Items the leader has to take himself: a key item opens his way on, a schematic is his store's.
constexpr uint32_t kKeyItemVtableRva = 0x174EAC0, kSchematicVtableRva = 0x174F560, kLogEntryVtableRva = 0x1772D98;   // audio and text logs too
volatile LONG leaderItemsSkipped = 0;
struct Snapshot {
    coop_bot::Enemy enemies[coop_bot::kMaxEnemies];
    uint32_t enemyEntity[coop_bot::kMaxEnemies]{};
    int enemyCount = 0;
    Corpse corpses[kMaxCorpses];
    int corpseCount = 0;
    coop_bot::Item items[coop_bot::kMaxItems];
    int itemCount = 0;
    Vec3 players[coop_bot::kMaxPlayers];   // every Isaac but the bot's own, local or a partner's ghost
    int playerCount = 0;
    Vec3 humans[coop_bot::kMaxPlayers];    // the same without the bots
    int humanCount = 0;
    bool doorBusy = false;
    Vec3 doorAt;
    unsigned scanned = 0, npcs = 0, alive = 0, engaged = 0, limbs = 0;
    int sighted = 0;   // enemies [0, sighted) have their limbs and sight (Sight)
};

// The necromorph players are the enemies of the human side, which the bots are always
// on (a necromorph leader gets no bots): in Versus, and in a story game with a necro
// player in it (user, 17.10.: they are killable then, so they shoot back).
volatile LONG necroTargetsSeen = 0, necroTargetLines = 0;
uint32_t necroTargetsLogged[8]{};
void NoteNecroTarget(uint32_t entity, const Vec3& at, float health) {
    InterlockedIncrement(&necroTargetsSeen);
    for (uint32_t e : necroTargetsLogged) if (e == entity) return;
    necroTargetsLogged[necroTargetLines % 8] = entity;
    if (InterlockedIncrement(&necroTargetLines) <= 20)
        gate::Event("bot: the necromorph player %08X is a target (at %.1f %.1f %.1f, health %.0f)", entity, at.x, at.y, at.z, health);
}
// One walk over the chain. self and leader are where the bot and the player it
// follows stand. Limbs and sight of the enemies found come from Sight.
void Scan(Snapshot& s, uint32_t selfEntity, const Vec3& self, const Vec3& leader) {
    static const uint32_t kItemHash = gate::NameHash("Item"), kPickupHash = gate::NameHash("PickupItem");
    s = Snapshot{};
    // the picked mode outlives the visit: Versus only on its map
    const bool necroTargets = (mpnative::InVersus() && mpmap::InVisit()) || bot::NecroInGame();
    uint32_t node = 0;
    if (!Read(base + kReflectableListRva, &node, 4)) return;
    float bestDoor = kDoorRange;
    uint32_t heart = 0;
    for (unsigned steps = 0; node && steps < 8192; ++steps) {
        const uint32_t entity = node - kEntityReflectable;
        uint32_t next = 0;
        const bool more = Read(node + 4, &next, 4) && next != node;
        uint32_t vtable = 0;
        ++s.scanned;
        // a Dementia phantom is only in its Isaac's head: no bot, turret or mine sees it
        if (entity != selfEntity && !phantom::Is(entity) && Read(entity, &vtable, 4) && vtable >= base + gate::kRdataLo && vtable < base + gate::kRdataHi) {
            Vec3 at;
            if (vtable == base + placement::kPlayerPrimaryVtableRva) {
                if (s.playerCount < coop_bot::kMaxPlayers && Pos(entity, at)) {
                    s.players[s.playerCount++] = at;
                    if (!IsBotEntity(entity) && s.humanCount < coop_bot::kMaxPlayers) s.humans[s.humanCount++] = at;
                }
            } else if (vtable == base + kGuardianTentacleVtableRva) {
                const uint32_t health = hits::FindHealthComponent(entity);
                float current = 0.0f, max = 0.0f;
                if (s.enemyCount < coop_bot::kMaxEnemies && health && Read(health + hits::kHealthCurrent, &current, 4) && current > 0.0f &&
                    !Hidden(entity) && Pos(entity, at) && (coop_bot::Distance(at, self) < kEnemyRange || coop_bot::Distance(at, leader) < kEnemyRange)) {
                    Read(health + hits::kHealthMax, &max, 4);
                    InterlockedIncrement(&tentaclesSeen);
                    ++s.alive;
                    coop_bot::Enemy& e = s.enemies[s.enemyCount];
                    e = coop_bot::Enemy{};
                    e.id = entity;
                    e.pos = at;
                    e.health = current;
                    e.maxHealth = max;
                    e.kind = coop_bot::EnemyKind::GuardianTentacle;
                    e.chest = {at.x, at.y + 0.5f, at.z};   // Sight measures it
                    s.enemyEntity[s.enemyCount++] = entity;
                    ++s.engaged;
                }
            } else if (vtable == base + kStandardDoorVtableRva) {
                uint32_t state = 0;
                if (Pos(entity, at) && Read(entity + kDoorState, &state, 4)) {
                    const float d = coop_bot::Flat(at, leader);
                    const bool open = state == 1 || state == 2 || state == 4;
                    if (DoorOpenedByLeader(entity, open, d) && d < bestDoor) { bestDoor = d; s.doorBusy = true; s.doorAt = at; }
                }
            } else if (vtable == base + kNecroPrimaryVtableRva) {
                const uint32_t health = necroTargets && s.enemyCount < coop_bot::kMaxEnemies && !Hidden(entity) ? hits::FindHealthComponent(entity) : 0;
                float current = 0.0f, max = 0.0f;
                if (health && Read(health + hits::kHealthCurrent, &current, 4) && current > 0.0f && Pos(entity, at) &&
                    (coop_bot::Distance(at, self) < kEnemyRange || coop_bot::Distance(at, leader) < kEnemyRange)) {
                    Read(health + hits::kHealthMax, &max, 4);
                    ++s.alive;
                    coop_bot::Enemy& e = s.enemies[s.enemyCount];
                    e = coop_bot::Enemy{};
                    e.id = entity;
                    e.pos = at;
                    e.health = current;
                    e.maxHealth = max;
                    e.vel = Velocity(entity, at);
                    e.chest = {at.x, at.y + 0.8f, at.z};   // Sight measures it
                    s.enemyEntity[s.enemyCount++] = entity;
                    ++s.engaged;
                    NoteNecroTarget(entity, at, current);
                }
            } else if (npcs::IsNpcReflectable(node) && !IsPlayerOrNecroEntity(entity)) {
                if (vtable == base + kNicoleBossVtableRva) {
                    if (!heart) heart = MarkerHeart(entity);
                    const LONG shown = Hidden(entity) ? 0 : 1;
                    if (InterlockedExchange(&nicoleShown, shown) != shown)
                        gate::Event("bot: Nicole %08X is %s", entity, shown ? "there (a target)" : "hidden (no target)");
                }
                const bool hidden = Hidden(entity);
                if (hidden) InterlockedIncrement(&hiddenSkips);
                const bool neutral = !hidden && Neutral(entity, vtable);
                if (neutral) {
                    InterlockedIncrement(&neutralSkips);
                    if (InterlockedIncrement(&neutralSeen) <= 10)
                        gate::Event("bot: %08X (vtable RVA %08X) is neutral: no target, kept out of the line of fire", entity, vtable - static_cast<uint32_t>(base));
                    if (s.playerCount < coop_bot::kMaxPlayers && Pos(entity, at)) s.players[s.playerCount++] = at;
                }
                ++s.npcs;   // every necromorph on the chain, near or not
                const bool forced = forcedEnemy && entity == forcedEnemy;
                const float range = forced ? kForcedRange : kEnemyRange;
                if (s.enemyCount < coop_bot::kMaxEnemies && !hidden && !neutral && !guidfix::IsGhostEntity(entity) && Pos(entity, at) &&
                    (coop_bot::Distance(at, self) < range || coop_bot::Distance(at, leader) < range)) {
                    const uint32_t health = hits::FindHealthComponent(entity);
                    float current = 0.0f, max = 0.0f;
                    const bool alive = health && Read(health + hits::kHealthCurrent, &current, 4) && current > 0.0f;
                    if (alive) ++s.alive;
                    if (!alive && health && s.corpseCount < kMaxCorpses &&
                        (coop_bot::Distance(at, self) < kCorpseRange || coop_bot::Distance(at, leader) < kCorpseRange))
                        s.corpses[s.corpseCount++] = Corpse{entity, at};
                    // Fighting, or lurking close by (a lurker waits with no target and nothing
                    // in its awareness list, 28.09.); the line of sight below decides the rest.
                    const bool close = coop_bot::Distance(at, self) < kLurkRange || coop_bot::Distance(at, leader) < kLurkRange;
                    const bool nest = vtable == base + kNestVtableRva;
                    if (nest && alive) InterlockedIncrement(&nestsSeen);
                    // No continue here: the walk steps to the next node at the loop's end.
                    bool lyingDead = false;
                    if (alive && !forced) {
                        Read(health + hits::kHealthMax, &max, 4);
                        lyingDead = PlayingDead(entity, at, current, max);
                        if (lyingDead) InterlockedIncrement(&playdeadSkips);
                    }
                    if (alive && !lyingDead && (forced || close || nest || Engaged(entity))) {
                        Read(health + hits::kHealthMax, &max, 4);
                        coop_bot::Enemy& e = s.enemies[s.enemyCount];
                        e = coop_bot::Enemy{};
                        e.id = entity;
                        e.pos = at;
                        e.health = current;
                        e.maxHealth = max;
                        e.limbsLost = LimbsLost(entity, health);
                        e.regenerating = Regenerating(entity, vtable);
                        e.kind = KindOf(vtable);
                        e.dodger = e.kind == coop_bot::EnemyKind::Slasher && TwitcherGuid(entity);
                        uint32_t hunted = 0;   // its target, entity + 0x10 (27.09.: the bot's own at +0x6F8)
                        if (Read(entity + kNpcTarget, &hunted, 4) && hunted > 0x10010) e.victim = hunted - 0x10;
                        e.vel = Velocity(entity, at);
                        e.chest = {at.x, at.y + 1.2f, at.z};   // Sight measures it
                        s.enemyEntity[s.enemyCount++] = entity;
                        ++s.engaged;
                    }
                }
            } else if (s.itemCount < coop_bot::kMaxItems && (gate::IsA(entity, vtable, kPickupHash) || gate::IsA(entity, vtable, kItemHash))) {
                uint32_t owner = 0;
                unsigned char hidden = 0;
                if (Read(entity + kItemOwner, &owner, 4) && !owner && Read(entity + kHiddenCount, &hidden, 1) && !hidden && Pos(entity, at) &&
                    coop_bot::Distance(at, self) < kItemRange) {
                    if (vtable == base + kKeyItemVtableRva || vtable == base + kSchematicVtableRva || vtable == base + kLogEntryVtableRva)
                        InterlockedIncrement(&leaderItemsSkipped);
                    else if (!turret::IsBody(entity) && !gadgets::IsBody(entity)) s.items[s.itemCount++] = coop_bot::Item{entity, at};
                }
            }
        }
        if (!more) break;
        node = next;
    }
    Vec3 heartAt;
    if (heart && !Hidden(heart) && s.enemyCount < coop_bot::kMaxEnemies && Pos(heart, heartAt) &&
        (coop_bot::Distance(heartAt, self) < kObjectiveRange || coop_bot::Distance(heartAt, leader) < kObjectiveRange)) {
        coop_bot::Enemy& e = s.enemies[s.enemyCount];
        e = coop_bot::Enemy{};
        e.id = heart;
        e.pos = e.chest = heartAt;
        e.health = e.maxHealth = 100.0f;   // below "dangerous": no stasis on it
        e.objective = true;
        s.enemyEntity[s.enemyCount++] = heart;
        if (InterlockedIncrement(&heartsSeen) == 1) gate::Event("bot: the Marker's heart %08X is a target (at %.1f %.1f %.1f)", heart, heartAt.x, heartAt.y, heartAt.z);
    }
}
// The costly half of a scan, for one enemy of it: its limbs from the skeleton and
// the rays from the bot's eye to them. The bot tick spreads these over a few
// frames (05.10.: a scan with every enemy in one go took up to 37 ms in a frame).
// A tentacle's points: the middles of its ragdoll's physics bodies (its origin may
// sit in the wall or the Guardian), farthest from the root first, those with a
// clear ray kept. The middle of them all stands in for its chest.
volatile LONG tentacleSights = 0, tentaclePoints = 0, tentacleNoRagdoll = 0, tentacleRefreshes = 0;
void TentacleSight(uint32_t selfEntity, uint32_t entity, const Vec3& eye, coop_bot::Enemy& e) {
    InterlockedIncrement(&tentacleSights);
    uint32_t vtable = 0, ragdollAt = 0, healthAt = 0, maps = 0, count = 0;
    const uint32_t ragdoll = Read(entity, &vtable, 4) && ComponentOffsets(entity, vtable, ragdollAt, healthAt) ? ComponentAt(entity, ragdollAt, kRagdollVtableRva) : 0;
    struct Point { Vec3 at; float out; };
    Point points[16];
    int n = 0;
    Vec3 sum{};
    if (ragdoll && Read(ragdoll + kJointMaps, &maps, 4) && maps && Read(ragdoll + kJointMapCount, &count, 4) && count && count <= kMaxJoints) {
        for (uint32_t j = 0; j < count && n < 16; ++j) {
            uint32_t body = 0;
            Vec3 center;
            float size = 0.0f;
            if (!Read(maps + j * kJointMapSize + kJointBody, &body, 4) || !body || !HitVolume(body, e.pos, center, size)) continue;
            points[n++] = Point{center, coop_bot::Distance(center, e.pos)};
            sum = coop_bot::Add(sum, center);
        }
    }
    if (!n) {
        InterlockedIncrement(&tentacleNoRagdoll);
        e.visible = !losReady || Clear(eye, e.chest, selfEntity, entity);
        return;
    }
    e.chest = coop_bot::Scale(sum, 1.0f / static_cast<float>(n));
    for (int a = 1; a < n; ++a)
        for (int b = a; b > 0 && points[b].out > points[b - 1].out; --b) std::swap(points[b], points[b - 1]);
    int rays = 0;
    for (int k = 0; k < n && e.limbCount < coop_bot::kMaxLimbs && rays < 6; ++k, ++rays) {
        if (LimbSight(eye, points[k].at, selfEntity, entity) != Sight::Clear) continue;
        e.limbKind[e.limbCount] = coop_bot::LimbKind::Arm;
        e.limbs[e.limbCount++] = points[k].at;
    }
    InterlockedExchangeAdd(&tentaclePoints, e.limbCount);
    e.visible = e.limbCount > 0 || !losReady || Clear(eye, e.chest, selfEntity, entity);
}
void Sight(Snapshot& s, int i, uint32_t selfEntity, const Vec3& self, const Vec3& eye) {
    coop_bot::Enemy& e = s.enemies[i];
    const uint32_t entity = s.enemyEntity[i];
    if (e.kind == coop_bot::EnemyKind::GuardianTentacle) { TentacleSight(selfEntity, entity, eye, e); s.limbs += static_cast<unsigned>(e.limbCount); return; }
    if (e.objective) { e.visible = !losReady || Clear(eye, e.pos, selfEntity, entity); return; }
    Vec3 limbs[coop_bot::kMaxLimbs];
    coop_bot::LimbKind kinds[coop_bot::kMaxLimbs]{};
    Parts parts;
    const int found = Limbs(entity, e.pos, limbs, kinds, e.chest, e.height, parts);
    if (e.height <= 0.0f && KindAimHeight(e.kind) < 1.2f) {
        e.chest = {e.pos.x, e.pos.y + KindAimHeight(e.kind), e.pos.z};
        InterlockedIncrement(&kindAimFallbacks);
    }
    PickExposed(eye, selfEntity, entity, e.chest, limbs, kinds, found, e);
    e.visible = losReady ? (e.limbCount > 0 || Clear(eye, e.chest, selfEntity, entity)) : std::fabs(e.pos.y - self.y) < 4.0f;
    if (!e.visible && ExposedParts(eye, selfEntity, entity, parts, e)) e.visible = true;
    if (e.kind == coop_bot::EnemyKind::Nest) {
        InterlockedIncrement(&nestSights);
        if (e.limbCount > 0) InterlockedIncrement(&nestPods);
        if (InterlockedIncrement(&nestLogLines) <= 40)
            gate::Event("bot: nest %08X at %.1f m, health %.0f/%.0f, pods in sight %d of %d found, body seen %d, height %.2f", entity,
                        coop_bot::Distance(self, e.pos), e.health, e.maxHealth, e.limbCount, found, e.visible ? 1 : 0, e.height);
    }
    // The body in sight but no limb: the camera's sphere cast catches on
    // thin limbs and edges; they are a hand beside the body, so keep them.
    if (e.visible && !e.limbCount && found > 0) {
        for (int k = 0; k < found; ++k) { e.limbKind[k] = kinds[k]; e.limbs[k] = limbs[k]; }
        e.limbCount = found;
        InterlockedIncrement(&limbsKept);
    }
    s.limbs += static_cast<unsigned>(e.limbCount);
}
// The enemy being shot at, anew every frame: where it stands and its limbs now,
// not a quarter second ago (a running slasher covers a metre in that time).
bool Refresh(coop_bot::Enemy& e, uint32_t selfEntity, const Vec3& eye) {
    const uint32_t entity = e.id;
    uint32_t vtable = 0;
    Vec3 at;
    if (!entity || !Read(entity, &vtable, 4) || vtable < base + gate::kRdataLo || vtable >= base + gate::kRdataHi || !Pos(entity, at)) return false;
    if (e.objective) { e.pos = e.chest = at; return true; }   // a prop: no skeleton to ask
    // A tentacle by its ragdoll bodies, as Sight measures it. Its vtable slot +0xA0
    // is Animated's GetWorldBonePos too (0x0074DB90 in the retail .rdata), but the
    // skeleton path sorts its points into arms and legs by height bands made for a
    // body standing on its feet, and replaced Sight's points every refresh.
    if (e.kind == coop_bot::EnemyKind::GuardianTentacle || vtable == base + kGuardianTentacleVtableRva) {
        e.pos = at;
        e.chest = {at.x, at.y + 0.5f, at.z};
        e.limbCount = 0;
        TentacleSight(selfEntity, entity, eye, e);
        InterlockedIncrement(&tentacleRefreshes);
        return true;
    }
    e.regenerating = Regenerating(entity, vtable);   // the target's, every refresh: it stands up between two scans
    Vec3 limbs[coop_bot::kMaxLimbs];
    coop_bot::LimbKind kinds[coop_bot::kMaxLimbs]{};
    Vec3 chest = e.chest;
    float height = 0.0f;
    Parts parts;
    const int found = Limbs(entity, at, limbs, kinds, chest, height, parts);
    // No skeleton: the same per-kind aim height as Sight, not Limbs' 1.2 m.
    if (height <= 0.0f && KindAimHeight(e.kind) < 1.2f) chest = {at.x, at.y + KindAimHeight(e.kind), at.z};
    e.pos = at;
    e.chest = chest;
    if (height > 0.0f) e.height = height;
    PickExposed(eye, selfEntity, entity, chest, limbs, kinds, found, e);
    if (!e.limbCount && (found > 0 || parts.count > 0)) {
        if (!losReady || Clear(eye, chest, selfEntity, entity)) {
            for (int i = 0; i < found; ++i) { e.limbKind[i] = kinds[i]; e.limbs[i] = limbs[i]; }
            e.limbCount = found;
            if (found > 0) InterlockedIncrement(&limbsKept);
        } else if (ExposedParts(eye, selfEntity, entity, parts, e)) {
            e.visible = true;
        }
    }
    return true;
}
// A new level, a restart or the main menu: what is kept by entity address goes,
// since the next level hands the same addresses to other entities (a necromorph
// there would count as playing dead, or as woken, by an old one's record).
volatile LONG levelForgets = 0;
void ForgetLevel() {
    for (Lying& l : lying) l = Lying{};
    for (Track& t : tracks) t = Track{};
    for (ZoneMemory& z : zoneMemory) z = ZoneMemory{};
    for (DoorSeen& d : doorsSeen) d = DoorSeen{};
    for (RegenSeen& r : regenSeen) r = RegenSeen{};
    InterlockedIncrement(&levelForgets);
}
}  // namespace bot_sense
