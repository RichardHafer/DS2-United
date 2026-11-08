// Dead Space 2 United by Hafer.
// Guard for damage processing with a dead damage record.
//
// Dump 17912 (2026-09-15 13:32): IDamageable::HandleEvents (0x00DD1B80)
// resolves the damage message's handle ([msg+8], table [0x0201C0F4], index
// handle >> 23) and, when the entry no longer carries that handle, passes null
// to IDamageable::ProcessDamage (0x00DC5260, thiscall(const DamageData&, bool)).
// That function skips the record only when bit 0 of this+4 is set; here it was
// clear and the read of [damage+0x60] faulted. So the damage record was freed
// before its message arrived (names from the Xbox build, 11.10.). The call is
// fenced: a null record returns without touching it, and the log counts it.
namespace listener_guard {
safetyhook::InlineHook hook;
bool installed = false;
volatile LONG fenced = 0;

int __fastcall OnListener(void* self, void*, void* damage, int reflected) {
    if (!damage) {
        InterlockedIncrement(&fenced);
        return 0;
    }
    return hook.thiscall<int>(self, damage, reflected);
}

bool Install(FILE* log) {
    // push esi / mov esi,ecx / test byte [esi+4],1 / jne +0xFC / push edi / mov edi,[esp+0xC]
    static const unsigned char kSite[] = {0x56,0x8B,0xF1,0xF6,0x46,0x04,0x01,0x0F,0x85,0xFC,0x00,0x00,0x00,0x57,0x8B,0x7C};
    // DS2Coop's world prop hits wrap the same function; whichever comes second chains.
    if (!hook_site::Install(log, "listener_guard", base + 0x9C5260, kSite, sizeof(kSite),
                            reinterpret_cast<void*>(OnListener), hook))
        return false;
    installed = true;
    std::fprintf(log, "listener_guard installed at %08X; a null damage record returns\n", static_cast<unsigned>(base + 0x9C5260));
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
