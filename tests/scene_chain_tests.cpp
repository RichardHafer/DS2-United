// Dead Space 2 United by Richard Hafer.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "../src/scene_catalog.h"

using DWORD = uint32_t;
struct Packet { uint32_t hash, token, reserved[10]; };
bool inLevel = true;
struct { unsigned Count() const { return 2; } } peerTable;
namespace cutscene { bool running = false; bool SequenceRunningNow() { return running; } }
DWORD TicksSince(DWORD now, DWORD then) { return now - then; }
#include "../src/runtime_coop_scenechain.inc"

int main() {
    constexpr uint32_t church = 0x39C2AFA6, player = 0x9FDD0216, guard = 0xE18BD5BE;
    constexpr uint32_t hospital = 0xA09BB44E, door = 0x2EFA8A64;
    for (uint32_t cue : {0x345FD344u, 0x102BCDE5u, 0x4888E9B4u, 0xDA5307E5u, 0x35507026u,
                         0x3D5724C2u, 0x3D5724C3u, 0x67FA8B27u, 0x73E6C89Cu})
        assert(scenechain::ActorOnlyCue(cue));
    assert(!scenechain::ActorOnlyCue(church) && !scenechain::ActorOnlyCue(player));
    Packet cue{};
    cue.hash = player;
    cue.token = 17;
    cue.reserved[6] = 4;
    cue.reserved[7] = church;
    struct LocalCue { Packet packet; } queued[1]{};
    queued[0].packet.hash = church;
    queued[0].packet.token = 17;
    queued[0].packet.reserved[5] = 77;
    queued[0].packet.reserved[6] = 4;
    assert(scenechain::PendingRoot(cue, queued, 1, 77) && !scenechain::Matches(cue));
    assert(!scenechain::PendingRoot(cue, queued, 0, 77));
    scenechain::QueueEarly(cue, 100);
    scenechain::QueueEarly(cue, 200);
    assert(scenechain::early.size() == 1);
    scenechain::early.clear();
    scenechain::Start(church,17,4,100,true);
    assert(scenechain::Matches(cue));
    constexpr uint32_t churchDoor = 0x62EA6643;
    assert(scenechain::Owns(churchDoor) && !scenechain::HasPlayed(churchDoor));
    assert(scenechain::First(churchDoor) && scenechain::HasPlayed(churchDoor));
    assert(scenechain::First(player) && !scenechain::First(player));
    assert(scenechain::Owns(guard) && !scenechain::Owns(door));
    assert(scenechain::Owns(scenechain::kShaftExit) && scenechain::Owns(scenechain::kKongSpawned));
    assert(scenechain::WaitsForExit(scenechain::kKongSpawned));
    assert(scenechain::First(scenechain::kShaftExit) && !scenechain::First(scenechain::kShaftExit));
    assert(!scenechain::WaitsForExit(scenechain::kKongSpawned));
    cue.token = 18;
    assert(!scenechain::Matches(cue));
    cue.token = 17;
    cue.reserved[6] = 3;
    assert(!scenechain::Matches(cue));
    cutscene::running = true;
    scenechain::Tick(20000);
    assert(scenechain::Owns(player));
    cutscene::running = false;
    scenechain::Tick(23100);
    assert(!scenechain::Owns(player));
    scenechain::Start(hospital,18,5,24000,false);
    assert(scenechain::Owns(door) && !scenechain::Owns(guard));
    scenechain::Start(church,17,6,25000,false);
    assert(!scenechain::HasPlayed(churchDoor));
    assert(!scenechain::Owns(scenechain::kShaftExit) && scenechain::Owns(guard));
    scenechain::Start(church,17,6,25000,true);
    assert(scenechain::WaitsForExit(scenechain::kKongSpawned));
    assert(scenechain::First(player));
    inLevel = false;
    scenechain::Tick(25001);
    assert(!scenechain::Owns(player));
    inLevel = true;
    for (auto hash : {0x22B9FA78u, 0x519F6B4Eu, 0xC1C016C2u, 0x83D96D13u})
        assert(scenechain::ActorOnlyCue(hash));
    for (auto hash : {0xDBC76F4Bu, 0xB7491B11u, 0x31C9798Cu})
        assert(!scenechain::ActorOnlyCue(hash));
    for (auto root : {0x31C7790Eu, 0x0F97BA0Eu}) {
        auto success = root == 0x31C7790Eu ? 0x31C8794Du : 0x0F99BA8Cu;
        auto death = root == 0x31C7790Eu ? 0x31C9798Cu : 0x0F98BA4Du;
        scenechain::Start(root, 17, 8, 26000, false);
        assert(!scenechain::Owns(success) && !scenechain::Owns(death));
        for (auto choice : {success, death}) {
            scenechain::Start(root, 17, 9, 26010, false, true);
            Packet result{};
            result.hash = choice;
            result.token = 17;
            result.reserved[7] = root;
            result.reserved[6] = 8;
            assert(!scenechain::Matches(result));
            result.reserved[6] = 9;
            assert(scenechain::Matches(result));
            assert(scenechain::First(choice));
            assert(!scenechain::First(success) && !scenechain::First(death));
        }
    }
    std::puts("Scene chain regression checks passed.");
}
