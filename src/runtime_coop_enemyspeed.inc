// Dead Space 2 United by Richard Hafer.
// Enemy speed per kind, one table for the story and one per game mode ([EnemySpeed]).
//
// The speed is the animation playback the game sets with Animated::SetAnimSpeed (0x00751F40);
// walking is root motion, so movement and attacks follow it. The randomizer half owns the hook on
// it and asks FactorFor for every call; the value the game asked for is kept, so a changed factor
// is applied by asking for the same value again. An animation that has to fit something else runs
// at the game's own speed:
//   - a scripted sequence: the NPC's IScriptedSequenceTarget at +0x230 (sequence +0x238, flags +0x240
//     bit 2, as DeadSpaceNPC::TestInteruptScriptSequence 0x00FED230 reads them),
//   - a grab: Sentient's paired coordinator at +0x2A8 (SetPairedCoordinator 0x00EA5100),
//   - the spawn, vents, a corpse being infected, the death: state names of its machines,
//   - the callers that time an animation against another one (enemy_speed::TimedCaller).
// A client's ghosts are left alone: the host moves them and SetAnimSpeed sends its speed along.
namespace enemyspeed {
constexpr uint32_t kViewInner = 0x194, kSpeed = 0x134, kSequence = 0x238, kSequenceFlags = 0x240, kPairedCoordinator = 0x2A8, kSetAnimSpeedRva = 0x351F40;
bool enabled = false, hooked = false, hookTried = false;
enemy_speed::Tables tables;
std::string iniPath;
FILETIME iniStamp{};
DWORD iniLookAt = 0, scanAt = 0, stateAt = 0, hookLookAt = 0;
int modeNow = enemy_speed::kStory;
struct Slot {
    uint32_t actor = 0, vtable = 0, inner = 0;
    uint32_t guid[4]{};
    int kind = enemy_speed::kNone;
    float raw = 1.0f, factor = 1.0f;   // what the game asked for; the factor last folded in
    bool busy = false, alive = true, timed = false;
    DWORD seenAt = 0, timedAt = 0;
};
Slot slots[128];
unsigned slotCount = 0;
volatile LONG tracked = 0, dropped = 0, full = 0, folded = 0, applied = 0, sequenceHolds = 0, grabHolds = 0, stateHolds = 0, timedHolds = 0, ghostsSkipped = 0,
              faults = 0, reloads = 0, lines = 0;
LONG perKind[enemy_speed::kKindCount]{};
struct HeldState { char name[28]; LONG count; };
HeldState heldStates[32]{};

int KindOf(uint32_t vtable) {
    using namespace enemy_speed;
    static constexpr struct { uint32_t rva; int kind; } kKinds[] = {
        {0x1783948u, kSlasher}, {0x1786DF0u, kPack}, {0x17864F8u, kSwarm}, {0x1788B10u, kTubeBeetle}, {0x176C400u, kLeaper}, {0x176CA40u, kLurker},
        {0x1785378u, kExploder}, {0x176BE80u, kInfector}, {0x1789018u, kPuker}, {0x176EFF8u, kPregnant}, {0x178CCA8u, kStalker}, {0x176D428u, kBrute},
        {0x176EAD8u, kUbermorph}, {0x1784410u, kDivider}, {0x1784930u, kDividerPart}, {0x1787970u, kTripod}, {0x1788330u, kTripod}, {0x1789F90u, kGuardian},
        {0x178A420u, kGuardian}, {0x1785D40u, kNest}, {0x1773CE8u, kBoss}, {0x1774B78u, kBoss},
    };
    for (const auto& k : kKinds) if (vtable == base + k.rva) return k.kind;
    return kNone;
}
Slot* Find(uint32_t actor) {
    for (unsigned i = 0; i < slotCount; ++i) if (slots[i].actor == actor) return &slots[i];
    return nullptr;
}
bool Same(const Slot& s) {
    uint32_t vtable = 0, inner = 0, guid[4]{};
    return Read(s.actor, &vtable, 4) && vtable == s.vtable && Read(s.actor + 0x24, guid, 16) && std::memcmp(guid, s.guid, 16) == 0 &&
           Read(s.actor + kViewInner, &inner, 4) && inner == s.inner;
}
bool InSequence(uint32_t actor) {
    uint32_t sequence = 0;
    uint8_t flags = 0;
    return Read(actor + kSequence, &sequence, 4) && sequence && Read(actor + kSequenceFlags, &flags, 1) && (flags & 4);
}
bool InGrab(uint32_t actor) {
    uint32_t coordinator = 0;
    return Read(actor + kPairedCoordinator, &coordinator, 4) && coordinator;
}
// Every machine of its tree, as laserdiag::OwnerStates walks it.
bool InOwnSpeedState(uint32_t actor) {
    namespace m = ::ds2::watchdog::machine;
    uint32_t stack[32]{}, root = 0;
    unsigned depth = 0, visited = 0;
    if (!Read(actor + m::kActorTree + m::kTreeRoot, &root, 4) || !root) return false;
    stack[depth++] = root;
    while (depth && visited < 32) {
        const uint32_t sm = stack[--depth];
        ++visited;
        uint32_t array = 0, index = 0, name = 0, child = 0, next = 0;
        char text[28]{};
        if (Read(sm + m::kStateArr, &array, 4) && array && Read(sm + m::kCurState, &index, 4) && index < 4096 &&
            Read(array + index * m::kStateSize + m::kStateName, &name, 4) && name && Read(name, text, sizeof(text) - 1) && enemy_speed::KeepsOwnSpeed(text)) {
            for (HeldState& h : heldStates) {
                if (h.name[0] && std::strcmp(h.name, text) != 0) continue;
                if (!h.name[0]) std::snprintf(h.name, sizeof(h.name), "%s", text);
                ++h.count;
                break;
            }
            return true;
        }
        if (Read(sm + m::kChildHead, &child, 4) && child && depth < 31) stack[depth++] = child;
        if (Read(sm + m::kNext, &next, 4) && next && depth < 31) stack[depth++] = next;
    }
    return false;
}
float Wanted(const Slot& s) { return enabled && s.alive && !s.busy && !s.timed ? tables.Factor(modeNow, s.kind) : 1.0f; }

// From the SetAnimSpeed hook, any caller. `caller` is the return address.
float __cdecl FactorFor(uint32_t actor, float raw, uint32_t caller) {
    if (!enabled) return 1.0f;
    Slot* s = Find(actor);
    if (!s || !Same(*s)) return 1.0f;
    s->raw = raw;
    if (caller >= base && enemy_speed::TimedCaller(caller - static_cast<uint32_t>(base))) {
        if (!s->timed) InterlockedIncrement(&timedHolds);
        s->timed = true;
        s->timedAt = GetTickCount();
    }
    // a sequence or a grab that started since the last look
    if (!s->busy && (InSequence(actor) || InGrab(actor))) s->busy = true;
    s->factor = Wanted(*s);
    if (s->factor != 1.0f) InterlockedIncrement(&folded);
    return s->factor;
}
__declspec(noinline) void Ask(uint32_t actor, float raw) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, float)>(base + kSetAnimSpeedRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(actor)), raw);
        InterlockedIncrement(&applied);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}

using HookFn = int(__cdecl*)(float(__cdecl*)(uint32_t, float, uint32_t));
using VariantFn = float(__cdecl*)(uint32_t);
VariantFn variantFn = nullptr;
bool Hook(DWORD now) {
    if (hooked || hookTried) return hooked;
    if (now - hookLookAt < 1000) return false;
    hookLookAt = now;
    const HMODULE module = ds2coop::PartnerModule("DS2RandomizerObserver.asi");
    const auto hook = module ? reinterpret_cast<HookFn>(GetProcAddress(module, "DS2Randomizer_SetAnimSpeedFactor")) : nullptr;
    variantFn = module ? reinterpret_cast<VariantFn>(GetProcAddress(module, "DS2Randomizer_VariantSpeed")) : nullptr;
    const int result = hook && variantFn ? hook(FactorFor) : -1;
    if (result == 0) return false;   // the randomizer half is not up yet
    hookTried = true;
    hooked = result == 1;
    gate::Event("enemy speed: SetAnimSpeed hook %s", hooked ? "in" : result < 0 && !hook ? "NOT in (the randomizer half has no export)" : "NOT in (signature)");
    return hooked;
}
void LoadTables(const char* why) {
    const char* ini = iniPath.c_str();
    const bool was = enabled;
    enabled = GetPrivateProfileIntA("EnemySpeed", "Enabled", 0, ini) != 0;
    tables = enemy_speed::Load([&](const char* section, const char* key, int fallback) {
        return static_cast<int>(GetPrivateProfileIntA(section, key, fallback, ini));
    });
    gate::Event("enemy speed %s (%s%s)", enabled ? "on" : "off", why, was != enabled ? ", switched" : "");
    if (!enabled) return;
    for (int m = 0; m < enemy_speed::kModeCount; ++m) {
        char text[320];
        size_t n = static_cast<size_t>(std::snprintf(text, sizeof(text), "enemy speed table %s:", enemy_speed::ModeSection(m)));
        for (int k = 0; k < enemy_speed::kKindCount && n + 24 < sizeof(text); ++k)
            if (tables.percent[m][k] != 100) n += static_cast<size_t>(std::snprintf(text + n, sizeof(text) - n, " %s=%d", enemy_speed::KindKey(k), tables.percent[m][k]));
        gate::Event("%s", text);
    }
}
bool Stamp(FILETIME& out) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(iniPath.c_str(), GetFileExInfoStandard, &data)) return false;
    out = data.ftLastWriteTime;
    return true;
}
void Scan(DWORD now) {
    uint32_t sim = 0;
    if (!Read(base + 0x1C1C0A0, &sim, 4) || !sim) { slotCount = 0; return; }
    for (uint32_t b = 0; b < 4096; ++b) {
        uint32_t node = 0;
        if (!Read(sim + 0x70 + b * 4, &node, 4)) break;
        for (unsigned guard = 0; node && guard < 64; ++guard) {
            uint32_t object = 0, vtable = 0, inner = 0;
            if (Read(node + 0x40, &object, 4) && object && Read(object, &vtable, 4)) {
                const int kind = KindOf(vtable);
                if (kind != enemy_speed::kNone && Read(object + kViewInner, &inner, 4) && inner) {
                    Slot* s = Find(object);
                    if (s && Same(*s)) s->seenAt = now;
                    else if (guidfix::IsGhostEntity(object)) InterlockedIncrement(&ghostsSkipped);
                    else {
                        if (!s && slotCount < std::size(slots)) s = &slots[slotCount++];
                        if (!s) InterlockedIncrement(&full);
                        else {
                            *s = Slot{};
                            s->actor = object;
                            s->vtable = vtable;
                            s->inner = inner;
                            s->kind = kind;
                            s->seenAt = now;
                            Read(object + 0x24, s->guid, 16);
                            float current = 1.0f;
                            const float variant = variantFn ? variantFn(object) : 1.0f;
                            if (Read(inner + kSpeed, &current, 4) && std::isfinite(current) && current >= 0.0f && variant > 0.0f) s->raw = current / variant;
                            InterlockedIncrement(&tracked);
                            InterlockedIncrement(&perKind[kind]);
                        }
                    }
                }
            }
            if (!Read(node + 0x34, &node, 4)) break;
        }
    }
    for (unsigned i = 0; i < slotCount;) {
        if (slots[i].seenAt == now) { ++i; continue; }
        InterlockedIncrement(&dropped);
        slots[i] = slots[--slotCount];
        slots[slotCount] = Slot{};
    }
}
void Update(DWORD now) {
    for (unsigned i = 0; i < slotCount; ++i) {
        Slot& s = slots[i];
        if (!Same(s)) continue;
        float health = 0.0f;
        const uint32_t hc = hits::FindHealthComponent(s.actor);
        s.alive = hc && Read(hc + hits::kHealthCurrent, &health, 4) && health > 0.0f;
        const bool sequence = InSequence(s.actor), grab = !sequence && InGrab(s.actor), state = !sequence && !grab && InOwnSpeedState(s.actor);
        const bool busy = sequence || grab || state;
        if (busy && !s.busy) InterlockedIncrement(sequence ? &sequenceHolds : grab ? &grabHolds : &stateHolds);
        s.busy = busy;
        if (s.timed && !busy && now - s.timedAt > 1500) s.timed = false;
        const float want = Wanted(s);
        if (want == s.factor) continue;
        if (InterlockedIncrement(&lines) <= 60)
            gate::Event("enemy speed: %s %08X x%.2f -> x%.2f (asked %.2f, %s%s)", enemy_speed::KindKey(s.kind), s.actor, s.factor, want, s.raw,
                        enemy_speed::ModeSection(modeNow), !s.alive ? ", dead" : sequence ? ", scripted sequence" : grab ? ", grab" : state ? ", spawn/vent/other" : s.timed ? ", timed" : "");
        Ask(s.actor, s.raw);
    }
}
int TickFilter() { InterlockedIncrement(&faults); return EXCEPTION_EXECUTE_HANDLER; }
void TickInner(DWORD now) {
    if (now - iniLookAt >= 2000) {
        iniLookAt = now;
        FILETIME stamp{};
        if (Stamp(stamp) && CompareFileTime(&stamp, &iniStamp) != 0) {
            iniStamp = stamp;
            InterlockedIncrement(&reloads);
            LoadTables("DS2United.ini changed");
        }
    }
    const bool running = mpmap::LmState(nullptr) == coop_mpmap::kRunning;
    if (!running) { slotCount = 0; return; }
    if (!enabled && !hooked) return;
    if (!Hook(now)) return;
    if (now - stateAt < 100) return;
    stateAt = now;
    const bool visit = mpmap::InVisit(), inArena = arena::RunningNow();
    const int picked = static_cast<int>(InterlockedCompareExchange(&gamemode::mode, 0, 0));
    const int mode = enemy_speed::ModeFor(visit || inArena, visit ? picked : arena::ModeNow(picked));
    if (mode != modeNow) {
        gate::Event("enemy speed: table %s -> %s", enemy_speed::ModeSection(modeNow), enemy_speed::ModeSection(mode));
        modeNow = mode;
    }
    // with nothing to change the scan stays off; a switched off table first puts the speeds back
    if (!enabled && !slotCount) return;
    if (enabled && now - scanAt >= 250) { scanAt = now; Scan(now); }
    Update(now);
    if (!enabled) slotCount = 0;
}
// Game thread, from coop::Poll.
void Tick() {
    if (iniPath.empty() || InterlockedCompareExchange(&faults, 0, 0) >= 20) return;
    __try { TickInner(GetTickCount()); }
    __except (TickFilter()) {}
}
void Install(FILE* log, const std::string& ini) {
    iniPath = ini;
    Stamp(iniStamp);
    LoadTables("start");
    std::fprintf(log, "enemy speed %s: [EnemySpeed] and one table per mode, %d to %d %%\n", enabled ? "on" : "off ([EnemySpeed] Enabled=0)", enemy_speed::kMinPercent,
                 enemy_speed::kMaxPercent);
}
void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG now = tracked + dropped * 3 + folded * 5 + applied * 7 + faults * 11 + reloads * 13;
    if (now == last || !hookTried) return;
    last = now;
    std::fprintf(log, "enemy_speed on=%d hook=%d mode=%s live=%u tracked=%ld dropped=%ld full=%ld folded=%ld applied=%ld holds sequence=%ld grab=%ld state=%ld timed=%ld "
                      "ghosts_skipped=%ld reloads=%ld faults=%ld |", enabled ? 1 : 0, hooked ? 1 : 0, enemy_speed::ModeSection(modeNow), slotCount, tracked, dropped, full,
                 folded, applied, sequenceHolds, grabHolds, stateHolds, timedHolds, ghostsSkipped, reloads, faults);
    for (int k = 0; k < enemy_speed::kKindCount; ++k) if (perKind[k]) std::fprintf(log, " %s=%ld", enemy_speed::KindKey(k), perKind[k]);
    std::fprintf(log, " | held states");
    for (const HeldState& h : heldStates) if (h.name[0]) std::fprintf(log, " %s=%ld", h.name, h.count);
    std::fprintf(log, "\n");
}
}  // namespace enemyspeed
