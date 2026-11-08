// Dead Space 2 United by Richard Hafer.
// Progression gate: nobody rides the elevator alone. Design and the offline
// half in coop_gate.h; the wire half in coop_gate_protocol.h. This file is the
// part that touches the game.
//
// Every scripted event in the level goes through one function: the message
// dispatch at 0x00756C60, `bool __cdecl(void* message, int flags)`, where the
// message is 12 bytes {name hash, flag byte, source object}. It looks the hash
// up in the registry at base+0x1C4CD8C and calls each listener. The gate wraps
// it: a message whose listeners would take the player somewhere is held back,
// a vote goes out to the partners, and the message is replayed once everybody
// has reached it too, or after the timeout, whichever comes first.
//
// "Would take the player somewhere" is decided by asking the listeners what
// they are. Every engine object answers a class query through vtable slot 4:
//   bool __thiscall QueryInterface(uint32_t classHash, void** out)
// (measured on the live Player 2026-09-17: 0x00860BD0 compares the hash with
// its own class hashes and walks up to the base class implementation), so a
// StandardDoor answers to StandardDoor, Door, Entity and Animated, and the gate
// needs no list of message names, only a list of class names.
//
// Level script nodes (Checkpoint, LevelChange) are not entities and do not
// answer the query; those are recognised by their vtable, taken from the
// constructor each descriptor's factory calls (probe_classmap, same day).
#include "coop_gate.h"
#include "coop_gate_protocol.h"
#include "generated_internal_messages.h"
#include "generated_message_names.h"
#include "generated_ride_triggers.h"

// A list of messages from the INI, by name or by 8-digit hex hash, so each
// sequence message can be set to mirrored or held (20:30). Names come from
// the live name table (3385 known); a message that has no known name is
// written as its hash, which the log prints anyway.
namespace coop_lists {
constexpr unsigned kMax = 64;
struct List {
    uint32_t hashes[kMax]{};
    unsigned count = 0;
    bool Has(uint32_t hash) const { for (unsigned i = 0; i < count; ++i) if (hashes[i] == hash) return true; return false; }
};
inline uint32_t NameHashOf(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) {
        const char c = (*s >= 'A' && *s <= 'Z') ? static_cast<char>(*s + 32) : *s;
        h = h * 0x1003Fu + static_cast<unsigned char>(c);
    }
    return h;
}
inline void Read(FILE* log, const std::string& ini, const char* section, const char* key, List& out) {
    char list[2048]{};
    GetPrivateProfileStringA(section, key, "", list, sizeof(list), ini.c_str());
    char* next = nullptr;
    out.count = 0;
    for (char* token = strtok_s(list, ",; ", &next); token && out.count < kMax; token = strtok_s(nullptr, ",; ", &next)) {
        bool hex = std::strlen(token) == 8;
        for (const char* c = token; hex && *c; ++c) hex = std::isxdigit(static_cast<unsigned char>(*c)) != 0;
        out.hashes[out.count++] = hex ? static_cast<uint32_t>(std::strtoul(token, nullptr, 16)) : NameHashOf(token);
    }
    if (out.count) {
        std::fprintf(log, "coop %s %s:", section, key);
        for (unsigned i = 0; i < out.count; ++i) std::fprintf(log, " %08X(%s)", out.hashes[i], coop_names::Name(out.hashes[i]));
        std::fprintf(log, "\n");
    }
}
}  // namespace coop_lists

namespace cutscene { bool Following(); bool Leading(); bool PartnerInScene(unsigned slot, DWORD now); bool SequenceRunningNow(); }   // runtime_coop_cutscene.inc
namespace worldcalls { bool DoorCloseCull(uint32_t hash); }   // runtime_coop_worldcalls.inc
namespace zoneobj { bool Drops(uint32_t hash, bool nested); bool Local(uint32_t hash); }   // runtime_coop_zoneobj.inc
namespace mpmap { bool InVisit(); }   // runtime_coop_mpmap.inc
namespace objonce { bool OnLocal(uint32_t hash); bool Is(uint32_t hash); bool AllowReplay(uint32_t hash); bool Mark(uint32_t hash); void NoteReplayed(uint32_t hash);
                    bool FromHistory(uint32_t hash); extern volatile LONG journaled, sentAnyway; }   // runtime_coop_objonce.inc
namespace level { bool PeerInMenu(uint32_t token, DWORD now); }   // runtime_coop_level.inc
namespace gate {
bool enabled = false, installed = false, trace = true;
uint32_t timeoutMs = coop_gate::kDefaultTimeoutMs;
float driftLimit = coop_gate::kDefaultDriftLimit;
// How close the local Isaac has to be to a trigger to count as having reached
// it. Triggers are volumes; this is measured to the entity's origin.
float triggerReach = 10.0f;
// A partner who waits at a progression point this player cannot reach pulls
// him over ([Gate] PullToWaitingPartnerMs, 0 = off). 25.09., ch11: the host
// shot the fuse, the barrier came down between the two players, and nothing on
// the client's side opens it again (the level is built for one Isaac). Whatever
// cut a player off, the group meets at the next progression point, whose
// hold waits for every player; the one who has not arrived after this long is
// put where the partner stood when he voted, like DS3 does at its checkpoints.
DWORD pullAfterMs = 0;
constexpr float kPullMinDistance = 8.0f;
volatile LONG pulls = 0;
// The message source is a handle: table at [0x0201C0F4], index = handle >> 23,
// 8-byte entries {object, id} (PlayerHelmetCommand::Activate, 0x00883F80).
constexpr uint32_t kHandleTableRva = 0x1C1C0F4;

constexpr uint32_t kDispatchRva = 0x356C60;
constexpr uint32_t kRegistryRva = 0x1C4CD8C;
constexpr uint32_t kBuckets = 0x1000;
// Read-only data of the unpacked image, where every vtable lives.
constexpr uint32_t kRdataLo = 0x1600000, kRdataHi = 0x1880000;

safetyhook::InlineHook dispatchHook;
coop_gate::Queue queue;
coop_gate_wire::Announcer announcer;
constexpr uint32_t kVoteLingerMs = 3000;   // a released vote still goes out this long
std::mutex lock;

// The same hash the engine uses for class and message names:
// h = h*0x1003F + lowercase(c).
constexpr uint32_t NameHash(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) {
        const char c = (*s >= 'A' && *s <= 'Z') ? static_cast<char>(*s + 32) : *s;
        h = h * 0x1003Fu + static_cast<unsigned char>(c);
    }
    return h;
}

// Which listener classes arm the gate. Entities are asked by hash; script
// nodes are matched by vtable. Both lists come from the INI, so a class that
// turns out to gate every doorway in the ship can be taken out without a build.
constexpr unsigned kMaxClasses = 40;
struct ClassRule { char name[32]; uint32_t hash; };
struct NodeRule { const char* name; uint32_t vtableRva; };
// Vtables from the live class map of 2026-09-17 (descriptor -> factory ->
// constructor -> first vtable store; analysis/ds2_classmap_live_2026-09-17.json).
// Script nodes only; entities answer the class query and need no vtable.
constexpr NodeRule kKnownNodes[] = {
    {"Checkpoint",        0x1741D60},
    {"LevelChange",       0x17588C4},
    {"StreamSet",         0x1719718},
    {"StreamScripter",    0x17221CC},
    {"FuncSpawn",         0x1759C58},
    {"VariableOperator",  0x171FF68},
    {"VariableWatcher",   0x172027C},
    {"AnimateLight",      0x17413A4},
    {"MoveLight",         0x1740070},
    {"ObjectiveEntry",    0x1776D20},
    {"LogEntry",          0x1772D98},
    {"OrEventGate",       0x172101C},
    {"AndEventGate",      0x1720F58},
    {"MultiManager",      0x1721178},
    {"MultiRemoveTarget", 0x171FCE8},
    {"LuaSwitch",         0x171F76C},
    {"ExecuteLuaScript",  0x171F704},
    {"EquipCommand",      0x174BC48},
    {"PlayerHelmetCommand", 0x172686C},
    {"ExecuteVFX",        0x171F904},
};
// A named set of listener classes: entities by class hash (inheritance
// aware), script nodes by vtable.
struct RuleSet {
    const char* label = "";
    ClassRule entities[kMaxClasses]{};
    unsigned entityCount = 0;
    uint32_t nodeVtables[kMaxClasses]{};
    const char* nodeNames[kMaxClasses]{};
    unsigned nodeCount = 0;
    unsigned Size() const { return entityCount + nodeCount; }
};
RuleSet progressionRules;
// Who may have sent a held message: only something a player did (walked into
// a trigger, used a door, rode a lift, brought an area in). A message from a
// spawner, a counter or a sequence controller is a consequence of something
// that was (or should have been) gated already; holding it waits for a partner
// who can never produce it. 17:06 run: the host held two enemy-intro
// sequences for 45 s each while the client, who does not run the host's
// NPCs, had nothing to vote with.
RuleSet sourceRules;
// Off by default since the 18:21 run: with it on, not one message was held in
// a whole session. The sources of area loads, checkpoints and doors are not
// the classes the list names (the load-complete source is a transient
// StreamSet helper, the trigger's own broadcast does not come from the
// TriggerBase). Left in as an opt-in; the log now names every source's
// vtable so the list can be filled from evidence.
bool sourceRule = false;
uint32_t sequenceTimeoutMs = 20000;
// Never held, whatever the listeners: [Gate] Exempt=name,name,hash...
coop_lists::List exempt;
volatile LONG passedExempt = 0;

// What was held, so the replay can hand the dispatch the exact bytes it saw.
struct Pending {
    bool used = false;
    uint32_t hash = 0;
    unsigned char raw[12]{};
    int flags = 0;
    DWORD heldAt = 0;
    char cls[32]{};
    // The trigger fired here but the local Isaac was not at it: a partner's
    // ghost walked in. The local vote waits until the local Isaac gets there.
    bool awaitingSelf = false;
    float triggerAt[3]{};
    // iMsgStreamSetLoadComplete's source is a transient object whose +8
    // points at the stream set's GUID; by replay time it is gone and its
    // handle resolves to nothing, so StreamSet::HandleEvents (0x0075A14D) then
    // reads [0+8] and faults (both machines, 15:30). The GUID is copied at
    // hold time and a stand-in object with the same shape is put behind the
    // handle for the duration of the replay.
    bool streamPayload = false;
    uint32_t streamGuid[4]{};
};
constexpr uint32_t kStreamSetLoadComplete = 0x5FC7E489u;
Pending pending[coop_gate::kHeldCapacity]{};

// Verdicts are remembered per hash, because the per-frame message and a few
// hundred others come through here every second and the registry walk is not
// free. A verdict ages out after ten seconds, which also covers a level change
// re-registering the same hash with different listeners.
struct Verdict { uint32_t hash; DWORD at; bool progression; char cls[32]; };
// When a ScriptedSequence last received a message on this machine: a spawn
// inside that window is the sequence's actor (see npcs::OnShouldSpawn).
volatile LONG lastSequenceTick = 0;
constexpr unsigned kVerdicts = 512;
Verdict verdicts[kVerdicts]{};
constexpr DWORD kVerdictTtlMs = 10000;

volatile LONG dispatched = 0, classified = 0, progression = 0, held = 0, released = 0,
              replayed = 0, timedOut = 0, votesSent = 0, votesReceived = 0, votesRelayed = 0,
              passedNested = 0, passedSolo = 0, passedNoPlayer = 0, passedInternal = 0, passedZoneLocal = 0, passedVersus = 0, queryFaults = 0, stray = 0;
volatile LONG passedNecro = 0, necroVotesIgnored = 0;
LONG replaying = 0;   // game thread only
bool holdNotice = false;   // a hold happened; Tick tells the player
// A hold that resolves within this is not worth a banner: the stream
// scripters that follow a released area load fire on both machines within
// milliseconds of each other and were showing four banners a second.
constexpr DWORD kNoticeAfterMs = 1500;
DWORD holdNoticeSince = 0;
bool holdNoticeShown = false;

// Event ring, flushed from the diagnostics thread once a second. A full ring drops
// its oldest line (05.10.: a prop hit logged every frame pushed the bots' kinesis
// lines out); how many is logged.
constexpr unsigned kEvents = 64;
char events[kEvents][256]{};
unsigned eventHead = 0, eventCount = 0, eventsOverwritten = 0;
// Its own mutex, never the queue's: Event() is called from inside queue
// scopes all over, and MSVC's std::mutex throws system_error on a recursive
// lock (18:34 and 18:37: abort on the partner-wait notice).
std::mutex eventLock;
void Event(const char* format, ...) {
    char text[sizeof(events[0])];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (botlog::Take(text)) return;   // the bots have their own file
    if (std::strncmp(text, "respawn: ", 9) == 0) vlog::MirrorIfMatch("respawn", text + 9);
    std::lock_guard<std::mutex> guard(eventLock);
    std::memcpy(events[(eventHead + eventCount) % kEvents], text, sizeof(text));
    if (eventCount < kEvents) ++eventCount; else { eventHead = (eventHead + 1) % kEvents; ++eventsOverwritten; }
}

// --- classification ---

// The class query, called under SEH with the shape checked first: the slot
// must open `mov edx,[esp+4] / mov al,1` or it is not the query at all.
bool QueryFilter(unsigned) { InterlockedIncrement(&queryFaults); return true; }
bool IsA(uint32_t object, uint32_t vtable, uint32_t classHash) {
    uint32_t fn = 0;
    unsigned char head[6]{};
    if (!Read(vtable + 0x10, &fn, 4) || fn < base + 0x1000 || fn >= base + hits::kCodeEnd) return false;
    if (!Read(fn, head, sizeof(head))) return false;
    // Three shapes seen: entities open `mov edx,[esp+4] / mov al,1` (Player
    // 0x00860BD0); script nodes open `cmp dword [esp+4], imm32` (Checkpoint
    // 0x00B75780); ScriptedEventSequence opens `mov eax,[esp+4] / cmp eax,
    // imm32` (0x00E93550, ret 8). Anything else is not the query. The third
    // was missing until 23.09., so no ScriptedEventSequence ever matched
    // AnyClasses and no such scene cue was ever shared.
    static const unsigned char kEntityShape[] = {0x8B, 0x54, 0x24, 0x04, 0xB0, 0x01};
    static const unsigned char kNodeShape[] = {0x81, 0x7C, 0x24, 0x04};
    static const unsigned char kCompareShape[] = {0x8B, 0x44, 0x24, 0x04, 0x3D};
    if (std::memcmp(head, kEntityShape, sizeof(kEntityShape)) != 0 &&
        std::memcmp(head, kNodeShape, sizeof(kNodeShape)) != 0 &&
        std::memcmp(head, kCompareShape, sizeof(kCompareShape)) != 0) return false;
    using Query = bool(__thiscall*)(void*, uint32_t, void**);
    void* out = nullptr;
    bool answer = false;
    __try {
        answer = reinterpret_cast<Query>(fn)(reinterpret_cast<void*>(object), classHash, &out);
    } __except (QueryFilter(GetExceptionCode())) {
        return false;
    }
    return answer && out != nullptr;
}
// The lifts only: Mover (0x00DA9430) and MoverController (0x00DA9A30) open their
// query `mov edx,[esp+4] / mov eax,[esp+8]`, a shape IsA does not accept, so no
// lift check ever matched and every ride left with whoever pressed (13.10., ch02).
// Taking that shape in IsA would make every Mover and door message progression
// for the first time; here it only answers "is this a lift".
bool IsLiftClass(uint32_t object, uint32_t vtable, uint32_t classHash) {
    if (IsA(object, vtable, classHash)) return true;
    uint32_t fn = 0;
    unsigned char head[8]{};
    if (!Read(vtable + 0x10, &fn, 4) || fn < base + 0x1000 || fn >= base + hits::kCodeEnd || !Read(fn, head, sizeof(head))) return false;
    static const unsigned char kPairShape[] = {0x8B, 0x54, 0x24, 0x04, 0x8B, 0x44, 0x24, 0x08};
    if (std::memcmp(head, kPairShape, sizeof(kPairShape)) != 0) return false;
    using Query = bool(__thiscall*)(void*, uint32_t, void**);
    void* out = nullptr;
    bool answer = false;
    __try {
        answer = reinterpret_cast<Query>(fn)(reinterpret_cast<void*>(object), classHash, &out);
    } __except (QueryFilter(GetExceptionCode())) {
        return false;
    }
    return answer && out != nullptr;
}
// A lift's or an elevator's own MoverController (generated_ride_triggers.h): a
// machine or a platform never holds a ride.
bool IsLiftController(uint32_t object, uint32_t vtable) {
    static const uint32_t kMoverController = NameHash("MoverController");
    uint32_t guid[4]{};
    if (!Read(object + 0x24, guid, 16)) return false;
    bool listed = false;
    for (const auto& known : coop_rides::kLiftControllers)
        if (std::memcmp(known, guid, 16) == 0) { listed = true; break; }
    return listed && IsLiftClass(object, vtable, kMoverController);
}

// Is this name registered here at all? The dispatch has no not-found branch
// (it walks the bucket chain and touches whatever it ended on), so nothing
// may be replayed on a machine whose level never registered the name. The
// client stuck in a loading screen on 2026-09-17 13:54 had replayed six of
// the host's door messages into a world that was not there yet.
bool Registered(uint32_t hash) {
    uint32_t table = 0, entry = 0;
    if (!Read(base + kRegistryRva, &table, 4) || !table) return false;
    if (!Read(table + (hash & (kBuckets - 1)) * 4, &entry, 4)) return false;
    for (unsigned guard = 0; entry && guard < 64; ++guard) {
        uint32_t key = 0;
        if (!Read(entry + 0x10, &key, 4)) return false;
        if (key == hash) return true;
        if (!Read(entry, &entry, 4)) return false;
    }
    return false;
}

// Every object that listens to `hash` (bounded), with its vtable.
unsigned Listeners(uint32_t hash, uint32_t* objects, uint32_t* vtables, unsigned max) {
    uint32_t table = 0, entry = 0;
    if (!Read(base + kRegistryRva, &table, 4) || !table) return 0;
    if (!Read(table + (hash & (kBuckets - 1)) * 4, &entry, 4)) return 0;
    bool found = false;
    for (unsigned guard = 0; entry && guard < 64 && !found; ++guard) {
        uint32_t key = 0;
        if (!Read(entry + 0x10, &key, 4)) return 0;
        found = key == hash;
        if (!found && !Read(entry, &entry, 4)) return 0;
    }
    if (!found || !entry) return 0;
    unsigned count = 0;
    uint32_t list = 0;
    Read(entry + 4, &list, 4);
    for (unsigned lists = 0; list && lists < 8 && count < max; ++lists) {
        uint32_t node = 0;
        Read(list + 8, &node, 4);
        for (unsigned nodes = 0; node && nodes < 64 && count < max; ++nodes) {
            uint32_t object = 0, vtable = 0;
            if (Read(node + 0x0C, &object, 4) && object && Read(object, &vtable, 4)) { objects[count] = object; vtables[count] = vtable; ++count; }
            if (!Read(node + 4, &node, 4)) break;
        }
        if (!Read(list, &list, 4)) break;
    }
    return count;
}
// The first object with exactly this vtable that listens to `hash`.
uint32_t ListenerWithVtable(uint32_t hash, uint32_t wanted) {
    uint32_t table = 0, entry = 0;
    if (!Read(base + kRegistryRva, &table, 4) || !table) return 0;
    if (!Read(table + (hash & (kBuckets - 1)) * 4, &entry, 4)) return 0;
    bool found = false;
    for (unsigned guard = 0; entry && guard < 64 && !found; ++guard) {
        uint32_t key = 0;
        if (!Read(entry + 0x10, &key, 4)) return 0;
        found = key == hash;
        if (!found && !Read(entry, &entry, 4)) return 0;
    }
    if (!found || !entry) return 0;
    uint32_t list = 0;
    Read(entry + 4, &list, 4);
    for (unsigned lists = 0; list && lists < 8; ++lists) {
        uint32_t node = 0;
        Read(list + 8, &node, 4);
        for (unsigned nodes = 0; node && nodes < 64; ++nodes) {
            uint32_t object = 0, vtable = 0;
            if (Read(node + 0x0C, &object, 4) && object && Read(object, &vtable, 4) && vtable == wanted) return object;
            if (!Read(node + 4, &node, 4)) break;
        }
        if (!Read(list, &list, 4)) break;
    }
    return 0;
}
// Walks the registry entry for `hash` and asks every listener. True if any of
// them is in the rule set; `cls` gets its name.
bool AnyListener(uint32_t hash, const RuleSet& rules, char* cls, size_t clsSize, unsigned& listeners) {
    listeners = 0;
    uint32_t table = 0, entry = 0;
    if (!Read(base + kRegistryRva, &table, 4) || !table) return false;
    if (!Read(table + (hash & (kBuckets - 1)) * 4, &entry, 4)) return false;
    bool found = false;
    for (unsigned guard = 0; entry && guard < 64 && !found; ++guard) {
        uint32_t key = 0;
        if (!Read(entry + 0x10, &key, 4)) return false;
        found = key == hash;
        if (!found && !Read(entry, &entry, 4)) return false;
    }
    if (!found || !entry) return false;
    uint32_t list = 0;
    Read(entry + 4, &list, 4);
    for (unsigned lists = 0; list && lists < 8; ++lists) {
        uint32_t node = 0;
        Read(list + 8, &node, 4);
        for (unsigned nodes = 0; node && nodes < 64; ++nodes) {
            uint32_t object = 0, vtable = 0;
            if (Read(node + 0x0C, &object, 4) && object && Read(object, &vtable, 4) &&
                vtable >= base + kRdataLo && vtable < base + kRdataHi) {
                ++listeners;
                for (unsigned i = 0; i < rules.nodeCount; ++i) {
                    if (vtable != base + rules.nodeVtables[i]) continue;
                    std::snprintf(cls, clsSize, "%s", rules.nodeNames[i]);
                    return true;
                }
                for (unsigned i = 0; i < rules.entityCount; ++i) {
                    if (!IsA(object, vtable, rules.entities[i].hash)) continue;
                    std::snprintf(cls, clsSize, "%s", rules.entities[i].name);
                    return true;
                }
            }
            if (!Read(node + 4, &node, 4)) break;
        }
        if (!Read(list, &list, 4)) break;
    }
    return false;
}

// Boxes a player steps into that start a ride without any Door, Mover or
// MoverController hearing the message (12.10., tools/coop_ride_triggers.py): the
// small lift on the drill machine (mm_drill_machine_elevator_up) is an animation
// of the drill prop and rose with whoever stood on it first. Held like any
// progression until every player's own box fired, i.e. everybody stands on it.
bool rideTriggers = false;   // [Gate] RideTriggers
volatile LONG ridesHeld = 0;
bool IsRideTrigger(uint32_t hash) {
    for (const auto& r : coop_rides::kRides) if (r.hash == hash) return true;
    return false;
}
bool Classify(uint32_t hash, DWORD now, char* cls, size_t clsSize) {
    Verdict& slot = verdicts[hash % kVerdicts];
    if (slot.hash == hash && now - slot.at < kVerdictTtlMs) {
        std::snprintf(cls, clsSize, "%s", slot.cls);
        return slot.progression;
    }
    unsigned listeners = 0;
    char found[32] = "";
    bool yes = AnyListener(hash, progressionRules, found, sizeof(found), listeners);
    if (!yes && rideTriggers && IsRideTrigger(hash)) {
        yes = true;
        std::snprintf(found, sizeof(found), "%s", "RideTrigger");
        InterlockedIncrement(&ridesHeld);
    }
    InterlockedIncrement(&classified);
    if (yes) {
        InterlockedIncrement(&progression);
        if (trace && slot.hash != hash)
            Event("coop_gate progression hash=%08X class=%s listeners=%u", hash, found, listeners);
    }
    slot.hash = hash; slot.at = now; slot.progression = yes;
    std::snprintf(slot.cls, sizeof(slot.cls), "%s", found);
    std::snprintf(cls, clsSize, "%s", found);
    return yes;
}

// --- presence ---

// Ghosts pull triggers.
//
// The 15:20 run released every stream gate within 15-110 ms by "quorum"
// although only one Isaac had walked to the door: the other machine had
// dispatched the same activation at the same moment, because the walker's
// ghost stands in that machine's world and its trigger volumes answer to any
// Player. So a dispatch here is not proof that the local Isaac reached
// anything. When the message's source resolves to a trigger entity, the local
// vote is cast only once the local Isaac is within `triggerReach` of it; the
// hold itself still starts at once, which is the wait needed on the machine
// whose player is elsewhere.
bool TriggerPosition(uint32_t source, float out[3]) {
    uint32_t table = 0, object = 0, id = 0, vtable = 0;
    if (!source || !Read(base + kHandleTableRva, &table, 4) || !table) return false;
    const uint32_t entry = table + (source >> 23) * 8;
    if (!Read(entry, &object, 4) || !Read(entry + 4, &id, 4) || id != source || !object) return false;
    if (!Read(object, &vtable, 4) || vtable < base + kRdataLo || vtable >= base + kRdataHi) return false;
    static const uint32_t kTriggerBase = NameHash("TriggerBase");
    if (!IsA(object, vtable, kTriggerBase)) return false;
    if (!Read(object + placement::kPlayerPosition, out, 12)) return false;   // the actor transform, position at +0xC0
    for (int i = 0; i < 3; ++i) if (!std::isfinite(out[i]) || std::fabs(out[i]) > 100000.0f) return false;
    return true;
}

// The class of the message's source against the source rules. An
// unresolvable source (the handle is stale, or zero) is not player-caused.
bool SourceIsPlayerCaused(uint32_t source, char* cls, size_t clsSize) {
    uint32_t table = 0, object = 0, id = 0, vtable = 0;
    if (!source || !Read(base + kHandleTableRva, &table, 4) || !table) return false;
    const uint32_t entry = table + (source >> 23) * 8;
    if (!Read(entry, &object, 4) || !Read(entry + 4, &id, 4) || id != source || !object) return false;
    if (!Read(object, &vtable, 4) || vtable < base + kRdataLo || vtable >= base + kRdataHi) return false;
    for (unsigned i = 0; i < sourceRules.nodeCount; ++i)
        if (vtable == base + sourceRules.nodeVtables[i]) { std::snprintf(cls, clsSize, "%s", sourceRules.nodeNames[i]); return true; }
    for (unsigned i = 0; i < sourceRules.entityCount; ++i)
        if (IsA(object, vtable, sourceRules.entities[i].hash)) { std::snprintf(cls, clsSize, "%s", sourceRules.entities[i].name); return true; }
    return false;
}

bool LocalPosition(float out[3]) {
    const uint32_t player = LocalSlotZeroEntity();   // Isaac or the necro body
    return player && Read(player + placement::kPlayerPosition, out, 12);
}

// Where a voter is right now: the local player from the Player, everybody else from
// the last PlayerState they sent. Unknown or silent means "cannot be located".
bool Where(uint32_t token, float out[3]) {
    if (token == peerReadiness.token) return LocalPosition(out);
    const auto* peer = peerTable.FindByToken(token);
    if (!peer) return false;
    const auto& runtime = peerRuntime[PeerSlot(peer)];
    if (!runtime.lastState) return false;
    for (int i = 0; i < 3; ++i) out[i] = runtime.pos[i];
    return true;
}

// How many players the quorum needs: this one plus every partner heard from
// lately. A partner who has gone quiet for a while (dead, reloading a
// checkpoint) still counts: at 20:00 the host walked through doors alone while
// the client reloaded, because ten silent seconds took him out of the quorum.
DWORD peerGraceMs = 60000;
// A necro player's machine is no voter (roles in runtime_coop.inc): it reaches no story trigger.
unsigned Required(DWORD now) {
    unsigned n = 1;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        // a partner in the main menu is no voter either
        if (peer.used && peer.token && now - peer.lastSeen < peerGraceMs && !roles::IsNecro(s, now) && !level::PeerInMenu(peer.token, now)) ++n;
    }
    return n;
}
// A level change waits for a dead human watching too: alone, the living one would load the next
// chapter without him (his vote comes when his partner's vote has brought him back there).
// Under the lock, from Take.
volatile LONG levelChangeWaitsSpectator = 0;
unsigned RequiredFor(uint32_t hash, DWORD now) {
    const unsigned n = Required(now), watching = roles::Spectators(now);
    if (!watching) return n;
    for (const auto& p : pending)
        if (p.used && p.hash == hash && std::strcmp(p.cls, "LevelChange") == 0) { InterlockedIncrement(&levelChangeWaitsSpectator); return n + watching; }
    return n;
}
// A configured partner who never connects keeps the peer table non-empty, so
// every progression message was held and replayed from a copy on the next
// Poll with a quorum of one. After PeerGraceMs without any partner ever
// identified, the gate passes messages straight through as in solo.
DWORD lonelySince = 0;
bool lonelyLogged = false;
volatile LONG passedLonely = 0;
bool NobodyConnected(DWORD now) {
    for (const auto& peer : peerTable.peers)
        if (peer.used && peer.token) {
            if (lonelyLogged) Event("coop_gate a partner connected: progression is gated again");
            lonelySince = 0;
            lonelyLogged = false;
            return false;
        }
    if (!lonelySince) { lonelySince = now; return false; }
    if (now - lonelySince < peerGraceMs) return false;
    if (!lonelyLogged) {
        lonelyLogged = true;
        Event("coop_gate no partner has connected for %lu s: progression passes as in solo", static_cast<unsigned long>((now - lonelySince) / 1000));
    }
    return true;
}


// --- the hook ---

// The last time each hash went through here, by hash slot. Read by the world
// mirror to fold a message this machine already fired itself.
constexpr unsigned kRecent = 1024;
struct Recent { uint32_t hash; DWORD at; };
Recent recent[kRecent]{};
void NoteRecent(uint32_t hash, DWORD now) { recent[hash % kRecent] = {hash, now}; }
void ForgetRecent(uint32_t hash) { if (recent[hash % kRecent].hash == hash) recent[hash % kRecent] = {}; }
bool SeenRecently(uint32_t hash, DWORD now, DWORD windowMs) {
    const Recent& r = recent[hash % kRecent];
    return r.hash == hash && TicksSince(now, r.at) <= windowMs;   // a stamp fresher than the caller's now is "just seen"
}
// Set by the mirror while it replays a host message: nothing about it is
// held, mirrored or noted again.
LONG mirroring = 0;
unsigned mirroringFrom = 0xFFu;   // the partner slot whose message the mirror replays now
// Returns true when the mirror took the message (a sequence cue waiting for
// the partners' streams, see mirror::seqwait): then it is not dispatched now.
bool (*g_mirrorHook)(uint32_t hash, const void* message, int flags, const char* verdictClass) = nullptr;
volatile LONG heldForStreams = 0;
// Told when a held message is released here, so the host's journal can carry
// "everybody already passed this" to a player who arrives later.
void (*g_passedHook)(uint32_t hash) = nullptr;

// Progression the others already went through. A joiner spawns at the host's
// checkpoint, and every stream trigger between there and the host is one the
// others passed minutes ago: their votes are long gone, and holding the
// joiner 45 seconds at each would make catching up a punishment. So what the
// host's journal says was released is let through here without a hold.
constexpr unsigned kPreapproved = 64;
uint32_t preapproved[kPreapproved]{};
unsigned preapprovedHead = 0;
volatile LONG passedThrough = 0, preapprovedVotes = 0, replayUnregistered = 0, restartClears = 0, passedScene = 0, nestedVotes = 0;
volatile LONG pendingFull = 0, replaySourceAlive = 0, replayStandInFaults = 0;
// A scene carries the players already: the leader's cues pull the follower
// through it, so a checkpoint or door inside its chain is not a place to wait.
// 25.09., the Tormentor: the client led, its chain reached
// msg_checkpoint_shuttlebay_kong_phase_1, the gate held it for the host (who
// followed the scene and never walked into that trigger himself), and the
// client's scene stood still while the host read "partner waiting".
bool SceneCarries(DWORD now) {
    if (cutscene::Following() || cutscene::Leading()) return true;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s)
        if (cutscene::PartnerInScene(s, now)) return true;
    return false;
}
// Held-class messages let through because their sender was not a player action.
volatile LONG passedSequenceStep = 0, earlyVotes = 0;
// A checkpoint restart or a new level: nothing held belongs to the new world.
// The held messages would replay into it (the 24.09. chamber hold survived a
// restart and merged with the next occurrence), and the local votes would
// answer a partner's hold of the same message there.
// The released-recently fold and the pull timer go too (reset in Tick): a
// partner's new votes for the same message in the reloaded world are real.
volatile LONG restartPending = 0;
void OnRestart(const char* why) {
    InterlockedExchange(&restartPending, 1);
    unsigned cleared = 0;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (auto& p : pending) if (p.used) { p = Pending{}; ++cleared; }
        for (auto& item : queue.items) item = coop_gate::Held{};
        announcer.Clear();
    }
    holdNotice = false;
    holdNoticeSince = 0;
    holdNoticeShown = false;
    InterlockedIncrement(&restartClears);
    Event("coop_gate cleared for %s: %u held messages dropped", why, cleared);
}
// A relayed MultiManager drops a checkpoint or level change of its chain (it
// must not fire here), but the partner whose own chain holds that message
// waits for this machine's vote. 25.09., the eye poke: the host's copy of 4FCB4627
// dropped set_ch11_post_eyepoke_checkpoint_game, the client held it and the
// host, 0.4 m from the trigger, never voted. The drop now votes instead.
volatile LONG droppedVotes = 0;
void VoteForDropped(uint32_t hash) {
    char cls[32] = "";
    const DWORD now = GetTickCount();
    if (!enabled || !inLevel || !peerTable.Count() || !Classify(hash, now, cls, sizeof(cls))) return;
    {
        std::lock_guard<std::mutex> guard(lock);
        announcer.Cast(hash, 0, nullptr, now, false);
    }
    InterlockedIncrement(&droppedVotes);
    Event("coop_gate %08X (%s, %s) dropped from a relayed chain here: vote sent for the partner who holds it", hash, coop_names::Name(hash), cls);
}
// A checkpoint restart puts the world back: what the others already passed
// has to be passed again, together.
void ResetApprovals() {
    std::lock_guard<std::mutex> guard(lock);
    for (auto& h : preapproved) h = 0;
    preapprovedHead = 0;
}
void Preapprove(uint32_t hash) {
    for (uint32_t h : preapproved) if (h == hash) return;
    preapproved[preapprovedHead] = hash;
    preapprovedHead = (preapprovedHead + 1) % kPreapproved;
}
bool Preapproved(uint32_t hash) {
    for (uint32_t h : preapproved) if (h == hash) return true;
    return false;
}

// What happened just before a scene started: the last broadcasts and the
// last direct sends (with the target's vtable), for runtime_coop_cutscene to
// print when the local Isaac's sequence begins. What started the ch11 scene
// on the host (23.09.) was unknown; this ring records it.
struct RecentMessage { uint32_t hash, target; DWORD at; bool direct; };
constexpr unsigned kRecentMessages = 64;
RecentMessage recentMessages[kRecentMessages]{};
volatile LONG recentCount = 0;
// Scene trace ([Cutscene] SceneTrace). While a scene runs here and for 3 s
// after, every message this machine dispatches or sends directly is written
// with its tick, whether it came from a partner's replay, and its target's
// vtable; the two machines' timelines side by side show where a scene
// splits (24.09.: Tiedemann in the wrong animation on the pulled player).
// Its own ring, flushed with the diagnostics, so the event lines are not
// pushed out.
bool sceneTraceEnabled = false;
volatile LONG sceneTraceUntil = 0;
constexpr unsigned kTraceLines = 512;
char traceLines[kTraceLines][176]{};
unsigned traceCount = 0;
volatile LONG traceDropped = 0, traceWritten = 0;
std::mutex traceLock;
bool SceneTraceOn() {
    return sceneTraceEnabled && static_cast<LONG>(GetTickCount() - static_cast<DWORD>(InterlockedCompareExchange(&sceneTraceUntil, 0, 0))) <= 0;
}
void SceneTrace(const char* format, ...) {
    std::lock_guard<std::mutex> guard(traceLock);
    if (traceCount == kTraceLines) { InterlockedIncrement(&traceDropped); return; }
    va_list args;
    va_start(args, format);
    std::vsnprintf(traceLines[traceCount++], sizeof(traceLines[0]), format, args);
    va_end(args);
    InterlockedIncrement(&traceWritten);
}
void NoteRecentMessage(uint32_t hash, uint32_t target, bool direct) {
    if (coop_internal::IsInternal(hash)) return;   // the engine's own per-frame traffic buried the level's messages
    static struct { uint32_t hash, target; DWORD at; } lastTrace[512];
    const DWORD traceNow = GetTickCount();
    auto& lt = lastTrace[(hash ^ target) % 512];
    const bool repeat = lt.hash == hash && lt.target == target && traceNow - lt.at < 500;   // per-frame traffic: once per half second
    if (!repeat) { lt.hash = hash; lt.target = target; lt.at = traceNow; }
    if (!repeat && SceneTraceOn())
        SceneTrace("coop_scene t=%lu %s %08X %s target_vt=%08X%s", static_cast<unsigned long>(GetTickCount()), direct ? "direct" : "local ",
                   hash, coop_names::Name(hash), target, (mirroring > 0 || replaying > 0) ? " (partner replay)" : "");
    const LONG n = InterlockedIncrement(&recentCount) - 1;
    RecentMessage& r = recentMessages[static_cast<unsigned>(n) % kRecentMessages];
    r.hash = hash; r.target = target; r.at = GetTickCount(); r.direct = direct;
}

// A player's own action (24.09., choice A): a sequence that starts right after
// the local Isaac used a switch, a panel or a machine is his action (climbing
// into a shaft, the kinesis panel, the eye poke) and pulls nobody along. The use
// is seen as a message whose source (the handle at +8) is one of these
// classes; the result per source vtable is cached.
bool personalActions = false;   // [Mirror] PersonalActions
volatile LONG lastUseAt = 0;
char lastUseClass[32] = "";
struct UseClass { const char* name; uint32_t hash; };
UseClass useClasses[] = {
    {"Switch", 0}, {"SimplePressAndHoldSwitch", 0}, {"PowerNodeSwitch", 0}, {"HackingStationMiniGame", 0}, {"EyePokeMachine", 0},
};
struct UseCache { uint32_t vtable; int index; };
UseCache useCache[64]{};
unsigned useCacheCount = 0;
void NoteUse(const void* message) {
    uint32_t source = 0, table = 0, object = 0, id = 0, vtable = 0;
    if (!personalActions || !message || !Read(reinterpret_cast<uintptr_t>(message) + 8, &source, 4) || !source ||
        !Read(base + kHandleTableRva, &table, 4) || !table || !Read(table + (source >> 23) * 8, &object, 4) ||
        !Read(table + (source >> 23) * 8 + 4, &id, 4) || id != source || !object || !Read(object, &vtable, 4) ||
        vtable < base + kRdataLo || vtable >= base + kRdataHi) return;
    int index = -2;
    for (unsigned i = 0; i < useCacheCount; ++i) if (useCache[i].vtable == vtable) { index = useCache[i].index; break; }
    if (index == -2) {
        index = -1;
        for (unsigned i = 0; i < std::size(useClasses) && index < 0; ++i) {
            if (!useClasses[i].hash) useClasses[i].hash = NameHash(useClasses[i].name);
            if (IsA(object, vtable, useClasses[i].hash)) index = static_cast<int>(i);
        }
        if (useCacheCount < std::size(useCache)) useCache[useCacheCount++] = UseCache{vtable, index};
    }
    if (index < 0) return;
    InterlockedExchange(&lastUseAt, static_cast<LONG>(GetTickCount()));
    std::snprintf(lastUseClass, sizeof(lastUseClass), "%s", useClasses[index].name);
}
// Audio trace ([Coop] AudioTrace). 25.09., the eye poke: after the scene the
// client, who had been inside the machine, had "hardly any ambience". Every
// message whose name is about sound (aud_, music, amb, snapshot, mix, loop)
// is logged with its path, so the two machines' logs can be laid side by side.
bool audioTrace = false;
volatile LONG audioLines = 0;
bool AudioName(const char* n) {
    if (!n || !n[0]) return false;
    if (_strnicmp(n, "aud_", 4) == 0) return true;
    return std::strstr(n, "music") || std::strstr(n, "amb") || std::strstr(n, "snapshot") || std::strstr(n, "_mix") ||
           std::strstr(n, "loop") || std::strstr(n, "_vo_") || std::strstr(n, "audio");
}
void AudioTrace(uint32_t hash, const char* path, uint32_t targetVtable) {
    if (!audioTrace) return;
    const char* n = coop_names::Name(hash);
    if (!AudioName(n)) return;
    static uint32_t lastHash = 0;
    static DWORD lastAt = 0;
    const DWORD now = GetTickCount();
    if (hash == lastHash && now - lastAt < 2000) return;   // a looping state message: once
    lastHash = hash; lastAt = now;
    if (InterlockedIncrement(&audioLines) > 800) return;
    if (targetVtable) Event("coop_audio t=%lu %s %08X %s target_vt=%08X", now, path, hash, n, targetVtable);
    else Event("coop_audio t=%lu %s %08X %s", now, path, hash, n);
}
// A cull-set message a partner's ghost set off in the local world (ghosttrigger).
constexpr uint32_t kStreamScripterVtableRva = 0x17221CC;
volatile LONG cullsDropped = 0;
bool GhostCull(uint32_t hash, uint32_t source) {
    if (!source || !peerTable.Count()) return false;
    uint32_t table = 0, object = 0, id = 0;
    if (!Read(base + kHandleTableRva, &table, 4) || !table || !Read(table + (source >> 23) * 8, &object, 4) ||
        !Read(table + (source >> 23) * 8 + 4, &id, 4) || id != source || !object) return false;
    // The partner's ghost as the source itself (12.10.: not one drop all session
    // through the touch record alone), or a touch trigger only his ghost touched.
    const bool ghostSource = ghosttrigger::cullGuard && (IsGhostPlayer(object) || IsGhostNecro(object));
    if (!ghostSource && !ghosttrigger::GhostOnlyTouch(object, GetTickCount())) return false;
    // One more than judged: a ninth listener would otherwise go unseen, and
    // the message dropped although something else hears it.
    uint32_t objects[9]{}, vtables[9]{};
    const unsigned n = Listeners(hash, objects, vtables, 9);
    if (!n || n > 8) return false;
    for (unsigned i = 0; i < n; ++i) if (vtables[i] != base + kStreamScripterVtableRva) return false;
    if (InterlockedIncrement(&cullsDropped) <= 100)
        Event("coop_gate %08X (%s) dropped: %s %08X, our Isaac did not - our rooms stay as ours", hash, coop_names::Name(hash),
              ghostSource ? "it came from a partner's ghost" : "a partner's ghost touched its trigger", object);
    return true;
}

// The same for the bots and level changes: a touch trigger queues its message
// with itself as the source, so a message from a trigger only a bot touched,
// heard by a LevelChange, would load another level because a bot walked (or
// was put by the call key) where the leader was not.
volatile LONG botLevelChangesHeld = 0;
constexpr uint32_t kLevelChangeNodeRva = 0x17588C4;
bool BotLevelChange(uint32_t hash, uint32_t source) {
    if (!source || !ghosttrigger::blockBotLevelChange) return false;
    uint32_t table = 0, object = 0, id = 0;
    if (!Read(base + kHandleTableRva, &table, 4) || !table || !Read(table + (source >> 23) * 8, &object, 4) ||
        !Read(table + (source >> 23) * 8 + 4, &id, 4) || id != source || !object) return false;
    if (!ghosttrigger::BotOnlyTouch(object, GetTickCount())) return false;
    uint32_t objects[16]{}, vtables[16]{};
    const unsigned n = Listeners(hash, objects, vtables, 16);
    bool levelChange = false;
    for (unsigned i = 0; i < n; ++i) levelChange = levelChange || vtables[i] == base + kLevelChangeNodeRva;
    if (!levelChange) return false;
    if (InterlockedIncrement(&botLevelChangesHeld) <= 20)
        Event("coop_gate %08X (%s) dropped: only a bot touched its trigger %08X and a LevelChange listens - the leader decides levels", hash,
              coop_names::Name(hash), object);
    return true;
}

struct DispatchDepth {
    DispatchDepth() { ++gateDispatchDepth; }
    ~DispatchDepth() { --gateDispatchDepth; }
    DispatchDepth(const DispatchDepth&) = delete;
    DispatchDepth& operator=(const DispatchDepth&) = delete;
};
// Daina's FuncSpawn completion starts her idle clip on the local actor.
constexpr uint32_t kDainaSpawnPose = 0x6F416682;
volatile LONG dainaSpawnPoses = 0;
bool __cdecl OnDispatch(void* message, int flags) {
    if (g_selfDrive) DriveSelf();   // skipped when nested (see DriveSelf)
    const DispatchDepth depth;
    InterlockedIncrement(&dispatched);
    {
        // A Zone visit's map never ends: its end messages are dropped whoever sends them.
        uint32_t zoneHash = 0;
        if (message && Read(reinterpret_cast<uintptr_t>(message), &zoneHash, 4) && zoneHash && zoneobj::Drops(zoneHash, replaying > 0 || mirroring > 0))
            return true;
    }
    {
        // An arena match plays no campaign hallucination (its Nicole and dementia scripts
        // can lock the arena's doors), wherever the message comes from.
        uint32_t arenaHash = 0;
        if (message && arena::RunningNow() && Read(reinterpret_cast<uintptr_t>(message), &arenaHash, 4) && arenaHash && arena::BlocksHallucination(arenaHash))
            return true;
    }
    if (message && replaying == 0 && mirroring == 0) {
        uint32_t cullHash = 0, cullSource = 0;
        if (Read(reinterpret_cast<uintptr_t>(message), &cullHash, 4) && cullHash && Read(reinterpret_cast<uintptr_t>(message) + 8, &cullSource, 4) &&
            (GhostCull(cullHash, cullSource) || BotLevelChange(cullHash, cullSource))) return true;
        if (cullHash && worldcalls::DoorCloseCull(cullHash)) return true;
    }
    {
        uint32_t recentHash = 0;
        if (message && Read(reinterpret_cast<uintptr_t>(message), &recentHash, 4) && recentHash) {
            AudioTrace(recentHash, replaying > 0 ? "replayed" : mirroring > 0 ? "mirrored" : "local", 0);
            NoteRecentMessage(recentHash, 0, false);
            if (!mirroring && !replaying) story_objects::Opening(recentHash);
            if (recentHash == kDainaSpawnPose) {
                if (InterlockedIncrement(&dainaSpawnPoses) <= 12)
                    Event("coop_scene Daina spawn pose dispatched locally (nested=%d)", (replaying || mirroring) ? 1 : 0);
                return dispatchHook.call<bool>(message, flags);
            }
            if (replaying == 0 && mirroring == 0 && objonce::OnLocal(recentHash)) return true;   // this round already ran it here
            spawnwin::Note(recentHash);   // the MP maps' spawn points moving with the objectives
            if (replaying == 0 && mirroring == 0 && !coop_internal::IsInternal(recentHash)) NoteUse(message);
        }
    }
    uint32_t hash = 0, source = 0;
    if (!message || replaying > 0 || mirroring > 0 || !Read(reinterpret_cast<uintptr_t>(message), &hash, 4) || !hash) {
        if (replaying > 0 || mirroring > 0) {
            InterlockedIncrement(&passedNested);
            // A sequence cue inside a replayed chain still marks the window.
            uint32_t nestedHash = 0;
            if (message && Read(reinterpret_cast<uintptr_t>(message), &nestedHash, 4) && nestedHash) {
                if (mirror::HoldNestedStory(nestedHash, message, flags, GetTickCount())) return true;
                if (mirror::StoryActorCue(nestedHash)) return dispatchHook.call<bool>(message, flags);
                const Verdict& v = verdicts[nestedHash % kVerdicts];
                if (v.hash == nestedHash && v.progression && std::strcmp(v.cls, "ScriptedSequence") == 0)
                    InterlockedExchange(&lastSequenceTick, static_cast<LONG>(GetTickCount()));
                // Progression inside a replayed chain passes, but not silently.
                // 25.09., the eye poke: the client did it, its own chain held
                // set_ch11_post_eyepoke_checkpoint_game for the host; the host
                // played the client's outcome, its chain passed the same
                // checkpoint here without a vote, and the client waited on a
                // partner standing 0.4 m from the trigger (and everything the
                // checkpoint starts, the room's drone and music, with it).
                char nestedCls[32] = "";
                const DWORD nestedNow = GetTickCount();
                if (enabled && inLevel && peerTable.Count() && Classify(nestedHash, nestedNow, nestedCls, sizeof(nestedCls))) {
                    uint32_t nestedSource = 0;
                    Read(reinterpret_cast<uintptr_t>(message) + 8, &nestedSource, 4);
                    {
                        std::lock_guard<std::mutex> guard(lock);
                        announcer.Cast(nestedHash, nestedSource, nullptr, nestedNow, false);
                    }
                    InterlockedIncrement(&nestedVotes);
                    Event("coop_gate replayed progression %08X (%s, %s) passed here: vote sent", nestedHash, coop_names::Name(nestedHash), nestedCls);
                }
            }
        }
        return dispatchHook.call<bool>(message, flags);
    }
    NoteRecent(hash, GetTickCount());
    // The engine talking to itself (iMsg*): never held, never mirrored. One
    // exception: iMsgStreamSetLoadComplete (0x5FC7E489), "the next area is
    // in", is classified like any other message (held only while StreamSet is
    // in [Gate] Nodes). Held after the level has settled it acts as the Dead
    // Space 3 door: the first two-player run of 14:24 waited on exactly this
    // one, by accident, and the door opened for both together. At level start
    // it is let through by the settle guard below.
    if (hash == 0x5FC7E489u) {
        // The area that just finished loading: the stream module keeps it.
        uint32_t completedSource = 0;
        Read(reinterpret_cast<uintptr_t>(message) + 8, &completedSource, 4);
        streams::OnLoadComplete(completedSource);
    }
    if (coop_internal::IsInternal(hash) && hash != 0x5FC7E489u) {
        InterlockedIncrement(&passedInternal);
        return dispatchHook.call<bool>(message, flags);
    }
    // Hold the Zone opening the map: every machine does it for itself, so its
    // messages are neither held nor offered to the mirror.
    if (zoneobj::Local(hash)) {
        InterlockedIncrement(&passedZoneLocal);
        return dispatchHook.call<bool>(message, flags);
    }
    // Alone there is nobody to wait for, and nothing is even classified.
    if (peerTable.Count() == 0) {
        InterlockedIncrement(&passedSolo);
        return dispatchHook.call<bool>(message, flags);
    }
    if (NobodyConnected(GetTickCount())) {
        InterlockedIncrement(&passedLonely);
        return dispatchHook.call<bool>(message, flags);
    }
    // Before the local Player exists nothing is held either. The level's
    // start-up sequences fire in that window (eight-player run, 2026-09-17:
    // every one of them was held and timed out), and delaying what may be
    // the very sequence that places the player is not progression gating.
    // Nor before the level has settled: the stream set that
    // brings the starting area in fires during the load, and a held one is a
    // loading screen that never ends (host, 14:23).
    if (!LocalSlotZeroEntity() || !levelSettled) {   // a necro body is a player for gating purposes
        InterlockedIncrement(&passedNoPlayer);
        // Passed, but not silently: if it is progression, the partner who
        // settled first is holding it and waiting for this machine's vote. In
        // the 16:20 new game the host held two intro sequences the client fired in
        // this window and never voted for, and sat 45 s on each.
        if (enabled && inLevel && peerTable.Count() && necro::LocalRole() == 0 && !spectate::Active()) {   // a necro at the vents votes for nothing
            char earlyCls[32] = "";
            if (!mirror::StoryActorCue(hash) && Classify(hash, GetTickCount(), earlyCls, sizeof(earlyCls))) {
                uint32_t earlySource = 0;
                Read(reinterpret_cast<uintptr_t>(message) + 8, &earlySource, 4);
                std::lock_guard<std::mutex> guard(lock);
                announcer.Cast(hash, earlySource, nullptr, GetTickCount(), false);
                InterlockedIncrement(&earlyVotes);
            }
        }
        return dispatchHook.call<bool>(message, flags);
    }
    const DWORD now = GetTickCount();
    char cls[32] = "";
    // Not progression, or the gate is switched off: offered to the world
    // mirror, then through. Enabled=0 only stops the holding. In Versus nothing
    // is held either: the other side never votes for what one side does (a
    // human's dispenser door would wait 180 s for the necro's machine).
    // A necro player's machine holds nothing and votes for nothing either: its body
    // reaches no story trigger, and the humans' progression reaches it mirrored.
    const bool necroPass = !play::VersusMode() && (necro::LocalRole() != 0 || spectate::Active());   // a dead human watching holds nothing either
    const bool versusPass = play::VersusMode() || necroPass;
    if (necroPass) InterlockedIncrement(&passedNecro);
    else if (versusPass) InterlockedIncrement(&passedVersus);
    if (mirror::RoutesStoryScene(hash, now) || !Classify(hash, now, cls, sizeof(cls)) || !enabled || versusPass) {
        if (g_mirrorHook && g_mirrorHook(hash, message, flags, cls)) { InterlockedIncrement(&heldForStreams); return true; }
        return dispatchHook.call<bool>(message, flags);
    }
    if (std::strcmp(cls, "StreamSet") == 0) streams::OnActivation(hash, static_cast<uint32_t>(flags));
    if (std::strcmp(cls, "ScriptedSequence") == 0) InterlockedExchange(&lastSequenceTick, static_cast<LONG>(now));
    // On the player's own exempt list: through, offered to the mirror.
    if (exempt.Has(hash) || mirror::Included(hash)) {   // an Include is mirrored, never held
        InterlockedIncrement(&passedExempt);
        if (g_mirrorHook && g_mirrorHook(hash, message, flags, cls)) { InterlockedIncrement(&heldForStreams); return true; }
        return dispatchHook.call<bool>(message, flags);
    }
    // Progression the others already passed: through without a hold.
    if (Preapproved(hash)) {
        InterlockedIncrement(&passedThrough);
        // Still a vote: the host never preapproves and may be holding this
        // very message (24.09. ch11 chamber exit: the client passed it twice
        // from the journal standing 1.5 m from the trigger, never voted, and
        // the host sat out the full 180 s).
        uint32_t passSource = 0;
        Read(reinterpret_cast<uintptr_t>(message) + 8, &passSource, 4);
        {
            std::lock_guard<std::mutex> guard(lock);
            announcer.Cast(hash, passSource, nullptr, now, false);
        }
        InterlockedIncrement(&preapprovedVotes);
        if (trace) Event("coop_gate passed (others were here, voting anyway) hash=%08X class=%s", hash, cls);
        return dispatchHook.call<bool>(message, flags);
    }
    Read(reinterpret_cast<uintptr_t>(message) + 8, &source, 4);
    if (SceneCarries(now)) {
        // Through, but with a vote: a partner outside the scene who holds
        // the same message is let go by it.
        {
            std::lock_guard<std::mutex> guard(lock);
            announcer.Cast(hash, source, nullptr, now, false);
        }
        InterlockedIncrement(&passedScene);
        Event("coop_gate passed (a scene carries the players) hash=%08X name=%s class=%s", hash, coop_names::Name(hash), cls);
        return dispatchHook.call<bool>(message, flags);
    }
    // Not sent by something a player did: through (opt-in, see sourceRule).
    char sourceCls[32] = "";
    if (sourceRule && hash != kStreamSetLoadComplete && !SourceIsPlayerCaused(source, sourceCls, sizeof(sourceCls))) {
        InterlockedIncrement(&passedSequenceStep);
        if (trace) Event("coop_gate passed (source not player-caused) hash=%08X source=%08X class=%s", hash, source, cls);
        if (g_mirrorHook && g_mirrorHook(hash, message, flags, cls)) { InterlockedIncrement(&heldForStreams); return true; }
        return dispatchHook.call<bool>(message, flags);
    }
    float at[3]{};
    // A sequence moves the player: the church grab drags Isaac metres away
    // from where he "voted" (20:10: both machines held the same cues, both
    // votes died on the drift, both timed out). A sequence vote has no anchor.
    const bool anchored = LocalPosition(at) && std::strcmp(cls, "ScriptedSequence") != 0;
    // Was it the local Isaac who pulled this, or a partner's ghost in this world?
    float triggerAt[3]{};
    bool awaitSelf = false;
    if (anchored && TriggerPosition(source, triggerAt) &&
        coop_gate::DistanceSquared(at, triggerAt) > triggerReach * triggerReach)
        awaitSelf = true;
    bool full = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        // A hold without the local vote when it was not the local Isaac (token 0 adds none).
        // A sequence gets less patience than a door: the partner may never
        // be able to produce it (host-only enemy intros, 17:06).
        const uint32_t patience = std::strcmp(cls, "ScriptedSequence") == 0 ? sequenceTimeoutMs : 0u;
        full = !queue.Hold(hash, source, now, awaitSelf ? 0u : peerReadiness.token, at, anchored, patience);
    }
    if (full) {
        // Full queue: through it goes, and the log says so.
        Event("coop_gate full, passed hash=%08X class=%s", hash, cls);
        return dispatchHook.call<bool>(message, flags);
    }
    bool noSlot = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        Pending* slot = nullptr;
        for (auto& p : pending) if (p.used && p.hash == hash) { slot = &p; break; }
        if (!slot) for (auto& p : pending) if (!p.used) { slot = &p; break; }
        if (slot) {
            const bool fresh = !slot->used;
            slot->used = true; slot->hash = hash; slot->flags = flags;
            if (fresh) { slot->heldAt = now; slot->awaitingSelf = awaitSelf; std::memcpy(slot->triggerAt, triggerAt, sizeof(triggerAt)); }
            std::memcpy(slot->raw, message, sizeof(slot->raw));
            std::snprintf(slot->cls, sizeof(slot->cls), "%s", cls);
        } else {
            noSlot = true;
        }
        if (!awaitSelf) announcer.Cast(hash, source, at, now, anchored);
    }
    if (noSlot) {
        // No copy to replay from: swallowing it would be a soft lock, so it passes
        // (the queue entry is released later as a stray, the vote still goes out).
        InterlockedIncrement(&pendingFull);
        Event("coop_gate pending full, passed hash=%08X class=%s", hash, cls);
        return dispatchHook.call<bool>(message, flags);
    }
    if (hash == kStreamSetLoadComplete) {
        // Copy what the listener will need while the source still exists.
        uint32_t table = 0, object = 0, id = 0, guidPtr = 0;
        std::lock_guard<std::mutex> guard(lock);
        for (auto& p : pending) {
            if (!p.used || p.hash != hash) continue;
            if (Read(base + kHandleTableRva, &table, 4) && table &&
                Read(table + (source >> 23) * 8, &object, 4) && Read(table + (source >> 23) * 8 + 4, &id, 4) &&
                id == source && object && Read(object + 8, &guidPtr, 4) && guidPtr && Read(guidPtr, p.streamGuid, 16))
                p.streamPayload = true;
            break;
        }
    }
    InterlockedIncrement(&held);
    if (trace) {
        uint32_t table = 0, object = 0, id = 0, sourceVtable = 0;
        if (Read(base + kHandleTableRva, &table, 4) && table && Read(table + (source >> 23) * 8, &object, 4) &&
            Read(table + (source >> 23) * 8 + 4, &id, 4) && id == source && object)
            Read(object, &sourceVtable, 4);
        Event("coop_gate hold hash=%08X name=%s source=%08X source_vtable=%08X class=%s required=%u%s", hash, coop_names::Name(hash), source,
              sourceVtable, cls, Required(now), awaitSelf ? " (pulled by a ghost - our vote waits for our Isaac)" : "");
    }
    // Shown from Tick, not from inside the dispatch.
    holdNotice = true;
    return true;
}

// What was released here a moment ago. 25.09., ch11 insider checkpoint: both
// voted, both released within 235 ms, and the partner's vote kept repeating
// for its three linger seconds; each repeat arrived as a new vote with
// no local hold behind it, and 1.5 s later both screens said PARTNER
// WAITING for a checkpoint everybody had passed. Those repeats are folded.
constexpr DWORD kAfterReleaseMs = 15000;
struct Released { uint32_t hash; DWORD at; };
Released recentlyReleased[16]{};
unsigned releasedNext = 0;
volatile LONG votesAfterRelease = 0;
// The pull to a waiting partner (Tick): which hold it times, and the last one
// it pulled for. Both reset on a restart, or the same checkpoint's hold in the
// reloaded world never pulled again.
struct PullWait { uint32_t waitHash, pulledHash; DWORD waitSince, waitLast; bool warned; };
PullWait pullWait{};
bool ReleasedRecently(uint32_t hash, DWORD now) {
    for (const auto& r : recentlyReleased)
        if (r.hash == hash && TicksSince(now, r.at) < kAfterReleaseMs) return true;
    return false;
}

// A partner's vote, from the receive loop.
void OnVote(const coop_gate_wire::Packet& packet, unsigned fromSlot, DWORD now) {
    InterlockedIncrement(&votesReceived);
    // A necro player's vote must not complete a quorum it is not part of (Required
    // leaves it out): it would release a hold before the last human got there.
    if (roles::IsNecro(SlotForToken(packet.token, fromSlot, now), now)) { InterlockedIncrement(&necroVotesIgnored); return; }
    const float at[3] = {packet.x, packet.y, packet.z};
    if (spectate::Active()) spectate::OnPartnerVote(packet.hash, at);   // a level change he waits at brings us back
    {
        std::lock_guard<std::mutex> guard(lock);
        if (ReleasedRecently(packet.hash, now)) InterlockedIncrement(&votesAfterRelease);
        else queue.Vote(packet.hash, packet.source, packet.token, now, at, !(packet.flags & coop_gate_wire::kFlagNoAnchor));
    }
    // The host passes votes on, once, for clients that do not reach each other directly.
    if (IsHost() && !(packet.flags & coop_gate_wire::kFlagRelayed)) {
        coop_gate_wire::Packet forward = packet;
        forward.flags |= coop_gate_wire::kFlagRelayed;
        RelayToOthers(&forward, sizeof(forward), fromSlot);
        InterlockedIncrement(&votesRelayed);
    }
}

// Replay a released message with exactly the bytes the dispatch saw. The
// source word is not a pointer but a handle: PlayerHelmetCommand::Activate
// (0x00883F80) resolves it through the table at 0x0201C0F4 with a generation
// check. So it is passed on untouched and the table decides whether it is
// still valid.
bool ReplayFilter(unsigned) { return true; }
void Replay(Pending& p, bool late, DWORD now) {
    if (!Registered(p.hash)) {
        InterlockedIncrement(&replayUnregistered);
        Event("coop_gate replay skipped hash=%08X class=%s: no listener registered here any more", p.hash, p.cls);
        // The banner cycle ends here too when nothing else is held.
        bool more = false;
        for (const auto& q : pending) if (q.used) { more = true; break; }
        if (!more) { holdNoticeSince = 0; holdNoticeShown = false; }
        return;
    }
    unsigned char raw[12];
    std::memcpy(raw, p.raw, sizeof(raw));
    const int flags = p.flags;
    // Stand-in source for the load-complete message (see Pending). The handle
    // table entry is borrowed for the call and put back right after. Only when
    // the real source is gone: a live one is better than an object whose
    // vtable is null (a listener calling into it faults mid-dispatch).
    uint32_t source = 0, table = 0, entry = 0, savedObject = 0, savedId = 0;
    uint32_t standIn[16]{};
    uint32_t guidCopy[4] = {p.streamGuid[0], p.streamGuid[1], p.streamGuid[2], p.streamGuid[3]};
    bool borrowed = false;
    std::memcpy(&source, p.raw + 8, 4);
    if (p.streamPayload && source && Read(base + kHandleTableRva, &table, 4) && table) {
        entry = table + (source >> 23) * 8;
        standIn[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(guidCopy));   // +8: the GUID pointer the listener reads
        const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(standIn));
        if (Read(entry, &savedObject, 4) && Read(entry + 4, &savedId, 4)) {
            if (savedObject && savedId == source) InterlockedIncrement(&replaySourceAlive);
            else if (WriteBytes(entry, &mine, 4) && WriteBytes(entry + 4, &source, 4)) borrowed = true;
        }
    }
    ++replaying;
    __try {
        dispatchHook.call<bool>(raw, flags);
    } __except (ReplayFilter(GetExceptionCode())) {
        if (borrowed) InterlockedIncrement(&replayStandInFaults);
        Event("coop_gate replay faulted hash=%08X%s", p.hash, borrowed ? " (stand-in source)" : "");
    }
    --replaying;
    if (borrowed) { WriteBytes(entry, &savedObject, 4); WriteBytes(entry + 4, &savedId, 4); }
    InterlockedIncrement(late ? &timedOut : &replayed);
    if (std::strcmp(p.cls, "ScriptedSequence") == 0) InterlockedExchange(&lastSequenceTick, static_cast<LONG>(now));
    if (g_passedHook) g_passedHook(p.hash);
    if (holdNoticeShown) notice::Show(late ? "CONTINUING WITHOUT PARTNER" : "PARTNER HERE - CONTINUING");
    // Nothing else held here: the banner cycle is over.
    bool more = false;
    for (const auto& q : pending) if (q.used) { more = true; break; }
    if (!more) { holdNoticeSince = 0; holdNoticeShown = false; }
    if (trace) Event("coop_gate release hash=%08X name=%s class=%s after=%u ms %s", p.hash, coop_names::Name(p.hash), p.cls,
                     static_cast<unsigned>(now - p.heldAt), late ? "TIMEOUT" : "quorum");
}

// From Poll, on the game thread: repeat the local votes, release what is ready.
void Tick(DWORD now) {
    if (InterlockedExchange(&restartPending, 0)) {
        // Asked by OnRestart, which comes before these are declared.
        pullWait = PullWait{};
        std::lock_guard<std::mutex> guard(lock);
        for (auto& r : recentlyReleased) r = Released{};
        releasedNext = 0;
    }
    if (!installed) return;
    if (holdNotice) { holdNotice = false; if (!holdNoticeSince) holdNoticeSince = now; }
    if (holdNoticeSince && !holdNoticeShown && now - holdNoticeSince >= kNoticeAfterMs) {
        holdNoticeShown = true;
        notice::Show("WAITING FOR PARTNER");
    }
    // Holds pulled by a ghost: the local vote is cast when the local Isaac arrives.
    {
        float at[3]{};
        if (LocalPosition(at)) {
            std::lock_guard<std::mutex> guard(lock);
            for (auto& p : pending) {
                if (!p.used || !p.awaitingSelf) continue;
                if (coop_gate::DistanceSquared(at, p.triggerAt) > triggerReach * triggerReach) continue;
                p.awaitingSelf = false;
                uint32_t source = 0;
                std::memcpy(&source, p.raw + 8, 4);
                queue.Vote(p.hash, source, peerReadiness.token, now, at, true);
                announcer.Cast(p.hash, source, at, now, true);
                if (trace) Event("coop_gate our Isaac reached the trigger of hash=%08X - voted", p.hash);
            }
        }
    }
    // A partner's vote with no local hold behind it, for a while now: he
    // stands at something this player has not reached. Tell the player, once
    // per wait, and point the way.
    // The notice is shown outside the queue lock: it calls into the game and
    // logs. (Event() used to take this mutex; MSVC's std::mutex throws
    // system_error on a recursive lock, two aborts at 18:34 and 18:37.)
    {
        char text[64] = "";
        uint32_t waitHash = 0;
        float best = -1.0f;
        {
            std::lock_guard<std::mutex> guard(lock);
            coop_gate::Held* wait = queue.PartnerWait(now, kNoticeAfterMs);
            // Once a minute per hash: the entry itself is short-lived (kLateVoteMs)
            // and comes back with every repeat of the partner's vote.
            static struct { uint32_t hash; DWORD at; } partnerNoticed[8]{};
            static DWORD lastPartnerNotice = 0;
            bool fresh = wait != nullptr && now - lastPartnerNotice >= 20000;   // one banner per 20 s, whatever the hash
            if (wait) for (const auto& n : partnerNoticed) if (n.hash == wait->hash && now - n.at < 60000) { fresh = false; break; }
            if (fresh) {
                unsigned oldest = 0;
                for (unsigned i = 1; i < 8; ++i) if (static_cast<int32_t>(partnerNoticed[i].at - partnerNoticed[oldest].at) < 0) oldest = i;
                partnerNoticed[oldest] = {wait->hash, now};
            lastPartnerNotice = now;
                float me[3]{};
                const bool here = LocalPosition(me);
                for (unsigned i = 0; i < wait->voterCount; ++i) {
                    if (!wait->anchored[i] || !here) continue;
                    const float d = std::sqrt(coop_gate::DistanceSquared(me, wait->votedAt[i]));
                    if (best < 0.0f || d < best) best = d;
                }
                // An anchorless vote is a partner who passed it (before his level
                // settled), not one standing there; nothing to walk to.
                if (best >= 0.0f) {
                    waitHash = wait->hash;
                    std::snprintf(text, sizeof(text), "PARTNER WAITING - %.0f M AWAY", best);
                }
            }
        }
        if (text[0]) {
            notice::Show(text);
            if (trace) Event("coop_gate partner waits at hash=%08X distance=%.1f", waitHash, best);
        }
    }
    if (pullAfterMs && peerTable.Count()) {
        float target[3]{};
        uint32_t hash = 0;
        bool have = false;
        {
            std::lock_guard<std::mutex> guard(lock);
            if (coop_gate::Held* wait = queue.PartnerWait(now, kNoticeAfterMs))
                for (unsigned i = 0; i < wait->voterCount && !have; ++i)
                    if (wait->anchored[i]) { std::memcpy(target, wait->votedAt[i], sizeof(target)); hash = wait->hash; have = true; }
        }
        // The held entry comes back with every repeat of his vote, so the wait is timed here.
        uint32_t& waitHash = pullWait.waitHash;
        uint32_t& pulledHash = pullWait.pulledHash;
        DWORD& waitSince = pullWait.waitSince;
        DWORD& waitLast = pullWait.waitLast;
        bool& warned = pullWait.warned;
        if (have) {
            if (hash != waitHash || now - waitLast > 5000) { waitHash = hash; waitSince = now; warned = false; }
            waitLast = now;
            float me[3]{};
            const bool away = LocalPosition(me) && coop_gate::DistanceSquared(me, target) > kPullMinDistance * kPullMinDistance;
            const bool outsideScene = !cutscene::Following() && !cutscene::SequenceRunningNow();
            if (away && outsideScene && hash != pulledHash) {
                if (!warned && pullAfterMs > 5000 && now - waitSince >= pullAfterMs - 5000) {
                    warned = true;
                    notice::Show("PARTNER WAITING - PULLING YOU IN 5 S");
                }
                if (now - waitSince >= pullAfterMs) {
                    pulledHash = hash;
                    InterlockedIncrement(&pulls);
                    char why[128];
                    std::snprintf(why, sizeof(why), "the partner waits at %08X (%s) for %u s", hash, coop_names::Name(hash),
                                  static_cast<unsigned>((now - waitSince) / 1000));
                    catchup::PullTo(target, why);
                }
            }
        }
    }
    if (peerTable.Count()) {
        std::lock_guard<std::mutex> guard(lock);
        announcer.token = peerReadiness.token;
        announcer.Repeat(now, [](const coop_gate_wire::Packet& p) {
            if (SendToPeers(&p, sizeof(p))) InterlockedIncrement(&votesSent);
        });
    }
    for (unsigned guard = 0; guard < 4; ++guard) {
        uint32_t hash = 0, source = 0;
        bool late = false;
        Pending copy{};
        bool found = false;
        {
            std::lock_guard<std::mutex> lockGuard(lock);
            if (!queue.TakeEach(hash, source, [now](uint32_t h) { return RequiredFor(h, now); }, now, timeoutMs, late, Where, driftLimit)) break;
            for (auto& p : pending) {
                if (!p.used || p.hash != hash) continue;
                copy = p; p = Pending{}; found = true;
                break;
            }
        }
        InterlockedIncrement(&released);
        {
            // The local vote did its job; a repeat would answer the next occurrence.
            std::lock_guard<std::mutex> lockGuard(lock);
            announcer.Retire(hash, now, kVoteLingerMs);
        }
        if (!found) { InterlockedIncrement(&stray); continue; }   // no local hold behind it
        {
            std::lock_guard<std::mutex> lockGuard(lock);
            recentlyReleased[releasedNext] = Released{hash, now};
            releasedNext = (releasedNext + 1) % std::size(recentlyReleased);
        }
        Replay(copy, late, now);
    }
}

// --- installation ---

// Fills a rule set from two INI keys: entity classes (asked by hash, so a
// subclass counts too) and script node classes (matched by vtable, from the
// table above).
void ReadRuleSet(FILE* log, const std::string& ini, RuleSet& rules, const char* section,
                 const char* classesKey, const char* classesDefault, const char* nodesKey, const char* nodesDefault) {
    rules.label = classesKey;
    char list[1024]{};
    GetPrivateProfileStringA(section, classesKey, classesDefault, list, sizeof(list), ini.c_str());
    char* next = nullptr;
    for (char* token = strtok_s(list, ",; ", &next); token && rules.entityCount < kMaxClasses; token = strtok_s(nullptr, ",; ", &next)) {
        auto& rule = rules.entities[rules.entityCount++];
        std::snprintf(rule.name, sizeof(rule.name), "%s", token);
        rule.hash = NameHash(rule.name);
    }
    char nodes[1024]{};
    GetPrivateProfileStringA(section, nodesKey, nodesDefault, nodes, sizeof(nodes), ini.c_str());
    next = nullptr;
    for (char* token = strtok_s(nodes, ",; ", &next); token && rules.nodeCount < kMaxClasses; token = strtok_s(nullptr, ",; ", &next)) {
        bool known = false;
        for (const auto& node : kKnownNodes) {
            if (_stricmp(node.name, token) != 0) continue;
            rules.nodeVtables[rules.nodeCount] = node.vtableRva;
            rules.nodeNames[rules.nodeCount++] = node.name;
            known = true;
        }
        if (!known) std::fprintf(log, "coop %s node class '%s' unknown (no vtable on record); ignored\n", section, token);
    }
    std::fprintf(log, "coop %s %s:", section, classesKey);
    for (unsigned i = 0; i < rules.entityCount; ++i) std::fprintf(log, " %s=%08X", rules.entities[i].name, rules.entities[i].hash);
    for (unsigned i = 0; i < rules.nodeCount; ++i) std::fprintf(log, " %s@%08X", rules.nodeNames[i], static_cast<unsigned>(base + rules.nodeVtables[i]));
    std::fprintf(log, "\n");
}
void ReadRules(FILE* log, const std::string& ini) {
    // StreamSet was the Dead Space 3 door: the trigger that brings the next
    // area in sits before the door into it, and that door stays shut until
    // the set is loaded. Held until everybody reached the trigger, the first
    // player waits in a loaded room at a closed door, and nobody is ever a
    // whole area ahead. It left the default at 22:30 (see below).
    // StreamScripter is not in the default: its messages are what a loaded
    // area fires next, on both machines within milliseconds, not something a
    // player walks into (15:56 run).
    // MultiManager was in from 17:10: a trigger's message usually has only a
    // MultiManager listening, and the chain it fires (cutscene, door, spawns)
    // is then sourced by the MultiManager, not player-caused, so it passes.
    // Holding at the trigger is what makes both players walk into it. It is
    // not in the built-in default any more.
    // ScriptedSequence left the gate 21:00: sequences are mirrored from
    // whoever fires them now (Mirror AnyClasses), so holding them here would
    // only delay the copy that arrives anyway.
    ReadRuleSet(log, ini, progressionRules, "Gate", "Classes", "SimpleOneTimeDoor,Mover,MoverController",
                "Nodes", "Checkpoint,LevelChange");   // StreamSet left 22:30: the streams module holds activations (client, shared streams),
                                                       // a held LoadComplete was the host stuck 95 s at an open door behind the client
    ReadRuleSet(log, ini, sourceRules, "Gate", "Sources", "TriggerBase,Player,StandardDoor,SimpleOneTimeDoor,Mover,MoverController",
                "SourceNodes", "StreamSet,Checkpoint,LevelChange");
    sourceRule = GetPrivateProfileIntA("Gate", "SourceRule", 0, ini.c_str()) != 0;
    rideTriggers = GetPrivateProfileIntA("Gate", "RideTriggers", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop_gate ride_triggers=%d (%u boxes that start a ride wait for every player: generated_ride_triggers.h)\n", rideTriggers ? 1 : 0,
                 static_cast<unsigned>(std::size(coop_rides::kRides)));
    sequenceTimeoutMs = static_cast<uint32_t>(GetPrivateProfileIntA("Gate", "SequenceTimeoutMs", 20000, ini.c_str()));
    coop_lists::Read(log, ini, "Gate", "Exempt", exempt);
    std::fprintf(log, "coop_gate source_rule=%d sequence_timeout_ms=%u\n", sourceRule ? 1 : 0, sequenceTimeoutMs);
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Gate", "Enabled", 1, ini.c_str()) != 0;
    trace = GetPrivateProfileIntA("Gate", "Trace", 1, ini.c_str()) != 0;
    timeoutMs = static_cast<uint32_t>(GetPrivateProfileIntA("Gate", "TimeoutMs", static_cast<int>(coop_gate::kDefaultTimeoutMs), ini.c_str()));
    peerGraceMs = static_cast<DWORD>(GetPrivateProfileIntA("Gate", "PeerGraceMs", 60000, ini.c_str()));
    if (timeoutMs < 5000) timeoutMs = 5000;
    announcer.repeatMs = timeoutMs + 5000;   // a vote outlives every hold it could complete
    driftLimit = static_cast<float>(GetPrivateProfileIntA("Gate", "DriftLimit", 12, ini.c_str()));
    if (driftLimit < 2.0f) driftLimit = 2.0f;
    triggerReach = static_cast<float>(GetPrivateProfileIntA("Gate", "TriggerReach", 10, ini.c_str()));
    pullAfterMs = static_cast<DWORD>(GetPrivateProfileIntA("Gate", "PullToWaitingPartnerMs", 0, ini.c_str()));
    std::fprintf(log, "coop_gate pull_to_waiting_partner_ms=%lu (0 = off; a player the group waits for at a progression point is put there)\n",
                 static_cast<unsigned long>(pullAfterMs));
    if (triggerReach < 2.0f) triggerReach = 2.0f;
    ReadRules(log, ini);
    // push ecx / mov edx,[esp+8] / mov ecx,[edx] / push esi / mov esi,ecx / push edi / mov edi,[0x0204CD8C]
    static const unsigned char kDispatch[] = {0x51,0x8B,0x54,0x24,0x08,0x8B,0x0A,0x56,0x8B,0xF1,0x57,0x8B,0x3D,0x8C,0xCD,0x04,0x02};
    // The randomizer's horde silencer wraps the same function; hook_site
    // takes the lock both use and chains behind it in either order.
    if (!hook_site::Install(log, "coop_gate dispatch", base + kDispatchRva, kDispatch, sizeof(kDispatch),
                            reinterpret_cast<void*>(OnDispatch), dispatchHook)) {
        std::fprintf(log, "coop_gate not installed; progression is not gated\n");
        return false;
    }
    installed = true;
    std::fprintf(log, "coop_gate enabled=%d trace=%d timeout_ms=%u drift=%.0f registry=%08X\n",
                 enabled, trace, timeoutMs, driftLimit, static_cast<unsigned>(base + kRegistryRva));
    return true;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG lastDispatched = -1;
    const LONG now = InterlockedCompareExchange(&dispatched, 0, 0);
    if (now != lastDispatched) {
        // Read under the lock, written without it: the game thread's dispatch takes this lock.
        unsigned waiting = 0, active = 0;
        uint32_t votesIn = 0, votesLate = 0, discarded = 0, droppedHolds = 0;
        {
            std::lock_guard<std::mutex> guard(lock);
            waiting = queue.Waiting(); active = announcer.Active();
            votesIn = queue.votesIn; votesLate = queue.votesLate; discarded = queue.discarded; droppedHolds = queue.dropped;
        }
        std::fprintf(log, "coop_gate dispatched=%ld classified=%ld progression=%ld held=%ld released=%ld replayed=%ld "
                          "timed_out=%ld stray=%ld waiting=%u votes_sent=%ld votes_recv=%ld votes_relayed=%ld "
                          "votes_in=%u votes_late=%u discarded=%u dropped=%u announcing=%u nested=%ld solo=%ld "
                          "no_player=%ld internal=%ld preapproved=%ld preapproved_votes=%ld not_player_caused=%ld early_votes=%ld query_faults=%ld "
                          "replay_unregistered=%ld restart_clears=%ld passed_scene=%ld nested_votes=%ld dropped_votes=%ld votes_after_release=%ld\n",
                     now, InterlockedCompareExchange(&classified, 0, 0), InterlockedCompareExchange(&progression, 0, 0),
                     InterlockedCompareExchange(&held, 0, 0), InterlockedCompareExchange(&released, 0, 0),
                     InterlockedCompareExchange(&replayed, 0, 0), InterlockedCompareExchange(&timedOut, 0, 0),
                     InterlockedCompareExchange(&stray, 0, 0), waiting,
                     InterlockedCompareExchange(&votesSent, 0, 0), InterlockedCompareExchange(&votesReceived, 0, 0),
                     InterlockedCompareExchange(&votesRelayed, 0, 0), votesIn, votesLate, discarded,
                     droppedHolds, active, InterlockedCompareExchange(&passedNested, 0, 0),
                     InterlockedCompareExchange(&passedSolo, 0, 0), InterlockedCompareExchange(&passedNoPlayer, 0, 0),
                     InterlockedCompareExchange(&passedInternal, 0, 0), InterlockedCompareExchange(&passedThrough, 0, 0),
                     InterlockedCompareExchange(&preapprovedVotes, 0, 0),
                     InterlockedCompareExchange(&passedSequenceStep, 0, 0), InterlockedCompareExchange(&earlyVotes, 0, 0),
                     InterlockedCompareExchange(&queryFaults, 0, 0), InterlockedCompareExchange(&replayUnregistered, 0, 0),
                     InterlockedCompareExchange(&restartClears, 0, 0), InterlockedCompareExchange(&passedScene, 0, 0),
                     InterlockedCompareExchange(&nestedVotes, 0, 0), InterlockedCompareExchange(&droppedVotes, 0, 0),
                     InterlockedCompareExchange(&votesAfterRelease, 0, 0));
        std::fprintf(log, "coop_gate passed_no_partner_connected=%ld pending_full=%ld replay_source_alive=%ld replay_stand_in_faults=%ld drive_self_nested=%ld zone_local=%ld "
                          "versus=%ld necro_machine=%ld necro_votes_ignored=%ld\n",
                     InterlockedCompareExchange(&passedLonely, 0, 0), InterlockedCompareExchange(&pendingFull, 0, 0),
                     InterlockedCompareExchange(&replaySourceAlive, 0, 0), InterlockedCompareExchange(&replayStandInFaults, 0, 0),
                     InterlockedCompareExchange(&driveSelfNested, 0, 0), InterlockedCompareExchange(&passedZoneLocal, 0, 0),
                     InterlockedCompareExchange(&passedVersus, 0, 0), InterlockedCompareExchange(&passedNecro, 0, 0),
                     InterlockedCompareExchange(&necroVotesIgnored, 0, 0));
        if (rideTriggers) std::fprintf(log, "coop_gate ride triggers classified as progression=%ld\n", InterlockedCompareExchange(&ridesHeld, 0, 0));
        lastDispatched = now;
    }
    // Both rings are copied under their locks and written after: Event and
    // SceneTrace on the game thread would otherwise wait for the file.
    {
        static char eventCopy[kEvents][sizeof(events[0])];
        unsigned count = 0, overwritten = 0;
        {
            std::lock_guard<std::mutex> guard(eventLock);
            for (unsigned i = 0; i < eventCount; ++i) std::memcpy(eventCopy[i], events[(eventHead + i) % kEvents], sizeof(eventCopy[i]));
            count = eventCount;
            eventCount = 0; eventHead = 0;
            overwritten = eventsOverwritten;
        }
        for (unsigned i = 0; i < count; ++i) std::fprintf(log, "%s\n", eventCopy[i]);
        static unsigned lastOverwritten = 0;
        if (overwritten != lastOverwritten) {
            std::fprintf(log, "coop events overwritten=%u (more than %u event lines in one second; the oldest were lost)\n", overwritten, kEvents);
            lastOverwritten = overwritten;
        }
    }
    {
        static char traceCopy[kTraceLines][sizeof(traceLines[0])];
        unsigned count = 0;
        {
            std::lock_guard<std::mutex> guard(traceLock);
            std::memcpy(traceCopy, traceLines, sizeof(traceLines[0]) * traceCount);
            count = traceCount;
            traceCount = 0;
        }
        for (unsigned i = 0; i < count; ++i) std::fprintf(log, "%s\n", traceCopy[i]);
    }
    static LONG lastDropped = 0;
    const LONG dropped = InterlockedCompareExchange(&traceDropped, 0, 0);
    if (dropped != lastDropped) { std::fprintf(log, "coop_scene trace lines dropped=%ld (ring full between two writes)\n", dropped); lastDropped = dropped; }
}
}  // namespace gate
