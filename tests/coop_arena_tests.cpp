// Campaign arenas: names, the area test, the member merge and the generated table.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "../src/coop_arena.h"

namespace {
using namespace coop_arena;

void Names() {
    assert(IsArenaName("arena_ch10") && IsArenaName("ARENA_x") && !IsArenaName("arena_") && !IsArenaName("mp_gov") && !IsArenaName(nullptr));
    assert(ArenaIndex("nope") < 0 && ArenaIndex(nullptr) < 0);
    for (unsigned i = 0; i < kArenaCount; ++i) {
        assert(ArenaIndex(kArenas[i].name) == static_cast<int>(i));
        assert(IsArenaName(kArenas[i].name) && !coop_mpmap::IsMpMap(kArenas[i].name) && coop_mpmap::MapIndex(kArenas[i].name) < 0);
    }
}

void Table() {
    for (unsigned i = 0; i < kArenaCount; ++i) {
        const Arena& a = kArenas[i];
        assert(a.label[0] && a.level[0] && a.stream[0]);
        assert(a.checkpoint[0] | a.checkpoint[1] | a.checkpoint[2] | a.checkpoint[3]);
        assert(a.setCount >= 1 && a.setCount <= 32 && a.memberCount >= 1 && a.memberCount <= 96);
        // every set's members are valid indices, every walk sample has a set
        assert(a.setStart && a.setMember && a.walkSet && a.setStart[0] == 0 && a.streaming <= 1);
        for (unsigned s = 0; s < a.setCount; ++s) {
            assert(a.setStart[s] <= a.setStart[s + 1]);
            for (unsigned k = a.setStart[s]; k < a.setStart[s + 1]; ++k) assert(a.setMember[k] < a.memberCount);
        }
        for (unsigned w = 0; w < a.walkCount; ++w) assert(a.walkSet[w] < a.setCount);
        // a walk sample belongs to its own set, and each set holds members
        for (unsigned w = 0; w < a.walkCount; ++w) {
            const int s = SetForPoint(a, a.walk[w]);
            assert(s >= 0 && s < static_cast<int>(a.setCount));
        }
        bool selected[96]{};
        for (unsigned s = 0; s < a.setCount; ++s) assert(SelectMembers(a, 1u << s, selected, 96) == static_cast<unsigned>(a.setStart[s + 1] - a.setStart[s]));
        const uint32_t all = a.setCount >= 32 ? 0xFFFFFFFFu : (1u << a.setCount) - 1u;
        assert(SelectMembers(a, all, selected, 96) == a.memberCount);   // the sets cover every member
        assert(a.spotCount >= 1 && a.walkCount >= a.spotCount);
        unsigned locks = 0;
        for (unsigned d = 0; d < a.doorCount; ++d) locks += a.doors[d].lock ? 1u : 0u;
        assert(locks >= 1);
        // every spot and every walk sample is inside its own arena
        for (unsigned s = 0; s < a.spotCount; ++s) assert(Inside(a, a.spots[s]));
        for (unsigned w = 0; w < a.walkCount; ++w) assert(Inside(a, a.walk[w]));
        // no duplicate members
        for (unsigned m = 0; m < a.memberCount; ++m) assert(Find(a.members, m, a.members[m]) < 0);
        // the story logic: sorted, every entry found, no door and no member listed
        for (unsigned b = 0; b < a.blockCount; ++b) {
            assert(a.blocks[b].kind == 1 || a.blocks[b].kind == 2);
            if (b) assert(coop_mpmap::Compare(a.blocks[b - 1].guid, a.blocks[b].guid) < 0);
            const char* cls = nullptr;
            assert(BlockKind(a, a.blocks[b].guid, &cls) == a.blocks[b].kind && cls == a.blocks[b].cls);
        }
        for (unsigned d = 0; d < a.doorCount; ++d) assert(BlockKind(a, a.doors[d].guid) == 0);
        assert(BlockKind(a, a.checkpoint) == 0);
        const uint32_t nobody[4] = {0, 0, 0, 0};
        assert(BlockKind(a, nobody) == 0);
    }
}

void Area() {
    static const float walk[][3] = {{0, 0, 0}, {10, 0, 0}, {10, -4, 10}};
    static const uint32_t sets[][4] = {{1, 2, 3, 4}};
    static const uint32_t members[][4] = {{5, 6, 7, 8}};
    static const ds2arena::Door doors[] = {{{9, 9, 9, 9}, {0, 0, 0}, true, "test"}};
    static const float spots[][3] = {{0, 0, 0}};
    const Arena a{"arena_t", "t", "ds_2", "ds_2.str", {1, 1, 1, 1}, sets, 1, members, 1, doors, 1, spots, 1, walk, 3, {0, -4, 0, 10, 0, 10}};
    const float near[3] = {3, 0.5f, 2}, far[3] = {-20, 0, 0}, above[3] = {0, 5, 0}, between[3] = {1, -3, 1};
    const float nan[3] = {std::nanf(""), 0, 0}, lower[3] = {10, -4.2f, 9};
    assert(Inside(a, near) && !Inside(a, far) && !Inside(a, above) && Inside(a, lower) && !Inside(a, nan));
    assert(!Inside(a, between));   // in the box, but 3 m below the only sample in reach
    Reach wide;
    wide.vertical = 3.0f;
    assert(Inside(a, between, wide));
}

void Merging() {
    static const float walk[][3] = {{0, 0, 0}};
    static const uint32_t members[][4] = {{1, 0, 0, 0}, {2, 0, 0, 0}, {3, 0, 0, 0}};
    const Arena a{"arena_m", "m", "ds_2", "ds_2.str", {1, 1, 1, 1}, members, 3, members, 3, nullptr, 0, walk, 1, walk, 1, {0, 0, 0, 0, 0, 0}};
    uint32_t list[4][4] = {{2, 0, 0, 0}, {9, 0, 0, 0}};
    unsigned char hidden[4] = {1, 1};
    unsigned count = 2;
    const Merge m = MergeMembers(a, list, hidden, count, 4);
    assert(m.added == 2 && m.unhidden == 1 && m.full == 0 && count == 4);
    assert(hidden[0] == 0 && hidden[1] == 1 && hidden[2] == 0 && hidden[3] == 0);   // the set's own non-arena member keeps its flag
    assert(list[2][0] == 1 && list[3][0] == 3);
    // again: nothing changes
    const Merge again = MergeMembers(a, list, hidden, count, 4);
    assert(again.added == 0 && again.unhidden == 0 && again.full == 0 && count == 4);
    // no room
    uint32_t small[2][4] = {{9, 0, 0, 0}};
    unsigned char smallHidden[2] = {0};
    unsigned n = 1;
    const Merge cut = MergeMembers(a, small, smallHidden, n, 2);
    assert(cut.added == 1 && cut.full == 2 && n == 2);
    // only the selected members
    uint32_t some[4][4] = {{9, 0, 0, 0}};
    unsigned char someHidden[4] = {0};
    unsigned k = 1;
    const bool pick[3] = {false, true, false};
    const Merge part = MergeMembers(a, some, someHidden, k, 4, pick);
    assert(part.added == 1 && k == 2 && some[1][0] == 2);
    assert(NextSpot(a, 0) == 0);
}

// The game modes' points: one row per arena, every point in its arena, the mode list.
void Points() {
    assert(!PointsFor(nullptr) && !PointsFor("arena_nope") && !PointsFor("mp_gov"));
    for (unsigned i = 0; i < kArenaCount; ++i) {
        const Arena& a = kArenas[i];
        const ArenaPoints* p = PointsFor(a.name);
        assert(p && (p->modes & 1u) && !(p->modes >> 4 & 1u));   // survival always, Versus never
        for (int kind = kZones; kind <= kRespawns; ++kind) {
            unsigned n = 0;
            const ds2arena::Point* list = PointList(*p, kind, n);
            assert(list && n >= 1);
            for (unsigned k = 0; k < n; ++k) {
                assert(Inside(a, list[k].pos));
                assert(list[k].radius >= 0.0f && list[k].radius <= 12.0f);
            }
        }
        assert(ModeFor(p, 0) == 0 && ModeFor(p, 4) == 0 && ModeFor(p, -1) == 0 && ModeFor(p, 9) == 0);
        assert(ModeFor(p, 1) == ((p->modes & 2u) ? 1 : 0) && ModeFor(p, 3) == ((p->modes & 8u) ? 3 : 0));
        assert(ModeFor(nullptr, 1) == 0);
        // stepping visits only what the arena plays and comes back round
        int m = 0;
        for (int s = 0; s < 10; ++s) {
            m = StepMode(p, m, 1, 5);
            assert(m == 0 || ModeFor(p, m) == m);
        }
        assert(StepMode(p, 0, -1, 5) == ((p->modes & 8u) ? 3 : (p->modes & 2u) ? 1 : 0));
        assert(NearestRespawn(*p, nullptr) == 0);
        const float* last = p->respawns[p->respawnCount - 1].pos;
        assert(NearestRespawn(*p, last) == static_cast<int>(p->respawnCount - 1));
    }
    unsigned none = 0;
    const ds2arena::Point* nothing = PointList(ds2arena::kArenaPoints[0], 7, none);
    assert(!nothing && none == 0);
}

// Door marks: sorted, found by GUID, the key cycles through every state.
void DoorMarks() {
    static const ds2arena::DoorMark marks[] = {{{1, 2, 3, 4}, kDoorLocked, {0, 0, 0}}, {{1, 2, 3, 9}, kDoorHeld, {0, 0, 0}}, {{7, 0, 0, 0}, kDoorInner, {0, 0, 0}}};
    ArenaPoints p = ds2arena::kArenaPoints[0];
    p.doorMarks = marks;
    p.doorMarkCount = 3;
    const uint32_t a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 9}, c[4] = {7, 0, 0, 0}, x[4] = {1, 2, 3, 5};
    assert(DoorMarkFor(&p, a) == kDoorLocked && DoorMarkFor(&p, b) == kDoorHeld && DoorMarkFor(&p, c) == kDoorInner && DoorMarkFor(&p, x) == kDoorAuto);
    assert(DoorMarkFor(nullptr, a) == kDoorAuto);
    assert(NextDoorState(kDoorAuto) == kDoorInner && NextDoorState(kDoorInner) == kDoorLocked && NextDoorState(kDoorLocked) == kDoorHeld &&
           NextDoorState(kDoorHeld) == kDoorAuto);
    assert(std::strcmp(DoorStateName(kDoorHeld), "held") == 0 && std::strcmp(DoorStateName(9), "auto") == 0);
    // saved marks: the last line of a door wins, other lines are skipped, the table is bounded
    const char* saved = "# hand note\r\n"
                        "doormark 5A897DE3-D01AB7ED-52444F52-45554749 state=inner at=(199.25 -11.00 87.00)\r\n"
                        "doormark 5A9A7D82-AE0A9AD0-52444F52-45554749 state=locked at=(182.27 -15.00 86.68)\n"
                        "doormark 5A897DE3-D01AB7ED-52444F52-45554749 state=held at=(199.25 -11.00 87.00)\n"
                        "doormark 5A897DE3-D01AB7ED-52444F52-45554749 state=bogus\n"
                        "doormark broken\n"
                        "doormark 00000001-00000002-00000003-00000004 state=auto";
    SavedMark got[4]{};
    assert(ParseSavedMarks(saved, got, 4) == 3);
    assert(got[0].guid[0] == 0x5A897DE3u && got[0].guid[1] == 0xD01AB7EDu && got[0].state == kDoorHeld);
    assert(got[1].guid[0] == 0x5A9A7D82u && got[1].guid[3] == 0x45554749u && got[1].state == kDoorLocked);
    assert(got[2].guid[3] == 4u && got[2].state == kDoorAuto);
    assert(ParseSavedMarks(saved, got, 1) == 1 && got[0].state == kDoorHeld);
    assert(ParseSavedMarks("", got, 4) == 0 && ParseSavedMarks(nullptr, got, 4) == 0);
    // the generated marks are sorted and none is "auto"
    for (unsigned i = 0; i < ds2arena::kArenaPointsCount; ++i) {
        const ArenaPoints& q = ds2arena::kArenaPoints[i];
        for (unsigned k = 0; k < q.doorMarkCount; ++k) {
            assert(q.doorMarks[k].state >= kDoorInner && q.doorMarks[k].state <= kDoorHeld);
            if (k) assert(coop_mpmap::Compare(q.doorMarks[k - 1].guid, q.doorMarks[k].guid) < 0);
        }
    }
}

// doors=open holds every door that is not locked; exclude= names whole families.
void ArenaOptions() {
    Arena a = kArenas[0];
    a.doorsOpen = 0;
    a.exclude = nullptr;
    assert(DoorRule(a, kDoorAuto, true) == kDoorLocked && DoorRule(a, kDoorAuto, false) == kDoorInner && DoorRule(a, kDoorInner, true) == kDoorInner);
    assert(DoorRule(a, kDoorHeld, true) == kDoorHeld && !Excludes(a, "Brute"));
    a.doorsOpen = 1;
    assert(DoorRule(a, kDoorAuto, false) == kDoorHeld && DoorRule(a, kDoorInner, true) == kDoorHeld && DoorRule(a, kDoorAuto, true) == kDoorLocked &&
           DoorRule(a, kDoorLocked, false) == kDoorLocked);
    a.exclude = "Brute,Pregnant";
    assert(Excludes(a, "Brute") && Excludes(a, "brute") && Excludes(a, "Pregnant") && !Excludes(a, "Brut") && !Excludes(a, "Zombie") && !Excludes(a, "") &&
           !Excludes(a, nullptr));
    a.exclude = "";
    assert(!Excludes(a, "Brute"));
    // the church crypt: doors open, no Brutes
    const int crypt = ArenaIndex("arena_ch03");
    assert(crypt >= 0 && kArenas[crypt].doorsOpen == 1 && Excludes(kArenas[crypt], "Brute") && !Excludes(kArenas[crypt], "Zombie"));
    for (unsigned i = 0; i < kArenaCount; ++i) assert(kArenas[i].exclude && kArenas[i].doorsOpen <= 1);
    // heights=: the Ishimura only on a player's floor, the Sprawl hub nothing above, the rest any height; the tripod room vents only
    for (unsigned i = 0; i < kArenaCount; ++i) {
        const int want = std::strcmp(kArenas[i].name, "arena_ch08") == 0 ? 1 : std::strcmp(kArenas[i].name, "arena_ch05") == 0 ? 2 : 0;
        assert(kArenas[i].heights == want && kArenas[i].ventsOnly == (std::strcmp(kArenas[i].name, "arena_ch01") == 0 ? 1 : 0));
    }
}

// streaming=sets: the own set first, then the sets in reach or held, nearest first, while the rooms fit.
void SetPicking() {
    static const float walk[][3] = {{0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {40, 0, 0}};
    static const uint8_t walkSet[] = {0, 1, 2, 3};
    static const uint32_t members[][4] = {{1, 0, 0, 0}, {2, 0, 0, 0}, {3, 0, 0, 0}, {4, 0, 0, 0}, {5, 0, 0, 0}};
    static const uint16_t setStart[] = {0, 2, 4, 6, 7};
    static const uint16_t setMember[] = {0, 1, 1, 2, 2, 3, 4};
    Arena a = kArenas[0];
    a.walk = walk;
    a.walkCount = 4;
    a.walkSet = walkSet;
    a.members = members;
    a.memberCount = 5;
    a.setCount = 4;
    a.setStart = setStart;
    a.setMember = setMember;
    float cost[32];
    const float at[3] = {2, 0, 0};
    SetCosts(a, at, cost, 32);
    assert(cost[0] == 4.0f && cost[1] == 64.0f && cost[2] == 324.0f && cost[3] == 1444.0f && cost[4] == 1e30f);
    const float above[3] = {0, 2, 0};
    SetCosts(a, above, cost, 32);
    assert(cost[0] == 36.0f);   // a metre of height counts three
    SetCosts(a, at, cost, 32);
    assert(PickSets(a, cost, 0, 144.0f, 0, 40) == 0x3u);           // 8 m in reach, 18 m not
    assert(PickSets(a, cost, 0, 144.0f, 0x8u, 40) == 0xBu);        // held although far
    assert(PickSets(a, cost, 0, 1e9f, 0, 3) == 0x3u);              // sets 0+1 hold 3 rooms, set 2 would make 4
    assert(PickSets(a, cost, 2, 1.0f, 0, 1) == 0x4u);              // the own set even alone over the limit
    assert(PickSets(a, cost, -1, 25.0f, 0, 40) == 0x1u);           // no own set: the sets in reach
    assert(PickSets(a, cost, 0, 1.0f, 0, 1, 3) == 0x9u);           // a spawn spot's set although far and over the limit
    // the crypt: its sets fit the list one by one, and its hallucination copy is no arena room
    const Arena& crypt = kArenas[ArenaIndex("arena_ch03")];
    bool picked[96]{};
    for (unsigned s = 0; s < crypt.setCount; ++s) assert(SelectMembers(crypt, 1u << s, picked, 96) <= 40);
}

// Behind a locked door: within 6 m, on the side the arena is not on.
void BehindDoors() {
    static const ds2arena::Door doors[] = {{{1, 2, 3, 4}, {0, 0, 0}, true, "test", {1.0f, 0.0f}, -1},
                                           {{5, 6, 7, 8}, {20, 0, 0}, true, "test", {1.0f, 0.0f}, 0}};
    Arena a = kArenas[0];
    a.doors = doors;
    a.doorCount = 2;
    a.doorsOpen = 0;
    const float near[3] = {-2, 0, 0}, behind[3] = {2, 0, 0}, far[3] = {9, 0, 0}, upstairs[3] = {2, 5, 0}, unknown[3] = {22, 0, 0};
    assert(!BehindLockedDoor(a, nullptr, near) && BehindLockedDoor(a, nullptr, behind));
    assert(!BehindLockedDoor(a, nullptr, far) && !BehindLockedDoor(a, nullptr, upstairs) && !BehindLockedDoor(a, nullptr, unknown));
    a.doorsOpen = 1;   // a door that is not locked holds nobody back
    static const ds2arena::Door inner[] = {{{1, 2, 3, 4}, {0, 0, 0}, false, "test", {1.0f, 0.0f}, -1}};
    a.doors = inner;
    a.doorCount = 1;
    assert(!BehindLockedDoor(a, nullptr, behind));
    // the mining hall: behind its locked door 5AE5BB72 is no arena
    const Arena& mine = kArenas[ArenaIndex("arena_ch09")];
    const float pastDoor[3] = {-114.2f, -7.7f, -7.1f}, hall[3] = {-120.0f, -7.9f, -8.0f};
    assert(BehindLockedDoor(mine, PointsFor(mine.name), pastDoor) && !BehindLockedDoor(mine, PointsFor(mine.name), hall));
    // ch10: the original checkpoint's room behind door 5AC7CAE4 is no arena either
    const Arena& gov = kArenas[ArenaIndex("arena_ch10")];
    const float checkpointRoom[3] = {177.0f, -7.0f, 93.5f}, corridor[3] = {182.0f, -7.0f, 93.5f};
    assert(BehindLockedDoor(gov, PointsFor(gov.name), checkpointRoom) && !BehindLockedDoor(gov, PointsFor(gov.name), corridor));
    // no spot of any arena lies behind one of its locked doors
    for (unsigned i = 0; i < kArenaCount; ++i)
        for (unsigned k = 0; k < kArenas[i].spotCount; ++k) assert(!BehindLockedDoor(kArenas[i], PointsFor(kArenas[i].name), kArenas[i].spots[k]));
}

// The spawn spots: who stands on one, and the free one nearest to a place.
void SpawnSpots() {
    static const float spots[][3] = {{0, 0, 0}, {2, 0, 0}, {10, 0, 0}, {0, 6, 0}};
    Arena a = kArenas[0];
    a.spots = spots;
    a.spotCount = 4;
    const float onSecond[3] = {2.5f, 0.1f, 0.5f}, nowhere[3] = {5, 0, 5}, above[3] = {0, 5, 0};
    assert(SpotAt(a, onSecond, 2.0f) == 1 && SpotAt(a, nowhere, 2.0f) == -1 && SpotAt(a, above, 2.0f) == 3);
    bool shared = true;
    const float near0[3] = {0.5f, 0, 0};
    assert(FreeSpotNear(a, near0, nullptr, 0, 1.5f, shared) == 0 && !shared);
    // spot 1 taken: the next nearest on the same floor (spot 4, 6 m up, costs 18 m)
    const float taken[3] = {0.2f, 0, 0.2f};
    assert(FreeSpotNear(a, near0, taken, 1, 1.5f, shared) == 1 && !shared);
    // all taken: the nearest, shared
    const float all[4 * 3] = {0, 0, 0, 2, 0, 0, 10, 0, 0, 0, 6, 0};
    assert(FreeSpotNear(a, near0, all, 4, 1.5f, shared) == 0 && shared);
    a.spotCount = 0;
    assert(FreeSpotNear(a, near0, nullptr, 0, 1.5f, shared) == -1);
    // every arena has spots on floors it walked
    for (unsigned i = 0; i < kArenaCount; ++i) {
        assert(kArenas[i].spotCount >= 1);
        for (unsigned s = 0; s < kArenas[i].spotCount; ++s) assert(SpotAt(kArenas[i], kArenas[i].spots[s], 0.1f) >= 0);
    }
}
}  // namespace

int main() {
    Names();
    Table();
    Area();
    Merging();
    Points();
    DoorMarks();
    ArenaOptions();
    SpawnSpots();
    BehindDoors();
    SetPicking();
    std::printf("coop_arena_tests: all passed (%u arenas)\n", kArenaCount);
    return 0;
}
