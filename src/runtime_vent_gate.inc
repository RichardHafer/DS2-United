// Dead Space 2 United by Hafer.
// Which of the MP maps' vents are open. A VentMarker takes part from its
// m_activateSelection message until its m_deactivateSelection (the MP necro
// player's vent choice and the map's own slashers; Marker Lab opens its second
// area's vents with vents1_off when the first doors open). The windows are in
// generated_survival_vents.h; this records which of their messages the level has
// sent since it began, and in which order. Fed by the dispatch hook in
// runtime_more_enemies.inc (any thread), read by the survival director.
namespace survival_rt { void Event(const char* format, ...); }   // runtime_survival.inc, safe from any thread
namespace vent_gate {
constexpr unsigned kMax = 48;
constexpr LONG kNoteLines = 40;   // window messages logged per level, one line each
uint32_t watched[kMax]{};
const char* names[kMax]{};
unsigned watchedCount = 0;
volatile LONG sent[kMax]{};
volatile LONG last[kMax]{};   // sequence number of the latest send this level, 0 none
volatile LONG sequence = 0;
volatile LONG notes = 0, resets = 0, noteLines = 0;
bool KeepOpenedFor(const char* map, int mode) {
    return mode >= 0 && mode <= 3 && coop_mpmap::IsMpMap(map);
}

void Watch() {
    if (watchedCount) return;
    auto add = [](uint32_t hash, const char* name) {
        if (!hash) return;
        for (unsigned i = 0; i < watchedCount; ++i) if (watched[i] == hash) return;
        if (watchedCount < kMax) { names[watchedCount] = name; watched[watchedCount++] = hash; }
    };
    for (const auto& row : ds2::kSurvivalVents) {
        add(row.selectOn, row.onName);
        add(row.selectOff, row.offName);
    }
    for (const auto& marker : ds2::kSurvivalVentMarkers) {
        add(marker.selectOn, marker.onName);
        add(marker.selectOff, marker.offName);
    }
}
// From the dispatch hook: every script message sent.
void Note(uint32_t hash) {
    for (unsigned i = 0; i < watchedCount; ++i)
        if (watched[i] == hash) {
            const LONG count = InterlockedIncrement(&sent[i]);
            const LONG order = InterlockedIncrement(&sequence);
            InterlockedExchange(&last[i], order);
            InterlockedIncrement(&notes);
            const LONG line = InterlockedIncrement(&noteLines);
            if (line <= kNoteLines)
                survival_rt::Event("vent window message %s (%08X) sent: #%ld of this level's window messages, %ld time(s)%s",
                                   names[i] && names[i][0] ? names[i] : "?", hash, order, count,
                                   line == kNoteLines ? "; no more of these lines this level" : "");
            return;
        }
}
LONG Sent(uint32_t hash) {
    for (unsigned i = 0; i < watchedCount; ++i) if (watched[i] == hash) return InterlockedCompareExchange(&sent[i], 0, 0);
    return 0;
}
LONG Last(uint32_t hash) {
    for (unsigned i = 0; i < watchedCount; ++i) if (watched[i] == hash) return InterlockedCompareExchange(&last[i], 0, 0);
    return 0;
}
// The level is released: its messages go with it.
void Reset() {
    for (unsigned i = 0; i < watchedCount; ++i) {
        InterlockedExchange(&sent[i], 0);
        InterlockedExchange(&last[i], 0);
    }
    InterlockedExchange(&sequence, 0);
    InterlockedExchange(&noteLines, 0);
    InterlockedIncrement(&resets);
}
// As VentMarker::HandleEvents (0x013052A0): m_activateSelection clears the vent's
// inactive bit and m_deactivateSelection sets it, so the later of the two decides
// (Concourse closes its upper atrium vents at obj_2_new and opens them again at
// obj_5_new). Before either, the vent has its start state.
template<class Vent>
bool Open(const Vent& vent, bool keepOpened) {
    const bool startsOpen = !vent.selectOn || vent.onAtStart;
    // Keep the activation even if both messages arrived between director ticks.
    if (keepOpened) return startsOpen || Sent(vent.selectOn) != 0;
    const LONG lastOn = Last(vent.selectOn), lastOff = Last(vent.selectOff);
    if (!lastOn && !lastOff) return startsOpen;
    return lastOn > lastOff;
}
}  // namespace vent_gate
