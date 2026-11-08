// Dead Space 2 United by Hafer.
#pragma once
// What a companion bot gets outside the campaign and when (runtime_coop_bot.inc,
// "progression"). Mercenaries and Versus: the Pulse Rifle, one more gun and a suit
// at once, like the character setup. Survival waves, the arenas, Objective and
// Hold the Zone: a cheaper gun, a suit, an expensive gun and a fourth gun after so
// many waves or round minutes, each bot on its own wave where it can be.
// Gun indices follow bot::kGunCatalogue, suit indices bot::kSuits.
#include <cstdint>
#include <cstdlib>

namespace coop_bot_progress {
constexpr int kGuns = 10;
constexpr int kCutter = 0, kPulseRifle = 1;
constexpr int kSteps = 4;   // progression: cheaper gun, suit, expensive gun, fourth gun; loadout: pulse rifle, suit, extra gun
constexpr int kSuitStep = 1;
constexpr int kMaxSuits = 32;
constexpr uint32_t kAllGuns = (1u << kGuns) - 1u;

enum class Kind : uint8_t { Off, Story, Loadout, Waves, Clock };
inline const char* KindName(Kind k) {
    switch (k) {
        case Kind::Story: return "story";
        case Kind::Loadout: return "loadout";
        case Kind::Waves: return "waves";
        case Kind::Clock: return "clock";
        default: return "off";
    }
}
inline bool Active(Kind k) { return k == Kind::Loadout || k == Kind::Waves || k == Kind::Clock; }
inline bool GunStep(int step) { return step != kSuitStep; }

inline uint32_t Mix(uint32_t x) {
    x += 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u;
    return x ^ (x >> 16);
}
inline uint32_t HashName(const char* s) {
    uint32_t h = 2166136261u;
    for (; s && *s; ++s) h = (h ^ static_cast<unsigned char>(*s)) * 16777619u;
    return h;
}
struct Rng {
    uint32_t s = 0;
    explicit Rng(uint32_t seed) : s(seed) {}
    uint32_t Next() { s += 0x6D2B79F5u; return Mix(s); }
    int Below(int n) { return n > 0 ? static_cast<int>(Next() % static_cast<uint32_t>(n)) : 0; }
};

// "3,5,7,9" or "2, 4.5, 6, 8": up to max numbers, a missing or bad one keeps its value.
inline int ParseList(const char* text, float* out, int max) {
    int n = 0;
    const char* p = text;
    while (p && *p && n < max) {
        while (*p == ' ' || *p == '\t') ++p;
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end != p && v >= 0.0 && v < 1000.0) out[n] = static_cast<float>(v);
        ++n;
        p = end != p ? end : p;
        while (*p && *p != ',') ++p;
        if (*p == ',') ++p;
    }
    return n;
}

// Survival director state (1 arming, 2 wave, 3 intermission, 4 dead): waves fully
// behind the players. The break after wave N counts as N.
inline int CompletedWaves(int state, int wave) {
    if (wave <= 0) return 0;
    if (state == 3) return wave;
    if (state == 2 || state == 4) return wave - 1;
    return 0;
}

// Cheaper tier: up to the median price of the candidates; the rest is expensive.
// One price for all: both tiers hold every candidate.
struct Tiers { uint32_t cheap = 0, expensive = 0, median = 0; };
inline Tiers SplitTiers(const uint32_t* prices, uint32_t candidates) {
    uint32_t sorted[kGuns];
    int n = 0;
    for (int i = 0; i < kGuns; ++i)
        if ((candidates >> i) & 1u) sorted[n++] = prices[i];
    for (int a = 1; a < n; ++a)
        for (int b = a; b > 0 && sorted[b] < sorted[b - 1]; --b) { const uint32_t t = sorted[b]; sorted[b] = sorted[b - 1]; sorted[b - 1] = t; }
    Tiers t;
    if (!n) return t;
    t.median = sorted[(n - 1) / 2];
    for (int i = 0; i < kGuns; ++i)
        if ((candidates >> i) & 1u) (prices[i] <= t.median ? t.cheap : t.expensive) |= 1u << i;
    if (!t.expensive) t.expensive = t.cheap;
    if (!t.cheap) t.cheap = t.expensive;
    return t;
}

// Favourites weigh more (the first most), guns a teammate already took less.
inline int Weight(int gun, const int* fav, int favCount, uint32_t taken) {
    int w = 2;
    for (int k = 0; k < favCount; ++k)
        if (fav[k] == gun) { w = k == 0 ? 8 : k == 1 ? 6 : 4; break; }
    if ((taken >> gun) & 1u) w = w > 2 ? w / 2 : 1;
    return w;
}
inline int Pick(Rng& rng, uint32_t pool, const int* fav, int favCount, uint32_t taken) {
    int total = 0;
    for (int i = 0; i < kGuns; ++i)
        if ((pool >> i) & 1u) total += Weight(i, fav, favCount, taken);
    if (!total) return -1;
    int r = rng.Below(total);
    for (int i = 0; i < kGuns; ++i) {
        if (!((pool >> i) & 1u)) continue;
        r -= Weight(i, fav, favCount, taken);
        if (r < 0) return i;
    }
    return -1;
}

struct GunPlan { int gun[3] = {-1, -1, -1}; };
// Cheaper gun, expensive gun, then any other. taken[k]: the teammates' picks for step k.
inline GunPlan PlanProgression(Rng& rng, const Tiers& t, const int* fav, int favCount, const uint32_t* taken) {
    GunPlan p;
    const uint32_t all = (t.cheap | t.expensive) & ~(1u << kCutter);
    p.gun[0] = Pick(rng, t.cheap & all, fav, favCount, taken[0]);
    uint32_t used = p.gun[0] >= 0 ? 1u << p.gun[0] : 0u;
    p.gun[1] = Pick(rng, t.expensive & all & ~used, fav, favCount, taken[1]);
    if (p.gun[1] < 0) p.gun[1] = Pick(rng, all & ~used, fav, favCount, taken[1]);
    if (p.gun[1] >= 0) used |= 1u << p.gun[1];
    p.gun[2] = Pick(rng, all & ~used, fav, favCount, taken[2]);
    return p;
}
// The Pulse Rifle as for every player, plus one more of any gun.
inline GunPlan PlanLoadout(Rng& rng, const int* fav, int favCount, uint32_t taken) {
    GunPlan p;
    p.gun[0] = kPulseRifle;
    p.gun[1] = Pick(rng, kAllGuns & ~(1u << kPulseRifle), fav, favCount, taken);
    return p;
}

// Up to stagger + 1 bots each get their own slot in a step; the rotation moves
// who goes first from step to step.
inline int StaggerSlot(unsigned botIndex, int rotation, int stagger) {
    if (stagger <= 0) return 0;
    return static_cast<int>((botIndex + static_cast<unsigned>(rotation < 0 ? 0 : rotation)) % static_cast<unsigned>(stagger + 1));
}
// When each step comes, in waves or seconds: start + slot * unit. 0 = never. A step
// never comes before an earlier one.
inline void DueTimes(const float* start, const int* slots, float unit, float* out) {
    float last = 0.0f;
    for (int s = 0; s < kSteps; ++s) {
        if (start[s] <= 0.0f) { out[s] = 0.0f; continue; }
        out[s] = start[s] + static_cast<float>(slots[s]) * unit;
        if (out[s] < last) out[s] = last;
        last = out[s];
    }
}

// Suits in the order a bot tries them: its favourites, then the rest shuffled;
// suits a teammate already picked go behind, the patient suit never. Returns the count.
inline int SuitOrder(Rng& rng, int kinds, const int* fav, int favCount, int patient, uint32_t mates, int* out, int cap) {
    if (kinds > kMaxSuits) kinds = kMaxSuits;
    int rest[kMaxSuits];
    int restCount = 0;
    for (int i = 0; i < kinds; ++i) {
        bool favourite = false;
        for (int k = 0; k < favCount; ++k) favourite = favourite || fav[k] == i;
        if (i != patient && !favourite) rest[restCount++] = i;
    }
    for (int i = restCount - 1; i > 0; --i) {
        const int j = rng.Below(i + 1);
        const int t = rest[i]; rest[i] = rest[j]; rest[j] = t;
    }
    int n = 0;
    auto add = [&](int idx) {
        if (idx < 0 || idx >= kinds || idx == patient || n >= cap) return;
        for (int k = 0; k < n; ++k) if (out[k] == idx) return;
        out[n++] = idx;
    };
    for (int pass = 0; pass < 2; ++pass) {
        const bool wantTaken = pass == 1;
        for (int k = 0; k < favCount; ++k)
            if (fav[k] >= 0 && fav[k] < kinds && (((mates >> fav[k]) & 1u) != 0) == wantTaken) add(fav[k]);
        for (int k = 0; k < restCount; ++k)
            if ((((mates >> rest[k]) & 1u) != 0) == wantTaken) add(rest[k]);
    }
    return n;
}
}  // namespace coop_bot_progress
