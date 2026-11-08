// Dead Space 2 United by Richard Hafer.
// Gabe Weller's patrol helmet (18.10., user: the only difference of Weller's suits is the helmet of
// his patrol suit). The retail Patrol Suit (Uniform 31CB7561-1B516549) names its helmet template
// 31CB7561-7F92A9B3 (CMD_SetUniformHelmetGUID, playersecurityhelmetweller with the shared security
// helmet textures). The Severed suit package uni_pdlcsecurityweller, shipped as the loose stream
// randomizer\ds2_severed_patrol, holds Gabe's own helmet template 31CB97F2-4601A667
// (pdlc_playersecurityhelmetweller, its own helmet rig and motions bank; the assets it shares with
// the Patrol package are byte-identical). Player::AttachHelmet (0x0087D210, thiscall(Player*,
// const Guid*), ret 4) spawns the helmet through SimManager::GetSpawnInfo; it is given Gabe's
// template instead when that template is registered. [Coop] PatrolWellerHelmet=1.
namespace patrolhelmet {
constexpr uint32_t kAttachHelmetRva = 0x47D210, kGlobalFindRva = 0x376310, kSimManagerRva = 0x1C1C0A0;
constexpr uint32_t kPatrolHelmet[4] = {0x31CB7561u, 0x7F92A9B3u, 0x534F5254u, 0x30314E49u};
alignas(16) uint32_t wellerHelmet[4] = {0x31CB97F2u, 0x4601A667u, 0x534F5254u, 0x30314E49u};
safetyhook::InlineHook hook;
bool enabled = true, installed = false;
volatile LONG calls = 0, patrolCalls = 0, swapped = 0, notRegistered = 0, faults = 0;

__declspec(noinline) bool Registered(const uint32_t* guid) {
    uint32_t manager = 0;
    if (!Read(base + kSimManagerRva, &manager, 4) || !manager) return false;
    __try {
        using Find = void*(__thiscall*)(uint32_t, const uint32_t*, unsigned);
        return reinterpret_cast<Find>(base + kGlobalFindRva)(manager, guid, 0) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}
void __fastcall OnAttachHelmet(void* player, void*, const uint32_t* guid) {
    InterlockedIncrement(&calls);
    uint32_t words[4]{};
    if (enabled && guid && Read(reinterpret_cast<uint32_t>(guid), words, 16) && std::memcmp(words, kPatrolHelmet, 16) == 0) {
        InterlockedIncrement(&patrolCalls);
        if (Registered(wellerHelmet)) {
            if (InterlockedIncrement(&swapped) <= 5)
                gate::Event("patrol helmet: player %08X gets Gabe Weller's helmet (31CB97F2-4601A667 for 31CB7561-7F92A9B3)", reinterpret_cast<uint32_t>(player));
            hook.thiscall<void>(player, static_cast<const uint32_t*>(wellerHelmet));
            return;
        }
        if (InterlockedIncrement(&notRegistered) <= 3)
            gate::Event("patrol helmet: Gabe's helmet template is not registered (randomizer\\ds2_severed_patrol not loaded); Isaac's helmet stays");
    }
    hook.thiscall<void>(player, guid);
}
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "PatrolWellerHelmet", 1, ini.c_str()) != 0;
    if (!enabled) {
        std::fprintf(log, "patrol helmet off ([Coop] PatrolWellerHelmet=0)\n");
        return;
    }
    // push ebp / mov ebp,esp / and esp,-16 / sub esp,0x54 / push ebx / push esi / mov esi,ecx
    static const unsigned char kBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x54, 0x53, 0x56, 0x8B, 0xF1};
    installed = hook_site::Install(log, "patrol helmet Player::AttachHelmet", base + kAttachHelmetRva, kBytes, sizeof(kBytes),
                                   reinterpret_cast<void*>(OnAttachHelmet), hook);
    std::fprintf(log, "patrol helmet %s: the Patrol Suit gets Gabe Weller's helmet when randomizer\\ds2_severed_patrol is loaded\n", installed ? "on" : "NOT installed");
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG sum = calls + patrolCalls * 3 + swapped * 7 + notRegistered * 11 + faults * 13;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "patrol helmet attach_calls=%ld patrol=%ld weller_helmet=%ld not_registered=%ld faults=%ld\n", calls, patrolCalls, swapped, notRegistered, faults);
}
}  // namespace patrolhelmet
