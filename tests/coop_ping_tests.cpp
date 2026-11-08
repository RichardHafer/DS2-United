// Dead Space 2 United by Richard Hafer.
// The link monitor's arithmetic, checked without a game or a clock.
//
// Every number this thing prints is going to be used to argue about whether a
// desync is the network's fault, so the arithmetic has to be right before the
// first remote test, not explained away after it.
#include <cassert>
#include <cstdio>
#include "../src/coop_ping_protocol.h"

namespace {

// A perfect link: every request answered after a fixed delay.
void SteadyLink() {
    coop_ping::Monitor m;
    uint32_t now = 1000;
    for (int i = 0; i < 20; ++i) {
        assert(m.Due(now));
        const auto request = m.MakeRequest(now);
        assert(request.kind == coop_ping::kRequest);
        assert(request.originTicks == now);
        now += 40;                                  // 40 ms on the wire
        const auto reply = coop_ping::MakeReply(request, 7, now);
        assert(reply.kind == coop_ping::kReply);
        assert(reply.originTicks == request.originTicks);  // stamp comes back untouched
        assert(m.OnReply(reply, now));
        now += coop_ping::kIntervalMs - 40;
    }
    assert(m.sent == 20 && m.answered == 20 && m.lost == 0);
    assert(m.lastRtt == 40 && m.minRtt == 40 && m.maxRtt == 40);
    assert(m.smoothedRtt == 40);
    assert(m.jitter == 0);
    assert(m.LossPercent() == 0);
    assert(m.stray == 0 && m.reordered == 0);
    std::printf("steady link: rtt=%u jitter=%u loss=%u%%\n",
                m.smoothedRtt, m.jitter, m.LossPercent());
}

// No reply at all: the window ages out and every request counts as lost.
void TotalLoss() {
    coop_ping::Monitor m;
    uint32_t now = 1000;
    for (int i = 0; i < 10; ++i) {
        m.MakeRequest(now);
        now += coop_ping::kIntervalMs;
        m.Expire(now);
    }
    now += coop_ping::kLostAfterMs + 1;
    m.Expire(now);
    assert(m.sent == 10 && m.answered == 0);
    assert(m.lost == 10);
    assert(m.LossPercent() == 100);
    std::printf("total loss: sent=%u lost=%u loss=%u%%\n", m.sent, m.lost, m.LossPercent());
}

// Half the replies go missing. The figure has to be the real ratio, and an
// expired request must not be counted twice (once by Expire and again when
// its slot is reused).
void HalfLoss() {
    coop_ping::Monitor m;
    uint32_t now = 1000;
    for (int i = 0; i < 40; ++i) {
        const auto request = m.MakeRequest(now);
        now += 50;
        if (i % 2 == 0) assert(m.OnReply(coop_ping::MakeReply(request, 7, now), now));
        now += coop_ping::kIntervalMs;
        m.Expire(now);
    }
    now += coop_ping::kLostAfterMs + 1;
    m.Expire(now);
    assert(m.sent == 40);
    assert(m.answered == 20);
    assert(m.lost == 20);              // not 40, and not 60
    assert(m.LossPercent() == 50);
    std::printf("half loss: answered=%u lost=%u loss=%u%%\n",
                m.answered, m.lost, m.LossPercent());
}

// A jumpy link. min and max must keep the extremes; the smoothed pair must sit
// between them rather than chasing the last sample.
void Jitter() {
    coop_ping::Monitor m;
    uint32_t now = 1000;
    const uint32_t delays[] = {30, 180, 32, 200, 28, 175, 31, 190, 29, 185};
    for (uint32_t delay : delays) {
        const auto request = m.MakeRequest(now);
        now += delay;
        assert(m.OnReply(coop_ping::MakeReply(request, 7, now), now));
        now += coop_ping::kIntervalMs;
    }
    assert(m.minRtt == 28);
    assert(m.maxRtt == 200);
    assert(m.smoothedRtt > m.minRtt && m.smoothedRtt < m.maxRtt);
    assert(m.jitter > 0);
    assert(m.lost == 0 && m.stray == 0);
    std::printf("jitter: rtt=%u jitter=%u min=%u max=%u\n",
                m.smoothedRtt, m.jitter, m.minRtt, m.maxRtt);
}

// Replies arriving out of order are counted, not treated as loss.
void Reordering() {
    coop_ping::Monitor m;
    uint32_t now = 1000;
    const auto first = m.MakeRequest(now);
    now += 10;
    const auto second = m.MakeRequest(now);
    now += 60;
    assert(m.OnReply(coop_ping::MakeReply(second, 7, now), now));   // later one first
    now += 5;
    assert(m.OnReply(coop_ping::MakeReply(first, 7, now), now));
    assert(m.answered == 2);
    assert(m.lost == 0);
    assert(m.reordered == 1);
    std::printf("reordering: answered=%u reordered=%u\n", m.answered, m.reordered);
}

// A duplicated reply, and one for a sequence never sent, are strays.
void Strays() {
    coop_ping::Monitor m;
    uint32_t now = 1000;
    const auto request = m.MakeRequest(now);
    now += 25;
    const auto reply = coop_ping::MakeReply(request, 7, now);
    assert(m.OnReply(reply, now));
    assert(!m.OnReply(reply, now));                 // the duplicate
    coop_ping::Packet forged = reply;
    forged.sequence = 9999;
    assert(!m.OnReply(forged, now));                // never asked for this one
    assert(m.answered == 1);
    assert(m.stray == 2);
    std::printf("strays: answered=%u stray=%u\n", m.answered, m.stray);
}

// Tick counts wrap after 49 days. The subtraction has to survive it, because
// a machine that has been up that long would otherwise report a 49-day ping.
void TickWrap() {
    coop_ping::Monitor m;
    uint32_t now = 0xFFFFFFF0;                      // 16 ms before the wrap
    const auto request = m.MakeRequest(now);
    now += 45;                                      // wraps past zero
    assert(m.OnReply(coop_ping::MakeReply(request, 7, now), now));
    assert(m.lastRtt == 45);
    std::printf("tick wrap: rtt=%u\n", m.lastRtt);
}

// Nothing is claimed from too little evidence.
void NoEarlyVerdict() {
    coop_ping::Monitor m;
    uint32_t now = 1000;
    for (int i = 0; i < 4; ++i) { m.MakeRequest(now); now += coop_ping::kIntervalMs; }
    now += coop_ping::kLostAfterMs + 1;
    m.Expire(now);
    assert(m.lost == 4);
    assert(m.LossPercent() == 0);     // four samples is not a verdict
    std::printf("no early verdict: lost=%u reported=%u%%\n", m.lost, m.LossPercent());
}

// The wire size has to stay off every other side-channel packet's length,
// because the receiver dispatches by length and nothing else.
void DistinctWireSize() {
    static_assert(sizeof(coop_ping::Packet) == 28, "ping wire size moved");
    const size_t taken[] = {24, 32, 36, 40, 44, 60};
    for (size_t other : taken) assert(sizeof(coop_ping::Packet) != other);
    assert(sizeof(coop_ping::Packet) <= 64);
    std::printf("wire size: %zu bytes, distinct\n", sizeof(coop_ping::Packet));
}

}  // namespace

int main() {
    SteadyLink();
    TotalLoss();
    HalfLoss();
    Jitter();
    Reordering();
    Strays();
    TickWrap();
    NoEarlyVerdict();
    DistinctWireSize();
    std::printf("coop ping tests passed\n");
    return 0;
}
