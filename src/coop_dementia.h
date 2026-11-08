// Dead Space 2 United by Hafer.
// The Dementia mutator's sanity meter (runtime_coop_dementia.inc), without the game.
#pragma once
#include <algorithm>

namespace coop_dementia {
constexpr float kStageAt[5] = {0.0f, 25.0f, 50.0f, 75.0f, 90.0f};
constexpr float kStageDrop = 8.0f;   // a stage ends this far below its threshold

// The stage for `meter`, starting from `current`: up at a threshold, down only well below it.
inline int StageFor(float meter, int current) {
    int s = current < 0 ? 0 : current > 4 ? 4 : current;
    while (s < 4 && meter >= kStageAt[s + 1]) ++s;
    while (s > 0 && meter < kStageAt[s] - kStageDrop) --s;
    return s;
}

struct Rates {
    float risePerS = 100.0f / 150.0f, fallPerS = 100.0f / 40.0f;
    float soloFactor = 0.5f;          // nobody else in the game at all
    float lowHealthFactor = 1.5f, lowHealthBelow = 0.35f;
};
// One step of `dt` seconds. Alone means no company within reach; the store counts as company.
inline float Step(float meter, bool alone, bool anyCompany, float healthFraction, bool shopOpen, float dt, const Rates& r) {
    if (alone && !shopOpen) {
        float rate = r.risePerS * (anyCompany ? 1.0f : r.soloFactor);
        if (healthFraction >= 0.0f && healthFraction < r.lowHealthBelow) rate *= r.lowHealthFactor;
        return std::min(100.0f, meter + rate * dt);
    }
    return std::max(0.0f, meter - r.fallPerS * dt);
}
// A med pack: health jumped by more than 15 % of its maximum.
inline bool Healed(float before, float after, float max) { return before >= 0.0f && max > 0.0f && after - before > max * 0.15f; }
}  // namespace coop_dementia
