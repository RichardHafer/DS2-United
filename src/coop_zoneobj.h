// Dead Space 2 United by Richard Hafer.
#pragma once
// Hold the Zone opens the MP maps (runtime_coop_zoneobj.inc): pure logic, no
// game memory. The plans come from tools/build_zone_objectives.py
// (generated_zone_objectives.h); analysis/HOLD_THE_ZONE_OBJECTIVES_2026-10-12.md.
#include <cstdint>
#include "generated_zone_objectives.h"

namespace coop_zoneobj {

constexpr bool SameText(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) if (*a != *b) return false;
    return *a == *b;
}
inline const MapPlan* PlanFor(int mapIndex) {
    return mapIndex >= 0 && mapIndex < static_cast<int>(kPlanCount) ? &kPlans[mapIndex] : nullptr;
}
// Index of the hash in the list, -1 when it is not there.
inline int IndexOf(const Message* list, unsigned count, uint32_t hash) {
    for (unsigned i = 0; i < count; ++i) if (list[i].hash == hash) return static_cast<int>(i);
    return -1;
}
inline bool InSorted(const uint32_t* list, unsigned count, uint32_t hash) {
    unsigned lo = 0, hi = count;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2;
        if (list[mid] == hash) return true;
        if (list[mid] < hash) lo = mid + 1; else hi = mid;
    }
    return false;
}
inline bool Sorted(const uint32_t* list, unsigned count) {
    for (unsigned i = 1; i < count; ++i) if (list[i - 1] >= list[i]) return false;
    return true;
}
// The batches in order: the steps, then the final objective's doors as one more.
inline unsigned Batches(const MapPlan& p) { return p.stepCount + (p.openCount ? 1u : 0u); }
inline const Message* BatchMessages(const MapPlan& p, unsigned batch, unsigned& count, const char*& label) {
    if (batch < p.stepCount) { count = p.steps[batch].count; label = p.steps[batch].label; return p.steps[batch].messages; }
    if (batch == p.stepCount && p.openCount) { count = p.openCount; label = "final objective's doors"; return p.open; }
    count = 0;
    label = "";
    return nullptr;
}

// How long a batch's relays take to play out (a health meter shown 5 s after an
// announcement must not come after the next step hid it).
inline uint32_t BatchTailMs(const MapPlan& p, unsigned batch) {
    return batch < p.stepCount ? p.steps[batch].tailMs : batch == p.stepCount ? p.openTailMs : 0u;
}
constexpr uint32_t kAfterTailMs = 500;

// One map's sends: the first batch startDelayMs after Start, every next one
// gapMs after the last, or once the last one's relays have played if that is later.
// Each batch is handed out once.
struct Runner {
    bool started = false, done = false;
    unsigned next = 0;
    uint32_t nextAt = 0;
    void Reset() { *this = Runner{}; }
    void Start(uint32_t now, uint32_t startDelayMs) { started = true; done = false; next = 0; nextAt = now + startDelayMs; }
    // The batch due now, -1 when none is.
    int Due(const MapPlan& p, uint32_t now, uint32_t gapMs) {
        if (!started || done || static_cast<int32_t>(now - nextAt) < 0) return -1;
        const unsigned batch = next++;
        const uint32_t wait = BatchTailMs(p, batch) + kAfterTailMs;
        nextAt = now + (wait > gapMs ? wait : gapMs);
        if (next >= Batches(p)) done = true;
        return batch < Batches(p) ? static_cast<int>(batch) : -1;
    }
};
// How long the plan's messages stay local after a batch went out: the relays'
// latest first arrival plus a margin for the scheduler and a slow frame.
constexpr uint32_t kLocalMarginMs = 5000;
inline uint32_t LocalHoldMs(const MapPlan& p) { return p.tailMs + kLocalMarginMs; }

}  // namespace coop_zoneobj
