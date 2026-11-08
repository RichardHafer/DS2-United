// Dead Space 2 United by Hafer.
// Drawing the Outbreak maps (DS2DAT12). Their meshes keep the Xbox 360
// primitive type: 6 is a triangle strip there, restarted at index 0xFFFF, but a
// triangle fan in D3D9, which has no restart either. That tears every mesh.
// Mark de Rune's exe (Dead Space 2 Multiplayer\deadspace2.exe, its embedded
// Adapter.dll, export OutbreakBatchedDraw) fixes it around the engine's indexed
// draw, and he allowed the user to reuse his work. This is the same approach,
// written here:
//
// - Draw 0x013D2C20, cdecl (type, ib, start, count, obj): ib[0] is the
//   IDirect3DIndexBuffer9, ib+0x10 the packed min/max index; obj+5 == 2,
//   word obj+0x3C == 6, obj+0x30/+0x34 the draw's count and start, obj+0x88 the
//   CPU copy of its indices. Such a draw is stitched once into a plain strip
//   (degenerate joins, one extra index where the winding would flip) in a
//   managed index buffer of its own, cached on the original buffer through
//   SetPrivateData so it dies with it, and drawn as type 5. When that fails,
//   each restart segment is drawn on its own.
// - SetTexture 0x013C8070 (stage, texture) caches 20 stages at 0x0211C6DC;
//   Xbox materials pass higher stages, which are ignored.
// - The material setup in 0x006DAE82 indexes a 6 row table at 0x02033E78 with
//   +0xB0; Xbox materials hold larger values, so both loops skip those rows
//   (0x006DAF87 takes no texture, 0x006DAFBF jumps to 0x006DB081).
//
// The strip fix only runs while an Outbreak map is the visit's map; the guards
// only act on values the stock maps never hold.
namespace outbreakdraw {
constexpr uint32_t kDrawRva = 0xFD2C20, kSetTextureRva = 0xFC8070, kGuardTextureRva = 0x2DAF87, kGuardLoopRva = 0x2DAFBF;
constexpr uint32_t kGuardTextureNext = 0x2DAF9A, kGuardLoopExit = 0x2DB081;
const unsigned char kDrawBytes[] = {0x51, 0x80, 0x3D, 0xB4, 0x3A, 0xF6, 0x01, 0x00, 0x0F, 0x84};
const unsigned char kSetTextureBytes[] = {0x56, 0x8B, 0x74, 0x24, 0x0C, 0x57, 0x83, 0xFE, 0xFF};
const unsigned char kGuardTextureBytes[] = {0x8B, 0x8E, 0xB0, 0x00, 0x00, 0x00, 0x8D, 0x0C, 0x49};
const unsigned char kGuardLoopBytes[] = {0x33, 0xDB, 0x39, 0x5D, 0x0C, 0x0F, 0x8E};
constexpr uint32_t kD3dFmtIndex16 = 101, kUsageDynamic = 0x200, kUsageWriteOnly = 8, kPoolManaged = 1, kSpdIUnknown = 1;
constexpr uint32_t kMaxPerBuffer = 1024, kMaxSourceIndices = 0x800000;
// {6A1C2F44-8B57-4E0B-9C1D-3F0B5D2E7A61}, ours (not Mark's) for the cache on a buffer
const GUID kCacheGuid = {0x6A1C2F44, 0x8B57, 0x4E0B, {0x9C, 0x1D, 0x3F, 0x0B, 0x5D, 0x2E, 0x7A, 0x61}};

bool enabled = true, stripsInstalled = false, guardsInstalled = false;
safetyhook::InlineHook drawHook, setTextureHook;
safetyhook::MidHook guardTexture, guardLoop;
volatile LONG type6 = 0, matched = 0, converted = 0, cachedDraws = 0, buildFailures = 0, segmentDraws = 0, segments = 0, noCpuIndices = 0;
volatile LONG stageGuards = 0, tableGuards = 0, lines = 0;

// --- stitching: coop_strips.h -------------------------------------------------------
using coop_strips::Stitch;

// --- COM by slot -------------------------------------------------------------------
template <typename R, typename... A> R Com(void* self, unsigned slot, A... args) {
    using Fn = R(__stdcall*)(void*, A...);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(self))[slot])(self, args...);
}
void Release(void* unk) { if (unk) Com<ULONG>(unk, 2); }
struct BufferDesc { uint32_t format, type, usage, pool, size; };

// --- the cache on one index buffer (an IUnknown the buffer releases) ---------------
struct Entry { Entry* next; const uint16_t* src; uint32_t start, count, outCount, minmax; void* ib; };
struct Cache;
HRESULT __stdcall CacheQuery(Cache* self, REFIID riid, void** out);
ULONG __stdcall CacheAddRef(Cache* self);
ULONG __stdcall CacheRelease(Cache* self);
void* const kCacheVtable[3] = {reinterpret_cast<void*>(CacheQuery), reinterpret_cast<void*>(CacheAddRef), reinterpret_cast<void*>(CacheRelease)};
struct Cache {
    void* const* vtable;
    volatile LONG refs;
    uint32_t entries;
    Entry* buckets[64];
};
HRESULT __stdcall CacheQuery(Cache* self, REFIID riid, void** out) {
    static const GUID unknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    if (!out) return E_POINTER;
    if (std::memcmp(&riid, &unknown, sizeof(GUID)) != 0) { *out = nullptr; return E_NOINTERFACE; }
    *out = self;
    CacheAddRef(self);
    return S_OK;
}
ULONG __stdcall CacheAddRef(Cache* self) { return static_cast<ULONG>(InterlockedIncrement(&self->refs)); }
ULONG __stdcall CacheRelease(Cache* self) {
    const LONG left = InterlockedDecrement(&self->refs);
    if (left) return static_cast<ULONG>(left);
    for (Entry*& head : self->buckets) {
        while (head) {
            Entry* e = head;
            head = e->next;
            Release(e->ib);
            delete e;
        }
    }
    delete self;
    return 0;
}

// The buffer's cache, created and attached on first use. Returned with a
// reference the caller drops.
Cache* CacheOf(void* ib) {
    Cache* cache = nullptr;
    DWORD size = sizeof(cache);
    if (Com<HRESULT>(ib, 5, &kCacheGuid, static_cast<void*>(&cache), &size) >= 0 && cache && size == sizeof(cache)) {
        if (cache->vtable == kCacheVtable) return cache;   // GetPrivateData added the reference
        Release(cache);
        return nullptr;
    }
    cache = new (std::nothrow) Cache{};
    if (!cache) return nullptr;
    cache->vtable = kCacheVtable;
    cache->refs = 1;
    // with D3DSPD_IUNKNOWN the data argument is the object itself (as Mark passes it)
    if (Com<HRESULT>(ib, 4, &kCacheGuid, static_cast<const void*>(cache), static_cast<DWORD>(sizeof(cache)), kSpdIUnknown) < 0) {
        CacheRelease(cache);
        return nullptr;
    }
    return cache;   // our reference; the buffer holds its own
}

Entry* Build(Cache* cache, void* ib, const uint16_t* src, uint32_t start, uint32_t count, Entry*& bucket) {
    if (cache->entries >= kMaxPerBuffer || count > kMaxSourceIndices) return nullptr;
    BufferDesc desc{};
    if (Com<HRESULT>(ib, 13, &desc) < 0 || desc.format != kD3dFmtIndex16 || (desc.usage & kUsageDynamic)) return nullptr;
    const uint32_t indices = desc.size / 2;
    if (start > indices || count > indices - start) return nullptr;
    uint32_t minmax = 0;
    const uint32_t outCount = Stitch(src, count, nullptr, &minmax);
    Entry* e = new (std::nothrow) Entry{nullptr, src, start, count, outCount, minmax, nullptr};
    if (!e) return nullptr;
    if (outCount) {
        void* device = nullptr;
        if (Com<HRESULT>(ib, 3, &device) < 0 || !device) { delete e; return nullptr; }
        const HRESULT made = Com<HRESULT>(device, 27, outCount * 2u, kUsageWriteOnly, kD3dFmtIndex16, kPoolManaged, &e->ib, static_cast<void*>(nullptr));
        Release(device);
        void* data = nullptr;
        if (made < 0 || !e->ib || Com<HRESULT>(e->ib, 11, 0u, 0u, &data, 0u) < 0 || !data) { Release(e->ib); delete e; return nullptr; }
        uint32_t check = 0;
        const uint32_t written = Stitch(src, count, static_cast<uint16_t*>(data), &check);
        Com<HRESULT>(e->ib, 12);
        if (written != outCount || check != minmax) { Release(e->ib); delete e; return nullptr; }
    }
    e->next = bucket;
    bucket = e;
    ++cache->entries;
    return e;
}

bool Active() {
    const int index = mpmap::mapIndex;
    return mpmap::InVisit() && index >= 0 && index < static_cast<int>(coop_mpmap::kMapCount) &&
           coop_mpmap::kMapTable[index].kind == coop_mpmap::MapKind::Outbreak;
}

using DrawFn = uint32_t(__cdecl*)(uint32_t, uint32_t*, uint32_t, uint32_t, uint8_t*);
uint32_t CallDraw(uint32_t type, uint32_t* ib, uint32_t start, uint32_t count, uint8_t* obj) {
    return drawHook.ccall<uint32_t>(type, ib, start, count, obj);
}
// Each restart segment on its own, from the original buffer (Mark's fallback).
uint32_t DrawSegments(uint32_t* ib, uint32_t start, uint32_t count, uint8_t* obj, const uint16_t* src) {
    uint32_t local[6];
    std::memcpy(local, ib, sizeof(local));
    uint32_t result = 0, i = 0;
    InterlockedIncrement(&segmentDraws);
    while (i < count) {
        while (i < count && src[i] == 0xFFFF) ++i;
        const uint32_t first = i;
        uint32_t lo = 0xFFFF, hi = 0;
        while (i < count && src[i] != 0xFFFF) { if (src[i] < lo) lo = src[i]; if (src[i] > hi) hi = src[i]; ++i; }
        if (i - first < 3) continue;
        local[4] = lo | hi << 16;
        InterlockedIncrement(&segments);
        result = CallDraw(5, local, start + first, i - first, obj);
    }
    return result;
}

// The draw itself never reads its fifth argument, so only the seven callers
// that pass the mesh there (the ones Mark redirects) are trusted with it.
constexpr uint32_t kMeshCallers[] = {0x3956A3, 0x395C0F, 0x395D14, 0x39822B, 0x3983CD, 0x443440, 0x44E153};   // return RVAs
bool MeshCaller(uint32_t ret) {
    for (const uint32_t r : kMeshCallers) if (ret == base + r) return true;
    return false;
}

uint32_t __cdecl OnDraw(uint32_t type, uint32_t* ib, uint32_t start, uint32_t count, uint8_t* obj) {
    if (type != 6 || !ib || !obj || !Active() || !MeshCaller(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()))))
        return CallDraw(type, ib, start, count, obj);
    InterlockedIncrement(&type6);
    if (obj[5] != 2 || *reinterpret_cast<uint16_t*>(obj + 0x3C) != 6 || *reinterpret_cast<uint32_t*>(obj + 0x30) != count ||
        *reinterpret_cast<uint32_t*>(obj + 0x34) != start)
        return CallDraw(type, ib, start, count, obj);
    InterlockedIncrement(&matched);
    const uint16_t* src = *reinterpret_cast<const uint16_t**>(obj + 0x88);
    void* buffer = reinterpret_cast<void*>(static_cast<uintptr_t>(ib[0]));
    if (!src) { InterlockedIncrement(&noCpuIndices); return CallDraw(type, ib, start, count, obj); }
    if (!buffer) return DrawSegments(ib, start, count, obj, src);
    Cache* cache = CacheOf(buffer);
    if (!cache) return DrawSegments(ib, start, count, obj, src);
    Entry*& bucket = cache->buckets[((static_cast<uint32_t>(reinterpret_cast<uintptr_t>(src)) >> 2) ^ start ^ count) & 63];
    Entry* e = bucket;
    while (e && !(e->src == src && e->start == start && e->count == count)) e = e->next;
    const bool fresh = !e;
    if (!e) {
        e = Build(cache, buffer, src, start, count, bucket);
        if (e) {
            InterlockedIncrement(&converted);
            if (InterlockedIncrement(&lines) <= 6)
                mpmap::Event("outbreakdraw: strip %08X start %u count %u -> %u indices, range %u..%u", static_cast<unsigned>(reinterpret_cast<uintptr_t>(src)),
                             start, count, e->outCount, e->minmax & 0xFFFF, e->minmax >> 16);
        } else {
            InterlockedIncrement(&buildFailures);
        }
    }
    uint32_t result = 0;
    if (e && e->ib) {
        uint32_t local[6];
        std::memcpy(local, ib, sizeof(local));
        local[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->ib));
        local[4] = e->minmax;
        if (!fresh) InterlockedIncrement(&cachedDraws);
        result = CallDraw(5, local, 0, e->outCount, obj);
    } else if (!e) {
        result = DrawSegments(ib, start, count, obj, src);
    }   // a draw with no full segment draws nothing
    CacheRelease(cache);
    return result;
}

uint32_t __cdecl OnSetTexture(uint32_t stage, uint32_t texture) {
    if (stage >= 0x14) {
        if (InterlockedIncrement(&stageGuards) <= 4) mpmap::Event("outbreakdraw: texture stage %u ignored (the cache has 20)", stage);
        return 0;
    }
    return setTextureHook.ccall<uint32_t>(stage, texture);
}
void OnGuardTexture(safetyhook::Context& ctx) {
    uint32_t row = 0;
    if (!Read(ctx.esi + 0xB0, &row, 4) || row < 6) return;
    InterlockedIncrement(&tableGuards);
    ctx.ecx = 0;
    ctx.eip = base + kGuardTextureNext;
}
void OnGuardLoop(safetyhook::Context& ctx) {
    uint32_t row = 0;
    if (!Read(ctx.esi + 0xB0, &row, 4) || row < 6) return;
    InterlockedIncrement(&tableGuards);
    ctx.eip = base + kGuardLoopExit;
}

bool Matches(FILE* log, const char* name, uint32_t rva, const unsigned char* bytes, size_t size) {
    unsigned char seen[16]{};
    const bool ok = size <= sizeof(seen) && Read(base + rva, seen, size) && std::memcmp(seen, bytes, size) == 0;
    std::fprintf(log, "outbreakdraw sig %-16s %08X %s\n", name, static_cast<unsigned>(base + rva), ok ? "ok" : "MISMATCH");
    return ok;
}
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("MpMap", "OutbreakRender", 1, ini.c_str()) != 0;
    if (!enabled) { std::fprintf(log, "outbreakdraw off ([MpMap] OutbreakRender=0): Outbreak meshes draw torn\n"); return; }
    if (Matches(log, "Draw", kDrawRva, kDrawBytes, sizeof(kDrawBytes))) {
        drawHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kDrawRva), reinterpret_cast<void*>(OnDraw));
        stripsInstalled = static_cast<bool>(drawHook);
    }
    bool guards = true;
    if (Matches(log, "SetTexture", kSetTextureRva, kSetTextureBytes, sizeof(kSetTextureBytes))) {
        setTextureHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kSetTextureRva), reinterpret_cast<void*>(OnSetTexture));
        guards = static_cast<bool>(setTextureHook) && guards;
    } else {
        guards = false;
    }
    if (Matches(log, "material texture", kGuardTextureRva, kGuardTextureBytes, sizeof(kGuardTextureBytes))) {
        auto made = safetyhook::MidHook::create(reinterpret_cast<void*>(base + kGuardTextureRva), OnGuardTexture);
        if (made) guardTexture = std::move(*made); else guards = false;
    } else {
        guards = false;
    }
    if (Matches(log, "material loop", kGuardLoopRva, kGuardLoopBytes, sizeof(kGuardLoopBytes))) {
        auto made = safetyhook::MidHook::create(reinterpret_cast<void*>(base + kGuardLoopRva), OnGuardLoop);
        if (made) guardLoop = std::move(*made); else guards = false;
    } else {
        guards = false;
    }
    guardsInstalled = guards;
    std::fprintf(log, "outbreakdraw strips %s, guards %s (Xbox strips restitched on Outbreak maps; after Mark de Rune's Adapter.dll)\n",
                 stripsInstalled ? "installed" : "NOT installed", guardsInstalled ? "installed" : "NOT all installed");
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    std::fprintf(log, "outbreakdraw strips=%d guards=%d type6=%ld matched=%ld converted=%ld cached_draws=%ld build_failures=%ld segment_draws=%ld segments=%ld "
                      "no_cpu_indices=%ld stage_guards=%ld table_guards=%ld\n",
                 stripsInstalled ? 1 : 0, guardsInstalled ? 1 : 0, InterlockedCompareExchange(&type6, 0, 0), InterlockedCompareExchange(&matched, 0, 0),
                 InterlockedCompareExchange(&converted, 0, 0), InterlockedCompareExchange(&cachedDraws, 0, 0), InterlockedCompareExchange(&buildFailures, 0, 0),
                 InterlockedCompareExchange(&segmentDraws, 0, 0), InterlockedCompareExchange(&segments, 0, 0), InterlockedCompareExchange(&noCpuIndices, 0, 0),
                 InterlockedCompareExchange(&stageGuards, 0, 0), InterlockedCompareExchange(&tableGuards, 0, 0));
}
}  // namespace outbreakdraw
