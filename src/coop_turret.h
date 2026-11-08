// Dead Space 2 United by Richard Hafer.
#pragma once
#include <cmath>
#include <cstdint>

// The placeable turret's arithmetic: where it may stand, what it shoots, how
// often and for how long. No game memory here; runtime_coop_turret.inc reads
// the world and calls these. Distances in metres, times in milliseconds.
namespace coop_turret {

struct V3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
inline V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 Scale(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Length(V3 a) { return std::sqrt(Dot(a, a)); }
inline float Distance(V3 a, V3 b) { return Length(Sub(a, b)); }
inline float Flat(V3 a, V3 b) { const float dx = a.x - b.x, dz = a.z - b.z; return std::sqrt(dx * dx + dz * dz); }
inline bool Finite(V3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

constexpr float kPi = 3.14159265f;

struct Config {
    float range = 20.0f;              // from the muzzle
    uint32_t fireIntervalMs = 250;    // 4 shots per second
    float damage = 10.0f;             // per shot; the pulse rifle does 5 to 9 per round at 10 rounds/s
    int ammo = 150;                   // rounds when full, 0 = unlimited
    float ammoRegenPerSec = 1.0f;     // rounds back per second, placed or carried
    float heatSeconds = 6.0f;         // continuous fire until it overheats, 0 = never
    float coolSeconds = 3.0f;         // overheated back to cold
    float turnDegPerSec = 360.0f;     // 0 = turns at once
    float aimToleranceDeg = 10.0f;    // fires only this close to the target's bearing
    float placeDistance = 1.5f;       // in front of the player's feet
    float maxRise = 0.9f;             // floor height against the player's feet
    float maxShift = 0.75f;           // how far the nav mesh may move the spot
    float friendClearance = 0.7f;     // nobody may stand on the spot
    float pickupRange = 4.0f;         // 0 = picked up from anywhere
    float muzzleHeight = 0.6f;        // above the floor
    float friendRadius = 0.5f;        // a shot passing a player this close is held
    float friendHeight = 1.9f;        // a player's capsule, from the feet
    float muzzleSkip = 0.3f;          // the first part of the shot line is the turret itself
};

inline float Clamp(float v, float lo, float hi) { return !(v >= lo) ? lo : v > hi ? hi : v; }
// INI values out of any sane range are pulled back in, never rejected.
inline Config Sanitized(Config c) {
    c.range = Clamp(c.range, 2.0f, 60.0f);
    if (c.fireIntervalMs < 50) c.fireIntervalMs = 50;
    if (c.fireIntervalMs > 5000) c.fireIntervalMs = 5000;
    c.damage = Clamp(c.damage, 0.1f, 1000.0f);
    if (c.ammo < 0) c.ammo = 0;
    if (c.ammo > 100000) c.ammo = 100000;
    c.ammoRegenPerSec = Clamp(c.ammoRegenPerSec, 0.0f, 100.0f);
    c.heatSeconds = Clamp(c.heatSeconds, 0.0f, 600.0f);
    c.coolSeconds = Clamp(c.coolSeconds, 0.1f, 600.0f);
    c.turnDegPerSec = Clamp(c.turnDegPerSec, 0.0f, 3600.0f);
    c.aimToleranceDeg = Clamp(c.aimToleranceDeg, 1.0f, 90.0f);
    c.placeDistance = Clamp(c.placeDistance, 0.8f, 4.0f);
    c.maxRise = Clamp(c.maxRise, 0.2f, 3.0f);
    c.maxShift = Clamp(c.maxShift, 0.2f, 3.0f);
    c.friendClearance = Clamp(c.friendClearance, 0.0f, 3.0f);
    c.pickupRange = Clamp(c.pickupRange, 0.0f, 1000.0f);
    c.muzzleHeight = Clamp(c.muzzleHeight, 0.2f, 1.8f);
    c.friendRadius = Clamp(c.friendRadius, 0.1f, 2.0f);
    c.friendHeight = Clamp(c.friendHeight, 0.5f, 3.0f);
    c.muzzleSkip = Clamp(c.muzzleSkip, 0.0f, 2.0f);
    return c;
}

// --- placement ---------------------------------------------------------------

enum class Place : uint8_t { Ok, NoPlayer, NoFloor, TooHigh, Shifted, Blocked, Occupied };
inline const char* PlaceName(Place p) {
    switch (p) {
    case Place::Ok: return "ok";
    case Place::NoPlayer: return "no_player";
    case Place::NoFloor: return "no_floor";
    case Place::TooHigh: return "too_high";
    case Place::Shifted: return "wall";
    case Place::Blocked: return "blocked";
    case Place::Occupied: return "occupied";
    }
    return "?";
}

// The spot asked for: placeDistance ahead along the player's flat facing.
inline bool Candidate(V3 feet, V3 forward, float distance, V3& out) {
    V3 f = {forward.x, 0.0f, forward.z};
    const float len = Length(f);
    if (!Finite(feet) || !std::isfinite(len) || len < 0.1f) return false;
    out = Add(feet, Scale(f, distance / len));
    return true;
}

struct PlaceInput {
    V3 feet, candidate;
    bool meshKnown = false;     // the nav mesh could be asked
    bool meshHit = false;       // and a walk toward the candidate came back
    V3 meshEnd;                 // where that walk ends
    bool floorHit = false;      // a cast straight down at the candidate met something
    float floorY = 0.0f;
};
struct PlaceResult { Place verdict = Place::NoFloor; V3 spot; };

// The nav mesh decides when it is there: a walk from the feet that ends far from
// the candidate ran into a wall or a drop. Without it the floor under the candidate does.
inline PlaceResult SpotFor(const PlaceInput& in, const Config& c) {
    PlaceResult r;
    if (!Finite(in.feet) || !Finite(in.candidate)) { r.verdict = Place::NoPlayer; return r; }
    if (in.meshKnown) {
        if (!in.meshHit || !Finite(in.meshEnd)) { r.verdict = Place::NoFloor; return r; }
        r.spot = in.meshEnd;
        if (Flat(in.meshEnd, in.candidate) > c.maxShift) { r.verdict = Place::Shifted; return r; }
    } else {
        if (!in.floorHit || !std::isfinite(in.floorY)) { r.verdict = Place::NoFloor; return r; }
        r.spot = {in.candidate.x, in.floorY, in.candidate.z};
    }
    if (std::fabs(r.spot.y - in.feet.y) > c.maxRise) { r.verdict = Place::TooHigh; return r; }
    r.verdict = Place::Ok;
    return r;
}
// The rest needs the spot: a clear line from the player's eye and nobody standing there.
inline Place Finish(Place spotVerdict, bool lineClear, float nearestFriend, const Config& c) {
    if (spotVerdict != Place::Ok) return spotVerdict;
    if (!lineClear) return Place::Blocked;
    if (nearestFriend < c.friendClearance) return Place::Occupied;
    return Place::Ok;
}

// --- what to shoot -----------------------------------------------------------

// EA::DamageZone: 0 head, 1/2 front limbs, 3/4 back limbs, 5 tail, 6 sac, 7-9
// generic appendages, 10 torso. Limbs first: that is what dismembers.
inline int ZoneRank(int zone) {
    if (zone == 1 || zone == 2) return 0;
    if (zone >= 5 && zone <= 9) return 1;
    if (zone == 3 || zone == 4) return 2;
    if (zone == 0) return 3;
    if (zone == 10) return 4;
    return 5;
}
// at: the middle of the hit shape. hit: where the line from the muzzle met it,
// when the cast said so. body: the ragdoll body (its collidable is at +0x10).
struct Part { V3 at; int zone = 10; int bone = -1; bool clear = false; uint32_t body = 0; V3 hit; bool hitKnown = false; };
// The best part with a clear line: lowest rank, then nearest. -1 when none is clear.
inline int ChoosePart(const Part* parts, int n, V3 muzzle) {
    int best = -1;
    float bestDist = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (!parts[i].clear || !Finite(parts[i].at)) continue;
        const float d = Distance(parts[i].at, muzzle);
        if (best < 0 || ZoneRank(parts[i].zone) < ZoneRank(parts[best].zone) ||
            (ZoneRank(parts[i].zone) == ZoneRank(parts[best].zone) && d < bestDist)) {
            best = i;
            bestDist = d;
        }
    }
    return best;
}

struct Target {
    uint32_t id = 0;
    float distance = 0.0f;      // muzzle to its body
    float health = 0.0f;
    bool visible = false;       // a clear line to some part
    bool onFriend = false;      // it goes for a player
    bool regenerating = false;  // down and regrowing (Ubermorph): shooting it is wasted
    bool objective = false;     // not an enemy (the Marker's heart)
};
inline bool Eligible(const Target& t, const Config& c) {
    return t.id && t.visible && t.health > 0.0f && !t.regenerating && !t.objective && std::isfinite(t.distance) && t.distance <= c.range;
}
// Lowest score wins: the nearest, with a bonus for one that goes for a player and
// for the one already under fire (no flicker between two equal targets).
constexpr float kFriendBonus = 6.0f, kKeepBonus = 3.0f;
inline int ChooseTarget(const Target* t, int n, uint32_t current, const Config& c) {
    int best = -1;
    float bestScore = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (!Eligible(t[i], c)) continue;
        float score = t[i].distance;
        if (t[i].onFriend) score -= kFriendBonus;
        if (current && t[i].id == current) score -= kKeepBonus;
        if (best < 0 || score < bestScore) { best = i; bestScore = score; }
    }
    return best;
}

// A shot that would pass a player: the line from the muzzle (its first muzzleSkip
// metres excluded) against each player's upright capsule, sampled.
inline float SegmentPointDistance(V3 a, V3 b, V3 p) {
    const V3 ab = Sub(b, a);
    const float len2 = Dot(ab, ab);
    float t = len2 > 1e-6f ? Dot(Sub(p, a), ab) / len2 : 0.0f;
    t = Clamp(t, 0.0f, 1.0f);
    return Distance(Add(a, Scale(ab, t)), p);
}
inline bool FriendInLine(V3 from, V3 to, const V3* feet, int n, const Config& c) {
    const V3 d = Sub(to, from);
    const float len = Length(d);
    if (!std::isfinite(len) || len <= c.muzzleSkip) return false;
    const V3 start = Add(from, Scale(d, c.muzzleSkip / len));
    for (int i = 0; i < n; ++i) {
        if (!Finite(feet[i])) continue;
        const V3 top = {feet[i].x, feet[i].y + c.friendHeight, feet[i].z};
        constexpr int kSteps = 12;
        for (int k = 0; k <= kSteps; ++k) {
            const float y = feet[i].y + (top.y - feet[i].y) * static_cast<float>(k) / kSteps;
            if (SegmentPointDistance(start, to, {feet[i].x, y, feet[i].z}) < c.friendRadius) return true;
        }
    }
    return false;
}

// --- turning -----------------------------------------------------------------

inline float WrapPi(float a) {
    if (!std::isfinite(a)) return 0.0f;
    a = std::fmod(a + kPi, 2.0f * kPi);
    if (a < 0.0f) a += 2.0f * kPi;
    return a - kPi;
}
// Bearing in the game's convention (bot_sense::Yaw): atan2(dx, dz).
inline float YawTo(V3 from, V3 to) { return std::atan2(to.x - from.x, to.z - from.z); }
inline float TurnToward(float yaw, float target, float maxStep) {
    const float diff = WrapPi(target - yaw);
    if (!(maxStep > 0.0f) || std::fabs(diff) <= maxStep) return WrapPi(target);
    return WrapPi(yaw + (diff > 0.0f ? maxStep : -maxStep));
}
inline bool Aligned(float yaw, float target, float toleranceRad) { return std::fabs(WrapPi(target - yaw)) <= toleranceRad; }
// Turn for dtMs at the configured rate (0 = at once).
inline float Turn(float yaw, float target, uint32_t dtMs, const Config& c) {
    if (!(c.turnDegPerSec > 0.0f)) return WrapPi(target);
    return TurnToward(yaw, target, c.turnDegPerSec * kPi / 180.0f * static_cast<float>(dtMs) / 1000.0f);
}

// --- pacing ------------------------------------------------------------------

// One shot per interval. Falling behind (a pause, no target for a while) never
// turns into a burst: the next shot is one interval after the late one.
struct Pacer {
    uint32_t next = 0;
    bool armed = false;
    bool Ready(uint32_t now) const { return !armed || static_cast<int32_t>(now - next) >= 0; }
    void Fired(uint32_t now, uint32_t interval) {
        if (armed && static_cast<int32_t>(now - next) >= 0 && now - next < interval) next += interval;
        else next = now + interval;
        armed = true;
    }
    void Reset() { armed = false; next = 0; }
};

// --- ammunition and heat -----------------------------------------------------

enum class Shot : uint8_t { Fired, Empty, Overheated };
struct Magazine {
    float rounds = 0.0f;
    float heat = 0.0f;          // 0 cold .. 1 overheated
    bool overheated = false;
    void Refill(const Config& c) { rounds = static_cast<float>(c.ammo); heat = 0.0f; overheated = false; }
    bool Unlimited(const Config& c) const { return c.ammo <= 0; }
    // Heat per shot: continuous fire reaches 1 after heatSeconds while it cools
    // at 1/coolSeconds all the time.
    static float HeatPerShot(const Config& c) {
        if (!(c.heatSeconds > 0.0f)) return 0.0f;
        const float rate = 1000.0f / static_cast<float>(c.fireIntervalMs);
        return (1.0f / c.heatSeconds + 1.0f / c.coolSeconds) / rate;
    }
    void Advance(uint32_t dtMs, const Config& c) {
        if (dtMs > 60000) dtMs = 60000;
        const float dt = static_cast<float>(dtMs) / 1000.0f;
        if (!Unlimited(c)) {
            rounds += c.ammoRegenPerSec * dt;
            if (rounds > static_cast<float>(c.ammo)) rounds = static_cast<float>(c.ammo);
        }
        heat -= dt / c.coolSeconds;
        if (heat <= 0.0f) { heat = 0.0f; overheated = false; }
    }
    bool CanFire(const Config& c) const { return !overheated && (Unlimited(c) || rounds >= 1.0f); }
    int Rounds(const Config& c) const { return Unlimited(c) ? -1 : static_cast<int>(rounds); }
    Shot Fire(const Config& c) {
        if (overheated) return Shot::Overheated;
        if (!Unlimited(c)) {
            if (rounds < 1.0f) return Shot::Empty;
            rounds -= 1.0f;
        }
        heat += HeatPerShot(c);
        if (heat >= 1.0f) { heat = 1.0f; overheated = true; }
        return Shot::Fired;
    }
};

// --- feedback effects ----------------------------------------------------------

// The pulse rifle's shot sound is authored inside its muzzle effect, so a flash
// without the sound needs another effect, and the sound alone is not on offer.
enum class ShotFx : uint8_t { None, PulseMuzzle, SilentFlash };
inline ShotFx ChooseShotFx(bool flash, bool sound, bool pulseLoaded) {
    if (flash && sound && pulseLoaded) return ShotFx::PulseMuzzle;
    if (flash) return ShotFx::SilentFlash;
    return ShotFx::None;
}

// At most perSecond plays in a window of one second that opens with the first one.
struct RateLimit {
    uint32_t windowStart = 0;
    int count = 0;
    bool started = false;
    bool Allow(uint32_t now, int perSecond) {
        if (perSecond <= 0) return false;
        if (!started || now - windowStart >= 1000) { windowStart = now; count = 0; started = true; }
        if (count >= perSecond) return false;
        ++count;
        return true;
    }
    void Reset() { started = false; count = 0; }
};

// An effect matrix in the entity layout (rows right, up, forward, position) with
// forward along dir. Straight up or down takes world X as right.
inline bool Basis(V3 dir, V3 pos, float m[16]) {
    const float len = Length(dir);
    if (!Finite(dir) || !Finite(pos) || !(len > 1e-4f)) return false;
    const V3 f = Scale(dir, 1.0f / len);
    V3 r = Cross({0.0f, 1.0f, 0.0f}, f);
    const float rl = Length(r);
    r = rl > 1e-3f ? Scale(r, 1.0f / rl) : V3{1.0f, 0.0f, 0.0f};
    const V3 u = Cross(f, r);
    const V3 rows[4] = {r, u, f, pos};
    for (int i = 0; i < 4; ++i) {
        m[i * 4 + 0] = rows[i].x;
        m[i * 4 + 1] = rows[i].y;
        m[i * 4 + 2] = rows[i].z;
        m[i * 4 + 3] = i == 3 ? 1.0f : 0.0f;
    }
    return true;
}

// Where the impact shows: the cast's contact point when it is plausible, else the middle of the shape.
inline V3 ImpactPoint(V3 center, V3 hit, bool hitKnown, float maxOffset) {
    return hitKnown && Finite(hit) && Distance(hit, center) <= maxOffset ? hit : center;
}

// A key press is the moment it goes down.
struct Edge {
    bool was = false;
    bool Pressed(bool down) { const bool p = down && !was; was = down; return p; }
};

}  // namespace coop_turret
