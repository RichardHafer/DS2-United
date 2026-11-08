// Dead Space 2 United by Richard Hafer.
#pragma once
#include <cstdint>

// "I hit something you own."
//
// With HostNPCs the host is authoritative over every enemy, and the engine's own
// damage broadcast refuses to send from a peer that is not authoritative:
// DirectSendDamageEvent (PC 0x010BD030) reads the target's net proxy at
// reflectable+0x0C, then IsAuthoritative, and returns without doing anything if
// either fails. So when the client shoots an enemy, the client applies the
// damage locally and the host never hears about it, which is why the
// two machines end up with different exploder sacks lying around.
//
// Rather than move authority for every bullet, which would cost a round trip
// before a hit registers, the client tells the host what it hit and the host
// applies it. One message, no authority churn.
//
// Identity is the net sub-key. SetSubKey stores it at reflectable-0x1C, which
// is entity+0x40, and it is the name the session uses for an object on both
// machines. The entity GUID rides along as a cross-check: it is not unique on
// its own (two pool enemies built from one record share a GUID), but combined
// with the sub-key it turns "I found an object" into "I found the right one".
//
// ExchangePlayerState dispatches by received length, so every side-channel
// packet needs a size of its own (static_assert in runtime_coop.inc).
namespace coop_hit {
constexpr uint32_t kMagic = 0x54494843, kVersion = 1;   // 'CHIT'

struct Packet {
    uint32_t magic, version, token, sequence;
    uint32_t subKey;
    uint32_t guid[4];
    float amount;
    uint32_t zone;
    uint32_t flags;
};
static_assert(sizeof(Packet) == 48, "Unexpected hit wire layout");

// Set while the damage fields are still being measured: the receiver may
// resolve and report the target but must not apply anything.
constexpr uint32_t kFlagObserveOnly = 1u << 0;
// A hit on a world object: one without a net proxy, named by its authored
// GUID alone (sub-key is the sentinel). Every machine has its own copy of the
// decompression button; the one that shot it tells the others to take the
// same health off theirs (18:20: the red button ended the breach for one
// player only).
constexpr uint32_t kFlagWorld = 1u << 1;
constexpr uint32_t kWorldSubKey = 0xFFFFFFFFu;

inline bool Valid(const Packet& p) {
    return p.magic == kMagic && p.version == kVersion && p.subKey != 0;
}

struct Sender {
    uint32_t token = 0, sequence = 0;

    Packet MakePacket(uint32_t subKey, const uint32_t guid[4], float amount,
                      uint32_t zone, uint32_t flags) {
        Packet p{};
        p.magic = kMagic;
        p.version = kVersion;
        p.token = token;
        p.sequence = ++sequence;
        p.subKey = subKey;
        for (int i = 0; i < 4; ++i) p.guid[i] = guid ? guid[i] : 0u;
        p.amount = amount;
        p.zone = zone;
        p.flags = flags;
        return p;
    }
};

// The receiver keeps no per-hit state on purpose. A hit is not an epoch the way
// a death is: two hits in a row are two hits, and folding retransmissions
// together would quietly swallow real damage. Plain UDP means one can be lost,
// and a lost bullet is a far smaller problem than a swallowed one.
struct Receiver {
    uint32_t peerToken = 0, lastSequence = 0;
    uint32_t received = 0, resolved = 0, unresolved = 0, mismatched = 0, stale = 0;

    // Every report now goes out three times, and copies of different hits
    // interleave on a slow link: the last sequence alone let the first hit's
    // late copy through as a second hit. So the recent (token, sequence)
    // pairs of every sender are remembered.
    static constexpr unsigned kRecent = 256;
    uint32_t recentToken[kRecent]{}, recentSequence[kRecent]{};
    unsigned recentHead = 0;

    // True when the packet is worth acting on. Ordering is not required, but a
    // sequence that has already been seen is a duplicate datagram, not a hit.
    bool Accept(const Packet& p, uint32_t peerReadinessToken) {
        if (!Valid(p)) return false;
        if (peerReadinessToken && p.token != peerReadinessToken) return false;
        ++received;
        for (unsigned i = 0; i < kRecent; ++i)
            if (recentSequence[i] == p.sequence && recentToken[i] == p.token) { ++stale; return false; }
        recentToken[recentHead] = p.token;
        recentSequence[recentHead] = p.sequence;
        recentHead = (recentHead + 1) % kRecent;
        peerToken = p.token;
        lastSequence = p.sequence;
        return true;
    }
};
}  // namespace coop_hit
