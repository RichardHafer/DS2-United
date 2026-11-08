// Dead Space 2 United by Richard Hafer.
// Loading-screen sync: the level starts for everybody together.
//
// The engine already has the gate for it (Xbox debug build, 24.09.). Every
// tick of the loading screen LevelManager asks OkToStart before it starts the
// level:
//     LevelManager::OkToStart         PC 0x0042C970 (thunk 0x0040233D), thiscall(), plain ret
//       UILoadingScreen::HasHoldTimeElapsed (0x00A41B70 -> 0x00A41A60), then
//       LevelManagerWait::OkToStart   PC 0x0042BDE0, every wait on the list at
//                                     [0x02021574] must have its flag +0x58 set
// CheckpointLoadComplete (0x0042D65A) sets LevelManager state +0x18 to 0xC
// (start) or 0xF (wait); in 0xF HandleEvents (0x0042E30C) asks again each tick
// and starts the level once the answer is yes. While it says no, logic stays
// paused and the native loading screen stays up. CheckpointLoadComplete only
// runs once the checkpoint's own StreamSet reported its load complete, so at
// the first yes the rooms around the restart point are in.
//
// The host barrier (2026-11-02, [Loading] HostBarrier=1). Every coordinated load
// gets a generation from the host: its own restart, a level change, a load from
// the menu. Each machine holds at OkToStart until it is READY (loaded, the rooms
// of the checkpoint set in, the join suit on, the partners' Isaacs here if asked),
// reports READY for that generation, and starts only when the host releases it.
// The host releases once every member of the session (and every lobby joiner it
// is pulling in) is READY or out: silent, in the main menu, never reloading, or
// past its deadline. One decision on one machine, so everybody leaves the
// loading screen within one network delay.
//
// The older peer-to-peer rule (HostBarrier=0, LegacyDecide below): each machine
// starts once every partner it heard from lately is ready or already playing
// after his own reload; who counted came from timing, not from the host.
namespace mpmap { bool InVisit(); }   // runtime_coop_mpmap.inc
namespace lobby { uint32_t PendingJoiners(DWORD now, DWORD* since); }   // runtime_coop_lobby.inc
namespace level { extern bool joinArmed; }                             // runtime_coop_level.inc
namespace loadsync {
bool enabled = false, installed = false, hostBarrier = true;
constexpr uint32_t kOkToStartRva = 0x2C970;
constexpr uint32_t kMagic = 0x5953444C;   // 'LDSY'
constexpr uint32_t kVersion = 3;   // 3: generation, release and flags
enum : uint32_t { kLoading = 1, kHeld = 2, kPlaying = 3, kReady = 4 };
// flags; bits 16..23 (host only): the machines it still waits for
enum : uint32_t { kFlagSolo = 1, kFlagRestartAsked = 2, kFlagWantsJoin = 4, kFlagRoomsOk = 8, kFlagSuitOk = 0x10, kFlagBodiesOk = 0x20, kFlagBarrier = 0x40 };
struct Packet {
    uint32_t magic, version, token, sequence;
    uint32_t mapHash, state, heldMs, playingMs;
    uint32_t mapMs;
    uint32_t gen;           // host: its load generation; client: the host generation this load belongs to
    uint32_t releasedGen;   // host: the last generation it released
    uint32_t flags;
    char map[48];
};
static_assert(sizeof(Packet) == 96, "Unexpected load sync wire layout");
bool Valid(const Packet& p) { return p.magic == kMagic && p.version == kVersion && p.token && p.state >= kLoading && p.state <= kReady && p.map[47] == 0; }

DWORD timeoutMs = 60000, restartGraceMs = 15000, joinTimeoutMs = 120000, memberSilenceMs = 20000, roomsWaitMs = 8000, suitWaitMs = 8000;
constexpr DWORD kFreshMs = 3000, kSendMs = 250, kMenuGraceMs = 8000;
safetyhook::InlineHook okHook;

// This machine's side.
uint32_t state = kLoading, mapHash = 0, startedMapHash = 0, sequence = 0;
char map[48]{};
DWORD heldSince = 0, playingSince = 0, mapSince = 0, lastSend = 0, lastTick = 0, lastWhy = 0;
// When the current local load began (the level went away, a restart, the
// first "not yet" of the level manager). A partner who started playing the same map
// after that has reloaded too.
DWORD loadBeganAt = 0;
bool mapChange = false;   // this load comes from another map
// Generations. Host: gen is its current load, releasedGen the last one it let go.
// Client: playedGen is the host generation of the level it last started, genAtLoadStart that
// value when the current load began, adoptedGen the host generation this load belongs to
// (0 until the host shows one newer than playedGen or loads the same map).
uint32_t gen = 0, releasedGen = 0, genAtLoadStart = 0, adoptedGen = 0, playedGen = 0;
bool SceneSyncCapable() { return enabled && installed; }
uint32_t SceneMapHash() { return installed && state == kPlaying ? mapHash : 0; }
uint32_t SceneGeneration() { return IsHost() ? gen : adoptedGen; }
// A restart this machine asked the host for, and when (the barrier waits for the host's answer).
DWORD restartAskedAt = 0;
// A restart of this machine alone (the checkpoint module's reload after a necro's time, a
// restart kept local on an MP map): the partners keep playing.
DWORD soloReloadAt = 0;
volatile LONG soloReloads = 0;
// Partners, by token.
struct Peer { uint32_t token; DWORD at; Packet last; };
Peer peers[coop_peers::kMaxPeers]{};
volatile LONG asks = 0, notReady = 0, holds = 0, releasedAll = 0, releasedTimeout = 0, pollIdle = 0, longestMs = 0;
volatile LONG packetsIn = 0, packetsOut = 0, lines = 0, readyWaits = 0, longestReadyGapMs = 0;
volatile LONG barriers = 0, released = 0, lateStarts = 0, hostSilentStarts = 0, soloStarts = 0, excludedSilent = 0, excludedMenu = 0,
              excludedTimeout = 0, excludedNoReload = 0, excludedJoiner = 0, roomsTimeouts = 0, suitTimeouts = 0, voidEvents = 0, genAdoptions = 0;

const char* StateName(uint32_t s) {
    return s == kLoading ? "loading" : s == kHeld ? "at the loading screen" : s == kReady ? "ready to start" : s == kPlaying ? "playing" : "?";
}
template<class... A> void Line(const char* format, A... args) {
    if (InterlockedIncrement(&lines) <= 400) gate::Event(format, args...);
}

void ReadMap() {
    char name[48]{};
    const uint32_t loop = level::GameMainLoop();
    if (!loop || !Read(loop + level::kMapOffset, name, sizeof(name) - 1)) return;
    const uint32_t hash = name[0] ? gate::NameHash(name) : 0;
    if (hash && hash != mapHash) {
        mapHash = hash;
        std::memcpy(map, name, sizeof(map));
        mapSince = GetTickCount();
    }
}
uint32_t MachineOfToken(uint32_t token) {
    const auto* p = peerTable.FindByToken(token);
    return p ? p->machine : kNoMachine;
}
// The host's last packet, on a client (the peer with machine 0).
const Peer* HostView() {
    if (IsHost()) return nullptr;
    for (const auto& q : peers)
        if (q.token && MachineOfToken(q.token) == 0) return &q;
    return nullptr;
}

void SendState(DWORD now);   // below
void EnterLoading() {
    const DWORD now = GetTickCount();
    if (state == kPlaying || !loadBeganAt) {
        loadBeganAt = Stamp(now);
        streams::curtain::MarkLoad("a load");   // the rooms check below takes only sets completed from now on
        if (IsHost()) {
            ++gen;
            Line("coop_barrier host load gen=%u begins (map %s was playing %lu ms)", gen, map[0] ? map : "?",
                 static_cast<unsigned long>(playingSince ? TicksSince(now, playingSince) : 0));
        } else {
            // A host load seen while we still played (his restart we now follow) is this one.
            const Peer* h = HostView();
            genAtLoadStart = playedGen;
            adoptedGen = h && (h->last.gen > playedGen || h->last.state != kPlaying) ? h->last.gen : 0;
            Line("coop_barrier client load begins; last played gen=%u, the host's gen=%u (%s), adopted=%u", playedGen, h ? h->last.gen : 0u,
                 h ? StateName(h->last.state) : "host not heard", adoptedGen);
        }
        if (state == kPlaying && peerTable.Count()) { state = kLoading; SendState(now); }
    }
    if (state != kHeld && state != kReady) state = kLoading;
}

void OnPacket(const Packet& p) {
    if (!installed) return;
    InterlockedIncrement(&packetsIn);
    Peer* slot = nullptr;
    for (auto& q : peers) if (q.token == p.token) { slot = &q; break; }
    if (!slot) {
        slot = &peers[0];
        for (auto& q : peers) if (q.at < slot->at) slot = &q;
        *slot = Peer{};
        slot->token = p.token;
    }
    if (slot->at && !coop_peers::NewerSerial(p.sequence, slot->last.sequence)) return;
    slot->at = Stamp(GetTickCount());
    slot->last = p;
    // A client takes the host's generation for its own load: the host is loading too
    // (any state but playing), or the host has moved on since our load began.
    if (!IsHost() && hostBarrier && state != kPlaying && MachineOfToken(p.token) == 0 && p.gen) {
        const bool sameMap = !mapHash || p.mapHash == mapHash;
        if (((p.state != kPlaying && sameMap) || p.gen > genAtLoadStart) && p.gen != adoptedGen) {
            adoptedGen = p.gen;
            InterlockedIncrement(&genAdoptions);
            Line("coop_barrier this load belongs to the host's gen=%u (host %s on %s)", p.gen, StateName(p.state), p.map[0] ? p.map : "?");
        }
    }
}

// For other modules: is the partner with this token on the local map? 1 yes,
// 0 no, -1 not known (load sync off, or nothing heard from him lately).
int PartnerOnOurMap(uint32_t token, DWORD now) {
    if (!installed || !token || !mapHash) return -1;
    for (const auto& q : peers)
        if (q.token == token && now - q.at <= 10000) return q.last.mapHash == mapHash ? 1 : 0;
    return -1;
}
// The host is in a coordinated load on a campaign map: the window a joiner may come in through.
bool HostLoading(DWORD now, char* hostMap, size_t size) {
    const Peer* h = HostView();
    if (!h || TicksSince(now, h->at) > kFreshMs || h->last.state == kPlaying || !h->last.map[0] || _strnicmp(h->last.map, "frontend", 8) == 0) return false;
    if (hostMap && size) std::snprintf(hostMap, size, "%s", h->last.map);
    return true;
}

bool menuSkipLogged = false;
volatile LONG menuSkips = 0;
void OnSoloRestart() { soloReloadAt = Stamp(GetTickCount()); InterlockedIncrement(&soloReloads); }
void OnRestartAsked() { restartAskedAt = Stamp(GetTickCount()); }
// The level manager has started the level (with PartnersDuringLoad the settle clock runs through the hold).
bool LevelStarted() { return !installed || state == kPlaying; }
bool SoloReload(DWORD now) { return soloReloadAt && TicksSince(now, soloReloadAt) < 60000; }
bool RestartAsked(DWORD now) { return restartAskedAt && TicksSince(now, restartAskedAt) < 60000; }

// The older rule: every partner heard from lately is ready, or plays the map after his own reload.
bool LegacyDecide(DWORD now, char* why, size_t whySize) {
    const DWORD held = now - heldSince;
    for (const auto& q : peers) {
        if (!q.token || now - q.at > kFreshMs || !KnownPeerToken(q.token)) continue;
        const Packet& p = q.last;
        const DWORD age = now - q.at;
        if (p.mapHash == mapHash && p.state == kReady) continue;
        if (p.mapHash == mapHash && p.state == kPlaying) {
            if (mapChange) continue;                                  // he went first
            if (SoloReload(now)) continue;                            // only we reload
            const DWORD sinceOurLoad = loadBeganAt ? TicksSince(now, loadBeganAt) : held;
            if (p.playingMs + age < sinceOurLoad + 1000) continue;
            if (held >= restartGraceMs) continue;                     // he is not reloading at all
            std::snprintf(why, whySize, "partner %08X plays %s since before our reload", q.token, p.map);
            return false;
        }
        if (_strnicmp(p.map, "frontend", 8) == 0 && held >= kMenuGraceMs) {
            if (!menuSkipLogged) {
                menuSkipLogged = true;
                InterlockedIncrement(&menuSkips);
                Line("coop_loadsync partner %08X stays in the main menu: not waited for", q.token);
            }
            continue;
        }
        std::snprintf(why, whySize, "partner %08X is %s (%s)", q.token, StateName(p.state), p.map[0] ? p.map : "?");
        return false;
    }
    return true;
}

// --- the host's decision ------------------------------------------------------------
// Per member of the barrier: still waited for, ready, or out (and why). Transitions are
// logged once each, so the log shows every member's way through the barrier.
enum Verdict : int { kWait = 0, kIn = 1, kOut = 2 };
struct Member { uint32_t token, machine; int verdict; char why[72]; };
Member members[coop_peers::kMaxPeers + coop_lobby::kMaxMembers]{};
unsigned memberCount = 0;
uint32_t waitMask = 0;   // machines the host still waits for (sent in flags bits 16..23)
// True when the verdict changed (the counters count members, not ticks). Out is final for the barrier.
bool NoteMember(uint32_t token, uint32_t machine, int verdict, const char* why) {
    Member* m = nullptr;
    for (unsigned i = 0; i < memberCount; ++i)
        if ((token && members[i].token == token) || (!members[i].token && members[i].machine == machine)) { m = &members[i]; break; }
    if (!m && memberCount < std::size(members)) { m = &members[memberCount++]; *m = Member{token, machine, -1, ""}; }
    if (!m) return false;
    if (token && !m->token) m->token = token;   // a lobby joiner that has connected keeps his line
    if (m->verdict == kOut) return false;
    if (m->verdict == verdict && std::strcmp(m->why, why) == 0) return false;
    const bool changedVerdict = m->verdict != verdict;
    m->verdict = verdict;
    std::snprintf(m->why, sizeof(m->why), "%s", why);
    if (changedVerdict || verdict != kWait)
        Line("coop_barrier gen=%u member machine=%d token=%08X %s after %lu ms: %s", gen, machine == kNoMachine ? -1 : static_cast<int>(machine), token,
             verdict == kIn ? "READY" : verdict == kOut ? "LEFT OUT" : "waited for", static_cast<unsigned long>(TicksSince(GetTickCount(), heldSince)), why);
    return changedVerdict;
}
bool IsOut(uint32_t token, uint32_t machine) {
    for (unsigned i = 0; i < memberCount; ++i)
        if ((token && members[i].token == token) || (!token && !members[i].token && members[i].machine == machine)) return members[i].verdict == kOut;
    return false;
}
// True when nobody is waited for any more; why names the first one still waited for.
bool HostDecide(DWORD now, char* why, size_t whySize) {
    const DWORD held = TicksSince(now, heldSince);
    unsigned waiting = 0;
    uint32_t mask = 0;
    why[0] = 0;
    auto out = [&](uint32_t token, uint32_t machine, volatile LONG* counter, const char* text) {
        if (NoteMember(token, machine, kOut, text)) InterlockedIncrement(counter);
    };
    auto wait = [&](uint32_t token, uint32_t machine, const char* text) {
        NoteMember(token, machine, kWait, text);
        if (!waiting++) std::snprintf(why, whySize, "machine %d: %s", machine == kNoMachine ? -1 : static_cast<int>(machine), text);
        if (machine < 8) mask |= 1u << machine;
    };
    uint32_t present = 0;   // machines with a peer slot
    for (const auto& peer : peerTable.peers) {
        if (!peer.used || !peer.token || peer.relayed) continue;
        const uint32_t machine = peer.machine;
        if (machine < 32) present |= 1u << machine;
        if (IsOut(peer.token, machine)) continue;
        const Peer* q = nullptr;
        for (const auto& c : peers) if (c.token == peer.token) { q = &c; break; }
        char text[72];
        if (!q) {
            if (held < memberSilenceMs) { wait(peer.token, machine, "has not reported yet"); continue; }
            out(peer.token, machine, &excludedSilent, "never reported");
            continue;
        }
        const Packet& p = q->last;
        const DWORD silent = TicksSince(now, q->at);
        const bool wantsJoin = (p.flags & kFlagWantsJoin) != 0;
        const DWORD deadline = wantsJoin ? joinTimeoutMs : timeoutMs;
        // A machine that was ready or playing sends four times a second; one loading may freeze.
        if (silent > memberSilenceMs && (p.state == kReady || p.state == kPlaying || held >= deadline)) {
            std::snprintf(text, sizeof(text), "silent for %lu ms (last %s)", static_cast<unsigned long>(silent), StateName(p.state));
            out(peer.token, machine, &excludedSilent, text);
            continue;
        }
        const bool sameMap = p.mapHash == mapHash;
        if (sameMap && p.state == kReady && p.gen == gen) { NoteMember(peer.token, machine, kIn, "ready on the same load"); continue; }
        if (sameMap && p.state == kPlaying && p.gen == gen) { NoteMember(peer.token, machine, kIn, "already plays this load"); continue; }
        if (held >= deadline) {
            std::snprintf(text, sizeof(text), "TIMEOUT after %lu ms, still %s on %s", static_cast<unsigned long>(held), StateName(p.state), p.map[0] ? p.map : "?");
            out(peer.token, machine, wantsJoin ? &excludedJoiner : &excludedTimeout, text);
            continue;
        }
        if (_strnicmp(p.map, "frontend", 8) == 0) {
            // A joiner waits for a story restart only. In a game mode (a campaign arena or an MP map)
            // he comes through the host's lobby, so a game mode's restart never waits for him (02.11.:
            // an arena restart held the host 2 minutes for a menu joiner whose story join had failed).
            const bool gameMode = InterlockedCompareExchange(&arena::stateMirror, 0, 0) != 0 || level::HostMpMapHere();
            if (wantsJoin && !gameMode) { wait(peer.token, machine, "in the main menu, joining"); continue; }
            if (held >= kMenuGraceMs) {
                out(peer.token, machine, &excludedMenu, "stays in the main menu");
                continue;
            }
            wait(peer.token, machine, "in the main menu");
            continue;
        }
        if (sameMap && p.state == kPlaying) {
            // He plays the map and has not begun this load: the restart follow pulls him in.
            if (held >= restartGraceMs) {
                out(peer.token, machine, &excludedNoReload, "never began this reload");
                continue;
            }
            wait(peer.token, machine, "plays on, has not begun the reload");
            continue;
        }
        if (!sameMap && p.state == kPlaying) {
            std::snprintf(text, sizeof(text), "still plays %s (the level follow brings him)", p.map[0] ? p.map : "?");
            wait(peer.token, machine, text);
            continue;
        }
        if (sameMap && (p.state == kReady || p.state == kHeld)) {
            std::snprintf(text, sizeof(text), "%s%s%s%s", p.state == kReady ? "ready, taking our generation" : "at the loading screen",
                          p.state == kHeld && !(p.flags & kFlagRoomsOk) ? ", rooms loading" : "", p.state == kHeld && !(p.flags & kFlagSuitOk) ? ", suit loading" : "",
                          p.state == kHeld && !(p.flags & kFlagBodiesOk) ? ", partners' Isaacs" : "");
            wait(peer.token, machine, text);
            continue;
        }
        std::snprintf(text, sizeof(text), "%s %s", StateName(p.state), p.map[0] ? p.map : "?");
        wait(peer.token, machine, text);
    }
    // Lobby joiners the host is pulling in that have not connected yet.
    DWORD since = 0;
    const uint32_t pending = lobby::PendingJoiners(now, &since);
    for (uint32_t machine = 1; machine < 32; ++machine) {
        if (!(pending >> machine & 1u) || (present >> machine & 1u) || IsOut(0, machine)) continue;
        const DWORD age = since ? TicksSince(now, since) : held;
        if (age >= joinTimeoutMs) {
            out(0, machine, &excludedJoiner, "the lobby joiner never connected");
            continue;
        }
        wait(0, machine, "lobby joiner, loading from the main menu");
    }
    waitMask = mask;
    return waiting == 0;
}

// --- the client's decision -----------------------------------------------------------
bool ClientDecide(DWORD now, char* why, size_t whySize, const char** how) {
    const DWORD held = TicksSince(now, heldSince);
    if (SoloReload(now)) { *how = "a reload of this machine alone"; InterlockedIncrement(&soloStarts); return true; }
    const Peer* h = HostView();
    if (!h) {
        if (held >= memberSilenceMs) { *how = "no word from the host at all"; InterlockedIncrement(&hostSilentStarts); return true; }
        std::snprintf(why, whySize, "no word from the host yet");
        return false;
    }
    const Packet& hp = h->last;
    const DWORD silent = TicksSince(now, h->at);
    if (silent > memberSilenceMs && (hp.state == kReady || hp.state == kPlaying || held >= timeoutMs)) {
        *how = "the host went silent";
        InterlockedIncrement(&hostSilentStarts);
        return true;
    }
    if (hp.state == kPlaying) {
        if (hp.mapHash != mapHash) {
            if (held >= timeoutMs) { *how = "TIMEOUT, the host plays another map"; return true; }
            std::snprintf(why, whySize, "the host still plays %s", hp.map[0] ? hp.map : "?");
            return false;
        }
        if (adoptedGen && hp.releasedGen >= adoptedGen) { *how = "the host released this load"; return true; }
        if (hp.gen > genAtLoadStart) { *how = "the host released this load before we were ready"; InterlockedIncrement(&lateStarts); return true; }
        if (RestartAsked(now) && held < restartGraceMs) {
            std::snprintf(why, whySize, "the host has not begun the restart we asked for");
            return false;
        }
        if (mapChange) { *how = "the host plays this level already (late start)"; InterlockedIncrement(&lateStarts); return true; }
        if (held >= restartGraceMs) { *how = "the host never began a matching reload: alone"; InterlockedIncrement(&soloStarts); return true; }
        std::snprintf(why, whySize, "the host plays on and has not begun a reload");
        return false;
    }
    // The host loads, holds or is ready: its release decides. The safety net below covers a lost host.
    if (held >= timeoutMs + joinTimeoutMs + 10000) { *how = "TIMEOUT, the host never released"; return true; }
    std::snprintf(why, whySize, "the host is %s on %s (gen=%u, ours=%u)%s", StateName(hp.state), hp.map[0] ? hp.map : "?", hp.gen, adoptedGen,
                  (hp.flags >> 16) & 0xFF ? ", waiting for others" : "");
    return false;
}

// --- this machine's readiness --------------------------------------------------------
// The rooms of the checkpoint set, the join suit, the partners' Isaacs. Each waits a bounded time.
bool partnersDuringLoad = false;
DWORD ghostWaitMs = 10000, readySince = 0;
volatile LONG ghostWaits = 0, ghostWaitTimeouts = 0;
bool PartnerBodiesHere(char* why, size_t whySize) {
    for (unsigned slot = 0; slot < coop_peers::kMaxPeers; ++slot) {
        const auto& peer = peerTable.peers[slot];
        if (!peer.used || !peer.token || peer.machine == kNoMachine) continue;
        if (GhostForMachine(peer.machine)) continue;
        std::snprintf(why, whySize, "partner %u's Isaac is not here yet", slot);
        return false;
    }
    return true;
}
bool roomsCheck = true;
// Set by runtime_coop_uniform.inc: 1 the join suit is on and resident, 0 still coming, -1 nothing to wait for.
int (*suitState)(char* why, size_t whySize) = nullptr;
uint32_t localFlags = 0;
unsigned roomsLoaded = 0, roomsRegistered = 0;
bool roomsDone = false, suitDone = false, bodiesDone = false;
bool LocalReady(DWORD now, char* why, size_t whySize) {
    const DWORD held = TicksSince(now, heldSince);
    if (!roomsDone) {
        uint32_t missing[4]{};
        const int r = roomsCheck ? streams::curtain::BarrierRooms(now, &roomsLoaded, &roomsRegistered, missing) : -1;
        // the checkpoint set's completion is dispatched before CheckpointLoadComplete; give a late note two seconds
        if (r < 0 && roomsCheck && held < 2000) {
            std::snprintf(why, whySize, "no stream set completed since the load yet");
        } else if (r != 0) {
            roomsDone = true;
            Line("coop_barrier rooms %s after %lu ms at the loading screen: %u of %u in a TOC loaded", r > 0 ? "ready" : "not checked (no fresh checkpoint set or check off)",
                 static_cast<unsigned long>(held), roomsLoaded, roomsRegistered);
        } else if (held >= roomsWaitMs) {
            roomsDone = true;
            InterlockedIncrement(&roomsTimeouts);
            Line("coop_barrier rooms NOT complete after %lu ms: %u of %u loaded, first missing %08X-%08X; ready anyway", static_cast<unsigned long>(held), roomsLoaded,
                 roomsRegistered, missing[0], missing[1]);
        } else {
            std::snprintf(why, whySize, "rooms %u of %u loaded", roomsLoaded, roomsRegistered);
        }
    }
    if (!suitDone) {
        char text[96] = "";
        const int s = suitState ? suitState(text, sizeof(text)) : -1;
        if (s != 0) {
            suitDone = true;
            if (s > 0) Line("coop_barrier join suit on after %lu ms: %s", static_cast<unsigned long>(held), text);
        } else if (held >= suitWaitMs) {
            suitDone = true;
            InterlockedIncrement(&suitTimeouts);
            Line("coop_barrier join suit NOT resident after %lu ms (%s); ready anyway, it streams in", static_cast<unsigned long>(held), text);
        } else if (roomsDone) {
            std::snprintf(why, whySize, "the join suit: %s", text);
        }
    }
    // On an MP map the session starts at the arrival, so no body comes during the hold.
    if (!bodiesDone) {
        char text[96] = "";
        if (!partnersDuringLoad || mpmap::InVisit() || PartnerBodiesHere(text, sizeof(text))) bodiesDone = true;
        else if (held >= ghostWaitMs) {
            bodiesDone = true;
            InterlockedIncrement(&ghostWaitTimeouts);
            Line("coop_loadsync %s: %s after %lu ms; ready anyway, his Isaac follows in the level", map, text, static_cast<unsigned long>(held));
        } else {
            InterlockedIncrement(&ghostWaits);
            if (roomsDone && suitDone) std::snprintf(why, whySize, "%s", text);
        }
    }
    localFlags = (roomsDone ? kFlagRoomsOk : 0u) | (suitDone ? kFlagSuitOk : 0u) | (bodiesDone ? kFlagBodiesOk : 0u);
    return roomsDone && suitDone && bodiesDone;
}

// --- after the release: the void watch -----------------------------------------------
// Diagnostics only: a local Isaac falling freely in the first seconds of a level says the
// floor was not there at the release. The fall guard ([MpMap] CampaignFallGuard) acts.
DWORD watchUntil = 0, watchSampleAt = 0, releasedAt = 0;
uint32_t watchPlayer = 0;
float watchStart[3]{}, watchLast[3]{};
bool watchHaveStart = false, voidLogged = false;
constexpr uint32_t kPlayerPosition = 0xC0;
void VoidWatch(DWORD now) {
    if (!watchUntil || static_cast<LONG>(now - watchUntil) > 0 || TicksSince(now, watchSampleAt) < 100) return;
    const DWORD dt = watchSampleAt ? TicksSince(now, watchSampleAt) : 0;
    watchSampleAt = Stamp(now);
    const uint32_t player = LocalCampaignPlayer();
    float at[3]{};
    if (!player || !Read(player + kPlayerPosition, at, sizeof(at))) return;
    if (player != watchPlayer) {
        watchPlayer = player;
        std::memcpy(watchStart, at, sizeof(at));
        std::memcpy(watchLast, at, sizeof(at));
        watchHaveStart = true;
        Line("coop_barrier Isaac %08X stands at (%.1f %.1f %.1f) %lu ms after the release", player, at[0], at[1], at[2],
             static_cast<unsigned long>(TicksSince(now, releasedAt)));
        return;
    }
    const float vy = dt ? (at[1] - watchLast[1]) * 1000.0f / static_cast<float>(dt) : 0.0f;
    std::memcpy(watchLast, at, sizeof(at));
    if (voidLogged || !watchHaveStart || scenekind::local != scenekind::kNone) return;
    if (vy < -12.0f && at[1] < watchStart[1] - 8.0f) {
        voidLogged = true;
        InterlockedIncrement(&voidEvents);
        Line("coop_barrier VOID: Isaac %08X falls at %.1f m/s, %.1f m below his start (%.1f %.1f %.1f), %lu ms after the release; rooms at the release %u of %u",
             player, vy, watchStart[1] - at[1], at[0], at[1], at[2], static_cast<unsigned long>(TicksSince(now, releasedAt)), roomsLoaded, roomsRegistered);
    }
}

void Release(DWORD now, volatile LONG* counter, const char* why) {
    InterlockedIncrement(counter);
    const LONG ms = static_cast<LONG>(now - heldSince);
    if (ms > InterlockedCompareExchange(&longestMs, 0, 0)) InterlockedExchange(&longestMs, ms);
    Line("coop_loadsync %s starts after %ld ms at the loading screen: %s", map[0] ? map : "?", ms, why);
    heldSince = 0;
    readySince = 0;
    releasedAt = Stamp(now);
    watchUntil = Stamp(now + 15000);
    watchSampleAt = 0;
    watchPlayer = 0;
    watchHaveStart = voidLogged = false;
    if (partnersDuringLoad) return;   // the settle clock ran through the hold
    InterlockedExchange(&loading_hold::holding, 0);
    InterlockedIncrement(&loading_hold::released);
}

// Every start of a level (the level manager's yes, the plugin's included), for the bots:
// after a restart they wait for it (bot::HoldTick).
volatile LONG levelStarts = 0;
volatile DWORD lastStartAt = 0;
bool OkToStartInner(void* self);
bool __fastcall OnOkToStart(void* self, void*) {
    const bool go = OkToStartInner(self);
    if (go) {
        lastStartAt = GetTickCount();
        InterlockedIncrement(&levelStarts);
    }
    return go;
}
void BeginHold(DWORD now) {
    if (state == kPlaying) EnterLoading();   // a restart fast enough to pass the first ask
    state = kHeld;
    heldSince = Stamp(now);
    menuSkipLogged = false;
    mapChange = startedMapHash != mapHash;
    memberCount = 0;
    waitMask = 0;
    roomsDone = !hostBarrier;
    suitDone = !hostBarrier;
    bodiesDone = false;
    roomsLoaded = roomsRegistered = 0;
    InterlockedIncrement(&holds);
    if (hostBarrier) InterlockedIncrement(&barriers);
    if (!partnersDuringLoad) InterlockedExchange(&loading_hold::holding, 1);
    SendState(now);   // the partner may be waiting on exactly this
    Line("coop_loadsync %s loaded here, waiting at the loading screen for the partners (%s)", map[0] ? map : "?",
         mapChange ? "new map" : SoloReload(now) ? "same map again, reloaded here alone" : "same map again");
    if (hostBarrier)
        Line("coop_barrier %s hold: map=%s gen=%u%s peers=%u%s%s", IsHost() ? "host" : "client", map[0] ? map : "?", IsHost() ? gen : adoptedGen,
             IsHost() ? "" : " (host gen at load start)", peerTable.Count(), RestartAsked(now) ? " restart_asked=1" : "", SoloReload(now) ? " solo=1" : "");
}
// Game thread, the LevelManager tick while the loading screen is up.
bool OkToStartInner(void* self) {
    InterlockedIncrement(&asks);
    const bool native = okHook.thiscall<bool>(self);
    if (!native) { InterlockedIncrement(&notReady); if (state != kHeld && state != kLoading) EnterLoading(); return false; }
    const DWORD now = GetTickCount();
    // an MP map arrival: its Isaac is spawned under this loading screen first (runtime_coop_mpmap.inc)
    if (mpmap::HoldStartForEarlySpawn(now)) { InterlockedIncrement(&notReady); return false; }
    ReadMap();
    // The main menu is never held: the partners only connect once they are in
    // it (25.09. 00:22: the host sat at the frontend's loading screen waiting
    // for a client that could not connect before its own menu was up).
    const bool frontend = !map[0] || _strnicmp(map, "frontend", 8) == 0;
    // A host pulling lobby joiners in holds even before the first of them has connected.
    const bool joinersComing = hostBarrier && IsHost() && lobby::PendingJoiners(now, nullptr) != 0;
    if (!enabled || (!peerTable.Count() && !joinersComing) || frontend) {
        if (state == kHeld || state == kReady) Release(now, &releasedAll, frontend ? "the main menu is never held" : "no partner connected any more");
        if (state != kPlaying) {
            if (IsHost() && state != kPlaying) releasedGen = gen;
            state = kPlaying; playingSince = now; startedMapHash = mapHash; soloReloadAt = 0; restartAskedAt = 0;
        }
        return true;
    }
    if (state != kHeld && state != kReady) BeginHold(now);
    char why[160] = "";
    const bool poll = TicksSince(now, lastTick) < 1000;
    if (!poll && InterlockedIncrement(&pollIdle) == 1)
        Line("coop_loadsync our network tick is not running at the loading screen; the timeout decides");
    if (state == kHeld) {
        bool ready = false;
        if (hostBarrier) ready = LocalReady(now, why, sizeof(why));
        else {
            // the older rule: bodies only
            ready = !partnersDuringLoad || mpmap::InVisit() || PartnerBodiesHere(why, sizeof(why));
            if (!ready && now - heldSince >= ghostWaitMs) {
                InterlockedIncrement(&ghostWaitTimeouts);
                ready = true;
                Line("coop_loadsync %s: %s after %lu ms; ready anyway, his Isaac follows in the level", map, why, static_cast<unsigned long>(now - heldSince));
            } else if (!ready) {
                InterlockedIncrement(&ghostWaits);
            }
        }
        if (ready) {
            state = kReady;
            readySince = Stamp(now);
            SendState(now);   // the others may wait on exactly this
            Line("coop_loadsync %s ready here after %lu ms at the loading screen, waiting until every partner is ready", map,
                 static_cast<unsigned long>(now - heldSince));
        }
    }
    bool go = false;
    const char* how = "every partner is ready too";
    if (state == kReady) {
        if (!hostBarrier) go = LegacyDecide(now, why, sizeof(why));
        else if (IsHost()) {
            go = HostDecide(now, why, sizeof(why));
            if (go) {
                unsigned in = 0, out = 0;
                for (unsigned i = 0; i < memberCount; ++i) members[i].verdict == kIn ? ++in : members[i].verdict == kOut ? ++out : 0u;
                static char text[96];
                std::snprintf(text, sizeof(text), "the host released gen=%u: %u member(s) ready, %u left out", gen, in, out);
                how = text;
                releasedGen = gen;
                InterlockedIncrement(&released);
            }
        } else {
            go = ClientDecide(now, why, sizeof(why), &how);
        }
        if (go) {
            const LONG gap = static_cast<LONG>(now - readySince);
            if (gap > InterlockedCompareExchange(&longestReadyGapMs, 0, 0)) InterlockedExchange(&longestReadyGapMs, gap);
            Release(now, &releasedAll, how);
        } else if (now - readySince < 20) {
            InterlockedIncrement(&readyWaits);
        }
    }
    // The older rule's own deadline; the barrier's deadlines are per member (host) and in ClientDecide.
    if (!go && !hostBarrier && now - heldSince >= timeoutMs) { Release(now, &releasedTimeout, "TIMEOUT"); go = true; }
    if (!go && hostBarrier && state == kHeld && now - heldSince >= timeoutMs + joinTimeoutMs + 10000) {
        Release(now, &releasedTimeout, "TIMEOUT, this machine never became ready");
        go = true;
    }
    if (!go) {
        if (now - lastWhy >= 5000) {
            lastWhy = now;
            Line("coop_loadsync still waiting after %lu ms: %s", now - heldSince, why[0] ? why : "?");
            if (now - heldSince >= 5000 && now - heldSince < 10000) notice::Show("WAITING FOR THE OTHER PLAYERS TO LOAD");
        }
        return false;
    }
    if (IsHost()) releasedGen = gen;
    else {
        const Peer* h = HostView();
        playedGen = adoptedGen ? adoptedGen : h ? h->last.gen : playedGen;
        adoptedGen = playedGen;
    }
    state = kPlaying;
    playingSince = now;
    startedMapHash = mapHash;
    loadBeganAt = 0;
    soloReloadAt = 0;
    restartAskedAt = 0;
    // At once: the first seconds of the level can stall Poll, and the
    // partner waited 4 s (25.09.) for a "playing" sent after the stall.
    SendState(now);
    return true;
}

void SendState(DWORD now) {
    if (!peerTable.Count()) return;
    lastSend = now;
    uint32_t flags = localFlags | (hostBarrier ? kFlagBarrier : 0u);
    if (SoloReload(now)) flags |= kFlagSolo;
    if (RestartAsked(now)) flags |= kFlagRestartAsked;
    if (level::joinArmed && (!map[0] || _strnicmp(map, "frontend", 8) == 0)) flags |= kFlagWantsJoin;
    if (IsHost()) flags |= (waitMask & 0xFFu) << 16;
    // clamped: ReadMap stamps mapSince with a fresher tick than the caller's now
    Packet p{kMagic, kVersion, peerReadiness.token, ++sequence, mapHash, state,
             (state == kHeld || state == kReady) ? TicksSince(now, heldSince) : 0u, state == kPlaying ? TicksSince(now, playingSince) : 0u,
             mapSince ? TicksSince(now, mapSince) : 0u, IsHost() ? gen : adoptedGen, IsHost() ? releasedGen : 0u, flags, {}};
    std::memcpy(p.map, map, sizeof(p.map) - 1);
    if (SendToPeers(&p, sizeof(p))) InterlockedIncrement(&packetsOut);
}
// From Poll: the local state goes out a few times a second, loading or not.
void Tick(DWORD now) {
    if (!installed) return;
    lastTick = now;
    if (!inLevel && state == kPlaying) EnterLoading();   // the level went away: whatever comes next is a load
    VoidWatch(now);
    if (!peerTable.Count() || now - lastSend < kSendMs) return;
    ReadMap();
    SendState(now);
}

// A checkpoint restart reloads: until the level manager asks again, this machine is loading.
void OnRestart() { if (state == kPlaying) EnterLoading(); }

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Loading", "SyncWithPartners", 0, ini.c_str()) != 0;
    hostBarrier = GetPrivateProfileIntA("Loading", "HostBarrier", 1, ini.c_str()) != 0;
    timeoutMs = static_cast<DWORD>(GetPrivateProfileIntA("Loading", "SyncTimeoutMs", 60000, ini.c_str()));
    restartGraceMs = static_cast<DWORD>(GetPrivateProfileIntA("Loading", "RestartGraceMs", 15000, ini.c_str()));
    joinTimeoutMs = static_cast<DWORD>(GetPrivateProfileIntA("Loading", "JoinTimeoutMs", 120000, ini.c_str()));
    memberSilenceMs = static_cast<DWORD>(GetPrivateProfileIntA("Loading", "MemberSilenceMs", 20000, ini.c_str()));
    roomsCheck = GetPrivateProfileIntA("Loading", "WaitForRooms", 1, ini.c_str()) != 0;
    roomsWaitMs = static_cast<DWORD>(GetPrivateProfileIntA("Loading", "RoomsWaitMs", 8000, ini.c_str()));
    suitWaitMs = static_cast<DWORD>(GetPrivateProfileIntA("Loading", "SuitWaitMs", 8000, ini.c_str()));
    partnersDuringLoad = GetPrivateProfileIntA("Loading", "PartnersDuringLoad", 0, ini.c_str()) != 0;
    ghostWaitMs = static_cast<DWORD>(GetPrivateProfileIntA("Loading", "GhostWaitMs", 10000, ini.c_str()));
    if (memberSilenceMs < 5000) memberSilenceMs = 5000;
    // mov ax,[ecx+0x1C] / shr ax,9 / test al,1 / je
    static const unsigned char kSig[] = {0x66, 0x8B, 0x41, 0x1C, 0x66, 0xC1, 0xE8, 0x09, 0xA8, 0x01, 0x74};
    // The bots need the start signal too ([Bot] Enabled); without partner sync the
    // hook only watches and passes the game's own answer on.
    const bool forBots = GetPrivateProfileIntA("Bot", "Enabled", 0, ini.c_str()) != 0;
    if (enabled || forBots)
        installed = hook_site::Install(log, "coop_loadsync LevelManager::OkToStart", base + kOkToStartRva, kSig, sizeof(kSig),
                                       reinterpret_cast<void*>(OnOkToStart), okHook);
    std::fprintf(log, "coop_loadsync enabled=%d installed=%d timeout=%lu ms restart_grace=%lu ms partners_during_load=%d ghost_wait=%lu ms (the level starts for everybody together)\n",
                 enabled ? 1 : 0, installed ? 1 : 0, timeoutMs, restartGraceMs, partnersDuringLoad ? 1 : 0, ghostWaitMs);
    std::fprintf(log, "coop_barrier host_barrier=%d join_timeout=%lu ms member_silence=%lu ms wait_for_rooms=%d rooms_wait=%lu ms suit_wait=%lu ms "
                      "(the host numbers every load and releases it once every member is ready or out)\n",
                 hostBarrier ? 1 : 0, joinTimeoutMs, memberSilenceMs, roomsCheck ? 1 : 0, roomsWaitMs, suitWaitMs);
    return installed;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG activity = InterlockedCompareExchange(&holds, 0, 0) + InterlockedCompareExchange(&releasedAll, 0, 0) +
                          InterlockedCompareExchange(&releasedTimeout, 0, 0) + InterlockedCompareExchange(&pollIdle, 0, 0) +
                          InterlockedCompareExchange(&voidEvents, 0, 0) + InterlockedCompareExchange(&genAdoptions, 0, 0);
    if (activity == last) return;
    last = activity;
    std::fprintf(log, "coop_loadsync ready_waits=%ld longest_ready_to_start_ms=%ld (how long a ready machine waited for the last partner) solo_reloads=%ld menu_skips=%ld\n",
                 InterlockedCompareExchange(&readyWaits, 0, 0), InterlockedCompareExchange(&longestReadyGapMs, 0, 0),
                 InterlockedCompareExchange(&soloReloads, 0, 0), InterlockedCompareExchange(&menuSkips, 0, 0));
    std::fprintf(log, "coop_loadsync asks=%ld not_ready_natively=%ld holds=%ld together=%ld timed_out=%ld poll_idle=%ld longest_ms=%ld "
                      "packets_in=%ld packets_out=%ld ghost_waits=%ld ghost_wait_timeouts=%ld state=%s map=%s\n",
                 InterlockedCompareExchange(&asks, 0, 0), InterlockedCompareExchange(&notReady, 0, 0),
                 InterlockedCompareExchange(&holds, 0, 0), InterlockedCompareExchange(&releasedAll, 0, 0),
                 InterlockedCompareExchange(&releasedTimeout, 0, 0), InterlockedCompareExchange(&pollIdle, 0, 0),
                 InterlockedCompareExchange(&longestMs, 0, 0), InterlockedCompareExchange(&packetsIn, 0, 0),
                 InterlockedCompareExchange(&packetsOut, 0, 0), InterlockedCompareExchange(&ghostWaits, 0, 0),
                 InterlockedCompareExchange(&ghostWaitTimeouts, 0, 0), StateName(state), map[0] ? map : "?");
    std::fprintf(log, "coop_barrier role=%s gen=%u released_gen=%u adopted_gen=%u barriers=%ld host_releases=%ld late_starts=%ld host_silent_starts=%ld solo_starts=%ld "
                      "left_out silent=%ld menu=%ld timeout=%ld no_reload=%ld joiner=%ld rooms_timeouts=%ld suit_timeouts=%ld void_events=%ld adoptions=%ld\n",
                 IsHost() ? "host" : "client", gen, releasedGen, adoptedGen, InterlockedCompareExchange(&barriers, 0, 0), InterlockedCompareExchange(&released, 0, 0),
                 InterlockedCompareExchange(&lateStarts, 0, 0), InterlockedCompareExchange(&hostSilentStarts, 0, 0), InterlockedCompareExchange(&soloStarts, 0, 0),
                 InterlockedCompareExchange(&excludedSilent, 0, 0), InterlockedCompareExchange(&excludedMenu, 0, 0), InterlockedCompareExchange(&excludedTimeout, 0, 0),
                 InterlockedCompareExchange(&excludedNoReload, 0, 0), InterlockedCompareExchange(&excludedJoiner, 0, 0), InterlockedCompareExchange(&roomsTimeouts, 0, 0),
                 InterlockedCompareExchange(&suitTimeouts, 0, 0), InterlockedCompareExchange(&voidEvents, 0, 0), InterlockedCompareExchange(&genAdoptions, 0, 0));
}

// --- the join suit (runtime_coop_uniform.inc reads it) ---------------------------------
// A story joiner starts a new game at the host's checkpoint, and the level's template
// dresses him in the patient suit (24.10.). The suit he should wear is armed here before the
// load and put on in Player::Init's own dress (coop_uniform OnChange at 0x008A8670).
struct JoinSuit { bool armed, consumed; uint32_t guid[4]; char why[48]; DWORD armedAt; uint32_t player; };
JoinSuit joinSuit{};
volatile LONG joinSuitsArmed = 0, joinSuitsTaken = 0, joinSuitsUpdated = 0;
void ArmJoinSuit(const uint32_t* guid, const char* why) {
    if (!guid || !(guid[0] | guid[1] | guid[2] | guid[3])) return;
    joinSuit = JoinSuit{};
    joinSuit.armed = true;
    std::memcpy(joinSuit.guid, guid, 16);
    std::snprintf(joinSuit.why, sizeof(joinSuit.why), "%s", why);
    joinSuit.armedAt = Stamp(GetTickCount());
    InterlockedIncrement(&joinSuitsArmed);
    Line("coop_barrier join suit armed: %08X-%08X-%08X-%08X (%s)", guid[0], guid[1], guid[2], guid[3], why);
}
// The chapter's start suit for the checkpoint a join loads (generated_checkpoints.h).
bool ChapterSuit(const uint32_t* checkpoint, uint32_t* out) {
    if (!checkpoint) return false;
    for (const auto& c : ds2checkpoints::kCheckpoints) {
        if (std::memcmp(c.guid, checkpoint, 16) != 0 || c.chapter >= ds2checkpoints::kChapterCount) continue;
        const auto& ch = ds2checkpoints::kChapters[c.chapter];
        if (!ch.hasUniform) continue;   // the same checkpoint can sit in a debug list row without a suit
        std::memcpy(out, ch.uniform, 16);
        return true;
    }
    return false;
}
void ArmJoinSuitForCheckpoint(const uint32_t* checkpoint, const char* why) {
    uint32_t suit[4]{};
    if (ChapterSuit(checkpoint, suit)) ArmJoinSuit(suit, why);
    else Line("coop_barrier no join suit: checkpoint %08X-%08X has no chapter start suit here; the level's own stays", checkpoint ? checkpoint[0] : 0u,
              checkpoint ? checkpoint[1] : 0u);
}
bool JoinSuitPending() { return joinSuit.armed && !joinSuit.consumed && TicksSince(GetTickCount(), joinSuit.armedAt) < 300000; }
void UpdateJoinSuit(const uint32_t* guid, const char* why) {
    if (!JoinSuitPending() || !guid || std::memcmp(guid, joinSuit.guid, 16) == 0) return;
    std::memcpy(joinSuit.guid, guid, 16);
    std::snprintf(joinSuit.why, sizeof(joinSuit.why), "%s", why);
    InterlockedIncrement(&joinSuitsUpdated);
    Line("coop_barrier join suit now %08X-%08X-%08X-%08X (%s)", guid[0], guid[1], guid[2], guid[3], why);
}
// For Player::Init's dress of a local body during a load: the armed suit, once.
bool TakeJoinSuit(uint32_t player, uint32_t* out, const char** why) {
    if (!JoinSuitPending() || state == kPlaying) return false;
    joinSuit.consumed = true;
    joinSuit.player = player;
    std::memcpy(out, joinSuit.guid, 16);
    *why = joinSuit.why;
    InterlockedIncrement(&joinSuitsTaken);
    return true;
}
void WriteJoinSuitDiagnostics(FILE* log) {
    if (!InterlockedCompareExchange(&joinSuitsArmed, 0, 0)) return;
    std::fprintf(log, "coop_barrier join_suits armed=%ld taken=%ld updated=%ld pending=%d player=%08X\n", InterlockedCompareExchange(&joinSuitsArmed, 0, 0),
                 InterlockedCompareExchange(&joinSuitsTaken, 0, 0), InterlockedCompareExchange(&joinSuitsUpdated, 0, 0), JoinSuitPending() ? 1 : 0, joinSuit.player);
}
}  // namespace loadsync
