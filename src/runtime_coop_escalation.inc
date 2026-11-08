// Dead Space 2 United by Hafer.
// The survival escalation on the co-op side ([Escalation] in DS2Survival.ini,
// computed by the randomizer's director for the current wave): the companion
// bots' hits shrink, shop prices grow, and the Ubermorph and the Brute shrug off
// part of every stasis. The wave is survmusic's: the host's own, or what the
// host sent a client.
namespace arena { bool RunningNow(); }   // runtime_coop_arena.inc
namespace escalation {
using EscalationFn = int(__cdecl*)(int, int*, int);
EscalationFn escalationFn = nullptr;
DWORD lookedAt = 0, polledAt = 0;
volatile LONG wave = 0, level = 0, botDamagePercent = 100, pricePercent = 100, bossResistPercent = 0, enhancedPercent = 0;
volatile LONG botHitsScaled = 0, bossStasisCut = 0, changes = 0;
constexpr uint32_t kHunterVtableRva = 0x176EAD8, kBruteVtableRva = 0x176D428;

void Tick(DWORD now) {
    if (now - polledAt < 500) return;
    polledAt = now;
    if (!escalationFn && now - lookedAt > 5000) {
        lookedAt = now;
        if (const HMODULE observer = ds2coop::PartnerModule("DS2RandomizerObserver.asi"))
            escalationFn = reinterpret_cast<EscalationFn>(GetProcAddress(observer, "DS2Survival_EscalationFor"));
    }
    const LONG w = mpmap::visit.phase == coop_mpmap::Phase::Arrived || arena::RunningNow() ? InterlockedCompareExchange(&survmusic::wave, 0, 0) : 0;
    int out[5] = {0, 100, 100, 0, 0};
    if (escalationFn && w > 0) escalationFn(static_cast<int>(w), out, 5);
    // both stored every poll: a short-circuit left the wave a poll behind a level change
    const bool levelChanged = InterlockedExchange(&level, out[0]) != out[0];
    const bool waveChanged = InterlockedExchange(&wave, w) != w;
    if (levelChanged || waveChanged) {
        if (InterlockedIncrement(&changes) <= 60 && out[0])
            gate::Event("escalation wave %ld level %d: bot damage %d%% prices %d%% boss stasis resist %d%% enhanced %d%%", w, out[0], out[1], out[2], out[3], out[4]);
    }
    InterlockedExchange(&botDamagePercent, out[1]);
    InterlockedExchange(&pricePercent, out[2]);
    InterlockedExchange(&bossResistPercent, out[3]);
    InterlockedExchange(&enhancedPercent, out[4]);
}

// From the health hooks' outer call: below 1 for a bot's hit on anything that is not a Player.
float BotHitFactor(const void* damage, uint32_t component) {
    const LONG percent = InterlockedCompareExchange(&botDamagePercent, 0, 0);
    if (percent >= 100 || !damage || !bot::BotBehind(damage)) return 1.0f;
    uint32_t owner = 0, vtable = 0;
    if (!Read(component + hits::kComponentOwner, &owner, 4) || !owner || !Read(owner, &vtable, 4) || vtable == base + placement::kPlayerPrimaryVtableRva) return 1.0f;
    InterlockedIncrement(&botHitsScaled);
    return static_cast<float>(percent < 0 ? 0 : percent) / 100.0f;
}
// From the stasis hook: what is left of a stasis on this entity.
float StasisFactor(uint32_t entity) {
    const LONG resist = InterlockedCompareExchange(&bossResistPercent, 0, 0);
    uint32_t vtable = 0;
    if (resist <= 0 || !entity || !Read(entity, &vtable, 4) || (vtable != base + kHunterVtableRva && vtable != base + kBruteVtableRva)) return 1.0f;
    if (InterlockedIncrement(&bossStasisCut) <= 30) gate::Event("escalation: stasis on the boss %08X cut by %ld%%", entity, resist);
    return 1.0f - static_cast<float>(resist > 100 ? 100 : resist) / 100.0f;
}
unsigned PricePercent() {
    const LONG percent = InterlockedCompareExchange(&pricePercent, 0, 0);
    return percent > 0 ? static_cast<unsigned>(percent) : 100u;
}

// The bot hit scaling needs the health hooks and the boss resistance the stasis
// hook; both come with other modules, and are laid here when those are off.
void Install(FILE* log) {
    const bool health = hits::InstallHealthHooks(log, "the bots' damage escalation is off");
    bool stasis = static_cast<bool>(bot::stasisEntityHook);
    if (!stasis) {
        static const unsigned char kStasisEntity[] = {0x53, 0x8B, 0xD9, 0xF3, 0x0F, 0x10, 0x43, 0x58, 0x0F, 0x2F, 0x44, 0x24, 0x0C, 0x56, 0x76, 0x06};
        stasis = hook_site::Install(log, "escalation StasisManager::StasisEntityInternal", base + bot::kStasisInternalRva, kStasisEntity, sizeof(kStasisEntity),
                                    reinterpret_cast<void*>(bot::OnStasisEntity), bot::stasisEntityHook);
    }
    std::fprintf(log, "escalation health_hooks=%d stasis_hook=%d (factors from DS2Survival_EscalationFor, wave from survmusic)\n", health ? 1 : 0, stasis ? 1 : 0);
}
void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG sum = level * 7 + botHitsScaled + bossStasisCut * 3;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "escalation wave=%ld level=%ld bot_damage=%ld%% prices=%ld%% boss_stasis_resist=%ld%% enhanced=%ld%% bot_hits_scaled=%ld boss_stasis_cut=%ld fn=%d\n",
                 InterlockedCompareExchange(&wave, 0, 0), InterlockedCompareExchange(&level, 0, 0), InterlockedCompareExchange(&botDamagePercent, 0, 0),
                 InterlockedCompareExchange(&pricePercent, 0, 0), InterlockedCompareExchange(&bossResistPercent, 0, 0),
                 InterlockedCompareExchange(&enhancedPercent, 0, 0), InterlockedCompareExchange(&botHitsScaled, 0, 0),
                 InterlockedCompareExchange(&bossStasisCut, 0, 0), escalationFn ? 1 : 0);
}
}  // namespace escalation
