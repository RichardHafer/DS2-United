// Dead Space 2 United by Hafer.
// The Objective mode's ears on the MP maps: which objective messages the level
// has sent since it began (generated_objective_table.h). Every map announces its
// five objectives with obj_1_new .. obj_5_new, feeds its round clock through
// TimerSets, and ends the round with a human or a necromorph win message. Fed by
// the dispatch hook in runtime_more_enemies.inc (any thread), read by the
// survival director.
namespace objective_watch {
constexpr unsigned kMax = 64;
uint32_t watched[kMax]{};
unsigned watchedCount = 0;
volatile LONG sent[kMax]{};
volatile LONG notes = 0, resets = 0;

void Watch() {
    if (watchedCount) return;
    auto add = [](uint32_t hash) {
        if (!hash) return;
        for (unsigned i = 0; i < watchedCount; ++i) if (watched[i] == hash) return;
        if (watchedCount < kMax) watched[watchedCount++] = hash;
    };
    for (uint32_t h : ds2::kObjectiveNew) add(h);
    for (uint32_t h : ds2::kObjectiveWin) add(h);
    for (uint32_t h : ds2::kObjectiveLoss) add(h);
    for (const auto& m : ds2::kObjectiveMaps)
        for (unsigned i = 0; i < m.count; ++i) add(m.bonuses[i].hash);
}
void Note(uint32_t hash) {
    for (unsigned i = 0; i < watchedCount; ++i)
        if (watched[i] == hash) {
            InterlockedIncrement(&sent[i]);
            InterlockedIncrement(&notes);
            return;
        }
}
LONG Sent(uint32_t hash) {
    for (unsigned i = 0; i < watchedCount; ++i) if (watched[i] == hash) return InterlockedCompareExchange(&sent[i], 0, 0);
    return 0;
}
// The level is released: its messages go with it.
void Reset() {
    for (unsigned i = 0; i < watchedCount; ++i) InterlockedExchange(&sent[i], 0);
    InterlockedIncrement(&resets);
}
// Only the round's result messages.
void ResetResult() {
    for (uint32_t h : ds2::kObjectiveWin)
        for (unsigned i = 0; i < watchedCount; ++i) if (watched[i] == h) InterlockedExchange(&sent[i], 0);
    for (uint32_t h : ds2::kObjectiveLoss)
        for (unsigned i = 0; i < watchedCount; ++i) if (watched[i] == h) InterlockedExchange(&sent[i], 0);
}
// The highest objective the map has announced (1..5), 0 before the first.
unsigned Current() {
    for (unsigned n = 5; n >= 1; --n)
        if (Sent(ds2::kObjectiveNew[n - 1]) > 0) return n;
    return 0;
}
bool Won() {
    for (uint32_t h : ds2::kObjectiveWin) if (Sent(h) > 0) return true;
    return false;
}
bool Lost() {
    for (uint32_t h : ds2::kObjectiveLoss) if (Sent(h) > 0) return true;
    return false;
}
const ds2::ObjectiveMap* MapFor(const char* map) {
    for (const auto& m : ds2::kObjectiveMaps)
        if (map && _stricmp(m.map, map) == 0) return &m;
    return nullptr;
}
}  // namespace objective_watch
