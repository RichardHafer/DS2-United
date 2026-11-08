// Dead Space 2 United by Richard Hafer.
// Performance and memory over a long run ([DS2Randomizer] PerfDiag, on by default).
// For the two open problems of long survival runs: the host's frame rate falls over
// a run (142 -> 24 fps on 19.10.), and from about wave 27 the inventory loses its
// hologram panel and item icons while texts still draw. Either can be memory: the
// game is a 32-bit process, 2 GB without the 4 GB copy, and a fragmented address
// space fails large allocations long before it is full.
// Every 30 s and at every wave change one line: frame times (average, 95th
// percentile, worst), hitches, private bytes, working set, free address space and
// its largest block, handles, GDI/USER objects, free texture memory, and the
// survival state. Hitches over 100 ms are logged one by one (bounded).
namespace survival_rt { extern volatile LONG stateMirror, waveMirror, aliveMirror; }
namespace perfdiag {
bool enabled = true;
LARGE_INTEGER frequency{}, lastFrame{};
// frame time histogram in 1 ms buckets up to 250 ms, the rest in the last one
constexpr unsigned kBuckets = 251;
volatile LONG buckets[kBuckets]{};
volatile LONG frames = 0, worstUs = 0, sumMs = 0, hitches50 = 0, hitches100 = 0, hitches250 = 0;
volatile LONG texMemMb = -1, texReads = 0, texFaults = 0;
struct Hitch { DWORD tick; LONG ms, wave, alive, state; };
constexpr unsigned kHitchRing = 32;
Hitch hitchRing[kHitchRing]{};
volatile LONG hitchWritten = 0;
LONG hitchPrinted = 0;
DWORD lastTexRead = 0;

int TexFilter() { InterlockedIncrement(&texFaults); return EXCEPTION_EXECUTE_HANDLER; }
// IDirect3DDevice9::GetAvailableTextureMem (vtable slot 4) on the game thread; the
// device pointer is the one the overlay draws with ([0x0211CDCC]).
LONG ReadTextureMemMb() {
    __try {
        const uint32_t device = *reinterpret_cast<const uint32_t*>(base + 0x1D1CDCC);
        if (!device) return -1;
        const uint32_t vtable = *reinterpret_cast<const uint32_t*>(device);
        using Fn = UINT(__stdcall*)(uint32_t);
        const UINT bytes = reinterpret_cast<Fn>(*reinterpret_cast<const uint32_t*>(vtable + 4 * 4))(device);
        return static_cast<LONG>(bytes / (1024u * 1024u));
    } __except (TexFilter()) {
        return -1;
    }
}

// Once per frame from the frame cap's tick. Cheap: one counter read, a few adds.
void FrameTick() {
    if (!enabled || !frequency.QuadPart) return;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (lastFrame.QuadPart) {
        const LONGLONG us = (now.QuadPart - lastFrame.QuadPart) * 1000000 / frequency.QuadPart;
        const LONG ms = static_cast<LONG>(us / 1000);
        InterlockedIncrement(&buckets[ms < static_cast<LONG>(kBuckets) ? ms : kBuckets - 1]);
        InterlockedIncrement(&frames);
        InterlockedExchangeAdd(&sumMs, ms);
        if (us > worstUs) InterlockedExchange(&worstUs, static_cast<LONG>(us > 0x7FFFFFFF ? 0x7FFFFFFF : us));
        if (ms >= 50) InterlockedIncrement(&hitches50);
        if (ms >= 250) InterlockedIncrement(&hitches250);
        if (ms >= 100) {
            InterlockedIncrement(&hitches100);
            const LONG n = InterlockedIncrement(&hitchWritten) - 1;
            hitchRing[n % kHitchRing] = Hitch{GetTickCount(), ms, InterlockedCompareExchange(&survival_rt::waveMirror, 0, 0),
                                              InterlockedCompareExchange(&survival_rt::aliveMirror, 0, 0), InterlockedCompareExchange(&survival_rt::stateMirror, 0, 0)};
        }
    }
    lastFrame = now;
    const DWORD tick = GetTickCount();
    if (tick - lastTexRead >= 30000) {
        lastTexRead = tick;
        InterlockedExchange(&texMemMb, ReadTextureMemMb());
        InterlockedIncrement(&texReads);
    }
}

struct AddressSpace { unsigned long long freeBytes = 0, largestFree = 0, committed = 0, reserved = 0; unsigned regions = 0; };
AddressSpace WalkAddressSpace() {
    AddressSpace a;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t at = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION m{};
    while (at < end && VirtualQuery(reinterpret_cast<void*>(at), &m, sizeof(m)) == sizeof(m)) {
        ++a.regions;
        if (m.State == MEM_FREE) {
            a.freeBytes += m.RegionSize;
            if (m.RegionSize > a.largestFree) a.largestFree = m.RegionSize;
        } else if (m.State == MEM_COMMIT) {
            a.committed += m.RegionSize;
        } else {
            a.reserved += m.RegionSize;
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
        if (next <= at) break;
        at = next;
    }
    return a;
}

struct MemoryCounters { DWORD cb; DWORD pageFaults; SIZE_T peakWorkingSet, workingSet, quotaPeakPaged, quotaPaged, quotaPeakNonPaged, quotaNonPaged, pagefile, peakPagefile, privateUsage; };
using MemInfoFn = BOOL(WINAPI*)(HANDLE, MemoryCounters*, DWORD);
MemInfoFn memInfo = nullptr;
unsigned long long userSpace = 0;
bool largeAddressAware = false;

void Install(FILE* log, const std::string& ini) {
    enabled = ConfigInt(ini, "DS2Randomizer", "PerfDiag", 1, 0, 1) != 0;
    QueryPerformanceFrequency(&frequency);
    if (HMODULE k32 = GetModuleHandleA("kernel32.dll")) memInfo = reinterpret_cast<MemInfoFn>(GetProcAddress(k32, "K32GetProcessMemoryInfo"));
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    userSpace = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress) - reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleA(nullptr));
    largeAddressAware = (reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(dos) + dos->e_lfanew)->FileHeader.Characteristics &
                         IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
    std::fprintf(log, "perfdiag %s ([DS2Randomizer] PerfDiag): exe %s, user address space %llu MB, memory counters %s\n", enabled ? "on" : "off",
                 largeAddressAware ? "large address aware (4 GB copy)" : "NOT large address aware (2 GB)", userSpace >> 20, memInfo ? "found" : "MISSING");
}

void WriteSnapshot(FILE* log, const char* why) {
    LONG copy[kBuckets];
    LONG total = 0;
    for (unsigned i = 0; i < kBuckets; ++i) { copy[i] = InterlockedExchange(&buckets[i], 0); total += copy[i]; }
    const LONG n = InterlockedExchange(&frames, 0), sum = InterlockedExchange(&sumMs, 0), worst = InterlockedExchange(&worstUs, 0);
    const LONG h50 = InterlockedExchange(&hitches50, 0), h100 = InterlockedExchange(&hitches100, 0), h250 = InterlockedExchange(&hitches250, 0);
    LONG p95 = 0, seen = 0;
    for (unsigned i = 0; i < kBuckets && total; ++i) {
        seen += copy[i];
        if (seen * 100 >= total * 95) { p95 = static_cast<LONG>(i); break; }
    }
    MemoryCounters mc{};
    mc.cb = sizeof(mc);
    const bool haveMem = memInfo && memInfo(GetCurrentProcess(), &mc, sizeof(mc));
    const AddressSpace a = WalkAddressSpace();
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    const DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    std::fprintf(log,
                 "perfdiag %s: frames=%ld avg_ms=%.1f p95_ms=%ld worst_ms=%.1f hitches50=%ld hitches100=%ld hitches250=%ld | private_mb=%llu working_mb=%llu "
                 "peak_working_mb=%llu | va_free_mb=%llu va_largest_free_mb=%llu va_committed_mb=%llu va_reserved_mb=%llu va_regions=%u of %llu MB | "
                 "handles=%lu gdi=%lu user=%lu tex_free_mb=%ld | survival state=%ld wave=%ld alive=%ld\n",
                 why, n, n ? static_cast<double>(sum) / n : 0.0, p95, worst / 1000.0, h50, h100, h250,
                 haveMem ? static_cast<unsigned long long>(mc.privateUsage) >> 20 : 0ull, haveMem ? static_cast<unsigned long long>(mc.workingSet) >> 20 : 0ull,
                 haveMem ? static_cast<unsigned long long>(mc.peakWorkingSet) >> 20 : 0ull, a.freeBytes >> 20, a.largestFree >> 20, a.committed >> 20, a.reserved >> 20,
                 a.regions, userSpace >> 20, handles, gdi, user, InterlockedCompareExchange(&texMemMb, 0, 0),
                 InterlockedCompareExchange(&survival_rt::stateMirror, 0, 0), InterlockedCompareExchange(&survival_rt::waveMirror, 0, 0),
                 InterlockedCompareExchange(&survival_rt::aliveMirror, 0, 0));
    if (a.largestFree < (64ull << 20))
        std::fprintf(log, "perfdiag WARNING: the largest free address block is %llu MB; big allocations (textures, streams) start to fail around here%s\n",
                     a.largestFree >> 20, largeAddressAware ? "" : "; the 4 GB copy (DS2_Start_4GB.cmd) doubles the space");
}

// Worker thread, once a second.
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    const LONG written = InterlockedCompareExchange(&hitchWritten, 0, 0);
    unsigned printed = 0;
    if (written - hitchPrinted > static_cast<LONG>(kHitchRing)) hitchPrinted = written - kHitchRing;
    while (hitchPrinted < written && printed < 10) {
        const Hitch h = hitchRing[hitchPrinted % kHitchRing];
        ++hitchPrinted;
        ++printed;
        std::fprintf(log, "perfdiag hitch %ld ms at tick %lu (survival state=%ld wave=%ld alive=%ld)\n", h.ms, h.tick, h.state, h.wave, h.alive);
    }
    static DWORD lastSnapshot = 0;
    static LONG lastWave = -1;
    const DWORD now = GetTickCount();
    const LONG wave = InterlockedCompareExchange(&survival_rt::waveMirror, 0, 0);
    if (wave != lastWave && wave > 0) {
        char why[32];
        std::snprintf(why, sizeof(why), "wave %ld", wave);
        WriteSnapshot(log, why);
        lastSnapshot = now;
    } else if (now - lastSnapshot >= 30000) {
        WriteSnapshot(log, "30 s");
        lastSnapshot = now;
    }
    lastWave = wave;
}
}  // namespace perfdiag
