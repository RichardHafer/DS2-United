// Dead Space 2 United by Hafer.
// Hold the Zone opens the MP map ([Zone] OpenObjectives in DS2United.ini).
//
// On a Zone visit every machine completes the map's objectives itself, one step
// at a time in the map's own order, by sending the message the map sends when
// the players finish that objective (generated_zone_objectives.h, built by
// tools/build_zone_objectives.py; evidence in
// analysis/HOLD_THE_ZONE_OBJECTIVES_2026-10-12.md). The map's relays then open
// the doors, move the vents and spawn points and announce the next objective.
// The final objective is never started: its start and completion and the
// round's end messages are dropped at the dispatch while the visit runs, and
// the doors its start would open are opened by their own messages.
//
// Each machine runs its own copy of the level logic, so nothing of this is
// shared: while the plan runs, every message it reaches through the relays
// passes the progression gate unheld and the mirror keeps it local. The sends
// go through the full dispatch at 0x00756C60 (bool __cdecl(msg*, int flags),
// the message {hash, in-flight byte, source}, source 0 as the mirror replays),
// so the randomizer's watchers see them. Game thread only.
#include "coop_zoneobj.h"
static_assert(coop_zoneobj::kPlanCount == coop_mpmap::kMapCount, "one Zone plan per map of coop_mpmap::kMapTable");
static_assert(coop_zoneobj::SameText(coop_zoneobj::kPlans[0].map, coop_mpmap::kMapTable[0].name) &&
                  coop_zoneobj::SameText(coop_zoneobj::kPlans[coop_mpmap::kMapCount - 1].map, coop_mpmap::kMapTable[coop_mpmap::kMapCount - 1].name),
              "the Zone plans follow coop_mpmap::kMapTable");
namespace zoneobj {
bool enabled = true, siteOk = false;
DWORD stepMs = 2500, startDelayMs = 1500;
struct Msg { uint32_t hash; uint8_t inFlight, pad[3]; uint32_t source; };
static_assert(sizeof(Msg) == 12, "message size");

// Read from the dispatch hook on any thread.
volatile LONG activePlan = -1;   // plan index while a Zone visit runs here, -1 none
volatile LONG catchUpPlan = -1;  // plan index while a late joiner replays the host's finished steps (objonce), -1 none
volatile LONG localUntil = 0;    // tick until which the plan's messages stay local, 0 none
// For DS2Coop_ZoneObjectives: 0 nothing here, 1 sending, 2 opened, 3 no plan or no dispatch.
enum State : LONG { kNone = 0, kSending = 1, kOpened = 2, kUnavailable = 3 };
volatile LONG state = kNone;
LONG epochState = kNone;   // the current arrival's, game thread

coop_zoneobj::Runner runner;
uint32_t epochSeen = 0xFFFFFFFFu;
int planIndex = -1;
bool settledLogged = false;
volatile LONG sent = 0, unregistered = 0, faults = 0, batchesSent = 0, plansRun = 0, plansSkipped = 0, localPassed = 0, dropLines = 0;
constexpr unsigned kMaxBlock = 32;
volatile LONG drops[kMaxBlock]{};
volatile LONG dropsNested = 0;

const coop_zoneobj::MapPlan* Active() {
    const LONG i = InterlockedCompareExchange(&activePlan, 0, 0);
    return coop_zoneobj::PlanFor(static_cast<int>(i));
}
// The dispatch: a message of the round's end or the final objective, dropped.
bool Drops(uint32_t hash, bool nested) {
    const coop_zoneobj::MapPlan* p = Active();
    if (!p) return false;
    const int i = coop_zoneobj::IndexOf(p->block, p->blockCount, hash);
    if (i < 0) return false;
    const LONG n = i < static_cast<int>(kMaxBlock) ? InterlockedIncrement(&drops[i]) : 0;
    if (nested) InterlockedIncrement(&dropsNested);
    if (n <= 3 && mpmap::Budget(dropLines, 60))
        mpmap::Event("zoneobj DROPPED %s (%08X) on %s%s: the Zone visit's map does not end (%ld time(s))", p->block[i].name, hash, p->map,
                     nested ? ", inside a replayed or mirrored chain" : "", n);
    return true;
}
// The gate and the mirror: a message the plan reaches stays on this machine.
bool Local(uint32_t hash) {
    const LONG until = InterlockedCompareExchange(&localUntil, 0, 0);
    if (!until || static_cast<LONG>(GetTickCount() - static_cast<DWORD>(until)) >= 0) return false;
    const coop_zoneobj::MapPlan* p = Active();
    if (!p) p = coop_zoneobj::PlanFor(static_cast<int>(InterlockedCompareExchange(&catchUpPlan, 0, 0)));
    if (!p || !coop_zoneobj::InSorted(p->local, p->localCount, hash)) return false;
    InterlockedIncrement(&localPassed);
    return true;
}

__declspec(noinline) bool SendRaw(uint32_t hash, bool& fault) {
    Msg m{hash, 0, {0, 0, 0}, 0};
    fault = false;
    __try {
        return reinterpret_cast<bool(__cdecl*)(Msg*, int)>(base + gate::kDispatchRva)(&m, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fault = true;
        return false;
    }
}
// only: bit i sends the batch's message i (a late joiner's catch-up sends what the host did)
void SendBatch(const coop_zoneobj::MapPlan& p, unsigned batch, DWORD now, DWORD nextInMs, uint32_t only = ~0u) {
    unsigned count = 0;
    const char* label = "";
    const coop_zoneobj::Message* list = coop_zoneobj::BatchMessages(p, batch, count, label);
    // the window covers this batch's relays; it is opened before the first send
    const LONG until = static_cast<LONG>((now + coop_zoneobj::LocalHoldMs(p)) | 1);
    const LONG was = InterlockedCompareExchange(&localUntil, 0, 0);
    if (!was || static_cast<LONG>(static_cast<DWORD>(until) - static_cast<DWORD>(was)) > 0) InterlockedExchange(&localUntil, until);
    char next[48] = "the last step";
    if (nextInMs) std::snprintf(next, sizeof(next), "the next step in %lu ms", static_cast<unsigned long>(nextInMs));
    mpmap::Event("zoneobj step %u/%u on %s: %s (%u message(s), relays %u ms); local for %u ms; %s", batch + 1, coop_zoneobj::Batches(p), p.map, label, count,
                 coop_zoneobj::BatchTailMs(p, batch), coop_zoneobj::LocalHoldMs(p), next);
    for (unsigned i = 0; i < count; ++i) {
        if (i < 32 && !(only & (1u << i))) continue;
        uint32_t objects[32]{}, vtables[32]{};
        const unsigned listeners = gate::Listeners(list[i].hash, objects, vtables, 32);
        if (!listeners) {
            InterlockedIncrement(&unregistered);
            mpmap::Event("zoneobj   %s (%08X): NO LISTENER here, not sent (its entity did not spawn or the stream is not loaded)", list[i].name, list[i].hash);
            continue;
        }
        bool fault = false;
        const bool delivered = SendRaw(list[i].hash, fault);
        InterlockedIncrement(fault ? &faults : &sent);
        mpmap::Event("zoneobj   %s (%08X): %s, %u listener(s), vtable RVAs %08X %08X %08X", list[i].name, list[i].hash,
                     fault ? "FAULTED in the dispatch" : delivered ? "sent" : "sent, the dispatch found no registry entry", listeners,
                     vtables[0] ? vtables[0] - static_cast<uint32_t>(base) : 0u, listeners > 1 ? vtables[1] - static_cast<uint32_t>(base) : 0u,
                     listeners > 2 ? vtables[2] - static_cast<uint32_t>(base) : 0u);
    }
    InterlockedIncrement(&batchesSent);
}

// The current level's map in the table, or the one the visit was started for.
int CurrentMapIndex(char* name, size_t size) {
    char map[64] = "", stream[64] = "";
    if (level::CurrentMap(map, sizeof(map), stream, sizeof(stream))) {
        std::snprintf(name, size, "%s", map);
        const int index = coop_mpmap::MapIndex(map);
        if (index >= 0) return index;
    } else {
        std::snprintf(name, size, "%s", mpmap::mapName);
    }
    return coop_mpmap::MapIndex(name);
}

// The clock modes that play the whole map: Hold the Zone and Mercenaries (13.10.).
bool OpensMap(LONG mode) { return mode == gamemode::Zone || mode == gamemode::Mercenaries; }
void Tick(DWORD now) {
    const bool zone = OpensMap(InterlockedCompareExchange(&gamemode::mode, 0, 0));
    const bool onMap = mpmap::Arrived() && mpmap::LmState(nullptr) == coop_mpmap::kRunning;
    if (!onMap) {
        if (InterlockedCompareExchange(&activePlan, 0, 0) >= 0) mpmap::Event("zoneobj dropping ends: the MP map is no longer running");
        InterlockedExchange(&activePlan, -1);
        InterlockedExchange(&localUntil, 0);
        InterlockedExchange(&state, kNone);
        return;
    }
    // a new arrival (a level restart counts as one): the plan starts over
    if (mpmap::visit.epochs != epochSeen) {
        epochSeen = mpmap::visit.epochs;
        runner.Reset();
        settledLogged = false;
        InterlockedExchange(&localUntil, 0);
        for (auto& d : drops) InterlockedExchange(&d, 0);   // counted per plan entry
        char map[64] = "";
        planIndex = CurrentMapIndex(map, sizeof(map));
        const coop_zoneobj::MapPlan* p = coop_zoneobj::PlanFor(planIndex);
        if (!zone) {
            planIndex = -1;
            epochState = kNone;
            mpmap::Event("zoneobj %s (arrival %u): game mode %s, neither Hold the Zone nor Mercenaries; nothing sent, nothing dropped", map, epochSeen,
                         gamemode::Name(InterlockedCompareExchange(&gamemode::mode, 0, 0)));
        } else if (!enabled) {
            planIndex = -1;
            epochState = kNone;
            mpmap::Event("zoneobj %s (arrival %u): Hold the Zone, [Zone] OpenObjectives=0; the objectives stay as the map starts them", map, epochSeen);
        } else if (!p || !siteOk) {
            planIndex = -1;
            epochState = kUnavailable;
            InterlockedIncrement(&plansSkipped);
            mpmap::Event("zoneobj %s (arrival %u): NOT opened: %s", map, epochSeen,
                         !siteOk ? "the dispatch at 0x00756C60 or the gate's hook on it was not ready at start" : "no plan for this map in generated_zone_objectives.h");
        } else {
            unsigned messages = 0;
            for (unsigned i = 0; i < p->stepCount; ++i) messages += p->steps[i].count;
            epochState = kSending;
            mpmap::Event("zoneobj %s (arrival %u): plan armed: %u step(s) with %u message(s), %u door message(s) of the final objective, %u dropped while "
                         "the visit runs, %u kept local; first step %lu ms after the level settles, then every %lu ms or once a step's relays have played",
                         p->map, epochSeen, p->stepCount, messages, p->openCount, p->blockCount, p->localCount, static_cast<unsigned long>(startDelayMs),
                         static_cast<unsigned long>(stepMs));
            if (planIndex != mpmap::mapIndex)
                mpmap::Event("zoneobj note: the level is %s, the visit was started for %s; the level's plan is used", p->map, mpmap::mapName);
        }
    }
    InterlockedExchange(&activePlan, planIndex);
    InterlockedExchange(&state, epochState);
    const coop_zoneobj::MapPlan* p = coop_zoneobj::PlanFor(planIndex);
    if (!p) return;
    const LONG until = InterlockedCompareExchange(&localUntil, 0, 0);
    if (until && static_cast<LONG>(now - static_cast<DWORD>(until)) >= 0) {
        InterlockedExchange(&localUntil, 0);
        mpmap::Event("zoneobj %s: the local window is over (%ld lookups kept a message on this machine); the gate and the mirror work as before", p->map,
                     InterlockedCompareExchange(&localPassed, 0, 0));
    }
    if (!runner.started) {
        // the map's own start (obj_1_new, 1 s after the load) has run once the body stands
        if (!levelSettled || !levelSettledSince) {
            if (!settledLogged) { settledLogged = true; mpmap::Event("zoneobj %s: waiting for the level to settle", p->map); }
            return;
        }
        runner.Start(now, startDelayMs);
        InterlockedIncrement(&plansRun);
        mpmap::Event("zoneobj %s: the level settled; the first step goes in %lu ms", p->map, static_cast<unsigned long>(startDelayMs));
    }
    const int batch = runner.Due(*p, now, stepMs);
    if (batch >= 0) SendBatch(*p, static_cast<unsigned>(batch), now, runner.done ? 0 : runner.nextAt - now);
    if (runner.done && epochState == kSending) {
        epochState = kOpened;
        InterlockedExchange(&state, kOpened);
        mpmap::Event("zoneobj %s: objectives opened (%ld sent, %ld without listener, %ld faulted); the final objective and the round's end stay dropped",
                     p->map, InterlockedCompareExchange(&sent, 0, 0), InterlockedCompareExchange(&unregistered, 0, 0), InterlockedCompareExchange(&faults, 0, 0));
    }
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Zone", "OpenObjectives", 1, ini.c_str()) != 0;
    const int step = static_cast<int>(GetPrivateProfileIntA("Zone", "OpenObjectivesStepMs", 2500, ini.c_str()));
    stepMs = static_cast<DWORD>(step < 1000 ? 1000 : step > 10000 ? 10000 : step);
    unsigned char head[5]{};
    // push ecx / mov edx,[esp+8]: the dispatch's own first bytes, or a hook's jmp in front of them
    const bool bytes = Read(base + gate::kDispatchRva, head, sizeof(head)) && ((head[0] == 0x51 && head[1] == 0x8B && head[2] == 0x54) || head[0] == 0xE9);
    // without the gate's hook the final objective could not be held back: nothing is sent then
    siteOk = bytes && gate::installed;
    std::fprintf(log, "zoneobj open_objectives=%d step_ms=%lu start_delay_ms=%lu dispatch %08X %s, gate hook %s\n", enabled ? 1 : 0,
                 static_cast<unsigned long>(stepMs), static_cast<unsigned long>(startDelayMs), static_cast<unsigned>(base + gate::kDispatchRva),
                 bytes ? "ok" : "MISMATCH", gate::installed ? "in (drops the final objective and the round's end on Zone visits)" : "NOT in: nothing is sent");
    for (unsigned i = 0; i < coop_zoneobj::kPlanCount; ++i) {
        const auto& p = coop_zoneobj::kPlans[i];
        unsigned messages = 0;
        for (unsigned s = 0; s < p.stepCount; ++s) messages += p.steps[s].count;
        std::fprintf(log, "zoneobj plan %-15s %u steps, %u messages, %u door messages, %u dropped, %u local%s, tail %u ms\n", p.map, p.stepCount, messages,
                     p.openCount, p.blockCount, p.localCount, coop_zoneobj::Sorted(p.local, p.localCount) ? "" : " (NOT SORTED: local lookups fail)", p.tailMs);
    }
}
void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    LONG dropSum = 0;
    for (auto& d : drops) dropSum += InterlockedCompareExchange(&d, 0, 0);
    const LONG sum = InterlockedCompareExchange(&sent, 0, 0) * 3 + InterlockedCompareExchange(&unregistered, 0, 0) * 5 + InterlockedCompareExchange(&faults, 0, 0) * 7 +
                     dropSum * 11 + InterlockedCompareExchange(&plansRun, 0, 0) * 13 + InterlockedCompareExchange(&plansSkipped, 0, 0) * 17 +
                     InterlockedCompareExchange(&localPassed, 0, 0) / 50 + InterlockedCompareExchange(&state, 0, 0) * 19;
    if (sum == last) return;
    last = sum;
    const coop_zoneobj::MapPlan* p = Active();
    char perMessage[512] = "";
    size_t used = 0;
    if (p)
        for (unsigned i = 0; i < p->blockCount && i < kMaxBlock && used < sizeof(perMessage) - 48; ++i)
            if (const LONG n = InterlockedCompareExchange(&drops[i], 0, 0))
                used += std::snprintf(perMessage + used, sizeof(perMessage) - used, " %s=%ld", p->block[i].name, n);
    std::fprintf(log, "zoneobj state=%ld plan=%s step=%u/%u runs=%ld skipped=%ld batches=%ld sent=%ld no_listener=%ld faults=%ld local_passed=%ld dropped=%ld "
                      "(nested %ld):%s\n",
                 InterlockedCompareExchange(&state, 0, 0), p ? p->map : "-", runner.next, p ? coop_zoneobj::Batches(*p) : 0u,
                 InterlockedCompareExchange(&plansRun, 0, 0), InterlockedCompareExchange(&plansSkipped, 0, 0), InterlockedCompareExchange(&batchesSent, 0, 0),
                 InterlockedCompareExchange(&sent, 0, 0), InterlockedCompareExchange(&unregistered, 0, 0), InterlockedCompareExchange(&faults, 0, 0),
                 InterlockedCompareExchange(&localPassed, 0, 0), dropSum, InterlockedCompareExchange(&dropsNested, 0, 0), used ? perMessage : " none");
}
}  // namespace zoneobj
