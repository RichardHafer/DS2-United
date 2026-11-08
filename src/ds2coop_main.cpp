// Dead Space 2 United by Hafer.
// DS2United.asi: the randomizer (observer.cpp) and the co-op plugin
// (coop_plugin.cpp) in one module. Each half keeps its own worker thread, log
// and settings section; built with DS2COOP_MODULE, their lookups of each
// other's exports go to this module instead of the separate builds.
#include <windows.h>
#include <cstdio>
#include <cstring>

extern "C" BOOL WINAPI DS2Coop_RandomizerMain(HINSTANCE module, DWORD reason, LPVOID reserved);
extern "C" BOOL WINAPI DS2Coop_CoopMain(HINSTANCE module, DWORD reason, LPVOID reserved);

namespace {
// An update can leave the old DS2Coop.asi (or HaferSprawl.asi) next to DS2United.asi.
// Both halves twice in one game break everything, so only the first copy runs.
bool active = false;
HANDLE onePerProcess = nullptr;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        char name[64];
        std::snprintf(name, sizeof(name), "Local\\DS2United.%lu", GetCurrentProcessId());
        onePerProcess = CreateMutexA(nullptr, FALSE, name);
        active = onePerProcess && GetLastError() != ERROR_ALREADY_EXISTS;
        // the builds before the rename know nothing of the mutex and load first (A-Z)
        const char* old = GetModuleHandleA("DS2Coop.asi") ? "DS2Coop.asi" : GetModuleHandleA("HaferSprawl.asi") ? "HaferSprawl.asi" : nullptr;
        if (active && old) {
            active = false;
            char path[MAX_PATH];
            const DWORD n = GetModuleFileNameA(module, path, MAX_PATH);
            if (n && n < MAX_PATH) {
                char* slash = std::strrchr(path, '\\');
                if (slash) {
                    std::snprintf(slash + 1, static_cast<size_t>(MAX_PATH - (slash + 1 - path)), "DS2United.log");
                    FILE* f = nullptr;
                    if (fopen_s(&f, path, "w") == 0 && f) {
                        std::fprintf(f, "DS2United.asi stays idle: the old %s is loaded too. Delete %s from plugins and start the game again.\n", old, old);
                        std::fclose(f);
                    }
                }
            }
        }
        if (!active) {
            if (onePerProcess) CloseHandle(onePerProcess);
            onePerProcess = nullptr;
            OutputDebugStringA("DS2United: another copy of the plugin is already loaded, this one stays idle\n");
            return TRUE;
        }
    }
    if (!active) return TRUE;
    DS2Coop_RandomizerMain(module, reason, reserved);
    DS2Coop_CoopMain(module, reason, reserved);
    return TRUE;
}
