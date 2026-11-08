// Build and run from the project directory:
// c++ -std=c++17 tests/scene_agreement_tests.cpp -o scene_agreement_tests && ./scene_agreement_tests
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include "../src/scene_catalog.h"

#ifdef NDEBUG
#error Scene agreement tests require assertions
#endif

uint32_t clockMs = 100;
std::array<bool, 3> online{true, true, true};
struct Frame { unsigned from, to; std::array<uint32_t, 17> words{}; };
std::deque<Frame> network;
unsigned dropCommits[3][3]{}, dropAcks[3][3]{}, writes[3][3]{};
Frame lastCommit{}, lastAck{};
void sendFrame(unsigned from, unsigned to, const void* data, unsigned size) {
    Frame f{from, to};
    assert(size == sizeof(f.words));
    if (!online[from] || !online[to]) return;
    std::memcpy(f.words.data(), data, size);
    if (f.words[0] == 0x5252494D) {
        const bool ack = f.words[12] == 0x31414353;
        if (ack) lastAck = f;
        else { lastCommit = f; ++writes[from][to]; }
        unsigned& drops = ack ? dropAcks[from][to] : dropCommits[from][to];
        if (drops) { --drops; return; }
    }
    network.push_back(f);
}

#define TEST_MACHINE 0
namespace host {
#include "scene_agreement_fixture.inc"
}
#undef TEST_MACHINE
#define TEST_MACHINE 1
namespace first {
#include "scene_agreement_fixture.inc"
}
#undef TEST_MACHINE
#define TEST_MACHINE 2
namespace second {
#include "scene_agreement_fixture.inc"
}
#undef TEST_MACHINE

namespace h = host::mirror::agreement;
namespace a = first::mirror::agreement;
namespace b = second::mirror::agreement;
constexpr uint32_t pull = 0x0F97BA0E, gather = 0x0F98BA4D;
constexpr uint32_t room[4] = {7, 0, 0, 0}, noRoom[4] = {};

void step() {
    host::tick();
    if (online[1]) first::tick();
    if (online[2]) second::tick();
    while (!network.empty()) {
        const Frame f = network.front();
        network.pop_front();
        if (!online[f.to]) continue;
        switch (f.to) {
        case 0: host::receive(f.from, f.words.data()); break;
        case 1: first::receive(f.from, f.words.data()); break;
        case 2: second::receive(f.from, f.words.data()); break;
        }
    }
    clockMs += 100;
}
template <typename F> void run(unsigned ticks, F beforeTick) {
    for (unsigned i = 0; i < ticks; ++i) { beforeTick(); step(); }
}
void committed(uint32_t hash, uint32_t attempt, uint32_t owner) {
    if (owner != 10) assert(h::AcceptCommit(hash, attempt, owner, clockMs));
    if (owner != 11) assert(a::AcceptCommit(hash, attempt, owner, clockMs));
    if (owner != 12) assert(b::AcceptCommit(hash, attempt, owner, clockMs));
    h::Consume(hash, owner);
    a::Consume(hash, owner);
    b::Consume(hash, owner);
    assert(!b::AcceptCommit(hash, attempt, owner, clockMs));
}
void testHostOwnedPull() {
    run(10, [] { h::Request(pull, 10, noRoom, clockMs); });
    assert(!h::Find(pull));
    second::loadedSet = 8;
    run(15, [] {
        h::Request(pull, 11, room, clockMs);
        assert(!h::Ready(pull, 11, clockMs));
    });
    second::loadedSet = 7;
    run(12, [] { h::Request(pull, 11, room, clockMs); });
    assert(h::Ready(pull, 11, clockMs));
    assert(!b::AcceptCommit(pull, 12, 10, clockMs));
    committed(pull, 11, 10);
}
void testMissingCommitOnOnePeer() {
    constexpr uint32_t attempt = 112;
    run(15, [&] { h::Request(pull, attempt, room, clockMs); });
    assert(h::Ready(pull, attempt, clockMs));
    assert(a::AcceptCommit(pull, attempt, 10, clockMs));
    assert(b::AcceptCommit(pull, attempt, 10, clockMs));
    // The owner dispatches locally. All three mirror copies are lost on the second link.
    h::Consume(pull, 10);
    a::Consume(pull, 10);
    run(6, [] {});
    assert(!a::AcceptCommit(pull, attempt, 10, clockMs));
    assert(b::AwaitingLeader(pull, clockMs));
    // Readiness traffic continues but cannot reconstruct an owner's dispatch.
    run(45, [] {});
    assert(!b::Find(pull) && !b::AcceptCommit(pull, attempt, 10, clockMs));
}
void testCommitDelivery() {
    // A client leads. Each hop loses its initial burst; the final client also loses its first receipt.
    auto start = [](uint32_t attempt) {
        run(15, [&] { a::Request(pull, attempt, room, clockMs); });
        assert(a::Ready(pull, attempt, clockMs));
        first::mirror::Packet p{first::mirror::kMagic, 1, 11, attempt, pull, 0, clockMs, {}};
        p.reserved[0] = room[0]; p.reserved[5] = first::mirror::kSceneSetMark; p.reserved[6] = attempt;
        assert(first::mirror::scene_delivery::Start(p, clockMs));
        a::Consume(pull, 11);
    };
    start(501);
    dropCommits[1][0] = dropCommits[0][2] = 3;
    dropAcks[2][0] = 1;
    run(25, [] {});
    assert(host::mirror::plays == 1 && second::mirror::plays == 1 && first::mirror::plays == 0);
    assert(writes[1][0] == 4 && writes[0][2] == 5 && writes[0][1] == 0);
    const auto staleCommit = lastCommit, staleAck = lastAck;
    ++host::generation; ++first::generation; ++second::generation;
    h::Clear(); a::Clear(); b::Clear();
    host::mirror::scene_delivery::Clear(); first::mirror::scene_delivery::Clear(); second::mirror::scene_delivery::Clear();
    start(502);
    dropCommits[0][2] = 1;
    run(3, [&] { network.push_back(staleCommit); network.push_back(staleAck); });
    run(15, [] {});
    assert(host::mirror::plays == 2 && second::mirror::plays == 2 && writes[0][2] == 7);
    // Host-owned route: a confirmed client receives one send while the other link stays unavailable.
    run(15, [] { h::Request(pull, 503, room, clockMs); });
    assert(h::Ready(pull, 503, clockMs));
    host::mirror::Packet hostCue{host::mirror::kMagic, 1, 10, 503, pull, 0, clockMs, {}};
    hostCue.reserved[0] = room[0]; hostCue.reserved[5] = host::mirror::kSceneSetMark; hostCue.reserved[6] = 503;
    assert(host::mirror::scene_delivery::Start(hostCue, clockMs));
    h::Consume(pull, 10);
    dropCommits[0][2] = 100;
    run(24, [] {});
    const unsigned expiredWrites = writes[0][2];
    dropCommits[0][2] = 0;
    run(30, [] {});
    assert(writes[0][1] == 1 && writes[0][2] == expiredWrites && first::mirror::plays == 1 && second::mirror::plays == 2);
}
void testCompetingOwnersAndGather() {
    first::position = 50;
    run(25, [] {
        a::Request(gather, 44, room, clockMs, 2);  // A ghost hit the first client's trigger.
        b::Request(gather, 33, room, clockMs, 0);  // The second client walked there.
    });
    assert(h::AwaitingLeader(gather, clockMs) && a::AwaitingLeader(gather, clockMs));
    assert(!b::Ready(gather, 33, clockMs));
    first::position = 0;
    run(12, [] { b::Request(gather, 33, room, clockMs, 0); });
    assert(b::Ready(gather, 33, clockMs));
    committed(gather, 33, 12);
}
void testRemoteGatherKeepsApproachRooms() {
    host::position = 50;
    host::loadedSet = 8;
    const unsigned requests = host::streams::requests;
    auto offer = [] { a::Request(gather, 49, room, clockMs, 0); };
    run(25, offer);
    assert(h::AwaitingLeader(gather, clockMs) && !a::Ready(gather, 49, clockMs));
    assert(!h::PreparingRooms(clockMs) && host::streams::requests == requests);
    host::position = 0;
    run(10, offer);
    assert(h::PreparingRooms(clockMs) && host::streams::requests > requests);
    assert(!a::Ready(gather, 49, clockMs));
    host::loadedSet = room[0];
    run(12, offer);
    assert(a::Ready(gather, 49, clockMs));
    committed(gather, 49, 11);
}
void testChurchDoor() {
    constexpr uint32_t cue = 0x39C2AFA6;
    constexpr uint32_t scene[4] = {0x34CB2202, 0x703E5DB9, 0x54544F43, 0x33363053};
    constexpr uint32_t checkpoint[4] = {0x39CB17CC, 0xA60AA309, 0x4B4C4157, 0x32345245};
    const auto* set = a::StartSet(cue, checkpoint);
    assert(first::streams::GuidEqual(set, scene));
    assert(a::StartSet(gather, room) == room);
    auto offer = [&] { a::Request(cue, 51, set, clockMs); };
    run(15, offer);
    assert(!a::Ready(cue, 51, clockMs));
    assert(host::streams::GuidEqual(host::streams::requested, scene));
    assert(first::streams::GuidEqual(first::streams::requested, scene));
    assert(second::streams::GuidEqual(second::streams::requested, scene));
    assert(first::streams::requests == 1);
    host::loadedSet = first::loadedSet = scene[0];
    host::placementAvailable = false;
    run(15, [&] {
        offer();
        assert(h::PreparingRooms(clockMs) && a::PreparingRooms(clockMs) && b::PreparingRooms(clockMs));
        assert(!a::Ready(cue, 51, clockMs));
    });
    second::loadedSet = scene[0];
    second::position = a::churchStart[0];
    second::mirror::actorsSettling = true;
    run(10, offer);
    assert(!a::Ready(cue, 51, clockMs));
    second::mirror::actorsSettling = false;
    run(10, offer);
    assert(!a::Ready(cue, 51, clockMs));
    host::placementAvailable = true;
    run(10, offer);
    assert(a::Ready(cue, 51, clockMs));
    host::dead = true;
    assert(!h::AcceptCommit(cue, 51, 11, clockMs));
    host::dead = false;
    host::levelSettled = false;
    assert(!h::AcceptCommit(cue, 51, 11, clockMs));
    host::levelSettled = true;
    assert(h::AcceptCommit(cue, 51, 11, clockMs));
    assert(host::placements == 1 && host::position == a::churchStart[0]);
    assert(first::placements == 0 && second::placements == 0);
    host::loadedSet = room[0];
    auto& reservation = *h::Find(cue);
    reservation.lastVote = 0;
    h::Cast(reservation, clockMs);
    assert(reservation.lastReason && h::AcceptCommit(cue, 51, 11, clockMs));
    assert(!h::AcceptCommit(cue, 52, 11, clockMs));
    assert(!h::AcceptCommit(cue, 51, 11, clockMs + h::kLease + 1));
    ++host::generation;
    assert(!h::AcceptCommit(cue, 51, 11, clockMs));
    --host::generation;
    committed(cue, 51, 11);
    assert(!h::PreparingRooms(clockMs) && !a::PreparingRooms(clockMs) && !b::PreparingRooms(clockMs));
    host::loadedSet = first::loadedSet = second::loadedSet = room[0];
    host::position = second::position = 0;
}
void testParticipationAndWorldChanges() {
    first::hook = false;
    run(12, [] {});
    assert(!h::Available(clockMs));
    first::hook = true;
    second::versus = true;
    run(12, [] {});
    assert(!a::Available(clockMs));
    second::versus = false;
    second::dead = true;
    run(25, [] { a::Request(pull, 77, room, clockMs); });
    assert(a::Available(clockMs) && !a::Ready(pull, 77, clockMs));
    second::dead = false;
    second::generation = 2;
    run(30, [] { a::Request(pull, 77, room, clockMs); });
    assert(!a::Ready(pull, 77, clockMs));
    second::generation = 1;
    run(20, [] { a::Request(pull, 77, room, clockMs); });
    assert(a::Ready(pull, 77, clockMs));
    host::peerTable.peers[2].lastSeen = clockMs - 10001;
    assert(!h::GroupReady(*h::Find(pull), clockMs));
    committed(pull, 77, 11);
}
void testOneGrantAcrossHashes() {
    // The same-tick tie is resolved by hash, independent of arrival order.
    run(15, [] {
        b::Request(gather, 81, room, clockMs);
        a::Request(pull, 82, room, clockMs);
    });
    assert(h::Find(pull)->granted && !h::Find(gather)->granted);
    assert(a::Ready(pull, 82, clockMs) && !b::Ready(gather, 81, clockMs));
    committed(pull, 82, 11);
    run(12, [] { b::Request(gather, 81, room, clockMs); });
    assert(b::Ready(gather, 81, clockMs));
    committed(gather, 81, 12);
}
void testOwnerLease() {
    run(12, [] { a::Request(pull, 91, room, clockMs); });
    assert(a::Ready(pull, 91, clockMs));
    // The owner stays connected and votes, but stops refreshing its offer.
    run(12, [] { b::Request(gather, 92, room, clockMs); });
    assert(h::Find(pull)->granted && !h::Find(gather)->granted);
    run(45, [] { b::Request(gather, 92, room, clockMs); });
    assert(!h::Find(pull) && b::Ready(gather, 92, clockMs));
    assert(!a::Ready(pull, 91, clockMs));
    committed(gather, 92, 12);

    run(12, [] { a::Request(pull, 93, room, clockMs); });
    online[1] = false;
    host::peerTable.peers[1] = {};
    second::peerTable.peers[1] = {};
    run(12, [] { b::Request(gather, 94, room, clockMs); });
    assert(!h::Find(pull) && b::Ready(gather, 94, clockMs));
}
int main() {
    assert(scene_catalog::PolicyFor(pull) == scene_catalog::Policy::Pull);
    assert(scene_catalog::PolicyFor(gather) == scene_catalog::Policy::Gather);
    run(15, [] {});
    assert(h::Available(clockMs) && a::Available(clockMs) && b::Available(clockMs));
    testHostOwnedPull();
    testMissingCommitOnOnePeer();
    testChurchDoor();
    testRemoteGatherKeepsApproachRooms();
    testCompetingOwnersAndGather();
    testParticipationAndWorldChanges();
    testOneGrantAcrossHashes();
    constexpr auto aim = h::kEllieAimCue;
    constexpr auto depart = h::kEllieDepartCue;
    assert(scene_catalog::PolicyFor(aim) == scene_catalog::Policy::Gather);
    assert(scene_catalog::PolicyFor(depart) == scene_catalog::Policy::Pull);
    h::capabilities[1].features = h::kChurchHandoff;
    assert(h::Available(clockMs, h::kChurchHandoff) && !h::SupportsCue(aim, clockMs) && !h::SupportsCue(depart, clockMs));
    run(20, [] {});
    assert(h::SupportsCue(aim, clockMs));
    first::position = 50;
    run(20, [&] { h::Request(aim, 101, room, clockMs); });
    assert(!h::Ready(aim, 101, clockMs));
    first::position = 0;
    run(15, [&] { h::Request(aim, 101, room, clockMs); });
    assert(h::Ready(aim, 101, clockMs));
    committed(aim, 101, 10);
    first::position = 50;
    run(20, [&] { h::Request(depart, 102, room, clockMs); });
    assert(h::Ready(depart, 102, clockMs));
    committed(depart, 102, 10);
    first::position = 0;
    testCommitDelivery();
    testOwnerLease();
    std::puts("Three-machine scene agreement tests passed.");
}
