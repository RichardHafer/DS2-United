// Dead Space 2 United by Richard Hafer.
// The player lifecycle in the match modes ([Lifecycle], 02.11.): survival, the clock modes,
// Versus, on the MP maps and in the campaign arenas. Included from runtime_coop_respawn.inc.
// Design and evidence: analysis/COOP_LIFECYCLE_MODES_2026-11-02.md.
//
// Four parts:
//  * Native placement. Player::Init (0x008A8270) moves every new Player without the MP bit to the
//    level's start checkpoint (0x008A84FA..0x008A8531: 0x00429B80, copy of checkpoint+0x60 by
//    0x00B75740, entity SetMatrix +0x2C, CMC::SetMatrix 0x00F72DC0). On an MP map that is the map
//    origin, the cause of every origin fall. A mid hook right after the lookup (0x008A8505, eax =
//    the checkpoint, ebx = 0) hands that code our spawn point instead, through a stand-in whose
//    +0x60 holds the matrix, so the body is put there by the game's own placement inside the
//    spawn. A later Init on a body we placed, or on our Isaac on an MP map, skips the move.
//  * The phase of this machine (off, loading, spawning, syncing, alive, dying, down, respawning,
//    game over, restarting, leaving), logged at every change with the time in the phase, with a
//    timeout per phase that says what is missing. Every machine sends its phase in an 18-byte
//    'LFCY' packet; the receive path is respawn::OnLifeBytes (same size as LIFE).
//  * The restart together: the host's restart (game over, a mode's result) is announced first;
//    every partner stops bringing downed players back and acknowledges; then the host restarts
//    (at most RestartAckMs later) and the partners follow through the checkpoint module.
//  * The join timeline: per TNL connection the ghosts this machine sends (in use, still pending)
//    and receives (players, enemies), the template check per partner, the wave state, from the
//    arrival until everything is in. A joiner that receives nothing asks the host for a re-ghost.
namespace mpmap { int RespawnMatrix(const float* toward, float* matrix, uint32_t* pointObject); void SendSpawnUsage(uint32_t point, int index); }
namespace matchend { int PhaseIndex(); }
namespace gamemode { bool CheckpointRestartNow(const char* why); }
namespace arena { void BeforeRestart(); }   // runtime_coop_arena.inc: the restarted arena's new Isaac is armed too
namespace lifecycle {
// runtime_coop_uniform.inc reads and writes these around its birth suit (it sits outside this namespace)
bool birthLoadOnly = false, birthPending = false;
uint32_t bornPlayer = 0;
bool enabled = false, nativePlacement = false, syncedRestart = false, joinTimeline = false, joinResync = false;
DWORD restartAckMs = 2500, restartTimeoutMs = 30000, spawnTimeoutMs = 8000, syncTimeoutMs = 15000, respawnTimeoutMs = 15000;

// --- native placement ---
constexpr uint32_t kMoveSiteRva = 0x4A8505, kMoveBlockRva = 0x4A84FA;
// The MP branch right before the block: Player+0x3D8 bit 13 (multiplayer) jumps over the whole
// checkpoint move (test cl,1 / je 0x008A84FA at 0x008A84C9). A host with a partner carries the bit,
// so its arena body never reached the move hook (02.11.: native on the client, curtain on the host).
// For an armed body the branch does its own work (the spawn time at +0x3F0) and then takes the
// move block, where the arm places it.
constexpr uint32_t kMpBranchRva = 0x4A84C9, kSpawnTimeField = 0x3F0;
safetyhook::MidHook mpBranchHook;
bool mpBranchOk = false;
volatile LONG mpBranchTaken = 0;
safetyhook::MidHook moveHook;
bool moveHookOk = false, moveSiteMatches = false;
alignas(16) unsigned char standIn[0xA0]{};   // a checkpoint as 0x00B75740 reads it: the matrix at +0x60
// loadOnly: armed before a level request, only a body made while that level loads takes it (never a
// bot spawned once it runs); keepFacing: the checkpoint's rotation, our position.
struct Want {
    bool armed = false;
    uint32_t player = 0;
    const char* why = "";
    DWORD at = 0;
    uint32_t placed = 0;
    bool loadOnly = false, keepFacing = false, haveFace = false;
    DWORD ttl = 0;
    float face[2]{};   // keepFacing: turned about the up axis until the forward row (+8/+10) points here
};
float lastFacing[2]{};   // the forward row Init got last, for the spawn lines
// Rows 0..2 turned about world Y by the angle from their forward row to `face`: a pure rotation,
// so the handedness stays the checkpoint's (a built matrix could come out mirrored).
void TurnRows(float* m, const float* face) {
    const float len = std::sqrt(face[0] * face[0] + face[1] * face[1]);
    if (!(len > 0.01f)) return;
    const float delta = std::atan2(face[1] / len, face[0] / len) - std::atan2(m[10], m[8]);
    const float c = std::cos(delta), s = std::sin(delta);
    for (int r = 0; r < 3; ++r) {
        const float x = m[r * 4], z = m[r * 4 + 2];
        m[r * 4] = x * c - z * s;
        m[r * 4 + 2] = x * s + z * c;
    }
}
Want want;
uint32_t placedBodies[8]{};
unsigned placedNext = 0;
volatile LONG initCalls = 0, placedNative = 0, movesSkipped = 0, movesKept = 0, ghostInits = 0, wantsMissed = 0, botMovesSkipped = 0;
unsigned moveLines = 0;
bool PlacedBefore(uint32_t player) {
    for (uint32_t b : placedBodies) if (b && b == player) return true;
    return false;
}
// A checkpoint restart makes the new Isaac while the level manager still reads RUNNING (02.11.
// logs: the arena restart's Init ran 0.5 s after the arm, init_calls +1 kept +1, the loading
// screen only after it), so a load-time arm also takes a body made while a restart is landing.
bool RestartBodyWindow();   // below, with the restart state
volatile LONG loadArmsRefused = 0, restartArmsTaken = 0;
// The caller chain through the frame pointers, for the first few lines: who sent this Init.
void Callers(uint32_t ebp, char* out, size_t size) {
    int used = 0;
    out[0] = 0;
    for (int depth = 0; depth < 7 && ebp; ++depth) {
        uint32_t next = 0, ret = 0;
        if (!Read(ebp, &next, 4) || !Read(ebp + 4, &ret, 4)) break;
        if (ret >= base + 0x1000 && ret < base + hits::kCodeEnd && used < static_cast<int>(size) - 12)
            used += std::snprintf(out + used, size - used, " %08X", ret);
        if (next <= ebp) break;
        ebp = next;
    }
}
bool WouldTake(uint32_t player);
void OnMpBranch(SafetyHookContext& ctx) {
    if (!(ctx.ecx & 1)) return;   // no MP bit: the move block runs anyway
    const uint32_t player = static_cast<uint32_t>(ctx.esi);
    if (!WouldTake(player)) return;
    // the MP branch's own write: settings +0x60 (or 2000 ms) plus the game clock
    uint32_t settings = 0, inner = 0, time = 0, clock = 0;
    if (!Read(base + 0x1C21624, &settings, 4) || !settings || !Read(settings + 0x0C, &inner, 4) || !inner || !Read(inner + 0x60, &time, 4)) time = 2000;
    Read(base + 0x1C4D3C0, &clock, 4);
    const uint32_t until = time + clock;
    WriteBytes(player + kSpawnTimeField, &until, 4);
    ctx.ecx &= ~1u;
    if (InterlockedIncrement(&mpBranchTaken) <= 20)
        gate::Event("lifecycle placement: Player::Init on %08X has the MP bit; the armed spawn takes the move block (spawn time %u written as the MP branch does)", player, until);
}
void OnCheckpointMove(SafetyHookContext& ctx) {
    InterlockedIncrement(&initCalls);
    const uint32_t player = static_cast<uint32_t>(ctx.esi), checkpoint = static_cast<uint32_t>(ctx.eax);
    if (!enabled || !nativePlacement || !player || play::StoryCoop()) return;   // never in story co-op
    if (IsGhostPlayer(player)) { InterlockedIncrement(&ghostInits); return; }
    const bool bot = IsBotEntity(player);
    if (want.armed && want.ttl && GetTickCount() - want.at > want.ttl) {
        want.armed = false;
        InterlockedIncrement(&wantsMissed);
        if (moveLines < 60) { ++moveLines; gate::Event("lifecycle placement: %s expired after %lu ms without a body", want.why, static_cast<unsigned long>(want.ttl)); }
    }
    const bool loading = mpmap::LmState(nullptr) != coop_mpmap::kRunning;
    const bool restartBody = RestartBodyWindow();
    const bool matches = want.armed && (want.player == player || (!want.player && !bot));
    if (matches && want.loadOnly && !loading && !restartBody) {
        InterlockedIncrement(&loadArmsRefused);
        if (moveLines < 60) {
            ++moveLines;
            gate::Event("lifecycle placement: Player::Init on %08X while the level runs and no restart lands: %s stays armed for a body of the load", player, want.why);
        }
    }
    if (matches && (!want.loadOnly || loading || restartBody)) {
        if (want.loadOnly && !loading) InterlockedIncrement(&restartArmsTaken);
        want.armed = false;
        want.placed = player;
        if (want.keepFacing) {
            float* m = reinterpret_cast<float*>(standIn + 0x60);
            float rows[12];
            if (checkpoint && Read(checkpoint + 0x60, rows, sizeof(rows)) && std::isfinite(rows[8]) && std::isfinite(rows[10]) &&
                std::fabs(rows[8] * rows[8] + rows[9] * rows[9] + rows[10] * rows[10] - 1.0f) < 0.1f) {
                std::memcpy(m, rows, sizeof(rows));
                m[3] = m[7] = m[11] = 0.0f;
            }
            if (want.haveFace) TurnRows(m, want.face);
        }
        lastFacing[0] = reinterpret_cast<const float*>(standIn + 0x60)[8];
        lastFacing[1] = reinterpret_cast<const float*>(standIn + 0x60)[10];
        placedBodies[placedNext++ % std::size(placedBodies)] = player;
        ctx.eax = static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(standIn));
        InterlockedIncrement(&placedNative);
        const float* m = reinterpret_cast<const float*>(standIn + 0x60);
        if (moveLines < 60) {
            ++moveLines;
            gate::Event("lifecycle placement: Player::Init puts %08X on our point (%.2f %.2f %.2f) facing (%.2f %.2f) for %s instead of the checkpoint %08X (%s)", player,
                        m[12], m[13], m[14], lastFacing[0], lastFacing[1], want.why, checkpoint,
                        loading ? "during the load" : restartBody ? "a restart is landing" : "the level runs");
        }
        return;
    }
    // A bot already stands where the bot code put it (next to its leader). A later Init on its
    // body (the dress after the spawn) moved it to the MP map's start checkpoint, the origin under
    // the map: all three bots fell after the arrival and after a restart (02.11.).
    if (bot && checkpoint && (mpmap::visit.phase == coop_mpmap::Phase::Arrived || arena::RunningNow())) {
        ctx.eax = 0;
        InterlockedIncrement(&botMovesSkipped);
        if (moveLines < 60) {
            ++moveLines;
            float at[3]{};
            Read(player + mpmap::kPlayerPos, at, sizeof(at));
            gate::Event("lifecycle placement: Player::Init on bot body %08X at (%.1f %.1f %.1f) would move it to the start checkpoint %08X: skipped", player, at[0], at[1],
                        at[2], checkpoint);
        }
        return;
    }
    // Our own body again, or our Isaac on an MP map: the map's start checkpoint is its origin.
    const bool ours = PlacedBefore(player) || player == mpmap::ourPlayer || player == LocalCampaignPlayer();
    if (!bot && ours && mpmap::visit.phase == coop_mpmap::Phase::Arrived && checkpoint) {
        ctx.eax = 0;
        InterlockedIncrement(&movesSkipped);
        if (moveLines < 60) {
            ++moveLines;
            char callers[96];
            Callers(static_cast<uint32_t>(ctx.ebp), callers, sizeof(callers));
            float at[3]{};
            Read(player + mpmap::kPlayerPos, at, sizeof(at));
            gate::Event("lifecycle placement: a later Player::Init on %08X at (%.1f %.1f %.1f) would move it to the start checkpoint %08X: skipped (callers%s)",
                        player, at[0], at[1], at[2], checkpoint, callers);
        }
        return;
    }
    InterlockedIncrement(&movesKept);
    static unsigned keptLines = 0;
    if (keptLines < 24) {
        ++keptLines;
        gate::Event("lifecycle placement: Player::Init on %08X (bot %d) keeps the move to checkpoint %08X (arm: %s%s, level manager %s, restart %s)", player, bot ? 1 : 0,
                    checkpoint, want.armed ? want.why : "none", want.armed && want.player && want.player != player ? ", for another body" : "",
                    loading ? "loading" : "running", restartBody ? "landing" : "none");
    }
}
bool WouldTake(uint32_t player) {
    if (!enabled || !nativePlacement || !moveHookOk || !player || play::StoryCoop() || IsGhostPlayer(player) || !want.armed) return false;
    if (want.ttl && GetTickCount() - want.at > want.ttl) return false;
    const bool bot = IsBotEntity(player);
    if (!(want.player == player || (!want.player && !bot))) return false;
    const bool loading = mpmap::LmState(nullptr) != coop_mpmap::kRunning;
    return !want.loadOnly || loading || RestartBodyWindow();
}
// Arm before a spawn call that runs Player::Init: player 0 = the next new local Player.
void ArmPlacement(uint32_t player, const float* matrix, const char* why) {
    if (!enabled || !nativePlacement || !moveHookOk || !matrix) return;
    float m[16];
    std::memcpy(m, matrix, sizeof(m));
    m[3] = m[7] = m[11] = 0.0f;
    m[15] = 1.0f;
    std::memcpy(standIn + 0x60, m, sizeof(m));
    want = Want{true, player, why, GetTickCount(), 0, false, false, 0};
}
// Armed before a level request: the first local Isaac that level makes, within ttl ms.
bool ArmLoadPlacement(const float* matrix, const char* why, bool keepFacing, DWORD ttl, const float* face = nullptr) {
    if (!enabled || !nativePlacement || !moveHookOk || !matrix) return false;
    ArmPlacement(0, matrix, why);
    want.loadOnly = true;
    want.keepFacing = keepFacing;
    if (face) { want.face[0] = face[0]; want.face[1] = face[1]; want.haveFace = true; }
    want.ttl = ttl;
    return true;
}
// The body a load-time arm placed (0 none yet); the arm itself stays until taken or cancelled.
uint32_t LoadPlaced(const char* why) { return !want.armed && want.why == why ? want.placed : 0; }
bool LoadArmed(const char* why) { return want.armed && want.why == why; }
void CancelPlacement(const char* why) {
    if (want.armed && moveLines < 60) { ++moveLines; gate::Event("lifecycle placement: %s cancelled (%s)", want.why, why); }
    want = Want{};
}
// After the call: the body the game placed for us, 0 when Init never asked.
uint32_t DisarmPlacement() {
    if (want.armed) {
        InterlockedIncrement(&wantsMissed);
        if (moveLines < 60) { ++moveLines; gate::Event("lifecycle placement: %s ran no Player::Init through the hook; the old placement follows", want.why); }
    }
    want.armed = false;
    const uint32_t placed = want.placed;
    want.placed = 0;
    return placed;
}
bool PlacementReady() { return enabled && nativePlacement && moveHookOk; }
// gamemode::CheckpointRestartNow sets it around its own call, so the restart hook lets it through.
bool restartReleasing = false;

// --- partners ---
enum Phase : uint8_t { Off, Loading, Spawning, Syncing, Alive, Dying, Down, Respawning, GameOver, Restarting, Leaving, PhaseCount };
const char* const kPhaseNames[] = {"off", "loading", "spawning", "syncing", "alive", "dying", "down", "respawning", "game_over", "restarting", "leaving"};
const char* PhaseName(unsigned p) { return p < PhaseCount ? kPhaseNames[p] : "?"; }
constexpr uint32_t kMagic = 0x5943464Cu;   // 'LFCY'
constexpr uint8_t kVersion = 1;
enum Kind : uint8_t { kStatus = 0, kPrepare = 1, kAck = 2, kGo = 3, kResync = 4, kCancel = 5 };
#pragma pack(push, 1)
struct Packet { uint32_t magic; uint8_t version, machine, phase, kind; uint32_t token; uint16_t seq, generation, enemies; };
#pragma pack(pop)
static_assert(sizeof(Packet) == 18, "the lifecycle packet rides on the LIFE size");
struct Partner { uint8_t phase = Off; DWORD at = 0, phaseAt = 0; uint16_t seq = 0, generation = 0, enemies = 0, acked = 0; bool seen = false; };
Partner partners[16]{};
uint16_t seq = 0;
volatile LONG packetsOut = 0, packetsIn = 0, packetsRejected = 0, partnerChanges = 0;
unsigned partnerLines = 0;
bool Fresh(const Partner& p, DWORD now) { return p.at && now - p.at <= 3000 + netclock::OneWay(); }
// For the disconnect log: what a partner was doing when he went silent.
void PartnerLastPhase(unsigned machine, char* out, size_t size) {
    if (machine >= 16 || !partners[machine].seen) { std::snprintf(out, size, "never reported a phase"); return; }
    const Partner& p = partners[machine];
    std::snprintf(out, size, "last phase %s, heard %lu ms ago", PhaseName(p.phase), static_cast<unsigned long>(GetTickCount() - p.at));
}
unsigned FreshPartners(DWORD now) {
    unsigned n = 0;
    for (unsigned m = 0; m < 16; ++m) if (m != machineIndex && Fresh(partners[m], now)) ++n;
    return n;
}
void Send(uint8_t kind, uint8_t phase, uint16_t generation, uint16_t enemies) {
    if (!network) return;
    Packet p{kMagic, kVersion, static_cast<uint8_t>(machineIndex), phase, kind, peerReadiness.token, ++seq, generation, enemies};
    if (SendToPeers(&p, sizeof(p)) > 0) InterlockedIncrement(&packetsOut);
}

// --- the restart together ---
enum class Restart { None, Preparing, Asked, Prepared, Landed };
const char* const kRestartNames[] = {"none", "preparing", "asked", "prepared", "landed"};
Restart restart = Restart::None;
uint16_t generation = 0;
DWORD restartAt = 0, restartSentAt = 0, landedAt = 0;
unsigned landedEpoch = 0, landedCount = 0;
LONG landedArenaArrivals = 0;
char restartWhy[64] = "";
volatile LONG restartsSynced = 0, restartsAlone = 0, restartAckTimeouts = 0, restartRefused = 0, restartsFollowed = 0, restartTimeouts = 0, restartsUnprepared = 0,
              holdsKept = 0;
void SetRestart(Restart r, DWORD now, const char* why) {
    if (r == restart) return;
    gate::Event("lifecycle restart %u: %s -> %s after %lu ms (%s)", generation, kRestartNames[static_cast<int>(restart)], kRestartNames[static_cast<int>(r)],
                static_cast<unsigned long>(restartAt ? now - restartAt : 0), why);
    restart = r;
    restartAt = Stamp(now);
    restartSentAt = 0;
}
bool RestartBodyWindow() { return enabled && restart == Restart::Landed && GetTickCount() - restartAt < restartTimeoutMs; }
// Downed players wait for the reload instead of coming back into the old world.
bool HoldReturns() {
    if (!enabled || !syncedRestart) return false;
    if (restart != Restart::None) return true;
    // the host is already restarting (its announcement may be lost)
    const Partner& host = partners[0];
    return machineIndex != 0 && Fresh(host, GetTickCount()) && host.phase == Restarting;
}
// gamemode::PollRestart: true when the restart is ours to run now.
// The checkpoint module's restart hook: a host's own restart in a match mode with partners (the
// pause menu's) is announced first and run by BeginRestart; not the death path, not our own call.
bool WantsSyncedRestart(bool fromDeathPath) {
    if (!enabled || !syncedRestart || restartReleasing || fromDeathPath || !IsHost() || restart != Restart::None) return false;
    if (!mpmap::InVisit() && !arena::RunningNow()) return false;
    return FreshPartners(GetTickCount()) > 0;
}
bool BeginRestart(const char* why) {
    const DWORD now = GetTickCount();
    if (!enabled || !syncedRestart || !IsHost()) return false;
    std::snprintf(restartWhy, sizeof(restartWhy), "%s", why ? why : "?");
    if (++generation == 0) ++generation;
    const unsigned fresh = FreshPartners(now);
    if (!fresh) {
        InterlockedIncrement(&restartsAlone);
        gate::Event("lifecycle restart %u asked (%s): no partner in the session, restarting now", generation, restartWhy);
        const bool ok = gamemode::CheckpointRestartNow(restartWhy);
        if (!ok) { InterlockedIncrement(&restartRefused); return true; }
        SetRestart(Restart::Asked, now, "solo");
        return true;
    }
    gate::Event("lifecycle restart %u asked (%s): %u partner(s) are told first, at most %lu ms", generation, restartWhy, fresh, static_cast<unsigned long>(restartAckMs));
    SetRestart(Restart::Preparing, now, "announced");
    return true;
}
// respawn::OnRestart (the checkpoint module's restart hook): the world is being rebuilt here.
void OnRestartLanded() {
    if (!enabled) return;
    const DWORD now = GetTickCount();
    landedAt = Stamp(now);
    ++landedCount;
    landedEpoch = mpmap::visit.epochs;
    landedArenaArrivals = InterlockedCompareExchange(&arena::readyArrivals, 0, 0);
    arena::BeforeRestart();
    if (restart == Restart::None) {
        InterlockedIncrement(&restartsUnprepared);
        gate::Event("lifecycle restart: a checkpoint restart without an announcement (the pause menu, a death fallback or a lost packet)");
    } else if (restart == Restart::Prepared) {
        InterlockedIncrement(&restartsFollowed);
    }
    SetRestart(Restart::Landed, now, "the checkpoint restart runs here");
}
void RestartTick(DWORD now, bool bodyStands) {
    switch (restart) {
    case Restart::None: return;
    case Restart::Preparing: {
        if (now - restartSentAt >= 150) { restartSentAt = now; Send(kPrepare, Restarting, generation, 0); }
        unsigned fresh = 0, acked = 0;
        char missing[48] = "";
        int used = 0;
        for (unsigned m = 0; m < 16; ++m) {
            if (m == machineIndex || !Fresh(partners[m], now)) continue;
            ++fresh;
            if (partners[m].acked == generation) ++acked;
            else if (used < 40) used += std::snprintf(missing + used, sizeof(missing) - used, " m%u", m);
        }
        const bool all = acked >= fresh, late = now - restartAt >= restartAckMs;
        if (!all && !late) return;
        if (all) InterlockedIncrement(&restartsSynced);
        else InterlockedIncrement(&restartAckTimeouts);
        gate::Event("lifecycle restart %u: %u of %u partner(s) ready after %lu ms%s%s; the map restarts", generation, acked, fresh,
                    static_cast<unsigned long>(now - restartAt), all ? "" : ", no answer from", missing);
        if (!gamemode::CheckpointRestartNow(restartWhy)) {
            InterlockedIncrement(&restartRefused);
            for (int i = 0; i < 3; ++i) Send(kCancel, Alive, generation, 0);
            SetRestart(Restart::None, now, "CheckpointRestart REFUSED: downed players come back by the old rules");
            return;
        }
        SetRestart(Restart::Asked, now, "CheckpointRestart accepted");
        for (int i = 0; i < 3; ++i) Send(kGo, Restarting, generation, 0);
        return;
    }
    case Restart::Prepared:
        if (now - restartSentAt >= 150 && now - restartAt < 4000) { restartSentAt = now; Send(kAck, Restarting, generation, 0); }
        [[fallthrough]];
    case Restart::Asked:
        if (now - restartAt >= restartTimeoutMs) {
            InterlockedIncrement(&restartTimeouts);
            SetRestart(Restart::None, now, "TIMEOUT: no checkpoint restart came; holds released");
        }
        return;
    case Restart::Landed: {
        const bool newArrival = mpmap::visit.epochs != landedEpoch || InterlockedCompareExchange(&arena::readyArrivals, 0, 0) != landedArenaArrivals;
        if (newArrival && bodyStands) {
            gate::Event("lifecycle restart %u: our new Isaac stands %lu ms after the restart landed", generation, static_cast<unsigned long>(now - landedAt));
            SetRestart(Restart::None, now, "back on the map");
        } else if (now - restartAt >= restartTimeoutMs) {
            InterlockedIncrement(&restartTimeouts);
            SetRestart(Restart::None, now, "TIMEOUT: no new Isaac after the restart");
        }
        return;
    }
    }
}

// --- the join timeline ---
struct LinkView { uint32_t connection = 0; unsigned machine = 0xFF, slot = 0xFF; bool ghosting = false; int sending = 0, pending = 0, inPlayers = 0, inEnemies = 0, inOther = 0; };
LinkView links[8]{};
unsigned linkCount = 0;
int ClassOfProxy(uint32_t proxy) {   // 0 other, 1 player, 2 enemy
    uint32_t vt = 0, info = 0;
    char name[16]{};
    if (!Read(proxy, &vt, 4) || vt != base + kNetProxyVtable || !Read(proxy + 0x44, &info, 4) || !info || !Read(info + 0x20, name, sizeof(name) - 1)) return 0;
    if (std::strncmp(name, "DeadSpaceNPC", 12) == 0) return 2;
    if (std::strcmp(name, "Player") == 0 || std::strcmp(name, "NecroPlayer") == 0) return 1;
    return 0;
}
void ReadLinks(bool countIncoming) {
    linkCount = 0;
    uint32_t iface = 0, count = 0, array = 0;
    if (!session || !Read(session + kSessionInterface, &iface, 4) || !iface || !Read(iface + kInterfaceConnectionCount, &count, 4) ||
        !Read(iface + kInterfaceConnectionArray, &array, 4))
        return;
    for (uint32_t i = 0; i < count && i < 16 && linkCount < std::size(links); ++i) {
        uint32_t c = 0, state = 0;
        if (!Read(array + 4 * i, &c, 4) || !c || !Read(c + kConnectionState, &state, 4) || state != 7) continue;
        LinkView& l = links[linkCount++];
        l = LinkView{};
        l.connection = c;
        l.slot = templates::SlotOfConnection(c);
        l.machine = l.slot < coop_peers::kMaxPeers ? peerTable.peers[l.slot].machine : 0xFF;
        unsigned char ghosting = 0;
        uint32_t ghostArray = 0, locals = 0;
        Read(c + 0x204, &ghosting, 1);
        l.ghosting = ghosting != 0;
        if (Read(c + 0x1F8, &ghostArray, 4) && ghostArray) { Read(c + 0x200, &l.sending, 4); Read(c + 0x1FC, &l.pending, 4); }
        if (countIncoming && Read(c + 0x20C, &locals, 4) && locals) {
            static uint32_t ghosts[1024];
            if (!Read(locals, ghosts, sizeof(ghosts))) continue;
            unsigned classified = 0;
            for (uint32_t g : ghosts) {
                if (!g) continue;
                if (++classified > 384) break;
                switch (ClassOfProxy(g)) {
                case 1: ++l.inPlayers; break;
                case 2: ++l.inEnemies; break;
                default: ++l.inOther; break;
                }
            }
        }
    }
}
// The host's enemies that TNL can ghost (a DeadSpaceNPC with a proxy), for the partners' check.
int HostEnemyCount() {
    uint32_t node = 0;
    if (!Read(base + 0x1C5B744, &node, 4)) return -1;
    int n = 0;
    for (unsigned steps = 0; node && steps < 4096; ++steps) {
        uint32_t proxy = 0, next = 0;
        if (Read(node + 0xC, &proxy, 4) && proxy && !guidfix::IsGhostEntity(node - 0x5C) && npcs::IsNpcReflectable(node)) ++n;
        if (!Read(node + 4, &next, 4) || next == node) break;
        node = next;
    }
    return n;
}
// Milestones of this machine's arrival, in ms after it (0 = not yet).
struct Join {
    unsigned epoch = 0;
    DWORD arrivedAt = 0, visitStartedAt = 0;
    DWORD body = 0, session = 0, connected = 0, ghosting = 0, firstGhost = 0, players = 0, firstEnemy = 0, enemies = 0, wave = 0, complete = 0;
    unsigned resyncs = 0;
    DWORD lastLog = 0, resyncAt = 0;
    bool timedOut = false, logged = false;
    const char* waveFrom = "";
};
Join join;
volatile LONG joinsComplete = 0, joinsTimedOut = 0, resyncAsks = 0, resyncServed = 0;
DWORD lastLinkRead = 0, lastHostCount = 0;
int hostEnemies = -1;   // the host's count: our own on the host, the host's LFCY figure on a client
int MarkMs(DWORD& field, DWORD now, bool cond) {
    if (!field && cond) field = Stamp(now - join.arrivedAt);
    return static_cast<int>(field);
}
void JoinLine(const char* what, DWORD now) {
    int sending = 0, pending = 0, players = 0, enemies = 0;
    for (unsigned i = 0; i < linkCount; ++i) { sending += links[i].sending; pending += links[i].pending; players += links[i].inPlayers; enemies += links[i].inEnemies; }
    gate::Event("lifecycle join %s at +%lu ms (arrival %u): body=%lu session=%lu connected=%lu ghosting=%lu first_ghost=%lu players=%lu first_enemy=%lu enemies=%lu "
                "wave=%lu(%s) | links=%u in players=%d enemies=%d host_enemies=%d | out ghosts=%d pending=%d resyncs=%u",
                what, static_cast<unsigned long>(now - join.arrivedAt), join.epoch, join.body, join.session, join.connected, join.ghosting, join.firstGhost,
                join.players, join.firstEnemy, join.enemies, join.wave, join.waveFrom, linkCount, players, enemies, hostEnemies, sending, pending, join.resyncs);
}
// Host side: one line per partner link every 2 s while its first ghosts are still going out.
struct LinkWatch { uint32_t connection = 0; DWORD seenAt = 0, doneAt = 0, lastLog = 0, quietSince = 0; int peakPending = 0; };
LinkWatch watches[8]{};
volatile LONG linksSynced = 0;
void WatchLinks(DWORD now) {
    for (unsigned i = 0; i < linkCount; ++i) {
        const LinkView& l = links[i];
        LinkWatch* w = nullptr;
        for (auto& x : watches) if (x.connection == l.connection) { w = &x; break; }
        if (!w) {
            for (auto& x : watches) if (!x.connection || now - x.seenAt > 600000) { w = &x; break; }
            if (!w) continue;
            *w = LinkWatch{l.connection, Stamp(now), 0, 0, 0, 0};
            gate::Event("lifecycle link to machine %u (connection %08X) up: ghosting=%d sending=%d pending=%d", l.machine, l.connection, l.ghosting ? 1 : 0, l.sending,
                        l.pending);
        }
        if (w->doneAt) continue;
        if (l.pending > w->peakPending) w->peakPending = l.pending;
        if (l.ghosting && l.sending > 0 && l.pending <= 2) {
            if (!w->quietSince) w->quietSince = Stamp(now);
            if (now - w->quietSince >= 1000) {
                w->doneAt = Stamp(now);
                InterlockedIncrement(&linksSynced);
                long ok = 0, held = 0, refused = 0;
                unsigned waiting = 0;
                templates::PeerStats(l.slot, ok, held, refused, waiting);
                gate::Event("lifecycle link to machine %u: first ghosts all out %lu ms after the link came up (%d ghosts, pending peak %d; templates matched %ld refused %ld "
                            "waiting %u)",
                            l.machine, static_cast<unsigned long>(now - w->seenAt), l.sending, w->peakPending, ok, refused, waiting);
            }
        } else {
            w->quietSince = 0;
        }
        if (!w->doneAt && now - w->lastLog >= 2000 && now - w->seenAt <= 60000) {
            w->lastLog = now;
            long ok = 0, held = 0, refused = 0;
            unsigned waiting = 0;
            templates::PeerStats(l.slot, ok, held, refused, waiting);
            gate::Event("lifecycle link to machine %u at +%lu ms: ghosting=%d sending=%d pending=%d | templates matched %ld refused %ld held %ld waiting %u | host enemies %d",
                        l.machine, static_cast<unsigned long>(now - w->seenAt), l.ghosting ? 1 : 0, l.sending, l.pending, ok, refused, held, waiting, hostEnemies);
        }
    }
}
void JoinTick(DWORD now, bool onMap, unsigned arrivalEpoch, bool bodyStands) {
    if (!joinTimeline) return;
    if (IsHost() && network && now - lastHostCount >= 1000) { lastHostCount = now; hostEnemies = HostEnemyCount(); }
    const bool watching = onMap && (join.epoch == arrivalEpoch) && !join.complete && !join.timedOut;
    if (onMap && join.epoch != arrivalEpoch) {
        join = Join{};
        join.epoch = arrivalEpoch;
        join.arrivedAt = Stamp(now);
        const DWORD start = static_cast<DWORD>(InterlockedCompareExchange(&mpmap::visitStartTick, 0, 0));
        join.visitStartedAt = start ? now - start : 0;
        gate::Event("lifecycle join: arrival %u, %lu ms after the visit started; watching the first sync", arrivalEpoch,
                    static_cast<unsigned long>(join.visitStartedAt));
    }
    if (network && now - lastLinkRead >= (watching ? 250u : 2000u)) {
        lastLinkRead = now;
        ReadLinks(watching || !IsHost());
        if (IsHost()) WatchLinks(now);
    }
    if (!watching) return;
    int inPlayers = 0, inEnemies = 0, inAny = 0;
    bool anyGhosting = false;
    for (unsigned i = 0; i < linkCount; ++i) {
        inPlayers += links[i].inPlayers;
        inEnemies += links[i].inEnemies;
        inAny += links[i].inPlayers + links[i].inEnemies + links[i].inOther;
        anyGhosting = anyGhosting || links[i].ghosting;
    }
    const unsigned fresh = FreshPartners(now);
    MarkMs(join.body, now, bodyStands);
    MarkMs(join.session, now, session != 0);
    MarkMs(join.connected, now, linkCount > 0);
    MarkMs(join.ghosting, now, anyGhosting);
    MarkMs(join.firstGhost, now, inAny > 0);
    const unsigned expect = fresh ? fresh : peerTable.Count();   // partners on an older build send no LFCY
    MarkMs(join.players, now, expect > 0 && inPlayers >= static_cast<int>(expect));
    MarkMs(join.firstEnemy, now, inEnemies > 0);
    const bool host = IsHost();
    const bool enemiesIn = host || hostEnemies == 0 || (hostEnemies > 0 && inEnemies * 5 >= hostEnemies * 4);
    MarkMs(join.enemies, now, enemiesIn && (host || hostEnemies >= 0));
    if (!join.wave) {
        const bool fromMusic = InterlockedCompareExchange(&survmusic::state, 0, 0) != 0;
        const bool fromHost = !host && respawn::others[0].at && now - respawn::others[0].at < 3000;
        if (host || fromMusic || fromHost) { MarkMs(join.wave, now, true); join.waveFrom = host ? "own director" : fromMusic ? "survnet" : "host life packet"; }
    }
    const bool solo = !network || !peerTable.Count();
    const bool done = join.body && (solo || (join.connected && join.players && join.enemies && join.wave));
    if (done) {
        join.complete = Stamp(now - join.arrivedAt);
        InterlockedIncrement(&joinsComplete);
        JoinLine(solo ? "complete (no partner)" : "COMPLETE", now);
        // the partners' suits as this machine sees them
        if (!solo) {
            uint32_t players[8]{};
            const unsigned n = nameplate::Targets(players, 8);
            for (unsigned i = 0; i < n; ++i) {
                if (!IsGhostPlayer(players[i])) continue;
                const int worn = bot::WornSuitIndex(players[i]);
                uint32_t body[3]{};
                gate::Event("lifecycle join: partner machine %u's Isaac %08X wears the %s, body %s", MachineOf(players[i]), players[i],
                            worn >= 0 ? bot::SuitName(worn) : "(no registered suit)", coop_uniform::BotBody(players[i], body) ? "complete" : "INCOMPLETE");
            }
        }
        return;
    }
    if (now - join.arrivedAt >= syncTimeoutMs) {
        join.timedOut = true;
        InterlockedIncrement(&joinsTimedOut);
        JoinLine("TIMEOUT", now);
        gate::Event("lifecycle join: missing after %lu ms:%s%s%s%s%s%s", static_cast<unsigned long>(syncTimeoutMs), join.body ? "" : " our body,",
                    join.connected ? "" : " a TNL link,", join.ghosting || !linkCount ? "" : " ghosting on the link,", join.players ? "" : " the partners' Isaacs,",
                    join.enemies ? "" : " the host's enemies,", join.wave ? "" : " the wave state");
        return;
    }
    if (now - join.lastLog >= 2000) { join.lastLog = now; JoinLine("progress", now); }
    // A joiner with a link but no ghosts at all, or none of the host's enemies, asks for a re-ghost.
    if (joinResync && !host && join.connected && join.resyncs < 3 && now - join.arrivedAt >= 6000 && (!join.resyncAt || now - join.resyncAt >= 8000) &&
        (inAny == 0 || (hostEnemies > 0 && inEnemies == 0))) {
        join.resyncAt = Stamp(now);
        ++join.resyncs;
        InterlockedIncrement(&resyncAsks);
        for (int i = 0; i < 3; ++i) Send(kResync, Syncing, generation, static_cast<uint16_t>(inEnemies));
        gate::Event("lifecycle join: %d ghosts in, %d of the host's %d enemies, %lu ms after the arrival: asking the host to ghost again (%u)", inAny, inEnemies,
                    hostEnemies, static_cast<unsigned long>(now - join.arrivedAt), join.resyncs);
    }
}

// --- this machine's phase ---
uint8_t phase = Off;
DWORD phaseAt = 0, statusSentAt = 0, timeoutLogged = 0;
volatile LONG phaseChanges = 0, phaseTimeouts = 0;
volatile LONG phaseCounts[PhaseCount]{};
unsigned phaseLines = 0;
uint8_t Observe(bool& bodyStands, bool& onMap, unsigned& arrivalEpoch) {
    bodyStands = false;
    onMap = false;
    arrivalEpoch = 0;
    if (matchend::PhaseIndex() != 0) return Leaving;
    const bool visit = mpmap::InVisit(), arenaRun = arena::RunningNow();
    if (!visit && !arenaRun) return Off;
    if (visit && mpmap::visit.phase == coop_mpmap::Phase::Returning) return Leaving;
    if (restart != Restart::None) {
        const uint32_t local = LocalCampaignPlayer();
        bodyStands = local && checkpoint::LocalIsaacAlive();
        return Restarting;
    }
    if ((visit && mpmap::visit.phase != coop_mpmap::Phase::Arrived) || mpmap::LmState(nullptr) != coop_mpmap::kRunning) return Loading;
    onMap = true;
    arrivalEpoch = visit ? mpmap::visit.epochs : 0x10000u + static_cast<unsigned>(InterlockedCompareExchange(&arena::starts, 0, 0)) * 256u + landedCount;
    const uint32_t local = LocalCampaignPlayer();
    const bool alive = local && checkpoint::LocalIsaacAlive();
    bodyStands = alive;
    if (respawn::stage >= 2) return Respawning;
    if (respawn::stage == 1) return InterlockedCompareExchange(&survmusic::state, 0, 0) == coop_respawn::GameOver ? GameOver : Down;
    if (respawn::dyingPlayer) return Dying;
    if (necro::LocalRole() != 0) return Alive;   // a Versus necro or a watcher
    if (!local) return !visit || !mpmap::playerDone ? Spawning : Down;
    if (!alive) return Dying;
    if (joinTimeline && network && !join.complete && !join.timedOut && join.epoch == arrivalEpoch) return Syncing;
    return Alive;
}
void PhaseTimeouts(DWORD now) {
    const DWORD in = now - phaseAt;
    DWORD limit = 0;
    const char* what = "";
    switch (phase) {
    case Spawning: limit = spawnTimeoutMs; what = "no body of ours on the map (see the mpmap spawn player lines)"; break;
    case Respawning: limit = respawnTimeoutMs; what = "the respawn did not finish (see the respawn: lines; the stage ladder falls back on its own)"; break;
    case Loading: limit = 120000; what = "the load did not end (loading sync, streams or the level manager)"; break;
    case Dying: limit = 20000; what = "health 0 but no ProcessDeath (death sequence stuck)"; break;
    default: return;
    }
    if (in < limit || timeoutLogged == phaseAt) return;
    timeoutLogged = phaseAt;
    InterlockedIncrement(&phaseTimeouts);
    gate::Event("lifecycle TIMEOUT: %s for %lu ms: %s (respawn stage %d, slot0 %08X, visit phase %d, playerDone %d)", PhaseName(phase), static_cast<unsigned long>(in), what,
                respawn::stage, LocalSlotZeroEntity(), static_cast<int>(mpmap::visit.phase), mpmap::playerDone ? 1 : 0);
}
void Tick(DWORD now) {
    if (!enabled) return;
    bool bodyStands = false, onMap = false;
    unsigned arrivalEpoch = 0;
    const uint8_t seen = Observe(bodyStands, onMap, arrivalEpoch);
    if (seen != phase) {
        InterlockedIncrement(&phaseChanges);
        InterlockedIncrement(&phaseCounts[seen]);
        if (phaseLines < 400) {
            ++phaseLines;
            gate::Event("lifecycle m%u: %s -> %s after %lu ms (respawn stage %d, survival %ld, restart %s, partners %u)", machineIndex, PhaseName(phase), PhaseName(seen),
                        static_cast<unsigned long>(phaseAt ? now - phaseAt : 0), respawn::stage, InterlockedCompareExchange(&survmusic::state, 0, 0),
                        kRestartNames[static_cast<int>(restart)], FreshPartners(now));
        }
        phase = seen;
        phaseAt = Stamp(now);
        statusSentAt = 0;   // say it at once
    }
    PhaseTimeouts(now);
    RestartTick(now, bodyStands);
    JoinTick(now, onMap, arrivalEpoch, bodyStands);
    if (network && phase != Off && (!statusSentAt || now - statusSentAt >= 1000 || (now - phaseAt < 400 && now - statusSentAt >= 100))) {
        statusSentAt = Stamp(now);
        const int enemies = IsHost() ? hostEnemies : [] { int n = 0; for (unsigned i = 0; i < linkCount; ++i) n += links[i].inEnemies; return n; }();
        Send(kStatus, phase, generation, static_cast<uint16_t>(enemies < 0 ? 0xFFFF : enemies > 0xFFFE ? 0xFFFE : enemies));
    }
}

bool OnBytes(const void* data, int size, DWORD now) {
    if (size != static_cast<int>(sizeof(Packet))) return false;
    Packet p{};
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != kMagic) return false;
    if (!enabled) return true;
    if (p.version != kVersion || !KnownPeerToken(p.token) || p.machine >= 16 || p.machine == machineIndex || p.phase >= PhaseCount) {
        InterlockedIncrement(&packetsRejected);
        return true;
    }
    Partner& o = partners[p.machine];
    const uint16_t ahead = static_cast<uint16_t>(p.seq - o.seq);
    if (o.at && now - o.at < 10000 && (ahead == 0 || ahead > 0x8000u)) return true;   // a copy (direct and relayed) or older
    InterlockedIncrement(&packetsIn);
    o.at = Stamp(now);
    o.seq = p.seq;
    if (p.kind == kStatus) {
        if (!o.seen || o.phase != p.phase) {
            InterlockedIncrement(&partnerChanges);
            if (partnerLines < 300) {
                ++partnerLines;
                gate::Event("lifecycle partner m%u: %s -> %s (generation %u, enemies %u)", p.machine, o.seen ? PhaseName(o.phase) : "unknown", PhaseName(p.phase),
                            p.generation, p.enemies);
            }
            o.phase = p.phase;
            o.phaseAt = Stamp(now);
            o.seen = true;
        }
        if (p.machine == 0 && p.enemies != 0xFFFF && !IsHost()) hostEnemies = p.enemies;
        o.enemies = p.enemies;
        return true;
    }
    if (p.kind == kAck) { o.acked = p.generation; return true; }
    if (p.kind == kResync) {
        if (!IsHost()) return true;
        auto* peer = peerTable.FindByToken(p.token);
        const bool served = peer && reghost::ForceReset(PeerSlot(peer), "the joiner sees no ghosts");
        if (served) InterlockedIncrement(&resyncServed);
        gate::Event("lifecycle machine %u asks for a re-ghost (%u enemies on his side, %d here): %s", p.machine, p.enemies, hostEnemies,
                    served ? "reset+activated" : "not done (no connection, cooldown or reghost off)");
        return true;
    }
    if (p.machine != 0) return true;   // prepare and go are the host's
    if (p.kind == kPrepare && syncedRestart) {
        if (p.generation != generation || restart == Restart::None) {
            generation = p.generation;
            if (restart == Restart::None || restart == Restart::Landed) {
                SetRestart(Restart::Prepared, now, "the host announced its restart");
                InterlockedIncrement(&holdsKept);
            }
        }
        return true;
    }
    if (p.kind == kGo) {
        if (restart == Restart::Prepared && p.generation == generation) gate::Event("lifecycle restart %u: the host restarted; following", generation);
        return true;
    }
    if (p.kind == kCancel) {
        if (restart == Restart::Prepared && p.generation == generation) SetRestart(Restart::None, now, "the host's restart was refused there");
        return true;
    }
    return true;
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Lifecycle", "Enabled", 1, ini.c_str()) != 0;
    nativePlacement = GetPrivateProfileIntA("Lifecycle", "NativePlacement", 1, ini.c_str()) != 0;
    syncedRestart = GetPrivateProfileIntA("Lifecycle", "SyncedRestart", 1, ini.c_str()) != 0;
    joinTimeline = GetPrivateProfileIntA("Lifecycle", "JoinTimeline", 1, ini.c_str()) != 0;
    joinResync = GetPrivateProfileIntA("Lifecycle", "JoinResync", 1, ini.c_str()) != 0;
    templates::sticky = enabled && GetPrivateProfileIntA("Lifecycle", "StickyTemplates", 1, ini.c_str()) != 0;
    auto ms = [&](const char* key, DWORD def, DWORD lo, DWORD hi) {
        const DWORD v = static_cast<DWORD>(GetPrivateProfileIntA("Lifecycle", key, static_cast<int>(def), ini.c_str()));
        return v < lo || v > hi ? def : v;
    };
    restartAckMs = ms("RestartAckMs", 2500, 0, 10000);
    restartTimeoutMs = ms("RestartTimeoutMs", 30000, 5000, 180000);
    spawnTimeoutMs = ms("SpawnTimeoutMs", 8000, 2000, 60000);
    syncTimeoutMs = ms("JoinSyncTimeoutMs", 15000, 3000, 120000);
    respawnTimeoutMs = ms("RespawnTimeoutMs", 15000, 3000, 120000);
    if (!enabled) {
        nativePlacement = syncedRestart = joinTimeline = joinResync = false;
        std::fprintf(log, "lifecycle off ([Lifecycle] Enabled=0): the old spawn, restart and template paths run\n");
        return false;
    }
    if (nativePlacement) {
        // mov ecx,[LevelManager] / call 0x00402658 / cmp eax,ebx / je +2D / lea edx,[esp+40h] / push edx / mov ecx,eax / call 0x00B75740
        static const unsigned char kBlock[] = {0x8B, 0x0D, 0xA4, 0xC0, 0x01, 0x02, 0xE8, 0x53, 0xA1, 0xB5, 0xFF, 0x3B, 0xC3, 0x74, 0x2D,
                                               0x8D, 0x54, 0x24, 0x40, 0x52, 0x8B, 0xC8, 0xE8, 0x2B, 0xD2, 0x2C, 0x00};
        unsigned char have[sizeof(kBlock)]{};
        moveSiteMatches = Read(base + kMoveBlockRva, have, sizeof(have)) && std::memcmp(have, kBlock, sizeof(kBlock)) == 0;
        // In from the start: the first body of a mode is made during its level's load. Idle unless a
        // mode spawn is armed; a story session never arms one.
        if (moveSiteMatches) moveHook = safetyhook::create_mid(reinterpret_cast<void*>(base + kMoveSiteRva), OnCheckpointMove);
        moveHookOk = moveSiteMatches && static_cast<bool>(moveHook);
        // shr ecx,0Dh / test cl,1 / je 0x008A84FA
        static const unsigned char kMpBranch[] = {0xF6, 0xC1, 0x01, 0x74, 0x2C};
        unsigned char branch[sizeof(kMpBranch)]{}, before[3]{};
        if (moveHookOk && Read(base + kMpBranchRva, branch, sizeof(branch)) && std::memcmp(branch, kMpBranch, sizeof(kMpBranch)) == 0 &&
            Read(base + kMpBranchRva - 3, before, 3) && before[0] == 0xC1 && before[1] == 0xE9 && before[2] == 0x0D)
            mpBranchHook = safetyhook::create_mid(reinterpret_cast<void*>(base + kMpBranchRva), OnMpBranch);
        mpBranchOk = static_cast<bool>(mpBranchHook);
        std::fprintf(log, "lifecycle native_placement: MP branch at %08X %s\n", static_cast<unsigned>(base + kMpBranchRva),
                     mpBranchOk ? "hooked (a body with the MP bit takes the armed move too)" : "NOT hooked (a body with the MP bit keeps the old placement)");
        std::fprintf(log, "lifecycle native_placement: Player::Init checkpoint move %08X %s, hook at %08X %s\n", static_cast<unsigned>(base + kMoveBlockRva),
                     moveSiteMatches ? "matches" : "MISMATCH", static_cast<unsigned>(base + kMoveSiteRva), moveHookOk ? "installed (idle until a mode spawn is armed)"
                                                                                                          : "NOT installed (old placement only)");
    }
    std::fprintf(log, "lifecycle on: native_placement=%d synced_restart=%d (ack %lu ms, timeout %lu ms) join_timeline=%d (sync timeout %lu ms, resync %d) "
                      "sticky_templates=%d spawn_timeout=%lu respawn_timeout=%lu\n",
                 moveHookOk ? 1 : 0, syncedRestart ? 1 : 0, static_cast<unsigned long>(restartAckMs), static_cast<unsigned long>(restartTimeoutMs), joinTimeline ? 1 : 0,
                 static_cast<unsigned long>(syncTimeoutMs), joinResync ? 1 : 0, templates::sticky ? 1 : 0, static_cast<unsigned long>(spawnTimeoutMs),
                 static_cast<unsigned long>(respawnTimeoutMs));
    return true;
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    std::fprintf(log, "lifecycle phase=%s for %lu ms changes=%ld timeouts=%ld | placement init_calls=%ld native=%ld skipped=%ld kept=%ld ghost_inits=%ld missed=%ld "
                      "load_arms_refused=%ld restart_arms_taken=%ld | restart=%s gen=%u synced=%ld alone=%ld ack_timeouts=%ld refused=%ld followed=%ld unprepared=%ld "
                      "timeouts=%ld holds=%ld\n",
                 PhaseName(phase), static_cast<unsigned long>(phaseAt ? GetTickCount() - phaseAt : 0), phaseChanges, phaseTimeouts, initCalls, placedNative, movesSkipped,
                 movesKept, ghostInits, wantsMissed, loadArmsRefused, restartArmsTaken, kRestartNames[static_cast<int>(restart)], generation, restartsSynced, restartsAlone,
                 restartAckTimeouts, restartRefused, restartsFollowed, restartsUnprepared, restartTimeouts, holdsKept);
    std::fprintf(log, "lifecycle net out=%ld in=%ld rejected=%ld partner_changes=%ld | joins complete=%ld timed_out=%ld resync asks=%ld served=%ld | links synced=%ld "
                      "host_enemies=%d\n",
                 packetsOut, packetsIn, packetsRejected, partnerChanges, joinsComplete, joinsTimedOut, resyncAsks, resyncServed, linksSynced, hostEnemies);
    templates::WriteStickyDiagnostics(log);
}
}  // namespace lifecycle
