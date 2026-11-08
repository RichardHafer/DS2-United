// Dead Space 2 United by Richard Hafer.
// The host's session table ([Coop] MeshRoster) and its seed ([Coop] SeedSync).
// TNL hands no ghost on to a third connection, so with three or more players every
// machine has to dial every other one. The lobby only tells a joiner about the
// players of its own batch, and a launcher star tells clients nothing at all, so the
// host sends everybody the full table: twice a second for 10 s after a change, every
// 5 s otherwise. A client dials whoever it does not know yet. The same packet carries
// the host's randomizer seed, which a client plays for as long as it is in the session.
namespace mesh {
constexpr uint32_t kMagic = 0x5453524Du;   // "MRST"
constexpr uint32_t kVersion = 1;
constexpr unsigned kEntries = coop_peers::kMaxPeers - 1;
#pragma pack(push, 1)
struct Entry { uint32_t machine, token, ip; uint16_t gamePort, flags; };
struct Packet {
    uint32_t magic, version, token, serial, seed;
    uint8_t count, pad[3];
    Entry entries[kEntries];
};
#pragma pack(pop)
static_assert(sizeof(Packet) == kPacketBytes, "Unexpected roster layout");

bool roster = true, seedSync = true;
uint32_t ownSeed = 0;
uint32_t serial = 0, signature = 0;
DWORD burstUntil = 0, lastSend = 0, lastStatus = 0;
uint32_t heardSerial = 0, heardToken = 0, adoptedSeed = 0;
char lastStatusLine[400] = "";
using AdoptFn = int(__cdecl*)(unsigned, int);
AdoptFn adopt = nullptr;
bool hasAdoptedSeed = false;
volatile LONG sent = 0, received = 0, ignored = 0, dialledFromRoster = 0, loopbackSkipped = 0, seedAdoptions = 0, seedReverts = 0, seedFailures = 0;

bool Loopback(uint32_t ipNet) { return (ntohl(ipNet) >> 24) == 127; }
const char* Ip(uint32_t ipNet, char (&out)[20]) {
    const uint32_t h = ntohl(ipNet);
    std::snprintf(out, sizeof(out), "%u.%u.%u.%u", h >> 24, (h >> 16) & 255, (h >> 8) & 255, h & 255);
    return out;
}

AdoptFn Adopter() {
    if (!adopt) {
        if (const HMODULE module = ds2coop::PartnerModule("DS2RandomizerObserver.asi"))
            adopt = reinterpret_cast<AdoptFn>(GetProcAddress(module, "DS2Randomizer_SetSessionSeed"));
        if (adopt) gate::Event("mesh: session seed export found");
    }
    return adopt;
}

// Game thread (the receive loop and ResetPartners run there).
void AdoptSeed(uint32_t seed, const char* why, bool active = true) {
    if (active == hasAdoptedSeed && seed == adoptedSeed) return;
    AdoptFn fn = Adopter();
    if (!fn) { InterlockedIncrement(&seedFailures); return; }
    const int result = fn(seed, active);
    if (result < 0) { InterlockedIncrement(&seedFailures); return; }
    gate::Event("mesh: seed %s %u (%s): result %d (1 enemy plans, 2 blueprints, 4 seed set, 0 unchanged)", active ? "adopted:" : "reverted, was", active ? seed : adoptedSeed,
                why, result);
    InterlockedIncrement(active ? &seedAdoptions : &seedReverts);
    hasAdoptedSeed = active;
    adoptedSeed = seed;
}

// The host's table, built from the peers it talks to directly.
unsigned Build(Packet& p) {
    p = Packet{};
    p.magic = kMagic;
    p.version = kVersion;
    p.token = peerReadiness.token;
    p.seed = seedSync ? ownSeed : 0;
    p.pad[0] = seedSync ? 1 : 0;
    if (!roster) return 0;
    unsigned n = 0;
    for (const auto& peer : peerTable.peers) {
        if (!peer.Addressable() || !peer.token || peer.machine == 0xFFFFFFFFu || n >= kEntries) continue;
        const unsigned dataPort = ntohs(peer.port);
        if (dataPort <= kDataPortOffset) continue;
        Entry& e = p.entries[n++];
        e.machine = peer.machine;
        e.token = peer.token;
        e.ip = peer.ip;
        e.gamePort = static_cast<uint16_t>(dataPort - kDataPortOffset);
    }
    p.count = static_cast<uint8_t>(n);
    return n;
}

uint32_t Signature(const Packet& p) {
    uint32_t h = 2166136261u ^ p.seed;
    for (unsigned i = 0; i < p.count; ++i) {
        const Entry& e = p.entries[i];
        for (uint32_t v : {e.machine, e.token, e.ip, static_cast<uint32_t>(e.gamePort)}) h = (h ^ v) * 16777619u;
    }
    return h ^ p.count;
}

// What every machine knows about its partners, logged when it changes.
void LogStatus(DWORD now) {
    if (now - lastStatus < 5000) return;
    lastStatus = now;
    char line[400];
    size_t used = 0;
    unsigned direct = 0, relayedOnly = 0, tnl = 0;
    for (const auto& peer : peerTable.peers) {
        if (!peer.used || !peer.token) continue;
        char ip[20];
        bool connectedTnl = false;
        if (peer.Addressable() && ntohs(peer.port) > kDataPortOffset)
            connectedTnl = reghost::AnyConnectionTo(peer.ip, ntohs(peer.port) - kDataPortOffset);
        if (peer.relayed) ++relayedOnly; else ++direct;
        if (connectedTnl) ++tnl;
        if (used < sizeof(line) - 60)
            used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, " m%d=%s%s%s", peer.machine == 0xFFFFFFFFu ? -1 : static_cast<int>(peer.machine),
                                                      peer.relayed ? "relayed" : Ip(peer.ip, ip), peer.relayed ? "" : connectedTnl ? "+tnl" : "-tnl",
                                                      peer.configured ? "" : "(learned)"));
    }
    char status[400];
    std::snprintf(status, sizeof(status), "%u partner(s), %u direct, %u relayed only, %u with a body connection, %u dialled:%s", direct + relayedOnly, direct, relayedOnly, tnl,
                  dialledCount, used ? line : " none");
    if (std::strcmp(status, lastStatusLine) == 0) return;
    std::snprintf(lastStatusLine, sizeof(lastStatusLine), "%s", status);
    gate::Event("mesh: machine %u%s: %s", machineIndex, IsHost() ? " (host)" : "", status);
    if (relayedOnly && !IsHost())
        gate::Event("mesh: %u partner(s) only through the host: no body of theirs on this machine until the host's table names them", relayedOnly);
}

void Tick(DWORD now) {
    if (!network || dataSock == INVALID_SOCKET) return;
    LogStatus(now);
    if ((!roster && !seedSync) || !IsHost() || !peerTable.Count()) return;
    Packet p;
    Build(p);
    const uint32_t sig = Signature(p);
    if (sig != signature) {
        signature = sig;
        ++serial;
        burstUntil = now + 10000;
        gate::Event("mesh: host table serial %u: %u partner(s), seed %u", serial, p.count, p.seed);
    }
    const DWORD every = static_cast<LONG>(burstUntil - now) > 0 ? 500 : 5000;
    if (now - lastSend < every) return;
    lastSend = now;
    p.serial = serial;
    for (const auto& to : peerTable.peers) {
        if (!to.Addressable()) continue;
        Packet out = p;
        // a partner on the host's own PC is 127.x to the host, which means nothing to a remote machine
        if (!Loopback(to.ip)) {
            unsigned kept = 0;
            for (unsigned i = 0; i < p.count; ++i) {
                if (Loopback(p.entries[i].ip)) { InterlockedIncrement(&loopbackSkipped); continue; }
                out.entries[kept++] = p.entries[i];
            }
            for (unsigned i = kept; i < kEntries; ++i) out.entries[i] = Entry{};
            out.count = static_cast<uint8_t>(kept);
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = to.ip;
        addr.sin_port = to.port;
        if (::sendto(dataSock, reinterpret_cast<const char*>(&out), sizeof(out), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == sizeof(out))
            InterlockedIncrement(&sent);
    }
}

// Receive loop, game thread. False when the bytes are not ours.
bool OnBytes(const void* data, int size, unsigned slot, DWORD now) {
    if (size != static_cast<int>(sizeof(Packet))) return false;
    Packet p{};
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != kMagic) return false;
    InterlockedIncrement(&received);
    // only a client takes the host's table; the host is machine 0 (or not identified yet)
    const auto& from = peerTable.peers[slot < coop_peers::kMaxPeers ? slot : 0];
    if (p.version != kVersion || IsHost() || from.token != p.token || (from.machine != 0 && from.machine != 0xFFFFFFFFu) || p.count > kEntries ||
        (p.token == heardToken && p.serial != heardSerial && !coop_peers::NewerSerial(p.serial, heardSerial))) {
        InterlockedIncrement(&ignored);
        return true;
    }
    heardSerial = p.serial;
    heardToken = p.token;
    if (roster) {
        unsigned added = 0;
        for (unsigned i = 0; i < p.count; ++i) {
            const Entry& e = p.entries[i];
            if (!e.machine || e.machine == machineIndex || !e.ip || !e.gamePort) continue;
            const uint16_t dataPort = htons(static_cast<u_short>(e.gamePort + kDataPortOffset));
            bool dialled = false;
            for (unsigned d = 0; d < dialledCount; ++d) dialled |= dialledIps[d] == e.ip && dialledPorts[d] == e.gamePort;
            if (dialled && peerTable.Find(e.ip, dataPort)) continue;
            const unsigned before = dialledCount;
            AddDial(e.ip, e.gamePort);
            if (!peerTable.Find(e.ip, dataPort)) peerTable.Configure(e.ip, dataPort, static_cast<uint32_t>(now));
            char ip[20];
            gate::Event("mesh: the host's table (serial %u) names machine %u at %s:%u, token %08X: %s (%u dialled)", p.serial, e.machine, Ip(e.ip, ip), e.gamePort, e.token,
                        dialledCount > before ? "dialled" : dialledCount >= kMaxDialled ? "NOT dialled, the dial list is full" : "side channel opened", dialledCount);
            InterlockedIncrement(&dialledFromRoster);
            ++added;
        }
        // the redial loop connects new entries; no need to wait out its 12 s
        if (added && session) connectStarted = GetTickCount() - 12001;
    }
    if (seedSync) AdoptSeed(p.seed, "the host's table", p.seed != 0 || p.pad[0] == 1);
    return true;
}

// A role picked in the menu starts over: back to the INI seed.
void OnPartnersReset() {
    heardSerial = heardToken = 0;
    signature = 0;
    lastStatusLine[0] = 0;
    if (hasAdoptedSeed) AdoptSeed(0, "the session ended", false);
}

void Install(FILE* log, const std::string& ini) {
    roster = GetPrivateProfileIntA("Coop", "MeshRoster", 1, ini.c_str()) != 0;
    seedSync = GetPrivateProfileIntA("Coop", "SeedSync", 1, ini.c_str()) != 0;
    ownSeed = static_cast<uint32_t>(GetPrivateProfileIntA("DS2Randomizer", "Seed", 1, ini.c_str()));
    std::fprintf(log, "mesh: host table %s ([Coop] MeshRoster), seed sync %s ([Coop] SeedSync), own seed %u, packet %zu bytes\n", roster ? "on" : "off",
                 seedSync ? "on" : "off", ownSeed, sizeof(Packet));
}

void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG sum = InterlockedCompareExchange(&sent, 0, 0) / 20 + InterlockedCompareExchange(&received, 0, 0) / 20 + InterlockedCompareExchange(&dialledFromRoster, 0, 0) * 7 +
                     InterlockedCompareExchange(&seedAdoptions, 0, 0) * 11 + InterlockedCompareExchange(&seedFailures, 0, 0) * 13 + InterlockedCompareExchange(&ignored, 0, 0) * 3;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "mesh sent=%ld received=%ld ignored=%ld dialled_from_table=%ld loopback_skipped=%ld serial=%u heard_serial=%u seed_adopted=%u adoptions=%ld reverts=%ld seed_failures=%ld\n",
                 InterlockedCompareExchange(&sent, 0, 0), InterlockedCompareExchange(&received, 0, 0), InterlockedCompareExchange(&ignored, 0, 0),
                 InterlockedCompareExchange(&dialledFromRoster, 0, 0), InterlockedCompareExchange(&loopbackSkipped, 0, 0), serial, heardSerial, adoptedSeed,
                 InterlockedCompareExchange(&seedAdoptions, 0, 0), InterlockedCompareExchange(&seedReverts, 0, 0), InterlockedCompareExchange(&seedFailures, 0, 0));
}
}  // namespace mesh
