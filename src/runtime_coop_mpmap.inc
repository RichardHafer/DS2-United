// Dead Space 2 United by Richard Hafer.
// A multiplayer map in single player ([MpMap], 2026-10-09).
//
// Research: analysis/DS2_MP_MAPS_IN_SP_2026-10-09.md. The engine picks SP or MP
// from the game flow mode, not from the map, so an MP map loads through the SP
// level change with global_assets as its global. Pressing [MpMap] Key twice
// within 3 s in the campaign asks GameMainLoop::RequestLevelChange (0x0040F870,
// thiscall(map, stream, clearCheckpoint, third), ret 0x10) for the map; the
// same double press on the MP map asks for the campaign map again.
//
// Offline evidence behind each step (live image read 2026-10-09, read-only):
//  * Saves. SaveManager::ChooseSaveSlot (0x00B99FE0, virtual, thiscall, 6 args,
//    ret 0x18) writes the slot with the main loop's map name; its own failure
//    path is "call 0x00A33D10 (dec [0x01F60304]), return". A held save takes
//    exactly that path. SavePoint::Store (0x015F0440, thiscall(allocator,
//    kind), ret 8, bool) is behind both restart points (SetDeathRestart and
//    StoreLevel); held, both keep the campaign state.
//  * Checkpoints. The departure keeps the death checkpoint (clear=false):
//    clearing it at 0x00B75840 also empties the death point. On the MP map the
//    start checkpoint lookup (0x00429B80) asks Find for the campaign GUID, which
//    fails there, and falls back to the default one. The way back writes the
//    campaign GUID back, sets it as override (0x00B757B0) and loads the
//    campaign map: RestoreSave (0x00B75900) restores the untouched death point,
//    the same as a checkpoint restart.
//  * Streams. Set::SetDesired (0x00758C80) hashes each member (h = h*33 + word)
//    and asks the stream manager (0x00748A40, which walks the registered TOCs);
//    a member no TOC knows is skipped at 0x00758D28. So the five MP-only
//    character streams are dropped natively too; DropForeignStreams=1 drops
//    them before the call and logs each one.
//  * Player. Every entity of the old level goes with the world (CloseImpl and
//    SimManager::DeleteAllEntities), and no MP map stream holds a Player, so the
//    player is recreated from the global_assets template with the recipe the
//    necro module's way back uses (GetSpawnInfo, SpawnEntity, PlayerManager
//    slot 0, SendPostSpawnInitialize), then put at a MultiplayerSpawnPoint
//    (static list 0x020218A0, count 0x02021920, ctor 0x00431A00, matrix rows
//    +0x70..+0xA0) because Player::Init moves him to the level checkpoint.
//  * MP logic. Stream spawns go through the creator gate (0x00777730, necro
//    module), script spawns (FuncSpawn) through the spawn filter's ShouldSpawn
//    (0x0040DB20, thiscall(prior, SpawnInfo*), ret 8, GUID pointer at +0x0C),
//    which both SpawnEntity (return 0x007764CC) and the creator's filter pass
//    (return 0x007753B6) call.
//
// Everything is idle until the first confirmed double press; the hooks the
// visit needs go in at that moment, on the game thread.
namespace overlay { void Curtain(DWORD ms); void LiftCurtain(); }   // runtime_coop_overlay.inc
namespace arena { bool RespawnHere(); bool Usable(const float* point); }   // runtime_coop_arena.inc
// runtime_coop_loadout.inc: the guns of the character setup's pick in Versus and Mercenaries
namespace loadout {
struct GunPick { const uint32_t* guid; const uint32_t* fallback; const char* name; int kind; };
int Guns(GunPick* out, int max);
const uint32_t* Suit(const char** name, uint32_t player, bool* handled);
const uint32_t* SuitRow(const char** name);   // the setup's campaign suit, no side effects
int ApplyMode();   // 1 Mercenaries, 4 Versus, 0 neither
}
// runtime_coop_lifecycle.inc ([Lifecycle] NativePlacement): Player::Init places the body on our point
namespace lifecycle { void ArmPlacement(uint32_t player, const float* matrix, const char* why); uint32_t DisarmPlacement(); bool PlacementReady(); }
namespace mpnative { bool InVersus(); }   // runtime_coop_mpnative.inc
int PartnerPositions(float* out, int max);   // runtime_coop.inc
namespace mpmap {
bool enabled = false, ready = false, hooksIn = false, hooksTried = false;
bool holdSaves = true, dropForeign = true, blockMpLogic = true, blockScriptSpawns = true;
bool spawnPlayer = true, giveLoadout = true, enableModules = true, dressIfBare = true;
bool useMapCheckpoint = true, putOnMap = true;
float putOnMapMeters = 60.0f;
int key = VK_DECIMAL, spawnPointWanted = -1, ammoPacks = 3;
char mapName[64] = "mp_bombRun", streamName[96] = "";
int mapIndex = 0;
uint32_t groupMask = 0;
DWORD watchdogMs = 45000, playerWaitMs = 500;
// How long the local body must stand before the co-op session ticks on an MP map
// (LevelSettled; 4 s in the campaign). The map is fully loaded once it runs.
DWORD settleMs = 1500;
DWORD SettleMs() { return settleMs; }
// A black screen from the arrival until the body stands ([MpMap] ArrivalCurtain,
// drawn by the overlay): the loading screen ends at RUNNING, before the player is
// spawned, and the camera looked into the void for about a second. It falls
// ArrivalCurtainMs after the body (or the necro ghost), at the latest after 6 s.
bool arrivalCurtain = true;
DWORD curtainAfterMs = 600, curtainLiftAt = 0, curtainSampleAt = 0;
float curtainSample[3]{};
int curtain = 0;   // 1 up, 2 falls once the body stands, curtainLiftAt at the earliest
constexpr DWORD kCurtainMaxMs = 6000;
volatile LONG curtainsRaised = 0, curtainsLifted = 0;
volatile LONG readyArrivals = 0;   // arrivals whose Isaac got his extras (DS2Coop_ArrivalsReady)
// True when this machine starts a Versus round as a necro: no Isaac is spawned,
// the ghost comes straight away (runtime_coop_mpnative.inc sets it).
bool (*necroStart)() = nullptr;
volatile LONG necroStarts = 0;
uint32_t playerTemplate[4] = {0x4BC78C36u, 0x9D70FEE7u, 0x544E4543u, 0x53574F4Bu};
uint32_t dressSuit[4] = {0x30C9B946u, 0xE243D95Fu, 0x54544950u, 0x32303153u};
// The suit rule (user, 02.11.): survival, Objective and Hold the Zone have no character setup, so a
// spawn and every respawn wear the Engineering Suit (a death costs the bought suit); Mercenaries and
// Versus wear the setup's pick.
constexpr uint32_t kEngineeringSuit[4] = {0x30C9B946u, 0xE243D95Fu, 0x54544950u, 0x32303153u};
const uint32_t* RuleSuit(const char*& why);   // below, after loadout's declarations
FILE* logFile = nullptr;
bool MapAvailable(int index);

// engine anchors (RVAs)
constexpr uint32_t kLevelManager = 0x1C1C0A4, kLmState = 0x18, kLmFlags = 0x1C;
constexpr uint32_t kStreamManager = 0x1C1C098, kTocCount = 0x130, kSetManager = 0x1C1C0BC;
constexpr uint32_t kChooseSaveSlot = 0x799FE0, kSaveFailed = 0x633D10, kPointStore = 0x11F0440;
constexpr uint32_t kDeathPoint = 0x1C8FC04, kLevelPoint = 0x1C8FC08, kOverrideGuid = 0x1C91820;
constexpr uint32_t kSetDesired = 0x358C80, kStreamLookup = 0x348A40, kStreamLoad = 0x34BDD0, kLoadToc = 0x349320;
constexpr uint32_t kSpawnFilter = 0xDB20, kFilterFromCreator = 0x3753B6, kFilterFromSpawnEntity = 0x3764CC, kCreator = 0x377730;
constexpr uint32_t kSpawnPoints = 0x1C218A0, kSpawnPointCount = 0x1C21920, kSpawnPointVtable = 0x16AC6B4, kSpawnPointMatrix = 0x70;
constexpr uint32_t kMpLevelSettings = 0x1C21624, kRespawnManager = 0x1C21628, kSpUiBlock = 0x1C89C3C, kMpUiBlock = 0x1C89C40;
constexpr uint32_t kCameraManager = 0x1C2131C, kCmBlenders = 0x10, kCmViewports = 0x164, kCmPlayerIds = 0x168, kBlenderStack = 0xD4;
constexpr uint32_t kGetDhc = 0xAB8D80, kAddEnergy = 0xB55D30, kDhcEnergy = 0xF0, kDhcEnergyMax = 0xF4;
constexpr uint32_t kPlayerFlags = 0x3D8, kFlagStasis = 0x08, kFlagTk = 0x10, kPlayerMmi = 0x140, kPlayerHud = 0xFB8, kPlayerChapter = 0xFC8;
constexpr uint32_t kPlayerPos = 0xC0;

// --- the log ---
// A ring written on the game thread, the watcher thread and inside hooks,
// printed by the diagnostics thread. Hard budget for the whole process.
constexpr unsigned kRingLines = 1024, kLineSize = 320;   // a load can log hundreds of lines within one flush
constexpr LONG kLineBudget = 6000;
char ring[kRingLines][kLineSize]{};
unsigned ringHead = 0, ringCount = 0;
SRWLOCK ringLock = SRWLOCK_INIT;
volatile LONG linesWritten = 0, linesOverBudget = 0, linesLost = 0;
DWORD startTick = 0;
volatile LONG visitStartTick = 0;
void Event(const char* format, ...) {
    if (InterlockedIncrement(&linesWritten) > kLineBudget) { InterlockedIncrement(&linesOverBudget); return; }
    char text[kLineSize];
    const DWORD now = GetTickCount();
    const DWORD since = now - startTick;
    const LONG visitAt = InterlockedCompareExchange(&visitStartTick, 0, 0);
    int n = visitAt ? std::snprintf(text, sizeof(text), "mpmap t=%lu.%03lu v=+%lu.%03lu ", since / 1000, since % 1000,
                                     (now - static_cast<DWORD>(visitAt)) / 1000, (now - static_cast<DWORD>(visitAt)) % 1000)
                    : std::snprintf(text, sizeof(text), "mpmap t=%lu.%03lu ", since / 1000, since % 1000);
    if (n < 0) n = 0;
    va_list args;
    va_start(args, format);
    std::vsnprintf(text + n, sizeof(text) - n, format, args);
    va_end(args);
    vlog::Mirror(text);
    AcquireSRWLockExclusive(&ringLock);
    if (ringCount == kRingLines) { ringHead = (ringHead + 1) % kRingLines; --ringCount; InterlockedIncrement(&linesLost); }
    std::memcpy(ring[(ringHead + ringCount) % kRingLines], text, sizeof(text));
    ++ringCount;
    ReleaseSRWLockExclusive(&ringLock);
}
// Bounded lines per kind: true while this kind still may log.
bool Budget(volatile LONG& counter, LONG limit) { return InterlockedIncrement(&counter) <= limit; }
volatile LONG gameThreadId = 0;   // set by Tick; the watchdog samples this thread when loading stalls

volatile LONG faults = 0;
uint32_t lastFaultAddress = 0;
int FaultFilter(EXCEPTION_POINTERS* info) {
    lastFaultAddress = info && info->ExceptionRecord ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress)) : 0;
    InterlockedIncrement(&faults);
    return EXCEPTION_EXECUTE_HANDLER;
}

// --- shared state ---
coop_mpmap::Visit visit;
coop_mpmap::DoublePress press;
volatile LONG phaseMirror = 0;       // coop_mpmap::Phase, for the watcher thread
volatile LONG sawNotRunning = 0;     // the watcher saw the level manager outside RUNNING
volatile LONG stuckFlag = 0;         // the watchdog fired; the game thread shows the notice
volatile LONG loopCache = 0;         // GameMainLoop, found on the game thread
struct Origin { char map[128]; char stream[128]; char third[128]; uint32_t death[4]; uint32_t override_[4]; float pos[3]; uint32_t player; bool frontend; };
Origin origin{};
bool InVisit() { return InterlockedCompareExchange(&phaseMirror, 0, 0) != static_cast<LONG>(coop_mpmap::Phase::Idle); }
// The arrival count of this visit: a round restart or a side swap reloads the map.
unsigned Run() { return static_cast<unsigned>(visit.epochs); }
bool Arrived() { return InterlockedCompareExchange(&phaseMirror, 0, 0) == static_cast<LONG>(coop_mpmap::Phase::Arrived); }
bool Loading() {
    const LONG p = InterlockedCompareExchange(&phaseMirror, 0, 0);
    return p == static_cast<LONG>(coop_mpmap::Phase::Leaving) || p == static_cast<LONG>(coop_mpmap::Phase::Loading) ||
           p == static_cast<LONG>(coop_mpmap::Phase::Returning);
}

uint32_t LmState(uint32_t* flags) {
    uint32_t lm = 0, state = 0xFFFFFFFFu, f = 0;
    if (Read(base + kLevelManager, &lm, 4) && lm) { Read(lm + kLmState, &state, 4); Read(lm + kLmFlags, &f, 2); }
    if (flags) *flags = f & 0xFFFF;
    return state;
}
uint32_t TocCount() {
    uint32_t manager = 0, count = 0;
    return Read(base + kStreamManager, &manager, 4) && manager && Read(manager + kTocCount, &count, 4) ? count : 0;
}
void MapOf(uint32_t loop, char* out, size_t size) {
    char buffer[128]{};
    out[0] = 0;
    if (loop && Read(loop + level::kMapOffset, buffer, sizeof(buffer) - 1)) std::snprintf(out, size, "%s", buffer);
}

// --- held saves ---
safetyhook::InlineHook chooseHook, storeHook;
volatile LONG savesHeld = 0, pointsHeld = 0, savesPassed = 0, pointsPassed = 0, saveLines = 0;
// A campaign co-op joiner plays the host's world: none of it goes into its
// own save slot. Ends when the main menu runs again.
bool campaignClientHold = false;
bool campaignClientLevelSeen = false;   // the joined level ran: the next main menu ends the hold
// A campaign arena runs (runtime_coop_arena.inc): held until it ends, the main menu check aside.
bool arenaHold = false;
// A debug menu checkpoint jump with [Debug] HoldSaves=1 (runtime_coop_debugmenu.inc): held until the main menu.
bool debugHold = false;
bool HoldingSaves() { return visit.Holding() || campaignClientHold || arenaHold || debugHold; }
// The death-restart and level points are the campaign's own restore state. Held only
// while a level that is not the campaign runs (an MP map, an arena): on a debug jump
// or a campaign join they are this run's checkpoints, and held they restored an empty
// inventory on every death and restart (13.10., ch02: points_held=4, 6 items -> 0).
bool HoldingPoints() { return visit.Holding() || arenaHold; }
// A save kiosk waits for the write to finish. 0x00A33D10 only takes back the kiosk's
// IncNumTimesSaved; the menu stayed busy with the logic paused (SaveKiosk::BeginSave
// 0x00BAE040) and the player froze until damage force-quit the kiosk (14.10.). A real
// write ends in the device callback at manager+0x24 (the object there has vtable 0x01B42B68,
// slot +0x44 = 0x00BAE3A0, thiscall(result, op), ret 8): result 0 refreshes the slot menu and
// puts the manager back to ready (2), so the kiosk carries on as after a save. Until 16.10.
// the vtable was compared with 0x01B42BA4 (the same slot read as +8 from inside the table),
// which never matched: the completion was never reported and the kiosk froze.
constexpr uint32_t kSaveCallbackVtable = 0x1742B68, kSaveCallbackSlot = 0x44, kSaveCallbackDone = 0x7AE3A0;
constexpr uint32_t kKioskChooseLo = 0x7A6780, kKioskChooseHi = 0x7A6860;   // SaveKiosk::ChooseSaveSlot
volatile LONG savesCompleted = 0, saveCompleteMismatch = 0, saveCompleteFaults = 0;
bool CompleteHeldSave(uint32_t manager) {
    uint32_t callback = 0, fn = 0, stateBefore = 0, stateAfter = 0;
    if (!Read(manager + 0x24, &callback, 4) || !Read(callback + kSaveCallbackSlot, &fn, 4) || fn != base + kSaveCallbackDone) {
        InterlockedIncrement(&saveCompleteMismatch);
        Event("held save: the write callback at manager %08X +0x24 has vtable %08X, slot +0x44 holds %08X (wanted %08X): completion NOT reported", manager,
              callback, fn, static_cast<uint32_t>(base + kSaveCallbackDone));
        return false;
    }
    Read(manager + 0x24 + 0xC, &stateBefore, 4);
    __try {
        reinterpret_cast<void(__thiscall*)(uint32_t, int, int)>(fn)(manager + 0x24, 0, 0);
    } __except (FaultFilter(GetExceptionInformation())) {
        InterlockedIncrement(&saveCompleteFaults);
        return false;
    }
    Read(manager + 0x24 + 0xC, &stateAfter, 4);
    InterlockedIncrement(&savesCompleted);
    Event("held save: completion reported (callback vtable %08X%s, state %u -> %u)", callback, callback == base + kSaveCallbackVtable ? "" : ", not the usual one",
          stateBefore, stateAfter);
    return true;
}
void __fastcall OnChooseSaveSlot(void* self, void*, uint32_t slot, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
    if (!holdSaves || !HoldingSaves()) {
        InterlockedIncrement(&savesPassed);
        chooseHook.thiscall<void>(self, slot, a1, a2, a3, a4, a5);
        return;
    }
    InterlockedIncrement(&savesHeld);
    const auto caller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    const bool kiosk = caller >= base + kKioskChooseLo && caller < base + kKioskChooseHi;
    clientprog::NoteSaveAttempt(kiosk ? "a held kiosk save" : "a held autosave");   // a client's own progress goes to its file instead
    char map[64]{};
    MapOf(static_cast<uint32_t>(InterlockedCompareExchange(&loopCache, 0, 0)), map, sizeof(map));
    if (Budget(saveLines, 60))
        Event("save HELD: SaveManager::ChooseSaveSlot slot %u (%s) on map %s, phase %s; answered with the native failure path, nothing written",
              slot + 1, slot == 0 ? "autosave" : "slot", map, coop_mpmap::PhaseName(visit.phase));
    __try {
        reinterpret_cast<void(__cdecl*)()>(base + kSaveFailed)();
    } __except (FaultFilter(GetExceptionInformation())) {
        Event("save hold: the native failure call faulted at %08X", lastFaultAddress);
    }
    const uint32_t manager = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    const bool completed = CompleteHeldSave(manager);
    uint32_t state = 0xFFFFFFFFu;
    Read(manager + 0xC, &state, 4);
    Event("save hold: %s, write completion %s (manager %08X state %u)", kiosk ? "from a save kiosk" : "not from a kiosk",
          completed ? "reported, the kiosk carries on" : "NOT reported (callback vtable mismatch or fault)", manager, state);
    if (kiosk)
        notice::Show(campaignClientHold ? "NOT SAVED - THE HOST SAVES THIS RUN" : debugHold ? "NOT SAVED - SAVES ARE OFF AFTER A DEBUG JUMP" : "NOT SAVED HERE");
}
bool __fastcall OnPointStore(void* self, void*, uint32_t allocator, uint32_t kind) {
    if (!holdSaves || !HoldingPoints()) {
        InterlockedIncrement(&pointsPassed);
        return storeHook.thiscall<bool>(self, allocator, kind);
    }
    InterlockedIncrement(&pointsHeld);
    const uint32_t point = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    uint32_t death = 0, levelPoint = 0;
    Read(base + kDeathPoint, &death, 4);
    Read(base + kLevelPoint, &levelPoint, 4);
    if (Budget(saveLines, 60))
        Event("save HELD: SavePoint::Store %s point %08X kind %u, phase %s; the campaign restart state stays as it was",
              point == death ? "death-restart" : point == levelPoint ? "level" : "other", point, kind, coop_mpmap::PhaseName(visit.phase));
    return false;
}

// --- stream set members ---
safetyhook::InlineHook desiredHook, loadHook, tocHook;
volatile LONG desiredCalls = 0, desiredLines = 0, memberLines = 0, membersSeen = 0, membersForeign = 0, membersDropped = 0;
volatile LONG streamLoads = 0, streamLoadLines = 0, tocLoads = 0, tocLines = 0, tocRefused = 0;
char lastStream[96] = "", lastToc[96] = "";
volatile LONG lastStreamAt = 0, lastTocAt = 0;
SRWLOCK nameLock = SRWLOCK_INIT;
void Remember(char* out, size_t size, const char* name, volatile LONG& at) {
    AcquireSRWLockExclusive(&nameLock);
    std::snprintf(out, size, "%s", name);
    ReleaseSRWLockExclusive(&nameLock);
    InterlockedExchange(&at, static_cast<LONG>(GetTickCount()));
}
__declspec(noinline) uint32_t LookupStream(uint32_t hash) {
    uint32_t manager = 0;
    if (!Read(base + kStreamManager, &manager, 4) || !manager) return 0;
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t)>(base + kStreamLookup)(reinterpret_cast<void*>(static_cast<uintptr_t>(manager)), hash);
    } __except (FaultFilter(GetExceptionInformation())) {
        return 0;
    }
}
void __fastcall OnSetDesired(void* self, void*, const uint32_t (*guids)[4], const unsigned char* hidden, uint32_t count, const uint32_t* activator) {
    constexpr unsigned kMax = 64;
    // coop_streams' hook sits behind this one (installed first): it judges the caller we saw.
    struct Outer {
        explicit Outer(uint32_t caller) { streams::pin::outerCaller = caller; }
        ~Outer() { streams::pin::outerCaller = 0; }
    } outer(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
    if (!visit.Holding() || !guids || !count) {
        desiredHook.thiscall<void>(self, guids, hidden, count, activator);
        return;
    }
    const LONG call = InterlockedIncrement(&desiredCalls);
    const uint32_t caller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    uint32_t act[4]{};
    if (activator) Read(reinterpret_cast<uintptr_t>(activator), act, 16);
    if (count > kMax) {
        if (Budget(desiredLines, 80)) Event("set_desired #%ld set=%08X members=%u: more than %u, passed unfiltered", call, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)), count, kMax);
        desiredHook.thiscall<void>(self, guids, hidden, count, activator);
        return;
    }
    bool known[kMax]{};
    unsigned foreign = 0;
    const bool logMembers = Budget(desiredLines, 80);
    if (logMembers)
        Event("set_desired #%ld set=%08X caller=%08X activator=%08X-%08X members=%u phase=%s policy=%s", call, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)),
              caller, act[0], act[1], count, coop_mpmap::PhaseName(visit.phase), dropForeign ? "drop_foreign" : "let_through");
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t g[4]{};
        Read(reinterpret_cast<uintptr_t>(guids[i]), g, 16);
        InterlockedIncrement(&membersSeen);
        const bool skipped = coop_mpmap::NullGuid(g) || coop_mpmap::InvalidGuid(g);
        const uint32_t hash = coop_mpmap::StreamHash(g);
        const uint32_t id = skipped ? 0 : LookupStream(hash);
        known[i] = id != 0;
        if (!skipped && !id) { ++foreign; InterlockedIncrement(&membersForeign); }
        if (logMembers && (!known[i] || Budget(memberLines, 300)))
            Event("  member %u %08X-%08X-%08X-%08X hash=%08X %s id=%08X status=%d%s", i, g[0], g[1], g[2], g[3], hash,
                  skipped ? "null (engine skips)" : id ? "known" : "FOREIGN (no registered TOC knows it)", id, id ? StreamStatus(id) : -9,
                  !skipped && !id ? (dropForeign ? " -> dropped before the loader" : " -> let through (SetDesired skips it at 0x00758D28)") : "");
    }
    if (!dropForeign || !foreign) {
        desiredHook.thiscall<void>(self, guids, hidden, count, activator);
        return;
    }
    uint32_t list[kMax][4]{};
    unsigned char flags[kMax]{};
    unsigned dropped = 0;
    const unsigned kept = coop_mpmap::FilterMembers(guids, hidden, count, known, true, list, flags, &dropped);
    InterlockedExchangeAdd(&membersDropped, static_cast<LONG>(dropped));
    if (logMembers) Event("set_desired #%ld: %u foreign member(s) dropped, %u passed on", call, dropped, kept);
    desiredHook.thiscall<void>(self, list, flags, kept, activator);
}
uint32_t __fastcall OnStreamLoad(void* manager, void*, const char* name, float priority, unsigned flags, const void* a4, const char* a5) {
    const uint32_t caller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    const uint32_t handle = loadHook.thiscall<uint32_t>(manager, name, priority, flags, a4, a5);
    if (!Loading()) return handle;
    InterlockedIncrement(&streamLoads);
    char copy[96]{};
    if (name) Read(reinterpret_cast<uintptr_t>(name), copy, sizeof(copy) - 1);
    Remember(lastStream, sizeof(lastStream), copy, lastStreamAt);
    if (Budget(streamLoadLines, 250))
        Event("stream request '%s' flags=%X handle=%08X status=%d caller=%08X%s", copy, flags, handle, handle ? StreamStatus(handle) : -9, caller,
              std::strstr(copy, "global_mp_assets") ? "  <- global_mp_assets" : "");
    return handle;
}
uint32_t __fastcall OnLoadToc(void* manager, void*, const char* name, void* callback) {
    const bool watch = Loading();
    const uint32_t before = watch ? TocCount() : 0;
    const uint32_t toc = tocHook.thiscall<uint32_t>(manager, name, callback);
    if (!watch) return toc;
    const uint32_t after = TocCount();
    InterlockedIncrement(&tocLoads);
    char copy[96]{};
    if (name) Read(reinterpret_cast<uintptr_t>(name), copy, sizeof(copy) - 1);
    Remember(lastToc, sizeof(lastToc), copy, lastTocAt);
    const bool refused = toc && after == before && before >= 32;
    if (refused) InterlockedIncrement(&tocRefused);
    if (Budget(tocLines, 120))
        Event("toc load '%s.toc' -> %08X registered %u -> %u%s", copy, toc, before, after,
              !toc ? "  NOT LOADED (file missing or unreadable)" : refused ? "  NOT REGISTERED (32-TOC limit)" : "");
    return toc;
}

// --- MP logic kept from spawning ---
constexpr size_t kEntityCount = sizeof(coop_mpmap::kMpMapEntities) / sizeof(coop_mpmap::kMpMapEntities[0]);
volatile LONG requested[kEntityCount]{};
volatile LONG blockedStream = 0, blockedScript = 0, watchedSeen = 0, gateLines = 0, watchLines = 0, recordsKept = 0;
volatile LONG blockedByGroup[coop_mpmap::kGroups]{};
bool keepSlasherRecords = true;
// The eight PlayAudioStreams of the MP round music (aud_play_stream_mus_01..08,
// global_mp_assets) are in the mp_globals group; with AllowMpMusic they spawn,
// so survival can play them (runtime_coop_survmusic.inc). Everything else of
// that group stays blocked (the UIPopup crash, dump 44368).
bool allowMpMusic = true;
volatile LONG musicKept = 0;
bool MusicEntity(const uint32_t* guid) {
    static const uint32_t kMusic[8][2] = {{0x52CB6B41u, 0x470E983Fu}, {0x52CB6B41u, 0x478A9287u}, {0x52CB6B41u, 0x47FD0367u}, {0x52CB6B41u, 0x411B1999u},
                                          {0x52CB6BF5u, 0x4D76DA52u}, {0x52CB6BF5u, 0x4E372802u}, {0x52CB6BF5u, 0x4EC02C29u}, {0x52CB6BF5u, 0x4F55352Bu}};
    if (guid[2] != 0x4D574F4Cu || guid[3] != 0x454C4C49u) return false;
    for (const auto& m : kMusic) if (guid[0] == m[0] && guid[1] == m[1]) return true;
    return false;
}
safetyhook::InlineHook filterHook;
const coop_mpmap::MapEntity* Entity(const uint32_t* guid) { return coop_mpmap::Find(coop_mpmap::kMpMapEntities, kEntityCount, guid); }
// A campaign arena (runtime_coop_arena.inc) keeps its story logic from spawning:
// true when this GUID must not spawn here (atCreator: the stream creation).
bool (*arenaBlocks)(const uint32_t* guid, bool atCreator) = nullptr;
// The plugin's own spawn of a blocked template (the nameplate helper FXObject of
// global_mp_assets, runtime_coop_nameplate.inc) passes both gates. Game thread.
bool ownSpawn = false;
volatile LONG ownSpawnsPassed = 0;
// Versus plays the map's own round: its end of round, clock, objective markers,
// team logic and MPLevelSettings spawn as in an MP round (runtime_coop_mpnative.inc
// sets versusWanted). The slashers and the other global_mp_assets entities stay out.
bool (*versusWanted)() = nullptr;
// Versus [NativeAudio]: the MP audio layer of global_mp_assets (announcer, stingers,
// the round music logic) may spawn; set by runtime_coop_versus.inc.
bool (*versusPassesClass)(const char* cls) = nullptr;
volatile LONG versusAudioPassed = 0;
constexpr uint32_t kVersusOpenGroups = (1u << coop_mpmap::kEndRound) | (1u << coop_mpmap::kRoundTimer) | (1u << coop_mpmap::kObjectiveMarkers) |
                                       (1u << coop_mpmap::kTeamLogic);
constexpr uint32_t kLevelSettingsGuid[4] = {0x58C9C3B1u, 0x0878CD53u, 0x3532554Cu, 0x58583337u};
// The necro ghost's RespawnProxy (MPLevelSettings attrs+0xC8, mp_globals): spawned
// at run time by CreateRespawnProxy for a Versus necro, never by the map's load
// (test 11.10. 22:02: blocked, so no ghost came out).
constexpr uint32_t kRespawnProxyGuid[4] = {0x58C9EA2Au, 0xCBDF5A07u, 0x3532554Cu, 0x58583337u};
volatile LONG versusOpened = 0, versusProxies = 0;
// Templates a Versus match spawns at run time out of global_mp_assets
// (generated_versus_templates.h: what MPLevelSettings, the RespawnProxy and the
// four NecroPlayer types reach): the necro itself (11.10. 23:38: blocked, the
// spawn at the picked vent made nothing and RegisterWithManager crashed), its
// cameras (12.10. 00:31: InitCameras spawned the chase camera from a missing
// record and crashed), projectiles, death drops and the vent highlight. Their
// records stay, the map's own instances are refused at the creator's filter,
// the run-time spawns pass.
bool VersusRuntimeTemplate(const uint32_t* guid) {
    if (!versusWanted || !versusWanted()) return false;
    for (const auto& t : coop_mpmap::kVersusTemplates)
        if (std::memcmp(t.guid, guid, sizeof(t.guid)) == 0) return true;
    return false;
}
bool BlocksHere(const coop_mpmap::MapEntity* e, const uint32_t* guid) {
    const bool versus = versusWanted && versusWanted();
    if (versus && versusPassesClass && e->cls && versusPassesClass(e->cls)) {
        InterlockedIncrement(&versusAudioPassed);
        return false;
    }
    if (!coop_mpmap::Blocks(e, mapIndex, versus ? groupMask & ~kVersusOpenGroups : groupMask)) {
        if (versus && coop_mpmap::Blocks(e, mapIndex, groupMask)) InterlockedIncrement(&versusOpened);
        return false;
    }
    if (versus && std::memcmp(guid, kLevelSettingsGuid, sizeof(kLevelSettingsGuid)) == 0) {
        InterlockedIncrement(&versusOpened);
        return false;
    }
    return true;
}
// From the creator gate (OnCreator in runtime_coop.inc): the GUID is inline at request+0x0C.
bool ShouldBlock(uint32_t request) {
    if (!enabled) return false;
    if (ownSpawn) { InterlockedIncrement(&ownSpawnsPassed); return false; }
    uint32_t guid[4]{};
    if (arenaBlocks && Read(request + 0x0C, guid, sizeof(guid)) && arenaBlocks(guid, true)) return true;
    if (!visit.Holding()) return false;
    if (!Read(request + 0x0C, guid, sizeof(guid))) return false;
    const coop_mpmap::MapEntity* e = Entity(guid);
    if (!e) return false;
    InterlockedIncrement(&requested[e - coop_mpmap::kMpMapEntities]);
    const bool block = blockMpLogic && BlocksHere(e, guid);
    if (block && allowMpMusic && MusicEntity(guid)) {
        InterlockedIncrement(&musicKept);
        if (Budget(gateLines, 60)) Event("spawn KEPT (mp_music) %s %08X-%08X-%08X-%08X: the round music survival plays", e->cls, guid[0], guid[1], guid[2], guid[3]);
        return false;
    }
    // The creator (0x00777730) registers the spawn record (0x00777660) whatever
    // the filter answers. The map's slashers are the vent spawners' targets, and a
    // survival vent spawn rebinds against that record, so they pass this gate and
    // the spawn filter refuses the entity itself.
    // The Versus ghost's RespawnProxy the same way: CreateRespawnProxy needs its spawn
    // record (GetSpawnInfo), the gate refusing it kept the record out too and no
    // ghost ever came (11.10. 22:21); the run-time spawn passes the filter.
    if (block && blockScriptSpawns && filterHook && (VersusRuntimeTemplate(guid) || mpsuit::KeepRecord(guid))) {
        InterlockedIncrement(&recordsKept);
        Event("spawn record kept (stream) %s %08X-%08X for Versus' run-time spawns; the spawn filter refuses the map's own instance", e->cls, guid[0], guid[1]);
        return false;
    }
    if (block && keepSlasherRecords && e->group == coop_mpmap::kMpSlashers && blockScriptSpawns && filterHook) {
        InterlockedIncrement(&recordsKept);
        if (Budget(gateLines, 60)) Event("spawn record kept (stream) %s %08X-%08X-%08X-%08X group=%s; the spawn filter refuses the entity", e->cls, guid[0], guid[1],
                                         guid[2], guid[3], coop_mpmap::GroupName(e->group));
        return false;
    }
    if (block) {
        InterlockedIncrement(&blockedStream);
        InterlockedIncrement(&blockedByGroup[e->group]);
        if (Budget(gateLines, 60)) Event("spawn BLOCKED (stream) %s %08X-%08X-%08X-%08X group=%s", e->cls, guid[0], guid[1], guid[2], guid[3], coop_mpmap::GroupName(e->group));
    } else {
        InterlockedIncrement(&watchedSeen);
        if (Budget(watchLines, 60)) Event("spawn seen (stream) %s %08X-%08X-%08X-%08X group=%s%s", e->cls, guid[0], guid[1], guid[2], guid[3], coop_mpmap::GroupName(e->group),
                                          e->group != coop_mpmap::kWatch ? " (its group is switched off)" : "");
    }
    return block;
}
bool __fastcall OnSpawnFilter(void* filter, void*, bool prior, uint32_t info) {
    if (ownSpawn) return filterHook.thiscall<bool>(filter, prior, info);
    if (arenaBlocks && info) {
        uint32_t at = 0, g[4]{};
        if (Read(info + 0x0C, &at, 4) && at && Read(at, g, 16) && arenaBlocks(g, false)) return false;
    }
    if (!visit.Holding() || !blockMpLogic || !blockScriptSpawns || !info) return filterHook.thiscall<bool>(filter, prior, info);
    const uint32_t caller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    uint32_t guidAt = 0, guid[4]{};
    const coop_mpmap::MapEntity* e = Read(info + 0x0C, &guidAt, 4) && guidAt && Read(guidAt, guid, 16) ? Entity(guid) : nullptr;
    if (!e) return filterHook.thiscall<bool>(filter, prior, info);
    const bool fromCreator = caller == base + kFilterFromCreator;
    if (!fromCreator && (VersusRuntimeTemplate(guid) || mpsuit::KeepRecord(guid))) {
        InterlockedIncrement(&versusProxies);
        if (Budget(watchLines, 60)) Event("spawn passed (Versus run time) %s %08X-%08X, caller %08X", e->cls, guid[0], guid[1], caller);
        return filterHook.thiscall<bool>(filter, prior, info);
    }
    if (!BlocksHere(e, guid) || (allowMpMusic && MusicEntity(guid))) {
        if (!fromCreator) {
            InterlockedIncrement(&requested[e - coop_mpmap::kMpMapEntities]);
            InterlockedIncrement(&watchedSeen);
            if (Budget(watchLines, 60)) Event("spawn seen (script, caller %08X) %s %08X-%08X-%08X-%08X", caller, e->cls, guid[0], guid[1], guid[2], guid[3]);
        }
        return filterHook.thiscall<bool>(filter, prior, info);
    }
    // From the creator's own filter pass only when the creator gate did not
    // answer first (gate absent): counted as a stream spawn.
    InterlockedIncrement(&requested[e - coop_mpmap::kMpMapEntities]);
    InterlockedIncrement(fromCreator ? &blockedStream : &blockedScript);
    InterlockedIncrement(&blockedByGroup[e->group]);
    if (Budget(gateLines, 60)) Event("spawn BLOCKED (%s, caller %08X) %s %08X-%08X-%08X-%08X group=%s", fromCreator ? "stream, at the filter" : "script", caller, e->cls,
                                     guid[0], guid[1], guid[2], guid[3], coop_mpmap::GroupName(e->group));
    return false;
}

// --- hooks, verified at start, installed at the first visit ---
struct Site { const char* name; uint32_t rva; unsigned char bytes[18]; unsigned size; bool required; bool ok; };
Site sites[] = {
    // mov eax,[esp+4] / push esi / push edi / mov edi,ecx / lea esi,[edi+4] / test eax,eax
    {"GameMainLoop::RequestLevelChange", 0xF870, {0x8B, 0x44, 0x24, 0x04, 0x56, 0x57, 0x8B, 0xF9, 0x8D, 0x77, 0x04, 0x85, 0xC0}, 13, true, false},
    // mov eax,[esp+4] / mov ecx,[eax] / mov [0x02091820],ecx
    {"Checkpoint::SetOverrideCheckpoint", 0x7757B0, {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x08, 0x89, 0x0D, 0x20, 0x18, 0x09, 0x02}, 12, true, false},
    // push esi / mov esi,[ecx+0x88] / xor eax,eax / xor edx,edx
    {"ServiceRegistry::Find", 0x17DB0, {0x56, 0x8B, 0xB1, 0x88, 0x00, 0x00, 0x00, 0x33, 0xC0, 0x33, 0xD2}, 11, true, false},
    // sub esp,0x418 / push esi / push edi / mov edi,[esp+0x424]
    {"SaveManager::ChooseSaveSlot", kChooseSaveSlot, {0x81, 0xEC, 0x18, 0x04, 0x00, 0x00, 0x56, 0x57, 0x8B, 0xBC, 0x24, 0x24, 0x04, 0x00, 0x00}, 15, true, false},
    // dec dword [0x01F60304] / ret
    {"save failure path", kSaveFailed, {0xFF, 0x0D, 0x04, 0x03, 0xF6, 0x01, 0xC3}, 7, true, false},
    // push ebp / mov ebp,esp / sub esp,28h / push ebx / push esi / mov esi,ecx / mov ecx,[esi+14h] / push edi / xor edi,edi
    {"SavePoint::Store", kPointStore, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x28, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x14, 0x57, 0x33, 0xFF}, 16, true, false},
    // sub esp,0x30 / push ebx / push ebp / push esi / mov esi,ecx / push edi / lea ecx,[esi+0x9A0]
    {"StreamSetManager::Set::SetDesired", kSetDesired, {0x83, 0xEC, 0x30, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x57, 0x8D, 0x8E, 0xA0, 0x09, 0x00, 0x00}, 15, false, false},
    // mov edx,[esp+4] / push esi / xor esi,esi / push esi / push ecx / mov eax,esp / mov [eax],edx / call
    {"StreamManager stream lookup", kStreamLookup, {0x8B, 0x54, 0x24, 0x04, 0x56, 0x33, 0xF6, 0x56, 0x51, 0x8B, 0xC4, 0x89, 0x10, 0xE8}, 14, false, false},
    // sub esp,0x124 / push ebx / push ebp / mov ebp,ecx
    {"StreamManager::Load", kStreamLoad, {0x81, 0xEC, 0x24, 0x01, 0x00, 0x00, 0x53, 0x55, 0x8B, 0xE9}, 10, false, false},
    // sub esp,0x24 / mov eax,[esp+0x28] / push ebx / push ebp / push esi / push ".toc"
    {"StreamManager::LoadToc", kLoadToc, {0x83, 0xEC, 0x24, 0x8B, 0x44, 0x24, 0x28, 0x53, 0x55, 0x56, 0x68, 0xB8, 0x8D, 0xB1, 0x01}, 15, false, false},
    // mov al,[esp+4] / push ebx..edi / mov edi,ecx / test al,al / je
    {"DeadSpaceSpawnFilter::ShouldSpawn", kSpawnFilter, {0x8A, 0x44, 0x24, 0x04, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9, 0x84, 0xC0, 0x0F, 0x84, 0xC6, 0x00, 0x00, 0x00}, 18, false, false},
    // call edx in SpawnEntity's and in the creator's filter loop (the two return addresses above)
    {"filter call in SpawnEntity", kFilterFromSpawnEntity - 2, {0xFF, 0xD2}, 2, false, false},
    {"filter call in the creator", kFilterFromCreator - 2, {0xFF, 0xD2}, 2, false, false},
    // mov eax,[esp+0xC] / mov edx,[esp+8] / push esi / push eax / push edx
    {"SimManager::GetSpawnInfo", 0x376440, {0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x54, 0x24, 0x08, 0x56, 0x50, 0x52}, 11, true, false},
    {"SimManager::SpawnEntity", 0x376480, {0x53, 0x56, 0x8B, 0x74, 0x24, 0x0C, 0x33, 0xC0, 0x8B, 0xD9, 0x39, 0x46, 0x10, 0x74, 0x68}, 15, true, false},
    {"SendPostSpawnInitialize", 0x3752A0, {0x83, 0xEC, 0x0C, 0xA1, 0xE4, 0xD5, 0x04, 0x02, 0x56, 0x8B, 0x74, 0x24, 0x18, 0x57, 0x8B, 0x7C}, 16, true, false},
    {"PlayerManager::Register", 0x37BB60, {0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14, 0x83, 0xCB, 0xFF, 0x8B, 0xF1}, 12, true, false},
    // mov eax,[esp+4] / mov [ecx+4],eax / ret 4
    {"SpawnInfo::SetTransform", 0x44A5B0, {0x8B, 0x44, 0x24, 0x04, 0x89, 0x41, 0x04, 0xC2, 0x04, 0x00}, 10, true, false},
    {"CharacterMovementComponent::SetMatrix", 0xB72DC0, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x48, 0x56, 0x8B, 0x75}, 12, true, false},
    {"MultiplayerSpawnPoint ctor (list 0x020218A0)", 0x31A6C, {0xA1, 0x20, 0x19, 0x02, 0x02, 0x83, 0xF8, 0x20, 0x73, 0x0D, 0x89, 0x34, 0x85, 0xA0, 0x18, 0x02, 0x02}, 17, true, false},
    {"GetDHC", kGetDhc, {0x85, 0xC9, 0x74, 0x0E, 0x0F, 0xB6, 0x05, 0x52, 0x42, 0x0A, 0x02, 0x50, 0xE8}, 13, false, false},
    {"HealthComponent::AddEnergy", kAddEnergy, {0x8A, 0x81, 0xC4, 0x00, 0x00, 0x00, 0xA8, 0x02, 0x75, 0x36, 0x0F, 0x57, 0xC0, 0x0F, 0x2F, 0x81}, 16, false, false},
    {"GetGlobalPickupItem", 0x9E7560, {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x28, 0x53, 0x8B, 0x1D}, 10, false, false},
    {"CanFitItem", 0x9CDD20, {0x83, 0xEC, 0x30, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x44, 0x57, 0x8B, 0xCE, 0xE8}, 14, false, false},
    {"Item::Pickup", 0x9EF170, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x14, 0x8B, 0x45, 0x08, 0x53, 0x56, 0x57, 0x8B}, 16, false, false},
    {"Quartermaster::RequisitionGun", 0x9DD5F0, {0x51, 0x53, 0x55, 0x56, 0x57, 0x8D, 0x44, 0x24, 0x10, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x1C, 0x50}, 16, false, false},
    {"FindGunEntryByProxyGUID", 0x9B28D0, {0x56, 0x33, 0xC0, 0x33, 0xF6, 0x39, 0x41, 0x18, 0x76, 0x49, 0x53, 0x57, 0x8B, 0x7C, 0x24, 0x10}, 16, false, false},
    {"Inventory::IsFull", 0x9C4C70, {0xF6, 0x41, 0x30, 0x02, 0x8B, 0x51, 0x08, 0x57, 0x8B, 0xFA, 0x74, 0x29, 0x33, 0xFF, 0x85, 0xD2}, 16, false, false},
    {"Base::Delete", 0x3568B0, {0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x06, 0x8B, 0x50, 0x08, 0x8B, 0xCE, 0xFF, 0xD2, 0x83, 0x3D}, 16, false, false},
    {"Inventory::RemoveItem", 0x9D7180, {0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xF1, 0x85, 0xC0, 0x74, 0x13, 0x50, 0xE8}, 13, false, false},
};
enum SiteIndex { sRequest, sOverride, sFind, sChoose, sSaveFailed, sStore, sDesired, sLookup, sLoad, sToc, sFilter, sFilterCallA, sFilterCallB,
                 sSpawnInfo, sSpawnEntity, sFinalize, sRegister, sTransform, sCmcMatrix, sSpawnList, sDhc, sAddEnergy,
                 sPickupSpawn, sCanFit, sPickup, sRequisition, sGunEntry, sIsFull, sDelete, sRemove, sCount };
static_assert(sizeof(sites) / sizeof(sites[0]) == sCount, "one entry per site");
// A detour of someone else at the entry (jmp rel32 followed by the retail tail) still leads to the function.
bool Matches(const Site& s) {
    unsigned char actual[18]{};
    if (!Read(base + s.rva, actual, s.size)) return false;
    return hook_site::Inspect(actual, s.bytes, s.size) != hook_site::State::Foreign;
}
bool Have(SiteIndex i) { return sites[i].ok; }
bool LoadoutReady() {
    return Have(sPickupSpawn) && Have(sCanFit) && Have(sPickup) && Have(sRequisition) && Have(sGunEntry) && Have(sIsFull) && Have(sDelete) && Have(sRemove);
}

bool InstallHooks() {
    if (hooksTried) return hooksIn;
    hooksTried = true;
    FILE* log = logFile;
    bool saves = !holdSaves;
    if (holdSaves && Have(sChoose) && Have(sSaveFailed) && Have(sStore)) {
        const bool a = hook_site::Install(log, "mpmap SaveManager::ChooseSaveSlot", base + kChooseSaveSlot, sites[sChoose].bytes, sites[sChoose].size,
                                          reinterpret_cast<void*>(OnChooseSaveSlot), chooseHook);
        const bool b = hook_site::Install(log, "mpmap SavePoint::Store", base + kPointStore, sites[sStore].bytes, sites[sStore].size,
                                          reinterpret_cast<void*>(OnPointStore), storeHook);
        saves = a && b;
    }
    bool desired = false, load = false, toc = false, filter = false;
    if (Have(sDesired) && Have(sLookup))
        desired = hook_site::Install(log, "mpmap Set::SetDesired", base + kSetDesired, sites[sDesired].bytes, sites[sDesired].size,
                                     reinterpret_cast<void*>(OnSetDesired), desiredHook);
    if (Have(sLoad))
        load = hook_site::Install(log, "mpmap StreamManager::Load", base + kStreamLoad, sites[sLoad].bytes, sites[sLoad].size,
                                  reinterpret_cast<void*>(OnStreamLoad), loadHook);
    if (Have(sToc))
        toc = hook_site::Install(log, "mpmap StreamManager::LoadToc", base + kLoadToc, sites[sToc].bytes, sites[sToc].size,
                                 reinterpret_cast<void*>(OnLoadToc), tocHook);
    if (blockMpLogic && blockScriptSpawns && Have(sFilter) && Have(sFilterCallA) && Have(sFilterCallB))
        filter = hook_site::Install(log, "mpmap DeadSpaceSpawnFilter::ShouldSpawn", base + kSpawnFilter, sites[sFilter].bytes, sites[sFilter].size,
                                    reinterpret_cast<void*>(OnSpawnFilter), filterHook);
    unsigned char creatorHead = 0;
    Read(base + kCreator, &creatorHead, 1);
    hooksIn = saves;
    Event("hooks: save_hold=%d (hold_saves=%d) set_desired=%d stream_load=%d load_toc=%d spawn_filter=%d creator_gate=%s; %s",
          saves ? 1 : 0, holdSaves ? 1 : 0, desired ? 1 : 0, load ? 1 : 0, toc ? 1 : 0, filter ? 1 : 0, creatorHead == 0xE9 ? "detoured (gate active)" : "NOT HOOKED (stream spawns pass)",
          hooksIn ? "ready" : "the save hold could not go in: no visit");
    return hooksIn;
}

// --- engine calls ---
__declspec(noinline) bool RequestLevel(uint32_t loop, const char* map, const char* stream, bool clearCheckpoint) {
    using Request = bool(__thiscall*)(void*, const char*, const char*, bool, const char*);
    __try {
        return reinterpret_cast<Request>(base + level::kRequestLevelChange)(reinterpret_cast<void*>(static_cast<uintptr_t>(loop)), map,
                                                                           stream && stream[0] ? stream : nullptr, clearCheckpoint, nullptr);
    } __except (FaultFilter(GetExceptionInformation())) {
        Event("RequestLevelChange faulted at %08X", lastFaultAddress);
        return false;
    }
}
__declspec(noinline) bool SetOverride(const uint32_t* guid) {
    __try {
        reinterpret_cast<void(__cdecl*)(const uint32_t*)>(base + level::kSetOverride)(guid);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        return false;
    }
}
// The checkpoint override (0x02091820) outlives its level: only the checkpoint
// setup at start clears it (0x00B75AB9), the native new game only overwrites it,
// and saves are held in an arena or a campaign join. The ch10 arena's
// checkpoint was found on mp_escape (every chapter's stream sets are loaded),
// so the next survival map started in ch10 (10.10.). Zero means no override.
bool ClearOverride(const char* why) {
    uint32_t before[4]{};
    Read(base + kOverrideGuid, before, 16);
    if (!(before[0] | before[1] | before[2] | before[3])) return true;
    static const uint32_t none[4]{};
    const bool ok = SetOverride(none);
    Event("checkpoint override %08X-%08X-%08X-%08X %s (%s)", before[0], before[1], before[2], before[3], ok ? "cleared" : "CLEAR FAULTED", why);
    return ok;
}

// --- the spawn points ---
struct Point { uint32_t object; float matrix[16]; };
unsigned ReadPoints(Point* out, unsigned cap, uint32_t& count) {
    count = 0;
    Read(base + kSpawnPointCount, &count, 4);
    unsigned n = 0;
    for (uint32_t i = 0; i < count && i < 32 && n < cap; ++i) {
        uint32_t object = 0, vtable = 0;
        if (!Read(base + kSpawnPoints + 4 * i, &object, 4) || !object || !Read(object, &vtable, 4) || vtable != base + kSpawnPointVtable) continue;
        out[n].object = object;
        if (!Read(object + kSpawnPointMatrix, out[n].matrix, sizeof(out[n].matrix))) continue;
        ++n;
    }
    return n;
}
void LogPoints() {
    Point points[32];
    uint32_t count = 0;
    const unsigned n = ReadPoints(points, 32, count);
    Event("MultiplayerSpawnPoint count=%u readable=%u", count, n);
    for (unsigned i = 0; i < n; ++i)
        Event("  spawn point %u at %08X pos=(%.2f %.2f %.2f) forward=(%.2f %.2f %.2f)", i, points[i].object, points[i].matrix[12], points[i].matrix[13],
              points[i].matrix[14], points[i].matrix[8], points[i].matrix[9], points[i].matrix[10]);
}

// --- player state for the log ---
__declspec(noinline) uint32_t HealthComponent(uint32_t entity) {
    if (!Have(sDhc)) return 0;
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*)>(base + kGetDhc)(reinterpret_cast<void*>(static_cast<uintptr_t>(entity)));
    } __except (FaultFilter(GetExceptionInformation())) {
        return 0;
    }
}
void DescribeWeapons(uint32_t player, char* out, size_t size) {
    uint32_t entries = 0, items = 0;
    size_t used = 0;
    out[0] = 0;
    if (!Read(player + bot::kWeaponInventory + bot::kInventoryEntries, &entries, 4) || !entries ||
        !Read(player + bot::kWeaponInventory + bot::kInventorySize, &items, 4) || items > 16) {
        std::snprintf(out, size, "unreadable");
        return;
    }
    for (uint32_t i = 0; i < items && i < 8; ++i) {
        uint32_t ref = 0, vtable = 0;
        int32_t clip = -1;
        if (!Read(entries + i * bot::kInventoryEntry + 4, &ref, 4) || ref < 0x10010 || !Read(ref - 0x10, &vtable, 4)) continue;
        const int idx = bot::GunIndexOfVtable(vtable);
        Read(ref - 0x10 + bot::kGunClip, &clip, 4);
        AppendReport(out, size, used, "%s%s(clip %d)", used ? "," : "",
                     idx >= 0 ? bot::kGunCatalogue[idx].name : vtable == base + bot::kGunProxyVtableRva ? "placeholder" : "item", clip);
    }
    if (!used) std::snprintf(out, size, "none");
}
void LogPlayer(const char* why, uint32_t player) {
    uint32_t pm = 0, slots = 0, slot0 = 0;
    if (Read(base + necro::kPlayerManager, &pm, 4) && pm && Read(pm + 4, &slots, 4) && slots) Read(slots, &slot0, 4);
    uint32_t cm = 0, viewports = 0, v0 = 0xFFFFFFFFu, blenders = 0, stack = 0xFFFFFFFFu, spUi = 0, mpUi = 0;
    if (Read(base + kCameraManager, &cm, 4) && cm) {
        Read(cm + kCmViewports, &viewports, 4);
        Read(cm + kCmPlayerIds, &v0, 4);
        if (Read(cm + kCmBlenders, &blenders, 4) && blenders) Read(blenders + kBlenderStack, &stack, 4);
    }
    Read(base + kSpUiBlock, &spUi, 4);
    Read(base + kMpUiBlock, &mpUi, 4);
    if (!player) {
        Event("player [%s]: none; slot0=%08X camera_manager=%08X viewports=%u v0_player=%d v0_stack=%d sp_ui=%08X", why, slot0, cm, viewports,
              static_cast<int>(v0), static_cast<int>(stack), spUi);
        return;
    }
    uint32_t vtable = 0, id = 0, flags = 0, mmi = 0, hud = 0, chapter = 0;
    float pos[3]{};
    Read(player, &vtable, 4);
    Read(player + necro::kPlayerIdOfPlayer, &id, 4);
    Read(player + kPlayerFlags, &flags, 4);
    Read(player + kPlayerMmi, &mmi, 4);
    Read(player + kPlayerHud, &hud, 4);
    Read(player + kPlayerChapter, &chapter, 4);
    Read(player + kPlayerPos, pos, sizeof(pos));
    const uint32_t hc = HealthComponent(player);
    float energy = -1.0f, energyMax = -1.0f;
    if (hc) { Read(hc + kDhcEnergy, &energy, 4); Read(hc + kDhcEnergyMax, &energyMax, 4); }
    uint32_t body[3]{};
    const bool bodyComplete = coop_uniform::BotBody(player, body);
    char weapons[200];
    DescribeWeapons(player, weapons, sizeof(weapons));
    Event("player [%s]: %08X vt=%s slot0=%s id=%d pos=(%.2f %.2f %.2f) flags3D8=%08X stasis=%d kinesis=%d stasis_energy=%.1f/%.1f chapter=%u "
          "body(mmi=%08X bones=%08X uniform=%08X complete=%d) rig_hud=%08X camera(viewports=%u v0_player=%d v0_stack=%d) sp_ui=%08X mp_ui=%08X weapons=%s",
          why, player, vtable == base + placement::kPlayerPrimaryVtableRva ? "Player" : "OTHER", slot0 == player + necro::kBasePlayer ? "this" : slot0 ? "other" : "empty",
          static_cast<int>(id), pos[0], pos[1], pos[2], flags, flags & kFlagStasis ? 1 : 0, flags & kFlagTk ? 1 : 0, energy, energyMax, chapter,
          body[0], body[1], body[2], bodyComplete ? 1 : 0, hud, viewports, static_cast<int>(v0), static_cast<int>(stack), spUi, mpUi, weapons);
}

// --- the player on the MP map ---
volatile LONG spawnTries = 0, spawnsDone = 0, spawnFailures = 0, ownPlayers = 0;
// [MpMap] SpawnPoint, else one point per machine, so co-op players do not all
// stand on the first point.
int PreferredPoint(unsigned count) {
    if (spawnPointWanted >= 0) return spawnPointWanted;
    return count ? static_cast<int>(machineIndex % count) : -1;
}
uint32_t ourPlayer = 0;
// The arrival body under the native loading screen ([Lifecycle] EarlySpawn): spawned while the level
// manager waits at WAIT_FOR_COMMAND (15, the OkToStart hold), so the first frame after the loading
// screen already shows him on his point; the curtain only when that did not take.
bool earlySpawn = true, earlyTried = false, earlyPlaced = false, lastSpawnPlacedNatively = false, curtainDeferred = false;
uint32_t earlyPlayer = 0;
DWORD earlyAt = 0;
constexpr uint32_t kWaitForCommand = 15;
volatile LONG earlySpawns = 0, earlyPlacedNative = 0, curtainsSkipped = 0, curtainsFallback = 0;
char spawnCurtainNote[96] = "";
bool birthSuitWanted = false;   // the arrival body was born in the setup's suit: the extras leave the suit alone
void SendSpawnUsage(uint32_t point, int index);   // below
__declspec(noinline) bool GetInfo(uint32_t sim, necro::SpawnInfo& info, const uint32_t* guid) {
    __try {
        return necro::GetSpawnInfo(sim, info, guid);
    } __except (FaultFilter(GetExceptionInformation())) {
        return false;
    }
}
__declspec(noinline) uint32_t SpawnRaw(uint32_t sim, necro::SpawnInfo& info, const float* matrix) {
    __try {
        reinterpret_cast<void(__thiscall*)(necro::SpawnInfo*, const float*)>(base + necro::kSetTransform)(&info, matrix);
        return necro::SpawnByInfo(sim, info);
    } __except (FaultFilter(GetExceptionInformation())) {
        return 0;
    }
}
__declspec(noinline) uint32_t RegisterSlotZero(uint32_t pm, uint32_t entity) {
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t, uint32_t, int)>(base + necro::kPmRegister)(
            reinterpret_cast<void*>(static_cast<uintptr_t>(pm)), entity + necro::kBasePlayer, 0, 1);
    } __except (FaultFilter(GetExceptionInformation())) {
        return 0xFFFFFFFEu;
    }
}
__declspec(noinline) bool FinalizeRaw(uint32_t entity) {
    __try {
        reinterpret_cast<void(__stdcall*)(uint32_t, int)>(base + necro::kFinalize)(entity, 0);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        return false;
    }
}
__declspec(noinline) int PlaceRaw(uint32_t entity, const float* matrix) {
    __try {
        uint32_t vtable = 0, setMatrix = 0;
        if (!Read(entity, &vtable, 4) || !Read(vtable + necro::kEntitySetMatrixSlot, &setMatrix, 4) || !setMatrix) return 0;
        using MatrixCall = void(__thiscall*)(uint32_t, const float*);
        reinterpret_cast<MatrixCall>(setMatrix)(entity, matrix);
        const uint32_t cmc = necro::FindCmc(entity);
        if (cmc) reinterpret_cast<MatrixCall>(base + necro::kCmcSetMatrixRva)(cmc, matrix);
        return cmc ? 2 : 1;
    } __except (FaultFilter(GetExceptionInformation())) {
        return -1;
    }
}
// The camera's aim after a move by hand. Player::Init takes it from the movement component's matrix
// right after its own placement (AimingData::UpdateSource 0x00F5A550, thiscall(matrix, flag), ret 8,
// called at 0x008A8852 and 0x008A8884 on [Player+0x6A0]+0x20 and +0xC0 with CMC+0xD0 and
// Player+0x3DC bit 3), so a native placement needs nothing. A body moved after Init (the old
// paths) kept the checkpoint's aim and faced the wrong way: the same two calls follow such a move.
// [Lifecycle] AimFromBody.
constexpr uint32_t kAimUpdateSourceRva = 0xB5A550, kPlayerAimOwner = 0x6A0, kAimData1 = 0x20, kAimData2 = 0xC0, kCmcMatrix = 0xD0, kPlayerFlags2 = 0x3DC;
bool aimFromBody = true, aimSiteOk = false;
volatile LONG aimResets = 0, aimFaults = 0, aimSkipped = 0;
__declspec(noinline) bool CallAimUpdate(uint32_t aim, uint32_t matrix, uint32_t flag) {
    __try {
        reinterpret_cast<void(__thiscall*)(uint32_t, uint32_t, uint32_t)>(base + kAimUpdateSourceRva)(aim, matrix, flag);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        return false;
    }
}
bool AimFromBody(uint32_t player, const char* why) {
    if (!aimFromBody || !aimSiteOk || !player) return false;
    uint32_t owner = 0, flags = 0, vtable = 0;
    const uint32_t cmc = necro::FindCmc(player);
    if (!Read(player, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva || !cmc || ((cmc + kCmcMatrix) & 15) || !Read(player + kPlayerAimOwner, &owner, 4) ||
        !owner || !Read(player + kPlayerFlags2, &flags, 4)) {
        if (InterlockedIncrement(&aimSkipped) <= 8) Event("aim from body (%s): %08X skipped (cmc %08X, aim owner %08X)", why, player, cmc, owner);
        return false;
    }
    const uint32_t flag = (flags >> 3) & 1u;
    const bool ok = CallAimUpdate(owner + kAimData1, cmc + kCmcMatrix, flag) && CallAimUpdate(owner + kAimData2, cmc + kCmcMatrix, flag);
    InterlockedIncrement(ok ? &aimResets : &aimFaults);
    if (aimResets + aimFaults <= 24) {
        float fwd[3]{};
        Read(player + kPlayerPos - 0x10, fwd, sizeof(fwd));
        Event("aim from body (%s): %08X camera aim set from the body (facing %.2f %.2f, flag %u): %s", why, player, fwd[0], fwd[2], flag, ok ? "done" : "FAULTED");
    }
    return ok;
}
// "body (x z) aim yaw a/b" for the spawn lines: the body's forward row and both aims' yaw (+4).
void FacingText(uint32_t player, char* out, size_t size) {
    float fwd[3]{}, yaw1 = 0.0f, yaw2 = 0.0f;
    uint32_t owner = 0;
    const bool body = player && Read(player + kPlayerPos - 0x10, fwd, sizeof(fwd));
    const bool aim = player && Read(player + kPlayerAimOwner, &owner, 4) && owner && Read(owner + kAimData1 + 4, &yaw1, 4) && Read(owner + kAimData2 + 4, &yaw2, 4);
    if (!body) { std::snprintf(out, size, "facing unknown"); return; }
    if (aim) std::snprintf(out, size, "facing (%.2f %.2f), yaw %.0f deg, aim yaw %.2f/%.2f", fwd[0], fwd[2], std::atan2(fwd[0], fwd[2]) * 57.29578f, yaw1, yaw2);
    else std::snprintf(out, size, "facing (%.2f %.2f), yaw %.0f deg, aim unread", fwd[0], fwd[2], std::atan2(fwd[0], fwd[2]) * 57.29578f);
}
// After a checkpoint restart the game's setup of the new Isaac moves him to the map
// origin about 1.5 s after our placement (15.10. mp_bombRun: to (0.1 -2.0 -0.2), a
// floor there, so FallRescue did not see it). A jump of more than 8 m to near the
// origin within 10 s of the spawn puts him back on his spawn point.
alignas(16) float guardMatrix[16]{};
uint32_t guardPlayer = 0;
DWORD guardSince = 0, guardSampleAt = 0;
float guardLast[3]{};
int guardPoint = -1, guardMoves = 0;
volatile LONG spawnGuardMoves = 0, spawnGuardQuiet = 0, spawnGuardFaults = 0;
void ArmSpawnGuard(uint32_t player, const float* matrix, int point) {
    std::memcpy(guardMatrix, matrix, sizeof(guardMatrix));
    guardPlayer = player;
    guardSince = guardSampleAt = GetTickCount();
    guardLast[0] = matrix[12];
    guardLast[1] = matrix[13];
    guardLast[2] = matrix[14];
    guardPoint = point;
    guardMoves = 0;
}
int PickRespawnPoint(const Point* points, const float (*pos)[3], unsigned n, const float* toward);   // below
uint32_t SpawnPlayer() {
    InterlockedIncrement(&spawnTries);
    uint32_t sim = 0, pm = 0;
    if (!Read(base + necro::kSimManager, &sim, 4) || !sim || !Read(base + necro::kPlayerManager, &pm, 4) || !pm) {
        Event("spawn player: no SimManager (%08X) or PlayerManager (%08X)", sim, pm);
        InterlockedIncrement(&spawnFailures);
        return 0;
    }
    necro::SpawnInfo info{};
    if (!GetInfo(sim, info, playerTemplate)) {
        Event("spawn player: template %08X-%08X-%08X-%08X has no instance data (global_assets not registered, or the GUID is wrong)",
              playerTemplate[0], playerTemplate[1], playerTemplate[2], playerTemplate[3]);
        InterlockedIncrement(&spawnFailures);
        return 0;
    }
    Point points[32];
    uint32_t count = 0;
    const unsigned n = ReadPoints(points, 32, count);
    float pos[32][3]{};
    for (unsigned i = 0; i < n; ++i) { pos[i][0] = points[i].matrix[12]; pos[i][1] = points[i].matrix[13]; pos[i][2] = points[i].matrix[14]; }
    // With a partner on the map, a point in use near him (a late joiner came in at
    // the map's start while the others were two areas on, 13.10.); else one per machine.
    float partners[8 * 3]{};
    const int partnerCount = PartnerPositions(partners, 8);
    const int nearPartner = partnerCount > 0 ? PickRespawnPoint(points, pos, n, partners) : -1;
    const int pick = nearPartner >= 0 ? nearPartner : coop_mpmap::ChoosePoint(pos, n, PreferredPoint(n), 0);
    // Entity::SetTransform copies with movaps: 16-byte aligned.
    alignas(16) float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (pick >= 0) {
        std::memcpy(matrix, points[pick].matrix, sizeof(matrix));
        matrix[3] = matrix[7] = matrix[11] = 0.0f;
        matrix[15] = 1.0f;
        matrix[13] += 0.2f;
    }
    const uint32_t entity = SpawnRaw(sim, info, matrix);
    uint32_t vtable = 0;
    if (!entity || !Read(entity, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva) {
        Event("spawn player: SpawnEntity gave %08X (vtable %08X, wanted %08X)%s", entity, vtable, static_cast<unsigned>(base + placement::kPlayerPrimaryVtableRva),
              faults ? " (a fault was caught)" : "");
        InterlockedIncrement(&spawnFailures);
        return 0;
    }
    // Player::Init registers a Player whose id (+0x420) is still -1 into slot 0
    // itself (RegisterWithManager 0x0086C1C0), the way the campaign's own Player
    // comes up, and that also gives him the attack-slot phase (+0xB48). Only if
    // slot 0 is not his afterwards is he registered by hand (the necro way back).
    // The game's own Init puts him on the point and dresses him in the setup's suit (lifecycle).
    if (pick >= 0) lifecycle::ArmPlacement(entity, matrix, "the arrival spawn");
    const char* birthName = nullptr;
    const uint32_t* birthSuit = lifecycle::PlacementReady() && !play::StoryCoop() && !loadsync::JoinSuitPending() ? RuleSuit(birthName) : nullptr;
    const bool birthArmed = birthSuit && coop_uniform::ArmBirth(birthSuit, "the arrival spawn (the setup's suit)");
    const bool finalized = FinalizeRaw(entity);
    if (birthArmed) coop_uniform::DisarmBirth(true);
    birthSuitWanted = birthArmed && finalized;
    const bool placedNatively = lifecycle::DisarmPlacement() == entity;
    lastSpawnPlacedNatively = placedNatively;
    uint32_t id = 0xFFFFFFFFu;
    Read(entity + necro::kPlayerIdOfPlayer, &id, 4);
    const char* registration = "by Player::Init";
    if (LocalSlotZeroEntity() != entity) {
        id = RegisterSlotZero(pm, entity);
        if (id != 0xFFFFFFFEu) WriteBytes(entity + necro::kPlayerIdOfPlayer, &id, 4);
        registration = id == 0 ? "by hand (Init did not)" : id == 0xFFFFFFFEu ? "FAULT" : "by hand, NOT slot 0";
    }
    int32_t attackPhase = -2;
    Read(entity + 0xB48, &attackPhase, 4);
    float afterInit[3]{};
    Read(entity + kPlayerPos, afterInit, sizeof(afterInit));
    // the old way when Init did not place him: our own move after it, and the spawn guard
    const int placed = placedNatively ? 3 : pick >= 0 ? PlaceRaw(entity, matrix) : 0;
    if (placed == 1 || placed == 2) AimFromBody(entity, "the arrival spawn after Init");
    if (placed > 0) ArmSpawnGuard(entity, matrix, pick);
    if (placedNatively && pick >= 0) SendSpawnUsage(points[pick].object, pick);
    InterlockedIncrement(&spawnsDone);
    Event("spawn player: %08X from template %08X-%08X at spawn point %d of %u; finalize=%s; PlayerManager id=%d registered %s; attack phase %d; "
          "Player::Init put him at (%.2f %.2f %.2f), spawn point (facing %.2f %.2f): %s; birth suit %s; slot0 now %08X",
          entity, playerTemplate[0], playerTemplate[1], pick, count, finalized ? "ok" : "FAULT", static_cast<int>(id), registration, attackPhase,
          afterInit[0], afterInit[1], afterInit[2], matrix[8], matrix[10],
          placed == 3 ? "placed by Init itself" : placed == 2 ? "moved after Init (entity+cmc)" : placed == 1 ? "moved after Init (entity only, no cmc)"
          : placed < 0 ? "FAULT" : "none, stays",
          birthArmed ? birthName : "none (old dress path)", LocalSlotZeroEntity());
    return entity;
}

// A gun the store's way (the bot weapon economy's calls): a placeholder proxy into
// a free weapon slot, then the Quartermaster's requisition streams the real gun in.
constexpr int kCutter = 0;
volatile LONG loadoutGiven = 0, loadoutFailed = 0, ammoGiven = 0;
bool GiveGun(uint32_t qm, uint32_t player, const uint32_t* guid, const char* name) {
    const unsigned char* g = reinterpret_cast<const unsigned char*>(guid);
    const int full = bot::ThisBool0(base + bot::kInventoryIsFullRva, player + bot::kWeaponInventory);
    const uint32_t proxy = full == 0 ? bot::SpawnPickupRaw(g) : 0;
    uint32_t vtable = 0;
    if (full != 0) {
        Event("loadout: weapon inventory full or unreadable (%d), no %s", full, name);
        InterlockedIncrement(&loadoutFailed);
        return false;
    }
    if (!proxy || !Read(proxy, &vtable, 4) || vtable != base + bot::kGunProxyVtableRva) {
        if (proxy) bot::DeleteRaw(proxy);
        Event("loadout: the %s placeholder %08X-%08X did not spawn (%08X vt %08X)", name, guid[0], guid[1], proxy, vtable);
        InterlockedIncrement(&loadoutFailed);
        return false;
    }
    if (bot::PickupRaw(proxy, base + bot::kItemPickupRva, player) != 1) {
        bot::DeleteRaw(proxy);
        Event("loadout: the %s placeholder did not go into a weapon slot", name);
        InterlockedIncrement(&loadoutFailed);
        return false;
    }
    const int ok = bot::RequisitionRaw(qm, player, guid);
    if (ok != 1) {
        bot::RemoveItemRaw(player + bot::kWeaponInventory, proxy);
        bot::DeleteRaw(proxy);
        InterlockedIncrement(&loadoutFailed);
    } else {
        InterlockedIncrement(&loadoutGiven);
    }
    Event("loadout: %s %08X-%08X requisition %s (placeholder %08X; the real gun replaces it once its stream is in)", name, guid[0], guid[1],
          ok == 1 ? "accepted" : ok < 0 ? "FAULTED" : "refused", proxy);
    return ok == 1;
}
void GiveAmmo(uint32_t qm, uint32_t player, const uint32_t* guid, const char* name);
bool suitFromLoadout = false;   // the setup's suit is streaming in: DressIfBare leaves the body alone
// The suit of the character setup's pick; also after a respawn, whose new body wears the
// level's own suit again (16.10.: an arena respawn came back in the kit's Engineering suit).
void GiveLoadoutSuit(uint32_t player) {
    suitFromLoadout = false;
    const char* suitName = nullptr;
    bool suitHandled = false;
    if (const uint32_t* suit = loadout::Suit(&suitName, player, &suitHandled)) {
        suitFromLoadout = coop_uniform::BotDress(player, suit);
        Event("loadout: suit %s %08X-%08X: ChangeToUniform %s", suitName, suit[0], suit[1], suitFromLoadout ? "called (streams in if needed)" : "FAILED or no uniform manager");
    } else if (suitHandled) {
        suitFromLoadout = true;   // mpsuit dresses it, now or when its skin stream is in
        Event("loadout: suit %s: dressed by mpsuit", suitName ? suitName : "?");
    }
}
// The suit rule: the setup's campaign suit in Mercenaries and Versus (null for an MP colour, which
// mpsuit dresses), the Engineering Suit everywhere else.
const uint32_t* RuleSuit(const char*& why) {
    if (loadout::ApplyMode()) {
        const char* name = nullptr;
        const uint32_t* row = loadout::SuitRow(&name);
        why = row ? "the rule: the character setup's suit (mercenaries, versus)" : "the rule: the setup's MP colour (mpsuit dresses it)";
        return row;
    }
    why = "the rule: Engineering Suit (survival, objective, hold the zone)";
    return kEngineeringSuit;
}
// The kit's guns come as placeholders that the real gun replaces once its stream is in. A client
// once stood with no gun at all after an arena restart (02.11.); the kit is checked 5 s and 15 s
// later and given once more when nothing came.
uint32_t kitCheckPlayer = 0;
DWORD kitCheckAt = 0;
int kitCheckStage = 0;
volatile LONG kitChecks = 0, kitRegiven = 0, kitStillEmpty = 0;
void GiveLoadout(uint32_t player, bool bornDressed = false);
void KitCheckTick(DWORD now) {
    if (!kitCheckPlayer || !kitCheckAt || static_cast<LONG>(now - kitCheckAt) < 0) return;
    const uint32_t player = kitCheckPlayer;
    if (player != LocalCampaignPlayer()) { kitCheckPlayer = 0; return; }
    uint32_t gunAt[bot::kGunKinds]{};
    int proxies = 0;
    const uint32_t carried = bot::CarriedGuns(player, gunAt, &proxies);
    InterlockedIncrement(&kitChecks);
    const unsigned after = kitCheckStage == 0 ? 5 : 15;
    if (carried) {
        Event("loadout check +%u s: guns %X carried, placeholders %d: the kit arrived", after, carried, proxies);
        kitCheckPlayer = 0;
        return;
    }
    if (kitCheckStage == 0) {
        Event("loadout check +5 s: no gun yet (placeholders %d), waiting for its stream", proxies);
        kitCheckStage = 1;
        kitCheckAt = now + 10000;
        return;
    }
    kitCheckPlayer = 0;
    if (proxies == 0 && InterlockedIncrement(&kitRegiven) <= 5) {
        Event("loadout check +15 s: NO GUN and no placeholder: the kit is given once more");
        GiveLoadout(player, true);
        kitCheckPlayer = 0;   // no second retry
    } else {
        InterlockedIncrement(&kitStillEmpty);
        Event("loadout check +15 s: NO GUN, %d placeholder(s) still waiting for their stream", proxies);
    }
}
void GiveLoadout(uint32_t player, bool bornDressed) {
    kitCheckPlayer = player;
    kitCheckAt = GetTickCount() + 5000;
    kitCheckStage = 0;
    if (!LoadoutReady()) { Event("loadout: a function signature did not match at start; none given"); InterlockedIncrement(&loadoutFailed); return; }
    uint32_t qm = 0;
    if (!Read(base + bot::kQuartermasterRva, &qm, 4) || !qm) { Event("loadout: no Quartermaster on this map"); InterlockedIncrement(&loadoutFailed); return; }
    uint32_t gunAt[bot::kGunKinds]{};
    int proxies = 0;
    const uint32_t carried = bot::CarriedGuns(player, gunAt, &proxies);
    // The character setup's pick in Versus and Mercenaries ([Loadout]); the plasma cutter otherwise.
    if (bornDressed) { suitFromLoadout = true; Event("loadout: born in the setup's suit, no change"); }
    else GiveLoadoutSuit(player);
    loadout::GunPick picks[3]{};
    const int picked = loadout::Guns(picks, 3);
    if (picked > 0) {
        for (int i = 0; i < picked; ++i) {
            const loadout::GunPick& p = picks[i];
            if (p.kind >= 0 && (carried & 1u << p.kind)) { Event("loadout: the %s is already carried (guns %X, placeholders %d)", p.name, carried, proxies); continue; }
            const uint32_t* given = GiveGun(qm, player, p.guid, p.name) ? p.guid : nullptr;
            if (!given && p.fallback && p.fallback != p.guid && GiveGun(qm, player, p.fallback, p.name)) given = p.fallback;
            if (given) GiveAmmo(qm, player, given, p.name);
        }
        return;
    }
    if (carried & 1u << kCutter) Event("loadout: the plasma cutter is already carried (guns %X, placeholders %d)", carried, proxies);
    else GiveGun(qm, player, bot::kGunCatalogue[kCutter].guid, "plasma cutter");
    GiveAmmo(qm, player, bot::kGunCatalogue[kCutter].guid, "plasma cutter");
}
// Ammo: the gun entry's drop GUID, picked up like a store purchase without a price.
void GiveAmmo(uint32_t qm, uint32_t player, const uint32_t* guid, const char* name) {
    const uint32_t entry = bot::GunEntryRaw(qm, reinterpret_cast<const unsigned char*>(guid));
    unsigned char drop[16]{};
    if (!entry || !Read(entry + bot::kGunEntryDrop, drop, 16)) { Event("loadout: no Quartermaster entry for the %s, no ammo", name); return; }
    int given = 0;
    const char* stop = "";
    for (int i = 0; i < ammoPacks && i < 10; ++i) {
        if (bot::CanFitRaw(drop, player + bot::kGeneralInventory) <= 0) { stop = " (no room)"; break; }
        const uint32_t item = bot::SpawnPickupRaw(drop);
        uint32_t vtable = 0, fn = 0;
        if (!item || !Read(item, &vtable, 4) || !Read(vtable + bot::kPickupSlot, &fn, 4) || fn < base + 0x1000 || fn >= base + gate::kRdataLo) {
            if (item) bot::DeleteRaw(item);
            stop = item ? " (the pack has no pickup function)" : " (the pack did not spawn)";
            break;
        }
        const int whole = bot::PickupRaw(item, fn, player);
        if (whole < 0) { stop = " (the pickup FAULTED; the pack is left in the world)"; break; }
        if (!whole) bot::DeleteRaw(item);
        ++given;
    }
    InterlockedExchangeAdd(&ammoGiven, given);
    Event("loadout: %d of %d %s ammo pack(s) picked up%s (MP bits held off %ld time(s), raw item faults %ld)", given, ammoPacks, name, stop,
          InterlockedCompareExchange(&mpquiet::scopes, 0, 0), InterlockedCompareExchange(&mpquiet::faults, 0, 0));
}
__declspec(noinline) bool AddEnergyRaw(uint32_t hc, float amount) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, float)>(base + kAddEnergy)(reinterpret_cast<void*>(static_cast<uintptr_t>(hc)), amount);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        return false;
    }
}
// Stasis and kinesis are bits 0x08 and 0x10 of Player+0x3D8 (what the level's
// player_enable_stasis / player_enable_tk do in Player::HandleEvents); the
// energy fill is the relay's job in the campaign, here a direct AddEnergy.
void EnableModules(uint32_t player) {
    uint32_t flags = 0;
    if (!Read(player + kPlayerFlags, &flags, 4)) { Event("modules: flags unreadable"); return; }
    const uint32_t want = flags | kFlagStasis | kFlagTk;
    const bool wrote = want != flags && WriteBytes(player + kPlayerFlags, &want, 4);
    const uint32_t hc = HealthComponent(player);
    float energy = -1.0f, energyMax = -1.0f, after = -1.0f;
    bool filled = false;
    if (hc && Read(hc + kDhcEnergy, &energy, 4) && Read(hc + kDhcEnergyMax, &energyMax, 4) && std::isfinite(energy) && std::isfinite(energyMax) &&
        energyMax > 0.0f && energyMax < 10000.0f && energy < energyMax && Have(sAddEnergy))
        filled = AddEnergyRaw(hc, energyMax - energy);
    if (hc) Read(hc + kDhcEnergy, &after, 4);
    Event("modules: flags %08X -> %08X (%s) stasis energy %.1f -> %.1f of %.1f (%s)", flags, want, want == flags ? "already on" : wrote ? "written" : "WRITE FAILED",
          energy, after, energyMax, !hc ? "no health component" : filled ? "filled" : "no fill needed or not possible");
}

// --- the visit itself ---
DWORD lastStatus = 0, extrasAt = 0;
uint32_t handledEpoch = 0;
int playerTries = 0;
bool playerDone = false, extrasDone = false;
char keyName[32] = "NUMPAD .";
const char* KeyName() { return keyName; }
void NameKey() {
    if (key == VK_DECIMAL) std::snprintf(keyName, sizeof(keyName), "NUMPAD .");
    else if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) std::snprintf(keyName, sizeof(keyName), "NUMPAD %d", key - VK_NUMPAD0);
    else if (key >= VK_F1 && key <= VK_F24) std::snprintf(keyName, sizeof(keyName), "F%d", key - VK_F1 + 1);
    else std::snprintf(keyName, sizeof(keyName), "KEY %d", key);
}
void Notice(const char* text) {
    const bool shown = notice::Show(text);
    Event("notice %s: %s", shown ? "shown" : "NOT shown (no player or notices off)", text);
}

void StartVisit(DWORD now) {
    const uint32_t loop = level::GameMainLoop();
    char map[128]{};
    MapOf(loop, map, sizeof(map));
    const uint32_t player = LocalCampaignPlayer();
    uint32_t death[4]{};
    Read(base + level::kDeathGuid, death, 16);
    const char* refusal = !ready ? "a required function did not match at start (see the mpmap sig lines)"
                        : !loop ? "no GameMainLoop service"
                        : !inLevel ? "not in a level"
                        : !player ? "no Isaac in PlayerManager slot 0 (necro form or loading)"
                        : !levelSettled ? "the level has not settled yet"
                        : LevelChangePending() ? "a level change is already pending"
                        : coop_mpmap::IsMpMap(map) ? "already on an MP map"
                        : mapIndex >= 0 && !MapAvailable(mapIndex) ? "the map's stream set is in no mounted archive (see the mpmap map lines)"
                        : coop_mpmap::NullGuid(death) ? "no campaign death checkpoint to come back to (reach a checkpoint first)"
                        : network && session && peerTable.Count() ? "partners are in this story session (an MP map with partners starts from the menu lobby)"
                        : nullptr;
    if (refusal) {
        Event("start REFUSED on map %s: %s", map[0] ? map : "?", refusal);
        Notice("MP MAP REFUSED - SEE DS2COOP.LOG");
        return;
    }
    if (!InstallHooks()) {
        Notice("MP MAP REFUSED - SAVES COULD NOT BE HELD");
        return;
    }
    origin = Origin{};
    std::snprintf(origin.map, sizeof(origin.map), "%s", map);
    char buffer[128]{};
    if (Read(loop + level::kStreamOffset, buffer, sizeof(buffer) - 1)) std::snprintf(origin.stream, sizeof(origin.stream), "%s", buffer);
    std::memset(buffer, 0, sizeof(buffer));
    if (Read(loop + 0x184, buffer, sizeof(buffer) - 1)) std::snprintf(origin.third, sizeof(origin.third), "%s", buffer);
    std::memcpy(origin.death, death, 16);
    Read(base + kOverrideGuid, origin.override_, 16);
    Read(player + kPlayerPos, origin.pos, sizeof(origin.pos));
    origin.player = player;
    difficulty::NoteCampaignOrigin();
    char stream[128];
    std::snprintf(stream, sizeof(stream), "%s", streamName[0] ? streamName : "");
    if (!stream[0]) std::snprintf(stream, sizeof(stream), "%s.str", mapName);
    uint32_t flags = 0;
    const uint32_t state = LmState(&flags);
    InterlockedExchange(&visitStartTick, static_cast<LONG>(now));
    Event("=== visit start: %s -> %s (stream %s) ===", origin.map, mapName, stream);
    Event("origin: map=%s stream=%s third='%s' death_checkpoint=%08X-%08X-%08X-%08X override=%08X-%08X-%08X-%08X player=%08X pos=(%.2f %.2f %.2f) "
          "level_manager state=%u (%s) flags=%04X mp_bit=%d tocs=%u",
          origin.map, origin.stream, origin.third, death[0], death[1], death[2], death[3], origin.override_[0], origin.override_[1], origin.override_[2],
          origin.override_[3], player, origin.pos[0], origin.pos[1], origin.pos[2], state, coop_mpmap::StateName(state), flags, flags & 2 ? 1 : 0, TocCount());
    LogPlayer("before leaving", player);
    visit.Request(now);
    spawnwin::Reset();   // a new map: its spawn points start from their own windows
    InterlockedExchange(&phaseMirror, static_cast<LONG>(visit.phase));
    handledEpoch = visit.epochs;
    InterlockedExchange(&sawNotRunning, 0);
    // The kept campaign checkpoint was found on the MP map too (the chapter TOCs
    // are always loaded), so the first test (09.10.) loaded mp_bombRun but put the
    // player back into the campaign rooms at his old spot. Only the GUID is
    // cleared: the death point data stays, and the way back writes the GUID back.
    if (useMapCheckpoint) {
        const uint32_t none[4]{};
        const bool cleared = WriteBytes(base + level::kDeathGuid, none, 16);
        Event("death checkpoint GUID %s for the visit (campaign %08X-%08X-%08X-%08X kept for the way back)", cleared ? "cleared" : "CLEAR FAILED",
              origin.death[0], origin.death[1], origin.death[2], origin.death[3]);
        ClearOverride("visit from the campaign; the way back sets the death checkpoint");
    }
    const bool ok = RequestLevel(loop, mapName, stream, false);
    Event("RequestLevelChange(%s, %s, clearCheckpoint=0) %s; saves are %s from now on", mapName, stream, ok ? "accepted" : "REFUSED",
          holdSaves ? "held" : "NOT held (HoldSaves=0)");
    if (!ok && useMapCheckpoint) WriteBytes(base + level::kDeathGuid, origin.death, 16);
    if (!ok) {
        visit.Abort(now);
        InterlockedExchange(&phaseMirror, static_cast<LONG>(visit.phase));
        InterlockedExchange(&visitStartTick, 0);
        Notice("MP MAP REFUSED BY THE GAME - SEE DS2COOP.LOG");
    }
}

// --- started from the main menu (runtime_coop_menu.inc) ---
// The native new game launch (UIFrontendManager::LaunchNewGame 0x00A40D40 plus
// the frontend close its callers run, 0x00A5A120), without the episode table.
constexpr uint32_t kUifm = 0x1C1C0E4, kNewGameFlagSet = 0x2B5D0, kClearLevel = 0x775870, kClearCheckpoint = 0x775840;
constexpr uint32_t kLaunchHelper = 0x1C1C0CC, kLaunchHelperCall = 0x9B8690, kFrontendClose = 0x65A120, kLaunchFrontend = 0x640700;
struct LaunchSite { const char* name; uint32_t rva; unsigned char bytes[12]; unsigned size; };
const LaunchSite kLaunchSites[] = {
    {"set 0x02021564 (cdecl)", kNewGameFlagSet, {0x8B, 0x44, 0x24, 0x04, 0xA3, 0x64, 0x15, 0x02, 0x02, 0xC3}, 10},
    {"Checkpoint::ClearLevel", kClearLevel, {0x8B, 0x0D, 0x08, 0xFC, 0x08, 0x02, 0xE9}, 7},
    {"Checkpoint::ClearCheckpoint", kClearCheckpoint, {0x8B, 0x0D, 0x04, 0xFC, 0x08, 0x02, 0x33, 0xC0, 0xA3}, 9},
    {"launch helper 0x00DB8690", kLaunchHelperCall, {0x8B, 0x81, 0x8C, 0x00, 0x00, 0x00, 0x83, 0xEC, 0x0C, 0x56, 0x57}, 11},
    {"UIFrontendManager close", kFrontendClose, {0x53, 0x56, 0x8B, 0xF1, 0x33, 0xDB, 0x39, 0x5E, 0x3C, 0x0F, 0x84}, 11},
    {"UIFrontendManager::LaunchFrontend", kLaunchFrontend, {0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xF1, 0x89, 0x86, 0x40, 0x09, 0x00}, 12},
};
bool launchSitesOk = false;
bool CheckLaunchSites() {
    if (launchSitesOk) return true;
    bool ok = true;
    for (const LaunchSite& s : kLaunchSites) {
        unsigned char actual[12]{};
        const bool match = Read(base + s.rva, actual, s.size) && std::memcmp(actual, s.bytes, s.size) == 0;
        if (!match) ok = false;
        Event("launch sig %-34s %08X %s", s.name, static_cast<unsigned>(base + s.rva), match ? "ok" : "MISMATCH");
    }
    launchSitesOk = ok;
    return ok;
}
__declspec(noinline) bool LaunchFromFrontendRaw(uint32_t loop, const char* map, const char* stream, int& step) {
    uint32_t uifm = 0, helper = 0;
    Read(base + kUifm, &uifm, 4);
    Read(base + kLaunchHelper, &helper, 4);
    if (!uifm || !helper) { step = -1; return false; }
    __try {
        step = 1;
        reinterpret_cast<void(__cdecl*)(uint32_t)>(base + kNewGameFlagSet)(1);
        step = 2;
        reinterpret_cast<void(__cdecl*)()>(base + kClearLevel)();
        step = 3;
        reinterpret_cast<void(__cdecl*)()>(base + kClearCheckpoint)();
        step = 4;
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kLaunchHelperCall)(reinterpret_cast<void*>(static_cast<uintptr_t>(helper)), 0);
        step = 5;
        difficulty::BeforeLevelRequest("main menu launch");
        using Request = bool(__thiscall*)(void*, const char*, const char*, bool, const char*);
        const bool ok = reinterpret_cast<Request>(base + level::kRequestLevelChange)(reinterpret_cast<void*>(static_cast<uintptr_t>(loop)), map, stream, true, "");
        if (!ok) return false;
        step = 6;
        reinterpret_cast<void(__thiscall*)(void*)>(base + kFrontendClose)(reinterpret_cast<void*>(static_cast<uintptr_t>(uifm)));
        step = 7;
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        Event("launch from the main menu faulted at %08X in step %d", lastFaultAddress, step);
        return false;
    }
}
__declspec(noinline) bool LaunchFrontendRaw() {
    uint32_t uifm = 0;
    if (!Read(base + kUifm, &uifm, 4) || !uifm) return false;
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kLaunchFrontend)(reinterpret_cast<void*>(static_cast<uintptr_t>(uifm)), 1);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        return false;
    }
}
// The joiner's half of a campaign co-op start: the host's map and checkpoint,
// loaded from the main menu with the new game recipe plus the checkpoint
// override (the level follow's SetOverride). Saves stay held on this machine.
constexpr uint32_t kCampaignSetOverride = 0x7757B0;
__declspec(noinline) bool LaunchCampaignRaw(uint32_t loop, const char* map, const char* stream, const uint32_t* checkpoint, int& step) {
    uint32_t uifm = 0, helper = 0;
    Read(base + kUifm, &uifm, 4);
    Read(base + kLaunchHelper, &helper, 4);
    if (!uifm || !helper) { step = -1; return false; }
    const bool haveCheckpoint = checkpoint && (checkpoint[0] | checkpoint[1] | checkpoint[2] | checkpoint[3]);
    __try {
        step = 1;
        reinterpret_cast<void(__cdecl*)(uint32_t)>(base + kNewGameFlagSet)(1);
        step = 2;
        reinterpret_cast<void(__cdecl*)()>(base + kClearLevel)();
        step = 3;
        reinterpret_cast<void(__cdecl*)()>(base + kClearCheckpoint)();
        step = 4;
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kLaunchHelperCall)(reinterpret_cast<void*>(static_cast<uintptr_t>(helper)), 0);
        step = 5;
        if (haveCheckpoint) reinterpret_cast<void(__cdecl*)(const uint32_t*)>(base + kCampaignSetOverride)(checkpoint);
        step = 6;
        difficulty::BeforeLevelRequest("campaign launch");
        using Request = bool(__thiscall*)(void*, const char*, const char*, bool, const char*);
        const bool ok = reinterpret_cast<Request>(base + level::kRequestLevelChange)(reinterpret_cast<void*>(static_cast<uintptr_t>(loop)), map,
                                                                                   stream && stream[0] ? stream : nullptr, true, "");
        if (!ok) return false;
        step = 7;
        reinterpret_cast<void(__thiscall*)(void*)>(base + kFrontendClose)(reinterpret_cast<void*>(static_cast<uintptr_t>(uifm)));
        step = 8;
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        Event("campaign launch faulted at %08X in step %d", lastFaultAddress, step);
        return false;
    }
}
void ArmJoinSuit(const char* stream, const uint32_t* checkpoint);   // below, with the campaign fall guard
bool StartCampaignFromMenu(const char* map, const char* stream, const uint32_t* checkpoint) {
    const uint32_t loop = level::GameMainLoop();
    const char* refusal = !ready ? "a required function did not match at start (see the mpmap sig lines)"
                        : !map || !map[0] ? "no map"
                        : !loop ? "no GameMainLoop service"
                        : LevelChangePending() ? "a level change is already pending"
                        : !CheckLaunchSites() ? "a launch function did not match (see the launch sig lines)"
                        : nullptr;
    if (refusal) { Event("campaign join of %s REFUSED: %s", map ? map : "?", refusal); return false; }
    if (holdSaves && !InstallHooks()) { Event("campaign join of %s REFUSED: saves could not be held", map); return false; }
    campaignClientHold = true;
    campaignClientLevelSeen = false;
    if (!checkpoint || !(checkpoint[0] | checkpoint[1] | checkpoint[2] | checkpoint[3])) ClearOverride("campaign start without a checkpoint");
    int step = 0;
    const bool ok = LaunchCampaignRaw(loop, map, stream, checkpoint, step);
    Event("=== campaign join: %s (stream %s) checkpoint %08X-%08X-%08X-%08X: %s (step %d of 8); saves are %s ===", map, stream && stream[0] ? stream : "(default)",
          checkpoint ? checkpoint[0] : 0, checkpoint ? checkpoint[1] : 0, checkpoint ? checkpoint[2] : 0, checkpoint ? checkpoint[3] : 0,
          ok ? "requested" : "FAILED", step, holdSaves ? "held" : "NOT held (HoldSaves=0)");
    if (!ok) campaignClientHold = false;
    if (ok) {
        level::NoteRoomTarget(map, checkpoint);
        ArmJoinSuit(stream, checkpoint);
    }
    return ok;
}

bool StartVisitFromMenu(const char* map, DWORD now) {
    const int index = coop_mpmap::MapIndex(map);
    const uint32_t loop = level::GameMainLoop();
    const char* refusal = !ready ? "a required function did not match at start (see the mpmap sig lines)"
                        : index < 0 ? "not in the map table"
                        : !MapAvailable(index) ? "its stream set is in no mounted archive (the Outbreak maps need DS2DAT12.DAT in the game folder)"
                        : !loop ? "no GameMainLoop service"
                        : visit.phase != coop_mpmap::Phase::Idle ? "a visit is already running"
                        : LevelChangePending() ? "a level change is already pending"
                        : !CheckLaunchSites() ? "a launch function did not match (see the launch sig lines)"
                        : nullptr;
    if (refusal) { Event("menu start of %s REFUSED: %s", map, refusal); return false; }
    if (!InstallHooks()) { Event("menu start of %s REFUSED: saves could not be held", map); return false; }
    // the block list and the player handling follow the chosen map
    mapIndex = index;
    std::snprintf(mapName, sizeof(mapName), "%s", coop_mpmap::MapName(index));
    char stream[128];
    std::snprintf(stream, sizeof(stream), "%s.str", mapName);
    origin = Origin{};
    origin.frontend = true;
    std::snprintf(origin.map, sizeof(origin.map), "frontend");
    uint32_t flags = 0;
    const uint32_t state = LmState(&flags);
    InterlockedExchange(&visitStartTick, static_cast<LONG>(now));
    Event("=== visit start from the main menu: %s (stream %s), level manager %s flags %04X ===", mapName, stream, coop_mpmap::StateName(state), flags);
    visit.Request(now);
    spawnwin::Reset();   // a new map: its spawn points start from their own windows
    InterlockedExchange(&phaseMirror, static_cast<LONG>(visit.phase));
    handledEpoch = visit.epochs;
    InterlockedExchange(&sawNotRunning, 0);
    ClearOverride("survival map from the main menu");
    int step = 0;
    const bool ok = LaunchFromFrontendRaw(loop, mapName, stream, step);
    Event("launch from the main menu: %s (step %d of 7); saves are %s", ok ? "requested" : "FAILED", step, holdSaves ? "held" : "NOT held (HoldSaves=0)");
    if (!ok) {
        visit.Abort(now);
        InterlockedExchange(&phaseMirror, static_cast<LONG>(visit.phase));
        InterlockedExchange(&visitStartTick, 0);
    }
    return ok;
}

void ReturnToCampaign(DWORD now, const char* why) {
    const uint32_t loop = level::GameMainLoop();
    if (!loop || LevelChangePending()) {
        Event("way back REFUSED (%s): %s", why, !loop ? "no GameMainLoop service" : "a level change is already pending");
        return;
    }
    if (origin.frontend) {
        // started from the main menu: back there the way the pause menu's quit goes
        InterlockedExchange(&sawNotRunning, 0);
        const bool ok = LaunchFrontendRaw();
        Event("way back (%s): LaunchFrontend(1) to the main menu %s", why, ok ? "called" : "FAULTED");
        if (ok) {
            visit.RequestReturn(now);
            InterlockedExchange(&phaseMirror, static_cast<LONG>(visit.phase));
        }
        return;
    }
    uint32_t death[4]{};
    Read(base + level::kDeathGuid, death, 16);
    const bool rewrite = std::memcmp(death, origin.death, 16) != 0;
    const bool written = !rewrite || WriteBytes(base + level::kDeathGuid, origin.death, 16);
    const bool overridden = SetOverride(origin.death);
    Event("way back (%s): death checkpoint %08X-%08X now, campaign %08X-%08X (%s), override %s", why, death[0], death[1], origin.death[0], origin.death[1],
          !rewrite ? "unchanged" : written ? "written back" : "WRITE FAILED", overridden ? "set" : "FAULT");
    difficulty::RestoreCampaignOrigin("way back to the campaign");
    InterlockedExchange(&sawNotRunning, 0);
    const bool ok = RequestLevel(loop, origin.map, origin.stream, false);
    Event("RequestLevelChange(%s, %s, clearCheckpoint=0) %s", origin.map, origin.stream, ok ? "accepted" : "REFUSED");
    if (ok) {
        visit.RequestReturn(now);
        InterlockedExchange(&phaseMirror, static_cast<LONG>(visit.phase));
    }
}

void OnPhaseChange(coop_mpmap::Phase from, DWORD now) {
    InterlockedExchange(&phaseMirror, static_cast<LONG>(visit.phase));
    uint32_t flags = 0;
    const uint32_t state = LmState(&flags);
    char map[128]{};
    MapOf(static_cast<uint32_t>(InterlockedCompareExchange(&loopCache, 0, 0)), map, sizeof(map));
    Event("phase %s -> %s (map %s, level manager %s, flags %04X, mp_bit %d, tocs %u, slot0 %08X)", coop_mpmap::PhaseName(from), coop_mpmap::PhaseName(visit.phase), map,
          coop_mpmap::StateName(state), flags, flags & 2 ? 1 : 0, TocCount(), LocalSlotZeroEntity());
    if (visit.phase == coop_mpmap::Phase::Idle) {
        Event("=== visit over on map %s after %lu s: saves released (held: %ld save(s), %ld restart point store(s)) ===", map,
              static_cast<unsigned long>((now - static_cast<DWORD>(InterlockedCompareExchange(&visitStartTick, 0, 0))) / 1000),
              InterlockedCompareExchange(&savesHeld, 0, 0), InterlockedCompareExchange(&pointsHeld, 0, 0));
        LogPlayer("after the visit", LocalCampaignPlayer());
        InterlockedExchange(&visitStartTick, 0);
        ourPlayer = 0;
    }
}

void OnArrival(DWORD now) {
    (void)now;
    // a restart reloads the world from its start: the spawn points' windows too
    // (13.10.: the host's respawn still counted a finished objective and stood
    // behind a door shut in the reset world)
    spawnwin::Reset();
    const bool early = earlyPlayer && LocalSlotZeroEntity() == earlyPlayer;
    curtainDeferred = false;
    if (arrivalCurtain && early && earlyPlaced) {
        InterlockedIncrement(&curtainsSkipped);
        std::snprintf(spawnCurtainNote, sizeof(spawnCurtainNote), "not raised: spawned and placed by Init under the loading screen");
    } else if (arrivalCurtain && !early && lifecycle::PlacementReady()) {
        curtainDeferred = true;   // the spawn follows in this tick: raised only if Init does not place him
    } else if (arrivalCurtain) {
        overlay::Curtain(kCurtainMaxMs);
        curtain = 1;
        InterlockedIncrement(&curtainsRaised);
        std::snprintf(spawnCurtainNote, sizeof(spawnCurtainNote), "raised: %s", early ? "the early body was not placed by Init" : "no native placement");
    }
    handledEpoch = visit.epochs;
    playerDone = extrasDone = false;
    playerTries = 0;
    ourPlayer = 0;
    if (!early) guardPlayer = 0;   // the early body keeps its spawn guard (a logged fallback)
    lastStatus = 0;
    if (early) {
        ourPlayer = earlyPlayer;
        playerDone = true;
        extrasAt = now - 1500;   // its kit in this tick
        Event("arrival: Isaac %08X was spawned %lu ms before the level ran, placed by Init: %s; curtain %s", earlyPlayer, static_cast<unsigned long>(now - earlyAt),
              earlyPlaced ? "yes" : "NO", spawnCurtainNote);
    } else if (earlyTried) {
        Event("arrival: the early spawn %s; the arrival spawn follows", earlyPlayer ? "is no longer slot 0" : "FAILED");
    }
    earlyPlayer = 0;
    earlyTried = false;
    uint32_t settings = 0, nrm = 0;
    Read(base + kMpLevelSettings, &settings, 4);
    Read(base + kRespawnManager, &nrm, 4);
    Event("arrived (run %u): MPLevelSettings=%08X NecroPlayerRespawnManager=%08X; the old Isaac %08X %s", visit.epochs, settings, nrm, origin.player,
          LocalSlotZeroEntity() == origin.player && origin.player ? "IS STILL IN SLOT 0" : "is gone (entities do not survive a level change)");
    LogPoints();
    LogPlayer("on arrival", LocalSlotZeroEntity());
}

// The native respawn (0x00891EC4) and the first MP spawn (0x0042B1CA) call
// MultiplayerSpawnPoint::SendOnUsageMessage (0x00431C20, thiscall, attributes
// at +0x0C, message hash at +4) after placing the player. The maps wire that
// message to the StreamScripters that show the spawn area's zones; a teleport
// alone crosses no zone trigger, so the old area stayed visible and the new one
// unlit (Concourse, 09.10.).
constexpr uint32_t kSendOnUsageRva = 0x31C20;
const unsigned char kSendOnUsageBytes[] = {0x8B, 0x41, 0x0C, 0x8B, 0x40, 0x04, 0x83, 0xEC, 0x0C, 0x85, 0xC0, 0x74};
bool spawnZoneMessage = true, sendOnUsageOk = false;
volatile LONG usageSent = 0, usageEmpty = 0, usageFaults = 0, usageUnregistered = 0;
// The dispatch walks the message table's bucket chain without a not-found
// branch, so only a registered name may go out (death_message.h, same table:
// 4096 buckets at [module+0x1C4CD8C], key at +0x10, next at +0).
bool MessageRegistered(uint32_t hash) {
    uint32_t table = 0, entry = 0;
    if (!hash || !Read(base + 0x1C4CD8C, &table, 4) || !table || !Read(table + (hash & 0xFFF) * 4, &entry, 4)) return false;
    for (unsigned step = 0; entry && step < 64; ++step) {
        uint32_t name = 0;
        if (!Read(entry + 0x10, &name, 4)) return false;
        if (name == hash) return true;
        if (!Read(entry, &entry, 4)) return false;
    }
    return false;
}
__declspec(noinline) bool SendOnUsageRaw(uint32_t point) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(base + kSendOnUsageRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(point)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void SendSpawnUsage(uint32_t point, int index) {
    if (!spawnZoneMessage || !sendOnUsageOk || !point) return;
    uint32_t attributes = 0, hash = 0;
    if (!Read(point + 0x0C, &attributes, 4) || !attributes || !Read(attributes + 4, &hash, 4) || !hash) {
        InterlockedIncrement(&usageEmpty);
        Event("put on map: spawn point %d has no usage message; the zones stay as they are", index);
        return;
    }
    if (!MessageRegistered(hash)) {
        InterlockedIncrement(&usageUnregistered);
        Event("put on map: spawn point %d usage message %08X is not registered (no listener), not sent", index, hash);
        return;
    }
    const bool ok = SendOnUsageRaw(point);
    InterlockedIncrement(ok ? &usageSent : &usageFaults);
    Event("put on map: spawn point %d usage message %08X %s (shows the zones of its area)", index, hash, ok ? "sent" : "FAULTED");
}

// A respawn's spawn point: one the map has in use now and that is valid for
// respawns (spawnwin), nearest to `toward` (the team or the place of death); -1
// when none qualifies or the points are not in the table.
volatile LONG respawnPicks = 0, respawnPickFallbacks = 0, respawnUnknownPoints = 0;
int PickRespawnPoint(const Point* points, const float (*pos)[3], unsigned n, const float* toward) {
    int best = -1;
    float bestDistance = 1e30f;
    unsigned known = 0, open = 0;
    for (unsigned i = 0; i < n; ++i) {
        uint32_t guid[4]{};
        const coop_spawnwin::Row* row = Read(points[i].object + 0x24, guid, 16) ? spawnwin::RowFor(guid) : nullptr;
        if (!row) continue;
        ++known;
        if (!row->respawn || !spawnwin::Open(*row) || !coop_mpmap::UsablePoint(pos[i])) continue;
        ++open;
        float d = 0.0f;
        if (toward) {
            // on the floor plan: a partner a storey up is still near
            const float dx = pos[i][0] - toward[0], dz = pos[i][2] - toward[2];
            d = dx * dx + dz * dz;
        }
        if (best < 0 || d < bestDistance) { best = static_cast<int>(i); bestDistance = d; }
    }
    if (known < n) InterlockedExchangeAdd(&respawnUnknownPoints, static_cast<LONG>(n - known));
    InterlockedIncrement(best >= 0 ? &respawnPicks : &respawnPickFallbacks);
    Event("put on map (respawn): %u spawn points, %u in the table, %u in use for respawns (window messages seen %ld); %s%s", n, known, open,
          InterlockedCompareExchange(&spawnwin::notes, 0, 0), best >= 0 ? "picked the one nearest to " : "none, the old choice",
          best < 0 ? "" : toward ? "the team" : "the list start");
    if (best >= 0 && toward) Event("put on map (respawn): point %d is %.1f m from it", best, std::sqrt(bestDistance));
    return best;
}

// A position worth steering toward (a partner, the place of death): not in the
// origin's column below the spawn points and not far below them. 13.10.: a partner
// falling at y -442 was the anchor and the respawn went to a point 451 m away.
bool UsableAnchor(const float* p) {
    if (!p || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    // a campaign arena has no spawn points: its rooms are the measure
    if (arena::RespawnHere()) return arena::Usable(p);
    Point points[32];
    uint32_t count = 0;
    const unsigned n = ReadPoints(points, 32, count);
    if (!n) return true;
    float lowest = 1e9f;
    for (unsigned i = 0; i < n; ++i) lowest = std::min(lowest, points[i].matrix[13]);
    if (p[1] < lowest - 15.0f) return false;
    return !(std::fabs(p[0]) < 1.0f && std::fabs(p[2]) < 1.0f && p[1] < lowest - 2.0f);
}

// A respawned Isaac straight onto a point in use, from RespawnPlayer's hook. The
// game's own setup of the new body (0x008A8270, delivered synchronously inside
// RespawnPlayer) has moved him to the level checkpoint by then, and nothing moved
// a body after our placement (13.10., 3 of 3), so he never shows at the map origin.
// Returns the point index, -1 when nothing was placed.
volatile LONG respawnPlacements = 0, respawnPlaceFailures = 0;
// The point a respawn takes (as PlaceRespawned picks it) as a placement matrix; -1 when none.
int RespawnMatrix(const float* toward, float* matrix, uint32_t* pointObject) {
    Point points[32];
    uint32_t count = 0;
    const unsigned n = ReadPoints(points, 32, count);
    if (!n) return -1;
    float pos[32][3]{};
    for (unsigned i = 0; i < n; ++i) { pos[i][0] = points[i].matrix[12]; pos[i][1] = points[i].matrix[13]; pos[i][2] = points[i].matrix[14]; }
    int pick = PickRespawnPoint(points, pos, n, toward);
    if (pick < 0) pick = coop_mpmap::ChoosePoint(pos, n, PreferredPoint(n), 0);
    if (pick < 0) return -1;
    std::memcpy(matrix, points[pick].matrix, 16 * sizeof(float));
    matrix[3] = matrix[7] = matrix[11] = 0.0f;
    matrix[15] = 1.0f;
    matrix[13] += 0.2f;
    if (pointObject) *pointObject = points[pick].object;
    return pick;
}
int PlaceRespawned(uint32_t player, const float* toward, float* placedAt) {
    alignas(16) float matrix[16];
    uint32_t object = 0;
    const int pick = RespawnMatrix(toward, matrix, &object);
    if (pick < 0) return -1;
    const int placed = PlaceRaw(player, matrix);
    if (placed > 0) AimFromBody(player, "a respawn on a spawn point");
    Event("respawn placement: %08X onto spawn point %d at (%.2f %.2f %.2f) facing (%.2f %.2f) inside RespawnPlayer's hook: %s", player, pick, matrix[12], matrix[13],
          matrix[14], matrix[8], matrix[10], placed == 2 ? "entity+cmc" : placed == 1 ? "entity only (no cmc found)" : "FAULT");
    if (placed <= 0) { InterlockedIncrement(&respawnPlaceFailures); return -1; }
    InterlockedIncrement(&respawnPlacements);
    SendSpawnUsage(object, pick);
    if (placedAt) { placedAt[0] = matrix[12]; placedAt[1] = matrix[13]; placedAt[2] = matrix[14]; }
    return pick;
}

// A respawned Isaac onto a given place (a campaign arena's point), from RespawnPlayer's
// hook as PlaceRespawned does it. True when placed.
// face: the forward row's x and z (the arena's middle), null keeps the world's +Z.
bool PlaceAt(uint32_t player, const float* at, float* placedAt, const float* face = nullptr) {
    alignas(16) float matrix[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, at[0], at[1] + 0.2f, at[2], 1.0f};
    if (face) { matrix[8] = face[0]; matrix[10] = face[1]; matrix[0] = face[1]; matrix[2] = -face[0]; }
    const int placed = PlaceRaw(player, matrix);
    if (placed > 0) AimFromBody(player, "a respawn on an arena point");
    Event("respawn placement: %08X onto (%.2f %.2f %.2f) facing (%.2f %.2f) inside RespawnPlayer's hook: %s", player, matrix[12], matrix[13], matrix[14], matrix[8],
          matrix[10], placed == 2 ? "entity+cmc" : placed == 1 ? "entity only (no cmc found)" : "FAULT");
    if (placed <= 0) { InterlockedIncrement(&respawnPlaceFailures); return false; }
    InterlockedIncrement(&respawnPlacements);
    if (placedAt) { placedAt[0] = matrix[12]; placedAt[1] = matrix[13]; placedAt[2] = matrix[14]; }
    return true;
}

// A bot's Versus respawn ([Bot] VersusDeaths): the point a player would get, near
// the team, without moving anybody or sending the point's usage message (that
// would show the zones of the bot's area on this machine).
bool RespawnSpot(const float* toward, float* out) {
    if (visit.phase != coop_mpmap::Phase::Arrived) return false;
    Point points[32];
    uint32_t count = 0;
    const unsigned n = ReadPoints(points, 32, count);
    if (!n) return false;
    float pos[32][3]{};
    for (unsigned i = 0; i < n; ++i) { pos[i][0] = points[i].matrix[12]; pos[i][1] = points[i].matrix[13]; pos[i][2] = points[i].matrix[14]; }
    const int pick = PickRespawnPoint(points, pos, n, toward);
    if (pick < 0) return false;
    std::memcpy(out, pos[pick], sizeof(pos[pick]));
    return true;
}

// A player still at campaign coordinates is nowhere near the map: he goes to a
// MultiplayerSpawnPoint (the bots follow him with their catch-up). A respawn
// takes a point the map has in use now, near the team.
void PutOnMap(uint32_t player, bool force = false, bool forRespawn = false, const float* toward = nullptr) {
    if (!putOnMap && !force) return;
    float at[3]{};
    if (!Read(player + kPlayerPos, at, sizeof(at))) return;
    Point points[32];
    uint32_t count = 0;
    const unsigned n = ReadPoints(points, 32, count);
    if (!n) { Event("put on map: no MultiplayerSpawnPoint readable, the player stays at (%.1f %.1f %.1f)", at[0], at[1], at[2]); return; }
    float pos[32][3]{};
    float nearest = 1e9f;
    unsigned nearestIndex = 0;
    for (unsigned i = 0; i < n; ++i) {
        pos[i][0] = points[i].matrix[12]; pos[i][1] = points[i].matrix[13]; pos[i][2] = points[i].matrix[14];
        const float dx = pos[i][0] - at[0], dy = pos[i][1] - at[1], dz = pos[i][2] - at[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < nearest) { nearest = d; nearestIndex = i; }
    }
    // A checkpoint restart brings Isaac back at the map origin, which can be
    // within PutOnMapMeters of a spawn point (09.10., mp_bombRun).
    const bool atOrigin = std::fabs(at[0]) < 1.0f && std::fabs(at[2]) < 1.0f;   // on the floor plan: one already falling from it too
    if (nearest <= putOnMapMeters && !force && !atOrigin) {
        Event("put on map: the player is %.1f m from the nearest spawn point, left where he is", nearest);
        // Its rooms still have to be shown: 12.10. on Concourse the player came in
        // on spawn point 0 and stood in an unrendered room until he walked on.
        SendSpawnUsage(points[nearestIndex].object, static_cast<int>(nearestIndex));
        return;
    }
    if (atOrigin) Event("put on map: the player stands at the map origin (%.2f %.2f %.2f): moved whatever the distance", at[0], at[1], at[2]);
    const int respawnPick = forRespawn ? PickRespawnPoint(points, pos, n, toward) : -1;
    const int pick = respawnPick >= 0 ? respawnPick : coop_mpmap::ChoosePoint(pos, n, PreferredPoint(n), 0);
    if (pick < 0) return;
    const coop_bot::Vec3 to = {pos[pick][0], pos[pick][1], pos[pick][2]};
    const bool moved = bot::MoveEntity(player, to);
    Event("put on map: player %08X was %.1f m from the nearest spawn point, %s spawn point %d at (%.2f %.2f %.2f)", player, nearest,
          moved ? "moved to" : "MOVE FAILED for", pick, to.x, to.y, to.z);
    if (moved) SendSpawnUsage(points[pick].object, pick);
}

void PlayerPhase(DWORD now) {
    if (visit.epochs != handledEpoch) OnArrival(now);
    // with native placement the body comes in the arrival tick, with nothing to hide
    if (!playerDone && now - visit.phaseAt >= (lifecycle::PlacementReady() ? 0u : playerWaitMs)) {
        const uint32_t slot0 = LocalSlotZeroEntity();
        if (slot0) {
            InterlockedIncrement(&ownPlayers);
            ourPlayer = slot0;
            playerDone = true;
            extrasAt = now;
            Event("the map brought a player of its own: %08X (%s); nothing spawned", slot0, IsNecroEntity(slot0) ? "NecroPlayer" : "Player");
            PutOnMap(slot0);
        } else if (!spawnPlayer) {
            playerDone = true;
            Event("no player on the map and SpawnPlayer=0: nothing to control (the test answer for blocker 2)");
        } else if (necroStart && necroStart()) {
            playerDone = true;
            InterlockedIncrement(&necroStarts);
            Event("spawn player: none, this machine starts the Versus round as a necro ghost");
        } else if (playerTries < 3 && (!playerTries || now - extrasAt >= 4000)) {
            ++playerTries;
            extrasAt = now;
            ourPlayer = SpawnPlayer();
            if (ourPlayer) { playerDone = true; extrasAt = lastSpawnPlacedNatively ? now - 1500 : now; }
            if (curtainDeferred && (ourPlayer || playerTries >= 3)) {
                curtainDeferred = false;
                if (ourPlayer && lastSpawnPlacedNatively) {
                    InterlockedIncrement(&curtainsSkipped);
                    std::snprintf(spawnCurtainNote, sizeof(spawnCurtainNote), "not raised: placed by Init in the arrival tick");
                } else {
                    overlay::Curtain(kCurtainMaxMs);
                    curtain = 1;
                    InterlockedIncrement(&curtainsRaised);
                    InterlockedIncrement(&curtainsFallback);
                    std::snprintf(spawnCurtainNote, sizeof(spawnCurtainNote), "raised: %s", ourPlayer ? "Init did not place him (old move)" : "no body spawned");
                }
            }
            else if (playerTries >= 3) { playerDone = true; Event("spawn player: gave up after 3 tries"); }
        }
    }
    if (curtain == 1 && playerDone) {
        curtain = 2;
        curtainLiftAt = now + curtainAfterMs;
        curtainSampleAt = 0;
    }
    // Down once the body stands still on the map (two samples 150 ms apart), not at a
    // fixed time: 13.10. on mp_solar 4 of 5 arrivals were moved to the map origin
    // about 0.1 s after our placement, under a curtain already gone. FallRescue puts
    // such a body back meanwhile.
    if (curtain == 2 && static_cast<LONG>(now - curtainLiftAt) >= 0) {
        const char* why = nullptr;
        float at[3]{};
        if (!ourPlayer) why = necroStarts ? "the necro ghost starts" : "no body of ours";
        else if (now - visit.phaseAt >= kCurtainMaxMs) why = "6 s after the arrival, standing or not";
        else if (Read(ourPlayer + kPlayerPos, at, sizeof(at)) && UsableAnchor(at)) {
            if (!curtainSampleAt) { std::memcpy(curtainSample, at, sizeof(at)); curtainSampleAt = now; }
            else if (now - curtainSampleAt >= 150) {
                const float dx = at[0] - curtainSample[0], dy = at[1] - curtainSample[1], dz = at[2] - curtainSample[2];
                if (dx * dx + dz * dz < 0.09f && std::fabs(dy) < 0.5f) why = "the body stands";
                std::memcpy(curtainSample, at, sizeof(at));
                curtainSampleAt = now;
            }
        } else {
            curtainSampleAt = 0;   // falling or in the origin's column: wait for the rescue
        }
        if (why) {
            curtain = 0;
            overlay::LiftCurtain();
            InterlockedIncrement(&curtainsLifted);
            Event("arrival curtain down %lu ms after the arrival (%s)", static_cast<unsigned long>(now - visit.phaseAt), why);
        }
    }
    if (ourPlayer && !extrasDone && now - extrasAt >= 1500) {
        extrasDone = true;
        InterlockedIncrement(&readyArrivals);
        if (LocalCampaignPlayer() != ourPlayer) {
            Event("extras skipped: slot 0 holds %08X, not our player %08X", LocalSlotZeroEntity(), ourPlayer);
        } else {
            if (giveLoadout) GiveLoadout(ourPlayer, birthSuitWanted);
            if (enableModules) EnableModules(ourPlayer);
            progress::StartBonus(ourPlayer, "the map arrival");
            uint32_t body[3]{};
            const bool complete = coop_uniform::BotBody(ourPlayer, body);
            if (!complete && dressIfBare && suitFromLoadout) {
                Event("body incomplete (mmi %08X bones %08X uniform %08X) while the setup's suit streams in: left alone", body[0], body[1], body[2]);
            } else if (!complete && dressIfBare) {
                const bool dressed = coop_uniform::BotDress(ourPlayer, dressSuit);
                Event("body incomplete (mmi %08X bones %08X uniform %08X): ChangeToUniform %08X-%08X %s", body[0], body[1], body[2], dressSuit[0], dressSuit[1],
                      dressed ? "called" : "FAILED or no uniform manager");
            } else {
                Event("body %s (mmi %08X bones %08X uniform %08X)", complete ? "complete" : "incomplete, DressIfBare=0", body[0], body[1], body[2]);
            }
            LogPlayer("after loadout and modules", ourPlayer);
            const int worn = bot::WornSuitIndex(ourPlayer);
            char facing[96];
            FacingText(ourPlayer, facing, sizeof(facing));
            const char* ruleWhy = "";
            RuleSuit(ruleWhy);
            Event("spawn summary: Isaac %08X native placement %s, %s, birth suit %s (wears the %s; %s), kit given at +%lu ms after the arrival, curtain %s", ourPlayer,
                  lastSpawnPlacedNatively ? "yes" : "no", facing, birthSuitWanted ? "yes" : "no (old dress path)", worn >= 0 ? bot::SuitName(worn) : "(no registered suit)",
                  ruleWhy, static_cast<unsigned long>(now - visit.phaseAt), spawnCurtainNote[0] ? spawnCurtainNote : "off ([MpMap] ArrivalCurtain=0)");
        }
    }
    const DWORD interval = now - visit.phaseAt < 60000 ? 5000 : 30000;
    if (playerDone && now - lastStatus >= interval) {
        lastStatus = now;
        const uint32_t slot0 = LocalSlotZeroEntity();
        LogPlayer("status", slot0 && !IsNecroEntity(slot0) ? slot0 : 0);
    }
}

volatile LONG tickFaults = 0;
bool stuckNoticed = false;
// The game's own setup of a new Isaac can move him to the level checkpoint, the
// map origin, after we placed him (13.10.: the joiner at (0.1 -11.3 -0.15) 1.5 s
// after his spawn, falling to y=-12257; the host after each restart). An Isaac
// alive in the origin's column below the spawn points, or far below all of them,
// goes back on a spawn point in use, near a partner.
volatile LONG fallRescues = 0;
bool BelowMap(const float* p) {
    Point points[32];
    uint32_t count = 0;
    const unsigned n = ReadPoints(points, 32, count);
    if (!n || !p) return false;
    float lowest = 1e9f;
    for (unsigned i = 0; i < n; ++i) lowest = std::min(lowest, points[i].matrix[13]);
    return p[1] < lowest - 15.0f;
}
DWORD rescueCheckAt = 0, rescuedAt = 0;
void FallRescue(DWORD now) {
    if (now - rescueCheckAt < 250) return;
    rescueCheckAt = now;
    const uint32_t player = LocalCampaignPlayer();
    uint32_t vtable = 0;
    if (!player || IsBotEntity(player) || !Read(player, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva) return;
    const uint32_t health = hits::FindHealthComponent(player);
    float left = 0.0f, at[3]{};
    if (!health || !Read(health + hits::kHealthCurrent, &left, 4) || !(left > 0.0f) || !Read(player + kPlayerPos, at, sizeof(at))) return;
    Point points[32];
    uint32_t count = 0;
    const unsigned n = ReadPoints(points, 32, count);
    if (!n) return;
    float lowest = 1e9f;
    for (unsigned i = 0; i < n; ++i) lowest = std::min(lowest, points[i].matrix[13]);
    const bool originColumn = std::fabs(at[0]) < 1.0f && std::fabs(at[2]) < 1.0f && at[1] < lowest - 2.0f;
    const bool farBelow = at[1] < lowest - 15.0f;
    if (!originColumn && !farBelow) return;
    if (rescuedAt && now - rescuedAt < 2000) return;
    rescuedAt = now;
    float partners[8 * 3]{};
    const int found = PartnerPositions(partners, 8);
    InterlockedIncrement(&fallRescues);
    Event("fall rescue: Isaac %08X at (%.1f %.1f %.1f), %s (lowest spawn point y %.1f): back on a spawn point in use%s", player, at[0], at[1], at[2],
          originColumn ? "in the map origin's column" : "far below the map", lowest, found > 0 ? ", near a partner" : "");
    PutOnMap(player, true, true, found > 0 ? partners : nullptr);
}
// --- campaign fall guard and join suit ---
// FallRescue serves MP map visits only. 24.10.: in a campaign arena and on story joins players
// fell through floors that were not there yet and kept falling. Off MP maps the guard keeps the
// last spot where Isaac stood still in gravity and puts him back there when he falls freely
// (faster than 12 m/s down and 8 m below it), outside scenes, alive. [MpMap] CampaignFallGuard.
bool campaignFallGuard = true;
alignas(16) float safeMatrix[16]{};
float fallLast[3]{};
DWORD fallSampleAt = 0, stillSince = 0, guardRescuedAt = 0;
uint32_t fallPlayer = 0;
bool haveSafe = false;
volatile LONG campaignFallRescues = 0, campaignFallFaults = 0, campaignSafeSpots = 0, campaignFallSequenceHolds = 0, campaignFallStateHolds = 0;
constexpr uint32_t kCmcFlagsOffset = 0x144;
// 19.11.: the chapter 7 launch from the solar array down to the Sprawl hub falls fast in gravity
// with no scene the scene reader knows, and the guard put Isaac back on the array. It now waits
// for a while after our Isaac's last scripted sequence, and while one of his state machines is in
// a flying or launch state.
constexpr DWORD kAfterSequenceMs = 90000;
bool FallingState(uint32_t player, char* states, size_t size) {
    laserdiag::OwnerStates(player, states, size);
    for (char* c = states; *c; ++c) *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
    // not "fall": a plain fall through a missing floor is what the guard is for
    return std::strstr(states, "fly") || std::strstr(states, "flight") || std::strstr(states, "launch") || std::strstr(states, "zg");
}
float standY[coop_peers::kMaxPeers]{};
DWORD standSince[coop_peers::kMaxPeers]{};
bool PartnerStands(DWORD now) {
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        if (!standSince[s] || now - standSince[s] < 1000) continue;
        const float* p = peerRuntime[s].pos;
        const float dx = p[0] - safeMatrix[12], dy = p[1] - safeMatrix[13], dz = p[2] - safeMatrix[14];
        if (dx * dx + dy * dy + dz * dz < 60.0f * 60.0f) return true;
    }
    return false;
}
void WatchPartners(DWORD now) {
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& r = peerRuntime[s];
        if (!peerTable.peers[s].used || !r.lastState || now - r.lastState > 1000 || std::fabs(r.pos[1] - standY[s]) > 1.0f ||
            loadsync::PartnerOnOurMap(peerTable.peers[s].token, now) == 0) {
            standSince[s] = 0;
            standY[s] = r.pos[1];
        } else if (!standSince[s]) {
            standSince[s] = now ? now : 1;
        }
    }
}
void CampaignFallGuard(DWORD now) {
    if (!campaignFallGuard || visit.phase != coop_mpmap::Phase::Idle || now - fallSampleAt < 100) return;
    const DWORD dt = fallSampleAt ? now - fallSampleAt : 0;
    fallSampleAt = now;
    WatchPartners(now);
    const uint32_t player = LocalCampaignPlayer();
    uint32_t vtable = 0;
    if (!player || IsBotEntity(player) || !Read(player, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva ||
        LmState(nullptr) != coop_mpmap::kRunning || necro::LocalRole() != 0) {
        fallPlayer = 0;
        return;
    }
    const uint32_t health = hits::FindHealthComponent(player);
    const uint32_t cmc = necro::FindCmc(player);
    float left = 0.0f, matrix[16]{};
    uint32_t flags = 0;
    if (!health || !Read(health + hits::kHealthCurrent, &left, 4) || !(left > 0.0f) || !cmc || !Read(cmc + kCmcFlagsOffset, &flags, 4) ||
        !Read(player + kPlayerPos - 0x30, matrix, sizeof(matrix)))
        return;
    const float* at = matrix + 12;
    if (player != fallPlayer) {   // a new Isaac (level start, restart, respawn): no safe spot yet
        fallPlayer = player;
        haveSafe = false;
        stillSince = 0;
        std::memcpy(fallLast, at, sizeof(fallLast));
        return;
    }
    const float vy = dt ? (at[1] - fallLast[1]) * 1000.0f / static_cast<float>(dt) : 0.0f;
    std::memcpy(fallLast, at, sizeof(fallLast));
    const bool gravity = (flags & 1) != 0;
    const bool scene = scenekind::local != scenekind::kNone;
    if (!gravity || scene) { stillSince = 0; return; }
    if (std::fabs(vy) < 1.0f) {
        if (!stillSince) stillSince = now;
        if (now - stillSince >= 500) {
            if (!haveSafe) InterlockedIncrement(&campaignSafeSpots);
            std::memcpy(safeMatrix, matrix, sizeof(safeMatrix));
            haveSafe = true;
        }
        return;
    }
    stillSince = 0;
    if (!haveSafe || vy > -12.0f || at[1] > safeMatrix[13] - 8.0f || (guardRescuedAt && now - guardRescuedAt < 2000)) return;
    const DWORD sequenceAt = static_cast<DWORD>(InterlockedCompareExchange(&cutscene::localSequenceAt, 0, 0));
    // In co-op a scripted drop takes everybody along: a partner standing near our safe spot means
    // that we fell alone, through a floor that is not there.
    const bool fellAlone = PartnerStands(now);
    if (sequenceAt && now - sequenceAt < kAfterSequenceMs && !fellAlone) {
        if (InterlockedIncrement(&campaignFallSequenceHolds) <= 10)
            Event("campaign fall guard: Isaac falling at %.1f m/s, %.1f m below his safe spot, left alone (scripted sequence %lu ms ago)", vy, safeMatrix[13] - at[1],
                  static_cast<unsigned long>(now - sequenceAt));
        return;
    }
    char states[200];
    if (FallingState(player, states, sizeof(states))) {
        if (InterlockedIncrement(&campaignFallStateHolds) <= 10)
            Event("campaign fall guard: Isaac falling at %.1f m/s, %.1f m below his safe spot, left alone (states %s)", vy, safeMatrix[13] - at[1], states);
        return;
    }
    if (InterlockedCompareExchange(&campaignFallRescues, 0, 0) <= 40)
        Event("campaign fall guard: states before the rescue: %s; last scripted sequence %s", states,
              sequenceAt ? "more than 90 s ago" : "none seen");
    guardRescuedAt = now;
    const int placed = PlaceRaw(player, safeMatrix);
    InterlockedIncrement(placed > 0 ? &campaignFallRescues : &campaignFallFaults);
    if (InterlockedCompareExchange(&campaignFallRescues, 0, 0) + InterlockedCompareExchange(&campaignFallFaults, 0, 0) <= 40)
        Event("campaign fall guard: Isaac %08X falling at %.1f m/s at (%.1f %.1f %.1f), %.1f m below his last safe spot: back to (%.1f %.1f %.1f): %s",
              player, vy, at[0], at[1], at[2], safeMatrix[13] - at[1], safeMatrix[12], safeMatrix[13], safeMatrix[14],
              placed == 2 ? "entity+cmc" : placed == 1 ? "entity only" : "FAULT");
    std::memcpy(fallLast, safeMatrix + 12, sizeof(fallLast));
}

// A story join starts a new game at the host's checkpoint: the joiner wore the patient suit
// while the host wore his (24.10.). The chapter's start suit comes on 5 s after he appears,
// only when he still wears the patient suit (an arena's kit comes first). [MpMap] JoinSuit.
bool joinSuit = true;
bool joinSuitArmed = false;
char joinSuitStream[64] = "";
uint32_t joinSuitCheckpoint[4]{};
DWORD joinSuitArmedAt = 0, joinSuitSeenAt = 0;
uint32_t joinSuitPlayer = 0;
volatile LONG joinSuitsGiven = 0, joinSuitsKept = 0, joinSuitsFailed = 0;
void ArmJoinSuit(const char* stream, const uint32_t* checkpoint) {
    if (!joinSuit) return;
    // with the host barrier the joiner is born in the chapter's suit already (loadsync)
    if (loadsync::hostBarrier) { Event("join suit: left to the loading barrier's birth suit ([Loading] HostBarrier=1)"); return; }
    std::memset(joinSuitCheckpoint, 0, sizeof(joinSuitCheckpoint));
    if (checkpoint) std::memcpy(joinSuitCheckpoint, checkpoint, sizeof(joinSuitCheckpoint));
    std::snprintf(joinSuitStream, sizeof(joinSuitStream), "%s", stream ? stream : "");
    joinSuitArmed = true;
    joinSuitArmedAt = GetTickCount();
    joinSuitPlayer = 0;
}
void JoinSuitTick(DWORD now) {
    if (!joinSuitArmed) return;
    if (now - joinSuitArmedAt > 180000) { joinSuitArmed = false; Event("join suit: no Isaac within 180 s of the join, given up"); return; }
    const uint32_t player = LocalCampaignPlayer();
    if (!player || LmState(nullptr) != coop_mpmap::kRunning || necro::LocalRole() != 0) { joinSuitPlayer = 0; return; }
    if (player != joinSuitPlayer) { joinSuitPlayer = player; joinSuitSeenAt = now; return; }
    if (now - joinSuitSeenAt < 5000) return;
    joinSuitArmed = false;
    const int worn = bot::WornSuitIndex(player);
    const bool bare = worn < 0 || worn == bot::PatientSuit();
    // by the checkpoint GUID: every campaign chapter streams ds_2.str, so the stream named only the last row
    uint32_t chapterSuit[4]{};
    const bool known = loadsync::ChapterSuit(joinSuitCheckpoint, chapterSuit);
    if (!bare || !known) {
        InterlockedIncrement(&joinSuitsKept);
        Event("join suit: Isaac %08X wears the %s; %s", player, worn >= 0 ? bot::SuitName(worn) : "(no registered suit)",
              !bare ? "kept" : "the joined checkpoint names no chapter suit, kept");
        return;
    }
    const bool dressed = coop_uniform::BotDress(player, chapterSuit);
    InterlockedIncrement(dressed ? &joinSuitsGiven : &joinSuitsFailed);
    Event("join suit: Isaac %08X (checkpoint %08X-%08X, stream %s) wore the patient suit: %s", player, joinSuitCheckpoint[0], joinSuitCheckpoint[1], joinSuitStream,
          dressed ? "changes into the chapter's start suit" : "suit change FAILED");
}

void SpawnGuard(DWORD now) {
    // armed with a fresh tick, possibly after this tick's now: wait for the clock to pass it
    if (!guardPlayer || static_cast<LONG>(now - guardSince) < 0 || now - guardSampleAt < 100) return;
    guardSampleAt = now;
    float at[3]{};
    if (LocalCampaignPlayer() != guardPlayer || !Read(guardPlayer + kPlayerPos, at, sizeof(at))) {
        Event("spawn guard: Isaac %08X is no longer slot 0 or unreadable %lu ms after the spawn; guard off", guardPlayer,
              static_cast<unsigned long>(now - guardSince));
        guardPlayer = 0;
        return;
    }
    const float dx = at[0] - guardLast[0], dz = at[2] - guardLast[2];
    const float jump = std::sqrt(dx * dx + dz * dz);
    const bool nearOrigin = std::fabs(at[0]) < 2.0f && std::fabs(at[2]) < 2.0f;
    const bool pointAway = guardMatrix[12] * guardMatrix[12] + guardMatrix[14] * guardMatrix[14] > 64.0f;
    if (jump > 8.0f && nearOrigin && pointAway && guardMoves < 3) {
        ++guardMoves;
        const int placed = PlaceRaw(guardPlayer, guardMatrix);
        if (placed > 0) InterlockedIncrement(&spawnGuardMoves);
        else InterlockedIncrement(&spawnGuardFaults);
        Event("spawn guard: Isaac %08X jumped %.1f m to (%.2f %.2f %.2f) %lu ms after the spawn: back on spawn point %d at (%.2f %.2f %.2f): %s", guardPlayer,
              jump, at[0], at[1], at[2], static_cast<unsigned long>(now - guardSince), guardPoint, guardMatrix[12], guardMatrix[13], guardMatrix[14],
              placed == 2 ? "entity+cmc" : placed == 1 ? "entity only (no cmc found)" : "FAULT");
        guardLast[0] = guardMatrix[12];
        guardLast[1] = guardMatrix[13];
        guardLast[2] = guardMatrix[14];
        return;
    }
    std::memcpy(guardLast, at, sizeof(at));
    if (now - guardSince >= kGuardMs) {
        if (!guardMoves) InterlockedIncrement(&spawnGuardQuiet);
        Event("spawn guard: Isaac %08X done %lu ms after the spawn, %d move(s) back; now at (%.2f %.2f %.2f)", guardPlayer,
              static_cast<unsigned long>(now - guardSince), guardMoves, at[0], at[1], at[2]);
        guardPlayer = 0;
    }
}

// The loading barrier asks this at LevelManager::OkToStart (runtime_coop_loadsync.inc): an MP map
// visit holds the start at the loading screen until the early spawn had its moment, 3 s at most,
// so the arrival's Isaac exists before the first frame even without partners to wait for.
DWORD earlyHoldSince = 0;
volatile LONG earlyHolds = 0, earlyHoldTimeouts = 0;
bool HoldStartForEarlySpawn(DWORD now) {
    const bool visiting = visit.phase == coop_mpmap::Phase::Leaving || visit.phase == coop_mpmap::Phase::Loading;
    // only an MP map: a campaign arena's Isaac comes from the level's own Player::Init
    if (!earlySpawn || !visiting || earlyTried || !spawnPlayer || !level::HostMpMapHere()) { earlyHoldSince = 0; return false; }
    if (!earlyHoldSince) { earlyHoldSince = now ? now : 1; InterlockedIncrement(&earlyHolds); }
    if (now - earlyHoldSince < 3000) return true;
    if (InterlockedIncrement(&earlyHoldTimeouts) <= 20) Event("early spawn: the start was held 3 s at the loading screen without a spawn; the level starts, the arrival tick spawns");
    earlyHoldSince = 0;
    earlyTried = true;   // the arrival path takes over
    return false;
}
void EarlySpawn(DWORD now, bool mapIsMp) {
    if (!earlySpawn || earlyTried || !spawnPlayer || !mapIsMp || LmState(nullptr) != kWaitForCommand || !lifecycle::PlacementReady()) return;
    if (LocalSlotZeroEntity() || (necroStart && necroStart()) || mpnative::InVersus()) return;
    Point points[32];
    uint32_t count = 0;
    if (!ReadPoints(points, 32, count)) return;   // the spawn points register with the load; wait for them
    earlyTried = true;
    earlyAt = now;
    earlyPlayer = SpawnPlayer();
    earlyPlaced = earlyPlayer && lastSpawnPlacedNatively;
    if (earlyPlayer) InterlockedIncrement(&earlySpawns);
    if (earlyPlaced) InterlockedIncrement(&earlyPlacedNative);
    Event("early spawn under the loading screen (level manager WAIT_FOR_COMMAND): Isaac %08X, placed by Init %s, birth suit %s", earlyPlayer, earlyPlaced ? "yes" : "NO",
          birthSuitWanted ? "yes" : "no");
}
void TickInner(DWORD now) {
    KitCheckTick(now);
    // a campaign joiner's save hold ends in the main menu (the Apt director only exists
    // there), once the joined level has run: the join itself starts in the main menu, and
    // checked at once the hold ended in the tick it began (13.10.)
    if (campaignClientHold) {
        uint32_t director = 0;
        const bool menu = Read(base + 0x1C8BE90, &director, 4) && director;
        if (!menu && inLevel) campaignClientLevelSeen = true;
        if (menu && campaignClientLevelSeen) {
            campaignClientHold = false;
            campaignClientLevelSeen = false;
            Event("campaign co-op over (main menu): saves released");
        }
    }
    // Idle costs a key poll per frame and nothing else.
    if (visit.phase != coop_mpmap::Phase::Idle) {
        const uint32_t loop = level::GameMainLoop();
        InterlockedExchange(&loopCache, static_cast<LONG>(loop));
        char map[128]{};
        MapOf(loop, map, sizeof(map));
        const bool mapIsMp = coop_mpmap::IsMpMap(map);
        const coop_mpmap::Phase before = visit.phase;
        if (InterlockedExchange(&sawNotRunning, 0)) visit.Observe(0, mapIsMp, now);
        visit.Observe(LmState(nullptr), mapIsMp, now);
        if (visit.phase != before) OnPhaseChange(before, now);
        if (visit.phase == coop_mpmap::Phase::Leaving || visit.phase == coop_mpmap::Phase::Loading) {
            if (before == coop_mpmap::Phase::Arrived || before == coop_mpmap::Phase::Idle) { earlyTried = false; earlyPlayer = 0; }
            EarlySpawn(now, mapIsMp);
        }
        // An accepted request whose load never began: the old level still runs a minute later.
        const bool leaving = visit.phase == coop_mpmap::Phase::Leaving, returning = visit.phase == coop_mpmap::Phase::Returning;
        if ((leaving || returning) && !visit.sawLoad && now - visit.phaseAt > 60000 && LmState(nullptr) == coop_mpmap::kRunning) {
            const coop_mpmap::Phase from = visit.phase;
            Event("the requested level change never started within 60 s; %s", leaving ? "visit called off" : "still on the MP map");
            visit.Set(leaving ? coop_mpmap::Phase::Idle : coop_mpmap::Phase::Arrived, now);
            OnPhaseChange(from, now);
        }
    } else {
        InterlockedExchange(&sawNotRunning, 0);   // a stale sample must not move the next departure on
    }
    const bool down = key && (GetAsyncKeyState(key) & 0x8000) != 0 && necro::GameWindowFocused();
    switch (press.Update(down, now)) {
    case coop_mpmap::DoublePress::kArmed: {
        char text[120];
        if (visit.phase == coop_mpmap::Phase::Idle)
            std::snprintf(text, sizeof(text), "PRESS %s AGAIN WITHIN 3 S TO LOAD %s (SAVES OFF THERE)", KeyName(), mapName);
        else
            std::snprintf(text, sizeof(text), "PRESS %s AGAIN WITHIN 3 S TO GO BACK TO THE %s", KeyName(), origin.frontend ? "MAIN MENU" : "CAMPAIGN");
        for (char* c = text; *c; ++c) *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
        Event("key %d: first press (phase %s)", key, coop_mpmap::PhaseName(visit.phase));
        Notice(text);
        break;
    }
    case coop_mpmap::DoublePress::kExpired:
        Event("key %d: no second press within 3 s, nothing done", key);
        break;
    case coop_mpmap::DoublePress::kConfirmed:
        Event("key %d: second press confirmed (phase %s)", key, coop_mpmap::PhaseName(visit.phase));
        if (visit.phase == coop_mpmap::Phase::Idle) StartVisit(now);
        else if (visit.phase == coop_mpmap::Phase::Leaving) Event("way back not yet: the departure is still being processed");
        else if (visit.phase == coop_mpmap::Phase::Returning) Event("way back already requested");
        else ReturnToCampaign(now, stuckNoticed ? "after the load watchdog fired" : "key");
        break;
    default:
        break;
    }
    if (visit.phase == coop_mpmap::Phase::Idle || visit.phase == coop_mpmap::Phase::Arrived) stuckNoticed = false;
    if (InterlockedExchange(&stuckFlag, 0) == 1) {
        stuckNoticed = true;
        char text[120];
        std::snprintf(text, sizeof(text), "MP MAP LOAD STUCK - PRESS %s TWICE TO GO BACK", KeyName());
        Notice(text);
    }
    if (visit.phase == coop_mpmap::Phase::Arrived) {
        PlayerPhase(now);
        FallRescue(now);
        SpawnGuard(now);
    }
    CampaignFallGuard(now);
    JoinSuitTick(now);
}
void Tick(DWORD now) {
    if (!enabled) return;
    InterlockedExchange(&gameThreadId, static_cast<LONG>(GetCurrentThreadId()));
    __try {
        TickInner(now);
    } __except (FaultFilter(GetExceptionInformation())) {
        if (InterlockedIncrement(&tickFaults) <= 20) Event("tick FAULT at %08X (phase %s)", lastFaultAddress, coop_mpmap::PhaseName(visit.phase));
    }
}

// --- the watcher: level manager states and the loading watchdog ---
coop_mpmap::Watchdog dog;
volatile LONG stateLines = 0, dumps = 0;
// Loaded and waiting streams in the stream manager's handle table (+0x5C buckets, +0x60 count).
void CountStreams(uint32_t& loaded, uint32_t& waiting) {
    loaded = waiting = 0;
    uint32_t manager = 0, table = 0, buckets = 0;
    if (!Read(base + kStreamManager, &manager, 4) || !manager || !Read(manager + 0x5C, &table, 4) || !Read(manager + 0x60, &buckets, 4) || !table) return;
    for (uint32_t b = 0; b < buckets && b < 8192; ++b) {
        uint32_t node = 0;
        if (!Read(table + 4 * b, &node, 4)) continue;
        for (unsigned hops = 0; node && hops < 64; ++hops) {
            uint32_t stream = 0, status = 0;
            if (Read(node + 4, &stream, 4) && stream && Read(stream + 0x24, &status, 4)) { if (status == 3) ++loaded; else ++waiting; }
            if (!Read(node + 8, &node, 4)) break;
        }
    }
}
uint32_t StreamByHandle(uint32_t handle) {
    uint32_t manager = 0, table = 0, buckets = 0, node = 0;
    if (!handle || !Read(base + kStreamManager, &manager, 4) || !manager || !Read(manager + 0x5C, &table, 4) || !Read(manager + 0x60, &buckets, 4) || !buckets ||
        !Read(table + 4 * (handle % buckets), &node, 4)) return 0;
    for (unsigned hops = 0; node && hops < 64; ++hops) {
        uint32_t nodeHandle = 0, stream = 0;
        if (!Read(node, &nodeHandle, 4)) return 0;
        if (nodeHandle == handle) return Read(node + 4, &stream, 4) ? stream : 0;
        if (!Read(node + 8, &node, 4)) return 0;
    }
    return 0;
}
// A stalled game thread (11.10. 22:11: 0 fps on the way back, both copies):
// its registers and the return addresses on its stack, copied while it is
// suspended for a moment (nothing that could need a lock it holds runs then),
// and on the second watchdog report one full dump next to the crash dumps.
volatile LONG hangSamples = 0, hangDumps = 0;
uint32_t ModuleEnd(uintptr_t module) {
    if (!module) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
    return static_cast<uint32_t>(module + nt->OptionalHeader.SizeOfImage);
}
const char* CodeOwner(uint32_t v, uint32_t& rva) {
    static const uintptr_t ours = reinterpret_cast<uintptr_t>(&__ImageBase);
    static const uintptr_t randomizer = reinterpret_cast<uintptr_t>(ds2coop::PartnerModule("DS2RandomizerObserver.asi"));
    static const uint32_t oursEnd = ModuleEnd(ours), randomizerEnd = ModuleEnd(randomizer);
    if (v >= base + 0x1000 && v < base + 0x1600000) { rva = static_cast<uint32_t>(v - base); return "exe"; }
    if (v >= ours + 0x1000 && v < oursEnd) { rva = static_cast<uint32_t>(v - ours); return "DS2Coop"; }
    if (randomizer && randomizer != ours && v >= randomizer + 0x1000 && v < randomizerEnd) { rva = static_cast<uint32_t>(v - randomizer); return "Randomizer"; }
    return nullptr;
}
void SampleGameThread() {
    if (InterlockedIncrement(&hangSamples) > 3) return;
    const DWORD tid = static_cast<DWORD>(InterlockedCompareExchange(&gameThreadId, 0, 0));
    if (!tid || tid == GetCurrentThreadId()) { Event("HANG sample: game thread unknown"); return; }
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) { Event("HANG sample: OpenThread(%lu) failed (%lu)", static_cast<unsigned long>(tid), static_cast<unsigned long>(GetLastError())); return; }
    static uint32_t stack[1024];
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    SIZE_T got = 0;
    bool haveContext = false;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        haveContext = GetThreadContext(thread, &ctx) != 0;
        // ReadProcessMemory is a kernel call; a shorter read when the stack ends sooner
        for (SIZE_T want = sizeof(stack); haveContext && want >= 256 && !got; want /= 2)
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(static_cast<uintptr_t>(ctx.Esp)), stack, want, &got)) got = 0;
        ResumeThread(thread);
    }
    CloseHandle(thread);
    if (!haveContext) { Event("HANG sample: no context for thread %lu", static_cast<unsigned long>(tid)); return; }
    uint32_t rva = 0;
    const char* eipOwner = CodeOwner(ctx.Eip, rva);
    Event("HANG sample %ld: game thread %lu eip %08X (%s+%X) esp %08X ebp %08X eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X, %lu stack bytes",
          InterlockedCompareExchange(&hangSamples, 0, 0), static_cast<unsigned long>(tid), ctx.Eip, eipOwner ? eipOwner : "?", rva, ctx.Esp, ctx.Ebp, ctx.Eax,
          ctx.Ebx, ctx.Ecx, ctx.Edx, ctx.Esi, ctx.Edi, static_cast<unsigned long>(got));
    char line[400];
    size_t used = 0;
    unsigned shown = 0;
    for (size_t i = 0; i < got / 4 && shown < 48; ++i) {
        const char* owner = CodeOwner(stack[i], rva);
        if (!owner) continue;
        used += std::snprintf(line + used, sizeof(line) - used, " +%03X=%s+%X", static_cast<unsigned>(i * 4), owner, rva);
        if (++shown % 8 == 0 || used > sizeof(line) - 40) { Event("HANG stack%s", line); used = 0; line[0] = 0; }
    }
    if (used) Event("HANG stack%s", line);
}
void FullHangDump() {
    if (InterlockedExchange(&hangDumps, 1)) return;
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    const auto write = dbghelp ? reinterpret_cast<WriteDump>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    if (!write) { Event("HANG dump: dbghelp MiniDumpWriteDump missing"); return; }
    char path[MAX_PATH];
    const char* folder = GetFileAttributesA("C:\\DS2CrashDumps") != INVALID_FILE_ATTRIBUTES ? "C:\\DS2CrashDumps" : ".";
    std::snprintf(path, sizeof(path), "%s\\deadspace2.hang.%lu.dmp", folder, static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { Event("HANG dump: cannot create %s (%lu)", path, static_cast<unsigned long>(GetLastError())); return; }
    const DWORD started = GetTickCount();
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, 2 /* MiniDumpWithFullMemory */, nullptr, nullptr, nullptr);
    CloseHandle(file);
    Event("HANG dump %s: %s in %lu ms", path, ok ? "written" : "FAILED", static_cast<unsigned long>(GetTickCount() - started));
}

void Dump(DWORD now, uint32_t state, DWORD inState) {
    InterlockedIncrement(&dumps);
    SampleGameThread();
    if (InterlockedCompareExchange(&dumps, 0, 0) >= 2) FullHangDump();
    char stream[96], toc[96];
    AcquireSRWLockShared(&nameLock);
    std::memcpy(stream, lastStream, sizeof(stream));
    std::memcpy(toc, lastToc, sizeof(toc));
    ReleaseSRWLockShared(&nameLock);
    const LONG streamAt = InterlockedCompareExchange(&lastStreamAt, 0, 0), tocAt = InterlockedCompareExchange(&lastTocAt, 0, 0);
    uint32_t loaded = 0, waiting = 0, flags = 0;
    CountStreams(loaded, waiting);
    LmState(&flags);
    Event("WATCHDOG: no loading progress for %lu s; level manager %s (%u) for %lu s, flags %04X, phase %s; last stream request '%s' %ld s ago, last toc '%s' %ld s ago; "
          "streams loaded=%u waiting=%u tocs=%u; press %s twice to try the way back",
          static_cast<unsigned long>(dog.QuietMs(now) / 1000), coop_mpmap::StateName(state), state, static_cast<unsigned long>(inState / 1000), flags,
          coop_mpmap::PhaseName(static_cast<coop_mpmap::Phase>(InterlockedCompareExchange(&phaseMirror, 0, 0))), stream,
          streamAt ? static_cast<long>((now - static_cast<DWORD>(streamAt)) / 1000) : -1L, toc, tocAt ? static_cast<long>((now - static_cast<DWORD>(tocAt)) / 1000) : -1L,
          loaded, waiting, TocCount(), KeyName());
    // The zone set: what it still loads (+0x188, count +0x240) and its queue (+0x99C).
    uint32_t manager = 0, sets = 0, zone = 0, loadingCount = 0, queued = 0, members = 0, unloading = 0;
    if (Read(base + kSetManager, &manager, 4) && manager && Read(manager + 0x10, &sets, 4) && sets && Read(sets, &zone, 4) && zone) {
        Read(zone + 0x240, &loadingCount, 4);
        Read(zone + 0x99C, &queued, 4);
        Read(zone + 0x184, &members, 4);
        Read(zone + 0xC8, &unloading, 4);
        Event("  zone set %08X: members=%u loading=%u unloading=%u queued=%u", zone, members, loadingCount, unloading, queued);
        for (uint32_t i = 0; i < loadingCount && i < 46; ++i) {
            uint32_t handle = 0;
            Read(zone + 0x188 + 4 * i, &handle, 4);
            char name[96] = "";
            const uint32_t s = StreamByHandle(handle);
            if (s) renderstall::StreamNameOf(s, name, sizeof(name));
            Event("  zone set loading handle=%08X status=%d %s", handle, StreamStatus(handle), name[0] ? name : "(no name)");
        }
    }
    // Every stream the manager has that is not loaded (status 3), at most 24.
    uint32_t table = 0, buckets = 0, mgr = 0;
    unsigned listed = 0;
    if (Read(base + kStreamManager, &mgr, 4) && mgr && Read(mgr + 0x5C, &table, 4) && Read(mgr + 0x60, &buckets, 4) && table) {
        for (uint32_t b = 0; b < buckets && b < 8192 && listed < 24; ++b) {
            uint32_t node = 0;
            Read(table + 4 * b, &node, 4);
            for (unsigned hops = 0; node && hops < 64 && listed < 24; ++hops) {
                uint32_t handle = 0, s = 0, status = 0, f = 0, refs = 0;
                Read(node, &handle, 4);
                if (Read(node + 4, &s, 4) && s && Read(s + 0x24, &status, 4) && status != 3) {
                    Read(s + 0x54, &f, 4);
                    Read(s + 0x58, &refs, 2);
                    char name[96] = "";
                    renderstall::StreamNameOf(s, name, sizeof(name));
                    Event("  stream not loaded: handle=%08X status=%d flags=%04X refs=%u %s", handle, static_cast<int>(status), f & 0xFFFF, refs & 0xFFFF, name);
                    ++listed;
                }
                if (!Read(node + 8, &node, 4)) break;
            }
        }
    }
    InterlockedExchange(&stuckFlag, 1);
}
DWORD WINAPI Watcher(void*) {
    uint32_t lastState = 0xFFFFFFFEu, lastFlags = 0;
    DWORD stateSince = GetTickCount(), lastCount = 0, activeUntil = 0;
    uint32_t loaded = 0, waiting = 0;
    for (;;) {
        const bool visiting = InVisit();
        DWORD now = GetTickCount();
        if (visiting) activeUntil = now + 30000;
        const bool active = visiting || now < activeUntil;
        Sleep(active ? 50 : 250);
        if (!active) { lastState = 0xFFFFFFFEu; continue; }
        now = GetTickCount();
        uint32_t flags = 0;
        const uint32_t state = LmState(&flags);
        if (state != coop_mpmap::kRunning) InterlockedExchange(&sawNotRunning, 1);
        if (state != lastState || flags != lastFlags) {
            if (lastState != 0xFFFFFFFEu && Budget(stateLines, 400))
                Event("loader %s (%d) -> %s (%d) after %lu ms, flags %04X mp_bit %d, phase %s", coop_mpmap::StateName(lastState), static_cast<int>(lastState),
                      coop_mpmap::StateName(state), static_cast<int>(state), static_cast<unsigned long>(now - stateSince), flags, flags & 2 ? 1 : 0,
                      coop_mpmap::PhaseName(static_cast<coop_mpmap::Phase>(InterlockedCompareExchange(&phaseMirror, 0, 0))));
            if (state != lastState) stateSince = now;
            lastState = state;
            lastFlags = flags;
        }
        if (now - lastCount >= 1000) { lastCount = now; CountStreams(loaded, waiting); }
        const uint32_t signature = state * 0x9E3779B1u ^ static_cast<uint32_t>(InterlockedCompareExchange(&streamLoads, 0, 0)) * 31u ^
                                   static_cast<uint32_t>(InterlockedCompareExchange(&tocLoads, 0, 0)) * 131u ^
                                   static_cast<uint32_t>(InterlockedCompareExchange(&desiredCalls, 0, 0)) * 1031u ^ loaded * 7u ^ waiting * 13u;
        if (Loading()) {
            if (!dog.running) dog.Start(signature, now);
            else dog.Observe(signature, now);
            if (dog.Stuck(now, watchdogMs)) Dump(now, state, now - stateSince);
        } else if (dog.running) {
            dog.Stop();
        }
    }
}

// --- map archives ---
// Which maps have their stream set in a mounted archive. The game mounts every
// DS2DAT<n>.DAT next to the exe for n below [0x01B18C30] = 13 (loop at
// 0x0042FDC0), so a map counts as available when its "<name>\<name>.str" hash
// is in one of those tables. The menu offers and the launch accepts only these.
constexpr unsigned kDatSlots = 13;
bool mapAvailable[coop_mpmap::kMapCount] = {};
unsigned mapArchiveSlot[coop_mpmap::kMapCount] = {};
bool MapAvailable(int index) { return index >= 0 && index < static_cast<int>(coop_mpmap::kMapCount) && mapAvailable[index]; }
// For the lobby's hello: bit i = kMapTable[i] can be loaded here.
uint32_t AvailableMask() {
    uint32_t mask = 0;
    for (unsigned i = 0; i < coop_mpmap::kMapCount; ++i) if (mapAvailable[i]) mask |= 1u << i;
    return mask;
}
void ScanArchives(FILE* log) {
    char dir[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = length && length < MAX_PATH ? std::strrchr(dir, '\\') : nullptr;
    if (slash) slash[1] = 0; else dir[0] = 0;
    uint32_t hashes[coop_mpmap::kMapCount];
    for (unsigned i = 0; i < coop_mpmap::kMapCount; ++i) {
        hashes[i] = coop_mpmap::MapStreamHash(coop_mpmap::kMapTable[i].name);
        mapAvailable[i] = false;
    }
    std::vector<unsigned char> table;
    std::string found;
    for (unsigned slot = 0; slot < kDatSlots; ++slot) {
        char path[MAX_PATH + 32];
        std::snprintf(path, sizeof(path), "%sDS2DAT%u.DAT", dir, slot);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        FILE* f = _fsopen(path, "rb", _SH_DENYNO);
        if (!f) { std::fprintf(log, "mpmap archive DS2DAT%u.DAT exists but could not be opened (errno %d)\n", slot, errno); continue; }
        unsigned char header[16]{};
        long count = std::fread(header, 1, sizeof(header), f) == sizeof(header) ? coop_mpmap::BigCount(header) : -1;
        if (count > 0) {
            table.resize(static_cast<size_t>(count) * 12u);
            if (std::fread(table.data(), 12, static_cast<size_t>(count), f) != static_cast<size_t>(count)) count = -1;
        }
        std::fclose(f);
        if (count < 0) {
            std::fprintf(log, "mpmap archive DS2DAT%u.DAT: no readable BIGH table (header %02X%02X%02X%02X count %08X data %08X)\n", slot, header[0], header[1], header[2],
                         header[3], coop_mpmap::BigEndian32(header + 8), coop_mpmap::BigEndian32(header + 12));
            continue;
        }
        char item[32];
        std::snprintf(item, sizeof(item), "%s%u(%ld)", found.empty() ? "" : " ", slot, count);
        found += item;
        for (unsigned i = 0; i < coop_mpmap::kMapCount; ++i)
            if (!mapAvailable[i] && count > 0 && coop_mpmap::BigHas(table.data(), static_cast<uint32_t>(count), hashes[i])) {
                mapAvailable[i] = true;
                mapArchiveSlot[i] = slot;
            }
    }
    std::fprintf(log, "mpmap archives in %s: slots %s\n", dir[0] ? dir : "(no exe path)", found.empty() ? "none" : found.c_str());
    for (unsigned i = 0; i < coop_mpmap::kMapCount; ++i) {
        if (mapAvailable[i])
            std::fprintf(log, "mpmap map %-15s \"%s\": stream %08X in DS2DAT%u.DAT\n", coop_mpmap::kMapTable[i].name, coop_mpmap::kMapTable[i].label, hashes[i],
                         mapArchiveSlot[i]);
        else
            std::fprintf(log, "mpmap map %-15s \"%s\": stream %08X in no archive, not offered%s\n", coop_mpmap::kMapTable[i].name, coop_mpmap::kMapTable[i].label,
                         hashes[i], coop_mpmap::kMapTable[i].kind == coop_mpmap::MapKind::Outbreak ? " (needs DS2DAT12.DAT in the game folder)" : "");
    }
}

// --- start ---
bool Install(FILE* log, const std::string& ini) {
    spawnwin::Watch();
    logFile = log;
    startTick = GetTickCount();
    enabled = GetPrivateProfileIntA("MpMap", "Enabled", 0, ini.c_str()) != 0;
    if (!enabled) { std::fprintf(log, "mpmap off\n"); return false; }
    key = GetPrivateProfileIntA("MpMap", "Key", VK_DECIMAL, ini.c_str());
    if (key < 0 || key > 255) key = 0;
    NameKey();
    GetPrivateProfileStringA("MpMap", "Map", "mp_bombRun", mapName, sizeof(mapName), ini.c_str());
    GetPrivateProfileStringA("MpMap", "Stream", "", streamName, sizeof(streamName), ini.c_str());
    mapIndex = coop_mpmap::MapIndex(mapName);
    if (mapIndex >= 0) std::snprintf(mapName, sizeof(mapName), "%s", coop_mpmap::MapName(mapIndex));
    holdSaves = GetPrivateProfileIntA("MpMap", "HoldSaves", 1, ini.c_str()) != 0;
    campaignFallGuard = GetPrivateProfileIntA("MpMap", "CampaignFallGuard", 1, ini.c_str()) != 0;
    joinSuit = GetPrivateProfileIntA("MpMap", "JoinSuit", 1, ini.c_str()) != 0;
    dropForeign = GetPrivateProfileIntA("MpMap", "DropForeignStreams", 1, ini.c_str()) != 0;
    watchdogMs = static_cast<DWORD>(GetPrivateProfileIntA("MpMap", "LoadWatchdogMs", 45000, ini.c_str()));
    blockMpLogic = GetPrivateProfileIntA("MpMap", "BlockMpLogic", 1, ini.c_str()) != 0;
    blockScriptSpawns = GetPrivateProfileIntA("MpMap", "BlockScriptSpawns", 1, ini.c_str()) != 0;
    groupMask = 0;
    if (GetPrivateProfileIntA("MpMap", "BlockEndRound", 1, ini.c_str())) groupMask |= 1u << coop_mpmap::kEndRound;
    if (GetPrivateProfileIntA("MpMap", "BlockRoundTimers", 1, ini.c_str())) groupMask |= 1u << coop_mpmap::kRoundTimer;
    if (GetPrivateProfileIntA("MpMap", "BlockObjectiveMarkers", 1, ini.c_str())) groupMask |= 1u << coop_mpmap::kObjectiveMarkers;
    if (GetPrivateProfileIntA("MpMap", "BlockTeamLogic", 1, ini.c_str())) groupMask |= 1u << coop_mpmap::kTeamLogic;
    if (GetPrivateProfileIntA("MpMap", "BlockMpSlashers", 1, ini.c_str())) groupMask |= 1u << coop_mpmap::kMpSlashers;
    if (GetPrivateProfileIntA("MpMap", "BlockMpGlobals", 1, ini.c_str())) groupMask |= 1u << coop_mpmap::kMpGlobals;
    keepSlasherRecords = GetPrivateProfileIntA("MpMap", "KeepSlasherRecords", 1, ini.c_str()) != 0;
    allowMpMusic = GetPrivateProfileIntA("MpMap", "AllowMpMusic", 1, ini.c_str()) != 0;
    spawnPlayer = GetPrivateProfileIntA("MpMap", "SpawnPlayer", 1, ini.c_str()) != 0;
    useMapCheckpoint = GetPrivateProfileIntA("MpMap", "UseMapCheckpoint", 1, ini.c_str()) != 0;
    putOnMap = GetPrivateProfileIntA("MpMap", "PutOnMap", 1, ini.c_str()) != 0;
    putOnMapMeters = static_cast<float>(GetPrivateProfileIntA("MpMap", "PutOnMapMeters", 60, ini.c_str()));
    spawnZoneMessage = GetPrivateProfileIntA("MpMap", "SpawnZoneMessage", 1, ini.c_str()) != 0;
    {
        unsigned char seen[sizeof(kSendOnUsageBytes)]{};
        sendOnUsageOk = Read(base + kSendOnUsageRva, seen, sizeof(seen)) && std::memcmp(seen, kSendOnUsageBytes, sizeof(seen)) == 0;
        std::fprintf(log, "mpmap sig %-44s %08X %s; spawn zone message %s\n", "MultiplayerSpawnPoint::SendOnUsageMessage", static_cast<unsigned>(base + kSendOnUsageRva),
                     sendOnUsageOk ? "ok" : "MISMATCH", !spawnZoneMessage ? "off ([MpMap] SpawnZoneMessage=0)" : sendOnUsageOk ? "on" : "off");
    }
    spawnPointWanted = GetPrivateProfileIntA("MpMap", "SpawnPoint", -1, ini.c_str());
    playerWaitMs = static_cast<DWORD>(GetPrivateProfileIntA("MpMap", "PlayerWaitMs", 500, ini.c_str()));
    const int settle = static_cast<int>(GetPrivateProfileIntA("MpMap", "SettleMs", 1500, ini.c_str()));
    settleMs = static_cast<DWORD>(settle < 250 ? 250 : settle > 4000 ? 4000 : settle);
    arrivalCurtain = GetPrivateProfileIntA("MpMap", "ArrivalCurtain", 1, ini.c_str()) != 0;
    earlySpawn = GetPrivateProfileIntA("Lifecycle", "EarlySpawn", 1, ini.c_str()) != 0 && GetPrivateProfileIntA("Lifecycle", "Enabled", 1, ini.c_str()) != 0;
    aimFromBody = GetPrivateProfileIntA("Lifecycle", "AimFromBody", 1, ini.c_str()) != 0;
    {
        // push ebp / mov ebp,esp / and esp,-16 / sub esp,84h / cmp byte [ebp+0Ch],0, and Init's first call to it
        static const unsigned char kHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x84, 0x00, 0x00, 0x00, 0x80, 0x7D, 0x0C, 0x00};
        unsigned char head[sizeof(kHead)]{}, call[5]{};
        int32_t rel = 0;
        const bool headOk = Read(base + kAimUpdateSourceRva, head, sizeof(head)) && std::memcmp(head, kHead, sizeof(head)) == 0;
        const bool callOk = Read(base + 0x4A8852, call, sizeof(call)) && call[0] == 0xE8 && (std::memcpy(&rel, call + 1, 4), base + 0x4A8857 + rel == base + kAimUpdateSourceRva);
        aimSiteOk = headOk && callOk;
        std::fprintf(log, "mpmap aim_from_body=%d: AimingData::UpdateSource %08X %s, Player::Init's call at %08X %s\n", aimFromBody ? 1 : 0,
                     static_cast<unsigned>(base + kAimUpdateSourceRva), headOk ? "matches" : "MISMATCH", static_cast<unsigned>(base + 0x4A8852),
                     callOk ? "matches" : "MISMATCH (camera aim after a move by hand stays off)");
    }
    const int curtainMs = static_cast<int>(GetPrivateProfileIntA("MpMap", "ArrivalCurtainMs", 600, ini.c_str()));
    curtainAfterMs = static_cast<DWORD>(curtainMs < 0 ? 0 : curtainMs > 3000 ? 3000 : curtainMs);
    giveLoadout = GetPrivateProfileIntA("MpMap", "GiveLoadout", 1, ini.c_str()) != 0;
    ammoPacks = GetPrivateProfileIntA("MpMap", "AmmoPacks", 3, ini.c_str());
    enableModules = GetPrivateProfileIntA("MpMap", "EnableStasisKinesis", 1, ini.c_str()) != 0;
    dressIfBare = GetPrivateProfileIntA("MpMap", "DressIfBare", 1, ini.c_str()) != 0;
    char text[64]{};
    GetPrivateProfileStringA("MpMap", "PlayerTemplate", "4BC78C36-9D70FEE7-544E4543-53574F4B", text, sizeof(text), ini.c_str());
    uint32_t parsed[4]{};
    if (coop_mpmap::ParseGuid(text, parsed)) std::memcpy(playerTemplate, parsed, 16);
    else std::fprintf(log, "mpmap PlayerTemplate '%s' is not a GUID; the default is used\n", text);
    GetPrivateProfileStringA("MpMap", "DressSuit", "30C9B946-E243D95F-54544950-32303153", text, sizeof(text), ini.c_str());
    if (coop_mpmap::ParseGuid(text, parsed)) std::memcpy(dressSuit, parsed, 16);
    else std::fprintf(log, "mpmap DressSuit '%s' is not a GUID; the default is used\n", text);
    std::fprintf(log, "mpmap config key=%d map=%s (index %d) stream=%s hold_saves=%d drop_foreign_streams=%d watchdog_ms=%lu block_mp_logic=%d groups=%02X "
                      "block_script_spawns=%d keep_slasher_records=%d spawn_player=%d template=%08X-%08X-%08X-%08X spawn_point=%d player_wait_ms=%lu settle_ms=%lu loadout=%d ammo_packs=%d "
                      "stasis_kinesis=%d dress_if_bare=%d suit=%08X-%08X\n",
                 key, mapName, mapIndex, streamName[0] ? streamName : "(map).str", holdSaves ? 1 : 0, dropForeign ? 1 : 0, static_cast<unsigned long>(watchdogMs),
                 blockMpLogic ? 1 : 0, groupMask, blockScriptSpawns ? 1 : 0, keepSlasherRecords ? 1 : 0, spawnPlayer ? 1 : 0, playerTemplate[0], playerTemplate[1], playerTemplate[2],
                 playerTemplate[3], spawnPointWanted, static_cast<unsigned long>(playerWaitMs), static_cast<unsigned long>(settleMs), giveLoadout ? 1 : 0, ammoPacks, enableModules ? 1 : 0,
                 dressIfBare ? 1 : 0, dressSuit[0], dressSuit[1]);
    std::fprintf(log, "mpmap arrival_curtain=%d after_ms=%lu (black from the arrival until the body stands, %lu ms at most)\n", arrivalCurtain ? 1 : 0,
                 static_cast<unsigned long>(curtainAfterMs), static_cast<unsigned long>(kCurtainMaxMs));
    if (mapIndex < 0) std::fprintf(log, "mpmap map %s is not in the map table; the block list then applies to every MP entity\n", mapName);
    ScanArchives(log);
    if (!holdSaves) std::fprintf(log, "mpmap WARNING HoldSaves=0: a save on the MP map would make the campaign slot boot into it\n");
    bool required = true;
    for (Site& s : sites) {
        s.ok = Matches(s);
        if (s.required && !s.ok) required = false;
        std::fprintf(log, "mpmap sig %-44s %08X %s%s\n", s.name, static_cast<unsigned>(base + s.rva), s.ok ? "ok" : "MISMATCH", s.required ? "" : " (optional)");
    }
    if (!coop_mpmap::Sorted(coop_mpmap::kMpMapEntities, kEntityCount)) { std::fprintf(log, "mpmap entity table is not sorted; MP logic blocking off\n"); blockMpLogic = false; }
    ready = required;
    press.windowMs = 3000;
    if (HANDLE thread = CreateThread(nullptr, 0, Watcher, nullptr, 0, nullptr)) CloseHandle(thread);
    std::fprintf(log, "mpmap ready=%d entities=%u loadout_functions=%d (press %s twice within 3 s in the campaign to load %s; nothing is hooked before that)\n",
                 ready ? 1 : 0, static_cast<unsigned>(kEntityCount), LoadoutReady() ? 1 : 0, KeyName(), mapName);
    return ready;
}

void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    std::fprintf(log, "mpmap spawn zone messages sent=%ld empty=%ld unregistered=%ld faults=%ld\n", InterlockedCompareExchange(&usageSent, 0, 0),
                 InterlockedCompareExchange(&usageEmpty, 0, 0), InterlockedCompareExchange(&usageUnregistered, 0, 0),
                 InterlockedCompareExchange(&usageFaults, 0, 0));
    std::fprintf(log, "mpmap respawn points picked=%ld fallbacks=%ld unknown_points=%ld | windows watched=%u messages=%ld resets=%ld\n",
                 InterlockedCompareExchange(&respawnPicks, 0, 0), InterlockedCompareExchange(&respawnPickFallbacks, 0, 0),
                 InterlockedCompareExchange(&respawnUnknownPoints, 0, 0), spawnwin::watchedCount, InterlockedCompareExchange(&spawnwin::notes, 0, 0),
                 InterlockedCompareExchange(&spawnwin::resets, 0, 0));
    std::fprintf(log, "mpmap spawn guard moves=%ld quiet=%ld faults=%ld | ready arrivals=%ld curtains raised=%ld lifted=%ld | fall rescues=%ld\n",
                 InterlockedCompareExchange(&spawnGuardMoves, 0, 0), InterlockedCompareExchange(&spawnGuardQuiet, 0, 0),
                 InterlockedCompareExchange(&spawnGuardFaults, 0, 0), InterlockedCompareExchange(&readyArrivals, 0, 0),
                 InterlockedCompareExchange(&curtainsRaised, 0, 0), InterlockedCompareExchange(&curtainsLifted, 0, 0),
                 InterlockedCompareExchange(&fallRescues, 0, 0));
    std::fprintf(log, "mpmap campaign fall_guard=%d rescues=%ld faults=%ld safe_spots=%ld held after_sequence=%ld flying_state=%ld | join_suit=%d given=%ld kept=%ld failed=%ld\n",
                 campaignFallGuard ? 1 : 0, InterlockedCompareExchange(&campaignFallRescues, 0, 0),
                 InterlockedCompareExchange(&campaignFallFaults, 0, 0), InterlockedCompareExchange(&campaignSafeSpots, 0, 0),
                 InterlockedCompareExchange(&campaignFallSequenceHolds, 0, 0), InterlockedCompareExchange(&campaignFallStateHolds, 0, 0), joinSuit ? 1 : 0,
                 InterlockedCompareExchange(&joinSuitsGiven, 0, 0), InterlockedCompareExchange(&joinSuitsKept, 0, 0),
                 InterlockedCompareExchange(&joinSuitsFailed, 0, 0));
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
    const LONG signature = InterlockedCompareExchange(&savesHeld, 0, 0) + InterlockedCompareExchange(&pointsHeld, 0, 0) * 3 +
                           InterlockedCompareExchange(&blockedStream, 0, 0) * 7 + InterlockedCompareExchange(&blockedScript, 0, 0) * 11 +
                           InterlockedCompareExchange(&membersForeign, 0, 0) * 13 + InterlockedCompareExchange(&streamLoads, 0, 0) * 17 +
                           InterlockedCompareExchange(&spawnsDone, 0, 0) * 19 + InterlockedCompareExchange(&faults, 0, 0) * 23 + InterlockedCompareExchange(&phaseMirror, 0, 0) * 29;
    if (signature == lastSignature || now - lastSummary < 5000) return;
    lastSignature = signature;
    lastSummary = now;
    std::fprintf(log, "mpmap summary phase=%s saves_held=%ld saves_completed=%ld complete_mismatch=%ld complete_faults=%ld points_held=%ld saves_passed=%ld points_passed=%ld set_desired=%ld members=%ld foreign=%ld dropped=%ld "
                      "stream_requests=%ld tocs=%ld toc_refused=%ld blocked_stream=%ld blocked_script=%ld records_kept=%ld watched=%ld spawn_tries=%ld spawns=%ld spawn_failures=%ld "
                      "own_players=%ld loadout=%ld/%ld ammo=%ld watchdog_dumps=%ld faults=%ld lines=%ld over_budget=%ld lost=%ld\n",
                 coop_mpmap::PhaseName(static_cast<coop_mpmap::Phase>(InterlockedCompareExchange(&phaseMirror, 0, 0))),
                 InterlockedCompareExchange(&savesHeld, 0, 0), InterlockedCompareExchange(&savesCompleted, 0, 0),
                 InterlockedCompareExchange(&saveCompleteMismatch, 0, 0), InterlockedCompareExchange(&saveCompleteFaults, 0, 0),
                 InterlockedCompareExchange(&pointsHeld, 0, 0), InterlockedCompareExchange(&savesPassed, 0, 0),
                 InterlockedCompareExchange(&pointsPassed, 0, 0), InterlockedCompareExchange(&desiredCalls, 0, 0), InterlockedCompareExchange(&membersSeen, 0, 0),
                 InterlockedCompareExchange(&membersForeign, 0, 0), InterlockedCompareExchange(&membersDropped, 0, 0), InterlockedCompareExchange(&streamLoads, 0, 0),
                 InterlockedCompareExchange(&tocLoads, 0, 0), InterlockedCompareExchange(&tocRefused, 0, 0), InterlockedCompareExchange(&blockedStream, 0, 0),
                 InterlockedCompareExchange(&blockedScript, 0, 0), InterlockedCompareExchange(&recordsKept, 0, 0), InterlockedCompareExchange(&watchedSeen, 0, 0),
                 InterlockedCompareExchange(&spawnTries, 0, 0),
                 InterlockedCompareExchange(&spawnsDone, 0, 0), InterlockedCompareExchange(&spawnFailures, 0, 0), InterlockedCompareExchange(&ownPlayers, 0, 0),
                 InterlockedCompareExchange(&loadoutGiven, 0, 0), InterlockedCompareExchange(&loadoutFailed, 0, 0), InterlockedCompareExchange(&ammoGiven, 0, 0),
                 InterlockedCompareExchange(&dumps, 0, 0), InterlockedCompareExchange(&faults, 0, 0), InterlockedCompareExchange(&linesWritten, 0, 0),
                 InterlockedCompareExchange(&linesOverBudget, 0, 0), InterlockedCompareExchange(&linesLost, 0, 0));
    // Spawn requests of listed entities, one line per class that was asked for.
    char line[600];
    size_t used = 0;
    line[0] = 0;
    for (size_t i = 0; i < kEntityCount; ++i) {
        const char* cls = coop_mpmap::kMpMapEntities[i].cls;
        bool first = true;
        for (size_t k = 0; k < i && first; ++k) if (coop_mpmap::kMpMapEntities[k].cls == cls || std::strcmp(coop_mpmap::kMpMapEntities[k].cls, cls) == 0) first = false;
        if (!first) continue;
        LONG total = 0;
        for (size_t k = i; k < kEntityCount; ++k)
            if (std::strcmp(coop_mpmap::kMpMapEntities[k].cls, cls) == 0) total += InterlockedCompareExchange(&requested[k], 0, 0);
        if (total) AppendReport(line, sizeof(line), used, " %s=%ld", cls, total);
    }
    if (used) std::fprintf(log, "mpmap spawn requests by class:%s (blocked by group: end_round=%ld round_timer=%ld objective_markers=%ld team_logic=%ld mp_slashers=%ld)\n", line,
                           InterlockedCompareExchange(&blockedByGroup[1], 0, 0), InterlockedCompareExchange(&blockedByGroup[2], 0, 0),
                           InterlockedCompareExchange(&blockedByGroup[3], 0, 0), InterlockedCompareExchange(&blockedByGroup[4], 0, 0),
                           InterlockedCompareExchange(&blockedByGroup[5], 0, 0));
}
}  // namespace mpmap
