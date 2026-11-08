// Dead Space 2 United by Hafer.
namespace terminalprop {
#include "generated_terminal_props.h"
bool Placed(const uint32_t* guid) {
    const auto* it = std::lower_bound(std::begin(kPlacedGuids), std::end(kPlacedGuids), guid,
        [](const auto& a, const uint32_t* b) { return std::lexicographical_compare(a, a + 4, b, b + 4); });
    return it != std::end(kPlacedGuids) && !std::memcmp(*it, guid, 16);
}
constexpr uint32_t kMagic = 0x31525054; // TPR1
constexpr uint32_t kDamageable = 0x274;
struct Packet {
    uint32_t magic, host, map, generation, classRva, serial;
    float pos[3], before;
    WorldPacket hit;
};
static_assert(sizeof(Packet) == 240);
struct Pending { Packet packet; DWORD at; unsigned next; };
Pending outgoing[32]{};
unsigned cursor = 0;
unsigned pending = 0;
uint32_t serial = 0;
constexpr DWORD delays[] = {150, 600, 1800, 4500, 9000};
struct Seen { uint32_t token, latest; uint64_t mask; };
Seen seen[coop_peers::kMaxPeers]{};
volatile LONG queued = 0, overwritten = 0, retried = 0, missing = 0, mismatched = 0, duplicates = 0, delivered = 0;
uint32_t HostToken() {
    if (IsHost()) return peerReadiness.token;
    for (const auto& peer : peerTable.peers)
        if (peer.used && peer.machine == 0 && peer.token) return peer.token;
    return 0;
}
bool Current(const Packet& p) {
    return p.host && p.map && p.generation && play::StoryCoop() && levelSettled &&
           p.map == loadsync::SceneMapHash() && p.generation == loadsync::SceneGeneration() && p.host == HostToken();
}
bool Queue(const WorldPacket& hit, uint32_t owner, uint32_t damageable, float before, float after, DWORD now) {
    if (after > 0 || !std::isfinite(after) || !std::isfinite(before) || before <= 0 || damageable != owner + kDamageable) return false;
    Packet p{};
    p.host = HostToken(); p.map = loadsync::SceneMapHash(); p.generation = loadsync::SceneGeneration();
    uint32_t vt = 0;
    if (!Current(p) || !Placed(hit.guid) || !Read(owner, &vt, 4) || vt < base + kCodeEnd || vt >= base + 0x1880000 ||
        !Read(owner + 0xC0, p.pos, sizeof(p.pos))) return false;
    for (float value : p.pos) if (!std::isfinite(value)) return false;
    p.magic = kMagic; p.hit = hit; p.classRva = vt - base; p.before = before; p.serial = ++serial;
    auto& entry = outgoing[cursor++ % std::size(outgoing)];
    if (entry.packet.magic) InterlockedIncrement(&overwritten);
    else ++pending;
    entry = Pending{p, now, 0};
    SendToPeers(&p, sizeof(p));
    InterlockedIncrement(&queued);
    return true;
}
void Tick(DWORD now) {
    if (!pending) return;
    for (auto& entry : outgoing) {
        if (!entry.packet.magic) continue;
        const DWORD age = now - entry.at;
        if (age > 10000 || !peerTable.Count() || !Current(entry.packet)) { entry = {}; --pending; continue; }
        if (age < delays[entry.next]) continue;
        SendToPeers(&entry.packet, sizeof(Packet));
        InterlockedIncrement(&retried);
        while (++entry.next < std::size(delays) && age >= delays[entry.next]) {}
        if (entry.next == std::size(delays)) { entry = {}; --pending; }
    }
}
bool SeenBefore(const Seen& s, uint32_t token, uint32_t seq) {
    if (s.token != token || coop_peers::NewerSerial(seq, s.latest)) return false;
    const uint32_t age = s.latest - seq;
    return age >= 64 || (s.mask & (uint64_t{1} << age)) != 0;
}
void Mark(Seen& s, uint32_t token, uint32_t seq) {
    if (s.token != token) { s = Seen{token, seq, 1}; return; }
    if (coop_peers::NewerSerial(seq, s.latest)) {
        const uint32_t shift = seq - s.latest;
        s.mask = (shift >= 64 ? 0 : s.mask << shift) | 1;
        s.latest = seq;
    } else s.mask |= uint64_t{1} << (s.latest - seq);
}
bool Receive(const Packet& p, unsigned ownerSlot) {
    if (p.magic != kMagic || !enabled || !worldHits || !propHits || ownerSlot >= coop_peers::kMaxPeers ||
        !Current(p) || p.hit.magic != kWorldMagic || p.hit.version != kWorldVersion ||
        !p.hit.token || peerTable.peers[ownerSlot].token != p.hit.token || p.classRva < kCodeEnd || p.classRva >= 0x1880000 ||
        !std::isfinite(p.before) || p.before <= 0 || p.hit.net[0x2C] || p.hit.net[0x2D] || !Placed(p.hit.guid)) return false;
    for (unsigned i = 0; i < 8; ++i) if (p.hit.net[i]) return false;
    for (float value : p.pos) if (!std::isfinite(value)) return false;
    auto& s = seen[ownerSlot];
    if (SeenBefore(s, p.hit.token, p.serial)) { InterlockedIncrement(&duplicates); return true; }
    const uint32_t object = mirror::ObjectByGuid(p.hit.guid);
    if (!object) { InterlockedIncrement(&missing); return true; }
    uint32_t vt = 0, owner = 0, flags = 0, actual[4]{};
    float pos[3]{}, hp = 0;
    if (!Read(object, &vt, 4) || vt != base + p.classRva || !Read(object + kEntityGuid, actual, sizeof(actual)) ||
        std::memcmp(actual, p.hit.guid, sizeof(actual)) || !Read(object + 0xC0, pos, sizeof(pos)) ||
        !Read(object + kDamageable + kDamageableOwner, &owner, 4) || owner != object ||
        !Read(object + kDamageable + 4, &flags, 4) || (flags & 1) ||
        !Read(object + kDamageable + kDamageableHealth, &hp, 4) || hp != p.before) { InterlockedIncrement(&mismatched); return true; }
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(pos[i]) || std::fabs(pos[i] - p.pos[i]) > 0.1f) { InterlockedIncrement(&mismatched); return true; }
    Mark(s, p.hit.token, p.serial);
    OnWorldPacket(p.hit);
    InterlockedIncrement(&delivered);
    return true;
}
}
