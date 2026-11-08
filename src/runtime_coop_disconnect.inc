// Dead Space 2 United by Richard Hafer.
// A partner that stops sending has to leave, and leave properly.
//
// Until now nothing removed one. A machine that crashed, was killed, or lost
// its network kept its seat: its readiness stayed true, so replicationReady,
// the progress gate and the checkpoint quorum all went on waiting for a vote
// that would never come, and its Isaac stood in the level forever. The engine
// would have given up eventually, but only eventually. Read out of a live
// session on 22.09., the two connections carried mPingTimeout 60000 with
// mPingRetryCount 30 on one side and 5000 with 16 on the other: half an hour
// and eighty seconds. In practice, never.
//
// The engine already has the whole mechanism, so this does not build a second
// one. NetConnection::checkTimeout (PC 0x01419F50, thiscall(now), ret 4) is
// called from NetInterface::processConnections every 1500 ms and reads exactly
// two fields of the connection:
//
//     +0xB0  mPingTimeout      how long silence may last before it pings
//     +0xB4  mPingRetryCount   how many pings go unanswered before it gives up
//     +0x110 mPingSendCount    pings sent since the last packet came in
//     +0x114 mLastPingSendTime when the last one went
//     +0x120                   set on a connection that must never time out
//
// Every packet received clears +0x110 and +0x114 (0x01418280), so the silence
// a partner is allowed is mPingTimeout * (mPingRetryCount + 1). When it runs
// out the engine sets the connection state to 9, calls onConnectionTerminated
// through the vtable and removes the connection, which takes that connection's
// ghosts with it, and with them the partner's body. That is the whole cleanup,
// done the way the engine does it, and it is why this file only writes two
// numbers instead of deleting anything itself.
//
// The side channel is ours and the engine knows nothing about it, so its peer
// slot is dropped here on the same deadline. That is the part that actually
// unblocks the session: the gates wait on readiness, and readiness lives in
// the slot.
//
// One trap, and the reason for the forgiveness below. A timeout measures the
// other machine's silence, but a stall on this machine looks the same: while it
// is frozen in a level load nothing is read from the socket, and afterwards
// every peer appears to have been quiet for the whole freeze. So whenever the
// local poll gap is longer than a second, that gap is handed back, to the peer
// table directly and to the engine through its own clearPingTimer.
namespace disconnect {
constexpr uint32_t kClearPingTimer = 0x1018280;   // NetConnection::clearPingTimer: mPingSendCount = mLastPingSendTime = 0
constexpr uint32_t kPingTimeout = 0xB0, kPingRetryCount = 0xB4, kNeverTimeout = 0x120;
constexpr uint32_t kRetries = 3;                  // silence allowed = interval * (retries + 1)

bool enabled = true, dropPeers = true, tuneEngine = true;
uint32_t timeoutMs = 10000, stallGraceMs = 1000;
bool installed = false;
volatile LONG tuned = 0, stallsForgiven = 0, dropped = 0, faults = 0;
// the host itself went silent: the match is over for this partner (matchend)
volatile LONG hostDropped = 0;
uint32_t lastPoll = 0, lastStallMs = 0, lastDropSilentMs = 0;

uint32_t PingInterval() {
    const uint32_t interval = timeoutMs / (kRetries + 1);
    return interval < 500 ? 500 : interval;
}

int Filter(unsigned) { InterlockedIncrement(&faults); return EXCEPTION_EXECUTE_HANDLER; }

// Writes the two numbers on every connection, and optionally hands back a
// stall. Cheap enough to repeat: connections come and go, and one that was
// made after the last pass would otherwise keep the engine's own defaults.
void TuneConnections(bool forgiveStall) {
    if (!tuneEngine || !session) return;
    uint32_t iface = 0, count = 0, array = 0;
    if (!Read(session + kSessionInterface, &iface, 4) || !iface ||
        !Read(iface + kInterfaceConnectionCount, &count, 4) || count > 16 ||
        !Read(iface + kInterfaceConnectionArray, &array, 4) || !array) return;
    const uint32_t interval = PingInterval(), retries = kRetries;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t connection = 0, never = 0, currentInterval = 0, currentRetries = 0;
        if (!Read(array + 4 * i, &connection, 4) || !connection) continue;
        if (Read(connection + kNeverTimeout, &never, 4) && never) continue;   // the engine marked this one exempt; leave it alone
        if (Read(connection + kPingTimeout, &currentInterval, 4) && currentInterval != interval &&
            WriteBytes(connection + kPingTimeout, &interval, 4)) InterlockedIncrement(&tuned);
        if (Read(connection + kPingRetryCount, &currentRetries, 4) && currentRetries != retries)
            WriteBytes(connection + kPingRetryCount, &retries, 4);
        if (!forgiveStall) continue;
        __try {
            using Clear = void (__thiscall*)(uint32_t);
            reinterpret_cast<Clear>(base + kClearPingTimer)(connection);
        } __except (Filter(GetExceptionCode())) {}
    }
}

// The side-channel seat. The engine's timeout takes the body and the
// connection; this takes the vote.
void DropPeer(unsigned slot, uint32_t silentMs) {
    auto& peer = peerTable.peers[slot];
    const uint32_t ip = peer.ip, machine = peer.machine, token = peer.token;
    const unsigned port = ntohs(peer.port);
    // Stop feeding a partner that is not listening. resetGhosting is the
    // engine's own end of ghosting for that connection and is already used
    // after a rebirth, so it is a known-good call here too.
    if (session && peer.Addressable() && port > kDataPortOffset) {
        if (const uint32_t connection = reghost::ConnectionTo(ip, port - kDataPortOffset))
            reghost::ResetOnly(connection);
    }
    // A configured partner keeps its seat and address (audit 16.10.: a client silent for
    // 30 s was erased for the session, and a client takes packets only from addresses it
    // was given); only who sat there goes, and its next hello takes the seat again.
    if (peer.configured) {
        peer.token = 0;
        peer.machine = 0xFFFFFFFFu;
        peer.lastSeen = 0;
        peer.relayed = 0;
    } else {
        peer = coop_peers::Peer{};
    }
    streams::ForgetPeer(slot);
    peerRuntime[slot] = PeerRuntime{};
    reghost::seenEpoch[slot] = 0;
    // Anything that machine was relaying goes with it: a forwarded player only
    // ever heard about through a peer that is gone has no other path here.
    if (token) {
        for (auto& other : peerTable.peers)
            if (other.used && other.relayed && other.token == token) other = coop_peers::Peer{};
    }
    lastDropSilentMs = silentMs;
    InterlockedIncrement(&dropped);
    if (!IsHost() && machine == 0) InterlockedExchange(&hostDropped, 1);
    notice::Show("CO-OP PARTNER TIMED OUT");
    char phase[80];
    lifecycle::PartnerLastPhase(machine, phase, sizeof(phase));
    gate::Event("coop_disconnect peer slot=%u machine=%d %u.%u.%u.%u:%u silent=%ums dropped (%s); seat and readiness released",
                slot, machine == kNoMachine ? -1 : static_cast<int>(machine),
                ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF, port, silentMs, phase);
}

void Poll(uint32_t now) {
    if (!enabled || !installed) return;
    // A local freeze is not their silence.
    bool forgiveStall = false;
    if (lastPoll) {
        const uint32_t gap = now - lastPoll;
        if (gap > stallGraceMs) {
            for (auto& peer : peerTable.peers) {
                if (!peer.used) continue;
                peer.lastSeen = (now - peer.lastSeen > gap) ? peer.lastSeen + gap : now;
            }
            lastStallMs = gap;
            forgiveStall = true;
            InterlockedIncrement(&stallsForgiven);
            gate::Event("coop_disconnect local stall of %u ms forgiven to every peer", gap);
        }
    }
    lastPoll = now;
    TuneConnections(forgiveStall);
    if (!dropPeers || forgiveStall) return;   // never drop on the frame a stall ended
    // Only a partner that was properly here can be considered gone. The first
    // build armed on the token alone and threw a partner out ten seconds into
    // its own connect (machine=-1, still handshaking), and from then on nobody
    // could see anybody (22.09. 19:2x). A token means it spoke once; a machine
    // index means the session accepted it.
    if (!inLevel) return;                 // a local level load is not their silence
    for (unsigned slot = 0; slot < coop_peers::kMaxPeers; ++slot) {
        auto& peer = peerTable.peers[slot];
        if (!peer.used || peer.relayed) continue;
        if (!peer.token || peer.machine == kNoMachine) continue;
        if (!peerRuntime[slot].readiness.havePeer) continue;   // never finished saying hello
        const uint32_t silent = now - peer.lastSeen;
        if (silent <= timeoutMs) continue;
        DropPeer(slot, silent);
    }
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "PeerTimeout", 0, ini.c_str()) != 0;   // off by default after 22.09.: it threw out a partner that was still connecting
    timeoutMs = static_cast<uint32_t>(GetPrivateProfileIntA("Coop", "PeerTimeoutMs", 30000, ini.c_str()));
    stallGraceMs = static_cast<uint32_t>(GetPrivateProfileIntA("Coop", "PeerTimeoutStallGraceMs", 1000, ini.c_str()));
    dropPeers = GetPrivateProfileIntA("Coop", "PeerTimeoutDropsSeat", 1, ini.c_str()) != 0;
    tuneEngine = GetPrivateProfileIntA("Coop", "PeerTimeoutTunesEngine", 1, ini.c_str()) != 0;
    if (timeoutMs < 10000) timeoutMs = 10000;     // below this a level load throws somebody out; ten seconds was already too short
    if (timeoutMs > 120000) timeoutMs = 120000;
    // mov eax, 0 / mov [ecx+0x114], eax / mov [ecx+0x110], eax / ret
    static const unsigned char kClear[] = {0x33, 0xC0, 0x89, 0x81, 0x14, 0x01, 0x00, 0x00, 0x89, 0x81, 0x10, 0x01, 0x00, 0x00, 0xC3};
    unsigned char actual[sizeof(kClear)]{};
    if (!Read(base + kClearPingTimer, actual, sizeof(actual)) || std::memcmp(actual, kClear, sizeof(kClear)) != 0) {
        std::fprintf(log, "coop_disconnect clearPingTimer signature mismatch at %08X; engine tuning off\n",
                     static_cast<unsigned>(base + kClearPingTimer));
        tuneEngine = false;
    }
    installed = true;
    std::fprintf(log, "coop_disconnect enabled=%d timeout=%ums ping_interval=%ums retries=%u stall_grace=%ums drops_seat=%d tunes_engine=%d\n",
                 enabled, timeoutMs, PingInterval(), kRetries, stallGraceMs, dropPeers, tuneEngine);
    return true;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&dropped, 0, 0) * 1000 +
                     InterlockedCompareExchange(&stallsForgiven, 0, 0) +
                     InterlockedCompareExchange(&faults, 0, 0);
    if (now == last) return;
    last = now;
    std::fprintf(log, "coop_disconnect dropped=%ld last_silent=%ums stalls_forgiven=%ld last_stall=%ums connections_tuned=%ld faults=%ld\n",
                 InterlockedCompareExchange(&dropped, 0, 0), lastDropSilentMs,
                 InterlockedCompareExchange(&stallsForgiven, 0, 0), lastStallMs,
                 InterlockedCompareExchange(&tuned, 0, 0), InterlockedCompareExchange(&faults, 0, 0));
}
}  // namespace disconnect
