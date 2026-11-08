// Dead Space 2 United by Richard Hafer.
// The peer table, checked without a game or a socket.
#include <cassert>
#include <cstdio>
#include "../src/coop_peers.h"

namespace {
using coop_peers::Table;
using coop_peers::kMaxPeers;
using coop_peers::kSilentMs;

// Four players on one machine: four different ports on the loopback address.
void FourLocalPlayers() {
    Table t;
    const uint32_t ip = 0x0100007F;
    for (uint16_t i = 0; i < 3; ++i) {
        auto* p = t.Accept(ip, static_cast<uint16_t>(27016 + i), 1000, true);
        assert(p);
        t.Identify(*p, 100 + i, 1 + i);
    }
    assert(t.Count() == 3);
    assert(t.learned == 3 && t.rejected == 0);
    // Each one is found again by address and by token, and they do not collide.
    for (uint16_t i = 0; i < 3; ++i) {
        auto* byAddress = t.Find(ip, static_cast<uint16_t>(27016 + i));
        auto* byToken = t.FindByToken(100 + i);
        assert(byAddress && byToken && byAddress == byToken);
        assert(byToken->machine == 1u + i);
    }
    std::printf("four local players: %u peers, learned=%u\n", t.Count(), t.learned);
}

// A machine that was given a peer list does not let strangers in.
void ConfiguredRefusesStrangers() {
    Table t;
    assert(t.Configure(0x0100007F, 27015, 1000));
    assert(t.Count() == 1);
    assert(!t.Accept(0x0100007F, 40000, 1000, false));
    assert(t.rejected == 1);
    // but the configured one is recognised when it finally speaks
    auto* p = t.Accept(0x0100007F, 27015, 1200, false);
    assert(p && p->configured == 1 && p->lastSeen == 1200);
    std::printf("configured table refuses strangers, rejected=%u\n", t.rejected);
}

// Same player, new address. One slot, not two.
void TokenMovesHouse() {
    Table t;
    auto* first = t.Accept(0x0100007F, 27016, 1000, true);
    t.Identify(*first, 777, 1);
    auto* second = t.Accept(0x0200007F, 30000, 2000, true);
    t.Identify(*second, 777, 1);
    assert(t.Count() == 1);
    auto* found = t.FindByToken(777);
    assert(found && found->port == 30000);
    std::printf("token moved house: %u peer left\n", t.Count());
}

// Full table. A peer that is merely quiet keeps its seat; one that has been
// gone long enough loses it, and a configured one never does.
void FullTable() {
    Table t;
    assert(t.Configure(0x0A000001, 1, 1000));
    for (unsigned i = 1; i < kMaxPeers; ++i)
        assert(t.Accept(0x0A000001, static_cast<uint16_t>(1 + i), 1000, true));
    assert(t.Count() == kMaxPeers);
    // Everyone is recent: the newcomer waits outside.
    assert(!t.Accept(0x0B000001, 9999, 1000 + kSilentMs / 2, true));
    assert(t.rejected == 1);
    // Now everyone has been silent long enough. The configured slot is still
    // untouchable, so a learned one is recycled instead.
    const uint32_t later = 1000 + kSilentMs + 1;
    auto* newcomer = t.Accept(0x0B000001, 9999, later, true);
    assert(newcomer && t.recycled == 1);
    assert(t.Count() == kMaxPeers);
    auto* configured = t.Find(0x0A000001, 1);
    assert(configured && configured->configured == 1);
    std::printf("full table: recycled=%u, configured slot survived\n", t.recycled);
}

// A silent peer is not a departed one.
void SilenceIsNotDeparture() {
    Table t;
    auto* p = t.Accept(0x0100007F, 27016, 1000, true);
    t.Identify(*p, 42, 1);
    // A long loading screen, then it speaks again from the same address.
    auto* again = t.Accept(0x0100007F, 27016, 1000 + kSilentMs - 1, true);
    assert(again == p);
    assert(again->token == 42);       // identity survived the quiet
    assert(t.Count() == 1 && t.learned == 1);
    std::printf("silence is not departure: token=%u kept\n", again->token);
}

void NoTokenNoIdentity() {
    Table t;
    auto* p = t.Accept(0x0100007F, 27016, 1000, true);
    t.Identify(*p, 0, 3);             // a packet with no token identifies nobody
    assert(p->token == 0);
    assert(!t.FindByToken(0));        // and zero never matches a slot
    std::printf("token 0 identifies nobody\n");
}
void HandshakeAdmission() {
    Table t;
    const uint32_t ip = 0x0100007F;
    coop_peer::Packet h{coop_peer::kMagic, 1, 42, 1, 0, 0, 0, 1};
    for (uint16_t port = 1; port < 20; ++port) {
        assert(!t.ReceiveSource(ip, port, 1, true, 91, &h, 4));
        auto wrong = h;
        wrong.version++;
        assert(!t.ReceiveSource(ip, port, 1, true, 91, &wrong, sizeof(wrong)));
    }
    assert(!t.Count());
    assert(!t.ReceiveSource(ip, 17, 10, true, 42, &h, sizeof(h)));
    assert(!t.ReceiveSource(ip, 17, 10, false, 91, &h, sizeof(h)));
    auto* peer = t.ReceiveSource(ip, 17, 10, true, 91, &h, sizeof(h));
    assert(peer && peer->lastSeen == 10);
    t.Identify(*peer, h.token, 1);
    assert(t.ReceiveSource(ip, 17, 500, true, 91, &h, 2) == peer && peer->lastSeen == 10);
    Table client;
    auto* host = client.Configure(ip, 18, 1);
    assert(!client.ReceiveSource(ip, 18, 15, false, 91, &h, 4));
    assert(client.ReceiveSource(ip, 18, 15, false, 91, &h, sizeof(h)) == host);
}

void RestartRequestsInOrder() {
    coop_peers::RestartRequest request;
    assert(!request.Accept(0, 1) && !request.Accept(98, 0));
    assert(request.Accept(98, 8) && request.Accept(98, 9));
    assert(!request.Accept(98, 8) && !request.Accept(98, 9));
    assert(request.Accept(99, 0xFFFFFFFEu) && request.Accept(99, 1));
    assert(!request.Accept(99, 0xFFFFFFFFu));
}
}  // namespace

int main() {
    HandshakeAdmission();
    RestartRequestsInOrder();
    FourLocalPlayers();
    ConfiguredRefusesStrangers();
    TokenMovesHouse();
    FullTable();
    SilenceIsNotDeparture();
    NoTokenNoIdentity();
    std::printf("coop peer table tests passed\n");
    return 0;
}
