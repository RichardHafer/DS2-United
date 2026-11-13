// Dead Space 2 United by Richard Hafer.
#pragma once
// Pure logic of the survival director (runtime_survival.inc): the configuration
// file, wave plans and their endless growth, family roles and substitution, the
// spawn queue, pacing, spawn point rules and scoring, the stuck verdict, the
// reward split and the best-wave file. No game memory is touched here.
#include "survival_rewards.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace survival {

inline uint32_t DeadlineAfter(uint32_t now, uint32_t delay) {
    const uint32_t end = now + delay;
    return end ? end : 1;
}
inline bool DeadlinePending(uint32_t now, uint32_t deadline) {
    return deadline && static_cast<int32_t>(deadline - now) > 0;
}

template <typename Reader>
bool MatchesEntity(uint32_t entity, uint32_t expectedVtable, const std::array<uint32_t, 4>& expectedGuid, uint32_t guidOffset, Reader read) {
    uint32_t vtable = 0;
    std::array<uint32_t, 4> guid{};
    return entity && expectedVtable && expectedGuid != guid &&
           read(entity, &vtable, sizeof(vtable)) && vtable == expectedVtable &&
           read(entity + guidOffset, guid.data(), sizeof(guid)) && guid == expectedGuid;
}

// --- text helpers ---
inline char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
inline std::string LowerText(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = Lower(c);
    return out;
}
inline bool EqualNoCase(const std::string& a, const std::string& b) { return LowerText(a) == LowerText(b); }
inline std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}
inline bool ParseUnsigned(const std::string& text, unsigned& out) {
    const std::string t = Trim(text);
    if (t.empty() || t.size() > 9) return false;
    for (char c : t) if (c < '0' || c > '9') return false;
    out = static_cast<unsigned>(std::strtoul(t.c_str(), nullptr, 10));
    return true;
}
inline bool ParseFloat(const std::string& text, float& out) {
    const std::string t = Trim(text);
    if (t.empty()) return false;
    char* end = nullptr;
    const float value = std::strtof(t.c_str(), &end);
    if (!end || *end || !std::isfinite(value)) return false;
    out = value;
    return true;
}
// "a, b,,c" -> {"a", "b", "c"}
inline std::vector<std::string> SplitList(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find(',', at);
        if (end == std::string::npos) end = text.size();
        const std::string part = Trim(text.substr(at, end - at));
        if (!part.empty()) out.push_back(part);
        at = end + 1;
    }
    return out;
}

// --- a small INI reader ---
// Sections and keys case-insensitive, `;` and `#` start a comment line, a UTF-8
// BOM is skipped. The game's GetPrivateProfile* would do the same, but this one
// can be tested and reads the whole file in one go.
using Section = std::map<std::string, std::string>;
using Ini = std::map<std::string, Section>;
inline Ini ParseIni(const std::string& text) {
    Ini ini;
    std::string current;
    size_t at = text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF ? 3 : 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = Trim(text.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            current = LowerText(Trim(line.substr(1, line.size() - 2)));
            ini[current];
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        ini[current][LowerText(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
    }
    return ini;
}
inline const std::string* Value(const Ini& ini, const std::string& section, const std::string& key) {
    const auto s = ini.find(LowerText(section));
    if (s == ini.end()) return nullptr;
    const auto k = s->second.find(LowerText(key));
    return k == s->second.end() ? nullptr : &k->second;
}

// --- families ---
// What a family does in a fight. Substitution keeps the role where it can.
enum class Role : uint8_t { Swarm, Ranged, Explosive, Heavy, Rusher, Special, Boss, Unknown };
inline const char* RoleName(Role r) {
    switch (r) {
    case Role::Swarm: return "swarm";
    case Role::Ranged: return "ranged";
    case Role::Explosive: return "explosive";
    case Role::Heavy: return "heavy";
    case Role::Rusher: return "rusher";
    case Role::Special: return "special";
    case Role::Boss: return "boss";
    default: return "unknown";
    }
}
struct FamilyInfo { const char* family; Role role; const char* fallbacks[4]; };
// The families a wave may ask for, with their stand-ins in order.
inline constexpr FamilyInfo kFamilies[] = {
    {"Zombie", Role::Swarm, {"PackEnemy", "Leaper", "Infector", nullptr}},
    {"PackEnemy", Role::Swarm, {"Zombie", "SwarmEnemy", "Leaper", nullptr}},
    {"SwarmEnemy", Role::Swarm, {"PackEnemy", "Zombie", nullptr, nullptr}},
    {"Leaper", Role::Rusher, {"Stalker", "Zombie", "PackEnemy", nullptr}},
    {"Stalker", Role::Rusher, {"Leaper", "Zombie", nullptr, nullptr}},
    {"Puker", Role::Ranged, {"Lurker", "Exploder", "Zombie", nullptr}},
    {"Lurker", Role::Ranged, {"Puker", "Leaper", "Zombie", nullptr}},
    {"Exploder", Role::Explosive, {"Puker", "Zombie", nullptr, nullptr}},
    {"Infector", Role::Special, {"TubeBeetle", "Leaper", "Zombie", nullptr}},
    {"TubeBeetle", Role::Special, {"Infector", "PackEnemy", "Zombie", nullptr}},
    {"Brute", Role::Heavy, {"DividerCore", "Pregnant", "Stalker", "Zombie"}},
    {"Pregnant", Role::Heavy, {"Brute", "Puker", "Zombie", nullptr}},
    {"DividerCore", Role::Heavy, {"Brute", "Pregnant", "Zombie", nullptr}},
    {"Hunter", Role::Boss, {"Brute", "DividerCore", "Stalker", "Zombie"}},
};
inline const FamilyInfo* Info(const std::string& family) {
    for (const auto& f : kFamilies) if (EqualNoCase(family, f.family)) return &f;
    return nullptr;
}
inline Role RoleOf(const std::string& family) {
    const FamilyInfo* f = Info(family);
    return f ? f->role : Role::Unknown;
}
// The names a player would use for a family, besides the game's own.
inline std::string FamilyAlias(const std::string& text) {
    static const char* const kAliases[][2] = {{"slasher", "Zombie"}, {"slashers", "Zombie"}, {"pack", "PackEnemy"}, {"packs", "PackEnemy"},
                                              {"swarmer", "SwarmEnemy"}, {"swarmers", "SwarmEnemy"}, {"leapers", "Leaper"}, {"stalkers", "Stalker"},
                                              {"pukers", "Puker"}, {"lurkers", "Lurker"}, {"exploders", "Exploder"}, {"infectors", "Infector"},
                                              {"tube", "TubeBeetle"}, {"tubebeetle", "TubeBeetle"}, {"crawler", "TubeBeetle"}, {"brutes", "Brute"},
                                              {"divider", "DividerCore"}, {"dividers", "DividerCore"}, {"ubermorph", "Hunter"}};
    const std::string t = LowerText(Trim(text));
    for (const auto& a : kAliases) if (t == a[0]) return a[1];
    return t;
}
// Bosses and fixed set pieces never take part, whatever a wave line asks for.
inline bool Excluded(const std::string& family) {
    static const char* const kNever[] = {"Guardian", "GuardianPod", "GuardianTentacle", "Nest", "NestProjectile", "Tripod",
                                         "TripodProjectile", "BossTripod", "Kong", "NicoleBoss", "DragTentacle", "DividerPart",
                                         "Landmine"};
    for (const char* n : kNever) if (EqualNoCase(family, n)) return true;
    return false;
}
// The Ubermorph regenerates without limit: a wave holding one ends on its timer.
inline bool Unkillable(const std::string& family) { return EqualNoCase(family, "Hunter"); }
// The family's canonical spelling, or empty when the name is unknown.
inline std::string CanonicalFamily(const std::string& family) {
    const FamilyInfo* f = Info(Trim(family));
    return f ? f->family : std::string();
}

struct Substitution {
    std::string family;     // empty: nothing available
    unsigned factor = 1;    // a heavy replaced by something lighter comes three times
    bool substituted = false;
};
// The family itself when available, else the first available stand-in.
inline Substitution Substitute(const std::string& family, const std::set<std::string>& available) {
    Substitution out;
    if (available.count(family)) { out.family = family; return out; }
    const FamilyInfo* f = Info(family);
    if (!f) return out;
    for (const char* stand : f->fallbacks) {
        if (!stand || !available.count(stand)) continue;
        out.family = stand;
        out.substituted = true;
        const Role from = f->role, to = RoleOf(stand);
        const bool heavyFrom = from == Role::Heavy || from == Role::Boss;
        const bool heavyTo = to == Role::Heavy || to == Role::Boss;
        out.factor = heavyFrom && !heavyTo ? 3u : 1u;
        return out;
    }
    // last resort: any available swarm family
    for (const auto& g : kFamilies) {
        if (g.role != Role::Swarm || !available.count(g.family)) continue;
        out.family = g.family;
        out.substituted = true;
        out.factor = f->role == Role::Heavy || f->role == Role::Boss ? 3u : 1u;
        return out;
    }
    return out;
}

// --- waves ---
enum class Path : uint8_t { Any, Vent, Direct };
inline const char* PathName(Path p) { return p == Path::Vent ? "vent" : p == Path::Direct ? "direct" : "any"; }
inline bool ParsePath(const std::string& text, Path& out) {
    const std::string t = LowerText(Trim(text));
    if (t.empty() || t == "any") { out = Path::Any; return true; }
    if (t == "vent") { out = Path::Vent; return true; }
    if (t == "direct") { out = Path::Direct; return true; }
    return false;
}

constexpr unsigned kMaxEntryCount = 60, kMaxGroup = 6;
struct Entry {
    std::string family;
    unsigned count = 0;
    Path path = Path::Any;
    unsigned group = 1;   // spawned together at one spot (packs)
};
struct Wave {
    std::vector<Entry> entries;
    unsigned maxAlive = 0;      // 0: the global cap
    unsigned intervalMs = 0;    // 0: the global pacing
    unsigned timeLimitS = 0;    // 0: the wave ends when its enemies are dead
    unsigned pauseS = 0;        // 0: the global pause after it
    int enhanced = -1;          // per cent of enhanced variants; -1: the escalation's
    bool boss = false;          // carries an unkillable family
    unsigned Total() const {
        unsigned n = 0;
        for (const auto& e : entries) n += e.count;
        return n;
    }
};

// "Zombie:6, PackEnemy:4:vent, Stalker:3:direct:3". False on a malformed item.
inline bool ParseEntries(const std::string& text, std::vector<Entry>& out, std::string* error) {
    out.clear();
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find(',', at);
        if (end == std::string::npos) end = text.size();
        const std::string item = Trim(text.substr(at, end - at));
        at = end + 1;
        if (item.empty()) { if (end >= text.size()) break; continue; }
        std::vector<std::string> parts;
        size_t p = 0;
        while (true) {
            const size_t colon = item.find(':', p);
            parts.push_back(Trim(item.substr(p, colon == std::string::npos ? std::string::npos : colon - p)));
            if (colon == std::string::npos) break;
            p = colon + 1;
        }
        Entry e;
        if (Excluded(parts[0])) {
            if (error) *error = "family '" + parts[0] + "' is a boss or set piece and never spawns";
            return false;
        }
        e.family = CanonicalFamily(parts[0]);
        if (e.family.empty()) {
            if (error) *error = "unknown family '" + parts[0] + "'";
            return false;
        }
        if (parts.size() < 2 || !ParseUnsigned(parts[1], e.count) || !e.count || e.count > kMaxEntryCount) {
            if (error) *error = "count of '" + item + "' must be 1.." + std::to_string(kMaxEntryCount);
            return false;
        }
        if (parts.size() >= 3 && !ParsePath(parts[2], e.path)) {
            if (error) *error = "path of '" + item + "' must be any, vent or direct";
            return false;
        }
        if (parts.size() >= 4 && (!ParseUnsigned(parts[3], e.group) || !e.group || e.group > kMaxGroup)) {
            if (error) *error = "group of '" + item + "' must be 1.." + std::to_string(kMaxGroup);
            return false;
        }
        if (parts.size() > 4) {
            if (error) *error = "too many fields in '" + item + "'";
            return false;
        }
        out.push_back(e);
        if (end >= text.size()) break;
    }
    if (out.empty()) {
        if (error) *error = "no enemies";
        return false;
    }
    return true;
}

// --- configuration ---
struct Config {
    bool enabled = false;
    std::string map = "mp_bombRun";
    unsigned startDelayS = 20, startKey = 0;
    unsigned maxAlive = 15, intervalMs = 1500, waveStartDelayMs = 2500, pauseS = 20;
    unsigned gameOverS = 10;    // every player down: the game over shows this long, then the next run arms
    unsigned restartOnGameOver = 1;   // then the map reloads from its checkpoint (MP maps), else the next run arms on it
    float minDistance = 12.0f, botMinDistance = 8.0f, maxDistance = 25.0f, idealDistance = 16.0f, floorBand = 12.0f, maxPathFactor = 2.2f, otherFloorBonus = 2.0f;
    float wallClearance = 0.3f;   // a direct spawn keeps this much room from walls (0 = off)
    unsigned candidatesPerTick = 3;
    bool ventSpawns = true, ventLineOfSight = false, ventCrossFamily = true, ventIgnoreFamilyOff = true;
    // From the nearest player or bot. VentMinDistance holds for one of them, every further living
    // player or bot takes VentMinStep off (user, 13.11.: 5 m solo, 2 m less per player or bot).
    float ventMinDistance = 5.0f, ventMinStep = 2.0f, ventMaxDistance = 30.0f;
    bool ventAnyMarker = true;   // every open VentMarker of the map, not only the five slasher spawners' own
    float ventMaxAbove = 2.5f;   // a wall vent this far above the floor below it is the necro players'; 0 = no limit
    bool ventMustReach = true;   // only vents whose floor the living can walk to (the nav mesh decides)
    bool stalkerNeedsCover = true;   // no Stalker on a map without CoverNodes (it only stands there)
    float catchUpMeters = 50.0f;   // an enemy this far from every living player is queued again near them; 0 = off
    float catchUpMetersFour = 40.0f;   // the same with four real players (linear per player, bots do not count)
    unsigned catchUpMs = 0;        // for this long first; 0 = at once
    // [Cleanup] on the MP maps: what would pile up in a long run
    bool cleanupKills = true, cleanupFades = true, pickupCap = true;
    unsigned cleanupDelayMs = 3000, fadeStallMs = 3000, maxPickups = 40, pickupHardMax = 80, pickupMinAgeS = 60;
    float pickupKeepRadius = 8.0f, killKeepRadius = 10.0f;
    unsigned ventSharePercent = 40, ventGapMs = 3500, ventWaitMs = 15000;
    unsigned stuckS = 60, ventStuckS = 30, inertS = 20;
    unsigned frozenS = 15;         // not a metre moved for this long: taken away and queued again (0 = off)
    bool forceAware = true, repairBlindTemplates = true, zombieChargeIgnoreVis = false;
    bool noPlayDead = true;            // no survival enemy lies down to play dead
    unsigned chargeUnseen = 0;         // charge without sight: 0 off, 1 in campaign arenas, 2 everywhere
    unsigned awareRefreshMs = 3000, maxChargers = 2;
    // Tracking range for the run: most templates track the players to 30 m only, beyond that
    // the engine drops them and forced awareness has nobody to point at (0 = leave it).
    float trackMeters = 80.0f;
    float zombieChargeFloor = 0.9f;
    float engageMeters = 10.0f;
    bool timedEndKill = true, superVariants = false;
    unsigned twitcherPercent = 20;     // a slasher comes as a Severed twitcher (0 = never)
    unsigned pregnantPercent = 20;     // a pregnant comes as the Severed pregnant (0 = never)
    unsigned navReserveKiB = 96;
    unsigned rewardAmmoPerGun = 2, rewardMedPacks = 1, rewardMedSize = 1, rewardCredits = 1000, rewardStasis = 1;
    unsigned endlessFrom = 4, endlessTo = 9, endlessGrowthPercent = 20, bossEvery = 10;
    bool notices = true;
    bool hud = true;            // the status line on the co-op overlay instead of RIG notices
    unsigned noticeGapMs = 4000;
    unsigned showLastEnemies = 3;   // this many left (nothing more to spawn): marked on screen
    unsigned logSpawnLines = 300;
    // [Escalation]: from FromWave on every wave hardens the run (the boss fight of
    // wave 10 is the last normal one). Percentages per wave past FromWave - 1.
    unsigned escFromWave = 11;
    unsigned escEnhancedPerWave = 5;                         // chance an enemy with an enhanced variant comes as one
    unsigned escEnhancedMax = 60;                            // ... at most (20.10.: from wave 21 every slasher was an oracle)
    unsigned escHealthPerWave = 3;                           // health and limb health
    unsigned escHealthMax = 60;                              // ... at most (20.10.: 623 health on a wave 23 oracle, 22 a hit)
    unsigned escOracleShare = 35, escOracleMaxAlive = 3;     // of the enhanced slashers, oracle twitchers at most this share and this many alive
    // Mercenaries, Objective and Zone add a stage whenever their queue runs dry; this many
    // stages count as one wave for the escalation, and their enhanced chance stays below
    // ClockEnhancedMax, the boss stage's own Enhanced= included (26.10.: oracles and super
    // slashers everywhere after a few minutes)
    unsigned escClockStages = 3, escClockEnhancedMax = 25;
    unsigned escSpeedPerWave = 1, escSpeedMax = 40;          // animation playback, capped
    float escPauseStepS = 0.5f;                              // the break between waves shrinks (0.5: a second every two waves)
    unsigned escPauseMinS = 8;
    unsigned escMaxAlivePerPlayer = 3, escMaxAliveCap = 23;  // MaxAlive is for one player; each more (bots too) adds
    float escRewardStep = 2.5f;                              // wave rewards shrink by this many percent a step ...
    unsigned escRewardMin = 40;                              // ... to this percent
    unsigned escPriceStep = 0, escPriceMax = 200;            // shop prices grow to this percent (0: never, user 20.10.)
    unsigned escBotDamageStep = 1, escBotDamageMin = 50;     // what the companion bots' hits do
    unsigned escBossStasisPerBoss = 20, escBossStasisMax = 80;   // stasis resistance of Ubermorph and Brute per boss wave reached
    std::vector<Wave> waves;
    // [Mercenaries]: the clock mode (picked in the map picker, see DS2United.ini [Menu]).
    struct Mercs {
        unsigned startSeconds = 150, startDelayS = 3, maxSeconds = 600;
        unsigned killMs = 1000, eliteKillMs = 5000, comboWindowMs = 3000, comboStep = 10, comboBonusMs = 10000;
        unsigned capsules = 2, capsuleMs = 30000, goldenMs = 60000, goldenPercent = 15, capsuleLifeS = 75, capsuleGapM = 12, capsuleCheckMs = 2000;
        unsigned capsuleTotal = 8, capsuleMinM = 20;              // per run; metres from every player
        unsigned fadeS = 480, fadeFloorPercent = 20, maxRunS = 900;   // time bonuses shrink; the run's hard end
        unsigned deathPenaltyMs = 15000, killTarget = 150, clearBonusPerSecond = 100, supplyKills = 30, resultS = 20, finalSeconds = 60, restart = 1;
        std::vector<Entry> finalEnemies;
        unsigned ranks[5] = {20000, 40000, 70000, 100000, 150000};   // C, B, A, S, SS
    } mercs;
    // [Objective]: the MP maps' own objectives against the hordes, on a clock.
    struct Objective {
        unsigned startDelayS = 3, startPercent = 100, bonusPercent = 100, stepBonusS = 30, stepFloorS = 90, maxSeconds = 1800;
        unsigned deathPenaltyMs = 10000, breatherS = 0, supplies = 1, stageStep = 1, resultS = 20, restart = 1;
        unsigned defendDps = 12, defendRadiusM = 4;   // what each enemy near a vulnerable defend target does to it
    } objective;
    // [Zone]: Hold the Zone, a zone that moves over the MP map on a clock.
    struct Zone {
        unsigned startDelayS = 3, startSeconds = 150, maxSeconds = 600, bonusS = 60, deathPenaltyMs = 10000;
        float radiusM = 4.5f;
        unsigned captureS = 30, teamSpeedPercent = 50, decayPercent = 50, contestEnemies = 2;
        unsigned supplies = 1, stageStep = 1, targetZones = 0, resultS = 20, restart = 1;
        unsigned fadeS = 600, fadeFloorPercent = 20, maxRunS = 720;
    } zone;
    // [VersusSlashers] and [VersusSkins]: the AI slashers of a Versus round
    // (DS2Coop_GameMode 4) in place of the map's own five, with the map's skins
    // from the pool, and super slashers while the humans dominate.
    struct Versus {
        bool enabled = true;
        unsigned alive = 5, firstSpawnS = 10, respawnMinS = 25, respawnMaxS = 40;
        unsigned health = 80, superHealth = 200;   // 0 = the donor's own
        unsigned fastObjectiveS = 45, killLead = 4, decayS = 60, maxLevel = 3;
        unsigned superPercentPerLevel = 34, extraAlivePerLevel = 1, superBurst = 1;
        bool retargetFromNecros = true;   // a slasher that goes for a NecroPlayer: its template tracks the human team
        std::map<std::string, unsigned> aliveFor;                  // lower-case map -> its own count
        std::map<std::string, std::vector<std::string>> skins;     // lower-case map -> model packages
        std::vector<std::string> defaultSkins{"slasher"};
    } versus;
    // [MapFamilies] <map>=<families>: only these families on that map, or with a leading '-'
    // every family but these (user 20.10.). Lower-case map name -> rule.
    struct MapRule { std::set<std::string> only, without; };
    std::map<std::string, MapRule> mapFamilies;
    std::vector<std::string> warnings;
};

inline unsigned UnsignedOr(const Ini& ini, const char* section, const char* key, unsigned fallback, unsigned lo, unsigned hi, Config& c) {
    const std::string* v = Value(ini, section, key);
    unsigned out = fallback;
    if (!v || v->empty()) return fallback;
    if (!ParseUnsigned(*v, out)) { c.warnings.push_back(std::string("[") + section + "] " + key + "='" + *v + "' is not a number; " + std::to_string(fallback) + " used"); return fallback; }
    if (out < lo || out > hi) {
        const unsigned clamped = out < lo ? lo : hi;
        c.warnings.push_back(std::string("[") + section + "] " + key + "=" + std::to_string(out) + " outside " + std::to_string(lo) + ".." + std::to_string(hi) + "; " + std::to_string(clamped) + " used");
        return clamped;
    }
    return out;
}
inline float FloatOr(const Ini& ini, const char* section, const char* key, float fallback, float lo, float hi, Config& c) {
    const std::string* v = Value(ini, section, key);
    float out = fallback;
    if (!v || v->empty()) return fallback;
    if (!ParseFloat(*v, out)) { c.warnings.push_back(std::string("[") + section + "] " + key + "='" + *v + "' is not a number"); return fallback; }
    return out < lo ? lo : out > hi ? hi : out;
}

constexpr unsigned kMaxWaves = 50;
inline Config ParseConfig(const std::string& text) {
    Config c;
    const Ini ini = ParseIni(text);
    const char* s = "Survival";
    c.enabled = UnsignedOr(ini, s, "Enabled", 0, 0, 1, c) != 0;
    if (const std::string* map = Value(ini, s, "Map")) if (!map->empty()) c.map = *map;
    c.startDelayS = UnsignedOr(ini, s, "StartDelaySeconds", 20, 0, 600, c);
    c.startKey = UnsignedOr(ini, s, "StartKey", 0, 0, 255, c);
    c.maxAlive = UnsignedOr(ini, s, "MaxAlive", 15, 1, 30, c);
    c.intervalMs = UnsignedOr(ini, s, "SpawnIntervalMs", 1500, 250, 60000, c);
    c.waveStartDelayMs = UnsignedOr(ini, s, "WaveStartDelayMs", 2500, 0, 60000, c);
    c.pauseS = UnsignedOr(ini, s, "PauseSeconds", 20, 0, 600, c);
    c.gameOverS = UnsignedOr(ini, s, "GameOverSeconds", 10, 3, 120, c);
    c.restartOnGameOver = UnsignedOr(ini, s, "RestartOnGameOver", 1, 0, 1, c);
    c.minDistance = FloatOr(ini, s, "MinDistance", 12.0f, 6.0f, 60.0f, c);
    c.maxDistance = FloatOr(ini, s, "MaxDistance", 25.0f, 8.0f, 80.0f, c);
    if (c.maxDistance < c.minDistance + 4.0f) {
        c.warnings.push_back("MaxDistance must be at least MinDistance + 4; raised");
        c.maxDistance = c.minDistance + 4.0f;
    }
    c.idealDistance = FloatOr(ini, s, "IdealDistance", 16.0f, c.minDistance, c.maxDistance, c);
    c.botMinDistance = FloatOr(ini, s, "BotMinDistance", 8.0f, 4.0f, c.minDistance, c);
    c.floorBand = FloatOr(ini, s, "FloorBand", 12.0f, 0.5f, 30.0f, c);
    c.wallClearance = FloatOr(ini, s, "WallClearance", 0.3f, 0.0f, 2.0f, c);
    c.otherFloorBonus = FloatOr(ini, s, "OtherFloorBonus", 2.0f, -10.0f, 10.0f, c);
    c.maxPathFactor = FloatOr(ini, s, "MaxPathFactor", 2.2f, 1.0f, 6.0f, c);
    c.candidatesPerTick = UnsignedOr(ini, s, "CandidatesPerTick", 3, 1, 12, c);
    c.ventSpawns = UnsignedOr(ini, s, "VentSpawns", 1, 0, 1, c) != 0;
    c.ventLineOfSight = UnsignedOr(ini, s, "VentLineOfSight", 0, 0, 1, c) != 0;
    c.ventAnyMarker = UnsignedOr(ini, s, "VentAnyMarker", 1, 0, 1, c) != 0;
    c.ventMinDistance = FloatOr(ini, s, "VentMinDistance", 5.0f, 0.0f, 60.0f, c);
    c.ventMinStep = FloatOr(ini, s, "VentMinStep", 2.0f, 0.0f, 30.0f, c);
    c.ventMaxDistance = FloatOr(ini, s, "VentMaxDistance", 30.0f, 8.0f, 120.0f, c);
    c.ventMaxAbove = FloatOr(ini, s, "VentMaxAbove", 2.5f, 0.0f, 50.0f, c);
    c.ventMustReach = UnsignedOr(ini, s, "VentMustReach", 1, 0, 1, c) != 0;
    c.stalkerNeedsCover = UnsignedOr(ini, s, "StalkerNeedsCover", 1, 0, 1, c) != 0;
    c.catchUpMeters = FloatOr(ini, s, "CatchUpMeters", 50.0f, 0.0f, 500.0f, c);
    c.catchUpMetersFour = FloatOr(ini, s, "CatchUpMetersFour", 40.0f, 0.0f, 500.0f, c);
    c.catchUpMs = UnsignedOr(ini, s, "CatchUpSeconds", 0, 0, 120, c) * 1000u;
    {
        const char* k = "Cleanup";
        c.cleanupKills = UnsignedOr(ini, k, "StuckCorpseDelete", 1, 0, 1, c) != 0;
        c.cleanupDelayMs = UnsignedOr(ini, k, "StuckCorpseDelayMs", 3000, 1000, 60000, c);
        c.killKeepRadius = FloatOr(ini, k, "StuckCorpseKeepRadius", 10.0f, 0.0f, 100.0f, c);
        c.cleanupFades = UnsignedOr(ini, k, "FadeSweeper", 1, 0, 1, c) != 0;
        c.fadeStallMs = UnsignedOr(ini, k, "FadeStallMs", 3000, 1000, 60000, c);
        c.pickupCap = UnsignedOr(ini, k, "PickupCap", 1, 0, 1, c) != 0;
        c.maxPickups = UnsignedOr(ini, k, "MaxPickups", 40, 5, 400, c);
        c.pickupHardMax = UnsignedOr(ini, k, "PickupHardMax", 80, 10, 500, c);
        if (c.pickupHardMax < c.maxPickups) c.pickupHardMax = c.maxPickups;
        c.pickupMinAgeS = UnsignedOr(ini, k, "PickupMinAgeSeconds", 60, 0, 3600, c);
        c.pickupKeepRadius = FloatOr(ini, k, "PickupKeepRadius", 8.0f, 0.0f, 100.0f, c);
    }
    if (c.ventMaxDistance < c.ventMinDistance + 4.0f) {
        c.warnings.push_back("[Survival] VentMaxDistance below VentMinDistance + 4; raised");
        c.ventMaxDistance = c.ventMinDistance + 4.0f;
    }
    c.ventCrossFamily = UnsignedOr(ini, s, "VentCrossFamily", 1, 0, 1, c) != 0;
    c.ventIgnoreFamilyOff = UnsignedOr(ini, s, "VentIgnoreVentFamilies", 1, 0, 1, c) != 0;
    c.ventSharePercent = UnsignedOr(ini, s, "VentShare", 40, 0, 100, c);
    c.ventGapMs = UnsignedOr(ini, s, "VentGapMs", 3500, 3000, 60000, c);
    c.ventWaitMs = UnsignedOr(ini, s, "VentWaitMs", 15000, 0, 120000, c);
    c.forceAware = UnsignedOr(ini, s, "ForceAware", 1, 0, 1, c) != 0;
    c.awareRefreshMs = UnsignedOr(ini, s, "AwareRefreshMs", 3000, 500, 60000, c);
    c.repairBlindTemplates = UnsignedOr(ini, s, "RepairBlindTemplates", 1, 0, 1, c) != 0;
    c.trackMeters = FloatOr(ini, s, "TrackMeters", 80.0f, 0.0f, 300.0f, c);
    c.maxChargers = UnsignedOr(ini, s, "MaxChargers", 2, 0, 30, c);
    c.zombieChargeFloor = FloatOr(ini, s, "ZombieChargeFloor", 0.9f, 0.0f, 1.0f, c);
    c.zombieChargeIgnoreVis = UnsignedOr(ini, s, "ZombieChargeIgnoreVis", 0, 0, 1, c) != 0;
    c.noPlayDead = UnsignedOr(ini, s, "NoPlayDead", 1, 0, 1, c) != 0;
    c.chargeUnseen = UnsignedOr(ini, s, "ChargeUnseen", 0, 0, 2, c);
    c.stuckS = UnsignedOr(ini, s, "StuckSeconds", 60, 15, 900, c);
    c.ventStuckS = UnsignedOr(ini, s, "VentStuckSeconds", 30, 10, 300, c);
    c.inertS = UnsignedOr(ini, s, "InertSeconds", 20, 5, 300, c);
    c.frozenS = UnsignedOr(ini, s, "FrozenSeconds", 15, 0, 300, c);
    if (c.frozenS && c.frozenS < 8) c.frozenS = 8;
    c.engageMeters = FloatOr(ini, s, "EngageMeters", 10.0f, 2.0f, 40.0f, c);
    c.timedEndKill = UnsignedOr(ini, s, "TimedEndKill", 1, 0, 1, c) != 0;
    c.superVariants = UnsignedOr(ini, s, "SuperVariants", 0, 0, 1, c) != 0;
    c.twitcherPercent = UnsignedOr(ini, s, "TwitcherPercent", 20, 0, 100, c);
    c.pregnantPercent = UnsignedOr(ini, s, "SeveredPregnantPercent", 20, 0, 100, c);
    c.navReserveKiB = UnsignedOr(ini, s, "NavPowerReserveKiB", 96, 0, 4096, c);
    c.rewardAmmoPerGun = UnsignedOr(ini, s, "RewardAmmoPerGun", 2, 0, 10, c);
    c.rewardMedPacks = UnsignedOr(ini, s, "RewardMedPacks", 1, 0, 10, c);
    c.rewardMedSize = UnsignedOr(ini, s, "RewardMedPackSize", 1, 0, 2, c);
    c.rewardCredits = UnsignedOr(ini, s, "RewardCredits", 1000, 0, 100000, c);
    c.rewardStasis = UnsignedOr(ini, s, "RewardStasis", 1, 0, 10, c);
    c.endlessFrom = UnsignedOr(ini, s, "EndlessCycleFrom", 4, 1, kMaxWaves, c);
    c.endlessTo = UnsignedOr(ini, s, "EndlessCycleTo", 9, 1, kMaxWaves, c);
    c.endlessGrowthPercent = UnsignedOr(ini, s, "EndlessGrowthPercent", 20, 0, 200, c);
    c.bossEvery = UnsignedOr(ini, s, "BossEvery", 10, 0, kMaxWaves, c);
    c.notices = UnsignedOr(ini, s, "Notices", 1, 0, 1, c) != 0;
    c.hud = UnsignedOr(ini, s, "Hud", 1, 0, 1, c) != 0;
    c.noticeGapMs = UnsignedOr(ini, s, "NoticeGapMs", 4000, 1000, 60000, c);
    c.showLastEnemies = UnsignedOr(ini, s, "ShowLastEnemies", 3, 0, 8, c);
    c.logSpawnLines = UnsignedOr(ini, s, "LogSpawnLines", 300, 0, 5000, c);
    {
        const char* e = "Escalation";
        c.escFromWave = UnsignedOr(ini, e, "FromWave", 11, 1, 1000, c);
        c.escEnhancedPerWave = UnsignedOr(ini, e, "EnhancedPerWave", 5, 0, 100, c);
        c.escEnhancedMax = UnsignedOr(ini, e, "EnhancedMax", 60, 0, 100, c);
        c.escHealthPerWave = UnsignedOr(ini, e, "HealthPerWave", 3, 0, 100, c);
        c.escHealthMax = UnsignedOr(ini, e, "HealthMax", 60, 0, 1000, c);
        c.escOracleShare = UnsignedOr(ini, e, "OracleShare", 35, 0, 100, c);
        c.escOracleMaxAlive = UnsignedOr(ini, e, "OracleMaxAlive", 3, 0, 50, c);
        c.escClockStages = UnsignedOr(ini, e, "ClockStagesPerWave", 3, 1, 50, c);
        c.escClockEnhancedMax = UnsignedOr(ini, e, "ClockEnhancedMax", 25, 0, 100, c);
        c.escSpeedPerWave = UnsignedOr(ini, e, "SpeedPerWave", 1, 0, 50, c);
        c.escSpeedMax = UnsignedOr(ini, e, "SpeedMax", 40, 0, 200, c);
        c.escPauseStepS = FloatOr(ini, e, "PauseStepSeconds", 0.5f, 0.0f, 60.0f, c);
        c.escPauseMinS = UnsignedOr(ini, e, "PauseMinSeconds", 8, 0, 600, c);
        c.escMaxAlivePerPlayer = UnsignedOr(ini, e, "MaxAlivePerPlayer", 3, 0, 30, c);
        c.escMaxAliveCap = UnsignedOr(ini, e, "MaxAliveCap", 23, 1, 30, c);
        c.escRewardStep = FloatOr(ini, e, "RewardStepPercent", 2.5f, 0.0f, 100.0f, c);
        c.escRewardMin = UnsignedOr(ini, e, "RewardMinPercent", 40, 0, 100, c);
        c.escPriceStep = UnsignedOr(ini, e, "PriceStepPercent", 0, 0, 100, c);
        c.escPriceMax = UnsignedOr(ini, e, "PriceMaxPercent", 200, 100, 1000, c);
        c.escBotDamageStep = UnsignedOr(ini, e, "BotDamageStepPercent", 1, 0, 100, c);
        c.escBotDamageMin = UnsignedOr(ini, e, "BotDamageMinPercent", 50, 0, 100, c);
        c.escBossStasisPerBoss = UnsignedOr(ini, e, "BossStasisResistPerBoss", 20, 0, 100, c);
        c.escBossStasisMax = UnsignedOr(ini, e, "BossStasisResistMax", 80, 0, 100, c);
    }
    for (unsigned n = 1; n <= kMaxWaves; ++n) {
        const std::string name = "Wave" + std::to_string(n);
        const std::string* enemies = Value(ini, name, "Enemies");
        if (!enemies) break;
        Wave w;
        std::string error;
        if (!ParseEntries(*enemies, w.entries, &error)) {
            c.warnings.push_back("[" + name + "] Enemies: " + error + "; waves stop before it");
            break;
        }
        const char* sec = name.c_str();
        w.maxAlive = UnsignedOr(ini, sec, "MaxAlive", 0, 0, 30, c);
        w.intervalMs = UnsignedOr(ini, sec, "SpawnIntervalMs", 0, 0, 60000, c);
        w.timeLimitS = UnsignedOr(ini, sec, "TimeLimit", 0, 0, 1800, c);
        w.pauseS = UnsignedOr(ini, sec, "Pause", 0, 0, 600, c);
        if (const std::string* e = Value(ini, sec, "Enhanced"); e && !e->empty()) w.enhanced = static_cast<int>(UnsignedOr(ini, sec, "Enhanced", 0, 0, 100, c));
        for (const auto& e : w.entries) if (Unkillable(e.family)) w.boss = true;
        if (w.boss && !w.timeLimitS) {
            w.timeLimitS = 90;
            c.warnings.push_back("[" + name + "] holds an Ubermorph and no TimeLimit; 90 s used");
        }
        c.waves.push_back(w);
    }
    if (c.waves.empty()) c.warnings.push_back("no [Wave1] Enemies line: nothing to play");
    if (c.endlessTo > c.waves.size()) c.endlessTo = static_cast<unsigned>(c.waves.size());
    if (c.endlessFrom > c.endlessTo) c.endlessFrom = c.endlessTo;
    const char* m = "Mercenaries";
    auto& k = c.mercs;
    k.startSeconds = UnsignedOr(ini, m, "StartSeconds", 150, 30, 900, c);
    k.startDelayS = UnsignedOr(ini, m, "StartDelaySeconds", 3, 0, 120, c);
    k.maxSeconds = UnsignedOr(ini, m, "MaxSeconds", 600, k.startSeconds, 3600, c);
    k.killMs = UnsignedOr(ini, m, "KillMs", 1000, 0, 60000, c);
    k.eliteKillMs = UnsignedOr(ini, m, "EliteKillMs", 5000, 0, 60000, c);
    k.comboWindowMs = UnsignedOr(ini, m, "ComboWindowMs", 3000, 500, 20000, c);
    k.comboStep = UnsignedOr(ini, m, "ComboStep", 10, 0, 100, c);
    k.comboBonusMs = UnsignedOr(ini, m, "ComboBonusMs", 10000, 0, 120000, c);
    k.capsules = UnsignedOr(ini, m, "Capsules", 2, 0, 20, c);
    k.capsuleMs = UnsignedOr(ini, m, "CapsuleMs", 30000, 0, 300000, c);
    k.goldenMs = UnsignedOr(ini, m, "GoldenCapsuleMs", 60000, 0, 300000, c);
    k.goldenPercent = UnsignedOr(ini, m, "GoldenPercent", 15, 0, 100, c);
    k.capsuleLifeS = UnsignedOr(ini, m, "CapsuleLifeSeconds", 75, 15, 600, c);
    k.capsuleGapM = UnsignedOr(ini, m, "CapsuleGapMeters", 12, 3, 60, c);
    k.capsuleTotal = UnsignedOr(ini, m, "CapsuleTotal", 8, 0, 200, c);
    k.capsuleMinM = UnsignedOr(ini, m, "CapsuleMinMeters", 20, 0, 150, c);
    k.fadeS = UnsignedOr(ini, m, "BonusFadeSeconds", 480, 0, 3600, c);
    k.fadeFloorPercent = UnsignedOr(ini, m, "BonusFloorPercent", 20, 0, 100, c);
    k.maxRunS = UnsignedOr(ini, m, "MaxRunSeconds", 900, 0, 7200, c);
    if (k.maxRunS && k.maxRunS < 120) k.maxRunS = 120;
    k.deathPenaltyMs = UnsignedOr(ini, m, "DeathPenaltyMs", 15000, 0, 300000, c);
    k.killTarget = UnsignedOr(ini, m, "KillTarget", 150, 0, 10000, c);
    k.clearBonusPerSecond = UnsignedOr(ini, m, "ClearBonusPerSecond", 100, 0, 100000, c);
    k.supplyKills = UnsignedOr(ini, m, "SupplyEveryKills", 30, 0, 1000, c);
    k.resultS = UnsignedOr(ini, m, "ResultSeconds", 20, 5, 120, c);
    k.restart = UnsignedOr(ini, m, "RestartAfterResult", 1, 0, 1, c);
    k.finalSeconds = UnsignedOr(ini, m, "FinalSeconds", 60, 0, 600, c);
    if (const std::string* f = Value(ini, m, "FinalEnemies")) {
        std::string error;
        if (!f->empty() && !ParseEntries(*f, k.finalEnemies, &error)) {
            c.warnings.push_back("[Mercenaries] FinalEnemies: " + error + "; none used");
            k.finalEnemies.clear();
        }
    }
    static const char* const kRankKeys[] = {"RankC", "RankB", "RankA", "RankS", "RankSS"};
    for (unsigned i = 0; i < 5; ++i) k.ranks[i] = UnsignedOr(ini, m, kRankKeys[i], k.ranks[i], 0, 100000000, c);
    const char* o = "Objective";
    auto& j = c.objective;
    j.startDelayS = UnsignedOr(ini, o, "StartDelaySeconds", 3, 0, 120, c);
    j.startPercent = UnsignedOr(ini, o, "StartPercent", 100, 50, 1000, c);
    j.bonusPercent = UnsignedOr(ini, o, "BonusPercent", 100, 0, 1000, c);
    j.stepBonusS = UnsignedOr(ini, o, "StepBonusSeconds", 30, 0, 600, c);
    j.stepFloorS = UnsignedOr(ini, o, "StepFloorSeconds", 90, 0, 600, c);
    j.maxSeconds = UnsignedOr(ini, o, "MaxSeconds", 1800, 60, 7200, c);
    j.deathPenaltyMs = UnsignedOr(ini, o, "DeathPenaltyMs", 10000, 0, 300000, c);
    j.breatherS = UnsignedOr(ini, o, "BreatherSeconds", 0, 0, 120, c);
    j.supplies = UnsignedOr(ini, o, "SuppliesPerObjective", 1, 0, 1, c);
    j.stageStep = UnsignedOr(ini, o, "StagesPerObjective", 1, 0, 10, c);
    j.resultS = UnsignedOr(ini, o, "ResultSeconds", 20, 5, 120, c);
    j.restart = UnsignedOr(ini, o, "RestartAfterResult", 1, 0, 1, c);
    j.defendDps = UnsignedOr(ini, o, "DefendDamagePerSecond", 12, 0, 500, c);
    j.defendRadiusM = UnsignedOr(ini, o, "DefendRadius", 4, 1, 20, c);
    const char* z = "Zone";
    auto& h = c.zone;
    h.startDelayS = UnsignedOr(ini, z, "StartDelaySeconds", 3, 0, 120, c);
    h.startSeconds = UnsignedOr(ini, z, "StartSeconds", 150, 30, 900, c);
    h.maxSeconds = UnsignedOr(ini, z, "MaxSeconds", 600, h.startSeconds, 3600, c);
    h.bonusS = UnsignedOr(ini, z, "BonusSeconds", 60, 0, 600, c);
    h.deathPenaltyMs = UnsignedOr(ini, z, "DeathPenaltyMs", 10000, 0, 300000, c);
    h.radiusM = FloatOr(ini, z, "RadiusMeters", 4.5f, 2.0f, 12.0f, c);
    h.captureS = UnsignedOr(ini, z, "CaptureSeconds", 30, 3, 300, c);
    h.teamSpeedPercent = UnsignedOr(ini, z, "TeamSpeedPercent", 50, 0, 200, c);
    h.decayPercent = UnsignedOr(ini, z, "DecayPercent", 50, 0, 200, c);
    h.contestEnemies = UnsignedOr(ini, z, "ContestEnemies", 2, 1, 20, c);
    h.supplies = UnsignedOr(ini, z, "SuppliesPerZone", 1, 0, 1, c);
    h.stageStep = UnsignedOr(ini, z, "StagesPerZone", 1, 0, 10, c);
    h.targetZones = UnsignedOr(ini, z, "TargetZones", 0, 0, 1000, c);
    h.fadeS = UnsignedOr(ini, z, "BonusFadeSeconds", 600, 0, 3600, c);
    h.fadeFloorPercent = UnsignedOr(ini, z, "BonusFloorPercent", 20, 0, 100, c);
    h.maxRunS = UnsignedOr(ini, z, "MaxRunSeconds", 720, 0, 7200, c);
    if (h.maxRunS && h.maxRunS < 120) h.maxRunS = 120;
    h.resultS = UnsignedOr(ini, z, "ResultSeconds", 20, 5, 120, c);
    h.restart = UnsignedOr(ini, z, "RestartAfterResult", 1, 0, 1, c);
    const char* vs = "VersusSlashers";
    auto& v = c.versus;
    v.enabled = UnsignedOr(ini, vs, "Enabled", 1, 0, 1, c) != 0;
    v.alive = UnsignedOr(ini, vs, "Alive", 5, 0, 12, c);
    v.firstSpawnS = UnsignedOr(ini, vs, "FirstSpawnSeconds", 10, 0, 120, c);
    v.respawnMinS = UnsignedOr(ini, vs, "RespawnMinSeconds", 25, 1, 300, c);
    v.respawnMaxS = UnsignedOr(ini, vs, "RespawnMaxSeconds", 40, v.respawnMinS, 300, c);
    v.health = UnsignedOr(ini, vs, "Health", 80, 0, 5000, c);
    v.superHealth = UnsignedOr(ini, vs, "SuperHealth", 200, 0, 5000, c);
    v.fastObjectiveS = UnsignedOr(ini, vs, "FastObjectiveSeconds", 45, 0, 600, c);
    v.killLead = UnsignedOr(ini, vs, "KillLead", 4, 0, 50, c);
    v.decayS = UnsignedOr(ini, vs, "PressureDecaySeconds", 60, 0, 600, c);
    v.maxLevel = UnsignedOr(ini, vs, "MaxPressure", 3, 0, 10, c);
    v.superPercentPerLevel = UnsignedOr(ini, vs, "SuperPercentPerLevel", 34, 0, 100, c);
    v.extraAlivePerLevel = UnsignedOr(ini, vs, "ExtraAlivePerLevel", 1, 0, 5, c);
    v.superBurst = UnsignedOr(ini, vs, "SuperBurst", 1, 0, 5, c);
    v.retargetFromNecros = UnsignedOr(ini, vs, "RetargetFromNecros", 1, 0, 1, c) != 0;
    // AlivePerMap=pdlc_mp_school:3, ...
    if (const std::string* per = Value(ini, vs, "AlivePerMap")) {
        for (const std::string& part : SplitList(*per)) {
            const size_t colon = part.find(':');
            unsigned n = 0;
            if (colon == std::string::npos || !ParseUnsigned(Trim(part.substr(colon + 1)), n) || n > 12) {
                c.warnings.push_back("[VersusSlashers] AlivePerMap entry '" + part + "' is not map:count (0..12); skipped");
                continue;
            }
            v.aliveFor[LowerText(Trim(part.substr(0, colon)))] = n;
        }
    }
    // [VersusSkins]: <map>=package,package,... and Default=...
    const auto skins = ini.find("versusskins");
    if (skins != ini.end())
        for (const auto& [key, value] : skins->second) {
            std::vector<std::string> list;
            for (const std::string& p : SplitList(value)) list.push_back(LowerText(p));
            if (list.empty()) { c.warnings.push_back("[VersusSkins] " + key + " lists no skin; skipped"); continue; }
            if (key == "default") v.defaultSkins = list;
            else v.skins[key] = list;
        }
    if (const auto mf = ini.find("mapfamilies"); mf != ini.end()) {
        for (const auto& [map, list] : mf->second) {
            Config::MapRule rule;
            std::string token;
            auto take = [&](std::string t) {
                t = Trim(t);
                if (t.empty()) return;
                const bool without = t[0] == '-';
                const std::string family = CanonicalFamily(FamilyAlias(without ? t.substr(1) : t));
                if (family.empty()) { c.warnings.push_back("[MapFamilies] " + map + ": '" + t + "' is no family; skipped"); return; }
                (without ? rule.without : rule.only).insert(family);
            };
            for (const char ch : list) {
                if (ch == ',' || ch == ' ' || ch == ';') { take(token); token.clear(); }
                else token += ch;
            }
            take(token);
            if (!rule.only.empty() || !rule.without.empty()) c.mapFamilies[LowerText(map)] = rule;
        }
    }
    return c;
}
// Whether [MapFamilies] lets this family come on this map (no rule: every family).
inline bool MapAllowsFamily(const Config& c, const std::string& map, const std::string& family) {
    const auto r = c.mapFamilies.find(LowerText(map));
    if (r == c.mapFamilies.end()) return true;
    if (r->second.without.count(family)) return false;
    return r->second.only.empty() || r->second.only.count(family) > 0;
}

// --- Versus slashers ---
inline const std::vector<std::string>& VersusSkinsFor(const Config::Versus& v, const std::string& map) {
    const auto it = v.skins.find(LowerText(map));
    return it != v.skins.end() ? it->second : v.defaultSkins;
}
inline unsigned VersusAliveFor(const Config::Versus& v, const std::string& map) {
    const auto it = v.aliveFor.find(LowerText(map));
    return it != v.aliveFor.end() ? it->second : v.alive;
}
// The humans' dominance in a Versus round. Fast objectives and a lead in player
// kills raise it, time without a raise lowers it again. Each level turns more of
// the new slashers into super slashers and keeps more of them alive.
struct VersusPressure {
    unsigned level = 0, raises = 0, drops = 0, objectives = 0, fast = 0, killRaises = 0;
    uint32_t objectiveAt = 0, changedAt = 0;
    int killMark = 0;
    void Start(uint32_t now, const Config::Versus& v) {
        *this = VersusPressure{};
        objectiveAt = changedAt = now;
        killMark = static_cast<int>(v.killLead);
    }
    // True when the level went up; at the top it only keeps the level from decaying.
    bool Raise(uint32_t now, const Config::Versus& v) {
        changedAt = now;
        if (level >= v.maxLevel) return false;
        ++level;
        ++raises;
        return true;
    }
    // An objective was done now.
    bool Objective(uint32_t now, const Config::Versus& v) {
        const uint32_t took = now - objectiveAt;
        objectiveAt = now;
        ++objectives;
        if (!v.fastObjectiveS || took >= v.fastObjectiveS * 1000u) return false;
        ++fast;
        return Raise(now, v);
    }
    // The round's player kills so far. Every KillLead more for the humans raises
    // it once; a lead that falls well back brings the next mark down again.
    bool Kills(int humans, int necros, uint32_t now, const Config::Versus& v) {
        if (!v.killLead) return false;
        const int lead = humans - necros, step = static_cast<int>(v.killLead);
        if (lead >= killMark) {
            killMark = lead + step;
            ++killRaises;
            return Raise(now, v);
        }
        if (killMark - lead > 2 * step) killMark = std::max(step, lead + step);
        return false;
    }
    bool Decay(uint32_t now, const Config::Versus& v) {
        if (!level || !v.decayS || now - changedAt < v.decayS * 1000u) return false;
        --level;
        ++drops;
        changedAt = now;
        return true;
    }
    unsigned SuperPercent(const Config::Versus& v) const { return std::min(100u, level * v.superPercentPerLevel); }
    unsigned Alive(unsigned base, const Config::Versus& v) const { return base + level * v.extraAlivePerLevel; }
};
// The respawn delay of a slasher slot after a death, MinS..MaxS from a 0..1 roll.
inline uint32_t VersusRespawnMs(const Config::Versus& v, float roll) {
    const float r = roll < 0.0f ? 0.0f : roll > 1.0f ? 1.0f : roll;
    return static_cast<uint32_t>((v.respawnMinS + (v.respawnMaxS - v.respawnMinS) * r) * 1000.0f);
}

// The wave the director plays as number n (1-based). Authored waves come first;
// after them every BossEvery-th wave repeats the last authored boss wave, the
// others cycle through EndlessCycleFrom..To with counts grown by
// EndlessGrowthPercent per cycle step taken.
inline Wave WaveFor(unsigned n, const Config& c) {
    if (!n || c.waves.empty()) return Wave{};
    if (n <= c.waves.size()) return c.waves[n - 1];
    const unsigned past = n - static_cast<unsigned>(c.waves.size());
    const Wave* boss = nullptr;
    for (const auto& w : c.waves) if (w.boss) boss = &w;
    Wave w;
    if (boss && c.bossEvery && n % c.bossEvery == 0) {
        w = *boss;
    } else {
        const unsigned from = c.endlessFrom ? c.endlessFrom : 1, to = c.endlessTo ? c.endlessTo : static_cast<unsigned>(c.waves.size());
        std::vector<unsigned> cycle;
        for (unsigned i = from; i <= to; ++i) if (!c.waves[i - 1].boss) cycle.push_back(i);
        if (cycle.empty()) cycle.push_back(static_cast<unsigned>(c.waves.size()));
        w = c.waves[cycle[(past - 1) % cycle.size()] - 1];
    }
    const double grow = 1.0 + static_cast<double>(c.endlessGrowthPercent) / 100.0 * past;
    for (auto& e : w.entries) {
        if (Unkillable(e.family)) continue;
        const double scaled = std::floor(e.count * grow + 0.5);
        e.count = static_cast<unsigned>(std::min<double>(scaled, kMaxEntryCount));
    }
    return w;
}

// --- escalation ---
// How far into the escalation wave n is: 0 up to FromWave - 1, then 1, 2, ...
inline unsigned EscLevel(unsigned wave, const Config& c) { return wave >= c.escFromWave ? wave - c.escFromWave + 1 : 0; }
inline unsigned EnhancedChance(unsigned wave, const Config& c) {
    const unsigned v = EscLevel(wave, c) * c.escEnhancedPerWave;
    return v > c.escEnhancedMax ? c.escEnhancedMax : v;
}
// A wave's own Enhanced= wins over the escalation, but never lowers it.
inline unsigned EnhancedChanceFor(unsigned wave, const Wave& w, const Config& c) {
    const unsigned esc = EnhancedChance(wave, c);
    return w.enhanced >= 0 && static_cast<unsigned>(w.enhanced) > esc ? static_cast<unsigned>(w.enhanced) : esc;
}
// The escalation wave of clock stage n: ClockStagesPerWave stages make one wave.
inline unsigned ClockEscWave(unsigned stage, const Config& c) {
    return c.escClockStages > 1 ? (stage + c.escClockStages - 1) / c.escClockStages : stage;
}
inline unsigned ClockEnhancedChance(unsigned stage, const Wave& w, const Config& c) {
    const unsigned v = EnhancedChanceFor(ClockEscWave(stage, c), w, c);
    return v > c.escClockEnhancedMax ? c.escClockEnhancedMax : v;
}
inline float HealthFactor(unsigned wave, const Config& c) {
    unsigned v = EscLevel(wave, c) * c.escHealthPerWave;
    if (v > c.escHealthMax) v = c.escHealthMax;
    return 1.0f + static_cast<float>(v) / 100.0f;
}
inline float SpeedFactor(unsigned wave, const Config& c) {
    unsigned v = EscLevel(wave, c) * c.escSpeedPerWave;
    if (v > c.escSpeedMax) v = c.escSpeedMax;
    return 1.0f + static_cast<float>(v) / 100.0f;
}
inline unsigned PauseFor(unsigned wave, unsigned basePause, const Config& c) {
    const unsigned cut = static_cast<unsigned>(static_cast<float>(EscLevel(wave, c)) * c.escPauseStepS + 1e-3f);   // whole seconds
    if (!cut || basePause <= c.escPauseMinS) return basePause;
    return basePause > cut + c.escPauseMinS ? basePause - cut : c.escPauseMinS;
}
// players: everybody PlayerManager holds (the human, partners, bots)
inline unsigned MaxAliveFor(unsigned base, unsigned players, const Config& c) {
    const unsigned extra = players > 1 ? (players - 1) * c.escMaxAlivePerPlayer : 0;
    const unsigned v = base + extra;
    return v > c.escMaxAliveCap ? (base > c.escMaxAliveCap ? base : c.escMaxAliveCap) : v;
}
inline unsigned RewardPercent(unsigned wave, const Config& c) {
    const unsigned cut = static_cast<unsigned>(static_cast<float>(EscLevel(wave, c)) * c.escRewardStep + 1e-3f);   // whole percent
    return cut >= 100 || 100 - cut < c.escRewardMin ? c.escRewardMin : 100 - cut;
}
inline unsigned PricePercent(unsigned wave, const Config& c) {
    const unsigned v = 100 + EscLevel(wave, c) * c.escPriceStep;
    return v > c.escPriceMax ? c.escPriceMax : v;
}
inline unsigned BotDamagePercent(unsigned wave, const Config& c) {
    const unsigned cut = EscLevel(wave, c) * c.escBotDamageStep;
    return cut >= 100 || 100 - cut < c.escBotDamageMin ? c.escBotDamageMin : 100 - cut;
}
// Boss waves reached so far (wave 10 is the first with BossEvery=10).
inline unsigned BossWavesReached(unsigned wave, const Config& c) { return c.bossEvery ? wave / c.bossEvery : 0; }
inline unsigned BossStasisResist(unsigned wave, const Config& c) {
    const unsigned v = BossWavesReached(wave, c) * c.escBossStasisPerBoss;
    return v > c.escBossStasisMax ? c.escBossStasisMax : v;
}
// A count scaled by a percentage, at least one when there was one to start with.
inline unsigned Scaled(unsigned n, unsigned percent) {
    if (!n) return 0;
    const unsigned v = (n * percent + 50) / 100;
    return v ? v : 1;
}

// --- the spawn queue ---
struct Item {
    std::string family;   // after substitution
    std::string asked;    // what the wave line said
    Path path = Path::Any;
    unsigned group = 1;   // size of the pack this item starts or belongs to
    bool groupHead = true;
    std::string package;  // Versus: only donors with this model package (a skin), empty = any
    bool super = false;   // Versus: gets the super slasher's health
};
// Heavies and bosses first (they are few and set the tone), the rest interleaved
// so a mixed wave arrives mixed: each step takes the entry with the most left
// relative to its size. Packs stay together.
inline std::vector<Item> BuildQueue(const Wave& w) {
    std::vector<Item> queue;
    std::vector<unsigned> left;
    for (const auto& e : w.entries) left.push_back(e.count);
    for (size_t i = 0; i < w.entries.size(); ++i) {
        const Role r = RoleOf(w.entries[i].family);
        if (r != Role::Heavy && r != Role::Boss) continue;
        const Entry& e = w.entries[i];
        while (left[i]) {
            const unsigned g = std::min(e.group, left[i]);
            for (unsigned k = 0; k < g; ++k) queue.push_back(Item{e.family, e.family, e.path, g, k == 0});
            left[i] -= g;
        }
    }
    for (;;) {
        int best = -1;
        double bestShare = -1.0;
        for (size_t i = 0; i < w.entries.size(); ++i) {
            if (!left[i]) continue;
            const double share = static_cast<double>(left[i]) / w.entries[i].count;
            if (share > bestShare + 1e-9) { bestShare = share; best = static_cast<int>(i); }
        }
        if (best < 0) break;
        const Entry& e = w.entries[static_cast<size_t>(best)];
        const unsigned g = std::min(e.group, left[static_cast<size_t>(best)]);
        for (unsigned k = 0; k < g; ++k) queue.push_back(Item{e.family, e.family, e.path, g, k == 0});
        left[static_cast<size_t>(best)] -= g;
    }
    return queue;
}

// The queue with every family replaced by what the map can field. Items whose
// family has no stand-in are dropped and counted.
inline std::vector<Item> ResolveQueue(const std::vector<Item>& queue, const std::set<std::string>& available, unsigned& dropped,
                                      std::map<std::string, std::string>* substitutions = nullptr) {
    std::vector<Item> out;
    dropped = 0;
    for (const auto& item : queue) {
        const Substitution sub = Substitute(item.family, available);
        if (sub.family.empty()) { ++dropped; continue; }
        if (sub.substituted && substitutions) (*substitutions)[item.family] = sub.family + (sub.factor > 1 ? " x" + std::to_string(sub.factor) : "");
        for (unsigned k = 0; k < sub.factor; ++k) {
            Item copy = item;
            copy.family = sub.family;
            // a stand-in for a direct-only heavy may well use a vent
            if (sub.substituted && copy.path == Path::Direct && RoleOf(sub.family) != Role::Heavy) copy.path = Path::Any;
            if (k) copy.groupHead = false;
            out.push_back(copy);
        }
    }
    return out;
}

// --- pacing ---
inline bool SpawnDue(uint32_t now, uint32_t lastSpawnAt, uint32_t intervalMs, unsigned alive, unsigned cap, bool groupFollower) {
    if (alive >= cap) return false;
    if (groupFollower) return now - lastSpawnAt >= 250u;   // the rest of a pack follows at once
    return now - lastSpawnAt >= intervalMs;
}
// The cap actually used: the wave's, the global one, and the NavPower pool's say.
inline unsigned EffectiveCap(unsigned waveCap, unsigned globalCap, bool navEnlarged) {
    unsigned cap = waveCap ? waveCap : globalCap;
    if (!navEnlarged && cap > 10) cap = 10;   // the retail 576 KiB pool gave out near 160 entries for the whole level
    return cap ? cap : 1;
}

// --- spawn points ---
struct V3 { float x = 0, y = 0, z = 0; };
inline V3 Sub(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float Length(const V3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
inline float Distance(const V3& a, const V3& b) { return Length(Sub(a, b)); }
inline float FlatDistance(const V3& a, const V3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

enum class PointVerdict : uint8_t { Ok, TooClose, TooFar, OtherFloor, NoPath, PathTooLong, Visible, Wall, Recent, OutsideArena, Count };
inline const char* VerdictName(PointVerdict v) {
    static const char* const names[] = {"ok", "too_close", "too_far", "other_floor", "no_path", "path_too_long", "visible", "wall", "recent", "outside_arena"};
    const unsigned i = static_cast<unsigned>(v);
    return i < static_cast<unsigned>(PointVerdict::Count) ? names[i] : "?";
}
constexpr float kSameFloor = 2.5f;   // height difference still counted as the same floor
struct PointRules { float minDistance = 12, botMinDistance = 12, maxDistance = 25, idealDistance = 18, floorBand = 3, maxPathFactor = 2.2f, recentRadius = 4, otherFloorBonus = 0; };
inline PointRules RulesOf(const Config& c) {
    PointRules r;
    r.minDistance = c.minDistance;
    r.botMinDistance = c.botMinDistance;
    r.maxDistance = c.maxDistance;
    r.idealDistance = c.idealDistance;
    r.floorBand = c.floorBand;
    r.maxPathFactor = c.maxPathFactor;
    r.otherFloorBonus = c.otherFloorBonus;
    return r;
}
// The checks that need no engine call: distance to every viewer, the floor band
// and the walk length the nav mesh reported (pathLength <= 0: no path).
inline PointVerdict CheckPoint(const V3& point, const V3& anchor, const V3* viewers, size_t viewerCount, float pathLength,
                               const PointRules& rules) {
    if (pathLength <= 0.0f) return PointVerdict::NoPath;
    if (std::fabs(point.y - anchor.y) > rules.floorBand) return PointVerdict::OtherFloor;
    const float fromAnchor = Distance(point, anchor);
    if (fromAnchor > rules.maxDistance) return PointVerdict::TooFar;
    for (size_t i = 0; i < viewerCount; ++i)
        if (Distance(point, viewers[i]) < (i ? rules.botMinDistance : rules.minDistance)) return PointVerdict::TooClose;   // viewers[0] is the player
    if (fromAnchor < rules.minDistance) return PointVerdict::TooClose;
    if (pathLength > fromAnchor * rules.maxPathFactor + 4.0f) return PointVerdict::PathTooLong;
    return PointVerdict::Ok;
}
// Higher is better: near the ideal distance, behind the anchor's facing, away
// from the last spots used, a little noise so equal spots take turns.
inline float ScorePoint(const V3& point, const V3& anchor, const V3& anchorForward, const V3* recent, size_t recentCount,
                        const PointRules& rules, float noise01) {
    const float d = Distance(point, anchor);
    float score = 10.0f - std::fabs(d - rules.idealDistance);
    const V3 to = Sub(point, anchor);
    const float flat = std::sqrt(to.x * to.x + to.z * to.z);
    const float fwd = std::sqrt(anchorForward.x * anchorForward.x + anchorForward.z * anchorForward.z);
    if (flat > 0.01f && fwd > 0.01f) {
        const float dot = (to.x * anchorForward.x + to.z * anchorForward.z) / (flat * fwd);
        score -= dot * 3.0f;   // behind the player scores up to 3 more
    }
    if (std::fabs(point.y - anchor.y) > kSameFloor) score += rules.otherFloorBonus;
    for (size_t i = 0; i < recentCount; ++i)
        if (Distance(point, recent[i]) < rules.recentRadius) score -= 4.0f;
    return score + noise01;
}
// Ring seeds around the anchor, the ring behind the player first.
inline std::vector<V3> RingSeeds(const V3& anchor, const V3& forward, const float* radii, size_t radiusCount, unsigned spokes, unsigned rotation) {
    std::vector<V3> out;
    if (!spokes) return out;
    const float yaw = std::atan2(forward.x, forward.z) + 3.14159265f;   // behind
    for (unsigned k = 0; k < spokes; ++k) {
        // 0, +1, -1, +2, -2 ... spokes away from straight behind, turned by `rotation` half steps
        const int step = (k % 2 ? 1 : -1) * static_cast<int>((k + 1) / 2);
        const float a = yaw + (static_cast<float>(step) + 0.5f * static_cast<float>(rotation % 2)) * (6.2831853f / static_cast<float>(spokes));
        for (size_t r = 0; r < radiusCount; ++r)
            out.push_back({anchor.x + std::sin(a) * radii[r], anchor.y, anchor.z + std::cos(a) * radii[r]});
    }
    return out;
}
// Polyline length.
inline float PathLength(const V3* points, size_t count) {
    float total = 0.0f;
    for (size_t i = 1; i < count; ++i) total += Distance(points[i - 1], points[i]);
    return total;
}
// Yaw in degrees for an enemy at `from` facing `to` (forward = (sin, 0, cos)).
inline float FacingYaw(const V3& from, const V3& to) {
    return static_cast<float>(std::atan2(to.x - from.x, to.z - from.z) * 180.0 / 3.14159265358979);
}
// Members of a pack stand around the head's spot.
inline V3 PackOffset(const V3& head, unsigned index) {
    if (!index) return head;
    const float a = static_cast<float>(index) * 2.0943951f;   // 120 degrees apart
    const float r = 1.3f;
    return {head.x + std::sin(a) * r, head.y, head.z + std::cos(a) * r};
}

// --- vent or direct ---
// roll01 in [0,1). Any: a vent when the family fits one and the roll lands in the share.
inline bool WantsVent(Path path, bool familyHasVent, bool ventsOn, unsigned sharePercent, float roll01) {
    if (!ventsOn || !familyHasVent) return false;
    if (path == Path::Vent) return true;
    if (path == Path::Direct) return false;
    return roll01 * 100.0f < static_cast<float>(sharePercent);
}

// --- tracked enemies ---
enum class Fate : uint8_t { Alive, Killed, Vanished, Stuck, VentStuck, Inert, Timer, WrittenOff };
inline const char* FateName(Fate f) {
    switch (f) {
    case Fate::Alive: return "alive";
    case Fate::Killed: return "killed";
    case Fate::Vanished: return "vanished";
    case Fate::Stuck: return "stuck";
    case Fate::VentStuck: return "vent_stuck";
    case Fate::Inert: return "inert";
    case Fate::Timer: return "timer_kill";
    case Fate::WrittenOff: return "written_off";
    }
    return "?";
}
struct Watch {
    uint32_t bornAt = 0, progressAt = 0;
    float bestDistance = 1e9f;
    bool engaged = false;
    uint32_t engagedAt = 0;
    float stillAt[3]{};            // where it stood when it last moved a metre
    uint32_t movedAt = 0;
};
// Its own movement, apart from the distance to the players: an enemy stuck in a wall next
// to a player counts as engaged and never stops making "progress".
inline void NoteMoved(Watch& w, float x, float y, float z, uint32_t now) {
    const float dx = x - w.stillAt[0], dy = y - w.stillAt[1], dz = z - w.stillAt[2];
    if (!w.movedAt || dx * dx + dy * dy + dz * dz > 1.0f) {
        w.stillAt[0] = x;
        w.stillAt[1] = y;
        w.stillAt[2] = z;
        w.movedAt = now ? now : 1;   // not now | 1: an even tick gave now+1 and "frozen for 4294967 s"
    }
}
// Lurkers and Pukers shoot from where they stand, Stalkers wait in cover.
// The vent minimum for this many living players and bots.
inline float VentMinFor(const Config& c, unsigned living) {
    const float d = c.ventMinDistance - c.ventMinStep * static_cast<float>(living > 1 ? living - 1 : 0);
    return d > 0.0f ? d : 0.0f;
}
inline bool HoldsStill(const char* family) {
    return _stricmp(family, "Lurker") == 0 || _stricmp(family, "Puker") == 0 || _stricmp(family, "Stalker") == 0;
}
inline bool Frozen(const Watch& w, uint32_t now, bool inVent, const Config& c) {
    const int32_t still = static_cast<int32_t>(now - w.movedAt), age = static_cast<int32_t>(now - w.bornAt);
    return c.frozenS && !inVent && w.movedAt && age >= 8000 && still >= static_cast<int32_t>(c.frozenS * 1000u);
}
// One observation of a living enemy: its distance to the nearest player, whether
// it still runs a vent machine and how many machines it has. Returns what the
// director should do with it now.
inline Fate Observe(Watch& w, uint32_t now, float nearest, bool inVent, unsigned machines, const Config& c) {
    if (nearest + 2.0f < w.bestDistance) { w.bestDistance = nearest; w.progressAt = now; }
    if (nearest <= c.engageMeters) {
        if (!w.engaged) { w.engaged = true; w.engagedAt = now; }
        w.progressAt = now;
    }
    const uint32_t age = now - w.bornAt;
    if (!machines && age >= c.inertS * 1000u) return Fate::Inert;
    if (inVent && age >= c.ventStuckS * 1000u) return Fate::VentStuck;
    if (now - w.progressAt >= c.stuckS * 1000u) return Fate::Stuck;
    return Fate::Alive;
}

// --- rewards ---
// Currency pickups of global_assets: 10000, 5000, 3000 and 1000 credits. Out: count per
// denomination, largest first; the rest below 1000 rounds up to one more 1000.
inline std::vector<unsigned> SplitCredits(unsigned amount) {
    static const unsigned kDenominations[] = {10000, 5000, 3000, 1000};
    std::vector<unsigned> out;
    for (unsigned d : kDenominations) { out.push_back(amount / d); amount %= d; }
    if (amount) ++out.back();
    return out;
}

// --- best wave file ---
// One "map wave" pair per line.
inline std::map<std::string, unsigned> ParseBest(const std::string& text) {
    std::map<std::string, unsigned> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = Trim(text.substr(at, end - at));
        at = end + 1;
        const size_t space = line.find_first_of(" \t");
        unsigned wave = 0;
        if (space == std::string::npos || !ParseUnsigned(line.substr(space + 1), wave)) continue;
        const std::string map = LowerText(Trim(line.substr(0, space)));
        if (!map.empty() && wave > out[map]) out[map] = wave;
    }
    return out;
}
inline std::string FormatBest(const std::map<std::string, unsigned>& best) {
    std::string out;
    for (const auto& e : best) out += e.first + " " + std::to_string(e.second) + "\n";
    return out;
}
// True when `wave` beats the stored best for `map` (and stores it).
inline bool UpdateBest(std::map<std::string, unsigned>& best, const std::string& map, unsigned wave) {
    unsigned& slot = best[LowerText(map)];
    if (wave <= slot) return false;
    slot = wave;
    return true;
}

// --- Mercenaries ---
// A clock instead of waves ([Mercenaries] in DS2Survival.ini): kills and time
// capsules add time, kills within the combo window chain, and every ComboStep-th
// chained kill pays a bonus. Times in game milliseconds (the pause stops them).
inline unsigned FamilyPoints(const std::string& family) {
    struct P { const char* family; unsigned points; };
    static const P kPoints[] = {{"Zombie", 100}, {"PackEnemy", 50}, {"SwarmEnemy", 20}, {"Leaper", 120}, {"Stalker", 200}, {"Puker", 150},
                                {"Lurker", 150}, {"Exploder", 100}, {"Infector", 120}, {"TubeBeetle", 80}, {"Brute", 500}, {"Pregnant", 250},
                                {"DividerCore", 400}, {"Hunter", 1500}};
    for (const auto& p : kPoints) if (EqualNoCase(family, p.family)) return p.points;
    return 100;
}
struct MercsScore {
    unsigned kills = 0, combo = 0, maxCombo = 0, score = 0, capsules = 0, deaths = 0;
    uint32_t lastKillAt = 0;
    bool anyKill = false;
    // The time bonus in ms for one kill at game time `now`; comboBonus when it completed a step.
    unsigned Kill(const std::string& family, bool elite, uint32_t now, unsigned killMs, unsigned eliteKillMs, unsigned comboWindowMs, unsigned comboStep,
                  unsigned comboBonusMs, bool& comboBonus) {
        comboBonus = false;
        combo = anyKill && now - lastKillAt <= comboWindowMs ? combo + 1 : 1;
        anyKill = true;
        lastKillAt = now;
        ++kills;
        if (combo > maxCombo) maxCombo = combo;
        const unsigned multiplier = 1 + (combo / 10u < 4u ? combo / 10u : 4u);   // x1..x5
        score += FamilyPoints(family) * (elite ? 2u : 1u) * multiplier;
        unsigned bonus = elite ? eliteKillMs : killMs;
        if (comboStep && combo % comboStep == 0) {
            bonus += comboBonusMs;
            comboBonus = true;
        }
        return bonus;
    }
};
// D..SS from the score; `ranks` holds the C, B, A, S and SS thresholds.
inline const char* MercsRank(unsigned score, const unsigned (&ranks)[5]) {
    static const char* const kNames[] = {"C", "B", "A", "S", "SS"};
    const char* rank = "D";
    for (unsigned i = 0; i < 5; ++i) if (score >= ranks[i]) rank = kNames[i];
    return rank;
}
// A time bonus after `elapsedMs` of the run: full at the start, falling evenly to
// floorPercent at fadeS and staying there, so a fast run still runs out of time.
inline unsigned FadedBonusMs(unsigned ms, uint32_t elapsedMs, unsigned fadeS, unsigned floorPercent) {
    if (!fadeS || floorPercent >= 100) return ms;
    const uint64_t fadeMs = static_cast<uint64_t>(fadeS) * 1000;
    const uint64_t done = elapsedMs < fadeMs ? elapsedMs : fadeMs;
    const uint64_t percent = 100 - (100 - floorPercent) * done / fadeMs;
    return static_cast<unsigned>(static_cast<uint64_t>(ms) * percent / 100);
}
// The clock after adding `addMs` (negative: a penalty), kept within 0..maxMs.
inline int64_t MercsClock(int64_t leftMs, int64_t addMs, int64_t maxMs) {
    const int64_t next = leftMs + addMs;
    return next < 0 ? 0 : next > maxMs ? maxMs : next;
}
// The Objective clock at the start: the map's own round time times StartPercent,
// at least a minute, at most MaxSeconds.
inline int64_t ObjectiveStartMs(float roundSeconds, unsigned startPercent, unsigned maxSeconds) {
    const int64_t round = roundSeconds > 0.0f ? static_cast<int64_t>(roundSeconds * 1000.0f) : 180000;
    const int64_t ms = round * startPercent / 100;
    const int64_t maxMs = static_cast<int64_t>(maxSeconds) * 1000;
    return ms < 60000 ? 60000 : ms > maxMs ? maxMs : ms;
}
// One of the map's TimerSets fired: an add grows the clock by its seconds times
// BonusPercent; a set (the map's "final N seconds") raises it to at least that.
inline int64_t ObjectiveBonusClock(int64_t leftMs, float seconds, bool relative, unsigned bonusPercent, int64_t maxMs) {
    const int64_t scaled = static_cast<int64_t>(seconds * 1000.0f) * bonusPercent / 100;
    if (relative) return MercsClock(leftMs, scaled, maxMs);
    return leftMs >= scaled ? leftMs : (scaled > maxMs ? maxMs : scaled);
}

// Hold the Zone: what the zone does this step.
enum class ZoneMode { Empty, Capturing, Contested, Losing };
inline const char* ZoneModeName(ZoneMode m) {
    switch (m) {
    case ZoneMode::Empty: return "EMPTY";
    case ZoneMode::Capturing: return "CAPTURING";
    case ZoneMode::Contested: return "CONTESTED";
    case ZoneMode::Losing: return "LOSING";
    }
    return "?";
}
// Players inside and no contest: up, faster for each more player (TeamSpeedPercent).
// Enough enemies inside: it stands. Nobody inside: down at DecayPercent of the
// capture speed (Empty when there is nothing to lose).
inline ZoneMode ZoneStep(float& progress, unsigned players, unsigned enemies, uint32_t dtMs, unsigned captureS, unsigned teamSpeedPercent,
                         unsigned decayPercent, unsigned contestEnemies) {
    const float base = captureS ? static_cast<float>(dtMs) / (static_cast<float>(captureS) * 1000.0f) : 1.0f;
    ZoneMode mode;
    if (players && enemies >= contestEnemies) {
        mode = ZoneMode::Contested;
    } else if (players) {
        progress += base * (1.0f + static_cast<float>(teamSpeedPercent) / 100.0f * static_cast<float>(players - 1));
        mode = ZoneMode::Capturing;
    } else if (progress > 0.0f && decayPercent) {
        progress -= base * static_cast<float>(decayPercent) / 100.0f;
        mode = ZoneMode::Losing;
    } else {
        mode = ZoneMode::Empty;
    }
    progress = progress < 0.0f ? 0.0f : progress > 1.0f ? 1.0f : progress;
    if (mode == ZoneMode::Losing && progress <= 0.0f) mode = ZoneMode::Empty;
    return mode;
}
// A zone spot recorded in the game (DS2Zones.txt): "map x y z [radius]" a line,
// '#' or ';' starts a comment, the map is compared without case. Radius 0: the
// configured one.
struct ZoneSpot { V3 pos; float radius = 0.0f; };
inline std::vector<ZoneSpot> ParseZoneSpots(const std::string& text, const std::string& map, unsigned& skipped, unsigned& other) {
    std::vector<ZoneSpot> out;
    skipped = other = 0;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        const size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos) line.resize(comment);
        std::vector<std::string> words;
        std::string word;
        for (char c : line) {
            if (c == ' ' || c == '\t' || c == ',' || c == '\r') {
                if (!word.empty()) words.push_back(word);
                word.clear();
            } else {
                word += c;
            }
        }
        if (!word.empty()) words.push_back(word);
        if (words.empty()) continue;
        ZoneSpot spot;
        if (words.size() < 4 || !ParseFloat(words[1], spot.pos.x) || !ParseFloat(words[2], spot.pos.y) || !ParseFloat(words[3], spot.pos.z) ||
            (words.size() > 4 && !ParseFloat(words[4], spot.radius))) {
            ++skipped;
            continue;
        }
        if (!EqualNoCase(words[0], map)) { ++other; continue; }
        if (!(spot.radius >= 2.0f && spot.radius <= 12.0f)) spot.radius = 0.0f;
        out.push_back(spot);
    }
    return out;
}
// The next recorded spot: any but the last one. -1 when there is none.
inline int PickZoneSpot(const std::vector<ZoneSpot>& spots, int last, uint32_t random) {
    if (spots.empty()) return -1;
    if (spots.size() == 1) return 0;
    std::vector<int> other;
    for (size_t i = 0; i < spots.size(); ++i)
        if (static_cast<int>(i) != last) other.push_back(static_cast<int>(i));
    return other[random % other.size()];
}
// Inside the zone: a standing cylinder, the radius on the floor plan, from half a
// metre below the spot to a metre above it (coop_zone::Inside is the same).
constexpr float kZoneBelowM = 0.5f, kZoneAboveM = 1.0f;
inline bool InZone(const V3& center, float radius, const V3& p) {
    const float dx = p.x - center.x, dz = p.z - center.z, dy = p.y - center.y;
    return dx * dx + dz * dz <= radius * radius && dy >= -kZoneBelowM && dy <= kZoneAboveM;
}

// --- a small deterministic generator ---
struct Rng {
    uint32_t state = 0x9E3779B9u;
    explicit Rng(uint32_t seed = 1) : state(seed ? seed : 0x9E3779B9u) {}
    uint32_t Next() {
        uint32_t x = state;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        return state = x;
    }
    float Unit() { return static_cast<float>(Next() >> 8) / 16777216.0f; }
};

// The next skin of a map's list: each one once per shuffled round, and a new
// round never starts with the one the last round ended on.
inline std::string NextVersusSkin(std::vector<std::string>& bag, const std::vector<std::string>& skins, const std::string& last, Rng& rng) {
    if (skins.empty()) return {};
    if (bag.empty()) {
        bag = skins;
        for (size_t i = bag.size(); i > 1; --i) std::swap(bag[i - 1], bag[rng.Next() % i]);
        if (bag.size() > 1 && bag.back() == last) std::swap(bag.front(), bag.back());
    }
    std::string skin = bag.back();
    bag.pop_back();
    return skin;
}

}  // namespace survival
