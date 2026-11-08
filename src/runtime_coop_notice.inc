// Dead Space 2 United by Hafer.
// On-screen notices through the RIG announcement, the "RIPPER Schematic
// Acquired" banner.
//
// A level's RASAnnouncement node (vtable 0x01B72B3C) has two attributes,
// CMD_SetText (a text HASH, e.g. 0xD8EEBB9C) and MSG_RcvPlayRAS. Its
// HandleEvents (0x010CE730) does nothing but
//     hud = *(Player + 0xFB8) - 0x10        (SafePtr to the RigAnnouncementSystem)
//     hud->Queue(0, textHash, 0)            0x010CE1E0, thiscall, ret 0xC
// and the RIG resolves the hash through the localized text table when it
// draws. That table ("LCH2", live 2026-09-17: 7403 entries) is a header
// {magic, ?, ?, ?, count +0x10, ?, hashes +0x18, strings +0x1C} over two
// parallel arrays: sorted hashes and char* pointers. It cannot grow, so a
// custom text borrows an entry nobody will miss (an export placeholder text,
// hash 0x0B768C07) by pointing its string pointer at a buffer in this plugin
// and queueing that hash. The table is a streamed resource, so the pointer is
// re-checked before every notice and rewritten when a reload replaced it.
//
// Unlike the redirector's MSGS channel (login screen, once, through Blaze),
// this reaches the RIG any time, through nothing but the game.
namespace notice {
bool enabled = false, installed = false;
constexpr uint32_t kQueueRva = 0xCCE1E0;         // 0x010CE1E0
constexpr uint32_t kPlayerHudOffset = 0xFB8;
constexpr uint32_t kMagic = 0x3248434C;           // 'LCH2'
uint32_t slotHash = 0x0B768C07;
volatile LONG table = 0;                          // header of the text table, 0 until found (scan thread)
volatile LONG scanWanted = 0;
char buffer[200] = "";
volatile LONG shown = 0, scans = 0, notFound = 0, faults = 0, patched = 0;

bool TableValid(uint32_t t) {
    uint32_t magic = 0, count = 0, hashes = 0, strings = 0;
    return t && Read(t, &magic, 4) && magic == kMagic && Read(t + 0x10, &count, 4) && count > 100 && count < 100000 &&
           Read(t + 0x18, &hashes, 4) && Read(t + 0x1C, &strings, 4) && hashes && strings;
}

// The header lives on the heap and is found by its magic. Walking every RW
// region took the game thread for a noticeable moment, so the walk runs on a
// thread of its own (reads only): while in a level or asked for, at most once
// per ten seconds after a miss.
uint32_t ScanForTable() {
    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t at = 0x10000;
    // In 64 KB pieces: a whole heap region (up to 512 MB in a 32-bit game) in
    // one buffer could fail to allocate.
    static unsigned char chunk[0x10000];
    while (at < 0x7FFF0000 && VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi))) {
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t next = regionBase + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize < 0x20000000) {
            for (uintptr_t piece = regionBase; piece < next; piece += sizeof(chunk)) {
                const size_t want = static_cast<size_t>(next - piece < sizeof(chunk) ? next - piece : sizeof(chunk));
                SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(piece), chunk, want, &got) || got != want) break;
                for (size_t i = 0; i + 4 <= got; i += 4) {
                    if (*reinterpret_cast<uint32_t*>(&chunk[i]) != kMagic) continue;
                    const uint32_t candidate = static_cast<uint32_t>(piece + i);
                    if (TableValid(candidate)) return candidate;
                }
            }
        }
        at = next;
    }
    return 0;
}
DWORD WINAPI ScanThread(void*) {
    DWORD lastMiss = 0;
    for (;;) {
        Sleep(500);
        if (TableValid(static_cast<uint32_t>(InterlockedCompareExchange(&table, 0, 0)))) continue;
        if (!inLevel && !InterlockedCompareExchange(&scanWanted, 0, 0)) continue;
        const DWORD now = GetTickCount();
        if (lastMiss && now - lastMiss < 10000) continue;
        InterlockedIncrement(&scans);
        const uint32_t found = ScanForTable();
        InterlockedExchange(&table, static_cast<LONG>(found));
        InterlockedExchange(&scanWanted, 0);
        if (found) lastMiss = 0;
        else { lastMiss = now; InterlockedIncrement(&notFound); }
    }
}
// Game thread: the known table, or 0 (and the scan thread is asked to look).
uint32_t FindTable() {
    const uint32_t t = static_cast<uint32_t>(InterlockedCompareExchange(&table, 0, 0));
    if (TableValid(t)) return t;
    InterlockedExchange(&scanWanted, 1);
    return 0;
}

// Point the entry for hash at text. Binary search over the sorted hashes.
bool PatchHash(uint32_t t, uint32_t hash, const char* text) {
    uint32_t count = 0, hashes = 0, strings = 0;
    Read(t + 0x10, &count, 4); Read(t + 0x18, &hashes, 4); Read(t + 0x1C, &strings, 4);
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2, h = [&] { uint32_t v = 0; Read(hashes + mid * 4, &v, 4); return v; }();
        if (h < hash) lo = mid + 1; else hi = mid;
    }
    uint32_t h = 0;
    if (lo >= count || !Read(hashes + lo * 4, &h, 4) || h != hash) return false;
    uint32_t current = 0;
    Read(strings + lo * 4, &current, 4);
    const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(text));
    return current == mine || WriteBytes(strings + lo * 4, &mine, 4);
}
// Game texts replaced for good (the main menu's "single player" becomes
// "co-op"). Applied again and again: the table is rebuilt on text reloads.
struct TextOverride { uint32_t hash; const char* text; };
TextOverride overrides[24]{};
unsigned overrideCount = 0;
volatile LONG overridePatches = 0, overrideMisses = 0;
void AddOverride(uint32_t hash, const char* text) {
    if (overrideCount < 24) overrides[overrideCount++] = TextOverride{hash, text};
}
// Game texts that keep their words and get ours after them (the title screen's
// legal line). The game's string is read again whenever the entry is not ours,
// so a rebuilt table still gets the original text in front.
struct TextAppend { uint32_t hash; const char* suffix; char text[240]; };
TextAppend appends[2]{};
unsigned appendCount = 0;
volatile LONG appendPatches = 0, appendMisses = 0;
void AddAppend(uint32_t hash, const char* suffix) {
    if (appendCount < 2) { appends[appendCount].hash = hash; appends[appendCount].suffix = suffix; ++appendCount; }
}
bool PatchAppend(uint32_t t, TextAppend& a) {
    uint32_t count = 0, hashes = 0, strings = 0;
    Read(t + 0x10, &count, 4); Read(t + 0x18, &hashes, 4); Read(t + 0x1C, &strings, 4);
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2, h = [&] { uint32_t v = 0; Read(hashes + mid * 4, &v, 4); return v; }();
        if (h < a.hash) lo = mid + 1; else hi = mid;
    }
    uint32_t h = 0, current = 0;
    if (lo >= count || !Read(hashes + lo * 4, &h, 4) || h != a.hash || !Read(strings + lo * 4, &current, 4) || !current) return false;
    const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a.text));
    if (current == mine) return true;
    char original[160] = "";
    if (!Read(current, original, sizeof(original) - 1)) return false;
    original[sizeof(original) - 1] = 0;
    std::snprintf(a.text, sizeof(a.text), "%s%s", original, a.suffix);
    if (!WriteBytes(strings + lo * 4, &mine, 4)) return false;
    gate::Event("coop_notice text %08X now '%s'", a.hash, a.text);
    return true;
}
void ApplyOverrides() {
    if (!overrideCount && !appendCount) return;
    const uint32_t t = FindTable();
    if (!t) return;
    for (unsigned i = 0; i < overrideCount; ++i) {
        if (PatchHash(t, overrides[i].hash, overrides[i].text)) InterlockedIncrement(&overridePatches);
        else InterlockedIncrement(&overrideMisses);
    }
    for (unsigned i = 0; i < appendCount; ++i) {
        if (PatchAppend(t, appends[i])) InterlockedIncrement(&appendPatches);
        else InterlockedIncrement(&appendMisses);
    }
}
// Point the borrowed entry at the local buffer. Binary search over the sorted hashes.
bool Patch() {
    const uint32_t t = FindTable();
    if (!t) return false;
    uint32_t count = 0, hashes = 0, strings = 0;
    Read(t + 0x10, &count, 4); Read(t + 0x18, &hashes, 4); Read(t + 0x1C, &strings, 4);
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2, h = [&] { uint32_t v = 0; Read(hashes + mid * 4, &v, 4); return v; }();
        if (h < slotHash) lo = mid + 1; else hi = mid;
    }
    uint32_t h = 0;
    if (lo >= count || !Read(hashes + lo * 4, &h, 4) || h != slotHash) return false;
    uint32_t current = 0;
    Read(strings + lo * 4, &current, 4);
    const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buffer));
    if (current == mine) return true;
    if (!WriteBytes(strings + lo * 4, &mine, 4)) return false;
    InterlockedIncrement(&patched);
    return true;
}

int Filter(unsigned) { InterlockedIncrement(&faults); return EXCEPTION_EXECUTE_HANDLER; }
void Queue(uint32_t hud) {
    using QueueFn = void(__thiscall*)(void*, int, uint32_t, int);
    __try {
        reinterpret_cast<QueueFn>(base + kQueueRva)(reinterpret_cast<void*>(hud), 0, slotHash, 0);
    } __except (Filter(GetExceptionCode())) {
    }
}

// Game thread. Returns false when there is nobody to show it to yet.
bool Show(const char* text) {
    if (!installed || !enabled) return false;
    const uint32_t player = LocalCampaignPlayer();
    uint32_t hudPtr = 0;
    if (!player || !Read(player + kPlayerHudOffset, &hudPtr, 4) || !hudPtr) return false;
    const uint32_t hud = hudPtr - 0x10;
    std::snprintf(buffer, sizeof(buffer), "%s", text);
    if (!Patch()) return false;
    Queue(hud);
    InterlockedIncrement(&shown);
    gate::Event("coop_notice shown: %s", buffer);
    return true;
}

// Notices from other plugins (the randomizer's survival director), through
// DS2Coop_QueueNotice in coop_plugin.cpp. Any thread may queue; the game
// thread shows one at a time, QueueGapMs apart, because the RIG reads the
// borrowed text when it draws and a second notice queued too early would
// overwrite the first one's words. A full queue drops its oldest entry.
constexpr unsigned kQueued = 6;
char queued[kQueued][sizeof(buffer)]{};
unsigned queuedHead = 0, queuedCount = 0;
SRWLOCK queuedLock = SRWLOCK_INIT;
DWORD queueGapMs = 2500, lastQueuedShown = 0;
volatile LONG queuedIn = 0, queuedShown = 0, queuedDropped = 0;
bool Enqueue(const char* text) {
    if (!installed || !enabled || !text || !text[0]) return false;
    AcquireSRWLockExclusive(&queuedLock);
    if (queuedCount == kQueued) {
        queuedHead = (queuedHead + 1) % kQueued;
        --queuedCount;
        InterlockedIncrement(&queuedDropped);
    }
    std::snprintf(queued[(queuedHead + queuedCount) % kQueued], sizeof(buffer), "%s", text);
    ++queuedCount;
    ReleaseSRWLockExclusive(&queuedLock);
    InterlockedIncrement(&queuedIn);
    return true;
}
// Game thread, from coop::Poll.
void DrainQueued(DWORD now) {
    if (!installed || !enabled || now - lastQueuedShown < queueGapMs) return;
    char text[sizeof(buffer)]{};
    AcquireSRWLockExclusive(&queuedLock);
    if (queuedCount) std::memcpy(text, queued[queuedHead], sizeof(text));
    ReleaseSRWLockExclusive(&queuedLock);
    if (!text[0] || !Show(text)) return;   // nobody to show it to yet: kept for the next frame
    AcquireSRWLockExclusive(&queuedLock);
    if (queuedCount) { queuedHead = (queuedHead + 1) % kQueued; --queuedCount; }
    ReleaseSRWLockExclusive(&queuedLock);
    lastQueuedShown = now;
    InterlockedIncrement(&queuedShown);
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Notice", "Enabled", 1, ini.c_str()) != 0;
    queueGapMs = static_cast<DWORD>(GetPrivateProfileIntA("Notice", "QueueGapMs", 2500, ini.c_str()));
    if (queueGapMs < 1000) queueGapMs = 1000;
    char slot[32]{};
    GetPrivateProfileStringA("Notice", "SlotHash", "0B768C07", slot, sizeof(slot), ini.c_str());
    slotHash = static_cast<uint32_t>(std::strtoul(slot, nullptr, 16));
    // sub esp,0x18 / mov eax,[esp+0x24] / push ebx / xor ebx,ebx / push esi / mov esi,ecx
    static const unsigned char kSig[] = {0x83,0xEC,0x18,0x8B,0x44,0x24,0x24,0x53,0x33,0xDB,0x56,0x8B,0xF1};
    unsigned char actual[sizeof(kSig)]{};
    if (!Read(base + kQueueRva, actual, sizeof(kSig)) || std::memcmp(actual, kSig, sizeof(kSig)) != 0) {
        std::fprintf(log, "coop_notice RIG queue signature mismatch at %08X; notices off\n", static_cast<unsigned>(base + kQueueRva));
        return false;
    }
    installed = true;
    bool scanner = false;
    if (enabled) {
        if (HANDLE thread = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr)) { CloseHandle(thread); scanner = true; }
    }
    std::fprintf(log, "coop_notice enabled=%d queue=%08X slot_hash=%08X table_scan_thread=%d export_queue_gap_ms=%lu\n", enabled,
                 static_cast<unsigned>(base + kQueueRva), slotHash, scanner ? 1 : 0, static_cast<unsigned long>(queueGapMs));
    return true;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&shown, 0, 0) + InterlockedCompareExchange(&scans, 0, 0) + InterlockedCompareExchange(&faults, 0, 0) +
                     InterlockedCompareExchange(&queuedIn, 0, 0) + InterlockedCompareExchange(&queuedDropped, 0, 0);
    if (now == last) return;
    last = now;
    std::fprintf(log, "coop_notice shown=%ld scans=%ld not_found=%ld patched=%ld faults=%ld table=%08X exported queued=%ld shown=%ld dropped=%ld\n",
                 InterlockedCompareExchange(&shown, 0, 0), InterlockedCompareExchange(&scans, 0, 0),
                 InterlockedCompareExchange(&notFound, 0, 0), InterlockedCompareExchange(&patched, 0, 0),
                 InterlockedCompareExchange(&faults, 0, 0), static_cast<uint32_t>(InterlockedCompareExchange(&table, 0, 0)),
                 InterlockedCompareExchange(&queuedIn, 0, 0), InterlockedCompareExchange(&queuedShown, 0, 0),
                 InterlockedCompareExchange(&queuedDropped, 0, 0));
}
}  // namespace notice
