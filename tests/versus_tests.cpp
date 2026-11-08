// Dead Space 2 United by Richard Hafer.
#include "../src/coop_versus.h"

#include <cassert>
#include <cstdio>

using namespace coop_versus;

static void Packets() {
    Packet p;
    p.kind = kScore;
    p.seq = 7;
    p.ref = 0x4453324D00000002ull;
    Packet q;
    assert(Parse(&p, sizeof(p), q) && q.kind == kScore && q.seq == 7 && q.ref == p.ref);
    assert(!Parse(&p, sizeof(p) - 1, q));
    Packet bad = p;
    bad.magic = 0;
    assert(!Parse(&bad, sizeof(bad), q));
    assert(FromHostOnly(kScore) && FromHostOnly(kSpawnPoint) && !FromHostOnly(kScoreAsk) && !FromHostOnly(kRespawnAsk));
    std::printf("packets ok\n");
}

static void Window() {
    SeqWindow w;
    assert(!w.Fresh(0));
    assert(w.Fresh(5) && !w.Fresh(5));
    assert(w.Fresh(3) && !w.Fresh(3));   // late but new
    assert(w.Fresh(6) && w.Fresh(100) && !w.Fresh(100));
    assert(w.Fresh(99) && !w.Fresh(99));
    assert(!w.Fresh(30));                // too old to tell
    assert(w.Fresh(200) && w.Fresh(137) && !w.Fresh(137));
    std::printf("window ok\n");
}

static void Points_() {
    assert(FindPoints(0x8ECD3FEB) && FindPoints(0x8ECD3FEB)->value == 100);
    assert(FindPoints(0x5BE3A67B)->value == -20 && FindPoints(0xE48BA8AF)->value == 300 && !FindPoints(0x0438AC13));   // retail (wiki)
    assert(IsStreak(19) && IsStreak(21) && !IsStreak(18) && IsObjective(11) && IsObjective(12) && !IsObjective(13));
    std::printf("points ok\n");
}

static void MatchFlow() {
    Match m;
    assert(m.End(1, 0, 1000) && !m.End(1, 0, 1100));
    assert(m.Due(5000, 12000, 20000) == 0 && m.Due(13000, 12000, 20000) == 1);
    m.Swap();
    assert(m.round == 2 && m.stage == Stage::Playing && m.Due(99999, 12000, 20000) == 0);
    assert(!m.End(1, 1, 50000) && m.End(2, 1, 50000) && m.winner[1] == 0 && m.winner[2] == 1);
    assert(m.Due(60000, 12000, 20000) == 0 && m.Due(82000, 12000, 20000) == 2);
    m.Over();
    assert(m.Due(999999, 12000, 20000) == 0 && m.stage == Stage::MatchOver);
    std::printf("match ok\n");
}

int main() {
    Packets();
    Window();
    Points_();
    MatchFlow();
    std::printf("versus tests passed\n");
    return 0;
}
