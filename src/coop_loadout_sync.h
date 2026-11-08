// Dead Space 2 United by Richard Hafer.
#pragma once
#include <cstdint>

// Replicates which weapons the local campaign Player carries, by FULL proxy
// GUID, so the peer can tell what its ghost is missing.
//
// The native ghost gun update only carries a name hash. That is not enough:
// DS2_COOP_GUN_NETWORK_2026-09-16.md shows the hash C2773912 (RivetGun) maps to
// two different proxy GUIDs, and a hash the ghost inventory does not contain
// simply finds nothing, which is why a peer's Force Gun and Seeker Rifle stayed
// invisible. Provisioning needs the full proxy GUID that RequisitionGun takes.
//
// One slot per packet: a single 140-byte manifest of the whole loadout was the
// earlier draft (removed 07.10.).
// ExchangePlayerState dispatches by received length, so every side-channel
// packet needs a size of its own (static_assert in runtime_coop.inc).
namespace coop_loadout {
constexpr uint32_t kMagic = 0x324C4443, kVersion = 1;
constexpr unsigned kMaxSlots = 16;  // the inventory reader accepts at most 16

struct Packet {
    uint32_t magic, version, token, epoch, sequence, slot, slots, guid[4];
};
static_assert(sizeof(Packet) == 44, "Unexpected loadout wire layout");

struct Entry {
    uint32_t guid[4]{};
    bool have = false;

    bool Same(const Entry& other) const {
        if (have != other.have) return false;
        if (!have) return true;
        for (unsigned i = 0; i < 4; ++i)
            if (guid[i] != other.guid[i]) return false;
        return true;
    }
};

inline bool Valid(const Packet& p) {
    return p.magic == kMagic && p.version == kVersion && p.token && p.epoch &&
           p.slots && p.slots <= kMaxSlots && p.slot < p.slots;
}

// A host's companion bot is not a peer, so its slots travel in a packet (and a
// size) of their own, with the bot's index; its ghost's sub-key top byte is
// 0xF0 + bot.
constexpr uint32_t kBotMagic = 0x42444C43, kMaxBots = 8;
struct BotPacket {
    uint32_t magic, version, token, bot, epoch, sequence, slot, slots, guid[4], reserved[8];   // the side channel tells packets apart by size; 48 to 76 are taken
};
static_assert(sizeof(BotPacket) == 80, "Unexpected bot loadout wire layout");

inline BotPacket ToBot(const Packet& p, uint32_t bot) {
    return BotPacket{kBotMagic, p.version, p.token, bot, p.epoch, p.sequence, p.slot, p.slots,
                     {p.guid[0], p.guid[1], p.guid[2], p.guid[3]}, {}};
}
inline Packet FromBot(const BotPacket& b) {
    return Packet{kMagic, b.version, b.token, b.epoch, b.sequence, b.slot, b.slots,
                  {b.guid[0], b.guid[1], b.guid[2], b.guid[3]}};
}
inline bool ValidBot(const BotPacket& b) {
    return b.magic == kBotMagic && b.bot < kMaxBots && Valid(FromBot(b));
}

// Sender: republishes only when the carried set actually changes, then sweeps
// every slot a few times because the side channel is plain UDP.
struct Sender {
    uint32_t token = 0, epoch = 0, sequence = 0, lastSend = 0;
    unsigned slots = 0, cursor = 0, sweeps = 0;
    Entry items[kMaxSlots]{};

    bool Same(const Entry* other, unsigned count) const {
        if (count != slots) return false;
        for (unsigned i = 0; i < count; ++i)
            if (!items[i].Same(other[i])) return false;
        return true;
    }
    void Publish(const Entry* source, unsigned count, uint32_t now) {
        if (count > kMaxSlots) count = kMaxSlots;
        if (Same(source, count)) return;
        for (unsigned i = 0; i < count; ++i) items[i] = source[i];
        slots = count;
        if (++epoch == 0) ++epoch;
        cursor = 0;
        sweeps = 3;
        lastSend = now - 1000;  // allow an immediate first send
    }
    // A steady loadout is swept again every kRefreshMs, for a partner that lost
    // its copy (see coop_suit::Sender); the receiver ignores a known epoch.
    static constexpr uint32_t kRefreshMs = 10000;
    bool Due(uint32_t now) const { return slots && now - lastSend >= (sweeps ? 60u : kRefreshMs); }
    Packet MakePacket(uint32_t now) {
        if (!sweeps) { sweeps = 1; cursor = 0; }
        lastSend = now;
        const unsigned slot = cursor;
        Packet packet{kMagic, kVersion, token, epoch, ++sequence, slot, slots,
                      {items[slot].guid[0], items[slot].guid[1],
                       items[slot].guid[2], items[slot].guid[3]}};
        if (++cursor >= slots) {
            cursor = 0;
            if (sweeps) --sweeps;
        }
        return packet;
    }
};

// Receiver: assembles the slots of one epoch. generation increments once the
// set is complete, so a consumer acts on a whole loadout, never a half one.
struct Receiver {
    uint32_t peerToken = 0, epoch = 0, generation = 0, lastSeen = 0;
    unsigned slots = 0, filled = 0;
    Entry items[kMaxSlots]{};
    bool seen[kMaxSlots]{};
    bool complete = false;

    bool Accept(const Packet& p, uint32_t peerReadinessToken, uint32_t now) {
        if (!Valid(p)) return false;
        if (peerReadinessToken && p.token != peerReadinessToken) return false;
        lastSeen = now;
        if (p.token != peerToken || p.epoch != epoch) {
            // a new loadout: start over, including which slots were received
            peerToken = p.token;
            epoch = p.epoch;
            slots = p.slots;
            filled = 0;
            complete = false;
            for (unsigned i = 0; i < kMaxSlots; ++i) { items[i] = Entry{}; seen[i] = false; }
        }
        if (p.slots != slots || p.slot >= slots) return false;
        Entry entry;
        for (unsigned i = 0; i < 4; ++i) entry.guid[i] = p.guid[i];
        entry.have = (p.guid[0] | p.guid[1] | p.guid[2] | p.guid[3]) != 0;
        items[p.slot] = entry;             // an empty slot is still a known slot
        if (!seen[p.slot]) { seen[p.slot] = true; ++filled; }
        if (!complete && filled >= slots) {
            complete = true;
            ++generation;
            return true;
        }
        return false;
    }
};
}  // namespace coop_loadout
