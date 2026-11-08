// Dead Space 2 United by Hafer.
// Who just appeared (18.10.: a foreign mod's spawn menu places an unused zero-G tripod that half
// works and sometimes crashes). Twice a second the SimManager's objects are walked as for the
// debug menu's "show all enemies"; every enemy the level did not have a moment ago gets one line
// with its class, GUID, place, health and whether Isaac is in zero-G. Tripods and bosses are
// followed: a line every 2 s while they live (bounded), and one when they are gone.
// [Debug] NpcWatch=1.
namespace npcwatch {
struct Class { uint32_t vtableRva; const char* name; bool follow; };
// Primary vtables (analysis/ds2_classmap_live_2026-09-17.json; the bot_sense kinds plus the tripods).
constexpr Class kClasses[] = {
    {0x1783948, "Zombie (slasher)", false}, {0x1786DF0, "PackEnemy", false},  {0x17864F8, "SwarmEnemy", false},
    {0x1788B10, "TubeBeetle", false},       {0x176C400, "Leaper", false},     {0x176CA40, "Lurker", false},
    {0x1785378, "Exploder", false},         {0x176BE80, "Infector", false},   {0x1789018, "Puker", false},
    {0x178CCA8, "Stalker", false},          {0x176D428, "Brute", true},       {0x176EAD8, "Hunter (Ubermorph)", true},
    {0x1784410, "DividerCore", false},      {0x1784930, "DividerPart", false}, {0x1789F90, "Guardian", true},
    {0x1787970, "Tripod", true},            {0x1788330, "WindowTripod", true}, {0x1773CE8, "BossTripod", true},
    {0x1787E28, "TripodProjectile", false}, {0x1774B78, "NicoleBoss", true},
    {0x178D508, "Landmine (Cyst)", false},
};
constexpr unsigned kClassCount = sizeof(kClasses) / sizeof(kClasses[0]);
constexpr DWORD kScanMs = 500, kSampleMs = 2000;
constexpr LONG kMaxLines = 400, kMaxPerClass = 30, kMaxSamples = 30;
constexpr uint32_t kSimManagerRva = 0x1C1C0A0, kNpcGuid = 0x24, kNpcPos = 0xC0;
bool enabled = true;
struct Known {
    uint32_t entity = 0;
    unsigned cls = 0;
    Guid guid{};
    DWORD bornAt = 0, sampleAt = 0;
    float pos[3]{}, health = 0.0f;
    LONG samples = 0;
    uint32_t reportedTwin = 0;
    uint32_t lastProxy = UINT32_MAX, lastKey = 0;
    bool alive = true;
};
std::vector<Known> known;
DWORD scanAt = 0;
bool wasInLevel = false;
volatile LONG scans = 0, newOnes = 0, goneOnes = 0, lines = 0, faults = 0, overBudget = 0;
LONG perClass[kClassCount]{}, seenPerClass[kClassCount]{};

bool Line() {
    if (InterlockedIncrement(&lines) <= kMaxLines) return true;
    InterlockedIncrement(&overBudget);
    return false;
}
int ClassOf(uint32_t vtable) {
    for (unsigned i = 0; i < kClassCount; ++i)
        if (vtable == base + kClasses[i].vtableRva) return static_cast<int>(i);
    return -1;
}
bool IsaacZeroG() {
    const uint32_t me = LocalSlotZeroEntity();
    const uint32_t cmc = me ? bot::CmcOf(me) : 0;
    uint32_t flags = 1;
    return cmc && Read(cmc + bot::kCmcFlags, &flags, 4) && !(flags & 1);
}
float Health(uint32_t entity) {
    const uint32_t hc = hits::FindHealthComponent(entity);
    float health = -1.0f;
    if (hc) Read(hc + hits::kHealthCurrent, &health, 4);
    return health;
}
void Scan(DWORD now) {
    uint32_t sim = 0;
    if (!Read(base + kSimManagerRva, &sim, 4) || !sim) return;
    for (Known& k : known) k.alive = false;
    float me[3]{};
    const uint32_t local = LocalSlotZeroEntity();
    const bool haveMe = local && Read(local + kNpcPos, me, 12);
    bool zeroGKnown = false, zeroG = false;
    for (uint32_t b = 0; b < 4096; ++b) {
        uint32_t node = 0;
        if (!Read(sim + 0x70 + b * 4, &node, 4)) break;
        for (unsigned guard = 0; node && guard < 64; ++guard) {
            uint32_t object = 0, vtable = 0;
            if (Read(node + 0x40, &object, 4) && object && Read(object, &vtable, 4)) {
                const int cls = ClassOf(vtable);
                if (cls >= 0 && !IsPlayerOrNecroEntity(object)) {
                    Guid guid{};
                    Read(object + kNpcGuid, guid.data(), 16);
                    Known* k = nullptr;
                    for (Known& e : known)
                        if (e.entity == object && e.guid == guid) { k = &e; break; }
                    float pos[3]{};
                    const bool havePos = Read(object + kNpcPos, pos, 12) && std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]);
                    if (!k) {
                        Known e;
                        e.entity = object;
                        e.cls = static_cast<unsigned>(cls);
                        e.guid = guid;
                        e.bornAt = e.sampleAt = Stamp(now);
                        if (havePos) std::memcpy(e.pos, pos, 12);
                        e.health = Health(object);
                        known.push_back(e);
                        k = &known.back();
                        InterlockedIncrement(&newOnes);
                        ++seenPerClass[cls];
                        if (!zeroGKnown) { zeroG = IsaacZeroG(); zeroGKnown = true; }
                        const float away = haveMe && havePos ? std::sqrt((pos[0] - me[0]) * (pos[0] - me[0]) + (pos[1] - me[1]) * (pos[1] - me[1]) +
                                                                         (pos[2] - me[2]) * (pos[2] - me[2]))
                                                             : -1.0f;
                        if ((kClasses[cls].follow || ++perClass[cls] <= kMaxPerClass) && Line())
                            gate::Event("npcwatch: new %s %08X (vtable %08X) guid %08X-%08X-%08X-%08X at (%.1f %.1f %.1f) %.0f m from Isaac, health %.0f, "
                                        "Isaac %s",
                                        kClasses[cls].name, object, vtable, guid[0], guid[1], guid[2], guid[3], pos[0], pos[1], pos[2], away, e.health,
                                        zeroG ? "in ZERO-G" : "in gravity");
                    }
                    if (cls == 0 && guid != Guid{} && !k->alive) {
                        for (Known& other : known) {
                            if (!other.alive || other.cls != 0 || other.entity == object || other.guid != guid) continue;
                            if (k->reportedTwin == other.entity || other.reportedTwin == object) continue;
                            k->reportedTwin = other.entity;
                            if (Line())
                                gate::Event("npcwatch: simultaneous slashers guid=%08X-%08X-%08X-%08X actors=%08X,%08X ghost_flags=%d,%d",
                                            guid[0], guid[1], guid[2], guid[3], object, other.entity,
                                            guidfix::IsGhostEntity(object), guidfix::IsGhostEntity(other.entity));
                        }
                    }
                    k->alive = true;
                    if (vtable == base + 0x176CA40 || vtable == base + 0x178D508) {
                        const uint32_t reflectable = object + hits::kEntityReflectable;
                        uint32_t proxy = 0, key = 0;
                        Read(reflectable + hits::kReflectableProxy, &proxy, 4);
                        if (proxy) Read(reflectable + hits::kSubKeyFromReflectable, &key, 4);
                        const float health = Health(object);
                        if ((proxy != k->lastProxy || key != k->lastKey || health != k->health) && Line())
                            gate::Event("npcwatch: %s %08X guid=%08X-%08X-%08X-%08X key=%08X proxy=%08X ghost=%d health=%.1f",
                                        kClasses[cls].name, object, guid[0], guid[1], guid[2], guid[3], key, proxy,
                                        guidfix::IsGhostEntity(object), health);
                        k->lastProxy = proxy;
                        k->lastKey = key;
                        k->health = health;
                    }
                    if (kClasses[k->cls].follow && TicksSince(now, k->sampleAt) >= kSampleMs && k->samples < kMaxSamples) {
                        k->sampleAt = Stamp(now);
                        ++k->samples;
                        const float moved = havePos ? std::sqrt((pos[0] - k->pos[0]) * (pos[0] - k->pos[0]) + (pos[1] - k->pos[1]) * (pos[1] - k->pos[1]) +
                                                                (pos[2] - k->pos[2]) * (pos[2] - k->pos[2]))
                                                    : -1.0f;
                        const float health = Health(object);
                        if (Line())
                            gate::Event("npcwatch: %s %08X after %lu s at (%.1f %.1f %.1f), moved %.1f m in 2 s, health %.0f", kClasses[k->cls].name, object,
                                        static_cast<unsigned long>(TicksSince(now, k->bornAt) / 1000), pos[0], pos[1], pos[2], moved, health);
                        if (havePos) std::memcpy(k->pos, pos, 12);
                        k->health = health;
                    }
                }
            }
            if (!Read(node + 0x34, &node, 4)) break;
        }
    }
    for (size_t i = 0; i < known.size();) {
        Known& k = known[i];
        if (k.alive) { ++i; continue; }
        InterlockedIncrement(&goneOnes);
        if (kClasses[k.cls].follow && Line())
            gate::Event("npcwatch: %s %08X gone after %lu s (last health %.0f at %.1f %.1f %.1f)", kClasses[k.cls].name, k.entity,
                        static_cast<unsigned long>(TicksSince(now, k.bornAt) / 1000), k.health, k.pos[0], k.pos[1], k.pos[2]);
        known[i] = known.back();
        known.pop_back();
    }
}
__declspec(noinline) void ScanSafe(DWORD now) {
    __try {
        Scan(now);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&faults) <= 3) gate::Event("npcwatch: FAULT while walking the objects; the scan goes on next time");
    }
}
// Game thread, every frame.
void Tick(DWORD now) {
    if (!enabled) return;
    if (!inLevel) {
        if (wasInLevel) known.clear();   // a level change: the old pointers mean nothing
        wasInLevel = false;
        return;
    }
    wasInLevel = true;
    if (scanAt && TicksSince(now, scanAt) < kScanMs) return;
    scanAt = Stamp(now);
    InterlockedIncrement(&scans);
    ScanSafe(now);
}
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Debug", "NpcWatch", 1, ini.c_str()) != 0;
    std::fprintf(log, "npcwatch %s: new enemies of the level get a line (class, GUID, place, health); tripods and bosses are followed\n",
                 enabled ? "on" : "off ([Debug] NpcWatch=0)");
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    static LONG last = -1;
    const LONG sum = newOnes + goneOnes * 3 + faults * 7;
    if (sum == last) return;
    last = sum;
    std::string counts;
    for (unsigned i = 0; i < kClassCount; ++i)
        if (seenPerClass[i]) counts += std::string(counts.empty() ? "" : ", ") + kClasses[i].name + " " + std::to_string(seenPerClass[i]);
    std::fprintf(log, "npcwatch scans=%ld new=%ld gone=%ld tracked=%zu lines=%ld over_budget=%ld faults=%ld | %s\n", scans, newOnes, goneOnes, known.size(),
                 lines, overBudget, faults, counts.empty() ? "none" : counts.c_str());
}
}  // namespace npcwatch
