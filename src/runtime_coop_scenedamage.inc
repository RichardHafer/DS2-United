// Dead Space 2 United by Richard Hafer.
// Enemies do not hurt our Isaac while a scene has him ([Coop] SceneDamageGuard).
//
// 27.11., reported: hanging from the train after the Brute's arm and in the church while the
// big tripod holds Isaac, enemies came and killed him in the middle of the scene; more enemies
// (randomizer copies, extra spawns) reach places the level never sent anybody during it.
// HealthComponent::HandleDamage (0x00F74D60) drops a hit on the local Isaac while scenekind
// says cinematic, short sequence, QTE or (SceneDamageGuardStationary) stationary aiming, when
// the attacker (DamageData +0x80, a SafePtr; a projectile's owner at +0x240) is a necromorph
// that is not part of the scene: one running a scripted sequence (+0x238/+0x240) or holding him
// in a paired attack (+0x2A8) still hits, so a failed QTE kills as it should.
namespace scenedamage {
constexpr uint32_t kHandleDamageRva = 0xB74D60, kAttacker = 0x80, kOwnerRef = 0x240, kSequence = 0x238, kSequenceFlags = 0x240, kPaired = 0x2A8;
bool enabled = true, stationary = true, installed = false;
safetyhook::InlineHook hook;
volatile LONG refused = 0, passedScene = 0, lines = 0;
uint32_t Necromorph(uint32_t ref) {
    for (const uint32_t who : {ref, ref - 0x10}) {
        uint32_t vtable = 0, owner = 0;
        if (who < 0x10000 || !Read(who, &vtable, 4)) continue;
        if (bot_sense::KindOf(vtable) != coop_bot::EnemyKind::Unknown) return who;
        if (Read(who + kOwnerRef, &owner, 4) && owner > 0x10010 && Read(owner - 0x10, &vtable, 4) && bot_sense::KindOf(vtable) != coop_bot::EnemyKind::Unknown)
            return owner - 0x10;
    }
    return 0;
}
bool PartOfScene(uint32_t npc) {
    uint32_t sequence = 0, coordinator = 0;
    uint8_t flags = 0;
    return (Read(npc + kSequence, &sequence, 4) && sequence && Read(npc + kSequenceFlags, &flags, 1) && (flags & 4)) ||
           (Read(npc + kPaired, &coordinator, 4) && coordinator);
}
// Only while scenekind read this very Isaac, a script has him (not the hacking panel or a walk and
// talk, where the game sends enemies on purpose), and for at most kMaxMs of one scene state.
constexpr DWORD kMaxMs = 180000;
volatile LONG overLong = 0;
bool Guarded(uint32_t kind, uint32_t isaac) {
    if (!isaac || scenekind::localPlayer != isaac || !scenekind::localScripted) return false;
    if (scenekind::localSince && GetTickCount() - scenekind::localSince > kMaxMs) {
        if (InterlockedIncrement(&overLong) == 1)
            gate::Event("scene damage: the scene state has lasted over %lu s, hits count again", static_cast<unsigned long>(kMaxMs / 1000));
        return false;
    }
    return kind == scenekind::kCinematic || kind == scenekind::kShort || kind == scenekind::kQte || (stationary && kind == scenekind::kStationary);
}
void __fastcall OnHandleDamage(void* component, void*, const void* damage) {
    const uint32_t kind = scenekind::local;
    const uint32_t isaac = enabled && damage ? LocalCampaignPlayer() : 0;
    if (Guarded(kind, isaac)) {
        uint32_t owner = 0, ref = 0;
        if (Read(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(component)) + hits::kComponentOwner, &owner, 4) && owner == isaac &&
            Read(reinterpret_cast<uintptr_t>(damage) + kAttacker, &ref, 4)) {
            if (const uint32_t npc = Necromorph(ref)) {
                if (PartOfScene(npc)) {
                    InterlockedIncrement(&passedScene);
                } else {
                    InterlockedIncrement(&refused);
                    if (InterlockedIncrement(&lines) <= 40) {
                        float amount = 0.0f;
                        uint32_t vtable = 0;
                        Read(reinterpret_cast<uintptr_t>(damage) + 0x50, &amount, 4);
                        Read(npc, &vtable, 4);
                        gate::Event("scene damage: a hit of %.1f on our Isaac by %s %08X dropped (%s)", amount, marks::KindText(bot_sense::KindOf(vtable)), npc,
                                    scenekind::kNames[kind < scenekind::kKinds ? kind : 0]);
                    }
                    return;
                }
            }
        }
    }
    hook.thiscall<void>(component, damage);
}
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "SceneDamageGuard", 1, ini.c_str()) != 0;
    stationary = GetPrivateProfileIntA("Coop", "SceneDamageGuardStationary", 1, ini.c_str()) != 0;
    if (!enabled) { std::fprintf(log, "scene damage guard off ([Coop] SceneDamageGuard=0)\n"); return; }
    // push ebp / mov ebp,esp / and esp,-16 / xorps xmm2,xmm2 / sub esp,74h / push ebx / push esi / mov esi,ecx
    static const unsigned char kSig[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x0F, 0x57, 0xD2, 0x83, 0xEC, 0x74, 0x53, 0x56, 0x8B, 0xF1};
    installed = hook_site::Install(log, "scene damage HealthComponent::HandleDamage", base + kHandleDamageRva, kSig, sizeof(kSig), reinterpret_cast<void*>(OnHandleDamage), hook);
    std::fprintf(log, "scene damage guard %s: enemies outside the scene do not hurt our Isaac in cinematics, short sequences, QTEs%s\n", installed ? "on" : "NOT installed",
                 stationary ? " and stationary aiming" : "");
}
void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    if (!installed || refused + passedScene * 3 + overLong * 7 == last) return;
    last = refused + passedScene * 3 + overLong * 7;
    std::fprintf(log, "scene damage refused=%ld scene_actors_passed=%ld over_long=%ld\n", refused, passedScene, overLong);
}
}  // namespace scenedamage
