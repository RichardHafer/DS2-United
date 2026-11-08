// Dead Space 2 United by Richard Hafer.
// Hold the Zone on this machine ([Zone] in DS2United.ini shows it, DS2Survival.ini
// [Zone] plays it). The survival director says where the zone is and how it
// stands through DS2Coop_SurvivalZone; a host passes that on to its partners
// (survnet kind 7, on a change and once a second), a partner takes it from there.
// Each machine then shows it for its own Isaac, on the game thread:
//   - a ring of resident glows on the floor at the radius, created the way the
//     gadgets' rings are (turret::CreateRaw), shimmering while it is contested;
//   - the locator: an entry in the ObjectiveManager ([0x02021430], a service, so
//     on every map) the way ObjectiveEntry::AddObjectivesToMap (0x010CF370) adds
//     one: AddObjective 0x010C0940, thiscall(id, sub, pos*, a*, b*, priority,
//     actual priority, make current), ret 0x20. It keeps the pointers, so they
//     point at our own storage. The locator's objective mode (PlayerNavHelpSM
//     GetSearchEndPositions 0x010A1980, mode byte +0x4E0 = 0; 1..3 are the
//     store, bench and save breadcrumbs) reads the current entry. AddObjective with
//     the same id only makes it current again, which is repeated once a second
//     (the map's own ObjectiveEntries add theirs; a respawn cycles the current
//     one). RemoveObjective 0x010A4460, thiscall(id, sub), ret 8;
//   - the overlay: "ZONE 45 M AHEAD" or "IN THE ZONE" after the director's line.
// It also records where zones go: RecordKey saves the spot you stand on into
// DS2Zones.txt, DeleteKey removes the nearest one; the director places zones only
// at recorded spots.
namespace arena { int RunningName(char* out, int size); }   // runtime_coop_arena.inc
namespace zone {
using coop_gadgets::V3;
bool enabled = true, ringWanted = true, locatorWanted = true, overlayWanted = true;
int ringPoints = 16;
// The floor glows under the pickups (global_assets, resident on every map), by
// name. Their colour is the effect's own colour key (pickup_stasis.vfb: 0.04
// 0.5 0.7); VFXHandle::SetTint (0x0069E080, thiscall(const float(&)[4]), ret 4,
// RGB at instance+0xA0) multiplies it, so a tint darkens channels but cannot add
// one: blue cannot turn red, cyan can turn green or blue.
struct Glow { const char* name; uint32_t guid[4]; };
const Glow kGlows[] = {
    {"stasis", {0x1B884B0E, 0x1B884B0E, 0x1B884B0E, 0xA18B4273}},
    {"node", {0x1E8DD9C8, 0x1E8DD9C8, 0x1E8DD9C8, 0x2384687E}},
    {"plasma_ammo", {0x9B167535, 0x9B167535, 0x9B167535, 0x80450BC2}},
    {"rivet_ammo", {0x89BC72A6, 0x89BC72A6, 0x89BC72A6, 0xC5EC0A1D}},
    {"treasure", {0x4CE076C4, 0x4CE076C4, 0x4CE076C4, 0x2F7F93ED}},
    {"health", {0xA3791C0E, 0xA3791C0E, 0xA3791C0E, 0xC849DE8D}},
    {"health_multi", {0xE065F567, 0xE065F567, 0xE065F567, 0x705FF074}},
    {"air", {0x85D8108E, 0x85D8108E, 0x85D8108E, 0xC7FCBD0F}},
    {"schematic", {0x59724357, 0x59724357, 0x59724357, 0x75BA0B12}},
    {"credits", {0x4DAA55C3, 0x4DAA55C3, 0x4DAA55C3, 0xE3ECF7CC}},
    {"pulse_ammo", {0xCD63A302, 0xCD63A302, 0xCD63A302, 0xB0D1AD2A}},
    {"ripper_ammo", {0x09F59E03, 0x09F59E03, 0x09F59E03, 0xD336E196}},
    {"flamer_ammo", {0x141C3E39, 0x141C3E39, 0x141C3E39, 0xF62B543E}},
    {"line_ammo", {0x9268CFEF, 0x9268CFEF, 0x9268CFEF, 0x71C5FC1F}},
    {"javelin_ammo", {0x01DC826B, 0x01DC826B, 0x01DC826B, 0x99413D79}},
    {"detonator_ammo", {0x8C02A028, 0x8C02A028, 0x8C02A028, 0x4593B990}},
    {"seeker_ammo", {0xB929FB84, 0xB929FB84, 0xB929FB84, 0x67689520}},
    {"force_ammo", {0x5FFC7AC8, 0x5FFC7AC8, 0x5FFC7AC8, 0x94708DA1}},
    {"contact_ammo", {0x9F8CA954, 0x9F8CA954, 0x9F8CA954, 0x2EC6C13C}},
    {"log", {0xAD856C1E, 0xAD856C1E, 0xAD856C1E, 0x528C4021}},
    {"keycard", {0xB3021A74, 0xB3021A74, 0xB3021A74, 0x8E512D1D}},
};
int glowIndex = 0;   // kGlows entry in use, -1 a GUID from the INI
uint32_t ringGuid[4] = {0x1B884B0E, 0x1B884B0E, 0x1B884B0E, 0xA18B4273};
const uint32_t kRingFallback[4] = {0xC91BAD4C, 0xC91BAD4C, 0xC91BAD4C, 0x107BC2CC};   // pickup_ui_stasis_pack_sm
alignas(16) float ringTint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
bool tinted = false;
constexpr uint32_t kSetTintRva = 0x29E080;
std::string iniFile;
constexpr uint32_t kManagerRva = 0x1C21430, kAddRva = 0xCC0940, kRemoveRva = 0xCA4460;
constexpr uint32_t kObjectiveId = 0x454E4F5A;   // 'ZONE'
constexpr DWORD kLocalFreshMs = 3000, kRemoteFreshMs = 6000, kSendMs = 1000, kReassertMs = 1000, kRefillMs = 500, kPulseMs = 150;

// --- what to show ---
struct Desired { int state = 0; uint32_t id = 0; float pos[3]{}; float radius = 0.0f; int percent = 0; DWORD at = 0; };
SRWLOCK lock = SRWLOCK_INIT;
Desired local, remote;
volatile LONG localSets = 0, remoteIn = 0, sent = 0;
// Any thread (the director's).
void SetLocal(int state, uint32_t id, const float* pos, float radius, int percent) {
    AcquireSRWLockExclusive(&lock);
    local.state = pos || !state ? state : 0;
    local.id = id;
    if (pos) std::memcpy(local.pos, pos, sizeof(local.pos));
    local.radius = radius;
    local.percent = percent;
    const DWORD t = GetTickCount();
    local.at = t ? t : 1;
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement(&localSets);
}
// Game thread, from survnet.
void SetRemote(const coop_zone::Wire& w) {
    AcquireSRWLockExclusive(&lock);
    remote.state = w.state < coop_zone::kStates ? w.state : 0;
    remote.id = w.id;
    std::memcpy(remote.pos, w.pos, sizeof(remote.pos));
    remote.radius = w.radius;
    remote.percent = w.percent;
    const DWORD t = GetTickCount();
    remote.at = t ? t : 1;
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement(&remoteIn);
}

// --- the ring ---
struct Slot { uint32_t handle = 0; bool held = false, skip = false; DWORD retryAt = 0; V3 at; };
Slot slots[coop_gadgets::kRingMaxPoints];
int ringN = 0, pulseNext = 0;
const uint32_t* ringEffect = nullptr;
DWORD chooseAt = 0, pulseAt = 0;
bool ringUpLogged = false;
volatile LONG ringBuilds = 0, ringCreated = 0, ringEnded = 0, ringAbsent = 0, ringFaults = 0, ringWalls = 0, ringDrops = 0, ringPulses = 0;
unsigned lineCount = 0;
void Line(const char* format, ...) {
    if (++lineCount > 60) return;
    char buffer[240];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    gate::Event("zone: %s", buffer);
}
bool Ours(const uint32_t* slot) {
    uint32_t back = 0;
    return *slot && Read(*slot + turret::kHandleOwner, &back, 4) && back == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(slot));
}
void ReleaseSlot(Slot& s) {
    if (Ours(&s.handle) && !turret::SoftKillRaw(&s.handle)) InterlockedIncrement(&ringFaults);
    s.handle = 0;
    s.held = false;
}
void ReleaseRing() {
    for (Slot& s : slots) if (s.held || s.handle) ReleaseSlot(s);
}
// Points on the radius; each finds its floor (a cast down from a metre above the
// middle's height), but only ever rises: a lower floor or none keeps it at the
// middle's height. One behind a wall is left out. The floor height comes from the
// cast's fraction: CastingTest's output vector is the hit normal (its collector
// keeps no ray), so its y is about 1 on any flat floor.
void BuildRing(const V3& center, float radius) {
    ReleaseRing();
    ringN = ringPoints;
    ringUpLogged = false;
    int walls = 0, drops = 0, floors = 0;
    float low = 1e9f, highest = -1e9f;
    for (int i = 0; i < ringN; ++i) {
        Slot& s = slots[i];
        s = Slot{};
        s.at = coop_gadgets::RingPoint(center, radius, i, ringN, 0.05f);
        if (!gadgets::castOn) continue;
        float fraction = 1.0f;
        uint32_t hit = 0;
        const float high = center.y + 0.9f;
        if (gadgets::Cast({center.x, high, center.z}, {s.at.x, high, s.at.z}, 0, fraction, hit, nullptr) && fraction < 0.999f) {
            s.skip = true;
            ++walls;
            continue;
        }
        const float top = center.y + 1.0f, bottom = center.y - 0.25f;
        fraction = 1.0f;
        const bool floor = gadgets::Cast({s.at.x, top, s.at.z}, {s.at.x, bottom, s.at.z}, 0, fraction, hit, nullptr) && fraction < 0.999f;
        const float y = floor ? top + (bottom - top) * fraction : center.y;
        if (y > center.y) {
            s.at.y = y + 0.05f;
            ++floors;
            if (s.at.y < low) low = s.at.y;
            if (s.at.y > highest) highest = s.at.y;
        } else {
            ++drops;   // stays at the middle's height
        }
    }
    InterlockedIncrement(&ringBuilds);
    InterlockedExchangeAdd(&ringWalls, walls);
    InterlockedExchangeAdd(&ringDrops, drops);
    char heights[64] = "";
    if (floors) std::snprintf(heights, sizeof(heights), ", raised to %.2f..%.2f (middle %.2f)", low - 0.05f, highest - 0.05f, center.y);
    Line("ring of %d points on %.1f m around (%.1f %.1f %.1f): %d behind a wall, %d at the middle's height%s%s", ringN, radius, center.x, center.y, center.z, walls, drops,
         heights, gadgets::castOn ? "" : " (no camera cast: unchecked, at the middle's height)");
}
bool ChooseEffect(DWORD now) {
    if (ringEffect) return true;
    if (static_cast<LONG>(now - chooseAt) < 0) return false;
    chooseAt = now + 1000;
    uint32_t resource = 0;
    if (turret::FindVfxRaw(ringGuid, resource) && resource) { ringEffect = ringGuid; return true; }
    resource = 0;
    if (turret::FindVfxRaw(kRingFallback, resource) && resource) {
        ringEffect = kRingFallback;
        Line("ring effect %08X-%08X not resident, the hologram %08X-%08X stands in", ringGuid[0], ringGuid[3], kRingFallback[0], kRingFallback[3]);
        return true;
    }
    if (InterlockedIncrement(&ringAbsent) <= 3) Line("neither %08X-%08X nor %08X-%08X is resident: no ring", ringGuid[0], ringGuid[3], kRingFallback[0], kRingFallback[3]);
    return false;
}
__declspec(noinline) bool SetTintRaw(uint32_t* slot) {
    __try {
        reinterpret_cast<void(__thiscall*)(uint32_t*, const float*)>(base + kSetTintRva)(slot, ringTint);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Missing points are created (four at most per call), ended ones refilled; while
// contested one point after the other is renewed, so the ring shimmers.
void ShowRing(DWORD now, bool contested) {
    if (!turret::VfxRunning() || !ChooseEffect(now)) return;
    if (contested && static_cast<LONG>(now - pulseAt) >= 0) {
        pulseAt = now + kPulseMs;
        for (int k = 0; k < ringN; ++k) {
            const int i = (pulseNext + k) % ringN;
            if (!slots[i].held) continue;
            ReleaseSlot(slots[i]);
            slots[i].retryAt = 0;
            pulseNext = i + 1;
            InterlockedIncrement(&ringPulses);
            break;
        }
    }
    int made = 0, shown = 0;
    for (int i = 0; i < ringN; ++i) {
        Slot& s = slots[i];
        if (s.skip) continue;
        if (s.held && !Ours(&s.handle)) {
            s.handle = 0;
            s.held = false;
            s.retryAt = now + kRefillMs;
            InterlockedIncrement(&ringEnded);
        }
        if (s.held) { ++shown; continue; }
        if (made >= 4 || static_cast<LONG>(now - s.retryAt) < 0) continue;
        alignas(16) const float m[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, s.at.x, s.at.y, s.at.z, 1.0f};
        s.handle = 0;
        ++made;
        if (!turret::CreateRaw(&s.handle, ringEffect, m)) {
            s.handle = 0;
            s.retryAt = now + 1000;
            InterlockedIncrement(&ringFaults);
            continue;
        }
        if (!s.handle) { s.retryAt = now + 1000; continue; }
        if (tinted && !SetTintRaw(&s.handle)) InterlockedIncrement(&ringFaults);
        s.held = true;
        s.retryAt = now + kRefillMs;
        InterlockedIncrement(&ringCreated);
        ++shown;
    }
    if (!ringUpLogged && shown) {
        ringUpLogged = true;
        Line("ring up: %d of %d points, effect %08X-%08X", shown, ringN, ringEffect[0], ringEffect[3]);
    }
}

// --- the locator ---
alignas(16) float locatorPos[4] = {0.0f, 0.0f, 0.0f, 1.0f};
uint32_t locatorManager = 0;
DWORD reassertAt = 0;
volatile LONG locatorAdds = 0, locatorRemoves = 0, locatorFaults = 0, locatorNoManager = 0;
__declspec(noinline) bool AddRaw(uint32_t manager) {
    __try {
        using Add = void(__thiscall*)(void*, uint32_t, uint32_t, const float*, const float*, const float*, float, float, int);
        // a low priority (FindNewObjective takes the lowest): current only while we say so
        reinterpret_cast<Add>(base + kAddRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(manager)), kObjectiveId, 0, locatorPos, locatorPos, locatorPos, 100.0f,
                                              100.0f, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool RemoveRaw(uint32_t manager) {
    __try {
        using Remove = void(__thiscall*)(void*, uint32_t, uint32_t);
        reinterpret_cast<Remove>(base + kRemoveRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(manager)), kObjectiveId, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool locatorReady = false;
void Locate(DWORD now) {
    if (!locatorReady || static_cast<LONG>(now - reassertAt) < 0) return;
    reassertAt = now + kReassertMs;
    uint32_t manager = 0;
    if (!Read(base + kManagerRva, &manager, 4) || !manager) {
        if (InterlockedIncrement(&locatorNoManager) == 1) Line("no ObjectiveManager: no locator line");
        return;
    }
    if (!AddRaw(manager)) {
        if (InterlockedIncrement(&locatorFaults) >= 3) { locatorReady = false; Line("three faulted AddObjective calls: the locator is off for the session"); }
        return;
    }
    if (!locatorManager) Line("locator entry %08X added to ObjectiveManager %08X at (%.1f %.1f %.1f)", kObjectiveId, manager, locatorPos[0], locatorPos[1], locatorPos[2]);
    locatorManager = manager;
    InterlockedIncrement(&locatorAdds);
}
void Unlocate(const char* why) {
    if (!locatorManager) return;
    uint32_t manager = 0;
    // only on the manager it went into
    if (Read(base + kManagerRva, &manager, 4) && manager == locatorManager && RemoveRaw(manager)) InterlockedIncrement(&locatorRemoves);
    else InterlockedIncrement(&locatorFaults);
    Line("locator entry removed (%s)", why);
    locatorManager = 0;
    reassertAt = 0;
}

// --- per tick ---
struct Shown { int state = 0; uint32_t id = 0; float pos[3]{}; float radius = 0.0f; int percent = 0; bool remote = false; };
Shown shown;
Desired sentWire;
DWORD sentAt = 0;
LONG startsSeen = 0;
volatile LONG changes = 0, overlayTexts = 0;
void Clear(const char* why) {
    if (!shown.state) return;
    ReleaseRing();
    Unlocate(why);
    Line("zone %u hidden: %s", shown.id, why);
    shown = Shown{};
}

// --- the ring's colour ---
// ColorKey steps to the next glow that is loaded, while a ring is up, and keeps it
// in [Zone] RingEffect.
int colorKey = VK_F7;
bool colorDown = false;
volatile LONG colourSteps = 0;
void Notice(const char* format, ...) {
    char text[96];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    for (char* c = text; *c; ++c) *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
    notice::Show(text);
}
void NextGlow() {
    const int count = static_cast<int>(std::size(kGlows));
    for (int step = 1; step <= count; ++step) {
        const int i = ((glowIndex < 0 ? count - 1 : glowIndex) + step) % count;
        uint32_t resource = 0;
        if (!turret::FindVfxRaw(kGlows[i].guid, resource) || !resource) continue;
        glowIndex = i;
        std::memcpy(ringGuid, kGlows[i].guid, sizeof(ringGuid));
        ringEffect = nullptr;
        chooseAt = 0;
        ReleaseRing();   // the points stay; the next tick creates them in the new glow
        for (Slot& s : slots) s.retryAt = 0;
        ringUpLogged = false;
        WritePrivateProfileStringA("Zone", "RingEffect", kGlows[i].name, iniFile.c_str());
        InterlockedIncrement(&colourSteps);
        Notice("zone ring: %s (%d/%d)", kGlows[i].name, i + 1, count);
        Line("ring effect %s %08X-%08X, kept in [Zone] RingEffect", kGlows[i].name, kGlows[i].guid[0], kGlows[i].guid[3]);
        return;
    }
    Notice("zone ring: no other glow is loaded");
}

// --- the spot recorder ---
// RecordKey saves the spot you stand on for this map; DeleteKey removes the
// nearest saved one within 6 m. DS2Zones.txt next to this INI, "map x y z" a
// line (the director reads it when a run begins; a fifth number is a radius).
// A saved spot shows its ring for a few seconds when no zone is up.
int recordKey = VK_PRIOR, deleteKey = VK_NEXT;
bool recordDown = false, deleteDown = false;
std::string spotsPath, capsulePath;
float previewRadius = 4.5f;
// In Mercenaries the same keys record where the time capsules go
// (DS2Capsules.txt, the same "map x y z" lines); the director drops them there.
bool CapsuleKeys() { return InterlockedCompareExchange(&gamemode::mode, 0, 0) == 1; }   // 1 = Mercenaries
const std::string& SpotFile() { return CapsuleKeys() ? capsulePath : spotsPath; }
const char* SpotKind() { return CapsuleKeys() ? "capsule spot" : "zone spot"; }
Desired preview;
DWORD previewUntil = 0;
volatile LONG spotsSaved = 0, spotsDeleted = 0, spotFaults = 0;
struct SpotLine { std::string text; bool spot = false; char map[64]{}; float pos[3]{}; };
std::vector<SpotLine> ReadSpots() {
    std::vector<SpotLine> lines;
    FILE* f = nullptr;
    if (fopen_s(&f, SpotFile().c_str(), "r") != 0 || !f) return lines;
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), f)) {
        SpotLine l;
        l.text = buffer;
        while (!l.text.empty() && (l.text.back() == '\n' || l.text.back() == '\r')) l.text.pop_back();
        std::string plain = l.text;
        const size_t comment = plain.find_first_of("#;");
        if (comment != std::string::npos) plain.resize(comment);
        for (char& c : plain) if (c == ',') c = ' ';
        l.spot = sscanf_s(plain.c_str(), "%63s %f %f %f", l.map, static_cast<unsigned>(sizeof(l.map)), &l.pos[0], &l.pos[1], &l.pos[2]) == 4;
        lines.push_back(l);
    }
    std::fclose(f);
    return lines;
}
unsigned CountSpots(const std::vector<SpotLine>& lines, const char* map) {
    unsigned n = 0;
    for (const SpotLine& l : lines) n += l.spot && _stricmp(l.map, map) == 0;
    return n;
}
void SpotKeys(DWORD now) {
    // Not in a story session with partners: the keys are a survival tool.
    if (play::StoryCoop() && peerTable.Count()) return;
    const bool focused = necro::GameWindowFocused();
    const bool save = recordKey && focused && (GetAsyncKeyState(recordKey) & 0x8000) != 0;
    const bool drop = deleteKey && focused && (GetAsyncKeyState(deleteKey) & 0x8000) != 0;
    const bool colour = colorKey && focused && (GetAsyncKeyState(colorKey) & 0x8000) != 0;
    const bool savePressed = save && !recordDown, dropPressed = drop && !deleteDown, colourPressed = colour && !colorDown;
    recordDown = save;
    deleteDown = drop;
    colorDown = colour;
    if (colourPressed && shown.state && ringWanted) {
        NextGlow();
        if (previewUntil) previewUntil = now + 8000;   // time to look at it
    }
    if (!savePressed && !dropPressed) return;
    const uint32_t player = inLevel && !turret::FrontendNow() ? LocalCampaignPlayer() : 0;
    char map[64] = "", stream[64] = "";
    float pos[3]{};
    if (!player || !level::CurrentMap(map, sizeof(map), stream, sizeof(stream)) || !Read(player + 0xC0, pos, sizeof(pos)) || !std::isfinite(pos[0]) ||
        !std::isfinite(pos[1]) || !std::isfinite(pos[2]))
        return;
    arena::RunningName(map, sizeof(map));   // a campaign arena: its name, the director's map
    std::vector<SpotLine> lines = ReadSpots();
    if (savePressed) {
        FILE* f = nullptr;
        const bool fresh = lines.empty();
        if (fopen_s(&f, SpotFile().c_str(), "a") != 0 || !f) {
            InterlockedIncrement(&spotFaults);
            Notice("%s not saved: %s not writable", SpotKind(), CapsuleKeys() ? "DS2Capsules.txt" : "DS2Zones.txt");
            return;
        }
        if (fresh) std::fprintf(f, CapsuleKeys() ? "# Mercenaries time capsule spots, recorded in the game ([Zone] RecordKey in a Mercenaries run): map x y z\n"
                                                 : "# Hold the Zone spots, recorded in the game ([Zone] RecordKey): map x y z [radius]\n");
        std::fprintf(f, "%s %.2f %.2f %.2f\n", map, pos[0], pos[1], pos[2]);
        std::fclose(f);
        InterlockedIncrement(&spotsSaved);
        const unsigned n = CountSpots(lines, map) + 1;
        Notice("%s %u saved on %s", SpotKind(), n, map);
        Line("%s %u saved on %s at (%.2f %.2f %.2f) in %s", SpotKind(), n, map, pos[0], pos[1], pos[2], SpotFile().c_str());
        preview = Desired{};
        preview.state = coop_zone::Empty;
        preview.id = 0xFFFF0000u | n;
        std::memcpy(preview.pos, pos, sizeof(preview.pos));
        preview.radius = CapsuleKeys() ? 1.0f : previewRadius;
        previewUntil = now + 6000;
        return;
    }
    int nearest = -1;
    float best = 6.0f * 6.0f;
    for (size_t i = 0; i < lines.size(); ++i) {
        const SpotLine& l = lines[i];
        if (!l.spot || _stricmp(l.map, map) != 0) continue;
        const float dx = l.pos[0] - pos[0], dy = l.pos[1] - pos[1], dz = l.pos[2] - pos[2], d = dx * dx + dy * dy + dz * dz;
        if (d <= best) { best = d; nearest = static_cast<int>(i); }
    }
    if (nearest < 0) { Notice("no %s within 6 m", SpotKind()); return; }
    const SpotLine gone = lines[static_cast<size_t>(nearest)];
    lines.erase(lines.begin() + nearest);
    FILE* f = nullptr;
    if (fopen_s(&f, SpotFile().c_str(), "w") != 0 || !f) {
        InterlockedIncrement(&spotFaults);
        Notice("%s not removed: %s not writable", SpotKind(), CapsuleKeys() ? "DS2Capsules.txt" : "DS2Zones.txt");
        return;
    }
    for (const SpotLine& l : lines) std::fprintf(f, "%s\n", l.text.c_str());
    std::fclose(f);
    InterlockedIncrement(&spotsDeleted);
    Notice("%s removed, %u left on %s", SpotKind(), CountSpots(lines, map), map);
    Line("spot at (%.2f %.2f %.2f) removed from %s (%.1f m away), %u left", gone.pos[0], gone.pos[1], gone.pos[2], map, std::sqrt(best), CountSpots(lines, map));
    previewUntil = 0;
}
// The host's word to its partners: a change three times, else once a second.
void Send(const Desired& d, DWORD now) {
    if (!network || !IsHost()) return;
    const bool changed = d.state != sentWire.state || d.id != sentWire.id || d.pos[0] != sentWire.pos[0] || d.pos[2] != sentWire.pos[2];
    if (!changed && (now - sentAt < kSendMs || !d.state)) return;   // "none" goes out on the change only
    coop_zone::Wire w{};
    w.id = d.id;
    std::memcpy(w.pos, d.pos, sizeof(w.pos));
    w.radius = d.radius;
    w.state = static_cast<uint8_t>(d.state);
    w.percent = static_cast<uint8_t>(d.percent < 0 ? 0 : d.percent > 100 ? 100 : d.percent);
    survnet::QueueZone(w, changed ? 3 : 1);
    sentWire = d;
    sentAt = now;
    InterlockedIncrement(&sent);
}
void Tick(DWORD now) {
    if (!enabled) return;
    Desired d;
    bool fromRemote = false;
    AcquireSRWLockShared(&lock);
    // set from other threads with a fresh tick: one newer than this `now` is fresh, not 49 days old
    auto fresh = [now](DWORD at, DWORD ms) { return at && (static_cast<LONG>(now - at) <= 0 || now - at < ms); };
    if (fresh(local.at, kLocalFreshMs)) d = local;
    else if (fresh(remote.at, kRemoteFreshMs)) { d = remote; fromRemote = true; }
    ReleaseSRWLockShared(&lock);
    if (!fromRemote && local.at) Send(d, now);
    SpotKeys(now);
    // a spot just saved shows its ring while no zone is up (no locator, nothing sent)
    const bool previewing = !d.state && previewUntil && static_cast<LONG>(now - previewUntil) < 0;
    if (previewing) d = preview;
    else previewUntil = 0;
    const LONG starts = InterlockedCompareExchange(&loadsync::levelStarts, 0, 0);
    if (starts != startsSeen) {
        startsSeen = starts;
        // the level took the effects; the manager is a service and keeps our entry
        for (Slot& s : slots) s = Slot{};
        ringEffect = nullptr;
        Unlocate("a level start");
        shown = Shown{};
    }
    if (!d.state || !inLevel || turret::FrontendNow()) { Clear(!d.state ? "no zone" : "out of the level"); return; }
    const bool moved = d.id != shown.id || d.pos[0] != shown.pos[0] || d.pos[1] != shown.pos[1] || d.pos[2] != shown.pos[2] || d.radius != shown.radius;
    if (moved) {
        InterlockedIncrement(&changes);
        Line("zone %u at (%.1f %.1f %.1f) radius %.1f, %s (%s)", d.id, d.pos[0], d.pos[1], d.pos[2], d.radius, coop_zone::StateName(d.state),
             previewing ? "a saved spot's preview" : fromRemote ? "the host's" : "our director's");
        if (ringWanted) BuildRing(V3{d.pos[0], d.pos[1], d.pos[2]}, d.radius);
        locatorPos[0] = d.pos[0];
        locatorPos[1] = d.pos[1];
        locatorPos[2] = d.pos[2];
        reassertAt = 0;
    }
    shown.state = d.state;
    shown.id = d.id;
    std::memcpy(shown.pos, d.pos, sizeof(shown.pos));
    shown.radius = d.radius;
    shown.percent = d.percent;
    shown.remote = fromRemote;
    const bool scene = turret::WorldReady() && !turret::CutsceneNow();
    if (ringWanted && scene) ShowRing(now, d.state == coop_zone::Contested);
    else if (ringWanted) ReleaseRing();
    if (locatorWanted && !previewing) Locate(now);
    else if (previewing) Unlocate("a saved spot's preview");
}
// The overlay's part after the director's line, for our own Isaac. Game thread.
bool OverlayText(char* out, size_t size) {
    if (!enabled || !overlayWanted || !shown.state) return false;
    const uint32_t player = inLevel ? LocalCampaignPlayer() : 0;
    float self[3]{}, forward[3]{};
    if (!player || !Read(player + 0xC0, self, sizeof(self)) || !Read(player + 0xB0, forward, sizeof(forward))) return false;
    coop_zone::Text(out, size, self, forward, shown.pos, shown.radius);
    InterlockedIncrement(&overlayTexts);
    return true;
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Zone", "Enabled", 1, ini.c_str()) != 0;
    ringWanted = GetPrivateProfileIntA("Zone", "Ring", 1, ini.c_str()) != 0;
    locatorWanted = GetPrivateProfileIntA("Zone", "Locator", 1, ini.c_str()) != 0;
    overlayWanted = GetPrivateProfileIntA("Zone", "Overlay", 1, ini.c_str()) != 0;
    ringPoints = GetPrivateProfileIntA("Zone", "RingPoints", 16, ini.c_str());
    if (ringPoints < 4) ringPoints = 4;
    if (ringPoints > coop_gadgets::kRingMaxPoints) ringPoints = coop_gadgets::kRingMaxPoints;
    iniFile = ini;
    const size_t slash = ini.find_last_of("\\/");
    const std::string folder = slash == std::string::npos ? std::string() : ini.substr(0, slash + 1);
    spotsPath = folder + "DS2Zones.txt";
    capsulePath = folder + "DS2Capsules.txt";
    recordKey = GetPrivateProfileIntA("Zone", "RecordKey", VK_PRIOR, ini.c_str());
    deleteKey = GetPrivateProfileIntA("Zone", "DeleteKey", VK_NEXT, ini.c_str());
    colorKey = GetPrivateProfileIntA("Zone", "ColorKey", VK_F7, ini.c_str());   // F8 is the catch-up key
    char text[64]{};
    // the preview ring of a saved spot has the director's radius
    GetPrivateProfileStringA("Zone", "RadiusMeters", "4.5", text, sizeof(text), ds2coop::ConfigFile(folder, "DS2Survival.ini").c_str());
    previewRadius = static_cast<float>(std::atof(text));
    if (!(previewRadius >= 2.0f && previewRadius <= 12.0f)) previewRadius = 4.5f;
    GetPrivateProfileStringA("Zone", "RingEffect", "", text, sizeof(text), ini.c_str());
    unsigned g[4]{};
    const char* effect = "stasis";
    if (text[0] && sscanf_s(text, "%x-%x-%x-%x", &g[0], &g[1], &g[2], &g[3]) == 4) {
        for (int i = 0; i < 4; ++i) ringGuid[i] = g[i];
        glowIndex = -1;
        effect = "a GUID";
    } else if (text[0]) {
        for (int i = 0; i < static_cast<int>(std::size(kGlows)); ++i)
            if (_stricmp(text, kGlows[i].name) == 0) { glowIndex = i; std::memcpy(ringGuid, kGlows[i].guid, sizeof(ringGuid)); effect = kGlows[i].name; }
        if (glowIndex == 0 && _stricmp(text, "stasis") != 0) std::fprintf(log, "zone: [Zone] RingEffect=%s is no glow name, stasis used\n", text);
    }
    GetPrivateProfileStringA("Zone", "RingTint", "", text, sizeof(text), ini.c_str());
    for (char* c = text; *c; ++c) if (*c == ',') *c = ' ';
    float tint[3]{};
    tinted = text[0] && sscanf_s(text, "%f %f %f", &tint[0], &tint[1], &tint[2]) == 3;
    for (int i = 0; i < 3 && tinted; ++i) ringTint[i] = tint[i] < 0.0f ? 0.0f : tint[i] > 4.0f ? 4.0f : tint[i];
    // push ebp / mov ebp,esp / and esp,-10h / mov eax,[ecx] / test eax,eax
    static const unsigned char kTint[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x01, 0x85, 0xC0};
    unsigned char tintHave[sizeof(kTint)]{};
    if (tinted && !(Read(base + kSetTintRva, tintHave, sizeof(tintHave)) && std::memcmp(tintHave, kTint, sizeof(tintHave)) == 0)) {
        tinted = false;
        std::fprintf(log, "zone: VFXHandle::SetTint %08X MISMATCH, [Zone] RingTint ignored\n", static_cast<unsigned>(base + kSetTintRva));
    }
    std::fprintf(log, "zone ring effect %s %08X-%08X, tint %s (%.2f %.2f %.2f); spots %s, keys record %d delete %d colour %d, preview radius %.1f\n", effect,
                 ringGuid[0], ringGuid[3], tinted ? "on" : "off", ringTint[0], ringTint[1], ringTint[2], spotsPath.c_str(), recordKey, deleteKey, colorKey,
                 previewRadius);
    // mov edx,[esp+4] / push ebx / mov ebx,[esp+0Ch] / push edi / mov edi,ecx
    static const unsigned char kAdd[] = {0x8B, 0x54, 0x24, 0x04, 0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x57, 0x8B, 0xF9};
    // push esi / push edi / mov edi,ecx / mov edx,[edi+0Ch] / xor esi,esi
    static const unsigned char kRemove[] = {0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x57, 0x0C, 0x33, 0xF6};
    unsigned char add[sizeof(kAdd)]{}, remove[sizeof(kRemove)]{};
    const bool addOk = Read(base + kAddRva, add, sizeof(add)) && std::memcmp(add, kAdd, sizeof(add)) == 0;
    const bool removeOk = Read(base + kRemoveRva, remove, sizeof(remove)) && std::memcmp(remove, kRemove, sizeof(remove)) == 0;
    locatorReady = enabled && locatorWanted && addOk && removeOk;
    std::fprintf(log, "zone display %s: ring %s (%d points, effect %08X-%08X), locator %s (AddObjective %s, RemoveObjective %s), overlay %s\n",
                 enabled ? "on" : "off ([Zone] Enabled=0)", ringWanted ? "on" : "off", ringPoints, ringGuid[0], ringGuid[3],
                 locatorReady ? "on" : locatorWanted ? "NOT ready" : "off", addOk ? "verified" : "MISMATCH", removeOk ? "verified" : "MISMATCH",
                 overlayWanted ? "on" : "off");
}
void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG sum = InterlockedCompareExchange(&localSets, 0, 0) / 10 + InterlockedCompareExchange(&remoteIn, 0, 0) / 10 + InterlockedCompareExchange(&changes, 0, 0) * 7 +
                     InterlockedCompareExchange(&ringCreated, 0, 0) * 3 + InterlockedCompareExchange(&locatorAdds, 0, 0) / 10 + InterlockedCompareExchange(&ringFaults, 0, 0) * 11 +
                     InterlockedCompareExchange(&locatorFaults, 0, 0) * 13 + InterlockedCompareExchange(&spotsSaved, 0, 0) * 17 +
                     InterlockedCompareExchange(&spotsDeleted, 0, 0) * 19 + InterlockedCompareExchange(&colourSteps, 0, 0) * 23;
    if (sum == last || !sum) return;
    std::fprintf(log, "zone spots saved=%ld deleted=%ld faults=%ld colour_steps=%ld glow=%s\n", InterlockedCompareExchange(&spotsSaved, 0, 0),
                 InterlockedCompareExchange(&spotsDeleted, 0, 0), InterlockedCompareExchange(&spotFaults, 0, 0), InterlockedCompareExchange(&colourSteps, 0, 0),
                 glowIndex >= 0 ? kGlows[glowIndex].name : "guid");
    last = sum;
    std::fprintf(log, "zone shown=%s id=%u percent=%d (%s) local_sets=%ld remote_in=%ld sent=%ld changes=%ld ring builds=%ld created=%ld ended=%ld pulses=%ld walls=%ld "
                      "drops=%ld absent=%ld faults=%ld locator adds=%ld removes=%ld faults=%ld no_manager=%ld overlay_texts=%ld\n",
                 coop_zone::StateName(shown.state), shown.id, shown.percent, shown.remote ? "host's" : "ours", InterlockedCompareExchange(&localSets, 0, 0),
                 InterlockedCompareExchange(&remoteIn, 0, 0), InterlockedCompareExchange(&sent, 0, 0), InterlockedCompareExchange(&changes, 0, 0),
                 InterlockedCompareExchange(&ringBuilds, 0, 0), InterlockedCompareExchange(&ringCreated, 0, 0), InterlockedCompareExchange(&ringEnded, 0, 0),
                 InterlockedCompareExchange(&ringPulses, 0, 0), InterlockedCompareExchange(&ringWalls, 0, 0), InterlockedCompareExchange(&ringDrops, 0, 0),
                 InterlockedCompareExchange(&ringAbsent, 0, 0), InterlockedCompareExchange(&ringFaults, 0, 0), InterlockedCompareExchange(&locatorAdds, 0, 0),
                 InterlockedCompareExchange(&locatorRemoves, 0, 0), InterlockedCompareExchange(&locatorFaults, 0, 0),
                 InterlockedCompareExchange(&locatorNoManager, 0, 0), InterlockedCompareExchange(&overlayTexts, 0, 0));
}
}  // namespace zone
