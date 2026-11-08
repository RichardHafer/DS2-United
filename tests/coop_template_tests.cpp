#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <vector>
#include "../src/coop_peer_readiness.h"
#include "../src/coop_template_protocol.h"

uint32_t clockNow = 10000;
DWORD TestTicks() { return clockNow; }
std::vector<coop_template::Packet> packets;
int TestSend(SOCKET, const char* bytes, int size, int, sockaddr*, int) {
    assert(size == sizeof(coop_template::Packet));
    coop_template::Packet packet{};
    std::memcpy(&packet, bytes, sizeof(packet));
    packets.push_back(packet);
    return size;
}
uint32_t lookupResult = 2, localFlags = 0;
namespace ghost_probe {
struct Event { uint32_t hash, lookup, node, classID, guid[4]; };
void Lookup(Event& e) {
    e.lookup = lookupResult; e.node = 0x100000; e.classID = 123;
    e.guid[0] = e.guid[1] = e.guid[2] = 0; e.guid[3] = e.hash;
}
}
bool Read(uintptr_t address, void* out, size_t size) {
    assert(size == 4);
    uint32_t value = address == 0x10002C ? localFlags : static_cast<uint32_t>(address - 0x48);
    std::memcpy(out, &value, 4); return true;
}
coop_peer::State peerReadiness;
SOCKET dataSock = 1;
sockaddr_in partnerAddr{};
bool partnerAddrKnown = true;
#define DS2COOP_TEMPLATE_TESTS   // the sticky per-partner mode needs the whole co-op half
#define sendto TestSend
#define GetTickCount TestTicks
#include "../src/runtime_coop_templates.inc"
#undef GetTickCount
#undef sendto

int main() {
    peerReadiness.token = 1; peerReadiness.epoch = 4;
    peerReadiness.peerToken = 2; peerReadiness.peerEpoch = 3;
    peerReadiness.peerAckToken = 1; peerReadiness.peerAckEpoch = 4;
    peerReadiness.peerReady = peerReadiness.localReady = peerReadiness.havePeer = true;
    peerReadiness.peerSeen = clockNow;
    templates::enabled = true;
    assert(!templates::Allow(7));
    templates::Poll(clockNow);
    assert(packets.size() == 1 && coop_template::Valid(packets.back()));
    auto reply = packets.back();
    reply.responderToken = 2; reply.responderEpoch = 3; reply.result = coop_template::Match;
    auto bad = reply; bad.responderEpoch = 2;
    assert(!templates::Receive(bad, clockNow));
    bad = reply; ++bad.query; assert(!templates::Receive(bad, clockNow));
    assert(templates::Receive(reply, clockNow));
    assert(templates::Allow(7));
    assert(!templates::Receive(reply, clockNow));
    // A fresh match is not asked again before 1.5 s; after that the renewal gets a new query ID.
    const size_t sentBefore = packets.size();
    clockNow += 501;
    templates::Poll(clockNow);
    assert(packets.size() == sentBefore);
    clockNow += 1000;
    templates::Poll(clockNow);
    assert(packets.size() == sentBefore + 1 && packets.back().query != reply.query);
    peerReadiness.peerSeen = clockNow;   // the peer is still heard from
    assert(!templates::Receive(reply, clockNow));
    clockNow += 2600;   // an answer holds for 4 s
    assert(!templates::Allow(7));

    // Incoming requests use the same local template reader and echo identity.
    auto request = packets.front(); request.ownerToken = 2; request.ownerEpoch = 3;
    assert(templates::Receive(request, clockNow));
    assert(packets.back().result == coop_template::Match);
    localFlags = 16;
    assert(templates::Receive(request, clockNow));
    assert(packets.back().result == coop_template::Mismatch);
    lookupResult = 1;
    assert(templates::Receive(request, clockNow));
    assert(packets.back().result == coop_template::Missing);
    lookupResult = 3;
    assert(templates::Receive(request, clockNow));
    assert(packets.back().result == coop_template::Unreadable);
    lookupResult = 2; localFlags = 0;

    ++peerReadiness.epoch; peerReadiness.peerAckEpoch = peerReadiness.epoch;
    assert(!templates::Allow(7));
    assert(!templates::Receive(reply, clockNow));
    for (uint32_t i = 8; i < 71; ++i) assert(!templates::Allow(i));
    assert(!templates::Allow(1000) && templates::full == 1);
    clockNow += 4000;
    assert(!templates::Allow(7));
    for (const auto& entry : templates::entries) assert(!entry.occupied);
    std::puts("Template query, reply identity, stale reply, expiry, mismatch, lifecycle and capacity tests passed.");
}
