// Dead Space 2 United by Richard Hafer.
// Spectator after death in story co-op ([Spectate], off by default;
// analysis/SPECTATOR_AFTER_DEATH_2026-10-16.md). A human who dies while another human
// lives restarts nobody: his death keeps the dead Player in slot 0 (the MP-map respawn's
// way: ProcessDeath's SP branch skipped, 0x20 and 0x1E swallowed), the engine's own MP
// ObserverCamera follows a living partner, and he comes back as a fresh Isaac when a
// living partner reaches the next checkpoint (user, 16.10.: no timer). The host learns a
// client's checkpoint from a CKPT reach report (flags 2). A partner's scene or a level
// change he is needed for brings him back early at the partner's side. When no human
// lives any more, the normal checkpoint restart runs. Role 4 in NROL: every partner
// feature treats him like a necro at the vents (no voter, no target, no stream reference).
namespace spectate {
bool enabled = false, inScenes = false, sceneRespawn = true, levelChangeRespawn = true, corpseHold = true, orbit = false;
int cameraMode = 0;   // 0 the MP observer camera, 1 the death camera only
float distance = 2.5f, height = 1.6f, fovDeg = 60.0f, blendSeconds = 0.75f, farFromCheckpointM = 40.0f;
DWORD findWaitMs = 8000;
int nextKey = VK_LBUTTON, prevKey = VK_RBUTTON;
bool cameraOk = false;

enum class Stage { None, Watching, Triggered, Scheduled, Direct, Respawned, Failed };
Stage stage = Stage::None;
const char* StageName(Stage s) {
    switch (s) {
    case Stage::None: return "none";
    case Stage::Watching: return "watching";
    case Stage::Triggered: return "triggered";
    case Stage::Scheduled: return "scheduled";
    case Stage::Direct: return "direct";
    case Stage::Respawned: return "respawned";
    case Stage::Failed: return "failed";
    }
    return "?";
}
enum class Why { None, HostCheckpoint, PartnerReached, Scene, LevelChange };
const char* WhyName(Why w) {
    switch (w) {
    case Why::None: return "-";
    case Why::HostCheckpoint: return "the host's next checkpoint";
    case Why::PartnerReached: return "a partner reached a checkpoint";
    case Why::Scene: return "a partner's scene";
    case Why::LevelChange: return "a partner at a level change";
    }
    return "?";
}

uint32_t dyingPlayer = 0, deadPlayer = 0, newPlayer = 0, deathGuid[4]{}, deathSubKey = 0;
DWORD dyingAt = 0, diedAt = 0, triggeredAt = 0, respawnedAt = 0, noneAliveSince = 0, finishedAt = 0;
float deathPos[3]{};
// What "the next checkpoint" is measured against: the client's applied (or pending) host
// checkpoint, the host's own current one.
uint32_t baseGuid[4]{}, baseEpoch = 0;
// The trigger: why, the checkpoint (zero for a scene or level change) and where he comes back.
Why why = Why::None;
uint32_t targetGuid[4]{};
alignas(16) float targetMatrix[16]{};
bool haveTarget = false, curtainUp = false, sceneCueHeld = false;
DWORD targetSeenAt = 0, gateSampleAt = 0;
float gateSample[3]{};
// A host's partner reports (CKPT flags 2) and level change votes, taken on the game thread.
uint32_t reachGuid[4]{}, reachToken = 0, reachSeq = 0;
bool reachWanted = false;
uint32_t voteHash = 0;
float voteAt[3]{};
bool voteWanted = false;
DWORD sceneAskedAt = 0;
unsigned sceneAuthor = 0xFF;

volatile LONG decisions = 0, decidedNative = 0, decidedScene = 0, swallowed20 = 0, swallowed1E = 0, starts = 0, duplicates = 0, guidRefused = 0, triggers = 0,
              respawns = 0, finishes = 0, fallbacksSchedule = 0, fallbacksDirect = 0, failures = 0, freshSpawns = 0, allDead = 0, corpseHolds = 0,
              cameraPushes = 0, cameraPops = 0, cameraFailures = 0, cameraFaults = 0, switches = 0, reachIn = 0, reachIgnored = 0, votesIn = 0, levelChangePulls = 0,
              sceneAsks = 0, trailFallbacks = 0, findWaits = 0, restartsRefused = 0, ends = 0, catchupRefused = 0, necroKeyRefused = 0;
unsigned lines = 0;
void Line(const char* format, ...) {
    if (++lines > 300) return;
    char text[400];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    gate::Event("spectate: %s", text);
}

bool Active() { return enabled && stage != Stage::None; }

// --- who lives ---
// A partner counts when he was heard lately, plays a human (role 0) and his ghost is not dead.
// Necro players, spectators and bots never count.
unsigned LivingHumans(DWORD now, unsigned* slots = nullptr, unsigned cap = 0, bool log = false) {
    unsigned n = 0;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        if (!peer.used || !peer.token) continue;
        const bool fresh = now - peer.lastSeen < 10000;
        const int role = roles::RoleOf(s, now);
        const uint32_t ghost = peer.machine < 0xF0 ? GhostForMachine(peer.machine) : 0;
        const bool dead = ghost && nameplate::Dead(ghost);
        const bool living = fresh && role == 0 && !dead;
        if (log) Line("  peer slot=%u machine=%u role=%d ghost=%08X dead=%d heard %lu ms ago -> %s", s, peer.machine, role, ghost, dead ? 1 : 0,
                      static_cast<unsigned long>(now - peer.lastSeen), living ? "alive" : "not counted");
        if (!living) continue;
        if (slots && n < cap) slots[n] = s;
        ++n;
    }
    return n;
}

// --- the camera: ObserverCameraInfo without the ObserverPlayer around it ---
constexpr uint32_t kZeroGuidRva = 0x340140, kDefaultSpawnRva = 0xC9D6D0, kQueryRva = 0xC80D20, kSetSubjectRva = 0xC85A20, kPushRva = 0x392530,
                   kPopRva = 0x393FF0, kCameraManagerRva = 0x1C2131C, kObserverInfoType = 0x08A5E969;
constexpr uint32_t kAttrFov = 0x08, kAttrOffset = 0x50, kAttrOrbit = 0x60, kAttrOrbitSpeed = 0x64;
uint32_t cameraEntity = 0, cameraInfo = 0, subjectGhost = 0;
unsigned subjectSlot = 0xFF;
DWORD pushedAt = 0, poseLogAt = 0, pushTriedAt = 0;
unsigned pushTries = 0;   // a camera that will not come is tried five times, 2 s apart
unsigned poseLines = 0;
__declspec(noinline) uint32_t SpawnInfoRaw() {
    __try {
        const uint32_t zero = reinterpret_cast<uint32_t(__cdecl*)()>(base + kZeroGuidRva)();
        return reinterpret_cast<uint32_t(__cdecl*)(int, uint32_t, const float*, int)>(base + kDefaultSpawnRva)(0, zero, nullptr, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&cameraFaults);
        return 0;
    }
}
__declspec(noinline) uint32_t QueryRaw(uint32_t object) {
    __try {
        return reinterpret_cast<uint32_t(__cdecl*)(uint32_t, uint32_t)>(base + kQueryRva)(object, kObserverInfoType);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&cameraFaults);
        return 0;
    }
}
__declspec(noinline) bool SubjectAndPushRaw(uint32_t cm, uint32_t info, uint32_t ghost, float blend) {
    __try {
        reinterpret_cast<void(__thiscall*)(uint32_t, uint32_t)>(base + kSetSubjectRva)(info, ghost);
        reinterpret_cast<void(__thiscall*)(uint32_t, uint32_t, uint32_t, float, int, int)>(base + kPushRva)(cm, 0, info, blend, 1, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&cameraFaults);
        return false;
    }
}
__declspec(noinline) bool PopRaw(uint32_t cm, uint32_t info) {
    __try {
        reinterpret_cast<void(__thiscall*)(uint32_t, uint32_t, uint32_t, float, int, int, int)>(base + kPopRva)(cm, 0, info, 0.0f, 1, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&cameraFaults);
        return false;
    }
}
uint32_t BlenderDepth(uint32_t cm) {
    uint32_t blenders = 0, depth = 0xFFFFFFFFu;
    if (cm && Read(cm + 0x10, &blenders, 4) && blenders) Read(blenders + 0xD4, &depth, 4);
    return depth;
}
void PopCamera(const char* reason) {
    if (!cameraInfo) return;
    uint32_t cm = 0;
    Read(base + kCameraManagerRva, &cm, 4);
    const uint32_t before = BlenderDepth(cm);
    const bool ok = cm && PopRaw(cm, cameraInfo);
    const bool deleted = cameraEntity && bot::DeleteRaw(cameraEntity);
    InterlockedIncrement(&cameraPops);
    Line("camera popped (%s): %s, deleted %d, blender stack %u -> %u", reason, ok ? "ok" : "FAILED", deleted ? 1 : 0, before, BlenderDepth(cm));
    cameraInfo = cameraEntity = 0;
    subjectGhost = 0;
}
bool PushCamera(unsigned slot, const char* reason) {
    if (cameraMode != 0 || !cameraOk || slot >= coop_peers::kMaxPeers) return false;
    const uint32_t ghost = GhostForMachine(peerTable.peers[slot].machine);
    uint32_t cm = 0;
    if (!ghost || !Read(base + kCameraManagerRva, &cm, 4) || !cm) {
        InterlockedIncrement(&cameraFailures);
        Line("camera for slot %u (%s): no %s", slot, reason, ghost ? "camera manager" : "ghost body");
        return false;
    }
    const uint32_t spawned = SpawnInfoRaw();
    const uint32_t info = spawned ? QueryRaw(spawned) : 0;
    uint32_t vtable = 0, attrs = 0;
    if (spawned) Read(spawned, &vtable, 4);
    if (!info || !Read(info + 0x0C, &attrs, 4) || !attrs) {
        InterlockedIncrement(&cameraFailures);
        Line("camera spawn FAILED: spawned=%08X vtable=%08X info=%08X attrs=%08X", spawned, vtable, info, attrs);
        if (spawned) bot::DeleteRaw(spawned);
        return false;
    }
    float fov = 0.0f, offset[3]{}, speed = 0.0f;
    uint32_t orbitWas = 0;
    Read(attrs + kAttrFov, &fov, 4);
    Read(attrs + kAttrOffset, offset, 12);
    Read(attrs + kAttrOrbit, &orbitWas, 4);
    Read(attrs + kAttrOrbitSpeed, &speed, 4);
    const float wantFov = fovDeg * 0.0174533f, wantOffset[3] = {0.0f, height, -distance};
    const uint32_t wantOrbit = orbit ? 1u : 0u;
    WriteBytes(attrs + kAttrFov, &wantFov, 4);
    WriteBytes(attrs + kAttrOffset, wantOffset, 12);
    WriteBytes(attrs + kAttrOrbit, &wantOrbit, 1);
    const uint32_t before = BlenderDepth(cm);
    if (!SubjectAndPushRaw(cm, info, ghost, blendSeconds)) {
        InterlockedIncrement(&cameraFailures);
        Line("camera subject/push FAULTED (info %08X, ghost %08X)", info, ghost);
        bot::DeleteRaw(spawned);
        return false;
    }
    cameraEntity = spawned;
    cameraInfo = info;
    subjectGhost = ghost;
    subjectSlot = slot;
    pushedAt = poseLogAt = GetTickCount();
    poseLines = 0;
    InterlockedIncrement(&cameraPushes);
    Line("camera on machine %u's ghost %08X (%s): entity %08X vtable %08X info %08X attrs %08X; defaults fov %.3f offset (%.1f %.1f %.1f) orbit %u speed %.0f, "
         "written fov %.3f offset (%.1f %.1f %.1f) orbit %u; blender stack %u -> %u",
         peerTable.peers[slot].machine, ghost, reason, spawned, vtable, info, attrs, fov, offset[0], offset[1], offset[2], orbitWas & 0xFF, speed, wantFov,
         wantOffset[0], wantOffset[1], wantOffset[2], wantOrbit, before, BlenderDepth(cm));
    return true;
}
// The first poses of the camera, to tell a camera that follows from one that stands.
void LogPose(DWORD now) {
    if (!cameraInfo || poseLines >= 5 || now - poseLogAt < 1000) return;
    poseLogAt = now;
    ++poseLines;
    uint32_t cm = 0, blenders = 0;
    float m[16]{}, ghostPos[3]{};
    if (!Read(base + kCameraManagerRva, &cm, 4) || !cm || !Read(cm + 0x10, &blenders, 4) || !blenders || !Read(blenders + 0x50, m, sizeof(m))) return;
    const bool ghostRead = subjectGhost && Read(subjectGhost + 0xC0, ghostPos, 12);
    const float dx = m[12] - ghostPos[0], dy = m[13] - ghostPos[1], dz = m[14] - ghostPos[2];
    Line("camera pose %u: (%.1f %.1f %.1f), %.1f m from the subject%s", poseLines, m[12], m[13], m[14], ghostRead ? std::sqrt(dx * dx + dy * dy + dz * dz) : -1.0f,
         ghostRead ? "" : " (subject unreadable)");
}

// --- trails of the living humans (PlayerState positions, every 250 ms) ---
struct Sample { float pos[3]; DWORD at; };
Sample trails[coop_peers::kMaxPeers][16]{};
unsigned trailNext[coop_peers::kMaxPeers]{};
void SampleTrails(DWORD now) {
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        if (!peerTable.peers[s].used || !peerRuntime[s].lastState || now - peerRuntime[s].lastState > 1000) continue;
        Sample& t = trails[s][trailNext[s]++ % 16];
        std::memcpy(t.pos, peerRuntime[s].pos, sizeof(t.pos));
        t.at = now;
    }
}
// Where to stand next to him: a spot he walked through a moment ago, at least 1.5 m away.
bool TrailSpot(unsigned slot, DWORD now, float* out) {
    if (slot >= coop_peers::kMaxPeers || !peerRuntime[slot].lastState || now - peerRuntime[slot].lastState > 3000) return false;
    const float* here = peerRuntime[slot].pos;
    for (unsigned i = 1; i <= 16; ++i) {
        const Sample& t = trails[slot][(trailNext[slot] - i) % 16];
        if (!t.at || now - t.at > 3000) break;
        const float dx = t.pos[0] - here[0], dz = t.pos[2] - here[2];
        if (dx * dx + dz * dz >= 1.5f * 1.5f) { std::memcpy(out, t.pos, 12); return true; }
    }
    out[0] = here[0] + 1.0f;
    out[1] = here[1];
    out[2] = here[2];
    return true;
}
float NearestHuman(const float* at, DWORD now, unsigned* slotOut) {
    unsigned slots[coop_peers::kMaxPeers]{};
    const unsigned n = LivingHumans(now, slots, coop_peers::kMaxPeers);
    float best = 1e30f;
    for (unsigned i = 0; i < n; ++i) {
        const float* p = peerRuntime[slots[i]].pos;
        const float dx = p[0] - at[0], dy = p[1] - at[1], dz = p[2] - at[2], d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < best) { best = d; if (slotOut) *slotOut = slots[i]; }
    }
    return best;
}
void MatrixAt(const float* at) {
    const float m[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, at[0], at[1] + 0.2f, at[2], 1.0f};
    std::memcpy(targetMatrix, m, sizeof(m));
    haveTarget = true;
}

// --- the death ---
bool SceneNow() { return cutscene::Following() || cutscene::SequenceRunningNow(); }
// Whether this death of the local Isaac is watched out instead of restarting everybody.
const char* Decline(uint32_t owner, DWORD now) {
    if (!enabled) return "off";
    if (!owner || owner != LocalCampaignPlayer() || IsBotEntity(owner)) return "not the local Isaac";
    if (!play::StoryCoop() || mpmap::InVisit() || respawn::OnRespawnMap()) return "not the story";
    if (necro::LocalRole() != 0) return "a necro player";
    if (!checkpoint::installed || !checkpoint::enabled) return "no checkpoint follow";
    if (!inScenes && SceneNow()) return "in a scene (the leader's rules decide)";
    if (!LivingHumans(now)) return "no other human alive";
    return nullptr;
}
// From bot::OnDeathSm: the decision at 0x18, then the item drop (0x20) and the red death
// screen (0x1E) of a death that is watched out are swallowed.
bool SwallowDeathMessage(uint32_t owner, uint32_t message) {
    if (!enabled || !owner) return false;
    const DWORD now = GetTickCount();
    if (message == 0x18 && owner == LocalCampaignPlayer()) {
        InterlockedIncrement(&decisions);
        const char* no = Decline(owner, now);
        Line("death of Isaac %08X: %s", owner, no ? no : "watched out (a partner lives)");
        LivingHumans(now, nullptr, 0, true);
        if (no) {
            InterlockedIncrement(no[0] == 'i' ? &decidedScene : &decidedNative);
            dyingPlayer = 0;
        } else {
            dyingPlayer = owner;
            dyingAt = now;
        }
        return false;
    }
    if (owner != dyingPlayer || (message != 0x20 && message != 0x1E)) return false;
    InterlockedIncrement(message == 0x20 ? &swallowed20 : &swallowed1E);
    Line("death message %02X of %08X swallowed (%s)", message, owner, message == 0x20 ? "his items stay with him" : "no red death screen");
    return true;
}
// From OnProcessDeath: true when this death is ours.
bool TakeDeath(uint32_t player, uint32_t damage) {
    if (!enabled || respawn::passThrough || !player) return false;
    if (stage != Stage::None) {
        if (player == deadPlayer) { InterlockedIncrement(&duplicates); return true; }
        return false;
    }
    if (player != dyingPlayer) return false;
    dyingPlayer = 0;
    const DWORD now = GetTickCount();
    if (const char* no = Decline(player, now)) {
        InterlockedIncrement(&decidedNative);
        Line("ProcessDeath of %08X: %s now, the native restart", player, no);
        return false;
    }
    // RespawnPlayer spawns from the body's GUID without a null check
    uint32_t manager = 0;
    if (!Read(player + 0x24, deathGuid, 16) || !Read(base + necro::kSimManager, &manager, 4) || !manager || !respawn::ResolvesSafe(manager, deathGuid)) {
        InterlockedIncrement(&guidRefused);
        Line("Isaac %08X GUID %08X-%08X does not resolve: the native restart", player, deathGuid[0], deathGuid[1]);
        return false;
    }
    Read(player + 0x38, &deathSubKey, 4);
    Read(player + 0xC0, deathPos, sizeof(deathPos));
    respawn::Snapshot(player);
    if (IsHost()) {
        std::memcpy(baseGuid, checkpoint::current, 16);
        baseEpoch = checkpoint::epoch;
    } else {
        std::memcpy(baseGuid, checkpoint::pending ? checkpoint::wanted : checkpoint::applied, 16);
        baseEpoch = checkpoint::pending ? checkpoint::wantedEpoch : checkpoint::appliedEpoch;
    }
    uint32_t attacker = 0;
    if (damage) Read(damage + 0x80, &attacker, 4);
    deadPlayer = player;
    newPlayer = 0;
    diedAt = now;
    why = Why::None;
    haveTarget = reachWanted = voteWanted = false;
    noneAliveSince = 0;
    stage = Stage::Watching;
    InterlockedIncrement(&starts);
    Line("Isaac %08X watches (%lu ms after the health ran out): attacker %08X, carried credits=%d guns=%X stacks=%u, baseline %s epoch %u %08X-%08X, died at (%.1f %.1f %.1f)",
         player, static_cast<unsigned long>(dyingAt ? now - dyingAt : 0), attacker > 0x10010 ? attacker - 0x10 : 0, respawn::saved.credits, respawn::saved.guns,
         respawn::saved.count, IsHost() ? "host" : "client", baseEpoch, baseGuid[0], baseGuid[1], deathPos[0], deathPos[1], deathPos[2]);
    unsigned slots[coop_peers::kMaxPeers]{};
    subjectSlot = LivingHumans(now, slots, coop_peers::kMaxPeers) ? slots[0] : 0xFFu;
    pushTries = 0;
    pushTriedAt = 0;
    return true;
}

// --- the way back ---
bool BodyGone() {
    uint32_t vtable = 0;
    return !deadPlayer || !Read(deadPlayer, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva;
}
// The corpse's own area went away: a new Isaac from the level's template with his GUID.
__declspec(noinline) uint32_t FreshIsaacRaw(uint32_t manager) {
    __try {
        return necro::RespawnPlayer(manager, targetMatrix);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
uint32_t FreshIsaac() {
    uint32_t manager = 0;
    if (!Read(base + necro::kPlayerManager, &manager, 4) || !manager) return 0;
    uint32_t keepGuid[4];
    std::memcpy(keepGuid, necro::playerGuid, 16);
    const uint32_t keepKey = necro::playerSubKeyWord;
    std::memcpy(necro::playerGuid, deathGuid, 16);
    necro::playerSubKeyWord = deathSubKey;
    const uint32_t fresh = FreshIsaacRaw(manager);
    std::memcpy(necro::playerGuid, keepGuid, 16);
    necro::playerSubKeyWord = keepKey;
    return fresh;
}
// Where he comes back for a checkpoint: on the checkpoint's own matrix when it is loaded here
// and a living human is near it, else beside that human. False: wait for it a little longer.
bool ResolveCheckpoint(DWORD now) {
    const uint32_t object = checkpoint::FindCheckpoint(targetGuid);
    alignas(16) float m[16]{};
    unsigned slot = 0xFF;
    if (object && Read(object + 0x60, m, sizeof(m)) && std::isfinite(m[12]) && std::isfinite(m[13]) && std::isfinite(m[14])) {
        const float closest = NearestHuman(m + 12, now, &slot);
        if (closest <= farFromCheckpointM) {
            m[13] += 0.2f;
            m[3] = m[7] = m[11] = 0.0f;
            m[15] = 1.0f;
            std::memcpy(targetMatrix, m, sizeof(m));
            haveTarget = true;
            Line("checkpoint %08X-%08X found (object %08X) at (%.1f %.1f %.1f), the nearest human %.1f m from it", targetGuid[0], targetGuid[1], object, m[12], m[13],
                 m[14], closest);
            return true;
        }
        Line("checkpoint %08X-%08X at (%.1f %.1f %.1f) but the nearest human is %.1f m away: beside him instead", targetGuid[0], targetGuid[1], m[12], m[13], m[14], closest);
    } else if (now - targetSeenAt < findWaitMs) {
        InterlockedIncrement(&findWaits);
        return false;
    } else {
        Line("checkpoint %08X-%08X not found here after %lu ms: beside a living human instead", targetGuid[0], targetGuid[1], static_cast<unsigned long>(now - targetSeenAt));
    }
    float spot[3]{};
    if (slot >= coop_peers::kMaxPeers) NearestHuman(deathPos, now, &slot);
    if (slot >= coop_peers::kMaxPeers) slot = subjectSlot;
    if (!TrailSpot(slot, now, spot)) return false;
    InterlockedIncrement(&trailFallbacks);
    MatrixAt(spot);
    return true;
}
void Begin(DWORD now) {
    PopCamera("the way back");
    overlay::Curtain(8000);
    curtainUp = true;
    gateSampleAt = 0;
    triggeredAt = now;
    respawnedAt = 0;
    InterlockedIncrement(&triggers);
    Line("back: %s, to (%.1f %.1f %.1f), %lu s after the death", WhyName(why), targetMatrix[12], targetMatrix[13], targetMatrix[14],
         static_cast<unsigned long>((now - diedAt) / 1000));
    if (BodyGone()) {
        const uint32_t fresh = FreshIsaac();
        Line("the dead body %08X is gone: a fresh Isaac from the level's template: %08X", deadPlayer, fresh);
        if (!fresh) { stage = Stage::Failed; InterlockedIncrement(&failures); overlay::ShowBanner("RESPAWN FAILED", "RESTART THE CHECKPOINT FROM THE PAUSE MENU", 10000); return; }
        InterlockedIncrement(&freshSpawns);
        newPlayer = fresh;
        localSettle.Adopt(fresh);
        mpmap::PlaceRaw(fresh, targetMatrix);
        respawnedAt = now;
        stage = Stage::Respawned;
        return;
    }
    stage = Stage::Triggered;
    const bool ok = respawn::CallOriginal(deadPlayer, true);
    Line("ProcessDeath MP branch for %08X %s", deadPlayer, ok ? "called" : "FAULTED");
}
// From respawn::OnRespawnPlayer, after the native respawn: the new body goes onto its spot here,
// before anybody sees it at the old checkpoint.
bool OnRespawned(uint32_t old, uint32_t after) {
    if (!enabled || old != deadPlayer || (stage != Stage::Triggered && stage != Stage::Scheduled && stage != Stage::Direct)) return false;
    newPlayer = after != old ? after : 0;
    respawnedAt = GetTickCount();
    stage = Stage::Respawned;
    InterlockedIncrement(&respawns);
    const bool adopted = newPlayer && localSettle.Adopt(newPlayer);
    const int placed = newPlayer && haveTarget ? mpmap::PlaceRaw(newPlayer, targetMatrix) : -1;
    float init[3]{};
    if (newPlayer) Read(newPlayer + 0xC0, init, 12);
    Line("RespawnPlayer old=%08X new=%08X %lu ms after the trigger, settle adopted %d, placed %s (target %.1f %.1f %.1f, now %.1f %.1f %.1f)", old, newPlayer,
         static_cast<unsigned long>(respawnedAt - triggeredAt), adopted ? 1 : 0, placed == 2 ? "entity+cmc" : placed == 1 ? "entity only" : "NOT", targetMatrix[12],
         targetMatrix[13], targetMatrix[14], init[0], init[1], init[2]);
    return true;
}
void Finish(uint32_t player, DWORD now) {
    float at[3]{};
    Read(player + 0xC0, at, 12);
    const float dx = at[0] - targetMatrix[12], dz = at[2] - targetMatrix[14];
    if (haveTarget && dx * dx + dz * dz > 2.0f * 2.0f) {
        const int placed = mpmap::PlaceRaw(player, targetMatrix);
        Line("the new body stood at (%.1f %.1f %.1f), %.1f m off its spot: placed again (%d)", at[0], at[1], at[2], std::sqrt(dx * dx + dz * dz), placed);
    }
    respawn::Restore(player);
    tk::OnBodyReplaced();
    // The checkpoint only now, so its save keeps a living, equipped Isaac. A client's
    // pending host checkpoint is applied by checkpoint::Tick once he no longer watches.
    if (IsHost() && (why == Why::PartnerReached)) {
        const uint32_t object = checkpoint::FindCheckpoint(targetGuid);
        const bool ok = object && checkpoint::ApplyTo(object);
        Line("host checkpoint %08X-%08X applied: %s (object %08X)", targetGuid[0], targetGuid[1], ok ? "ok" : "NOT", object);
    }
    const Why was = why;
    stage = Stage::None;
    finishedAt = Stamp(now);
    deadPlayer = 0;
    newPlayer = 0;
    why = Why::None;
    InterlockedIncrement(&finishes);
    overlay::HideBanner();
    notice::Enqueue("BACK IN THE FIGHT");
    Line("Isaac %08X back (%s), %lu ms after the trigger, %lu s after the death", player, WhyName(was), static_cast<unsigned long>(now - triggeredAt),
         static_cast<unsigned long>((now - diedAt) / 1000));
}
void LiftCurtain(const char* reason, DWORD now) {
    if (!curtainUp) return;
    curtainUp = false;
    overlay::LiftCurtain();
    Line("curtain down %lu ms after the trigger (%s)", static_cast<unsigned long>(now - triggeredAt), reason);
}
void CurtainTick(DWORD now) {
    if (!curtainUp || stage != Stage::None) return;
    if (now - triggeredAt > 12000 || (respawnedAt && now - respawnedAt >= 4000)) { LiftCurtain("4 s after the respawn", now); return; }
    const uint32_t body = LocalCampaignPlayer();
    float at[3]{};
    if (!body || !Read(body + 0xC0, at, 12)) return;
    if (!gateSampleAt) { std::memcpy(gateSample, at, 12); gateSampleAt = now; return; }
    if (now - gateSampleAt < 150) return;
    const float dx = at[0] - gateSample[0], dy = at[1] - gateSample[1], dz = at[2] - gateSample[2];
    std::memcpy(gateSample, at, 12);
    gateSampleAt = now;
    if (dx * dx + dz * dz < 0.09f && std::fabs(dy) < 0.5f) LiftCurtain("the new body stands", now);
}
// Everything back to the living, before a restart or a level change takes the world.
void End(const char* reason) {
    if (stage == Stage::None && !dyingPlayer && !cameraInfo && !curtainUp) return;
    InterlockedIncrement(&ends);
    Line("ended in stage %s: %s", StageName(stage), reason);
    PopCamera(reason);
    if (curtainUp) LiftCurtain(reason, GetTickCount());
    stage = Stage::None;
    dyingPlayer = deadPlayer = newPlayer = 0;
    why = Why::None;
    haveTarget = reachWanted = voteWanted = sceneCueHeld = false;
    overlay::HideBanner();
}
void OnRestart() { End("a checkpoint restart"); }
void OnLevelExit() { End("the level exit"); }
// Nobody alive any more: the native restart, as if he had died now.
void AllDead(DWORD now) {
    InterlockedIncrement(&allDead);
    const uint32_t body = deadPlayer;
    Line("no human alive for %lu ms: the checkpoint restart for everybody (%s)", static_cast<unsigned long>(now - noneAliveSince), IsHost() ? "pulled here" : "the native death");
    End("everybody is down");
    if (IsHost()) checkpoint::PullRestart();
    else if (body && !BodyGone()) respawn::NativeDeath(body);
}

// --- what partners tell ---
// The host: a client reached a checkpoint (CKPT flags 2).
void OnPartnerReached(const uint32_t* guid, uint32_t token, uint32_t seq) {
    InterlockedIncrement(&reachIn);
    if (!IsHost() || stage != Stage::Watching || checkpoint::GuidZero(guid) || checkpoint::GuidEqual(guid, checkpoint::current) || (token == reachToken && seq == reachSeq)) {
        InterlockedIncrement(&reachIgnored);
        return;
    }
    reachToken = token;
    reachSeq = seq;
    std::memcpy(reachGuid, guid, 16);
    reachWanted = true;
}
// A partner's gate vote while this machine watches: a level change he waits at needs us there.
void OnPartnerVote(uint32_t hash, const float* at) {
    if (stage != Stage::Watching || !levelChangeRespawn) return;
    InterlockedIncrement(&votesIn);
    voteHash = hash;
    std::memcpy(voteAt, at, 12);
    voteWanted = true;
}
// The mirror: a partner's scene cue. True: the cue waits until Isaac is back beside the author.
bool AskScene(DWORD now, unsigned author) {
    if (stage != Stage::Watching || !sceneRespawn) return stage != Stage::None && stage != Stage::Failed && sceneCueHeld;
    InterlockedIncrement(&sceneAsks);
    sceneAskedAt = Stamp(now);
    sceneAuthor = author;
    sceneCueHeld = true;
    return true;
}
bool SceneCuesHeld() { return sceneCueHeld && stage != Stage::None && stage != Stage::Failed; }
bool SceneReady() { return sceneCueHeld && stage == Stage::None && LocalCampaignPlayer() && checkpoint::LocalIsaacAlive(); }
unsigned SubjectSlot() { return Active() ? subjectSlot : 0xFFu; }

// --- the game thread ---
bool keyDown[2]{};
uint32_t lastSim = 0;
DWORD lastSlow = 0, bannerAt = 0;
void SwitchSubject(int step, const char* reason) {
    unsigned slots[coop_peers::kMaxPeers]{};
    const unsigned n = LivingHumans(GetTickCount(), slots, coop_peers::kMaxPeers);
    if (!n) return;
    unsigned at = 0;
    for (unsigned i = 0; i < n; ++i) if (slots[i] == subjectSlot) at = i;
    const unsigned next = slots[(at + n + static_cast<unsigned>(step)) % n];
    if (next == subjectSlot && cameraInfo && GhostForMachine(peerTable.peers[next].machine) == subjectGhost) return;
    InterlockedIncrement(&switches);
    PopCamera(reason);
    subjectSlot = next;
    pushTries = 0;
    pushTriedAt = 0;   // the next watch tick pushes the camera on him
    bannerAt = 0;
}
void Watch(DWORD now) {
    // the keys (not in the pause menu: the game time stands still there)
    const uint32_t sim = turret::SimMs();
    const bool running = sim != lastSim;
    lastSim = sim;
    const bool focused = necro::GameWindowFocused();
    const int keys[2] = {nextKey, prevKey};
    for (int k = 0; k < 2; ++k) {
        const bool down = focused && keys[k] > 0 && keys[k] < 256 && (GetAsyncKeyState(keys[k]) & 0x8000) != 0;
        if (down && !keyDown[k] && running) SwitchSubject(k == 0 ? 1 : -1, "the key");
        keyDown[k] = down;
    }
    LogPose(now);
    // The camera reads its subject every frame: a body that is no ghost Isaac any more (a disconnect,
    // his necro transform) is let go now, not at the next slow tick.
    if (cameraInfo && subjectGhost && !IsGhostPlayer(subjectGhost)) {
        PopCamera("the subject's body went away");
        pushTries = 0;
        pushTriedAt = 0;
    }
    if (now - lastSlow < 250) return;
    lastSlow = now;
    SampleTrails(now);
    // the corpse stays where it fell
    if (corpseHold && !BodyGone()) {
        float at[3]{};
        if (Read(deadPlayer + 0xC0, at, 12) && (at[1] < deathPos[1] - 3.0f || !std::isfinite(at[1]))) {
            const bool moved = bot::MoveEntity(deadPlayer, coop_bot::Vec3{deathPos[0], deathPos[1], deathPos[2]});
            if (InterlockedIncrement(&corpseHolds) <= 10) Line("the corpse fell to y %.1f: put back to (%.1f %.1f %.1f): %d", at[1], deathPos[0], deathPos[1], deathPos[2], moved ? 1 : 0);
        }
    }
    unsigned slots[coop_peers::kMaxPeers]{};
    const unsigned living = LivingHumans(now, slots, coop_peers::kMaxPeers);
    if (!living) {
        if (!noneAliveSince) noneAliveSince = Stamp(now);
        else if (now - noneAliveSince >= 2000) { AllDead(now); return; }
    } else {
        noneAliveSince = 0;
        // the watched partner died, left or got a new body
        bool subjectLives = false;
        for (unsigned i = 0; i < living; ++i) subjectLives = subjectLives || slots[i] == subjectSlot;
        if (!subjectLives) SwitchSubject(0, "the watched partner is gone");
        else if (cameraInfo && GhostForMachine(peerTable.peers[subjectSlot].machine) != subjectGhost) {
            PopCamera("a new ghost body");
            pushTries = 0;
            pushTriedAt = 0;
        }
        if (!cameraInfo && cameraMode == 0 && cameraOk && pushTries < 5 && (!pushTriedAt || now - pushTriedAt >= 2000)) {
            ++pushTries;
            pushTriedAt = Stamp(now);
            if (PushCamera(subjectSlot, pushTries == 1 ? "watching" : "watching, again")) pushTries = 0;
        }
    }
    if (now - bannerAt >= 1000) {
        bannerAt = now;
        char title[64];
        const uint32_t machine = subjectSlot < coop_peers::kMaxPeers ? peerTable.peers[subjectSlot].machine : 0xFF;
        if (machine < 256 && nameplate::names[machine][0]) std::snprintf(title, sizeof(title), "WATCHING %s", nameplate::names[machine]);
        else std::snprintf(title, sizeof(title), "YOU DIED");
        overlay::ShowBanner(title, living > 1 ? "BACK AT THE NEXT CHECKPOINT - MOUSE BUTTONS SWITCH" : "BACK AT THE NEXT CHECKPOINT", 1500);
    }
    // the triggers
    if (why == Why::None) {
        if (!IsHost() && checkpoint::pending && static_cast<int32_t>(checkpoint::wantedEpoch - baseEpoch) > 0 && !checkpoint::GuidEqual(checkpoint::wanted, baseGuid)) {
            why = Why::HostCheckpoint;
            std::memcpy(targetGuid, checkpoint::wanted, 16);
            targetSeenAt = now;
            Line("the host's checkpoint moved: epoch %u -> %u, %08X-%08X", baseEpoch, checkpoint::wantedEpoch, targetGuid[0], targetGuid[1]);
        } else if (reachWanted) {
            reachWanted = false;
            why = Why::PartnerReached;
            std::memcpy(targetGuid, reachGuid, 16);
            targetSeenAt = now;
            Line("a partner (token %08X, report %u) reached checkpoint %08X-%08X", reachToken, reachSeq, targetGuid[0], targetGuid[1]);
        } else if (sceneAskedAt && sceneCueHeld) {
            unsigned slot = sceneAuthor < coop_peers::kMaxPeers ? sceneAuthor : subjectSlot;
            float spot[3]{};
            if (TrailSpot(slot, now, spot) || TrailSpot(subjectSlot, now, spot)) {
                why = Why::Scene;
                MatrixAt(spot);
            } else if (now - sceneAskedAt > 5000) {
                sceneCueHeld = false;
                Line("a partner's scene: nobody to stand beside, the cue is dropped");
            }
            sceneAskedAt = 0;
        } else if (voteWanted) {
            voteWanted = false;
            char cls[32] = "";
            if (gate::Classify(voteHash, now, cls, sizeof(cls)) && std::strcmp(cls, "LevelChange") == 0) {
                why = Why::LevelChange;
                InterlockedIncrement(&levelChangePulls);
                MatrixAt(voteAt);
                Line("a partner waits at the level change %08X (%s)", voteHash, coop_names::Name(voteHash));
            }
        }
    }
    if (why == Why::None) return;
    if ((why == Why::HostCheckpoint || why == Why::PartnerReached) && !haveTarget && !ResolveCheckpoint(now)) return;
    Begin(now);
}
void Tick(DWORD now) {
    if (!enabled) return;
    if (stage == Stage::None) {
        CurtainTick(now);
        if (sceneCueHeld && finishedAt && now - finishedAt > 3000) sceneCueHeld = false;
        return;
    }
    if (!inLevel || necro::RestartOrLevelChange()) { End(!inLevel ? "the level went away" : "a restart or level change runs"); return; }
    switch (stage) {
    case Stage::Watching: Watch(now); return;
    case Stage::Triggered:
    case Stage::Scheduled:
    case Stage::Direct:
        if (BodyGone()) {
            Line("the dead body went away while waiting for RespawnPlayer");
            stage = Stage::Watching;   // Begin spawns fresh
            Begin(now);
            return;
        }
        if (now - triggeredAt < 3000 * (stage == Stage::Triggered ? 1u : stage == Stage::Scheduled ? 2u : 3u)) return;
        if (stage == Stage::Triggered) {
            stage = Stage::Scheduled;
            InterlockedIncrement(&fallbacksSchedule);
            Line("no RespawnPlayer 3 s after the trigger: pending bit + pre-tick set directly: %s", respawn::ScheduleDirect(deadPlayer) ? "ok" : "FAILED");
        } else if (stage == Stage::Scheduled) {
            stage = Stage::Direct;
            InterlockedIncrement(&fallbacksDirect);
            Line("still no RespawnPlayer: called directly: %s", respawn::RespawnDirect(deadPlayer) ? "ok" : "FAULTED");
        } else {
            stage = Stage::Failed;
            InterlockedIncrement(&failures);
            LiftCurtain("the respawn failed", now);
            Line("no new Isaac even after the direct call: stays down");
            overlay::ShowBanner("RESPAWN FAILED", "RESTART THE CHECKPOINT FROM THE PAUSE MENU", 10000);
        }
        return;
    case Stage::Respawned: {
        const uint32_t slot0 = LocalCampaignPlayer();
        if (slot0 && slot0 != deadPlayer && now - respawnedAt >= 300) Finish(slot0, now);
        else if (now - respawnedAt > 5000) {
            stage = Stage::Failed;
            InterlockedIncrement(&failures);
            LiftCurtain("no new Isaac in slot 0", now);
            Line("no new Isaac in slot 0 5 s after RespawnPlayer");
        }
        return;
    }
    default: return;
    }
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Spectate", "Enabled", 0, ini.c_str()) != 0;
    cameraMode = GetPrivateProfileIntA("Spectate", "CameraMode", 0, ini.c_str());
    inScenes = GetPrivateProfileIntA("Spectate", "InScenes", 0, ini.c_str()) != 0;
    sceneRespawn = GetPrivateProfileIntA("Spectate", "SceneRespawn", 1, ini.c_str()) != 0;
    levelChangeRespawn = GetPrivateProfileIntA("Spectate", "LevelChangeRespawn", 1, ini.c_str()) != 0;
    corpseHold = GetPrivateProfileIntA("Spectate", "CorpseHold", 1, ini.c_str()) != 0;
    orbit = GetPrivateProfileIntA("Spectate", "Orbit", 0, ini.c_str()) != 0;
    nextKey = GetPrivateProfileIntA("Spectate", "NextKey", VK_LBUTTON, ini.c_str());
    prevKey = GetPrivateProfileIntA("Spectate", "PrevKey", VK_RBUTTON, ini.c_str());
    findWaitMs = static_cast<DWORD>(GetPrivateProfileIntA("Spectate", "FindWaitMs", 8000, ini.c_str()));
    char text[32]{};
    const auto number = [&](const char* key, float fallback, float lo, float hi) {
        GetPrivateProfileStringA("Spectate", key, "", text, sizeof(text), ini.c_str());
        const float v = text[0] ? static_cast<float>(std::atof(text)) : fallback;
        return std::isfinite(v) && v >= lo && v <= hi ? v : fallback;
    };
    distance = number("Distance", 2.5f, 0.5f, 10.0f);
    height = number("Height", 1.6f, 0.0f, 5.0f);
    fovDeg = number("Fov", 60.0f, 30.0f, 110.0f);
    blendSeconds = number("BlendSeconds", 0.75f, 0.0f, 5.0f);
    farFromCheckpointM = number("FarFromCheckpointM", 40.0f, 5.0f, 500.0f);
    if (findWaitMs > 60000) findWaitMs = 8000;
    if (!enabled) { std::fprintf(log, "spectate off ([Spectate] Enabled=0)\n"); return; }
    if (!overlay::ready) overlay::Install(log, ini);   // the banner and the curtain
    const bool hooks = respawn::InstallHooks(log);
    struct Site { const char* name; uint32_t rva; unsigned char bytes[16]; size_t size; };
    static const Site kSites[] = {
        {"ZERO_GUID", kZeroGuidRva, {0xB8, 0x01, 0x00, 0x00, 0x00, 0x84, 0x05, 0x98}, 8},
        {"DefaultSpawn<ObserverCameraInfo>", kDefaultSpawnRva, {0x83, 0xEC, 0x24, 0x56, 0x57, 0xE8, 0x26, 0x5F, 0xFE, 0xFF, 0x8B, 0x4C, 0x24, 0x38, 0x8B, 0x54}, 16},
        {"QueryInterface", kQueryRva, {0x51, 0x8D, 0x04, 0x24, 0x50, 0x51, 0x8B, 0x4C, 0x24, 0x14, 0x8B, 0xC4, 0xC7, 0x44, 0x24, 0x08}, 16},
        {"ObserverCameraInfo::SetSubject", kSetSubjectRva, {0x8B, 0x44, 0x24, 0x04, 0x56, 0x57, 0x8D, 0x79, 0x60, 0x85, 0xC0, 0x74, 0x05, 0x8D, 0x70, 0x10}, 16},
        {"CameraManager::PushCameraInfo", kPushRva, {0x8B, 0x54, 0x24, 0x10, 0xD9, 0x44, 0x24, 0x0C, 0x8B, 0xC1, 0x8B, 0x4C, 0x24, 0x14, 0x51, 0x52}, 16},
        {"CameraManager::PopCameraInfo", kPopRva, {0x8B, 0x44, 0x24, 0x14, 0xD9, 0x44, 0x24, 0x0C, 0x8B, 0x54, 0x24, 0x08, 0x56, 0x8B, 0xF1, 0x8B}, 16},
    };
    cameraOk = true;
    for (const Site& s : kSites) {
        unsigned char actual[16]{};
        const bool ok = Read(base + s.rva, actual, s.size) && std::memcmp(actual, s.bytes, s.size) == 0;
        cameraOk = cameraOk && ok;
        std::fprintf(log, "spectate site %s %08X: %s\n", s.name, static_cast<unsigned>(base + s.rva), ok ? "ok" : "MISMATCH");
    }
    if (!hooks) {
        enabled = false;
        std::fprintf(log, "spectate NOT installed: the death and respawn hooks are missing\n");
        return;
    }
    std::fprintf(log, "spectate on: camera_mode=%d (%s) distance=%.1f height=%.1f fov=%.0f orbit=%d blend=%.2f in_scenes=%d scene_respawn=%d level_change_respawn=%d "
                      "corpse_hold=%d next_key=%d prev_key=%d find_wait_ms=%lu far_from_checkpoint_m=%.0f\n",
                 cameraMode, cameraMode == 0 ? (cameraOk ? "MP observer camera" : "MP observer camera MISSING: the death camera stays") : "the death camera", distance,
                 height, fovDeg, orbit ? 1 : 0, blendSeconds, inScenes ? 1 : 0, sceneRespawn ? 1 : 0, levelChangeRespawn ? 1 : 0, corpseHold ? 1 : 0, nextKey, prevKey,
                 static_cast<unsigned long>(findWaitMs), farFromCheckpointM);
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    static LONG last = -1;
    const LONG sum = decisions + starts * 3 + triggers * 5 + finishes * 7 + failures * 11 + cameraPushes * 13 + ends * 17 + reachIn * 19 + votesIn * 23 + allDead * 29;
    if (sum == last && stage == Stage::None) return;
    last = sum;
    std::fprintf(log, "spectate stage=%s why=%s dead=%08X subject_slot=%u ghost=%08X camera=%08X baseline_epoch=%u | decisions=%ld native=%ld scene_native=%ld swallowed "
                      "drop=%ld screen=%ld starts=%ld duplicates=%ld bad_guid=%ld triggers=%ld respawns=%ld finishes=%ld fallbacks schedule=%ld direct=%ld failures=%ld "
                      "fresh=%ld all_dead=%ld corpse_holds=%ld | camera pushes=%ld pops=%ld failures=%ld faults=%ld switches=%ld | reach_in=%ld ignored=%ld votes_in=%ld "
                      "level_change_pulls=%ld scene_asks=%ld trail_fallbacks=%ld find_waits=%ld restarts_refused=%ld ends=%ld catchup_refused=%ld necro_key_refused=%ld\n",
                 StageName(stage), WhyName(why), deadPlayer, subjectSlot, subjectGhost, cameraInfo, baseEpoch, decisions, decidedNative, decidedScene, swallowed20,
                 swallowed1E, starts, duplicates, guidRefused, triggers, respawns, finishes, fallbacksSchedule, fallbacksDirect, failures, freshSpawns, allDead,
                 corpseHolds, cameraPushes, cameraPops, cameraFailures, cameraFaults, switches, reachIn, reachIgnored, votesIn, levelChangePulls, sceneAsks,
                 trailFallbacks, findWaits, restartsRefused, ends, catchupRefused, necroKeyRefused);
}
}  // namespace spectate
