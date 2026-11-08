// Dead Space 2 United by Hafer.
// Global record lookup by GUID, redirected to the pool twin on a miss.
//
// Pool copies keep the retail records' first two GUID words and swap the
// last two for the pool tag (4D444E52-4C4F4F50). Typed reference fields in a
// copied group are rewritten to the twins when the pool is built, but a class
// default is not in the group at all: the Divider named its parts that way
// (see runtime_divider_probe.inc), and the Guardian names its tentacles and
// its dismemberable prop the same way. Dump 31068 (2026-09-15 18:51): a pool
// Guardian placed by an extra-spawn rule walked its five pod slots
// (0x012D9C30), asked the global lookup 0x00776310 for the retail tentacle
// 36C89034-C3F378D4-4E59414D-30445241, got null because the retail record
// lives only in the transport hub archive, and wrote through it at +0x234.
//
// 0x00776310 is a plain hash chain: hash = ((w0 * 33 + w1) * 33 + w2) * 33
// + w3, bucket at manager+0x70, record hash at +0x1C, next at +0x34;
// thiscall(manager, const Guid*, flags) with ret 8. The wrapper calls it, and
// on a miss for a GUID without the pool tag asks once more for the twin. A
// hit is returned as if the retail record had been there. Only misses pay.
namespace guid_redirect {
safetyhook::InlineHook hook;
bool installed = false;
volatile LONG redirected = 0, missed = 0;
constexpr unsigned kPoolTag0 = 0x4D444E52u, kPoolTag1 = 0x4C4F4F50u;

void* __fastcall OnFind(void* manager, void*, const unsigned* guid, unsigned flags) {
    void* record = hook.thiscall<void*>(manager, guid, flags);
    if (record || !guid) return record;
    unsigned words[4]{};
    if (!Read(reinterpret_cast<uintptr_t>(guid), words, sizeof(words))) return nullptr;
    if (words[2] == kPoolTag0 && words[3] == kPoolTag1) return nullptr;
    const unsigned twin[4] = {words[0], words[1], kPoolTag0, kPoolTag1};
    record = hook.thiscall<void*>(manager, twin, flags);
    InterlockedIncrement(record ? &redirected : &missed);
    return record;
}

bool Install(FILE* log) {
    // push esi / push edi / mov edi,[esp+0xC] / mov edx,[edi] / mov eax,edx / shl eax,5 / add eax,edx
    static const unsigned char kSite[] = {0x56,0x57,0x8B,0x7C,0x24,0x0C,0x8B,0x17,0x8B,0xC2,0xC1,0xE0,0x05,0x03,0xC2,0x03};
    unsigned char actual[sizeof(kSite)]{};
    if (!Read(base + 0x376310, actual, sizeof(actual)) || std::memcmp(actual, kSite, sizeof(kSite)) != 0) {
        std::fprintf(log, "guid_redirect signature_mismatch at %08X; not installed\n", static_cast<unsigned>(base + 0x376310));
        return false;
    }
    hook = safetyhook::create_inline(reinterpret_cast<void*>(base + 0x376310), reinterpret_cast<void*>(OnFind));
    if (!hook) {
        std::fprintf(log, "guid_redirect hook_failed at %08X\n", static_cast<unsigned>(base + 0x376310));
        return false;
    }
    installed = true;
    std::fprintf(log, "guid_redirect installed at %08X; misses retry with the pool twin\n", static_cast<unsigned>(base + 0x376310));
    return true;
}

void WriteDiagnostics(FILE* log) {
    static LONG lastRedirected = 0, lastMissed = 0;
    const LONG hits = InterlockedCompareExchange(&redirected, 0, 0);
    const LONG misses = InterlockedCompareExchange(&missed, 0, 0);
    if (hits != lastRedirected || misses != lastMissed) {
        std::fprintf(log, "guid_redirect redirected=%ld twin_missing=%ld\n", hits, misses);
        lastRedirected = hits;
        lastMissed = misses;
    }
}
}  // namespace guid_redirect
