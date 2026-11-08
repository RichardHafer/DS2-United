// Dead Space 2 United by Richard Hafer.
// The Versus roster without a game: refs, the roster table, the team split,
// round 2's swap and the MP level from XP.
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "../src/coop_mpnative.h"

using namespace coop_mpnative;

namespace {
Entry Make(unsigned machine, const char* name, uint32_t xp = 0) {
    Entry e{};
    e.machine = static_cast<uint8_t>(machine);
    e.xp = xp;
    CopyName(e.name, name);
    return e;
}

void Refs() {
    assert(RefFor(0) == 0x4453324D00000001ull && RefFor(7) == 0x4453324D00000008ull);
    assert(MachineOfRef(RefFor(0)) == 0 && MachineOfRef(RefFor(255)) == 255);
    assert(MachineOfRef(0) == -1 && MachineOfRef(0x4453324D00000000ull) == -1 && MachineOfRef(0x1234000000000003ull) == -1);
    std::printf("refs ok\n");
}

void Table() {
    Roster r;
    bool changed = false;
    assert(r.Upsert(Make(3, "Clarke"), &changed) && changed && r.count == 1 && r.entries[0].team == kNoTeam);
    changed = false;
    assert(r.Upsert(Make(3, "Clarke"), &changed) && !changed);
    r.entries[0].team = kNecro;
    assert(r.Upsert(Make(3, "Clarke", 9000), &changed) && changed && r.entries[0].xp == 9000 && r.entries[0].team == kNecro);
    for (unsigned m = 0; m < 8; ++m) r.Upsert(Make(m, "x"));
    assert(r.count == 8 && !r.Upsert(Make(9, "full")));
    assert(r.Remove(3) && r.count == 7 && !r.Find(3) && r.Find(7) && !r.Remove(3));
    char name[24];
    CopyName(name, "A\x01very long name with \x7f odd bytes 1234567890");
    assert(std::strlen(name) == 23 && name[0] == 'A' && name[1] == 'v');
    std::printf("table ok\n");
}

void Teams() {
    Roster r;
    for (unsigned m : {5u, 0u, 2u, 7u, 1u}) r.Upsert(Make(m, "p"));
    assert(AssignTeams(r, 0) && !AssignTeams(r, 0));
    // five players, half rounded up: machines 0, 1, 2 are humans
    assert(r.Find(0)->team == kHuman && r.Find(1)->team == kHuman && r.Find(2)->team == kHuman && r.Find(5)->team == kNecro && r.Find(7)->team == kNecro);
    assert(r.TeamCount(kHuman, 1) == 3 && r.TeamCount(kNecro, 1) == 2);
    // round 2 swaps the sides without touching the roster
    assert(r.TeamCount(kHuman, 2) == 2 && r.TeamCount(kNecro, 2) == 3 && EffectiveTeam(kHuman, 2) == kNecro && EffectiveTeam(kNoTeam, 2) == kNoTeam);
    // a fixed number of humans, never more than the players
    assert(AssignTeams(r, 1) && r.TeamCount(kHuman, 1) == 1 && r.Find(0)->team == kHuman);
    AssignTeams(r, 9);
    assert(r.TeamCount(kHuman, 1) == 5);
    Roster solo;
    solo.Upsert(Make(0, "solo"));
    AssignTeams(solo, 0);
    assert(solo.entries[0].team == kHuman);
    std::printf("teams ok\n");
}

void Levels() {
    assert(LevelForXp(0) == 1 && LevelForXp(4999) == 1 && LevelForXp(5000) == 2 && LevelForXp(10499) == 2);
    assert(LevelForXp(2224749) == 59 && LevelForXp(2224750) == 60 && LevelForXp(0xFFFFFFFFu) == 60);
    for (unsigned i = 1; i < 60; ++i) assert(kLevelScore[i] > kLevelScore[i - 1]);
    std::printf("levels ok\n");
}
}  // namespace

static void Late() {
    using namespace coop_mpnative;
    const unsigned two[] = {0, 1};
    assert(LateTeam(two, 2, 1, 0, 1, 0) == kNecro && LateTeam(two, 2, 0, 0, 0, 0) == kHuman);
    const unsigned three[] = {0, 2, 1};   // the lobby's order does not matter
    assert(LateTeam(three, 3, 1, 0, 1, 0) == kHuman && LateTeam(three, 3, 2, 0, 2, 0) == kNecro);
    const unsigned four[] = {0, 1, 2, 3};
    assert(LateTeam(four, 4, 1, 0, 1, 0) == kHuman && LateTeam(four, 4, 2, 0, 2, 0) == kNecro && LateTeam(four, 4, 3, 0, 2, 1) == kNecro);
    assert(LateTeam(four, 4, 1, 1, 1, 0) == kNecro);   // one human wanted
    // a machine the lobby never had: the smaller side, a tie to the humans
    assert(LateTeam(two, 2, 5, 0, 1, 1) == kHuman && LateTeam(two, 2, 5, 0, 2, 1) == kNecro && LateTeam(two, 2, 5, 3, 2, 1) == kHuman);
    std::printf("late teams ok\n");
}

int main() {
    Refs();
    Table();
    Teams();
    Late();
    Levels();
    std::printf("mpnative tests passed\n");
    return 0;
}
