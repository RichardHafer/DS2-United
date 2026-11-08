// Dead Space 2 United by Richard Hafer.
// Campaign arenas ([Arena] Arenas in DS2United.ini): survival in a recorded part
// of the campaign (arenas/*.txt -> tools/build_arenas.py -> generated_arenas.h).
// - Picked in the survival map picker like an MP map. The level loads at the
//   arena's checkpoint through the campaign join recipe
//   (mpmap::StartCampaignFromMenu: new game launch plus checkpoint override);
//   saves and restart points are held for as long as the arena runs.
// - Every zone set switch keeps all arena rooms and none of them loads hidden
//   (streams::pin::arenaMerge). The game's own hides of arena rooms are refused
//   (streams::vis::arenaKeepsVisible), and once a second a loaded arena room
//   that is still hidden is shown.
// - streaming=sets in the arena file (ch03, 55 rooms): only the stream sets around
//   the players are kept, each with its neighbour rooms as the level authored them:
//   a player's own set (the walk sample nearest to him) and every set the walk had
//   within kSetReachM of him, nearest first while the rooms fit the switch's list.
//   A set stays kSetHoldMs after the players left its reach (16.10.: at a border
//   the sets swapped every 2 s and the rooms loaded and unloaded in view). When the
//   wanted sets change, the local player's set is activated again so the switch
//   brings them in; the rest unloads the game's way.
// - The doors out are locked with StandardDoor::SetIsLocked (vtable 0x01B4D5D8
//   slot 130 = 0x00DA1740, byte +0x258; the Xbox lock message calls only this
//   slot), checked again once a second because scripts unlock doors.
// - A held door (its mark, or every unlocked door with doors=open) is opened straight
//   into "opening" and its wish to close is refused in worldcalls::OnDoorTransition.
// - The survival director (DS2RandomizerObserver) asks DS2Coop_Arena* for the
//   arena's name, its area and its spots.
// Bots come along as in the campaign.
// [Arena] Modes: an arena plays the picked mode when it has that mode's points
// (generated_arena_points.h, tools/build_arena_points.py; Mercenaries and Hold the
// Zone, else survival), a death respawns at the arena point nearest to the team
// (runtime_coop_respawn.inc) instead of restarting the level, and co-op joiners are
// launched into the arena with the host (runtime_coop_lobby.inc). Off: survival
// only and the restart at the arena checkpoint, as before.
namespace gamemode { extern volatile LONG mode; }   // runtime_coop_gamemode.inc
namespace arenarec { extern std::string folder; }   // runtime_coop_arenarec.inc
namespace overlay { void Curtain(DWORD ms); void LiftCurtain(); }   // runtime_coop_overlay.inc
namespace arena {
using coop_arena::Arena;
bool enabled = true, setIsLockedOk = false, modesOn = true;
enum class State : LONG { Off = 0, Launching = 1, Running = 2 };
State state = State::Off;
int active = -1;
volatile LONG stateMirror = 0, activeMirror = -1;   // for the exports, any thread
DWORD launchedAt = 0, runningAt = 0, lastPass = 0, offMapSince = 0, settledAt = 0, activatedAt = 0;
unsigned activations = 0;
constexpr DWORD kLaunchTimeoutMs = 180000, kPassMs = 1000, kOffMapMs = 5000, kActivateAfterMs = 8000;
constexpr uint32_t kSetIsLockedSlot = 130 * 4, kSetIsLockedRva = 0x9A1740, kDoorLocked = 0x258, kDoorState = 0x234, kDoorPos = 0xC0;
const unsigned char kSetIsLockedBytes[] = {0x8A, 0x44, 0x24, 0x04, 0x88, 0x81, 0x58, 0x02, 0x00, 0x00, 0xC2, 0x04, 0x00};
constexpr unsigned kMaxDoors = 32, kMaxMembers = 96;

struct DoorRun {
    uint32_t entity = 0;
    unsigned locks = 0, holds = 0, unlocks = 0;
    bool wrongLogged = false, closeLogged = false;
    int rule = -1, tableRule = 0;   // coop_arena::DoorState
    DWORD heldAt = 0;
    uint32_t guid[4]{};   // marked doors only: whose run this is
    unsigned pass = 0;    // the door pass that last met it, 0 free
    uint32_t navSeen = ~0u;   // held doors: state, previous, links cut, obstacle live, as last logged
    unsigned navStale = 0;    // passes in a row open with the graph still blocked
};
DoorRun doorRuns[kMaxDoors]{};
uint32_t memberHandles[kMaxMembers]{};
bool memberRegistered[kMaxMembers]{}, memberLoaded[kMaxMembers]{};
unsigned lastRegistered = ~0u, lastLoaded = ~0u, lastVisible = ~0u;
volatile LONG starts = 0, refused = 0, arrivals = 0, ends = 0, merges = 0, added = 0, unhidden = 0, full = 0;
volatile LONG doorLocks = 0, doorRelocks = 0, doorFaults = 0, doorWrongClass = 0, shows = 0, showLines = 0, insideAsked = 0, insideNo = 0, faults = 0;

const Arena* Active() { return active >= 0 && active < static_cast<int>(coop_arena::kArenaCount) ? &coop_arena::kArenas[active] : nullptr; }
bool RunningNow() { return InterlockedCompareExchange(&stateMirror, 0, 0) == static_cast<LONG>(State::Running); }
// [Arena] BlockHallucinations: the campaign's Nicole and dementia scripts stay off in a match
// (20.10., church crypt: one started mid-match; the ch03 set pieces lock a door while she shows).
bool blockHallucinations = true;
volatile LONG hallucinationsBlocked = 0, hallucinationLines = 0;
bool BlocksHallucination(uint32_t hash) {
    if (!blockHallucinations) return false;
    const char* name = coop_names::Name(hash);
    if (!name || !name[0]) return false;
    char lower[96];
    unsigned i = 0;
    for (; name[i] && i + 1 < sizeof(lower); ++i) lower[i] = static_cast<char>(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    lower[i] = 0;
    if (!std::strstr(lower, "dementia") && !std::strstr(lower, "nicole") && !std::strstr(lower, "hallucin")) return false;
    InterlockedIncrement(&hallucinationsBlocked);
    if (InterlockedIncrement(&hallucinationLines) <= 40) mpmap::Event("arena: campaign hallucination %08X (%s) blocked", hash, name);
    return true;
}

// --- the game modes ([Arena] Modes) ---
volatile LONG respawnPicks = 0, readyArrivals = 0;
const coop_arena::ArenaPoints* PointsOf(LONG index) {
    return index >= 0 && index < static_cast<LONG>(coop_arena::kArenaCount) ? coop_arena::PointsFor(coop_arena::kArenas[index].name) : nullptr;
}
// The mode an arena row plays when `picked` is chosen (picker, launch check). Any thread.
int ModeForName(const char* name, int picked) { return modesOn ? coop_arena::ModeFor(coop_arena::PointsFor(name), picked) : 0; }
// The mode the arena being launched or running plays (DS2Coop_GameMode). Any thread.
int ModeNow(int picked) {
    if (!modesOn || InterlockedCompareExchange(&stateMirror, 0, 0) == static_cast<LONG>(State::Off)) return 0;
    return coop_arena::ModeFor(PointsOf(InterlockedCompareExchange(&activeMirror, 0, 0)), picked);
}
// A death here respawns on an arena point instead of reloading the level.
bool RespawnHere() { return modesOn && RunningNow(); }
// The arena respawn point nearest to `toward` (null: the first). Game thread.
bool SpotLoaded(const Arena& a, unsigned spot);   // below, with the rooms
const float* LastInside();
// The respawn (user, 16.10.): the recorded spot nearest to `toward` (the place of death) that no
// partner stands on and whose rooms are loaded (16.10. night: a random spot in an unloaded
// room dropped the new Isaac through the floor); without `toward` near where he last stood
// in the arena.
uint32_t rngState = 0;
uint32_t Random() {
    if (!rngState) rngState = GetTickCount() ^ (machineIndex * 2654435761u) ^ 0x9E3779B9u;
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}
bool RespawnPoint(const float* toward, float* out) {
    const LONG index = InterlockedCompareExchange(&activeMirror, 0, 0);
    if (!RespawnHere() || index < 0 || index >= static_cast<LONG>(coop_arena::kArenaCount)) return false;
    const Arena& a = coop_arena::kArenas[index];
    if (!a.spotCount) return false;
    float partners[8 * 3]{};
    const int n = PartnerPositions(partners, 8);
    const float* place = toward ? toward : LastInside();
    int best = -1, loadedSpots = 0;
    float bestCost = 1e30f;
    bool bestFree = false;
    for (unsigned s = 0; s < a.spotCount; ++s) {
        if (!SpotLoaded(a, s)) continue;
        ++loadedSpots;
        bool used = false;
        for (int k = 0; k < n && !used; ++k) {
            const float dx = a.spots[s][0] - partners[3 * k], dz = a.spots[s][2] - partners[3 * k + 2];
            used = dx * dx + dz * dz <= 2.25f && std::fabs(a.spots[s][1] - partners[3 * k + 1]) <= 2.0f;
        }
        const float cost = place ? coop_arena::SpotCost(a.spots[s], place) : static_cast<float>(Random() % 1000);
        if (best < 0 || (!used && !bestFree) || (used == !bestFree && cost < bestCost)) { best = static_cast<int>(s); bestCost = cost; bestFree = !used; }
    }
    bool shared = false;
    const bool fallback = best < 0;
    if (fallback) best = place ? coop_arena::FreeSpotNear(a, place, partners, static_cast<unsigned>(n), 1.5f, shared) : static_cast<int>(Random() % a.spotCount);
    if (best < 0) return false;
    std::memcpy(out, a.spots[best], 12);
    InterlockedIncrement(&respawnPicks);
    mpmap::Event("arena respawn: spot %d of %u at (%.1f %.1f %.1f), %s; %d spot(s) with loaded rooms%s%s", best + 1, a.spotCount, out[0], out[1], out[2],
                 toward ? "nearest to the place given" : place ? "nearest to where he last stood in the arena" : "random", loadedSpots,
                 fallback ? " (NONE: the nearest of all)" : "", !fallback && !bestFree ? ", every loaded spot taken: shared" : "");
    return true;
}
// A place worth steering a respawn toward, or a body that did not fall out: in the arena's rooms.
bool Usable(const float* point) {
    const Arena* a = Active();
    coop_arena::Reach reach;
    reach.horizontal = 8.0f;
    reach.vertical = 4.0f;
    reach.margin = 3.0f;
    return a && point && coop_arena::Inside(*a, point, reach);
}
// For the survival director: the arena's zone (0), capsule (1) or respawn (2) points as
// x, y, z, radius quadruples (radius 0: the mode's default). Any thread.
int Points(int kind, float* out, int max) {
    if (InterlockedCompareExchange(&stateMirror, 0, 0) == static_cast<LONG>(State::Off)) return 0;
    const coop_arena::ArenaPoints* p = PointsOf(InterlockedCompareExchange(&activeMirror, 0, 0));
    unsigned n = 0;
    const ds2arena::Point* list = p ? coop_arena::PointList(*p, kind, n) : nullptr;
    for (int i = 0; list && out && i < static_cast<int>(n) && i < max; ++i) {
        std::memcpy(out + 4 * i, list[i].pos, 12);
        out[4 * i + 3] = list[i].radius;
    }
    return list ? static_cast<int>(n) : 0;
}
void SetState(State s) {
    state = s;
    InterlockedExchange(&stateMirror, static_cast<LONG>(s));
}

// --- streaming=sets ---
bool selected[kMaxMembers]{};
volatile LONG wantedMask = -1;   // the sets kept (bit = set index), any thread
uint32_t activatedMask = 0;
unsigned maskTries = 0;
volatile LONG maskChanges = 0, setActivations = 0;
constexpr float kSetReachM = 12.0f;
constexpr DWORD kSetHoldMs = 10000;
constexpr unsigned kSetRoomLimit = 40;   // of streams::pin::kMaxDesired 46: room for the switch's own list
DWORD setNearAt[32]{};
int forcedSet = -1;      // a start spot's set: loaded before the Isaac goes there
DWORD forcedUntil = 0;
// The director's living enemies and their spawners (DS2Survival_KeepPoints): their rooms stay too.
using KeepPointsFn = int(__cdecl*)(float*, int);
KeepPointsFn keepPointsFn = nullptr;
bool keepLooked = false;
volatile LONG keepPointsUsed = 0;
constexpr int kKeepMost = 64;
int KeepPoints(float* out, int max) {
    if (!keepLooked) {
        keepLooked = true;
        if (const HMODULE module = ds2coop::PartnerModule("DS2RandomizerObserver.asi"))
            keepPointsFn = reinterpret_cast<KeepPointsFn>(GetProcAddress(module, "DS2Survival_KeepPoints"));
        mpmap::Event("arena: the director's keep points %s", keepPointsFn ? "found (enemy and spawner rooms stay loaded)" : "MISSING (randomizer half too old)");
    }
    return keepPointsFn ? keepPointsFn(out, max) : 0;
}
// The sets around the local Isaac and the partners; the start set before anybody is there.
uint32_t WantedSets(const Arena& a, int& localSet) {
    localSet = -1;
    if (!a.streaming) return 0xFFFFFFFFu;
    const DWORD now = GetTickCount();
    float cost[32], best[32];
    for (float& c : best) c = 1e30f;
    float points[(9 + kKeepMost) * 3]{};
    int count = 0;
    const uint32_t me = LocalSlotZeroEntity();
    if (me && Read(me + mpmap::kPlayerPos, points, 12) && std::isfinite(points[0]) && std::isfinite(points[1]) && std::isfinite(points[2])) {
        localSet = coop_arena::SetForPoint(a, points);
        count = 1;
    }
    count += PartnerPositions(points + 3 * count, 8);
    const int people = count;
    const int kept = KeepPoints(points + 3 * count, kKeepMost);
    if (kept > 0) { count += kept; InterlockedExchange(&keepPointsUsed, kept); }
    for (int i = 0; i < count && i < 9; ++i) {
        coop_arena::SetCosts(a, points + 3 * i, cost, 32);
        for (unsigned s = 0; s < 32; ++s) if (cost[s] < best[s]) best[s] = cost[s];
    }
    const float reach2 = kSetReachM * kSetReachM;
    uint32_t held = 0;
    for (unsigned s = 0; s < a.setCount && s < 32; ++s) {
        if (best[s] <= reach2) setNearAt[s] = now ? now : 1;
        if (setNearAt[s] && now - setNearAt[s] < kSetHoldMs) held |= 1u << s;
    }
    int first = localSet;
    if (first < 0) {
        for (int i = 1; i < people && first < 0; ++i) first = coop_arena::SetForPoint(a, points + 3 * i);
        if (first < 0 && a.walkCount && a.walkSet[0] < a.setCount) first = a.walkSet[0];
    }
    const int forced = forcedSet >= 0 && static_cast<LONG>(forcedUntil - now) > 0 ? forcedSet : -1;
    return coop_arena::PickSets(a, best, first, reach2, held, kSetRoomLimit, forced);
}
// Game thread: the wanted sets now, and the members they hold.
uint32_t Refresh(const Arena& a, int& localSet) {
    const uint32_t mask = WantedSets(a, localSet);
    coop_arena::SelectMembers(a, mask, selected, kMaxMembers);
    InterlockedExchange(&wantedMask, static_cast<LONG>(mask));
    return mask;
}

// --- hooks in coop_streams and mpmap ---
unsigned Merge(uint32_t (*list)[4], unsigned char* hidden, unsigned& count, unsigned max, uint32_t caller) {
    const Arena* a = Active();
    if (!a || state == State::Off) return 0;
    int localSet = -1;
    if (a->streaming) Refresh(*a, localSet);
    const coop_arena::Merge m = coop_arena::MergeMembers(*a, list, hidden, count, max, a->streaming ? selected : nullptr);
    InterlockedIncrement(&merges);
    InterlockedExchangeAdd(&added, static_cast<LONG>(m.added));
    InterlockedExchangeAdd(&unhidden, static_cast<LONG>(m.unhidden));
    InterlockedExchangeAdd(&full, static_cast<LONG>(m.full));
    if (InterlockedCompareExchange(&merges, 0, 0) <= 40)
        mpmap::Event("arena %s: zone set switch (caller %08X%s), %u room(s) added, %u unhidden, %u without room, %u desired", a->name, caller,
                     caller == base + streams::pin::kFromActivateSetRva ? ", ActivateSet" : "", m.added, m.unhidden, m.full, count);
    return m.added + m.unhidden;
}
// The story logic inside the arena: listed kind 1 is not created, and the spawn
// filter refuses everything listed (kind 2 keeps its spawn record).
volatile LONG notCreated = 0, filterRefused = 0, blockLines = 0;
bool Blocks(const uint32_t* guid, bool atCreator) {
    const Arena* a = Active();
    if (!a || state == State::Off) return false;
    const char* cls = "?";
    const int kind = coop_arena::BlockKind(*a, guid, &cls);
    if (!kind || (atCreator && kind != 1)) return false;
    InterlockedIncrement(atCreator ? &notCreated : &filterRefused);
    if (InterlockedIncrement(&blockLines) <= 120)
        mpmap::Event("arena %s: %s %s %08X-%08X-%08X-%08X", a->name, atCreator ? "not created:" : "spawn refused:", cls, guid[0], guid[1], guid[2], guid[3]);
    return true;
}
bool KeepsVisible(uint32_t handle) {
    if (!handle || state != State::Running) return false;
    const Arena* a = Active();
    for (unsigned i = 0; a && i < a->memberCount && i < kMaxMembers; ++i)
        if (memberHandles[i] == handle) return !a->streaming || selected[i];
    return false;
}

// --- doors ---
// Each door follows the door table (boundary locked, inner left alone) unless it was
// marked in the game (the recorder's DoorKey): inner is never locked, locked always,
// held is kept open. Marks made this session count first, then the generated ones.
constexpr unsigned kMaxLiveMarks = 32, kMaxMarkedDoors = 32;
struct LiveMark { uint32_t guid[4]; int state; };
LiveMark liveMarks[kMaxLiveMarks]{};
unsigned liveMarkCount = 0;
DoorRun markRuns[kMaxMarkedDoors]{};   // marked doors that are not in the door table
volatile LONG doorUnlocks = 0, doorHolds = 0, liveMarksSet = 0, closesRefused = 0, insideBehindDoor = 0;

int MarkOf(const uint32_t* guid) {
    for (unsigned i = 0; i < liveMarkCount; ++i)
        if (coop_arena::SameGuid(liveMarks[i].guid, guid)) return liveMarks[i].state;
    const Arena* a = Active();
    return a ? coop_arena::DoorMarkFor(coop_arena::PointsFor(a->name), guid) : coop_arena::kDoorAuto;
}
// The recorder's door key while the arena runs. Game thread.
void SetLiveMark(const uint32_t* guid, int mark) {
    InterlockedIncrement(&liveMarksSet);
    for (unsigned i = 0; i < liveMarkCount; ++i)
        if (coop_arena::SameGuid(liveMarks[i].guid, guid)) { liveMarks[i].state = mark; return; }
    if (liveMarkCount >= kMaxLiveMarks) return;
    std::memcpy(liveMarks[liveMarkCount].guid, guid, 16);
    liveMarks[liveMarkCount++].state = mark;
}
// Marks made in an earlier run of this arena (the door key writes them to
// plugins\DS2Arenas\doors\<arena>.txt): they count again without a rebuild.
volatile LONG savedMarksLoaded = 0;
void LoadSavedMarks(const Arena& a) {
    liveMarkCount = 0;
    if (arenarec::folder.empty()) return;
    const std::string path = arenarec::folder + "\\doors\\" + a.name + ".txt";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) {
        mpmap::Event("arena %s: no saved door marks (%s)", a.name, path.c_str());
        return;
    }
    std::string text;
    char chunk[4096];
    size_t got = 0;
    while (text.size() < (1u << 20) && (got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, got);
    std::fclose(f);
    coop_arena::SavedMark marks[kMaxLiveMarks]{};
    const unsigned n = coop_arena::ParseSavedMarks(text.c_str(), marks, kMaxLiveMarks);
    for (unsigned i = 0; i < n; ++i) {
        std::memcpy(liveMarks[i].guid, marks[i].guid, 16);
        liveMarks[i].state = marks[i].state;
    }
    liveMarkCount = n;
    InterlockedExchangeAdd(&savedMarksLoaded, static_cast<LONG>(n));
    mpmap::Event("arena %s: %u saved door mark(s) from %s", a.name, n, path.c_str());
    for (unsigned i = 0; i < n; ++i)
        mpmap::Event("arena %s: saved mark %08X-%08X-%08X-%08X %s", a.name, marks[i].guid[0], marks[i].guid[1], marks[i].guid[2], marks[i].guid[3],
                     coop_arena::DoorStateName(marks[i].state));
}
int RuleFor(const Arena& a, bool tableLock, const uint32_t* guid) { return coop_arena::DoorRule(a, MarkOf(guid), tableLock); }
// worldcalls::OnDoorTransition, game thread: a held door's wish to close (6), or to snap
// shut (0) while it opens or stands open, is refused while the arena runs.
bool RefusesClose(uint32_t door, uint32_t wanted) {
    if (!door || !RunningNow()) return false;
    DoorRun* hit = nullptr;
    for (DoorRun& r : doorRuns) if (r.entity == door && r.rule == coop_arena::kDoorHeld) { hit = &r; break; }
    for (DoorRun& r : markRuns) if (!hit && r.entity == door && r.rule == coop_arena::kDoorHeld) { hit = &r; break; }
    if (!hit) return false;
    uint32_t current = 0;
    Read(door + kDoorState, &current, 4);
    if (wanted == worldcalls::kDoorClosed && current != worldcalls::kDoorOpening && current != worldcalls::kDoorOpen) return false;
    InterlockedIncrement(&closesRefused);
    if (!hit->closeLogged) {
        hit->closeLogged = true;
        mpmap::Event("arena door %08X held open: its wish to %s refused (state %u); asked again every tick", door, wanted == worldcalls::kDoorClosed ? "shut" : "close", current);
    }
    return true;
}

__declspec(noinline) bool CallSetIsLocked(uint32_t fn, uint32_t door, bool lock) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, bool)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(door)), lock);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&doorFaults);
        return false;
    }
}
// StandardDoor::SendDoorMsg (0x00DADEF0, thiscall(type), ret 4) sends the door's message of a
// type, [[door+0x0C]+0x17C+type*12]: 7 open start front, 8 back. The door tick sends the open
// start only when the door opens after loading (0x00DD264C); a held door opened from closed
// skipped it, and with it the scripts it starts (ch03: lt_crypt_on lights the lift room, whose
// own triggers switch it dark when it loads). So both go out at the opening and again 2 s after
// arena rooms loaded, for the held doors within kResendReachM.
constexpr uint32_t kSendDoorMsgRva = 0x9ADEF0, kProbeHash = 0xDFFB2607;   // lt_crypt_on, for the log
const unsigned char kSendDoorMsgBytes[] = {0x83, 0xEC, 0x0C, 0x8B, 0x44, 0x24, 0x10, 0x8B, 0x49, 0x0C, 0x8D, 0x04, 0x40, 0x8B, 0x94, 0x81, 0x7C, 0x01, 0x00, 0x00};
constexpr float kResendReachM = 40.0f;
bool sendDoorMsgOk = false;
DWORD resendAt = 0;
volatile LONG doorMessages = 0, doorMessageLines = 0, doorResends = 0;
__declspec(noinline) bool CallSendDoorMsg(uint32_t door, int type) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, int)>(base + kSendDoorMsgRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(door)), type);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&doorFaults);
        return false;
    }
}
void SendOpenMessages(uint32_t door, const uint32_t* guid, const char* why) {
    if (!sendDoorMsgOk || !door) return;
    uint32_t attrs = 0, hashes[2]{};
    bool sent[2]{};
    if (Read(door + 0x0C, &attrs, 4) && attrs) {
        Read(attrs + 0x17C + 7 * 12, &hashes[0], 4);
        Read(attrs + 0x17C + 8 * 12, &hashes[1], 4);
    }
    for (int k = 0; k < 2; ++k)
        if (hashes[k]) {
            sent[k] = CallSendDoorMsg(door, 7 + k);
            if (sent[k]) InterlockedIncrement(&doorMessages);
        }
    if (InterlockedIncrement(&doorMessageLines) > 40) return;
    uint32_t objects[16], vtables[16];
    const unsigned front = hashes[0] ? gate::Listeners(hashes[0], objects, vtables, 16) : 0;
    const unsigned back = hashes[1] ? gate::Listeners(hashes[1], objects, vtables, 16) : 0;
    mpmap::Event("arena door %08X-%08X open-start messages (%s): front %08X to %u listener(s) %s, back %08X to %u %s", guid[0], guid[1], why, hashes[0], front,
                 !hashes[0] ? "none" : sent[0] ? "sent" : "FAULTED", hashes[1], back, !hashes[1] ? "none" : sent[1] ? "sent" : "FAULTED");
}

// Door::TransitionToState(4) past our own hook (no partner wait, no mirror): every machine
// opens its own copy. Its native way (TryToActivateDoor) would also switch to the door's
// open set, and with streaming=sets the rooms come from the players' sets instead.
__declspec(noinline) bool CallOpen(uint32_t door) {
    __try {
        worldcalls::ClearLockHologram(door);
        void* self = reinterpret_cast<void*>(static_cast<uintptr_t>(door));
        if (worldcalls::doorHook) worldcalls::doorHook.thiscall<void>(self, worldcalls::kDoorOpening);
        else reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + worldcalls::kDoorTransitionRva)(self, worldcalls::kDoorOpening);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&doorFaults);
        return false;
    }
}
// A held door and the nav graph. Its own open entry (4 -> 5) reconnects its links and calls
// UnblockGraph (verified 20.10. in dumps 25016 and 13176 and live in ch03 and ch10); this only
// catches a held door that stands at 5/5 for two passes with BlockGraph's NavPower obstacle still
// live or its links still cut. analysis/DOOR_NAV_BLOCK_2026-10-20.md, [Arena] UnblockHeldDoors.
bool unblockHeldDoors = true, unblockOk = false, operateOk = false;
constexpr uint32_t kUnblockGraphRva = 0x9B7E30, kOperateGraphRva = 0x9C5950, kNavObstacleIndexRva = 0x1CB79EA;
constexpr uint32_t kDoorPrevious = 0x238, kDoorFlags = 0x240, kFlagLinksCut = 0x200, kObstacleHandle = 0x24;
constexpr uint32_t kEntityComponentTypes = 0x44, kEntityComponents = 0x48;
const unsigned char kUnblockGraphBytes[] = {0x85, 0xC9, 0x74, 0x18, 0x0F, 0xB6, 0x05, 0xEA, 0x79, 0x0B, 0x02, 0x50, 0xE8, 0x3F, 0x72, 0xA1};
const unsigned char kOperateGraphBytes[] = {0x83, 0xEC, 0x18, 0x53, 0x55, 0x8B, 0x2D, 0x08, 0x20, 0x02, 0x02, 0x56, 0x8B, 0xF1, 0x8B, 0x4E};
volatile LONG doorNavPasses = 0, doorNavStale = 0, doorNavFixes = 0, doorNavFixFailed = 0, doorNavNoComponent = 0, doorNavLines = 0;

uint32_t NavObstacleOf(uint32_t door) {
    unsigned char index = 0, slot = 0;
    uint32_t types = 0, components = 0, comp = 0;
    if (!Read(base + kNavObstacleIndexRva, &index, 1) || !index || !Read(door + kEntityComponentTypes, &types, 4) || !types ||
        !Read(door + kEntityComponents, &components, 4) || !components || !Read(types + index + 0xC, &slot, 1) || !Read(components + slot * 4u, &comp, 4))
        return 0;
    return comp;
}
struct DoorNav { uint32_t state = 0, previous = 0, comp = 0, handle = 0; bool cut = false, live = false; };
DoorNav ReadDoorNav(uint32_t door) {
    DoorNav n;
    uint32_t flags = 0, link = 0;
    Read(door + kDoorState, &n.state, 4);
    Read(door + kDoorPrevious, &n.previous, 4);
    Read(door + kDoorFlags, &flags, 4);
    n.cut = (flags & kFlagLinksCut) != 0;
    n.comp = NavObstacleOf(door);
    n.live = n.comp && Read(n.comp + kObstacleHandle, &n.handle, 4) && n.handle && Read(n.handle, &link, 4) && link;
    return n;
}
void CheckDoorNav(uint32_t door, const uint32_t* guid, DoorRun& r) {
    InterlockedIncrement(&doorNavPasses);
    const DoorNav n = ReadDoorNav(door);
    if (!n.comp) InterlockedIncrement(&doorNavNoComponent);
    const uint32_t seen = n.state | n.previous << 8 | (n.cut ? 1u << 16 : 0u) | (n.live ? 1u << 17 : 0u) | (n.comp ? 1u << 18 : 0u);
    if (seen != r.navSeen) {
        r.navSeen = seen;
        if (InterlockedIncrement(&doorNavLines) <= 60)
            mpmap::Event("arena door %08X-%08X nav: object %08X state %u/%u, links %s, obstacle %s (component %08X handle %08X)", guid[0], guid[1], door, n.state, n.previous,
                         n.cut ? "CUT" : "connected", !n.comp ? "none (no NavObstacleComponent)" : n.live ? "LIVE" : "gone", n.comp, n.handle);
    }
    if (n.state != worldcalls::kDoorOpen || n.previous != worldcalls::kDoorOpen || (!n.live && !n.cut)) { r.navStale = 0; return; }
    InterlockedIncrement(&doorNavStale);
    if (++r.navStale < 2 || !unblockHeldDoors) return;
    r.navStale = 0;
    bool callFaulted = false;
    __try {
        if (n.cut && operateOk) reinterpret_cast<void(__thiscall*)(void*, bool)>(base + kOperateGraphRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(door)), false);
        if (n.live && unblockOk) reinterpret_cast<void(__thiscall*)(void*)>(base + kUnblockGraphRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(door)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        callFaulted = true;
    }
    const DoorNav after = ReadDoorNav(door);
    const bool fixed = !callFaulted && !after.cut && !after.live;
    InterlockedIncrement(fixed ? &doorNavFixes : &doorNavFixFailed);
    if (InterlockedIncrement(&doorNavLines) <= 60)
        mpmap::Event("arena door %08X-%08X held open but still blocked for the AI (links %s, obstacle %s): %s%s -> links %s, obstacle %s%s", guid[0], guid[1],
                     n.cut ? "cut" : "connected", n.live ? "live" : "gone", n.cut ? (operateOk ? "OperateOnGraphConnections(false) " : "links left, signature MISMATCH ") : "",
                     n.live ? (unblockOk ? "UnblockGraph" : "obstacle left, signature MISMATCH") : "", after.cut ? "CUT" : "connected", after.live ? "LIVE" : "gone",
                     callFaulted ? " (FAULTED)" : "");
}

// One door by its rule (coop_arena::DoorState).
void ApplyDoor(const uint32_t* guid, int rule, const char* why, DoorRun& r, unsigned& found, unsigned& locked, DWORD now) {
    const uint32_t door = mirror::ObjectByGuid(guid);
    if (!door) {
        if (r.entity) mpmap::Event("arena door %08X-%08X: gone from the world (its stream unloaded)", guid[0], guid[1]);
        r.entity = 0;
        return;
    }
    ++found;
    uint32_t vtable = 0, fn = 0;
    Read(door, &vtable, 4);
    if (vtable) Read(vtable + kSetIsLockedSlot, &fn, 4);
    if (!setIsLockedOk || fn != base + kSetIsLockedRva) {
        if (!r.wrongLogged) {
            r.wrongLogged = true;
            InterlockedIncrement(&doorWrongClass);
            mpmap::Event("arena door %08X-%08X: object %08X vtable %08X slot 130 %08X is not StandardDoor::SetIsLocked; left alone", guid[0], guid[1], door, vtable, fn);
        }
        return;
    }
    unsigned char isLocked = 0;
    uint32_t doorState = 0;
    Read(door + kDoorLocked, &isLocked, 1);
    Read(door + kDoorState, &doorState, 4);
    if (door != r.entity || rule != r.rule) {
        float pos[3]{};
        Read(door + kDoorPos, pos, sizeof(pos));
        mpmap::Event("arena door %08X-%08X (%s, %s): object %08X at (%.1f %.1f %.1f), state %u, locked %u", guid[0], guid[1], coop_arena::DoorStateName(rule), why,
                     door, pos[0], pos[1], pos[2], doorState, isLocked);
        r.entity = door;
        r.rule = rule;
    }
    if (rule == coop_arena::kDoorLocked) {
        if (isLocked) { ++locked; return; }
        if (!CallSetIsLocked(fn, door, true)) {
            mpmap::Event("arena door %08X-%08X: SetIsLocked FAULTED", guid[0], guid[1]);
            return;
        }
        Read(door + kDoorLocked, &isLocked, 1);
        if (!isLocked) { mpmap::Event("arena door %08X-%08X: SetIsLocked(true) called, the flag is still 0", guid[0], guid[1]); return; }
        ++locked;
        if (r.locks++ == 0) {
            InterlockedIncrement(&doorLocks);
            mpmap::Event("arena door %08X-%08X: locked (state %u)", guid[0], guid[1], doorState);
        } else {
            InterlockedIncrement(&doorRelocks);
            if (r.locks <= 10) mpmap::Event("arena door %08X-%08X: unlocked again by the game, locked again (#%u)", guid[0], guid[1], r.locks);
        }
        return;
    }
    // inner and held: never locked; the door table's inner doors are left as the game has them
    if (isLocked && rule != r.tableRule) {
        const bool called = CallSetIsLocked(fn, door, false);
        Read(door + kDoorLocked, &isLocked, 1);
        if (called && !isLocked) InterlockedIncrement(&doorUnlocks);
        if (r.unlocks++ < 10) mpmap::Event("arena door %08X-%08X: unlocked for its mark %s: %s", guid[0], guid[1], coop_arena::DoorStateName(rule),
                                           !called ? "FAULTED" : isLocked ? "the flag is still 1" : "ok");
        return;
    }
    // held: opened from closed (0) or waiting (3); closing is refused in RefusesClose
    if (rule == coop_arena::kDoorHeld && !isLocked && (doorState == worldcalls::kDoorClosed || doorState == 3) && now - r.heldAt >= 1000) {
        r.heldAt = now;
        const bool called = CallOpen(door);
        uint32_t after = 0;
        Read(door + kDoorState, &after, 4);
        InterlockedIncrement(&doorHolds);
        if (r.holds++ < 10) mpmap::Event("arena door %08X-%08X held open: opened from state %u, now %u%s", guid[0], guid[1], doorState, after,
                                         !called ? " (FAULTED)" : after == doorState ? " (UNCHANGED)" : "");
        if (called && after != doorState) SendOpenMessages(door, guid, "opened");
    }
    if (rule == coop_arena::kDoorHeld) CheckDoorNav(door, guid, r);
}
// A marked door's run by its GUID, so a door marked back to auto frees its own run
// (and is no longer held) and the others keep theirs.
DoorRun* MarkRun(const uint32_t* guid, unsigned pass) {
    DoorRun* spare = nullptr;
    for (DoorRun& r : markRuns) {
        if (r.pass && coop_arena::SameGuid(r.guid, guid)) { r.pass = pass; return &r; }
        if (!r.pass && !spare) spare = &r;
    }
    if (!spare) return nullptr;
    *spare = DoorRun{};
    std::memcpy(spare->guid, guid, 16);
    spare->pass = pass;
    return spare;
}
bool InTable(const Arena& a, const uint32_t* guid) {
    for (unsigned i = 0; i < a.doorCount; ++i)
        if (coop_arena::SameGuid(a.doors[i].guid, guid)) return true;
    return false;
}
// Rooms loaded: the open-start messages of the held doors near the local Isaac again.
void ResendHeld(const Arena& a, DWORD now) {
    resendAt = 0;
    float me[3]{};
    const uint32_t local = LocalSlotZeroEntity();
    if (!local || !Read(local + mpmap::kPlayerPos, me, sizeof(me))) return;
    unsigned sent = 0, distant = 0;
    auto consider = [&](DoorRun& r, const uint32_t* guid) {
        if (!r.entity || r.rule != coop_arena::kDoorHeld) return;
        float pos[3]{};
        uint32_t doorState = 0;
        if (!Read(r.entity + kDoorPos, pos, sizeof(pos)) || !Read(r.entity + kDoorState, &doorState, 4)) return;
        if (doorState != worldcalls::kDoorOpening && doorState != worldcalls::kDoorOpen) return;
        const float dx = pos[0] - me[0], dy = pos[1] - me[1], dz = pos[2] - me[2];
        if (dx * dx + dy * dy + dz * dz > kResendReachM * kResendReachM) { ++distant; return; }
        SendOpenMessages(r.entity, guid, "rooms loaded");
        ++sent;
    };
    for (unsigned i = 0; i < a.doorCount && i < kMaxDoors; ++i) consider(doorRuns[i], a.doors[i].guid);
    for (DoorRun& r : markRuns) if (r.pass) consider(r, r.guid);
    InterlockedIncrement(&doorResends);
    uint32_t objects[16], vtables[16];
    if (InterlockedCompareExchange(&doorResends, 0, 0) <= 30)
        mpmap::Event("arena %s: rooms loaded, open-start messages again for %u held door(s) (%u farther than %.0f m) %lu ms after arrival; lt_crypt_on listeners %u",
                     a.name, sent, distant, kResendReachM, static_cast<unsigned long>(now - runningAt), gate::Listeners(kProbeHash, objects, vtables, 16));
}
void DoorPass(const Arena& a, unsigned& found, unsigned& locked, DWORD now) {
    for (unsigned i = 0; i < a.doorCount && i < kMaxDoors; ++i) {
        const ds2arena::Door& d = a.doors[i];
        doorRuns[i].tableRule = d.lock ? coop_arena::kDoorLocked : coop_arena::kDoorInner;
        ApplyDoor(d.guid, RuleFor(a, d.lock, d.guid), d.why, doorRuns[i], found, locked, now);
    }
    // marked doors the door table does not have
    static unsigned pass = 0;
    if (++pass == 0) pass = 1;
    const coop_arena::ArenaPoints* p = coop_arena::PointsFor(a.name);
    for (unsigned k = 0; p && k < p->doorMarkCount; ++k) {
        const uint32_t* guid = p->doorMarks[k].guid;
        if (InTable(a, guid)) continue;
        const int mark = MarkOf(guid);
        if (mark == coop_arena::kDoorAuto) continue;
        DoorRun* r = MarkRun(guid, pass);
        if (!r) break;
        r->tableRule = coop_arena::kDoorAuto;
        ApplyDoor(guid, coop_arena::DoorRule(a, mark, false), "marked in the game", *r, found, locked, now);
    }
    for (unsigned k = 0; k < liveMarkCount; ++k) {
        const uint32_t* guid = liveMarks[k].guid;
        if (InTable(a, guid) || coop_arena::DoorMarkFor(p, guid) != coop_arena::kDoorAuto || liveMarks[k].state == coop_arena::kDoorAuto) continue;
        DoorRun* r = MarkRun(guid, pass);
        if (!r) break;
        r->tableRule = coop_arena::kDoorAuto;
        ApplyDoor(guid, coop_arena::DoorRule(a, liveMarks[k].state, false), "marked just now", *r, found, locked, now);
    }
    for (DoorRun& r : markRuns) if (r.pass && r.pass != pass) r = DoorRun{};
}

// --- necro-only blockers ---
// 21.10., arena_ch10: a ScriptableProp (5ACB6EF0-E1930623) stood in a held door with a
// Havok filter that blocks NPCs but not Isaac, bots or corpses; live necros ran on the
// spot there. EA's filter info: top bits 01 = DetailedFilterInfo at info << 2 (type +0,
// flags +4); two objects collide when each one's type meets the other's flags. Isaac is
// type 0x10000, a live necro 0x20000. A body that blocks necros only, near a door the
// arena keeps usable, loses the necro bit ([Arena] OpenNpcClips). Havok keeps overlaps it
// already has, so a necro standing in the blocker at that moment stays blocked.
bool openNpcClips = true;
constexpr uint32_t kFilterTagMask = 0xC0000000, kFilterDetail = 0x40000000, kTypePlayer = 0x10000, kTypeNpc = 0x20000;
constexpr uint32_t kFixedIsland = 0x20, kBodyFilterInfo = 0x2C, kBodyPos = 0x120, kEntityGuid = 0x24;
// [Arena] NpcClipReachM: 2.5 m left a blocker 3.3 m from a held door of the marker floor (02.11.):
// the door object sits at the frame's middle, the blocker at its side.
float clipReachM = 7.0f;
constexpr DWORD kClipPassMs = 3000;
constexpr unsigned kMaxClipBodies = 8192, kMaxClipsSeen = 32;
struct ClipBody { uint32_t body, info; };
ClipBody clipBodies[kMaxClipBodies];
uint32_t clipsSeen[kMaxClipsSeen]{};   // NPC-only bodies already logged this match
unsigned clipsSeenCount = 0;
DWORD clipPassAt = 0;
volatile LONG clipPasses = 0, clipWalked = 0, clipNpcOnly = 0, clipsOpened = 0, clipsReopened = 0, clipsShared = 0, clipsAway = 0, clipFaults = 0, clipLines = 0;

bool ClipSeen(uint32_t body) {
    for (unsigned i = 0; i < clipsSeenCount; ++i) if (clipsSeen[i] == body) return true;
    if (clipsSeenCount < kMaxClipsSeen) clipsSeen[clipsSeenCount++] = body;
    return false;
}
void ClipPass(const Arena& a, DWORD now) {
    if (!openNpcClips || (clipPassAt && now - clipPassAt < kClipPassMs)) return;
    clipPassAt = now;
    struct OpenDoor { float pos[3]; const uint32_t* guid; };
    OpenDoor open[kMaxDoors + kMaxMarkedDoors];
    unsigned openCount = 0;
    auto add = [&](const DoorRun& r, const uint32_t* guid) {
        if (!r.entity || r.rule < 0 || r.rule == coop_arena::kDoorLocked || openCount >= std::size(open)) return;
        if (Read(r.entity + kDoorPos, open[openCount].pos, 12)) open[openCount++].guid = guid;
    };
    for (unsigned i = 0; i < a.doorCount && i < kMaxDoors; ++i) add(doorRuns[i], a.doors[i].guid);
    for (const DoorRun& r : markRuns) if (r.pass) add(r, r.guid);
    if (!openCount) return;
    InterlockedIncrement(&clipPasses);
    uint32_t manager = 0, world = 0;
    if (!Read(base + bot::kHavokManagerRva, &manager, 4) || !manager || !Read(manager + bot::kHavokWorld, &world, 4) || !world) {
        InterlockedIncrement(&clipFaults);
        return;
    }
    // Every body with its filter info: the fixed island, then the active and inactive ones.
    unsigned n = 0;
    auto walkIsland = [&](uint32_t island) {
        uint32_t bodies = 0, count = 0;
        if (!island || !Read(island + bot::kIslandBodies, &bodies, 4) || !bodies || !Read(island + bot::kIslandBodies + 4, &count, 4) || count > kMaxClipBodies) return;
        for (uint32_t j = 0; j < count && n < kMaxClipBodies; ++j) {
            uint32_t body = 0, info = 0;
            if (Read(bodies + j * 4, &body, 4) && body && Read(body + kBodyFilterInfo, &info, 4) && (info & kFilterTagMask) == kFilterDetail) clipBodies[n++] = {body, info};
        }
    };
    uint32_t fixed = 0;
    Read(world + kFixedIsland, &fixed, 4);
    walkIsland(fixed);
    for (const uint32_t list : {bot::kActiveIslands, bot::kInactiveIslands}) {
        uint32_t islands = 0, count = 0;
        if (!Read(world + list, &islands, 4) || !islands || !Read(world + list + 4, &count, 4) || count > 2048) continue;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t island = 0;
            if (Read(islands + i * 4, &island, 4)) walkIsland(island);
        }
    }
    InterlockedExchange(&clipWalked, static_cast<LONG>(n));
    LONG npcOnly = 0;
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t detail = clipBodies[i].info << 2;
        uint32_t typeFlags[2]{};
        if (!Read(detail, typeFlags, sizeof(typeFlags)) || !(typeFlags[1] & kTypeNpc) || (typeFlags[1] & kTypePlayer)) continue;
        ++npcOnly;
        const uint32_t body = clipBodies[i].body;
        float pos[3]{};
        Read(body + kBodyPos, pos, sizeof(pos));
        unsigned nearest = 0;
        float best = 1e9f;
        for (unsigned d = 0; d < openCount; ++d) {
            const float dx = pos[0] - open[d].pos[0], dy = pos[1] - open[d].pos[1], dz = pos[2] - open[d].pos[2];
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < best) { best = dist; nearest = d; }
        }
        const uint32_t owner = bot::EntityOfBody(body);
        uint32_t vtable = 0, guid[4]{};
        if (owner) { Read(owner, &vtable, 4); Read(owner + kEntityGuid, guid, sizeof(guid)); }
        const bool firstSight = !ClipSeen(body);
        if (best > clipReachM) {
            if (firstSight) {
                InterlockedIncrement(&clipsAway);
                if (InterlockedIncrement(&clipLines) <= 40)
                    mpmap::Event("arena %s: necro-only collision left alone, %.1f m from the nearest usable door: body %08X of %08X-%08X-%08X-%08X (vtable %08X) at (%.2f %.2f %.2f), type %08X flags %08X",
                                 a.name, best, body, guid[0], guid[1], guid[2], guid[3], vtable, pos[0], pos[1], pos[2], typeFlags[0], typeFlags[1]);
            }
            continue;
        }
        // A filter record another body shares stays as it is.
        unsigned users = 0;
        for (unsigned k = 0; k < n; ++k) users += clipBodies[k].info == clipBodies[i].info ? 1u : 0u;
        if (users > 1) {
            if (firstSight) {
                InterlockedIncrement(&clipsShared);
                if (InterlockedIncrement(&clipLines) <= 40)
                    mpmap::Event("arena %s: necro-only collision at door %08X-%08X NOT opened, its filter record %08X is shared by %u bodies: body %08X of %08X-%08X-%08X-%08X",
                                 a.name, open[nearest].guid[0], open[nearest].guid[1], detail, users, body, guid[0], guid[1], guid[2], guid[3]);
            }
            continue;
        }
        const uint32_t flags = typeFlags[1] & ~kTypeNpc;
        uint32_t after = 0;
        const bool written = WriteBytes(detail + 4, &flags, 4) && Read(detail + 4, &after, 4) && after == flags;
        if (!written) InterlockedIncrement(&clipFaults);
        else InterlockedIncrement(firstSight ? &clipsOpened : &clipsReopened);
        if (InterlockedIncrement(&clipLines) <= 40)
            mpmap::Event("arena %s: necro-only collision %s at door %08X-%08X (%.2f m): body %08X of %08X-%08X-%08X-%08X (vtable %08X) at (%.2f %.2f %.2f), "
                         "filter %08X type %08X flags %08X -> %08X%s", a.name, firstSight ? "opened" : "closed again by the game, opened again", open[nearest].guid[0],
                         open[nearest].guid[1], best, body, guid[0], guid[1], guid[2], guid[3], vtable, pos[0], pos[1], pos[2], detail, typeFlags[0], typeFlags[1], after,
                         written ? "" : " (WRITE FAILED)");
    }
    InterlockedExchange(&clipNpcOnly, npcOnly);
}

// --- rooms ---
// The stream manager's handle table (+0x5C buckets, +0x60 count, nodes {handle,
// stream, next}). A stream's name pointer is [[stream+0x18]]+4, and that is the
// pointer the TOC lookup returns for a member GUID (live 09.10.: the TOC slot
// is stream+0x18). The first build compared the lookup's result with handles.
struct Loaded { uint32_t handle, stream, name; };
unsigned LoadedStreams(Loaded* out, unsigned max) {
    uint32_t manager = 0, table = 0, buckets = 0;
    if (!Read(base + kStreamManager, &manager, 4) || !manager || !Read(manager + 0x5C, &table, 4) || !Read(manager + 0x60, &buckets, 4) || !table || !buckets ||
        buckets > 4096) return 0;
    unsigned n = 0;
    for (uint32_t b = 0; b < buckets && n < max; ++b) {
        uint32_t node = 0;
        if (!Read(table + 4 * b, &node, 4)) continue;
        for (unsigned hops = 0; node && hops < 64 && n < max; ++hops) {
            uint32_t handle = 0, stream = 0, slot = 0, identity = 0, name = 0, next = 0;
            Read(node, &handle, 4);
            if (Read(node + 4, &stream, 4) && stream && Read(stream + 0x18, &slot, 4) && slot && Read(slot, &identity, 4) && identity && Read(identity + 4, &name, 4) && name)
                out[n++] = Loaded{handle, stream, name};
            if (!Read(node + 8, &next, 4)) break;
            node = next;
        }
    }
    return n;
}
// A spawn spot is safe once every room of its stream set that has a TOC is loaded (the
// room pass's view, a second old at most); streaming=sets: and its set is wanted.
bool SpotLoaded(const Arena& a, unsigned spot) {
    if (spot >= a.spotCount) return false;
    const int s = coop_arena::SetForPoint(a, a.spots[spot]);
    if (s < 0 || !a.setStart) return true;   // no set known: the rooms are the arena's
    if (a.streaming && !(static_cast<uint32_t>(InterlockedCompareExchange(&wantedMask, 0, 0)) >> s & 1u)) return false;
    unsigned registered = 0;
    for (unsigned k = a.setStart[s]; k < a.setStart[s + 1]; ++k) {
        const unsigned m = a.setMember[k];
        if (m >= kMaxMembers || !memberRegistered[m]) continue;
        ++registered;
        if (!memberLoaded[m]) return false;
    }
    return registered > 0;
}
float lastInside[3]{};
bool haveLastInside = false;
const float* LastInside() { return haveLastInside ? lastInside : nullptr; }
void RoomPass(const Arena& a, DWORD now) {
    if (!mpmap::Have(mpmap::sLookup)) {
        static bool said = false;
        if (!said) { said = true; mpmap::Event("arena %s: the stream lookup did not match at start; rooms are neither checked nor shown", a.name); }
        return;
    }
    static Loaded table[512];
    const unsigned count = LoadedStreams(table, 512);
    unsigned registered = 0, loaded = 0, visible = 0, shownNow = 0;
    int localSet = -1;
    const uint32_t mask = a.streaming ? Refresh(a, localSet) : 0xFFFFFFFFu;
    if (a.streaming && mask != activatedMask) {
        // the players moved into other sets: the switch that brings them comes from the local set
        const int setIndex = localSet >= 0 ? localSet : 0;
        const uint32_t node = now - activatedAt >= 2000 ? mirror::ObjectByGuid(a.sets[setIndex]) : 0;
        if (node) {
            bool ok = false;
            __try {
                reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + streams::kActivateRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(node)), 0);
                ok = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedIncrement(&faults);
            }
            activatedAt = now;
            InterlockedIncrement(&setActivations);
            if (InterlockedIncrement(&maskChanges) <= 60)
                mpmap::Event("arena %s streaming: sets %08X wanted (was %08X), local Isaac in set %d; set %d activated: %s", a.name, mask, activatedMask, localSet,
                             setIndex, ok ? "called" : "FAULTED");
            activatedMask = mask;
            maskTries = 0;
        }
    }
    for (unsigned i = 0; i < a.memberCount && i < kMaxMembers; ++i) {
        memberHandles[i] = 0;
        memberRegistered[i] = memberLoaded[i] = false;
        if (a.streaming && !selected[i]) continue;   // left to the game: it may unload
        const uint32_t name = mpmap::LookupStream(coop_mpmap::StreamHash(a.members[i]));
        if (!name) continue;
        memberRegistered[i] = true;
        ++registered;
        const Loaded* hit = nullptr;
        for (unsigned k = 0; k < count && !hit; ++k) if (table[k].name == name) hit = &table[k];
        if (!hit) continue;
        const uint32_t handle = hit->handle, stream = hit->stream;
        memberHandles[i] = handle;
        uint32_t status = 0;
        if (!Read(stream + 0x24, &status, 4) || status != 3) continue;
        memberLoaded[i] = true;
        ++loaded;
        if (streams::vis::Visible(stream)) { ++visible; continue; }
        if (stream && streams::vis::SetVisible(handle, true, false)) {
            ++shownNow;
            InterlockedIncrement(&shows);
            if (InterlockedIncrement(&showLines) <= 60)
                mpmap::Event("arena room %08X-%08X-%08X-%08X (handle %08X) was hidden: shown", a.members[i][0], a.members[i][1], a.members[i][2], a.members[i][3], handle);
        }
    }
    if (loaded != lastLoaded && lastLoaded != ~0u && loaded > lastLoaded) resendAt = now + 2000;
    if (registered != lastRegistered || loaded != lastLoaded || visible + shownNow != lastVisible) {
        lastRegistered = registered;
        lastLoaded = loaded;
        lastVisible = visible + shownNow;
        mpmap::Event("arena %s rooms: %u of %u in a TOC, %u loaded, %u visible (%u shown just now) %lu ms after arrival; %u streams in the manager", a.name,
                     registered, a.memberCount, loaded, visible + shownNow, shownNow, static_cast<unsigned long>(now - runningAt), count);
    }
    // Rooms the arrival's set switch did not bring: switch to the start set again (the
    // engine's own level start call, Activate(node, 0)); the merge adds the rest.
    if (a.streaming) {
        // a wanted room still missing a while after its switch: the switch once more (three times per change)
        if (loaded < registered && now - activatedAt >= 5000 && maskTries < 3) {
            ++maskTries;
            activatedMask = 0;   // the next pass activates the local set again
        }
        return;
    }
    if (loaded < registered && now - runningAt >= kActivateAfterMs && activations < 2 && now - activatedAt >= 15000) {
        const uint32_t node = mirror::ObjectByGuid(a.sets[0]);
        ++activations;
        activatedAt = now;
        bool ok = false;
        if (node) {
            __try {
                reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + streams::kActivateRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(node)), 0);
                ok = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedIncrement(&faults);
            }
        }
        mpmap::Event("arena %s: %u of %u rooms not loaded after %lu ms; start set %08X-%08X node %08X activated again: %s", a.name, registered - loaded, registered,
                     static_cast<unsigned long>(now - runningAt), a.sets[0][0], a.sets[0][1], node, !node ? "NO NODE" : ok ? "called" : "FAULTED");
    }
}

// --- the start spot ---
// The level loads at the arena's checkpoint, which is not where the arena begins
// (ch03: in front of the save station, 4 m behind the recorded spots). A new Isaac of
// the arena (arrival, checkpoint restart) goes onto a recorded spot (user, 16.10.):
// solo a random one; in co-op the free spot nearest to a partner who already stands on
// a spot. Nobody on a spot yet: the host takes a random one at once, a joiner whose game
// loaded first waits up to kStartWaitMs for one. Only the recorded spots are used: with
// more players than spots one is shared. He faces the middle of the arena's walk (the spots
// carry no facing), the camera too; 3 s later the spot is checked once.
constexpr DWORD kStartWaitMs = 4000;
volatile LONG startPlacements = 0, startPlaceFailures = 0, startRandom = 0, startBesidePartner = 0, startWaits = 0;
uint32_t startChecked = 0, startWanted = 0;
DWORD startPlacedAt = 0, startWantedAt = 0, startPickedAt = 0;
int startTarget = -1;
char startWhy[96] = "";
// 17.10.: arena_ch10's spot 6 never counted as loaded and the curtain stood 23.5 s; a solo or
// first Isaac now takes a spot whose rooms are already in, and waits 6 s at most for any other.
constexpr DWORD kStartLoadMs = 6000;
bool startWaitLogged = false;
float startSpot[3]{};
// Where a player may not stand (user, 16.10.: the crypt's lift has no navigation, so an Isaac
// on it was out of every enemy's reach): pushed back to where he last stood outside, like an
// invisible wall; the director's points there do not count as inside either.
struct NoGo { const char* arena; float x0, z0, x1, z1, y0, y1; };
constexpr NoGo kNoGo[] = {
    {"arena_ch03", 56.8f, -474.0f, 60.2f, -470.6f, -31.5f, -26.0f},   // the lift platform, Mover 3926B250-8B976F99 at (58.5 -30.0 -472.0)
};
bool InNoGo(const Arena& a, const float* p) {
    for (const NoGo& n : kNoGo)
        if (_stricmp(n.arena, a.name) == 0 && p[0] >= n.x0 && p[0] <= n.x1 && p[2] >= n.z0 && p[2] <= n.z1 && p[1] >= n.y0 && p[1] <= n.y1) return true;
    return false;
}
float lastFree[3]{};
bool haveLastFree = false;
volatile LONG noGoPushes = 0;
void NoGoPass(const Arena& a, uint32_t local) {
    float p[3]{};
    if (!local || !Read(local + mpmap::kPlayerPos, p, sizeof(p)) || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return;
    if (!InNoGo(a, p)) { std::memcpy(lastFree, p, sizeof(p)); haveLastFree = true; return; }
    if (!haveLastFree) return;
    alignas(16) float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float rows[12]{};
    if (Read(local + mpmap::kPlayerPos - 0x30, rows, sizeof(rows)) && std::isfinite(rows[8]) && std::isfinite(rows[10]) &&
        std::fabs(rows[8] * rows[8] + rows[9] * rows[9] + rows[10] * rows[10] - 1.0f) < 0.1f)
        std::memcpy(matrix, rows, sizeof(rows));
    matrix[3] = matrix[7] = matrix[11] = 0.0f;
    matrix[12] = lastFree[0];
    matrix[13] = lastFree[1];
    matrix[14] = lastFree[2];
    matrix[15] = 1.0f;
    const int placed = mpmap::PlaceRaw(local, matrix);
    if (InterlockedIncrement(&noGoPushes) <= 20)
        mpmap::Event("arena %s: Isaac %08X stepped onto a closed place at (%.2f %.2f %.2f): back to (%.2f %.2f %.2f) %s", a.name, local, p[0], p[1], p[2], lastFree[0],
                     lastFree[1], lastFree[2], placed > 0 ? "" : "FAILED");
}
// Black ("LOADING") from the new Isaac's first frame until he stands on his start spot in
// the kit's suit (user, 16.10.: the arena showed him at its checkpoint for a few seconds,
// then he jumped to the spot and changed his suit). [Arena] StartCurtain.
bool startCurtain = true, curtainUp = false;
DWORD curtainLiftAt = 0, curtainSince = 0;
volatile LONG curtains = 0, curtainsLifted = 0;
volatile LONG oldStartCurtains = 0;
char nativeWhyNot[96] = "";   // why the native start (below) did not run, for the old path's lines
void LiftStartCurtain(DWORD now, const char* why) {
    if (!curtainUp) return;
    overlay::LiftCurtain();
    curtainUp = false;
    curtainLiftAt = 0;
    InterlockedIncrement(&curtainsLifted);
    mpmap::Event("arena start curtain lifted after %lu ms: %s", static_cast<unsigned long>(now - curtainSince), why);
}
bool FacingFor(const float* pos, float* dir);   // below, with the native start
void PlaceAtStart(const Arena& a, uint32_t local, DWORD now) {
    if (!a.spotCount) { startWanted = 0; LiftStartCurtain(now, "no start spots"); return; }
    float partners[8 * 3]{};
    const int n = PartnerPositions(partners, 8);
    int anchor = -1, anchorSpot = -1;
    for (int k = 0; k < n && anchor < 0; ++k) {
        const int s = coop_arena::SpotAt(a, partners + 3 * k, 2.0f);
        if (s >= 0) { anchor = k; anchorSpot = s; }
    }
    if (anchor < 0 && n > 0 && machineIndex != 0 && now - startWantedAt < kStartWaitMs) {
        if (!startWaitLogged) {
            startWaitLogged = true;
            InterlockedIncrement(&startWaits);
            mpmap::Event("arena %s start: Isaac %08X (machine %u) waits up to %lu ms for a partner on a spot (%d partner(s) seen, none on a spot)", a.name, local,
                         machineIndex, static_cast<unsigned long>(kStartWaitMs), n);
        }
        return;
    }
    if (startTarget < 0) {
        bool shared = false;
        if (anchor >= 0) {
            startTarget = coop_arena::FreeSpotNear(a, partners + 3 * anchor, partners, static_cast<unsigned>(n), 1.5f, shared);
        } else {
            int loaded[32];
            unsigned loadedCount = 0;
            for (unsigned s = 0; s < a.spotCount && s < 32; ++s) if (SpotLoaded(a, s)) loaded[loadedCount++] = static_cast<int>(s);
            startTarget = loadedCount ? loaded[Random() % loadedCount] : static_cast<int>(Random() % a.spotCount);
        }
        startPickedAt = now;
        InterlockedIncrement(anchor >= 0 ? &startBesidePartner : &startRandom);
        if (anchor >= 0) std::snprintf(startWhy, sizeof(startWhy), "the free spot nearest to the partner on spot %d%s", anchorSpot + 1, shared ? ", all taken: shared" : "");
        else std::snprintf(startWhy, sizeof(startWhy), "%s", n ? "random (no partner on a spot)" : "random (solo)");
        // its rooms load first (streaming=sets: its set is wanted until he is there)
        const int set = coop_arena::SetForPoint(a, a.spots[startTarget]);
        if (a.streaming && set >= 0) { forcedSet = set; forcedUntil = now + kStartLoadMs + 5000; }
        mpmap::Event("arena %s start: Isaac %08X (machine %u) goes to spot %d of %u, %s; its rooms (set %d) %s", a.name, local, machineIndex, startTarget + 1,
                     a.spotCount, startWhy, set, SpotLoaded(a, static_cast<unsigned>(startTarget)) ? "are loaded" : "load first");
    }
    int pick = startTarget;
    const bool ready = SpotLoaded(a, static_cast<unsigned>(pick));
    if (!ready && now - startPickedAt < kStartLoadMs) return;
    if (!ready) {
        // the rooms never came: the loaded spot nearest to where he stands
        float here[3]{};
        Read(local + mpmap::kPlayerPos, here, sizeof(here));
        int alt = -1;
        float altCost = 1e30f;
        for (unsigned s = 0; s < a.spotCount; ++s)
            if (SpotLoaded(a, s) && coop_arena::SpotCost(a.spots[s], here) < altCost) { altCost = coop_arena::SpotCost(a.spots[s], here); alt = static_cast<int>(s); }
        mpmap::Event("arena %s start: the rooms of spot %d did not load in %lu ms; %s", a.name, pick + 1, static_cast<unsigned long>(kStartLoadMs),
                     alt >= 0 ? "the nearest loaded spot instead" : "NO spot has loaded rooms, staying at the checkpoint");
        if (alt < 0) { startWanted = 0; startTarget = -1; LiftStartCurtain(now, "no spot loaded, at the checkpoint"); return; }
        pick = alt;
    }
    float spot[3];
    std::memcpy(spot, a.spots[pick], sizeof(spot));
    startWanted = 0;
    startTarget = -1;
    if (forcedSet >= 0) forcedUntil = now + 5000;   // until the switch to his own set
    alignas(16) float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float rows[12]{};
    if (Read(local + mpmap::kPlayerPos - 0x30, rows, sizeof(rows)) && std::isfinite(rows[8]) && std::isfinite(rows[10]) &&
        std::fabs(rows[8] * rows[8] + rows[9] * rows[9] + rows[10] * rows[10] - 1.0f) < 0.1f)
        std::memcpy(matrix, rows, sizeof(rows));
    matrix[3] = matrix[7] = matrix[11] = 0.0f;
    matrix[12] = spot[0];
    matrix[13] = spot[1] + 0.2f;
    matrix[14] = spot[2];
    matrix[15] = 1.0f;
    // the same facing as the native start: toward the middle of the arena's walk
    float face[2]{};
    if (FacingFor(spot, face)) lifecycle::TurnRows(matrix, face);
    float was[3]{};
    Read(local + mpmap::kPlayerPos, was, sizeof(was));
    const int placed = mpmap::PlaceRaw(local, matrix);
    if (placed > 0) mpmap::AimFromBody(local, "the arena's old start");
    InterlockedIncrement(placed > 0 ? &startPlacements : &startPlaceFailures);
    std::memcpy(startSpot, spot, sizeof(startSpot));
    startPlacedAt = now;
    startChecked = placed > 0 ? local : 0;
    if (curtainUp) curtainLiftAt = now + 1500;   // the kit's suit streams in meanwhile
    mpmap::Event("arena %s start: Isaac %08X (machine %u) from (%.2f %.2f %.2f) onto spot %d of %u (%.2f %.2f %.2f), %s, rooms loaded after %lu ms, %lu ms after he "
                 "appeared, facing (%.2f %.2f): %s", a.name, local, machineIndex, was[0], was[1], was[2], pick + 1, a.spotCount, spot[0], spot[1], spot[2], startWhy,
                 static_cast<unsigned long>(now - startPickedAt), static_cast<unsigned long>(now - startWantedAt), matrix[8], matrix[10],
                 placed == 2 ? "entity+cmc" : placed == 1 ? "entity only (no cmc found)" : "FAULT");
}
void CheckStart(uint32_t local, DWORD now) {
    if (!startChecked || now - startPlacedAt < 3000) return;
    float at[3]{};
    const bool read = startChecked == local && Read(local + mpmap::kPlayerPos, at, sizeof(at));
    const float dx = at[0] - startSpot[0], dy = at[1] - startSpot[1], dz = at[2] - startSpot[2];
    if (!read) mpmap::Event("arena start check: Isaac %08X is gone or no longer the local Isaac", startChecked);
    else mpmap::Event("arena start check: Isaac %08X now at (%.2f %.2f %.2f), %.1f m from his start spot 3 s after the placement", local, at[0], at[1], at[2],
                      std::sqrt(dx * dx + dy * dy + dz * dz));
    startChecked = 0;
}

// --- the player's kit ---
// An arena loads as a new game at its checkpoint: patient suit and no gun (ch04,
// 09.10.). Each new Isaac (arrival, death restart) gets the kit once, 2 s after he
// appears: [Arena] Suit if he wears the patient suit or none (the bots follow
// him out of it), the plasma cutter with ammo, stasis and kinesis (mpmap's MP kit).
bool outfit = true, bonusGiven = false;
uint32_t suit[4] = {0x30C9B946u, 0xE243D95Fu, 0x54544950u, 0x32303153u};   // Engineering Suit
uint32_t outfitted = 0, isaacSeen = 0;
DWORD isaacSeenAt = 0;
LONG respawnEpochSeen = 0;
volatile LONG outfits = 0, suitChanges = 0, respawnedBodies = 0, ownSuitsBack = 0, necroSkips = 0;
// The suit the local Isaac last wore alive (bought in the arena or the kit's), for his respawned
// body: 24.10. a respawn put everyone back into [Arena] Suit and threw the bought suit away.
uint32_t ownSuit[4]{};
bool haveOwnSuit = false;
void RememberOwnSuit(uint32_t local) {
    uint32_t worn[4]{};
    if (!local || !bot::WornSuit(local, worn)) return;
    const int idx = bot::SuitOfGuid(worn);
    if (idx < 0 || idx == bot::PatientSuit()) return;
    std::memcpy(ownSuit, worn, sizeof(ownSuit));
    haveOwnSuit = true;
}
const uint32_t* KitSuit(const char*& why);   // below, with the native start
void Outfit(uint32_t local, DWORD now) {
    if (!local || local == outfitted) return;
    if (local != isaacSeen) {
        isaacSeen = local;
        isaacSeenAt = now;
        // a new Isaac of the arena goes to a start spot; a respawned body stays where respawn put it
        const Arena* a = Active();
        if (startCurtain && a && a->spotCount && respawn::stage == 0 && InterlockedCompareExchange(&respawn::epoch, 0, 0) == respawnEpochSeen) {
            InterlockedIncrement(&oldStartCurtains);
            mpmap::Event("arena start curtain raised for Isaac %08X: the old start (%s)", local, nativeWhyNot[0] ? nativeWhyNot : "native start not armed");
            overlay::Curtain(kStartLoadMs + kStartWaitMs + 8000);
            curtainUp = true;
            curtainLiftAt = 0;
            curtainSince = now;
            InterlockedIncrement(&curtains);
        }
        return;
    }
    if (now - isaacSeenAt < 2000) return;
    outfitted = local;
    // a respawned body has its own things back (respawn::Finish): no second kit, no bonus
    const LONG epoch = InterlockedCompareExchange(&respawn::epoch, 0, 0);
    if (epoch != respawnEpochSeen) {
        respawnEpochSeen = epoch;
        InterlockedIncrement(&respawnedBodies);
        // His guns and items came back (respawn::Finish), but the body comes from the player
        // template in the patient suit (user, 17.10.): only the suit changes, into the one he wore alive.
        const int worn = outfit ? bot::WornSuitIndex(local) : -2;
        const char* ruleWhy = "";
        const uint32_t* want = mpmap::RuleSuit(ruleWhy);   // Engineering, or the setup's suit in Mercenaries
        const int wantIdx = want ? bot::SuitOfGuid(want) : -1;
        const bool change = outfit && want && wantIdx >= 0 && worn != wantIdx;
        const bool dressed = change && coop_uniform::BotDress(local, want);
        if (dressed) {
            InterlockedIncrement(&suitChanges);
            if (haveOwnSuit) InterlockedIncrement(&ownSuitsBack);
        }
        mpmap::Event("arena: Isaac %08X is a respawned body (respawn epoch %ld): no kit; wears the %s%s%s (%s)", local, epoch,
                     worn >= 0 ? bot::SuitName(worn) : "(no registered suit)", !change ? "" : dressed ? ", changes into the " : ", suit change FAILED for the ",
                     !change ? "" : wantIdx >= 0 ? bot::SuitName(wantIdx) : "[Arena] Suit", ruleWhy);
        LiftStartCurtain(now, "a respawned body");
        return;
    }
    // a new Isaac of the arena (arrival or checkpoint restart): the director's restart waits for this
    InterlockedIncrement(&readyArrivals);
    startWanted = local;   // PlaceAtStart from the tick, maybe after a wait for a partner
    startWantedAt = now;
    startWaitLogged = false;
    if (!outfit) return;
    InterlockedIncrement(&outfits);
    // the start of a run follows the suit rule like the native start: whatever he wore before
    const int worn = bot::WornSuitIndex(local);
    const char* ruleWhy = "";
    const uint32_t* want = KitSuit(ruleWhy);
    const int wantIdx = bot::SuitOfGuid(want);
    const bool change = wantIdx >= 0 && worn != wantIdx;
    const bool dressed = change && coop_uniform::BotDress(local, want);
    if (dressed) InterlockedIncrement(&suitChanges);
    mpmap::Event("arena kit for Isaac %08X: wears the %s; %s %s (%s)", local, worn >= 0 ? bot::SuitName(worn) : "(no registered suit)",
                 !change ? "keeps it, it is" : dressed ? "changes into the" : "suit change FAILED for the", wantIdx >= 0 ? bot::SuitName(wantIdx) : "(unregistered suit)",
                 ruleWhy);
    mpmap::GiveLoadout(local);
    mpmap::EnableModules(local);
    // once per arena start, not again after every checkpoint restart
    if (!bonusGiven) {
        bonusGiven = true;
        progress::StartBonus(local, "the arena kit");
    }
}

// --- the native start ([Lifecycle] NativePlacement, 02.11.) ---
// The first body of the arena is made by the level's own load. Before the level request this
// machine's start spot and suit are armed: Player::Init puts the body on the spot facing the
// arena's middle (lifecycle's hook at 0x008A8505; Init's AimingData::UpdateSource calls at
// 0x008A8852/0x008A8884 then take the camera's aim from that matrix) and dresses it in the kit's suit
// (uniform's birth swap at 0x008A8670), so the first frame after the loading screen shows him
// there, dressed; the kit follows in the first tick the level runs. Offline (02.11. log of the
// arena_ch01 test): nothing but Init moved the arena body after the load (no teleport events,
// the old placement held 3 s later), so Init's move is the only one to replace. The old kit
// change, teleport and curtain run only when a part did not take, and say which.
const char kNativeStartWhy[] = "the arena start";
bool nativeArmed = false, nativeBirthArmed = false;
// A restart's arm is made while the old level still runs (02.11., 81C9F06A run: its first tick
// judged the old body and cancelled the arm, so the restart's Isaac never got it): judged only once
// the level manager has left RUNNING since.
bool nativeNeedsReload = false;
int nativeSpot = -1;
float nativeSpotPos[3]{};
uint32_t nativeSuit[4]{};
const char* nativeSuitWhy = "none";
const char* nativeSpotWhy = "";
const char* nativeWhen = "";
DWORD nativeArmedAt = 0, nativeRunningAt = 0;
volatile LONG nativeStarts = 0, nativePlacementMisses = 0, nativeSuitMisses = 0, suitFallbackCurtains = 0;
// The set the recording started in (the checkpoint's): only its spots are certain to be loaded.
int StartSet(const Arena& a) { return a.streaming && a.walkSet && a.walkCount && a.walkSet[0] != 255 ? a.walkSet[0] : -1; }
int PickNativeSpot(const Arena& a, const char*& why) {
    int candidates[32];
    unsigned n = 0;
    const int set = StartSet(a);
    for (unsigned s = 0; s < a.spotCount && n < 32; ++s)
        if (set < 0 || coop_arena::SetForPoint(a, a.spots[s]) == set) candidates[n++] = static_cast<int>(s);
    if (!n) { why = "no spot in the start set"; return -1; }
    if (!network || !peerTable.Count()) { why = "random (solo)"; return candidates[Random() % n]; }
    // co-op: every machine its own spot, without knowing the others' picks yet
    uint32_t h = 2166136261u;
    for (const char* c = a.name; *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
    why = "by machine number (co-op)";
    return candidates[(h + machineIndex) % n];
}
// The suit rule for the first spawn of a run and an arena restart (a new run): the setup's suit in
// Mercenaries, else [Arena] Suit (the arena kit), else the Engineering Suit. A bought suit is not kept.
const uint32_t* KitSuit(const char*& why) {
    if (loadout::ApplyMode()) {
        const char* name = nullptr;
        if (const uint32_t* row = loadout::SuitRow(&name)) { why = "the rule: the character setup's suit (mercenaries)"; return row; }
    }
    if (outfit) { why = "the arena kit: [Arena] Suit"; return suit; }
    why = "the rule: Engineering Suit";
    return mpmap::kEngineeringSuit;
}
// The spots carry no facing of their own: each faces the middle of the arena's walk.
bool FacingFor(const float* pos, float* dir) {
    const Arena* a = Active();
    if (!a || !pos) return false;
    const float cx = (a->box[0] + a->box[3]) * 0.5f, cz = (a->box[2] + a->box[5]) * 0.5f;
    const float dx = cx - pos[0], dz = cz - pos[2], len = std::sqrt(dx * dx + dz * dz);
    if (!(len > 0.5f)) return false;
    dir[0] = dx / len;
    dir[1] = dz / len;
    return true;
}
void ArmNativeStart(const Arena& a, const char* when) {
    nativeArmed = nativeBirthArmed = false;
    nativeSpot = -1;
    nativeRunningAt = 0;
    nativeWhen = when;
    nativeWhyNot[0] = 0;
    if (!lifecycle::PlacementReady()) { std::snprintf(nativeWhyNot, sizeof(nativeWhyNot), "the placement hook is not ready"); }
    else if (!a.spotCount) { std::snprintf(nativeWhyNot, sizeof(nativeWhyNot), "the arena has no start spots"); }
    else {
        nativeSpot = PickNativeSpot(a, nativeSpotWhy);
        if (nativeSpot < 0) std::snprintf(nativeWhyNot, sizeof(nativeWhyNot), "%s", nativeSpotWhy);
    }
    if (nativeSpot >= 0) {
        std::memcpy(nativeSpotPos, a.spots[nativeSpot], sizeof(nativeSpotPos));
        const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, nativeSpotPos[0], nativeSpotPos[1] + 0.2f, nativeSpotPos[2], 1};
        float face[2]{};
        const bool faced = FacingFor(nativeSpotPos, face);
        nativeArmed = lifecycle::ArmLoadPlacement(m, kNativeStartWhy, true, 180000, faced ? face : nullptr);
        if (!nativeArmed) std::snprintf(nativeWhyNot, sizeof(nativeWhyNot), "the placement could not be armed");
    }
    const uint32_t* want = KitSuit(nativeSuitWhy);
    if (want) std::memcpy(nativeSuit, want, sizeof(nativeSuit));
    lifecycle::bornPlayer = 0;
    nativeBirthArmed = want && coop_uniform::Arm() && coop_uniform::ArmBirth(want, "the arena start");
    if (nativeBirthArmed) lifecycle::birthLoadOnly = true;
    nativeArmedAt = GetTickCount();
    mpmap::Event("arena %s native start armed (%s): spot %d of %u (%.2f %.2f %.2f, %s), placement %s, birth suit %s (%s)", a.name, when, nativeSpot + 1, a.spotCount,
                 nativeSpotPos[0], nativeSpotPos[1], nativeSpotPos[2], nativeSpotWhy, nativeArmed ? "armed" : nativeWhyNot, nativeBirthArmed ? "armed" : "NOT armed",
                 nativeSuitWhy);
}
void CancelNativeStart(const char* why) {
    if (nativeArmed) lifecycle::CancelPlacement(why);
    if (nativeBirthArmed && lifecycle::birthPending) coop_uniform::DisarmBirth(false);
    nativeArmed = nativeBirthArmed = false;
}
// The first tick the level runs with a new Isaac of ours: did the native start take, and the kit.
// kOld when the old start must run (no arm, or Init did not place him); kWait while a restart's
// body stands before the reload (it is made while the level still runs, the loading screen follows).
enum NativeResult { kOld, kNative, kWait };
volatile LONG nativeWaits = 0, nativeReloadTimeouts = 0;
uint32_t nativeWaitLogged = 0;
NativeResult NativeArrival(const Arena& a, uint32_t local, DWORD now) {
    if (!nativeArmed && !nativeBirthArmed) return kOld;
    if (nativeNeedsReload) {
        // the restart's own body (Init took the arm) or the arm still waits for one: the old start must not judge it yet
        const uint32_t placedEarly = nativeArmed ? lifecycle::LoadPlaced(kNativeStartWhy) : 0;
        const bool ours = placedEarly == local || (nativeArmed && lifecycle::LoadArmed(kNativeStartWhy));
        if (!ours) { nativeNeedsReload = false; return NativeArrival(a, local, now); }
        if (now - nativeArmedAt > 10000) {
            InterlockedIncrement(&nativeReloadTimeouts);
            mpmap::Event("arena restart: no loading screen seen 10 s after the arm; Isaac %08X is judged now", local);
            nativeNeedsReload = false;
        } else {
            if (nativeWaitLogged != local) {
                nativeWaitLogged = local;
                InterlockedIncrement(&nativeWaits);
                mpmap::Event("arena restart: Isaac %08X stands before the reload (%s); kit and judgement wait for the level to run again", local,
                             placedEarly == local ? "Init put him on the spot" : "the arm is not taken yet");
            }
            return kWait;
        }
    }
    if (!nativeRunningAt) nativeRunningAt = Stamp(now);
    const uint32_t placed = nativeArmed ? lifecycle::LoadPlaced(kNativeStartWhy) : 0;
    const bool placedOk = placed && placed == local;
    const bool bornOk = nativeBirthArmed && lifecycle::bornPlayer == local;
    if (nativeBirthArmed && !bornOk && lifecycle::birthPending) coop_uniform::DisarmBirth(true);
    if (nativeArmed && !placedOk && lifecycle::LoadArmed(kNativeStartWhy)) lifecycle::CancelPlacement("the arena's Isaac never ran Init through the hook");
    const bool wasArmed = nativeArmed;
    nativeArmed = nativeBirthArmed = false;
    if (!placedOk) {
        InterlockedIncrement(&nativePlacementMisses);
        std::snprintf(nativeWhyNot, sizeof(nativeWhyNot), "%s", !wasArmed ? (nativeWhyNot[0] ? nativeWhyNot : "placement not armed")
                                                                 : placed ? "Init placed another body" : "Init never took the arm");
        mpmap::Event("arena spawn: Isaac %08X native placement NO (%s, placed %08X), birth suit %s (%s): the old start follows (kit change, teleport, curtain)", local,
                     nativeWhyNot, placed, bornOk ? "yes" : "no", nativeSuitWhy);
        return kOld;
    }
    InterlockedIncrement(&nativeStarts);
    outfitted = isaacSeen = local;
    isaacSeenAt = now;
    InterlockedIncrement(&readyArrivals);   // the director's restart waits for this
    std::memcpy(startSpot, nativeSpotPos, sizeof(startSpot));
    startPlacedAt = now;
    startChecked = local;
    startWanted = 0;
    startTarget = -1;
    const int set = coop_arena::SetForPoint(a, nativeSpotPos);
    if (a.streaming && set >= 0) { forcedSet = set; forcedUntil = now + 5000; }
    // the suit: born in it, or changed now under a short curtain (the fallback)
    const int worn = bot::WornSuitIndex(local);
    const int wantIdx = bot::SuitOfGuid(nativeSuit);
    const char* curtain = "not raised: native placement and birth suit took";
    if (!bornOk && wantIdx >= 0 && worn != wantIdx) {
        InterlockedIncrement(&nativeSuitMisses);
        const bool dressed = coop_uniform::BotDress(local, nativeSuit);
        if (dressed) InterlockedIncrement(&suitChanges);
        if (startCurtain && dressed) {
            overlay::Curtain(4000);
            curtainUp = true;
            curtainSince = now;
            curtainLiftAt = now + 1500;
            InterlockedIncrement(&suitFallbackCurtains);
            curtain = "RAISED 1.5 s: the birth suit did not take, the suit changes under it";
        } else {
            curtain = dressed ? "not raised ([Arena] StartCurtain=0), the suit changes in view" : "not raised; the suit change FAILED";
        }
    }
    mpmap::GiveLoadout(local, true);
    mpmap::EnableModules(local);
    if (!bonusGiven) {
        bonusGiven = true;
        progress::StartBonus(local, "the arena kit");
    }
    InterlockedIncrement(&outfits);
    float at[3]{};
    Read(local + mpmap::kPlayerPos, at, sizeof(at));
    const float dx = at[0] - nativeSpotPos[0], dz = at[2] - nativeSpotPos[2];
    char facing[96];
    mpmap::FacingText(local, facing, sizeof(facing));
    mpmap::Event("arena spawn: Isaac %08X native placement yes (spot %d of %u, %s, at %.2f %.2f %.2f, %.1f m from the spot, Init facing %.2f %.2f, now %s), birth suit %s "
                 "(%s, wears the %s), kit given at +%lu ms after the level ran (%lu ms after the arm, %s), curtain %s",
                 local, nativeSpot + 1, a.spotCount, nativeSpotWhy, at[0], at[1], at[2], std::sqrt(dx * dx + dz * dz), lifecycle::lastFacing[0], lifecycle::lastFacing[1],
                 facing, bornOk ? "yes" : "no", nativeSuitWhy, worn >= 0 ? bot::SuitName(worn) : "(no registered suit)", static_cast<unsigned long>(now - nativeRunningAt),
                 static_cast<unsigned long>(now - nativeArmedAt), nativeWhen, curtain);
    return kNative;
}
// lifecycle::OnRestartLanded, before the checkpoint restart rebuilds the arena: its new Isaac too.
void BeforeRestart() {
    if (state == State::Off) return;   // Launching too: a restart right after the arrival
    const Arena* a = Active();
    if (!a) return;
    ArmNativeStart(*a, "the arena restart");
    nativeNeedsReload = true;
}

// --- start and end ---
void Hooks(bool on) {
    streams::pin::arenaMerge = on ? Merge : nullptr;
    streams::vis::arenaKeepsVisible = on ? KeepsVisible : nullptr;
    mpmap::arenaBlocks = on ? Blocks : nullptr;
}
void End(const char* why) {
    const Arena* a = Active();
    mpmap::Event("=== arena %s over (%s) after %lu s: doors locked %ld, locked again %ld, rooms shown %ld, hides refused %ld; saves released ===",
                 a ? a->name : "?", why, static_cast<unsigned long>((GetTickCount() - launchedAt) / 1000), InterlockedCompareExchange(&doorLocks, 0, 0),
                 InterlockedCompareExchange(&doorRelocks, 0, 0), InterlockedCompareExchange(&shows, 0, 0),
                 InterlockedCompareExchange(&streams::vis::arenaHidesRefused, 0, 0));
    LiftStartCurtain(GetTickCount(), "the arena ended");
    CancelNativeStart("the arena ended");
    nativeNeedsReload = false;
    haveOwnSuit = false;   // the next run starts with the kit again
    Hooks(false);
    SetState(State::Off);
    active = -1;
    InterlockedExchange(&activeMirror, -1);
    mpmap::arenaHold = false;
    InterlockedIncrement(&ends);
}
// Game thread, from menu::Tick: the picker chose an arena.
bool Start(const char* name, DWORD now) {
    const int index = coop_arena::ArenaIndex(name);
    const char* refusal = !enabled ? "arenas are off ([Arena] Arenas=0)"
                        : index < 0 ? "not in generated_arenas.h"
                        : state != State::Off ? "an arena is already running"
                        : nullptr;
    if (refusal) {
        InterlockedIncrement(&refused);
        mpmap::Event("arena %s REFUSED: %s", name ? name : "?", refusal);
        return false;
    }
    const Arena& a = coop_arena::kArenas[index];
    active = index;
    InterlockedExchange(&activeMirror, index);
    for (DoorRun& r : doorRuns) r = DoorRun{};
    for (DoorRun& r : markRuns) r = DoorRun{};
    LoadSavedMarks(a);
    std::memset(memberHandles, 0, sizeof(memberHandles));
    lastRegistered = lastLoaded = lastVisible = ~0u;
    activations = 0;
    outfitted = isaacSeen = 0;
    startChecked = startWanted = 0;
    startTarget = forcedSet = -1;
    haveLastInside = false;
    bonusGiven = false;
    respawnEpochSeen = InterlockedCompareExchange(&respawn::epoch, 0, 0);
    activatedMask = 0;
    maskTries = 0;
    std::memset(setNearAt, 0, sizeof(setNearAt));
    resendAt = 0;
    clipPassAt = 0;
    clipsSeenCount = 0;
    InterlockedExchange(&wantedMask, -1);
    launchedAt = now;
    runningAt = settledAt = offMapSince = activatedAt = 0;
    mpmap::arenaHold = true;
    Hooks(true);
    SetState(State::Launching);
    mpmap::Event("=== arena %s (\"%s\") starting: level %s stream %s checkpoint %08X-%08X-%08X-%08X, %u sets, %u rooms, %u doors (%s), %u spots; "
                 "set switch hook %s, hide hook %s ===", a.name, a.label, a.level, a.stream, a.checkpoint[0], a.checkpoint[1], a.checkpoint[2], a.checkpoint[3],
                 a.setCount, a.memberCount, a.doorCount, setIsLockedOk ? "SetIsLocked ok" : "SetIsLocked MISMATCH, doors stay as they are", a.spotCount,
                 streams::pin::hooked ? "on" : "OFF (rooms are not held)", streams::vis::hooked ? "on" : "OFF (rooms may go dark)");
    const coop_arena::ArenaPoints* p = coop_arena::PointsFor(a.name);
    const LONG picked = InterlockedCompareExchange(&gamemode::mode, 0, 0);
    mpmap::Event("arena %s doors %s; families left out: %s; streaming %s", a.name, a.doorsOpen ? "open (every unlocked door held open)" : "by the door table",
                 a.exclude && a.exclude[0] ? a.exclude : "none", a.streaming ? "by stream sets" : "every room");
    mpmap::Event("arena %s modes %s: picked mode %ld plays %d; %u zone, %u capsule and %u respawn points; a death %s", a.name,
                 modesOn ? "on" : "off ([Arena] Modes=0)", picked, ModeNow(picked), p ? p->zoneCount : 0u, p ? p->capsuleCount : 0u, p ? p->respawnCount : 0u,
                 modesOn && respawn::enabled ? "respawns at the arena point nearest to the team" : "reloads the arena checkpoint");
    ArmNativeStart(a, "the arena launch");
    if (!mpmap::StartCampaignFromMenu(a.level, a.stream, a.checkpoint)) {
        End("the campaign launch was refused");
        InterlockedIncrement(&refused);
        return false;
    }
    InterlockedIncrement(&starts);
    return true;
}

// Game thread, from coop::Poll.
void Tick(DWORD now) {
    if (state == State::Off) return;
    const Arena* a = Active();
    if (!a) { End("no arena"); return; }
    __try {
        char map[64] = "", stream[64] = "";
        const bool haveMap = level::CurrentMap(map, sizeof(map), stream, sizeof(stream));
        const bool onLevel = haveMap && _stricmp(map, a->level) == 0;
        const bool running = mpmap::LmState(nullptr) == coop_mpmap::kRunning;
        const uint32_t local = LocalSlotZeroEntity();
        if (nativeNeedsReload && !running) nativeNeedsReload = false;   // the restart's load has begun
        // the native start's body: kit in the first tick the level runs, no curtain, no teleport
        NativeResult native = kOld;
        if (onLevel && running && local && local != outfitted && local != isaacSeen && necro::LocalRole() == 0 && respawn::stage == 0)
            native = NativeArrival(*a, local, now);
        if (state == State::Launching) {
            if (now - launchedAt > kLaunchTimeoutMs) { End("the level never ran within 180 s"); return; }
            if (!onLevel || !running || !local) { settledAt = 0; return; }
            if (!settledAt) settledAt = now;
            if (now - settledAt < 1000) return;
            SetState(State::Running);
            runningAt = lastPass = now;
            InterlockedIncrement(&arrivals);
            uint32_t death[4]{};
            Read(base + level::kDeathGuid, death, 16);
            float pos[3]{};
            Read(local + mpmap::kPlayerPos, pos, sizeof(pos));
            mpmap::Event("arena %s arrived after %lu ms: Isaac %08X at (%.2f %.2f %.2f), %s the arena; checkpoint %08X-%08X-%08X-%08X (%s)", a->name,
                         static_cast<unsigned long>(now - launchedAt), local, pos[0], pos[1], pos[2], coop_arena::Inside(*a, pos) ? "inside" : "OUTSIDE",
                         death[0], death[1], death[2], death[3], coop_arena::SameGuid(death, a->checkpoint) ? "the arena's"
                                                                 : !(death[0] | death[1] | death[2] | death[3]) ? "none stored: restart points are held"
                                                                 : "NOT the arena's");
            char text[96];
            std::snprintf(text, sizeof(text), "%s", a->label);
            for (char* c = text; *c; ++c) *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
            notice::Show(text);
        }
        // Running. The main menu or another level ends it; a death restart passes through.
        const bool inMenu = haveMap && _strnicmp(map, "frontend", 8) == 0;
        if (inMenu || (haveMap && !onLevel)) {
            if (!offMapSince) offMapSince = now;
            if (inMenu) mpmap::ClearOverride("arena over, back in the main menu");
            if (inMenu || now - offMapSince >= kOffMapMs) { End(inMenu ? "back in the main menu" : "another level"); return; }
            return;
        }
        offMapSince = 0;
        if (curtainUp && ((curtainLiftAt && now >= curtainLiftAt) || now - curtainSince > kStartLoadMs + kStartWaitMs + 7000))
            LiftStartCurtain(now, curtainLiftAt ? "on the start spot" : "the placement took too long");
        // A living necro's body is slot 0 too; it is no Isaac: no kit, no start spot (24.10.: the kit
        // failed on it and it was moved to spot 4).
        const bool isaac = necro::LocalRole() == 0;
        if (running && !isaac && local && local != outfitted) {
            outfitted = local;
            if (InterlockedIncrement(&necroSkips) <= 8)
                mpmap::Event("arena: slot 0 entity %08X is the necro body (role %d): no kit, no start spot", local, necro::LocalRole());
        }
        if (running && isaac && native != kWait) {
            RememberOwnSuit(local);
            Outfit(local, now);
            if (startWanted && startWanted != local) { startWanted = 0; startTarget = -1; }   // another Isaac by now
            if (startWanted) PlaceAtStart(*a, local, now);
            else NoGoPass(*a, local);
            CheckStart(local, now);
        }
        if (!running || now - lastPass < kPassMs) return;
        lastPass = now;
        float here[3]{};
        if (local && Read(local + mpmap::kPlayerPos, here, sizeof(here)) && coop_arena::Inside(*a, here)) {
            std::memcpy(lastInside, here, sizeof(lastInside));
            haveLastInside = true;
        }
        unsigned found = 0, locked = 0;
        DoorPass(*a, found, locked, now);
        ClipPass(*a, now);
        RoomPass(*a, now);
        if (resendAt && static_cast<LONG>(now - resendAt) >= 0) ResendHeld(*a, now);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&faults) <= 10) mpmap::Event("arena tick FAULTED");
    }
}

// --- for the survival director (exports in coop_plugin.cpp), any thread ---
int RunningName(char* out, int size) {
    if (!RunningNow()) return 0;
    const LONG index = InterlockedCompareExchange(&activeMirror, 0, 0);
    if (index < 0 || index >= static_cast<LONG>(coop_arena::kArenaCount)) return 0;
    if (out && size > 0) std::snprintf(out, static_cast<size_t>(size), "%s", coop_arena::kArenas[index].name);
    return 1;
}
int InsidePoint(const float* point) {
    const LONG index = InterlockedCompareExchange(&activeMirror, 0, 0);
    if (!point || !RunningNow() || index < 0 || index >= static_cast<LONG>(coop_arena::kArenaCount)) return 0;
    InterlockedIncrement(&insideAsked);
    const Arena& a = coop_arena::kArenas[index];
    bool inside = coop_arena::Inside(a, point) && !InNoGo(a, point);
    if (inside && coop_arena::BehindLockedDoor(a, coop_arena::PointsFor(a.name), point)) {
        inside = false;
        InterlockedIncrement(&insideBehindDoor);
    }
    // streaming=sets: only where the rooms are loaded (the sets the players stand in)
    if (inside && a.streaming) {
        const int s = coop_arena::SetForPoint(a, point);
        const uint32_t mask = static_cast<uint32_t>(InterlockedCompareExchange(&wantedMask, 0, 0));
        inside = s >= 0 && (mask >> s & 1u);
    }
    if (!inside) InterlockedIncrement(&insideNo);
    return inside ? 1 : 0;
}
int Spots(float* out, int max) {
    const LONG index = InterlockedCompareExchange(&activeMirror, 0, 0);
    if (!RunningNow() || index < 0 || index >= static_cast<LONG>(coop_arena::kArenaCount)) return 0;
    const Arena& a = coop_arena::kArenas[index];
    const int n = static_cast<int>(a.spotCount);
    for (int i = 0; out && i < n && i < max; ++i) std::memcpy(out + 3 * i, a.spots[i], 12);
    return n;
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Arena", "Arenas", 1, ini.c_str()) != 0;
    modesOn = GetPrivateProfileIntA("Arena", "Modes", 1, ini.c_str()) != 0;
    outfit = GetPrivateProfileIntA("Arena", "Kit", 1, ini.c_str()) != 0;
    startCurtain = GetPrivateProfileIntA("Arena", "StartCurtain", 1, ini.c_str()) != 0;
    blockHallucinations = GetPrivateProfileIntA("Arena", "BlockHallucinations", 1, ini.c_str()) != 0;
    openNpcClips = GetPrivateProfileIntA("Arena", "OpenNpcClips", 1, ini.c_str()) != 0;
    clipReachM = static_cast<float>(GetPrivateProfileIntA("Arena", "NpcClipReachM", 7, ini.c_str()));
    std::fprintf(log, "arena necro-only collision at usable doors %s, within %.0f m ([Arena] NpcClipReachM)\n",
                 openNpcClips ? "opened (bodies near a held or inner door lose the NPC filter bit)" : "left as the level has it ([Arena] OpenNpcClips=0)", clipReachM);
    unblockHeldDoors = GetPrivateProfileIntA("Arena", "UnblockHeldDoors", 1, ini.c_str()) != 0;
    unsigned char unblockSeen[sizeof(kUnblockGraphBytes)]{}, operateSeen[sizeof(kOperateGraphBytes)]{};
    unblockOk = Read(base + kUnblockGraphRva, unblockSeen, sizeof(unblockSeen)) && std::memcmp(unblockSeen, kUnblockGraphBytes, sizeof(unblockSeen)) == 0;
    operateOk = Read(base + kOperateGraphRva, operateSeen, sizeof(operateSeen)) && std::memcmp(operateSeen, kOperateGraphBytes, sizeof(operateSeen)) == 0;
    std::fprintf(log, "arena sig %-28s %08X %s\n", "StandardDoor::UnblockGraph", static_cast<unsigned>(base + kUnblockGraphRva), unblockOk ? "ok" : "MISMATCH");
    std::fprintf(log, "arena sig %-28s %08X %s\n", "OperateOnGraphConnections", static_cast<unsigned>(base + kOperateGraphRva), operateOk ? "ok" : "MISMATCH");
    std::fprintf(log, "arena held doors and the nav graph: %s\n", unblockHeldDoors ? "a door open 5/5 for two passes with its obstacle live or links cut is unblocked"
                                                                                  : "only logged ([Arena] UnblockHeldDoors=0)");
    char text[64]{};
    GetPrivateProfileStringA("Arena", "Suit", "", text, sizeof(text), ini.c_str());
    uint32_t parsed[4]{};
    if (text[0] && coop_mpmap::ParseGuid(text, parsed)) std::memcpy(suit, parsed, sizeof(suit));
    else if (text[0]) std::fprintf(log, "arena Suit=%s is no GUID; the Engineering Suit it is\n", text);
    std::fprintf(log, "arena kit %s: suit %08X-%08X-%08X-%08X instead of the patient suit, plasma cutter, stasis and kinesis\n", outfit ? "on" : "off ([Arena] Kit=0)",
                 suit[0], suit[1], suit[2], suit[3]);
    std::fprintf(log, "arena start curtain %s\n", startCurtain ? "on: black from the new Isaac's first frame until he stands on his start spot" : "off ([Arena] StartCurtain=0)");
    unsigned char seen[sizeof(kSetIsLockedBytes)]{};
    setIsLockedOk = Read(base + kSetIsLockedRva, seen, sizeof(seen)) && std::memcmp(seen, kSetIsLockedBytes, sizeof(seen)) == 0;
    std::fprintf(log, "arena sig %-28s %08X %s\n", "StandardDoor::SetIsLocked", static_cast<unsigned>(base + kSetIsLockedRva), setIsLockedOk ? "ok" : "MISMATCH");
    unsigned char msgSeen[sizeof(kSendDoorMsgBytes)]{};
    sendDoorMsgOk = Read(base + kSendDoorMsgRva, msgSeen, sizeof(msgSeen)) && std::memcmp(msgSeen, kSendDoorMsgBytes, sizeof(msgSeen)) == 0;
    std::fprintf(log, "arena sig %-28s %08X %s\n", "StandardDoor::SendDoorMsg", static_cast<unsigned>(base + kSendDoorMsgRva),
                 sendDoorMsgOk ? "ok" : "MISMATCH (held doors send no open-start messages)");
    std::fprintf(log, "arena %s: %u arena(s) in the picker; set switch hook %s, hide hook %s\n", enabled ? "on" : "off ([Arena] Arenas=0)", coop_arena::kArenaCount,
                 streams::pin::hooked ? "installed" : "NOT installed", streams::vis::hooked ? "installed" : "NOT installed");
    for (unsigned i = 0; enabled && i < coop_arena::kArenaCount; ++i) {
        const Arena& a = coop_arena::kArenas[i];
        unsigned locks = 0;
        for (unsigned d = 0; d < a.doorCount; ++d) locks += a.doors[d].lock ? 1u : 0u;
        unsigned create = 0;
        for (unsigned b = 0; b < a.blockCount; ++b) create += a.blocks[b].kind == 1 ? 1u : 0u;
        std::fprintf(log, "arena %s \"%s\": %u sets, %u rooms, %u doors to lock, %u inner%s, %u spots, story logic %u not created and %u refused at the filter; "
                          "families left out: %s\n", a.name, a.label, a.setCount, a.memberCount, locks, a.doorCount - locks, a.doorsOpen ? " (held open)" : "", a.spotCount,
                     create, a.blockCount - create, a.exclude && a.exclude[0] ? a.exclude : "none");
        const coop_arena::ArenaPoints* p = coop_arena::PointsFor(a.name);
        std::fprintf(log, "arena %s modes %s: plays%s%s%s; %u zone, %u capsule, %u respawn points\n", a.name, modesOn ? "on" : "off ([Arena] Modes=0, survival only)",
                     " survival", p && (p->modes & 2u) ? ", mercenaries" : "", p && (p->modes & 8u) ? ", hold the zone" : "", p ? p->zoneCount : 0u,
                     p ? p->capsuleCount : 0u, p ? p->respawnCount : 0u);
    }
}
// Only when something moved: the line used to repeat every few seconds in the menu.
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    const LONG values[] = {static_cast<LONG>(state), InterlockedCompareExchange(&starts, 0, 0), InterlockedCompareExchange(&refused, 0, 0),
                           InterlockedCompareExchange(&arrivals, 0, 0), InterlockedCompareExchange(&ends, 0, 0), InterlockedCompareExchange(&merges, 0, 0),
                           InterlockedCompareExchange(&added, 0, 0), InterlockedCompareExchange(&unhidden, 0, 0), InterlockedCompareExchange(&full, 0, 0),
                           InterlockedCompareExchange(&doorLocks, 0, 0), InterlockedCompareExchange(&doorRelocks, 0, 0), InterlockedCompareExchange(&doorFaults, 0, 0),
                           InterlockedCompareExchange(&doorWrongClass, 0, 0), InterlockedCompareExchange(&shows, 0, 0),
                           InterlockedCompareExchange(&streams::vis::arenaHidesRefused, 0, 0), InterlockedCompareExchange(&notCreated, 0, 0),
                           InterlockedCompareExchange(&filterRefused, 0, 0), InterlockedCompareExchange(&outfits, 0, 0), InterlockedCompareExchange(&suitChanges, 0, 0),
                           InterlockedCompareExchange(&insideAsked, 0, 0), InterlockedCompareExchange(&insideNo, 0, 0), InterlockedCompareExchange(&faults, 0, 0),
                           InterlockedCompareExchange(&readyArrivals, 0, 0), InterlockedCompareExchange(&respawnedBodies, 0, 0),
                           InterlockedCompareExchange(&respawnPicks, 0, 0), InterlockedCompareExchange(&doorUnlocks, 0, 0),
                           InterlockedCompareExchange(&doorHolds, 0, 0), InterlockedCompareExchange(&liveMarksSet, 0, 0),
                           InterlockedCompareExchange(&wantedMask, 0, 0), InterlockedCompareExchange(&setActivations, 0, 0),
                           InterlockedCompareExchange(&closesRefused, 0, 0), InterlockedCompareExchange(&startPlacements, 0, 0),
                           InterlockedCompareExchange(&startPlaceFailures, 0, 0), InterlockedCompareExchange(&doorMessages, 0, 0),
                           InterlockedCompareExchange(&doorResends, 0, 0), InterlockedCompareExchange(&startRandom, 0, 0),
                           InterlockedCompareExchange(&startBesidePartner, 0, 0), InterlockedCompareExchange(&startWaits, 0, 0),
                           InterlockedCompareExchange(&insideBehindDoor, 0, 0)};
    static LONG lastNav[5] = {-1};
    const LONG nav[5] = {InterlockedCompareExchange(&doorNavStale, 0, 0), InterlockedCompareExchange(&doorNavFixes, 0, 0),
                         InterlockedCompareExchange(&doorNavFixFailed, 0, 0), InterlockedCompareExchange(&doorNavNoComponent, 0, 0),
                         InterlockedCompareExchange(&doorNavPasses, 0, 0) > 0 ? 1 : 0};
    if (std::memcmp(lastNav, nav, sizeof(nav)) != 0) {
        std::memcpy(lastNav, nav, sizeof(nav));
        std::fprintf(log, "arena door_nav unblock=%d sigs=%d/%d held_checks=%ld open_but_blocked=%ld unblocked=%ld unblock_failed=%ld no_component=%ld\n",
                     unblockHeldDoors ? 1 : 0, unblockOk ? 1 : 0, operateOk ? 1 : 0, InterlockedCompareExchange(&doorNavPasses, 0, 0), nav[0], nav[1], nav[2], nav[3]);
    }
    static LONG lastClips[8] = {-1};
    const LONG clips[8] = {InterlockedCompareExchange(&clipPasses, 0, 0), InterlockedCompareExchange(&clipWalked, 0, 0), InterlockedCompareExchange(&clipNpcOnly, 0, 0),
                           InterlockedCompareExchange(&clipsOpened, 0, 0), InterlockedCompareExchange(&clipsReopened, 0, 0), InterlockedCompareExchange(&clipsShared, 0, 0),
                           InterlockedCompareExchange(&clipsAway, 0, 0), InterlockedCompareExchange(&clipFaults, 0, 0)};
    if (std::memcmp(lastClips + 3, clips + 3, sizeof(LONG) * 5) != 0 || lastClips[0] < 0) {
        std::memcpy(lastClips, clips, sizeof(clips));
        std::fprintf(log, "arena npc_clips on=%d passes=%ld bodies_walked=%ld npc_only_now=%ld opened=%ld reopened=%ld shared_skipped=%ld away_from_doors=%ld faults=%ld\n",
                     openNpcClips ? 1 : 0, clips[0], clips[1], clips[2], clips[3], clips[4], clips[5], clips[6], clips[7]);
    }
    static LONG last[sizeof(values) / sizeof(values[0])] = {-1};
    if (std::memcmp(last, values, sizeof(values)) == 0) return;
    const LONG index = InterlockedCompareExchange(&activeMirror, 0, 0);
    const char* activeName = index >= 0 && index < static_cast<LONG>(coop_arena::kArenaCount) ? coop_arena::kArenas[index].name : "-";
    std::memcpy(last, values, sizeof(values));
    static LONG lastBlocked = -1;
    if (const LONG blocked = InterlockedCompareExchange(&hallucinationsBlocked, 0, 0); blocked != lastBlocked) {
        lastBlocked = blocked;
        std::fprintf(log, "arena hallucinations block=%d blocked=%ld\n", blockHallucinations ? 1 : 0, blocked);
    }
    std::fprintf(log, "arena state=%ld active=%s starts=%ld refused=%ld arrivals=%ld ends=%ld merges=%ld added=%ld unhidden=%ld full=%ld door_locks=%ld door_relocks=%ld "
                      "door_faults=%ld door_wrong_class=%ld shows=%ld hides_refused=%ld not_created=%ld filter_refused=%ld kits=%ld suit_changes=%ld inside_asked=%ld "
                      "inside_no=%ld (behind a locked door %ld) faults=%ld | new_isaacs=%ld start_placements=%ld (random %ld, beside a partner %ld, waits %ld) start_place_failures=%ld respawned_bodies=%ld respawn_spots=%ld | door_unlocks=%ld door_holds=%ld door_closes_refused=%ld door_messages=%ld door_resends=%ld door_marks_live=%ld | sets_wanted=%08lX set_activations=%ld\n",
                 values[0], activeName, values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8], values[9], values[10],
                 values[11], values[12], values[13], values[14], values[15], values[16], values[17], values[18], values[19], values[20], values[38], values[21], values[22],
                 values[31], values[35], values[36], values[37], values[32], values[23], values[24], values[25], values[26], values[30], values[33], values[34], values[27], values[28], values[29]);
    std::fprintf(log, "arena own_suits_back=%ld (respawned bodies dressed in the suit worn alive) necro_bodies_skipped=%ld remembered_suit=%s\n",
                 InterlockedCompareExchange(&ownSuitsBack, 0, 0), InterlockedCompareExchange(&necroSkips, 0, 0),
                 haveOwnSuit && bot::SuitOfGuid(ownSuit) >= 0 ? bot::SuitName(bot::SuitOfGuid(ownSuit)) : "none");
    std::fprintf(log, "arena native_start starts=%ld placement_misses=%ld suit_misses=%ld suit_fallback_curtains=%ld old_start_curtains=%ld armed=%d/%d "
                      "restart_waits=%ld reload_timeouts=%ld needs_reload=%d last_why_not='%s'\n",
                 InterlockedCompareExchange(&nativeStarts, 0, 0), InterlockedCompareExchange(&nativePlacementMisses, 0, 0),
                 InterlockedCompareExchange(&nativeSuitMisses, 0, 0), InterlockedCompareExchange(&suitFallbackCurtains, 0, 0),
                 InterlockedCompareExchange(&oldStartCurtains, 0, 0), nativeArmed ? 1 : 0, nativeBirthArmed ? 1 : 0, InterlockedCompareExchange(&nativeWaits, 0, 0),
                 InterlockedCompareExchange(&nativeReloadTimeouts, 0, 0), nativeNeedsReload ? 1 : 0, nativeWhyNot);
}
}  // namespace arena
