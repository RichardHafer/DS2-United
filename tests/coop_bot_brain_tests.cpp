// The companion bot's decisions, checked without a game.
#include <cassert>
#include <cmath>
#include <cstdio>
#include "../src/coop_bot_brain.h"
#include "../src/coop_bot_steer.h"
#include "../src/coop_bot_bench.h"

namespace {
using namespace coop_bot;

Perception Calm() {
    Perception p;
    p.now = 10000;
    p.self = {0, 0, 0};
    p.leader = {2, 0, 0};
    p.health01 = 1.0f;
    p.medpacks = 2;
    p.clip = 10;
    p.reserve = 20;
    return p;
}
Enemy Slasher(uint32_t id, Vec3 at) {
    Enemy e;
    e.id = id;
    e.pos = at;
    e.chest = {at.x, at.y + 1.3f, at.z};
    e.limbs[0] = {at.x + 0.4f, at.y + 1.4f, at.z};
    e.limbs[1] = {at.x - 0.4f, at.y + 1.4f, at.z};
    e.limbs[2] = {at.x + 0.2f, at.y + 0.5f, at.z};
    e.limbCount = 3;
    e.health = 60;
    e.maxHealth = 60;
    return e;
}

// Nothing going on: stays near the leader, never glued to him, walks looking
// where it goes, looks at the leader only after standing a moment, and does
// not wander off every few seconds.
void LoitersNearTheLeader() {
    Brain b;
    Perception p = Calm();
    bool lookedAtLeader = false, lookedElsewhere = false;
    uint32_t stoodSince = 0;
    int starts = 0;
    bool wasMoving = false;
    for (int i = 0; i < 1200; ++i) {   // two minutes
        p.now += 100;
        const Command c = b.Tick(p);
        assert(c.mode == Mode::Idle);
        assert(!c.fire && !c.melee && !c.aim && !c.stasis && !c.heal);
        if (c.move) {
            const float d = Flat(c.moveTo, p.leader);
            assert(d >= b.tune.idleMin - 0.01f && d <= b.tune.idleMax + 0.01f);
            assert(!c.look);   // walking: eyes on the way
            const Vec3 to = Sub(c.moveTo, p.self);
            const float len = Flat(c.moveTo, p.self);
            if (len > 0.0f) p.self = Add(p.self, Scale(to, (len < 0.3f ? len : 0.3f) / len));
            if (!wasMoving) ++starts;
            stoodSince = 0;
        } else {
            if (!stoodSince) stoodSince = p.now;
            if (c.look) assert(p.now - stoodSince + 100 >= b.tune.settleMinMs);
        }
        wasMoving = c.move;
        if (!c.look) continue;
        if (Distance(c.lookAt, Add(p.leader, {0, 1.6f, 0})) < 0.01f) lookedAtLeader = true;
        else lookedElsewhere = true;
    }
    assert(lookedAtLeader && lookedElsewhere);
    assert(starts >= 1 && starts <= 8);
}

// The leader walks off: the bot follows, and runs when far behind.
void FollowsAndRuns() {
    Brain b;
    Perception p = Calm();
    p.leader = {8, 0, 0};
    Command c = b.Tick(p);
    assert(c.mode == Mode::Follow && c.move && c.run);   // 8 m behind: it sprints
    assert(Flat(c.moveTo, p.leader) < 3.0f);
    p.self = {4.5f, 0, 0};                               // 3.5 m: still going, walking
    p.leaderMoving = true;
    p.now += 100;
    c = b.Tick(p);
    assert(c.mode == Mode::Follow && !c.run);
}

// A door the group waits at: run there, even when idle.
void RushesToProgression() {
    Brain b;
    Perception p = Calm();
    p.progression = true;
    p.progressionAt = {15, 0, 3};
    const Command c = b.Tick(p);
    assert(c.mode == Mode::Rush && c.move && c.run);
    assert(Distance(c.moveTo, p.progressionAt) < 0.01f);
}

// An enemy at a good distance: aims at a limb, fires only when on target, at the fire rate.
void ShootsLimbsOnTarget() {
    Brain b;
    Perception p = Calm();
    p.enemies[0] = Slasher(7, {0, 0, 7});
    p.enemyCount = 1;
    p.aimErrorDeg = 20.0f;
    Command c = b.Tick(p);
    assert(c.mode == Mode::Combat && c.aim && !c.fire && c.target == 7);
    bool onLimb = false;
    for (int i = 0; i < 3; ++i) if (Distance(c.lookAt, p.enemies[0].limbs[i]) < 0.01f) onLimb = true;
    assert(onLimb);
    p.aimErrorDeg = 1.0f;
    p.now += 10;
    c = b.Tick(p);
    assert(c.fire);
    p.now += 100;
    c = b.Tick(p);
    assert(!c.fire);   // not faster than the fire interval
    p.now += 500;
    c = b.Tick(p);
    assert(c.fire);
}

// Limbs are cycled: a limb that takes a while gives way to the next.
void CyclesLimbs() {
    Brain b;
    Perception p = Calm();
    p.enemies[0] = Slasher(7, {0, 0, 7});
    p.enemyCount = 1;
    const Vec3 first = b.Tick(p).lookAt;
    p.now += 3000;
    const Vec3 second = b.Tick(p).lookAt;
    assert(Distance(first, second) > 0.1f);
}

// Right on top of the bot: swing every 1.2-1.8 s without aiming, standing through
// each swing, backing off in between.
void MeleesWhenClose() {
    Brain b;
    Perception p = Calm();
    p.enemies[0] = Slasher(3, {0, 0, 1.0f});
    p.enemyCount = 1;
    int swings = 0;
    uint32_t lastSwing = 0;
    for (int i = 0; i < 120; ++i) {
        p.now += 100;
        const Command c = b.Tick(p);
        assert(!c.aim && !c.fire);
        if (c.melee || (lastSwing && p.now - lastSwing < b.tune.meleeHoldMs)) assert(!c.move);   // the swing: standing
        else assert(c.move);   // backing off
        if (c.melee) {
            if (lastSwing) assert(p.now - lastSwing >= 1200 && p.now - lastSwing <= 1900);
            lastSwing = p.now;
            ++swings;
        }
    }
    assert(swings >= 6 && swings <= 10);
}

// Backing off never takes it far from the leader.
void RetreatStaysOnTheLeash() {
    Brain b;
    Perception p = Calm();
    p.self = {0, 0, 9};
    p.leader = {0, 0, 0};
    p.enemies[0] = Slasher(3, {0, 0, 6.5f});   // inside the keep distance, outside melee
    p.enemyCount = 1;
    const Command c = b.Tick(p);
    assert(c.move);
    assert(Flat(c.moveTo, p.leader) <= b.tune.leash + 0.01f);
}

// A big one: stasis first, once on target, then not again during the cooldown.
void StasisOnDangerous() {
    Brain b;
    Perception p = Calm();
    Enemy brute = Slasher(9, {0, 0, 8});
    brute.maxHealth = 800;
    brute.health = 800;
    p.enemies[0] = brute;
    p.enemyCount = 1;
    p.aimErrorDeg = 2.0f;
    Command c = b.Tick(p);
    assert(c.stasis && c.aim);
    p.enemies[0].stasised = true;
    p.now += 500;
    c = b.Tick(p);
    assert(!c.stasis && c.aim);
    p.enemies[0].stasised = false;
    p.now += 1000;
    c = b.Tick(p);
    assert(!c.stasis);   // cooldown
    p.stasis01 = 0.1f;
    p.now += 9000;
    c = b.Tick(p);
    assert(!c.stasis);   // no energy
}

// Empty clip: reload instead of firing.
void ReloadsWhenEmpty() {
    Brain b;
    Perception p = Calm();
    p.enemies[0] = Slasher(7, {0, 0, 7});
    p.enemyCount = 1;
    p.clip = 0;
    p.aimErrorDeg = 1.0f;
    Command c = b.Tick(p);
    assert(c.reload && !c.fire);
    p.now += 200;
    p.reloading = true;
    c = b.Tick(p);
    assert(!c.reload && !c.fire);
}

// Yellow health: heal, unless something is on top of it; not twice in a row.
void HealsAtSixtyPercent() {
    Brain b;
    Perception p = Calm();
    p.health01 = 0.55f;
    Command c = b.Tick(p);
    assert(c.heal);
    p.now += 500;
    c = b.Tick(p);
    assert(!c.heal);
    p.medpacks = 0;
    p.now += 10000;
    c = b.Tick(p);
    assert(!c.heal);
    p.medpacks = 1;
    p.enemies[0] = Slasher(3, {0, 0, 1.0f});
    p.enemyCount = 1;
    c = b.Tick(p);
    assert(!c.heal && c.mode == Mode::Combat);
}

// Items near the group are picked up; far ones are left.
void PicksUpNearbyItems() {
    Brain b;
    Perception p = Calm();
    p.items[0] = {42, {5, 0, -3}};
    p.items[1] = {43, {30, 0, 0}};
    p.itemCount = 2;
    Command c = b.Tick(p);
    assert(c.mode == Mode::Pickup && c.move && Distance(c.moveTo, p.items[0].pos) < 0.01f);
    p.self = p.items[0].pos;
    p.pickupInReach = true;
    p.now += 400;
    c = b.Tick(p);
    assert(c.use);
    p.itemCount = 0;
    p.items[0] = {43, {30, 0, 0}};
    p.itemCount = 1;
    c = b.Tick(p);
    assert(c.mode != Mode::Pickup);
}

// An item it cannot take (full, not its own): a few tries in reach, then it is
// left alone, and the bot does not keep walking at it. Nothing while the leader walks.
void GivesUpOnItems() {
    Brain b;
    Perception p = Calm();
    p.items[0] = {42, {5, 0, -3}};
    p.itemCount = 1;
    p.leaderMoving = true;
    Command c = b.Tick(p);
    assert(c.mode != Mode::Pickup);
    p.leaderMoving = false;
    p.now += 100;
    c = b.Tick(p);
    assert(c.mode == Mode::Pickup && c.move && !c.look);   // walking: eyes on the way
    p.self = p.items[0].pos;
    p.pickupInReach = true;
    int uses = 0;
    for (int i = 0; i < 20; ++i) {
        p.now += 200;
        c = b.Tick(p);
        if (c.use) ++uses;
    }
    assert(uses == static_cast<int>(b.tune.itemTries));
    assert(c.mode != Mode::Pickup);
    p.now += 30000;
    assert(b.Tick(p).mode != Mode::Pickup);   // still left alone
    p.now += 40000;
    assert(b.Tick(p).mode == Mode::Pickup);   // after a minute it may try again
}

// Held by an enemy: mash, nothing else.
void MashesWhenGrabbed() {
    Brain b;
    Perception p = Calm();
    p.grabbed = true;
    const Command c = b.Tick(p);
    assert(c.mash && !c.fire && !c.move);
}

// An enemy out of sight is not shot at through a wall.
void IgnoresInvisibleEnemies() {
    Brain b;
    Perception p = Calm();
    p.enemies[0] = Slasher(7, {0, 0, 7});
    p.enemies[0].visible = false;
    p.enemyCount = 1;
    const Command c = b.Tick(p);
    assert(!c.fire && !c.holdFire && c.target == 0);
}

// A path is walked point by point; a reached point gives way to the next.
void WalksThePath() {
    Steer st;
    Trail trail;
    const Vec3 path[] = {{0, 0, 0}, {5, 0, 0}, {5, 0, 5}, {10, 0, 5}};
    uint32_t now = 1000;
    SteerOutput o = st.Tick({0, 0, 0}, {10, 0, 5}, true, {10, 0, 5}, trail, now);
    assert(o.askPath);
    st.SetPath(path, 4, now);
    o = st.Tick({0, 0, 0}, {10, 0, 5}, true, {10, 0, 5}, trail, now += 50);
    assert(o.route == Route::Nav && Distance(o.toward, path[1]) < 0.01f);
    o = st.Tick({4.6f, 0, 0}, {10, 0, 5}, true, {10, 0, 5}, trail, now += 50);
    assert(Distance(o.toward, path[2]) < 0.01f);
    o = st.Tick({5, 0, 4.5f}, {10, 0, 5}, true, {10, 0, 5}, trail, now += 50);
    assert(Distance(o.toward, path[3]) < 0.01f);
}

// No path for a while: the leader's trail, from where the bot stands onwards.
void ShadowsWithoutAPath() {
    Steer st;
    Trail trail;
    for (int i = 0; i < 20; ++i) trail.Note({static_cast<float>(i), 0, 0}, 100u * i);
    uint32_t now = 5000;
    SteerOutput o = st.Tick({3.1f, 0, 0}, {19, 0, 0}, true, {19, 0, 0}, trail, now);
    st.NoPath(now);
    o = st.Tick({3.1f, 0, 0}, {19, 0, 0}, true, {19, 0, 0}, trail, now += 100);
    assert(o.route == Route::Direct);   // not yet
    o = st.Tick({3.2f, 0, 0}, {19, 0, 0}, true, {19, 0, 0}, trail, now += 1600);
    assert(o.route == Route::Shadow);
    assert(o.toward.x > 3.2f && o.toward.x < 5.0f);
}

// The trail is spaced by distance: standing still adds nothing.
void TrailSpacing() {
    Trail t;
    for (int i = 0; i < 50; ++i) t.Note({0, 0, 0.01f * i}, 10u * i);
    assert(t.count == 1);
    t.Note({0, 0, 2}, 1000);
    assert(t.count == 2);
}

// Stuck for good, or far behind: teleport.
void TeleportsWhenHopeless() {
    Steer st;
    Trail trail;
    uint32_t now = 1000;
    SteerOutput o;
    for (int i = 0; i < 200; ++i) {
        o = st.Tick({0, 0, 0}, {20, 0, 0}, true, {20, 0, 0}, trail, now += 50);
        if (o.teleport) break;
    }
    assert(o.teleport);
    assert(now - 1000 >= st.giveUpMs - 100);   // getting nowhere, 20 m from the leader
    Steer far;
    o = far.Tick({0, 0, 0}, {60, 0, 0}, true, {60, 0, 0}, trail, 1000);
    assert(o.teleport);
}

// Nothing to do: no movement, no teleport.
void IdleSteer() {
    Steer st;
    Trail trail;
    const SteerOutput o = st.Tick({0, 0, 0}, {1, 0, 0}, false, {1, 0, 0}, trail, 1000);
    assert(!o.move && !o.teleport);
}
}  // namespace

// A goal behind a wall: the mesh path ends short of it. Standing at that end
// counts as arrived, and nothing walks on at the wall.
void StopsShortOfAWall() {
    Steer s;
    Trail trail;
    const Vec3 goal = {3, 0, 0}, leader = {0, 0, 1};
    SteerOutput o = s.Tick({0, 0, 0}, goal, true, leader, trail, 1000);
    assert(o.askPath && !o.blocked);
    const Vec3 path[] = {{0, 0, 0}, {1.5f, 0, 0}};   // the wall stands at x = 1.6
    s.SetPath(path, 2, 1000);
    o = s.Tick({0, 0, 0}, goal, true, leader, trail, 1100);
    assert(o.move && o.route == Route::Nav && Distance(o.toward, path[1]) < 0.01f);   // along the mesh, not straight at the goal
    o = s.Tick({1.2f, 0, 0}, goal, true, leader, trail, 1400);
    assert(!o.move && o.blocked && Distance(o.reachable, path[1]) < 0.01f);
}

// Weapons ------------------------------------------------------------------------

Perception Armed(Gun a, Gun b = Gun::None, Gun c = Gun::None, Gun d = Gun::None) {
    Perception p = Calm();
    const Gun kinds[4] = {a, b, c, d};
    for (int i = 0; i < 4; ++i) { p.guns[i].kind = kinds[i]; p.guns[i].clip = kinds[i] == Gun::None ? -1 : 5; p.guns[i].reserve = 20; }
    p.activeSlot = 0;
    p.cutterHorizontal = 0;
    p.aimErrorDeg = 0.0f;
    return p;
}
Enemy LegsOnly(uint32_t id, Vec3 at) {
    Enemy e = Slasher(id, at);
    e.limbs[0] = {at.x + 0.2f, at.y + 0.5f, at.z};
    e.limbs[1] = {at.x - 0.2f, at.y + 0.5f, at.z};
    e.limbKind[0] = e.limbKind[1] = LimbKind::Leg;
    e.limbCount = 2;
    return e;
}

// The cutter turns its blade across the limb: horizontal for a leg, and only
// fires once the blade stands right.
void CutterTurnsForLegs() {
    Brain b;
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = LegsOnly(7, {0, 0, 6});
    p.enemyCount = 1;
    Command c = b.Tick(p);
    assert(c.mode == Mode::Combat && c.aim && c.altFire && !c.fire);
    p.now += 100;
    c = b.Tick(p);
    assert(!c.altFire);                 // no second turn right away
    p.cutterHorizontal = 1;
    p.cutterTurning = true;             // still on its way round: no shot yet
    p.now += 800;
    c = b.Tick(p);
    assert(!c.fire && !c.altFire);
    p.cutterTurning = false;
    p.now += 100;
    c = b.Tick(p);
    assert(c.fire && !c.altFire);
}

// Two in a row: the line gun, taken first, then fired low between the legs.
void LineGunThroughARow() {
    Brain b;
    Perception p = Armed(Gun::PlasmaCutter, Gun::LineGun);
    p.self = {0, 0, 0};
    p.leader = {0, 0, -2};
    p.enemies[0] = LegsOnly(7, {0, 0, 4});
    p.enemies[1] = LegsOnly(8, {0.3f, 0, 8.5f});
    p.enemyCount = 2;
    Command c = b.Tick(p);
    assert(c.slot == 1 && !c.fire);
    p.activeSlot = 1;
    p.now += 100;
    c = b.Tick(p);
    assert(c.fire && !c.altFire);
    assert(std::fabs(c.lookAt.y - 0.5f) < 0.01f && std::fabs(c.lookAt.x) < 0.01f);   // between the legs
}

// A pack coming on: a mine where it will be, then a pause before the next.
void MineLeadsThePack() {
    Brain b;
    Perception p = Armed(Gun::LineGun);
    p.self = {0, 0, 0};
    p.leader = {0, 0, -2};
    for (int i = 0; i < 3; ++i) {
        p.enemies[i] = Slasher(10 + i, {static_cast<float>(i) - 1.0f, 0, 10});
        p.enemies[i].vel = {0, 0, -1.0f};   // walking at the bot
    }
    p.enemyCount = 3;
    Command c = b.Tick(p);
    assert(c.altFire && !c.fire);
    assert(c.lookAt.z < 10.0f && c.lookAt.z > 10.0f - b.tune.mineLeadMax - 0.01f);   // ahead of them, not too far
    p.now += 100;
    c = b.Tick(p);
    assert(!c.altFire);
}

// A spear in the group: the shock once per hit, inside the window.
void JavelinShocksOnce() {
    Brain b;
    Perception p = Armed(Gun::PlasmaCutter, Gun::None, Gun::Javelin);
    p.self = {0, 0, 0};
    p.leader = {0, 0, -2};
    p.enemies[0] = Slasher(20, {0, 0, 7});
    p.enemies[1] = Slasher(21, {1.5f, 0, 7.5f});
    p.enemyCount = 2;
    Command c = b.Tick(p);
    assert(c.slot == 2);
    p.activeSlot = 2;
    p.now += 100;
    c = b.Tick(p);
    assert(c.fire);
    p.spearHitAt = p.now + 50;
    p.now += 300;
    c = b.Tick(p);
    assert(c.altFire);
    p.now += 100;
    c = b.Tick(p);
    assert(!c.altFire);
    p.spearHitAt = 0;
    p.now += 5000;
    assert(!b.Tick(p).altFire);
}

// The Ubermorph cannot die: shot at until three of its four limbs are off, then
// left alone until they grow back.
void LeavesACrippledUbermorph() {
    Brain b;
    Perception p = Calm();
    Enemy u = Slasher(99, {0, 0, 6});
    u.maxHealth = 100000.0f;
    u.health = 100000.0f;
    u.limbsLost = 2;
    p.enemies[0] = u;
    p.enemyCount = 1;
    assert(b.Tick(p).mode == Mode::Combat);
    p.enemies[0].limbsLost = 3;
    p.now += 100;
    assert(b.Tick(p).mode != Mode::Combat);
    p.enemies[0].limbsLost = 1;   // regrown
    p.now += 100;
    assert(b.Tick(p).mode == Mode::Combat);
}

// Left far behind in a fight: beyond 22 m gun down, run to the leader (on until 16 m);
// nearer it comes back aimed; close again it fights where it stands.
void RunsBackWhenFarBehind() {
    Brain b;
    Perception p = Calm();
    p.leader = {-30, 0, 0};
    p.enemies[0] = Slasher(96, {0, 0, 7});
    p.enemyCount = 1;
    p.aimErrorDeg = 0.5f;
    for (int i = 0; i < 10; ++i) {
        p.now += 150;
        const Command c = b.Tick(p);
        assert(c.mode == Mode::Combat && c.move && c.run && !c.aim && !c.fire);
    }
    // Still 18 m off: keeps running (16 m ends it).
    p.self = {-12, 0, 0};
    p.now += 150;
    assert(b.Tick(p).run);
    // 13 m off: walks back with the gun up.
    p.self = {-17, 0, 0};
    bool aimed = false;
    for (int i = 0; i < 10 && !aimed; ++i) {
        p.now += 150;
        const Command c = b.Tick(p);
        assert(!c.run && c.move);
        aimed = c.aim;
    }
    assert(aimed);
}

// An Ubermorph down and regrowing its limbs is no target at all (user 20.10.: the
// bots aimed at it all through its regrowth): not aimed at, shot, slowed, kicked or
// swung at; something killable comes first meanwhile; once it stands again it is
// shot as before.
void HoldsFireWhileItRegrows() {
    Brain b;
    Perception p = Calm();
    Enemy u = Slasher(98, {0, 0, 6});
    u.maxHealth = u.health = 100000.0f;
    u.limbsLost = 1;
    u.regenerating = true;
    p.enemies[0] = u;
    p.enemyCount = 1;
    p.aimErrorDeg = 0.5f;
    for (int i = 0; i < 20; ++i) {
        p.now += 150;
        const Command c = b.Tick(p);
        assert(c.target != 98 && !c.aim);
        assert(!c.fire && !c.holdFire && !c.altFire && !c.holdAlt && !c.melee && !c.stasis && !c.stomp);
    }
    // Close by it is not swung at either.
    p.enemies[0].pos = {0, 0, 1.0f};
    p.enemies[0].chest = {0, 1.3f, 1.0f};
    for (int i = 0; i < 20; ++i) {
        p.now += 150;
        const Command c = b.Tick(p);
        assert(c.target != 98 && !c.fire && !c.melee && !c.stomp && !c.stasis);
    }
    p.enemies[0].pos = {0, 0, 6};
    p.enemies[0].chest = {0, 1.3f, 6};
    // A slasher about: that one first.
    p.enemies[1] = Slasher(97, {3, 0, 8});
    p.enemyCount = 2;
    Brain other;
    p.now += 150;
    assert(other.Tick(p).target == 97);
    // Up again: shot.
    p.enemyCount = 1;
    p.enemies[0].regenerating = false;
    bool fired = false;
    for (int i = 0; i < 20 && !fired; ++i) {
        p.now += 150;
        fired = b.Tick(p).fire;
    }
    assert(fired);
}

// Far behind the leader in a fight, even with an enemy on the bot: beyond 22 m it runs
// with the gun down (on until 16 m); nearer it comes back aimed and shooting, stepping to
// the sides; held by an enemy, it fights (20.10.).
void BreaksAwayToTheLeader() {
    Brain b;
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, 30};
    p.enemies[0] = Slasher(40, {0, 0, 1.0f});
    p.enemyCount = 1;
    for (int i = 0; i < 6; ++i) {
        p.now += 150;
        const Command c = b.Tick(p);
        assert(c.move && c.run && Flat(c.moveTo, p.leader) < 0.5f && !c.melee && !c.fire && !c.stomp);
    }
    // 18 m off: still running (the run ends at 16 m)
    p.self = {0, 0, 12};
    p.enemies[0].pos = {0, 0, 13};
    p.enemies[0].chest = {0, 1.3f, 13};
    p.now += 150;
    assert(b.Tick(p).run);
    // 13 m off: back on foot with the gun up, toward him
    p.self = {0, 0, 17};
    p.enemies[0].pos = {0, 0, 14};
    p.enemies[0].chest = {0, 1.3f, 14};
    p.now += 150;
    const Command k = b.Tick(p);
    assert(k.move && !k.run && k.aim && k.moveTo.z > p.self.z);
    // held: no running off
    Brain held;
    Perception q = Armed(Gun::PlasmaCutter);
    q.leader = {0, 0, 30};
    q.grabbed = true;
    q.enemies[0] = Slasher(41, {0, 0, 1.0f});
    q.enemyCount = 1;
    q.now += 150;
    assert(!held.Tick(q).run);
    std::printf("break away ok\n");
}

// 15 m behind with a slasher 5 m off: it walks back aimed, shoots on the way and changes
// sides now and then.
void KitesBackShooting() {
    Brain b;
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, 15};
    p.enemies[0] = Slasher(42, {0, 0, -5});
    p.enemyCount = 1;
    int shots = 0, left = 0, right = 0, runs = 0;
    for (int i = 0; i < 40; ++i) {
        p.now += 100;
        const Command c = b.Tick(p);
        assert(c.move && c.aim);
        if (c.run) ++runs;
        if (c.fire) ++shots;
        if (c.moveTo.x < p.self.x - 0.5f) ++left;
        if (c.moveTo.x > p.self.x + 0.5f) ++right;
        assert(c.moveTo.z > p.self.z);   // always toward him
    }
    assert(runs == 0 && shots >= 1 && left >= 1 && right >= 1);
    std::printf("kites back ok (shots %d, left %d, right %d)\n", shots, left, right);
}

// The pulse rifle in bursts: the trigger held a while, then let go.
void PulseInBursts() {
    Brain b;
    Perception p = Armed(Gun::PulseRifle);
    p.enemies[0] = Slasher(30, {0, 0, 7});
    p.enemyCount = 1;
    int held = 0, released = 0;
    for (int i = 0; i < 24; ++i) {
        const Command c = b.Tick(p);
        if (c.holdFire) ++held; else ++released;
        p.now += 50;
    }
    assert(held > 0 && released > 0);
}

// A pack at range: a grenade, lobbed, only with 25 rounds in the clip.
void GrenadeNeedsTwentyFive() {
    Brain b;
    Perception p = Armed(Gun::PulseRifle);
    p.self = {0, 0, 0};
    p.leader = {0, 0, -2};
    p.enemies[0] = Slasher(40, {0, 0, 10});
    p.enemies[1] = Slasher(41, {1, 0, 10.5f});
    p.enemyCount = 2;
    p.guns[0].clip = 20;
    Command c = b.Tick(p);
    assert(!c.altFire);
    Brain b2;
    p.guns[0].clip = 30;
    c = b2.Tick(p);
    assert(c.altFire);
    assert(c.lookAt.y > 0.3f);   // raised for the arc
}

// The ripper throws blades (alt fire) at a single enemy, one every 1.3 s.
void RipperThrows() {
    Brain b;
    Perception p = Armed(Gun::Ripper);
    p.enemies[0] = Slasher(50, {0, 0, 6});
    p.enemyCount = 1;
    Command c = b.Tick(p);
    assert(c.altFire && !c.fire);
    p.now += 500;
    assert(!b.Tick(p).altFire);
    p.now += 900;
    assert(b.Tick(p).altFire);
}

// The seeker: the scope on first, then the shot.
void SeekerZoomsFirst() {
    Brain b;
    Perception p = Armed(Gun::SeekerRifle);
    Enemy big = Slasher(60, {0, 0, 15});
    big.maxHealth = 400.0f;
    big.health = 400.0f;
    p.enemies[0] = big;
    p.enemyCount = 1;
    p.stasis01 = 0.0f;   // no stasis in the way
    p.seekerZoomed = 0;
    Command c = b.Tick(p);
    assert(c.altFire && !c.fire);
    p.seekerZoomed = 1;
    p.now += 1100;
    c = b.Tick(p);
    assert(c.fire && !c.altFire);
}

// Detonator: only for a group walking at the bot, and only while the level has few mines.
void DetonatorInTheirWay() {
    Perception p = Armed(Gun::Detonator);
    p.self = {0, 0, 0};
    p.leader = {0, 0, -2};
    p.enemies[0] = Slasher(70, {0, 0, 11});
    p.enemies[1] = Slasher(71, {1, 0, 11.5f});
    p.enemies[0].vel = p.enemies[1].vel = {0, 0, -1.5f};
    p.enemyCount = 2;
    p.detonatorMines = 4;
    Brain crowded;
    assert(!crowded.Tick(p).fire);
    p.detonatorMines = 0;
    Brain b;
    const Command c = b.Tick(p);
    assert(c.fire);
    assert(c.lookAt.z > 3.0f && c.lookAt.z < 11.0f && std::fabs(c.lookAt.y) < 0.2f);   // on the floor, between them and the bot
}

// Desperate: buried in four slashers with no human near, the pulse rifle's grenade goes
// at its own feet and the detonator into the one in front, mates near or not; with the
// leader close it keeps to the usual rules (user 20.10.).
void DesperateBlasts() {
    Perception p = Armed(Gun::PulseRifle);
    p.guns[0].clip = 30;
    p.self = {0, 0, 0};
    p.leader = {0, 0, -15};
    p.enemies[0] = Slasher(90, {0, 0, 1.5f});
    p.enemies[1] = Slasher(91, {1.5f, 0, 0});
    p.enemies[2] = Slasher(92, {-1.5f, 0, 0.5f});
    p.enemies[3] = Slasher(93, {0.5f, 0, -1.5f});
    p.enemyCount = 4;
    p.mates[0].pos = {1.0f, 0, 1.0f};   // a mate right in it: no reason to hold
    p.mateCount = 1;
    Brain b;
    bool grenade = false;
    for (int i = 0; i < 6 && !grenade; ++i) {
        p.now += 150;
        const Command c = b.Tick(p);
        if (c.altFire) {
            grenade = true;
            assert(c.desperate);
            // the floor just ahead, within the aim's pitch range (0.8 m was past it, 24.10.)
            assert(Flat(c.lookAt, p.self) > 1.2f && Flat(c.lookAt, p.self) < 2.5f && std::fabs(c.lookAt.y) < 0.2f);
        }
    }
    assert(grenade);
    // the leader within 6 m: no grenade at its feet
    Perception q = p;
    q.leader = {0, 0, -4};
    Brain held;
    for (int i = 0; i < 6; ++i) {
        q.now += 150;
        const Command c = held.Tick(q);
        assert(!c.desperate);
        assert(!(c.altFire && Flat(c.lookAt, q.self) < 2.5f));
    }
    // the detonator into the one in front
    Perception d = Armed(Gun::Detonator);
    d.self = p.self;
    d.leader = p.leader;
    for (int i = 0; i < 4; ++i) d.enemies[i] = p.enemies[i];
    d.enemyCount = 4;
    d.detonatorMines = 0;
    Brain det;
    bool mine = false;
    for (int i = 0; i < 6 && !mine; ++i) {
        d.now += 150;
        const Command c = det.Tick(d);
        if (c.fire) { mine = true; assert(c.desperate && Flat(c.lookAt, d.self) < 2.5f); }
    }
    assert(mine);
    std::printf("desperate ok\n");
}

// Surrounded with a contact beam in hand and a force gun in the next slot: it slams and
// charges with what it holds instead of swapping guns for slam and push in turn (20.10.).
void NoGunSwapsInACrowd() {
    Perception p = Armed(Gun::ContactBeam, Gun::ForceGun);
    p.enemies[0] = Slasher(95, {0, 0, 1.2f});
    p.enemies[1] = Slasher(96, {1.2f, 0, 0.3f});
    p.enemies[2] = Slasher(97, {-1.2f, 0, 0.3f});
    p.enemyCount = 3;
    Brain b;
    int swaps = 0, slams = 0;
    for (int i = 0; i < 60; ++i) {
        p.now += 150;
        const Command c = b.Tick(p);
        if (c.slot >= 0 && c.slot != p.activeSlot) { ++swaps; p.activeSlot = c.slot; }
        if (c.altFire && p.activeSlot == 0) ++slams;
    }
    assert(swaps <= 1);
    assert(slams >= 1);
    std::printf("crowd keeps its gun ok (swaps %d, slams %d)\n", swaps, slams);
}

// The contact beam: the trigger held until the gun says full and the aim sits,
// then let go (the beam leaves on release).
void ContactChargesAndReleases() {
    Brain b;
    Perception p = Armed(Gun::ContactBeam);
    Enemy big = Slasher(80, {0, 0, 8});
    big.maxHealth = big.health = 400.0f;
    p.enemies[0] = big;
    p.enemyCount = 1;
    p.stasis01 = 0.0f;
    p.contactReady = 0;
    Command c = b.Tick(p);
    assert(c.holdFire);
    p.now += 400;
    assert(b.Tick(p).holdFire);          // still charging
    p.contactReady = 1;
    p.aimErrorDeg = 5.0f;                // full, but off target: keep holding
    p.now += 100;
    assert(b.Tick(p).holdFire);
    p.aimErrorDeg = 0.5f;                // on target: let go
    p.now += 100;
    assert(!b.Tick(p).holdFire);
}

// The force gun's focused shot: one press, then the aim stays on the target
// through the wind-up (the plan does not change under it).
void ForceBeamHoldsTheAim() {
    Brain b;
    Perception p = Armed(Gun::ForceGun);
    Enemy big = Slasher(81, {0, 0, 9});
    big.maxHealth = big.health = 400.0f;
    p.enemies[0] = big;
    p.enemyCount = 1;
    p.stasis01 = 0.0f;
    Command c = b.Tick(p);
    assert(c.altFire);
    p.now += 300;
    c = b.Tick(p);
    assert(!c.altFire && c.aim && Distance(c.lookAt, p.enemies[0].chest) < 0.01f);
}

// The flame canister costs 25 fuel: not with less.
void FlameCanisterNeedsFuel() {
    Perception p = Armed(Gun::Flamethrower);
    p.enemies[0] = Slasher(82, {0, 0, 6});
    p.enemies[1] = Slasher(83, {1, 0, 6.5f});
    p.enemyCount = 2;
    p.guns[0].clip = 10;
    Brain lean;
    assert(!lean.Tick(p).altFire);
    p.guns[0].clip = 40;
    Brain b;
    assert(b.Tick(p).altFire);
}

// Surrounded by three: the contact beam's slam.
void SlamsWhenSurrounded() {
    Brain b;
    Perception p = Armed(Gun::PlasmaCutter, Gun::ContactBeam);
    p.activeSlot = 1;
    p.enemies[0] = Slasher(90, {1.5f, 0, 0});
    p.enemies[1] = Slasher(91, {-1.5f, 0, 0.5f});
    p.enemies[2] = Slasher(92, {0, 0, 2.0f});
    p.enemyCount = 3;
    p.stasis01 = 0.0f;
    const Command c = b.Tick(p);
    assert(c.altFire && !c.melee);
}

// A player next to the enemy: no grenade and no stasis there.
void NoBurstsNearPlayers() {
    Perception p = Armed(Gun::PulseRifle);
    p.self = {0, 0, 0};
    p.leader = {0, 0, -2};
    p.enemies[0] = Slasher(100, {0, 0, 10});
    p.enemies[1] = Slasher(101, {1, 0, 10.5f});
    p.enemyCount = 2;
    p.guns[0].clip = 30;               // enough for a grenade
    p.players[0] = {1.5f, 0, 9.0f};   // a partner right by them
    p.playerCount = 1;
    Brain b;
    assert(!b.Tick(p).altFire);
    p.playerCount = 0;
    Brain clear;
    assert(clear.Tick(p).altFire);
    // Stasis on a big one, only with nobody beside it.
    Perception q = Calm();
    Enemy big = Slasher(102, {0, 0, 6});
    big.maxHealth = big.health = 400.0f;
    q.enemies[0] = big;
    q.enemyCount = 1;
    q.aimErrorDeg = 0.0f;
    q.leader = {0.5f, 0, 6.5f};       // the leader next to it
    Brain s1;
    assert(!s1.Tick(q).stasis);
    q.leader = {0, 0, -3};
    Brain s2;
    assert(s2.Tick(q).stasis);
}

// In a fight it stays by the leader: too far from him it walks back to his side
// (shooting on the way), close by it holds; it does not chase a far target.
void FightsFromTheLeadersSide() {
    Brain b;
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, 0};
    p.self = {0, 0, -8};            // 8 m behind the leader
    p.enemies[0] = Slasher(110, {0, 0, 9});
    p.enemyCount = 1;
    // After a moment to react (a step out of the leader's line may come first).
    Command c = b.Tick(p);
    assert(c.mode == Mode::Combat && c.aim);
    for (int i = 0; i < 15 && !(c.move && Flat(c.moveTo, p.leader) <= b.tune.combatSide + b.tune.combatBack + 0.5f); ++i) { p.now += 100; c = b.Tick(p); }
    assert(c.move && c.aim);
    assert(Flat(c.moveTo, p.leader) <= b.tune.combatSide + b.tune.combatBack + 0.5f);   // beside him
    assert(Flat(c.moveTo, p.enemies[0].pos) > Flat(p.leader, p.enemies[0].pos) - 0.5f);  // not in front of him
    // At its place: it stands and shoots, even with the target out at 9 m.
    Brain b2;
    p.self = {2.2f, 0, -1.0f};
    p.now += 100;
    c = b2.Tick(p);
    assert(!c.move && c.aim);
}

// The way back to the leader leads past an enemy: it holds and fights.
void HoldsWhenTheWayIsDangerous() {
    Brain b;
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, 0};
    p.self = {0, 0, 10};
    p.enemies[0] = Slasher(111, {0, 0, 5});   // right between the bot and the leader, on the way back
    p.enemyCount = 1;
    const Command c = b.Tick(p);
    assert(!c.move && c.aim);
}

// A lurker at its feet: stomped where the bot stands, no swing over it, no
// backing off. The same one up on a wall is shot instead.
void StompsSmallOneUnderfoot() {
    Brain b;
    Perception p = Calm();
    Enemy lurker = Slasher(40, {0, -0.4f, 0.95f});
    lurker.limbCount = 0;
    lurker.height = 0.7f;
    lurker.chest = {0, -0.1f, 0.95f};
    p.enemies[0] = lurker;
    p.enemyCount = 1;
    int stomps = 0;
    for (int i = 0; i < 50; ++i) {
        p.now += 100;
        const Command c = b.Tick(p);
        assert(!c.melee && !c.fire && !c.aim && !c.move);
        if (c.stomp) ++stomps;
    }
    assert(stomps >= 5 && stomps <= 6);
    Brain wall;
    p.enemies[0].pos = {0, 2.0f, 1.2f};
    p.enemies[0].chest = {0, 2.3f, 1.2f};
    p.aimErrorDeg = 0.0f;
    bool fired = false;
    for (int i = 0; i < 30; ++i) {
        p.now += 100;
        const Command c = wall.Tick(p);
        assert(!c.stomp);
        fired = fired || c.fire || c.melee;
    }
    assert(fired);
}

// --- the squad ---
Mate MateAt(uint32_t id, Vec3 at, uint32_t target = 0) {
    Mate m;
    m.id = id;
    m.pos = at;
    m.goal = at;
    m.target = target;
    return m;
}

// How many bots one enemy gets depends on its kind and on what else is there.
uint32_t TargetAfter(Perception p) {
    Brain b;
    uint32_t target = 0;
    for (int i = 0; i < 12; ++i) { p.now += 150; target = b.Tick(p).target; }
    return target;
}
void FocusDependsOnKindAndCount() {
    Perception p = Calm();
    p.aimErrorDeg = 0.5f;
    // Two slashers, both taken by a mate: the third bot still shoots, it does not stand idle.
    p.enemies[0] = Slasher(81, {0, 0, 6});
    p.enemies[1] = Slasher(82, {3, 0, 7});
    p.enemies[0].kind = p.enemies[1].kind = EnemyKind::Slasher;
    p.enemyCount = 2;
    p.mates[0] = MateAt(900, {-3, 0, 0}, 81);
    p.mates[1] = MateAt(901, {-4, 0, 1}, 82);
    p.mateCount = 2;
    const uint32_t t = TargetAfter(p);
    assert(t == 81 || t == 82);
    // Three pack boys, one taken: a free one, one bot each.
    p.enemies[0] = Slasher(71, {0, 0, 5});
    p.enemies[1] = Slasher(72, {2, 0, 8});
    p.enemies[2] = Slasher(73, {-2, 0, 8});
    for (int i = 0; i < 3; ++i) p.enemies[i].kind = EnemyKind::Pack;
    p.enemyCount = 3;
    p.mates[0] = MateAt(900, {-3, 0, 0}, 71);
    p.mateCount = 1;
    const uint32_t free = TargetAfter(p);
    assert(free == 72 || free == 73);
    // A slasher taken by both mates and a free one: the free one, even if farther.
    p.enemies[0] = Slasher(61, {0, 0, 4});
    p.enemies[1] = Slasher(62, {6, 0, 12});
    p.enemies[0].kind = p.enemies[1].kind = EnemyKind::Slasher;
    p.enemyCount = 2;
    p.mates[0] = MateAt(900, {-3, 0, 0}, 61);
    p.mateCount = 1;
    assert(TargetAfter(p) == 62);
    // The Ubermorph alone with two bots on it: the third joins in.
    Enemy u = Slasher(91, {0, 0, 6});
    u.kind = EnemyKind::Hunter;
    u.maxHealth = u.health = 100000.0f;
    u.limbsLost = 0;
    p.enemies[0] = u;
    p.enemyCount = 1;
    p.mates[0] = MateAt(900, {-3, 0, 0}, 91);
    p.mates[1] = MateAt(901, {-4, 0, 1}, 91);
    p.mateCount = 2;
    assert(TargetAfter(p) == 91);
}


// Two about as close: the one a mate already fights is left to it.
void SpreadsOverTargets() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(50, {0, 0, 6});
    p.enemies[1] = Slasher(51, {-1, 0, 6.5f});
    p.enemyCount = 2;
    Brain alone;
    assert(alone.Tick(p).target == 50);
    p.squadSize = 2;
    p.squadIndex = 1;
    p.mates[0] = MateAt(900, {-3, 0, 0}, 50);
    p.mateCount = 1;
    Brain b;
    assert(b.Tick(p).target == 51);
}

// A big one is not ganged up on: a mate has it, so this bot takes the other.
void FocusesTheBigOne() {
    Perception p = Armed(Gun::PlasmaCutter);
    Enemy big = Slasher(60, {0, 0, 7});
    big.maxHealth = big.health = 400.0f;
    p.enemies[0] = big;
    p.enemies[1] = Slasher(61, {-1, 0, 6.5f});
    p.enemyCount = 2;
    p.stasis01 = 0.0f;
    Brain alone;
    assert(alone.Tick(p).target == 61);
    p.squadSize = 2;
    p.mates[0] = MateAt(900, {-3, 0, 0}, 60);
    p.mateCount = 1;
    Brain b;
    assert(b.Tick(p).target == 61);
    // A lone enemy is still everyone's.
    p.enemyCount = 1;
    Brain one;
    assert(one.Tick(p).target == 60);
}

// One going for a mate close to it comes first.
void CoversAMate() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(70, {0, 0, 5});
    Enemy hunter = Slasher(71, {-6, 0, 3});
    p.enemies[1] = hunter;
    p.enemyCount = 2;
    p.squadSize = 2;
    p.mates[0] = MateAt(900, {-6, 0, 0});
    p.mateCount = 1;
    Brain calm;
    assert(calm.Tick(p).target == 70);
    p.enemies[1].victim = 900;
    Brain b;
    assert(b.Tick(p).target == 71);
}

// The leader between the bot and its target: no shot, a step out of the line.
void NoShotThroughTheLeader() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(80, {0, 0, 8});
    p.enemyCount = 1;
    p.leader = {0, 0, 4};
    Brain b;
    bool blocked = false, stepped = false, early = false;
    for (int i = 0; i < 25; ++i) {
        p.now += 100;
        const Command c = b.Tick(p);
        assert(!c.fire && !c.holdFire);
        blocked = blocked || c.laneBlocked;
        const bool out = c.move && std::fabs(c.moveTo.x) > 1.0f;
        early = early || (out && i < 10);
        stepped = stepped || out;
    }
    assert(blocked && stepped && !early);   // not for a leader just passing through the line
    p.leader = {3, 0, 0};
    bool fired = false;
    for (int i = 0; i < 10; ++i) { p.now += 100; fired = fired || b.Tick(p).fire; }
    assert(fired);
}

// Two bots take the two sides of the leader in a fight.
void SquadTakesBothSides() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, 0};
    p.enemies[0] = Slasher(90, {0, 0, 10});
    p.enemyCount = 1;
    p.squadSize = 2;
    p.mateCount = 1;
    Perception q = p;
    p.self = {-0.5f, 0, -9};
    p.squadIndex = 0;
    p.mates[0] = MateAt(901, {0.5f, 0, -9}, 90);
    q.self = {0.5f, 0, -9};
    q.squadIndex = 1;
    q.mates[0] = MateAt(900, {-0.5f, 0, -9}, 90);
    Brain first, second;
    Command a = first.Tick(p), b = second.Tick(q);
    for (int i = 0; i < 15 && !(a.move && b.move && a.moveTo.x > 1.0f && b.moveTo.x < -1.0f); ++i) {
        p.now += 100;
        q.now += 100;
        a = first.Tick(p);
        b = second.Tick(q);
    }
    assert(a.move && b.move);
    assert(a.moveTo.x > 1.0f && b.moveTo.x < -1.0f);
}

// Standing on a mate: it steps off. Following: not to where a mate is headed.
void KeepsRoomFromMates() {
    Perception p = Calm();
    p.squadSize = 2;
    p.mates[0] = MateAt(900, {0.3f, 0, 0.2f});
    p.mateCount = 1;
    Brain b;
    const Command c = b.Tick(p);
    assert(c.move && Flat(c.moveTo, p.mates[0].pos) >= 1.5f);
    // Following far behind, the mate walking to the very spot.
    Perception f = Calm();
    f.self = {0, 0, -12};
    f.leader = {0, 0, 0};
    f.squadSize = 2;
    Brain probe;
    const Vec3 spot = probe.Tick(f).moveTo;
    f.mates[0] = MateAt(900, {5, 0, -12});
    f.mates[0].goal = spot;
    f.mates[0].moving = true;
    f.mateCount = 1;
    Brain g;
    const Command d = g.Tick(f);
    assert(d.move && Flat(d.moveTo, spot) >= 1.5f);
}

// An item a mate goes for is left to it.
void LeavesAMatesItem() {
    Perception p = Calm();
    p.items[0] = {42, {-2, 0, -3}};   // off the leader's feet (those are his)
    p.itemCount = 1;
    Brain alone;
    assert(alone.Tick(p).mode == Mode::Pickup);
    p.squadSize = 2;
    p.mates[0] = MateAt(900, {-2, 0, 0});
    p.mates[0].item = 42;
    p.mateCount = 1;
    Brain b;
    assert(b.Tick(p).mode != Mode::Pickup);
}

// Rocking between a wall and the trail: moving, but no closer. The goal is given
// up within a few seconds instead of forever; plain walking never gives up.
void GivesUpWithoutProgress() {
    Steer steer;
    Trail trail;
    const Vec3 goal = {0, 0, 10}, leader = {0, 0, 6};   // an idle spot by the leader
    const Vec3 path[3] = {{0, 0, 0}, {0, 0, 5}, {0, 0, 10}};
    uint32_t now = 1000;
    bool gaveUp = false;
    for (int i = 0; i < 80 && !gaveUp; ++i, now += 100) {
        const Vec3 self = {i % 2 ? 0.8f : 0.0f, 0, 1.0f};   // side to side, never forward
        const SteerOutput o = steer.Tick(self, goal, true, leader, trail, now);
        if (o.askPath) steer.SetPath(path, 3, now);
        if (o.blocked) { gaveUp = true; assert(!o.move && now - 1000 <= 5000); }
    }
    assert(gaveUp);
    Steer walker;
    now = 1000;
    for (int i = 0; i < 90; ++i, now += 100) {
        const Vec3 self = {0, 0, 0.1f * static_cast<float>(i)};
        const SteerOutput o = walker.Tick(self, goal, true, leader, trail, now);
        if (o.askPath) walker.SetPath(path, 3, now);
        assert(!o.blocked);
    }
}

// A fight spot it could not reach is not walked at again right away.
void KeepsOffABlockedSpot() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, 0};
    p.self = {0, 0, -8};
    p.enemies[0] = Slasher(95, {0, 0, 9});
    p.enemyCount = 1;
    Brain b;
    const Command first = b.Tick(p);
    assert(first.move);
    b.Unreachable(first.moveTo, p.self, p.now);
    p.now += 100;
    assert(!b.Tick(p).move);
    p.now += 7000;
    assert(b.Tick(p).move);
}

// Partner pose: the first bot goes to its place behind the leader's right and,
// once there, watches him; a turn of his moves the place, a glance does not.
// The second bot of a squad takes a place of its own, not this one.
void KeepsThePartnerPose() {
    Perception p = Calm();
    p.leader = {0, 0, 0};
    p.leaderYaw = 0.0f;   // facing +z
    p.self = {3, 0, 3};
    Brain b;
    b.tune.partnerPose = true;
    b.tune.partnerApproach = 0.0f;
    b.tune.partnerForward = -1.23f;
    b.tune.partnerRight = 1.71f;
    b.tune.partnerFaceNear = 1.4f;
    const Vec3 spot = {-1.71f * 1.0f * 1.0f, 0.0f, -1.23f};   // his left is (cos, 0, -sin) = +x here, so his right is -x
    Command c = b.Tick(p);
    assert(c.move && Flat(c.moveTo, spot) < 0.05f);
    p.self = {spot.x + 0.1f, 0, spot.z};
    bool watched = false;
    for (int i = 0; i < 40; ++i) {
        p.now += 100;
        c = b.Tick(p);
        assert(!c.move);
        watched = watched || (c.look && Distance(c.lookAt, {0, 1.6f, 0}) < 0.01f);
    }
    assert(watched);
    p.leaderYaw = 0.3f;   // a glance: stays
    p.now += 100;
    assert(!b.Tick(p).move);
    p.leaderYaw = 3.14159265f;   // turned round: the place goes with him
    p.now += 100;
    c = b.Tick(p);
    assert(c.move && Flat(c.moveTo, {1.71f, 0, 1.23f}) < 0.05f);
    Brain second;
    second.tune.partnerPose = true;
    second.tune.partnerApproach = 0.0f;
    second.tune.partnerForward = -1.23f;
    second.tune.partnerRight = 1.71f;
    Perception q = Calm();
    q.squadSize = 2;
    q.squadIndex = 1;
    q.self = {3, 0, 3};
    q.leader = {0, 0, 0};
    const Command d = second.Tick(q);
    assert(!d.move || Flat(d.moveTo, spot) > 0.5f);
}

// At its place but turned away: a short step at the leader turns the body. Facing
// him already: it stands. A few tries at most.
void FacesThePartner() {
    Perception p = Calm();
    p.leader = {0, 0, 0};
    p.leaderYaw = 0.0f;
    p.self = {-1.71f, 0, -1.23f};   // on the spot
    p.selfYawKnown = true;
    const float toLeader = std::atan2(1.71f, 1.23f);
    p.selfYaw = toLeader + 1.5f;   // turned well away
    Brain b;
    b.tune.partnerPose = true;
    b.tune.partnerApproach = 0.0f;
    b.tune.partnerForward = -1.23f;
    b.tune.partnerRight = 1.71f;
    b.tune.partnerFaceNear = 1.4f;
    int steps = 0;
    bool towardHim = true;
    for (int i = 0; i < 100; ++i) {
        p.now += 100;
        const Command c = b.Tick(p);
        if (c.move) {
            ++steps;
            towardHim = towardHim && Flat(c.moveTo, p.leader) < Flat(p.self, p.leader);
        }
    }
    assert(steps > 0 && towardHim);
    assert(steps <= 3 * 4 + 1);   // three tries of about 350 ms
    Brain facing;
    facing.tune.partnerPose = true;
    facing.tune.partnerApproach = 0.0f;
    facing.tune.partnerForward = -1.23f;
    facing.tune.partnerRight = 1.71f;
    p.selfYaw = toLeader + 0.1f;
    for (int i = 0; i < 60; ++i) { p.now += 100; assert(!facing.Tick(p).move); }
}

// The partner place in a wall: the other side, then the closest point, and there
// it counts as arrived: it looks at the leader instead of "walking" for good.
void PartnerPlaceInAWall() {
    Perception p = Calm();
    p.leader = {0, 0, 0};
    p.leaderYaw = 0.0f;
    p.self = {3, 0, 3};
    Brain b;
    b.tune.partnerPose = true;
    b.tune.partnerApproach = 0.0f;
    b.tune.partnerForward = -1.23f;
    b.tune.partnerRight = 1.71f;
    b.tune.partnerFaceNear = 1.4f;
    Command c = b.Tick(p);
    assert(c.move && Flat(c.moveTo, {-1.71f, 0, -1.23f}) < 0.05f);
    b.Unreachable(c.moveTo, {-0.6f, 0, -1.2f}, p.now);
    p.now += 100;
    c = b.Tick(p);
    assert(c.move && Flat(c.moveTo, {1.71f, 0, -1.23f}) < 0.05f);   // mirrored: behind his left
    const Vec3 closest = {1.2f, 0, -1.0f};
    b.Unreachable(c.moveTo, closest, p.now);
    p.self = closest;
    bool watched = false;
    for (int i = 0; i < 40; ++i) {
        p.now += 100;
        c = b.Tick(p);
        assert(!c.move);
        watched = watched || (c.look && Distance(c.lookAt, {0, 1.6f, 0}) < 0.01f);
    }
    assert(watched);
}

// The second bot has its own partner place: behind the leader, to his right.
void SecondPartnerPlace() {
    Perception p = Calm();
    p.leader = {0, 0, 0};
    p.leaderYaw = 0.0f;   // facing +z; his right is -x in this right-handed frame
    p.self = {3, 0, 3};
    p.squadSize = 2;
    p.squadIndex = 1;
    Brain b;
    b.tune.partnerPose = true;
    b.tune.partnerApproach = 0.0f;
    const Command c = b.Tick(p);
    assert(c.move && Flat(c.moveTo, {-b.tune.partner2Right, 0, b.tune.partner2Forward}) < 0.05f);
    p.squadIndex = 2;
    Brain third;
    third.tune.partnerPose = true;
    third.tune.partnerApproach = 0.0f;
    const Command d = third.Tick(p);
    assert(!d.move || Flat(d.moveTo, c.moveTo) > 0.5f);   // the third loiters
}

// It comes in from outside its place, the last stretch at the leader with its
// eyes on him, and face steps never walk it much closer than its place.
void ComesInFacingHim() {
    Perception p = Calm();
    p.leader = {0, 0, 0};
    p.leaderYaw = 0.0f;
    p.self = {0, 0, -6};
    Brain b;
    b.tune.partnerPose = true;
    const float reach = std::sqrt(b.tune.partnerForward * b.tune.partnerForward + b.tune.partnerRight * b.tune.partnerRight);
    const Vec3 spot = {-b.tune.partnerRight, 0, b.tune.partnerForward};
    Command c = b.Tick(p);
    assert(c.move && Flat(c.moveTo, p.leader) > reach + 0.4f);   // the point outside
    p.self = c.moveTo;
    p.now += 100;
    c = b.Tick(p);
    assert(c.move && Flat(c.moveTo, spot) < 0.05f && c.look);   // in, eyes on him
    p.self = spot;
    p.selfYawKnown = true;
    p.selfYaw = std::atan2(-spot.x, -spot.z) + 1.4f;   // arrived turned away anyway
    float closest = 99.0f;
    for (int i = 0; i < 100; ++i) {
        p.now += 100;
        c = b.Tick(p);
        if (c.move) {
            closest = Flat(c.moveTo, p.leader) < closest ? Flat(c.moveTo, p.leader) : closest;
            p.self = c.moveTo;   // it gets there
        }
    }
    assert(closest >= b.tune.partnerFaceNear - 0.01f && closest >= reach - b.tune.partnerFaceCloser - 0.01f);
}

// --- the bench ---
BenchNode Node(uint32_t key, int type, bool filled, bool start, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
    BenchNode n;
    n.key = key;
    n.type = type;
    n.filled = filled;
    n.start = start;
    n.links[0] = a;
    n.links[1] = b;
    n.links[2] = c;
    return n;
}

// Only nodes next to a filled one (or a start point) can be bought; the best
// stat wins, a blank is bought on the way to a good stat, and a finished tree
// gives nothing.
void PicksBenchNodes() {
    // start (filled) -> capacity; start -> blank -> damage
    BenchNode tree[4] = {Node(1, 0, true, true, 2, 3), Node(2, 3, false, false, 1), Node(3, 0, false, false, 1, 4), Node(4, 1, false, false, 3)};
    assert(PickBenchNode(tree, 4) == 1);   // capacity (5) beats damage two steps away (6 / 2)
    tree[1].filled = true;
    assert(PickBenchNode(tree, 4) == 2);   // the blank on the way to damage
    tree[2].filled = true;
    assert(PickBenchNode(tree, 4) == 3);
    tree[3].filled = true;
    assert(PickBenchNode(tree, 4) == -1);
    // Nothing filled yet: only the start point.
    BenchNode fresh[2] = {Node(10, 0, false, true, 11), Node(11, 1, false, false, 10)};
    assert(PickBenchNode(fresh, 2) == 0);
    // A node with no way in is never picked.
    BenchNode island[2] = {Node(20, 0, true, true), Node(21, 1, false, false)};
    assert(PickBenchNode(island, 2) == -1);
}

// An empty gun with nothing left for it: no reload loop, the gun with ammo instead.
// Empty with ammo left: a reload, eyes on the target.
void SwitchesFromAnEmptyGun() {
    Perception p = Armed(Gun::PlasmaCutter, Gun::PulseRifle);
    p.guns[0].clip = 0;
    p.guns[0].reserve = 0;
    p.clip = 0;
    p.reserve = 0;
    p.enemies[0] = Slasher(97, {0, 0, 7});
    p.enemyCount = 1;
    Brain b;
    const Command c = b.Tick(p);
    assert(!c.reload && c.slot == 1 && c.look);
    Perception q = Armed(Gun::PlasmaCutter);
    q.guns[0].clip = 0;
    q.clip = 0;
    q.reserve = 20;
    q.enemies[0] = Slasher(98, {0, 0, 7});
    q.enemyCount = 1;
    Brain r;
    const Command d = r.Tick(q);
    assert(d.reload && d.look && !d.fire);
}

// The pulse rifle works on limbs too, not the chest; so does the aim while the
// weapon changes.
void PulseAimsAtLimbs() {
    Perception p = Armed(Gun::PulseRifle);
    p.enemies[0] = Slasher(99, {0, 0, 7});
    p.enemyCount = 1;
    Brain b;
    bool fired = false;
    for (int i = 0; i < 10; ++i) {
        p.now += 100;
        const Command c = b.Tick(p);
        bool onLimb = false;
        for (int l = 0; l < p.enemies[0].limbCount; ++l) onLimb = onLimb || Distance(c.lookAt, p.enemies[0].limbs[l]) < 0.01f;
        assert(onLimb);
        if (c.holdFire) { fired = true; assert(c.shotAtLimb && !c.shotAtChest); }
    }
    assert(fired);
    Perception q = Armed(Gun::PlasmaCutter, Gun::PulseRifle);
    q.guns[0].clip = 0;
    q.guns[0].reserve = 0;
    q.clip = 0;
    q.reserve = 0;
    q.enemies[0] = Slasher(100, {0, 0, 7});
    q.enemyCount = 1;
    Brain w;
    const Command d = w.Tick(q);
    assert(d.slot == 1 && Distance(d.lookAt, q.enemies[0].chest) > 0.05f);
}

// It keeps its own pace: a few steps of the leader leave it where it is; once
// he is past the follow distance it goes, to a place that stays while he keeps
// walking, and it arrives rather than trailing him.
void CombatKeepsItsOwnPace() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, 0};
    p.self = {2.2f, 0, -1.0f};
    p.enemies[0] = Slasher(120, {0, 0, 9});
    p.enemyCount = 1;
    Brain b;
    assert(!b.Tick(p).move);
    p.leader = {0, 0, -5};   // a few steps back: it holds
    Command c = b.Tick(p);
    for (int i = 0; i < 12; ++i) { assert(!c.move); p.now += 100; c = b.Tick(p); }
    p.leader = {0, 0, -7.5f};   // past the follow distance: it goes
    p.now += 100;
    c = b.Tick(p);
    assert(c.move && c.why == MoveWhy::Follow);
    const Vec3 chosen = c.moveTo;
    p.leader = {0, 0, -8.0f};   // and he keeps walking: the place stays for now
    p.now += 100;
    c = b.Tick(p);
    assert(c.move && Flat(c.moveTo, chosen) < 0.01f);
    p.self = chosen;   // there
    p.now += 100;
    assert(!b.Tick(p).move);
}

// The Marker's heart in the finale: shot before a closer enemy, and from farther
// off than any enemy would be.
void ShootsTheObjective() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(130, {0, 0, 5});
    Enemy heart;
    heart.id = 131;
    heart.pos = heart.chest = {0, 2, 35};
    heart.health = heart.maxHealth = 100.0f;
    heart.objective = true;
    p.enemies[1] = heart;
    p.enemyCount = 2;
    Brain b;
    assert(b.Tick(p).target == 131);
    p.enemies[1].objective = false;
    Brain plain;
    assert(plain.Tick(p).target == 130);
}

// Left behind in a fight: back to the leader at once, still aiming and shooting.
void FollowsInAFight() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, 0};
    p.self = {-7, 0, 0};
    p.enemies[0] = Slasher(140, {-7, 0, 8});
    p.enemyCount = 1;
    Brain b;
    const Command c = b.Tick(p);
    assert(c.move && c.aim && Flat(c.moveTo, p.leader) < 4.0f);
    bool fired = false;
    for (int i = 0; i < 10; ++i) { p.now += 100; const Command d = b.Tick(p); assert(d.move); fired = fired || d.fire; }
    assert(fired);
}

// Held by an enemy: nothing but mashing.
void MashesWhenHeld() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(141, {0, 0, 1});
    p.enemyCount = 1;
    p.grabbed = true;
    Brain b;
    const Command c = b.Tick(p);
    assert(c.mash && !c.fire && !c.move);
}

// Left far behind with an enemy at arm's length (the Ubermorph): it still makes
// its way back to the leader, not just away from the enemy.
void FollowsWithAnEnemyOnIt() {
    const float gaps[] = {0.9f, 2.0f};
    for (const float gap : gaps) {
        Perception p = Armed(Gun::PlasmaCutter);
        p.leader = {0, 0, 0};
        p.self = {-20, 0, 0};
        p.enemies[0] = Slasher(150, {-20.0f - gap, 0, 0.3f});
        p.enemies[0].maxHealth = p.enemies[0].health = 100000.0f;
        p.enemyCount = 1;
        Brain b;
        bool closer = false;
        for (int i = 0; i < 12; ++i) {
            const Command c = b.Tick(p);
            closer = closer || (c.move && Flat(c.moveTo, p.leader) < Flat(p.self, p.leader) - 0.5f);
            p.now += 100;
        }
        assert(closer);
    }
}

// The Ubermorph waits while something killable is about, unless it is on top of the bot.
void UbermorphComesLast() {
    Perception p = Armed(Gun::PlasmaCutter);
    Enemy uber = Slasher(160, {0, 0, 5});
    uber.maxHealth = uber.health = 100000.0f;
    p.enemies[0] = uber;
    p.enemies[1] = Slasher(161, {3, 0, 9});
    p.enemyCount = 2;
    Brain b;
    assert(b.Tick(p).target == 161);
    p.enemies[0].pos = p.enemies[0].chest = {0, 0, 2};
    Brain close;
    assert(close.Tick(p).target == 160);
    // Alone, it is still fought.
    p.enemies[0].pos = p.enemies[0].chest = {0, 0, 6};
    p.enemyCount = 1;
    Brain alone;
    assert(alone.Tick(p).target == 160);
}

// The leader shuffles forward and back while aiming: the bot holds its place.
void HoldsWhileLeaderShuffles() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.self = {2.2f, 0, -1};
    p.enemies[0] = Slasher(170, {0, 0, 12});
    p.enemyCount = 1;
    Brain b;
    for (int i = 0; i < 40; ++i) {
        p.leader = {0, 0, (i / 10) % 2 ? 4.0f : -4.0f};
        const Command c = b.Tick(p);
        assert(!c.move || c.why == MoveWhy::Lane);
        p.now += 100;
    }
}

// A personality's reaction: a new enemy is fought only after its moment.
void NoticesAfterItsReaction() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(190, {0, 0, 9});
    p.enemyCount = 1;
    Brain b;
    b.tune.reactMinMs = b.tune.reactMaxMs = 400;
    assert(b.Tick(p).target == 0);
    p.now += 300;
    assert(b.Tick(p).target == 0);
    p.now += 150;
    assert(b.Tick(p).target == 190);
}

// Two bots with their own reactions do not both switch in the same frame.
void BotsSwitchApart() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(191, {0, 0, 9});
    p.enemyCount = 1;
    Brain a(11), b(12345);
    a.tune.reactMinMs = b.tune.reactMinMs = 150;
    a.tune.reactMaxMs = b.tune.reactMaxMs = 700;
    uint32_t aAt = 0, bAt = 0;
    for (int i = 0; i < 100 && (!aAt || !bAt); ++i) {
        if (!aAt && a.Tick(p).target) aAt = p.now + 1;
        if (!bAt && b.Tick(p).target) bAt = p.now + 1;
        p.now += 16;
    }
    assert(aAt && bAt && aAt != bAt);
}

// After the last enemy it keeps its eyes open a moment, then relaxes.
void ScansAfterAFight() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(192, {0, 0, 8});
    p.enemyCount = 1;
    Brain b;
    b.tune.postCombatMinMs = b.tune.postCombatMaxMs = 1500;
    assert(b.Tick(p).target == 192);
    p.enemyCount = 0;
    p.now += 100;
    Command c = b.Tick(p);
    assert(c.mode == Mode::Combat && c.aim && !c.target && c.lookAt.z > 2.0f);
    p.now += 1600;
    c = b.Tick(p);
    assert(c.mode != Mode::Combat);
}

// A new target is aimed at before the first shot; the aim wanders only a little.
void AimsBeforeItShoots() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(193, {0, 0, 7});
    p.enemyCount = 1;
    p.aimErrorDeg = 0.1f;
    Brain b;
    b.tune.aimAcquireMs = 300;
    b.tune.aimSway = 0.1f;
    Command c = b.Tick(p);
    assert(c.target == 193 && !c.fire && !c.holdFire);
    bool fired = false;
    for (int i = 0; i < 30; ++i) {
        p.now += 50;
        c = b.Tick(p);
        fired = fired || c.fire;
        bool nearPoint = Distance(c.lookAt, p.enemies[0].chest) < 0.35f;
        for (int l = 0; l < p.enemies[0].limbCount; ++l) nearPoint = nearPoint || Distance(c.lookAt, p.enemies[0].limbs[l]) < 0.35f;
        assert(nearPoint);
    }
    assert(fired);
}

// Its own place: to the leader's right at the profile's angle.
void TakesItsOwnPlace() {
    Perception p;
    p.leader = {0, 0, 0};
    p.leaderYaw = 0.0f;   // facing +z; his right is -x
    p.self = {0, 0, -10};
    Brain b;
    b.tune.ownPlaces = true;
    b.tune.followAngleDeg = 90.0f;
    b.tune.followDistance = 2.5f;
    const Command c = b.Tick(p);
    assert(c.move && c.moveTo.x < -2.0f && std::fabs(c.moveTo.z) < 0.5f);
}

// Empty hands (a new game): no aiming, it keeps away; it swings only at what is on it.
void UnarmedKeepsAway() {
    Perception p;
    p.gunsKnown = true;
    p.enemies[0] = Slasher(200, {0, 0, 5});
    p.enemyCount = 1;
    Brain b;
    Command c = b.Tick(p);
    assert(c.target == 200 && !c.aim && !c.fire && !c.move);   // 5 m off: it stands its ground
    p.enemies[0] = Slasher(200, {0, 0, 3});
    Brain nearer;
    c = nearer.Tick(p);
    assert(!c.aim && !c.fire && c.move && c.moveTo.z < 0.0f);   // closer: it backs away
    p.enemies[0] = Slasher(200, {0, 0, 1});
    p.now = 5000;   // the first swing waits its gap, as the armed one does
    Brain close;
    c = close.Tick(p);
    assert(!c.aim && c.melee);
}

// The calm values the runtime sets: the leader's steps back and forth near a
// following bot and near the partner do not move them.
void IgnoresTheLeadersShuffle() {
    Perception p;
    p.self = {2.5f, 0, -1.0f};
    p.leaderMoving = true;
    Brain b;
    b.tune.followReanchor = 3.5f;
    b.tune.followKeep = 4.5f;
    for (int i = 0; i < 40; ++i) {
        p.leader = {0, 0, (i / 5) % 2 ? 1.5f : -1.5f};
        const Command c = b.Tick(p);
        assert(!c.move);
        p.now += 100;
    }
    // The partner: settled at its place, the leader shuffling 1.5 m does not move it.
    Perception q;
    q.leader = {0, 0, 0};
    q.leaderYaw = 0.0f;
    Brain partner;
    partner.tune.partnerPose = true;
    partner.tune.partnerMove = 3.0f;
    partner.tune.partnerTurnDeg = 100.0f;
    partner.tune.reactMinMs = partner.tune.reactMaxMs = 400;
    q.self = {-0.85f, 0, 0.97f};
    for (int i = 0; i < 30; ++i) { partner.Tick(q); q.now += 100; }
    int moves = 0;
    for (int i = 0; i < 40; ++i) {
        q.leader = {0, 0, (i / 5) % 2 ? 1.5f : 0.0f};
        const Command c = partner.Tick(q);
        if (c.move && Flat(c.moveTo, q.self) > 1.2f) ++moves;
        q.now += 100;
    }
    assert(moves == 0);
}

// A swing: turned toward the enemy's middle first, then the swing, standing.
void SwingsFacingTheChest() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(210, {0, 0, 1});
    p.enemyCount = 1;
    p.now = 5000;
    p.selfYawKnown = true;
    p.selfYaw = 3.14159265f;   // facing away
    Brain b;
    Command c = b.Tick(p);
    assert(!c.melee && c.move && c.moveTo.z > 0.0f && Distance(c.lookAt, p.enemies[0].chest) < 0.3f);
    p.selfYaw = 0.0f;          // now facing it
    p.now += 100;
    c = b.Tick(p);
    assert(c.melee && !c.move && Distance(c.lookAt, p.enemies[0].chest) < 0.3f);
    p.now += 200;
    c = b.Tick(p);
    assert(!c.move);           // standing through the swing
}

// The leader walks at it: a step aside, off his line; beside his way it stays.
void MakesWayForTheLeader() {
    Perception p;
    p.leader = {0, 0, 0};
    p.leaderVel = {0, 0, 3.0f};    // walking toward +z
    p.self = {0.3f, 0, 1.2f};
    Brain b;
    Command c = b.Tick(p);
    assert(c.move && c.moveTo.x > 1.0f);   // to its side of his line, out of the way
    p.self = {2.5f, 0, 1.2f};
    Brain aside;
    c = aside.Tick(p);
    assert(!(c.move && c.why == MoveWhy::Step));
    p.self = {0.3f, 0, -2.0f};              // behind him: nothing to do
    Brain behind;
    c = behind.Tick(p);
    assert(!(c.move && c.why == MoveWhy::Step));
}

// Zero-G flight: turns to the goal first, then thrusts; aims elsewhere and
// slides; each bot its own place by the leader; another surface told apart.
void FliesTowardItsPlace() {
    BodyFrame b;
    b.pos = {0, 0, 0};
    b.forward = {0, 0, 1};
    b.left = {-1, 0, 0};
    b.up = {0, 1, 0};
    FlightStick s = FlyToward(b, {0, 0, 12}, {0, 0, 12}, false, false);
    assert(std::fabs(s.yaw) < 0.01f && std::fabs(s.pitch) < 0.01f);
    assert(s.ly < -0.95f && std::fabs(s.lx) < 0.05f && s.boost);
    s = FlyToward(b, {-6, 0, 0}, {-6, 0, 0}, false, false);   // to its left: turn, no thrust yet
    assert(s.yaw > 0.5f && s.ly == 0.0f);
    s = FlyToward(b, {0, 6, 1}, {0, 6, 1}, false, false);     // above: pitch up (negative)
    assert(s.pitch < -0.5f);
    s = FlyToward(b, {0, 0, 0.5f}, {0, 0, 0.5f}, false, false);   // there: no thrust
    assert(s.ly == 0.0f && s.lx == 0.0f && !s.boost);
    s = FlyToward(b, {5, 0, 0}, {0, 0, 10}, true, true);      // aims ahead, its place to the right: slides right
    assert(s.lx > 0.9f && std::fabs(s.ly) < 0.05f && !s.boost);
    BodyFrame leader = b;
    leader.pos = {0, 10, 0};
    const Vec3 first = FlightGoal(leader, true, 0), second = FlightGoal(leader, true, 1), third = FlightGoal(leader, true, 2);
    assert(first.z < 0.0f && first.x < 0.0f && second.x > 0.0f && third.y < 10.0f);
    const Vec3 standing = FlightGoal(leader, false, 0);
    assert(standing.y > 10.5f);
    BodyFrame onWall = leader;
    onWall.pos = {1, 0.5f, 0};
    onWall.up = {1, 0, 0};
    assert(OtherSurface(b, onWall));
    BodyFrame beside = b;
    beside.pos = {3, 0.2f, 1};
    assert(!OtherSurface(b, beside));
}

// The leader walks about in a fight: a bot with a target holds where it stands;
// one without (the enemy out of its sight) stays with the leader. Left far behind,
// each goes back to him.
void HoldsWhileTheLeaderMovesInAFight() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, 0};
    p.self = {2.0f, 0, -1.0f};
    p.enemies[0] = Slasher(150, {0, 0, 9});
    p.enemyCount = 1;
    Brain b;
    Command c = b.Tick(p);
    const float walk[][2] = {{0, 2}, {1.5f, 3}, {-2, 1}, {0, -3}, {3, -2}, {0.5f, 0.5f}, {2.0f, -0.8f}};
    for (const auto& w : walk) {
        p.leader = {w[0], 0, w[1]};
        for (int i = 0; i < 6; ++i) { p.now += 100; c = b.Tick(p); assert(!c.move); }
    }
    // The same without a target: the enemy out of sight, but after the leader. It
    // does not hold the bot any more (08.10.: standing still for an enemy it could
    // not see looked frozen); the bot stays with the leader instead.
    Perception q = p;
    q.enemies[0].visible = false;
    q.enemies[0].victim = 0x1234;
    q.leader = {0, 0, 0};
    Brain unseen;
    for (const auto& w : walk) {
        q.leader = {w[0], 0, w[1]};
        for (int i = 0; i < 6; ++i) { q.now += 100; c = unseen.Tick(q); assert(c.mode != Mode::Combat); }
    }
    assert(unseen.unseenThreatFollows > 0);
    // Left far behind: back to him.
    q.leader = {0, 0, 12};
    bool back = false;
    for (int i = 0; i < 20 && !back; ++i) { q.now += 100; c = unseen.Tick(q); back = c.move; }
    assert(back);
}

// Standing right beside the leader it looks past him, from farther at him (06.10.:
// heads jittered looking at him from 0.3 m).
void LooksPastALeaderRightBeside() {
    const float distances[] = {0.5f, 3.0f};
    for (const float apart : distances) {
        Perception p = Armed(Gun::PlasmaCutter);
        p.leader = {0, 0, 0};
        p.self = {apart, 0, 0};
        Brain b;
        b.tune.glanceChance = 0.0f;
        b.tune.idleMin = apart - 0.1f;   // its idle spot that far off
        b.tune.idleMax = apart + 0.1f;
        bool atLeader = false, looked = false;
        for (int i = 0; i < 300; ++i) {
            p.now += 100;
            const Command c = b.Tick(p);
            if (c.move) {
                const Vec3 to = Sub(c.moveTo, p.self);
                const float len = Flat(c.moveTo, p.self);
                if (len > 0.0f) p.self = Add(p.self, Scale(to, (len < 0.3f ? len : 0.3f) / len));
                continue;
            }
            if (!c.look) continue;
            looked = true;
            atLeader = atLeader || Distance(c.lookAt, Vec3{0, 1.6f, 0}) < 0.01f;
        }
        assert(looked);
        assert(atLeader == (apart > b.tune.leaderLookMin));
        assert((b.leaderLookSkips > 0) == (apart < b.tune.leaderLookMin));
    }
}

// A necromorph playing dead behind a wall: never seen, still, after nobody. It
// holds no bot and raises no gun (06.10.). Seen once, it is remembered a while.
void PlayingDeadBehindAWallHoldsNobody() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, 0};
    p.self = {2.0f, 0, -1.0f};
    p.enemies[0] = Slasher(150, {0, 0, 5});
    p.enemies[0].visible = false;
    p.enemyCount = 1;
    Brain b;
    for (int i = 0; i < 30; ++i) {
        p.now += 100;
        const Command c = b.Tick(p);
        assert(c.mode != Mode::Combat && !c.aim);
    }
    assert(b.unawareSkips > 0);
    // Moving close by: heard. Beyond the back-off range a bot does not stop for one
    // it cannot see (08.10.: it looked frozen), so this one is right beside it.
    Perception heard = p;
    heard.enemies[0].pos = {3.0f, 0, 0.5f};
    heard.enemies[0].chest = {3.0f, 1.2f, 0.5f};
    heard.enemies[0].vel = {1.5f, 0, 0};
    Brain h;
    bool up = false;
    for (int i = 0; i < 30 && !up; ++i) { heard.now += 100; const Command hc = h.Tick(heard); up = hc.aim && hc.mode != Mode::Combat; }
    assert(up && h.alertFrames > 0);
    // Seen, then behind the wall again: still a threat within the memory, not after.
    Perception seen = p;
    seen.enemies[0].pos = {3.0f, 0, 0.5f};
    seen.enemies[0].chest = {3.0f, 1.2f, 0.5f};
    seen.enemies[0].visible = true;
    Brain s;
    for (int i = 0; i < 10; ++i) { seen.now += 100; s.Tick(seen); }
    seen.enemies[0].visible = false;
    seen.now += 1000;
    assert(s.Tick(seen).mode != Mode::Combat && s.unseenThreatFollows > 0);
    seen.now += s.tune.threatMemoryMs + 1000;
    bool calm = false;
    for (int i = 0; i < 30 && !calm; ++i) { seen.now += 100; calm = !s.Tick(seen).aim; }
    assert(calm);
}

// The leader runs in the straitjacket: the bot runs right behind him, an enemy
// beside it or not; the third one a little farther back; there, it stops.
// An enemy seen a moment ago, now behind a wall a few metres off: the bot
// keeps following the leader instead of standing still with its gun up.
void UnseenThreatDoesNotFreeze() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, 0};
    p.self = {2.0f, 0, -1.0f};
    p.enemies[0] = Slasher(151, {0, 0, 6});
    p.enemyCount = 1;
    Brain b;
    for (int i = 0; i < 5; ++i) { p.now += 100; b.Tick(p); }
    p.enemies[0].visible = false;
    p.now += 500;
    const Command c = b.Tick(p);
    assert(c.mode != Mode::Combat && b.unseenThreatFollows > 0);
}

// A mate fights an enemy this bot cannot see: the bot comes over, by the mesh.
void JoinsAMatesFight() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, 0};
    p.self = {-6.0f, 0, -4.0f};
    p.enemies[0] = Slasher(160, {6, 0, 8});
    p.enemies[0].visible = false;
    p.enemyCount = 1;
    p.squadSize = 2;
    p.mates[0] = MateAt(920, {4, 0, 2}, 160);
    p.mates[0].aiming = true;
    p.mates[0].aimAt = {6, 1.2f, 8};
    p.mateCount = 1;
    Brain b;
    p.now += 100;
    const Command c = b.Tick(p);
    assert(c.move && c.why == MoveWhy::Chase && Flat(c.moveTo, p.enemies[0].pos) < 0.1f && b.joinFrames == 1);
    // Close enough: it stops there, alert, and does not walk into it.
    p.self = {5.0f, 0, 5.5f};
    p.now += 100;
    assert(!(b.Tick(p).why == MoveWhy::Chase));
}

// The leader shoots at something round the corner: the bots come too. Without
// his shots and with nobody on it, an unseen enemy does not pull them.
void JoinsWhenTheLeaderShoots() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, 0};
    p.self = {-3.0f, 0, -3.0f};
    p.enemies[0] = Slasher(161, {5, 0, 9});
    p.enemies[0].visible = false;
    p.enemyCount = 1;
    Brain b;
    p.now += 100;
    assert(b.Tick(p).why != MoveWhy::Chase);
    p.leaderShooting = true;
    p.now += 100;
    const Command c = b.Tick(p);
    assert(c.move && c.why == MoveWhy::Chase && c.run);
}

void HangsOnTheLeaderInAChase() {
    Perception p = Calm();
    p.chase = true;
    p.leader = {0, 0, 10};
    p.leaderVel = {0, 0, 4.0f};
    p.leaderMoving = true;
    p.self = {0, 0, 2};
    p.enemies[0] = Slasher(160, {1, 0, 4});
    p.enemyCount = 1;
    Brain b;
    Command c = b.Tick(p);
    assert(c.move && c.run && !c.fire && !c.melee && !c.stomp && c.mode == Mode::Rush);
    assert(Flat(c.moveTo, p.leader) < 2.0f && c.moveTo.z < p.leader.z);
    p.squadIndex = 2;
    p.squadSize = 3;
    Brain third;
    c = third.Tick(p);
    assert(c.move && Flat(c.moveTo, p.leader) > 3.0f && Flat(c.moveTo, p.leader) < 4.5f);
    p.squadIndex = 0;
    p.squadSize = 1;
    p.self = {0, 0, 8.6f};
    p.leaderVel = {};
    Brain close;
    c = close.Tick(p);
    assert(!c.move);
}

// Loot at the leader's feet is his: the bot leaves it.
void LeavesTheLeadersLoot() {
    Perception p = Calm();
    p.items[0] = {44, {2.5f, 0, 1.0f}};   // 1.1 m from the leader
    p.itemCount = 1;
    Brain b;
    const Command c = b.Tick(p);
    assert(c.mode != Mode::Pickup);
    // What a bot just stomped out of a corpse or crate beside him is the squad's to take.
    p.items[0].botLoot = true;
    Brain d;
    assert(d.Tick(p).mode == Mode::Pickup);
}

// --- the leader's orders ---------------------------------------------------------

bool Attacks(const Command& c) { return c.fire || c.holdFire || c.altFire || c.holdAlt || c.melee || c.stasis || c.stomp; }
// One brain tick plus a walk of up to `step` metres toward where it wants to go.
Command StepTick(Brain& b, Perception& p, float step = 0.4f) {
    const Command c = b.Tick(p);
    if (c.move) {
        const Vec3 to = {c.moveTo.x - p.self.x, 0.0f, c.moveTo.z - p.self.z};
        const float len = Length(to);
        if (len > 0.001f) p.self = Add(p.self, Scale(to, (len < step ? len : step) / len));
    }
    p.now += 100;
    return c;
}

// Hold: it stays on its spot whatever the leader does, still fights from there,
// backs off what comes too close and comes back, and still heals.
void HoldKeepsItsSpotAndFights() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.orders.hold = true;
    p.orders.holdSpot = {0, 0, 0};
    p.leader = {14, 0, 0};
    p.leaderMoving = true;
    p.leaderVel = {3, 0, 0};
    Brain b;
    for (int i = 0; i < 60; ++i) {
        const Command c = StepTick(b, p);
        assert(c.mode != Mode::Rush);
        assert(!c.move || Flat(c.moveTo, p.orders.holdSpot) < 1.0f);
    }
    assert(Flat(p.self, p.orders.holdSpot) < 0.5f);
    // A door to run to: a holding squad stays.
    p.progression = true;
    p.progressionAt = {14, 0, 3};
    assert(b.Tick(p).mode != Mode::Rush);
    p.progression = false;
    // An enemy in front: it fights from the spot.
    p.enemies[0] = Slasher(301, {0, 0, 8});
    p.enemyCount = 1;
    bool fired = false;
    for (int i = 0; i < 40; ++i) {
        const Command c = StepTick(b, p);
        if (c.mode == Mode::Combat) assert(c.target == 301);
        fired = fired || c.fire;
        assert(!c.move || Flat(c.moveTo, p.orders.holdSpot) <= b.tune.holdLeash + 0.5f);
    }
    assert(fired);
    assert(Flat(p.self, p.orders.holdSpot) < 1.5f);
    // It comes too close: a step back, never far from the spot ...
    p.enemies[0].pos = {p.self.x, 0, p.self.z + 2.0f};
    p.enemies[0].chest = Add(p.enemies[0].pos, {0, 1.3f, 0});
    Command c = b.Tick(p);
    assert(c.move && c.why == MoveWhy::Retreat);
    assert(Flat(c.moveTo, p.enemies[0].pos) > Flat(p.self, p.enemies[0].pos));
    assert(Flat(c.moveTo, p.orders.holdSpot) <= b.tune.holdLeash);
    // ... and back to the spot once it is gone.
    p.enemyCount = 0;
    p.self = {0, 0, -3};
    p.now += 5000;
    c = b.Tick(p);
    assert(c.move && Flat(c.moveTo, p.orders.holdSpot) < 0.01f);
    // Hurt: the med pack, as ever.
    p.self = p.orders.holdSpot;
    p.health01 = 0.3f;
    c = b.Tick(p);
    assert(c.heal);
    // Follow again: back to the leader.
    p.health01 = 1.0f;
    p.orders.hold = false;
    p.now += 5000;
    bool toLeader = false;
    for (int i = 0; i < 20; ++i) {
        c = StepTick(b, p);
        toLeader = toLeader || (c.move && Flat(c.moveTo, p.leader) < 4.0f);
    }
    assert(toLeader);
}

// Go there: it walks (runs, when far) to the point, then holds there while the
// leader walks off.
void GoThereWalksThenHolds() {
    Perception p = Calm();
    p.orders.hold = true;
    p.orders.holdSpot = {15, 0, 4};
    Brain b;
    Command c = b.Tick(p);
    assert(c.move && c.run && Flat(c.moveTo, p.orders.holdSpot) < 0.01f);
    for (int i = 0; i < 200 && Flat(p.self, p.orders.holdSpot) > b.tune.holdArriveStop; ++i) StepTick(b, p, 0.5f);
    assert(Flat(p.self, p.orders.holdSpot) <= b.tune.holdArriveStop + 0.01f);
    const Vec3 arrived = p.self;
    p.leader = {-10, 0, 5};
    p.leaderMoving = true;
    p.leaderVel = {-3, 0, 0};
    for (int i = 0; i < 50; ++i) {
        c = StepTick(b, p);
        assert(!c.move || Flat(c.moveTo, p.orders.holdSpot) < 1.0f);
    }
    assert(Flat(p.self, arrived) < 0.5f);
    // Under fire on the way: far from the spot it runs there with the gun down.
    Perception q = Armed(Gun::PlasmaCutter);
    q.orders.hold = true;
    q.orders.holdSpot = {25, 0, 0};
    q.enemies[0] = Slasher(310, {0, 0, 8});
    q.enemyCount = 1;
    Brain d;
    c = d.Tick(q);
    assert(c.mode == Mode::Combat && c.move && c.run && Flat(c.moveTo, q.orders.holdSpot) < 0.01f && !c.fire);
}

// The leader's target: fought first, ahead of a nearer one, unless something is
// right on the bot; after the time is up the usual choice is back.
void ForcedTargetOverridesAndExpires() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(401, {0, 0, 5});
    p.enemies[1] = Slasher(402, {0, 0, 14});
    p.enemyCount = 2;
    {
        Brain b;
        assert(b.Tick(p).target == 401);   // nearest, without an order
    }
    p.orders.focus = 402;
    p.orders.focusUntil = p.now + 20000;
    Brain b;
    Command c = b.Tick(p);
    assert(c.mode == Mode::Combat && c.target == 402 && c.focused);
    // Something on the bot: that first.
    p.enemies[2] = Slasher(403, {1.5f, 0, 0.5f});
    p.enemyCount = 3;
    c = b.Tick(p);
    assert(c.target == 403 && !c.focused);
    p.enemyCount = 2;
    p.now += 100;
    assert(b.Tick(p).target == 402);
    // Out of sight: the usual choice meanwhile.
    p.enemies[1].visible = false;
    p.now += 100;
    assert(b.Tick(p).target == 401);
    p.enemies[1].visible = true;
    // The time is up.
    p.now = p.orders.focusUntil;
    for (int i = 0; i < 30; ++i) { c = b.Tick(p); p.now += 100; }
    assert(c.target == 401 && !c.focused);
}

// Hold fire: aims, never shoots, swings, stomps or slows; something attacking
// it or the leader is fought back until it is gone.
void HoldFireBlocksButReturnsFire() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.orders.holdFire = true;
    p.enemies[0] = Slasher(501, {0, 0, 8});
    p.enemyCount = 1;
    Brain b;
    bool held = false, aimed = false;
    for (int i = 0; i < 40; ++i) {
        const Command c = b.Tick(p);
        assert(!Attacks(c) && c.holdFireActive && !c.returnFire);
        held = held || c.fireHeld;
        aimed = aimed || c.aim;
        p.now += 100;
    }
    assert(held && aimed);
    // It comes for the bot: fire back.
    p.enemies[0].pos = {0, 0, 3};
    p.enemies[0].chest = {0, 1.3f, 3};
    p.enemies[0].limbs[0] = {0.4f, 1.4f, 3};
    p.enemies[0].limbs[1] = {-0.4f, 1.4f, 3};
    p.enemies[0].limbs[2] = {0.2f, 0.5f, 3};
    bool fought = false;
    for (int i = 0; i < 30; ++i) {
        const Command c = b.Tick(p);
        assert(!c.holdFireActive);
        fought = fought || Attacks(c);
        p.now += 100;
    }
    assert(fought && b.Provoker() == 501);
    // It backs off: still the one that attacked, still fought.
    p.enemies[0] = Slasher(501, {0, 0, 9});
    p.now += 10000;
    fought = false;
    for (int i = 0; i < 20; ++i) { const Command c = b.Tick(p); fought = fought || c.fire; p.now += 100; }
    assert(fought);
    // Gone: a new one far off is left alone again.
    p.enemies[0] = Slasher(502, {0, 0, 9});
    p.now += 10000;
    for (int i = 0; i < 20; ++i) { const Command c = b.Tick(p); assert(!Attacks(c)); p.now += 100; }
    // A hit on the leader, or a hold on him, frees it too.
    p.leaderHealth01 = 0.8f;
    Command c = b.Tick(p);
    assert(c.returnFire && !c.holdFireActive);
    p.now += 20000;
    for (int i = 0; i < 5; ++i) { b.Tick(p); p.now += 100; }
    p.enemies[0].id = 503;   // a fresh face, so no provoker is left
    p.now += 20000;
    c = b.Tick(p);
    assert(c.holdFireActive);
    p.leaderGrabbed = true;
    c = b.Tick(p);
    assert(c.returnFire && !c.holdFireActive);
    // The leader's own target is always fair game.
    Perception q = Armed(Gun::PlasmaCutter);
    q.orders.holdFire = true;
    q.orders.focus = 601;
    q.orders.focusUntil = q.now + 20000;
    q.enemies[0] = Slasher(601, {0, 0, 8});
    q.enemyCount = 1;
    Brain f;
    bool shot = false;
    for (int i = 0; i < 20; ++i) { c = f.Tick(q); shot = shot || c.fire; q.now += 100; }
    assert(shot);
}

// Attack formation: its place is ahead of the leader (his walk, else his
// facing), the squad spread sideways; in a fight it goes in front of him toward
// the enemy and closes in.
void VanguardLeadsTheWay() {
    Perception p = Calm();
    p.orders.vanguard = true;
    p.leader = {0, 0, 0};
    p.leaderYaw = 0.0f;   // facing +z
    p.self = {0, 0, -3};
    Brain b;
    Command c = b.Tick(p);
    assert(c.move && c.moveTo.z >= b.tune.vanguardAhead - 0.01f);
    // Walking to +x: ahead along his walk.
    p.leaderVel = {3, 0, 0};
    p.leader = {5, 0, 0};
    c = b.Tick(p);
    assert(c.move && c.moveTo.x > p.leader.x + 2.5f);
    // A squad of two: one each side.
    Perception s = Calm();
    s.orders.vanguard = true;
    s.leader = {0, 0, 0};
    s.squadSize = 2;
    s.squadIndex = 1;
    Brain second;
    c = second.Tick(s);
    assert(c.move && c.moveTo.z > 2.5f && std::fabs(c.moveTo.x) > 1.0f);
    // Cover (the default) keeps it beside or behind him.
    Perception k = Calm();
    k.leader = {0, 0, 0};
    k.self = {0, 0, -9};
    Brain cover;
    c = cover.Tick(k);
    assert(c.move && c.moveTo.z < 0.5f);
    // In a fight: in front of him toward the enemy, closer than in cover.
    Perception f = Armed(Gun::PlasmaCutter);
    f.orders.vanguard = true;
    f.leader = {0, 0, 0};
    f.self = {2, 0, -1};
    f.enemies[0] = Slasher(701, {0, 0, 14});
    f.enemyCount = 1;
    Brain v;
    bool ahead = false, chased = false;   // no walking at a target it sees
    for (int i = 0; i < 40; ++i) {
        c = StepTick(v, f);
        ahead = ahead || (c.move && c.moveTo.z > f.leader.z + 1.0f);
        chased = chased || c.why == MoveWhy::Chase;
    }
    assert(ahead && !chased);
    assert(f.self.z > f.leader.z + 1.0f);
    assert(Flat(f.self, f.leader) < v.tune.combatFollow);
}

// --- looting and the rescue -------------------------------------------------------

LootSpot Spot(uint32_t id, LootKind kind, Vec3 at, bool botKill = false) {
    LootSpot s;
    s.id = id;
    s.kind = kind;
    s.pos = at;
    s.botKill = botKill;
    return s;
}

// A corpse close by: stomped only once nothing is about and things stayed calm a
// moment, never with the squad's stomps used up, never one by the leader's feet,
// never out of range; the walk there gets interrupted by a new enemy.
void StompsCorpsesWhenCalm() {
    Perception p = Calm();
    p.loot[0] = Spot(801, LootKind::Corpse, {0, 0, 4});
    p.lootCount = 1;
    {
        Brain off;   // the switch is off
        for (int i = 0; i < 30; ++i) assert(StepTick(off, p).mode != Mode::Loot);
    }
    Brain b;
    b.tune.lootWanted = true;
    // A live one about: no looting, and none for a moment after it is gone.
    p.enemies[0] = Slasher(802, {0, 0, 15});
    p.enemyCount = 1;
    for (int i = 0; i < 5; ++i) assert(StepTick(b, p).mode != Mode::Loot);
    p.enemyCount = 0;
    const uint32_t calmFrom = p.now;
    Command c;
    while (p.now - calmFrom < b.tune.lootCalmMs - 100) { c = StepTick(b, p); assert(c.mode != Mode::Loot); }
    for (int i = 0; i < 5 && c.mode != Mode::Loot; ++i) c = StepTick(b, p);
    assert(c.mode == Mode::Loot && c.loot == 801 && c.move);
    // At the corpse it stomps, the runtime counts the stomps, and the limit holds.
    int stomps = 0;
    bool left = false;
    for (int i = 0; i < 200 && !left; ++i) {
        c = StepTick(b, p);
        if (c.stomp) {
            assert(c.mode == Mode::Loot && c.loot == 801 && !c.aim);
            assert(Flat(p.self, p.loot[0].pos) <= b.tune.stompLootReach + 0.01f);
            p.loot[0].stomps = ++stomps;
        }
        left = c.mode != Mode::Loot;
    }
    assert(left && stomps == b.tune.corpseStompsMax);
    for (int i = 0; i < 30; ++i) assert(!StepTick(b, p).stomp);
    // Another squad member's stomps count too: a corpse already at the limit is left alone.
    Brain fresh;
    fresh.tune.lootWanted = true;
    Perception q = Calm();
    q.loot[0] = Spot(803, LootKind::Corpse, {0, 0, 2});
    q.loot[0].stomps = fresh.tune.corpseStompsMax;
    q.lootCount = 1;
    for (int i = 0; i < 30; ++i) assert(StepTick(fresh, q).mode != Mode::Loot);
    // The leader stands on it (his own stomp), or it is out of reach: left alone.
    Perception r = Calm();
    r.loot[0] = Spot(804, LootKind::Corpse, {2.5f, 0, 0.5f});
    r.loot[1] = Spot(805, LootKind::Corpse, {0, 0, 25});
    r.lootCount = 2;
    Brain s;
    s.tune.lootWanted = true;
    for (int i = 0; i < 30; ++i) assert(StepTick(s, r).mode != Mode::Loot);
    // Walking there, an enemy turns up: the fight comes first, at once.
    Perception w = Armed(Gun::PlasmaCutter);
    w.loot[0] = Spot(806, LootKind::Corpse, {0, 0, 6});
    w.lootCount = 1;
    Brain t;
    t.tune.lootWanted = true;
    w.now += 5000;
    c = StepTick(t, w);
    assert(c.mode == Mode::Loot);
    w.enemies[0] = Slasher(807, {5, 0, 5});
    w.enemyCount = 1;
    c = StepTick(t, w);
    assert(c.mode == Mode::Combat && c.target == 807 && !c.loot);
}

// A crate up on a wall is swung at from beside it, never stomped; one on the
// floor is stomped, never swung at (06.10.).
void SwingsAtAWallBox() {
    const bool kinds[] = {true, false};
    for (const bool wall : kinds) {
        Perception p = Calm();
        p.loot[0] = Spot(931, LootKind::Crate, {-3, wall ? 1.0f : 0.0f, 0});
        p.lootCount = 1;
        Brain b;
        b.tune.lootWanted = true;
        int swings = 0, stomps = 0;
        for (int i = 0; i < 120; ++i) {
            const Command c = StepTick(b, p);
            if (c.melee) {
                assert(c.mode == Mode::Loot && c.loot == 931);
                assert(Flat(p.self, p.loot[0].pos) <= b.tune.wallCrateReach + 0.01f);
                ++swings;
            }
            if (c.stomp) ++stomps;
            p.loot[0].stomps = swings + stomps;
        }
        if (wall) assert(swings > 0 && swings <= b.tune.wallCrateHitsMax && stomps == 0);
        else assert(stomps > 0 && swings == 0);
    }
}

// Half the crates are shot open from range with a plain gun in hand, the same
// half for every bot; the other half are stomped. Hold fire: none is shot.
void ShootsHalfTheCrates() {
    const uint32_t shotId = 0x900, stompedId = 0x930;
    {
        Perception p = Armed(Gun::PlasmaCutter);
        p.loot[0] = Spot(shotId, LootKind::Crate, {-5, 0, 0});
        p.loot[0].inSight = true;
        p.lootCount = 1;
        Brain b;
        b.tune.lootWanted = true;
        int shots = 0;
        for (int i = 0; i < 80; ++i) {
            const Command c = StepTick(b, p);
            assert(!c.stomp && !c.melee);
            if (c.mode == Mode::Loot && c.loot == shotId) assert(c.aim && !c.move);
            if (c.fire) ++shots;
            p.loot[0].stomps = shots;
        }
        assert(shots > 0 && shots <= b.tune.crateShotsMax);
        assert(Flat(p.self, p.loot[0].pos) > 4.9f);   // never walked up to it
    }
    {
        Perception p = Armed(Gun::PlasmaCutter);
        p.loot[0] = Spot(stompedId, LootKind::Crate, {-5, 0, 0});
        p.loot[0].inSight = true;
        p.lootCount = 1;
        Brain b;
        b.tune.lootWanted = true;
        bool stomped = false;
        for (int i = 0; i < 80; ++i) {
            const Command c = StepTick(b, p);
            assert(!c.fire);
            stomped = stomped || c.stomp;
        }
        assert(stomped);
    }
    {
        Perception p = Armed(Gun::PlasmaCutter);
        p.orders.holdFire = true;
        p.loot[0] = Spot(shotId, LootKind::Crate, {-5, 0, 0});
        p.loot[0].inSight = true;
        p.lootCount = 1;
        Brain b;
        b.tune.lootWanted = true;
        for (int i = 0; i < 80; ++i) assert(!StepTick(b, p).fire);
    }
}

// A nest is shot only at its pods; with none in sight it is aimed at, not shot.
void ShootsANestsPods() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(941, {0, 0, 8});
    p.enemies[0].kind = EnemyKind::Nest;
    p.enemies[0].health = p.enemies[0].maxHealth = 3000.0f;
    p.enemies[0].limbCount = 0;
    p.enemyCount = 1;
    Brain b;
    for (int i = 0; i < 40; ++i) {
        const Command c = StepTick(b, p);
        assert(!c.fire);
    }
    assert(b.nestHolds > 0);
    p.enemies[0].limbs[0] = {0.5f, 1.0f, 8.0f};
    p.enemies[0].limbKind[0] = LimbKind::Arm;
    p.enemies[0].limbCount = 1;
    bool fired = false;
    for (int i = 0; i < 60 && !fired; ++i) {
        const Command c = StepTick(b, p);
        if (c.fire) { fired = true; assert(Distance(c.lookAt, p.enemies[0].limbs[0]) < 0.01f); }
    }
    assert(fired);
}

// The Guardian: its tentacles first, its body never shot without a point on it.
void ShootsTheGuardiansTentacles() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(951, {0, 1.5f, 6});
    p.enemies[0].kind = EnemyKind::Guardian;
    p.enemies[0].health = p.enemies[0].maxHealth = 30000.0f;
    p.enemies[0].limbCount = 0;
    p.enemies[1] = Slasher(952, {1.5f, 1.0f, 7});
    p.enemies[1].kind = EnemyKind::GuardianTentacle;
    p.enemies[1].health = p.enemies[1].maxHealth = 40.0f;
    p.enemies[1].limbs[0] = {1.5f, 1.2f, 6.5f};
    p.enemies[1].limbCount = 1;
    p.enemyCount = 2;
    Brain b;
    bool firedAtTentacle = false;
    for (int i = 0; i < 60; ++i) {
        const Command c = StepTick(b, p);
        if (c.target) assert(c.target == 952);
        firedAtTentacle = firedAtTentacle || (c.fire && c.target == 952);
    }
    assert(firedAtTentacle);
    // Tentacles gone: the body is aimed at, never shot without a point on it.
    p.enemies[1].health = 0.0f;
    for (int i = 0; i < 40; ++i) assert(!StepTick(b, p).fire);
}

// Crates and lockers: the nearest first, its own kills before the leader's, a
// mate's spot left to the mate; a locker gets use presses only while the game
// offers it, a few at most.
void LootsContainersInOrder() {
    Perception p = Calm();
    p.loot[0] = Spot(901, LootKind::Locker, {0, 0, 6});
    p.loot[1] = Spot(902, LootKind::Crate, {-3, 0, 0});
    p.lootCount = 2;
    Brain b;
    b.tune.lootWanted = true;
    Command c = StepTick(b, p);
    assert(c.mode == Mode::Loot && c.loot == 902);   // the nearer one
    bool stomped = false;
    for (int i = 0; i < 60 && !stomped; ++i) { c = StepTick(b, p); stomped = c.stomp && c.loot == 902; }
    assert(stomped);
    // The crate broke (the runtime drops it from the list): on to the locker.
    p.loot[0] = Spot(901, LootKind::Locker, {0, 0, 6});
    p.lootCount = 1;
    c = StepTick(b, p);
    assert(c.mode == Mode::Loot && c.loot == 901 && c.move && !c.use);
    for (int i = 0; i < 60 && c.move; ++i) c = StepTick(b, p);
    assert(!c.use);   // there, but not offered yet
    p.lootOffered = 901;
    int presses = 0;
    for (int i = 0; i < 100; ++i) {
        c = StepTick(b, p);
        if (c.use) { assert(c.loot == 901 && c.mode == Mode::Loot); ++presses; }
    }
    assert(presses == b.tune.lockerTries);   // then it gives up on a locker that never opens
    // On the way to a crate an enemy shows up: no more looting until it is gone and things calm down.
    Perception a = Armed(Gun::PlasmaCutter);
    a.loot[0] = Spot(921, LootKind::Crate, {0, 0, 7});
    a.lootCount = 1;
    Brain ab;
    ab.tune.lootWanted = true;
    c = StepTick(ab, a);
    assert(c.mode == Mode::Loot && c.loot == 921);
    a.enemies[0] = Slasher(922, {-6, 0, 6});
    a.enemyCount = 1;
    c = StepTick(ab, a);
    assert(c.mode == Mode::Combat && !c.loot && !c.stomp);
    a.enemyCount = 0;
    c = StepTick(ab, a);
    assert(c.mode != Mode::Loot);   // calm first
    for (int i = 0; i < 20; ++i) c = StepTick(ab, a);
    assert(c.mode == Mode::Loot && c.loot == 921);
    // Its own kill comes before a nearer one of the leader's.
    Perception k = Calm();
    k.loot[0] = Spot(911, LootKind::Corpse, {0, 0, 4});
    k.loot[1] = Spot(912, LootKind::Corpse, {0, 0, -5}, true);
    k.lootCount = 2;
    Brain own;
    own.tune.lootWanted = true;
    assert(StepTick(own, k).loot == 912);
    // A mate on it: the other one.
    k.mates[0].id = 77;
    k.mates[0].pos = {0, 0, -4};
    k.mates[0].loot = 912;
    k.mateCount = 1;
    Brain other;
    other.tune.lootWanted = true;
    assert(StepTick(other, k).loot == 911);
}

// The leader in a grab: the grabber at once, over the bot's nearer enemy, the
// forced target, hold fire and a full focus cap; back to normal once he is free.
void RescuesTheGrabbedLeader() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, 6};
    p.enemies[0] = Slasher(701, {2, 0, 2});          // nearer to the bot
    p.enemies[1] = Slasher(702, {0.5f, 0, 6.5f});    // on the leader
    p.enemyCount = 2;
    p.orders.holdFire = true;
    p.orders.focus = 701;
    p.orders.focusUntil = p.now + 60000;
    p.mates[0].id = 51;
    p.mates[0].pos = {-2, 0, 4};
    p.mates[0].target = 702;                         // a slasher's cap is one: full
    p.mateCount = 1;
    p.loot[0] = Spot(703, LootKind::Corpse, {1, 0, 1});
    p.lootCount = 1;
    Brain b;
    b.tune.lootWanted = true;
    b.tune.aimAcquireMs = 600;
    Command c = b.Tick(p);
    assert(c.target == 701 && !c.rescue);
    p.leaderGrabbed = true;
    p.leaderGrabber = 702;
    bool attacked = false;
    for (int i = 0; i < 20; ++i) {
        p.now += 100;
        c = b.Tick(p);
        assert(c.mode == Mode::Combat && c.target == 702 && c.rescue && !c.holdFireActive && !c.focused);
        attacked = attacked || Attacks(c);
    }
    assert(attacked);
    // The grabber out of sight (a rail or pane in the way): it runs at him and never
    // shoots into what is in between; close enough, it swings.
    {
        Brain blind;
        Perception q = p;
        for (int i = 0; i < q.enemyCount; ++i) if (q.enemies[i].id == 702) q.enemies[i].visible = false;
        bool ran = false;
        for (int i = 0; i < 20; ++i) {
            q.now += 100;
            const Command d = blind.Tick(q);
            assert(d.rescue && d.target == 702 && !d.fire && !d.holdFire);
            ran = ran || (d.move && d.run);
        }
        assert(ran && blind.rescueCloseIns > 0);
    }
    // Too far off to help: it keeps to its own.
    Brain far;
    Perception f = p;
    f.self = {0, 0, -30};
    f.leader = {0, 0, -28};
    f.enemies[1].pos = {0, 0, f.self.z + far.tune.rescueRange + 2.0f};
    assert(!far.Tick(f).rescue);
    // Free again: the rescue ends.
    p.leaderGrabbed = false;
    p.leaderGrabber = 0;
    p.now += 100;
    c = b.Tick(p);
    assert(!c.rescue && c.target == 701);
}

// At rest before a wall: turns the most open way (opposite first on a tie), not
// into the leader, not at all in the open or boxed in; on a hold spot no step
// that would leave it.
void TurnsAwayFromAWall() {
    FaceOpenInput in;
    in.selfYaw = 0.0f;
    in.count = FaceCandidates(in.selfYaw, in.yaws);
    for (int i = 0; i < in.count; ++i) in.clear[i] = 4.0f;
    in.self = {0, 0, 0};
    in.leader = {10, 0, 10};
    // Open ahead: nothing.
    in.wallAhead = -1.0f;
    assert(!DecideFaceOpen(in).turn);
    in.wallAhead = 2.0f;
    assert(!DecideFaceOpen(in).turn);
    // A wall 0.6 m ahead, every way equally open: the opposite way.
    in.wallAhead = 0.6f;
    FaceOpenOutput out = DecideFaceOpen(in);
    assert(out.turn && out.pick == 0 && std::fabs(out.yaw - 3.1415927f) < 0.01f && out.step);
    // A side clearly more open wins.
    in.clear[0] = 2.0f;
    in.clear[3] = 4.0f;
    in.clear[1] = in.clear[2] = in.clear[4] = 1.0f;
    out = DecideFaceOpen(in);
    assert(out.turn && out.pick == 3);
    // The mesh ends ahead counts like a wall.
    in.wallAhead = -1.0f;
    in.navShort = true;
    assert(DecideFaceOpen(in).turn);
    in.navShort = false;
    in.wallAhead = 0.6f;
    // The leader stands right there that way: the next best.
    in.leader = FromYaw(in.yaws[3]);
    out = DecideFaceOpen(in);
    assert(out.turn && out.pick == 0);
    // Everything closed: stays.
    for (int i = 0; i < in.count; ++i) in.clear[i] = 0.8f;
    assert(!DecideFaceOpen(in).turn);
    // Holding a spot: turns, but no step that would leave it by more than 0.5 m.
    for (int i = 0; i < in.count; ++i) in.clear[i] = 4.0f;
    in.leader = {10, 0, 10};
    in.holding = true;
    in.holdSpot = {0, 0, 0.3f};
    out = DecideFaceOpen(in);
    assert(out.turn && !out.step);
    in.holdSpot = {0, 0, 0};
    out = DecideFaceOpen(in);
    assert(out.turn && out.step);
}

// --- antics ---------------------------------------------------------------------
// A calm scene: the leader stands at the origin facing +z, the bot 3.6 m off.
Perception AnticCalm() {
    Perception p = Calm();
    p.now = 100000;
    p.leader = {0, 0, 0};
    p.leaderYaw = 0.0f;
    p.self = {3, 0, -2};
    p.selfYaw = 0.0f;
    p.selfYawKnown = true;
    return p;
}
// The bot walks where it is told at 1.5 m/s, its body turning along the walk;
// its aim settles at once.
void AnticWalk(Perception& p, const Command& c, uint32_t dt) {
    p.now += dt;
    if (c.move) {
        Vec3 to = Sub(c.moveTo, p.self);
        to.y = 0.0f;
        const float len = Length(to), most = 1.5f * static_cast<float>(dt) / 1000.0f;
        if (len > 0.001f) {
            p.self = Add(p.self, Scale(to, (len < most ? len : most) / len));
            p.selfYaw = std::atan2(to.x, to.z);
        }
    }
    p.aimErrorDeg = 0.0f;
}
uint32_t AnticStarts(const Brain& b) {
    uint32_t n = 0;
    for (int k = 0; k < kAntics; ++k) n += b.AnticCounts().started[k];
    return n;
}

// Antics=0 (the default): never one, whatever else would fit.
void NoAnticsWhenOff() {
    Brain b;
    b.tune.harmfulAntics = true;
    Perception p = AnticCalm();
    p.killPos = {2, 0, 1};
    p.leaderLine = 1;
    assert(!b.ForceAntic(p, Antic::Stare));
    for (int i = 0; i < 20 * 60 * 20; ++i) {   // 20 minutes
        p.killId = 77 + static_cast<uint32_t>(i / 2000);
        p.killAt = p.now;
        const Command c = b.Tick(p);
        assert(c.antic == Antic::None && b.AnticNow() == Antic::None);
        assert(!c.stasis && !c.melee && !c.stomp && !c.aim);
        AnticWalk(p, c, 50);
    }
    assert(AnticStarts(b) == 0);
}

// Each level keeps its gap between two antics of a bot, counted from the end of
// the last one (and from the first calm frame for the first).
void AnticsKeepTheirGap() {
    for (int level = 1; level <= 3; ++level) {
        Brain b(0x1234u + static_cast<uint32_t>(level) * 7919u);
        b.tune.antics = level;
        Perception p = AnticCalm();
        const uint32_t first = p.now;
        uint32_t lastEnd = 0;
        int starts = 0, ends = 0;
        AnticEvent ev[8];
        for (int i = 0; i < 30 * 60 * 20; ++i) {   // 30 minutes
            const Command c = b.Tick(p);
            const int n = b.TakeAnticEvents(ev, 8);
            for (int k = 0; k < n; ++k) {
                if (ev[k].kind == AnticEvent::Start) {
                    ++starts;
                    assert(p.now - (lastEnd ? lastEnd : first) >= b.tune.anticGapMinMs[level - 1]);
                } else {
                    ++ends;
                    lastEnd = p.now;
                    assert(ev[k].kind == AnticEvent::Finish);   // nothing in this calm scene breaks one off
                }
            }
            AnticWalk(p, c, 50);
        }
        const int most = static_cast<int>(30u * 60u * 1000u / b.tune.anticGapMinMs[level - 1]) + 1;
        assert(starts >= 1 && starts <= most);
        assert(ends >= starts - 1);
        std::printf("antics level %d: %d in 30 min\n", level, starts);
    }
}

// Only one bot of the squad at a time; a mate busy with one holds the others back.
void OneAnticAtATime() {
    Brain a(0xA11CEu), b(0xB0B0u);
    a.tune.antics = b.tune.antics = 3;
    Perception pa = AnticCalm(), pb = AnticCalm();
    pb.self = {-3, 0, -1};
    pa.mateCount = pb.mateCount = 1;
    pa.squadSize = pb.squadSize = 2;
    pb.squadIndex = 1;
    int both = 0;
    for (int i = 0; i < 30 * 60 * 20; ++i) {
        pa.mates[0].pos = pb.self;
        pa.mates[0].antic = b.AnticNow() != Antic::None;
        const Command ca = a.Tick(pa);
        pb.mates[0].pos = pa.self;
        pb.mates[0].antic = a.AnticNow() != Antic::None;
        const Command cb = b.Tick(pb);
        if (a.AnticNow() != Antic::None && b.AnticNow() != Antic::None) ++both;
        AnticWalk(pa, ca, 50);
        AnticWalk(pb, cb, 50);
        pb.now = pa.now;
    }
    assert(both == 0);
    assert(AnticStarts(a) > 0 && AnticStarts(b) > 0);
    // A mate that is always busy: never one of its own.
    Brain c(0xC0FFEEu);
    c.tune.antics = 3;
    Perception pc = AnticCalm();
    pc.mateCount = 1;
    pc.mates[0].pos = {-3, 0, 0};
    pc.mates[0].antic = true;
    for (int i = 0; i < 10 * 60 * 20; ++i) AnticWalk(pc, c.Tick(pc), 50);
    assert(AnticStarts(c) == 0 && c.AnticCounts().rejected[static_cast<int>(AnticSkip::MateBusy)] > 0);
}

// An enemy turns up: the antic is over in that very frame and the bot fights;
// none starts again until things have been calm for a while.
void AnticBreaksOffForAThreat() {
    Brain b;
    b.tune.antics = 3;
    Perception p = AnticCalm();
    assert(b.ForceAntic(p, Antic::Stare));
    for (int i = 0; i < 20; ++i) AnticWalk(p, b.Tick(p), 50);
    assert(b.AnticNow() == Antic::Stare);
    p.enemies[0] = Slasher(9, {0, 0, 8});
    p.enemyCount = 1;
    const Command c = b.Tick(p);
    assert(b.AnticNow() == Antic::None && c.antic == Antic::None);
    assert(c.mode == Mode::Combat && c.target == 9);
    AnticEvent ev[8];
    const int n = b.TakeAnticEvents(ev, 8);
    assert(n == 2 && ev[1].kind == AnticEvent::Abort && ev[1].antic == Antic::Stare);
    assert(b.AnticCounts().aborted[static_cast<int>(Antic::Stare)] == 1);
    // Gone again: still too soon for another.
    p.enemyCount = 0;
    p.now += 3000;
    b.Tick(p);
    assert(!b.ForceAntic(p, Antic::AimSpam));
    p.now += b.tune.anticCalmMs;
    b.Tick(p);
    assert(b.ForceAntic(p, Antic::AimSpam));
    // The runtime's own business (a tube, a scene) ends it too.
    p.busy = "tube";
    b.Tick(p);
    assert(b.AnticNow() == Antic::None);
}

// Aim spam: in front of the leader's face, the aim tapped 75 ms down, 75 ms up
// for 2 s, never a shot; stare: 3 s of looking at his face without a step.
void AimSpamTapsAndStareStares() {
    Brain b;
    b.tune.antics = 2;
    Perception p = AnticCalm();
    const Vec3 head = {0, 1.6f, 0};
    assert(b.ForceAntic(p, Antic::AimSpam));
    bool aimWas = false;
    uint32_t flipAt = 0, firstAim = 0, lastAimEnd = 0;
    int flips = 0;
    for (int i = 0; i < 4000 && b.AnticNow() == Antic::AimSpam; ++i) {
        const Command c = b.Tick(p);
        assert(!c.fire && !c.holdFire && !c.melee && !c.stasis && !c.altFire && !c.stomp);
        if (c.antic == Antic::AimSpam && c.aim) {
            assert(!c.move && Flat(p.self, {0, 0, 1.5f}) < 0.5f);
            assert(c.look && Distance(c.lookAt, head) < 0.01f);
            if (!firstAim) firstAim = p.now;
        }
        if (c.antic == Antic::AimSpam && c.aim != aimWas) {
            if (flipAt) assert(p.now - flipAt == 75);
            flipAt = p.now;
            ++flips;
            if (!c.aim) lastAimEnd = p.now;
        }
        if (c.antic == Antic::AimSpam) aimWas = c.aim;
        AnticWalk(p, c, 5);
    }
    assert(b.AnticNow() == Antic::None);
    assert(firstAim && lastAimEnd - firstAim >= 1850 && p.now - firstAim <= 2010);
    assert(flips >= 26 && b.AnticCounts().taps == 14);
    assert(b.AnticCounts().finished[static_cast<int>(Antic::AimSpam)] == 1);

    Brain s;
    s.tune.antics = 2;
    p = AnticCalm();
    assert(s.ForceAntic(p, Antic::Stare));
    uint32_t stareFrom = 0;
    for (int i = 0; i < 4000 && s.AnticNow() == Antic::Stare; ++i) {
        const Command c = s.Tick(p);
        assert(!c.aim && !c.fire && !c.melee && !c.stasis);
        if (s.AnticPhase() == 3 && c.antic == Antic::Stare) {
            if (!stareFrom) stareFrom = p.now;
            assert(!c.move && c.look && Distance(c.lookAt, head) < 0.01f && Flat(p.self, {0, 0, 1.2f}) < 0.5f);
        }
        AnticWalk(p, c, 10);
    }
    assert(stareFrom && p.now - stareFrom >= 3000 && s.AnticCounts().finished[static_cast<int>(Antic::Stare)] == 1);
    // The leader walks off: over.
    Brain m;
    m.tune.antics = 2;
    p = AnticCalm();
    assert(m.ForceAntic(p, Antic::Stare));
    for (int i = 0; i < 10; ++i) AnticWalk(p, m.Tick(p), 50);
    p.leader = {1.2f, 0, 0};
    m.Tick(p);
    assert(m.AnticNow() == Antic::None && m.AnticCounts().aborted[static_cast<int>(Antic::Stare)] == 1);
}

// Shadow boxing never swings with the leader within 2.5 m or before it, wherever
// he stands; with him right before it the whole time it swings at nothing.
void ShadowBoxingMissesTheLeader() {
    int swings = 0, skipped = 0;
    for (uint32_t seed = 1; seed <= 60; ++seed) {
        Brain b(seed * 2654435761u);
        b.tune.antics = 3;
        Perception p = AnticCalm();
        p.self = {0, 0, -4};
        p.leader = {0, 0, 0};
        assert(b.ForceAntic(p, Antic::ShadowBoxing));
        uint32_t rng = seed * 747796405u + 1u;
        auto next = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return static_cast<float>(rng & 0xFFFF) / 65536.0f; };
        for (int i = 0; i < 2000 && b.AnticNow() == Antic::ShadowBoxing; ++i) {
            if (i % 25 == 0) {   // the leader somewhere else around it every half second
                const float a = next() * 6.2831853f, r = 1.0f + next() * 4.5f;
                p.leader = {p.self.x + std::sin(a) * r, 0, p.self.z + std::cos(a) * r};
            }
            const Command c = b.Tick(p);
            assert(!c.fire && !c.stasis && !c.aim);
            if (c.melee) {
                ++swings;
                const float off = std::fabs(std::atan2(std::sin(std::atan2(p.leader.x - p.self.x, p.leader.z - p.self.z) - p.selfYaw),
                                                       std::cos(std::atan2(p.leader.x - p.self.x, p.leader.z - p.self.z) - p.selfYaw)));
                assert(Flat(p.self, p.leader) > 2.5f && off >= 75.0f * 0.01745329f);
            }
            AnticWalk(p, c, 20);
        }
        assert(b.AnticNow() == Antic::None);
        skipped += static_cast<int>(b.AnticCounts().swingsSkipped);
    }
    assert(swings > 0 && skipped > 0);
    // Right before it, 1.5 m off, from the first swing on: not one.
    Brain b;
    b.tune.antics = 3;
    Perception p = AnticCalm();
    p.self = {0, 0, -4};
    assert(b.ForceAntic(p, Antic::ShadowBoxing));
    int spins = 0;
    for (int i = 0; i < 2000 && b.AnticNow() == Antic::ShadowBoxing; ++i) {
        if (b.AnticPhase() >= 1) p.leader = Add(p.self, Scale(FromYaw(p.selfYaw), 1.5f));
        const Command c = b.Tick(p);
        assert(!c.melee);
        if (b.AnticPhase() == 0 && c.move) { assert(c.direct && Flat(c.moveTo, p.self) < 0.7f); ++spins; }
        AnticWalk(p, c, 20);
    }
    assert(spins > 0 && b.AnticCounts().swings == 0 && b.AnticCounts().swingsSkipped >= 2);
    assert(b.AnticCounts().rejected[static_cast<int>(AnticSkip::LeaderClose)] >= 2);
    // The leader too close to start with: not even begun.
    Brain c;
    c.tune.antics = 3;
    p = AnticCalm();
    p.self = {0, 0, -2};
    assert(!c.ForceAntic(p, Antic::ShadowBoxing));
}

// The stasis prank only with HarmfulAntics, energy and a clear line; one shot at
// the chest, then a look elsewhere. The victory stomp needs its own fresh kill.
void HarmfulAnticsOnlyWhenAllowed() {
    {
        Brain b;
        b.tune.antics = 3;
        Perception p = AnticCalm();
        p.leaderLine = 1;
        assert(!b.ForceAntic(p, Antic::StasisPrank));
        for (int i = 0; i < 30 * 60 * 20; ++i) {
            const Command c = b.Tick(p);
            assert(!c.stasis && b.AnticNow() != Antic::StasisPrank);
            AnticWalk(p, c, 50);
        }
        assert(b.AnticCounts().started[static_cast<int>(Antic::StasisPrank)] == 0);
    }
    {
        Brain b;
        b.tune.antics = 3;
        b.tune.harmfulAntics = true;
        Perception p = AnticCalm();
        p.leaderLine = 0;
        assert(!b.ForceAntic(p, Antic::StasisPrank) && b.AnticCounts().rejected[static_cast<int>(AnticSkip::NoLine)] == 1);
        p.leaderLine = 1;
        p.stasis01 = 0.1f;
        assert(!b.ForceAntic(p, Antic::StasisPrank) && b.AnticCounts().rejected[static_cast<int>(AnticSkip::NoStasis)] == 1);
        p.stasis01 = 1.0f;
        assert(b.ForceAntic(p, Antic::StasisPrank));
        int shots = 0;
        bool lookedAway = false;
        for (int i = 0; i < 2000 && b.AnticNow() == Antic::StasisPrank; ++i) {
            const Command c = b.Tick(p);
            if (c.stasis) {
                ++shots;
                const float d = Flat(p.self, p.leader);
                assert(c.aim && Distance(c.lookAt, {0, b.tune.anticChest, 0}) < 0.01f && d >= 2.5f && d <= 7.0f);
            }
            if (b.AnticPhase() == 3 && c.look) {
                const Vec3 to = Sub(c.lookAt, p.self), leader = Sub(p.leader, p.self);
                lookedAway = lookedAway || Dot({to.x, 0, to.z}, {leader.x, 0, leader.z}) < 0.0f;
            }
            AnticWalk(p, c, 20);
        }
        assert(shots == 1 && lookedAway && b.AnticCounts().stasisShots == 1);
        // Once in ten minutes at most.
        p.now += 60000;
        assert(!b.ForceAntic(p, Antic::StasisPrank) && b.AnticCounts().rejected[static_cast<int>(AnticSkip::Cooldown)] == 1);
        p.now += b.tune.anticRareMs;
        assert(b.ForceAntic(p, Antic::StasisPrank));
        // Squad-wide: a mate's prank a minute ago holds a bot that never pranked.
        Brain mate;
        mate.tune.antics = 3;
        mate.tune.harmfulAntics = true;
        Perception q = AnticCalm();
        q.leaderLine = 1;
        q.stasis01 = 1.0f;
        q.squadPrankAt = q.now - 60000;
        assert(!mate.ForceAntic(q, Antic::StasisPrank) && mate.AnticCounts().rejected[static_cast<int>(AnticSkip::Cooldown)] == 1);
        q.squadPrankAt = q.now - mate.tune.anticRareMs - 1;
        assert(mate.ForceAntic(q, Antic::StasisPrank));
    }
    {
        Brain b;
        b.tune.antics = 3;
        Perception p = AnticCalm();
        assert(!b.ForceAntic(p, Antic::VictoryStomp) && b.AnticCounts().rejected[static_cast<int>(AnticSkip::NoKill)] == 1);
        p.killId = 42;
        p.killAt = p.now - 12000;
        p.killPos = {2, 0, 1};
        assert(b.ForceAntic(p, Antic::VictoryStomp));
        int stomps = 0;
        bool lookedAtLeader = false;
        for (int i = 0; i < 2000 && b.AnticNow() == Antic::VictoryStomp; ++i) {
            const Command c = b.Tick(p);
            if (c.stomp) { ++stomps; assert(Flat(p.self, p.killPos) <= b.tune.stompLootReach + 0.01f && !c.aim); }
            if (b.AnticPhase() == 2 && c.look) lookedAtLeader = lookedAtLeader || Distance(c.lookAt, {0, 1.6f, 0}) < 0.01f;
            AnticWalk(p, c, 20);
        }
        assert(stomps >= 3 && stomps <= 4 && lookedAtLeader);
        assert(b.AnticCounts().finished[static_cast<int>(Antic::VictoryStomp)] == 1);
        p.killId = 43;
        p.killAt = p.now;
        assert(!b.ForceAntic(p, Antic::VictoryStomp) && b.AnticCounts().rejected[static_cast<int>(AnticSkip::Cooldown)] == 1);
    }
}

// --- 07.10.: cooldowns, mate lanes, backing off, aim -------------------------------

// The leader in the line for one tick: the stasis is held, not spent. Once he is
// out of the line it goes within a few ticks, not 8 s later.
void StasisNotSpentOnAHeldShot() {
    Perception p = Calm();   // guns unknown: it fights with what it holds
    Enemy big = Slasher(300, {0, 0, 6});
    big.health = big.maxHealth = 400.0f;   // dangerous: stasis first
    p.enemies[0] = big;
    p.enemyCount = 1;
    p.aimErrorDeg = 1.0f;
    p.leader = {0, 0, 2.5f};
    Brain b;
    Command c = b.Tick(p);
    assert(!c.stasis && c.laneBlocked);
    assert(b.heldShots == 1);
    p.leader = {3, 0, 0};
    bool cast = false;
    for (int i = 0; i < 3 && !cast; ++i) { p.now += 100; cast = b.Tick(p).stasis; }
    assert(cast);
    // A real cast starts the cooldown.
    bool again = false;
    for (int i = 0; i < 20; ++i) { p.now += 100; again = again || b.Tick(p).stasis; }
    assert(!again);
}

// The same for the plain shot: a held shot does not use up the fire interval.
void HeldShotKeepsTheFireInterval() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(301, {0, 0, 7});
    p.enemyCount = 1;
    p.leader = {0, 0, 3.5f};
    Brain b;
    Command c = b.Tick(p);
    assert(!c.fire && c.laneBlocked);
    p.leader = {3, 0, 0};
    p.now += 50;
    c = b.Tick(p);
    assert(c.fire);
}

// A mate between the bot and its target: no shot through it, and the bot waits
// for the mate to step out before it steps aside itself.
// An enemy behind a wall: the bot keeps its gun up but does not turn to it
// through the wall; once it is in sight again the bot looks at it.
void NoLookThroughAWall() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(330, {0, 0, 5});
    p.enemyCount = 1;
    Brain b;
    p.now += 100;
    assert(b.Tick(p).target == 330);
    p.enemies[0].visible = false;
    for (int i = 0; i < 5; ++i) {
        p.now += 100;
        const Command c = b.Tick(p);
        assert(!c.fire && (!c.look || c.lookAt.z < 1.0f));
    }
    p.enemies[0].visible = true;
    p.now += 100;
    const Command c = b.Tick(p);
    assert(c.target == 330 && c.look);
}

void NoShotThroughAMate() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(302, {0, 0, 7});
    p.enemyCount = 1;
    p.leader = {3, 0, 0};
    p.squadSize = 2;
    p.mates[0] = MateAt(910, {0, 0, 3});
    p.mateCount = 1;
    Brain b;
    bool early = false, stepped = false;
    for (int i = 0; i < 25; ++i) {
        p.now += 100;
        const Command c = b.Tick(p);
        assert(!c.fire && !c.holdFire && c.laneBlocked);
        const bool out = c.move && c.why == MoveWhy::Lane;
        early = early || (out && i < 10);
        stepped = stepped || out;
    }
    assert(!early && stepped && b.mateLaneHolds >= 25);
    p.mates[0] = MateAt(910, {-3, 0, 0});
    bool fired = false;
    for (int i = 0; i < 10; ++i) { p.now += 100; fired = fired || b.Tick(p).fire; }
    assert(fired);
}

// Standing in an aiming mate's line of fire: a step out of it, at once, and it
// keeps aiming at its own target meanwhile.
void StepsOutOfAMatesLine() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.enemies[0] = Slasher(303, {0, 0, 6});
    p.enemyCount = 1;
    p.leader = {2, 0, -1};
    p.squadSize = 2;
    p.squadIndex = 0;
    Mate m = MateAt(911, {0, 0, -3}, 303);
    m.aiming = true;
    m.aimAt = {0, 1.3f, 6};
    p.mates[0] = m;
    p.mateCount = 1;
    Brain b;
    Command c = b.Tick(p);
    assert(c.aim && c.target == 303);
    assert(c.move && c.why == MoveWhy::Lane && std::fabs(c.moveTo.x) >= b.tune.laneWidth);
    assert(b.mateLaneSteps == 1);
    // It holds that place a moment instead of stepping back into the line.
    const Vec3 to = c.moveTo;
    p.now += 100;
    c = b.Tick(p);
    assert(c.move && Distance(c.moveTo, to) < 0.01f);
    // Out of the line: nothing more to do.
    p.self = to;
    for (int i = 0; i < 10; ++i) { p.now += 100; c = b.Tick(p); }
    assert(b.mateLaneSteps == 1);
    // A mate that is not aiming moves nobody.
    Perception q = Armed(Gun::PlasmaCutter);
    q.enemies[0] = Slasher(304, {0, 0, 6});
    q.enemyCount = 1;
    q.leader = {2, 0, -1};
    q.squadSize = 2;
    q.mates[0] = MateAt(912, {0, 0, -3}, 304);
    q.mateCount = 1;
    Brain calm;
    assert(!calm.Tick(q).move);
}

// Two enemies within a few metres: it backs off them while it shoots, away from
// both, though neither is closer than its own keep distance.
void BacksOffACrowdWhileShooting() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, -2};
    p.enemies[0] = Slasher(310, {-1.0f, 0, 4});
    p.enemies[1] = Slasher(311, {1.0f, 0, 4});
    p.enemyCount = 2;
    Brain b;
    bool fired = false, backed = false;
    for (int i = 0; i < 6; ++i) {
        p.now += 100;
        const Command c = b.Tick(p);
        assert(c.aim);
        if (c.move && c.backOff) {
            backed = true;
            assert(c.why == MoveWhy::Retreat && c.moveTo.z < p.self.z - 0.5f);
        }
        fired = fired || c.fire;
    }
    assert(backed && fired && b.backOffs > 0);
    // With both rules off and nobody inside the keep distance it stands.
    Perception q = p;
    Brain off;
    off.tune.crowdCount = 0;
    off.tune.backOffRange = 0.0f;
    bool moved = false;
    for (int i = 0; i < 6; ++i) {
        q.now += 100;
        const Command c = off.Tick(q);
        moved = moved || (c.move && c.why == MoveWhy::Retreat);
    }
    assert(!moved);
}

// An enemy that is not its target close by (a mate fights it): it backs off that
// one too, away from it, not only from its own target.
void BacksOffAnEnemyThatIsNotItsTarget() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {0, 0, -2};
    p.enemies[0] = Slasher(320, {2.0f, 0, 0.5f});   // on the bot's side, the mate's
    p.enemies[1] = Slasher(321, {0, 0, 8});          // its own
    p.enemyCount = 2;
    p.squadSize = 2;
    p.mates[0] = MateAt(913, {4, 0, 0}, 320);
    p.mateCount = 1;
    Brain b;
    const Command c = b.Tick(p);
    assert(c.target == 321);
    assert(c.move && c.backOff && c.moveTo.x < p.self.x - 0.5f);
}

// The limb list comes sorted anew every refresh: the aim stays on its limb by
// where it is on the body, not by its place in the list.
void AimStaysOnItsLimbWhenTheListReorders() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {3, 0, 0};
    p.enemies[0] = Slasher(330, {0, 0, 7});
    p.enemyCount = 1;
    p.aimErrorDeg = 20.0f;   // no shot: only the aim
    Brain b;
    Command c = b.Tick(p);
    const Vec3 first = c.lookAt;
    // The same limbs, another order.
    Enemy& e = p.enemies[0];
    const Vec3 l0 = e.limbs[0], l1 = e.limbs[1], l2 = e.limbs[2];
    const LimbKind k0 = e.limbKind[0], k1 = e.limbKind[1], k2 = e.limbKind[2];
    e.limbs[0] = l2; e.limbKind[0] = k2;
    e.limbs[1] = l0; e.limbKind[1] = k0;
    e.limbs[2] = l1; e.limbKind[2] = k1;
    for (int i = 0; i < 5; ++i) {
        p.now += 100;
        c = b.Tick(p);
        assert(Distance(c.lookAt, first) < 0.01f);
    }
    // After a while the next limb, and only one switch.
    p.now += 2600;
    c = b.Tick(p);
    assert(Distance(c.lookAt, first) > 0.1f && b.limbSwitches == 1);
}

// No limb to aim at: the shot at the body may be as far off as the body is wide.
void ChestShotHasTheBodysTolerance() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {3, 0, 0};
    Enemy e = Slasher(340, {0, 0, 6});
    e.limbCount = 0;
    p.enemies[0] = e;
    p.enemyCount = 1;
    Brain b;
    p.aimErrorDeg = 2.1f;   // beyond a limb's 1.7 deg at 6 m, within the body's 2.4
    const Command c = b.Tick(p);
    assert(Distance(c.lookAt, e.chest) < 0.01f && c.fire);
}

// A moving aim point: the brain tells the runtime how fast it moves, so the look
// can lead it (a climb is vertical, so the vertical part counts too).
void ReportsTheAimPointsSpeed() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {3, 0, 0};
    p.aimErrorDeg = 20.0f;
    Brain b;
    Command c;
    for (int i = 0; i < 10; ++i) {
        p.enemies[0] = Slasher(350, {0.2f * static_cast<float>(i), 0.1f * static_cast<float>(i), 7});
        p.enemyCount = 1;
        p.now += 100;
        c = b.Tick(p);
    }
    assert(std::fabs(c.lookVel.x - 2.0f) < 0.3f && std::fabs(c.lookVel.y - 1.0f) < 0.2f);
    // It stops: the speed goes back to nothing.
    for (int i = 0; i < 5; ++i) { p.now += 100; c = b.Tick(p); }
    assert(Length(c.lookVel) < 0.01f);
}

// --- 08.10.: human senses and aim ---------------------------------------------------

Perception FacingNorth() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {3, 0, 0};
    p.selfId = 77;
    p.viewYaw = 0.0f;   // looking toward +z
    p.viewYawKnown = true;
    return p;
}

// A still enemy behind the bot with a clear ray is not seen; once it moves it is
// heard, and the bot turns to it after its reaction, not at once.
void EnemyBehindIsHeardFirst() {
    Perception p = FacingNorth();
    p.enemies[0] = Slasher(400, {0, 0, -6});
    p.enemyCount = 1;
    Brain b;
    b.tune.humanSenses = true;
    for (int i = 0; i < 10; ++i) {
        p.now += 100;
        const Command c = b.Tick(p);
        assert(c.target != 400 && !c.fire);
    }
    assert(b.fovHidden >= 10 && b.heardTurns == 0);
    p.enemies[0].vel = {0, 0, 1.0f};
    const uint32_t start = p.now;
    uint32_t turnedAt = 0;
    Command c;
    for (int i = 0; i < 30; ++i) {
        p.now += 50;
        c = b.Tick(p);
        if (c.target == 400 && !turnedAt) turnedAt = p.now;
    }
    assert(turnedAt && turnedAt - start >= b.tune.hearReactMinMs && turnedAt - start <= b.tune.hearReactMaxMs + 100);
    assert(c.target == 400 && c.look && c.lookAt.z < -3.0f && b.heardTurns == 1);
    // Without the switch the same enemy is fought at once.
    Perception q = FacingNorth();
    q.enemies[0] = Slasher(401, {0, 0, -6});
    q.enemyCount = 1;
    Brain old;
    q.now += 100;
    assert(old.Tick(q).target == 401);
}

// Heard behind a wall: alert, but never a look or a target through the wall.
void HeardBehindAWallIsNotLookedAt() {
    Perception p = FacingNorth();
    p.enemies[0] = Slasher(402, {0, 0, -6});
    p.enemies[0].vel = {0, 0, 1.0f};
    p.enemies[0].visible = false;
    p.enemyCount = 1;
    Brain b;
    b.tune.humanSenses = true;
    for (int i = 0; i < 40; ++i) {
        p.now += 50;
        const Command c = b.Tick(p);
        assert(c.target != 402 && !c.fire);
        assert(!c.look || c.lookAt.z > -1.0f);
    }
    assert(b.heardTurns == 0 && b.fovHidden == 0);
}

// Very close it is noticed whichever way the bot looks; a little farther, not.
void PeripheralIsAlwaysSeen() {
    Perception p = FacingNorth();
    p.enemies[0] = Slasher(403, {0.5f, 0, -1.5f});
    p.enemyCount = 1;
    Brain b;
    b.tune.humanSenses = true;
    p.now += 100;
    assert(b.Tick(p).target == 403);
    Perception q = FacingNorth();
    q.enemies[0] = Slasher(404, {0.5f, 0, -3.0f});
    q.enemyCount = 1;
    Brain far;
    far.tune.humanSenses = true;
    q.now += 100;
    assert(far.Tick(q).target != 404 && far.fovHidden == 1);
    // In the cone at an angle, farther off: seen.
    Perception r = FacingNorth();
    r.enemies[0] = Slasher(405, {5.0f, 0, 8.0f});   // 32 deg right
    r.enemyCount = 1;
    Brain cone;
    cone.tune.humanSenses = true;
    r.now += 100;
    assert(cone.Tick(r).target == 405);
}

// Hurt by something behind it: a short reaction, then it turns to it.
void HitMakesItTurn() {
    Perception p = FacingNorth();
    p.enemies[0] = Slasher(406, {0.5f, 0, -3.5f});
    p.enemyCount = 1;
    Brain b;
    b.tune.humanSenses = true;
    for (int i = 0; i < 5; ++i) { p.now += 50; assert(b.Tick(p).target != 406); }
    p.health01 = 0.8f;
    p.now += 50;
    assert(b.Tick(p).target != 406);   // not in the same frame
    bool turned = false;
    for (int i = 0; i < 10 && !turned; ++i) { p.now += 50; turned = b.Tick(p).target == 406; }
    assert(turned && b.hitTurns == 1 && b.heardTurns == 0);
}

// A new target: no look for a moment, then a flick that lands off and settles
// within a few hundred ms; the first shot waits for it.
void AimAcquisitionHasADelayAndConverges() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {3, 0, 0};
    p.viewYaw = 1.2f;   // looking 70 deg to the right of it
    p.viewYawKnown = true;
    p.enemies[0] = Slasher(410, {0, 0, 7});
    p.enemyCount = 1;
    Brain b;
    b.tune.humanAim = true;
    const uint32_t start = p.now;
    uint32_t firstLook = 0, firstShot = 0;
    float startOff = 0.0f, endOff = 99.0f, peakAfterLook = 0.0f;
    for (int i = 0; i < 70; ++i) {
        p.now += 16;
        const Command c = b.Tick(p);
        if (c.look && !firstLook) { firstLook = p.now; startOff = b.AimOffsetDeg(); }
        if (c.look) {
            float near = 99.0f;
            for (int l = 0; l < p.enemies[0].limbCount; ++l) near = std::fmin(near, Distance(c.lookAt, p.enemies[0].limbs[l]));
            near = std::fmin(near, Distance(c.lookAt, p.enemies[0].chest));
            if (p.now - firstLook < 50) peakAfterLook = std::fmax(peakAfterLook, near);
            endOff = b.AimOffsetDeg();
        }
        if (c.fire && !firstShot) firstShot = p.now;
    }
    assert(firstLook > start + 16);
    assert(startOff > 1.0f && peakAfterLook > 0.1f);
    assert(endOff < 0.8f);
    assert(firstShot && firstShot > firstLook && firstShot - start <= 900);
    assert(b.acquisitions == 1 && b.settles == 1 && b.overshoots + b.undershoots == 1);
    assert(b.acquireMsSum == firstShot - (start + 16));   // from the frame it took the target
    std::printf("aim acquisition: look after %u ms, first shot after %u ms, settled after %u ms, %.1f deg off at the flick\n", firstLook - start,
                firstShot - start, b.settleMsSum, startOff);
}

// A steady fight against a strafing enemy, a second one after five seconds: the
// human aim and senses cost a shot or two, not the steady fire (07.10.: bots that
// hardly fired).
int SteadyFightShots(bool human) {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {3, 0, 0};
    p.viewYawKnown = true;
    Brain b;
    b.tune.humanAim = human;
    b.tune.humanSenses = human;
    int shots = 0;
    for (int i = 0; i < 200; ++i) {
        p.now += 50;
        const float t = static_cast<float>(i) * 0.05f;
        const uint32_t id = i < 100 ? 420u : 421u;
        p.enemies[0] = Slasher(id, {2.0f * std::sin(t), 0, 7});
        p.enemies[0].vel = {2.0f * std::cos(t), 0, 0};
        p.enemyCount = 1;
        const Command c = b.Tick(p);
        if (c.look) p.viewYaw = std::atan2(c.lookAt.x - p.self.x, c.lookAt.z - p.self.z);   // the camera follows
        if (c.fire) ++shots;
    }
    if (human) assert(b.acquisitions == 2 && b.aimWaits == 0);
    return shots;
}
void SteadyFightKeepsFiring() {
    const int before = SteadyFightShots(false), now = SteadyFightShots(true);
    std::printf("steady fight shots: %d without, %d with human aim\n", before, now);
    assert(before >= 18 && now * 10 >= before * 8);
}

// A twitcher dodges a quarter second into an aim and not again for 1.5 s or more: the
// bot shoots at once instead of pausing and settling first, and its cutter blade is not
// turned before the shot (18.10.).
uint32_t FirstShotMs(bool dodger, bool snap) {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {3, 0, 0};
    p.viewYaw = 1.2f;
    p.viewYawKnown = true;
    p.aimErrorDeg = 0.5f;
    p.enemies[0] = Slasher(dodger ? 430u : 431u, {0, 0, 7});
    p.enemies[0].kind = EnemyKind::Slasher;
    p.enemies[0].dodger = dodger;
    p.enemyCount = 1;
    Brain b;
    b.tune.humanAim = true;
    b.tune.aimAcquireMs = 600;
    b.tune.dodgerSnap = snap;
    const uint32_t start = p.now;
    for (int i = 0; i < 120; ++i) {
        p.now += 16;
        const Command c = b.Tick(p);
        if (c.fire) {
            if (dodger && snap) assert(b.dodgerSnaps == 1 && b.dodgerFrames >= 1 && b.dodgerShots == 1);
            if (!dodger) assert(b.slasherFrames >= 1 && b.slasherShots == 1 && !b.dodgerFrames);
            return p.now - start;
        }
    }
    return 0;
}
void SnapsAtATwitcher() {
    const uint32_t plain = FirstShotMs(false, true), twitcher = FirstShotMs(true, true), off = FirstShotMs(true, false);
    std::printf("first shot: %u ms at a slasher, %u ms at a twitcher, %u ms with TwitcherSnapShots=0\n", plain, twitcher, off);
    assert(twitcher && twitcher <= 250);
    assert(plain >= 600 && off >= 600);
}

// Stasis first on twitchers and charging stalkers (user 18.10.: "definitely more stasis"):
// a charging stalker is picked before a nearer slasher and slowed even with little energy,
// one standing off is not; a twitcher gets the next cast 2.5 s later, not 8 s.
void StasisFirstOnTwitchersAndStalkers() {
    Perception p = Armed(Gun::PlasmaCutter);
    p.leader = {3, 0, 0};
    p.aimErrorDeg = 0.5f;
    p.stasis01 = 0.1f;
    p.enemies[0] = Slasher(440, {0, 0, 8});
    p.enemies[0].kind = EnemyKind::Slasher;
    p.enemies[1] = Slasher(441, {-1, 0, 11});
    p.enemies[1].kind = EnemyKind::Stalker;
    p.enemies[1].vel = {0, 0, -6};   // coming at the bot
    p.enemyCount = 2;
    Brain b;
    Command c = b.Tick(p);
    assert(c.target == 441 && c.stasis);
    assert(b.stasisOnStalkers == 1 && !b.stasisOnTwitchers);
    // Standing off at 11 m: an ordinary target, no stasis on so little energy.
    Brain b2;
    p.enemies[1].vel = {0, 0, 0};
    c = b2.Tick(p);
    assert(c.target == 440 && !c.stasis && !b2.stasisOnStalkers);
    // Off by the switch: the stalker is just a stalker.
    Brain b3;
    b3.tune.stasisPriority = false;
    p.enemies[1].vel = {0, 0, -6};
    c = b3.Tick(p);
    assert(c.target == 440 && !c.stasis);
    // A twitcher, the first one slowed: the next cast comes 2.5 s later.
    Perception q = Armed(Gun::PlasmaCutter);
    q.leader = {3, 0, 0};
    q.aimErrorDeg = 0.5f;
    q.enemies[0] = Slasher(450, {0, 0, 8});
    q.enemies[0].kind = EnemyKind::Slasher;
    q.enemies[0].dodger = true;
    q.enemyCount = 1;
    Brain t;
    c = t.Tick(q);
    assert(c.target == 450 && c.stasis && t.stasisOnTwitchers == 1);
    q.enemies[0].stasised = true;
    q.enemies[1] = Slasher(451, {1, 0, 9});
    q.enemies[1].kind = EnemyKind::Slasher;
    q.enemies[1].dodger = true;
    q.enemyCount = 2;
    bool cast = false;
    uint32_t castAt = 0;
    const uint32_t start = q.now;
    for (int i = 0; i < 300 && !cast; ++i) {
        q.now += 16;
        c = t.Tick(q);
        if (c.stasis) { cast = true; castAt = q.now - start; }
    }
    std::printf("priority stasis: second twitcher slowed %u ms after the first\n", castAt);
    std::fflush(stdout);
    assert(cast && castAt >= 2500 && castAt < 3500 && t.stasisOnTwitchers == 2);
}

// Power node doors: every refusal is told apart, in a fixed order, and a bot with
// its own node goes once the leader has stood near the panel a moment.
NodeDoorFacts ReadyDoor() {
    NodeDoorFacts f;
    f.nodes = 2;
    f.required = 1;
    f.selfDist = 8.0f;
    f.leaderDist = 3.0f;
    f.leaderNearMs = 5000;
    return f;
}
void OpensANodeDoorOnlyWhenItMay() {
    const UsableTune t;
    assert(JudgeNodeDoor(ReadyDoor(), t) == DoorVerdict::Go);
    NodeDoorFacts f = ReadyDoor();
    f.busy = true;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::Busy);
    f = ReadyDoor();
    f.enabled = false;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::Disabled);
    f = ReadyDoor();
    f.on = true;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::AlreadyOn);
    f = ReadyDoor();
    f.inUse = true;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::InUse);
    f = ReadyDoor();
    f.leaderAtIt = true;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::LeaderAtIt);
    f = ReadyDoor();
    f.waitingOut = true;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::WaitingOut);
    f = ReadyDoor();
    f.failures = t.maxFailures;
    f.sinceFailureMs = t.retryMs * 10;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::GaveUp);
    f = ReadyDoor();
    f.failures = 1;
    f.sinceFailureMs = t.retryMs - 1;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::Cooldown);
    f.sinceFailureMs = t.retryMs;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::Go);
    // Its own nodes only: none, or fewer than the panel asks for.
    f = ReadyDoor();
    f.nodes = 0;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::NoNodes);
    f.nodes = 1;
    f.required = 2;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::NoNodes);
    f.required = 0;   // a panel whose price could not be read: never
    assert(JudgeNodeDoor(f, t) == DoorVerdict::NoNodes);
    f = ReadyDoor();
    f.calm = false;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::Enemies);
    f = ReadyDoor();
    f.otherBot = true;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::OtherBot);
    f = ReadyDoor();
    f.selfDist = t.doorReachMeters + 0.5f;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::TooFar);
    f = ReadyDoor();
    f.selfDy = -(t.floorMeters + 0.5f);
    assert(JudgeNodeDoor(f, t) == DoorVerdict::TooFar);
    f = ReadyDoor();
    f.leaderDist = t.doorLeaderMeters + 0.5f;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::LeaderFar);
    f = ReadyDoor();
    f.leaderDy = t.floorMeters + 0.5f;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::LeaderFar);
    f = ReadyDoor();
    f.leaderNearMs = t.doorWaitMs - 1;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::LeaderNotSettled);
    // The leader at the panel outranks everything the bot could do about it.
    f = ReadyDoor();
    f.leaderAtIt = true;
    f.nodes = 0;
    f.calm = false;
    assert(JudgeNodeDoor(f, t) == DoorVerdict::LeaderAtIt);
    for (unsigned i = 0; i < static_cast<unsigned>(DoorVerdict::Count); ++i) assert(DoorVerdictName(static_cast<DoorVerdict>(i))[0] != '?');
}

// Stasis stations: low on stasis, calm, and the station ready; after the leader.
StationFacts ReadyStation() {
    StationFacts f;
    f.energy01 = 0.2f;
    f.charge = 999.0f;
    f.selfDist = 5.0f;
    f.leaderDist = 4.0f;
    return f;
}
void UsesAStasisStationOnlyWhenItMay() {
    const UsableTune t;
    assert(JudgeStation(ReadyStation(), t) == StationVerdict::Go);
    StationFacts f = ReadyStation();
    f.busy = true;
    assert(JudgeStation(f, t) == StationVerdict::Busy);
    f = ReadyStation();
    f.stasisModule = false;
    assert(JudgeStation(f, t) == StationVerdict::NoModule);
    f = ReadyStation();
    f.energy01 = t.stationBelow;
    assert(JudgeStation(f, t) == StationVerdict::EnergyOk);
    f = ReadyStation();
    f.energyKnown = false;
    assert(JudgeStation(f, t) == StationVerdict::EnergyOk);
    f = ReadyStation();
    f.airStation = true;
    assert(JudgeStation(f, t) == StationVerdict::AirStation);
    f = ReadyStation();
    f.state = 0;
    assert(JudgeStation(f, t) == StationVerdict::Locked);
    // The leader charging, or about to: the bot waits its turn.
    f = ReadyStation();
    f.targetSet = f.leaderTarget = true;
    f.state = 3;
    assert(JudgeStation(f, t) == StationVerdict::LeaderUsing);
    f = ReadyStation();
    f.leaderAtIt = true;
    assert(JudgeStation(f, t) == StationVerdict::LeaderUsing);
    for (int s = 2; s <= 4; ++s) {
        f = ReadyStation();
        f.state = s;
        assert(JudgeStation(f, t) == StationVerdict::Charging);
    }
    f = ReadyStation();
    f.targetSet = true;
    assert(JudgeStation(f, t) == StationVerdict::Charging);
    for (int s = 5; s <= 7; ++s) {
        f = ReadyStation();
        f.state = s;
        assert(JudgeStation(f, t) == StationVerdict::Downtime);
    }
    f = ReadyStation();
    f.downtime = 3.0f;
    assert(JudgeStation(f, t) == StationVerdict::Downtime);
    f = ReadyStation();
    f.charge = 0.0f;
    assert(JudgeStation(f, t) == StationVerdict::Empty);
    f = ReadyStation();
    f.failures = t.maxFailures;
    assert(JudgeStation(f, t) == StationVerdict::GaveUp);
    f = ReadyStation();
    f.failures = 1;
    f.sinceFailureMs = 10;
    assert(JudgeStation(f, t) == StationVerdict::Cooldown);
    f = ReadyStation();
    f.calm = false;
    assert(JudgeStation(f, t) == StationVerdict::Enemies);
    f = ReadyStation();
    f.otherBot = true;
    assert(JudgeStation(f, t) == StationVerdict::OtherBot);
    f = ReadyStation();
    f.selfDist = t.stationMeters + 1.0f;
    assert(JudgeStation(f, t) == StationVerdict::TooFar);
    f = ReadyStation();
    f.leaderDist = t.stationMeters + 1.0f;
    assert(JudgeStation(f, t) == StationVerdict::LeaderFar);
    for (unsigned i = 0; i < static_cast<unsigned>(StationVerdict::Count); ++i) assert(StationVerdictName(static_cast<StationVerdict>(i))[0] != '?');
}

int main() {
    OpensANodeDoorOnlyWhenItMay();
    UsesAStasisStationOnlyWhenItMay();
    TurnsAwayFromAWall();
    LoitersNearTheLeader();
    FollowsAndRuns();
    RushesToProgression();
    ShootsLimbsOnTarget();
    CyclesLimbs();
    MeleesWhenClose();
    RetreatStaysOnTheLeash();
    StasisOnDangerous();
    ReloadsWhenEmpty();
    HealsAtSixtyPercent();
    PicksUpNearbyItems();
    GivesUpOnItems();
    MashesWhenGrabbed();
    IgnoresInvisibleEnemies();
    WalksThePath();
    ShadowsWithoutAPath();
    TrailSpacing();
    TeleportsWhenHopeless();
    IdleSteer();
    StopsShortOfAWall();
    CutterTurnsForLegs();
    LineGunThroughARow();
    MineLeadsThePack();
    JavelinShocksOnce();
    LeavesACrippledUbermorph();
    HoldsFireWhileItRegrows();
    BreaksAwayToTheLeader();
    KitesBackShooting();
    DesperateBlasts();
    NoGunSwapsInACrowd();
    RunsBackWhenFarBehind();
    FocusDependsOnKindAndCount();
    PulseInBursts();
    GrenadeNeedsTwentyFive();
    RipperThrows();
    SeekerZoomsFirst();
    DetonatorInTheirWay();
    ContactChargesAndReleases();
    ForceBeamHoldsTheAim();
    FlameCanisterNeedsFuel();
    SlamsWhenSurrounded();
    NoBurstsNearPlayers();
    FightsFromTheLeadersSide();
    HoldsWhenTheWayIsDangerous();
    StompsSmallOneUnderfoot();
    SpreadsOverTargets();
    FocusesTheBigOne();
    CoversAMate();
    NoShotThroughTheLeader();
    SquadTakesBothSides();
    KeepsRoomFromMates();
    LeavesAMatesItem();
    GivesUpWithoutProgress();
    KeepsOffABlockedSpot();
    KeepsThePartnerPose();
    FacesThePartner();
    PartnerPlaceInAWall();
    SecondPartnerPlace();
    ComesInFacingHim();
    PicksBenchNodes();
    SwitchesFromAnEmptyGun();
    PulseAimsAtLimbs();
    CombatKeepsItsOwnPace();
    ShootsTheObjective();
    FollowsInAFight();
    FollowsWithAnEnemyOnIt();
    UbermorphComesLast();
    HoldsWhileLeaderShuffles();
    NoticesAfterItsReaction();
    BotsSwitchApart();
    ScansAfterAFight();
    AimsBeforeItShoots();
    TakesItsOwnPlace();
    UnarmedKeepsAway();
    IgnoresTheLeadersShuffle();
    SwingsFacingTheChest();
    MakesWayForTheLeader();
    FliesTowardItsPlace();
    HoldsWhileTheLeaderMovesInAFight();
    PlayingDeadBehindAWallHoldsNobody();
    UnseenThreatDoesNotFreeze();
    JoinsAMatesFight();
    JoinsWhenTheLeaderShoots();
    LooksPastALeaderRightBeside();
    HangsOnTheLeaderInAChase();
    LeavesTheLeadersLoot();
    MashesWhenHeld();
    HoldKeepsItsSpotAndFights();
    GoThereWalksThenHolds();
    ForcedTargetOverridesAndExpires();
    HoldFireBlocksButReturnsFire();
    VanguardLeadsTheWay();
    StompsCorpsesWhenCalm();
    LootsContainersInOrder();
    SwingsAtAWallBox();
    ShootsHalfTheCrates();
    ShootsANestsPods();
    ShootsTheGuardiansTentacles();
    RescuesTheGrabbedLeader();
    NoAnticsWhenOff();
    AnticsKeepTheirGap();
    OneAnticAtATime();
    AnticBreaksOffForAThreat();
    AimSpamTapsAndStareStares();
    ShadowBoxingMissesTheLeader();
    HarmfulAnticsOnlyWhenAllowed();
    StasisNotSpentOnAHeldShot();
    HeldShotKeepsTheFireInterval();
    NoShotThroughAMate();
    NoLookThroughAWall();
    StepsOutOfAMatesLine();
    BacksOffACrowdWhileShooting();
    BacksOffAnEnemyThatIsNotItsTarget();
    AimStaysOnItsLimbWhenTheListReorders();
    ChestShotHasTheBodysTolerance();
    ReportsTheAimPointsSpeed();
    EnemyBehindIsHeardFirst();
    HeardBehindAWallIsNotLookedAt();
    PeripheralIsAlwaysSeen();
    HitMakesItTurn();
    AimAcquisitionHasADelayAndConverges();
    SteadyFightKeepsFiring();
    SnapsAtATwitcher();
    StasisFirstOnTwitchersAndStalkers();
    std::puts("coop_bot_brain_tests: all passed");
    return 0;
}
