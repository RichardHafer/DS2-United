// Dead Space 2 United by Hafer.
// Overflow for the sound bank heap. The memory class "soundbank" (0x7A10372B)
// is a fixed 96 MB heap ([0x0201C130], registered at 0x00411AA0; base +0x0C,
// size +0x10, inner allocator +0x48). The Outbreak maps' banks are uncompressed
// PCM (mall 25.4 MB of banks against 1.9 MB on Titan Mines), so the heap runs
// full while the checkpoint loads. Its Alloc returns null, and the stream
// loader inflates the next bank (global_mp_assets.sbk, 10 MB) into address 0
// (dump 47844, 0x0140BC83; 107 banks filled the heap to the end).
//
// When the heap refuses, the allocation comes from VirtualAlloc instead, and
// that pointer is known to the heap's Free, GetSize and Contains. The class
// methods (vtable 0x01AAE188) are hooked for this one instance only:
// Alloc 0x00452E80 thiscall(size, params) ret 8 (params: list of {type, value,
// next}, 2 = alignment, 3 = offset), Free 0x00453030 thiscall(ptr, x) ret 8,
// Contains 0x00452E20 and GetSize 0x00452E50 thiscall(ptr) ret 4. The
// ICoreAllocator view (+4) allocates through the same Alloc (0x00452F20).
namespace bankheap {
constexpr uint32_t kHeapGlobal = 0x1C1C130;   // RVA of [0x0201C130]
struct Site { const char* name; uint32_t rva; unsigned char bytes[8]; };
const Site kSites[] = {
    {"Alloc", 0x52E80, {0x51, 0x8B, 0x54, 0x24, 0x0C, 0x53, 0x55, 0x56}},
    {"Free", 0x53030, {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x49, 0x48, 0x50}},
    {"Contains", 0x52E20, {0x8B, 0x41, 0x0C, 0x8B, 0x54, 0x24, 0x04, 0x3B}},
    {"GetSize", 0x52E50, {0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0, 0x74, 0x10}},
};
constexpr unsigned kMaxBlocks = 1024;

bool enabled = true, installed = false;
safetyhook::InlineHook allocHook, freeHook, containsHook, sizeHook;
SRWLOCK lock = SRWLOCK_INIT;
struct Block { uint32_t ptr, raw, size; };
Block blocks[kMaxBlocks];
unsigned blockCount = 0;
volatile LONG refusals = 0, overflowAllocs = 0, overflowFails = 0, overflowFrees = 0, tableFull = 0, lines = 0;
volatile LONG overflowBytes = 0, peakBytes = 0;

uint32_t Heap() {
    uint32_t heap = 0;
    Read(base + kHeapGlobal, &heap, 4);
    return heap;
}
int FindLocked(uint32_t ptr) {
    for (unsigned i = 0; i < blockCount; ++i) if (blocks[i].ptr == ptr) return static_cast<int>(i);
    return -1;
}
bool Ours(uint32_t ptr, uint32_t* size) {
    AcquireSRWLockShared(&lock);
    const int i = ptr ? FindLocked(ptr) : -1;
    if (i >= 0 && size) *size = blocks[i].size;
    ReleaseSRWLockShared(&lock);
    return i >= 0;
}
// The heap's own parameter list: alignment (type 2) and offset (type 3).
void Params(uint32_t params, uint32_t& align, uint32_t& offset) {
    align = 0x10;
    offset = 0;
    for (unsigned guard = 0; params && guard < 8; ++guard) {
        uint32_t node[3]{};
        if (!Read(params, node, sizeof(node))) break;
        if (node[0] == 2 && node[1]) align = node[1];
        if (node[0] == 3) offset = node[1];
        params = node[2];
    }
    if (align & (align - 1)) align = 0x10;
}

uint32_t __fastcall OnAlloc(void* self, void*, uint32_t size, uint32_t params) {
    const uint32_t got = allocHook.thiscall<uint32_t>(self, size, params);
    const uint32_t heap = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    if (got || !size || heap != Heap()) return got;
    InterlockedIncrement(&refusals);
    uint32_t align = 0, offset = 0;
    Params(params, align, offset);
    const size_t total = static_cast<size_t>(size) + align + (offset % align);
    void* raw = VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!raw) {
        InterlockedIncrement(&overflowFails);
        if (InterlockedIncrement(&lines) <= 16) mpmap::Event("bankheap: heap full and VirtualAlloc(%lu) failed too", static_cast<unsigned long>(total));
        return 0;
    }
    const uint32_t r = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(raw));
    uint32_t p = (r + offset + align - 1) / align * align - offset;
    if (p < r) p += align;
    AcquireSRWLockExclusive(&lock);
    const bool stored = blockCount < kMaxBlocks;
    if (stored) blocks[blockCount++] = Block{p, r, size};
    ReleaseSRWLockExclusive(&lock);
    if (!stored) {
        VirtualFree(raw, 0, MEM_RELEASE);
        InterlockedIncrement(&tableFull);
        return 0;
    }
    InterlockedIncrement(&overflowAllocs);
    const LONG now = InterlockedExchangeAdd(&overflowBytes, static_cast<LONG>(size)) + static_cast<LONG>(size);
    LONG peak = InterlockedCompareExchange(&peakBytes, 0, 0);
    while (now > peak && InterlockedCompareExchange(&peakBytes, now, peak) != peak) peak = InterlockedCompareExchange(&peakBytes, 0, 0);
    if (InterlockedIncrement(&lines) <= 16)
        mpmap::Event("bankheap: soundbank heap full; %lu bytes (align %lX) from overflow at %08X, overflow now %.1f MB in %u blocks", static_cast<unsigned long>(size),
                     static_cast<unsigned long>(align), p, now / 1048576.0, blockCount);
    return p;
}
void __fastcall OnFree(void* self, void*, uint32_t ptr, uint32_t extra) {
    if (ptr) {
        AcquireSRWLockExclusive(&lock);
        const int i = FindLocked(ptr);
        Block b{};
        if (i >= 0) { b = blocks[i]; blocks[i] = blocks[--blockCount]; }
        ReleaseSRWLockExclusive(&lock);
        if (i >= 0) {
            VirtualFree(reinterpret_cast<void*>(static_cast<uintptr_t>(b.raw)), 0, MEM_RELEASE);
            InterlockedIncrement(&overflowFrees);
            InterlockedExchangeAdd(&overflowBytes, -static_cast<LONG>(b.size));
            return;
        }
    }
    freeHook.thiscall<void>(self, ptr, extra);
}
uint32_t __fastcall OnContains(void* self, void*, uint32_t ptr) {
    if (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)) == Heap() && Ours(ptr, nullptr)) return 1;
    return containsHook.thiscall<uint32_t>(self, ptr);
}
uint32_t __fastcall OnGetSize(void* self, void*, uint32_t ptr) {
    uint32_t size = 0;
    if (Ours(ptr, &size)) return size;
    return sizeHook.thiscall<uint32_t>(self, ptr);
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("MpMap", "SoundBankOverflow", 1, ini.c_str()) != 0;
    if (!enabled) { std::fprintf(log, "bankheap off ([MpMap] SoundBankOverflow=0): the Outbreak maps overflow the 96 MB sound bank heap\n"); return; }
    bool ok = true;
    for (const Site& s : kSites) {
        unsigned char seen[8]{};
        const bool match = Read(base + s.rva, seen, sizeof(seen)) && std::memcmp(seen, s.bytes, sizeof(seen)) == 0;
        if (!match) ok = false;
        std::fprintf(log, "bankheap sig %-8s %08X %s\n", s.name, static_cast<unsigned>(base + s.rva), match ? "ok" : "MISMATCH");
    }
    const uint32_t heap = Heap();
    uint32_t region[2]{};
    if (heap) Read(heap + 0x0C, region, sizeof(region));
    std::fprintf(log, "bankheap soundbank heap %08X: base %08X size %.1f MB\n", heap, region[0], region[1] / 1048576.0);
    if (!ok) { std::fprintf(log, "bankheap NOT installed: the Outbreak maps overflow the sound bank heap\n"); return; }
    allocHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kSites[0].rva), reinterpret_cast<void*>(OnAlloc));
    freeHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kSites[1].rva), reinterpret_cast<void*>(OnFree));
    containsHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kSites[2].rva), reinterpret_cast<void*>(OnContains));
    sizeHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kSites[3].rva), reinterpret_cast<void*>(OnGetSize));
    installed = allocHook && freeHook && containsHook && sizeHook;
    std::fprintf(log, "bankheap %s: a bank the full heap refuses comes from VirtualAlloc\n", installed ? "installed" : "hook FAILED");
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    std::fprintf(log, "bankheap installed=%d refusals=%ld overflow_allocs=%ld frees=%ld live_blocks=%u live_mb=%.1f peak_mb=%.1f fails=%ld table_full=%ld\n", installed ? 1 : 0,
                 InterlockedCompareExchange(&refusals, 0, 0), InterlockedCompareExchange(&overflowAllocs, 0, 0), InterlockedCompareExchange(&overflowFrees, 0, 0),
                 blockCount, InterlockedCompareExchange(&overflowBytes, 0, 0) / 1048576.0, InterlockedCompareExchange(&peakBytes, 0, 0) / 1048576.0,
                 InterlockedCompareExchange(&overflowFails, 0, 0), InterlockedCompareExchange(&tableFull, 0, 0));
}
}  // namespace bankheap
