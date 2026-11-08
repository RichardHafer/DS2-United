// Dead Space 2 United by Hafer.
// Machines one player at a time ([Coop] PanelLock): hacking panels, power node doors
// and the eye poke machine.
//
// Every machine runs its own copy of such a machine, and the mirror carries what one
// copy does to the others (the classes are mirrored, their results are shared
// outcomes). 13.10., the train's panel: the host went to it while the client was
// already in its minigame, and the client's game went down. So whoever starts one
// claims it: its GUID goes to every partner on the start and then every 400 ms while
// he stays at it. A partner's own start at that machine is refused while the claim is
// fresh (1.5 s on LAN, with extra time for the measured round trip and jitter).
//
// The start is PlayerUseObjectSM::CheckUseObjectStart (bot::OnUseStart): the usable
// offered at Player+0x3F4 is an IUsable inside the machine, and its first slot returns
// the machine (lea eax,[ecx-offset]; ret), which Install checks per class:
//   HackingStationMiniGame  0x01B45140, IUsable +0x280 (vtable 0x01B44F68, ctor 0x00BB72D0)
//   PowerNodeSwitch         0x01B5DC78, IUsable +0x230 (vtable 0x01B5DB00)
//   EyePokeMachine          0x01B77468, IUsable +0x230 (vtable 0x01B772F0, ctor 0x010B1700)
// Each class keeps its use state differently, so the claim lasts while our Isaac stays
// within kLeaveM of where he started (they all hold him in place), at most kMaxHoldMs.
namespace story_objects {
void ReplayUnlock(uint32_t hash, uint32_t sender, DWORD now) {
    if (!sender || !gate::Registered(hash) || mirror::Faulted(hash)) return;
    mirror::Packet p{};
    p.hash = hash;
    p.token = sender;
    mirror::Replay(p, now);
}
void FinishUse(uint32_t obj) {
    reinterpret_cast<void(__thiscall*)(void*)>(base + 0xABEB60)(reinterpret_cast<void*>(obj));
}
void DisablePanel(uint32_t obj) {
    reinterpret_cast<void(__thiscall*)(void*, int)>(base + 0xABF590)(reinterpret_cast<void*>(obj), 0);
}
}
namespace uselock {
bool enabled = false;
constexpr uint32_t kMagic = 0x4C455355;   // 'USEL'
constexpr uint8_t kVersion = 1;
constexpr uint32_t kOfferedUsable = 0x3F4;
constexpr DWORD kBeatMs = 400, kFreshMs = 1500, kLeaveMs = 1000, kMaxHoldMs = 180000;
constexpr float kLeaveM = 4.0f;
struct Machine { const char* name; uint32_t ownerRva, usableRva, offset; bool ok; };
Machine machines[] = {
    {"hacking panel", 0x1745140, 0x1744F68, 0x280, false},
    {"power node door", 0x175DC78, 0x175DB00, 0x230, false},
    {"eye poke machine", 0x1777468, 0x17772F0, 0x230, false},
};
#pragma pack(push, 1)
struct Packet { uint32_t magic; uint8_t version, holding; uint16_t pad; uint32_t token, seq; uint32_t guid[4]; };
#pragma pack(pop)
static_assert(sizeof(Packet) == 32, "machine lock packet size");
using Claim = ::coop_panel::Claim;
Claim claims[coop_peers::kMaxPeers]{};
const Machine* heldKind = nullptr;
uint32_t heldMachine = 0, heldPlayer = 0, heldGuid[4]{}, seqOut = 0, pending = 0, pendingPlayer = 0;
const Machine* pendingKind = nullptr;
float heldAt[3]{};
DWORD heldSince = 0, heldBeatAt = 0, awaySince = 0;
volatile LONG refused = 0, claimsSent = 0, claimsReceived = 0, releasesSent = 0, releasesReceived = 0, races = 0, expired = 0, lines = 0;

bool Line() { return InterlockedIncrement(&lines) <= 80; }
// The machine a usable belongs to, 0 for any other usable.
uint32_t MachineOf(uint32_t usable, const Machine** kind) {
    uint32_t usableVtable = 0, ownerVtable = 0;
    if (!usable || !Read(usable, &usableVtable, 4)) return 0;
    for (const auto& m : machines) {
        if (!m.ok || usableVtable != base + m.usableRva || usable < m.offset) continue;
        const uint32_t owner = usable - m.offset;
        if (!Read(owner, &ownerVtable, 4) || ownerVtable != base + m.ownerRva) return 0;
        *kind = &m;
        return owner;
    }
    return 0;
}
void Send(uint8_t holding) {
    if (!network || !peerTable.Count()) return;
    Packet p{kMagic, kVersion, holding, 0, peerReadiness.token, ++seqOut, {}};
    std::memcpy(p.guid, heldGuid, sizeof(p.guid));
    if (SendToPeers(&p, sizeof(p)) > 0) InterlockedIncrement(holding ? &claimsSent : &releasesSent);
}
const Claim* FreshClaim(const uint32_t* guid, DWORD now) {
    const DWORD fresh = netclock::Window(kFreshMs);
    for (const auto& c : claims)
        if (c.at && peerTable.peers[c.slot].token == c.token && now - c.at < fresh && std::memcmp(c.guid, guid, 16) == 0) return &c;
    return nullptr;
}
void Release(const char* why) {
    if (!heldMachine) return;
    Send(0);
    gate::Event("machine lock: %s %08X-%08X released (%s) after %lu ms", heldKind ? heldKind->name : "?", heldGuid[0], heldGuid[1], why,
                static_cast<unsigned long>(GetTickCount() - heldSince));
    heldMachine = heldPlayer = 0;
    heldKind = nullptr;
    std::memset(heldGuid, 0, sizeof(heldGuid));
}
void ResetClaims() {
    for (auto& c : claims) c = {};
    heldMachine = heldPlayer = pending = pendingPlayer = 0;
    heldKind = pendingKind = nullptr;
}
// From the use start hook, game thread, before the game's own start: true refuses our
// Isaac's start at a machine a partner holds. Otherwise the machine waits for Started.
bool RefuseLocal(uint32_t player) {
    pending = 0;
    if (!enabled || !network || !peerTable.Count() || player != LocalSlotZeroEntity()) return false;
    uint32_t usable = 0;
    const Machine* kind = nullptr;
    if (!Read(player + kOfferedUsable, &usable, 4)) return false;
    const uint32_t machine = MachineOf(usable, &kind);
    if (!machine) return false;
    uint32_t guid[4]{};
    if (!Read(machine + 0x24, guid, sizeof(guid)) || !(guid[0] | guid[1] | guid[2] | guid[3])) return false;
    if (kind->ownerRva == mirror::kPowerNodeOutcome.vtableRva && story_objects::Paid(guid)) return true;
    if (kind->ownerRva == mirror::kPowerNodeOutcome.vtableRva && mirror::actionOutcomes) {
        uint32_t attributes = 0, unlock = 0;
        if (Read(machine + 0x0C, &attributes, 4) && attributes &&
            Read(attributes + mirror::kPowerNodeOutcome.offset, &unlock, 4) && unlock && mirror::OutcomePlayedHere(unlock)) {
            if (Line()) gate::Event("machine lock: power node door %08X-%08X already paid by partner, unlock=%08X", guid[0], guid[1], unlock);
            return true;
        }
    }
    const DWORD now = GetTickCount();
    if (const Claim* c = FreshClaim(guid, now)) {
        InterlockedIncrement(&refused);
        if (Line()) gate::Event("machine lock: our use of the %s %08X-%08X refused, partner slot %u is at it (claim %lu ms old)", kind->name, guid[0], guid[1],
                                c->slot, static_cast<unsigned long>(now - c->at));
        notice::Show("A PARTNER IS USING THIS");
        return true;
    }
    pending = machine;
    pendingPlayer = player;
    pendingKind = kind;
    return false;
}
// After the game's own start: a started use claims the machine.
void Started(uint32_t player, bool started) {
    const uint32_t machine = pending;
    pending = 0;
    if (!machine || !started || player != pendingPlayer) return;
    if (heldMachine && heldMachine != machine) Release("another machine");
    heldMachine = machine;
    heldPlayer = player;
    heldKind = pendingKind;
    Read(machine + 0x24, heldGuid, sizeof(heldGuid));
    Read(player + placement::kPlayerPosition, heldAt, sizeof(heldAt));
    heldSince = heldBeatAt = GetTickCount();
    awaySince = 0;
    Send(1);
    if (Line()) gate::Event("machine lock: claimed the %s %08X-%08X (%08X) for our Isaac %08X", heldKind->name, heldGuid[0], heldGuid[1], machine, player);
}
// Game thread, from Poll: the heartbeat while our Isaac is at the machine, the release after.
void Tick() {
    if (!enabled) return;
    const DWORD now = GetTickCount();
    const DWORD fresh = netclock::Window(kFreshMs);
    for (auto& c : claims)
        if (c.at && now - c.at >= fresh) { c.at = 0; InterlockedIncrement(&expired); }
    if (!heldMachine) return;
    if (!inLevel || LocalSlotZeroEntity() != heldPlayer) { Release("level or Isaac changed"); return; }
    uint32_t vtable = 0;
    if (!Read(heldMachine, &vtable, 4) || !heldKind || vtable != base + heldKind->ownerRva) { Release("machine gone"); return; }
    uint32_t guid[4]{};
    if (!Read(heldMachine + 0x24, guid, sizeof(guid)) || std::memcmp(heldGuid, guid, sizeof(guid))) { Release("machine replaced"); return; }
    if (now - heldSince >= kMaxHoldMs) { Release("held for the longest allowed"); return; }
    float at[3]{};
    if (Read(heldPlayer + placement::kPlayerPosition, at, sizeof(at))) {
        const float dx = at[0] - heldAt[0], dy = at[1] - heldAt[1], dz = at[2] - heldAt[2];
        if (dx * dx + dy * dy + dz * dz > kLeaveM * kLeaveM) {
            if (!awaySince) awaySince = now;
            if (now - awaySince >= kLeaveMs) { Release("our Isaac walked away"); return; }
        } else {
            awaySince = 0;
        }
    }
    if (const Claim* c = FreshClaim(heldGuid, now)) {
        static DWORD raceAt = 0;
        if (now - raceAt > 5000) {
            raceAt = now;
            InterlockedIncrement(&races);
            gate::Event("machine lock: RACE, partner slot %u claims the %s %08X-%08X too while we are at it", c->slot, heldKind->name, heldGuid[0], heldGuid[1]);
        }
    }
    if (now - heldBeatAt >= kBeatMs) { heldBeatAt = now; Send(1); }
}
// A partner's claim or release, from the side channel's receive loop.
bool OnBytes(const void* data, int size, unsigned slot, DWORD now, bool fromHost) {
    if (size != static_cast<int>(sizeof(Packet))) return false;
    Packet p{};
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != kMagic || p.version != kVersion || p.holding > 1 || !p.token || !(p.guid[0] | p.guid[1] | p.guid[2] | p.guid[3])) return false;
    const auto* sender = peerTable.FindByToken(p.token);
    if (!sender || (!fromHost && PeerSlot(sender) != slot)) return false;
    if (!enabled) return true;
    const unsigned from = PeerSlot(sender);
    Claim& entry = claims[from];
    const bool changed = !entry.at || std::memcmp(entry.guid, p.guid, sizeof(p.guid));
    if (!entry.Update(p.token, p.seq, from, p.guid, p.holding != 0, Stamp(now))) return true;
    if (!p.holding) {
        InterlockedIncrement(&releasesReceived);
        return true;
    }
    if (changed && Line())
        gate::Event("machine lock: partner slot %u is at machine %08X-%08X", from, p.guid[0], p.guid[1]);
    InterlockedIncrement(&claimsReceived);
    return true;
}
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "PanelLock", 1, ini.c_str()) != 0;
    char kinds[160] = "";
    size_t used = 0;
    unsigned good = 0;
    for (auto& m : machines) {
        // lea eax,[ecx-offset] / ret in the IUsable's first slot
        uint32_t slot = 0;
        unsigned char code[7]{};
        int32_t disp = 0;
        m.ok = Read(base + m.usableRva, &slot, 4) && Read(slot, code, sizeof(code)) && code[0] == 0x8D && code[1] == 0x81 && code[6] == 0xC3 &&
               (std::memcpy(&disp, code + 2, 4), disp == -static_cast<int32_t>(m.offset));
        if (m.ok) ++good;
        if (used < sizeof(kinds)) used += std::snprintf(kinds + used, sizeof(kinds) - used, "%s%s=%s", used ? ", " : "", m.name, m.ok ? "ok" : "MISMATCH");
    }
    if (enabled && !good) enabled = false;
    if ((enabled || ghosttrigger::liftWait) && !bot::useGuard) bot::InstallUseGuard(log);
    if (enabled && !bot::useGuard) enabled = false;
    std::fprintf(log, "machine lock %s (one player at a time: %s; use start hook %s)\n", enabled ? "on" : "OFF", kinds, bot::useGuard ? "in" : "missing");
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    const LONG values[] = {claimsSent, releasesSent, claimsReceived, releasesReceived, refused, races, expired};
    static LONG last[std::size(values)] = {-1};
    if (std::memcmp(last, values, sizeof(values)) == 0) return;
    std::memcpy(last, values, sizeof(values));
    std::fprintf(log, "machine lock claims_sent=%ld releases_sent=%ld claims_received=%ld releases_received=%ld refused=%ld races=%ld expired=%ld holding=%d\n",
                 values[0], values[1], values[2], values[3], values[4], values[5], values[6], heldMachine ? 1 : 0);
}
}  // namespace uselock
