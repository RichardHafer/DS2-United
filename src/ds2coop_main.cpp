// Dead Space 2 United by Richard Hafer.
// DS2United.asi: the randomizer (observer.cpp) and the co-op plugin
// (coop_plugin.cpp) in one module. Each half keeps its own worker thread, log
// and settings section; built with DS2COOP_MODULE, their lookups of each
// other's exports go to this module instead of the separate builds.
#include <windows.h>
#include <cstdio>
#include <algorithm>
#include <cstring>

extern "C" BOOL WINAPI DS2Coop_RandomizerMain(HINSTANCE module, DWORD reason, LPVOID reserved);
extern "C" BOOL WINAPI DS2Coop_CoopMain(HINSTANCE module, DWORD reason, LPVOID reserved);

namespace {
// An update can leave the old DS2Coop.asi (or HaferSprawl.asi) next to DS2United.asi.
// Both halves twice in one game break everything, so only the first copy runs.
bool active = false;
HANDLE onePerProcess = nullptr;

// United runs on its own edition of MarkerPatch (dinput8.dll with the export below). The official
// one hooks the suit functions and retimes the main loop before any plugin loads, so with it the
// mod stays off. Found as the game folder's dinput8.dll or as an .asi beside a plain ASI loader.
bool HasText(HMODULE module, const char* text) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(module) + dos->e_lfanew);
    const char* image = reinterpret_cast<const char*>(module);
    const size_t size = nt->OptionalHeader.SizeOfImage, n = std::strlen(text);
    MEMORY_BASIC_INFORMATION mbi{};
    for (size_t at = 0; at < size;) {
        if (!VirtualQuery(image + at, &mbi, sizeof(mbi)) || !mbi.RegionSize) break;
        const size_t end = (std::min)(size, at + static_cast<size_t>(mbi.RegionSize));
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (size_t i = at; i + n <= end; ++i)
                if (image[i] == text[0] && std::memcmp(image + i, text, n) == 0) return true;
        at = end;
    }
    return false;
}
const char* OfficialMarkerPatch(char* path, DWORD size) {
    if (HMODULE asi = GetModuleHandleA("MarkerPatch.asi")) {
        GetModuleFileNameA(asi, path, size);
        return "MarkerPatch.asi";
    }
    HMODULE proxy = GetModuleHandleA("dinput8.dll");
    if (!proxy || GetProcAddress(proxy, "MarkerPatchUnitedEdition")) return nullptr;
    char game[MAX_PATH];
    const DWORD g = GetModuleFileNameA(nullptr, game, MAX_PATH), p = GetModuleFileNameA(proxy, path, size);
    char* gs = g && g < MAX_PATH ? std::strrchr(game, '\\') : nullptr;
    const char* ps = p && p < size ? std::strrchr(path, '\\') : nullptr;
    if (!gs || !ps || gs - game != ps - path || _strnicmp(game, path, static_cast<size_t>(gs - game)) != 0) return nullptr;   // the system's own dinput8
    return HasText(proxy, "MarkerPatch.ini") || HasText(proxy, "MARKERPATCH_LAA_RESTART") ? "dinput8.dll" : nullptr;
}
void WriteIdleLog(HINSTANCE module, const char* text) {
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(module, path, MAX_PATH);
    char* slash = n && n < MAX_PATH ? std::strrchr(path, '\\') : nullptr;
    if (!slash) return;
    std::snprintf(slash + 1, static_cast<size_t>(MAX_PATH - (slash + 1 - path)), "DS2United.log");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") == 0 && f) {
        std::fprintf(f, "%s\n", text);
        std::fclose(f);
    }
}
char idleText[700];
DWORD WINAPI ShowIdleBox(LPVOID) {
    MessageBoxA(nullptr, idleText, "Dead Space 2 United", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    return 0;
}
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
            char text[300];
            std::snprintf(text, sizeof(text), "DS2United.asi stays idle: the old %s is loaded too. Delete %s from plugins and start the game again.", old, old);
            WriteIdleLog(module, text);
        }
        char marker[MAX_PATH] = "";
        const char* official = active ? OfficialMarkerPatch(marker, MAX_PATH) : nullptr;
        if (official) {
            active = false;
            std::snprintf(idleText, sizeof(idleText),
                          "Dead Space 2 United stays off: the official MarkerPatch is loaded (%s).\n\n"
                          "United needs its own edition of MarkerPatch, the dinput8.dll from the United release. "
                          "It reads the same MarkerPatch.ini and keeps the fixes that work with the mod.\n\n"
                          "Put that dinput8.dll into the game folder%s and start the game again.",
                          marker, !std::strcmp(official, "MarkerPatch.asi") ? " and remove MarkerPatch.asi from plugins" : "");
            WriteIdleLog(module, idleText);
            // not from inside the loader lock
            if (HANDLE t = CreateThread(nullptr, 0, ShowIdleBox, nullptr, 0, nullptr)) CloseHandle(t);
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
