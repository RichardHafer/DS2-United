// Dead Space 2 United by Richard Hafer.
// The survival director without a game: configuration and the shipped
// template, wave plans and endless growth, substitution, the spawn queue,
// pacing, spawn point rules, the stuck verdict, rewards, the best-wave file and
// the generated vent and engine tables.
#include <array>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include "../src/survival.h"
#include "../src/survival_capsules.h"
#include "../src/coop_zone.h"
#include "../src/coop_marks.h"

namespace ds2 { using Guid = std::array<uint32_t, 4>; }
#include "../src/generated_survival_vents.h"
#include "../src/generated_global_targets.h"
#include "../src/generated_slasher_skins.h"

namespace {
using namespace survival;

std::string ReadFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void IniReader() {
    const Ini ini = ParseIni("\xEF\xBB\xBF; comment\n[Survival]\r\nEnabled = 1\r\n# also a comment\nMAP=mp_gov\n[wave1]\nEnemies=Zombie:3\n");
    assert(Value(ini, "survival", "enabled") && *Value(ini, "SURVIVAL", "Enabled") == "1");
    assert(*Value(ini, "Survival", "Map") == "mp_gov");
    assert(Value(ini, "Wave1", "enemies") && !Value(ini, "Wave2", "Enemies") && !Value(ini, "Survival", "Nope"));
    std::printf("ini ok\n");
}

void Entries() {
    std::vector<Entry> e;
    std::string error;
    assert(ParseEntries("Zombie:6", e, &error) && e.size() == 1 && e[0].family == "Zombie" && e[0].count == 6 && e[0].path == Path::Any && e[0].group == 1);
    assert(ParseEntries(" zombie : 4 : vent , Stalker:3:direct:3,PackEnemy:2:any", e, &error) && e.size() == 3);
    assert(e[0].family == "Zombie" && e[0].path == Path::Vent && e[1].family == "Stalker" && e[1].group == 3 && e[1].path == Path::Direct);
    assert(e[2].family == "PackEnemy" && e[2].path == Path::Any);
    assert(!ParseEntries("Guardian:1", e, &error) && error.find("boss") != std::string::npos);
    assert(!ParseEntries("Necro:1", e, &error) && error.find("unknown") != std::string::npos);
    assert(!ParseEntries("Zombie", e, &error) && !ParseEntries("Zombie:0", e, &error) && !ParseEntries("Zombie:61", e, &error));
    assert(!ParseEntries("Zombie:2:sideways", e, &error) && !ParseEntries("Zombie:2:any:7", e, &error) && !ParseEntries("Zombie:2:any:2:x", e, &error));
    assert(!ParseEntries("", e, &error) && !ParseEntries(" , ", e, &error));
    std::printf("entries ok\n");
}

void Template() {
    const std::string text = ReadFile("DS2Survival.ini");
    assert(!text.empty());
    const Config c = ParseConfig(text);
    for (const auto& w : c.warnings) std::printf("template warning: %s\n", w.c_str());
    assert(c.warnings.empty());
    assert(EqualNoCase(c.map, "any"));   // every MP map, picked in the main menu
    assert(c.startDelayS == 20 && c.startKey == 107 && c.maxAlive == 15 && c.pauseS == 20 && c.minDistance == 12.0f && c.maxDistance == 25.0f);
    assert(c.waves.size() == 10);
    // the wave table the task asked for
    assert(c.waves[0].Total() == 6 && c.waves[0].entries[0].family == "Zombie");
    assert(c.waves[1].Total() == 8);
    assert(c.waves[2].entries.size() == 2 && c.waves[2].entries[1].family == "PackEnemy");
    assert(c.waves[3].entries.size() == 3 && c.waves[3].entries[1].family == "Leaper" && c.waves[3].entries[2].family == "Puker");
    assert(c.waves[4].entries[0].family == "Brute" && c.waves[4].entries[0].count == 1 && c.waves[4].entries[0].path == Path::Direct);
    assert(c.waves[5].entries[2].family == "Stalker" && c.waves[5].entries[2].group == 2 && c.waves[6].entries[2].group == 3);
    assert(c.waves[7].entries[0].family == "Pregnant" && c.waves[7].entries[0].path == Path::Direct);
    assert(c.waves[8].entries[1].family == "DividerCore");
    assert(c.waves[9].boss && c.waves[9].timeLimitS == 120 && c.waves[9].entries[0].family == "Hunter" && c.waves[9].intervalMs == 2500);
    assert(c.waves[9].entries.size() == 9 && c.waves[9].enhanced == 50 && c.waves[9].maxAlive == 14 && c.waves[9].Total() == 32);
    for (size_t i = 0; i < 9; ++i) assert(!c.waves[i].boss && !c.waves[i].timeLimitS);
    std::printf("template ok (%zu waves, enabled=%d)\n", c.waves.size(), c.enabled ? 1 : 0);
}

void ConfigEdges() {
    Config c = ParseConfig("[Survival]\nEnabled=1\nMaxAlive=99\nMinDistance=20\nMaxDistance=21\nVentGapMs=10\nStartKey=abc\n"
                           "[Wave1]\nEnemies=Zombie:2\n[Wave2]\nEnemies=Hunter:1\n[Wave3]\nEnemies=Kong:1\n[Wave4]\nEnemies=Zombie:1\n");
    assert(c.enabled && c.maxAlive == 30 && c.maxDistance == 24.0f && c.ventGapMs == 3000 && c.startKey == 0);
    // wave 3 is refused, so the list stops before it; the boss wave got its timer
    assert(c.waves.size() == 2 && c.waves[1].boss && c.waves[1].timeLimitS == 90);
    assert(c.warnings.size() >= 5);
    Config none = ParseConfig("[Survival]\nEnabled=1\n");
    assert(none.waves.empty() && !none.warnings.empty());
    assert(!ParseConfig("").enabled);
    assert(none.hud && !ParseConfig("[Survival]\nEnabled=1\nHud=0\n").hud);
    assert(none.gameOverS == 10 && ParseConfig("[Survival]\nGameOverSeconds=1\n").gameOverS == 3 &&
           ParseConfig("[Survival]\nGameOverSeconds=25\n").gameOverS == 25);
    assert(ParseConfig(ReadFile("DS2Survival.ini")).gameOverS == 10);
    assert(none.trackMeters == 80.0f && ParseConfig(ReadFile("DS2Survival.ini")).trackMeters == 80.0f);
    assert(ParseConfig("[Survival]\nTrackMeters=0\n").trackMeters == 0.0f && ParseConfig("[Survival]\nTrackMeters=900\n").trackMeters == 300.0f);
    // vents: no real minimum, a radius around the nearest player or bot
    assert(none.ventMinDistance == 0.0f && none.ventMaxDistance == 30.0f && !none.ventLineOfSight);
    const Config tight = ParseConfig("[Survival]\nVentMinDistance=10\nVentMaxDistance=9\n");
    assert(tight.ventMinDistance == 10.0f && tight.ventMaxDistance == 14.0f);
    const Config shipped = ParseConfig(ReadFile("DS2Survival.ini"));
    assert(shipped.ventMinDistance == 0.0f && shipped.ventMaxDistance == 30.0f && !shipped.ventLineOfSight);
    std::printf("config edges ok\n");
}

void Endless() {
    const Config c = ParseConfig(ReadFile("DS2Survival.ini"));
    const Wave w4 = WaveFor(4, c);
    assert(w4.Total() == 19);
    const Wave w11 = WaveFor(11, c);   // first endless wave: wave 4 grown by 20 %
    assert(w11.entries.size() == w4.entries.size());
    assert(w11.entries[0].count == 14 && w11.entries[1].count == 5 && w11.entries[2].count == 4);
    const Wave w12 = WaveFor(12, c);   // wave 5, two steps past: +40 %
    assert(w12.entries[0].family == "Brute" && w12.entries[0].count == 1 && w12.entries[1].count == 8);
    const Wave w16 = WaveFor(16, c);   // the cycle 4..9 has six waves: 16 is wave 9 again
    assert(w16.entries[1].family == "DividerCore");
    const Wave w17 = WaveFor(17, c);   // and 17 starts over at wave 4, grown by 140 %
    assert(w17.entries[0].family == "Zombie" && w17.entries[0].count == 29);
    const Wave w20 = WaveFor(20, c);   // every tenth: the boss wave, its Ubermorph never multiplied
    assert(w20.boss && w20.timeLimitS == 120 && w20.entries[0].family == "Hunter" && w20.entries[0].count == 1 && w20.entries[1].family == "DividerCore" &&
           w20.entries[1].count == 6 && w20.enhanced == 50);
    const Wave far = WaveFor(400, c);
    for (const auto& e : far.entries) assert(e.count <= kMaxEntryCount);
    assert(WaveFor(0, c).entries.empty());
    std::printf("endless ok\n");
}

void Substitution_() {
    const std::set<std::string> all = {"Zombie", "PackEnemy", "Leaper", "Puker", "Exploder", "Lurker", "Infector", "Stalker", "TubeBeetle", "SwarmEnemy",
                                       "Pregnant", "DividerCore", "Hunter"};
    Substitution s = Substitute("Zombie", all);
    assert(s.family == "Zombie" && !s.substituted && s.factor == 1);
    s = Substitute("Brute", all);   // Brute's package is trimmed from the pool
    assert(s.family == "DividerCore" && s.substituted && s.factor == 1);
    s = Substitute("Brute", {"Zombie", "Stalker"});
    assert(s.family == "Stalker" && s.factor == 3);
    s = Substitute("Hunter", {"Zombie"});
    assert(s.family == "Zombie" && s.factor == 3);
    s = Substitute("Lurker", {"Leaper", "Zombie"});
    assert(s.family == "Leaper" && s.factor == 1);
    s = Substitute("Infector", {"SwarmEnemy"});   // last resort: any swarm family
    assert(s.family == "SwarmEnemy");
    assert(Substitute("Zombie", {}).family.empty());
    assert(Excluded("Guardian") && Excluded("tripod") && Excluded("NicoleBoss") && !Excluded("Brute"));
    assert(Unkillable("Hunter") && !Unkillable("DividerCore"));
    assert(RoleOf("Exploder") == Role::Explosive && RoleOf("Brute") == Role::Heavy && RoleOf("Nope") == Role::Unknown);
    std::printf("substitution ok\n");
}

void Queue() {
    Wave w;
    std::string error;
    assert(ParseEntries("Zombie:6, Brute:1:direct, Exploder:3, Stalker:3:direct:3", w.entries, &error));
    const auto q = BuildQueue(w);
    assert(q.size() == 13);
    assert(q[0].family == "Brute" && q[0].path == Path::Direct);   // heavies first
    // packs stay together: three Stalkers in a row, the first the head
    size_t at = 0;
    while (at < q.size() && q[at].family != "Stalker") ++at;
    assert(at + 2 < q.size() && q[at].groupHead && q[at + 1].family == "Stalker" && !q[at + 1].groupHead && !q[at + 2].groupHead && q[at].group == 3);
    // mixed: no five Zombies in a row
    unsigned run = 0, longest = 0;
    for (const auto& i : q) { run = i.family == "Zombie" ? run + 1 : 0; longest = run > longest ? run : longest; }
    assert(longest <= 2);
    unsigned dropped = 0;
    std::map<std::string, std::string> subs;
    const auto resolved = ResolveQueue(q, {"Zombie", "Exploder", "Leaper"}, dropped, &subs);
    // Brute -> Zombie x3 (path freed), Stalker -> Leaper
    assert(dropped == 0 && resolved.size() == 15 && resolved[0].family == "Zombie" && resolved[0].path == Path::Any && resolved[0].groupHead &&
           !resolved[1].groupHead && subs.at("Brute") == "Zombie x3" && subs.at("Stalker") == "Leaper");
    const auto nothing = ResolveQueue(q, {}, dropped, nullptr);
    assert(nothing.empty() && dropped == 13);
    std::printf("queue ok\n");
}

void Pacing() {
    assert(!SpawnDue(10000, 9000, 1500, 0, 15, false) && SpawnDue(10500, 9000, 1500, 0, 15, false));
    assert(!SpawnDue(20000, 0, 1500, 15, 15, false));
    assert(SpawnDue(9300, 9000, 1500, 3, 15, true) && !SpawnDue(9100, 9000, 1500, 3, 15, true));
    assert(SpawnDue(5, 0xFFFFF000u, 1500, 0, 15, false));   // tick wrap
    assert(EffectiveCap(0, 15, true) == 15 && EffectiveCap(8, 15, true) == 8 && EffectiveCap(0, 15, false) == 10 && EffectiveCap(0, 0, true) == 1);
    std::printf("pacing ok\n");
}

void Points() {
    PointRules r;
    const V3 anchor{0, 0, 0};
    const V3 bot{3, 0, 0};
    const V3 viewers[] = {anchor, bot};
    assert(CheckPoint({0, 0, 18}, anchor, viewers, 2, 20.0f, r) == PointVerdict::Ok);
    assert(CheckPoint({0, 0, 18}, anchor, viewers, 2, 0.0f, r) == PointVerdict::NoPath);
    assert(CheckPoint({0, 4, 18}, anchor, viewers, 2, 20.0f, r) == PointVerdict::OtherFloor);
    assert(CheckPoint({0, 0, 30}, anchor, viewers, 2, 31.0f, r) == PointVerdict::TooFar);
    assert(CheckPoint({0, 0, 8}, anchor, viewers, 2, 9.0f, r) == PointVerdict::TooClose);
    assert(CheckPoint({13, 0, 0}, anchor, viewers, 2, 14.0f, r) == PointVerdict::TooClose);   // 10 m from the bot
    assert(CheckPoint({0, 0, 18}, anchor, viewers, 2, 60.0f, r) == PointVerdict::PathTooLong);
    // behind the player beats in front of him; the ideal distance beats the edge
    const V3 forward{0, 0, 1};
    const float behind = ScorePoint({0, 0, -18}, anchor, forward, nullptr, 0, r, 0.0f);
    const float ahead = ScorePoint({0, 0, 18}, anchor, forward, nullptr, 0, r, 0.0f);
    const float edge = ScorePoint({0, 0, -24.5f}, anchor, forward, nullptr, 0, r, 0.0f);
    assert(behind > ahead && behind > edge && std::fabs(behind - 13.0f) < 1e-3f);
    const V3 recent[] = {{0, 0, -17}};
    assert(ScorePoint({0, 0, -18}, anchor, forward, recent, 1, r, 0.0f) < behind - 3.5f);
    // the default config reaches the floors above and below and favours them
    const PointRules floors = RulesOf(Config{});
    assert(CheckPoint({0, 4.5f, 18}, anchor, viewers, 2, 26.0f, floors) == PointVerdict::Ok);
    assert(CheckPoint({0, -4.5f, -18}, anchor, viewers, 2, 26.0f, floors) == PointVerdict::Ok);
    assert(CheckPoint({0, 14, 18}, anchor, viewers, 2, 30.0f, floors) == PointVerdict::OtherFloor);
    assert(CheckPoint({13, 0, 0}, anchor, viewers, 2, 14.0f, floors) == PointVerdict::Ok);   // a bot 10 m away no longer blocks
    assert(CheckPoint({3, 0, 7}, anchor, viewers, 2, 9.0f, floors) == PointVerdict::TooClose);
    assert(ScorePoint({0, -4.5f, -18}, anchor, forward, nullptr, 0, floors, 0.0f) > ScorePoint({0, 0, -18}, anchor, forward, nullptr, 0, floors, 0.0f) + 1.0f);
    const float radii[] = {14, 18, 23.5f};
    const auto seeds = RingSeeds(anchor, forward, radii, 3, 12, 0);
    assert(seeds.size() == 36);
    assert(seeds[0].z < -13.9f && std::fabs(seeds[0].x) < 0.01f);   // straight behind first
    for (const auto& s : seeds) { const float d = Distance(s, anchor); assert(d > 13.9f && d < 23.6f && s.y == 0.0f); }
    const V3 path[] = {{0, 0, 0}, {3, 0, 4}, {3, 0, 10}};
    assert(std::fabs(PathLength(path, 3) - 11.0f) < 1e-4f && PathLength(path, 1) == 0.0f);
    assert(std::fabs(FacingYaw({0, 0, 0}, {0, 0, 5})) < 1e-3f && std::fabs(FacingYaw({0, 0, 0}, {5, 0, 0}) - 90.0f) < 1e-3f);
    assert(Distance(PackOffset({1, 2, 3}, 0), {1, 2, 3}) == 0.0f && std::fabs(Distance(PackOffset({1, 2, 3}, 2), {1, 2, 3}) - 1.3f) < 1e-4f);
    std::printf("points ok\n");
}

void VentChoice() {
    assert(!WantsVent(Path::Vent, false, true, 100, 0.0f) && !WantsVent(Path::Any, true, false, 100, 0.0f));
    assert(WantsVent(Path::Vent, true, true, 0, 0.99f) && !WantsVent(Path::Direct, true, true, 100, 0.0f));
    assert(WantsVent(Path::Any, true, true, 40, 0.39f) && !WantsVent(Path::Any, true, true, 40, 0.41f));
    std::printf("vent choice ok\n");
}

void Stuck() {
    Config c;
    Watch w;
    w.bornAt = w.progressAt = 1000;
    assert(Observe(w, 2000, 30.0f, false, 4, c) == Fate::Alive);
    assert(Observe(w, 30000, 20.0f, false, 4, c) == Fate::Alive);   // came 10 m closer: progress
    assert(Observe(w, 60000, 19.0f, false, 4, c) == Fate::Alive && w.progressAt == 30000);   // 1 m is no progress
    assert(Observe(w, 90001, 19.0f, false, 4, c) == Fate::Stuck);
    Watch near;
    near.bornAt = near.progressAt = 0;
    assert(Observe(near, 100000, 5.0f, false, 4, c) == Fate::Alive && near.engaged && near.engagedAt == 100000);
    assert(Observe(near, 159000, 6.0f, false, 4, c) == Fate::Alive);   // fighting at close range keeps it alive
    Watch vent;
    vent.bornAt = vent.progressAt = 0;
    assert(Observe(vent, 29000, 15.0f, true, 3, c) == Fate::Alive && Observe(vent, 30000, 15.0f, true, 3, c) == Fate::VentStuck);
    Watch inert;
    inert.bornAt = inert.progressAt = 0;
    assert(Observe(inert, 19000, 15.0f, false, 0, c) == Fate::Alive && Observe(inert, 20000, 15.0f, false, 0, c) == Fate::Inert);
    assert(std::strcmp(FateName(Fate::WrittenOff), "written_off") == 0 && std::strcmp(FateName(Fate::Timer), "timer_kill") == 0);
    // frozen: in a wall next to a player it stays engaged, but it does not move
    Watch wall;
    wall.bornAt = 0;
    NoteMoved(wall, 1.0f, 0.0f, 1.0f, 2000);
    assert(wall.movedAt == 2000u);
    {
        // 17.10.: a stamp of now|1 on an even tick read as frozen for 4294967 s in the same tick
        Watch moving;
        moving.bornAt = 0;
        NoteMoved(moving, 0.0f, 0.0f, 0.0f, 20000);
        NoteMoved(moving, 5.0f, 0.0f, 0.0f, 40000);
        assert(!Frozen(moving, 40000, false, c));
    }
    NoteMoved(wall, 1.5f, 0.0f, 1.5f, 6000);   // 0.7 m: not moved
    assert(!Frozen(wall, 16000, false, c) && Frozen(wall, 17001, false, c) && !Frozen(wall, 17001, true, c));
    NoteMoved(wall, 2.5f, 0.0f, 1.0f, 17001);   // 1.5 m: moved
    assert(!Frozen(wall, 30000, false, c) && Frozen(wall, 32002, false, c));
    Watch young;
    young.bornAt = 10000;
    NoteMoved(young, 0.0f, 0.0f, 0.0f, 10000);
    Config slow = c;
    slow.frozenS = 1;
    assert(!Frozen(young, 17000, false, slow) && Frozen(young, 18000, false, slow));   // never in its first 8 s
    slow.frozenS = 0;
    assert(!Frozen(young, 900000, false, slow));
    assert(HoldsStill("Lurker") && HoldsStill("puker") && HoldsStill("Stalker") && !HoldsStill("Zombie"));
    std::printf("stuck ok\n");
}

void Rewards() {
    auto s = SplitCredits(1000);
    assert(s.size() == 4 && s[0] == 0 && s[1] == 0 && s[2] == 0 && s[3] == 1);
    s = SplitCredits(19500);   // 10000 + 5000 + 3000 + 1000 + the rest as one more 1000
    assert(s[0] == 1 && s[1] == 1 && s[2] == 1 && s[3] == 2);
    s = SplitCredits(0);
    assert(s[0] == 0 && s[1] == 0 && s[2] == 0 && s[3] == 0);
    std::printf("rewards ok\n");
}

void Escalation() {
    const Config c = ParseConfig("[Survival]\nEnabled=1\nBossEvery=10\n[Wave1]\nEnemies=Zombie:3\n");
    // nothing before wave 11; the boss wave 10 is the last normal one
    assert(EscLevel(10, c) == 0 && EscLevel(11, c) == 1 && EscLevel(20, c) == 10);
    assert(EnhancedChance(10, c) == 0 && EnhancedChance(11, c) == 5 && EnhancedChance(16, c) == 30 && EnhancedChance(22, c) == 60 && EnhancedChance(40, c) == 60);   // +5 % a step, capped at 60 %
    assert(std::fabs(HealthFactor(10, c) - 1.0f) < 1e-6f && std::fabs(HealthFactor(11, c) - 1.03f) < 1e-5f && std::fabs(HealthFactor(20, c) - 1.3f) < 1e-5f &&
           std::fabs(HealthFactor(30, c) - 1.6f) < 1e-5f && std::fabs(HealthFactor(40, c) - 1.6f) < 1e-5f);   // +3 % a step, capped at +60 %
    assert(c.escOracleShare == 35 && c.escOracleMaxAlive == 3);
    // clock modes: three stages make one escalation wave, enhanced capped at 25 %
    assert(ClockEscWave(1, c) == 1 && ClockEscWave(3, c) == 1 && ClockEscWave(4, c) == 2 && ClockEscWave(30, c) == 10 && ClockEscWave(33, c) == 11);
    Wave boss;
    boss.enhanced = 50;
    assert(ClockEnhancedChance(10, Wave{}, c) == 0 && ClockEnhancedChance(33, Wave{}, c) == 5 && ClockEnhancedChance(90, Wave{}, c) == 25 && ClockEnhancedChance(10, boss, c) == 25);
    const Config one = ParseConfig("[Survival]\nEnabled=1\n[Wave1]\nEnemies=Zombie:3\n[Escalation]\nClockStagesPerWave=1\nClockEnhancedMax=100\n");
    assert(ClockEscWave(12, one) == 12 && ClockEnhancedChance(12, Wave{}, one) == 10);
    assert(std::fabs(SpeedFactor(11, c) - 1.01f) < 1e-5f && std::fabs(SpeedFactor(40, c) - 1.3f) < 1e-5f && std::fabs(SpeedFactor(60, c) - 1.4f) < 1e-5f);   // +1 % a step, capped at +40 %
    assert(PauseFor(10, 20, c) == 20 && PauseFor(11, 20, c) == 20 && PauseFor(12, 20, c) == 19 && PauseFor(13, 20, c) == 19 && PauseFor(30, 20, c) == 10 &&
           PauseFor(34, 20, c) == 8 && PauseFor(30, 5, c) == 5);   // a second every two waves
    // MaxAlive is for one player; each more adds three, capped at 23
    assert(MaxAliveFor(15, 1, c) == 15 && MaxAliveFor(15, 2, c) == 18 && MaxAliveFor(15, 3, c) == 21 && MaxAliveFor(15, 4, c) == 23 &&
           MaxAliveFor(15, 8, c) == 23);
    assert(RewardPercent(10, c) == 100 && RewardPercent(11, c) == 98 && RewardPercent(12, c) == 95 && RewardPercent(40, c) == 40);   // 2.5 % a step
    assert(PricePercent(10, c) == 100 && PricePercent(12, c) == 100 && PricePercent(60, c) == 100);   // prices no longer scale
    assert(BotDamagePercent(10, c) == 100 && BotDamagePercent(11, c) == 99 && BotDamagePercent(50, c) == 60 && BotDamagePercent(70, c) == 50);
    // +20 % stasis resistance per boss wave reached, at most 80 %
    assert(BossStasisResist(9, c) == 0 && BossStasisResist(10, c) == 20 && BossStasisResist(25, c) == 40 && BossStasisResist(90, c) == 80);
    assert(Scaled(2, 40) == 1 && Scaled(2, 100) == 2 && Scaled(0, 100) == 0 && Scaled(3, 50) == 2);
    // the section is read
    const Config d = ParseConfig("[Survival]\nEnabled=1\n[Escalation]\nFromWave=5\nEnhancedPerWave=25\nEnhancedMax=100\nBossStasisResistMax=60\n[Wave1]\nEnemies=Zombie:3\n");
    assert(EscLevel(5, d) == 1 && EnhancedChance(8, d) == 100 && BossStasisResist(40, d) == 60);
    // a wave's own Enhanced= raises the chance, never lowers the escalation's
    const Config e = ParseConfig("[Survival]\nEnabled=1\n[Wave1]\nEnemies=Zombie:3\n[Wave2]\nEnemies=Hunter:1:direct, Zombie:3\nEnhanced=40\nTimeLimit=120\n");
    assert(e.waves.size() == 2 && e.waves[0].enhanced == -1 && e.waves[1].enhanced == 40 && e.waves[1].boss && e.waves[1].timeLimitS == 120);
    assert(EnhancedChanceFor(2, e.waves[1], e) == 40 && EnhancedChanceFor(1, e.waves[0], e) == 0 && EnhancedChanceFor(20, e.waves[1], e) == 50);
    assert(WaveFor(20, e).enhanced == 40);   // the repeated boss wave keeps it
    std::printf("escalation ok\n");
}

void Best() {
    auto b = ParseBest("mp_bombRun 7\nmp_gov 3\ngarbage\nmp_bombrun 5\n\nmp_x notanumber\n");
    assert(b.size() == 2 && b.at("mp_bombrun") == 7 && b.at("mp_gov") == 3);
    assert(!UpdateBest(b, "MP_BOMBRUN", 6) && UpdateBest(b, "mp_bombRun", 8) && b.at("mp_bombrun") == 8);
    assert(UpdateBest(b, "mp_solar", 1));
    const auto again = ParseBest(FormatBest(b));
    assert(again == b);
    std::printf("best ok\n");
}

void Rng_() {
    Rng a(5), b(5), c(0);
    for (int i = 0; i < 100; ++i) { const float u = a.Unit(); assert(u >= 0.0f && u < 1.0f && u == b.Unit()); }
    assert(c.Next() != 0);
    std::printf("rng ok\n");
}

void Tables() {
    unsigned bomb = 0, wall = 0;
    for (const auto& row : ds2::kSurvivalVents) {
        assert(row.map && row.family && row.context && row.name && std::strlen(row.survival) == 64 && std::strlen(row.ventFree) == 64);
        assert(std::strncmp(row.context, "SPAWNMODE_VENT|", 15) == 0);
        if (std::strcmp(row.map, "mp_bombRun") == 0) { ++bomb; if (std::strcmp(row.context, "SPAWNMODE_VENT|Wall") == 0) ++wall; }
    }
    assert(bomb == 5 && wall == 1);
    // the Wall vent of mp_bombRun shares its contract with campaign wall donors
    const char* wallKey = nullptr;
    for (const auto& row : ds2::kSurvivalVents)
        if (std::strcmp(row.map, "mp_bombRun") == 0 && std::strcmp(row.context, "SPAWNMODE_VENT|Wall") == 0) wallKey = row.survival;
    unsigned matches = 0;
    for (const auto& key : ds2::kSurvivalDonorKeys) if (wallKey && std::strcmp(key.survival, wallKey) == 0) ++matches;
    assert(matches >= 20);
    unsigned engines = 0;
    for (const auto& e : ds2::kSurvivalEngines) {
        assert(std::strcmp(e.targetClass, "DamageableProp") == 0 || std::strcmp(e.targetClass, "ScriptableProp") == 0);
        if (std::strcmp(e.map, "mp_bombRun") == 0) ++engines;
    }
    assert(engines == 14);
    // the vents' selection windows: Marker Lab opens one vent at the start, three
    // more with vents1_off (one through its culling group) and the last with vents2_off
    auto nameHash = [](const char* s) {
        uint32_t h = 0;
        for (; *s; ++s) h = h * 0x1003Fu + static_cast<uint32_t>(std::tolower(static_cast<unsigned char>(*s)));
        return h;
    };
    unsigned govStart = 0, govSecond = 0, govLast = 0, govInherited = 0;
    for (const auto& row : ds2::kSurvivalVents) {
        assert(row.selectOn == (row.onName[0] ? nameHash(row.onName) : 0u) && row.selectOff == (row.offName[0] ? nameHash(row.offName) : 0u));
        if (std::strcmp(row.map, "mp_bombRun") == 0) assert(!row.selectOn && !row.selectOff);   // all five sit in the hub
        if (std::strcmp(row.map, "mp_gov") != 0) continue;
        if (row.onAtStart && row.selectOff == nameHash("vents1_off")) ++govStart;
        if (row.selectOn == nameHash("vents1_off") && row.selectOff == nameHash("vents2_off") && !row.onAtStart) ++govSecond;
        if (row.selectOn == nameHash("vents2_off") && !row.selectOff) ++govLast;
        if (row.inherited) ++govInherited;
    }
    assert(govStart == 1 && govSecond == 3 && govLast == 1 && govInherited == 1);
    // every VentMarker: Marker Lab has 18, four open from the round start (three wall, one floor)
    unsigned govMarkers = 0, govAtStart = 0, govAtStartWall = 0, ownFound = 0;
    for (const auto& m : ds2::kSurvivalVentMarkers) {
        assert(std::isfinite(m.x) && std::isfinite(m.y) && std::isfinite(m.z));
        if (std::strcmp(m.map, "mp_gov") != 0) continue;
        ++govMarkers;
        if (m.onAtStart) { ++govAtStart; if (m.wall) ++govAtStartWall; }
    }
    // an MP row's own vent is a marker of its map; a campaign arena's row may borrow a
    // spawner whose own vent lies elsewhere, so it needs a marker of its kind in the arena
    unsigned mpRows = 0, arenaRows = 0, arenaUsable = 0;
    for (const auto& row : ds2::kSurvivalVents) {
        const size_t len = std::strlen(row.context);
        const bool wallRow = len >= 5 && std::strcmp(row.context + len - 5, "|Wall") == 0;
        if (std::strncmp(row.map, "arena_", 6) == 0) {
            ++arenaRows;
            for (const auto& m : ds2::kSurvivalVentMarkers)
                if (std::strcmp(m.map, row.map) == 0 && m.wall == wallRow) { ++arenaUsable; assert(m.onAtStart && !m.selectOn && !m.selectOff); break; }
            continue;
        }
        ++mpRows;
        for (const auto& m : ds2::kSurvivalVentMarkers)
            if (std::strcmp(m.map, row.map) == 0 && m.vent == row.vent) { ++ownFound; assert(m.selectOn == row.selectOn && m.selectOff == row.selectOff); break; }
    }
    assert(govMarkers == 18 && govAtStart == 4 && govAtStartWall == 3 && ownFound == mpRows);
    assert(arenaRows > 0 && arenaUsable == arenaRows);
    assert(ParseConfig("[Survival]\n").ventAnyMarker && !ParseConfig("[Survival]\nVentAnyMarker=0\n").ventAnyMarker);
    std::printf("tables ok (bombRun vents=%u wall=%u wall_donors=%u engines=%u; mp_gov vents %u/%u/%u)\n", bomb, wall, matches, engines, govStart, govSecond,
                govLast);
}
}  // namespace

void Mercenaries() {
    // defaults, the shipped template and the edges
    const Config none = ParseConfig("[Survival]\n");
    assert(none.mercs.startSeconds == 150 && none.mercs.capsules == 2 && none.mercs.capsuleTotal == 8 && none.mercs.maxRunS == 900 && none.mercs.comboStep == 10 && none.mercs.finalEnemies.empty());
    const Config shipped = ParseConfig(ReadFile("DS2Survival.ini"));
    assert(shipped.mercs.startSeconds == 150 && shipped.mercs.capsuleMs == 30000 && !shipped.mercs.finalEnemies.empty());
    const Config odd = ParseConfig("[Mercenaries]\nStartSeconds=5\nMaxSeconds=10\nFinalEnemies=Zombie:abc\n");
    assert(odd.mercs.startSeconds == 30 && odd.mercs.maxSeconds == 30 && odd.mercs.finalEnemies.empty() && !odd.warnings.empty());

    // kills chain inside the window; every 10th chained kill pays the combo bonus
    MercsScore s;
    bool combo = false;
    unsigned bonus = 0;
    for (unsigned i = 0; i < 9; ++i) {
        bonus = s.Kill("Zombie", false, 1000 + i * 2000, 1000, 5000, 3000, 10, 10000, combo);
        assert(bonus == 1000 && !combo);
    }
    bonus = s.Kill("Zombie", false, 1000 + 9 * 2000, 1000, 5000, 3000, 10, 10000, combo);
    assert(bonus == 11000 && combo && s.combo == 10 && s.maxCombo == 10);
    assert(s.score == 9 * 100 + 100 * 2);   // the tenth kill counts double
    // a gap longer than the window starts over; an elite gives its own time
    bonus = s.Kill("Hunter", true, 1000 + 9 * 2000 + 3001, 1000, 5000, 3000, 10, 10000, combo);
    assert(bonus == 5000 && !combo && s.combo == 1 && s.maxCombo == 10 && s.kills == 11);
    assert(s.score == 1100 + 1500 * 2);
    // ranks and the clock
    const unsigned ranks[5] = {20000, 40000, 70000, 100000, 150000};
    assert(std::strcmp(MercsRank(0, ranks), "D") == 0 && std::strcmp(MercsRank(20000, ranks), "C") == 0 && std::strcmp(MercsRank(99999, ranks), "A") == 0 &&
           std::strcmp(MercsRank(150000, ranks), "SS") == 0);
    assert(MercsClock(10000, -15000, 600000) == 0 && MercsClock(590000, 30000, 600000) == 600000 && MercsClock(1000, 2000, 600000) == 3000);
    // bonuses: full at the start, halfway down at half the fade, the floor from then on
    assert(FadedBonusMs(30000, 0, 480, 20) == 30000 && FadedBonusMs(30000, 240000, 480, 20) == 18000);
    assert(FadedBonusMs(30000, 480000, 480, 20) == 6000 && FadedBonusMs(30000, 3600000, 480, 20) == 6000);
    assert(FadedBonusMs(30000, 999999, 0, 20) == 30000 && FadedBonusMs(30000, 999999, 480, 100) == 30000);
    assert(FamilyPoints("hunter") == 1500 && FamilyPoints("Unknown") == 100);
    std::printf("mercenaries ok\n");
}

void Objective() {
    const Config none = ParseConfig("[Survival]\n");
    assert(none.objective.startPercent == 100 && none.objective.bonusPercent == 100 && none.objective.stepBonusS == 30 && none.objective.restart == 1);
    const Config shipped = ParseConfig(ReadFile("DS2Survival.ini"));
    assert(shipped.objective.startDelayS == 3 && shipped.objective.breatherS == 0 && shipped.objective.maxSeconds == 1800 && shipped.warnings.empty());
    assert(shipped.objective.defendDps == 12 && shipped.objective.defendRadiusM == 4 && none.objective.defendDps == 12);
    assert(none.objective.stepFloorS == 90 && shipped.objective.stepFloorS == 90 && ParseConfig("[Objective]\nStepFloorSeconds=0\n").objective.stepFloorS == 0);
    const Config odd = ParseConfig("[Objective]\nStartPercent=10\nRestartAfterResult=7\n");
    unsigned objectiveWarnings = 0;
    for (const auto& w : odd.warnings) objectiveWarnings += w.rfind("[Objective]", 0) == 0;
    assert(odd.objective.startPercent == 50 && odd.objective.restart == 1 && objectiveWarnings == 2);
    // the start: the round time scaled, at least a minute, at most MaxSeconds
    assert(ObjectiveStartMs(240.0f, 200, 1800) == 480000 && ObjectiveStartMs(120.0f, 50, 1800) == 60000 && ObjectiveStartMs(0.0f, 100, 1800) == 180000);
    assert(ObjectiveStartMs(240.0f, 1000, 1800) == 1800000);
    // a map bonus adds, a map set raises to at least, both within the maximum
    assert(ObjectiveBonusClock(100000, 195.0f, true, 200, 1800000) == 490000);
    assert(ObjectiveBonusClock(100000, 60.0f, false, 200, 1800000) == 120000 && ObjectiveBonusClock(300000, 60.0f, false, 200, 1800000) == 300000);
    assert(ObjectiveBonusClock(1790000, 30.0f, true, 100, 1800000) == 1800000);
    std::printf("objective ok\n");
}

void VersusSlashers() {
    const Config none = ParseConfig("[Survival]\n");
    const auto& d = none.versus;
    assert(d.enabled && d.alive == 5 && d.respawnMinS == 25 && d.respawnMaxS == 40 && d.health == 80 && d.superHealth == 200);
    assert(VersusSkinsFor(d, "mp_gov") == std::vector<std::string>{"slasher"} && VersusAliveFor(d, "pdlc_mp_school") == 5);
    // the shipped template: the user's skins per map (11.10.), Academy with three
    const Config shipped = ParseConfig(ReadFile("DS2Survival.ini"));
    assert(shipped.warnings.empty());
    const auto& s = shipped.versus;
    assert((VersusSkinsFor(s, "MP_BOMBRUN") == std::vector<std::string>{"slasher_miner", "slasher_var1", "slasher_var2"}));
    assert((VersusSkinsFor(s, "mp_gov") == std::vector<std::string>{"slasher_doctor", "slasher_hospital", "slasher_gov_worker", "slashersecurity"}));
    assert((VersusSkinsFor(s, "pdlc_mp_mall") == std::vector<std::string>{"slasher_male_civilian", "slasher_fem_civilian", "slasher_fem_eyes"}));
    assert((VersusSkinsFor(s, "mp_solar") == std::vector<std::string>{"slasher", "slasher_fanatic", "slasherclergy"}));
    assert(VersusAliveFor(s, "pdlc_mp_school") == 3 && VersusAliveFor(s, "mp_escape") == 5);
    const Config odd = ParseConfig("[VersusSlashers]\nRespawnMinSeconds=50\nRespawnMaxSeconds=20\nAlivePerMap=mp_gov:2, broken, mp_solar:99\n[VersusSkins]\nDefault=Slasher_Var2\nmp_escape= ,\n");
    unsigned versusWarnings = 0;
    for (const auto& w : odd.warnings) versusWarnings += w.find("Versus") != std::string::npos;
    assert(odd.versus.respawnMaxS == 50 && versusWarnings == 4 && VersusAliveFor(odd.versus, "MP_GOV") == 2 && VersusAliveFor(odd.versus, "mp_solar") == 5);
    assert(VersusSkinsFor(odd.versus, "mp_escape") == std::vector<std::string>{"slasher_var2"});
    assert(VersusRespawnMs(d, 0.0f) == 25000 && VersusRespawnMs(d, 1.0f) == 40000 && VersusRespawnMs(d, 0.5f) == 32500 && VersusRespawnMs(d, 7.0f) == 40000);

    // pressure: fast objectives raise it, slow ones do not, the top holds it
    VersusPressure p;
    p.Start(1000, d);
    assert(!p.Objective(61000, d) && p.level == 0);            // 60 s: slow
    assert(p.Objective(90000, d) && p.level == 1);             // 29 s: fast
    assert(p.SuperPercent(d) == 34 && p.Alive(5, d) == 6);
    assert(p.Objective(100000, d) && p.Objective(110000, d) && p.level == 3 && p.SuperPercent(d) == 100);
    assert(!p.Objective(120000, d) && p.level == 3 && p.changedAt == 120000);
    // it decays one level per minute without a raise
    assert(!p.Decay(179999, d) && p.Decay(180000, d) && p.level == 2);
    assert(!p.Decay(200000, d) && p.Decay(240000, d) && p.level == 1);
    // kills: every 4 more for the humans raise it once
    VersusPressure k;
    k.Start(0, d);
    assert(!k.Kills(3, 0, 1000, d) && k.Kills(5, 1, 2000, d) && k.level == 1 && k.killMark == 8);
    assert(!k.Kills(7, 0, 3000, d) && k.Kills(8, 0, 4000, d) && k.level == 2);
    // the necros catch up: the mark comes back down
    assert(!k.Kills(8, 8, 5000, d) && k.killMark == 4 && k.Kills(12, 8, 6000, d) && k.level == 3);
    Config::Versus off = d;
    off.killLead = 0;
    off.fastObjectiveS = 0;
    VersusPressure n;
    n.Start(0, off);
    assert(!n.Kills(20, 0, 1000, off) && !n.Objective(2000, off) && n.level == 0);

    // the skin bag: each once per round, no repeat across the round boundary
    Rng rng(7);
    const std::vector<std::string> skins{"a", "b", "c"};
    std::vector<std::string> bag;
    std::string last;
    for (int round = 0; round < 50; ++round) {
        std::set<std::string> seen;
        for (int i = 0; i < 3; ++i) {
            const std::string next = NextVersusSkin(bag, skins, last, rng);
            assert(next != last && seen.insert(next).second);
            last = next;
        }
    }
    std::vector<std::string> one;
    assert(NextVersusSkin(one, {"x"}, "x", rng) == "x" && NextVersusSkin(one, {}, "", rng).empty());
    // every Zombie pool target's model has its skin row, under the same package
    unsigned zombies = 0;
    for (const auto& row : ds2::kGlobalTargets) {
        if (std::strcmp(row.family, "Zombie") != 0) continue;
        ++zombies;
        const ds2::SlasherSkinRow* skin = nullptr;
        for (const auto& s2 : ds2::kSlasherSkins) if (s2.model == row.model) skin = &s2;
        assert(skin && std::strcmp(skin->package, row.package) == 0);
    }
    assert(zombies >= 16);
    std::printf("versus slashers ok (%u zombie pool targets, %zu skins)\n", zombies, std::size(ds2::kSlasherSkins));
}

void Zone() {
    const Config none = ParseConfig("[Survival]\n");
    assert(none.zone.startSeconds == 150 && none.zone.captureS == 30 && none.zone.radiusM == 4.5f && none.zone.contestEnemies == 2 && none.zone.targetZones == 0);
    const Config shipped = ParseConfig(ReadFile("DS2Survival.ini"));
    assert(shipped.warnings.empty() && shipped.zone.bonusS == 60 && shipped.zone.captureS == 30 && shipped.zone.decayPercent == 50);
    const Config odd = ParseConfig("[Zone]\nRadiusMeters=50\nCaptureSeconds=900\n");
    unsigned zoneWarnings = 0;
    for (const auto& w : odd.warnings) zoneWarnings += w.rfind("[Zone]", 0) == 0;
    assert(odd.zone.radiusM == 12.0f && odd.zone.captureS == 300 && zoneWarnings == 1);
    // alone: 25 s to fill; a second player adds half the speed
    float p = 0.0f;
    assert(ZoneStep(p, 1, 0, 12500, 25, 50, 50, 1) == ZoneMode::Capturing && std::fabs(p - 0.5f) < 1e-4f);
    float q = 0.0f;
    ZoneStep(q, 2, 0, 10000, 25, 50, 50, 1);
    assert(std::fabs(q - 0.6f) < 1e-4f);
    // contested: it stands; nobody inside: it drains at half speed, then is empty
    assert(ZoneStep(p, 3, 1, 5000, 25, 50, 50, 1) == ZoneMode::Contested && std::fabs(p - 0.5f) < 1e-4f);
    assert(ZoneStep(p, 3, 1, 5000, 25, 50, 50, 2) == ZoneMode::Capturing);
    float r = 0.1f;
    assert(ZoneStep(r, 0, 4, 2500, 25, 50, 50, 1) == ZoneMode::Losing && std::fabs(r - 0.05f) < 1e-4f);
    assert(ZoneStep(r, 0, 0, 5000, 25, 50, 50, 1) == ZoneMode::Empty && r == 0.0f);
    float keep = 0.3f;
    assert(ZoneStep(keep, 0, 0, 5000, 25, 50, 0, 1) == ZoneMode::Empty && keep == 0.3f);
    float full = 0.95f;
    ZoneStep(full, 1, 0, 5000, 25, 50, 50, 1);
    assert(full == 1.0f);
    // inside: a cylinder, the radius on the floor plan, 0.5 m below to 1 m above the spot
    const V3 c{10.0f, 2.0f, -5.0f};
    assert(InZone(c, 4.5f, {13.0f, 2.5f, -2.0f}) && InZone(c, 4.5f, {10.0f, 2.9f, -5.0f}) && InZone(c, 4.5f, {10.0f, 1.6f, -5.0f}));
    assert(!InZone(c, 4.5f, {14.0f, 2.0f, -1.0f}) && !InZone(c, 4.5f, {10.0f, 3.2f, -5.0f}) && !InZone(c, 4.5f, {10.0f, 1.4f, -5.0f}));
    const float cc[3] = {10.0f, 2.0f, -5.0f}, upper[3] = {10.0f, 3.2f, -5.0f}, lower[3] = {10.0f, 1.6f, -5.0f};
    assert(!coop_zone::Inside(cc, 4.5f, upper) && coop_zone::Inside(cc, 4.5f, lower));
    // the way: facing +z, +x is on the left (left = up x forward)
    const float self[3] = {0.0f, 0.0f, 0.0f}, facing[3] = {0.0f, 0.0f, 1.0f};
    const float ahead[3] = {1.0f, 0.0f, 20.0f}, left[3] = {20.0f, 0.0f, 1.0f}, right[3] = {-20.0f, 0.0f, 2.0f}, behind[3] = {0.0f, 5.0f, -20.0f};
    const char* vertical = "";
    assert(std::strcmp(coop_zone::Bearing(self, facing, ahead, &vertical), "AHEAD") == 0 && !vertical[0]);
    assert(std::strcmp(coop_zone::Bearing(self, facing, left, &vertical), "LEFT") == 0);
    assert(std::strcmp(coop_zone::Bearing(self, facing, right, &vertical), "RIGHT") == 0);
    assert(std::strcmp(coop_zone::Bearing(self, facing, behind, &vertical), "BEHIND") == 0 && std::strcmp(vertical, "UP") == 0);
    char text[40];
    coop_zone::Text(text, sizeof(text), self, facing, behind, 4.5f);
    assert(std::strcmp(text, "ZONE 21 M BEHIND UP") == 0);
    const float close[3] = {1.0f, 0.5f, 1.0f};
    coop_zone::Text(text, sizeof(text), close, facing, self, 4.5f);
    assert(std::strcmp(text, "IN THE ZONE") == 0);
    // recorded spots: this map's lines, commas allowed, a radius only within 2..12
    unsigned skipped = 0, other = 0;
    const auto spots = ParseZoneSpots("# header\nmp_bombRun 1.5 2.0 -3.25\nMP_BOMBRUN, 10, 0, 10, 6 ; big one\nmp_gov 0 0 0\nmp_bombRun 4 5\n"
                                      "mp_bombRun 20 0 20 50\n\n",
                                      "mp_bombrun", skipped, other);
    assert(spots.size() == 3 && skipped == 1 && other == 1);
    assert(spots[0].pos.z == -3.25f && spots[0].radius == 0.0f && spots[1].radius == 6.0f && spots[2].radius == 0.0f);
    // the next spot: any but the last
    assert(PickZoneSpot({}, -1, 7) == -1 && PickZoneSpot({spots[0]}, 0, 7) == 0);
    bool sawFirst = false, sawThird = false;
    for (uint32_t seed = 0; seed < 8; ++seed) {
        const int next = PickZoneSpot(spots, 1, seed);
        assert(next == 0 || next == 2);
        sawFirst = sawFirst || next == 0;
        sawThird = sawThird || next == 2;
    }
    assert(sawFirst && sawThird);
    std::printf("zone ok\n");
}

void Marks() {
    using coop_marks::Camera;
    using coop_marks::Place;
    // at the origin, looking along +z (x left, y up), 90 degrees across, 16:9
    Camera c{};
    c.axis[0][0] = c.axis[1][1] = c.axis[2][2] = 1.0f;
    c.tanX = 1.0f;
    c.tanY = 9.0f / 16.0f;
    const float ahead[3] = {0.0f, 0.0f, 10.0f}, right[3] = {-5.0f, 0.0f, 10.0f}, up[3] = {0.0f, 2.0f, 10.0f}, farLeft[3] = {40.0f, 0.0f, 10.0f};
    const float behindLeft[3] = {1.0f, 0.0f, -10.0f}, behind[3] = {0.0f, 0.0f, -10.0f};
    auto a = Place(c, ahead, 1920.0f, 1080.0f, 50.0f);
    assert(a.onScreen && std::fabs(a.x - 960.0f) < 0.01f && std::fabs(a.y - 540.0f) < 0.01f);
    a = Place(c, right, 1920.0f, 1080.0f, 50.0f);   // -x is on the right: half way to the edge
    assert(a.onScreen && std::fabs(a.x - 1440.0f) < 0.01f && std::fabs(a.y - 540.0f) < 0.01f);
    a = Place(c, up, 1920.0f, 1080.0f, 50.0f);
    assert(a.onScreen && std::fabs(a.y - 540.0f * (1.0f - 0.2f / 0.5625f)) < 0.1f);
    a = Place(c, farLeft, 1920.0f, 1080.0f, 50.0f);   // off to the left: on the left edge, the arrow pointing left
    assert(!a.onScreen && std::fabs(a.x - 50.0f) < 0.01f && std::fabs(a.y - 540.0f) < 0.01f && a.dirX < -0.99f);
    a = Place(c, behindLeft, 1920.0f, 1080.0f, 50.0f);
    assert(!a.onScreen && a.dirX < -0.99f && std::fabs(a.x - 50.0f) < 0.01f);
    a = Place(c, behind, 1920.0f, 1080.0f, 50.0f);   // straight behind: the bottom edge
    assert(!a.onScreen && a.dirY > 0.99f && std::fabs(a.y - 1030.0f) < 0.01f);
    // standing at (5 0 0), looking along +x: left is -z
    Camera t{};
    t.axis[0][2] = -1.0f;
    t.axis[1][1] = 1.0f;
    t.axis[2][0] = 1.0f;
    t.pos[0] = 5.0f;
    t.tanX = 1.0f;
    t.tanY = 0.75f;
    const float there[3] = {15.0f, 0.0f, 0.0f}, toLeft[3] = {15.0f, 0.0f, -3.0f};
    a = Place(t, there, 800.0f, 600.0f, 40.0f);
    assert(a.onScreen && std::fabs(a.x - 400.0f) < 0.01f && std::fabs(a.y - 300.0f) < 0.01f);
    a = Place(t, toLeft, 800.0f, 600.0f, 40.0f);
    assert(a.onScreen && std::fabs(a.x - 400.0f * (1.0f - 0.3f)) < 0.01f);
    std::printf("marks ok\n");
}

// [MapFamilies]: only these on a map, or all but the ones with a '-'; player names work too.
void MapFamilies() {
    const Config c = ParseConfig("[Survival]\nEnabled=1\n[Wave1]\nEnemies=Zombie:3\n[MapFamilies]\n"
                                 "mp_bombRun=slasher, Puker, pack\narena_ch10=-Brute, -ubermorph -divider\nmp_gov=Nonsense\n");
    assert(MapAllowsFamily(c, "mp_bombRun", "Zombie") && MapAllowsFamily(c, "MP_BOMBRUN", "PackEnemy") && MapAllowsFamily(c, "mp_bombRun", "Puker"));
    assert(!MapAllowsFamily(c, "mp_bombRun", "Brute") && !MapAllowsFamily(c, "mp_bombRun", "Leaper"));
    assert(MapAllowsFamily(c, "arena_ch10", "Zombie") && !MapAllowsFamily(c, "arena_ch10", "Brute") && !MapAllowsFamily(c, "arena_ch10", "Hunter") &&
           !MapAllowsFamily(c, "arena_ch10", "DividerCore"));
    assert(MapAllowsFamily(c, "mp_escape", "Brute"));   // no rule: everything
    assert(MapAllowsFamily(c, "mp_gov", "Brute") && !c.warnings.empty());   // an unknown name is warned about and ignored
    std::printf("map families ok\n");
}

int main() {
    assert(!DeadlinePending(0x80000001u, 0));
    const uint32_t wrapped = DeadlineAfter(0xfffffff0u, 32);
    assert(DeadlinePending(0xfffffff0u, wrapped) && DeadlinePending(15, wrapped) && !DeadlinePending(16, wrapped));
    assert(DeadlineAfter(0xfffffff0u, 16) == 1);
    const uint32_t ptr = 0x1000, vt = 0x2000;
    const std::array<uint32_t, 4> expected{1, 2, 3, 4};
    auto current = expected;
    bool readable = true;
    auto read = [&](uint32_t addr, void* dst, size_t size) {
        if (addr == ptr && size == sizeof(vt)) { std::memcpy(dst, &vt, size); return true; }
        if (!readable || addr != ptr + 0x24 || size != sizeof(current)) return false;
        std::memcpy(dst, current.data(), size);
        return true;
    };
    assert(MatchesEntity(ptr, vt, expected, 0x24, read));
    ++current[2];
    assert(!MatchesEntity(ptr, vt, expected, 0x24, read));
    current = expected;
    readable = false;
    assert(!MatchesEntity(ptr, vt, expected, 0x24, read));
    readable = true;
    current = {};
    assert(!MatchesEntity(ptr, vt, current, 0x24, read));

    survival_capsules::Snapshot host;
    host.seq = 1; host.token = 7;
    for (uint32_t i = 1; i <= 20; ++i) host.Apply(5, {i, {2, 3, 4}, 0});
    survival_capsules::Replica client;
    assert(client.Receive(host));
    int kind = 0;
    survival_capsules::Item capsule{};
    for (uint32_t i = 1; i <= 20; ++i) assert(client.Take(kind, capsule) && kind == 5 && capsule.id == i);
    assert(!client.Take(kind, capsule));
    auto old = host;
    host.Apply(6, {3, {}, 0}); host.Apply(6, {19, {}, 0}); ++host.seq;
    assert(client.Receive(host) && !client.Receive(old) && !client.Receive(host));
    assert(client.Take(kind, capsule) && kind == 6 && capsule.id == 3);
    // Another snapshot arrives before the client has finished applying the previous one.
    ++host.seq; assert(client.Receive(host));
    assert(client.Take(kind, capsule) && kind == 6 && capsule.id == 19);
    while (client.Take(kind, capsule)) assert(kind == 5 && capsule.id != 3 && capsule.id != 19);
    host.Apply(6, {}); ++host.seq; assert(client.Receive(host));
    unsigned removed = 0;
    while (client.Take(kind, capsule)) { assert(kind == 6); ++removed; }
    assert(removed == 18);
    old.seq = 123; old.items[1] = old.items[0]; assert(!client.Receive(old));
    old.count = 99; assert(!client.Receive(old));
    host.token = 9; host.seq = 1; host.Apply(5, {1, {3, 2, 1}, 1});
    assert(client.Receive(host));
    assert(client.Take(kind, capsule) && kind == 6 && capsule.id == 0);
    assert(client.Take(kind, capsule) && kind == 5 && capsule.id == 1 && capsule.golden);
    survival::RewardPartner participant, joiner;
    survival::RewardReceiver follower;
    survival::RewardQueue received;
    participant.Bind(22);
    assert(participant.pending.Push(7) && participant.pending.Push(9));
    joiner.Bind(33); // Joined after both grants.
    assert(participant.Hello(follower.generation) && joiner.Hello(1));
    survival::RewardWire grant, late;
    bool added = false;
    assert(!joiner.Grant(11, 3, 1000, late));
    for (uint32_t now = 1000; now <= 2000; now += 500) assert(participant.Grant(11, 3, now, grant)); // All lost.
    assert(!participant.Grant(11, 3, 2100, grant));
    assert(participant.Grant(11, 3, 2500, grant) && grant.wave == 7);
    assert(!follower.Accept(grant, 33, received, added));
    assert(follower.Accept(grant, 22, received, added) && added);
    auto ack = grant; ack.kind = survival::RewardKind::Ack; ack.sender = grant.target; ack.target = grant.sender;
    for (uint32_t now = 3000; now <= 4000; now += 500) {
        assert(participant.Grant(11, 3, now, grant));
        assert(follower.Accept(grant, 22, received, added) && !added); // Earlier acknowledgments lost.
    }
    assert(received.count == 1 && participant.Ack(ack, 3) && !participant.Ack(ack, 3));
    assert(participant.Grant(11, 3, 4050, grant) && grant.wave == 9);
    assert(follower.Accept(grant, 22, received, added) && added);
    follower.Restart(); received.Clear();
    assert(!follower.Accept(grant, 22, received, added));
    assert(participant.Hello(follower.generation) && !participant.Hello(1));
    assert(!participant.Grant(11, 3, 6000, grant));
    assert(participant.pending.Push(15) && participant.Grant(11, 3, 6000, grant));
    participant.Restart(); // Host starts a new run while an old grant is in flight.
    assert(follower.ObserveRun(11, 4, received) && !follower.Accept(grant, 22, received, added));
    assert(participant.pending.Push(1) && participant.Grant(11, 4, 6010, late));
    assert(follower.Accept(late, 22, received, added) && added && !follower.Accept(grant, 22, received, added));
    assert(!participant.Ack(ack, 4));
    assert(received.count == 1);
    survival::RewardQueue owed;
    uint32_t wave = 0;
    assert(!owed.Take(wave));
    for (uint32_t i = 0; i < 64; ++i) assert(owed.Push(i));
    assert(!owed.Push(70));
    for (uint32_t i = 0; i < 30; ++i) { assert(owed.Take(wave) && wave == i); assert(owed.Push(i + 64)); }
    for (uint32_t i = 30; i < 94; ++i) assert(owed.Take(wave) && wave == i);
    assert(!owed.Take(wave));
    assert(owed.Push(33)); owed.Clear();
    assert(!owed.Take(wave) && owed.Push(1) && owed.Take(wave) && wave == 1);
    IniReader();
    Mercenaries();
    Objective();
    Zone();
    Marks();
    VersusSlashers();
    Entries();
    Template();
    ConfigEdges();
    Endless();
    Substitution_();
    Queue();
    Pacing();
    Points();
    VentChoice();
    Stuck();
    Rewards();
    Escalation();
    MapFamilies();
    Best();
    Rng_();
    Tables();
    std::printf("survival tests passed\n");
    return 0;
}
