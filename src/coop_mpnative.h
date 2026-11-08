// Dead Space 2 United by Richard Hafer.
#pragma once
// Versus on the game's own multiplayer systems (analysis/DS2_MP_INFRASTRUCTURE_2026-10-11.md):
// the roster every machine keeps (who plays, on which team, with which weapon,
// suit and XP), the 64-bit player refs the native systems key on, the team split
// and the MP level from XP. Pure logic, no game memory.
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace coop_mpnative {
constexpr uint32_t kMagic = 0x564E504D;   // 'MPNV'
constexpr uint8_t kVersion = 1;
constexpr unsigned kMaxPlayers = 8;
enum Team : uint8_t { kHuman = 0, kNecro = 1, kNoTeam = 2 };
enum Kind : uint8_t { kOwn = 1, kRoster = 2 };

// The native systems key every player on a non-zero 64-bit ref (a Blaze player
// id offline is 0): ours carry 'DS2M' in the high word and machine + 1 below.
constexpr uint64_t kRefBase = 0x4453324D00000000ull;
inline uint64_t RefFor(unsigned machine) { return kRefBase | (machine + 1u); }
inline int MachineOfRef(uint64_t ref) {
    const uint32_t low = static_cast<uint32_t>(ref);
    return (ref >> 32) == (kRefBase >> 32) && low >= 1 && low <= 256 ? static_cast<int>(low - 1) : -1;
}

#pragma pack(push, 1)
struct Entry { uint8_t machine, team, weapon, skin; uint32_t xp; char name[24]; };
struct Packet { uint32_t magic; uint8_t version, kind, count, round; uint32_t token, serial; Entry entries[kMaxPlayers]; };
#pragma pack(pop)
static_assert(sizeof(Entry) == 32, "roster entry size");
static_assert(sizeof(Packet) == 272, "roster packet size");

// Round 2 plays with the sides swapped; the roster keeps the round 1 split.
inline uint8_t EffectiveTeam(uint8_t team, uint8_t round) {
    if (team > kNecro) return kNoTeam;
    return round == 2 ? static_cast<uint8_t>(team ^ 1u) : team;
}

struct Roster {
    Entry entries[kMaxPlayers]{};
    unsigned count = 0;
    uint32_t serial = 0;   // the host bumps it on every change

    const Entry* Find(unsigned machine) const {
        for (unsigned i = 0; i < count; ++i) if (entries[i].machine == machine) return &entries[i];
        return nullptr;
    }
    // Adds or updates by machine; the team stays the roster's own. False when full.
    bool Upsert(const Entry& e, bool* changed = nullptr) {
        for (unsigned i = 0; i < count; ++i) {
            if (entries[i].machine != e.machine) continue;
            Entry merged = e;
            merged.team = entries[i].team;
            if (std::memcmp(&merged, &entries[i], sizeof(Entry)) != 0) {
                entries[i] = merged;
                if (changed) *changed = true;
            }
            return true;
        }
        if (count >= kMaxPlayers) return false;
        entries[count] = e;
        entries[count].team = kNoTeam;
        ++count;
        if (changed) *changed = true;
        return true;
    }
    bool Remove(unsigned machine) {
        for (unsigned i = 0; i < count; ++i) {
            if (entries[i].machine != machine) continue;
            for (unsigned j = i + 1; j < count; ++j) entries[j - 1] = entries[j];
            entries[--count] = Entry{};
            return true;
        }
        return false;
    }
    unsigned TeamCount(uint8_t team, uint8_t round) const {
        unsigned n = 0;
        for (unsigned i = 0; i < count; ++i) n += EffectiveTeam(entries[i].team, round) == team;
        return n;
    }
};

// The host's split before a round: by machine number, the first `humansWanted`
// (0: half, rounded up) are humans, the rest necromorphs. True when a team changed.
inline bool AssignTeams(Roster& r, unsigned humansWanted) {
    unsigned order[kMaxPlayers];
    for (unsigned i = 0; i < r.count; ++i) order[i] = i;
    for (unsigned i = 1; i < r.count; ++i)
        for (unsigned j = i; j > 0 && r.entries[order[j]].machine < r.entries[order[j - 1]].machine; --j) {
            const unsigned t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    const unsigned humans = humansWanted ? (humansWanted < r.count ? humansWanted : r.count) : (r.count + 1) / 2;
    bool changed = false;
    for (unsigned k = 0; k < r.count; ++k) {
        const uint8_t team = k < humans ? kHuman : kNecro;
        if (r.entries[order[k]].team != team) {
            r.entries[order[k]].team = team;
            changed = true;
        }
    }
    return changed;
}

// A player who reaches the roster after the round began (every joiner: the
// side channel only runs in the level) gets the side the lobby showed for its
// machine: the same split over the machines that launched. A machine the lobby
// did not have takes the side with fewer players, a tie going to the humans.
inline uint8_t LateTeam(const unsigned* launched, unsigned n, unsigned machine, unsigned humansWanted, unsigned humansNow, unsigned necrosNow) {
    unsigned sorted[kMaxPlayers];
    unsigned m = 0;
    for (unsigned i = 0; i < n && m < kMaxPlayers; ++i) sorted[m++] = launched[i];
    for (unsigned i = 1; i < m; ++i)
        for (unsigned j = i; j > 0 && sorted[j] < sorted[j - 1]; --j) {
            const unsigned t = sorted[j];
            sorted[j] = sorted[j - 1];
            sorted[j - 1] = t;
        }
    const unsigned humans = humansWanted ? (humansWanted < m ? humansWanted : m) : (m + 1) / 2;
    for (unsigned k = 0; k < m; ++k)
        if (sorted[k] == machine) return k < humans ? kHuman : kNecro;
    if (humansWanted) return humansNow < humansWanted ? kHuman : kNecro;
    return humansNow <= necrosNow ? kHuman : kNecro;
}

// The MP level: rows of mp_levels (frontend UICSV 0385, 60 rows) whose score is
// at most the XP, at least 1.
constexpr uint32_t kLevelScore[60] = {
    0,       5000,    10500,   16500,   23000,   30250,   38250,   47000,   56500,   66750,   77750,   89500,
    102250,  116000,  130750,  146500,  163250,  181000,  199750,  219500,  240500,  262750,  286250,  311000,
    337000,  364250,  392750,  422500,  453500,  486000,  520000,  555500,  592500,  631000,  671000,  712500,
    755500,  800000,  846000,  893500,  942750,  993750,  1046500, 1101000, 1157250, 1215250, 1275000, 1336500,
    1399750, 1464750, 1531750, 1600750, 1671750, 1744750, 1819750, 1896750, 1975750, 2056750, 2139750, 2224750};
inline unsigned LevelForXp(uint32_t xp) {
    unsigned level = 0;
    for (uint32_t score : kLevelScore) level += score <= xp;
    return level ? level : 1;
}

// A name for the wire: printable ASCII, at most 23 characters.
inline void CopyName(char (&out)[24], const char* in) {
    size_t n = 0;
    for (const char* c = in; c && *c && n < sizeof(out) - 1; ++c)
        if (*c >= 32 && *c <= 126) out[n++] = *c;
    out[n] = 0;
}
}  // namespace coop_mpnative
