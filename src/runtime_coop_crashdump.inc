// Dead Space 2 United by Richard Hafer.
// Crash dumps ([Coop] CrashDumps). 12.10.: the client crashed at
// deadspace2+0xFB44D7 during a followed scene and nothing was left but the
// Windows event (no WER dump, no log line). An unhandled exception now writes
// a full dump and a short text file next to it (C:\DS2CrashDumps when that
// folder exists, else the game folder), then goes on to the previous filter.
#include <dbghelp.h>
namespace crashdump {
bool enabled = false;
LPTOP_LEVEL_EXCEPTION_FILTER previous = nullptr;
std::string logToKeep;   // set by coop_plugin.cpp once the log is open
FILE* logStream = nullptr;
volatile LONG inside = 0, written = 0, reclaimed = 0;

// Our own logs are open deny-write; a copy has to share writing to read them.
bool CopyShared(const char* from, const char* to) {
    const HANDLE in = CreateFileA(from, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) return false;
    const HANDLE out = CreateFileA(to, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = out != INVALID_HANDLE_VALUE;
    static char chunk[0x10000];
    while (ok) {
        DWORD got = 0, done = 0;
        if (!ReadFile(in, chunk, sizeof(chunk), &got, nullptr)) { ok = false; break; }
        if (!got) break;
        ok = WriteFile(out, chunk, got, &done, nullptr) && done == got;
    }
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    CloseHandle(in);
    return ok;
}
void Folder(char* out, size_t size) {
    std::snprintf(out, size, "%s", GetFileAttributesA("C:\\DS2CrashDumps") != INVALID_FILE_ATTRIBUTES ? "C:\\DS2CrashDumps" : ".");
}
void WriteNote(const char* path, EXCEPTION_POINTERS* info) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char text[512];
    const EXCEPTION_RECORD* r = info ? info->ExceptionRecord : nullptr;
    const CONTEXT* c = info ? info->ContextRecord : nullptr;
    const int n = std::snprintf(text, sizeof(text),
        "code=%08lX address=%p (exe+%X) access=%lu target=%p\r\n"
        "eip=%08lX esp=%08lX ebp=%08lX eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX thread=%lu tick=%lu\r\n",
        r ? r->ExceptionCode : 0, r ? r->ExceptionAddress : nullptr,
        r ? static_cast<unsigned>(reinterpret_cast<uintptr_t>(r->ExceptionAddress) - base) : 0u,
        r && r->NumberParameters >= 2 ? static_cast<unsigned long>(r->ExceptionInformation[0]) : 0ul,
        r && r->NumberParameters >= 2 ? reinterpret_cast<void*>(r->ExceptionInformation[1]) : nullptr,
        c ? c->Eip : 0, c ? c->Esp : 0, c ? c->Ebp : 0, c ? c->Eax : 0, c ? c->Ebx : 0, c ? c->Ecx : 0, c ? c->Edx : 0,
        c ? c->Esi : 0, c ? c->Edi : 0, static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long>(GetTickCount()));
    DWORD done = 0;
    if (n > 0) WriteFile(file, text, static_cast<DWORD>(n), &done, nullptr);
    CloseHandle(file);
}
LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* info) {
    // Another filter that chains back to ours must not loop.
    if (InterlockedExchange(&inside, 1)) return EXCEPTION_CONTINUE_SEARCH;
    if (!InterlockedExchange(&written, 1)) {
        char folder[MAX_PATH], path[MAX_PATH];
        Folder(folder, sizeof(folder));
        std::snprintf(path, sizeof(path), "%s\\deadspace2.crash.%lu.txt", folder, static_cast<unsigned long>(GetCurrentProcessId()));
        WriteNote(path, info);
        if (!logToKeep.empty()) {
            if (logStream) std::fflush(logStream);
            std::snprintf(path, sizeof(path), "%s\\deadspace2.crash.%lu.log", folder, static_cast<unsigned long>(GetCurrentProcessId()));
            CopyShared(logToKeep.c_str(), path);
        }
        HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
        using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, void*, void*);
        const auto write = dbghelp ? reinterpret_cast<WriteDump>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
        std::snprintf(path, sizeof(path), "%s\\deadspace2.crash.%lu.dmp", folder, static_cast<unsigned long>(GetCurrentProcessId()));
        HANDLE file = write ? CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
            write(GetCurrentProcess(), GetCurrentProcessId(), file,
                  static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                  info ? &exception : nullptr, nullptr, nullptr);
            CloseHandle(file);
        }
    }
    const LONG result = previous ? previous(info) : EXCEPTION_CONTINUE_SEARCH;
    InterlockedExchange(&inside, 0);
    return result;
}
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "CrashDumps", 0, ini.c_str()) != 0;
    if (enabled) previous = SetUnhandledExceptionFilter(OnUnhandled);
    char folder[MAX_PATH];
    Folder(folder, sizeof(folder));
    std::fprintf(log, "coop_crashdump enabled=%d folder=%s (an unhandled exception writes deadspace2.crash.<pid>.dmp, .txt and a copy of this log)\n", enabled ? 1 : 0,
                 folder);
}
// From the diagnostics thread: a filter set later by somebody else goes behind ours.
void Keep() {
    if (!enabled || InterlockedCompareExchange(&inside, 0, 0)) return;
    const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(OnUnhandled);
    if (current == OnUnhandled) return;
    previous = current;
    InterlockedIncrement(&reclaimed);
}
void WriteDiagnostics(FILE* log) {
    Keep();
    if (enabled && InterlockedCompareExchange(&reclaimed, 0, 0))
        std::fprintf(log, "coop_crashdump filter_reclaimed=%ld (another module had set its own; it is chained behind ours)\n",
                     InterlockedCompareExchange(&reclaimed, 0, 0));
}
}  // namespace crashdump
