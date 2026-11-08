// Dead Space 2 United by Richard Hafer.
#pragma once
// Pure logic of the multiplayer-map visit (runtime_coop_mpmap.inc): the double
// key press, map names, GUID text, the per-map entity table lookup, the stream
// set member filter, level manager state names, the loading watchdog, the visit
// phases and the spawn point choice. No game memory is touched here.
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace coop_mpmap {

// --- maps ----------------------------------------------------------------------
// Every map the survival menu can start. The index is the bit in
// MapEntity::maps and the number the lobby sends, so new maps go at the end.
// Shipped: in the game's own list (LoadMapInfo 0x00A99800). Outbreak: the
// console DLC maps, PC format in DS2DAT12.DAT; the menu appends them when that
// archive is mounted. The level and its stream list are "<name>", "<name>.str".
enum class MapKind : uint8_t { Shipped, Outbreak };
struct MapInfo { const char* name; const char* label; MapKind kind; };
constexpr MapInfo kMapTable[] = {
    {"mp_bombRun", "titan mines", MapKind::Shipped},
    {"mp_detonate", "fuel core", MapKind::Shipped},
    {"mp_escape", "escape", MapKind::Shipped},
    {"mp_gov", "marker lab", MapKind::Shipped},
    {"mp_solar", "solar array", MapKind::Shipped},
    {"pdlc_mp_mall", "concourse", MapKind::Outbreak},
    {"pdlc_mp_school", "academy", MapKind::Outbreak},
};
constexpr unsigned kMapCount = sizeof(kMapTable) / sizeof(kMapTable[0]);
constexpr unsigned kMaxMaps = 32;
static_assert(kMapCount <= kMaxMaps, "MapEntity::maps is a 32 bit mask");
inline const char* MapName(int index) { return index >= 0 && index < static_cast<int>(kMapCount) ? kMapTable[index].name : "?"; }
inline const char* MapLabel(int index) { return index >= 0 && index < static_cast<int>(kMapCount) ? kMapTable[index].label : "?"; }

inline char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
inline bool EqualNoCase(const char* a, const char* b) {
    if (!a || !b) return false;
    for (; *a && *b; ++a, ++b) if (Lower(*a) != Lower(*b)) return false;
    return *a == *b;
}
inline bool StartsWithNoCase(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    for (; *prefix; ++s, ++prefix) if (!*s || Lower(*s) != Lower(*prefix)) return false;
    return true;
}
// An MP map stream: "mp_..." (the shipped maps and developer ones) or
// "pdlc_mp_..." (the Outbreak maps), with something after the prefix.
inline bool IsMpMap(const char* name) {
    if (StartsWithNoCase(name, "mp_")) return name[3] != 0;
    if (StartsWithNoCase(name, "pdlc_mp_")) return name[8] != 0;
    return false;
}
inline int MapIndex(const char* name) {
    for (unsigned i = 0; i < kMapCount; ++i) if (EqualNoCase(name, kMapTable[i].name)) return static_cast<int>(i);
    return -1;
}

// --- the archives ----------------------------------------------------------------
// The DAT name hash: sdbm over the lowercased path with backslashes. A map's
// main stream set is "<name>\<name>.str".
inline uint32_t HashStep(uint32_t h, char c) {
    const unsigned char b = static_cast<unsigned char>(c == '/' ? '\\' : Lower(c));
    return b + (h << 6) + (h << 16) - h;
}
inline uint32_t ArchiveHash(const char* path) {
    uint32_t h = 0;
    for (; path && *path; ++path) h = HashStep(h, *path);
    return h;
}
inline uint32_t MapStreamHash(const char* name) {
    uint32_t h = 0;
    for (const char* p = name; p && *p; ++p) h = HashStep(h, *p);
    h = HashStep(h, '\\');
    for (const char* p = name; p && *p; ++p) h = HashStep(h, *p);
    for (const char* p = ".str"; *p; ++p) h = HashStep(h, *p);
    return h;
}
// A BIGH archive header: 'BIGH', id, count, data offset, all big endian. Stock
// archives put the data at 16 + count*12 + 8 (an 8 byte trailer); Mark's
// DS2DAT12 has no trailer (16 + count*12), and the game mounts it anyway, so
// only a data offset inside the table is refused. Returns the entry count, or
// -1 when the 16 bytes are no header.
inline uint32_t BigEndian32(const unsigned char* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline long BigCount(const unsigned char* header) {
    if (header[0] != 'B' || header[1] != 'I' || header[2] != 'G' || header[3] != 'H') return -1;
    const uint32_t count = BigEndian32(header + 8);
    if (count > 1000000u || BigEndian32(header + 12) < 16u + count * 12u) return -1;
    return static_cast<long>(count);
}
// The entry table follows the header: {offset, size, hash} per entry, sorted by
// hash (the game binary-searches it; a hash may repeat).
inline bool BigHas(const unsigned char* entries, uint32_t count, uint32_t hash) {
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const uint32_t h = BigEndian32(entries + mid * 12u + 8u);
        if (h == hash) return true;
        if (h < hash) lo = mid + 1; else hi = mid;
    }
    return false;
}

// --- GUIDs ---------------------------------------------------------------------
inline int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = Lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
// "XXXXXXXX-XXXXXXXX-XXXXXXXX-XXXXXXXX", the catalog's and the INI's spelling.
inline bool ParseGuid(const char* text, uint32_t out[4]) {
    if (!text) return false;
    for (unsigned w = 0; w < 4; ++w) {
        uint32_t value = 0;
        for (unsigned d = 0; d < 8; ++d) {
            const int h = HexDigit(*text++);
            if (h < 0) return false;
            value = value << 4 | static_cast<uint32_t>(h);
        }
        out[w] = value;
        if (w < 3 && *text++ != '-') return false;
    }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    return *text == 0;
}
inline bool NullGuid(const uint32_t* g) { return !(g[0] | g[1] | g[2] | g[3]); }
// The engine's "no GUID" value, which Set::SetDesired skips like a null one.
inline bool InvalidGuid(const uint32_t* g) {
    return g[0] == 0xBADBADBAu && g[1] == 0xBEEFBEEFu && g[2] == 0xEAC15A55u && g[3] == 0x91100911u;
}
// The stream lookup key Set::SetDesired builds before asking the stream
// manager (0x00758CF9): h = h * 33 + word, over the four words.
inline uint32_t StreamHash(const uint32_t* g) {
    uint32_t h = g[0];
    for (unsigned i = 1; i < 4; ++i) h = h * 33u + g[i];
    return h;
}
inline int Compare(const uint32_t* a, const uint32_t* b) {
    for (unsigned i = 0; i < 4; ++i) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

// --- the per-map entity table (generated_mpmap_block.h) --------------------------
enum Group : uint8_t { kWatch = 0, kEndRound = 1, kRoundTimer = 2, kObjectiveMarkers = 3, kTeamLogic = 4, kMpSlashers = 5, kMpGlobals = 6, kGroups = 7 };
inline const char* GroupName(unsigned g) {
    static const char* const names[kGroups] = {"watch", "end_round", "round_timer", "objective_markers", "team_logic", "mp_slashers", "mp_globals"};
    return g < kGroups ? names[g] : "?";
}
struct MapEntity {
    uint32_t words[4];
    uint8_t group;
    uint32_t maps;         // bit i = kMapTable[i]
    uint8_t spawnOnLoad;   // spawn_flags 1 somewhere: the stream spawns it itself
    const char* cls;
};
// Binary search over a table sorted by GUID words.
inline const MapEntity* Find(const MapEntity* table, size_t count, const uint32_t* guid) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int c = Compare(table[mid].words, guid);
        if (c == 0) return &table[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}
inline bool Sorted(const MapEntity* table, size_t count) {
    for (size_t i = 1; i < count; ++i) if (Compare(table[i - 1].words, table[i].words) >= 0) return false;
    return true;
}
// Blocked: a known entity of a blocking group that is on this map (or on any
// MP map when the map is not one of the five), and whose group is switched on.
inline bool Blocks(const MapEntity* e, int mapIndex, uint32_t groupMask) {
    if (!e || e->group == kWatch || !(groupMask >> e->group & 1u)) return false;
    return mapIndex < 0 || (e->maps >> mapIndex & 1u);
}

// --- the stream set members of a SetDesired call ---------------------------------
// known[i]: the stream manager resolved member i. Unknown members are dropped
// when dropUnknown is set; null and invalid GUIDs stay (the engine skips them).
// Returns the kept count; out and outHidden must hold count entries.
inline unsigned FilterMembers(const uint32_t (*in)[4], const unsigned char* hidden, unsigned count, const bool* known, bool dropUnknown,
                              uint32_t (*out)[4], unsigned char* outHidden, unsigned* dropped) {
    unsigned n = 0, gone = 0;
    for (unsigned i = 0; i < count; ++i) {
        const bool skipNatively = NullGuid(in[i]) || InvalidGuid(in[i]);
        if (dropUnknown && !skipNatively && !known[i]) { ++gone; continue; }
        for (unsigned w = 0; w < 4; ++w) out[n][w] = in[i][w];
        outHidden[n] = hidden ? hidden[i] : 0;
        ++n;
    }
    if (dropped) *dropped = gone;
    return n;
}

// --- level manager states (LevelLoadState.h, Xbox enumerator names) ---------------
constexpr uint32_t kRunning = 16;
inline const char* StateName(uint32_t s) {
    static const char* const names[] = {"NO_LEVEL", "LOAD_TEXT_ASSETS", "LOAD_LOADING_SCREEN", "FETCH_MP_TUNING", "SWAP_DISC",
                                        "LOAD_GLOBAL_SHARED", "LOAD_SHARED_TOC", "LOAD_SHARED", "LOAD_PLAYER_ANIMS",
                                        "LOAD_MPWEAPONSTREAMS", "LOAD_MPUNIFORMSTREAMS", "LOAD_CHECKPOINT", "LOADING_COMPLETE",
                                        "WAIT_FOR_REFLECTABLES", "LOAD_MPSKINSTREAMS", "WAIT_FOR_COMMAND", "RUNNING"};
    return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

// --- the double press -----------------------------------------------------------
// The first press arms, a second one within windowMs confirms. A key held down
// counts once.
struct DoublePress {
    enum Event { kNone, kArmed, kConfirmed, kExpired };
    uint32_t windowMs = 3000;
    bool wasDown = false, armed = false;
    uint32_t armedAt = 0;
    Event Update(bool down, uint32_t now) {
        const bool edge = down && !wasDown;
        wasDown = down;
        Event event = kNone;
        if (armed && now - armedAt > windowMs) { armed = false; event = kExpired; }
        if (!edge) return event;
        if (armed) { armed = false; return kConfirmed; }
        armed = true;
        armedAt = now;
        return kArmed;
    }
    void Reset() { armed = false; }
};

// --- the loading watchdog ------------------------------------------------------
// Progress is any change of a signature the caller builds (level manager state,
// stream requests, TOC loads, loaded stream count). Stuck reports come once per
// limit, at most maxReports times per load.
struct Watchdog {
    bool running = false;
    uint32_t signature = 0, progressAt = 0, reportedAt = 0;
    unsigned reports = 0, maxReports = 4;
    void Start(uint32_t sig, uint32_t now) { running = true; signature = sig; progressAt = now; reports = 0; reportedAt = 0; }
    void Stop() { running = false; }
    bool Observe(uint32_t sig, uint32_t now) {
        if (!running || sig == signature) return false;
        signature = sig;
        progressAt = now;
        reports = 0;
        return true;
    }
    uint32_t QuietMs(uint32_t now) const { return running ? now - progressAt : 0; }
    bool Stuck(uint32_t now, uint32_t limitMs) {
        if (!running || !limitMs || now - progressAt < limitMs || reports >= maxReports) return false;
        if (reports && now - reportedAt < limitMs) return false;
        ++reports;
        reportedAt = now;
        return true;
    }
};

// --- the visit -----------------------------------------------------------------
// Leaving: the request went out, the old level still runs (the map name in the
// main loop is the new one from the moment of the request, so the phases wait
// for the level manager to leave RUNNING first). Loading: the new level is being
// built. Arrived: an MP map runs. Returning: the way back was asked for.
// Saves stay held from Leaving until a non-MP level runs again.
enum class Phase { Idle, Leaving, Loading, Arrived, Returning };
inline const char* PhaseName(Phase p) {
    switch (p) {
    case Phase::Idle: return "idle";
    case Phase::Leaving: return "leaving";
    case Phase::Loading: return "loading";
    case Phase::Arrived: return "arrived";
    case Phase::Returning: return "returning";
    }
    return "?";
}
struct Visit {
    Phase phase = Phase::Idle;
    bool sawLoad = false;
    uint32_t phaseAt = 0, epochs = 0;
    bool Holding() const { return phase != Phase::Idle; }
    void Set(Phase p, uint32_t now) { phase = p; phaseAt = now; }
    void Request(uint32_t now) { sawLoad = false; Set(Phase::Leaving, now); }
    void RequestReturn(uint32_t now) { sawLoad = false; Set(Phase::Returning, now); }
    void Abort(uint32_t now) { Set(Phase::Idle, now); }
    // One sample of the level manager. Returns true when the phase changed.
    bool Observe(uint32_t state, bool mapIsMp, uint32_t now) {
        const Phase before = phase;
        const bool running = state == kRunning;
        if (!running) sawLoad = true;
        switch (phase) {
        case Phase::Idle:
            break;
        case Phase::Leaving:
            if (sawLoad) Set(Phase::Loading, now);
            break;
        case Phase::Loading:
            if (!running) break;
            sawLoad = false;
            if (mapIsMp) { ++epochs; Set(Phase::Arrived, now); }
            else Set(Phase::Idle, now);   // the load ended somewhere else (main menu, a save)
            break;
        case Phase::Arrived:
            if (!running) Set(Phase::Loading, now);   // a restart of the MP map, or a level change of its own
            break;
        case Phase::Returning:
            if (!sawLoad || !running) break;
            sawLoad = false;
            if (mapIsMp) { ++epochs; Set(Phase::Arrived, now); }   // still (again) on an MP map
            else Set(Phase::Idle, now);
            break;
        }
        return phase != before;
    }
};

// --- spawn points --------------------------------------------------------------
inline bool UsablePoint(const float* p) {
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) &&
           std::fabs(p[0]) < 100000.0f && std::fabs(p[1]) < 100000.0f && std::fabs(p[2]) < 100000.0f;
}
// The preferred point when it is usable, else the first usable one not in
// usedMask, else the first usable one at all. -1 when none is.
inline int ChoosePoint(const float (*pos)[3], unsigned count, int preferred, uint32_t usedMask) {
    if (preferred >= 0 && static_cast<unsigned>(preferred) < count && UsablePoint(pos[preferred])) return preferred;
    int any = -1;
    for (unsigned i = 0; i < count && i < 32; ++i) {
        if (!UsablePoint(pos[i])) continue;
        if (!(usedMask >> i & 1u)) return static_cast<int>(i);
        if (any < 0) any = static_cast<int>(i);
    }
    return any;
}

}  // namespace coop_mpmap
