// Dead Space 2 United by Hafer.
// Re-ghosting after a partner's world was rebuilt.
//
// 2026-09-17: sometimes the others stopped seeing the host, mostly after a
// checkpoint restart or a death. The log of 14:34 shows the shape: the
// client's level is torn down (reflectables=0), every ghost in it is
// deleted with it (the host's Isaac, the enemies) while the TNL connection
// itself survives (connections=1 throughout). On the server side of that
// connection the GhostConnection still believes every one of those ghosts is
// in place, so it never sends them again. The client comes back into the
// level to an empty room.
//
// TNL has the cure and the engine ships it: GhostConnection::resetGhosting
// (PC 0x01416850: mGhosting=false, mScoping=false, rpcEndGhosting,
// ++mGhostingSequence, clearGhostInfo) followed by activateGhosting
// (0x01416A20: ++sequence, rebuild the free list, mScoping=true,
// rpcStartGhosting); the client answers rpcReadyForNormalGhosts and its
// remote (0x01415480) turns ghosting back on with every in-scope object sent
// afresh. Both return at once on the client side of a connection (no ghost
// array), so calling them on every connection is safe.
//
// The signal: a partner's readiness epoch moves and the partner is ready
// again, so its Player was replaced. Then every machine resets ghosting on
// its connection to that partner (matched by TNL port at connection+0x10C,
// address at +0x108), once per epoch.
//
// On an MP map the epoch moves with every spawn and necro respawn while the
// world stands; a reset there deleted and resent the partner's Isaac and bots
// each time, back in the Engineering suit with empty hands (Versus, 13.10.:
// five per round). There only the first change of each arrival (a new round or
// a side swap reloads the map) resets; later ones are noted.
namespace mpmap { bool InVisit(); unsigned Run(); }   // runtime_coop_mpmap.inc
namespace reghost {
bool enabled = false, installed = false;
constexpr uint32_t kResetGhosting = 0x1016850, kActivateGhosting = 0x1016A20;
constexpr uint32_t kConnectionIp = 0x108, kConnectionPort = 0x10C;
uint32_t seenEpoch[coop_peers::kMaxPeers]{};
unsigned resetRun[coop_peers::kMaxPeers]{};   // the MP arrival (run + 1) a reset was made in
volatile LONG resets = 0, notFound = 0, faults = 0, lateJoins = 0, sameRunSkipped = 0, spectatorSkipped = 0, teamSwitchSkipped = 0;
// A team switch (necro transform, back to Isaac, a watched scene) replaces the partner's body but
// not his world: a reset deleted and resent every enemy and they were invisible on his screen for
// seconds (user, 17.10.). [Coop] ReghostOnTeamSwitch=0: no reset when his role changed within 15 s.
// His role packet may come up to a second after the new epoch, so every change waits 1.5 s for it.
bool resetOnTeamSwitch = false;
DWORD epochSeenAt[coop_peers::kMaxPeers]{};
// A partner whose first epoch arrives while our level has run this long joined
// late (the menu lobby mid-level, or back after a crash): ghosting to his new
// connection was never started, and he loaded into a world without players
// or enemies (12.10.). A partner that loaded with us is ready well before.
constexpr DWORD kLateJoinMs = 20000;

uint32_t ConnectionTo(uint32_t ip, unsigned tnlPort) {
    uint32_t iface = 0, count = 0, array = 0;
    if (!session || !Read(session + kSessionInterface, &iface, 4) || !iface) return 0;
    if (!Read(iface + kInterfaceConnectionCount, &count, 4) || !Read(iface + kInterfaceConnectionArray, &array, 4)) return 0;
    for (uint32_t i = 0; i < count && i < 16; ++i) {
        uint32_t connection = 0, cip = 0, cport = 0, state = 0;
        if (!Read(array + 4 * i, &connection, 4) || !connection) continue;
        if (!Read(connection + kConnectionIp, &cip, 4) || !Read(connection + kConnectionPort, &cport, 4)) continue;
        if (cip == ip && cport == tnlPort && Read(connection + kConnectionState, &state, 4) && state == 7) return connection;
    }
    return 0;
}

// Any connection to ip:port, pending ones included (the redial must not stack
// a second attempt on top of one still handshaking).
bool AnyConnectionTo(uint32_t ip, unsigned tnlPort) {
    uint32_t iface = 0, count = 0, array = 0;
    if (!session || !Read(session + kSessionInterface, &iface, 4) || !iface) return false;
    if (!Read(iface + kInterfaceConnectionCount, &count, 4) || !Read(iface + kInterfaceConnectionArray, &array, 4)) return false;
    for (uint32_t i = 0; i < count && i < 16; ++i) {
        uint32_t connection = 0, cip = 0, cport = 0;
        if (!Read(array + 4 * i, &connection, 4) || !connection) continue;
        if (Read(connection + kConnectionIp, &cip, 4) && Read(connection + kConnectionPort, &cport, 4) && cip == ip && cport == tnlPort) return true;
    }
    return false;
}
int Filter(unsigned) { InterlockedIncrement(&faults); return EXCEPTION_EXECUTE_HANDLER; }
// Ending ghosting without starting it again. A rebirth wants reset+activate;
// a partner that timed out wants only the reset, because there is nobody left
// to ghost to.
bool ResetOnly(uint32_t connection) {
    using Call = void(__thiscall*)(void*);
    __try {
        reinterpret_cast<Call>(base + kResetGhosting)(reinterpret_cast<void*>(connection));
        return true;
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}
// A grab (PairedAttackCoordinator, reflectable vtable 0x01B73150) on this machine, local or a
// partner's copy: a reset now deletes and resends the bodies it pairs, and a copy that unpacks
// before its defender waits for good (16.10., grab 1: the client's reset rebuilt the host's
// Isaac right as the grab came). The reset waits for the grab, at most kGrabWaitMs.
bool waitForGrabs = true;   // [Coop] ReghostWaitsForGrabs
constexpr DWORD kGrabWaitMs = 12000;
DWORD grabWaitSince[coop_peers::kMaxPeers]{};
volatile LONG deferredForGrab = 0, grabWaitsTimedOut = 0;
uint32_t GrabObject(uint32_t& flags) {
    uint32_t node = 0;
    if (!Read(base + 0x1C5B744, &node, 4)) return 0;
    for (unsigned steps = 0; node && steps < 4096; ++steps) {
        uint32_t vtable = 0, next = 0;
        if (Read(node, &vtable, 4) && vtable == base + 0x1773150) {
            flags = 0;
            Read(node - 0x58 + 0x10, &flags, 1);
            return node - 0x58;
        }
        if (!Read(node + 4, &next, 4) || next == node) break;
        node = next;
    }
    return 0;
}
bool ResetAndActivate(uint32_t connection) {
    using Call = void(__thiscall*)(void*);
    __try {
        reinterpret_cast<Call>(base + kResetGhosting)(reinterpret_cast<void*>(connection));
        reinterpret_cast<Call>(base + kActivateGhosting)(reinterpret_cast<void*>(connection));
        return true;
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}

void Poll() {
    if (!installed || !enabled || !session) return;
    if (!mpmap::InVisit()) for (unsigned& r : resetRun) r = 0;   // a new visit counts its arrivals from 1 again
    for (unsigned slot = 0; slot < coop_peers::kMaxPeers; ++slot) {
        const auto& peer = peerTable.peers[slot];
        const auto& readiness = peerRuntime[slot].readiness;
        if (!peer.used || !peer.Addressable() || !readiness.havePeer) continue;
        if (readiness.peerEpoch == seenEpoch[slot]) { grabWaitSince[slot] = 0; epochSeenAt[slot] = 0; continue; }
        if (!readiness.peerReady) continue;          // wait until the new player is there
        if (mpmap::InVisit() && !levelSettled) continue;   // our own arrival first: its run decides
        if (!resetOnTeamSwitch && seenEpoch[slot]) {
            const DWORD tick = GetTickCount();
            if (!epochSeenAt[slot]) epochSeenAt[slot] = Stamp(tick);
            if (tick - epochSeenAt[slot] < 1500) continue;
            const DWORD changed = roles::roleChangedAt[slot];
            if (changed && tick - changed < 15000) {
                InterlockedIncrement(&teamSwitchSkipped);
                gate::Event("coop_reghost peer machine=%u epoch %u->%u: a team switch %lu ms ago (role %d), the world stands, no reset", peer.machine, seenEpoch[slot],
                            readiness.peerEpoch, static_cast<unsigned long>(tick - changed), roles::RoleOf(slot, tick));
                seenEpoch[slot] = readiness.peerEpoch;
                epochSeenAt[slot] = 0;
                continue;
            }
        }
        if (waitForGrabs && seenEpoch[slot] && !mpmap::InVisit()) {
            uint32_t flags = 0;
            const uint32_t grab = GrabObject(flags);
            const DWORD tick = GetTickCount();
            if (grab) {
                if (!grabWaitSince[slot]) {
                    grabWaitSince[slot] = Stamp(tick);   // not tick | 1: an even tick ended the wait at once
                    InterlockedIncrement(&deferredForGrab);
                    gate::Event("coop_reghost peer machine=%u epoch %u->%u: a grab runs (coordinator %08X flags %02X), the reset waits", peer.machine,
                                seenEpoch[slot], readiness.peerEpoch, grab, flags);
                }
                if (tick - grabWaitSince[slot] < kGrabWaitMs) continue;
                InterlockedIncrement(&grabWaitsTimedOut);
                gate::Event("coop_reghost peer machine=%u: the grab still runs after %lu ms, reset anyway", peer.machine, static_cast<unsigned long>(kGrabWaitMs));
            }
            grabWaitSince[slot] = 0;
        }
        const uint32_t before = seenEpoch[slot];
        seenEpoch[slot] = readiness.peerEpoch;
        epochSeenAt[slot] = 0;
        const bool late = before == 0 && levelSettled && levelSettledSince && GetTickCount() - levelSettledSince > kLateJoinMs;
        if (before == 0 && !late) continue;          // the first epoch seen is the initial one
        if (mpmap::InVisit() && !late) {
            const unsigned run = mpmap::Run() + 1;
            if (resetRun[slot] == run) {
                if (InterlockedIncrement(&sameRunSkipped) <= 40)
                    gate::Event("coop_reghost peer machine=%u epoch %u->%u: a respawn on the MP map (run %u), the world stands, no reset", peer.machine, before,
                                readiness.peerEpoch, run - 1);
                continue;
            }
            resetRun[slot] = run;
        }
        if (!late && (roles::Spectating(slot, GetTickCount()) || (roles::spectatorBackAt[slot] && GetTickCount() - roles::spectatorBackAt[slot] < 15000))) {
            InterlockedIncrement(&spectatorSkipped);
            gate::Event("coop_reghost peer machine=%u epoch %u->%u: back from watching dead (a new Isaac at the checkpoint), the world stands, no reset", peer.machine,
                        before, readiness.peerEpoch);
            continue;
        }
        const unsigned tnlPort = ntohs(peer.port) - kDataPortOffset;
        const uint32_t connection = ConnectionTo(peer.ip, tnlPort);
        if (!connection) { InterlockedIncrement(&notFound); continue; }
        const bool ok = ResetAndActivate(connection);
        if (ok) InterlockedIncrement(&resets);
        if (late) InterlockedIncrement(&lateJoins);
        gate::Event("coop_reghost peer machine=%u epoch %u->%u connection=%08X %s%s", peer.machine, before,
                    readiness.peerEpoch, connection, ok ? "reset+activated" : "FAULTED", late ? " (joined while our level ran)" : "");
    }
}

// A joiner that has a link but no ghosts asks for this ([Lifecycle] JoinResync): the same reset
// and activation as a rebirth, at most once per 5 s per partner.
DWORD forcedAt[coop_peers::kMaxPeers]{};
volatile LONG forced = 0, forcedRefused = 0;
bool ForceReset(unsigned slot, const char* why) {
    if (!installed || slot >= coop_peers::kMaxPeers || !session) { InterlockedIncrement(&forcedRefused); return false; }
    const auto& peer = peerTable.peers[slot];
    const DWORD now = GetTickCount();
    if (!peer.Addressable() || (forcedAt[slot] && now - forcedAt[slot] < 5000)) { InterlockedIncrement(&forcedRefused); return false; }
    const uint32_t connection = ConnectionTo(peer.ip, ntohs(peer.port) - kDataPortOffset);
    if (!connection) { InterlockedIncrement(&notFound); return false; }
    forcedAt[slot] = Stamp(now);
    const bool ok = ResetAndActivate(connection);
    if (ok) InterlockedIncrement(&forced);
    gate::Event("coop_reghost peer machine=%u connection=%08X %s on request (%s)", peer.machine, connection, ok ? "reset+activated" : "FAULTED", why);
    return ok;
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "ReghostOnRebirth", 1, ini.c_str()) != 0;
    waitForGrabs = GetPrivateProfileIntA("Coop", "ReghostWaitsForGrabs", 1, ini.c_str()) != 0;
    resetOnTeamSwitch = GetPrivateProfileIntA("Coop", "ReghostOnTeamSwitch", 0, ini.c_str()) != 0;
    // push esi / mov esi,ecx / xor eax,eax / cmp [esi+0x1F8],eax
    static const unsigned char kReset[] = {0x56,0x8B,0xF1,0x33,0xC0,0x39,0x86,0xF8,0x01,0x00,0x00};
    // cmp dword [ecx+0x1F8],0 / je / inc dword [ecx+0x208]
    static const unsigned char kActivate[] = {0x83,0xB9,0xF8,0x01,0x00,0x00,0x00,0x0F,0x84};
    unsigned char a[16]{}, b[16]{};
    if (!Read(base + kResetGhosting, a, sizeof(kReset)) || std::memcmp(a, kReset, sizeof(kReset)) != 0 ||
        !Read(base + kActivateGhosting, b, sizeof(kActivate)) || std::memcmp(b, kActivate, sizeof(kActivate)) != 0) {
        std::fprintf(log, "coop_reghost signature mismatch (reset %08X / activate %08X); off\n",
                     static_cast<unsigned>(base + kResetGhosting), static_cast<unsigned>(base + kActivateGhosting));
        return false;
    }
    installed = true;
    std::fprintf(log, "coop_reghost enabled=%d reset=%08X activate=%08X\n", enabled,
                 static_cast<unsigned>(base + kResetGhosting), static_cast<unsigned>(base + kActivateGhosting));
    return true;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&resets, 0, 0) + InterlockedCompareExchange(&notFound, 0, 0) + InterlockedCompareExchange(&faults, 0, 0) +
                     InterlockedCompareExchange(&sameRunSkipped, 0, 0);
    const LONG grabState = InterlockedCompareExchange(&deferredForGrab, 0, 0) + InterlockedCompareExchange(&grabWaitsTimedOut, 0, 0) +
                           InterlockedCompareExchange(&forced, 0, 0) * 7 + InterlockedCompareExchange(&forcedRefused, 0, 0) * 11;
    if (now + grabState == last) return;
    last = now + grabState;
    std::fprintf(log, "coop_reghost resets=%ld not_found=%ld faults=%ld late_joins=%ld mp_respawns_without_reset=%ld deferred_for_grab=%ld grab_waits_timed_out=%ld "
                      "asked_resets=%ld asked_refused=%ld\n",
                 InterlockedCompareExchange(&resets, 0, 0), InterlockedCompareExchange(&notFound, 0, 0), InterlockedCompareExchange(&faults, 0, 0),
                 InterlockedCompareExchange(&lateJoins, 0, 0), InterlockedCompareExchange(&sameRunSkipped, 0, 0), InterlockedCompareExchange(&deferredForGrab, 0, 0),
                 InterlockedCompareExchange(&grabWaitsTimedOut, 0, 0), InterlockedCompareExchange(&forced, 0, 0), InterlockedCompareExchange(&forcedRefused, 0, 0));
}
}  // namespace reghost
