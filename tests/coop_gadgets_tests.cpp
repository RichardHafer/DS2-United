// The stasis mine's and heal field's arithmetic without a game: reach, target
// filtering, stasis values, heal clamping, durations, charges and key actions.
#include <cassert>
#include <cmath>
#include <cstdio>
#include "../src/coop_gadgets.h"

namespace {
using namespace coop_gadgets;

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

void Sanitizing() {
    MineConfig m;
    m.triggerMeters = 50.0f; m.radiusMeters = 1.0f; m.rechargeSeconds = -4.0f; m.slowPercent = 150.0f; m.durationSeconds = std::nanf("");
    const MineConfig s = Sanitized(m);
    assert(s.triggerMeters == 10.0f && s.radiusMeters == 10.0f);   // the burst never smaller than the trigger
    assert(s.rechargeSeconds == 0.0f && s.slowPercent == 99.0f && s.durationSeconds == 0.0f);
    FieldConfig f;
    f.durationSeconds = 0.0f; f.ratePercent = 500.0f; f.tickMs = 1; f.radiusMeters = std::nanf("");
    const FieldConfig g = Sanitized(f);
    assert(g.durationSeconds == 1.0f && g.ratePercent == 100.0f && g.tickMs == 50 && g.radiusMeters == 0.5f);
    const MineConfig d = Sanitized(MineConfig{});
    assert(d.triggerMeters == 2.5f && d.radiusMeters == 4.0f && d.rechargeSeconds == 40.0f);
    const FieldConfig e = Sanitized(FieldConfig{});
    assert(e.durationSeconds == 12.0f && e.radiusMeters == 3.0f && e.ratePercent == 8.0f && e.rechargeSeconds == 60.0f);
    assert(Ms(12.0f) == 12000 && Ms(0.0f) == 0 && Ms(-1.0f) == 0 && Ms(0.0015f) == 2);
    std::printf("sanitizing ok\n");
}

void Reach() {
    const V3 c{0, 0, 0};
    assert(Within(c, {2.4f, 0, 0}, 2.5f, 2.0f));
    assert(Within(c, {0, 1.9f, 2.5f}, 2.5f, 2.0f));      // flat distance counts, the edge is in
    assert(!Within(c, {2.6f, 0, 0}, 2.5f, 2.0f));
    assert(!Within(c, {0, 2.1f, 0.5f}, 2.5f, 2.0f));     // the floor above
    assert(!Within(c, {0, -2.1f, 0.5f}, 2.5f, 2.0f));    // the floor below
    assert(!Within(c, {std::nanf(""), 0, 0}, 2.5f, 2.0f));
    assert(!Within({std::nanf(""), 0, 0}, {0, 0, 0}, 2.5f, 2.0f));
    std::printf("reach ok\n");
}

Body Enemy(uint32_t id, float x, float health = 50.0f) {
    Body b;
    b.id = id;
    b.pos = {x, 0, 0};
    b.health = health;
    return b;
}

void Filtering() {
    assert(StasisTarget(Enemy(1, 0)));
    Body b = Enemy(1, 0);
    b.health = 0.0f; assert(!StasisTarget(b));                  // dead
    b = Enemy(1, 0); b.health = std::nanf(""); assert(!StasisTarget(b));
    b = Enemy(1, 0); b.player = true; assert(!StasisTarget(b));
    b = Enemy(1, 0); b.bot = true; assert(!StasisTarget(b));
    b = Enemy(1, 0); b.neutral = true; assert(!StasisTarget(b));
    b = Enemy(1, 0); b.objective = true; assert(!StasisTarget(b));
    b = Enemy(1, 0); b.ghost = true; assert(!StasisTarget(b));
    b = Enemy(1, 0); b.hidden = true; assert(!StasisTarget(b));
    b = Enemy(0, 0); assert(!StasisTarget(b));
    std::printf("filtering ok\n");
}

void TriggerAndBurst() {
    const MineConfig c = Sanitized(MineConfig{});
    const V3 mine{0, 0, 0};
    Body bodies[6] = {Enemy(1, 3.0f), Enemy(2, 3.9f), Enemy(3, 5.0f), Enemy(4, 1.0f), Enemy(5, 2.0f), Enemy(6, 3.5f)};
    bodies[3].player = true;     // a player standing on it never sets it off
    bodies[4].health = 0.0f;     // nor a corpse
    assert(!Triggered(mine, bodies, 6, c));
    bodies[5].pos = {2.4f, 0.5f, 0};   // a live one steps in
    assert(Triggered(mine, bodies, 6, c));
    int out[6];
    const int n = InBurst(mine, bodies, 6, c, out, 6);
    assert(n == 3 && out[0] == 0 && out[1] == 1 && out[2] == 5);   // 3.0, 3.9 and 2.4 m; 5 m and the player and the corpse stay out
    assert(InBurst(mine, bodies, 6, c, out, 2) == 2);               // capped
    Body above = Enemy(7, 1.0f);
    above.pos.y = 3.0f;          // on the floor above
    assert(!Triggered(mine, &above, 1, c));
    std::printf("trigger and burst ok\n");
}

void StasisNumbers() {
    MineConfig c;
    StasisValues v = Stasis(c, 95.0f, 8.0f);
    assert(v.fromGame && Near(v.slowPercent, 95.0f) && Near(v.durationSeconds, 8.0f));
    v = Stasis(c, std::nanf(""), -1.0f);
    assert(!v.fromGame && Near(v.slowPercent, 95.0f) && Near(v.durationSeconds, 8.0f));   // unreadable: retail numbers
    v = Stasis(c, 250.0f, 1000.0f);
    assert(!v.fromGame && Near(v.slowPercent, 95.0f) && Near(v.durationSeconds, 8.0f));
    c.durationSeconds = 4.0f;
    c.slowPercent = 50.0f;
    v = Stasis(c, 95.0f, 8.0f);
    assert(!v.fromGame && Near(v.slowPercent, 50.0f) && Near(v.durationSeconds, 4.0f));
    std::printf("stasis numbers ok\n");
}

void HealClamping() {
    Patient p;
    p.id = 1; p.health = 50.0f; p.max = 100.0f;
    assert(Near(HealAmount(p, 8.0f, 1000), 8.0f));
    assert(Near(HealAmount(p, 8.0f, 250), 2.0f));
    p.health = 99.0f;
    assert(Near(HealAmount(p, 8.0f, 1000), 1.0f));      // never past max
    p.health = 100.0f;
    assert(HealAmount(p, 8.0f, 1000) == 0.0f);
    p.health = 120.0f;
    assert(HealAmount(p, 8.0f, 1000) == 0.0f);          // above max stays as it is
    p.health = 0.0f;
    assert(HealAmount(p, 8.0f, 1000) == 0.0f);          // no revival
    p.health = 50.0f; p.dead = true;
    assert(HealAmount(p, 8.0f, 1000) == 0.0f);          // dead by flag
    p.dead = false; p.local = false;
    assert(HealAmount(p, 8.0f, 1000) == 0.0f);          // a partner's copy
    p.local = true; p.max = 0.0f;
    assert(HealAmount(p, 8.0f, 1000) == 0.0f);
    p.max = 100.0f;
    assert(HealAmount(p, 8.0f, 0) == 0.0f);             // a paused tick
    // A whole field from 10 health: 12 s at 8 %/s in 250 ms steps reaches 100, never more.
    p.health = 10.0f;
    for (int i = 0; i < 48; ++i) p.health += HealAmount(p, 8.0f, 250);
    assert(Near(p.health, 100.0f));
    p.health = 50.0f;
    for (int i = 0; i < 48; ++i) p.health += HealAmount(p, 1.0f, 250);
    assert(Near(p.health, 62.0f));
    FieldConfig c;
    p.pos = {2.9f, 0.0f, 0.0f};
    assert(InField({0, 0, 0}, p, c));
    p.pos = {3.1f, 0.0f, 0.0f};
    assert(!InField({0, 0, 0}, p, c));
    std::printf("heal clamping ok\n");
}

void Clocks() {
    Clock k;
    assert(k.Step(1000) == 0 && !k.paused);   // first sample
    assert(k.Step(1016) == 16 && !k.paused);
    assert(k.Step(1016) == 0 && k.paused);    // the clock stands: paused
    assert(k.Step(900) == 0);                 // backwards
    assert(k.Step(900 + 70000) == 0);         // a load
    assert(k.Step(900 + 70033) == 33);
    assert(k.Step(0) == 0 && !k.known);
    Countdown d;
    d.Start(12000);
    assert(d.SecondsLeft() == 12);
    assert(!d.Advance(11500) && d.SecondsLeft() == 1);
    assert(d.Advance(600) && d.left == 0);
    assert(!d.Advance(100));                  // reached zero once only
    d.Start(1);
    assert(d.SecondsLeft() == 1);
    std::printf("clocks ok\n");
}

void Charges() {
    Charge c;
    assert(c.state == ChargeState::Ready && c.Take() && c.state == ChargeState::Out);
    assert(!c.Take());                        // one charge
    c.Refund();
    assert(c.state == ChargeState::Ready);    // picked up unused: back at once
    assert(c.Take());
    c.Spend(40000);
    assert(c.state == ChargeState::Recharging && c.SecondsLeft() == 40);
    assert(!c.Take());
    c.Refund();
    assert(c.state == ChargeState::Recharging);   // a refund never skips a recharge
    assert(!c.Advance(0) && !c.Advance(39000) && c.SecondsLeft() == 1);
    assert(c.Advance(1000) && c.state == ChargeState::Ready && c.SecondsLeft() == 0);
    assert(!c.Advance(5000));
    assert(c.Take());
    c.Spend(0);
    assert(c.state == ChargeState::Ready);    // no recharge time
    Charge r;
    r.Spend(1000);
    assert(r.state == ChargeState::Ready);    // nothing out, nothing spent
    std::printf("charges ok\n");
}

void KeyActions() {
    const MineConfig m = Sanitized(MineConfig{});
    assert(MineAction(false, 0.0f, true, ChargeState::Ready, m) == MinePress::Place);
    assert(MineAction(false, 0.0f, false, ChargeState::Ready, m) == MinePress::NotNow);
    assert(MineAction(false, 0.0f, true, ChargeState::Recharging, m) == MinePress::Recharging);
    assert(MineAction(false, 0.0f, false, ChargeState::Recharging, m) == MinePress::Recharging);
    assert(MineAction(true, 3.0f, true, ChargeState::Out, m) == MinePress::PickUp);
    assert(MineAction(true, 3.0f, false, ChargeState::Out, m) == MinePress::PickUp);   // picking up needs no ready world
    assert(MineAction(true, 5.0f, true, ChargeState::Out, m) == MinePress::TooFar);
    MineConfig anywhere = m;
    anywhere.pickupRange = 0.0f;
    assert(MineAction(true, 500.0f, true, ChargeState::Out, anywhere) == MinePress::PickUp);
    assert(FieldAction(false, true, ChargeState::Ready) == FieldPress::Place);
    assert(FieldAction(true, true, ChargeState::Out) == FieldPress::Running);
    assert(FieldAction(false, true, ChargeState::Recharging) == FieldPress::Recharging);
    assert(FieldAction(false, false, ChargeState::Ready) == FieldPress::NotNow);
    Edge e;
    assert(e.Pressed(true) && !e.Pressed(true) && !e.Pressed(false) && e.Pressed(true));
    std::printf("key actions ok\n");
}

// A mine's life in game time: placed, armed, set off, recharged.
void MineLife() {
    const MineConfig c = Sanitized(MineConfig{});
    Charge charge;
    Countdown arm;
    assert(charge.Take());
    arm.Start(Ms(c.armSeconds));
    assert(!arm.Advance(500) && arm.left);    // not armed yet: no look
    assert(arm.Advance(500) && !arm.left);
    charge.Spend(Ms(c.rechargeSeconds));
    uint32_t t = 0;
    while (!charge.Advance(250)) t += 250;
    assert(t + 250 == 40000);
    std::printf("mine life ok\n");
}

void RingSanitizing() {
    RingConfig c;
    c.points = 1; c.maxEffects = 500; c.perSecond = 0; c.pulseSeconds = -1.0f; c.fadeSeconds = std::nanf(""); c.burstSeconds = 60.0f; c.lift = 5.0f;
    const RingConfig s = Sanitized(c);
    assert(s.points == 3 && s.maxEffects == 48 && s.perSecond == 1 && s.pulseSeconds == 0.0f && s.fadeSeconds == 0.0f && s.burstSeconds == 5.0f && s.lift == 1.0f);
    c.points = 100;
    assert(Sanitized(c).points == kRingMaxPoints);
    const RingConfig d = Sanitized(RingConfig{});
    assert(d.enabled && d.points == 10 && d.maxEffects == 24 && d.perSecond == 24 && Near(d.fadeSeconds, 2.0f) && Near(d.lift, 0.05f));
    std::printf("ring sanitizing ok\n");
}

void RingPoints() {
    const V3 c{10.0f, -3.0f, 5.0f};
    const int counts[] = {3, 10, 24};
    for (int n : counts) {
        V3 sum{};
        for (int i = 0; i < n; ++i) {
            const V3 p = RingPoint(c, 2.5f, i, n, 0.05f);
            assert(Near(Flat(c, p), 2.5f) && Near(p.y, -2.95f));
            // Neighbours one chord apart: evenly spaced.
            const V3 q = RingPoint(c, 2.5f, (i + 1) % n, n, 0.05f);
            assert(Near(Flat(p, q), 2.0f * 2.5f * std::sin(kRingPi / static_cast<float>(n)), 2e-3f));
            sum.x += p.x - c.x;
            sum.z += p.z - c.z;
        }
        assert(Near(sum.x, 0.0f, 1e-3f) && Near(sum.z, 0.0f, 1e-3f));   // centred
    }
    const V3 first = RingPoint(c, 4.0f, 0, 10, 0.0f);
    assert(Near(first.x, 10.0f) && Near(first.z, 9.0f));
    const V3 quarter = RingPoint(c, 4.0f, 1, 4, 0.0f);
    assert(Near(quarter.x, 14.0f) && Near(quarter.z, 5.0f));
    std::printf("ring points ok\n");
}

void RingThinning() {
    for (int n = 1; n <= kRingMaxPoints; ++n) {
        bool used[kRingMaxPoints]{};
        for (int i = 0; i < n; ++i) {
            const int r = RingRank(i, n);
            assert(r >= 0 && r < n && !used[r]);   // a permutation
            used[r] = true;
        }
        assert(RingRank(0, n) == 0);
    }
    assert(RingRank(-1, 10) == 10 && RingRank(10, 10) == 10);
    // Ten points thinned to two leaves two opposite ones, to five every other.
    int kept[10], k = 0;
    for (int i = 0; i < 10; ++i)
        if (PointShown(i, 10, 2)) kept[k++] = i;
    assert(k == 2 && kept[0] == 0 && kept[1] == 5);
    k = 0;
    for (int i = 0; i < 10; ++i)
        if (PointShown(i, 10, 5)) kept[k++] = i;
    assert(k == 5);
    for (int j = 1; j < 5; ++j) assert(kept[j] - kept[j - 1] >= 1 && kept[j] - kept[j - 1] <= 3);

    // The field's last two seconds: all ten until then, fewer each step, none at the end.
    assert(VisibleCount(10, 12000, 2000) == 10 && VisibleCount(10, 2000, 2000) == 10);
    assert(VisibleCount(10, 1999, 2000) == 10 && VisibleCount(10, 1000, 2000) == 5 && VisibleCount(10, 1, 2000) == 1);
    assert(VisibleCount(10, 0, 2000) == 0 && VisibleCount(10, 0, 0) == 10 && VisibleCount(0, 500, 2000) == 0);
    int last = 10;
    for (int left = 2000; left >= 0; left -= 50) {
        const int v = VisibleCount(10, static_cast<uint32_t>(left), 2000);
        assert(v <= last);
        // A point once gone stays gone.
        for (int i = 0; i < 10; ++i) assert(PointShown(i, 10, last) || !PointShown(i, 10, v));
        last = v;
    }
    assert(last == 0);
    std::printf("ring thinning ok\n");
}

void RingCaps() {
    assert(RingBudget(10, 0, 24) == 10);
    assert(RingBudget(10, 20, 24) == 4);    // the field's ten and the mine's ten: four more fit
    assert(RingBudget(10, 24, 24) == 0 && RingBudget(10, 30, 24) == 0 && RingBudget(0, 0, 24) == 0 && RingBudget(-2, 0, 24) == 0);
    assert(PulseStepMs(10, 3.0f) == 300 && PulseStepMs(10, 0.0f) == 0 && PulseStepMs(0, 3.0f) == 0);
    assert(PulseStepMs(24, 0.5f) == 50);   // never faster than 20 renewals a second
    // Default ring: a renewal every 300 ms is well inside 24 effects a second, and both
    // rings plus the burst ring that replaces the mine's stay inside the cap.
    const RingConfig c = Sanitized(RingConfig{});
    assert(1000 / PulseStepMs(c.points, c.pulseSeconds) < static_cast<uint32_t>(c.perSecond));
    assert(2 * c.points <= c.maxEffects);
    std::printf("ring caps ok\n");
}

}  // namespace

int main() {
    Sanitizing();
    Reach();
    Filtering();
    TriggerAndBurst();
    StasisNumbers();
    HealClamping();
    Clocks();
    Charges();
    KeyActions();
    MineLife();
    RingSanitizing();
    RingPoints();
    RingThinning();
    RingCaps();
    std::printf("coop_gadgets tests passed\n");
    return 0;
}
