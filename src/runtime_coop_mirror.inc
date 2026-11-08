// Dead Space 2 United by Richard Hafer.
// World mirror: what happens to the host's world happens to everybody's.
//
// Each machine runs its own copy of the level logic, and the logic is driven
// by messages. Many of them only ever fire on the host: an enemy's death
// message (CMD_SetOnDeathMessage) comes out of an enemy that exists nowhere
// else, so a client's quarantine counter never counts, its doors never
// unlock, its vent never breaks although the ghost that broke it is walking
// out of it. Seen in the runs before 2026-09-17.
//
// So every machine repeats, to every other, each message whose listeners
// move the world (doors, movers, props), and the receiver replays it
// unless it fired the same message itself a moment ago (its own Isaac walked
// into the same door trigger). Doors were the visible case: one Isaac walks
// through, on everybody else's screen the door stays shut and the ghost clips
// through it, because the trigger in front of the door only answers to the
// local Player. Counters and objectives (VariableOperator, ObjectiveEntry...)
// are repeated by the HOST only: they count things that exist only there,
// enemies above all, and a count must have one author.
//
// What is NOT mirrored, by rule: animated and moving lights (AnimateLight,
// MoveLight: cosmetic, re-sent every frame, and each machine runs its own
// copy; dropped 23.09. for the host's network load), spawns
// (FuncSpawn: enemies stay the host's),
// streaming (StreamSet, StreamScripter), checkpoints and level changes (their
// own modules), scripted sequences (the gate), player commands (helmet,
// equip), and the pure relays, gates and Lua nodes: their OUTPUTS are
// mirrored, and mirroring the input as well would make the receiver's own
// relay fire the output a second time, which on a counter is a wrong number.
//
// A door opening twice is nothing. A quarantine counting one death twice is
// a door that never opens. That asymmetry decides every rule here.
//
// MultiManager is a relay, not a leaf. 2026-09-17 14:55 it sat in HostNodes
// and the host's `..._mm_front_open`, `mm_room_lower_enter`,
// `remove_area01_extras` were replayed on the client, whose MultiManagers
// then switched ITS stream sets to wherever the host had walked: "the map
// unloaded on the second player as soon as the host went into another room".
// Relays fan out to streams as readily as to doors; they stay excluded.
namespace mirror {
bool enabled = false, installed = false, trace = false, clientsMirror = false;
constexpr uint32_t kMagic = 0x5252494D;   // 'MIRR'
constexpr uint32_t kVersion = 1;
struct Packet {
    uint32_t magic, version, token, sequence;
    uint32_t hash, flags, tick;
    uint32_t reserved[10];
};
static_assert(sizeof(Packet) == 68, "Unexpected mirror wire layout");
constexpr uint32_t kSpawnWaitMark = 0x4E575053;   // 'SPWN' in reserved[5], GUID in reserved[0..3]
bool Valid(const Packet& p) { return p.magic == kMagic && p.version == kVersion && p.token && p.hash; }
// The host forwards a client's message to the other clients with this set;
// a forwarded message keeps its author's token and is never forwarded again.
constexpr uint32_t kFlagRelayed = 1;
// The journal: what happened since the host's last checkpoint, replayed to a
// player who arrives in the middle of it. The problem: a player joins
// at the checkpoint while the others have been playing past it for ten
// minutes, and every door they opened, every vent that broke, every death a
// quarantine counted happened in a world the joiner was not in. So the host keeps
// every message it mirrored since its last checkpoint, per name with a
// count, and a joiner asks for the lot once its level matches the host's.
// Reset whenever the host's death checkpoint moves: the joiner starts from
// that checkpoint, so that is where its missing history begins.
constexpr uint32_t kFlagJournalRequest = 2;   // client -> host: send me the journal
constexpr uint32_t kFlagJournal = 4;          // host -> client: one journal entry, reserved[0] = repeat count
constexpr uint32_t kFlagPassed = 8;           // journal entry: progression the host's gate released (not replayed, pre-approved)
// A direct send: the host delivered this message to ONE object, not to the
// registry. reserved[0..3] carry that object's GUID; the receiver finds its
// own copy of the object and delivers the same way. This is how a vent
// breaks (the NPC's vent machine tells the VentMarker), how a MultiManager
// opens its door, how most of the world actually moves; none of it ever
// passed the broadcast dispatch, which is why the first mirror saw doors
// only through their relays and vents not at all.
constexpr uint32_t kFlagDirect = 16;
// A C++ call on a world object, not a message at all. Vents (17:20): the
// necromorph's exit state machine calls VentMarker::SetCoverState(bool)
// straight on the vent; no message ever passes the dispatch or the direct
// send, so the first two mirrors could not see a vent break. The hook
// on such a call sends the object's GUID, a call id and the arguments
// (reserved[0..3] GUID, payload = {id, args...}); the client finds its own
// copy and makes the same call. Call ids live in runtime_coop_worldcalls.inc.
constexpr uint32_t kFlagCall = 32;
// A local sequence cue waits for the partners' streams (seqwait): the same
// cue goes out at once with this bit, and a partner only loads the sender's stream set
// for it, so the cue itself can later play everywhere at the same moment.
constexpr uint32_t kFlagDeferred = 0x80000000u;
constexpr uint32_t kFlagPrepare = 0x40000000u;
volatile LONG prepareSent = 0, prepareReceived = 0, prepareFollowed = 0;

gate::RuleSet worldRules;    // listeners that move the world: repeated by whoever fires it
gate::RuleSet hostRules;     // listeners that count the world: repeated by the host alone
gate::RuleSet anyRules;      // repeated by ANY machine, host or client (sequences since 21:00)
gate::RuleSet voiceRules;    // repeated by any machine, but no cutscene: no stream wait, no reference
gate::RuleSet excludeRules;  // listeners that mean "not to be repeated"
DWORD dedupeMs = 5000;

// Host: what went out, repeated a few times because one lost datagram would
// be one door that never opens over there.
constexpr unsigned kOutbox = 64;
struct Out { Packet packet; DWORD at; unsigned sent; };
Out outbox[kOutbox]{};
unsigned outHead = 0;
uint32_t sequence = 0;
constexpr unsigned kRepeats = 3;
constexpr DWORD kRepeatMs = 150;
constexpr DWORD kOutboxMaxAgeMs = 2000;   // well past the last repeat (2 x kRepeatMs after the first send)
volatile LONG outboxExpired = 0, staleSequences = 0;

// Receiver: which sequences were already replayed, per author (the sequence
// stored at its own slot; a retransmit finds itself there), and what is
// waiting for the game thread.
uint32_t seen[coop_peers::kMaxPeers][256]{};
// Whose sequences a row holds. A partner who restarts his game comes back
// with a new token and counts from one again; without this his first 255
// messages found "their" slot already taken and were dropped as repeats.
uint32_t seenToken[coop_peers::kMaxPeers]{};
// The highest sequence taken per author. A journal delivery burns through
// more than 256 sequences inside the repeat window, so the ring slot of a
// late repeat already holds a newer number; anything 256 or more behind the
// highest is such a repeat and is dropped.
uint32_t seenHighest[coop_peers::kMaxPeers]{};
// The last history request answered per partner: the request now repeats
// like everything else, and a repeat must not restart a delivery under way.
uint32_t lastJournalAsk[coop_peers::kMaxPeers]{};
constexpr unsigned kInbox = 64;
Packet inbox[kInbox]{};
unsigned inCount = 0;
std::mutex lock;

struct Verdict { uint32_t hash; DWORD at; bool mirror; bool counts; bool sequence; bool multiManager; bool voice; char cls[32]; };

// A story cue plays once per machine, from whoever fires it first ([Mirror] StoryCuesOnce).
// Sequence and dialogue cues are shared from any machine and the receiver only folded
// its own copy within DedupeMs. A player behind the group walks into the trigger of a
// scene the group played minutes ago: his cue went out again and every partner's NPC
// restarted that scene in a room whose animations were long unloaded (T-pose), and his
// own NPC replayed it too. Neutral NPC spawns had the same gap after 120 s (a second
// Edgar standing at the spawn point). So the second copy from another origin is dropped,
// as a one-shot trigger never fires twice alone. A cue one origin fires twice is an
// authored repeat (a loop, a room entry) and stays shared. Cleared with the world queues.
struct OnceBook {
    static constexpr unsigned char kLocal = 0xFE;
    struct Entry { uint32_t key; unsigned char origin; bool repeatable; };
    Entry entries[1024]{};
    unsigned used = 0;
    volatile LONG swallowedLocal = 0, droppedPartner = 0, repeats = 0, full = 0, lines = 0;
    std::mutex m;
    Entry* Find(uint32_t key, bool add) {
        if (!key) return nullptr;
        for (unsigned i = 0, at = (key * 2654435761u) % 1024; i < 1024; ++i, at = (at + 1) % 1024) {
            if (entries[at].key == key) return &entries[at];
            if (!entries[at].key) {
                if (!add || used >= 900) return nullptr;
                ++used;
                entries[at] = Entry{key, 0, false};
                entries[at].origin = 0xFF;
                return &entries[at];
            }
        }
        return nullptr;
    }
    // The origin that played it here first, or -1 when the copy from `origin` may play.
    int Taken(uint32_t key, unsigned char origin) {
        std::lock_guard<std::mutex> guard(m);
        const Entry* e = Find(key, false);
        return e && !e->repeatable && e->origin != origin ? e->origin : -1;
    }
    void Note(uint32_t key, unsigned char origin) {
        std::lock_guard<std::mutex> guard(m);
        Entry* e = Find(key, true);
        if (!e) { InterlockedIncrement(&full); return; }
        if (e->origin == 0xFF) e->origin = origin;
        else if (e->origin == origin && !e->repeatable) { e->repeatable = true; InterlockedIncrement(&repeats); }
    }
    void Clear() {
        std::lock_guard<std::mutex> guard(m);
        std::fill(std::begin(entries), std::end(entries), Entry{});
        used = 0;
    }
};
bool storyOnce = true;   // [Mirror] StoryCuesOnce
OnceBook storyCues, neutralBook;
const char* OriginName(int origin, char* out, size_t size) {
    if (origin == OnceBook::kLocal) std::snprintf(out, size, "%s", "our own trigger");
    else std::snprintf(out, size, "partner %d", origin);
    return out;
}
Verdict verdicts[512]{};

// Host: the journal, in first-seen order. Counts matter (five deaths are
// five) and are capped where a state tick would otherwise dominate.
constexpr unsigned kJournal = 256, kJournalRepeatCap = 64;
// A direct send is not always twelve bytes. set_metamodel_var (A5ECDE4F)
// to a ScriptableProp carries its variable after the header, and replayed
// as a bare header it faulted on the client (16:20). So the host copies what
// follows the header too (kPayload bytes, plain data as far as anyone can
// tell), and the client delivers the same bytes. A message that faults
// anyway is not replayed again: for a direct send only to that one object,
// and only until the world is rebuilt. 25.09.: DCD05F43 ("rws: send message",
// the animation event every ScriptableProp hears) faulted once on the finale's
// marker heart and, remembered by hash alone, was dropped for every prop
// until the game was closed.
constexpr unsigned kPayload = 24;    // bytes 12..36 of the message
constexpr unsigned kMessageCopy = 12 + kPayload;
struct Entry { uint32_t hash, flags, count; uint32_t guid[4]; unsigned char payload[kPayload]; };
constexpr unsigned kFaulted = 32;
struct FaultKey { uint32_t hash, guid0, guid1; };
FaultKey faulted[kFaulted]{};
unsigned faultedHead = 0;
volatile LONG faultedSkips = 0;
bool Faulted(uint32_t hash, uint32_t guid0 = 0, uint32_t guid1 = 0) {
    for (const auto& f : faulted) if (f.hash == hash && f.guid0 == guid0 && f.guid1 == guid1) return true;
    return false;
}
void RememberFault(uint32_t hash, uint32_t guid0 = 0, uint32_t guid1 = 0) {
    if (Faulted(hash, guid0, guid1)) return;
    faulted[faultedHead] = FaultKey{hash, guid0, guid1};
    faultedHead = (faultedHead + 1) % kFaulted;
}
Entry journal[kJournal]{};
unsigned journalCount = 0;
LONG journalCheckpoint = 0;
// Pending journal deliveries: one peer slot, position in the journal.
struct Delivery { bool active; unsigned slot; unsigned next; };
Delivery deliveries[coop_peers::kMaxPeers]{};
// Client: journal asked for once per host level epoch and own map.
uint32_t journalAskedEpoch = 0;
char journalAskedMap[64]{};
// History does not repeat.
//
// The case: the player who arrived late walks through rooms the group
// finished. His own triggers are fresh: they fire again, lock the quarantine
// doors again, and the mirror would carry that to the players up front, who
// would find their doors shut behind a fight that is over. So a hash that is
// already in the history a client received is replayed locally (his own
// world needs it) but never sent out again; and the host drops an incoming
// mirror whose hash its own journal already holds. What the group did since
// the checkpoint stays done.
constexpr unsigned kHistory = 256;
uint32_t history[kHistory]{};
unsigned historyCount = 0;
bool InHistory(uint32_t hash) {
    for (unsigned i = 0; i < historyCount; ++i) if (history[i] == hash) return true;
    return false;
}
void RememberHistory(uint32_t hash) {
    if (InHistory(hash)) return;
    if (historyCount < kHistory) history[historyCount++] = hash;
}
// [Mirror] Journal: 0 off, 1 everywhere, 2 match modes only. Story partners all
// load in together at the host's checkpoint restart, so nobody there needs the
// group's history, and a history replay was the source of double cues and
// counters taken as "already done" (audit 16.10. items 5, 7, 14).
int journalMode = 2;
bool JournalOn() { return journalMode == 1 || (journalMode == 2 && !play::StoryCoop()); }
volatile LONG journalSkippedStory = 0;
bool InJournal(uint32_t hash) {
    if (!JournalOn()) return false;
    for (unsigned i = 0; i < journalCount; ++i) if (journal[i].hash == hash) return true;
    return false;
}
volatile LONG heldBack = 0;
volatile LONG journalSent = 0, journalReplayed = 0, journalRequests = 0, journalResets = 0;

void JournalNote(uint32_t hash, uint32_t flags, const uint32_t* guid = nullptr, const unsigned char* payload = nullptr) {
    if (!JournalOn()) { InterlockedIncrement(&journalSkippedStory); return; }
    for (unsigned i = 0; i < journalCount; ++i) {
        if (journal[i].hash != hash) continue;
        if (guid && std::memcmp(journal[i].guid, guid, 16) != 0) continue;   // same name, another object
        if (journal[i].count < kJournalRepeatCap) ++journal[i].count;
        journal[i].flags = flags;
        if (payload) std::memcpy(journal[i].payload, payload, kPayload);   // the latest value wins
        return;
    }
    if (journalCount < kJournal) {
        Entry e{hash, flags, 1, {}, {}};
        if (guid) std::memcpy(e.guid, guid, 16);
        if (payload) std::memcpy(e.payload, payload, kPayload);
        journal[journalCount++] = e;
    }
}
// The host's gate released this: the joiner may pass it without a vote.
void OnPassed(uint32_t hash) {
    if (!IsHost() || !installed || !JournalOn()) return;
    std::lock_guard<std::mutex> guard(lock);
    for (unsigned i = 0; i < journalCount; ++i)
        if (journal[i].hash == hash && (journal[i].flags & kFlagPassed)) return;
    if (journalCount < kJournal) journal[journalCount++] = Entry{hash, kFlagPassed, 1, {}};
}
void JournalReset() {
    journalCount = 0;
    for (auto& d : deliveries) d.active = false;
    InterlockedIncrement(&journalResets);
}
// A checkpoint restart, here or on the host: the journal starts over and a
// client's memory of what "already happened" is wiped, or a door the group
// opened before the restart would be held back / pre-approved the second time.
void ClearWorldQueues();   // below
// Everything the local world fires that is NOT shared, once per message and
// level ([Mirror] GapTrace): the reason and the listeners' vtables. 25.09.,
// the psych hall props: a withheld MultiRemoveTarget left the client with two
// objects the host had deleted. A withheld message whose result shows up in
// only one world (tools/coop_world_diff.py) is the next such gap.
bool gapTrace = false;
uint32_t gapSeen[2048]{};
volatile LONG gapLines = 0, gapWithheld = 0;
void NoteGap(uint32_t hash, const char* why, const char* cls) {
    if (!gapTrace || !hash || !peerTable.Count() || !inLevel || gate::mirroring || gate::replaying || coop_internal::IsInternal(hash)) return;
    unsigned slot = (hash * 2654435761u) & (std::size(gapSeen) - 1);
    for (unsigned probe = 0; probe < 16; ++probe, slot = (slot + 1) & (std::size(gapSeen) - 1)) {
        if (gapSeen[slot] == hash) return;
        if (!gapSeen[slot]) break;
    }
    gapSeen[slot] = hash;
    uint32_t objects[6]{}, vtables[6]{};
    const unsigned n = gate::Listeners(hash, objects, vtables, 6);
    if (!n) return;   // nobody hears it: nothing can differ
    InterlockedIncrement(&gapWithheld);
    if (InterlockedIncrement(&gapLines) > 400) return;
    char list[96] = "";
    size_t used = 0;
    for (unsigned i = 0; i < n && used < sizeof(list) - 10; ++i) used += std::snprintf(list + used, sizeof(list) - used, " %08X", vtables[i]);
    gate::Event("coop_gap %08X (%s) not shared: %s%s%s, listeners%s", hash, coop_names::Name(hash), why, cls && cls[0] ? " " : "",
                cls ? cls : "", n ? list : " none");
}
namespace scene_delivery { void Clear(); }
void OnRestart() {
    ClearWorldQueues();
    std::lock_guard<std::mutex> guard(lock);
    // Messages of the old world still queued either way: on a slow link they
    // would land in the reloaded one (a door opened again, a vent re-broken).
    scene_delivery::Clear();
    for (auto& o : outbox) o.packet.hash = 0;
    inCount = 0;
    if (IsHost()) JournalReset();
    historyCount = 0;
    journalAskedEpoch = 0;   // ask the host for the (new, empty) journal again
}

volatile LONG offered = 0, mirrored = 0, sent = 0, received = 0, duplicate = 0, folded = 0,
              replayed = 0, dropped = 0, excluded = 0, relayed = 0, throttled = 0, unregistered = 0, notInLevel = 0;
// A hash goes out at most this often. The first two-player run mirrored 9303
// messages in ten minutes: a handful of names that a mover or a light
// re-sends every frame. Those are state, not events; once per half second
// carries the state and spares the wire.
constexpr DWORD kPerHashMs = 500;
struct Sent { uint32_t hash; DWORD at; };
Sent lastSent[512]{};

// Messages named in the INI: [Mirror] Include= is repeated by
// whoever fires it, whatever listens (a sequence's "freed" cue, so that one
// shooter frees both; the church, 20:10); [Mirror] ExcludeHashes= is never
// repeated. Both by name or hash.
coop_lists::List include, excludeHashes;
bool Included(uint32_t hash) { return include.Has(hash); }

bool ChurchActorsSettling(DWORD now);
#include "runtime_coop_scene_agreement.inc"

#include "runtime_coop_scenechain.inc"

bool StoryActorCue(uint32_t hash) {
    return scenechain::ActorOnlyCue(hash) && installed && enabled && play::StoryCoop() && !scenechain::Owns(hash);
}
bool RoutesStoryScene(uint32_t hash, DWORD now) {
    if (installed && enabled && scenechain::Owns(hash)) return true;
    if (StoryActorCue(hash)) return true;
    const auto policy = scene_catalog::PolicyFor(hash);
    if (policy == scene_catalog::Policy::Unknown || !installed || !enabled || mpmap::InVisit() || play::SurvivalMode() || play::VersusMode()) return false;
    return policy == scene_catalog::Policy::Personal || agreement::SupportsCue(hash, now);
}

// A local sequence cue that waits for the partners' streams (see
// streams::SceneReady). Kept in order, released in order, once per hash.
void SendPrepare(uint32_t hash, const void* message, int flags, const uint32_t* set);
namespace seqwait {
bool enabled = true;
DWORD waitMs = 90000;
constexpr unsigned kMax = 64;
constexpr DWORD kRetryMs = 250, kNoticeAfterMs = 1500;
struct Item { uint32_t hash; int flags; unsigned char raw[12]; DWORD since; uint32_t set[4]; DWORD preparedAt; uint32_t attempt, map, generation, rank; };
uint32_t nextAttempt = 0;
Item items[kMax]{};
unsigned count = 0;
DWORD lastTry = 0;
bool noticeShown = false;
volatile LONG taken = 0, released = 0, timedOut = 0;
bool Take(uint32_t hash, const void* message, int flags, DWORD now, const char* who, const uint32_t* required, bool managed = false, uint32_t rank = 1) {
    for (unsigned i = 0; i < count; ++i) if (items[i].hash == hash) return true;   // a trigger re-firing: already waiting
    if (count >= kMax || (!managed && count >= 16)) {
        gate::Event("coop_mirror scene queue full hash=%08X managed=%d", hash, managed);
        return managed;
    }
    Item& it = items[count++];
    it.hash = hash; it.flags = flags; it.since = now;
    std::memcpy(it.raw, message, sizeof(it.raw));
    InterlockedIncrement(&taken);
    gate::Event("coop_mirror sequence waits for the partners' streams hash=%08X name=%s: %s", hash, coop_names::Name(hash), who);
    std::memcpy(it.set, required, sizeof(it.set));
    it.preparedAt = now;
    it.attempt = managed ? ++nextAttempt : 0;
    if (managed && !it.attempt) it.attempt = ++nextAttempt;
    it.rank = rank;
    it.map = loadsync::SceneMapHash();
    it.generation = loadsync::SceneGeneration();
    if (count == 1 && !managed) SendPrepare(hash, message, flags, it.set);
    return true;
}

// 22:20: a player waiting for his cutscene must not be able to walk
// past it. Two levers on the local Isaac, both written every frame while the
// wait lasts and both undone before the cue plays: the position row (+0xC0)
// is put back where the wait began (the stand-in creature is pinned the
// same way, and that holds), and the animation clock (AnimView +0x194 ->
// +0x40 -> speed +0x134) is set to 0, so no locomotion advances. The clock
// value is read once and restored exactly; the position write counts its
// corrections and the largest drift it undid, so the log says whether the
// pin was ever fought.
bool freezeEnabled = true;
bool frozen = false;
float frozenAt[3]{}, savedSpeed = 1.0f;
uint32_t savedSpeedAt = 0;
unsigned corrections = 0;
float maxDrift = 0.0f;
volatile LONG freezes = 0;
uint32_t AnimSpeedAddress(uint32_t player) {
    uint32_t view = 0, inner = 0;
    float speed = 0.0f;
    if (!Read(player + kAnimViewOffset, &view, 4) || !view || !Read(view + kAnimViewInner, &inner, 4) || !inner) return 0;
    if (!Read(inner + kAnimSpeedInner, &speed, 4) || !(speed >= 0.0f && speed <= 8.0f)) return 0;   // not the known clock
    return inner + kAnimSpeedInner;
}
void Freeze() {
    const uint32_t player = LocalSlotZeroEntity();   // Isaac or the necro body: both are pinned the same way
    if (!player) return;
    if (!frozen) {
        if (!Read(player + placement::kPlayerPosition, frozenAt, 12)) return;
        savedSpeedAt = AnimSpeedAddress(player);
        if (savedSpeedAt) Read(savedSpeedAt, &savedSpeed, 4);
        frozen = true; corrections = 0; maxDrift = 0.0f;
        InterlockedIncrement(&freezes);
        gate::Event("coop_mirror waiting: Isaac held at %.1f %.1f %.1f (anim clock %s)", frozenAt[0], frozenAt[1], frozenAt[2], savedSpeedAt ? "stopped" : "not found");
        return;
    }
    float now[3]{};
    if (Read(player + placement::kPlayerPosition, now, 12)) {
        const float drift = std::sqrt(coop_gate::DistanceSquared(now, frozenAt));
        if (drift > 0.01f) { ++corrections; if (drift > maxDrift) maxDrift = drift; WriteBytes(player + placement::kPlayerPosition, frozenAt, 12); }
    }
    if (savedSpeedAt && AnimSpeedAddress(player) == savedSpeedAt) { const float zero = 0.0f; WriteBytes(savedSpeedAt, &zero, 4); }
}
void Unfreeze() {
    if (!frozen) return;
    frozen = false;
    // The saved clock address lives inside the body's AnimView. If the body was
    // replaced meanwhile (necromorph hand-over, restart) that memory is freed and
    // possibly recycled: only write it back while it still belongs to slot 0.
    if (savedSpeedAt && AnimSpeedAddress(LocalSlotZeroEntity()) == savedSpeedAt) WriteBytes(savedSpeedAt, &savedSpeed, 4);
    else if (savedSpeedAt) savedSpeedAt = 0;
    gate::Event("coop_mirror waiting over: Isaac released (position corrections=%u max_drift=%.2f m, anim clock %s)", corrections, maxDrift, savedSpeedAt ? "restored" : "untouched");
    savedSpeedAt = 0;
}
// Everything waiting is dropped (a restart, a level change): nothing of the
// old world is played into the new one.
void Clear() {
    count = 0;
    noticeShown = false;
    Unfreeze();
}
}  // namespace seqwait

// The partner won this occurrence. Its committed cue replaces our saved local dispatch.
void DropLocalCandidate(uint32_t hash) {
    using namespace seqwait;
    for (unsigned i = 0; i < count; ++i) {
        if (!items[i].attempt || items[i].hash != hash) continue;
        --count;
        std::memmove(items + i, items + i + 1, (count - i) * sizeof(Item));
        if (!count) { noticeShown = false; Unfreeze(); }
        return;
    }
}
bool SubmitStoryStart(uint32_t hash, const void* message, int flags, DWORD now, const uint32_t* set) {
    const auto policy = scene_catalog::PolicyFor(hash);
    if (!seqwait::enabled || (policy != scene_catalog::Policy::Pull && policy != scene_catalog::Policy::Gather) ||
        !RoutesStoryScene(hash, now) || !levelSettled || !inLevel) return false;
    uint32_t trigger = 0;
    bool ambiguous = false;
    const uint32_t actor = ghosttrigger::SceneActor(message, now, trigger, ambiguous);
    uint32_t rank = 1;
    if (cutscene::Leading() || (actor && actor == LocalSlotZeroEntity())) rank = 0;
    else if (actor && IsGhostPlayer(actor)) rank = 2;
    return seqwait::Take(hash, message, flags, now, policy == scene_catalog::Policy::Pull ? "native player placement" : "group arrival", agreement::StartSet(hash, set), true, rank);
}

// Every sequence cue that waits, from Tick: released when the partners are
// here, or after waitMs regardless.
constexpr uint32_t kSceneSetMark = 0x54455343;
void SendToSlot(unsigned slot, const Packet& p);
#include "runtime_coop_scene_delivery.inc"
void SendLocal(uint32_t hash, const void* message, int flags, const uint32_t* set = nullptr, uint32_t attempt = 0, uint32_t parent = 0);
bool HoldGroupStep(uint32_t hash, const void* message, int flags) {
    if (scenechain::owner != peerReadiness.token &&
        (scenechain::ChurchTransition(hash) || scene_catalog::IsPromptBranch(scenechain::root, hash))) return true;
    if (!scenechain::First(hash)) return true;
    if (scenechain::promptBranches && scene_catalog::IsPromptBranch(scenechain::root, hash))
        gate::Event("coop_mirror prompt branch chosen root=%08X cue=%08X owner=%08X attempt=%u",
                    scenechain::root, hash, scenechain::owner, scenechain::attempt);
    if (scenechain::owner == peerReadiness.token) SendLocal(hash, message, flags, nullptr, scenechain::attempt, scenechain::root);
    return false;
}
bool HoldNestedStory(uint32_t hash, const void* message, int flags, DWORD now) {
    if (scenechain::Owns(hash)) return HoldGroupStep(hash, message, flags);
    const auto policy = scene_catalog::PolicyFor(hash);
    if (policy != scene_catalog::Policy::Pull && policy != scene_catalog::Policy::Gather) return false;
    if (storyOnce && storyCues.Taken(hash, OnceBook::kLocal) >= 0 && RoutesStoryScene(hash, now)) return true;
    return SubmitStoryStart(hash, message, flags, now, streams::area);
}
void SequenceTick(DWORD now) {
    using namespace seqwait;
    if (!count) return;
    if (!inLevel || !LocalSlotZeroEntity()) { Clear(); return; }
    if (frozen && freezeEnabled) Freeze();   // every frame: the pin
    if (now - lastTry < kRetryMs) return;
    lastTry = now;
    char who[128] = "";
    Item& head = items[0];
    const bool managed = head.attempt != 0;
    if (managed) {
        const uint32_t map = loadsync::SceneMapHash(), generation = loadsync::SceneGeneration();
        if (!map) return;
        if ((head.map && head.map != map) || (head.generation && generation && head.generation != generation)) {
            gate::Event("coop_mirror discarded scene from previous load hash=%08X attempt=%u", head.hash, head.attempt);
            Clear();
            agreement::Clear();
            return;
        }
        head.map = map;
        head.generation = generation;
    }
    if (managed && streams::GuidZero(head.set)) {
        if (streams::GuidZero(streams::area)) {
            if (!noticeShown && now - head.since >= kNoticeAfterMs) { noticeShown = true; notice::Show("WAITING FOR SCENE ROOMS"); }
            return;
        }
        std::memcpy(head.set, streams::area, sizeof(head.set));
    }
    if (managed) agreement::Request(head.hash, head.attempt, head.set, now, head.rank);
    const bool here = !peerTable.Count() || (managed ? agreement::Ready(head.hash, head.attempt, now)
                                                    : streams::SceneReady(head.set, now, who, sizeof(who)));
    const bool late = !managed && !here && now - head.since >= waitMs;
    if (!here && !late) {
        if (!managed && now - head.preparedAt >= 2000) {
            head.preparedAt = now;
            if (!streams::Covers(head.set)) streams::Follow(head.set);
            SendPrepare(head.hash, head.raw, head.flags, head.set);
        }
        if (freezeEnabled && !managed) Freeze();
        else if (frozen) Unfreeze();
        if (!noticeShown && now - head.since >= kNoticeAfterMs) { noticeShown = true; notice::Show("WAITING FOR PARTNER - CUTSCENE"); }
        return;
    }
    Unfreeze();
    // Everything queued goes, in order: once the partners are here the
    // whole chain belongs to them too.
    Item batch[kMax];
    unsigned n = 1;
    while (!managed && n < count && !items[n].attempt && !late && streams::GuidEqual(items[n].set, head.set)) ++n;
    std::memcpy(batch, items, sizeof(Item) * n);
    count -= n;
    std::memmove(items, items + n, count * sizeof(Item));
    for (unsigned i = 0; i < n; ++i) {
        if (batch[i].attempt) {
            const bool handoff = batch[i].hash == scenechain::kChurch && agreement::Available(now, agreement::kChurchHandoff);
            const bool qte = scene_catalog::IsPromptRoot(batch[i].hash);
            scenechain::Start(batch[i].hash, peerReadiness.token, batch[i].attempt, now, handoff,
                              qte && agreement::Available(now, agreement::kCampaignSceneUpdate));
            if (storyOnce) storyCues.Note(batch[i].hash, OnceBook::kLocal);
            agreement::Consume(batch[i].hash, peerReadiness.token);
        }
        SendLocal(batch[i].hash, batch[i].raw, batch[i].flags, batch[i].set, batch[i].attempt);
        streams::SetReference(0xFEu, now);   // local cutscene: this machine leads the streams
        ++gate::replaying;
        __try {
            gate::dispatchHook.call<bool>(batch[i].raw, batch[i].flags);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            gate::Event("coop_mirror waiting sequence faulted hash=%08X", batch[i].hash);
        }
        --gate::replaying;
        InterlockedExchange(&gate::lastSequenceTick, static_cast<LONG>(now));
        InterlockedIncrement(late ? &timedOut : &released);
        gate::Event("coop_mirror sequence released hash=%08X name=%s after %u ms %s", batch[i].hash, coop_names::Name(batch[i].hash),
                    static_cast<unsigned>(now - batch[i].since), late ? "TIMEOUT" : "(partners here)");
    }
    if (noticeShown) notice::Show(late ? "CONTINUING WITHOUT PARTNER" : "PARTNER HERE - CONTINUING");
    noticeShown = false;
}

// A scene the partner walked into is his ([Mirror] GhostSequenceHoldMs, 0 = off). 02.11.: the host
// went ahead to a door, his own scene waited for the client's streams (seqwait), and in that time
// his ghost entered the same trigger on the client: the client's game started the QTE on its own
// Isaac far behind, the client led, and the host who opened the door followed without a prompt.
// A local sequence cue within kEntryWindowMs of a partner ghost's trigger entry that our Isaac
// did not make waits for the partner's own copy; it plays here only after holdMs without one.
extern bool finaleActive;   // below, with the finale
namespace ghostseq {
DWORD holdMs = 20000;
constexpr DWORD kEntryWindowMs = 1500;
constexpr float kNearM = 4.0f;
constexpr unsigned kMax = 64;
struct Item { uint32_t hash; int flags; unsigned char raw[12]; DWORD since; unsigned author; uint32_t set[4], token, epoch; };
Item items[kMax]{};
unsigned count = 0;
volatile LONG held = 0, partnerCopies = 0, timedOut = 0, nearSkipped = 0, full = 0;
volatile LONG confirmedReleases = 0, requests = 0, replies = 0, unknownStarts = 0, startMismatches = 0;
constexpr uint32_t kSceneMagic = 0x314E4353;  // SCN1
struct ScenePacket {
    uint32_t magic, version, sender, target, epoch, reply, guid[4];
    uint32_t startAgeMs, reserved[6];
};
static_assert(sizeof(ScenePacket) == sizeof(Packet), "Scene confirmations share the mirror receive branch");
struct Confirmation { uint32_t token, epoch; DWORD askedAt, confirmedAt; };
Confirmation confirmations[coop_peers::kMaxPeers]{};
uint32_t sceneEpoch = 0, startGuid[4]{};
DWORD sceneAt = 0;
void BeginScene(uint32_t sequenceObject, DWORD now) {
    if (!++sceneEpoch) ++sceneEpoch;
    sceneAt = now;
    if (!Read(sequenceObject + 0x24, startGuid, sizeof(startGuid)) ||
        streams::GuidZero(startGuid) || ObjectByGuid(startGuid) != sequenceObject) {
        std::memset(startGuid, 0, sizeof(startGuid));
        if (++unknownStarts <= 16) gate::Event("coop_mirror missing scene start GUID sequence=%08X epoch=%u", sequenceObject, sceneEpoch);
    }
}
void Clear() {
    count = 0;
    std::memset(startGuid, 0, sizeof(startGuid));
    std::memset(confirmations, 0, sizeof(confirmations));
}
bool Confirmed(unsigned slot, uint32_t token, DWORD now) {
    const auto& c = confirmations[slot];
    return c.token == token && c.epoch == sceneEpoch && c.confirmedAt && TicksSince(now, c.confirmedAt) < 1500;
}
bool OnScenePacket(const ScenePacket& p, unsigned from, DWORD now) {
    if (p.version != 1 || p.reply > 1 || !p.epoch || !KnownPeerToken(p.sender) || !p.target ||
        from >= coop_peers::kMaxPeers || (IsHost() && peerTable.peers[from].token != p.sender)) return false;
    if (p.target != peerReadiness.token) {
        if (IsHost() && KnownPeerToken(p.target)) RelayToOthers(&p, sizeof(p), from);
        return true;
    }
    if (!inLevel || (!count && p.reply) || streams::GuidZero(startGuid)) return true;
    const unsigned slot = SlotForToken(p.sender, from, now);
    if (slot >= coop_peers::kMaxPeers) return true;
    if (p.reply) {
        auto& c = confirmations[slot];
        if (streams::GuidEqual(p.guid, startGuid) && cutscene::Leading() && p.epoch == sceneEpoch && c.epoch == p.epoch && c.token == p.sender && c.askedAt) c.confirmedAt = now;
    } else if (cutscene::FollowsSlot(slot)) {
        const DWORD age = TicksSince(now, sceneAt);
        const DWORD skew = age >= p.startAgeMs ? age - p.startAgeMs : p.startAgeMs - age;
        if (!streams::GuidEqual(p.guid, startGuid) || skew > netclock::Window(3000, 6000)) {
            if (++startMismatches <= 16)
                gate::Event("coop_mirror scene confirmation refused peer=%u local=%08X-%08X remote=%08X-%08X start_skew_ms=%u",
                            slot, startGuid[0], startGuid[1], p.guid[0], p.guid[1], skew);
            return true;
        }
        ScenePacket ack = p;
        ack.reply = 1;
        ack.target = p.sender;
        ack.sender = peerReadiness.token;
        if (SendToPeers(&ack, sizeof(ack))) ++replies;
    }
    return true;
}
bool SharedStart(Item& it, DWORD now) {
    if (!cutscene::Leading() || !it.token || peerTable.peers[it.author].token != it.token || streams::GuidZero(startGuid)) return false;
    // A ghost can trip the trigger one tick before the local scene begins.
    if (!it.epoch && static_cast<LONG>(sceneAt - it.since) >= 0 && sceneAt - it.since <= kEntryWindowMs) it.epoch = sceneEpoch;
    if (it.epoch != sceneEpoch) return false;
    if (Confirmed(it.author, it.token, now)) return true;
    auto& c = confirmations[it.author];
    if (c.token != it.token || c.epoch != sceneEpoch || !c.askedAt || TicksSince(now, c.askedAt) >= 200) {
        c = Confirmation{it.token, sceneEpoch, now, 0};
        ScenePacket probe{kSceneMagic, 1, peerReadiness.token, it.token, sceneEpoch, 0};
        std::memcpy(probe.guid, startGuid, 16);
        probe.startAgeMs = TicksSince(now, sceneAt);
        if (SendToPeers(&probe, sizeof(probe))) ++requests;
    }
    return false;
}
unsigned SlotOfGhost(uint32_t ghost) {
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s)
        if (peerTable.peers[s].used && GhostForMachine(peerTable.peers[s].machine) == ghost) return s;
    return 0xFFu;
}
// From the local dispatch, before the story book notes the cue as ours.
bool Take(uint32_t hash, const void* message, int flags, DWORD now) {
    if (!holdMs || !peerTable.Count() || finaleActive || cutscene::Following()) return false;
    for (unsigned i = 0; i < count; ++i) if (items[i].hash == hash) return true;
    uint32_t trigger = 0;
    bool ambiguous = false;
    const uint32_t actor = ghosttrigger::SceneActor(message, now, trigger, ambiguous);
    if (ambiguous) return false;
    if (actor && actor == LocalSlotZeroEntity()) return false;
    unsigned author = 0xFFu;
    if (actor) {
        author = SlotOfGhost(actor);
        if (author >= coop_peers::kMaxPeers) return false;
    } else if (count && !trigger) {
        author = items[count - 1].author;
    } else {
        const uint32_t sourceTrigger = trigger;
        uint32_t ghost = 0;
        if (!ghosttrigger::GhostEnteredFirst(now, kEntryWindowMs, ghost, trigger)) return false;
        if (sourceTrigger && sourceTrigger != trigger) return false;
        author = SlotOfGhost(ghost);
        if (author >= coop_peers::kMaxPeers) return false;
        float his[3]{}, me[3]{};
        const uint32_t local = LocalSlotZeroEntity();
        if (!local || !Read(local + 0xC0, me, 12) || !Read(ghost + 0xC0, his, 12)) return false;
        const float dx = his[0] - me[0], dy = his[1] - me[1], dz = his[2] - me[2];
        if (dx * dx + dy * dy + dz * dz < kNearM * kNearM) { InterlockedIncrement(&nearSkipped); return false; }   // side by side: either may lead
        gate::Event("coop_mirror sequence %08X (%s) started by partner %u's ghost in trigger %08X, our Isaac %.1f m away: it waits for his own copy "
                    "(up to %lu ms)", hash, coop_names::Name(hash), author, trigger, std::sqrt(dx * dx + dy * dy + dz * dz),
                    static_cast<unsigned long>(holdMs));
    }
    if (!count && cutscene::Leading() && Confirmed(author, peerTable.peers[author].token, now)) return false;
    if (count >= kMax) { InterlockedIncrement(&full); return false; }
    Item& it = items[count++];
    if (actor && held < 64)
        gate::Event("coop_mirror scene cue=%08X trigger=%08X actor=%08X attributed to peer=%u", hash, trigger, actor, author);
    it.hash = hash; it.flags = flags; it.since = now; it.author = author;
    it.token = peerTable.peers[author].token;
    it.epoch = cutscene::Leading() ? sceneEpoch : 0;
    std::memcpy(it.set, streams::area, sizeof(it.set));
    std::memcpy(it.raw, message, sizeof(it.raw));
    InterlockedIncrement(&held);
    return true;
}
void Tick(DWORD now) {
    if (!count) return;
    if (!inLevel || !LocalSlotZeroEntity()) { count = 0; return; }
    unsigned kept = 0;
    for (unsigned i = 0; i < count; ++i) {
        Item& it = items[i];
        if (scenechain::HasPlayed(it.hash)) {
            InterlockedIncrement(&partnerCopies);
            gate::Event("coop_mirror dropped queued cue=%08X already played in scene=%08X attempt=%u", it.hash, scenechain::root, scenechain::attempt);
            continue;
        }
        const int first = storyCues.Taken(it.hash, OnceBook::kLocal);
        if (first >= 0) {
            InterlockedIncrement(&partnerCopies);
            gate::Event("coop_mirror sequence %08X (%s) held for partner %u: his copy played here after %u ms, ours dropped", it.hash, coop_names::Name(it.hash),
                        it.author, static_cast<unsigned>(now - it.since));
            continue;
        }
        const bool shared = SharedStart(it, now);
        if (!shared && now - it.since < holdMs && peerTable.Count()) { items[kept++] = it; continue; }
        if (shared) {
            if (++confirmedReleases <= 64)
                gate::Event("coop_mirror shared scene %08X-%08X epoch=%u follower=%u released cue=%08X after %u ms",
                            startGuid[0], startGuid[1], sceneEpoch, it.author, it.hash, static_cast<unsigned>(now - it.since));
        } else {
            InterlockedIncrement(&timedOut);
            gate::Event("coop_mirror unconfirmed ghost cue=%08X peer=%u epoch=%u waited=%u ms",
                        it.hash, it.author, it.epoch, static_cast<unsigned>(now - it.since));
        }
        if (SubmitStoryStart(it.hash, it.raw, it.flags, now, it.set)) continue;
        storyCues.Note(it.hash, OnceBook::kLocal);
        char why[128] = "";
        if (seqwait::enabled && peerTable.Count() && (seqwait::count || !streams::SceneReady(it.set, now, why, sizeof(why))) &&
            seqwait::Take(it.hash, it.raw, it.flags, now, why, it.set)) continue;
        SendLocal(it.hash, it.raw, it.flags, it.set);
        streams::SetReference(0xFEu, now);
        ++gate::replaying;
        __try {
            gate::dispatchHook.call<bool>(it.raw, it.flags);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            gate::Event("coop_mirror held sequence faulted hash=%08X", it.hash);
        }
        --gate::replaying;
    }
    count = kept;
}
}  // namespace ghostseq
void SendPrepare(uint32_t hash, const void* message, int flags, const uint32_t* set) {
    (void)message;
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> guard(lock);
    Out& o = outbox[outHead];
    outHead = (outHead + 1) % kOutbox;
    o.packet = Packet{kMagic, kVersion, peerReadiness.token, ++sequence, hash, kFlagPrepare | (static_cast<uint32_t>(flags) << 8), now, {}};
    if (set && !streams::GuidZero(set)) {
        std::memcpy(o.packet.reserved, set, 16);
        o.packet.reserved[5] = kSceneSetMark;
    }
    o.at = now - kRepeatMs;
    o.sent = 0;
    InterlockedIncrement(&prepareSent);
}
// Sender side: the message goes to the outbox (host: and the journal).
void SendLocal(uint32_t hash, const void* message, int flags, const uint32_t* set, uint32_t attempt, uint32_t parent) {
    (void)message;
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> guard(lock);
    // Byte +4 of a message is the dispatch's in-flight marker (the dispatch
    // writes 1 there and puts it back), not data: it stays out of the packet,
    // whose low byte carries the mirror's own flags.
    Packet packet{kMagic, kVersion, peerReadiness.token, ++sequence, hash, static_cast<uint32_t>(flags) << 8, now, {}};
    if (set && (attempt || !streams::GuidZero(set))) {
        std::memcpy(packet.reserved, set, 16);
        packet.reserved[5] = kSceneSetMark;
        packet.reserved[6] = attempt;
        if (attempt && hash == scenechain::kChurch && scenechain::churchHandoff) packet.reserved[7] = 1;
        if (attempt && scenechain::promptBranches) packet.reserved[7] |= agreement::kCampaignSceneUpdate;
    }
    if (parent) {
        packet.reserved[5] = scenechain::kMark;
        packet.reserved[6] = attempt;
        packet.reserved[7] = parent;
    }
    InterlockedIncrement(&mirrored);
    if (!parent && attempt && packet.reserved[5] == kSceneSetMark && scene_delivery::Start(packet, now)) return;
    Out& o = outbox[outHead];
    outHead = (outHead + 1) % kOutbox;
    o.packet = packet;
    o.at = now - kRepeatMs;   // first send on the next tick
    o.sent = 0;
    if (IsHost() && !parent && !attempt) JournalNote(hash, o.packet.flags);
}
void SendLocalWithGuid(uint32_t hash, const void* message, int flags, const uint32_t* guid) {
    SendLocal(hash, message, flags);
    std::lock_guard<std::mutex> guard(lock);
    Out& o = outbox[(outHead + kOutbox - 1) % kOutbox];
    std::memcpy(o.packet.reserved, guid, 16);
    o.packet.reserved[5] = kSpawnWaitMark;
}
// What a host's FuncSpawn announces. With HostNPCs a client suppresses the
// spawn, so its FuncSpawn never finishes and never sends SpawnDone (or any
// other message of its own). 24.09., ch11: "ses_11_01_nicole_spawned" reaches
// a MultiManager that switches on vfx_igc_11_01_nicole_light_on, the massive
// blue light, and the client stood without it. MultiManagers are excluded
// from the mirror, so the source decides here: whatever a FuncSpawn (vtable
// 0x01B59C58, written by its ctor 0x00EA5A40) sends on the host is repeated,
// each one, never throttled. The source handle sits at message+8 and is
// resolved through the handle table the gate uses.
// That alone never matched (24.09., sent=0): SpawnDone (Xbox 0x842F81B8)
// sends CMsg(event, BasePtrMsgData(spawned)): the handle at +8 is the data
// carrying the new NPC, not the spawner. So the SpawnDone names are read off
// the spawners themselves: FuncSpawn+0x0C is its attributes, +0x20 on them the
// SpawnDone message hash (live 24.09.: spawn_ses_11_01_nicole -> 48A21418).
bool spawnerMessages = false;   // [Mirror] SpawnerMessages
constexpr uint32_t kFuncSpawnVtableRva = 0x1759C58, kSimManagerRva = 0x1C1C0A0;
constexpr uint32_t kFuncSpawnAttributes = 0x0C, kSpawnDoneMessage = 0x20;
void TraceStalkerRequest(uint32_t hash, const char* stage) {
    if (!trace) return;
    switch (hash) {
        case 0xB83470C2: // Left entrance taunter and its four wave spawners.
        case 0xAAD119E9:
        case 0x7DEAD7EA:
        case 0x510495EB:
        case 0x241E53EC:
            break;
        default: return;
    }
    static LONG lines = 0;
    if (InterlockedIncrement(&lines) > 100) return;
    const uint32_t object = gate::ListenerWithVtable(hash, base + kFuncSpawnVtableRva);
    uint32_t attributes = 0, target[4]{};
    int32_t limit = -1, spawned = -1, pending = -1;
    if (object && Read(object + kFuncSpawnAttributes, &attributes, 4) && attributes) {
        Read(attributes + 0x0C, target, sizeof(target));
        Read(attributes + 0x1C, &limit, 4);
        Read(object + 0xC0, &spawned, 4);
        Read(object + 0xC8, &pending, 4);
    }
    gate::Event("coop_mirror stalker spawn %s request=%08X spawner=%08X template=%08X-%08X-%08X-%08X count=%d max=%d queued=%d",
                stage, hash, object, target[0], target[1], target[2], target[3], spawned, limit, pending);
}
constexpr unsigned kMaxSpawnDone = 512;
uint32_t spawnDone[kMaxSpawnDone]{};
unsigned spawnDoneCount = 0;
volatile LONG spawnerSent = 0, spawnerNamed = 0, spawnDoneScans = 0;
// Scene props spawned by the trigger that starts a scene. 24.09.: ch11's
// TriggerPhysical 4FCB35C9 -> mm_se_11_06_start spawns Tiedemann AND his
// javelin gun (spawn_javelin_gun_b, FuncSpawn 52CB38C8, max 1); only the
// leader touches that trigger and MultiManagers are not mirrored, so the
// follower's Tiedemann had no gun. A FuncSpawn (attributes at +0x0C: +0x00
// its request name, +0x0C the template GUID, +0x1C max spawns) that spawns one
// non-NPC thing has its request mirrored from any machine; a partner replays
// it once he follows a scene (plus kPropSettleMs, so the actor the prop is
// attached to exists), and max 1 keeps a second request from spawning twice.
bool propSpawns = false;   // [Mirror] SceneProps
constexpr uint32_t kPropSpawnMark = 0x504F5250;   // 'PROP' in reserved[5]
// Neutral NPCs spawned by the leader's scene. 24.09., the Tormentor (ch12
// igc_03_03): the host's door chain spawned the Unitologists and the security
// men (msg_spawn_npcm_security_igc_03_03_a..c, msg_spawn_npcf_unitologist...)
// through FuncSpawns; with NeutralNPCsLocal=1 the host does not ghost them and
// the client never fired the requests, so its scene played without them. The
// spawners only exist once the scene's stream set has loaded, too late for
// the three-second scan, so the dispatch asks the registry for a FuncSpawn
// listener itself. The receiver waits until the request is registered here
// (the partner's set has loaded) and plays it.
bool neutralSpawns = false;   // [Mirror] NeutralNPCSpawns
constexpr uint32_t kNeutralSpawnMark = 0x5350434E;   // 'NCPS' in reserved[5]
constexpr uint32_t kNeutralDescriptorRva = 0x1CB4830;   // NeutralNPC, as runtime_coop_npcs.inc
constexpr DWORD kNeutralWaitMs = 10000;
// A follower's own sequence cue, sent to the leader as a catch-up. 25.09.,
// Tiedemann: after both machines fired 5225A301, only the follower's world
// went on (E225D1BA, then the javelin's gun_aim_on); the leader's never did,
// and the follower's cues are kept from him; both players stood stuck, the
// leader for 14 s. The leader now plays such a cue only when his own world has
// not fired it within kFollowerCueGraceMs of its arrival: a scene that runs
// normally folds it, a leader stuck behind his follower is pulled on.
bool followerCueCatchUp = false;   // [Mirror] FollowerCueCatchUp, off: the leader never plays a follower's cue
constexpr uint32_t kFollowerCueMark = 0x45554346;   // 'FCUE' in reserved[5]
constexpr DWORD kFollowerCueGraceMs = 3000;
struct FollowerCue { Packet packet; DWORD since; };
FollowerCue followerCues[16]{};
unsigned followerCueCount = 0;
volatile LONG followerCuesSent = 0, followerCuesFolded = 0, followerCuesPlayed = 0;
struct NeutralWait { Packet packet; DWORD since; };
NeutralWait neutralWaits[16]{};
unsigned neutralWaitCount = 0;
volatile LONG neutralSent = 0, neutralWaited = 0, neutralPlayed = 0, neutralExpired = 0, spawnRequestsUnshared = 0;
// One spawn per request on each machine. Both machines usually fire the same
// request themselves AND play the partner's copy of it: 25.09., ch01, the
// client's own request for Edgar (75DE451E) and the host's replay of it both
// ran and two Edgars stood in the room. Whichever comes first spawns; the
// other is dropped. A request of the same name again after kNeutralOnceMs is a
// new one; a restart or a level exit forgets them all.
constexpr DWORD kNeutralOnceMs = 120000;
struct NeutralDone { uint32_t hash; DWORD at; };
NeutralDone neutralDone[32]{};
unsigned neutralDoneHead = 0;
volatile LONG neutralOwnDropped = 0, neutralPartnerDropped = 0;
// After kNeutralOnceMs only a copy from another origin than the first is a duplicate (StoryCuesOnce).
bool NeutralDoneHere(uint32_t hash, DWORD now, unsigned char origin) {
    for (const auto& d : neutralDone) if (d.hash == hash && d.at && now - d.at < kNeutralOnceMs) return true;
    return storyOnce && neutralBook.Taken(hash, origin) >= 0;
}
void NoteNeutralDone(uint32_t hash, DWORD now, unsigned char origin) {
    if (storyOnce) neutralBook.Note(hash, origin);
    for (auto& d : neutralDone) if (d.hash == hash) { d.at = now; return; }
    neutralDone[neutralDoneHead] = NeutralDone{hash, now};
    neutralDoneHead = (neutralDoneHead + 1) % std::size(neutralDone);
}
constexpr uint32_t kGetSpawnInfoRva = 0x376440;
constexpr DWORD kPropWaitMs = 8000, kPropSettleMs = 500;
constexpr unsigned kMaxProps = 128;
uint32_t propRequests[kMaxProps]{};
unsigned propRequestCount = 0;
struct PropClass { uint32_t attributes; int prop; };   // 2 neutral or local NPC, 1 prop, 0 not, -1 unknown
PropClass propClasses[256]{};
unsigned propClassCount = 0;
struct PropWait { Packet packet; DWORD since; };
PropWait propWaits[16]{};
unsigned propWaitCount = 0;
volatile LONG propSent = 0, propWaited = 0, propPlayed = 0, propExpired = 0, propClassified = 0, propUnknown = 0;
int ClassifyTemplate(const uint32_t* guid) {
    uint32_t sim = 0;
    uint32_t info[16]{};
    if (!Read(base + kSimManagerRva, &sim, 4) || !sim) return -1;
    __try {
        using Get = void (__thiscall*)(void*, uint32_t*, const uint32_t*, int);
        reinterpret_cast<Get>(base + kGetSpawnInfoRva)(reinterpret_cast<void*>(sim), info, guid, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    // The spawn filter reads the packet at info+0x10 and its descriptor at [packet].
    uint32_t descriptor = 0;
    if (!info[4] || !Read(info[4], &descriptor, 4) || !descriptor) return -1;
    if (descriptor == base + kNeutralDescriptorRva) return 2;
    // A local class (NicoleBoss) is spawned by every machine itself, like a
    // neutral: its request is shared and played once. 25.09.: the host's chain
    // spawned the dementia Nicole, the client's relayed copy of the same
    // manager dropped the FuncSpawn and the client fought an empty room.
    if (npcs::LocalByDescriptor(descriptor)) return 2;
    const auto type = npc_policy::Classify(descriptor);
    return type == npc_policy::Classification::NPC ? 0 : type == npc_policy::Classification::Other ? 1 : -1;
}
int PropClassOf(uint32_t attributes) {
    for (unsigned i = 0; i < propClassCount; ++i) if (propClasses[i].attributes == attributes) return propClasses[i].prop;
    uint32_t guid[4]{}, name = 0, max = 0;
    int prop = -1;
    if (Read(attributes + 0x0C, guid, 16) && Read(attributes, &name, 4) && Read(attributes + 0x1C, &max, 4)) {
        // A neutral NPC counts whatever its maximum; a prop only with max 1.
        prop = ClassifyTemplate(guid);
        if (prop == 1 && max != 1) prop = 0;
    }
    if (propClassCount < std::size(propClasses)) propClasses[propClassCount++] = PropClass{attributes, prop};
    InterlockedIncrement(prop < 0 ? &propUnknown : &propClassified);
    if (prop == 1 && trace)
        gate::Event("coop_mirror scene prop spawner request=%08X template=%08X-%08X max=%u", name, guid[0], guid[1], max);
    return prop;
}
bool IsPropRequest(uint32_t hash) {
    for (unsigned i = 0; i < propRequestCount; ++i) if (propRequests[i] == hash) return true;
    return false;
}
// The outcome of a player's own action (choice A). 24.09.: the eye poke on
// one machine left the other in the world before it: msgEyepokeEnd reaches a
// MultiManager (the Hunter scene, the objective, the post-eyepoke checkpoint,
// the helmet), and MultiManagers are not mirrored. The outcome names are read
// off the machines themselves (attributes at +0x0C): EyePokeMachine
// m_gameEndedMsgSend (+0x108), HackingStationMiniGame CMD_SetOnSuccessMessage
// (+0x3B4). Whoever's machine sends one shares it; each partner replays it and
// runs the same chain without doing the action himself.
bool actionOutcomes = false;   // [Mirror] ActionOutcomes
struct OutcomeSource { uint32_t vtableRva, offset; };
// PowerNodeSwitch dispatches this message after it deducts the opening cost.
constexpr OutcomeSource kPowerNodeOutcome{0x175DC78, 0x2F4};
constexpr OutcomeSource kOutcomeSources[] = {{0x1777468, 0x108}, {0x1745140, 0x3B4}, kPowerNodeOutcome};
constexpr unsigned kMaxOutcomes = 64;
uint32_t outcomeNames[kMaxOutcomes]{};
unsigned outcomeCount = 0;
// Outcomes a partner already played here: when the local Isaac does the same action
// later, its result is dropped instead of running the chain a second time.
uint32_t outcomesPlayed[kMaxOutcomes]{};
unsigned outcomesPlayedCount = 0;
volatile LONG outcomesSent = 0, outcomesReplayed = 0, outcomesKept = 0;
bool IsActionOutcome(uint32_t hash) {
    for (unsigned i = 0; i < outcomeCount; ++i) if (outcomeNames[i] == hash) return true;
    return false;
}
bool OutcomePlayedHere(uint32_t hash) {
    for (unsigned i = 0; i < outcomesPlayedCount; ++i) if (outcomesPlayed[i] == hash) return true;
    return false;
}
// Messages the level expects from its enemies. 24.09., psec03 balcony: the
// two Leapers' death messages (psec03_balcony_leaper_dead_f1,
// ..._02_dead_f1) go to an AndEventGate that opens the door. The enemies are
// the host's; a client only has ghosts, which never send them, and the gate is
// no mirrored class, so the client's door stayed shut. Every message an
// authored enemy sends for the level (death, health change, end of spawn,
// noticing the player; generated_death_messages.h, 643 enemies) is repeated
// by the host whatever listens to it.
bool enemyMessages = false;   // [Mirror] EnemyMessages
std::vector<uint32_t> enemyMessageHashes;
volatile LONG enemyMessagesSent = 0;
void BuildEnemyMessages() {
    enemyMessageHashes.clear();
    for (const auto& row : ds2::kDeathMessages)
        for (const auto* m : {&row.death, &row.healthModified, &row.spawnDone, &row.aware})
            if (m->hash) enemyMessageHashes.push_back(m->hash);
    std::sort(enemyMessageHashes.begin(), enemyMessageHashes.end());
    enemyMessageHashes.erase(std::unique(enemyMessageHashes.begin(), enemyMessageHashes.end()), enemyMessageHashes.end());
}
bool IsEnemyMessage(uint32_t hash) { return std::binary_search(enemyMessageHashes.begin(), enemyMessageHashes.end(), hash); }
// MultiManagers, relayed with their dangerous outputs held back. They were
// excluded since 2026-09-17 14:55: the host's mm_room_lower_enter and
// remove_area01_extras, replayed on the client, switched ITS stream sets to
// the host's room and removed its objects. But most story consequences run
// through them (objective, light, eye poke chain, scene props), and each one
// was patched on its own. Now a MultiManager a partner's message triggered
// sends only the targets that are neither dangerous nor mirrored anyway:
// MultiManager::SendMessages (PC 0x00801A50, thiscall, plain ret) walks the
// target mask at +0x58 (16 bits, attributes +0x14 + i * 0x10: message_t,
// delay float at +0x0C), queues each with its delay, and deletes the manager
// when its response count (+0x60) runs out. For an armed manager the mask
// loses the bits of the dropped targets for that one call. A kept target that
// is itself a MultiManager's trigger arms that one too, for its delay plus
// three seconds, so a chain is filtered all the way down.
bool relayMultiManagers = false;   // [Mirror] MultiManagers
constexpr uint32_t kMultiManagerVtableRva = 0x1721178, kSendMessagesRva = 0x401A50;
constexpr uint32_t kMmMask = 0x58, kMmResponses = 0x60, kMmTargets = 0x14, kMmTargetStride = 0x10;
gate::RuleSet mmDropRules;   // outputs never replayed through a relayed MultiManager
safetyhook::InlineHook sendMessagesHook;
// cullOnly: a local manager a door's shut output starts while our Isaac stands
// on the other side (worldcalls::DoorCloseCull): only its cull sets are dropped.
struct MmArm { uint32_t manager; DWORD until; bool fromHost; bool cullOnly; };
MmArm mmArms[64]{};
volatile LONG mmArmsEvicted = 0, mmArmsLate = 0, mmAllDropped = 0;
bool replayFromHost = true;   // author of the replay being dispatched right now
volatile LONG mmTriggersRelayed = 0, mmArmed = 0, mmFiltered = 0, mmTargetsKept = 0, mmTargetsDropped = 0, mmLines = 0, mmUnsafeTriggers = 0;
volatile LONG mmCullArms = 0, mmCullsDropped = 0, mmCullLines = 0;
void ArmManager(uint32_t manager, DWORD until, bool fromHost, bool cullOnly = false) {
    // Same manager first, then a free slot, then the arm that runs out soonest.
    unsigned slot = std::size(mmArms);
    for (unsigned i = 0; i < std::size(mmArms); ++i)
        if (mmArms[i].manager == manager) { slot = i; break; }
    for (unsigned i = 0; slot == std::size(mmArms) && i < std::size(mmArms); ++i)
        if (!mmArms[i].manager) slot = i;
    if (slot == std::size(mmArms)) {
        slot = 0;
        for (unsigned i = 1; i < std::size(mmArms); ++i)
            if (static_cast<LONG>(mmArms[i].until - mmArms[slot].until) < 0) slot = i;
        InterlockedIncrement(&mmArmsEvicted);
    }
    // A partner's relay arm drops the cull sets anyway: it stays.
    if (cullOnly && mmArms[slot].manager == manager && !mmArms[slot].cullOnly) {
        if (static_cast<LONG>(until - mmArms[slot].until) > 0) mmArms[slot].until = until;
        return;
    }
    mmArms[slot] = MmArm{manager, until, fromHost, cullOnly};
    InterlockedIncrement(cullOnly ? &mmCullArms : &mmArmed);
}
unsigned ArmManagersOf(uint32_t hash, DWORD until, bool fromHost, bool cullOnly = false) {
    uint32_t objects[16]{}, vtables[16]{};
    const unsigned n = gate::Listeners(hash, objects, vtables, 16);
    unsigned armed = 0;
    for (unsigned i = 0; i < n; ++i)
        if (vtables[i] == base + kMultiManagerVtableRva) { ArmManager(objects[i], until, fromHost, cullOnly); ++armed; }
    return armed;
}
// A message only StreamScripters hear: a cull set showing or hiding rooms.
bool OnlyStreamScripters(uint32_t hash) {
    uint32_t objects[9]{}, vtables[9]{};
    const unsigned n = gate::Listeners(hash, objects, vtables, 9);
    if (!n || n > 8) return false;
    for (unsigned i = 0; i < n; ++i) if (vtables[i] != base + gate::kStreamScripterVtableRva) return false;
    return true;
}
// A late arm still filters: a manager that a partner's message triggered and
// that sends only now must not fire its dangerous targets here.
constexpr DWORD kArmGraceMs = 30000;
bool TakeArm(uint32_t manager, DWORD now, bool& fromHost, bool* cullOnly = nullptr) {
    for (auto& a : mmArms)
        if (a.manager == manager) {
            const LONG left = static_cast<LONG>(a.until - now);
            fromHost = a.fromHost;
            if (cullOnly) *cullOnly = a.cullOnly;
            a.manager = 0; a.until = now;
            if (left < 0) InterlockedIncrement(&mmArmsLate);
            return left >= -static_cast<LONG>(kArmGraceMs);
        }
    return false;
}
// Why a target of a relayed MultiManager is not replayed here, or nullptr.
// Camera and fade targets belong to the player a chain is about. In a scene
// this machine follows (or one whose cue just arrived) that player is the local one too:
// the partner's scene camera has to start here as well.
bool SceneCameraClass(const char* cls) {
    return _stricmp(cls, "PushCamera") == 0 || _stricmp(cls, "PopCamera") == 0 || _stricmp(cls, "FadeScreenFx") == 0;
}
volatile LONG mmSceneCameras = 0;
// Messages that stay on the machine that fires them ([Mirror] LocalNamePatterns,
// substrings of the name). 25.09., the Nicole fight: every machine fights its
// own Nicole, and each player's dementia phases (the intro, the teleport, the
// paired death when he loses one) crossed over to the partner, whose Isaac
// then played a death grab without its camera. Nothing whose name matches is
// sent, replayed, or fired by a relayed manager.
// Only for the game's finale ([Mirror] LocalNameScope): the rule wakes up with
// the first message whose name marks the final area (dementialand, entered
// from ch11's dementia checkpoint), here or from a partner, holds through the
// finale's checkpoint restarts and ends when the level goes. Nicole scenes
// earlier in the game stay shared as before.
constexpr unsigned kLocalPatterns = 8;
char localPatterns[kLocalPatterns][24]{};
unsigned localPatternCount = 0;
char scopePatterns[kLocalPatterns][24]{};
unsigned scopePatternCount = 0;
bool finaleActive = false;
volatile LONG localNamesKept = 0;
// The MP round music (aud_play_stream_mus_01..08, aud_stop_current_music) is
// played by every machine for itself (runtime_coop_survmusic.inc).
bool MusicHash(uint32_t hash) { return coop_survmusic::IsMusicHash(hash); }
// Titan Mines' proximity switches (mp_proximityswitch_obj*_player_enter/exit): a
// machine's zone never counts its own player, only the other's ghost, so the
// necro's machine announced the human leaving and its relayed reset turned the
// human's objective switch off for good (Versus, 13.10.; the enable that follows
// is dropped as dangerous, a FuncSpawn listens to it). Each machine keeps its own.
bool ProximityZoneHash(uint32_t hash) {
    static const uint32_t kZones[] = {0xA7D9BA97u, 0xC0BE04FFu, 0x1A59DDD6u, 0x8D462200u, 0x8CDA0115u, 0x59CE3F01u};
    for (uint32_t z : kZones) if (z == hash) return true;
    return false;
}
// A kiosk's restore script (*_mm_save_kiosk, a checkpoint set's completion message) runs in
// single player once, when a save made at that kiosk loads. In co-op a partner's stream follow
// completed the set again mid-game and the mirror sent the script on: it locked the vestry lift
// door on both machines (16.10.). [Mirror] AlwaysLocalNames, always on, not only in the finale.
char alwaysLocal[4][32]{};
unsigned alwaysLocalCount = 0;
bool AlwaysLocal(const char* n) {
    char lower[128];
    unsigned i = 0;
    for (; n[i] && i + 1 < sizeof(lower); ++i) lower[i] = static_cast<char>(n[i] >= 'A' && n[i] <= 'Z' ? n[i] + 32 : n[i]);
    lower[i] = 0;
    for (unsigned k = 0; k < alwaysLocalCount; ++k)
        if (std::strstr(lower, alwaysLocal[k])) return true;
    return false;
}
bool LocalName(uint32_t hash) {
    if (hash == gate::kDainaSpawnPose) return true;
    if (MusicHash(hash)) return true;
    if (alwaysLocalCount) {
        const char* name = coop_names::Name(hash);
        if (name && name[0] && AlwaysLocal(name)) return true;
    }
    if (mpmap::InVisit() && ProximityZoneHash(hash)) return true;
    if (zoneobj::Local(hash)) return true;   // Hold the Zone opening the map, which every machine does itself
    if (!localPatternCount) return false;
    const char* n = coop_names::Name(hash);
    if (!n || !n[0]) return false;
    if (!finaleActive) {
        for (unsigned i = 0; i < scopePatternCount && !finaleActive; ++i)
            if (std::strstr(n, scopePatterns[i])) {
                finaleActive = true;
                gate::Event("coop_mirror the finale begins (%08X %s): %u name patterns stay local from here", hash, n, localPatternCount);
            }
        if (!finaleActive && scopePatternCount) return false;
    }
    for (unsigned i = 0; i < localPatternCount; ++i)
        if (std::strstr(n, localPatterns[i])) return true;
    return false;
}
void NoteLocalName(uint32_t hash, const char* where) {
    if (InterlockedIncrement(&localNamesKept) <= 60)
        gate::Event("coop_mirror %08X (%s) stays local (%s)", hash, coop_names::Name(hash), where);
}
const char* DropReason(uint32_t hash, bool fromHost, char* cls, size_t clsSize) {
    unsigned listeners = 0;
    if (LocalName(hash)) { std::snprintf(cls, clsSize, "%s", "local name"); return "stays local"; }
    if (gate::AnyListener(hash, mmDropRules, cls, clsSize, listeners)) {
        const DWORD cue = static_cast<DWORD>(InterlockedCompareExchange(&gate::lastSequenceTick, 0, 0));
        const bool inScene = cutscene::Following() || (cue && GetTickCount() - cue < 3000);
        if (!(inScene && SceneCameraClass(cls))) return "dangerous";
        InterlockedIncrement(&mmSceneCameras);   // kept: a followed scene's camera
    }
    // Dialogue stays with the relayed manager. A manager's lines are timed
    // targets (60 s, 62 s after the trigger) that the scheduler delivers
    // outside the dispatch hook, so the leader never mirrors them: 25.09., the
    // Tormentor scene, the host dropped the two PlayAudioStreams as "mirrored
    // anyway", the client's log had not one voice line mirrored, and the host
    // played the scene without them. Played here at the relayed manager's own
    // time instead; a leader's copy arriving as well is folded by SeenRecently.
    if (gate::AnyListener(hash, voiceRules, cls, clsSize, listeners)) return nullptr;
    if (gate::AnyListener(hash, worldRules, cls, clsSize, listeners) || gate::AnyListener(hash, anyRules, cls, clsSize, listeners))
        return "mirrored anyway";
    // Objectives and logs are repeated by the host alone: from a client's chain nobody else sends them.
    if (fromHost && gate::AnyListener(hash, hostRules, cls, clsSize, listeners)) return "mirrored by the host";
    return nullptr;
}
// Enemy spawns asked for in a client's world ([Mirror] EnemySpawnsFromClients).
// 25.09., ch11 marker test bed: the client walked into the Hunter trap; his
// FuncSpawn was suppressed by HostNPCs (the host owns the enemies) and the
// host's relayed copy of the trap's MultiManager dropped the FuncSpawn as
// dangerous, so the Ubermorph spawned nowhere. The same happens to any
// FuncSpawn a client's own trigger or chain reaches before the host's Isaac
// does. So every message a client's world sends to FuncSpawns that all spawn
// hostile NPCs goes to the host as a request, with the GUID of what sent it
// (a trigger, a MultiManager), and the host plays it. When the host's own
// world later sends the same message from the same sender, that activation
// already happened for the client and is dropped once.
bool enemySpawnsFromClients = false;
constexpr uint32_t kEnemySpawnMark = 0x4E505345;   // 'ESPN' in reserved[5], sender GUID in reserved[0..3]
struct EnemyRequest { uint32_t hash, guid[4]; };
EnemyRequest enemyRequests[64]{};
unsigned enemyRequestNext = 0;
volatile LONG enemyRequestsSent = 0, enemyRequestsPlayed = 0, enemyRequestsFolded = 0;
bool HostileSpawnOnlyUncached(uint32_t hash) {
    uint32_t objects[8]{}, vtables[8]{};
    const unsigned n = gate::Listeners(hash, objects, vtables, 8);
    if (!n) return false;
    for (unsigned i = 0; i < n; ++i) {
        uint32_t attributes = 0, guid[4]{};
        if (vtables[i] != base + kFuncSpawnVtableRva || !Read(objects[i] + kFuncSpawnAttributes, &attributes, 4) || !attributes ||
            !Read(attributes + 0x0C, guid, 16) || ClassifyTemplate(guid) != 0) return false;
    }
    return true;
}
// Every dispatched message asks, so the answer is kept per hash for ten seconds.
bool HostileSpawnOnly(uint32_t hash) {
    static struct { uint32_t hash; DWORD at; bool yes; } cache[512]{};
    auto& c = cache[(hash ^ (hash >> 9)) % 512];
    const DWORD now = GetTickCount();
    if (c.hash == hash && c.at && now - c.at < 10000) return c.yes;
    c.hash = hash; c.at = now; c.yes = HostileSpawnOnlyUncached(hash);
    return c.yes;
}
// The GUID of the object that sent the message (handle at message+8). A
// level GUID ends in the author's eight letters and digits (44455257-38343145);
// 12.10. the client sent 533C35AD-00000000 and 00000032-00000003 from objects
// without one, and no request ever folded.
bool AuthoredGuid(const uint32_t* guid) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(guid + 2);
    for (unsigned i = 0; i < 8; ++i)
        if (!((bytes[i] >= '0' && bytes[i] <= '9') || (bytes[i] >= 'A' && bytes[i] <= 'Z') || (bytes[i] >= 'a' && bytes[i] <= 'z') || bytes[i] == '_')) return false;
    return true;
}
bool SenderGuid(const void* message, uint32_t* guid) {
    uint32_t handle = 0, table = 0, object = 0, id = 0;
    if (!Read(reinterpret_cast<uintptr_t>(message) + 8, &handle, 4) || !handle || !Read(base + gate::kHandleTableRva, &table, 4) || !table ||
        !Read(table + (handle >> 23) * 8, &object, 4) || !Read(table + (handle >> 23) * 8 + 4, &id, 4) || id != handle || !object) return false;
    if (!Read(object + 0x24, guid, 16) || !AuthoredGuid(guid)) { std::memset(guid, 0, 16); return false; }
    return true;
}
void SendEnemyRequest(uint32_t hash, const void* message, int flags) {
    uint32_t guid[4]{};
    SenderGuid(message, guid);
    SendLocal(hash, message, flags);
    {
        std::lock_guard<std::mutex> guard(lock);
        Out& o = outbox[(outHead + kOutbox - 1) % kOutbox];
        std::memcpy(o.packet.reserved, guid, 16);
        o.packet.reserved[5] = kEnemySpawnMark;
    }
    InterlockedIncrement(&enemyRequestsSent);
    gate::Event("coop_mirror enemy spawn %08X (%s) from %08X-%08X in our world: asked of the host", hash, coop_names::Name(hash), guid[0], guid[1]);
}
void NoteEnemyRequest(uint32_t hash, const uint32_t* guid) {
    EnemyRequest& r = enemyRequests[enemyRequestNext++ % std::size(enemyRequests)];
    r.hash = hash;
    std::memcpy(r.guid, guid, 16);
}
// Host: the local world sends a spawn a client's request already played.
// Without a sender GUID on either side the message itself decides.
bool TakeEnemyRequest(uint32_t hash, const void* message) {
    uint32_t guid[4]{};
    const bool haveGuid = SenderGuid(message, guid);
    for (auto& r : enemyRequests) {
        if (r.hash != hash) continue;
        const bool requestGuid = (r.guid[0] | r.guid[1] | r.guid[2] | r.guid[3]) != 0;
        if (haveGuid && requestGuid && std::memcmp(r.guid, guid, 16) != 0) continue;
        r = EnemyRequest{};
        return true;
    }
    return false;
}

// A MultiManager that drives a lift (a target heard by a Mover or a
// MoverController, also one level down) waits while a cabin holds a player
// and another is outside it. 25.09.: the cabin button's manager closed the
// doors and sent the ride 3 s later; the partner behind the lower door was
// left on the floor below on both machines. Held whole, before its first
// target, on every machine that runs it, so the doors stay open too.
struct MmLiftHold { uint32_t manager, vtable; bool armed, fromHost, noticed; DWORD since; bool cullOnly, releasing; uint32_t guid[4]; };
MmLiftHold mmLiftHolds[4]{};
volatile LONG mmLiftHeld = 0, mmLiftReleased = 0, mmLiftTimedOut = 0, mmLiftDropped = 0;
bool HeardByLift(uint32_t hash) {
    uint32_t objects[8]{}, vtables[8]{};
    const unsigned n = gate::Listeners(hash, objects, vtables, 8);
    for (unsigned i = 0; i < n; ++i)
        if (gate::IsLiftController(objects[i], vtables[i])) return true;
    return false;
}
// The cabin managers found offline (tools/coop_ride_triggers.py), also those that reach
// their ride through a VariableSwitch or an event gate, which the walk below cannot follow.
bool LiftManager(uint32_t manager) {
    uint32_t guid[4]{};
    if (!Read(manager + 0x24, guid, 16)) return false;
    for (const auto& known : coop_rides::kLiftManagers)
        if (std::memcmp(known, guid, 16) == 0) return true;
    return false;
}
bool MovesLift(uint32_t manager, unsigned depth) {
    if (LiftManager(manager)) return true;
    uint16_t mask = 0;
    uint32_t attributes = 0;
    if (!Read(manager + kMmMask, &mask, 2) || !Read(manager + 0x0C, &attributes, 4) || !attributes) return false;
    for (unsigned i = 0; i < 16; ++i) {
        uint32_t hash = 0;
        if (!(mask & (1u << i)) || !Read(attributes + kMmTargets + i * kMmTargetStride, &hash, 4) || !hash) continue;
        if (HeardByLift(hash)) return true;
        if (depth) {
            uint32_t objects[8]{}, vtables[8]{};
            const unsigned n = gate::Listeners(hash, objects, vtables, 8);
            for (unsigned k = 0; k < n; ++k)
                if (vtables[k] == base + kMultiManagerVtableRva && objects[k] != manager && MovesLift(objects[k], depth - 1)) return true;
        }
    }
    return false;
}
void __fastcall OnSendMessages(void* self);
volatile LONG mmLiftFaults = 0;
__declspec(noinline) void ReleaseLiftManager(uint32_t manager) {
    __try {
        OnSendMessages(reinterpret_cast<void*>(static_cast<uintptr_t>(manager)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&mmLiftFaults);
    }
}
void MmLiftTick(DWORD now) {
    for (auto& h : mmLiftHolds) {
        if (!h.manager) continue;
        uint32_t vtable = 0;
        if (!inLevel || !Read(h.manager, &vtable, 4) || vtable != h.vtable || ObjectByGuid(h.guid) != h.manager) { h = MmLiftHold{}; InterlockedIncrement(&mmLiftDropped); continue; }
        // No body here (a restart or a hand-over under way): PartnersInLift would say yes
        // to an empty slot 0 and the manager the restart deletes would run (audit 16.10.).
        if (!LocalSlotZeroEntity()) continue;
        char who[64] = "";
        const bool late = ghosttrigger::liftWaitMs && now - h.since >= ghosttrigger::liftWaitMs;
        if (!late && !ghosttrigger::PartnersInLift(now, who, sizeof(who))) {
            if (!h.noticed && now - h.since >= 1000) { h.noticed = true; notice::Show("WAITING FOR PARTNER - LIFT"); }
            continue;
        }
        const MmLiftHold held = h;
        InterlockedIncrement(late ? &mmLiftTimedOut : &mmLiftReleased);
        gate::Event("coop_lift manager %08X released after %u ms: %s", held.manager, static_cast<unsigned>(now - held.since),
                    late ? "TIMEOUT, going without the partner" : "everybody is in the cabin");
        if (held.noticed || late) notice::Show(late ? "CONTINUING WITHOUT PARTNER" : "PARTNER HERE - CONTINUING");
        // A partner's relayed copy keeps its filter: armed again for this one call.
        if (held.armed) ArmManager(held.manager, now + 3000, held.fromHost, held.cullOnly);
        // The slot is cleared only after the call: HoldForLift recognises the
        // release by it, or a timed-out ride would be held again at once.
        h.releasing = true;
        ReleaseLiftManager(held.manager);
        h = MmLiftHold{};
    }
}
bool HoldForLift(uint32_t manager, DWORD now, bool& fromHost) {
    if (!ghosttrigger::liftWait || (!peerTable.Count() && !AnyBot()) || !inLevel) return false;
    for (const auto& h : mmLiftHolds) if (h.manager == manager) return !h.releasing;   // repeated presses stay held
    if (!MovesLift(manager, 1)) return false;
    char who[64] = "";
    if (ghosttrigger::PartnersInLift(now, who, sizeof(who))) {
        static volatile LONG rides = 0;
        if (InterlockedIncrement(&rides) <= 50) {
            uint32_t guid[4]{};
            Read(manager + 0x24, guid, 16);
            float me[3]{};
            gate::LocalPosition(me);
            gate::Event("coop_lift manager %08X-%08X moves a lift, not held: %s (our Isaac at %.1f,%.1f,%.1f)", guid[0], guid[1], who, me[0], me[1], me[2]);
        }
        return false;
    }
    MmLiftHold* slot = nullptr;
    for (auto& h : mmLiftHolds) if (!h.manager) { slot = &h; break; }
    if (!slot) return false;
    uint32_t vtable = 0;
    Read(manager, &vtable, 4);
    bool cullOnly = false;
    const bool armed = TakeArm(manager, now, fromHost, &cullOnly);
    *slot = MmLiftHold{manager, vtable, armed, fromHost, false, now, cullOnly, false, {}};
    Read(manager + 0x24, slot->guid, sizeof(slot->guid));
    InterlockedIncrement(&mmLiftHeld);
    uint32_t guid[4]{};
    Read(manager + 0x24, guid, 16);
    gate::Event("coop_lift manager %08X-%08X (%s) held before its doors and ride: %s", guid[0], guid[1], armed ? "a partner's" : "ours", who);
    return true;
}
// A relayed manager's target that a dropped class (a spawner, a stream set) hears goes
// nowhere here, and every other listener of it went with it ([Mirror] SplitDroppedTargets).
// 14.10., ch02 tram station: the partner's mm_spawn_mov_tram_car_fire arrived, its target
// spawn_mov_tram_car_fire (four FuncSpawns of the burning tram, and the MultiManager that
// ends the Nicole hallucination) was dropped, while the trigger message itself had already
// switched off our own end triggers: the hallucination stayed for good on both machines.
// Such a target now goes straight to its MultiManager listeners; their own targets are
// filtered as a relay's again, so no spawner or stream set is reached through them.
bool splitDropped = true;
struct Split { uint32_t hash; DWORD due; bool fromHost; };
Split splits[16]{};
unsigned splitCount = 0;
volatile LONG splitDelivered = 0, splitManagers = 0, splitOther = 0, splitQueued = 0, splitFull = 0, splitFaults = 0, splitNone = 0;
unsigned DeliverToManagers(uint32_t hash, bool fromHost, DWORD now) {
    uint32_t objects[16], vtables[16];
    const unsigned n = gate::Listeners(hash, objects, vtables, 16);
    unsigned char raw[kMessageCopy]{};
    std::memcpy(raw, &hash, 4);   // raw[4], the in-flight marker, stays 0; no source
    unsigned delivered = 0;
    ArmManagersOf(hash, now + 3000, fromHost);
    for (unsigned i = 0; i < n; ++i) {
        if (vtables[i] != base + kMultiManagerVtableRva) { InterlockedIncrement(&splitOther); continue; }
        ++gate::mirroring;
        __try {
            ghosttrigger::sendHook.ccall<int>(raw, reinterpret_cast<void*>(objects[i]), 0);
            ++delivered;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&splitFaults);
        }
        --gate::mirroring;
    }
    InterlockedExchangeAdd(&splitManagers, static_cast<LONG>(delivered));
    InterlockedIncrement(delivered ? &splitDelivered : &splitNone);
    return delivered;
}
void SplitTarget(uint32_t hash, float delay, bool fromHost, DWORD now) {
    const DWORD wait = delay > 0.0f && delay < 600.0f ? static_cast<DWORD>(delay * 1000.0f) : 0;
    if (!wait) {
        const unsigned delivered = DeliverToManagers(hash, fromHost, now);
        if (delivered && InterlockedIncrement(&mmLines) <= 400)
            gate::Event("coop_mirror dropped target %08X (%s) delivered to %u MultiManager listener(s) only", hash, coop_names::Name(hash), delivered);
        return;
    }
    if (splitCount >= std::size(splits)) { InterlockedIncrement(&splitFull); return; }
    splits[splitCount++] = Split{hash, now + wait, fromHost};
    InterlockedIncrement(&splitQueued);
}
void SplitTick(DWORD now) {
    for (unsigned i = 0; i < splitCount;) {
        if (static_cast<int32_t>(now - splits[i].due) < 0) { ++i; continue; }
        const Split s = splits[i];
        splits[i] = splits[--splitCount];
        if (!inLevel) continue;
        const unsigned delivered = DeliverToManagers(s.hash, s.fromHost, now);
        if (InterlockedIncrement(&mmLines) <= 400)
            gate::Event("coop_mirror delayed dropped target %08X (%s) delivered to %u MultiManager listener(s) only", s.hash, coop_names::Name(s.hash), delivered);
    }
}
void __fastcall OnSendMessages(void* self) {
    const auto manager = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    const DWORD now = GetTickCount();
    bool fromHost = replayFromHost;
    if (HoldForLift(manager, now, fromHost)) return;
    bool cullOnly = false;
    const bool armed = TakeArm(manager, now, fromHost, &cullOnly);   // always taken, so an arm never outlives the activation it was for
    if (!armed && gate::mirroring == 0) { sendMessagesHook.thiscall<void>(self); return; }
    cullOnly = armed && cullOnly && gate::mirroring == 0;   // a replay running now filters as a relay
    uint16_t mask = 0;
    uint32_t attributes = 0;
    int32_t responses = 0;
    if (!Read(manager + kMmMask, &mask, 2) || !Read(manager + 0x0C, &attributes, 4) || !attributes || !Read(manager + kMmResponses, &responses, 4)) {
        sendMessagesHook.thiscall<void>(self);
        return;
    }
    InterlockedIncrement(&mmFiltered);
    uint32_t guid[4]{};
    Read(manager + 0x24, guid, 16);
    uint16_t keep = mask;
    for (unsigned i = 0; i < 16; ++i) {
        if (!(mask & (1u << i))) continue;
        uint32_t hash = 0;
        float delay = 0.0f;
        const uint32_t target = attributes + kMmTargets + i * kMmTargetStride;
        if (!Read(target, &hash, 4) || !hash) continue;
        Read(target + 0x0C, &delay, 4);
        char cls[32] = "";
        if (cullOnly) {
            const DWORD wait = delay > 0.0f && delay < 600.0f ? static_cast<DWORD>(delay * 1000.0f) : 0;
            if (OnlyStreamScripters(hash)) {
                keep = static_cast<uint16_t>(keep & ~(1u << i));
                InterlockedIncrement(&mmCullsDropped);
                if (InterlockedIncrement(&mmCullLines) <= 200)
                    gate::Event("coop_mirror multimanager %08X-%08X target %u %08X (%s) dropped: a cull set for the other side of the door than our Isaac",
                                guid[0], guid[1], i, hash, coop_names::Name(hash));
            } else {
                ArmManagersOf(hash, now + wait + 3000, fromHost, true);   // a nested cull manager is filtered the same way
            }
            continue;
        }
        const char* why = DropReason(hash, fromHost, cls, sizeof(cls));
        if (why) {
            keep = static_cast<uint16_t>(keep & ~(1u << i));
            InterlockedIncrement(&mmTargetsDropped);
            gate::VoteForDropped(hash);   // a dropped checkpoint still counts as reached
            if (splitDropped && std::strcmp(why, "dangerous") == 0) SplitTarget(hash, delay, fromHost, now);
        } else {
            InterlockedIncrement(&mmTargetsKept);
            const DWORD wait = delay > 0.0f && delay < 600.0f ? static_cast<DWORD>(delay * 1000.0f) : 0;
            ArmManagersOf(hash, now + wait + 3000, fromHost);
        }
        if (InterlockedIncrement(&mmLines) <= 400)
            gate::Event("coop_mirror multimanager %08X-%08X target %u %08X (%s) delay=%.1f %s%s%s%s", guid[0], guid[1], i, hash,
                        coop_names::Name(hash), delay, why ? "dropped: " : "kept", why ? why : "", why ? " " : "", why ? cls : "");
    }
    if (keep == mask) { sendMessagesHook.thiscall<void>(self); return; }
    // Nothing left to send here: not called at all. With an empty mask the
    // original returns before it counts the response, and a one-shot manager
    // would stay alive with no targets for the local Isaac's later use.
    if (!keep) { InterlockedIncrement(&mmAllDropped); return; }
    WriteBytes(manager + kMmMask, &keep, 2);
    sendMessagesHook.thiscall<void>(self);
    // A manager on its last response deletes itself in there; any other keeps its targets for its next activation.
    if (responses != 1) WriteBytes(manager + kMmMask, &mask, 2);
}
bool TriggerUnsafe(uint32_t hash) {
    char cls[32] = "";
    unsigned listeners = 0;
    return gate::AnyListener(hash, mmDropRules, cls, sizeof(cls), listeners);
}
bool IsSpawnDone(uint32_t hash) {
    for (unsigned i = 0; i < spawnDoneCount; ++i) if (spawnDone[i] == hash) return true;
    return false;
}
// Prompt results in a followed scene. 24.09.: the host led the insider fight
// and passed the prompt; the client, following with the streamed buttons,
// failed its own TestInputEvent (vtable 0x01B76F7C), whose failure message
// (cct_msg_bossInsider_struggle_fail_01) runs the kill by script, past the
// follower's damage block, and his death restarted both. So every
// TestInputEvent's success and failure names are read off the prompts
// (attributes at +0x0C, message hashes at +0x18 and +0x24; the PDB layout,
// the same message_t form as FuncSpawn's SpawnDone). A follower drops its own
// results, whoever leads sends his, and they are replayed only on a machine
// that follows a scene.
bool promptsShared = false;   // [Mirror] PromptResultsFromLeader
constexpr uint32_t kTestInputVtableRva = 0x1776F7C, kPromptSuccess = 0x18, kPromptFailure = 0x24;
constexpr unsigned kMaxPromptNames = 128;
uint32_t promptNames[kMaxPromptNames]{};
unsigned promptNameCount = 0;
volatile LONG promptDropped = 0, promptSent = 0, promptReplayed = 0, promptRefused = 0;
bool IsPromptResult(uint32_t hash) {
    for (unsigned i = 0; i < promptNameCount; ++i) if (promptNames[i] == hash) return true;
    return false;
}

// The SpawnDone, prop request, action outcome and prompt names all come from
// one walk over the SimManager's entity lists. It ran inside the dispatch hook,
// the whole level at once every 3 s per kind; now it runs from Tick with a
// budget per frame, and a finished pass replaces the published lists. The
// lists are at most a pass (plus kScanIntervalMs) old, as before.
constexpr unsigned kScanBudget = 1000;
constexpr DWORD kScanIntervalMs = 3000;
struct ScanWalk {
    bool active, listStarted, wantSpawn, wantPrompts;
    uint32_t sim, node, entity;
    unsigned lists, steps, spawnCount, propCount, outcomeCount, promptCount;
    uint32_t spawn[kMaxSpawnDone], props[kMaxProps], outcomes[kMaxOutcomes], prompts[kMaxPromptNames];
};
ScanWalk walk{};
DWORD walkDoneAt = 0;
volatile LONG promptScans = 0, scanRestarts = 0;
void ScanEntity(uint32_t entity) {
    uint32_t vtable = 0;
    if (!Read(entity, &vtable, 4)) return;
    if (walk.wantSpawn) {
        uint32_t attributes = 0, hash = 0;
        if (vtable == base + kFuncSpawnVtableRva && Read(entity + kFuncSpawnAttributes, &attributes, 4) && attributes) {
            if (Read(attributes + kSpawnDoneMessage, &hash, 4) && hash && walk.spawnCount < kMaxSpawnDone) {
                bool known = false;
                for (unsigned i = 0; i < walk.spawnCount; ++i) if (walk.spawn[i] == hash) { known = true; break; }
                if (!known) walk.spawn[walk.spawnCount++] = hash;
            }
            uint32_t request = 0;
            if (propSpawns && walk.propCount < kMaxProps && Read(attributes, &request, 4) && request && PropClassOf(attributes) == 1)
                walk.props[walk.propCount++] = request;
        }
        for (const auto& source : kOutcomeSources) {
            uint32_t outcomeAttributes = 0, outcome = 0;
            if (actionOutcomes && vtable == base + source.vtableRva && walk.outcomeCount < kMaxOutcomes &&
                Read(entity + kFuncSpawnAttributes, &outcomeAttributes, 4) && outcomeAttributes &&
                Read(outcomeAttributes + source.offset, &outcome, 4) && outcome)
                walk.outcomes[walk.outcomeCount++] = outcome;
        }
    }
    if (walk.wantPrompts && vtable == base + kTestInputVtableRva && walk.promptCount + 2 <= kMaxPromptNames) {
        uint32_t attributes = 0, success = 0, failure = 0;
        if (Read(entity + kFuncSpawnAttributes, &attributes, 4) && attributes &&
            Read(attributes + kPromptSuccess, &success, 4) && Read(attributes + kPromptFailure, &failure, 4)) {
            if (success) walk.prompts[walk.promptCount++] = success;
            if (failure) walk.prompts[walk.promptCount++] = failure;
        }
    }
}
void ScanTick(DWORD now) {
    const bool wantSpawn = (spawnerMessages && hostNPCs && IsHost()) || propSpawns || actionOutcomes;
    const bool wantPrompts = promptsShared;
    if (!wantSpawn && !wantPrompts) { walk.active = false; return; }
    uint32_t sim = 0;
    if (!Read(base + kSimManagerRva, &sim, 4) || !sim) {
        walk.active = false;
        spawnDoneCount = propRequestCount = 0;
        promptNameCount = 0;
        return;
    }
    if (walk.active && (walk.sim != sim || walk.wantSpawn != wantSpawn || walk.wantPrompts != wantPrompts)) {
        walk.active = false;   // another world or other wishes: start over
        InterlockedIncrement(&scanRestarts);
    }
    if (!walk.active) {
        if (walkDoneAt && now - walkDoneAt < kScanIntervalMs) return;
        uint32_t node = 0;
        if (!Read(sim + 0x60, &node, 4)) {
            spawnDoneCount = propRequestCount = 0;
            promptNameCount = 0;
            walkDoneAt = now;
            return;
        }
        walk.active = true; walk.listStarted = false;
        walk.wantSpawn = wantSpawn; walk.wantPrompts = wantPrompts;
        walk.sim = sim; walk.node = node; walk.entity = 0;
        walk.lists = walk.steps = walk.spawnCount = walk.propCount = walk.outcomeCount = walk.promptCount = 0;
    }
    unsigned budget = kScanBudget;
    while (walk.node && walk.lists < 512) {
        if (!walk.listStarted) {
            walk.entity = 0;
            Read(walk.node + 0xC, &walk.entity, 4);
            walk.steps = 0;
            walk.listStarted = true;
        }
        while (walk.entity && walk.steps < 100000) {
            if (!budget) return;   // the next frame goes on from here
            --budget;
            ++walk.steps;
            ScanEntity(walk.entity);
            uint32_t next = 0;
            walk.entity = Read(walk.entity + 0x1C, &next, 4) ? next : 0;
        }
        walk.listStarted = false;
        ++walk.lists;
        uint32_t next = 0;
        walk.node = Read(walk.node, &next, 4) ? next : 0;
    }
    if (walk.wantSpawn) {
        std::memcpy(spawnDone, walk.spawn, sizeof(uint32_t) * walk.spawnCount);
        spawnDoneCount = walk.spawnCount;
        std::memcpy(propRequests, walk.props, sizeof(uint32_t) * walk.propCount);
        propRequestCount = walk.propCount;
        std::memcpy(outcomeNames, walk.outcomes, sizeof(uint32_t) * walk.outcomeCount);
        outcomeCount = walk.outcomeCount;
        InterlockedIncrement(&spawnDoneScans);
    }
    if (walk.wantPrompts) {
        std::memcpy(promptNames, walk.prompts, sizeof(uint32_t) * walk.promptCount);
        promptNameCount = walk.promptCount;
        InterlockedIncrement(&promptScans);
    }
    walk.active = false;
    walkDoneAt = now;
}
// SpawnDone's payload is BasePtrMsgData (Xbox ctor 0x83F8ECC0): +8 a handle
// on the spawned entity. Its GUID goes with the message, and the receiver
// waits until it has that entity: ch11's light is an ExecuteVFX parented to
// the spawned Nicole (VFXTarget 52CB4090-F760AE1E), and replayed 94 ms after
// the host's spawn it found no Nicole on the client yet (24.09., no light).
constexpr DWORD kSpawnWaitMs = 5000, kSpawnSettleMs = 500;
uint32_t ResolveHandle(uint32_t handle) {
    uint32_t table = 0, object = 0, id = 0;
    return handle && Read(base + gate::kHandleTableRva, &table, 4) && table &&
           Read(table + (handle >> 23) * 8, &object, 4) && Read(table + (handle >> 23) * 8 + 4, &id, 4) && id == handle ? object : 0;
}
bool SpawnedGuid(const void* message, uint32_t* guid) {
    uint32_t dataHandle = 0, entityHandle = 0;
    if (!Read(reinterpret_cast<uintptr_t>(message) + 8, &dataHandle, 4)) return false;
    const uint32_t data = ResolveHandle(dataHandle);
    if (!data || !Read(data + 8, &entityHandle, 4)) return false;
    const uint32_t entity = ResolveHandle(entityHandle);
    return entity && Read(entity + 0x24, guid, 16) && (guid[0] | guid[1] | guid[2] | guid[3]);
}
struct SpawnWait { Packet packet; DWORD since, foundAt; };
constexpr unsigned kSpawnWaits = 16;
SpawnWait spawnWaits[kSpawnWaits]{};
unsigned spawnWaitCount = 0;
volatile LONG spawnGuidSent = 0, spawnWaited = 0, spawnWaitFound = 0, spawnWaitLate = 0;
bool FromFuncSpawn(const void* message) {
    uint32_t source = 0, table = 0, object = 0, id = 0, vtable = 0;
    return Read(reinterpret_cast<uintptr_t>(message) + 8, &source, 4) && source &&
           Read(base + gate::kHandleTableRva, &table, 4) && table &&
           Read(table + (source >> 23) * 8, &object, 4) && Read(table + (source >> 23) * 8 + 4, &id, 4) &&
           id == source && object && Read(object, &vtable, 4) && vtable == base + kFuncSpawnVtableRva;
}

constexpr DWORD kPersonalWindowMs = 2000;
volatile LONG personalKept = 0;
// Returns true when the message is withheld here (a sequence cue waiting for
// the partners' streams, or a local result a partner's copy replaces); the
// gate then does not dispatch it.
// The results of a player's action on a receptacle, puzzle, handle, switch or
// destructible ([Mirror] SourceShare, SourceClasses). 12.10., offline from the level
// data (tools/coop_message_graph.py): such a result mostly goes straight to logic
// nodes that are no mirrored class (74 of 98 kinesis receptacles feed OrEventGates
// and VariableSwitches), so the partner's world never heard the cell go in. It is
// shared by its sender, with the sender's GUID; the partner replays it, and when
// his own copy of that object later sends the same message, that one is dropped
// (the action already happened for the group). A message also heard by a stream,
// checkpoint, camera, teleport or Lua class (SourceUnsafeClasses/Nodes) stays local.
bool sourceShare = false;
gate::RuleSet sourceRules, sourceUnsafeRules;
constexpr uint32_t kSourceMark = 0x45435253;   // 'SRCE' in reserved[5], sender GUID in reserved[0..3]
struct SourceKey { uint32_t hash, guid[4]; };
SourceKey sourcesDone[128]{};
unsigned sourceDoneNext = 0;
struct SourceCache { uint32_t vtable; int index; };
SourceCache sourceCache[96]{};
unsigned sourceCacheCount = 0;
volatile LONG sourceSent = 0, sourceReplayed = 0, sourceDropped = 0, sourceUnsafe = 0, sourceLines = 0;
// The sender's class from the rule list (by IsA, cached per vtable) and its GUID.
bool SharedSource(const void* message, char* cls, size_t clsSize, uint32_t* guid) {
    uint32_t handle = 0, table = 0, object = 0, id = 0, vtable = 0;
    if (!sourceRules.entityCount || !Read(reinterpret_cast<uintptr_t>(message) + 8, &handle, 4) || !handle ||
        !Read(base + gate::kHandleTableRva, &table, 4) || !table || !Read(table + (handle >> 23) * 8, &object, 4) ||
        !Read(table + (handle >> 23) * 8 + 4, &id, 4) || id != handle || !object || !Read(object, &vtable, 4) ||
        vtable < base + gate::kRdataLo || vtable >= base + gate::kRdataHi)
        return false;
    int index = -2;
    for (unsigned i = 0; i < sourceCacheCount; ++i) if (sourceCache[i].vtable == vtable) { index = sourceCache[i].index; break; }
    if (index == -2) {
        index = -1;
        for (unsigned i = 0; i < sourceRules.entityCount && index < 0; ++i)
            if (gate::IsA(object, vtable, sourceRules.entities[i].hash)) index = static_cast<int>(i);
        if (sourceCacheCount < std::size(sourceCache)) sourceCache[sourceCacheCount++] = SourceCache{vtable, index};
    }
    if (index < 0) return false;
    std::snprintf(cls, clsSize, "%s", sourceRules.entities[index].name);
    return Read(object + 0x24, guid, 16) && (guid[0] | guid[1] | guid[2] | guid[3]);
}
bool SourceDoneHere(uint32_t hash, const uint32_t* guid) {
    for (const auto& k : sourcesDone)
        if (k.hash == hash && std::memcmp(k.guid, guid, 16) == 0) return true;
    return false;
}
void NoteSourceDone(uint32_t hash, const uint32_t* guid) {
    if (SourceDoneHere(hash, guid)) return;
    SourceKey& k = sourcesDone[sourceDoneNext++ % std::size(sourcesDone)];
    k.hash = hash;
    std::memcpy(k.guid, guid, 16);
}

volatile LONG watcherSendsDropped = 0;
// The object a message was sent from (message+8 is its handle), 0 when unknown.
uint32_t SenderObject(const void* message) {
    uint32_t handle = 0, table = 0, object = 0, id = 0;
    if (!Read(reinterpret_cast<uintptr_t>(message) + 8, &handle, 4) || !handle || !Read(base + gate::kHandleTableRva, &table, 4) || !table ||
        !Read(table + (handle >> 23) * 8, &object, 4) || !Read(table + (handle >> 23) * 8 + 4, &id, 4) || id != handle)
        return 0;
    return object;
}
bool OnLocalDispatch(uint32_t hash, const void* message, int flags, const char*) {
    if (!installed || !enabled) return false;
    InterlockedIncrement(&offered);
    if (scenechain::Owns(hash)) return HoldGroupStep(hash, message, flags);
    // a Dementia phantom's own messages (its death, sent by its health component) stay on this machine
    if (phantom::Any()) {
        const uint32_t sender = SenderObject(message);
        uint32_t owner = 0;
        if (sender && (phantom::Is(sender) || (Read(sender + hits::kComponentOwner, &owner, 4) && phantom::Is(owner)))) {
            InterlockedIncrement(&phantom::messagesDropped);
            return false;
        }
    }
    // The finale wakes from any message, a replayed one or a watching necro's too (a necro's
    // machine may hear the finale's names only inside a replayed scene).
    if (!finaleActive && scopePatternCount) LocalName(hash);
    // A necro watching a scene as Isaac is a spectator: what his world fires reaches nobody.
    if (gate::mirroring == 0 && gate::replaying == 0 && necro::LocalRole() == 3) { InterlockedIncrement(&watcherSendsDropped); return false; }
    if (gate::mirroring == 0 && gate::replaying == 0 && LocalName(hash)) { NoteLocalName(hash, "not sent"); return false; }
    // The MP maps' objective chain reaches every machine whatever else hears it
    // (a MultiRemoveTarget listens to every obj_N_complete on Marker Lab); the
    // receivers run it once per round (objonce).
    if (gate::mirroring == 0 && gate::replaying == 0 && !excludeHashes.Has(hash) && objonce::Is(hash)) {
        InterlockedIncrement(&objonce::sentAnyway);
        gate::Event("coop_mirror objective %08X (%s) shared: the round's objective chain runs once on every machine", hash, coop_names::Name(hash));
        SendLocal(hash, message, flags);
        return false;
    }
    if (enemySpawnsFromClients && hostNPCs && gate::mirroring == 0 && gate::replaying == 0 && HostileSpawnOnly(hash)) {
        if (!IsHost()) { SendEnemyRequest(hash, message, flags); return false; }   // the local spawn is suppressed; the host spawns it
        if (TakeEnemyRequest(hash, message)) {
            InterlockedIncrement(&enemyRequestsFolded);
            gate::Event("coop_mirror enemy spawn %08X (%s) of ours dropped: a client's request from the same sender already spawned it", hash,
                        coop_names::Name(hash));
            return true;
        }
    }
    const DWORD now = GetTickCount();
    if (promptsShared && gate::mirroring == 0 && gate::replaying == 0 && IsPromptResult(hash)) {
        if (cutscene::Following()) {
            // The local result does not count; the leader's replay must not be folded against it.
            gate::ForgetRecent(hash);
            if (InterlockedIncrement(&promptDropped) <= 32)
                gate::Event("coop_mirror prompt result %08X (%s) of ours dropped: we follow, the leader's result decides", hash, coop_names::Name(hash));
            return true;
        }
        if (cutscene::Leading()) {
            InterlockedIncrement(&promptSent);
            gate::Event("coop_mirror prompt result %08X (%s) sent: we lead this scene", hash, coop_names::Name(hash));
            SendLocal(hash, message, flags);
            return false;
        }
    }
    if (actionOutcomes && gate::mirroring == 0 && gate::replaying == 0 && IsActionOutcome(hash)) {
        if (OutcomePlayedHere(hash)) {
            InterlockedIncrement(&outcomesKept);
            gate::Event("coop_mirror action outcome %08X (%s) of ours dropped: a partner's already ran its chain here", hash, coop_names::Name(hash));
            return true;
        }
        InterlockedIncrement(&outcomesSent);
        gate::Event("coop_mirror action outcome %08X (%s) of ours shared: partners run its chain too", hash, coop_names::Name(hash));
        SendLocal(hash, message, flags);
        return false;
    }
    if (sourceShare && gate::mirroring == 0 && gate::replaying == 0 && !excludeHashes.Has(hash)) {
        uint32_t guid[4]{};
        char cls[32] = "";
        if (SharedSource(message, cls, sizeof(cls), guid)) {
            if (SourceDoneHere(hash, guid)) {
                InterlockedIncrement(&sourceDropped);
                gate::Event("coop_mirror %s %08X-%08X result %08X (%s) of ours dropped: a partner's already ran here", cls, guid[0], guid[1], hash,
                            coop_names::Name(hash));
                return true;
            }
            char unsafeCls[32] = "";
            unsigned listeners = 0;
            if (gate::AnyListener(hash, sourceUnsafeRules, unsafeCls, sizeof(unsafeCls), listeners)) {
                InterlockedIncrement(&sourceUnsafe);
                if (InterlockedIncrement(&sourceLines) <= 200)
                    gate::Event("coop_mirror %s %08X-%08X result %08X (%s) stays local: also heard by %s", cls, guid[0], guid[1], hash,
                                coop_names::Name(hash), unsafeCls);
            } else {
                NoteSourceDone(hash, guid);
                InterlockedIncrement(&sourceSent);
                if (InterlockedIncrement(&sourceLines) <= 200)
                    gate::Event("coop_mirror %s %08X-%08X result %08X (%s) shared", cls, guid[0], guid[1], hash, coop_names::Name(hash));
                SendLocal(hash, message, flags);
                std::lock_guard<std::mutex> guard(lock);
                Out& o = outbox[(outHead + kOutbox - 1) % kOutbox];
                std::memcpy(o.packet.reserved, guid, 16);
                o.packet.reserved[5] = kSourceMark;
                return false;
            }
        }
    }
    if ((neutralSpawns || propSpawns) && gate::mirroring == 0 && gate::replaying == 0) {
        const uint32_t spawner = gate::ListenerWithVtable(hash, base + kFuncSpawnVtableRva);
        uint32_t attributes = 0, max = 0;
        if (spawner && Read(spawner + kFuncSpawnAttributes, &attributes, 4) && attributes) {
            const int cls = PropClassOf(attributes);
            Read(attributes + 0x1C, &max, 4);
            if (cls == 2 && neutralSpawns) {
                if (NeutralDoneHere(hash, now, OnceBook::kLocal)) {
                    InterlockedIncrement(&neutralOwnDropped);
                    gate::Event("coop_mirror neutral NPC spawn %08X (%s) of ours dropped: the partner's copy already spawned it here", hash, coop_names::Name(hash));
                    return true;
                }
                NoteNeutralDone(hash, now, OnceBook::kLocal);
                // A partner's copy still waiting for its spawner here is superseded by this one.
                for (unsigned i = 0; i < neutralWaitCount;)
                    if (neutralWaits[i].packet.hash == hash) neutralWaits[i] = neutralWaits[--neutralWaitCount]; else ++i;
                InterlockedIncrement(&neutralSent);
                gate::Event("coop_mirror neutral NPC spawn %08X (%s) of ours shared (max=%d)", hash, coop_names::Name(hash), static_cast<int>(max));
                SendLocal(hash, message, flags);
                std::lock_guard<std::mutex> guard(lock);
                outbox[(outHead + kOutbox - 1) % kOutbox].packet.reserved[5] = kNeutralSpawnMark;
                return false;
            }
            if (cls == 1 && propSpawns) {
                InterlockedIncrement(&propSent);
                gate::Event("coop_mirror scene prop request %08X (%s) sent (found at dispatch)", hash, coop_names::Name(hash));
                SendLocal(hash, message, flags);
                std::lock_guard<std::mutex> guard(lock);
                outbox[(outHead + kOutbox - 1) % kOutbox].packet.reserved[5] = kPropSpawnMark;
                return false;
            }
            if (cutscene::Leading() || cutscene::SequenceRunningNow() || now - static_cast<DWORD>(InterlockedCompareExchange(&gate::lastSequenceTick, 0, 0)) < 5000)
                if (InterlockedIncrement(&spawnRequestsUnshared) <= 64)
                    gate::Event("coop_mirror spawn request %08X (%s) near a scene not shared: class=%d max=%d", hash, coop_names::Name(hash), cls, static_cast<int>(max));
        }
    }
    if (propSpawns && gate::mirroring == 0 && gate::replaying == 0 && IsPropRequest(hash)) {
        InterlockedIncrement(&propSent);
        gate::Event("coop_mirror scene prop request %08X (%s) sent", hash, coop_names::Name(hash));
        SendLocal(hash, message, flags);
        std::lock_guard<std::mutex> guard(lock);
        outbox[(outHead + kOutbox - 1) % kOutbox].packet.reserved[5] = kPropSpawnMark;
        return false;
    }
    if (spawnerMessages && hostNPCs && IsHost() && gate::mirroring == 0 && gate::replaying == 0 &&
        !excludeHashes.Has(hash) && (IsSpawnDone(hash) || FromFuncSpawn(message))) {
        if (trace && InterlockedIncrement(&spawnerNamed) <= 64)
            gate::Event("coop_mirror spawner message hash=%08X name=%s repeated for the clients", hash, coop_names::Name(hash));
        InterlockedIncrement(&spawnerSent);
        uint32_t spawned[4]{};
        if (SpawnedGuid(message, spawned)) {
            InterlockedIncrement(&spawnGuidSent);
            SendLocalWithGuid(hash, message, flags, spawned);
        } else {
            SendLocal(hash, message, flags);
        }
        return false;
    }
    Verdict& v = verdicts[hash % 512];
    if (v.hash != hash || now - v.at > 10000) {
        unsigned listeners = 0;
        char cls[32] = "";
        bool yes = false, counts = false, isSequence = false, isMultiManager = false, isVoice = false;
        const char* gap = nullptr;
        if (excludeHashes.Has(hash)) {
            InterlockedIncrement(&excluded);
            gap = "excluded by hash";
        } else if (include.Has(hash)) {
            yes = true;
            std::snprintf(cls, sizeof(cls), "%s", "Include");
        } else if (gate::AnyListener(hash, anyRules, cls, sizeof(cls), listeners)) {
            // Checked before the exclusions: ch11's ses_11_06_player_010 is
            // heard by its ScriptedEventSequence AND by an audio MultiManager,
            // and the excluded MultiManager kept the whole scene from the
            // partner (23.09.).
            // 21:00: every scripted sequence is shared, from whoever
            // fires it, and the few that must not be are listed by name
            // (ExcludeHashes), far less work than naming the ones to share.
            // The receiver folds a cue it fired itself within five seconds.
            yes = true;
            isSequence = true;
        } else if (relayMultiManagers && gate::ListenerWithVtable(hash, base + kMultiManagerVtableRva)) {
            // The receiver's copy sends only the safe targets (OnSendMessages).
            if (TriggerUnsafe(hash)) {
                InterlockedIncrement(&mmUnsafeTriggers);
                InterlockedIncrement(&excluded);
                gap = "MultiManager trigger also heard by an unsafe class";
            } else {
                yes = IsHost() || clientsMirror;
                isMultiManager = true;
                std::snprintf(cls, sizeof(cls), "%s", "MultiManager");
                if (yes) InterlockedIncrement(&mmTriggersRelayed);
            }
        } else if (gate::AnyListener(hash, excludeRules, cls, sizeof(cls), listeners)) {
            InterlockedIncrement(&excluded);
            gap = "excluded class";
        } else if (gate::AnyListener(hash, voiceRules, cls, sizeof(cls), listeners)) {
            // 23.09.: scene dialogue is ScriptedSentences (Isaac) and
            // PlayAudioStreams (the NPCs) started by a trigger in the room
            // through a MultiManager. The scene reached the partner it pulled
            // in, the lines did not: he played the cutscene in silence.
            yes = true;
            isVoice = true;
        } else if (gate::AnyListener(hash, worldRules, cls, sizeof(cls), listeners)) {
            // 2026-09-17: a ghost clipping through a shut door is fine;
            // a player up front being handed a way back is not. So the world
            // is repeated by the host alone unless ClientsMirror=1, and a
            // client behind the group can open nothing for anybody.
            yes = IsHost() || clientsMirror;
        } else if (gate::AnyListener(hash, hostRules, cls, sizeof(cls), listeners)) {
            yes = IsHost();                                // the host's alone
            counts = true;
        }
        if (isSequence && v.hash != hash && StoryActorCue(hash)) {
            static volatile LONG actorRouteLines = 0;
            if (InterlockedIncrement(&actorRouteLines) <= 64)
                gate::Event("coop_mirror actor cue=%08X name=%s listeners=%u shared without a player wait", hash, coop_names::Name(hash), listeners);
        }
        if (yes && trace && v.hash != hash) gate::Event("coop_mirror world hash=%08X class=%s listeners=%u", hash, cls, listeners);
        if (!yes && v.hash != hash) NoteGap(hash, gap ? gap : (isMultiManager ? "a client's MultiManager" : "no shared class"), gap ? cls : "");
        v.hash = hash; v.at = now; v.mirror = yes; v.counts = counts; v.sequence = isSequence; v.multiManager = isMultiManager; v.voice = isVoice;
        std::snprintf(v.cls, sizeof(v.cls), "%s", cls);
    }
    const auto policy = scene_catalog::PolicyFor(hash);
    if (policy == scene_catalog::Policy::Personal && !mpmap::InVisit() && !play::SurvivalMode() && !play::VersusMode()) return false;
    const bool playerSequence = v.sequence && !StoryActorCue(hash);
    if (playerSequence && v.mirror && gate::mirroring == 0 && gate::replaying == 0 &&
        (policy == scene_catalog::Policy::Pull || policy == scene_catalog::Policy::Gather) && RoutesStoryScene(hash, now)) {
        if (storyOnce && storyCues.Taken(hash, OnceBook::kLocal) >= 0) return true;
        if (SubmitStoryStart(hash, message, flags, now, streams::area)) return true;
    }
    if (playerSequence) InterlockedExchange(&gate::lastSequenceTick, static_cast<LONG>(now));
    if (storyOnce && v.mirror && (v.sequence || v.voice) && gate::mirroring == 0 && gate::replaying == 0) {
        const int first = storyCues.Taken(hash, OnceBook::kLocal);
        if (first >= 0) {
            InterlockedIncrement(&storyCues.swallowedLocal);
            if (InterlockedIncrement(&storyCues.lines) <= 200) {
                char who[24];
                gate::Event("coop_mirror story cue %08X (%s) of ours dropped: %s played it here already", hash, coop_names::Name(hash),
                            OriginName(first, who, sizeof(who)));
            }
            return true;
        }
        if (playerSequence && ghostseq::Take(hash, message, flags, now)) return true;
        storyCues.Note(hash, OnceBook::kLocal);
    }
    if (!v.mirror) {
        if (enemyMessages && IsHost() && gate::mirroring == 0 && gate::replaying == 0 && !excludeHashes.Has(hash) && IsEnemyMessage(hash) &&
            !TriggerUnsafe(hash)) {
            if (InterlockedIncrement(&enemyMessagesSent) <= 400)
                gate::Event("coop_mirror enemy message %08X (%s) sent: heard by %s here, the clients' ghosts never send it", hash,
                            coop_names::Name(hash), v.cls[0] ? v.cls : "no mirrored class");
            SendLocal(hash, message, flags);
        }
        return false;
    }
    // The local Isaac's own action (choice A): nobody is pulled into it.
    // The same for a MultiManager a local use triggered: the begin chain of a panel,
    // machine or shaft (camera, helmet, sequence) stays local; its outcome is
    // shared (ActionOutcomes).
    if ((playerSequence || v.multiManager) && gate::personalActions && gate::mirroring == 0 && gate::replaying == 0 && !cutscene::Following()) {
        const LONG usedAt = InterlockedCompareExchange(&gate::lastUseAt, 0, 0);
        if (usedAt && now - static_cast<DWORD>(usedAt) <= kPersonalWindowMs) {
            InterlockedIncrement(&personalKept);
            gate::Event("coop_mirror %s %08X (%s) is our Isaac's own action (%s used %u ms before): not shared",
                        v.multiManager ? "multimanager trigger" : "sequence", hash,
                        coop_names::Name(hash), gate::lastUseClass, static_cast<unsigned>(now - static_cast<DWORD>(usedAt)));
            return false;
        }
    }
    // Already happened for the group: not sent again (see history above).
    if (!IsHost() && InHistory(hash)) { InterlockedIncrement(&heldBack); return false; }
    // A partner leads the cutscene the local Isaac is playing: the cues fired here are the
    // local outcome of it and stay here (runtime_coop_cutscene.inc).
    if (playerSequence && gate::mirroring == 0 && gate::replaying == 0 && (cutscene::Following() || cutscene::InFollowedChain(now))) {
        cutscene::NoteSuppressed(hash);
        if (!followerCueCatchUp) return false;
        SendLocal(hash, message, flags);
        {
            std::lock_guard<std::mutex> guard(lock);
            outbox[(outHead + kOutbox - 1) % kOutbox].packet.reserved[5] = kFollowerCueMark;
        }
        InterlockedIncrement(&followerCuesSent);
        return false;
    }
    // A local sequence, and a partner does not have the local room loaded: it
    // waits (and so does its dispatch here), see seqwait and SequenceTick.
    if (playerSequence && seqwait::enabled && gate::replaying == 0 && gate::mirroring == 0 && inLevel && levelSettled) {
        char who[128] = "";
        if ((seqwait::count || !streams::SceneReady(streams::area, now, who, sizeof(who))) &&
            seqwait::Take(hash, message, flags, now, who, streams::area))
            return true;
    }
    if (playerSequence && gate::mirroring == 0) streams::SetReference(0xFEu, now);   // local cutscene: this machine leads the streams
    // A message that COUNTS (a death into a quarantine) is never throttled:
    // two kills inside half a second are two, and 17:31 the quarantine on the
    // client stood at three of five for exactly this and the fold below.
    Sent& last = lastSent[hash % 512];
    const bool counted = v.counts || (enemyMessages && IsEnemyMessage(hash));   // two deaths of the same name are two
    if (!counted && last.hash == hash && now - last.at < kPerHashMs) { InterlockedIncrement(&throttled); return false; }
    last.hash = hash; last.at = now;
    SendLocal(hash, message, flags, playerSequence ? streams::area : nullptr);
    return false;
}

// The object-by-GUID lookup Checkpoint::Find uses: [0x0201C0A0]->Find(guid, 0)
// (0x00776CA0, thiscall). Returns the object or 0.
constexpr uint32_t kObjectRegistryRva = 0x1C1C0A0, kFindByGuidRva = 0x376CA0;
int LookupFilter(unsigned) { return EXCEPTION_EXECUTE_HANDLER; }
uint32_t ObjectByGuid(const uint32_t* guid) {
    uint32_t registry = 0;
    if (!Read(base + kObjectRegistryRva, &registry, 4) || !registry) return 0;
    using Find = uint32_t(__thiscall*)(void*, const uint32_t*, uint32_t);
    __try {
        return reinterpret_cast<Find>(base + kFindByGuidRva)(reinterpret_cast<void*>(registry), guid, 0);
    } __except (LookupFilter(GetExceptionCode())) {
        return 0;
    }
}

// A direct send seen on the host (from the SendMessageTo hook): is the
// target a world object worth repeating, and what is its GUID?
struct DirectVerdict { uint32_t vtable; DWORD at; bool mirror; char cls[32]; };
DirectVerdict directVerdicts[256]{};
volatile LONG directOffered = 0, directMirrored = 0, directReplayed = 0, directNotFound = 0, directSceneDropped = 0, directSceneLogged = 0,
              directFollowerDropped = 0;
bool ClassOf(uint32_t object, uint32_t vtable, const gate::RuleSet& rules, char* cls, size_t clsSize) {
    for (unsigned i = 0; i < rules.nodeCount; ++i)
        if (vtable == base + rules.nodeVtables[i]) { std::snprintf(cls, clsSize, "%s", rules.nodeNames[i]); return true; }
    for (unsigned i = 0; i < rules.entityCount; ++i)
        if (gate::IsA(object, vtable, rules.entities[i].hash)) { std::snprintf(cls, clsSize, "%s", rules.entities[i].name); return true; }
    return false;
}
// Direct sends whose payload is a pointer cannot travel: call_lua (B2385537,
// 2644 times in the 17:06 run) carries the script's address, which means
// nothing on the other machine. Listed by hash in [Mirror] DirectExclude.
constexpr unsigned kMaxDirectExclude = 32;
uint32_t directExclude[kMaxDirectExclude]{};
unsigned directExcludeCount = 0;
volatile LONG directExcluded = 0, directFollowing = 0, necroSceneSkips = 0;
bool DirectExcluded(uint32_t hash) {
    for (unsigned i = 0; i < directExcludeCount; ++i) if (directExclude[i] == hash) return true;
    return false;
}
void OnLocalDirect(uint32_t hash, const void* message, uint32_t target) {
    if (!installed || !enabled || !IsHost() || !target || !peerTable.Count() || !message) return;
    if (DirectExcluded(hash)) { InterlockedIncrement(&directExcluded); return; }
    // Following a client's scene, the host's direct sends are its copy of that scene: sent
    // back they doubled the leader's own (audit 16.10., the 24.09. double gun_fire reversed).
    if (cutscene::Following()) { InterlockedIncrement(&directFollowing); return; }
    if (LocalName(hash)) { NoteLocalName(hash, "direct send not shared"); return; }
    unsigned char payload[kPayload]{};
    Read(reinterpret_cast<uintptr_t>(message) + 12, payload, kPayload);   // best effort: a shorter message reads its neighbours
    if (coop_internal::IsInternal(hash)) return;
    uint32_t vtable = 0;
    if (!Read(target, &vtable, 4) || vtable < base + gate::kRdataLo || vtable >= base + gate::kRdataHi) return;
    InterlockedIncrement(&directOffered);
    const DWORD now = GetTickCount();
    // Verdict per target class (vtable), not per message: the same door gets
    // many names, the class decides.
    DirectVerdict& v = directVerdicts[(vtable >> 2) % 256];
    if (v.vtable != vtable || now - v.at > 10000) {
        char cls[32] = "";
        bool yes = false;
        if (ClassOf(target, vtable, excludeRules, cls, sizeof(cls))) yes = false;
        else yes = ClassOf(target, vtable, worldRules, cls, sizeof(cls)) || ClassOf(target, vtable, hostRules, cls, sizeof(cls));
        if (yes && trace && v.vtable != vtable) gate::Event("coop_mirror direct target class=%s vtable=%08X", cls, vtable);
        v.vtable = vtable; v.at = now; v.mirror = yes;
        std::snprintf(v.cls, sizeof(v.cls), "%s", cls);
    }
    if (!v.mirror) return;
    uint32_t guid[4]{};
    if (!Read(target + 0x24, guid, 16) || !(guid[0] | guid[1] | guid[2] | guid[3])) return;
    // Per object and message: at most twice a second, and an identical
    // payload not again within two seconds. One ScriptableProp took the same
    // message 6264 times in a session (20:50): a per-tick update, state,
    // not an event, and it filled the outbox, the journal and the log with it.
    {
        static struct { uint32_t hash, guid0, guid1, payloadHash; DWORD at; } recentDirect[64]{};
        uint32_t payloadHash = 0;
        for (unsigned i = 0; i < kPayload; ++i) payloadHash = payloadHash * 31 + payload[i];
        const unsigned slot = (hash ^ guid[1]) % 64;
        auto& r = recentDirect[slot];
        if (r.hash == hash && r.guid0 == guid[0] && r.guid1 == guid[1]) {
            if ((r.payloadHash == payloadHash && now - r.at < 2000) || now - r.at < 500) { InterlockedIncrement(&throttled); return; }
        }
        r = {hash, guid[0], guid[1], payloadHash, now};
    }
    std::lock_guard<std::mutex> guard(lock);
    Out& o = outbox[outHead];
    outHead = (outHead + 1) % kOutbox;
    // Byte +4 of the message is the dispatch's in-flight marker, not data: on the
    // wire it would read as a journal request (2) or a pre-approval (8).
    o.packet = Packet{kMagic, kVersion, peerReadiness.token, ++sequence, hash, kFlagDirect, now, {}};
    std::memcpy(o.packet.reserved, guid, 16);
    std::memcpy(&o.packet.reserved[4], payload, kPayload);   // live: reserved[4..9] = the payload
    o.at = now - kRepeatMs;
    o.sent = 0;
    InterlockedIncrement(&directMirrored);
    JournalNote(hash, o.packet.flags, guid, payload);
    // The log line once per object and message per ten seconds.
    static struct { uint32_t hash, guid1; DWORD at; } logged[32]{};
    auto& l = logged[(hash ^ guid[1]) % 32];
    if (trace && !(l.hash == hash && l.guid1 == guid[1] && now - l.at < 10000)) {
        l = {hash, guid[1], now};
        gate::Event("coop_mirror direct hash=%08X name=%s class=%s guid=%08X-%08X-%08X-%08X", hash, coop_names::Name(hash), v.cls, guid[0], guid[1], guid[2], guid[3]);
    }
}

// A world call happened here (from a hook in runtime_coop_worldcalls; host only unless anyMachine).
// The hash is synthetic (0xC0DE0000 | id) so the journal folds per object and
// call, keeping the latest arguments.
volatile LONG callsMirrored = 0, callsReplayed = 0, callsNotFound = 0;
void OnLocalCall(const uint32_t* guid, uint32_t callId, uint32_t a0, uint32_t a1, const char* what, bool anyMachine = false, bool keep = true) {
    if (!installed || !enabled || (!IsHost() && !anyMachine) || !peerTable.Count() || !guid || !(guid[0] | guid[1] | guid[2] | guid[3])) return;
    const DWORD now = GetTickCount();
    const uint32_t hash = 0xC0DE0000u | (callId & 0xFFFF);
    unsigned char payload[kPayload]{};
    const uint32_t words[3] = {callId, a0, a1};
    std::memcpy(payload, words, sizeof(words));
    std::lock_guard<std::mutex> guard(lock);
    Out& o = outbox[outHead];
    outHead = (outHead + 1) % kOutbox;
    o.packet = Packet{kMagic, kVersion, peerReadiness.token, ++sequence, hash, kFlagCall, now, {}};
    std::memcpy(o.packet.reserved, guid, 16);
    std::memcpy(&o.packet.reserved[4], payload, kPayload);
    o.at = now - kRepeatMs;
    o.sent = 0;
    InterlockedIncrement(&callsMirrored);
    if (keep) JournalNote(hash, o.packet.flags, guid, payload);   // a door vote is a moment, not state: not for late joiners
    if (trace) gate::Event("coop_mirror call %s(%u, %u) guid=%08X-%08X-%08X-%08X", what, a0, a1, guid[0], guid[1], guid[2], guid[3]);
}

// Receive: fold retransmits, queue for the game thread, and (on the host)
// pass a client's message on to the other clients, every copy of it.
void OnPacket(const Packet& p, unsigned fromSlot) {
    if (!installed || !enabled) return;
    if (p.reserved[5] == scene_delivery::kAck || (p.reserved[5] == kSceneSetMark && (p.reserved[7] & agreement::kCommitReceipt))) {
        std::lock_guard<std::mutex> guard(lock);
        scene_delivery::Handle(p, fromSlot, GetTickCount());
        return;
    }
    if (p.reserved[5] == kSceneSetMark && p.reserved[6] && (p.reserved[4] || (p.flags & kFlagDeferred))) return;
    if (p.flags & kFlagJournalRequest) {
        if (!IsHost()) return;
        InterlockedIncrement(&journalRequests);
        std::lock_guard<std::mutex> guard(lock);
        const unsigned asker = fromSlot % coop_peers::kMaxPeers;
        if (lastJournalAsk[asker] == (p.token ^ p.sequence)) return;   // a repeat of the one being answered
        lastJournalAsk[asker] = p.token ^ p.sequence;
        deliveries[asker] = Delivery{true, asker, 0};
        return;
    }
    InterlockedIncrement(&received);
    bool fresh = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        // The host's journal is the record of what the group already did:
        // a client repeating any of it is not relayed and not replayed.
        if (IsHost() && !(p.flags & (kFlagJournal | kFlagCall)) && p.reserved[5] != scenechain::kMark && !(p.reserved[5] == kSceneSetMark && p.reserved[6]) && InJournal(p.hash)) { InterlockedIncrement(&heldBack); return; }
        const unsigned author = SlotForToken(p.token, fromSlot, GetTickCount()) % coop_peers::kMaxPeers;
        if (seenToken[author] != p.token) {
            seenToken[author] = p.token;
            std::memset(seen[author], 0, sizeof(seen[author]));
            seenHighest[author] = 0;
        }
        uint32_t& slot = seen[author][p.sequence % 256];
        if (seenHighest[author] >= 256 && p.sequence <= seenHighest[author] - 256) {
            InterlockedIncrement(&staleSequences);
        } else if (slot != p.sequence) {
            if (inCount < kInbox) {
                slot = p.sequence;
                if (p.sequence > seenHighest[author]) seenHighest[author] = p.sequence;
                fresh = true;
                inbox[inCount++] = p;
            } else {
                InterlockedIncrement(&dropped);   // not marked seen, so a repeat can still get in
            }
        }
    }
    if (IsHost() && !(p.flags & kFlagRelayed)) {
        // Every copy, not just the first: the author's repeats cover his leg
        // to the host, and one relayed copy per message lost 1-3 % of them on the
        // leg to the other clients. They fold the extra copies themselves.
        Packet forward = p;
        forward.flags |= kFlagRelayed;
        RelayToOthers(&forward, sizeof(forward), fromSlot);
        if (fresh) InterlockedIncrement(&relayed);
    }
    if (!fresh) { InterlockedIncrement(&duplicate); return; }
}

// One host message on this machine. The local logic may have fired the same
// message a moment ago (the same door, opened by the local Isaac), and then
// there is nothing to repeat. The source stays zero: it is a handle on the
// host's machine and means nothing here; the randomizer's own scripted
// releases have been sent sourceless for a day without a listener minding.
bool ReplayDirect(const Packet& p);
void Replay(const Packet& p, DWORD now);
// A partner's sequence cue held until his set is loaded here (or until
// followWaitMs is over). kFlagDeferred marks the second pass.
bool followSequence = true;
DWORD followWaitMs = 10000;

constexpr unsigned kDeferred = 16;
struct DeferredCue { bool used; Packet packet; uint32_t set[4]; DWORD since; };
DeferredCue deferredCues[kDeferred]{};
unsigned deferredCount = 0;
volatile LONG deferred = 0, deferredPlayed = 0, deferredLate = 0;
// A partner's scene cue held while this necro machine turns back into Isaac to watch it
// (necro::AskWatch). Played in order once Isaac stands in slot 0; dropped when the watch is.
struct WatchCue { Packet packet; DWORD since; };
WatchCue watchCues[12]{};
unsigned watchCueCount = 0;
volatile LONG watchCuesQueued = 0, watchCuesPlayed = 0, watchCuesDropped = 0;
bool QueueWatchCue(const Packet& p, DWORD now) {
    if (watchCueCount >= std::size(watchCues)) return false;
    auto& cue = watchCues[watchCueCount++];
    cue = WatchCue{p, now};
    // This commit already consumed its reservation before the spectator queued it.
    if (p.reserved[5] == kSceneSetMark && p.reserved[6]) cue.packet.flags |= kFlagDeferred;
    InterlockedIncrement(&watchCuesQueued);
    gate::Event("coop_mirror scene cue %08X (%s) waits for our Isaac: the necro watches the scene (%s)", p.hash, coop_names::Name(p.hash),
                spectate::Active() ? "a dead human" : necro::WatchWaitReason());
    return true;
}
void WatchCueTick(DWORD now) {
    if (!watchCueCount) return;
    if (necro::WatchReady() || spectate::SceneReady()) {
        const unsigned n = watchCueCount;
        WatchCue cues[std::size(watchCues)];
        std::memcpy(cues, watchCues, sizeof(WatchCue) * n);
        watchCueCount = 0;
        for (unsigned i = 0; i < n; ++i) {
            InterlockedIncrement(&watchCuesPlayed);
            gate::Event("coop_mirror scene cue %08X (%s) played for the watching necro, %u ms after it came", cues[i].packet.hash,
                        coop_names::Name(cues[i].packet.hash), static_cast<unsigned>(now - cues[i].since));
            // late is broken too: the scene then runs seconds behind its leader (17.10.: 3 s with the watch suit)
            const unsigned late = static_cast<unsigned>(now - cues[i].since);
            gate::Event("test watch_cue %s: %08X played %u ms after it came", late <= 1500 ? "PASS" : "FAIL", cues[i].packet.hash, late);
            InterlockedExchange(&gate::lastSequenceTick, static_cast<LONG>(now));   // its actors (17.10.: the tripod) may spawn here
            Replay(cues[i].packet, now);
        }
        return;
    }
    if ((necro::WatchCuesHeld() || spectate::SceneCuesHeld()) && now - watchCues[0].since < 20000) return;
    InterlockedExchangeAdd(&watchCuesDropped, static_cast<LONG>(watchCueCount));
    gate::Event("coop_mirror %u scene cue(s) for the watching necro dropped: %s (%s)", watchCueCount, necro::WatchCuesHeld() ? "20 s without Isaac" : "the watch was dropped",
                necro::WatchWaitReason());
    gate::Event("test watch_cue FAIL: %u cue(s) dropped (%s)", watchCueCount, necro::WatchWaitReason());
    watchCueCount = 0;
}
bool Defer(const Packet& p, const uint32_t* set, DWORD now) {
    if (deferredCount >= kDeferred) return false;
    DeferredCue& d = deferredCues[deferredCount++];
    d.used = true; d.packet = p; d.since = now;
    if (p.reserved[5] == kSceneSetMark) d.packet.reserved[4] = 1;
    else d.packet.flags |= kFlagDeferred;
    std::memcpy(d.set, set, 16);
    if (deferredCount == 1 && !streams::Covers(set)) streams::Follow(set);
    gate::Event("coop_mirror sequence deferred until the partner's set is loaded hash=%08X name=%s set=%08X-%08X", p.hash, coop_names::Name(p.hash), set[0], set[1]);
    return true;
}
// Waiting cues leave their queue in arrival order: a scene's cues released in one tick
// play as the partner fired them (19.10., igc_03_03: the client replayed start_player
// before pull_player_to_start, the pull replaced part 010 and the scene never ran there).
template <typename T>
void EraseKeepOrder(T* items, unsigned& count, unsigned index) {
    for (unsigned j = index + 1; j < count; ++j) items[j - 1] = items[j];
    --count;
}
void FollowerCueTick(DWORD now) {
    for (unsigned i = 0; i < followerCueCount;) {
        FollowerCue& c = followerCues[i];
        const DWORD age = now - c.since;
        // The local world fired it since it arrived (or just before): nothing to catch up.
        if (gate::SeenRecently(c.packet.hash, now, age + 1000)) {
            EraseKeepOrder(followerCues, followerCueCount, i);
            InterlockedIncrement(&followerCuesFolded);
            continue;
        }
        if (age < netclock::Window(kFollowerCueGraceMs)) { ++i; continue; }
        const Packet packet = c.packet;
        EraseKeepOrder(followerCues, followerCueCount, i);
        if (!inLevel || !LocalSlotZeroEntity()) continue;
        InterlockedIncrement(&followerCuesPlayed);
        gate::Event("coop_mirror follower's cue %08X (%s) never fired here in %u ms: played, we catch up with him", packet.hash,
                    coop_names::Name(packet.hash), static_cast<unsigned>(age));
        Replay(packet, now);
    }
}
// A partner's sequence cue right behind his NPC spawn: here the spawn may still be on
// its way, and a sequence started without its actor never ends, nor does the removal
// at its end (13.10., ch02: crowd 5-7 spawned by the partner's dispatcher, their
// cues replayed 1 ms before the NPCs existed, remove_crowd_5/6/7 never ran on the
// host and the civilians stayed). The cue waits until no NPC spawn is pending and the
// last one had time to stand; our own spawn's cue folds it.
constexpr DWORD kNpcCueSettleMs = 1500, kNpcCueMaxMs = 15000;
struct NpcCue { Packet packet; DWORD since; };
NpcCue npcCues[16]{};
unsigned npcCueCount = 0;
DWORD neutralPlayedAt = 0;
bool ChurchActorsSettling(DWORD now) {
    return neutralWaitCount || npcCueCount || (neutralPlayedAt && now - neutralPlayedAt < kNpcCueSettleMs);
}
volatile LONG npcCuesHeld = 0, npcCuesFolded = 0, npcCuesPlayed = 0, npcCuesLate = 0, npcCueLines = 0, npcCuesQueuedBehind = 0;
bool HoldCueForNpc(const Packet& p, DWORD now) {
    if ((p.flags & (kFlagDeferred | kFlagJournal)) || npcCueCount >= std::size(npcCues)) return false;
    // a later cue never overtakes one still waiting
    const bool behind = npcCueCount > 0;
    if (!behind && !neutralWaitCount && !(neutralPlayedAt && now - neutralPlayedAt < kNpcCueSettleMs)) return false;
    unsigned listeners = 0;
    char cls[32] = "";
    if (!gate::AnyListener(p.hash, anyRules, cls, sizeof(cls), listeners)) return false;
    NpcCue& c = npcCues[npcCueCount++];
    c.packet = p;
    c.packet.flags |= kFlagDeferred;
    c.since = now;
    InterlockedIncrement(&npcCuesHeld);
    if (behind) InterlockedIncrement(&npcCuesQueuedBehind);
    if (InterlockedIncrement(&npcCueLines) <= 64)
        gate::Event("coop_mirror sequence cue %08X (%s) of a partner waits: %u NPC spawn(s) pending here, the last played %u ms ago%s", p.hash,
                    coop_names::Name(p.hash), neutralWaitCount, neutralPlayedAt ? static_cast<unsigned>(now - neutralPlayedAt) : 0u,
                    behind ? ", queued behind an earlier cue" : "");
    return true;
}
void Replay(const Packet& p, DWORD now);
void NpcCueTick(DWORD now) {
    bool blocked = false;
    for (unsigned i = 0; i < npcCueCount;) {
        NpcCue& c = npcCues[i];
        const DWORD age = now - c.since;
        if (gate::SeenRecently(c.packet.hash, now, age + 1000)) {
            EraseKeepOrder(npcCues, npcCueCount, i);
            InterlockedIncrement(&npcCuesFolded);
            continue;
        }
        const bool settled = !neutralWaitCount && (!neutralPlayedAt || now - neutralPlayedAt >= kNpcCueSettleMs);
        const bool late = age >= kNpcCueMaxMs;
        // an older cue still waiting keeps the newer ones behind it
        if (blocked || (!settled && !late)) { blocked = true; ++i; continue; }
        const Packet packet = c.packet;
        EraseKeepOrder(npcCues, npcCueCount, i);
        if (!inLevel || !LocalSlotZeroEntity()) continue;
        InterlockedIncrement(late ? &npcCuesLate : &npcCuesPlayed);
        if (InterlockedIncrement(&npcCueLines) <= 64)
            gate::Event("coop_mirror sequence cue %08X (%s) of a partner plays after %u ms%s", packet.hash, coop_names::Name(packet.hash),
                        static_cast<unsigned>(age), late ? " (the NPC spawns never settled)" : ": its NPCs had time to stand");
        Replay(packet, now);
    }
}
void NeutralWaitTick(DWORD now) {
    for (unsigned i = 0; i < neutralWaitCount;) {
        NeutralWait& w = neutralWaits[i];
        const bool ready = inLevel && gate::Registered(w.packet.hash);
        const bool late = now - w.since >= kNeutralWaitMs;
        if (!ready && !late) { ++i; continue; }
        const Packet packet = w.packet;
        const DWORD waited = now - w.since;
        neutralWaits[i] = neutralWaits[--neutralWaitCount];
        const unsigned author = SlotForToken(packet.token, 0xFFu, now);
        const unsigned char origin = author < coop_peers::kMaxPeers ? static_cast<unsigned char>(author) : 0xFD;
        if (ready && NeutralDoneHere(packet.hash, now, origin)) {
            InterlockedIncrement(&neutralPartnerDropped);
            gate::Event("coop_mirror neutral NPC spawn %08X of a partner dropped: our own request already spawned it", packet.hash);
        } else if (ready) {
            NoteNeutralDone(packet.hash, now, origin);
            InterlockedIncrement(&neutralPlayed);
            gate::Event("coop_mirror neutral NPC spawn %08X plays after %u ms", packet.hash, static_cast<unsigned>(waited));
            Replay(packet, now);
            neutralPlayedAt = Stamp(now);
        } else {
            InterlockedIncrement(&neutralExpired);
            gate::Event("coop_mirror neutral NPC spawn %08X dropped: its spawner never registered here within %u ms", packet.hash, kNeutralWaitMs);
        }
    }
}
void PropWaitTick(DWORD now) {
    const DWORD followed = cutscene::FollowingSince();
    for (unsigned i = 0; i < propWaitCount;) {
        PropWait& w = propWaits[i];
        const bool ready = followed && now - followed >= kPropSettleMs;
        const bool late = now - w.since >= kPropWaitMs;
        if (!ready && !late) { ++i; continue; }
        const Packet packet = w.packet;
        propWaits[i] = propWaits[--propWaitCount];
        if (ready) {
            InterlockedIncrement(&propPlayed);
            gate::Event("coop_mirror scene prop request %08X plays: we follow the scene", packet.hash);
            Replay(packet, now);
        } else {
            InterlockedIncrement(&propExpired);
            gate::Event("coop_mirror scene prop request %08X dropped: no scene followed here within %u ms", packet.hash, kPropWaitMs);
        }
    }
}
void SpawnWaitTick(DWORD now) {
    for (unsigned i = 0; i < spawnWaitCount;) {
        SpawnWait& w = spawnWaits[i];
        if (!w.foundAt && ObjectByGuid(w.packet.reserved)) w.foundAt = now;
        const bool ready = w.foundAt && now - w.foundAt >= kSpawnSettleMs;
        const bool late = now - w.since >= netclock::Window(kSpawnWaitMs, 5000);   // TNL brings the ghost one wire trip later
        if (!ready && !late) { ++i; continue; }
        const Packet packet = w.packet;
        const DWORD waited = now - w.since;
        spawnWaits[i] = spawnWaits[--spawnWaitCount];
        InterlockedIncrement(ready ? &spawnWaitFound : &spawnWaitLate);
        gate::Event("coop_mirror spawner message hash=%08X plays after %u ms: %s %08X-%08X", packet.hash, static_cast<unsigned>(waited),
                    ready ? "the spawned entity is here" : "TIMEOUT, the spawned entity never came", packet.reserved[0], packet.reserved[1]);
        Replay(packet, now);
    }
}
void DeferredTick(DWORD now) {
    if (!deferredCount) return;
    // In order: the head decides, everything behind it follows in one go once
    // the head is ready or late.
    DeferredCue& head = deferredCues[0];
    const bool late = now - head.since >= followWaitMs;
    if (!late && !streams::Covers(head.set)) return;
    DeferredCue batch[kDeferred];
    unsigned n = 1;
    while (n < deferredCount && !late && streams::GuidEqual(head.set, deferredCues[n].set)) ++n;
    std::memcpy(batch, deferredCues, sizeof(DeferredCue) * n);
    deferredCount -= n;
    std::memmove(deferredCues, deferredCues + n, sizeof(DeferredCue) * deferredCount);
    for (unsigned i = 0; i < n; ++i) {
        InterlockedIncrement(late ? &deferredLate : &deferredPlayed);
        gate::Event("coop_mirror deferred sequence plays hash=%08X after %u ms %s", batch[i].packet.hash, static_cast<unsigned>(now - batch[i].since), late ? "TIMEOUT (set not loaded)" : "(set loaded)");
        Replay(batch[i].packet, now);
    }
    if (deferredCount && !streams::Covers(deferredCues[0].set)) streams::Follow(deferredCues[0].set);
}
// A checkpoint restart or a new map: what waited for the old world goes with it.
void ClearWorldQueues() {
    std::fill(std::begin(gapSeen), std::end(gapSeen), 0u);
    InterlockedExchangeAdd(&watchCuesDropped, static_cast<LONG>(watchCueCount));
    watchCueCount = 0;
    cutscene::OnWorldReset();
    scenechain::early.clear();
    scenechain::Clear();
    agreement::Clear();
    seqwait::Clear();
    ghostseq::Clear();
    splitCount = 0;
    deferredCount = 0;
    spawnWaitCount = 0;
    propWaitCount = 0;
    neutralWaitCount = 0;
    followerCueCount = 0;
    npcCueCount = 0;
    neutralPlayedAt = 0;
    for (auto& d : neutralDone) d = NeutralDone{};
    storyCues.Clear();
    neutralBook.Clear();
    outcomesPlayedCount = 0;
    for (auto& k : sourcesDone) k = SourceKey{};
    for (auto& a : mmArms) a.manager = 0;
    for (auto& r : enemyRequests) r = EnemyRequest{};
    for (auto& f : faulted) f = FaultKey{};
    propClassCount = 0;
    for (auto& h : mmLiftHolds) h = MmLiftHold{};   // their managers belong to the world that goes
}
// The randomizer's vent windows and objective count follow the dispatch through
// its own detour in front of the gate's; a replay calls behind both.
using NoteMessageFn = void(__cdecl*)(uint32_t);
NoteMessageFn noteMessageFn = nullptr;
DWORD noteMessageLookAt = 0;
volatile LONG randomizerNotes = 0;
void NoteForRandomizer(uint32_t hash) {
    if (!noteMessageFn) {
        const DWORD now = GetTickCount();
        if (noteMessageLookAt && now - noteMessageLookAt < 5000) return;
        noteMessageLookAt = Stamp(now);
        if (const HMODULE module = ds2coop::PartnerModule("DS2RandomizerObserver.asi"))
            noteMessageFn = reinterpret_cast<NoteMessageFn>(GetProcAddress(module, "DS2Survival_NoteMessage"));
        if (!noteMessageFn) return;
    }
    noteMessageFn(hash);
    InterlockedIncrement(&randomizerNotes);
}

__declspec(noinline) void JournalReplayed(uint32_t hash, uint32_t flags) {
    std::lock_guard<std::mutex> guard(lock);
    JournalNote(hash, flags);
    InterlockedIncrement(&objonce::journaled);
}

bool HasQueuedRoot(const Packet& step) {
    return scenechain::PendingRoot(step, deferredCues, deferredCount, kSceneSetMark) ||
           scenechain::PendingRoot(step, npcCues, npcCueCount, kSceneSetMark) ||
           scenechain::PendingRoot(step, watchCues, watchCueCount, kSceneSetMark);
}
void ReplayReadySteps(DWORD now) {
    auto& steps = scenechain::early;
    for (size_t i = 0; i < steps.size();) {
        const Packet p = steps[i].packet;
        const bool ready = scenechain::Matches(p) && !scenechain::WaitsForExit(p.hash);
        if (!ready && (TicksSince(now, steps[i].at) < 5000 || HasQueuedRoot(p))) { ++i; continue; }
        steps.erase(steps.begin() + i);
        if (ready) Replay(p, now);
    }
}
void Replay(const Packet& p, DWORD now) {
    if (!(p.flags & kFlagCall) && zoneobj::Drops(p.hash, true)) return;   // a Zone visit's map never ends, not through a partner either
    if (!(p.flags & kFlagCall) && LocalName(p.hash)) { NoteLocalName(p.hash, "a partner's copy not played"); return; }
    const bool companion = !(p.flags & (kFlagJournal | kFlagDirect | kFlagCall | kFlagPrepare)) && p.reserved[5] == scenechain::kMark;
    if (companion && scenechain::Matches(p) && scenechain::WaitsForExit(p.hash)) {
        scenechain::QueueEarly(p, now);
        return;
    }
    if (companion && !scenechain::Matches(p)) {
        if ((scene_catalog::IsCompanion(p.reserved[7], p.hash) || scene_catalog::IsPromptBranch(p.reserved[7], p.hash)) && p.reserved[6] &&
            (HasQueuedRoot(p) || agreement::AcceptCommit(p.reserved[7], p.reserved[6], p.token, now) || agreement::AwaitingLeader(p.reserved[7], now))) scenechain::QueueEarly(p, now);
        return;
    }
    if (!companion && p.reserved[5] != kSceneSetMark && scenechain::Owns(p.hash) && !(p.flags & (kFlagCall | kFlagDirect | kFlagJournal))) return;
    const bool committed = !(p.flags & (kFlagJournal | kFlagDirect | kFlagCall | kFlagPrepare)) && p.reserved[5] == kSceneSetMark && p.reserved[6];
    if (committed && !(p.flags & kFlagDeferred) && !p.reserved[4]) {
        if (!agreement::AcceptCommit(p.hash, p.reserved[6], p.token, now)) {
            if (agreement::logLines < 100) {
                ++agreement::logLines;
                gate::Event("scene commit refused cue=%08X attempt=%u owner=%08X", p.hash, p.reserved[6], p.token);
            }
            return;
        }
        DropLocalCandidate(p.hash);
        agreement::Consume(p.hash, p.token);
    }
    if (scene_catalog::PolicyFor(p.hash) == scene_catalog::Policy::Personal && !mpmap::InVisit() && !play::SurvivalMode() && !play::VersusMode()) return;
    if (p.flags & kFlagPrepare) {
        if (StoryActorCue(p.hash)) return;
        InterlockedIncrement(&prepareReceived);
        if (!inLevel || !LocalSlotZeroEntity()) return;
        const unsigned author = SlotForToken(p.token, 0xFFu, now);
        uint32_t theirSet[4]{};
        const bool pinned = p.reserved[5] == kSceneSetMark;
        if (pinned) std::memcpy(theirSet, p.reserved, 16);
        const bool known = pinned || streams::PeerSet(author, theirSet);
        const bool have = known && streams::Covers(theirSet);
        if (known && !have) { streams::Follow(theirSet); InterlockedIncrement(&prepareFollowed); }
        if (author < coop_peers::kMaxPeers) streams::SetReference(author, now);
        gate::Event("coop_mirror partner %u waits with his scene %s: %s", author, coop_names::Name(p.hash),
                    !known ? "his stream set is unknown here" : have ? "we have his streams" : "loading his stream set");
        return;
    }
    if (p.flags & kFlagCall) {
        if (!inLevel || !LocalSlotZeroEntity()) { InterlockedIncrement(&notInLevel); return; }
        const uint32_t* payload = (p.flags & kFlagJournal) ? &p.reserved[5] : &p.reserved[4];
        if (worldcalls::ReplayCall(payload[0], p.reserved, payload + 1, (p.flags & kFlagJournal) != 0, SlotForToken(p.token, 0xFFu, now))) InterlockedIncrement(&callsReplayed);
        else InterlockedIncrement(&callsNotFound);
        return;
    }
    if (p.flags & kFlagDirect) {
        if (!inLevel || !LocalSlotZeroEntity()) { InterlockedIncrement(&notInLevel); return; }
        // 23.09., ch11: the host's scene sent "gun_fire" to a ScriptableProp;
        // replayed on the client (another area, not in the scene) the gun shot
        // HIS Isaac into the scene's prompts, he died and the death restarted
        // both. What a partner's scene does to world objects stays his while
        // he is in it and the local player is not.
        // A follower's own sequences send the same direct messages to the
        // same props; the leader's on top doubled them (24.09.: gun_fire to
        // Tiedemann's javelin twice, two replays faulted, Tiedemann in the
        // wrong animation on the pulled player).
        if (!(p.flags & kFlagJournal) && cutscene::Following()) {
            InterlockedIncrement(&directFollowerDropped);
            if (gate::SceneTraceOn()) gate::SceneTrace("coop_scene t=%lu direct %08X %s of the leader not replayed: we follow, our sequences send it",
                                                       static_cast<unsigned long>(now), p.hash, coop_names::Name(p.hash));
            return;
        }
        if (!(p.flags & kFlagJournal) && cutscene::PartnerInScene(SlotForToken(p.token, 0xFFu, now), now) && !cutscene::SequenceRunningNow()) {
            InterlockedIncrement(&directSceneDropped);
            if (InterlockedIncrement(&directSceneLogged) <= 32)
                gate::Event("coop_mirror direct hash=%08X name=%s not replayed: the partner is in his scene, we are not", p.hash, coop_names::Name(p.hash));
            return;
        }
        if (ReplayDirect(p)) {
            InterlockedIncrement(&directReplayed);
            if (trace) gate::Event("coop_mirror direct replayed hash=%08X", p.hash);
            if (gate::SceneTraceOn()) gate::SceneTrace("coop_scene t=%lu direct %08X %s replayed from a partner guid=%08X-%08X",
                                                       static_cast<unsigned long>(now), p.hash, coop_names::Name(p.hash), p.reserved[0], p.reserved[1]);
        }
        else InterlockedIncrement(&directNotFound);
        return;
    }
    if (!(p.flags & (kFlagCall | kFlagDirect | kFlagPrepare)) && story_objects::Opened(p.hash)) return;
    const bool partnerOutcome = actionOutcomes && !(p.flags & kFlagJournal) && IsActionOutcome(p.hash);
    const bool partnerSource = p.reserved[5] == kSourceMark && !(p.flags & kFlagJournal);
    if (promptsShared && IsPromptResult(p.hash)) {
        if (!cutscene::Following()) {
            InterlockedIncrement(&promptRefused);
            gate::Event("coop_mirror prompt result %08X (%s) of a partner not replayed: we do not follow his scene", p.hash, coop_names::Name(p.hash));
            return;
        }
        InterlockedIncrement(&promptReplayed);
        gate::Event("coop_mirror prompt result %08X (%s) of the leader replayed", p.hash, coop_names::Name(p.hash));
    }
    if (p.reserved[5] == kEnemySpawnMark && !(p.flags & kFlagDeferred) && !(p.flags & kFlagJournal)) {
        if (!IsHost() || !enemySpawnsFromClients) return;
        TraceStalkerRequest(p.hash, "requested");
        NoteEnemyRequest(p.hash, p.reserved);
        InterlockedIncrement(&enemyRequestsPlayed);
        gate::Event("coop_mirror enemy spawn %08X (%s) asked for by a client (sender %08X-%08X): played here", p.hash, coop_names::Name(p.hash),
                    p.reserved[0], p.reserved[1]);
    }
    if (p.reserved[5] == kFollowerCueMark && !(p.flags & kFlagDeferred) && !(p.flags & kFlagJournal)) {
        if (followerCueCount < std::size(followerCues)) {
            FollowerCue& c = followerCues[followerCueCount++];
            c.packet = p; c.packet.flags |= kFlagDeferred; c.since = now;
        }
        return;
    }
    if (p.reserved[5] == kNeutralSpawnMark && !(p.flags & kFlagDeferred) && !(p.flags & kFlagJournal)) {
        if (neutralWaitCount < std::size(neutralWaits)) {
            NeutralWait& w = neutralWaits[neutralWaitCount++];
            w.packet = p; w.packet.flags |= kFlagDeferred; w.since = now;
            InterlockedIncrement(&neutralWaited);
            gate::Event("coop_mirror neutral NPC spawn %08X of a partner waits until its spawner is here", p.hash);
        }
        return;
    }
    if (p.reserved[5] == kPropSpawnMark && !(p.flags & kFlagDeferred) && !(p.flags & kFlagJournal)) {
        if (propWaitCount < std::size(propWaits)) {
            PropWait& w = propWaits[propWaitCount++];
            w.packet = p; w.packet.flags |= kFlagDeferred; w.since = now;
            InterlockedIncrement(&propWaited);
            gate::Event("coop_mirror scene prop request %08X of a partner waits for our scene", p.hash);
        }
        return;
    }
    if (p.reserved[5] == kSpawnWaitMark && !(p.flags & kFlagDeferred) && !(p.flags & kFlagJournal)) {
        if (spawnWaitCount < kSpawnWaits) {
            SpawnWait& w = spawnWaits[spawnWaitCount++];
            w.packet = p; w.packet.flags |= kFlagDeferred; w.since = now; w.foundAt = 0;
            InterlockedIncrement(&spawnWaited);
            return;
        }
    }
    const bool actorOnly = StoryActorCue(p.hash);
    if (!actorOnly && (committed || followSequence) && p.reserved[5] == kSceneSetMark && !p.reserved[4] &&
        !(p.flags & kFlagJournal) && inLevel && LocalSlotZeroEntity() &&
        (deferredCount || !streams::Covers(p.reserved))) {
        if (Defer(p, p.reserved, now)) {
            InterlockedIncrement(&deferred);
            return;
        }
    }
    // The agreed church opener targets Isaac. Its NPC prerequisites were checked before the vote.
    if (!(committed && p.hash == scenechain::kChurch) && HoldCueForNpc(p, now)) return;
    // A journal entry is history, not a duplicate of anything done here: it goes
    // through as many times as it happened, folded against nothing.
    const bool fromHistory = (p.flags & kFlagJournal) != 0;
    if (fromHistory ? objonce::FromHistory(p.hash) : !objonce::AllowReplay(p.hash)) return;
    if (!committed && !companion && !fromHistory && gate::SeenRecently(p.hash, now, dedupeMs)) { InterlockedIncrement(&folded); return; }
    // Only into a world that exists here and knows the name. The dispatch has
    // no not-found branch, and a message replayed into a level still loading
    // is the best explanation for the loading screen that never ended.
    if (!inLevel || !LocalSlotZeroEntity()) { InterlockedIncrement(&notInLevel); return; }
    if (!gate::Registered(p.hash)) { InterlockedIncrement(&unregistered); return; }
    if (Faulted(p.hash)) { InterlockedIncrement(&faultedSkips); return; }
    bool storyCue = false;
    unsigned char storyOrigin = 0xFD;
    if (storyOnce && !fromHistory) {
        unsigned listeners = 0;
        char cls[32] = "";
        storyCue = gate::AnyListener(p.hash, anyRules, cls, sizeof(cls), listeners) || gate::AnyListener(p.hash, voiceRules, cls, sizeof(cls), listeners);
        const unsigned author = SlotForToken(p.token, 0xFFu, now);
        if (author < coop_peers::kMaxPeers) storyOrigin = static_cast<unsigned char>(author);
        const int first = storyCue ? storyCues.Taken(p.hash, storyOrigin) : -1;
        if (first >= 0 && !committed && !companion) {
            InterlockedIncrement(&storyCues.droppedPartner);
            if (InterlockedIncrement(&storyCues.lines) <= 200) {
                char who[24];
                gate::Event("coop_mirror story cue %08X (%s) of partner %u dropped: %s played it here already", p.hash, coop_names::Name(p.hash),
                            static_cast<unsigned>(storyOrigin), OriginName(first, who, sizeof(who)));
            }
            return;
        }
    }
    const int localRole = necro::LocalRole();
    const bool spectating = spectate::Active() && !spectate::SceneReady();
    if (spectating && !actorOnly) {
        // A dead human watching comes back beside the author first ([Spectate] SceneRespawn);
        // the cue then plays as a follower's would.
        unsigned sceneListeners = 0;
        char sceneCls[32] = "";
        if (gate::AnyListener(p.hash, anyRules, sceneCls, sizeof(sceneCls), sceneListeners)) {
            const unsigned author = SlotForToken(p.token, 0xFFu, now);
            if (!fromHistory && spectate::AskScene(now, author) && QueueWatchCue(p, now)) return;
            InterlockedIncrement(&necroSceneSkips);
            return;
        }
    }
    if (!actorOnly && (localRole == 1 || localRole == 2 || (localRole == 3 && !necro::WatchReady()))) {
        // A necro player's machine plays no human scene with a necro body or a ghost in slot 0.
        // It watches it as Isaac instead (necro::AskWatch): the cue waits here until Isaac stands
        // in slot 0, then plays as a follower's would. Refused: skipped, as before.
        unsigned sceneListeners = 0;
        char sceneCls[32] = "";
        if (gate::AnyListener(p.hash, anyRules, sceneCls, sizeof(sceneCls), sceneListeners)) {
            const unsigned author = SlotForToken(p.token, 0xFFu, now);
            if (!fromHistory && necro::AskWatch(now, author) && QueueWatchCue(p, now)) return;
            InterlockedIncrement(&necroSceneSkips);
            return;
        }
    }
    {
        // A partner's sequence cue opens the scripted-spawn window here too:
        // the actor it needs is spawned locally in the seconds that follow.
        unsigned listeners = 0;
        char cls[32] = "";
        if (!actorOnly && gate::AnyListener(p.hash, anyRules, cls, sizeof(cls), listeners)) {
            InterlockedExchange(&gate::lastSequenceTick, static_cast<LONG>(now));
            // The partner whose cutscene this is leads the streams for a while
            // (see streams::SetReference): whoever it pulls next to him gets
            // his room.
            if (!fromHistory) {
                const unsigned author = SlotForToken(p.token, 0xFFu, now);
                if (author < coop_peers::kMaxPeers) streams::SetReference(author, now);
            }
            // And it may pull the local player to him: if his room is not loaded here, his
            // set is activated first and the cue waits for it (deferred, up
            // to followWaitMs), so what the sequence shows exists here.
            if (followSequence && !fromHistory && !(p.flags & kFlagDeferred)) {
                const unsigned author = SlotForToken(p.token, 0xFFu, now);
                uint32_t theirSet[4]{};
                if (!companion && p.reserved[5] != kSceneSetMark && streams::PeerSet(author, theirSet) && !streams::Covers(theirSet)) {
                    if (Defer(p, theirSet, now)) { InterlockedIncrement(&deferred); return; }
                }
            }
        }
    }
    if (companion && !scenechain::First(p.hash)) return;
    if (companion && scenechain::promptBranches && scene_catalog::IsPromptBranch(scenechain::root, p.hash))
        gate::Event("coop_mirror prompt branch received root=%08X cue=%08X owner=%08X attempt=%u",
                    scenechain::root, p.hash, p.token, p.reserved[6]);
    if (companion && scenechain::ChurchTransition(p.hash))
        gate::Event("coop_mirror church handoff cue=%08X owner=%08X attempt=%u", p.hash, p.token, p.reserved[6]);
    if (committed) scenechain::Start(p.hash, p.token, p.reserved[6], now, p.hash == scenechain::kChurch && (p.reserved[7] & agreement::kChurchHandoff),
                                     scene_catalog::IsPromptRoot(p.hash) && (p.reserved[7] & agreement::kCampaignSceneUpdate));
    unsigned char raw[12]{};
    std::memcpy(raw, &p.hash, 4);   // raw[4], the in-flight marker, stays 0
    const bool sequenceBefore = cutscene::SequenceRunningNow();
    // A MultiManager this triggers sends only its safe targets here (flag 8 managers send later, so they are armed).
    if (relayMultiManagers && sendMessagesHook) {
        const unsigned author = SlotForToken(p.token, 0xFFu, now);
        replayFromHost = author < coop_peers::kMaxPeers && peerTable.peers[author].machine == 0;
        ArmManagersOf(p.hash, now + 3000, replayFromHost);
    }
    if (objonce::Mark(p.hash)) {
        objonce::NoteReplayed(p.hash);
        // a late joiner's history carries what the round did, whoever did it
        if (IsHost() && !fromHistory) JournalReplayed(p.hash, p.flags & 0x3FFFFF00u);
    }
    ++gate::mirroring;
    const unsigned mirroringBefore = gate::mirroringFrom;
    gate::mirroringFrom = SlotForToken(p.token, 0xFFu, now);
    spawnwin::Note(p.hash);   // the gate never sees a replay: the spawn points follow the partner's objectives too
    NoteForRandomizer(p.hash);   // nor does the randomizer's detour in front of it
    __try {
        gate::dispatchHook.call<bool>(raw, static_cast<int>((p.flags & ~kFlagDeferred) >> 8));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RememberFault(p.hash);
        gate::Event("coop_mirror replay faulted hash=%08X (not again in this world)", p.hash);
    }
    gate::mirroringFrom = mirroringBefore;
    --gate::mirroring;
    if (p.reserved[5] == kEnemySpawnMark) TraceStalkerRequest(p.hash, "after dispatch");
    replayFromHost = true;
    if (!Faulted(p.hash)) story_objects::Opening(p.hash);
    if (storyCue && !Faulted(p.hash)) storyCues.Note(p.hash, storyOrigin);
    // Only an outcome that really ran here stops a later local one (a folded,
    // unregistered or faulted replay changed nothing).
    if (partnerSource && !Faulted(p.hash)) {
        NoteSourceDone(p.hash, p.reserved);
        InterlockedIncrement(&sourceReplayed);
        if (InterlockedIncrement(&sourceLines) <= 200)
            gate::Event("coop_mirror a partner's result %08X (%s) from %08X-%08X replayed", p.hash, coop_names::Name(p.hash), p.reserved[0], p.reserved[1]);
    }
    if (partnerOutcome && !Faulted(p.hash)) {
        if (!OutcomePlayedHere(p.hash) && outcomesPlayedCount < kMaxOutcomes) outcomesPlayed[outcomesPlayedCount++] = p.hash;
        InterlockedIncrement(&outcomesReplayed);
        gate::Event("coop_mirror action outcome %08X (%s) of a partner replayed: our world moves on with his", p.hash, coop_names::Name(p.hash));
    }
    if (!fromHistory && !sequenceBefore && cutscene::SequenceRunningNow()) cutscene::NoteReplay(SlotForToken(p.token, 0xFFu, now), true, false, now);
    // Not noted as "recent": a replay is the host's doing, and noting it
    // folded the host's NEXT death message of the same name (kills inside
    // five seconds) as if this machine had fired it itself.
    InterlockedIncrement(fromHistory ? &journalReplayed : &replayed);
    if (gate::SceneTraceOn()) gate::SceneTrace("coop_scene t=%lu replay %08X %s age=%u ms%s", static_cast<unsigned long>(now), p.hash,
                                               coop_names::Name(p.hash), static_cast<unsigned>(now - p.tick), fromHistory ? " (history)" : "");
    if (trace && !fromHistory) gate::Event("coop_mirror replayed hash=%08X name=%s age=%u ms", p.hash, coop_names::Name(p.hash), static_cast<unsigned>(now - p.tick));
}
// The history entry: the same message, `count` times over.
void ReplayHistory(const Packet& p, DWORD now) {
    unsigned count = p.reserved[4];
    if (count > kJournalRepeatCap) count = kJournalRepeatCap;
    for (unsigned i = 0; i < count; ++i) Replay(p, now);
    if (trace) gate::Event("coop_mirror history hash=%08X x%u", p.hash, count);
}

void SendToSlot(unsigned slot, const Packet& p) {
    const auto& peer = peerTable.peers[slot];
    if (!peer.Addressable()) return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = peer.ip;
    to.sin_port = peer.port;
    if (::sendto(dataSock, reinterpret_cast<const char*>(&p), sizeof(p), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == sizeof(p))
        traffic::Note(&p, sizeof(p), 1);
}

// Deliver a direct send to the local copy of the object, the way the host did.
bool ReplayDirect(const Packet& p) {
    if (Faulted(p.hash, p.reserved[0], p.reserved[1])) { InterlockedIncrement(&faultedSkips); return false; }
    const uint32_t object = ObjectByGuid(p.reserved);
    if (!object) return false;
    unsigned char raw[kMessageCopy]{};
    std::memcpy(raw, &p.hash, 4);   // raw[4], the in-flight marker, stays 0
    if (p.flags & kFlagJournal) std::memcpy(raw + 12, &p.reserved[5], 20);
    else std::memcpy(raw + 12, &p.reserved[4], kPayload);
    bool ok = false;
    const bool sequenceBefore = cutscene::SequenceRunningNow();
    ++gate::mirroring;
    spawnwin::Note(p.hash);
    __try {
        ghosttrigger::sendHook.ccall<int>(raw, reinterpret_cast<void*>(object), 0);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RememberFault(p.hash, p.reserved[0], p.reserved[1]);
        gate::Event("coop_mirror direct replay faulted hash=%08X on %08X-%08X (object %08X): not again for this object in this world", p.hash,
                    p.reserved[0], p.reserved[1], object);
    }
    --gate::mirroring;
    if (!(p.flags & kFlagJournal)) {
        const DWORD now = GetTickCount();
        cutscene::NoteReplay(SlotForToken(p.token, 0xFFu, now), !sequenceBefore && cutscene::SequenceRunningNow(), true, now);
    }
    return ok;
}

// Game thread, from Poll.
void Tick(DWORD now) {
    if (!installed || !enabled) return;
    SplitTick(now);
    if (IsHost()) {
        // A new checkpoint starts a new history.
        const LONG sets = InterlockedCompareExchange(&checkpoint::hostSets, 0, 0);
        if (sets != journalCheckpoint) { journalCheckpoint = sets; JournalReset(); }
        // Deliveries: a page of the journal per tick, per asking peer, each
        // entry twice for safety (the receiver folds the repeat by sequence).
        std::lock_guard<std::mutex> guard(lock);
        for (auto& d : deliveries) {
            if (!d.active) continue;
            for (unsigned n = 0; n < 8 && d.next < journalCount; ++n, ++d.next) {
                Packet p{kMagic, kVersion, peerReadiness.token, ++sequence, journal[d.next].hash, journal[d.next].flags | kFlagJournal, now, {}};
                if (journal[d.next].flags & (kFlagDirect | kFlagCall)) {
                    std::memcpy(p.reserved, journal[d.next].guid, 16);
                    std::memcpy(&p.reserved[5], journal[d.next].payload, 20);   // journal: [4] is the count, [5..9] the payload
                }
                p.reserved[4] = journal[d.next].count;
                SendToSlot(d.slot, p);
                SendToSlot(d.slot, p);
                InterlockedIncrement(&journalSent);
            }
            if (d.next >= journalCount) d.active = false;
        }
    } else if (inLevel && LocalSlotZeroEntity() && level::havePacket) {
        // Ask for the history once this level is the host's level, once per
        // host level epoch and per local level.
        char map[64]{}, stream[64]{};
        if (JournalOn() && level::CurrentMap(map, sizeof(map), stream, sizeof(stream)) && _stricmp(map, level::wanted.map) == 0 &&
            (journalAskedEpoch != level::wanted.epoch || _stricmp(journalAskedMap, map) != 0)) {
            journalAskedEpoch = level::wanted.epoch;
            std::snprintf(journalAskedMap, sizeof(journalAskedMap), "%s", map);
            historyCount = 0;
            {
                // Through the outbox, so it repeats: one lost request was a
                // whole level without the group's history.
                std::lock_guard<std::mutex> guard(lock);
                Out& o = outbox[outHead];
                outHead = (outHead + 1) % kOutbox;
                o.packet = Packet{kMagic, kVersion, peerReadiness.token, ++sequence, 1, kFlagJournalRequest, now, {}};
                o.at = now - kRepeatMs;
                o.sent = 0;
            }
            gate::Event("coop_mirror history requested for %s epoch=%u", map, level::wanted.epoch);
        }
    }
    if (peerTable.Count()) {
        std::lock_guard<std::mutex> guard(lock);
        scene_delivery::Tick(now);
        for (auto& o : outbox) {
            if (!o.packet.hash || o.sent >= kRepeats) continue;
            // Queued while nobody was there: old news by the time a peer appears.
            if (now - o.packet.tick > kOutboxMaxAgeMs) { o.packet.hash = 0; InterlockedIncrement(&outboxExpired); continue; }
            if (now - o.at < kRepeatMs) continue;
            o.at = now;
            ++o.sent;
            if (SendToPeers(&o.packet, sizeof(o.packet))) InterlockedIncrement(&sent);
            if (o.sent >= kRepeats) o.packet.hash = 0;
        }
    }
    Packet batch[kInbox];
    unsigned count = 0;
    {
        std::lock_guard<std::mutex> guard(lock);
        count = inCount;
        std::memcpy(batch, inbox, sizeof(Packet) * count);
        inCount = 0;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (batch[i].flags & kFlagPassed) { gate::Preapprove(batch[i].hash); continue; }
        if (batch[i].flags & kFlagJournal) { RememberHistory(batch[i].hash); ReplayHistory(batch[i], now); }
        else Replay(batch[i], now);
    }
    ScanTick(now);
    DeferredTick(now);
    SpawnWaitTick(now);
    PropWaitTick(now);
    NeutralWaitTick(now);
    NpcCueTick(now);
    MmLiftTick(now);
    FollowerCueTick(now);
    WatchCueTick(now);
    ReplayReadySteps(now);
    scenechain::Tick(now);
    agreement::Tick(now);
    SequenceTick(now);
    ghostseq::Tick(now);
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Mirror", "Enabled", 1, ini.c_str()) != 0;
    journalMode = GetPrivateProfileIntA("Mirror", "Journal", 2, ini.c_str());
    std::fprintf(log, "coop_mirror journal: %s ([Mirror] Journal=%d)\n",
                 journalMode == 0 ? "off" : journalMode == 1 ? "story and match modes" : "match modes only; story partners load in together at the host's checkpoint restart",
                 journalMode);
    trace = GetPrivateProfileIntA("Mirror", "Trace", 1, ini.c_str()) != 0;
    dedupeMs = static_cast<DWORD>(GetPrivateProfileIntA("Mirror", "DedupeMs", 5000, ini.c_str()));
    storyOnce = GetPrivateProfileIntA("Mirror", "StoryCuesOnce", 1, ini.c_str()) != 0;
    splitDropped = GetPrivateProfileIntA("Mirror", "SplitDroppedTargets", 1, ini.c_str()) != 0;
    clientsMirror = GetPrivateProfileIntA("Mirror", "ClientsMirror", 0, ini.c_str()) != 0;
    gapTrace = GetPrivateProfileIntA("Mirror", "GapTrace", 0, ini.c_str()) != 0;
    spawnerMessages = GetPrivateProfileIntA("Mirror", "SpawnerMessages", 0, ini.c_str()) != 0;
    promptsShared = GetPrivateProfileIntA("Mirror", "PromptResultsFromLeader", 0, ini.c_str()) != 0;
    gate::personalActions = GetPrivateProfileIntA("Mirror", "PersonalActions", 0, ini.c_str()) != 0;
    actionOutcomes = GetPrivateProfileIntA("Mirror", "ActionOutcomes", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop_mirror action_outcomes=%d (eye poke, hacking and power node unlock results)\n", actionOutcomes ? 1 : 0);
    std::fprintf(log, "coop_mirror personal_actions=%d (a sequence right after our own switch/panel use pulls nobody)\n", gate::personalActions ? 1 : 0);
    propSpawns = GetPrivateProfileIntA("Mirror", "SceneProps", 0, ini.c_str()) != 0;
    neutralSpawns = GetPrivateProfileIntA("Mirror", "NeutralNPCSpawns", 0, ini.c_str()) != 0;
    enemyMessages = GetPrivateProfileIntA("Mirror", "EnemyMessages", 0, ini.c_str()) != 0;
    if (enemyMessages) BuildEnemyMessages();
    std::fprintf(log, "coop_mirror enemy_messages=%d known=%u (death/health/spawn-done/aware messages of the host's enemies go to every client)\n",
                 enemyMessages ? 1 : 0, static_cast<unsigned>(enemyMessageHashes.size()));
    std::fprintf(log, "coop_mirror neutral_npc_spawns=%d (a NeutralNPC spawn request is played on every machine; for NeutralNPCsLocal=1)\n", neutralSpawns ? 1 : 0);
    std::fprintf(log, "coop_mirror scene_props=%d (single-prop spawn requests are shared and replayed in a followed scene)\n", propSpawns ? 1 : 0);
    std::fprintf(log, "coop_mirror prompt_results_from_leader=%d (a follower drops his own prompt results; the leader's are replayed)\n", promptsShared ? 1 : 0);
    std::fprintf(log, "coop_mirror spawner_messages=%d (the host repeats what its FuncSpawns send; a client's are suppressed with its spawns)\n", spawnerMessages ? 1 : 0);
    seqwait::enabled = GetPrivateProfileIntA("Mirror", "SequenceWaitsForStreams", 1, ini.c_str()) != 0;
    seqwait::waitMs = static_cast<DWORD>(GetPrivateProfileIntA("Mirror", "SequenceWaitMs", 90000, ini.c_str()));
    ghostseq::holdMs = static_cast<DWORD>(GetPrivateProfileIntA("Mirror", "GhostSequenceHoldMs", 20000, ini.c_str()));
    std::fprintf(log, "coop_mirror ghost_sequence_hold_ms=%lu (a scene a partner's ghost starts here waits for his own copy)\n", static_cast<unsigned long>(ghostseq::holdMs));
    seqwait::freezeEnabled = GetPrivateProfileIntA("Mirror", "SequenceWaitFreeze", 1, ini.c_str()) != 0;
    followSequence = GetPrivateProfileIntA("Mirror", "FollowSequence", 1, ini.c_str()) != 0;
    followWaitMs = static_cast<DWORD>(GetPrivateProfileIntA("Mirror", "FollowWaitMs", 10000, ini.c_str()));
    std::fprintf(log, "coop_mirror sequence_waits_for_streams=%d wait_ms=%lu pinned_scene_set=1 freeze=%d follow_sequence=%d follow_wait_ms=%lu\n",
                 seqwait::enabled ? 1 : 0, static_cast<unsigned long>(seqwait::waitMs), seqwait::freezeEnabled ? 1 : 0,
                 followSequence ? 1 : 0, static_cast<unsigned long>(followWaitMs));
    coop_lists::Read(log, ini, "Mirror", "Include", include);
    coop_lists::Read(log, ini, "Mirror", "ExcludeHashes", excludeHashes);
    followerCueCatchUp = GetPrivateProfileIntA("Mirror", "FollowerCueCatchUp", 0, ini.c_str()) != 0;
    {
        char list[256]{};
        char always[160]{};
        GetPrivateProfileStringA("Mirror", "AlwaysLocalNames", "_mm_save_kiosk", always, sizeof(always), ini.c_str());
        char* rest = nullptr;
        for (char* token = strtok_s(always, ",; ", &rest); token && alwaysLocalCount < std::size(alwaysLocal); token = strtok_s(nullptr, ",; ", &rest)) {
            for (char* c = token; *c; ++c) if (*c >= 'A' && *c <= 'Z') *c = static_cast<char>(*c + 32);
            std::snprintf(alwaysLocal[alwaysLocalCount++], sizeof(alwaysLocal[0]), "%s", token);
        }
        std::fprintf(log, "coop_mirror always_local_names=%u%s", alwaysLocalCount, alwaysLocalCount ? ":" : "");
        for (unsigned i = 0; i < alwaysLocalCount; ++i) std::fprintf(log, " %s", alwaysLocal[i]);
        std::fprintf(log, " (never sent, never played from a partner)\n");
        GetPrivateProfileStringA("Mirror", "LocalNamePatterns", "", list, sizeof(list), ini.c_str());
        char* next = nullptr;
        for (char* token = strtok_s(list, ",; ", &next); token && localPatternCount < kLocalPatterns; token = strtok_s(nullptr, ",; ", &next)) {
            for (char* c = token; *c; ++c) if (*c >= 'A' && *c <= 'Z') *c = static_cast<char>(*c + 32);
            std::snprintf(localPatterns[localPatternCount++], sizeof(localPatterns[0]), "%s", token);
        }
        std::fprintf(log, "coop_mirror local_name_patterns=%u%s", localPatternCount, localPatternCount ? ":" : "");
        for (unsigned i = 0; i < localPatternCount; ++i) std::fprintf(log, " %s", localPatterns[i]);
        std::fprintf(log, " (messages whose name contains one stay on the machine that fires them)\n");
        char scope[256]{};
        GetPrivateProfileStringA("Mirror", "LocalNameScope", "", scope, sizeof(scope), ini.c_str());
        next = nullptr;
        for (char* token = strtok_s(scope, ",; ", &next); token && scopePatternCount < kLocalPatterns; token = strtok_s(nullptr, ",; ", &next)) {
            for (char* c = token; *c; ++c) if (*c >= 'A' && *c <= 'Z') *c = static_cast<char>(*c + 32);
            std::snprintf(scopePatterns[scopePatternCount++], sizeof(scopePatterns[0]), "%s", token);
        }
        std::fprintf(log, "coop_mirror local_name_scope=%u%s", scopePatternCount, scopePatternCount ? ":" : " (always active)");
        for (unsigned i = 0; i < scopePatternCount; ++i) std::fprintf(log, " %s", scopePatterns[i]);
        std::fprintf(log, "\n");
    }
    {
        char list[512]{};
        GetPrivateProfileStringA("Mirror", "DirectExclude", "B2385537,368C9BF3,BF2F11C2", list, sizeof(list), ini.c_str());
        char* next = nullptr;
        for (char* token = strtok_s(list, ",; ", &next); token && directExcludeCount < kMaxDirectExclude; token = strtok_s(nullptr, ",; ", &next))
            directExclude[directExcludeCount++] = static_cast<uint32_t>(strtoul(token, nullptr, 16));
        // iMsgDamage carries a handle into this machine's damage table at +8: on another machine it
        // names nothing (13.10.: 20 mirrored onto Marker Lab's props, each a no-op at best)
        if (!DirectExcluded(0x703C37E7u) && directExcludeCount < kMaxDirectExclude) directExclude[directExcludeCount++] = 0x703C37E7u;
        std::fprintf(log, "coop_mirror direct_exclude:");
        for (unsigned i = 0; i < directExcludeCount; ++i) std::fprintf(log, " %08X", directExclude[i]);
        std::fprintf(log, "\n");
    }
    if (!enabled) { std::fprintf(log, "coop_mirror off\n"); return false; }
    if (!gate::installed) { std::fprintf(log, "coop_mirror needs the gate's dispatch hook; off\n"); return false; }
    gate::ReadRuleSet(log, ini, worldRules, "Mirror", "Classes",
                      "StandardDoor,SimpleOneTimeDoor,Mover,MoverController,DamageableProp,ScriptableProp,"
                      "DismemberableProp,DrivenScriptableProp,LinearScriptableProp,SecondaryAnimScriptableProp,VentMarker",
                      "Nodes", "");
    // Lights stay client side even when an older INI still lists them.
    if (GetPrivateProfileIntA("Mirror", "MirrorLights", 0, ini.c_str()) == 0) {
        unsigned kept = 0, lights = 0;
        for (unsigned i = 0; i < worldRules.nodeCount; ++i) {
            const char* name = worldRules.nodeNames[i];
            if (name && (_stricmp(name, "AnimateLight") == 0 || _stricmp(name, "MoveLight") == 0)) { ++lights; continue; }
            worldRules.nodeVtables[kept] = worldRules.nodeVtables[i];
            worldRules.nodeNames[kept++] = name;
        }
        worldRules.nodeCount = kept;
        std::fprintf(log, "coop_mirror lights stay client side (MirrorLights=0): %u light node class(es) removed from the world rules\n", lights);
    }
    gate::ReadRuleSet(log, ini, hostRules, "Mirror", "HostClasses", "",
                      "HostNodes", "VariableOperator,VariableWatcher,ObjectiveEntry,LogEntry");
    gate::ReadRuleSet(log, ini, anyRules, "Mirror", "AnyClasses", "ScriptedSequence,ScriptedEventSequence", "AnyNodes", "");
    gate::ReadRuleSet(log, ini, voiceRules, "Mirror", "VoiceClasses", "ScriptedSentence,PlayAudioStream", "VoiceNodes", "");
    gate::ReadRuleSet(log, ini, excludeRules, "Mirror", "Exclude", "",
                      "ExcludeNodes", "FuncSpawn,StreamSet,StreamScripter,Checkpoint,LevelChange,EquipCommand,PlayerHelmetCommand,"
                      "OrEventGate,AndEventGate,LuaSwitch,ExecuteLuaScript,MultiManager,MultiRemoveTarget");
    sourceShare = GetPrivateProfileIntA("Mirror", "SourceShare", 0, ini.c_str()) != 0;
    if (sourceShare) {
        gate::ReadRuleSet(log, ini, sourceRules, "Mirror", "SourceClasses",
                          "TKReceptacle,TKReceptacleTarget,SolarPuzzle,LinearHandle,Switch,SimplePressAndHoldSwitch,PowerNodeSwitch,Destructible,DamageableProp",
                          "SourceNodes", "");
        gate::ReadRuleSet(log, ini, sourceUnsafeRules, "Mirror", "SourceUnsafeClasses",
                          "PushCamera,PopCamera,SimpleTeleport,PlayerChangeState,FadeScreenFx,TestInputEvent,TentacleControl,PlayerAir",
                          "SourceUnsafeNodes", "StreamSet,StreamScripter,MultiRemoveTarget,Checkpoint,LevelChange,EquipCommand,PlayerHelmetCommand,ExecuteLuaScript,LuaSwitch");
    }
    std::fprintf(log, "coop_mirror source_share=%d (the result of an action on a receptacle, puzzle, switch or destructible is shared by its sender)\n",
                 sourceShare ? 1 : 0);
    relayMultiManagers = GetPrivateProfileIntA("Mirror", "MultiManagers", 0, ini.c_str()) != 0;
    enemySpawnsFromClients = GetPrivateProfileIntA("Mirror", "EnemySpawnsFromClients", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop_mirror enemy_spawns_from_clients=%d (an enemy spawn a client's world asks for is played on the host)\n",
                 enemySpawnsFromClients ? 1 : 0);
    // Camera, teleport, fade, prompts and player state belong to whoever the chain is about.
    gate::ReadRuleSet(log, ini, mmDropRules, "Mirror", "MultiManagerDropClasses",
                      "PushCamera,PopCamera,SimpleTeleport,PlayerChangeState,FadeScreenFx,TestInputEvent,TentacleControl,PlayerAir",
                      "MultiManagerDropNodes", "StreamSet,StreamScripter,MultiRemoveTarget,Checkpoint,LevelChange,FuncSpawn,"
                      "VariableOperator,VariableWatcher,EquipCommand,PlayerHelmetCommand,ExecuteLuaScript,LuaSwitch");
    if (relayMultiManagers) {
        // sub esp,0x10 / push esi / mov esi,ecx / mov eax,[esi+0x60] / test eax,eax
        static const unsigned char kSend[] = {0x83, 0xEC, 0x10, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x60, 0x85, 0xC0};
        if (!hook_site::Install(log, "coop_mirror multimanager send", base + kSendMessagesRva, kSend, sizeof(kSend),
                                reinterpret_cast<void*>(OnSendMessages), sendMessagesHook)) {
            relayMultiManagers = false;
        }
    }
    std::fprintf(log, "coop_mirror multimanagers=%d (relayed; a partner's copy sends only the targets that are neither dangerous nor mirrored anyway)\n",
                 relayMultiManagers ? 1 : 0);
    gate::g_mirrorHook = OnLocalDispatch;
    gate::g_passedHook = OnPassed;
    installed = true;
    std::fprintf(log, "coop_mirror role=%s dedupe_ms=%u repeats=%u world=%u host_only=%u exclude=%u story_cues_once=%d\n",
                 IsHost() ? "host (repeats everything, relays)" : "client (repeats the world, replays)",
                 static_cast<unsigned>(dedupeMs), kRepeats, worldRules.Size(), hostRules.Size(), excludeRules.Size(), storyOnce ? 1 : 0);
    return true;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG activity = InterlockedCompareExchange(&mirrored, 0, 0) + InterlockedCompareExchange(&received, 0, 0) +
                          InterlockedCompareExchange(&directMirrored, 0, 0) + InterlockedCompareExchange(&seqwait::taken, 0, 0) +
                          InterlockedCompareExchange(&deferred, 0, 0);
    if (activity == last) return;
    last = activity;
    std::fprintf(log, "coop_mirror scene confirmations requests=%ld replies=%ld released=%ld missing_guid=%ld mismatches=%ld\n",
                 ghostseq::requests, ghostseq::replies, ghostseq::confirmedReleases, ghostseq::unknownStarts, ghostseq::startMismatches);
    std::fprintf(log, "coop_mirror ghost_sequences held=%ld partner_copies=%ld timed_out=%ld near_skipped=%ld full=%ld waiting=%u\n", InterlockedCompareExchange(&ghostseq::held, 0, 0),
                 InterlockedCompareExchange(&ghostseq::partnerCopies, 0, 0), InterlockedCompareExchange(&ghostseq::timedOut, 0, 0),
                 InterlockedCompareExchange(&ghostseq::nearSkipped, 0, 0), InterlockedCompareExchange(&ghostseq::full, 0, 0), ghostseq::count);
    std::fprintf(log, "coop_mirror sequences waiting=%u taken=%ld released=%ld freezes=%ld timed_out=%ld deferred=%ld deferred_played=%ld deferred_late=%ld\n",
                 seqwait::count, InterlockedCompareExchange(&seqwait::taken, 0, 0), InterlockedCompareExchange(&seqwait::released, 0, 0),
                 InterlockedCompareExchange(&seqwait::freezes, 0, 0),
                 InterlockedCompareExchange(&seqwait::timedOut, 0, 0), InterlockedCompareExchange(&deferred, 0, 0),
                 InterlockedCompareExchange(&deferredPlayed, 0, 0), InterlockedCompareExchange(&deferredLate, 0, 0));
    if (gate::personalActions) std::fprintf(log, "coop_mirror personal_actions kept=%ld\n", InterlockedCompareExchange(&personalKept, 0, 0));
    std::fprintf(log, "coop_mirror necro_scenes skipped=%ld watch_cues queued=%ld played=%ld dropped=%ld waiting=%u watcher_sends_dropped=%ld\n",
                 InterlockedCompareExchange(&necroSceneSkips, 0, 0), InterlockedCompareExchange(&watchCuesQueued, 0, 0),
                 InterlockedCompareExchange(&watchCuesPlayed, 0, 0), InterlockedCompareExchange(&watchCuesDropped, 0, 0), watchCueCount,
                 InterlockedCompareExchange(&watcherSendsDropped, 0, 0));
    if (sourceShare)
        std::fprintf(log, "coop_mirror source_share sent=%ld replayed=%ld dropped_second=%ld stayed_local_unsafe=%ld classes_cached=%u\n",
                     InterlockedCompareExchange(&sourceSent, 0, 0), InterlockedCompareExchange(&sourceReplayed, 0, 0),
                     InterlockedCompareExchange(&sourceDropped, 0, 0), InterlockedCompareExchange(&sourceUnsafe, 0, 0), sourceCacheCount);
    if (actionOutcomes) std::fprintf(log, "coop_mirror action_outcomes names=%u sent=%ld replayed=%ld dropped_second=%ld\n", outcomeCount,
                     InterlockedCompareExchange(&outcomesSent, 0, 0), InterlockedCompareExchange(&outcomesReplayed, 0, 0),
                     InterlockedCompareExchange(&outcomesKept, 0, 0));
    if (relayMultiManagers)
        std::fprintf(log, "coop_mirror multimanagers relayed=%ld unsafe_triggers=%ld armed=%ld filtered_sends=%ld targets_kept=%ld targets_dropped=%ld all_dropped=%ld arms_late=%ld arms_evicted=%ld scene_cameras_kept=%ld\n",
                     InterlockedCompareExchange(&mmTriggersRelayed, 0, 0), InterlockedCompareExchange(&mmUnsafeTriggers, 0, 0),
                     InterlockedCompareExchange(&mmArmed, 0, 0), InterlockedCompareExchange(&mmFiltered, 0, 0),
                     InterlockedCompareExchange(&mmTargetsKept, 0, 0), InterlockedCompareExchange(&mmTargetsDropped, 0, 0),
                     InterlockedCompareExchange(&mmAllDropped, 0, 0), InterlockedCompareExchange(&mmArmsLate, 0, 0),
                     InterlockedCompareExchange(&mmArmsEvicted, 0, 0), InterlockedCompareExchange(&mmSceneCameras, 0, 0));
    if (relayMultiManagers && splitDropped)
        std::fprintf(log, "coop_mirror split_targets delivered=%ld managers=%ld other_listeners_left=%ld no_manager=%ld queued=%ld queue_full=%ld faults=%ld waiting=%u\n",
                     InterlockedCompareExchange(&splitDelivered, 0, 0), InterlockedCompareExchange(&splitManagers, 0, 0),
                     InterlockedCompareExchange(&splitOther, 0, 0), InterlockedCompareExchange(&splitNone, 0, 0),
                     InterlockedCompareExchange(&splitQueued, 0, 0), InterlockedCompareExchange(&splitFull, 0, 0),
                     InterlockedCompareExchange(&splitFaults, 0, 0), splitCount);
    std::fprintf(log, "coop_mirror door_shut_culls managers_armed=%ld cull_targets_dropped=%ld (cull sets of a door output for the other side than our Isaac)\n",
                 InterlockedCompareExchange(&mmCullArms, 0, 0), InterlockedCompareExchange(&mmCullsDropped, 0, 0));
    if (enemyMessages)
        std::fprintf(log, "coop_mirror enemy_messages sent=%ld\n", InterlockedCompareExchange(&enemyMessagesSent, 0, 0));
    if (enemySpawnsFromClients)
        std::fprintf(log, "coop_mirror enemy_spawns_from_clients sent=%ld played=%ld own_folded=%ld\n", InterlockedCompareExchange(&enemyRequestsSent, 0, 0),
                     InterlockedCompareExchange(&enemyRequestsPlayed, 0, 0), InterlockedCompareExchange(&enemyRequestsFolded, 0, 0));
    std::fprintf(log, "coop_lift managers held=%ld released=%ld timed_out=%ld dropped=%ld cabin_faults=%ld\n", mmLiftHeld, mmLiftReleased, mmLiftTimedOut,
                 mmLiftDropped, ghosttrigger::cabinFaults);
    std::fprintf(log, "coop_mirror wire outbox_expired=%ld stale_sequences=%ld entity_scans spawn=%ld prompts=%ld restarts=%ld\n",
                 InterlockedCompareExchange(&outboxExpired, 0, 0), InterlockedCompareExchange(&staleSequences, 0, 0),
                 InterlockedCompareExchange(&spawnDoneScans, 0, 0), InterlockedCompareExchange(&promptScans, 0, 0),
                 InterlockedCompareExchange(&scanRestarts, 0, 0));
    if (neutralSpawns || propSpawns) {
        std::fprintf(log, "coop_mirror follower_cues sent=%ld folded=%ld played=%ld\n", InterlockedCompareExchange(&followerCuesSent, 0, 0),
                     InterlockedCompareExchange(&followerCuesFolded, 0, 0), InterlockedCompareExchange(&followerCuesPlayed, 0, 0));
        std::fprintf(log, "coop_mirror neutral_spawns sent=%ld waited=%ld played=%ld expired=%ld unshared_near_scene=%ld own_dropped=%ld partner_dropped=%ld\n",
                     InterlockedCompareExchange(&neutralSent, 0, 0), InterlockedCompareExchange(&neutralWaited, 0, 0),
                     InterlockedCompareExchange(&neutralPlayed, 0, 0), InterlockedCompareExchange(&neutralExpired, 0, 0),
                     InterlockedCompareExchange(&spawnRequestsUnshared, 0, 0), InterlockedCompareExchange(&neutralOwnDropped, 0, 0),
                     InterlockedCompareExchange(&neutralPartnerDropped, 0, 0));
        std::fprintf(log, "coop_mirror neutral_once known=%u repeats=%ld full=%ld\n", neutralBook.used,
                     InterlockedCompareExchange(&neutralBook.repeats, 0, 0), InterlockedCompareExchange(&neutralBook.full, 0, 0));
        std::fprintf(log, "coop_mirror npc_cues held=%ld folded=%ld played=%ld late=%ld waiting=%u queued_behind=%ld\n", InterlockedCompareExchange(&npcCuesHeld, 0, 0),
                     InterlockedCompareExchange(&npcCuesFolded, 0, 0), InterlockedCompareExchange(&npcCuesPlayed, 0, 0),
                     InterlockedCompareExchange(&npcCuesLate, 0, 0), npcCueCount, InterlockedCompareExchange(&npcCuesQueuedBehind, 0, 0));
    }
    if (storyOnce)
        std::fprintf(log, "coop_mirror story_once known=%u ours_dropped=%ld partners_dropped=%ld repeats=%ld full=%ld\n", storyCues.used,
                     InterlockedCompareExchange(&storyCues.swallowedLocal, 0, 0), InterlockedCompareExchange(&storyCues.droppedPartner, 0, 0),
                     InterlockedCompareExchange(&storyCues.repeats, 0, 0), InterlockedCompareExchange(&storyCues.full, 0, 0));
    if (propSpawns)
        std::fprintf(log, "coop_mirror scene_props requests=%u classified=%ld unknown=%ld sent=%ld waited=%ld played=%ld expired=%ld\n",
                     propRequestCount, InterlockedCompareExchange(&propClassified, 0, 0), InterlockedCompareExchange(&propUnknown, 0, 0),
                     InterlockedCompareExchange(&propSent, 0, 0), InterlockedCompareExchange(&propWaited, 0, 0),
                     InterlockedCompareExchange(&propPlayed, 0, 0), InterlockedCompareExchange(&propExpired, 0, 0));
    if (promptsShared)
        std::fprintf(log, "coop_mirror prompts names=%u dropped=%ld sent=%ld replayed=%ld refused=%ld\n", promptNameCount,
                     InterlockedCompareExchange(&promptDropped, 0, 0), InterlockedCompareExchange(&promptSent, 0, 0),
                     InterlockedCompareExchange(&promptReplayed, 0, 0), InterlockedCompareExchange(&promptRefused, 0, 0));
    if (spawnerMessages) std::fprintf(log, "coop_mirror spawner_messages spawn_done_names=%u with_guid=%ld waited=%ld found=%ld late=%ld scans=%ld sent=%ld\n", spawnDoneCount,
                                      InterlockedCompareExchange(&spawnGuidSent, 0, 0), InterlockedCompareExchange(&spawnWaited, 0, 0),
                                      InterlockedCompareExchange(&spawnWaitFound, 0, 0), InterlockedCompareExchange(&spawnWaitLate, 0, 0),
                                      InterlockedCompareExchange(&spawnDoneScans, 0, 0), InterlockedCompareExchange(&spawnerSent, 0, 0));
    if (journalMode == 2) std::fprintf(log, "coop_mirror journal story_notes_skipped=%ld (match modes only)\n", InterlockedCompareExchange(&journalSkippedStory, 0, 0));
    std::fprintf(log, "coop_mirror offered=%ld mirrored=%ld throttled=%ld excluded=%ld sent=%ld received=%ld relayed=%ld duplicate=%ld folded=%ld replayed=%ld not_in_level=%ld unregistered=%ld dropped=%ld "
                      "journal=%u resets=%ld requests=%ld history_sent=%ld history_replayed=%ld held_back=%ld "
                      "direct_offered=%ld direct_mirrored=%ld direct_replayed=%ld direct_not_found=%ld faulted_skips=%ld "
                      "calls_mirrored=%ld calls_replayed=%ld calls_not_found=%ld direct_excluded=%ld direct_scene_dropped=%ld randomizer_notes=%ld\n",
                 InterlockedCompareExchange(&offered, 0, 0), InterlockedCompareExchange(&mirrored, 0, 0),
                 InterlockedCompareExchange(&throttled, 0, 0), InterlockedCompareExchange(&excluded, 0, 0),
                 InterlockedCompareExchange(&sent, 0, 0), InterlockedCompareExchange(&received, 0, 0),
                 InterlockedCompareExchange(&relayed, 0, 0), InterlockedCompareExchange(&duplicate, 0, 0),
                 InterlockedCompareExchange(&folded, 0, 0), InterlockedCompareExchange(&replayed, 0, 0),
                 InterlockedCompareExchange(&notInLevel, 0, 0), InterlockedCompareExchange(&unregistered, 0, 0),
                 InterlockedCompareExchange(&dropped, 0, 0), journalCount, InterlockedCompareExchange(&journalResets, 0, 0),
                 InterlockedCompareExchange(&journalRequests, 0, 0), InterlockedCompareExchange(&journalSent, 0, 0),
                 InterlockedCompareExchange(&journalReplayed, 0, 0), InterlockedCompareExchange(&heldBack, 0, 0),
                 InterlockedCompareExchange(&directOffered, 0, 0), InterlockedCompareExchange(&directMirrored, 0, 0),
                 InterlockedCompareExchange(&directReplayed, 0, 0), InterlockedCompareExchange(&directNotFound, 0, 0),
                 InterlockedCompareExchange(&faultedSkips, 0, 0), InterlockedCompareExchange(&callsMirrored, 0, 0),
                 InterlockedCompareExchange(&callsReplayed, 0, 0), InterlockedCompareExchange(&callsNotFound, 0, 0),
                 InterlockedCompareExchange(&directExcluded, 0, 0), InterlockedCompareExchange(&directSceneDropped, 0, 0),
                 InterlockedCompareExchange(&randomizerNotes, 0, 0));
}
}  // namespace mirror
