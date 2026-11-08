// Dead Space 2 United by Richard Hafer.
// Map pictures for the campaign arenas and the Outbreak maps ([Menu] MapPictures, on;
// analysis/ARENA_MAP_PICTURES_2026-10-16.md). The picker (MP20) and the lobby (MP02) pick
// their picture in ActionScript: gotoAndStop("map" + GetVar("mMapIndex")). mMapIndex is the
// list index (MP20 +0x22C, MP02 +0x240), so every arena past the five stock maps asked for a
// frame that did not exist. tools/build_map_pictures.py gives both movies one frame per arena
// labelled "map" + its name (maparena_ch03) and MP02 the Outbreak frames map5/map6; here the
// movie's mMapIndex variable is pointed at a string we own before each SetInfo:
//   AptSubscriberVariable at screen+0x10: entries [screen+0x14], count [screen+0x18], 0x18
//   bytes each: +0 name, +4 data, +8 type (AddVariable 0x0071B270). GetExternVariable
//   0x007197C0 reads type 3 and 6 as the string *(char**)data, type 0 as an int.
// The key is the arena's name, "5"/"6" for Concourse/Academy, else the list index as text,
// which is what the stock frames always got. MP20's SetInfo (0x00A4BA90, gamemode's hook)
// refreshes the movie itself, so the key is set before the original runs; MP02's SetInfo
// 0x00A49CF0 (thiscall(index), ret 4) is hooked here. Its first call comes from Init before
// the variables exist; the OnShow call binds.
namespace mappictures {
bool enabled = true, installed = false;
constexpr uint32_t kMp02SetInfoRva = 0x649CF0, kVariables = 0x14, kVariableCount = 0x18, kEntrySize = 0x18, kTypeString = 6;
// mov eax,[esp+4] / push esi / push edi / mov edi,[0x020213FC] / mov esi,ecx / mov [esi+0x240],eax
const unsigned char kMp02SetInfo[] = {0x8B, 0x44, 0x24, 0x04, 0x56, 0x57, 0x8B, 0x3D, 0xFC, 0x13, 0x02, 0x02, 0x8B, 0xF1, 0x89, 0x86, 0x40, 0x02, 0x00, 0x00};
safetyhook::InlineHook mp02Hook;

struct Screen {
    const char* name;       // the movie, for the frame scan and the log
    char key[48];
    char* keyPtr;           // what the bound variable points at
    uint32_t lastScreen = 0, aptScanned = 0;
    volatile LONG binds = 0, rebinds = 0, notFound = 0, keyChanges = 0, faults = 0;
    unsigned lines = 0;
};
Screen mp20{"mp20gamesettings", "", nullptr}, mp02{"mp02lobby", "", nullptr};

// The picture key of list entry `index`.
void KeyFor(uint32_t index, char* out, size_t size) {
    uint32_t dgm = 0, maps = 0, count = 0;
    char map[64]{};
    if (Read(base + menu::kGameManager, &dgm, 4) && dgm && Read(dgm + menu::kDgmMaps, &maps, 4) && Read(dgm + menu::kDgmMapCount, &count, 4) && maps &&
        index < count && menu::EntryMap(maps + index * menu::kMapEntrySize, map, sizeof(map))) {
        if (coop_arena::IsArenaName(map)) { std::snprintf(out, size, "%s", map); return; }
        if (_stricmp(map, "pdlc_mp_mall") == 0) { std::snprintf(out, size, "5"); return; }
        if (_stricmp(map, "pdlc_mp_school") == 0) { std::snprintf(out, size, "6"); return; }
    }
    std::snprintf(out, size, "%u", index);
}

// How many "map<arena>" labels the loaded movie holds, once per loaded copy.
void ScanFrames(Screen& s) {
    const gamemode::native::Slot slot = gamemode::native::FindSlot(s.name);
    if (slot.index < 0 || !slot.aptData || slot.aptData == s.aptScanned) return;
    s.aptScanned = slot.aptData;
    bool found[coop_arena::kArenaCount]{};
    constexpr size_t kPage = 0x1000, kKeep = 64;
    char buf[kPage + kKeep];
    size_t carry = 0;
    for (uint32_t done = 0; done < 0x200000; done += kPage) {
        if (!Read(slot.aptData + done, buf + carry, kPage)) break;
        const size_t have = carry + kPage;
        for (size_t i = 0; i + 9 <= have; ++i) {
            if (buf[i] != 'm' || std::memcmp(buf + i, "maparena_", 9) != 0) continue;
            for (unsigned a = 0; a < coop_arena::kArenaCount; ++a) {
                const char* n = coop_arena::kArenas[a].name;
                const size_t len = std::strlen(n);
                if (i + 3 + len < have && std::memcmp(buf + i + 3, n, len) == 0 && buf[i + 3 + len] == 0) found[a] = true;
            }
        }
        carry = kKeep;
        std::memmove(buf, buf + have - carry, carry);
    }
    unsigned present = 0;
    char missing[160] = "";
    size_t used = 0;
    for (unsigned a = 0; a < coop_arena::kArenaCount; ++a) {
        if (found[a]) { ++present; continue; }
        if (used < sizeof(missing) - 16) used += static_cast<size_t>(std::snprintf(missing + used, sizeof(missing) - used, " %s", coop_arena::kArenas[a].name));
    }
    mpmap::Event("mappictures %s: movie data %08X holds %u of %u arena frames%s%s", s.name, slot.aptData, present, coop_arena::kArenaCount,
                 present < coop_arena::kArenaCount ? "; missing:" : "", missing);
}

// Points the screen's mMapIndex at our key; true when it is bound.
bool Bind(uintptr_t screen, Screen& s) {
    uint32_t array = 0, count = 0;
    if (!Read(screen + kVariables, &array, 4) || !Read(screen + kVariableCount, &count, 4) || !array || count > 64) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t entry = array + i * kEntrySize;
        uint32_t namePtr = 0, data = 0, type = 0;
        char name[12]{};
        if (!Read(entry, &namePtr, 4) || !namePtr || !Read(namePtr, name, 10) || std::strcmp(name, "mMapIndex") != 0) continue;
        Read(entry + 4, &data, 4);
        Read(entry + 8, &type, 4);
        const uint32_t want = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s.keyPtr));
        if (data == want && type == kTypeString) return true;
        const bool ok = WriteBytes(entry + 4, &want, 4) && WriteBytes(entry + 8, &kTypeString, 4);
        InterlockedIncrement(ok ? (s.lastScreen == screen ? &s.rebinds : &s.binds) : &s.faults);
        mpmap::Event("mappictures %s: screen %08X variable %u of %u mMapIndex type %u data %08X -> string key%s", s.name, static_cast<unsigned>(screen), i, count, type, data,
                     ok ? "" : " FAILED");
        s.lastScreen = static_cast<uint32_t>(screen);
        if (ok) ScanFrames(s);
        return ok;
    }
    InterlockedIncrement(&s.notFound);
    return false;
}

void Before(uintptr_t screen, uint32_t index, Screen& s) {
    if (!enabled || !screen) return;
    __try {
        if (!s.keyPtr) s.keyPtr = s.key;
        char key[48];
        KeyFor(index, key, sizeof(key));
        if (std::strcmp(key, s.key) != 0) {
            std::snprintf(s.key, sizeof(s.key), "%s", key);
            InterlockedIncrement(&s.keyChanges);
            if (s.lines < 40) {
                ++s.lines;
                mpmap::Event("mappictures %s: row %u -> frame \"map%s\"", s.name, index, s.key);
            }
        }
        Bind(screen, s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&s.faults);
    }
}
void BeforeMp20(uint8_t* screen, uint32_t index) { Before(reinterpret_cast<uintptr_t>(screen), index, mp20); }
void __fastcall OnMp02SetInfo(void* screen, void*, uint32_t index) {
    Before(reinterpret_cast<uintptr_t>(screen), index, mp02);
    mp02Hook.thiscall<void>(screen, index);
}

void Install(FILE* log, const std::string& ini) {
    enabled = menu::installed && GetPrivateProfileIntA("Menu", "MapPictures", 1, ini.c_str()) != 0;
    if (!enabled) {
        std::fprintf(log, "mappictures off (%s)\n", menu::installed ? "[Menu] MapPictures=0" : "the menu module is not installed");
        return;
    }
    mp20.keyPtr = mp20.key;
    mp02.keyPtr = mp02.key;
    unsigned char have[sizeof(kMp02SetInfo)]{};
    const bool sigOk = Read(base + kMp02SetInfoRva, have, sizeof(have)) && std::memcmp(have, kMp02SetInfo, sizeof(have)) == 0;
    if (sigOk) mp02Hook = safetyhook::create_inline(reinterpret_cast<void*>(base + kMp02SetInfoRva), reinterpret_cast<void*>(OnMp02SetInfo));
    installed = true;
    std::fprintf(log, "mappictures on: picker through the game mode's SetInfo hook (%s), lobby SetInfo %08X %s; frames \"map<arena>\", Outbreak \"map5\"/\"map6\"\n",
                 gamemode::enabled ? "installed" : "NOT installed, no picker pictures", static_cast<unsigned>(base + kMp02SetInfoRva),
                 !sigOk ? "MISMATCH, no lobby pictures" : mp02Hook ? "hooked" : "hook FAILED");
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&mp20.binds, 0, 0) + InterlockedCompareExchange(&mp02.binds, 0, 0) * 7 +
                     InterlockedCompareExchange(&mp20.keyChanges, 0, 0) * 13 + InterlockedCompareExchange(&mp02.keyChanges, 0, 0) * 17 +
                     InterlockedCompareExchange(&mp20.faults, 0, 0) * 19 + InterlockedCompareExchange(&mp02.faults, 0, 0) * 23 +
                     InterlockedCompareExchange(&mp20.notFound, 0, 0) * 29 + InterlockedCompareExchange(&mp02.notFound, 0, 0) * 31;
    if (now == last) return;
    last = now;
    for (Screen* s : {&mp20, &mp02})
        std::fprintf(log, "mappictures %s binds=%ld rebinds=%ld not_found=%ld key_changes=%ld faults=%ld key='%s'\n", s->name, InterlockedCompareExchange(&s->binds, 0, 0),
                     InterlockedCompareExchange(&s->rebinds, 0, 0), InterlockedCompareExchange(&s->notFound, 0, 0), InterlockedCompareExchange(&s->keyChanges, 0, 0),
                     InterlockedCompareExchange(&s->faults, 0, 0), s->key);
}
}  // namespace mappictures
