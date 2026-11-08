// Dead Space 2 United by Richard Hafer.
// Level exit guard (15.10.). Every way out of a level, the main menu included,
// runs through the level manager's load states. Five of them (1, 2, 5, 7, 8)
// leave only on iMsgStreamLoadComplete with both stream queues empty (handler
// 0x0042DEC0, queue check 0x00748250). When a load reuses a copy that is still
// dispatched, as the menu does with text_assets_global after a held pool
// (analysis/TEXT_PACK_HOLD_2026-10-13.md), nothing loads, the message never
// comes and the screen stays black. The 14.10. nudge ran from the stream
// update, which the game only calls while it has streaming work, so it never
// ran in exactly that case (15.10., PID 52736: state 1, queues 0/0, nudges=0).
// This runs from the main loop's frame tick and covers every such state,
// whatever kept the copy alive. The handler checks the queues itself, so the
// message cannot skip a load that is still running.
//
// Last resort, from the worker: a load state that does not move for
// RelaunchAfterSeconds with idle queues, or a main loop that stops while the
// game is in front, writes a small dump and restarts the game.
namespace exit_guard {
constexpr uint32_t kLevelManager = 0x1C1C0A4, kStreamManager = 0x1C1C098, kWindow = 0x1C21F64, kLmState = 0x18;
constexpr uint32_t kHandler = 0x2DEC0, kMsgLoadComplete = 0x1C488A8;
// LOAD_CHECKPOINT (11) leaves on the checkpoint set's loading-completed message: LoadFromCheckpoint
// 0x0042DA00 links [0x0204D3A0] and activates the set, StreamSet::StreamSetLoadingCompleted sends
// it (0x00759491), HandleEvents 0x0042E0E5 unlinks it and runs CheckpointLoadComplete 0x0042D5D0
// (thunk 0x004014FB) without looking at the state.
constexpr uint32_t kMsgSetLoaded = 0x1C4D3A0, kLoadCheckpoint = 11, kSetManager = 0x1C1C0BC, kSetUnloadCount = 0xC8;
constexpr uint32_t kRunning = 16, kNoState = 0xFFFFFFFFu;

bool enabled = true, relaunch = true, installed = false;
unsigned nudgeAfterMs = 1500, maxNudges = 5, relaunchAfterS = 60, frozenS = 30, checkpointNudgeMs = 8000;
std::string pluginDirectory;

const char* Name(uint32_t s) {
    static const char* const names[] = {"NO_LEVEL", "LOAD_TEXT_ASSETS", "LOAD_LOADING_SCREEN", "FETCH_MP_TUNING", "SWAP_DISC",
                                        "LOAD_GLOBAL_SHARED", "LOAD_SHARED_TOC", "LOAD_SHARED", "LOAD_PLAYER_ANIMS",
                                        "LOAD_MPWEAPONSTREAMS", "LOAD_MPUNIFORMSTREAMS", "LOAD_CHECKPOINT", "LOADING_COMPLETE",
                                        "WAIT_FOR_REFLECTABLES", "LOAD_MPSKINSTREAMS", "WAIT_FOR_COMMAND", "RUNNING"};
    return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}
// The states the handler leaves on iMsgStreamLoadComplete.
bool WaitsForLoadComplete(uint32_t s) { return s == 1 || s == 2 || s == 5 || s == 7 || s == 8; }
// Load states that are stuck when nothing streams. Not the Blaze waits (3, 13), not
// WAIT_FOR_COMMAND (15: "press a key" and the co-op start barrier), not RUNNING.
bool LoadState(uint32_t s) { return s >= 1 && s <= 14 && s != 3 && s != 13; }

bool Peek(uint32_t address, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(address));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool Queues(uint32_t* loads, uint32_t* unloads) {
    uint32_t sm = 0;
    return Peek(static_cast<uint32_t>(base) + kStreamManager, &sm) && sm && Peek(sm + 0x7C, loads) && Peek(sm + 0x84, unloads);
}

// Game-thread events go through a small ring; the worker writes them.
SRWLOCK ringLock = SRWLOCK_INIT;
char ring[32][224];
unsigned ringHead = 0, ringTail = 0;
volatile LONG ringDropped = 0;
void Event(const char* format, ...) {
    char text[224];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    AcquireSRWLockExclusive(&ringLock);
    if (ringHead - ringTail < 32) std::memcpy(ring[ringHead++ % 32], text, sizeof(text));
    else InterlockedIncrement(&ringDropped);
    ReleaseSRWLockExclusive(&ringLock);
}
void Drain(FILE* log) {
    char text[224];
    for (;;) {
        AcquireSRWLockExclusive(&ringLock);
        const bool any = ringTail != ringHead;
        if (any) std::memcpy(text, ring[ringTail++ % 32], sizeof(text));
        ReleaseSRWLockExclusive(&ringLock);
        if (!any) return;
        if (log) std::fprintf(log, "exit_guard %s\n", text);
    }
}

// --- game thread: the frame tick ---------------------------------------------------------------
volatile LONG ticks = 0, transitions = 0, nudges = 0, nudgesAdvanced = 0, nudgeFaults = 0, badCode = 0, audits = 0;
volatile LONG checkpointNudges = 0, checkpointAdvanced = 0;
uint32_t seenLm = 0, seenState = kNoState;
DWORD stateSince = 0, idleSince = 0, lastNudge = 0;
unsigned nudgesHere = 0;
int handlerOk = -1;   // -1 unchecked

bool HandlerOk() {
    if (handlerOk < 0) {
        static const uint8_t expected[] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x00, 0x83, 0xEC, 0x08, 0x56, 0x57};
        uint8_t code[sizeof(expected)]{};
        handlerOk = Read(base + kHandler, code, sizeof(code)) && !std::memcmp(code, expected, sizeof(code)) ? 1 : 0;
    }
    return handlerOk == 1;
}
bool SendLoadComplete(uint32_t lm, uint32_t id) {
    uint32_t message[8]{};
    message[0] = id;
    using Handler = void(__thiscall*)(void*, void*);
    __try {
        reinterpret_cast<Handler>(base + kHandler)(reinterpret_cast<void*>(static_cast<uintptr_t>(lm)), message);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// What the level left behind, once the next one runs: our pool and text streams.
void Audit() {
    uint32_t manager = 0;
    if (!Read(base + kStreamManager, &manager, 4) || !manager) return;
    const auto states = ds2::native::inspectStreamStates(manager, Read);
    unsigned ours = 0, text = 0;
    char names[120] = "";
    size_t used = 0;
    for (const auto& s : states.entries) {
        char name[96];
        if (!text_packs::StreamName(s, name, sizeof(name))) continue;
        if (!std::strncmp(name, "text_assets\\", 12)) ++text;
        if (std::strncmp(name, "randomizer\\", 11)) continue;
        if (++ours <= 2 && used < sizeof(names) - 40)
            used += static_cast<size_t>(std::snprintf(names + used, sizeof(names) - used, " %s(status %u)", name, s.status));
    }
    InterlockedIncrement(&audits);
    Event("running: %zu streams, %u text, %u from randomizer\\%s (snapshot %d)", states.entries.size(), text, ours, names,
          static_cast<int>(states.result));
}

void CheckpointNudge(uint32_t lm, uint32_t state, DWORD now);   // below
void FrameTick() {
    InterlockedIncrement(&ticks);
    if (!enabled) return;
    uint32_t lm = 0, state = kNoState;
    if (!Peek(static_cast<uint32_t>(base) + kLevelManager, &lm) || !lm || !Peek(lm + kLmState, &state)) { seenLm = 0; seenState = kNoState; return; }
    const DWORD now = GetTickCount();
    if (lm != seenLm || state != seenState) {
        uint32_t loads = 0, unloads = 0;
        Queues(&loads, &unloads);
        if (seenState != kNoState && InterlockedIncrement(&transitions) <= 800)
            Event("lm %s(%u) -> %s(%u) after %lu ms, queues %u/%u%s", Name(seenState), seenState, Name(state), state,
                  static_cast<unsigned long>(now - stateSince), loads, unloads, lm != seenLm ? ", new level manager" : "");
        seenLm = lm;
        seenState = state;
        stateSince = now;
        idleSince = lastNudge = 0;
        nudgesHere = 0;
        if (state == kRunning) Audit();
        return;
    }
    if (state == kLoadCheckpoint) { CheckpointNudge(lm, state, now); return; }
    if (!WaitsForLoadComplete(state) || nudgesHere >= maxNudges) return;
    uint32_t loads = 1, unloads = 1;
    if (!Queues(&loads, &unloads)) return;
    if (loads || unloads) { idleSince = 0; return; }
    if (!idleSince) idleSince = now;
    if (now - idleSince < nudgeAfterMs || (lastNudge && now - lastNudge < nudgeAfterMs)) return;
    lastNudge = now;
    ++nudgesHere;
    uint32_t id = 0;
    if (!HandlerOk() || !Peek(static_cast<uint32_t>(base) + kMsgLoadComplete, &id) || !id) {
        InterlockedIncrement(&badCode);
        nudgesHere = maxNudges;
        Event("nudge skipped in %s(%u): handler bytes %s, message id %08X", Name(state), state, HandlerOk() ? "ok" : "CHANGED", id);
        return;
    }
    const bool ok = SendLoadComplete(lm, id);
    InterlockedIncrement(ok ? &nudges : &nudgeFaults);
    uint32_t after = kNoState;
    Peek(lm + kLmState, &after);
    if (after != state) InterlockedIncrement(&nudgesAdvanced);
    Event("nudge %u/%u: %s(%u) for %lu ms, queues idle %lu ms, sent iMsgStreamLoadComplete %08X: %s, state now %s(%u)", nudgesHere,
          maxNudges, Name(state), state, static_cast<unsigned long>(now - stateSince), static_cast<unsigned long>(now - idleSince), id,
          ok ? "ok" : "FAULT", Name(after), after);
}

// What the checkpoint load waits for, before the nudge (16.10.: three debug-menu chapter jumps
// from a level sat in LOAD_CHECKPOINT with idle queues and every sampled stream at status 3).
void LogCheckpointWait() {
    uint32_t manager = 0, setManager = 0, sets = 0, zone = 0, unloads = 0;
    if (Read(base + kSetManager, &setManager, 4) && setManager && Read(setManager + 0x10, &sets, 4) && sets && Read(sets, &zone, 4) && zone)
        Read(zone + kSetUnloadCount, &unloads, 4);
    if (!Read(base + kStreamManager, &manager, 4) || !manager) return;
    const auto states = ds2::native::inspectStreamStates(manager, Read);
    unsigned byStatus[6]{}, other = 0, notShown = 0;
    char names[150] = "";
    size_t used = 0;
    for (const auto& st : states.entries) {
        if (st.status < 6) ++byStatus[st.status]; else ++other;
        if (st.status == 3 || notShown >= 3) continue;
        char name[96];
        if (!text_packs::StreamName(st, name, sizeof(name))) std::snprintf(name, sizeof(name), "handle %08X", st.handle);
        if (used < sizeof(names) - 48) used += static_cast<size_t>(std::snprintf(names + used, sizeof(names) - used, " %.40s(%u)", name, st.status));
        ++notShown;
    }
    Event("checkpoint wait: %zu streams (status 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u other:%u), zone set %08X unload list %u, not loaded:%s", states.entries.size(),
          byStatus[0], byStatus[1], byStatus[2], byStatus[3], byStatus[4], byStatus[5], other, zone, unloads, names[0] ? names : " none");
}
// After CheckpointNudgeSeconds in LOAD_CHECKPOINT with idle queues the loading-completed message
// goes to the level manager (twice at most); a normal checkpoint load takes under a second. The
// restart after RestartAfterSeconds stays the last resort.
void CheckpointNudge(uint32_t lm, uint32_t state, DWORD now) {
    if (!checkpointNudgeMs || nudgesHere >= 2) return;
    uint32_t loads = 1, unloads = 1;
    if (!Queues(&loads, &unloads)) return;
    if (loads || unloads) { idleSince = 0; return; }
    if (!idleSince) idleSince = now;
    if (now - stateSince < checkpointNudgeMs || now - idleSince < 2000 || (lastNudge && now - lastNudge < checkpointNudgeMs)) return;
    lastNudge = now;
    ++nudgesHere;
    uint32_t id = 0;
    if (!HandlerOk() || !Peek(static_cast<uint32_t>(base) + kMsgSetLoaded, &id) || !id) {
        InterlockedIncrement(&badCode);
        nudgesHere = 2;
        Event("checkpoint nudge skipped: handler bytes %s, message id %08X", HandlerOk() ? "ok" : "CHANGED", id);
        return;
    }
    LogCheckpointWait();
    const bool ok = SendLoadComplete(lm, id);
    InterlockedIncrement(ok ? &checkpointNudges : &nudgeFaults);
    uint32_t after = kNoState;
    Peek(lm + kLmState, &after);
    if (after != state) InterlockedIncrement(&checkpointAdvanced);
    Event("checkpoint nudge %u/2: %s(%u) for %lu ms, queues idle %lu ms, sent the set-loaded message %08X: %s, state now %s(%u)", nudgesHere, Name(state), state,
          static_cast<unsigned long>(now - stateSince), static_cast<unsigned long>(now - idleSince), id, ok ? "ok" : "FAULT", Name(after), after);
}

// --- worker: the last resort -----------------------------------------------------------------
volatile LONG relaunching = 0;
uint32_t pollLm = 0, pollState = kNoState;
DWORD stuckSince = 0, ticksAt = 0;
LONG lastTicks = -1;
bool warned = false;

bool GameInFront() {
    uint32_t window = 0;
    if (!Read(base + kWindow, &window, 4) || !window) return false;
    const HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(window));
    if (IsIconic(hwnd)) return false;
    // a window that stopped pumping is shown through a ghost window that belongs to dwm
    return GetForegroundWindow() == hwnd || IsHungAppWindow(hwnd);
}

void WriteSmallDump(FILE* log) {
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    const auto write = dbghelp ? reinterpret_cast<WriteDump>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    if (!write) return;
    char path[MAX_PATH];
    const bool folder = GetFileAttributesA("C:\\DS2CrashDumps") != INVALID_FILE_ATTRIBUTES;
    std::snprintf(path, sizeof(path), "%sdeadspace2.stuck.%lu.dmp", folder ? "C:\\DS2CrashDumps\\" : pluginDirectory.c_str(),
                  static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    // threads with stacks plus every module's data segment: the level manager and the stream manager globals
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, 0x1000 | 0x1, nullptr, nullptr, nullptr);
    CloseHandle(file);
    if (log) std::fprintf(log, "exit_guard dump %s: %s\n", path, ok ? "written" : "FAILED");
}

// The game's own command line again, after the process is gone. steam_appid.txt
// lets the exe start without Steam's relaunch (the launchers write it too).
bool StartAgain(FILE* log) {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    std::wstring folder(exe);
    folder.resize(folder.find_last_of(L"\\/") + 1);
    const std::wstring appid = folder + L"steam_appid.txt";
    if (GetFileAttributesW(appid.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(appid.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(f, "47780", 5, &written, nullptr);
            CloseHandle(f);
        }
    }
    const wchar_t* tail = GetCommandLineW();
    if (*tail == L'"') {
        for (++tail; *tail && *tail != L'"'; ++tail) {}
        if (*tail) ++tail;
    } else {
        while (*tail && *tail != L' ') ++tail;
    }
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(system) + L"\\cmd.exe\" /d /c ping -n 4 127.0.0.1 >nul & start \"\" \"" + exe + L"\"" + tail;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(command.begin(), command.end());
    line.push_back(0);
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr,
                             folder.c_str(), &si, &pi);
    if (!ok) ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, folder.c_str(), &si, &pi);
    if (!ok) {
        if (log) std::fprintf(log, "exit_guard restart FAILED: CreateProcess error %lu; the game stays as it is\n", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void Relaunch(FILE* log, const char* reason) {
    if (InterlockedExchange(&relaunching, 1)) return;
    if (log) {
        std::fprintf(log, "exit_guard RESTART: %s; nudges=%ld advanced=%ld transitions=%ld\n", reason, InterlockedCompareExchange(&nudges, 0, 0),
                     InterlockedCompareExchange(&nudgesAdvanced, 0, 0), InterlockedCompareExchange(&transitions, 0, 0));
        std::fflush(log);
    }
    WriteSmallDump(log);
    if (FILE* marker = std::fopen((pluginDirectory + "DS2United.restarted.txt").c_str(), "w")) {
        SYSTEMTIME t{};
        GetLocalTime(&t);
        std::fprintf(marker, "%02u.%02u. %02u:%02u:%02u pid %lu: %s\n", t.wDay, t.wMonth, t.wHour, t.wMinute, t.wSecond,
                     static_cast<unsigned long>(GetCurrentProcessId()), reason);
        std::fclose(marker);
    }
    if (!StartAgain(log)) { InterlockedExchange(&relaunching, 0); relaunch = false; return; }
    if (log) std::fprintf(log, "exit_guard restart started; ending this process\n");
    _flushall();
    TerminateProcess(GetCurrentProcess(), 0xE0D5);
}

void Poll(FILE* log) {
    Drain(log);
    if (!installed || !enabled || !relaunch || InterlockedCompareExchange(&relaunching, 0, 0)) return;
    const DWORD now = GetTickCount();
    uint32_t lm = 0, state = kNoState, loads = 1, unloads = 1;
    const bool read = Read(base + kLevelManager, &lm, 4) && lm && Read(lm + kLmState, &state, 4) && Queues(&loads, &unloads);
    // a) a load state that does not move while nothing streams
    if (read && LoadState(state) && lm == pollLm && state == pollState && !loads && !unloads) {
        if (!stuckSince) stuckSince = now;
        const DWORD stuck = now - stuckSince;
        if (!warned && stuck >= relaunchAfterS * 500) {
            warned = true;
            if (log) std::fprintf(log, "exit_guard warning: %s(%u) has not moved for %lu s with idle queues; restart at %u s\n", Name(state), state,
                                  static_cast<unsigned long>(stuck / 1000), relaunchAfterS);
        }
        if (relaunchAfterS && stuck >= relaunchAfterS * 1000) {
            char reason[160];
            std::snprintf(reason, sizeof(reason), "level manager %s(%u) did not move for %lu s with idle stream queues", Name(state), state,
                          static_cast<unsigned long>(stuck / 1000));
            Relaunch(log, reason);
            return;
        }
    } else {
        stuckSince = 0;
        warned = false;
        pollLm = lm;
        pollState = state;
    }
    // b) the main loop stopped
    const LONG t = InterlockedCompareExchange(&ticks, 0, 0);
    if (t != lastTicks) { lastTicks = t; ticksAt = now; return; }
    if (!frozenS || t <= 0 || now - ticksAt < frozenS * 1000 || IsDebuggerPresent() || !GameInFront()) return;
    char reason[160];
    std::snprintf(reason, sizeof(reason), "the main loop has not run a frame for %lu s (level manager %s(%u))",
                  static_cast<unsigned long>((now - ticksAt) / 1000), Name(state), state);
    Relaunch(log, reason);
}

void Install(FILE* log, const std::string& ini, const std::string& directory, bool frameTick) {
    pluginDirectory = directory;
    enabled = ConfigInt(ini, "ExitGuard", "Enabled", 1, 0, 1) != 0;
    nudgeAfterMs = static_cast<unsigned>(ConfigInt(ini, "ExitGuard", "NudgeAfterMs", 1500, 500, 10000));
    maxNudges = static_cast<unsigned>(ConfigInt(ini, "ExitGuard", "MaxNudges", 5, 1, 20));
    relaunch = ConfigInt(ini, "ExitGuard", "Restart", 1, 0, 1) != 0;
    relaunchAfterS = static_cast<unsigned>(ConfigInt(ini, "ExitGuard", "RestartAfterSeconds", 60, 20, 600));
    frozenS = static_cast<unsigned>(ConfigInt(ini, "ExitGuard", "FrozenSeconds", 30, 0, 600));
    checkpointNudgeMs = static_cast<unsigned>(ConfigInt(ini, "ExitGuard", "CheckpointNudgeSeconds", 8, 0, 50)) * 1000u;
    if (frozenS && frozenS < 10) frozenS = 10;
    installed = true;
    std::fprintf(log, "exit_guard %s: frame_tick=%d handler=%s nudge_after=%u ms max_nudges=%u checkpoint_nudge_after=%u s restart=%d after=%u s frozen=%u s\n",
                 enabled ? "on" : "off ([ExitGuard] Enabled=0)", frameTick ? 1 : 0, HandlerOk() ? "ok" : "CHANGED (no nudges)", nudgeAfterMs,
                 maxNudges, checkpointNudgeMs / 1000u, relaunch ? 1 : 0, relaunchAfterS, frozenS);
    if (!frameTick) std::fprintf(log, "exit_guard: no frame tick (frame_cap hook missing), so no nudges and no frozen check\n");
    const std::string marker = directory + "DS2United.restarted.txt";
    if (FILE* f = std::fopen(marker.c_str(), "r")) {
        char text[256] = "";
        if (!std::fgets(text, sizeof(text), f)) text[0] = 0;
        std::fclose(f);
        std::remove(marker.c_str());
        text[std::strcspn(text, "\r\n")] = 0;
        std::fprintf(log, "exit_guard: the previous session was restarted by the guard: %s\n", text);
    }
}

void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG sum = InterlockedCompareExchange(&transitions, 0, 0) + InterlockedCompareExchange(&nudges, 0, 0) * 1000 +
                     InterlockedCompareExchange(&checkpointNudges, 0, 0) * 3 + InterlockedCompareExchange(&checkpointAdvanced, 0, 0) * 5 +
                     InterlockedCompareExchange(&badCode, 0, 0) * 7 + InterlockedCompareExchange(&nudgeFaults, 0, 0) * 13 +
                     InterlockedCompareExchange(&ringDropped, 0, 0) * 17;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "exit_guard summary ticks=%ld transitions=%ld nudges=%ld advanced=%ld checkpoint_nudges=%ld checkpoint_advanced=%ld faults=%ld bad_code=%ld "
                      "audits=%ld dropped_lines=%ld\n",
                 InterlockedCompareExchange(&ticks, 0, 0), InterlockedCompareExchange(&transitions, 0, 0), InterlockedCompareExchange(&nudges, 0, 0),
                 InterlockedCompareExchange(&nudgesAdvanced, 0, 0), InterlockedCompareExchange(&checkpointNudges, 0, 0),
                 InterlockedCompareExchange(&checkpointAdvanced, 0, 0), InterlockedCompareExchange(&nudgeFaults, 0, 0),
                 InterlockedCompareExchange(&badCode, 0, 0), InterlockedCompareExchange(&audits, 0, 0),
                 InterlockedCompareExchange(&ringDropped, 0, 0));
}
}  // namespace exit_guard
