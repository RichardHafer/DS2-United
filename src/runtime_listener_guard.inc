// Guard for a message listener that dereferences a null entity.
//
// Dump 17912 (2026-09-15 13:32): the engine's per-frame broadcast (static
// message at 0x0204D4A0, sent from 0x007701C0) reached a subscriber whose
// handler 0x00DD1B80 takes the message for another kind, falls into its
// "no entity" branch (0x00DD1BB3: xor edx,edx / push edx / push edx) and
// calls 0x00DC5260(this, entity = null, 0). That function skips the entity
// only when bit 0 of this+4 is set; here it was clear and the read of
// [entity+0x60] faulted. A subscriber that still receives frame ticks in a
// state where it has nothing to listen for is a listener that outlived its
// object; which one is not known yet. Until it is, the call is fenced: a
// null entity returns 0 without touching it, and the log counts the fences.
namespace listener_guard {
safetyhook::InlineHook hook;
bool installed = false;
volatile LONG fenced = 0;

int __fastcall OnListener(void* self, void*, void* entity, int flag) {
    if (!entity) {
        InterlockedIncrement(&fenced);
        return 0;
    }
    return hook.thiscall<int>(self, entity, flag);
}

bool Install(FILE* log) {
    // push esi / mov esi,ecx / test byte [esi+4],1 / jne +0xFC / push edi / mov edi,[esp+0xC]
    static const unsigned char kSite[] = {0x56,0x8B,0xF1,0xF6,0x46,0x04,0x01,0x0F,0x85,0xFC,0x00,0x00,0x00,0x57,0x8B,0x7C};
    unsigned char actual[sizeof(kSite)]{};
    if (!Read(base + 0x9C5260, actual, sizeof(actual)) || std::memcmp(actual, kSite, sizeof(kSite)) != 0) {
        std::fprintf(log, "listener_guard signature_mismatch at %08X; not installed\n", static_cast<unsigned>(base + 0x9C5260));
        return false;
    }
    hook = safetyhook::create_inline(reinterpret_cast<void*>(base + 0x9C5260), reinterpret_cast<void*>(OnListener));
    if (!hook) {
        std::fprintf(log, "listener_guard hook_failed at %08X\n", static_cast<unsigned>(base + 0x9C5260));
        return false;
    }
    installed = true;
    std::fprintf(log, "listener_guard installed at %08X; null entity returns 0\n", static_cast<unsigned>(base + 0x9C5260));
    return true;
}

void WriteDiagnostics(FILE* log) {
    static LONG last = 0;
    const LONG now = InterlockedCompareExchange(&fenced, 0, 0);
    if (now != last) {
        std::fprintf(log, "listener_guard fenced=%ld\n", now);
        last = now;
    }
}
}  // namespace listener_guard
