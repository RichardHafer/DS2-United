// Dead Space 2 United by Richard Hafer.
// Waypoints on the screen ([Marks] in DS2United.ini): the Hold the Zone zone and the
// Mercenaries time capsules, drawn by the overlay at the camera's projection
// (runtime_coop_overlay.inc, coop_marks.h). The zone comes from zone::shown (the
// director's or the host's), the capsules from the director through
// DS2Coop_SurvivalMarks (its own on the host or alone, its copies of the host's
// on a partner). Game thread; the overlay draws on the render thread.
namespace marks {
bool enabled = true, zoneWanted = true, capsulesWanted = true;
// [Nameplate] Overlay: the names over the partners' and bots' heads drawn here
// wherever the game's own MP plate cannot be built (it lives in global_mp_assets).
bool namesWanted = true;
// [Necro] Radar: a necro player (alive or at the vents, never in Versus) hears the humans'
// hearts within RadarRange metres, through walls. Drawn as kMarkHeart.
bool radarWanted = true;
float radarRange = 40.0f;
constexpr uint32_t kHeartColor = 0xFFFF3A3A;
volatile LONG radarTicks = 0, radarHearts = 0, radarOutOfRange = 0, radarNoBody = 0, radarGhostSelf = 0;
unsigned radarNow = 0;
// The partners' NecroPlayers get theirs here everywhere: the game's plate is a Player's
// only (13.10.: no name over a necro, Versus included). An enemy necro's name shows only
// while he is in sight, a teammate's always.
struct NameMark { uint32_t player; char name[24]; bool bot, necro, hidden; };
NameMark nameMarks[8]{};
unsigned nameMarkCount = 0;
DWORD namesScanAt = 0;
volatile LONG namesShown = 0, nameScans = 0, necroNames = 0, necroNamesHidden = 0, necroCorpsesSkipped = 0, necroDuplicatesSkipped = 0, necroNotLiving = 0;
constexpr uint32_t kNameColor = 0xF0E8F4FF, kBotNameColor = 0xF0A8D8F0, kNecroNameColor = 0xF0FF8A6A;
constexpr float kNecroHeadLift = 1.6f, kEnemyNameNoSightM = 15.0f;
constexpr uint32_t kGameManagerRva = 0x1C213FC;   // DGM; +0xF2 the local team
constexpr float kHeadLift = 2.05f;
int scalePercent = 100;
constexpr DWORD kCapsuleFreshMs = 1500;
struct Capsules { int count = 0; float pos[8][3]{}; bool golden[8]{}; DWORD at = 0; };
SRWLOCK lock = SRWLOCK_INIT;
Capsules capsules;
// The last enemies of a survival wave (the director's ShowLastEnemies); here or the host's.
struct Enemies { int count = 0; float pos[8][3]{}; DWORD at = 0; };
Enemies enemies;
bool enemiesWanted = true;   // [Marks] Enemies
// The debug menu's "show all enemies" ([Marks] AllEnemies starts it on): every hostile NPC
// of the level (the SimManager's objects, the kinds bot_sense knows, bosses included),
// alive, the nearest kAllMost with kind and health. Scanned five times a second.
volatile LONG showAll = 0;
constexpr unsigned kAllMost = 60;
constexpr uint32_t kAllEnemyColor = 0xFFFFE040, kAllBossColor = 0xFFFF6040;
struct Seen { float pos[3]; float health, dist2; uint8_t kind; };
Seen allSeen[kAllMost]{};
unsigned allCount = 0, allTotal = 0;
DWORD allScanAt = 0;
volatile LONG allScans = 0, allMost = 0, allFaults = 0;
const char* KindText(coop_bot::EnemyKind k) {
    static const char* const kNames[] = {"?", "SLASHER", "PACK", "SWARMER", "TUBE BEETLE", "LEAPER", "LURKER", "EXPLODER", "INFECTOR", "PUKER",
                                         "STALKER", "BRUTE", "UBERMORPH", "DIVIDER", "DIVIDER PART", "GUARDIAN", "BOSS", "NEST", "TENTACLE"};
    const unsigned i = static_cast<unsigned>(k);
    return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}
void ScanAll(const float* self) {
    uint32_t sim = 0;
    unsigned kept = 0, total = 0;
    Seen found[kAllMost]{};
    if (Read(base + 0x1C1C0A0, &sim, 4) && sim) {
        for (uint32_t b = 0; b < 4096; ++b) {
            uint32_t node = 0;
            if (!Read(sim + 0x70 + b * 4, &node, 4)) break;
            for (unsigned guard = 0; node && guard < 64; ++guard) {
                uint32_t object = 0, vtable = 0;
                if (Read(node + 0x40, &object, 4) && object && Read(object, &vtable, 4)) {
                    const auto kind = bot_sense::KindOf(vtable);
                    float pos[3]{}, health = 0.0f;
                    const uint32_t hc = kind != coop_bot::EnemyKind::Unknown && !IsPlayerOrNecroEntity(object) ? hits::FindHealthComponent(object) : 0;
                    if (hc && Read(hc + hits::kHealthCurrent, &health, 4) && health > 0.0f && Read(object + 0xC0, pos, 12) && std::isfinite(pos[0]) &&
                        std::isfinite(pos[1]) && std::isfinite(pos[2])) {
                        ++total;
                        const float dx = pos[0] - self[0], dy = pos[1] - self[1], dz = pos[2] - self[2];
                        Seen s{{pos[0], pos[1], pos[2]}, health, dx * dx + dy * dy + dz * dz, static_cast<uint8_t>(kind)};
                        // the nearest kAllMost: replace the farthest kept one
                        if (kept < kAllMost) {
                            found[kept++] = s;
                        } else {
                            unsigned farthest = 0;
                            for (unsigned i = 1; i < kept; ++i) if (found[i].dist2 > found[farthest].dist2) farthest = i;
                            if (s.dist2 < found[farthest].dist2) found[farthest] = s;
                        }
                    }
                }
                if (!Read(node + 0x34, &node, 4)) break;
            }
        }
    }
    std::memcpy(allSeen, found, sizeof(found));
    allCount = kept;
    allTotal = total;
    InterlockedIncrement(&allScans);
    if (static_cast<LONG>(total) > InterlockedCompareExchange(&allMost, 0, 0)) InterlockedExchange(&allMost, static_cast<LONG>(total));
}
__declspec(noinline) void ScanAllSafe(const float* self) {
    __try {
        ScanAll(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&allFaults);
        allCount = 0;
    }
}
constexpr DWORD kEnemyFreshMs = 2500;
volatile LONG enemySets = 0, enemiesMost = 0;
void SetEnemies(const float* xyz, int count) {
    if (count < 0 || !xyz) count = 0;
    if (count > 8) count = 8;
    AcquireSRWLockExclusive(&lock);
    enemies.count = count;
    for (int i = 0; i < count; ++i) std::memcpy(enemies.pos[i], xyz + i * 3, sizeof(enemies.pos[i]));
    enemies.at = Stamp(GetTickCount());
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement(&enemySets);
    if (count > InterlockedCompareExchange(&enemiesMost, 0, 0)) InterlockedExchange(&enemiesMost, count);
}
volatile LONG capsuleSets = 0, capsulesMost = 0, ticksShown = 0, hiddenPaused = 0, hiddenScene = 0, hiddenInside = 0, gameThread = 0;
// Any thread (the director).
void SetCapsules(const float* xyz, const int* golden, int count) {
    if (count < 0) count = 0;
    if (count > 8) count = 8;
    AcquireSRWLockExclusive(&lock);
    capsules.count = xyz ? count : 0;
    for (int i = 0; i < capsules.count; ++i) {
        std::memcpy(capsules.pos[i], xyz + i * 3, sizeof(capsules.pos[i]));
        capsules.golden[i] = golden && golden[i] != 0;
    }
    capsules.at = Stamp(GetTickCount());
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement(&capsuleSets);
    if (count > InterlockedCompareExchange(&capsulesMost, 0, 0)) InterlockedExchange(&capsulesMost, count);
}

uint32_t lastSim = 0;
DWORD simStillSince = 0;
constexpr uint32_t kZoneEmpty = 0xFF6FF2FF, kZoneCapturing = 0xFF8CFFB4, kZoneContested = 0xFFFF5A48, kZoneLosing = 0xFFFFB347;
constexpr uint32_t kCapsule = 0xFFFFC84A, kGoldenCapsule = 0xFFFFF0A0, kEnemyMark = 0xFFFF4A3A;
// The partners' necro ghosts in the PlayerManager slots, with the name their machine sent.
unsigned NecroGhosts(uint32_t* out, unsigned cap) {
    uint32_t manager = 0, slots = 0, capacity = 0;
    if (!Read(base + kPlayerManagerRva, &manager, 4) || !manager || !Read(manager + 4, &slots, 4) || !slots || !Read(manager + 8, &capacity, 4) ||
        !capacity || capacity > 8)
        return 0;
    unsigned n = 0;
    for (uint32_t i = 0; i < capacity && n < cap; ++i) {
        uint32_t bp = 0;
        if (Read(slots + 4 * i, &bp, 4) && bp >= 0x103C0 && IsGhostNecro(bp - 0x3C0)) out[n++] = bp - 0x3C0;
    }
    return n;
}
// An enemy necro in sight of our body: a cast from our eyes to his head, the hit on
// him counts as seen; without the cast only a near one.
bool InSight(uint32_t self, const float* selfFeet, uint32_t target, const float* head) {
    const coop_bot::Vec3 eye = {selfFeet[0], selfFeet[1] + 1.6f, selfFeet[2]}, to = {head[0], head[1], head[2]};
    float fraction = 1.0f;
    uint32_t hit = 0;
    if (bot::CastRay(eye, to, self, fraction, hit)) return fraction >= 0.98f || (hit && (hit == target || hit - 0x10 == target));
    const float dx = head[0] - selfFeet[0], dz = head[2] - selfFeet[2];
    return dx * dx + dz * dz <= kEnemyNameNoSightM * kEnemyNameNoSightM;
}
// Every 250 ms: the Players to name and their names, when the native plates are not here,
// and the partners' necros always.
void ScanNames(DWORD now, uint32_t self, const float* selfFeet, bool selfValid) {
    if (now - namesScanAt < 250) return;
    namesScanAt = now;
    nameMarkCount = 0;
    if (!namesWanted || !nameplate::enabled) return;
    InterlockedIncrement(&nameScans);
    if (!(nameplate::installed && !nameplate::residentLogged) || play::StoryCoop()) {
        uint32_t targets[8];
        const unsigned n = nameplate::Targets(targets, 8);
        for (unsigned i = 0; i < n && nameMarkCount < std::size(nameMarks); ++i) {
            NameMark& m = nameMarks[nameMarkCount];
            unsigned key = 0;
            if (IsGhostNecro(targets[i]) || nameplate::Dead(targets[i]) || roles::WatchingMachine(MachineOf(targets[i]), now) ||
                !nameplate::NameFor(targets[i], m.name, m.bot, key)) continue;
            m.player = targets[i];
            m.necro = m.hidden = false;
            ++nameMarkCount;
        }
    }
    uint32_t necros[4];
    const unsigned k = NecroGhosts(necros, 4);
    if (!k) return;
    uint32_t dgm = 0;
    uint8_t team = 0;
    const bool weAreNecro = Read(base + kGameManagerRva, &dgm, 4) && dgm && Read(dgm + 0xF2, &team, 1) && team == 1;   // 1 = necro
    // One name per partner, on his living necro only (17.10., screenshot: two "PLAYER 2" over the
    // client's earlier necro bodies, still ghosts in their PlayerManager slots after their deaths).
    bool named[256]{};
    for (unsigned i = 0; i < k && nameMarkCount < std::size(nameMarks); ++i) {
        if (nameplate::Find(necros[i])) continue;   // the game's own plate shows it
        const uint32_t machine = MachineOf(necros[i]);
        if (machine > 255 || roles::WatchingMachine(machine, now)) continue;
        if (restsync::Dead(necros[i])) { InterlockedIncrement(&necroCorpsesSkipped); continue; }
        bool living = false;
        for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s)
            if (peerTable.peers[s].used && peerTable.peers[s].machine == machine) living = roles::RoleOf(s, now) == 1;
        if (!living) { InterlockedIncrement(&necroNotLiving); continue; }
        if (named[machine]) { InterlockedIncrement(&necroDuplicatesSkipped); continue; }
        named[machine] = true;
        NameMark& m = nameMarks[nameMarkCount];
        if (nameplate::names[machine][0]) std::snprintf(m.name, sizeof(m.name), "%s", nameplate::names[machine]);
        else if (machine < 0xF0) std::snprintf(m.name, sizeof(m.name), "PLAYER %u", machine + 1);
        else continue;
        m.player = necros[i];
        m.bot = false;
        m.necro = true;
        m.hidden = false;
        if (!weAreNecro) {
            float feet[3]{};
            if (!selfValid || !Read(necros[i] + placement::kPlayerPosition, feet, sizeof(feet))) { m.hidden = true; }
            else {
                const float at[3] = {feet[0], feet[1] + kNecroHeadLift, feet[2]};
                m.hidden = !InSight(self, selfFeet, necros[i], at);
            }
            if (m.hidden) InterlockedIncrement(&necroNamesHidden);
        }
        InterlockedIncrement(&necroNames);
        ++nameMarkCount;
    }
}

void Tick(DWORD now) {
    const bool all = InterlockedCompareExchange(&showAll, 0, 0) != 0;
    const int role = necro::LocalRole();
    const bool radar = radarWanted && (role == 1 || role == 2) && necro::invasion && !play::VersusMode();
    if ((!enabled && !namesWanted && !all && !radar) || !overlay::ready) return;
    InterlockedExchange(&gameThread, static_cast<LONG>(GetCurrentThreadId()));
    // game time stands still in the pause menu
    const uint32_t sim = turret::SimMs();
    if (sim != lastSim) { lastSim = sim; simStillSince = now; }
    const bool paused = now - simStillSince > 300;
    // a necro machine has no Isaac: its NecroPlayer is the body the names are measured from
    uint32_t player = inLevel && !turret::FrontendNow() ? LocalCampaignPlayer() : 0;
    if (!player && inLevel && necro::necroEntity && IsNecroEntity(necro::necroEntity)) player = necro::necroEntity;
    // at the vents only the radar: the ghost proxy is where the hearts are measured from
    const uint32_t ghostSelf = !player && inLevel && radar ? necro::LocalGhost() : 0;
    const uint32_t self = player ? player : ghostSelf;
    // a ghost has no body in slot 0, which the level's settle follows: only the load counts then
    const bool worldReady = player ? turret::WorldReady() : !turret::FrontendNow() && (!loadsync::installed || loadsync::state == loadsync::kPlaying);
    if (!self || !worldReady || paused || turret::CutsceneNow()) {
        if (paused && self) InterlockedIncrement(&hiddenPaused);
        if (self && turret::CutsceneNow()) InterlockedIncrement(&hiddenScene);
        overlay::HideMarks();
        return;
    }
    overlay::Marks out{};
    out.scalePercent = scalePercent;
    out.selfValid = Read(self + 0xC0, out.self, sizeof(out.self)) && std::isfinite(out.self[0]) && std::isfinite(out.self[1]) && std::isfinite(out.self[2]);
    const auto add = [&](const float* pos, uint32_t color, uint8_t kind) -> overlay::Mark* {
        if (out.count >= static_cast<int>(std::size(out.m))) return nullptr;
        overlay::Mark& m = out.m[out.count++];
        std::memcpy(m.pos, pos, sizeof(m.pos));
        m.color = color;
        m.kind = kind;
        return &m;
    };
    if (radar && out.selfValid) {
        InterlockedIncrement(&radarTicks);
        if (ghostSelf) InterlockedIncrement(&radarGhostSelf);
        radarNow = 0;
        for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
            const coop_peers::Peer& peer = peerTable.peers[s];
            if (!peer.used || !peer.token || roles::IsNecro(s, now)) continue;
            const uint32_t body = GhostForMachine(peer.machine);
            float feet[3]{};
            if (!body || nameplate::Dead(body) || !Read(body + placement::kPlayerPosition, feet, sizeof(feet)) || !std::isfinite(feet[0]) || !std::isfinite(feet[1]) ||
                !std::isfinite(feet[2])) {
                InterlockedIncrement(&radarNoBody);
                continue;
            }
            const float dx = feet[0] - out.self[0], dy = feet[1] - out.self[1], dz = feet[2] - out.self[2];
            if (dx * dx + dy * dy + dz * dz > radarRange * radarRange) { InterlockedIncrement(&radarOutOfRange); continue; }
            if (add(feet, kHeartColor, overlay::kMarkHeart)) { ++radarNow; InterlockedIncrement(&radarHearts); }
        }
    }
    if (ghostSelf) {
        if (!out.count) { overlay::HideMarks(); return; }
        overlay::SetMarks(out);
        InterlockedIncrement(&ticksShown);
        return;
    }
    ScanNames(now, player, out.self, out.selfValid);
    for (unsigned i = 0; i < nameMarkCount; ++i) {
        const NameMark& nm = nameMarks[i];
        float feet[3]{};
        if (nm.hidden || !(nm.necro ? IsGhostNecro(nm.player) : nameplate::IsPlayer(nm.player)) ||
            !Read(nm.player + placement::kPlayerPosition, feet, sizeof(feet)) || !std::isfinite(feet[0]) || !std::isfinite(feet[1]) || !std::isfinite(feet[2]))
            continue;
        const float head[3] = {feet[0], feet[1] + (nm.necro ? kNecroHeadLift : kHeadLift), feet[2]};
        if (overlay::Mark* m = add(head, nm.necro ? kNecroNameColor : nm.bot ? kBotNameColor : kNameColor, overlay::kMarkName)) {
            std::snprintf(m->text, sizeof(m->text), "%s", nameMarks[i].name);
            InterlockedIncrement(&namesShown);
        }
    }
    const zone::Shown& z = zone::shown;
    if (enabled && zoneWanted && z.state) {
        // in it, the status line says IN THE ZONE and the ring is around us
        if (out.selfValid && coop_zone::Inside(z.pos, z.radius, out.self)) {
            InterlockedIncrement(&hiddenInside);
        } else {
            const uint32_t color = z.state == coop_zone::Capturing ? kZoneCapturing : z.state == coop_zone::Contested ? kZoneContested
                                 : z.state == coop_zone::Losing    ? kZoneLosing
                                                                   : kZoneEmpty;
            if (overlay::Mark* m = add(z.pos, color, overlay::kMarkZone)) {
                if (z.state == coop_zone::Contested) std::snprintf(m->text, sizeof(m->text), "CONTESTED");
                else if (z.percent > 0) std::snprintf(m->text, sizeof(m->text), "%d %%", z.percent);
            }
        }
    }
    if (enabled && capsulesWanted) {
        AcquireSRWLockShared(&lock);
        const Capsules c = capsules;
        ReleaseSRWLockShared(&lock);
        if (c.at && now - c.at < kCapsuleFreshMs)
            for (int i = 0; i < c.count; ++i) add(c.pos[i], c.golden[i] ? kGoldenCapsule : kCapsule, c.golden[i] ? overlay::kMarkGolden : overlay::kMarkCapsule);
    }
    if (enabled && enemiesWanted) {
        AcquireSRWLockShared(&lock);
        const Enemies e = enemies;
        ReleaseSRWLockShared(&lock);
        if (e.at && now - e.at < kEnemyFreshMs)
            for (int i = 0; i < e.count; ++i)
                if (overlay::Mark* m = add(e.pos[i], kEnemyMark, overlay::kMarkEnemy)) std::snprintf(m->text, sizeof(m->text), "ENEMY");
    }
    if (all && out.selfValid) {
        if (now - allScanAt >= 200) {
            allScanAt = now;
            ScanAllSafe(out.self);
        }
        out.enemyScanAt = allScanAt;
        for (unsigned i = 0; i < allCount; ++i) {
            const auto kind = static_cast<coop_bot::EnemyKind>(allSeen[i].kind);
            const bool boss = kind == coop_bot::EnemyKind::Boss || kind == coop_bot::EnemyKind::Hunter || kind == coop_bot::EnemyKind::Brute;
            if (overlay::Mark* m = add(allSeen[i].pos, boss ? kAllBossColor : kAllEnemyColor, overlay::kMarkDebugEnemy))
                std::snprintf(m->text, sizeof(m->text), "%s %.0f", KindText(kind), allSeen[i].health);
        }
    }
    if (!out.count) { overlay::HideMarks(); return; }
    overlay::SetMarks(out);
    InterlockedIncrement(&ticksShown);
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Marks", "Enabled", 1, ini.c_str()) != 0;
    zoneWanted = GetPrivateProfileIntA("Marks", "Zone", 1, ini.c_str()) != 0;
    capsulesWanted = GetPrivateProfileIntA("Marks", "Capsules", 1, ini.c_str()) != 0;
    enemiesWanted = GetPrivateProfileIntA("Marks", "Enemies", 1, ini.c_str()) != 0;
    InterlockedExchange(&showAll, GetPrivateProfileIntA("Marks", "AllEnemies", 0, ini.c_str()) != 0 ? 1 : 0);
    scalePercent = GetPrivateProfileIntA("Marks", "Scale", 100, ini.c_str());
    namesWanted = GetPrivateProfileIntA("Nameplate", "Overlay", 1, ini.c_str()) != 0;
    radarWanted = GetPrivateProfileIntA("Necro", "Radar", 1, ini.c_str()) != 0;
    {
        const int range = GetPrivateProfileIntA("Necro", "RadarRange", 40, ini.c_str());
        radarRange = static_cast<float>(range < 5 ? 5 : range > 200 ? 200 : range);
        overlay::heartRange = radarRange;
    }
    if (namesWanted && !bot_sense::losVerified) bot_sense::InstallLineOfSight(log);   // an enemy necro's name needs him in sight
    if (scalePercent < 25) scalePercent = 25;
    if (scalePercent > 400) scalePercent = 400;
    if ((enabled || namesWanted || radarWanted) && !overlay::ready) overlay::Install(log, ini);
    std::fprintf(log, "marks %s: zone %s, capsules %s, last enemies %s, names %s, scale %d%%, overlay %s (camera: CameraManager player 0's camera-to-world matrix, the RenderContext's "
                      "horizontal field of view and aspect)\n",
                 enabled ? "on" : "off ([Marks] Enabled=0)", zoneWanted ? "on" : "off", capsulesWanted ? "on" : "off", enemiesWanted ? "on" : "off ([Marks] Enemies=0)",
                 namesWanted ? "on where the game's MP nameplate is not resident" : "off ([Nameplate] Overlay=0)", scalePercent, overlay::ready ? "ready" : "NOT ready");
    std::fprintf(log, "marks heartbeat radar %s, range %.0f m (a necro player, alive or at the vents, sees the humans' hearts through walls)\n",
                 radarWanted ? "on" : "off ([Necro] Radar=0)", radarRange);
}
void WriteDiagnostics(FILE* log) {
    if (radarWanted && InterlockedCompareExchange(&radarTicks, 0, 0)) {
        static LONG lastRadar = -1;
        const LONG radarHundreds = InterlockedCompareExchange(&radarTicks, 0, 0) / 100;
        if (radarHundreds != lastRadar) {
            lastRadar = radarHundreds;
            std::fprintf(log, "marks radar ticks=%ld from_ghost=%ld hearts=%ld out_of_range=%ld no_body=%ld now=%u drawn=%ld\n",
                         InterlockedCompareExchange(&radarTicks, 0, 0), InterlockedCompareExchange(&radarGhostSelf, 0, 0),
                         InterlockedCompareExchange(&radarHearts, 0, 0), InterlockedCompareExchange(&radarOutOfRange, 0, 0),
                         InterlockedCompareExchange(&radarNoBody, 0, 0), radarNow, InterlockedCompareExchange(&overlay::marksHearts, 0, 0));
        }
    }
    if (!enabled && !namesWanted) return;
    static LONG last = -1;
    const LONG sum = InterlockedCompareExchange(&capsuleSets, 0, 0) / 100 + InterlockedCompareExchange(&ticksShown, 0, 0) / 100 +
                     InterlockedCompareExchange(&namesShown, 0, 0) / 100 + InterlockedCompareExchange(&nameScans, 0, 0) / 100 +
                     InterlockedCompareExchange(&hiddenPaused, 0, 0) / 100 + InterlockedCompareExchange(&hiddenScene, 0, 0) / 100 +
                     InterlockedCompareExchange(&hiddenInside, 0, 0) / 100;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "marks enemy_sets=%ld enemies_most=%ld | all enemies %s scans=%ld most_alive=%ld shown_now=%u of %u faults=%ld\n",
                 InterlockedCompareExchange(&enemySets, 0, 0), InterlockedCompareExchange(&enemiesMost, 0, 0), InterlockedCompareExchange(&showAll, 0, 0) ? "on" : "off",
                 InterlockedCompareExchange(&allScans, 0, 0), InterlockedCompareExchange(&allMost, 0, 0), allCount, allTotal, InterlockedCompareExchange(&allFaults, 0, 0));
    std::fprintf(log, "marks ticks_shown=%ld capsule_sets=%ld capsules_most=%ld names_shown=%ld name_scans=%ld named_now=%u necro_names=%ld out_of_sight=%ld hidden paused=%ld scene=%ld "
                      "inside_zone=%ld game_thread=%ld\n",
                 InterlockedCompareExchange(&ticksShown, 0, 0), InterlockedCompareExchange(&capsuleSets, 0, 0), InterlockedCompareExchange(&capsulesMost, 0, 0),
                 InterlockedCompareExchange(&namesShown, 0, 0), InterlockedCompareExchange(&nameScans, 0, 0), nameMarkCount,
                 InterlockedCompareExchange(&necroNames, 0, 0), InterlockedCompareExchange(&necroNamesHidden, 0, 0),
                 InterlockedCompareExchange(&hiddenPaused, 0, 0), InterlockedCompareExchange(&hiddenScene, 0, 0), InterlockedCompareExchange(&hiddenInside, 0, 0),
                 InterlockedCompareExchange(&gameThread, 0, 0));
    std::fprintf(log, "marks necro names skipped: corpses=%ld not_living=%ld duplicates=%ld\n", InterlockedCompareExchange(&necroCorpsesSkipped, 0, 0),
                 InterlockedCompareExchange(&necroNotLiving, 0, 0), InterlockedCompareExchange(&necroDuplicatesSkipped, 0, 0));
}
}  // namespace marks
