// Dead Space 2 United by Richard Hafer.
// Standalone campaign co-op plugin.
//
// The co-op work grew inside the randomizer observer and ended up borrowing
// from it: the player tracking in runtime_placement.inc, the loading-screen
// state in runtime_loading_hold.inc, the per-frame tick in runtime_frame_cap.inc
// and, most awkwardly, its only periodic game-thread call, which lived in
// runtime_more_enemies.inc (DrainDeferredRepeats called coop::Poll).
//
// This entry point makes the co-op modules a plugin of their own. It supplies
// exactly the six things they used from the observer:
//   Guid / base / Read, the four bridge pointers the co-op modules assign to
//   each other, the handful of placement constants and globals, the two
//   loading-hold flags, a frame tick, and a caller for coop::Poll.
//
// Nothing here includes a randomizer module, and the configuration lives in its
// own DS2United.ini, so the two plugins share no code, no hook site chosen by the
// randomizer, and no configuration key. The one site both would want is the
// per-frame tick at 0x3701C0: both take it under the lock in hook_site.h and
// whichever comes second chains behind the first (2026-09-17: refusing, or
// racing, left two of four instances without a driver).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "coop_panel_claims.h"
#include <windows.h>
#include "vr_compat.h"
#include <winsock2.h>
#include <tlhelp32.h>
#include "ds2coop_config.h"
#include "survival_rewards.h"
#include "survival_capsules.h"
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <share.h>
#include <string>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;   // this module, for the lobby build id
#include "../third_party/safetyhook/safetyhook.hpp"
#include "hook_site.h"
#include "log_rotation.h"
#include "stream_registry.h"   // the randomizer's registry reader: says why a loose stream never arrives
#include "selection.h"
#include "generated_scene_kinds.h"
#include "scene_catalog.h"
#include "generated_enemy_catalog.h"     // play-dead spawns (bot senses)
#include "generated_playdead_donors.h"
#include "generated_variant_donors.h"     // the twitcher copies (bot senses)
#include "generated_scripted_containers.h"
#include "generated_death_messages.h"   // the messages a level expects from its enemies
#include "generated_fx_colors.h"        // [Visuals] effect and blood colours
#include "spawn_watchdog.h"             // the state machine layout (the laser diagnostics read the owner's states)

namespace {
using Guid = std::array<unsigned, 4>;
uintptr_t base = 0;

// Read from every thread: calls are summed per thread and added in batches
// (thousands per frame), queries and faults are rare and counted directly.
volatile LONG readCalls = 0, readQueries = 0, readFaults = 0;   // for the bots' frame budget log (runtime_coop_bot.inc)
volatile LONGLONG readQueryTicks = 0;   // QueryPerformanceCounter ticks spent in VirtualQuery, same log
thread_local LONG readCallsPending = 0;
// Reads straight from memory: the plugin lives in the game's own process, and
// ReadProcessMemory cost 0.39 us a call under WOW64 against 0.007 us for a copy
// (measured 27.09.) at five to seven thousand reads a frame. Only inside regions
// VirtualQuery calls committed and readable without a guard page (a guard page
// touched by a copy loses its guard), remembered per thread for two seconds; a
// region freed since is caught by the handler. A read across two regions goes
// through the kernel copy as before.
// VirtualQuery walks the page table from the address to the end of its region,
// about 4 us per MB (05.10.: 3.4 ms at the start of a committed 256 MB block),
// and the game's main heap is one 96 MB block. So there is one slot per 64 KB of
// the whole 4 GB address space (an earlier 1024 slots modulo 64 MB let the heap
// evict itself), and one query fills the slots of its whole region.
struct ReadRegion { uint32_t start, end; DWORD epoch; };
constexpr unsigned kReadSlots = 1u << 16;
// The 768 KB slot table of one thread, allocated on its first read and given
// back when the thread ends (the start-up thread is gone after a few seconds).
// Static TLS keeps the thread notifications coming despite DisableThreadLibraryCalls.
struct ReadCache {
    ReadRegion* regions = nullptr;
    ~ReadCache() { if (regions) VirtualFree(regions, 0, MEM_RELEASE); }
};
thread_local ReadCache readCache;
thread_local DWORD readEpoch = 1, readEpochAt = 0;
bool ReadableProtection(DWORD protect) {   // x86 reads execute-only pages too
    return !(protect & (PAGE_GUARD | PAGE_NOACCESS)) && (protect & 0xFF) != 0;
}
bool CopyGuarded(uintptr_t address, void* out, size_t bytes) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool Read(uintptr_t address, void* out, size_t bytes) {
    if (++readCallsPending >= 256) {
        InterlockedExchangeAdd(&readCalls, readCallsPending);
        readCallsPending = 0;
    }
    if (!bytes) return true;
    if (address < 0x10000 || address + bytes < address) return false;
    ReadRegion*& readRegions = readCache.regions;
    if (!readRegions) {
        readRegions = static_cast<ReadRegion*>(VirtualAlloc(nullptr, kReadSlots * sizeof(ReadRegion), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!readRegions) {
            SIZE_T done = 0;
            return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out, bytes, &done) && done == bytes;
        }
    }
    const DWORD now = GetTickCount();
    if (now - readEpochAt > 2000) {
        ++readEpoch;
        readEpochAt = now;
    }
    const uintptr_t chunk = (address >> 16) & (kReadSlots - 1);
    ReadRegion& slot = readRegions[chunk];
    if (slot.epoch != readEpoch || address < slot.start || address + bytes > slot.end) {
        InterlockedIncrement(&readQueries);
        MEMORY_BASIC_INFORMATION mbi{};
        LARGE_INTEGER t0{}, t1{};
        QueryPerformanceCounter(&t0);
        const bool known = VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != 0;
        QueryPerformanceCounter(&t1);
        InterlockedExchangeAdd64(&readQueryTicks, t1.QuadPart - t0.QuadPart);
        if (!known || mbi.State != MEM_COMMIT || !ReadableProtection(mbi.Protect)) return false;
        const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
        if (address + bytes > end) {
            SIZE_T done = 0;
            return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out, bytes, &done) && done == bytes;
        }
        // The region runs from the queried page to its end: every 64 KB it covers learns it.
        // The first slot keeps an earlier start of the same region.
        const uintptr_t last = ((end - 1) >> 16) & (kReadSlots - 1);
        for (uintptr_t c = chunk; c <= last; ++c) {
            ReadRegion& s = readRegions[c];
            const bool sameEarlier = c == chunk && s.epoch == readEpoch && s.end == end && s.start < start;
            if (!sameEarlier) s.start = static_cast<uint32_t>(c == chunk ? start : c << 16);
            s.end = static_cast<uint32_t>(end);
            s.epoch = readEpoch;
        }
    }
    if (CopyGuarded(address, out, bytes)) return true;
    InterlockedIncrement(&readFaults);
    slot.start = slot.end = 0;
    return false;
}

// Plain writable memory (the bots' pads, entity fields) is written with a
// guarded copy; WriteProcessMemory is kept for code and read-only data, where
// it changes the protection for the write. 08.10.: the game hung in a loading
// screen with the main thread inside WriteProcessMemory from the pad writes,
// thousands of kernel calls a frame.
bool WritableProtection(DWORD protect) {
    const DWORD p = protect & 0xFF;
    return !(protect & (PAGE_GUARD | PAGE_NOACCESS)) && (p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE);
}
bool CopyIn(uintptr_t address, const void* input, size_t bytes) {
    __try {
        std::memcpy(reinterpret_cast<void*>(address), input, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
volatile LONG writesDirect = 0, writesKernel = 0, writeQueries = 0;
// The last writable regions of this thread, for two seconds like the read cache: the bots'
// pad writes asked VirtualQuery for every field, 5 % of the game thread in the sampler (02.11.).
struct WriteRegion { uintptr_t start = 0, end = 0; DWORD epoch = 0; };
thread_local WriteRegion writeRegions[8];
thread_local unsigned writeRegionNext = 0;
bool WriteBytes(uintptr_t address, const void* input, size_t bytes) {
    if (!bytes) return true;
    if (address < 0x10000 || address + bytes < address) return false;
    const DWORD now = GetTickCount();
    if (now - readEpochAt > 2000) {
        ++readEpoch;
        readEpochAt = now;
    }
    for (const WriteRegion& r : writeRegions) {
        if (r.epoch != readEpoch || address < r.start || address + bytes > r.end) continue;
        if (CopyIn(address, input, bytes)) { InterlockedIncrement(&writesDirect); return true; }
        break;   // freed or protected since: ask again below
    }
    MEMORY_BASIC_INFORMATION mbi{};
    InterlockedIncrement(&writeQueries);
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
        WritableProtection(mbi.Protect) && address + bytes <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize &&
        CopyIn(address, input, bytes)) {
        WriteRegion& r = writeRegions[writeRegionNext++ % 8];
        r.start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        r.end = r.start + mbi.RegionSize;
        r.epoch = readEpoch;
        InterlockedIncrement(&writesDirect);
        return true;
    }
    InterlockedIncrement(&writesKernel);
    SIZE_T done = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
                              input, bytes, &done) && done == bytes;
}

// Bridges the co-op modules assign to one another. The first two belong to the
// retired local-Isaac experiments and stay null here; the co-op code checks them.
uint32_t (*g_ghostModelEntry)(uint32_t entity) = nullptr;
void (*g_recordActor)(uint32_t entity) = nullptr;
void (*g_uniformPoll)() = nullptr;
bool (*g_uniformPreparePose)(uint32_t component) = nullptr;
bool (*g_uniformGhostDetached)(uint32_t component) = nullptr;

// What the co-op modules used to take from the randomizer.

// The local player is found through the native PlayerManager (see
// LocalCampaignPlayer in runtime_coop_state.inc), so this plugin does NOT hook
// the Player constructor. These globals stay empty; only the retired
// stand-in and ghost-skeleton paths read them, so those cannot act here either.
namespace placement {
constexpr uint32_t kPlayerPrimaryVtableRva = 0x17294D0;
constexpr uint32_t kPlayerPosition = 0xC0, kPlayerForward = 0xB0;
volatile LONG playerObject = 0;
volatile LONG playerObjects[4]{};
volatile LONG playerConstructions = 0;
float YawDegrees(const float* forward) {
    return static_cast<float>(std::atan2(static_cast<double>(forward[0]),
                                         static_cast<double>(forward[2])) * 180.0 / 3.14159265358979);
}
}  // namespace placement

// Only used to decide when a level has settled. Without the randomizer's
// loading-screen hold these stay zero, which makes LevelSettled fall back to
// "a player exists and has been there a few seconds".
namespace loading_hold {
volatile LONG released = 0, holding = 0;
}

// Set once a periodic game-thread driver exists; coop_uniform requires it.
// Either the plugin's frame tick, or the self-drive below when that site is taken.
namespace frame_cap {
bool installed = false;
}

// True when the frame tick could not be hooked and the co-op modules must drive
// themselves from a hook they own (see DriveSelf in runtime_coop.inc).
bool g_selfDrive = false;

#include "runtime_coop.inc"
#include "runtime_coop_uniform.inc"

// Frame tick: the periodic game-thread call the co-op modules need.
safetyhook::InlineHook tickHook;
volatile LONG frames = 0;

void __cdecl OnFrameTick() {
    InterlockedIncrement(&frames);
    coop::Poll();                       // session, transports, player registration
    if (g_uniformPoll) g_uniformPoll();  // ghost uniforms, on the game thread
    tickHook.call<void>();
}

bool InstallFrameTick(FILE* log) {
    // mov eax,[0x0204D414] / sub esp,0xC / cmp eax,[0x0204D418] / je
    static const unsigned char kSite[] = {0xA1,0x14,0xD4,0x04,0x02,0x83,0xEC,0x0C,
                                          0x3B,0x05,0x18,0xD4,0x04,0x02,0x74,0x05};
    // Shared with the randomizer's frame cap: taken under a lock and chained
    // behind whichever hook is already there (see hook_site.h for the race
    // this replaces).
    if (!hook_site::Install(log, "coop_plugin frame tick", base + 0x3701C0, kSite, sizeof(kSite),
                            reinterpret_cast<void*>(OnFrameTick), tickHook))
        return false;
    frame_cap::installed = true;
    return true;
}

// Start-up
FILE* logFile = nullptr;
volatile LONG running = 0;

std::string PluginDirectory() {
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&PluginDirectory), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string directory(path);
    const auto slash = directory.find_last_of('\\');
    return slash == std::string::npos ? std::string(".") : directory.substr(0, slash);
}

DWORD WINAPI Diagnostics(LPVOID) {
    while (InterlockedCompareExchange(&running, 0, 0)) {
        Sleep(1000);
        if (!logFile) continue;
        if (coop::installed) coop::WriteDiagnostics(logFile);
        coop_uniform::WriteDiagnostics(logFile);
        static LONG lastKernelWrites = -1;
        const LONG kernelWrites = InterlockedCompareExchange(&writesKernel, 0, 0);
        if (kernelWrites != lastKernelWrites) {
            lastKernelWrites = kernelWrites;
            std::fprintf(logFile, "coop_plugin writes direct=%ld kernel=%ld read_faults=%ld (kernel: code or read-only data, through WriteProcessMemory)\n",
                         InterlockedCompareExchange(&writesDirect, 0, 0), kernelWrites, InterlockedCompareExchange(&readFaults, 0, 0));
        }
        std::fflush(logFile);
    }
    return 0;
}

// The DS2 executable is packed: at DllMain time the bytes at a retail address
// are still the packer's, so every signature check fails and no hook installs.
// The observer waits for the unpacked image before touching anything and this
// plugin must do the same: without it the co-op plugin silently installed
// nothing, including the single-instance bypass, so a second game could not
// start (2026-09-16 22:42).
bool UnpackedImage() {
    constexpr unsigned char expected[] = {
        0xE8,0x8B,0xB1,0x8A,0xFF,0x8B,0x35,0,0,0,0,
        0x68,0,0x20,0,0,0x8D,0x4C,0x24,0x34,0x51,0x8B,0xCB,
        0xC6,0x46,0x28,0,0xE8,0xB0,0xB1,0x8A,0xFF,
        0x8B,0xD8,0x89,0x5C,0x24,0x10,0xC6,0x46,0x28,1
    };
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + 0xACB2B0, actual, sizeof(actual))) return false;
    for (size_t i = 0; i < sizeof(actual); ++i) {
        if (i >= 7 && i < 11) continue;  // a relocated absolute address
        if (actual[i] != expected[i]) return false;
    }
    return true;
}

DWORD WINAPI Start(LPVOID) {
    base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const std::string directory = PluginDirectory();
    // The merged DS2United.ini (ds2coop::ConfigFile), else its own file of that
    // name, else the randomizer's (an old [Coop] section there keeps working).
    std::string ini = ds2coop::ConfigFile(directory + "\\", "DS2United.ini");
    if (!ds2coop::FileExists(ini))
        ini = directory + "\\DS2Randomizer.ini";

    unsigned index = 0;
    std::string logPath;
    do {
        const std::string stem = directory + "\\DS2United" + (index ? "." + std::to_string(index) : "");
        logPath = stem + ".log";
        // The last three runs survive as .prev.log, .prev2.log and .prev3.log: a crashed
        // client's log was overwritten by its own restart before anyone could read it (20:10),
        // and on 16.10. two quick restarts rotated a crashed host's log away. A log another
        // copy of the game still writes is left alone and the next number is tried.
        if (!log_rotation::Rotate(stem)) continue;
        logFile = _fsopen(logPath.c_str(), "w", _SH_DENYWR);
    } while (!logFile && ++index < 8);
    if (!logFile) return 0;

    std::fprintf(logFile, "coop_plugin standalone build; base=%08X ini=%s\n",
                 static_cast<unsigned>(base), ini.c_str());
    SYSTEMTIME started{};
    GetLocalTime(&started);   // tools/coop_test_report.py takes the session's start from here
    std::fprintf(logFile, "coop_plugin session started %04u-%02u-%02u %02u:%02u:%02u\n", started.wYear, started.wMonth, started.wDay, started.wHour, started.wMinute,
                 started.wSecond);
    coop::crashdump::logToKeep = logPath;
    coop::crashdump::logStream = logFile;
    coop::vlog::Open(directory, index);
    coop::botlog::Open(directory, index);
    std::fprintf(logFile, "lobby and Versus lines go to DS2Versus%s.log, bot lines to DS2Bots%s.log\n", index ? ("." + std::to_string(index)).c_str() : "",
                 index ? ("." + std::to_string(index)).c_str() : "");
    std::fflush(logFile);

    // Win the race for the instance check before doing anything else.
    //
    // The game looks for a window of its own and quits if it finds one, and a
    // second or third instance that loses that race exits so early its plugin
    // never writes another line, which is what was seen on 2026-09-17,
    // a log with one line in it and no crash dump.
    //
    // It cannot be done here and now: the exe is packed, so at this
    // moment the import slot still holds a packer thunk rather than
    // FindWindowW. So this watches the slot and takes it the instant the packer
    // makes it real, which is well before the unpacked-image wait below would
    // finish. Install calls it again later and finds it already done.
    // DS2COOP_PORT is the second copy for the main menu lobby.
    char environment[64]{};
    if (GetEnvironmentVariableA("DS2COOP", environment, sizeof(environment)) > 0 ||
        GetEnvironmentVariableA("DS2COOP_PORT", environment, sizeof(environment)) > 0)
        coop::WaitAndDisableInstanceCheck(logFile, 30000);
    // The game's thread pins go to every core ([Coop] UnpinThreads): two copies
    // on one PC otherwise share core 0.
    coop::focusrate::EarlyUnpin(logFile, ini);

    constexpr unsigned kAttempts = 300;
    unsigned waited = 0;
    for (; waited < kAttempts && !UnpackedImage(); ++waited) {
        if (waited && waited % 30 == 0) {
            std::fprintf(logFile, "coop_plugin still waiting for the unpacked image, %u s\n", waited);
            std::fflush(logFile);
        }
        Sleep(1000);
    }
    if (waited >= kAttempts) {
        std::fprintf(logFile, "coop_plugin unpacked image never appeared after %u checks; nothing installed\n",
                     kAttempts);
        std::fflush(logFile);
        return 0;
    }
    std::fprintf(logFile, "coop_plugin unpacked image ready after %u s\n", waited);
    // The frontend stream is opened about 3 s from now, so the native mode
    // row's file redirect cannot wait for InstallDelayMs.
    coop::gamemode::native::EarlyInstall(logFile, ini);
    // global_assets uploads the visor flares before InstallDelayMs as well
    coop::rigcolor::EarlyInstall(logFile, ini);
    coop::fxcolor::EarlyInstall(logFile, ini);
    std::fflush(logFile);
    // Other plugins (the DS2 Debug Menu) verify the retail bytes of their
    // bindings right after the image is unpacked; a hook of ours that lands
    // first makes them disable themselves ("sometimes the debug menu does not
    // load", 21:40). So they get a head start. [Coop] InstallDelayMs.
    {
        const int delay = GetPrivateProfileIntA("Coop", "InstallDelayMs", 3000, ini.c_str());
        if (delay > 0) {
            std::fprintf(logFile, "coop_plugin giving other plugins %d ms before installing hooks\n", delay);
            std::fflush(logFile);
            Sleep(static_cast<DWORD>(delay));
        }
    }

    // The driver has to exist before coop_uniform::Install, which refuses to
    // install without one. Poll() itself is inert until coop::Install ran.
    if (!InstallFrameTick(logFile)) {
        // Fall back to driving from a hook this plugin owns, so a contested frame tick
        // costs cadence rather than the whole uniform path.
        g_selfDrive = true;
        frame_cap::installed = true;
        std::fprintf(logFile, "coop_plugin self_drive=1 (periodic work runs from the "
                              "local-game-player hook, throttled to ~16 ms)\n");
    }
    coop::Install(logFile, ini);
    coop_uniform::Install(logFile, ini);
    coop::FinalizeNPCSetup(logFile);

    InterlockedExchange(&running, 1);
    CreateThread(nullptr, 0, Diagnostics, nullptr, 0, nullptr);
    std::fflush(logFile);
    return 0;
}
}  // namespace

// For the randomizer's survival director: on-screen text through this plugin's
// RIG notice, the one owner of the borrowed text slot. Any thread; 1 when queued.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_QueueNotice(const char* text) {
    coop::survnet::Broadcast(text);   // a host's wave notices reach the partners too
    return coop::notice::Enqueue(text) ? 1 : 0;
}
// The survival status line on the overlay ([Hud]): 1 while it can be drawn, and
// the director then sends no RIG notices. Any thread.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_HudReady() {
    return coop::hud::Ready() ? 1 : 0;
}
// [Difficulty] Hud: 1 when the director ends its HUD line with the game's difficulty
// (it reads the global itself). Any thread.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_DifficultyHud() {
    return coop::difficulty::HudWanted() ? 1 : 0;
}
// The director's line and the event line under it (empty: nothing to show).
// A host sends both to the partners. Any thread.
extern "C" __declspec(dllexport) void __cdecl DS2Coop_SurvivalHud(const char* line, const char* flash) {
    coop::hud::SetLocal(line, flash);
}
// Where a NavPower walk from `from` toward `to` ends: the survival watchdog asks when it
// writes an enemy off. The path count with `end` filled, 0 without a level space or a path.
// NavPower takes its own lock; QueryRaw runs under SEH.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_NavPathEnd(const float* from, const float* to, float* end) {
    if (!from || !to || !end) return 0;
    coop_bot::Vec3 e{};
    int n = 0;
    if (!coop::bot_nav::PathEnd({from[0], from[1], from[2]}, {to[0], to[1], to[2]}, e, n)) return 0;
    end[0] = e.x;
    end[1] = e.y;
    end[2] = e.z;
    return n;
}
// 0 no network session wanted (solo, bots), 1 host, 2 client. A client runs no
// survival director: the host's enemies reach it as ghosts.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_NetRole() {
    return coop::NetRole();
}
// The host's director at a wave end (kind 2, the wave): every partner drops its
// own rewards. Any thread.
// Kind 3, the director's state for the music ((wave << 8) | state): played here
// and, when hosting, sent to the partners.
extern "C" __declspec(dllexport) void __cdecl DS2Coop_SurvivalEvent(int kind, int value) {
    if (kind == 2) {
        coop::survnet::Queue(2, nullptr, static_cast<uint32_t>(value));
        InterlockedIncrement(&coop::survnet::rewardEvents);   // our own cleared wave counts too (lives refill)
    }
    if (kind == 3) {
        coop::survmusic::SetState(value & 0xFF, value >> 8, "director");
        coop::survnet::Queue(3, nullptr, static_cast<uint32_t>(value));
    }
}
// The host's (or a solo) director pays profile XP: run, wave or step, XP and
// coop_progress flags. Booked here and sent to the partners. Any thread.
extern "C" __declspec(dllexport) void __cdecl DS2Coop_SurvivalProgress(int run, int wave, int xp, int flags) {
    coop::progress::FromDirector(static_cast<uint32_t>(run), static_cast<uint32_t>(wave), xp > 0 ? static_cast<uint32_t>(xp) : 0u,
                                 static_cast<uint32_t>(flags));
}
// (respawn epoch << 2) | (armed << 1) | pending: the survival director keeps its
// run through a respawn and adopts the new Isaac.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_RespawnState() {
    return static_cast<int>((InterlockedCompareExchange(&coop::respawn::epoch, 0, 0) << 2) | ((InterlockedCompareExchange(&coop::respawn::armed, 0, 0) & 1) << 1) |
                            (InterlockedCompareExchange(&coop::respawn::pending, 0, 0) & 1));
}
// The game mode picked in the map picker (0 survival, 1 mercenaries, 2 objective,
// 3 hold the zone, 4 versus); the survival director reads it when a run arms. Any thread.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_GameMode() {
    // A campaign arena plays the picked mode only when it has that mode's points, else
    // survival (13.10.: Versus picked, then an arena row: the director idled behind the locked doors).
    if (InterlockedCompareExchange(&coop::arena::stateMirror, 0, 0) != 0)
        return coop::arena::ModeNow(static_cast<int>(InterlockedCompareExchange(&coop::gamemode::mode, 0, 0)));
    return static_cast<int>(InterlockedCompareExchange(&coop::gamemode::mode, 0, 0));
}
// Hold the Zone's objectives on this MP map ([Zone] OpenObjectives): 0 nothing here,
// 1 being opened, 2 opened (every door open), 3 not possible on this map. Any thread.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_ZoneObjectives() {
    return static_cast<int>(InterlockedCompareExchange(&coop::zoneobj::state, 0, 0));
}
// The Versus round for the survival director's slashers: out[0] stage (0 playing,
// 1 round over, 2 match over), out[1] round, out[2]/out[3] the round's player
// kills by the humans/necros (host only). 1 while a Versus match runs on an MP
// map here, else 0. Game thread.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_VersusRound(int* out, int count) {
    return coop::versus::RoundInfo(out, count);
}
// The Objective mode's defend phases: `enemies` holds count x {x, y, z} of the
// director's living enemies; those within `radius` of a vulnerable defend target
// hit it for dps x seconds each. Returns how many targets are vulnerable (-1 not
// ready); `lowest` gets the weakest one's health fraction (-1 none). Game thread.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_DefendTick(const float* enemies, int count, float seconds, float dps, float radius, float* lowest) {
    return coop::objectives::defend::Tick(enemies, count, seconds, dps, radius, lowest);
}
// Mercenaries capsules in co-op. The host's director announces each one it places
// (kind 5, pos = {x, y, z}) and each one that is gone (kind 6, id 0 = all); a
// partner's director takes them one at a time and keeps its own copies in step.
extern "C" __declspec(dllexport) void __cdecl DS2Coop_SurvivalCapsuleSend(int kind, unsigned id, const float* pos, int golden) {
    coop::survnet::CapsuleWire wire{};
    wire.id = id;
    if (pos) std::memcpy(wire.pos, pos, sizeof(wire.pos));
    wire.golden = golden ? 1 : 0;
    coop::survnet::QueueCapsule(static_cast<uint8_t>(kind == 5 ? 5 : 6), wire);
}
extern "C" __declspec(dllexport) int __cdecl DS2Coop_SurvivalCapsule(int* kind, unsigned* id, float* pos, int* golden) {
    coop::survnet::CapsuleEvent e{};
    if (!coop::survnet::TakeCapsule(e)) return 0;
    if (kind) *kind = e.kind;
    if (id) *id = e.wire.id;
    if (pos) std::memcpy(pos, e.wire.pos, sizeof(e.wire.pos));
    if (golden) *golden = e.wire.golden;
    return 1;
}
// Hold the Zone: where the zone is and how it stands (state 0 none, 1 empty,
// 2 capturing, 3 contested, 4 losing; pos {x, y, z}, null with state 0). Shown
// here on the game thread and passed on to the partners when hosting. Any thread.
extern "C" __declspec(dllexport) void __cdecl DS2Coop_SurvivalZone(int state, unsigned id, const float* pos, float radius, int percent) {
    coop::zone::SetLocal(state, id, pos, radius, percent);
}
// Mercenaries: the capsules standing now, count x {x, y, z} and golden per capsule,
// for the waypoints on the screen. Any thread; a list not renewed for 1.5 s is dropped.
extern "C" __declspec(dllexport) void __cdecl DS2Coop_SurvivalMarks(const float* xyz, const int* golden, int count) {
    coop::marks::SetCapsules(xyz, golden, count);
}
// The last enemies of a wave, count x {x, y, z} (0 clears): marked here and sent to the
// partners. Any thread (the director).
extern "C" __declspec(dllexport) void __cdecl DS2Coop_SurvivalEnemyMarks(const float* xyz, int count) {
    coop::marks::SetEnemies(xyz, count);
    coop::survnet::QueueEnemies(xyz, count);
}
// The partners' Isaacs (fresh state packets only), count x {x, y, z}.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_PartnerPositions(float* out, int max) {
    return coop::PartnerPositions(out, max);
}
// Arrivals on an MP map whose Isaac is spawned and equipped. The director arms the
// next run after a checkpoint restart once this moves. Any thread.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_ArrivalsReady() {
    // a campaign arena's new Isaac (arrival or checkpoint restart) counts too
    return static_cast<int>(InterlockedCompareExchange(&coop::mpmap::readyArrivals, 0, 0) + InterlockedCompareExchange(&coop::arena::readyArrivals, 0, 0));
}
// The Objective result: reload the map from its checkpoint (the partners follow).
// 1 when queued for the game thread, 0 when the restart site did not verify.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_RestartCheckpoint() {
    return coop::gamemode::RequestRestart();
}
// Human deaths on the MP map so far: this machine's and the partners' (their LIFE
// packets going from up to down). Mercenaries takes time off for each. Any thread.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_HumanDeaths() {
    return static_cast<int>(InterlockedCompareExchange(&coop::respawn::humanDeaths, 0, 0));
}
// 1 when every human on the MP map is down and nobody comes back on a timer:
// the host's director ends the run (game over). Any thread.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_TeamDown() {
    return static_cast<int>(InterlockedCompareExchange(&coop::respawn::teamDownFlag, 0, 0));
}
// The wave of the last reward event (a client's director drops its share of it).
extern "C" __declspec(dllexport) void __cdecl DS2Coop_ResetSurvivalRewards() {
    coop::survnet::ResetRewards();
}
extern "C" __declspec(dllexport) int __cdecl DS2Coop_TakeRewardWave() {
    uint32_t wave = 0;
    AcquireSRWLockExclusive(&coop::survnet::lock);
    const bool any = coop::survnet::receivedRewards.Take(wave);
    ReleaseSRWLockExclusive(&coop::survnet::lock);
    return any ? static_cast<int>(wave) : -1;
}
extern "C" __declspec(dllexport) int __cdecl DS2Coop_LastRewardWave() {
    return static_cast<int>(InterlockedCompareExchange(&coop::survnet::lastRewardWave, 0, 0));
}
// On a client: how many reward events the host sent so far.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_RewardEvents() {
    return static_cast<int>(InterlockedCompareExchange(&coop::survnet::rewardEvents, 0, 0));
}
// A running campaign arena, for the survival director (any thread): 1 and its
// name ("arena_..."), the survival map name while it runs.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_Arena(char* name, int size) {
    return coop::arena::RunningName(name, size);
}
// 1 when the point (x, y, z) is in the running arena's rooms.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_ArenaInside(const float* point) {
    return coop::arena::InsidePoint(point);
}
// The running arena's marked spots: up to `max` (x, y, z) triples; returns how many it has.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_ArenaSpots(float* out, int max) {
    return coop::arena::Spots(out, max);
}
// The running arena's points for a game mode: kind 0 Hold the Zone, 1 Mercenaries capsules,
// 2 respawn; up to `max` (x, y, z, radius) quadruples, radius 0 for the mode's default.
// Returns how many it has.
extern "C" __declspec(dllexport) int __cdecl DS2Coop_ArenaPoints(int kind, float* out, int max) {
    return coop::arena::Points(kind, out, max);
}

// In DS2United.asi (DS2COOP_MODULE) ds2coop_main.cpp's DllMain calls this half.
#ifdef DS2COOP_MODULE
extern "C" BOOL WINAPI DS2Coop_CoopMain(HINSTANCE instance, DWORD reason, LPVOID) {
#else
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
#endif
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        CreateThread(nullptr, 0, Start, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&running, 0);
    }
    return TRUE;
}
