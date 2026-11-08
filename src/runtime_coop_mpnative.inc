// Dead Space 2 United by Richard Hafer.
// Versus on the game's own multiplayer systems ([Versus] in DS2United.ini;
// analysis/DS2_MP_INFRASTRUCTURE_2026-10-11.md). The BlazeHub stays null; the
// native systems get their answers about players from our roster instead.
//
// The roster (coop_mpnative.h): every machine sends its own entry (machine,
// name, weapon, suit, XP from its profile) as an 'MPNV' packet every 2 s; the
// host merges them, splits the teams and sends the whole roster back. A player
// is a 64-bit ref (RefFor(machine)), the key every native system uses.
//
// Seams in this file, each with its own byte check:
//   - IStatsManager::HasStats 0x0137DAC0 (thiscall(int64), ret 8) and GetStat
//     0x0137DB00 (thiscall(int64, stat, uint* out), ret 0x10) dereference the
//     BlazeHub without a test (GetPlayerScoreStat 0x00A52CC0 reaches them from
//     the XP nameplate, the killer card, MP upgrades). With no hub they answer
//     from the roster in a Versus visit (stat 0x4F XP, 0x4E skill) and "no
//     stats" otherwise, in every mode.
//   - DGM::GetPlayerTeamIndex(int64) 0x00A3A270 (thiscall, ret 8; 0 human, 1
//     necro, 2 none) and DGM::GetTeamCount(team) 0x00A3A2E0 (thiscall, ret 4)
//     answer from the roster in a Versus visit.
//   - DGM+0xF2, the local team, follows the roster in a Versus visit.
namespace loadout { bool FrontendXp(uint64_t id, uint32_t* xp); }   // runtime_coop_loadout.inc: the character setup's level
namespace mpnative {
namespace mp = coop_mpnative;
bool enabled = true;
unsigned humansWanted = 0;     // [Versus] Humans, 0 = half
std::string profilePath;

// --- profile ---
struct Profile { uint32_t xp = 0; uint8_t weapon = 0, skin = 0; };
Profile profile;
uint32_t ProfileXp() { return profile.xp; }
void LoadProfile() {
    profile.xp = GetPrivateProfileIntA("Profile", "XP", 0, profilePath.c_str());
    profile.weapon = static_cast<uint8_t>(GetPrivateProfileIntA("Profile", "Weapon", 0, profilePath.c_str()));
    profile.skin = static_cast<uint8_t>(GetPrivateProfileIntA("Profile", "Suit", 0, profilePath.c_str()));
}
void SaveProfile() {
    char text[16];
    std::snprintf(text, sizeof(text), "%u", profile.xp);
    WritePrivateProfileStringA("Profile", "XP", text, profilePath.c_str());
    std::snprintf(text, sizeof(text), "%u", profile.weapon);
    WritePrivateProfileStringA("Profile", "Weapon", text, profilePath.c_str());
    std::snprintf(text, sizeof(text), "%u", profile.skin);
    WritePrivateProfileStringA("Profile", "Suit", text, profilePath.c_str());
}

// --- the roster ---
SRWLOCK lock = SRWLOCK_INIT;
mp::Roster roster;
uint8_t roundNo = 1;
uint32_t hostToken = 0;              // whose roster we follow (a client)
DWORD seenAt[256]{};                 // host: last entry per machine
DWORD sentAt = 0, rosterSentAt = 0;
volatile LONG active = 0;            // a Versus visit is on
volatile LONG ownSent = 0, rosterSent = 0, ownIn = 0, rosterIn = 0, refused = 0, teamChanges = 0, localTeamWrites = 0;
uint8_t localTeam = mp::kNoTeam;
unsigned lines = 0;
void Line(const char* format, ...) {
    if (++lines > 400) return;
    char buffer[240];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    vlog::WriteText("roster", buffer);
}

mp::Entry OwnEntry() {
    mp::Entry e{};
    e.machine = static_cast<uint8_t>(machineIndex);
    e.weapon = profile.weapon;
    e.skin = profile.skin;
    e.xp = profile.xp;
    char buffer[24];
    mp::CopyName(e.name, nameplate::OwnName(buffer));
    return e;
}
mp::Packet Make(uint8_t kind) {
    mp::Packet p{};
    p.magic = mp::kMagic;
    p.version = mp::kVersion;
    p.kind = kind;
    p.round = roundNo;
    p.token = peerReadiness.token;
    return p;
}
// Answers for one ref, under the lock. False when the ref is not ours.
bool Lookup(uint64_t ref, mp::Entry& out) {
    const int machine = mp::MachineOfRef(ref);
    if (machine < 0) return false;
    AcquireSRWLockShared(&lock);
    const mp::Entry* e = roster.Find(static_cast<unsigned>(machine));
    if (e) out = *e;
    ReleaseSRWLockShared(&lock);
    return e != nullptr;
}

// Humans on the human side this round, from the host's roster; -1 outside a Versus
// visit. The bots fill that side up to four, the necromorph players take no seat.
int HumanSideHumans() {
    if (!InterlockedCompareExchange(&active, 0, 0)) return -1;
    AcquireSRWLockShared(&lock);
    const unsigned n = roster.TeamCount(mp::kHuman, roundNo);
    ReleaseSRWLockShared(&lock);
    return static_cast<int>(n);
}

// From the side channel's receive loop (any thread of it). True when it was ours.
bool OnBytes(const void* data, int size) {
    if (size != static_cast<int>(sizeof(mp::Packet))) return false;
    mp::Packet p{};
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != mp::kMagic) return false;
    if (p.version != mp::kVersion || !KnownPeerToken(p.token) || p.count > mp::kMaxPlayers) { InterlockedIncrement(&refused); return true; }
    AcquireSRWLockExclusive(&lock);
    if (p.kind == mp::kOwn && IsHost() && p.count == 1) {
        bool changed = false;
        roster.Upsert(p.entries[0], &changed);
        seenAt[p.entries[0].machine] = Stamp(GetTickCount());   // non-zero: 0 means never seen
        if (changed) ++roster.serial;
        InterlockedIncrement(&ownIn);
    } else if (p.kind == mp::kRoster && !IsHost()) {
        if (p.serial != roster.serial || p.round != roundNo) {
            roster = mp::Roster{};
            for (unsigned i = 0; i < p.count; ++i) {
                roster.entries[i] = p.entries[i];
            }
            roster.count = p.count;
            roster.serial = p.serial;
            roundNo = p.round ? p.round : 1;
        }
        hostToken = p.token;
        InterlockedIncrement(&rosterIn);
    }
    ReleaseSRWLockExclusive(&lock);
    return true;
}

// A necromorph player's round starts as the ghost (necro::GhostStart), once the
// visit has arrived; it waits for the necro pool, MPLevelSettings and the host's
// respawn manager, trying once a second.
const char* const kGhostReasons[] = {"ghost up",
                                     "the necro module is off ([Necro] Enabled=0)",
                                     "already a necro or a ghost",
                                     "no Isaac in slot 0 yet",
                                     "no sim or player manager",
                                     "the necro pool is not loaded yet",
                                     "no MPLevelSettings",
                                     "the host's respawn manager has not arrived",
                                     "CreateRespawnProxy signature MISMATCH",
                                     "CreateRespawnProxy faulted",
                                     "no proxy came out"};
DWORD ghostTryAt = 0;
int lastGhostResult = -1;
unsigned proxyFailures = 0;   // CreateRespawnProxy called, no ghost seen: slower, and give up after 6
volatile LONG ghostTries = 0, ghostDone = 0, ghostFaults = 0;
// [Versus] NecroWithoutIsaac: a necro starts the round as the ghost; mpmap spawns
// no Isaac for it (0: an Isaac first, removed when the ghost comes, as before).
bool necroWithoutIsaac = true;
bool InVersus();
bool NecroStart() { return necroWithoutIsaac && necro::enabled && InVersus() && localTeam == mp::kNecro; }
__declspec(noinline) int GhostStartGuarded() {
    __try {
        return necro::GhostStart(necroWithoutIsaac);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&ghostFaults);
        return 9;
    }
}
void PollGhostStart(DWORD now, bool visit) {
    if (!visit || localTeam != mp::kNecro || mpmap::visit.phase != coop_mpmap::Phase::Arrived) { ghostTryAt = 0; lastGhostResult = -1; proxyFailures = 0; return; }
    if (proxyFailures >= 6) return;
    if (necro::playerGone || necro::necroEntity) return;
    // early and often: each try only checks what is there until the host's respawn manager arrives
    if (!ghostTryAt) { ghostTryAt = now + 500; return; }
    if (static_cast<LONG>(now - ghostTryAt) < 0) return;
    ghostTryAt = now + 250;
    InterlockedIncrement(&ghostTries);
    const int result = GhostStartGuarded();
    if (!result) InterlockedIncrement(&ghostDone);
    if (result == 9 || result == 10) {
        ghostTryAt = now + 5000;
        if (++proxyFailures >= 6) Line("ghost start given up after %u proxy attempts: %s", proxyFailures, kGhostReasons[result]);
    }
    if (result != lastGhostResult) {
        lastGhostResult = result;
        Line("ghost start (try %ld): %s", InterlockedCompareExchange(&ghostTries, 0, 0),
             result >= 0 && result < static_cast<int>(std::size(kGhostReasons)) ? kGhostReasons[result] : "?");
    }
}

// Game thread, from Poll.
bool InVersus() { return enabled && InterlockedCompareExchange(&gamemode::mode, 0, 0) == gamemode::Versus; }
void Tick(DWORD now) {
    if (!enabled) return;
    const bool versus = InVersus();
    const bool visit = versus && mpmap::InVisit();
    // our own entry, every machine
    const mp::Entry own = OwnEntry();
    AcquireSRWLockExclusive(&lock);
    bool changed = false;
    if (!network || IsHost()) {
        roster.Upsert(own, &changed);
        seenAt[own.machine] = Stamp(now);
        // a partner silent for 10 s leaves the roster
        for (unsigned i = 0; i < roster.count;) {
            const unsigned m = roster.entries[i].machine;
            if (m != own.machine && (!seenAt[m] || static_cast<LONG>(now - seenAt[m]) > 10000)) { roster.Remove(m); changed = true; continue; }
            ++i;
        }
        // teams are set before a round and kept through it
        if (!InterlockedCompareExchange(&active, 0, 0) && mp::AssignTeams(roster, humansWanted)) {
            changed = true;
            InterlockedIncrement(&teamChanges);
        }
        // the joiners reach the roster only in the level: each gets the side the lobby showed
        if (InterlockedCompareExchange(&active, 0, 0)) {
            unsigned launched[mp::kMaxPlayers] = {0};
            unsigned n = 1;
            for (const auto& member : lobby::table.members)
                if (member.used && n < mp::kMaxPlayers) launched[n++] = member.machine;
            for (unsigned i = 0; i < roster.count; ++i) {
                mp::Entry& e = roster.entries[i];
                if (e.team <= mp::kNecro) continue;
                e.team = mp::LateTeam(launched, n, e.machine, humansWanted, roster.TeamCount(mp::kHuman, 1), roster.TeamCount(mp::kNecro, 1));
                changed = true;
                InterlockedIncrement(&teamChanges);
                Line("machine %u '%s' joined the round late: team %s (%u launched)", e.machine, e.name, e.team == mp::kHuman ? "human" : "necro", n);
            }
        }
        if (changed) ++roster.serial;
    }
    const mp::Roster snapshot = roster;
    ReleaseSRWLockExclusive(&lock);
    // The picked mode counts in the menus and on the MP map, not in a campaign level.
    // and in a Mercenaries visit for the MP suits: the partners' skins come from it
    const bool versusHere = (versus && (!inLevel || mpmap::InVisit())) || mpsuit::Wanted();
    if (network && versusHere && now - sentAt >= 2000) {
        sentAt = now;
        mp::Packet p = Make(mp::kOwn);
        p.count = 1;
        p.entries[0] = own;
        if (!IsHost() && SendToPeers(&p, sizeof(p)) > 0) InterlockedIncrement(&ownSent);
    }
    if (network && versusHere && IsHost() && (changed || now - rosterSentAt >= 2000)) {
        rosterSentAt = now;
        mp::Packet p = Make(mp::kRoster);
        p.count = static_cast<uint8_t>(snapshot.count);
        p.serial = snapshot.serial;
        for (unsigned i = 0; i < snapshot.count; ++i) p.entries[i] = snapshot.entries[i];
        if (SendToPeers(&p, sizeof(p)) > 0) InterlockedIncrement(&rosterSent);
    }
    if (changed) {
        char list[200] = "";
        size_t used = 0;
        for (unsigned i = 0; i < snapshot.count && used < sizeof(list) - 32; ++i)
            used += std::snprintf(list + used, sizeof(list) - used, "%s%u:%s:%s", i ? ", " : "", snapshot.entries[i].machine, snapshot.entries[i].name,
                                  snapshot.entries[i].team == mp::kHuman ? "human" : snapshot.entries[i].team == mp::kNecro ? "necro" : "-");
        Line("roster %u (serial %u, round %u): %s", snapshot.count, snapshot.serial, roundNo, list);
    }

    // the visit itself; MPLevelSettings spawns in it, so the native name VFX stays off
    // (it would be built without its text, 0x0086AEDE)
    const LONG was = InterlockedExchange(&active, visit ? 1 : 0);
    InterlockedExchange(&vlog::matchOn, visit ? 1 : 0);
    if (visit) necro::KeepNameplatesOff();
    // every machine loads the necro pool for the visit ([Necro] LoadPool=2 waits for a demand):
    // the necro players need it to turn, the humans to see them
    if (visit && necro::enabled && necro::poolMode == 2 && !necro::poolDemanded && mpmap::visit.phase == coop_mpmap::Phase::Arrived) {
        necro::Demand(false);
        Line("necro pool demanded for the Versus visit");
    }
    if (visit != (was != 0)) Line("%s", visit ? "visit on: the native MP seams answer from the roster" : "visit over: the seams are back to the game's own answers");
    uint32_t dgm = 0;
    if (!Read(base + menu::kGameManager, &dgm, 4) || !dgm) return;
    const mp::Entry* me = snapshot.Find(machineIndex);
    const uint8_t wanted = visit && me ? mp::EffectiveTeam(me->team, roundNo) : mp::kHuman;
    uint8_t current = 0;
    if (Read(dgm + 0xF2, &current, 1) && current != wanted && (visit || was) && wanted <= mp::kNecro) {
        if (WriteBytes(dgm + 0xF2, &wanted, 1)) {
            InterlockedIncrement(&localTeamWrites);
            Line("local team %u -> %u (DGM+0xF2)", current, wanted);
        }
    }
    localTeam = wanted;
    PollGhostStart(now, visit);
}

// --- seams ---
constexpr uint32_t kOnlineServicesRva = 0x1C213DC, kHubOffset = 0x94;
constexpr uint32_t kHasStatsRva = 0xF7DAC0, kGetStatRva = 0xF7DB00, kTeamIndexRva = 0x63A270, kTeamCountRva = 0x63A2E0;
constexpr int kStatXp = 0x4F, kStatSkill = 0x4E;
safetyhook::InlineHook hasStatsHook, getStatHook, teamIndexHook, teamCountHook;
volatile LONG statAsks = 0, statAnswered = 0, statRefused = 0, teamAsks = 0, teamAnswered = 0, countAsks = 0;
bool HubPresent() {
    uint32_t services = 0, hub = 0;
    return Read(base + kOnlineServicesRva, &services, 4) && services && Read(services + kHubOffset, &hub, 4) && hub;
}
bool __fastcall OnHasStats(void* self, void*, uint32_t lo, uint32_t hi) {
    if (HubPresent()) return hasStatsHook.thiscall<bool>(self, lo, hi);
    InterlockedIncrement(&statAsks);
    uint32_t xp = 0;
    if (loadout::FrontendXp((static_cast<uint64_t>(hi) << 32) | lo, &xp)) { InterlockedIncrement(&statAnswered); return true; }
    mp::Entry e{};
    const bool known = InterlockedCompareExchange(&active, 0, 0) && Lookup((static_cast<uint64_t>(hi) << 32) | lo, e);
    InterlockedIncrement(known ? &statAnswered : &statRefused);
    return known;
}
bool __fastcall OnGetStat(void* self, void*, uint32_t lo, uint32_t hi, int stat, uint32_t* out) {
    if (HubPresent()) return getStatHook.thiscall<bool>(self, lo, hi, stat, out);
    InterlockedIncrement(&statAsks);
    uint32_t xp = 0;
    if (out && (stat == kStatXp || stat == kStatSkill) && loadout::FrontendXp((static_cast<uint64_t>(hi) << 32) | lo, &xp)) {
        *out = stat == kStatXp ? xp : 0u;
        InterlockedIncrement(&statAnswered);
        return true;
    }
    mp::Entry e{};
    if (!out || !InterlockedCompareExchange(&active, 0, 0) || !Lookup((static_cast<uint64_t>(hi) << 32) | lo, e) || (stat != kStatXp && stat != kStatSkill)) {
        InterlockedIncrement(&statRefused);
        return false;
    }
    *out = stat == kStatXp ? e.xp : 0u;
    InterlockedIncrement(&statAnswered);
    return true;
}
uint8_t __fastcall OnTeamIndex(void* self, void*, uint32_t lo, uint32_t hi) {
    if (!InterlockedCompareExchange(&active, 0, 0)) return teamIndexHook.thiscall<uint8_t>(self, lo, hi);
    InterlockedIncrement(&teamAsks);
    mp::Entry e{};
    if (!Lookup((static_cast<uint64_t>(hi) << 32) | lo, e)) return mp::kNoTeam;
    InterlockedIncrement(&teamAnswered);
    return mp::EffectiveTeam(e.team, roundNo);
}
uint8_t __fastcall OnTeamCount(void* self, void*, uint32_t team) {
    if (!InterlockedCompareExchange(&active, 0, 0)) return teamCountHook.thiscall<uint8_t>(self, team);
    InterlockedIncrement(&countAsks);
    AcquireSRWLockShared(&lock);
    const unsigned n = roster.TeamCount(static_cast<uint8_t>(team), roundNo);
    ReleaseSRWLockShared(&lock);
    return static_cast<uint8_t>(n);
}

bool HookAt(FILE* log, const char* name, uint32_t rva, const unsigned char* bytes, size_t size, void* fn, safetyhook::InlineHook& hook) {
    return hook_site::Install(log, name, base + rva, bytes, size, fn, hook);
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Versus", "Enabled", 1, ini.c_str()) != 0;
    humansWanted = GetPrivateProfileIntA("Versus", "Humans", 0, ini.c_str());
    if (humansWanted > mp::kMaxPlayers) humansWanted = 0;
    const size_t slash = ini.find_last_of("\\/");
    profilePath = (slash == std::string::npos ? std::string() : ini.substr(0, slash + 1)) + "DS2Profile.ini";
    LoadProfile();
    if (enabled) mpmap::versusWanted = InVersus;
    necroWithoutIsaac = GetPrivateProfileIntA("Versus", "NecroWithoutIsaac", 1, ini.c_str()) != 0;
    if (enabled) mpmap::necroStart = NecroStart;
    if (enabled && !necro::enabled) std::fprintf(log, "versus: the necro module is off ([Necro] Enabled=0): the necro team spawns as Isaac\n");
    // mov eax,[OnlineServices] / mov ecx,[eax+94h] / mov edx,[esp+8] (both stats getters start alike)
    unsigned char stats[15] = {0xA1, 0, 0, 0, 0, 0x8B, 0x88, 0x94, 0x00, 0x00, 0x00, 0x8B, 0x54, 0x24, 0x08};
    const uint32_t services = static_cast<uint32_t>(base + kOnlineServicesRva);
    std::memcpy(stats + 1, &services, 4);
    // sub esp,8 / push esi / lea eax,[esp+4] / push eax / mov esi,ecx
    static const unsigned char kTeamIndex[] = {0x83, 0xEC, 0x08, 0x56, 0x8D, 0x44, 0x24, 0x04, 0x50, 0x8B, 0xF1};
    // sub esp,8 / push ebx / push ebp / lea eax,[esp+8] / push eax / mov ebp,ecx / xor bl,bl
    static const unsigned char kTeamCount[] = {0x83, 0xEC, 0x08, 0x53, 0x55, 0x8D, 0x44, 0x24, 0x08, 0x50, 0x8B, 0xE9, 0x32, 0xDB};
    // The stats guard closes a crash in every mode, so it goes in even with Versus off.
    const bool hasOk = HookAt(log, "versus IStatsManager::HasStats", kHasStatsRva, stats, sizeof(stats), reinterpret_cast<void*>(OnHasStats), hasStatsHook);
    const bool getOk = HookAt(log, "versus IStatsManager::GetStat", kGetStatRva, stats, sizeof(stats), reinterpret_cast<void*>(OnGetStat), getStatHook);
    bool teamOk = false, countOk = false;
    if (enabled) {
        teamOk = HookAt(log, "versus DGM::GetPlayerTeamIndex(int64)", kTeamIndexRva, kTeamIndex, sizeof(kTeamIndex), reinterpret_cast<void*>(OnTeamIndex), teamIndexHook);
        countOk = HookAt(log, "versus DGM::GetTeamCount", kTeamCountRva, kTeamCount, sizeof(kTeamCount), reinterpret_cast<void*>(OnTeamCount), teamCountHook);
    }
    std::fprintf(log, "versus %s: humans %u (0 = half), profile %s (XP %u, level %u, weapon %u, suit %u); stats guard %s/%s, team index %s, team count %s\n",
                 enabled ? "on" : "off ([Versus] Enabled=0)", humansWanted, profilePath.c_str(), profile.xp, mp::LevelForXp(profile.xp), profile.weapon,
                 profile.skin, hasOk ? "in" : "MISSING", getOk ? "in" : "MISSING", teamOk ? "in" : enabled ? "MISSING" : "off", countOk ? "in" : enabled ? "MISSING" : "off");
}
void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG sum = InterlockedCompareExchange(&ownIn, 0, 0) / 5 + InterlockedCompareExchange(&rosterIn, 0, 0) / 5 + InterlockedCompareExchange(&teamChanges, 0, 0) * 3 +
                     InterlockedCompareExchange(&statAsks, 0, 0) + InterlockedCompareExchange(&teamAsks, 0, 0) + InterlockedCompareExchange(&countAsks, 0, 0) +
                     InterlockedCompareExchange(&localTeamWrites, 0, 0) * 7 + InterlockedCompareExchange(&active, 0, 0) * 11;
    if (sum == last || !sum) return;
    last = sum;
    AcquireSRWLockShared(&lock);
    const unsigned count = roster.count, humans = roster.TeamCount(mp::kHuman, roundNo), necros = roster.TeamCount(mp::kNecro, roundNo);
    const uint32_t serial = roster.serial;
    ReleaseSRWLockShared(&lock);
    std::fprintf(log, "versus active=%ld round=%u roster=%u (serial %u, humans %u, necros %u) local_team=%u own sent=%ld in=%ld roster sent=%ld in=%ld refused=%ld "
                      "team_changes=%ld team_writes=%ld stats asks=%ld answered=%ld refused=%ld team asks=%ld answered=%ld count asks=%ld\n",
                 InterlockedCompareExchange(&active, 0, 0), roundNo, count, serial, humans, necros, localTeam, InterlockedCompareExchange(&ownSent, 0, 0),
                 InterlockedCompareExchange(&ownIn, 0, 0), InterlockedCompareExchange(&rosterSent, 0, 0), InterlockedCompareExchange(&rosterIn, 0, 0),
                 InterlockedCompareExchange(&refused, 0, 0), InterlockedCompareExchange(&teamChanges, 0, 0), InterlockedCompareExchange(&localTeamWrites, 0, 0),
                 InterlockedCompareExchange(&statAsks, 0, 0), InterlockedCompareExchange(&statAnswered, 0, 0), InterlockedCompareExchange(&statRefused, 0, 0),
                 InterlockedCompareExchange(&teamAsks, 0, 0), InterlockedCompareExchange(&teamAnswered, 0, 0), InterlockedCompareExchange(&countAsks, 0, 0));
    if (InterlockedCompareExchange(&ghostTries, 0, 0))
        std::fprintf(log, "versus ghost starts tries=%ld done=%ld faults=%ld last='%s' necro ghost_starts=%ld\n", InterlockedCompareExchange(&ghostTries, 0, 0),
                     InterlockedCompareExchange(&ghostDone, 0, 0), InterlockedCompareExchange(&ghostFaults, 0, 0),
                     lastGhostResult >= 0 && lastGhostResult < static_cast<int>(std::size(kGhostReasons)) ? kGhostReasons[lastGhostResult] : "-",
                     InterlockedCompareExchange(&necro::ghostStarts, 0, 0));
}
}  // namespace mpnative
