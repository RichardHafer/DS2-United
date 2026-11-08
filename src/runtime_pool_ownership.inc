// Dead Space 2 United by Richard Hafer.
// Every entity built from a pool packet belongs to the level, not to the pool.
//
// PoolActorsFollowLevel rewrites the stream handle of the packets this plugin
// swaps in at the spawner. It never saw the packets a swapped enemy builds on
// its own. A Pregnant from the pool releases swarm units out of its own
// package, and those reach the create path with the pool's handle untouched.
//
// Dump 30704 (2026-09-11) is that case. Nine swarm units from the pool copy of
// npcs_psec03_lower were the only pool actors still alive after a checkpoint
// restart. The restart destroyed every enemy the level owned, the Pregnants
// among them, and rebuilt the state tables. The swarm units survived on the
// pool's handle, and the first one to tick a machine whose table had no
// successor faulted in StateMachine::Update. Same family as
// analysis/STATE_TABLE_GENERATIONS.md, entered through the back door.
//
// The create function 0x00776480 reads the handle from packet+0x08 once, at
// 0x007764A0, and the constructor it calls reads the same field again for
// entity+0x38. So the packet itself has to change before either read. This
// hook sits on that first read. It only touches packets that name the pool
// root, and only hands over a stream that is ready right now, so no packet
// ever names a stream the engine would refuse to register against.
namespace pool_ownership {
safetyhook::MidHook createHook;
volatile LONG seen = 0, followed = 0, noLevel = 0, levelNotReady = 0, invalidManager = 0;

// 0x007764A0 on the game thread. esi is the packet, ebx the entity manager.
void OnCreate(SafetyHookContext& ctx) {
    if (!settings.enabled || !settings.poolActorsFollowLevel) return;
    uint32_t handle = 0;
    if (!Read(ctx.esi + 0x08, &handle, 4) || !handle) return;
    const uint32_t root = streamLifetime.handle;
    if (!root || handle != root) return;
    InterlockedIncrement(&seen);
    uint32_t manager = 0;
    if (!Read(base + 0x1C1C0A0, &manager, 4) || !manager || manager != ctx.ebx) {
        InterlockedIncrement(&invalidManager);
        return;
    }
    const uint32_t level = static_cast<uint32_t>(InterlockedCompareExchange(&lastLevelHandle, 0, 0));
    if (!level || level == root) { InterlockedIncrement(&noLevel); return; }
    // The same status call the create path makes a few instructions later,
    // on the same manager. A level stream that is not ready any more keeps
    // the packet as it is; that is what the game did before this hook.
    uint32_t streams = 0;
    using Status = int (__thiscall*)(void*, uint32_t);
    if (!Read(base + 0x1C1C098, &streams, 4) || !streams ||
        reinterpret_cast<Status>(base + 0xDAA0)(reinterpret_cast<void*>(streams), level) != 3) {
        InterlockedIncrement(&levelNotReady);
        return;
    }
    *reinterpret_cast<uint32_t*>(ctx.esi + 0x08) = level;
    InterlockedIncrement(&followed);
}

bool Install(FILE* log) {
    // mov ecx,[esi+8] / mov [esp+10h],ecx / test [ebx+28h],dl
    const unsigned char expected[] = {0x8B,0x4E,0x08,0x89,0x4C,0x24,0x10,0x84,0x53,0x28};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + 0x3764A0, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected))) {
        std::fprintf(log, "pool_children create signature mismatch; children keep the pool handle\n");
        return false;
    }
    createHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x3764A0), OnCreate);
    if (!createHook) {
        std::fprintf(log, "pool_children hook failed; children keep the pool handle\n");
        return false;
    }
    std::fprintf(log, "pool_children hook installed at %08X follow_level=%d\n",
        static_cast<unsigned>(base + 0x3764A0), settings.poolActorsFollowLevel ? 1 : 0);
    return true;
}

void Summary(FILE* log) {
    // `seen` counts packets that named the pool root at the create path. The
    // swapped enemies never show up here, their packet already carries the
    // level's handle. What does show up is whatever a pool enemy spawns by
    // itself. `no_level` means no spawner lookup has been seen yet in this
    // level, `level_not_ready` that the remembered stream is gone.
    std::fprintf(log, "pool_children seen=%ld followed=%ld no_level=%ld level_not_ready=%ld invalid_manager=%ld level=%08X\n",
        InterlockedCompareExchange(&seen, 0, 0), InterlockedCompareExchange(&followed, 0, 0),
        InterlockedCompareExchange(&noLevel, 0, 0), InterlockedCompareExchange(&levelNotReady, 0, 0),
        InterlockedCompareExchange(&invalidManager, 0, 0),
        static_cast<unsigned>(InterlockedCompareExchange(&lastLevelHandle, 0, 0)));
}
}
