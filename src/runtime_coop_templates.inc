// Dead Space 2 United by Richard Hafer.
namespace gate { void Event(const char* format, ...); }   // runtime_coop_gate.inc
namespace templates {
bool enabled = false, sticky = false;   // sticky: [Lifecycle] StickyTemplates, below
struct Entry {
    coop_template::Packet request{};
    uint32_t wanted = 0, sent = 0, created = 0, received = 0, result = 0;
    bool occupied = false, transmitted = false;
};
Entry entries[64]{};
uint32_t serial = 0, ownerEpoch = 0, peerEpoch = 0, peerToken = 0, cursor = 0, lastSend = 0;
volatile LONG queued = 0, matched = 0, rejected = 0, missing = 0, full = 0, held = 0, sent = 0;
std::mutex logMutex;
coop_template::Packet results[32]{};
unsigned resultCount = 0;
volatile LONG logDropped = 0, logRepeats = 0;
// Logged once per template and result: a Leaper in scope for 48 minutes put
// 5799 identical "result=1" lines into the host's log (24.09.).
struct Logged { uint32_t hash, result; };
Logged logged[128]{};
unsigned loggedNext = 0;
void Record(const coop_template::Packet& packet) {
    std::lock_guard<std::mutex> guard(logMutex);
    for (const auto& l : logged)
        if (l.hash == packet.identity.hash && l.result == packet.result) { InterlockedIncrement(&logRepeats); return; }
    bool updated = false;
    for (auto& l : logged)
        if (l.hash == packet.identity.hash) { l.result = packet.result; updated = true; break; }
    if (!updated) logged[loggedNext++ % std::size(logged)] = Logged{packet.identity.hash, packet.result};
    if (resultCount < std::size(results)) results[resultCount++] = packet;
    else InterlockedIncrement(&logDropped);
}
void Flush(FILE* log) {
    if (!enabled) return;
    coop_template::Packet pending[32]{};
    unsigned count = 0;
    {
        std::lock_guard<std::mutex> guard(logMutex);
        count = resultCount;
        std::copy_n(results, count, pending);
        resultCount = 0;
    }
    for (unsigned i = 0; i < count; ++i) {
        const auto& p = pending[i];
        std::fprintf(log, "coop_template query=%u owner=%08X:%u responder=%08X:%u result=%u hash=%08X class=%08X flags=%08X guid=%08X-%08X-%08X-%08X\n",
            p.query, p.ownerToken, p.ownerEpoch, p.responderToken, p.responderEpoch, p.result,
            p.identity.hash, p.identity.classID, p.identity.flags,
            p.identity.guid[0], p.identity.guid[1], p.identity.guid[2], p.identity.guid[3]);
    }
    static LONG lastRepeats = -1;
    const LONG repeats = InterlockedCompareExchange(&logRepeats, 0, 0);
    if (count || repeats != lastRepeats)
        std::fprintf(log, "coop_template sent=%ld matched=%ld rejected=%ld repeats_not_logged=%ld log_dropped=%ld\n",
                     InterlockedCompareExchange(&sent, 0, 0), InterlockedCompareExchange(&matched, 0, 0),
                     InterlockedCompareExchange(&rejected, 0, 0), repeats, InterlockedCompareExchange(&logDropped, 0, 0));
    lastRepeats = repeats;
}

bool ReadIdentity(uint32_t hash, coop_template::Identity& identity, uint32_t& status) {
    ghost_probe::Event event{};
    event.hash = hash;
    ghost_probe::Lookup(event);
    status = event.lookup == 1 ? coop_template::Missing : coop_template::Unreadable;
    if (event.lookup != 2) return false;
    identity.hash = hash;
    std::copy_n(event.guid, 4, identity.guid);
    identity.classID = event.classID;
    return Read(event.node + 0x2C, &identity.flags, 4);
}
void Refresh(uint32_t now) {
    if (!peerReadiness.Ready(now) || ownerEpoch != peerReadiness.epoch ||
        peerEpoch != peerReadiness.peerEpoch || peerToken != peerReadiness.peerToken) {
        for (auto& entry : entries) entry = {};
        ownerEpoch = peerReadiness.epoch;
        peerEpoch = peerReadiness.peerEpoch;
        peerToken = peerReadiness.peerToken;
    }
}
bool Allow(uint32_t object) {
    const uint32_t now = GetTickCount();
    Refresh(now);
    if (!peerReadiness.Ready(now)) { InterlockedIncrement(&held); return false; }
    uint32_t hash = 0, status = 0;
    coop_template::Identity identity{};
    if (!Read(object + 0x48, &hash, 4) || !ReadIdentity(hash, identity, status)) {
        InterlockedIncrement(&missing); return false;
    }
    Entry* empty = nullptr;
    for (auto& entry : entries) {
        if (!entry.occupied) { if (!empty) empty = &entry; continue; }
        if (!coop_template::Equal(entry.request.identity, identity)) continue;
        entry.wanted = now;
        // Renewed from 1.5 s on and asked again every 0.5 s: 4 s outlast a few lost
        // packets over the internet before the host takes the ghosts out of scope.
        const bool ready = entry.result == coop_template::Match && now - entry.received <= 4000;
        if (!ready) InterlockedIncrement(&held);
        return ready;
    }
    if (!empty) { InterlockedIncrement(&full); return false; }
    if (++serial == 0) ++serial;
    empty->occupied = true;
    empty->wanted = now;
    empty->request = {coop_template::kMagic, coop_template::kVersion, serial,
        peerReadiness.token, peerReadiness.epoch, 0, 0, coop_template::Request, identity};
    InterlockedIncrement(&queued);
    return false;
}
bool Send(const coop_template::Packet& packet) {
    return ::sendto(dataSock, reinterpret_cast<const char*>(&packet), sizeof(packet), 0,
        reinterpret_cast<sockaddr*>(&partnerAddr), sizeof(partnerAddr)) == sizeof(packet);
}
bool ReceiveSticky(coop_template::Packet packet, uint32_t now);
void PollSticky(DWORD now);
bool Receive(coop_template::Packet packet, uint32_t now) {
    if (sticky) return enabled && ReceiveSticky(packet, now);
    Refresh(now);
    if (!enabled || !peerReadiness.Ready(now) || !coop_template::Valid(packet)) return false;
    if (packet.result == coop_template::Request) {
        if (packet.ownerToken != peerReadiness.peerToken || packet.ownerEpoch != peerReadiness.peerEpoch) return false;
        coop_template::Identity local{};
        uint32_t status = coop_template::Unreadable;
        if (ReadIdentity(packet.identity.hash, local, status))
            status = coop_template::Equal(local, packet.identity) ? coop_template::Match : coop_template::Mismatch;
        packet.result = status;
        packet.responderToken = peerReadiness.token;
        packet.responderEpoch = peerReadiness.epoch;
        Record(packet);
        Send(packet);
        return true;
    }
    for (auto& entry : entries) {
        if (!entry.occupied || !entry.transmitted || now - entry.created > 1000 ||
            !coop_template::ReplyMatches(entry.request, packet, peerReadiness.peerToken, peerReadiness.peerEpoch)) continue;
        entry.result = packet.result;
        Record(packet);
        entry.received = now;
        // A new query ID on renewal prevents an old reply extending the cache.
        entry.transmitted = false;
        if (packet.result == coop_template::Match) InterlockedIncrement(&matched);
        else InterlockedIncrement(&rejected);
        return true;
    }
    return false;
}
void Poll(uint32_t now) {
    if (!enabled) return;
    if (sticky) { PollSticky(now); return; }
    Refresh(now);
    if (!partnerAddrKnown || !peerReadiness.Ready(now) || now - lastSend < 25) return;
    lastSend = now;
    unsigned budget = 4;
    for (unsigned i = 0; i < std::size(entries) && budget; ++i) {
        auto& entry = entries[cursor++ % std::size(entries)];
        if (!entry.occupied) continue;
        if (now - entry.wanted > 5000) { entry = {}; continue; }
        if (now - entry.sent < 500 && entry.sent) continue;
        // A match stays good for 4 s (Allow); asking again before 1.5 s only doubles the traffic.
        if (entry.result == coop_template::Match && entry.received && now - entry.received < 1500) continue;
        if (entry.transmitted && now - entry.created > 1000) entry.transmitted = false;
        if (!entry.transmitted) {
            entry.created = now;
            if (++serial == 0) ++serial;
            entry.request.query = serial;
        }
        if (Send(entry.request)) { entry.transmitted = true; InterlockedIncrement(&sent); }
        entry.sent = now;
        --budget;
    }
}

} // namespace templates

#ifndef DS2COOP_TEMPLATE_TESTS
#include "runtime_coop_templates_sticky.inc"
#else
namespace templates {
bool ReceiveSticky(coop_template::Packet, uint32_t) { return false; }
void PollSticky(DWORD) {}
}
bool TemplateScopeAllowed(uint32_t object) { return templates::Allow(object); }
#endif
