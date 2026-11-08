// Dead Space 2 United by Hafer.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>

using DWORD = uint32_t;
namespace coop_peers { constexpr unsigned kMaxPeers = 3; }
struct Peer { unsigned token; };
struct {
    Peer peers[3]{{22}, {33}, {}};
    unsigned Count() const { return 2; }
    Peer* FindByToken(unsigned t) { for (auto& p : peers) if (t && t == p.token) return &p; return nullptr; }
} peerTable;
unsigned PeerSlot(Peer* p) { return static_cast<unsigned>(p - peerTable.peers); }
bool KnownPeerToken(unsigned t) { return peerTable.FindByToken(t) != nullptr; }
bool IsHost() { return true; }
struct { unsigned token = 11; } peerReadiness;
bool kongShared = true, inLevel = true, levelSettled = true;
DWORD now = 100;
DWORD GetTickCount() { return now; }
namespace loadsync {
uint32_t SceneMapHash() { return 123; }
uint32_t SceneGeneration() { return 8; }
}
namespace gate { void Event(const char*, ...) {} }
constexpr uint32_t boss = 100, kEntityGuid = 8, kKongPhase = 16;
const uint32_t guid[4]{4,3,2,1};
int phase = 2, applications = 0, sends = 0;
unsigned char wire[64]{};
bool IsKong(uint32_t e) { return e == boss; }
bool Read(unsigned address, void* dst, unsigned len) {
    if (address == boss + kEntityGuid && len == 16) std::memcpy(dst, guid, len);
    else if (address == boss + kKongPhase && len == 4) std::memcpy(dst, &phase, len);
    else return false;
    return true;
}
int SendToPeers(const void* p, unsigned n) { assert(n <= sizeof(wire)); std::memcpy(wire,p,n); return ++sends; }
namespace mirror { unsigned ObjectByGuid(const uint32_t* g) { return std::memcmp(g,guid,16) == 0 ? boss : 0; } }
namespace bot {
bool ApplyKongStasis(uint32_t e, float f, float d) { assert(e == boss && f == 0.1f && d > 0); ++applications; return true; }
}
#include "../src/runtime_coop_kong_stasis.inc"

int main() {
    SendKongStasis(boss, 0.1f, 1.5f);
    assert(sends == 1);
    now += 60;
    RepeatKongStasis(now);
    KongStasis p{};
    std::memcpy(&p, wire, sizeof(p));
    assert(sends == 2 && p.duration < 1.45f && p.duration > 1.43f);
    p.token = 22;
    assert(!OnKongStasis(&p, 1));
    assert(OnKongStasis(&p, 0) && OnKongStasis(&p, 0) && applications == 1);
    ++p.sequence; ++p.generation;
    assert(OnKongStasis(&p, 0) && applications == 1);
    --p.generation; ++p.phase;
    assert(OnKongStasis(&p, 0) && applications == 1);
    --p.phase;
    assert(OnKongStasis(&p, 0) && applications == 2);
    p.duration = NAN;
    assert(!OnKongStasis(&p, 0) && applications == 2);
    now += 1000;
    RepeatKongStasis(now);
    assert(sends == 2);
}
