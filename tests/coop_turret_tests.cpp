// The turret's arithmetic without a game: placement, target and part choice,
// the friendly line check, turning, fire pacing, ammunition and heat, and the
// choices behind the feedback effects.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string_view>
#include "../src/coop_turret.h"

namespace {
using namespace coop_turret;

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

void Sanitizing() {
    Config c;
    c.range = 500.0f; c.fireIntervalMs = 1; c.damage = -3.0f; c.ammo = -5; c.coolSeconds = 0.0f; c.placeDistance = 50.0f;
    const Config s = Sanitized(c);
    assert(s.range == 60.0f && s.fireIntervalMs == 50 && s.damage == 0.1f && s.ammo == 0 && s.coolSeconds == 0.1f && s.placeDistance == 4.0f);
    Config nan;
    nan.range = std::nanf("");
    assert(Sanitized(nan).range == 2.0f);   // NaN goes to the low end, never through
    std::printf("sanitizing ok\n");
}

void CandidateAhead() {
    V3 out;
    assert(Candidate({1.0f, 2.0f, 3.0f}, {0.0f, 0.5f, 2.0f}, 1.5f, out));   // pitch ignored, length ignored
    assert(Near(out.x, 1.0f) && Near(out.y, 2.0f) && Near(out.z, 4.5f));
    assert(!Candidate({0, 0, 0}, {0.0f, 1.0f, 0.0f}, 1.5f, out));          // looking straight up: no facing
    assert(!Candidate({std::nanf(""), 0, 0}, {1, 0, 0}, 1.5f, out));
    std::printf("candidate ok\n");
}

void PlacementWithMesh() {
    Config c;
    PlaceInput in;
    in.feet = {0, 0, 0};
    in.candidate = {0, 0, 1.5f};
    in.meshKnown = true;
    in.meshHit = true;
    in.meshEnd = {0.1f, 0.05f, 1.45f};
    PlaceResult r = SpotFor(in, c);
    assert(r.verdict == Place::Ok && Near(r.spot.z, 1.45f) && Near(r.spot.y, 0.05f));
    assert(Finish(r.verdict, true, 5.0f, c) == Place::Ok);
    assert(Finish(r.verdict, false, 5.0f, c) == Place::Blocked);
    assert(Finish(r.verdict, true, 0.3f, c) == Place::Occupied);
    in.meshEnd = {0, 0, 0.4f};              // the walk stopped at a wall
    assert(SpotFor(in, c).verdict == Place::Shifted);
    in.meshEnd = {0, 1.5f, 1.5f};           // a ledge above
    assert(SpotFor(in, c).verdict == Place::TooHigh);
    in.meshHit = false;                     // mesh known, no path: nothing to stand on
    assert(SpotFor(in, c).verdict == Place::NoFloor);
    assert(Finish(Place::NoFloor, true, 9.0f, c) == Place::NoFloor);   // an earlier verdict stands
    std::printf("placement with mesh ok\n");
}

void PlacementWithoutMesh() {
    Config c;
    PlaceInput in;
    in.feet = {0, 10.0f, 0};
    in.candidate = {1.5f, 10.0f, 0};
    in.floorHit = true;
    in.floorY = 9.8f;
    PlaceResult r = SpotFor(in, c);
    assert(r.verdict == Place::Ok && Near(r.spot.x, 1.5f) && Near(r.spot.y, 9.8f));
    in.floorY = 8.0f;                       // a drop
    assert(SpotFor(in, c).verdict == Place::TooHigh);
    in.floorHit = false;
    assert(SpotFor(in, c).verdict == Place::NoFloor);
    in.feet.x = std::nanf("");
    assert(SpotFor(in, c).verdict == Place::NoPlayer);
    assert(std::string_view(PlaceName(Place::Shifted)) == "wall");
    std::printf("placement without mesh ok\n");
}

void PartChoice() {
    Part p[4];
    p[0] = {{0, 1.2f, 5}, 10, 4, true};     // torso
    p[1] = {{0.4f, 1.3f, 5}, 1, 31, true};  // an arm
    p[2] = {{-0.4f, 1.3f, 5}, 2, 33, false};// the other arm, behind something
    p[3] = {{0.2f, 0.4f, 5}, 3, 12, true};  // a leg
    const V3 muzzle = {0, 0.6f, 0};
    assert(ChoosePart(p, 4, muzzle) == 1);
    p[1].clear = false;
    assert(ChoosePart(p, 4, muzzle) == 3);  // legs before the torso
    p[3].clear = false;
    assert(ChoosePart(p, 4, muzzle) == 0);
    p[0].clear = false;
    assert(ChoosePart(p, 4, muzzle) == -1);
    // Same rank: the nearer one.
    Part q[2] = {{{0, 1, 8}, 1, 1, true}, {{0, 1, 6}, 2, 2, true}};
    assert(ChoosePart(q, 2, muzzle) == 1);
    assert(ZoneRank(5) < ZoneRank(3) && ZoneRank(3) < ZoneRank(0) && ZoneRank(0) < ZoneRank(10) && ZoneRank(10) < ZoneRank(42));
    std::printf("part choice ok\n");
}

void TargetChoice() {
    Config c;
    Target t[5];
    t[0] = {1, 12.0f, 100.0f, true, false, false, false};
    t[1] = {2, 8.0f, 100.0f, true, false, false, false};
    t[2] = {3, 5.0f, 100.0f, false, false, false, false};   // not in sight
    t[3] = {4, 25.0f, 100.0f, true, true, false, false};    // out of range, even going for a player
    t[4] = {5, 4.0f, 0.0f, true, false, false, false};      // dead
    assert(ChooseTarget(t, 5, 0, c) == 1);
    // The one under fire stays while it is not clearly worse.
    assert(ChooseTarget(t, 5, 1, c) == 1);
    t[0].distance = 10.5f;
    assert(ChooseTarget(t, 5, 1, c) == 0);  // 10.5 - 3 < 8
    // One going for a player wins over a closer idle one.
    t[0].distance = 12.0f;
    t[0].onFriend = true;
    assert(ChooseTarget(t, 5, 0, c) == 0);  // 12 - 6 < 8
    t[0].regenerating = true;
    assert(ChooseTarget(t, 5, 0, c) == 1);
    t[1].objective = true;
    assert(ChooseTarget(t, 5, 0, c) == -1);
    assert(ChooseTarget(t, 0, 0, c) == -1);
    std::printf("target choice ok\n");
}

void FriendlyLine() {
    Config c;
    const V3 muzzle = {0, 0.6f, 0}, enemy = {0, 1.0f, 10.0f};
    V3 friends[3] = {{0, 0, 5.0f}, {3.0f, 0, 5.0f}, {0, 0, 12.0f}};
    assert(FriendInLine(muzzle, enemy, friends, 1, c));          // right in the way
    assert(!FriendInLine(muzzle, enemy, friends + 1, 1, c));     // 3 m to the side
    assert(!FriendInLine(muzzle, enemy, friends + 2, 1, c));     // behind the enemy
    V3 hugging = {0.0f, 0.0f, -0.2f};                            // the player right behind the turret
    assert(!FriendInLine(muzzle, enemy, &hugging, 1, c));
    V3 crouched = {0.4f, 0.0f, 3.0f};                            // 0.4 m beside the line: held
    assert(FriendInLine(muzzle, enemy, &crouched, 1, c));
    V3 above = {0.0f, 3.0f, 5.0f};                               // on a walkway above the line
    assert(!FriendInLine(muzzle, enemy, &above, 1, c));
    std::printf("friendly line ok\n");
}

void Turning() {
    Config c;
    assert(Near(WrapPi(3.5f * kPi), -0.5f * kPi));
    assert(Near(YawTo({0, 0, 0}, {1, 0, 0}), 0.5f * kPi));
    assert(Near(YawTo({0, 0, 0}, {0, 0, 1}), 0.0f));
    // 360 deg/s for 100 ms is 36 degrees.
    float yaw = Turn(0.0f, kPi / 2.0f, 100, c);
    assert(Near(yaw, kPi / 5.0f));
    assert(!Aligned(yaw, kPi / 2.0f, c.aimToleranceDeg * kPi / 180.0f));
    yaw = Turn(yaw, kPi / 2.0f, 1000, c);
    assert(Near(yaw, kPi / 2.0f) && Aligned(yaw, kPi / 2.0f, 0.01f));
    // The short way round across +-pi.
    yaw = Turn(0.9f * kPi, -0.9f * kPi, 50, c);
    assert(yaw > 0.9f * kPi || yaw < -0.9f * kPi);
    c.turnDegPerSec = 0.0f;
    assert(Near(Turn(0.0f, 2.0f, 1, c), 2.0f));
    std::printf("turning ok\n");
}

void Pacing() {
    Pacer p;
    uint32_t shots = 0;
    for (uint32_t now = 1000; now <= 2000; now += 16)   // one second of 60 fps frames
        if (p.Ready(now)) { p.Fired(now, 250); ++shots; }
    assert(shots == 4 || shots == 5);
    // A long stall gives one shot, not a burst of the missed ones.
    uint32_t now = 10000;
    assert(p.Ready(now));
    p.Fired(now, 250);
    assert(!p.Ready(now + 16) && !p.Ready(now + 249) && p.Ready(now + 250));
    // Frames late by a little keep the average rate.
    p.Reset();
    shots = 0;
    for (uint32_t t = 0; t < 10000; t += 33)
        if (p.Ready(t)) { p.Fired(t, 250); ++shots; }
    assert(shots >= 39 && shots <= 41);
    // The clock wrapping around zero.
    p.Reset();
    p.Fired(0xFFFFFF00u, 250);
    assert(!p.Ready(0xFFFFFF10u) && p.Ready(0x000000FAu));
    std::printf("pacing ok (%u shots in 10 s at 30 fps)\n", shots);
}

void AmmoAndHeat() {
    Config c;
    c.ammo = 10;
    c.ammoRegenPerSec = 2.0f;
    c.heatSeconds = 0.0f;
    Magazine m;
    m.Refill(c);
    for (int i = 0; i < 10; ++i) assert(m.Fire(c) == Shot::Fired);
    assert(!m.CanFire(c) && m.Fire(c) == Shot::Empty && m.Rounds(c) == 0);
    m.Advance(500, c);                       // 1 round back
    assert(m.CanFire(c) && m.Rounds(c) == 1);
    m.Advance(60000, c);
    assert(m.Rounds(c) == 10);               // capped
    c.ammo = 0;                              // unlimited
    assert(m.Unlimited(c) && m.Rounds(c) == -1 && m.Fire(c) == Shot::Fired);

    // Heat: continuous fire overheats after heatSeconds, cooling takes coolSeconds.
    Config h;
    h.ammo = 0;
    h.heatSeconds = 6.0f;
    h.coolSeconds = 3.0f;
    h.fireIntervalMs = 250;
    Magazine g;
    g.Refill(h);
    uint32_t firedMs = 0;
    for (uint32_t t = 0; t < 20000 && !g.overheated; t += 250) {
        g.Advance(250, h);
        if (g.CanFire(h)) { g.Fire(h); firedMs = t; }
    }
    assert(g.overheated && firedMs >= 5000 && firedMs <= 6500);   // the first shot lands on a cold barrel
    assert(!g.CanFire(h) && g.Fire(h) == Shot::Overheated);
    g.Advance(2900, h);
    assert(g.overheated);
    g.Advance(200, h);
    assert(!g.overheated && g.CanFire(h));
    // Short bursts with pauses never overheat.
    Magazine b;
    b.Refill(h);
    for (int burst = 0; burst < 20; ++burst) {
        for (int i = 0; i < 8; ++i) { b.Advance(250, h); b.Fire(h); }
        b.Advance(3000, h);
    }
    assert(!b.overheated);
    std::printf("ammo and heat ok (overheated after %u ms)\n", firedMs);
}

void KeyEdge() {
    Edge e;
    assert(!e.Pressed(false) && e.Pressed(true) && !e.Pressed(true) && !e.Pressed(false) && e.Pressed(true));
    std::printf("key edge ok\n");
}

void ShotEffectChoice() {
    assert(ChooseShotFx(true, true, true) == ShotFx::PulseMuzzle);
    assert(ChooseShotFx(true, true, false) == ShotFx::SilentFlash);   // pulse rifle stream not loaded
    assert(ChooseShotFx(true, false, true) == ShotFx::SilentFlash);   // flash without the authored sound
    assert(ChooseShotFx(false, true, true) == ShotFx::None);          // the sound comes only with the muzzle effect
    assert(ChooseShotFx(false, false, true) == ShotFx::None);
    std::printf("shot effect choice ok\n");
}

void EffectRate() {
    RateLimit r;
    int allowed = 0;
    for (uint32_t t = 0; t < 1000; t += 50) allowed += r.Allow(t, 8) ? 1 : 0;
    assert(allowed == 8);
    assert(r.Allow(1000, 8));                 // a new window
    assert(!r.Allow(1100, 0));                // 0 = off
    RateLimit w;
    assert(w.Allow(0xFFFFFF00u, 1) && !w.Allow(0xFFFFFFF0u, 1) && w.Allow(0x00000300u, 1));   // the tick wraps
    r.Reset();
    assert(r.Allow(1200, 1) && !r.Allow(1300, 1));
    std::printf("effect rate ok\n");
}

void EffectBasis() {
    float m[16];
    assert(Basis({0.0f, 0.0f, 2.0f}, {1.0f, 2.0f, 3.0f}, m));
    // Facing +Z: right is +X, up is +Y, as the player's matrix at +0x90 for that facing.
    assert(Near(m[0], 1.0f) && Near(m[5], 1.0f) && Near(m[10], 1.0f) && Near(m[12], 1.0f) && Near(m[13], 2.0f) && Near(m[14], 3.0f) && m[15] == 1.0f);
    // The live player matrix: forward (-0.515, 0, -0.857) had right (-0.857, 0, 0.515).
    assert(Basis({-0.515f, 0.0f, -0.857f}, {0, 0, 0}, m) && Near(m[0], -0.857f, 2e-3f) && Near(m[2], 0.515f, 2e-3f) && Near(m[5], 1.0f, 2e-3f));
    assert(Basis({1.0f, -1.0f, 0.0f}, {0, 0, 0}, m));
    const V3 r = {m[0], m[1], m[2]}, u = {m[4], m[5], m[6]}, f = {m[8], m[9], m[10]};
    assert(Near(Length(r), 1.0f) && Near(Length(u), 1.0f) && Near(Length(f), 1.0f) && Near(Dot(r, u), 0.0f) && Near(Dot(u, f), 0.0f) && Near(Dot(r, f), 0.0f));
    assert(u.y > 0.0f);                       // aimed down, still upright
    assert(Basis({0.0f, -3.0f, 0.0f}, {0, 0, 0}, m) && Near(m[0], 1.0f));   // straight down
    assert(!Basis({0, 0, 0}, {0, 0, 0}, m) && !Basis({std::nanf(""), 0, 1}, {0, 0, 0}, m) && !Basis({0, 0, 1}, {std::nanf(""), 0, 0}, m));
    std::printf("effect basis ok\n");
}

void ImpactPlacement() {
    const V3 center = {0, 1, 0};
    assert(Near(ImpactPoint(center, {0, 1, 0.2f}, true, 0.6f).z, 0.2f));
    assert(Near(ImpactPoint(center, {0, 1, 2.0f}, true, 0.6f).z, 0.0f));   // a contact far off is not this part
    assert(Near(ImpactPoint(center, {0, 1, 0.2f}, false, 0.6f).z, 0.0f));
    assert(Near(ImpactPoint(center, {std::nanf(""), 1, 0}, true, 0.6f).x, 0.0f));
    std::printf("impact placement ok\n");
}

}  // namespace

int main() {
    Sanitizing();
    CandidateAhead();
    PlacementWithMesh();
    PlacementWithoutMesh();
    PartChoice();
    TargetChoice();
    FriendlyLine();
    Turning();
    Pacing();
    AmmoAndHeat();
    KeyEdge();
    ShotEffectChoice();
    EffectRate();
    EffectBasis();
    ImpactPlacement();
    std::printf("coop_turret tests passed\n");
    return 0;
}
