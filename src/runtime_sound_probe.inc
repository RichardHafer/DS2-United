// Dead Space 2 United by Hafer.
// Which enemies ask for sounds and which of those the audio system finds
// (DS2Randomizer.ini SoundProbe=1, read only). LuaToEarsAudioGlue::PlayEntityEmx
// (PC 0x007B07E0) calls Entity::PlaySound(name, opts) at 0x007B0A45. At
// 0x007B0A3D edx is the event name and ebx the entity; 0x007B0A51 is reached
// only when PlaySound returned an instance (esi). Counted per entity vtable (one
// per enemy family); the survival director names the vtables of its spawns.
// One run tells apart: the Lua handler never asks (no calls), the event is in
// no loaded bank (calls, not found), and played (found, with the instance's
// scene mask +0x5C and occlusion +0x70 sampled).
namespace sound_probe {
bool enabled = false;
safetyhook::MidHook callHook, foundHook;
constexpr uint32_t kCallRva = 0x3B0A3D, kFoundRva = 0x3B0A51;
struct Slot {
    volatile LONG vtable = 0, calls = 0, found = 0, misses = 0, empty = 0, missNamed = 0, sampled = 0;
    char family[24]{};   // from the director's spawns
    char missNames[3][40]{};
    char lastFound[40]{};
    uint32_t mask[3]{};
    float occlusion[3]{};
};
Slot slots[48];
volatile LONG overflow = 0, noEntity = 0, totalCalls = 0;
thread_local int pendingSlot = -1;
thread_local char pendingName[40];

int FindSlot(uint32_t vtable) {
    for (int i = 0; i < static_cast<int>(std::size(slots)); ++i) {
        const LONG have = InterlockedCompareExchange(&slots[i].vtable, static_cast<LONG>(vtable), 0);
        if (have == 0 || static_cast<uint32_t>(have) == vtable) return i;
    }
    InterlockedIncrement(&overflow);
    return -1;
}
// The director, for every enemy it spawned.
void NoteFamily(uint32_t vtable, const char* family) {
    if (!enabled || !vtable || !family) return;
    const int i = FindSlot(vtable);
    if (i >= 0 && !slots[i].family[0]) std::snprintf(slots[i].family, sizeof(slots[i].family), "%s", family);
}
void Miss(Slot& s, const char* name) {
    InterlockedIncrement(&s.misses);
    const LONG n = InterlockedIncrement(&s.missNamed) - 1;
    if (n >= 0 && n < 3) std::memcpy(s.missNames[n], name, sizeof(s.missNames[n]) - 1);
}
void OnCall(SafetyHookContext& ctx) {
    // the previous call on this thread never reached the found branch
    if (pendingSlot >= 0) Miss(slots[pendingSlot], pendingName);
    pendingSlot = -1;
    InterlockedIncrement(&totalCalls);
    uint32_t vtable = 0;
    if (!ctx.ebx || !Read(ctx.ebx, &vtable, 4) || !vtable) { InterlockedIncrement(&noEntity); return; }
    const int i = FindSlot(vtable);
    if (i < 0) return;
    InterlockedIncrement(&slots[i].calls);
    std::memset(pendingName, 0, sizeof(pendingName));
    if (!ctx.edx || !Read(ctx.edx, pendingName, sizeof(pendingName) - 1) || !pendingName[0]) InterlockedIncrement(&slots[i].empty);
    pendingSlot = i;
}
void OnFound(SafetyHookContext& ctx) {
    if (pendingSlot < 0) return;
    Slot& s = slots[pendingSlot];
    pendingSlot = -1;
    InterlockedIncrement(&s.found);
    std::memcpy(s.lastFound, pendingName, sizeof(s.lastFound) - 1);
    const LONG n = InterlockedIncrement(&s.sampled) - 1;
    if (n >= 0 && n < 3 && ctx.esi) {
        Read(ctx.esi + 0x5C, &s.mask[n], 4);
        Read(ctx.esi + 0x70, &s.occlusion[n], 4);
    }
}

void Install(FILE* log) {
    // lea ecx,[esp+30h] / push ecx / push edx / mov ecx,ebx / call eax / mov esi,eax / test esi,esi / jz
    static const unsigned char kCall[] = {0x8D, 0x4C, 0x24, 0x30, 0x51, 0x52, 0x8B, 0xCB, 0xFF, 0xD0, 0x8B, 0xF0, 0x85, 0xF6, 0x0F, 0x84};
    // cmp byte ptr [esp+0Eh],0 / jz +26h / mov edx,[ebx]
    static const unsigned char kFound[] = {0x80, 0x7C, 0x24, 0x0E, 0x00, 0x74, 0x26, 0x8B, 0x13};
    unsigned char a[sizeof(kCall)]{}, b[sizeof(kFound)]{};
    const bool callOk = Read(base + kCallRva, a, sizeof(a)) && std::memcmp(a, kCall, sizeof(a)) == 0;
    const bool foundOk = Read(base + kFoundRva, b, sizeof(b)) && std::memcmp(b, kFound, sizeof(b)) == 0;
    if (callOk && foundOk) {
        callHook = safetyhook::create_mid(reinterpret_cast<void*>(base + kCallRva), OnCall);
        foundHook = safetyhook::create_mid(reinterpret_cast<void*>(base + kFoundRva), OnFound);
    }
    if (!callHook || !foundHook) { callHook.reset(); foundHook.reset(); enabled = false; }
    std::fprintf(log, "sound_probe %s: call site %08X %s, found site %08X %s\n", enabled ? "installed" : "NOT installed", static_cast<unsigned>(base + kCallRva),
                 callOk ? "verified" : "MISMATCH", static_cast<unsigned>(base + kFoundRva), foundOk ? "verified" : "MISMATCH");
}

// Every 30 s from the observer loop, only when something moved; at most 24 lines.
void Summary(FILE* log) {
    static DWORD at = 0;
    static LONG seen = -1;
    const DWORD now = GetTickCount();
    const LONG asked = InterlockedCompareExchange(&totalCalls, 0, 0);
    if (!enabled || now - at < 30000 || asked == seen) return;
    at = now;
    seen = asked;
    std::fprintf(log, "sound_probe calls=%ld no_entity=%ld slot_overflow=%ld\n", asked, InterlockedCompareExchange(&noEntity, 0, 0),
                 InterlockedCompareExchange(&overflow, 0, 0));
    unsigned lines = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (auto& s : slots) {
            if (!s.vtable || (pass == 0) != (s.family[0] != 0) || lines >= 24) continue;
            if (pass == 1 && !s.calls) continue;
            ++lines;
            std::fprintf(log, "sound_probe family=%s vtable=%08X calls=%ld found=%ld misses=%ld empty_names=%ld last_found=%s miss_names=%s,%s,%s "
                              "instances mask=%08X/%08X/%08X occlusion=%.2f/%.2f/%.2f\n",
                         s.family[0] ? s.family : "?", static_cast<unsigned>(s.vtable), s.calls, s.found, s.misses, s.empty, s.lastFound[0] ? s.lastFound : "-",
                         s.missNames[0][0] ? s.missNames[0] : "-", s.missNames[1][0] ? s.missNames[1] : "-", s.missNames[2][0] ? s.missNames[2] : "-",
                         s.mask[0], s.mask[1], s.mask[2], s.occlusion[0], s.occlusion[1], s.occlusion[2]);
        }
}
}  // namespace sound_probe
