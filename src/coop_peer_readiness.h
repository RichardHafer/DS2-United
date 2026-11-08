// Dead Space 2 United by Richard Hafer.
#pragma once
#include <cstdint>

namespace coop_peer {
constexpr uint32_t kMagic = 0x32524443, kVersion = 1, kTimeout = 3000;
struct Packet {
    uint32_t magic, version, token, epoch, ackToken, ackEpoch, flags, sequence;
};
static_assert(sizeof(Packet) == 32, "Unexpected readiness wire layout");

inline bool ValidHello(const Packet& p, uint32_t localToken) {
    return p.magic == kMagic && p.version == kVersion && p.token && p.token != localToken &&
           p.epoch && p.flags <= 1;
}

struct State {
    uint32_t token = 0, epoch = 1, sequence = 0, player = 0;
    uint32_t peerToken = 0, peerEpoch = 0, peerSequence = 0, peerSeen = 0;
    uint32_t peerAckToken = 0, peerAckEpoch = 0;
    bool localReady = false, peerReady = false, havePeer = false;

    void UpdateLocal(bool ready, uint32_t currentPlayer) {
        if (player != currentPlayer || localReady != ready) {
            if (++epoch == 0) ++epoch;
            player = currentPlayer;
            localReady = ready;
        }
    }
    Packet MakePacket() {
        return {kMagic, kVersion, token, epoch, peerToken, peerEpoch,
                localReady ? 1U : 0U, ++sequence};
    }
    bool Accept(const Packet& packet, uint32_t now) {
        if (!ValidHello(packet, token)) return false;
        if (havePeer && packet.token == peerToken) {
            const uint32_t delta = packet.sequence - peerSequence;
            if (!delta || delta >= 0x80000000U) return false;
        } else if (havePeer && now - peerSeen <= kTimeout) {
            return false;
        }
        havePeer = true;
        peerToken = packet.token;
        peerEpoch = packet.epoch;
        peerSequence = packet.sequence;
        peerAckToken = packet.ackToken;
        peerAckEpoch = packet.ackEpoch;
        peerReady = packet.flags == 1;
        peerSeen = now;
        return true;
    }
    bool Ready(uint32_t now) const {
        return localReady && havePeer && peerReady && now - peerSeen <= kTimeout &&
               peerAckToken == token && peerAckEpoch == epoch;
    }
};
} // namespace coop_peer
