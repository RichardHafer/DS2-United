// Dead Space 2 United by Richard Hafer.
// Survival in the main menu ([Menu] in DS2United.ini).
//
// The PC main menu (PC01) shows "survival" in place of "multiplayer" and opens
// the game's own Multiplayer screen (FE03) without the EA login; its first item
// opens the native map picker (MP20), whose Select starts the chosen MP map in
// single player through [MpMap]. Only three vtable slots and six label pointers
// change, no code bytes. Evidence: analysis/DS2_MAIN_MENU_SURVIVAL_2026-10-09.md.
//
// Never reached from here: the login (UIFrontendManager::Transition(3)),
// matchmaking (0x00A5A4E0), playgroups (0x00AA45F0) and the lobby, leaderboard,
// friends and character screens. All of them need Blaze or the EA login.
#include "united_version.h"
namespace nativelobby { bool HostWants(); bool OpenFromPicker(uint32_t entry, const char* map); }   // runtime_coop_nativelobby.inc
namespace loadout { bool OpenFromMenu(); }   // runtime_coop_loadout.inc: the character setup on item 4
namespace gamemode_nav { void Sound(); }   // runtime_coop_gamemode.inc: the menu navigation sound
namespace menu {
bool enabled = false, installed = false;
// RVAs (image base 0x400000)
constexpr uint32_t kPc01SelectSlot = 0x17392B0, kFe03SelectSlot = 0x1739540, kMp20SelectSlot = 0x173A368, kSp01SelectSlot = 0x1738198;
constexpr uint32_t kSp01Select = 0x6ABE20, kSp01Labels = 0x1B600FC, kSp01Descriptions = 0x1B60110;
constexpr uint32_t kSp01PreviouslyLabel = 0x172D144, kSp01PreviouslyDesc = 0x172D0C4, kPc01SingleLabel = 0x172D738;
// LCH2 text ids (SDBM of the lowercased id): "single player" and the screen title "//single player"
constexpr uint32_t kTextSinglePlayer = 0x3F9A2A69, kTextSinglePlayerTitle = 0x6E1336CC;
// "$ui_gamemode_mc", the label of the map picker's mode row (Mark's movie): "Game Mode"
// in capitals among the menu's lower-case labels
constexpr uint32_t kTextGameMode = 0xA72AD0B5;
const char kGameModeLabel[] = "game mode";
// The two lines at the bottom of the Multiplayer screens (FE33NatTypeWarning). Without
// EA's servers the game always says "Your NAT Type: Strict"; they say ours instead
// ([Menu] NatLine1, NatLine2; empty keeps the game's). Hashes read from the text table
// in dump 7320 (13.10.).
constexpr uint32_t kTextNatType = 0xFCE1D1EF, kTextNatSupport = 0x6F0A5BC4;
char natLine1[96] = "", natLine2[96] = "";
// The title screen's legal line (FE01, ui_fe01_legaltext_mc: "(c) 2011 Electronic Arts
// Inc.", read live 13.10.) keeps EA's text; [Menu] TitleLine goes after it.
constexpr uint32_t kTextLegal = 0x85247E23;
char titleLine[96] = "";
constexpr char kTitleLine[] = "Dead Space 2 United - Fan made mod";
constexpr uint32_t kPc01Select = 0x6A1C30, kFe03Select = 0x6A9DD0, kMp20Select = 0x667B50, kPushScreen = 0x6879D0;
constexpr uint32_t kFe03InputSlot = 0x17394E0, kFe03Input = 0x6AF9F0;   // FE03 HandleInput, thiscall(code) ret 4
constexpr uint32_t kDirector = 0x1C8BE90, kGameManager = 0x1C213FC;
constexpr uint32_t kPc01Labels = 0x1B5FFCC, kFe03Labels = 0x1B5FFEC;
constexpr uint32_t kPc01MultiLabel = 0x172D720, kFe03FirstLabel = 0x172D680;   // "$UIMenu_Multi_Player_mc", "$UIMenu_Quick_Match_mc"
constexpr uint32_t kFe03Hash = 0xBE64F23A;
constexpr uint32_t kMp20Index = 0x22C, kMp20Any = 0x229, kDgmMaps = 0x22C, kDgmMapCount = 0x230, kMapEntrySize = 0x54;

// Literal labels: the formatter (0x01371AA0) shows text without '$', '#' or '{' as written.
// 14.10.: the main menu says "gamemodes" and "story"; the map picker's mode row still says survival.
const char kSurvivalLabel[] = "gamemodes";
const char kCoopLabel[] = "story", kCoopTitle[] = "//story";
const char kJoinDescription[] = "Join a friend's game: you load in at the host's next checkpoint restart";
char joinDescription[200] = "";
// Screen titles (LCH2 ids read from deadspace2_global.en.lh2): "/main menu" and the
// Multiplayer screen's "//multiplayer".
constexpr uint32_t kTextMainMenuTitle = 0xD701A383, kTextMultiplayerTitle = 0xDCEF509F;
const char kMainMenuTitle[] = "/United", kModesTitle[] = "//gamemodes";
constexpr uint32_t kFormatLookupCall = 0xF71A1C, kLocaleLookup = 0xF70B60;
volatile LONG titleLookups = 0;
// The release notice, once per game start: when the main menu first shows its title, or at
// the start when no main menu hook can tell.
volatile LONG releaseNoticeShown = 0;
void ReleaseNotice(DWORD ms) {
    if (InterlockedExchange(&releaseNoticeShown, 1)) return;
    overlay::ShowNotice("DEAD SPACE 2 UNITED " DS2UNITED_VERSION ": A HIGHLY EXPERIMENTAL RELEASE",
                        "Expect crashes and bugs. Back up your saves before you play.",
                        "F1 shows the keys. Problems: send the logs from the plugins folder to the Dead Space Modding Discord.", ms);
}
const char* __fastcall FormatMenuText(void* locale, void*, uint32_t hash) {
    if (hash == kTextMainMenuTitle) {
        InterlockedIncrement(&titleLookups);
        ReleaseNotice(12000);
        return kMainMenuTitle;
    }
    return reinterpret_cast<const char*(__thiscall*)(void*, uint32_t)>(base + kLocaleLookup)(locale, hash);
}
bool InstallTitleLookup() {
    // The formatter copies the result before the periodic text overrides can run.
    const unsigned char expected[] = {0x50,0x8B,0xCF,0xE8,0x3F,0xF1,0xFF,0xFF,0x3B,0xC6};
    unsigned char actual[sizeof(expected)]{};
    const uintptr_t call = base + kFormatLookupCall;
    if (!Read(call - 3, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected))) return false;
    const uint32_t relative = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&FormatMenuText) - (call + 5));
    if (!WriteBytes(call + 1, &relative, sizeof(relative))) return false;
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(call), 5);
    return true;
}
const char* const kFe03Texts[5] = {"start survival", "host co-op", lobby::modeStatus, "records", "unavailable"};
// The game's map list order, the fallback when an entry's name cannot be read.
constexpr const char* kListOrder[5] = {"mp_bombRun", "mp_escape", "mp_solar", "mp_detonate", "mp_gov"};

using SelectFn = bool(__thiscall*)(void*, int);
volatile LONG pc01Calls = 0, fe03Calls = 0, mp20Calls = 0, pushes = 0, swallowed = 0, launches = 0, refused = 0, faults = 0, missingRefusals = 0;
volatile LONG survivalRefusals = 0, survivalWarnings = 0;
// The picked map in the picked mode: can the director play it? (13.10.: with survival,
// the pool or the master switch off, the map loaded empty without a word.) A refusal
// keeps the picker open, the overlay says why and what fixes it.
bool LaunchAllowed(const char* map) {
    const bool arena = coop_arena::IsArenaName(map);
    const LONG picked = InterlockedCompareExchange(&gamemode::mode, 0, 0);
    const int mode = arena ? arena::ModeForName(map, static_cast<int>(picked)) : static_cast<int>(picked);
    if (arena && mode != picked) mpmap::Event("menu: %s has no data for mode %ld: it plays mode %d", map, picked, mode);
    char why[240], fix[200];
    const int ready = lobby::SurvivalCheck(map, mode, why, fix);
    const int index = coop_mpmap::MapIndex(map);
    const char* label = index >= 0 ? coop_mpmap::MapLabel(index) : map;
    mpmap::Event("menu: survival check for %s (mode %d): %s%s%s", map, mode,
                 ready == 1 ? "ready" : ready == 2 ? "ready with a warning" : ready < 0 ? "still starting" : "NOT ready", why[0] ? " - " : "", why);
    if (ready == 1) return true;
    char what[120];
    if (ready == 2) {
        InterlockedIncrement(&survivalWarnings);
        std::snprintf(what, sizeof(what), "%s starts without AI enemies", label);
        overlay::ShowHint(what, why, fix, 12000);
        return true;
    }
    InterlockedIncrement(&survivalRefusals);
    InterlockedIncrement(&refused);
    std::snprintf(what, sizeof(what), "%s cannot start", label);
    overlay::ShowHint(what, why, fix, 15000);
    return false;
}
volatile LONG launchPending = 0;
char pendingMap[64] = "";
DWORD pendingAt = 0;

struct Site { const char* name; uint32_t rva; unsigned char bytes[16]; unsigned size; };
const Site kSites[] = {
    // mov eax,[esp+4] / sub esp,0x34 / push ebx / xor ebx,ebx / cmp eax,4 / ja
    {"PC01 OnInput_Select", kPc01Select, {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x34, 0x53, 0x33, 0xDB, 0x83, 0xF8, 0x04, 0x0F, 0x87}, 15},
    // mov ecx,[esp+4] / xor al,al / cmp ecx,4 / ja
    {"FE03 OnInput_Select", kFe03Select, {0x8B, 0x4C, 0x24, 0x04, 0x32, 0xC0, 0x83, 0xF9, 0x04, 0x0F, 0x87}, 11},
    // mov eax,[ecx+0x22C] / mov ecx,[0x0201C0E4] / push eax / call BeginMatchMaking
    {"MP20 OnInput_Select", kMp20Select, {0x8B, 0x81, 0x2C, 0x02, 0x00, 0x00, 0x8B, 0x0D, 0xE4, 0xC0, 0x01, 0x02, 0x50, 0xE8}, 14},
    // mov eax,[esp+4] / push esi / push edi / push 1 / push eax / mov esi,ecx / call
    {"Apt3dDirector::PushScreen", kPushScreen, {0x8B, 0x44, 0x24, 0x04, 0x56, 0x57, 0x6A, 0x01, 0x50, 0x8B, 0xF1, 0xE8}, 12},
    // push ebx / push esi / push edi / mov edi,[esp+0x10] / push edi / mov esi,ecx / xor ebx,ebx / call
    {"SP01 OnInput_Select", kSp01Select, {0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x57, 0x8B, 0xF1, 0x33, 0xDB, 0xE8}, 13},
};
bool sitesOk = false;
void AppendExtraMaps();

__declspec(noinline) bool CallSelect(uint32_t fn, void* screen, int index) {
    __try {
        return reinterpret_cast<SelectFn>(base + fn)(screen, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return true;
    }
}
__declspec(noinline) bool PushScreen(uint32_t hash) {
    uint32_t director = 0;
    if (!Read(base + kDirector, &director, 4) || !director) return false;
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kPushScreen)(reinterpret_cast<void*>(static_cast<uintptr_t>(director)), hash);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}

bool __fastcall OnPc01Select(void* screen, void*, int index) {
    InterlockedIncrement(&pc01Calls);
    if (index != 1) return CallSelect(kPc01Select, screen, index);
    // instead of Transition(3): the Multiplayer screen, with its own camera move and no login
    const bool pushed = PushScreen(kFe03Hash);
    InterlockedIncrement(&pushes);
    mpmap::Event("menu: main menu item 1 (survival) on screen %08X -> Multiplayer screen %s", static_cast<unsigned>(reinterpret_cast<uintptr_t>(screen)),
                 pushed ? "pushed" : "NOT pushed (no director)");
    return true;
}
// --- the Multiplayer screen's pages (14.10.) ---
// Item 3 ("unavailable") shows the records, the bottom bar's "EA Account" (input 10) how to
// play and "log out" (input 12) what this build is. The same button again closes the page.
constexpr uint32_t kTextEaAccount1 = 0x2B87C44F, kTextEaAccount2 = 0xBA5B35C9, kTextLogOut = 0x17F1B87B;
const char kHowToLabel[] = "how to play", kAboutLabel[] = "about";
constexpr DWORD kPageMs = 30000;
std::string pagesIni, bestFile;
volatile LONG pagesOpened = 0, pagesClosed = 0;
void KeyName(int vk, char* out, size_t size) {
    out[0] = 0;
    if (vk <= 0) { std::snprintf(out, size, "(off)"); return; }
    UINT scan = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    LONG param = static_cast<LONG>(scan << 16);
    // the numpad keys and Insert need their own names
    if (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) { std::snprintf(out, size, vk <= VK_NUMPAD9 ? "Num %d" : "Num %c", vk <= VK_NUMPAD9 ? vk - VK_NUMPAD0 : "*+,-./"[vk - VK_MULTIPLY]); return; }
    if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT) param |= 1 << 24;
    if (!scan || !GetKeyNameTextA(param, out, static_cast<int>(size))) std::snprintf(out, size, "key %d", vk);
}
int IniKey(const char* section, const char* key, int fallback) { return static_cast<int>(GetPrivateProfileIntA(section, key, fallback, pagesIni.c_str())); }
void PageEvent(const char* page, bool opened) {
    InterlockedIncrement(opened ? &pagesOpened : &pagesClosed);
    mpmap::Event("menu: %s page %s", page, opened ? "shown" : "closed");
}
void ShowRecords() {
    std::map<std::string, std::string> rows;   // map -> its records
    std::vector<std::string> order;
    std::ifstream file(bestFile, std::ios::binary);
    std::string line;
    unsigned n = 0;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t space = line.find(' ');
        if (space == std::string::npos) continue;
        std::string key = line.substr(0, space), value = line.substr(space + 1), mode;
        const size_t colon = key.find(':');
        if (colon != std::string::npos) { mode = key.substr(0, colon); key = key.substr(colon + 1); }
        char part[64];
        if (mode.empty()) std::snprintf(part, sizeof(part), "wave %s", value.c_str());
        else if (mode == "mercenaries") std::snprintf(part, sizeof(part), "mercenaries %s points", value.c_str());
        else if (mode == "zone") std::snprintf(part, sizeof(part), "zone %s held", value.c_str());
        else if (mode == "objective") std::snprintf(part, sizeof(part), "objective cleared, %s s left", value.c_str());
        else std::snprintf(part, sizeof(part), "%s %s", mode.c_str(), value.c_str());
        if (!rows.count(key)) order.push_back(key);
        std::string& row = rows[key];
        row += row.empty() ? part : std::string(" | ") + part;
        ++n;
    }
    std::vector<std::string> texts;
    for (const auto& key : order) {
        const int index = coop_mpmap::MapIndex(key.c_str());
        texts.push_back(std::string(index >= 0 ? coop_mpmap::MapLabel(index) : key.c_str()) + ": " + rows[key]);
    }
    if (texts.empty()) texts.push_back("No records yet: finish a round of any mode and it shows up here.");
    texts.push_back("Survival keeps the highest wave, Mercenaries the score, Hold the Zone the zones held, Objective the time left.");
    std::vector<const char*> lines;
    for (const auto& t : texts) lines.push_back(t.c_str());
    PageEvent("records", overlay::ShowPage("RECORDS", lines.data(), static_cast<int>(lines.size()), kPageMs));
    mpmap::Event("menu: records from %s: %u entries, %zu maps", bestFile.c_str(), n, order.size());
}
// A key name between the overlay's key colour marks.
void KeyMark(int vk, char* out, size_t size) {
    char name[32];
    KeyName(vk, name, sizeof(name));
    std::snprintf(out, size, "\x01%s\x02", name);
}
// The page reads the INI each time it opens, so it shows the keys as they are set now.
// Topics in the topic colour, keys in the key colour; features that are off are left out.
void ShowHowTo() {
    char shop[40], start[40], bots[40], call[40], mark[40], mine[40], field[40], turret[40], ring[40], necro[40], visit[40];
    char follow[40], goThere[40], attack[40], hold[40], vanguard[40], help[40], debug[40], freeCam[40], slow[40], hideHud[40], bug[48];
    KeyMark(IniKey("Shop", "Key", 'V'), shop, sizeof(shop));
    KeyMark(IniKey("Bot", "FollowHoldKey", VK_NUMPAD1), follow, sizeof(follow));
    KeyMark(IniKey("Bot", "GoThereKey", VK_NUMPAD2), goThere, sizeof(goThere));
    KeyMark(IniKey("Bot", "AttackTargetKey", VK_NUMPAD3), attack, sizeof(attack));
    KeyMark(IniKey("Bot", "FireHoldKey", VK_NUMPAD4), hold, sizeof(hold));
    KeyMark(IniKey("Bot", "VanguardKey", VK_NUMPAD5), vanguard, sizeof(vanguard));
    KeyMark(IniKey("Menu", "HelpKey", VK_F1), help, sizeof(help));
    KeyMark(IniKey("Survival", "StartKey", 0), start, sizeof(start));
    KeyMark(IniKey("Bot", "Key", 0), bots, sizeof(bots));
    KeyMark(IniKey("Bot", "CallKey", 0), call, sizeof(call));
    KeyMark(IniKey("Bot", "MarkKey", 105), mark, sizeof(mark));
    KeyMark(IniKey("Gadgets", "StasisMineKey", 103), mine, sizeof(mine));
    KeyMark(IniKey("Gadgets", "HealFieldKey", 104), field, sizeof(field));
    KeyMark(IniKey("Turret", "Key", 102), turret, sizeof(turret));
    KeyMark(IniKey("Zone", "ColorKey", VK_F7), ring, sizeof(ring));
    KeyMark(IniKey("Necro", "Key", VK_F11), necro, sizeof(necro));
    KeyMark(IniKey("MpMap", "Key", VK_DECIMAL), visit, sizeof(visit));
    KeyMark(IniKey("Debug", "Key", VK_INSERT), debug, sizeof(debug));
    KeyMark(IniKey("CameraTools", "FreeCamKey", VK_F6), freeCam, sizeof(freeCam));
    KeyMark(IniKey("CameraTools", "SlowKey", VK_F5), slow, sizeof(slow));
    KeyMark(IniKey("CameraTools", "HideHudKey", VK_F10), hideHud, sizeof(hideHud));
    char camUp[40], camDown[40], timeUp[40], timeDown[40], tiltL[40], tiltR[40];
    KeyMark(IniKey("CameraTools", "SpeedUpKey", VK_PRIOR), camUp, sizeof(camUp));
    KeyMark(IniKey("CameraTools", "SpeedDownKey", VK_NEXT), camDown, sizeof(camDown));
    KeyMark(IniKey("CameraTools", "TimeFasterKey", VK_HOME), timeUp, sizeof(timeUp));
    KeyMark(IniKey("CameraTools", "TimeSlowerKey", VK_END), timeDown, sizeof(timeDown));
    KeyMark(IniKey("CameraTools", "RollLeftKey", VK_OEM_COMMA), tiltL, sizeof(tiltL));
    KeyMark(IniKey("CameraTools", "RollRightKey", VK_OEM_PERIOD), tiltR, sizeof(tiltR));
    char bugKey[32];
    KeyName(IniKey("Coop", "BugReportKey", VK_F12), bugKey, sizeof(bugKey));
    std::snprintf(bug, sizeof(bug), "\x01%s%s\x02", IniKey("Coop", "BugReportCtrl", 1) ? "Ctrl+" : "", bugKey);
    const bool gadgetsInStory = IniKey("Gadgets", "Story", 1) != 0;
    std::vector<std::string> l;
    char t[240];
    auto add = [&](const char* format, ...) {
        va_list args;
        va_start(args, format);
        std::vsnprintf(t, sizeof(t), format, args);
        va_end(args);
        l.push_back(t);
    };
    add("\x03Supply terminal\x02 %s: guns, ammo, med packs, suits and upgrades for your credits.", shop);
    add("\x03Survival\x02 waves with breaks, the terminal opens in the breaks; %s starts the first wave at once.", start);
    add("\x03Mercenaries\x02 the clock runs down, kills and time capsules add seconds.");
    add("\x03Objective\x02 the map's own goals against the clock. \x03Hold the Zone\x02 stand in the ring, %s changes its colour.", ring);
    add("\x03Gadgets\x02 %s stasis mine, %s heal field, %s turret%s.", mine, field, turret, gadgetsInStory ? "" : " (only in the game modes)");
    if (IniKey("Bot", "Enabled", 0)) {
        add("\x03" "Bots\x02 %s on/off, %s calls them over, %s marks the moment in the bot log.", bots, call, mark);
        add("\x03" "Bot orders\x02 %s follow/hold, %s go there, %s attack target, %s hold fire, %s attack/cover formation.", follow, goThere, attack, hold,
            vanguard);
    }
    if (IniKey("Necro", "Enabled", 0)) add("\x03Necromorph player\x02 (experimental) %s.", necro);
    if (IniKey("MpMap", "Enabled", 0)) add("\x03Multiplayer map visit\x02 %s twice in the campaign, again on the map to go back.", visit);
    if (IniKey("CameraTools", "Enabled", 0)) {
        add("\x03" "Camera\x02 %s free camera, %s slow motion, %s hides the HUD.", freeCam, slow, hideHud);
        add("\x03" "Camera\x02 %s/%s flying speed, %s/%s game speed, %s/%s tilt; pad buttons in [CameraTools].", camUp, camDown, timeUp, timeDown, tiltL, tiltR);
    }
    if (IniKey("Debug", "Enabled", 1)) add("\x03" "Debug menu\x02 %s.", debug);
    add("\x03" "Co-op\x02 host co-op opens a lobby; friends pick join co-op with your address set in DS2UnitedConfig.exe.");
    if (IniKey("Coop", "BugReport", 1)) add("\x03" "Bug report\x02 %s saves the logs, a screenshot and a dump into one folder for a report.", bug);
    add("%s shows or hides this page at any time. Every key can be changed in DS2UnitedConfig.exe.", help);
    std::vector<const char*> lines;
    for (const auto& s : l) lines.push_back(s.c_str());
    PageEvent("how to play", overlay::ShowPage("HOW TO PLAY", lines.data(), static_cast<int>(lines.size()), kPageMs));
}
// [Menu] HelpKey shows the how to play page in a level too. Only the window in front
// reacts, so two copies on one PC do not both open it.
int helpKey = VK_F1;
bool helpWasDown = false;
volatile LONG helpPresses = 0;
void PollHelpKey() {
    if (helpKey <= 0 || pagesIni.empty()) return;
    const bool down = (GetAsyncKeyState(helpKey) & 0x8000) != 0;
    const bool pressed = down && !helpWasDown;
    helpWasDown = down;
    if (!pressed) return;
    uint32_t window = 0;
    if (!Read(base + focusrate::kWindowRva, &window, 4) || !window ||
        GetForegroundWindow() != reinterpret_cast<HWND>(static_cast<uintptr_t>(window)))
        return;
    InterlockedIncrement(&helpPresses);
    ShowHowTo();
}
void ShowAbout() {
    const uint32_t build = lobby::BuildId();
    const time_t stamp = static_cast<time_t>(build);
    tm local{};
    char when[32] = "?";
    if (localtime_s(&local, &stamp) == 0) std::strftime(when, sizeof(when), "%d.%m.%Y %H:%M", &local);
    char l[8][200];
    std::snprintf(l[0], 200, "Dead Space 2 United " DS2UNITED_VERSION ", a fan made mod: co-op story, survival game modes, bots and the enemy randomizer.");
    std::snprintf(l[1], 200, "This build: %08X from %s. Everybody in a session needs the same one.", build, when);
    std::snprintf(l[2], 200, "A highly experimental release: expect crashes and bugs, and back up your saves.");
    std::snprintf(l[3], 200, "Created by Richard Hafer. Thanks to Wemino (Marker Patch) and Mark (Dead Space 2 Multiplayer).");
    std::snprintf(l[4], 200, "Help, news and bug reports: the Dead Space Modding Discord, discord.com/invite/sWMRkYJ5JV");
    std::snprintf(l[5], 200, "The keys: the how to play page here, or its key in the game (F1 unless changed).");
    std::snprintf(l[6], 200, "Settings: DS2UnitedConfig.exe in the game folder.");
    std::snprintf(l[7], 200, "Logs for bug reports: plugins\\DS2United.log, DS2Randomizer.log, DS2Bots.log.");
    const char* lines[8];
    for (int i = 0; i < 8; ++i) lines[i] = l[i];
    PageEvent("about", overlay::ShowPage("ABOUT", lines, 8, kPageMs));
}
// One press reaches the select several times (17.10.: three calls within 300 ms):
// the decision is logged and shown once per press.
bool FirstOfPress(int index) {
    static int lastIndex = -1;
    static DWORD lastAt = 0;
    const DWORD now = GetTickCount();
    const bool first = index != lastIndex || now - lastAt > 1000;
    lastIndex = index;
    lastAt = now;
    return first;
}
volatile LONG launcherMenuRefusals = 0;
// A launcher's client (DS2COOP) neither hosts nor plays survival alone: it has no wave
// director of its own (NetRole 2) and follows its host.
bool RefuseLauncherClient(int index, const char* item, bool first) {
    if (!coop::installed || !LauncherClient()) return false;
    InterlockedIncrement(&launcherMenuRefusals);
    if (first) {
        mpmap::Event("menu: Multiplayer item %d (%s) -> REFUSED: the launcher (DS2COOP) started this copy as client machine %u", index, item, machineIndex);
        // the overlay card, not a RIG notice: those wait for a player and would show in the next level
        overlay::ShowHint("This copy was started as the client by the launcher",
                          "the launcher fixed the roles: this copy joins the host and cannot host or play survival alone",
                          "pick join co-op: it follows the host's story level, or the host's survival launch once the host picked host co-op", 15000);
    }
    return true;
}
bool __fastcall OnFe03Select(void* screen, void*, int index) {
    InterlockedIncrement(&fe03Calls);
    const bool first = FirstOfPress(index);
    if (index == 0) {
        if (RefuseLauncherClient(index, "start survival", first)) return false;
        const bool solo = !LauncherHost() && ConfigureSolo();
        lobby::Stop();
        if (first) mpmap::Event("menu: Multiplayer item 0 (start survival) -> native map picker (%s)", solo ? "solo" : LauncherHost() ? "the launcher's host, network kept" : "role kept");
        // the launcher's host plays survival as the networked host; only host co-op takes the partner along
        if (first && !solo && LauncherHost())
            overlay::ShowHint("This copy was started as the host by the launcher", "start survival plays without the partner: no lobby, no launch for the partner",
                              "to play together pick host co-op; the partner's copy picks join co-op", 15000);
        AppendExtraMaps();
        return CallSelect(kFe03Select, screen, 0);   // bookmark + MP20, as natively
    }
    if (index == 1) {
        if (RefuseLauncherClient(index, "host co-op", first)) return false;
        const bool hosting = coop::installed && lobby::StartHost() && ConfigureAsHost();
        if (first || hosting)
            mpmap::Event("menu: Multiplayer item 1 (host co-op) -> %s%s", hosting ? "lobby open, native map picker" : "REFUSED (see the lobby and coop role lines)",
                         hosting && LauncherHost() ? " (the launcher's host: role, partners and session kept)" : "");
        if (!hosting) {
            // the lobby opened but the role was refused: a session of this game start still runs (StartHost shows its own refusals)
            if (first && coop::installed && lobby::role == lobby::Role::Host)
                overlay::ShowHint("Host co-op refused", "this game start already ran a co-op session that cannot host another match",
                                  "quit to the desktop and start the game again to host", 15000);
            lobby::Stop();
            return false;
        }
        AppendExtraMaps();
        return CallSelect(kFe03Select, screen, 0);
    }
    if (index == 2) {
        const bool joining = coop::installed && lobby::StartJoin(joinAddress);
        if (first) mpmap::Event("menu: Multiplayer item 2 (join co-op) -> %s", joining ? "saying hello to the host" : "REFUSED");
        return false;   // stays on this screen; the label shows the progress
    }
    if (index == 3) {
        ShowRecords();
        return false;
    }
    if (index == 4) {
        // the native item 2's push of the character setup ([Loadout])
        const bool opened = loadout::OpenFromMenu();
        mpmap::Event("menu: Multiplayer item 4 (character setup) -> %s", opened ? "FE16 pushed" : "refused (see the loadout line)");
        if (opened) return true;
    }
    InterlockedIncrement(&swallowed);
    mpmap::Event("menu: Multiplayer item %d ignored (its native action needs Blaze)", index);
    return false;
}
// X (code 10, also the help bar's "welcome" click) pushes the welcome-back screen,
// which reads the null Blaze hub (dump 2328, 0x0060E144 in NU04WelcomeBack::Show);
// code 12 opens the online disconnect popup. Both are swallowed.
volatile LONG fe03InputSwallowed = 0;
int __fastcall OnFe03Input(void* screen, void*, int code) {
    if (code == 10 || code == 12) {
        if (InterlockedIncrement(&fe03InputSwallowed) <= 10) mpmap::Event("menu: Multiplayer screen input %d swallowed (it needs Blaze)", code);
        if (code == 10) ShowHowTo();
        else ShowAbout();
        return 2;
    }
    return reinterpret_cast<int(__thiscall*)(void*, int)>(base + kFe03Input)(screen, code);
}

// SP01's input (vtable slot 0x01B38138, 0x00A989D0, thiscall(code), ret 4): in the list state
// ([screen+0x518] 0) it hands over to the shared list input 0x00A6AEF0, which knows only
// 8..13 (up, down, select, back); left (6) and right (7) went nowhere. On the "join co-op"
// row they now switch between joining as a human and as a necromorph: the row's label
// changes, UIExternScrollMenuIterator::UpdateItem (0x015E9800) draws it again and the menu's
// own navigation sound plays.
constexpr uint32_t kSp01InputSlot = 0x1738138, kSp01Input = 0x6989D0, kListActiveIndex = 0x11E8C50, kListUpdateItem = 0x11E9800;
bool sp01InputOk = false;
volatile LONG joinRoleSwitches = 0;
int __fastcall OnSp01Input(void* screen, void*, int code) {
    if ((code == 6 || code == 7) && screen && lobby::JoinRoleOffered()) {
        lobby::LoadJoinRole();
        const uintptr_t s = reinterpret_cast<uintptr_t>(screen);
        uint32_t state = 1;
        int row = -1;
        if (lobby::ShowsJoinChoice() && Read(s + 0x518, &state, 4) && state == 0) {
            __try {
                row = reinterpret_cast<int(__thiscall*)(void*)>(base + kListActiveIndex)(reinterpret_cast<void*>(s + 0x5C));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                row = -1;
            }
        }
        if (row == 4) {
            lobby::SetJoinRole(!lobby::joinAsNecro);
            InterlockedIncrement(&joinRoleSwitches);
            __try {
                reinterpret_cast<void(__thiscall*)(void*, int, int)>(base + kListUpdateItem)(reinterpret_cast<void*>(s + 0x5C), 4, 0);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            gamemode_nav::Sound();
            return 2;
        }
    }
    return reinterpret_cast<int(__thiscall*)(void*, int)>(base + kSp01Input)(screen, code);
}

// The co-op screen (SP01, the old single player screen): whoever starts a game
// here hosts; "previously on Dead Space" became "join co-op".
volatile LONG sp01Calls = 0;
bool __fastcall OnSp01Select(void* screen, void*, int index) {
    InterlockedIncrement(&sp01Calls);
    if (index == 4) {
        lobby::LoadJoinRole();
        const bool joining = coop::installed && lobby::StartJoin(joinAddress);
        if (joining && lobby::JoinRoleOffered() && lobby::joinAsNecro) necro::JoinAsNecro();
        mpmap::Event("menu: co-op item 4 (join co-op) -> %s", joining ? "saying hello to the host, waiting for its next checkpoint restart" : "REFUSED");
        return false;   // stays here; the label shows the progress
    }
    // continue, new game, load game, new game+: this machine hosts; the lobby
    // waits for joiners and the network switches on when the first one knocks
    difficulty::OnCampaignScreen();   // the difficulty screen ahead is the campaign's own
    const bool lobbyOpen = coop::installed && lobby::StartHost(lobby::Mode::Campaign);
    mpmap::Event("menu: co-op item %d -> native, campaign lobby %s", index, lobbyOpen ? "open" : "NOT open");
    return CallSelect(kSp01Select, screen, index);
}

// The map list entry's name: "mp_bombRun\ mp_bombRun.str " behind a String at
// entry+4, either inline or through a pointer; the map is the first token.
bool EntryMap(uint32_t entry, char* out, size_t size) {
    char text[48]{};
    uint32_t pointer = 0;
    const auto take = [&](const char* s) {
        size_t n = 0;
        while (s[n] && s[n] != '\\' && s[n] != ' ' && n + 1 < size) { out[n] = s[n]; ++n; }
        out[n] = 0;
        return coop_mpmap::MapIndex(out) >= 0 || coop_arena::ArenaIndex(out) >= 0;
    };
    if (Read(entry + 4, &pointer, 4) && pointer && Read(pointer, text, sizeof(text) - 1) && take(text)) return true;
    std::memset(text, 0, sizeof(text));
    return Read(entry + 4, text, 15) && take(text);
}

// The game's list holds the five shipped maps (LoadMapInfo 0x00A99800, run once
// from 0x00AA4584). Before the picker opens, every further map of
// coop_mpmap::kMapTable whose archive is mounted is appended the way
// LoadMapInfo does it: Array<MultiPlayerMapInfo>::Add (thiscall, returns the
// zeroed 0x54 byte entry, count +1) and String::Set (thiscall, copies) on the
// path +4, name +0x14, description +0x24 and mission text +0x34. The picker
// reads entries bounds checked against the count (0x00A49CF0, 0x00A4BA90) and
// shows text without '$' as written.
constexpr uint32_t kMapListAdd = 0x691CB0, kStringSet = 0x33AAE0;
const unsigned char kMapListAddBytes[] = {0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x08, 0x57, 0x33, 0xFF, 0x39, 0x46, 0x04, 0x75};
const unsigned char kStringSetBytes[] = {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x8B, 0xF1, 0x85, 0xDB, 0x74};
bool extraMaps = true, extraSitesOk = false;
volatile LONG extraAdded = 0, extraFaults = 0;
struct ExtraTexts { char path[80], description[64], mission[64], label[64]; };
ExtraTexts extraTexts[coop_mpmap::kMapCount], arenaTexts[coop_arena::kArenaCount];

__declspec(noinline) bool AppendMapRaw(uint32_t list, const char* path, const char* name, const char* description, const char* mission) {
    __try {
        const uint32_t entry = reinterpret_cast<uint32_t(__thiscall*)(void*)>(base + kMapListAdd)(reinterpret_cast<void*>(static_cast<uintptr_t>(list)));
        if (!entry) return false;
        *reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(entry)) = 1;
        using Set = void(__thiscall*)(void*, const char*);
        const auto set = reinterpret_cast<Set>(base + kStringSet);
        set(reinterpret_cast<void*>(static_cast<uintptr_t>(entry + 0x04)), path);
        set(reinterpret_cast<void*>(static_cast<uintptr_t>(entry + 0x14)), name);
        set(reinterpret_cast<void*>(static_cast<uintptr_t>(entry + 0x24)), description);
        set(reinterpret_cast<void*>(static_cast<uintptr_t>(entry + 0x34)), mission);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&extraFaults);
        return false;
    }
}
// Game thread (the Multiplayer screen's select), before the picker is pushed.
void AppendExtraMaps() {
    if (!extraMaps || !extraSitesOk) return;
    uint32_t dgm = 0, maps = 0, count = 0;
    if (!Read(base + kGameManager, &dgm, 4) || !dgm) { mpmap::Event("menu: extra maps skipped: no game manager"); return; }
    Read(dgm + kDgmMaps, &maps, 4);
    Read(dgm + kDgmMapCount, &count, 4);
    if (!maps || !count || count > coop_mpmap::kMaxMaps) {
        mpmap::Event("menu: extra maps skipped: map list %08X count %u", maps, count);
        return;
    }
    bool listed[coop_mpmap::kMapCount] = {}, arenaListed[coop_arena::kArenaCount] = {};
    for (uint32_t i = 0; i < count; ++i) {
        char name[64]{};
        if (!EntryMap(maps + i * kMapEntrySize, name, sizeof(name))) continue;
        const int mp = coop_mpmap::MapIndex(name), ar = coop_arena::ArenaIndex(name);
        if (mp >= 0) listed[mp] = true;
        if (ar >= 0) arenaListed[ar] = true;
    }
    unsigned added = 0, missing = 0;
    for (unsigned i = 0; i < coop_mpmap::kMapCount; ++i) {
        const coop_mpmap::MapInfo& m = coop_mpmap::kMapTable[i];
        if (listed[i] || m.kind == coop_mpmap::MapKind::Shipped) continue;
        if (!mpmap::MapAvailable(static_cast<int>(i))) { ++missing; continue; }
        ExtraTexts& t = extraTexts[i];
        std::snprintf(t.path, sizeof(t.path), "%s\\ %s.str ", m.name, m.name);
        std::snprintf(t.description, sizeof(t.description), "outbreak dlc map (ds2dat12)");
        std::snprintf(t.mission, sizeof(t.mission), "survive the waves");
        const bool ok = AppendMapRaw(dgm + kDgmMaps, t.path, m.label, t.description, t.mission);
        uint32_t after = 0;
        Read(dgm + kDgmMapCount, &after, 4);
        mpmap::Event("menu: map list + %s (\"%s\") %s, count %u", m.name, m.label, ok ? "appended" : "FAULTED", after);
        if (!ok) return;
        ++added;
        InterlockedIncrement(&extraAdded);
    }
    // the campaign arenas (runtime_coop_arena.inc), after the maps
    for (unsigned i = 0; arena::enabled && i < coop_arena::kArenaCount; ++i) {
        const coop_arena::Arena& a = coop_arena::kArenas[i];
        if (arenaListed[i]) continue;
        if (count + added >= coop_mpmap::kMaxMaps) { mpmap::Event("menu: arena %s not appended: the list is full", a.name); break; }
        ExtraTexts& t = arenaTexts[i];
        std::snprintf(t.path, sizeof(t.path), "%s\\ %s.str ", a.name, a.name);
        std::snprintf(t.description, sizeof(t.description), "campaign arena: doors locked, rooms held");
        std::snprintf(t.mission, sizeof(t.mission), "survive the waves");
        std::snprintf(t.label, sizeof(t.label), "%s", a.label);   // the title alone (user, 16.10.)
        const bool ok = AppendMapRaw(dgm + kDgmMaps, t.path, t.label, t.description, t.mission);
        uint32_t after = 0;
        Read(dgm + kDgmMapCount, &after, 4);
        mpmap::Event("menu: map list + %s (\"%s\") %s, count %u", a.name, t.label, ok ? "appended" : "FAULTED", after);
        if (!ok) return;
        ++added;
        InterlockedIncrement(&extraAdded);
    }
    if (added || missing) mpmap::Event("menu: extra maps: %u appended, %u not mounted, list now %u", added, missing, count + added);
}

bool __fastcall OnMp20Select(void* screen, void*, int) {
    InterlockedIncrement(&mp20Calls);
    const uint32_t s = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(screen));
    uint32_t index = 0, dgm = 0, maps = 0, count = 0;
    unsigned char any = 0;
    Read(s + kMp20Index, &index, 4);
    Read(s + kMp20Any, &any, 1);
    Read(base + kGameManager, &dgm, 4);
    if (dgm) { Read(dgm + kDgmMaps, &maps, 4); Read(dgm + kDgmMapCount, &count, 4); }
    if (!maps || !count || count > coop_mpmap::kMaxMaps) {
        InterlockedIncrement(&refused);
        mpmap::Event("menu: map picker select REFUSED: no map list (manager %08X list %08X count %u)", dgm, maps, count);
        return true;
    }
    uint32_t pick = index;
    const LONG picked = InterlockedCompareExchange(&gamemode::mode, 0, 0);
    if (any || index >= count) {
        uint32_t enabledMask = 0;
        for (uint32_t i = 0; i < count; ++i) {
            unsigned char on = 0;
            char name[64]{};
            // 'any map' only picks maps every joiner can load, and an arena only in a mode it plays
            if (EntryMap(maps + i * kMapEntrySize, name, sizeof(name)) &&
                (lobby::MissingFor(coop_mpmap::MapIndex(name)) || (coop_arena::IsArenaName(name) && arena::ModeForName(name, static_cast<int>(picked)) != picked)))
                continue;
            if (Read(maps + i * kMapEntrySize, &on, 1) && on) enabledMask |= 1u << i;
        }
        uint32_t live = 0;
        for (uint32_t i = 0; i < count; ++i) if (enabledMask >> i & 1u) ++live;
        if (!live) { mpmap::Event("menu: map picker 'any map' REFUSED: no enabled map"); return true; }
        uint32_t nth = GetTickCount() % live;
        for (uint32_t i = 0; i < count; ++i)
            if (enabledMask >> i & 1u) { if (!nth) { pick = i; break; } --nth; }
    }
    char map[64]{};
    const bool read = EntryMap(maps + pick * kMapEntrySize, map, sizeof(map));
    if (!read && pick < 5) std::snprintf(map, sizeof(map), "%s", kListOrder[pick]);
    if (!map[0]) {
        InterlockedIncrement(&refused);
        mpmap::Event("menu: map picker select REFUSED: entry %u of %u has no readable map name", pick, count);
        return true;
    }
    if (!LaunchAllowed(map)) return true;   // stays on the picker
    // an arena plays the modes it has points for: the lobby, the loadout and the joiners
    // follow the mode it really plays, not a Versus picked on an earlier row
    if (coop_arena::IsArenaName(map)) {
        const int plays = arena::ModeForName(map, static_cast<int>(picked));
        if (plays != picked) {
            InterlockedExchange(&gamemode::mode, plays);
            mpmap::Event("menu: %s cannot play mode %ld: the picked mode is now %d", map, picked, plays);
        }
    }
    // [Difficulty] Pick: the game's difficulty screen first, its accept runs the rest of this
    // select (difficulty::LaunchPicked); a pick a row already holds is written here instead
    if (difficulty::OpenPicker(maps + pick * kMapEntrySize, map)) {
        mpmap::Event("menu: map picker select index=%u any=%u count=%u -> entry %u = %s (%s); difficulty screen first", index, any, count, pick, map,
                     read ? "read from the list" : "list order fallback");
        return true;
    }
    // a co-op host waits on the game's own lobby screen for the countdown ([NativeLobby])
    if (nativelobby::HostWants() && nativelobby::OpenFromPicker(maps + pick * kMapEntrySize, map)) {
        mpmap::Event("menu: map picker select index=%u any=%u -> entry %u = %s; lobby screen open", index, any, pick, map);
        return true;
    }
    std::snprintf(pendingMap, sizeof(pendingMap), "%s", map);
    pendingAt = GetTickCount();
    InterlockedExchange(&launchPending, 1);
    mpmap::Event("menu: map picker select index=%u any=%u count=%u -> entry %u = %s (%s); launch on the next game tick", index, any, count, pick, map,
                 read ? "read from the list" : "list order fallback");
    return true;   // never BeginMatchMaking
}

DWORD lastTextPass = 0;
// Game thread, from coop::Poll: the launch runs outside the menu's input dispatch.
void Tick(DWORD now) {
    PollHelpKey();
    if (installed && now - lastTextPass >= 500) { lastTextPass = now; notice::ApplyOverrides(); }
    if (!installed || !InterlockedCompareExchange(&launchPending, 0, 0)) return;
    InterlockedExchange(&launchPending, 0);
    const bool isArena = coop_arena::IsArenaName(pendingMap);
    // a joiner without this map's archive (the Outbreak maps need DS2DAT12.DAT) would drop out at the launch
    const uint8_t lacking = isArena ? 0 : lobby::MissingFor(coop_mpmap::MapIndex(pendingMap));
    if (lacking) {
        InterlockedIncrement(&refused);
        InterlockedIncrement(&missingRefusals);
        char text[120];
        std::snprintf(text, sizeof(text), "PLAYER %u CANNOT LOAD %s (DS2DAT12.DAT MISSING) - PICK ANOTHER MAP", lacking + 1u,
                      coop_mpmap::MapLabel(coop_mpmap::MapIndex(pendingMap)));
        notice::Enqueue(text);
        char what[96];
        std::snprintf(what, sizeof(what), "Player %u cannot load %s", lacking + 1u, coop_mpmap::MapLabel(coop_mpmap::MapIndex(pendingMap)));
        overlay::ShowHint(what, "that player's game has no DS2DAT12.DAT (the Outbreak maps)", "pick another map, or that player installs the Outbreak maps", 15000);
        mpmap::Event("menu: launch of %s REFUSED: joiner machine %u cannot load it", pendingMap, lacking);
        return;
    }
    const bool started = isArena ? arena::Start(pendingMap, now) : mpmap::StartVisitFromMenu(pendingMap, now);
    InterlockedIncrement(started ? &launches : &refused);
    // the joiners follow into an arena too ([Arena] Modes): its index as an arena code
    if (started && (!isArena || arena::modesOn))
        lobby::HostLaunched(isArena ? coop_lobby::ArenaCode(static_cast<unsigned>(coop_arena::ArenaIndex(pendingMap))) : coop_mpmap::MapIndex(pendingMap));
    mpmap::Event("menu: launch of %s %s %lu ms after the select", pendingMap, started ? "started" : "REFUSED", static_cast<unsigned long>(now - pendingAt));
}

// The vtables sit in read-only image data, where WriteProcessMemory refused
// (first test 09.10.): lift the protection for the one dword.
bool WriteSlot(uint32_t address, uint32_t value, DWORD& protectBefore) {
    DWORD old = 0;
    protectBefore = 0;
    void* at = reinterpret_cast<void*>(static_cast<uintptr_t>(address));
    // image pages (live: PAGE_READONLY, allocated EXECUTE_WRITECOPY); the hooks use the same route for code
    if (!VirtualProtect(at, 4, PAGE_READWRITE, &old) && !VirtualProtect(at, 4, PAGE_WRITECOPY, &old) &&
        !VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    protectBefore = old;
    const bool ok = CopyIn(address, &value, 4);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(static_cast<uintptr_t>(address)), 4, old, &ignored);
    return ok;
}
bool SwapSlot(FILE* log, const char* name, uint32_t slotRva, uint32_t expectRva, void* handler) {
    uint32_t current = 0;
    if (!Read(base + slotRva, &current, 4)) { std::fprintf(log, "menu slot %s %08X unreadable\n", name, static_cast<unsigned>(base + slotRva)); return false; }
    const uint32_t ours = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handler));
    if (current == ours) return true;
    if (current != base + expectRva) {
        std::fprintf(log, "menu slot %s %08X holds %08X, expected %08X (another mod?); left alone\n", name, static_cast<unsigned>(base + slotRva), current,
                     static_cast<unsigned>(base + expectRva));
        return false;
    }
    DWORD protect = 0;
    const bool ok = WriteSlot(base + slotRva, ours, protect);
    uint32_t after = 0;
    Read(base + slotRva, &after, 4);
    std::fprintf(log, "menu slot %s %08X: %08X -> %08X %s (protection %02lX, reads %08X)\n", name, static_cast<unsigned>(base + slotRva), current, ours,
                 ok && after == ours ? "ok" : "WRITE FAILED", static_cast<unsigned long>(protect), after);
    return ok && after == ours;
}
bool SetLabel(FILE* log, const char* name, uint32_t rva, uint32_t expectRva, const char* text) {
    uint32_t current = 0;
    if (!Read(base + rva, &current, 4)) return false;
    const uint32_t ours = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(text));
    if (current != ours && expectRva && current != base + expectRva) {
        std::fprintf(log, "menu label %s %08X holds %08X, expected %08X; left alone\n", name, static_cast<unsigned>(base + rva), current,
                     static_cast<unsigned>(base + expectRva));
        return false;
    }
    const bool ok = current == ours || WriteBytes(base + rva, &ours, 4);
    std::fprintf(log, "menu label %s %08X -> \"%s\" %s\n", name, static_cast<unsigned>(base + rva), text, ok ? "ok" : "WRITE FAILED");
    return ok;
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Menu", "Enabled", 0, ini.c_str()) != 0;
    // the hint cards need the overlay in the menus too
    overlay::hintsOn = GetPrivateProfileIntA("Notice", "OverlayHints", 1, ini.c_str()) != 0;
    overlay::hintMaxMs = static_cast<DWORD>(GetPrivateProfileIntA("Notice", "OverlayHintSeconds", 5, ini.c_str())) * 1000u;
    if (!overlay::ready) overlay::Install(log, ini);   // hint cards and the release notice
    std::fprintf(log, "hint cards %s ([Notice] OverlayHints): refusals and warnings say why and how to fix it\n", overlay::hintsOn ? (overlay::ready ? "on" : "on, overlay NOT installed") : "off");
    helpKey = static_cast<int>(GetPrivateProfileIntA("Menu", "HelpKey", VK_F1, ini.c_str()));
    if (helpKey > 0 && !overlay::ready) overlay::Install(log, ini);
    if (helpKey > 0 && overlay::ready) pagesIni = ini;
    std::fprintf(log, "help key %d ([Menu] HelpKey): %s\n", helpKey,
                 helpKey <= 0 ? "off" : overlay::ready ? "shows the how to play page in menus and levels" : "overlay NOT installed, no page");
    std::fprintf(log, "Dead Space 2 United %s, a highly experimental release: notice on the main menu, overlay %s\n", DS2UNITED_VERSION,
                 overlay::ready ? "ready" : "NOT installed");
    if (!enabled || !mpmap::enabled) ReleaseNotice(12000);
    if (!enabled) { std::fprintf(log, "menu off ([Menu] Enabled=0)\n"); return; }
    if (!mpmap::enabled) { std::fprintf(log, "menu off: needs [MpMap] Enabled=1 for the launch\n"); return; }
    sitesOk = true;
    for (const Site& s : kSites) {
        unsigned char actual[16]{};
        const bool ok = Read(base + s.rva, actual, s.size) && std::memcmp(actual, s.bytes, s.size) == 0;
        if (!ok) sitesOk = false;
        std::fprintf(log, "menu sig %-28s %08X %s\n", s.name, static_cast<unsigned>(base + s.rva), ok ? "ok" : "MISMATCH");
    }
    if (!sitesOk) { std::fprintf(log, "menu off: a menu function did not match\n"); return; }
    extraMaps = GetPrivateProfileIntA("Menu", "ExtraMaps", 1, ini.c_str()) != 0;
    {
        unsigned char add[sizeof(kMapListAddBytes)]{}, set[sizeof(kStringSetBytes)]{};
        const bool addOk = Read(base + kMapListAdd, add, sizeof(add)) && std::memcmp(add, kMapListAddBytes, sizeof(add)) == 0;
        const bool setOk = Read(base + kStringSet, set, sizeof(set)) && std::memcmp(set, kStringSetBytes, sizeof(set)) == 0;
        extraSitesOk = addOk && setOk;
        std::fprintf(log, "menu sig %-28s %08X %s\n", "map list Add", static_cast<unsigned>(base + kMapListAdd), addOk ? "ok" : "MISMATCH");
        std::fprintf(log, "menu sig %-28s %08X %s\n", "String::Set", static_cast<unsigned>(base + kStringSet), setOk ? "ok" : "MISMATCH");
        unsigned offered = 0, extras = 0;
        for (unsigned i = 0; i < coop_mpmap::kMapCount; ++i) {
            if (coop_mpmap::kMapTable[i].kind == coop_mpmap::MapKind::Shipped) continue;
            ++extras;
            if (mpmap::MapAvailable(static_cast<int>(i))) ++offered;
        }
        std::fprintf(log, "menu extra maps %s: %u of %u mounted%s\n", !extraMaps ? "off ([Menu] ExtraMaps=0)" : extraSitesOk ? "on" : "off (a list function did not match)",
                     offered, extras, offered ? "" : " (nothing to append)");
    }
    bool ok = SetLabel(log, "main menu item 1", kPc01Labels + 4, kPc01MultiLabel, kSurvivalLabel);
    notice::AddOverride(kTextMainMenuTitle, kMainMenuTitle);
    const bool lookup = InstallTitleLookup();
    if (!lookup) ReleaseNotice(12000);
    std::fprintf(log, "menu first title lookup: %s\n", lookup ? "installed" : "unavailable; table override active");
    notice::AddOverride(kTextMultiplayerTitle, kModesTitle);
    notice::AddOverride(kTextEaAccount1, kHowToLabel);
    notice::AddOverride(kTextEaAccount2, kHowToLabel);
    notice::AddOverride(kTextLogOut, kAboutLabel);
    pagesIni = ini;
    bestFile = ini.substr(0, ini.find_last_of("\\/") + 1) + "DS2SurvivalBest.txt";
    notice::AddOverride(kTextGameMode, kGameModeLabel);
    GetPrivateProfileStringA("Menu", "NatLine1", "Dead Space 2 United game modes", natLine1, sizeof(natLine1), ini.c_str());
    GetPrivateProfileStringA("Menu", "NatLine2", "Host or join over LAN (or virtual network), set up in DS2UnitedConfig.exe", natLine2, sizeof(natLine2), ini.c_str());
    // an INI from before the rename still carries the old default texts
    if (!std::strcmp(natLine1, "Co-op Mod gamemodes")) std::snprintf(natLine1, sizeof(natLine1), "Dead Space 2 United game modes");
    if (!std::strcmp(natLine2, "Host or join over LAN (or virtual network), set up in DS2CoopConfig.exe"))
        std::snprintf(natLine2, sizeof(natLine2), "Host or join over LAN (or virtual network), set up in DS2UnitedConfig.exe");
    if (natLine1[0]) notice::AddOverride(kTextNatType, natLine1);
    if (natLine2[0]) notice::AddOverride(kTextNatSupport, natLine2);
    std::fprintf(log, "menu NAT lines: '%s' / '%s' (in place of the game's NAT warning; empty keeps it)\n", natLine1[0] ? natLine1 : "(the game's)",
                 natLine2[0] ? natLine2 : "(the game's)");
    // on a line of its own: after EA's text it wrapped in the middle (14.10.)
    GetPrivateProfileStringA("Menu", "TitleLine", kTitleLine, titleLine + 1, sizeof(titleLine) - 1, ini.c_str());
    // the earlier defaults of older INIs
    if (!std::strcmp(titleLine + 1, "Co-op Mod") || !std::strcmp(titleLine + 1, "Dead Space 2 United"))
        std::snprintf(titleLine + 1, sizeof(titleLine) - 1, "%s", kTitleLine);
    if (titleLine[1]) {
        titleLine[0] = '\n';
        notice::AddAppend(kTextLegal, titleLine);
    }
    std::fprintf(log, "menu title line: '%s' after EA's legal line (empty keeps it alone)\n", titleLine[1] ? titleLine + 1 : "");
    const bool coopCampaign = GetPrivateProfileIntA("Menu", "CampaignCoop", 1, ini.c_str()) != 0;
    if (coopCampaign) {
        SetLabel(log, "main menu item 0", kPc01Labels, kPc01SingleLabel, kCoopLabel);
        lobby::SetIdleStatus();
        std::snprintf(joinDescription, sizeof(joinDescription), "%s%s", kJoinDescription,
                      lobby::JoinRoleOffered() ? ". Left/right: join as a human or as a necromorph" : "");
        SetLabel(log, "co-op item 4", kSp01Labels + 16, kSp01PreviouslyLabel, lobby::status);
        SetLabel(log, "co-op item 4 description", kSp01Descriptions + 16, kSp01PreviouslyDesc, joinDescription);
        notice::AddOverride(kTextSinglePlayer, kCoopLabel);
        notice::AddOverride(kTextSinglePlayerTitle, kCoopTitle);
    }
    for (int i = 0; i < 5; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "multiplayer item %d", i);
        SetLabel(log, name, kFe03Labels + 4 * i, i == 0 ? kFe03FirstLabel : 0, kFe03Texts[i]);
    }
    ok = SwapSlot(log, "PC01 select", kPc01SelectSlot, kPc01Select, reinterpret_cast<void*>(OnPc01Select)) && ok;
    ok = SwapSlot(log, "FE03 select", kFe03SelectSlot, kFe03Select, reinterpret_cast<void*>(OnFe03Select)) && ok;
    {
        // mov edx,[esp+4] / mov eax,edx / sub esp,0xC / sub eax,0xA / je / sub eax,2: on its own, the menu does not depend on it
        static const unsigned char kInput[] = {0x8B, 0x54, 0x24, 0x04, 0x8B, 0xC2, 0x83, 0xEC, 0x0C, 0x83, 0xE8, 0x0A, 0x74, 0x21, 0x83, 0xE8};
        unsigned char actual[sizeof(kInput)]{};
        const bool inputOk = Read(base + kFe03Input, actual, sizeof(actual)) && std::memcmp(actual, kInput, sizeof(kInput)) == 0;
        std::fprintf(log, "menu sig %-28s %08X %s\n", "FE03 HandleInput", static_cast<unsigned>(base + kFe03Input), inputOk ? "ok" : "MISMATCH (X stays native)");
        if (inputOk) SwapSlot(log, "FE03 HandleInput", kFe03InputSlot, kFe03Input, reinterpret_cast<void*>(OnFe03Input));
    }
    ok = SwapSlot(log, "MP20 select", kMp20SelectSlot, kMp20Select, reinterpret_cast<void*>(OnMp20Select)) && ok;
    if (coopCampaign) ok = SwapSlot(log, "SP01 select", kSp01SelectSlot, kSp01Select, reinterpret_cast<void*>(OnSp01Select)) && ok;
    if (coopCampaign) {
        // mov eax,[ecx+518h] / sub eax,0 / push esi / mov esi,2 / je: on its own, the menu does not depend on it
        static const unsigned char kInput[] = {0x8B, 0x81, 0x18, 0x05, 0x00, 0x00, 0x83, 0xE8, 0x00, 0x56, 0xBE, 0x02, 0x00, 0x00, 0x00, 0x74};
        unsigned char actual[sizeof(kInput)]{};
        const bool inputOk = Read(base + kSp01Input, actual, sizeof(actual)) && std::memcmp(actual, kInput, sizeof(kInput)) == 0;
        std::fprintf(log, "menu sig %-28s %08X %s\n", "SP01 HandleInput", static_cast<unsigned>(base + kSp01Input), inputOk ? "ok" : "MISMATCH (no join role on the row)");
        if (inputOk) sp01InputOk = SwapSlot(log, "SP01 HandleInput", kSp01InputSlot, kSp01Input, reinterpret_cast<void*>(OnSp01Input));
    }
    installed = ok;
    std::fprintf(log, "menu %s: main menu 'survival' -> Multiplayer screen -> map picker -> [MpMap] launch\n", installed ? "installed" : "NOT installed");
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    static LONG lastTitleLookups = -1;
    const LONG hits = InterlockedCompareExchange(&titleLookups, 0, 0);
    if (hits != lastTitleLookups) {
        std::fprintf(log, "menu main title lookups=%ld\n", hits);
        lastTitleLookups = hits;
    }
    std::fprintf(log, "menu co-op screen calls=%ld text_patches=%ld text_misses=%ld appended=%ld append_misses=%ld\n", InterlockedCompareExchange(&sp01Calls, 0, 0),
                 InterlockedCompareExchange(&notice::overridePatches, 0, 0), InterlockedCompareExchange(&notice::overrideMisses, 0, 0),
                 InterlockedCompareExchange(&notice::appendPatches, 0, 0), InterlockedCompareExchange(&notice::appendMisses, 0, 0));
    std::fprintf(log, "menu extra maps appended=%ld faults=%ld\n", InterlockedCompareExchange(&extraAdded, 0, 0), InterlockedCompareExchange(&extraFaults, 0, 0));
    std::fprintf(log, "menu installed=%d main_menu_calls=%ld multiplayer_calls=%ld map_picker_calls=%ld pushes=%ld swallowed=%ld launches=%ld refused=%ld faults=%ld pending=%ld\n",
                 installed ? 1 : 0, InterlockedCompareExchange(&pc01Calls, 0, 0), InterlockedCompareExchange(&fe03Calls, 0, 0),
                 InterlockedCompareExchange(&mp20Calls, 0, 0), InterlockedCompareExchange(&pushes, 0, 0), InterlockedCompareExchange(&swallowed, 0, 0),
                 InterlockedCompareExchange(&launches, 0, 0), InterlockedCompareExchange(&refused, 0, 0), InterlockedCompareExchange(&faults, 0, 0),
                 InterlockedCompareExchange(&launchPending, 0, 0));
    std::fprintf(log, "menu launches refused because a joiner lacks the map: %ld; Multiplayer screen inputs swallowed: %ld; survival not ready: %ld refused, %ld warned; "
                      "launcher client refusals: %ld\n",
                 InterlockedCompareExchange(&missingRefusals, 0, 0), InterlockedCompareExchange(&fe03InputSwallowed, 0, 0),
                 InterlockedCompareExchange(&survivalRefusals, 0, 0), InterlockedCompareExchange(&survivalWarnings, 0, 0),
                 InterlockedCompareExchange(&launcherMenuRefusals, 0, 0));
}
}  // namespace menu
