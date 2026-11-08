// Dead Space 2 United by Richard Hafer.
// The multiplayer-map visit without a game: double press, map names, GUID text,
// the generated entity table, the member filter, the watchdog, the visit phases
// and the spawn point choice.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../src/coop_mpmap.h"
#include "../src/generated_mpmap_block.h"

namespace {
using namespace coop_mpmap;

void Maps() {
    assert(MapIndex("mp_bombRun") == 0 && MapIndex("MP_BOMBRUN") == 0 && MapIndex("mp_solar") == 4);
    assert(MapIndex("mp_bombRun2") == -1 && MapIndex("ds_2") == -1 && MapIndex(nullptr) == -1);
    assert(IsMpMap("mp_gov") && IsMpMap("MP_x") && !IsMpMap("mp_") && !IsMpMap("ds_2") && !IsMpMap("frontend") && !IsMpMap(nullptr));
    assert(IsMpMap("pdlc_mp_mall") && IsMpMap("PDLC_MP_School") && !IsMpMap("pdlc_mp_") && !IsMpMap("pdlc1_ch01_mng"));
    assert(MapIndex("pdlc_mp_mall") == 5 && MapIndex("PDLC_MP_SCHOOL") == 6 && kMapTable[5].kind == MapKind::Outbreak);
    for (unsigned i = 0; i < kMapCount; ++i) {
        assert(MapIndex(kMapTable[i].name) == static_cast<int>(i) && IsMpMap(kMapTable[i].name) && kMapTable[i].label[0]);
        assert((kMapTable[i].kind == MapKind::Shipped) == (i < 5));
    }
    assert(std::strcmp(MapName(5), "pdlc_mp_mall") == 0 && std::strcmp(MapName(-1), "?") == 0 && std::strcmp(MapName(kMapCount), "?") == 0);
    assert(std::strcmp(MapLabel(0), "titan mines") == 0 && std::strcmp(MapLabel(99), "?") == 0);
    // the generated block table uses the same bit order
    static_assert(sizeof(kBlockMaps) / sizeof(kBlockMaps[0]) == kMapCount, "block table map count");
    for (unsigned i = 0; i < kMapCount; ++i) assert(std::strcmp(kBlockMaps[i], kMapTable[i].name) == 0);
    std::printf("maps ok\n");
}

void Archives() {
    // hashes as ds2big lists them for DS2DAT12.DAT and the stock MP archive
    assert(MapStreamHash("pdlc_mp_mall") == 0xA52CF19Fu && MapStreamHash("pdlc_mp_school") == 0x106A019Fu);
    assert(ArchiveHash("pdlc_mp_mall\\pdlc_mp_mall.str") == 0xA52CF19Fu && ArchiveHash("PDLC_MP_MALL/pdlc_mp_mall.STR") == 0xA52CF19Fu);
    assert(MapStreamHash("mp_detonate") == 0x64166F5Fu && MapStreamHash("mp_gov") == 0x6469001Fu);
    assert(ArchiveHash("map.txt") == 0xE4529CFEu && ArchiveHash("") == 0 && ArchiveHash(nullptr) == 0);
    // a three entry archive: header, then {offset, size, hash} big endian, sorted by hash
    unsigned char a[16 + 3 * 12]{};
    const auto put = [&](unsigned at, uint32_t v) {
        a[at] = static_cast<unsigned char>(v >> 24);
        a[at + 1] = static_cast<unsigned char>(v >> 16);
        a[at + 2] = static_cast<unsigned char>(v >> 8);
        a[at + 3] = static_cast<unsigned char>(v);
    };
    a[0] = 'B'; a[1] = 'I'; a[2] = 'G'; a[3] = 'H';
    put(8, 3);
    put(12, 16 + 3 * 12 + 8);
    put(16 + 8, 0x106A019Fu);
    put(28 + 8, 0x64166F5Fu);
    put(40 + 8, 0xA52CF19Fu);
    assert(BigCount(a) == 3);
    assert(BigHas(a + 16, 3, 0x106A019Fu) && BigHas(a + 16, 3, 0x64166F5Fu) && BigHas(a + 16, 3, 0xA52CF19Fu));
    assert(!BigHas(a + 16, 3, 0x6469001Fu) && !BigHas(a + 16, 3, 0) && !BigHas(a + 16, 0, 0x106A019Fu));
    put(12, 16 + 3 * 12);   // no trailer, like Mark's DS2DAT12 (count 0xF0, data at 0xB50)
    assert(BigCount(a) == 3);
    put(12, 16 + 3 * 12 - 1);
    assert(BigCount(a) == -1);
    put(12, 16 + 3 * 12 + 8);
    a[3] = 'X';
    assert(BigCount(a) == -1);
    std::printf("archives ok\n");
}

void Guids() {
    uint32_t g[4]{};
    assert(ParseGuid("4BC78C36-9D70FEE7-544E4543-53574F4B", g));
    assert(g[0] == 0x4BC78C36u && g[1] == 0x9D70FEE7u && g[2] == 0x544E4543u && g[3] == 0x53574F4Bu);
    assert(ParseGuid("4bc78c36-9d70fee7-544e4543-53574f4b \r\n", g) && g[3] == 0x53574F4Bu);
    assert(!ParseGuid("4BC78C36-9D70FEE7-544E4543", g));
    assert(!ParseGuid("4BC78C36-9D70FEE7-544E4543-53574F4BX", g));
    assert(!ParseGuid("4BC78C36_9D70FEE7-544E4543-53574F4B", g));
    assert(!ParseGuid("", g) && !ParseGuid(nullptr, g));
    const uint32_t zero[4]{}, bad[4] = {0xBADBADBAu, 0xBEEFBEEFu, 0xEAC15A55u, 0x91100911u};
    assert(NullGuid(zero) && !NullGuid(g) && InvalidGuid(bad) && !InvalidGuid(g));
    // h = ((g0 * 33 + g1) * 33 + g2) * 33 + g3, as 0x00758CF9 computes it with shifts
    const uint32_t s[4] = {1, 2, 3, 4};
    assert(StreamHash(s) == ((1u * 33 + 2) * 33 + 3) * 33 + 4);
    uint32_t a = g[0], b = 0;
    b = (a << 5) + g[1] + a; a = b;
    b = (a << 5) + g[2] + a; a = b;
    b = (a << 5) + g[3] + a;
    assert(StreamHash(g) == b);
    std::printf("guids ok\n");
}

void Table() {
    const size_t n = sizeof(coop_mpmap::kMpMapEntities) / sizeof(coop_mpmap::kMpMapEntities[0]);
    assert(n > 100);
    assert(Sorted(coop_mpmap::kMpMapEntities, n));
    unsigned perGroup[kGroups]{}, sequencerMaps = 0;
    for (size_t i = 0; i < n; ++i) {
        const MapEntity& e = coop_mpmap::kMpMapEntities[i];
        assert(e.group < kGroups && e.maps && e.maps < (1u << kMapCount) && e.cls && e.cls[0]);
        ++perGroup[e.group];
        if (std::strcmp(e.cls, "MPEndOfGameSequencer") == 0) sequencerMaps |= e.maps;
        assert(Find(coop_mpmap::kMpMapEntities, n, e.words) == &e);
    }
    assert(sequencerMaps == (1u << kMapCount) - 1);   // one end-game sequencer on every map
    for (unsigned g = 0; g < kGroups; ++g) assert(perGroup[g] > 0);
    const uint32_t absent[4] = {0x4BC78C36u, 0x9D70FEE7u, 0x544E4543u, 0x53574F4Bu};
    assert(!Find(coop_mpmap::kMpMapEntities, n, absent));
    // blocking follows the group switches and the map
    const MapEntity* timer = nullptr;
    for (size_t i = 0; i < n && !timer; ++i) if (coop_mpmap::kMpMapEntities[i].group == kRoundTimer) timer = &coop_mpmap::kMpMapEntities[i];
    assert(timer);
    int on = -1, off = -1;
    for (int m = 0; m < static_cast<int>(kMapCount); ++m) { if (timer->maps >> m & 1) on = m; else off = m; }
    const uint32_t all = 0x3E;
    assert(Blocks(timer, on, all) && !Blocks(timer, off, all) && Blocks(timer, -1, all));
    assert(!Blocks(timer, on, all & ~(1u << kRoundTimer)));
    const MapEntity* watch = nullptr;
    for (size_t i = 0; i < n && !watch; ++i) if (coop_mpmap::kMpMapEntities[i].group == kWatch) watch = &coop_mpmap::kMpMapEntities[i];
    assert(watch && !Blocks(watch, -1, 0xFF) && !Blocks(nullptr, 0, all));
    assert(std::strcmp(GroupName(kMpSlashers), "mp_slashers") == 0 && std::strcmp(GroupName(99), "?") == 0);
    std::printf("table ok (%zu entities)\n", n);
}

void Members() {
    const uint32_t in[5][4] = {{1, 1, 1, 1}, {2, 2, 2, 2}, {0, 0, 0, 0}, {0xBADBADBAu, 0xBEEFBEEFu, 0xEAC15A55u, 0x91100911u}, {5, 5, 5, 5}};
    const unsigned char hidden[5] = {0, 1, 0, 1, 1};
    const bool known[5] = {true, false, false, false, true};
    uint32_t out[5][4]{};
    unsigned char outHidden[5]{};
    unsigned dropped = 99;
    assert(FilterMembers(in, hidden, 5, known, false, out, outHidden, &dropped) == 5 && dropped == 0);
    assert(FilterMembers(in, hidden, 5, known, true, out, outHidden, &dropped) == 4 && dropped == 1);
    assert(out[0][0] == 1 && out[1][0] == 0 && out[2][0] == 0xBADBADBAu && out[3][0] == 5);   // null and invalid stay
    assert(outHidden[0] == 0 && outHidden[2] == 1 && outHidden[3] == 1);
    assert(FilterMembers(in, nullptr, 2, known, true, out, outHidden, nullptr) == 1 && outHidden[0] == 0);
    std::printf("members ok\n");
}

void DoublePressKey() {
    DoublePress k;
    assert(k.Update(false, 0) == DoublePress::kNone);
    assert(k.Update(true, 100) == DoublePress::kArmed);
    assert(k.Update(true, 200) == DoublePress::kNone);       // held: one press
    assert(k.Update(false, 300) == DoublePress::kNone);
    assert(k.Update(true, 2900) == DoublePress::kConfirmed);
    assert(!k.armed);
    assert(k.Update(false, 3000) == DoublePress::kNone);
    assert(k.Update(true, 4000) == DoublePress::kArmed);
    assert(k.Update(false, 4100) == DoublePress::kNone);
    assert(k.Update(false, 7001) == DoublePress::kExpired);   // 3001 ms later
    assert(k.Update(false, 7100) == DoublePress::kNone);
    assert(k.Update(true, 7200) == DoublePress::kArmed);       // a new first press
    assert(k.Update(false, 7300) == DoublePress::kNone);
    assert(k.Update(true, 10300) == DoublePress::kArmed);      // too late: expired and armed again
    assert(k.Update(false, 10400) == DoublePress::kNone && k.Update(true, 10500) == DoublePress::kConfirmed);
    std::printf("double press ok\n");
}

void Dog() {
    Watchdog w;
    assert(!w.Stuck(1000, 45000));
    w.Start(1, 0);
    assert(!w.Observe(1, 1000) && w.QuietMs(1000) == 1000);
    assert(w.Observe(2, 5000) && w.QuietMs(6000) == 1000);
    assert(!w.Stuck(49999, 45000));
    assert(w.Stuck(50000, 45000));
    assert(!w.Stuck(60000, 45000));        // once per limit
    assert(w.Stuck(95000, 45000));
    assert(w.Stuck(140000, 45000) && w.Stuck(185000, 45000));
    assert(!w.Stuck(500000, 45000));       // four reports per load at most
    assert(w.Observe(3, 500001) && !w.Stuck(500002, 45000) && w.Stuck(545001, 45000));
    assert(!w.Stuck(600000, 0));
    w.Stop();
    assert(!w.Observe(4, 700000) && !w.Stuck(900000, 45000) && w.QuietMs(900000) == 0);
    std::printf("watchdog ok\n");
}

void Phases() {
    Visit v;
    assert(!v.Holding());
    assert(!v.Observe(kRunning, true, 0));                           // idle ignores everything
    v.Request(100);
    assert(v.phase == Phase::Leaving && v.Holding());
    assert(!v.Observe(kRunning, true, 150));                         // the old level still runs under the new name
    assert(v.Observe(5, true, 200) && v.phase == Phase::Loading);
    assert(!v.Observe(11, true, 300));
    assert(v.Observe(kRunning, true, 400) && v.phase == Phase::Arrived && v.epochs == 1);
    assert(v.Observe(1, true, 500) && v.phase == Phase::Loading);    // death restart of the MP map
    assert(v.Observe(kRunning, true, 600) && v.phase == Phase::Arrived && v.epochs == 2);
    v.RequestReturn(700);
    assert(v.phase == Phase::Returning && v.Holding());
    assert(!v.Observe(kRunning, false, 750));                        // the MP level still runs under the campaign name
    assert(!v.Observe(6, false, 800));
    assert(v.Observe(kRunning, false, 900) && v.phase == Phase::Idle && !v.Holding());
    // a load that ends on a non-MP map (main menu, a loaded save) ends the visit
    v.Request(1000);
    v.Observe(2, true, 1100);
    assert(v.Observe(kRunning, false, 1200) && v.phase == Phase::Idle);
    // a return that ends on an MP map again keeps holding
    v.Request(2000); v.Observe(3, true, 2100); v.Observe(kRunning, true, 2200);
    v.RequestReturn(2300);
    v.Observe(4, true, 2400);
    assert(v.Observe(kRunning, true, 2500) && v.phase == Phase::Arrived && v.Holding());
    v.Abort(2600);
    assert(v.phase == Phase::Idle && std::strcmp(PhaseName(v.phase), "idle") == 0);
    std::printf("phases ok\n");
}

void Points() {
    const float pos[5][3] = {{0, 0, 0}, {1, 2, 3}, {NAN, 0, 0}, {4, 5, 6}, {7, 8, 9}};
    assert(ChoosePoint(pos, 5, -1, 0) == 1);
    assert(ChoosePoint(pos, 5, 3, 0) == 3);
    assert(ChoosePoint(pos, 5, 0, 0) == 1);          // the preferred one is unusable
    assert(ChoosePoint(pos, 5, 2, 0) == 1);
    assert(ChoosePoint(pos, 5, -1, 1u << 1) == 3);   // the bots take the others
    assert(ChoosePoint(pos, 5, -1, (1u << 1) | (1u << 3) | (1u << 4)) == 1);
    assert(ChoosePoint(pos, 1, -1, 0) == -1);
    assert(ChoosePoint(pos, 0, 0, 0) == -1);
    const float far[1][3] = {{1e6f, 0, 0}};
    assert(ChoosePoint(far, 1, -1, 0) == -1);
    std::printf("points ok\n");
}

void States() {
    assert(std::strcmp(StateName(11), "LOAD_CHECKPOINT") == 0 && std::strcmp(StateName(16), "RUNNING") == 0);
    assert(std::strcmp(StateName(0), "NO_LEVEL") == 0 && std::strcmp(StateName(17), "?") == 0);
    std::printf("states ok\n");
}
}  // namespace

int main() {
    Maps();
    Archives();
    Guids();
    Table();
    Members();
    DoublePressKey();
    Dog();
    Phases();
    Points();
    States();
    std::printf("all mpmap tests passed\n");
    return 0;
}
