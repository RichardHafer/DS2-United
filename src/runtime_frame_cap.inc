// Frame cap: the game's script logic ties itself to the frame time, and
// above 60 FPS elevators refuse to arrive and doors stay shut, with or
// without this mod (every friend's hang of 10.09. ran uncapped). Asking
// players to cap the game themselves worked for some of them. This does it
// in the plugin: the main loop calls 0x007701C0 exactly once per frame (its
// only caller is the loop at 0x0042F876; it bumps the frame counter at
// 0x0204D410 and sends the per-frame message), so the call is wrapped and
// the wrapper does not return before the frame's share of time is over.
// Sleep for the coarse part with the timer period at 1 ms, spin for the last
// two milliseconds. FpsCap=0 switches it off; VSync on top is harmless.
namespace frame_cap {
safetyhook::InlineHook tickHook;
bool installed = false;
unsigned fpsCap = 60;
LARGE_INTEGER frequency{}, lastFrame{};
volatile LONG frames = 0, slept = 0;

void Pace() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (lastFrame.QuadPart) {
        const LONGLONG interval = frequency.QuadPart / fpsCap;
        const LONGLONG due = lastFrame.QuadPart + interval;
        if (now.QuadPart < due) {
            const LONGLONG spinTicks = frequency.QuadPart / 500;   // 2 ms
            while (due - now.QuadPart > spinTicks) {
                Sleep(1);
                QueryPerformanceCounter(&now);
            }
            while (now.QuadPart < due) {
                YieldProcessor();
                QueryPerformanceCounter(&now);
            }
            InterlockedIncrement(&slept);
        }
        // a stall longer than a frame does not owe time to the next one
        lastFrame.QuadPart = now.QuadPart - due > interval ? now.QuadPart : due;
    } else {
        lastFrame = now;
    }
    InterlockedIncrement(&frames);
}

void __cdecl OnFrameTick() {
    tickHook.call<void>();
    if (fpsCap) Pace();
}

bool Install(FILE* log, const std::string& ini) {
    fpsCap = static_cast<unsigned>(ConfigInt(ini, "DS2Randomizer", "FpsCap", 60, 0, 240));
    if (fpsCap && fpsCap < 30) fpsCap = 30;
    if (!fpsCap) { std::fprintf(log, "frame_cap off (FpsCap=0)\n"); return false; }
    // mov eax,[0x0204D414] / sub esp,0xC / cmp eax,[0x0204D418] / je
    static const unsigned char kSite[] = {0xA1,0x14,0xD4,0x04,0x02,0x83,0xEC,0x0C,0x3B,0x05,0x18,0xD4,0x04,0x02,0x74,0x05};
    QueryPerformanceFrequency(&frequency);
    if (!frequency.QuadPart) return false;
    // 1 ms timer period without linking winmm: the plugin only needs the one call
    if (HMODULE winmm = LoadLibraryA("winmm.dll")) {
        using BeginPeriod = UINT(WINAPI*)(UINT);
        if (auto begin = reinterpret_cast<BeginPeriod>(GetProcAddress(winmm, "timeBeginPeriod"))) begin(1);
    }
    // Other plugins wrap the same call. Taken under the shared lock and chained
    // behind whichever got there first; refusing used to leave the game
    // uncapped exactly when another plugin was loaded.
    if (!hook_site::Install(log, "frame_cap", base + 0x3701C0, kSite, sizeof(kSite),
                            reinterpret_cast<void*>(OnFrameTick), tickHook))
        return false;
    installed = true;
    std::fprintf(log, "frame_cap fps=%u\n", fpsCap);
    return true;
}

void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&frames, 0, 0);
    if (now / 3000 != last / 3000) {
        std::fprintf(log, "frame_cap frames=%ld paced=%ld fps=%u\n", now, InterlockedCompareExchange(&slept, 0, 0), fpsCap);
        last = now;
    }
}
}  // namespace frame_cap
