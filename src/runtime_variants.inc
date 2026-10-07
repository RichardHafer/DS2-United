// Speed and size for the stat variants, applied at runtime.
//
// Neither is authored. The animation playback factor lives in the CCT AnimView,
// which the Xbox PDB lays out as m_AnimSpeed at +0x174 with uniformScale and
// invUniformScale at +0x148/+0x14C. On PC the layout is the same (checked
// against 58 live animated objects in two dumps: parentObject at +0x144 points
// back at the actor) and the actor keeps a pointer to the view's +0x40 at
// actor+0x194. Only two things write m_AnimSpeed: the view's constructor
// (1.0) and Animated::SetAnimSpeed at 0x00751F40, which the game calls for
// stasis and a few behaviours. So a variant's speed is written once when the
// actor is created and folded into every SetAnimSpeed the game makes for it
// afterwards, which keeps stasis working (it slows a fast enemy down by the
// same proportion). The size is the view's uniform scale, the same visual
// only scale DS1 had: the collision capsule does not grow with it.
//
// The table below remembers which actors are variants. It is only ever
// touched on the game thread, from the post-create observer and from the
// SetAnimSpeed hook, and an entry is verified by vtable and GUID before use,
// the way the watchdog does it, because an actor address is reused.
namespace variant_runtime {
safetyhook::MidHook speedHook;
bool enabled = false;
volatile LONG speedApplied = 0, scaleApplied = 0, viewMissing = 0, callsScaled = 0;
constexpr uint32_t kViewInner = 0x194;      // actor -> AnimView + 0x40
constexpr uint32_t kSpeed = 0x134;          // inner + 0x134 = view + 0x174
constexpr uint32_t kScale = 0x108;          // inner + 0x108 = view + 0x148
constexpr uint32_t kInvScale = 0x10C;
struct Entry {
    uint32_t actor = 0, vtable = 0;
    Guid guid{};
    float speed = 1.0f;
};
constexpr unsigned kEntries = 64;
Entry entries[kEntries];
unsigned nextEntry = 0;

bool WriteFloat(uint32_t address, float value) {
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &value, 4, &written) && written == 4;
}

// Game thread, from the post-create observer, once the actor exists.
void OnVariantCreated(uint32_t actor, const ds2::EnemyRecord& record) {
    if (!enabled || !actor) return;
    const float speed = record.speedPercent / 100.0f, scale = record.scalePercent / 100.0f;
    if (record.speedPercent == 100 && record.scalePercent == 100) return;
    uint32_t inner = 0, vtable = 0;
    Guid guid{};
    if (!Read(actor + kViewInner, &inner, 4) || !inner || !Read(actor, &vtable, 4) ||
        !Read(actor + 0x24, guid.data(), 16)) {
        InterlockedIncrement(&viewMissing);
        return;
    }
    if (record.speedPercent != 100 && WriteFloat(inner + kSpeed, speed)) InterlockedIncrement(&speedApplied);
    if (record.scalePercent != 100 && scale > 0.0f &&
        WriteFloat(inner + kScale, scale) && WriteFloat(inner + kInvScale, 1.0f / scale))
        InterlockedIncrement(&scaleApplied);
    auto& slot = entries[nextEntry++ % kEntries];
    slot.actor = actor; slot.vtable = vtable; slot.guid = guid; slot.speed = speed;
}

// 0x00751F40, Animated::SetAnimSpeed(float): ecx is the actor, the value is
// the first stack argument. A variant's factor is folded into it.
void OnSetAnimSpeed(SafetyHookContext& ctx) {
    if (!enabled) return;
    for (const auto& entry : entries) {
        if (!entry.actor || entry.actor != ctx.ecx || entry.speed == 1.0f) continue;
        uint32_t vtable = 0;
        Guid guid{};
        // A stale entry for a reused address must not hide a newer one behind it.
        if (!Read(entry.actor, &vtable, 4) || vtable != entry.vtable ||
            !Read(entry.actor + 0x24, guid.data(), 16) || guid != entry.guid) continue;
        float value = 0.0f;
        if (!Read(ctx.esp + 4, &value, 4)) return;
        if (WriteFloat(ctx.esp + 4, value * entry.speed)) InterlockedIncrement(&callsScaled);
        return;
    }
}

// Entity::AttachModel (0x007D2230): ecx is the entity, the stack argument the
// resource entry of the model it is about to instantiate (its GUID at +0x1C,
// the MetaModel at +0x14). For a variant with a skin the engine arrives here
// with the base model whatever the record, the model stream or the character
// say (12.09., skin_build/set_model logs), so the entry is swapped for the
// skin's own MetaModel, looked up by GUID the way the engine does it
// (0x00790650, cdecl, pointer to the four words, returns the table entry;
// its sibling 0x007906D0 returns the payload instead and crashed here, dump
// 45612).
safetyhook::MidHook attachHook;
volatile LONG attachSwapped = 0, attachMissing = 0, attachSeen = 0;

// Every AttachModel of an entity whose model is a skin's base or skin model,
// or whose record is a pool or variant copy, is remembered with the outcome,
// so a part that keeps the base look can be traced to its record.
struct AttachEvent { LONG ready = 0; Guid entity{}, model{}; unsigned outcome = 0, tick = 0; };
AttachEvent attachEvents[128]{};
volatile LONG attachCount = 0;

void RememberAttach(const Guid& entity, const Guid& model, unsigned outcome) {
    const LONG slot = InterlockedIncrement(&attachCount) - 1;
    if (slot < 0 || slot >= static_cast<LONG>(std::size(attachEvents))) return;
    auto& e = attachEvents[slot];
    e.entity = entity; e.model = model; e.outcome = outcome; e.tick = GetTickCount();
    InterlockedExchange(&e.ready, 1);
}

void OnAttachModel(SafetyHookContext& ctx) {
    if (!enabled) return;
    Guid guid{};
    if (!Read(ctx.ecx + 0x24, guid.data(), 16)) return;
    const Guid none{};
    const Guid* skinModel = nullptr;
    for (const auto& owner : ds2::kSkinOwners)
        if (owner.guid == guid && !(owner.skinModel == none)) { skinModel = &owner.skinModel; break; }
    uint32_t entry = 0;
    Guid current{};
    Read(ctx.esp + 4, &entry, 4);
    if (entry) Read(entry + 0x1C, current.data(), 16);
    // logged: a pool copy, a variant copy, or a model any skin is about
    const bool copy = guid[2] == 0x4D444E52u && (guid[3] == 0x4C4F4F50u || guid[3] == 0x56415249u);
    bool skinModelOfAnyone = false;
    for (const auto& owner : ds2::kSkinOwners)
        if (!(owner.skinModel == none) && (current == owner.skinModel)) skinModelOfAnyone = true;
    if (!skinModel) {
        if (copy || skinModelOfAnyone) RememberAttach(guid, current, 0);   // 0 = not an owner
        return;
    }
    InterlockedIncrement(&attachSeen);
    if (!entry) { RememberAttach(guid, current, 4); return; }            // 4 = owner without entry
    if (current == *skinModel) { RememberAttach(guid, current, 1); return; }   // 1 = already the skin
    using Lookup = uint32_t (__cdecl*)(const void*);
    const uint32_t replacement = reinterpret_cast<Lookup>(base + 0x390650)(skinModel->data());
    if (!replacement) { InterlockedIncrement(&attachMissing); RememberAttach(guid, current, 3); return; }   // 3 = skin not resident
    SIZE_T written = 0;
    if (WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(ctx.esp + 4), &replacement, 4, &written) && written == 4) {
        InterlockedIncrement(&attachSwapped);
        RememberAttach(guid, current, 2);                                 // 2 = swapped
    }
}

void FlushAttach(FILE* log) {
    static unsigned next = 0;
    static const char* names[] = {"not_owner", "already_skin", "swapped", "skin_missing", "no_entry"};
    while (next < std::size(attachEvents) && InterlockedCompareExchange(&attachEvents[next].ready, 1, 1) == 1) {
        const auto& e = attachEvents[next++];
        std::fprintf(log, "attach_model entity=%08X-%08X-%08X-%08X model=%08X-%08X-%08X-%08X outcome=%s tick=%u\n",
            e.entity[0], e.entity[1], e.entity[2], e.entity[3], e.model[0], e.model[1], e.model[2], e.model[3], names[e.outcome % 5], e.tick);
    }
}

// Sound events reach the audio system through the Lua bridge
// LuaToEarsAudioGlue::PlayEntityEmx (PC 0x007B07E0, Lua C function at
// 0x007B0BC0): argument 1 is the event name ("exploder_attack.emx"), argument 5
// the entity. The name is kept in a stack slot and loaded into edx at
// 0x007B0A33 for Entity::PlaySound (vtable+0xA8). At 0x007B0A37 edx is that
// name and ebx the entity (resolved through 0x404D72 with class hash
// 38523FC3). A skinned variant's events are renamed here, the DS1 way: the string in the
// animation bank turned out not to be what is played (13.09., "mixed" sounds
// with every bank layout), so the swap happens where the name is consumed.
// Every call of an owner, and of any entity whose name mentions a skin's
// event, is remembered for the log.
constexpr uint32_t kPlayEntityEmxRva = 0x3B0A37;
safetyhook::MidHook emxHook;
volatile LONG emxSeen = 0, emxSwapped = 0;
struct EmxEvent { LONG ready = 0; Guid entity{}; char name[48]{}; char to[48]{}; unsigned tick = 0; };
EmxEvent emxEvents[256]{};
volatile LONG emxCount = 0;

void RememberEmx(const Guid& entity, const char* name, const char* to) {
    const LONG slot = InterlockedIncrement(&emxCount) - 1;
    if (slot < 0 || slot >= static_cast<LONG>(std::size(emxEvents))) return;
    auto& e = emxEvents[slot];
    e.entity = entity;
    std::strncpy(e.name, name ? name : "", sizeof(e.name) - 1);
    std::strncpy(e.to, to ? to : "", sizeof(e.to) - 1);
    e.tick = GetTickCount();
    InterlockedExchange(&e.ready, 1);
}

void OnPlayEntityEmx(SafetyHookContext& ctx) {
    if (!enabled || !ctx.edx) return;
    // edx = the event name (Lua argument 1, loaded from its stack slot for the
    // PlaySound call), ebx = the entity
    char name[48]{};
    if (!Read(ctx.edx, name, sizeof(name) - 1) || !name[0]) return;
    Guid guid{};
    if (!ctx.ebx || !Read(ctx.ebx + 0x24, guid.data(), 16)) return;
    const Guid none{};
    const Guid* skinModel = nullptr;
    for (const auto& owner : ds2::kSkinOwners)
        if (owner.guid == guid && !(owner.skinModel == none)) { skinModel = &owner.skinModel; break; }
    // a row names the event with or without ".emx"; the handler may pass either
    auto matches = [](const char* from, const char* text) {
        const size_t n = std::strlen(from);
        if (std::strncmp(from, text, n) != 0) return false;
        return text[n] == 0 || std::strcmp(text + n, ".emx") == 0;
    };
    bool interesting = skinModel != nullptr;
    for (const auto& row : ds2::kSkinSounds)
        if (row.from[0] && matches(row.from, name)) interesting = true;
    if (!interesting) return;
    InterlockedIncrement(&emxSeen);
    const char* to = nullptr;
    if (skinModel)
        for (const auto& row : ds2::kSkinSounds)
            if (row.skinModel == *skinModel && matches(row.from, name)) { to = row.to; break; }
    static char replacementNames[32][48]{};
    static volatile LONG replacementNext = 0;
    if (to) {
        // the name buffer outlives the call: a fixed ring in the plugin image
        const LONG slot = (InterlockedIncrement(&replacementNext) - 1) % 32;
        char* buffer = replacementNames[slot];
        std::memset(buffer, 0, 48);
        std::strncpy(buffer, to, 40);
        const size_t fromLength = std::strlen(name), toLength = std::strlen(to);
        if (fromLength > 4 && std::strcmp(name + fromLength - 4, ".emx") == 0 && (toLength < 4 || std::strcmp(to + toLength - 4, ".emx") != 0))
            std::strncat(buffer, ".emx", 47 - std::strlen(buffer));
        ctx.edx = reinterpret_cast<uintptr_t>(buffer);
        InterlockedIncrement(&emxSwapped);
        to = buffer;
    }
    RememberEmx(guid, name, to);
}

void FlushEmx(FILE* log) {
    static unsigned next = 0;
    while (next < std::size(emxEvents) && InterlockedCompareExchange(&emxEvents[next].ready, 1, 1) == 1) {
        const auto& e = emxEvents[next++];
        std::fprintf(log, "play_emx entity=%08X-%08X-%08X-%08X name=%s %s%s tick=%u\n",
            e.entity[0], e.entity[1], e.entity[2], e.entity[3], e.name, e.to[0] ? "swapped_to=" : "kept", e.to, e.tick);
    }
}

// PlayEmx (no entity, PC 0x007B0580): the Lua handler of the character
// banks (aud_cct.lua) plays the mutterings this way. At 0x007B05B4 eax holds
// the name; names that mention a skin's rename key are logged, with a zero
// entity, so the log shows which names the handler asks for.
constexpr uint32_t kPlayEmxNameRva = 0x3B05B4;
safetyhook::MidHook emxPlainHook;

void OnPlayEmx(SafetyHookContext& ctx) {
    if (!enabled || !ctx.eax) return;
    char name[48]{};
    if (!Read(ctx.eax, name, sizeof(name) - 1)) return;
    bool interesting = false;
    for (const auto& row : ds2::kSkinSounds)
        if (row.from[0] && (std::strcmp(row.from, name) == 0 || std::strcmp(row.to, name) == 0)) interesting = true;
    if (!interesting && std::strstr(name, "explode") == nullptr) return;
    InterlockedIncrement(&emxSeen);
    RememberEmx(Guid{}, name, nullptr);
}

bool InstallEmx(FILE* log) {
    // lea ecx,[esp+20h] / mov edi,eax / call 0072A500
    const unsigned char plain[] = {0x8D, 0x4C, 0x24, 0x20, 0x8B, 0xF8, 0xE8, 0x41, 0x9F, 0xF7, 0xFF};
    unsigned char have[sizeof(plain)]{};
    if (Read(base + kPlayEmxNameRva, have, sizeof(have)) && std::memcmp(have, plain, sizeof(plain)) == 0) {
        emxPlainHook = safetyhook::create_mid(reinterpret_cast<void*>(base + kPlayEmxNameRva), OnPlayEmx);
        std::fprintf(log, "variant_runtime play_emx_plain %s at %08X\n", emxPlainHook ? "installed" : "failed", static_cast<unsigned>(base + kPlayEmxNameRva));
    } else
        std::fprintf(log, "variant_runtime play_emx_plain signature mismatch\n");
    // mov eax,[eax+0A8h] / lea ecx,[esp+30h] / push ecx / push edx / mov ecx,ebx / call eax
    const unsigned char expected[] = {0x8B, 0x80, 0xA8, 0x00, 0x00, 0x00, 0x8D, 0x4C, 0x24, 0x30, 0x51, 0x52, 0x8B, 0xCB, 0xFF, 0xD0};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kPlayEntityEmxRva, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected))) {
        std::fprintf(log, "variant_runtime play_emx signature mismatch; sound renames off\n");
        return false;
    }
    emxHook = safetyhook::create_mid(reinterpret_cast<void*>(base + kPlayEntityEmxRva), OnPlayEntityEmx);
    if (!emxHook) { std::fprintf(log, "variant_runtime play_emx hook failed; sound renames off\n"); return false; }
    unsigned rows = 0;
    for (const auto& row : ds2::kSkinSounds) if (row.from[0]) ++rows;
    std::fprintf(log, "variant_runtime play_emx installed at %08X renames=%u\n", static_cast<unsigned>(base + kPlayEntityEmxRva), rows);
    return true;
}

bool InstallAttach(FILE* log) {
    // sub esp,0Ch / push ebx / push esi / mov esi,ecx / cmp [esi+140h],0
    const unsigned char expected[] = {0x83,0xEC,0x0C,0x53,0x56,0x8B,0xF1,0x83,0xBE,0x40,0x01,0x00,0x00,0x00};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + 0x3D2230, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected))) {
        std::fprintf(log, "variant_runtime attach_model signature mismatch; skins off\n");
        return false;
    }
    attachHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x3D2230), OnAttachModel);
    if (!attachHook) { std::fprintf(log, "variant_runtime attach_model hook failed; skins off\n"); return false; }
    std::fprintf(log, "variant_runtime attach_model installed at %08X lookup=%08X\n", static_cast<unsigned>(base + 0x3D2230), static_cast<unsigned>(base + 0x390650));
    return true;
}

bool Install(FILE* log) {
    // push esi / mov esi,ecx / mov eax,[esi+194h] / test eax,eax / je
    const unsigned char expected[] = {0x56,0x8B,0xF1,0x8B,0x86,0x94,0x01,0x00,0x00,0x85,0xC0,0x74};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + 0x351F40, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected))) {
        std::fprintf(log, "variant_runtime set_anim_speed signature mismatch; speed and size off\n");
        return false;
    }
    speedHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x351F40), OnSetAnimSpeed);
    if (!speedHook) {
        std::fprintf(log, "variant_runtime hook failed; speed and size off\n");
        return false;
    }
    std::fprintf(log, "variant_runtime installed at %08X view=actor+0x%X speed=+0x%X scale=+0x%X\n",
        static_cast<unsigned>(base + 0x351F40), kViewInner, kSpeed, kScale);
    InstallAttach(log);
    InstallEmx(log);
    return true;
}

void Summary(FILE* log) {
    std::fprintf(log, "variant_runtime speed_applied=%ld scale_applied=%ld view_missing=%ld set_anim_speed_scaled=%ld skin_attach_seen=%ld swapped=%ld missing=%ld\n",
        InterlockedCompareExchange(&speedApplied, 0, 0), InterlockedCompareExchange(&scaleApplied, 0, 0),
        InterlockedCompareExchange(&viewMissing, 0, 0), InterlockedCompareExchange(&callsScaled, 0, 0),
        InterlockedCompareExchange(&attachSeen, 0, 0), InterlockedCompareExchange(&attachSwapped, 0, 0),
        InterlockedCompareExchange(&attachMissing, 0, 0));
}
}
