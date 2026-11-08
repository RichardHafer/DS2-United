// Dead Space 2 United by Hafer.
// The MP maps' objective chain runs once per round, on every machine, from
// whoever fires a step first ([Mirror] MpObjectivesOnce).
//
// Every machine runs the whole map logic. 13.10. on Marker Lab the client's
// completions never reached the host: each obj_N_complete is also heard by a
// MultiRemoveTarget (it removes an objective indicator the visit never spawns),
// so the mirror kept it local, and the host's marker stayed invincible under the
// client's hits (analysis/MP_OBJECTIVE_SYNC_2026-10-13.md). The set is the five
// announcements (obj_N_new) plus the map's step messages from the Zone plans
// (generated_zone_objectives.h) and obj_N_complete, which the round clock's TimerSets
// hear on maps whose plan steps are other messages (13.10., Versus on mp_bombRun: the
// host kept obj_1_complete, an OrEventGate listens, the client's relayed manager dropped
// it as mirrored anyway, and the client's clock ran out 390 s early), and every
// message the map's round clock adds time on (generated_objective_table.h: the fuse
// timers, timerSet_objN_add and the like). The first copy
// of each, local or a partner's, runs; every later one is swallowed, so a prop dying
// late on one machine or a relayed manager does not run a chain twice. Cleared on
// every arrival run.
//
// A late joiner: the host journals each step it saw, and the joiner's history
// replays them here in the plan's order and pace (each step's relay tail), sent
// through the full dispatch and kept off the wire like a Zone step.
// Not on Zone visits, where every machine opens the map itself.
#include "generated_objective_table.h"
namespace objonce {
bool enabled = true;
constexpr unsigned kMax = 30;   // bits of doneMask and catchUpMask
constexpr uint32_t kNew[5] = {gate::NameHash("obj_1_new"), gate::NameHash("obj_2_new"), gate::NameHash("obj_3_new"), gate::NameHash("obj_4_new"),
                              gate::NameHash("obj_5_new")};
constexpr const char* kNewNames[5] = {"obj_1_new", "obj_2_new", "obj_3_new", "obj_4_new", "obj_5_new"};
constexpr uint32_t kComplete[5] = {gate::NameHash("obj_1_complete"), gate::NameHash("obj_2_complete"), gate::NameHash("obj_3_complete"),
                                   gate::NameHash("obj_4_complete"), gate::NameHash("obj_5_complete")};
constexpr const char* kCompleteNames[5] = {"obj_1_complete", "obj_2_complete", "obj_3_complete", "obj_4_complete", "obj_5_complete"};
constexpr int kAnnouncement = -1, kCompletion = -2;   // batchOf for the names not in the plan
uint32_t hashes[kMax]{};
const char* names[kMax]{};
int batchOf[kMax]{};   // the plan step, kAnnouncement or kCompletion
int msgOf[kMax]{};     // the message's place in its step
volatile LONG count = 0;
int planIndex = -1;
uint32_t epochSeen = 0xFFFFFFFFu;
volatile LONG doneMask = 0;
volatile LONG catchUpMask = 0;   // steps the host's history says are done, not yet sent here
DWORD catchUpAt = 0;
volatile LONG localFirst = 0, swallowed = 0, soloRepeats = 0, replayDropped = 0, replayFirst = 0, journaled = 0, historyQueued = 0,
              historyAnnouncementsSkipped = 0, historyDone = 0, catchUpBatches = 0, sentAnyway = 0, resets = 0, lines = 0;

int IndexOf(uint32_t hash) {
    const LONG n = InterlockedCompareExchange(&count, 0, 0);
    for (LONG i = 0; i < n; ++i) if (hashes[i] == hash) return static_cast<int>(i);
    return -1;
}
// Game thread, from Tick: a new arrival run starts a new round.
void Refresh() {
    if (mpmap::visit.epochs == epochSeen) return;
    epochSeen = mpmap::visit.epochs;
    InterlockedExchange(&count, 0);
    InterlockedExchange(&doneMask, 0);
    InterlockedExchange(&catchUpMask, 0);
    InterlockedExchange(&zoneobj::catchUpPlan, -1);
    catchUpAt = 0;
    char map[64] = "";
    planIndex = zoneobj::CurrentMapIndex(map, sizeof(map));
    LONG n = 0;
    for (unsigned i = 0; i < 5; ++i) { hashes[n] = kNew[i]; names[n] = kNewNames[i]; batchOf[n] = kAnnouncement; msgOf[n] = 0; ++n; }
    unsigned steps = 0;
    if (const coop_zoneobj::MapPlan* p = coop_zoneobj::PlanFor(planIndex)) {
        for (unsigned s = 0; s < p->stepCount; ++s)
            for (unsigned m = 0; m < p->steps[s].count && n < static_cast<LONG>(kMax); ++m) {
                hashes[n] = p->steps[s].messages[m].hash;
                names[n] = p->steps[s].messages[m].name;
                batchOf[n] = static_cast<int>(s);
                msgOf[n] = static_cast<int>(m);
                ++n;
                ++steps;
            }
    }
    // the completions and the round clock's messages the plan does not name already
    auto addCompletion = [&](uint32_t hash, const char* name) {
        if (!hash || n >= static_cast<LONG>(kMax)) return;
        for (LONG k = 0; k < n; ++k) if (hashes[k] == hash) return;
        hashes[n] = hash; names[n] = name && name[0] ? name : "(round clock)"; batchOf[n] = kCompletion; msgOf[n] = 0; ++n;
    };
    for (unsigned i = 0; i < 5; ++i) addCompletion(kComplete[i], kCompleteNames[i]);
    unsigned clock = 0;
    for (const auto& m : ds2::kObjectiveMaps)
        if (_stricmp(m.map, map) == 0)
            for (unsigned i = 0; i < m.count; ++i, ++clock) addCompletion(m.bonuses[i].hash, m.bonuses[i].name);
    InterlockedExchange(&count, n);
    InterlockedIncrement(&resets);
    mpmap::Event("objonce %s (arrival %u): %s; %ld message(s) run once per round (5 announcements, %u step message(s) of the plan, the rest completions and "
                 "the round clock's %u)", map, epochSeen,
                 !enabled ? "OFF ([Mirror] MpObjectivesOnce=0)"
                 : zoneobj::OpensMap(InterlockedCompareExchange(&gamemode::mode, 0, 0)) ? "the mode opens the map itself, not used"
                                                                                        : "first copy wins",
                 n, steps, clock);
}
bool Active() {
    return enabled && mpmap::Arrived() && mpmap::visit.epochs == epochSeen && InterlockedCompareExchange(&count, 0, 0) > 0 &&
           !zoneobj::OpensMap(InterlockedCompareExchange(&gamemode::mode, 0, 0));
}
bool Is(uint32_t hash) { return Active() && IndexOf(hash) >= 0; }
bool Done(uint32_t hash) {
    const int i = Active() ? IndexOf(hash) : -1;
    return i >= 0 && (InterlockedCompareExchange(&doneMask, 0, 0) & (1L << i)) != 0;
}
// True when this call marked it (its first copy this round).
bool Mark(uint32_t hash) {
    const int i = Active() ? IndexOf(hash) : -1;
    if (i < 0) return false;
    return (InterlockedOr(&doneMask, 1L << i) & (1L << i)) == 0;
}

// The gate, on a local dispatch (neither replayed nor mirrored): true swallows it.
bool OnLocal(uint32_t hash) {
    const int i = Active() ? IndexOf(hash) : -1;
    if (i < 0) return false;
    if (Mark(hash)) {
        InterlockedIncrement(&localFirst);
        if (mpmap::Budget(lines, 120)) mpmap::Event("objonce %s (%08X) ran here first this round", names[i], hash);
        return false;
    }
    if (!peerTable.Count()) { InterlockedIncrement(&soloRepeats); return false; }   // alone the map does what it always does
    InterlockedIncrement(&swallowed);
    if (mpmap::Budget(lines, 120)) mpmap::Event("objonce %s (%08X) of ours swallowed: it already ran here this round", names[i], hash);
    return true;
}

// The mirror, about to replay a partner's copy: false drops it.
bool AllowReplay(uint32_t hash) {
    const int i = Active() ? IndexOf(hash) : -1;
    if (i < 0) return true;
    if (!Done(hash)) return true;
    InterlockedIncrement(&replayDropped);
    if (mpmap::Budget(lines, 120)) mpmap::Event("objonce %s (%08X) of a partner dropped: it already ran here this round", names[i], hash);
    return false;
}
void NoteReplayed(uint32_t hash) {
    const int i = IndexOf(hash);
    InterlockedIncrement(&replayFirst);
    if (i >= 0 && mpmap::Budget(lines, 120)) mpmap::Event("objonce %s (%08X) of a partner ran here first this round", names[i], hash);
}

// A history entry on a joiner: true when objonce takes it. A step waits for the
// catch-up, an announcement comes with the chain of the step before it.
bool FromHistory(uint32_t hash) {
    const int i = Active() ? IndexOf(hash) : -1;
    if (i < 0) return false;
    if (Done(hash)) { InterlockedIncrement(&historyDone); return true; }
    if (batchOf[i] == kCompletion) return false;   // replayed as history: the clock's TimerSets want it
    if (batchOf[i] == kAnnouncement) {
        InterlockedIncrement(&historyAnnouncementsSkipped);
        if (mpmap::Budget(lines, 120)) mpmap::Event("objonce history %s (%08X) not replayed: the step before it announces it", names[i], hash);
        return true;
    }
    InterlockedOr(&catchUpMask, 1L << i);
    InterlockedIncrement(&historyQueued);
    if (mpmap::Budget(lines, 120)) mpmap::Event("objonce history %s (%08X): the host has done step %d, caught up here once the level settled", names[i], hash,
                                                batchOf[i] + 1);
    return true;
}

// Game thread: the joiner's catch-up, one step per relay tail.
void Tick(DWORD now) {
    if (!mpmap::Arrived()) return;
    Refresh();
    const LONG pending = InterlockedCompareExchange(&catchUpMask, 0, 0);
    if (!pending || !Active()) return;
    const coop_zoneobj::MapPlan* p = coop_zoneobj::PlanFor(planIndex);
    if (!p || !levelSettled || !levelSettledSince || now - levelSettledSince < zoneobj::startDelayMs) return;
    if (catchUpAt && static_cast<LONG>(now - catchUpAt) < 0) return;
    int batch = -1;
    const LONG n = InterlockedCompareExchange(&count, 0, 0);
    for (LONG i = 0; i < n; ++i)
        if ((pending & (1L << i)) && batchOf[i] >= 0 && (batch < 0 || batchOf[i] < batch)) batch = batchOf[i];
    if (batch < 0) { InterlockedExchange(&catchUpMask, 0); return; }
    uint32_t only = 0;
    LONG taken = 0;
    for (LONG i = 0; i < n; ++i)
        if ((pending & (1L << i)) && batchOf[i] == batch) {
            taken |= 1L << i;
            if (!Done(hashes[i])) only |= 1u << msgOf[i];
        }
    InterlockedAnd(&catchUpMask, ~taken);
    const DWORD tail = coop_zoneobj::BatchTailMs(*p, static_cast<unsigned>(batch)) + coop_zoneobj::kAfterTailMs;
    catchUpAt = (now + (tail > zoneobj::stepMs ? tail : zoneobj::stepMs)) | 1;
    if (!only) return;
    InterlockedExchange(&zoneobj::catchUpPlan, planIndex);
    InterlockedIncrement(&catchUpBatches);
    mpmap::Event("objonce catch-up on %s: the host's step %d (%s), kept on this machine", p->map, batch + 1, p->steps[batch].label);
    zoneobj::SendBatch(*p, static_cast<unsigned>(batch), now, InterlockedCompareExchange(&catchUpMask, 0, 0) ? catchUpAt - now : 0, only);
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Mirror", "MpObjectivesOnce", 1, ini.c_str()) != 0;
    std::fprintf(log, "objonce mp_objectives_once=%d (the MP maps' obj_N_new and step messages run once per round on every machine, the first copy wins; "
                      "a late joiner catches up the host's steps)\n", enabled ? 1 : 0);
}
void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG sum = localFirst + swallowed * 3 + soloRepeats * 5 + replayDropped * 7 + replayFirst * 11 + journaled * 13 + historyQueued * 17 +
                     historyAnnouncementsSkipped * 19 + historyDone * 23 + catchUpBatches * 29 + sentAnyway * 31 + resets * 37;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "objonce done=%08lX local_first=%ld swallowed=%ld solo_repeats=%ld sent=%ld partner_first=%ld partner_dropped=%ld journaled=%ld "
                      "history queued=%ld announcements_skipped=%ld already_done=%ld catch_up_steps=%ld pending=%08lX rounds=%ld\n",
                 static_cast<unsigned long>(InterlockedCompareExchange(&doneMask, 0, 0)), localFirst, swallowed, soloRepeats, sentAnyway, replayFirst,
                 replayDropped, journaled, historyQueued, historyAnnouncementsSkipped, historyDone, catchUpBatches,
                 static_cast<unsigned long>(InterlockedCompareExchange(&catchUpMask, 0, 0)), resets);
}
}  // namespace objonce
