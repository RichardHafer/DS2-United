// Dead Space 2 United by Richard Hafer.
#pragma once
#include <cstdint>

// Side-channel notification that a peer's local campaign Player entered the
// native death path (retail ProcessDeath). Observation stage: the receiver only
// records the event. Mirroring the death locally is a separate, opt-in step.
//
// ExchangePlayerState dispatches by received length, so every side-channel
// packet needs a size of its own (static_assert in runtime_coop.inc).
namespace coop_death {
constexpr uint32_t kMagic = 0x32444443, kVersion = 1;

struct Packet {
    uint32_t magic, version, token, epoch, sequence, cause;
};
static_assert(sizeof(Packet) == 24, "Unexpected death wire layout");

// cause is reserved for later classification (damage source, difficulty path).
constexpr uint32_t kCauseUnknown = 0;

// The acknowledgement.
//
// Four blind retransmits were fine on one machine, where nothing is lost. Over
// the internet, four lost datagrams in a row restart one player at the
// checkpoint while the others go on. Other messages can afford a loss (a suit
// arrives late, a position is superseded 33 ms later); this one cannot, so the
// sender keeps going until each partner has confirmed it. 20 bytes.
// owner is the token of the player whose death is confirmed. The host
// forwards acks it is not the owner of, so a client's death is confirmed by
// the other clients too, not only by the host (25.09.: a client-originated
// death retransmitted for the full 15 s because the acks of the clients
// behind the host never came back to it).
struct Ack {
    uint32_t magic, version, token, epoch, owner;
};
static_assert(sizeof(Ack) == 20, "Unexpected death ack wire layout");
constexpr uint32_t kAckMagic = 0x4B434441;   // 'ADCK'

inline bool ValidAck(const Ack& a) {
    return a.magic == kAckMagic && a.version == kVersion && a.token && a.epoch;
}
inline Ack MakeAck(const Packet& death, uint32_t selfToken) {
    return {kAckMagic, kVersion, selfToken, death.epoch, death.token};
}

// How long the sender keeps trying before it accepts that somebody is gone.
// Generous, because the alternative to trying is two players in different
// chapters; bounded, because a partner who has actually left must not stop the
// session from ever sending another death.
constexpr uint32_t kAckRetryMs = 250;
constexpr uint32_t kAckGiveUpMs = 15000;
constexpr unsigned kMaxAckPeers = 8;

inline bool Valid(const Packet& p) {
    return p.magic == kMagic && p.version == kVersion && p.token && p.epoch;
}

// Sender state: one epoch per local death, retransmitted until every partner
// has acknowledged it or the give-up time passes.
struct Sender {
    uint32_t token = 0, epoch = 0, sequence = 0;
    uint32_t lastSend = 0, startedAt = 0;
    bool sending = false;
    // Who still has to answer. Filled from the peer list when the death happens,
    // so a partner who joins afterwards is not waited for.
    uint32_t awaiting[kMaxAckPeers]{};
    unsigned awaitingCount = 0;
    uint32_t attempts = 0, acked = 0, abandoned = 0;

    void Record(uint32_t now, const uint32_t* peers, unsigned peerCount) {
        if (++epoch == 0) ++epoch;
        awaitingCount = 0;
        for (unsigned i = 0; i < peerCount && awaitingCount < kMaxAckPeers; ++i)
            if (peers[i]) awaiting[awaitingCount++] = peers[i];
        sending = true;
        startedAt = now;
        lastSend = now - kAckRetryMs;   // allow an immediate first send
    }

    // An acknowledgement came back. Only the current epoch counts: an ack for a
    // death two checkpoints ago must not end the current one.
    bool Acknowledge(const Ack& ack) {
        if (!ValidAck(ack) || ack.epoch != epoch || (ack.owner && ack.owner != token)) return false;
        for (unsigned i = 0; i < awaitingCount; ++i) {
            if (awaiting[i] != ack.token) continue;
            awaiting[i] = awaiting[--awaitingCount];
            ++acked;
            if (!awaitingCount) sending = false;   // everybody has it
            return true;
        }
        return false;
    }

    // Stop trying, but say so, so the log can tell "delivered" from "gave up".
    bool Expire(uint32_t now) {
        if (!sending || now - startedAt < kAckGiveUpMs) return false;
        sending = false;
        ++abandoned;
        return true;
    }

    bool Due(uint32_t now) const { return sending && now - lastSend >= kAckRetryMs; }
    unsigned Outstanding() const { return awaitingCount; }

    Packet MakePacket(uint32_t now) {
        lastSend = now;
        ++attempts;
        return {kMagic, kVersion, token, epoch, ++sequence, kCauseUnknown};
    }
};

// Receiver state: accept a packet only from a known peer token and only once
// per epoch, so retransmissions do not count as repeated deaths.
struct Receiver {
    uint32_t peerToken = 0, lastEpoch = 0, deaths = 0, lastSeen = 0;

    bool Accept(const Packet& p, uint32_t peerReadinessToken, uint32_t now) {
        if (!Valid(p)) return false;
        if (peerReadinessToken && p.token != peerReadinessToken) return false;
        // Newer epochs only: a late copy of an older death, arriving after the
        // next one on a slow link, is not a new death (wrap-safe compare).
        if (peerToken == p.token && static_cast<int32_t>(p.epoch - lastEpoch) <= 0) { lastSeen = now; return false; }
        peerToken = p.token;
        lastEpoch = p.epoch;
        lastSeen = now;
        ++deaths;
        return true;
    }
};
}  // namespace coop_death
