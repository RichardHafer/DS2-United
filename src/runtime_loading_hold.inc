// Dead Space 2 United by Richard Hafer.
// Minimum loading screen time, and a wait for the randomizer's own streams.
//
// The loading screen ends when UILoadingScreenData::HasHoldTimeElapsed(float
// elapsed) says so (PC 0x00A41A60, thiscall, one float argument). It compares
// the elapsed seconds with the authored CMD_SetMinTimeToHoldLoadScreen at
// [this+0xC]+0x30 and takes a jb (0x00A41A89, bytes 72 29) into the hold
// branch while the time is not up. MarkerPatch's SkipArtificialLoadingDelay
// writes two NOPs over that jb, so the screen ends as soon as the level's own
// streams are in, and the pool this plugin requests during the load can still
// be loading when the first spawns are decided. This hook takes the timing
// over: the original runs with 0 seconds (held, the retail hold branch runs)
// while the conditions below say hold, and with a very large time (released,
// the authored minimum is skipped) otherwise. The jb bytes are put back before the
// first hold, whichever plugin was loaded first.
namespace loading_hold {

constexpr uint32_t kHasHoldTimeElapsedRva = 0x641A60;
constexpr uint32_t kHoldBranchRva = 0x641A89;
// push esi / mov esi,ecx / mov ecx,[esi+524Ch] / test ecx,ecx / je +0Fh / cmp byte ptr [esi+52D0h],0
const unsigned char kExpected[] = {0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0x4C, 0x52, 0x00, 0x00, 0x85, 0xC9, 0x74, 0x0F,
                                   0x80, 0xBE, 0xD0, 0x52, 0x00, 0x00, 0x00};
// comiss xmm0,[eax+30h], right in front of the jb
const unsigned char kBranchContext[] = {0x0F, 0x2F, 0x40, 0x30};

safetyhook::InlineHook holdHook;
bool enabled = false;
int minimumSeconds = 4, maximumSeconds = 30;
bool waitForPool = true;
bool streamProbeActive = false;
volatile LONG calls = 0, heldMinimum = 0, heldPool = 0, released = 0;
volatile LONG branchRestored = 0, branchUnknown = 0, longestHoldMs = 0, holding = 0, screens = 0;

// True while the pool (or a skin's model stream) is wanted for this level and
// is not ready yet. Until 15.09. a loading screen that ended before the probe
// had even placed its request (one second on a debug menu load) was let go,
// because "nothing requested" counted as "nothing to wait for". Now the wait
// starts as soon as the probe is in its Waiting state, so the request itself
// falls inside the held screen. Terminal probe results (registry full, the
// prerequisite timeout) release the screen, and MaximumLoadingSeconds caps
// the rest.
bool PoolPending() {
    if (!streamProbeActive) return false;
    if (InterlockedCompareExchange(&streamProbeHandle, 0, 0) == 0) {
        const LONG result = InterlockedCompareExchange(&streamProbeResult, 0, 0);
        if (streamLifetime.state != ds2::GlobalStreamLifetime::State::Waiting || result == 1 || result == 11) return false;
        // The frontend never gets a pool (its screens register three or four
        // TOCs, a level registers over twenty), so its loading screens are
        // not held for a request that will never come.
        uint32_t tocs = 0;
        return streamLifetime.manager && Read(streamLifetime.manager + 0x130, &tocs, 4) && tocs > 8 && tocs <= 32;
    }
    RecheckStreamGraph();
    if (InterlockedCompareExchange(&streamSelectionAllowed, 1, 1) == 0) return true;
    for (unsigned i = 0; i < ModelStreamCount() && i < std::size(modelStreams); ++i)
        if (modelStreams[i].handle && modelStreams[i].locked && modelStreams[i].status != 3) return true;
    return false;
}

// Puts the jb into the hold branch back if another plugin replaced it with
// NOPs. False when the site is not what this build expects.
bool EnsureHoldBranch() {
    unsigned char site[6]{};
    if (!Read(base + kHoldBranchRva - 4, site, sizeof(site))) return false;
    if (std::memcmp(site, kBranchContext, sizeof(kBranchContext)) != 0) { InterlockedIncrement(&branchUnknown); return false; }
    if (site[4] == 0x72 && site[5] == 0x29) return true;
    if (site[4] != 0x90 || site[5] != 0x90) { InterlockedIncrement(&branchUnknown); return false; }
    const unsigned char jb[] = {0x72, 0x29};
    void* target = reinterpret_cast<void*>(base + kHoldBranchRva);
    DWORD previous = 0;
    if (!VirtualProtect(target, sizeof(jb), PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(target, jb, sizeof(jb));
    VirtualProtect(target, sizeof(jb), previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jb));
    InterlockedIncrement(&branchRestored);
    return true;
}

bool __fastcall OnHasHoldTimeElapsed(void* self, void*, float elapsed) {
    InterlockedIncrement(&calls);
    const bool holdMinimum = elapsed < static_cast<float>(minimumSeconds);
    const bool holdPool = waitForPool && elapsed < static_cast<float>(maximumSeconds) && PoolPending();
    if (holdMinimum || holdPool) {
        InterlockedIncrement(holdMinimum ? &heldMinimum : &heldPool);
        InterlockedExchange(&holding, 1);
        const LONG ms = static_cast<LONG>(elapsed * 1000.0f);
        if (ms > InterlockedCompareExchange(&longestHoldMs, 0, 0)) InterlockedExchange(&longestHoldMs, ms);
        // the retail hold branch, or a plain "not yet" if the site is foreign
        if (!EnsureHoldBranch()) return false;
        return holdHook.thiscall<bool>(self, 0.0f);
    }
    InterlockedIncrement(&released);
    // one loading screen ends: the first release after a hold
    if (InterlockedExchange(&holding, 0) == 1) InterlockedIncrement(&screens);
    return holdHook.thiscall<bool>(self, 1.0e6f);
}

int Setting(const char* ini, const char* key, int fallback, int low, int high) {
    const int value = static_cast<int>(GetPrivateProfileIntA("DS2Randomizer", key, fallback, ini));
    return value < low ? low : value > high ? high : value;
}

bool Install(FILE* log, const char* ini, bool streamProbe) {
    minimumSeconds = Setting(ini, "MinimumLoadingSeconds", 4, 0, 60);
    waitForPool = Setting(ini, "LoadingWaitsForPool", 1, 0, 1) != 0;
    maximumSeconds = Setting(ini, "MaximumLoadingSeconds", 30, 0, 120);
    streamProbeActive = streamProbe;
    if (minimumSeconds == 0 && !waitForPool) {
        std::fprintf(log, "loading_hold off (MinimumLoadingSeconds=0, LoadingWaitsForPool=0); the loading screen timing is the game's\n");
        return false;
    }
    unsigned char actual[sizeof(kExpected)]{};
    if (!Read(base + kHasHoldTimeElapsedRva, actual, sizeof(actual)) || std::memcmp(actual, kExpected, sizeof(actual)) != 0) {
        std::fprintf(log, "loading_hold signature mismatch at %08X; loading screen timing untouched\n", static_cast<unsigned>(base + kHasHoldTimeElapsedRva));
        return false;
    }
    unsigned char site[6]{};
    const char* branch = "unknown";
    if (Read(base + kHoldBranchRva - 4, site, sizeof(site)) && std::memcmp(site, kBranchContext, sizeof(kBranchContext)) == 0) {
        if (site[4] == 0x72 && site[5] == 0x29) branch = "jb";
        else if (site[4] == 0x90 && site[5] == 0x90) branch = "nop_by_markerpatch";
    }
    holdHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kHasHoldTimeElapsedRva), reinterpret_cast<void*>(OnHasHoldTimeElapsed));
    if (!holdHook) {
        std::fprintf(log, "loading_hold hook failed; loading screen timing untouched\n");
        return false;
    }
    enabled = true;
    std::fprintf(log, "loading_hold installed at %08X minimum_s=%d wait_for_pool=%d maximum_s=%d hold_branch=%s\n",
                 static_cast<unsigned>(base + kHasHoldTimeElapsedRva), minimumSeconds, waitForPool ? 1 : 0, maximumSeconds, branch);
    return true;
}

void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    static LONG last = -1;
    const LONG total = InterlockedCompareExchange(&screens, 0, 0) + InterlockedCompareExchange(&branchUnknown, 0, 0);
    if (total == last) return;
    last = total;
    std::fprintf(log, "loading_hold screens=%ld calls=%ld held_minimum=%ld held_pool=%ld released=%ld longest_hold_ms=%ld branch_restored=%ld branch_unknown=%ld\n",
                 InterlockedCompareExchange(&screens, 0, 0),
                 InterlockedCompareExchange(&calls, 0, 0), InterlockedCompareExchange(&heldMinimum, 0, 0),
                 InterlockedCompareExchange(&heldPool, 0, 0), InterlockedCompareExchange(&released, 0, 0),
                 InterlockedCompareExchange(&longestHoldMs, 0, 0), InterlockedCompareExchange(&branchRestored, 0, 0),
                 InterlockedCompareExchange(&branchUnknown, 0, 0));
}

}  // namespace loading_hold
