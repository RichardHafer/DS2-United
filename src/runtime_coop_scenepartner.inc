// Dead Space 2 United by Richard Hafer.
// The partner in cutscenes ([ScenePartner], off by default; analysis/COOP_CUTSCENES_2026-10-18.md).
// Every machine plays a cutscene on its own Isaac and hides the partners' ghosts
// (runtime_coop_cutscene.inc). A scene whose mode is "puppet" shows the partners'
// Isaac ghosts instead, parked beside the local Isaac and playing an Isaac clip, the way
// the engine drives ghosts in a paired attack. Nothing is sent: every machine does this
// for the partners it sees.
//  * Sentient::SetLocalAnimControlAndCacheNetRPCs 0x00EA51E0 (thiscall(bool), ret 4):
//    true caches the replicated animation state, its RPCs and speed instead of playing
//    them (+0x290 |= 0x10, [+0x184] |= 0x200); false replays the caches.
//  * Player::EntityGhostTick 0x0086B940 (vtable +0xF8, thiscall, plain ret) is skipped
//    for a puppet: there the network history moves the ghost (Entity::GhostTick ->
//    SetMatrix) and the replicated aim bends its bones.
//  * Entity::SetMatrix, vtable +0x2C (0x007D1290, thiscall(const float m[16]), ret 4;
//    rows right, up, forward, position, 16-byte aligned) parks it.
//  * Player::SetAnimation, vtable +0x150 (0x0086BBB0, thiscall(id, blend, flag800,
//    noMerge), ret 0x10), id = sdbm(clip). Animated::IsValidStateId 0x0074D500 checks the
//    id against the character's states (banks attach per character, so every "player"
//    clip resident here plays on a ghost too), IsAnimDone 0x0074D420 restarts it,
//    GetCurrentState 0x0074D250 is saved for the release.
//  * The anim view [ent+0x194] byte +0xAE (bone mods) is 0 while the clip plays.
//  * Its movement component (getter 0x00EB8D40) takes collision mode 0
//    (SetCollisionMode 0x00F712A0, thiscall(mode, bool), ret 8; the mode is at +0x224),
//    so a parked capsule cannot block the local Isaac's scripted path.
// Released: bone mods back, the caches replayed (or the saved state when both are empty),
// collision back, then the ghost tick runs again and the network history moves it home.
// A scene part with a clip of its own for the partner (coop_partner_<Isaac's clip>, a state
// of the mod's partner bank in the pool, tools/build_anim_banks.py) makes the scene a puppet
// scene and plays that clip in sync: it starts when Isaac's own clip of the part starts (after
// a pull-to-position walk), once, holding its last frame. Without one the partner stands by.
// Where a synced puppet stands ([ScenePartner] SyncedPlacement): the clips are in the level's
// space like Isaac's. "path" moves the ghost along the clip's m_root every frame (from
// randomizer\ds2coop_partner_paths.bin), for a pose whose root motion is taken out as the
// game does for Isaac; "origin" puts it at the level origin, for a pose that keeps its root
// motion; "isaac" on Isaac's matrix as the part begins.
namespace scenepartner {
bool enabled = false, installed = false, followIsaac = false, collisionOff = true;
bool defaultPuppet = false;
std::string clipName = "unarmed_s_baseidle";
uint32_t clipId = 0;
float sideOffset = 1.0f, backOffset = 0.3f;
std::vector<std::string> puppetScenes, hiddenScenes;
bool allPuppet = false;

constexpr uint32_t kGhostTickRva = 0x46B940, kAnimControlRva = 0xAA51E0, kIsValidStateRva = 0x34D500, kIsAnimDoneRva = 0x34D420;
constexpr uint32_t kCurrentStateRva = 0x34D250, kSetAnimSpeedRva = 0x351F40, kMovementRva = 0xAB8D40, kSetCollisionRva = 0xB712A0;
constexpr uint32_t kSetMatrixSlot = 0x2C, kGhostTickSlot = 0xF8, kSetAnimationSlot = 0x150;
constexpr uint32_t kSetMatrixRva = 0x3D1290, kSetAnimationRva = 0x46BBB0;
constexpr uint32_t kView = 0x194, kCharacter = 0x188, kBoneMods = 0xAE, kCollisionMode = 0x224, kMatrix = 0x90;
constexpr uint32_t kCacheA = 0x2D4, kCacheB = 0x2F4, kPacking = 0x292;
constexpr DWORD kPlaceMs = 2000, kReleaseGraceMs = 600, kRestartMs = 1000;
constexpr unsigned kMaxPuppets = 3;
// push ebp / mov ebp,esp / and esp,-16 / sub esp,0x48 / push esi / push edi / mov esi,ecx
const unsigned char kGhostTickBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x48, 0x56, 0x57, 0x8B, 0xF1};
struct Probe { const char* name; uint32_t rva; unsigned char bytes[8]; unsigned size; };
// the code the module calls, as in the unpacked image (dumps 21076 and 10-09)
const Probe kProbes[] = {
    {"Animated::IsValidStateId", kIsValidStateRva, {0x8B, 0x89, 0x94, 0x01, 0x00, 0x00, 0x32, 0xC0}, 8},
    {"Animated::IsAnimDone", kIsAnimDoneRva, {0x8B, 0x89, 0x94, 0x01, 0x00, 0x00, 0x32, 0xC0}, 8},
    {"Animated::GetCurrentState", kCurrentStateRva, {0x8B, 0x81, 0x94, 0x01, 0x00, 0x00, 0x85, 0xC0}, 8},
    {"Entity::SetMatrix", kSetMatrixRva, {0x8B, 0x44, 0x24, 0x04, 0xF3, 0x0F, 0x10, 0x10}, 8},
    {"Player::SetAnimation", kSetAnimationRva, {0x56, 0x6A, 0x00, 0x8B, 0xF1, 0xE8}, 6},
    {"movement component getter", kMovementRva, {0x85, 0xC9, 0x74, 0x0E, 0x0F, 0xB6, 0x05}, 7},
    {"CMC::SetCollisionMode", kSetCollisionRva, {0x56, 0x8B, 0x74, 0x24, 0x08, 0x57, 0x8B, 0xF9}, 8},
};
bool codeOk = false, collisionCodeOk = false;

struct Puppet {
    uint32_t ghost = 0, vtable = 0, machine = 0, view = 0, character = 0, movement = 0, body = 0;
    uint32_t savedState = 0, savedCollision = 0;
    uint8_t savedBoneMods = 0;
    bool collisionSet = false, animControl = false, synced = false, pathWarned = false;
    DWORD startedAt = 0, restartedAt = 0, syncStartedAt = 0;
    uint32_t part = 0, playing = 0;   // the scene part (sequence) its clip was chosen for, and that clip
    unsigned slot = 0;
};
Puppet puppets[kMaxPuppets]{};
volatile LONG ghostTicksSkipped = 0;
volatile LONG started = 0, released = 0, droppedGone = 0, restarts = 0, faults = 0, stateReapplied = 0, scenesPuppet = 0, scenesHidden = 0;
volatile LONG rejectNoView = 0, rejectCharacter = 0, rejectClip = 0, rejectVtable = 0, sceneUnknown = 0, syncedPlays = 0, syncedMissing = 0;
volatile LONG rejectNoBody = 0, bodyChanged = 0;
// The ghost's body pose buffer and its bone count (coop_uniform's Body: +0x1E0/+0x1E4).
// A ghost still waiting for its body is left alone, and a puppet whose body is swapped
// is let go first (19.10., host crash 7908: the partner's ghost got its body while a
// puppet, and its freed pose buffer was written to later).
constexpr uint32_t kBodyBuffer = 0x1E0, kBodyCount = 0x1E4;
uint32_t BodyOf(uint32_t ghost) {
    uint32_t buffer = 0, count = 0;
    if (!Read(ghost + kBodyBuffer, &buffer, 4) || !Read(ghost + kBodyCount, &count, 4) || !count || count > 132) return 0;
    return buffer;
}
safetyhook::InlineHook ghostTickHook;
// the scene now and its decision
uint32_t sceneSequence = 0;
bool scenePuppet = false;
DWORD wantedUntil = 0;
char sceneStem[32] = "", sceneAnim[64] = "";
// this part's own partner clip and when Isaac's clip of the part begins
constexpr DWORD kPartCaptureMs = 300, kPartStartWaitMs = 3000;
uint32_t syncedId = 0, sceneClipId = 0;
DWORD partSequenceAt = 0, partStartedAt = 0;
bool partRunning = false;
alignas(16) float partMatrix[16]{};
bool partMatrixOk = false;
volatile LONG partStartsByClip = 0, partStartsByWait = 0, pathFrames = 0, pathMissing = 0;
enum Placement { kPlaceIsaac, kPlaceOrigin, kPlacePath };
Placement syncedPlacement = kPlacePath;
const char* const kPlacementNames[] = {"isaac", "origin", "path"};
// m_root's place per frame of each partner clip: 'PPTH', version 1, count, then per clip its
// state hash, frame count and 16 floats a frame (rows right, up, forward, position)
struct RootPath { uint32_t id = 0; uint32_t frames = 0; std::vector<float> rows; };
std::vector<RootPath> rootPaths;
const RootPath* PathFor(uint32_t id) {
    for (const RootPath& p : rootPaths)
        if (p.id == id) return &p;
    return nullptr;
}
void LoadPaths(FILE* log, const std::string& file) {
    rootPaths.clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) { std::fprintf(log, "scene_partner no root paths (%s): synced clips placed by %s\n", file.c_str(), kPlacementNames[syncedPlacement]); return; }
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    uint32_t head[3]{};
    if (data.size() < 12) return;
    std::memcpy(head, data.data(), 12);
    if (head[0] != 0x48545050 || head[1] != 1) { std::fprintf(log, "scene_partner %s is no root path file\n", file.c_str()); return; }
    size_t at = 12;
    for (uint32_t i = 0; i < head[2] && at + 8 <= data.size(); ++i) {
        RootPath path;
        std::memcpy(&path.id, data.data() + at, 4);
        std::memcpy(&path.frames, data.data() + at + 4, 4);
        at += 8;
        const size_t bytes = static_cast<size_t>(path.frames) * 64;
        if (!path.frames || path.frames > 100000 || at + bytes > data.size()) break;
        path.rows.resize(static_cast<size_t>(path.frames) * 16);
        std::memcpy(path.rows.data(), data.data() + at, bytes);
        at += bytes;
        rootPaths.push_back(std::move(path));
    }
    std::fprintf(log, "scene_partner root paths: %zu clips from %s\n", rootPaths.size(), file.c_str());
}

bool IsPuppet(uint32_t ghost) {
    if (!ghost) return false;
    for (const Puppet& p : puppets)
        if (p.ghost == ghost) return true;
    return false;
}
void __fastcall OnGhostTick(void* self) {
    if (IsPuppet(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)))) {
        InterlockedIncrement(&ghostTicksSkipped);
        return;
    }
    ghostTickHook.thiscall<void>(self);
}

// --- engine calls, each fenced -----------------------------------------------------------------
int Filter(unsigned) { InterlockedIncrement(&faults); return EXCEPTION_EXECUTE_HANDLER; }
uint32_t Slot(uint32_t entity, uint32_t slot) {
    uint32_t vtable = 0, fn = 0;
    return Read(entity, &vtable, 4) && Read(vtable + slot, &fn, 4) ? fn : 0;
}
__declspec(noinline) bool ValidState(uint32_t entity, uint32_t id) {
    __try {
        return reinterpret_cast<bool(__thiscall*)(void*, uint32_t)>(base + kIsValidStateRva)(reinterpret_cast<void*>(entity), id);
    } __except (Filter(GetExceptionCode())) { return false; }
}
__declspec(noinline) bool AnimDone(uint32_t entity) {
    __try {
        return reinterpret_cast<bool(__thiscall*)(void*)>(base + kIsAnimDoneRva)(reinterpret_cast<void*>(entity));
    } __except (Filter(GetExceptionCode())) { return false; }
}
__declspec(noinline) uint32_t CurrentState(uint32_t entity) {
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*)>(base + kCurrentStateRva)(reinterpret_cast<void*>(entity));
    } __except (Filter(GetExceptionCode())) { return 0xFFFFFFFFu; }
}
__declspec(noinline) bool PlayState(uint32_t entity, uint32_t id, bool blend) {
    const uint32_t fn = Slot(entity, kSetAnimationSlot);
    if (fn != base + kSetAnimationRva) return false;
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t, bool, bool, bool)>(fn)(reinterpret_cast<void*>(entity), id, blend, false, false);
        reinterpret_cast<void(__thiscall*)(void*, float)>(base + kSetAnimSpeedRva)(reinterpret_cast<void*>(entity), 1.0f);
        return true;
    } __except (Filter(GetExceptionCode())) { return false; }
}
__declspec(noinline) bool AnimControl(uint32_t entity, bool local) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, bool)>(base + kAnimControlRva)(reinterpret_cast<void*>(entity), local);
        return true;
    } __except (Filter(GetExceptionCode())) { return false; }
}
__declspec(noinline) bool SetMatrix(uint32_t entity, const float* m) {
    const uint32_t fn = Slot(entity, kSetMatrixSlot);
    if (fn != base + kSetMatrixRva) return false;
    __try {
        reinterpret_cast<void(__thiscall*)(void*, const float*)>(fn)(reinterpret_cast<void*>(entity), m);
        return true;
    } __except (Filter(GetExceptionCode())) { return false; }
}
__declspec(noinline) uint32_t Movement(uint32_t entity) {
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*)>(base + kMovementRva)(reinterpret_cast<void*>(entity));
    } __except (Filter(GetExceptionCode())) { return 0; }
}
__declspec(noinline) bool SetCollision(uint32_t movement, uint32_t mode) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t, bool)>(base + kSetCollisionRva)(reinterpret_cast<void*>(movement), mode, true);
        return true;
    } __except (Filter(GetExceptionCode())) { return false; }
}

// --- the scene and its mode ---------------------------------------------------------------------
bool InList(const std::vector<std::string>& list, const char* stem) {
    for (const std::string& s : list)
        if (_stricmp(s.c_str(), stem) == 0) return true;
    return false;
}
uint32_t Sdbm(const char* text) {   // HashString_SDBM 0x007423C0: A-Z lowered, h = h * 0x1003F + c
    uint32_t h = 0;
    for (const char* c = text; *c; ++c) h = h * 0x1003F + static_cast<uint32_t>(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
    return h;
}
// The running sequence's Isaac clip and its stem (igc_03_03 for igc_03_03_player_010) from
// the scene table.
bool SceneOf(uint32_t sequence) {
    uint32_t guid[4]{};
    sceneStem[0] = sceneAnim[0] = 0;
    if (!sequence || !Read(sequence + 0x24, guid, 16)) return false;
    for (const auto& s : coop_scenes::kSequences) {
        if (s.guid[0] != guid[0] || s.guid[1] != guid[1] || s.guid[2] != guid[2] || s.guid[3] != guid[3] || !s.anim) continue;
        const char* cut = std::strstr(s.anim, "_player");
        const size_t length = cut ? static_cast<size_t>(cut - s.anim) : std::strlen(s.anim);
        if (length + 1 > sizeof(sceneStem) || std::strlen(s.anim) + 1 > sizeof(sceneAnim)) return false;
        std::memcpy(sceneStem, s.anim, length);
        sceneStem[length] = 0;
        std::snprintf(sceneAnim, sizeof(sceneAnim), "%s", s.anim);
        return true;
    }
    return false;
}
void DecideScene(uint32_t sequence, uint32_t isaac, DWORD now) {
    sceneSequence = sequence;
    const bool known = SceneOf(sequence);
    if (!known) {
        std::snprintf(sceneStem, sizeof(sceneStem), "unknown");
        InterlockedIncrement(&sceneUnknown);
    }
    char partner[96];
    std::snprintf(partner, sizeof(partner), "coop_partner_%s", sceneAnim);
    syncedId = known && ValidState(isaac, Sdbm(partner)) ? Sdbm(partner) : 0;
    sceneClipId = known ? Sdbm(sceneAnim) : 0;
    partSequenceAt = now;
    partStartedAt = 0;
    partRunning = false;
    partMatrixOk = false;
    const char* why = "default";
    if (known && InList(hiddenScenes, sceneStem)) { scenePuppet = false; why = "HiddenScenes"; }
    else if (syncedId) { scenePuppet = true; why = "a partner clip of its own"; }
    else if (known && (allPuppet || InList(puppetScenes, sceneStem))) { scenePuppet = true; why = allPuppet ? "PuppetScenes=*" : "PuppetScenes"; }
    else scenePuppet = known && defaultPuppet;
    InterlockedIncrement(scenePuppet ? &scenesPuppet : &scenesHidden);
    if (scenePuppet && !syncedId) InterlockedIncrement(&syncedMissing);
    gate::Event("scene_partner sequence %08X scene %s (%s): partners %s (%s); their own clip %s %s", sequence, sceneStem, sceneAnim[0] ? sceneAnim : "-",
                scenePuppet ? "stand by as puppets" : "stay hidden", why, partner, syncedId ? "is loaded: played in sync" : "is not loaded: the standby clip");
}

// --- puppets --------------------------------------------------------------------------------
bool StillThere(const Puppet& p) {
    uint32_t vtable = 0, view = 0;
    return p.ghost && Read(p.ghost, &vtable, 4) && vtable == p.vtable && IsGhostPlayer(p.ghost) && MachineOf(p.ghost) == p.machine &&
           Read(p.ghost + kView, &view, 4) && view == p.view;
}
// Isaac's matrix moved to the puppet's place: SideOffset to his right (each further
// puppet on the other side, then further out), BackOffset behind him.
bool PlaceFor(uint32_t isaac, unsigned index, float* m) {
    if (!Read(isaac + kMatrix, m, 64)) return false;
    for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) return false;
    const float side = (index % 2 ? -1.0f : 1.0f) * sideOffset * static_cast<float>(1 + index / 2);
    for (int i = 0; i < 3; ++i) m[12 + i] += side * m[i] - backOffset * m[8 + i];
    return true;
}
void Place(Puppet& p, uint32_t isaac, unsigned index) {
    alignas(16) float m[16];
    if (PlaceFor(isaac, index, m)) SetMatrix(p.ghost, m);
}
bool Start(Puppet& p, unsigned slot, uint32_t ghost, uint32_t isaac, unsigned index, DWORD now) {
    uint32_t vtable = 0, view = 0, character = 0, localCharacter = 0;
    if (!Read(ghost, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva) { InterlockedIncrement(&rejectVtable); return false; }
    if (!Read(ghost + kView, &view, 4) || !view) { InterlockedIncrement(&rejectNoView); return false; }
    if (!Read(ghost + kCharacter, &character, 4) || !Read(isaac + kCharacter, &localCharacter, 4) || !character || character != localCharacter) {
        InterlockedIncrement(&rejectCharacter);
        return false;
    }
    if (!ValidState(ghost, clipId) && !(syncedId && ValidState(ghost, syncedId))) { InterlockedIncrement(&rejectClip); return false; }
    const uint32_t body = BodyOf(ghost);
    if (!body) { InterlockedIncrement(&rejectNoBody); return false; }
    p = Puppet{};
    p.ghost = ghost; p.vtable = vtable; p.machine = MachineOf(ghost); p.view = view; p.character = character; p.slot = slot; p.startedAt = now;
    p.body = body;
    p.savedState = CurrentState(ghost);
    p.animControl = AnimControl(ghost, true);
    Read(view + kBoneMods, &p.savedBoneMods, 1);
    const uint8_t off = 0;
    WriteBytes(view + kBoneMods, &off, 1);
    if (collisionOff && collisionCodeOk) {
        p.movement = Movement(ghost);
        if (p.movement && Read(p.movement + kCollisionMode, &p.savedCollision, 4) && p.savedCollision < 16) p.collisionSet = SetCollision(p.movement, 0);
    }
    Place(p, isaac, index);
    InterlockedIncrement(&started);
    float pos[3]{};
    Read(ghost + 0xC0, pos, 12);
    gate::Event("scene_partner puppet on: partner %u ghost %08X machine %u, scene %s, anim control %d, state before %08X, bone mods %u, collision %s, at (%.2f %.2f %.2f)",
                slot, ghost, p.machine, sceneStem, p.animControl ? 1 : 0, p.savedState, p.savedBoneMods, p.collisionSet ? "off" : "unchanged", pos[0], pos[1], pos[2]);
    return true;
}
// A new scene part: its own partner clip from the part's start, or the standby clip.
// The clip a puppet should play now: the part's own partner clip once Isaac's clip of the
// part runs (started once, from its first frame), the standby clip before and otherwise.
void ChooseClip(Puppet& p, DWORD now) {
    const bool synced = syncedId && partRunning && ValidState(p.ghost, syncedId);
    const uint32_t want = synced ? syncedId : clipId;
    if (p.playing == want && (p.part == sceneSequence || !synced)) {
        p.part = sceneSequence;
        return;
    }
    const bool playing = PlayState(p.ghost, want, !synced);
    p.part = sceneSequence;
    p.playing = want;
    p.synced = synced && playing;
    p.restartedAt = p.syncStartedAt = now;
    if (synced) {
        if (playing) InterlockedIncrement(&syncedPlays);
        gate::Event("scene_partner partner %u: coop_partner_%s %s, in sync with Isaac's clip; placed by %s", p.slot, sceneAnim,
                    playing ? "plays" : "did NOT play", kPlacementNames[syncedPlacement]);
    } else {
        gate::Event("scene_partner partner %u: standby clip %s %s", p.slot, clipName.c_str(), playing ? "plays" : "did NOT play");
    }
}
// Where a synced puppet stands this frame (SyncedPlacement).
void PlaceSynced(Puppet& p, DWORD now) {
    alignas(16) static const float kOrigin[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Placement mode = syncedPlacement;
    const RootPath* path = mode == kPlacePath ? PathFor(syncedId) : nullptr;
    if (mode == kPlacePath && !path) {
        if (!p.pathWarned) {
            p.pathWarned = true;
            InterlockedIncrement(&pathMissing);
            gate::Event("scene_partner partner %u: no root path for coop_partner_%s, placed at the level origin instead", p.slot, sceneAnim);
        }
        mode = kPlaceOrigin;
    }
    if (mode == kPlacePath) {
        const uint32_t frame = std::min<uint32_t>(static_cast<uint32_t>((now - p.syncStartedAt) * 30 / 1000), path->frames - 1);
        alignas(16) float m[16];
        std::memcpy(m, &path->rows[static_cast<size_t>(frame) * 16], sizeof(m));
        if (SetMatrix(p.ghost, m)) InterlockedIncrement(&pathFrames);
    } else if (mode == kPlaceOrigin) {
        if (now - p.syncStartedAt < 400) SetMatrix(p.ghost, kOrigin);
    } else if (partMatrixOk && now - partStartedAt < kPartCaptureMs + 100) {
        SetMatrix(p.ghost, partMatrix);
    }
}
void Release(Puppet& p, const char* why) {
    if (!p.ghost) return;
    const bool there = StillThere(p);
    bool reapplied = false;
    if (there) {
        WriteBytes(p.view + kBoneMods, &p.savedBoneMods, 1);
        if (p.collisionSet && p.movement) SetCollision(p.movement, p.savedCollision);
        uint32_t cacheA = 0, cacheB = 0;
        uint8_t packing = 0;
        Read(p.ghost + kCacheA, &cacheA, 4);
        Read(p.ghost + kCacheB, &cacheB, 4);
        Read(p.ghost + kPacking, &packing, 1);
        if (p.animControl) AnimControl(p.ghost, false);
        // nothing cached to replay: the state from before the scene comes back
        if (!cacheA && !cacheB && !packing && p.savedState != 0xFFFFFFFFu && p.savedState != clipId && ValidState(p.ghost, p.savedState)) {
            reapplied = PlayState(p.ghost, p.savedState, true);
            if (reapplied) InterlockedIncrement(&stateReapplied);
        }
        InterlockedIncrement(&released);
    } else {
        InterlockedIncrement(&droppedGone);
    }
    gate::Event("scene_partner puppet off: partner %u ghost %08X (%s)%s%s", p.slot, p.ghost, why, there ? "" : ", the ghost is gone: nothing called",
                reapplied ? ", its state from before the scene played again" : "");
    p = Puppet{};   // the ghost tick runs again from here
}
void ReleaseAll(const char* why) {
    for (Puppet& p : puppets) Release(p, why);
}
// Level gone: the ghosts went with it, nothing to call.
void ForgetAll() {
    for (Puppet& p : puppets)
        if (p.ghost) { InterlockedIncrement(&droppedGone); p = Puppet{}; }
}

// Game thread, from Poll, before the cutscene module hides anyone.
void Tick(DWORD now) {
    if (!installed) return;
    const uint32_t isaac = inLevel ? LocalCampaignPlayer() : 0;
    if (!isaac) { ForgetAll(); sceneSequence = 0; return; }
    uint32_t sequence = 0;
    unsigned char flags = 0;
    const bool inSequence = Read(isaac + 0x240, &flags, 1) && (flags & 4) && Read(isaac + 0x238, &sequence, 4) && sequence;
    if (inSequence && sequence != sceneSequence) DecideScene(sequence, isaac, now);
    // the part begins when Isaac's own clip of it does (after a pull-to-position walk)
    if (inSequence && !partRunning) {
        const uint32_t state = sceneClipId ? CurrentState(isaac) : 0;
        const bool byClip = sceneClipId && state == sceneClipId;
        if (byClip || now - partSequenceAt >= kPartStartWaitMs) {
            partRunning = true;
            partStartedAt = now;
            InterlockedIncrement(byClip ? &partStartsByClip : &partStartsByWait);
            gate::Event("scene_partner Isaac's clip %s %s after %lu ms (his state %08X)", sceneAnim[0] ? sceneAnim : "-",
                        byClip ? "began" : "was not seen: the part counts as begun", static_cast<unsigned long>(now - partSequenceAt), state);
        }
    }
    // Isaac's matrix over the part's first moments (SyncedPlacement=isaac)
    if (partRunning && now - partStartedAt < kPartCaptureMs) partMatrixOk = Read(isaac + kMatrix, partMatrix, 64);
    const bool want = inSequence && scenePuppet && scenekind::local == scenekind::kCinematic && peerTable.Count();
    if (want) wantedUntil = now + kReleaseGraceMs;
    if (!want && static_cast<LONG>(now - wantedUntil) >= 0) {
        ReleaseAll(inSequence ? "the scene part does not take puppets" : "the scene is over");
        if (!inSequence) sceneSequence = 0;
        return;
    }
    unsigned index = 0;
    for (unsigned s = 0; s < coop_peers::kMaxPeers && index < kMaxPuppets; ++s) {
        const auto& peer = peerTable.peers[s];
        Puppet* mine = nullptr;
        for (Puppet& p : puppets) if (p.ghost && p.slot == s) mine = &p;
        if (!peer.used || !peer.token) { if (mine) Release(*mine, "the partner left"); continue; }
        const uint32_t ghost = GhostForMachine(peer.machine);
        if (mine && (!StillThere(*mine) || mine->ghost != ghost)) {
            Release(*mine, "the partner's ghost changed");
            mine = nullptr;
        }
        if (mine && BodyOf(mine->ghost) != mine->body) {
            InterlockedIncrement(&bodyChanged);
            Release(*mine, "the partner's body changed");
            mine = nullptr;
            continue;   // taken again next frame once the new body is complete
        }
        if (!ghost || !IsGhostPlayer(ghost)) continue;   // a necro ghost or none: it stays hidden
        if (!mine && want) {
            for (Puppet& p : puppets)
                if (!p.ghost) { if (Start(p, s, ghost, isaac, index, now)) mine = &p; break; }
        }
        if (!mine) continue;
        if (inSequence) ChooseClip(*mine, now);
        if (mine->synced) {
            PlaceSynced(*mine, now);
        } else {
            if (followIsaac || now - mine->startedAt < kPlaceMs) Place(*mine, isaac, index);
            // at most one restart a second: a view that kept answering "done" would freeze it on frame 0
            if (now - mine->restartedAt >= kRestartMs && AnimDone(mine->ghost) && PlayState(mine->ghost, clipId, false)) {
                mine->restartedAt = now;
                InterlockedIncrement(&restarts);
            }
        }
        ++index;
    }
}

std::vector<std::string> SplitList(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find(',', at);
        if (end == std::string::npos) end = text.size();
        std::string item = text.substr(at, end - at);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.erase(item.begin());
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.pop_back();
        if (!item.empty()) out.push_back(item);
        at = end + 1;
    }
    return out;
}
float IniFloat(const std::string& ini, const char* name, float fallback, float low, float high) {
    char text[32]{}, def[32]{};
    std::snprintf(def, sizeof(def), "%g", fallback);
    GetPrivateProfileStringA("ScenePartner", name, def, text, sizeof(text), ini.c_str());
    const float v = static_cast<float>(std::atof(text));
    return v >= low && v <= high ? v : fallback;
}
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("ScenePartner", "Enabled", 0, ini.c_str()) != 0;
    if (!enabled) { std::fprintf(log, "scene_partner off ([ScenePartner] Enabled=0)\n"); return; }
    char text[2048]{};
    GetPrivateProfileStringA("ScenePartner", "DefaultMode", "hidden", text, sizeof(text), ini.c_str());
    defaultPuppet = _stricmp(text, "puppet") == 0;
    GetPrivateProfileStringA("ScenePartner", "PuppetScenes", "", text, sizeof(text), ini.c_str());
    puppetScenes = SplitList(text);
    allPuppet = puppetScenes.size() == 1 && puppetScenes[0] == "*";
    GetPrivateProfileStringA("ScenePartner", "HiddenScenes", "", text, sizeof(text), ini.c_str());
    hiddenScenes = SplitList(text);
    GetPrivateProfileStringA("ScenePartner", "Clip", "unarmed_s_baseidle", text, sizeof(text), ini.c_str());
    clipName = text[0] ? text : "unarmed_s_baseidle";
    clipId = Sdbm(clipName.c_str());
    sideOffset = IniFloat(ini, "SideOffset", 1.0f, -5.0f, 5.0f);
    backOffset = IniFloat(ini, "BackOffset", 0.3f, -5.0f, 5.0f);
    followIsaac = GetPrivateProfileIntA("ScenePartner", "Follow", 0, ini.c_str()) != 0;
    collisionOff = GetPrivateProfileIntA("ScenePartner", "PuppetCollision", 0, ini.c_str()) == 0;
    GetPrivateProfileStringA("ScenePartner", "SyncedPlacement", "path", text, sizeof(text), ini.c_str());
    syncedPlacement = _stricmp(text, "origin") == 0 ? kPlaceOrigin : _stricmp(text, "isaac") == 0 ? kPlaceIsaac : kPlacePath;
    LoadPaths(log, "randomizer\\ds2coop_partner_paths.bin");
    codeOk = collisionCodeOk = true;
    for (const Probe& probe : kProbes) {
        unsigned char seen[8]{};
        const bool ok = Read(base + probe.rva, seen, probe.size) && std::memcmp(seen, probe.bytes, probe.size) == 0;
        bool& group = probe.rva == kSetCollisionRva || probe.rva == kMovementRva ? collisionCodeOk : codeOk;
        group = group && ok;
        if (!ok) std::fprintf(log, "scene_partner %s at %08X: bytes differ\n", probe.name, static_cast<unsigned>(base + probe.rva));
    }
    // SetLocalAnimControl is hooked by the npcs pair guard; its tail is checked instead
    static const unsigned char kAnimControlTail[] = {0x8B, 0xF1, 0x8A, 0x86, 0x90, 0x02, 0x00, 0x00};
    unsigned char tail[sizeof(kAnimControlTail)]{};
    const bool controlOk = Read(base + kAnimControlRva + 6, tail, sizeof(tail)) && std::memcmp(tail, kAnimControlTail, sizeof(tail)) == 0;
    if (!controlOk) std::fprintf(log, "scene_partner Sentient::SetLocalAnimControlAndCacheNetRPCs at %08X: bytes differ\n", static_cast<unsigned>(base + kAnimControlRva));
    codeOk = codeOk && controlOk;
    uint32_t tickSlot = 0;
    if (!Read(base + placement::kPlayerPrimaryVtableRva + kGhostTickSlot, &tickSlot, 4) || tickSlot != base + kGhostTickRva) {
        std::fprintf(log, "scene_partner Player vtable +0x%X holds %08X, not EntityGhostTick %08X\n", kGhostTickSlot, tickSlot, static_cast<unsigned>(base + kGhostTickRva));
        codeOk = false;
    }
    if (codeOk)
        installed = hook_site::Install(log, "scene_partner Player::EntityGhostTick", base + kGhostTickRva, kGhostTickBytes, sizeof(kGhostTickBytes),
                                       reinterpret_cast<void*>(OnGhostTick), ghostTickHook);
    std::fprintf(log, "scene_partner %s: default %s, %zu puppet scenes%s, %zu hidden scenes, clip %s (%08X), side %.2f m, back %.2f m, follow %d, "
                      "puppet collision %s; code %s, collision code %s\n",
                 installed ? "installed" : "NOT installed", defaultPuppet ? "puppet" : "hidden", puppetScenes.size(), allPuppet ? " (all)" : "", hiddenScenes.size(),
                 clipName.c_str(), clipId, sideOffset, backOffset, followIsaac ? 1 : 0, collisionOff ? "off while parked" : "kept",
                 codeOk ? "ok" : "MISMATCH", collisionCodeOk ? "ok" : "MISMATCH (collision kept)");
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    std::fprintf(log, "scene_partner parts began with Isaac's clip=%ld after the wait=%ld; synced placement %s, root path frames=%ld missing=%ld\n",
                 InterlockedCompareExchange(&partStartsByClip, 0, 0), InterlockedCompareExchange(&partStartsByWait, 0, 0), kPlacementNames[syncedPlacement],
                 InterlockedCompareExchange(&pathFrames, 0, 0), InterlockedCompareExchange(&pathMissing, 0, 0));
    std::fprintf(log, "scene_partner scenes puppet=%ld hidden=%ld unknown=%ld own_clips played=%ld absent=%ld puppets started=%ld released=%ld gone=%ld "
                      "state_reapplied=%ld restarts=%ld ghost_ticks_skipped=%ld faults=%ld rejected vtable=%ld no_view=%ld character=%ld clip=%ld no_body=%ld body_changed=%ld\n",
                 InterlockedCompareExchange(&scenesPuppet, 0, 0), InterlockedCompareExchange(&scenesHidden, 0, 0), InterlockedCompareExchange(&sceneUnknown, 0, 0),
                 InterlockedCompareExchange(&syncedPlays, 0, 0), InterlockedCompareExchange(&syncedMissing, 0, 0),
                 InterlockedCompareExchange(&started, 0, 0), InterlockedCompareExchange(&released, 0, 0), InterlockedCompareExchange(&droppedGone, 0, 0),
                 InterlockedCompareExchange(&stateReapplied, 0, 0), InterlockedCompareExchange(&restarts, 0, 0), InterlockedCompareExchange(&ghostTicksSkipped, 0, 0),
                 InterlockedCompareExchange(&faults, 0, 0), InterlockedCompareExchange(&rejectVtable, 0, 0), InterlockedCompareExchange(&rejectNoView, 0, 0),
                 InterlockedCompareExchange(&rejectCharacter, 0, 0), InterlockedCompareExchange(&rejectClip, 0, 0),
                 InterlockedCompareExchange(&rejectNoBody, 0, 0), InterlockedCompareExchange(&bodyChanged, 0, 0));
}
}  // namespace scenepartner
