// Dead Space 2 United by Richard Hafer.
// The game's own difficulty in co-op and the game modes (runtime_coop_difficulty.inc),
// without the game: names, the picker's rules, the packet fields and the generation
// rules that keep every machine on the host's value.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace coop_difficulty {
// PlayerSettings::DifficultyOptions, which is also the row of the difficulty screen (SP02).
constexpr int kCount = 5, kCasual = 0, kNormal = 1, kSurvivalist = 2, kZealot = 3, kHardcore = 4;
constexpr uint8_t kNone = 0xFF;   // lobby byte: the host sent none

inline bool Valid(int d) { return d >= 0 && d < kCount; }
inline const char* Name(int d) {
    static const char* const kNames[kCount] = {"casual", "normal", "survivalist", "zealot", "hard core"};
    return Valid(d) ? kNames[d] : "?";
}
// The menu's English labels, for the survival HUD.
inline const char* HudName(int d) {
    static const char* const kNames[kCount] = {"CASUAL", "NORMAL", "SURVIVALIST", "ZEALOT", "HARD CORE"};
    return Valid(d) ? kNames[d] : "";
}

// Our pick on the difficulty screen. A row the screen locked counts only for Hard Core
// with AllowHardcore (the route from the map picker never syncs the unlock).
inline bool PickAllowed(int row, bool rowEnabled, bool allowHardcore) { return Valid(row) && (rowEnabled || (row == kHardcore && allowHardcore)); }

// The SP02 Select is shared by the front end and the in-game screen. A pick we own never
// reaches the native Select: its stale load reason could load a save or start a campaign.
enum class Route { Forward, Own, Refuse };
inline Route RouteSelect(bool frontEnd, bool owned, bool inMatch, bool lockInMatch) {
    if (frontEnd) return owned ? Route::Own : Route::Forward;
    return inMatch && lockInMatch ? Route::Refuse : Route::Forward;
}

// The in-game popup keeps the lowest difficulty of the run (achievements); a launch sets both.
inline int LowestAfterChange(int lowest, int d) { return Valid(lowest) && lowest < d ? lowest : d; }

// --- packet fields ---
inline uint8_t LobbyByte(int d) { return Valid(d) ? static_cast<uint8_t>(d) : kNone; }
inline int FromLobbyByte(int b) { return Valid(b) ? b : -1; }
// CampaignPacket's former pad: difficulty + 1, 0 = none.
inline uint16_t CampaignField(int d) { return Valid(d) ? static_cast<uint16_t>(d + 1) : 0; }
inline int FromCampaignField(unsigned f) { return f >= 1 && f <= static_cast<unsigned>(kCount) ? static_cast<int>(f) - 1 : -1; }
// survnet kind 13 keeps (generation << 8) | difficulty in its value word.
inline uint32_t SurvnetValue(uint32_t generation, int d) { return generation << 8 | LobbyByte(d); }
inline int SurvnetDifficulty(uint32_t value) { return FromLobbyByte(static_cast<int>(value & 0xFF)); }
inline uint32_t SurvnetGeneration(uint32_t value) { return value >> 8; }

#pragma pack(push, 1)
// survnet kind 13, host to clients. The text starts with a 0 byte, so an older build shows nothing.
struct HostWire { uint8_t zero = 0, difficulty = kNone, flags = 0, pad = 0; uint32_t generation = 0, ackToken = 0, ackRequest = 0; };
// survnet kind 14, a client to the host: the change it made in the pause menu.
struct RequestWire { uint8_t zero = 0, difficulty = kNone, pad[2]{}; uint32_t token = 0, request = 0, baseGeneration = 0; };
#pragma pack(pop)
static_assert(sizeof(HostWire) == 16 && sizeof(RequestWire) == 16, "difficulty wire size");
enum HostFlag : uint8_t { kFlagLocked = 1 };   // a match runs: nobody changes it

// --- host ---
// The host's value is the one every machine plays. Each change gets a new generation.
struct Leader {
    uint32_t generation = 0, refreshAt = 0;
    int value = -1;
    // The global as read this tick: true when it moved (send the new generation three times).
    bool Observe(int current) {
        if (!Valid(current) || current == value) return false;
        value = current;
        ++generation;
        return true;
    }
    // A handled request gets a new generation even without a change, so the asker stops waiting.
    void Bump() { ++generation; }
    bool RefreshDue(uint32_t now, uint32_t everyMs) {
        if (!generation || (refreshAt && now - refreshAt < everyMs)) return false;
        refreshAt = now;
        return true;
    }
};
enum class Verdict { Apply, Same, RefusedLocked, RefusedInvalid };
inline Verdict Judge(int requested, int current, bool locked) {
    if (!Valid(requested)) return Verdict::RefusedInvalid;
    if (locked) return Verdict::RefusedLocked;
    return requested == current ? Verdict::Same : Verdict::Apply;
}
// A request arrives three times; each one is handled once.
struct RequestLog {
    struct Entry { uint32_t token = 0, request = 0; };
    Entry entries[8];
    unsigned next = 0;
    // true the first time (and records it), false for a copy
    bool First(uint32_t token, uint32_t request) {
        for (const Entry& e : entries)
            if (e.token == token && e.request == request && token) return false;
        entries[next++ % 8] = Entry{token, request};
        return true;
    }
};

// --- client ---
// The host's word as a client last took it, and the request it waits on.
struct Follower {
    enum class Take { Apply, Stale, Held, Ignored };
    uint32_t hostToken = 0, generation = 0;
    int value = -1;
    bool pending = false;
    int requested = -1;
    uint32_t request = 0, base = 0, sentAt = 0;
    // A host packet. A refresh never takes back a newer change: an older or equal generation
    // is stale, and while our own request waits only its answer counts (or the wait runs out).
    Take OnHost(uint32_t token, uint32_t gen, int d, uint32_t ackToken, uint32_t ackRequest, uint32_t ownToken, uint32_t now, uint32_t holdMs) {
        if (!token || !gen || !Valid(d)) return Take::Ignored;
        if (token != hostToken) {   // another host, or the host started again
            hostToken = token;
            generation = 0;
            value = -1;
            pending = false;
        }
        const bool answer = pending && ackToken == ownToken && ackRequest == request;
        if (pending && !answer && now - sentAt < holdMs) return Take::Held;
        pending = false;
        if (gen <= generation) return Take::Stale;
        generation = gen;
        value = d;
        return Take::Apply;
    }
    // Our change in the pause menu, sent to the host; returns the request number.
    uint32_t Ask(int d, uint32_t now) {
        pending = true;
        requested = d;
        base = generation;
        sentAt = now;
        return ++request;
    }
    // true once when the host never answered within holdMs
    bool Expired(uint32_t now, uint32_t holdMs) {
        if (!pending || now - sentAt < holdMs) return false;
        pending = false;
        return true;
    }
};
// The row picked in the pause menu's difficulty screen; the change it causes is a user change.
struct LocalPick {
    int row = -1;
    uint32_t at = 0;
    void Arm(int r, uint32_t now) { row = r; at = now ? now : 1; }
    bool Matches(int current, uint32_t now, uint32_t windowMs) const { return row >= 0 && at && current == row && now - at < windowMs; }
    void Clear() { row = -1; at = 0; }
};
enum class ClientStep { None, Ask, Restore };
// A client's tick: its own change goes to the host, anything else that moved the global
// (a save's restore) goes back to the host's value.
inline ClientStep DecideClient(int global, int lastSeen, bool userChange, bool pending, int hostValue) {
    if (Valid(global) && global != lastSeen && userChange) return ClientStep::Ask;
    if (!pending && Valid(hostValue) && global != hostValue) return ClientStep::Restore;
    return ClientStep::None;
}

// --- HUD ---
// The separator the line already uses: the game modes' running lines use "|".
inline const char* SeparatorFor(const char* line) { return line && std::strstr(line, "  |  ") ? "  |  " : "  -  "; }
// "<line><separator>ZEALOT" when all of it fits; an empty line or an unknown difficulty stays.
inline bool AppendSuffix(char* line, size_t size, int d) {
    if (!line || !size || !line[0] || !Valid(d)) return false;
    const char* separator = SeparatorFor(line);
    const size_t used = std::strlen(line), add = std::strlen(separator) + std::strlen(HudName(d));
    if (used + add + 1 > size) return false;
    std::snprintf(line + used, size - used, "%s%s", separator, HudName(d));
    return true;
}
}  // namespace coop_difficulty
