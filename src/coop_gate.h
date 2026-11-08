// Dead Space 2 United by Richard Hafer.
#pragma once
#include <cstdint>

// Nobody rides the elevator alone.
//
// Dead Space 3's co-op will not let one player leave the others behind: a lift,
// a door, a chapter transition all wait until everybody is there. Without that,
// two players in a streamed campaign drift into different parts of the ship and
// the doors, scripted rooms and enemies keyed to a player's presence break.
//
// The condition is a quorum on the event, not a distance: hold until every
// player has triggered the same thing (pressed the elevator, opened the door,
// walked into the trigger). A release radius is wrong in a big room, in a long
// corridor and in a room with two levels; "everyone pressed it" needs no
// number, because the game already decides who is close enough to press.
//
// It is also symmetric: each machine holds its own copy and releases when the
// votes are in. No host has to arbitrate.
//
// The gate is deliberately NOT a list of message names. Names differ per level
// and there are hundreds; a list would be stale the day after it was written. A
// progression message is recognised by *who listens to it*: if any listener is
// a mover, a door or a scripted sequence, the message takes people somewhere.
//
// It never swallows: a swallowed one-shot trigger is a permanent soft lock (the
// failure removed from the randomizer's quarantines). A held message is
// replayed once the quorum is in, and if it never is, the timeout lets it
// through anyway. The gate can delay progress, never destroy it.
namespace coop_gate {

// After this long the gate gives up and lets the message through. A gate that
// can block forever is worse than no gate, because the failure is silent and
// looks like a broken level. Long enough to walk back for somebody, short
// enough that a disconnected player does not end the session.
// Three minutes since 2026-09-17 17:00: with the ghosts no longer pulling
// triggers, a cutscene the host walks into is held until the partner walks
// into it too, and 45 s was not enough to go and get him. The partner is told
// on screen that he is being waited for.
constexpr uint32_t kDefaultTimeoutMs = 180000;
// A partner's vote that arrived before the local hold lives this long. Any
// longer and it answers the NEXT occurrence of the same message: the 15:45
// run released every repeat of a stream activation in 16 ms on votes cast
// for the one before, and the two worlds drifted apart on it.
constexpr uint32_t kLateVoteMs = 10000;
constexpr unsigned kHeldCapacity = 16;
constexpr unsigned kMaxVoters = 8;

// How far a voter may drift from the spot he voted at before his vote stops
// counting. Deliberately generous: shifting your feet inside the lift must not
// cost you your vote, walking back out into the corridor must.
//
// This is measured against the voter's own voting position, never against the
// other players and never against the button: the button's reach is barely one
// body wide, so four people could never stand in it at once, and a radius
// between players is wrong in a big room, in a corridor and at a door two
// people open from opposite sides.
constexpr float kDefaultDriftLimit = 12.0f;

inline float DistanceSquared(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

// A vote is not a latch. Three players press the elevator, the third walks
// back out, the fourth walks in and presses: four votes, three passengers, and
// the lift leaves somebody behind. So a vote only counts while that player is
// still at the thing they voted on (near the elevator, not near the other
// players).
//
// Presence is asked for at the moment the quorum is counted, never cached, so
// stepping out takes the vote with it.
struct Held {
    bool used = false;
    uint32_t hash = 0;
    uint32_t source = 0;
    uint32_t heldSince = 0;
    uint32_t attempts = 0;
    uint32_t voters[kMaxVoters]{};
    // Where each voter stood when he voted. That spot, not the button and not
    // the other players, is what his vote is anchored to.
    float votedAt[kMaxVoters][3]{};
    // A vote cast before the voter had a position (a level-start sequence
    // fires before the Player exists) has no spot to be measured against.
    // It counts for as long as the voter is known at all. Measured 2026-09-17
    // with eight players: every start-of-level hold timed out because those
    // votes were anchored to (0,0,0), a place nobody ever stood.
    bool anchored[kMaxVoters]{};
    unsigned voterCount = 0;
    // A shorter patience for this entry (0 = the queue's). A scripted
    // sequence the partner may never be able to produce (an enemy's intro on
    // the host) should not stall for the full door timeout.
    uint32_t timeoutMs = 0;

    int IndexOf(uint32_t token) const {
        for (unsigned i = 0; i < voterCount; ++i)
            if (voters[i] == token) return static_cast<int>(i);
        return -1;
    }
    bool HasVote(uint32_t token) const { return IndexOf(token) >= 0; }
    bool AddVote(uint32_t token, const float at[3], bool hasAnchor = true) {
        if (!token || HasVote(token)) return false;
        if (voterCount >= kMaxVoters) return false;
        voters[voterCount] = token;
        anchored[voterCount] = hasAnchor && at != nullptr;
        if (at) for (int i = 0; i < 3; ++i) votedAt[voterCount][i] = at[i];
        ++voterCount;
        return true;
    }
    // How many of the voters are still standing where they voted.
    //
    // `where(token, out)` fills the voter's current position and returns false
    // when that player cannot be located at all: somebody who has gone is not
    // counted as present, nor as having moved, because the timeout resolves a
    // partner who left the session.
    template <class Where>
    unsigned LiveVotes(Where where, float driftLimit) const {
        const float limit = driftLimit * driftLimit;
        unsigned live = 0;
        for (unsigned i = 0; i < voterCount; ++i) {
            float now[3]{};
            if (!where(voters[i], now)) continue;
            if (!anchored[i] || DistanceSquared(now, votedAt[i]) <= limit) ++live;
        }
        return live;
    }
};

struct Queue {
    Held items[kHeldCapacity]{};
    uint32_t held = 0, replayed = 0, expired = 0, dropped = 0, votesIn = 0, votesLate = 0;
    // Entries that only ever held a partner's vote and never the local message.
    // When one of those times out there is nothing to replay: this machine's
    // level logic never produced the message, and injecting it would fire a
    // sequence here that only happened over there.
    uint32_t discarded = 0;

    Held* Find(uint32_t hash) {
        for (auto& item : items) if (item.used && item.hash == hash) return &item;
        return nullptr;
    }

    // Remember a message to replay later, and count the local vote for it. A hash
    // already waiting is not stored twice: a trigger the player keeps walking
    // into would otherwise queue the same progression a dozen times and fire it
    // a dozen times when the gate opens.
    bool Hold(uint32_t hash, uint32_t source, uint32_t now, uint32_t selfToken,
              const float at[3], bool hasAnchor = true, uint32_t timeoutOverrideMs = 0) {
        if (Held* existing = Find(hash)) {
            ++existing->attempts;
            existing->AddVote(selfToken, at, hasAnchor);
            if (timeoutOverrideMs) existing->timeoutMs = timeoutOverrideMs;
            return true;
        }
        for (auto& item : items) {
            if (item.used) continue;
            item = Held{};
            item.used = true; item.hash = hash; item.source = source;
            item.heldSince = now; item.attempts = 1; item.timeoutMs = timeoutOverrideMs;
            item.AddVote(selfToken, at, hasAnchor);
            ++held;
            return true;
        }
        // No room. Letting it through is the safe direction: a gate that drops
        // progression is a soft lock, a gate that leaks one is a player who got
        // ahead.
        ++dropped;
        return false;
    }

    // A partner says it reached this message too.
    //
    // A vote for something not held here is kept, not discarded: the other
    // machine may have got there first, and throwing the vote away would
    // mean the local hold never reaches quorum. It is recorded as a fresh entry
    // with their vote on it and no local vote, so it waits for this machine.
    void Vote(uint32_t hash, uint32_t source, uint32_t token, uint32_t now,
              const float at[3], bool hasAnchor = true) {
        if (Held* existing = Find(hash)) {
            if (existing->AddVote(token, at, hasAnchor)) ++votesIn;
            return;
        }
        for (auto& item : items) {
            if (item.used) continue;
            item = Held{};
            item.used = true; item.hash = hash; item.source = source;
            item.heldSince = now; item.attempts = 0;
            item.AddVote(token, at, hasAnchor);
            ++held;
            ++votesLate;
            return;
        }
        ++dropped;
    }

    // Everything whose quorum is in, oldest first; `required` is how many
    // players must have reached it, counting ourselves. `timedOut` marks the
    // ones released because the others never arrived.
    template <class Where>
    bool Take(uint32_t& hash, uint32_t& source, unsigned required, uint32_t now,
              uint32_t timeoutMs, bool& timedOut, Where where,
              float driftLimit = kDefaultDriftLimit) {
        return TakeEach(hash, source, [required](uint32_t) { return required; }, now, timeoutMs, timedOut, where, driftLimit);
    }
    // The same with the quorum asked per held message (`need(hash)`).
    template <class Where, class Need>
    bool TakeEach(uint32_t& hash, uint32_t& source, Need need, uint32_t now,
                  uint32_t timeoutMs, bool& timedOut, Where where,
                  float driftLimit = kDefaultDriftLimit) {
        Held* oldest = nullptr;
        bool oldestTimedOut = false;
        for (auto& item : items) {
            if (!item.used) continue;
            // Counted fresh every time, so a player who stepped back out of the
            // elevator stops counting the moment he does.
            const bool quorum = item.LiveVotes(where, driftLimit) >= need(item.hash);
            const uint32_t patience = item.timeoutMs ? item.timeoutMs : timeoutMs;
            const bool late = now - item.heldSince >= (item.attempts ? patience : kLateVoteMs);
            if (!quorum && !late) continue;
            // A partner's vote never matched by a local message has nothing
            // behind it to replay; only the local Hold sets attempts. Whether
            // it timed out or the quorum shrank around it, it is cleared, never
            // fired.
            if (item.attempts == 0) {
                item = Held{};
                ++discarded;
                continue;
            }
            if (!oldest || static_cast<int32_t>(item.heldSince - oldest->heldSince) < 0) {
                oldest = &item;
                oldestTimedOut = !quorum;
            }
        }
        if (!oldest) return false;
        hash = oldest->hash;
        source = oldest->source;
        timedOut = oldestTimedOut;
        if (timedOut) ++expired; else ++replayed;
        *oldest = Held{};
        return true;
    }

    // Oldest entry that holds only partners' votes and no local one, older
    // than `ageMs`: the partner is waiting for this player somewhere.
    Held* PartnerWait(uint32_t now, uint32_t ageMs) {
        Held* best = nullptr;
        for (auto& item : items) {
            if (!item.used || item.attempts != 0 || now - item.heldSince < ageMs) continue;
            if (!best || static_cast<int32_t>(item.heldSince - best->heldSince) < 0) best = &item;
        }
        return best;
    }

    unsigned Waiting() const {
        unsigned n = 0;
        for (const auto& item : items) if (item.used) ++n;
        return n;
    }
};
}  // namespace coop_gate
