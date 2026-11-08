// Dead Space 2 United by Richard Hafer.
// Paths for the companion bot from the game's own NavPower mesh.
//
// The game asks exactly this for the locator line (PlayerNavHelpSM::NavSearch,
// PC 0x010BE060): the level's navigation space from the NavManager
// ([0x020AD0C0], space list at +0x20, NavSpace next +4, name +8, SpaceHandle
// +0x10; one space named 0 while a level stands, none in menus),
// bfx::CreatePolylinePath (0x005801B0, cdecl, hidden result pointer first:
// (RCPtr* out, SpaceHandle*, Vector3* start, Vector3* end, layer, PathSpec*))
// with layer 0 and PathSpec {0xFF, ~0, 0 = surface only, 0, 0, 0}, then the
// segments through the RCPtr (count 0x0057B9A0, type 0x0057B9F0, surface
// segment 0x0057BA50, link segment 0x0057BAC0; start at segment+8, end at
// +0x14) and the RCPtr destructor (0x0057F180) on every path, empty or not:
// a missed one leaks the NavPower pool into its "out of heap" int3. Game
// coordinates go in and come out unchanged (Y-up, ConvertVec is a copy).
// Endpoints are snapped to the mesh; an unreachable goal gives a path to the
// closest reachable spot, so the caller compares the last point with the goal.
// Game thread only, never while a level loads.
namespace bot_nav {
using coop_bot::Vec3;
constexpr uint32_t kNavManagerRva = 0x1CAD0C0, kSpaceList = 0x20, kSpaceNext = 4, kSpaceName = 8, kSpaceHandle = 0x10;
constexpr uint32_t kCreatePathRva = 0x1801B0, kNumSegmentsRva = 0x17B9A0, kSegmentTypeRva = 0x17B9F0, kSurfaceSegmentRva = 0x17BA50,
                   kLinkSegmentRva = 0x17BAC0, kPathDtorRva = 0x17F180;
bool ready = false;
volatile LONG queries = 0, found = 0, empty = 0, noSpace = 0, faults = 0;

struct PathSpec { uint32_t blockage, geoMatch, linkUsage; uint8_t usePenalty, pad[3]; float penalty, maxSearch; };
static_assert(sizeof(PathSpec) == 0x18, "bfx::PathSpec");
struct RCPtr { void* proxy; };

uint32_t LevelSpace() {
    uint32_t manager = 0, ns = 0;
    if (!Read(base + kNavManagerRva, &manager, 4) || !manager || !Read(manager + kSpaceList, &ns, 4)) return 0;
    for (unsigned steps = 0; ns && steps < 16; ++steps) {
        uint32_t name = 1, proxy = 0, space = 0, next = 0;
        if (Read(ns + kSpaceName, &name, 4) && name == 0 && Read(ns + kSpaceHandle, &proxy, 4) && proxy && Read(proxy, &space, 4) && space)
            return ns + kSpaceHandle;
        if (!Read(ns + kSpaceNext, &next, 4) || next == ns) break;
        ns = next;
    }
    return 0;
}

// POD only inside the __try (the destructor must run on every path).
__declspec(noinline) int QueryRaw(uint32_t space, const float* from, const float* to, float (*out)[3], int cap) {
    using Create = RCPtr* (__cdecl*)(RCPtr*, void*, const float*, const float*, uint32_t, const PathSpec*);
    using Count = int (__thiscall*)(const RCPtr*);
    using Type = int (__thiscall*)(const RCPtr*, int);
    using Segment = const uint8_t* (__thiscall*)(const RCPtr*, int);
    using Dtor = void (__thiscall*)(RCPtr*);
    PathSpec spec = {0xFF, 0xFFFFFFFFu, 0u, 0, {0, 0, 0}, 0.0f, 0.0f};
    RCPtr path = {nullptr};
    int n = 0;
    __try {
        reinterpret_cast<Create>(base + kCreatePathRva)(&path, reinterpret_cast<void*>(static_cast<uintptr_t>(space)), from, to, 0, &spec);
        const int segments = reinterpret_cast<Count>(base + kNumSegmentsRva)(&path);
        for (int i = 0; i < segments && n + 1 < cap; ++i) {
            const int type = reinterpret_cast<Type>(base + kSegmentTypeRva)(&path, i);
            const uint8_t* segment = reinterpret_cast<Segment>(base + (type == 0 ? kSurfaceSegmentRva : kLinkSegmentRva))(&path, i);
            if (!segment) continue;
            std::memcpy(out[n++], segment + 0x08, 12);
            if (i == segments - 1) std::memcpy(out[n++], segment + 0x14, 12);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = -1;
    }
    __try {
        if (path.proxy) reinterpret_cast<Dtor>(base + kPathDtorRva)(&path);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = -1;
    }
    return n;
}

// Waypoints from -> to (from itself first). False: no level space, no path, or a
// path that neither starts near the bot nor gets near the goal.
bool Query(const Vec3& from, const Vec3& to, Vec3* out, int cap, int& count) {
    count = 0;
    if (!ready || !inLevel || !levelSettled || cap < 2) return false;
    const uint32_t space = LevelSpace();
    if (!space) { InterlockedIncrement(&noSpace); return false; }
    InterlockedIncrement(&queries);
    float a[3] = {from.x, from.y, from.z}, b[3] = {to.x, to.y, to.z};
    float points[coop_bot::kMaxWaypoints][3];
    const int limit = cap < coop_bot::kMaxWaypoints ? cap : coop_bot::kMaxWaypoints;
    const int n = QueryRaw(space, a, b, points, limit);
    if (n < 0) { InterlockedIncrement(&faults); return false; }
    if (n < 2) { InterlockedIncrement(&empty); return false; }
    for (int i = 0; i < n; ++i) out[i] = {points[i][0], points[i][1], points[i][2]};
    // The locator's own check: the path must start at the bot (5 m), and it has to reach the goal area.
    if (coop_bot::Distance(out[0], from) > 5.0f || coop_bot::Distance(out[n - 1], to) > 3.0f) { InterlockedIncrement(&empty); return false; }
    count = n;
    InterlockedIncrement(&found);
    return true;
}

// Where a walk from -> to really ends: the goal itself, or the closest point the
// mesh reaches toward it. False without a level space or a path.
bool PathEnd(const Vec3& from, const Vec3& to, Vec3& end, int& count) {
    count = 0;
    if (!ready || !inLevel || !levelSettled) return false;
    const uint32_t space = LevelSpace();
    if (!space) { InterlockedIncrement(&noSpace); return false; }
    InterlockedIncrement(&queries);
    float a[3] = {from.x, from.y, from.z}, b[3] = {to.x, to.y, to.z};
    float points[coop_bot::kMaxWaypoints][3];
    const int n = QueryRaw(space, a, b, points, coop_bot::kMaxWaypoints);
    if (n < 0) { InterlockedIncrement(&faults); return false; }
    if (n < 2) { InterlockedIncrement(&empty); return false; }
    end = {points[n - 1][0], points[n - 1][1], points[n - 1][2]};
    count = n;
    InterlockedIncrement(&found);
    return true;
}

// The mesh point next to `at`: a path's start is snapped to the mesh, so a short walk
// from `at` tells where the mesh is. False when it is farther than 2.5 m (1.5 m up or down).
bool Snap(const Vec3& at, Vec3& out) {
    if (!ready || !inLevel || !levelSettled) return false;
    const uint32_t space = LevelSpace();
    if (!space) { InterlockedIncrement(&noSpace); return false; }
    InterlockedIncrement(&queries);
    float a[3] = {at.x, at.y, at.z}, b[3] = {at.x + 0.75f, at.y, at.z};
    float points[coop_bot::kMaxWaypoints][3];
    const int n = QueryRaw(space, a, b, points, coop_bot::kMaxWaypoints);
    if (n < 0) { InterlockedIncrement(&faults); return false; }
    if (n < 1) { InterlockedIncrement(&empty); return false; }
    out = {points[0][0], points[0][1], points[0][2]};
    return coop_bot::Flat(out, at) <= 2.5f && std::fabs(out.y - at.y) <= 1.5f;
}

bool Install(FILE* log) {
    struct Site { const char* name; uint32_t rva; unsigned char bytes[16]; };
    static const Site kSites[] = {
        {"CreatePolylinePath", kCreatePathRva, {0xA1, 0xCC, 0xF4, 0x02, 0x02, 0x53, 0x55, 0x8B, 0x68, 0x44, 0x56, 0x57, 0x33, 0xFF, 0x3B, 0xEF}},
        {"GetNumSegments", kNumSegmentsRva, {0xA1, 0xCC, 0xF4, 0x02, 0x02, 0x56, 0x8B, 0x70, 0x44, 0x85, 0xF6, 0x57, 0x8B, 0xF9, 0x74, 0x07}},
        {"GetSegmentType", kSegmentTypeRva, {0xA1, 0xCC, 0xF4, 0x02, 0x02, 0x56, 0x8B, 0x70, 0x44, 0x85, 0xF6, 0x57, 0x8B, 0xF9, 0x74, 0x07}},
        {"GetSurfaceSegment", kSurfaceSegmentRva, {0xA1, 0xCC, 0xF4, 0x02, 0x02, 0x56, 0x57, 0x8B, 0x78, 0x44, 0x85, 0xFF, 0x8B, 0xF1, 0x74, 0x07}},
        {"GetLinkSegment", kLinkSegmentRva, {0xA1, 0xCC, 0xF4, 0x02, 0x02, 0x56, 0x8B, 0x70, 0x44, 0x85, 0xF6, 0x57, 0x8B, 0xF9, 0x74, 0x07}},
        {"~PolylinePathRCPtr", kPathDtorRva, {0xA1, 0xCC, 0xF4, 0x02, 0x02, 0x85, 0xC0, 0x57, 0x8B, 0xF9, 0x74, 0x2C, 0x56, 0x8B, 0x70, 0x44}},
    };
    ready = true;
    for (const Site& site : kSites) {
        unsigned char actual[16]{};
        if (!Read(base + site.rva, actual, sizeof(actual)) || std::memcmp(actual, site.bytes, sizeof(actual)) != 0) {
            std::fprintf(log, "bot nav: %s signature mismatch at %08X; no NavPower paths, the bot walks the leader's trail\n", site.name,
                         static_cast<unsigned>(base + site.rva));
            ready = false;
        }
    }
    // CreatePolylinePath's first 64 bytes repeat elsewhere: its call into _Internal pins it.
    static const unsigned char kInternalCall[] = {0xE8, 0x77, 0xFE, 0xFF, 0xFF};
    unsigned char call[sizeof(kInternalCall)]{};
    if (ready && (!Read(base + 0x180214, call, sizeof(call)) || std::memcmp(call, kInternalCall, sizeof(call)) != 0)) {
        std::fprintf(log, "bot nav: CreatePolylinePath is not where it should be (call at %08X); no NavPower paths\n", static_cast<unsigned>(base + 0x180214));
        ready = false;
    }
    std::fprintf(log, "bot nav ready=%d (NavPower paths on the game's own mesh, layer 0, surface only)\n", ready ? 1 : 0);
    return ready;
}
}  // namespace bot_nav
