// Dead Space 2 United by Richard Hafer.
// Checkpoint follow: the clients restart where the host restarts.
//
// Observed 2026-09-17: a client saved at a kiosk, the host had a plain game
// checkpoint. The death mirror worked (both died together), and then each
// came back somewhere else.
//
// Every change of the death restart point goes through one function,
// Checkpoint::SetDeathRestart (PC 0x00BA1F20, thiscall(int, int), ret 8):
// the checkpoint trigger's HandleEvents calls it (0x00BACC40), and so does the
// node with a checkpoint GUID in its attributes that the kiosk path uses
// (0x00BA67BC). It copies the checkpoint's 16-byte GUID (this+0x24) into the
// global death checkpoint at 0x0208FCC4, announces it, and autosaves. The
// restart reads that global back (GetDeathCheckpointId 0x00B75810, called by
// the main loop's restart at 0x00429B80) and resolves it with
// Checkpoint::Find(const Guid*) (0x00B86C30, cdecl): override GUID first,
// otherwise the object registry by GUID plus a class query for Checkpoint
// (0xDC24DB08).
//
// So: the host sends its death checkpoint GUID, two times a second, forever.
// A client that follows the host lets none of its own SetDeathRestart calls
// through and instead applies the host's GUID with the same call on its own
// copy of that checkpoint, once the object exists here.
//
// And the restart itself follows too. Every checkpoint restart (the pause
// menu's "restart checkpoint", GameMainLoop::CheckpointRestart 0x0040F840;
// LevelRestart 0x0040FC70; the death path 0x00ABB2A0) ends in one
// implementation, 0x0040EC10 (`thiscall bool(GameMainLoop*)`, sets
// this+0x210 = 1 for the main loop to pick up). The DS2 Debug Menu hooks the
// thunk in front of it (0x004054C0), not the function, so the function itself
// is free to wrap. The host counts its restarts into the checkpoint packet
// (reserved[0]); a client that sees the count move restarts the same way,
// unless it restarted itself in the last ten seconds, which is what the
// death mirror already did for it.
namespace loadsync { void OnRestart(); void OnSoloRestart(); void OnRestartAsked(); bool LevelStarted(); bool JoinSuitPending(); void UpdateJoinSuit(const uint32_t* guid, const char* why); }   // runtime_coop_loadsync.inc
namespace clientprog { extern bool enabled; extern uint64_t hostId; int ReadFile(uint64_t host, coop_client_progress::File& f); }   // runtime_coop_clientprogress.inc
namespace respawn { void OnRestart(); }    // runtime_coop_respawn.inc
namespace respawn { bool SuppressDeathRestart(); }   // runtime_coop_respawn.inc
namespace arena { bool RespawnHere(); }   // runtime_coop_arena.inc: a campaign arena with [Arena] Modes
namespace lifecycle { bool WantsSyncedRestart(bool fromDeathPath); bool BeginRestart(const char* why); }   // runtime_coop_lifecycle.inc
namespace checkpoint {
bool enabled = false, followHost = true, installed = false, followRestart = true;
constexpr uint32_t kSetDeathRestartRva = 0x7A1F20;
constexpr uint32_t kRestartImplRva = 0xEC10;
constexpr uint32_t kRestartRequestedOffset = 0x210;
safetyhook::InlineHook restartHook;
uint32_t restartEpoch = 0;          // host: restarts so far
uint32_t seenRestartEpoch = 0;      // client: the host's count already answered
bool restartBaselineSeen = false;   // the first packet's restart count is the baseline, not a restart
uint32_t baselineToken = 0;         // whose counts those are: a host that restarts his game counts from zero again
uint32_t wantedRestartEpoch = 0;
bool restartWanted = false;
DWORD lastLocalRestart = 0;
constexpr DWORD kOwnRestartGraceMs = 10000;
volatile LONG restartsLocal = 0, restartsFollowed = 0, restartsSkippedOwn = 0, restartsFaulted = 0, restartsSynced = 0, staleFollowsDropped = 0;
// A host restart heard while this machine loads belongs to the level that went (02.11.: a client
// came out of the main menu into an arena and pulled the host's older mp_detonate restart there,
// so its fresh arena loaded twice). Dropped once a load is seen after it was heard.
bool restartWantedSawLoad = false;
// The tick does not run through every loading screen, so the load can pass unseen (02.11. client:
// a restart heard before the arena loaded was pulled in the arena 0.3 s after its first Isaac was
// born, while his suit still streamed in). The map it was heard on decides as well.
uint32_t restartWantedMap = 0;
volatile LONG staleFollowsOtherMap = 0;
bool LevelRunningNow() {
    uint32_t lm = 0, state = 0;
    return Read(base + 0x1C1C0A4, &lm, 4) && lm && Read(lm + 0x18, &state, 4) && state == 16;
}
// The follow was one-directional, and it showed: on 22.09. the client died and
// restarted twice (restarts_local=2) while the host followed none, because the
// host is the only machine that ever announced anything. A death is a death
// whoever dies, so a client now asks and the host answers: it restarts, which
// bumps its own count, and every client follows that the usual way. Keeping the
// decision on the host means three machines behave like two, and a request that
// is lost on the wire is repeated.
constexpr uint32_t kFlagRestartRequest = 1;
// A client reached a checkpoint while the host watches dead ([Spectate]): {guid, reserved[1] = report}.
constexpr uint32_t kFlagReached = 2;
uint32_t reachGuid[4]{}, reachSeq = 0;
unsigned reachToSend = 0;
DWORD lastReachSend = 0;
volatile LONG reachesSent = 0;
constexpr unsigned kRequestSends = 6;        // plain UDP: the ask goes out a few times
constexpr DWORD kRequestIntervalMs = 200;
unsigned requestsToSend = 0;                 // client: how many copies of the ask are left
DWORD lastRequestSend = 0;
uint32_t restartRequestSeq = 0;              // client: which ask this is
// host: the last ask it acted on, per client (copies of two clients' asks interleave on the wire)
coop_peers::RestartRequest served[coop_peers::kMaxPeers]{};
// Before a client's restart: if the host's newest checkpoint never got applied here (its object
// was not found), the restart would load our older one. The death checkpoint the restart reads
// (GetDeathCheckpointId 0x00B75810 copies 0x0208FCC4) gets the host's GUID; LevelManager::
// GetCheckpoint resolves it by the registry, else Find(null) takes the override or the default.
// ([Checkpoint] HostCheckpointOnRestart)
bool hostCheckpointOnRestart = true;
volatile LONG hostCheckpointWrites = 0;
bool hostRestartWanted = false;
volatile LONG restartsRequested = 0, restartsServed = 0, requestsKeptLocal = 0, requestsIgnored = 0;
constexpr uint32_t kFindRva = 0x786C30;
constexpr uint32_t kDeathGuidRva = 0x1C8FCC4;

constexpr uint32_t kMagic = 0x54504B43;   // 'CKPT'
constexpr uint32_t kVersion = 1;
struct Packet {
    uint32_t magic, version, token, sequence;
    uint32_t guid[4];
    uint32_t epoch, flags;
    uint32_t reserved[4];
};
static_assert(sizeof(Packet) == 56, "Unexpected checkpoint wire layout");
bool Valid(const Packet& p) { return p.magic == kMagic && p.version == kVersion && p.token; }

safetyhook::InlineHook setHook;
LONG applying = 0;   // game thread only: the plugin's own apply must pass the client guard

// Host side.
uint32_t current[4]{};
uint32_t epoch = 0, sequence = 0;
DWORD lastSend = 0;
// Client side.
uint32_t wanted[4]{}, applied[4]{};
uint32_t wantedEpoch = 0, appliedEpoch = 0;
bool pending = false;
DWORD lastTry = 0;
unsigned tries = 0;
// 17.10. (user: "the necro player got no checkpoints"): a necro defers the host's checkpoint, so a
// host restart then loads our older one. Once Isaac is back and the newer one is applied, one more
// restart here loads the world where the host stands.
bool reloadAfterApply = false;
DWORD reloadMarkedAt = 0;
// The reload itself waits until the level runs: asked for at the loading screen it was taken
// for the restart still in progress (17.10., 4FCB5607: "restarting here once more" logged,
// restarts_followed stayed 1, the level started at the old checkpoint).
bool reloadDue = false;
DWORD reloadDueAt = 0;
uint32_t reloadMap = 0;   // the map it belongs to: after a level change it is dropped
uint32_t CurrentMapHash() {
    char name[64]{};
    const uint32_t loop = level::GameMainLoop();
    return loop && Read(loop + 4, name, sizeof(name) - 1) && name[0] ? gate::NameHash(name) : 0;   // +4: the map name, as level::kMapOffset
}
volatile LONG necroReloads = 0, necroReloadsMarked = 0, necroReloadsExpired = 0, necroReloadsFailed = 0;
// The necro join waits for it (a minute at most, the load included).
bool ReloadWaiting() {
    const DWORD now = GetTickCount();
    return (reloadAfterApply && TicksSince(now, reloadMarkedAt) < 60000) || (reloadDue && TicksSince(now, reloadDueAt) < 60000);
}

volatile LONG hostSets = 0, suppressed = 0, sent = 0, received = 0, appliedCount = 0,
              notFound = 0, applyRefused = 0, faults = 0, hostAdopted = 0, appliesDeferredNecro = 0;

bool GuidEqual(const uint32_t* a, const uint32_t* b) { return std::memcmp(a, b, 16) == 0; }
bool GuidZero(const uint32_t* a) { return !(a[0] | a[1] | a[2] | a[3]); }

// Everything a restart resets, whoever started it. A pulled restart (the
// host's followed by a client, a client's death served by the host) calls
// the game's restart below the hook and skipped all of it until 24.09.: the
// journal, the door votes and the gate queue outlived the world they
// belonged to.
// The survival director's enemies go before the world is rebuilt (DS2Survival_BeforeTeardown in the
// randomizer half: their machines parked on the idle table, the enemies deleted). Dump 75636 (02.11.):
// seven of them lived through a pause-menu restart in arena_ch01 and crashed the host 4 s later.
using TeardownFn = int(__cdecl*)(int deleteNow);
TeardownFn teardownFn = nullptr;
bool teardownLooked = false;
volatile LONG teardownCalls = 0, teardownEnemies = 0;
void PurgeDirectorEnemies(const char* why) {
    if (!teardownLooked) {
        teardownLooked = true;
        if (const HMODULE module = ds2coop::PartnerModule("DS2RandomizerObserver.asi"))
            teardownFn = reinterpret_cast<TeardownFn>(GetProcAddress(module, "DS2Survival_BeforeTeardown"));
    }
    if (!teardownFn) return;
    InterlockedIncrement(&teardownCalls);
    const int n = teardownFn(1);
    InterlockedExchangeAdd(&teardownEnemies, n);
    if (n) gate::Event("coop_checkpoint %s: the director's %d live enemies parked and deleted before the restart", why, n);
}
void RestartBookkeeping() {
    PurgeDirectorEnemies("a checkpoint restart");
    // A partner's death that had not been played here yet belongs to the world
    // that is going; played after the reload it would kill the new Isaac and
    // restart everybody a second time.
    InterlockedExchange(&pendingPeerDeath, 0);
    restartBookkeepingAt = Stamp(GetTickCount());
    respawn::OnRestart();   // a death waiting for its respawn belongs to the old world
    gate::ResetApprovals();
    gate::OnRestart("a checkpoint restart");
    necro::OnRestart();   // a necromorph player hands slot 0 back to Isaac before the world is rebuilt
    bot::OnRestart();     // the restart deletes the bot with every other registered Player
    mirror::OnRestart();
    worldcalls::OnRestart();   // door votes and a waiting door: the restart puts everybody back
    ghosttrigger::OnRestart();
    loadsync::OnRestart();   // loading again until the level manager asks
    spectate::OnRestart();   // the watching camera goes before the world it looks at
}

// A bot's death must not restart the level (28.09.: both bots died at full health,
// no enemy near, from something that kills outright, and the level restarted
// around a living player). The death path (0x00ABB260, the call returning to
// 0x00ABB2A5, reached through the thunk 0x004054C0 that the Debug Menu detours,
// so the return address is looked for on the stack) is refused while the local
// Isaac is alive and a bot died a moment ago. Pause-menu and level restarts pass.
constexpr uint32_t kDeathPathReturnRva = 0x6BB2A5;
volatile LONG botRestartsRefused = 0;
bool FromDeathPath(const uint32_t* stack) {
    const uint32_t want = static_cast<uint32_t>(base + kDeathPathReturnRva);
    for (unsigned i = 0; i < 24; ++i) {
        uint32_t v = 0;
        if (Read(reinterpret_cast<uintptr_t>(stack + i), &v, 4) && v == want) return true;
    }
    return false;
}
bool LocalIsaacAlive() {
    const uint32_t me = LocalCampaignPlayer();
    const uint32_t health = me ? hits::FindHealthComponent(me) : 0;
    float left = 0.0f;
    return health && Read(health + hits::kHealthCurrent, &left, 4) && left > 0.0f;
}
volatile LONG respawnRestartsRefused = 0, restartCallerLines = 0;
// The first code addresses on the stack: who asked for a local restart.
void LogRestartCallers(const uint32_t* stack, const char* verdict) {
    if (InterlockedIncrement(&restartCallerLines) > 20) return;
    char text[160] = "";
    size_t used = 0;
    unsigned found = 0;
    for (unsigned i = 0; i < 48 && found < 6; ++i) {
        uint32_t v = 0;
        if (!Read(reinterpret_cast<uintptr_t>(stack + i), &v, 4) || v < base + 0x1000 || v >= base + hits::kCodeEnd) continue;
        used += std::snprintf(text + used, sizeof(text) - used, " %08X", v);
        ++found;
        if (used >= sizeof(text) - 10) break;
    }
    gate::Event("coop_checkpoint local restart %s; stack code addresses:%s", verdict, text);
}
void EnsureHostCheckpoint(const char* why) {
    if (!hostCheckpointOnRestart || IsHost() || !followHost) return;
    const uint32_t* host = pending ? wanted : appliedEpoch ? applied : nullptr;
    uint32_t death[4]{};
    if (!host || GuidZero(host) || !Read(base + kDeathGuidRva, death, 16) || GuidEqual(death, host)) return;
    if (!WriteBytes(base + kDeathGuidRva, host, 16)) return;
    InterlockedIncrement(&hostCheckpointWrites);
    gate::Event("coop_checkpoint %s: the death checkpoint %08X-%08X was not the host's; %08X-%08X-%08X-%08X written for the restart (%s)", why, death[0], death[1],
                host[0], host[1], host[2], host[3], pending ? "his newest was never found here" : "it moved since");
}
bool __fastcall OnRestartImpl(void* self, void*) {
    const DWORD now = GetTickCount();
    const auto* stack = reinterpret_cast<const uint32_t*>(_AddressOfReturnAddress());
    if (botDiedAt && TicksSince(now, botDiedAt) < 20000 && FromDeathPath(stack) && LocalIsaacAlive()) {
        if (InterlockedIncrement(&botRestartsRefused) <= 20)
            gate::Event("coop_checkpoint a restart from the death path refused: a bot died %lu ms ago, the player is alive", static_cast<unsigned long>(now - botDiedAt));
        return false;
    }
    // A respawn on the MP map leaves the native death screen running, and its
    // timer still asks for the restart (09.10.: the host restarted 33 s after his
    // respawn with nobody dead). While Isaac lives that restart is refused.
    // Watching a partner, the death screen's timer asks for the restart too: refused until he is back.
    if (FromDeathPath(stack) && spectate::Active()) {
        InterlockedIncrement(&spectate::restartsRefused);
        LogRestartCallers(stack, "from the death path REFUSED (watching a partner until the next checkpoint)");
        return false;
    }
    if (FromDeathPath(stack) && LocalIsaacAlive() && respawn::SuppressDeathRestart()) {
        InterlockedIncrement(&respawnRestartsRefused);
        LogRestartCallers(stack, "from the death path REFUSED (respawned, Isaac alive)");
        return false;
    }
    // The host's own restart in a match mode with partners (the pause menu's): announced to every
    // partner first; the synced restart runs it through here again in at most RestartAckMs.
    if (lifecycle::WantsSyncedRestart(FromDeathPath(stack))) {
        InterlockedIncrement(&restartsSynced);
        LogRestartCallers(stack, "held: the partners are told first ([Lifecycle] SyncedRestart), the map restarts for everybody");
        lifecycle::BeginRestart("the host's checkpoint restart");
        return false;
    }
    LogRestartCallers(stack, FromDeathPath(stack) ? "from the death path" : "passed");
    lastLocalRestart = GetTickCount();
    InterlockedIncrement(&restartsLocal);
    if (IsHost()) { ++restartEpoch; lastSend = 0; }
    else if (followHost && (mpmap::InVisit() || arena::RespawnHere())) {
        // On an MP map a client's restart reloads only its own world: asked of the
        // host it reloaded everybody and both fell through the map origin (13.10.).
        InterlockedIncrement(&requestsKeptLocal);
        LogRestartCallers(stack, "on an MP map: this machine only, the host is not asked");
        loadsync::OnSoloRestart();
    } else if (followHost) {
        // The host decides; the client only reports: the host restarts and the
        // announcement comes back round to everybody, this machine included.
        if (++restartRequestSeq == 0) ++restartRequestSeq;
        requestsToSend = kRequestSends;
        lastRequestSend = 0;
        InterlockedIncrement(&restartsRequested);
        loadsync::OnRestartAsked();   // the loading screen waits for the host's answer
        EnsureHostCheckpoint("our own restart");
    }
    RestartBookkeeping();
    return restartHook.thiscall<bool>(self);
}

bool __fastcall OnSetDeathRestart(void* self, void*, int a, int b) {
    if (!IsHost() && followHost && applying == 0 && appliedEpoch != 0) {
        // The client's own checkpoints and kiosk saves do not move its
        // restart point; the host's do. Until the first one from the host
        // has landed, the client keeps its own, so a client that loads
        // before the host is never left without any restart point at all.
        InterlockedIncrement(&suppressed);
        // A host watching dead learns of it: he comes back here (runtime_coop_spectate.inc).
        uint32_t guid[4]{};
        if (roles::Spectators(GetTickCount()) && Read(reinterpret_cast<uintptr_t>(self) + 0x24, guid, 16) && !GuidZero(guid) && !GuidEqual(guid, reachGuid)) {
            std::memcpy(reachGuid, guid, 16);
            ++reachSeq;
            reachToSend = kRequestSends;
            lastReachSend = 0;
        }
        return false;
    }
    const bool result = setHook.thiscall<bool>(self, a, b);
    if (IsHost()) {
        uint32_t guid[4]{};
        if (Read(base + kDeathGuidRva, guid, 16) && !GuidEqual(guid, current)) {
            std::memcpy(current, guid, 16);
            ++epoch;
            lastSend = 0;   // goes out on the next tick
            InterlockedIncrement(&hostSets);
        }
    }
    return result;
}

void OnPacket(const Packet& p, unsigned slot) {
    if (IsHost()) {
        if (p.flags == kFlagReached) { spectate::OnPartnerReached(p.guid, p.token, p.reserved[1]); return; }
        // The other thing a client sends: "I died, restart everybody".
        // Answered once per ask; the copies that follow carry the same sequence.
        if (!followHost || p.flags != kFlagRestartRequest || !p.reserved[1]) return;
        InterlockedIncrement(&received);
        if (mpmap::InVisit() || arena::RespawnHere()) {
            if (InterlockedIncrement(&requestsIgnored) <= 10)
                gate::Event("coop_checkpoint restart asked for by %08X on %s: ignored", p.token, mpmap::InVisit() ? "an MP map" : "a campaign arena");
            return;
        }
        if (!served[slot].Accept(p.token, p.reserved[1])) return;
        hostRestartWanted = true;
        InterlockedIncrement(&restartsServed);
        gate::Event("coop_checkpoint restart asked for by %08X (ask %u)", p.token, p.reserved[1]);
        return;
    }
    // Another client's restart request or reach report (the clients are meshed): only the
    // host answers it, and its token must not become this client's baseline.
    if (!followHost || p.flags != 0) return;
    InterlockedIncrement(&received);
    clientprog::OnHost(p.token, p.reserved[1], p.reserved[2], p.reserved[3]);
    // A story join still loading: the client progress file for this host names the suit to be born in.
    if (clientprog::enabled && clientprog::hostId && loadsync::JoinSuitPending()) {
        static uint64_t triedFor = 0;
        if (triedFor != clientprog::hostId) {
            triedFor = clientprog::hostId;
            coop_client_progress::File f;
            const int r = clientprog::ReadFile(clientprog::hostId, f);
            if (r == 0 && f.hasSuit) loadsync::UpdateJoinSuit(f.suit, "the client progress file for this host");
            else gate::Event("coop_checkpoint join suit: %s for host %08X%08X, the chapter's suit stays", r == 0 ? "the client progress file names no suit" : r == 1 ? "no client progress file" : "the client progress file did not read",
                             static_cast<unsigned>(clientprog::hostId >> 32), static_cast<unsigned>(clientprog::hostId));
        }
    }
    // The host's restart count: the first packet's count is the baseline
    // (whatever it is, zero included); every later change is a restart to follow.
    // Counts only move forward. On a slow link an older packet can land after
    // a newer one; "different" then read as a restart that never happened.
    if (!restartBaselineSeen || baselineToken != p.token) {
        restartBaselineSeen = true;
        baselineToken = p.token;
        seenRestartEpoch = p.reserved[0];
        appliedEpoch = 0;   // a new host game: its checkpoint counts start over too
        // and nothing the old host asked for is followed any more (audit 16.10.)
        restartWanted = false;
        wantedRestartEpoch = 0;
        pending = false;
        wantedEpoch = 0;
    } else if (static_cast<int32_t>(p.reserved[0] - seenRestartEpoch) > 0 &&
               (!restartWanted || static_cast<int32_t>(p.reserved[0] - wantedRestartEpoch) > 0)) {
        if (!followRestart) seenRestartEpoch = p.reserved[0];
        else { wantedRestartEpoch = p.reserved[0]; restartWanted = true; restartWantedSawLoad = !LevelRunningNow(); restartWantedMap = CurrentMapHash(); }
    }
    if (GuidZero(p.guid) || !p.epoch) return;                            // restart count only, no checkpoint yet
    if (p.epoch == appliedEpoch && GuidEqual(p.guid, applied)) return;   // already there
    if (appliedEpoch && static_cast<int32_t>(p.epoch - appliedEpoch) < 0) return;   // a late older checkpoint
    if (pending && static_cast<int32_t>(p.epoch - wantedEpoch) < 0) return;
    if (pending && p.epoch == wantedEpoch) return;                        // already trying
    std::memcpy(wanted, p.guid, 16);
    wantedEpoch = p.epoch;
    pending = true;
    tries = 0;
    lastTry = 0;
}

// The client's restart point stored again with what it carries now (its progress was just given back).
void Reapply(const char* why) {
    if (IsHost() || !appliedEpoch || pending) return;
    std::memcpy(wanted, applied, 16);
    wantedEpoch = appliedEpoch;
    pending = true;
    tries = 0;
    lastTry = 0;
    gate::Event("coop_checkpoint the host's checkpoint is stored again (%s)", why);
}
int Filter(unsigned) { InterlockedIncrement(&faults); return EXCEPTION_EXECUTE_HANDLER; }
uint32_t FindCheckpoint(const uint32_t* guid) {
    using Find = uint32_t(__cdecl*)(const uint32_t*);
    __try {
        return reinterpret_cast<Find>(base + kFindRva)(guid);
    } __except (Filter(GetExceptionCode())) {
        return 0;
    }
}
bool ApplyTo(uint32_t object) {
    bool ok = false;
    ++applying;
    __try {
        ok = setHook.thiscall<bool>(reinterpret_cast<void*>(object), 0, 0);
    } __except (Filter(GetExceptionCode())) {
        ok = false;
    }
    --applying;
    return ok;
}

// Pull the game's own restart, the way the follow always has. Returns false
// while the level is not ready for it, so the caller can try again.
bool PullRestart() {
    if (!restartHook || !inLevel || !LocalSlotZeroEntity()) return false;
    const uint32_t loop = level::GameMainLoop();
    uint32_t requested = 0;
    if (!loop || !Read(loop + kRestartRequestedOffset, &requested, 4)) return false;
    if (requested) return true;   // the game is already doing it
    // A local restart as far as the grace windows go, so the host's count
    // coming back round is not followed a second time. The host counts it:
    // every other client follows a restart served for one client's death.
    lastLocalRestart = GetTickCount();
    if (IsHost()) { ++restartEpoch; lastSend = 0; }
    RestartBookkeeping();
    __try {
        restartHook.thiscall<bool>(reinterpret_cast<void*>(loop));
        InterlockedIncrement(&restartsFollowed);
    } __except (Filter(GetExceptionCode())) {
        InterlockedIncrement(&restartsFaulted);
    }
    return true;
}

// The level went away: a restart asked for or followed there belongs to it (audit 16.10.:
// a client's death during the host's chapter load restarted the new level for everybody).
void OnLevelExit() {
    if (hostRestartWanted || restartWanted || requestsToSend)
        gate::Event("coop_checkpoint level exit: dropped%s%s%s", hostRestartWanted ? " a client's restart request" : "", restartWanted ? " a restart to follow" : "",
                    requestsToSend ? " our own restart request" : "");
    hostRestartWanted = false;
    if (restartWanted) { restartWanted = false; seenRestartEpoch = wantedRestartEpoch; }
    requestsToSend = 0;
}
// From Poll, on the game thread.
void Tick(DWORD now) {
    if (!installed || !enabled) return;
    if (IsHost()) {
        // A client asked for a restart. A local restart in the last ten
        // seconds already covers it.
        if (hostRestartWanted) {
            if (lastLocalRestart && TicksSince(now, lastLocalRestart) < kOwnRestartGraceMs) {
                hostRestartWanted = false;
                InterlockedIncrement(&restartsSkippedOwn);
            } else if (PullRestart()) {
                hostRestartWanted = false;
                gate::Event("coop_checkpoint restart pulled for a client's death");
            }
        }
        // Sent from the start, restart count included: the client takes the
        // first count it sees as its baseline, so when the host's first
        // packet only went out with its first death restart (count 1) the
        // client adopted that as the baseline and never followed (14:19: the
        // host died and restarted, the client received two packets, followed
        // none). With count 0 announced beforehand the first restart is seen
        // as a change. A packet with no checkpoint and no restart is ignored
        // by the client's OnPacket after it has noted the count.
        // A loaded save writes the death checkpoint without SetDeathRestart (RestoreFromSavedGame
        // 0x00B75A00): until the host's next checkpoint trigger the clients kept their own and a
        // client kiosk save moved it (16.10., 19:27-19:33). Taken from the game itself.
        if (inLevel) {
            uint32_t guid[4]{};
            if (Read(base + kDeathGuidRva, guid, 16) && (guid[0] | guid[1] | guid[2] | guid[3]) && !GuidEqual(guid, current)) {
                std::memcpy(current, guid, 16);
                ++epoch;
                lastSend = 0;
                InterlockedIncrement(&hostAdopted);
                gate::Event("coop_checkpoint host death checkpoint taken from the game (save load or restore): %08X-%08X-%08X-%08X epoch=%u", guid[0], guid[1],
                            guid[2], guid[3], epoch);
            }
        }
        if (!peerTable.Count() || now - lastSend < 500) return;
        lastSend = now;
        // reserved[1..3]: the host's save key and id, for the clients' own progress (runtime_coop_clientprogress.inc)
        const uint64_t hostId = clientprog::OwnHostId();
        Packet p{kMagic, kVersion, peerReadiness.token, ++sequence, {current[0], current[1], current[2], current[3]}, epoch, 0,
                 {restartEpoch, clientprog::HostSaveKey(), static_cast<uint32_t>(hostId >> 32), static_cast<uint32_t>(hostId)}};
        if (SendToPeers(&p, sizeof(p))) InterlockedIncrement(&sent);
        return;
    }
    // The local death, reported to the host so it restarts everybody. Repeated a
    // few times because the side channel is plain UDP and one lost datagram
    // would leave the pair at different checkpoints.
    if (reachToSend && peerTable.Count() && now - lastReachSend >= kRequestIntervalMs) {
        lastReachSend = now;
        --reachToSend;
        Packet reach{kMagic, kVersion, peerReadiness.token, ++sequence, {reachGuid[0], reachGuid[1], reachGuid[2], reachGuid[3]}, 0, kFlagReached, {0, reachSeq, 0, 0}};
        if (SendToPeers(&reach, sizeof(reach))) InterlockedIncrement(&reachesSent);
    }
    if (requestsToSend && peerTable.Count() && now - lastRequestSend >= kRequestIntervalMs) {
        lastRequestSend = now;
        --requestsToSend;
        Packet ask{kMagic, kVersion, peerReadiness.token, ++sequence, {0, 0, 0, 0}, 0, kFlagRestartRequest, {0, restartRequestSeq, 0, 0}};
        if (SendToPeers(&ask, sizeof(ask))) InterlockedIncrement(&sent);
    }
    if (restartWanted && !LevelRunningNow()) restartWantedSawLoad = true;
    if (restartWanted && restartWantedMap && CurrentMapHash() && CurrentMapHash() != restartWantedMap) {
        restartWanted = false;
        seenRestartEpoch = wantedRestartEpoch;
        InterlockedIncrement(&staleFollowsDropped);
        InterlockedIncrement(&staleFollowsOtherMap);
        gate::Event("coop_checkpoint the host's restart %u was heard on another map (%08X, now %08X): not followed here", wantedRestartEpoch, restartWantedMap,
                    CurrentMapHash());
    }
    if (restartWanted && restartWantedSawLoad && LevelRunningNow()) {
        restartWanted = false;
        seenRestartEpoch = wantedRestartEpoch;
        InterlockedIncrement(&staleFollowsDropped);
        gate::Event("coop_checkpoint the host's restart %u was heard before this level loaded: not followed here", wantedRestartEpoch);
    }
    if (restartWanted && restartHook) {
        // A local restart in the last ten seconds (the death mirror, the local
        // death, or the player's own hand) already did it; otherwise do what
        // the host did.
        if (lastLocalRestart && TicksSince(now, lastLocalRestart) < kOwnRestartGraceMs) {
            restartWanted = false;
            InterlockedIncrement(&restartsSkippedOwn);
        } else {
            const bool stale = pending && !IsHost() && necro::LocalRole() != 0;
            if (!stale) EnsureHostCheckpoint("following the host's restart");
            if (PullRestart()) {
                restartWanted = false;
                gate::Event("coop_checkpoint restart followed (host restarted)%s", stale ? ": it loads our older checkpoint, the host's newer one waited while we played a necro" : "");
                if (stale) { reloadAfterApply = true; reloadMarkedAt = now; InterlockedIncrement(&necroReloadsMarked); }
            }
        }
        if (!restartWanted) seenRestartEpoch = wantedRestartEpoch;
    }
    if (reloadAfterApply && TicksSince(now, reloadMarkedAt) >= 60000) {
        reloadAfterApply = false;   // no restart out of the blue later
        InterlockedIncrement(&necroReloadsExpired);
        gate::Event("coop_checkpoint the reload after our necro time waited a minute for the host's checkpoint: dropped");
    }
    if (reloadDue) {
        uint32_t requested = 1;
        const uint32_t loop = level::GameMainLoop();
        if (TicksSince(now, reloadDueAt) >= 60000) {
            reloadDue = false;
            InterlockedIncrement(&necroReloadsFailed);
            gate::Event("coop_checkpoint the reload after our necro time found no running level for a minute: dropped");
        } else if (inLevel && reloadMap && CurrentMapHash() != reloadMap) {
            reloadDue = false;
            InterlockedIncrement(&necroReloadsFailed);
            gate::Event("coop_checkpoint the reload after our necro time is dropped: another map is loaded now");
        } else if (inLevel && levelSettled && loadsync::LevelStarted() && LocalSlotZeroEntity() && necro::LocalRole() == 0 && !spectate::Active() && loop &&
                   Read(loop + kRestartRequestedOffset, &requested, 4) && !requested) {
            reloadDue = false;
            const LONG before = InterlockedCompareExchange(&restartsFollowed, 0, 0);
            loadsync::OnSoloRestart();   // the partners keep playing: nobody to wait for at the loading screen
            const bool asked = PullRestart() && InterlockedCompareExchange(&restartsFollowed, 0, 0) != before;
            InterlockedIncrement(asked ? &necroReloads : &necroReloadsFailed);
            gate::Event("coop_checkpoint the host's checkpoint %08X is ours now: %s", applied[0],
                        asked ? "restarting here once more to load it" : "the reload could not be asked for (see restarts_faulted)");
        }
    }
    if (!pending || now - lastTry < 1500) return;
    lastTry = now;
    // A necro client's slot 0 holds no Isaac: the checkpoint's store would keep a restart point
    // without him. It waits until he is Isaac again for good (role 0).
    if (necro::LocalRole() != 0 || spectate::Active()) {
        if (InterlockedIncrement(&appliesDeferredNecro) == 1 || appliesDeferredNecro % 40 == 0)
            gate::Event("coop_checkpoint apply deferred: this machine %s", spectate::Active() ? "watches a partner dead (the checkpoint brings him back)" : "plays a necro");
        return;
    }
    ++tries;
    const uint32_t object = FindCheckpoint(wanted);
    if (!object) {
        // Not loaded here yet: the host may be an area ahead. Keep asking; a
        // newer GUID from the host replaces this one anyway. Three minutes of
        // asking is a checkpoint of another world: given up (audit 16.10.).
        InterlockedIncrement(&notFound);
        if (tries >= 120) {
            gate::Event("coop_checkpoint %08X-%08X-%08X-%08X never found here after %u tries: given up", wanted[0], wanted[1], wanted[2], wanted[3], tries);
            pending = false;
        }
        return;
    }
    if (ApplyTo(object)) {
        std::memcpy(applied, wanted, 16);
        appliedEpoch = wantedEpoch;
        pending = false;
        InterlockedIncrement(&appliedCount);
        if (reloadAfterApply && !IsHost()) {
            reloadAfterApply = false;
            reloadDue = true;
            reloadDueAt = Stamp(now);
            reloadMap = CurrentMapHash();
            gate::Event("coop_checkpoint the host's checkpoint %08X applied after our necro time: one more restart here once the level runs", wanted[0]);
        }
        gate::Event("coop_checkpoint applied %08X-%08X-%08X-%08X object=%08X epoch=%u tries=%u",
                    wanted[0], wanted[1], wanted[2], wanted[3], object, wantedEpoch, tries);
    } else {
        InterlockedIncrement(&applyRefused);
        if (tries >= 10) { pending = false; gate::Event("coop_checkpoint gave up epoch=%u after %u refusals", wantedEpoch, tries); }
    }
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Checkpoint", "Enabled", 1, ini.c_str()) != 0;
    followHost = GetPrivateProfileIntA("Checkpoint", "FollowHost", 1, ini.c_str()) != 0;
    if (!enabled) { std::fprintf(log, "coop_checkpoint off\n"); return false; }
    // sub esp,0x30 / push ebx / push esi / push 0 / mov esi,ecx / xor bl,bl / call
    static const unsigned char kSet[] = {0x83,0xEC,0x30,0x53,0x56,0x6A,0x00,0x8B,0xF1,0x32,0xDB,0xE8};
    // push ecx / mov edx,[0x02091820] / mov eax,[0x02091828] / push ebx / mov ebx,[0x02091824]
    static const unsigned char kFind[] = {0x51,0x8B,0x15,0x20,0x18,0x09,0x02,0xA1,0x28,0x18,0x09,0x02,0x53,0x8B,0x1D,0x24};
    unsigned char actual[sizeof(kFind)]{};
    if (!Read(base + kFindRva, actual, sizeof(kFind)) || std::memcmp(actual, kFind, sizeof(kFind)) != 0) {
        std::fprintf(log, "coop_checkpoint Checkpoint::Find signature mismatch at %08X; off\n", static_cast<unsigned>(base + kFindRva));
        return false;
    }
    if (!hook_site::Install(log, "coop_checkpoint SetDeathRestart", base + kSetDeathRestartRva, kSet, sizeof(kSet),
                            reinterpret_cast<void*>(OnSetDeathRestart), setHook))
        return false;
    followRestart = GetPrivateProfileIntA("Checkpoint", "FollowRestart", 1, ini.c_str()) != 0;
    hostCheckpointOnRestart = GetPrivateProfileIntA("Checkpoint", "HostCheckpointOnRestart", 1, ini.c_str()) != 0;
    // push ecx / push edi / mov edi,ecx / call
    static const unsigned char kRestart[] = {0x51,0x57,0x8B,0xF9,0xE8};
    if (!hook_site::Install(log, "coop_checkpoint RestartImpl", base + kRestartImplRva, kRestart, sizeof(kRestart),
                            reinterpret_cast<void*>(OnRestartImpl), restartHook))
        std::fprintf(log, "coop_checkpoint restart follow off (hook failed)\n");
    installed = true;
    std::fprintf(log, "coop_checkpoint follow_host=%d death_guid=%08X find=%08X role=%s\n", followHost,
                 static_cast<unsigned>(base + kDeathGuidRva), static_cast<unsigned>(base + kFindRva),
                 IsHost() ? "host (sends)" : "client (follows)");
    return true;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG activity = InterlockedCompareExchange(&hostSets, 0, 0) + InterlockedCompareExchange(&suppressed, 0, 0) +
                          InterlockedCompareExchange(&received, 0, 0) + InterlockedCompareExchange(&notFound, 0, 0) +
                          InterlockedCompareExchange(&restartsLocal, 0, 0) + InterlockedCompareExchange(&restartsFollowed, 0, 0);
    if (activity == last) return;
    last = activity;
    std::fprintf(log, "coop_checkpoint host_sets=%ld suppressed=%ld sent=%ld received=%ld applied=%ld not_found=%ld "
                      "refused=%ld faults=%ld pending=%d current=%08X-%08X-%08X-%08X epoch=%u "
                      "restarts_local=%ld restarts_followed=%ld restarts_requested=%ld restarts_served=%ld restarts_skipped_own=%ld restarts_faulted=%ld restart_epoch=%u\n",
                 InterlockedCompareExchange(&hostSets, 0, 0), InterlockedCompareExchange(&suppressed, 0, 0),
                 InterlockedCompareExchange(&sent, 0, 0), InterlockedCompareExchange(&received, 0, 0),
                 InterlockedCompareExchange(&appliedCount, 0, 0), InterlockedCompareExchange(&notFound, 0, 0),
                 InterlockedCompareExchange(&applyRefused, 0, 0), InterlockedCompareExchange(&faults, 0, 0),
                 pending, IsHost() ? current[0] : applied[0], IsHost() ? current[1] : applied[1],
                 IsHost() ? current[2] : applied[2], IsHost() ? current[3] : applied[3], IsHost() ? epoch : appliedEpoch,
                 InterlockedCompareExchange(&restartsLocal, 0, 0), InterlockedCompareExchange(&restartsFollowed, 0, 0),
                 InterlockedCompareExchange(&restartsRequested, 0, 0), InterlockedCompareExchange(&restartsServed, 0, 0),
                 InterlockedCompareExchange(&restartsSkippedOwn, 0, 0), InterlockedCompareExchange(&restartsFaulted, 0, 0),
                 IsHost() ? restartEpoch : seenRestartEpoch);
    std::fprintf(log, "coop_checkpoint host_taken_from_game=%ld applies_deferred=%ld reach_reports_sent=%ld host_checkpoint_on_restart=%d writes=%ld | stale follows "
                      "dropped=%ld (other map %ld) | synced_holds=%ld | director teardowns=%ld enemies=%ld\n",
                 InterlockedCompareExchange(&hostAdopted, 0, 0), InterlockedCompareExchange(&appliesDeferredNecro, 0, 0), InterlockedCompareExchange(&reachesSent, 0, 0),
                 hostCheckpointOnRestart ? 1 : 0, InterlockedCompareExchange(&hostCheckpointWrites, 0, 0), InterlockedCompareExchange(&staleFollowsDropped, 0, 0),
                 InterlockedCompareExchange(&staleFollowsOtherMap, 0, 0), InterlockedCompareExchange(&restartsSynced, 0, 0),
                 InterlockedCompareExchange(&teardownCalls, 0, 0), InterlockedCompareExchange(&teardownEnemies, 0, 0));
    std::fprintf(log, "coop_checkpoint necro_reloads marked=%ld done=%ld expired=%ld failed=%ld waiting=%d due=%d\n", InterlockedCompareExchange(&necroReloadsMarked, 0, 0),
                 InterlockedCompareExchange(&necroReloads, 0, 0), InterlockedCompareExchange(&necroReloadsExpired, 0, 0),
                 InterlockedCompareExchange(&necroReloadsFailed, 0, 0), reloadAfterApply ? 1 : 0, reloadDue ? 1 : 0);
    if (InterlockedCompareExchange(&requestsKeptLocal, 0, 0) || InterlockedCompareExchange(&requestsIgnored, 0, 0))
        std::fprintf(log, "coop_checkpoint mp_map restarts kept_local=%ld requests_ignored=%ld\n", InterlockedCompareExchange(&requestsKeptLocal, 0, 0),
                     InterlockedCompareExchange(&requestsIgnored, 0, 0));
}
}  // namespace checkpoint
