// Dead Space 2 United by Richard Hafer.
#pragma once
// Versus match traffic (runtime_coop_versus.inc): pure logic, no game memory.
// The host owns the match: scoring events, the end of each round, the side
// swap, the end of the match and the respawn points. Every packet goes out a
// few times; a receiver takes each (sender, seq) once.
#include <cstdint>
#include <cstring>

namespace coop_versus {
constexpr uint32_t kMagic = 0x53535256;   // 'VRSS'
constexpr uint8_t kVersion = 1;
enum Kind : uint8_t {
    kScoreAsk = 1,      // client -> host: a scoring event computed on the client
    kScore = 2,         // host -> all: a scoring event to replay
    kRoundEnd = 3,      // host -> all: round a ended, team b won
    kSwap = 4,          // host -> all: sides swap, round a starts
    kMatchOver = 5,     // host -> all: back to the menu
    kRespawnAsk = 6,    // client -> host: queue me for a respawn at pos
    kRespawnDelay = 7,  // host -> one client (by ref): respawn in a ms
    kSpawnPoint = 8,    // host -> one client (by ref): the point's GUID
};
inline const char* KindName(uint8_t k) {
    switch (k) {
    case kScoreAsk: return "score_ask";
    case kScore: return "score";
    case kRoundEnd: return "round_end";
    case kSwap: return "swap";
    case kMatchOver: return "match_over";
    case kRespawnAsk: return "respawn_ask";
    case kRespawnDelay: return "respawn_delay";
    case kSpawnPoint: return "spawn_point";
    default: return "?";
    }
}
// Host-only kinds are taken from machine 0 only; asks only by the host.
inline bool FromHostOnly(uint8_t k) { return k == kScore || k == kRoundEnd || k == kSwap || k == kMatchOver || k == kRespawnDelay || k == kSpawnPoint; }

#pragma pack(push, 1)
struct Packet {
    uint32_t magic = kMagic;
    uint8_t version = kVersion, kind = 0, machine = 0, round = 0;
    uint32_t token = 0, seq = 0;
    uint64_t ref = 0, other = 0;
    int32_t a = 0, b = 0;
    float pos[4]{};
    uint32_t guid[4]{};
};
#pragma pack(pop)
static_assert(sizeof(Packet) == 72, "versus packet size");

inline bool Parse(const void* data, int length, Packet& out) {
    if (length != static_cast<int>(sizeof(Packet))) return false;
    std::memcpy(&out, data, sizeof(Packet));
    return out.magic == kMagic;
}

// Seen sequence numbers of one sender: the newest and the 63 before it.
struct SeqWindow {
    uint32_t top = 0;
    uint64_t bits = 0;
    bool Fresh(uint32_t seq) {
        if (!seq) return false;
        if (!top || seq > top) {
            const uint32_t shift = top ? seq - top : 64;
            bits = shift >= 64 ? 0 : bits << shift;
            bits |= 1;
            top = seq;
            return true;
        }
        const uint32_t back = top - seq;
        if (back >= 64) return false;   // too old to tell: dropped
        const uint64_t bit = 1ull << back;
        if (bits & bit) return false;
        bits |= bit;
        return true;
    }
};

// Scoring event types (ScoreBoardEntryImpl::LogScoringEvent): objective
// completion fires on every machine, the streaks inside the replay.
constexpr uint32_t kTypeEnemyKill = 2, kTypeObjective = 11, kTypeTeamObjective = 12, kTypeStreakFirst = 19, kTypeStreakLast = 21;
inline bool IsStreak(uint32_t type) { return type >= kTypeStreakFirst && type <= kTypeStreakLast; }
inline bool IsObjective(uint32_t type) { return type == kTypeObjective || type == kTypeTeamObjective; }

// The points per combat-text event: GetMPTuningValue hashes (from Blaze title
// settings natively, so absent offline) and our defaults, the retail values where
// the wiki lists them (deadspace.fandom.com, Dead Space 2 Multiplayer): human's kill
// 100, necro's kill of a human 300, assists 25 / 150, objective 400-500, heal and
// rescue 20, team kill -20, suicide -300 / -100.
struct Points { uint32_t hash; const char* key; int value; };
inline Points kPoints[] = {
    {0x8ECD3FEB, "HumanKill", 100},     {0xE48BA8AF, "NecroKill", 300},      {0x718DC336, "HumanAssist", 25},
    {0x2F0CC072, "NecroAssist", 150},   {0xC3C9AB53, "Rescue", 20},          {0x5FF6040C, "HumanStreak5", 50},
    {0xA192FE28, "HumanStreak10", 100}, {0xA193FE67, "HumanStreak20", 200},  {0xC079B248, "NecroStreak5", 50},
    {0x1036DEEC, "NecroStreak10", 100}, {0x1037DF2B, "NecroStreak20", 200},  {0x637E3FE0, "Objective", 400},
    {0x2B46F69C, "TeamObjective", 100}, {0xBE3A7B68, "PairedDamage", 1},     {0x18E203C8, "Heal", 20},
    {0x5BE3A67B, "TeamKill", -20},      {0x2D1558C1, "HumanSuicide", -300},  {0xCC91AA85, "NecroSuicide", -100},
    {0x80D0AE7A, "HumanDouble", 0},     {0x05EC8AAD, "HumanTriple", 0},      {0xFA45DD24, "HumanQuad", 0},
    {0xD68F173E, "NecroDouble", 0},     {0x5BAAF371, "NecroTriple", 0},      {0x500445E8, "NecroQuad", 0},
};
inline const Points* FindPoints(uint32_t hash) {
    for (const Points& p : kPoints)
        if (p.hash == hash) return &p;
    return nullptr;
}

// The match: two rounds with the sides swapped, the host's clock between them.
enum class Stage : uint8_t { Playing, RoundOver, MatchOver };
struct Match {
    Stage stage = Stage::Playing;
    uint8_t round = 1;          // 1 or 2
    int8_t winner[3] = {-1, -1, -1};
    uint32_t endedAt = 0;
    bool swapSent = false, overSent = false;
    // A round ended; false when it was already over.
    bool End(uint8_t r, int team, uint32_t now) {
        if (r < 1 || r > 2 || stage != Stage::Playing || r != round) return false;
        winner[r] = static_cast<int8_t>(team);
        stage = Stage::RoundOver;
        endedAt = now;
        return true;
    }
    // Host: what is due now. 1 = swap, 2 = match over, 0 = nothing.
    int Due(uint32_t now, uint32_t swapMs, uint32_t resultsMs) const {
        if (stage != Stage::RoundOver) return 0;
        if (round == 1 && !swapSent && now - endedAt >= swapMs) return 1;
        if (round == 2 && !overSent && now - endedAt >= swapMs + resultsMs) return 2;
        return 0;
    }
    void Swap() {
        if (round != 1) return;
        round = 2;
        stage = Stage::Playing;
        swapSent = true;
    }
    void Over() { stage = Stage::MatchOver; overSent = true; }
};
}  // namespace coop_versus
