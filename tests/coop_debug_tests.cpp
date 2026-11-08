// Dead Space 2 United by Richard Hafer.
// Debug menu: cursor, checkpoint rows, chapter jumps, confirmation, the map.txt table.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "../src/coop_debug.h"

namespace {
using namespace coop_debug;

bool Is(const uint32_t* g, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return g[0] == a && g[1] == b && g[2] == c && g[3] == d; }
int Find(const char* chapter, const char* name) {
    for (unsigned i = 0; i < kCheckpointCount; ++i)
        if (std::strcmp(kChapters[kCheckpoints[i].chapter].key, chapter) == 0 && std::strcmp(kCheckpoints[i].name, name) == 0) return static_cast<int>(i);
    return -1;
}

void Table() {
    assert(kChapterCount >= 10 && kCheckpointCount >= 150);
    unsigned covered = 0;
    for (unsigned c = 0; c < kChapterCount; ++c) {
        assert(kChapters[c].count > 0 && kChapters[c].first == covered);
        for (unsigned i = kChapters[c].first; i < kChapters[c].first + kChapters[c].count; ++i) assert(kCheckpoints[i].chapter == c);
        covered += kChapters[c].count;
    }
    assert(covered == kCheckpointCount);
    // decoded from the guid names, checked against the catalog's Checkpoint objects
    const int tz = Find("ch04_asc", "transition_zone_04"), stalker = Find("ch04_asc", "stalker_room_debug"), lab = Find("ch10_gov", "marker_lab_2_debug");
    assert(tz >= 0 && Is(kCheckpoints[tz].guid, 0x31CB0696u, 0xB5B8F4B5u, 0x43495250u, 0x32303045u) && kCheckpoints[tz].inCatalog);
    assert(stalker >= 0 && Is(kCheckpoints[stalker].guid, 0x31CAD370u, 0x32A3E302u, 0x43524F57u, 0x31313048u));
    assert(lab >= 0 && Is(kCheckpoints[lab].guid, 0x5ACAB4F5u, 0x214AFA81u, 0x52444F52u, 0x45554749u));
    unsigned known = 0;
    for (unsigned i = 0; i < kCheckpointCount; ++i) known += kCheckpoints[i].inCatalog ? 1u : 0u;
    assert(known * 10 >= kCheckpointCount * 9);
    // the chapter's start suit: ch02 and ch03 begin in the Engineering Suit, ch01 has none
    assert(!kChapters[0].hasUniform && kChapters[1].hasUniform && Is(kChapters[1].uniform, 0x30C9B946u, 0xE243D95Fu, 0x54544950u, 0x32303153u));
}

void Labels() {
    char text[64];
    const int stalker = Find("ch04_asc", "stalker_room_debug");
    CheckpointLabel(static_cast<unsigned>(stalker), text, sizeof(text));
    assert(std::strcmp(text, "CH04  stalker room debug") == 0);
    CheckpointLabel(kCheckpointCount, text, sizeof(text));
    assert(std::strcmp(text, "?") == 0);
    char small[8];
    CheckpointLabel(static_cast<unsigned>(stalker), small, sizeof(small));
    assert(std::strlen(small) == 7);
}

void Jumps() {
    const int ch2 = static_cast<int>(kChapters[1].first), ch3 = static_cast<int>(kChapters[2].first);
    assert(ChapterJump(0, 1) == ch2);
    assert(ChapterJump(ch2 + 2, 1) == ch3);
    assert(ChapterJump(ch2 + 2, -1) == ch2);     // to the start of its own chapter first
    assert(ChapterJump(ch2, -1) == 0);           // then to the previous one
    assert(ChapterJump(0, -1) == static_cast<int>(kChapters[kChapterCount - 1].first));
    const int last = static_cast<int>(kCheckpointCount) - 1;
    assert(ChapterJump(last, 1) == 0);           // wraps
}

void Cursoring() {
    Cursor c;
    StepTab(c, -1);
    assert(c.tab == kHost);
    StepTab(c, 1);
    assert(c.tab == kLevels);
    StepRow(c, -1, 5);
    assert(c.row[kLevels] == 4);
    Fit(c, 5, 3);
    assert(c.top[kLevels] == 2);
    StepRow(c, 1, 5);
    assert(c.row[kLevels] == 0);
    Fit(c, 5, 3);
    assert(c.top[kLevels] == 0);
    StepRow(c, 1, 0);
    assert(c.row[kLevels] == 0);
    c.row[kLevels] = 9;
    Fit(c, 4, 10);
    assert(c.row[kLevels] == 3 && c.top[kLevels] == 0);
    assert(CheatIsToggle(kGodMode) && !CheatIsToggle(kFullHealth) && std::strcmp(TabName(kSuits), "SUITS") == 0);
}

void HostTab() {
    assert(std::strcmp(TabName(kHost), "HOST") == 0);
    for (int r = 0; r < kHostRowCount; ++r) assert(std::strcmp(HostRowName(r), "?") != 0);
    assert(HostRowIsDirector(kStartRun) && HostRowIsDirector(kRestartRun) && !HostRowIsDirector(kKillAll));
    assert(HostRowIsEveryone(kHealAll) && HostRowIsEveryone(kGodAll) && !HostRowIsEveryone(kRestartCheckpoint));
    assert(HostRowNeedsConfirm(kRestartCheckpoint) && !HostRowNeedsConfirm(kHealAll));
    for (int r = 0; r < kHostRowCount; ++r) assert((EveryoneCommandFor(r) != 0) == HostRowIsEveryone(r));
    HostValues v;
    StepHostValue(v, kJumpWave, -10, 14);
    assert(v.wave == 1);
    StepHostValue(v, kJumpWave, 5000, 14);
    assert(v.wave == 999);
    StepHostValue(v, kSpawnEnemy, -1, 14);
    assert(v.family == 13);
    StepHostValue(v, kSpawnEnemy, 1, 14);
    assert(v.family == 0);
    StepHostValue(v, kSpawnEnemy, 1, 0);         // no kinds known yet: unchanged
    assert(v.family == 0);
    StepHostValue(v, kMaxAlive, -1, 14);
    assert(v.maxAlive == 0);
    StepHostValue(v, kMaxAlive, 50, 14);
    assert(v.maxAlive == 40);
}

void Confirming() {
    Confirm k;
    assert(!k.Press(0, 3, 1000, 3000));
    assert(k.Armed(0, 3, 2000, 3000) && !k.Armed(0, 4, 2000, 3000));
    assert(k.Press(0, 3, 2500, 3000));          // second press
    assert(!k.Armed(0, 3, 2600, 3000));
    assert(!k.Press(0, 3, 3000, 3000));
    assert(!k.Press(0, 3, 7000, 3000));         // too late: armed again instead
    assert(!k.Press(1, 3, 7100, 3000));         // another tab
    assert(k.Press(1, 3, 7200, 3000));
}
}  // namespace

int main() {
    Table();
    Labels();
    Jumps();
    Cursoring();
    HostTab();
    Confirming();
    std::printf("coop_debug_tests: all passed (%u chapters, %u checkpoints)\n", kChapterCount, kCheckpointCount);
    return 0;
}
