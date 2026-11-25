// Dead Space 2 United by Richard Hafer.
// Survival waves on a multiplayer map ([Survival] in plugins\DS2Survival.ini).
//
// Design, evidence and the test procedure: analysis/DS2_SURVIVAL_2026-10-09.md.
// The director lives next to the machinery it drives: the global enemy pool,
// the forced donor in OnLookup and the FuncSpawn repeat (DriveForcedSpawn).
// Two arrival paths:
//  * vent: one of the map's own slasher FuncSpawns is driven with a pool vent
//    donor, and OnLookup rebinds the donor to the slasher's spawning component
//    (forcedVentSource), so it climbs out of the MP vent;
//  * direct: a prop FuncSpawn of the map's main stream is the engine, the donor
//    takes the prop's place and ApplyPlacement moves it to a nav mesh point out
//    of every player's and bot's sight.
// Everything runs on the thread that also runs the pool's stream update
// (GlobalDonorAllowed refuses pool donors on any other), from the frame tick
// or the state machine tick, whichever is that thread. On-screen text goes
// through the co-op plugin's exported notice queue (DS2Coop_QueueNotice).
namespace survival_rt {
using survival::V3;

survival::Config cfg;
std::string iniPath, bestPath, zoneSpotsPath;
bool installed = false;
FILE* logFile = nullptr;
// For DS2Survival_Ready (the co-op menu asks before a launch): 0 still starting,
// 1 ready, 2 not ready; why and fix are written once, before readiness is set.
volatile LONG readiness = 0;
char notReadyWhy[200] = "", notReadyFix[200] = "";
void NotReady(const char* why, const char* fix) {
    std::snprintf(notReadyWhy, sizeof(notReadyWhy), "%s", why);
    std::snprintf(notReadyFix, sizeof(notReadyFix), "%s", fix);
    InterlockedExchange(&readiness, 2);
    if (logFile) std::fprintf(logFile, "survival readiness: NOT ready: %s | fix: %s\n", notReadyWhy, notReadyFix);
}

// --- engine anchors (RVAs, image base 0x00400000) ---
constexpr uint32_t kServiceRegistry = 0x1C1C09C, kFindService = 0x17DB0, kMapOffset = 4;
constexpr uint32_t kLevelManager = 0x1C1C0A4, kLmState = 0x18, kRunning = 16;
constexpr uint32_t kSimManager = 0x1C1C0A0, kGetSpawnInfo = 0x376440;
constexpr uint32_t kPlayerManager = 0x1C1C0D4, kBasePlayer = 0x3C0, kPlayerVtable = 0x17294D0;
constexpr uint32_t kEntityPos = 0xC0, kEntityForward = 0xB0, kEntityGuid = 0x24, kSpawnerPos = 0xB0;
constexpr uint32_t kSpawnPoints = 0x1C218A0, kSpawnPointCount = 0x1C21920, kSpawnPointVtable = 0x16AC6B4, kSpawnPointMatrix = 0x70;
constexpr uint32_t kCastingTest = 0xB5B180;
constexpr uint32_t kNavManager = 0x1CAD0C0, kSpaceList = 0x20, kSpaceNext = 4, kSpaceName = 8, kSpaceHandle = 0x10;
constexpr uint32_t kCreatePath = 0x1801B0, kNumSegments = 0x17B9A0, kSegmentType = 0x17B9F0, kSurfaceSegment = 0x17BA50,
                   kLinkSegment = 0x17BAC0, kPathDtor = 0x17F180;
constexpr uint32_t kNavSystem = 0x1C2F4CC;
constexpr uint32_t kGlobalPickup = 0x9E7560, kItemDrop = 0x9D7660, kItemDropSlot = 0x1F0, kItemOwner = 0x240, kBaseDelete = 0x3568B0;
constexpr uint32_t kSetHealth = 0xB84C60, kSetHealthSlot = 0x20, kHealthVtable = 0x1764760, kDismemberableVtable = 0x1764C68;
constexpr uint32_t kComponentOwner = 0x10, kHealthCurrent = 0xE8;
constexpr uint32_t kWeaponInventory = 0xC04, kInventoryEntries = 4, kInventorySize = 8, kInventoryEntry = 0xC;
constexpr uint32_t kRdataLo = 0x16A1000, kRdataHi = 0x1880000;
constexpr float kEyeHeight = 1.6f;
constexpr int kMaxWaypoints = 64;

constexpr uint32_t NameHash(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) {
        const char c = (*s >= 'A' && *s <= 'Z') ? static_cast<char>(*s + 32) : *s;
        h = h * 0x1003Fu + static_cast<unsigned char>(c);
    }
    return h;
}

// --- the log ---
// A ring filled on the game thread and printed by the worker. Hard budget for the
// whole process; spawn lines and refusal lines have their own smaller ones.
constexpr unsigned kRingLines = 512, kLineSize = 360;
constexpr LONG kLineBudget = 8000;
char ring[kRingLines][kLineSize]{};
unsigned ringHead = 0, ringCount = 0;
SRWLOCK ringLock = SRWLOCK_INIT;
volatile LONG linesWritten = 0, linesOverBudget = 0, linesLost = 0;
DWORD startTick = 0;
void Event(const char* format, ...) {
    if (InterlockedIncrement(&linesWritten) > kLineBudget) { InterlockedIncrement(&linesOverBudget); return; }
    char text[kLineSize];
    const DWORD since = GetTickCount() - startTick;
    int n = std::snprintf(text, sizeof(text), "survival t=%lu.%03lu ", since / 1000, since % 1000);
    if (n < 0) n = 0;
    va_list args;
    va_start(args, format);
    std::vsnprintf(text + n, sizeof(text) - n, format, args);
    va_end(args);
    AcquireSRWLockExclusive(&ringLock);
    if (ringCount == kRingLines) { ringHead = (ringHead + 1) % kRingLines; --ringCount; InterlockedIncrement(&linesLost); }
    std::memcpy(ring[(ringHead + ringCount) % kRingLines], text, sizeof(text));
    ++ringCount;
    ReleaseSRWLockExclusive(&ringLock);
}
bool Budget(unsigned& counter, unsigned limit) { return ++counter <= limit; }

volatile LONG faults = 0;
uint32_t lastFaultAddress = 0;
int FaultFilter(EXCEPTION_POINTERS* info) {
    lastFaultAddress = info && info->ExceptionRecord ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress)) : 0;
    InterlockedIncrement(&faults);
    return EXCEPTION_EXECUTE_HANDLER;
}

// --- signatures ---
// Every engine function the director calls, checked at start. The bytes and the
// calling conventions are the ones the co-op plugin verified and runs with
// (mpmap, bot nav, bot senses, turret, gadgets) and the watchdog's SetHealth.
struct Site { const char* name; uint32_t rva; unsigned char bytes[18]; unsigned size; bool required; bool ok; };
Site sites[] = {
    // push esi / mov esi,[ecx+0x88] / xor eax,eax / xor edx,edx (thiscall(hash))
    {"ServiceRegistry::Find", kFindService, {0x56, 0x8B, 0xB1, 0x88, 0x00, 0x00, 0x00, 0x33, 0xC0, 0x33, 0xD2}, 11, true, false},
    // mov eax,[esp+0xC] / mov edx,[esp+8] / push esi / push eax / push edx (thiscall, ret 0xC)
    {"SimManager::GetSpawnInfo", kGetSpawnInfo, {0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x54, 0x24, 0x08, 0x56, 0x50, 0x52}, 11, true, false},
    // push ebp / mov ebp,esp / and esp,-16 / sub esp,1ACh / fld [01B625A8h] (cdecl, float in st0)
    {"CollisionCamera::CastingTest", kCastingTest, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xAC, 0x01, 0x00, 0x00, 0xD9, 0x05, 0xA8, 0x25, 0xB6, 0x01}, 18, true, false},
    {"bfx::CreatePolylinePath", kCreatePath, {0xA1, 0xCC, 0xF4, 0x02, 0x02, 0x53, 0x55, 0x8B, 0x68, 0x44, 0x56, 0x57, 0x33, 0xFF, 0x3B, 0xEF}, 16, true, false},
    {"PolylinePath GetNumSegments", kNumSegments, {0xA1, 0xCC, 0xF4, 0x02, 0x02, 0x56, 0x8B, 0x70, 0x44, 0x85, 0xF6, 0x57, 0x8B, 0xF9, 0x74, 0x07}, 16, true, false},
    {"PolylinePath GetSegmentType", kSegmentType, {0xA1, 0xCC, 0xF4, 0x02, 0x02, 0x56, 0x8B, 0x70, 0x44, 0x85, 0xF6, 0x57, 0x8B, 0xF9, 0x74, 0x07}, 16, true, false},
    {"PolylinePath GetSurfaceSegment", kSurfaceSegment, {0xA1, 0xCC, 0xF4, 0x02, 0x02, 0x56, 0x57, 0x8B, 0x78, 0x44, 0x85, 0xFF, 0x8B, 0xF1, 0x74, 0x07}, 16, true, false},
    {"PolylinePath GetLinkSegment", kLinkSegment, {0xA1, 0xCC, 0xF4, 0x02, 0x02, 0x56, 0x8B, 0x70, 0x44, 0x85, 0xF6, 0x57, 0x8B, 0xF9, 0x74, 0x07}, 16, true, false},
    {"~PolylinePathRCPtr", kPathDtor, {0xA1, 0xCC, 0xF4, 0x02, 0x02, 0x85, 0xC0, 0x57, 0x8B, 0xF9, 0x74, 0x2C, 0x56, 0x8B, 0x70, 0x44}, 16, true, false},
    // CreatePolylinePath's head repeats elsewhere; its call into _Internal pins it
    {"CreatePolylinePath internal call", 0x180214, {0xE8, 0x77, 0xFE, 0xFF, 0xFF}, 5, true, false},
    // movss xmm3,[01AA5ED0] / movss xmm0,[esp+4] / xorps (thiscall(float), slot +0x20)
    {"HealthComponent::SetHealth", kSetHealth, {0xF3, 0x0F, 0x10, 0x1D, 0xD0, 0x5E, 0xAA, 0x01, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x04, 0x0F, 0x57}, 16, false, false},
    // mov eax,[esp+4] / sub esp,0x28 / push ebx / mov ebx,[...] (cdecl(guid*, 0))
    {"GetGlobalPickupItem", kGlobalPickup, {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x28, 0x53, 0x8B, 0x1D}, 10, false, false},
    // push ecx / push ebx / push esi / mov esi,ecx / mov eax,[esi+240h] / push edi / test eax,eax (thiscall(pos))
    {"Item::Drop", kItemDrop, {0x51, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x40, 0x02, 0x00, 0x00, 0x57, 0x85, 0xC0}, 14, false, false},
    {"Base::Delete", kBaseDelete, {0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x06, 0x8B, 0x50, 0x08, 0x8B, 0xCE, 0xFF, 0xD2, 0x83, 0x3D}, 16, false, false},
    // the MultiplayerSpawnPoint ctor's list append: count 0x02021920, list 0x020218A0
    {"MultiplayerSpawnPoint list", 0x31A6C, {0xA1, 0x20, 0x19, 0x02, 0x02, 0x83, 0xF8, 0x20, 0x73, 0x0D, 0x89, 0x34, 0x85, 0xA0, 0x18, 0x02, 0x02}, 17, false, false},
    // or byte [ecx+0x724],4 / ret: the flag the awareness pass reads as "forced"
    {"AwarenessComponent::ForcedAware", 0x773260, {0x80, 0x89, 0x24, 0x07, 0x00, 0x00, 0x04, 0xC3}, 8, false, false},
    // HandleEvents: MSG_AWARE at attr+0x20 sets the same flag (pins the vtable check)
    {"AwarenessComponent::HandleEvents", 0xB87C80, {0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x08, 0x8B, 0x56, 0x0C, 0x8B, 0x01, 0x3B, 0x42, 0x20, 0x75, 0x0B, 0x80}, 18, false, false},
    // count [01F612A8] against max [01F612AC]
    {"CadaverousNPC::SetChargingMode", 0xBDA680, {0x80, 0x7C, 0x24, 0x04, 0x00, 0x74, 0x1D, 0xA1, 0xA8, 0x12, 0xF6, 0x01, 0x3B, 0x05, 0xAC, 0x12, 0xF6, 0x01}, 18, false, false},
    // Zombie::SetChargingMode reads ChargeIgnoreVis at attr+0x1CC
    {"Zombie charge ignore-vis read", 0xEC9B0E, {0x8B, 0x46, 0x0C, 0x80, 0xB8, 0xCC, 0x01, 0x00, 0x00, 0x00}, 10, false, false},
    // UpdateApproachTarget reads ChargeStartChance at attr+0x140
    {"Approach charge chance read", 0xBFD94C, {0x8B, 0x41, 0x0C, 0xF3, 0x0F, 0x10, 0x88, 0x40, 0x01, 0x00, 0x00}, 11, false, false},
    // DeadSpaceNPC spawn: cmp byte [edx+0B8h],0 / je / or dword [esi+6F4h],10h (CMD_SetPlayDeadEnabled -> flag bit 0x10)
    {"NPC play dead flag from template", 0xBECEF8, {0x80, 0xBA, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x74, 0x09, 0x83, 0x8E, 0xF4, 0x06, 0x00, 0x00, 0x10}, 16, false, false},
    // RespondToDamage: mov eax,[esi+6F4h] / shr eax,4 / test al,1 / jz (the play dead roll needs bit 0x10)
    {"NPC play dead roll test", 0xC152F6, {0x8B, 0x86, 0xF4, 0x06, 0x00, 0x00, 0xC1, 0xE8, 0x04, 0xA8, 0x01, 0x0F, 0x84}, 13, false, false},
    // UpdateApproachTarget: mov ecx,[esi+70h] / push edi / call IsEntityVisible / test al,al / je (a charge needs sight)
    {"Approach charge sight gate", 0xBFD95C, {0x8B, 0x4E, 0x70, 0x57, 0xE8, 0xEB, 0x33, 0xF7, 0xFF, 0x84, 0xC0, 0x74, 0x52}, 13, false, false},
};
enum SiteIndex { sFind, sSpawnInfo, sCast, sPath, sCount0, sType, sSurface, sLink, sDtor, sInternal, sSetHealth, sPickup, sDrop, sDelete, sSpawnList,
                 sForcedAware, sAwareHandle, sChargeSet, sZombieVisRead, sChargeChanceRead, sPlayDeadFlag, sPlayDeadRoll, sChargeSight, sSiteCount };
static_assert(sizeof(sites) / sizeof(sites[0]) == sSiteCount, "one entry per site");
bool Have(SiteIndex i) { return sites[i].ok; }
bool killReady = false, rewardsReady = false;

// --- engine calls (POD frames only: SEH) ---
__declspec(noinline) uint32_t FindServiceRaw(uint32_t registry, uint32_t hash) {
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t)>(base + kFindService)(reinterpret_cast<void*>(static_cast<uintptr_t>(registry)), hash);
    } __except (FaultFilter(GetExceptionInformation())) {
        return 0;
    }
}
__declspec(noinline) bool LookupRaw(uint32_t manager, ds2::native::SpawnInfo32* info, const Guid* guid) {
    using Lookup = void* (__thiscall*)(void*, ds2::native::SpawnInfo32*, const Guid*, unsigned);
    __try {
        reinterpret_cast<Lookup>(base + kGetSpawnInfo)(reinterpret_cast<void*>(static_cast<uintptr_t>(manager)), info, guid, 0);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        return false;
    }
}
// What the casts cost, for the summary line (one QueryPerformanceCounter pair each).
volatile LONG castMicros = 0, castMicrosMax = 0;
__declspec(noinline) bool CastRaw(const float* from, const float* to, uint32_t exclude, float& fraction, uint32_t& hit) {
    using Cast = float(__cdecl*)(const float*, const float*, const void*, const uint32_t*, uint32_t, float*, uint32_t*);
    alignas(16) float at[4]{};
    static LARGE_INTEGER hz{};
    if (!hz.QuadPart) QueryPerformanceFrequency(&hz);
    LARGE_INTEGER t0{}, t1{};
    QueryPerformanceCounter(&t0);
    bool ok = false;
    __try {
        fraction = reinterpret_cast<Cast>(base + kCastingTest)(from, to, reinterpret_cast<const void*>(static_cast<uintptr_t>(exclude)), nullptr, 0, at, &hit);
        ok = std::isfinite(fraction);
    } __except (FaultFilter(GetExceptionInformation())) {
        ok = false;
    }
    QueryPerformanceCounter(&t1);
    if (hz.QuadPart > 0) {
        const LONG us = static_cast<LONG>((t1.QuadPart - t0.QuadPart) * 1000000 / hz.QuadPart);
        InterlockedExchangeAdd(&castMicros, us);
        if (us > castMicrosMax) InterlockedExchange(&castMicrosMax, us);
    }
    return ok;
}
struct PathSpec { uint32_t blockage, geoMatch, linkUsage; uint8_t usePenalty, pad[3]; float penalty, maxSearch; };
static_assert(sizeof(PathSpec) == 0x18, "bfx::PathSpec");
struct RCPtr { void* proxy; };
// The RCPtr destructor runs on every path: a missed one leaks the NavPower pool.
__declspec(noinline) int PathRaw(uint32_t space, const float* from, const float* to, float (*out)[3], int cap) {
    using Create = RCPtr* (__cdecl*)(RCPtr*, void*, const float*, const float*, uint32_t, const PathSpec*);
    using Count = int(__thiscall*)(const RCPtr*);
    using Type = int(__thiscall*)(const RCPtr*, int);
    using Segment = const uint8_t* (__thiscall*)(const RCPtr*, int);
    using Dtor = void(__thiscall*)(RCPtr*);
    PathSpec spec = {0xFF, 0xFFFFFFFFu, 0u, 0, {0, 0, 0}, 0.0f, 0.0f};
    RCPtr path = {nullptr};
    int n = 0;
    __try {
        reinterpret_cast<Create>(base + kCreatePath)(&path, reinterpret_cast<void*>(static_cast<uintptr_t>(space)), from, to, 0, &spec);
        const int segments = reinterpret_cast<Count>(base + kNumSegments)(&path);
        for (int i = 0; i < segments && n + 1 < cap; ++i) {
            const int type = reinterpret_cast<Type>(base + kSegmentType)(&path, i);
            const uint8_t* segment = reinterpret_cast<Segment>(base + (type == 0 ? kSurfaceSegment : kLinkSegment))(&path, i);
            if (!segment) continue;
            std::memcpy(out[n++], segment + 0x08, 12);
            if (i == segments - 1) std::memcpy(out[n++], segment + 0x14, 12);
        }
    } __except (FaultFilter(GetExceptionInformation())) {
        n = -1;
    }
    __try {
        if (path.proxy) reinterpret_cast<Dtor>(base + kPathDtor)(&path);
    } __except (FaultFilter(GetExceptionInformation())) {
        n = -1;
    }
    return n;
}
__declspec(noinline) uint32_t SpawnPickupRaw(const uint32_t* guid) {
    __try {
        return reinterpret_cast<uint32_t(__cdecl*)(const void*, int)>(base + kGlobalPickup)(guid, 0);
    } __except (FaultFilter(GetExceptionInformation())) {
        return 0;
    }
}
__declspec(noinline) bool DropRaw(uint32_t item, const float* pos) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, const float*)>(base + kItemDrop)(reinterpret_cast<void*>(static_cast<uintptr_t>(item)), pos);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        return false;
    }
}
__declspec(noinline) bool DeleteRaw(uint32_t object) {
    __try {
        reinterpret_cast<void(__cdecl*)(uint32_t)>(base + kBaseDelete)(object);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        return false;
    }
}
__declspec(noinline) bool SetHealthRaw(uint32_t component, float value) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, float)>(base + kSetHealth)(reinterpret_cast<void*>(static_cast<uintptr_t>(component)), value);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        return false;
    }
}

// --- world reads ---
bool ReadV3(uint32_t address, V3& out) {
    float f[3]{};
    if (!Read(address, f, sizeof(f)) || !std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2])) return false;
    out = {f[0], f[1], f[2]};
    return true;
}
uint32_t LmState() {
    uint32_t lm = 0, state = 0xFFFFFFFFu;
    if (Read(base + kLevelManager, &lm, 4) && lm) Read(lm + kLmState, &state, 4);
    return state;
}
// Game thread only: the registry is the game's.
bool CurrentMap(char* out, size_t size) {
    out[0] = 0;
    uint32_t registry = 0;
    if (!Read(base + kServiceRegistry, &registry, 4) || !registry) return false;
    static constexpr uint32_t kLoopHash = NameHash("GameMainLoop");
    const uint32_t loop = FindServiceRaw(registry, kLoopHash);
    char buffer[64]{};
    if (!loop || !Read(loop + kMapOffset, buffer, sizeof(buffer) - 1)) return false;
    std::snprintf(out, size, "%s", buffer);
    return out[0] != 0;
}
struct Viewer { uint32_t entity = 0; V3 feet, eye, forward; bool alive = true; };
// The players in PlayerManager slots, slot 0 (Isaac) first; the bots are Players too.
unsigned ReadViewers(Viewer* out, unsigned cap, uint32_t& local) {
    local = 0;
    uint32_t manager = 0, slots = 0, capacity = 0;
    if (!Read(base + kPlayerManager, &manager, 4) || !manager || !Read(manager + 4, &slots, 4) || !slots ||
        !Read(manager + 8, &capacity, 4) || !capacity || capacity > 8) return 0;
    unsigned n = 0;
    for (uint32_t i = 0; i < capacity && n < cap; ++i) {
        uint32_t bp = 0, vtable = 0;
        if (!Read(slots + 4 * i, &bp, 4) || bp < 0x10000 + kBasePlayer) continue;
        const uint32_t entity = bp - kBasePlayer;
        if (!Read(entity, &vtable, 4) || vtable != base + kPlayerVtable) continue;
        Viewer v;
        v.entity = entity;
        if (!ReadV3(entity + kEntityPos, v.feet)) continue;
        ReadV3(entity + kEntityForward, v.forward);
        v.eye = {v.feet.x, v.feet.y + kEyeHeight, v.feet.z};
        if (i == 0) local = entity;
        out[n++] = v;
    }
    return n;
}
// A component whose vtable is one of the two health vtables and whose owner
// points back at the actor. The offset is remembered per actor class.
struct HealthOffset { uint32_t vtable, offset; };
HealthOffset healthOffsets[64]{};
uint32_t FindHealth(uint32_t actor) {
    const uint32_t health = static_cast<uint32_t>(base + kHealthVtable), dismemberable = static_cast<uint32_t>(base + kDismemberableVtable);
    uint32_t actorVtable = 0;
    if (!Read(actor, &actorVtable, 4) || !actorVtable) return 0;
    HealthOffset& cached = healthOffsets[(actorVtable >> 2) & 63];
    auto ok = [&](uint32_t candidate) {
        uint32_t vtable = 0, owner = 0;
        return candidate >= 0x10000 && Read(candidate, &vtable, 4) && (vtable == health || vtable == dismemberable) &&
               Read(candidate + kComponentOwner, &owner, 4) && owner == actor;
    };
    uint32_t candidate = 0;
    if (cached.vtable == actorVtable && Read(actor + cached.offset, &candidate, 4) && ok(candidate)) return candidate;
    uint32_t window[0x400]{};
    if (!Read(actor, window, sizeof(window))) return 0;
    for (uint32_t i = 0; i < 0x400; ++i) {
        if (!ok(window[i])) continue;
        cached = HealthOffset{actorVtable, i * 4};
        return window[i];
    }
    return 0;
}
// -1 when unreadable.
float HealthOf(uint32_t actor) {
    const uint32_t component = FindHealth(actor);
    float current = -1.0f;
    if (!component || !Read(component + kHealthCurrent, &current, 4) || !std::isfinite(current)) return -1.0f;
    return current;
}
// NavPower's pool: system [0x0202F4CC] -> allocator -> dlmalloc state with dvsize
// +0x08, topsize +0x0C, footprint +0x1AC (dump 2924). False when implausible.
bool NavHeadroom(uint32_t& top, uint32_t& dv, uint32_t& footprint) {
    uint32_t system = 0, allocator = 0, state = 0;
    top = dv = footprint = 0;
    if (!Read(base + kNavSystem, &system, 4) || !system || !Read(system, &allocator, 4) || !allocator || !Read(allocator, &state, 4) || !state) return false;
    if (!Read(state + 0x08, &dv, 4) || !Read(state + 0x0C, &top, 4) || !Read(state + 0x1AC, &footprint, 4)) return false;
    return footprint >= 0x10000 && footprint <= 0x4000000 && top <= 0x4000000 && dv <= footprint;
}
uint32_t LevelSpace() {
    uint32_t manager = 0, ns = 0;
    if (!Read(base + kNavManager, &manager, 4) || !manager || !Read(manager + kSpaceList, &ns, 4)) return 0;
    for (unsigned steps = 0; ns && steps < 16; ++steps) {
        uint32_t name = 1, proxy = 0, space = 0, next = 0;
        if (Read(ns + kSpaceName, &name, 4) && name == 0 && Read(ns + kSpaceHandle, &proxy, 4) && proxy && Read(proxy, &space, 4) && space)
            return ns + kSpaceHandle;
        if (!Read(ns + kSpaceNext, &next, 4) || next == ns) break;
        ns = next;
    }
    return 0;
}
// Where a walk from -> to ends on the mesh and how long it is. False: no level
// space, no path, or a path that does not start at `from`.
bool NavWalk(const V3& from, const V3& to, V3& end, float& length) {
    const uint32_t space = LevelSpace();
    if (!space) return false;
    float a[3] = {from.x, from.y, from.z}, b[3] = {to.x, to.y, to.z};
    float points[kMaxWaypoints][3];
    const int n = PathRaw(space, a, b, points, kMaxWaypoints);
    if (n < 2) return false;
    V3 list[kMaxWaypoints];
    for (int i = 0; i < n; ++i) list[i] = {points[i][0], points[i][1], points[i][2]};
    if (survival::Distance(list[0], from) > 5.0f) return false;
    end = list[n - 1];
    length = survival::PathLength(list, static_cast<size_t>(n));
    return true;
}
volatile LONG casts = 0, castFaults = 0, wallNudges = 0, wallRefusals = 0;
// True when nothing of the world stands between a and b.
bool Clear(const V3& a, const V3& b, uint32_t exclude) {
    alignas(16) float from[4] = {a.x, a.y, a.z, 1.0f}, to[4] = {b.x, b.y, b.z, 1.0f};
    float fraction = 1.0f;
    uint32_t hit = 0;
    InterlockedIncrement(&casts);
    if (!CastRaw(from, to, exclude, fraction, hit)) { InterlockedIncrement(&castFaults); return true; }   // a faulted cast counts as seen
    return fraction >= 0.999f;
}
// Room around a direct spawn spot. A body that appears against a wall can end
// up inside it (09.10.: necromorphs stuck in walls): eight rays at waist
// height; what is closer than WallClearance pushes the spot away.
float RayFraction(const V3& a, const V3& b) {
    alignas(16) float from[4] = {a.x, a.y, a.z, 1.0f}, to[4] = {b.x, b.y, b.z, 1.0f};
    float fraction = 1.0f;
    uint32_t hit = 0;
    InterlockedIncrement(&casts);
    if (!CastRaw(from, to, 0, fraction, hit)) { InterlockedIncrement(&castFaults); return 1.0f; }
    return fraction < 0.0f ? 0.0f : fraction > 1.0f ? 1.0f : fraction;
}
float WallRoom(const V3& spot, V3& push) {
    const float clearance = cfg.wallClearance, reach = clearance + 0.4f;
    float nearest = reach;
    push = {0.0f, 0.0f, 0.0f};
    for (unsigned k = 0; k < 8; ++k) {
        const float angle = static_cast<float>(k) * 0.78539816f;
        const float dx = std::cos(angle), dz = std::sin(angle);
        const V3 from = {spot.x, spot.y + 0.8f, spot.z}, to = {spot.x + dx * reach, spot.y + 0.8f, spot.z + dz * reach};
        const float d = RayFraction(from, to) * reach;
        if (d < nearest) nearest = d;
        if (d < clearance) { push.x -= dx * (clearance - d); push.z -= dz * (clearance - d); }
    }
    return nearest;
}
// true with spot kept or moved to enough room; false when no room was found.
bool KeepOffWalls(V3& spot) {
    if (cfg.wallClearance <= 0.0f) return true;
    V3 push;
    float room = WallRoom(spot, push);
    if (room >= cfg.wallClearance) return true;
    for (unsigned pass = 0; pass < 2; ++pass) {
        const float length = std::sqrt(push.x * push.x + push.z * push.z);
        if (!(length > 0.01f)) break;   // walls all round: no way out
        const float scale = length > cfg.wallClearance ? cfg.wallClearance / length : 1.0f;
        const V3 moved = {spot.x + push.x * scale, spot.y, spot.z + push.z * scale};
        // the step itself must stay in the open
        if (RayFraction({spot.x, spot.y + 0.9f, spot.z}, {moved.x, moved.y + 0.9f, moved.z}) < 0.99f) break;
        spot = moved;
        room = WallRoom(spot, push);
        if (room >= cfg.wallClearance * 0.75f) { InterlockedIncrement(&wallNudges); return true; }
    }
    InterlockedIncrement(&wallRefusals);
    return false;
}
// Seen by a viewer: either of two heights on the spot has a free line to an eye.
bool Seen(const V3& spot, const Viewer* viewers, unsigned count, unsigned& by) {
    for (unsigned i = 0; i < count; ++i) {
        const V3 low = {spot.x, spot.y + 1.0f, spot.z}, high = {spot.x, spot.y + 1.8f, spot.z};
        if (Clear(viewers[i].eye, low, viewers[i].entity) || Clear(viewers[i].eye, high, viewers[i].entity)) { by = i; return true; }
    }
    return false;
}
unsigned ReadSpawnPoints(V3* out, unsigned cap) {
    if (!Have(sSpawnList)) return 0;
    uint32_t count = 0;
    Read(base + kSpawnPointCount, &count, 4);
    unsigned n = 0;
    for (uint32_t i = 0; i < count && i < 32 && n < cap; ++i) {
        uint32_t object = 0, vtable = 0;
        float matrix[16]{};
        if (!Read(base + kSpawnPoints + 4 * i, &object, 4) || !object || !Read(object, &vtable, 4) || vtable != base + kSpawnPointVtable ||
            !Read(object + kSpawnPointMatrix, matrix, sizeof(matrix)) || !std::isfinite(matrix[12])) continue;
        out[n++] = {matrix[12], matrix[13], matrix[14]};
    }
    return n;
}

// --- donors ---
// Pool indices per family for direct spawns; per vent spot and family for vents.
std::map<std::string, std::vector<size_t>> directDonors;
// The enhanced ("super_" package) variants per family, chosen with the
// escalation's chance instead of taking turns with the ordinary ones.
std::map<std::string, std::vector<size_t>> directEnhanced;
std::set<Guid> superGuids;
std::map<std::string, std::string> unavailableWhy;   // filled at install, refined by the probe
// Severed enemies (17.10./18.10.): retargeted copies ([DS2Randomizer] Twitchers), taken instead of
// one of their family with [Survival] TwitcherPercent (slashers) or SeveredPregnantPercent. Direct
// ones per family, vent ones per vent spot and family (their survival keys come from their
// sources, build_survival_vents.py).
std::map<std::string, std::vector<size_t>> twitcherDirect;
size_t twitcherVentSpots = 0, twitcherVentCopies = 0, oracleDirectCopies = 0, oracleVentCopies = 0, oracleVentAlone = 0;
volatile LONG twitcherRolls = 0, twitcherPicks = 0, twitcherVentPicks = 0, twitcherNoCopy = 0;
struct VentSpot {
    const ds2::SurvivalVentRow* row = nullptr;
    ds2::EnemyRecord source;
    std::map<std::string, std::vector<size_t>> donors;
    std::map<std::string, std::vector<size_t>> twitchers;   // Severed copies with this spawner's survival key, per family (MaybeTwitcher)
    uint32_t pointer = 0;
    V3 pos;
    bool resolvable = false;
    int open = -1;   // the vent's selection window as last logged (vent_gate), -1 not yet this run
    std::vector<size_t> markers;   // kSurvivalVentMarkers of its map and kind (wall or floor) it may send its arrival through
    uint32_t lastUseAt = 0, coolUntil = 0;
    unsigned made = 0, refused = 0;
};
std::vector<VentSpot> ventSpots;
struct Engine { const ds2::SurvivalEngineRow* row = nullptr; uint32_t pointer = 0; bool bad = false; unsigned made = 0, failed = 0; };
std::vector<Engine> engines;
std::map<size_t, unsigned> donorFailures;
std::map<std::string, size_t> donorTurn;

const ds2::EnemyRecord* PoolRecord(const Guid& guid) {
    const auto it = std::lower_bound(pool.begin(), pool.end(), guid, [](const auto& record, const Guid& g) { return record.guid < g; });
    return it != pool.end() && it->guid == guid ? &*it : nullptr;
}
bool FamilyPackaged(const std::string& family) {
    for (const auto& packaged : ds2::kPackagedFamilies)
        if (family == packaged.family) return packaged.resident;
    return true;
}
// Map=any: every MP map ("mp_" and the Outbreak "pdlc_mp_" ones) and every
// campaign arena of the co-op plugin ("arena_..."); the spawners of maps not
// loaded never resolve, so only the current map's vents and engines are ever used.
bool MapAllowed(const char* name) {
    if (!name || !name[0]) return false;
    if (survival::EqualNoCase(cfg.map.c_str(), "any")) return coop_mpmap::IsMpMap(name) || coop_arena::IsArenaName(name);
    return survival::EqualNoCase(name, cfg.map.c_str());
}
// A campaign arena of the co-op plugin (DS2Coop_Arena*, runtime_coop_arena.inc):
// while it runs its name is the map, direct spots must lie in its rooms, and
// its marked spots seed the search. Its vents are rows like an MP map's
// (tools/build_survival_vents.py, arena_tables): the campaign's vent spawners in
// its rooms, or borrowed from the chapter's main stream, through the markers in
// its rooms; one behind a door that stays locked is not used.
using ArenaNameFn = int(__cdecl*)(char*, int);
using ArenaInsideFn = int(__cdecl*)(const float*);
using ArenaSpotsFn = int(__cdecl*)(float*, int);
using ArenaPointsFn = int(__cdecl*)(int, float*, int);
ArenaNameFn arenaName = nullptr;
ArenaInsideFn arenaInside = nullptr;
ArenaSpotsFn arenaSpots = nullptr;
ArenaPointsFn arenaPoints = nullptr;   // zone (0) and capsule (1) points of the arena's modes
bool arenaLooked = false;
bool ArenaRunning(char* name, size_t size) {
    if (!arenaLooked) {
        arenaLooked = true;
        if (HMODULE coop = ds2coop::PartnerModule("DS2United.asi")) {
            arenaName = reinterpret_cast<ArenaNameFn>(GetProcAddress(coop, "DS2Coop_Arena"));
            arenaInside = reinterpret_cast<ArenaInsideFn>(GetProcAddress(coop, "DS2Coop_ArenaInside"));
            arenaSpots = reinterpret_cast<ArenaSpotsFn>(GetProcAddress(coop, "DS2Coop_ArenaSpots"));
            arenaPoints = reinterpret_cast<ArenaPointsFn>(GetProcAddress(coop, "DS2Coop_ArenaPoints"));
        }
        Event("arenas: %s", arenaName && arenaInside && arenaSpots ? "DS2United.asi exports found" : "DS2United.asi or its DS2Coop_Arena exports missing: no campaign arenas");
    }
    return arenaName && arenaInside && arenaSpots && arenaName(name, static_cast<int>(size)) == 1;
}
// The model package (skin) of each pool record, from its source's model; null
// when the model is no known skin. Zombies through kSlasherSkins, the rest
// through the pool's own targets.
std::vector<const char*> poolPackage;
const char* PackageOf(size_t index) { return index < poolPackage.size() ? poolPackage[index] : nullptr; }
bool SuperSkin(size_t index) {
    const char* p = PackageOf(index);
    return p && std::strncmp(p, "super_", 6) == 0;
}
// The one direct super slasher (48CECFBF, copied from ch11 arena01) authors
// CMD_SetHealth=0 and dies the moment it spawns, without a death message.
const Guid kZeroHealthDonors[] = {{0x48CECFBFu, 0xFD12B2B2u, 0x4D444E52u, 0x4C4F4F50u}};
unsigned zeroHealthSkipped = 0;
// Ellie's two slashers of street_combat track only "Human NPCs" (in the Marker Test-Bed arena
// none of their 14 ever took a target), author Health 1 and send the gate scene's interrupt
// when hit.
const Guid kStoryDonors[] = {{0x31CAF23Au, 0x84B8CF8Fu, 0x43495250u, 0x32303045u}, {0x31CAF23Bu, 0x818D595Du, 0x43495250u, 0x32303045u}};
bool StoryDonor(const Guid& g) { return std::find(std::begin(kStoryDonors), std::end(kStoryDonors), g) != std::end(kStoryDonors); }
unsigned storyDonorsSkipped = 0;
// A list with nothing but enhanced donors is no source for an enemy that comes as an ordinary one
// (14.11.: wave 4 on mp_escape brought super_puker for every vent puker).
bool OnlyEnhanced(const std::vector<size_t>& list) {
    for (const size_t i : list)
        if (!superGuids.count(pool[i].guid)) return false;
    return !list.empty();
}
size_t learnedLastResort = 0;
bool ItemEnhanced();   // below
// Built once from the pool, which does not change during a session.
void BuildDonors() {
    for (const auto& row : ds2::kGlobalTargets)
        if (std::strncmp(row.package, "super_", 6) == 0) superGuids.insert(row.guid);
    poolPackage.assign(pool.size(), nullptr);
    for (size_t i = 0; i < pool.size(); ++i) {
        for (const auto& skin : ds2::kSlasherSkins)
            if (skin.model == pool[i].model) { poolPackage[i] = skin.package; break; }
        if (!poolPackage[i])
            for (const auto& row : ds2::kGlobalTargets)
                if (row.model == pool[i].model) { poolPackage[i] = row.package; break; }
        // the vent records are copies with GUIDs of their own: by the template list alone every
        // super_leaper and super_puker out of a vent counted as an ordinary one (18.11.)
        if (SuperSkin(i)) superGuids.insert(pool[i].guid);
    }
    for (const auto& row : ds2::kVariantDonors) {
        if (!ds2::rowRetargeted(row)) continue;
        const ds2::EnemyRecord* t = PoolRecord(row.guid);
        if (!t || !t->targetEnabled || !t->retargeted) continue;
        const size_t index = static_cast<size_t>(t - pool.data());
        if (const char* slash = std::strrchr(row.package, '\\')) poolPackage[index] = slash + 1;
        // The oracle twitcher is the enhanced slasher's counterpart (18.10.): it comes when
        // the wave's escalation asks for an enhanced enemy, not through TwitcherPercent.
        if (t->enhanced) {
            superGuids.insert(t->guid);
            if (ds2::directContext(t->context) && survival::Info(t->family)) {
                (cfg.superVariants ? directDonors : directEnhanced)[t->family].push_back(index);
                ++oracleDirectCopies;
            }
            continue;
        }
        if (ds2::directContext(t->context) && survival::Info(t->family)) twitcherDirect[t->family].push_back(index);
    }
    for (size_t i = 0; i < pool.size(); ++i) {
        const auto& r = pool[i];
        if (!r.privateTemplate || !r.variantName.empty() || r.passive || r.model == Guid{} || !ds2::directContext(r.context)) continue;
        if (std::find(std::begin(kZeroHealthDonors), std::end(kZeroHealthDonors), r.guid) != std::end(kZeroHealthDonors)) { ++zeroHealthSkipped; continue; }
        if (StoryDonor(r.guid)) { ++storyDonorsSkipped; continue; }
        if (!survival::Info(r.family)) continue;
        if (!r.targetEnabled && !withdrawnForCampaign.count(r.guid)) {
            if (!unavailableWhy.count(r.family))
                unavailableWhy[r.family] = FamilyPackaged(r.family) ? "target disabled ([Targets] or withdrawn)" : "package trimmed (not forced resident in the pool)";
            continue;
        }
        if (!cfg.superVariants && superGuids.count(r.guid)) { directEnhanced[r.family].push_back(i); continue; }
        directDonors[r.family].push_back(i);
    }
    for (const auto& f : survival::kFamilies)
        if (!directDonors.count(f.family) && !unavailableWhy.count(f.family)) unavailableWhy[f.family] = "no pool template";
    // Versus plays on every MP map, whatever Map says; spawners of other maps never resolve
    for (const auto& row : ds2::kSurvivalVents) {
        if ((!MapAllowed(row.map) && !cfg.versus.enabled) || !row.survival[0]) continue;
        VentSpot spot;
        spot.row = &row;
        spot.source.guid = row.target;
        spot.source.family = row.family;
        spot.source.context = row.context;
        spot.source.vent = row.vent;
        spot.source.ventFreeKey = row.ventFree;
        spot.source.ventOpenKey = row.ventOpen;
        spot.source.sourceEnabled = false;
        spot.source.targetEnabled = false;
        std::map<std::string, std::vector<size_t>> oracles, learnedBad;
        for (const auto& key : ds2::kSurvivalDonorKeys) {
            if (std::strcmp(key.survival, row.survival) != 0) continue;
            const ds2::EnemyRecord* r = PoolRecord(key.donor);
            if (!r || !r->privateTemplate || !r->targetEnabled || r->passive || r->context != row.context) continue;
            if (StoryDonor(r->guid)) { ++storyDonorsSkipped; continue; }
            // [VentFamilies] guards campaign rooms that count deaths; here the
            // watchdog writes off a donor that never becomes an NPC. The learned
            // per record blocklist still applies.
            const bool learned = r->ventBlocked && !(cfg.ventIgnoreFamilyOff && ventFamilyBlockedGuids.count(r->guid));
            if (r->family != row.family && !cfg.ventCrossFamily) continue;
            if (!survival::Info(r->family)) continue;
            if (learned) {
                if (cfg.ventLearnedLastResort && !r->retargeted && r->variantName.empty() && !superGuids.count(r->guid))
                    learnedBad[r->family].push_back(static_cast<size_t>(r - pool.data()));
                continue;
            }
            // an oracle twitcher is one of the vent's enhanced donors (superGuids), the others its Severed rolls
            if (r->retargeted && r->enhanced) { oracles[r->family].push_back(static_cast<size_t>(r - pool.data())); continue; }
            if (r->retargeted) { spot.twitchers[r->family].push_back(static_cast<size_t>(r - pool.data())); continue; }
            if (!r->variantName.empty()) continue;
            spot.donors[r->family].push_back(static_cast<size_t>(r - pool.data()));
        }
        // A campaign room learned that this record gets stuck there; its only ordinary Leaper was
        // blocked that way and every vent was left with enhanced ones. Here it is tried when the
        // vent has no other ordinary donor of the family; the stuck watch retires it (NoteVentFailure).
        for (const auto& f : learnedBad) {
            auto& list = spot.donors[f.first];
            if (!OnlyEnhanced(list) && !list.empty()) continue;
            list.insert(list.end(), f.second.begin(), f.second.end());
            learnedLastResort += f.second.size();
        }
        // Only next to plain donors of its family: PickEscalated falls back to the enhanced list
        // when a vent has nothing else, and the oracle must wait for the escalation (18.10.).
        for (auto& f : oracles) {
            auto plain = spot.donors.find(f.first);
            if (plain == spot.donors.end() || plain->second.empty()) { oracleVentAlone += f.second.size(); continue; }
            plain->second.insert(plain->second.end(), f.second.begin(), f.second.end());
            oracleVentCopies += f.second.size();
        }
        if (!spot.twitchers.empty()) {
            ++twitcherVentSpots;
            for (const auto& f : spot.twitchers) twitcherVentCopies += f.second.size();
        }
        // the arrival animation fits one kind of vent: a wall spawner leaves through wall vents
        const size_t len = std::strlen(row.context);
        const bool wall = len >= 5 && std::strcmp(row.context + len - 5, "|Wall") == 0;
        for (size_t m = 0; m < std::size(ds2::kSurvivalVentMarkers); ++m) {
            const auto& marker = ds2::kSurvivalVentMarkers[m];
            if (std::strcmp(marker.map, row.map) != 0 || marker.wall != wall) continue;
            if (!cfg.ventAnyMarker && marker.vent != row.vent) continue;
            spot.markers.push_back(m);
        }
        ventSpots.push_back(std::move(spot));
    }
    for (const auto& row : ds2::kSurvivalEngines)
        if (MapAllowed(row.map) || cfg.versus.enabled) engines.push_back(Engine{&row});
}
// Versus donor filters: a list's members with this skin.
bool HasPackage(const std::vector<size_t>& list, const std::string& package) {
    for (const size_t i : list)
        if (const char* p = PackageOf(i); p && package == p) return true;
    return false;
}
void FilterPackage(const std::vector<size_t>& list, const std::string& package, std::vector<size_t>& out) {
    out.clear();
    for (const size_t i : list)
        if (const char* p = PackageOf(i); p && package == p) out.push_back(i);
}
bool VentPathOn() {
    return cfg.ventSpawns && settings.ventRebinding && bindingOwnerHook && bindingConstructorHook;
}
bool FamilyHasVent(const std::string& family, bool ordinary = false) {
    if (!VentPathOn()) return false;
    for (const auto& s : ventSpots)
        if (s.resolvable && s.donors.count(family) && !s.donors.at(family).empty() && (!ordinary || !OnlyEnhanced(s.donors.at(family)))) return true;
    return false;
}
// Versus: a vent donor of this skin on a spawner of the current map.
unsigned PackageVentSpots(const std::string& family, const std::string& package) {
    if (!VentPathOn()) return 0;
    unsigned n = 0;
    for (const auto& s : ventSpots)
        if (s.resolvable && s.donors.count(family) && HasPackage(s.donors.at(family), package)) ++n;
    return n;
}
unsigned PackageDirectDonors(const std::string& family, const std::string& package) {
    unsigned n = 0;
    for (const auto* m : {&directDonors, &directEnhanced}) {
        const auto it = m->find(family);
        if (it == m->end()) continue;
        for (const size_t i : it->second)
            if (const char* p = PackageOf(i); p && package == p) ++n;
    }
    return n;
}
bool ItemHasVent(const survival::Item& item) {
    return item.package.empty() ? FamilyHasVent(item.family) : PackageVentSpots(item.family, item.package) > 0;
}

// The registry's spawner with this GUID, still usable.
// The engine's own object registry, for a spawner our constructor list missed: after an
// arena's checkpoint restart (02.11.) both vent spawners of arena_ch01 were "NOT FOUND in the
// spawner registry" and the first wave never started, while the map had them.
constexpr uint32_t kObjectRegistryRva = 0x1C1C0A0, kFindByGuidRva = 0x376CA0;
volatile LONG registryFallbackFound = 0, registryFallbackMissing = 0, registryFallbackFaults = 0;
int RegistryFilter() { InterlockedIncrement(&registryFallbackFaults); return EXCEPTION_EXECUTE_HANDLER; }
uint32_t EngineObjectByGuid(const uint32_t* guid) {
    uint32_t registry = 0;
    if (!Read(base + kObjectRegistryRva, &registry, 4) || !registry) return 0;
    using Find = uint32_t(__thiscall*)(void*, const uint32_t*, uint32_t);
    __try {
        return reinterpret_cast<Find>(base + kFindByGuidRva)(reinterpret_cast<void*>(registry), guid, 0);
    } __except (RegistryFilter()) {
        return 0;
    }
}
// Every entry with the GUID, not the first: after an arena restart the list held the old,
// freed spawners ahead of the rebuilt ones with the same GUIDs, and the first wave never came
// (02.11., "WAITING FOR SPAWN POINTS", moved= rising). Entries whose object is no longer that
// spawner are dropped; when none is usable the engine's registry is asked.
volatile LONG staleSpawnersDropped = 0;
uint32_t KnownSpawnerByGuid(const Guid& guid) {
    uint32_t candidates[8]{};
    unsigned count = 0;
    {
        std::lock_guard<std::mutex> guard(knownSpawnersLock);
        for (const auto& entry : knownSpawners)
            if (entry.pointer && entry.guid == guid && count < std::size(candidates)) candidates[count++] = entry.pointer;
    }
    uint32_t usable = 0;
    for (unsigned i = 0; i < count && !usable; ++i) {
        uint32_t vtable = 0;
        Guid at{};
        if (!Read(candidates[i], &vtable, 4) || vtable != base + kFuncSpawnVtableRva || !Read(candidates[i] + 0x24, at.data(), sizeof(at)) || at != guid) {
            std::lock_guard<std::mutex> guard(knownSpawnersLock);
            for (auto& entry : knownSpawners)
                if (entry.pointer == candidates[i] && entry.guid == guid) entry = KnownSpawner{};
            InterlockedIncrement(&staleSpawnersDropped);
            continue;
        }
        if (KnownSpawnerUsable(candidates[i], guid)) usable = candidates[i];
    }
    if (usable) return usable;
    // game thread only (the director's tick): the registry lookup is not thread safe
    uint32_t object = EngineObjectByGuid(guid.data()), vtable = 0;
    if (object && Read(object, &vtable, 4) && vtable == base + kFuncSpawnVtableRva) {
        bool listed = false;
        for (unsigned i = 0; i < count; ++i) listed = listed || candidates[i] == object;
        if (!listed) {
            std::lock_guard<std::mutex> guard(knownSpawnersLock);
            knownSpawners[knownSpawnerCount++ % kKnownSpawnerCapacity] = KnownSpawner{object, guid, 0};
            if (InterlockedIncrement(&registryFallbackFound) <= 20)
                Event("spawner %08X-%08X was missing from our list; the engine's registry has it at %08X: taken back in", guid[0], guid[1], object);
        }
        return KnownSpawnerUsable(object, guid) ? object : 0;
    }
    InterlockedIncrement(&registryFallbackMissing);
    return 0;
}
// Its own target still has instance data: HandleEvents looks it up before OnLookup.
bool SpawnerTargetResolves(uint32_t spawner) {
    uint32_t manager = 0, attributes = 0;
    Guid target{};
    if (!Read(base + kSimManager, &manager, 4) || !manager || !Read(spawner + kFuncSpawnAttributes, &attributes, 4) || !attributes ||
        !Read(attributes + kSpawnAttrTargetGuid, target.data(), 16)) return false;
    ds2::native::SpawnInfo32 info{};
    return LookupRaw(manager, &info, &target) && info.record && info.attributes;
}
// A donor's instance data is there and belongs to the pool stream the selection accepts.
bool DonorResolves(size_t index) {
    uint32_t manager = 0;
    if (index >= pool.size() || !Read(base + kSimManager, &manager, 4) || !manager) return false;
    ds2::native::SpawnInfo32 info{};
    return LookupRaw(manager, &info, &pool[index].guid) && info.record && info.attributes && info.field08 && GlobalDonorAllowed(info.field08);
}

// --- notices through the co-op plugin ---
using QueueNoticeFn = int(__cdecl*)(const char*);
QueueNoticeFn queueNotice = nullptr;
// The co-op plugin's role: 0 no network, 1 host, 2 client. A client runs no
// director; the host's enemies reach it as ghosts and its notices through the
// side channel.
using NetRoleFn = int(__cdecl*)();
using SurvivalEventFn = void(__cdecl*)(int, int);
using SurvivalProgressFn = void(__cdecl*)(int, int, int, int);
using DefendFn = int(__cdecl*)(const float*, int, float, float, float, float*);
DefendFn defendTick = nullptr;
using CapsuleSendFn = void(__cdecl*)(int, unsigned, const float*, int);
using CapsuleTakeFn = int(__cdecl*)(int*, unsigned*, float*, int*);
using PartnersFn = int(__cdecl*)(float*, int);
CapsuleSendFn capsuleSend = nullptr;
CapsuleTakeFn capsuleTake = nullptr;
PartnersFn partnerPositions = nullptr;
using ZoneShowFn = void(__cdecl*)(int, unsigned, const float*, float, int);
ZoneShowFn zoneShow = nullptr;
using CapsuleMarksFn = void(__cdecl*)(const float*, const int*, int);
CapsuleMarksFn capsuleMarks = nullptr;
using EnemyMarksFn = void(__cdecl*)(const float*, int);
EnemyMarksFn enemyMarks = nullptr;
using VersusRoundFn = int(__cdecl*)(int*, int);
VersusRoundFn versusRound = nullptr;
void (__cdecl* resetRewardSession)() = nullptr;
NetRoleFn netRole = nullptr, takeRewardWave = nullptr, rewardEvents = nullptr, respawnState = nullptr, lastRewardWave = nullptr, teamDown = nullptr, gameMode = nullptr,
          humanDeaths = nullptr, restartCheckpoint = nullptr, zoneObjectives = nullptr, arrivalsReady = nullptr;
SurvivalEventFn survivalEvent = nullptr;
SurvivalProgressFn survivalProgress = nullptr;
bool netRoleLooked = false, clientLogged = false;
int NetRole() {
    if (!netRoleLooked) {
        netRoleLooked = true;
        if (HMODULE coop = ds2coop::PartnerModule("DS2United.asi")) {
            netRole = reinterpret_cast<NetRoleFn>(GetProcAddress(coop, "DS2Coop_NetRole"));
            resetRewardSession = reinterpret_cast<decltype(resetRewardSession)>(GetProcAddress(coop, "DS2Coop_ResetSurvivalRewards"));
            takeRewardWave = reinterpret_cast<NetRoleFn>(GetProcAddress(coop, "DS2Coop_TakeRewardWave"));
            rewardEvents = reinterpret_cast<NetRoleFn>(GetProcAddress(coop, "DS2Coop_RewardEvents"));
            respawnState = reinterpret_cast<NetRoleFn>(GetProcAddress(coop, "DS2Coop_RespawnState"));
            lastRewardWave = reinterpret_cast<NetRoleFn>(GetProcAddress(coop, "DS2Coop_LastRewardWave"));
            teamDown = reinterpret_cast<NetRoleFn>(GetProcAddress(coop, "DS2Coop_TeamDown"));
            gameMode = reinterpret_cast<NetRoleFn>(GetProcAddress(coop, "DS2Coop_GameMode"));
            humanDeaths = reinterpret_cast<NetRoleFn>(GetProcAddress(coop, "DS2Coop_HumanDeaths"));
            restartCheckpoint = reinterpret_cast<NetRoleFn>(GetProcAddress(coop, "DS2Coop_RestartCheckpoint"));
            arrivalsReady = reinterpret_cast<NetRoleFn>(GetProcAddress(coop, "DS2Coop_ArrivalsReady"));
            zoneObjectives = reinterpret_cast<NetRoleFn>(GetProcAddress(coop, "DS2Coop_ZoneObjectives"));
            defendTick = reinterpret_cast<DefendFn>(GetProcAddress(coop, "DS2Coop_DefendTick"));
            capsuleSend = reinterpret_cast<CapsuleSendFn>(GetProcAddress(coop, "DS2Coop_SurvivalCapsuleSend"));
            capsuleTake = reinterpret_cast<CapsuleTakeFn>(GetProcAddress(coop, "DS2Coop_SurvivalCapsule"));
            partnerPositions = reinterpret_cast<PartnersFn>(GetProcAddress(coop, "DS2Coop_PartnerPositions"));
            zoneShow = reinterpret_cast<ZoneShowFn>(GetProcAddress(coop, "DS2Coop_SurvivalZone"));
            capsuleMarks = reinterpret_cast<CapsuleMarksFn>(GetProcAddress(coop, "DS2Coop_SurvivalMarks"));
            enemyMarks = reinterpret_cast<EnemyMarksFn>(GetProcAddress(coop, "DS2Coop_SurvivalEnemyMarks"));
            versusRound = reinterpret_cast<VersusRoundFn>(GetProcAddress(coop, "DS2Coop_VersusRound"));
            survivalEvent = reinterpret_cast<SurvivalEventFn>(GetProcAddress(coop, "DS2Coop_SurvivalEvent"));
            survivalProgress = reinterpret_cast<SurvivalProgressFn>(GetProcAddress(coop, "DS2Coop_SurvivalProgress"));
        }
    }
    return netRole ? netRole() : 0;
}
int clientRewardsTaken = -1, respawnEpochSeen = 0;
bool noticeLooked = false;
unsigned noticeLines = 0;

// The status line on the co-op plugin's overlay ([Hud] in DS2United.ini): drawn
// small at the top centre and sent to the partners there. While it is up the
// RIG notices stay off; events (a wave starts or is cleared, the run's end) show
// for a few seconds under the line.
using HudReadyFn = int(__cdecl*)();
using HudSetFn = void(__cdecl*)(const char*, const char*);
HudReadyFn hudReady = nullptr;
HudSetFn hudSet = nullptr;
bool hudLooked = false;
char hudFlash[96] = "";
uint32_t hudFlashUntil = 0;
constexpr uint32_t kHudFlashMs = 4000;
bool HudOn() {
    if (!cfg.hud) return false;
    if (!hudLooked) {
        hudLooked = true;
        if (HMODULE coop = ds2coop::PartnerModule("DS2United.asi")) {
            hudReady = reinterpret_cast<HudReadyFn>(GetProcAddress(coop, "DS2Coop_HudReady"));
            hudSet = reinterpret_cast<HudSetFn>(GetProcAddress(coop, "DS2Coop_SurvivalHud"));
        }
        Event("hud: %s", hudReady && hudSet ? "DS2United.asi exports found" : "DS2United.asi or its HUD exports missing: RIG notices");
    }
    return hudReady && hudSet && hudReady() == 1;
}
// [Difficulty] Hud in DS2United.ini (DS2Coop_DifficultyHud): the game's difficulty, read
// straight from the global (Options.difficulty 0x01F6031C), ends the line. -1 when off.
using DifficultyHudFn = int(__cdecl*)();
DifficultyHudFn difficultyHud = nullptr;
bool difficultyHudLooked = false;
int HudDifficulty() {
    if (!difficultyHudLooked) {
        difficultyHudLooked = true;
        if (HMODULE coop = ds2coop::PartnerModule("DS2United.asi")) difficultyHud = reinterpret_cast<DifficultyHudFn>(GetProcAddress(coop, "DS2Coop_DifficultyHud"));
        Event("hud: difficulty on the line %s", !difficultyHud ? "off (no DS2Coop_DifficultyHud export)" : difficultyHud() == 1 ? "on" : "off ([Difficulty] Hud=0)");
    }
    if (!difficultyHud || difficultyHud() != 1) return -1;
    int32_t d = -1;
    return Read(base + 0x1B6031C, &d, 4) && coop_difficulty::Valid(d) ? d : -1;
}

// status: the line itself says it (enemies left, a countdown); no flash under it.
void Notice(const char* text, bool status = false) {
    if (HudOn()) {
        if (!status) {
            std::snprintf(hudFlash, sizeof(hudFlash), "%s", text);
            hudFlashUntil = GetTickCount() + kHudFlashMs;
            if (!hudFlashUntil) hudFlashUntil = 1;
        }
        if (Budget(noticeLines, 400)) Event("notice on the HUD%s: %s", status ? " line" : "", text);
        return;
    }
    if (!cfg.notices) return;
    if (!noticeLooked) {
        noticeLooked = true;
        if (HMODULE coop = ds2coop::PartnerModule("DS2United.asi"))
            queueNotice = reinterpret_cast<QueueNoticeFn>(GetProcAddress(coop, "DS2Coop_QueueNotice"));
        Event("notices: %s", queueNotice ? "through DS2United.asi DS2Coop_QueueNotice" : "DS2United.asi or its DS2Coop_QueueNotice export missing; log only");
    }
    const int queued = queueNotice ? queueNotice(text) : 0;
    if (Budget(noticeLines, 400)) Event("notice %s: %s", queued ? "queued" : "NOT queued", text);
}

// --- tracked enemies ---
struct Tracked {
    uint32_t actor = 0, vtable = 0;
    Guid guid{}, donor{};
    LONG serviceEpoch = 0;
    std::string family;
    bool vent = false, unkillable = false;
    unsigned wave = 0;
    survival::Watch watch;
    bool sawFirstPos = false, killSent = false;
    survival::Fate killFate = survival::Fate::Stuck;   // what a kill this director sent counts as
    uint32_t killAt = 0;
    V3 spawnedAt;
    uint32_t forcedAt = 0, targetAt = 0;
    unsigned forces = 0;
    // Losing the target after the first one: since when, how often, and for how long in all.
    uint32_t lostAt = 0, untargetedMs = 0;
    unsigned losses = 0;
    bool justLost = false;
    uintptr_t engine = 0;           // the spawner a direct one came from
    uintptr_t spawner = 0;          // the spawner it came from, direct or vent: it owns the enemy's stream
    unsigned placeChecks = 0, replaced = 0;
    bool awareDone = false, charged = false, playDeadChecked = false;
    bool escalated = false;
    int marker = -1;            // the VentMarker a vent one was sent through (kSurvivalVentMarkers)
    int ownMarker = -1;         // its spawner's own vent, for the comparison
    bool arrivalChecked = false;
    bool super = false;         // Versus: a super slasher (its skin, or the super health alone)
    int targetKind = 0;         // Versus: what it goes for (1 human, 2 necro player, 3 other)
    uint32_t farSince = 0;      // since when it is farther than CatchUpMeters from every living player
};
std::vector<Tracked> tracked;
// Where the vent arrivals came out, 3 s after the spawn: near the chosen vent,
// near the spawner's own (the redirect did not take), or elsewhere.
volatile LONG arrivalsAtChosen = 0, arrivalsAtOwn = 0, arrivalsElsewhere = 0;
unsigned arrivalLines = 0;
std::map<uintptr_t, unsigned> engineMisplaced;   // direct spawns an engine left off their spot, this run
// A vent whose arrivals cannot reach anyone is not used again this run (20.10., the ch09
// mining entry hall arena: the vent 5ACA0989-B86CF919 dropped Pukers and slashers into a
// pocket behind the arena's locked door 5A4AF1F3, where the players saw only their marks
// and the watchdog took each after 15-63 s; analysis/NAVPOWER_FORMAT_2026-10-20.md).
unsigned markerFailures[std::size(ds2::kSurvivalVentMarkers)]{};
bool markerRetired[std::size(ds2::kSurvivalVentMarkers)]{};
volatile LONG ventsRetired = 0, ventRetiredSkips = 0;
volatile LONG replacedSpawns = 0;
volatile LONG requeuedToVent = 0;
volatile LONG walkedNotPlaced = 0, enginesForgiven = 0;
volatile LONG frozenKills = 0;
// Enemies the stuck, frozen, catch-up and placement checks left alone because they were in
// a fight: a grab (either side) or close to a player (engaged_enemy.h, the watchdog's
// WatchdogSpareFights and WatchdogSpareMeters).
volatile LONG sparedFights[5]{};
unsigned sparedLines = 0;

// awareness state (functions below, before PollTracked)
constexpr uint32_t kAwareVtable = 0x17646B0, kAwareHandle = 0xB87C80, kZombieVtable = 0x1783948, kZombieChargeSet = 0xEC9AE0;
constexpr uint32_t kChargeCount = 0x1B612A8, kChargeMax = 0x1B612AC;
constexpr uint32_t kNpcAwareness = 0x71C, kNpcTarget = 0x6F8, kCadFlags = 0x874;
constexpr uint32_t kAwAttr = 0x0C, kAwOwner = 0x10, kAwCount = 0x720, kAwFlags = 0x724;
constexpr uint32_t kAttrTrackSq = 0x00, kAttrAlwaysSq = 0x14, kAttrSpan = 0x18, kAttrAtSpawn = 0x1C, kAttrGroups = 0x50;
constexpr uint32_t kChargeStart = 0x140, kChargeIgnoreVis = 0x1CC;
constexpr uint32_t kGroupPlayer = 0x3A574BC0;   // live value of every working donor's first tracking group
bool awareReady = false, chargeMaxReady = false, zombieChargeReady = false, playDeadReady = false, chargeSightReady = false;
// Play dead (user, 16.10.: arena slashers lay down and got up later): spawn copies the template's
// CMD_SetPlayDeadEnabled into the NPC flag word (+0x6F4 bit 0x10); a limb lost to an attacker
// more than 3 m away then rolls 15 % to lie down. [Survival] NoPlayDead clears the bit.
constexpr uint32_t kNpcFlags = 0x6F4, kNpcPlayDeadEnabled = 0x10;
// Charging without sight (user, 16.10.: slashers crept through the campaign corridors): the
// approach decision (0x00FFD800, once a second) charges only a target IsEntityVisible sees;
// forced awareness gives a target, never sight. During a run its je becomes jo, never taken
// after the test (one byte, so the game thread never sees half of it). [Survival] ChargeUnseen.
constexpr uint32_t kChargeSightJump = 0xBFD967;
constexpr uint8_t kJe = 0x74, kJo = 0x70;
bool chargeSightSet = false;
// Templates whose tracking groups lack the players get GroupPlayer in a free slot for the run.
struct TrackEdit { uint32_t attr; unsigned slot; };
std::vector<TrackEdit> trackEdits;
// Tracking ranges raised for the run ([Survival] TrackMeters). UpdateTrackedEntity
// (0x00F81F90) compares [attr+0x00] live on every pass and drops a player beyond it.
struct RangeEdit { uint32_t attr; float oldSq, newSq; };
std::vector<RangeEdit> rangeEdits;
std::set<uint32_t> awarePackets, chargePackets;
SRWLOCK chargeLock = SRWLOCK_INIT;   // RestoreCharging also runs from the release hooks
volatile LONG levelEndingAsked = 0, levelEndingEpoch = 0;
uint32_t endingPlayer = 0, endingRunningSince = 0;
bool endingSeen = false;
struct ChargeEdit { uint32_t packet; float oldStart, newStart; uint8_t oldVis, newVis; };
std::vector<ChargeEdit> chargeEdits;
uint32_t chargeMaxBefore = 0;
bool chargeMaxSet = false;
struct AwareCounts {
    unsigned forced = 0, repaired = 0, noComponent = 0, mismatch = 0, targets = 0, neverTargeted = 0, charging = 0, writeFails = 0, playDeadOff = 0, groupAdded = 0;
    unsigned rangeRaised = 0, targetsLost = 0, targetsRegained = 0, diedUntargeted = 0;
    float targetSum = 0, targetMax = 0;
};
AwareCounts aw;
unsigned awareLines = 0;
void RestoreCharging();
void RestoreVersusGroups();

// --- the run ---
enum class State { Off, Arming, Wave, Intermission, Dead };
const char* StateName(State s) {
    switch (s) {
    case State::Off: return "off";
    case State::Arming: return "arming";
    case State::Wave: return "wave";
    case State::Intermission: return "intermission";
    case State::Dead: return "dead";
    }
    return "?";
}
struct Counters { unsigned spawned = 0, vent = 0, direct = 0, killed = 0, stuckKills = 0, ventStuckKills = 0, timerKills = 0, inert = 0, vanished = 0, writtenOff = 0, dropped = 0; };
struct Refusals {
    unsigned verdict[static_cast<unsigned>(survival::PointVerdict::Count)]{};
    unsigned searches = 0, searchFailed = 0, noEngine = 0, noDonor = 0, notMade = 0, ownTarget = 0, ventBusy = 0, ventClose = 0, ventSeen = 0,
             ventFar = 0, ventClosed = 0, ventRefused = 0, navBudget = 0, cap = 0, rebindMissed = 0, lockBusy = 0, ventHigh = 0, ventCut = 0,
             ventWinding = 0, ventWindingUsed = 0;
};
struct Run {
    uint32_t clockStartSim = 0;   // game time the clock modes started (bonus fade, MaxRunSeconds)
    State state = State::Off;
    unsigned number = 0, wave = 0, bestBefore = 0;
    bool keepOpenedVents = false, allVentsOpen = false;
    bool placeDividersAtBirth = false;
    unsigned dividerPlacementLines = 0;
    int zoneObjectivesSeen = -2;
    LONG zoneVentResets = -1;
    std::string map;   // the map this run is on (Map=any)
    uint32_t player = 0, armedAt = 0, stateAt = 0, waveAt = 0, lastSpawnAt = 0, pauseUntil = 0, notRunningSince = 0;
    bool probed = false, poolWaitLogged = false, keyWasDown = false, deathSeen = false, navGateOff = false;
    // 18.10.: after a game over the arena's vent spawners (an npc_stream) were not registered yet 2 s
    // after the reload; the one probe found none and the run never started. Probed again while
    // arming until they are there.
    unsigned probeMissing = 0, reprobes = 0;
    // the countdown ran out but the first wave cannot start yet: why, for the HUD and the log
    const char* waitWhy = nullptr;
    uint32_t waitLoggedAt = 0, waitSince = 0;
    uint32_t probedAt = 0, ventRetryAt = 0;
    size_t ventRetryCursor = 0;
    bool gameOver = false;   // every human down (DS2Coop_TeamDown): Dead until the map reloads or the next run arms
    bool adminUsed = false;  // a host tool shaped this run (debug menu HOST tab): no best is saved
    uint32_t xpWave = 0;          // kill XP of the running wave (coop_progress.h)
    bool xpRunPaid = false, xpResultPaid = false;
    uint32_t navLowSince = 0;
    std::set<std::string> available;
    survival::Wave spec;
    std::vector<survival::Item> queue;
    size_t next = 0;
    uint32_t itemSince = 0;
    uint32_t directSince = 0;     // the current item's first direct search: the relax levels count from here
    unsigned itemTries = 0;
    unsigned total = 0;   // killable enemies the wave fields
    unsigned requeued = 0;   // arena enemies left outside and queued again, this run
    unsigned caughtUp = 0;   // enemies left far behind, taken away and queued again, this run
    unsigned ventFallbacks = 0;   // items sent to a vent because the direct path had no donor or engine
    unsigned enginesRested = 0;
    Counters wc, rc;      // this wave, this run
    Refusals refusals;
    unsigned lastLeft = ~0u;
    uint32_t lastLeftNotice = 0, lastPoll = 0, lastNavLog = 0;
    unsigned countdownShown = 0;
    // the direct spawn point search
    bool searching = false, haveReady = false;
    std::vector<V3> seeds;
    size_t seedNext = 0;
    V3 best, ready;
    float bestScore = -1e9f;
    bool haveBest = false;
    uint32_t readyAt = 0;
    unsigned rotation = 0;
    unsigned anchorTurn = 0, anchorIndex = 0, relaxLogged = 0;
    // the pack being placed
    V3 packHead;
    unsigned packIndex = 0;
    uint32_t packAt = 0;
    bool packDirect = false;
    std::vector<V3> recent;    // last spots used
    std::vector<V3> goodSpots; // spots that passed every check, re-checked before reuse
    unsigned goodNext = 0;
    unsigned spawnLines = 0, refuseLines = 0, killLines = 0, poolLines = 0;
    // Mercenaries (DS2Coop_GameMode 1): a clock instead of waves, in game time
    bool mercs = false, mercsOver = false, mercsCleared = false, finalDone = false;
    int64_t clockMs = 0;
    uint32_t clockSim = 0, lastCapsuleTry = 0, stageRetryAt = 0;
    survival::MercsScore ms;
    LONG deathsSeen = -1;
    unsigned supplyAt = 0, capsuleTries = 0, capsulesPlaced = 0, capsulesLost = 0, capsulesExpired = 0, mercsLines = 0;
    unsigned resultBonus = 0;
    bool resultRecord = false;
    // Objective (DS2Coop_GameMode 2): the MP map's own objectives on the same clock
    bool objective = false, objectiveOver = false, objectiveWon = false;
    const ds2::ObjectiveMap* omap = nullptr;
    unsigned objStep = 0, objDone = 0, objKills = 0, objDeaths = 0, objLines = 0;
    uint32_t bonusApplied = 0;   // bit i: the map's TimerSet i was counted
    // the time an objective step brought, shown once: the map's bonus and the step come ~100 ms apart
    int64_t noteAddMs = 0;
    uint32_t noteAt = 0;
    unsigned noteObjective = 0;
    uint32_t breatherUntil = 0, objectiveBegunSim = 0, objectiveEndedSim = 0, defendSim = 0;
    const char* objectiveEnd = "";
    float defendFraction = -1.0f;   // the weakest vulnerable defend target's health, -1 when none
    int defendOpen = 0;
    // Hold the Zone (DS2Coop_GameMode 3): one zone at a time, on the same clock
    bool zone = false, zoneOver = false, zoneCleared = false, zoneHas = false, zoneHadOne = false;
    V3 zoneCenter;
    float zoneProgress = 0.0f, zoneRadius = 0.0f;
    int zoneSpot = -1;   // the recorded spot it stands on, -1 found by the search
    survival::ZoneMode zoneMode = survival::ZoneMode::Empty;
    unsigned zoneHeld = 0, zoneKills = 0, zoneDeaths = 0, zoneLines = 0, zoneTries = 0, zonePlaced = 0, zoneInside = 0, zoneEnemiesInside = 0;
    uint32_t zoneId = 0, zoneSim = 0, zoneLastTry = 0, zoneSearchSince = 0, zoneShownAt = 0;
    int zoneShownState = -1, zoneShownPercent = -1;
    const char* zoneEnd = "";
    // Versus (DS2Coop_GameMode 4, [VersusSlashers]): the AI slashers of a round in
    // place of the map's own; the players fight each other, the director keeps
    // the slasher count and the skins, and the pressure makes super slashers
    bool versus = false, versusPlaying = false, vSuperVent = false;
    int vStage = -1, vRound = 0, vKills[2] = {0, 0};
    unsigned vBase = 0, vObjStep = 0, vSpawned = 0, vSuper = 0, vSuperHealthOnly = 0, vKilled = 0, vPeak = 0, vLines = 0, vQueueLines = 0;
    unsigned vHealthSet = 0, vHealthFailed = 0, vDroppedSeen = 0;
    uint32_t vStartedAt = 0;
    survival::VersusPressure pressure;
    std::vector<uint32_t> vDue;               // when each empty slasher slot may be filled again
    std::vector<std::string> vSkins, vBag;    // the map's skins the pool can field, and the shuffled turn
    std::string vLastSkin;
    std::map<std::string, unsigned> vMade;    // spawned per skin this round
};
Run run;
// Mercenaries, Objective and Zone count stages, not waves; the escalation reads them as waves.
bool ClockRun() { return run.mercs || run.objective || run.zone; }
unsigned EscWave(unsigned wave) { return ClockRun() ? survival::ClockEscWave(wave, cfg) : wave; }
// The host's director hold (debug menu, admin below): spawns and the wave timers stand still.
namespace admin { volatile LONG hold = 0; }
volatile LONG versusRoundMirror = 0, versusStageMirror = 0, versusPressureMirror = 0;   // a Versus run's state for the worker's diagnostics
// An Objective result was shown on this level: its objectives are spent until the
// level is loaded again (the checkpoint restart the result asks for).
bool objectiveSpent = false;
uint32_t restartAskedAt = 0;
// After the ask the next run arms 2 s after the co-op plugin has put the new Isaac
// on the reloaded map (DS2Coop_ArrivalsReady moves), 30 s after the ask at the latest.
constexpr uint32_t kRestartSettleMs = 2000, kRestartMaxWaitMs = 30000;
int restartArrivals = -1;
uint32_t restartReadyAt = 0;
volatile LONG restartWaits = 0, restartTimeouts = 0, restartLastWaitMs = 0;
void NoteRestart(uint32_t now) {
    restartAskedAt = now ? now : 1;
    restartArrivals = arrivalsReady ? arrivalsReady() : -1;
    restartReadyAt = 0;
    // The checkpoint restart reloads the map without a level release (16.10.: after a cleared
    // Objective run the next run on mp_gov played survival, "objectives are spent"). The
    // reloaded map announces obj_1_new before our new Isaac stands, so this happens at the ask.
    const LONG before = static_cast<LONG>(objective_watch::Current());
    objective_watch::Reset();
    vent_gate::Reset();
    objectiveSpent = false;
    Event("restart asked: objective watch (objective %ld announced), vent windows and the spent flag cleared for the reloaded map", before);
}
bool RestartSettled(uint32_t now) {
    const int ready = arrivalsReady ? arrivalsReady() : -1;
    if (!restartReadyAt && ready >= 0 && restartArrivals >= 0 && ready != restartArrivals) {
        restartReadyAt = now ? now : 1;
        Event("restart: our Isaac stands on the reloaded map %u ms after the ask; the next run arms in %u ms", now - restartAskedAt, kRestartSettleMs);
    }
    const bool settled = restartReadyAt && now - restartReadyAt >= kRestartSettleMs;
    if (!settled && now - restartAskedAt < kRestartMaxWaitMs) return false;
    if (!settled) {
        InterlockedIncrement(&restartTimeouts);
        Event("restart: no new Isaac seen %u ms after the ask (ready arrivals %d, %d at the ask%s); the next run arms anyway", now - restartAskedAt, ready,
              restartArrivals, arrivalsReady ? "" : ", DS2Coop_ArrivalsReady missing");
    }
    InterlockedIncrement(&restartWaits);
    InterlockedExchange(&restartLastWaitMs, static_cast<LONG>(now - restartAskedAt));
    // a win or loss message of the old map that came after the ask belongs to no run
    if (objective_watch::Won() || objective_watch::Lost()) {
        objective_watch::ResetResult();
        Event("restart: a result message of the old map arrived after the ask; cleared");
    }
    restartAskedAt = 0;
    restartReadyAt = 0;
    restartArrivals = -1;
    return true;
}
// The end of a run on an MP map reloads the map from its checkpoint, as the pause
// menu's restart does (DS2Coop_RestartCheckpoint: GameMainLoop::CheckpointRestart;
// the partners follow through the checkpoint module): every door, pickup, body and
// script back to the arrival. The next run arms on the reloaded map. Only the host or
// a solo player asks, on an MP map or in a campaign arena (its checkpoint override
// brings the arena back; DS2Coop_ArrivalsReady moves with its new Isaac).
volatile LONG realRestarts = 0, realRestartRefusals = 0;
bool RealRestart(const char* why, uint32_t now) {
    if (NetRole() == 2) return false;
    const bool mpMap = coop_mpmap::IsMpMap(run.map.c_str()) || coop_arena::IsArenaName(run.map.c_str());
    const int asked = mpMap && restartCheckpoint ? restartCheckpoint() : -1;
    if (asked == 1) {
        NoteRestart(now);
        InterlockedIncrement(&realRestarts);
    } else {
        InterlockedIncrement(&realRestartRefusals);
    }
    Event("%s: checkpoint restart %s", why,
          asked == 1 ? "asked, the map reloads" : !mpMap ? "not on an MP map or arena (the next run arms here)"
          : !restartCheckpoint ? "not available (DS2Coop_RestartCheckpoint missing)" : "REFUSED (the next run arms here)");
    return asked == 1;
}
bool InArena() { return coop_arena::IsArenaName(run.map.c_str()); }
// The campaign arenas are bigger than the rooms the window was tuned for (user, 17.10., the
// Ishimura: every direct spawn of a wave at its far end, 17 to 23 m off and a walk of up to 2.2
// times that): there the window is pulled in.
constexpr float kArenaMaxDistance = 18.0f, kArenaIdealDistance = 13.0f, kArenaPathFactor = 1.6f, kArenaVentMaxDistance = 20.0f;
float MaxDistanceHere() { return InArena() ? std::min(cfg.maxDistance, kArenaMaxDistance) : cfg.maxDistance; }
// Past the arena's vent cap once an item has waited 4 s: a room with two vents at one end (the
// Stalker Room, 29 m from its far end) must still get its enemies.
unsigned RelaxLevel(uint32_t now);
float VentMaxDistanceHere(uint32_t now) {
    const bool waited = run.itemSince && now - run.itemSince >= 4000;   // the first relax step
    return InArena() && !waited ? std::min(cfg.ventMaxDistance, kArenaVentMaxDistance) : cfg.ventMaxDistance;
}
// The campaign arenas field no Brute (user, 17.10.): every Brute of a wave is the Ubermorph there
// (one hunts at a time, a second is skipped as always), or nothing on a map without one.
volatile LONG arenaBrutesSwapped = 0, arenaBrutesDropped = 0;
void ArenaBrutes(std::vector<survival::Item>& queue) {
    if (!InArena()) return;
    const bool hunter = run.available.count("Hunter") != 0;
    unsigned swapped = 0, dropped = 0;
    for (auto it = queue.begin(); it != queue.end();) {
        if (!survival::EqualNoCase(it->family, "Brute")) { ++it; continue; }
        if (hunter) {
            it->family = "Hunter";
            it->path = survival::Path::Any;
            ++swapped;
            ++it;
        } else {
            it = queue.erase(it);
            ++dropped;
        }
    }
    if (!swapped && !dropped) return;
    InterlockedExchangeAdd(&arenaBrutesSwapped, static_cast<LONG>(swapped));
    InterlockedExchangeAdd(&arenaBrutesDropped, static_cast<LONG>(dropped));
    Event("arena %s: %u Brute(s) %s", run.map.c_str(), swapped + dropped, hunter ? "become the Ubermorph" : "dropped (no Ubermorph on this map)");
}
// spawns=vents in the arena file: the director spawns only through vents there (user, 16.10.: the tripod room)
const coop_arena::Arena* RunArena() {
    const int i = InArena() ? coop_arena::ArenaIndex(run.map.c_str()) : -1;
    return i >= 0 ? &coop_arena::kArenas[i] : nullptr;
}
bool ArenaVentsOnly() {
    const coop_arena::Arena* a = RunArena();
    return a && a->ventsOnly;
}
// heights= in the arena file (user, 16.10.): 0 any height in its rooms (the default), 1 only on
// a floor a player stands on (Ishimura), 2 nothing above the players (Sprawl hub); -1 off an arena.
int ArenaHeights() {
    const coop_arena::Arena* a = RunArena();
    return a ? a->heights : -1;
}
unsigned StartDelayS() {
    return run.mercs ? cfg.mercs.startDelayS : run.objective ? cfg.objective.startDelayS : run.zone ? cfg.zone.startDelayS : cfg.startDelayS;
}
// Game time in ms ([0x0204D3C0], the clock the turret and the shop use): it stands still in the pause menu.
constexpr uint32_t kSimMsRva = 0x1C4D3C0;
uint32_t SimMs() {
    uint32_t ms = 0;
    return Read(base + kSimMsRva, &ms, 4) ? ms : 0;
}

// --- cleanup on the MP maps ([Cleanup] in DS2Survival.ini) ---
// 13.10., crash 16592 (Havok's refill found no address space left) and the frame rate
// sinking over long runs: 1286 of the world's 2252 rigid bodies belonged to 99 dead
// enemies whose delete-fade never advanced, because a body that does not tick never runs
// its fade step (Entity::Tick 0x007D3070 -> 0x007D2700 -> Base::Delete at 0x007D27C5);
// 73 of them were the watchdog's SetHealth(0) victims. And outside a real online match
// no pickup cap exists (the lifetime manager's pickup bucket is never sized), so a 15-wave
// run left 432 drops. Times are game time: nothing is removed while paused.
namespace cleanup {
constexpr uint32_t kEntityFadeOutRva = 0x3D3220, kRegisteredRva = 0x9B433F, kUnregisteredRva = 0x9B43CD;
constexpr uint32_t kAlpha = 0x158, kFadeFlags = 0x168, kFadeDelete = 4, kItemFlags = 0x248, kItemRegistered = 0x298, kItemFadeSlot = 0x70, kEntityPos = 0xC0;
constexpr uint32_t kNetProxy = 0x5C + 0x0C;
safetyhook::MidHook fadeHook, registerHook, unregisterHook;
bool fadeHooked = false, pickupHooked = false;
volatile LONG active = 0;   // an MP map runs here: set by TickInner
struct Record { uint32_t entity, vtable, simAt; };
constexpr unsigned kFades = 512, kPickups = 512, kKills = 64;
Record fades[kFades]{}, pickups[kPickups]{}, kills[kKills]{};
unsigned fadeHead = 0;
SRWLOCK lock = SRWLOCK_INIT;
volatile LONG fadesSeen = 0, fadesDeleted = 0, fadesProgressed = 0, fadesChanged = 0, fadesGhost = 0, killsNoted = 0, killsDeleted = 0, killsKept = 0,
              killsGone = 0, pickupsRegistered = 0, pickupsUnregistered = 0, pickupsFaded = 0, pickupsDropped = 0, pickupsMost = 0, lines = 0;

// Entity::FadeOut(ms, deleteWhenDone, b), thiscall: every delete-fade with a duration.
void OnEntityFadeOut(SafetyHookContext& ctx) {
    if (!InterlockedCompareExchange(&active, 0, 0) || !ctx.ecx) return;
    const uint32_t ms = *reinterpret_cast<const uint32_t*>(ctx.esp + 4);
    const uint8_t deleteWhenDone = *reinterpret_cast<const uint8_t*>(ctx.esp + 8);
    if (!ms || !deleteWhenDone) return;
    const uint32_t vtable = *reinterpret_cast<const uint32_t*>(ctx.ecx);
    const uint32_t sim = SimMs();
    AcquireSRWLockExclusive(&lock);
    if (!InterlockedCompareExchange(&active, 0, 0)) { ReleaseSRWLockExclusive(&lock); return; }
    fades[fadeHead++ % kFades] = Record{static_cast<uint32_t>(ctx.ecx), vtable, sim};
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement(&fadesSeen);
}
// PickupItem::UpdatePickupRegistrationState: esi registers (0x00DB433F) or unregisters
// (0x00DB43CD). Only the dynamic pickups (+0x248 bit 7: drops, rewards, ammo, credits).
void OnPickupRegistered(SafetyHookContext& ctx) {
    if (!InterlockedCompareExchange(&active, 0, 0) || !ctx.esi) return;
    const uint32_t item = static_cast<uint32_t>(ctx.esi);
    if (!((*reinterpret_cast<const uint32_t*>(item + kItemFlags) >> 7) & 1)) return;
    const uint32_t vtable = *reinterpret_cast<const uint32_t*>(item);
    const uint32_t sim = SimMs();
    AcquireSRWLockExclusive(&lock);
    if (!InterlockedCompareExchange(&active, 0, 0)) { ReleaseSRWLockExclusive(&lock); return; }
    Record* slot = nullptr;
    for (auto& p : pickups) {
        if (p.entity == item) { slot = &p; break; }
        if (!slot && !p.entity) slot = &p;
    }
    if (slot) *slot = Record{item, vtable, sim};
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement(slot ? &pickupsRegistered : &pickupsDropped);
}
void OnPickupUnregistered(SafetyHookContext& ctx) {
    if (!ctx.esi) return;
    AcquireSRWLockExclusive(&lock);
    for (auto& p : pickups)
        if (p.entity == static_cast<uint32_t>(ctx.esi)) { p = Record{}; InterlockedIncrement(&pickupsUnregistered); break; }
    ReleaseSRWLockExclusive(&lock);
}
// The watchdog's SetHealth(0): this actor goes after the delay unless a player is near.
void NoteKill(uint32_t actor) {
    uint32_t vtable = 0;
    if (!cfg.cleanupKills || !actor || !Read(actor, &vtable, 4)) return;
    AcquireSRWLockExclusive(&lock);
    if (!InterlockedCompareExchange(&active, 0, 0)) { ReleaseSRWLockExclusive(&lock); return; }
    for (auto& k : kills)
        if (!k.entity) { k = Record{actor, vtable, SimMs()}; break; }
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement(&killsNoted);
}
void Line(const char* format, ...) {
    if (InterlockedIncrement(&lines) > 80) return;
    char buffer[300];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Event("cleanup %s", buffer);
}
void Install(FILE* log) {
    static const unsigned char kFade[] = {0x80, 0x7C, 0x24, 0x08, 0x00, 0x74, 0x07, 0x83, 0x89, 0x68, 0x01, 0x00, 0x00, 0x04};
    static const unsigned char kRegister[] = {0x8B, 0x86, 0x48, 0x02, 0x00, 0x00, 0x8B, 0x4E, 0x0C, 0x8B, 0x91, 0x80, 0x00, 0x00, 0x00};
    static const unsigned char kUnregister[] = {0x8B, 0x0D, 0xEC, 0x12, 0x02, 0x02, 0x6A, 0x00, 0x53, 0x56, 0xE8};
    const bool deleteOk = Have(sDelete);
    if ((cfg.cleanupFades || cfg.cleanupKills) && deleteOk)
        fadeHooked = cfg.cleanupFades && hook_site::InstallMid(log, "cleanup fade sweeper (Entity::FadeOut)", base + kEntityFadeOutRva, kFade, sizeof(kFade), OnEntityFadeOut, fadeHook);
    if (cfg.pickupCap)
        pickupHooked = hook_site::InstallMid(log, "cleanup pickup register", base + kRegisteredRva, kRegister, sizeof(kRegister), OnPickupRegistered, registerHook) &&
                       hook_site::InstallMid(log, "cleanup pickup unregister", base + kUnregisteredRva, kUnregister, sizeof(kUnregister), OnPickupUnregistered, unregisterHook);
    std::fprintf(log, "cleanup on the MP maps: stuck corpses %s (after %u ms, not within %.0f m of a player), fade sweeper %s (stalled %u ms), pickup cap %s "
                      "(max %u, hard %u, older than %u s, not within %.0f m of a player); Base::Delete %s\n",
                 cfg.cleanupKills && deleteOk ? "deleted" : "kept", cfg.cleanupDelayMs, cfg.killKeepRadius, fadeHooked ? "on" : "OFF", cfg.fadeStallMs,
                 pickupHooked ? "on" : "OFF", cfg.maxPickups, cfg.pickupHardMax, cfg.pickupMinAgeS, cfg.pickupKeepRadius, deleteOk ? "verified" : "MISMATCH");
}
}  // namespace cleanup
// A Mercenaries time capsule: a dropped pickup the director watches.
struct Capsule { uint32_t item = 0, vtable = 0, id = 0; Guid guid{}; V3 pos; bool golden = false; uint32_t bornAt = 0; bool nearSeen = false; };
std::vector<Capsule> capsules;
uint32_t capsuleIdNext = 0;
// Co-op: every partner keeps its own copy of each capsule the host placed
// (pickups are not ghosted); the host's word places and removes them.
void AnnounceCapsule(int kind, const Capsule& c) {
    if (NetRole() != 1 || !capsuleSend) return;
    const float pos[3] = {c.pos.x, c.pos.y, c.pos.z};
    capsuleSend(kind, c.id, pos, c.golden ? 1 : 0);
}
volatile LONG capsulesTaken = 0;
bool SameEntity(uint32_t entity, uint32_t vtable, const Guid& guid) {
    return survival::MatchesEntity(entity, vtable, guid, kEntityGuid,
        [](uint32_t addr, void* dst, size_t size) { return Read(addr, dst, size); });
}
void DeleteCapsules(const char* why) {
    unsigned deleted = 0;
    for (const auto& c : capsules) {
        uint32_t owner = 1;
        // only one still lying where it was dropped: a taken one belongs to a player now
        if (Have(sDelete) && SameEntity(c.item, c.vtable, c.guid) && Read(c.item + kItemOwner, &owner, 4) && !owner && DeleteRaw(c.item)) ++deleted;
    }
    if (!capsules.empty()) Event("mercenaries: %zu capsule(s) cleared (%u deleted): %s", capsules.size(), deleted, why);
    if (!capsules.empty()) AnnounceCapsule(6, Capsule{});   // id 0: every copy
    capsules.clear();
}
survival::Rng rng(1);
std::map<std::string, unsigned> best;
volatile LONG runsStarted = 0, wavesStarted = 0, wavesCleared = 0, totalSpawned = 0, totalKilled = 0, tickFaults = 0, driverFrame = 0, driverMachine = 0;
volatile LONG stateMirror = 0, waveMirror = 0, aliveMirror = 0, leftMirror = 0;

void LoadBest() {
    std::ifstream file(bestPath, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    best = survival::ParseBest(text);
}
void SaveBest() {
    if (FILE* f = std::fopen(bestPath.c_str(), "wb")) {
        const std::string text = survival::FormatBest(best);
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

// A run a host tool shaped is no record.
bool UpdateBestFor(const std::string& key, unsigned value) {
    if (run.adminUsed) {
        Event("no best saved for %s (%u): host tools were used in this run", key.c_str(), value);
        return false;
    }
    return survival::UpdateBest(best, key, value);
}

// OnLookup's refusals of the level's own enemies while a run holds an arena (game thread).
volatile LONG levelSpawnsRefused = 0;
void NoteLevelSpawnRefused(uint32_t spawner, const Guid& target, const std::string& family) {
    const LONG n = InterlockedIncrement(&levelSpawnsRefused);
    if (n <= 40 || n % 25 == 0)
        Event("arena run: level spawner %08X refused its own %s %08X-%08X (#%ld, state %s): the director owns the arena's enemies", spawner, family.c_str(),
              target[0], target[1], n, StateName(static_cast<State>(InterlockedCompareExchange(&stateMirror, 0, 0))));
}
void SetState(State s, uint32_t now) {
    if (run.state != s) Event("state %s -> %s (run %u, wave %u)", StateName(run.state), StateName(s), run.number, run.wave);
    run.state = s;
    run.stateAt = now;
    InterlockedExchange(&stateMirror, static_cast<LONG>(s));
    const LONG owns = s != State::Off && !run.versus && InArena() ? 1 : 0;
    if (InterlockedExchange(&arenaRunOwnsSpawns, owns) != owns)
        Event("arena run %u: the level's own enemy spawners are %s", run.number, owns ? "refused (only the director spawns)" : "free again");
    // the co-op plugin plays the music for it (and tells the partners when hosting);
    // a Mercenaries, Objective or Zone run reports 5, so a death there is back after the delay
    const unsigned reported = (run.mercs || run.objective || run.zone) && s == State::Wave ? 5u : static_cast<unsigned>(s);
    // a Versus run tells nobody: the match keeps its own music and respawns
    if (NetRole() != 2 && survivalEvent && !run.versus) survivalEvent(3, static_cast<int>((run.wave << 8) | reported));
}
unsigned AliveCount() {
    unsigned n = 0;
    for (const auto& t : tracked) if (t.actor) ++n;
    return n;
}
unsigned WaveAlive() {
    unsigned n = 0;
    for (const auto& t : tracked) if (t.actor && t.wave == run.wave && !t.unkillable) ++n;
    return n;
}
// How far a vent marker sits above the walkable floor below it: NavPower snaps a path's
// start onto the mesh, so the first point of a short path from the marker is that floor.
// Once per marker and level; -1 when the mesh gives no answer.
std::vector<float> markerDrop;
std::vector<V3> markerFloor;
LONG markerDropResets = -1;
volatile LONG markerDropLines = 0;
float MarkerDrop(size_t m) {
    const LONG resets = InterlockedCompareExchange(&vent_gate::resets, 0, 0);
    if (resets != markerDropResets || markerDrop.size() != std::size(ds2::kSurvivalVentMarkers)) {
        markerDropResets = resets;
        markerDrop.assign(std::size(ds2::kSurvivalVentMarkers), -2.0f);
        markerFloor.assign(std::size(ds2::kSurvivalVentMarkers), V3{});
        InterlockedExchange(&markerDropLines, 0);
    }
    float& drop = markerDrop[m];
    if (drop > -2.0f) return drop;
    const auto& marker = ds2::kSurvivalVentMarkers[m];
    drop = -1.0f;
    if (const uint32_t space = LevelSpace()) {
        float a[3] = {marker.x, marker.y, marker.z}, b[3] = {marker.x + 0.5f, marker.y, marker.z + 0.5f};
        float points[kMaxWaypoints][3];
        const int n = PathRaw(space, a, b, points, kMaxWaypoints);
        if (n >= 1) {
            drop = std::max(0.0f, marker.y - points[0][1]);
            markerFloor[m] = {points[0][0], points[0][1], points[0][2]};
        }
    }
    if (InterlockedIncrement(&markerDropLines) <= 48) {
        if (drop < 0.0f)
            Event("vent marker %zu on %s at (%.1f %.1f %.1f): no floor found on the mesh, the nearest player's height decides", m, marker.map, marker.x, marker.y, marker.z);
        else
            Event("vent marker %zu on %s at (%.1f %.1f %.1f): %.1f m above its floor%s", m, marker.map, marker.x, marker.y, marker.z, drop,
                  cfg.ventMaxAbove > 0.0f && drop > cfg.ventMaxAbove ? ", left to the necro players" : "");
    }
    return drop;
}
// Whether the living can walk to a vent's floor (13.10. on Solar Array: zombies out of the
// vents of a catwalk nobody can reach, 0 m above its own floor, stood there all wave).
// NavPower ends a path at the closest point it can reach, so a walk from the nearest player
// that stops far from the vent's floor means the vent is cut off. Kept 20 s per marker; a
// walk that gives no answer (player off the mesh, no level space) is not kept and lets it be.
constexpr float kReachSlack = 2.5f;
constexpr uint32_t kReachKeepMs = 20000;
struct MarkerReach { uint32_t at = 0; int state = -1; float gap = 0.0f, length = -1.0f; V3 feet{}; };
std::vector<MarkerReach> markerReach;
LONG markerReachResets = -1;
volatile LONG reachWalks = 0, reachCut = 0, reachUnknown = 0, reachLines = 0, windingLines = 0;
unsigned reachRewalks = 0;   // set per TryVent
bool MarkerReachable(size_t m, const V3& feet, uint32_t now) {
    const LONG resets = InterlockedCompareExchange(&vent_gate::resets, 0, 0);
    if (resets != markerReachResets || markerReach.size() != std::size(ds2::kSurvivalVentMarkers)) {
        markerReachResets = resets;
        markerReach.assign(std::size(ds2::kSurvivalVentMarkers), MarkerReach{});
    }
    MarkerReach& r = markerReach[m];
    // a walk counted from a player who has since moved on says little about its length; a few
    // of those are walked again per vent search (each walk is a NavPower path)
    if (r.state >= 0 && now - r.at < kReachKeepMs) {
        if (survival::Distance(r.feet, feet) <= 6.0f || !reachRewalks) return r.state == 1;
        --reachRewalks;
    }
    if (MarkerDrop(m) < 0.0f) return true;   // no floor known: the height rule decides
    const V3 floor = markerFloor[m];
    V3 end;
    float length = 0.0f;
    InterlockedIncrement(&reachWalks);
    if (!NavWalk(feet, floor, end, length)) { InterlockedIncrement(&reachUnknown); return true; }
    const float gap = survival::Distance(end, floor);
    const int state = gap <= kReachSlack ? 1 : 0;
    if (state != r.state && InterlockedIncrement(&reachLines) <= 64) {
        const auto& marker = ds2::kSurvivalVentMarkers[m];
        Event("vent marker %zu on %s at (%.1f %.1f %.1f): %s, the walk from (%.1f %.1f %.1f) ends %.1f m from its floor (%.1f m long)", m, marker.map,
              marker.x, marker.y, marker.z, state ? "reachable" : "CUT OFF, not used", feet.x, feet.y, feet.z, gap, length);
    }
    if (!state) InterlockedIncrement(&reachCut);
    r = MarkerReach{now, state, gap, length, feet};
    return state == 1;
}
// The walk to a reachable vent's floor from MarkerReachable's last answer, -1 when unknown.
float MarkerWalk(size_t m) {
    return m < markerReach.size() && markerReach[m].state == 1 ? markerReach[m].length : -1.0f;
}

// The last few of a wave on the screen (13.10.: one stuck somewhere kept the wave open
// more and more often): at ShowLastEnemies or fewer left, with nothing more to spawn,
// every one still alive is marked in the overlay, on the partners' screens too.
DWORD lastEnemyMarksAt = 0;
bool enemyMarksShown = false;
volatile LONG enemyMarkSets = 0;
unsigned Left();
void LastEnemyMarks(uint32_t now) {
    NetRole();   // resolves the exports
    if (!enemyMarks || now - lastEnemyMarksAt < 500) return;
    lastEnemyMarksAt = now;
    const unsigned left = Left();
    float xyz[8 * 3]{};
    int n = 0;
    unsigned noActor = 0, otherWave = 0, roaming = 0, noPos = 0;
    const bool wanted = cfg.showLastEnemies && left && left <= cfg.showLastEnemies;
    if (wanted && run.next >= run.queue.size())
        for (const auto& t : tracked) {
            V3 p;
            if (n >= 8) break;
            if (!t.actor) { ++noActor; continue; }
            if (t.unkillable) { ++roaming; continue; }
            if (t.wave != run.wave) { ++otherWave; continue; }
            if (!ReadV3(t.actor + kEntityPos, p)) { ++noPos; continue; }
            xyz[n * 3] = p.x;
            xyz[n * 3 + 1] = p.y;
            xyz[n * 3 + 2] = p.z;
            ++n;
        }
    // every change of what is marked, and why one left is not (15.10. Concourse: the last
    // Exploder stood unmarked for most of its 54 s)
    static int lastN = -1;
    static unsigned lastLeft = 0;
    if (wanted && (n != lastN || left != lastLeft) && Budget(run.spawnLines, cfg.logSpawnLines))
        Event("wave %u: %u left, %d marked (queue %zu of %zu spawned; not marked: no actor %u, other wave %u, Ubermorph %u, no position %u)", run.wave, left, n,
              run.next, run.queue.size(), noActor, otherWave, roaming, noPos);
    lastN = n;
    lastLeft = left;
    if (!n && !enemyMarksShown) return;
    if (n && !enemyMarksShown) Event("wave %u: %u left and nothing more to spawn, %d marked on the players' screens", run.wave, left, n);
    enemyMarksShown = n > 0;
    enemyMarks(xyz, n);
    InterlockedIncrement(&enemyMarkSets);
}
// What must stay loaded in a campaign arena (DS2Survival_KeepPoints, for DS2Coop's stream
// sets): every living enemy and the spawner it came out of. An enemy belongs to its
// spawner's stream and goes with it (16.10.: seven at once in the crypt when the player
// walked towards the lift and that room's set was dropped).
SRWLOCK keepLock = SRWLOCK_INIT;
constexpr int kKeepMost = 64;
float keepPoints[kKeepMost * 3]{};
int keepCount = 0;
uint32_t keepAt = 0;
volatile LONG keepUpdates = 0, keepMost = 0;
void UpdateKeepPoints(uint32_t now) {
    if (now - keepAt < 500) return;
    keepAt = now;
    float xyz[kKeepMost * 3];
    int n = 0;
    if (InArena())
        for (const auto& t : tracked) {
            if (!t.actor) continue;
            V3 p;
            if (n < kKeepMost && ReadV3(t.actor + kEntityPos, p)) { xyz[n * 3] = p.x; xyz[n * 3 + 1] = p.y; xyz[n * 3 + 2] = p.z; ++n; }
            if (n < kKeepMost && t.spawner && ReadV3(t.spawner + kSpawnerPos, p)) { xyz[n * 3] = p.x; xyz[n * 3 + 1] = p.y; xyz[n * 3 + 2] = p.z; ++n; }
        }
    AcquireSRWLockExclusive(&keepLock);
    std::memcpy(keepPoints, xyz, sizeof(float) * 3 * static_cast<size_t>(n));
    keepCount = n;
    ReleaseSRWLockExclusive(&keepLock);
    InterlockedIncrement(&keepUpdates);
    if (n > InterlockedCompareExchange(&keepMost, 0, 0)) InterlockedExchange(&keepMost, n);
}
// Any thread.
int KeepPoints(float* out, int max) {
    if (!out || max <= 0) return 0;
    AcquireSRWLockShared(&keepLock);
    const int n = keepCount < max ? keepCount : max;
    std::memcpy(out, keepPoints, sizeof(float) * 3 * static_cast<size_t>(n));
    ReleaseSRWLockShared(&keepLock);
    return n;
}
unsigned Left() {
    const unsigned done = run.wc.killed + run.wc.stuckKills + run.wc.ventStuckKills + run.wc.timerKills + run.wc.inert + run.wc.vanished + run.wc.writtenOff +
                          run.wc.dropped;
    return run.total > done ? run.total - done : 0;
}

// The HUD line for the state the run is in; empty when no run is on.
float mercsNearestCapsule = -1.0f;   // metres from the nearest living player, for the HUD
void MercsHudLine(uint32_t now, char* out, size_t size) {
    auto secondsUntil = [&](uint32_t at) { return static_cast<int32_t>(at - now) > 0 ? (at - now + 999) / 1000 : 0u; };
    const auto& s = run.ms;
    switch (run.state) {
    case State::Arming:
        std::snprintf(out, size, "MERCENARIES  -  %u S ON THE CLOCK  -  START IN %u S", cfg.mercs.startSeconds, secondsUntil(run.armedAt + StartDelayS() * 1000u));
        break;
    case State::Wave: {
        const unsigned left = static_cast<unsigned>((run.clockMs + 999) / 1000);
        char capsule[24] = "";
        if (mercsNearestCapsule >= 0.0f) std::snprintf(capsule, sizeof(capsule), "  |  CAPSULE %.0f M", mercsNearestCapsule);
        std::snprintf(out, size, "%u:%02u  |  KILLS %u  |  COMBO %u  |  SCORE %u%s", left / 60, left % 60, s.kills, s.combo, s.score, capsule);
        break;
    }
    case State::Dead:
        std::snprintf(out, size, "%s  -  SCORE %u  -  RANK %s  -  KILLS %u  -  BEST COMBO %u%s", run.mercsCleared ? "CLEARED" : run.mercsOver ? "TIME UP" : "FAILED",
                      s.score, survival::MercsRank(s.score, cfg.mercs.ranks), s.kills, s.maxCombo, run.resultRecord ? "  -  NEW BEST" : "");
        break;
    default:
        break;
    }
}
void ObjectiveHudLine(uint32_t now, char* out, size_t size) {
    auto secondsUntil = [&](uint32_t at) { return static_cast<int32_t>(at - now) > 0 ? (at - now + 999) / 1000 : 0u; };
    const unsigned clock = static_cast<unsigned>((run.clockMs + 999) / 1000);
    switch (run.state) {
    case State::Arming: {
        const unsigned start = static_cast<unsigned>(survival::ObjectiveStartMs(run.omap ? run.omap->roundSeconds : 0.0f, cfg.objective.startPercent,
                                                                                cfg.objective.maxSeconds) / 1000);
        std::snprintf(out, size, "OBJECTIVE  -  %u:%02u ON THE CLOCK  -  START IN %u S", start / 60, start % 60, secondsUntil(run.armedAt + StartDelayS() * 1000u));
        break;
    }
    case State::Wave: {
        char breather[32] = "";
        if (survival::DeadlinePending(now, run.breatherUntil)) std::snprintf(breather, sizeof(breather), "  |  BREATHER %u S", secondsUntil(run.breatherUntil));
        else if (run.defendFraction >= 0.0f) std::snprintf(breather, sizeof(breather), "  |  DEFEND %u %%", static_cast<unsigned>(run.defendFraction * 100.0f + 0.5f));
        if (run.objStep) std::snprintf(out, size, "OBJECTIVE %u/5  |  %u:%02u  |  KILLS %u%s", run.objStep, clock / 60, clock % 60, run.objKills, breather);
        else std::snprintf(out, size, "OBJECTIVE  |  %u:%02u  |  KILLS %u%s", clock / 60, clock % 60, run.objKills, breather);
        break;
    }
    case State::Dead:
        if (run.objectiveWon)
            std::snprintf(out, size, "MAP CLEARED  -  %u:%02u LEFT  -  KILLS %u  -  DEATHS %u%s", clock / 60, clock % 60, run.objKills, run.objDeaths,
                          run.resultRecord ? "  -  NEW BEST" : "");
        else std::snprintf(out, size, "%s  -  OBJECTIVE %u/5  -  KILLS %u  -  DEATHS %u", run.objectiveEnd, run.objStep, run.objKills, run.objDeaths);
        break;
    default:
        break;
    }
}
void ZoneHudLine(uint32_t now, char* out, size_t size) {
    auto secondsUntil = [&](uint32_t at) { return static_cast<int32_t>(at - now) > 0 ? (at - now + 999) / 1000 : 0u; };
    const unsigned clock = static_cast<unsigned>((run.clockMs + 999) / 1000);
    switch (run.state) {
    case State::Arming:
        std::snprintf(out, size, "HOLD THE ZONE  -  %u:%02u ON THE CLOCK  -  START IN %u S", cfg.zone.startSeconds / 60, cfg.zone.startSeconds % 60,
                      secondsUntil(run.armedAt + StartDelayS() * 1000u));
        break;
    case State::Wave:
        if (!run.zoneHas)
            std::snprintf(out, size, "ZONE %u  |  %u:%02u  |  HELD %u  |  NEW ZONE COMING", run.zoneHeld + 1, clock / 60, clock % 60, run.zoneHeld);
        else
            std::snprintf(out, size, "ZONE %u  |  %u:%02u  |  HELD %u  |  %u %%  %s", run.zoneHeld + 1, clock / 60, clock % 60, run.zoneHeld,
                          static_cast<unsigned>(run.zoneProgress * 100.0f), survival::ZoneModeName(run.zoneMode));
        break;
    case State::Dead:
        std::snprintf(out, size, "%s  -  ZONES HELD %u  -  KILLS %u  -  DEATHS %u%s", run.zoneCleared ? "ALL ZONES HELD" : run.zoneOver ? run.zoneEnd : "FAILED",
                      run.zoneHeld, run.zoneKills, run.zoneDeaths, run.resultRecord ? "  -  NEW BEST" : "");
        break;
    default:
        break;
    }
}
// true when the line names the difficulty already (the arming line, after SURVIVAL)
bool SurvivalHudLine(uint32_t now, char* out, size_t size, int difficulty) {
    auto secondsUntil = [&](uint32_t at) { return static_cast<int32_t>(at - now) > 0 ? (at - now + 999) / 1000 : 0u; };
    switch (run.state) {
    case State::Arming: {
        char tag[24] = "";
        if (coop_difficulty::Valid(difficulty)) std::snprintf(tag, sizeof(tag), "  -  %s", coop_difficulty::HudName(difficulty));
        if (run.waitWhy) std::snprintf(out, size, "SURVIVAL%s  -  STARTING: WAITING FOR %s", tag, run.waitWhy);
        else if (cfg.startDelayS) std::snprintf(out, size, "SURVIVAL%s  -  FIRST WAVE IN %u S", tag, secondsUntil(run.armedAt + cfg.startDelayS * 1000u));
        else std::snprintf(out, size, "SURVIVAL%s  -  PRESS THE START KEY", tag);
        return true;
    }
    case State::Wave:
        if (run.spec.timeLimitS) std::snprintf(out, size, "WAVE %u  -  SURVIVE %u S", run.wave, secondsUntil(run.waveAt + run.spec.timeLimitS * 1000u));
        else std::snprintf(out, size, "WAVE %u  -  %u LEFT", run.wave, Left());
        break;
    case State::Intermission:
        std::snprintf(out, size, "WAVE %u CLEARED  -  NEXT WAVE IN %u S", run.wave, secondsUntil(run.pauseUntil));
        break;
    case State::Dead:
        if (run.gameOver)
            std::snprintf(out, size, "GAME OVER  -  WAVE %u  -  BEST %u  -  NEW RUN IN %u S", run.wave, run.wave > run.bestBefore ? run.wave : run.bestBefore,
                          secondsUntil(run.stateAt + cfg.gameOverS * 1000u));
        else std::snprintf(out, size, "RUN OVER  -  WAVE %u  -  BEST %u", run.wave, run.wave > run.bestBefore ? run.wave : run.bestBefore);
        break;
    case State::Off:
        break;
    }
    return false;
}
void HudLine(uint32_t now, char* out, size_t size) {
    out[0] = 0;
    if (run.versus) return;   // the match has its own HUD
    const int difficulty = HudDifficulty();
    if (run.mercs) MercsHudLine(now, out, size);
    else if (run.objective) ObjectiveHudLine(now, out, size);
    else if (run.zone) ZoneHudLine(now, out, size);
    else if (SurvivalHudLine(now, out, size, difficulty)) return;
    // "WAVE 4  -  11 LEFT  -  ZEALOT"; a line too long for it stays as it is
    coop_difficulty::AppendSuffix(out, size, difficulty);
}
// Every tick: the line and the flash go to DS2Coop when they change, and once a
// second anyway (a partner who joins late gets them too).
char hudSentLine[96] = "", hudSentFlash[96] = "";
uint32_t hudSentAt = 0;
volatile LONG hudPushes = 0;
void UpdateHud(uint32_t now) {
    if (!HudOn()) return;
    char line[96];
    HudLine(now, line, sizeof(line));
    const char* flash = hudFlashUntil && static_cast<int32_t>(hudFlashUntil - now) > 0 && run.state != State::Off ? hudFlash : "";
    if (std::strcmp(line, hudSentLine) == 0 && std::strcmp(flash, hudSentFlash) == 0 && now - hudSentAt < 1000) return;
    std::snprintf(hudSentLine, sizeof(hudSentLine), "%s", line);
    std::snprintf(hudSentFlash, sizeof(hudSentFlash), "%s", flash);
    hudSentAt = now;
    hudSet(line, flash);
    InterlockedIncrement(&hudPushes);
}

// --- the director's enemies and a level teardown ---
// Our pool enemies outlive a checkpoint restart or a level change while the tables of their state
// machines are freed with the old level (09.09.; dump 75636, 02.11.: seven of them after a pause-menu
// restart in arena_ch01, the crash 4 s later in the new level). Before the teardown every machine of
// every enemy still alive is parked on the watchdog's idle table (nothing reads the old tables any
// more, exit lists included) and the enemy is deleted (Base::Delete, as the cleanup does). Once the
// level manager has left RUNNING the delete waits for the next running level; parking is immediate.
struct Zombie { uint32_t actor, vtable; Guid guid; LONG serviceEpoch; };
std::vector<Zombie> zombies;
LONG teardownMark = 0;
uint32_t teardownMarkAt = 0;
bool teardownMarkSawLoad = false;
volatile LONG purges = 0, purgedEnemies = 0, purgeMachines = 0, purgeDeleted = 0, purgeDeferred = 0, purgeStale = 0, zombiesDeleted = 0, zombiesGone = 0;
unsigned ParkMachines(uint32_t sm, unsigned depth) {
    unsigned parked = 0;
    for (unsigned guard = 0; sm && guard < 32; ++guard) {
        if (ds2_watchdog_runtime::Idle(sm)) ++parked;
        uint32_t child = 0, next = 0;
        if (depth < 6 && Read(sm + ds2::watchdog::machine::kChildHead, &child, 4) && child) parked += ParkMachines(child, depth + 1);
        if (!Read(sm + ds2::watchdog::machine::kNext, &next, 4) || next == sm) break;
        sm = next;
    }
    return parked;
}
unsigned ParkActor(uint32_t actor) {
    uint32_t tree = 0, root = 0;
    if (!Read(actor + ds2::watchdog::machine::kActorTree, &tree, 4) || !tree || !Read(tree + ds2::watchdog::machine::kTreeRoot, &root, 4) || !root) return 0;
    return ParkMachines(root, 0);
}
void PurgeForTeardown(bool deleteNow, const char* why, uint32_t now) {
    unsigned enemies = 0, machines = 0, deleted = 0, later = 0, stale = 0;
    for (auto& t : tracked) {
        if (!t.actor) continue;
        if (t.serviceEpoch != InterlockedCompareExchange(&streamServiceEpoch, 0, 0) || !SameEntity(t.actor, t.vtable, t.guid)) { ++stale; t.actor = 0; continue; }
        ++enemies;
        machines += ParkActor(t.actor);
        if (deleteNow && Have(sDelete) && DeleteRaw(t.actor)) ++deleted;
        else { zombies.push_back(Zombie{t.actor, t.vtable, t.guid, t.serviceEpoch}); ++later; }
        t.actor = 0;
    }
    // The mark parks every older enemy machine that still ticks: only when ours were alive (a story
    // restart has nothing of ours to outlive it), and taken back when no loading screen follows.
    if (enemies) {
        ds2_watchdog_runtime::NoteTeardown(why);
        teardownMark = ds2_watchdog_runtime::TeardownMark();
        teardownMarkAt = now ? now : 1;
        teardownMarkSawLoad = false;
    }
    InterlockedIncrement(&purges);
    InterlockedExchangeAdd(&purgedEnemies, static_cast<LONG>(enemies));
    InterlockedExchangeAdd(&purgeMachines, static_cast<LONG>(machines));
    InterlockedExchangeAdd(&purgeDeleted, static_cast<LONG>(deleted));
    InterlockedExchangeAdd(&purgeDeferred, static_cast<LONG>(later));
    InterlockedExchangeAdd(&purgeStale, static_cast<LONG>(stale));
    if (enemies || stale || later)
        Event("teardown (%s): %u enemies of ours still alive, %u state machines parked, %u deleted now, %u deleted once a level runs again, %u already gone; "
              "older enemy machines park on their next tick (mark %ld)",
              why, enemies, machines, deleted, later, stale, enemies ? teardownMark : 0L);
}
void WatchTeardownMark(uint32_t lm, uint32_t now) {
    if (!teardownMarkAt) return;
    if (lm != kRunning) { teardownMarkSawLoad = true; return; }
    if (teardownMarkSawLoad) { teardownMarkAt = 0; return; }   // the teardown came: the mark stays for the survivors
    if (now - teardownMarkAt < 10000) return;
    ds2_watchdog_runtime::DropTeardownMark(teardownMark);
    Event("teardown mark %ld dropped: the level kept running 10 s after it was set (no restart came), the level's own enemies tick again", teardownMark);
    teardownMarkAt = 0;
}
// The deferred ones, once a level runs again: still the same object, then deleted.
void DeleteZombies(uint32_t lm) {
    if (zombies.empty() || lm != kRunning || !Have(sDelete)) return;
    unsigned deleted = 0, gone = 0;
    for (const Zombie& z : zombies) {
        if (z.serviceEpoch != InterlockedCompareExchange(&streamServiceEpoch, 0, 0) || !SameEntity(z.actor, z.vtable, z.guid)) { ++gone; continue; }
        ParkActor(z.actor);   // parked again: a new machine would be on a live table, an old one stays idle
        if (DeleteRaw(z.actor)) ++deleted;
    }
    zombies.clear();
    InterlockedExchangeAdd(&zombiesDeleted, static_cast<LONG>(deleted));
    InterlockedExchangeAdd(&zombiesGone, static_cast<LONG>(gone));
    Event("teardown: %u parked enemies of the old level deleted, %u were gone already", deleted, gone);
}

void PayRunEnd();   // below, with Retire
void EndRun(const char* why, uint32_t now) {
    PayRunEnd();
    if (run.state == State::Off) return;
    // the wave reached counts however the run ends (death, restart, way back);
    // a Mercenaries run keeps its score instead (MercsOver)
    if (run.versus) {
        Event("versus run %u ended (%s): round %d, slashers spawned %u (super %u, super health only %u), killed %u, pressure %u (peak %u, raises %u, drops %u)",
              run.number, why, run.vRound, run.vSpawned, run.vSuper, run.vSuperHealthOnly, run.vKilled, run.pressure.level, run.vPeak, run.pressure.raises,
              run.pressure.drops);
        InterlockedExchange(&versusRoundMirror, 0);
        InterlockedExchange(&versusStageMirror, 0);
        InterlockedExchange(&versusPressureMirror, 0);
    }
    if (!run.mercs && !run.objective && !run.zone && !run.versus && run.wave && UpdateBestFor(run.map, run.wave)) {
        SaveBest();
        Event("best wave on %s is now %u", run.map.c_str(), run.wave);
    }
    if (run.mercs)
        Event("mercenaries run %u ended (%s): score %u kills %u best combo %u capsules %u deaths %u clock %.1f s; capsules placed %u lost %u expired %u", run.number,
              why, run.ms.score, run.ms.kills, run.ms.maxCombo, run.ms.capsules, run.ms.deaths, static_cast<double>(run.clockMs) / 1000.0, run.capsulesPlaced,
              run.capsulesLost, run.capsulesExpired);
    if (run.objective)
        Event("objective run %u ended (%s): objective %u/5, %u done, kills %u, deaths %u, clock %.1f s, map bonuses counted %08X", run.number, why, run.objStep,
              run.objDone, run.objKills, run.objDeaths, static_cast<double>(run.clockMs) / 1000.0, run.bonusApplied);
    if (run.zone) {
        Event("zone run %u ended (%s): %u zone(s) held, %u placed in %u tries, kills %u, deaths %u, clock %.1f s", run.number, why, run.zoneHeld, run.zonePlaced,
              run.zoneTries, run.zoneKills, run.zoneDeaths, static_cast<double>(run.clockMs) / 1000.0);
        if (zoneShow) zoneShow(0, run.zoneId, nullptr, 0.0f, 0);
    }
    // a released level takes its pickups with it: only a running one gets them deleted
    if (LmState() != kRunning || InterlockedCompareExchange(&levelEndingAsked, 0, 0)) {
        if (!capsules.empty()) AnnounceCapsule(6, Capsule{});
        capsules.clear();
    }
    else DeleteCapsules(why);
    mercsNearestCapsule = -1.0f;
    Event("run %u ended at wave %u: %s; spawned=%u killed=%u watchdog_kills=%u written_off=%u vanished=%u requeued_outside_arena=%u caught_up=%u engines_rested=%u "
          "engines_avoided=%zu (no engine calls: the level owns the enemies)",
          run.number, run.wave, why, run.rc.spawned, run.rc.killed, run.rc.stuckKills + run.rc.ventStuckKills, run.rc.inert + run.rc.writtenOff,
          run.rc.vanished, run.requeued, run.caughtUp, run.enginesRested, avoidedEngines.size());
    // The Ubermorph never dies: on a level that keeps running it stays tracked into the next
    // run, whose "one at a time" rule then brings no second one (13.10.: three of them after
    // three Mercenaries runs on Fuel Core).
    if (InterlockedCompareExchange(&levelEndingAsked, 0, 0)) {
        tracked.clear();
    } else {
        tracked.erase(std::remove_if(tracked.begin(), tracked.end(), [](const Tracked& t) { return !t.actor || !t.unkillable; }), tracked.end());
        for (const auto& t : tracked) Event("run %u: the Ubermorph %08X stays on the level and is kept for the next run", run.number, t.actor);
    }
    avoidedEngines.clear();
    engineRests.clear();
    engineMisplaced.clear();
    donorFailures.clear();
    Event("run %u: %u item(s) sent to a vent for want of a direct donor or engine; %ld level spawn(s) refused in arenas so far", run.number, run.ventFallbacks,
          InterlockedCompareExchange(&levelSpawnsRefused, 0, 0));
    Event("run %u: checks that spared an enemy in a fight so far (per pass): grab=%ld held=%ld player_grab=%ld near_player=%ld", run.number,
          InterlockedCompareExchange(&sparedFights[1], 0, 0), InterlockedCompareExchange(&sparedFights[2], 0, 0),
          InterlockedCompareExchange(&sparedFights[3], 0, 0), InterlockedCompareExchange(&sparedFights[4], 0, 0));
    RestoreCharging();
    RestoreVersusGroups();
    awarePackets.clear();
    for (auto& s : ventSpots) { s.pointer = 0; s.resolvable = false; s.lastUseAt = s.coolUntil = 0; }
    for (auto& e : engines) { e.pointer = 0; e.bad = false; }
    for (unsigned& f : markerFailures) f = 0;
    for (bool& r : markerRetired) r = false;
    SetState(State::Off, now);
    run = Run{};
    InterlockedExchange(&admin::hold, 0);
    InterlockedExchange(&waveMirror, 0);
}

// --- availability ---
safetyhook::MidHook dividerLocationHook;

void OnDividerLocation(SafetyHookContext& ctx) {
    auto* placement = dividerPlacement;
    if (!placement || placement->frame != ctx.esp || placement->spawner != ctx.edi ||
        !ctx.ebx || ctx.ebx != lastCreatedActor) return;
    // FuncSpawn's location branch applies this matrix to both the actor and its movement component.
    placement->locationApplied = true;
    ctx.ebp = reinterpret_cast<uintptr_t>(placement->matrix);
    ctx.eip = base + 0xACB36C;
}

// A Stalker hides behind CoverNodes, peeks and charges from there. The nodes exist in only
// seven campaign archives (the stalker rooms, ch04_asc among them); on the MP maps and the
// other arenas it stood around and hardly attacked (13.10., Fuel Core: 19 stalkers, never
// charged). Such a map gets the Stalker's substitutes instead.
bool StalkerCoverOn(const std::string& map) {
    const char* m = map.c_str();
    if (_strnicmp(m, "mp_", 3) == 0 || _strnicmp(m, "pdlc_mp_", 8) == 0) return false;
    if (coop_arena::IsArenaName(m)) return _strnicmp(m, "arena_ch04", 10) == 0;
    return true;
}
// Which families this map can field now: a direct donor whose instance data
// resolves on the pool stream, or a vent donor on a vent whose slasher
// spawner resolves. Logged once per run with the reason for every family.
// A vent's selection window (vent_gate): logged when it opens or closes, and once per run.
template<class Vent>
bool VentAvailable(const Vent& vent) {
    return run.allVentsOpen || vent_gate::Open(vent, run.keepOpenedVents);
}
void LogVentGate(VentSpot& s) {
    const int open = VentAvailable(*s.row) ? 1 : 0;
    if (open == s.open) return;
    const bool first = s.open < 0;
    s.open = open;
    Event("vent %s (vent %08X-%08X) %s%s: window %s%s (sent %ld) .. %s (sent %ld)%s; dispatch notes %ld", s.row->name, s.row->vent[0], s.row->vent[1],
          first ? "is " : "now ", open ? "OPEN" : "CLOSED", s.row->onName[0] ? s.row->onName : "start", s.row->onAtStart ? " (round start)" : "",
          vent_gate::Sent(s.row->selectOn), s.row->offName[0] ? s.row->offName : "end", vent_gate::Sent(s.row->selectOff),
          s.row->inherited ? ", window from its culling group" : "", InterlockedCompareExchange(&vent_gate::notes, 0, 0));
}
// How many of the map's VentMarkers are open: logged when it changes, with the list.
int markersOpenLogged = -1;
void LogVentGates() {
    for (auto& s : ventSpots)
        if (s.open >= 0 && survival::EqualNoCase(s.row->map, run.map)) LogVentGate(s);
    unsigned open = 0, total = 0, wall = 0;
    char list[400] = "";
    size_t used = 0;
    for (const auto& m : ds2::kSurvivalVentMarkers) {
        if (!survival::EqualNoCase(m.map, run.map.c_str())) continue;
        ++total;
        if (!VentAvailable(m)) continue;
        ++open;
        if (m.wall) ++wall;
        if (used < sizeof(list) - 24) used += std::snprintf(list + used, sizeof(list) - used, "%s%08X%s", used ? "," : "", m.vent[1], m.wall ? "w" : "f");
    }
    if (!total || static_cast<int>(open) == markersOpenLogged) return;
    markersOpenLogged = static_cast<int>(open);
    Event("vents open on %s: %u of %u (%u wall, %u floor): %s; vent arrivals may use any of them (VentAnyMarker=%d)", run.map.c_str(), open, total, wall,
          open - wall, list, cfg.ventAnyMarker ? 1 : 0);
}

void Probe(uint32_t now, bool quiet = false) {
    run.probed = true;
    run.probedAt = now ? now : 1;
    run.available.clear();
    unsigned engineCount = 0;
    for (auto& e : engines) {
        e.pointer = KnownSpawnerByGuid(e.row->spawner);
        uint32_t attributes = 0, trigger = 0;
        const bool usable = e.pointer && SpawnerTargetResolves(e.pointer) && Read(e.pointer + kFuncSpawnAttributes, &attributes, 4) && attributes &&
                            Read(attributes + kSpawnAttrTrigger, &trigger, 4) && trigger;
        if (!usable) e.pointer = 0;
        else ++engineCount;
    }
    if (!quiet)
        Event("engines (direct spawns): %u of %zu map prop spawners live with a resolvable target; fallback: the area rule picker", engineCount, engines.size());
    unsigned ventsHere = 0, ventsLive = 0;
    for (auto& s : ventSpots) {
        if (!survival::EqualNoCase(s.row->map, run.map)) { s.pointer = 0; s.resolvable = false; continue; }
        s.pointer = KnownSpawnerByGuid(s.row->spawner);
        s.resolvable = s.pointer && SpawnerTargetResolves(s.pointer);
        s.pos = V3{};
        if (s.pointer) ReadV3(s.pointer + kSpawnerPos, s.pos);
        ++ventsHere;
        if (s.resolvable) ++ventsLive;
        if (quiet) { s.open = -1; continue; }
        std::string families;
        for (const auto& d : s.donors) families += (families.empty() ? "" : ",") + d.first + "x" + std::to_string(d.second.size());
        Event("vent %s spawner=%08X-%08X %s pointer=%08X target_resolves=%d pos=(%.1f %.1f %.1f) vent=%08X-%08X context=%s donors=%s%s", s.row->name,
              s.row->spawner[0], s.row->spawner[1], s.pointer ? "live" : "NOT FOUND in the spawner registry", s.pointer, s.resolvable ? 1 : 0, s.pos.x, s.pos.y,
              s.pos.z, s.row->vent[0], s.row->vent[1], s.row->context, families.empty() ? "none" : families.c_str(),
              s.pointer && !s.resolvable ? " (its MP slasher has no instance data: blocked by [MpMap] or not loaded)" : "");
        s.open = -1;
        LogVentGate(s);
    }
    run.probeMissing = ventsHere - ventsLive;
    markersOpenLogged = -1;
    if (!quiet) LogVentGates();
    if (!quiet) Event("vent redirect so far: written=%ld already_there=%ld failed=%ld | arrivals at the chosen vent=%ld at the spawner's own=%ld elsewhere=%ld",
          InterlockedCompareExchange(&ventRedirectsWritten, 0, 0), InterlockedCompareExchange(&ventRedirectsSame, 0, 0),
          InterlockedCompareExchange(&ventRedirectFailures, 0, 0), InterlockedCompareExchange(&arrivalsAtChosen, 0, 0),
          InterlockedCompareExchange(&arrivalsAtOwn, 0, 0), InterlockedCompareExchange(&arrivalsElsewhere, 0, 0));
    if (!VentPathOn() && !quiet)
        Event("vent path off: %s", !cfg.ventSpawns ? "VentSpawns=0" : !settings.ventRebinding ? "VentRebinding=0 in DS2Randomizer.ini" : "vent binding hooks not installed");
    for (const auto& f : survival::kFamilies) {
        const std::string family = f.family;
        unsigned resolvable = 0, tried = 0;
        const auto it = directDonors.find(family);
        if (it != directDonors.end())
            for (size_t index : it->second) {
                if (tried >= 6) break;
                ++tried;
                if (DonorResolves(index)) ++resolvable;
            }
        unsigned ventDonors = 0, ventSpotsWith = 0;
        for (const auto& s : ventSpots) {
            const auto d = s.donors.find(family);
            if (d == s.donors.end() || d->second.empty()) continue;
            ventDonors += static_cast<unsigned>(d->second.size());
            if (s.resolvable) ++ventSpotsWith;
        }
        const bool ventOk = VentPathOn() && ventSpotsWith > 0;
        const bool noCover = cfg.stalkerNeedsCover && family == "Stalker" && !StalkerCoverOn(run.map);
        const int arenaIndex = coop_arena::ArenaIndex(run.map.c_str());
        const bool excluded = arenaIndex >= 0 && coop_arena::Excludes(coop_arena::kArenas[arenaIndex], f.family);
        const bool mapOut = !survival::MapAllowsFamily(cfg, run.map, family);
        const bool ventsOnly = ArenaVentsOnly();
        const bool missingLocation = run.placeDividersAtBirth && family == "DividerCore" && !dividerLocationHook;
        const bool ok = ((resolvable > 0 && !ventsOnly) || ventOk) && !noCover && !excluded && !mapOut && !missingLocation;
        if (ok) run.available.insert(family);
        const char* why = ok ? "" : missingLocation ? "Divider spawn location hook unavailable"
                                  : mapOut ? "[MapFamilies] leaves it out on this map"
                                  : excluded ? "the arena leaves it out (exclude= in its arena file)"
                                  : ventsOnly && !ventOk ? "the arena spawns only through vents (spawns=vents) and no vent of it has a donor of this family"
                                  : noCover ? "no CoverNodes on this map, a Stalker only stands there (StalkerNeedsCover=1)"
                                  : it == directDonors.end() ? (unavailableWhy.count(family) ? unavailableWhy[family].c_str() : "no pool template")
                                  : "no donor resolves on the pool stream (pool not dispatched or model stream missing)";
        if (!quiet)
            Event("availability %-11s role=%-9s %s direct=%u/%u resolvable (of %zu) vent=%u donor(s) on %u live vent(s)%s%s", family.c_str(), survival::RoleName(f.role),
                  ok ? "AVAILABLE  " : "unavailable", resolvable, tried, it == directDonors.end() ? size_t(0) : it->second.size(), ventDonors, ventSpotsWith,
                  ok ? "" : ": ", why);
    }
    if (quiet) {
        std::string families;
        for (const auto& f : run.available) families += (families.empty() ? "" : ",") + f;
        Event("probe again #%u: %u of %u vent spawners of %s live, families available: %s (engine registry fallback found=%ld missing=%ld faults=%ld, "
              "spawners used up=%ld moved=%ld stale_dropped=%ld)", run.reprobes, ventsLive, ventsHere, run.map.c_str(), families.empty() ? "none" : families.c_str(),
              InterlockedCompareExchange(&registryFallbackFound, 0, 0), InterlockedCompareExchange(&registryFallbackMissing, 0, 0),
              InterlockedCompareExchange(&registryFallbackFaults, 0, 0), InterlockedCompareExchange(&knownSpawnersRetired, 0, 0),
              InterlockedCompareExchange(&knownSpawnersMoved, 0, 0), InterlockedCompareExchange(&staleSpawnersDropped, 0, 0));
        (void)now;
        return;
    }
    std::set<std::string> asked;
    for (const auto& w : cfg.waves) for (const auto& e : w.entries) asked.insert(e.family);
    for (const auto& family : asked) {
        if (run.available.count(family)) continue;
        const survival::Substitution sub = survival::Substitute(family, run.available);
        Event("substitution %s -> %s%s", family.c_str(), sub.family.empty() ? "NOTHING (entries dropped)" : sub.family.c_str(),
              sub.factor > 1 ? " x3 (a heavy replaced by a lighter family)" : "");
    }
    (void)now;
}

// --- spawning ---
constexpr unsigned kNotReached = 0xFFFFu;
enum class Drive { Made, NotMade, OwnTarget, Busy };
// One forced spawn through `spawner`. The stream probe mutex is taken here, on
// its own thread, so OnLookup's try_lock cannot fail and leave the spawner to
// build its own target.
Drive DriveOne(uintptr_t spawner, const ds2::ExtraSpawnRule& rule, size_t donor, const ds2::EnemyRecord* ventSource, uint32_t& actor, unsigned& status,
               bool& armed, bool& applied, unsigned& bindResult) {
    actor = 0;
    status = kNotReached;
    armed = applied = false;
    bindResult = 0;
    std::unique_lock<std::recursive_mutex> guard(streamProbeMutex, std::try_to_lock);
    if (!guard.owns_lock()) return Drive::Busy;
    forcedVentSource = ventSource;
    survivalSpawnActive = true;
    lastCreatedStatus = kNotReached;
    lastCreatedArmed = lastCreatedApplied = false;
    lastCreatedBind = 0;
    const bool made = DriveForcedSpawn(spawner, rule, donor);
    actor = lastCreatedActor;
    status = lastCreatedStatus;
    armed = lastCreatedArmed;
    applied = lastCreatedApplied;
    bindResult = lastCreatedBind;
    forcedVentSource = nullptr;
    survivalSpawnActive = false;
    if (!made) return Drive::NotMade;
    if (status != static_cast<unsigned>(ds2::SelectionStatus::PacketReplaced)) return Drive::OwnTarget;
    return Drive::Made;
}
// OnLookup's vent rebind refused the donor (bindResult 200 + RebindResult, 200 = Ready).
bool RebindRefused(unsigned bind) { return bind > 200 && bind < 300; }
const char* StatusText(unsigned status) {
    return status == kNotReached ? "spawner refused before the lookup (population gate, vent busy or retired)" : ds2::statusName(status);
}
// Next donor of a list, skipping those that failed three times this session.
size_t PickDonor(const std::string& key, const std::vector<size_t>& list) {
    if (list.empty()) return SIZE_MAX;
    size_t& turn = donorTurn[key];
    for (size_t k = 0; k < list.size(); ++k) {
        const size_t index = list[(turn + k) % list.size()];
        if (donorFailures[index] >= 3) continue;
        turn = (turn + k + 1) % list.size();
        return index;
    }
    return SIZE_MAX;
}
// Escalation: with the wave's chance an enemy with an enhanced variant comes as
// one (the ordinary list is the fallback both ways).
volatile LONG enhancedPicks = 0, ordinaryPicks = 0, oraclePicks = 0, oracleCapped = 0;
bool IsOracle(size_t i) { return pool[i].retargeted && pool[i].enhanced; }
unsigned OraclesAlive() {
    unsigned n = 0;
    for (const auto& t : tracked) {
        if (!t.actor) continue;
        const auto found = std::lower_bound(pool.begin(), pool.end(), t.donor, [](const auto& r, const Guid& g) { return r.guid < g; });
        if (found != pool.end() && found->guid == t.donor && found->retargeted && found->enhanced) ++n;
    }
    return n;
}
unsigned EnhancedChanceNow() {
    return ClockRun() ? survival::ClockEnhancedChance(run.wave, run.spec, cfg) : survival::EnhancedChanceFor(run.wave, run.spec, cfg);
}
// The current item's one roll for the enhanced variant. Rolled per try, a vent with nothing but
// enhanced donors would win in the end whatever the chance.
struct ItemRoll { unsigned number = 0, wave = 0; size_t next = SIZE_MAX; bool enhanced = false; };
ItemRoll itemRoll;
bool ItemEnhanced() {
    if (itemRoll.number != run.number || itemRoll.wave != run.wave || itemRoll.next != run.next) {
        const unsigned chance = EnhancedChanceNow();
        itemRoll = ItemRoll{run.number, run.wave, run.next, chance && rng.Unit() * 100.0f < static_cast<float>(chance)};
    }
    return itemRoll.enhanced;
}
volatile LONG enhancedFallbackRefused = 0, ordinaryStandIns = 0;
size_t PickEscalated(const std::string& key, const std::vector<size_t>& ordinary, const std::vector<size_t>* enhanced, bool fixedSkin = false) {
    if (fixedSkin) {   // an item that asks for a package gets it, enhanced or not
        std::vector<size_t> all = ordinary;
        if (enhanced) all.insert(all.end(), enhanced->begin(), enhanced->end());
        return PickDonor(key, all);
    }
    std::vector<size_t> plain, super;
    for (const size_t i : ordinary) (superGuids.count(pool[i].guid) ? super : plain).push_back(i);
    if (enhanced) super.insert(super.end(), enhanced->begin(), enhanced->end());
    const unsigned chance = EnhancedChanceNow();
    if (plain.empty() && !chance) { InterlockedIncrement(&enhancedFallbackRefused); return SIZE_MAX; }
    const bool wantEnhanced = !super.empty() && chance && ItemEnhanced();
    size_t donor = SIZE_MAX;
    // Oracle twitchers at most OracleShare of the enhanced picks and OracleMaxAlive at once;
    // the super slashers take the rest ([Escalation], 20.10.).
    if (wantEnhanced) {
        std::vector<size_t> oracles, others;
        for (const size_t i : super) (IsOracle(i) ? oracles : others).push_back(i);
        const bool oracleOk = !oracles.empty() && OraclesAlive() < cfg.escOracleMaxAlive && rng.Unit() * 100.0f < static_cast<float>(cfg.escOracleShare);
        if (oracleOk) {
            donor = PickDonor(key + ":oracle", oracles);
        } else {
            if (!oracles.empty()) InterlockedIncrement(&oracleCapped);
            if (!others.empty()) donor = PickDonor(key + ":enhanced", others);
        }
    }
    // The fallback never takes an oracle twitcher: it comes only when the escalation asks for it.
    std::vector<size_t> fallback;
    if (plain.empty())
        for (const size_t i : super)
            if (!(pool[i].retargeted && pool[i].enhanced)) fallback.push_back(i);
    if (donor == SIZE_MAX) donor = PickDonor(key, plain.empty() ? fallback : plain);
    if (donor != SIZE_MAX) InterlockedIncrement(superGuids.count(pool[donor].guid) ? &enhancedPicks : &ordinaryPicks);
    if (donor != SIZE_MAX && pool[donor].retargeted && pool[donor].enhanced) InterlockedIncrement(&oraclePicks);
    return donor;
}
// [Survival] TwitcherPercent / SeveredPregnantPercent: a slasher or pregnant comes as its Severed
// counterpart, never a super slasher, and only where a copy of its family fits the same arrival
// (a direct one, or one with the vent spawner's survival key).
volatile LONG twitcherTooEarly = 0;
size_t MaybeTwitcher(const survival::Item& item, size_t donor, const std::map<std::string, std::vector<size_t>>* ventTwitchers, const char* ventName) {
    const unsigned percent = item.family == "Zombie" ? cfg.twitcherPercent : item.family == "Pregnant" ? cfg.pregnantPercent : 0;
    if (donor == SIZE_MAX || !percent || item.super || superGuids.count(pool[donor].guid)) return donor;
    if (EscWave(run.wave) < cfg.severedFromWave) { InterlockedIncrement(&twitcherTooEarly); return donor; }
    InterlockedIncrement(&twitcherRolls);
    if (rng.Unit() * 100.0f >= static_cast<float>(percent)) return donor;
    const auto& lists = ventTwitchers ? *ventTwitchers : twitcherDirect;
    const auto list = lists.find(item.family);
    const size_t twitcher = list == lists.end() ? SIZE_MAX
                                                : PickDonor(std::string(ventTwitchers ? "vent:severed:" : "direct:severed:") + (ventName ? ventName : "") + ":" + item.family, list->second);
    if (twitcher == SIZE_MAX) { InterlockedIncrement(&twitcherNoCopy); return donor; }
    InterlockedIncrement(ventTwitchers ? &twitcherVentPicks : &twitcherPicks);
    return twitcher;
}
// In an arena the area rule picker takes a spawner inside it first: the ch04
// arena's outside engines (an elevator, a corridor) kept their enemies on their
// own arrival path, every placement on the spot was undone (09.10.).
bool EngineInsideArena(uintptr_t spawner) {
    V3 p;
    return arenaInside && ReadV3(spawner + kSpawnerPos, p) && arenaInside(&p.x) == 1;
}
// A direct enemy is made at its engine and moved to its spot after that; some engines put it
// back on their own spot for up to 2.5 s (13.11. mp_escape: four times at (-1 5.9 0), the map's
// middle). An engine whose own spot is nearer a living player than MinDistance is skipped.
volatile LONG enginesNearSkipped = 0;
bool EngineNearPlayer(uint32_t pointer, const Viewer* viewers, unsigned viewerCount) {
    V3 p;
    if (!viewers || !ReadV3(pointer + kSpawnerPos, p)) return false;
    for (unsigned i = 0; i < viewerCount; ++i)
        if (viewers[i].alive && survival::Distance(p, viewers[i].feet) < cfg.minDistance) return true;
    return false;
}
uintptr_t PickEngine(Engine** used, const Viewer* viewers, unsigned viewerCount) {
    *used = nullptr;
    for (auto& e : engines) {
        // 17.10., Marker Labs: an engine that put its enemies back behind a locked door twice was
        // still used for five more, the avoid list was only asked by the area rule picker
        if (e.bad || !e.pointer || EngineAvoided(e.pointer)) continue;
        if (!KnownSpawnerUsable(e.pointer, e.row->spawner)) { e.pointer = 0; continue; }
        if (EngineNearPlayer(e.pointer, viewers, viewerCount)) { InterlockedIncrement(&enginesNearSkipped); continue; }
        *used = &e;
        return e.pointer;
    }
    const uintptr_t picked = SpawnerForAreaRule(InArena() ? EngineInsideArena : nullptr);
    if (picked || avoidedEngines.empty()) return picked;
    // Every engine retired: forgive them all rather than refuse the rest of the run
    // (19.10. mp_escape: 17 retired, a Brute refused 116 times).
    InterlockedIncrement(&enginesForgiven);
    Event("all %zu retired engines forgiven: no other engine was left", avoidedEngines.size());
    avoidedEngines.clear();
    engineMisplaced.clear();
    return SpawnerForAreaRule(InArena() ? EngineInsideArena : nullptr);
}
void Track(uint32_t actor, size_t donor, bool vent, const V3& at, uint32_t now) {
    Tracked t;
    t.actor = actor;
    t.serviceEpoch = InterlockedCompareExchange(&streamServiceEpoch, 0, 0);
    Read(actor, &t.vtable, 4);
    Read(actor + kEntityGuid, t.guid.data(), 16);
    t.donor = pool[donor].guid;
    t.family = pool[donor].family;
    t.vent = vent;
    t.unkillable = survival::Unkillable(t.family);
    t.wave = run.wave;
    t.watch.bornAt = t.watch.progressAt = now;
    t.spawnedAt = at;
    sound_probe::NoteFamily(t.vtable, t.family.c_str());
    tracked.push_back(t);
    InterlockedIncrement(&survivalLevelSpawns);
    ++run.wc.spawned;
    ++run.rc.spawned;
    if (vent) { ++run.wc.vent; ++run.rc.vent; } else { ++run.wc.direct; ++run.rc.direct; }
    InterlockedIncrement(&totalSpawned);
}
void Refuse(const char* what, const survival::Item& item, const char* detail) {
    if (Budget(run.refuseLines, 120)) Event("refused wave=%u family=%s path=%s: %s%s%s", run.wave, item.family.c_str(), survival::PathName(item.path), what,
                                           detail && detail[0] ? " " : "", detail ? detail : "");
}

// When each VentMarker last let an enemy out (kSurvivalVentMarkers order).
uint32_t markerUsedAt[std::size(ds2::kSurvivalVentMarkers)]{};
int MarkerIndex(const Guid& vent) {
    for (size_t m = 0; m < std::size(ds2::kSurvivalVentMarkers); ++m)
        if (ds2::kSurvivalVentMarkers[m].vent == vent) return static_cast<int>(m);
    return -1;
}
// A vent arrival: any open VentMarker of the map within reach of the living,
// through a free slasher spawner of the same kind (wall or floor). The spawner's
// spawning header is pointed at the chosen vent (ventRedirect, OnLookup); the
// vent least recently used goes first.
bool TryVent(const survival::Item& item, uint32_t now, const Viewer* viewers, unsigned viewerCount) {
    unsigned living = 0;
    for (unsigned i = 0; i < viewerCount; ++i) living += viewers[i].alive ? 1u : 0u;
    const float ventMin = survival::VentMinFor(cfg, living);
    struct Pick { size_t spot = SIZE_MAX, marker = SIZE_MAX; float nearest = 0.0f, roll = 0.0f; uint32_t used = 0; float walk = -1.0f; };
    Pick pick, winding;
    reachRewalks = 6;
    const size_t count = ventSpots.size();
    const bool enhancedAllowed = !item.package.empty() || ItemEnhanced();
    for (size_t k = 0; k < count; ++k) {
        const size_t index = (run.rotation + k) % count;
        VentSpot& s = ventSpots[index];
        const auto d = s.donors.find(item.family);
        if (!s.resolvable || d == s.donors.end() || d->second.empty() || survival::DeadlinePending(now, s.coolUntil)) continue;
        if (!enhancedAllowed && OnlyEnhanced(d->second)) continue;
        if (!item.package.empty() && !HasPackage(d->second, item.package)) continue;
        if (!KnownSpawnerUsable(s.pointer, s.row->spawner)) { s.resolvable = false; s.pointer = 0; ++run.probeMissing; continue; }
        // one arrival per spawner at a time: its header must not change under the last one
        if ((s.lastUseAt && now - s.lastUseAt < cfg.ventGapMs) || !VentQuiet(s.pointer, now)) { ++run.refusals.ventBusy; continue; }
        for (const size_t m : s.markers) {
            const auto& marker = ds2::kSurvivalVentMarkers[m];
            // behind the doors the map has not opened yet, or an area it closed
            if (!VentAvailable(marker)) { ++run.refusals.ventClosed; continue; }
            // an arena's vent behind one of its locked doors leads nowhere the players are
            if (const coop_arena::Arena* arena = RunArena()) {
                const float place[3] = {marker.x, marker.y, marker.z};
                if (coop_arena::BehindLockedDoor(*arena, coop_arena::PointsFor(arena->name), place)) { ++run.refusals.ventClosed; continue; }
            }
            if (markerUsedAt[m] && now - markerUsedAt[m] < cfg.ventGapMs) { ++run.refusals.ventBusy; continue; }
            if (markerRetired[m]) { InterlockedIncrement(&ventRetiredSkips); continue; }
            const V3 at{marker.x, marker.y, marker.z};
            // the radius counts from the living: a body that waits for its respawn draws no enemies
            float nearest = 1e9f, nearestFeetY = 0.0f;
            V3 nearestFeet{};
            for (unsigned i = 0; i < viewerCount; ++i)
                if (viewers[i].alive) {
                    const float away = survival::Distance(at, viewers[i].feet);
                    if (away < nearest) { nearest = away; nearestFeetY = viewers[i].feet.y; nearestFeet = viewers[i].feet; }
                }
            if (nearest < ventMin) { ++run.refusals.ventClose; continue; }
            if (nearest > VentMaxDistanceHere(now)) { ++run.refusals.ventFar; continue; }
            // the MP maps' high wall vents are the necro players' (their slashers only use floor-level
            // ones; 13.10. on Solar Array they came out 7 m up). Measured from the walkable floor below
            // the vent, so where the players stand does not matter; the nearest player's floor only
            // when the mesh gives no answer.
            if (const int heights = ArenaHeights(); heights >= 0) {
                // an arena's vents are the campaign's own, at any height; its height rule counts
                // from the floor below the vent (a wall vent sits about a metre above it)
                const float drop = MarkerDrop(m);
                const float floorY = drop >= 0.0f ? at.y - drop : at.y - (marker.wall ? 1.0f : 0.0f);
                bool fits = heights == 0 || (heights == 2 && floorY - nearestFeetY <= survival::kSameFloor);
                for (unsigned i = 0; heights == 1 && i < viewerCount && !fits; ++i)
                    fits = viewers[i].alive && std::fabs(floorY - viewers[i].feet.y) <= survival::kSameFloor;
                if (!fits) { ++run.refusals.ventHigh; continue; }
            } else if (cfg.ventMaxAbove > 0.0f && marker.wall) {
                const float drop = MarkerDrop(m);
                if (drop >= 0.0f ? drop > cfg.ventMaxAbove : at.y - nearestFeetY > cfg.ventMaxAbove) { ++run.refusals.ventHigh; continue; }
            }
            if (cfg.ventMustReach && !MarkerReachable(m, nearestFeet, now)) { ++run.refusals.ventCut; continue; }
            unsigned by = 0;
            if (cfg.ventLineOfSight && Seen(at, viewers, viewerCount, by)) { ++run.refusals.ventSeen; continue; }
            const float roll = rng.Unit();
            // Close by air, far on foot (16.10., Marker Test-Bed: side by side corridors, a vent 21 m
            // away was a 104 m walk and its slashers came half a minute later): a walk longer than
            // MaxPathFactor times the distance, as for a direct spot, is used only when no other is left.
            const float walk = cfg.ventMustReach ? MarkerWalk(m) : -1.0f;
            if (walk > nearest * cfg.maxPathFactor + 4.0f) {
                ++run.refusals.ventWinding;
                if (InterlockedIncrement(&windingLines) <= 24)
                    Event("vent marker %zu at (%.1f %.1f %.1f): %.1f m away but a %.1f m walk, kept back", m, marker.x, marker.y, marker.z, nearest, walk);
                if (winding.spot == SIZE_MAX || walk < winding.walk) winding = Pick{index, m, nearest, roll, markerUsedAt[m], walk};
                continue;
            }
            if (pick.spot == SIZE_MAX || markerUsedAt[m] < pick.used || (markerUsedAt[m] == pick.used && roll > pick.roll))
                pick = Pick{index, m, nearest, roll, markerUsedAt[m], walk};
        }
    }
    if (pick.spot == SIZE_MAX && winding.spot != SIZE_MAX) {
        pick = winding;
        ++run.refusals.ventWindingUsed;
    }
    if (pick.spot == SIZE_MAX) return false;
    VentSpot& s = ventSpots[pick.spot];
    const auto& marker = ds2::kSurvivalVentMarkers[pick.marker];
    const auto& all = s.donors.find(item.family)->second;
    std::vector<size_t> skinOnly;
    if (!item.package.empty()) FilterPackage(all, item.package, skinOnly);
    const auto& donors = item.package.empty() ? all : skinOnly;
    const size_t donor = MaybeTwitcher(item, PickEscalated(std::string("vent:") + s.row->name + ":" + item.family + (item.package.empty() ? "" : ":" + item.package), donors, nullptr, !item.package.empty()), &s.twitchers, s.row->name);
    if (donor == SIZE_MAX) { ++run.refusals.noDonor; return false; }
    ds2::ExtraSpawnRule rule;
    rule.kind = item.family;
    rule.arrival = ds2::ExtraArrival::Here;
    uint32_t actor = 0;
    unsigned status = 0, bind = 0;
    bool armed = false, applied = false;
    ventRedirect = VentRedirect{true, false, marker.vent, {}};
    const Drive result = DriveOne(s.pointer, rule, donor, &s.source, actor, status, armed, applied, bind);
    const VentRedirect redirect = ventRedirect;
    ventRedirect = VentRedirect{};
    s.lastUseAt = now;
    markerUsedAt[pick.marker] = now;
    if (result == Drive::Busy) { ++run.refusals.lockBusy; return false; }
    if (result != Drive::Made) {
        ++s.refused;
        ++run.refusals.ventRefused;
        s.coolUntil = survival::DeadlineAfter(now, 10000);
        if (result == Drive::NotMade && status != kNotReached && !RebindRefused(bind)) ++donorFailures[donor];
        Refuse("vent spawn not made:", item, StatusText(status));
        return false;
    }
    ++s.made;
    if (armed && !applied) ++run.refusals.rebindMissed;
    const V3 at{marker.x, marker.y, marker.z};
    Track(actor, donor, true, at, now);
    tracked.back().spawner = s.pointer;
    tracked.back().marker = static_cast<int>(pick.marker);
    tracked.back().ownMarker = MarkerIndex(s.row->vent);
    tracked.back().super = item.super || SuperSkin(donor);
    if (Budget(run.spawnLines, cfg.logSpawnLines))
        Event("spawn #%u wave=%u family=%s skin=%s path=vent spawner=%s vent=%08X-%08X%s at (%.1f %.1f %.1f) donor=%08X-%08X actor=%08X nearest_viewer=%.1f m header=%s "
              "rebind=%s(result %u) alive=%u",
              run.rc.spawned, run.wave, item.family.c_str(), PackageOf(donor) ? PackageOf(donor) : "?", s.row->name, marker.vent[0], marker.vent[1],
              marker.vent == s.row->vent ? " (its own)" : "", at.x, at.y, at.z, pool[donor].guid[0], pool[donor].guid[1], actor, pick.nearest,
              !redirect.written ? "NOT WRITTEN" : redirect.was == redirect.vent ? "already there" : "redirected", !armed ? "same_header" : applied ? "applied" : "MISSED",
              bind, AliveCount());
    return true;
}

// Four players at four corners can see every spot of a small arena, and an item
// that finds no spot for 30 s is dropped: standing apart would empty the waves.
// The longer the current item waits, the more the search gives way, distance
// first and sight last (the user, 09.10.: in a small room rather right around
// the corner, as long as nobody sees it): from 4 s the distances shrink to 60 %,
// from 8 s 3 m from everyone is enough, from 14 s only a player facing the spot
// sees it, and from 22 s, just before the item would be dropped, sight no longer
// counts. Each search starts at the next player in turn, so a player standing
// alone gets enemies near him too.
constexpr uint32_t kRelaxShorterMs = 4000, kRelaxCornerMs = 8000, kRelaxFacingMs = 14000, kRelaxSightMs = 22000;
constexpr float kCornerDistance = 3.0f;
// 13.11. mp_escape, solo: a Leaper waited 15 s for a busy vent, and its first direct search
// already ran at level 3 (3 m) because the levels counted from the item's start.
unsigned RelaxLevel(uint32_t now) {
    if (!run.directSince) return 0;
    const uint32_t wait = now - run.directSince;
    return wait >= kRelaxSightMs ? 4 : wait >= kRelaxFacingMs ? 3 : wait >= kRelaxCornerMs ? 2 : wait >= kRelaxShorterMs ? 1 : 0;
}
const char* RelaxName(unsigned level) {
    static const char* const kNames[5] = {"strict", "distances at 60 %", "3 m from everyone, unseen", "3 m, unseen by anyone facing it",
                                          "3 m, sight ignored (last resort)"};
    return level < 5 ? kNames[level] : "?";
}
// The distance rules of a relax level.
survival::PointRules RelaxedRules(unsigned relax) {
    survival::PointRules r = survival::RulesOf(cfg);
    if (InArena()) {
        r.maxDistance = MaxDistanceHere();
        r.idealDistance = std::min(r.idealDistance, kArenaIdealDistance);
        r.maxPathFactor = std::min(r.maxPathFactor, kArenaPathFactor);
        if (r.minDistance > r.idealDistance - 2.0f) r.minDistance = r.idealDistance - 2.0f;
    }
    // an arena's rooms bound the search; its height rule replaces FloorBand there
    switch (ArenaHeights()) {
    case 0:
    case 2: r.floorBand = 30.0f; r.otherFloorBonus = 0.0f; break;
    case 1: r.floorBand = survival::kSameFloor; break;
    default: break;
    }
    if (relax == 1) {
        r.minDistance *= 0.6f;
        r.botMinDistance *= 0.6f;
        r.idealDistance *= 0.6f;
    } else if (relax >= 2) {
        r.minDistance = r.botMinDistance = kCornerDistance;
        r.idealDistance = std::max(kCornerDistance + 3.0f, r.idealDistance * 0.4f);
    }
    return r;
}
// Seen by a player looking its way (within 60 degrees of his facing).
bool SeenFacing(const V3& spot, const Viewer* viewers, unsigned count, unsigned& by) {
    for (unsigned i = 0; i < count; ++i) {
        const float tx = spot.x - viewers[i].eye.x, tz = spot.z - viewers[i].eye.z;
        const float fx = viewers[i].forward.x, fz = viewers[i].forward.z;
        const float tl = std::sqrt(tx * tx + tz * tz), fl = std::sqrt(fx * fx + fz * fz);
        if (tl > 0.5f && fl > 0.01f && (tx * fx + tz * fz) / (tl * fl) < 0.5f) continue;
        const V3 low = {spot.x, spot.y + 1.0f, spot.z}, high = {spot.x, spot.y + 1.8f, spot.z};
        if (Clear(viewers[i].eye, low, viewers[i].entity) || Clear(viewers[i].eye, high, viewers[i].entity)) { by = i; return true; }
    }
    return false;
}

// One step of the search for a direct spot: up to CandidatesPerTick seeds.
constexpr float kFloorStep = 4.5f;
void SearchStep(uint32_t now, const Viewer* viewers, unsigned viewerCount) {
    if (!viewerCount) return;
    const unsigned relax = RelaxLevel(now);
    if (!relax) run.relaxLogged = 0;
    if (relax > run.relaxLogged) {
        run.relaxLogged = relax;
        Event("wave %u: no spot for %u s with %u player(s) and bot(s): search relaxed to level %u (%s)", run.wave,
              static_cast<unsigned>((now - run.directSince) / 1000), viewerCount, relax, RelaxName(relax));
    }
    const survival::PointRules rules = RelaxedRules(relax);
    // the anchor takes turns among the living; a dead player waiting for the wave end is none
    if (!run.searching) {
        run.anchorIndex = run.anchorTurn++ % viewerCount;
        for (unsigned k = 0; k < viewerCount && !viewers[run.anchorIndex].alive; ++k) run.anchorIndex = run.anchorTurn++ % viewerCount;
    }
    if (run.anchorIndex >= viewerCount) run.anchorIndex = 0;
    if (!viewers[run.anchorIndex].alive) { run.searching = false; return; }   // everybody down
    const Viewer& anchor = viewers[run.anchorIndex];
    if (!run.searching) {
        run.searching = true;
        run.haveBest = false;
        run.bestScore = -1e9f;
        run.seeds.clear();
        run.seedNext = 0;
        ++run.refusals.searches;
        // Fresh rings around the player first: spots remembered from earlier
        // searches come last, or a run keeps reusing the area it started in.
        const float radii[] = {rules.minDistance + 2.0f, rules.idealDistance, rules.maxDistance - 1.5f};
        const auto circle = survival::RingSeeds(anchor.feet, anchor.forward, radii, 3, 12, run.rotation++);
        run.seeds.insert(run.seeds.end(), circle.begin(), circle.end());
        // The nav query snaps a seed to the mesh nearest it, so rings shifted up
        // and down reach the floors above and below.
        if (rules.floorBand > survival::kSameFloor)
            for (const float dy : {kFloorStep, -kFloorStep}) {
                if (dy > rules.floorBand + 0.5f || (dy > 0.0f && ArenaHeights() == 2)) continue;
                const V3 shifted = {anchor.feet.x, anchor.feet.y + dy, anchor.feet.z};
                const auto band = survival::RingSeeds(shifted, anchor.forward, &rules.idealDistance, 1, 12, run.rotation);
                run.seeds.insert(run.seeds.end(), band.begin(), band.end());
            }
        V3 points[32];
        const unsigned n = ReadSpawnPoints(points, 32);
        for (unsigned i = 0; i < n; ++i) {
            const float d = survival::Distance(points[i], anchor.feet);
            if (d >= rules.minDistance && d <= rules.maxDistance) run.seeds.push_back(points[i]);
        }
        if (InArena() && arenaSpots) {
            float spots[16][3]{};
            const int count = arenaSpots(&spots[0][0], 16);
            for (int i = 0; i < count && i < 16; ++i) {
                const V3 spot{spots[i][0], spots[i][1], spots[i][2]};
                const float d = survival::Distance(spot, anchor.feet);
                if (d >= rules.minDistance && d <= rules.maxDistance) run.seeds.push_back(spot);
            }
        }
        for (const V3& g : run.goodSpots) {
            const float d = survival::Distance(g, anchor.feet);
            if (d >= rules.minDistance && d <= rules.maxDistance) run.seeds.push_back(g);
        }
    }
    V3 feet[8];
    for (unsigned i = 0; i < viewerCount && i < 8; ++i) feet[i] = viewers[i].feet;
    for (unsigned step = 0; step < cfg.candidatesPerTick && run.seedNext < run.seeds.size(); ++step) {
        const V3 seed = run.seeds[run.seedNext++];
        V3 end;
        float length = 0.0f;
        survival::PointVerdict v = survival::PointVerdict::NoPath;
        if (NavWalk(anchor.feet, seed, end, length))
            v = survival::CheckPoint(end, anchor.feet, feet, viewerCount < 8 ? viewerCount : 8, length, rules);
        if (v == survival::PointVerdict::Ok && ArenaHeights() == 2 && end.y - anchor.feet.y > survival::kSameFloor) v = survival::PointVerdict::OtherFloor;
        if (v == survival::PointVerdict::Ok && InArena()) {
            // the nav mesh runs on through the locked doors
            const float at[3] = {end.x, end.y, end.z};
            if (!arenaInside || !arenaInside(at)) v = survival::PointVerdict::OutsideArena;
        }
        if (v == survival::PointVerdict::Ok && !Clear({end.x, end.y + 0.3f, end.z}, {end.x, end.y + 2.0f, end.z}, 0)) v = survival::PointVerdict::Wall;
        if (v == survival::PointVerdict::Ok) {
            const V3 before = end;
            if (!KeepOffWalls(end)) {
                v = survival::PointVerdict::Wall;
            } else if (survival::Distance(before, end) > 0.05f) {
                // moved: back onto the nav mesh, and it must land where it was put
                V3 snapped;
                float again = 0.0f;
                if (!NavWalk(anchor.feet, end, snapped, again) || survival::Distance(snapped, end) > 0.6f) v = survival::PointVerdict::Wall;
                else end = snapped;
            }
        }
        unsigned by = 0;
        if (v == survival::PointVerdict::Ok && relax < 4 && (relax < 3 ? Seen(end, viewers, viewerCount, by) : SeenFacing(end, viewers, viewerCount, by)))
            v = survival::PointVerdict::Visible;
        ++run.refusals.verdict[static_cast<unsigned>(v)];
        if (v != survival::PointVerdict::Ok) continue;
        bool known = false;
        for (const V3& g : run.goodSpots) if (survival::Distance(g, end) < 2.0f) known = true;
        if (!known) {
            if (run.goodSpots.size() < 24) run.goodSpots.push_back(end);
            else run.goodSpots[run.goodNext++ % 24] = end;   // the oldest gives way
        }
        const float score = survival::ScorePoint(end, anchor.feet, anchor.forward, run.recent.data(), run.recent.size(), rules, rng.Unit());
        if (score > run.bestScore) { run.bestScore = score; run.best = end; run.haveBest = true; }
        if (score >= 8.0f) break;   // good enough, no need to look further
    }
    if (run.haveBest && (run.bestScore >= 8.0f || run.seedNext >= run.seeds.size())) {
        run.searching = false;
        run.haveReady = true;
        run.ready = run.best;
        run.readyAt = now;
    } else if (run.seedNext >= run.seeds.size()) {
        run.searching = false;
        ++run.refusals.searchFailed;
    }
}

bool TryDirect(const survival::Item& item, uint32_t now, const Viewer* viewers, unsigned viewerCount) {
    if (!run.directSince) run.directSince = now ? now : 1;
    if (!viewerCount) return false;
    V3 spot;
    const bool follower = !item.groupHead && run.packDirect && now - run.packAt < 3000;
    if (follower) {
        spot = survival::PackOffset(run.packHead, ++run.packIndex);
        if (!KeepOffWalls(spot)) spot = run.packHead;   // no room beside the head: on it, as before the offsets
    } else {
        if (run.haveReady && now - run.readyAt > 2000) run.haveReady = false;   // the players moved on
        if (!run.haveReady) { SearchStep(now, viewers, viewerCount); if (!run.haveReady) return false; }
        spot = run.ready;
    }
    const auto list = directDonors.find(item.family);
    const auto enhanced = directEnhanced.find(item.family);
    static const std::vector<size_t> none;
    const std::vector<size_t>* plainList = list == directDonors.end() ? &none : &list->second;
    const std::vector<size_t>* enhancedList = enhanced == directEnhanced.end() ? nullptr : &enhanced->second;
    std::vector<size_t> plainSkin, enhancedSkin;
    if (!item.package.empty()) {
        FilterPackage(*plainList, item.package, plainSkin);
        plainList = &plainSkin;
        if (enhancedList) { FilterPackage(*enhancedList, item.package, enhancedSkin); enhancedList = &enhancedSkin; }
    }
    const size_t donor = MaybeTwitcher(item, list == directDonors.end() && enhanced == directEnhanced.end()
                             ? SIZE_MAX
                             : PickEscalated("direct:" + item.family + (item.package.empty() ? "" : ":" + item.package), *plainList, enhancedList, !item.package.empty()), nullptr, nullptr);
    if (donor == SIZE_MAX) { ++run.refusals.noDonor; Refuse("no direct donor left", item, nullptr); return false; }
    Engine* engine = nullptr;
    const uintptr_t spawner = PickEngine(&engine, viewers, viewerCount);
    if (!spawner) { ++run.refusals.noEngine; Refuse("no engine spawner", item, "(no live prop spawner and no area rule spawner)"); return false; }
    ds2::ExtraSpawnRule rule;
    rule.kind = item.family;
    rule.arrival = ds2::ExtraArrival::Direct;
    rule.hasPosition = true;
    rule.position[0] = spot.x;
    rule.position[1] = spot.y + 0.1f;
    rule.position[2] = spot.z;
    rule.hasYaw = true;
    rule.yaw = survival::FacingYaw(spot, viewers[0].feet);
    uint32_t actor = 0;
    unsigned status = 0, bind = 0;
    bool armed = false, applied = false;
    DividerPlacement placement;
    const bool placeDivider = run.placeDividersAtBirth && item.family == "DividerCore" && dividerLocationHook;
    if (placeDivider) {
        const double yaw = rule.yaw * 3.14159265358979 / 180.0;
        placement.matrix[0] = placement.matrix[10] = static_cast<float>(std::cos(yaw));
        placement.matrix[8] = static_cast<float>(std::sin(yaw));
        placement.matrix[2] = -placement.matrix[8];
        placement.matrix[5] = placement.matrix[15] = 1.0f;
        std::copy_n(rule.position, 3, placement.matrix + 12);
        placement.spawner = spawner;
    }
    auto* previousPlacement = dividerPlacement;
    dividerPlacement = placeDivider ? &placement : nullptr;
    const Drive result = DriveOne(spawner, rule, donor, nullptr, actor, status, armed, applied, bind);
    dividerPlacement = previousPlacement;
    if (placeDivider && result != Drive::Busy && Budget(run.dividerPlacementLines, 40)) {
        V3 actual{};
        const bool read = actor && ReadV3(actor + kEntityPos, actual);
        Event("Divider birth wave=%u actor=%08X engine=%08X drive=%u packet=%d location=%d target=(%.1f %.1f %.1f) measured=%d (%.1f %.1f %.1f)",
              run.wave, actor, static_cast<unsigned>(spawner), static_cast<unsigned>(result), placement.frame != 0,
              placement.locationApplied, spot.x, spot.y, spot.z, read, actual.x, actual.y, actual.z);
    }
    Guid engineGuid{};
    Read(static_cast<uint32_t>(spawner) + kEntityGuid, engineGuid.data(), 16);
    if (result == Drive::Busy) { ++run.refusals.lockBusy; return false; }
    if (result == Drive::OwnTarget) {
        ++run.refusals.ownTarget;
        if (engine) engine->bad = true;
        Refuse("engine built its own target instead of the donor:", item, StatusText(status));
        return false;
    }
    if (result == Drive::NotMade) {
        ++run.refusals.notMade;
        // A vent spawner as the engine whose vent contract refused the donor: the engine is at fault
        // (17.10., Transport Hub: an arena vent spawner failed all 15 slasher donors three times, and
        // the Stalker Room afterwards had no direct slasher left).
        if (RebindRefused(bind)) {
            if (engine) engine->bad = true;
            else if (!EngineAvoided(spawner)) avoidedEngines.push_back(spawner);
            Event("engine %08X-%08X: its vent contract refused donor %08X-%08X (bind %u); not used for direct spawns again this run", engineGuid[0], engineGuid[1],
                  pool[donor].guid[0], pool[donor].guid[1], bind);
            return false;
        }
        if (status != kNotReached) ++donorFailures[donor];
        if (!engine && status == kNotReached && NoteEngineRefusal(spawner)) {
            ++run.enginesRested;
            Event("engine %08X-%08X (area rule picker%s) refused three spawns in a row before the lookup: rests 30 s", engineGuid[0], engineGuid[1],
                  !InArena() ? "" : EngineInsideArena(spawner) ? ", inside the arena" : ", OUTSIDE the arena");
        }
        if (engine) {
            ++engine->failed;
            if (status == kNotReached && !engine->made && engine->failed >= 3) {
                engine->bad = true;
                Event("engine %s %08X-%08X retired: three spawns refused before the lookup", engine->row->name, engine->row->spawner[0], engine->row->spawner[1]);
            }
        }
        char detail[200];
        std::snprintf(detail, sizeof(detail), "%s (donor %08X-%08X failures %u, engine %08X-%08X)", StatusText(status), pool[donor].guid[0], pool[donor].guid[1],
                      donorFailures[donor], engineGuid[0], engineGuid[1]);
        Refuse("direct spawn not made:", item, detail);
        return false;
    }
    if (engine) ++engine->made;
    NoteEngineMade(spawner);
    run.haveReady = false;
    if (item.groupHead) { run.packHead = spot; run.packIndex = 0; run.packAt = now; run.packDirect = item.group > 1; }
    run.recent.push_back(spot);
    if (run.recent.size() > 6) run.recent.erase(run.recent.begin());
    Track(actor, donor, false, spot, now);
    tracked.back().engine = spawner;
    tracked.back().spawner = spawner;
    tracked.back().super = item.super || SuperSkin(donor);
    float nearest = 1e9f;
    for (unsigned i = 0; i < viewerCount; ++i) nearest = std::min(nearest, survival::Distance(spot, viewers[i].feet));
    if (Budget(run.spawnLines, cfg.logSpawnLines))
        Event("spawn #%u wave=%u family=%s skin=%s path=direct%s donor=%08X-%08X actor=%08X at (%.1f %.1f %.1f) from_player=%.1f m dy=%+.1f (%s floor) nearest_viewer=%.1f m viewers=%u los=blocked "
              "engine=%08X-%08X%s%s alive=%u relax=%u anchor=%u",
              run.rc.spawned, run.wave, item.family.c_str(), PackageOf(donor) ? PackageOf(donor) : "?", follower ? " (pack)" : "", pool[donor].guid[0], pool[donor].guid[1], actor, spot.x, spot.y, spot.z,
              survival::Distance(spot, viewers[0].feet), spot.y - viewers[0].feet.y,
              std::fabs(spot.y - viewers[0].feet.y) > survival::kSameFloor ? "other" : "same", nearest, viewerCount, engineGuid[0], engineGuid[1], engine ? "" : " (area rule picker)",
              !InArena() ? "" : EngineInsideArena(spawner) ? " inside the arena" : " OUTSIDE the arena", AliveCount(), RelaxLevel(now), run.anchorIndex);
    return true;
}

// The next item of the queue, when its time has come.
unsigned playersSeen = 1;   // PlayerManager's bodies at the last tick: the human, partners and bots
// The host's Max alive from the debug menu, 0 = the INI's. The NavPower guard below still applies.
volatile LONG adminCap = 0;
// maxalive= in the arena file (user 21.10.: 15 at once was too many for one player in the
// security checkpoint): the arena's one-player cap instead of MaxAlive; a wave's own MaxAlive
// counts only below it. MaxAlive is the player's own setting: arena and wave only lower it (24.11.).
// Further human players add MaxAlivePerPlayer each, bots MaxAlivePerBot.
unsigned BaseCap(const char** from = nullptr) {
    const coop_arena::Arena* a = RunArena();
    const unsigned arenaCap = a ? a->maxAlive : 0u, wave = run.spec.maxAlive;
    const unsigned lowest = survival::BaseMaxAlive(cfg.maxAlive, arenaCap, wave);
    if (from) *from = lowest == cfg.maxAlive ? "MaxAlive" : wave && lowest == wave ? "the wave's MaxAlive" : "the arena's maxalive=";
    return lowest;
}
// Human players of the run, this one included.
unsigned HumansSeen() {
    float partners[8 * 3];
    const int n = partnerPositions && NetRole() == 1 ? partnerPositions(partners, 8) : 0;
    const unsigned humans = 1u + static_cast<unsigned>(n > 0 ? n : 0);
    return humans < playersSeen ? humans : playersSeen;
}
unsigned WaveCap() {
    const unsigned baseCap = BaseCap();
    const LONG forced = InterlockedCompareExchange(&adminCap, 0, 0);
    const unsigned dynamic = forced > 0 ? static_cast<unsigned>(forced) : survival::MaxAliveFor(baseCap, HumansSeen(), playersSeen, cfg);
    return survival::EffectiveCap(dynamic, dynamic, navpower::enlarged > 0);
}
// Probe one missing vent at a time while sectors stream during a wave.
void RecoverVent(uint32_t now) {
    if (!VentPathOn() || !run.probeMissing || survival::DeadlinePending(now, run.ventRetryAt)) return;
    run.ventRetryAt = survival::DeadlineAfter(now, 1000);
    for (size_t i = 0; i < ventSpots.size(); ++i) {
        auto& s = ventSpots[run.ventRetryCursor++ % ventSpots.size()];
        if (s.resolvable || !survival::EqualNoCase(s.row->map, run.map)) continue;
        const uint32_t pointer = KnownSpawnerByGuid(s.row->spawner);
        if (pointer && SpawnerTargetResolves(pointer)) {
            // Recompute family availability now that this sector can supply donors.
            ++run.reprobes;
            Probe(now, true);
        }
        break;
    }
}
void SpawnStep(uint32_t now, const Viewer* viewers, unsigned viewerCount) {
    if (run.next >= run.queue.size()) return;
    if (now - run.waveAt < cfg.waveStartDelayMs) return;
    const survival::Item& item = run.queue[run.next];
    const unsigned cap = WaveCap();
    const unsigned interval = run.spec.intervalMs ? run.spec.intervalMs : cfg.intervalMs;
    const unsigned alive = AliveCount();
    const bool follower = !item.groupHead;
    if (!survival::SpawnDue(now, run.lastSpawnAt, interval, alive, cap, follower)) {
        if (alive >= cap) ++run.refusals.cap;
        return;
    }
    if (cfg.navReserveKiB) {
        uint32_t top = 0, dv = 0, footprint = 0;
        if (!run.navGateOff && NavHeadroom(top, dv, footprint) && top + dv < cfg.navReserveKiB * 1024u) {
            ++run.refusals.navBudget;
            if (!run.navLowSince) run.navLowSince = now;
            if (now - run.navLowSince >= 30000 && !AliveCount()) {
                run.navGateOff = true;
                Event("navpower gate off for this run: low for 30 s with no survival enemy alive (top=%u dv=%u footprint=%u); the alive cap alone guards now",
                      top, dv, footprint);
            }
            if (now - run.lastNavLog > 10000) {
                run.lastNavLog = now;
                Event("navpower pool low: top=%u dv=%u footprint=%u (reserve %u KiB); spawning waits", top, dv, footprint, cfg.navReserveKiB);
            }
            return;
        }
        run.navLowSince = 0;
    }
    if (!run.itemSince) { run.itemSince = now; run.itemTries = 0; }
    // an Ubermorph already hunting is not doubled
    if (survival::Unkillable(item.family)) {
        for (const auto& t : tracked)
            if (t.actor && t.unkillable) {
                Event("wave %u: an Ubermorph (%08X) is already hunting; no second one", run.wave, t.actor);
                ++run.next;
                run.itemSince = 0;
                run.directSince = 0;
                return;
            }
    }
    ++run.itemTries;
    const bool ventsOnly = ArenaVentsOnly();
    // for an item that comes as an ordinary one a vent with enhanced donors alone is none
    const bool hasVent = item.package.empty() && !ItemEnhanced() ? FamilyHasVent(item.family, true) : ItemHasVent(item);
    if (ventsOnly && !hasVent && item.package.empty() && item.family != "Zombie" && FamilyHasVent(item.family) && FamilyHasVent("Zombie", true)) {
        InterlockedIncrement(&ordinaryStandIns);
        Event("wave %u: the arena's vents have only enhanced %s, a Zombie comes instead", run.wave, item.family.c_str());
        run.queue[run.next].family = "Zombie";
        return;
    }
    const bool ventWanted = ventsOnly ? hasVent && VentPathOn() : survival::WantsVent(item.path, hasVent, VentPathOn(), cfg.ventSharePercent, rng.Unit());
    bool made = false;
    const bool ventTried = ventWanted && (item.groupHead || ventsOnly);
    if (ventTried) made = TryVent(item, now, viewers, viewerCount);
    const bool mayDirect = !ventsOnly && (item.path != survival::Path::Vent || !hasVent || now - run.itemSince >= cfg.ventWaitMs);
    const unsigned directHopeless = run.refusals.noDonor + run.refusals.noEngine;
    if (!made && mayDirect) made = TryDirect(item, now, viewers, viewerCount);
    // no direct donor or engine: the vents are the only way in (17.10., Stalker Room)
    if (!made && !ventTried && hasVent && VentPathOn() && run.refusals.noDonor + run.refusals.noEngine != directHopeless) {
        ++run.ventFallbacks;
        made = TryVent(item, now, viewers, viewerCount);
    }
    if (made) {
        run.lastSpawnAt = now;
        ++run.next;
        run.itemSince = 0;
        run.directSince = 0;
        return;
    }
    // an item that cannot be placed for 30 s is given up, so the wave can end
    if (now - run.itemSince >= 30000) {
        ++run.wc.dropped;
        ++run.rc.dropped;
        Event("wave %u: %s (%s) given up after %u tries in 30 s; refusals so far: search_failed=%u visible=%u no_path=%u too_close=%u other_floor=%u "
              "not_made=%u vent_busy=%u vent_seen=%u", run.wave, item.family.c_str(), survival::PathName(item.path), run.itemTries,
              run.refusals.searchFailed, run.refusals.verdict[static_cast<unsigned>(survival::PointVerdict::Visible)],
              run.refusals.verdict[static_cast<unsigned>(survival::PointVerdict::NoPath)], run.refusals.verdict[static_cast<unsigned>(survival::PointVerdict::TooClose)],
              run.refusals.verdict[static_cast<unsigned>(survival::PointVerdict::OtherFloor)], run.refusals.notMade, run.refusals.ventBusy, run.refusals.ventSeen);
        ++run.next;
        run.itemSince = 0;
        run.directSince = 0;
    }
}

// --- tracking ---
// An enemy from a vent that got nowhere: the second one retires that vent for the run.
void NoteVentFailure(const Tracked& t) {
    if (!t.vent || t.marker < 0 || t.marker >= static_cast<int>(std::size(ds2::kSurvivalVentMarkers)) || markerRetired[t.marker]) return;
    if (const auto found = std::lower_bound(pool.begin(), pool.end(), t.donor, [](const auto& r, const Guid& g) { return r.guid < g; });
        found != pool.end() && found->guid == t.donor && found->ventBlocked)
        donorFailures[static_cast<size_t>(found - pool.begin())] = 3;   // the campaign was right about this record
    if (++markerFailures[t.marker] < 2) return;
    markerRetired[t.marker] = true;
    InterlockedIncrement(&ventsRetired);
    const auto& m = ds2::kSurvivalVentMarkers[t.marker];
    Event("vent %08X-%08X at (%.1f %.1f %.1f) retired for this run: two of its arrivals reached nobody (last %s actor=%08X)", m.vent[0], m.vent[1], m.x, m.y,
          m.z, t.family.c_str(), t.actor);
}
void KillActor(Tracked& t, const char* why, survival::Fate fate, uint32_t now) {
    if (!killReady || t.killSent || !SameEntity(t.actor, t.vtable, t.guid)) return;
    const uint32_t component = FindHealth(t.actor);
    uint32_t vtable = 0, slot = 0;
    t.killSent = true;
    t.killFate = fate;
    t.killAt = now;
    if (!component || !Read(component, &vtable, 4) || !Read(vtable + kSetHealthSlot, &slot, 4) || slot != base + kSetHealth) {
        Event("watchdog %s actor=%08X family=%s: no health component with the verified SetHealth slot; written off", why, t.actor, t.family.c_str());
        return;
    }
    const bool ok = SetHealthRaw(component, 0.0f);
    if (ok) cleanup::NoteKill(t.actor);
    Event("watchdog %s actor=%08X family=%s age=%lus engaged=%d: SetHealth(0) %s", why, t.actor, t.family.c_str(),
          static_cast<unsigned long>((now - t.watch.bornAt) / 1000), t.watch.engaged ? 1 : 0, ok ? "called" : "FAULTED");
}
void MercsKill(const Tracked& t, uint32_t now);   // below, with the Mercenaries code
// Versus: a slasher gone frees its slot; the map's own respawn delay after a
// kill, a short one when it vanished or was written off.
void VersusRetired(survival::Fate fate, uint32_t now) {
    if (!run.versusPlaying) return;
    const bool killed = fate == survival::Fate::Killed;
    if (killed) ++run.vKilled;
    run.vDue.push_back(now + (killed ? survival::VersusRespawnMs(cfg.versus, rng.Unit()) : 3000u));
}
// Done with an enemy: count it and stop tracking.
// The profile XP (coop_progress.h): the host or a solo director pays, the co-op
// plugin books it and sends it to the partners (DS2Coop_SurvivalProgress). A run
// with a host tool pays nothing anywhere.
coop_progress::XpRules xpRules;   // [ProgressXp]
volatile LONG xpPayments = 0, xpPaid = 0;
void PayXp(coop_progress::Event what, unsigned wave, uint32_t xp) {
    if (!xp || run.versus || NetRole() == 2) return;
    InterlockedIncrement(&xpPayments);
    InterlockedExchangeAdd(&xpPaid, static_cast<LONG>(xp));
    Event("xp: %s, run %u wave %u pays %u%s%s", coop_progress::EventName(what), run.number, wave, xp,
          run.adminUsed ? " (host tool used: refused everywhere)" : "", survivalProgress ? "" : " (DS2Coop_SurvivalProgress MISSING)");
    if (survivalProgress)
        survivalProgress(static_cast<int>(run.number), static_cast<int>(wave), static_cast<int>(xp), static_cast<int>(coop_progress::Flags(what, run.adminUsed)));
}
// The unfinished wave's kills, once per run (death, game over, the run's end).
void PayRunEnd() {
    if (run.xpRunPaid || run.mercs || run.objective || run.zone) return;
    run.xpRunPaid = true;
    PayXp(coop_progress::kRunEnd, run.wave, run.xpWave);
    run.xpWave = 0;
}
void Retire(Tracked& t, survival::Fate fate, uint32_t now) {
    if (run.versus) VersusRetired(fate, now);
    const bool inWave = t.wave == run.wave && !t.unkillable && (run.state == State::Wave);
    Counters* sets[2] = {inWave ? &run.wc : nullptr, &run.rc};
    for (Counters* c : sets) {
        if (!c) continue;
        switch (fate) {
        case survival::Fate::Killed: ++c->killed; break;
        case survival::Fate::Vanished: ++c->vanished; break;
        case survival::Fate::Stuck: ++c->stuckKills; break;
        case survival::Fate::VentStuck: ++c->ventStuckKills; break;
        case survival::Fate::Inert: ++c->inert; break;
        case survival::Fate::Timer: ++c->timerKills; break;
        default: ++c->writtenOff; break;
        }
    }
    if (fate == survival::Fate::Killed) InterlockedIncrement(&totalKilled);
    if (fate == survival::Fate::Killed && inWave && !run.versus && !run.mercs && !run.objective && !run.zone)
        run.xpWave += coop_progress::KillXp(xpRules,
                                            survival::RoleOf(t.family) == survival::Role::Heavy ? coop_progress::KillSize::Heavy
                                            : survival::EqualNoCase(t.family, "SwarmEnemy")      ? coop_progress::KillSize::Swarm
                                                                                                 : coop_progress::KillSize::Normal,
                                            superGuids.count(t.donor) != 0);
    if (fate == survival::Fate::Killed && run.mercs && run.state == State::Wave) MercsKill(t, now);
    if (fate == survival::Fate::Killed && run.objective && run.state == State::Wave) ++run.objKills;
    if (fate == survival::Fate::Killed && run.zone && run.state == State::Wave) ++run.zoneKills;
    const uint32_t untargeted = t.untargetedMs + (t.lostAt ? now - t.lostAt : 0);
    if (t.lostAt && fate != survival::Fate::Killed) ++aw.diedUntargeted;
    if (Budget(run.killLines, 600))
        Event("%s wave=%u family=%s path=%s actor=%08X alive=%lus engaged_after=%s target_after=%s target_lost=%u untargeted=%lus charged=%d forces=%u spawned_at=(%.1f %.1f %.1f) left=%u",
              survival::FateName(fate), t.wave, t.family.c_str(), t.vent ? "vent" : "direct", t.actor, static_cast<unsigned long>((now - t.watch.bornAt) / 1000),
              t.watch.engaged ? std::to_string((t.watch.engagedAt - t.watch.bornAt) / 1000).append("s").c_str() : "never",
              t.targetAt ? std::to_string((t.targetAt - t.watch.bornAt) / 1000).append("s").c_str() : "never", t.losses,
              static_cast<unsigned long>(untargeted / 1000), t.charged ? 1 : 0, t.forces, t.spawnedAt.x, t.spawnedAt.y, t.spawnedAt.z, Left());
    if (!t.targetAt && awareReady) ++aw.neverTargeted;
    t.actor = 0;
}
// --- awareness: every survival enemy hunts the players ---
// analysis/DS2_SURVIVAL_AWARENESS_2026-10-09.md. Out-of-sight spawns are not
// aware, some templates track no group at all, and only one Cadaverous NPC in
// the level may charge at a time.

void ApplyChargeSight() {
    if (!chargeSightReady || !cfg.chargeUnseen || (cfg.chargeUnseen == 1 && !InArena()) || InterlockedCompareExchange(&levelEndingAsked, 0, 0)) return;
    AcquireSRWLockExclusive(&chargeLock);
    if (!chargeSightSet) {
        uint8_t now = 0;
        const bool ok = Read(base + kChargeSightJump, &now, 1) && now == kJe && WriteBytes(base + kChargeSightJump, &kJo, 1);
        if (ok) FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(base + kChargeSightJump), 1);
        chargeSightSet = ok;
        Event("chargers: charge without sight %s for this run (%s)", ok ? "on" : "NOT SET", InArena() ? "campaign arena" : "every map");
    }
    ReleaseSRWLockExclusive(&chargeLock);
}
void ApplyChargeMax() {
    ApplyChargeSight();
    if (!chargeMaxReady || !cfg.maxChargers || chargeMaxSet || InterlockedCompareExchange(&levelEndingAsked, 0, 0)) return;
    uint32_t before = 0;
    if (!Read(base + kChargeMax, &before, 4)) return;
    const uint32_t want = cfg.maxChargers;
    if (before >= want) { Event("chargers: engine maximum already %u, left alone", before); return; }
    if (WriteBytes(base + kChargeMax, &want, 4)) {
        chargeMaxBefore = before;
        chargeMaxSet = true;
        Event("chargers: maximum %u -> %u for this run", before, want);
    } else Event("chargers: maximum write FAILED");
}
// Run end: the global maximum goes back; template edits only where our value is still in place.
void RestoreCharging() {
    AcquireSRWLockExclusive(&chargeLock);
    if (chargeMaxSet) {
        uint32_t count = 0;
        Read(base + kChargeCount, &count, 4);
        const bool ok = WriteBytes(base + kChargeMax, &chargeMaxBefore, 4);
        Event("chargers: maximum back to %u (%s), count=%u at run end", chargeMaxBefore, ok ? "ok" : "FAILED", count);
        chargeMaxSet = false;
    }
    if (chargeSightSet) {
        const bool ok = WriteBytes(base + kChargeSightJump, &kJe, 1);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(base + kChargeSightJump), 1);
        chargeSightSet = false;
        Event("chargers: a charge needs sight again (%s)", ok ? "ok" : "FAILED");
    }
    unsigned groupsBack = 0;
    for (const TrackEdit& e : trackEdits) {
        uint32_t slot = 0;
        const uint32_t none = 0xFFFFFFFFu;
        if (Writable(e.attr + kAttrGroups + 4 * e.slot, 4) && Read(e.attr + kAttrGroups + 4 * e.slot, &slot, 4) && slot == kGroupPlayer &&
            WriteBytes(e.attr + kAttrGroups + 4 * e.slot, &none, 4))
            ++groupsBack;
    }
    if (!trackEdits.empty()) Event("tracking groups put back=%u of %zu", groupsBack, trackEdits.size());
    trackEdits.clear();
    unsigned rangesBack = 0;
    for (const RangeEdit& e : rangeEdits) {
        float sq = 0.0f;
        if (Writable(e.attr + kAttrTrackSq, 4) && Read(e.attr + kAttrTrackSq, &sq, 4) && sq == e.newSq && WriteBytes(e.attr + kAttrTrackSq, &e.oldSq, 4)) ++rangesBack;
    }
    if (!rangeEdits.empty()) Event("tracking ranges put back=%u of %zu", rangesBack, rangeEdits.size());
    rangeEdits.clear();
    unsigned restored = 0, left = 0;
    for (const auto& e : chargeEdits) {
        float start = 0.0f;
        uint8_t vis = 0;
        if (Writable(e.packet + kChargeStart, 4) && Read(e.packet + kChargeStart, &start, 4) && start == e.newStart && Read(e.packet + kChargeIgnoreVis, &vis, 1) &&
            vis == e.newVis && WriteBytes(e.packet + kChargeStart, &e.oldStart, 4) && WriteBytes(e.packet + kChargeIgnoreVis, &e.oldVis, 1))
            ++restored;
        else
            ++left;
    }
    if (!chargeEdits.empty()) Event("charge templates restored=%u left=%u (left: packet gone or changed since)", restored, left);
    chargeEdits.clear();
    chargePackets.clear();
    ReleaseSRWLockExclusive(&chargeLock);
}
// From the release hooks, on whatever thread runs them: template values and the
// charger limit go back now, while the pool is still loaded; the run itself ends
// on the director's next tick, and nothing else happens until a level runs again.
void LevelEnding(const char* why) {
    if (!installed) return;
    __try {
        RestoreCharging();
    } __except (FaultFilter(GetExceptionInformation())) {
    }
    if (InterlockedExchange(&levelEndingAsked, 1) == 0) {
        InterlockedExchange(&levelEndingEpoch, InterlockedCompareExchange(&streamServiceEpoch, 0, 0));
        Event("level ending at %s: charging restored, the run ends on the next tick", why);
    }
}
void AwareTemplate(const Tracked& t, uint32_t attr) {
    uint32_t groups[4]{};
    float trackSq = 0, alwaysSq = 0, span = 0;
    uint8_t atSpawn = 0;
    if (!Read(attr + kAttrGroups, groups, sizeof(groups))) return;
    Read(attr + kAttrTrackSq, &trackSq, 4);
    Read(attr + kAttrAlwaysSq, &alwaysSq, 4);
    Read(attr + kAttrSpan, &span, 4);
    Read(attr + kAttrAtSpawn, &atSpawn, 1);
    const bool blind = groups[0] == 0xFFFFFFFFu && groups[1] == 0xFFFFFFFFu && groups[2] == 0xFFFFFFFFu && groups[3] == 0xFFFFFFFFu;
    const char* result = "unchanged";
    // No registration hangs off the group ids; the awareness pass reads them every update.
    const bool tracksPlayers = groups[0] == kGroupPlayer || groups[1] == kGroupPlayer || groups[2] == kGroupPlayer || groups[3] == kGroupPlayer;
    if (blind && cfg.repairBlindTemplates) {
        if (WriteBytes(attr + kAttrGroups, &kGroupPlayer, 4)) { result = "repaired (GroupPlayer)"; ++aw.repaired; }
        else result = "repair FAILED";
    } else if (blind) {
        result = "blind, left (RepairBlindTemplates=0)";
    } else if (!tracksPlayers && cfg.repairBlindTemplates && !run.versus) {
        // tracks somebody else only (Ellie's slashers: "Human NPCs"): the players join for the run
        unsigned slot = 0;
        while (slot < 4 && groups[slot] != 0xFFFFFFFFu) ++slot;
        if (slot == 4) result = "tracks others only, no free slot";
        else if (WriteBytes(attr + kAttrGroups + 4 * slot, &kGroupPlayer, 4)) {
            AcquireSRWLockExclusive(&chargeLock);
            trackEdits.push_back(TrackEdit{attr, slot});
            ReleaseSRWLockExclusive(&chargeLock);
            ++aw.groupAdded;
            result = "tracks others only: GroupPlayer added for the run";
        } else result = "group add FAILED";
    }
    // At least as far as the catch-up reaches, or an enemy between the two forgets the players
    // and wanders until it is taken away.
    float wantM = cfg.trackMeters;
    if (wantM > 0.0f && cfg.catchUpMeters > 0.0f) wantM = std::max(wantM, cfg.catchUpMeters + 10.0f);
    const float wantSq = wantM * wantM;
    const char* range = "kept";
    if (wantM > 0.0f && std::isfinite(trackSq) && trackSq < wantSq) {
        if (WriteBytes(attr + kAttrTrackSq, &wantSq, 4)) {
            AcquireSRWLockExclusive(&chargeLock);
            rangeEdits.push_back(RangeEdit{attr, trackSq, wantSq});
            ReleaseSRWLockExclusive(&chargeLock);
            ++aw.rangeRaised;
            range = "raised";
        } else {
            ++aw.writeFails;
            range = "raise FAILED";
        }
    }
    if (Budget(awareLines, 64))
        Event("aware template family=%s donor=%08X-%08X attr=%08X groups=[%08X %08X %08X %08X] at_spawn=%u span=%.0f track_m=%.0f always_m=%.0f -> %s; track range %s%s", t.family.c_str(),
              t.donor[0], t.donor[1], attr, groups[0], groups[1], groups[2], groups[3], atSpawn, span, std::sqrt(trackSq > 0 ? trackSq : 0.0f),
              std::sqrt(alwaysSq > 0 ? alwaysSq : 0.0f), result, range,
              std::strcmp(range, "raised") == 0 ? (" to " + std::to_string(static_cast<int>(wantM)) + " m").c_str() : "");
}
// Zombie templates: run whenever they may, also off the target's screen.
void ChargeTemplate(const Tracked& t) {
    if (!zombieChargeReady || t.vtable != base + kZombieVtable || InterlockedCompareExchange(&levelEndingAsked, 0, 0)) return;
    uint32_t packet = 0;
    if (!Read(t.actor + 0x0C, &packet, 4) || !packet) return;
    AcquireSRWLockExclusive(&chargeLock);
    const bool fresh = chargePackets.insert(packet).second;
    ReleaseSRWLockExclusive(&chargeLock);
    if (!fresh) return;
    float start = 0.0f;
    uint8_t vis = 0;
    if (!Read(packet + kChargeStart, &start, 4) || !Read(packet + kChargeIgnoreVis, &vis, 1) || !std::isfinite(start)) return;
    ChargeEdit e{packet, start, start, vis, vis};
    if (cfg.zombieChargeFloor > 0.0f && start < cfg.zombieChargeFloor) e.newStart = cfg.zombieChargeFloor;
    if (cfg.zombieChargeIgnoreVis) e.newVis = 1;
    bool ok = true;
    if (e.newStart != e.oldStart) ok = WriteBytes(packet + kChargeStart, &e.newStart, 4) && ok;
    if (e.newVis != e.oldVis) ok = WriteBytes(packet + kChargeIgnoreVis, &e.newVis, 1) && ok;
    if (e.newStart != e.oldStart || e.newVis != e.oldVis) {
        AcquireSRWLockExclusive(&chargeLock);
        chargeEdits.push_back(e);
        ReleaseSRWLockExclusive(&chargeLock);
    }
    if (Budget(awareLines, 64))
        Event("charge template donor=%08X-%08X packet=%08X start=%.2f->%.2f ignore_vis=%u->%u%s", t.donor[0], t.donor[1], packet, e.oldStart, e.newStart, e.oldVis,
              e.newVis, ok ? "" : " WRITE FAILED");
}
// Versus: whom the slashers go for. The pool's donors track GroupPlayer; whether
// a NecroPlayer is in it is unknown offline. A slasher whose target is one
// switches its template to the human team group (team_player, DGM+0x238, the
// group Versus puts every human into), put back when the run ends.
constexpr uint32_t kNecroPlayerVtable = 0x17783A0, kTeamPlayerGroup = 0x91E21583;
volatile LONG versusTargetsHuman = 0, versusTargetsNecro = 0, versusTargetsOther = 0, versusRetargeted = 0;
struct GroupEdit { uint32_t attr; uint32_t old[4]; };
std::vector<GroupEdit> groupEdits;
void RetargetTemplate(uint32_t attr, const Tracked& t) {
    for (const auto& e : groupEdits) if (e.attr == attr) return;
    GroupEdit e{attr, {}};
    if (!Read(attr + kAttrGroups, e.old, sizeof(e.old))) return;
    const uint32_t want[4] = {kTeamPlayerGroup, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    const bool ok = WriteBytes(attr + kAttrGroups, want, sizeof(want));
    if (ok) { groupEdits.push_back(e); InterlockedIncrement(&versusRetargeted); }
    Event("versus retarget donor=%08X-%08X attr=%08X groups=[%08X %08X %08X %08X] -> team_player %s", t.donor[0], t.donor[1], attr, e.old[0], e.old[1], e.old[2],
          e.old[3], ok ? "written" : "WRITE FAILED");
}
void RestoreVersusGroups() {
    for (const auto& e : groupEdits) {
        uint32_t now0 = 0;
        if (Read(e.attr + kAttrGroups, &now0, 4) && now0 == kTeamPlayerGroup) WriteBytes(e.attr + kAttrGroups, e.old, sizeof(e.old));
    }
    if (!groupEdits.empty()) Event("versus retarget: %zu template(s) put back", groupEdits.size());
    groupEdits.clear();
}
// target: the SafePtr at NPC+0x6F8, which points at entity+0x10.
void VersusTarget(Tracked& t, uint32_t target, uint32_t attr) {
    uint32_t vtable = 0;
    if (!target || target < 0x10010 || !Read(target - 0x10, &vtable, 4)) return;
    const int kind = vtable == base + kPlayerVtable ? 1 : vtable == base + kNecroPlayerVtable ? 2 : 3;
    if (kind == t.targetKind) return;
    t.targetKind = kind;
    InterlockedIncrement(kind == 1 ? &versusTargetsHuman : kind == 2 ? &versusTargetsNecro : &versusTargetsOther);
    if (Budget(run.vLines, 120) || kind == 2)
        Event("versus target actor=%08X donor=%08X-%08X -> %08X (%s)", t.actor, t.donor[0], t.donor[1], target - 0x10,
              kind == 1 ? "a human" : kind == 2 ? "a NECRO PLAYER" : "something else");
    if (kind == 2 && cfg.versus.retargetFromNecros && attr) RetargetTemplate(attr, t);
}
__declspec(noinline) bool ClearPlayDead(uint32_t actor, bool& wasOn) {
    __try {
        const LONG before = InterlockedAnd(reinterpret_cast<volatile LONG*>(actor + kNpcFlags), ~static_cast<LONG>(kNpcPlayDeadEnabled));
        wasOn = (before & kNpcPlayDeadEnabled) != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Per poll and live enemy: force awareness (again every AwareRefreshMs), note its first target.
void Awareness(Tracked& t, uint32_t now) {
    if (!awareReady || t.awareDone || now - t.watch.bornAt < 300) return;
    uint32_t comp = 0, vtable = 0, owner = 0;
    if (!Read(t.actor + kNpcAwareness, &comp, 4) || !comp || !Read(comp, &vtable, 4) || vtable != base + kAwareVtable || !Read(comp + kAwOwner, &owner, 4) ||
        owner != t.actor) {
        if (now - t.watch.bornAt > 5000) {
            t.awareDone = true;
            ++(comp && vtable ? aw.mismatch : aw.noComponent);
            if (Budget(awareLines, 64))
                Event("aware actor=%08X family=%s: no awareness component (comp=%08X vtable=%08X owner=%08X)", t.actor, t.family.c_str(), comp, vtable, owner);
        }
        return;
    }
    uint32_t attr = 0;
    if (Read(comp + kAwAttr, &attr, 4) && attr && awarePackets.insert(attr).second) AwareTemplate(t, attr);
    ChargeTemplate(t);
    if (!t.playDeadChecked && cfg.noPlayDead && playDeadReady) {
        t.playDeadChecked = true;
        bool wasOn = false;
        if (ClearPlayDead(t.actor, wasOn)) { if (wasOn) ++aw.playDeadOff; }
        else ++aw.writeFails;
    }
    if (cfg.forceAware && (!t.forcedAt || now - t.forcedAt >= cfg.awareRefreshMs)) {
        uint8_t flags = 0;
        if (Read(comp + kAwFlags, &flags, 1)) {
            const uint8_t want = static_cast<uint8_t>(flags | 4);
            if (want == flags || WriteBytes(comp + kAwFlags, &want, 1)) {
                if (!t.forcedAt) {
                    ++aw.forced;
                    uint32_t count = 0;
                    Read(comp + kAwCount, &count, 4);
                    if (Budget(run.spawnLines, cfg.logSpawnLines))
                        Event("aware actor=%08X family=%s comp=%08X flags_before=%02X tracked=%u forced_at=+%.1fs", t.actor, t.family.c_str(), comp, flags, count,
                              (now - t.watch.bornAt) / 1000.0f);
                }
                t.forcedAt = now;
                ++t.forces;
            } else ++aw.writeFails;
        }
    }
    if (run.versus) {
        uint32_t target = 0;
        if (Read(t.actor + kNpcTarget, &target, 4) && target) VersusTarget(t, target, attr);
    }
    if (t.targetAt) {
        uint32_t target = 0;
        Read(t.actor + kNpcTarget, &target, 4);
        if (!target && !t.lostAt) {
            t.lostAt = now ? now : 1;
            ++t.losses;
            ++aw.targetsLost;
            t.justLost = true;
        } else if (target && t.lostAt) {
            t.untargetedMs += now - t.lostAt;
            t.lostAt = 0;
            ++aw.targetsRegained;
        }
    }
    if (!t.targetAt) {
        uint32_t target = 0;
        if (Read(t.actor + kNpcTarget, &target, 4) && target) {
            t.targetAt = now;
            const float after = (now - t.watch.bornAt) / 1000.0f;
            ++aw.targets;
            aw.targetSum += after;
            aw.targetMax = std::max(aw.targetMax, after);
            if (Budget(run.spawnLines, cfg.logSpawnLines)) Event("target actor=%08X family=%s after=%.1fs target=%08X", t.actor, t.family.c_str(), after, target);
        }
    }
    if (!t.charged && t.vtable == base + kZombieVtable) {
        uint32_t flags = 0;
        if (Read(t.actor + kCadFlags, &flags, 4) && (flags & 2)) { t.charged = true; ++aw.charging; }
    }
}
void AwareWaveLine() {
    uint32_t count = 0, max = 0;
    Read(base + kChargeCount, &count, 4);
    Read(base + kChargeMax, &max, 4);
    Event("    awareness: forced=%u repaired_templates=%u groups_added=%u ranges_raised=%u no_component=%u comp_mismatch=%u write_fails=%u targets=%u (avg %.1fs max %.1fs) "
          "never_targeted=%u lost=%u regained=%u gone_while_lost=%u charging_seen=%u play_dead_off=%u chargers count=%u max=%u sight_gate=%s alive=%u",
          aw.forced, aw.repaired, aw.groupAdded, aw.rangeRaised, aw.noComponent, aw.mismatch, aw.writeFails, aw.targets, aw.targets ? aw.targetSum / aw.targets : 0.0f,
          aw.targetMax, aw.neverTargeted, aw.targetsLost, aw.targetsRegained, aw.diedUntargeted, aw.charging, aw.playDeadOff, count, max,
          chargeSightSet ? "open" : "stock", AliveCount());
    const unsigned repaired = aw.repaired, rangeRaised = aw.rangeRaised;
    aw = AwareCounts{};
    aw.repaired = repaired;
    aw.rangeRaised = rangeRaised;
}

// Escalation of one enemy, by the wave it was spawned in: health and every limb's
// health times the wave's factor, animation playback (movement follows it)
// through the variant runtime, which folds it into later SetAnimSpeed calls so
// stasis still slows it by the same proportion.
volatile LONG escalatedEnemies = 0, escalatedHealth = 0, escalatedSpeed = 0, escalationFaults = 0;
__declspec(noinline) bool ScaleHealth(uint32_t component, float factor) {
    __try {
        float* max = reinterpret_cast<float*>(component + kHealthCurrent - 4);   // HealthComponent +0xE4 max, +0xE8 current
        float* current = reinterpret_cast<float*>(component + kHealthCurrent);
        if (!(*max > 0.0f) || !(*current > 0.0f) || !std::isfinite(*max) || !std::isfinite(*current)) return false;
        *max *= factor;
        *current *= factor;
        uint32_t vtable = *reinterpret_cast<const uint32_t*>(component);
        if (vtable == base + kDismemberableVtable) {
            float* zones = reinterpret_cast<float*>(component + 0x1F8);   // one health per damage zone (0 head .. 10 torso)
            for (unsigned z = 0; z <= 10; ++z)
                if (std::isfinite(zones[z]) && zones[z] > 0.0f) zones[z] *= factor;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&escalationFaults);
        return false;
    }
}
// Versus: every slasher gets the MP slashers' health, a super one SuperHealth,
// whatever its campaign donor authored.
volatile LONG versusHealthSet = 0, versusHealthFailed = 0;
void VersusHealth(const Tracked& t) {
    const unsigned want = t.super ? cfg.versus.superHealth : cfg.versus.health;
    if (!want) return;
    const uint32_t component = FindHealth(t.actor);
    float max = 0.0f;
    const bool ok = component && Read(component + kHealthCurrent - 4, &max, 4) && max > 0.0f && std::isfinite(max) &&
                    ScaleHealth(component, static_cast<float>(want) / max);
    InterlockedIncrement(ok ? &versusHealthSet : &versusHealthFailed);
    ++(ok ? run.vHealthSet : run.vHealthFailed);
    if (Budget(run.vLines, 120) || !ok)
        Event("versus health actor=%08X %s: %.0f -> %u %s", t.actor, t.super ? "super" : "ordinary", max, want, ok ? "set" : "FAILED (no health component or max <= 0)");
}
void Escalate(Tracked& t) {
    t.escalated = true;
    if (run.versus) { VersusHealth(t); return; }
    const unsigned wave = EscWave(t.wave);
    if (!survival::EscLevel(wave, cfg)) return;
    InterlockedIncrement(&escalatedEnemies);
    const float health = survival::HealthFactor(wave, cfg), speed = survival::SpeedFactor(wave, cfg);
    if (health > 1.0f) {
        const uint32_t component = FindHealth(t.actor);
        if (component && ScaleHealth(component, health)) InterlockedIncrement(&escalatedHealth);
    }
    if (speed > 1.0f && variant_runtime::ApplyEscalationSpeed(t.actor, speed)) InterlockedIncrement(&escalatedSpeed);
}
// The catch-up distance: CatchUpMeters for one player, CatchUpMetersFour with four real
// players (bots do not count), linear in between and beyond, never closer than the
// spawn distances plus 2 m, or the ones brought back would go again.
unsigned catchUpHumans = 1;
float CatchUpDistance() {
    float partners[8 * 3];
    const int partnerCount = partnerPositions && NetRole() == 1 ? partnerPositions(partners, 8) : 0;
    catchUpHumans = 1u + static_cast<unsigned>(partnerCount > 0 ? partnerCount : 0);
    const float step = (cfg.catchUpMeters - cfg.catchUpMetersFour) / 3.0f;
    const float closest = std::max(cfg.ventMaxDistance, cfg.maxDistance) + 2.0f;
    return std::max(closest, cfg.catchUpMeters - step * static_cast<float>(catchUpHumans - 1));
}
// Where the walk from a written-off enemy toward the nearest living player ends, on the
// game's NavPower mesh (DS2United.asi's bot paths). Tells a gap in the mesh from a slow enemy.
using NavPathEndFn = int(__cdecl*)(const float*, const float*, float*);
NavPathEndFn navPathEnd = nullptr;
bool navLooked = false;
volatile LONG navReports = 0, navReached = 0, navShort = 0, navNoPath = 0, navLines = 0;
void NavReport(const Tracked& t, const V3& at, const Viewer* viewers, unsigned viewerCount) {
    if (!navLooked) {
        navLooked = true;
        if (HMODULE coop = ds2coop::PartnerModule("DS2United.asi")) navPathEnd = reinterpret_cast<NavPathEndFn>(GetProcAddress(coop, "DS2Coop_NavPathEnd"));
    }
    if (!navPathEnd) return;
    int nearestViewer = -1;
    float bestD = 1e9f;
    for (unsigned i = 0; i < viewerCount; ++i) {
        if (!viewers[i].alive) continue;
        const float d = survival::Distance(at, viewers[i].feet);
        if (d < bestD) { bestD = d; nearestViewer = static_cast<int>(i); }
    }
    if (nearestViewer < 0) return;
    const V3& f = viewers[nearestViewer].feet;
    const float from[3] = {at.x, at.y, at.z}, to[3] = {f.x, f.y, f.z};
    float end[3]{};
    const int n = navPathEnd(from, to, end);
    InterlockedIncrement(&navReports);
    const bool log = InterlockedIncrement(&navLines) <= 60;
    if (!n) {
        InterlockedIncrement(&navNoPath);
        if (log) Event("nav %s actor=%08X at (%.1f %.1f %.1f): no path at all toward the player at (%.1f %.1f %.1f)", t.family.c_str(), t.actor, at.x, at.y, at.z, f.x, f.y,
                       f.z);
        return;
    }
    const float shortBy = survival::Distance(V3{end[0], end[1], end[2]}, f);
    InterlockedIncrement(shortBy > 2.0f ? &navShort : &navReached);
    if (log)
        Event("nav %s actor=%08X at (%.1f %.1f %.1f): the walk toward the player at (%.1f %.1f %.1f) ends at (%.1f %.1f %.1f), %.1f m short (%d points)%s",
              t.family.c_str(), t.actor, at.x, at.y, at.z, f.x, f.y, f.z, end[0], end[1], end[2], shortBy, n, shortBy > 2.0f ? ": a gap in the mesh" : "");
}
void PollTracked(uint32_t now, const Viewer* viewers, unsigned viewerCount) {
    auto reader = [](uint32_t address, void* output, size_t size) { return Read(address, output, size); };
    unsigned kills = 0;
    const float catchUp = cfg.catchUpMeters > 0.0f ? CatchUpDistance() : 0.0f;
    for (auto& t : tracked) {
        if (!t.actor) continue;
        uint32_t vtable = 0;
        Guid guid{};
        if (!Read(t.actor, &vtable, 4) || vtable != t.vtable || !Read(t.actor + kEntityGuid, guid.data(), 16) || guid != t.guid) {
            // gone from the world: a killed one we saw dying is counted already,
            // anything else went with its stream or was cleaned up
            Retire(t, t.killSent ? t.killFate : survival::Fate::Vanished, now);
            continue;
        }
        bool dead = false, inVent = false;
        unsigned machines = 0;
        ds2_watchdog_runtime::ForEachMachine(t.actor, reader, [&](uint32_t sm) {
            ++machines;
            uint32_t id = 0;
            if (!Read(sm + ds2::watchdog::machine::kTableId, &id, 4)) return;
            if (id == ds2::watchdog::machine::kDeadTable) dead = true;
            if (ds2::watchdog::machine::ventTableName(id)) inVent = true;
        });
        const float health = HealthOf(t.actor);
        if (dead || (health >= 0.0f && health <= 0.0f && machines)) {
            if (run.placeDividersAtBirth && !t.vent && t.family == "DividerCore" && now - t.watch.bornAt < 3000 &&
                Budget(run.dividerPlacementLines, 40)) {
                V3 end{};
                const bool measured = ReadV3(t.actor + kEntityPos, end);
                Event("Divider early death actor=%08X age_ms=%u measured=%d at=(%.1f %.1f %.1f) target_gap_m=%.1f watchdog=%d",
                      t.actor, now - t.watch.bornAt, measured, end.x, end.y, end.z,
                      measured ? survival::Distance(end, t.spawnedAt) : -1.0f, t.killSent);
            }
            Retire(t, t.killSent ? t.killFate : survival::Fate::Killed, now);
            continue;
        }
        if (!t.escalated && now - t.watch.bornAt >= 400) Escalate(t);
        Awareness(t, now);
        V3 pos;
        if (!ReadV3(t.actor + kEntityPos, pos)) continue;
        // Read once per enemy and pass, and only when a check is about to act.
        bool fightRead = false;
        ds2::engaged::Result fightResult;
        auto inFight = [&](const char* check) {
            if (!ds2_watchdog_runtime::spareFights) return false;
            if (!fightRead) {
                fightRead = true;
                auto reader = [](uint32_t address, void* out, size_t size) { return Read(address, out, size); };
                fightResult = ds2::engaged::Check(t.actor, base, ds2_watchdog_runtime::spareMeters, reader);
            }
            if (!fightResult.Engaged()) return false;
            InterlockedIncrement(&sparedFights[static_cast<int>(fightResult.reason)]);
            if (Budget(sparedLines, 60))
                Event("enemy %08X (%s) spared by the %s check: in a fight (%s, nearest player %.1f m, coordinator %08X)", t.actor, t.family.c_str(), check,
                      ds2::engaged::ReasonName(fightResult.reason), fightResult.meters < 1e8f ? fightResult.meters : -1.0f, fightResult.coordinator);
            return true;
        };
        if (t.vent && t.marker >= 0 && !t.arrivalChecked && now - t.watch.bornAt >= 3000) {
            t.arrivalChecked = true;
            const auto& chosen = ds2::kSurvivalVentMarkers[t.marker];
            const float toChosen = survival::Distance(pos, V3{chosen.x, chosen.y, chosen.z});
            float toOwn = -1.0f;
            if (t.ownMarker >= 0) {
                const auto& own = ds2::kSurvivalVentMarkers[t.ownMarker];
                toOwn = survival::Distance(pos, V3{own.x, own.y, own.z});
            }
            const bool atChosen = toChosen <= 6.0f, atOwn = !atChosen && toOwn >= 0.0f && toOwn <= 6.0f;
            InterlockedIncrement(atChosen ? &arrivalsAtChosen : atOwn ? &arrivalsAtOwn : &arrivalsElsewhere);
            if (arrivalLines < 40) {
                ++arrivalLines;
                Event("vent arrival %s actor=%08X at (%.1f %.1f %.1f): %.1f m from the chosen vent %08X-%08X, %.1f m from its spawner's own -> %s", t.family.c_str(),
                      t.actor, pos.x, pos.y, pos.z, toChosen, chosen.vent[0], chosen.vent[1], toOwn,
                      atChosen ? "came out of the chosen vent" : atOwn ? "came out of the OWN vent (redirect did not take)" : "somewhere else");
            }
        }
        // A direct one is put on its spot right after the spawn, but the engine's own
        // arrival can put it back on the spawner a frame later (ch04 arena, 09.10.: an
        // elevator's null spawner left four of six zombies in the elevator, 35-49 m off).
        // Checked four times in the first two seconds and placed again when 5 m off; an
        // engine that does it twice is not picked again this run. In an arena only an
        // enemy outside it counts: inside, an engine that leaves its enemies on its own
        // spot is still a good engine (10.10.: counting those retired every engine in
        // the room but the gate keeper's, which refused everything).
        if (!t.vent && t.placeChecks < 4 && now - t.watch.bornAt >= 250u + 450u * t.placeChecks) {
            ++t.placeChecks;
            const float off = survival::Distance(pos, t.spawnedAt);
            const bool outside = InArena() && (!arenaInside || arenaInside(&pos.x) != 1);
            // Farther than a run could carry it: a slasher covers 8 m in 1.6 s, and
            // counting that as the engine's doing put runners back and retired their
            // engines (19.10. mp_escape: 17 retired, the waves starved).
            const float reach = std::max(5.0f, 2.0f + 7.0f * static_cast<float>(now - t.watch.bornAt) / 1000.0f);
            if (off > 5.0f && off <= reach) InterlockedIncrement(&walkedNotPlaced);
            if (off > reach && !inFight("placement")) {
                ds2::ExtraSpawnRule rule;
                rule.kind = t.family;
                rule.arrival = ds2::ExtraArrival::Direct;
                rule.hasPosition = true;
                rule.position[0] = t.spawnedAt.x;
                rule.position[1] = t.spawnedAt.y + 0.1f;
                rule.position[2] = t.spawnedAt.z;
                const bool ok = ApplyPlacement(t.actor, rule);
                ++t.replaced;
                InterlockedIncrement(&replacedSpawns);
                const bool counts = !InArena() || outside;
                const unsigned misplaced = t.engine && counts ? ++engineMisplaced[t.engine] : 0;
                const bool avoid = misplaced == 2;
                if (avoid) avoidedEngines.push_back(t.engine);
                if (Budget(run.spawnLines, cfg.logSpawnLines))
                    Event("enemy %08X stood %.1f m from its spot %lu ms after the spawn, at (%.1f %.1f %.1f)%s: placed again %s (engine %08X%s)", t.actor, off,
                          static_cast<unsigned long>(now - t.watch.bornAt), pos.x, pos.y, pos.z, !InArena() ? "" : outside ? " OUTSIDE the arena" : " inside the arena",
                          ok ? "ok" : "FAILED", static_cast<unsigned>(t.engine), avoid ? ", which did it twice: not used again this run" : "");
                const V3 stoodAt = pos;
                if (ok) pos = t.spawnedAt;
                // In an arena an enemy still outside it at the last check stands behind a
                // locked door: it is killed out of sight and its item queued again. On an MP
                // map an engine that keeps putting its enemy back on its own spot can hold it
                // where nobody gets to it (15.10. Concourse: an Exploder four times back at
                // (-5.5 -3.5 32), the wave's last for 54 s until the stuck watchdog): with no
                // living player within 12 m it goes the same way.
                float nearestHere = 1e9f;
                for (unsigned i = 0; i < viewerCount; ++i)
                    if (viewers[i].alive) nearestHere = std::min(nearestHere, survival::Distance(stoodAt, viewers[i].feet));
                const bool heldAway = !InArena() && nearestHere > 12.0f;
                if ((outside || heldAway) && t.placeChecks == 4 && t.replaced >= 2 && !t.unkillable && !t.killSent && killReady) {
                    survival::Item again;
                    again.family = again.asked = t.family;
                    // Through a vent when it has one: the next direct spawn met the same fate
                    // six times in a row (19.10. mp_escape, a Puker the placement never moved).
                    const bool byVent = ItemHasVent(again) && VentPathOn();
                    again.path = byVent ? survival::Path::Vent : survival::Path::Direct;
                    if (byVent) InterlockedIncrement(&requeuedToVent);
                    run.queue.push_back(again);
                    ++run.total;
                    ++run.requeued;
                    KillActor(t, outside ? "outside the arena after four placements, queued again"
                                         : "held on its engine's spot after four placements with nobody near, queued again",
                              survival::Fate::WrittenOff, now);
                }
            }
        }
        if (!t.sawFirstPos && now - t.watch.bornAt >= 3000) {
            t.sawFirstPos = true;
            if (Budget(run.spawnLines, cfg.logSpawnLines))
                Event("enemy %08X (%s, %s) 3 s after its spawn at (%.1f %.1f %.1f), %.1f m from where it was asked, machines=%u in_vent=%d health=%.0f", t.actor,
                      t.family.c_str(), t.vent ? "vent" : "direct", pos.x, pos.y, pos.z, survival::Distance(pos, t.spawnedAt), machines, inVent ? 1 : 0, health);
        }
        float nearest = 1e9f, nearestLiving = 1e9f;
        for (unsigned i = 0; i < viewerCount; ++i) {
            const float d = survival::Distance(pos, viewers[i].feet);
            nearest = std::min(nearest, d);
            if (viewers[i].alive) nearestLiving = std::min(nearestLiving, d);
        }
        if (t.justLost) {
            t.justLost = false;
            if (Budget(run.spawnLines, cfg.logSpawnLines))
                Event("target lost actor=%08X family=%s age=%lus nearest player %.1f m (loss %u)", t.actor, t.family.c_str(),
                      static_cast<unsigned long>((now - t.watch.bornAt) / 1000), nearest < 1e8f ? nearest : -1.0f, t.losses);
        }
        if (t.unkillable) continue;   // the Ubermorph roams; it never holds a wave up
        if (t.killSent) {
            if (now - t.killAt > 6000) Retire(t, survival::Fate::WrittenOff, now);   // the kill did nothing
            continue;
        }
        // Frozen (16.10., Marker Test-Bed: a slasher stood in a wall by the lockers for 65 s, a
        // player within the engage range kept its progress fresh): not a metre moved for
        // FrozenSeconds, it is taken away and its item queued again.
        survival::NoteMoved(t.watch, pos.x, pos.y, pos.z, now);
        if (survival::Frozen(t.watch, now, inVent, cfg) && !survival::HoldsStill(t.family.c_str()) && !run.versus && run.state == State::Wave &&
            killReady && kills < 3 && !inFight("frozen")) {
            ++kills;
            survival::Item again;
            again.family = again.asked = t.family;
            again.path = survival::Path::Any;
            run.queue.push_back(again);
            ++run.total;
            InterlockedIncrement(&frozenKills);
            char why[160];
            std::snprintf(why, sizeof(why), "frozen: within 1 m of (%.1f %.1f %.1f) for %lus, %.0f m from the nearest player, engaged=%d, queued again",
                          t.watch.stillAt[0], t.watch.stillAt[1], t.watch.stillAt[2], static_cast<unsigned long>((now - t.watch.movedAt) / 1000),
                          nearestLiving, t.watch.engaged ? 1 : 0);
            NoteVentFailure(t);
            NavReport(t, pos, viewers, viewerCount);
            KillActor(t, why, survival::Fate::WrittenOff, now);
            if (!killReady) Retire(t, survival::Fate::WrittenOff, now);
            continue;
        }
        // Catch-up (13.10., as in CoD zombies): one left farther than the catch-up distance behind
        // every living player (for CatchUpSeconds, 0 = at once) is taken away and its item queued
        // again, so it comes back near them through a vent or a direct spawn. Not in Versus, not
        // for Brutes (the Ubermorph is unkillable above), not inside a vent, not in its first 4 s
        // (a fresh one can still stand at its spawner for two).
        if (catchUp > 0.0f && !run.versus && run.state == State::Wave && killReady && nearestLiving < 1e8f && !inVent &&
            _stricmp(t.family.c_str(), "Brute") != 0 && now - t.watch.bornAt > 4000) {
            if (nearestLiving <= catchUp) {
                t.farSince = 0;
            } else if (!t.farSince && cfg.catchUpMs) {
                t.farSince = now ? now : 1;
            } else if ((!cfg.catchUpMs || now - t.farSince >= cfg.catchUpMs) && kills < 3 && !inFight("catch-up")) {
                ++kills;
                survival::Item again;
                again.family = again.asked = t.family;
                again.path = survival::Path::Any;
                run.queue.push_back(again);
                ++run.total;
                ++run.caughtUp;
                char why[128];
                std::snprintf(why, sizeof(why), "caught up: %.0f m from the nearest player (limit %.0f m, %u real player(s)), queued again",
                              nearestLiving, catchUp, catchUpHumans);
                KillActor(t, why, survival::Fate::WrittenOff, now);
                if (!killReady) Retire(t, survival::Fate::WrittenOff, now);
                continue;
            }
        }
        const survival::Fate fate = survival::Observe(t.watch, now, nearest, inVent, machines, cfg);
        if (fate == survival::Fate::Inert) { Retire(t, fate, now); continue; }
        if ((fate == survival::Fate::Stuck || fate == survival::Fate::VentStuck) && kills < 3 && !inFight(fate == survival::Fate::Stuck ? "stuck" : "vent_stuck")) {
            ++kills;
            NoteVentFailure(t);
            if (fate == survival::Fate::Stuck) NavReport(t, pos, viewers, viewerCount);
            KillActor(t, fate == survival::Fate::Stuck ? "stuck (no progress toward any player)" : "vent_stuck", fate, now);
            if (!killReady) Retire(t, survival::Fate::WrittenOff, now);
        }
    }
    tracked.erase(std::remove_if(tracked.begin(), tracked.end(), [](const Tracked& t) { return !t.actor; }), tracked.end());
    InterlockedExchange(&aliveMirror, static_cast<LONG>(AliveCount()));
}

// --- rewards ---
struct GunAmmo { uint32_t vtableRva; const char* gun; uint32_t ammo[4]; };
// Gun vtables (co-op bot gun catalogue) and the global_assets SpareAmmo of each.
constexpr GunAmmo kGunAmmo[] = {
    {0x17515D8, "PlasmaCutter", {0x4BC78C36, 0x9D651322, 0x544E4543, 0x53574F4B}},
    {0x1751A00, "PulseRifle", {0x4BC78C36, 0x9D67757C, 0x544E4543, 0x53574F4B}},
    {0x17511B8, "LineGun", {0x4BC78CEC, 0xDFF02CCF, 0x544E4543, 0x53574F4B}},
    {0x1750D98, "Javelin", {0x35C97DB8, 0x53ABA0A9, 0x44455257, 0x38343145}},
    {0x1752580, "Ripper", {0x30C8692D, 0x79E101DC, 0x4E484F4A, 0x324E4F53}},
    {0x1753100, "Detonator", {0x58CA30C7, 0xA9ADE7CB, 0x3045454C, 0x58373634}},
    {0x1753520, "Flamethrower", {0x30C8692D, 0x6DBBF822, 0x4E484F4A, 0x324E4F53}},
    {0x1773780, "SeekerRifle", {0x30C889E7, 0x7019FF4E, 0x4E484F4A, 0x324E4F53}},
    {0x1753948, "ForceGun", {0x30C87D90, 0xE7570C3E, 0x4C454D50, 0x4B534544}},
    {0x1752CE0, "ContactBeam", {0x30C87377, 0xE3BC55EA, 0x4C454D50, 0x4B534544}},
};
constexpr uint32_t kMedPacks[3][4] = {{0x4BC78C36, 0x9D5B89BA, 0x544E4543, 0x53574F4B},
                                      {0x4BC78C36, 0x9D5DEC14, 0x544E4543, 0x53574F4B},
                                      {0x4BC78C36, 0x9D604E6E, 0x544E4543, 0x53574F4B}};
constexpr uint32_t kStasisPack[4] = {0x4BC806EA, 0x40D9D6F7, 0x544E4543, 0x53574F4B};
constexpr uint32_t kCurrency[4][4] = {{0x36C836AD, 0x038D34DA, 0x44455257, 0x34303245},    // 10000
                                      {0x36C836AC, 0xFE409ACF, 0x44455257, 0x34303245},    // 5000
                                      {0x36C836AC, 0xF8A08E76, 0x44455257, 0x34303245},    // 3000
                                      {0x36C836AC, 0xE6FCE89A, 0x44455257, 0x34303245}};   // 1000
unsigned dropIndex = 0;
bool DropOne(const uint32_t* guid, const V3& center, const char* what) {
    const uint32_t item = SpawnPickupRaw(guid);
    uint32_t vtable = 0, slot = 0, owner = 1;
    if (!item) { Event("reward %s: GetGlobalPickupItem gave nothing for %08X-%08X", what, guid[0], guid[1]); return false; }
    if (!Read(item, &vtable, 4) || vtable < base + kRdataLo || vtable >= base + kRdataHi || !Read(vtable + kItemDropSlot, &slot, 4) ||
        slot != base + kItemDrop || !Read(item + kItemOwner, &owner, 4) || owner) {
        Event("reward %s: %08X is no droppable pickup (vtable %08X, drop slot %08X, owner %08X); deleted", what, item, vtable, slot, owner);
        if (Have(sDelete)) DeleteRaw(item);
        return false;
    }
    const float a = static_cast<float>(dropIndex++) * 0.9f;
    alignas(16) float pos[4] = {center.x + std::sin(a) * 0.9f, center.y + 0.3f, center.z + std::cos(a) * 0.9f, 1.0f};
    if (!DropRaw(item, pos)) {
        Event("reward %s: Item::Drop faulted at %08X; deleted", what, lastFaultAddress);
        if (Have(sDelete)) DeleteRaw(item);
        return false;
    }
    return true;
}
// The run's wave, or on a client the host's (it drops its own share of it).
unsigned RewardWave() {
    if (run.state != State::Off && run.wave) return EscWave(run.wave);
    const int host = lastRewardWave ? lastRewardWave() : 0;
    return host > 0 ? static_cast<unsigned>(host) : 0;
}
void DropRewards(const Viewer* viewers, unsigned viewerCount, unsigned wave) {
    const unsigned rewardPercent = survival::RewardPercent(wave, cfg);
    if (!rewardsReady || !viewerCount) {
        Event("rewards skipped: %s", !rewardsReady ? "a pickup function signature did not match" : "no player");
        return;
    }
    // At the player, not at the nearest spawn point: that one could be 40 m away
    // in another room (09.10.: "half of them missing or somewhere else").
    const Viewer& p = viewers[0];
    V3 center = p.feet;
    const char* where = "at the player's feet";
    const V3 ahead = {p.feet.x + p.forward.x * 1.5f, p.feet.y, p.feet.z + p.forward.z * 1.5f};
    const V3 chest = {p.feet.x, p.feet.y + 1.0f, p.feet.z}, aheadChest = {ahead.x, ahead.y + 1.0f, ahead.z};
    const V3 aheadLow = {ahead.x, ahead.y + 0.3f, ahead.z};
    if (Clear(chest, aheadChest, p.entity) && Clear(aheadChest, aheadLow, p.entity)) { center = ahead; where = "1.5 m in front of the player"; }
    unsigned ammo = 0, guns = 0, meds = 0, credits = 0, stasis = 0, dropFailures = 0;
    std::string gunNames;
    if (cfg.rewardAmmoPerGun) {
        uint32_t entries = 0, size = 0;
        if (Read(p.entity + kWeaponInventory + kInventoryEntries, &entries, 4) && entries && Read(p.entity + kWeaponInventory + kInventorySize, &size, 4) && size <= 16)
            for (uint32_t i = 0; i < size; ++i) {
                uint32_t ref = 0, vtable = 0;
                if (!Read(entries + i * kInventoryEntry + 4, &ref, 4) || ref < 0x10010 || !Read(ref - 0x10, &vtable, 4)) continue;
                for (const auto& g : kGunAmmo) {
                    if (vtable != base + g.vtableRva) continue;
                    ++guns;
                    gunNames += (gunNames.empty() ? "" : ",") + std::string(g.gun);
                    for (unsigned k = 0; k < survival::Scaled(cfg.rewardAmmoPerGun, rewardPercent); ++k) (DropOne(g.ammo, center, g.gun) ? ammo : dropFailures)++;
                }
            }
        if (!guns)
            for (unsigned k = 0; k < survival::Scaled(cfg.rewardAmmoPerGun, rewardPercent); ++k) (DropOne(kGunAmmo[0].ammo, center, "PlasmaCutter (no gun readable)") ? ammo : dropFailures)++;
    }
    for (unsigned k = 0; k < survival::Scaled(cfg.rewardMedPacks, rewardPercent); ++k) (DropOne(kMedPacks[cfg.rewardMedSize > 2 ? 2 : cfg.rewardMedSize], center, "med pack") ? meds : dropFailures)++;
    for (unsigned k = 0; k < survival::Scaled(cfg.rewardStasis, rewardPercent); ++k) (DropOne(kStasisPack, center, "stasis pack") ? stasis : dropFailures)++;
    const auto split = survival::SplitCredits(cfg.rewardCredits * rewardPercent / 100);
    static const unsigned kValues[] = {10000, 5000, 3000, 1000};
    for (size_t d = 0; d < split.size() && d < 4; ++d)
        for (unsigned k = 0; k < split[d]; ++k) {
            if (DropOne(kCurrency[d], center, "credits")) credits += kValues[d];
            else ++dropFailures;
        }
    Event("rewards at %s (%.1f %.1f %.1f): ammo=%u for %u gun(s) [%s] med_packs=%u stasis=%u credits=%u failed=%u (rewards at %u%%, wave %u)", where, center.x,
          center.y, center.z, ammo, guns, gunNames.empty() ? "none read" : gunNames.c_str(), meds, stasis, credits, dropFailures, rewardPercent, wave);
}
// A wave's rewards for a local Isaac who is down at the wave end (DS2Coop
// [Respawn]: he comes back a moment later at a spawn point) wait for the new body.
survival::RewardQueue rewardsOwed;
uint32_t rewardsLocal = 0;   // the local Isaac (PlayerManager slot 0) of this tick
void GiveRewards(const Viewer* viewers, unsigned viewerCount, unsigned wave) {
    if (!viewerCount || !viewers[0].alive || viewers[0].entity != rewardsLocal) {
        if (rewardsOwed.Push(wave)) Event("rewards held for wave %u until the local player returns (%zu waiting)", wave, rewardsOwed.count);
        else Event("rewards queue full: cannot hold wave %u", wave);
        return;
    }
    DropRewards(viewers, viewerCount, wave);
}

// Rewards for everybody: this machine drops its own, and the reward event makes each
// partner drop his share at his own Isaac (as at a survival wave end).
void ShareRewards(const Viewer* viewers, unsigned viewerCount, const char* why) {
    GiveRewards(viewers, viewerCount, RewardWave());
    // partners scale their share by this wave, so a clock run sends its escalation wave
    if (NetRole() != 2 && survivalEvent) survivalEvent(2, static_cast<int>(EscWave(run.wave)));
    Event("rewards shared (%s): local drop, reward event to the partners (wave %u, escalation wave %u)", why, run.wave, EscWave(run.wave));
}

// --- Mercenaries ---
// The clock mode (DS2Coop_GameMode 1, [Mercenaries] in DS2Survival.ini). The spawning core
// stays: the next stage's enemies join the queue whenever it runs dry, so there is no break,
// and the escalation follows the stage. Kills, combos and time capsules add time, a death
// takes some off; at zero the run is over. The clock runs on game time (SimMs), so the
// pause menu stops it.
volatile LONG mercsRuns = 0, mercsKills = 0, mercsComboBonuses = 0, mercsDeaths = 0;
int GameModeNow() {
    NetRole();
    return gameMode ? gameMode() : 0;
}
LONG HumanDeathsNow() {
    NetRole();
    return humanDeaths ? static_cast<LONG>(humanDeaths()) : -1;
}
void MercsFlash(const char* format, ...) {
    char text[96];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    Notice(text);
}
bool EliteKill(const Tracked& t) {
    const survival::Role role = survival::RoleOf(t.family);
    return role == survival::Role::Heavy || role == survival::Role::Boss || superGuids.count(t.donor) != 0;
}
void MercsKill(const Tracked& t, uint32_t now) {
    const auto& k = cfg.mercs;
    bool comboBonus = false;
    const bool elite = EliteKill(t);
    const uint32_t sim = SimMs();
    const unsigned full = run.ms.Kill(t.family, elite, sim, k.killMs, k.eliteKillMs, k.comboWindowMs, k.comboStep, k.comboBonusMs, comboBonus);
    const unsigned bonus = survival::FadedBonusMs(full, sim - run.clockStartSim, k.fadeS, k.fadeFloorPercent);
    run.clockMs = survival::MercsClock(run.clockMs, bonus, static_cast<int64_t>(k.maxSeconds) * 1000);
    InterlockedIncrement(&mercsKills);
    if (comboBonus) {
        InterlockedIncrement(&mercsComboBonuses);
        MercsFlash("COMBO %u  +%u S", run.ms.combo, survival::FadedBonusMs(k.comboBonusMs, sim - run.clockStartSim, k.fadeS, k.fadeFloorPercent) / 1000);
    }
    if (Budget(run.mercsLines, 200))
        Event("mercenaries kill %u: %s%s +%u ms, combo %u, score %u, clock %.1f s", run.ms.kills, t.family.c_str(), elite ? " (elite)" : "", bonus, run.ms.combo,
              run.ms.score, static_cast<double>(run.clockMs) / 1000.0);
    (void)now;
}

// One pickup dropped exactly at `at`; 0 when it did not take.
uint32_t DropAt(const uint32_t* guid, const V3& at, const char* what) {
    const uint32_t item = SpawnPickupRaw(guid);
    uint32_t vtable = 0, slot = 0, owner = 1;
    if (!item) { Event("mercenaries %s: GetGlobalPickupItem gave nothing for %08X-%08X", what, guid[0], guid[1]); return 0; }
    if (!Read(item, &vtable, 4) || vtable < base + kRdataLo || vtable >= base + kRdataHi || !Read(vtable + kItemDropSlot, &slot, 4) ||
        slot != base + kItemDrop || !Read(item + kItemOwner, &owner, 4) || owner) {
        Event("mercenaries %s: %08X is no droppable pickup; deleted", what, item);
        if (Have(sDelete)) DeleteRaw(item);
        return 0;
    }
    alignas(16) float pos[4] = {at.x, at.y + 0.3f, at.z, 1.0f};
    if (!DropRaw(item, pos)) {
        if (Have(sDelete)) DeleteRaw(item);
        return 0;
    }
    return item;
}
// Normal capsules are stasis packs (stasis slows time, and they glow blue); golden ones a credit chip.
const uint32_t* CapsuleGuid(bool golden) { return golden ? kCurrency[1] : kStasisPack; }
// A capsule near an open vent of the map: the map opens its vents with the areas
// around them, so nothing lands behind a door it has not opened. On the nav mesh,
// reachable from the nearest living player, away from players and other capsules.
// Recorded capsule spots (DS2Capsules.txt: the zone keys in a Mercenaries run).
// With any for the map the capsules go there, a random free one each time.
std::vector<survival::ZoneSpot> capsuleSpots;
std::string capsuleSpotsPath;
// A campaign arena's own points for a mode (DS2Coop_ArenaPoints: 0 zones, 1 capsules),
// for when nothing was recorded under its name.
std::vector<survival::ZoneSpot> ArenaModePoints(int kind) {
    std::vector<survival::ZoneSpot> out;
    if (!InArena() || !arenaPoints) return out;
    float points[16][4]{};
    const int n = arenaPoints(kind, &points[0][0], 16);
    for (int i = 0; i < n && i < 16; ++i) {
        survival::ZoneSpot s;
        s.pos = V3{points[i][0], points[i][1], points[i][2]};
        s.radius = points[i][3];
        out.push_back(s);
    }
    return out;
}
void LoadCapsuleSpotsFile();
void LoadCapsuleSpots() {
    LoadCapsuleSpotsFile();
    if (!capsuleSpots.empty() || !InArena()) return;
    capsuleSpots = ArenaModePoints(1);
    Event("capsule spots: %zu built in for campaign arena %s%s", capsuleSpots.size(), run.map.c_str(),
          arenaPoints ? "" : " (DS2Coop_ArenaPoints missing: around a player, inside the arena)");
}
void LoadCapsuleSpotsFile() {
    capsuleSpots.clear();
    std::ifstream file(capsuleSpotsPath, std::ios::binary);
    if (!file) { Event("capsule spots: %s not found, the capsules go by the open vents", capsuleSpotsPath.c_str()); return; }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    unsigned skipped = 0, other = 0;
    capsuleSpots = survival::ParseZoneSpots(text, run.map, skipped, other);
    Event("capsule spots: %zu for %s in %s (%u for other maps, %u lines not understood)", capsuleSpots.size(), run.map.c_str(), capsuleSpotsPath.c_str(),
          other, skipped);
}
bool PlaceCapsule(uint32_t now, const Viewer* viewers, unsigned viewerCount) {
    const auto& k = cfg.mercs;
    ++run.capsuleTries;
    std::vector<const Viewer*> living;
    for (unsigned i = 0; i < viewerCount; ++i) if (viewers[i].alive) living.push_back(&viewers[i]);
    if (living.empty()) return false;
    std::vector<size_t> open;
    for (size_t m = 0; m < std::size(ds2::kSurvivalVentMarkers); ++m)
        if (survival::EqualNoCase(ds2::kSurvivalVentMarkers[m].map, run.map.c_str()) && VentAvailable(ds2::kSurvivalVentMarkers[m])) open.push_back(m);
    const float angle = rng.Unit() * 6.2831853f;
    V3 seed;
    bool recorded = false;
    // Across the map, away from the team (13.10.: they all came up a few metres off):
    // of the recorded spots, else the open vents, the farther half from the nearest
    // player is drawn from, never closer than CapsuleMinMeters while any is that far.
    auto nearestPlayer = [&](const V3& p) {
        float nearest = 1e9f;
        for (const Viewer* v : living) nearest = std::min(nearest, survival::Distance(p, v->feet));
        return nearest;
    };
    std::vector<std::pair<float, V3>> spots;
    if (!capsuleSpots.empty()) {
        for (const auto& s : capsuleSpots) spots.push_back({nearestPlayer(s.pos), s.pos});
        recorded = true;
    } else {
        for (size_t m : open) {
            const auto& marker = ds2::kSurvivalVentMarkers[m];
            const float r = 2.0f + rng.Unit() * 4.0f;
            const V3 p{marker.x + std::cos(angle) * r, marker.y, marker.z + std::sin(angle) * r};
            spots.push_back({nearestPlayer(p), p});
        }
    }
    std::vector<std::pair<float, V3>> free;
    for (const auto& s : spots) {
        bool ok = s.first >= 6.0f;
        for (const auto& c : capsules) ok = ok && survival::Distance(s.second, c.pos) >= static_cast<float>(k.capsuleGapM);
        if (ok) free.push_back(s);
    }
    if (!free.empty()) {
        std::sort(free.begin(), free.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        size_t farCount = 0;
        while (farCount < free.size() && free[farCount].first >= static_cast<float>(k.capsuleMinM)) ++farCount;
        const size_t drawFrom = farCount ? farCount : (free.size() + 1) / 2;
        seed = free[rng.Next() % drawFrom].second;
    } else if (recorded) {
        return false;   // every recorded spot is taken or next to a player: later
    } else {
        const Viewer& v = *living[rng.Next() % living.size()];
        const float r = static_cast<float>(k.capsuleMinM) + 5.0f + rng.Unit() * 20.0f;
        seed = {v.feet.x + std::cos(angle) * r, v.feet.y, v.feet.z + std::sin(angle) * r};
    }
    const Viewer* from = living[0];
    for (const Viewer* v : living)
        if (survival::Distance(v->feet, seed) < survival::Distance(from->feet, seed)) from = v;
    V3 end;
    float length = 0.0f;
    if (!NavWalk(from->feet, seed, end, length) || survival::Distance(end, seed) > 6.0f || length > 300.0f) return false;
    // the nav mesh runs on through the arena's locked doors
    if (InArena() && (!arenaInside || arenaInside(&end.x) != 1)) return false;
    for (const Viewer* v : living) if (survival::Distance(end, v->feet) < 6.0f) return false;
    for (const auto& c : capsules) if (survival::Distance(end, c.pos) < static_cast<float>(k.capsuleGapM)) return false;
    if (!Clear({end.x, end.y + 0.3f, end.z}, {end.x, end.y + 2.0f, end.z}, 0)) return false;
    const bool golden = rng.Unit() * 100.0f < static_cast<float>(k.goldenPercent);
    const uint32_t item = DropAt(CapsuleGuid(golden), end, golden ? "golden capsule" : "capsule");
    if (!item) return false;
    Capsule c;
    c.item = item;
    Read(item, &c.vtable, 4);
    Read(item + kEntityGuid, c.guid.data(), 16);
    c.pos = end;
    c.golden = golden;
    c.bornAt = now;
    c.id = ++capsuleIdNext;
    capsules.push_back(c);
    AnnounceCapsule(5, c);
    ++run.capsulesPlaced;
    Event("mercenaries %s %u of %u placed at (%.1f %.1f %.1f) item %08X, %.0f m path and %.0f m straight from the nearest player, %s, %zu on the map",
          golden ? "golden capsule" : "capsule", run.capsulesPlaced, k.capsuleTotal, end.x, end.y, end.z, item, length, nearestPlayer(end),
          recorded ? "at a recorded spot" : open.empty() ? "around a player (no open vent)" : "by an open vent", capsules.size());
    if (k.capsuleTotal && run.capsulesPlaced == k.capsuleTotal) MercsFlash("LAST TIME CAPSULE");
    return true;
}
// Taken: the pickup has an owner now, or it is gone and a player was right there a moment ago.
constexpr float kPartnerTakeMeters = 2.0f;
void WatchCapsules(uint32_t now, const Viewer* viewers, unsigned viewerCount) {
    const auto& k = cfg.mercs;
    mercsNearestCapsule = -1.0f;
    float partners[8 * 3];
    const int partnerCount = partnerPositions && NetRole() == 1 ? partnerPositions(partners, 8) : 0;
    for (size_t i = 0; i < capsules.size();) {
        Capsule& c = capsules[i];
        float nearest = 1e9f, nearestPartner = 1e9f;
        for (unsigned v = 0; v < viewerCount; ++v)
            if (viewers[v].alive) nearest = std::min(nearest, survival::Distance(c.pos, viewers[v].feet));
        for (int q = 0; q < partnerCount; ++q)
            nearestPartner = std::min(nearestPartner, survival::Distance(c.pos, V3{partners[q * 3], partners[q * 3 + 1], partners[q * 3 + 2]}));
        nearest = std::min(nearest, nearestPartner);
        uint32_t owner = 0;
        const bool there = SameEntity(c.item, c.vtable, c.guid);
        if (there && !Read(c.item + kItemOwner, &owner, 4)) { ++i; continue; }
        const bool owned = there && owner != 0;
        // a partner picks up his own copy: standing on ours is the sign
        const bool partnerTook = there && !owned && nearestPartner <= kPartnerTakeMeters;
        if (partnerTook && Have(sDelete)) DeleteRaw(c.item);
        if (owned || partnerTook || (!there && c.nearSeen)) {
            const unsigned add = survival::FadedBonusMs(c.golden ? k.goldenMs : k.capsuleMs, SimMs() - run.clockStartSim, k.fadeS, k.fadeFloorPercent);
            run.clockMs = survival::MercsClock(run.clockMs, add, static_cast<int64_t>(k.maxSeconds) * 1000);
            ++run.ms.capsules;
            InterlockedIncrement(&capsulesTaken);
            MercsFlash("%sCAPSULE  +%u S", c.golden ? "GOLDEN " : "", add / 1000);
            Event("mercenaries %s taken (%s, nearest player %.1f m): +%u ms, clock %.1f s", c.golden ? "golden capsule" : "capsule",
                  owned ? "picked up" : partnerTook ? "a partner stood on it" : "gone next to a player", nearest, add, static_cast<double>(run.clockMs) / 1000.0);
            AnnounceCapsule(6, c);
            capsules.erase(capsules.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (!there) {
            ++run.capsulesLost;
            Event("mercenaries capsule %08X gone with nobody near (nearest %.1f m): lost, another is placed", c.item, nearest);
            AnnounceCapsule(6, c);
            capsules.erase(capsules.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (now - c.bornAt > k.capsuleLifeS * 1000u) {
            ++run.capsulesExpired;
            if (Have(sDelete)) DeleteRaw(c.item);
            Event("mercenaries capsule %08X not taken in %u s: moved", c.item, k.capsuleLifeS);
            AnnounceCapsule(6, c);
            capsules.erase(capsules.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        c.nearSeen = nearest < 6.0f;
        if (mercsNearestCapsule < 0.0f || nearest < mercsNearestCapsule) mercsNearestCapsule = nearest;
        ++i;
    }
}
// The clock modes share the stage queue; their log lines name the mode that runs.
const char* ClockModeName() { return run.objective ? "objective" : run.zone ? "zone" : "mercenaries"; }
// The next stage joins the queue: its enemies, its cap and pacing, its escalation.
void AppendStage(unsigned n, uint32_t now) {
    survival::Wave spec = survival::WaveFor(n, cfg);
    spec.timeLimitS = 0;   // a boss wave's timer means nothing on the clock
    std::map<std::string, std::string> subs;
    unsigned dropped = 0;
    auto more = survival::ResolveQueue(survival::BuildQueue(spec), run.available, dropped, &subs);
    ArenaBrutes(more);
    if (more.empty()) {
        // nothing of this stage can spawn here: try again later instead of racing through the stages
        run.stageRetryAt = survival::DeadlineAfter(now, 5000);
        if (Budget(run.mercsLines, 200)) Event("%s stage %u: no family of it is available on this map (%u dropped); retry in 5 s", ClockModeName(), n, dropped);
        return;
    }
    run.stageRetryAt = 0;
    run.wave = n;
    run.spec = spec;
    run.queue.insert(run.queue.end(), more.begin(), more.end());
    for (const auto& item : more) if (!survival::Unkillable(item.family)) ++run.total;
    InterlockedExchange(&waveMirror, static_cast<LONG>(n));
    Event("%s stage %u joins: %zu more enemies (queue %zu, next %zu, alive %u, clock %.1f s, cap %u) escalation wave %u enhanced %u%% health x%.2f",
          ClockModeName(), n, more.size(), run.queue.size(), run.next, AliveCount(), static_cast<double>(run.clockMs) / 1000.0, WaveCap(),
          EscWave(n), survival::ClockEnhancedChance(n, spec, cfg), survival::HealthFactor(EscWave(n), cfg));
    (void)now;
}
// The last FinalSeconds: FinalEnemies once, ahead of everything queued.
void InjectFinal(uint32_t now) {
    run.finalDone = true;
    survival::Wave w;
    w.entries = cfg.mercs.finalEnemies;
    std::map<std::string, std::string> subs;
    unsigned dropped = 0;
    auto items = survival::ResolveQueue(survival::BuildQueue(w), run.available, dropped, &subs);
    ArenaBrutes(items);
    run.queue.insert(run.queue.begin() + static_cast<std::ptrdiff_t>(run.next), items.begin(), items.end());
    for (const auto& item : items) if (!survival::Unkillable(item.family)) ++run.total;
    MercsFlash("FINAL %u SECONDS - ELITES INBOUND", static_cast<unsigned>((run.clockMs + 999) / 1000));
    Event("mercenaries final enemies: %zu queued first (%u dropped)", items.size(), dropped);
    (void)now;
}
void MercsBegin(uint32_t now) {
    LoadCapsuleSpots();
    run.clockMs = static_cast<int64_t>(cfg.mercs.startSeconds) * 1000;
    run.clockSim = SimMs();
    run.deathsSeen = HumanDeathsNow();
    run.ms = survival::MercsScore{};
    run.clockStartSim = run.clockSim;
    InterlockedIncrement(&mercsRuns);
    Event("mercenaries run %u starts on %s: %u s on the clock, kills +%u ms (elite +%u), combo window %u ms step %u bonus %u ms, capsules %u (+%u ms, golden "
          "+%u ms at %u%%), death -%u ms, kill target %u, sim %u, deaths so far %ld",
          run.number, run.map.c_str(), cfg.mercs.startSeconds, cfg.mercs.killMs, cfg.mercs.eliteKillMs, cfg.mercs.comboWindowMs, cfg.mercs.comboStep,
          cfg.mercs.comboBonusMs, cfg.mercs.capsules, cfg.mercs.capsuleMs, cfg.mercs.goldenMs, cfg.mercs.goldenPercent, cfg.mercs.deathPenaltyMs,
          cfg.mercs.killTarget, run.clockSim, run.deathsSeen);
    Event("mercenaries run %u limits: %u capsule(s) at a time, %u in all (0 = no limit), %u m from every player; bonuses fall to %u %% over %u s; "
          "the run ends after %u s at the latest (0 = never)", run.number, cfg.mercs.capsules, cfg.mercs.capsuleTotal, cfg.mercs.capsuleMinM,
          cfg.mercs.fadeFloorPercent, cfg.mercs.fadeS, cfg.mercs.maxRunS);
    MercsFlash("MERCENARIES  -  %u S ON THE CLOCK", cfg.mercs.startSeconds);
    (void)now;
}
void MercsOver(uint32_t now, bool cleared) {
    const auto& k = cfg.mercs;
    run.mercsOver = true;
    run.mercsCleared = cleared;
    run.resultBonus = cleared ? static_cast<unsigned>(run.clockMs / 1000) * k.clearBonusPerSecond : 0;
    run.ms.score += run.resultBonus;
    const std::string key = "mercenaries:" + run.map;
    const unsigned before = best.count(survival::LowerText(key)) ? best[survival::LowerText(key)] : 0;
    run.resultRecord = UpdateBestFor(key, run.ms.score);
    if (run.resultRecord) SaveBest();
    const char* rank = survival::MercsRank(run.ms.score, k.ranks);
    if (!run.xpResultPaid && xpRules.mercsDivisor) {
        run.xpResultPaid = true;
        PayXp(coop_progress::kMercsResult, run.wave, run.ms.score / xpRules.mercsDivisor);
    }
    Event("mercenaries run %u %s: score %u (clear bonus %u) rank %s, kills %u, best combo %u, capsules %u, deaths %u, stage %u; best before %u%s", run.number,
          cleared ? "CLEARED" : "TIME UP", run.ms.score, run.resultBonus, rank, run.ms.kills, run.ms.maxCombo, run.ms.capsules, run.ms.deaths, run.wave, before,
          run.resultRecord ? ", new best saved" : "");
    char text[120];
    std::snprintf(text, sizeof(text), "%s - SCORE %u - RANK %s%s", cleared ? "CLEARED" : "TIME UP", run.ms.score, rank, run.resultRecord ? " - NEW BEST" : "");
    Notice(text);
    SetState(State::Dead, now);
}
// A partner's copies of the host's capsules, by the host's id.
struct CapsuleCopy { uint32_t id = 0, item = 0, vtable = 0; V3 pos; bool golden = false; Guid guid{}; };
std::vector<CapsuleCopy> capsuleCopies;
volatile LONG capsuleCopiesPlaced = 0, capsuleCopiesRemoved = 0;
void FollowHostCapsules() {
    if (!capsuleTake) return;
    int kind = 0, golden = 0;
    unsigned id = 0;
    float pos[3]{};
    for (int guard = 0; guard < 8 && capsuleTake(&kind, &id, pos, &golden); ++guard) {
        if (kind != 5 && kind != 6) continue;
        if (kind == 5) {
            if (!id || std::any_of(capsuleCopies.begin(), capsuleCopies.end(), [id](const CapsuleCopy& c) { return c.id == id; })) continue;
            const uint32_t item = DropAt(CapsuleGuid(golden != 0), V3{pos[0], pos[1], pos[2]}, golden ? "golden capsule (host's)" : "capsule (host's)");
            if (!item) continue;
            CapsuleCopy copy{id, item, 0, V3{pos[0], pos[1], pos[2]}, golden != 0};
            Read(item, &copy.vtable, 4);
            Read(item + kEntityGuid, copy.guid.data(), sizeof(copy.guid));
            capsuleCopies.push_back(copy);
            InterlockedIncrement(&capsuleCopiesPlaced);
            Event("co-op client: the host's capsule %u copied at (%.1f %.1f %.1f) item %08X", id, pos[0], pos[1], pos[2], item);
            continue;
        }
        for (size_t i = 0; i < capsuleCopies.size();) {
            if (id && capsuleCopies[i].id != id) { ++i; continue; }
            uint32_t owner = 1;
            const CapsuleCopy& copy = capsuleCopies[i];
            // one we picked up belongs to us now
            if (SameEntity(copy.item, copy.vtable, copy.guid) && Read(copy.item + kItemOwner, &owner, 4) && !owner && Have(sDelete)) DeleteRaw(copy.item);
            capsuleCopies.erase(capsuleCopies.begin() + static_cast<std::ptrdiff_t>(i));
            InterlockedIncrement(&capsuleCopiesRemoved);
        }
    }
}

// The capsules standing now as waypoints on the screen (DS2Coop_SurvivalMarks):
// ours on the host or alone, the copies of the host's on a partner. Every tick.
template <typename List>
void ShowCapsuleMarks(const List& list) {
    NetRole();   // resolves the export
    if (!capsuleMarks) return;
    float xyz[8 * 3]{};
    int golden[8]{};
    int n = 0;
    for (const auto& c : list) {
        if (n >= 8) break;
        xyz[n * 3] = c.pos.x;
        xyz[n * 3 + 1] = c.pos.y;
        xyz[n * 3 + 2] = c.pos.z;
        golden[n] = c.golden ? 1 : 0;
        ++n;
    }
    capsuleMarks(xyz, golden, n);
}
void ZoneVents();
void MercsTick(uint32_t now, const Viewer* viewers, unsigned viewerCount) {
    const auto& k = cfg.mercs;
    ShowCapsuleMarks(capsules);
    if (now - run.lastPoll >= 500) {
        run.lastPoll = now;
        PollTracked(now, viewers, viewerCount);   // kills reach MercsKill through Retire
        ZoneVents();   // the co-op plugin opens the whole map for Mercenaries too
        LogVentGates();
        WatchCapsules(now, viewers, viewerCount);
    }
    // the clock on game time: the pause menu stops it, a hitch longer than a second is not counted
    const uint32_t sim = SimMs();
    const uint32_t delta = sim - run.clockSim;
    run.clockSim = sim;
    if (delta < 1000) run.clockMs = survival::MercsClock(run.clockMs, -static_cast<int64_t>(delta), static_cast<int64_t>(k.maxSeconds) * 1000);
    // a death anywhere costs time
    const LONG deaths = HumanDeathsNow();
    if (deaths >= 0) {
        if (run.deathsSeen >= 0 && deaths > run.deathsSeen) {
            const unsigned n = static_cast<unsigned>(deaths - run.deathsSeen);
            run.ms.deaths += n;
            InterlockedExchangeAdd(&mercsDeaths, static_cast<LONG>(n));
            run.clockMs = survival::MercsClock(run.clockMs, -static_cast<int64_t>(k.deathPenaltyMs) * n, static_cast<int64_t>(k.maxSeconds) * 1000);
            MercsFlash("DOWN  -%u S", k.deathPenaltyMs * n / 1000);
            Event("mercenaries: %u death(s) (%ld in all): -%u ms, clock %.1f s", n, deaths, k.deathPenaltyMs * n, static_cast<double>(run.clockMs) / 1000.0);
        }
        run.deathsSeen = deaths;
    }
    if (capsules.size() < k.capsules && (!k.capsuleTotal || run.capsulesPlaced < k.capsuleTotal) && now - run.lastCapsuleTry >= k.capsuleCheckMs) {
        run.lastCapsuleTry = now;
        PlaceCapsule(now, viewers, viewerCount);
    }
    if (run.next >= run.queue.size() && !survival::DeadlinePending(now, run.stageRetryAt)) AppendStage(run.wave + 1, now);
    if (!run.finalDone && k.finalSeconds && !k.finalEnemies.empty() && run.clockMs <= static_cast<int64_t>(k.finalSeconds) * 1000) InjectFinal(now);
    if (k.supplyKills && run.ms.kills >= run.supplyAt + k.supplyKills) {
        run.supplyAt = run.ms.kills;
        ShareRewards(viewers, viewerCount, "mercenaries supplies");
        MercsFlash("SUPPLIES DROPPED");
    }
    if (k.killTarget && run.ms.kills >= k.killTarget) { MercsOver(now, true); return; }
    if (run.clockMs <= 0) { MercsOver(now, false); return; }
    if (k.maxRunS && sim - run.clockStartSim >= k.maxRunS * 1000u) {
        Event("mercenaries run %u reached MaxRunSeconds (%u s): over with %.1f s left on the clock", run.number, k.maxRunS, static_cast<double>(run.clockMs) / 1000.0);
        run.clockMs = 0;
        MercsOver(now, false);
        return;
    }
    SpawnStep(now, viewers, viewerCount);
}

// --- Objective ---
// The MP map's own objectives (DS2Coop_GameMode 2, [Objective] in DS2Survival.ini) on
// the Mercenaries clock. The map announces objective n with obj_n_new, so obj_n_new
// means n - 1 is done: the clock gains StepBonusSeconds, the hordes pause for
// BreatherSeconds, supplies drop and the next stage joins. The map's TimerSets (its
// own time bonuses, generated_objective_table.h) add BonusPercent of their seconds.
// The human win message ends the run as cleared; the clock running out, the
// necromorph win message or everybody down end it as lost. After the result the
// map restarts from its checkpoint, so its objectives start over.
volatile LONG objectiveRuns = 0, objectiveSteps = 0, objectiveBonuses = 0, objectiveWins = 0, objectiveLosses = 0, objectiveRestarts = 0;
void ObjectiveBegin(uint32_t now) {
    const auto& k = cfg.objective;
    run.omap = objective_watch::MapFor(run.map.c_str());
    run.clockMs = survival::ObjectiveStartMs(run.omap ? run.omap->roundSeconds : 0.0f, k.startPercent, k.maxSeconds);
    run.clockSim = SimMs();
    run.objectiveBegunSim = run.clockSim;
    run.deathsSeen = HumanDeathsNow();
    run.objStep = objective_watch::Current();
    run.bonusApplied = 0;
    // bonuses the map sent before the run began belong to no one
    if (run.omap)
        for (unsigned i = 0; i < run.omap->count && i < 32; ++i)
            if (objective_watch::Sent(run.omap->bonuses[i].hash) > 0) run.bonusApplied |= 1u << i;
    InterlockedIncrement(&objectiveRuns);
    Event("objective run %u starts on %s: clock %.0f s (round %.0f s x %u%%), objective %u/5 announced, %u map bonus(es) (%08X already sent), step +%u s, "
          "bonuses x%u%%, death -%u ms, breather %u s, watch notes %ld",
          run.number, run.map.c_str(), static_cast<double>(run.clockMs) / 1000.0, run.omap ? run.omap->roundSeconds : 0.0f, k.startPercent, run.objStep,
          run.omap ? run.omap->count : 0u, run.bonusApplied, k.stepBonusS, k.bonusPercent, k.deathPenaltyMs, k.breatherS,
          InterlockedCompareExchange(&objective_watch::notes, 0, 0));
    const unsigned left = static_cast<unsigned>(run.clockMs / 1000);
    MercsFlash("OBJECTIVE  -  %u:%02u ON THE CLOCK", left / 60, left % 60);
    (void)now;
}
void ObjectiveOver(uint32_t now, bool won, const char* why) {
    if (won && !run.xpResultPaid) {
        run.xpResultPaid = true;
        PayXp(coop_progress::kObjectiveClear, run.objStep, xpRules.objectiveClear);
    }
    run.objectiveOver = true;
    run.objectiveWon = won;
    run.objectiveEnd = why;
    run.objectiveEndedSim = SimMs();
    objectiveSpent = true;
    InterlockedIncrement(won ? &objectiveWins : &objectiveLosses);
    const unsigned left = static_cast<unsigned>((run.clockMs + 999) / 1000);
    const std::string key = "objective:" + run.map;
    const unsigned before = best.count(survival::LowerText(key)) ? best[survival::LowerText(key)] : 0;
    run.resultRecord = won && UpdateBestFor(key, left);
    if (run.resultRecord) SaveBest();
    Event("objective run %u %s (%s): objective %u/5, %u done, %u s left, kills %u, deaths %u, played %u s; best before %u s left%s", run.number,
          won ? "CLEARED" : "LOST", why, run.objStep, run.objDone, left, run.objKills, run.objDeaths, (run.objectiveEndedSim - run.objectiveBegunSim) / 1000u, before,
          run.resultRecord ? ", new best saved" : "");
    char text[120];
    if (won) std::snprintf(text, sizeof(text), "MAP CLEARED - %u:%02u LEFT%s", left / 60, left % 60, run.resultRecord ? " - NEW BEST" : "");
    else std::snprintf(text, sizeof(text), "%s - OBJECTIVE %u OF 5", why, run.objStep);
    Notice(text);
    SetState(State::Dead, now);
}
// The defend phases (Titan Mines' bench, Fuel Core's fuses and detonator): DS2Coop
// gets where the living enemies are, and those near a vulnerable target hit it.
void ObjectiveDefend() {
    const auto& k = cfg.objective;
    if (!defendTick || !k.defendDps) return;
    float pos[64 * 3];
    int n = 0;
    for (const auto& t : tracked) {
        V3 p;
        if (!t.actor || n >= 64 || !ReadV3(t.actor + kEntityPos, p)) continue;
        pos[n * 3] = p.x; pos[n * 3 + 1] = p.y; pos[n * 3 + 2] = p.z;
        ++n;
    }
    const uint32_t sim = SimMs();
    const float seconds = run.defendSim ? static_cast<float>(sim - run.defendSim) / 1000.0f : 0.0f;
    run.defendSim = sim;
    float lowest = -1.0f;
    const int open = defendTick(pos, n, seconds, static_cast<float>(k.defendDps), static_cast<float>(k.defendRadiusM), &lowest);
    run.defendFraction = open > 0 ? lowest : -1.0f;
    if (open != run.defendOpen) {
        Event("objective: %d defend target(s) vulnerable (was %d), weakest at %.0f %%, %d enemies tracked", open, run.defendOpen, lowest * 100.0f, n);
        if (open > 0 && run.defendOpen <= 0) MercsFlash("DEFEND THE OBJECTIVE");
        run.defendOpen = open;
    }
}
void ObjectiveTick(uint32_t now, const Viewer* viewers, unsigned viewerCount) {
    const auto& k = cfg.objective;
    const int64_t maxMs = static_cast<int64_t>(k.maxSeconds) * 1000;
    if (now - run.lastPoll >= 500) {
        run.lastPoll = now;
        PollTracked(now, viewers, viewerCount);   // kills count through Retire
        LogVentGates();
        ObjectiveDefend();
    }
    // the clock on game time: the pause menu stops it, a hitch longer than a second is not counted
    const uint32_t sim = SimMs();
    const uint32_t delta = sim - run.clockSim;
    run.clockSim = sim;
    if (delta < 1000) run.clockMs = survival::MercsClock(run.clockMs, -static_cast<int64_t>(delta), maxMs);
    const LONG deaths = HumanDeathsNow();
    if (deaths >= 0) {
        if (run.deathsSeen >= 0 && deaths > run.deathsSeen) {
            const unsigned n = static_cast<unsigned>(deaths - run.deathsSeen);
            run.objDeaths += n;
            run.clockMs = survival::MercsClock(run.clockMs, -static_cast<int64_t>(k.deathPenaltyMs) * n, maxMs);
            if (k.deathPenaltyMs) MercsFlash("DOWN  -%u S", k.deathPenaltyMs * n / 1000);
            Event("objective: %u death(s) (%ld in all): -%u ms, clock %.1f s", n, deaths, k.deathPenaltyMs * n, static_cast<double>(run.clockMs) / 1000.0);
        }
        run.deathsSeen = deaths;
    }
    // the map's own time bonuses
    int64_t added = 0;
    if (run.omap)
        for (unsigned i = 0; i < run.omap->count && i < 32; ++i) {
            const auto& b = run.omap->bonuses[i];
            if ((run.bonusApplied & (1u << i)) || objective_watch::Sent(b.hash) <= 0) continue;
            run.bonusApplied |= 1u << i;
            const int64_t was = run.clockMs;
            run.clockMs = survival::ObjectiveBonusClock(run.clockMs, b.seconds, b.relative, k.bonusPercent, maxMs);
            added += run.clockMs - was;
            InterlockedIncrement(&objectiveBonuses);
            Event("objective: the map's time bonus %s (%08X, %s %.0f s) -> clock %.1f s (%+.1f s)", b.name[0] ? b.name : "?", b.hash, b.relative ? "adds" : "sets",
                  b.seconds, static_cast<double>(run.clockMs) / 1000.0, static_cast<double>(run.clockMs - was) / 1000.0);
        }
    // objectives done: the map announced the next one
    const unsigned current = objective_watch::Current();
    if (current > run.objStep) {
        const unsigned done = run.objStep ? current - run.objStep : (current > 1 ? current - 1 : 0);
        const unsigned was = run.objStep;
        run.objStep = current;
        if (done) {
            run.objDone += done;
            PayXp(coop_progress::kObjectiveStep, run.objDone, xpRules.objectiveStep * done);
            InterlockedExchangeAdd(&objectiveSteps, static_cast<LONG>(done));
            const int64_t before = run.clockMs;
            run.clockMs = survival::MercsClock(run.clockMs, static_cast<int64_t>(k.stepBonusS) * 1000 * done, maxMs);
            // StepFloorSeconds: after a step the clock holds at least this much (16.10.: maps
            // with few bonuses of their own, Marker Lab one, left too little for the next one)
            const int64_t floorMs = static_cast<int64_t>(k.stepFloorS) * 1000;
            if (run.clockMs < floorMs) run.clockMs = survival::MercsClock(run.clockMs, floorMs - run.clockMs, maxMs);
            added += run.clockMs - before;
            run.breatherUntil = k.breatherS ? survival::DeadlineAfter(now, k.breatherS * 1000u) : 0;
            for (unsigned i = 0; i < k.stageStep * done; ++i) AppendStage(run.wave + 1, now);
            if (k.supplies) ShareRewards(viewers, viewerCount, "objective done");
            Event("objective: %u -> %u announced, %u done (%u in all): clock %.1f s (step +%u s, floor %u s), breather %u s, stage %u", was, current, done,
                  run.objDone, static_cast<double>(run.clockMs) / 1000.0, k.stepBonusS * done, k.stepFloorS, k.breatherS, run.wave);
            run.noteObjective = current - 1;
            run.noteAddMs += added;
            run.noteAt = now ? now : 1;
            added = 0;
        } else if (Budget(run.objLines, 40)) {
            Event("objective: the map announced objective %u", current);
        }
    }
    // the 16.10. notice said "+30 S" while the map's own +60 s came 100 ms earlier: both go into one
    if (added > 0) {
        run.noteAddMs += added;
        if (!run.noteAt) run.noteAt = now ? now : 1;
    }
    if (run.noteAt && now - run.noteAt >= (run.noteObjective ? 700u : 1500u)) {
        const unsigned seconds = static_cast<unsigned>((run.noteAddMs + 500) / 1000);
        if (run.noteObjective) MercsFlash("OBJECTIVE %u DONE  +%u S", run.noteObjective, seconds);
        else if (seconds) MercsFlash("+%u S", seconds);
        run.noteAddMs = 0;
        run.noteAt = 0;
        run.noteObjective = 0;
    }
    if (objective_watch::Won()) { ObjectiveOver(now, true, "MAP CLEARED"); return; }
    if (objective_watch::Lost()) { ObjectiveOver(now, false, "OBJECTIVE FAILED"); return; }
    if (run.clockMs <= 0) { ObjectiveOver(now, false, "TIME UP"); return; }
    // more enemies whenever the queue runs dry; none during the breather
    if (run.next >= run.queue.size() && !survival::DeadlinePending(now, run.stageRetryAt)) AppendStage(run.wave + 1, now);
    if (survival::DeadlinePending(now, run.breatherUntil)) return;
    SpawnStep(now, viewers, viewerCount);
}

// --- Hold the Zone ---
// DS2Coop_GameMode 3, [Zone] in DS2Survival.ini. One zone at a time on the MP map,
// at a multiplayer spawn point or an open vent, MinDistance..MaxDistance metres of
// path from the nearest player, in the open. Players inside (bots too, and for a
// host its partners) fill it, faster together; ContestEnemies enemies inside stop
// it; nobody inside lets it drain. A full zone adds BonusSeconds, drops supplies,
// brings the next stage and the zone moves. The clock runs as in Mercenaries and
// deaths cost time. DS2Coop draws it (a ring of glows, the locator line, the way
// on the overlay) and passes it to the partners.
volatile LONG zoneRuns = 0, zonesHeld = 0, zonePlacements = 0, zonePlaceFails = 0, zoneContests = 0, zoneShows = 0;
// state: 0 none, else 1 + ZoneMode; sent on a change and once a second.
void ShowZone(int state, uint32_t now) {
    if (!zoneShow) return;
    const int percent = static_cast<int>(run.zoneProgress * 100.0f);
    if (state == run.zoneShownState && percent == run.zoneShownPercent && now - run.zoneShownAt < 1000) return;
    run.zoneShownState = state;
    run.zoneShownPercent = percent;
    run.zoneShownAt = now;
    const float pos[3] = {run.zoneCenter.x, run.zoneCenter.y, run.zoneCenter.z};
    zoneShow(state, run.zoneId, state ? pos : nullptr, run.zoneRadius, percent);
    InterlockedIncrement(&zoneShows);
}
// Spots recorded in the game (DS2Coop [Zone] RecordKey writes DS2Zones.txt next to
// this INI, one "map x y z [radius]" a line). Zones go only there.
std::vector<survival::ZoneSpot> zoneSpots;
FILETIME zoneSpotsWritten{};
void LoadZoneSpotsFile();
void LoadZoneSpots() {
    LoadZoneSpotsFile();
    if (!zoneSpots.empty() || !InArena()) return;
    zoneSpots = ArenaModePoints(0);
    Event("zone spots: %zu built in for campaign arena %s", zoneSpots.size(), run.map.c_str());
}
void LoadZoneSpotsFile() {
    zoneSpots.clear();
    WIN32_FILE_ATTRIBUTE_DATA a{};
    zoneSpotsWritten = GetFileAttributesExA(zoneSpotsPath.c_str(), GetFileExInfoStandard, &a) ? a.ftLastWriteTime : FILETIME{};
    std::ifstream file(zoneSpotsPath, std::ios::binary);
    if (!file) { Event("zone spots: %s not found, no zones until spots are recorded", zoneSpotsPath.c_str()); return; }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    unsigned skipped = 0, other = 0;
    zoneSpots = survival::ParseZoneSpots(text, run.map, skipped, other);
    Event("zone spots: %zu for %s in %s (%u for other maps, %u lines not understood)", zoneSpots.size(), run.map.c_str(), zoneSpotsPath.c_str(), other, skipped);
}
void SetZone(const V3& at, float radius, int spot, uint32_t now) {
    run.zoneCenter = at;
    run.zoneRadius = radius;
    run.zoneSpot = spot;
    run.zoneHas = run.zoneHadOne = true;
    run.zoneProgress = 0.0f;
    run.zoneMode = survival::ZoneMode::Empty;
    ++run.zoneId;
    ++run.zonePlaced;
    InterlockedIncrement(&zonePlacements);
    MercsFlash(run.zoneHeld ? "THE ZONE MOVED" : "HOLD THE ZONE");
    run.zoneShownState = -1;
    ShowZone(1 + static_cast<int>(run.zoneMode), now);
}
bool PlaceRecordedZone(uint32_t now) {
    const auto& k = cfg.zone;
    const int last = run.zoneHadOne ? run.zoneSpot : -1;
    const int pick = survival::PickZoneSpot(zoneSpots, last, rng.Next());
    if (pick < 0) return false;
    const auto& spot = zoneSpots[static_cast<size_t>(pick)];
    SetZone(spot.pos, spot.radius > 0.0f ? spot.radius : k.radiusM, pick, now);
    Event("zone %u placed at recorded spot %d of %zu (%.1f %.1f %.1f), radius %.1f m", run.zoneHeld + 1, pick + 1, zoneSpots.size(), spot.pos.x, spot.pos.y,
          spot.pos.z, run.zoneRadius);
    return true;
}
// DS2Zones.txt is read again when it changes, so a spot recorded during the run
// counts for the next zone.
bool PlaceZone(uint32_t now) {
    ++run.zoneTries;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExA(zoneSpotsPath.c_str(), GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &zoneSpotsWritten) != 0) LoadZoneSpots();
    return PlaceRecordedZone(now);
}
// "BILD-AUF" for [Zone] RecordKey, the key's name in the keyboard's language.
std::string RecordKeyName() {
    const UINT vk = GetPrivateProfileIntA("Zone", "RecordKey", VK_PRIOR, iniPath.c_str());
    LONG code = static_cast<LONG>(MapVirtualKeyA(vk, MAPVK_VK_TO_VSC)) << 16;
    if ((vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE) code |= 1 << 24;
    char name[32] = "";
    if (!GetKeyNameTextA(code, name, sizeof(name))) std::snprintf(name, sizeof(name), "KEY %u", vk);
    for (char* c = name; *c; ++c) *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
    return name;
}
void ZoneBegin(uint32_t now) {
    const auto& k = cfg.zone;
    run.clockMs = static_cast<int64_t>(k.startSeconds) * 1000;
    run.clockSim = run.zoneSim = SimMs();
    run.clockStartSim = run.clockSim;
    run.deathsSeen = HumanDeathsNow();
    run.zoneSearchSince = now;
    run.zoneLastTry = 0;
    LoadZoneSpots();
    InterlockedIncrement(&zoneRuns);
    Event("zone run %u starts on %s: %u s on the clock (max %u), radius %.1f m (%.1f m below, %.1f m above), capture %u s (+%u %% per more player, "
          "drain %u %%), contested by %u, +%u s per zone, death -%u ms, target %u, %zu recorded spot(s), deaths so far %ld",
          run.number, run.map.c_str(), k.startSeconds, k.maxSeconds, k.radiusM, survival::kZoneBelowM, survival::kZoneAboveM, k.captureS, k.teamSpeedPercent,
          k.decayPercent, k.contestEnemies, k.bonusS, k.deathPenaltyMs, k.targetZones, zoneSpots.size(), run.deathsSeen);
    Event("zone run %u limits: the zone bonus falls to %u %% over %u s; the run ends after %u s at the latest (0 = never)", run.number, k.fadeFloorPercent, k.fadeS,
          k.maxRunS);
    MercsFlash("HOLD THE ZONE  -  %u:%02u ON THE CLOCK", k.startSeconds / 60, k.startSeconds % 60);
}
void ZoneOver(uint32_t now, bool cleared, const char* why) {
    run.zoneOver = true;
    run.zoneCleared = cleared;
    run.zoneEnd = why;
    run.zoneHas = false;
    ShowZone(0, now);
    const std::string key = "zone:" + run.map;
    const unsigned before = best.count(survival::LowerText(key)) ? best[survival::LowerText(key)] : 0;
    run.resultRecord = UpdateBestFor(key, run.zoneHeld);
    if (run.resultRecord) SaveBest();
    Event("zone run %u %s (%s): %u zone(s) held, kills %u, deaths %u, stage %u; best before %u%s", run.number, cleared ? "CLEARED" : "OVER", why, run.zoneHeld,
          run.zoneKills, run.zoneDeaths, run.wave, before, run.resultRecord ? ", new best saved" : "");
    char text[120];
    std::snprintf(text, sizeof(text), "%s - ZONES HELD %u%s", why, run.zoneHeld, run.resultRecord ? " - NEW BEST" : "");
    Notice(text);
    SetState(State::Dead, now);
}
// The co-op plugin opened the map's objectives ([Zone] OpenObjectives in
// DS2United.ini, state 2): every door is open, so every vent is in play.
void ZoneVents() {
    NetRole();
    const LONG resets = InterlockedCompareExchange(&vent_gate::resets, 0, 0);
    if (resets != run.zoneVentResets) {
        run.zoneVentResets = resets;
        run.zoneObjectivesSeen = -2;
        run.allVentsOpen = false;
    }
    const int s = zoneObjectives ? zoneObjectives() : -1;
    if (s == run.zoneObjectivesSeen) return;
    run.zoneObjectivesSeen = s;
    const bool open = s == 2 || (run.keepOpenedVents && run.allVentsOpen);
    if (open != run.allVentsOpen) {
        run.allVentsOpen = open;
        markersOpenLogged = -1;
    }
    Event("zone: the map's objectives %s (DS2Coop_ZoneObjectives %d); vent windows %s", s == 2 ? "are open" : s == 1 ? "are being opened" : s == 3 ? "cannot be opened here"
          : s == 0 ? "are not opened (off or not a Zone visit)" : "are unknown (DS2United.asi without DS2Coop_ZoneObjectives)", s,
          open ? "ignored: every vent counts as open" : "as the map sets them");
}
void ZoneTick(uint32_t now, const Viewer* viewers, unsigned viewerCount) {
    const auto& k = cfg.zone;
    const int64_t maxMs = static_cast<int64_t>(k.maxSeconds) * 1000;
    if (now - run.lastPoll >= 500) {
        run.lastPoll = now;
        PollTracked(now, viewers, viewerCount);   // kills count through Retire
        ZoneVents();
        LogVentGates();
    }
    // the clock on game time: the pause menu stops it, a hitch longer than a second is not counted
    const uint32_t sim = SimMs();
    const uint32_t delta = sim - run.clockSim;
    run.clockSim = sim;
    const bool noSpots = !run.zoneHas && zoneSpots.empty();
    if (delta < 1000 && !noSpots) run.clockMs = survival::MercsClock(run.clockMs, -static_cast<int64_t>(delta), maxMs);
    const LONG deaths = HumanDeathsNow();
    if (deaths >= 0) {
        if (run.deathsSeen >= 0 && deaths > run.deathsSeen) {
            const unsigned n = static_cast<unsigned>(deaths - run.deathsSeen);
            run.zoneDeaths += n;
            run.clockMs = survival::MercsClock(run.clockMs, -static_cast<int64_t>(k.deathPenaltyMs) * n, maxMs);
            if (k.deathPenaltyMs) MercsFlash("DOWN  -%u S", k.deathPenaltyMs * n / 1000);
            Event("zone: %u death(s) (%ld in all): -%u ms, clock %.1f s", n, deaths, k.deathPenaltyMs * n, static_cast<double>(run.clockMs) / 1000.0);
        }
        run.deathsSeen = deaths;
    }
    if (!run.zoneHas) {
        if (now - run.zoneLastTry >= 500) {
            run.zoneLastTry = now;
            if (!PlaceZone(now)) {
                InterlockedIncrement(&zonePlaceFails);
                if (run.zoneTries % 20 == 1) {
                    MercsFlash("NO ZONE SPOTS ON THIS MAP  -  RECORD ONE WITH %s", RecordKeyName().c_str());
                    if (Budget(run.zoneLines, 80))
                        Event("zone: no recorded spot for %s in %s (%u s); the clock stands", run.map.c_str(), zoneSpotsPath.c_str(),
                              (now - run.zoneSearchSince) / 1000);
                }
            } else {
                run.zoneSearchSince = 0;
            }
        }
        run.zoneSim = sim;
    } else {
        unsigned players = 0, enemies = 0;
        for (unsigned i = 0; i < viewerCount; ++i)
            if (viewers[i].alive && survival::InZone(run.zoneCenter, run.zoneRadius, viewers[i].feet)) ++players;
        if (partnerPositions && NetRole() == 1) {
            float partners[8 * 3];
            const int n = partnerPositions(partners, 8);
            for (int q = 0; q < n; ++q)
                if (survival::InZone(run.zoneCenter, run.zoneRadius, V3{partners[q * 3], partners[q * 3 + 1], partners[q * 3 + 2]})) ++players;
        }
        for (const auto& t : tracked) {
            V3 at;
            if (t.actor && !t.unkillable && ReadV3(t.actor + kEntityPos, at) && survival::InZone(run.zoneCenter, run.zoneRadius + 0.5f, at)) ++enemies;
        }
        const uint32_t dt = sim - run.zoneSim < 1000 ? sim - run.zoneSim : 0;
        run.zoneSim = sim;
        const survival::ZoneMode before = run.zoneMode;
        run.zoneMode = survival::ZoneStep(run.zoneProgress, players, enemies, dt, k.captureS, k.teamSpeedPercent, k.decayPercent, k.contestEnemies);
        run.zoneInside = players;
        run.zoneEnemiesInside = enemies;
        if (run.zoneMode != before) {
            if (run.zoneMode == survival::ZoneMode::Contested) {
                InterlockedIncrement(&zoneContests);
                MercsFlash("ZONE CONTESTED");
            }
            if (Budget(run.zoneLines, 120))
                Event("zone %u: %s -> %s at %.0f %% (%u player(s), %u enemies inside)", run.zoneHeld + 1, survival::ZoneModeName(before),
                      survival::ZoneModeName(run.zoneMode), run.zoneProgress * 100.0f, players, enemies);
        }
        if (run.zoneProgress >= 1.0f) {
            ++run.zoneHeld;
            InterlockedIncrement(&zonesHeld);
            PayXp(coop_progress::kZoneHeld, run.zoneHeld, xpRules.zone);
            const int64_t was = run.clockMs;
            run.clockMs = survival::MercsClock(run.clockMs, survival::FadedBonusMs(k.bonusS * 1000u, SimMs() - run.clockStartSim, k.fadeS, k.fadeFloorPercent), maxMs);
            for (unsigned i = 0; i < k.stageStep; ++i) AppendStage(run.wave + 1, now);
            if (k.supplies) ShareRewards(viewers, viewerCount, "zone held");
            Event("zone %u HELD (%u player(s) inside): clock %.1f s (%+.1f s), stage %u", run.zoneHeld, players, static_cast<double>(run.clockMs) / 1000.0,
                  static_cast<double>(run.clockMs - was) / 1000.0, run.wave);
            MercsFlash("ZONE %u HELD  +%u S", run.zoneHeld, static_cast<unsigned>((run.clockMs - was) / 1000));
            run.zoneHas = false;
            run.zoneProgress = 0.0f;
            run.zoneSearchSince = now;
            run.zoneLastTry = 0;
            ShowZone(0, now);
            if (k.targetZones && run.zoneHeld >= k.targetZones) { ZoneOver(now, true, "ALL ZONES HELD"); return; }
        } else {
            ShowZone(1 + static_cast<int>(run.zoneMode), now);
        }
    }
    if (run.clockMs <= 0) { ZoneOver(now, false, "TIME UP"); return; }
    if (k.maxRunS && SimMs() - run.clockStartSim >= k.maxRunS * 1000u) {
        Event("zone run %u reached MaxRunSeconds (%u s) with %u zone(s) held", run.number, k.maxRunS, run.zoneHeld);
        ZoneOver(now, false, "TIME UP");
        return;
    }
    if (run.next >= run.queue.size() && !survival::DeadlinePending(now, run.stageRetryAt)) AppendStage(run.wave + 1, now);
    SpawnStep(now, viewers, viewerCount);
}

// --- waves ---
void StartWave(unsigned n, uint32_t now) {
    run.wave = n;
    run.spec = survival::WaveFor(n, cfg);
    run.xpWave = 0;
    std::map<std::string, std::string> subs;
    unsigned dropped = 0;
    run.queue = survival::ResolveQueue(survival::BuildQueue(run.spec), run.available, dropped, &subs);
    ArenaBrutes(run.queue);
    run.next = 0;
    run.itemSince = 0;
    run.directSince = 0;
    run.waveAt = now;
    run.lastSpawnAt = now;
    run.wc = Counters{};
    run.total = 0;
    for (const auto& item : run.queue) if (!survival::Unkillable(item.family)) ++run.total;
    run.lastLeft = run.total;
    run.lastLeftNotice = now;
    run.countdownShown = 0;
    run.refusals = Refusals{};
    run.haveReady = run.searching = false;
    std::string asked, queue, subText;
    for (const auto& e : run.spec.entries)
        asked += (asked.empty() ? "" : ", ") + e.family + ":" + std::to_string(e.count) + ":" + survival::PathName(e.path) + (e.group > 1 ? ":" + std::to_string(e.group) : "");
    std::map<std::string, unsigned> byFamily;
    for (const auto& item : run.queue) ++byFamily[item.family];
    for (const auto& f : byFamily) queue += (queue.empty() ? "" : " ") + f.first + "=" + std::to_string(f.second);
    for (const auto& s : subs) subText += (subText.empty() ? "" : " ") + s.first + "->" + s.second;
    uint32_t top = 0, dv = 0, footprint = 0;
    const bool nav = NavHeadroom(top, dv, footprint);
    const unsigned cap = WaveCap();
    const char* capFrom = "";
    const unsigned baseCap = BaseCap(&capFrom);
    Event("=== wave %u start: asked [%s] queue %zu (%s) substitutions [%s] dropped=%u cap=%u (one player %u from %s, %u players) interval_ms=%u time_limit_s=%u "
          "carried_over_alive=%u navpower %s top=%u dv=%u footprint=%u",
          n, asked.c_str(), run.queue.size(), queue.c_str(), subText.empty() ? "none" : subText.c_str(), dropped, cap, baseCap, capFrom, playersSeen,
          run.spec.intervalMs ? run.spec.intervalMs : cfg.intervalMs, run.spec.timeLimitS, AliveCount(), nav ? "readable" : "unreadable", top, dv, footprint);
    if (survival::EscLevel(n, cfg))
        Event("    escalation level %u: enhanced %u%% health x%.2f speed x%.2f cap %u (%u players) pause after %u s rewards %u%% prices %u%% bot damage %u%% "
              "boss stasis resist %u%%",
              survival::EscLevel(n, cfg), survival::EnhancedChanceFor(n, run.spec, cfg), survival::HealthFactor(n, cfg), survival::SpeedFactor(n, cfg), cap, playersSeen,
              survival::PauseFor(n, run.spec.pauseS ? run.spec.pauseS : cfg.pauseS, cfg), survival::RewardPercent(n, cfg), survival::PricePercent(n, cfg),
              survival::BotDamagePercent(n, cfg), survival::BossStasisResist(n, cfg));
    if (run.spec.enhanced >= 0)
        Event("    the wave's own Enhanced=%d%%: %u%% of the enemies with an enhanced variant come as one", run.spec.enhanced,
              survival::EnhancedChanceFor(n, run.spec, cfg));
    InterlockedIncrement(&wavesStarted);
    InterlockedExchange(&waveMirror, static_cast<LONG>(n));
    char text[120];
    if (run.spec.timeLimitS) std::snprintf(text, sizeof(text), "WAVE %u - SURVIVE %u S", n, run.spec.timeLimitS);
    else std::snprintf(text, sizeof(text), "WAVE %u - %u ENEMIES", n, run.total);
    if (!ClockRun()) Notice(text);
    SetState(State::Wave, now);
}
void EndWave(uint32_t now, const Viewer* viewers, unsigned viewerCount, const char* how) {
    const auto& r = run.refusals;
    Event("=== wave %u end (%s) after %lus: spawned=%u (vent %u, direct %u) killed=%u stuck_kills=%u vent_stuck_kills=%u inert=%u vanished=%u written_off=%u given_up=%u "
          "still_alive=%u",
          run.wave, how, static_cast<unsigned long>((now - run.waveAt) / 1000), run.wc.spawned, run.wc.vent, run.wc.direct, run.wc.killed, run.wc.stuckKills,
          run.wc.ventStuckKills, run.wc.inert, run.wc.vanished, run.wc.writtenOff, run.wc.dropped, AliveCount());
    Event("    refusals: searches=%u search_failed=%u points ok=%u too_close=%u too_far=%u other_floor=%u no_path=%u path_too_long=%u visible=%u wall=%u "
          "outside_arena=%u no_engine=%u no_donor=%u not_made=%u own_target=%u vent busy=%u close=%u far=%u closed=%u seen=%u high=%u cut_off=%u winding=%u winding_used=%u refused=%u rebind_missed=%u "
          "lock_busy=%u cap_waits=%u navpower_waits=%u",
          r.searches, r.searchFailed, r.verdict[0], r.verdict[1], r.verdict[2], r.verdict[3], r.verdict[4], r.verdict[5], r.verdict[6], r.verdict[7],
          r.verdict[static_cast<unsigned>(survival::PointVerdict::OutsideArena)], r.noEngine,
          r.noDonor, r.notMade, r.ownTarget, r.ventBusy, r.ventClose, r.ventFar, r.ventClosed, r.ventSeen, r.ventHigh, r.ventCut, r.ventWinding, r.ventWindingUsed, r.ventRefused, r.rebindMissed,
          r.lockBusy, r.cap, r.navBudget);
    Event("    vent reach: walks=%ld cut_off=%ld no_answer=%ld", InterlockedCompareExchange(&reachWalks, 0, 0),
          InterlockedCompareExchange(&reachCut, 0, 0), InterlockedCompareExchange(&reachUnknown, 0, 0));
    AwareWaveLine();
    Event("    vent redirect: written=%ld already_there=%ld failed=%ld | arrivals at the chosen vent=%ld at the spawner's own=%ld elsewhere=%ld",
          InterlockedCompareExchange(&ventRedirectsWritten, 0, 0), InterlockedCompareExchange(&ventRedirectsSame, 0, 0),
          InterlockedCompareExchange(&ventRedirectFailures, 0, 0), InterlockedCompareExchange(&arrivalsAtChosen, 0, 0),
          InterlockedCompareExchange(&arrivalsAtOwn, 0, 0), InterlockedCompareExchange(&arrivalsElsewhere, 0, 0));
    InterlockedIncrement(&wavesCleared);
    if (!run.mercs && !run.objective && !run.zone) {
        PayXp(coop_progress::kWaveEnd, run.wave, coop_progress::WaveXp(xpRules, run.wave, run.spec.boss) + run.xpWave);
        run.xpWave = 0;
    }
    GiveRewards(viewers, viewerCount, RewardWave());
    // every partner drops its own share; solo and host count it too (the lives per wave)
    if (NetRole() != 2 && survivalEvent) survivalEvent(2, static_cast<int>(run.wave));
    const unsigned pause = survival::PauseFor(run.wave, run.spec.pauseS ? run.spec.pauseS : cfg.pauseS, cfg);
    run.pauseUntil = now + pause * 1000u;
    run.countdownShown = 0;
    char text[120];
    std::snprintf(text, sizeof(text), "WAVE %u CLEARED - SUPPLIES DROPPED - NEXT WAVE IN %u S", run.wave, pause);
    Notice(text);
    SetState(State::Intermission, now);
}

// Every human down with nobody coming back on a timer (DS2Coop_TeamDown).
void GameOver(uint32_t now) {
    PayRunEnd();
    const unsigned reached = run.wave;
    const bool record = UpdateBestFor(run.map, reached);
    if (record) SaveBest();
    const bool reload = cfg.restartOnGameOver && NetRole() != 2 && (coop_mpmap::IsMpMap(run.map.c_str()) || InArena());
    Event("game over at wave %u: every player is down (best before %u%s); in %u s %s", reached, run.bestBefore, record ? ", new best saved" : "",
          cfg.gameOverS, reload ? "the map reloads from its checkpoint" : "the next run arms, the level stays");
    char text[120];
    std::snprintf(text, sizeof(text), "GAME OVER - WAVE %u REACHED - BEST %u", reached, record ? reached : run.bestBefore);
    Notice(text);
    run.gameOver = true;
    SetState(State::Dead, now);
}

bool KeyPressed() {
    if (!cfg.startKey) return false;
    DWORD owner = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &owner);
    const bool down = owner == GetCurrentProcessId() && (GetAsyncKeyState(static_cast<int>(cfg.startKey)) & 0x8000) != 0;
    const bool pressed = down && !run.keyWasDown;
    run.keyWasDown = down;
    return pressed;
}

// --- the director ---
char mapCache[64] = "";
uint32_t mapReadAt = 0;
bool PoolReady(const char** why) {
    if (!globalTemplates) { *why = "GlobalTemplates=0: no pool"; return false; }
    if (!streamProbeThread) { *why = "the pool stream has not been updated on any thread yet"; return false; }
    if (streamProbeThread != GetCurrentThreadId()) { *why = "this is not the pool's stream thread"; return false; }
    if (!InterlockedCompareExchange(&streamSelectionAllowed, 0, 0)) { *why = "the pool is not dispatched yet (selection gate closed)"; return false; }
    if (!GlobalDonorAllowed(0)) { *why = "GlobalDonorAllowed refuses (pool paused, unlocked or not ready)"; return false; }
    return true;
}

// --- Versus slashers ---
// DS2Coop_GameMode 4, [VersusSlashers] and [VersusSkins] in DS2Survival.ini. The
// map's own five slashers stay blocked (DS2Coop's MP map gate); the host's director
// keeps their count on the map with pool slashers in the map's skins, through
// the map's vents where a vent donor of the skin exists, else out of sight on
// foot. The partners get them as ghosts. DS2Coop_VersusRound gives the round,
// its stage and the player kills; objective_watch the objectives done.
volatile LONG versusRuns = 0, versusRounds = 0, versusSpawned = 0, versusSuper = 0, versusSuperHealthOnly = 0, versusRaises = 0, versusNoMatch = 0;
const char* const kSuperSkin = "super_slasher";
void VersusQueue(bool burst, uint32_t now) {
    const auto& v = cfg.versus;
    survival::Item item;
    item.family = item.asked = "Zombie";
    const unsigned chance = run.pressure.SuperPercent(v);
    const bool super = burst || (chance && rng.Unit() * 100.0f < static_cast<float>(chance));
    if (super && run.vSuperVent) {
        item.package = kSuperSkin;
    } else {
        item.package = survival::NextVersusSkin(run.vBag, run.vSkins, run.vLastSkin, rng);
        run.vLastSkin = item.package;
    }
    item.super = super;
    item.path = ItemHasVent(item) ? survival::Path::Vent : survival::Path::Direct;
    run.queue.push_back(item);
    ++run.total;
    if (Budget(run.vQueueLines, 200))
        Event("versus queue %s%s skin=%s path=%s alive=%u target=%u pressure=%u (super %u%%) due=%zu", burst ? "burst " : "", super ? "SUPER" : "ordinary",
              item.package.empty() ? "any" : item.package.c_str(), survival::PathName(item.path), AliveCount(), run.pressure.Alive(run.vBase, v), run.pressure.level,
              chance, run.vDue.size());
    (void)now;
}
void VersusRaised(uint32_t now, const char* why) {
    const auto& v = cfg.versus;
    run.vPeak = std::max(run.vPeak, run.pressure.level);
    InterlockedIncrement(&versusRaises);
    Event("versus pressure up to %u (%s): super %u%%, alive target %u, %u super slasher(s) now; objectives %u (%u fast), kills %d:%d", run.pressure.level, why,
          run.pressure.SuperPercent(v), run.pressure.Alive(run.vBase, v), v.superBurst, run.pressure.objectives, run.pressure.fast, run.vKills[0], run.vKills[1]);
    for (unsigned i = 0; i < v.superBurst; ++i) VersusQueue(true, now);
}
void VersusBegin(uint32_t now, int round) {
    const auto& v = cfg.versus;
    run.versusPlaying = true;
    run.vRound = round;
    run.wave = static_cast<unsigned>(round);
    run.waveAt = run.vStartedAt = now;
    run.pressure.Start(now, v);
    run.vObjStep = objective_watch::Current();
    run.vKills[0] = run.vKills[1] = 0;
    run.vBase = survival::VersusAliveFor(v, run.map);
    run.vDue.clear();
    for (unsigned i = 0; i < run.vBase; ++i) run.vDue.push_back(now + v.firstSpawnS * 1000u + i * 2000u);
    run.queue.clear();
    run.next = 0;
    run.itemSince = 0;
    run.directSince = 0;
    run.vDroppedSeen = run.rc.dropped;
    run.vMade.clear();
    run.vBag.clear();
    run.vSkins.clear();
    // the map's skins the pool can field here: a vent donor on one of its spawners or a direct one
    std::string report;
    for (const std::string& skin : survival::VersusSkinsFor(v, run.map)) {
        const unsigned vents = PackageVentSpots("Zombie", skin), direct = PackageDirectDonors("Zombie", skin);
        if (vents || direct) run.vSkins.push_back(skin);
        char part[120];
        std::snprintf(part, sizeof(part), " %s(vent spawners %u, direct %u%s)", skin.c_str(), vents, direct, vents || direct ? "" : ", LEFT OUT");
        report += part;
    }
    if (run.vSkins.empty()) run.vSkins.push_back("slasher");
    const unsigned superVents = PackageVentSpots("Zombie", kSuperSkin);
    run.vSuperVent = superVents > 0;
    InterlockedIncrement(&versusRounds);
    Event("versus round %d starts on %s: %u slashers, first in %u s, respawn %u..%u s, health %u/%u (super); skins:%s; super slashers %s; pressure: fast objective < %u s, "
          "kill lead %u, decay %u s, max %u; objective %u announced",
          round, run.map.c_str(), run.vBase, v.firstSpawnS, v.respawnMinS, v.respawnMaxS, v.health, v.superHealth, report.c_str(),
          run.vSuperVent ? ("through " + std::to_string(superVents) + " vent spawner(s)").c_str() : "NONE on this map (super health on the map's skins instead)",
          v.fastObjectiveS, v.killLead, v.decayS, v.maxLevel, run.vObjStep);
}
void VersusRoundOver(uint32_t now, int stage) {
    unsigned killedNow = 0;
    for (auto& t : tracked)
        if (t.actor && !t.killSent) { KillActor(t, "versus round over", survival::Fate::Timer, now); ++killedNow; }
    run.versusPlaying = false;
    run.vDue.clear();
    run.next = run.queue.size();
    std::string skins;
    for (const auto& m : run.vMade) skins += " " + m.first + "=" + std::to_string(m.second);
    Event("versus round %d over (stage %d): spawned %u (super %u, super health only %u), killed %u, %u killed for the end; pressure %u (peak %u, raises %u, drops %u, "
          "objectives %u fast %u, kill raises %u, kills %d:%d); health set %u failed %u; skins:%s",
          run.vRound, stage, run.vSpawned, run.vSuper, run.vSuperHealthOnly, run.vKilled, killedNow, run.pressure.level, run.vPeak, run.pressure.raises,
          run.pressure.drops, run.pressure.objectives, run.pressure.fast, run.pressure.killRaises, run.vKills[0], run.vKills[1], run.vHealthSet, run.vHealthFailed,
          skins.empty() ? " none" : skins.c_str());
}
void VersusTick(uint32_t now, const Viewer* viewers, unsigned viewerCount) {
    const auto& v = cfg.versus;
    if (now - run.lastPoll >= 500) { run.lastPoll = now; PollTracked(now, viewers, viewerCount); LogVentGates(); }
    int info[4] = {-1, 0, 0, 0};
    if (!versusRound || versusRound(info, 4) != 1) {
        InterlockedIncrement(&versusNoMatch);
        if (run.vStage != -2 && Budget(run.vLines, 120))
            Event("versus: %s; no slashers", versusRound ? "DS2Coop_VersusRound reports no match on this map" : "DS2Coop_VersusRound missing (DS2United.asi too old)");
        run.vStage = -2;
        if (run.versusPlaying) VersusRoundOver(now, -2);
        return;
    }
    const int stage = info[0], round = info[1];
    if (stage != run.vStage) { Event("versus: match stage %d -> %d, round %d", run.vStage, stage, round); run.vStage = stage; }
    InterlockedExchange(&versusRoundMirror, round);
    InterlockedExchange(&versusStageMirror, stage);
    InterlockedExchange(&versusPressureMirror, static_cast<LONG>(run.pressure.level));
    if (stage != 0) {
        if (run.versusPlaying) VersusRoundOver(now, stage);
        return;
    }
    if (!run.versusPlaying || round != run.vRound) VersusBegin(now, round);
    // the humans' pace: objectives done, the lead in player kills
    const unsigned current = objective_watch::Current();
    if (current > run.vObjStep) {
        const unsigned done = run.vObjStep ? current - run.vObjStep : (current > 1 ? current - 1 : 0);
        const uint32_t took = now - run.pressure.objectiveAt;
        run.vObjStep = current;
        for (unsigned i = 0; i < done; ++i)
            if (run.pressure.Objective(now, v)) VersusRaised(now, "fast objective");
        if (done) Event("versus objective: %u done (%u announced) after %.1f s, pressure %u", done, current, took / 1000.0f, run.pressure.level);
    }
    if (info[2] != run.vKills[0] || info[3] != run.vKills[1]) {
        run.vKills[0] = info[2];
        run.vKills[1] = info[3];
        if (run.pressure.Kills(info[2], info[3], now, v)) VersusRaised(now, "kill lead");
    }
    if (run.pressure.Decay(now, v))
        Event("versus pressure down to %u: no raise for %u s; super %u%%, alive target %u", run.pressure.level, v.decayS, run.pressure.SuperPercent(v),
              run.pressure.Alive(run.vBase, v));
    // an item given up gets its slot back shortly
    for (; run.vDroppedSeen < run.rc.dropped; ++run.vDroppedSeen) run.vDue.push_back(now + 3000);
    // a super slasher that found no open vent comes as the map's skin with the super health
    if (run.next < run.queue.size()) {
        survival::Item& head = run.queue[run.next];
        if (head.package == kSuperSkin && run.itemSince && now - run.itemSince >= cfg.ventWaitMs) {
            head.package = survival::NextVersusSkin(run.vBag, run.vSkins, run.vLastSkin, rng);
            run.vLastSkin = head.package;
            head.path = ItemHasVent(head) ? survival::Path::Vent : survival::Path::Direct;
            run.itemSince = now;
            run.directSince = 0;
            Event("versus: no open vent for a super slasher in %u s; a %s with the super health instead", cfg.ventWaitMs / 1000, head.package.c_str());
        }
    }
    // the slots: alive, queued and waiting add up to the target
    const unsigned target = run.pressure.Alive(run.vBase, v);
    const unsigned pending = static_cast<unsigned>(run.queue.size() - run.next);
    while (AliveCount() + pending + run.vDue.size() < target) run.vDue.push_back(now);
    std::sort(run.vDue.begin(), run.vDue.end(), [now](uint32_t a, uint32_t b) { return static_cast<int32_t>(a - now) < static_cast<int32_t>(b - now); });
    if (!run.vDue.empty() && static_cast<int32_t>(now - run.vDue.front()) >= 0) {
        if (AliveCount() + pending >= target) run.vDue.erase(run.vDue.begin());   // the pressure went down: one slot fewer
        else if (!pending) { run.vDue.erase(run.vDue.begin()); VersusQueue(false, now); }
    }
    const unsigned before = run.rc.spawned;
    SpawnStep(now, viewers, viewerCount);
    if (run.rc.spawned > before && !tracked.empty()) {
        const Tracked& t = tracked.back();
        const char* skin = run.next ? run.queue[run.next - 1].package.c_str() : "?";
        ++run.vSpawned;
        ++run.vMade[skin];
        InterlockedIncrement(&versusSpawned);
        if (t.super) {
            const bool healthOnly = std::strcmp(skin, kSuperSkin) != 0;
            ++(healthOnly ? run.vSuperHealthOnly : run.vSuper);
            InterlockedIncrement(healthOnly ? &versusSuperHealthOnly : &versusSuper);
        }
    }
}

namespace cleanup {
__declspec(noinline) bool FadePickupRaw(uint32_t item, uint32_t fn) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t, bool, bool)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(item)), 1000u, true, false);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        return false;
    }
}
float NearestViewer(const float* at, const Viewer* viewers, unsigned viewerCount) {
    float nearest = 1e9f;
    for (unsigned i = 0; i < viewerCount; ++i) {
        const float dx = at[0] - viewers[i].feet.x, dy = at[1] - viewers[i].feet.y, dz = at[2] - viewers[i].feet.z;
        nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    return nearest;
}
bool CapsuleItem(uint32_t item) {
    for (const auto& c : capsules) if (c.item == item) return true;
    for (const auto& c : capsuleCopies) if (c.item == item) return true;
    return false;
}
// Delete-fades that did not move: the engine's own last step, Base::Delete.
void SweepFades(uint32_t sim) {
    Record due[64];
    unsigned n = 0;
    AcquireSRWLockExclusive(&lock);
    for (auto& f : fades)
        if (f.entity && sim - f.simAt >= cfg.fadeStallMs && n < std::size(due)) { due[n++] = f; f = Record{}; }
    ReleaseSRWLockExclusive(&lock);
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t e = due[i].entity;
        uint32_t vtable = 0, flags = 0, proxy = 0;
        float alpha = 0.0f;
        if (!Read(e, &vtable, 4) || vtable != due[i].vtable || !Read(e + kFadeFlags, &flags, 4) || !(flags & kFadeDelete)) { InterlockedIncrement(&fadesChanged); continue; }
        if (!Read(e + kAlpha, &alpha, 4) || alpha < 0.999f) { InterlockedIncrement(&fadesProgressed); continue; }
        if (vtable == base + placement::kPlayerPrimaryVtableRva || !Read(e + kNetProxy, &proxy, 4) || proxy) { InterlockedIncrement(&fadesGhost); continue; }
        const bool ok = DeleteRaw(e);
        InterlockedIncrement(&fadesDeleted);
        Line("fade of %08X (vtable RVA %08X) stood still %u ms: Base::Delete %s", e, vtable - static_cast<uint32_t>(base), sim - due[i].simAt, ok ? "done" : "FAULTED");
    }
}
// The watchdog's victims once the death had its time, unless a player can see the body.
void SweepKills(uint32_t sim, const Viewer* viewers, unsigned viewerCount) {
    for (auto& k : kills) {
        if (!k.entity || sim - k.simAt < cfg.cleanupDelayMs) continue;
        const Record r = k;
        k = Record{};
        uint32_t vtable = 0, proxy = 0;
        float at[3]{};
        if (!Read(r.entity, &vtable, 4) || vtable != r.vtable) { InterlockedIncrement(&killsGone); continue; }
        if (HealthOf(r.entity) != 0.0f || !Read(r.entity + kNetProxy, &proxy, 4) || proxy) { InterlockedIncrement(&killsKept); continue; }
        if (!Read(r.entity + kEntityPos, at, sizeof(at)) || NearestViewer(at, viewers, viewerCount) < cfg.killKeepRadius) {
            InterlockedIncrement(&killsKept);   // the fade sweeper takes it if its fade stalls later
            continue;
        }
        const bool ok = DeleteRaw(r.entity);
        InterlockedIncrement(&killsDeleted);
        Line("watchdog victim %08X deleted %u ms after its SetHealth(0) (%s)", r.entity, sim - r.simAt, ok ? "done" : "FAULTED");
    }
}
// Oldest first, away from the players, at most 8 a pass; above the hard limit the radius
// no longer protects.
void CapPickups(uint32_t sim, const Viewer* viewers, unsigned viewerCount) {
    Record live[kPickups];
    unsigned n = 0;
    AcquireSRWLockExclusive(&lock);
    for (auto& p : pickups) {
        if (!p.entity) continue;
        uint32_t vtable = 0, flags = 0, owner = 0;
        uint8_t registered = 0;
        if (!Read(p.entity, &vtable, 4) || vtable != p.vtable || !Read(p.entity + kItemFlags, &flags, 4) || (flags & 0x200) ||
            !Read(p.entity + kItemRegistered, &registered, 1) || !registered || !Read(p.entity + kItemOwner, &owner, 4) || owner) {
            p = Record{};
            continue;
        }
        live[n++] = p;
    }
    ReleaseSRWLockExclusive(&lock);
    if (static_cast<LONG>(n) > InterlockedCompareExchange(&pickupsMost, 0, 0)) InterlockedExchange(&pickupsMost, static_cast<LONG>(n));
    if (n <= cfg.maxPickups) return;
    std::sort(live, live + n, [](const Record& a, const Record& b) { return static_cast<int32_t>(a.simAt - b.simAt) < 0; });
    const bool hard = n > cfg.pickupHardMax;
    unsigned over = n - cfg.maxPickups, faded = 0, kept = 0;
    for (unsigned i = 0; i < n && over && faded < 8; ++i) {
        const Record& p = live[i];
        if (sim - p.simAt < cfg.pickupMinAgeS * 1000u || CapsuleItem(p.entity)) { ++kept; continue; }
        float at[3]{};
        if (!hard && Read(p.entity + kEntityPos, at, sizeof(at)) && NearestViewer(at, viewers, viewerCount) < cfg.pickupKeepRadius) { ++kept; continue; }
        uint32_t fn = 0;
        if (!Read(p.vtable + kItemFadeSlot, &fn, 4) || fn < base + 0x1000) { ++kept; continue; }
        if (FadePickupRaw(p.entity, fn)) { ++faded; --over; InterlockedIncrement(&pickupsFaded); }
    }
    if (faded) Line("pickups: %u tracked (max %u%s), %u oldest faded out, %u kept (young, near a player or a capsule)", n, cfg.maxPickups, hard ? ", over the hard limit" : "",
                    faded, kept);
}
DWORD passAt = 0;
void Tick(uint32_t now, bool mpMap, const Viewer* viewers, unsigned viewerCount) {
    if (InterlockedExchange(&active, mpMap ? 1 : 0) && !mpMap) {
        AcquireSRWLockExclusive(&lock);
        std::fill_n(fades, kFades, Record{});
        std::fill_n(pickups, kPickups, Record{});
        std::fill_n(kills, kKills, Record{});
        ReleaseSRWLockExclusive(&lock);
    }
    if (!mpMap || now - passAt < 500) return;
    passAt = now;
    const uint32_t sim = SimMs();
    if (fadeHooked) SweepFades(sim);
    if (cfg.cleanupKills && Have(sDelete)) SweepKills(sim, viewers, viewerCount);
    if (pickupHooked) CapPickups(sim, viewers, viewerCount);
}
void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG sum = fadesSeen / 50 + fadesDeleted * 3 + killsDeleted * 5 + killsKept * 7 + pickupsFaded * 11 + pickupsRegistered / 50 + pickupsDropped * 13;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "cleanup fades seen=%ld deleted=%ld progressed=%ld changed=%ld ghosts=%ld | watchdog victims noted=%ld deleted=%ld kept=%ld gone=%ld | "
                      "pickups registered=%ld unregistered=%ld most=%ld faded=%ld table_full=%ld\n",
                 fadesSeen, fadesDeleted, fadesProgressed, fadesChanged, fadesGhost, killsNoted, killsDeleted, killsKept, killsGone, pickupsRegistered,
                 pickupsUnregistered, pickupsMost, pickupsFaded, pickupsDropped);
}
}  // namespace cleanup

// --- host tools ---
// The co-op debug menu's HOST tab asks through DS2Survival_Admin from its own thread;
// the asks wait here and run on the director's tick. A tool that changes the run's
// course (a wave jump, a skipped wave, an extra enemy, the hold, the cap) marks the
// run, and a marked run saves no best.
namespace admin {
enum Command : int { kStartNow = 1, kJumpWave, kEndWave, kSpawn, kHold, kCap, kRestartRun };
struct Ask { int command, value; };
SRWLOCK lock = SRWLOCK_INIT;
Ask asks[8]{};
unsigned askCount = 0;
char result[96] = "", info[96] = "";
volatile LONG resultSeq = 0, asked = 0, done = 0, refused = 0;
uint32_t holdTickAt = 0, infoAt = 0;
bool Queue(int command, int value) {
    if (!installed) return false;
    AcquireSRWLockExclusive(&lock);
    const bool room = askCount < std::size(asks);
    if (room) asks[askCount++] = Ask{command, value};
    ReleaseSRWLockExclusive(&lock);
    if (room) InterlockedIncrement(&asked);
    return room;
}
// "!" in front marks a refusal for the panel.
void Say(bool ok, const char* format, ...) {
    char text[96];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    AcquireSRWLockExclusive(&lock);
    std::snprintf(result, sizeof(result), "%s%s", ok ? "" : "!", text);
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement(&resultSeq);
    InterlockedIncrement(ok ? &done : &refused);
    Event("host tool %s: %s", ok ? "done" : "refused", text);
}
}  // namespace admin
bool WaveRun() { return run.state != State::Off && !run.mercs && !run.objective && !run.zone && !run.versus; }
unsigned KillWave(const char* why, uint32_t now) {
    unsigned n = 0;
    for (auto& t : tracked)
        if (t.actor && t.wave == run.wave && !t.unkillable && !t.killSent) { KillActor(t, why, survival::Fate::Timer, now); ++n; }
    return n;
}
void AdminStep(uint32_t now, const Viewer* viewers, unsigned viewerCount) {
    admin::Ask asks[8];
    unsigned count = 0;
    AcquireSRWLockExclusive(&admin::lock);
    count = admin::askCount;
    std::copy_n(admin::asks, count, asks);
    admin::askCount = 0;
    ReleaseSRWLockExclusive(&admin::lock);
    for (unsigned i = 0; i < count; ++i) {
        const int value = asks[i].value;
        if (NetRole() == 2) { admin::Say(false, "only the host runs the director"); continue; }
        switch (asks[i].command) {
        case admin::kStartNow:
            if (run.state != State::Arming || run.versus) { admin::Say(false, "no run is arming (%s)", StateName(run.state)); break; }
            if (run.available.empty()) { admin::Say(false, "the pool is not ready yet"); break; }
            StartWave(1, now);
            if (run.mercs) MercsBegin(now);
            else if (run.objective) ObjectiveBegin(now);
            else if (run.zone) ZoneBegin(now);
            admin::Say(true, "run %u started now", run.number);
            break;
        case admin::kJumpWave: {
            if (!WaveRun() || run.state == State::Dead) { admin::Say(false, "wave jumps are for a running survival run"); break; }
            if (run.available.empty()) { admin::Say(false, "the pool is not ready yet"); break; }
            const unsigned target = static_cast<unsigned>(value < 1 ? 1 : value > 999 ? 999 : value);
            const unsigned from = run.wave, killed = KillWave("host tool wave jump", now);
            run.adminUsed = true;
            StartWave(target, now);
            admin::Say(true, "wave %u -> %u (%u enemies of wave %u killed)", from, target, killed, from);
            break;
        }
        case admin::kEndWave:
            if (!WaveRun()) { admin::Say(false, "no survival wave to end"); break; }
            if (run.state == State::Wave) {
                const unsigned killed = KillWave("host tool wave end", now);
                run.next = run.queue.size();
                run.adminUsed = true;
                const unsigned wave = run.wave;
                EndWave(now, viewers, viewerCount, "host tool");
                admin::Say(true, "wave %u ended, %u enemies killed, rewards dropped", wave, killed);
            } else if (run.state == State::Intermission) {
                StartWave(run.wave + 1, now);
                admin::Say(true, "break skipped, wave %u started", run.wave);
            } else {
                admin::Say(false, "nothing to end (%s)", StateName(run.state));
            }
            break;
        case admin::kSpawn: {
            if (run.state != State::Wave || run.versus) { admin::Say(false, "extra enemies only during a wave"); break; }
            if (value < 0 || value >= static_cast<int>(std::size(survival::kFamilies))) { admin::Say(false, "unknown kind %d", value); break; }
            const char* family = survival::kFamilies[value].family;
            survival::Item item;
            item.family = item.asked = family;
            unsigned dropped = 0;
            std::map<std::string, std::string> subs;
            const auto resolved = survival::ResolveQueue({item}, run.available, dropped, &subs);
            if (resolved.empty()) { admin::Say(false, "%s cannot come on this map, nor any stand-in", family); break; }
            run.queue.insert(run.queue.begin() + static_cast<std::ptrdiff_t>(std::min(run.next, run.queue.size())), resolved[0]);
            if (!survival::Unkillable(resolved[0].family)) ++run.total;
            run.adminUsed = true;
            if (resolved[0].family != family) admin::Say(true, "%s comes next (as %s)", family, resolved[0].family.c_str());
            else admin::Say(true, "%s comes next", family);
            break;
        }
        case admin::kHold:
            if (value && !WaveRun()) { admin::Say(false, "the hold is for survival waves only"); break; }
            InterlockedExchange(&admin::hold, value ? 1 : 0);
            admin::holdTickAt = 0;
            if (value) run.adminUsed = true;
            admin::Say(true, "director hold %s", value ? "ON: no spawns, timers stand still" : "OFF");
            break;
        case admin::kCap:
            InterlockedExchange(&adminCap, value < 0 ? 0 : value > 40 ? 40 : value);
            if (value > 0 && run.state != State::Off) run.adminUsed = true;
            admin::Say(true, value > 0 ? "max alive %d (now %u with the NavPower guard)" : "max alive back to the INI's (now %u)", value, WaveCap());
            break;
        case admin::kRestartRun:
            if (run.state == State::Off) { admin::Say(false, "no run to restart"); break; }
            InterlockedExchange(&admin::hold, 0);
            if (RealRestart("host tool restart", now)) {
                EndRun("host tool: the map reloads from its checkpoint", now);
                admin::Say(true, "the map reloads; the next run arms on it");
            } else {
                EndRun("host tool: restart", now);
                admin::Say(true, "run ended; the next one arms now");
            }
            break;
        default:
            admin::Say(false, "unknown tool %d", asks[i].command);
            break;
        }
    }
}
// The panel's line: mode, state, wave, alive, queue, hold, cap.
void AdminInfo(uint32_t now) {
    if (now - admin::infoAt < 250) return;
    admin::infoAt = now;
    char text[96];
    const char* mode = run.versus ? "VERSUS" : run.mercs ? "MERCS" : run.objective ? "OBJECTIVE" : run.zone ? "ZONE" : "SURVIVAL";
    const unsigned alive = AliveCount();
    const bool held = InterlockedCompareExchange(&admin::hold, 0, 0) != 0;
    const LONG cap = InterlockedCompareExchange(&adminCap, 0, 0);
    switch (run.state) {
    case State::Off: std::snprintf(text, sizeof(text), "NO RUN"); break;
    case State::Arming: {
        const unsigned delay = StartDelayS(), gone = (now - run.armedAt) / 1000u;
        std::snprintf(text, sizeof(text), "%s ARMING  START IN %u S", mode, delay > gone ? delay - gone : 0u);
        break;
    }
    case State::Wave:
        std::snprintf(text, sizeof(text), "%s W%u  ALIVE %u  QUEUE %zu/%zu", mode, run.wave, alive, run.next, run.queue.size());
        break;
    case State::Intermission: {
        const uint32_t left = static_cast<int32_t>(run.pauseUntil - now) > 0 ? (run.pauseUntil - now + 999) / 1000 : 0;
        std::snprintf(text, sizeof(text), "%s W%u CLEARED  NEXT IN %u S", mode, run.wave, left);
        break;
    }
    case State::Dead: std::snprintf(text, sizeof(text), "%s W%u  OVER", mode, run.wave); break;
    }
    size_t used = std::strlen(text);
    if (held && used < sizeof(text)) used += std::snprintf(text + used, sizeof(text) - used, "  HOLD");
    if (cap > 0 && used < sizeof(text)) used += std::snprintf(text + used, sizeof(text) - used, "  CAP %ld", cap);
    if (run.adminUsed && used < sizeof(text)) used += std::snprintf(text + used, sizeof(text) - used, "  NO RECORD");
    const int difficulty = HudDifficulty();
    if (coop_difficulty::Valid(difficulty) && used < sizeof(text)) std::snprintf(text + used, sizeof(text) - used, "  D%d", difficulty);
    AcquireSRWLockExclusive(&admin::lock);
    std::snprintf(admin::info, sizeof(admin::info), "%s", text);
    ReleaseSRWLockExclusive(&admin::lock);
}

void TickInner(uint32_t now) {
    NameKnownSpawners();
    if (now - mapReadAt >= 1000 || !mapCache[0]) {
        mapReadAt = now;
        char arena[64];
        if (!CurrentMap(mapCache, sizeof(mapCache))) mapCache[0] = 0;
        else if (ArenaRunning(arena, sizeof(arena))) std::snprintf(mapCache, sizeof(mapCache), "%s", arena);   // a campaign arena: its name is the map
    }
    // Versus plays on every MP map, whatever [Survival] Map says
    const bool versusMode = cfg.versus.enabled && mapCache[0] && coop_mpmap::IsMpMap(mapCache) && GameModeNow() == 4;
    const bool onMap = mapCache[0] && (MapAllowed(mapCache) || versusMode) && (run.state == State::Off || survival::EqualNoCase(mapCache, run.map.c_str()));
    const uint32_t lm = LmState();
    // never left on over a loading screen or another map
    if (!onMap || lm != kRunning) InterlockedExchange(&arenaRunOwnsSpawns, 0);
    Viewer viewers[8];
    uint32_t local = 0;
    const unsigned viewerCount = ReadViewers(viewers, 8, local);
    if (viewerCount) playersSeen = viewerCount;
    for (unsigned i = 0; i < viewerCount; ++i) {
        const float health = HealthOf(viewers[i].entity);
        viewers[i].alive = !(health >= 0.0f && health <= 0.0f);   // unreadable counts as alive
    }
    cleanup::Tick(now, !InterlockedCompareExchange(&levelEndingAsked, 0, 0) && mapCache[0] && (coop_mpmap::IsMpMap(mapCache) || coop_arena::IsArenaName(mapCache)) && lm == kRunning, viewers, viewerCount);
    // In Versus the host may be a necro without an Isaac of its own: a human on the map stands in
    const uint32_t self = local ? local : versusMode && viewerCount ? viewers[0].entity : 0;
    rewardsLocal = local;
    if (rewardsOwed.count && onMap && viewerCount && local && viewers[0].entity == local && viewers[0].alive && lm == kRunning &&
        !InterlockedCompareExchange(&levelEndingAsked, 0, 0)) {
        uint32_t wave = 0;
        if (rewardsOwed.Take(wave)) DropRewards(viewers, viewerCount, wave);
    }

    // A level release point fired: end the run, then stay idle until a level
    // runs again (the flag clears once the level manager is back in RUNNING).
    WatchTeardownMark(lm, now);
    if (InterlockedCompareExchange(&levelEndingAsked, 0, 0)) {
        if (!tracked.empty()) PurgeForTeardown(false, "the level is being released", now);
        if (run.state != State::Off) EndRun("the level is being released", now);
        if (!endingSeen) { endingSeen = true; endingPlayer = self; endingRunningSince = 0; vent_gate::Reset(); objective_watch::Reset(); objectiveSpent = false; restartAskedAt = 0; capsuleCopies.clear(); rewardsOwed.Clear();
            if (resetRewardSession) resetRewardSession();
            else if (takeRewardWave) for (unsigned i = 0; i < 64 && takeRewardWave() >= 0; ++i) {}
            if (rewardEvents) clientRewardsTaken = rewardEvents(); }
        if (lm != kRunning || !self) { endingRunningSince = 0; return; }
        if (!endingRunningSince) endingRunningSince = now;
        // a new level: the service was replaced, or a new Isaac, or 30 s of RUNNING as the last resort
        const bool newService = InterlockedCompareExchange(&streamServiceEpoch, 0, 0) != InterlockedCompareExchange(&levelEndingEpoch, 0, 0);
        if (!newService && self == endingPlayer && now - endingRunningSince < 30000) return;
        Event("level running again (%s); the director may arm", newService ? "new streaming service" : self != endingPlayer ? "new Isaac" : "30 s fallback");
        InterlockedExchange(&levelEndingAsked, 0);
        endingSeen = false;
    }
    DeleteZombies(lm);
    if (run.state != State::Off) {
        if (!onMap) {
            if (!tracked.empty()) PurgeForTeardown(false, "the map changed", now);
            EndRun(mapCache[0] ? "the map changed" : "no map (menu or loading)", now);
            return;
        }
        // any loading screen ends the run at once: no spawns, polls or writes while a level is torn down
        if (lm != kRunning) {
            if (!tracked.empty()) PurgeForTeardown(false, "the level manager left RUNNING", now);
            EndRun("the level manager left RUNNING (restart or level change)", now);
            return;
        }
        // a respawn on the map (DS2Coop [Respawn]) keeps the run: the new body is
        // ours. It appears before the respawn's epoch moves (09.10.: run ended 259 ms
        // too early), so a pending respawn counts too; the epoch seen follows
        // every tick, so a later checkpoint restart is still told apart.
        NetRole();
        const int rs = respawnState ? respawnState() : 0;
        // Versus: bodies come and go with every death and side swap; only the level counts
        if (local && local != run.player && !run.versus) {
            if ((rs & 1) || (rs >> 2) != respawnEpochSeen) {
                Event("respawn: Isaac %08X replaced by %08X, the run goes on", run.player, local);
                run.player = local;
            } else {
                EndRun("a new Isaac (checkpoint restart)", now);
                return;
            }
        }
        if (!(rs & 1)) respawnEpochSeen = rs >> 2;
    }
    AdminStep(now, viewers, viewerCount);
    if (run.state == State::Off) {
        if (!onMap || lm != kRunning || !self) return;
        if (NetRole() == 2) {
            if (!clientLogged) { clientLogged = true; Event("co-op client on %s: the host runs the waves, no director here", mapCache); }
            FollowHostCapsules();
            ShowCapsuleMarks(capsuleCopies);
            // the host's wave ends: our own share next to our own Isaac
            if (takeRewardWave) {
                if (rewardsOwed.count == rewardsOwed.waves.size()) return;
                const int wave = takeRewardWave();
                if (wave >= 0) GiveRewards(viewers, viewerCount, static_cast<unsigned>(wave));
                return;
            }
            const int hostEvents = rewardEvents ? rewardEvents() : 0;
            if (clientRewardsTaken < 0) clientRewardsTaken = hostEvents;
            if (hostEvents > clientRewardsTaken) {
                ++clientRewardsTaken;
                Event("co-op client: applying legacy reward event %d of %d", clientRewardsTaken, hostEvents);
                GiveRewards(viewers, viewerCount, RewardWave());
            }
            return;
        }
        clientLogged = false;
        // Versus (DS2Coop_GameMode 4): the players fight each other, the director
        // only keeps the AI slashers ([VersusSlashers])
        static bool versusLogged = false, arenaVersusLogged = false;
        if (GameModeNow() == 4 && coop_arena::IsArenaName(mapCache)) {
            if (!arenaVersusLogged) { arenaVersusLogged = true; Event("versus picked, but %s is a campaign arena: it plays survival", mapCache); }
        } else if (GameModeNow() == 4) {
            if (!versusMode) {
                if (!versusLogged) {
                    versusLogged = true;
                    Event("versus on %s: the director stays idle (%s)", mapCache, cfg.versus.enabled ? "not an MP map" : "[VersusSlashers] Enabled=0");
                }
                return;
            }
            versusLogged = false;
            run = Run{};
            run.number = static_cast<unsigned>(InterlockedIncrement(&runsStarted));
            run.player = self;
            run.armedAt = now;
            run.map = mapCache;
            run.versus = true;
            rng = survival::Rng(now ^ 0x5A5AF00Du);
            InterlockedIncrement(&versusRuns);
            Event("versus run %u armed on %s: %u viewer(s), local Isaac %08X, %s; slashers when the round plays (DS2Coop_VersusRound %s)", run.number, mapCache,
                  viewerCount, local, local ? "ours" : "none here (a necro host)", versusRound ? "found" : "MISSING");
            SetState(State::Arming, now);
            ApplyChargeMax();
            return;
        }
        versusLogged = false;
        const float health = HealthOf(local);
        if (health >= 0.0f && health <= 0.0f) return;
        // a run end asked for the checkpoint restart: the next run arms on the reloaded map
        if (restartAskedAt && !RestartSettled(now)) return;
        run = Run{};
        donorFailures.clear();
        run.number = static_cast<unsigned>(InterlockedIncrement(&runsStarted));
        run.player = local;
        run.armedAt = now;
        if (respawnState) respawnEpochSeen = respawnState() >> 2;
        LoadBest();
        run.map = mapCache;
        // the game mode picked in the map picker; a campaign arena gets only the modes it has
        // points for from DS2Coop_GameMode, and Objective needs an MP map whose objectives are not spent yet
        const int mode = GameModeNow();
        run.placeDividersAtBirth = mode >= 0 && mode <= 3 && coop_mpmap::IsMpMap(mapCache);
        run.keepOpenedVents = vent_gate::KeepOpenedFor(mapCache, mode);
        if (run.keepOpenedVents)
            Event("survival vents: retaining opened exits on %s for mode %d", mapCache, mode);
        run.mercs = mode == 1;
        run.objective = mode == 2 && !InArena() && objective_watch::MapFor(run.map.c_str()) && !objectiveSpent;
        run.zone = mode == 3;
        if (InArena()) Event("run %u on campaign arena %s: mode %d (%s)", run.number, run.map.c_str(), mode, run.mercs ? "mercenaries" : run.zone ? "hold the zone" : "survival");
        if (run.objective) run.omap = objective_watch::MapFor(run.map.c_str());
        if (mode == 2 && !run.objective)
            Event("run %u: Objective picked, but %s; survival instead", run.number,
                  InArena() ? "a campaign arena has no objectives" : objectiveSpent ? "this level's objectives are spent (restart pending)" : "this map has no objectives");
        run.bestBefore = best.count(survival::LowerText(run.map)) ? best[survival::LowerText(run.map)] : 0;
        rng = survival::Rng(now ^ 0xA5A5F00Du);
        noticeLooked = false;
        Event("run %u armed on %s: player %08X, %u viewer(s), best so far %u; first wave %s", run.number, mapCache, local, viewerCount, run.bestBefore,
              cfg.startDelayS ? ("in " + std::to_string(cfg.startDelayS) + " s or on the start key").c_str() : "on the start key");
        if (run.mercs) Event("run %u is a MERCENARIES run (DS2Coop_GameMode %d): %u s on the clock, start in %u s", run.number, GameModeNow(), cfg.mercs.startSeconds,
                             StartDelayS());
        if (run.objective) Event("run %u is an OBJECTIVE run on %s: round %.0f s, %u map bonus(es), objective %u/5 announced, start in %u s", run.number,
                                 run.map.c_str(), run.omap->roundSeconds, run.omap->count, objective_watch::Current(), StartDelayS());
        if (run.zone) Event("run %u is a HOLD THE ZONE run on %s: %u s on the clock, start in %u s, zone display %s", run.number, run.map.c_str(),
                            cfg.zone.startSeconds, StartDelayS(), zoneShow ? "through DS2Coop_SurvivalZone" : "MISSING (DS2United.asi too old): overlay line only");
        SetState(State::Arming, now);
        ApplyChargeMax();
        char text[120];
        if (run.mercs) std::snprintf(text, sizeof(text), "MERCENARIES - START IN %u S", StartDelayS());
        else if (run.objective) std::snprintf(text, sizeof(text), "OBJECTIVE - START IN %u S", StartDelayS());
        else if (run.zone) std::snprintf(text, sizeof(text), "HOLD THE ZONE - START IN %u S", StartDelayS());
        else if (cfg.startDelayS) std::snprintf(text, sizeof(text), "SURVIVAL - FIRST WAVE IN %u S", cfg.startDelayS);
        else std::snprintf(text, sizeof(text), "SURVIVAL - PRESS THE START KEY");
        Notice(text, true);
        return;
    }
    // the local player's death ends the run's waves; the restart ends the run
    // (unless the co-op plugin respawns him on the map)
    NetRole();
    const bool respawning = respawnState && (respawnState() & 3);   // pending, or lives left
    if (!run.versus && run.state != State::Dead && local == run.player && !respawning) {
        const float health = HealthOf(local);
        if (health >= 0.0f && health <= 0.0f) {
            const unsigned reached = run.wave;
            const bool record = UpdateBestFor(run.map, reached);
            if (record) SaveBest();
            PayRunEnd();
            Event("player %08X died at wave %u (best before %u%s); waiting for the checkpoint restart or the way back", local, reached, run.bestBefore,
                  record ? ", new best saved" : "");
            char text[120];
            std::snprintf(text, sizeof(text), "WAVE %u REACHED - BEST %u", reached, record ? reached : run.bestBefore);
            Notice(text);
            SetState(State::Dead, now);
            return;
        }
    }
    // every human on the map down (DS2Coop [Respawn]): game over, the level stays
    if (!run.versus && run.state == State::Wave && NetRole() != 2 && teamDown && teamDown() == 1) {
        if (run.objective) ObjectiveOver(now, false, "EVERYONE DOWN");
        else if (run.zone) ZoneOver(now, false, "EVERYONE DOWN");
        else GameOver(now);
        return;
    }
    const bool pressed = KeyPressed();
    // The host's hold: the start delay, the wave timer and the break stand still.
    const bool held = InterlockedCompareExchange(&admin::hold, 0, 0) && WaveRun();
    const uint32_t heldFor = held && admin::holdTickAt ? now - admin::holdTickAt : 0;
    admin::holdTickAt = held ? now : 0;
    if (heldFor) { run.armedAt += heldFor; run.waveAt += heldFor; run.pauseUntil += heldFor; run.lastSpawnAt += heldFor; }
    switch (run.state) {
    case State::Arming: {
        const char* why = nullptr;
        const bool timeUpNow = StartDelayS() && now - run.armedAt >= StartDelayS() * 1000u;
        // After a map restart the countdown reached 0 and nothing started (players, 24.10.); the
        // reason was never on screen. It is now, and the log repeats it every 10 s.
        auto waiting = [&](const char* reason, const char* detail) {
            if (!timeUpNow) return;
            if (!run.waitSince) run.waitSince = now;
            run.waitWhy = reason;
            if (!run.waitLoggedAt || now - run.waitLoggedAt >= 10000) {
                run.waitLoggedAt = now;
                Event("run %u: the start delay is over for %lu s, the first wave waits: %s%s%s (reprobes %u, families %zu)", run.number,
                      static_cast<unsigned long>((now - run.waitSince) / 1000), reason, detail ? ": " : "", detail ? detail : "", run.reprobes, run.available.size());
            }
        };
        if (!PoolReady(&why)) {
            if (now - run.armedAt > 15000 && Budget(run.poolLines, 8)) Event("waiting for the enemy pool: %s (stream thread %lu, this thread %lu)", why,
                                                                         streamProbeThread, GetCurrentThreadId());
            waiting("THE ENEMY POOL", why);
            return;
        }
        if (!run.probed) Probe(now);
        else if ((run.probeMissing || run.available.empty()) && now - run.probedAt >= (run.reprobes < 15 ? 2000u : 5000u)) {
            // the spawners of a reloaded map can register late: after the 15 quick tries, every 5 s
            ++run.reprobes;
            Probe(now, true);
        }
        if (run.versus) {
            if (!run.available.count("Zombie")) {
                if (Budget(run.poolLines, 8)) Event("versus: no slasher can come on %s (see the availability lines); waiting", run.map.c_str());
                return;
            }
            SetState(State::Wave, now);
            return;
        }
        const bool timeUp = StartDelayS() && now - run.armedAt >= StartDelayS() * 1000u;
        if (timeUp || pressed) {
            if (run.available.empty()) {
                if (Budget(run.poolLines, 8)) Event("no family is available on this map; nothing to spawn (see the availability lines)");
                waiting("SPAWN POINTS", "no enemy family can come on this map yet");
                return;
            }
            if (run.waitSince) Event("run %u: the first wave starts %lu s after the start delay (it waited for %s)", run.number,
                                     static_cast<unsigned long>((now - run.waitSince) / 1000), run.waitWhy ? run.waitWhy : "?");
            run.waitWhy = nullptr;
            StartWave(1, now);
            if (run.mercs) MercsBegin(now);
            else if (run.objective) ObjectiveBegin(now);
            else if (run.zone) ZoneBegin(now);
        }
        return;
    }
    case State::Wave: {
        RecoverVent(now);
        if (run.versus) { VersusTick(now, viewers, viewerCount); return; }
        if (run.mercs) { MercsTick(now, viewers, viewerCount); return; }
        if (run.objective) { ObjectiveTick(now, viewers, viewerCount); return; }
        if (run.zone) { ZoneTick(now, viewers, viewerCount); return; }
        if (now - run.lastPoll >= 500) { run.lastPoll = now; PollTracked(now, viewers, viewerCount); LogVentGates(); }
        const unsigned left = Left();
        InterlockedExchange(&leftMirror, static_cast<LONG>(left));
        LastEnemyMarks(now);
        UpdateKeepPoints(now);
        if (held) return;
        if (run.spec.timeLimitS && now - run.waveAt >= run.spec.timeLimitS * 1000u) {
            unsigned killedNow = 0;
            if (cfg.timedEndKill)
                for (auto& t : tracked)
                    if (t.actor && t.wave == run.wave && !t.unkillable) { KillActor(t, "timed wave over", survival::Fate::Timer, now); ++killedNow; }
            Event("wave %u timer over: %zu of %zu queued spawned, %u killed by the timer", run.wave, run.next, run.queue.size(), killedNow);
            run.next = run.queue.size();
            EndWave(now, viewers, viewerCount, "timer");
            return;
        }
        if (!run.spec.timeLimitS && run.next >= run.queue.size() && !WaveAlive()) { EndWave(now, viewers, viewerCount, "all dead"); return; }
        if (left != run.lastLeft && (now - run.lastLeftNotice >= cfg.noticeGapMs || left <= 3) && left && !run.spec.timeLimitS) {
            run.lastLeft = left;
            run.lastLeftNotice = now;
            char text[80];
            std::snprintf(text, sizeof(text), "WAVE %u - ENEMIES LEFT %u", run.wave, left);
            Notice(text, true);
        }
        SpawnStep(now, viewers, viewerCount);
        return;
    }
    case State::Intermission: {
        if (now - run.lastPoll >= 500) { run.lastPoll = now; PollTracked(now, viewers, viewerCount); LogVentGates(); }
        const uint32_t remaining = static_cast<int32_t>(run.pauseUntil - now) > 0 ? (run.pauseUntil - now + 999) / 1000 : 0;
        if ((remaining == 10 && run.countdownShown != 10) || (remaining == 5 && run.countdownShown != 5)) {
            run.countdownShown = remaining;
            char text[80];
            std::snprintf(text, sizeof(text), "NEXT WAVE IN %u S", remaining);
            Notice(text, true);
        }
        if (!remaining || pressed) StartWave(run.wave + 1, now);
        return;
    }
    case State::Dead:
        // after the Objective result: the enemies go and the map restarts from its
        // checkpoint, which loads its objectives again (DS2Coop: the partners follow)
        if (run.objectiveOver && now - run.stateAt >= cfg.objective.resultS * 1000u) {
            unsigned killedNow = 0;
            for (auto& t : tracked)
                if (t.actor && !t.unkillable && !t.killSent) { KillActor(t, "objective over", survival::Fate::Timer, now); ++killedNow; }
            const bool host = NetRole() != 2;
            int asked = -1;
            if (cfg.objective.restart && host && restartCheckpoint) asked = restartCheckpoint();
            if (asked == 1) {
                NoteRestart(now);
                InterlockedIncrement(&objectiveRestarts);
            } else {
                Notice("QUIT TO THE MAIN MENU TO PLAY THIS MAP AGAIN");
            }
            Event("objective: result shown %u s; %u enemies killed; checkpoint restart %s", cfg.objective.resultS, killedNow,
                  asked == 1 ? "asked" : !cfg.objective.restart ? "off (RestartAfterResult=0)" : !restartCheckpoint ? "not available (DS2Coop_RestartCheckpoint missing)"
                                                                                                 : "REFUSED");
            EndRun("objective over", now);
            return;
        }
        // after the Hold the Zone result: the enemies go, the next run arms on the same map
        if (run.zoneOver && now - run.stateAt >= cfg.zone.resultS * 1000u) {
            unsigned killedNow = 0;
            for (auto& t : tracked)
                if (t.actor && !t.unkillable && !t.killSent) { KillActor(t, "zone over", survival::Fate::Timer, now); ++killedNow; }
            Event("zone: result shown %u s; %u enemies killed for the next run", cfg.zone.resultS, killedNow);
            if (cfg.zone.restart) RealRestart("zone over", now);
            EndRun("zone over", now);
            return;
        }
        // after the Mercenaries result: the enemies and capsules go, the next run arms
        if (run.mercsOver && now - run.stateAt >= cfg.mercs.resultS * 1000u) {
            unsigned killedNow = 0;
            for (auto& t : tracked)
                if (t.actor && !t.unkillable && !t.killSent) { KillActor(t, "mercenaries over", survival::Fate::Timer, now); ++killedNow; }
            Event("mercenaries: result shown %u s; %u enemies killed for the next run", cfg.mercs.resultS, killedNow);
            if (cfg.mercs.restart) RealRestart("mercenaries over", now);
            EndRun("mercenaries over", now);
            return;
        }
        // after the game over: the enemies go, everybody respawns (DS2Coop sees
        // the run end) and the next run arms on the new Isaac
        if (run.gameOver && now - run.stateAt >= cfg.gameOverS * 1000u) {
            unsigned killedNow = 0, left = 0;
            for (auto& t : tracked) {
                if (!t.actor) continue;
                ++left;
                if (t.unkillable || t.killSent) continue;
                KillActor(t, "game over", survival::Fate::Timer, now);
                ++killedNow;
            }
            Event("game over: %u of %u enemies killed for the next run (kill ready %d)", killedNow, left, killReady ? 1 : 0);
            const bool real = cfg.restartOnGameOver && RealRestart("game over", now);
            EndRun(real ? "game over: the map reloads from its checkpoint" : "game over: every player was down", now);
        }
        return;
    case State::Off:
        return;
    }
}

uint32_t lastTick = 0;
volatile LONG frameThread = 0, machineThread = 0, frameDrivesAt = 0;
thread_local bool inTick = false;
// The director's clock stands still while the game's does (the pause menu). On wall
// time its stage timers, the stuck watchdog and the capsule timers ran on: in a
// 4-minute pause (13.10., Mercenaries on mp_bombRun) four stages joined, the frozen
// enemies were killed as stuck and replaced, and on unpausing their ragdolls and the
// new spawns at the same vents blew up the physics until Havok's pool ran out
// (crash 16592 at 0x004EAFB2). The paused time is taken out of every timer.
uint32_t pausedTotal = 0, lastWall = 0, lastSimSeen = 0;
volatile LONG pausedTicks = 0, pauses = 0;
bool pausedBefore = false;
void Tick(volatile LONG* driverCounter) {
    if (!installed || inTick) return;
    if (!streamProbeThread || streamProbeThread != GetCurrentThreadId()) return;
    const uint32_t wall = GetTickCount();
    if (wall - lastTick < 100) return;
    lastTick = wall;
    const uint32_t sim = SimMs();
    const bool paused = lastWall && sim == lastSimSeen && run.state != State::Off && LmState() == kRunning &&
                        !InterlockedCompareExchange(&levelEndingAsked, 0, 0);
    if (paused) pausedTotal += wall - lastWall;
    lastWall = wall;
    lastSimSeen = sim;
    const uint32_t now = wall - pausedTotal;
    if (paused != pausedBefore) {
        pausedBefore = paused;
        if (paused) InterlockedIncrement(&pauses);
        Event("game clock %s: the director %s (paused %u s in all)", paused ? "stopped" : "running again", paused ? "waits" : "goes on", pausedTotal / 1000u);
    }
    InterlockedIncrement(driverCounter);
    inTick = true;
    __try {
        if (paused) InterlockedIncrement(&pausedTicks);
        else TickInner(now);
        UpdateHud(now);
        AdminInfo(now);
    } __except (FaultFilter(GetExceptionInformation())) {
        if (InterlockedIncrement(&tickFaults) <= 20) Event("tick FAULT at %08X (state %s)", lastFaultAddress, StateName(run.state));
        forcedVentSource = nullptr;
        survivalSpawnActive = false;
    }
    inTick = false;
}
// The frame tick (once per frame, outside every state machine update) drives
// when it runs on the pool's stream thread; the state machine tick only stands
// in when it does not.
void FrameTick() {
    const DWORD thread = GetCurrentThreadId();
    InterlockedExchange(&frameThread, static_cast<LONG>(thread));
    if (!installed || thread != streamProbeThread) return;
    InterlockedExchange(&frameDrivesAt, static_cast<LONG>(GetTickCount()));
    Tick(&driverFrame);
}
void MachineTick() {
    const DWORD thread = GetCurrentThreadId();
    InterlockedExchange(&machineThread, static_cast<LONG>(thread));
    if (!installed || thread != streamProbeThread) return;
    if (GetTickCount() - static_cast<DWORD>(InterlockedCompareExchange(&frameDrivesAt, 0, 0)) < 1000) return;
    Tick(&driverMachine);
}

// --- start ---
// Worker thread, after the selection hook and the spawn multiplier are in.
void Install(FILE* log, const std::string& directory) {
    logFile = log;
    startTick = GetTickCount();
    iniPath = ds2coop::ConfigFile(directory, "DS2Survival.ini");
    bestPath = directory + "DS2SurvivalBest.txt";
    zoneSpotsPath = directory + "DS2Zones.txt";
    capsuleSpotsPath = directory + "DS2Capsules.txt";
    std::ifstream file(iniPath, std::ios::binary);
    if (!file) {
        std::fprintf(log, "survival off: %s not found\n", iniPath.c_str());
        NotReady("the survival settings file is missing", "reinstall the mod: DS2United.ini belongs in the plugins folder");
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    cfg = survival::ParseConfig(text);
    for (const auto& w : cfg.warnings) std::fprintf(log, "survival config warning: %s\n", w.c_str());
    {
        const char* f = iniPath.c_str();
        auto key = [&](const char* name, unsigned fallback, unsigned hi) {
            const UINT v = GetPrivateProfileIntA("ProgressXp", name, fallback, f);
            return v > hi ? hi : static_cast<unsigned>(v);
        };
        xpRules.kill = key("KillXp", 100, 100000);
        xpRules.heavyKill = key("HeavyKillXp", 300, 100000);
        xpRules.swarmKill = key("SwarmKillXp", 25, 100000);
        xpRules.wave = key("WaveXp", 100, 100000);
        xpRules.waveCap = key("WaveXpCap", 40, 1000);
        xpRules.boss = key("BossXp", 2000, 1000000);
        xpRules.mercsDivisor = key("MercenariesScoreDivisor", 8, 1000);
        xpRules.objectiveStep = key("ObjectiveStepXp", 500, 100000);
        xpRules.objectiveClear = key("ObjectiveClearXp", 3000, 1000000);
        xpRules.zone = key("ZoneXp", 500, 100000);
        std::fprintf(log, "survival xp: kill %u (heavy %u, swarm %u, x2 enhanced), wave %u x wave (cap wave %u), boss +%u, mercenaries score/%u, "
                          "objective step %u, objective cleared %u, zone held %u\n",
                     xpRules.kill, xpRules.heavyKill, xpRules.swarmKill, xpRules.wave, xpRules.waveCap, xpRules.boss, xpRules.mercsDivisor,
                     xpRules.objectiveStep, xpRules.objectiveClear, xpRules.zone);
    }
    if (!cfg.enabled) {
        std::fprintf(log, "survival off ([Survival] Enabled=0 in %s)\n", iniPath.c_str());
        NotReady("survival is off ([Survival] Enabled=0)", "turn on Survival in DS2UnitedConfig.exe, or set [Survival] Enabled=1");
        return;
    }
    std::fprintf(log, "survival config map=%s start_delay_s=%u start_key=%u max_alive=%u interval_ms=%u pause_s=%u distance=%.0f..%.0f bot_min=%.0f ideal=%.0f floor_band=%.1f wall_clearance=%.1f other_floor_bonus=%.1f "
                      "path_factor=%.1f vents=%d vent_share=%u vent_gap_ms=%u vent_los=%d vent_m=%.0f(-%.0f per extra player or bot)..%.0f vent_max_above=%.1f vent_must_reach=%d catch_up=%.0f..%.0fm/%us vent_cross_family=%d vent_ignore_vent_families=%d stuck_s=%u vent_stuck_s=%u inert_s=%u "
                      "engage_m=%.0f timed_end_kill=%d super=%d nav_reserve_kib=%u rewards ammo=%u med=%u(size %u) credits=%u stasis=%u endless=%u..%u +%u%% "
                      "boss_every=%u waves=%zu notices=%d\n",
                 cfg.map.c_str(), cfg.startDelayS, cfg.startKey, cfg.maxAlive, cfg.intervalMs, cfg.pauseS, cfg.minDistance, cfg.maxDistance, cfg.botMinDistance, cfg.idealDistance,
                 cfg.floorBand, cfg.wallClearance, cfg.otherFloorBonus, cfg.maxPathFactor, cfg.ventSpawns ? 1 : 0, cfg.ventSharePercent, cfg.ventGapMs, cfg.ventLineOfSight ? 1 : 0, cfg.ventMinDistance, cfg.ventMinStep, cfg.ventMaxDistance,
                 cfg.ventMaxAbove, cfg.ventMustReach ? 1 : 0, cfg.catchUpMeters, cfg.catchUpMetersFour, cfg.catchUpMs / 1000u, cfg.ventCrossFamily ? 1 : 0, cfg.ventIgnoreFamilyOff ? 1 : 0, cfg.stuckS, cfg.ventStuckS, cfg.inertS, cfg.engageMeters, cfg.timedEndKill ? 1 : 0, cfg.superVariants ? 1 : 0,
                 cfg.navReserveKiB, cfg.rewardAmmoPerGun, cfg.rewardMedPacks, cfg.rewardMedSize, cfg.rewardCredits, cfg.rewardStasis, cfg.endlessFrom,
                 cfg.endlessTo, cfg.endlessGrowthPercent, cfg.bossEvery, cfg.waves.size(), cfg.notices ? 1 : 0);
    for (size_t i = 0; i < cfg.waves.size(); ++i) {
        std::string line;
        for (const auto& e : cfg.waves[i].entries)
            line += (line.empty() ? "" : ", ") + e.family + ":" + std::to_string(e.count) + ":" + survival::PathName(e.path) + (e.group > 1 ? ":" + std::to_string(e.group) : "");
        std::fprintf(log, "survival wave %zu: %s%s%s\n", i + 1, line.c_str(), cfg.waves[i].timeLimitS ? (" time_limit=" + std::to_string(cfg.waves[i].timeLimitS) + "s").c_str() : "",
                     cfg.waves[i].intervalMs ? (" interval_ms=" + std::to_string(cfg.waves[i].intervalMs)).c_str() : "");
    }
    bool required = true;
    for (Site& s : sites) {
        unsigned char actual[18]{};
        s.ok = Read(base + s.rva, actual, s.size) && hook_site::Inspect(actual, s.bytes, s.size) != hook_site::State::Foreign;
        if (s.required && !s.ok) required = false;
        std::fprintf(log, "survival sig %-34s %08X %s%s\n", s.name, static_cast<unsigned>(base + s.rva), s.ok ? "ok" : "MISMATCH", s.required ? "" : " (optional)");
    }
    cleanup::Install(log);
    killReady = Have(sSetHealth);
    for (const uint32_t vtableRva : {kHealthVtable, kDismemberableVtable}) {
        uint32_t slot = 0;
        if (!Read(base + vtableRva + kSetHealthSlot, &slot, 4) || slot != base + kSetHealth) killReady = false;
    }
    rewardsReady = Have(sPickup) && Have(sDrop);
    {
        uint32_t handle = 0, zombieSlot = 0, count = 0, max = 0;
        awareReady = Have(sForcedAware) && Have(sAwareHandle) && Read(base + kAwareVtable + 4, &handle, 4) && handle == base + kAwareHandle;
        chargeMaxReady = Have(sChargeSet) && Writable(base + kChargeMax, 4);
        zombieChargeReady = Have(sZombieVisRead) && Have(sChargeChanceRead) && Read(base + kZombieVtable + 0x348, &zombieSlot, 4) && zombieSlot == base + kZombieChargeSet;
        playDeadReady = Have(sPlayDeadFlag) && Have(sPlayDeadRoll);
        chargeSightReady = Have(sChargeSight);
        std::fprintf(log, "survival play dead off=%d ready=%d; charge without sight=%u (0 off, 1 arenas, 2 everywhere) ready=%d; "
                          "story donors left out: Ellie's two slashers\n",
                     cfg.noPlayDead ? 1 : 0, playDeadReady ? 1 : 0, cfg.chargeUnseen, chargeSightReady ? 1 : 0);
        Read(base + kChargeCount, &count, 4);
        Read(base + kChargeMax, &max, 4);
        std::fprintf(log, "survival awareness ready=%d (force=%d refresh_ms=%u repair_blind=%d group_player=%08X) chargers ready=%d (max_chargers=%u, engine count=%u max=%u) "
                          "zombie_charge ready=%d (floor=%.2f ignore_vis=%d)\n",
                     awareReady ? 1 : 0, cfg.forceAware ? 1 : 0, cfg.awareRefreshMs, cfg.repairBlindTemplates ? 1 : 0, kGroupPlayer, chargeMaxReady ? 1 : 0,
                     cfg.maxChargers, count, max, zombieChargeReady ? 1 : 0, cfg.zombieChargeFloor, cfg.zombieChargeIgnoreVis ? 1 : 0);
    }
    std::string missing;
    if (!settings.enabled) missing += lookupMismatch ? " the selection lookup (signature mismatch: a different game build);" : " randomizer master switch (Enabled=1);";
    if (!selectionHook) missing += " selection hook;";
    if (!globalTemplates) missing += " GlobalTemplates=1 (the enemy pool);";
    if (!spawnerCtorHook) missing += " FuncSpawn registry hook;";
    if (!moreEnemiesHook) missing += " spawn multiplier hook (DriveForcedSpawn);";
    if (!required) missing += " a required engine signature;";
    const unsigned char chooseLocation[] = {0x83,0xC5,0x58,0x8B,0xCD,0xE8,0x2B,0x61,0x53,0xFF};
    const unsigned char applyLocation[] = {0x8B,0x06,0x8B,0x50,0x2C,0x55,0x8B,0xCE,0xFF,0xD2,0x56,0xE8,0x84,0xBA,0x9A,0xFF,
        0x83,0xC4,0x04,0x85,0xC0,0x74,0x52,0x55,0x8B,0xC8,0xE8,0x35,0x7A,0x0A,0x00,0xEB,0x48};
    unsigned char actual[sizeof(applyLocation)]{};
    const bool locationMatches = Read(base + 0xACB330, actual, sizeof(chooseLocation)) && !std::memcmp(actual, chooseLocation, sizeof(chooseLocation)) &&
        Read(base + 0xACB36C, actual, sizeof(applyLocation)) && !std::memcmp(actual, applyLocation, sizeof(applyLocation));
    if (missing.empty() && locationMatches)
        dividerLocationHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0xACB330), OnDividerLocation);
    std::fprintf(log, "survival Divider birth location: signature=%d hook=%d\n", locationMatches, static_cast<bool>(dividerLocationHook));
    BuildDonors();
    std::string families;
    for (const auto& d : directDonors) families += " " + d.first + "=" + std::to_string(d.second.size());
    std::fprintf(log, "survival donors (direct, pool templates):%s\n", families.empty() ? " none" : families.c_str());
    for (const auto& w : unavailableWhy) if (!directDonors.count(w.first)) std::fprintf(log, "survival donors %s: none (%s)\n", w.first.c_str(), w.second.c_str());
    for (const auto& s : ventSpots) {
        std::string list;
        for (const auto& d : s.donors) list += " " + d.first + "=" + std::to_string(d.second.size());
        std::fprintf(log, "survival vent %s (%s) donors:%s\n", s.row->name, s.row->context, list.empty() ? " none" : list.c_str());
    }
    std::fprintf(log, "survival engines on %s: %zu prop spawners in the main streams\n", cfg.map.c_str(), engines.size());
    if (zeroHealthSkipped) std::fprintf(log, "survival donors: %u direct donor(s) with authored health 0 left out (they die on the spawn)\n", zeroHealthSkipped);
    {
        const auto& v = cfg.versus;
        std::fprintf(log, "survival versus slashers %s: alive=%u first=%us respawn=%u..%us health=%u super_health=%u fast_objective=%us kill_lead=%u decay=%us max=%u "
                          "super_per_level=%u%% extra_per_level=%u burst=%u retarget_from_necros=%d\n",
                     v.enabled ? "on" : "off ([VersusSlashers] Enabled=0)", v.alive, v.firstSpawnS, v.respawnMinS, v.respawnMaxS, v.health, v.superHealth,
                     v.fastObjectiveS, v.killLead, v.decayS, v.maxLevel, v.superPercentPerLevel, v.extraAlivePerLevel, v.superBurst, v.retargetFromNecros ? 1 : 0);
        // what the pool offers each map's skins: wall/floor vent spawners of that map with a donor of the skin, and direct donors
        std::map<std::string, std::vector<std::string>> lists = v.skins;
        lists["(default)"] = v.defaultSkins;
        for (const auto& [map, skins] : lists) {
            std::string line;
            for (const std::string& skin : skins) {
                unsigned vents = 0;
                for (const auto& s : ventSpots) {
                    const auto d = s.donors.find("Zombie");
                    if (survival::EqualNoCase(s.row->map, map) && d != s.donors.end() && HasPackage(d->second, skin)) ++vents;
                }
                const unsigned direct = PackageDirectDonors("Zombie", skin);
                line += " " + skin + "(vent spawners " + std::to_string(vents) + ", direct " + std::to_string(direct) + (vents || direct ? ")" : ", NONE)");
            }
            std::fprintf(log, "survival versus skins %s:%s\n", map.c_str(), line.c_str());
        }
    }
    installed = missing.empty();
    // the escalation's speed rides on the variant runtime's SetAnimSpeed hook
    if (installed && cfg.escSpeedPerWave) variant_runtime::InstallSpeedOnly(log);
    std::fprintf(log, "survival %s; kill=%d rewards=%d vent_path=%d (VentRebinding=%d, binding hooks=%d); best wave file %s\n",
                 installed ? "ready" : ("NOT ready, missing:" + missing).c_str(), killReady ? 1 : 0, rewardsReady ? 1 : 0, VentPathOn() ? 1 : 0,
                 settings.ventRebinding ? 1 : 0, bindingOwnerHook && bindingConstructorHook ? 1 : 0, bestPath.c_str());
    if (installed) {
        survivalLevelEnding = LevelEnding;
        survivalFrameTick = FrameTick;
        survivalMachineTick = MachineTick;
        InterlockedExchange(&readiness, 1);
        std::fprintf(log, "survival readiness: ready (map=%s)\n", cfg.map.c_str());
    } else if (!settings.enabled) {
        NotReady(lookupMismatch ? "a game function survival needs has moved (another game build)" : "the randomizer master switch is off",
                 lookupMismatch ? "use the supported Steam build of the game; DS2Randomizer.log names the function" : "set [DS2Randomizer] Enabled=1 or [Survival] Enabled=1");
    } else if (!globalTemplates) {
        if (poolStageFailed.empty())
            NotReady("the enemy pool is off ([DS2Randomizer] GlobalTemplates=0)", "set GlobalTemplates=1 (configurator: Randomizer, Expert page)");
        else {
            char why[160];
            std::snprintf(why, sizeof(why), "the enemy pool did not build (stage %s)", poolStageFailed.c_str());
            NotReady(why, "reinstall the randomizer folder of the mod; DS2Randomizer.log has the details");
        }
    } else {
        char why[160];
        std::snprintf(why, sizeof(why), "a game function survival needs did not hook:%s", missing.c_str());
        NotReady(why, "another plugin may hook the same function, or the game build differs; see DS2Randomizer.log");
    }
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    cleanup::WriteDiagnostics(log);
    static char copy[kRingLines][kLineSize];
    unsigned count = 0;
    AcquireSRWLockExclusive(&ringLock);
    for (unsigned i = 0; i < ringCount; ++i) std::memcpy(copy[i], ring[(ringHead + i) % kRingLines], kLineSize);
    count = ringCount;
    ringHead = ringCount = 0;
    ReleaseSRWLockExclusive(&ringLock);
    for (unsigned i = 0; i < count; ++i) std::fprintf(log, "%s\n", copy[i]);
    static LONG lastSignature = -1;
    static DWORD lastSummary = 0;
    const DWORD now = GetTickCount();
    const LONG signature = InterlockedCompareExchange(&stateMirror, 0, 0) + InterlockedCompareExchange(&waveMirror, 0, 0) * 7 +
                           InterlockedCompareExchange(&totalSpawned, 0, 0) * 13 + InterlockedCompareExchange(&totalKilled, 0, 0) * 17 +
                           InterlockedCompareExchange(&faults, 0, 0) * 19 + InterlockedCompareExchange(&runsStarted, 0, 0) * 23;
    if (signature == lastSignature || now - lastSummary < 10000) return;
    lastSignature = signature;
    lastSummary = now;
    std::fprintf(log, "survival summary state=%s run=%ld wave=%ld alive=%ld left=%ld spawned=%ld killed=%ld waves_started=%ld cleared=%ld casts=%ld cast_faults=%ld cast_us_avg=%ld cast_us_max=%ld wall_nudges=%ld wall_refusals=%ld "
                      "frozen_kills=%ld escalated=%ld health=%ld speed=%ld esc_faults=%ld enhanced_picks=%ld ordinary_picks=%ld enhanced_fallback_refused=%ld hud_pushes=%ld pauses=%ld paused_ticks=%ld paused_s=%u "
                      "driver frame=%ld machine=%ld threads frame=%lu machine=%lu pool_stream=%lu tick_faults=%ld faults=%ld last_fault=%08X lines=%ld over_budget=%ld lost=%ld\n",
                 StateName(static_cast<State>(InterlockedCompareExchange(&stateMirror, 0, 0))), InterlockedCompareExchange(&runsStarted, 0, 0),
                 InterlockedCompareExchange(&waveMirror, 0, 0), InterlockedCompareExchange(&aliveMirror, 0, 0), InterlockedCompareExchange(&leftMirror, 0, 0),
                 InterlockedCompareExchange(&totalSpawned, 0, 0), InterlockedCompareExchange(&totalKilled, 0, 0), InterlockedCompareExchange(&wavesStarted, 0, 0),
                 InterlockedCompareExchange(&wavesCleared, 0, 0), InterlockedCompareExchange(&casts, 0, 0), InterlockedCompareExchange(&castFaults, 0, 0),
                 InterlockedCompareExchange(&casts, 0, 0) ? InterlockedCompareExchange(&castMicros, 0, 0) / InterlockedCompareExchange(&casts, 0, 0) : 0L,
                 InterlockedCompareExchange(&castMicrosMax, 0, 0),
                 InterlockedCompareExchange(&wallNudges, 0, 0), InterlockedCompareExchange(&wallRefusals, 0, 0), InterlockedCompareExchange(&frozenKills, 0, 0),
                 InterlockedCompareExchange(&escalatedEnemies, 0, 0), InterlockedCompareExchange(&escalatedHealth, 0, 0),
                 InterlockedCompareExchange(&escalatedSpeed, 0, 0), InterlockedCompareExchange(&escalationFaults, 0, 0),
                 InterlockedCompareExchange(&enhancedPicks, 0, 0), InterlockedCompareExchange(&ordinaryPicks, 0, 0),
                 InterlockedCompareExchange(&enhancedFallbackRefused, 0, 0), InterlockedCompareExchange(&hudPushes, 0, 0),
                 InterlockedCompareExchange(&pauses, 0, 0), InterlockedCompareExchange(&pausedTicks, 0, 0), pausedTotal / 1000u,
                 InterlockedCompareExchange(&driverFrame, 0, 0), InterlockedCompareExchange(&driverMachine, 0, 0),
                 static_cast<unsigned long>(InterlockedCompareExchange(&frameThread, 0, 0)), static_cast<unsigned long>(InterlockedCompareExchange(&machineThread, 0, 0)),
                 static_cast<unsigned long>(streamProbeThread), InterlockedCompareExchange(&tickFaults, 0, 0),
                 InterlockedCompareExchange(&faults, 0, 0), lastFaultAddress, InterlockedCompareExchange(&linesWritten, 0, 0),
                 InterlockedCompareExchange(&linesOverBudget, 0, 0), InterlockedCompareExchange(&linesLost, 0, 0));
    std::fprintf(log, "survival placements replaced=%ld written_off_requeued_by_vent=%ld walked_not_placed=%ld engines_forgiven=%ld vents_retired=%ld vent_retired_skips=%ld engines_near_player_skipped=%ld | nav reports=%ld reached=%ld short=%ld no_path=%ld\n", InterlockedCompareExchange(&replacedSpawns, 0, 0),
                 InterlockedCompareExchange(&requeuedToVent, 0, 0), InterlockedCompareExchange(&walkedNotPlaced, 0, 0),
                 InterlockedCompareExchange(&enginesForgiven, 0, 0), InterlockedCompareExchange(&ventsRetired, 0, 0),
                 InterlockedCompareExchange(&ventRetiredSkips, 0, 0), InterlockedCompareExchange(&enginesNearSkipped, 0, 0),
                 InterlockedCompareExchange(&navReports, 0, 0), InterlockedCompareExchange(&navReached, 0, 0),
                 InterlockedCompareExchange(&navShort, 0, 0), InterlockedCompareExchange(&navNoPath, 0, 0));
    size_t directCopies = 0;
    for (const auto& f : twitcherDirect) directCopies += f.second.size();
    std::fprintf(log, "survival severed from_wave=%u too_early=%ld\n", cfg.severedFromWave, InterlockedCompareExchange(&twitcherTooEarly, 0, 0));
    std::fprintf(log, "survival severed twitcher_percent=%u pregnant_percent=%u direct_copies=%zu vent_spots=%zu vent_copies=%zu rolls=%ld direct_picks=%ld vent_picks=%ld no_copy=%ld "
                      "| oracle (an enhanced enemy, from the escalation only) direct_copies=%zu vent_copies=%zu left_out_without_plain_donor=%zu picks=%ld capped=%ld share=%u%% max_alive=%u\n",
                 cfg.twitcherPercent, cfg.pregnantPercent, directCopies, twitcherVentSpots, twitcherVentCopies, InterlockedCompareExchange(&twitcherRolls, 0, 0), InterlockedCompareExchange(&twitcherPicks, 0, 0),
                 InterlockedCompareExchange(&twitcherVentPicks, 0, 0), InterlockedCompareExchange(&twitcherNoCopy, 0, 0), oracleDirectCopies, oracleVentCopies, oracleVentAlone,
                 InterlockedCompareExchange(&oraclePicks, 0, 0), InterlockedCompareExchange(&oracleCapped, 0, 0), cfg.escOracleShare, cfg.escOracleMaxAlive);
    if (InterlockedCompareExchange(&objectiveRuns, 0, 0) || InterlockedCompareExchange(&objective_watch::notes, 0, 0))
        std::fprintf(log, "survival objective runs=%ld steps=%ld map_bonuses=%ld wins=%ld losses=%ld restarts=%ld watch: %u messages watched, %ld notes, %ld resets, "
                          "objective %u/5 announced, won=%d lost=%d\n",
                     InterlockedCompareExchange(&objectiveRuns, 0, 0), InterlockedCompareExchange(&objectiveSteps, 0, 0),
                     InterlockedCompareExchange(&objectiveBonuses, 0, 0), InterlockedCompareExchange(&objectiveWins, 0, 0),
                     InterlockedCompareExchange(&objectiveLosses, 0, 0), InterlockedCompareExchange(&objectiveRestarts, 0, 0), objective_watch::watchedCount,
                     InterlockedCompareExchange(&objective_watch::notes, 0, 0), InterlockedCompareExchange(&objective_watch::resets, 0, 0),
                     objective_watch::Current(), objective_watch::Won() ? 1 : 0, objective_watch::Lost() ? 1 : 0);
    if (InterlockedCompareExchange(&realRestarts, 0, 0) || InterlockedCompareExchange(&realRestartRefusals, 0, 0) || InterlockedCompareExchange(&restartWaits, 0, 0))
        std::fprintf(log, "survival restarts asked=%ld refused=%ld | waits done=%ld timed_out=%ld last_wait_ms=%ld | arrivals export %s\n",
                     InterlockedCompareExchange(&realRestarts, 0, 0), InterlockedCompareExchange(&realRestartRefusals, 0, 0),
                     InterlockedCompareExchange(&restartWaits, 0, 0), InterlockedCompareExchange(&restartTimeouts, 0, 0),
                     InterlockedCompareExchange(&restartLastWaitMs, 0, 0), arrivalsReady ? "found" : "MISSING");
    if (InterlockedCompareExchange(&versusRuns, 0, 0))
        std::fprintf(log, "survival versus runs=%ld rounds=%ld spawned=%ld super=%ld super_health_only=%ld raises=%ld health set=%ld failed=%ld targets human=%ld "
                          "NECRO=%ld other=%ld retargeted=%ld no_match_ticks=%ld round_export=%s | now: %s round %ld stage %ld alive %ld pressure %ld\n",
                     InterlockedCompareExchange(&versusRuns, 0, 0), InterlockedCompareExchange(&versusRounds, 0, 0), InterlockedCompareExchange(&versusSpawned, 0, 0),
                     InterlockedCompareExchange(&versusSuper, 0, 0), InterlockedCompareExchange(&versusSuperHealthOnly, 0, 0), InterlockedCompareExchange(&versusRaises, 0, 0),
                     InterlockedCompareExchange(&versusHealthSet, 0, 0), InterlockedCompareExchange(&versusHealthFailed, 0, 0),
                     InterlockedCompareExchange(&versusTargetsHuman, 0, 0), InterlockedCompareExchange(&versusTargetsNecro, 0, 0),
                     InterlockedCompareExchange(&versusTargetsOther, 0, 0), InterlockedCompareExchange(&versusRetargeted, 0, 0),
                     InterlockedCompareExchange(&versusNoMatch, 0, 0), versusRound ? "found" : "missing",
                     InterlockedCompareExchange(&versusRoundMirror, 0, 0) ? "versus run" : "no versus run", InterlockedCompareExchange(&versusRoundMirror, 0, 0),
                     InterlockedCompareExchange(&versusStageMirror, 0, 0), InterlockedCompareExchange(&aliveMirror, 0, 0),
                     InterlockedCompareExchange(&versusPressureMirror, 0, 0));
    if (InterlockedCompareExchange(&zoneRuns, 0, 0))
        std::fprintf(log, "survival zone runs=%ld held=%ld placed=%ld place_fails=%ld contests=%ld shows=%ld display=%s\n", InterlockedCompareExchange(&zoneRuns, 0, 0),
                     InterlockedCompareExchange(&zonesHeld, 0, 0), InterlockedCompareExchange(&zonePlacements, 0, 0),
                     InterlockedCompareExchange(&zonePlaceFails, 0, 0), InterlockedCompareExchange(&zoneContests, 0, 0), InterlockedCompareExchange(&zoneShows, 0, 0),
                     zoneShow ? "ds2coop" : "none");
}
}  // namespace survival_rt

// The co-op half's checkpoint restart hook, before the restart runs (game thread): the director's
// enemies are parked and deleted while their tables still exist. Returns how many there were.
extern "C" __declspec(dllexport) int __cdecl DS2Survival_BeforeTeardown(int deleteNow) {
    const LONG before = InterlockedCompareExchange(&survival_rt::purgedEnemies, 0, 0);
    survival_rt::PurgeForTeardown(deleteNow != 0, deleteNow ? "a checkpoint restart is about to run" : "the level is about to go", GetTickCount());
    return static_cast<int>(InterlockedCompareExchange(&survival_rt::purgedEnemies, 0, 0) - before);
}
