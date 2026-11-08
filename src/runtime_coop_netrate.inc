// Dead Space 2 United by Hafer.
// The ghost's update rate.
//
// Measured in the 16:28 client dump (connection 0x34ABF820): TNL's rate
// block at connection+0xD8 holds minPacketSendPeriod=96, minPacketRecvPeriod
// =96, maxSendBandwidth=2500, maxRecvBandwidth=2500, and the remote's the
// same at +0xE8. NetConnection::computeNegotiatedRate (0x014182B0) takes
// max(local send period, remote recv period) into +0x100 and the packet size
// into +0xFC (240 bytes). So a partner's body was refreshed every 96 ms, ten
// times a second, on a 2.5 KB/s budget (Dead Space 2's console lobby
// defaults). That is the stutter, before any interpolation.
//
// NetConnection::setFixedRateParameters(sendPeriod, recvPeriod, sendBw,
// recvBw) is 0x01418350 (thiscall, ret 0x10): clears the adaptive bit (+0xBC
// bit 2), writes the four fields, sets +0xF8 so the next packet carries the
// new rate to the other side, and negotiates. Both ends call it with the same
// numbers, so the negotiated period is ours. [Coop] NetSendPeriodMs (default
// 32 = ~30 Hz) and NetBandwidth bytes/s (default 12000; 240 bytes at 30 Hz is
// 7.2 KB/s). Applied to every established connection, once per connection,
// and again if the game ever puts the defaults back.
namespace netrate {
bool enabled = false, installed = false;
constexpr uint32_t kSetFixedRateRva = 0x1018350;
constexpr uint32_t kRateBlock = 0xD8, kNegotiatedPeriod = 0x100, kPacketSize = 0xFC;
uint32_t sendPeriodMs = 32, bandwidth = 12000;
DWORD lastPass = 0;
volatile LONG applied = 0, faults = 0, connectionsSeen = 0;
uint32_t lastNegotiated = 0, lastPacketSize = 0;

int Filter(unsigned) { InterlockedIncrement(&faults); return EXCEPTION_EXECUTE_HANDLER; }
bool Apply(uint32_t connection) {
    using Set = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
    __try {
        reinterpret_cast<Set>(base + kSetFixedRateRva)(reinterpret_cast<void*>(connection), sendPeriodMs, sendPeriodMs, bandwidth, bandwidth);
        return true;
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}

// Game thread, from Poll: once a second over the interface's connections.
void Poll() {
    if (!installed || !enabled || !session) return;
    const DWORD now = GetTickCount();
    if (now - lastPass < 1000) return;
    lastPass = now;
    uint32_t iface = 0, count = 0, array = 0;
    if (!Read(session + kSessionInterface, &iface, 4) || !iface) return;
    if (!Read(iface + kInterfaceConnectionCount, &count, 4) || !Read(iface + kInterfaceConnectionArray, &array, 4)) return;
    for (uint32_t i = 0; i < count && i < 16; ++i) {
        uint32_t connection = 0, state = 0, current = 0;
        if (!Read(array + 4 * i, &connection, 4) || !connection) continue;
        if (!Read(connection + kConnectionState, &state, 4) || state != 7) continue;
        if (!Read(connection + kRateBlock, &current, 4)) continue;
        InterlockedIncrement(&connectionsSeen);
        if (current == sendPeriodMs) {
            Read(connection + kNegotiatedPeriod, &lastNegotiated, 4);
            Read(connection + kPacketSize, &lastPacketSize, 4);
            continue;
        }
        if (Apply(connection)) {
            InterlockedIncrement(&applied);
            Read(connection + kNegotiatedPeriod, &lastNegotiated, 4);
            Read(connection + kPacketSize, &lastPacketSize, 4);
            gate::Event("coop_netrate connection %08X: send period %u -> %u ms, bandwidth %u B/s (negotiated %u ms, packet %u bytes)",
                        connection, current, sendPeriodMs, bandwidth, lastNegotiated, lastPacketSize);
        }
    }
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "NetRate", 1, ini.c_str()) != 0;
    sendPeriodMs = static_cast<uint32_t>(GetPrivateProfileIntA("Coop", "NetSendPeriodMs", 32, ini.c_str()));
    bandwidth = static_cast<uint32_t>(GetPrivateProfileIntA("Coop", "NetBandwidth", 12000, ini.c_str()));
    if (sendPeriodMs < 8) sendPeriodMs = 8;
    if (sendPeriodMs > 500) sendPeriodMs = 500;
    if (bandwidth < 1000) bandwidth = 1000;
    // and dword [ecx+0xBC],-5 / mov eax,[esp+0x10] / mov edx,[esp+0xC]
    static const unsigned char kSig[] = {0x83,0xA1,0xBC,0x00,0x00,0x00,0xFB,0x8B,0x44,0x24,0x10,0x8B,0x54,0x24,0x0C};
    unsigned char actual[sizeof(kSig)]{};
    if (!Read(base + kSetFixedRateRva, actual, sizeof(kSig)) || std::memcmp(actual, kSig, sizeof(kSig)) != 0) {
        std::fprintf(log, "coop_netrate setFixedRateParameters signature mismatch at %08X; off\n", static_cast<unsigned>(base + kSetFixedRateRva));
        return false;
    }
    installed = true;
    std::fprintf(log, "coop_netrate enabled=%d send_period_ms=%u bandwidth=%u (TNL default was 96 ms / 2500 B/s)\n", enabled, sendPeriodMs, bandwidth);
    return true;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&applied, 0, 0) + InterlockedCompareExchange(&faults, 0, 0);
    if (now == last) return;
    last = now;
    std::fprintf(log, "coop_netrate applied=%ld faults=%ld connections_seen=%ld negotiated_ms=%u packet_bytes=%u\n",
                 InterlockedCompareExchange(&applied, 0, 0), InterlockedCompareExchange(&faults, 0, 0),
                 InterlockedCompareExchange(&connectionsSeen, 0, 0), lastNegotiated, lastPacketSize);
}
}  // namespace netrate
