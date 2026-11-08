// Dead Space 2 United by Richard Hafer.
// Zone switches held up by the pool's character packages.
//
// The pool root depends on the stock character packages (char_str\npc\*),
// and level stream sets list the same packages as members. Whoever loads a
// package first owns it. When the pool comes first, the set finds it loaded,
// never takes it over and never unloads it. When the level comes first (the
// usual case above ~95 FPS, because the pool request waits for a stream update
// and a one second throttle), the set's own load claims the record that the
// pool then also holds through its implicit parent reference.
//
// At the next area switch Set::ProcessPendingLoads (0x00758080) unloads it.
// The explicit bit goes, the pool's reference keeps it at status 3, and the
// set calls Unload again every tick and loads nothing new until the record
// reads status -1 at 0x007580D0. Elevators wait for that load forever
// (friend logs: school 10.09., CEC hub 12.09., public sector 10.09.).
//
// The hook sits on that status test. A package that only the pool still
// holds is shown to the set as unloaded, so the set drops it from its unload
// list and goes on. The record stays loaded under the pool's reference, which
// is exactly the state a capped game ends up in.
namespace zone_unload {

constexpr uint32_t kFunctionRva = 0x358080;
constexpr uint32_t kStatusTestRva = 0x3580D0;
// sub esp,1Ch / push ebx / mov ebx,[0201C098] / push ebp / push esi / mov esi,ecx / mov eax,[esi+0C8h]
const unsigned char kPrologue[] = {0x83, 0xEC, 0x1C, 0x53, 0x8B, 0x1D, 0x98, 0xC0, 0x01, 0x02,
                                   0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0xC8, 0x00, 0x00, 0x00};
// mov eax,[eax+4] / test eax,eax / je / cmp dword ptr [eax+24h],-1 / jne +36h
const unsigned char kSite[] = {0x8B, 0x40, 0x04, 0x85, 0xC0, 0x74, 0x06, 0x83, 0x78, 0x24, 0xFF, 0x75, 0x36};
constexpr uint32_t kSiteOffset = 7;   // kSite starts 7 bytes before the cmp

constexpr uint32_t kUnloadList = 0x10, kUnloadCount = 0xC8, kUnloadCapacity = 0x2E;
constexpr uint32_t kStreamStatus = 0x24, kStreamIdentity = 0x2C, kStreamDescriptor = 0x18;
constexpr uint32_t kStreamFlags = 0x54, kStreamRefs = 0x58, kExplicitRef = 0x20;
constexpr uint32_t kManagerBuckets = 0x5C, kManagerBucketCount = 0x60;

safetyhook::MidHook hook;
bool installed = false;
DWORD delayMs = 250;
// The engine compares [eax+24h] with -1; pointing eax here makes it read -1.
alignas(16) uint32_t unloadedRecord[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFFFFFFFFu, 0, 0, 0, 0, 0, 0};

volatile LONG calls = 0, notLoaded = 0, explicitHeld = 0, noRefs = 0, foreign = 0, poolGone = 0;
volatile LONG othersPending = 0, delayed = 0, healed = 0;

// Which requirement rows are character packages, filled once at install.
unsigned characterRows[std::size(ds2::kStreamRequirements)]{};
unsigned characterRowCount = 0;

struct Pending { uint32_t set = 0, handle = 0; DWORD first = 0, last = 0; };
Pending pending[16]{};

struct Event {
    LONG ready; DWORD tick; uint32_t set, setIndex, handle, identity, flags, refs, stuckMs, unloadCount; int row;
};
Event events[64]{};
volatile LONG eventCount = 0;
LONG eventsWritten = 0;

uint32_t StreamFor(uint32_t manager, uint32_t handle) {
    uint32_t buckets = 0, bucketCount = 0, node = 0;
    if (!manager || !Read(manager + kManagerBuckets, &buckets, 4) || !Read(manager + kManagerBucketCount, &bucketCount, 4) ||
        !buckets || !bucketCount || !Read(buckets + 4 * (handle % bucketCount), &node, 4)) return 0;
    for (unsigned guard = 0; node && guard < 256; ++guard) {
        uint32_t entry[3]{};
        if (!Read(node, entry, sizeof(entry))) return 0;
        if (entry[0] == handle) return entry[1];
        node = entry[2];
    }
    return 0;
}

// The requirement row of a character package the pool depends on, -1 otherwise.
int PoolPackageRow(uint32_t stream) {
    uint32_t identity = 0, descriptor = 0, header = 0, fields[2]{};
    if (!Read(stream + kStreamIdentity, &identity, 4) || !identity) return -1;
    for (unsigned i = 0; i < characterRowCount; ++i) {
        const auto& row = ds2::kStreamRequirements[characterRows[i]];
        if (row.identity != identity) continue;
        const size_t length = std::strlen(row.name);
        char name[160]{};
        if (length >= sizeof(name) || !Read(stream + kStreamDescriptor, &descriptor, 4) || !descriptor ||
            !Read(descriptor, &header, 4) || !header || !Read(header, fields, sizeof(fields)) ||
            fields[0] != identity || !fields[1] || !Read(fields[1], name, length + 1) ||
            std::memcmp(name, row.name, length + 1) != 0) return -1;
        return static_cast<int>(characterRows[i]);
    }
    return -1;
}

enum class Verdict { Unloaded, NotLoaded, ExplicitHeld, NoRefs, Foreign, Candidate };

Verdict Classify(uint32_t stream, uint32_t& flags, uint32_t& refs, int& row) {
    int32_t status = 0;
    uint16_t refWord = 0;
    flags = 0; refs = 0; row = -1;
    if (!Read(stream + kStreamStatus, &status, 4)) return Verdict::NotLoaded;
    if (status == -1) return Verdict::Unloaded;
    if (status != 3) return Verdict::NotLoaded;
    if (!Read(stream + kStreamFlags, &flags, 4) || (flags & kExplicitRef)) return Verdict::ExplicitHeld;
    if (!Read(stream + kStreamRefs, &refWord, 2) || !refWord) return Verdict::NoRefs;
    refs = refWord;
    row = PoolPackageRow(stream);
    return row < 0 ? Verdict::Foreign : Verdict::Candidate;
}

// True while the pool root still exists, so its parent references are real.
bool PoolRootPresent(uint32_t manager) {
    const uint32_t handle = streamLifetime.handle;
    if (!handle || manager != streamLifetime.manager) return false;
    const uint32_t root = StreamFor(manager, handle);
    int32_t status = -1;
    return root && Read(root + kStreamStatus, &status, 4) && status != -1;
}

// True when another entry of this unload list is still on its way out. Its
// reference on a package may be the one that keeps the package loaded, and
// that unload finishes on its own.
bool OthersStillUnloading(uint32_t set, uint32_t manager, uint32_t self) {
    uint32_t count = 0;
    if (!Read(set + kUnloadCount, &count, 4) || count > kUnloadCapacity) return true;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t handle = 0;
        if (!Read(set + kUnloadList + 4 * i, &handle, 4)) return true;
        if (handle == self) continue;
        const uint32_t stream = StreamFor(manager, handle);
        if (!stream) continue;
        uint32_t flags = 0, refs = 0;
        int row = -1;
        const auto verdict = Classify(stream, flags, refs, row);
        if (verdict != Verdict::Unloaded && verdict != Verdict::Candidate) return true;
    }
    return false;
}

Pending& Track(uint32_t set, uint32_t handle, DWORD now) {
    Pending* oldest = &pending[0];
    for (auto& entry : pending) {
        if (entry.set == set && entry.handle == handle) {
            // not seen for a second: a new stall, not the same one
            if (now - entry.last > 1000) entry.first = now;
            entry.last = now;
            return entry;
        }
        if (entry.last < oldest->last) oldest = &entry;
    }
    *oldest = Pending{set, handle, now, now};
    return *oldest;
}

void Forget(uint32_t set, uint32_t handle) {
    for (auto& entry : pending)
        if (entry.set == set && entry.handle == handle) entry = Pending{};
}

uint32_t SetIndex(uint32_t set) {
    uint32_t manager = 0, sets = 0, count = 0;
    if (!Read(base + 0x1C1C0BC, &manager, 4) || !manager || !Read(manager + 0x10, &sets, 4) ||
        !Read(manager + 0x14, &count, 4) || count > 16) return 0xFF;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry = 0;
        if (Read(sets + 4 * i, &entry, 4) && entry == set) return i;
    }
    return 0xFF;
}

// eax = stream record, ecx = handle, esi = set, ebx = stream manager
void OnStatusTest(SafetyHookContext& ctx) {
    InterlockedIncrement(&calls);
    const uint32_t stream = ctx.eax, handle = ctx.ecx, set = ctx.esi, manager = ctx.ebx;
    uint32_t flags = 0, refs = 0;
    int row = -1;
    switch (Classify(stream, flags, refs, row)) {
    case Verdict::Unloaded: return;
    case Verdict::NotLoaded: InterlockedIncrement(&notLoaded); return;
    case Verdict::ExplicitHeld: InterlockedIncrement(&explicitHeld); return;
    case Verdict::NoRefs: InterlockedIncrement(&noRefs); return;
    case Verdict::Foreign: InterlockedIncrement(&foreign); return;
    case Verdict::Candidate: break;
    }
    if (!PoolRootPresent(manager)) { InterlockedIncrement(&poolGone); return; }
    const DWORD now = GetTickCount();
    Pending& entry = Track(set, handle, now);
    if (OthersStillUnloading(set, manager, handle)) { InterlockedIncrement(&othersPending); return; }
    if (now - entry.first < delayMs) { InterlockedIncrement(&delayed); return; }
    const DWORD stuck = now - entry.first;
    Forget(set, handle);
    uint32_t count = 0;
    Read(set + kUnloadCount, &count, 4);
    ctx.eax = static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(unloadedRecord));
    InterlockedIncrement(&healed);
    const LONG slot = InterlockedIncrement(&eventCount) - 1;
    if (slot >= 0 && slot < static_cast<LONG>(std::size(events))) {
        auto& event = events[slot];
        event.tick = now; event.set = set; event.setIndex = SetIndex(set); event.handle = handle;
        Read(stream + kStreamIdentity, &event.identity, 4);
        event.flags = flags; event.refs = refs; event.stuckMs = stuck; event.unloadCount = count; event.row = row;
        InterlockedExchange(&event.ready, 1);
    }
}

// The checkpoint quick restart (GameMainLoop::UpdateQuickRestart 0x00410200, state 1) waits
// until set 0 has nothing left to unload (0x007578E0: +0x99C, +0xC8, +0x240 all zero), but
// nothing runs ProcessPendingLoads any more while it waits. A package caught in the unload
// list when the restart began stayed there, and so did the loading screen (16.10., ch03
// church, slasherclergy, host). From the frame tick: once the status test has been silent for
// a second, pool-held packages are taken off the unload lists the way the set drops a record
// that reads unloaded (0x007580D6: shift the rest down, count - 1).
volatile LONG sweeps = 0, swept = 0, sweepFaults = 0;
LONG sweepCalls = -1;
DWORD callsQuietSince = 0, lastSweep = 0;
struct SweepEvent { uint32_t set, handle, count; int row; };
SweepEvent sweepEvents[16]{};
volatile LONG sweepEventCount = 0;
LONG sweepEventsWritten = 0;
bool TakeOff(uint32_t set, uint32_t index) {
    __try {
        auto* list = reinterpret_cast<uint32_t*>(set + kUnloadList);
        auto* count = reinterpret_cast<uint32_t*>(set + kUnloadCount);
        for (uint32_t i = index; i + 1 < *count; ++i) list[i] = list[i + 1];
        --*count;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&sweepFaults);
        return false;
    }
}
void FrameSweep() {
    if (!installed) return;
    const DWORD now = GetTickCount();
    const LONG seen = InterlockedCompareExchange(&calls, 0, 0);
    if (seen != sweepCalls) { sweepCalls = seen; callsQuietSince = now; return; }
    if (now - callsQuietSince < 1000 || now - lastSweep < 250) return;
    lastSweep = now;
    uint32_t setManager = 0, sets = 0, setCount = 0, manager = 0;
    if (!Read(base + 0x1C1C0BC, &setManager, 4) || !setManager || !Read(setManager + 0x10, &sets, 4) || !sets ||
        !Read(setManager + 0x14, &setCount, 4) || setCount > 16) return;
    if (!Read(base + 0x1C1C098, &manager, 4) || !manager || !PoolRootPresent(manager)) return;
    InterlockedIncrement(&sweeps);
    for (uint32_t s = 0; s < setCount; ++s) {
        uint32_t set = 0, count = 0;
        if (!Read(sets + 4 * s, &set, 4) || !set || !Read(set + kUnloadCount, &count, 4) || !count || count > kUnloadCapacity) continue;
        for (uint32_t i = count; i-- > 0;) {
            uint32_t handle = 0, flags = 0, refs = 0;
            int row = -1;
            if (!Read(set + kUnloadList + 4 * i, &handle, 4)) break;
            const uint32_t stream = StreamFor(manager, handle);
            if (!stream || Classify(stream, flags, refs, row) != Verdict::Candidate || !TakeOff(set, i)) continue;
            InterlockedIncrement(&swept);
            const LONG slot = InterlockedIncrement(&sweepEventCount) - 1;
            if (slot >= 0 && slot < static_cast<LONG>(std::size(sweepEvents))) sweepEvents[slot] = SweepEvent{set, handle, count, row};
        }
    }
}

bool Install(FILE* log, const std::string& ini) {
    const bool enabled = ConfigInt(ini, "DS2Randomizer", "ZoneUnloadHeal", 1, 0, 1) != 0;
    delayMs = static_cast<DWORD>(ConfigInt(ini, "DS2Randomizer", "ZoneUnloadHealDelayMs", 250, 0, 5000));
    if (!enabled) { std::fprintf(log, "zone_unload heal off (ZoneUnloadHeal=0)\n"); return false; }
    for (unsigned i = 0; i < std::size(ds2::kStreamRequirements); ++i)
        if (ds2::kStreamRequirements[i].identity && std::strstr(ds2::kStreamRequirements[i].name, "\\char_str\\"))
            characterRows[characterRowCount++] = i;
    unsigned char prologue[sizeof(kPrologue)]{}, site[sizeof(kSite)]{};
    const bool prologueOk = Read(base + kFunctionRva, prologue, sizeof(prologue)) && !std::memcmp(prologue, kPrologue, sizeof(prologue));
    const bool siteOk = Read(base + kStatusTestRva - kSiteOffset, site, sizeof(site)) && !std::memcmp(site, kSite, sizeof(site));
    if (!prologueOk || !siteOk) {
        std::fprintf(log, "zone_unload signature mismatch prologue=%d site=%d at %08X; zone switches untouched\n",
                     prologueOk ? 1 : 0, siteOk ? 1 : 0, static_cast<unsigned>(base + kStatusTestRva));
        return false;
    }
    hook = safetyhook::create_mid(reinterpret_cast<void*>(base + kStatusTestRva), OnStatusTest);
    if (!hook) { std::fprintf(log, "zone_unload hook failed; zone switches untouched\n"); return false; }
    installed = true;
    std::fprintf(log, "zone_unload heal installed at %08X delay_ms=%lu character_packages=%u\n",
                 static_cast<unsigned>(base + kStatusTestRva), delayMs, characterRowCount);
    return true;
}

// How many of the pool's character packages a stream set has claimed right now.
// Nonzero means the level won the load race; the next switch that drops one
// of them would have stalled without the heal.
void SampleClaims(FILE* log) {
    static DWORD lastSample = 0;
    static int lastOwned = -1;
    const DWORD now = GetTickCount();
    if (now - lastSample < 5000) return;
    lastSample = now;
    uint32_t manager = 0;
    if (!Read(base + 0x1C1C098, &manager, 4) || !manager || !PoolRootPresent(manager)) {
        if (lastOwned > 0) std::fprintf(log, "zone_race pool_gone\n");
        lastOwned = -1;
        return;
    }
    const auto states = ds2::native::inspectStreamStates(manager, Read);
    if (states.result != ds2::native::StreamStates::Stable) return;
    int owned = 0;
    std::string names;
    for (const auto& stream : states.entries) {
        if (stream.status != 3 || !(stream.flags & kExplicitRef) || !stream.dependencyRefs) continue;
        const int row = PoolPackageRow(stream.pointer);
        if (row < 0) continue;
        ++owned;
        if (names.size() < 300) {
            const char* name = std::strrchr(ds2::kStreamRequirements[row].name, '\\');
            names += names.empty() ? "" : ",";
            names += name ? name + 1 : ds2::kStreamRequirements[row].name;
        }
    }
    if (owned != lastOwned)
        std::fprintf(log, "zone_race claimed_by_level=%d packages=%s\n", owned, names.empty() ? "-" : names.c_str());
    lastOwned = owned;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    SampleClaims(log);
    while (eventsWritten < static_cast<LONG>(std::size(events)) &&
           InterlockedCompareExchange(&events[eventsWritten].ready, 1, 1) == 1) {
        const auto& e = events[eventsWritten++];
        std::fprintf(log, "zone_unload healed tick=%lu set=%08X set_index=%u handle=%08X package=%s identity=%08X flags=%08X refs=%u stuck_ms=%u unload_list=%u\n",
                     e.tick, e.set, e.setIndex, e.handle, e.row >= 0 ? ds2::kStreamRequirements[e.row].name : "?",
                     e.identity, e.flags, e.refs, e.stuckMs, e.unloadCount);
    }
    // Written by the frame tick before the count moves on; a slot is only read once the count is past it.
    const LONG sweepCount = std::min<LONG>(InterlockedCompareExchange(&sweepEventCount, 0, 0), static_cast<LONG>(std::size(sweepEvents)));
    while (sweepEventsWritten < sweepCount) {
        const auto& e = sweepEvents[sweepEventsWritten++];
        std::fprintf(log, "zone_unload swept set=%08X handle=%08X package=%s unload_list=%u (the set was not processing its unloads)\n",
                     e.set, e.handle, e.row >= 0 ? ds2::kStreamRequirements[e.row].name : "?", e.count);
    }
    static LONG lastSwept = 0, lastSweepFaults = 0;
    const LONG sweptNow = InterlockedCompareExchange(&swept, 0, 0), faultsNow = InterlockedCompareExchange(&sweepFaults, 0, 0);
    if (sweptNow != lastSwept || faultsNow != lastSweepFaults) {
        std::fprintf(log, "zone_unload sweeps=%ld swept=%ld faults=%ld\n", InterlockedCompareExchange(&sweeps, 0, 0), sweptNow, faultsNow);
        lastSwept = sweptNow;
        lastSweepFaults = faultsNow;
    }
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&calls, 0, 0);
    static DWORD lastTick = 0;
    const DWORD tick = GetTickCount();
    if (now == last || tick - lastTick < 10000) return;
    last = now; lastTick = tick;
    std::fprintf(log, "zone_unload checks=%ld healed=%ld not_loaded=%ld explicit_held=%ld no_refs=%ld foreign=%ld pool_gone=%ld others_unloading=%ld delayed=%ld events_dropped=%ld\n",
                 now, InterlockedCompareExchange(&healed, 0, 0), InterlockedCompareExchange(&notLoaded, 0, 0),
                 InterlockedCompareExchange(&explicitHeld, 0, 0), InterlockedCompareExchange(&noRefs, 0, 0),
                 InterlockedCompareExchange(&foreign, 0, 0), InterlockedCompareExchange(&poolGone, 0, 0),
                 InterlockedCompareExchange(&othersPending, 0, 0), InterlockedCompareExchange(&delayed, 0, 0),
                 std::max<LONG>(0, InterlockedCompareExchange(&eventCount, 0, 0) - static_cast<LONG>(std::size(events))));
}

}  // namespace zone_unload
