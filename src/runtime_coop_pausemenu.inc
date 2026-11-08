// Dead Space 2 United by Hafer.
// The pause menu's rows (SP10PauseMenu, the menu of the campaign and of the MP maps
// as we play them: 0 checkpoint restart, 1 settings, 2 tutorials, 3 friends list,
// 4 quit; OnInput_Select 0x00A98D90 switches on the row). Tutorials and the friends
// list are always locked, and the checkpoint restart is the host's alone: a
// partner's restart reloads every machine. A locked row gets the state the game
// gives the restart row on Hardcore (DoUpdateItemAndLabel 0x00A69210, thiscall(item,
// data, row, state) ret 0x10: [item] = 0x01B2EB6C selected, 0x01B2EB60 not), and its select
// does nothing, as the friends list's does offline. (15.10.: the row is the third argument;
// with the second the rows never locked, only the select was refused.)
int NetRole();   // runtime_coop.inc
namespace pausemenu {
bool enabled = true, installed = false;
constexpr uint32_t kUpdateRva = 0x669210, kSelectRva = 0x698D90;
constexpr uint32_t kLockedActiveRva = 0x172EB6C, kLockedRva = 0x172EB60;
constexpr uint32_t kIterator = 0x5C, kIteratorInactive = 0x2C, kIteratorRow = 0x28;
constexpr int kRestart = 0, kTutorials = 2, kFriends = 3;
// A living necro gets "suicide" in place of the friends list (user, 17.10.). The row's label is the
// text key at 0x01F60164[3], "$ui_viewfriends_mc" (DoUpdateItemAndLabel puts it at item+4); its
// entry in the text table (sdbm of the key without "$") points at our word while he is one.
constexpr uint32_t kTextViewFriends = 0xFF1A4390;
const char kSuicideLabel[] = "suicide";
uint32_t friendsOriginal = 0;
volatile LONG suicideLabels = 0, suicideSelects = 0;
void FriendsLabel(bool suicide) {
    const uint32_t t = notice::FindTable();
    if (!t) return;
    uint32_t count = 0, hashes = 0, strings = 0;
    Read(t + 0x10, &count, 4); Read(t + 0x18, &hashes, 4); Read(t + 0x1C, &strings, 4);
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        uint32_t h = 0;
        Read(hashes + mid * 4, &h, 4);
        if (h < kTextViewFriends) lo = mid + 1; else hi = mid;
    }
    uint32_t h = 0, current = 0;
    if (lo >= count || !Read(hashes + lo * 4, &h, 4) || h != kTextViewFriends || !Read(strings + lo * 4, &current, 4)) return;
    const uint32_t ours = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(kSuicideLabel));
    if (suicide && current != ours) {
        friendsOriginal = current;
        if (WriteBytes(strings + lo * 4, &ours, 4)) InterlockedIncrement(&suicideLabels);
    } else if (!suicide && current == ours && friendsOriginal) {
        WriteBytes(strings + lo * 4, &friendsOriginal, 4);
    }
}
// mov eax,[esp+0Ch] / push ebx / mov ebx,[esp+8] / push esi / push edi / mov edi,ecx / mov ecx,[eax*4+...]
const unsigned char kUpdateBytes[] = {0x8B, 0x44, 0x24, 0x0C, 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x0C, 0x85};
// mov eax,[esp+4] / push ebx / xor bl,bl / push esi / mov esi,ecx / cmp eax,4 / ja
const unsigned char kSelectBytes[] = {0x8B, 0x44, 0x24, 0x04, 0x53, 0x32, 0xDB, 0x56, 0x8B, 0xF1, 0x83, 0xF8, 0x04, 0x0F, 0x87, 0x9D};
safetyhook::InlineHook updateHook, selectHook;
volatile LONG rowsLocked = 0, restartRowsLocked = 0, selectsRefused = 0, restartsRefused = 0, restartsPassed = 0;

bool Locked(int row) { return row == kTutorials || (row == kFriends && !necro::SuicideAvailable()) || (row == kRestart && NetRole() == 2); }
int ActiveRow(uint32_t screen) {
    uint8_t inactive = 1;
    int32_t row = -1;
    if (!Read(screen + kIterator + kIteratorInactive, &inactive, 1) || inactive == 1 || !Read(screen + kIterator + kIteratorRow, &row, 4)) return -1;
    return row;
}
void __fastcall OnUpdate(void* screen, void*, uint32_t* item, void* data, int row, uint32_t state) {
    if (row == kFriends) FriendsLabel(necro::SuicideRow());
    updateHook.thiscall<void>(screen, item, data, row, state);
    if (!item || !Locked(row)) return;
    const uint32_t s = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(screen));
    item[0] = base + (ActiveRow(s) == row ? kLockedActiveRva : kLockedRva);
    InterlockedIncrement(row == kRestart ? &restartRowsLocked : &rowsLocked);
}
bool __fastcall OnSelect(void* screen, void*, int row) {
    if (row == kFriends && necro::SuicideAvailable()) {
        InterlockedIncrement(&suicideSelects);
        necro::CommitSuicide();
        return true;
    }
    if (Locked(row)) {
        InterlockedIncrement(row == kRestart ? &restartsRefused : &selectsRefused);
        return false;
    }
    if (row == kRestart) InterlockedIncrement(&restartsPassed);
    return selectHook.thiscall<bool>(screen, row);
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "PauseMenuLocks", 1, ini.c_str()) != 0;
    if (!enabled) {
        std::fprintf(log, "pause menu locks off ([Coop] PauseMenuLocks=0)\n");
        return;
    }
    const bool update = hook_site::Install(log, "pause menu rows (SP10 DoUpdateItemAndLabel)", base + kUpdateRva, kUpdateBytes, sizeof(kUpdateBytes),
                                           reinterpret_cast<void*>(OnUpdate), updateHook);
    const bool select = update && hook_site::Install(log, "pause menu select (SP10 OnInput_Select)", base + kSelectRva, kSelectBytes, sizeof(kSelectBytes),
                                                     reinterpret_cast<void*>(OnSelect), selectHook);
    if (update && !select) updateHook = {};
    installed = update && select;
    std::fprintf(log, "pause menu locks %s: tutorials and friends list locked, checkpoint restart for the host and solo only\n", installed ? "on" : "NOT installed");
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG sum = rowsLocked + restartRowsLocked * 3 + selectsRefused * 5 + restartsRefused * 7 + restartsPassed * 11;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "pause menu rows locked=%ld restart rows locked (partner)=%ld | selects refused=%ld restarts refused=%ld restarts passed=%ld | role %d | "
                      "suicide labels=%ld selects=%ld\n",
                 rowsLocked, restartRowsLocked, selectsRefused, restartsRefused, restartsPassed, NetRole(), suicideLabels, suicideSelects);
}
}  // namespace pausemenu
