// Dead Space 2 United by Hafer.
// The bug key ([Coop] BugReportKey, Ctrl+F12 by default). A press saves what a report
// needs into one folder: C:\DS2CrashDumps\bug_<time>_<id>_m<machine>_<pid> (else
// plugins\DS2BugReports): the logs (all kept runs), the INIs, a screenshot, a small
// dump with every thread and a note (role, map, position, peers). The partners get
// 'BUGR' and save their own folder under the same id. The key is read on a thread of
// its own, so it also works while the game thread hangs.
namespace bugreport {
constexpr uint32_t kMagic = 0x52475542u;   // 'BUGR'
#pragma pack(push, 1)
struct Packet { uint32_t magic, token, id; };
#pragma pack(pop)
static_assert(sizeof(Packet) == 12, "bug report packet size");
bool enabled = true, ctrl = true, fullDump = false;
int key = VK_F12;
std::string pluginFolder;
volatile LONG wantedId = 0, wantedFrom = -1, pressed = 0, requestsIn = 0, written = 0, failures = 0, sentPackets = 0, copiedFiles = 0, copyFailures = 0,
              shots = 0, dumps = 0, dumpFailures = 0, flushTimeouts = 0;
uint32_t lastIds[8]{};
unsigned lastIdNext = 0;
// The game thread's picture, refreshed by Tick: the report thread does not touch game objects.
struct Snapshot { DWORD at = 0; int role = 0; bool hasPos = false; float pos[3]{}; char map[64]{}, stream[64]{}; };
Snapshot snap;
SRWLOCK snapLock = SRWLOCK_INIT;
char lastFolder[MAX_PATH] = "";

void Tick(DWORD now) {
    if (!enabled) return;
    static DWORD last = 0;
    if (now - last < 250) return;
    last = now;
    Snapshot s;
    s.at = now;
    s.role = necro::LocalRole();
    s.hasPos = gate::LocalPosition(s.pos);
    std::memcpy(s.map, level::reportMap, sizeof(s.map) - 1);
    std::memcpy(s.stream, level::reportStream, sizeof(s.stream) - 1);
    AcquireSRWLockExclusive(&snapLock);
    snap = s;
    ReleaseSRWLockExclusive(&snapLock);
}

// The game thread may hold the log's lock while it hangs: the flush gets a second, no more.
DWORD WINAPI FlushThread(void*) {
    if (crashdump::logStream) std::fflush(crashdump::logStream);
    return 0;
}
void FlushLog() {
    const HANDLE thread = CreateThread(nullptr, 0, FlushThread, nullptr, 0, nullptr);
    if (!thread) return;
    if (WaitForSingleObject(thread, 1000) != WAIT_OBJECT_0) InterlockedIncrement(&flushTimeouts);
    CloseHandle(thread);
}
void CopyMatching(const char* pattern, const char* folder) {
    WIN32_FIND_DATAA found{};
    const HANDLE find = FindFirstFileA((pluginFolder + pattern).c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char to[MAX_PATH];
        std::snprintf(to, sizeof(to), "%s\\%s", folder, found.cFileName);
        InterlockedIncrement(crashdump::CopyShared((pluginFolder + found.cFileName).c_str(), to) ? &copiedFiles : &copyFailures);
    } while (FindNextFileA(find, &found));
    FindClose(find);
}

struct WindowPick { HWND hwnd = nullptr; LONG area = 0; };
BOOL CALLBACK PickWindow(HWND hwnd, LPARAM data) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    RECT r{};
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &r)) return TRUE;
    auto& pick = *reinterpret_cast<WindowPick*>(data);
    const LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > pick.area) { pick.hwnd = hwnd; pick.area = area; }
    return TRUE;
}
// From the screen, never through the window: a hung game thread would not answer WM_PRINT.
bool Screenshot(const char* path) {
    WindowPick pick;
    EnumWindows(PickWindow, reinterpret_cast<LPARAM>(&pick));
    RECT r{};
    if (!pick.hwnd || !GetWindowRect(pick.hwnd, &r)) return false;
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader = BITMAPINFOHEADER{sizeof(BITMAPINFOHEADER), w, h, 1, 24, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bitmap && memory) {
        const HGDIOBJ old = SelectObject(memory, bitmap);
        ok = BitBlt(memory, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT) != 0;
        SelectObject(memory, old);
        GdiFlush();
    }
    if (ok) {
        const DWORD stride = (static_cast<DWORD>(w) * 3 + 3) & ~3u, size = stride * static_cast<DWORD>(h);
        BITMAPFILEHEADER head{0x4D42, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + size, 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
        const HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD done = 0;
        ok = file != INVALID_HANDLE_VALUE && WriteFile(file, &head, sizeof(head), &done, nullptr) &&
             WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &done, nullptr) && WriteFile(file, bits, size, &done, nullptr);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return ok;
}
// Every thread's stack and the game's data, no heap (a full dump with BugReportFullDump=1).
bool Dump(const char* path) {
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, void*, void*);
    const auto write = dbghelp ? reinterpret_cast<WriteDump>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    const HANDLE file = write ? CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    if (file == INVALID_HANDLE_VALUE) return false;
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        fullDump ? MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules
                 : MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const bool ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, type, nullptr, nullptr, nullptr) != 0;
    CloseHandle(file);
    return ok;
}
void WriteNote(const char* path, uint32_t id, long from) {
    const HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    AcquireSRWLockShared(&snapLock);
    const Snapshot s = snap;
    ReleaseSRWLockShared(&snapLock);
    const DWORD now = GetTickCount();
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::string text;
    char line[400];
    std::snprintf(line, sizeof(line), "bug report %08X\r\npressed_by=%s machine=%u pid=%lu host=%d\r\ntime=%04u-%02u-%02u %02u:%02u:%02u tick=%lu\r\n", id,
                  from < 0 ? "this machine" : "a partner", from < 0 ? machineIndex : static_cast<unsigned>(from), static_cast<unsigned long>(GetCurrentProcessId()),
                  IsHost() ? 1 : 0, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, static_cast<unsigned long>(now));
    text += line;
    // A game thread that stopped ticking is the first thing a hang report has to say.
    std::snprintf(line, sizeof(line), "game_thread_silent_ms=%lu role=%d map=%s stream=%s pos=", s.at ? static_cast<unsigned long>(now - s.at) : 0ul, s.role,
                  s.map[0] ? s.map : "-", s.stream[0] ? s.stream : "-");
    text += line;
    if (s.hasPos) std::snprintf(line, sizeof(line), "(%.2f %.2f %.2f)\r\n", s.pos[0], s.pos[1], s.pos[2]);
    else std::snprintf(line, sizeof(line), "-\r\n");
    text += line;
    std::snprintf(line, sizeof(line), "peers=%u log=%s\r\n", peerTable.Count(), crashdump::logToKeep.c_str());
    text += line;
    for (unsigned slot = 0; slot < coop_peers::kMaxPeers; ++slot) {
        const auto& p = peerTable.peers[slot];
        if (!p.used) continue;
        std::snprintf(line, sizeof(line), "peer slot=%u machine=%u token=%08X role=%d silent_ms=%lu relayed=%u\r\n", slot, p.machine, p.token,
                      roles::RoleOf(slot, now), static_cast<unsigned long>(now - p.lastSeen), p.relayed);
        text += line;
    }
    DWORD done = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &done, nullptr);
    CloseHandle(file);
}
void Write(uint32_t id, long from) {
    char root[MAX_PATH], folder[MAX_PATH], path[MAX_PATH];
    if (GetFileAttributesA("C:\\DS2CrashDumps") != INVALID_FILE_ATTRIBUTES) {
        std::snprintf(root, sizeof(root), "C:\\DS2CrashDumps");
    } else {
        std::snprintf(root, sizeof(root), "%sDS2BugReports", pluginFolder.c_str());
        CreateDirectoryA(root, nullptr);
    }
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::snprintf(folder, sizeof(folder), "%s\\bug_%04u%02u%02u-%02u%02u%02u_%08X_m%u_%lu", root, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, id,
                  machineIndex, static_cast<unsigned long>(GetCurrentProcessId()));
    if (!CreateDirectoryA(folder, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) { InterlockedIncrement(&failures); return; }
    std::snprintf(path, sizeof(path), "%s\\note.txt", folder);
    WriteNote(path, id, from);
    std::snprintf(path, sizeof(path), "%s\\screen.bmp", folder);
    if (Screenshot(path)) InterlockedIncrement(&shots);
    FlushLog();
    for (const char* pattern : {"DS2United*.log", "DS2Versus*.log", "DS2Bots*.log", "DS2Randomizer*.log", "*.ini"}) CopyMatching(pattern, folder);
    std::snprintf(path, sizeof(path), "%s\\deadspace2.bug.%lu.dmp", folder, static_cast<unsigned long>(GetCurrentProcessId()));
    InterlockedIncrement(Dump(path) ? &dumps : &dumpFailures);
    std::snprintf(lastFolder, sizeof(lastFolder), "%s", folder);
    InterlockedIncrement(&written);
    gate::Event("bug report %08X saved to %s (pressed %s)", id, folder, from < 0 ? "here" : "by a partner");
    notice::Enqueue(from < 0 ? "BUG REPORT SAVED" : "BUG REPORT SAVED (PARTNER)");
}
bool Seen(uint32_t id) {
    for (uint32_t last : lastIds) if (last == id) return true;
    lastIds[lastIdNext++ % 8] = id;
    return false;
}
DWORD WINAPI Worker(void*) {
    bool down = false;
    DWORD lastPress = 0;
    for (;;) {
        Sleep(50);
        const DWORD now = GetTickCount();
        const bool held = necro::GameWindowFocused() && (GetAsyncKeyState(key) & 0x8000) != 0 && (!ctrl || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
        if (held && !down && now - lastPress > 10000) {
            lastPress = now;
            InterlockedIncrement(&pressed);
            const uint32_t id = (now * 2654435761u) ^ (GetCurrentProcessId() << 8) ^ machineIndex;
            Seen(id);
            // three times, the side channel drops packets
            for (int i = 0; i < 3 && network && peerReadiness.token; ++i) {
                const Packet p{kMagic, peerReadiness.token, id};
                if (SendToPeers(&p, sizeof(p)) > 0) InterlockedIncrement(&sentPackets);
                if (i < 2) Sleep(100);
            }
            Write(id, -1);
        }
        down = held;
        const LONG remote = InterlockedExchange(&wantedId, 0);
        if (remote && !Seen(static_cast<uint32_t>(remote))) Write(static_cast<uint32_t>(remote), InterlockedCompareExchange(&wantedFrom, 0, 0));
    }
}
// Network thread: a partner pressed the key.
bool OnBytes(const void* data, int size) {
    if (size != static_cast<int>(sizeof(Packet))) return false;
    Packet p{};
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != kMagic || !p.id) return false;
    const coop_peers::Peer* sender = peerTable.FindByToken(p.token);
    if (!sender) return false;
    InterlockedIncrement(&requestsIn);
    if (!enabled) return true;
    InterlockedExchange(&wantedFrom, static_cast<LONG>(sender->machine));
    InterlockedExchange(&wantedId, static_cast<LONG>(p.id));
    return true;
}
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "BugReport", 1, ini.c_str()) != 0;
    key = GetPrivateProfileIntA("Coop", "BugReportKey", VK_F12, ini.c_str());
    ctrl = GetPrivateProfileIntA("Coop", "BugReportCtrl", 1, ini.c_str()) != 0;
    fullDump = GetPrivateProfileIntA("Coop", "BugReportFullDump", 0, ini.c_str()) != 0;
    const size_t slash = ini.find_last_of("\\/");
    pluginFolder = slash == std::string::npos ? std::string() : ini.substr(0, slash + 1);
    bool started = false;
    if (enabled && key > 0 && key < 256)
        if (HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) { CloseHandle(thread); started = true; }
    std::fprintf(log, "coop_bugreport enabled=%d key=%d ctrl=%d full_dump=%d thread=%d folder=%s (a press saves logs, INIs, a screenshot and a dump; partners save theirs)\n",
                 enabled ? 1 : 0, key, ctrl ? 1 : 0, fullDump ? 1 : 0, started ? 1 : 0,
                 GetFileAttributesA("C:\\DS2CrashDumps") != INVALID_FILE_ATTRIBUTES ? "C:\\DS2CrashDumps" : "plugins\\DS2BugReports");
}
void WriteDiagnostics(FILE* log) {
    const LONG now = InterlockedCompareExchange(&pressed, 0, 0) + InterlockedCompareExchange(&requestsIn, 0, 0) + InterlockedCompareExchange(&written, 0, 0);
    static LONG last = 0;
    if (!enabled || now == last) return;
    last = now;
    std::fprintf(log, "coop_bugreport pressed=%ld partner_requests=%ld written=%ld failures=%ld packets_sent=%ld files=%ld copy_failures=%ld shots=%ld dumps=%ld "
                      "dump_failures=%ld flush_timeouts=%ld last=%s\n",
                 InterlockedCompareExchange(&pressed, 0, 0), InterlockedCompareExchange(&requestsIn, 0, 0), InterlockedCompareExchange(&written, 0, 0),
                 InterlockedCompareExchange(&failures, 0, 0), InterlockedCompareExchange(&sentPackets, 0, 0), InterlockedCompareExchange(&copiedFiles, 0, 0),
                 InterlockedCompareExchange(&copyFailures, 0, 0), InterlockedCompareExchange(&shots, 0, 0), InterlockedCompareExchange(&dumps, 0, 0),
                 InterlockedCompareExchange(&dumpFailures, 0, 0), InterlockedCompareExchange(&flushTimeouts, 0, 0), lastFolder[0] ? lastFolder : "-");
}
}  // namespace bugreport
