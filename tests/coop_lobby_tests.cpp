#include "../src/coop_lobby.h"

#include <cassert>
#include <cstdio>

using namespace coop_lobby;

static void Packets() {
    const Packet p = Make(Type::Hello, 0x1234, 7, 27016, 0xBEEF, 0xCAFE);
    Packet q;
    assert(Parse(&p, sizeof(p), q) && q.type == static_cast<uint8_t>(Type::Hello) && q.token == 0x1234 && q.seq == 7 && q.gamePort == 27016 &&
           q.build == 0xBEEF && q.randomizer == 0xCAFE);
    assert(!Parse(&p, sizeof(p) - 1, q));
    Packet bad = p;
    bad.magic = 0;
    assert(!Parse(&bad, sizeof(bad), q));
    bad = p;
    bad.version = 1;   // the first lobby, without the roster
    assert(!Parse(&bad, sizeof(bad), q));
    bad = p;
    bad.type = 0;
    assert(!Parse(&bad, sizeof(bad), q));
    // version 4 added the difficulty byte after the version 3 layout, version 5 the seed
    assert(sizeof(Packet) == static_cast<size_t>(kVersion4Bytes) + 4 && p.difficulty == kNoDifficulty && p.seed == 0);
    assert(RefusalBytes(4) == kVersion4Bytes);
    assert(OtherVersion(&p, sizeof(p)) == -1);
    bad = p;
    bad.version = 3;
    assert(!Parse(&bad, kVersion3Bytes, q) && OtherVersion(&bad, kVersion3Bytes) == 3 && RefusalBytes(3) == kVersion3Bytes && RefusalBytes(2) == 0);
    bad.magic = 0;
    assert(OtherVersion(&bad, kVersion3Bytes) == -1 && OtherVersion(&bad, 4) == -1 && OtherVersion(nullptr, 120) == -1);
    // a version 3 joiner reads the first 120 bytes of our refusal as its own packet
    Packet refusal = Make(Type::Mismatch, 1, 2, 27015, 0xBEEF, 0xCAFE);
    refusal.version = 3;
    unsigned char v3[kVersion3Bytes];
    std::memcpy(v3, &refusal, sizeof(v3));
    assert(v3[4] == 3 && v3[5] == static_cast<uint8_t>(Type::Mismatch));
    std::printf("packets ok\n");
}

static void HostTable() {
    Host h;
    assert(h.Admit(1, 100, 27016, 0) == 1);
    assert(h.Admit(2, 100, 27017, 0) == 2);
    assert(h.Admit(1, 100, 27016, 50) == 1);   // the same joiner keeps its number
    assert(h.Count() == 2);
    h.Forget(1, 100);
    assert(h.Count() == 1 && h.Admit(3, 100, 27018, 60) == 1);   // the lowest free number again
    assert(h.Admit(4, 100, 27019, 70) == 3);
    assert(h.Admit(98, 1, 1, 80, 3) == 0 && h.Count() == 3);   // the host's bots hold the other seats
    Packet launch = Make(Type::Launch, 1, 1, 27015, 1, 1);
    h.FillRoster(launch);
    unsigned machines = 0;
    for (const auto& r : launch.roster) machines |= 1u << r.machine;
    assert(machines == 0xF);   // machines 1, 2 and 3 with their game ports, the empty rows are machine 0
    for (const auto& r : launch.roster) assert(!r.machine || (r.gamePort >= 27016 && r.gamePort <= 27019));
    for (uint32_t ip = 10; h.Count() < kMaxMembers; ++ip) assert(h.Admit(ip, 100, 27020, 90));
    assert(h.Count() == kMaxMembers && h.Admit(99, 1, 1, 95) == 0);   // full: eight players
    // the maps each joiner can load: machine 3 has no DS2DAT12 (maps 5 and 6)
    assert(h.MissingMap(6) == 0);   // nothing said yet: assumed there
    h.NoteMaps(3, 100, 0x7F);
    h.NoteMaps(2, 100, 0x7F);
    h.NoteMaps(4, 100, 0x1F);
    assert(h.MissingMap(0) == 0 && h.MissingMap(4) == 0 && h.MissingMap(5) == 3 && h.MissingMap(6) == 3);
    assert(h.MissingMap(-1) == 0 && h.MissingMap(40) == 0);
    assert(h.Expire(10000, 5000) == kMaxMembers && h.Count() == 0);
    // a second match in the host's running session: machine 1 is still connected
    Host again;
    assert(again.Admit(20, 100, 27016, 0, kMaxMembers, 1u << 1) == 2);
    assert(again.Admit(21, 100, 27017, 0, kMaxMembers, 1u << 1 | 1u << 3) == 4);
    assert(again.Admit(22, 100, 27018, 0, 3, 1u << 1) == 0);   // the held partner takes a seat too
    std::printf("host table ok\n");
}

static void JoinerFlow() {
    const uint32_t build = 0xBEEF, rnd = 0xCAFE;
    Joiner j;
    Packet welcome = Make(Type::Welcome, 0xAA, 1, 27015, build, rnd);
    welcome.machine = 2;
    assert(!j.OnPacket(welcome, build, rnd) && j.state == Joiner::State::Idle);   // not started
    j.Start();
    assert(!j.OnPacket(welcome, build, rnd) && j.state == Joiner::State::Welcomed && j.machine == 2 && j.token == 0xAA && j.hostGamePort == 27015);
    Packet other = Make(Type::Launch, 0xBB, 2, 27015, build, rnd);
    other.machine = 3;
    other.map = 1;
    assert(!j.OnPacket(other, build, rnd));   // another lobby's launch is ignored
    Packet launch = Make(Type::Launch, 0xAA, 3, 27015, build, rnd);
    launch.machine = 2;
    launch.map = 0;
    launch.roster[0] = RosterEntry{0x0100007F, 27016, 1, 0};
    launch.roster[1] = RosterEntry{0x0100007F, 27017, 2, 0};
    launch.flags = ModeFlags(4);
    launch.difficulty = 3;
    assert(j.hostDifficulty == -1);
    assert(j.OnPacket(launch, build, rnd) && j.map == 0 && j.state == Joiner::State::Launched && j.hostMode == 4 && j.hostDifficulty == 3);
    assert(ModeFlags(0) == 0x80 && ModeFromFlags(ModeFlags(0)) == 0 && ModeFromFlags(0) == -1 && ModeFlags(-1) == 0 && ModeFlags(200) == 0);
    assert(j.roster[0].gamePort == 27016 && j.roster[1].machine == 2 && j.roster[2].machine == 0);
    assert(!j.hasHostSeed);
    const uint32_t seeds[] = {0u, 69u, 0xFFFFFFFFu};
    for (uint32_t seed : seeds) {
        Joiner seeded;
        seeded.Start();
        Packet offer = launch;
        offer.seed = seed;
        offer.profile.pad = seed == 0 ? 1 : 0;
        assert(seeded.OnPacket(offer, build, rnd) && seeded.hasHostSeed && seeded.hostSeed == seed);
    }
    assert(!j.OnPacket(launch, build, rnd));   // exactly once
    Joiner direct;
    direct.Start();
    Packet noDifficulty = launch;
    noDifficulty.difficulty = 7;   // out of range: none
    assert(direct.OnPacket(noDifficulty, build, rnd) && direct.machine == 2 && direct.hostDifficulty == -1);   // a launch without a welcome first still counts
    Joiner wrong;
    wrong.Start();
    assert(!wrong.OnPacket(launch, build + 1, rnd) && wrong.state == Joiner::State::Refused);
    Joiner otherPool;
    otherPool.Start();
    assert(!otherPool.OnPacket(launch, build, rnd + 1) && otherPool.state == Joiner::State::Refused);
    Joiner full;
    full.Start();
    assert(!full.OnPacket(Make(Type::Full, 0xAA, 4, 27015, build, rnd), build, rnd) && full.state == Joiner::State::Refused);
    // a campaign arena rides in the map byte as kArenaFlag | its index
    assert(ArenaCode(0) == 0x80 && ArenaCode(3) == 0x83 && ArenaCode(0x7F) == kNoMap && ArenaCode(500) == kNoMap);
    assert(IsArenaCode(0x80) && IsArenaCode(0xFE) && !IsArenaCode(kNoMap) && !IsArenaCode(0) && !IsArenaCode(0x7F) && !IsArenaCode(-1));
    assert(ArenaIndexOf(ArenaCode(5)) == 5 && ArenaIndexOf(4) == -1 && ArenaIndexOf(kNoMap) == -1);
    Joiner arena;
    arena.Start();
    Packet arenaLaunch = launch;
    arenaLaunch.map = ArenaCode(1);
    arenaLaunch.flags = ModeFlags(1);
    assert(arena.OnPacket(arenaLaunch, build, rnd) && IsArenaCode(arena.map) && ArenaIndexOf(arena.map) == 1 && arena.hostMode == 1);
    std::printf("joiner ok\n");
}

static void Campaign() {
    const uint32_t build = 0xBEEF, rnd = 0xCAFE;
    CampaignPacket c;
    c.token = 0xAA;
    c.machine = 1;
    c.build = build;
    c.randomizer = rnd;
    c.gamePort = 27015;
    c.checkpoint[0] = 0x1234;
    c.difficulty = 4;   // zealot + 1
    std::snprintf(c.map, sizeof(c.map), "%s", "ch06_cec");
    std::snprintf(c.stream, sizeof(c.stream), "%s", "ch06_cec.str");
    CampaignPacket q;
    assert(ParseCampaign(&c, sizeof(c), q) && q.checkpoint[0] == 0x1234 && q.difficulty == 4);
    CampaignPacket empty = c;
    empty.map[0] = 0;
    assert(!ParseCampaign(&empty, sizeof(empty), q));
    Joiner j;
    j.Start();
    assert(j.OnCampaign(c, build, rnd) && j.state == Joiner::State::Launched && j.machine == 1);
    assert(!j.hasHostSeed);
    Joiner zero;
    zero.Start();
    CampaignPacket offered = c;
    offered.flags |= 1;
    assert(zero.OnCampaign(offered, build, rnd) && zero.hasHostSeed && zero.hostSeed == 0);
    assert(!j.OnCampaign(c, build, rnd));   // once
    Joiner w;
    w.Start();
    Packet welcome = Make(Type::Welcome, 0xBB, 1, 27015, build, rnd);
    welcome.machine = 2;
    w.OnPacket(welcome, build, rnd);
    assert(!w.OnCampaign(c, build, rnd));   // another host's restart
    Joiner idle;
    assert(!idle.OnCampaign(c, build, rnd));   // never asked to join
    std::printf("campaign ok\n");
}

static Profile Card(const char* name, uint32_t xp, uint16_t ping) {
    Profile p;
    std::snprintf(p.name, sizeof(p.name), "%s", name);
    p.xp = xp;
    p.pingMs = ping;
    return p;
}

static void LobbyList() {
    Host h;
    assert(h.Admit(10, 1, 27016, 0) == 1 && h.Admit(11, 1, 27017, 0) == 2 && h.Admit(12, 1, 27018, 0) == 3);
    assert(h.NoteProfile(10, 1, Card("alpha", 5000, 40)) && !h.NoteProfile(10, 1, Card("alpha", 5000, 40)));
    assert(h.NoteProfile(11, 1, Card("bravo", 0, 250)) && h.NoteProfile(12, 1, Card("charlie", 99, 0)));
    assert(!h.NoteProfile(99, 1, Card("nobody", 0, 0)));
    StatePacket s;
    assert(FillRows(s, h, Card("host", 7, 0), false, 0) == 4 && s.count == 4);
    assert(s.rows[0].flags == (kRowUsed | kRowHost) && std::strcmp(s.rows[0].name, "host") == 0 && s.rows[0].xp == 7);
    assert(s.rows[1].machine == 1 && s.rows[2].machine == 2 && s.rows[3].machine == 3 && s.rows[4].flags == 0);
    assert(s.rows[1].pingMs == 40 && s.rows[2].team == kTeamNone);
    // Versus: four players, two humans in rows 0..1, two necromorphs from row 4
    assert(FillRows(s, h, Card("host", 7, 0), true, 0) == 4);
    assert(s.rows[0].team == kTeamHuman && s.rows[1].machine == 1 && s.rows[1].team == kTeamHuman);
    assert(s.rows[2].flags == 0 && s.rows[3].flags == 0);
    assert(s.rows[4].machine == 2 && s.rows[4].team == kTeamNecro && s.rows[5].machine == 3 && s.rows[5].team == kTeamNecro);
    // five humans wanted of four: all humans, in order
    FillRows(s, h, Card("host", 7, 0), true, 5);
    for (unsigned i = 0; i < 4; ++i) assert(s.rows[i].team == kTeamHuman && s.rows[i].flags & kRowUsed);
    // eight players, one human: the necromorphs do not fit rows 4..7, so the list runs on
    for (uint32_t ip = 20; h.Count() < kMaxMembers; ++ip) h.Admit(ip, 1, 27020, 0);
    assert(FillRows(s, h, Card("host", 7, 0), true, 1) == 8 && s.rows[0].team == kTeamHuman && s.rows[1].team == kTeamNecro && s.rows[7].flags & kRowUsed);
    StatePacket q;
    s.token = 5;
    s.countdownMs = 12000;
    assert(ParseState(&s, sizeof(s), q) && q.count == 8 && q.countdownMs == 12000);
    assert(!ParseState(&s, sizeof(s) - 1, q));
    StatePacket bad = s;
    bad.count = 9;
    assert(!ParseState(&bad, sizeof(bad), q));
    assert(SignalBars(0, true, true) == 13 && SignalBars(100, true, false) == 4 && SignalBars(133, true, false) == 4 && SignalBars(134, true, false) == 3);
    assert(SignalBars(300, true, false) == 2 && SignalBars(500, true, false) == 1 && SignalBars(501, true, false) == 0 && SignalBars(0, false, false) == 5);
    // the host's goodbye ends a joiner's wait, another lobby's does not
    const uint32_t build = 0xBEEF, rnd = 0xCAFE;
    Joiner j;
    j.Start();
    Packet welcome = Make(Type::Welcome, 0xAA, 1, 27015, build, rnd);
    welcome.machine = 1;
    j.OnPacket(welcome, build, rnd);
    assert(!j.OnPacket(Make(Type::Bye, 0xBB, 2, 27015, build, rnd), build, rnd) && j.state == Joiner::State::Welcomed);
    assert(!j.OnPacket(Make(Type::Bye, 0xAA, 3, 27015, build, rnd), build, rnd) && j.state == Joiner::State::Refused);
    std::printf("lobby list ok\n");
}

int main() {
    Campaign();
    Packets();
    HostTable();
    JoinerFlow();
    LobbyList();
    std::printf("lobby tests passed\n");
    return 0;
}
