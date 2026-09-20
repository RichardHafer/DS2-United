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
#include <cstdio>
#include <mutex>
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

std::mutex lock;
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

struct Pending { uint32_t actor = 0, vtable = 0; Guid guid{}; };
Pending queue[kQueueCapacity]{};
volatile LONG queued = 0;
volatile LONG tracked = 0, expired = 0, released = 0, unreadable = 0;
volatile LONG killed = 0, killSkippedFamily = 0, componentMissing = 0, identityChanged = 0;

// The second queue: actors whose scripted death message still has to be said.
// Same rule as the kill queue - the worker only queues, the game thread calls.
bool releaseEnabled = true;
bool sendVerified = false;
struct Stranded { uint32_t actor = 0, vtable = 0; Guid guid{}; bool inVent = false; };

// Death proxies (19.09.2026, a friend's softlocks). The post-create hook
// transplants the SOURCE enemy's death message into the replacement's
// attributes; when that fails (the friend's run: 27 not_found, the donor's own
// message is nowhere in its components - crawler `turret_crawler_01_dead`,
// pack boy `var_inc_decompression_pack`, several TubeBeetle/PackEnemy donors)
// the replacement dies with a message the room is not waiting for, and a
// quarantine that counts deaths never completes. Nothing caught that before:
// ReleaseScriptedDeath only speaks for actors that could never die.
//
// So a refused transplant registers the actor here with the message the
// source would have sent. The logging worker watches its state machines; the
// first time the dead table shows up, the game thread dispatches that message
// once (registered names only, identity re-checked). An actor that vanishes
// without a witnessed death is dropped, not spoken for - a room left behind
// must not have its door opened from afar.
struct DeathProxy { uint32_t actor = 0, vtable = 0; Guid guid{}; uint32_t hash = 0; char name[48]{}; bool queued = false, announced = false; };
constexpr size_t kDeathProxyCapacity = 64;
DeathProxy deathProxies[kDeathProxyCapacity]{};
DeathProxy deathQueue[16]{};
volatile LONG deathQueued = 0;
volatile LONG proxiesRegistered = 0, proxiesSent = 0, proxiesUnregistered = 0, proxiesLost = 0, proxiesFaults = 0, proxiesFull = 0;
bool deathProxyEnabled = true;
Stranded releaseQueue[kQueueCapacity]{};
volatile LONG releaseQueued = 0;
volatile LONG messagesReleased = 0, releaseNoMessage = 0, releaseUnregistered = 0;
volatile LONG releaseIdentityChanged = 0, releaseFaults = 0;

bool ReadAt(uintptr_t address, void* output, size_t bytes) {
    SIZE_T done = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
                                        output, bytes, &done) && done == bytes;
}

// A component whose vtable is one of the two health vtables and whose owner
// points back at this actor. The back-pointer is what rules out a coincidence.
// Every component the actor owns, recognised the same way the health component
// is: a pointer inside the actor to an object whose owner field points back.
// The four script messages sit on three different components, so a search that
// only knows the health one misses two of them.
template<class Sink>
void ForEachComponent(uint32_t actor, Sink sink) {
    // The same component pointer is stored at more than one offset inside an
    // actor, so scanning by offset used to hand the caller the same component
    // twice. A caller that refuses a write when two components carry the same
    // message then refused its own single component: the 2026-09-10 death
    // message pass reported written=9 against refused=15 for that reason.
    // De-duplicate by component address, not by offset.
    uint32_t seen[64]{};
    unsigned count = 0;
    for (uint32_t offset = 0; offset < kActorScanBytes; offset += 4) {
        uint32_t candidate = 0, owner = 0, vtable = 0;
        if (!ReadAt(actor + offset, &candidate, 4) || candidate < 0x10000) continue;
        if (!ReadAt(candidate, &vtable, 4) || vtable < 0x10000) continue;
        if (!ReadAt(candidate + kComponentOwner, &owner, 4) || owner != actor) continue;
        bool repeated = false;
        for (unsigned i = 0; i < count; ++i) {
            if (seen[i] == candidate) { repeated = true; break; }
        }
        if (repeated) continue;
        if (count < std::size(seen)) seen[count++] = candidate;
        sink(candidate);
    }
}

uint32_t FindHealthComponent(uint32_t actor) {
    const uint32_t health = static_cast<uint32_t>(moduleBase + kHealthVtableRva);
    const uint32_t dismemberable = static_cast<uint32_t>(moduleBase + kDismemberableVtableRva);
    for (uint32_t offset = 0; offset < kActorScanBytes; offset += 4) {
        uint32_t candidate = 0;
        if (!ReadAt(actor + offset, &candidate, 4) || candidate < 0x10000) continue;
        uint32_t vtable = 0, owner = 0;
        if (!ReadAt(candidate, &vtable, 4)) continue;
        if (vtable != health && vtable != dismemberable) continue;
        if (!ReadAt(candidate + kComponentOwner, &owner, 4) || owner != actor) continue;
        return candidate;
    }
    return 0;
}

// Walks one actor's machine tree: root at tree+0x04, then children and
// siblings. Bounded and cycle-guarded, because the memory may already be junk.
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
    if (!enabled || (!killEnabled && !repairEnabled && !releaseEnabled) || tickHook) return false;
    const unsigned char setHealth[] = {0xF3,0x0F,0x10,0x1D,0xD0,0x5E,0xAA,0x01,
                                       0xF3,0x0F,0x10,0x44,0x24,0x04,0x0F,0x57};
    const unsigned char tick[] = {0x83,0xEC,0x08,0x53,0x56,0x8B,0xF1,0x83,
                                  0x4E,0x14,0x01,0x8B,0x4E,0x0C,0x8B,0x56};
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
    // carry an inline hook of ours - the horde feature and the message trace both
    // sit on that instruction - and reading a jmp there would look like the
    // function had moved. What is checked instead cannot be coincidence: the
    // exact instruction run that loads the message registry, plus the registry
    // address it loads, which has to be the one this plugin already uses.
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
    std::fprintf(log, "watchdog tick hook: %s repair=%u kill=%u release=%u\n",
                 tickHook ? "installed" : "FAILED",
                 repairEnabled ? 1u : 0u, killEnabled ? 1u : 0u, releaseEnabled ? 1u : 0u);
    return static_cast<bool>(tickHook);
}

// Direct reads guarded by SEH. On the game thread this is far cheaper than
// ReadProcessMemory, and a bad pointer costs an exception instead of a crash.
// Kept in its own function because SEH and C++ objects do not share a frame.
__declspec(noinline) bool ProbeTable(uint32_t sm, uint32_t& tableId, uint32_t& array, uint32_t& rank) {
    __try {
        tableId = *reinterpret_cast<const uint32_t*>(sm + machine::kTableId);
        array = *reinterpret_cast<const uint32_t*>(sm + machine::kStateArr);
        rank = *reinterpret_cast<const uint32_t*>(sm + machine::kAllocNumber);
        const uint32_t index = *reinterpret_cast<const uint32_t*>(sm + machine::kCurState);
        if (!array || index > 256) return false;
        const uint32_t entry = *reinterpret_cast<const uint32_t*>(array + index * 4);
        if (!entry) return false;
        const uint32_t update = *reinterpret_cast<const uint32_t*>(entry + machine::kStateUpdateMessage);
        const uint32_t transitions = *reinterpret_cast<const uint32_t*>(entry + machine::kStateTransArr);
        if (!update || !transitions) return false;
        // The update walks both lists, so both must be readable, not just set.
        (void)*reinterpret_cast<const uint32_t*>(update);
        (void)*reinterpret_cast<const uint32_t*>(transitions);
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
void OnMachineTick(uint32_t sm) {
    InterlockedIncrement(&machineTicks);
    uint32_t tableId = 0, array = 0, rank = 0;
    const bool usable = ProbeTable(sm, tableId, array, rank);
    if (!tableId) return;
    const uint32_t live = liveTables.Lookup(tableId);
    const uint32_t liveRank = liveTables.RankOf(tableId);
    if (usable && (!live || rank >= liveRank)) {
        liveTables.Observe(tableId, array, rank);
        return;
    }
    if (!repairEnabled || !live || live == array) {
        if (!usable) InterlockedIncrement(&repairNoDonor);
        return;
    }
    InterlockedIncrement(&stale);
    uint32_t checkId = 0, checkArray = 0, checkRank = 0;
    if (PokeArray(sm, live) && ProbeTable(sm, checkId, checkArray, checkRank)) {
        InterlockedIncrement(&repaired);
    } else {
        PokeArray(sm, array);
        InterlockedIncrement(&repairNoDonor);
    }
}

// Work another module owns but that has to run on this thread. The spawn
// multiplier uses it to space its repeats out in time; this hook is the only
// periodic call the plugin has on the thread that owns the actors.
void (*gameThreadWork)() = nullptr;

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

// Say, on behalf of an actor that can no longer die, the thing the level was
// waiting to hear. Game thread only.
//
// The message is deliberately NOT cleared afterwards. Two actors can share one
// authored attribute block - spawn 14 and spawn 15 of the hospital lobby both
// pointed at header 243873C0 on 2026-09-16 - so clearing one would silence the
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
void ProxyDeath(uint32_t actor, uint32_t hash, const char* name) {
    if (!enabled || !deathProxyEnabled || !actor || !hash) return;
    uint32_t vtable = 0;
    Guid guid{};
    if (!ReadAt(actor, &vtable, 4) || !ReadAt(actor + 0x24, guid.data(), sizeof(guid))) return;
    std::lock_guard<std::mutex> guard(lock);
    DeathProxy* slot = nullptr;
    for (auto& p : deathProxies) if (p.actor == actor) { slot = &p; break; }
    if (!slot) for (auto& p : deathProxies) if (!p.actor) { slot = &p; break; }
    if (!slot) { InterlockedIncrement(&proxiesFull); return; }
    *slot = DeathProxy{};
    slot->actor = actor; slot->vtable = vtable; slot->guid = guid; slot->hash = hash;
    std::snprintf(slot->name, sizeof(slot->name), "%s", name ? name : "");
    InterlockedIncrement(&proxiesRegistered);
}

// Logging worker, under `lock`: has a proxied actor died? Then hand it to the
// game thread. Gone without a witnessed death: dropped.
template<class Reader>
void PollDeathProxies(FILE* log, Reader& read) {
    for (auto& p : deathProxies) {
        if (!p.actor || p.queued) continue;
        if (!p.announced) {
            // the post-create hook has no log handle; the worker announces
            p.announced = true;
            std::fprintf(log, "watchdog death_proxy registered actor=%08X entity=%08X-%08X-%08X-%08X message=%s (%08X): transplant refused, watchdog will say it\n",
                         p.actor, p.guid[0], p.guid[1], p.guid[2], p.guid[3], p.name, p.hash);
        }
        uint32_t vtable = 0;
        Guid guid{};
        if (!read(p.actor, &vtable, 4) || vtable != p.vtable ||
            !read(p.actor + 0x24, guid.data(), sizeof(guid)) || guid != p.guid) {
            InterlockedIncrement(&proxiesLost);
            std::fprintf(log, "watchdog death_proxy lost actor=%08X message=%s (actor gone before a death was seen)\n", p.actor, p.name);
            p = DeathProxy{};
            continue;
        }
        bool dead = false;
        ForEachMachine(p.actor, read, [&](uint32_t sm) {
            uint32_t id = 0;
            if (read(sm + machine::kTableId, &id, 4) && id == machine::kDeadTable) dead = true;
        });
        if (!dead) continue;
        const LONG count = InterlockedCompareExchange(&deathQueued, 0, 0);
        if (count >= static_cast<LONG>(std::size(deathQueue))) continue;   // next second
        deathQueue[count] = p;
        InterlockedExchange(&deathQueued, count + 1);
        p.queued = true;
        std::fprintf(log, "watchdog death_proxy dead actor=%08X entity=%08X-%08X-%08X-%08X: saying %s (%08X) for it\n",
                     p.actor, p.guid[0], p.guid[1], p.guid[2], p.guid[3], p.name, p.hash);
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
    std::lock_guard<std::mutex> guard(lock);
    for (auto& q : deathProxies) if (q.actor == p.actor && q.hash == p.hash) q = DeathProxy{};
}

// Game thread. Hot path: a single atomic read and out when nothing is queued.
void OnTick(SafetyHookContext& context) {
    OnMachineTick(context.ecx);
    if (gameThreadWork) gameThreadWork();
    // One release per tick. The dispatch runs every listener of the message, and
    // a quarantine's listener can open a door and start a cutscene, so two in one
    // tick is more than this hook has any business doing.
    if (InterlockedCompareExchange(&releaseQueued, 0, 0) > 0) {
        Stranded stranded;
        {
            std::lock_guard<std::mutex> guard(lock);
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
            std::lock_guard<std::mutex> guard(lock);
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
        std::lock_guard<std::mutex> guard(lock);
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
    using SetHealth = void(__thiscall*)(void*, float);
    reinterpret_cast<SetHealth>(slot)(reinterpret_cast<void*>(component), 0.0f);
    InterlockedIncrement(&killed);
}

// Called from the post-create hook for an actor the randomizer replaced.
// allowKill is decided there, where the donor's family is known.
template<class Reader>
void Track(uint32_t actor, const Guid& selected, bool allowKill, uint32_t leashMs, Reader& read) {
    if (!enabled || !actor) return;
    uint32_t vtable = 0;
    Guid guid{};
    if (!read(actor, &vtable, 4) || !read(actor + 0x24, guid.data(), sizeof(guid))) return;
    std::lock_guard<std::mutex> guard(lock);
    if (tracker.Track(actor, vtable, guid, selected, GetTickCount(), allowKill, leashMs))
        InterlockedIncrement(&tracked);
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

// Logging worker. Reads only, and queues rather than calling the engine.
template<class Reader>
void Poll(FILE* log, Reader& read) {
    if (!enabled) return;
    std::lock_guard<std::mutex> guard(lock);
    // No repair here any more: a once-a-second pass on the logging worker lost
    // the race against the game's own tick. OnMachineTick does it in place.
    const uint32_t now = GetTickCount();
    if (deathProxyEnabled) PollDeathProxies(log, read);
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
                std::fprintf(log, "watchdog vent_stuck actor=%08X donor=%08X-%08X-%08X-%08X "
                                  "age_s=%u machine=%s table=%08X pos=%s action=watching\n",
                             entry.actor, entry.selected[0], entry.selected[1], entry.selected[2],
                             entry.selected[3], (now - entry.bornTick) / 1000u, ventName, ventTable,
                             ActorPositionText(entry.actor, read, where, sizeof(where)));
                tracker.MarkProbed(i);
                InterlockedIncrement(&probes);
                break;
            }
            std::fprintf(log, "watchdog state actor=%08X entity=%08X-%08X-%08X-%08X age_s=%u "
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
                std::fprintf(log, "watchdog vent_stuck actor=%08X donor=%08X-%08X-%08X-%08X "
                                  "age_s=%u machine=%s action=donor_blocked\n",
                             entry.actor, entry.selected[0], entry.selected[1], entry.selected[2],
                             entry.selected[3], (now - entry.bornTick) / 1000u, stillInVent);
                if (reportBadVentDonor) reportBadVentDonor(entry.selected);
            }
            if (corpse) {
                InterlockedIncrement(&corpses);
                std::fprintf(log, "watchdog corpse actor=%08X entity=%08X-%08X-%08X-%08X age_s=%u "
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
                std::fprintf(log, "watchdog inert actor=%08X donor=%08X-%08X-%08X-%08X age_s=%u "
                                  "machines=0 action=donor_blocked\n",
                             entry.actor, entry.selected[0], entry.selected[1], entry.selected[2],
                             entry.selected[3], (now - entry.bornTick) / 1000u);
                if (reportBadVentDonor) reportBadVentDonor(entry.selected);
            }
            const LONG count = InterlockedCompareExchange(&queued, 0, 0);
            // No kill for an actor without state machines: SetHealth(0) on an
            // actor that never became an NPC has no death to run, and the
            // release below already speaks for it (21.09.2026).
            const bool willKill = entry.allowKill && killEnabled && living &&
                                  count < static_cast<LONG>(kQueueCapacity);
            if (willKill) {
                queue[count] = {entry.actor, entry.vtable, entry.guid};
                InterlockedExchange(&queued, count + 1);
            } else if (entry.allowKill && killEnabled) {
                // Queue full: leave it for the next pass rather than losing it.
                break;
            } else if (!entry.allowKill) {
                InterlockedIncrement(&killSkippedFamily);
            }
            // The two cases where the actor is not merely unvisited but unable:
            // no state machine at all after the whole leash, or still running a
            // vent machine it will never leave. Both were already condemned
            // above; this is what makes that verdict cost the room nothing.
            //
            // An enemy with a readable tree that the player simply has not
            // reached yet is deliberately not here. Saying its death for it
            // would open a door in front of a living enemy, which is a worse
            // bug than the one being fixed.
            //
            // Below the kill decision on purpose. A full kill queue breaks out
            // above and leaves the entry tracked, so the same actor comes back
            // through this case on the next pass. Queued before that break, its
            // message would be said twice, and a room that counts deaths would
            // count one enemy as two.
            // A queued kill produces the real death and with it the message; a
            // release on top would say it twice, and a room that counts deaths
            // would count one enemy as two (21.09.2026).
            if (releaseEnabled && sendVerified && (!living || (stillInVent && !willKill))) {
                const LONG waiting = InterlockedCompareExchange(&releaseQueued, 0, 0);
                if (waiting < static_cast<LONG>(kQueueCapacity)) {
                    releaseQueue[waiting] = {entry.actor, entry.vtable, entry.guid,
                                             stillInVent != nullptr};
                    InterlockedExchange(&releaseQueued, waiting + 1);
                    std::fprintf(log, "watchdog release_queued actor=%08X entity=%08X-%08X-%08X-%08X "
                                      "reason=%s\n",
                                 entry.actor, entry.guid[0], entry.guid[1], entry.guid[2],
                                 entry.guid[3], !living ? "no_state_machines" : "stuck_in_vent");
                }
            }
            char where[48]{};
            std::fprintf(log,
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

void WriteDiagnostics(FILE* log) {
    static LONG lastTotal = -1;
    if (!enabled) return;
    std::lock_guard<std::mutex> guard(lock);
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
                       InterlockedCompareExchange(&releaseFaults, 0, 0);
    if (total == lastTotal) return;
    lastTotal = total;
    std::fprintf(log, "watchdog summary: tracked=%ld expired=%ld killed=%ld family_excluded=%ld "
                      "component_missing=%ld identity_changed=%ld released=%ld unreadable=%ld "
                      "stale=%ld repaired=%ld no_live_table=%ld ticks=%ld live_tables=%zu "
                      "live=%zu dropped=%zu queued=%ld vent_stuck=%ld inert=%ld corpses=%ld\n",
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
                 tracker.Live(), tracker.Dropped(), InterlockedCompareExchange(&queued, 0, 0),
                 InterlockedCompareExchange(&ventStuck, 0, 0),
                 InterlockedCompareExchange(&inert, 0, 0),
                 InterlockedCompareExchange(&corpses, 0, 0));
    {
        LONG pending = 0;
        for (const auto& p : deathProxies) if (p.actor) ++pending;
        std::fprintf(log, "watchdog death_proxy registered=%ld sent=%ld unregistered=%ld lost=%ld faults=%ld full=%ld pending=%ld\n",
                     InterlockedCompareExchange(&proxiesRegistered, 0, 0), InterlockedCompareExchange(&proxiesSent, 0, 0),
                     InterlockedCompareExchange(&proxiesUnregistered, 0, 0), InterlockedCompareExchange(&proxiesLost, 0, 0),
                     InterlockedCompareExchange(&proxiesFaults, 0, 0), InterlockedCompareExchange(&proxiesFull, 0, 0), pending);
    }
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
