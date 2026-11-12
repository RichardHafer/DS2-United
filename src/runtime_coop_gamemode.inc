// Dead Space 2 United by Richard Hafer.
// Game modes, picked in the native map picker (MP20, [Menu] Modes): up/down
// cycles the mode and the map row reads "titan mines [mercenaries]". The
// survival director asks DS2Coop_GameMode when a run arms; a campaign arena plays
// the modes it has points for ([Arena] Modes), the others as survival.
//
// MP20 has one row, so its up/down codes do nothing natively and are free:
//   HandleInput 0x00A7E7D0 (vtable slot 0x01B3A308, thiscall(code), ret 4):
//     6 left, 7 right, 13 select, everything else to the base 0x00A6AEF0.
//   SetInfo 0x00A4BA90 (thiscall(index), ret 4): copies the map's label to +0x234
//     (64 bytes), points row 0 at it and, while the screen is active (+0x4C),
//     refreshes the movie (0x015E9800 on +0x5C, thiscall(0, 0)). Show 0x00A97B50
//     calls it when the picker opens, so the label is there from the start.
//   Left/right play the navigation sound [0x02089FC0] through the dispatch.
namespace mappictures { void BeforeMp20(uint8_t* screen, uint32_t index); }   // runtime_coop_mappictures.inc
namespace difficulty { extern bool allowHardcore; bool SetPick(int row); int PickValue(); int Current(); }   // runtime_coop_difficulty.inc
namespace gamemode {
enum Mode : int { Survival = 0, Mercenaries = 1, Objective = 2, Zone = 3, Versus = 4, kCount };
const char* const kNames[kCount] = {"survival", "mercenaries", "objective", "zone", "versus"};
bool enabled = false;
volatile LONG mode = Survival;
safetyhook::InlineHook setInfoHook;
uint32_t inputOriginal = 0;
volatile LONG cycles = 0, labels = 0, refreshes = 0, faults = 0, arenaRows = 0, lockSkips = 0;
constexpr uint32_t kInputSlot = 0x173A308, kInputRva = 0x67E7D0, kSetInfoRva = 0x64BA90, kRefreshRva = 0x11E9800, kNavSoundRva = 0x1C89FC0;
constexpr uint32_t kRows = 0x214, kActive = 0x4C, kMovie = 0x5C, kName = 0x234, kNameSize = 64;

const char* Name(LONG m) { return m >= 0 && m < kCount ? kNames[m] : "?"; }
// Objective needs the MP map's own objective chain, which no campaign arena has. Any thread.
bool PlaysObjective() {
    return InterlockedCompareExchange(&mode, 0, 0) == Objective && InterlockedCompareExchange(&arena::stateMirror, 0, 0) == 0;
}

// A campaign arena row; its map name ("arena_...") into `name` when asked.
bool ArenaRow(const uint8_t* screen, char* name = nullptr, size_t size = 0) {
    uint32_t index = 0, dgm = 0, maps = 0, count = 0;
    unsigned char any = 0;
    if (!Read(reinterpret_cast<uintptr_t>(screen) + menu::kMp20Index, &index, 4) || !Read(reinterpret_cast<uintptr_t>(screen) + menu::kMp20Any, &any, 1)) return false;
    if (any || !Read(base + menu::kGameManager, &dgm, 4) || !dgm || !Read(dgm + menu::kDgmMaps, &maps, 4) || !Read(dgm + menu::kDgmMapCount, &count, 4) ||
        index >= count || !maps)
        return false;
    char map[64]{};
    if (!menu::EntryMap(maps + index * menu::kMapEntrySize, map, sizeof(map)) || !coop_arena::IsArenaName(map)) return false;
    if (name && size) std::snprintf(name, size, "%s", map);
    return true;
}
// The next mode from `before` on this row: every mode on an MP map, on an arena row only
// those it can play (survival when [Arena] Modes is off).
LONG NextMode(const uint8_t* screen, LONG before, bool up) {
    char name[64]{};
    if (!ArenaRow(screen, name, sizeof(name))) return (before + (up ? 1 : kCount - 1)) % kCount;
    if (!arena::modesOn) return before;
    return coop_arena::StepMode(coop_arena::PointsFor(name), before, up ? 1 : -1, kCount);
}
// What the row plays with the picked mode.
int RowMode(const uint8_t* screen, LONG picked) {
    char name[64]{};
    return ArenaRow(screen, name, sizeof(name)) ? arena::ModeForName(name, picked) : static_cast<int>(picked);
}
// Can entry `index` of the map list play `picked`? Every MP map plays every mode, an arena
// only the modes it has points for; "any map" (index == count) picks a fitting map itself.
bool IndexPlays(uint32_t index, LONG picked) {
    uint32_t dgm = 0, maps = 0, count = 0;
    if (!Read(base + menu::kGameManager, &dgm, 4) || !dgm || !Read(dgm + menu::kDgmMaps, &maps, 4) || !Read(dgm + menu::kDgmMapCount, &count, 4) || !maps ||
        index >= count)
        return true;
    char map[64]{};
    if (!menu::EntryMap(maps + index * menu::kMapEntrySize, map, sizeof(map)) || !coop_arena::IsArenaName(map)) return true;
    return arena::ModeForName(map, static_cast<int>(picked)) == picked;
}
// The map row's step keeps the picked mode (16.10.: Versus picked on an MP map, then the
// map row stepped onto arenas that cannot play it): maps that cannot play it are skipped.
// GetNextIndexLooping 0x00A4BA40 / GetPrevIndexLooping 0x00A4B9F0, thiscall(index) ret 4.
constexpr uint32_t kNextIndexRva = 0x64BA40, kPrevIndexRva = 0x64B9F0;
uint32_t StepMap(uint8_t* screen, uint32_t index, bool right, LONG picked) {
    using StepFn = uint32_t(__thiscall*)(void*, uint32_t);
    const StepFn step = reinterpret_cast<StepFn>(base + (right ? kNextIndexRva : kPrevIndexRva));
    uint32_t next = step(screen, index);
    for (unsigned i = 0; i < 64 && next != index && !IndexPlays(next, picked); ++i) {
        InterlockedIncrement(&lockSkips);
        next = step(screen, next);
    }
    return IndexPlays(next, picked) ? next : index;
}

// The native mode row, as in Mark's DS2 multiplayer ([Menu] NativeModeRow): a
// second menu item above the map, left/right on it steps the mode. It needs
// Mark's MP20 movie (clips m0 and m1), so frontend\frontend.str is read from
// randomizer\ds2coop_frontend.str (tools/build_frontend_override.py), the stock
// file with only that movie swapped. The archives are never touched.
//   File open 0x0056F3C0 (thiscall(path, a2, a3, a4, a5), ret 0x14) copies the
//     path, so the name can be swapped. It searches the archives first, then the
//     game folder, where the randomizer's loose streams already load from.
//   The frontend is opened ~3 s after start and on every return to the main
//     menu, so everything goes in right after unpack, before InstallDelayMs.
//   MP20 ctor 0x00AAB7B0: item count, array size and item count 1 -> 2
//     (0x00AAB7B5, 0x00AAB830, 0x00AAB836, 0x00AAB84A).
//   SetInfo tail 0x00A4BB50: entry 0 = {mode text, mode}, entry 1 = {map label,
//     map}, refresh on the current row (cursor 0x015E8C50, -1 for none).
//   HandleInput 0x00A7E83D (right) / 0x00A7E8B6 (left), after the arrow sound:
//     row 0 steps the mode, row 1 the map in entry 1; both resume at the native
//     arrow animation 0x00A7E865 / 0x00A7E8DE once the two pushed args are gone.
// Each SetInfo checks the Apt registry ([0x0204199C], 192 AptAnimationInfo slots
// of 0xE0: aptData +0x08, aptConst +0x0C, mIsUsed +0x30, mAptFileName +0x34, per
// the PDB). The slot holds no size, so the movie's data is searched for the row
// label only Mark's movie has. Without it the screen goes back to one item and
// the stock code plus the label run as before.
// The difficulty row ([Difficulty] Pick=2, analysis/MP20_THIRD_ROW_2026-10-20.md):
//   the ctor sites get 3, entry 2 = {difficulty name, difficulty}, left/right on
//   row 2 steps difficulty::SetPick. It needs the movie's m2 (tools/mp20_third_row.py,
//   marker kThirdMarker); without it the screen keeps two items and SP02 asks.
//   Up/down need nothing: the shared list input wraps over [+0x220] (0x00A50BB0 /
//   0x00A50D10), which the menu build 0x00A55960 hands to the scroll iterator.
namespace native {
enum Verdict { Unknown, Stock, Mark };
const char* const kVerdicts[] = {"unknown", "stock", "mark"};
const char* const kRowText[kCount] = {"survival", "mercenaries", "objective", "hold the zone", "versus"};   // lower case like the map names
// [Menu] FrontendColours=stock picks the copy built without the recolour steps, when it is there.
char overridePath[64] = "randomizer\\ds2coop_frontend.str";
const char kStockOverridePath[] = "randomizer\\ds2coop_frontend_stock.str";
// [Loadout]: the suit list with the campaign suits (tools/build_loadout_frontend.py).
// PlayerSkinDB reads mpskins.lua once at boot, through the same file open.
char skinsPath[] = "randomizer\\ds2coop_mpskins.lua";
// The in-game menus (pause menu, settings, popups and their button libraries) live in
// global_assets\global_assets.str, opened through the same file open. The red copy from
// tools/recolor_global_assets.py differs in colour values only, so global_assets.toc (names,
// dependencies, payload sizes) stays valid. [Menu] RedInGameMenus, and the file must be there.
char globalPath[] = "randomizer\\ds2coop_global_assets.str";
volatile LONG globalRedirecting = 0, globalOpens = 0, globalRedirects = 0;
// The loading screens (loading_screen\loading_screen_global.str, DS2DAT6/7/8), the same way.
char loadingPath[] = "randomizer\\ds2coop_loading_screen.str";
volatile LONG loadingRedirecting = 0, loadingOpens = 0, loadingRedirects = 0;
// The MP screens (global_mp_assets\global_mp_assets.str, DS2DAT9): the online pause menu with
// its timer and objective list, scoring, spectator, vent pick (user, 16.10.: it stayed blue).
// The archive key is the sdbm of exactly this path; other opens naming global_mp_assets are
// logged (the first 8), so one run shows whether the game asks for it this way.
char mpAssetsPath[] = "randomizer\\ds2coop_global_mp_assets.str";
volatile LONG mpRedirecting = 0, mpOpens = 0, mpRedirects = 0, mpNearMisses = 0;
char mpMissPaths[8][80]{};
volatile LONG mpMissReady[8]{};
LONG mpMissesPrinted = 0;
volatile LONG skinsRedirecting = 0, skinsOpens = 0, skinsRedirects = 0;
// push ebx / push esi / push edi / mov esi,ecx / xor ebx,ebx / mov [esi+4],ebx / mov eax,[esp+0x20] / mov ecx,[esp+0x1C]
const unsigned char kOpen[] = {0x53, 0x56, 0x57, 0x8B, 0xF1, 0x33, 0xDB, 0x89, 0x5E, 0x04, 0x8B, 0x44, 0x24, 0x20, 0x8B, 0x4C};
constexpr uint32_t kMovieSize = 686320;
constexpr char kRowLabel[] = "$ui_gamemode_mc";
constexpr char kThirdMarker[] = "DS2Coop:MP20:m2";
constexpr uint32_t kScanBytes = 0x200000;   // the movie grew past 1 MB with the arena pictures (16.10.)
constexpr uint32_t kOpenRva = 0x16F3C0, kTailRva = 0x64BB50, kTailResumeRva = 0x64BB79, kRightRva = 0x67E83D, kLeftRva = 0x67E8B6;
constexpr uint32_t kRightResumeRva = 0x67E865, kLeftResumeRva = 0x67E8DE, kCursorRva = 0x11E8C50, kNextRva = 0x64BA40, kPrevRva = 0x64B9F0;
constexpr uint32_t kRegistry = 0x1C4199C, kSlots = 192, kSlotSize = 0xE0;
constexpr uint32_t kCtorSites[4] = {0x6AB7B5, 0x6AB830, 0x6AB836, 0x6AB84A};
constexpr uint32_t kItems = 0x1A8, kArray = 0x214, kArraySize = 0x21C, kItemCount = 0x220, kCursorObject = 0x5C;
bool wanted = false, active = false, ctorPatched = false, threeWanted = false;
unsigned char ctorRows = 2;
volatile LONG redirecting = 0, screen = 0, lastRows = -1, difficultySteps = 0, rowChanges = 0;
safetyhook::InlineHook openHook;
safetyhook::MidHook tailHook, rightHook, leftHook;
uintptr_t rightResume = 0, leftResume = 0;
DWORD startTick = 0;
volatile LONG opens = 0, frontendOpens = 0, redirects = 0, lastFrontendRedirected = -1, firstFrontendTick = 0;
volatile LONG decisions = 0, nativeFills = 0, fallbacks = 0, modeSteps = 0, mapSteps = 0, arenaSteps = 0, faults = 0, lastVerdict = -1, lastNative = -1;

struct OpenNote { DWORD at, thread; uint32_t caller; bool frontend, redirected; char path[80]; };
constexpr LONG kNotes = 32;
OpenNote notes[kNotes];
volatile LONG noteCount = 0, noteReady[kNotes]{};
LONG notesPrinted = 0;

bool IsFrontendPath(const char* path) {
    static const char kWant[] = "frontend\\frontend.str";
    size_t i = 0;
    for (; kWant[i]; ++i) {
        char c = path[i];
        if (c == '/') c = '\\';
        if (static_cast<char>(std::tolower(static_cast<unsigned char>(c))) != kWant[i]) return false;
    }
    return path[i] == 0;
}
bool PathIs(const char* path, const char* want) {
    size_t i = 0;
    for (; want[i]; ++i) {
        char c = path[i];
        if (c == '/') c = '\\';
        if (static_cast<char>(std::tolower(static_cast<unsigned char>(c))) != want[i]) return false;
    }
    return path[i] == 0;
}
bool IsGlobalAssetsPath(const char* path) {
    static const char kWant[] = "global_assets\\global_assets.str";
    size_t i = 0;
    for (; kWant[i]; ++i) {
        char c = path[i];
        if (c == '/') c = '\\';
        if (static_cast<char>(std::tolower(static_cast<unsigned char>(c))) != kWant[i]) return false;
    }
    return path[i] == 0;
}
bool ContainsNoCase(const char* path, const char* want) {
    for (const char* p = path; *p; ++p) {
        size_t i = 0;
        for (; want[i] && p[i] && std::tolower(static_cast<unsigned char>(p[i])) == want[i]; ++i) {}
        if (!want[i]) return true;
    }
    return false;
}
// "mpskins.lua" at the end of the path, after a separator or alone.
bool IsSkinsPath(const char* path) {
    static const char kWant[] = "mpskins.lua";
    const size_t n = std::strlen(path), w = sizeof(kWant) - 1;
    if (n < w) return false;
    for (size_t i = 0; i < w; ++i)
        if (static_cast<char>(std::tolower(static_cast<unsigned char>(path[n - w + i]))) != kWant[i]) return false;
    return n == w || path[n - w - 1] == '\\' || path[n - w - 1] == '/';
}
void Note(const char* path, bool frontend, bool redirected, uint32_t caller) {
    // the first 24 opens, then frontend and skin list opens only
    if (!frontend && InterlockedCompareExchange(&noteCount, 0, 0) >= 24) return;
    const LONG i = InterlockedIncrement(&noteCount) - 1;
    if (i >= kNotes) return;
    OpenNote& n = notes[i];
    n.at = GetTickCount() - startTick;
    n.thread = GetCurrentThreadId();
    n.caller = caller;
    n.frontend = frontend;
    n.redirected = redirected;
    __try {
        std::snprintf(n.path, sizeof(n.path), "%s", path ? path : "(null)");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(n.path, sizeof(n.path), "(unreadable)");
    }
    InterlockedExchange(&noteReady[i], 1);
}
uint32_t __fastcall OnOpen(void* op, void*, const char* path, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
    InterlockedIncrement(&opens);
    bool frontend = false, skins = false, global = false, loading = false, mp = false;
    __try {
        frontend = path && IsFrontendPath(path);
        skins = path && !frontend && IsSkinsPath(path);
        global = path && !frontend && !skins && IsGlobalAssetsPath(path);
        loading = path && !frontend && !skins && !global && PathIs(path, "loading_screen\\loading_screen_global.str");
        mp = path && !frontend && !skins && !global && !loading && PathIs(path, "global_mp_assets\\global_mp_assets.str");
        if (path && !mp && ContainsNoCase(path, "global_mp_assets")) {
            const LONG i = InterlockedIncrement(&mpNearMisses) - 1;
            if (i < 8) {
                std::snprintf(mpMissPaths[i], sizeof(mpMissPaths[i]), "%s", path);
                InterlockedExchange(&mpMissReady[i], 1);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        frontend = skins = global = loading = mp = false;
    }
    if (mp) {
        const bool mpRedirect = InterlockedCompareExchange(&mpRedirecting, 0, 0) != 0;
        InterlockedIncrement(&mpOpens);
        if (mpRedirect) InterlockedIncrement(&mpRedirects);
        Note(path, true, mpRedirect, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
        return openHook.thiscall<uint32_t>(op, mpRedirect ? mpAssetsPath : path, a2, a3, a4, a5);
    }
    if (loading) {
        const bool loadingRedirect = InterlockedCompareExchange(&loadingRedirecting, 0, 0) != 0;
        InterlockedIncrement(&loadingOpens);
        if (loadingRedirect) InterlockedIncrement(&loadingRedirects);
        Note(path, true, loadingRedirect, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
        return openHook.thiscall<uint32_t>(op, loadingRedirect ? loadingPath : path, a2, a3, a4, a5);
    }
    if (global) {
        const bool globalRedirect = InterlockedCompareExchange(&globalRedirecting, 0, 0) != 0;
        InterlockedIncrement(&globalOpens);
        if (globalRedirect) InterlockedIncrement(&globalRedirects);
        Note(path, true, globalRedirect, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
        return openHook.thiscall<uint32_t>(op, globalRedirect ? globalPath : path, a2, a3, a4, a5);
    }
    const bool redirect = frontend && InterlockedCompareExchange(&redirecting, 0, 0);
    if (skins) {
        const bool skinsRedirect = InterlockedCompareExchange(&skinsRedirecting, 0, 0) != 0;
        InterlockedIncrement(&skinsOpens);
        if (skinsRedirect) InterlockedIncrement(&skinsRedirects);
        Note(path, true, skinsRedirect, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
        return openHook.thiscall<uint32_t>(op, skinsRedirect ? skinsPath : path, a2, a3, a4, a5);
    }
    if (frontend) {
        InterlockedIncrement(&frontendOpens);
        InterlockedCompareExchange(&firstFrontendTick, static_cast<LONG>(GetTickCount() - startTick) | 1, 0);
        InterlockedExchange(&lastFrontendRedirected, redirect ? 1 : 0);
    }
    if (redirect) InterlockedIncrement(&redirects);
    Note(path, frontend, redirect, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
    return openHook.thiscall<uint32_t>(op, redirect ? overridePath : path, a2, a3, a4, a5);
}

// The lowest usable registry slot named mp20gamesettings is the movie Apt uses.
struct Slot { int index = -1; uint32_t state = 0, aptData = 0, aptConst = 0; };
Slot FindSlot(const char* name) {
    Slot found;
    uint32_t registry = 0;
    if (!Read(base + kRegistry, &registry, 4) || !registry) return found;
    for (uint32_t i = 0; i < kSlots; ++i) {
        const uint32_t slot = registry + 4 + i * kSlotSize;
        uint32_t state = 0;
        char text[48]{};
        if (!Read(slot + 0x30, &state, 4) || (state != 3 && state != 4) || !Read(slot + 0x34, text, sizeof(text) - 1) || _stricmp(text, name) != 0) continue;
        found.index = static_cast<int>(i);
        found.state = state;
        Read(slot + 0x08, &found.aptData, 4);
        Read(slot + 0x0C, &found.aptConst, 4);
        return found;
    }
    return found;
}
// Bit 0: Mark's row label, bit 1: "mMenuItemText", which both MP20 movies carry
// (so the window really is the movie), bit 2: the third row's marker, which sits
// behind the label. -1 when the start is unreadable.
int ScanMovie(uint32_t start) {
    static const char kItemText[] = "mMenuItemText";
    constexpr size_t kPage = 0x1000, kKeep = 16;
    static_assert(sizeof(kRowLabel) - 1 <= kKeep && sizeof(kThirdMarker) - 1 <= kKeep, "a needle across two pages must fit the carry");
    if (!start) return -1;
    char buf[kPage + kKeep];
    size_t carry = 0;
    int found = 0;
    const auto at = [&](size_t i, size_t have, const char* needle, size_t n) { return i + n <= have && std::memcmp(buf + i, needle, n) == 0; };
    for (uint32_t done = 0; done < kScanBytes; done += kPage) {
        if (!Read(start + done, buf + carry, kPage)) return done ? found : -1;
        const size_t have = carry + kPage;
        for (size_t i = 0; i < have; ++i) {
            if (buf[i] == '$' && at(i, have, kRowLabel, sizeof(kRowLabel) - 1)) found |= 1;
            else if (buf[i] == 'm' && at(i, have, kItemText, sizeof(kItemText) - 1)) found |= 2;
            else if (buf[i] == 'D' && at(i, have, kThirdMarker, sizeof(kThirdMarker) - 1)) found |= 4;
        }
        if ((found & 5) == 5) return found;
        carry = kKeep;
        std::memmove(buf, buf + have - carry, carry);
    }
    return found;
}
Verdict MovieVerdict(Slot& slot, bool& third) {
    static uint32_t cachedData = 0, cachedConst = 0;
    static Verdict cached = Unknown;
    static bool cachedThird = false;
    third = false;
    slot = FindSlot("mp20gamesettings");
    if (slot.index < 0) return Unknown;
    if (slot.aptData == cachedData && slot.aptConst == cachedConst && cachedData) {
        third = cachedThird;
        return cached;
    }
    const int a = ScanMovie(slot.aptData), c = ScanMovie(slot.aptConst);
    const int seen = (a < 0 ? 0 : a) | (c < 0 ? 0 : c);
    cached = (seen & 1) ? Mark : (seen & 2) ? Stock : Unknown;
    cachedThird = cached == Mark && (seen & 4) != 0;
    cachedData = slot.aptData;
    cachedConst = slot.aptConst;
    mpmap::Event("native row: movie scan aptData %08X -> %d, aptConst %08X -> %d: %s%s", slot.aptData, a, slot.aptConst, c, kVerdicts[cached],
                 cachedThird ? " with the third row m2" : cached == Mark ? " (two rows, no m2)" : "");
    third = cachedThird;
    return cached;
}
void LogSlots() {
    // the stock libraries, then Mark's recoloured copies the MP20 movie imports (two lines: an event line holds 255 characters)
    static const char* const kMovies[2][4] = {{"mp20gamesettings", "libfrontend", "libshared", "sharedlibpc"},
                                              {"libfrontemp", "libsharmp", "sharedlibmp", "basescreen"}};
    for (const auto& group : kMovies) {
        char line[256];
        int n = std::snprintf(line, sizeof(line), "native row: registry");
        for (const char* name : group) {
            const Slot s = FindSlot(name);
            if (n > 0 && n < static_cast<int>(sizeof(line)))
                n += std::snprintf(line + n, sizeof(line) - n, " %s=%d/%u/%08X/%08X", name, s.index, s.state, s.aptData, s.aptConst);
        }
        mpmap::Event("%s (index/state/aptData/aptConst)", line);
    }
}

int Cursor(uint8_t* s) {
    return reinterpret_cast<int(__thiscall*)(void*)>(base + kCursorRva)(s + kCursorObject);
}
void SetInfo(uint8_t* s, uint32_t index) {
    reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + 0x64BA90)(s, index);
}
// "<count> {text, index} ..." for the log, one entry per allocated slot (up to three).
void DescribeEntries(const uint8_t* s, char* out, size_t size) {
    uint32_t array = 0, count = 0, allocated = 0, items = 0, e[6]{};
    Read(reinterpret_cast<uintptr_t>(s) + kArray, &array, 4);
    Read(reinterpret_cast<uintptr_t>(s) + kItemCount, &count, 4);
    Read(reinterpret_cast<uintptr_t>(s) + kArraySize, &allocated, 4);
    Read(reinterpret_cast<uintptr_t>(s) + kItems, &items, 4);
    const uint32_t shown = allocated >= 3 ? 3 : allocated >= 2 ? 2 : 1;
    char t[3][24]{};
    if (array && Read(array, e, shown * 8))
        for (uint32_t i = 0; i < shown; ++i)
            if (e[2 * i]) Read(e[2 * i], t[i], sizeof(t[i]) - 1);
    int n = std::snprintf(out, size, "items=%u count=%u array=%08X size=%u", items, count, array, allocated);
    for (uint32_t i = 0; i < shown && n > 0 && static_cast<size_t>(n) < size; ++i) n += std::snprintf(out + n, size - n, " e%u={%s,%u}", i, t[i], e[2 * i + 1]);
}

// The difficulty row holds the pick it shows, so the map picker's select launches
// with it and skips SP02 (difficulty::OpenPicker). Hard core only with AllowHardcore.
// SetPick every time: a pick SP02 left behind is not held.
int RowDifficulty() {
    int d = difficulty::PickValue();
    if (!coop_difficulty::Valid(d)) d = difficulty::Current();
    if (!coop_difficulty::Valid(d) || (d == coop_difficulty::kHardcore && !difficulty::allowHardcore)) d = coop_difficulty::kNormal;
    difficulty::SetPick(d);
    return d;
}
int StepDifficulty(int d, bool right) {
    for (int i = 0; i < coop_difficulty::kCount; ++i) {
        d = (d + (right ? 1 : coop_difficulty::kCount - 1)) % coop_difficulty::kCount;
        if (d != coop_difficulty::kHardcore || difficulty::allowHardcore) break;
    }
    return d;
}

// The map description under the rows follows the mode (user 20.10.): Objective keeps the
// game's own versus texts, which describe exactly that mode's objectives; the other modes
// get a line about the place and one about the mode. The five shipped maps and "any map"
// show a text id (+0x278 holds "$ui_versusmap_description_NN_mc"), so their table entries
// point at our buffers, or back at the game's text for Objective. Arenas and the Outbreak
// maps show their entry's literal, so +0x278 gets ours (64 bytes, as SetInfo copies).
namespace desc {
constexpr uint32_t kHashes[6] = {0xCAE7F776, 0xF96EC835, 0x27F598F4, 0x567C69B3, 0x85033A72, 0x4B1FE288};   // 00..04, any
const char* const kPlaces[5] = {
    "The Titan Mines, deep beneath the Sprawl.", "The Ishimura's hangar, docked beside the Sprawl.", "The solar array out on the Sprawl's hull.",
    "The fuel core at the heart of the Sprawl.", "The Government Sector lab where the Marker was built."};
const char* const kModeLines[kCount] = {"Survive the waves together; every tenth one brings the Ubermorph.",
                                        "Mercenaries: the clock runs, and every kill buys more time.", "",
                                        "Hold the zone: stay inside the marked area while the waves come.",
                                        "Versus: the humans against necromorph players."};
const char* const kModeShort[kCount] = {"survive the waves", "race the clock", "play its objectives", "hold the zone", "humans vs necromorphs"};
char texts[6][224]{};
uint32_t originals[6]{};
volatile LONG applied = 0, restored = 0, literals = 0, missing = 0, faults = 0;
uint32_t TextSlot(uint32_t t, uint32_t hash) {
    uint32_t count = 0, hashes = 0, strings = 0;
    if (!Read(t + 0x10, &count, 4) || !Read(t + 0x18, &hashes, 4) || !Read(t + 0x1C, &strings, 4) || !count || !hashes || !strings) return 0;
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        uint32_t h = 0;
        Read(hashes + mid * 4, &h, 4);
        if (h < hash) lo = mid + 1; else hi = mid;
    }
    uint32_t h = 0;
    return lo < count && Read(hashes + lo * 4, &h, 4) && h == hash ? strings + lo * 4 : 0;
}
// The table entries of the shipped maps and "any map", for this mode.
void PointTexts(int m) {
    const uint32_t t = notice::FindTable();
    if (!t) { InterlockedIncrement(&missing); return; }
    for (unsigned i = 0; i < 6; ++i) {
        const uint32_t slot = TextSlot(t, kHashes[i]);
        uint32_t current = 0;
        if (!slot || !Read(slot, &current, 4)) { InterlockedIncrement(&missing); continue; }
        const uint32_t ours = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(texts[i]));
        if (current != ours) originals[i] = current;   // a rebuilt table brings new originals
        if (i < 5) std::snprintf(texts[i], sizeof(texts[i]), "%s %s", kPlaces[i], kModeLines[m]);
        else std::snprintf(texts[i], sizeof(texts[i]), "Take your chances: a random map or campaign arena that plays %s.",
                           m == Survival ? "survival" : kRowText[m]);
        const uint32_t want = m == Objective && originals[i] ? originals[i] : ours;
        if (current != want && WriteBytes(slot, &want, 4)) InterlockedIncrement(want == ours ? &applied : &restored);
    }
}
// Game thread, from the SetInfo tail before the refresh.
void Describe(uint8_t* s, int m, uint32_t index) {
    if (m < 0 || m >= kCount) m = Survival;
    __try {
        PointTexts(m);
        unsigned char any = 0;
        Read(reinterpret_cast<uintptr_t>(s) + menu::kMp20Any, &any, 1);
        if (any) return;
        uint32_t dgm = 0, maps = 0, count = 0;
        if (!Read(base + menu::kGameManager, &dgm, 4) || !dgm || !Read(dgm + menu::kDgmMaps, &maps, 4) || !Read(dgm + menu::kDgmMapCount, &count, 4) ||
            index >= count || !maps)
            return;
        char map[64]{};
        if (!menu::EntryMap(maps + index * menu::kMapEntrySize, map, sizeof(map))) return;
        char line[64]{};
        if (coop_arena::IsArenaName(map)) {
            const char* digits = std::strstr(map, "_ch");
            const unsigned chapter = digits ? static_cast<unsigned>(std::atoi(digits + 3)) : 0;
            std::snprintf(line, sizeof(line), "Campaign arena, chapter %u: %s.", chapter, kModeShort[m]);
        } else {
            const int i = coop_mpmap::MapIndex(map);
            if (i < 0 || coop_mpmap::kMapTable[i].kind == coop_mpmap::MapKind::Shipped) return;   // the text id above
            std::snprintf(line, sizeof(line), "Outbreak DLC map: %s.", kModeShort[m]);
        }
        line[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(line[0])));
        WriteBytes(reinterpret_cast<uintptr_t>(s) + 0x278, line, sizeof(line));
        InterlockedIncrement(&literals);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}
}  // namespace desc
void OnTail(SafetyHookContext& ctx) {
    uint8_t* s = reinterpret_cast<uint8_t*>(ctx.esi);
    __try {
        const uintptr_t at = reinterpret_cast<uintptr_t>(s);
        uint32_t allocated = 0, count = 0, items = 0, array = 0, map = 0;
        Read(at + kArraySize, &allocated, 4);
        Read(at + kItemCount, &count, 4);
        Read(at + kItems, &items, 4);
        Read(at + kArray, &array, 4);
        Read(at + menu::kMp20Index, &map, 4);
        Slot slot;
        bool third = false;
        const Verdict verdict = MovieVerdict(slot, third);
        // two or three items as the ctor patch left them, one after a stock fallback on this screen
        const bool rowsOk = allocated >= 2 && count >= 2 && items >= 2 && array;
        const bool native = active && rowsOk && (verdict == Mark || (verdict == Unknown && InterlockedCompareExchange(&lastFrontendRedirected, 0, 0) == 1));
        // The menu build after Show's first SetInfo hands [+0x220] to the scroll iterator,
        // so the count set here is the one up/down wraps over.
        const uint32_t rows = !native ? 1 : ctorRows == 3 && third && allocated >= 3 ? 3 : 2;
        const LONG d = InterlockedIncrement(&decisions);
        const bool verdictChanged = InterlockedExchange(&lastVerdict, verdict) != verdict;
        const bool nativeChanged = InterlockedExchange(&lastNative, native ? 1 : 0) != (native ? 1 : 0);
        const bool rowsChanged = InterlockedExchange(&lastRows, static_cast<LONG>(rows)) != static_cast<LONG>(rows);
        const bool changed = verdictChanged || nativeChanged || rowsChanged;
        if (d <= 8 || changed) {
            char entries[200];
            DescribeEntries(s, entries, sizeof(entries));
            mpmap::Event("native row: screen %08X movie=%s%s (slot %d aptData %08X) %s -> %s", static_cast<unsigned>(at), kVerdicts[verdict], third ? "+m2" : "", slot.index,
                         slot.aptData, entries,
                         rows == 3 ? "native rows with difficulty" : native ? (ctorRows == 3 ? "native row, two rows (the movie has no m2; SP02 asks)" : "native row")
                                   : "one row (tier 1)");
            if (d == 1 || changed) LogSlots();
        }
        if (!native) {
            if (InterlockedCompareExchange(&screen, 0, 0) == static_cast<LONG>(at)) InterlockedExchange(&screen, 0);
            if (rowsOk) {
                const uint32_t one = 1;
                WriteBytes(at + kItems, &one, 4);
                WriteBytes(at + kItemCount, &one, 4);
                InterlockedIncrement(&fallbacks);
            }
            // entry 1 stays valid either way; the stock tail fills entry 0
            if (allocated >= 2 && array) {
                const uint32_t e1[2] = {static_cast<uint32_t>(at + kName), map};
                WriteBytes(array + 8, e1, 8);
            }
            return;
        }
        if (count != rows || items != rows) {
            WriteBytes(at + kItems, &rows, 4);
            WriteBytes(at + kItemCount, &rows, 4);
            InterlockedIncrement(&rowChanges);
        }
        InterlockedExchange(&screen, static_cast<LONG>(at));
        const LONG m = InterlockedCompareExchange(&mode, 0, 0);
        const int shownMode = RowMode(s, m >= 0 && m < kCount ? m : 0);
        const char* text = kRowText[shownMode >= 0 && shownMode < kCount ? shownMode : 0];
        uint32_t e[6] = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(text)), static_cast<uint32_t>(m), static_cast<uint32_t>(at + kName), map, 0, 0};
        if (rows == 3) {
            const int level = RowDifficulty();
            e[4] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(coop_difficulty::Name(level)));
            e[5] = static_cast<uint32_t>(level);
        }
        WriteBytes(array, e, rows == 3 ? sizeof(e) : 16);
        InterlockedIncrement(&nativeFills);
        desc::Describe(s, shownMode, map);
        unsigned char shown = 0;
        if (Read(at + kActive, &shown, 1) && shown) {
            int row = Cursor(s);
            if (row < 0) row = 0;
            reinterpret_cast<void(__thiscall*)(void*, int, int)>(base + kRefreshRva)(s + kMovie, row, 0);
        }
        ctx.eip = base + kTailResumeRva;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}

__declspec(naked) void RightResume() {
    __asm {
        add esp, 8
        jmp dword ptr [rightResume]
    }
}
__declspec(naked) void LeftResume() {
    __asm {
        add esp, 8
        jmp dword ptr [leftResume]
    }
}
void Step(SafetyHookContext& ctx, bool right) {
    uint8_t* s = reinterpret_cast<uint8_t*>(ctx.esi);
    if (!active || !s || InterlockedCompareExchange(&screen, 0, 0) != static_cast<LONG>(reinterpret_cast<uintptr_t>(s))) return;
    __try {
        const uintptr_t at = reinterpret_cast<uintptr_t>(s);
        uint32_t array = 0, mapBefore = 0, mapAfter = 0;
        if (!Read(at + kArray, &array, 4) || !array) return;
        const int row = Cursor(s);
        const LONG before = InterlockedCompareExchange(&mode, 0, 0);
        const int levelBefore = difficulty::PickValue();
        Read(at + menu::kMp20Index, &mapBefore, 4);
        if (row == 0) {
            const LONG next = NextMode(s, before, right);
            if (ArenaRow(s)) InterlockedIncrement(&arenaSteps);
            else InterlockedIncrement(&modeSteps);
            if (next != before) InterlockedExchange(&mode, next);
            SetInfo(s, mapBefore);
        } else if (row == 2 && InterlockedCompareExchange(&lastRows, 0, 0) == 3) {
            difficulty::SetPick(StepDifficulty(RowDifficulty(), right));
            InterlockedIncrement(&difficultySteps);
            SetInfo(s, mapBefore);
        } else {
            uint32_t index = 0;
            Read(array + 12, &index, 4);
            const uint32_t next = StepMap(s, index, right, before);
            WriteBytes(array + 12, &next, 4);
            SetInfo(s, next);
            InterlockedIncrement(&mapSteps);
        }
        Read(at + menu::kMp20Index, &mapAfter, 4);
        const LONG steps = InterlockedCompareExchange(&modeSteps, 0, 0) + InterlockedCompareExchange(&mapSteps, 0, 0) + InterlockedCompareExchange(&arenaSteps, 0, 0) +
                           InterlockedCompareExchange(&difficultySteps, 0, 0);
        if (steps <= 40)
            mpmap::Event("native row: %s on row %d of %ld, mode %s -> %s, map %u -> %u, difficulty %s -> %s", right ? "right" : "left", row,
                         InterlockedCompareExchange(&lastRows, 0, 0), Name(before), Name(InterlockedCompareExchange(&mode, 0, 0)), mapBefore, mapAfter,
                         coop_difficulty::Valid(levelBefore) ? coop_difficulty::Name(levelBefore) : "none",
                         coop_difficulty::Valid(difficulty::PickValue()) ? coop_difficulty::Name(difficulty::PickValue()) : "none");
        ctx.edi = array;
        ctx.eip = reinterpret_cast<uintptr_t>(right ? &RightResume : &LeftResume);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}
void OnRight(SafetyHookContext& ctx) { Step(ctx, true); }
void OnLeft(SafetyHookContext& ctx) { Step(ctx, false); }

bool Matches(uint32_t rva, const unsigned char* expected, size_t size) {
    unsigned char actual[48]{};
    return size <= sizeof(actual) && Read(base + rva, actual, size) && std::memcmp(actual, expected, size) == 0;
}
// Header and the movie's declared size, so a stale or foreign file is never read.
bool CheckFile(FILE* log, const std::string& path) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        std::fprintf(log, "native row: %s not found (error %lu); build it with tools/build_frontend_override.py\n", path.c_str(), GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    static const unsigned char kHeader[12] = {'3', 's', 'l', 'o', 0x0C, 0, 0, 0, 2, 0, 3, 1};
    static const char kMovieName[] = "mp20gamesettings.apx";
    std::vector<unsigned char> chunk(1 << 20);
    unsigned char header[12]{};
    bool headerOk = false;
    long long movieAt = -1;
    uint32_t declared = 0;
    long long offset = 0;
    size_t carry = 0;
    DWORD got = 0;
    while (ReadFile(f, chunk.data() + carry, static_cast<DWORD>(chunk.size() - carry), &got, nullptr) && got) {
        const size_t have = carry + got;
        if (offset == 0 && have >= 12) {
            std::memcpy(header, chunk.data(), 12);
            headerOk = std::memcmp(header, kHeader, 12) == 0;
            if (!headerOk) break;
        }
        for (size_t i = 36; i + sizeof(kMovieName) <= have; ++i) {
            if (chunk[i] != 'm' || std::memcmp(chunk.data() + i, kMovieName, sizeof(kMovieName)) != 0 || std::memcmp(chunk.data() + i - 36, "RDHS", 4) != 0) continue;
            std::memcpy(&declared, chunk.data() + i - 4, 4);
            movieAt = offset + static_cast<long long>(i) - 36;
            break;
        }
        if (movieAt >= 0) break;
        // the last 64 bytes go round again, so a header across two reads is found
        carry = have < 64 ? have : 64;
        std::memmove(chunk.data(), chunk.data() + have - carry, carry);
        offset += static_cast<long long>(have - carry);
    }
    CloseHandle(f);
    // Mark's movie as he shipped it, or larger with the arena pictures (tools/build_map_pictures.py)
    const bool ok = headerOk && size.QuadPart % 0x20000 == 0 && movieAt >= 0 && declared >= kMovieSize && declared <= kMovieSize * 4;
    std::fprintf(log, "native row: file %s size=%lld header=%s movie header at 0x%llX declares %u bytes (at least Mark's %u, larger with map pictures) -> %s\n", path.c_str(),
                 static_cast<long long>(size.QuadPart), headerOk ? "ok" : "BAD", static_cast<unsigned long long>(movieAt < 0 ? 0 : movieAt), declared, kMovieSize,
                 ok ? "ok" : "REJECTED");
    return ok;
}

void Undo() {
    openHook.reset();
    tailHook.reset();
    rightHook.reset();
    leftHook.reset();
    if (ctorPatched) {
        const unsigned char one = 1;
        for (uint32_t site : kCtorSites) WriteBytes(base + site, &one, 1);
        ctorPatched = false;
    }
    InterlockedExchange(&redirecting, 0);
    active = false;
}

// The file open alone, for the suit list when the native row is off or did not go in.
void InstallOpenOnly(FILE* log) {
    if (openHook || (!InterlockedCompareExchange(&skinsRedirecting, 0, 0) && !InterlockedCompareExchange(&globalRedirecting, 0, 0) &&
                     !InterlockedCompareExchange(&loadingRedirecting, 0, 0) && !InterlockedCompareExchange(&mpRedirecting, 0, 0)))
        return;
    if (!Matches(kOpenRva, kOpen, sizeof(kOpen))) {
        InterlockedExchange(&skinsRedirecting, 0);
        InterlockedExchange(&globalRedirecting, 0);
        InterlockedExchange(&loadingRedirecting, 0);
        InterlockedExchange(&mpRedirecting, 0);
        std::fprintf(log, "in-game menus: the file open %08X differs from retail; the stock global_assets.str stays\n", static_cast<unsigned>(base + kOpenRva));
        std::fprintf(log, "loadout suit list: the file open %08X differs from retail; the stock mpskins.lua stays\n", static_cast<unsigned>(base + kOpenRva));
        return;
    }
    openHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kOpenRva), reinterpret_cast<void*>(OnOpen));
    if (!openHook) {
        InterlockedExchange(&skinsRedirecting, 0);
        InterlockedExchange(&globalRedirecting, 0);
        InterlockedExchange(&loadingRedirecting, 0);
        InterlockedExchange(&mpRedirecting, 0);
    }
    std::fprintf(log, "file open hook %s on its own (suit list, in-game menus)\n", openHook ? "in" : "FAILED");
}
void EarlyInstall(FILE* log, const std::string& ini) {
    startTick = GetTickCount();
    {
        char exe[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string folder(exe);
        folder = folder.substr(0, folder.find_last_of('\\') + 1);
        const bool loadout = GetPrivateProfileIntA("Loadout", "Enabled", 0, ini.c_str()) != 0;
        const DWORD attributes = GetFileAttributesA((folder + skinsPath).c_str());
        const bool present = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
        InterlockedExchange(&skinsRedirecting, loadout && present ? 1 : 0);
        std::fprintf(log, "loadout suit list: %s%s\n", !loadout ? "off ([Loadout] Enabled=0)" : present ? "mpskins.lua -> " : "no ", present || !loadout ? (loadout ? skinsPath : "") : skinsPath);
        // the red in-game menus and loading screens: whole streams of 128 KiB blocks with the stream header
        const bool red = GetPrivateProfileIntA("Menu", "RedInGameMenus", 1, ini.c_str()) != 0;
        const auto check = [&](const char* file, LONGLONG least) -> const char* {
            HANDLE h = CreateFileA((folder + file).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            static const unsigned char kHeader[] = {0x33, 0x73, 0x6C, 0x6F, 0x0C, 0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x01};
            unsigned char head[sizeof(kHeader)]{};
            DWORD got = 0;
            LARGE_INTEGER size{};
            const char* why = "ok";
            if (h == INVALID_HANDLE_VALUE) why = "missing";
            else if (!GetFileSizeEx(h, &size) || size.QuadPart < least || size.QuadPart % 0x20000) why = "bad size";
            else if (!ReadFile(h, head, sizeof(head), &got, nullptr) || got != sizeof(head) || std::memcmp(head, kHeader, sizeof(head)) != 0) why = "bad header";
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            return why;
        };
        const char* why = red ? check(globalPath, 0x1000000) : "off";
        const char* whyLoading = red ? check(loadingPath, 0x100000) : "off";
        const char* whyMp = red ? check(mpAssetsPath, 0x2000000) : "off";
        const bool use = red && std::strcmp(why, "ok") == 0, useLoading = red && std::strcmp(whyLoading, "ok") == 0, useMp = red && std::strcmp(whyMp, "ok") == 0;
        InterlockedExchange(&globalRedirecting, use ? 1 : 0);
        InterlockedExchange(&loadingRedirecting, useLoading ? 1 : 0);
        InterlockedExchange(&mpRedirecting, useMp ? 1 : 0);
        std::fprintf(log, "in-game menus: %s; loading screens: %s; MP screens: %s\n", !red ? "stock ([Menu] RedInGameMenus=0)" : use ? "red (randomizer\\ds2coop_global_assets.str)" : why,
                     !red ? "stock" : useLoading ? "red (randomizer\\ds2coop_loading_screen.str)" : whyLoading,
                     !red ? "stock" : useMp ? "red (randomizer\\ds2coop_global_mp_assets.str)" : whyMp);
    }
    const bool modes = GetPrivateProfileIntA("Menu", "Modes", 1, ini.c_str()) != 0;
    wanted = modes && GetPrivateProfileIntA("Menu", "NativeModeRow", 0, ini.c_str()) != 0;
    if (!wanted) {
        std::fprintf(log, "native row off (%s)\n", modes ? "[Menu] NativeModeRow=0" : "[Menu] Modes=0");
        InstallOpenOnly(log);
        return;
    }
    const int start = static_cast<int>(GetPrivateProfileIntA("Menu", "Mode", 0, ini.c_str()));
    InterlockedExchange(&mode, start >= 0 && start < kCount ? start : Survival);
    // read here: the difficulty module installs after the frontend opened
    threeWanted = GetPrivateProfileIntA("Difficulty", "Pick", 1, ini.c_str()) == 2;
    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string folder(exe);
    folder = folder.substr(0, folder.find_last_of('\\') + 1);
    char colours[16]{};
    GetPrivateProfileStringA("Menu", "FrontendColours", "red", colours, sizeof(colours), ini.c_str());
    if (!_stricmp(colours, "stock")) {
        if (GetFileAttributesA((folder + kStockOverridePath).c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::snprintf(overridePath, sizeof(overridePath), "%s", kStockOverridePath);
            std::fprintf(log, "frontend colours: stock (%s)\n", kStockOverridePath);
        } else {
            std::fprintf(log, "frontend colours: stock wanted, but %s is missing; the red copy stays\n", kStockOverridePath);
        }
    }
    if (!CheckFile(log, folder + overridePath)) {
        std::fprintf(log, "native row NOT installed: no usable frontend copy, the stock frontend and the label row stay\n");
        InstallOpenOnly(log);
        return;
    }
    // push 1 / push 'MP20'; mov [esi+0x220],1 / cmp [eax],1 / jge; push 8 / push 1 / push ecx
    static const unsigned char kCtorA[] = {0x6A, 0x01, 0x68, 0x30, 0x32, 0x50, 0x4D, 0x8B, 0xF1};
    static const unsigned char kCtorB[] = {0xC7, 0x86, 0x20, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x83, 0x38, 0x01, 0x7D, 0x21};
    static const unsigned char kCtorC[] = {0x6A, 0x08, 0x6A, 0x01, 0x51, 0xE8};
    // mov eax,[esi+0x214] / mov [eax],edi / mov ecx,[esi+0x214] / mov edx,[esi+0x22C] / mov [ecx+4],edx / cmp byte [esi+0x4C],0 / je / push 0 / push 0 / lea ecx,[esi+0x5C]
    static const unsigned char kTail[] = {0x8B, 0x86, 0x14, 0x02, 0x00, 0x00, 0x89, 0x38, 0x8B, 0x8E, 0x14, 0x02, 0x00, 0x00, 0x8B, 0x96, 0x2C, 0x02,
                                          0x00, 0x00, 0x89, 0x51, 0x04, 0x80, 0x7E, 0x4C, 0x00, 0x74, 0x0C, 0x6A, 0x00, 0x6A, 0x00, 0x8D, 0x4E, 0x5C, 0xE8};
    static const unsigned char kTailResume[] = {0x5F, 0x5E, 0x5B, 0xC2, 0x04, 0x00};
    // mov edi,[esi+0x214] / mov edx,[edi+4] / add esp,8 / push edx / mov ecx,esi / call Next
    static const unsigned char kRight[] = {0x8B, 0xBE, 0x14, 0x02, 0x00, 0x00, 0x8B, 0x57, 0x04, 0x83, 0xC4, 0x08, 0x52, 0x8B, 0xCE, 0xE8, 0xEF, 0xD1, 0xFC, 0xFF};
    // mov edi,[esi+0x214] / mov eax,[edi+4] / add esp,8 / push eax / mov ecx,esi / call Prev
    static const unsigned char kLeft[] = {0x8B, 0xBE, 0x14, 0x02, 0x00, 0x00, 0x8B, 0x47, 0x04, 0x83, 0xC4, 0x08, 0x50, 0x8B, 0xCE, 0xE8, 0x26, 0xD1, 0xFC, 0xFF};
    // push 0 / lea ecx,[esp+0x1C] / call (the arrow animation's string)
    static const unsigned char kResume[] = {0x6A, 0x00, 0x8D, 0x4C, 0x24, 0x1C, 0xE8};
    // cmp byte [ecx+0x2C],1 / jne / or eax,-1 / ret
    static const unsigned char kCursor[] = {0x80, 0x79, 0x2C, 0x01, 0x75, 0x04, 0x83, 0xC8, 0xFF, 0xC3};
    // mov eax,[esp+4] / push ebx / push esi / mov esi,[0x020213FC]
    static const unsigned char kNext[] = {0x8B, 0x44, 0x24, 0x04, 0x53, 0x56, 0x8B, 0x35, 0xFC, 0x13, 0x02, 0x02, 0x8B, 0x96, 0x30, 0x02, 0x00, 0x00, 0x57, 0x40};
    static const unsigned char kPrev[] = {0x8B, 0x44, 0x24, 0x04, 0x53, 0x56, 0x8B, 0x35, 0xFC, 0x13, 0x02, 0x02, 0x8B, 0x96, 0x30, 0x02, 0x00, 0x00, 0x57, 0x83};
    struct Site { const char* name; uint32_t rva; const unsigned char* bytes; size_t size; };
    const Site sites[] = {
        {"open", kOpenRva, kOpen, sizeof(kOpen)},
        {"ctor count", 0x6AB7B4, kCtorA, sizeof(kCtorA)},
        {"ctor items", 0x6AB82A, kCtorB, sizeof(kCtorB)},
        {"ctor array", 0x6AB847, kCtorC, sizeof(kCtorC)},
        {"SetInfo tail", kTailRva, kTail, sizeof(kTail)},
        {"SetInfo end", kTailResumeRva, kTailResume, sizeof(kTailResume)},
        {"input right", kRightRva, kRight, sizeof(kRight)},
        {"input left", kLeftRva, kLeft, sizeof(kLeft)},
        {"right resume", kRightResumeRva, kResume, sizeof(kResume)},
        {"left resume", kLeftResumeRva, kResume, sizeof(kResume)},
        {"cursor", kCursorRva, kCursor, sizeof(kCursor)},
        {"next", kNextRva, kNext, sizeof(kNext)},
        {"prev", kPrevRva, kPrev, sizeof(kPrev)},
    };
    char failed[256]{};
    int bad = 0;
    for (const Site& s : sites) {
        if (Matches(s.rva, s.bytes, s.size)) continue;
        ++bad;
        const size_t used = std::strlen(failed);
        std::snprintf(failed + used, sizeof(failed) - used, " %s %08X", s.name, static_cast<unsigned>(base + s.rva));
    }
    if (bad) {
        std::fprintf(log, "native row NOT installed: %d site(s) differ from retail:%s\n", bad, failed);
        InstallOpenOnly(log);
        return;
    }
    rightResume = base + kRightResumeRva;
    leftResume = base + kLeftResumeRva;
    openHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kOpenRva), reinterpret_cast<void*>(OnOpen));
    tailHook = safetyhook::create_mid(reinterpret_cast<void*>(base + kTailRva), OnTail);
    rightHook = safetyhook::create_mid(reinterpret_cast<void*>(base + kRightRva), OnRight);
    leftHook = safetyhook::create_mid(reinterpret_cast<void*>(base + kLeftRva), OnLeft);
    const bool hooks = openHook && tailHook && rightHook && leftHook;
    int written = 0;
    const unsigned char rows = threeWanted ? 3 : 2;
    if (hooks) {
        ctorPatched = true;
        for (uint32_t site : kCtorSites) {
            unsigned char now = 0;
            if (WriteBytes(base + site, &rows, 1) && Read(base + site, &now, 1) && now == rows) ++written;
        }
    }
    if (!hooks || written != 4) {
        std::fprintf(log, "native row NOT installed: open=%d tail=%d right=%d left=%d ctor=%d/4; everything undone\n", openHook ? 1 : 0, tailHook ? 1 : 0, rightHook ? 1 : 0,
                     leftHook ? 1 : 0, written);
        Undo();
        InstallOpenOnly(log);
        return;
    }
    active = true;
    ctorRows = rows;
    InterlockedExchange(&redirecting, 1);
    std::fprintf(log, "native row installed at +%lu ms: frontend\\frontend.str -> %s, %d sites verified, MP20 has %u items%s, start mode %s; frontend opens before the hook: "
                      "unknown, first one seen is logged\n",
                 GetTickCount() - startTick, overridePath, static_cast<int>(sizeof(sites) / sizeof(sites[0])), static_cast<unsigned>(rows),
                 rows == 3 ? " ([Difficulty] Pick=2: the difficulty row when the movie has m2, else two)" : "", Name(InterlockedCompareExchange(&mode, 0, 0)));
}

void WriteDiagnostics(FILE* log) {
    static LONG lastDesc = -1;
    const LONG descNow = desc::applied + desc::restored * 3 + desc::literals * 7 + desc::missing * 11 + desc::faults * 13;
    if (descNow != lastDesc) {
        lastDesc = descNow;
        std::fprintf(log, "gamemode descriptions ours=%ld originals=%ld literals=%ld missing=%ld faults=%ld (objective keeps the game's texts)\n", desc::applied,
                     desc::restored, desc::literals, desc::missing, desc::faults);
    }
    static LONG lastGlobal = -1;
    const LONG globalNow = InterlockedCompareExchange(&globalOpens, 0, 0) * 2 + InterlockedCompareExchange(&globalRedirects, 0, 0) +
                           InterlockedCompareExchange(&loadingOpens, 0, 0) * 64 + InterlockedCompareExchange(&loadingRedirects, 0, 0) * 32 +
                           InterlockedCompareExchange(&mpOpens, 0, 0) * 4096 + InterlockedCompareExchange(&mpRedirects, 0, 0) * 2048 +
                           InterlockedCompareExchange(&mpNearMisses, 0, 0) * 262144;
    if (globalNow != lastGlobal) {
        lastGlobal = globalNow;
        std::fprintf(log, "in-game menus: global_assets opens=%ld redirected=%ld (redirect %s) | loading screens opens=%ld redirected=%ld (redirect %s)\n",
                     InterlockedCompareExchange(&globalOpens, 0, 0), InterlockedCompareExchange(&globalRedirects, 0, 0),
                     InterlockedCompareExchange(&globalRedirecting, 0, 0) ? "on" : "off", InterlockedCompareExchange(&loadingOpens, 0, 0),
                     InterlockedCompareExchange(&loadingRedirects, 0, 0), InterlockedCompareExchange(&loadingRedirecting, 0, 0) ? "on" : "off");
        std::fprintf(log, "in-game menus: global_mp_assets opens=%ld redirected=%ld (redirect %s), other opens naming it=%ld\n", InterlockedCompareExchange(&mpOpens, 0, 0),
                     InterlockedCompareExchange(&mpRedirects, 0, 0), InterlockedCompareExchange(&mpRedirecting, 0, 0) ? "on" : "off",
                     InterlockedCompareExchange(&mpNearMisses, 0, 0));
    }
    for (; mpMissesPrinted < 8 && InterlockedCompareExchange(&mpMissReady[mpMissesPrinted], 0, 0); ++mpMissesPrinted)
        std::fprintf(log, "in-game menus: an open naming global_mp_assets but not the stream itself: %s\n", mpMissPaths[mpMissesPrinted]);
    if (!wanted) return;
    const LONG ready = InterlockedCompareExchange(&noteCount, 0, 0);
    for (; notesPrinted < ready && notesPrinted < kNotes && InterlockedCompareExchange(&noteReady[notesPrinted], 0, 0); ++notesPrinted) {
        const OpenNote& n = notes[notesPrinted];
        std::fprintf(log, "native row open #%ld +%lu ms thread %lu caller %08X%s%s: %s\n", notesPrinted + 1, n.at, n.thread, n.caller, n.frontend ? " FRONTEND" : "",
                     n.redirected ? " -> redirected" : "", n.path);
    }
    static LONG last[12] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    const LONG now[12] = {InterlockedCompareExchange(&frontendOpens, 0, 0), InterlockedCompareExchange(&redirects, 0, 0), InterlockedCompareExchange(&decisions, 0, 0),
                          InterlockedCompareExchange(&nativeFills, 0, 0), InterlockedCompareExchange(&fallbacks, 0, 0), InterlockedCompareExchange(&modeSteps, 0, 0),
                          InterlockedCompareExchange(&mapSteps, 0, 0), InterlockedCompareExchange(&faults, 0, 0), InterlockedCompareExchange(&arenaSteps, 0, 0),
                          InterlockedCompareExchange(&difficultySteps, 0, 0), InterlockedCompareExchange(&lastRows, 0, 0), InterlockedCompareExchange(&rowChanges, 0, 0)};
    if (std::memcmp(last, now, sizeof(now)) == 0) return;
    std::memcpy(last, now, sizeof(now));
    const LONG v = InterlockedCompareExchange(&lastVerdict, 0, 0), nat = InterlockedCompareExchange(&lastNative, 0, 0);
    std::fprintf(log, "native_mode_row=%s movie=%s opens=%ld frontend_opens=%ld redirected=%ld first_frontend_open=+%ld ms setinfo=%ld native_fills=%ld fallbacks=%ld "
                      "mode_steps=%ld map_steps=%ld arena_steps=%ld faults=%ld | rows=%ld of %u patched, row_count_writes=%ld difficulty_steps=%ld\n",
                 !active ? "off" : nat == 1 ? "active" : nat == 0 ? "fallback" : "waiting", v >= 0 && v <= Mark ? kVerdicts[v] : "not seen",
                 InterlockedCompareExchange(&opens, 0, 0), now[0], now[1], InterlockedCompareExchange(&firstFrontendTick, 0, 0) & ~1L, now[2], now[3], now[4], now[5], now[6],
                 now[8], now[7], now[10], static_cast<unsigned>(ctorRows), now[11], now[9]);
}
}  // namespace native

__declspec(noinline) void Refresh(uint8_t* screen) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, int, int)>(base + kRefreshRva)(screen + kMovie, 0, 0);
        InterlockedIncrement(&refreshes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}
// "<label> [mode]" in the row's own buffer; an arena row shows the mode it plays.
void Label(uint8_t* screen) {
    unsigned char active = 0;
    if (ArenaRow(screen)) InterlockedIncrement(&arenaRows);
    char* name = reinterpret_cast<char*>(screen + kName);
    char label[kNameSize]{};
    std::memcpy(label, name, kNameSize - 1);
    // a label from an earlier pass is cut back first
    if (char* bracket = std::strstr(label, " [")) *bracket = 0;
    char tag[24];
    std::snprintf(tag, sizeof(tag), " [%s]", Name(RowMode(screen, InterlockedCompareExchange(&mode, 0, 0))));
    const size_t room = kNameSize - 1 - std::strlen(tag);
    if (std::strlen(label) > room) label[room] = 0;
    std::snprintf(name, kNameSize, "%s%s", label, tag);
    InterlockedIncrement(&labels);
    if (Read(reinterpret_cast<uintptr_t>(screen) + kActive, &active, 1) && active) Refresh(screen);
}
void __fastcall OnSetInfo(uint8_t* screen, void*, uint32_t index) {
    mappictures::BeforeMp20(screen, index);   // SetInfo refreshes the movie: its picture key first
    setInfoHook.thiscall<void>(screen, index);
    if (!enabled || !screen || InterlockedCompareExchange(&native::screen, 0, 0) == static_cast<LONG>(reinterpret_cast<uintptr_t>(screen))) return;
    __try {
        Label(screen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}
void NavSound() {
    uint32_t hash = 0;
    if (Read(base + kNavSoundRva, &hash, 4) && hash) survmusic::Send(hash);
}
}  // namespace gamemode
namespace gamemode_nav { void Sound() { gamemode::NavSound(); } }
namespace gamemode {
uint32_t __fastcall OnInput(uint8_t* screen, void*, int code) {
    // with the native row up/down moves between the two rows
    if (enabled && screen && (code == 8 || code == 9) &&
        InterlockedCompareExchange(&native::screen, 0, 0) != static_cast<LONG>(reinterpret_cast<uintptr_t>(screen))) {
        const LONG before = InterlockedCompareExchange(&mode, 0, 0);
        const LONG next = NextMode(screen, before, code == 9);
        InterlockedExchange(&mode, next);
        InterlockedIncrement(&cycles);
        uint32_t rows = 0, index = 0;
        if (Read(reinterpret_cast<uintptr_t>(screen) + kRows, &rows, 4) && rows && Read(rows + 4, &index, 4)) {
            __try {
                reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kSetInfoRva)(screen, index);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedIncrement(&faults);
            }
        }
        NavSound();
        mpmap::Event("gamemode: %s -> %s (map picker %s)", Name(before), Name(next), code == 9 ? "down" : "up");
        return 2;
    }
    const uint32_t result = reinterpret_cast<uint32_t(__thiscall*)(void*, int)>(inputOriginal)(screen, code);
    // one row: the stock left/right stepped the map in entry 0; past the maps that cannot play the mode
    if (enabled && screen && (code == 6 || code == 7) &&
        InterlockedCompareExchange(&native::screen, 0, 0) != static_cast<LONG>(reinterpret_cast<uintptr_t>(screen))) {
        __try {
            const LONG picked = InterlockedCompareExchange(&mode, 0, 0);
            uint32_t rows = 0, index = 0;
            if (Read(reinterpret_cast<uintptr_t>(screen) + kRows, &rows, 4) && rows && Read(rows + 4, &index, 4) && !IndexPlays(index, picked)) {
                const uint32_t next = StepMap(screen, index, code == 7, picked);
                if (next != index) {
                    WriteBytes(rows + 4, &next, 4);
                    reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kSetInfoRva)(screen, next);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&faults);
        }
    }
    return result;
}

// The survival director's Objective result asks for the map again: the checkpoint
// restart the pause menu offers (GameMainLoop::CheckpointRestart 0x0040F840,
// thiscall bool(), false while a level change is under way). It runs on the game
// thread from Poll, and the checkpoint module makes the partners follow.
constexpr uint32_t kCheckpointRestartRva = 0xF840;
bool restartVerified = false;
volatile LONG restartWanted = 0, restartsAsked = 0, restartsDone = 0, restartsRefused = 0;
int RequestRestart() {
    if (!restartVerified) return 0;
    InterlockedExchange(&restartWanted, 1);
    InterlockedIncrement(&restartsAsked);
    return 1;
}
bool CheckpointRestartNow(const char* why) {
    const uint32_t loop = level::GameMainLoop();
    bool ok = false;
    if (loop) {
        lifecycle::restartReleasing = true;
        __try {
            ok = reinterpret_cast<bool(__thiscall*)(void*)>(base + kCheckpointRestartRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(loop)));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&faults);
        }
        lifecycle::restartReleasing = false;
    }
    InterlockedIncrement(ok ? &restartsDone : &restartsRefused);
    mpmap::Event("mode restart (%s): CheckpointRestart on GameMainLoop %08X %s", why ? why : "?", loop, ok ? "accepted" : "REFUSED");
    return ok;
}
// With partners the host first tells everybody ([Lifecycle] SyncedRestart) and restarts once they
// stopped bringing downed players back, or after RestartAckMs.
void PollRestart() {
    if (!InterlockedExchange(&restartWanted, 0)) return;
    if (lifecycle::BeginRestart("the director's run end")) return;
    CheckpointRestartNow("the director's run end");
}
void VerifyRestart(FILE* log) {
    // push esi / mov esi,ecx / call (busy check) / test al,al / je +4 / xor al,al / pop esi / ret
    static const unsigned char kHead[] = {0x56, 0x8B, 0xF1, 0xE8};
    static const unsigned char kTail[] = {0x84, 0xC0, 0x74, 0x04, 0x32, 0xC0, 0x5E, 0xC3};
    unsigned char head[sizeof(kHead)]{}, tail[sizeof(kTail)]{};
    restartVerified = Read(base + kCheckpointRestartRva, head, sizeof(head)) && std::memcmp(head, kHead, sizeof(head)) == 0 &&
                      Read(base + kCheckpointRestartRva + 8, tail, sizeof(tail)) && std::memcmp(tail, kTail, sizeof(tail)) == 0;
    std::fprintf(log, "gamemode objective restart: CheckpointRestart %08X %s\n", static_cast<unsigned>(base + kCheckpointRestartRva),
                 restartVerified ? "verified" : "MISMATCH (the Objective result cannot reload the map)");
}

void Install(FILE* log, const std::string& ini) {
    VerifyRestart(log);
    enabled = menu::installed && GetPrivateProfileIntA("Menu", "Modes", 1, ini.c_str()) != 0;
    const int wanted = static_cast<int>(GetPrivateProfileIntA("Menu", "Mode", 0, ini.c_str()));
    // Without the mode row every visit plays survival; [Menu] Mode only picks where the
    // row starts (13.10.: Modes=0 kept Mode, so every visit played that mode for good).
    InterlockedExchange(&mode, enabled && wanted >= 0 && wanted < kCount ? wanted : Survival);
    if (!enabled) {
        std::fprintf(log, "gamemode off (%s): every visit plays survival%s\n", menu::installed ? "[Menu] Modes=0" : "the menu module is not installed",
                     wanted != Survival ? " ([Menu] Mode ignored)" : "");
        return;
    }
    // mov eax,[esp+4] / push ebx / mov ebx,[0x020213FC] / push esi / mov esi,ecx / mov [esi+0x22C],eax
    static const unsigned char kSetInfo[] = {0x8B, 0x44, 0x24, 0x04, 0x53, 0x8B, 0x1D, 0xFC, 0x13, 0x02, 0x02, 0x56, 0x8B, 0xF1, 0x89, 0x86, 0x2C, 0x02, 0x00, 0x00};
    // sub esp,0x1C / push ebx / mov ebx,[esp+0x24] / push esi / mov eax,ebx / sub eax,6
    static const unsigned char kInput[] = {0x83, 0xEC, 0x1C, 0x53, 0x8B, 0x5C, 0x24, 0x24, 0x56, 0x8B, 0xC3, 0x83, 0xE8, 0x06};
    unsigned char a[sizeof(kSetInfo)]{}, b[sizeof(kInput)]{};
    const bool setInfoOk = Read(base + kSetInfoRva, a, sizeof(a)) && std::memcmp(a, kSetInfo, sizeof(a)) == 0;
    const bool inputOk = Read(base + kInputRva, b, sizeof(b)) && std::memcmp(b, kInput, sizeof(b)) == 0;
    if (setInfoOk && inputOk) {
        setInfoHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kSetInfoRva), reinterpret_cast<void*>(OnSetInfo));
        inputOriginal = static_cast<uint32_t>(base + kInputRva);
        if (!setInfoHook || !menu::SwapSlot(log, "MP20 input", kInputSlot, kInputRva, reinterpret_cast<void*>(OnInput))) {
            setInfoHook.reset();
            enabled = false;
        }
    } else {
        enabled = false;
    }
    if (!enabled) InterlockedExchange(&mode, Survival);   // no row to change it: survival, as with Modes=0
    std::fprintf(log, "gamemode %s: SetInfo %08X %s, HandleInput %08X %s; start mode %s; up/down in the map picker cycles %d modes; native row %s\n",
                 enabled ? "installed" : "NOT installed", static_cast<unsigned>(base + kSetInfoRva), setInfoOk ? "verified" : "MISMATCH",
                 static_cast<unsigned>(base + kInputRva), inputOk ? "verified" : "MISMATCH", Name(InterlockedCompareExchange(&mode, 0, 0)), static_cast<int>(kCount),
                 native::active ? (native::ctorRows == 3 ? "installed with the difficulty row (the label row is its fallback)" : "installed (the label row is its fallback)")
                                : native::wanted ? "NOT installed" : "off");
}
void WriteDiagnostics(FILE* log) {
    native::WriteDiagnostics(log);
    static LONG lastRestarts = -1;
    const LONG restarts = InterlockedCompareExchange(&restartsAsked, 0, 0) + InterlockedCompareExchange(&restartsRefused, 0, 0) * 100;
    if (restarts != lastRestarts) {
        lastRestarts = restarts;
        if (restarts) std::fprintf(log, "gamemode objective restarts asked=%ld done=%ld refused=%ld\n", InterlockedCompareExchange(&restartsAsked, 0, 0),
                                   InterlockedCompareExchange(&restartsDone, 0, 0), InterlockedCompareExchange(&restartsRefused, 0, 0));
    }
    if (!enabled) return;
    std::fprintf(log, "gamemode mode=%s cycles=%ld labels=%ld refreshes=%ld arena_rows=%ld lock_skips=%ld faults=%ld\n", Name(InterlockedCompareExchange(&mode, 0, 0)),
                 InterlockedCompareExchange(&cycles, 0, 0), InterlockedCompareExchange(&labels, 0, 0), InterlockedCompareExchange(&refreshes, 0, 0),
                 InterlockedCompareExchange(&arenaRows, 0, 0), InterlockedCompareExchange(&lockSkips, 0, 0), InterlockedCompareExchange(&faults, 0, 0));
}
}  // namespace gamemode
