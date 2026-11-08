// Dead Space 2 United by Richard Hafer.
#include "../src/coop_dementia.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace coop_dementia;

namespace {
// Seconds until the meter reaches `target` with the given situation, stepped like the game (0.2 s).
float SecondsTo(float target, bool alone, bool anyCompany, float health, bool shop, float start, const Rates& r) {
    float m = start, t = 0.0f;
    while ((alone ? m < target : m > target) && t < 10000.0f) { m = Step(m, alone, anyCompany, health, shop, 0.2f, r); t += 0.2f; }
    return t;
}
}  // namespace

int main() {
    // stages rise at their thresholds and only fall well below them
    assert(StageFor(0.0f, 0) == 0);
    assert(StageFor(24.9f, 0) == 0 && StageFor(25.0f, 0) == 1);
    assert(StageFor(60.0f, 0) == 2 && StageFor(95.0f, 0) == 4);
    assert(StageFor(20.0f, 1) == 1);                  // inside the drop band: stays
    assert(StageFor(16.9f, 1) == 0);                  // 25 - 8
    assert(StageFor(45.0f, 2) == 2 && StageFor(41.9f, 2) == 1);
    assert(StageFor(10.0f, 4) == 0);                  // falls through every stage at once
    assert(StageFor(50.0f, 9) == 2 && StageFor(5.0f, -3) == 0);   // out-of-range stages are clamped first

    Rates r;
    // alone with company somewhere: 0 -> 100 in about 150 s; together: 100 -> 0 in about 40 s
    const float up = SecondsTo(100.0f, true, true, 1.0f, false, 0.0f, r);
    assert(std::fabs(up - 150.0f) < 1.0f);
    const float down = SecondsTo(0.0f, false, true, 1.0f, false, 100.0f, r);
    assert(std::fabs(down - 40.0f) < 1.0f);
    // nobody else in the game: half as fast; low health: one and a half times as fast
    assert(std::fabs(SecondsTo(100.0f, true, false, 1.0f, false, 0.0f, r) - 300.0f) < 1.0f);
    assert(std::fabs(SecondsTo(100.0f, true, true, 0.2f, false, 0.0f, r) - 100.0f) < 1.0f);
    // unknown health (-1) counts as healthy; the store counts as company
    assert(Step(50.0f, true, true, -1.0f, false, 1.0f, r) > 50.0f);
    assert(Step(50.0f, true, true, 1.0f, true, 1.0f, r) < 50.0f);
    // the meter stays in 0..100
    assert(Step(99.9f, true, true, 0.1f, false, 5.0f, r) == 100.0f);
    assert(Step(0.5f, false, true, 1.0f, false, 5.0f, r) == 0.0f);
    // a med pack is a jump above 15 % of the maximum, nothing before the first reading
    assert(Healed(40.0f, 70.0f, 100.0f));
    assert(!Healed(40.0f, 50.0f, 100.0f));
    assert(!Healed(-1.0f, 100.0f, 100.0f));
    assert(!Healed(40.0f, 90.0f, 0.0f));
    std::printf("Dementia meter passed: stages with an 8-point drop band, 150 s up, 40 s down, solo half, low health 1.5x, store as company.\n");
    return 0;
}
