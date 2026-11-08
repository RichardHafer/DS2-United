// Dead Space 2 United by Richard Hafer.
// The Outbreak maps' sounds (DS2DAT12: 154 .snu streams, 294 bank sounds) are
// EA codec 2, 16 bit big endian PCM ("P6B0"). The PC build carries that decoder
// (descriptor 0x01C6D57C, byte swap in 0x00562CF0) but registers only L32S,
// L32P and Xas1 (0x00735C02..1C) and P6L0 in a second player (0x013C6302). The
// bank loader's lookup 0x0052B0F0 then returns null and 0x0053BB10 reads through
// it (dump 44640, edx = 'P6B0'). On a miss for P6B0 the lookup registers a
// private copy of the descriptor with the native Add 0x0052B120 (head insert)
// and returns it. One copy per registry: the list node is part of the descriptor.
//
// Descriptor, 7 dwords: size query, init, (unused), decode, next node, key, 0.
namespace pcmcodec {
constexpr uint32_t kFindRva = 0x12B0F0, kAddRva = 0x12B120, kDescriptorRva = 0x186D57C;
constexpr uint32_t kKey = 0x50364230;   // 'P6B0'
const unsigned char kFindBytes[] = {0x8B, 0x09, 0x85, 0xC9, 0x74, 0x18, 0x8B, 0x54, 0x24, 0x04};
const unsigned char kAddBytes[] = {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x8B, 0x31, 0x8B, 0xD6};
constexpr unsigned kMaxRegistries = 8;

bool enabled = true, installed = false;
safetyhook::InlineHook findHook;
SRWLOCK lock = SRWLOCK_INIT;
uint32_t descriptor[7] = {};
struct Copy { uint32_t registry; uint32_t words[7]; };
Copy copies[kMaxRegistries] = {};
unsigned copyCount = 0;
volatile LONG lookups = 0, misses = 0, registered = 0, reused = 0, full = 0, faults = 0, otherMisses = 0;
volatile LONG missLines = 0;

const char* Tag(uint32_t key, char* out) {
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>(key >> (24 - 8 * i));
        out[i] = c >= 32 && c < 127 ? c : '?';
    }
    out[4] = 0;
    return out;
}

__declspec(noinline) uint32_t AddRaw(uint32_t registry, uint32_t* entry) {
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t*)>(base + kAddRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(registry)), entry);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return 0;
    }
}

uint32_t __fastcall OnFind(void* registry, void*, uint32_t key) {
    InterlockedIncrement(&lookups);
    const uint32_t found = findHook.thiscall<uint32_t>(registry, key);
    if (found || !registry) return found;
    if (key != kKey) {
        InterlockedIncrement(&otherMisses);
        char tag[5];
        if (InterlockedIncrement(&missLines) <= 8) mpmap::Event("pcmcodec: registry %08X has no '%s' (%08X), not ours to add", static_cast<unsigned>(reinterpret_cast<uintptr_t>(registry)), Tag(key, tag), key);
        return found;
    }
    InterlockedIncrement(&misses);
    const uint32_t reg = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(registry));
    AcquireSRWLockExclusive(&lock);
    uint32_t result = 0;
    for (unsigned i = 0; i < copyCount && !result; ++i)
        if (copies[i].registry == reg) result = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(copies[i].words));
    bool fresh = false;
    if (result) {
        InterlockedIncrement(&reused);   // registered by another thread a moment ago
    } else if (copyCount < kMaxRegistries) {
        Copy& c = copies[copyCount];
        std::memcpy(c.words, descriptor, sizeof(c.words));
        c.words[4] = 0;
        c.registry = reg;
        result = AddRaw(reg, c.words);
        if (result) { ++copyCount; fresh = true; InterlockedIncrement(&registered); }
    } else {
        InterlockedIncrement(&full);
    }
    ReleaseSRWLockExclusive(&lock);
    if (fresh) mpmap::Event("pcmcodec: registered the P6B0 decoder (16 bit big endian PCM) in registry %08X as %08X", reg, result);
    else if (!result && InterlockedIncrement(&missLines) <= 8) mpmap::Event("pcmcodec: P6B0 missing in registry %08X and NOT registered (faults %ld, full %ld)", reg, faults, full);
    return result;
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("MpMap", "RegisterPcmCodec", 1, ini.c_str()) != 0;
    if (!enabled) { std::fprintf(log, "pcmcodec off ([MpMap] RegisterPcmCodec=0): Outbreak map sounds crash the bank loader\n"); return; }
    unsigned char find[sizeof(kFindBytes)]{}, add[sizeof(kAddBytes)]{};
    const bool findOk = Read(base + kFindRva, find, sizeof(find)) && std::memcmp(find, kFindBytes, sizeof(find)) == 0;
    const bool addOk = Read(base + kAddRva, add, sizeof(add)) && std::memcmp(add, kAddBytes, sizeof(add)) == 0;
    const bool descOk = Read(base + kDescriptorRva, descriptor, sizeof(descriptor)) && descriptor[5] == kKey && descriptor[0] == base + 0x1611B0 &&
                        descriptor[1] == base + 0x1611E0 && descriptor[3] == base + 0x162F80;
    std::fprintf(log, "pcmcodec sig lookup %08X %s, add %08X %s, descriptor %08X %s (%08X %08X %08X %08X next %08X key %08X)\n",
                 static_cast<unsigned>(base + kFindRva), findOk ? "ok" : "MISMATCH", static_cast<unsigned>(base + kAddRva), addOk ? "ok" : "MISMATCH",
                 static_cast<unsigned>(base + kDescriptorRva), descOk ? "ok" : "MISMATCH", descriptor[0], descriptor[1], descriptor[2], descriptor[3], descriptor[4],
                 descriptor[5]);
    if (!findOk || !addOk || !descOk) { std::fprintf(log, "pcmcodec NOT installed: Outbreak map sounds will crash the bank loader\n"); return; }
    findHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kFindRva), reinterpret_cast<void*>(OnFind));
    installed = static_cast<bool>(findHook);
    std::fprintf(log, "pcmcodec %s: a lookup that misses P6B0 registers the built-in decoder\n", installed ? "installed" : "hook FAILED");
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    std::fprintf(log, "pcmcodec installed=%d lookups=%ld p6b0_misses=%ld registered=%ld reused=%ld registries=%u full=%ld faults=%ld other_misses=%ld\n", installed ? 1 : 0,
                 InterlockedCompareExchange(&lookups, 0, 0), InterlockedCompareExchange(&misses, 0, 0), InterlockedCompareExchange(&registered, 0, 0),
                 InterlockedCompareExchange(&reused, 0, 0), copyCount, InterlockedCompareExchange(&full, 0, 0), InterlockedCompareExchange(&faults, 0, 0),
                 InterlockedCompareExchange(&otherMisses, 0, 0));
}
}  // namespace pcmcodec
