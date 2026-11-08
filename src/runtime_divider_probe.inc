// Dead Space 2 United by Hafer.
// Divider probe: why a pool Divider core drops no parts.
//
// Read-only observation of the two retail functions that turn a severed
// limb (or the core's death) into a crawling DividerPart, found through the
// core's own vtable (01B84410, slot 133) in dump 32212 (2026-09-15):
//
//   0x012F68D0  DividerCore::OnDismember(event)
//               flag 0x800 in event+0x54 marks a part as gone (bit in
//               core+0x950); then the shared NPC handler 0x01015010; then,
//               when the dismemberable health (+0x240) changed and the
//               event carries 0x200, the vtable predicate at slot 128 gates
//               the death split into all five parts; otherwise one part
//   0x012E6070  DividerCore::SpawnDividerPart(event, index)
//               index < 5 and its bit in core+0x950 clear; the reference
//               GUID sits at attributes+0x1F0+16*index (CMD_SetHead/LeftHand/
//               RightHand/LeftFoot/RightFootDividerPartRef); 0x00776440 looks
//               it up in the global GUID table (hash chain, not scoped to a
//               stream), 0x00776480 creates the packet (the pool_children
//               hook lives inside it), message 332094AC yields the actor
//
// Every stage is logged. The playtest of 2026-09-15 02:08 answered it at the
// first stage: the five part references are not authored on the record, they
// are class defaults, and the defaults name the retail part templates of
// npcs_transport_hub_1 (58CB7942-...-43495250-30393645). The pool carries
// copies of exactly those five, rewritten into the RNDM/POOL namespace with
// the first two words kept, but a pool core still asks for the retail ones,
// and outside the transport hub the lookup returns nothing (found=0 for every
// limb, no part ever spawned). So the one write this file makes is a register:
// at the lookup, when the retail GUID is not registered and its pool twin is,
// the pointer the lookup gets is moved to the twin. Retail cores in the
// transport hub keep their own templates, the lookup finds those first.
namespace divider_probe {
safetyhook::MidHook spawnEntry, spawnLookup, spawnCreate, spawnActor, dismemberEntry, dismemberSplit, spawnRedirect;
bool installed = false;
constexpr uint32_t kPoolTag0 = 0x4D444E52u, kPoolTag1 = 0x4C4F4F50u;
Guid redirectedGuids[5]{};
volatile LONG redirects = 0, redirectMisses = 0;
struct Event {
    LONG ready = 0;
    unsigned stage = 0, core = 0, index = 0, mask = 0, a = 0, b = 0, tick = 0;
    Guid guid{};
};
Event events[96]{};
volatile LONG count = 0;

void Record(unsigned stage, unsigned core, unsigned index, unsigned a, unsigned b, const Guid* guid) {
    const LONG slot = InterlockedIncrement(&count) - 1;
    if (slot < 0 || slot >= static_cast<LONG>(std::size(events))) return;
    auto& event = events[slot];
    event.stage = stage;
    event.core = core;
    event.index = index;
    event.a = a;
    event.b = b;
    event.mask = 0;
    Read(core + 0x950, &event.mask, 4);
    if (guid) event.guid = *guid;
    event.tick = GetTickCount();
    InterlockedExchange(&event.ready, 1);
}

// 0x012E6070: ecx = core, [esp+4] = event, [esp+8] = part index.
void OnSpawnEntry(SafetyHookContext& ctx) {
    uint32_t index = 0, attributes = 0, flags = 0, event = 0;
    Guid guid{};
    Read(ctx.esp + 8, &index, 4);
    Read(ctx.esp + 4, &event, 4);
    if (event) Read(event + 0x54, &flags, 4);
    if (Read(ctx.ecx + 0xC, &attributes, 4) && attributes && index < 5)
        Read(attributes + 0x1F0 + 16 * index, guid.data(), 16);
    Record(0, ctx.ecx, index, flags, attributes, &guid);
}
// 0x012E60C5: after the GUID lookup; [esp+0x5C] is the record it found.
void OnSpawnLookup(SafetyHookContext& ctx) {
    uint32_t found = 0, index = 0;
    Read(ctx.esp + 0x5C, &found, 4);
    Read(ctx.ebp + 0xC, &index, 4);
    Record(1, ctx.edi, index, found, 0, nullptr);
}
// 0x012E60E8: after the create; ebx is the packet.
void OnSpawnCreate(SafetyHookContext& ctx) {
    uint32_t index = 0, handle = 0;
    Read(ctx.ebp + 0xC, &index, 4);
    if (ctx.ebx) Read(ctx.ebx + 8, &handle, 4);
    Record(2, ctx.edi, index, ctx.ebx, handle, nullptr);
}
// 0x012E6121: after the actor query; esi is the part actor, ebx the packet.
void OnSpawnActor(SafetyHookContext& ctx) {
    uint32_t index = 0;
    Read(ctx.ebp + 0xC, &index, 4);
    Record(3, ctx.edi, index, ctx.esi, ctx.ebx, nullptr);
}
// 0x012F68D0: ecx = core, [esp+4] = dismemberment event.
void OnDismemberEntry(SafetyHookContext& ctx) {
    uint32_t event = 0, flags = 0, limb = 0;
    Read(ctx.esp + 4, &event, 4);
    if (event) { Read(event + 0x54, &flags, 4); Read(event + 0x58, &limb, 4); }
    Record(4, ctx.ecx, 0xFFFFFFFFu, flags, limb, nullptr);
}
// 0x012F694F: ebp = health+0x240 before, ebx = health component, edi = event.
void OnDismemberSplit(SafetyHookContext& ctx) {
    uint32_t after = 0, flags = 0;
    Read(ctx.ebx + 0x240, &after, 4);
    Read(ctx.edi + 0x54, &flags, 4);
    Record(5, ctx.esi, flags, ctx.ebp, after, nullptr);
}

// 0x012E60B6: ecx points at the reference GUID, ebx is the entity manager,
// edi the core, [ebp+0xC] the part index. Same lookup the game makes next
// (0x00776310, this = manager, args guid pointer and an ignored dword).
void OnSpawnRedirect(SafetyHookContext& ctx) {
    using Find = void* (__thiscall*)(void*, const Guid*, unsigned);
    Guid retail{};
    uint32_t index = 0;
    if (!ctx.ebx || !Read(ctx.ecx, retail.data(), 16) || !Read(ctx.ebp + 0xC, &index, 4) || index >= 5) return;
    if (retail[2] == kPoolTag0 && retail[3] == kPoolTag1) return;
    const auto find = reinterpret_cast<Find>(base + 0x376310);
    if (find(reinterpret_cast<void*>(ctx.ebx), &retail, 0)) return;
    Guid twin{retail[0], retail[1], kPoolTag0, kPoolTag1};
    if (!find(reinterpret_cast<void*>(ctx.ebx), &twin, 0)) {
        InterlockedIncrement(&redirectMisses);
        Record(6, ctx.edi, index, 0, 0, &twin);
        return;
    }
    redirectedGuids[index] = twin;
    ctx.ecx = reinterpret_cast<uintptr_t>(&redirectedGuids[index]);
    InterlockedIncrement(&redirects);
    Record(6, ctx.edi, index, 1, 0, &twin);
}

bool Install(FILE* log) {
    struct Site { uint32_t rva; const char* bytes; size_t size; void (*fn)(SafetyHookContext&); safetyhook::MidHook* hook; };
    const Site sites[] = {
        {0xEE6070, "\x55\x8B\xEC\x83\xE4\xF0\x83\xEC\x64\x53\x56\x57", 12, OnSpawnEntry, &spawnEntry},
        {0xEE60C5, "\x83\x7C\x24\x5C\x00\x0F\x84\x20\x02\x00\x00", 11, OnSpawnLookup, &spawnLookup},
        {0xEE60E8, "\x85\xDB\x0F\x84\x00\x02\x00\x00", 8, OnSpawnCreate, &spawnCreate},
        {0xEE6121, "\x85\xF6\x0F\x84\xBE\x01\x00\x00", 8, OnSpawnActor, &spawnActor},
        {0xEF68D0, "\x53\x55\x56\x8B\xF1\x57\x85\xF6\x74\x11\x0F\xB6", 12, OnDismemberEntry, &dismemberEntry},
        {0xEF694F, "\x3B\xAB\x40\x02\x00\x00\x74\x6F\xF7\x47\x54\x00", 12, OnDismemberSplit, &dismemberSplit},
        {0xEE60B6, "\x6A\x00\x51\x8D\x44\x24\x54\x50\x8B\xCB\xE8\x7B\x03\x49\xFF", 15, OnSpawnRedirect, &spawnRedirect},
    };
    for (const auto& site : sites) {
        unsigned char actual[16]{};
        if (!Read(base + site.rva, actual, site.size) || std::memcmp(actual, site.bytes, site.size)) {
            std::fprintf(log, "divider_probe signature_mismatch at %08X; not installed\n",
                         static_cast<unsigned>(base + site.rva));
            return false;
        }
    }
    for (const auto& site : sites) {
        *site.hook = safetyhook::create_mid(reinterpret_cast<void*>(base + site.rva), site.fn);
        if (!*site.hook) {
            for (const auto& undo : sites) undo.hook->reset();
            std::fprintf(log, "divider_probe hook_failed at %08X\n", static_cast<unsigned>(base + site.rva));
            return false;
        }
    }
    installed = true;
    std::fprintf(log, "divider_probe installed read_only=0 part_redirect=1 spawn_part=%08X on_dismember=%08X detail_limit=%u\n",
                 static_cast<unsigned>(base + 0xEE6070), static_cast<unsigned>(base + 0xEF68D0),
                 static_cast<unsigned>(std::size(events)));
    return true;
}

void Flush(FILE* log) {
    static unsigned next = 0;
    static const char* const names[] = {"spawn_part_entry", "spawn_part_lookup", "spawn_part_create",
                                        "spawn_part_actor", "dismember_entry", "dismember_split", "spawn_part_redirect"};
    while (next < std::size(events) && InterlockedCompareExchange(&events[next].ready, 1, 1) == 1) {
        const auto& e = events[next++];
        switch (e.stage) {
        case 0:
            std::fprintf(log, "divider_probe stage=%s core=%08X index=%u mask=%08X event_flags=%08X attrs=%08X ref=%08X-%08X-%08X-%08X tick=%u\n",
                         names[e.stage], e.core, e.index, e.mask, e.a, e.b, e.guid[0], e.guid[1], e.guid[2], e.guid[3], e.tick);
            break;
        case 1:
            std::fprintf(log, "divider_probe stage=%s core=%08X index=%u mask=%08X record=%08X found=%d tick=%u\n",
                         names[e.stage], e.core, e.index, e.mask, e.a, e.a != 0, e.tick);
            break;
        case 2:
            std::fprintf(log, "divider_probe stage=%s core=%08X index=%u mask=%08X packet=%08X packet_handle=%08X tick=%u\n",
                         names[e.stage], e.core, e.index, e.mask, e.a, e.b, e.tick);
            break;
        case 3:
            std::fprintf(log, "divider_probe stage=%s core=%08X index=%u mask=%08X actor=%08X packet=%08X tick=%u\n",
                         names[e.stage], e.core, e.index, e.mask, e.a, e.b, e.tick);
            break;
        case 4:
            std::fprintf(log, "divider_probe stage=%s core=%08X mask=%08X event_flags=%08X limb=%08X gone=%d fatal=%d tick=%u\n",
                         names[e.stage], e.core, e.mask, e.a, e.b, (e.a & 0x800) != 0, (e.a & 0x200) != 0, e.tick);
            break;
        case 6:
            std::fprintf(log, "divider_probe stage=%s core=%08X index=%u twin=%08X-%08X-%08X-%08X redirected=%d tick=%u\n",
                         names[e.stage], e.core, e.index, e.guid[0], e.guid[1], e.guid[2], e.guid[3], e.a, e.tick);
            break;
        default:
            std::fprintf(log, "divider_probe stage=%s core=%08X mask=%08X event_flags=%08X health240_before=%08X after=%08X changed=%d tick=%u\n",
                         names[e.stage], e.core, e.mask, e.index, e.a, e.b, e.a != e.b, e.tick);
            break;
        }
    }
}
}  // namespace divider_probe
