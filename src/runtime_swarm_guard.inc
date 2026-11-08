// Dead Space 2 United by Hafer.
// Guard for a swarm transition check that reads a missing target.
//
// Crash 2026-10-09 21:44 at 0x012DB473, campaign with three bots, right after
// five swarmers spawned. 0x012DB3F0 is a condition of the swarm state machine
// (built next to "SwarmEnemyRetreatST"; called from its CheckTransition slots
// 0x012EA410 and 0x012F9BA0). It compares the position of the swarmer's other
// target ([npc+0x8A0], null checked) with the one GetTarget returns (vtable
// +0x304, 0x00FFEEF0: [npc+0x6F8] - 0x10, or null) and reads [target+0x100]
// without a check. A swarmer without a target is a normal state: the caller
// at 0x012EA4E7 tests the same GetTarget result for null. Here a null target
// means "condition not met": jump to the function's own false exit
// (0x012DB5E6, mov al,bl with bl still 0).
namespace swarm_guard {
constexpr uint32_t kSiteRva = 0xEDB461, kFalseExitRva = 0xEDB5E6;
safetyhook::MidHook hook;
bool installed = false;
volatile LONG fenced = 0, lastNpc = 0;

void OnAfterGetTarget(safetyhook::Context& ctx) {
    if (ctx.eax) return;
    InterlockedIncrement(&fenced);
    InterlockedExchange(&lastNpc, static_cast<LONG>(ctx.ecx));
    ctx.eip = base + kFalseExitRva;
}

bool Install(FILE* log) {
    // movss xmm5,[esp+28h] / movss xmm4,[esp+24h] / movss xmm3,[esp+20h] / movaps xmm6,[eax+100h]
    static const unsigned char kSite[] = {0xF3, 0x0F, 0x10, 0x6C, 0x24, 0x28, 0xF3, 0x0F, 0x10, 0x64, 0x24, 0x24,
                                          0xF3, 0x0F, 0x10, 0x5C, 0x24, 0x20, 0x0F, 0x28, 0xB0, 0x00, 0x01, 0x00, 0x00};
    static const unsigned char kExit[] = {0x8A, 0xC3, 0x5B, 0x8B, 0xE5, 0x5D, 0xC3};
    unsigned char exitSeen[sizeof(kExit)]{};
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(base + kFalseExitRva), exitSeen, sizeof(exitSeen), &got) ||
        got != sizeof(exitSeen) || std::memcmp(exitSeen, kExit, sizeof(kExit)) != 0) {
        std::fprintf(log, "swarm_guard false exit %08X does not match; not installed\n", static_cast<unsigned>(base + kFalseExitRva));
        return false;
    }
    if (!hook_site::InstallMid(log, "swarm_guard", base + kSiteRva, kSite, sizeof(kSite), OnAfterGetTarget, hook))
        return false;
    installed = true;
    std::fprintf(log, "swarm_guard: a swarmer without a target fails the check at %08X instead of reading null+0x100\n",
                 static_cast<unsigned>(base + 0xEDB3F0));
    return true;
}

// Crash 2026-10-24 15:20 (dump 36148, and a friend's 4392, both survival): the client's copy of a
// host Swarmer is deleted; the delete message runs the Swarmer handler 0x012E9BD0, which asks the
// entity for its movement component (0x00EB8D40, type 0x25) and hands the answer unchecked to
// CharacterMovementComponent::SetCollisionMode 0x00F712A0 (thiscall(mode, force), ret 8). Our
// copies have no such component, so it read null+0x224. A null component now just returns.
constexpr uint32_t kSetCollisionModeRva = 0xB712A0;
safetyhook::InlineHook collisionHook;
bool collisionInstalled = false;
volatile LONG collisionNulls = 0;
volatile LONG collisionLastCaller = 0;

void __fastcall SetCollisionModeGuard(void* component, void*, int mode, int force) {
    if (!component) {
        InterlockedIncrement(&collisionNulls);
        InterlockedExchange(&collisionLastCaller, static_cast<LONG>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
        return;
    }
    collisionHook.thiscall<void>(component, mode, force);
}

bool InstallCollisionGuard(FILE* log) {
    // push esi / mov esi,[esp+8] / push edi / mov edi,ecx
    static const unsigned char kEntry[] = {0x56, 0x8B, 0x74, 0x24, 0x08, 0x57, 0x8B, 0xF9};
    if (!hook_site::Install(log, "swarm_guard collision mode", base + kSetCollisionModeRva, kEntry, sizeof(kEntry),
                            reinterpret_cast<void*>(SetCollisionModeGuard), collisionHook))
        return false;
    collisionInstalled = true;
    std::fprintf(log, "swarm_guard: SetCollisionMode %08X on a missing movement component is skipped (Swarmer copy delete)\n",
                 static_cast<unsigned>(base + kSetCollisionModeRva));
    return true;
}

void WriteDiagnostics(FILE* log) {
    static LONG lastNulls = 0;
    const LONG nulls = InterlockedCompareExchange(&collisionNulls, 0, 0);
    if (nulls != lastNulls) {
        std::fprintf(log, "swarm_guard collision_mode_nulls=%ld last_caller=%08X\n", nulls,
                     static_cast<unsigned>(InterlockedCompareExchange(&collisionLastCaller, 0, 0)));
        lastNulls = nulls;
    }
    static LONG last = 0;
    const LONG now = InterlockedCompareExchange(&fenced, 0, 0);
    if (now != last) {
        const uint32_t npc = static_cast<uint32_t>(InterlockedCompareExchange(&lastNpc, 0, 0));
        uint32_t vtable = 0;
        SIZE_T got = 0;
        if (npc) ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(npc), &vtable, 4, &got);
        std::fprintf(log, "swarm_guard fenced=%ld last_npc=%08X vtable=%08X\n", now, npc, vtable);
        last = now;
    }
}
}  // namespace swarm_guard
