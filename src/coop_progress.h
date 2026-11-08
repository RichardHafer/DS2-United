// Dead Space 2 United by Richard Hafer.
#pragma once
// Profile and weapon progression (15.10.): the profile level comes from XP
// (Survival waves and results, the Versus match score), each gun levels by its
// owner's own kills and unlocks its clip and damage upgrades. The non-gun MP
// upgrades (stasis, necro rows) follow the profile level as in retail MP.
// analysis/MP_UPGRADES_EFFECTS_2026-10-15.md, analysis/SURVIVAL_PROGRESSION_SEAMS_2026-10-15.md.
// Pure logic, no game memory.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace coop_progress {

// --- guns ------------------------------------------------------------------------
// A hit names its gun by the ScoringComponent inflictor bit (DamageData+0x94):
// every campaign and MP gun record and its projectiles carry their gun bit.
// The Foam Finger is a Plasma Cutter record and carries 0x20.
struct Gun { const char* key; const char* name; uint32_t inflictor; };
inline constexpr Gun kGuns[] = {
    {"PlasmaCutter", "Plasma Cutter", 0x20},  {"PulseRifle", "Pulse Rifle", 0x40},   {"LineGun", "Line Gun", 0x80},
    {"Flamethrower", "Flamethrower", 0x100},  {"ContactBeam", "Contact Beam", 0x200}, {"Ripper", "Ripper", 0x400},
    {"ForceGun", "Force Gun", 0x800},         {"Javelin", "Javelin Gun", 0x1000},     {"Detonator", "Detonator", 0x2000},
    {"RivetGun", "Rivet Gun", 0x4000},        {"SeekerRifle", "Seeker Rifle", 0x8000},
};
constexpr int kGunCount = static_cast<int>(sizeof(kGuns) / sizeof(kGuns[0]));
inline int GunByInflictor(uint32_t flags) {
    for (int i = 0; i < kGunCount; ++i)
        if (flags & kGuns[i].inflictor) return i;
    return -1;
}
inline int GunByKey(const char* key) {
    for (int i = 0; key && i < kGunCount; ++i)
        if (!std::strcmp(kGuns[i].key, key)) return i;
    return -1;
}

// --- weapon levels -----------------------------------------------------------------
constexpr int kMaxWeaponLevel = 10;
struct Rules {
    // kills for levels 2..10 (level 1 is 0 kills)
    uint32_t levelKills[kMaxWeaponLevel - 1] = {25, 75, 150, 250, 400, 600, 850, 1150, 1500};
    // the weapon level each tier needs, and the factor it gives
    unsigned clipLevel[2] = {3, 7}, damageLevel[2] = {5, 10};
    float clipFactor[2] = {1.25f, 1.5f}, damageFactor[2] = {1.10f, 1.20f};
};
inline unsigned WeaponLevel(const Rules& r, uint32_t kills) {
    unsigned level = 1;
    for (uint32_t need : r.levelKills) level += kills >= need;
    return level;
}
// kills that reach the next level, 0 at the top
inline uint32_t KillsForNext(const Rules& r, unsigned level) {
    return level >= 1 && level < static_cast<unsigned>(kMaxWeaponLevel) ? r.levelKills[level - 1] : 0;
}
inline unsigned ClipTier(const Rules& r, unsigned level) { return (level >= r.clipLevel[0]) + (level >= r.clipLevel[1]); }
inline unsigned DamageTier(const Rules& r, unsigned level) { return (level >= r.damageLevel[0]) + (level >= r.damageLevel[1]); }
inline float ClipFactor(const Rules& r, unsigned level) {
    const unsigned t = ClipTier(r, level);
    return t ? r.clipFactor[t - 1] : 1.0f;
}
inline float DamageFactor(const Rules& r, unsigned level) {
    const unsigned t = DamageTier(r, level);
    return t ? r.damageFactor[t - 1] : 1.0f;
}
// A clip size after the bonus: never smaller than before, at least one more round when a tier is on.
inline uint32_t BoostedClip(uint32_t clip, float factor) {
    if (!(factor > 1.0f) || !clip) return clip;
    const uint32_t more = static_cast<uint32_t>(static_cast<float>(clip) * factor);
    return more > clip ? more : clip + 1;
}

// "25,75,150" into out[n]; false (out untouched) unless exactly n rising positive numbers.
inline bool ParseRising(const char* text, uint32_t* out, int n) {
    uint32_t v[16]{};
    int count = 0;
    const char* p = text;
    while (p && *p && count < 16) {
        char* end = nullptr;
        const unsigned long x = std::strtoul(p, &end, 10);
        if (end == p) return false;
        v[count++] = static_cast<uint32_t>(x);
        p = end;
        while (*p == ' ' || *p == ',') ++p;
    }
    if (count != n) return false;
    for (int i = 0; i < n; ++i)
        if (!v[i] || (i && v[i] <= v[i - 1])) return false;
    for (int i = 0; i < n; ++i) out[i] = v[i];
    return true;
}

// --- profile rows of mp_upgrades (UIC 0386), rows 14..32 --------------------------------
// The gun rows 0..13 are left to the weapon levels (1.0 here). Values have no
// offline source (Blaze title settings); these follow the closed beta (1.5) and
// the report's proposals. Row 29 is a flag (stasis fires only if != 1.0), 30/31
// are flags (> 1.0 adds one shot of energy), 25..28 divide incoming damage.
constexpr int kRows = 33;
inline constexpr uint8_t kRowLevel[kRows] = {33, 40, 29, 44, 25, 14, 19, 50, 54, 46, 57, 42, 34, 38, 15, 6, 21,
                                              24, 3,  9,  12, 27, 18, 35, 39, 43, 31, 53, 58, 1,  5,  30, 48};
inline float RowValue(int row) {
    if (row >= 14 && row <= 24) return 1.10f;   // necro melee, ranged, paired
    if (row >= 25 && row <= 28) return 1.20f;   // necro health
    if (row == 29) return 1.5f;                  // player stasis
    if (row == 30 || row == 31) return 2.0f;     // stasis energy
    if (row == 32) return 1.25f;                 // pack stasis resistance
    return 1.0f;
}
inline float ProfileUpgrade(int row, unsigned level) {
    if (row < 14 || row >= kRows) return 1.0f;
    return level >= kRowLevel[row] ? RowValue(row) : 1.0f;
}

// --- profile unlocks -----------------------------------------------------------------
// The profile level each gun and gadget unlocks at, in the weapon select, the survival
// shop and for the gadget keys. [Unlocks] in DS2United.ini overrides every one by key;
// the suits' defaults come with their rows (generated_loadout_rows.h, the stock lua).
struct UnlockDefault { const char* key; unsigned level; };
inline constexpr UnlockDefault kUnlockDefaults[] = {
    {"PlasmaCutter", 0}, {"PulseRifle", 0}, {"RivetGun", 0}, {"Flamethrower", 4}, {"LineGun", 7}, {"ForceGun", 11},
    {"Ripper", 14},      {"SeekerRifle", 17}, {"Detonator", 22}, {"Javelin", 28}, {"ContactBeam", 35}, {"HandCannon", 60},
    {"Turret", 8},       {"StasisMine", 15}, {"HealField", 25},
};
inline unsigned DefaultUnlock(const char* key) {
    for (const auto& d : kUnlockDefaults)
        if (key && !std::strcmp(d.key, key)) return d.level;
    return 0;
}
// The weapon select's backend rows (0 the Pulse Rifle, never listed; 1..6 native, 7..11 ours).
inline constexpr const char* kBackendKeys[] = {"PulseRifle", "PlasmaCutter", "RivetGun", "LineGun", "ForceGun", "SeekerRifle",
                                               "Javelin",    "Ripper",       "Flamethrower", "ContactBeam", "Detonator", "HandCannon"};
constexpr int kBackendRows = static_cast<int>(sizeof(kBackendKeys) / sizeof(kBackendKeys[0]));
inline const char* BackendKey(int backend) { return backend >= 0 && backend < kBackendRows ? kBackendKeys[backend] : nullptr; }
constexpr unsigned kMaxUnlockLevel = 60;

// --- survival XP (host director) -------------------------------------------------------
// Events, for the dedup key and the log: one wave end, the unfinished wave at a
// run end, and the clock modes' results.
enum Event : uint8_t { kWaveEnd = 1, kRunEnd = 2, kMercsResult = 3, kObjectiveStep = 4, kObjectiveClear = 5, kZoneHeld = 6 };
inline const char* EventName(unsigned e) {
    switch (e) {
    case kWaveEnd: return "wave";
    case kRunEnd: return "run end";
    case kMercsResult: return "mercenaries";
    case kObjectiveStep: return "objective step";
    case kObjectiveClear: return "objective cleared";
    case kZoneHeld: return "zone held";
    default: return "?";
    }
}
// flags: bit 0 the host's run used a host tool; bits 8..15 the event
constexpr uint32_t kTaintHost = 1;
inline uint32_t Flags(Event e, bool tainted) { return (static_cast<uint32_t>(e) << 8) | (tainted ? kTaintHost : 0); }
inline unsigned EventOf(uint32_t flags) { return (flags >> 8) & 0xFF; }

// The retail MP points (deadspace.fandom.com, Dead Space 2 Multiplayer): a necromorph
// kill 100, a human kill 300, an assist 25, an objective 400-500, a round won 1000,
// both rounds +2000. Survival takes the same scale: a kill 100 (a heavy one 300, a
// swarm baby 25), wave 10 cleared 1000 (a won round), a boss wave +2000.
struct XpRules { unsigned kill = 100, heavyKill = 300, swarmKill = 25, wave = 100, waveCap = 40, boss = 2000, mercsDivisor = 8, objectiveStep = 500,
                 objectiveClear = 3000, zone = 500; };
inline uint32_t WaveXp(const XpRules& r, unsigned wave, bool boss) {
    const unsigned w = wave < r.waveCap ? wave : r.waveCap;
    return r.wave * w + (boss ? r.boss : 0);
}
enum class KillSize : uint8_t { Swarm, Normal, Heavy };
inline uint32_t KillXp(const XpRules& r, KillSize size, bool enhanced) {
    const uint32_t xp = size == KillSize::Heavy ? r.heavyKill : size == KillSize::Swarm ? r.swarmKill : r.kill;
    return enhanced ? xp * 2 : xp;
}

// Payment dedup on a client: (run, wave or step, event), last 64 kept.
struct Paid {
    uint64_t keys[64]{};
    unsigned next = 0;
    static uint64_t Key(uint32_t run, uint32_t wave, unsigned event) {
        return (static_cast<uint64_t>(run) << 32) | (static_cast<uint64_t>(wave & 0xFFFFFF) << 8) | (event & 0xFF);
    }
    // true the first time
    bool Take(uint32_t run, uint32_t wave, unsigned event) {
        const uint64_t k = Key(run, wave, event) | 1ull << 63;
        for (uint64_t x : keys)
            if (x == k) return false;
        keys[next++ % 64] = k;
        return true;
    }
};

// --- text ------------------------------------------------------------------------------
// Percent of the way to the next profile level (100 at the top).
inline unsigned PercentToNext(uint32_t xp, const uint32_t* levelScore, unsigned levels) {
    unsigned level = 0;
    for (unsigned i = 0; i < levels; ++i) level += levelScore[i] <= xp;
    if (!level || level >= levels) return 100;
    const uint32_t from = levelScore[level - 1], to = levelScore[level];
    return to > from ? static_cast<unsigned>((static_cast<uint64_t>(xp - from) * 100) / (to - from)) : 100;
}
}  // namespace coop_progress
