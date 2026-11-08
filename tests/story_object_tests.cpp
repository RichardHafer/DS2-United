// Dead Space 2 United by Richard Hafer.
#include <windows.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>
#include "../src/coop_peers.h"

constexpr uintptr_t base = 0x400000;
constexpr uint32_t panelObject = 0x11000000, cystObject = 0x12000000, health = 0x12002000;
std::map<uintptr_t, uint8_t> mem;
template<class T> void put(uintptr_t a, const T& v) {
    for (size_t i = 0; i < sizeof(v); ++i) mem[a+i] = reinterpret_cast<const uint8_t*>(&v)[i];
}
bool Read(uintptr_t a, void* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        auto it = mem.find(a+i);
        if (it == mem.end()) return false;
        static_cast<uint8_t*>(p)[i] = it->second;
    }
    return true;
}
bool network = true, levelSettled = true, host = true, cystLoaded = true;
bool IsHost() { return host; }
struct { unsigned token = 11; } peerReadiness;
namespace play { bool StoryCoop() { return true; } }
namespace loadsync {
unsigned map = 24, gen = 2;
unsigned SceneMapHash() { return map; }
unsigned SceneGeneration() { return gen; }
}
struct { struct Peer { unsigned token, machine; bool used; } peers[coop_peers::kMaxPeers]{{12,0,true},{13,2,true}}; unsigned Count() const { return 2; } } peerTable;
namespace gate { void Event(const char*, ...) {} }
#include "../src/story_object_catalog.h"
namespace mirror {
bool actionOutcomes = true;
unsigned propObject = 0;
uint32_t placedGuid[4]{};
uint32_t ObjectByGuid(const uint32_t* guid) {
    if (!std::memcmp(guid, placedGuid, 16)) return propObject;
    if (!std::memcmp(guid, story_objects::panels[0].guid, 16)) return panelObject;
    if (cystLoaded && !std::memcmp(guid, story_objects::cysts[0], 16)) return cystObject;
    return 0;
}
}
namespace hits {
bool worldHits = true, enabled = true, applyHits = true;
constexpr uint32_t kHealthVtableRva = 0x1764760, kDismemberableVtableRva = 0x1764c68, kComponentOwner = 0x10, kHealthCurrent = 0xe8;
int applied = 0;
bool CallSetHealth(uint32_t c, float value) { assert(c == health); put(c + kHealthCurrent, value); ++applied; return true; }
}
std::vector<uint8_t> last;
void SendToPeers(const void* p, size_t n) { last.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p)+n); }
namespace hits {
#include "../src/coop_prop_wire.h"
constexpr uint32_t kCodeEnd = 0x16A1000, kEntityGuid = 0x24, kDamageableOwner = 0x0c, kDamageableHealth = 0x44;
bool propHits = true;
unsigned propCalls = 0;
void OnWorldPacket(const WorldPacket&) { ++propCalls; }
#include "../src/runtime_coop_terminalprop.inc"
}
#include "../src/runtime_coop_storyobjects.inc"
namespace story_objects {
unsigned closed = 0, disabled = 0, unlocks = 0;
void ReplayUnlock(uint32_t hash, uint32_t sender, DWORD) { assert(sender == 12); ++unlocks; Opening(hash); }
void FinishUse(uint32_t obj) { assert(obj == panelObject); ++closed; put(obj+0x4D4,0u); }
void DisablePanel(uint32_t obj) { assert(obj == panelObject); ++disabled; put(obj+0x234,uint8_t(0)); }
}
int main() {
    using namespace story_objects;
    panelsReady = cystsReady = true;
    put(panelObject, uint32_t(base+0x175DC78)); put(panelObject+0x24,panels[0].guid);
    put(panelObject+0x0C, 0x11001000u); put(0x110012F4u,panels[0].unlock);
    Opening(panels[1].unlock);
    assert(!paid);
    Opening(panels[0].unlock);
    assert(Paid(panels[0].guid) && !Paid(panels[1].guid));
    put(cystObject,uint32_t(base+0x178D508)); put(cystObject+0x24,cysts[0]);
    put(base+0x1CA39BA,uint8_t(3)); put(cystObject+0x44,0x12004000u); put(cystObject+0x48,0x12005000u);
    put(0x1200400Fu,uint8_t(2)); put(0x12005008u,health);
    put(health,uint32_t(base+hits::kHealthVtableRva)); put(health+0x10,cystObject);
    put(health+0xc4,uint8_t(0)); put(health+0xe8,0.0f); put(cystObject+0x2c0,0u);
    CheckCyst(0); assert(!dead);
    put(cystObject+0x2c0,8u);
    Tick(100); assert(dead==1 && last.size()==sizeof(Packet));
    Packet p{}; std::memcpy(&p,last.data(),sizeof(p)); p.token=12;
    ++loadsync::gen; World();
    assert(!Paid(panels[0].guid));
    Receive(&p,sizeof(p),0); assert(!paid && !dead);
    ++p.generation;
    Receive(&p,sizeof(p),1); assert(!paid && !dead);
    auto invalid=p; invalid.dead=uint64_t{1}<<63;
    Receive(&invalid,sizeof(invalid),0); assert(!paid && !dead);
    host=false; cystLoaded=false; Receive(&p,sizeof(p),0);
    assert(paid==1 && dead==1 && !Opened(panels[0].unlock));
    put(panelObject+0x234,uint8_t(1)); put(panelObject+0x4D4,0x7777u);
    ClosePanel(0, 700); ClosePanel(0, 800);
    assert(unlocks==1 && closed==1 && disabled==1 && Opened(panels[0].unlock));
    CheckCyst(0); assert(!hits::applied);
    cystLoaded=true; put(health+0xe8,100.0f); put(cystObject+0x2c0,1u);
    put(health+0x10,0u); CheckCyst(0); assert(!hits::applied);
    put(health+0x10,cystObject); CheckCyst(0); assert(hits::applied==1);
    Receive(&p,sizeof(p),0); CheckCyst(0); assert(hits::applied==1);
    ++loadsync::gen; World();
    host = true;
    put(health+0xe8, 100.0f);
    put(cystObject+0x2c0, 8u);
    BeforeDelete(cystObject); assert(!dead);
    put(health+0xe8, 0.0f);
    put(cystObject+0x2c0, 0u);
    BeforeDelete(cystObject); assert(!dead);
    put(cystObject+0x2c0, 8u);
    BeforeDelete(cystObject); assert(dead == 1);
    cystLoaded = false;
    last.clear(); Tick(1000); assert(last.size() == sizeof(Packet));
    last.clear(); Tick(2000); assert(last.empty());
    Tick(3000); assert(last.size() == sizeof(Packet)); // First snapshot lost.
    Packet retry{}; std::memcpy(&retry, last.data(), sizeof(retry)); retry.token = 12;
    host = false; dead = 0;
    Receive(&retry, sizeof(retry), 0); Receive(&retry, sizeof(retry), 0);
    CheckCyst(0); assert(dead == 1 && hits::applied == 1);
    cystLoaded = true; put(health+0xe8, 100.0f);
    CheckCyst(0); CheckCyst(0); assert(hits::applied == 2);
    ++loadsync::gen; World();
    Receive(&retry, sizeof(retry), 0); assert(!dead);
    {
        namespace tp = hits::terminalprop;
        host = true; peerReadiness.token = 12;
        const uint32_t obj = 0x13000000, hc = obj + tp::kDamageable;
        const float pos[3] = {5,6,-17};
        put(obj, uint32_t(base + 0x1760000)); put(obj + 0xC0, pos);
        put(obj + hits::kEntityGuid, tp::kPlacedGuids[0]); put(hc + hits::kDamageableOwner, obj);
        put(hc + hits::kDamageableHealth, 30.f); put(hc + 4, 0u);
        hits::WorldPacket hit{hits::kWorldMagic, hits::kWorldVersion, 12, 8};
        assert(!tp::Queue(hit, obj, hc, 30.f, 0.f, 4000));
        std::memcpy(hit.guid, tp::kPlacedGuids[0], 16);
        std::memcpy(mirror::placedGuid, hit.guid, 16);
        assert(!tp::Queue(hit, obj, hc, 30.f, 1.f, 4000));
        assert(tp::Queue(hit, obj, hc, 30.f, 0.f, 4000));
        last.clear(); tp::Tick(4100); assert(last.empty());
        tp::Tick(4200); assert(last.size() == sizeof(tp::Packet));
        tp::Packet msg{}; std::memcpy(&msg, last.data(), sizeof(msg));
        host = false; peerReadiness.token = 11;
        assert(tp::Receive(msg, 0) && !hits::propCalls); // First copy lost, second reached an unloaded room.
        mirror::propObject = obj;
        put(obj + 0xC0, 4.f); assert(tp::Receive(msg, 0) && !hits::propCalls);
        put(obj + 0xC0, pos); put(hc + 4, 1u); assert(tp::Receive(msg, 0) && !hits::propCalls);
        put(hc + 4, 0u); put(hc + hits::kDamageableHealth, 60.f); assert(tp::Receive(msg, 0) && !hits::propCalls);
        put(hc + hits::kDamageableHealth, 30.f);
        assert(!tp::Receive(msg, 1)); assert(tp::Receive(msg, 0) && hits::propCalls == 1);
        assert(tp::Receive(msg, 0) && hits::propCalls == 1);
        ++loadsync::gen;
        assert(!tp::Receive(msg, 0) && hits::propCalls == 1);
        last.clear(); tp::Tick(5000); assert(last.empty());
        host = true; peerReadiness.token = 12;
        assert(tp::Queue(hit, obj, hc, 30.f, 0.f, 6000));
        for (auto delay : tp::delays) {
            last.clear(); tp::Tick(6000 + delay - 1); assert(last.empty());
            tp::Tick(6000 + delay); assert(last.size() == sizeof(tp::Packet));
        }
        assert(!tp::pending);
        assert(tp::Queue(hit, obj, hc, 30.f, 0.f, 16000));
        last.clear(); tp::Tick(26001); assert(last.empty() && !tp::pending);
        tp::Seen order{};
        tp::Mark(order, 12, 0xfffffffeu); tp::Mark(order, 12, 0u);
        assert(tp::SeenBefore(order,12,0xfffffffeu) && !tp::SeenBefore(order,12,0xffffffffu));
        tp::Mark(order,12,0xffffffffu); assert(tp::SeenBefore(order,12,0xffffffffu));
    }
    std::puts("Story object payment, cyst retirement, lethal prop retries, stream-in, duplicate, and checkpoint checks passed.");
}
