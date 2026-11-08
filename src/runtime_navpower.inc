// Dead Space 2 United by Richard Hafer.
// NavPower's system pool, and why it is the enemy limit.
//
// Dump 2924 (20.09.2026): int3 at 0x005B480D, "System ran out of available
// heap memory and will crash". The engine hands NavPower (bfx, the pathfinding
// library) one fixed block for its system allocator: bfx::SystemCreate at
// 0x005B4620, cdecl (memory, size, params, customAllocator), creates a
// dlmalloc-style pool over that block (0x005A9E10 -> 0x005C5520). Retail size
// is 0x90000 = 576 KiB (pool+0x1AC footprint in the dump); the block comes
// from the engine allocator in the manager constructor at 0x01363220.
//
// At the crash the pool held 160 navigation entries, the top chunk was 32
// bytes, the largest free chunk 8088 bytes, and the request that died was
// 23444 bytes (an entry table: 4 + sum(0x34 + 24 * nodes) over every entry).
// Every enemy standing (and, it seems, lying) around costs a few KiB here,
// so about 160 of them is where retail Dead Space 2 gives up, whatever the
// frame rate says.
//
// The hook below sits on SystemCreate and swaps the block for a larger one
// (VirtualAlloc, committed, never freed). SystemCreate returns early while
// [0x0202F4CC] is set; the system is re-created on level changes and every
// creation gets the same block. The engine's 576 KiB block is left
// alone. Nothing else reads the size.
namespace navpower {
constexpr uintptr_t kSystemCreateRva = 0x001B4620;
constexpr unsigned kRetailPoolBytes = 0x90000;
safetyhook::InlineHook createHook;
unsigned poolKiB = 0;
void* ourBlock = nullptr;
unsigned ourBytes = 0;
volatile LONG creates = 0, enlarged = 0;

using SystemCreateFn = void* (__cdecl*)(void*, unsigned, void*, void*);

void* __cdecl OnSystemCreate(void* memory, unsigned size, void* params, void* allocator) {
    InterlockedIncrement(&creates);
    const unsigned wanted = poolKiB * 1024u;
    if (wanted > size) {
        if (!ourBlock) {
            ourBlock = VirtualAlloc(nullptr, wanted, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            ourBytes = ourBlock ? wanted : 0;
        }
        if (ourBlock && ourBytes >= wanted) {
            InterlockedIncrement(&enlarged);
            return createHook.call<void*>(ourBlock, wanted, params, allocator);
        }
    }
    return createHook.call<void*>(memory, size, params, allocator);
}

void Load(const std::string& ini) {
    // 0 keeps retail. Up to 64 MiB; 8x retail (4608) is the default.
    poolKiB = static_cast<unsigned>(ConfigInt(ini, "DS2Randomizer", "NavPowerPoolKiB", 4608, 0, 65536));
}

// cmp dword ptr [0x0202F4CC],0 / jne +0xE1 / push esi / mov esi,[esp+14h] / test esi,esi / push edi / mov edi,[esp+14h]
static const unsigned char kSig[] = {0x83,0x3D,0xCC,0xF4,0x02,0x02,0x00,0x0F,0x85,0xE1,0x00,0x00,0x00,0x56,0x8B,0x74,0x24,0x14,0x85,0xF6,0x57,0x8B,0x7C,0x24};

// The race (20.09.2026, evening): the engine creates the NavPower system
// early in its own start-up; the plugin's worker used to reach this hook after
// the catalog and the pool were built, seconds later, and lost the race in two
// sessions out of nine ("system already created"). The hook now goes in as
// the very first thing the worker does, as soon as the executable is
// unpacked: the site is polled until the retail bytes (or another plugin's
// jmp in front of them) show up. hook_site::Install itself gives up at once
// on packed bytes, so the waiting is done here. The system is re-created on
// every level change (creates=5 in one session), so a lost first race still
// only costs the first level.
bool WaitForSite(FILE* log, unsigned maxMs) {
    const uintptr_t site = base + kSystemCreateRva;
    const DWORD start = GetTickCount();
    unsigned polls = 0;
    for (;;) {
        unsigned char actual[sizeof(kSig)]{};
        ++polls;
        if (Read(site, actual, sizeof(actual))) {
            const auto state = hook_site::Inspect(actual, kSig, sizeof(kSig));
            if (state != hook_site::State::Foreign) {
                if (polls > 1) std::fprintf(log, "navpower pool: site unpacked after %lu ms (%u polls)\n", GetTickCount() - start, polls);
                return true;
            }
        }
        if (GetTickCount() - start > maxMs) {
            std::fprintf(log, "navpower pool: site still packed after %u ms; not hooked\n", maxMs);
            return false;
        }
        Sleep(10);
    }
}

bool Install(FILE* log) {
    if (!poolKiB) { std::fprintf(log, "navpower pool: retail (%u KiB)\n", kRetailPoolBytes / 1024); return false; }
    if (!WaitForSite(log, 20000)) return false;
    uint32_t existing = 0;
    if (Read(base + 0x1C2F4CC, &existing, 4) && existing)
        std::fprintf(log, "navpower pool: system already created at %08X before this plugin loaded; the first level keeps the retail pool, every level change re-creates the system through the hook\n", existing);
    const bool ok = hook_site::Install(log, "navpower SystemCreate", base + kSystemCreateRva, kSig, sizeof(kSig),
                                       reinterpret_cast<void*>(OnSystemCreate), createHook);
    std::fprintf(log, "navpower pool: %s at %08X, pool %u KiB (retail %u KiB)\n",
                 ok ? "hooked" : "NOT hooked", static_cast<unsigned>(base + kSystemCreateRva), poolKiB, kRetailPoolBytes / 1024);
    return ok;
}

void Summary(FILE* log) {
    if (!createHook) return;
    std::fprintf(log, "navpower pool creates=%ld enlarged=%ld block=%p bytes=%u\n",
                 InterlockedCompareExchange(&creates, 0, 0), InterlockedCompareExchange(&enlarged, 0, 0), ourBlock, ourBytes);
}
}  // namespace navpower
