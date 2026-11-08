// Dead Space 2 United by Hafer.
#include "story_object_catalog.h"
namespace story_objects {
constexpr uint32_t kMagic = 0x314F5453; // STO1
struct Packet { uint32_t magic, layout, token, map, generation, paid; uint64_t dead; };
static_assert(sizeof(Packet) == 32 && std::size(cysts) < 64 && std::size(panels) < 32);
uint32_t map = 0, generation = 0, paid = 0, opened = 0;
uint32_t author[std::size(panels)]{};
DWORD lastUnlockTry[std::size(panels)]{};
uint64_t dead = 0;
unsigned cursor = 0, logCount = 0;
DWORD scanned = 0, sent = 0;
bool panelsReady = false, cystsReady = false;
bool Active() { return play::StoryCoop() && network && peerTable.Count() && loadsync::SceneMapHash() && loadsync::SceneGeneration(); }
void World() {
    const uint32_t m = loadsync::SceneMapHash(), g = loadsync::SceneGeneration();
    if (m == map && g == generation) return;
    map = m; generation = g;
    paid = opened = 0; dead = 0;
    std::fill(std::begin(lastUnlockTry), std::end(lastUnlockTry), 0); scanned = sent = 0; cursor = logCount = 0;
}
bool Bytes(uint32_t rva, std::initializer_list<uint8_t> expected) {
    uint8_t bytes[24]{};
    return expected.size() <= sizeof(bytes) && Read(base + rva, bytes, expected.size()) && std::equal(expected.begin(), expected.end(), bytes);
}
void Install(FILE* log) {
    panelsReady = mirror::actionOutcomes && Bytes(0xADC937, {0x6A,0,0x81,0xC6,0xF4,2,0,0,0x56}) &&
                  Bytes(0xABF599, {0x38,0x86,0x34,2,0,0}) &&
                  Bytes(0xABEB60, {0x53,0x56,0x8B,0xF1,0x8B,6,0x8B,0x90,0xF8,1,0,0});
    cystsReady = hits::worldHits && hits::enabled && hits::applyHits && Bytes(0xEF3844, {0x0F,0xB6,5,0xBA,0x39,0x0A,2,0x50}) &&
                 Bytes(0x3CF080, {0x8B,0x51,0x48,0x33,0xC0}) &&
                 Bytes(0xEFEF3E, {0x83,0x8F,0xC0,2,0,0,8});
    std::fprintf(log, "story objects: paid panel replay=%d cyst death replay=%d placement layout=%08X\n", panelsReady, cystsReady, kLayout);
}
uint32_t Entity(const uint32_t* guid, uint32_t classRva) {
    const uint32_t obj = mirror::ObjectByGuid(guid);
    uint32_t vt = 0, actual[4]{};
    return obj && Read(obj, &vt, 4) && vt == base + classRva &&
           Read(obj + 0x24, actual, 16) && !std::memcmp(actual, guid, 16) ? obj : 0;
}
// The opening broadcast has a stack copy of the name and no sender handle.
bool Opened(uint32_t hash) {
    if (!opened || map != loadsync::SceneMapHash() || generation != loadsync::SceneGeneration()) return false;
    for (unsigned i = 0; i < std::size(panels); ++i)
        if (panels[i].unlock == hash && (opened & (1u << i))) return true;
    return false;
}
void Opening(uint32_t hash) {
    if (!panelsReady) return;
    for (unsigned i = 0; i < std::size(panels); ++i) {
        const auto& panel = panels[i];
        if (panel.unlock != hash || !Active()) continue;
        World();
        const uint32_t bit = 1u << i;
        if (opened & bit) return;
        const uint32_t obj = Entity(panel.guid, 0x175DC78);
        uint32_t attrs = 0, actual = 0;
        if (!obj || !Read(obj + 0x0C, &attrs, 4) || !attrs || !Read(attrs + 0x2F4, &actual, 4) || actual != hash) return;
        opened |= bit;
        paid |= bit;
        sent = 0;
        gate::Event("story objects: paid panel guid=%08X-%08X unlock=%08X", panel.guid[0], panel.guid[1], hash);
    }
}
bool Paid(const uint32_t* guid) {
    if (!panelsReady || !paid || !Active() || map != loadsync::SceneMapHash() || generation != loadsync::SceneGeneration()) return false;
    for (unsigned i = 0; i < std::size(panels); ++i)
        if ((paid & (1u << i)) && !std::memcmp(panels[i].guid, guid, 16)) return true;
    return false;
}
void ReplayUnlock(uint32_t hash, uint32_t sender, DWORD now);
void FinishUse(uint32_t obj);
void DisablePanel(uint32_t obj);
void ClosePanel(unsigned index, DWORD now) {
    const auto& panel = panels[index];
    const uint32_t obj = Entity(panel.guid, 0x175DC78);
    uint8_t panelEnabled = 0;
    uint32_t user = 0;
    if (!obj || !Read(obj + 0x234, &panelEnabled, 1) || !Read(obj + 0x4D4, &user, 4)) return;
    if (!(opened & (1u << index))) {
        if (!lastUnlockTry[index] || now - lastUnlockTry[index] >= 1500) {
            lastUnlockTry[index] = now;
            ReplayUnlock(panel.unlock, author[index], now);
        }
        if (!(opened & (1u << index)) || Entity(panel.guid, 0x175DC78) != obj) return;
        if (!Read(obj + 0x234, &panelEnabled, 1) || !Read(obj + 0x4D4, &user, 4)) return;
    }
    if (!user && !panelEnabled) return;
    __try {
        if (user) FinishUse(obj);
        if (panelEnabled) DisablePanel(obj);
        if (logCount++ < 60) gate::Event("story objects: closed paid panel guid=%08X-%08X open_user=%08X", panel.guid[0], panel.guid[1], user);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if (logCount++ < 60) gate::Event("story objects: paid panel cleanup fault guid=%08X-%08X", panel.guid[0], panel.guid[1]);
    }
}
uint32_t CystHealth(uint32_t obj) {
    uint8_t id = 0, slot = 0;
    uint32_t layout = 0, comps = 0, hc = 0, vt = 0, owner = 0;
    if (!Read(base + 0x1CA39BA, &id, 1) || !Read(obj + 0x44, &layout, 4) || !layout ||
        !Read(obj + 0x48, &comps, 4) || !comps || !Read(layout + 12 + id, &slot, 1) ||
        !Read(comps + slot * 4, &hc, 4) || !hc || !Read(hc, &vt, 4) ||
        (vt != base + hits::kHealthVtableRva && vt != base + hits::kDismemberableVtableRva) ||
        !Read(hc + hits::kComponentOwner, &owner, 4) || owner != obj) return 0;
    return hc;
}
void InspectCyst(unsigned i, uint32_t obj, bool retiring) {
    if (!obj) return;
    const uint32_t hc = CystHealth(obj);
    uint32_t flags = 0;
    float hp = 0;
    uint8_t healthFlags = 0;
    if (!hc || !Read(hc + hits::kHealthCurrent, &hp, 4) || !std::isfinite(hp) ||
        !Read(hc + 0xC4, &healthFlags, 1) || !Read(obj + 0x2C0, &flags, 4)) {
        if (logCount++ < 60) gate::Event("story objects: unready cyst guid=%08X-%08X hc=%08X", cysts[i][0], cysts[i][1], hc);
        return;
    }
    const uint64_t bit = uint64_t{1} << i;
    const bool isDead = (healthFlags & 2) || (hp <= 0 && !(healthFlags & 1));
    if (!(dead & bit)) {
        if (!isDead || !(flags & (8u | 64u))) return;
        dead |= bit;
        sent = 0;
        gate::Event("story objects: cyst death guid=%08X-%08X flags=%08X", cysts[i][0], cysts[i][1], flags);
    } else if (!retiring && !isDead && hp > 0 && !(healthFlags & 1)) {
        const bool set = hits::CallSetHealth(hc, 0.0f);
        if (logCount++ < 60) gate::Event("story objects: cyst death applied=%d guid=%08X-%08X health_before=%.1f", set, cysts[i][0], cysts[i][1], hp);
    }
}
void CheckCyst(unsigned i) {
    InspectCyst(i, Entity(cysts[i], 0x178D508), false);
}
// A killed wall body may leave the room between two scans.
void BeforeDelete(uint32_t obj) {
    uint32_t vt = 0, guid[4]{};
    if (!cystsReady || !levelSettled || !Read(obj, &vt, 4) || vt != base + 0x178D508 ||
        !Active() || !Read(obj + 0x24, guid, sizeof(guid))) return;
    for (unsigned i = 0; i < std::size(cysts); ++i) {
        if (std::memcmp(cysts[i], guid, sizeof(guid)) != 0) continue;
        World();
        if (!(dead & (uint64_t{1} << i))) InspectCyst(i, obj, true);
        return;
    }
}
bool Receive(const void* data, int size, unsigned slot) {
    if (size != sizeof(Packet)) return false;
    Packet p{};
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != kMagic) return false;
    if (p.layout != kLayout || !Active() || slot >= coop_peers::kMaxPeers || p.token != peerTable.peers[slot].token ||
        (!IsHost() && peerTable.peers[slot].machine != 0)) return true;
    World();
    if (p.map != map || p.generation != generation || (p.paid >> std::size(panels)) || (p.dead >> std::size(cysts))) return true;
    const uint32_t newPaid = panelsReady ? p.paid & ~paid : 0;
    const uint64_t newDead = cystsReady ? p.dead & ~dead : 0;
    if (newPaid || newDead) {
        for (unsigned i = 0; i < std::size(panels); ++i)
            if (newPaid & (1u << i)) author[i] = p.token;
        paid |= newPaid; dead |= newDead; sent = 0;
        if (logCount++ < 60) gate::Event("story objects: received paid=%08X dead=%016llX token=%08X gen=%u", newPaid, newDead, p.token, generation);
    }
    return true;
}
void Tick(DWORD now) {
    World();
    if (!Active() || !levelSettled) return;
    if (!scanned || now - scanned >= 80) {
        scanned = now;
        if (panelsReady) for (unsigned i = 0; i < std::size(panels); ++i)
            if (paid & (1u << i)) ClosePanel(i, now);
        if (cystsReady) for (unsigned n = 0; n < 8; ++n) {
            CheckCyst(cursor);
            cursor = (cursor + 1) % std::size(cysts);
        }
    }
    if ((paid || dead) && (!sent || now - sent >= 2000)) {
        Packet p{kMagic, kLayout, peerReadiness.token, map, generation, paid, dead};
        SendToPeers(&p, sizeof(p));
        sent = now;
    }
}
}
