// Death delivery, checked without a game.
//
// This is the one message the mod cannot afford to lose. Every other one is
// recoverable: a suit arrives a second late, a position is superseded a
// thirtieth of a second later. Lose a death and one player restarts at the
// checkpoint while the others do not, and from then on it is two campaigns.
#include <cassert>
#include <cstdio>
#include "../src/coop_death_protocol.h"

namespace {
using coop_death::Sender;
using coop_death::Receiver;
constexpr uint32_t ME = 0x1111, B = 0x2222, C = 0x3333, D = 0x4444;

// Three partners, all answering. The sender stops as soon as the last one does.
void StopsWhenEverybodyAnswers() {
    Sender s; s.token = ME;
    const uint32_t peers[] = {B, C, D};
    uint32_t now = 1000;
    s.Record(now, peers, 3);
    assert(s.sending && s.Outstanding() == 3);
    const auto packet = s.MakePacket(now);
    assert(s.Acknowledge(coop_death::MakeAck(packet, B)));
    assert(s.Outstanding() == 2 && s.sending);
    assert(s.Acknowledge(coop_death::MakeAck(packet, C)));
    assert(s.Acknowledge(coop_death::MakeAck(packet, D)));
    assert(!s.sending && s.Outstanding() == 0 && s.acked == 3);
    std::printf("stops when everybody answers: acked=%u attempts=%u\n", s.acked, s.attempts);
}

// One partner silent: it keeps trying, far past the four blind shots the old
// version managed, and then gives up loudly rather than for ever.
void KeepsTryingThenGivesUp() {
    Sender s; s.token = ME;
    const uint32_t peers[] = {B, C};
    uint32_t now = 1000;
    s.Record(now, peers, 2);
    const auto first = s.MakePacket(now);
    s.Acknowledge(coop_death::MakeAck(first, B));       // B answers, C never does
    unsigned sent = 1;
    for (uint32_t t = now; t < now + coop_death::kAckGiveUpMs; t += 50) {
        if (s.Due(t)) { s.MakePacket(t); ++sent; }
    }
    assert(sent > 20);                                   // not four and done
    assert(s.sending && s.Outstanding() == 1);
    assert(s.Expire(now + coop_death::kAckGiveUpMs + 1));
    assert(!s.sending && s.abandoned == 1);
    std::printf("keeps trying then gives up: sent=%u outstanding=%u abandoned=%u\n",
                sent, s.Outstanding(), s.abandoned);
}

// An acknowledgement for an older death must not end the current one. Two
// checkpoints in a row is exactly how this would have gone wrong.
void StaleAckIsIgnored() {
    Sender s; s.token = ME;
    const uint32_t peers[] = {B};
    s.Record(1000, peers, 1);
    const auto oldPacket = s.MakePacket(1000);
    s.Acknowledge(coop_death::MakeAck(oldPacket, B));
    assert(!s.sending);
    // A second death, and the ack for the first one arrives late.
    s.Record(2000, peers, 1);
    assert(s.sending);
    assert(!s.Acknowledge(coop_death::MakeAck(oldPacket, B)));   // wrong epoch
    assert(s.sending && s.Outstanding() == 1);
    std::printf("stale ack ignored: still sending for epoch %u\n", s.epoch);
}

// A stranger's acknowledgement is not one of ours.
void UnknownAckIsIgnored() {
    Sender s; s.token = ME;
    const uint32_t peers[] = {B};
    s.Record(1000, peers, 1);
    const auto packet = s.MakePacket(1000);
    assert(!s.Acknowledge(coop_death::MakeAck(packet, 0x9999)));
    assert(s.sending && s.Outstanding() == 1);
    std::printf("unknown ack ignored: outstanding=%u\n", s.Outstanding());
}

// Playing alone, nothing is waited for.
void SoloNeedsNoAck() {
    Sender s; s.token = ME;
    s.Record(1000, nullptr, 0);
    assert(s.Outstanding() == 0);
    // Nothing to wait for, so one send and the give-up timer resolves it.
    assert(s.Due(1000));
    s.MakePacket(1000);
    assert(s.Expire(1000 + coop_death::kAckGiveUpMs + 1));
    std::printf("solo needs no ack\n");
}

// The receiver answers what it accepted, and folds retransmissions.
void ReceiverAcksAndFolds() {
    Sender s; s.token = B;
    const uint32_t peers[] = {ME};
    s.Record(1000, peers, 1);
    Receiver r;
    unsigned accepted = 0;
    for (int i = 0; i < 5; ++i) {                 // five retransmits of one death
        const auto packet = s.MakePacket(1000 + i * 300);
        if (r.Accept(packet, B, 1000 + i * 300)) ++accepted;
        const auto ack = coop_death::MakeAck(packet, ME);
        assert(coop_death::ValidAck(ack));
        s.Acknowledge(ack);
    }
    assert(accepted == 1);                        // one death, not five
    assert(r.deaths == 1);
    assert(!s.sending);                           // the first ack already ended it
    std::printf("receiver acks and folds: accepted=%u deaths=%u\n", accepted, r.deaths);
}

// A slow link reorders: the second death arrives, then a late copy of the first.
void LateOlderDeathIsNotANewOne() {
    Sender s; s.token = B;
    const uint32_t peers[] = {ME};
    s.Record(1000, peers, 1);
    const auto first = s.MakePacket(1000);
    s.Record(5000, peers, 1);
    const auto second = s.MakePacket(5000);
    Receiver r;
    assert(r.Accept(second, B, 5200));
    assert(!r.Accept(first, B, 5400));             // late, older: not a third death
    assert(r.deaths == 1);
    std::printf("late older death folded: deaths=%u\n", r.deaths);
}

// An ack for another player's death, forwarded by the host, ends nothing here.
void AckForSomebodyElseIsIgnored() {
    Sender s; s.token = B;
    const uint32_t peers[] = {ME};
    s.Record(1000, peers, 1);
    auto packet = s.MakePacket(1000);
    packet.token = 0xC0FFEEu;                      // the same epoch number, another owner
    assert(!s.Acknowledge(coop_death::MakeAck(packet, ME)));
    assert(s.sending);
    packet.token = B;
    assert(s.Acknowledge(coop_death::MakeAck(packet, ME)));
    std::printf("ack for another owner ignored\n");
}

void WireSizesStayDistinct() {
    static_assert(sizeof(coop_death::Packet) == 24, "death size moved");
    static_assert(sizeof(coop_death::Ack) == 20, "ack size moved");
    const size_t taken[] = {24, 28, 32, 36, 44, 48, 52, 56, 60};
    for (size_t other : taken) assert(sizeof(coop_death::Ack) != other);
    std::printf("wire sizes: death %zu, ack %zu, both distinct\n",
                sizeof(coop_death::Packet), sizeof(coop_death::Ack));
}
}  // namespace

int main() {
    StopsWhenEverybodyAnswers();
    KeepsTryingThenGivesUp();
    StaleAckIsIgnored();
    UnknownAckIsIgnored();
    SoloNeedsNoAck();
    ReceiverAcksAndFolds();
    LateOlderDeathIsNotANewOne();
    AckForSomebodyElseIsIgnored();
    WireSizesStayDistinct();
    std::printf("coop death ack tests passed\n");
    return 0;
}
