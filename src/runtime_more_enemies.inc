// Dead Space 2 United by Richard Hafer.
// Spawn multiplier runtime adapter.
//
// Hook site is FuncSpawn::HandleEvents + 0x24E (RVA 0x00ACB41E), the first
// instruction of the epilogue that only the successful-create path reaches.
// Everything the authored arrival needs has already run there: the entity
// exists, its transform and group are set, SimManager finalization is done and
// SpawnDone has sent the authored completion message and incremented the
// spawner's own count. EDI is the FuncSpawn object and ESP is back at the
// frame base, which the epilogue itself proves.
//
// A repeat is produced exactly the way the engine produces its own queued
// retry at RVA 0x00ACB44E: build a message whose first dword is
// Attributes+0x00 and dispatch it through the object's own HandleEvents slot.
// So the authored target, transform, group, population gate and completion
// path are the game's, not ours. Nothing here selects a donor.

// FuncSpawn, confirmed in the constructor at RVA 0x00AA5A40 and in the
// consuming code itself.
constexpr uint32_t kFuncSpawnVtableRva = 0x01759C58;
constexpr uint32_t kFuncSpawnHandleEventsRva = 0x00ACB1D0;
constexpr uint32_t kMoreEnemiesHookRva = 0x00ACB41E;
// push ebx / mov ecx,edi / call SpawnDone: the authored arrival's SpawnDone,
// right before the epilogue hook above. FuncSpawn::SpawnDone itself.
constexpr uint32_t kBeforeSpawnDoneRva = 0x00ACB416;
constexpr uint32_t kFuncSpawnSpawnDoneRva = 0x00ABA610;
constexpr unsigned kFuncSpawnAttributes = 0x0C;
constexpr unsigned kFuncSpawnCount = 0xC0;
// Attributes, read off the retail code: 0x00ACB1F9 compares the incoming
// message id against +0x00, 0x00ACB202 reads the limit at +0x1C, 0x00ACB2A1
// passes +0x0C as the target GUID and 0x00EBA61A gates the completion message
// on +0x20.
constexpr unsigned kSpawnAttrTrigger = 0x00;
constexpr unsigned kSpawnAttrTargetGuid = 0x0C;
constexpr unsigned kSpawnAttrMaxSpawn = 0x1C;
constexpr unsigned kSpawnAttrCompletionMessage = 0x20;

// The authored limit at Attributes+0x1C is checked against the spawner's own
// count at +0xC0, and SpawnDone retires the spawner once the count reaches it.
// Parking the count far below the limit for the duration of the repeats keeps
// both out of the way without writing a single byte of authored level data;
// the authored value is put back before the hook returns.
constexpr int kMoreEnemiesCountSentinel = -0x40000000;

ds2::MoreEnemiesSettings moreEnemies;
bool moreEnemiesSkipScene = true;    // [DS2Randomizer] MoreEnemiesSkipScripted
std::map<std::string, unsigned> moreEnemiesFamilyTotals;
ds2::MoreEnemiesBudget moreEnemiesBudget;
volatile LONG moreEnemiesBudgetReset = 0;
safetyhook::MidHook moreEnemiesHook;
thread_local bool moreEnemiesInside = false;

// Hordes. One swarmer or one pack boy standing in for a slasher is no threat,
// so a replacement from such a family is repeated until the horde size is
// reached (SwarmHorde, PackHorde in the INI). OnCreated leaves a mark on the
// spawner the moment the replacement is built and SpawnDone (the hook below)
// takes the mark on its way out. The repeat is the MoreEnemies repeat; a
// horde is only ever a swarmer or pack boy replacing another kind, and
// MoreEnemies never stacks on top of it, so the two never multiply each
// other. A mark older than a few seconds is a leftover and is ignored.
//
// A room that counts kills counts them through the source's death message,
// and every member of a horde carries it: the message hash lives in the
// authored attribute block, which all spawns of one donor share, so it can
// neither be written per copy nor cleared for the copies alone (both were
// tried on 15.09.: six counts for one spawn, then a wave counter that never
// came back). The count is fixed at the sender instead. Every script message
// goes through 0x00756C60 (hash at message+0). For every authored arrival
// that was repeated (horde or MoreEnemies, 20.09.2026 both) one send of its
// death message passes and one per copy is swallowed: whichever of the group
// dies first carries the room's count, the rest are silent. One kill per
// authored spawn, never fewer (a stuck copy cannot hold a door), never more.
// Entries die with the level's stream service and after fifteen minutes, so
// a leftover can never swallow a later room's honest death.
// hordeCopy stays set while a repeat is dispatched, for diagnostics.
unsigned swarmHorde = 6, packHorde = 4;
// Filled once by LoadMoreEnemiesSettings, before any hook exists, and never
// changed after that. Turning hordes off later only sets hordesOff, so the game
// thread never reads the map while another thread clears it.
std::map<std::string, unsigned> hordeSizes;
volatile LONG hordesOff = 0;
bool HordesActive() { return !hordeSizes.empty() && !InterlockedCompareExchange(&hordesOff, 0, 0); }
void DisableHordes() { InterlockedExchange(&hordesOff, 1); }
thread_local bool hordeCopy = false;
struct HordeMark { uintptr_t spawner; DWORD tick; unsigned size; uint32_t deathHash; };
// `passes` sends of `hash` go through before `pending` sends are swallowed.
struct HordeSilence { LONG pending = 0, passes = 0, generation = 0; DWORD armed = 0; };
constexpr DWORD kHordeSilenceMaxAgeMs = 15 * 60 * 1000;
std::map<uint32_t, HordeSilence> hordeSilences;
volatile LONG hordeGeneration = 1;
std::mutex hordeSilenceLock;
safetyhook::InlineHook messageHook;
bool silenceAvailable = false;
volatile LONG hordeSilenceArmed = 0, hordeSendsSilenced = 0, hordeSilencePassed = 0, hordeSilenceExpired = 0;

// One authored arrival was repeated `copies` times: one death passes, the
// copies' deaths are swallowed. `copies` may be 0 when the copies come later
// (deferred vent repeats) and register one by one through ArmHordeSilenceCopy.
void ArmHordeSilence(uint32_t hash, unsigned copies, unsigned passes = 1) {
    if (!hash || !silenceAvailable) return;
    std::lock_guard<std::mutex> guard(hordeSilenceLock);
    const DWORD now = GetTickCount();
    const auto found = hordeSilences.find(hash);
    if (found == hordeSilences.end()) return;
    auto& slot = found->second;
    const LONG generation = InterlockedCompareExchange(&hordeGeneration, 0, 0);
    if (slot.generation != generation || now - slot.armed > kHordeSilenceMaxAgeMs) slot = {};
    slot.generation = generation;
    slot.pending += static_cast<LONG>(copies);
    slot.passes += static_cast<LONG>(passes);
    slot.armed = now;
    InterlockedExchangeAdd(&hordeSilenceArmed, static_cast<LONG>(copies));
}
void ArmHordeSilenceCopy(uint32_t hash) { ArmHordeSilence(hash, 1, 0); }

// true = swallow this send
bool ConsumeHordeSilence(uint32_t hash) {
    // The keys are fixed before the hook is installed.
    const auto found = hordeSilences.find(hash);
    if (found == hordeSilences.end()) return false;
    std::lock_guard<std::mutex> guard(hordeSilenceLock);
    auto& entry = found->second;
    if (entry.generation != InterlockedCompareExchange(&hordeGeneration, 0, 0)) return false;
    if (GetTickCount() - entry.armed > kHordeSilenceMaxAgeMs) {
        entry = {};
        InterlockedIncrement(&hordeSilenceExpired);
        return false;
    }
    if (entry.passes > 0) { --entry.passes; InterlockedIncrement(&hordeSilencePassed); return false; }
    if (entry.pending > 0) { --entry.pending; return true; }
    return false;
}

// Stream teardown may hold an engine lock.
void ClearHordeSilence() {
    InterlockedIncrement(&hordeGeneration);
}

// 0x00756C60: bool __cdecl(message*, flags); message+0 is the hash. Returns
// false when the name is not in the table, which is what an unknown script
// message gets anyway; a swallowed send reports true, as if delivered.
bool __cdecl OnDispatchMessage(void* message, int flags) {
    uint32_t hash = 0;
    if (message && Read(reinterpret_cast<uintptr_t>(message), &hash, 4) && hash) {
        vent_gate::Note(hash);   // the MP maps' vents opening and closing
        objective_watch::Note(hash);   // and their objectives
        if (ConsumeHordeSilence(hash)) {
            InterlockedIncrement(&hordeSendsSilenced);
            return true;
        }
    }
    return messageHook.call<bool>(message, flags);
}
constexpr size_t kHordeMarkCapacity = 16;
constexpr DWORD kHordeMarkMaxAgeMs = 5000;
HordeMark hordeMarks[kHordeMarkCapacity]{};
std::mutex hordeLock;
volatile LONG swarmHordes = 0, swarmHordeExtras = 0;

// What the post-create hook made of this spawner's arrival: the donor family
// that now stands there. Read once by the epilogue hook of the same
// HandleEvents call (same thread), so the multiplier counts the enemy that
// exists, not the one the level authored. The plugin's own repeats
// (moreEnemiesInside) leave no note: the authored arrival's decision covers them.
struct ReplacementNote { uintptr_t spawner = 0; std::string family; };
thread_local ReplacementNote replacementNote;
void NoteReplacement(uintptr_t spawner, const std::string& family) {
    if (moreEnemiesInside || !spawner) return;
    replacementNote.spawner = spawner;
    replacementNote.family = family;
}

void MarkHordeReplacement(uintptr_t spawner, const std::string& family, uint32_t deathHash) {
    if (!spawner || !HordesActive()) return;
    const auto size = hordeSizes.find(family);
    if (size == hordeSizes.end() || size->second <= 1) return;
    std::lock_guard<std::mutex> guard(hordeLock);
    const DWORD now = GetTickCount();
    size_t slot = 0;
    for (size_t i = 0; i < kHordeMarkCapacity; ++i) {
        if (hordeMarks[i].spawner == spawner) { slot = i; break; }
        if (!hordeMarks[i].spawner) { slot = i; continue; }
        if (hordeMarks[slot].spawner &&
            static_cast<LONG>(hordeMarks[i].tick - hordeMarks[slot].tick) < 0) slot = i;
    }
    hordeMarks[slot] = {spawner, now, size->second, deathHash};
}

// The horde marked on this spawner (size 0 when there is none); the mark is spent.
HordeMark TakeHordeMark(uintptr_t spawner) {
    if (!spawner || !HordesActive()) return {};
    std::lock_guard<std::mutex> guard(hordeLock);
    const DWORD now = GetTickCount();
    for (auto& mark : hordeMarks) {
        if (mark.spawner != spawner) continue;
        HordeMark taken = now - mark.tick <= kHordeMarkMaxAgeMs ? mark : HordeMark{};
        mark = {};
        return taken;
    }
    return {};
}

// The same test without spending the mark.
bool HasHordeMark(uintptr_t spawner) {
    if (!spawner || !HordesActive()) return false;
    std::lock_guard<std::mutex> guard(hordeLock);
    const DWORD now = GetTickCount();
    for (const auto& mark : hordeMarks)
        if (mark.spawner == spawner) return now - mark.tick <= kHordeMarkMaxAgeMs && mark.size > 1;
    return false;
}

volatile LONG moreEnemiesStatusCounts[static_cast<unsigned>(ds2::MoreEnemiesStatus::Count)]{};
volatile LONG moreEnemiesRequested = 0, moreEnemiesCreated = 0;
volatile LONG moreEnemiesInnerEmpty = 0, moreEnemiesRestoreFailed = 0;

struct MoreEnemiesDetail {
    LONG ready;
    unsigned spawner, attributes, tick;
    Guid target;
    unsigned status, wanted, made;
    int authoredCount, authoredMax;
    char family[24];
};
constexpr LONG kMoreEnemiesDetailCapacity = 64;
MoreEnemiesDetail moreEnemiesDetails[kMoreEnemiesDetailCapacity]{};
volatile LONG moreEnemiesDetailCount = 0;

bool WriteBytes(uintptr_t address, const void* input, size_t bytes) {
    SIZE_T done = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
                              input, bytes, &done) && done == bytes;
}

bool Writable(uintptr_t address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & kWritable)) return false;
    const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return address >= reinterpret_cast<uintptr_t>(info.BaseAddress) &&
           bytes <= end - address;
}

void CountMoreEnemies(ds2::MoreEnemiesStatus status) {
    InterlockedIncrement(&moreEnemiesStatusCounts[static_cast<unsigned>(status)]);
}

void RecordMoreEnemiesDetail(uintptr_t spawner, uintptr_t attributes, const Guid& target,
                             ds2::MoreEnemiesStatus status, unsigned wanted, unsigned made,
                             int authoredCount, int authoredMax, const char* family) {
    if (!observeDetails) return;
    const LONG slot = InterlockedIncrement(&moreEnemiesDetailCount) - 1;
    if (slot < 0 || slot >= kMoreEnemiesDetailCapacity) return;
    auto& detail = moreEnemiesDetails[slot];
    detail.spawner = static_cast<unsigned>(spawner);
    detail.attributes = static_cast<unsigned>(attributes);
    detail.tick = GetTickCount();
    detail.target = target;
    detail.status = static_cast<unsigned>(status);
    detail.wanted = wanted;
    detail.made = made;
    detail.authoredCount = authoredCount;
    detail.authoredMax = authoredMax;
    std::snprintf(detail.family, sizeof(detail.family), "%s", family ? family : "unknown");
    InterlockedExchange(&detail.ready, 1);
}

// Spaced-out repeats.
//
// A vent arrival plays a climb-out animation and the vent carries exactly one
// enemy at a time. Dispatching the copies in the same frame puts two enemies in
// one vent: measured over a full session, all nine doubled vent spawners bound
// both copies to the same vent header, and six of those nine pairs were still
// alive after 90 seconds, always both halves together.
//
// The engine solves this for itself. FuncSpawn carries the number of spawns it
// still owes at +0xC8, an armed flag at +0xCC and the id of its retry message at
// +0xD0, and when it cannot deliver now it reschedules through the timer at
// 0x02050E24 with the delay constant at 0x01AAE2C8, 2.0 seconds. Its retry is
// bound to the authored budget, which a repeat by definition exceeds, so it
// cannot carry the extras. Its delay is copied, and the repeat also waits
// until the vent is empty (ventq::Busy below).
//
// Each entry is one copy. The dispatch itself is unchanged: the count is parked
// and restored inside a single call, exactly as the immediate path does it.
struct DeferredRepeat {
    uintptr_t spawner;
    uint32_t attributes, trigger, completion, vtable;
    Guid target;
    int authoredCount;
    DWORD due;
    bool horde;
    uint32_t deathHash;
    bool retire = false;   // last repeat of a pre-parked spawner: let SpawnDone retire it
};
constexpr size_t kDeferredCapacity = 32;
DeferredRepeat deferred[kDeferredCapacity]{};
size_t deferredCount = 0;
std::mutex deferredLock;
unsigned moreEnemiesVentDelayMs = 2000;
// The last spawn of each spawner, ours or the game's own retry (game thread).
// A vent repeat waits until the vent has been quiet this long: a slasher still
// climbing out of a wall vent while the next one came through the same vent
// crashed twice on 08.10. (0x00E93CCF, the wall-exit state machine not linked
// yet; both times spawner 31CAF23B, repeats 109 ms apart).
unsigned moreEnemiesVentGapMs = 4000;
struct SpawnerLast { uintptr_t spawner; DWORD at; };
SpawnerLast spawnerLast[64]{};
unsigned spawnerLastNext = 0;
volatile LONG ventGapWaits = 0;
// Age of a stamp taken with a fresh tick against an older captured now: 0, not 49 days.
DWORD TickAge(DWORD now, DWORD at) { return static_cast<LONG>(now - at) > 0 ? now - at : 0; }
void NoteSpawnerSpawn(uintptr_t spawner) {
    if (!spawner) return;
    for (auto& s : spawnerLast)
        if (s.spawner == spawner) { s.at = GetTickCount(); return; }
    spawnerLast[spawnerLastNext++ % std::size(spawnerLast)] = SpawnerLast{spawner, GetTickCount()};
}
// The stamps are GetTickCount; survival passes its own clock without the paused time, so the
// age is taken on the stamps' clock (18.10.: after a few game overs every vent waited the whole
// paused time, about 24 s, after each spawn).
bool VentQuiet(uintptr_t spawner, DWORD) {
    if (!moreEnemiesVentGapMs) return true;
    const DWORD tick = GetTickCount();
    for (const auto& s : spawnerLast)
        if (s.spawner == spawner) return TickAge(tick, s.at) >= moreEnemiesVentGapMs;
    return true;
}
volatile LONG moreEnemiesDeferred = 0, moreEnemiesDeferredMade = 0, moreEnemiesDeferredLost = 0;

// ---------------------------------------------------------------- vent occupancy
//
// A VentMarker queues the NPCs that want out: DeadSpaceNPCExitVentSM registers in
// its exitVentStandby state (RegisterWaitingNPC 0x012E0A80: 8 places at +0x374, a
// SafePtr to entity+0x10 each, count at +0x3B4; a ninth is refused and the SM
// ignores that) and deregisters when it leaves that state, i.e. when it starts to
// climb. So the queue spaces the starts only; it does not keep a second NPC out
// while the first still climbs. Stasis on a climber keeps it in the vent for many
// seconds, and the repeats that waited a fixed time went in on top of it (15.10.,
// a friend's report: the vent jammed and the room never cleared; 08.10. the same
// overlap crashed at 0x00E93CCF). A repeat now waits until no NPC that came out of
// that vent is still inside it and its queue is empty. A dead or vanished NPC left
// first in a queue blocks every other one for good and is taken out.
namespace ventq {
constexpr uint32_t kMarkerVtableRva = 0x178C5F0, kStandby = 0x374, kStandbyCount = 0x3B4, kStandbyMax = 8;
constexpr uint32_t kSimManagerRva = 0x1C1C0A0, kFindBaseRva = 0x376CA0, kDeregisterRva = 0xEE0AF0;
constexpr uint32_t kVentComponentIndexRva = 0x1CA846A, kVentFlags = 0x44, kHealthCurrent = 0xE8, kTimeMultiplier = 0x84, kEntityGuid = 0x24;
constexpr DWORD kScanMs = 500, kBadHeadMs = 3000, kLongInsideMs = 20000, kArrivalWatchMs = 180000, kRepeatBusyMs = 90000, kInVentAfterMs = 3000;
// VentMarker::DeregisterWaitingNPC: push ecx / push ebp / push esi / mov esi,ecx / mov edx,[esi+3B4h] / xor ebp,ebp / push edi / test edx,edx
const unsigned char kDeregisterBytes[] = {0x51, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x96, 0xB4, 0x03, 0x00, 0x00, 0x33, 0xED, 0x57, 0x85, 0xD2};
// SimManager::FindBase: push esi / mov esi,[esp+8] / mov edx,[esi] / xor eax,eax / push edi / mov edi,ecx / test edx,edx / jnz
const unsigned char kFindBaseBytes[] = {0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x16, 0x33, 0xC0, 0x57, 0x8B, 0xF9, 0x85, 0xD2, 0x75, 0x13};
bool guard = true;   // [DS2Randomizer] VentQueueGuard
bool ready = false, repairReady = false;
struct SpawnerVent { uintptr_t spawner; uint32_t vent; };
struct Arrival { uint32_t actor, vtable, vent; Guid id; DWORD at; bool seenInside, left, longNoted; };
struct Queue { uint32_t vent, head; DWORD headSince, badSince; unsigned peak; bool fullNoted; };
SpawnerVent spawnerVents[64]{};
unsigned spawnerVentNext = 0;
Arrival arrivals[64]{};
unsigned arrivalNext = 0;
Queue queues[32]{};
unsigned queueNext = 0;
DWORD lastScan = 0;
volatile LONG arrivalsSeen = 0, ventsFound = 0, ventsMissing = 0, fullSeen = 0, longInside = 0, badHeads = 0, badHeadsFailed = 0,
              repeatWaits = 0, repeatsDropped = 0, faults = 0;

// a few lines for the log; the worker writes them with the diagnostics
constexpr unsigned kLines = 48, kLineSize = 256;
constexpr LONG kLineBudget = 300;
char lines[kLines][kLineSize];
unsigned lineHead = 0, lineCount = 0;
SRWLOCK lineLock = SRWLOCK_INIT;
volatile LONG linesWritten = 0, linesLost = 0;
void Note(const char* format, ...) {
    if (InterlockedIncrement(&linesWritten) > kLineBudget) { InterlockedIncrement(&linesLost); return; }
    char text[kLineSize];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    AcquireSRWLockExclusive(&lineLock);
    if (lineCount == kLines) { lineHead = (lineHead + 1) % kLines; --lineCount; InterlockedIncrement(&linesLost); }
    std::memcpy(lines[(lineHead + lineCount) % kLines], text, sizeof(text));
    ++lineCount;
    ReleaseSRWLockExclusive(&lineLock);
}

// Base::GetComponent 0x007CF080 by reads alone: [[e+0x48] + 4 * byte [[e+0x44] + index + 0xC]]
uint32_t Component(uint32_t entity, uint32_t indexRva) {
    uint8_t index = 0, slot = 0;
    uint32_t list = 0, table = 0, component = 0;
    if (!entity || !Read(base + indexRva, &index, 1) || !Read(entity + 0x48, &list, 4) || !list || !Read(entity + 0x44, &table, 4) || !table ||
        !Read(table + index + 0xC, &slot, 1) || !Read(list + slot * 4u, &component, 4))
        return 0;
    return component;
}
// DSNPCVentComponent::m_flags bit 0 (SetInVent 0x00FF1F80): 1 inside, 0 out, -1 unknown
int InVent(uint32_t entity) {
    const uint32_t c = Component(entity, kVentComponentIndexRva);
    uint8_t flags = 0;
    if (!c || !Read(c + kVentFlags, &flags, 1)) return -1;
    return flags & 1;
}
bool HealthOf(uint32_t entity, float& v) {
    const uint32_t h = ds2_watchdog_runtime::FindHealthComponent(entity);
    return h && Read(h + kHealthCurrent, &v, 4) && std::isfinite(v);
}
float Health(uint32_t entity) {
    float v = 0.0f;
    return HealthOf(entity, v) ? v : -1.0f;
}
float TimeScale(uint32_t entity) {
    float t = -1.0f;
    return Read(entity + kTimeMultiplier, &t, 4) && std::isfinite(t) ? t : -1.0f;
}
bool IsMarker(uint32_t vent) {
    uint32_t vtable = 0;
    return vent && Read(vent, &vtable, 4) && vtable == base + kMarkerVtableRva;
}
unsigned Count(uint32_t vent) {
    uint32_t n = 0;
    return IsMarker(vent) && Read(vent + kStandbyCount, &n, 4) && n <= kStandbyMax ? n : 0;
}
uint32_t Entry(uint32_t vent, unsigned i) {
    uint32_t p = 0;
    return Read(vent + kStandby + i * 8, &p, 4) && p ? p - 0x10 : 0;
}
bool Alive(const Arrival& a) {
    uint32_t vtable = 0;
    Guid id{};
    return a.actor && Read(a.actor, &vtable, 4) && vtable == a.vtable && Read(a.actor + kEntityGuid, id.data(), 16) && id == a.id;
}

__declspec(noinline) uint32_t FindRaw(const uint32_t* guid) {
    __try {
        uint32_t sim = 0;
        if (!Read(base + kSimManagerRva, &sim, 4) || !sim) return 0;
        using Find = uint32_t(__thiscall*)(uint32_t, const uint32_t*, uint32_t);
        return reinterpret_cast<Find>(base + kFindBaseRva)(sim, guid, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return 0;
    }
}
__declspec(noinline) int DeregisterRaw(uint32_t vent, uint32_t entity) {
    __try {
        using Fn = bool(__thiscall*)(uint32_t, uint32_t);
        return reinterpret_cast<Fn>(base + kDeregisterRva)(vent, entity) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return -1;
    }
}

// The vent an authored vent arrival comes out of: its target's vent GUID
// (generated catalog), found by SimManager::FindBase. Game thread only.
uint32_t VentFor(uintptr_t spawner, uint32_t attributes) {
    for (auto& s : spawnerVents)
        if (s.spawner == spawner && spawner) {
            if (IsMarker(s.vent)) return s.vent;
            s.spawner = 0;
            break;
        }
    Guid target{};
    if (!attributes || !Read(attributes + kSpawnAttrTargetGuid, target.data(), 16)) return 0;
    const ds2::VentGuidRow* row = nullptr;
    for (const auto& r : ds2::kVentGuids)
        if (r.target == target) { row = &r; break; }
    if (!row) return 0;
    const uint32_t vent = FindRaw(row->vent.data());
    if (!IsMarker(vent)) {
        if (InterlockedIncrement(&ventsMissing) <= 8)
            Note("vent_queue: spawner %08X target %08X-%08X has vent %08X-%08X-%08X-%08X, FindBase gave %08X (not a VentMarker)", static_cast<unsigned>(spawner),
                 target[0], target[1], row->vent[0], row->vent[1], row->vent[2], row->vent[3], vent);
        return 0;
    }
    InterlockedIncrement(&ventsFound);
    spawnerVents[spawnerVentNext++ % std::size(spawnerVents)] = SpawnerVent{spawner, vent};
    return vent;
}
void Track(uint32_t vent) {
    for (const auto& q : queues)
        if (q.vent == vent) return;
    queues[queueNext++ % std::size(queues)] = Queue{vent};
}
// From the post-create observer (game thread): every authored arrival, ours included.
void NoteArrival(uintptr_t spawner, uint32_t attributes, uint32_t actor) {
    if (!ready || !actor) return;
    const uint32_t vent = VentFor(spawner, attributes);
    if (!vent) return;
    Arrival a{};
    a.actor = actor;
    a.vent = vent;
    a.at = GetTickCount();
    if (!Read(actor, &a.vtable, 4) || !Read(actor + kEntityGuid, a.id.data(), 16)) return;
    arrivals[arrivalNext++ % std::size(arrivals)] = a;
    InterlockedIncrement(&arrivalsSeen);
    Track(vent);
}
bool StillInside(Arrival& a, DWORD now) {
    if (!a.actor || a.left) return false;
    if (TickAge(now, a.at) > kArrivalWatchMs || !Alive(a)) { a.actor = 0; return false; }
    const int in = InVent(a.actor);
    if (in == 1) {
        a.seenInside = true;
        float hp = 0.0f;
        return !HealthOf(a.actor, hp) || hp > 0.0f;
    }
    // ExitVentSM sets the flag a moment after the spawn; out after that means out for good
    if (in == 0 && (a.seenInside || TickAge(now, a.at) > kInVentAfterMs)) a.left = true;
    return !a.left && in != 0;
}
// True while the spawner's vent has an NPC of its own inside or a queue. Game thread.
bool Busy(uintptr_t spawner, uint32_t* ventOut = nullptr) {
    if (!ready || !guard) return false;
    uint32_t attributes = 0;
    if (!Read(spawner + kFuncSpawnAttributes, &attributes, 4)) return false;
    const uint32_t vent = VentFor(spawner, attributes);
    if (ventOut) *ventOut = vent;
    if (!vent) return false;
    if (Count(vent)) return true;
    const DWORD now = GetTickCount();
    for (auto& a : arrivals)
        if (a.vent == vent && StillInside(a, now)) return true;
    return false;
}

// Game thread, twice a second: the queues of the vents seen, and the NPCs still inside.
void Scan(DWORD now) {
    if (!ready || now - lastScan < kScanMs) return;
    lastScan = now;
    for (auto& a : arrivals) {
        if (!StillInside(a, now) || a.longNoted || TickAge(now, a.at) < kLongInsideMs) continue;
        a.longNoted = true;
        InterlockedIncrement(&longInside);
        const unsigned n = Count(a.vent);
        Note("vent_queue: NPC %08X still inside vent %08X %lu s after its arrival: health %.0f, time scale %.2f (below 1 = stasis), queue %u of 8, first %08X",
             a.actor, a.vent, static_cast<unsigned long>((now - a.at) / 1000), Health(a.actor), TimeScale(a.actor), n, n ? Entry(a.vent, 0) : 0u);
    }
    for (auto& q : queues) {
        if (!q.vent) continue;
        if (!IsMarker(q.vent)) { q = Queue{}; continue; }
        const unsigned n = Count(q.vent);
        if (n > q.peak) q.peak = n;
        if (!n) { q.head = 0; q.badSince = 0; continue; }
        if (n >= kStandbyMax && !q.fullNoted) {
            q.fullNoted = true;
            InterlockedIncrement(&fullSeen);
            Note("vent_queue: vent %08X has all 8 places taken; a ninth NPC would wait inside for good", q.vent);
        }
        const uint32_t head = Entry(q.vent, 0);
        if (head != q.head) { q.head = head; q.headSince = now; q.badSince = 0; }
        uint32_t vtable = 0;
        const bool gone = !head || !Read(head, &vtable, 4) || !vtable;
        float hp = 1.0f;
        const bool dead = !gone && HealthOf(head, hp) && hp <= 0.0f;
        if (!gone && !dead) { q.badSince = 0; continue; }
        if (!q.badSince) { q.badSince = now; continue; }
        if (now - q.badSince < kBadHeadMs || !repairReady || !guard) continue;
        const int r = DeregisterRaw(q.vent, head);
        InterlockedIncrement(r == 1 ? &badHeads : &badHeadsFailed);
        Note("vent_queue: vent %08X kept a %s NPC %08X first for %lu ms while %u wait(s); DeregisterWaitingNPC %s, %u left", q.vent, gone ? "vanished" : "dead", head,
             static_cast<unsigned long>(now - q.badSince), n - 1, r == 1 ? "took it out" : r == 0 ? "did not find it" : "FAULTED", Count(q.vent));
        q.badSince = 0;
    }
}

void Install(FILE* log) {
    unsigned char d[sizeof(kDeregisterBytes)]{}, f[sizeof(kFindBaseBytes)]{};
    const bool findOk = Read(base + kFindBaseRva, f, sizeof(f)) && std::memcmp(f, kFindBaseBytes, sizeof(f)) == 0;
    const bool deregisterOk = Read(base + kDeregisterRva, d, sizeof(d)) && std::memcmp(d, kDeregisterBytes, sizeof(d)) == 0;
    ready = findOk;
    repairReady = findOk && deregisterOk;
    std::fprintf(log, "vent_queue guard=%d: FindBase %s, DeregisterWaitingNPC %s; vent repeats %s, dead first NPCs %s\n", guard ? 1 : 0,
                 findOk ? "ok" : "MISMATCH", deregisterOk ? "ok" : "MISMATCH", ready && guard ? "wait for an empty vent" : "keep the old timing only",
                 repairReady && guard ? "are taken out of the queue" : "stay");
}
void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG sum = arrivalsSeen + ventsMissing * 3 + fullSeen * 5 + longInside * 7 + badHeads * 11 + badHeadsFailed * 13 + repeatWaits / 25 + repeatsDropped * 17 + faults * 19;
    if (sum != last) {
        last = sum;
        std::fprintf(log, "vent_queue arrivals=%ld vents found=%ld missing=%ld | queue full=%ld long_inside=%ld | dead_first taken_out=%ld failed=%ld | "
                          "repeat waits=%ld dropped=%ld | faults=%ld lines_lost=%ld\n",
                     arrivalsSeen, ventsFound, ventsMissing, fullSeen, longInside, badHeads, badHeadsFailed, repeatWaits, repeatsDropped, faults, linesLost);
    }
    char copy[kLines][kLineSize];
    unsigned count = 0;
    AcquireSRWLockExclusive(&lineLock);
    for (unsigned i = 0; i < lineCount; ++i) std::memcpy(copy[i], lines[(lineHead + i) % kLines], kLineSize);
    count = lineCount;
    lineHead = lineCount = 0;
    ReleaseSRWLockExclusive(&lineLock);
    for (unsigned i = 0; i < count; ++i) std::fprintf(log, "%s\n", copy[i]);
}
}  // namespace ventq

bool QueueRepeat(const DeferredRepeat& entry) {
    std::lock_guard<std::mutex> guard(deferredLock);
    if (deferredCount >= kDeferredCapacity) return false;
    deferred[deferredCount++] = entry;
    InterlockedIncrement(&moreEnemiesDeferred);
    return true;
}

void RecheckStreamGraph();   // runtime_stream_probe.inc, included later

// ---------------------------------------------------------------- extra spawns
//
// A designer's addition to a spawner: when it fires, it brings along the
// enemies listed for it in plugins\DS2ExtraSpawns.tsv (src/extra_spawns.h),
// each placed at the spot and facing the placement readout printed. The
// spawner is driven once more per extra, exactly like a MoreEnemies repeat
// (count parked, completion hidden), with the donor forced in OnLookup. Once
// the actor exists it is moved the way FuncSpawn itself moves a spawn onto
// its location entity (0x00ECB38D): rotation rows through vtable slot +0x24,
// position through slot +0x20, then the movement component (0x00876E00) is
// told the same through 0x00F63540 and 0x00F638A0. An extra never counts
// for the room: its death message is swallowed like a horde copy's.
ds2::ExtraSpawnRules extraSpawns;
// Every FuncSpawn the game constructs (ctor 0x00EA5A40, `this` in ecx), so an
// area rule has a spawner to drive before anything in the level has fired.
// The GUID at +0x24 is not written yet when the constructor starts, so the
// game thread records it shortly after (NameKnownSpawners) and every later use
// requires the same GUID at the same address. The list is emptied when the
// level's stream service retires, which frees every spawner of that level.
constexpr uint32_t kFuncSpawnCtorRva = 0x00AA5A40;
constexpr unsigned kKnownSpawnerCapacity = 256;
safetyhook::MidHook spawnerCtorHook;
struct KnownSpawner { uint32_t pointer; Guid guid; unsigned attempts; };
KnownSpawner knownSpawners[kKnownSpawnerCapacity]{};
unsigned knownSpawnerCount = 0;   // total constructed since the last reset, under knownSpawnersLock
std::mutex knownSpawnersLock;
volatile LONG knownSpawnersPending = 0;   // constructed, not named yet
DWORD knownSpawnersLastNaming = 0;
volatile LONG knownSpawnersNamed = 0, knownSpawnersUnnamed = 0, knownSpawnersMoved = 0, knownSpawnersRetired = 0, knownSpawnersCleared = 0;
volatile LONG knownSpawnersClearAsked = 0, knownSpawnersFloorAsked = 0;
// Under knownSpawnersLock: forget the old level's spawners.
void ClearKnownSpawnersLocked() {
    for (auto& entry : knownSpawners) entry = KnownSpawner{};
    knownSpawnerCount = 0;
    InterlockedExchange(&knownSpawnersPending, 0);
    InterlockedExchange(&knownSpawnersFloorAsked, 1);
    InterlockedIncrement(&knownSpawnersCleared);
}
void OnFuncSpawnConstructed(SafetyHookContext& ctx) {
    std::lock_guard<std::mutex> guard(knownSpawnersLock);
    // the new level's first spawner: the old ones go before it is added
    if (InterlockedExchange(&knownSpawnersClearAsked, 0)) ClearKnownSpawnersLocked();
    knownSpawners[knownSpawnerCount++ % kKnownSpawnerCapacity] = KnownSpawner{static_cast<uint32_t>(ctx.ecx), Guid{}, 0};
    InterlockedIncrement(&knownSpawnersPending);
}
// The spawner history (recentSpawners) is kept for the editor; area rules
// only take entries newer than this sequence.
uint64_t recentSpawnerFloor = 0;
// From OnStreamServiceRetired: every spawner of the old level is gone. That hook
// runs inside the engine's stream code, so it takes no lock and only asks; the
// game thread clears before it next touches the list. A checkpoint restart on
// 08.10. hung in the loading screen right after this hook took two locks here.
void ClearKnownSpawners() {
    InterlockedExchange(&moreEnemiesBudgetReset, 1);
    InterlockedExchange(&knownSpawnersClearAsked, 1);
}
// Game thread only. The history floor moves once the list was cleared, wherever that happened.
void ApplyKnownSpawnersClear() {
    if (InterlockedExchange(&knownSpawnersClearAsked, 0)) {
        std::lock_guard<std::mutex> guard(knownSpawnersLock);
        ClearKnownSpawnersLocked();
    }
    if (!InterlockedExchange(&knownSpawnersFloorAsked, 0)) return;
    const std::lock_guard<std::mutex> guard(spawnerToolsMutex);
    recentSpawnerFloor = recentSpawnerSequence;
}
// Game thread, at most every 50 ms: give fresh entries their GUID. An entry
// that still reads no FuncSpawn identity after 64 tries is dropped.
void NameKnownSpawners() {
    ApplyKnownSpawnersClear();
    if (InterlockedCompareExchange(&knownSpawnersPending, 0, 0) <= 0) return;
    const DWORD now = GetTickCount();
    if (now - knownSpawnersLastNaming < 50) return;
    knownSpawnersLastNaming = now;
    std::lock_guard<std::mutex> guard(knownSpawnersLock);
    LONG pending = 0;
    unsigned work = 0;
    for (auto& entry : knownSpawners) {
        if (!entry.pointer || entry.guid != Guid{}) continue;
        if (++work > 16) { ++pending; continue; }
        uint32_t vtable = 0;
        Guid guid{};
        if (Read(entry.pointer, &vtable, 4) && vtable == base + kFuncSpawnVtableRva &&
            Read(entry.pointer + 0x24, guid.data(), sizeof(guid)) && guid != Guid{}) {
            entry.guid = guid;
            InterlockedIncrement(&knownSpawnersNamed);
        } else if (++entry.attempts >= 64) {
            entry = KnownSpawner{};
            InterlockedIncrement(&knownSpawnersUnnamed);
        } else {
            ++pending;
        }
    }
    InterlockedExchange(&knownSpawnersPending, pending);
}

// Still the spawner that was recorded: same vtable, same GUID, and not used up.
// A spawner whose count reached its authored maximum was retired by SpawnDone
// and sits on the engine's deletion list.
bool KnownSpawnerUsable(uint32_t pointer, const Guid& recorded) {
    uint32_t vtable = 0, attributes = 0;
    Guid guid{};
    if (!Read(pointer, &vtable, 4) || vtable != base + kFuncSpawnVtableRva ||
        !Read(pointer + 0x24, guid.data(), sizeof(guid)) || guid != recorded) {
        InterlockedIncrement(&knownSpawnersMoved);
        return false;
    }
    int count = 0, max = -1;
    if (!Read(pointer + kFuncSpawnAttributes, &attributes, 4) || !attributes ||
        !Read(pointer + kFuncSpawnCount, &count, 4) || !Read(attributes + kSpawnAttrMaxSpawn, &max, 4) ||
        (max > 0 && count >= max)) {
        InterlockedIncrement(&knownSpawnersRetired);
        return false;
    }
    return true;
}
// Area rules (`near:<m>` rows): fired from the tick, not from a spawner.
ds2::ExtraAreaRules extraAreas;
std::vector<bool> extraAreaFired;
// A rule that found no usable spawner stays unfired and tries again after
// kExtraAreaRetryMs; one spawner search costs up to 96 lookups on the game thread.
std::vector<DWORD> extraAreaRetryAt;
std::vector<unsigned> extraAreaMisses;
constexpr DWORD kExtraAreaRetryMs = 5000;
DWORD extraAreaLastPoll = 0;
volatile LONG extraAreasFired = 0, extraAreasMade = 0, extraAreasNoSpawner = 0, extraAreasNoDonor = 0, extraAreasRetried = 0;
std::mutex extraLock;
std::string extraSpawnsPath;
bool extraSpawnsEnabled = true;
FILETIME extraSpawnsWritten{};
bool extraSpawnsLoaded = false;
volatile LONG extraSpawnsMade = 0, extraSpawnsFailed = 0, extraSpawnsPlaced = 0, extraSpawnsNoDonor = 0;

void LoadExtraSpawns(FILE* log) {
    if (!extraSpawnsEnabled || extraSpawnsPath.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExA(extraSpawnsPath.c_str(), GetFileExInfoStandard, &info)) {
        if (!extraSpawnsLoaded) { extraSpawnsLoaded = true; std::fprintf(log, "extra_spawns file=missing (%s)\n", extraSpawnsPath.c_str()); }
        return;
    }
    if (extraSpawnsLoaded && info.ftLastWriteTime.dwLowDateTime == extraSpawnsWritten.dwLowDateTime &&
        info.ftLastWriteTime.dwHighDateTime == extraSpawnsWritten.dwHighDateTime) return;
    extraSpawnsWritten = info.ftLastWriteTime;
    extraSpawnsLoaded = true;
    std::ifstream file(extraSpawnsPath, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    ds2::ExtraSpawnRules parsed;
    ds2::ExtraAreaRules areas;
    if (!ds2::parseExtraSpawns(text, parsed, &areas)) {
        std::fprintf(log, "extra_spawns result=rejected (header or a row is malformed); previous rules kept\n");
        return;
    }
    {
        std::lock_guard<std::mutex> guard(extraLock);
        extraSpawns = std::move(parsed);
        extraAreas = std::move(areas);
        extraAreaFired.assign(extraAreas.size(), false);
        extraAreaRetryAt.assign(extraAreas.size(), 0);
        extraAreaMisses.assign(extraAreas.size(), 0);
    }
    std::fprintf(log, "extra_spawns result=accepted spawners=%zu rows=%zu areas=%zu\n", extraSpawns.size(), ds2::extraSpawnCount(extraSpawns), extraAreas.size());
}

// An extra is the donor forced in OnLookup; without the selection hook the
// spawner would build its own enemy and that would be counted as the extra.
bool HasExtraSpawns(uintptr_t spawner) {
    if (!extraSpawnsEnabled || !selectionHook) return false;
    uint32_t attributes = 0;
    Guid guid{};
    if (!Read(spawner + kFuncSpawnAttributes, &attributes, 4) || !ReadSpawnerIdentity(static_cast<uint32_t>(spawner), attributes, guid)) return false;
    std::lock_guard<std::mutex> guard(extraLock);
    return extraSpawns.count(guid) != 0;
}

// The pool record for a rule's kind: a family name or a pool package name.
size_t DonorForKind(const std::string& kind) {
    for (const auto& row : ::ds2::kGlobalTargets) {
        if (kind != row.package) continue;
        const auto it = std::lower_bound(pool.begin(), pool.end(), row.guid,
            [](const auto& record, const Guid& guid) { return record.guid < guid; });
        if (it != pool.end() && it->guid == row.guid) return static_cast<size_t>(it - pool.begin());
    }
    for (size_t i = 0; i < pool.size(); ++i)
        if (pool[i].privateTemplate && pool[i].family == kind && pool[i].variantName.empty() && !pool[i].passive) return i;
    return SIZE_MAX;
}

bool ApplyPlacement(uint32_t actor, const ds2::ExtraSpawnRule& rule) {
    if (!actor || !rule.hasPosition) return false;
    uint32_t vtable = 0;
    if (!Read(actor, &vtable, 4) || vtable < base + 0x16A1000 || vtable > base + 0x1880000) return false;
    __declspec(align(16)) float rotation[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
    __declspec(align(16)) float position[4] = {rule.position[0], rule.position[1], rule.position[2], 1.0f};
    if (rule.hasYaw) {
        const double radians = rule.yaw * 3.14159265358979 / 180.0;
        const float s = static_cast<float>(std::sin(radians)), c = static_cast<float>(std::cos(radians));
        rotation[0] = c;  rotation[2] = -s;
        rotation[8] = s;  rotation[10] = c;
    }
    using SetVector = void (__thiscall*)(void*, const float*);
    using Component = void* (__cdecl*)(void*);
    using MoveRotate = void (__thiscall*)(void*, const float*, int);
    using MovePosition = void (__thiscall*)(void*, const float*);
    uint32_t setRotation = 0, setPosition = 0;
    if (!Read(vtable + 0x24, &setRotation, 4) || !Read(vtable + 0x20, &setPosition, 4) || !setRotation || !setPosition) return false;
    if (rule.hasYaw) reinterpret_cast<SetVector>(setRotation)(reinterpret_cast<void*>(actor), rotation);
    reinterpret_cast<SetVector>(setPosition)(reinterpret_cast<void*>(actor), position);
    void* movement = reinterpret_cast<Component>(base + 0x476E00)(reinterpret_cast<void*>(actor));
    if (movement) {
        if (rule.hasYaw) reinterpret_cast<MoveRotate>(base + 0xB63540)(movement, rotation, 0);
        reinterpret_cast<MovePosition>(base + 0xB638A0)(movement, position);
    }
    return true;
}

// Called from the SpawnDone hook, after the spawner and its attributes were
// verified. Drives the spawner once per extra and places the result.
unsigned RunExtraSpawns(uintptr_t spawner, uint32_t attributes, uint32_t trigger, uint32_t completion,
                    uint32_t handleEvents, const Guid& target, int authoredCount) {
    Guid guid{};
    if (!ReadSpawnerIdentity(static_cast<uint32_t>(spawner), attributes, guid)) return 0;
    std::vector<ds2::ExtraSpawnRule> rules;
    {
        std::lock_guard<std::mutex> guard(extraLock);
        const auto it = extraSpawns.find(guid);
        if (it == extraSpawns.end()) return 0;
        rules = it->second;
    }
    if (!Writable(spawner + kFuncSpawnCount, sizeof(int))) return 0;
    const bool hideCompletion = completion != 0;
    const auto source = std::lower_bound(pool.begin(), pool.end(), target,
        [](const auto& record, const Guid& g) { return record.guid < g; });
    const uint32_t deathHash = source != pool.end() && source->guid == target ? source->deathMessage.hash : 0;
    uint32_t message[4]{trigger, 0, 0, 0};
    using HandleEventsFn = void(__thiscall*)(void*, void*);
    const auto dispatch = reinterpret_cast<HandleEventsFn>(handleEvents);
    const uint32_t hidden = 0;
    if (deathHash && !silenceAvailable) return 0;
    if (hideCompletion && !WriteBytes(attributes + kSpawnAttrCompletionMessage, &hidden, sizeof(hidden))) return 0;
    unsigned madeCopies = 0;
    for (size_t r = 0; r < rules.size(); ++r) {
        const auto& rule = rules[r];
        const Guid salt{static_cast<uint32_t>(r), rule.count, rule.chance, 0x45585452u};
        if (rule.chance < 100 && ::ds2::stableHash(settings.seed, guid, salt, 0x45585452u) % 100 >= rule.chance) continue;
        const size_t donor = DonorForKind(rule.kind);
        if (donor == SIZE_MAX) { InterlockedIncrement(&extraSpawnsNoDonor); continue; }
        for (unsigned copy = 0; copy < rule.count; ++copy) {
            int parked = kMoreEnemiesCountSentinel;
            if (!WriteBytes(spawner + kFuncSpawnCount, &parked, sizeof(parked))) break;
            forcedDonorIndex = donor;
            extraSpawnActive = true;
            lastCreatedActor = 0;
            moreEnemiesInside = true;
            dispatch(reinterpret_cast<void*>(spawner), message);
            moreEnemiesInside = false;
            extraSpawnActive = false;
            forcedDonorIndex = SIZE_MAX;
            int after = 0;
            const bool made = Read(spawner + kFuncSpawnCount, &after, 4) && after != parked && lastCreatedActor;
            if (!made) { InterlockedIncrement(&extraSpawnsFailed); continue; }
            InterlockedIncrement(&extraSpawnsMade);
            ++madeCopies;
            if (rule.arrival == ds2::ExtraArrival::Direct && ApplyPlacement(lastCreatedActor, rule)) InterlockedIncrement(&extraSpawnsPlaced);
        }
    }
    TakeHordeMark(spawner);   // the extras marked the spawner too; not a horde
    if (!WriteBytes(spawner + kFuncSpawnCount, &authoredCount, sizeof(authoredCount))) InterlockedIncrement(&moreEnemiesRestoreFailed);
    if (hideCompletion && !WriteBytes(attributes + kSpawnAttrCompletionMessage, &completion, sizeof(completion))) InterlockedIncrement(&moreEnemiesRestoreFailed);
    if (madeCopies && deathHash) ArmHordeSilence(deathHash, madeCopies);
    return madeCopies;
}

// Vent spawners with an authored maximum (19.09.2026).
//
// Every deferred vent repeat of a spawner with authored_max=1 was lost while
// the same repeats of a max=-1 vent spawner arrived on time (session log of
// 19.09.: 18 lost, all max=1; 12 made, all max=-1). SpawnDone (0x00EBA610)
// increments the count and, once it reaches the maximum, retires the spawner:
// 0x007568B0 unregisters it and puts it on the deletion list at 0x0204CDA4.
// The immediate repeats run inside that same call and never notice; two
// seconds later the object is freed and the deferred repeat fails its
// identity check.
//
// So for an arrival that would retire a vent spawner the count is parked
// before SpawnDone runs (hook at 0x00ECB416, the call site), SpawnDone sends
// its completion message as always but sees no maximum, and the retirement is
// owed: the last deferred repeat runs with count = max-1 so its own SpawnDone
// retires the spawner the retail way, and whenever that cannot happen (no
// repeats after all, a refused repeat) SpawnDone is called once here with the
// completion parked and no entity, which is the same retirement without a
// second message.
safetyhook::MidHook beforeSpawnDoneHook;
bool spawnDoneVerified = false;
thread_local struct { uintptr_t spawner = 0; int authored = 0; } prePark;
volatile LONG ventRetirePlanned = 0, ventRetiredNatural = 0, ventRetiredSynthetic = 0,
              ventRetireLost = 0, ventRetireFailed = 0;

using SpawnDoneFn = void(__thiscall*)(void*, void*);

// count = max-1, completion parked, SpawnDone once without an entity: the
// message branch is skipped when the completion is 0 and the entity is only
// read there. Leaves the count at max and the spawner retired.
bool RetireVentSpawner(uintptr_t spawner, uint32_t attributes, int max) {
    if (!spawnDoneVerified || max <= 0) return false;
    uint32_t vtable = 0, completion = 0;
    if (!Read(spawner, &vtable, 4) || vtable != base + kFuncSpawnVtableRva ||
        !Writable(spawner + kFuncSpawnCount, sizeof(int)) ||
        !Writable(attributes + kSpawnAttrCompletionMessage, sizeof(uint32_t)) ||
        !Read(attributes + kSpawnAttrCompletionMessage, &completion, 4))
        return false;
    const uint32_t hidden = 0;
    const int before = max - 1;
    if (!WriteBytes(attributes + kSpawnAttrCompletionMessage, &hidden, sizeof(hidden))) return false;
    if (!WriteBytes(spawner + kFuncSpawnCount, &before, sizeof(before))) {
        if (!WriteBytes(attributes + kSpawnAttrCompletionMessage, &completion, sizeof(completion)))
            InterlockedIncrement(&moreEnemiesRestoreFailed);
        return false;
    }
    const bool wasInside = moreEnemiesInside;
    moreEnemiesInside = true;
    reinterpret_cast<SpawnDoneFn>(base + kFuncSpawnSpawnDoneRva)(reinterpret_cast<void*>(spawner), nullptr);
    moreEnemiesInside = wasInside;
    if (!WriteBytes(attributes + kSpawnAttrCompletionMessage, &completion, sizeof(completion)))
        InterlockedIncrement(&moreEnemiesRestoreFailed);
    int after = 0;
    return Read(spawner + kFuncSpawnCount, &after, 4) && after == max;
}

// Retirement owed by the post hook's own scope: paid on every exit unless the
// deferred queue took it over.
struct RetireGuard {
    uintptr_t spawner; uint32_t attributes; int max; bool owed;
    ~RetireGuard() {
        if (!owed) return;
        InterlockedIncrement(RetireVentSpawner(spawner, attributes, max) ? &ventRetiredSynthetic : &ventRetireFailed);
    }
};

// An arrival OnBeforeSpawnDone parked that leaves OnAuthoredSpawnComplete
// before the RetireGuard exists: pay the retirement SpawnDone skipped. When
// even that fails the count is put back to the maximum, so the sentinel is
// never left behind and the spawner cannot fire again.
void PayOwedRetirement(uintptr_t spawner) {
    if (!spawner || prePark.spawner != spawner) return;
    prePark.spawner = 0;
    const int max = prePark.authored + 1;
    uint32_t attributes = 0;
    if (Read(spawner + kFuncSpawnAttributes, &attributes, 4) && attributes && RetireVentSpawner(spawner, attributes, max)) {
        InterlockedIncrement(&ventRetiredSynthetic);
        return;
    }
    InterlockedIncrement(&ventRetireFailed);
    uint32_t vtable = 0;
    if (!Read(spawner, &vtable, 4) || vtable != base + kFuncSpawnVtableRva ||
        !Writable(spawner + kFuncSpawnCount, sizeof(int)) || !WriteBytes(spawner + kFuncSpawnCount, &max, sizeof(max)))
        InterlockedIncrement(&moreEnemiesRestoreFailed);
}

// 0x00ECB416, the authored arrival about to call SpawnDone; edi is the
// FuncSpawn. The plugin's own repeats park the count themselves and are skipped.
void OnBeforeSpawnDone(SafetyHookContext& ctx) {
    if (moreEnemiesInside || !moreEnemiesVentDelayMs) return;
    if (CoopClient()) { InterlockedIncrement(&coopClientSkips); return; }
    if (!moreEnemies.enabled && !HordesActive()) return;
    // Only an arrival that OnAuthoredSpawnComplete will repeat is parked. With
    // MoreEnemies off and no horde or extra spawn for it, that hook returns
    // early and a parked count was never restored: the spawner kept a count of
    // about -2^30 and was never retired.
    if (!moreEnemies.enabled && !HasHordeMark(ctx.edi) && !HasExtraSpawns(ctx.edi)) return;
    const uintptr_t spawner = ctx.edi;
    uint32_t vtable = 0, attributes = 0;
    int count = 0, max = 0;
    Guid target{};
    if (!spawner || !Read(spawner, &vtable, 4) || vtable != base + kFuncSpawnVtableRva ||
        !Read(spawner + kFuncSpawnAttributes, &attributes, 4) || !attributes ||
        !Read(spawner + kFuncSpawnCount, &count, 4) ||
        !Read(attributes + kSpawnAttrMaxSpawn, &max, 4) ||
        !Read(attributes + kSpawnAttrTargetGuid, target.data(), 16))
        return;
    // HandleEvents only runs while count < max, so this arrival retires the
    // spawner exactly when count + 1 == max
    if (max == -1 || count + 1 != max) return;
    const auto found = std::lower_bound(pool.begin(), pool.end(), target,
        [](const auto& record, const Guid& guid) { return record.guid < guid; });
    if (found == pool.end() || found->guid != target || !ds2::ventContext(found->context)) return;
    if (!Writable(spawner + kFuncSpawnCount, sizeof(int))) return;
    const int parked = kMoreEnemiesCountSentinel;
    if (!WriteBytes(spawner + kFuncSpawnCount, &parked, sizeof(parked))) return;
    prePark.spawner = spawner;
    prePark.authored = count;
    InterlockedIncrement(&ventRetirePlanned);
}

// One forced spawn through `spawner`, for an area rule. The same steps as a
// RunExtraSpawns copy: count parked, completion hidden, donor forced, the
// actor moved to the rule's spot.
bool DriveForcedSpawn(uintptr_t spawner, const ds2::ExtraSpawnRule& rule, size_t donor) {
    uint32_t attributes = 0, vtable = 0, handleEvents = 0, trigger = 0, completion = 0;
    if (!Read(spawner, &vtable, 4) || vtable != base + kFuncSpawnVtableRva ||
        !Read(vtable + 4, &handleEvents, 4) || handleEvents != base + kFuncSpawnHandleEventsRva ||
        !Read(spawner + kFuncSpawnAttributes, &attributes, 4) || !attributes ||
        !Read(attributes + kSpawnAttrTrigger, &trigger, 4) || !trigger ||
        !Read(attributes + kSpawnAttrCompletionMessage, &completion, 4) ||
        !Writable(spawner + kFuncSpawnCount, sizeof(int))) return false;
    int authored = 0;
    if (!Read(spawner + kFuncSpawnCount, &authored, 4)) return false;
    const bool hideCompletion = completion != 0;
    const uint32_t hidden = 0;
    if (hideCompletion && !WriteBytes(attributes + kSpawnAttrCompletionMessage, &hidden, sizeof(hidden))) return false;
    uint32_t message[4]{trigger, 0, 0, 0};
    using HandleEventsFn = void(__thiscall*)(void*, void*);
    int parked = kMoreEnemiesCountSentinel;
    bool made = false;
    if (WriteBytes(spawner + kFuncSpawnCount, &parked, sizeof(parked))) {
        forcedDonorIndex = donor;
        extraSpawnActive = true;
        lastCreatedActor = 0;
        moreEnemiesInside = true;
        reinterpret_cast<HandleEventsFn>(handleEvents)(reinterpret_cast<void*>(spawner), message);
        moreEnemiesInside = false;
        extraSpawnActive = false;
        forcedDonorIndex = SIZE_MAX;
        int after = 0;
        made = Read(spawner + kFuncSpawnCount, &after, 4) && after != parked && lastCreatedActor;
    }
    TakeHordeMark(spawner);
    if (!WriteBytes(spawner + kFuncSpawnCount, &authored, sizeof(authored))) InterlockedIncrement(&moreEnemiesRestoreFailed);
    if (hideCompletion && !WriteBytes(attributes + kSpawnAttrCompletionMessage, &completion, sizeof(completion)))
        InterlockedIncrement(&moreEnemiesRestoreFailed);
    if (made && rule.arrival == ds2::ExtraArrival::Direct && ApplyPlacement(lastCreatedActor, rule)) InterlockedIncrement(&extraSpawnsPlaced);
    return made;
}

// Engines whose forced enemies did not stay where they were put (the survival
// director's placement check): the ch04 arena's newest prop spawner was an
// elevator's null spawner, and four of six zombies stood in the elevator behind
// a locked door (09.10.). Skipped by the picker until the run ends.
std::vector<uintptr_t> avoidedEngines;
// Engines that refuse before the lookup (ch04 arena 10.10.: the gate keeper's
// spawner refused 120 forced spawns in a row and wave 2 stalled) rest for 30 s
// after three refusals in a row.
struct EngineRest { uintptr_t spawner; unsigned refusals; DWORD until; };
std::vector<EngineRest> engineRests;
bool EngineResting(uintptr_t spawner) {
    const DWORD now = GetTickCount();
    for (const auto& r : engineRests)
        if (r.spawner == spawner && r.until && static_cast<LONG>(r.until - now) > 0) return true;
    return false;
}
// True when this refusal sends the engine to rest.
bool NoteEngineRefusal(uintptr_t spawner) {
    for (auto& r : engineRests) {
        if (r.spawner != spawner) continue;
        if (++r.refusals < 3) return false;
        r.refusals = 0;
        r.until = GetTickCount() + 30000;
        return true;
    }
    engineRests.push_back(EngineRest{spawner, 1, 0});
    return false;
}
void NoteEngineMade(uintptr_t spawner) {
    for (auto& r : engineRests) if (r.spawner == spawner) r.refusals = 0;
}
bool EngineAvoided(uintptr_t spawner) {
    return std::find(avoidedEngines.begin(), avoidedEngines.end(), spawner) != avoidedEngines.end() || EngineResting(spawner);
}

// A live spawner whose own target still resolves: HandleEvents looks the
// target up before OnLookup may swap the packet, so a spawner from a room
// whose streams are gone would create nothing. Newest first. `prefer` (a
// survival arena: the spawner stands inside it) outranks every target class.
uintptr_t SpawnerForAreaRule(bool (*prefer)(uintptr_t) = nullptr) {
    ApplyKnownSpawnersClear();
    uint32_t manager = 0;
    if (!Read(base + 0x1C1C0A0, &manager, 4) || !manager) return 0;
    // Each candidate comes with the GUID recorded for it, so a pointer whose
    // object was freed or replaced is refused before anything is called on it.
    uint32_t candidates[96]{};
    Guid recorded[96]{};
    unsigned count = 0;
    // newest constructed spawners first
    {
        std::lock_guard<std::mutex> guard(knownSpawnersLock);
        const unsigned total = knownSpawnerCount;
        const unsigned limit = total < kKnownSpawnerCapacity ? total : kKnownSpawnerCapacity;
        for (unsigned back = 1; back <= limit && count < 48; ++back) {
            const auto& entry = knownSpawners[(total - back) % kKnownSpawnerCapacity];
            if (!entry.pointer || entry.guid == Guid{}) continue;
            bool dup = false;
            for (unsigned i = 0; i < count; ++i) if (candidates[i] == entry.pointer) dup = true;
            if (!dup) { recorded[count] = entry.guid; candidates[count++] = entry.pointer; }
        }
    }
    {
        const std::lock_guard<std::mutex> guard(spawnerToolsMutex);
        // recentSpawners is a ring by sequence; take the newest entries
        uint64_t best = 0;
        for (const auto& entry : recentSpawners) if (entry.sequence > best) best = entry.sequence;
        for (uint64_t seq = best; seq > recentSpawnerFloor && count < std::size(candidates); --seq) {
            const auto& entry = recentSpawners[(seq - 1) % recentSpawners.size()];
            if (entry.sequence != seq || !entry.pointer || entry.source == Guid{}) continue;
            bool dup = false;
            for (unsigned i = 0; i < count; ++i) if (candidates[i] == entry.pointer) dup = true;
            if (!dup) { recorded[count] = entry.source; candidates[count++] = entry.pointer; }
            if (best - seq > 512) break;
        }
    }
    // Which spawner is the safest engine for a forced enemy? One whose own
    // target is no enemy at all (a prop, a pickup: nothing of its arrival
    // logic applies, the packet is simply swapped) or an enemy that arrives
    // plainly. A vent spawner is the last resort: its vent contract would try
    // to rebind the forced donor's headers. Newest first within a class.
    using Lookup = void* (__thiscall*)(void*, ds2::native::SpawnInfo32*, const Guid*, unsigned);
    uintptr_t best = 0;
    int bestScore = -1;
    for (unsigned i = 0; i < count; ++i) {
        const uintptr_t spawner = candidates[i];
        uint32_t vtable = 0, attributes = 0;
        Guid target{};
        if (!KnownSpawnerUsable(candidates[i], recorded[i]) || EngineAvoided(spawner)) continue;
        if (!Read(spawner, &vtable, 4) || vtable != base + kFuncSpawnVtableRva ||
            !Read(spawner + kFuncSpawnAttributes, &attributes, 4) || !attributes ||
            !Read(attributes + kSpawnAttrTargetGuid, target.data(), 16) || target == Guid{}) continue;
        ds2::native::SpawnInfo32 info{};
        reinterpret_cast<Lookup>(base + 0x376440)(reinterpret_cast<void*>(manager), &info, &target, 0);
        if (!info.record || !info.attributes) continue;
        const auto found = std::lower_bound(pool.begin(), pool.end(), target,
            [](const auto& record, const Guid& guid) { return record.guid < guid; });
        int score = 3;   // not an enemy the catalog knows: a prop or the like
        if (found != pool.end() && found->guid == target)
            score = ds2::ventContext(found->context) ? 0 : (ds2::directContext(found->context) ? 2 : 1);
        if (prefer && prefer(spawner)) score += 10;
        if (score > bestScore) { bestScore = score; best = spawner; }
        if (score == (prefer ? 13 : 3)) break;
    }
    return best;
}

void ResetExtraAreas() {
    std::lock_guard<std::mutex> guard(extraLock);
    std::fill(extraAreaFired.begin(), extraAreaFired.end(), false);
    std::fill(extraAreaRetryAt.begin(), extraAreaRetryAt.end(), 0);
    std::fill(extraAreaMisses.begin(), extraAreaMisses.end(), 0);
}

// Game thread, from the tick: an area rule fires once when the player is
// within its radius of the spot.
void ExtraAreaTick() {
    if (!extraSpawnsEnabled || !selectionHook) return;
    NameKnownSpawners();
    const DWORD now = GetTickCount();
    if (now - extraAreaLastPoll < 250) return;
    extraAreaLastPoll = now;
    std::vector<std::pair<size_t, ds2::ExtraAreaRule>> due;
    float position[3]{}, forward[3]{};
    {
        std::lock_guard<std::mutex> guard(extraLock);
        if (extraAreas.empty()) return;
        if (!placement::PlayerTransform(position, forward)) return;
        for (size_t i = 0; i < extraAreas.size(); ++i) {
            if (extraAreaFired[i]) continue;
            if (extraAreaRetryAt[i] && static_cast<LONG>(now - extraAreaRetryAt[i]) < 0) continue;
            const auto& r = extraAreas[i].rule;
            const float dx = r.position[0] - position[0], dy = r.position[1] - position[1], dz = r.position[2] - position[2];
            if (dx * dx + dy * dy + dz * dz <= extraAreas[i].radius * extraAreas[i].radius) due.emplace_back(i, extraAreas[i]);
        }
    }
    // At most one spawner search per pass: when it finds nothing, every other
    // due rule would find nothing too and just waits for its retry.
    bool searched = false, spawnerFound = false;
    uintptr_t spawner = 0;
    auto markFired = [](size_t index) {
        std::lock_guard<std::mutex> guard(extraLock);
        if (index < extraAreaFired.size()) extraAreaFired[index] = true;
    };
    for (const auto& item : due) {
        const auto& rule = item.second.rule;
        char text[256]{};
        const size_t donor = DonorForKind(rule.kind);
        if (donor == SIZE_MAX) {
            // the pool does not change during a session, so this never retries
            markFired(item.first);
            InterlockedIncrement(&extraAreasNoDonor);
            std::snprintf(text, sizeof(text), "extra_area kind=%s at (%.1f %.1f %.1f): no donor in the pool", rule.kind.c_str(), rule.position[0], rule.position[1], rule.position[2]);
            placement::Queue(text);
            continue;
        }
        if (!searched) {
            searched = true;
            spawner = SpawnerForAreaRule();
            spawnerFound = spawner != 0;
        }
        if (!spawnerFound) {
            unsigned misses = 0;
            {
                std::lock_guard<std::mutex> guard(extraLock);
                if (item.first < extraAreaRetryAt.size()) {
                    extraAreaRetryAt[item.first] = (now + kExtraAreaRetryMs) | 1;
                    misses = ++extraAreaMisses[item.first];
                }
            }
            InterlockedIncrement(misses <= 1 ? &extraAreasNoSpawner : &extraAreasRetried);
            // said once per rule and level, the retries only count
            if (misses == 1) {
                std::snprintf(text, sizeof(text), "extra_area kind=%s at (%.1f %.1f %.1f): no live spawner with a resolvable target; retrying every %lu s", rule.kind.c_str(), rule.position[0], rule.position[1], rule.position[2], kExtraAreaRetryMs / 1000);
                placement::Queue(text);
            }
            continue;
        }
        markFired(item.first);
        InterlockedIncrement(&extraAreasFired);
        unsigned made = 0;
        for (unsigned copy = 0; copy < rule.count; ++copy)
            if (DriveForcedSpawn(spawner, rule, donor)) ++made;
        InterlockedExchangeAdd(&extraAreasMade, static_cast<LONG>(made));
        Guid sid{};
        Read(spawner + 0x24, sid.data(), 16);
        std::snprintf(text, sizeof(text), "extra_area kind=%s x%u at (%.1f %.1f %.1f) radius=%.1f via spawner=%08X-%08X made=%u", rule.kind.c_str(), rule.count,
                      rule.position[0], rule.position[1], rule.position[2], item.second.radius, sid[0], sid[1], made);
        placement::Queue(text);
    }
}

// One repeat per tick at most, and only when its time has come. This is the
// plugin's only periodic call on the game thread, so the stream graph recheck,
// the placement key and the area rules ride along with it.
void DrainDeferredRepeats() {
    if (survivalMachineTick) survivalMachineTick();
    RecheckStreamGraph();
    placement::Poll();
    ExtraAreaTick();
    ventq::Scan(GetTickCount());
    if ((!moreEnemies.enabled && !HordesActive()) || moreEnemiesInside) return;
    if (InterlockedCompareExchange(&moreEnemiesDeferred, 0, 0) ==
        InterlockedCompareExchange(&moreEnemiesDeferredMade, 0, 0) +
        InterlockedCompareExchange(&moreEnemiesDeferredLost, 0, 0)) return;
    DeferredRepeat entry{};
    {
        std::lock_guard<std::mutex> guard(deferredLock);
        if (!deferredCount) return;
        const DWORD now = GetTickCount();
        // this runs on every state machine update; a waiting queue is looked at ten times a second
        static DWORD lastPick = 0;
        if (now - lastPick < 100) return;
        lastPick = now;
        size_t pick = kDeferredCapacity;
        for (size_t i = 0; i < deferredCount; ++i) {
            if (static_cast<LONG>(now - deferred[i].due) < 0) continue;
            if (!VentQuiet(deferred[i].spawner, now)) { InterlockedIncrement(&ventGapWaits); continue; }
            uint32_t vent = 0;
            if (ventq::Busy(deferred[i].spawner, &vent)) {
                InterlockedIncrement(&ventq::repeatWaits);
                if (now - deferred[i].due < ventq::kRepeatBusyMs) continue;
                // the vent never emptied: this copy is given up rather than pushed in
                InterlockedIncrement(&ventq::repeatsDropped);
                InterlockedIncrement(&moreEnemiesDeferredLost);
                if (deferred[i].retire) InterlockedIncrement(&ventRetireLost);
                ventq::Note("vent_queue: a repeat of spawner %08X dropped, vent %08X not empty for %lu s", static_cast<unsigned>(deferred[i].spawner), vent,
                            static_cast<unsigned long>((now - deferred[i].due) / 1000));
                deferred[i] = deferred[--deferredCount];
                return;
            }
            pick = i;
            break;
        }
        if (pick == kDeferredCapacity) return;
        entry = deferred[pick];
        deferred[pick] = deferred[--deferredCount];
    }

    // The object may be gone, recycled or hooked by something else since the
    // repeat was queued, so everything the immediate path checks is checked
    // again here, against the values recorded then.
    uint32_t vtable = 0, attributes = 0, handleEvents = 0, trigger = 0;
    Guid target{};
    int authoredCount = 0;
    if (!Read(entry.spawner, &vtable, 4) || vtable != entry.vtable ||
        vtable != base + kFuncSpawnVtableRva ||
        !Read(vtable + 4, &handleEvents, 4) ||
        handleEvents != base + kFuncSpawnHandleEventsRva ||
        !Read(entry.spawner + kFuncSpawnAttributes, &attributes, 4) ||
        attributes != entry.attributes ||
        !Read(attributes + kSpawnAttrTrigger, &trigger, 4) || trigger != entry.trigger ||
        !Read(attributes + kSpawnAttrTargetGuid, target.data(), 16) || target != entry.target ||
        !Read(entry.spawner + kFuncSpawnCount, &authoredCount, 4) ||
        !Writable(entry.spawner + kFuncSpawnCount, sizeof(int))) {
        InterlockedIncrement(&moreEnemiesDeferredLost);
        if (entry.retire) InterlockedIncrement(&ventRetireLost);
        return;
    }
    const bool hideCompletion = moreEnemies.suppressCompletion && entry.completion != 0;
    if (hideCompletion && !Writable(attributes + kSpawnAttrCompletionMessage, sizeof(uint32_t))) {
        InterlockedIncrement(&moreEnemiesDeferredLost);
        if (entry.retire) InterlockedIncrement(&ventRetireLost);
        return;
    }
    int max = -1;
    if (entry.retire && (!Read(attributes + kSpawnAttrMaxSpawn, &max, 4) || max <= 0)) {
        InterlockedIncrement(&moreEnemiesDeferredLost);
        InterlockedIncrement(&ventRetireLost);
        return;
    }
    const uint32_t hidden = 0;
    if (hideCompletion && !WriteBytes(attributes + kSpawnAttrCompletionMessage, &hidden, sizeof(hidden))) {
        InterlockedIncrement(&moreEnemiesDeferredLost);
        if (entry.retire) InterlockedIncrement(&ventRetireLost);
        return;
    }

    uint32_t message[4]{trigger, 0, 0, 0};
    using HandleEventsFn = void(__thiscall*)(void*, void*);
    // the last repeat of a pre-parked spawner retires it through its own
    // SpawnDone: count max-1 -> max
    int parked = entry.retire ? max - 1 : kMoreEnemiesCountSentinel;
    bool made = false;
    moreEnemiesInside = true;
    hordeCopy = entry.horde;
    if (WriteBytes(entry.spawner + kFuncSpawnCount, &parked, sizeof(parked))) {
        reinterpret_cast<HandleEventsFn>(handleEvents)(
            reinterpret_cast<void*>(entry.spawner), message);
        int after = 0;
        made = Read(entry.spawner + kFuncSpawnCount, &after, 4) && after != parked;
    }
    hordeCopy = false;
    moreEnemiesInside = false;
    if (entry.retire) {
        if (made) InterlockedIncrement(&ventRetiredNatural);
        else InterlockedIncrement(RetireVentSpawner(entry.spawner, attributes, max) ? &ventRetiredSynthetic : &ventRetireFailed);
        authoredCount = max;   // what the spawner holds now, retired
    }
    // the repeat's own arrival marked the spawner again; that mark is spent
    TakeHordeMark(entry.spawner);
    if (made) {
        if (entry.horde) InterlockedIncrement(&swarmHordeExtras);
        if (entry.deathHash) ArmHordeSilenceCopy(entry.deathHash);
    }
    if (!WriteBytes(entry.spawner + kFuncSpawnCount, &authoredCount, sizeof(authoredCount)))
        InterlockedIncrement(&moreEnemiesRestoreFailed);
    if (hideCompletion &&
        !WriteBytes(attributes + kSpawnAttrCompletionMessage, &entry.completion, sizeof(entry.completion)))
        InterlockedIncrement(&moreEnemiesRestoreFailed);
    InterlockedIncrement(made ? &moreEnemiesDeferredMade : &moreEnemiesDeferredLost);
    if (made) InterlockedIncrement(&moreEnemiesCreated);
}

// Every authored arrival that actually produced an entity passes here.
void OnAuthoredSpawnComplete(SafetyHookContext& ctx) {
    if (CoopClient()) { InterlockedIncrement(&coopClientSkips); return; }   // nothing was parked for it either
    if (moreEnemiesInside) {
        if (moreEnemies.enabled) CountMoreEnemies(ds2::MoreEnemiesStatus::Reentrant);
        return;
    }
    if (InterlockedExchange(&moreEnemiesBudgetReset, 0)) moreEnemiesBudget = {};
    const HordeMark hordeMark = TakeHordeMark(ctx.edi);
    const unsigned hordeSize = hordeMark.size;
    std::string replacementFamily;
    if (replacementNote.spawner == ctx.edi) replacementFamily = replacementNote.family;
    replacementNote = ReplacementNote{};
    const bool horde = hordeSize > 1;
    const bool extras = HasExtraSpawns(ctx.edi);
    if (!moreEnemies.enabled && !horde && !extras) {
        // Parked anyway (the mark or the rules changed in between): the
        // retirement SpawnDone skipped is still owed.
        PayOwedRetirement(ctx.edi);
        return;
    }

    const uintptr_t spawner = ctx.edi;
    uint32_t attributes = 0, vtable = 0, handleEvents = 0;
    int authoredCount = 0, authoredMax = 0;
    uint32_t trigger = 0, completion = 0;
    Guid target{};
    if (!spawner || !Read(spawner, &vtable, 4) ||
        !Read(spawner + kFuncSpawnAttributes, &attributes, 4) || !attributes ||
        !Read(spawner + kFuncSpawnCount, &authoredCount, 4) ||
        !Read(attributes + kSpawnAttrTrigger, &trigger, 4) || !trigger ||
        !Read(attributes + kSpawnAttrMaxSpawn, &authoredMax, 4) ||
        !Read(attributes + kSpawnAttrCompletionMessage, &completion, 4) ||
        !Read(attributes + kSpawnAttrTargetGuid, target.data(), 16)) {
        CountMoreEnemies(ds2::MoreEnemiesStatus::AttributesUnreadable);
        PayOwedRetirement(spawner);
        return;
    }
    // A pre-parked arrival (OnBeforeSpawnDone): the count read above is the
    // sentinel + 1. What SpawnDone would have left is authored + 1 (= max),
    // and the retirement it skipped is owed: by the last vent repeat, or by
    // the guard on any other way out of here.
    bool retireOwed = false;
    if (prePark.spawner == spawner) {
        prePark.spawner = 0;
        authoredCount = prePark.authored + 1;
        if (!WriteBytes(spawner + kFuncSpawnCount, &authoredCount, sizeof(authoredCount)))
            InterlockedIncrement(&moreEnemiesRestoreFailed);
        retireOwed = true;
    }
    RetireGuard retireGuard{spawner, attributes, authoredCount, retireOwed};
    // The dispatch slot must still be the retail HandleEvents. Anything else
    // means a foreign hook owns the vtable and the repeat is refused.
    if (vtable != base + kFuncSpawnVtableRva || !Read(vtable + 4, &handleEvents, 4) ||
        handleEvents != base + kFuncSpawnHandleEventsRva) {
        CountMoreEnemies(ds2::MoreEnemiesStatus::VtableMismatch);
        RecordMoreEnemiesDetail(spawner, attributes, target,
                                ds2::MoreEnemiesStatus::VtableMismatch, 0, 0,
                                authoredCount, authoredMax, nullptr);
        return;
    }

    // No extras or copies next to a scripted corpse: its spawner may loop.
    if (IsCorpseTemplate(target)) {
        CountMoreEnemies(ds2::MoreEnemiesStatus::CorpseTemplate);
        RecordMoreEnemiesDetail(spawner, attributes, target, ds2::MoreEnemiesStatus::CorpseTemplate, 0, 0,
                                authoredCount, authoredMax, nullptr);
        return;
    }
    const unsigned customCopies = extras ? RunExtraSpawns(spawner, attributes, trigger, completion, handleEvents, target, authoredCount) : 0;
    if (!moreEnemies.enabled && !horde) return;

    const auto found = std::lower_bound(pool.begin(), pool.end(), target,
        [](const auto& record, const Guid& guid) { return record.guid < guid; });
    if (found == pool.end() || found->guid != target) {
        CountMoreEnemies(ds2::MoreEnemiesStatus::UnknownTarget);
        RecordMoreEnemiesDetail(spawner, attributes, target,
                                ds2::MoreEnemiesStatus::UnknownTarget, 0, 0,
                                authoredCount, authoredMax, nullptr);
        return;
    }

    // A horde wants its size in total and MoreEnemies never stacks on it.
    // Neither the budget nor the per-trigger cap applies to a horde: six
    // swarmers are the point.
    ds2::MoreEnemiesDecision decision{ds2::MoreEnemiesStatus::Multiplied, 0};
    // 12.10.: the saferoom scene's one slasher came out four times; the three
    // copies were in no scene and roamed a room the level never puts one in.
    if (moreEnemiesSkipScene && IsSceneActor(target))
        decision = ds2::MoreEnemiesDecision{ds2::MoreEnemiesStatus::SceneActor, 0};
    else if (horde) decision.extras = hordeSize - 1;
    else if (moreEnemies.enabled)
        decision = ds2::decideMoreEnemies(moreEnemies, moreEnemiesFamilyTotals,
                                          found->family, found->context,
                                          moreEnemiesBudget, replacementFamily);
    if (horde) InterlockedIncrement(&swarmHordes);
    const char* family = horde ? "SwarmHorde" : found->family.c_str();
    // the message this slot's death is counted by, whoever ends up dying first
    const uint32_t deathHash = horde ? hordeMark.deathHash : found->deathMessage.hash;
    if (deathHash && !silenceAvailable) return;
    const unsigned originalPass = customCopies && deathHash == found->deathMessage.hash ? 0u : 1u;
    if (decision.status != ds2::MoreEnemiesStatus::Multiplied || !decision.extras) {
        CountMoreEnemies(decision.status);
        RecordMoreEnemiesDetail(spawner, attributes, target, decision.status, 0, 0,
                                authoredCount, authoredMax, family);
        return;
    }

    // Repeating SpawnDone would resend the authored per-spawn completion
    // message once per copy. Level logic counts those, so the message is
    // parked for the duration of the repeats. If that write is impossible the
    // multiplier does nothing rather than let a door count wrong.
    const bool hideCompletion = moreEnemies.suppressCompletion && completion != 0;
    if (hideCompletion &&
        !Writable(attributes + kSpawnAttrCompletionMessage, sizeof(completion))) {
        CountMoreEnemies(ds2::MoreEnemiesStatus::CompletionNotWritable);
        RecordMoreEnemiesDetail(spawner, attributes, target,
                                ds2::MoreEnemiesStatus::CompletionNotWritable, 0, 0,
                                authoredCount, authoredMax, family);
        return;
    }
    if (!Writable(spawner + kFuncSpawnCount, sizeof(int))) {
        CountMoreEnemies(ds2::MoreEnemiesStatus::CountNotWritable);
        RecordMoreEnemiesDetail(spawner, attributes, target,
                                ds2::MoreEnemiesStatus::CountNotWritable, 0, 0,
                                authoredCount, authoredMax, family);
        return;
    }

    const uint32_t hidden = 0;
    if (hideCompletion && !WriteBytes(attributes + kSpawnAttrCompletionMessage, &hidden, sizeof(hidden))) {
        CountMoreEnemies(ds2::MoreEnemiesStatus::CompletionNotWritable);
        return;
    }

    // The engine's own retry builds exactly this message: id, a zero byte and a
    // zero dword. Sixteen bytes so a longer read can never reach past it.
    uint32_t message[4]{trigger, 0, 0, 0};
    using HandleEventsFn = void(__thiscall*)(void*, void*);
    const auto dispatch = reinterpret_cast<HandleEventsFn>(handleEvents);

    // A vent cannot take two arrivals at once, so those repeats are queued
    // with the engine's own retry spacing instead of being dispatched here.
    if (moreEnemiesVentDelayMs && ds2::ventContext(found->context)) {
        unsigned queued = 0;
        for (unsigned copy = 0; copy < decision.extras; ++copy) {
            DeferredRepeat entry{spawner, attributes, trigger, completion, vtable, target,
                                 authoredCount,
                                 GetTickCount() + moreEnemiesVentDelayMs * (copy + 1), horde, deathHash};
            entry.retire = retireOwed && copy + 1 == decision.extras;
            if (!QueueRepeat(entry)) break;
            ++queued;
        }
        // one death of this slot counts; each copy registers itself when made
        if (queued && deathHash && originalPass) ArmHordeSilence(deathHash, 0);
        // all copies queued, the flagged last one among them: the queue owns
        // the retirement now. Otherwise the guard retires on the way out and
        // the queued copies find no spawner (queue full: 32 entries).
        if (retireOwed && queued == decision.extras) retireGuard.owed = false;
        if (hideCompletion)
            WriteBytes(attributes + kSpawnAttrCompletionMessage, &completion, sizeof(completion));
        CountMoreEnemies(ds2::MoreEnemiesStatus::Multiplied);
        InterlockedExchangeAdd(&moreEnemiesRequested, static_cast<LONG>(decision.extras));
        RecordMoreEnemiesDetail(spawner, attributes, target, ds2::MoreEnemiesStatus::Multiplied,
                                decision.extras, queued, authoredCount, authoredMax,
                                family);
        return;
    }

    unsigned made = 0;
    moreEnemiesInside = true;
    hordeCopy = horde;
    for (unsigned copy = 0; copy < decision.extras; ++copy) {
        int parked = kMoreEnemiesCountSentinel;
        if (!WriteBytes(spawner + kFuncSpawnCount, &parked, sizeof(parked))) break;
        dispatch(reinterpret_cast<void*>(spawner), message);
        int after = 0;
        if (!Read(spawner + kFuncSpawnCount, &after, 4)) break;
        // SpawnDone is the only writer of this field, so an unchanged sentinel
        // means the repeat was refused before it created anything.
        if (after == parked) { InterlockedIncrement(&moreEnemiesInnerEmpty); break; }
        ++made;
    }
    hordeCopy = false;
    moreEnemiesInside = false;
    // every repeat marked the spawner again; those marks are spent
    TakeHordeMark(spawner);
    if (horde) InterlockedExchangeAdd(&swarmHordeExtras, static_cast<LONG>(made));
    if (made && deathHash) ArmHordeSilence(deathHash, made, originalPass);

    if (!WriteBytes(spawner + kFuncSpawnCount, &authoredCount, sizeof(authoredCount)))
        InterlockedIncrement(&moreEnemiesRestoreFailed);
    if (hideCompletion &&
        !WriteBytes(attributes + kSpawnAttrCompletionMessage, &completion, sizeof(completion)))
        InterlockedIncrement(&moreEnemiesRestoreFailed);

    CountMoreEnemies(ds2::MoreEnemiesStatus::Multiplied);
    InterlockedExchangeAdd(&moreEnemiesRequested, static_cast<LONG>(decision.extras));
    InterlockedExchangeAdd(&moreEnemiesCreated, static_cast<LONG>(made));
    RecordMoreEnemiesDetail(spawner, attributes, target, ds2::MoreEnemiesStatus::Multiplied,
                            decision.extras, made, authoredCount, authoredMax,
                            family);
}

void LoadMoreEnemiesSettings(const std::string& ini) {
    moreEnemies.enabled = ConfigInt(ini, "DS2Randomizer", "MoreEnemies", 0, 0, 1) != 0;
    moreEnemiesSkipScene = ConfigInt(ini, "DS2Randomizer", "MoreEnemiesSkipScripted", 1, 0, 1) != 0;
    moreEnemies.vents = ConfigInt(ini, "DS2Randomizer", "MoreEnemiesVents", 0, 0, 1) != 0;
    moreEnemiesVentDelayMs = static_cast<unsigned>(
        ConfigInt(ini, "DS2Randomizer", "MoreEnemiesVentDelayMs", 2000, 0, 30000));
    moreEnemiesVentGapMs = static_cast<unsigned>(
        ConfigInt(ini, "DS2Randomizer", "MoreEnemiesVentGapMs", 4000, 0, 30000));
    ventq::guard = ConfigInt(ini, "DS2Randomizer", "VentQueueGuard", 1, 0, 1) != 0;
    moreEnemies.suppressCompletion =
        ConfigInt(ini, "DS2Randomizer", "MoreEnemiesHideCompletion", 1, 0, 1) != 0;
    moreEnemies.crossFamily =
        ConfigInt(ini, "DS2Randomizer", "MoreEnemiesCrossFamily", 1, 0, 1) != 0;
    moreEnemies.perTrigger = static_cast<unsigned>(ConfigInt(ini, "DS2Randomizer",
        "MoreEnemiesMaxPerTrigger", 3, 1, static_cast<int>(ds2::kMoreEnemiesMaxPerTrigger)));
    moreEnemies.budgetPerSecond = static_cast<unsigned>(ConfigInt(ini, "DS2Randomizer",
        "MoreEnemiesPerSecond", 48, 0, static_cast<int>(ds2::kMoreEnemiesMaxPerSecond)));   // 16 arrivals x 3 extras
    moreEnemies.defaultTotal = static_cast<unsigned>(ConfigInt(ini, "MoreEnemies",
        "Default", 1, 1, static_cast<int>(ds2::kMoreEnemiesMaxPerTrigger)));
    extraSpawnsEnabled = ConfigInt(ini, "DS2Randomizer", "ExtraSpawns", 1, 0, 1) != 0;
    swarmHorde = static_cast<unsigned>(ConfigInt(ini, "DS2Randomizer", "SwarmHorde", 6, 1, 12));
    packHorde = static_cast<unsigned>(ConfigInt(ini, "DS2Randomizer", "PackHorde", 4, 1, 12));
    // Hordes run with or without MoreEnemies (19.09.2026): a swarmer or pack
    // boy that replaces another kind of enemy arrives as a horde, never a
    // swarmer standing in for a swarmer (the room authored those by the
    // dozen), and MoreEnemies never stacks on top of a horde.
    hordeSizes.clear();
    if (swarmHorde > 1) hordeSizes["SwarmEnemy"] = swarmHorde;
    if (packHorde > 1) hordeSizes["PackEnemy"] = packHorde;
    for (const auto& record : pool) {
        if (record.deathMessage.hash) hordeSilences.try_emplace(record.deathMessage.hash);
        if (moreEnemiesFamilyTotals.count(record.family)) continue;
        const int total = ConfigInt(ini, "MoreEnemies", record.family.c_str(),
                                    static_cast<int>(moreEnemies.defaultTotal), 1,
                                    static_cast<int>(ds2::kMoreEnemiesMaxPerTrigger));
        moreEnemiesFamilyTotals.emplace(record.family, static_cast<unsigned>(total));
    }
}

bool InstallMoreEnemies(FILE* log) {
    // 0x00ACB411: call SimManager finalize, push created, call SpawnDone, then
    // the epilogue. All three displacements are module relative constants, so
    // the whole run is checked byte for byte.
    static const unsigned char kSite[] = {
        0xE8,0x8A,0x9E,0x8A,0xFF, 0x53, 0x8B,0xCF, 0xE8,0xF2,0xF1,0xFE,0xFF,
        0x5F,0x5E,0x5D,0x5B, 0x83,0xC4,0x44, 0xC2,0x04,0x00
    };
    unsigned char actual[sizeof(kSite)]{};
    if (!Read(base + 0x00ACB411, actual, sizeof(actual)) ||
        std::memcmp(actual, kSite, sizeof(actual)) != 0) {
        std::fprintf(log, "more_enemies signature mismatch at %08X; no hook installed\n",
                     static_cast<unsigned>(base + 0x00ACB411));
        return false;
    }
    uint32_t handleEvents = 0;
    if (!Read(base + kFuncSpawnVtableRva + 4, &handleEvents, 4) ||
        handleEvents != base + kFuncSpawnHandleEventsRva) {
        std::fprintf(log, "more_enemies vtable slot %08X holds %08X, expected %08X; "
                          "no hook installed\n",
                     static_cast<unsigned>(base + kFuncSpawnVtableRva + 4),
                     handleEvents, static_cast<unsigned>(base + kFuncSpawnHandleEventsRva));
        return false;
    }
    moreEnemiesHook = safetyhook::create_mid(
        reinterpret_cast<void*>(base + kMoreEnemiesHookRva), OnAuthoredSpawnComplete);
    if (!moreEnemiesHook) {
        std::fprintf(log, "more_enemies hook creation failed at %08X\n",
                     static_cast<unsigned>(base + kMoreEnemiesHookRva));
        return false;
    }
    // FuncSpawn::SpawnDone: sub esp,18h / push esi / mov esi,ecx / push edi /
    // mov edi,[esi+0Ch] / cmp dword ptr [edi+20h],0
    static const unsigned char kSpawnDone[] = {0x83,0xEC,0x18,0x56,0x8B,0xF1,0x57,0x8B,0x7E,0x0C,0x83,0x7F,0x20,0x00};
    unsigned char spawnDone[sizeof(kSpawnDone)]{};
    spawnDoneVerified = Read(base + kFuncSpawnSpawnDoneRva, spawnDone, sizeof(spawnDone)) &&
                        std::memcmp(spawnDone, kSpawnDone, sizeof(kSpawnDone)) == 0;
    // The call site is inside the run verified above; installed after the
    // epilogue hook so the two patches (..41D and 41E..) never overlap.
    if (spawnDoneVerified && moreEnemiesVentDelayMs)
        beforeSpawnDoneHook = safetyhook::create_mid(
            reinterpret_cast<void*>(base + kBeforeSpawnDoneRva), OnBeforeSpawnDone);
    if (extraSpawnsEnabled || survivalWanted) {
        // push ebp / mov ebp,esp / and esp,-16 / push ecx / mov eax,[ebp+8] / push ebx / push esi / push edi / push eax / mov esi,ecx / call
        static const unsigned char kCtor[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x51,0x8B,0x45,0x08,0x53,0x56,0x57,0x50,0x8B,0xF1,0xE8};
        unsigned char ctorBytes[sizeof(kCtor)]{};
        if (Read(base + kFuncSpawnCtorRva, ctorBytes, sizeof(ctorBytes)) && std::memcmp(ctorBytes, kCtor, sizeof(kCtor)) == 0)
            spawnerCtorHook = safetyhook::create_mid(reinterpret_cast<void*>(base + kFuncSpawnCtorRva), OnFuncSpawnConstructed);
        std::fprintf(log, "extra_spawns spawner registry %s at %08X (area rules can fire before the level's first spawn)\n",
                     spawnerCtorHook ? "hooked" : "OFF - constructor signature mismatch", static_cast<unsigned>(base + kFuncSpawnCtorRva));
    }
    std::fprintf(log, "more_enemies vent retirement hook %s at %08X (SpawnDone %08X %s)\n",
                 beforeSpawnDoneHook ? "installed" : "OFF",
                 static_cast<unsigned>(base + kBeforeSpawnDoneRva),
                 static_cast<unsigned>(base + kFuncSpawnSpawnDoneRva),
                 spawnDoneVerified ? "matches" : "MISMATCH");
    // survival reads the MP maps' vent windows off the same dispatch
    if (survivalWanted) { vent_gate::Watch(); objective_watch::Watch(); }
    if (HordesActive() || moreEnemies.enabled || extraSpawnsEnabled || survivalWanted) {
        // push ecx / mov edx,[esp+8] / mov ecx,[edx] / push esi / mov esi,ecx / push edi / mov edi,[0x0204CD8C]
        static const unsigned char kDispatch[] = {0x51,0x8B,0x54,0x24,0x08,0x8B,0x0A,0x56,0x8B,0xF1,0x57,0x8B,0x3D,0x8C,0xCD,0x04,0x02};
        // Other plugins wrap the same dispatch; the shared lock in hook_site.h
        // lets either order chain (see frame_cap).
        silenceAvailable = hook_site::Install(log, "death message dispatch", base + 0x356C60, kDispatch, sizeof(kDispatch),
                                              reinterpret_cast<void*>(OnDispatchMessage), messageHook);
        if (!silenceAvailable) {
            std::fprintf(log, "death message dispatch not hooked; hordes and death-counted repeats off, and survival's objective count and vent windows are blind\n");
            DisableHordes();
        }
    }
    std::fprintf(log, "more_enemies hook installed at %08X; repeats authored arrivals only\n",
                 static_cast<unsigned>(base + kMoreEnemiesHookRva));
    return true;
}

// The game-thread work runs inside the watchdog's state machine hook: an access violation in
// the survival tick, the stream graph, placement, the vent queue or a repeat would take the
// game's update down with it. Caught, counted and logged; the per-call flags are put back.
volatile LONG gameThreadFaults = 0;
DWORD gameThreadFaultCode = 0;
int GameThreadFilter(DWORD code) {
    gameThreadFaultCode = code;
    return EXCEPTION_EXECUTE_HANDLER;
}
__declspec(noinline) bool DrainGuarded() {
    __try {
        DrainDeferredRepeats();
        return true;
    } __except (GameThreadFilter(GetExceptionCode())) {
        return false;
    }
}
void DrainDeferredRepeatsSafe() {
    if (DrainGuarded()) return;
    moreEnemiesInside = false;
    hordeCopy = false;
    const LONG n = InterlockedIncrement(&gameThreadFaults);
    if (n <= 5)
        ventq::Note("game thread work FAULT %08X (survival tick, stream graph, placement, vents, repeats): caught, the game goes on (%ld so far)",
                    static_cast<unsigned>(gameThreadFaultCode), n);
}

void WriteMoreEnemiesConfig(FILE* log) {
    ds2_watchdog_runtime::gameThreadWork = DrainDeferredRepeatsSafe;
    ventq::Install(log);
    std::fprintf(log, "more_enemies skip_scripted=%d scene_actors=%u (a spawn a scene names as its actor is not multiplied)\n",
                 moreEnemiesSkipScene ? 1 : 0, static_cast<unsigned>(std::size(ds2::kSceneActors)));
    std::fprintf(log, "more_enemies enabled=%d vents=%d hide_completion=%d cross_family=%d "
                      "max_per_trigger=%u per_second=%u default=%u swarm_horde=%u pack_horde=%u hordes_active=%d\n",
                 moreEnemies.enabled, moreEnemies.vents, moreEnemies.suppressCompletion, moreEnemies.crossFamily,
                 moreEnemies.perTrigger, moreEnemies.budgetPerSecond, moreEnemies.defaultTotal,
                 swarmHorde, packHorde, HordesActive() ? 1 : 0);
    for (const auto& entry : moreEnemiesFamilyTotals) {
        const bool guarded = ds2::moreEnemiesProtectedFamily(entry.first);
        if (entry.second <= 1 && !guarded) continue;
        std::fprintf(log, "more_enemies family=%s total=%u protected=%d\n",
                     entry.first.c_str(), guarded ? 1u : entry.second, guarded);
    }
}

void WriteMoreEnemiesDiagnostics(FILE* log, LONG& flushed) {
    static LONG lastTotal = -1;
    ventq::WriteDiagnostics(log);
    LoadExtraSpawns(log);
    static LONG lastExtras = -1;
    const LONG extrasNow = InterlockedCompareExchange(&extraSpawnsMade, 0, 0) + InterlockedCompareExchange(&extraSpawnsFailed, 0, 0) +
                           InterlockedCompareExchange(&extraSpawnsNoDonor, 0, 0);
    if (extrasNow != lastExtras) {
        std::fprintf(log, "extra_spawns made=%ld placed=%ld failed=%ld no_donor=%ld\n", InterlockedCompareExchange(&extraSpawnsMade, 0, 0),
                     InterlockedCompareExchange(&extraSpawnsPlaced, 0, 0), InterlockedCompareExchange(&extraSpawnsFailed, 0, 0),
                     InterlockedCompareExchange(&extraSpawnsNoDonor, 0, 0));
        lastExtras = extrasNow;
    }
    static LONG lastAreas = -1;
    const LONG areasNow = InterlockedCompareExchange(&extraAreasFired, 0, 0) + InterlockedCompareExchange(&extraAreasNoSpawner, 0, 0) +
                          InterlockedCompareExchange(&extraAreasNoDonor, 0, 0);
    if (areasNow != lastAreas) {
        std::fprintf(log, "extra_areas fired=%ld made=%ld no_spawner=%ld retried=%ld no_donor=%ld\n", InterlockedCompareExchange(&extraAreasFired, 0, 0),
                     InterlockedCompareExchange(&extraAreasMade, 0, 0), InterlockedCompareExchange(&extraAreasNoSpawner, 0, 0),
                     InterlockedCompareExchange(&extraAreasRetried, 0, 0), InterlockedCompareExchange(&extraAreasNoDonor, 0, 0));
        std::fprintf(log, "extra_spawns spawner_registry named=%ld unnamed_dropped=%ld refused_identity=%ld refused_retired=%ld level_resets=%ld\n",
                     InterlockedCompareExchange(&knownSpawnersNamed, 0, 0), InterlockedCompareExchange(&knownSpawnersUnnamed, 0, 0),
                     InterlockedCompareExchange(&knownSpawnersMoved, 0, 0), InterlockedCompareExchange(&knownSpawnersRetired, 0, 0),
                     InterlockedCompareExchange(&knownSpawnersCleared, 0, 0));
        lastAreas = areasNow;
    }
    LONG total = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(ds2::MoreEnemiesStatus::Count); ++i)
        total += InterlockedCompareExchange(&moreEnemiesStatusCounts[i], 0, 0);
    while (flushed < kMoreEnemiesDetailCapacity &&
           InterlockedCompareExchange(&moreEnemiesDetails[flushed].ready, 1, 1) == 1) {
        const auto& detail = moreEnemiesDetails[flushed];
        std::fprintf(log, "more_enemies_event spawner=%08X attrs=%08X "
                          "target=%08X-%08X-%08X-%08X family=%s result=%s wanted=%u made=%u "
                          "authored_count=%d authored_max=%d tick=%u\n",
                     detail.spawner, detail.attributes, detail.target[0], detail.target[1],
                     detail.target[2], detail.target[3], detail.family,
                     ds2::moreEnemiesStatusName(detail.status), detail.wanted, detail.made,
                     detail.authoredCount, detail.authoredMax, detail.tick);
        ++flushed;
    }
    total += InterlockedCompareExchange(&moreEnemiesDeferredMade, 0, 0) +
             InterlockedCompareExchange(&moreEnemiesDeferredLost, 0, 0);
    if (total == lastTotal) return;
    lastTotal = total;
    std::fprintf(log, "more_enemies summary triggers=%ld extra_requested=%ld "
                      "extra_created=%ld inner_no_actor=%ld restore_failures=%ld swarm_hordes=%ld swarm_horde_extras=%ld horde_silence_armed=%ld horde_silence_passed=%ld horde_silence_expired=%ld horde_sends_silenced=%ld\n",
                 total, InterlockedCompareExchange(&moreEnemiesRequested, 0, 0),
                 InterlockedCompareExchange(&moreEnemiesCreated, 0, 0),
                 InterlockedCompareExchange(&moreEnemiesInnerEmpty, 0, 0),
                 InterlockedCompareExchange(&moreEnemiesRestoreFailed, 0, 0),
                 InterlockedCompareExchange(&swarmHordes, 0, 0),
                 InterlockedCompareExchange(&swarmHordeExtras, 0, 0),
                 InterlockedCompareExchange(&hordeSilenceArmed, 0, 0),
                 InterlockedCompareExchange(&hordeSilencePassed, 0, 0),
                 InterlockedCompareExchange(&hordeSilenceExpired, 0, 0),
                 InterlockedCompareExchange(&hordeSendsSilenced, 0, 0));
    const LONG queuedTotal = InterlockedCompareExchange(&moreEnemiesDeferred, 0, 0);
    if (InterlockedCompareExchange(&ventRetirePlanned, 0, 0))
        std::fprintf(log, "more_enemies vent_retire planned=%ld natural=%ld synthetic=%ld lost=%ld failed=%ld\n",
                     InterlockedCompareExchange(&ventRetirePlanned, 0, 0), InterlockedCompareExchange(&ventRetiredNatural, 0, 0),
                     InterlockedCompareExchange(&ventRetiredSynthetic, 0, 0), InterlockedCompareExchange(&ventRetireLost, 0, 0),
                     InterlockedCompareExchange(&ventRetireFailed, 0, 0));
    if (queuedTotal)
        std::fprintf(log, "more_enemies deferred queued=%ld made=%ld lost=%ld delay_ms=%u vent_gap_ms=%u gap_waits=%ld\n",
                     queuedTotal, InterlockedCompareExchange(&moreEnemiesDeferredMade, 0, 0),
                     InterlockedCompareExchange(&moreEnemiesDeferredLost, 0, 0),
                     moreEnemiesVentDelayMs, moreEnemiesVentGapMs, InterlockedCompareExchange(&ventGapWaits, 0, 0));
    for (unsigned i = 0; i < static_cast<unsigned>(ds2::MoreEnemiesStatus::Count); ++i) {
        const LONG count = InterlockedCompareExchange(&moreEnemiesStatusCounts[i], 0, 0);
        if (count) std::fprintf(log, "more_enemies reason=%s count=%ld\n",
                                ds2::moreEnemiesStatusName(i), count);
    }
}
