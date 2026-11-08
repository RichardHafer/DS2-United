// Dead Space 2 United by Richard Hafer.
#include "../src/coop_mpmap.h"
#include "../src/coop_zoneobj.h"

#include <cassert>
#include <cstdio>

using namespace coop_zoneobj;

namespace {
uint32_t NameHash(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) h = h * 0x1003Fu + static_cast<unsigned char>(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
    return h;
}
void CheckMessages(const Message* list, unsigned count) {
    for (unsigned i = 0; i < count; ++i) assert(list[i].hash == NameHash(list[i].name));
}
}  // namespace

int main() {
    // one plan per map, in the map table's order
    assert(kPlanCount == coop_mpmap::kMapCount);
    for (unsigned i = 0; i < kPlanCount; ++i) {
        const MapPlan& p = kPlans[i];
        assert(SameText(p.map, coop_mpmap::kMapTable[i].name));
        assert(PlanFor(static_cast<int>(i)) == &p);
        assert(p.stepCount >= 3 && p.blockCount >= 3 && p.localCount > 0);
        assert(Sorted(p.local, p.localCount));
        CheckMessages(p.open, p.openCount);
        CheckMessages(p.block, p.blockCount);
        bool roundEnd = false;
        for (unsigned b = 0; b < p.blockCount; ++b)
            roundEnd |= p.block[b].hash == NameHash("online_end_round_human_win") || p.block[b].hash == NameHash("round_end_humanwin");
        assert(roundEnd);   // the humans' win is dropped on every map
        for (unsigned s = 0; s < p.stepCount; ++s) {
            CheckMessages(p.steps[s].messages, p.steps[s].count);
            for (unsigned m = 0; m < p.steps[s].count; ++m) {
                assert(IndexOf(p.block, p.blockCount, p.steps[s].messages[m].hash) < 0);   // nothing sent is dropped
                assert(InSorted(p.local, p.localCount, p.steps[s].messages[m].hash));      // and everything sent stays local
            }
        }
        for (unsigned o = 0; o < p.openCount; ++o) {
            assert(IndexOf(p.block, p.blockCount, p.open[o].hash) < 0);
            assert(InSorted(p.local, p.localCount, p.open[o].hash));
        }
        assert(!PlanFor(-1) && !PlanFor(static_cast<int>(kPlanCount)));
    }
    assert(!InSorted(kPlans[0].local, kPlans[0].localCount, 0u));

    // the batches: the steps, then the final objective's doors when there are any
    const MapPlan& titan = kPlans[0];
    assert(Batches(titan) == titan.stepCount + (titan.openCount ? 1u : 0u));
    Runner r;
    assert(r.Due(titan, 0, 2500) == -1);   // nothing before Start
    r.Start(1000, 1500);
    assert(r.Due(titan, 2499, 2500) == -1);
    int expected = 0;
    uint32_t now = 2500;
    for (;;) {
        const int b = r.Due(titan, now, 2500);
        assert(b == expected);
        unsigned count = 0;
        const char* label = nullptr;
        assert(BatchMessages(titan, static_cast<unsigned>(b), count, label) && count && label && label[0]);
        ++expected;
        if (r.done) break;
        assert(r.Due(titan, now + 2499, 2500) == -1);   // the gap holds
        now += 2500;
    }
    assert(static_cast<unsigned>(expected) == Batches(titan));
    assert(r.Due(titan, now + 100000, 2500) == -1);   // each batch once
    unsigned count = 1;
    const char* label = nullptr;
    assert(!BatchMessages(titan, Batches(titan), count, label) && count == 0);
    // a batch whose relays take longer than the gap holds the next one back
    for (unsigned i = 0; i < kPlanCount; ++i) {
        const MapPlan& p = kPlans[i];
        Runner g;
        g.Start(0, 0);
        uint32_t at = 0;
        for (unsigned b = 0; b < Batches(p); ++b) {
            assert(g.Due(p, at, 2500) == static_cast<int>(b));
            const uint32_t wait = BatchTailMs(p, b) + kAfterTailMs > 2500 ? BatchTailMs(p, b) + kAfterTailMs : 2500;
            if (!g.done) assert(g.Due(p, at + wait - 1, 2500) == -1);
            at += wait;
        }
        assert(g.done);
    }
    // the tick may wrap
    Runner w;
    w.Start(0xFFFFF000u, 1500);
    assert(w.Due(titan, 0xFFFFF5DBu, 2500) == -1 && w.Due(titan, 0xFFFFF5DCu, 2500) == 0);
    std::printf("zone objectives tests passed (%u plans)\n", kPlanCount);
    return 0;
}
