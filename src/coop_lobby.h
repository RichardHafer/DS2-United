// Dead Space 2 United by Richard Hafer.
#pragma once
// The main menu lobby for online survival (runtime_coop_lobby.inc): pure logic,
// no game memory. A host announces the map it picked; joiners say hello, get a
// machine number, and launch the same map when the host does. The launch also
// carries the roster, because every machine has to dial every other one: TNL
// does not forward a ghost, so player bodies need a full mesh. Plain UDP on
// [Coop] Port + kLobbyPortOffset, next to the game's TNL port and the data channel.
#include <cstdint>
#include <cstring>

namespace coop_lobby {

constexpr uint32_t kMagic = 0x59424F4C;   // 'LOBY'
// 4: the launch carries the host's difficulty (a byte after the version 3 layout).
// 5: the launch and the campaign packet carry the host's randomizer seed (0 = keep your own).
constexpr uint8_t kVersion = 5;
constexpr int kVersion3Bytes = 120, kVersion4Bytes = 121;
constexpr uint8_t kNoDifficulty = 0xFF, kDifficulties = 5;
constexpr unsigned kLobbyPortOffset = 2000;
// Up to seven joiners: eight players fill the PlayerManager's 8 slots. The host's
// bots take slots too, so the host admits 7 minus its bots (Host::Admit's cap).
constexpr unsigned kMaxMembers = 7;
constexpr uint8_t kNoMap = 0xFF;
// A launch's map byte: an index into coop_mpmap::kMapTable, or a campaign arena as
// kArenaFlag | its index into the generated arena table (kNoMap stays free).
constexpr uint8_t kArenaFlag = 0x80;
inline bool IsArenaCode(int map) { return map >= kArenaFlag && map < kNoMap; }
inline uint8_t ArenaCode(unsigned arenaIndex) { return arenaIndex < kNoMap - kArenaFlag ? static_cast<uint8_t>(kArenaFlag | arenaIndex) : kNoMap; }
inline int ArenaIndexOf(int map) { return IsArenaCode(map) ? map & ~kArenaFlag : -1; }

enum class Type : uint8_t { Hello = 1, Welcome = 2, Launch = 3, Bye = 4, Full = 5, Mismatch = 6 };

#pragma pack(push, 1)
struct RosterEntry { uint32_t ip = 0; uint16_t gamePort = 0; uint8_t machine = 0, pad = 0; };
// A player's card for the lobby list: a joiner's hello carries its own, pingMs
// being its last round trip to the host (0 = not measured yet).
struct Profile { char name[24]{}; uint32_t xp = 0; uint16_t pingMs = 0, pad = 0; };
struct Packet {
    uint32_t magic = kMagic;
    uint8_t version = kVersion;
    uint8_t type = 0;
    uint8_t machine = 0;     // Welcome/Launch: the joiner's machine number
    uint8_t map = kNoMap;    // Launch: index into coop_mpmap::kMapTable
    uint32_t token = 0;      // the host's lobby, so a stale launch is never taken
    uint32_t seq = 0;
    uint16_t gamePort = 0;   // the sender's TNL port ([Coop] Port)
    uint8_t members = 0;     // Welcome/Launch: joiners known to the host
    uint8_t flags = 0;       // Launch: kModeFlag | the host's game mode
    uint32_t build = 0;      // DS2United.asi build; Hello/Launch also carry the randomizer's
    uint32_t randomizer = 0; // DS2RandomizerObserver.asi build (0: not loaded)
    uint32_t reserved = 0;   // Hello: the maps the joiner can load (bit = index into coop_mpmap::kMapTable);
                             // Welcome/Launch: the seq of the hello it answers (the joiner's round trip)
    RosterEntry roster[kMaxMembers];   // Launch: every joiner, ip as the host sees it
    Profile profile;         // Hello: the joiner's card
    uint8_t difficulty = kNoDifficulty;   // Launch: the host's difficulty (0..4)
    uint32_t seed = 0;       // Launch: profile.pad == 1 also enables seed zero
};
#pragma pack(pop)
static_assert(sizeof(Profile) == 32, "lobby profile size");
static_assert(sizeof(Packet) == kVersion4Bytes + 4, "lobby packet size");

// The host's game mode rides in a launch's flags, so every machine loads the map
// for the same mode (Versus opens the map's own round logic at load time).
constexpr uint8_t kModeFlag = 0x80;
inline uint8_t ModeFlags(long mode) { return mode >= 0 && mode < 0x80 ? static_cast<uint8_t>(kModeFlag | mode) : 0; }
inline int ModeFromFlags(uint8_t flags) { return flags & kModeFlag ? flags & 0x7F : -1; }

inline Packet Make(Type type, uint32_t token, uint32_t seq, uint16_t gamePort, uint32_t build, uint32_t randomizer) {
    Packet p;
    p.type = static_cast<uint8_t>(type);
    p.token = token;
    p.seq = seq;
    p.gamePort = gamePort;
    p.build = build;
    p.randomizer = randomizer;
    return p;
}
// A received datagram: exactly one packet with our magic and version.
inline bool Parse(const void* data, int length, Packet& out) {
    if (length != static_cast<int>(sizeof(Packet))) return false;
    std::memcpy(&out, data, sizeof(Packet));
    return out.magic == kMagic && out.version == kVersion && out.type >= 1 && out.type <= 6;
}
// A lobby packet of another version (an older or newer build): its version, else -1.
inline int OtherVersion(const void* data, int length) {
    if (!data || length < 5) return -1;
    uint32_t magic = 0;
    std::memcpy(&magic, data, 4);
    const uint8_t version = static_cast<const uint8_t*>(data)[4];
    return magic == kMagic && version != kVersion ? version : -1;
}
// The bytes of a refusal a joiner of `version` can read: versions 3 and 4 are our layout without
// the fields added since, so their build refusal still arrives. 0 for any other version.
inline int RefusalBytes(int version) { return version == 3 ? kVersion3Bytes : version == 4 ? kVersion4Bytes : 0; }

// The campaign: the host restarts a checkpoint (or loads a level) and the
// joiners load the same map with the same checkpoint. No late join: a joiner
// gets in at the host's next restart.
constexpr uint32_t kCampaignMagic = 0x43424F4C;   // 'LOBC'
#pragma pack(push, 1)
struct CampaignPacket {
    uint32_t magic = kCampaignMagic;
    uint8_t version = kVersion;
    uint8_t machine = 0;
    uint8_t members = 0;
    uint8_t flags = 0;
    uint32_t token = 0, seq = 0, build = 0, randomizer = 0;
    uint16_t gamePort = 0;
    uint16_t difficulty = 0;   // the host's difficulty + 1, 0 = none
    uint32_t checkpoint[4]{};
    char map[64]{};
    char stream[64]{};
    RosterEntry roster[kMaxMembers];
    uint32_t seed = 0;         // flags bit 0 also enables seed zero
};
#pragma pack(pop)
static_assert(sizeof(CampaignPacket) == 232, "campaign packet size");
inline bool ParseCampaign(const void* data, int length, CampaignPacket& out) {
    if (length != static_cast<int>(sizeof(CampaignPacket))) return false;
    std::memcpy(&out, data, sizeof(CampaignPacket));
    out.map[sizeof(out.map) - 1] = 0;
    out.stream[sizeof(out.stream) - 1] = 0;
    return out.magic == kCampaignMagic && out.version == kVersion && out.map[0] != 0;
}

// --- host ----------------------------------------------------------------------
struct Member { bool used = false; uint32_t ip = 0; uint16_t port = 0, gamePort = 0; uint8_t machine = 0; uint32_t lastSeen = 0; uint32_t maps = ~0u; Profile profile{}; };
struct Host {
    Member members[kMaxMembers];
    // The machine number for a hello from ip:port: the one it already has, else
    // the lowest free one; 0 when `cap` joiners are already in. `held` (bit n =
    // machine n) are partners still in the host's running session.
    uint8_t Admit(uint32_t ip, uint16_t port, uint16_t gamePort, uint32_t now, unsigned cap = kMaxMembers, uint32_t held = 0) {
        for (auto& m : members)
            if (m.used && m.ip == ip && m.port == port) { m.lastSeen = now; m.gamePort = gamePort; return m.machine; }
        unsigned heldCount = 0;
        for (uint8_t machine = 1; machine <= kMaxMembers; ++machine) if (held >> machine & 1u) ++heldCount;
        if (Count() + heldCount >= cap) return 0;
        for (uint8_t machine = 1; machine <= kMaxMembers; ++machine) {
            bool taken = (held >> machine & 1u) != 0;
            for (const auto& m : members) if (m.used && m.machine == machine) taken = true;
            if (taken) continue;
            for (auto& m : members) {
                if (m.used) continue;
                m = Member{true, ip, port, gamePort, machine, now};
                return machine;
            }
        }
        return 0;
    }
    void Forget(uint32_t ip, uint16_t port) {
        for (auto& m : members) if (m.used && m.ip == ip && m.port == port) m = Member{};
    }
    // Joiners that stopped saying hello. Not used after the launch: in the level
    // the data channel and TNL own the connection.
    unsigned Expire(uint32_t now, uint32_t afterMs) {
        unsigned n = 0;
        for (auto& m : members)
            if (m.used && now - m.lastSeen > afterMs) { m = Member{}; ++n; }
        return n;
    }
    unsigned Count() const {
        unsigned n = 0;
        for (const auto& m : members) if (m.used) ++n;
        return n;
    }
    // The maps a joiner can load, from its hello (every hello: a joiner who
    // copies DS2DAT12.DAT in and restarts says so the next time).
    void NoteMaps(uint32_t ip, uint16_t port, uint32_t maps) {
        for (auto& m : members) if (m.used && m.ip == ip && m.port == port) m.maps = maps;
    }
    // The joiner's card from its hello; true when it changed.
    bool NoteProfile(uint32_t ip, uint16_t port, const Profile& p) {
        for (auto& m : members) {
            if (!m.used || m.ip != ip || m.port != port) continue;
            Profile clean = p;
            clean.name[sizeof(clean.name) - 1] = 0;
            clean.pad = 0;
            if (std::memcmp(&clean, &m.profile, sizeof(Profile)) == 0) return false;
            m.profile = clean;
            return true;
        }
        return false;
    }
    // The first joiner who cannot load map `index` (its machine number), 0 when all can.
    uint8_t MissingMap(int index) const {
        if (index < 0 || index >= 32) return 0;
        for (const auto& m : members) if (m.used && !(m.maps >> index & 1u)) return m.machine;
        return 0;
    }
    void FillRoster(Packet& p) const {
        unsigned n = 0;
        for (const auto& m : members)
            if (m.used && n < kMaxMembers) p.roster[n++] = RosterEntry{m.ip, m.gamePort, m.machine, 0};
        for (; n < kMaxMembers; ++n) p.roster[n] = RosterEntry{};
    }
};

// --- the lobby list --------------------------------------------------------------
// The host sends every joiner the whole list several times a second while no map
// is launched: the map picked for the lobby screen (kNoMap: the host is not on it),
// the game mode, the countdown and the eight rows of the native player list.
constexpr uint32_t kStateMagic = 0x53424F4C;   // 'LOBS'
constexpr unsigned kRows = 8;
enum RowFlag : uint8_t { kRowUsed = 1, kRowHost = 2 };
enum RowTeam : uint8_t { kTeamHuman = 0, kTeamNecro = 1, kTeamNone = 2 };
#pragma pack(push, 1)
struct Row { uint8_t machine = 0, team = kTeamNone, flags = 0, pad = 0; uint32_t xp = 0; uint16_t pingMs = 0, pad2 = 0; char name[24]{}; };
struct StatePacket {
    uint32_t magic = kStateMagic;
    uint8_t version = kVersion;
    uint8_t map = kNoMap;
    uint8_t mode = 0;          // ModeFlags of the host's game mode
    uint8_t count = 0;         // players in the rows
    uint32_t token = 0, seq = 0;
    int32_t countdownMs = -1;  // -1: waiting for players
    Row rows[kRows];
};
#pragma pack(pop)
static_assert(sizeof(Row) == 36, "lobby row size");
static_assert(sizeof(StatePacket) == 308, "lobby state size");
inline bool ParseState(const void* data, int length, StatePacket& out) {
    if (length != static_cast<int>(sizeof(StatePacket))) return false;
    std::memcpy(&out, data, sizeof(StatePacket));
    for (auto& r : out.rows) r.name[sizeof(r.name) - 1] = 0;
    return out.magic == kStateMagic && out.version == kVersion && out.count <= kRows;
}

// The rows in the native list's order: the host first, then the joiners by
// machine. With teams the first `humansWanted` players (0: half, rounded up) are
// humans, the rest necromorphs, the same split as coop_mpnative::AssignTeams;
// humans fill rows 0..3 and necromorphs rows 4..7 when both sides fit, since the
// native list has no team column. Returns the player count.
inline unsigned FillRows(StatePacket& s, const Host& h, const Profile& own, bool teams, unsigned humansWanted) {
    Row players[kRows];
    unsigned n = 0;
    players[n].machine = 0;
    players[n].flags = kRowUsed | kRowHost;
    players[n].xp = own.xp;
    std::memcpy(players[n].name, own.name, sizeof(players[n].name));
    players[n].name[sizeof(players[n].name) - 1] = 0;
    ++n;
    for (uint8_t machine = 1; machine <= kMaxMembers && n < kRows; ++machine)
        for (const auto& m : h.members) {
            if (!m.used || m.machine != machine) continue;
            Row& r = players[n++];
            r.machine = machine;
            r.flags = kRowUsed;
            r.xp = m.profile.xp;
            r.pingMs = m.profile.pingMs;
            std::memcpy(r.name, m.profile.name, sizeof(r.name));
            r.name[sizeof(r.name) - 1] = 0;
            break;
        }
    const unsigned humans = !teams ? n : humansWanted ? (humansWanted < n ? humansWanted : n) : (n + 1) / 2;
    const bool split = teams && humans <= kRows / 2 && n - humans <= kRows / 2;
    for (auto& r : s.rows) r = Row{};
    for (unsigned i = 0; i < n; ++i) {
        players[i].team = !teams ? kTeamNone : i < humans ? kTeamHuman : kTeamNecro;
        const unsigned slot = split && i >= humans ? kRows / 2 + (i - humans) : i;
        s.rows[slot] = players[i];
    }
    s.count = static_cast<uint8_t>(n);
    return n;
}

// The native signal icon (GetPlayerSignalBar 0x00A53230): 4 bars up to 133 ms,
// 3 to 233, 2 to 333, 1 to 500, 0 above, 5 for no connection; the host's row adds 9.
inline int SignalBars(unsigned pingMs, bool known, bool host) {
    const int bars = host ? 4 : !known ? 5 : pingMs <= 133 ? 4 : pingMs <= 233 ? 3 : pingMs <= 333 ? 2 : pingMs <= 500 ? 1 : 0;
    return host ? bars + 9 : bars;
}

// --- joiner --------------------------------------------------------------------
struct Joiner {
    enum class State : uint8_t { Idle, Hello, Welcomed, Launched, Refused };
    State state = State::Idle;
    uint32_t token = 0;
    uint8_t machine = 0, members = 0;
    int map = -1;
    int hostMode = -1;   // the launch's game mode, -1 when the host sent none
    int hostDifficulty = -1;   // the launch's difficulty, -1 when the host sent none
    uint32_t hostSeed = 0;
    bool hasHostSeed = false;
    uint16_t hostGamePort = 0;
    RosterEntry roster[kMaxMembers];
    const char* refusal = nullptr;
    void Start() { *this = Joiner{}; state = State::Hello; }
    // Returns true exactly once: when this packet asks for the launch.
    bool OnPacket(const Packet& p, uint32_t ourBuild, uint32_t ourRandomizer) {
        if (state == State::Idle || state == State::Launched || state == State::Refused) return false;
        const Type t = static_cast<Type>(p.type);
        if (t == Type::Full) { state = State::Refused; refusal = "the host's lobby is full"; return false; }
        if (t == Type::Bye) {
            if (!token || p.token == token) { state = State::Refused; refusal = "the host closed the lobby"; }
            return false;
        }
        if (t == Type::Mismatch || p.build != ourBuild) { state = State::Refused; refusal = "the host runs a different DS2Coop build"; return false; }
        if (p.randomizer != ourRandomizer) { state = State::Refused; refusal = "the host runs a different randomizer build"; return false; }
        if (t != Type::Welcome && t != Type::Launch) return false;
        if (!p.machine) return false;
        if (state == State::Welcomed && token && p.token != token) return false;   // another lobby
        token = p.token;
        machine = p.machine;
        members = p.members;
        hostGamePort = p.gamePort;
        if (state == State::Hello) state = State::Welcomed;
        if (t == Type::Launch && p.map != kNoMap) {
            map = p.map;
            hostMode = ModeFromFlags(p.flags);
            hostDifficulty = p.difficulty < kDifficulties ? p.difficulty : -1;
            hostSeed = p.seed;
            hasHostSeed = p.seed != 0 || p.profile.pad == 1;
            std::memcpy(roster, p.roster, sizeof(roster));
            state = State::Launched;
            return true;
        }
        return false;
    }
    // The host's campaign restart: true exactly once per lobby.
    CampaignPacket campaign;
    bool OnCampaign(const CampaignPacket& p, uint32_t ourBuild, uint32_t ourRandomizer) {
        if (state != State::Hello && state != State::Welcomed) return false;
        if (p.build != ourBuild) { state = State::Refused; refusal = "the host runs a different DS2Coop build"; return false; }
        if (p.randomizer != ourRandomizer) { state = State::Refused; refusal = "the host runs a different randomizer build"; return false; }
        if (!p.machine || (token && p.token != token)) return false;
        token = p.token;
        machine = p.machine;
        members = p.members;
        hostGamePort = p.gamePort;
        std::memcpy(roster, p.roster, sizeof(roster));
        hostSeed = p.seed;
        hasHostSeed = p.seed != 0 || (p.flags & 1) != 0;
        campaign = p;
        state = State::Launched;
        return true;
    }
};

}  // namespace coop_lobby
