// Dead Space 2 United by Richard Hafer.
#pragma once
#include <cstdint>

// Replicates which suit the local campaign Player currently wears, so the peer
// can dress this machine's ghost in that suit instead of its own local suit.
//
// Without this the uniform module dresses every ghost in the donor suit, which
// is the local player's suit: each screen then shows its own suit on both
// bodies. Only the cosmetic identity travels here; the receiving side still
// performs the native ChangeToUniform and may refuse a suit whose stream is not
// resident.
//
// ExchangePlayerState dispatches by received length, so every side-channel
// packet needs a size of its own (static_assert in runtime_coop.inc).
namespace coop_suit {
constexpr uint32_t kMagic = 0x32534443, kVersion = 1;

struct Packet {
    uint32_t magic, version, token, epoch, sequence, guid[4];
};
static_assert(sizeof(Packet) == 36, "Unexpected suit wire layout");

inline bool Valid(const Packet& p) {
    return p.magic == kMagic && p.version == kVersion && p.token && p.epoch &&
           (p.guid[0] | p.guid[1] | p.guid[2] | p.guid[3]) != 0;
}

// Sender: publishes the local suit once per change, retransmitted a bounded
// number of times because the side channel is plain UDP. A steady suit is sent
// again every kRefreshMs: a partner that dropped and rejoined clears its copy,
// and 12.10. the client's ghost of the host stayed in the patient suit for the
// rest of the session. The receiver ignores repeats it already has.
struct Sender {
    static constexpr uint32_t kRefreshMs = 5000;
    uint32_t token = 0, epoch = 0, sequence = 0, pending = 0, lastSend = 0;
    uint32_t guid[4]{};
    bool have = false;

    bool Same(const uint32_t g[4]) const {
        return have && guid[0] == g[0] && guid[1] == g[1] && guid[2] == g[2] && guid[3] == g[3];
    }
    void Publish(const uint32_t g[4], uint32_t now) {
        if (Same(g)) return;
        for (unsigned i = 0; i < 4; ++i) guid[i] = g[i];
        have = true;
        if (++epoch == 0) ++epoch;
        pending = 5;
        lastSend = now - 1000;  // allow an immediate first send
    }
    bool Due(uint32_t now) const { return have && now - lastSend >= (pending ? 150u : kRefreshMs); }
    Packet MakePacket(uint32_t now) {
        lastSend = now;
        if (pending) --pending;
        return {kMagic, kVersion, token, epoch, ++sequence, {guid[0], guid[1], guid[2], guid[3]}};
    }
};

// Receiver: accepts one suit per epoch from the known peer token. generation
// increments only on a real change, so the uniform module can re-dress the
// ghost exactly once per peer suit change instead of every frame.
struct Receiver {
    uint32_t peerToken = 0, epoch = 0, guid[4]{}, generation = 0, lastSeen = 0;
    bool have = false;

    bool Accept(const Packet& p, uint32_t peerReadinessToken, uint32_t now) {
        if (!Valid(p)) return false;
        if (peerReadinessToken && p.token != peerReadinessToken) return false;
        lastSeen = now;
        // a retransmission, or a late copy of an older suit arriving after the newer one
        if (have && p.token == peerToken && static_cast<int32_t>(p.epoch - epoch) <= 0) return false;
        peerToken = p.token;
        epoch = p.epoch;
        for (unsigned i = 0; i < 4; ++i) guid[i] = p.guid[i];
        have = true;
        ++generation;
        return true;
    }
};
}  // namespace coop_suit
