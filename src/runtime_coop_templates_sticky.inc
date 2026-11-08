// Dead Space 2 United by Richard Hafer.
// Included from runtime_coop_templates.inc (kept apart so tests/coop_template_tests.cpp builds the old path alone).
namespace templates {
// --- sticky, per partner ([Lifecycle] StickyTemplates) -------------------------------------------
// TNL kills a ghost the moment its object drops out of scope while it has updates pending, which
// a moving enemy always has. The answer above expired after 4 s and was wiped whenever either
// side's readiness epoch moved (every respawn on an MP map), so each blip deleted and rebuilt every
// enemy on the client. A joiner that stalled 3 s while it built those ghosts lost them all again
// (02.11. analysis: late joiners saw the enemies about a minute late). Here an answer belongs to the
// partner's token and lasts until his level goes away: he reports not ready, he is silent for 10 s,
// a new token, or our own level is gone. Our own epoch no longer matters. Requests go straight to
// each partner's address.
constexpr unsigned kStickyEntries = 128;
constexpr DWORD kSilentRevokeMs = 10000, kAskRetryMs = 500, kNoMatchRetryMs = 5000;
struct StickyEntry { coop_template::Identity identity; uint32_t query, sentAt, wanted, result; bool used; };
struct StickyPeer {
    uint32_t token = 0;
    bool revoked = false;
    StickyEntry e[kStickyEntries]{};
    unsigned cursor = 0;
    LONG matched = 0, refused = 0, held = 0, revokes = 0, asks = 0, full = 0;   // game thread
    DWORD firstMatchAt = 0, tokenAt = 0;
};
StickyPeer stickyPeers[coop_peers::kMaxPeers]{};
volatile LONG stickyAllowed = 0, stickyHeld = 0, stickyNoSlot = 0, stickyRevokes = 0, stickyAnswers = 0, stickyReplies = 0, stickyStale = 0;
bool stickyLevel = false;
unsigned stickyLines = 0;
void StickyLine(const char* format, ...) {
    if (stickyLines >= 60) return;
    ++stickyLines;
    char text[200];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    gate::Event("coop_template sticky %s", text);
}
void Revoke(unsigned slot, const char* why) {
    StickyPeer& p = stickyPeers[slot];
    unsigned had = 0;
    for (auto& e : p.e) { if (e.used && e.result == coop_template::Match) ++had; e = StickyEntry{}; }
    InterlockedIncrement(&p.revokes);
    InterlockedIncrement(&stickyRevokes);
    if (had) StickyLine("slot %u machine %u: %u matched template(s) revoked (%s)", slot, peerTable.peers[slot].machine, had, why);
}
// The partner's level stands: known token, heard lately, not reporting a level teardown.
bool PeerLevelStands(unsigned slot, DWORD now) {
    if (slot >= coop_peers::kMaxPeers) return false;
    const auto& peer = peerTable.peers[slot];
    const auto& r = peerRuntime[slot].readiness;
    StickyPeer& p = stickyPeers[slot];
    if (!peer.used || !r.havePeer) return false;
    if (r.peerToken != p.token) {
        if (p.token) Revoke(slot, "a new token: the partner restarted the game");
        p = StickyPeer{};
        p.token = r.peerToken;
        p.tokenAt = now;
    }
    if (now - r.peerSeen > kSilentRevokeMs) {
        if (!p.revoked) { Revoke(slot, "silent for 10 s"); p.revoked = true; }
        return false;
    }
    // Not ready means his level is being torn down or loads (a respawn keeps him ready): at once,
    // ghosts must not reach a world that is going.
    if (!r.peerReady) {
        if (!p.revoked) { Revoke(slot, "he reports not ready: his level is going or loading"); p.revoked = true; }
        return false;
    }
    p.revoked = false;
    return true;
}
StickyEntry* FindSticky(StickyPeer& p, uint32_t hash) {
    for (auto& e : p.e) if (e.used && e.identity.hash == hash) return &e;
    return nullptr;
}
bool AllowSlot(unsigned slot, uint32_t object) {
    if (slot >= coop_peers::kMaxPeers) { InterlockedIncrement(&stickyNoSlot); return false; }
    const DWORD now = GetTickCount();
    if (stickyLevel != inLevel) {   // our own world went or came: nothing we scoped before exists
        stickyLevel = inLevel;
        for (unsigned i = 0; i < coop_peers::kMaxPeers; ++i) for (auto& e : stickyPeers[i].e) e = StickyEntry{};
    }
    StickyPeer& p = stickyPeers[slot];
    const bool stands = PeerLevelStands(slot, now);
    uint32_t hash = 0;
    if (!Read(object + 0x48, &hash, 4) || !hash) { InterlockedIncrement(&missing); return false; }
    if (StickyEntry* e = FindSticky(p, hash)) {
        e->wanted = now;
        if (e->result == coop_template::Match && stands) { InterlockedIncrement(&stickyAllowed); return true; }
        InterlockedIncrement(&stickyHeld);
        InterlockedIncrement(&p.held);
        return false;
    }
    if (!stands || !peerRuntime[slot].readiness.peerReady) { InterlockedIncrement(&stickyHeld); InterlockedIncrement(&p.held); return false; }
    coop_template::Identity identity{};
    uint32_t status = 0;
    if (!ReadIdentity(hash, identity, status)) { InterlockedIncrement(&missing); return false; }
    StickyEntry* slotEntry = nullptr;
    for (auto& e : p.e) if (!e.used) { slotEntry = &e; break; }
    if (!slotEntry) {
        // the least recently wanted one that never matched, else the least recently wanted one
        for (auto& e : p.e) if (e.result != coop_template::Match && (!slotEntry || e.wanted < slotEntry->wanted)) slotEntry = &e;
        if (!slotEntry) for (auto& e : p.e) if (!slotEntry || e.wanted < slotEntry->wanted) slotEntry = &e;
        InterlockedIncrement(&p.full);
    }
    *slotEntry = StickyEntry{identity, 0, 0, now, coop_template::Request, true};
    InterlockedIncrement(&queued);
    InterlockedIncrement(&stickyHeld);
    InterlockedIncrement(&p.held);
    return false;
}
bool SendToSlot(unsigned slot, const coop_template::Packet& packet) {
    const auto& peer = peerTable.peers[slot];
    if (!peer.Addressable()) return false;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = peer.ip;
    to.sin_port = peer.port;
    return ::sendto(dataSock, reinterpret_cast<const char*>(&packet), sizeof(packet), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == sizeof(packet);
}
void PollSticky(DWORD now) {
    if (now - lastSend < 25) return;
    lastSend = now;
    unsigned budget = 8;
    for (unsigned slot = 0; slot < coop_peers::kMaxPeers && budget; ++slot) {
        StickyPeer& p = stickyPeers[slot];
        if (!peerTable.peers[slot].used || !p.token) continue;
        for (unsigned n = 0; n < kStickyEntries && budget; ++n) {
            StickyEntry& e = p.e[p.cursor++ % kStickyEntries];
            if (!e.used || e.result == coop_template::Match) continue;
            if (now - e.wanted > 30000) { e = StickyEntry{}; continue; }
            const DWORD retry = e.result == coop_template::Request ? kAskRetryMs : kNoMatchRetryMs;
            if (e.sentAt && now - e.sentAt < retry) continue;
            if (++serial == 0) ++serial;
            e.query = serial;
            e.sentAt = Stamp(now);
            coop_template::Packet ask = {coop_template::kMagic, coop_template::kVersion, serial, peerReadiness.token, peerReadiness.epoch, 0, 0,
                                         coop_template::Request, e.identity};
            if (SendToSlot(slot, ask)) { InterlockedIncrement(&sent); InterlockedIncrement(&p.asks); }
            --budget;
        }
    }
}
bool ReceiveSticky(coop_template::Packet packet, uint32_t now) {
    if (!coop_template::Valid(packet)) return false;
    if (packet.result == coop_template::Request) {
        // answer any partner, at his own address; epochs do not matter for what is resident
        auto* owner = peerTable.FindByToken(packet.ownerToken);
        if (!owner || !inLevel) return false;
        coop_template::Identity local{};
        uint32_t status = coop_template::Unreadable;
        if (ReadIdentity(packet.identity.hash, local, status))
            status = coop_template::Equal(local, packet.identity) ? coop_template::Match : coop_template::Mismatch;
        packet.result = status;
        packet.responderToken = peerReadiness.token;
        packet.responderEpoch = peerReadiness.epoch;
        Record(packet);
        InterlockedIncrement(&stickyAnswers);
        SendToSlot(PeerSlot(owner), packet);
        return true;
    }
    if (packet.ownerToken != peerReadiness.token) return false;
    auto* responder = peerTable.FindByToken(packet.responderToken);
    if (!responder) return false;
    const unsigned slot = PeerSlot(responder);
    StickyPeer& p = stickyPeers[slot];
    StickyEntry* e = FindSticky(p, packet.identity.hash);
    if (!e || e->query != packet.query || !coop_template::Equal(e->identity, packet.identity)) { InterlockedIncrement(&stickyStale); return true; }
    e->result = packet.result;
    Record(packet);
    InterlockedIncrement(&stickyReplies);
    if (packet.result == coop_template::Match) {
        InterlockedIncrement(&matched);
        InterlockedIncrement(&p.matched);
        if (!p.firstMatchAt) {
            p.firstMatchAt = Stamp(now);
            StickyLine("slot %u machine %u: first template match %lu ms after his token was seen (round trip %lu ms)", slot, responder->machine,
                       static_cast<unsigned long>(now - p.tokenAt), static_cast<unsigned long>(now - e->sentAt));
        }
    } else {
        InterlockedIncrement(&rejected);
        InterlockedIncrement(&p.refused);
    }
    return true;
}
// The partner slot behind a TNL connection (address +0x108, TNL port +0x10C; the data port is TNL + 1000).
unsigned SlotOfConnection(uint32_t connection) {
    uint32_t ip = 0, port = 0;
    if (!connection || !Read(connection + 0x108, &ip, 4) || !Read(connection + 0x10C, &port, 4)) return coop_peers::kMaxPeers;
    for (unsigned i = 0; i < coop_peers::kMaxPeers; ++i) {
        const auto& peer = peerTable.peers[i];
        if (peer.Addressable() && peer.ip == ip && ntohs(peer.port) - kDataPortOffset == port) return i;
    }
    return coop_peers::kMaxPeers;
}
// For the join timeline: what one partner's template check looks like.
void PeerStats(unsigned slot, long& ok, long& heldOut, long& refused, unsigned& pending) {
    ok = heldOut = refused = 0;
    pending = 0;
    if (slot >= coop_peers::kMaxPeers) return;
    const StickyPeer& p = stickyPeers[slot];
    ok = p.matched;
    heldOut = p.held;
    refused = p.refused;
    for (const auto& e : p.e) if (e.used && e.result == coop_template::Request) ++pending;
}
void WriteStickyDiagnostics(FILE* log) {
    if (!sticky) return;
    static LONG last = -1;
    const LONG sum = stickyAllowed / 1000 + stickyHeld / 100 + stickyRevokes + stickyAnswers + stickyReplies + stickyStale + stickyNoSlot;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "coop_template sticky allowed=%ld held=%ld no_slot=%ld revokes=%ld answers=%ld replies=%ld stale=%ld\n", stickyAllowed, stickyHeld, stickyNoSlot,
                 stickyRevokes, stickyAnswers, stickyReplies, stickyStale);
    for (unsigned i = 0; i < coop_peers::kMaxPeers; ++i) {
        const StickyPeer& p = stickyPeers[i];
        if (!p.token) continue;
        unsigned matchedNow = 0, pending = 0;
        for (const auto& e : p.e) {
            if (!e.used) continue;
            if (e.result == coop_template::Match) ++matchedNow;
            else if (e.result == coop_template::Request) ++pending;
        }
        std::fprintf(log, "coop_template sticky slot=%u machine=%u matched_now=%u pending=%u matches=%ld refused=%ld held=%ld asks=%ld revokes=%ld full=%ld\n", i,
                     peerTable.peers[i].machine, matchedNow, pending, p.matched, p.refused, p.held, p.asks, p.revokes, p.full);
    }
}
} // namespace templates

bool TemplateScopeAllowed(uint32_t object) {
    if (!templates::sticky) return templates::Allow(object);
    return templates::AllowSlot(FirstPeerSlot(), object);
}
// The same per TNL connection, so each partner gets only the enemies he can build (runtime_coop.inc's
// scope hook calls this once it passes the connection; see analysis/COOP_LIFECYCLE_MODES_2026-11-02.md).
bool TemplateScopeAllowedFor(uint32_t connection, uint32_t object) {
    if (!templates::sticky) return templates::Allow(object);
    const unsigned slot = templates::SlotOfConnection(connection);
    if (slot >= coop_peers::kMaxPeers) { InterlockedIncrement(&templates::stickyNoSlot); return templates::AllowSlot(FirstPeerSlot(), object); }
    return templates::AllowSlot(slot, object);
}
