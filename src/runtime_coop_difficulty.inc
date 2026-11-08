// Dead Space 2 United by Richard Hafer.
// The game's own difficulty for the game modes and co-op ([Difficulty], coop_difficulty.h).
// Evidence: analysis/DIFFICULTY_NATIVE_2026-10-20.md.
// - Pick (0 off, 1 the SP02 screen, 2 the difficulty row in the map picker): any picker sets the
//   pick (SetPick for a row that holds it); a held pick skips the screen.
//   The map picker's select (MP20) opens the game's difficulty screen (SP02, hash
//   0xE037EC08) first. Its accept writes the two globals LaunchNewGame writes
//   (0x01F6031C difficulty, 0x01F60320 lowest) and starts the match; back returns to the
//   picker. Hard Core is locked in this route (the unlock is only synced by SP01), so
//   AllowHardcore opens the row. A pick we own never reaches the native Select 0x00AAC060:
//   its stale load reason would load a save or start a campaign.
// - LockInMatch: in survival and the other game modes the in-game SP02 refuses every pick,
//   on every machine, and goes back to the options screen with the native Cancel.
// - Sync: the host's value is every machine's. The host sends it on survnet kind 13
//   ((generation << 8) | difficulty) three times on a change and every 2 s; a client's change
//   in the pause menu goes to the host as kind 14 and comes back as the host's next
//   generation. The lobby launch and the campaign packet carry it before the level loads.
// The four SP02 slots (vtable 0x01B38250) are shared by the front end and the in-game
// instance at UIComponentManager+0x9C4; every handler asks the front end test first.
bool SurvivalSession();   // runtime_coop.inc, after this file
namespace difficulty {
using coop_difficulty::Valid; using coop_difficulty::Name; using coop_difficulty::kCount; using coop_difficulty::kNormal; using coop_difficulty::kHardcore;
using coop_difficulty::kNone; using coop_difficulty::LowestAfterChange; using coop_difficulty::PickAllowed; using coop_difficulty::Route;
using coop_difficulty::RouteSelect; using coop_difficulty::FromLobbyByte; using coop_difficulty::SurvnetValue; using coop_difficulty::HostWire;
using coop_difficulty::RequestWire; using coop_difficulty::kFlagLocked; using coop_difficulty::Leader; using coop_difficulty::Follower;
using coop_difficulty::RequestLog; using coop_difficulty::Verdict; using coop_difficulty::Judge; using coop_difficulty::LocalPick;
using coop_difficulty::ClientStep; using coop_difficulty::DecideClient;
// Pick: 0 off, 1 the SP02 screen, 2 the map picker's difficulty row (while no row holds a pick the screen asks)
int pickMode = 1;
bool syncOn = true, lockOn = true, allowHardcore = true, hudOn = true, probeOn = false;
bool installed = false, globalsOk = false, slotsOk = false, pickReady = false, refreshOk = false, popOk = false;
// RVAs (image base 0x400000)
constexpr uint32_t kDifficulty = 0x1B6031C, kLowest = 0x1B60320, kTimesPlayed = 0x1B60300;
constexpr uint32_t kUifm = 0x1C1C0E4, kFrontendBlock = 0x1C89C38, kDirector = 0x1C8BE90, kUiOptions = 0x1C21448, kPlayerSettings = 0x1C87C38;
constexpr uint32_t kSelectSlot = 0x17382D0, kCancelSlot = 0x17382D4, kShowSlot = 0x173829C, kInitSlot = 0x17382CC;
constexpr uint32_t kSelect = 0x6AC060, kCancel = 0x68B110, kShow = 0x698AF0, kInit = 0x668FA0;
constexpr uint32_t kPopScreen = 0x687A80, kCurrentScreen = 0x634F00, kUpdateItem = 0x11E9800;
constexpr uint32_t kGetDifficulty = 0x634070, kSetLowest = 0x634080, kLaunchWrites = 0x640D58;
constexpr uint32_t kSp02Hash = 0xE037EC08;
// SP02 screen fields (Show 0x00A98AF0, InitMenuParams 0x00A68FA0)
constexpr uint32_t kRowsEnabled = 0x230, kHardcoreRow = 0x234, kNewGamePlus = 0x228, kInitialRow = 0x158, kBuildPending = 0x224, kActive = 0x4C, kList = 0x5C;
constexpr DWORD kRefreshMs = 2000, kHoldMs = 10000, kArmMs = 120000;

volatile LONG lines = 0, linesDropped = 0;
void Line(const char* format, ...) {
    if (InterlockedIncrement(&lines) > 800) { InterlockedIncrement(&linesDropped); return; }
    char text[400];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    mpmap::Event("difficulty: %s", text);
}

int ReadInt(uint32_t rva) {
    int32_t v = -1;
    return Read(base + rva, &v, 4) ? v : -1;
}
int Current() { return ReadInt(kDifficulty); }
bool InMatch() { return SurvivalSession() || play::VersusMode(); }
bool InFrontEnd() {
    uint32_t uifm = 0, flags = 0, block = 0;
    return Read(base + kUifm, &uifm, 4) && uifm && Read(uifm + 0x38, &flags, 4) && (flags >> 2 & 1u) && Read(base + kFrontendBlock, &block, 4) && block;
}
uint32_t Director() {
    uint32_t d = 0;
    return Read(base + kDirector, &d, 4) ? d : 0;
}
__declspec(noinline) uint32_t TopScreen() {
    const uint32_t director = Director();
    if (!director) return 0;
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*)>(base + kCurrentScreen)(reinterpret_cast<void*>(static_cast<uintptr_t>(director)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// The PlayerSettings scales of difficulty d: taken, dealt (outgoing is 1 / dealt) and ammo.
void Scales(int d, float& taken, float& dealt, float& ammo) {
    taken = dealt = ammo = 1.0f;
    static const uint32_t kOffsets[kCount][3] = {{0x14, 0x10, 0x0C}, {0, 0, 0}, {0x20, 0x1C, 0x18}, {0x2C, 0x28, 0x24}, {0x38, 0x34, 0x30}};
    uint32_t ps = 0, block = 0;
    if (!Valid(d) || d == kNormal || !Read(base + kPlayerSettings, &ps, 4) || !ps || !Read(ps + 0xC, &block, 4) || !block) return;
    Read(block + kOffsets[d][0], &taken, 4);
    Read(block + kOffsets[d][1], &dealt, 4);
    Read(block + kOffsets[d][2], &ammo, 4);
}

volatile LONG writes = 0, writeFailures = 0, launchWrites = 0, origins = 0, originRestores = 0;
int ownWriteValue = -1;
DWORD ownWriteAt = 0;
// The two globals as LaunchNewGame writes them (launch: both = d) or as the in-game popup
// does (lowest = min). Game thread.
bool Write(int d, bool launch, const char* why) {
    if (!globalsOk || !Valid(d)) return false;
    const int before = Current(), lowestBefore = ReadInt(kLowest);
    const int32_t value = d, lowest = launch ? d : LowestAfterChange(lowestBefore, d);
    const bool ok = WriteBytes(base + kDifficulty, &value, 4) && WriteBytes(base + kLowest, &lowest, 4);
    const int after = Current(), lowestAfter = ReadInt(kLowest);
    InterlockedIncrement(ok && after == d ? &writes : &writeFailures);
    ownWriteValue = d;
    ownWriteAt = GetTickCount();
    float taken = 1, dealt = 1, ammo = 1;
    Scales(after, taken, dealt, ammo);
    Line("%s: %s -> %s (lowest %d -> %d) %s; damage taken x%.2f, outgoing x%.2f, world ammo x%.2f", why, Name(before), Name(after), lowestBefore, lowestAfter,
         ok && after == d ? "written" : "WRITE FAILED", taken, dealt > 0.01f ? 1.0f / dealt : 1.0f, ammo);
    return ok && after == d;
}

// --- the value before a level request ----------------------------------------------------
int launchValue = -1;
char launchSource[64] = "";
void SetLaunch(int d, const char* source) {
    launchValue = syncOn && Valid(d) ? d : -1;
    std::snprintf(launchSource, sizeof(launchSource), "%s", source ? source : "?");
    Line("launch value from %s: %s%s", launchSource, Valid(d) ? Name(d) : "none", Valid(d) && !syncOn ? " (ignored: [Difficulty] Sync=0)" : "");
}
Follower follower;
// From the raw launches in runtime_coop_mpmap.inc, right before RequestLevelChange. The
// SpareAmmo of the new level is scaled with this value on its first tick.
void BeforeLevelRequest(const char* why) {
    if (!installed) return;
    int d = launchValue;
    const char* source = launchSource;
    if (!Valid(d) && syncOn && NetRole() == 2 && session && Valid(follower.value)) {
        d = follower.value;
        source = "the host's last survnet value";
    }
    launchValue = -1;
    if (!Valid(d)) {
        Line("%s: level request with the difficulty as it is (%s, lowest %d)", why, Name(Current()), ReadInt(kLowest));
        return;
    }
    char tag[128];
    std::snprintf(tag, sizeof(tag), "%s, from %s", why, source);
    if (Write(d, true, tag)) InterlockedIncrement(&launchWrites);
}
uint8_t LobbyByte() { return syncOn && globalsOk ? coop_difficulty::LobbyByte(Current()) : kNone; }
uint16_t CampaignField() { return syncOn && globalsOk ? coop_difficulty::CampaignField(Current()) : 0; }

// A survival visit from the campaign: the campaign's value goes back on the way back
// (checkpoint restarts do not restore UIOptions).
int originValue = -1, originLowest = -1;
void NoteCampaignOrigin() {
    originValue = Current();
    originLowest = ReadInt(kLowest);
    InterlockedIncrement(&origins);
    Line("campaign visit: the campaign plays %s (lowest %d), kept for the way back", Name(originValue), originLowest);
}
void RestoreCampaignOrigin(const char* why) {
    if (!Valid(originValue)) return;
    const int now = Current();
    if (now == originValue) {
        Line("%s: the campaign's %s is still set", why, Name(originValue));
    } else if (globalsOk) {
        Write(originValue, false, why);
        const int32_t lowest = Valid(originLowest) ? originLowest : originValue;
        WriteBytes(base + kLowest, &lowest, 4);
        InterlockedIncrement(&originRestores);
    }
    originValue = originLowest = -1;
}

// --- the pick: what the next match from the map picker launches with ------------------------
// Any picker sets it. The SP02 screen sets it for one launch; a row in the map picker
// (Pick=2, runtime_coop_gamemode.inc) calls SetPick and holds it, and SP02 is then skipped.
int pick = -1;
bool pickHeld = false;
char pickSource[48] = "";
// For a picker UI: hold `row` (0..4) as the pick, -1 clears it. Game thread.
bool SetPick(int row) {
    const int before = pick;
    pick = Valid(row) ? row : -1;
    pickHeld = Valid(pick);
    std::snprintf(pickSource, sizeof(pickSource), "%s", pickHeld ? "a row in the map picker" : "");
    if (before != pick) Line("pick %s -> %s (%s)", Name(before), Valid(pick) ? Name(pick) : "none", pickHeld ? "held" : "cleared");
    return pickHeld || row < 0;
}
int PickValue() { return pick; }
// The name a row shows: the pick, else what a launch would play now (CASUAL ... HARD CORE).
const char* PickName() { return coop_difficulty::HudName(Valid(pick) ? pick : Current()); }
// Both globals from the pick, and the launch value the level request writes again.
bool ApplyPick(const char* why) {
    if (!Valid(pick)) return false;
    char tag[96];
    std::snprintf(tag, sizeof(tag), "%s (%s)", why, pickSource[0] ? pickSource : "?");
    const bool written = Write(pick, true, tag);
    SetLaunch(pick, pickSource[0] ? pickSource : why);
    return written;
}
// The rest of the map picker's select (OnMp20Select's tail): true when the lobby screen
// opened, else the launch runs on the next game tick (menu::Tick).
bool LaunchPicked(uint32_t entry, const char* map, bool lobbyScreen) {
    if (lobbyScreen && nativelobby::HostWants() && nativelobby::OpenFromPicker(entry, map)) {
        mpmap::Event("menu: %s after the difficulty screen; lobby screen open", map);
        return true;
    }
    std::snprintf(menu::pendingMap, sizeof(menu::pendingMap), "%s", map);
    menu::pendingAt = GetTickCount();
    InterlockedExchange(&menu::launchPending, 1);
    mpmap::Event("menu: %s after the difficulty screen; launch on the next game tick", map);
    return false;
}

// --- the SP02 screen in the main menu ------------------------------------------------------
bool owned = false, committed = false, lobbyDeferred = false, rowRefreshWanted = false;
uint32_t pickEntry = 0, ownedScreen = 0;
char pickMap[64] = "";
DWORD pushedAt = 0, notTopSince = 0, deferredAt = 0, rowRefreshSince = 0;
int lastPick = -1;
LONG launchesAtCommit = 0, refusedAtCommit = 0;
volatile LONG pushes = 0, pushFailures = 0, shows = 0, inits = 0, picks = 0, pickRefusals = 0, pickRepeats = 0, cancels = 0, forwards = 0;
volatile LONG inGameRefusals = 0, inGameSame = 0, inGameForwards = 0, lobbyOpens = 0, lobbyFallbacks = 0, rowRefreshes = 0, faults = 0, ownLost = 0;

void LoadFlow(char* out, size_t size) {
    uint32_t uifm = 0, flags = 0, flow[4]{};
    Read(base + kUifm, &uifm, 4);
    if (uifm) { Read(uifm + 0x38, &flags, 4); Read(uifm + 0x96C, flow, sizeof(flow)); }
    uint32_t options = 0, records = 0, id = 0;
    unsigned char unlocked = 0;
    if (Read(base + kUiOptions, &options, 4) && options && Read(options + 0x160, &records, 4) && records) {
        Read(records + 0x40, &id, 4);
        Read(records + 0x44, &unlocked, 1);
    }
    std::snprintf(out, size, "uifm %08X +0x38 %08X block %08X, load flow reason %u slot %d episode %u difficulty %u, hard core unlock id %08X byte %u, times played %d", uifm, flags,
                  static_cast<unsigned>(ReadInt(kFrontendBlock)), flow[0], static_cast<int>(flow[1]), flow[2], flow[3], id, unlocked, ReadInt(kTimesPlayed));
}
void ScreenState(uint32_t s, char* out, size_t size) {
    unsigned char rows[5]{}, plus = 0, pending = 0, active = 0;
    int32_t initial = -1;
    Read(s + kRowsEnabled, rows, 5);
    Read(s + kNewGamePlus, &plus, 1);
    Read(s + kBuildPending, &pending, 1);
    Read(s + kActive, &active, 1);
    Read(s + kInitialRow, &initial, 4);
    std::snprintf(out, size, "rows %u%u%u%u%u new_game_plus %u highlighted %d build_pending %u active %u", rows[0], rows[1], rows[2], rows[3], rows[4], plus,
                  static_cast<int>(initial), pending, active);
}

// From OnMp20Select (runtime_coop_menu.inc), map resolved and allowed: true when the
// difficulty screen went up and its accept starts the match. A pick a row holds is
// written here and the select goes on without the screen.
bool rowMissingLogged = false;
bool OpenPicker(uint32_t entry, const char* map) {
    if (!installed || !pickMode) return false;
    if (pickHeld && Valid(pick)) {
        ApplyPick("the map picker's select");
        Line("map picker select for %s: the held pick %s, no difficulty screen", map, Name(pick));
        return false;
    }
    if (!pickReady) return false;
    if (pickMode == 2 && !rowMissingLogged) {
        rowMissingLogged = true;
        Line("Pick=2: no row holds a pick (a frontend without the difficulty row m2?); the difficulty screen asks");
    }
    if (!InFrontEnd()) { Line("picker for %s skipped: not in the front end", map); return false; }
    if (owned && TopScreen() == kSp02Hash) {   // the same press again
        InterlockedIncrement(&pickRepeats);
        return true;
    }
    const uint32_t director = Director();
    const uint32_t topBefore = TopScreen();
    uint32_t transition = 0;
    if (director) Read(director + 0x43C, &transition, 4);
    char flow[240];
    LoadFlow(flow, sizeof(flow));
    pickEntry = entry;
    std::snprintf(pickMap, sizeof(pickMap), "%s", map);
    owned = true;
    committed = lobbyDeferred = false;
    pushedAt = GetTickCount();
    notTopSince = 0;
    const bool pushed = menu::PushScreen(kSp02Hash);
    const uint32_t topAfter = TopScreen();
    // a push that left the stack as it was: the picker is not up, the launch goes ahead without it
    const bool up = pushed && (topAfter == kSp02Hash || (!topAfter && !topBefore));
    InterlockedIncrement(up ? &pushes : &pushFailures);
    Line("SP02 push for %s: director %08X transition %u, top %08X -> %08X, %s; %s; the global is %s (lowest %d)", map, director, transition, topBefore, topAfter,
         up ? "pushed" : pushed ? "NOT on top (launch without the picker)" : "FAILED (no director)", flow, Name(Current()), ReadInt(kLowest));
    if (!up) owned = false;
    return up;
}
// Back in the campaign's own route (SP01 select): SP02 is the game's again.
void OnCampaignScreen() {
    if (!pickHeld) launchValue = -1;   // a one-shot pick never reaches the campaign's own launch
    if (!owned && !lobbyDeferred) return;
    Line("SP01 select: the campaign route, the difficulty screen is native again (owned %d)", owned ? 1 : 0);
    owned = committed = lobbyDeferred = false;
}

__declspec(noinline) bool NativeSelect(void* screen, int row) {
    __try {
        return reinterpret_cast<bool(__thiscall*)(void*, int)>(base + kSelect)(screen, row);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return true;
    }
}
__declspec(noinline) bool NativeCancel(void* screen) {
    __try {
        return reinterpret_cast<bool(__thiscall*)(void*)>(base + kCancel)(screen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return true;
    }
}
__declspec(noinline) int NativeShow(void* screen) {
    __try {
        return reinterpret_cast<int(__thiscall*)(void*)>(base + kShow)(screen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return 3;
    }
}
__declspec(noinline) void NativeInit(void* screen) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(base + kInit)(screen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}
__declspec(noinline) bool PopScreen() {
    const uint32_t director = Director();
    if (!director || !popOk) return false;
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(base + kPopScreen)(reinterpret_cast<void*>(static_cast<uintptr_t>(director)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}
// Row 4 drawn again with the state we wrote (UIExternScrollMenuIterator::UpdateItem, thiscall(row, 0), ret 8).
__declspec(noinline) bool RefreshRow(uint32_t screen, int row) {
    if (!refreshOk) return false;
    __try {
        reinterpret_cast<void(__thiscall*)(void*, int, int)>(base + kUpdateItem)(reinterpret_cast<void*>(static_cast<uintptr_t>(screen + kList)), row, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}
bool RowRefreshable(uint32_t s) {
    unsigned char pending = 1, active = 0;
    return Read(s + kBuildPending, &pending, 1) && !pending && Read(s + kActive, &active, 1) && active;
}

int __fastcall OnShow(void* screen, void*) {
    const int result = NativeShow(screen);
    InterlockedIncrement(&shows);
    const uint32_t s = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(screen));
    if (!owned || !InFrontEnd()) return result;
    ownedScreen = s;
    char native[160], after[160];
    ScreenState(s, native, sizeof(native));
    // a stale REPEAT_GAME reason must not lock Hard Core as New Game+
    const unsigned char zero = 0, one = 1;
    WriteBytes(s + kNewGamePlus, &zero, 1);
    if (allowHardcore) WriteBytes(s + kHardcoreRow, &one, 1);
    const bool refreshed = RowRefreshable(s) && RefreshRow(s, kHardcore);
    if (refreshed) InterlockedIncrement(&rowRefreshes);
    rowRefreshWanted = !refreshed;
    rowRefreshSince = GetTickCount();
    ScreenState(s, after, sizeof(after));
    Line("SP02 show %08X (result %d, %lu ms after the push): native %s; ours %s; hard core row %s", s, result, static_cast<unsigned long>(GetTickCount() - pushedAt), native,
         after, refreshed ? "drawn again" : "drawn on the next update");
    return result;
}
void __fastcall OnInit(void* screen, void*) {
    NativeInit(screen);
    InterlockedIncrement(&inits);
    if (!owned || !InFrontEnd()) return;
    const uint32_t s = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(screen));
    int32_t native = -1;
    Read(s + kInitialRow, &native, 4);
    if (Valid(lastPick)) {
        const int32_t row = lastPick;
        WriteBytes(s + kInitialRow, &row, 4);
    }
    Line("SP02 init %08X: highlighted row %d -> %d%s", s, static_cast<int>(native), Valid(lastPick) ? lastPick : static_cast<int>(native), Valid(lastPick) ? " (the last pick)" : "");
}

// The accepted pick: both globals, then the tail of the map picker's select.
void Commit(int row) {
    lastPick = row;
    committed = true;
    InterlockedIncrement(&picks);
    Line("SP02 pick: row %d (%s) for %s, %lu ms after the push", row, Name(row), pickMap, static_cast<unsigned long>(GetTickCount() - pushedAt));
    // for this launch only: the next select asks again
    pick = row;
    pickHeld = false;
    std::snprintf(pickSource, sizeof(pickSource), "%s", "the difficulty screen");
    ApplyPick("the difficulty screen's accept");
    launchesAtCommit = InterlockedCompareExchange(&menu::launches, 0, 0);
    refusedAtCommit = InterlockedCompareExchange(&menu::refused, 0, 0);
    // the lobby screen opens from the picker, after its authored MP20 -> MP00 move
    if (nativelobby::HostWants()) {
        if (PopScreen()) {
            owned = false;
            lobbyDeferred = true;
            deferredAt = GetTickCount();
            Line("SP02 popped; the lobby screen opens once the director is back on the map picker");
            return;
        }
        Line("SP02 pop FAILED; the lobby screen opens from here");
        if (LaunchPicked(pickEntry, pickMap, true)) { InterlockedIncrement(&lobbyOpens); owned = false; return; }
    } else {
        LaunchPicked(pickEntry, pickMap, false);
    }
}

DWORD lastHintAt = 0;
LocalPick localPick;
bool __fastcall OnSelect(void* screen, void*, int row) {
    const uint32_t s = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(screen));
    const bool front = InFrontEnd();
    const bool match = !front && InMatch();
    const Route route = RouteSelect(front, owned, match, lockOn);
    unsigned char rowOn = 0;
    if (row >= 0 && row < kCount) Read(s + kRowsEnabled + static_cast<uint32_t>(row), &rowOn, 1);
    if (route == Route::Own) {
        if (committed || lobbyDeferred) {
            InterlockedIncrement(&pickRepeats);
            return true;   // the same press again, or a launch is under way
        }
        if (!PickAllowed(row, rowOn != 0, allowHardcore)) {
            InterlockedIncrement(&pickRefusals);
            Line("SP02 pick row %d REFUSED: enabled byte %u, AllowHardcore=%d", row, rowOn, allowHardcore ? 1 : 0);
            return false;
        }
        Commit(row);
        return true;
    }
    const int current = Current();
    if (route == Route::Refuse) {
        if (row == current) {
            InterlockedIncrement(&inGameSame);
            NativeCancel(screen);   // the native close for the row already set
            return true;
        }
        InterlockedIncrement(&inGameRefusals);
        const DWORD now = GetTickCount();
        if (now - lastHintAt > 1000) {
            lastHintAt = now;
            overlay::ShowHint("The difficulty is fixed for this match", "survival and the game modes keep the difficulty they started with, on every machine",
                              "pick it on the difficulty screen before the next match", 8000);
            Line("in-game SP02 pick row %d (%s) REFUSED: a match runs (survival %d, versus %d, role %d); the global stays %s; back to the options", row, Name(row),
                 SurvivalSession() ? 1 : 0, play::VersusMode() ? 1 : 0, NetRole(), Name(current));
        }
        NativeCancel(screen);
        return true;
    }
    if (front) {
        InterlockedIncrement(&forwards);
        return NativeSelect(screen, row);   // the campaign's own route
    }
    // story: the native popup; a change it makes is this player's (a client asks the host)
    InterlockedIncrement(&inGameForwards);
    if (row != current && Valid(row)) localPick.Arm(row, GetTickCount());
    Line("in-game SP02 pick row %d (%s, enabled %u) -> native (the global is %s, role %d, sync %d)", row, Name(row), rowOn, Name(current), NetRole(), syncOn ? 1 : 0);
    return NativeSelect(screen, row);
}
bool __fastcall OnCancel(void* screen, void*) {
    if (owned && InFrontEnd()) {
        InterlockedIncrement(&cancels);
        Line("SP02 back to the map picker after %lu ms (no pick for %s)", static_cast<unsigned long>(GetTickCount() - pushedAt), pickMap);
        owned = committed = false;
        launchValue = -1;
    }
    return NativeCancel(screen);
}

// --- sync ----------------------------------------------------------------------------------
Leader leader;
RequestLog requestLog;
uint32_t ackToken = 0, ackRequest = 0;
volatile LONG hostChanges = 0, hostSends = 0, hostRefreshes = 0, received = 0, applied = 0, same = 0, stale = 0, held = 0, ignored = 0;
volatile LONG requestsSent = 0, requestsIn = 0, requestsApplied = 0, requestsSame = 0, requestsRefused = 0, requestTimeouts = 0, restores = 0;
void SendHost(int repeats, const char* why) {
    HostWire w;
    w.difficulty = coop_difficulty::LobbyByte(leader.value);
    w.flags = InMatch() && lockOn ? kFlagLocked : 0;
    w.generation = leader.generation;
    w.ackToken = ackToken;
    w.ackRequest = ackRequest;
    survnet::QueueDifficulty(&w, sizeof(w), SurvnetValue(leader.generation, leader.value), repeats);
    InterlockedIncrement(repeats > 1 ? &hostSends : &hostRefreshes);
    if (repeats > 1) Line("host sends %s, generation %u%s (%s)", Name(leader.value), leader.generation, w.flags & kFlagLocked ? ", locked for the match" : "", why);
}
struct OutRequest { survnet::Packet packet; DWORD at; int left; };
OutRequest outRequest{};
void Ask(int d, DWORD now) {
    const uint32_t request = follower.Ask(d, now);
    RequestWire w;
    w.difficulty = coop_difficulty::LobbyByte(d);
    w.token = peerReadiness.token;
    w.request = request;
    w.baseGeneration = follower.base;
    outRequest.packet = survnet::Packet{survnet::kMagic, survnet::kVersion, 14, 0, peerReadiness.token, request, {}};
    std::memcpy(outRequest.packet.text, &w, sizeof(w));
    const uint32_t value = SurvnetValue(follower.base, d);
    std::memcpy(outRequest.packet.text + sizeof(outRequest.packet.text) - 4, &value, 4);
    outRequest.at = 0;
    outRequest.left = 3;
    InterlockedIncrement(&requestsSent);
    Line("client change to %s in the pause menu: request %u to the host (base generation %u, the host's value %s)", Name(d), request, follower.base, Name(follower.value));
}
// survnet kinds 13 and 14 (runtime_coop.inc survnet::OnPacket), game thread.
bool OnSurvnet(uint8_t kind, uint32_t token, const void* text, uint32_t value) {
    InterlockedIncrement(&received);
    if (!installed || !syncOn) { InterlockedIncrement(&ignored); return true; }
    const DWORD now = GetTickCount();
    if (kind == 13) {
        if (IsHost()) { InterlockedIncrement(&ignored); return false; }
        HostWire w;
        std::memcpy(&w, text, sizeof(w));
        const int d = FromLobbyByte(w.difficulty);
        const int before = follower.value;
        const uint32_t genBefore = follower.generation;
        const Follower::Take take = follower.OnHost(token, w.generation, d, w.ackToken, w.ackRequest, peerReadiness.token, now, kHoldMs);
        if (take == Follower::Take::Held) { InterlockedIncrement(&held); return true; }
        if (take == Follower::Take::Stale) { InterlockedIncrement(&stale); return true; }
        if (take == Follower::Take::Ignored) { InterlockedIncrement(&ignored); return true; }
        const int global = Current();
        const bool answer = w.ackToken == peerReadiness.token && w.ackRequest;
        if (global == d || !inLevel) {
            InterlockedIncrement(&same);
            if (before != d || answer)
                Line("host value %s -> %s, generation %u -> %u (value word %08X)%s; the global is %s%s", Name(before), Name(d), genBefore, w.generation, value,
                     answer ? ", the answer to our request" : "", Name(global), inLevel ? "" : " (not in a level: written before the next level request)");
            return true;
        }
        InterlockedIncrement(&applied);
        char why[96];
        std::snprintf(why, sizeof(why), "the host's generation %u%s", w.generation, answer ? " (our request's answer)" : "");
        Write(d, false, why);
        return true;
    }
    if (kind == 14) {
        if (!IsHost()) { InterlockedIncrement(&ignored); return false; }
        RequestWire w;
        std::memcpy(&w, text, sizeof(w));
        if (!requestLog.First(w.token, w.request)) return true;
        InterlockedIncrement(&requestsIn);
        const int d = FromLobbyByte(w.difficulty), current = Current();
        const bool locked = InMatch() && lockOn;
        const Verdict v = Judge(d, current, locked);
        ackToken = w.token;
        ackRequest = w.request;
        if (v == Verdict::Apply) {
            InterlockedIncrement(&requestsApplied);
            char why[96];
            std::snprintf(why, sizeof(why), "client %08X's request %u", w.token, w.request);
            if (Write(d, false, why)) leader.value = d;
        } else {
            InterlockedIncrement(v == Verdict::Same ? &requestsSame : &requestsRefused);
            Line("client %08X's request %u for %s (base generation %u): %s; the host keeps %s", w.token, w.request, Name(d), w.baseGeneration,
                 v == Verdict::Same ? "already set" : v == Verdict::RefusedLocked ? "REFUSED, a match runs" : "REFUSED, no difficulty", Name(current));
        }
        leader.value = Current();
        leader.Bump();
        SendHost(3, "the answer to a request");
        return true;
    }
    return false;
}

// --- probes ([Difficulty] Probe) --------------------------------------------------------------
namespace probe {
constexpr uint32_t kBefore = 0x47D0A4, kAfter = 0x47D0BE, kScaleAmount = 0x9B3FC0;
safetyhook::MidHook beforeHook, afterHook;
safetyhook::InlineHook scaleHook;
uint32_t lastData = 0, lastPlayer = 0, lastFlags = 0;
float lastBefore = 0.0f;
unsigned hitLines = 0, scaleLines = 0;
// game thread only, flushed by Tick every 10 s
unsigned hits = 0, local = 0, ghost = 0, bot = 0, flagged = 0, otherPath = 0, scaleCalls = 0, scaleBuckets[5]{};
float ratioSum = 0.0f, ratioMin = 0.0f, ratioMax = 0.0f;
DWORD flushedAt = 0;
void OnBefore(SafetyHookContext& ctx) {
    lastData = static_cast<uint32_t>(ctx.esi);
    lastPlayer = static_cast<uint32_t>(ctx.ebx);
    lastFlags = 0;
    lastBefore = 0.0f;
    Read(lastData + 0x54, &lastFlags, 4);
    Read(lastData + 0x50, &lastBefore, 4);
}
void OnAfter(SafetyHookContext& ctx) {
    const uint32_t data = static_cast<uint32_t>(ctx.esi);
    if (!lastData || data != lastData) { ++otherPath; lastData = 0; return; }   // the MP branch or a hit that skipped the SP path
    lastData = 0;
    float after = 0.0f;
    Read(data + 0x50, &after, 4);
    const float ratio = lastBefore > 0.0001f ? after / lastBefore : 1.0f;
    const bool isLocal = lastPlayer == LocalCampaignPlayer(), isBot = IsBotEntity(lastPlayer);
    ++hits;
    ++(isLocal ? local : isBot ? bot : ghost);
    if (lastFlags & 0x04000000u) ++flagged;
    if (hits == 1 || ratio < ratioMin) ratioMin = ratio;
    if (hits == 1 || ratio > ratioMax) ratioMax = ratio;
    ratioSum += ratio;
    if (hitLines < 10) {
        ++hitLines;
        uint32_t mpFlags = 0;
        Read(lastPlayer + 0x3D8, &mpFlags, 4);
        float taken = 1, dealt = 1, ammo = 1;
        const int d = Current();
        Scales(d, taken, dealt, ammo);
        Line("probe hit %u on %s %08X: %.2f -> %.2f (x%.3f, %s expects x%.2f), Player+0x3D8 %08X (MP bit %u), DamageData+0x54 %08X", hitLines,
             isLocal ? "the local Isaac" : isBot ? "a bot" : "another Player (ghost)", lastPlayer, lastBefore, after, ratio, Name(d), lastFlags & 0x04000000u ? 1.0f : taken,
             mpFlags, mpFlags >> 13 & 1u, lastFlags);
    }
}
void __fastcall OnScaleAmount(void* item, void*, float scale) {
    const uint32_t i = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(item));
    int32_t before = 0, after = 0;
    Read(i + 0x260, &before, 4);
    scaleHook.thiscall<void>(item, scale);
    Read(i + 0x260, &after, 4);
    ++scaleCalls;
    const int bucket = scale > 1.25f ? 0 : scale > 0.8f ? 1 : scale > 0.55f ? 2 : scale > 0.45f ? 3 : 4;
    ++scaleBuckets[bucket];
    if (scaleLines < 20) {
        ++scaleLines;
        Line("probe ammo %u: SpareAmmo %08X %d -> %d with scale %.2f (%s)", scaleLines, i, static_cast<int>(before), static_cast<int>(after), scale, Name(Current()));
    }
}
void Flush(DWORD now) {
    if (!flushedAt) flushedAt = now;
    if (now - flushedAt < 10000) return;
    flushedAt = now;
    if (!hits && !otherPath && !scaleCalls) return;
    float taken = 1, dealt = 1, ammo = 1;
    Scales(Current(), taken, dealt, ammo);
    Line("probe 10 s: hits %u (local %u, ghost %u, bot %u, flagged %u) ratio mean %.3f min %.3f max %.3f, expected x%.2f; MP path %u | ammo scales %u (1.5: %u, 1.0: %u, 0.6: %u, 0.5: %u, other: %u)",
         hits, local, ghost, bot, flagged, hits ? ratioSum / hits : 0.0f, ratioMin, ratioMax, taken, otherPath, scaleCalls, scaleBuckets[0], scaleBuckets[1], scaleBuckets[2],
         scaleBuckets[3], scaleBuckets[4]);
    hits = local = ghost = bot = flagged = otherPath = scaleCalls = 0;
    for (unsigned& b : scaleBuckets) b = 0;
    ratioSum = ratioMin = ratioMax = 0.0f;
}
}  // namespace probe

// --- the game tick -------------------------------------------------------------------------
volatile LONG mirrorGlobal = -1, mirrorLowest = -1, mirrorGeneration = 0, mirrorHostValue = -1, mirrorHostGeneration = 0, mirrorPending = 0, mirrorOwned = 0, mirrorMatch = 0;
volatile LONG mirrorPick = -1;
int lastSeen = -2;
void PickTick(DWORD now) {
    if (lobbyDeferred) {
        const uint32_t director = Director();
        uint32_t transition = 1;
        if (director) Read(director + 0x43C, &transition, 4);
        const uint32_t top = TopScreen();
        const bool late = now - deferredAt > 5000;
        if ((!transition && top == nativelobby::kMp20Hash) || late) {
            lobbyDeferred = false;
            const bool opened = LaunchPicked(pickEntry, pickMap, true);
            InterlockedIncrement(opened ? &lobbyOpens : &lobbyFallbacks);
            Line("lobby screen for %s %s after %lu ms (top %08X, transition %u)", pickMap, opened ? "opened" : "NOT opened: direct launch", static_cast<unsigned long>(now - deferredAt),
                 top, transition);
            if (!opened) committed = false;
        }
        return;
    }
    if (!owned) return;
    if (!InFrontEnd()) {
        Line("left the front end with the picker owned (launch of %s %s)", pickMap, committed ? "under way" : "never picked");
        owned = committed = false;
        return;
    }
    // a direct launch the menu refused (a joiner lacks the map): the screen stays, pick again or back
    if (committed && !InterlockedCompareExchange(&menu::launchPending, 0, 0) && InterlockedCompareExchange(&menu::launches, 0, 0) == launchesAtCommit &&
        InterlockedCompareExchange(&menu::refused, 0, 0) != refusedAtCommit) {
        committed = false;
        Line("the launch of %s was refused; the difficulty screen stays", pickMap);
    }
    const uint32_t top = TopScreen();
    if (top && top != kSp02Hash) {
        if (!notTopSince) notTopSince = now;
        if (now - notTopSince > 2000 && !committed) {
            InterlockedIncrement(&ownLost);
            Line("SP02 no longer on top (top %08X for 2 s): the picker is given up", top);
            owned = false;
            return;
        }
    } else {
        notTopSince = 0;
    }
    if (rowRefreshWanted && ownedScreen) {
        if (RowRefreshable(ownedScreen)) {
            rowRefreshWanted = false;
            if (RefreshRow(ownedScreen, kHardcore)) InterlockedIncrement(&rowRefreshes);
            Line("SP02 hard core row drawn again %lu ms after the show", static_cast<unsigned long>(now - rowRefreshSince));
        } else if (now - rowRefreshSince > 3000) {
            rowRefreshWanted = false;
        }
    }
}
void SyncTick(DWORD now) {
    const int global = Current();
    if (!syncOn || !network) { lastSeen = global; return; }
    if (IsHost()) {
        const int before = leader.value;
        if (leader.Observe(global)) {
            InterlockedIncrement(&hostChanges);
            const bool mine = global == ownWriteValue && now - ownWriteAt < 5000;
            const bool menuPick = localPick.Matches(global, now, kArmMs);
            Line("host change %s -> %s (generation %u): %s", Name(before), Name(global), leader.generation,
                 mine ? "written by this plugin" : menuPick ? "the pause menu's difficulty screen" : "another writer (a save's load or the game)");
            if (menuPick) localPick.Clear();
            ackToken = ackRequest = 0;
            SendHost(3, "a change");
            leader.refreshAt = now;
        } else if (leader.RefreshDue(now, kRefreshMs)) {
            SendHost(1, "refresh");
        }
        lastSeen = global;
        return;
    }
    // client
    if (follower.Expired(now, kHoldMs)) {
        InterlockedIncrement(&requestTimeouts);
        Line("the host did not answer request %u for %s within %lu s; its value %s counts again", follower.request, Name(follower.requested),
             static_cast<unsigned long>(kHoldMs / 1000), Name(follower.value));
    }
    if (outRequest.left > 0 && (!outRequest.at || now - outRequest.at >= 100)) {
        outRequest.packet.repeat = static_cast<uint16_t>(3 - outRequest.left);
        SendToPeers(&outRequest.packet, sizeof(outRequest.packet));
        outRequest.at = now;
        --outRequest.left;
    }
    if (!inLevel || !session) { lastSeen = global; return; }
    const bool userChange = slotsOk && localPick.Matches(global, now, kArmMs) && !(InMatch() && lockOn);
    switch (DecideClient(global, lastSeen, userChange, follower.pending, follower.value)) {
    case ClientStep::Ask:
        localPick.Clear();
        Ask(global, now);
        break;
    case ClientStep::Restore: {
        InterlockedIncrement(&restores);
        char why[128];
        std::snprintf(why, sizeof(why), "back to the host's value (generation %u): the global was %s, not set by the pause menu here", follower.generation, Name(global));
        Write(follower.value, false, why);
        break;
    }
    case ClientStep::None:
        break;
    }
    lastSeen = Current();
}
void Tick(DWORD now) {
    if (!installed) return;
    PickTick(now);
    if (globalsOk) SyncTick(now);
    if (probeOn) probe::Flush(now);
    InterlockedExchange(&mirrorGlobal, Current());
    InterlockedExchange(&mirrorLowest, ReadInt(kLowest));
    InterlockedExchange(&mirrorGeneration, static_cast<LONG>(leader.generation));
    InterlockedExchange(&mirrorHostValue, follower.value);
    InterlockedExchange(&mirrorHostGeneration, static_cast<LONG>(follower.generation));
    InterlockedExchange(&mirrorPending, follower.pending ? 1 : 0);
    InterlockedExchange(&mirrorOwned, (owned ? 1 : 0) | (committed ? 2 : 0) | (lobbyDeferred ? 4 : 0));
    InterlockedExchange(&mirrorMatch, inLevel && InMatch() ? 1 : 0);
    InterlockedExchange(&mirrorPick, Valid(pick) ? pick | (pickHeld ? 0x100 : 0) : -1);
}

// --- install -------------------------------------------------------------------------------
struct Sig { const char* name; uint32_t rva; unsigned char bytes[16]; unsigned size; };
bool Check(FILE* log, const Sig& s) {
    unsigned char actual[16]{};
    const bool ok = Read(base + s.rva, actual, s.size) && std::memcmp(actual, s.bytes, s.size) == 0;
    std::fprintf(log, "difficulty sig %-34s %08X %s\n", s.name, static_cast<unsigned>(base + s.rva), ok ? "ok" : "MISMATCH");
    return ok;
}
void InstallProbe(FILE* log) {
    // test dword [esi+0x54],0x4000000 / jne / mov ecx,[0x02087C38]; mov eax,[ebx+0x1688] / test eax,eax / je; the first instruction of ScaleAmountForDifficulty
    const Sig before{"damage before the scale", probe::kBefore, {0xF7, 0x46, 0x54, 0x00, 0x00, 0x00, 0x04, 0x75, 0x11, 0x8B, 0x0D, 0x38, 0x7C, 0x08, 0x02}, 15};
    const Sig after{"damage after the scale", probe::kAfter, {0x8B, 0x83, 0x88, 0x16, 0x00, 0x00, 0x85, 0xC0, 0x74, 0x1E}, 10};
    const Sig scale{"ScaleAmountForDifficulty", probe::kScaleAmount, {0x8B, 0x81, 0x60, 0x02, 0x00, 0x00, 0xDB, 0x81, 0x60, 0x02, 0x00, 0x00, 0x83, 0xEC, 0x08}, 15};
    const bool beforeOk = Check(log, before), afterOk = Check(log, after), scaleOk = Check(log, scale);
    const bool damageOk = beforeOk && afterOk;
    if (damageOk) {
        probe::beforeHook = safetyhook::create_mid(reinterpret_cast<void*>(base + probe::kBefore), probe::OnBefore);
        probe::afterHook = safetyhook::create_mid(reinterpret_cast<void*>(base + probe::kAfter), probe::OnAfter);
        if (!probe::beforeHook || !probe::afterHook) { probe::beforeHook.reset(); probe::afterHook.reset(); }
    }
    if (scaleOk) probe::scaleHook = safetyhook::create_inline(reinterpret_cast<void*>(base + probe::kScaleAmount), reinterpret_cast<void*>(probe::OnScaleAmount));
    std::fprintf(log, "difficulty probe: damage %s, ammo %s (first 10 hits and 20 ammo scales logged, then every 10 s)\n",
                 probe::beforeHook && probe::afterHook ? "hooked" : damageOk ? "HOOK FAILED" : "MISMATCH", probe::scaleHook ? "hooked" : scaleOk ? "HOOK FAILED" : "MISMATCH");
}
void Install(FILE* log, const std::string& ini) {
    const int wantedPick = static_cast<int>(GetPrivateProfileIntA("Difficulty", "Pick", 1, ini.c_str()));
    pickMode = wantedPick >= 0 && wantedPick <= 2 ? wantedPick : 1;
    syncOn = GetPrivateProfileIntA("Difficulty", "Sync", 1, ini.c_str()) != 0;
    lockOn = GetPrivateProfileIntA("Difficulty", "LockInMatch", 1, ini.c_str()) != 0;
    // off by default (user 02.11.): hard core is for the story; the modes route keeps it locked
    allowHardcore = GetPrivateProfileIntA("Difficulty", "AllowHardcore", 0, ini.c_str()) != 0;
    hudOn = GetPrivateProfileIntA("Difficulty", "Hud", 1, ini.c_str()) != 0;
    probeOn = GetPrivateProfileIntA("Difficulty", "Probe", 0, ini.c_str()) != 0;
    std::fprintf(log, "difficulty config: Pick=%d Sync=%d LockInMatch=%d AllowHardcore=%d Hud=%d Probe=%d\n", pickMode, syncOn ? 1 : 0, lockOn ? 1 : 0,
                 allowHardcore ? 1 : 0, hudOn ? 1 : 0, probeOn ? 1 : 0);
    // mov eax,[0x01F6031C] / ret; mov eax,[esp+4] / mov [0x01F60320],eax / ret; LaunchNewGame: mov [0x01F6031C],eax / mov [0x01F60320],eax
    const Sig get{"UIOptions::GetDifficulty", kGetDifficulty, {0xA1, 0x1C, 0x03, 0xF6, 0x01, 0xC3}, 6};
    const Sig lowest{"UIOptions::SetLowestDifficulty", kSetLowest, {0x8B, 0x44, 0x24, 0x04, 0xA3, 0x20, 0x03, 0xF6, 0x01, 0xC3}, 10};
    const Sig launch{"LaunchNewGame's two writes", kLaunchWrites, {0xA3, 0x1C, 0x03, 0xF6, 0x01, 0xA3, 0x20, 0x03, 0xF6, 0x01}, 10};
    const bool getOk = Check(log, get), lowestOk = Check(log, lowest), launchOk = Check(log, launch);
    globalsOk = getOk && lowestOk && launchOk;
    // sub esp,0x34 / push esi / push edi / mov edi,[esp+0x40] / mov esi,ecx / xor al,al / cmp [edi+esi+0x230],al
    const Sig select{"SP02 OnInput_Select", kSelect, {0x83, 0xEC, 0x34, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x40, 0x8B, 0xF1, 0x32, 0xC0, 0x38, 0x84, 0x37}, 16};
    // mov eax,[0x0201C0E4] / mov ecx,[eax+0x38] / shr ecx,2 / test cl,1 / je
    const Sig cancel{"SP02 OnInput_Cancel", kCancel, {0xA1, 0xE4, 0xC0, 0x01, 0x02, 0x8B, 0x48, 0x38, 0xC1, 0xE9, 0x02, 0xF6, 0xC1, 0x01, 0x74, 0x1C}, 16};
    const Sig show{"SP02 Show", kShow, {0xA1, 0xE4, 0xC0, 0x01, 0x02, 0x56, 0x8B, 0xF1, 0x8B, 0x48, 0x38, 0xC1, 0xE9, 0x02, 0xF6, 0xC1}, 16};
    const Sig init{"SP02 InitMenuParams", kInit, {0x56, 0x8B, 0xF1, 0xE8, 0x68, 0x7B, 0xFE, 0xFF, 0xA1, 0xE4, 0xC0, 0x01, 0x02, 0x8B, 0x48, 0x38}, 16};
    const Sig pop{"Apt3dDirector::PopScreen", kPopScreen, {0x56, 0x8B, 0xF1, 0x8B, 0x86, 0xF8, 0x03, 0x00, 0x00, 0x83, 0xF8, 0x01, 0x7C, 0x3C, 0x8B, 0x8C}, 16};
    const Sig top{"Apt3dDirector current screen", kCurrentScreen, {0x8B, 0x91, 0xF8, 0x03, 0x00, 0x00, 0x33, 0xC0, 0x85, 0xD2, 0x7C, 0x09}, 12};
    const Sig update{"UIExternScrollMenuIterator::UpdateItem", kUpdateItem, {0x51, 0x8B, 0x44, 0x24, 0x08, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x5E, 0x40, 0x83, 0xFB, 0xFF}, 15};
    const bool selectOk = Check(log, select), cancelOk = Check(log, cancel), showOk = Check(log, show), initOk = Check(log, init);
    const bool screenOk = selectOk && cancelOk && showOk;
    const bool popSigOk = Check(log, pop), topOk = Check(log, top);
    popOk = popSigOk && topOk;
    refreshOk = Check(log, update);
    std::fprintf(log, "difficulty now %s (global %d, lowest %d, times played %d)%s\n", Name(Current()), Current(), ReadInt(kLowest), ReadInt(kTimesPlayed),
                 globalsOk ? "" : "; the globals did not verify: nothing is written, no sync, no HUD");
    const bool wantSlots = (pickMode && menu::installed) || lockOn || syncOn;
    if (wantSlots && screenOk) {
        const bool a = menu::SwapSlot(log, "SP02 select (difficulty)", kSelectSlot, kSelect, reinterpret_cast<void*>(OnSelect));
        const bool b = a && menu::SwapSlot(log, "SP02 cancel (difficulty)", kCancelSlot, kCancel, reinterpret_cast<void*>(OnCancel));
        const bool c = b && menu::SwapSlot(log, "SP02 show (difficulty)", kShowSlot, kShow, reinterpret_cast<void*>(OnShow));
        slotsOk = a && b && c;
        if (!slotsOk) {
            // all three or none: an owned pick needs the cancel and the show
            DWORD protect = 0;
            if (a) menu::WriteSlot(base + kSelectSlot, static_cast<uint32_t>(base + kSelect), protect);
            if (b) menu::WriteSlot(base + kCancelSlot, static_cast<uint32_t>(base + kCancel), protect);
            std::fprintf(log, "difficulty slots NOT installed (select %d cancel %d show %d); the swapped ones are back to native\n", a ? 1 : 0, b ? 1 : 0, c ? 1 : 0);
        } else if (initOk && pickMode) {
            menu::SwapSlot(log, "SP02 init (difficulty)", kInitSlot, kInit, reinterpret_cast<void*>(OnInit));
        }
    }
    pickReady = pickMode && slotsOk && menu::installed && globalsOk && popOk;
    installed = true;
    if (probeOn) InstallProbe(log);
    std::fprintf(log, "difficulty %s: picker %s; sync %s; lock in matches %s; hard core row %s; HUD %s\n", slotsOk || globalsOk ? "installed" : "NOT installed",
                 pickReady ? (pickMode == 2 ? "Pick=2, the difficulty row in the map picker (SP02 asks while no row holds a pick)" : "on (map picker -> SP02 -> launch)")
                 : !pickMode ? "off ([Difficulty] Pick=0)" : !menu::installed ? "off (the menu module is not installed)" : "off (a site or slot did not verify)",
                 syncOn && globalsOk ? "on (survnet kinds 13/14 every 2 s, lobby launch, campaign packet)" : "off", lockOn && slotsOk ? "on" : "off",
                 allowHardcore ? "opened in the picker" : "as the unlock says", hudOn && globalsOk ? "on" : "off");
}
bool HudWanted() { return installed && hudOn && globalsOk; }

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    char state[512];
    const LONG flags = InterlockedCompareExchange(&mirrorOwned, 0, 0), picked = InterlockedCompareExchange(&mirrorPick, 0, 0);
    std::snprintf(state, sizeof(state),
                  "difficulty global=%s(%ld) lowest=%ld role=%d match=%ld | host gen=%ld changes=%ld sends=%ld requests in=%ld applied=%ld same=%ld refused=%ld | "
                  "client host=%s(%ld) gen=%ld pending=%ld applied=%ld held=%ld asked=%ld timeouts=%ld restores=%ld | pick=%s(%s) mode=%d | picker owned=%ld "
                  "committed=%ld lobby_wait=%ld",
                  Name(static_cast<int>(InterlockedCompareExchange(&mirrorGlobal, 0, 0))), InterlockedCompareExchange(&mirrorGlobal, 0, 0),
                  InterlockedCompareExchange(&mirrorLowest, 0, 0), NetRole(), InterlockedCompareExchange(&mirrorMatch, 0, 0), InterlockedCompareExchange(&mirrorGeneration, 0, 0),
                  InterlockedCompareExchange(&hostChanges, 0, 0), InterlockedCompareExchange(&hostSends, 0, 0), InterlockedCompareExchange(&requestsIn, 0, 0),
                  InterlockedCompareExchange(&requestsApplied, 0, 0), InterlockedCompareExchange(&requestsSame, 0, 0), InterlockedCompareExchange(&requestsRefused, 0, 0),
                  Name(static_cast<int>(InterlockedCompareExchange(&mirrorHostValue, 0, 0))), InterlockedCompareExchange(&mirrorHostValue, 0, 0),
                  InterlockedCompareExchange(&mirrorHostGeneration, 0, 0), InterlockedCompareExchange(&mirrorPending, 0, 0), InterlockedCompareExchange(&applied, 0, 0),
                  InterlockedCompareExchange(&held, 0, 0), InterlockedCompareExchange(&requestsSent, 0, 0), InterlockedCompareExchange(&requestTimeouts, 0, 0),
                  InterlockedCompareExchange(&restores, 0, 0), picked < 0 ? "none" : Name(static_cast<int>(picked & 0xFF)),
                  picked < 0 ? "-" : picked & 0x100 ? "held" : "once", pickMode, flags & 1, flags >> 1 & 1, flags >> 2 & 1);
    static char lastState[512] = "";
    if (std::strcmp(state, lastState) != 0) {
        std::snprintf(lastState, sizeof(lastState), "%s", state);
        std::fprintf(log, "%s\n", state);
        std::fprintf(log, "difficulty screen pushes=%ld failed=%ld shows=%ld inits=%ld picks=%ld refused=%ld repeats=%ld cancels=%ld forwarded=%ld lost=%ld row_redraws=%ld "
                          "lobby opens=%ld fallbacks=%ld | in game refused=%ld same_row=%ld native=%ld | writes=%ld failed=%ld launch_writes=%ld origins=%ld restored=%ld "
                          "faults=%ld lines=%ld dropped=%ld\n",
                     InterlockedCompareExchange(&pushes, 0, 0), InterlockedCompareExchange(&pushFailures, 0, 0), InterlockedCompareExchange(&shows, 0, 0),
                     InterlockedCompareExchange(&inits, 0, 0), InterlockedCompareExchange(&picks, 0, 0), InterlockedCompareExchange(&pickRefusals, 0, 0),
                     InterlockedCompareExchange(&pickRepeats, 0, 0), InterlockedCompareExchange(&cancels, 0, 0), InterlockedCompareExchange(&forwards, 0, 0),
                     InterlockedCompareExchange(&ownLost, 0, 0), InterlockedCompareExchange(&rowRefreshes, 0, 0), InterlockedCompareExchange(&lobbyOpens, 0, 0),
                     InterlockedCompareExchange(&lobbyFallbacks, 0, 0), InterlockedCompareExchange(&inGameRefusals, 0, 0), InterlockedCompareExchange(&inGameSame, 0, 0),
                     InterlockedCompareExchange(&inGameForwards, 0, 0), InterlockedCompareExchange(&writes, 0, 0), InterlockedCompareExchange(&writeFailures, 0, 0),
                     InterlockedCompareExchange(&launchWrites, 0, 0), InterlockedCompareExchange(&origins, 0, 0), InterlockedCompareExchange(&originRestores, 0, 0),
                     InterlockedCompareExchange(&faults, 0, 0), InterlockedCompareExchange(&lines, 0, 0), InterlockedCompareExchange(&linesDropped, 0, 0));
    }
    // the refreshes, once a minute when they moved
    static DWORD trafficAt = 0;
    static LONG lastTraffic = -1;
    const DWORD now = GetTickCount();
    const LONG traffic = InterlockedCompareExchange(&hostRefreshes, 0, 0) + InterlockedCompareExchange(&received, 0, 0);
    if (traffic != lastTraffic && now - trafficAt >= 60000) {
        trafficAt = now;
        lastTraffic = traffic;
        std::fprintf(log, "difficulty survnet refreshes sent=%ld received=%ld (same=%ld stale=%ld held=%ld ignored=%ld)\n", InterlockedCompareExchange(&hostRefreshes, 0, 0),
                     InterlockedCompareExchange(&received, 0, 0), InterlockedCompareExchange(&same, 0, 0), InterlockedCompareExchange(&stale, 0, 0),
                     InterlockedCompareExchange(&held, 0, 0), InterlockedCompareExchange(&ignored, 0, 0));
    }
}
}  // namespace difficulty
