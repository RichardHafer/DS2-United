// Dead Space 2 United by Hafer.
#pragma once
#include <cstdint>

// Round-trip time, jitter and packet loss on the side channel.
//
// The sync code was tuned on one machine or a LAN, where a packet is never
// late, out of order or gone. This makes the delay visible before anything is
// tuned for it. It only measures and changes no game state.
//
// ExchangePlayerState dispatches by received length, so every side-channel
// packet needs a size of its own (static_assert in runtime_coop.inc).
namespace coop_ping {
constexpr uint32_t kMagic = 0x474E5043, kVersion = 1;   // 'CPNG'
constexpr uint32_t kRequest = 1, kReply = 2;

struct Packet {
    uint32_t magic, version, kind, token, sequence, originTicks, peerTicks;
};
static_assert(sizeof(Packet) == 28, "Unexpected ping wire layout");

inline bool Valid(const Packet& p) {
    return p.magic == kMagic && p.version == kVersion &&
           (p.kind == kRequest || p.kind == kReply);
}

// A request is answered with the sender's own origin stamp copied back
// untouched. That is deliberate: the two machines never have to agree on a
// clock, and the round trip is then a subtraction on the machine that asked.
// `peerTicks` is the responder's own tick count, carried only so a later
// stage can estimate one-way skew; nothing here uses it.
inline Packet MakeReply(const Packet& request, uint32_t token, uint32_t now) {
    return {kMagic, kVersion, kReply, token, request.sequence,
            request.originTicks, now};
}

constexpr uint32_t kIntervalMs = 500;
// How long an unanswered request waits before it counts as lost. Generous on
// purpose: a slow link is not a lost packet, and counting it as one would make
// the loss figure say "bad connection" when it means "far away".
constexpr uint32_t kLostAfterMs = 3000;
constexpr unsigned kWindow = 32;

// One outstanding request.
struct Outstanding {
    uint32_t sequence = 0, sentAt = 0;
    bool answered = false, used = false;
};

// Rolling view of the link. All times are millisecond tick counts from the
// caller, so this stays testable without a game or a clock.
struct Monitor {
    uint32_t token = 0;
    uint32_t sequence = 0, lastSend = 0;
    Outstanding window[kWindow]{};

    uint32_t sent = 0, answered = 0, lost = 0;
    uint32_t lastRtt = 0, minRtt = 0, maxRtt = 0;
    // Smoothed round trip and smoothed deviation, the way RTP does it: a
    // sixteenth of the error each time. An average alone hides the thing that
    // actually ruins a shooter, which is the spread.
    uint32_t smoothedRtt = 0, jitter = 0;
    // Replies that arrived for a sequence no longer in the window, or for one
    // already answered. A duplicate is not an error, but it is worth seeing.
    uint32_t stray = 0;
    // Replies that arrived after a later reply already had. Plain UDP, so this
    // is expected and only interesting as a count.
    uint32_t reordered = 0;
    uint32_t highestAnswered = 0;

    bool Due(uint32_t now) const {
        return !lastSend || now - lastSend >= kIntervalMs;
    }

    Packet MakeRequest(uint32_t now) {
        lastSend = now;
        ++sequence;
        ++sent;
        // Oldest slot wins, so a stalled link cannot starve the window.
        unsigned slot = 0;
        for (unsigned i = 0; i < kWindow; ++i) {
            if (!window[i].used) { slot = i; break; }
            if (window[slot].used &&
                static_cast<int32_t>(window[i].sentAt - window[slot].sentAt) < 0)
                slot = i;
        }
        // Evicting a slot that was never answered is exactly a lost packet, and
        // the only place that can be noticed without a timer running.
        if (window[slot].used && !window[slot].answered) ++lost;
        window[slot] = {sequence, now, false, true};
        return {kMagic, kVersion, kRequest, token, sequence, now, 0};
    }

    // Age out requests nobody answered. Called from the same poll as Due.
    void Expire(uint32_t now) {
        for (auto& slot : window) {
            if (!slot.used || slot.answered) continue;
            if (now - slot.sentAt < kLostAfterMs) continue;
            ++lost;
            slot.used = false;
        }
    }

    // A reply came back. Returns true when it answers an outstanding request.
    bool OnReply(const Packet& p, uint32_t now) {
        if (!Valid(p) || p.kind != kReply) return false;
        Outstanding* match = nullptr;
        for (auto& slot : window)
            if (slot.used && slot.sequence == p.sequence) { match = &slot; break; }
        if (!match || match->answered) { ++stray; return false; }
        match->answered = true;
        ++answered;
        // The origin stamp is ours, so this subtraction needs no shared clock.
        // Unsigned wrap is correct here and is why it is not a signed compare.
        const uint32_t rtt = now - p.originTicks;
        lastRtt = rtt;
        if (!minRtt || rtt < minRtt) minRtt = rtt;
        if (rtt > maxRtt) maxRtt = rtt;
        if (!smoothedRtt) {
            smoothedRtt = rtt;
        } else {
            const uint32_t larger = rtt > smoothedRtt ? rtt : smoothedRtt;
            const uint32_t smaller = rtt > smoothedRtt ? smoothedRtt : rtt;
            const uint32_t deviation = larger - smaller;
            jitter += (deviation > jitter ? deviation - jitter : 0) / 16;
            if (deviation < jitter) jitter -= (jitter - deviation) / 16;
            smoothedRtt += (rtt > smoothedRtt ? (rtt - smoothedRtt) / 8 : 0);
            if (rtt < smoothedRtt) smoothedRtt -= (smoothedRtt - rtt) / 8;
        }
        if (static_cast<int32_t>(p.sequence - highestAnswered) > 0)
            highestAnswered = p.sequence;
        else
            ++reordered;
        return true;
    }

    // Percent, rounded down, of requests that never came back. Zero until a
    // handful have been sent, because one lost packet out of two is not "50%
    // loss", it is not enough to say anything.
    unsigned LossPercent() const {
        if (sent < 8) return 0;
        const uint32_t accounted = answered + lost;
        if (!accounted) return 0;
        return static_cast<unsigned>(lost * 100u / accounted);
    }
};
}  // namespace coop_ping
