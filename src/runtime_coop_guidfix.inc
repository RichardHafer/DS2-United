// Dead Space 2 United by Richard Hafer.
// Two Isaacs, one GUID.
//
// The partner's ghost Player is built from the replicated Player of the
// other machine, and that Player is the level's Player entity, GUID
// 36CAB03F-2C39E63A-45544150-3230314C on every machine. So the ghost is
// registered under the same GUID as the local Isaac, and it was registered
// later, so it sits at the head of the registry's bucket chain. Every script
// node that references "the Player" by GUID (a cutscene's actor, its camera
// target, the sequence end) got the ghost instead of the local Isaac.
//
// Two crash dumps of 16:28 (deadspace2.exe.33708 / .36236, both machines,
// same second) end in Player+0x230's sequence-end handler (0x00892690)
// running on the GHOST Player: its local-player index (Player+0x420) is 1,
// the per-local-player table at [0x0202131C] holds one entry, the lookup at
// 0x00793EC0 returns index 4 on a miss, and 0x0084C060 walks garbage. The
// broken cutscene cameras of the new-game intro are the same lookup a few
// steps earlier.
//
// The registry lookup (0x00776CA0, `thiscall(guid*, context)`, 133 call
// sites) is wrapped: a ghost Player coming back is answered with the local
// Isaac. Nothing in the campaign wants a ghost by GUID; the net layer
// addresses ghosts by index.
//
// Not at the prologue (20:40). The DS2 Debug Menu (DS2_Enemy_Spawner.asi)
// verifies the retail bytes of the functions it binds before it uses them,
// and its suit change looks suits up in "the retail object database", this
// registry. With a jump written over FindByGuid's first bytes it refused:
// "suit change blocked: retail binding mismatch". So the function's start is
// left alone and the three epilogues are hooked instead; each is exactly
// `pop edi / pop esi / ret 8`, five bytes, with eax the result and esi still
// the GUID pointer loaded at entry. The answer is rewritten in eax.
namespace guidfix {
bool enabled = false, installed = false;
constexpr uint32_t kFindByGuidRva = 0x376CA0;
constexpr uint32_t kEpilogueRvas[] = {0x376D20, 0x376D4E, 0x376D91};
safetyhook::MidHook epilogueHooks[3];
volatile LONG lookups = 0, redirected = 0, noLocal = 0;

// Any entity that is a ghost: an Entity's NetReflectable sits at +0x5C, its
// proxy at +0x0C on that, the ghost bit is proxy+0x30 bit 1 (the same three
// reads IsGhostPlayer makes, without the Player vtable check).
bool IsGhostEntity(uint32_t entity) {
    uint32_t vtable = 0, proxy = 0, flags = 0;
    if (!Read(entity, &vtable, 4) || vtable < base + gate::kRdataLo || vtable >= base + gate::kRdataHi) return false;
    if (!Read(entity + 0x5C + 0x0C, &proxy, 4) || !proxy) return false;
    return Read(proxy + 0x30, &flags, 4) && (flags & 2);
}
// A local object with the same GUID as a ghost. The registry's hash buckets
// (+0x4074) turned out to be an empty cache in the live game (probe 19:40:
// 4096 nulls, the real search is 0x007747C0 over [registry+0x68]); the list
// that does hold every entity is the NetReflectable chain at 0x0205B744
// (node - 0x5C = entity, next at node+4), the one the NPC scan and the hit
// resolver walk. A ghost NPC carries the authored GUID of the enemy it
// mirrors (17:50: the stasis-room necromorph), so a scripted sequence asking
// for its actor by GUID got the ghost, which is animated by the host, not by
// the sequence. If a LOCAL entity with that GUID exists, that is the one the
// script meant.
volatile LONG redirectedNpc = 0, ghostOnly = 0;
constexpr uint32_t kReflectableListRva = 0x1C5B744, kEntityReflectable = 0x5C;
uint32_t LocalTwin(uint32_t, uint32_t ghost, const uint32_t* guid) {
    uint32_t node = 0;
    if (!Read(base + kReflectableListRva, &node, 4)) return 0;
    for (unsigned steps = 0; node && steps < 4096; ++steps) {
        const uint32_t entity = node - kEntityReflectable;
        uint32_t g[4]{};
        if (entity != ghost && Read(entity + 0x24, g, 16) && std::memcmp(g, guid, 16) == 0 && !IsGhostEntity(entity)) return entity;
        uint32_t next = 0;
        if (!Read(node + 4, &next, 4) || next == node) break;
        node = next;
    }
    return 0;
}
volatile LONG redirectedBot = 0;
uint32_t Redirect(uint32_t found, const uint32_t* guid) {
    if (IsBotEntity(found)) {
        const uint32_t local = LocalSlotZeroEntity();
        if (local && !IsBotEntity(local)) { InterlockedIncrement(&redirectedBot); return local; }
    }
    if (!enabled || !found || !guid) return found;
    InterlockedIncrement(&lookups);
    if (IsGhostPlayer(found)) {
        // Never hand a script the ghost Player: it is animated by the host and
        // the sequence code walks its Player+0x420 index (the double crash of
        // dumps 33708/36236). While the human plays as a necromorph the local
        // body in slot 0 is the NecroPlayer; that is the actor the script meant.
        uint32_t local = LocalCampaignPlayer();
        if (!local) local = LocalSlotZeroEntity();
        if (!local) { InterlockedIncrement(&noLocal); return 0; }
        InterlockedIncrement(&redirected);
        return local;
    }
    if (!IsGhostEntity(found)) return found;
    const uint32_t twin = LocalTwin(0, found, guid);
    if (!twin) {
        InterlockedIncrement(&ghostOnly);
        // A script asking for a ghost with no local twin right after a
        // sequence cue: that is a sequence whose actor was never spawned here
        // (the spawn came earlier than the window, or the host did it long
        // ago). Logged once per GUID so the case can be seen and the spawn
        // rule tuned; the church grab is the one to watch (18:30).
        const DWORD sequenceAt = static_cast<DWORD>(InterlockedCompareExchange(&gate::lastSequenceTick, 0, 0));
        if (sequenceAt && GetTickCount() - sequenceAt <= 5000) {
            static uint32_t noted[8][4]{};
            static unsigned notedHead = 0;
            bool seen = false;
            for (auto& n : noted) if (std::memcmp(n, guid, 16) == 0) { seen = true; break; }
            if (!seen) {
                std::memcpy(noted[notedHead], guid, 16); notedHead = (notedHead + 1) % 8;
                uint32_t vtable = 0; Read(found, &vtable, 4);
                gate::Event("coop_guidfix sequence wants ghost %08X (vtable %08X) guid=%08X-%08X-%08X-%08X - no local actor",
                            found, vtable, guid[0], guid[1], guid[2], guid[3]);
            }
        }
        return found;
    }
    InterlockedIncrement(&redirectedNpc);
    return twin;
}
// FindByGuid(guid, after) also walks every entity with that GUID: a caller loops with the last
// result as `after` (the scripted sequence's actors, 0x00EB8300..0x00EB8396). A redirected result
// sent that walk back to an entity before the ghost, the ghost came again, and the loop never
// ended (16.10., the watching necro's client froze in a scene with its main thread in here; the
// exit guard restarted it). In a walk nothing is redirected: ghosts and bots are stepped over to
// the next entity with that GUID. At the epilogues (pop edi / pop esi / ret 8): [esp+0x10] = after.
thread_local bool walking = false;
volatile LONG walks = 0, walkSkips = 0, walkEnds = 0;
__declspec(noinline) uint32_t FindNext(const uint32_t* guid, uint32_t after) {
    uint32_t sim = 0;
    if (!Read(base + 0x1C1C0A0, &sim, 4) || !sim) return 0;
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*, const uint32_t*, uint32_t)>(base + kFindByGuidRva)(reinterpret_cast<void*>(sim), guid, after);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
bool Skippable(uint32_t entity) { return entity && (IsBotEntity(entity) || (enabled && (IsGhostPlayer(entity) || IsGhostEntity(entity)))); }
void OnEpilogue(SafetyHookContext& ctx) {
    if (walking) return;
    const auto guid = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(ctx.esi));
    uint32_t after = 0;
    Read(static_cast<uint32_t>(ctx.esp) + 0x10, &after, 4);
    if (!after) { ctx.eax = Redirect(static_cast<uint32_t>(ctx.eax), guid); return; }
    InterlockedIncrement(&walks);
    uint32_t found = static_cast<uint32_t>(ctx.eax);
    if (!Skippable(found)) return;
    walking = true;
    for (unsigned i = 0; i < 64 && Skippable(found); ++i) found = FindNext(guid, found);
    walking = false;
    if (Skippable(found)) found = 0;
    InterlockedIncrement(found ? &walkSkips : &walkEnds);
    ctx.eax = found;
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "GhostGuidRedirect", 1, ini.c_str()) != 0;
    // The prologue is only checked, never touched: push esi / mov esi,[esp+8] / mov edx,[esi]
    static const unsigned char kSig[] = {0x56,0x8B,0x74,0x24,0x08,0x8B,0x16};
    unsigned char actual[8]{};
    if (!Read(base + kFindByGuidRva, actual, sizeof(kSig)) || std::memcmp(actual, kSig, sizeof(kSig)) != 0) {
        std::fprintf(log, "coop_guidfix FindByGuid signature mismatch at %08X; off\n", static_cast<unsigned>(base + kFindByGuidRva));
        return false;
    }
    static const unsigned char kEpilogue[] = {0x5F,0x5E,0xC2,0x08,0x00};   // pop edi / pop esi / ret 8
    unsigned ok = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (!Read(base + kEpilogueRvas[i], actual, sizeof(kEpilogue)) || std::memcmp(actual, kEpilogue, sizeof(kEpilogue)) != 0) {
            std::fprintf(log, "coop_guidfix epilogue %u signature mismatch at %08X\n", i, static_cast<unsigned>(base + kEpilogueRvas[i]));
            continue;
        }
        auto result = safetyhook::MidHook::create(reinterpret_cast<void*>(base + kEpilogueRvas[i]), OnEpilogue);
        if (!result) { std::fprintf(log, "coop_guidfix epilogue %u hook failed\n", i); continue; }
        epilogueHooks[i] = std::move(*result);
        ++ok;
    }
    installed = ok == 3;
    std::fprintf(log, "coop_guidfix enabled=%d epilogues=%u/3 (a ghost found by GUID is answered with the local object; prologue untouched for the debug menu)\n", enabled, ok);
    return installed;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&redirected, 0, 0) + InterlockedCompareExchange(&redirectedNpc, 0, 0) +
                     InterlockedCompareExchange(&redirectedBot, 0, 0) + InterlockedCompareExchange(&walkSkips, 0, 0) + InterlockedCompareExchange(&walkEnds, 0, 0);
    if (now == last) return;
    last = now;
    std::fprintf(log, "coop_guidfix lookups=%ld redirected_ghost=%ld no_local=%ld redirected_npc=%ld ghost_only=%ld redirected_bot=%ld walks=%ld stepped_over=%ld walk_ends=%ld\n",
                 InterlockedCompareExchange(&lookups, 0, 0), InterlockedCompareExchange(&redirected, 0, 0), InterlockedCompareExchange(&noLocal, 0, 0),
                 InterlockedCompareExchange(&redirectedNpc, 0, 0), InterlockedCompareExchange(&ghostOnly, 0, 0),
                 InterlockedCompareExchange(&redirectedBot, 0, 0), InterlockedCompareExchange(&walks, 0, 0), InterlockedCompareExchange(&walkSkips, 0, 0),
                 InterlockedCompareExchange(&walkEnds, 0, 0));
}
}  // namespace guidfix
