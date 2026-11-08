// Dead Space 2 United by Hafer.
// Observe the native material lookup and the geometry instance copy.
safetyhook::MidHook materialLookupProbe, materialCopyProbe;
struct MaterialEvent {
    LONG ready = 0;
    unsigned stage = 0, mesh = 0, hash = 0, material = 0, tick = 0;
};
MaterialEvent materialEvents[64]{};
volatile LONG materialDetailCount = 0, materialLookupCalls = 0, materialCopyCalls = 0, materialNulls = 0;

void RecordMaterial(unsigned stage, unsigned mesh, unsigned material) {
    unsigned hash = 0;
    if (!Read(mesh + 0xC, &hash, 4) || (hash != 0x66A81894 && hash != 0x4CDF2147)) return;
    InterlockedIncrement(stage ? &materialCopyCalls : &materialLookupCalls);
    if (!material) InterlockedIncrement(&materialNulls);
    const LONG slot = InterlockedIncrement(&materialDetailCount) - 1;
    if (slot < 0 || slot >= static_cast<LONG>(std::size(materialEvents))) return;
    auto& event = materialEvents[slot];
    event.stage = stage;
    event.mesh = mesh;
    event.hash = hash;
    event.material = material;
    event.tick = GetTickCount();
    InterlockedExchange(&event.ready, 1);
}

void OnMaterialLookup(SafetyHookContext& ctx) {
    RecordMaterial(0, ctx.edi, ctx.eax);
}

void OnMaterialCopy(SafetyHookContext& ctx) {
    unsigned material = 0;
    if (Read(ctx.esi + 0x54, &material, 4)) RecordMaterial(1, ctx.esi, material);
}

bool InstallMaterialProbe(FILE* log) {
    const unsigned char lookup[] = {0xE8,0x46,0x04,0x06,0x00,0x33,0xDB,0x83,0xC4,0x04,0x89,0x47,0x54};
    const unsigned char copy[] = {0x8B,0x4E,0x0C,0x51,0xE8,0x42,0x0A,0x06,0x00};
    unsigned char actual[sizeof(lookup)]{};
    if (!Read(base + 0x3C4D05, actual, sizeof(lookup)) || std::memcmp(actual, lookup, sizeof(lookup)) ||
        !Read(base + 0x3C46E5, actual, sizeof(copy)) || std::memcmp(actual, copy, sizeof(copy))) {
        std::fprintf(log, "material_probe signature_mismatch; not installed\n"); return false;
    }
    materialLookupProbe = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x3C4D0A), OnMaterialLookup);
    materialCopyProbe = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x3C46E5), OnMaterialCopy);
    if (!materialLookupProbe || !materialCopyProbe) {
        materialLookupProbe.reset(); materialCopyProbe.reset();
        std::fprintf(log, "material_probe hook_failed\n"); return false;
    }
    std::fprintf(log, "material_probe installed read_only=1 hashes=66A81894,4CDF2147 detail_limit=64\n");
    return true;
}

void FlushMaterialProbe(FILE* log) {
    static unsigned next = 0;
    static LONG last = -1;
    while (next < std::size(materialEvents) && InterlockedCompareExchange(&materialEvents[next].ready, 1, 1) == 1) {
        const auto& event = materialEvents[next++];
        std::fprintf(log, "material_binding stage=%s mesh=%08X hash=%08X material=%08X bound=%d tick=%u\n",
            event.stage ? "instance_copy" : "native_lookup", event.mesh, event.hash,
            event.material, event.material != 0, event.tick);
    }
    const LONG count = InterlockedCompareExchange(&materialDetailCount, 0, 0);
    if (count != last) {
        std::fprintf(log, "material_summary lookup=%ld copy=%ld null=%ld details=%u/64\n",
            InterlockedCompareExchange(&materialLookupCalls, 0, 0),
            InterlockedCompareExchange(&materialCopyCalls, 0, 0),
            InterlockedCompareExchange(&materialNulls, 0, 0), next);
        last = count;
    }
}
