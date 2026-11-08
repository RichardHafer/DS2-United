// Dead Space 2 United by Richard Hafer.
// The vote on the wire, checked without a game.
#include <cassert>
#include <cstdio>
#include "../src/coop_gate_protocol.h"

namespace {
using coop_gate_wire::Announcer;
using coop_gate_wire::Packet;

void WireSizeIsItsOwn() {
    static_assert(sizeof(Packet) == 40, "vote size moved");
    const size_t taken[] = {20, 24, 28, 32, 36, 44, 48, 52, 60};
    for (size_t other : taken) assert(sizeof(Packet) != other);
    std::printf("wire size: vote %zu, distinct from every other packet\n", sizeof(Packet));
}

// The hole in "resend while holding": B released and stopped, A never got B's
// vote. So a vote keeps going out for a fixed time after it was cast, whether
// or not the caster still holds anything.
void RepeatsAfterCasting() {
    Announcer a; a.token = 0x1111;
    const float at[3] = {1, 2, 3};
    a.Cast(0xE1E1, 0x99, at, 1000);
    unsigned sent = 0;
    Packet last{};
    for (uint32_t t = 1000; t < 1000 + coop_gate_wire::kRepeatMs + 2000; t += 50)
        a.Repeat(t, [&](const Packet& p) { ++sent; last = p; });
    assert(sent >= coop_gate_wire::kRepeatMs / coop_gate_wire::kRepeatIntervalMs - 1);
    assert(sent <= coop_gate_wire::kRepeatMs / coop_gate_wire::kRepeatIntervalMs + 2);
    assert(a.Active() == 0);                                  // retired after the window
    assert(last.hash == 0xE1E1 && last.source == 0x99 && last.token == 0x1111);
    assert(last.x == 1 && last.y == 2 && last.z == 3 && coop_gate_wire::Valid(last));
    std::printf("repeats after casting: %u packets over %u ms, then retired\n", sent, coop_gate_wire::kRepeatMs);
}

// Walking in and out of the same trigger is one entry, restarted, not ten.
void RecastRestartsNotDuplicates() {
    Announcer a; a.token = 1;
    const float at[3] = {0, 0, 0};
    for (int i = 0; i < 10; ++i) a.Cast(0xCCCC, 0, at, 1000 + i * 10);
    assert(a.Active() == 1 && a.cast == 10);
    std::printf("recast restarts, not duplicates: active=%u\n", a.Active());
}

// Full: the oldest gives way, nothing crashes, the newest is kept.
void FullEvictsOldest() {
    Announcer a; a.token = 1;
    const float at[3] = {0, 0, 0};
    for (unsigned i = 0; i < coop_gate_wire::kAnnounceCapacity + 3; ++i) a.Cast(0x1000 + i, 0, at, 1000 + i);
    assert(a.Active() == coop_gate_wire::kAnnounceCapacity && a.evicted == 3);
    bool newest = false, oldest = false;
    for (const auto& e : a.entries) {
        if (e.used && e.hash == 0x1000 + coop_gate_wire::kAnnounceCapacity + 2) newest = true;
        if (e.used && e.hash == 0x1000) oldest = true;
    }
    assert(newest && !oldest);
    std::printf("full evicts oldest: evicted=%u\n", a.evicted);
}
}  // namespace

// A released vote keeps going out for the linger time, then stops, so one
// lost datagram cannot leave the partner holding.
void ReleasedVoteLingers() {
    Announcer a; a.token = 1;
    a.repeatMs = 185000;
    const float at[3] = {0, 0, 0};
    a.Cast(0xAB, 1, at, 1000);
    unsigned sent = 0;
    auto count = [&](const coop_gate_wire::Packet&) { ++sent; };
    a.Repeat(1000, count);
    a.Retire(0xAB, 1000, 3000);
    for (uint32_t t = 1400; t <= 3900; t += 400) a.Repeat(t, count);
    const unsigned during = sent;
    for (uint32_t t = 4400; t <= 8000; t += 400) a.Repeat(t, count);
    assert(during >= 6);                // still repeating inside the linger
    assert(sent == during);             // and silent after it
    assert(a.Active() == 0);
    std::printf("released vote lingers: %u copies, then quiet\n", during);
}

int main() {
    WireSizeIsItsOwn();
    RepeatsAfterCasting();
    RecastRestartsNotDuplicates();
    FullEvictsOldest();
    ReleasedVoteLingers();
    std::printf("coop gate wire tests passed\n");
    return 0;
}
