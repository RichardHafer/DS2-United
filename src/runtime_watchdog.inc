// Dead Space 2 United by Richard Hafer.
// Watchdog over the enemies the randomizer created. Include at file scope.
//
// A replaced enemy that never dies can hold a room closed. Each one is tracked
// from creation; one that outlives the timeout and is still verifiably the same
// object is killed through the engine's own zero-health path.
//
// Two rules shape the design.
//
// A recorded pointer is not trusted. An actor that died is freed and its memory
// reused, so every entry carries the actor's vtable and its entity GUID and both
// must still match before anything happens.
//
// The kill never runs on the logging worker. Calling into the engine from there
// is the lock-order mistake that produced the infinite loading screen, and
// SetHealth is not thread safe against the game's own update. The worker only
// queues; a mid-hook on the state-machine update drains the queue on the thread
// that owns those objects.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include "spawn_watchdog.h"
#include "death_message.h"
#include "../third_party/safetyhook/safetyhook.hpp"

namespace ds2_watchdog_runtime {
using namespace ds2::watchdog;
constexpr size_t kCapacity = 64;
constexpr size_t kQueueCapacity = 8;

// SetHealth, vtable slot +0x20 on both health components. It clamps, consults
// the mode and the flags and runs the engine's own zero path, so the death
// events, the drop and any door counter fire the way they normally would.
constexpr uintptr_t kSetHealthRva = 0xB84C60;
constexpr size_t kSetHealthSlot = 0x20;
constexpr uintptr_t kHealthVtableRva = 0x1764760;
constexpr uintptr_t kDismemberableVtableRva = 0x1764C68;
// StateMachine::Update. Every NPC ticks it, on the thread that owns them.
constexpr uintptr_t kTickRva = 0xFE2CF0;
// The engine's script-message dispatch, 0x00756C60 with the usual base.
//
// A replacement that never became an NPC cannot die, and a room that counts
// deaths then waits forever. Measured in the running game on 2026-09-16: the
// hospital lobby runs a VariableOperator that adds 1 to a variable on
// `hospital_lobby_slasher_dead` and a VariableWatcher that opens the door when
// that variable reaches 5. Five authored slashers carry the message; three
// replacements died, the fourth was the untouched original, and the fifth was a
// pool Lurker with zero state machines. The variable stood at 4 and could not
// move. So the last thing the watchdog does with an actor it has condemned is
// say the one thing the level was waiting to hear.
//
// The message object and the call shape come from two engine callers, 0x0040DE70
// and 0x0040E7B0, not from a guess: twelve bytes of {hash at +0, a flag byte at
// +4 the dispatch saves and restores, a source pointer at +8}, passed __cdecl
// together with an int. The 0x0040E7B0 variant zeroes the source before it
// calls, so a null source is a value the engine produces itself.
constexpr uintptr_t kSendMessageRva = 0x356C60;
// The component stores its owner here, which is what makes the search safe.
constexpr uint32_t kComponentOwner = 0x10;
// How far into an actor to look for the component pointer. It is not a fixed
// offset: +0x2DC and +0x3A0 on the two classes this was first written against,
// but a live look at six enemies the watchdog had given up on found the pointer
// at +0x8F8, +0x92C and +0x9D8, past the old 0x800 window. Every kill those
// enemies were queued for was dropped as component_missing, which is why a
// stuck quarantine stayed stuck. The search itself is safe at any size: a
// candidate only counts when it carries a health vtable and its owner field
// points back at this actor.
constexpr uint32_t kActorScanBytes = 0x1000;

// `lock` guards the tracker and the proxy table and is only taken by the
// logging worker, which holds it for a whole pass. The game thread only
// takes `queueLock`, held for a few copies and never across a read of game
// memory or a file write: the kill, release and death queues and the
// staging of new entries from the post-create hook.
std::mutex lock;
std::mutex queueLock;
Tracker<kCapacity> tracker;
// Written and read only from the game thread, so it needs no lock and is
// never a poll interval out of date.
TableRegistry<64> liveTables;
bool repairEnabled = true;
volatile LONG machineTicks = 0, stale = 0, repaired = 0, repairNoDonor = 0;
bool enabled = false;
bool killEnabled = false;
uint32_t timeoutMs = 90000;
uintptr_t moduleBase = 0;
safetyhook::MidHook tickHook;
void OnTick(SafetyHookContext&);
extern void (*gameThreadWork)();   // below
extern bool driverOnly;

struct Pending { uint32_t actor = 0, vtable = 0; Guid guid{}; };
Pending queue[kQueueCapacity]{};
volatile LONG queued = 0;
volatile LONG tracked = 0, expired = 0, released = 0, unreadable = 0;
volatile LONG killed = 0, killSkippedFamily = 0, componentMissing = 0, identityChanged = 0, killFaults = 0;

// The second queue: actors whose scripted death message still has to be said.
// Same rule as the kill queue: the worker only queues, the game thread calls.
bool releaseEnabled = true;
bool sendVerified = false;
struct Stranded { uint32_t actor = 0, vtable = 0; Guid guid{}; bool inVent = false; };

// Death proxies (19.09.2026, a friend's softlocks). The post-create hook
// transplants the source enemy's death message into the replacement's
// attributes. When that fails (the friend's run: 27 not_found, the donor's own
// message nowhere in its components: crawler `turret_crawler_01_dead`, pack
// boy `var_inc_decompression_pack`, several TubeBeetle/PackEnemy donors) the
// replacement dies with a message the room is not waiting for, and a
// quarantine that counts deaths never completes. Nothing caught that before:
// ReleaseScriptedDeath only speaks for actors that could never die.
//
// So a refused transplant registers the actor here with the message the
// source would have sent. The logging worker watches its state machines; the
// first time the dead table shows up, the game thread dispatches that message
// once (registered names only, identity re-checked). An actor that vanishes
// without a witnessed death is dropped, not spoken for: a room left behind
// must not have its door opened from afar.
// A proxy leaves the table when it is handed to the game thread.
struct DeathProxy { uint32_t actor = 0, vtable = 0; Guid guid{}; uint32_t hash = 0; char name[48]{}; bool announced = false; };
constexpr size_t kDeathProxyCapacity = 64;
DeathProxy deathProxies[kDeathProxyCapacity]{};
DeathProxy deathQueue[16]{};
volatile LONG deathQueued = 0;
volatile LONG proxiesRegistered = 0, proxiesSent = 0, proxiesUnregistered = 0, proxiesLost = 0, proxiesFaults = 0, proxiesFull = 0, proxiesTeardown = 0;
volatile LONG proxiesNoDispatch = 0;
bool deathProxyEnabled = true;
// Set by InstallKill once the tick hook is in and the dispatch verified.
// Without both a proxy could never be said and would only fill the table.
volatile LONG proxyReady = 0;

// New entries from the post-create hook wait here for the next poll.
struct TrackRequest { uint32_t actor = 0, vtable = 0; Guid guid{}, selected{}; uint32_t tick = 0, leashMs = 0; bool allowKill = false; };
TrackRequest trackStaging[kCapacity]{};
size_t trackStaged = 0;
DeathProxy proxyStaging[32]{};
size_t proxyStaged = 0;
volatile LONG stagingFull = 0;
Stranded releaseQueue[kQueueCapacity]{};
volatile LONG releaseQueued = 0;
volatile LONG messagesReleased = 0, releaseNoMessage = 0, releaseUnregistered = 0;
volatile LONG releaseIdentityChanged = 0, releaseFaults = 0;

bool ReadAt(uintptr_t address, void* output, size_t bytes) {
    SIZE_T done = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
                                        output, bytes, &done) && done == bytes;
}

// Every object the actor owns, in offset order: a pointer inside the actor's
// first kActorScanBytes to an object whose owner field points back at it.
// The window is read in one call and each distinct pointer once (vtable and
// owner in one read); reading dword by dword cost up to 2048
// ReadProcessMemory calls per actor on the game thread. The same pointer
// is stored at several offsets, and it is visited only once: a caller that
// refuses a write when two components carry the same message refused its
// own single component otherwise (2026-09-10: written=9, refused=15).
// visit(object, vtable) returns false to stop.
template<class Visit>
void ForEachOwnedObject(uint32_t actor, Visit visit) {
    constexpr uint32_t kWords = kActorScanBytes / 4;
    uint32_t window[kWords]{};
    if (!ReadAt(actor, window, sizeof(window))) {
        // runs into an unreadable page: take each readable page on its own
        for (uint32_t offset = 0; offset < kActorScanBytes;) {
            uint32_t chunk = 0x1000 - ((actor + offset) & 0xFFF);
            if (chunk > kActorScanBytes - offset) chunk = kActorScanBytes - offset;
            ReadAt(actor + offset, reinterpret_cast<unsigned char*>(window) + offset, chunk);
            offset += chunk;
        }
    }
    // open addressing set of the pointers already looked at; 0 marks a free slot
    constexpr uint32_t kSlots = kWords * 2;
    uint32_t seen[kSlots]{};
    for (uint32_t i = 0; i < kWords; ++i) {
        const uint32_t candidate = window[i];
        if (candidate < 0x10000 || (candidate & 3)) continue;
        uint32_t slot = ((candidate >> 2) * 2654435761u) % kSlots;
        while (seen[slot] && seen[slot] != candidate) slot = (slot + 1) % kSlots;
        if (seen[slot]) continue;
        seen[slot] = candidate;
        uint32_t head[(kComponentOwner + 4) / 4]{};
        if (!ReadAt(candidate, head, sizeof(head)) || head[0] < 0x10000 ||
            head[kComponentOwner / 4] != actor) continue;
        if (!visit(candidate, head[0])) return;
    }
}

// Every component the actor owns. The four script messages sit on three
// different components, so a search that only knows the health one misses
// two of them.
template<class Sink>
void ForEachComponent(uint32_t actor, Sink sink) {
    ForEachOwnedObject(actor, [&](uint32_t component, uint32_t) { sink(component); return true; });
}

// A component whose vtable is one of the two health vtables and whose owner
// points back at this actor. The back-pointer is what rules out a coincidence.
uint32_t FindHealthComponent(uint32_t actor) {
    const uint32_t health = static_cast<uint32_t>(moduleBase + kHealthVtableRva);
    const uint32_t dismemberable = static_cast<uint32_t>(moduleBase + kDismemberableVtableRva);
    uint32_t found = 0;
    ForEachOwnedObject(actor, [&](uint32_t component, uint32_t vtable) {
        if (vtable != health && vtable != dismemberable) return true;
        found = component;
        return false;
    });
    return found;
}

// printf into a string, for lines collected under `lock` and written after it.
void Appendf(std::string& out, const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length > 0) out.append(line, static_cast<size_t>(length) < sizeof(line) ? static_cast<size_t>(length) : sizeof(line) - 1);
}

// Where an actor stands, for the reports below: entity +0xC0 (x y z), the
// same field the placement readout and the extra spawns use. "(?)" when the
// actor cannot be read. A friend's softlock (21.09.2026, church sanctuary)
// had two Leapers alive after the whole leash and nothing in the log said
// where they were.
template<class Reader>
const char* ActorPositionText(uint32_t actor, Reader& read, char* out, size_t size) {
    float p[3]{};
    if (!actor || !read(actor + 0xC0, p, sizeof(p)) || !(p[0] == p[0]) || p[0] > 1e6f || p[0] < -1e6f) { std::snprintf(out, size, "(?)"); return out; }
    std::snprintf(out, size, "(%.1f %.1f %.1f)", p[0], p[1], p[2]);
    return out;
}

// Walks one actor's machine tree: root at tree+0x04, then children and
// siblings. Bounded and cycle-guarded, because the memory may already be junk.
template<class Reader, class Visit>
void ForEachMachine(uint32_t actor, Reader& read, Visit visit) {
    // The tree is embedded in the actor, not a pointer stored there: every
    // machine's m_pTree equals actor+0x248 exactly. Reading a pointer from that
    // address found the tree's own first field, which is zero, so the walk
    // visited nothing at all.
    const uint32_t tree = actor + machine::kActorTree;
    uint32_t root = 0;
    if (!read(tree + machine::kTreeRoot, &root, 4) || !root) return;
    uint32_t stack[32]{};
    uint32_t seen[32]{};
    unsigned depth = 0, count = 0;
    stack[depth++] = root;
    while (depth) {
        const uint32_t sm = stack[--depth];
        bool repeat = false;
        for (unsigned i = 0; i < count; ++i) repeat = repeat || seen[i] == sm;
        if (repeat || count >= 32) continue;
        seen[count++] = sm;
        visit(sm);
        uint32_t child = 0, next = 0;
        if (read(sm + machine::kChildHead, &child, 4) && child && depth < 32) stack[depth++] = child;
        if (read(sm + machine::kNext, &next, 4) && next && depth < 32) stack[depth++] = next;
    }
}

void Configure(const char* ini, FILE* log) {
    std::lock_guard<std::mutex> guard(lock);
    enabled = GetPrivateProfileIntA("DS2Randomizer", "Watchdog", 0, ini) != 0;
    killEnabled = GetPrivateProfileIntA("DS2Randomizer", "WatchdogKill", 0, ini) != 0;
    repairEnabled = GetPrivateProfileIntA("DS2Randomizer", "WatchdogRepairTables", 1, ini) != 0;
    releaseEnabled = GetPrivateProfileIntA("DS2Randomizer", "WatchdogReleaseDeathMessage", 1, ini) != 0;
    deathProxyEnabled = GetPrivateProfileIntA("DS2Randomizer", "WatchdogDeathProxy", 1, ini) != 0;
    liveTables.Reset();
    const int seconds = GetPrivateProfileIntA("DS2Randomizer", "WatchdogSeconds", 90, ini);
    timeoutMs = static_cast<uint32_t>(seconds < 10 ? 10 : seconds > 3600 ? 3600 : seconds) * 1000u;
    tracker.Reset();
    std::fprintf(log, "watchdog config: enabled=%u kill=%u repair_tables=%u release_death_message=%u "
                      "timeout_s=%u capacity=%zu\n",
                 enabled ? 1u : 0u, killEnabled ? 1u : 0u, repairEnabled ? 1u : 0u,
                 releaseEnabled ? 1u : 0u, timeoutMs / 1000u, kCapacity);
}

bool CheckSignature(FILE* log, const char* name, uintptr_t rva,
                    const unsigned char* expected, size_t size) {
    unsigned char actual[16]{};
    const bool matched = size <= sizeof(actual) && ReadAt(moduleBase + rva, actual, size) &&
                         std::memcmp(actual, expected, size) == 0;
    std::fprintf(log, "watchdog signature: %s rva=%08X %s\n", name,
                 static_cast<unsigned>(rva), matched ? "matched" : "MISMATCH");
    return matched;
}

bool InstallKill(uintptr_t baseAddress, FILE* log) {
    std::lock_guard<std::mutex> guard(lock);
    moduleBase = baseAddress;
    if (tickHook) return false;
    const unsigned char tick[] = {0x83,0xEC,0x08,0x53,0x56,0x8B,0xF1,0x83,
                                  0x4E,0x14,0x01,0x8B,0x4E,0x0C,0x8B,0x56};
    if (!enabled || (!killEnabled && !repairEnabled && !releaseEnabled)) {
        // The game-thread work (queued vent repeats, area extra spawns, the placement
        // key, survival's fallback driver) rides on this tick. With the watchdog off
        // it is installed for that alone (13.10.: Watchdog=0 left the queue undrained).
        if (!gameThreadWork) return false;
        if (!CheckSignature(log, "tick", kTickRva, tick, sizeof(tick))) {
            std::fprintf(log, "game-thread driver: tick signature moved; queued vent repeats, area extra spawns and the placement key stay off\n");
            return false;
        }
        driverOnly = true;
        tickHook = safetyhook::create_mid(reinterpret_cast<void*>(moduleBase + kTickRva), OnTick);
        std::fprintf(log, "watchdog tick %s as the game-thread driver only (watchdog features off)\n", tickHook ? "installed" : "FAILED");
        return false;
    }
    const unsigned char setHealth[] = {0xF3,0x0F,0x10,0x1D,0xD0,0x5E,0xAA,0x01,
                                       0xF3,0x0F,0x10,0x44,0x24,0x04,0x0F,0x57};
    bool verified = CheckSignature(log, "tick", kTickRva, tick, sizeof(tick));
    bool killable = CheckSignature(log, "set health", kSetHealthRva, setHealth, sizeof(setHealth));
    // The strongest check: both vtables must really carry SetHealth in slot +0x20.
    for (const auto vtableRva : {kHealthVtableRva, kDismemberableVtableRva}) {
        uint32_t slot = 0;
        const bool ok = ReadAt(moduleBase + vtableRva + kSetHealthSlot, &slot, 4) &&
                        slot == moduleBase + kSetHealthRva;
        std::fprintf(log, "watchdog vtable: %08X slot+%02zX %s\n",
                     static_cast<unsigned>(moduleBase + vtableRva), kSetHealthSlot,
                     ok ? "carries set health" : "MISMATCH");
        killable = ok && killable;
    }
    if (killEnabled && !killable) {
        std::fprintf(log, "watchdog kill disabled: set-health signatures moved\n");
        killEnabled = false;
    }
    // The dispatch is checked from its sixth byte on. Its first five may already
    // carry one of this plugin's inline hooks (the horde feature and the message
    // trace both sit on that instruction), and reading a jmp there would look
    // like the function had moved. What is checked instead cannot be
    // coincidence: the exact instruction run that loads the message registry,
    // plus the registry address it loads, which has to be the one this plugin
    // already uses.
    const unsigned char sendTail[] = {0x8B,0x0A,0x56,0x8B,0xF1,0x57,0x8B,0x3D};
    unsigned char tail[sizeof(sendTail)]{};
    uint32_t registry = 0;
    sendVerified = ReadAt(moduleBase + kSendMessageRva + 5, tail, sizeof(tail)) &&
                   std::memcmp(tail, sendTail, sizeof(tail)) == 0 &&
                   ReadAt(moduleBase + kSendMessageRva + 0x0D, &registry, 4) &&
                   registry == moduleBase + ds2::kMessageTableRva;
    std::fprintf(log, "watchdog message dispatch rva=%08X %s\n",
                 static_cast<unsigned>(kSendMessageRva),
                 sendVerified ? "matched" : "MISMATCH");
    if (releaseEnabled && !sendVerified) {
        std::fprintf(log, "watchdog death-message release disabled: dispatch moved\n");
        releaseEnabled = false;
    }
    if (!verified) {
        std::fprintf(log, "watchdog disabled: tick signature moved\n");
        killEnabled = false;
        repairEnabled = false;
        releaseEnabled = false;
        return false;
    }
    tickHook = safetyhook::create_mid(reinterpret_cast<void*>(moduleBase + kTickRva), OnTick);
    if (!tickHook) { killEnabled = false; repairEnabled = false; releaseEnabled = false; }
    InterlockedExchange(&proxyReady, deathProxyEnabled && sendVerified && tickHook ? 1 : 0);
    std::fprintf(log, "watchdog tick hook: %s repair=%u kill=%u release=%u death_proxy=%ld\n",
                 tickHook ? "installed" : "FAILED",
                 repairEnabled ? 1u : 0u, killEnabled ? 1u : 0u, releaseEnabled ? 1u : 0u,
                 InterlockedCompareExchange(&proxyReady, 0, 0));
    if (deathProxyEnabled && !proxyReady)
        std::fprintf(log, "watchdog death proxy disabled: %s\n", sendVerified ? "no tick hook" : "dispatch moved");
    return static_cast<bool>(tickHook);
}

// Direct reads guarded by SEH. On the game thread this is far cheaper than
// ReadProcessMemory, and a bad pointer costs an exception instead of a crash.
// Kept in its own function because SEH and C++ objects do not share a frame.
// A state array the engine built holds 4-aligned heap pointers or nulls. Dump 75636 (02.11., arena
// restart): DeadSpaceNPCMainSM's array 2D888C98 had been freed and refilled with text, state 3 read
// 0x1C and state 2 0x402B349A; the update read [0x1C+0xC]. Every state up to the current one is
// checked, so a refilled block is caught even when the current entry still reads.
inline bool PlausiblePointer(uint32_t p) { return p >= 0x10000u && (p & 3u) == 0 && p < 0xC0000000u; }
__declspec(noinline) bool ArrayPlausible(uint32_t array, uint32_t index) {
    __try {
        // only up to the current state: the array's length is not stored here, past it lies other memory
        for (uint32_t i = 0; i <= index; ++i) {
            const uint32_t entry = *reinterpret_cast<const uint32_t*>(array + i * 4);
            if (!entry) continue;
            if (!PlausiblePointer(entry)) return false;
            const uint32_t transitions = *reinterpret_cast<const uint32_t*>(entry + machine::kStateTransArr);
            const uint32_t update = *reinterpret_cast<const uint32_t*>(entry + machine::kStateUpdateMessage);
            if (!PlausiblePointer(transitions) || !PlausiblePointer(update)) return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool ProbeTable(uint32_t sm, uint32_t& tableId, uint32_t& array, uint32_t& rank) {
    __try {
        tableId = *reinterpret_cast<const uint32_t*>(sm + machine::kTableId);
        array = *reinterpret_cast<const uint32_t*>(sm + machine::kStateArr);
        rank = *reinterpret_cast<const uint32_t*>(sm + machine::kAllocNumber);
        const uint32_t index = *reinterpret_cast<const uint32_t*>(sm + machine::kCurState);
        if (!array || index > 256) return false;
        if (!PlausiblePointer(array) || !ArrayPlausible(array, index)) return false;
        const uint32_t entry = *reinterpret_cast<const uint32_t*>(array + index * 4);
        if (!entry) return false;
        const uint32_t update = *reinterpret_cast<const uint32_t*>(entry + machine::kStateUpdateMessage);
        const uint32_t transitions = *reinterpret_cast<const uint32_t*>(entry + machine::kStateTransArr);
        if (!update || !transitions) return false;
        // The update walks both lists, so both must be readable, not just set,
        // and so must their first element: it reads message +0/+4 and
        // transition +0/+8 (dump 39300, 09.10.: NPCGhostSM whose update list
        // pointed at code, *update readable, [*update+4] not).
        const uint32_t message = *reinterpret_cast<const uint32_t*>(update);
        const uint32_t transition = *reinterpret_cast<const uint32_t*>(transitions);
        if (message) { (void)*reinterpret_cast<const uint32_t*>(message); (void)*reinterpret_cast<const uint32_t*>(message + 4); }
        if (transition) { (void)*reinterpret_cast<const uint32_t*>(transition); (void)*reinterpret_cast<const uint32_t*>(transition + 8); }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool PokeArray(uint32_t sm, uint32_t array) {
    __try {
        *reinterpret_cast<uint32_t*>(sm + machine::kStateArr) = array;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


// Runs at the top of StateMachine::Update, before it touches its table. A poll
// once a second was too late: the game rebuilds the tables and ticks the stale
// machines long before the worker looks again. This is the only place where
// the check cannot lose that race.
// Do not wait for the table to break. A machine whose class has already been
// seen on a newer table is from the previous generation, and it is migrated
// while the old block is still mapped. Waiting for the probe to fail lost the
// race every time: the free happens on another thread inside the same call.
// A machine whose current state the update would fault on, with no newer table
// to move to, gets a table of empty states: every list it walks is empty, so
// the update does nothing and the machine idles until its owner goes.
struct IdleState { uint32_t transitions, name, enter, update, exit; };
const uint32_t kNoEntries = 0;
const char kIdleName[] = "idle";
const IdleState kIdleState = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&kNoEntries)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(kIdleName)),
                              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&kNoEntries)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&kNoEntries)),
                              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&kNoEntries))};
uint32_t idleArray[257]{};
volatile LONG idled = 0;
struct IdledNote { uint32_t sm, tableId; };
IdledNote idledNotes[8]{};
// True when the update would fault right away: no array, or the current state's
// lists or their first elements unreadable. A null state is the game's own case.
__declspec(noinline) bool WouldFault(uint32_t sm) {
    __try {
        const uint32_t array = *reinterpret_cast<const uint32_t*>(sm + machine::kStateArr);
        const uint32_t index = *reinterpret_cast<const uint32_t*>(sm + machine::kCurState);
        if (!array || index > 256) return true;
        if (array == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(idleArray))) return false;
        if (!PlausiblePointer(array) || !ArrayPlausible(array, index)) return true;
        const uint32_t entry = *reinterpret_cast<const uint32_t*>(array + index * 4);
        if (!entry) return false;
        const uint32_t update = *reinterpret_cast<const uint32_t*>(entry + machine::kStateUpdateMessage);
        const uint32_t transitions = *reinterpret_cast<const uint32_t*>(entry + machine::kStateTransArr);
        const uint32_t message = *reinterpret_cast<const uint32_t*>(update);
        const uint32_t transition = *reinterpret_cast<const uint32_t*>(transitions);
        if (message) { (void)*reinterpret_cast<const uint32_t*>(message); (void)*reinterpret_cast<const uint32_t*>(message + 4); }
        if (transition) { (void)*reinterpret_cast<const uint32_t*>(transition); (void)*reinterpret_cast<const uint32_t*>(transition + 8); }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}
__declspec(noinline) bool Idle(uint32_t sm) {
    if (!idleArray[0])
        for (auto& slot : idleArray) slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&kIdleState));
    __try {
        uint32_t& index = *reinterpret_cast<uint32_t*>(sm + machine::kCurState);
        if (index > 256) index = 0;
        *reinterpret_cast<uint32_t*>(sm + machine::kStateArr) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(idleArray));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void IdleIfBroken(uint32_t sm, uint32_t tableId) {
    if (!repairEnabled || !WouldFault(sm) || !Idle(sm)) return;
    const LONG n = InterlockedIncrement(&idled) - 1;
    if (n < 8) idledNotes[n] = IdledNote{sm, tableId};
}

// Enemies that live through a level teardown (our resident pool's, 09.09. and dump 75636) keep
// machines whose tables the teardown freed; the block is refilled by the next load, possibly while
// such a machine ticks, so a check at the top of the update can lose that race. A machine allocated
// before the last teardown (m_allocNumber is handed out in order: dump 14224, old <= 642, new >= 645)
// that still ticks afterwards and belongs to a DeadSpaceNPC is parked on the idle table at once,
// without reading its table. NoteTeardown is called by the survival director and the co-op restart.
volatile LONG maxRankSeen = 0, teardownRank = 0, teardowns = 0, survivorsParked = 0;
struct ParkedNote { uint32_t sm, tableId, rank, actor; };
ParkedNote parkedNotes[8]{};
char teardownWhy[64] = "";
void NoteTeardown(const char* why) {
    const LONG mark = InterlockedCompareExchange(&maxRankSeen, 0, 0);
    if (mark > InterlockedCompareExchange(&teardownRank, 0, 0)) InterlockedExchange(&teardownRank, mark);
    InterlockedIncrement(&teardowns);
    std::snprintf(teardownWhy, sizeof(teardownWhy), "%s", why ? why : "?");
}
// A mark set for a restart that never came (no loading screen followed) would park the level's own
// enemies on their next tick: the director takes it back. Machines already parked stay parked.
volatile LONG teardownMarksDropped = 0;
void DropTeardownMark(LONG mark) {
    if (InterlockedCompareExchange(&teardownRank, 0, mark) == mark) InterlockedIncrement(&teardownMarksDropped);
}
LONG TeardownMark() { return InterlockedCompareExchange(&teardownRank, 0, 0); }
// The machine's owner as a DeadSpaceNPC: the tree root sits at actor+0x248 (machine::kActorTree), the
// NetReflectable at actor+0x5C answers its class rep through a "mov eax,imm32; ret" getter (+0x78).
constexpr uint32_t kNpcClassRepRva = 0x1CA8328;
__declspec(noinline) uint32_t NpcOwnerOf(uint32_t sm) {
    __try {
        const uint32_t tree = *reinterpret_cast<const uint32_t*>(sm + 0x18);
        if (!PlausiblePointer(tree)) return 0;
        const uint32_t actor = tree - machine::kActorTree;
        const uint32_t vt = *reinterpret_cast<const uint32_t*>(actor + 0x5C);
        if (!PlausiblePointer(vt)) return 0;
        const uint32_t getter = *reinterpret_cast<const uint32_t*>(vt + 0x78);
        if (getter < moduleBase + 0x1000 || getter >= moduleBase + 0x1800000) return 0;
        const unsigned char* g = reinterpret_cast<const unsigned char*>(static_cast<uintptr_t>(getter));
        if (g[0] != 0xB8 || g[5] != 0xC3) return 0;
        return *reinterpret_cast<const uint32_t*>(getter + 1) == moduleBase + kNpcClassRepRva ? actor : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
void OnMachineTick(uint32_t sm) {
    InterlockedIncrement(&machineTicks);
    uint32_t tableId = 0, array = 0, rank = 0;
    const bool usable = ProbeTable(sm, tableId, array, rank);
    if (!tableId) return;
    if (array == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(idleArray))) return;   // already idled
    if (rank < 0x10000000u && static_cast<LONG>(rank) > InterlockedCompareExchange(&maxRankSeen, 0, 0)) InterlockedExchange(&maxRankSeen, static_cast<LONG>(rank));
    const LONG mark = InterlockedCompareExchange(&teardownRank, 0, 0);
    if (repairEnabled && mark && rank && static_cast<LONG>(rank) <= mark) {
        const uint32_t actor = NpcOwnerOf(sm);
        if (actor && Idle(sm)) {
            const LONG n = InterlockedIncrement(&survivorsParked) - 1;
            if (n < 8) parkedNotes[n] = ParkedNote{sm, tableId, rank, actor};
            return;
        }
    }
    const uint32_t live = liveTables.Lookup(tableId);
    const uint32_t liveRank = liveTables.RankOf(tableId);
    if (usable && (!live || rank >= liveRank)) {
        liveTables.Observe(tableId, array, rank);
        return;
    }
    if (!repairEnabled || !live || live == array) {
        if (!usable) { InterlockedIncrement(&repairNoDonor); IdleIfBroken(sm, tableId); }
        return;
    }
    InterlockedIncrement(&stale);
    uint32_t checkId = 0, checkArray = 0, checkRank = 0;
    if (PokeArray(sm, live) && ProbeTable(sm, checkId, checkArray, checkRank)) {
        InterlockedIncrement(&repaired);
    } else {
        PokeArray(sm, array);
        InterlockedIncrement(&repairNoDonor);
        IdleIfBroken(sm, tableId);
    }
}

// Work another module owns but that has to run on this thread. The spawn
// multiplier uses it to space its repeats out in time; this hook is the only
// periodic call the plugin has on the thread that owns the actors.
void (*gameThreadWork)() = nullptr;
bool driverOnly = false;   // the tick runs gameThreadWork alone (Watchdog=0)
// The queued work runs from the frame tick, after the frame's logic, not from
// inside an NPC's StateMachine::Update. Run from there it could spawn, kill or
// tear down actors while that NPC's update was half done; dumps 75636 and
// 51624 crashed in the update right after it (0x013E2D18, the machine's state
// array already freed and reused; 0x012DAF9D, a null vtable). This tick only
// stands in while the frame tick does not run on the actors' thread.
volatile LONG machineThread = 0, machineTickAt = 0, frameWorkAt = 0;
volatile LONG frameWorkRuns = 0, machineWorkRuns = 0;
void RunQueuedWork();
bool FrameDrives() {
    const LONG at = InterlockedCompareExchange(&frameWorkAt, 0, 0);
    return at && GetTickCount() - static_cast<DWORD>(at) < 1000;
}

// The twelve bytes the dispatch expects. Laid out by hand rather than by the
// compiler so the padding after the flag byte is ours and is zero.
#pragma pack(push, 1)
struct ScriptMessagePacket {
    uint32_t hash = 0;
    uint8_t inFlight = 0;      // the dispatch writes 1 here and puts it back
    uint8_t padding[3]{};
    uint32_t source = 0;       // the engine sends null here too, see 0x0040E7B0
};
#pragma pack(pop)
static_assert(sizeof(ScriptMessagePacket) == 12, "the dispatch reads a 12-byte message");

// SEH and C++ objects do not share a frame, so the call lives alone. A listener
// that walks a half-torn-down object costs an exception instead of the session.
__declspec(noinline) bool DispatchMessage(uintptr_t sendAddress, uint32_t hash) {
    using Send = int(__cdecl*)(void*, int);
    ScriptMessagePacket packet;
    packet.hash = hash;
    __try {
        reinterpret_cast<Send>(sendAddress)(&packet, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SetHealth(0) through the slot that was just verified. The identity check
// before it is not a lock: the actor may be torn down by another thread
// between that check and this call.
__declspec(noinline) bool CallSetHealth(uintptr_t setHealth, uint32_t component) {
    using SetHealth = void(__thiscall*)(void*, float);
    __try {
        reinterpret_cast<SetHealth>(setHealth)(reinterpret_cast<void*>(component), 0.0f);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Say, on behalf of an actor that can no longer die, the thing the level was
// waiting to hear. Game thread only.
//
// The message is deliberately not cleared afterwards. Two actors can share one
// authored attribute block (spawn 14 and spawn 15 of the hospital lobby both
// pointed at header 243873C0 on 2026-09-16), so clearing one would silence the
// other, which is the very failure this exists to undo. Each actor is queued at
// most once, because the tracker retires it in the same breath, and an actor
// that reaches here has already been condemned as unable to build a single
// state machine or as never having left its vent.
void ReleaseScriptedDeath(const Stranded& target) {
    uint32_t vtable = 0;
    Guid guid{};
    if (!ReadAt(target.actor, &vtable, 4) || vtable != target.vtable ||
        !ReadAt(target.actor + 0x24, guid.data(), sizeof(guid)) || guid != target.guid) {
        InterlockedIncrement(&releaseIdentityChanged);
        return;
    }
    auto readWord = [](uint32_t address, void* out, size_t size) {
        return ReadAt(address, out, size);
    };
    unsigned sent = 0;
    ForEachComponent(target.actor, [&](uint32_t component) {
        uint32_t data = 0, descriptor = 0;
        if (!ReadAt(component + ds2::kComponentAttributes, &data, 4) ||
            data <= ds2::kAttributesBias)
            return;
        if (!ReadAt(data - ds2::kAttributesBias, &descriptor, 4)) return;
        for (const auto& field : ds2::kDeathFields) {
            if (descriptor != moduleBase + field.descriptorRva) continue;
            uint32_t hash = 0;
            if (!ReadAt(data + field.offset, &hash, 4) || !hash) continue;
            // An unregistered name is the one thing never to hand the dispatch:
            // it walks the bucket chain with no branch for "not found" and ends
            // up incrementing null. Registered or nothing.
            if (!ds2::messageEntry(static_cast<uint32_t>(moduleBase), hash, readWord)) {
                InterlockedIncrement(&releaseUnregistered);
                continue;
            }
            if (DispatchMessage(moduleBase + kSendMessageRva, hash)) ++sent;
            else InterlockedIncrement(&releaseFaults);
        }
    });
    if (sent) InterlockedExchangeAdd(&messagesReleased, static_cast<LONG>(sent));
    else InterlockedIncrement(&releaseNoMessage);
}

// From the post-create hook: the replacement `actor` could not be given the
// source's death message; say `hash` (`name`) for it when it dies.
// Staged here and entered into the table by the next poll.
void ProxyDeath(uint32_t actor, uint32_t hash, const char* name) {
    if (!enabled || !deathProxyEnabled || !actor || !hash) return;
    if (!InterlockedCompareExchange(&proxyReady, 0, 0)) { InterlockedIncrement(&proxiesNoDispatch); return; }
    DeathProxy proxy;
    if (!ReadAt(actor, &proxy.vtable, 4) || !ReadAt(actor + 0x24, proxy.guid.data(), sizeof(proxy.guid))) return;
    proxy.actor = actor;
    proxy.hash = hash;
    std::snprintf(proxy.name, sizeof(proxy.name), "%s", name ? name : "");
    std::lock_guard<std::mutex> guard(queueLock);
    if (proxyStaged >= std::size(proxyStaging)) { InterlockedIncrement(&stagingFull); return; }
    proxyStaging[proxyStaged++] = proxy;
}

// Worker, under `lock`: a staged proxy takes its slot in the table.
void EnterDeathProxy(const DeathProxy& proxy) {
    DeathProxy* slot = nullptr;
    for (auto& p : deathProxies) if (p.actor == proxy.actor) { slot = &p; break; }
    if (!slot) for (auto& p : deathProxies) if (!p.actor) { slot = &p; break; }
    if (!slot) { InterlockedIncrement(&proxiesFull); return; }
    *slot = proxy;
    InterlockedIncrement(&proxiesRegistered);
}

// Logging worker, under `lock`: has a proxied actor died? Then hand it to the
// game thread. Gone without a witnessed death: dropped. Lines go to `out`.
template<class Reader>
void PollDeathProxies(std::string& out, Reader& read) {
    // Classify first, report second. A checkpoint restart tears the level's
    // enemies down in one go: several proxied actors vanish in the same pass and
    // the rest sit in the dead table on their way out. Their messages would land
    // in the level being rebuilt (friend log 20, 22.09.2026: 14 said, 16 lost in
    // one second). Real multi-kills lose no actor, so two lost in one pass is the
    // teardown signature.
    size_t lostNow = 0;
    bool dying[std::size(deathProxies)]{};
    for (size_t i = 0; i < std::size(deathProxies); ++i) {
        auto& p = deathProxies[i];
        if (!p.actor) continue;
        if (!p.announced) {
            // the post-create hook has no log handle; the worker announces
            p.announced = true;
            Appendf(out, "watchdog death_proxy registered actor=%08X entity=%08X-%08X-%08X-%08X message=%s (%08X): transplant refused, watchdog will say it\n",
                         p.actor, p.guid[0], p.guid[1], p.guid[2], p.guid[3], p.name, p.hash);
        }
        uint32_t vtable = 0;
        Guid guid{};
        if (!read(p.actor, &vtable, 4) || vtable != p.vtable ||
            !read(p.actor + 0x24, guid.data(), sizeof(guid)) || guid != p.guid) {
            InterlockedIncrement(&proxiesLost);
            Appendf(out, "watchdog death_proxy lost actor=%08X message=%s (actor gone before a death was seen)\n", p.actor, p.name);
            p = DeathProxy{};
            ++lostNow;
            continue;
        }
        ForEachMachine(p.actor, read, [&](uint32_t sm) {
            uint32_t id = 0;
            if (read(sm + machine::kTableId, &id, 4) && id == machine::kDeadTable) dying[i] = true;
        });
    }
    if (lostNow >= 2) {
        // whatever still waits for the game thread belongs to the same teardown
        LONG dropped = 0;
        {
            std::lock_guard<std::mutex> guard(queueLock);
            dropped = InterlockedExchange(&deathQueued, 0);
        }
        for (size_t i = 0; i < std::size(deathProxies); ++i) {
            if (!dying[i]) continue;
            deathProxies[i] = DeathProxy{};
            ++dropped;
        }
        InterlockedExchangeAdd(&proxiesTeardown, dropped);
        if (dropped)
            Appendf(out, "watchdog death_proxy teardown lost=%zu dropped=%ld (level restart, deaths not said)\n",
                         lostNow, dropped);
        return;
    }
    for (size_t i = 0; i < std::size(deathProxies); ++i) {
        auto& p = deathProxies[i];
        if (!dying[i]) continue;
        {
            std::lock_guard<std::mutex> guard(queueLock);
            const LONG count = InterlockedCompareExchange(&deathQueued, 0, 0);
            if (count >= static_cast<LONG>(std::size(deathQueue))) continue;   // next second
            deathQueue[count] = p;
            InterlockedExchange(&deathQueued, count + 1);
        }
        Appendf(out, "watchdog death_proxy dead actor=%08X entity=%08X-%08X-%08X-%08X: saying %s (%08X) for it\n",
                p.actor, p.guid[0], p.guid[1], p.guid[2], p.guid[3], p.name, p.hash);
        p = DeathProxy{};
    }
}

// Game thread: one proxied death per tick.
void SayProxiedDeath(const DeathProxy& p) {
    uint32_t vtable = 0;
    Guid guid{};
    // identity may legitimately be gone by now (the corpse was cleaned up in
    // the same second): the death was witnessed, the message is still owed
    if (ReadAt(p.actor, &vtable, 4) && vtable == p.vtable && ReadAt(p.actor + 0x24, guid.data(), sizeof(guid)) && guid != p.guid) {
        InterlockedIncrement(&proxiesLost);
        return;
    }
    auto readWord = [](uint32_t address, void* out, size_t size) { return ReadAt(address, out, size); };
    if (!ds2::messageEntry(static_cast<uint32_t>(moduleBase), p.hash, readWord)) {
        InterlockedIncrement(&proxiesUnregistered);
        return;
    }
    if (DispatchMessage(moduleBase + kSendMessageRva, p.hash)) InterlockedIncrement(&proxiesSent);
    else InterlockedIncrement(&proxiesFaults);
}

// Game thread, on every state machine update. With nothing queued it only runs
// the table check and the game-thread work; each queue is one atomic read.
void OnTick(SafetyHookContext& context) {
    InterlockedExchange(&machineThread, static_cast<LONG>(GetCurrentThreadId()));
    InterlockedExchange(&machineTickAt, static_cast<LONG>(GetTickCount()));
    if (!driverOnly) OnMachineTick(context.ecx);
    if (FrameDrives()) return;
    InterlockedIncrement(&machineWorkRuns);
    RunQueuedWork();
}

// From the frame tick: the actors' thread, outside every state machine update.
// Only while machines tick, as before; a loading screen gets none of this work.
void FrameWork() {
    if (!tickHook || static_cast<DWORD>(InterlockedCompareExchange(&machineThread, 0, 0)) != GetCurrentThreadId()) return;
    if (GetTickCount() - static_cast<DWORD>(InterlockedCompareExchange(&machineTickAt, 0, 0)) > 250) return;
    InterlockedExchange(&frameWorkAt, static_cast<LONG>(GetTickCount() | 1));
    InterlockedIncrement(&frameWorkRuns);
    RunQueuedWork();
}

void RunQueuedWork() {
    if (gameThreadWork) gameThreadWork();
    if (driverOnly) return;
    // One release per tick. The dispatch runs every listener of the message, and
    // a quarantine's listener can open a door and start a cutscene, so two in one
    // tick is more than this hook has any business doing.
    if (InterlockedCompareExchange(&releaseQueued, 0, 0) > 0) {
        Stranded stranded;
        {
            std::lock_guard<std::mutex> guard(queueLock);
            const LONG count = InterlockedCompareExchange(&releaseQueued, 0, 0);
            if (count > 0) {
                stranded = releaseQueue[count - 1];
                InterlockedExchange(&releaseQueued, count - 1);
            }
        }
        if (stranded.actor) ReleaseScriptedDeath(stranded);
    }
    if (sendVerified && InterlockedCompareExchange(&deathQueued, 0, 0) > 0) {
        DeathProxy proxy;
        {
            std::lock_guard<std::mutex> guard(queueLock);
            const LONG count = InterlockedCompareExchange(&deathQueued, 0, 0);
            if (count > 0) {
                proxy = deathQueue[count - 1];
                InterlockedExchange(&deathQueued, count - 1);
            }
        }
        if (proxy.actor) SayProxiedDeath(proxy);
    }
    if (InterlockedCompareExchange(&queued, 0, 0) <= 0) return;
    Pending target;
    {
        std::lock_guard<std::mutex> guard(queueLock);
        const LONG count = InterlockedCompareExchange(&queued, 0, 0);
        if (count <= 0) return;
        target = queue[count - 1];
        InterlockedExchange(&queued, count - 1);
    }
    // Re-check identity here too: the queue was filled up to a second ago and
    // this is the last moment before the engine is called.
    uint32_t vtable = 0;
    Guid guid{};
    if (!ReadAt(target.actor, &vtable, 4) || vtable != target.vtable ||
        !ReadAt(target.actor + 0x24, guid.data(), sizeof(guid)) || guid != target.guid) {
        InterlockedIncrement(&identityChanged);
        return;
    }
    const uint32_t component = FindHealthComponent(target.actor);
    if (!component) { InterlockedIncrement(&componentMissing); return; }
    uint32_t componentVtable = 0, slot = 0;
    if (!ReadAt(component, &componentVtable, 4) ||
        !ReadAt(componentVtable + kSetHealthSlot, &slot, 4) ||
        slot != moduleBase + kSetHealthRva) {
        InterlockedIncrement(&componentMissing);
        return;
    }
    InterlockedIncrement(CallSetHealth(slot, component) ? &killed : &killFaults);
}

// Called from the post-create hook for an actor the randomizer replaced.
// allowKill is decided there, where the donor's family is known.
template<class Reader>
void Track(uint32_t actor, const Guid& selected, bool allowKill, uint32_t leashMs, Reader& read) {
    if (!enabled || !actor) return;
    TrackRequest request;
    if (!read(actor, &request.vtable, 4) || !read(actor + 0x24, request.guid.data(), sizeof(request.guid))) return;
    request.actor = actor;
    request.selected = selected;
    request.tick = GetTickCount();
    request.leashMs = leashMs;
    request.allowKill = allowKill;
    std::lock_guard<std::mutex> guard(queueLock);
    if (trackStaged >= std::size(trackStaging)) { InterlockedIncrement(&stagingFull); return; }
    trackStaging[trackStaged++] = request;
}

bool WriteAt(uintptr_t address, const void* input, size_t bytes) {
    SIZE_T done = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
                              input, bytes, &done) && done == bytes;
}

// The state a machine is in right now, by name. A vent arrival that never
// leaves its climb-out sits in the same state forever, and that is the
// difference between "the enemy is stuck in the vent" and "the player has not
// walked over there yet", which is all the leash alone can say.
template<class Reader>
unsigned ReadStateName(uint32_t sm, Reader& read, char* out, size_t size) {
    out[0] = 0;
    uint32_t array = 0, index = 0, name = 0;
    if (!read(sm + machine::kStateArr, &array, 4) || !array ||
        !read(sm + machine::kCurState, &index, 4) || index > 4096)
        return 0;
    if (!read(array + index * machine::kStateSize + machine::kStateName, &name, 4) || !name)
        return 0;
    char buffer[32]{};
    if (!read(name, buffer, sizeof(buffer) - 1)) return 0;
    buffer[sizeof(buffer) - 1] = 0;
    for (size_t i = 0; i < sizeof(buffer); ++i) {
        const unsigned char c = static_cast<unsigned char>(buffer[i]);
        if (!c) break;
        if (c < 0x20 || c > 0x7E) { buffer[i] = 0; break; }
    }
    std::snprintf(out, size, "%s", buffer);
    return index;
}

unsigned probeSeconds = 6;
volatile LONG probes = 0;
// A replacement that is still running one of the vent machines this long after
// it spawned never made it out of the vent. Killing it lets the room count a
// death instead of waiting for the full leash, and the donor is reported so the
// next run does not pick it for a vent again.
unsigned ventStuckSeconds = 8;
volatile LONG ventStuck = 0, inert = 0, corpses = 0;
void (*reportBadVentDonor)(const Guid&) = nullptr;

// Logging worker, under `lock`: one pass over the tracked actors. Reads
// only, and queues rather than calling the engine. Lines go to `out`.
template<class Reader>
void PollTracked(std::string& out, Reader& read) {
    // No repair here any more: a once-a-second pass on the logging worker lost
    // the race against the game's own tick. OnMachineTick does it in place.
    const uint32_t now = GetTickCount();
    if (deathProxyEnabled) PollDeathProxies(out, read);
    for (size_t i = 0; i < kCapacity; ++i) {
        switch (tracker.Inspect(i, now, timeoutMs, read)) {
        case Verdict::Empty:
            break;
        case Verdict::Waiting: {
            const auto& entry = tracker.At(i);
            // One probe, at the later of the two timers, so the vent check
            // below actually gets its turn. With the probe at 6 s and the vent
            // verdict at 8 s the probe used to run first, mark the entry as
            // probed and never look again.
            unsigned probeAt = probeSeconds;
            if (ventStuckSeconds > probeAt) probeAt = ventStuckSeconds;
            if (entry.probed || !probeSeconds || now - entry.bornTick < probeAt * 1000u)
                break;
            uint32_t vtable = 0;
            if (!read(entry.actor, &vtable, 4) || vtable != entry.vtable) break;
            char line[256]{};
            size_t used = 0;
            unsigned machines = 0;
            ForEachMachine(entry.actor, read, [&](uint32_t sm) {
                char name[32]{};
                const unsigned index = ReadStateName(sm, read, name, sizeof(name));
                ++machines;
                if (!name[0] || used + 40 >= sizeof(line)) return;
                used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used,
                                                          "%s%s:%u", used ? "," : "", name, index));
            });
            uint32_t ventTable = 0;
            const char* ventName = nullptr;
            ForEachMachine(entry.actor, read, [&](uint32_t sm) {
                uint32_t id = 0;
                if (!read(sm + machine::kTableId, &id, 4)) return;
                if (const char* name = machine::ventTableName(id)) {
                    ventTable = id;
                    ventName = name;
                }
            });
            if (ventName && ventStuckSeconds && (now - entry.bornTick) >= ventStuckSeconds * 1000u) {
                // A suspicion, not a verdict. One reading this early put donors
                // on the blocklist that were merely waiting for the player, and
                // that list is for good. So this only reports; the entry stays
                // tracked, and the leash decides, with the whole timeout behind
                // it, whether the same vent machine is still running.
                InterlockedIncrement(&ventStuck);
                char where[48]{};
                Appendf(out, "watchdog vent_stuck actor=%08X donor=%08X-%08X-%08X-%08X "
                                  "age_s=%u machine=%s table=%08X pos=%s action=watching\n",
                             entry.actor, entry.selected[0], entry.selected[1], entry.selected[2],
                             entry.selected[3], (now - entry.bornTick) / 1000u, ventName, ventTable,
                             ActorPositionText(entry.actor, read, where, sizeof(where)));
                tracker.MarkProbed(i);
                InterlockedIncrement(&probes);
                break;
            }
            Appendf(out, "watchdog state actor=%08X entity=%08X-%08X-%08X-%08X age_s=%u "
                              "machines=%u states=%s\n",
                         entry.actor, entry.guid[0], entry.guid[1], entry.guid[2], entry.guid[3],
                         (now - entry.bornTick) / 1000u, machines, line[0] ? line : "unreadable");
            // An empty tree here says nothing yet. Over one session the same
            // super lurker came up empty once and carried two or three machines
            // five times, so at six seconds this mostly means an enemy that has
            // not been activated: a proximity spawn waits in its vent until the
            // player is near and builds nothing until then. The verdict belongs
            // at the leash, where an empty tree has had the full timeout to fill.
            InterlockedIncrement(&probes);
            tracker.MarkProbed(i);
            break;
        }
        case Verdict::Released:
            InterlockedIncrement(&released);
            tracker.Retire(i);
            break;
        case Verdict::Unreadable:
            InterlockedIncrement(&unreadable);
            tracker.Retire(i);
            break;
        case Verdict::Expired: {
            const auto& entry = tracker.At(i);
            InterlockedIncrement(&expired);
            // A body the player already made looks exactly like an enemy that
            // never engaged: same object, same identity, still there after the
            // leash. Without this the kill counter reports corpses as stranded
            // enemies, which is the difference between "the watchdog removed
            // 45 of your fights" and "it poked 45 bodies".
            bool corpse = false;
            unsigned living = 0;
            const char* stillInVent = nullptr;
            ForEachMachine(entry.actor, read, [&](uint32_t sm) {
                ++living;
                uint32_t id = 0;
                if (!read(sm + machine::kTableId, &id, 4)) return;
                if (id == machine::kDeadTable) corpse = true;
                if (const char* name = machine::ventTableName(id)) stillInVent = name;
            });
            // The vent suspicion from the probe, confirmed: the whole leash has
            // passed and a vent state machine is still running. Only now does
            // the donor go on the blocklist.
            if (!corpse && stillInVent) {
                Appendf(out, "watchdog vent_stuck actor=%08X donor=%08X-%08X-%08X-%08X "
                                  "age_s=%u machine=%s action=donor_blocked\n",
                             entry.actor, entry.selected[0], entry.selected[1], entry.selected[2],
                             entry.selected[3], (now - entry.bornTick) / 1000u, stillInVent);
                if (reportBadVentDonor) reportBadVentDonor(entry.selected);
            }
            if (corpse) {
                InterlockedIncrement(&corpses);
                Appendf(out, "watchdog corpse actor=%08X entity=%08X-%08X-%08X-%08X age_s=%u "
                                  "action=left_alone\n",
                             entry.actor, entry.guid[0], entry.guid[1], entry.guid[2], entry.guid[3],
                             (now - entry.bornTick) / 1000u);
                tracker.Retire(i);
                break;
            }
            // Not one state machine after the whole leash. That is the Super
            // Puker at the triage vent: an actor that exists, holds its place and
            // never becomes an NPC, while a slasher and an infector out of the
            // same vent built four machines each. Nothing in the authored data
            // tells those two cases apart, so the donor is remembered the way a
            // vent-stuck one is and is not offered again. Judged here and not at
            // the probe, because only here has it had the whole timeout.
            if (!living) {
                InterlockedIncrement(&inert);
                Appendf(out, "watchdog inert actor=%08X donor=%08X-%08X-%08X-%08X age_s=%u "
                                  "machines=0 action=donor_blocked\n",
                             entry.actor, entry.selected[0], entry.selected[1], entry.selected[2],
                             entry.selected[3], (now - entry.bornTick) / 1000u);
                if (reportBadVentDonor) reportBadVentDonor(entry.selected);
            }
            // No kill for an actor without state machines: SetHealth(0) on an
            // actor that never became an NPC has no death to run, and the
            // release below already speaks for it (21.09.2026).
            const bool wantKill = entry.allowKill && killEnabled && living;
            bool willKill = false;
            if (wantKill) {
                std::lock_guard<std::mutex> queueGuard(queueLock);
                const LONG count = InterlockedCompareExchange(&queued, 0, 0);
                if (count < static_cast<LONG>(kQueueCapacity)) {
                    queue[count] = {entry.actor, entry.vtable, entry.guid};
                    InterlockedExchange(&queued, count + 1);
                    willKill = true;
                }
            }
            if (wantKill && !willKill) {
                // Queue full: leave it for the next pass rather than losing it.
                // Only for a kill that is wanted: an actor without state
                // machines used to land here too and came back every second
                // without ever reaching the release below.
                break;
            } else if (!entry.allowKill) {
                InterlockedIncrement(&killSkippedFamily);
            }
            // The two cases where the actor is not merely unvisited but unable:
            // no state machine at all after the whole leash, or still running a
            // vent machine it will never leave. Both were already condemned
            // above; this is what makes that verdict cost the room nothing.
            //
            // An enemy with a readable tree that the player has not
            // reached yet is deliberately not here. Saying its death for it
            // would open a door in front of a living enemy, which is a worse
            // bug than the one being fixed.
            //
            // Below the kill decision on purpose, and never next to a queued
            // kill: a full kill queue breaks out above and the actor comes back
            // on the next pass, and a queued kill produces the real death with
            // its message. Either way a release here would say it twice, and a
            // room that counts deaths would count one enemy as two (21.09.2026).
            if (releaseEnabled && sendVerified && (!living || (stillInVent && !willKill))) {
                bool queuedRelease = false;
                {
                    std::lock_guard<std::mutex> queueGuard(queueLock);
                    const LONG waiting = InterlockedCompareExchange(&releaseQueued, 0, 0);
                    if (waiting < static_cast<LONG>(kQueueCapacity)) {
                        releaseQueue[waiting] = {entry.actor, entry.vtable, entry.guid,
                                                 stillInVent != nullptr};
                        InterlockedExchange(&releaseQueued, waiting + 1);
                        queuedRelease = true;
                    }
                }
                if (queuedRelease) {
                    Appendf(out, "watchdog release_queued actor=%08X entity=%08X-%08X-%08X-%08X "
                                      "reason=%s\n",
                                 entry.actor, entry.guid[0], entry.guid[1], entry.guid[2],
                                 entry.guid[3], !living ? "no_state_machines" : "stuck_in_vent");
                }
            }
            char where[48]{};
            Appendf(out,
                "watchdog expired actor=%08X entity=%08X-%08X-%08X-%08X "
                "donor=%08X-%08X-%08X-%08X age_s=%u leash_s=%u pos=%s action=%s\n",
                entry.actor, entry.guid[0], entry.guid[1], entry.guid[2], entry.guid[3],
                entry.selected[0], entry.selected[1], entry.selected[2], entry.selected[3],
                (now - entry.bornTick) / 1000u,
                (entry.timeoutMs ? entry.timeoutMs : timeoutMs) / 1000u,
                ActorPositionText(entry.actor, read, where, sizeof(where)),
                willKill ? "queued_kill" : entry.allowKill ? "report_only" : "family_excluded");
            tracker.MarkReported(i);
            tracker.Retire(i);
            break;
        }
        }
    }
}

// Logging worker. What the game thread staged is entered first, and the
// lines are written once `lock` is released.
template<class Reader>
void Poll(FILE* log, Reader& read) {
    if (!enabled) return;
    TrackRequest requests[std::size(trackStaging)];
    DeathProxy proxies[std::size(proxyStaging)];
    size_t requestCount = 0, proxyCount = 0;
    {
        std::lock_guard<std::mutex> guard(queueLock);
        for (; requestCount < trackStaged; ++requestCount) requests[requestCount] = trackStaging[requestCount];
        for (; proxyCount < proxyStaged; ++proxyCount) proxies[proxyCount] = proxyStaging[proxyCount];
        trackStaged = proxyStaged = 0;
    }
    std::string out;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (size_t r = 0; r < requestCount; ++r) {
            const auto& q = requests[r];
            if (tracker.Track(q.actor, q.vtable, q.guid, q.selected, q.tick, q.allowKill, q.leashMs))
                InterlockedIncrement(&tracked);
        }
        for (size_t p = 0; p < proxyCount; ++p) EnterDeathProxy(proxies[p]);
        PollTracked(out, read);
    }
    if (!out.empty()) std::fputs(out.c_str(), log);
}

void WriteDiagnostics(FILE* log) {
    static LONG lastTotal = -1;
    if (tickHook) {
        static LONG lastFrameRuns = -1;
        const LONG frameRuns = InterlockedCompareExchange(&frameWorkRuns, 0, 0), machineRuns = InterlockedCompareExchange(&machineWorkRuns, 0, 0);
        static LONG lastMachineRuns = 0;
        if (frameRuns / 1000 != lastFrameRuns / 1000 || machineRuns != lastMachineRuns) {
            std::fprintf(log, "game-thread work from frame=%ld from state machine=%ld (the second only while the frame tick is missing)\n", frameRuns, machineRuns);
            lastFrameRuns = frameRuns;
            lastMachineRuns = machineRuns;
        }
    }
    if (!enabled) return;
    // the table sizes under `lock`, the printing after it
    size_t live = 0, dropped = 0;
    LONG pending = 0;
    {
        std::lock_guard<std::mutex> guard(lock);
        live = tracker.Live();
        dropped = tracker.Dropped();
        for (const auto& p : deathProxies) if (p.actor) ++pending;
    }
    const LONG total = InterlockedCompareExchange(&tracked, 0, 0) +
                       InterlockedCompareExchange(&killed, 0, 0) +
                       InterlockedCompareExchange(&stale, 0, 0) +
                       InterlockedCompareExchange(&repaired, 0, 0) +
                       InterlockedCompareExchange(&repairNoDonor, 0, 0) +
                       // A queued kill that never happens is the interesting
                       // case, so let it print a line of its own.
                       InterlockedCompareExchange(&expired, 0, 0) +
                       InterlockedCompareExchange(&componentMissing, 0, 0) +
                       InterlockedCompareExchange(&identityChanged, 0, 0) +
                       InterlockedCompareExchange(&messagesReleased, 0, 0) +
                       InterlockedCompareExchange(&releaseNoMessage, 0, 0) +
                       InterlockedCompareExchange(&releaseUnregistered, 0, 0) +
                       InterlockedCompareExchange(&releaseFaults, 0, 0) +
                       InterlockedCompareExchange(&killFaults, 0, 0) +
                       InterlockedCompareExchange(&stagingFull, 0, 0) +
                       InterlockedCompareExchange(&proxiesNoDispatch, 0, 0) +
                       InterlockedCompareExchange(&survivorsParked, 0, 0) +
                       InterlockedCompareExchange(&idled, 0, 0) +
                       InterlockedCompareExchange(&teardowns, 0, 0);
    if (total == lastTotal) return;
    lastTotal = total;
    std::fprintf(log, "watchdog summary: tracked=%ld expired=%ld killed=%ld family_excluded=%ld "
                      "component_missing=%ld identity_changed=%ld released=%ld unreadable=%ld "
                      "stale=%ld repaired=%ld no_live_table=%ld ticks=%ld live_tables=%zu "
                      "live=%zu dropped=%zu queued=%ld vent_stuck=%ld inert=%ld corpses=%ld "
                      "kill_faults=%ld staging_full=%ld\n",
                 InterlockedCompareExchange(&tracked, 0, 0),
                 InterlockedCompareExchange(&expired, 0, 0),
                 InterlockedCompareExchange(&killed, 0, 0),
                 InterlockedCompareExchange(&killSkippedFamily, 0, 0),
                 InterlockedCompareExchange(&componentMissing, 0, 0),
                 InterlockedCompareExchange(&identityChanged, 0, 0),
                 InterlockedCompareExchange(&released, 0, 0),
                 InterlockedCompareExchange(&unreadable, 0, 0),
                 InterlockedCompareExchange(&stale, 0, 0),
                 InterlockedCompareExchange(&repaired, 0, 0),
                 InterlockedCompareExchange(&repairNoDonor, 0, 0),
                 InterlockedCompareExchange(&machineTicks, 0, 0), liveTables.Count(),
                 live, dropped, InterlockedCompareExchange(&queued, 0, 0),
                 InterlockedCompareExchange(&ventStuck, 0, 0),
                 InterlockedCompareExchange(&inert, 0, 0),
                 InterlockedCompareExchange(&corpses, 0, 0),
                 InterlockedCompareExchange(&killFaults, 0, 0),
                 InterlockedCompareExchange(&stagingFull, 0, 0));
    if (InterlockedCompareExchange(&teardowns, 0, 0) && !InterlockedCompareExchange(&survivorsParked, 0, 0))
        std::fprintf(log, "watchdog teardowns=%ld mark_rank=%ld dropped=%ld last='%s': no enemy machine older than the mark ticked since (none parked)\n",
                     InterlockedCompareExchange(&teardowns, 0, 0), InterlockedCompareExchange(&teardownRank, 0, 0),
                     InterlockedCompareExchange(&teardownMarksDropped, 0, 0), teardownWhy);
    if (const LONG parked = InterlockedCompareExchange(&survivorsParked, 0, 0)) {
        std::fprintf(log, "watchdog survivors_parked=%ld teardowns=%ld mark_rank=%ld last='%s' (enemy machines older than the last level teardown, parked unread)", parked,
                     InterlockedCompareExchange(&teardowns, 0, 0), InterlockedCompareExchange(&teardownRank, 0, 0), teardownWhy);
        for (LONG i = 0; i < parked && i < 8; ++i)
            std::fprintf(log, " [sm %08X table %08X rank %u actor %08X]", parkedNotes[i].sm, parkedNotes[i].tableId, parkedNotes[i].rank, parkedNotes[i].actor);
        std::fprintf(log, "\n");
    }
    if (const LONG idleCount = InterlockedCompareExchange(&idled, 0, 0)) {
        std::fprintf(log, "watchdog idled=%ld (broken state tables with no newer one: update would fault, machine parked on empty states)", idleCount);
        for (LONG i = 0; i < idleCount && i < 8; ++i) std::fprintf(log, " [sm %08X table %08X]", idledNotes[i].sm, idledNotes[i].tableId);
        std::fprintf(log, "\n");
    }
    std::fprintf(log, "watchdog death_proxy registered=%ld sent=%ld unregistered=%ld lost=%ld faults=%ld full=%ld pending=%ld teardown=%ld no_dispatch=%ld\n",
                 InterlockedCompareExchange(&proxiesRegistered, 0, 0), InterlockedCompareExchange(&proxiesSent, 0, 0),
                 InterlockedCompareExchange(&proxiesUnregistered, 0, 0), InterlockedCompareExchange(&proxiesLost, 0, 0),
                 InterlockedCompareExchange(&proxiesFaults, 0, 0), InterlockedCompareExchange(&proxiesFull, 0, 0), pending,
                 InterlockedCompareExchange(&proxiesTeardown, 0, 0), InterlockedCompareExchange(&proxiesNoDispatch, 0, 0));
    std::fprintf(log, "watchdog release: messages=%ld no_message=%ld unregistered=%ld "
                      "identity_changed=%ld faults=%ld queued=%ld\n",
                 InterlockedCompareExchange(&messagesReleased, 0, 0),
                 InterlockedCompareExchange(&releaseNoMessage, 0, 0),
                 InterlockedCompareExchange(&releaseUnregistered, 0, 0),
                 InterlockedCompareExchange(&releaseIdentityChanged, 0, 0),
                 InterlockedCompareExchange(&releaseFaults, 0, 0),
                 InterlockedCompareExchange(&releaseQueued, 0, 0));
}
}  // namespace ds2_watchdog_runtime
