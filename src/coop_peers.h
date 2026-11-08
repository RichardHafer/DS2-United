// Dead Space 2 United by Hafer.
#pragma once
#include <cstdint>
#include <cstring>
#include "coop_peer_readiness.h"

// More than one partner.
//
// The side channel was written for exactly two machines: one partner address,
// one readiness token, one of every receiver, and a PlayerState packet with no
// sender in it. None of that is an architectural limit: the identity scheme
// underneath already carries a whole byte of machine index, because
// AllocatePlayerSubKey builds `(machine << 24) | counter` and refuses anything
// above 255. The engine's own NetInterface holds an array of connections, not
// one. It was only the transport that assumed a pair.
//
// This is the table that stops it assuming that. It holds addresses and
// identity and nothing that belongs to the game, so it can be tested without
// one.
namespace coop_peers {
constexpr unsigned kMaxPeers = 8;

// A peer is identified by its readiness token once it has said hello, and by
// its address before that. The token is what every packet except PlayerState
// already carries, so routing by it costs nothing new on the wire.
struct Peer {
    bool used = false;
    uint32_t ip = 0;          // network order, as it arrives
    uint16_t port = 0;        // network order
    uint32_t token = 0;       // 0 until the peer has identified itself
    uint32_t machine = 0xFFFFFFFF;
    uint32_t lastSeen = 0;
    uint32_t configured = 0;  // 1 when it came from the launcher, not from a packet
    // A player only ever heard about through somebody else.
    //
    // A client refuses packets from anyone it was not given, so two clients
    // that were not told about each other (a star launcher) have no direct
    // path: the host saw everybody and the clients only the host. Rather than
    // open every client to every stranger, the host forwards what it hears,
    // and a forwarded player gets a slot with no address of its own: known
    // by token, never written to directly.
    uint32_t relayed = 0;

    bool Is(uint32_t otherIp, uint16_t otherPort) const {
        return used && !relayed && ip == otherIp && port == otherPort;
    }
    bool Addressable() const { return used && !relayed && ip && port; }
};

// How long a peer may be silent before its slot can be taken by somebody else.
// Long on purpose: a loading screen is not a departure, and recycling a slot
// under a peer that is merely busy would hand its identity to a stranger.
constexpr uint32_t kSilentMs = 120000;

inline bool NewerSerial(uint32_t next, uint32_t previous) {
    const uint32_t delta = next - previous;
    return delta && delta < 0x80000000u;
}

struct RestartRequest {
    uint32_t token = 0, last = 0;
    bool Accept(uint32_t sender, uint32_t seq) {
        if (!sender || !seq || (token == sender && !NewerSerial(seq, last))) return false;
        token = sender;
        last = seq;
        return true;
    }
};

struct Table {
    Peer peers[kMaxPeers]{};
    unsigned learned = 0, rejected = 0, recycled = 0;

    unsigned Count() const {
        unsigned n = 0;
        for (const auto& p : peers) if (p.used) ++n;
        return n;
    }

    Peer* Find(uint32_t ip, uint16_t port) {
        for (auto& p : peers) if (p.Is(ip, port)) return &p;
        return nullptr;
    }

    Peer* FindByToken(uint32_t token) {
        if (!token) return nullptr;
        for (auto& p : peers) if (p.used && p.token == token) return &p;
        return nullptr;
    }

    // A peer named in the launcher. Configured slots are never recycled: they
    // are meant to be there even while they are still starting up.
    Peer* Configure(uint32_t ip, uint16_t port, uint32_t now) {
        if (Peer* existing = Find(ip, port)) return existing;
        for (auto& p : peers) {
            if (p.used) continue;
            p = Peer{};
            p.used = true; p.ip = ip; p.port = port;
            p.lastSeen = now; p.configured = 1;
            return &p;
        }
        ++rejected;
        return nullptr;
    }

    // A packet arrived from somewhere. Returns the peer it belongs to, adding a
    // slot when there is room.
    //
    // `openToStrangers` is the host's position: it has no configured peers and
    // has to learn everyone from their first packet. A machine that was given a
    // peer list does not accept anybody else, because then a stray datagram
    // could take a seat at the table.
    Peer* Accept(uint32_t ip, uint16_t port, uint32_t now, bool openToStrangers) {
        if (Peer* existing = Find(ip, port)) { existing->lastSeen = now; return existing; }
        if (!openToStrangers) { ++rejected; return nullptr; }
        for (auto& p : peers) {
            if (p.used) continue;
            p = Peer{};
            p.used = true; p.ip = ip; p.port = port; p.lastSeen = now;
            ++learned;
            return &p;
        }
        // Full. A slot that has been silent long enough, and was not configured,
        // can be taken; otherwise the newcomer is turned away rather than
        // evicting somebody who is simply loading.
        Peer* oldest = nullptr;
        for (auto& p : peers) {
            if (p.configured) continue;
            if (now - p.lastSeen < kSilentMs) continue;
            if (!oldest || static_cast<int32_t>(p.lastSeen - oldest->lastSeen) < 0) oldest = &p;
        }
        if (!oldest) { ++rejected; return nullptr; }
        *oldest = Peer{};
        oldest->used = true; oldest->ip = ip; oldest->port = port; oldest->lastSeen = now;
        ++recycled;
        ++learned;
        return oldest;
    }

    // Only a readiness handshake may claim an unknown endpoint. Dispatch refreshes
    // an existing peer's deadline after its packet has passed validation.
    Peer* ReceiveSource(uint32_t ip, uint16_t port, uint32_t now, bool host, uint32_t localToken,
                        const void* data, int size) {
        Peer* source = Find(ip, port);
        if (source && source->token) return source;
        coop_peer::Packet hello{};
        if (size != sizeof(hello)) return nullptr;
        std::memcpy(&hello, data, sizeof(hello));
        if (!coop_peer::ValidHello(hello, localToken)) return nullptr;
        return source ? source : Accept(ip, port, now, host);
    }

    // A player only heard about through another peer. Returns its slot, making
    // one on first contact. Never takes a slot away from a directly connected
    // peer: a forwarded player is the weakest claim in the table, so it only
    // fills space nobody else wanted.
    Peer* AcceptRelayed(uint32_t token, uint32_t machine, uint32_t now) {
        if (!token) return nullptr;
        if (Peer* known = FindByToken(token)) {
            known->lastSeen = now;
            if (machine != 0xFFFFFFFF) known->machine = machine;
            return known;
        }
        for (auto& p : peers) {
            if (p.used) continue;
            p = Peer{};
            p.used = true; p.relayed = 1; p.token = token;
            p.machine = machine; p.lastSeen = now;
            ++learned;
            return &p;
        }
        ++rejected;
        return nullptr;
    }

    // Drop forwarded players that have gone quiet. They cost a slot and, unlike
    // a peer with an address, there is nothing to retry: if the relay stopped
    // mentioning them, they are gone.
    void ExpireRelayed(uint32_t now) {
        for (auto& p : peers) {
            if (!p.used || !p.relayed) continue;
            if (now - p.lastSeen > kSilentMs) p = Peer{};
        }
    }

    // Identity, once the peer has said who it is. A token that is already on
    // another slot means the same machine came back from a new address, so the
    // old slot goes rather than two slots claiming one player.
    void Identify(Peer& peer, uint32_t token, uint32_t machine) {
        if (!token) return;
        for (auto& other : peers) {
            if (&other == &peer || !other.used || other.token != token) continue;
            other = Peer{};
        }
        peer.token = token;
        if (machine != 0xFFFFFFFF) peer.machine = machine;
    }
};
}  // namespace coop_peers
