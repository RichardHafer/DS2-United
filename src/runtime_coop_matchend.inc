// Dead Space 2 United by Richard Hafer.
// Leaving the match ends it for everybody (user, 16.10.: when the host leaves, the whole
// lobby closes and everyone is thrown out; afterwards host or join works again without a
// restart). [Coop] EndWithHost, on.
//
// The host back in the main menu tells every partner over the data channel for 3 s (a
// level packet with kFlagEnded), then ends its session. A partner that hears it, or whose
// host timed out (disconnect::hostDropped), goes back to the main menu the way the pause
// menu's quit does and ends its session there. Ending a session closes the menu lobby,
// forgets the partners and the level follow, and lifts the one-session-per-start block.
//
// The session goes the way GameManager::DestroyNetSession (0x015EFE50) ends retail's:
// UseScopeQuery(0) 0x00829BA0, DisableGhosting 0x008298F0, then vtable 0x01B22B8C slot 0
// (scalar deleting destructor 0x0082B610, thiscall(flags), ret 4) with flag 1. ~NetSession
// frees the local proxies, deletes the interface (every live connection gets a "Shutdown"
// disconnect, its ghosts go, the socket closes through our OnSocketDtor) and clears the
// singleton [0x020216DC]; a later StartSession builds a new one on the same port. Only in
// the main menu with no level (a PairedAttackCoordinator alive later calls the singleton
// without a null check, 0x010E62E0), with every established connection in state 7 (the
// interface destructor loops forever on any other), no listeners left on the session, and
// not from inside the engine's network code. Evidence:
// analysis/COOP_SESSION_TEARDOWN_2026-10-16.md. When a guard keeps refusing for 10 s, or the menu's host
// or join item wants it now, the connections are dropped instead (TNL::NetInterface::
// disconnect 0x0141B980, removePendingConnection 0x0141B1C0) and the idle session is kept
// for the next role; the session itself is symmetric, host and client exist only here.
namespace matchend {
bool enabled = true;
enum class Phase : int { Idle, Telling, Leaving, Ending };
const char* const kPhases[] = {"idle", "telling", "leaving", "ending"};
Phase phase = Phase::Idle;
int PhaseIndex() { return static_cast<int>(phase); }   // runtime_coop_lifecycle.inc: 0 while no match end runs
DWORD phaseAt = 0, lastTell = 0, lastKick = 0, lastTry = 0;
bool sawLevel = false, reusable = false;
char why[96] = "";
char lastRetry[96] = "";
volatile LONG hostEnds = 0, tells = 0, kicks = 0, ends = 0, teardowns = 0, keptSessions = 0, retries = 0, connectionsDropped = 0, pendingReleased = 0,
              teardownFaults = 0;
char lastResult[200] = "none";

constexpr uint32_t kSessionVtable = 0x1722B8C, kScalarDtor = 0x42B610, kSessionDtor = 0x42B000, kUseScopeQuery = 0x429BA0, kDisableGhosting = 0x4298F0;
constexpr uint32_t kRemovePending = 0x101B1C0, kIfaceDtor = 0x101BCA0, kNetDisconnect = 0x101B980, kShutdownText = 0x1799540;
constexpr uint32_t kTsAllocator = 0x1D38D20, kNetEventPtr = 0x1C872E8, kScopeObject = 0x6C, kListenersA = 0x4C, kListenersB = 0x58;
constexpr uint32_t kProxyHead = 0x28, kProxyNext = 0x34, kProxyFlags = 0x30, kScheduledCount = 0x34, kLagFlag = 0x8C;

bool InFrontend() {
    uint32_t director = 0;
    return Read(base + 0x1C8BE90, &director, 4) && director;   // the Apt director exists in the main menu only
}
// The engine's session, EARS interface and TNL code: Poll driven from the getter there must not delete the session.
bool InNetCode(uint32_t address) {
    const uint32_t rva = address - base;
    return (rva >= 0x428000 && rva < 0x42C000) || (rva >= 0x456000 && rva < 0x458200) || (rva >= 0x1010000 && rva < 0x1022000);
}

int signatures = -1;   // -1 not checked, 0 mismatch, 1 ok
bool SignaturesOk(FILE* log = nullptr) {
    if (signatures >= 0) return signatures == 1;
    struct Sig { uint32_t rva; unsigned char bytes[16]; size_t size; const char* name; };
    static const Sig kSigs[] = {
        {kScalarDtor, {0x56, 0x8B, 0xF1, 0xE8, 0xE8, 0xF9, 0xFF, 0xFF, 0xF6, 0x44, 0x24, 0x08, 0x01, 0x74, 0x09, 0x56}, 16, "NetSession scalar dtor"},
        {kSessionDtor, {0x83, 0xEC, 0x0C, 0x55, 0x56, 0x8B, 0xF1, 0x80, 0xBE, 0x8C, 0x00, 0x00, 0x00, 0x00, 0x8D, 0x6E}, 16, "~NetSession"},
        {kUseScopeQuery, {0x8B, 0x41, 0x6C, 0x8B, 0x4C, 0x24, 0x04, 0x89, 0x48, 0x34, 0xC2, 0x04, 0x00}, 13, "UseScopeQuery"},
        {kDisableGhosting, {0x8B, 0x49, 0x1C, 0x85, 0xC9, 0x74, 0x05, 0xE9, 0xE4, 0xE3, 0x02, 0x00, 0xC3}, 13, "DisableGhosting"},
        {kRemovePending, {0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x57, 0x24, 0x33, 0xF6, 0x85, 0xD2, 0x7E, 0x4A, 0x8B, 0x47, 0x2C}, 16, "removePendingConnection"},
        {kIfaceDtor, {0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x0C, 0x00, 0xC7, 0x06, 0x4C, 0x95, 0xB9, 0x01, 0x74, 0x1B, 0x90}, 16, "~TNL::NetInterface"},
        {kNetDisconnect, {0x81, 0xEC, 0xB0, 0x07, 0x00, 0x00, 0x55, 0x56, 0x8B, 0xB4, 0x24, 0xBC, 0x07, 0x00, 0x00, 0x8B}, 16, "NetInterface::disconnect"},
    };
    signatures = 1;
    for (const Sig& sig : kSigs) {
        unsigned char have[16]{};
        const bool ok = Read(base + sig.rva, have, sig.size) && std::memcmp(have, sig.bytes, sig.size) == 0;
        if (!ok) signatures = 0;
        if (log) std::fprintf(log, "matchend signature %s %08X: %s\n", sig.name, static_cast<unsigned>(base + sig.rva), ok ? "ok" : "MISMATCH");
    }
    uint32_t slot0 = 0;
    const bool slotOk = Read(base + kSessionVtable, &slot0, 4) && slot0 == base + kScalarDtor;
    if (!slotOk) signatures = 0;
    char text[12]{};
    const bool textOk = Read(base + kShutdownText, text, 9) && std::strcmp(text, "Shutdown") == 0;
    if (!textOk) signatures = 0;
    if (log) std::fprintf(log, "matchend NetSession vtable slot 0 %s, \"Shutdown\" text %s\n", slotOk ? "ok" : "MISMATCH", textOk ? "ok" : "MISMATCH");
    return signatures == 1;
}

struct Links { uint32_t iface = 0, count = 0, array = 0, pending = 0, pendingArray = 0; };
bool ReadLinks(uint32_t s, Links& l) {
    return s && Read(s + kSessionInterface, &l.iface, 4) && l.iface && Read(l.iface + kInterfaceConnectionCount, &l.count, 4) &&
           Read(l.iface + kInterfaceConnectionArray, &l.array, 4) && Read(l.iface + kInterfacePendingCount, &l.pending, 4) &&
           Read(l.iface + kInterfacePendingArray, &l.pendingArray, 4) && l.count <= 64 && l.pending <= 64;
}

// Who may run as a new role on the session that stayed: nobody connected, nothing pending.
bool Reusable() {
    Links l;
    return reusable && session && !faulted && ReadLinks(session, l) && !l.count && !l.pending;
}

// Releases the pending connections one by one; stops when a count does not fall.
unsigned ReleasePending(uint32_t iface) {
    unsigned released = 0;
    for (unsigned guard = 0; guard < 64; ++guard) {
        uint32_t pending = 0, array = 0, connection = 0;
        if (!Read(iface + kInterfacePendingCount, &pending, 4) || !pending || !Read(iface + kInterfacePendingArray, &array, 4) || !array ||
            !Read(array + 4 * (pending - 1), &connection, 4) || !connection)
            break;
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kRemovePending)(reinterpret_cast<void*>(static_cast<uintptr_t>(iface)), connection);
        uint32_t after = pending;
        Read(iface + kInterfacePendingCount, &after, 4);
        if (after >= pending) break;
        ++released;
    }
    InterlockedExchangeAdd(&pendingReleased, static_cast<LONG>(released));
    return released;
}

// What the old session left in our own state.
void ForgetSessionState() {
    registeredPlayer = registeredSubKey = registeredProxy = 0;
    for (auto& r : botnet::registered) r = 0;
    for (auto& e : reghost::seenEpoch) e = 0;
    for (auto& r : reghost::resetRun) r = 0;
    necro::necroProxy = necro::necroSubKey = 0;
    necro::respawnManager = necro::respawnManagerSubKey = 0;
    InterlockedExchange(&necro::nrmRegistered, 0);
    necro::nrmRegisterTries = 0;
    InterlockedExchange(&ghostAttachEntity, 0);
    attachedGhostCount = 0;
    std::memset(attachedGhosts, 0, sizeof(attachedGhosts));
    std::memset(steadyGhost, 0, sizeof(steadyGhost));
    disconnect::lastPoll = 0;
    InterlockedExchange(&disconnect::hostDropped, 0);
    connectStarted = 0;
    connected = replicationReady = false;
}

enum class Outcome { Torn, Retry, Refused };
Outcome TearDown(char* detail, size_t size) {
    const uint32_t s = session;
    if (faulted) { std::snprintf(detail, size, "refused: the co-op plugin faulted earlier"); return Outcome::Refused; }
    if (!SignaturesOk()) { std::snprintf(detail, size, "refused: an engine signature differs from retail"); return Outcome::Refused; }
    if (!InFrontend() || inLevel) { std::snprintf(detail, size, "waiting: not in the main menu yet"); return Outcome::Retry; }
    if (gateDispatchDepth > 0 || (driveCaller && InNetCode(driveCaller))) {
        std::snprintf(detail, size, "waiting: Poll runs inside %s", gateDispatchDepth > 0 ? "a message dispatch" : "the engine's network code");
        return Outcome::Retry;
    }
    uint32_t vtable = 0, singleton = 0, scope = 0, listenersA = 0, listenersB = 0, scheduled = 0;
    unsigned char lag = 0;
    if (!Read(s, &vtable, 4) || vtable != base + kSessionVtable) { std::snprintf(detail, size, "refused: session %08X has vtable %08X", s, vtable); return Outcome::Refused; }
    if (!Read(base + kSessionSingleton, &singleton, 4) || singleton != s) {
        std::snprintf(detail, size, "refused: the singleton is %08X, not our %08X", singleton, s);
        return Outcome::Refused;
    }
    Links l;
    if (!ReadLinks(s, l)) { std::snprintf(detail, size, "refused: the interface is unreadable"); return Outcome::Refused; }
    for (uint32_t i = 0; i < l.count; ++i) {
        uint32_t connection = 0, state = 0;
        if (!Read(l.array + 4 * i, &connection, 4) || !connection || !Read(connection + kConnectionState, &state, 4) || state != 7) {
            std::snprintf(detail, size, "waiting: connection %u of %u (%08X) is in state %u, not 7", i + 1, l.count, connection, state);
            return Outcome::Retry;
        }
    }
    Read(s + kListenersA, &listenersA, 4);
    Read(s + kListenersB, &listenersB, 4);
    if (listenersA || listenersB) {
        std::snprintf(detail, size, "waiting: %u + %u listeners still on the session", listenersA, listenersB);
        return Outcome::Retry;
    }
    Read(s + kScopeObject, &scope, 4);
    Read(s + kScheduledCount, &scheduled, 4);
    Read(s + kLagFlag, &lag, 1);
    unsigned proxies = 0, ghostProxies = 0;
    uint32_t proxy = 0;
    Read(s + kProxyHead, &proxy, 4);
    for (unsigned guard = 0; proxy && guard < 512; ++guard) {
        uint32_t flags = 0;
        Read(proxy + kProxyFlags, &flags, 4);
        ++proxies;
        if (flags & 2) ++ghostProxies;
        if (!Read(proxy + kProxyNext, &proxy, 4)) break;
    }
    gate::Event("matchend teardown: session %08X iface %08X, %u connection(s), %u pending, %u proxies (%u ghost), %u scheduled, scope %08X, lag byte %u, socket users %u",
                s, l.iface, l.count, l.pending, proxies, ghostProxies, scheduled, scope, lag, socketUsers);
    const DWORD started = GetTickCount();
    session = 0;   // nested plugin code sees no session from here on
    connected = replicationReady = false;
    unsigned released = 0;
    uint32_t faultCode = 0;
    __try {
        released = ReleasePending(l.iface);
        if (scope) reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kUseScopeQuery)(reinterpret_cast<void*>(static_cast<uintptr_t>(s)), 0);
        reinterpret_cast<void(__thiscall*)(void*)>(base + kDisableGhosting)(reinterpret_cast<void*>(static_cast<uintptr_t>(s)));
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kScalarDtor)(reinterpret_cast<void*>(static_cast<uintptr_t>(s)), 1);
    } __except (faultCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&teardownFaults);
        faulted = true;   // the engine's state is not to be trusted any more
        std::snprintf(detail, size, "FAULT %08X inside the teardown: co-op off until a restart", faultCode);
        return Outcome::Refused;
    }
    uint32_t singletonAfter = 0, allocator = 0, eventPtr = 0;
    Read(base + kSessionSingleton, &singletonAfter, 4);
    Read(base + kTsAllocator, &allocator, 4);
    Read(base + kNetEventPtr, &eventPtr, 4);
    ForgetSessionState();
    std::snprintf(detail, size, "%u pending released, singleton %08X, socket %s (users %u), allocator %08X, event ptr %08X, %lu ms", released, singletonAfter,
                  udp == INVALID_SOCKET ? "closed" : "STILL OPEN", socketUsers, allocator, eventPtr, static_cast<unsigned long>(GetTickCount() - started));
    return Outcome::Torn;
}

// The fallback: every connection disconnected the engine's way, the session kept.
bool DropConnections(char* detail, size_t size) {
    Links l;
    if (!ReadLinks(session, l)) { std::snprintf(detail, size, "connections unreadable"); return false; }
    unsigned dropped = 0, released = 0;
    uint32_t faultCode = 0;
    __try {
        for (unsigned guard = 0; guard < 64; ++guard) {
            uint32_t count = 0, array = 0, connection = 0, state = 0;
            if (!Read(l.iface + kInterfaceConnectionCount, &count, 4) || !count || !Read(l.iface + kInterfaceConnectionArray, &array, 4) || !array ||
                !Read(array, &connection, 4) || !connection || !Read(connection + kConnectionState, &state, 4))
                break;
            if (state != 7 && state != 1 && state != 4) break;   // disconnect does nothing in any other state
            reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t, const char*)>(base + kNetDisconnect)(
                reinterpret_cast<void*>(static_cast<uintptr_t>(l.iface)), connection, 5, reinterpret_cast<const char*>(base + kShutdownText));
            uint32_t after = count;
            Read(l.iface + kInterfaceConnectionCount, &after, 4);
            if (after >= count) break;
            ++dropped;
        }
        released = ReleasePending(l.iface);
    } __except (faultCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&teardownFaults);
        std::snprintf(detail, size, "FAULT %08X while dropping connections", faultCode);
        return false;
    }
    InterlockedExchangeAdd(&connectionsDropped, static_cast<LONG>(dropped));
    ReadLinks(session, l);
    std::snprintf(detail, size, "%u connection(s) dropped, %u pending released, %u and %u left", dropped, released, l.count, l.pending);
    return !l.count && !l.pending;
}

void Settle(const char* how, bool reset) {
    level::ResetFollow();
    InterlockedExchange(&disconnect::hostDropped, 0);
    if (reset) {
        ResetPartners();
        network = false;
        machineIndex = 0;
        connectStarted = 0;
        ApplyRole("the match ended");
    }
    lobby::Stop();
    InterlockedIncrement(&ends);
    std::snprintf(lastResult, sizeof(lastResult), "%s: %s", why, how);
    gate::Event("matchend: %s", lastResult);
    phase = Phase::Idle;
    sawLevel = false;
    lastRetry[0] = 0;
}

// One try at ending; `patient`: a guard that may clear soon waits for the next try.
bool TryEnd(DWORD now, bool patient) {
    char detail[160] = "", how[200];
    const uint32_t old = session;
    if (!old) {
        Settle("no session to end", true);
        return true;
    }
    const Outcome o = TearDown(detail, sizeof(detail));
    if (o == Outcome::Torn) {
        InterlockedIncrement(&teardowns);
        reusable = false;
        std::snprintf(how, sizeof(how), "session %08X ended (%s)", old, detail);
        Settle(how, true);
        return true;
    }
    if (o == Outcome::Retry && patient) {
        InterlockedIncrement(&retries);
        if (std::strcmp(lastRetry, detail) != 0) {
            std::snprintf(lastRetry, sizeof(lastRetry), "%s", detail);
            gate::Event("matchend teardown %s (%lu ms in)", detail, static_cast<unsigned long>(now - phaseAt));
        }
        return false;
    }
    char dropped[96] = "";
    const bool ok = !faulted && DropConnections(dropped, sizeof(dropped));
    reusable = ok;
    InterlockedIncrement(&keptSessions);
    std::snprintf(how, sizeof(how), "session %08X kept (%s); %s", old, detail, ok ? dropped : faulted ? "co-op faulted, restart the game" : dropped);
    Settle(how, ok);
    return true;
}

bool Busy() { return phase != Phase::Idle; }
// The menu's host or join item while the last match is still being ended: finish it now.
void EndNow() {
    if (phase == Phase::Telling || phase == Phase::Ending || (phase == Phase::Leaving && InFrontend())) TryEnd(GetTickCount(), false);
}

void Tick(DWORD now) {
    if (!enabled || fromEnvironmentRole) return;
    const bool frontend = InFrontend();
    if (session && inLevel && !frontend) sawLevel = true;
    switch (phase) {
    case Phase::Idle:
        if (!session || !network) return;
        if (IsHost()) {
            if (!sawLevel || !frontend) return;
            phase = Phase::Telling;
            phaseAt = now;
            lastTell = 0;
            InterlockedIncrement(&hostEnds);
            std::snprintf(why, sizeof(why), "the host left the match");
            gate::Event("matchend: the host is back in the main menu; telling %u partner(s) for 3 s, then ending session %08X", peerTable.Count(), session);
            return;
        }
        if (InterlockedCompareExchange(&level::hostEnded, 0, 0)) std::snprintf(why, sizeof(why), "the host ended the match");
        else if (InterlockedCompareExchange(&disconnect::hostDropped, 0, 0)) std::snprintf(why, sizeof(why), "the host timed out");
        else return;
        phase = Phase::Leaving;
        phaseAt = now;
        lastKick = 0;
        InterlockedIncrement(&kicks);
        gate::Event("matchend: %s; %s", why, frontend ? "already in the main menu" : "back to the main menu");
        if (!frontend) notice::Show(InterlockedCompareExchange(&level::hostEnded, 0, 0) ? "THE HOST ENDED THE MATCH" : "THE HOST IS GONE - MATCH OVER");
        return;
    case Phase::Telling:
        if (now - lastTell >= 250) {
            lastTell = now;
            if (level::SendEnded()) InterlockedIncrement(&tells);
        }
        if (now - phaseAt >= 3000) {
            phase = Phase::Ending;
            phaseAt = now;
            lastTry = 0;
        }
        return;
    case Phase::Leaving:
        if (frontend) {
            phase = Phase::Ending;
            phaseAt = now;
            lastTry = 0;
            return;
        }
        // the pause menu's quit; once more after 10 s if the level change was refused
        if (!lastKick || (now - lastKick >= 10000 && now - phaseAt < 40000)) {
            lastKick = now;
            if (mpmap::InVisit()) mpmap::ReturnToCampaign(now, why);
            else {
                const bool ok = !LevelChangePending() && mpmap::LaunchFrontendRaw();
                gate::Event("matchend: LaunchFrontend(1) to the main menu %s", ok ? "called" : "REFUSED (a level change is pending or the call faulted)");
            }
        }
        return;
    case Phase::Ending:
        if (lastTry && now - lastTry < 250) return;
        lastTry = now;
        TryEnd(now, now - phaseAt < 10000);
        return;
    }
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "EndWithHost", 1, ini.c_str()) != 0;
    const bool sigs = SignaturesOk(log);
    std::fprintf(log, "matchend %s ([Coop] EndWithHost): the host leaving the match ends it for every partner, then any PC may host or join again; session teardown %s\n",
                 enabled ? "on" : "off", sigs ? "verified" : "NOT verified (the connections are dropped instead)");
}
void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&hostEnds, 0, 0) + InterlockedCompareExchange(&kicks, 0, 0) * 16 + InterlockedCompareExchange(&ends, 0, 0) * 256 +
                     InterlockedCompareExchange(&tells, 0, 0) * 4096 + static_cast<LONG>(phase) * 1048576 + InterlockedCompareExchange(&retries, 0, 0) * 8;
    if (now == last) return;
    last = now;
    std::fprintf(log, "matchend phase=%s host_ends=%ld told=%ld kicks=%ld ends=%ld teardowns=%ld kept=%ld retries=%ld dropped=%ld pending_released=%ld faults=%ld "
                      "ended_in=%ld reusable=%d last='%s'\n",
                 kPhases[static_cast<int>(phase)], InterlockedCompareExchange(&hostEnds, 0, 0), InterlockedCompareExchange(&tells, 0, 0),
                 InterlockedCompareExchange(&kicks, 0, 0), InterlockedCompareExchange(&ends, 0, 0), InterlockedCompareExchange(&teardowns, 0, 0),
                 InterlockedCompareExchange(&keptSessions, 0, 0), InterlockedCompareExchange(&retries, 0, 0), InterlockedCompareExchange(&connectionsDropped, 0, 0),
                 InterlockedCompareExchange(&pendingReleased, 0, 0), InterlockedCompareExchange(&teardownFaults, 0, 0),
                 InterlockedCompareExchange(&level::endedIn, 0, 0), reusable ? 1 : 0, lastResult);
}
}  // namespace matchend
