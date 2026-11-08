// Dead Space 2 United by Richard Hafer.
// The MP maps' player spawn points and when each is in use
// (generated_mp_spawn_windows.h): a point is active from its m_activateMessage
// until its m_deactivateMessage; Marker Lab moves its humans to the power area
// at obj_1_complete and on to c_lab at obj_3_complete. This notes which of
// those messages the level has sent, and in which order: a point starts as it is
// authored, then the later of its two messages decides (Academy's start points are
// initially active and also have an activate message). Fed by the gate's dispatch hook (local,
// replayed and mirrored messages alike), read by mpmap::PutOnMap for respawns.
namespace spawnwin {
constexpr unsigned kMax = 32;
uint32_t watched[kMax]{};
unsigned watchedCount = 0;
volatile LONG sent[kMax]{}, lastAt[kMax]{};
volatile LONG notes = 0, resets = 0, order = 0;

void Watch() {
    if (watchedCount) return;
    auto add = [](uint32_t hash) {
        if (!hash) return;
        for (unsigned i = 0; i < watchedCount; ++i) if (watched[i] == hash) return;
        if (watchedCount < kMax) watched[watchedCount++] = hash;
    };
    for (const auto& row : coop_spawnwin::kRows) { add(row.on); add(row.off); }
}
void Note(uint32_t hash) {
    for (unsigned i = 0; i < watchedCount; ++i)
        if (watched[i] == hash) {
            InterlockedIncrement(&sent[i]);
            InterlockedExchange(&lastAt[i], InterlockedIncrement(&order));
            InterlockedIncrement(&notes);
            return;
        }
}
// When the message was last sent, in the order of this level's notes (0: never).
LONG LastAt(uint32_t hash) {
    for (unsigned i = 0; i < watchedCount; ++i) if (watched[i] == hash) return InterlockedCompareExchange(&lastAt[i], 0, 0);
    return 0;
}
// A new visit: the old level's messages go with it.
void Reset() {
    for (unsigned i = 0; i < watchedCount; ++i) { InterlockedExchange(&sent[i], 0); InterlockedExchange(&lastAt[i], 0); }
    InterlockedIncrement(&resets);
}
const coop_spawnwin::Row* RowFor(const uint32_t* guid) {
    for (const auto& row : coop_spawnwin::kRows)
        if (std::memcmp(row.guid, guid, 16) == 0) return &row;
    return nullptr;
}
bool Open(const coop_spawnwin::Row& row) {
    const LONG onAt = row.on ? LastAt(row.on) : 0, offAt = row.off ? LastAt(row.off) : 0;
    if (!onAt && !offAt) return row.initiallyActive;
    return onAt > offAt;
}
}  // namespace spawnwin
