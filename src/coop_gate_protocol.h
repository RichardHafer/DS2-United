// Dead Space 2 United by Hafer.
#pragma once
#include <cmath>
#include <cstdint>

// A vote on the wire: "my player reached this progression message, standing
// here". One datagram per vote, repeated for a while, because the side channel
// is plain UDP and a lost vote is a lift that waits for nobody.
//
// 40 bytes. ExchangePlayerState dispatches by received length, so every side-channel
// packet needs a size of its own (static_assert in runtime_coop.inc).
namespace coop_gate_wire {
constexpr uint32_t kMagic = 0x45544F56;   // 'VOTE'
constexpr uint32_t kVersion = 1;

// The host forwards a client's vote to the other clients with this set, so a
// forwarded vote is filed under the voter's token and never forwarded again.
constexpr uint32_t kFlagRelayed = 1;
// The voter had no position yet (level start, before the Player exists); the
// receiver counts the vote without measuring it against a spot.
constexpr uint32_t kFlagNoAnchor = 2;

struct Packet {
    uint32_t magic, version, token, sequence;
    uint32_t hash, source;
    float x, y, z;
    uint32_t flags;
};
static_assert(sizeof(Packet) == 40, "Unexpected vote wire layout");

inline bool Valid(const Packet& p) {
    return p.magic == kMagic && p.version == kVersion && p.token && p.hash &&
           std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

// Keeps repeating a vote after it was cast.
//
// Resending only while still holding has a hole: A and B
// both press, A's vote reaches B, B's vote is lost. B has both votes and
// releases; A keeps repeating to a partner who no longer cares, and waits for
// a vote B will never send again because B stopped holding. So a vote is
// repeated for a fixed time after it was cast, whether or not the caster still
// holds anything.
constexpr uint32_t kRepeatMs = 15000;   // the default; the runtime sets it to the gate timeout plus a margin
constexpr uint32_t kRepeatIntervalMs = 400;
constexpr unsigned kAnnounceCapacity = 16;

struct Announcer {
    struct Entry {
        bool used = false;
        uint32_t hash = 0, source = 0, castAt = 0, lastSent = 0;
        float at[3]{};
        bool anchored = true;
    };
    Entry entries[kAnnounceCapacity]{};
    uint32_t token = 0, sequence = 0;
    // How long a vote keeps going out. Fifteen seconds was not enough: a
    // partner who loads the level a minute later never hears a vote that
    // stopped repeating after fifteen, and holds until his own timeout.
    uint32_t repeatMs = kRepeatMs;
    uint32_t cast = 0, repeated = 0, evicted = 0;

    // A vote on a hash already announced restarts its repeat window rather
    // than adding a second entry, so walking in and out of a trigger costs
    // nothing extra on the wire.
    void Cast(uint32_t hash, uint32_t source, const float at[3], uint32_t now, bool anchored = true) {
        Entry* slot = nullptr;
        for (auto& e : entries) if (e.used && e.hash == hash) { slot = &e; break; }
        if (!slot) for (auto& e : entries) if (!e.used) { slot = &e; break; }
        if (!slot) {
            // Full: the oldest gives way. Its partner had 15 s of repeats.
            slot = &entries[0];
            for (auto& e : entries)
                if (static_cast<int32_t>(e.castAt - slot->castAt) < 0) slot = &e;
            ++evicted;
        }
        slot->used = true;
        slot->hash = hash; slot->source = source;
        slot->castAt = now;
        slot->lastSent = now - kRepeatIntervalMs;   // goes out on the next Due
        for (int i = 0; i < 3; ++i) slot->at[i] = at ? at[i] : 0.0f;
        slot->anchored = anchored && at != nullptr;
        ++cast;
    }

    // Everything due for another repeat, one packet each; expired entries are
    // retired on the way.
    template <class Send>
    void Repeat(uint32_t now, Send send) {
        for (auto& e : entries) {
            if (!e.used) continue;
            if (now - e.castAt > repeatMs) { e = Entry{}; continue; }
            if (now - e.lastSent < kRepeatIntervalMs) continue;
            e.lastSent = now;
            Packet p{kMagic, kVersion, token, ++sequence, e.hash, e.source,
                     e.at[0], e.at[1], e.at[2], e.anchored ? 0u : kFlagNoAnchor};
            send(p);
            ++repeated;
        }
    }

    // The hold this vote belonged to is over. It keeps going out for
    // lingerMs more, not longer, or it answers the next occurrence of the
    // same message on the other side. Stopping at once reopened the hole
    // described above: a machine that reached its quorum on the first vote
    // had sent exactly one datagram, and if that one was lost the partner
    // waited out his whole timeout.
    void Retire(uint32_t hash, uint32_t now, uint32_t lingerMs) {
        for (auto& e : entries) {
            if (!e.used || e.hash != hash) continue;
            if (now - e.castAt + lingerMs < repeatMs) e.castAt = now + lingerMs - repeatMs;
        }
    }
    void Clear() {
        for (auto& e : entries) e = Entry{};
    }

    unsigned Active() const {
        unsigned n = 0;
        for (const auto& e : entries) if (e.used) ++n;
        return n;
    }
};
}  // namespace coop_gate_wire
