// Dead Space 2 United by Richard Hafer.
// Side logs next to DS2United.log, each with the N-th copy's number as DS2United.N.log
// and the last run kept as .prev.log:
//   DS2Versus.log  the menu lobby, the game's lobby screen, the Versus roster and
//                  match, the map's phase changes copied in, and during a match
//                  the necro module's notes and the respawn lines;
//   DS2Bots.log    every bot line: install, events and counters.
struct SideLog {
    FILE* file = nullptr;
    SRWLOCK lock = SRWLOCK_INIT;
    DWORD startTick = 0, flushedAt = 0, flushMs = 0;
    volatile LONG written = 0, dropped = 0;
    LONG maxLines = 60000;
    void Open(const std::string& directory, const char* name, unsigned index, const char* what) {
        const std::string suffix = index ? "." + std::to_string(index) : "";
        const std::string path = directory + "\\" + name + suffix + ".log";
        log_rotation::Rotate(directory + "\\" + name + suffix);   // .prev, .prev2, .prev3
        file = _fsopen(path.c_str(), "w", _SH_DENYWR);
        startTick = GetTickCount();
        if (!file) return;
        SYSTEMTIME st{};
        GetLocalTime(&st);
        std::fprintf(file, "%s log %04u-%02u-%02u %02u:%02u:%02u, copy %u: %s (DS2United%s.log has the rest)\n", name, st.wYear, st.wMonth, st.wDay, st.wHour,
                     st.wMinute, st.wSecond, index, what, suffix.c_str());
        std::fflush(file);
    }
    void WriteText(const char* tag, const char* text) {
        if (!file) return;
        if (InterlockedIncrement(&written) > maxLines) { InterlockedIncrement(&dropped); return; }
        SYSTEMTIME st{};
        GetLocalTime(&st);
        const DWORD now = GetTickCount(), since = now - startTick;
        AcquireSRWLockExclusive(&lock);
        std::fprintf(file, "%02u:%02u:%02u.%03u t=%lu.%03lu [%s] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, since / 1000, since % 1000, tag, text);
        // a chatty log is flushed at most every flushMs; a crash then loses that much
        if (!flushMs || now - flushedAt >= flushMs) { std::fflush(file); flushedAt = now; }
        ReleaseSRWLockExclusive(&lock);
    }
    void Flush() {
        if (!file) return;
        AcquireSRWLockExclusive(&lock);
        std::fflush(file);
        flushedAt = GetTickCount();
        ReleaseSRWLockExclusive(&lock);
    }
};

namespace vlog {
SideLog side;
void Open(const std::string& directory, unsigned index) {
    side.Open(directory, "DS2Versus", index, "menu lobby, lobby screen, Versus roster and match");
}
// The diagnostics' counters go here too when the file is open.
FILE* Or(FILE* fallback) { return side.file ? side.file : fallback; }
void WriteText(const char* tag, const char* text) { side.WriteText(tag, text); }
void Write(const char* tag, const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    WriteText(tag, text);
}
// A main-log line the lobby log wants as well (the map's phases, the menu's picks).
void Mirror(const char* text) {
    static const char* const kPrefixes[] = {"lobby: ", "menu: ", "phase ", "arrived ", "=== visit", "way back", "gamemode: ", "loader ", "spawn player", "WATCHDOG",
                                            "HANG", "  zone set"};
    for (const char* p : kPrefixes) {
        const char* at = std::strstr(text, p);
        if (at && at - text < 48) { WriteText("map", text); return; }
    }
}
// While a Versus match runs, the necro module's notes and the respawn lines too.
volatile LONG matchOn = 0;
void MirrorIfMatch(const char* tag, const char* text) {
    if (InterlockedCompareExchange(&matchOn, 0, 0)) WriteText(tag, text);
}
}  // namespace vlog

namespace botlog {
SideLog side;
void Open(const std::string& directory, unsigned index) {
    side.flushMs = 500;
    side.maxLines = 200000;
    side.Open(directory, "DS2Bots", index, "the companion bots");
}
FILE* Or(FILE* fallback) { return side.file ? side.file : fallback; }
// An event line of the bots ("bot ...", "bot_aim: ...", "botnet ..."): written here
// instead of the main log. False when the file is not open or the line is not theirs.
bool Take(const char* text) {
    if (!side.file || std::strncmp(text, "bot", 3) != 0) return false;
    const char c = text[3];
    if (c != ' ' && c != ':' && c != '_' && c != 'n') return false;
    side.WriteText("bot", text);
    return true;
}
}  // namespace botlog
