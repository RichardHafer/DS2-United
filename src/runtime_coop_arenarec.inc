// Dead Space 2 United by Hafer.
// Arena recorder ([Arena] in DS2United.ini). RecordKey starts a recording and the
// next press saves it; MarkKey marks the spot the player stands on. A campaign
// arena is built from what a walk through its rooms shows:
// - every stream set activation (StreamSet::Activate, through
//   streams::recordActivation): the set, its member streams, who asked (door,
//   lift, script) and the entity that asked (a door's GUID);
// - every completed area (streams::area), which is the room the player is in;
// - the player's position once a second while moving, and the marked spots.
// Each event is appended at once to plugins\DS2Arenas\<map>_<date>_<time>.txt,
// so a crash keeps what was walked; the summary at the end lists the union of
// the member streams and the doors used.
// DoorKey marks the door nearest to the player (8 m): each press steps auto, inner
// (never locked), locked, held (kept open). During a recording the mark goes into
// it; in a running arena it works at once and goes to plugins\DS2Arenas\doors\
// <arena>.txt (both read by tools/build_arena_points.py, last mark wins).
namespace arenarec {
bool enabled = true;
int recordKey = VK_HOME, markKey = VK_END, doorKey = VK_DELETE;
bool doorDown = false;
constexpr float kDoorReach = 8.0f;
struct DoorMarkMade { uint32_t guid[4]; int state; };
DoorMarkMade recordedMarks[64]{};
unsigned recordedMarkCount = 0;
volatile LONG doorMarks = 0, doorMarkMisses = 0;
std::string folder;
FILE* out = nullptr;
bool recording = false, recordDown = false, markDown = false;
DWORD startedAt = 0, lastSample = 0;
float lastPos[3]{};
bool havePos = false;
char recMap[64] = "";
uint32_t lastArea[4]{};
constexpr unsigned kMaxSets = 64, kMaxMembers = 512, kMaxDoors = 64;
uint32_t sets[kMaxSets][4]{}, setMembers[kMaxSets] = {};
unsigned setCount = 0;
uint32_t members[kMaxMembers][4]{};
unsigned memberCount = 0;
uint32_t doors[kMaxDoors][4]{}, doorVtables[kMaxDoors] = {};
unsigned doorCount = 0, spots = 0, samples = 0, activationsSeen = 0;
volatile LONG recordings = 0;

void Guid(char* buffer, size_t size, const uint32_t* g) { std::snprintf(buffer, size, "%08X-%08X-%08X-%08X", g[0], g[1], g[2], g[3]); }
bool Zero(const uint32_t* g) { return !(g[0] | g[1] | g[2] | g[3]); }
bool Same(const uint32_t* a, const uint32_t* b) { return std::memcmp(a, b, 16) == 0; }
float Seconds() { return (GetTickCount() - startedAt) / 1000.0f; }
void Line(const char* format, ...) {
    if (!out) return;
    va_list args;
    va_start(args, format);
    std::vfprintf(out, format, args);
    va_end(args);
    std::fputc('\n', out);
    std::fflush(out);
}
bool PlayerPos(float pos[3]) {
    const uint32_t player = LocalSlotZeroEntity();
    return player && Read(player + mpmap::kPlayerPos, pos, 12) && std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]);
}
void AddMember(const uint32_t* g) {
    for (unsigned i = 0; i < memberCount; ++i) if (Same(members[i], g)) return;
    if (memberCount < kMaxMembers) std::memcpy(members[memberCount++], g, 16);
}

// Game thread, from StreamSet::Activate (coop_streams).
void OnActivation(uint32_t node, uint32_t actor, uint32_t kind) {
    if (!recording) return;
    ++activationsSeen;
    uint32_t guid[4]{};
    Read(node + streams::kNodeGuid, guid, 16);
    uint32_t list[streams::kMaxMembers][4]{};
    const unsigned n = streams::Members(node, list);
    uint32_t actorGuid[4]{}, actorVtable = 0;
    if (actor) { Read(actor, &actorVtable, 4); Read(actor + 0x24, actorGuid, 16); }
    char g[40], a[40];
    Guid(g, sizeof(g), guid);
    Guid(a, sizeof(a), actorGuid);
    float pos[3]{};
    const bool havePlayer = PlayerPos(pos);
    std::string text;
    for (unsigned i = 0; i < n; ++i) {
        char m[40];
        Guid(m, sizeof(m), list[i]);
        text += i ? " " : "";
        text += m;
        AddMember(list[i]);
    }
    bool known = false;
    for (unsigned i = 0; i < setCount; ++i) if (Same(sets[i], guid)) known = true;
    if (!known && setCount < kMaxSets && !Zero(guid)) { std::memcpy(sets[setCount], guid, 16); setMembers[setCount++] = n; }
    if (kind == streams::kKindDoor && actor && !Zero(actorGuid)) {
        bool seen = false;
        for (unsigned i = 0; i < doorCount; ++i) if (Same(doors[i], actorGuid)) seen = true;
        if (!seen && doorCount < kMaxDoors) { std::memcpy(doors[doorCount], actorGuid, 16); doorVtables[doorCount++] = actorVtable; }
    }
    Line("t=%.1f activate set=%s kind=%s actor=%s actor_vtable=%08X player=(%.2f %.2f %.2f)%s members=%u %s", Seconds(), g, streams::KindName(kind), a, actorVtable,
         pos[0], pos[1], pos[2], havePlayer ? "" : " (no player)", n, text.c_str());
}

// The StandardDoor nearest to the player within kDoorReach (a storey counts three times).
struct NearDoor { uint32_t entity = 0; uint32_t guid[4]{}; float pos[3]{}; float distance = 0.0f; };
bool FindNearestDoor(NearDoor& nearest) {
    float self[3]{};
    if (!PlayerPos(self)) return false;
    uint32_t node = 0;
    if (!Read(base + bot_sense::kReflectableListRva, &node, 4)) return false;
    float best = kDoorReach * kDoorReach;
    bool found = false;
    for (unsigned steps = 0; node && steps < 8192; ++steps) {
        const uint32_t entity = node - bot_sense::kEntityReflectable;
        uint32_t next = 0, vtable = 0;
        const bool more = Read(node + 4, &next, 4) && next != node;
        float at[3]{};
        if (Read(entity, &vtable, 4) && vtable == base + worldcalls::kStandardDoorVtableRva && Read(entity + 0xC0, at, 12) && std::isfinite(at[0]) &&
            std::isfinite(at[1]) && std::isfinite(at[2])) {
            const float dx = at[0] - self[0], dy = (at[1] - self[1]) * 3.0f, dz = at[2] - self[2];
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < best) {
                best = d;
                nearest.entity = entity;
                std::memcpy(nearest.pos, at, 12);
                found = Read(entity + 0x24, nearest.guid, 16) && !Zero(nearest.guid);
            }
        }
        if (!more) break;
        node = next;
    }
    if (found) nearest.distance = std::sqrt(best);
    return found;
}
int RecordedMark(const uint32_t* guid) {
    for (unsigned i = 0; i < recordedMarkCount; ++i) if (Same(recordedMarks[i].guid, guid)) return recordedMarks[i].state;
    return -1;
}
void NoteRecordedMark(const uint32_t* guid, int state) {
    for (unsigned i = 0; i < recordedMarkCount; ++i) if (Same(recordedMarks[i].guid, guid)) { recordedMarks[i].state = state; return; }
    if (recordedMarkCount < 64) { std::memcpy(recordedMarks[recordedMarkCount].guid, guid, 16); recordedMarks[recordedMarkCount++].state = state; }
}
const char* MarkText(int state) {
    switch (state) {
        case coop_arena::kDoorInner: return "OPEN (NEVER LOCKED)";
        case coop_arena::kDoorLocked: return "FORCED LOCKED";
        case coop_arena::kDoorHeld: return "HELD OPEN";
        default: return "AUTO (THE ARENA BUILD DECIDES)";
    }
}
// The door key: the nearest door's mark one step on.
void MarkDoor() {
    char arenaName[64] = "";
    const bool inArena = arena::RunningName(arenaName, sizeof(arenaName)) == 1;
    if (!recording && !inArena) {
        notice::Show("DOOR MARKS: START A RECORDING OR AN ARENA FIRST");
        return;
    }
    NearDoor d;
    if (!FindNearestDoor(d)) {
        InterlockedIncrement(&doorMarkMisses);
        notice::Show("NO DOOR WITHIN 8 M");
        return;
    }
    const int recorded = recording ? RecordedMark(d.guid) : -1;
    const int current = recorded >= 0 ? recorded : inArena ? arena::MarkOf(d.guid) : coop_arena::kDoorAuto;
    const int next = coop_arena::NextDoorState(current);
    char g[40];
    Guid(g, sizeof(g), d.guid);
    if (recording) {
        NoteRecordedMark(d.guid, next);
        Line("t=%.1f doormark %s state=%s at=(%.2f %.2f %.2f) dist=%.1f", Seconds(), g, coop_arena::DoorStateName(next), d.pos[0], d.pos[1], d.pos[2], d.distance);
    }
    if (inArena) {
        arena::SetLiveMark(d.guid, next);
        const std::string doorFolder = folder + "\\doors";
        CreateDirectoryA(folder.c_str(), nullptr);
        CreateDirectoryA(doorFolder.c_str(), nullptr);
        const std::string path = doorFolder + "\\" + arenaName + ".txt";
        FILE* file = nullptr;
        if (fopen_s(&file, path.c_str(), "a") == 0 && file) {
            std::fprintf(file, "doormark %s state=%s at=(%.2f %.2f %.2f)\n", g, coop_arena::DoorStateName(next), d.pos[0], d.pos[1], d.pos[2]);
            std::fclose(file);
        } else {
            mpmap::Event("arenarec: door mark NOT written to %s", path.c_str());
        }
    }
    InterlockedIncrement(&doorMarks);
    char text[96];
    std::snprintf(text, sizeof(text), "DOOR %08X: %s (%.1f M)", d.guid[0], MarkText(next), d.distance);
    notice::Show(text);
    mpmap::Event("arenarec: door %s at (%.1f %.1f %.1f), %.1f m away: %s -> %s (%s%s)", g, d.pos[0], d.pos[1], d.pos[2], d.distance,
                 coop_arena::DoorStateName(current), coop_arena::DoorStateName(next), recording ? "recording" : "", inArena ? (recording ? ", arena" : "arena, works now") : "");
}

void Start(DWORD now) {
    char map[64] = "", stream[64] = "";
    if (!level::CurrentMap(map, sizeof(map), stream, sizeof(stream)) || !map[0] || _strnicmp(map, "frontend", 8) == 0) {
        notice::Show("ARENA RECORDER: NOT IN A LEVEL");
        return;
    }
    SYSTEMTIME t{};
    GetLocalTime(&t);
    CreateDirectoryA(folder.c_str(), nullptr);
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\%s_%04u%02u%02u_%02u%02u%02u.txt", folder.c_str(), map, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (fopen_s(&out, path, "w") != 0 || !out) {
        out = nullptr;
        notice::Show("ARENA RECORDER: FILE NOT WRITABLE");
        mpmap::Event("arenarec: could not open %s", path);
        return;
    }
    recording = true;
    startedAt = lastSample = now;
    havePos = false;
    setCount = memberCount = doorCount = spots = samples = activationsSeen = recordedMarkCount = 0;
    std::memset(lastArea, 0, sizeof(lastArea));
    std::snprintf(recMap, sizeof(recMap), "%s", map);
    uint32_t checkpoint[4]{};
    Read(base + level::kDeathGuid, checkpoint, 16);
    char cp[40], area[40];
    Guid(cp, sizeof(cp), checkpoint);
    Guid(area, sizeof(area), streams::area);
    float pos[3]{};
    PlayerPos(pos);
    Line("# Dead Space 2 arena recording");
    Line("map=%s", map);
    Line("stream=%s", stream);
    Line("started=%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    Line("checkpoint=%s", cp);
    Line("area_at_start=%s asked_by=%s", area, streams::KindName(streams::areaKind));
    Line("player_at_start=(%.2f %.2f %.2f)", pos[0], pos[1], pos[2]);
    Line("# events");
    InterlockedIncrement(&recordings);
    notice::Show("ARENA RECORDING ON: WALK THE ROOMS, END MARKS A SPOT, DEL MARKS A DOOR, HOME SAVES");
    mpmap::Event("arenarec: recording to %s (map %s, checkpoint %s)", path, map, cp);
}

void Stop(const char* why) {
    if (!recording) return;
    recording = false;
    Line("# summary (%s) after %.0f s: %u sets, %u member streams, %u doors used, %u spots, %u samples, %u activations", why, Seconds(), setCount, memberCount,
         doorCount, spots, samples, activationsSeen);
    for (unsigned i = 0; i < setCount; ++i) {
        char g[40];
        Guid(g, sizeof(g), sets[i]);
        Line("set=%s members=%u", g, setMembers[i]);
    }
    for (unsigned i = 0; i < memberCount; ++i) {
        char g[40];
        Guid(g, sizeof(g), members[i]);
        Line("member=%s", g);
    }
    for (unsigned i = 0; i < doorCount; ++i) {
        char g[40];
        Guid(g, sizeof(g), doors[i]);
        Line("door=%s vtable=%08X", g, doorVtables[i]);
    }
    if (out) { std::fclose(out); out = nullptr; }
    char text[96];
    std::snprintf(text, sizeof(text), "ARENA SAVED: %u ROOM SETS, %u SPOTS", setCount, spots);
    notice::Show(text);
    mpmap::Event("arenarec: saved (%s): %u sets, %u members, %u doors, %u spots", why, setCount, memberCount, doorCount, spots);
}

// Game thread, from coop::Poll.
void Tick(DWORD now) {
    if (!enabled) return;
    const bool focused = necro::GameWindowFocused();
    const bool rec = recordKey && focused && (GetAsyncKeyState(recordKey) & 0x8000) != 0;
    const bool mark = markKey && focused && (GetAsyncKeyState(markKey) & 0x8000) != 0;
    const bool doorPressed = doorKey && focused && (GetAsyncKeyState(doorKey) & 0x8000) != 0;
    if (rec && !recordDown) { if (recording) Stop("key"); else Start(now); }
    recordDown = rec;
    if (doorPressed && !doorDown && inLevel) MarkDoor();
    doorDown = doorPressed;
    if (!recording) { markDown = mark; return; }
    char map[64] = "", stream[64] = "";
    if (!level::CurrentMap(map, sizeof(map), stream, sizeof(stream)) || _stricmp(map, recMap) != 0) { Stop("level changed"); markDown = mark; return; }
    if (!Zero(streams::area) && !Same(streams::area, lastArea)) {
        std::memcpy(lastArea, streams::area, 16);
        char g[40];
        Guid(g, sizeof(g), lastArea);
        float pos[3]{};
        PlayerPos(pos);
        Line("t=%.1f area %s asked_by=%s player=(%.2f %.2f %.2f)", Seconds(), g, streams::KindName(streams::areaKind), pos[0], pos[1], pos[2]);
    }
    float pos[3]{};
    const bool have = PlayerPos(pos);
    if (mark && !markDown && have) {
        ++spots;
        char g[40];
        Guid(g, sizeof(g), streams::area);
        Line("t=%.1f spot %u at (%.2f %.2f %.2f) area=%s", Seconds(), spots, pos[0], pos[1], pos[2], g);
        char text[64];
        std::snprintf(text, sizeof(text), "ARENA SPOT %u MARKED", spots);
        notice::Show(text);
    }
    markDown = mark;
    if (have && now - lastSample >= 1000) {
        lastSample = now;
        const float dx = pos[0] - lastPos[0], dy = pos[1] - lastPos[1], dz = pos[2] - lastPos[2];
        if (!havePos || dx * dx + dy * dy + dz * dz >= 1.0f) {
            havePos = true;
            std::memcpy(lastPos, pos, sizeof(lastPos));
            ++samples;
            Line("t=%.1f pos (%.2f %.2f %.2f)", Seconds(), pos[0], pos[1], pos[2]);
        }
    }
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Arena", "Recorder", 1, ini.c_str()) != 0;
    recordKey = GetPrivateProfileIntA("Arena", "RecordKey", VK_HOME, ini.c_str());
    markKey = GetPrivateProfileIntA("Arena", "MarkKey", VK_END, ini.c_str());
    doorKey = GetPrivateProfileIntA("Arena", "DoorKey", VK_DELETE, ini.c_str());
    if (doorKey < 0 || doorKey > 255) doorKey = 0;
    if (recordKey < 0 || recordKey > 255) recordKey = 0;
    if (markKey < 0 || markKey > 255) markKey = 0;
    const size_t slash = ini.find_last_of("\\/");
    folder = (slash == std::string::npos ? std::string(".") : ini.substr(0, slash)) + "\\DS2Arenas";
    if (enabled) streams::recordActivation = OnActivation;
    std::fprintf(log, "arenarec %s: record key %d, mark key %d, door key %d, files in %s%s\n", enabled ? "on" : "off ([Arena] Recorder=0)", recordKey, markKey, doorKey,
                 folder.c_str(),
                 enabled && !streams::installed ? " (coop_streams is off: no set activations, areas and positions only)" : "");
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    std::fprintf(log, "arenarec recording=%d recordings=%ld sets=%u members=%u doors=%u spots=%u samples=%u door_marks=%ld door_misses=%ld\n", recording ? 1 : 0,
                 InterlockedCompareExchange(&recordings, 0, 0), setCount, memberCount, doorCount, spots, samples, InterlockedCompareExchange(&doorMarks, 0, 0),
                 InterlockedCompareExchange(&doorMarkMisses, 0, 0));
}
}  // namespace arenarec
