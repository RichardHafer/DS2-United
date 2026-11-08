// Dead Space 2 United by Richard Hafer.
// The progression gate, checked without a game.
//
// The property that matters more than all the others: a held message must
// always come out again. A gate that can lose progression is a soft lock, and
// this project has already spent a night removing one of those.
#include <cassert>
#include <cstdio>
#include <array>
#include <map>
#include "../src/coop_gate.h"

namespace {
using coop_gate::Queue;
using coop_gate::kHeldCapacity;
constexpr uint32_t kTimeout = coop_gate::kDefaultTimeoutMs;
constexpr uint32_t ME = 0x1111, B = 0x2222, C = 0x3333, D = 0x4444;

// A little world of player positions, so the tests can walk people around.
struct World {
    std::map<uint32_t, std::array<float, 3>> at;
    void Put(uint32_t token, float x, float y, float z) { at[token] = {x, y, z}; }
    // The predicate the gate asks: where is this player now?
    bool operator()(uint32_t token, float out[3]) const {
        const auto found = at.find(token);
        if (found == at.end()) return false;
        for (int i = 0; i < 3; ++i) out[i] = found->second[i];
        return true;
    }
};
const float kLiftLeft[3]  = {0, 0, 0};
const float kLiftBack[3]  = {2, 0, 1};
const float kLiftRight[3] = {-2, 0, 1};
const float kLiftFront[3] = {0, 0, -2};
const float kCorridor[3]  = {60, 0, 0};

// Four players, one lift. It waits for the last one.
void LiftWaitsForEverybody() {
    Queue q; World w;
    uint32_t now = 1000, hash = 0, source = 0;
    bool timedOut = true;
    w.Put(ME, 0, 0, 0);
    assert(q.Hold(0xE1E1, 0x99, now, ME, kLiftLeft));
    assert(!q.Take(hash, source, 4, now, kTimeout, timedOut, w));
    w.Put(B, 2, 0, 1);   q.Vote(0xE1E1, 0x99, B, now + 500, kLiftBack);
    w.Put(C, -2, 0, 1);  q.Vote(0xE1E1, 0x99, C, now + 900, kLiftRight);
    assert(!q.Take(hash, source, 4, now + 900, kTimeout, timedOut, w));  // 3 of 4
    w.Put(D, 0, 0, -2);  q.Vote(0xE1E1, 0x99, D, now + 1200, kLiftFront);
    assert(q.Take(hash, source, 4, now + 1200, kTimeout, timedOut, w));
    assert(hash == 0xE1E1 && !timedOut && q.replayed == 1);
    std::printf("lift waits for everybody: released at 4 of 4\n");
}

// The lift case: three press the lift, the third steps back out, the fourth
// steps in and presses. Four votes, three passengers: it must not go.
void VoteDiesWhenTheVoterLeaves() {
    Queue q; World w;
    uint32_t now = 1000, hash = 0, source = 0;
    bool timedOut = false;
    constexpr uint32_t kLift = 0x11F70000;
    w.Put(ME, 0, 0, 0);  q.Hold(kLift, 0x55, now, ME, kLiftLeft);
    w.Put(B, 2, 0, 1);   q.Vote(kLift, 0x55, B, now + 100, kLiftBack);
    w.Put(C, -2, 0, 1);  q.Vote(kLift, 0x55, C, now + 200, kLiftRight);
    // C walks back out into the corridor; D walks in and presses.
    w.Put(C, 60, 0, 0);
    w.Put(D, 0, 0, -2);  q.Vote(kLift, 0x55, D, now + 300, kLiftFront);
    auto* item = q.Find(kLift);
    assert(item && item->voterCount == 4);              // four votes on paper
    assert(!q.Take(hash, source, 4, now + 300, kTimeout, timedOut, w));
    // C comes back to where he voted. Now it goes, with nobody pressing again.
    w.Put(C, -2, 0, 1);
    assert(q.Take(hash, source, 4, now + 400, kTimeout, timedOut, w));
    assert(!timedOut && hash == kLift);
    std::printf("vote dies when the voter leaves: 4 votes, 3 present, held\n");
}

// Shifting your feet inside the lift must not cost you your vote.
void SmallMovementKeepsTheVote() {
    Queue q; World w;
    uint32_t hash = 0, source = 0; bool timedOut = false;
    w.Put(ME, 0, 0, 0); q.Hold(0x7777, 0, 1000, ME, kLiftLeft);
    w.Put(ME, 3, 0, 3);                                  // a couple of steps
    assert(q.Take(hash, source, 1, 1000, kTimeout, timedOut, w));
    assert(!timedOut);
    std::printf("small movement keeps the vote\n");
}

// A door two players open from opposite sides. Each is measured against his own
// voting spot, so both count even though they are nowhere near each other.
void DoorFromBothSides() {
    Queue q; World w;
    uint32_t hash = 0, source = 0; bool timedOut = false;
    constexpr uint32_t kDoorHash = 0xD0000000;   // letters that are not hex digits
                                                 // have bitten this file twice today
    const float inFront[3] = {0, 0, 5};
    const float behind[3]  = {0, 0, -5};
    w.Put(ME, 0, 0, 5); q.Hold(kDoorHash, 0, 1000, ME, inFront);
    w.Put(B, 0, 0, -5); q.Vote(kDoorHash, 0, B, 1100, behind);
    assert(q.Take(hash, source, 2, 1100, kTimeout, timedOut, w));
    assert(!timedOut);
    std::printf("door from both sides: both votes count\n");
}

// The quorum per held message: a level change also waits for a dead partner watching
// (the spectator), a door next to it does not.
void QuorumPerMessage() {
    Queue q; World w;
    uint32_t hash = 0, source = 0; bool timedOut = true;
    constexpr uint32_t kLevelChange = 0x1EE1, kDoor = 0xD00D;
    w.Put(ME, 0, 0, 0);
    assert(q.Hold(kLevelChange, 0, 1000, ME, kLiftLeft));
    assert(q.Hold(kDoor, 0, 1001, ME, kLiftLeft));
    const auto need = [&](uint32_t h) { return h == kLevelChange ? 2u : 1u; };
    assert(q.TakeEach(hash, source, need, 1002, kTimeout, timedOut, w) && hash == kDoor && !timedOut);
    assert(!q.TakeEach(hash, source, need, 1003, kTimeout, timedOut, w));
    w.Put(B, 0, 0, 0); q.Vote(kLevelChange, 0, B, 1004, kLiftLeft);
    assert(q.TakeEach(hash, source, need, 1005, kTimeout, timedOut, w) && hash == kLevelChange && !timedOut);
    std::printf("quorum per message: the level change waited for the second player, the door did not\n");
}

// Alone, nothing waits.
void SoloIsItsOwnQuorum() {
    Queue q; World w;
    uint32_t hash = 0, source = 0; bool timedOut = true;
    w.Put(ME, 0, 0, 0);
    assert(q.Hold(0xAAAA, 0, 1000, ME, kLiftLeft));
    assert(q.Take(hash, source, 1, 1000, kTimeout, timedOut, w));
    assert(!timedOut);
    std::printf("solo is its own quorum: through immediately\n");
}

// Somebody never turns up. The message must still come out.
void TimeoutAlwaysReleases() {
    Queue q; World w;
    uint32_t now = 1000, hash = 0, source = 0; bool timedOut = false;
    w.Put(ME, 0, 0, 0);
    q.Hold(0xBBBB, 0, now, ME, kLiftLeft);
    assert(!q.Take(hash, source, 4, now + kTimeout - 1, kTimeout, timedOut, w));
    assert(q.Take(hash, source, 4, now + kTimeout + 1, kTimeout, timedOut, w));
    assert(hash == 0xBBBB && timedOut && q.expired == 1);
    std::printf("timeout releases: expired=%u\n", q.expired);
}

// A partner who got there first must not have his vote thrown away.
void VoteBeforeHold() {
    Queue q; World w;
    uint32_t hash = 0, source = 0; bool timedOut = true;
    w.Put(B, 2, 0, 1);
    q.Vote(0xCAFE, 0x77, B, 1000, kLiftBack);
    assert(q.Waiting() == 1 && q.votesLate == 1);
    assert(!q.Take(hash, source, 2, 1000, kTimeout, timedOut, w));
    w.Put(ME, 0, 0, 0);
    assert(q.Hold(0xCAFE, 0x77, 1100, ME, kLiftLeft));
    assert(q.Take(hash, source, 2, 1100, kTimeout, timedOut, w));
    assert(!timedOut);
    std::printf("vote before hold: kept, released at 2 of 2\n");
}

// Walking in and out of a trigger is one vote, not ten.
void RepeatsAreOneVote() {
    Queue q; World w;
    w.Put(ME, 0, 0, 0);
    for (int i = 0; i < 10; ++i) q.Hold(0xCCCC, 0, 1000 + i, ME, kLiftLeft);
    auto* item = q.Find(0xCCCC);
    assert(item && item->voterCount == 1 && item->attempts == 10);
    uint32_t hash = 0, source = 0; bool timedOut = false;
    assert(!q.Take(hash, source, 2, 2000, kTimeout, timedOut, w));
    std::printf("repeats are one vote: attempts=%u voters=%u\n",
                item->attempts, item->voterCount);
}

// A partner's vote for something this machine never produced is not a message.
// It waits in case the local copy arrives, and when it does not, it is cleared,
// never fired, because nothing here ever sent it.
void StrayVoteNeverFires() {
    Queue q; World w;
    uint32_t hash = 0, source = 0; bool timedOut = false;
    w.Put(B, 2, 0, 1);
    q.Vote(0x5714, 0x11, B, 1000, kLiftBack);
    assert(q.Waiting() == 1);
    // Timed out: cleared, not returned.
    assert(!q.Take(hash, source, 2, 1000 + kTimeout + 1, kTimeout, timedOut, w));
    assert(q.Waiting() == 0 && q.discarded == 1 && q.expired == 0);
    // Quorum shrinking to one around a lone foreign vote: cleared as well.
    q.Vote(0x5715, 0x11, B, 2000, kLiftBack);
    assert(!q.Take(hash, source, 1, 2000, kTimeout, timedOut, w));
    assert(q.Waiting() == 0 && q.discarded == 2);
    std::printf("stray vote never fires: discarded=%u\n", q.discarded);
}

// A vote cast before the voter had a position counts wherever he is now.
// Eight players, 2026-09-17: every level-start hold timed out because the
// votes were anchored to (0,0,0).
void AnchorlessVoteCounts() {
    Queue q; World w;
    uint32_t hash = 0, source = 0; bool timedOut = false;
    w.Put(ME, 0, 0, 0); q.Hold(0xA0A0, 0, 1000, ME, kLiftLeft);
    w.Put(B, 300, 0, 300);                                  // far from anywhere
    q.Vote(0xA0A0, 0, B, 1100, nullptr, false);            // no anchor
    assert(q.Take(hash, source, 2, 1100, kTimeout, timedOut, w));
    assert(!timedOut);
    std::printf("anchorless vote counts: released at 2 of 2\n");
}

// A full queue leaks rather than drops.
void FullQueueLeaks() {
    Queue q;
    for (unsigned i = 0; i < kHeldCapacity; ++i)
        assert(q.Hold(0x1000 + i, 0, 1000, ME, kLiftLeft));
    assert(!q.Hold(0xDEAD, 0, 1000, ME, kLiftLeft));
    assert(q.dropped == 1);
    std::printf("full queue leaks: dropped=%u (caller passes it on)\n", q.dropped);
}

// Everything that goes in comes out, one way or the other.
void NothingIsEverLost() {
    Queue q; World w;
    uint32_t now = 1000;
    unsigned accepted = 0;
    for (unsigned i = 0; i < kHeldCapacity + 4; ++i)
        if (q.Hold(0x2000 + i, 0, now + i, ME, kLiftLeft)) ++accepted;
    uint32_t hash = 0, source = 0; bool timedOut = false;
    unsigned out = 0;
    now += kHeldCapacity + 4 + kTimeout + 1;   // past the LAST one's timeout
    while (q.Take(hash, source, 4, now, kTimeout, timedOut, w)) ++out;
    assert(out == accepted);
    assert(q.replayed + q.expired == accepted);
    assert(q.dropped == 4);
    std::printf("nothing lost: in=%u out=%u passed_through=%u\n",
                accepted, out, q.dropped);
}
}  // namespace

int main() {
    LiftWaitsForEverybody();
    VoteDiesWhenTheVoterLeaves();
    SmallMovementKeepsTheVote();
    DoorFromBothSides();
    SoloIsItsOwnQuorum();
    QuorumPerMessage();
    TimeoutAlwaysReleases();
    VoteBeforeHold();
    RepeatsAreOneVote();
    StrayVoteNeverFires();
    AnchorlessVoteCounts();
    FullQueueLeaks();
    NothingIsEverLost();
    std::printf("coop gate tests passed\n");
    return 0;
}
