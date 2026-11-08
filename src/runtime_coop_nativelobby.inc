// Dead Space 2 United by Hafer.
// The game's own pre-game lobby screens in front of our menu lobby
// ([NativeLobby] in DS2United.ini; evidence: analysis/DS2_NATIVE_LOBBY_2026-10-11.md).
//
// MP00 (the lobby frame: tabs, countdown, quit popup) and its tab screen MP02
// (eight player rows, map info) run offline as long as GetJoinedGame answers
// null for them, which it does. Our part:
//   - the host's map picker select pushes MP00 instead of launching; a joiner's
//     game tick pushes it once the host's lobby list names a map;
//   - MP00 OnShow takes its online branch (StartLobby, MP02) and MP00 Tick draws
//     the countdown we keep in MPLobbyManager+0x18;
//   - MP02 Tick fills the rows from the lobby list instead of the Blaze game;
//   - the stats and settings tabs, invite and mute stay shut (they reach Blaze);
//   - the quit popup's Yes goes back to the Multiplayer screen;
//   - at zero the host launches through the map picker's own route and the
//     joiners follow the launch packet as before.
//   - the host starts the match: by default the countdown waits ("waiting for
//     players...") until he clicks LAUNCH on the help bar (Mark's RIG-LINK has the
//     same host button) or presses select (A, Enter); [NativeLobby] AutoStartSeconds
//     brings back a countdown once MinPlayers are in. A click on a player row also
//     arrives as select and no longer starts the match.
// A campaign arena ([Arena] Modes) goes through the same screen: its map byte is the
// arena code (coop_lobby::ArenaCode), MP02 shows its map list entry (the arena's label).
namespace loadout { bool LobbyTabsOpen(LONG mode); }   // runtime_coop_loadout.inc: the weapon and suit tabs, Versus and Mercenaries
namespace nativelobby {
bool enabled = false, installed = false;
int minPlayers = 2;
int32_t countdownMs = 0, startNowMs = 3000;   // countdownMs 0: only the host starts

// RVAs (image base 0x400000)
constexpr uint32_t kMp00OnShowSite = 0x66559C, kMp00TickSite = 0x649B12, kMp02Tick = 0x6658B0, kChangeTab = 0x649C30;
constexpr uint32_t kMp00ItemStateSlot = 0x1737524, kMp00ItemState = 0x649950;
constexpr uint32_t kMp02InputSlot = 0x17375B8, kMp02Input = 0x689390;
constexpr uint32_t kMp02CommandSlot = 0x1737628, kMp02Command = 0x649E80;
constexpr uint32_t kMp02SelectSlot = 0x1737618, kMp02Select = 0x636280;
constexpr uint32_t kQuitResultSlot = 0x172EBB8, kQuitResult = 0x649CB0;
constexpr uint32_t kUpdateActiveItem = 0x11EA310, kUpdateItem = 0x11E9800, kHelpBar = 0xC7C320, kActiveIndex = 0x11E8C50;
constexpr uint32_t kStopLobby = 0x654010, kPopToScreen = 0x6627A0, kStartTransition = 0x67BB40, kCurrentScreen = 0x634F00;
constexpr uint32_t kSetStreamList = 0xF780C0, kSetStreamListSlot = 0x4C;
constexpr uint32_t kEndOfMatch = 0x1C89C29, kLobbyManager = 0x1C213E8, kFrontendBlock = 0x1C89C38, kFrontendManager = 0x1C1C0E4, kOnlineServices = 0x1C213DC;
constexpr uint32_t kStateActiveDisabled = 0x172EB6C, kStateInactiveDisabled = 0x172EB60;
constexpr uint32_t kMp00Hash = 0x57808EE7, kMp20Hash = 0xD00E0435;
constexpr uint32_t kMp00Offset = 0x6BEC, kMp02Offset = 0x6E38;
constexpr uint32_t kCommandInvite = 0x696E7669, kCommandMute = 0x6D757465;   // 'invi', 'mute'
// MP02's help bar (vtable 0x01B37528 slot 6 = 0x00A49DF0, thiscall(bar), plain ret) adds its
// buttons through AddButton (0x01097D20, thiscall(id, label hash, glyph code), ret 0xC); a
// click comes back to the FSCommand as type 6 with the id. GetLocalizedTextStatic
// (0x01371280, cdecl(hash), plain ret) only for the log.
constexpr uint32_t kMp02HelpSlot = 0x1737540, kMp02Help = 0x649DF0, kAddHelpButton = 0xC97D20, kLocalizedText = 0xF71280;
constexpr uint32_t kCommandLaunch = 0x6C6E6368, kCommandBack = 0x6261636B;   // 'lnch' (as Mark's), 'back'
constexpr uint32_t kLabelQuit = 0xF13FCD9C, kLabelLaunch = 0x68686F1E;      // "quit", "LAUNCH" (stock text)
constexpr DWORD kRowClickSelectMs = 300;   // a row click injects select (13): not a launch

struct Sig { const char* name; uint32_t rva; const char* hex; };
const Sig kSigs[] = {
    {"MP00 OnShow", 0x665580, "56 8B F1 C7 86 48 02 00 00 FF FF FF FF A1 DC 13"},
    {"MP00 OnShow branch", kMp00OnShowSite, "84 C0 74 5B 8B 0D E8 13 02 02"},
    {"MP00 Tick", 0x649AF0, "A1 DC 13 02 02 83 EC 20 55 56 8B F1 8B 48 24 8B"},
    {"MP00 Tick game test", kMp00TickSite, "33 ED 39 6C 24 0C 0F 84 FC 00 00 00"},
    {"MP02 Tick", kMp02Tick, "83 EC 24 53 55 8B E9 80 BD 88 00 00 00 00 56 57"},
    {"MP02 OnShow", 0x6AACB0, "B8 40 10 00 00 E8 B6 5D 98 00 A1 DC 13 02 02 53"},
    {"MP00 DoUpdateItemAndLabel", kMp00ItemState, "A1 E4 C0 01 02 56 8B 74 24 10 57 8B F9 8B 48 38"},
    {"MP00 ConditionalChangeTab", kChangeTab, "83 EC 0C 56 8B 74 24 14 57 8B 7C 24 1C 3B F7 74"},
    {"quit popup result", kQuitResult, "8B 44 24 04 80 78 04 00 75 2F 83 78 08 00 75 29"},
    {"MP02 HandleInput", kMp02Input, "51 A1 38 9C 08 02 56 8B F1 85 C0 74 13 80 B8 BC"},
    {"MP02 FSCommand", kMp02Command, "8B 44 24 08 55 8B 6C 24 08 57 8B F9 85 C0 74 3D"},
    {"MP02 OnInput_Select", kMp02Select, "32 C0 C2 04 00"},
    {"MPLobbyManager::StopLobby", kStopLobby, "56 8B F1 80 7E 10 00 74 45 68 44 B2 05 02 8D 4E"},
    {"Apt3dDirector::PopToScreenHelper", kPopToScreen, "53 56 8B F1 33 DB 39 9E 3C 04 00 00"},
    {"Apt3dDirector::StartTransition", kStartTransition, "83 EC 20 53 55 56 8B F1 8B 86 30 03"},
    {"Apt3dDirector current screen", kCurrentScreen, "8B 91 F8 03 00 00 33 C0 85 D2 7C 09"},
    {"UIExternScrollMenuIterator::UpdateItem", kUpdateItem, "51 8B 44 24 08 53 56 8B F1 8B 5E 40"},
    {"UIExternScrollMenuIterator::UpdateActiveItem", kUpdateActiveItem, "8B 41 28 6A 00 50 E8 E5 F4 FF FF C3"},
    {"UIExternScrollMenuIterator active index", kActiveIndex, "80 79 2C 01 75 04 83 C8 FF C3"},
    {"help bar refresh", kHelpBar, "56 8B F1 80 7E 30 00 0F 84 90 00 00"},
    {"IGameManager::SetStreamList", kSetStreamList, "8B 44 24 04 83 EC 18 56 8B F1 57 8D 7E 4C"},
};
size_t Hex(const char* hex, unsigned char* out, size_t cap) {
    size_t n = 0;
    while (*hex && n < cap) {
        while (*hex == ' ') ++hex;
        if (!hex[0] || !hex[1]) break;
        out[n++] = static_cast<unsigned char>(std::strtoul(std::string(hex, 2).c_str(), nullptr, 16));
        hex += 2;
    }
    return n;
}

// --- state ------------------------------------------------------------------------------------
enum class Side { None, Host, Join };
volatile LONG armed = 0;
Side side = Side::None;
int mapIndex = -1;               // coop_mpmap::kMapTable, or an arena code (coop_lobby::ArenaCode)
char mapName[64] = "";
DWORD armedAt = 0, lastOpenTry = 0;
int32_t countdown = -1;          // ms, -1 = waiting for players
DWORD countdownAt = 0;
volatile LONG startNowRequests = 0, leaveRequested = 0;
bool startNow = false, watched = false, popPending = false, launchCheck = false, hintShown = false;
DWORD popSince = 0;
LONG launchesAtArm = 0, menuLaunchesBefore = 0, menuRefusedBefore = 0;
volatile LONG opens = 0, openRefusals = 0, pushFailures = 0, pops = 0, popFailures = 0, launches = 0, relaunchWaits = 0, faults = 0;
volatile LONG showForced = 0, showNative = 0, mp00TickForced = 0, mp02Ours = 0, mp02Native = 0, rebuilds = 0, updateItems = 0, badRows = 0;
volatile LONG tabOverrides = 0, tabRefusals = 0, inputSwallowed = 0, commandsSwallowed = 0, quitYes = 0, quitNo = 0, selects = 0;
unsigned lines = 0;
void Line(const char* format, ...) {
    if (++lines > 1000) return;
    char buffer[400];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    vlog::WriteText("lobby screen", buffer);
}
bool Armed() { return InterlockedCompareExchange(&armed, 0, 0) != 0; }
uint32_t Ptr(uint32_t address) {
    uint32_t v = 0;
    Read(address, &v, 4);
    return v;
}

// The rows MP02 shows, built on the game tick and copied by MP02 Tick.
struct View { coop_lobby::Row rows[coop_lobby::kRows]; unsigned count = 0, local = 0; bool teams = false; uint32_t version = 0; };
SRWLOCK viewLock = SRWLOCK_INIT;
View view;
DWORD viewAt = 0;
void SetView(const coop_lobby::Row (&rows)[coop_lobby::kRows], unsigned count, unsigned local, bool teams, DWORD now) {
    AcquireSRWLockExclusive(&viewLock);
    const bool same = view.count == count && view.local == local && view.teams == teams && std::memcmp(view.rows, rows, sizeof(view.rows)) == 0;
    // pings move all the time: at most two rebuilds a second
    if (!same && (now - viewAt >= 500 || view.count != count)) {
        std::memcpy(view.rows, rows, sizeof(view.rows));
        view.count = count;
        view.local = local;
        view.teams = teams;
        ++view.version;
        viewAt = now;
    }
    ReleaseSRWLockExclusive(&viewLock);
}

// The map byte's name: an MP map, or a campaign arena's "arena_..." name.
const char* CodeName(int code) {
    const int arenaIndex = coop_lobby::ArenaIndexOf(code);
    if (arenaIndex >= 0) return arenaIndex < static_cast<int>(coop_arena::kArenaCount) ? coop_arena::kArenas[arenaIndex].name : "?";
    return coop_mpmap::MapName(code);
}

// --- the screens ------------------------------------------------------------------------------
// Trap 1: OnShow asks IsInMultiplayerGame (false without a hub) and else queues
// a frontend reset; armed, it takes the online branch.
safetyhook::MidHook showHook, mp00TickHook;
void OnMp00Show(safetyhook::Context& ctx) {
    if (!Armed()) { InterlockedIncrement(&showNative); return; }
    ctx.eax |= 1;
    InterlockedIncrement(&showForced);
}
// Trap 2: MP00 Tick only draws the countdown with a joined game; the pointer is
// tested and never followed.
void OnMp00Tick(safetyhook::Context& ctx) {
    if (!Armed()) return;
    *reinterpret_cast<uint32_t*>(ctx.esp + 0xC) = 1;
    InterlockedIncrement(&mp00TickForced);
}

// The rows: 16 bytes each at [MP02+0x220], read by DoUpdateItemAndLabel. The
// names are pointer variables the movie reads on every redraw, so they live here.
#pragma pack(push, 1)
struct NativeRow { uint32_t name; uint8_t remote, muted, speaking, headset; int32_t level, signal; };
#pragma pack(pop)
static_assert(sizeof(NativeRow) == 16, "MP02 row size");
char names[coop_lobby::kRows][48];
uint32_t shownVersion = ~0u;
safetyhook::InlineHook mp02TickHook, changeTabHook;
using ThisFn = void(__thiscall*)(void*);
using UpdateItemFn = void(__thiscall*)(void*, int, bool);
__declspec(noinline) void FillScreen(uintptr_t s) {
    __try {
        *reinterpret_cast<uint8_t*>(s + 0x23C) = 0;
        if (!*reinterpret_cast<uint8_t*>(s + 0x88)) reinterpret_cast<ThisFn>(base + kUpdateActiveItem)(reinterpret_cast<void*>(s + 0x5C));
        const uint32_t lobbyManager = Ptr(base + kLobbyManager);
        uint8_t* dirty = lobbyManager ? reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(lobbyManager) + 0x11) : nullptr;
        View v;
        AcquireSRWLockShared(&viewLock);
        v = view;
        ReleaseSRWLockShared(&viewLock);
        if (v.version == shownVersion && !(dirty && *dirty)) return;
        const uint32_t rows = *reinterpret_cast<uint32_t*>(s + 0x220), size = *reinterpret_cast<uint32_t*>(s + 0x224);
        if (!rows || size < coop_lobby::kRows) { InterlockedIncrement(&badRows); return; }
        if (dirty) *dirty = 0;
        shownVersion = v.version;
        *reinterpret_cast<uint32_t*>(s + 0x238) = v.count;
        for (unsigned i = 0; i < coop_lobby::kRows; ++i) {
            const coop_lobby::Row& r = v.rows[i];
            NativeRow& n = *reinterpret_cast<NativeRow*>(static_cast<uintptr_t>(rows) + 16u * i);
            if (!(r.flags & coop_lobby::kRowUsed)) { n = NativeRow{0, 0, 0, 0, 0, 0, 5}; continue; }
            if (v.teams && r.team <= coop_lobby::kTeamNecro) std::snprintf(names[i], sizeof(names[i]), "%s (%s)", r.name, r.team == coop_lobby::kTeamHuman ? "human" : "necro");
            else std::snprintf(names[i], sizeof(names[i]), "%s", r.name);
            const bool host = (r.flags & coop_lobby::kRowHost) != 0;
            n.name = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(names[i]));
            n.remote = r.machine != v.local;
            n.muted = n.speaking = n.headset = 0;
            n.level = static_cast<int32_t>(coop_mpnative::LevelForXp(r.xp));
            n.signal = coop_lobby::SignalBars(r.pingMs, r.pingMs != 0, host);
        }
        for (unsigned i = 0; i < coop_lobby::kRows; ++i) {
            reinterpret_cast<UpdateItemFn>(base + kUpdateItem)(reinterpret_cast<void*>(s + 0x5C), static_cast<int>(i), false);
            InterlockedIncrement(&updateItems);
        }
        reinterpret_cast<ThisFn>(base + kHelpBar)(reinterpret_cast<void*>(s + 0x160));
        InterlockedIncrement(&rebuilds);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}
int __fastcall OnMp02Tick(void* screen, void*) {
    if (!Armed()) {
        InterlockedIncrement(&mp02Native);
        return mp02TickHook.thiscall<int>(screen);
    }
    InterlockedIncrement(&mp02Ours);
    FillScreen(reinterpret_cast<uintptr_t>(screen));
    return 0;   // eThreadReturnNormal
}

// Trap 3: the stats (FE11) and settings (FE12) tabs reach the BlazeHub. Their
// item state reads "disabled" while armed, which the tab cycling skips (it
// compares these exact pointers), and a tab change away from the lobby is refused.
// The weapon (3) and suit (4) tabs follow the lobby's mode. A joiner's gamemode::mode
// is its own picker's until the launch packet (13.10.: "the host plays game mode 4 (ours
// was 0)" after the lobby showed its tabs shut), so a joiner reads the host's state.
volatile LONG tabDecision = -1, tabsOpened = 0, tabsShut = 0, tabsFromHost = 0;
LONG LobbyMode(bool& fromHost) {
    fromHost = false;
    if (side == Side::Join) {
        const int host = coop_lobby::ModeFromFlags(lobby::hostState.mode);
        if (host >= 0) { fromHost = true; return host; }
    }
    return InterlockedCompareExchange(&gamemode::mode, 0, 0);
}
bool TabsOpen() {
    bool fromHost = false;
    LONG mode = LobbyMode(fromHost);
    // an arena plays only the modes it has points for (Versus picked plays survival there)
    if (coop_lobby::IsArenaCode(mapIndex)) mode = arena::ModeForName(CodeName(mapIndex), static_cast<int>(mode));
    const bool open = loadout::LobbyTabsOpen(mode);
    InterlockedIncrement(open ? &tabsOpened : &tabsShut);
    if (fromHost) InterlockedIncrement(&tabsFromHost);
    const LONG decision = (open ? 0x100 : 0) | (fromHost ? 0x200 : 0) | (mode & 0xFF);
    if (InterlockedExchange(&tabDecision, decision) != decision)
        Line("weapon and suit tabs %s: %s, lobby mode %s from %s (own picker %s)", open ? "OPEN" : "shut", side == Side::Join ? "joiner" : side == Side::Host ? "host" : "-",
             gamemode::Name(mode), fromHost ? "the host's lobby state" : "this machine", gamemode::Name(InterlockedCompareExchange(&gamemode::mode, 0, 0)));
    return open;
}
using ItemStateFn = void(__thiscall*)(void*, uint32_t*, void*, int, uint32_t);
void __fastcall OnMp00ItemState(void* self, void*, uint32_t* dst, void* src, int index, uint32_t defaultState) {
    reinterpret_cast<ItemStateFn>(base + kMp00ItemState)(self, dst, src, index, defaultState);
    if (!Armed() || (index != 3 && index != 4) || !dst || TabsOpen()) return;
    int active = -1;
    __try {
        active = reinterpret_cast<int(__thiscall*)(void*)>(base + kActiveIndex)(reinterpret_cast<char*>(self) + 0x5C);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
    dst[0] = static_cast<uint32_t>(base + (active == index ? kStateActiveDisabled : kStateInactiveDisabled));
    InterlockedIncrement(&tabOverrides);
}
int lastRefusedFrom = -1, lastRefusedTo = -1;
void __fastcall OnChangeTab(void* self, void*, int from, int to) {
    if (Armed() && from == 2 && to != 2 && !((to == 3 || to == 4) && TabsOpen())) {
        InterlockedIncrement(&tabRefusals);
        lastRefusedFrom = from;
        lastRefusedTo = to;
        return;
    }
    changeTabHook.thiscall<void>(self, from, to);
}

// Trap 5: X opens the friends list (Nucleus), Y mutes through a Blaze player.
using InputFn = int(__thiscall*)(void*, int);
using CommandFn = int(__thiscall*)(void*, uint32_t, int, int);
using SelectFn = bool(__thiscall*)(void*, int);
int lastSwallowed = 0;
bool launchButton = true, helpOk = false, helpLogged = false;
DWORD rowClickAt = 0;
const char* launchSource = "";
volatile LONG codeCounts[16]{}, helpBuildsHost = 0, helpBuildsJoin = 0, launchClicks = 0, rowClicks = 0, selectsAfterRowClick = 0, commandsSeen = 0;
uint32_t lastCommandId = 0;
int lastCommandType = -1;
int __fastcall OnMp02Input(void* self, void*, int code) {
    if (Armed() && code >= 0 && code < 16) InterlockedIncrement(&codeCounts[code]);
    if (Armed() && (code == 10 || code == 12)) {
        InterlockedIncrement(&inputSwallowed);
        lastSwallowed = code;
        return 2;
    }
    return reinterpret_cast<InputFn>(base + kMp02Input)(self, code);
}
int __fastcall OnMp02Command(void* self, void*, uint32_t id, int type, int x) {
    if (Armed()) {
        InterlockedIncrement(&commandsSeen);
        lastCommandId = id;
        lastCommandType = type;
    }
    if (Armed() && type == 6 && (id == kCommandInvite || id == kCommandMute)) {
        InterlockedIncrement(&commandsSwallowed);
        return 2;
    }
    if (Armed() && type == 6 && id == kCommandLaunch) {
        InterlockedIncrement(&launchClicks);
        if (side == Side::Host) {
            launchSource = "the LAUNCH button";
            InterlockedIncrement(&startNowRequests);
        }
        return 2;
    }
    if (Armed() && type == 0) {
        rowClickAt = GetTickCount();
        InterlockedIncrement(&rowClicks);
    }
    return reinterpret_cast<CommandFn>(base + kMp02Command)(self, id, type, x);
}
// Select on the list is empty on PC; the host's starts the match now (A, Enter), not
// the select a click on a player row injects.
bool __fastcall OnMp02Select(void* self, void*, int index) {
    InterlockedIncrement(&selects);
    if (Armed() && side == Side::Host) {
        if (rowClickAt && GetTickCount() - rowClickAt < kRowClickSelectMs) {
            InterlockedIncrement(&selectsAfterRowClick);
        } else {
            launchSource = "select";
            InterlockedIncrement(&startNowRequests);
        }
    }
    return reinterpret_cast<SelectFn>(base + kMp02Select)(self, index);
}
// The help bar armed: quit for everybody, LAUNCH for the host. Mute and invite reach Blaze.
using HelpFn = void(__thiscall*)(void*);
using AddButtonFn = void(__thiscall*)(void*, uint32_t, uint32_t, int);
void LabelText(uint32_t hash, char* out, size_t size) {
    out[0] = 0;
    uint32_t text = 0;
    __try {
        text = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(reinterpret_cast<const char*(__cdecl*)(uint32_t)>(base + kLocalizedText)(hash)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
    if (!text || !Read(text, out, size - 1)) { std::snprintf(out, size, "(none)"); return; }
    out[size - 1] = 0;
    for (char* c = out; *c; ++c) if (static_cast<unsigned char>(*c) < 0x20 || static_cast<unsigned char>(*c) > 0x7E) { *c = 0; break; }
}
void __fastcall OnMp02HelpBar(void* bar, void*) {
    if (!Armed()) {
        reinterpret_cast<HelpFn>(base + kMp02Help)(bar);
        return;
    }
    const bool host = side == Side::Host;
    bool ok = false;
    __try {
        reinterpret_cast<AddButtonFn>(base + kAddHelpButton)(bar, kCommandBack, kLabelQuit, 11);
        if (host && launchButton) reinterpret_cast<AddButtonFn>(base + kAddHelpButton)(bar, kCommandLaunch, kLabelLaunch, 13);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
    InterlockedIncrement(host ? &helpBuildsHost : &helpBuildsJoin);
    if (helpLogged) return;
    helpLogged = true;
    char label[32];
    LabelText(kLabelLaunch, label, sizeof(label));
    const uint32_t b = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(bar));
    Line("MP02 help bar built (%s) on %08X, enabled %u: quit%s%s; label %08X reads \"%s\"", host ? "host" : "joiner", b, Ptr(b + 0x30) & 0xFF,
         host && launchButton ? " + LAUNCH" : "", ok ? "" : " (FAULTED)", kLabelLaunch, label);
}

// Trap 4: Yes on the quit popup leaves a Blaze game (nothing offline); armed,
// the game tick leaves our lobby and pops back to the Multiplayer screen.
using QuitFn = void(__thiscall*)(void*, const void*);
void __fastcall OnQuitResult(void* self, void*, const unsigned char* result) {
    if (!Armed()) {
        reinterpret_cast<QuitFn>(base + kQuitResult)(self, result);
        return;
    }
    unsigned char flag = 1;
    uint32_t value = 1;
    Read(reinterpret_cast<uintptr_t>(result) + 4, &flag, 1);
    Read(reinterpret_cast<uintptr_t>(result) + 8, &value, 4);
    const bool yes = !flag && !value;
    InterlockedIncrement(yes ? &quitYes : &quitNo);
    if (yes) InterlockedExchange(&leaveRequested, 1);
}

// --- arming -----------------------------------------------------------------------------------
const char* Refusal() {
    if (!installed) return "not installed";
    const uint32_t services = Ptr(base + kOnlineServices);
    if (!services) return "no OnlineServices";
    if (Ptr(services + 0x94)) return "a BlazeHub exists (a real online session changes every answer)";
    if (!Ptr(services + 0x24)) return "no IGameManager at OnlineServices+0x24";
    if (!Ptr(base + kLobbyManager)) return "no MPLobbyManager";
    if (!Ptr(base + kFrontendBlock)) return "no frontend screen block";
    const uint32_t director = Ptr(base + menu::kDirector);
    if (!director) return "no Apt3dDirector";
    if (Ptr(director + 0x43C)) return "the director is mid-transition";
    const uint32_t frontend = Ptr(base + kFrontendManager);
    if (frontend && Ptr(frontend + 0x3C) == 5) return "the frontend is starting a connection (state 5)";
    return nullptr;
}
uint32_t CurrentScreen() {
    const uint32_t director = Ptr(base + menu::kDirector);
    if (!director) return 0;
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*)>(base + kCurrentScreen)(reinterpret_cast<void*>(static_cast<uintptr_t>(director)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return 0;
    }
}
// MP02 OnShow finds the map by the game manager's stream list: the entry's own
// text, compared byte for byte.
__declspec(noinline) bool SetStreamList(uint32_t gm, uint32_t text) {
    const uint32_t vtable = Ptr(gm);
    if (!vtable || Ptr(vtable + kSetStreamListSlot) != base + kSetStreamList) return false;
    __try {
        reinterpret_cast<void(__thiscall*)(void*, const char*)>(base + kSetStreamList)(reinterpret_cast<void*>(static_cast<uintptr_t>(gm)),
                                                                                        reinterpret_cast<const char*>(static_cast<uintptr_t>(text)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}
void ReadText(uint32_t address, char* out, size_t size) {
    std::memset(out, 0, size);
    if (address) Read(address, out, size - 1);
    out[size - 1] = 0;
}
// The map list entry of a kMapTable map or an arena code (a joiner never opened the
// picker, so the extra maps and the arenas are appended first).
uint32_t EntryFor(int index) {
    const int arenaIndex = coop_lobby::ArenaIndexOf(index);
    menu::AppendExtraMaps();
    const uint32_t dgm = Ptr(base + menu::kGameManager);
    const uint32_t maps = dgm ? Ptr(dgm + menu::kDgmMaps) : 0, count = dgm ? Ptr(dgm + menu::kDgmMapCount) : 0;
    if (!maps || !count || count > coop_mpmap::kMaxMaps) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        char name[64]{};
        if (!menu::EntryMap(maps + i * menu::kMapEntrySize, name, sizeof(name))) continue;
        if (arenaIndex >= 0 ? coop_arena::ArenaIndex(name) == arenaIndex : coop_mpmap::MapIndex(name) == index) return maps + i * menu::kMapEntrySize;
    }
    return 0;
}

bool Open(Side who, int index, uint32_t entry, DWORD now) {
    if (const char* why = Refusal()) {
        InterlockedIncrement(&openRefusals);
        Line("%s open REFUSED: %s", who == Side::Host ? "host" : "joiner", why);
        return false;
    }
    const uint32_t services = Ptr(base + kOnlineServices), gm = Ptr(services + 0x24), lobbyManager = Ptr(base + kLobbyManager);
    const uint32_t block = Ptr(base + kFrontendBlock), director = Ptr(base + menu::kDirector), frontend = Ptr(base + kFrontendManager);
    unsigned char endOfMatch = 0xFF;
    Read(base + kEndOfMatch, &endOfMatch, 1);
    const unsigned char zero = 0;
    WriteBytes(base + kEndOfMatch, &zero, 1);   // pre-game: MP02 instead of the MP19 results, tabs 0/1 off
    const uint32_t text = entry ? Ptr(entry + 4) : 0;
    char entryText[64], readBack[64];
    ReadText(text, entryText, sizeof(entryText));
    const bool listed = text && entryText[0] && SetStreamList(gm, text);
    ReadText(Ptr(gm + 0x4C), readBack, sizeof(readBack));
    side = who;
    mapIndex = index;
    std::snprintf(mapName, sizeof(mapName), "%s", CodeName(index));
    countdown = -1;
    countdownAt = now;
    startNow = false;
    helpLogged = false;
    rowClickAt = 0;
    watched = false;
    hintShown = false;
    launchCheck = false;
    InterlockedExchange(&startNowRequests, 0);
    InterlockedExchange(&leaveRequested, 0);
    launchesAtArm = InterlockedCompareExchange(&lobby::joinLaunches, 0, 0);
    armedAt = now;
    shownVersion = ~0u;
    InterlockedExchange(&tabDecision, -1);
    InterlockedExchange(&armed, 1);
    const uint32_t screenBefore = CurrentScreen();
    const bool pushed = menu::PushScreen(kMp00Hash);
    Line("%s opens the lobby for %s (map byte %02X): entry %08X '%s' stream list %s -> '%s'; end-of-match %02X -> 0; services %08X gm %08X stats %08X voip %08X hub %08X; "
         "lobby %08X (+0x10 %u +0x18 %d +0x1C %d); block %08X MP00 %08X MP02 %08X; director %08X screen %08X frontend state %u target %u; push %s",
         who == Side::Host ? "host" : "joiner", mapName, index, entry, entryText, listed ? "set" : "NOT set", readBack, endOfMatch, services, gm, Ptr(services + 0x34),
         Ptr(services + 0x3C), Ptr(services + 0x94), lobbyManager, Ptr(lobbyManager + 0x10) & 0xFF, static_cast<int>(Ptr(lobbyManager + 0x18)),
         static_cast<int>(Ptr(lobbyManager + 0x1C)), block, block + kMp00Offset, block + kMp02Offset, director, screenBefore, frontend ? Ptr(frontend + 0x3C) : 0,
         frontend ? Ptr(frontend + 0x92C) : 0, pushed ? "ok" : "FAILED");
    if (!pushed) {
        InterlockedExchange(&armed, 0);
        side = Side::None;
        InterlockedIncrement(&pushFailures);
        return false;
    }
    InterlockedIncrement(&opens);
    return true;
}

// From the map picker's select (runtime_coop_menu.inc): a co-op host goes to the lobby screen.
bool HostWants() { return installed && lobby::role == lobby::Role::Host && lobby::mode == lobby::Mode::Survival && !Armed(); }
bool OpenFromPicker(uint32_t entry, const char* map) {
    if (coop_arena::IsArenaName(map)) {
        const int arenaIndex = coop_arena::ArenaIndex(map);
        // the joiners only follow into an arena with [Arena] Modes
        if (arenaIndex < 0 || !arena::modesOn) { Line("map picker: arena %s %s; direct launch", map, arenaIndex < 0 ? "is not in the table" : "without [Arena] Modes"); return false; }
        return Open(Side::Host, coop_lobby::ArenaCode(static_cast<unsigned>(arenaIndex)), entry, GetTickCount());
    }
    const int index = coop_mpmap::MapIndex(map);
    if (index < 0) { Line("map picker: %s is no kMapTable map; direct launch", map); return false; }
    if (const uint8_t lacking = lobby::MissingFor(index)) { Line("map picker: machine %u cannot load %s; the direct route refuses it", lacking, map); return false; }
    return Open(Side::Host, index, entry, GetTickCount());
}

void StopNativeLobby() {
    const uint32_t lobbyManager = Ptr(base + kLobbyManager);
    if (!lobbyManager) return;
    __try {
        reinterpret_cast<ThisFn>(base + kStopLobby)(reinterpret_cast<void*>(static_cast<uintptr_t>(lobbyManager)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}
void Disarm(const char* why) {
    if (!Armed()) return;
    InterlockedExchange(&armed, 0);
    Line("disarmed (%s) after %lu ms; countdown was %d", why, static_cast<unsigned long>(GetTickCount() - armedAt), countdown);
    side = Side::None;
    countdown = -1;
}
void RequestPop(DWORD now) {
    popPending = true;
    popSince = now;
}
// Back to the Multiplayer screen, as its own cancel does; MP00 OnHide stops the native lobby.
void TryPop(DWORD now) {
    const uint32_t director = Ptr(base + menu::kDirector);
    if (!director || Ptr(director + 0x43C)) {
        if (now - popSince > 5000) { popPending = false; InterlockedIncrement(&popFailures); Line("pop to the Multiplayer screen given up: director %08X busy", director); }
        return;
    }
    popPending = false;
    const uint32_t before = CurrentScreen();
    if (before != kMp00Hash) { Line("pop skipped: the current screen is %08X, not the lobby", before); return; }
    uint32_t screen = 0;
    __try {
        screen = reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t)>(base + kPopToScreen)(reinterpret_cast<void*>(static_cast<uintptr_t>(director)), menu::kFe03Hash);
        if (screen) reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kStartTransition)(reinterpret_cast<void*>(static_cast<uintptr_t>(director)), screen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
    InterlockedIncrement(screen ? &pops : &popFailures);
    Line("pop to the Multiplayer screen: %s (screen %08X)", screen ? "transition started" : "FAILED", screen);
}

void Leave(DWORD now) {
    if (!Armed()) return;
    const Side was = side;
    Disarm(was == Side::Host ? "the host left the lobby screen" : "the joiner left the lobby");
    // a host keeps its lobby open (its joiners go back to waiting); a joiner says goodbye
    if (was == Side::Join) lobby::Stop();
    StopNativeLobby();
    RequestPop(now);
}

// --- the game tick ----------------------------------------------------------------------------
void WriteCountdown() {
    const uint32_t lobbyManager = Ptr(base + kLobbyManager);
    if (lobbyManager) WriteBytes(lobbyManager + 0x18, &countdown, 4);
}
void Watch(DWORD now) {
    if (watched || now - armedAt < 1500) return;
    watched = true;
    const uint32_t block = Ptr(base + kFrontendBlock), lobbyManager = Ptr(base + kLobbyManager);
    int active = -2;
    __try {
        if (block) active = reinterpret_cast<int(__thiscall*)(void*)>(base + kActiveIndex)(reinterpret_cast<void*>(static_cast<uintptr_t>(block + kMp00Offset + 0x5C)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
    char nextMap[64];
    ReadText(block ? block + kMp02Offset + 0x314 : 0, nextMap, sizeof(nextMap));
    Line("1.5 s after the open: screen %08X, MP00 tab %d (expect 2), popup %d, MP02 map index %d, '%s'; lobby +0x10 %u +0x11 %u +0x18 %d; end-of-match %u; "
         "OnShow forced %ld native %ld, MP00 tick forced %ld, MP02 ticks ours %ld native %ld, rebuilds %ld",
         CurrentScreen(), active, block ? static_cast<int>(Ptr(block + kMp00Offset + 0x248)) : -2, block ? static_cast<int>(Ptr(block + kMp02Offset + 0x240)) : -2, nextMap,
         Ptr(lobbyManager + 0x10) & 0xFF, Ptr(lobbyManager + 0x11) & 0xFF, static_cast<int>(Ptr(lobbyManager + 0x18)), Ptr(base + kEndOfMatch) & 0xFF,
         InterlockedCompareExchange(&showForced, 0, 0), InterlockedCompareExchange(&showNative, 0, 0), InterlockedCompareExchange(&mp00TickForced, 0, 0),
         InterlockedCompareExchange(&mp02Ours, 0, 0), InterlockedCompareExchange(&mp02Native, 0, 0), InterlockedCompareExchange(&rebuilds, 0, 0));
}

void HostTick(DWORD now) {
    const bool teams = mpnative::InVersus();
    coop_lobby::StatePacket& s = lobby::stateOut;
    coop_lobby::FillRows(s, lobby::table, lobby::OwnProfile(0), teams, mpnative::humansWanted);
    const bool mine = Armed() && side == Side::Host;
    s.map = mine ? static_cast<uint8_t>(mapIndex) : coop_lobby::kNoMap;
    s.mode = lobby::LaunchFlags();
    s.countdownMs = mine ? countdown : -1;
    lobby::stateOutReady = true;
    if (!mine) return;
    SetView(s.rows, s.count, 0, teams, now);
    if (InterlockedExchange(&startNowRequests, 0) && !startNow) {
        startNow = true;
        Line("the host launched (%s): start in %d ms, countdown was %d", launchSource, startNowMs, countdown);
        if (countdown < 0 || countdown > startNowMs) { countdown = startNowMs; countdownAt = now; }
    }
    const int players = s.count;
    if (countdown < 0) {
        if (startNow || (countdownMs > 0 && players >= minPlayers)) {
            countdown = startNow ? startNowMs : countdownMs;
            countdownAt = now;
            Line("countdown %d ms: %d player(s), %d needed%s", countdown, players, minPlayers, startNow ? ", started by the host" : "");
        } else if (!hintShown) {
            hintShown = true;
            notice::Enqueue(countdownMs > 0 ? "WAITING FOR PLAYERS - LAUNCH OR SELECT STARTS NOW" : "WAITING FOR PLAYERS - LAUNCH OR SELECT STARTS THE MATCH");
        }
        return;
    }
    if (players < minPlayers && !startNow) {
        Line("countdown stopped at %d ms: %d player(s), %d needed", countdown, players, minPlayers);
        countdown = -1;
        return;
    }
    countdown -= static_cast<int32_t>(now - countdownAt);
    countdownAt = now;
    if (countdown > 0) return;
    countdown = 0;
    WriteCountdown();
    StopNativeLobby();
    Disarm("the countdown ran out");
    menuLaunchesBefore = InterlockedCompareExchange(&menu::launches, 0, 0);
    menuRefusedBefore = InterlockedCompareExchange(&menu::refused, 0, 0);
    std::snprintf(menu::pendingMap, sizeof(menu::pendingMap), "%s", mapName);
    menu::pendingAt = now;
    InterlockedExchange(&menu::launchPending, 1);
    launchCheck = true;
    InterlockedIncrement(&launches);
    Line("launching %s for %d player(s) through the map picker's route", mapName, players);
}
// A launch the menu refused (a joiner lacks the map) leaves the host on the lobby screen: wait again.
void CheckLaunch(DWORD now) {
    if (!launchCheck || InterlockedCompareExchange(&menu::launchPending, 0, 0)) return;
    launchCheck = false;
    if (InterlockedCompareExchange(&menu::launches, 0, 0) != menuLaunchesBefore) return;
    if (InterlockedCompareExchange(&menu::refused, 0, 0) == menuRefusedBefore || CurrentScreen() != kMp00Hash) return;
    InterlockedIncrement(&relaunchWaits);
    side = Side::Host;
    countdown = -1;
    startNow = false;
    helpLogged = false;
    rowClickAt = 0;
    armedAt = now;
    InterlockedExchange(&armed, 1);
    Line("the launch was refused; back to waiting on the lobby screen");
}

void JoinTick(DWORD now) {
    const coop_lobby::StatePacket& st = lobby::hostState;
    const bool fresh = lobby::hostStateAt && now - lobby::hostStateAt < 3000;
    const bool welcomed = lobby::role == lobby::Role::Join && lobby::joiner.state == coop_lobby::Joiner::State::Welcomed;
    if (!Armed()) {
        if (!welcomed || !fresh || st.map == coop_lobby::kNoMap || popPending || now - lastOpenTry < 1000) return;
        lastOpenTry = now;
        const uint32_t screen = CurrentScreen();
        if (screen != menu::kFe03Hash && screen != kMp20Hash) return;
        const int arenaIndex = coop_lobby::ArenaIndexOf(st.map);
        const bool loadable = arenaIndex >= 0 ? arenaIndex < static_cast<int>(coop_arena::kArenaCount) && arena::enabled
                                              : st.map < coop_mpmap::kMapCount && mpmap::MapAvailable(st.map);
        if (!loadable) {
            static int refusedMap = -1;
            if (refusedMap != st.map) {
                refusedMap = st.map;
                Line("the host's lobby map byte %02X (%s) is not loadable here%s; staying on the Multiplayer screen", st.map, CodeName(st.map),
                     arenaIndex >= 0 ? " (arena unknown or [Arena] Arenas=0)" : "");
            }
            return;
        }
        Open(Side::Join, st.map, EntryFor(st.map), now);
        return;
    }
    if (side != Side::Join) return;
    if (InterlockedCompareExchange(&lobby::joinLaunches, 0, 0) != launchesAtArm) {
        StopNativeLobby();
        Disarm("the host launched");
        return;
    }
    const char* why = lobby::role != lobby::Role::Join ? (lobby::joiner.refusal ? lobby::joiner.refusal : "the lobby closed")
                      : !fresh                        ? "the host went quiet"
                      : st.map == coop_lobby::kNoMap   ? "the host left the lobby screen"
                      : st.map != mapIndex             ? "the host picked another map"
                                                       : nullptr;
    if (why) {
        Disarm(why);
        StopNativeLobby();
        RequestPop(now);
        return;
    }
    const int hostMode = coop_lobby::ModeFromFlags(st.mode);
    SetView(st.rows, st.count, lobby::joiner.machine, hostMode == gamemode::Versus, now);
    if (st.countdownMs < 0) countdown = -1;
    else {
        const int32_t left = st.countdownMs - static_cast<int32_t>(now - lobby::hostStateAt);
        countdown = left > 0 ? left : 0;
    }
}

// Game thread, from Poll, between the lobby's packets and the menu's launch.
void Tick(DWORD now) {
    if (!installed) return;
    if (InterlockedExchange(&leaveRequested, 0)) Leave(now);
    if (popPending) TryPop(now);
    CheckLaunch(now);   // before the list is built, so the joiners never see a gap
    if (lobby::role == lobby::Role::Host && lobby::mode == lobby::Mode::Survival) HostTick(now);
    else lobby::stateOutReady = false;
    if (lobby::role == lobby::Role::Join || side == Side::Join) JoinTick(now);
    if (!Armed()) return;
    WriteCountdown();
    Watch(now);
}

// --- install ----------------------------------------------------------------------------------
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("NativeLobby", "Enabled", 0, ini.c_str()) != 0;
    minPlayers = GetPrivateProfileIntA("NativeLobby", "MinPlayers", 2, ini.c_str());
    if (minPlayers < 1) minPlayers = 1;
    if (minPlayers > 8) minPlayers = 8;
    // 0: the countdown waits for the host's launch; else it starts once MinPlayers are in
    int seconds = GetPrivateProfileIntA("NativeLobby", "AutoStartSeconds", 0, ini.c_str());
    if (seconds < 0) seconds = 0;
    if (seconds > 0 && seconds < 3) seconds = 3;
    if (seconds > 5999) seconds = 5999;
    countdownMs = seconds * 1000;
    int launch = GetPrivateProfileIntA("NativeLobby", "LaunchSeconds", 3, ini.c_str());
    if (launch < 1) launch = 1;
    if (launch > 10) launch = 10;
    startNowMs = launch * 1000;
    launchButton = GetPrivateProfileIntA("NativeLobby", "LaunchButton", 1, ini.c_str()) != 0;
    if (!enabled) { std::fprintf(log, "native lobby off ([NativeLobby] Enabled=0)\n"); return; }
    if (!menu::installed) { std::fprintf(log, "native lobby off: needs the menu ([Menu] Enabled=1)\n"); return; }
    bool sigsOk = true;
    for (const Sig& s : kSigs) {
        unsigned char want[32]{}, have[32]{};
        const size_t n = Hex(s.hex, want, sizeof(want));
        const bool ok = n && Read(base + s.rva, have, n) && std::memcmp(have, want, n) == 0;
        if (!ok) sigsOk = false;
        std::fprintf(log, "native lobby sig %-44s %08X %s", s.name, static_cast<unsigned>(base + s.rva), ok ? "ok\n" : "MISMATCH:");
        if (!ok) {
            for (size_t i = 0; i < n; ++i) std::fprintf(log, " %02X", have[i]);
            std::fprintf(log, "\n");
        }
    }
    if (!sigsOk) { std::fprintf(log, "native lobby off: a function did not match\n"); return; }
    unsigned char showBytes[10]{}, tickBytes[12]{}, mp02Bytes[16]{}, tabBytes[16]{};
    Hex("84 C0 74 5B 8B 0D E8 13 02 02", showBytes, sizeof(showBytes));
    Hex("33 ED 39 6C 24 0C 0F 84 FC 00 00 00", tickBytes, sizeof(tickBytes));
    Hex("83 EC 24 53 55 8B E9 80 BD 88 00 00 00 00 56 57", mp02Bytes, sizeof(mp02Bytes));
    Hex("83 EC 0C 56 8B 74 24 14 57 8B 7C 24 1C 3B F7 74", tabBytes, sizeof(tabBytes));
    bool ok = hook_site::InstallMid(log, "native lobby MP00 OnShow branch", base + kMp00OnShowSite, showBytes, sizeof(showBytes), OnMp00Show, showHook);
    ok = hook_site::InstallMid(log, "native lobby MP00 Tick game test", base + kMp00TickSite, tickBytes, sizeof(tickBytes), OnMp00Tick, mp00TickHook) && ok;
    ok = hook_site::Install(log, "native lobby MP02 Tick", base + kMp02Tick, mp02Bytes, sizeof(mp02Bytes), reinterpret_cast<void*>(OnMp02Tick), mp02TickHook) && ok;
    ok = hook_site::Install(log, "native lobby MP00 ConditionalChangeTab", base + kChangeTab, tabBytes, sizeof(tabBytes), reinterpret_cast<void*>(OnChangeTab), changeTabHook) && ok;
    ok = menu::SwapSlot(log, "MP00 DoUpdateItemAndLabel", kMp00ItemStateSlot, kMp00ItemState, reinterpret_cast<void*>(OnMp00ItemState)) && ok;
    ok = menu::SwapSlot(log, "MP02 HandleInput", kMp02InputSlot, kMp02Input, reinterpret_cast<void*>(OnMp02Input)) && ok;
    ok = menu::SwapSlot(log, "MP02 FSCommand", kMp02CommandSlot, kMp02Command, reinterpret_cast<void*>(OnMp02Command)) && ok;
    ok = menu::SwapSlot(log, "MP02 OnInput_Select", kMp02SelectSlot, kMp02Select, reinterpret_cast<void*>(OnMp02Select)) && ok;
    ok = menu::SwapSlot(log, "MP00 quit popup result", kQuitResultSlot, kQuitResult, reinterpret_cast<void*>(OnQuitResult)) && ok;
    installed = ok;
    // the help bar: its own signatures; without them the lobby works with select only
    const Sig help[] = {{"MP02 help bar buttons", kMp02Help, "56 6A 0B 68 9C CD 3F F1 68 6B 63 61 62 8B F1 E8"},
                        {"help bar AddButton", kAddHelpButton, "83 EC 08 8B 44 24 0C 88 44 24 03 C1 E8 08 88 44"},
                        {"GetLocalizedTextStatic", kLocalizedText, "8B 44 24 04 8B 0D C8 13 02 02 50 E8"}};
    bool helpSigs = true;
    for (const Sig& s : help) {
        unsigned char want[32]{}, have[32]{};
        const size_t n = Hex(s.hex, want, sizeof(want));
        const bool same = n && Read(base + s.rva, have, n) && std::memcmp(have, want, n) == 0;
        if (!same) helpSigs = false;
        std::fprintf(log, "native lobby sig %-44s %08X %s\n", s.name, static_cast<unsigned>(base + s.rva), same ? "ok" : "MISMATCH: help bar left alone");
    }
    helpOk = installed && helpSigs && menu::SwapSlot(log, "MP02 help bar buttons", kMp02HelpSlot, kMp02Help, reinterpret_cast<void*>(OnMp02HelpBar));
    std::fprintf(log, "native lobby %s: start %s, launch countdown %d s, LAUNCH button %s, team rows %s\n", installed ? "installed" : "NOT installed (the hooks stay inert)",
                 countdownMs > 0 ? "by itself" : "by the host only", startNowMs / 1000,
                 !launchButton ? "off ([NativeLobby] LaunchButton=0)" : helpOk ? "on (host)" : "NOT installed (select only)", mpnative::enabled ? "in Versus" : "off");
    if (countdownMs > 0) std::fprintf(log, "native lobby auto start: %d s once %d player(s) are in\n", countdownMs / 1000, minPlayers);
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    static LONG last = -1;
    const LONG sum = InterlockedCompareExchange(&opens, 0, 0) * 7 + InterlockedCompareExchange(&openRefusals, 0, 0) * 11 + InterlockedCompareExchange(&rebuilds, 0, 0) +
                     InterlockedCompareExchange(&tabRefusals, 0, 0) * 13 + InterlockedCompareExchange(&inputSwallowed, 0, 0) * 17 +
                     InterlockedCompareExchange(&quitYes, 0, 0) * 19 + InterlockedCompareExchange(&quitNo, 0, 0) * 23 + InterlockedCompareExchange(&launches, 0, 0) * 29 +
                     InterlockedCompareExchange(&faults, 0, 0) * 31 + InterlockedCompareExchange(&pops, 0, 0) * 37 + InterlockedCompareExchange(&selects, 0, 0) * 41 +
                     InterlockedCompareExchange(&commandsSeen, 0, 0) * 43 + InterlockedCompareExchange(&helpBuildsHost, 0, 0) * 47 +
                     InterlockedCompareExchange(&helpBuildsJoin, 0, 0) * 53;
    LONG codesSum = 0;
    for (int i = 0; i < 16; ++i) codesSum += (i + 1) * InterlockedCompareExchange(&codeCounts[i], 0, 0);
    if (sum + codesSum == last) return;
    last = sum + codesSum;
    std::fprintf(log, "native lobby launch: help bar builds host=%ld joiner=%ld, LAUNCH clicks=%ld, row clicks=%ld, selects after a row click=%ld (ignored), "
                      "commands=%ld (last %08X type %d); input codes",
                 InterlockedCompareExchange(&helpBuildsHost, 0, 0), InterlockedCompareExchange(&helpBuildsJoin, 0, 0), InterlockedCompareExchange(&launchClicks, 0, 0),
                 InterlockedCompareExchange(&rowClicks, 0, 0), InterlockedCompareExchange(&selectsAfterRowClick, 0, 0), InterlockedCompareExchange(&commandsSeen, 0, 0),
                 lastCommandId, lastCommandType);
    for (int code = 0; code < 16; ++code) {
        const LONG n = InterlockedCompareExchange(&codeCounts[code], 0, 0);
        if (n) std::fprintf(log, " %d:%ld", code, n);
    }
    std::fprintf(log, "\n");
    std::fprintf(log, "native lobby armed=%d side=%s map=%s countdown=%d opens=%ld refused=%ld push_failures=%ld launches=%ld relaunch_waits=%ld pops=%ld pop_failures=%ld faults=%ld\n",
                 Armed() ? 1 : 0, side == Side::Host ? "host" : side == Side::Join ? "join" : "-", mapIndex >= 0 ? mapName : "-", countdown,
                 InterlockedCompareExchange(&opens, 0, 0), InterlockedCompareExchange(&openRefusals, 0, 0), InterlockedCompareExchange(&pushFailures, 0, 0),
                 InterlockedCompareExchange(&launches, 0, 0), InterlockedCompareExchange(&relaunchWaits, 0, 0), InterlockedCompareExchange(&pops, 0, 0),
                 InterlockedCompareExchange(&popFailures, 0, 0), InterlockedCompareExchange(&faults, 0, 0));
    std::fprintf(log, "native lobby screens OnShow forced=%ld native=%ld MP00 tick forced=%ld MP02 ticks ours=%ld native=%ld rebuilds=%ld update_items=%ld bad_rows=%ld\n",
                 InterlockedCompareExchange(&showForced, 0, 0), InterlockedCompareExchange(&showNative, 0, 0), InterlockedCompareExchange(&mp00TickForced, 0, 0),
                 InterlockedCompareExchange(&mp02Ours, 0, 0), InterlockedCompareExchange(&mp02Native, 0, 0), InterlockedCompareExchange(&rebuilds, 0, 0),
                 InterlockedCompareExchange(&updateItems, 0, 0), InterlockedCompareExchange(&badRows, 0, 0));
    std::fprintf(log, "native lobby input tab_overrides=%ld tab_refusals=%ld (last %d->%d) tabs open=%ld shut=%ld host_mode=%ld swallowed=%ld (last code %d) "
                      "commands_swallowed=%ld selects=%ld quit yes=%ld no=%ld\n",
                 InterlockedCompareExchange(&tabOverrides, 0, 0), InterlockedCompareExchange(&tabRefusals, 0, 0), lastRefusedFrom, lastRefusedTo,
                 InterlockedCompareExchange(&tabsOpened, 0, 0), InterlockedCompareExchange(&tabsShut, 0, 0), InterlockedCompareExchange(&tabsFromHost, 0, 0),
                 InterlockedCompareExchange(&inputSwallowed, 0, 0), lastSwallowed, InterlockedCompareExchange(&commandsSwallowed, 0, 0),
                 InterlockedCompareExchange(&selects, 0, 0), InterlockedCompareExchange(&quitYes, 0, 0), InterlockedCompareExchange(&quitNo, 0, 0));
    AcquireSRWLockShared(&viewLock);
    const View v = view;
    ReleaseSRWLockShared(&viewLock);
    if (!v.count) return;
    std::fprintf(log, "native lobby rows (version %u, local machine %u):", v.version, v.local);
    for (unsigned i = 0; i < coop_lobby::kRows; ++i)
        if (v.rows[i].flags & coop_lobby::kRowUsed)
            std::fprintf(log, " [%u] m%u '%s' team %u xp %u ping %u%s", i, v.rows[i].machine, v.rows[i].name, v.rows[i].team, v.rows[i].xp, v.rows[i].pingMs,
                         v.rows[i].flags & coop_lobby::kRowHost ? " host" : "");
    std::fprintf(log, "\n");
}
}  // namespace nativelobby
