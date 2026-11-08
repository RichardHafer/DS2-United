// Dead Space 2 United by Hafer.
// Observe the texture resource handler (deadspace2.exe 0x013DC190) for a
// handful of texture identities: the skin's own textures and the base
// exploder's. A tg4h creates the texture entry, a tg4d looks the entry up by
// crc32(group + name) (0x013D4260) and downloads the pixels (0x013DBFA0).
// The first skin test drew black, so this records which of these steps the
// skin copies reach. Read only, off unless TextureProbe=1.
namespace texture_probe {
safetyhook::MidHook headerHook, lookupHook, downloadHook;
bool enabled = false;
struct Event {
    LONG ready = 0;
    unsigned stage = 0, crc = 0, value = 0, tick = 0;
};
Event events[64]{};
volatile LONG count = 0;
struct SkinEvent {
    LONG ready = 0;
    unsigned instance = 0, client = 0, mesh = 0, key = 0, material = 0, tick = 0;
    unsigned model = 0, modelOffset = 0;   // the MetaModel (resource id) the client holds, if one was found
};
SkinEvent skinEvents[128]{};
volatile LONG count_skin = 0;
const unsigned kWatched[] = {0x21C8CA45u, 0xF0828145u, 0xDC101A77u, 0xA4ADFA9Du, 0x75E7B19Du, 0x59752AAFu, 0x4D9599B1u};

bool Watched(unsigned crc) {
    for (unsigned w : kWatched) if (w == crc) return true;
    return false;
}

// esi is the resource: +4 its descriptor, whose +0xC word is the texture crc.
unsigned CrcOf(const SafetyHookContext& ctx) {
    unsigned descriptor = 0, crc = 0;
    if (!Read(ctx.esi + 4, &descriptor, 4) || !descriptor || !Read(descriptor + 0xC, &crc, 4)) return 0;
    return crc;
}

void Record(unsigned stage, unsigned crc, unsigned value) {
    if (!Watched(crc)) return;
    const LONG slot = InterlockedIncrement(&count) - 1;
    if (slot < 0 || slot >= static_cast<LONG>(std::size(events))) return;
    auto& event = events[slot];
    event.stage = stage; event.crc = crc; event.value = value; event.tick = GetTickCount();
    InterlockedExchange(&event.ready, 1);
}

void OnHeader(SafetyHookContext& ctx) { Record(0, CrcOf(ctx), ctx.edi); }          // tg4h branch, edi = descriptor word 2
void OnLookup(SafetyHookContext& ctx) { Record(1, CrcOf(ctx), ctx.eax); }          // after 0x013D4260: eax = texture index or -1
void OnDownload(SafetyHookContext& ctx) { Record(2, CrcOf(ctx), ctx.eax & 0xFF); } // after 0x013DBFA0: al = success

bool Install(FILE* log) {
    struct Site { uint32_t rva; unsigned char bytes[6]; void (*fn)(SafetyHookContext&); safetyhook::MidHook* hook; };
    Site sites[] = {
        {0xFDC1AA, {0x8B,0x46,0x18,0x6A,0x03,0x6A}, OnHeader, &headerHook},
        {0xFDC1D6, {0x83,0xC4,0x04,0x8D,0x4C,0x24}, OnLookup, &lookupHook},
        {0xFDC1F2, {0x8B,0x36,0x83,0xC4,0x10,0xFF}, OnDownload, &downloadHook},
    };
    for (auto& site : sites) {
        unsigned char actual[6]{};
        if (!Read(base + site.rva, actual, 6) || std::memcmp(actual, site.bytes, 6)) {
            std::fprintf(log, "texture_probe signature mismatch at %08X; not installed\n", static_cast<unsigned>(base + site.rva));
            return false;
        }
    }
    for (auto& site : sites) {
        *site.hook = safetyhook::create_mid(reinterpret_cast<void*>(base + site.rva), site.fn);
        if (!*site.hook) { std::fprintf(log, "texture_probe hook failed at %08X\n", static_cast<unsigned>(base + site.rva)); return false; }
    }
    std::fprintf(log, "texture_probe installed read_only=1 watched=%u\n", static_cast<unsigned>(std::size(kWatched)));
    return true;
}

// 0x007BC5E0 builds the skinned geometry of one mesh instance (ecx): +0x54 is
// the mesh resource whose submeshes carry the material keys at +0xC. Logged
// for the exploder's and the skin's material keys, so one run shows which mesh
// objects the walking figure was built from.
safetyhook::MidHook skinHook;
const unsigned kMaterialKeys[] = {0xAA47428Au, 0xAB2ABBAFu, 0x6A909653u, 0xFDEB49C1u, 0xB6FFE266u, 0x540ED047u};

void OnSkinBuild(SafetyHookContext& ctx) {
    uint32_t mesh = 0, subs = 0, subCount = 0, client = 0;
    if (!Read(ctx.ecx + 0x54, &mesh, 4) || !mesh || !Read(mesh + 0x50, &subs, 4) || !Read(mesh + 0x34, &subCount, 4) || subCount > 64) return;
    Read(ctx.ecx + 8, &client, 4);
    // which MetaModel does the client hold? walk its first 0x200 bytes for a
    // pointer to a payload starting with "ldMM" and read the id at +0x10
    unsigned model = 0, modelOffset = 0;
    for (unsigned off = 0; client && off < 0x200 && !model; off += 4) {
        uint32_t ptr = 0, magic = 0;
        if (!Read(client + off, &ptr, 4) || ptr < 0x10000 || !Read(ptr, &magic, 4) || magic != 0x4D4D646C) continue;
        Read(ptr + 0x10, &model, 4); modelOffset = off;
    }
    for (uint32_t i = 0; i < subCount; ++i) {
        uint32_t key = 0, material = 0;
        if (!Read(subs + i * 0xC0 + 0xC, &key, 4)) return;
        for (unsigned k : kMaterialKeys) {
            if (k != key) continue;
            Read(subs + i * 0xC0 + 0x54, &material, 4);
            const LONG slot = InterlockedIncrement(&count_skin) - 1;
            if (slot >= 0 && slot < static_cast<LONG>(std::size(skinEvents))) {
                auto& e = skinEvents[slot];
                e.instance = ctx.ecx; e.mesh = mesh; e.key = key; e.material = material; e.client = client; e.tick = GetTickCount();
                e.model = model; e.modelOffset = modelOffset;
                InterlockedExchange(&e.ready, 1);
            }
        }
    }
}

// 0x007BB2F0: MetaModelInstance takes its model. ecx is the instance, the
// stack argument the resource entry (its GUID at +0x1C) or null, in which case
// the model comes from the instance's own variable set. Logged with the
// caller's return address for the exploder's model and the skin's.
safetyhook::MidHook setModelHook;
struct SetModelEvent {
    LONG ready = 0; unsigned instance = 0, entry = 0, id = 0, caller = 0, tick = 0;
    unsigned packet = 0, count = 0;
    unsigned hashes[12]{}, types[12]{}, ids[12]{};   // the attribute packet the instance was built from
};
SetModelEvent setModelEvents[64]{};
volatile LONG count_setmodel = 0;

void OnSetModel(SafetyHookContext& ctx) {
    uint32_t entry = 0, caller = 0, id = 0;
    Read(ctx.esp + 4, &entry, 4); Read(ctx.esp, &caller, 4);
    if (entry) Read(entry + 0x1C, &id, 4);
    if (entry && id != 0xEDBBC721u && id != 0xFD8E5C54u) return;
    const LONG slot = InterlockedIncrement(&count_setmodel) - 1;
    if (slot < 0 || slot >= static_cast<LONG>(std::size(setModelEvents))) return;
    auto& e = setModelEvents[slot];
    e.instance = ctx.ecx; e.entry = entry; e.id = id; e.caller = caller; e.tick = GetTickCount();
    // the packet at instance+4: list at +0x1C, count (u16) at +0x20; each
    // attribute: hash +0x10, type +0x14, resolved resource entry +0x1C
    uint32_t packet = 0, list = 0; uint16_t attrCount = 0;
    if (Read(ctx.ecx + 4, &packet, 4) && packet && Read(packet + 0x1C, &list, 4) && Read(packet + 0x20, &attrCount, 2)) {
        e.packet = packet; e.count = attrCount;
        for (unsigned i = 0; i < attrCount && i < 12; ++i) {
            uint32_t attr = 0, resource = 0;
            if (!Read(list + i * 4, &attr, 4) || !attr) break;
            Read(attr + 0x10, &e.hashes[i], 4); Read(attr + 0x14, &e.types[i], 4);
            if (Read(attr + 0x1C, &resource, 4) && resource) Read(resource + 0x1C, &e.ids[i], 4);
        }
    }
    InterlockedExchange(&e.ready, 1);
}

bool InstallSkinProbe(FILE* log) {
    const unsigned char setModel[] = {0x8B,0x44,0x24,0x04,0x83,0xEC,0x0C,0x56,0x8B,0xF1,0x85,0xC0};
    unsigned char actualSet[sizeof(setModel)]{};
    if (Read(base + 0x3BB2F0, actualSet, sizeof(actualSet)) && !std::memcmp(actualSet, setModel, sizeof(setModel))) {
        setModelHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x3BB2F0), OnSetModel);
        std::fprintf(log, "set_model_probe %s\n", setModelHook ? "installed" : "hook failed");
    } else {
        std::fprintf(log, "set_model_probe signature mismatch\n");
    }
    const unsigned char expected[] = {0x55,0x8B,0xEC,0x83,0xEC,0x40,0x53,0x56,0x8B,0xF1,0xF6,0x46,0x62,0x04};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + 0x3BC5E0, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected))) {
        std::fprintf(log, "skin_probe signature mismatch; not installed\n"); return false;
    }
    skinHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x3BC5E0), OnSkinBuild);
    if (!skinHook) { std::fprintf(log, "skin_probe hook failed\n"); return false; }
    std::fprintf(log, "skin_probe installed read_only=1 keys=%u\n", static_cast<unsigned>(std::size(kMaterialKeys)));
    return true;
}

void Flush(FILE* log) {
    static unsigned nextSet = 0;
    while (nextSet < std::size(setModelEvents) && InterlockedCompareExchange(&setModelEvents[nextSet].ready, 1, 1) == 1) {
        const auto& e = setModelEvents[nextSet++];
        std::fprintf(log, "set_model instance=%08X entry=%08X id=%08X caller=%08X packet=%08X attrs=%u tick=%u\n", e.instance, e.entry, e.id, e.caller, e.packet, e.count, e.tick);
        for (unsigned i = 0; i < e.count && i < 12; ++i)
            std::fprintf(log, "    attr hash=%08X type=%u resource_id=%08X\n", e.hashes[i], e.types[i], e.ids[i]);
    }
    static unsigned nextSkin = 0;
    while (nextSkin < std::size(skinEvents) && InterlockedCompareExchange(&skinEvents[nextSkin].ready, 1, 1) == 1) {
        const auto& e = skinEvents[nextSkin++];
        std::fprintf(log, "skin_build instance=%08X client=%08X model=%08X at=+%X mesh=%08X key=%08X material=%08X tick=%u\n", e.instance, e.client, e.model, e.modelOffset, e.mesh, e.key, e.material, e.tick);
    }
    static unsigned next = 0;
    static const char* names[] = {"header", "lookup", "download"};
    while (next < std::size(events) && InterlockedCompareExchange(&events[next].ready, 1, 1) == 1) {
        const auto& event = events[next++];
        std::fprintf(log, "texture_probe stage=%s crc=%08X value=%08X tick=%u\n", names[event.stage % 3], event.crc, event.value, event.tick);
    }
}
}
