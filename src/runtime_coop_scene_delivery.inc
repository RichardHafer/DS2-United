// Dead Space 2 United by Hafer.
namespace scene_delivery {
constexpr uint32_t kAck = 0x31414353;
constexpr DWORD kPeriod = 150;
struct Transfer { Packet message{}; uint32_t waiting[coop_peers::kMaxPeers]{}; DWORD start{}, last{}, lifetime{}; };
struct Receipt { Packet message{}; DWORD start{}; };
Transfer outgoing[16]{};
Receipt receipts[32]{};
void Clear() {
    for (auto& t : outgoing) t = {};
    for (auto& r : receipts) r = {};
}
bool Same(const Packet& p, const Packet& q, uint32_t owner) {
    return p.hash == q.hash && p.token == owner && p.sequence == q.sequence &&
           p.reserved[6] == q.reserved[6] && p.reserved[8] == q.reserved[8] && p.reserved[9] == q.reserved[9];
}
bool World(const Packet& p) {
    return inLevel && p.reserved[8] && p.reserved[8] == loadsync::SceneMapHash() &&
           p.reserved[9] && p.reserved[9] == loadsync::SceneGeneration();
}
bool Track(const Packet& p, unsigned exclude, DWORD now) {
    Transfer next{};
    bool any = false;
    for (unsigned i = 0; i < coop_peers::kMaxPeers; ++i) {
        const auto& peer = peerTable.peers[i];
        if (i == exclude || !peer.used || (!IsHost() && peer.machine != 0)) continue;
        if (!peer.Addressable()) return false;
        next.waiting[i] = peer.token;
        any = true;
    }
    if (!any) return IsHost() && exclude < coop_peers::kMaxPeers;
    for (auto& t : outgoing) if (!t.message.hash || now - t.start > t.lifetime) {
        next.message = p;
        next.start = now;
        next.lifetime = netclock::Window(1500, agreement::kLease - 1500);
        next.last = now - kPeriod;
        t = next;
        return true;
    }
    return false;
}
bool Start(Packet p, DWORD now) {
    if (!agreement::Available(now, agreement::kCommitReceipt)) return false;
    p.reserved[7] |= agreement::kCommitReceipt;
    p.reserved[8] = loadsync::SceneMapHash();
    p.reserved[9] = loadsync::SceneGeneration();
    return World(p) && Track(p, coop_peers::kMaxPeers, now);
}
void Tick(DWORD now) {
    for (auto& t : outgoing) {
        if (!t.message.hash) continue;
        if (!World(t.message)) { t = {}; continue; }
        if (now - t.start > t.lifetime) {
            for (unsigned i = 0; i < coop_peers::kMaxPeers; ++i) if (t.waiting[i])
                gate::Event("scene receipt missing cue=%08X attempt=%u peer=%08X generation=%u age=%u",
                            t.message.hash, t.message.reserved[6], t.waiting[i], t.message.reserved[9],
                            static_cast<unsigned>(now - t.start));
            t = {};
            continue;
        }
        if (now - t.last < kPeriod) continue;
        t.last = now;
        bool any = false;
        for (unsigned i = 0; i < coop_peers::kMaxPeers; ++i) {
            const auto& peer = peerTable.peers[i];
            if (!t.waiting[i]) continue;
            if (!peer.Addressable() || peer.token != t.waiting[i]) { t.waiting[i] = 0; continue; }
            SendToSlot(i, t.message);
            any = true;
        }
        if (!any) t = {};
    }
}
bool Handle(const Packet& p, unsigned from, DWORD now) {
    const bool ack = p.reserved[5] == kAck;
    if (!ack && !(p.reserved[5] == kSceneSetMark && (p.reserved[7] & agreement::kCommitReceipt))) return false;
    if (from >= coop_peers::kMaxPeers || !World(p) || !p.reserved[6]) return true;
    const auto& peer = peerTable.peers[from];
    if (!peer.Addressable()) return true;
    if (ack) {
        if (p.flags || p.token != peer.token) return true;
        for (auto& t : outgoing) if (Same(t.message, p, p.reserved[0]) && t.waiting[from] == p.token) t.waiting[from] = 0;
        return true;
    }
    if ((p.flags & 0xffu & ~kFlagRelayed) || (p.flags & (kFlagDeferred | kFlagPrepare)) || p.reserved[4]) return true;
    if (IsHost() ? ((p.flags & kFlagRelayed) || p.token != peer.token) :
        (peer.machine != 0 || (!(p.flags & kFlagRelayed) && p.token != peer.token))) return true;
    Receipt* available = nullptr;
    bool admitted = false;
    for (auto& r : receipts) {
        if (!r.message.hash || now - r.start > agreement::kLease) available = &r;
        else if (Same(r.message, p, p.token)) admitted = true;
    }
    if (!admitted) {
        auto* reservation = agreement::Find(p.hash);
        if (!available || !reservation || inCount >= kInbox || !streams::GuidEqual(reservation->offer.set, p.reserved) ||
            !agreement::AcceptCommit(p.hash, p.reserved[6], p.token, now)) return true;
        if (IsHost()) {
            Packet relay = p;
            relay.flags |= kFlagRelayed;
            if (!Track(relay, from, now)) return true;
        }
        *available = Receipt{p, now};
        inbox[inCount++] = p;
    }
    Packet reply{};
    reply.magic = kMagic; reply.version = kVersion; reply.token = peerReadiness.token;
    reply.hash = p.hash; reply.sequence = p.sequence;
    reply.reserved[0] = p.token; reply.reserved[5] = kAck; reply.reserved[6] = p.reserved[6];
    reply.reserved[8] = p.reserved[8]; reply.reserved[9] = p.reserved[9];
    SendToSlot(from, reply);
    return true;
}
}
