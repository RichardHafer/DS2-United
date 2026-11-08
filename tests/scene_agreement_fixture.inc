// Each inclusion constructs a separate copy of the game-facing globals and the protocol.
using DWORD = uint32_t;
using LONG = int32_t;
constexpr unsigned Machine = TEST_MACHINE;

struct Peer { bool used; uint32_t token, machine; DWORD lastSeen; bool Addressable() const { return used; } };
namespace coop_peers { constexpr unsigned kMaxPeers = 3; }
struct PeerTable {
    Peer peers[coop_peers::kMaxPeers]{};
    Peer* FindByToken(uint32_t token) {
        for (auto& p : peers) if (p.used && p.token == token) return &p;
        return nullptr;
    }
    unsigned Count() const {
        return static_cast<unsigned>(std::count_if(std::begin(peers), std::end(peers), [](const Peer& p) { return p.used; }));
    }
} peerTable;
unsigned PeerSlot(const Peer* p) { return static_cast<unsigned>(p - peerTable.peers); }
bool KnownPeerToken(uint32_t t) { return peerTable.FindByToken(t) != nullptr; }
bool IsHost() { return Machine == 0; }
struct { uint32_t token = 10 + Machine; } peerReadiness;
struct { float pos[3]{}; DWORD lastState{}; } peerRuntime[coop_peers::kMaxPeers];

bool inLevel = true, levelSettled = true, dead = false, hook = true, versus = false, registered = true;
uint32_t generation = 1, loadedSet = 7, currentMap = 9;
float position = 0;
unsigned placements = 0;
bool placementAvailable = true;
namespace catchup {
const char* StageChurchIsaac(uint32_t, const float* at) {
    if (!placementAvailable) return "controller unavailable";
    ++placements;
    position = at[0];
    return nullptr;
}
}
DWORD GetTickCount() { return clockMs; }
DWORD Stamp(DWORD t) { return t ? t : 1; }
DWORD TicksSince(DWORD now, DWORD t) { return static_cast<LONG>(now - t) < 0 ? 0 : now - t; }
uint32_t LocalSlotZeroEntity() { return 1; }
namespace nameplate { bool Dead(uint32_t) { return dead; } }
namespace loadsync {
bool SceneSyncCapable() { return hook; }
uint32_t SceneGeneration() { return generation; }
uint32_t SceneMapHash() { return currentMap; }
}
namespace play {
bool SurvivalMode() { return false; }
bool VersusMode() { return versus; }
}
namespace spectate { bool Active() { return false; } }
namespace mpmap { bool InVisit() { return false; } }
namespace necro { int LocalRole() { return 0; } }
namespace cutscene {
bool FollowsSlot(unsigned) { return false; }
bool SequenceRunningNow() { return false; }
}
namespace gate {
bool Registered(uint32_t) { return registered; }
bool LocalPosition(float* p) { p[0] = position; return true; }
void Event(const char*, ...) {}
}
namespace streams {
bool GuidEqual(const uint32_t* a, const uint32_t* b) { return std::memcmp(a, b, 16) == 0; }
bool GuidZero(const uint32_t* g) { return !(g[0] || g[1] || g[2] || g[3]); }
bool Covers(const uint32_t* g) { return g[0] == loadedSet; }
uint32_t requested[4]{};
unsigned requests = 0;
void Follow(const uint32_t* set) {
    std::memcpy(requested, set, sizeof(requested));
    ++requests;  // Tests deliver the load completion explicitly.
}
}
namespace coop_gate {
float DistanceSquared(const float* a, const float* b) { return (a[0] - b[0]) * (a[0] - b[0]); }
}
namespace netclock { DWORD Window(DWORD ms, DWORD) { return ms; } }

void SendToPeers(const void* data, unsigned size) {
    if (IsHost()) {
        sendFrame(Machine, 1, data, size);
        sendFrame(Machine, 2, data, size);
    } else sendFrame(Machine, 0, data, size);
}
void RelayToOthers(const void* data, unsigned size, unsigned from) {
    for (unsigned i = 1; i < 3; ++i) if (i != from) sendFrame(Machine, i, data, size);
}
namespace mirror {
struct Packet { uint32_t magic, version, token, sequence, hash, flags, tick, reserved[10]; };
constexpr uint32_t kMagic = 0x5252494D, kVersion = 1, kSceneSetMark = 0x54455343, kFlagRelayed = 1;
constexpr uint32_t kFlagPrepare = 0x40000000, kFlagDeferred = 0x80000000;
constexpr unsigned kInbox = 64;
Packet inbox[kInbox]{};
unsigned inCount = 0, plays = 0;
void SendToSlot(unsigned slot, const Packet& p) { sendFrame(Machine, slot, &p, sizeof(p)); }
bool actorsSettling = false;
bool ChurchActorsSettling(DWORD) { return actorsSettling; }
#include "../src/runtime_coop_scene_agreement.inc"
#include "../src/runtime_coop_scene_delivery.inc"
}
void tick() {
    for (unsigned i = 0; i < 3; ++i) {
        if (i == Machine || !online[i]) continue;
        peerTable.peers[i] = Peer{true, 10 + i, i, clockMs};
        peerRuntime[i].lastState = clockMs;
    }
    mirror::agreement::Tick(clockMs);
    mirror::scene_delivery::Tick(clockMs);
}
void receive(unsigned from, const void* data) {
    mirror::Packet m;
    std::memcpy(&m, data, sizeof(m));
    if (m.magic == mirror::kMagic) {
        assert(mirror::scene_delivery::Handle(m, from, clockMs));
        while (mirror::inCount) {
            const auto& cue = mirror::inbox[--mirror::inCount];
            assert(mirror::agreement::AcceptCommit(cue.hash, cue.reserved[6], cue.token, clockMs));
            mirror::agreement::Consume(cue.hash, cue.token);
            ++mirror::plays;
        }
        return;
    }
    mirror::agreement::Packet p;
    std::memcpy(&p, data, sizeof(p));
    mirror::agreement::OnPacket(p, from, clockMs);
}
