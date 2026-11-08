// Dead Space 2 United by Richard Hafer.
struct KongStasis {
    uint32_t magic, token, sequence, map, generation, guid[4];
    int32_t phase;
    float factor, duration;
};
static_assert(sizeof(KongStasis) == 48, "Unexpected Tormentor stasis wire layout");
constexpr uint32_t kKongStasisMagic = 0x3154534B; // KST1
uint32_t stasisSequence = 0;
struct SeenStasis { uint32_t token, sequence; };
SeenStasis seenStasis[coop_peers::kMaxPeers]{};
KongStasis lastCast{};
DWORD castAt = 0;
unsigned castCopies = 0, stasisLines = 0;
bool ValidKongStasis(const KongStasis& p) {
    return p.map && p.generation && p.sequence && (p.guid[0] | p.guid[1] | p.guid[2] | p.guid[3]) &&
           p.factor >= 0 && p.factor <= 1 && p.duration > 0 && p.duration <= 120;
}
void SendKongStasis(uint32_t entity, float factor, float duration) {
    if (!kongShared || !inLevel || !levelSettled || !IsKong(entity) || !peerTable.Count()) return;
    KongStasis p{kKongStasisMagic, peerReadiness.token, ++stasisSequence, loadsync::SceneMapHash(), loadsync::SceneGeneration(), {}, 0, factor, duration};
    if (!Read(entity + kEntityGuid, p.guid, 16) || !Read(entity + kKongPhase, &p.phase, 4) || !ValidKongStasis(p)) {
        if (stasisLines++ < 64) gate::Event("tormentor stasis: skipped local cast map=%08X gen=%u factor=%.3f duration=%.3f", p.map, p.generation, factor, duration);
        return;
    }
    const int sent = SendToPeers(&p, sizeof(p));
    lastCast = p; castAt = GetTickCount(); castCopies = 1;
    if (stasisLines++ < 64) gate::Event("tormentor stasis: cast %u phase=%d factor=%.3f duration=%.3f sent=%d", p.sequence, p.phase, factor, duration, sent);
}
bool OnKongStasis(const void* data, unsigned slot) {
    KongStasis p{};
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != kKongStasisMagic || !ValidKongStasis(p) || !KnownPeerToken(p.token) ||
        p.token == peerReadiness.token || (IsHost() && peerTable.peers[slot].token != p.token)) return false;
    if (!kongShared || !inLevel || !levelSettled || p.map != loadsync::SceneMapHash() || p.generation != loadsync::SceneGeneration()) {
        if (stasisLines++ < 64) gate::Event("tormentor stasis: cast %u skipped enabled=%d settled=%d map=%08X/%08X generation=%u/%u",
                                            p.sequence, kongShared, levelSettled, p.map, loadsync::SceneMapHash(), p.generation, loadsync::SceneGeneration());
        return true;
    }
    auto& seen = seenStasis[PeerSlot(peerTable.FindByToken(p.token))];
    if (seen.token == p.token && static_cast<int32_t>(p.sequence - seen.sequence) <= 0) return true;
    const uint32_t kong = mirror::ObjectByGuid(p.guid);
    int32_t phase = -1;
    if (IsKong(kong) && Read(kong + kKongPhase, &phase, 4) && phase == p.phase && bot::ApplyKongStasis(kong, p.factor, p.duration)) {
        seen = {p.token, p.sequence};
        if (stasisLines++ < 64) gate::Event("tormentor stasis: applied cast %u from %08X phase=%d duration=%.3f", p.sequence, p.token, phase, p.duration);
    } else if (stasisLines++ < 64) {
        gate::Event("tormentor stasis: unavailable target=%08X local phase=%d expected=%d cast=%u", kong, phase, p.phase, p.sequence);
    }
    return true;
}
void RepeatKongStasis(DWORD now) {
    if (castCopies && castCopies < 3 && now - castAt >= (castCopies == 1 ? 60u : 180u)) {
        KongStasis p = lastCast;
        p.duration -= static_cast<float>(now - castAt) / 1000.0f;
        if (now - castAt <= 350 && ValidKongStasis(p) && p.map == loadsync::SceneMapHash() && p.generation == loadsync::SceneGeneration())
            SendToPeers(&p, sizeof(p));
        ++castCopies;
    }
}
