// Dead Space 2 United by Richard Hafer.
// How the companion bot gets somewhere, without the game.
//
// Three ways, tried in this order:
//  1. NavPower: a path from the navigation mesh (the one the enemies use),
//     followed waypoint by waypoint and asked again when the goal moves or
//     the bot gets stuck.
//  2. Shadow: where the mesh has no answer (vents, zero gravity, lifts, places
//     only Isaac can go) the bot walks the leader's own trail (the spots the
//     leader actually stood on, in the order he walked them), so it cannot
//     leave the ground the leader proved.
//  3. Catch-up: stuck even on the trail, or hopelessly far behind: the runtime
//     puts the bot next to the leader.
// The runtime supplies the path query and does the actual moving; everything
// here is arithmetic, tested in tests/coop_bot_brain_tests.cpp.
#pragma once
#include <cstdint>
#include "coop_bot_brain.h"

namespace coop_bot {

constexpr int kMaxWaypoints = 48, kTrailPoints = 240;

struct Trail {
    Vec3 points[kTrailPoints];
    uint32_t at[kTrailPoints]{};
    int head = 0, count = 0;
    // A new point when the leader has moved a little (0.75 m), so the trail is
    // spaced by distance, not by time: standing still adds nothing.
    void Note(Vec3 p, uint32_t now) {
        if (count && Distance(points[(head + kTrailPoints - 1) % kTrailPoints], p) < 0.75f) return;
        points[head] = p;
        at[head] = now;
        head = (head + 1) % kTrailPoints;
        if (count < kTrailPoints) ++count;
    }
    void Clear() { head = count = 0; }
    Vec3 Get(int i) const { return points[(head - count + i + kTrailPoints) % kTrailPoints]; }   // 0 = oldest
    // The point to walk to: the one after the trail point nearest to the bot.
    bool NextFrom(Vec3 self, Vec3& out) const {
        if (!count) return false;
        int best = 0;
        float bestD = 1e9f;
        for (int i = 0; i < count; ++i) {
            const float d = Distance(Get(i), self);
            if (d <= bestD) { bestD = d; best = i; }   // later points win ties: keep going forward
        }
        if (bestD > 12.0f) return false;               // not on the trail at all
        out = Get(best + 1 < count ? best + 1 : best);
        return true;
    }
};

enum class Route : uint8_t { None, Nav, Shadow, Direct };

struct SteerOutput {
    bool move = false;
    Vec3 toward;          // the point to walk at this frame
    Route route = Route::None;
    bool teleport = false;  // give up walking: the runtime moves the bot to the leader
    bool askPath = false;   // the runtime should query NavPower from self to goal now
    bool blocked = false;   // the goal is off the mesh and the bot stands at the closest point to it
    Vec3 reachable;         // that closest point (valid with blocked)
};

class Steer {
public:
    // Tuning.
    float arrive = 0.8f;             // a waypoint counts as reached
    float replanMove = 1.5f;         // goal moved this far: ask again
    uint32_t replanMs = 1500;        // and at least this often while walking
    uint32_t stuckMs = 2000;         // no progress for this long counts as stuck
    float stuckDistance = 0.35f;
    uint32_t shadowAfterMs = 1500;   // no path this long: walk the trail
    uint32_t teleportAfterMs = 7000; // stuck on every route this long: catch up
    float teleportDistance = 45.0f;  // or this far behind the leader
    bool distanceTeleport = true;    // false: only being stuck puts the bot by the leader
    // Getting closer at all: the way left (along the path) has to shrink by
    // progressGain within giveUpMs, or the goal is given up where the bot stands.
    // Moving alone does not count (28.09.: a bot rocked between a wall and the
    // leader's trail for a minute, never "stuck", never caught up).
    float progressGain = 0.4f;
    uint32_t giveUpMs = 4000;
    float giveUpNear = 8.0f;         // beyond this from the leader, no progress means catch up instead

    // NavPower answers an unreachable goal with a path to the closest point it
    // can reach, so where the path ends says whether the goal itself is on the mesh.
    void SetPath(const Vec3* points, int n, uint32_t now) {
        count_ = n < kMaxWaypoints ? n : kMaxWaypoints;
        for (int i = 0; i < count_; ++i) path_[i] = points[i];
        next_ = count_ > 1 ? 1 : 0;   // point 0 is where the bot stands
        pathAt_ = now;
        if (count_ > 0) {
            noPathSince_ = 0;
            end_ = path_[count_ - 1];
            reaches_ = Flat(end_, goal_) < 0.6f && std::fabs(end_.y - goal_.y) < 1.2f;
        }
    }
    void NoPath(uint32_t now) {
        count_ = next_ = 0;
        pathAt_ = now;
        if (!noPathSince_) noPathSince_ = now;
    }
    void Reset() { count_ = next_ = 0; pathAt_ = noPathSince_ = progressAt_ = stuckSince_ = bestAt_ = 0; goalSet_ = false; }

    SteerOutput Tick(Vec3 self, Vec3 goal, bool wantMove, Vec3 leader, const Trail& trail, uint32_t now) {
        SteerOutput o;
        if (!wantMove) { stuckSince_ = 0; progressAt_ = now; progressFrom_ = self; bestAt_ = 0; return o; }
        o.move = true;
        // Progress check: moved at least stuckDistance within stuckMs.
        if (!progressAt_ || Distance(self, progressFrom_) >= stuckDistance) { progressAt_ = now; progressFrom_ = self; stuckSince_ = 0; }
        else if (now - progressAt_ >= stuckMs && !stuckSince_) stuckSince_ = now;
        const bool stuck = stuckSince_ != 0;
        // Far behind or stuck for good: the runtime puts the bot by the leader.
        if ((distanceTeleport && Flat(self, leader) > teleportDistance) || (stuck && now - stuckSince_ >= teleportAfterMs)) {
            o.teleport = true;
            Reset();
            return o;
        }
        // Ask for a path when there is none, the goal moved, it is old, or the bot is stuck.
        const bool goalMoved = !goalSet_ || Distance(goal, goal_) > replanMove;
        if (goalMoved || !pathAt_ || now - pathAt_ >= replanMs || (stuck && now - pathAt_ >= 500)) {
            o.askPath = true;
            goal_ = goal;
            goalSet_ = true;
        }
        if (goalMoved) bestAt_ = 0;
        // No closer for giveUpMs: this goal is out of reach, stop here.
        const float left = WayLeft(self, goal);
        if (!bestAt_ || left < best_ - progressGain) { best_ = left; bestAt_ = now; }
        else if (now - bestAt_ >= giveUpMs) {
            // Far from the leader and getting nowhere: catch up rather than stay behind.
            if (Flat(self, leader) > giveUpNear) {
                o.teleport = true;
                Reset();
                return o;
            }
            o.move = false;
            o.blocked = true;
            o.reachable = self;
            bestAt_ = 0;
            stuckSince_ = 0;
            progressAt_ = now;
            progressFrom_ = self;
            ++giveUps_;
            return o;
        }
        // The goal is off the mesh (behind a wall, inside a prop) and the bot stands
        // as close as the mesh lets it: that is arriving, not pushing on into the wall.
        if (!goalMoved && count_ > 0 && !reaches_ && Flat(self, end_) < arrive) {
            o.move = false;
            o.blocked = true;
            o.reachable = end_;
            stuckSince_ = 0;
            progressAt_ = now;
            progressFrom_ = self;
            return o;
        }
        // Walk the path.
        if (count_ > 0 && !(stuck && now - stuckSince_ >= 1000)) {
            while (next_ < count_ - 1 && Flat(self, path_[next_]) < arrive) ++next_;
            o.toward = path_[next_];
            o.route = Route::Nav;
            return o;
        }
        // No path (for a while), or stuck on it: the leader's trail.
        if ((noPathSince_ && now - noPathSince_ >= shadowAfterMs) || stuck) {
            Vec3 next;
            if (trail.NextFrom(self, next)) {
                o.toward = next;
                o.route = Route::Shadow;
                return o;
            }
        }
        // Nothing better: straight at it, until one of the others kicks in.
        o.toward = goal;
        o.route = Route::Direct;
        return o;
    }

    int Waypoints() const { return count_; }
    unsigned GiveUps() const { return giveUps_; }
    bool Stuck() const { return stuckSince_ != 0; }

private:
    Vec3 path_[kMaxWaypoints];
    int count_ = 0, next_ = 0;
    uint32_t pathAt_ = 0, noPathSince_ = 0, progressAt_ = 0, stuckSince_ = 0;
    Vec3 progressFrom_, goal_, end_;
    bool goalSet_ = false, reaches_ = true;
    float best_ = 0.0f;
    uint32_t bestAt_ = 0;
    unsigned giveUps_ = 0;
    // The way still to go: along the path when there is one, else straight.
    float WayLeft(Vec3 self, Vec3 goal) const {
        if (count_ <= 0 || next_ >= count_) return Flat(self, goal);
        float left = Flat(self, path_[next_]);
        for (int i = next_; i + 1 < count_; ++i) left += Flat(path_[i], path_[i + 1]);
        return left;
    }
};

// --- zero-G flight ---------------------------------------------------------------
// A flying Isaac thrusts along its own
// body: the left stick pushes forward (ly -1, 3 m/s), back and sideways (lx +1 to
// its right, 2 m/s), never up or down; it climbs by pitching. Its look input turns
// the body itself (yaw > 0 to its left, pitch > 0 down, before the signs the
// runtime learns). The frame is its CharacterMovementComponent's.
struct BodyFrame { Vec3 pos, forward, left, up; };
struct FlightStick {
    float yaw = 0.0f, pitch = 0.0f;   // look command, -1..1
    float lx = 0.0f, ly = 0.0f;       // thrust
    bool boost = false;               // run held: twice the speed
    float lookOffDeg = 0.0f;          // the look point off the body's forward
    float distance = 0.0f;            // to the goal
};
struct FlightTuning {
    float turnGain = 1.6f;    // look command per radian off
    float maxTurn = 0.8f;     // above 0.8 the machine speeds the turn up on its own
    float minTurn = 0.14f;    // the look curve's dead zone is 0.1
    float alignDeg = 25.0f;   // full thrust once the goal lies this close ahead
    float arrive = 1.0f;      // no thrust inside this
    float slowFrom = 3.5f;    // less thrust inside this (it drifts on, 2.5 m/s2 to a stop)
    float boostFrom = 9.0f;   // boost beyond this, when not aiming
};
inline float ClampUnit(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Turns the body toward lookAt and pushes toward goal. Looking where it flies it
// turns first and thrusts forward once aligned; looking elsewhere (a fight) the
// goal's direction is split over forward and sideways thrust.
inline FlightStick FlyToward(const BodyFrame& b, Vec3 goal, Vec3 lookAt, bool lookingAway, bool aiming, const FlightTuning& t = FlightTuning{}) {
    FlightStick s;
    const Vec3 look = Sub(lookAt, b.pos);
    const float f = Dot(look, b.forward), l = Dot(look, b.left), u = Dot(look, b.up);
    auto turn = [&](float off) {
        if (std::fabs(off) < 0.012f) return 0.0f;
        float c = off * t.turnGain;
        if (std::fabs(c) < t.minTurn) c = c < 0.0f ? -t.minTurn : t.minTurn;
        return ClampUnit(c, -t.maxTurn, t.maxTurn);
    };
    s.yaw = turn(std::atan2(l, f));
    s.pitch = -turn(std::atan2(u, std::sqrt(f * f + l * l)));
    const float lookLength = Length(look);
    s.lookOffDeg = lookLength > 0.01f ? std::acos(ClampUnit(f / lookLength, -1.0f, 1.0f)) * 57.2957795f : 0.0f;
    const Vec3 to = Sub(goal, b.pos);
    s.distance = Length(to);
    if (s.distance < t.arrive) return s;
    const Vec3 dir = Scale(to, 1.0f / s.distance);
    const float along = Dot(dir, b.forward), right = -Dot(dir, b.left);
    const float ramp = ClampUnit((s.distance - t.arrive) / (t.slowFrom - t.arrive), 0.3f, 1.0f);
    if (lookingAway) {
        s.ly = -ClampUnit(along * 1.5f, -1.0f, 1.0f) * ramp;
        s.lx = ClampUnit(right * 1.5f, -1.0f, 1.0f) * ramp;
        return s;
    }
    const float offDeg = std::acos(ClampUnit(along, -1.0f, 1.0f)) * 57.2957795f;
    if (offDeg <= t.alignDeg) s.ly = -ramp;
    else if (offDeg < 60.0f) s.ly = -ramp * (60.0f - offDeg) / (60.0f - t.alignDeg);
    if (offDeg < 60.0f) s.lx = ClampUnit(right * 1.5f, -1.0f, 1.0f) * ramp;
    s.boost = !aiming && s.distance > t.boostFrom && offDeg <= t.alignDeg;
    return s;
}
// Where a flying bot goes: behind the flying leader, each bot its own place (left,
// right, below), in his body frame; with the leader on his feet, above them beside
// him, where it can come down.
inline Vec3 FlightGoal(const BodyFrame& leader, bool leaderFlying, int squadIndex) {
    const int i = squadIndex % 3;
    static const float kSide[] = {1.4f, -1.4f, 0.0f}, kBack[] = {2.2f, 2.2f, 3.2f}, kRise[] = {0.3f, 0.3f, -1.0f};
    static const float kStandSide[] = {1.3f, -1.3f, 0.0f}, kStandBack[] = {0.2f, 0.2f, 1.4f};
    if (leaderFlying)
        return Add(leader.pos, Add(Add(Scale(leader.forward, -kBack[i]), Scale(leader.left, kSide[i])), Scale(leader.up, kRise[i])));
    return Add(leader.pos, Add(Add(Scale(leader.up, 1.2f), Scale(leader.left, kStandSide[i])), Scale(leader.forward, -kStandBack[i])));
}
// The leader stands on another surface: off the bot's own floor, or turned against it.
inline bool OtherSurface(const BodyFrame& self, const BodyFrame& leader) {
    return std::fabs(Dot(Sub(leader.pos, self.pos), self.up)) > 2.0f || Dot(self.up, leader.up) < 0.87f;
}

// --- facing open space ------------------------------------------------------------
// A bot at rest with a wall (or the end of the walkable mesh) right before its
// face turns toward the most open way, opposite first, and takes a short step.
constexpr int kFaceCandidates = 5;
struct FaceOpenInput {
    float wallAhead = -1.0f;      // metres to geometry straight ahead at eye height, -1 none within the cast
    bool navShort = false;        // the mesh ends short of a point 1.5 m ahead
    float selfYaw = 0.0f;
    float yaws[kFaceCandidates]{};
    float clear[kFaceCandidates]{};   // free metres each way (cast and mesh)
    int count = 0;
    Vec3 self, leader;
    bool holding = false;
    Vec3 holdSpot;
    float step = 0.45f;           // how far the short walk goes
};
struct FaceOpenOutput {
    bool turn = false, step = false;
    int pick = -1;
    float yaw = 0.0f, clear = 0.0f;
    const char* why = "open";
};
inline FaceOpenOutput DecideFaceOpen(const FaceOpenInput& in) {
    FaceOpenOutput out;
    const bool wall = (in.wallAhead >= 0.0f && in.wallAhead < 1.2f) || in.navShort;
    if (!wall) return out;
    float best = 1.5f;
    for (int i = 0; i < in.count && i < kFaceCandidates; ++i) {
        const Vec3 dir = FromYaw(in.yaws[i]);
        const Vec3 toLeader = Sub(in.leader, in.self);
        const float leaderFlat = Flat(in.leader, in.self);
        if (leaderFlat < 2.0f && leaderFlat > 0.05f &&
            (dir.x * toLeader.x + dir.z * toLeader.z) / leaderFlat > 0.77f)
            continue;   // he stands right there: no walking into him
        if (in.clear[i] > best + 0.25f || (out.pick < 0 && in.clear[i] >= best)) { best = in.clear[i]; out.pick = i; }
    }
    if (out.pick < 0) { out.why = "boxed in or the leader in the way"; return out; }
    out.turn = true;
    out.yaw = in.yaws[out.pick];
    out.clear = in.clear[out.pick];
    out.why = "wall ahead";
    out.step = !in.holding || Flat(Add(in.self, Scale(FromYaw(out.yaw), in.step)), in.holdSpot) <= 0.5f;
    return out;
}
// Candidate ways for DecideFaceOpen: opposite first, then the diagonals behind, then the sides.
inline int FaceCandidates(float selfYaw, float* yaws) {
    static const float kOffsets[kFaceCandidates] = {3.1415927f, 2.3561945f, -2.3561945f, 1.5707963f, -1.5707963f};
    for (int i = 0; i < kFaceCandidates; ++i) yaws[i] = selfYaw + kOffsets[i];
    return kFaceCandidates;
}

}  // namespace coop_bot
