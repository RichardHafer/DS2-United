// Dead Space 2 United by Richard Hafer.
// The registry that holds dead aiming data.
//
// Four dumps from 22.09., 17:20 to 17:28, both instances crashing at the same
// moment, two addresses, one cause. BoneAimingData objects register themselves
// in a global array held by the singleton at [0x02021358]: pointer array at
// +4, count at +8. Their destructor (0x00EC8D90) unregisters properly: it calls
// 0x00EA3D50, which walks the array comparing *(element->[0xC]) against its own
// key, then shifts the tail down and decrements the count. Nothing wrong with
// that.
//
// What goes wrong is upstream. In dump 44228 the array held nineteen entries
// and six consecutive ones (a single 0x240 block, one character's worth) had
// been freed without ever running that destructor, and the memory already
// belonged to a Lua audio script: reading through them found the text
// "\_common\aud_dynamic_ambience.lua" where an object should have been. The
// next legitimate destructor then walked the array, dereferenced one of those
// six, and died at 0x00EA3D6D. The sibling crash at 0x00737863 is the same
// region coming apart in the allocator's own free list.
//
// So the entries are the symptom of a wholesale release somewhere else, and
// until that is found this keeps the array honest. Before every unregister,
// and once a second besides, entries that cannot survive the scan are dropped:
// an entry whose vtable is outside the image, or whose [0xC] or the word behind
// it cannot be read, is exactly what the loop would crash on. Nothing that
// could still be walked is touched, so this removes only what was already lost.
namespace aimguard {
constexpr uint32_t kUnregisterRva = 0xAA3D50;      // Unregister(registry, entry), thiscall, ret 4
constexpr uint32_t kRegistrySingleton = 0x1C21358; // [0x02021358]
constexpr uint32_t kEntryOwner = 0xC;

bool enabled = true, installed = false;
safetyhook::InlineHook unregisterHook;
uint32_t imageLimit = 0;
volatile LONG purged = 0, passes = 0, faults = 0;
uint32_t lastTick = 0, lastCount = 0;

bool Survivable(uint32_t entry) {
    uint32_t vtable = 0, owner = 0, key = 0;
    return entry >= 0x10000 && Read(entry, &vtable, 4) && vtable >= base + 0x1000 && vtable < imageLimit &&
           Read(entry + kEntryOwner, &owner, 4) && owner >= 0x10000 && Read(owner, &key, 4);
}

void Purge(uint32_t registry) {
    uint32_t array = 0, count = 0;
    if (!registry || !Read(registry + 4, &array, 4) || !array ||
        !Read(registry + 8, &count, 4) || !count || count > 4096) return;
    lastCount = count;
    uint32_t keep = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry = 0;
        if (!Read(array + 4 * i, &entry, 4) || !Survivable(entry)) { InterlockedIncrement(&purged); continue; }
        if (keep != i && !WriteBytes(array + 4 * keep, &entry, 4)) return;
        ++keep;
    }
    if (keep != count && WriteBytes(registry + 8, &keep, 4))
        gate::Event("coop_aimguard registry %08X: %u of %u aiming entries were already freed and were dropped", registry, count - keep, count);
    InterlockedIncrement(&passes);
}

uint32_t Registry() {
    uint32_t registry = 0;
    return Read(base + kRegistrySingleton, &registry, 4) ? registry : 0;
}

uint32_t __fastcall OnUnregister(uint32_t registry, void*, uint32_t entry) {
    if (enabled) Purge(registry);
    return unregisterHook.thiscall<uint32_t>(reinterpret_cast<void*>(registry), entry);
}

void Tick(uint32_t now) {
    if (!enabled || !installed) return;
    if (now - lastTick < 1000) return;
    lastTick = now;
    __try { Purge(Registry()); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&faults); }
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "AimRegistryGuard", 1, ini.c_str()) != 0;
    if (!enabled) { std::fprintf(log, "coop_aimguard off by ini\n"); return true; }
    // mov eax,[esp+4] / mov edx,[eax+0xC] / push edi / mov edi,[edx] / mov edx,[ecx+8]
    static const unsigned char expected[] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x50, 0x0C, 0x57, 0x8B, 0x3A, 0x8B, 0x51, 0x08};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kUnregisterRva, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) {
        std::fprintf(log, "coop_aimguard signature mismatch at %08X; off\n", static_cast<unsigned>(base + kUnregisterRva));
        enabled = false;
        return false;
    }
    uint32_t peOffset = 0, sizeOfImage = 0;
    if (!Read(base + 0x3C, &peOffset, 4) || !Read(base + peOffset + 0x50, &sizeOfImage, 4) || !sizeOfImage) sizeOfImage = 0x3384000;
    imageLimit = static_cast<uint32_t>(base) + sizeOfImage;
    unregisterHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kUnregisterRva), reinterpret_cast<void*>(OnUnregister));
    installed = static_cast<bool>(unregisterHook);
    std::fprintf(log, "coop_aimguard %s at %08X (registry [%08X], image ends %08X)\n",
                 installed ? "installed" : "FAILED", static_cast<unsigned>(base + kUnregisterRva),
                 static_cast<unsigned>(base + kRegistrySingleton), imageLimit);
    return installed;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&purged, 0, 0) * 1000 + InterlockedCompareExchange(&faults, 0, 0);
    if (now == last) return;
    last = now;
    std::fprintf(log, "coop_aimguard purged=%ld passes=%ld last_count=%u faults=%ld\n",
                 InterlockedCompareExchange(&purged, 0, 0), InterlockedCompareExchange(&passes, 0, 0),
                 lastCount, InterlockedCompareExchange(&faults, 0, 0));
}
}  // namespace aimguard
