// Dead Space 2 United by Hafer.
namespace save_guard {
safetyhook::InlineHook hook;
volatile LONG calls = 0, blocked = 0, passed = 0, unknown = 0, dropped = 0;
struct Event { uint32_t player; coop_save::Evidence evidence; bool bot; };
std::mutex mutex;
Event events[32]{};
unsigned count = 0;

void __fastcall OnStore(void* self, void*) {
    const auto player = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    const auto evidence = coop_save::Inspect(player,
        static_cast<uint32_t>(base + placement::kPlayerPrimaryVtableRva),
        [](uint32_t address, void* out, size_t size) { return Read(address, out, size); });
    InterlockedIncrement(&calls);
    const bool bot = IsBotEntity(player);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (count < std::size(events)) events[count++] = {player, evidence, bot};
        else InterlockedIncrement(&dropped);
    }
    if (evidence.role == coop_save::Role::Ghost || bot) {
        InterlockedIncrement(&blocked);
        return;
    }
    // Unknown objects retain native behavior; only verified ghosts are excluded.
    if (evidence.role == coop_save::Role::Unknown) InterlockedIncrement(&unknown);
    hook.thiscall<void>(self);
    InterlockedIncrement(&passed);
}

void Install(FILE* log, const std::string& ini) {
    const bool requested = GetPrivateProfileIntA("Coop", "ProtectLocalSave", 0, ini.c_str()) != 0 || botWanted;
    std::fprintf(log, "coop_save_guard config=%d unknown_policy=native\n", requested);
    if (!requested) return;
    const unsigned char expected[] = {
        0xA1,0xA4,0xC0,0x01,0x02,0x83,0xEC,0x10,0x57,0x8B,0xF9,
        0x8A,0x48,0x1C,0xD0,0xE9,0xF6,0xC1,0x01,0x0F,0x85,0xCA,0x02,0x00,0x00};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + 0x47C7F0, actual, sizeof(actual)) ||
        std::memcmp(actual, expected, sizeof(actual))) {
        std::fprintf(log, "coop_save_guard signature_mismatch; disabled\n");
        return;
    }
    hook = safetyhook::create_inline(reinterpret_cast<void*>(base + 0x47C7F0),
                                   reinterpret_cast<void*>(OnStore));
    std::fprintf(log, "coop_save_guard signature_ok hook=%d va=%08X\n",
                 static_cast<bool>(hook), static_cast<unsigned>(base + 0x47C7F0));
}

void Flush(FILE* log) {
    if (!hook) return;
    Event pending[32]{};
    unsigned size = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        size = count;
        std::copy_n(events, size, pending);
        count = 0;
    }
    for (unsigned i = 0; i < size; ++i) {
        const auto& e = pending[i];
        std::fprintf(log, "coop_save_guard player=%08X vt=%08X proxy=%08X flags=%08X role=%d action=%s\n",
            e.player, e.evidence.vtable, e.evidence.proxy, e.evidence.flags,
            static_cast<int>(e.evidence.role),
            e.evidence.role == coop_save::Role::Ghost ? "blocked" : e.bot ? "blocked_bot" : "native");
    }
    static LONG last = 0;
    const LONG total = InterlockedCompareExchange(&calls, 0, 0);
    if (total == last) return;
    last = total;
    std::fprintf(log, "coop_save_guard calls=%ld blocked=%ld returned=%ld unknown=%ld log_dropped=%ld\n",
                 total, blocked, passed, unknown, dropped);
}
}  // namespace save_guard
