// Dead Space 2 United by Hafer.
#pragma once
#include <cmath>
#include <cstdint>

// The stasis mine's and the heal field's arithmetic: who is in reach, who may be
// slowed or healed, by how much, and when a charge is back. No game memory here;
// runtime_coop_gadgets.inc reads the world and calls these. Distances in metres,
// times in milliseconds of game time.
namespace coop_gadgets {

struct V3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
inline bool Finite(V3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
inline float Flat(V3 a, V3 b) { const float dx = a.x - b.x, dz = a.z - b.z; return std::sqrt(dx * dx + dz * dz); }
inline float Clamp(float v, float lo, float hi) { return !(v >= lo) ? lo : v > hi ? hi : v; }

// Inside a flat circle around center, at most maxRise above or below it. Feet
// positions on both sides, so a crawler on the ceiling or the next floor is out.
inline bool Within(V3 center, V3 at, float radius, float maxRise) {
    if (!Finite(center) || !Finite(at) || !(radius >= 0.0f)) return false;
    return std::fabs(at.y - center.y) <= maxRise && Flat(center, at) <= radius;
}

struct MineConfig {
    float triggerMeters = 2.5f;
    float radiusMeters = 4.0f;
    float rechargeSeconds = 40.0f;
    float armSeconds = 1.0f;          // after placing, before it can go off
    float maxRise = 2.0f;             // vertical reach of trigger and burst
    float pickupRange = 4.0f;         // 0 = from anywhere
    float placeDistance = 1.2f;
    float durationSeconds = 0.0f;     // 0 = the owner's own stasis duration
    float slowPercent = 0.0f;         // 0 = the game's stasis strength
};
inline MineConfig Sanitized(MineConfig c) {
    // The bot senses list an idle necromorph only within 10 m of the mine.
    c.triggerMeters = Clamp(c.triggerMeters, 0.5f, 10.0f);
    c.radiusMeters = Clamp(c.radiusMeters, 0.5f, 10.0f);
    if (c.radiusMeters < c.triggerMeters) c.radiusMeters = c.triggerMeters;
    c.rechargeSeconds = Clamp(c.rechargeSeconds, 0.0f, 3600.0f);
    c.armSeconds = Clamp(c.armSeconds, 0.0f, 10.0f);
    c.maxRise = Clamp(c.maxRise, 0.5f, 6.0f);
    c.pickupRange = Clamp(c.pickupRange, 0.0f, 1000.0f);
    c.placeDistance = Clamp(c.placeDistance, 0.8f, 4.0f);
    c.durationSeconds = Clamp(c.durationSeconds, 0.0f, 30.0f);
    c.slowPercent = Clamp(c.slowPercent, 0.0f, 99.0f);
    return c;
}

struct FieldConfig {
    float durationSeconds = 12.0f;
    float radiusMeters = 3.0f;
    float ratePercent = 8.0f;         // of max health per second
    float rechargeSeconds = 60.0f;    // counted from the end of the field
    float maxRise = 2.0f;
    float placeDistance = 1.0f;
    uint32_t tickMs = 250;            // one heal step per tick
};
inline FieldConfig Sanitized(FieldConfig c) {
    c.durationSeconds = Clamp(c.durationSeconds, 1.0f, 120.0f);
    c.radiusMeters = Clamp(c.radiusMeters, 0.5f, 15.0f);
    c.ratePercent = Clamp(c.ratePercent, 0.1f, 100.0f);
    c.rechargeSeconds = Clamp(c.rechargeSeconds, 0.0f, 3600.0f);
    c.maxRise = Clamp(c.maxRise, 0.5f, 6.0f);
    c.placeDistance = Clamp(c.placeDistance, 0.8f, 4.0f);
    if (c.tickMs < 50) c.tickMs = 50;
    if (c.tickMs > 1000) c.tickMs = 1000;
    return c;
}
inline uint32_t Ms(float seconds) { return seconds > 0.0f ? static_cast<uint32_t>(seconds * 1000.0f + 0.5f) : 0u; }

// --- the mine's targets --------------------------------------------------------

// What the scan knows about something near the mine.
struct Body {
    uint32_t id = 0;
    V3 pos;                    // feet
    float health = 0.0f;
    bool player = false;       // an Isaac: the human, a partner's ghost or a necro player
    bool bot = false;
    bool neutral = false;      // people who are not enemies
    bool objective = false;    // the Marker's heart
    bool ghost = false;        // another machine's copy: its owner decides
    bool hidden = false;
};
inline bool StasisTarget(const Body& b) {
    return b.id && b.health > 0.0f && !b.player && !b.bot && !b.neutral && !b.objective && !b.ghost && !b.hidden && Finite(b.pos);
}
inline bool Triggered(V3 mine, const Body* bodies, int n, const MineConfig& c) {
    for (int i = 0; i < n; ++i)
        if (StasisTarget(bodies[i]) && Within(mine, bodies[i].pos, c.triggerMeters, c.maxRise)) return true;
    return false;
}
// Indices of everything the burst slows, at most cap.
inline int InBurst(V3 mine, const Body* bodies, int n, const MineConfig& c, int* out, int cap) {
    int m = 0;
    for (int i = 0; i < n && m < cap; ++i)
        if (StasisTarget(bodies[i]) && Within(mine, bodies[i].pos, c.radiusMeters, c.maxRise)) out[m++] = i;
    return m;
}

// The stasis values handed to the game: the owner's own unless the INI says
// otherwise. Out of range readings fall back to the retail numbers seen live
// (95 percent, 8 seconds), never to nothing.
struct StasisValues { float slowPercent = 95.0f; float durationSeconds = 8.0f; bool fromGame = true; };
inline StasisValues Stasis(const MineConfig& c, float gameSlow, float playerDuration) {
    StasisValues v;
    const bool slowOk = std::isfinite(gameSlow) && gameSlow > 0.0f && gameSlow <= 100.0f;
    const bool durationOk = std::isfinite(playerDuration) && playerDuration > 0.0f && playerDuration <= 60.0f;
    v.fromGame = slowOk && durationOk && !(c.slowPercent > 0.0f) && !(c.durationSeconds > 0.0f);
    v.slowPercent = c.slowPercent > 0.0f ? c.slowPercent : slowOk ? gameSlow : 95.0f;
    v.durationSeconds = c.durationSeconds > 0.0f ? c.durationSeconds : durationOk ? playerDuration : 8.0f;
    return v;
}

// --- the heal field ----------------------------------------------------------

struct Patient {
    uint32_t id = 0;
    V3 pos;
    float health = 0.0f, max = 0.0f;
    bool dead = false;         // the component says so, whatever the number
    bool local = true;         // this machine owns its health
};
// The health one heal step brings: rate percent of max per second over dtMs,
// never past max, nothing for the dead (no revival) or someone else's copy.
inline float HealAmount(const Patient& p, float ratePercent, uint32_t dtMs) {
    if (!p.local || p.dead || !(p.health > 0.0f) || !(p.max > 0.0f) || !std::isfinite(p.health) || !std::isfinite(p.max)) return 0.0f;
    if (p.health >= p.max) return 0.0f;
    const float amount = p.max * ratePercent / 100.0f * static_cast<float>(dtMs) / 1000.0f;
    if (!(amount > 0.0f)) return 0.0f;
    return amount < p.max - p.health ? amount : p.max - p.health;
}
inline bool InField(V3 field, const Patient& p, const FieldConfig& c) { return Within(field, p.pos, c.radiusMeters, c.maxRise); }

// Game time between ticks: nothing across a pause (the clock stands), a jump
// backwards or a gap of a minute or more (a load).
struct Clock {
    uint32_t last = 0;
    bool known = false;
    bool paused = false;
    uint32_t Step(uint32_t now) {
        uint32_t dt = 0;
        paused = known && now == last;
        if (known && now != last && static_cast<int32_t>(now - last) > 0 && now - last < 60000) dt = now - last;
        last = now;
        known = now != 0;
        return dt;
    }
};

// Counts down in game time.
struct Countdown {
    uint32_t left = 0;
    void Start(uint32_t ms) { left = ms; }
    // True on the step that reaches zero.
    bool Advance(uint32_t dt) {
        if (!left) return false;
        if (dt >= left) { left = 0; return true; }
        left -= dt;
        return false;
    }
    int SecondsLeft() const { return static_cast<int>((left + 999) / 1000); }
};

// One charge: ready, out in the world, or recharging.
enum class ChargeState : uint8_t { Ready, Out, Recharging };
struct Charge {
    ChargeState state = ChargeState::Ready;
    Countdown recharge;
    bool Take() {
        if (state != ChargeState::Ready) return false;
        state = ChargeState::Out;
        return true;
    }
    // Back unused: at once.
    void Refund() {
        if (state != ChargeState::Out) return;
        state = ChargeState::Ready;
        recharge.left = 0;
    }
    // Used up: back after the recharge.
    void Spend(uint32_t rechargeMs) {
        if (state != ChargeState::Out) return;
        if (!rechargeMs) { state = ChargeState::Ready; return; }
        state = ChargeState::Recharging;
        recharge.Start(rechargeMs);
    }
    // True on the step it becomes ready again.
    bool Advance(uint32_t dt) {
        if (state != ChargeState::Recharging) return false;
        if (recharge.Advance(dt) || !recharge.left) { state = ChargeState::Ready; return true; }
        return false;
    }
    int SecondsLeft() const { return state == ChargeState::Recharging ? recharge.SecondsLeft() : 0; }
};
inline const char* ChargeName(ChargeState s) {
    switch (s) {
    case ChargeState::Ready: return "ready";
    case ChargeState::Out: return "out";
    case ChargeState::Recharging: return "recharging";
    }
    return "?";
}

// What a key press does with the mine.
enum class MinePress : uint8_t { Place, PickUp, TooFar, Recharging, NotNow };
inline MinePress MineAction(bool placed, float distance, bool worldReady, ChargeState charge, const MineConfig& c) {
    if (placed) return !(c.pickupRange > 0.0f) || distance <= c.pickupRange ? MinePress::PickUp : MinePress::TooFar;
    if (charge == ChargeState::Recharging) return MinePress::Recharging;
    if (!worldReady || charge != ChargeState::Ready) return MinePress::NotNow;
    return MinePress::Place;
}
enum class FieldPress : uint8_t { Place, Running, Recharging, NotNow };
inline FieldPress FieldAction(bool running, bool worldReady, ChargeState charge) {
    if (running) return FieldPress::Running;
    if (charge == ChargeState::Recharging) return FieldPress::Recharging;
    if (!worldReady || charge != ChargeState::Ready) return FieldPress::NotNow;
    return FieldPress::Place;
}

// A key press is the moment it goes down.
struct Edge {
    bool was = false;
    bool Pressed(bool down) { const bool p = down && !was; was = down; return p; }
};

// --- the radius rings ----------------------------------------------------------

// Effect points on a gadget's radius: the mine's trigger reach while armed, its
// burst reach for a moment when it goes off, the heal field's reach while it runs.
constexpr int kRingMaxPoints = 24;
constexpr float kRingPi = 3.14159265f;
struct RingConfig {
    bool enabled = true;
    int points = 10;
    int maxEffects = 24;          // live ring effects of both gadgets together
    int perSecond = 24;           // ring effects created per second
    float pulseSeconds = 3.0f;    // the mine's ring is renewed once around in this time, 0 = never
    float fadeSeconds = 2.0f;     // the field's ring thins out over its last seconds
    float burstSeconds = 1.2f;    // how long the burst ring stays
    float lift = 0.05f;           // above the gadget's floor
    bool wallCheck = true;        // drop points behind a wall or over a drop
};
inline RingConfig Sanitized(RingConfig c) {
    c.points = c.points < 3 ? 3 : c.points > kRingMaxPoints ? kRingMaxPoints : c.points;
    c.maxEffects = c.maxEffects < 1 ? 1 : c.maxEffects > 48 ? 48 : c.maxEffects;
    c.perSecond = c.perSecond < 1 ? 1 : c.perSecond > 60 ? 60 : c.perSecond;
    c.pulseSeconds = Clamp(c.pulseSeconds, 0.0f, 30.0f);
    c.fadeSeconds = Clamp(c.fadeSeconds, 0.0f, 10.0f);
    c.burstSeconds = Clamp(c.burstSeconds, 0.2f, 5.0f);
    c.lift = Clamp(c.lift, 0.0f, 1.0f);
    return c;
}

// Point i of n, evenly around center, the first one along +z.
inline V3 RingPoint(V3 center, float radius, int i, int n, float lift) {
    const float a = n > 0 ? 2.0f * kRingPi * static_cast<float>(i) / static_cast<float>(n) : 0.0f;
    return {center.x + radius * std::sin(a), center.y + lift, center.z + radius * std::cos(a)};
}

// The order points keep while a ring thins: bit reversed, so what is left stays
// spread around the circle. Rank 0 stays longest; ranks are 0..n-1, each once.
inline int RingRank(int i, int n) {
    if (n <= 0 || n > 64 || i < 0 || i >= n) return n;
    bool seen[64]{};
    int rank = 0;
    for (unsigned j = 0; j < 64; ++j) {
        unsigned r = 0;
        for (int b = 0; b < 6; ++b) r |= ((j >> b) & 1u) << (5 - b);
        const int index = static_cast<int>(r * static_cast<unsigned>(n) / 64u);
        if (seen[index]) continue;
        seen[index] = true;
        if (index == i) return rank;
        ++rank;
    }
    return n;
}

// How many points show with leftMs to go: all of them until the last fadeMs,
// then fewer in proportion, none at the end.
inline int VisibleCount(int n, uint32_t leftMs, uint32_t fadeMs) {
    if (n <= 0) return 0;
    if (!fadeMs || leftMs >= fadeMs) return n;
    return static_cast<int>((static_cast<uint64_t>(n) * leftMs + fadeMs - 1) / fadeMs);
}
inline bool PointShown(int i, int n, int visible) { return RingRank(i, n) < visible; }

// How many of wanted new effects fit under the cap with live ones up.
inline int RingBudget(int wanted, int live, int cap) {
    const int room = cap - live;
    return wanted <= 0 || room <= 0 ? 0 : wanted < room ? wanted : room;
}

// Game time between two renewed points so the whole ring is renewed once per pulse.
inline uint32_t PulseStepMs(int n, float pulseSeconds) {
    if (n <= 0 || !(pulseSeconds > 0.0f)) return 0;
    const uint32_t step = Ms(pulseSeconds) / static_cast<uint32_t>(n);
    return step < 50 ? 50 : step;
}

}  // namespace coop_gadgets
