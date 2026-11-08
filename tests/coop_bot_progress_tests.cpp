// Dead Space 2 United by Richard Hafer.
// The bots' loadout progression outside the campaign, checked without a game.
#include <cstdio>
#include "../src/coop_bot_progress.h"

namespace {
using namespace coop_bot_progress;
int failures = 0;
void Check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
// The supply terminal's default prices, in bot::kGunCatalogue order.
const uint32_t kPrices[kGuns] = {1000, 2500, 2500, 3000, 2000, 2500, 2500, 3000, 4000, 4000};
constexpr uint32_t kNoCutter = kAllGuns & ~(1u << kCutter);

void Lists() {
    float v[4] = {3, 5, 7, 9};
    Check(ParseList("4,6, 8 ,10", v, 4) == 4 && v[0] == 4 && v[1] == 6 && v[2] == 8 && v[3] == 10, "four numbers");
    float w[4] = {3, 5, 7, 9};
    ParseList("2.5,,x,0", w, 4);
    Check(w[0] == 2.5f && w[1] == 5 && w[2] == 7 && w[3] == 0, "empty and bad entries keep their value, 0 stays 0");
    float s[4] = {1, 2, 3, 4};
    Check(ParseList("", s, 4) == 0 && s[0] == 1, "an empty list changes nothing");
}

void Waves() {
    Check(CompletedWaves(1, 0) == 0, "arming: nothing done");
    Check(CompletedWaves(2, 1) == 0, "first wave running: nothing done");
    Check(CompletedWaves(3, 3) == 3, "the break after wave 3 counts 3");
    Check(CompletedWaves(2, 4) == 3, "wave 4 running counts 3");
    Check(CompletedWaves(0, 7) == 0, "off counts nothing");
}

void TiersFromPrices() {
    const Tiers t = SplitTiers(kPrices, kNoCutter);
    Check(t.median == 2500, "median of the terminal prices");
    Check(t.cheap == ((1u << 1) | (1u << 2) | (1u << 4) | (1u << 5) | (1u << 6)), "cheaper: pulse, line, ripper, detonator, flamethrower");
    Check(t.expensive == ((1u << 3) | (1u << 7) | (1u << 8) | (1u << 9)), "expensive: javelin, seeker, force, contact");
    Check(!(t.cheap & t.expensive), "no gun in both tiers");
    const uint32_t same[kGuns] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
    const Tiers u = SplitTiers(same, kNoCutter);
    Check(u.cheap == kNoCutter && u.expensive == kNoCutter, "one price: both tiers hold every gun");
}

void PlansStayInTheirTiers() {
    const Tiers t = SplitTiers(kPrices, kNoCutter);
    const int fav[2] = {7, 0};
    for (uint32_t seed = 1; seed < 400; ++seed) {
        Rng rng(Mix(seed));
        const uint32_t taken[3] = {0, 0, 0};
        const GunPlan p = PlanProgression(rng, t, fav, 2, taken);
        Check(p.gun[0] >= 0 && ((t.cheap >> p.gun[0]) & 1u), "first gun is a cheaper one");
        Check(p.gun[1] >= 0 && ((t.expensive >> p.gun[1]) & 1u), "second gun is an expensive one");
        Check(p.gun[2] >= 0 && p.gun[2] != p.gun[0] && p.gun[2] != p.gun[1], "fourth gun is another one");
        Check(p.gun[0] != kCutter && p.gun[1] != kCutter && p.gun[2] != kCutter, "never the plasma cutter, it is the base");
        if (failures) return;
    }
}

void SameSeedSamePlan() {
    const Tiers t = SplitTiers(kPrices, kNoCutter);
    const uint32_t taken[3] = {0, 0, 0};
    Rng a(1234), b(1234);
    const GunPlan p = PlanProgression(a, t, nullptr, 0, taken);
    const GunPlan q = PlanProgression(b, t, nullptr, 0, taken);
    Check(p.gun[0] == q.gun[0] && p.gun[1] == q.gun[1] && p.gun[2] == q.gun[2], "a seed always gives the same plan");
}

void FavouritesWeighMore() {
    const int fav[1] = {8};   // force gun
    int hits = 0, plain = 0;
    for (uint32_t seed = 1; seed <= 2000; ++seed) {
        Rng r1(Mix(seed)), r2(Mix(seed));
        if (Pick(r1, kNoCutter, fav, 1, 0) == 8) ++hits;
        if (Pick(r2, kNoCutter, nullptr, 0, 0) == 8) ++plain;
    }
    Check(hits > plain * 2, "a favourite comes up far more often");
    int teamHits = 0;
    for (uint32_t seed = 1; seed <= 2000; ++seed) {
        Rng r(Mix(seed));
        if (Pick(r, kNoCutter, fav, 1, 1u << 8) == 8) ++teamHits;
    }
    Check(teamHits < hits, "a gun a teammate took comes up less often");
}

void LoadoutIsPulsePlusOne() {
    for (uint32_t seed = 1; seed < 300; ++seed) {
        Rng rng(Mix(seed));
        const GunPlan p = PlanLoadout(rng, nullptr, 0, 0);
        Check(p.gun[0] == kPulseRifle, "the pulse rifle always");
        Check(p.gun[1] >= 0 && p.gun[1] < kGuns && p.gun[1] != kPulseRifle, "one more, never a second pulse rifle");
        if (failures) return;
    }
    const int fav[2] = {kPulseRifle, kCutter};
    int cutters = 0;
    for (uint32_t seed = 1; seed <= 1000; ++seed) {
        Rng rng(Mix(seed));
        if (PlanLoadout(rng, fav, 2, 0).gun[1] == kCutter) ++cutters;
    }
    Check(cutters > 150, "a favourite plasma cutter is a likely extra gun");
}

void Stagger() {
    for (int rotation = 0; rotation < 5; ++rotation) {
        const int a = StaggerSlot(0, rotation, 2), b = StaggerSlot(1, rotation, 2), c = StaggerSlot(2, rotation, 2);
        Check(a != b && b != c && a != c, "three bots, stagger 2: three different slots");
        Check(a >= 0 && a <= 2 && b >= 0 && b <= 2 && c >= 0 && c <= 2, "slots stay within the stagger");
    }
    Check(StaggerSlot(2, 1, 0) == 0, "stagger 0: everybody at once");
    const float base[kSteps] = {3, 5, 7, 9};
    const int slots[kSteps] = {2, 0, 1, 2};
    float due[kSteps];
    DueTimes(base, slots, 1.0f, due);
    Check(due[0] == 5 && due[1] == 5 && due[2] == 8 && due[3] == 11, "a step never comes before the one before it");
    const float never[kSteps] = {3, 0, 7, 9};
    const int none[kSteps] = {0, 0, 0, 0};
    DueTimes(never, none, 1.0f, due);
    Check(due[1] == 0 && due[2] == 7, "0 = that step never comes");
    const float minutes[kSteps] = {120, 240, 360, 480};
    const int late[kSteps] = {1, 1, 1, 1};
    DueTimes(minutes, late, 45.0f, due);
    Check(due[0] == 165 && due[3] == 525, "clock modes stagger in seconds");
}

void Suits() {
    const int fav[3] = {1, 16, 13};
    int out[kMaxSuits];
    Rng rng(99);
    const int n = SuitOrder(rng, 23, fav, 3, 14, 0, out, kMaxSuits);
    Check(n == 22, "every suit but the patient suit");
    Check(out[0] == 1 && out[1] == 16 && out[2] == 13, "favourites first, best first");
    bool patient = false;
    for (int i = 0; i < n; ++i) patient = patient || out[i] == 14;
    Check(!patient, "never the patient suit");
    Rng rng2(99);
    const int m = SuitOrder(rng2, 23, fav, 3, 14, 1u << 1, out, kMaxSuits);
    Check(m == 22 && out[0] == 16 && out[1] == 13, "a favourite a teammate wears goes behind");
    bool taken = false;
    for (int i = 0; i < 21; ++i) taken = taken || out[i] == 1;
    Check(!taken, "the taken favourite comes after every free suit");
}
}  // namespace

int main() {
    Lists();
    Waves();
    TiersFromPrices();
    PlansStayInTheirTiers();
    SameSeedSamePlan();
    FavouritesWeighMore();
    LoadoutIsPulsePlusOne();
    Stagger();
    Suits();
    if (failures) {
        std::printf("coop_bot_progress_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("coop_bot_progress_tests: all passed\n");
    return 0;
}
