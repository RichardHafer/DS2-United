// Dead Space 2 United by Richard Hafer.
// Scripted teleports of the local Isaac, from the engine's own marker (analysis/
// SIMGROUP_INTERCEPT_2026-10-17.md). Entity::SetTeleporting (0x007D0A80, thiscall, plain ret;
// it writes the frame number to entity+0x178) marks selected scripted moves:
// SimpleTeleport::Teleport (0x010B82B0), ScriptedSequenceSM::TeleportTargetToDest (0x00EB87F0,
// two calls) and NicoleBossSM::TeleportNicole (0x010B8900). Each machine watches its own Isaac
// and sends a serial with the destination in its VISI packet; a necro's ghost camera follows
// that instead of guessing from a position jump.
namespace teleport {
namespace traintrace {
bool enabled = false, watching = false;
DWORD since = 0, sampledAt = 0;
unsigned lines = 0;
uint32_t watchedPlayer = 0;
const uint32_t sequences[][4] = {
    {0x58CA778D,0x48648F18,0x5A54414B,0x39383230},
    {0x58CA7901,0xA51E3FB0,0x5A54414B,0x39383230},
    {0x30CA7915,0xC10A823E,0x54544950,0x32303153},
};
void Snapshot(const char* phase, uint32_t player, uint32_t sequence, int result = -1) {
    if (lines >= 600) return;
    ++lines;
    uint32_t guid[4]{}, current = 0, root = 0, master = 0xFFFFFFFFu, move = 0xFFFFFFFFu;
    float pos[3]{}, dest[3]{};
    unsigned char flags = 0;
    const bool positionOk = player && Read(player + 0xC0, pos, sizeof(pos));
    const bool destinationOk = sequence && Read(sequence + 0x1C0, dest, sizeof(dest));
    const bool guidOk = sequence && Read(sequence + 0x24, guid, sizeof(guid));
    if (sequence) Read(sequence + 0x200, &move, 4);
    if (player) {
        Read(player + 0x238, &current, 4);
        Read(player + 0x240, &flags, 1);
        if (Read(player + 0x24C, &root, 4) && root) Read(root + 0xC, &master, 4);
    }
    gate::Event("train_trace %s t=%lu player=%08X local=%08X bot=%d seq=%08X guid=%08X-%08X-%08X-%08X valid=%d/%d/%d pos=%.3f,%.3f,%.3f dest=%.3f,%.3f,%.3f move=%u current=%08X flags=%02X master=%u result=%d",
                phase, GetTickCount(), player, LocalSlotZeroEntity(), IsBotEntity(player) ? 1 : 0, sequence,
                guid[0], guid[1], guid[2], guid[3], positionOk, destinationOk, guidOk,
                pos[0], pos[1], pos[2], dest[0], dest[1], dest[2], move, current, flags, master, result);
}
bool Begin(uint32_t player, uint32_t sequence) {
    if (!enabled || !sequence) return false;
    uint32_t guid[4]{};
    if (!Read(sequence + 0x24, guid, sizeof(guid))) return false;
    bool match = false;
    for (const auto& expected : sequences) if (std::memcmp(expected, guid, sizeof(guid)) == 0) match = true;
    if (!match) return false;
    Snapshot("start-before", player, sequence);
    if (player == LocalSlotZeroEntity()) {
        watchedPlayer = player;
        since = GetTickCount();
        sampledAt = 0;
        watching = true;
    }
    return true;
}
void Tick(DWORD now) {
    if (!enabled || !watching) return;
    if (!inLevel || LocalSlotZeroEntity() != watchedPlayer || now - since >= 20000 || lines >= 600) {
        gate::Event("train_trace stop t=%lu age=%lu in_level=%d same_player=%d lines=%u",
                    now, now - since, inLevel ? 1 : 0, LocalSlotZeroEntity() == watchedPlayer ? 1 : 0, lines);
        watching = false;
        return;
    }
    if (sampledAt && now - sampledAt < 100) return;
    sampledAt = now;
    uint32_t sequence = 0;
    Read(watchedPlayer + 0x238, &sequence, 4);
    Snapshot("sample", watchedPlayer, sequence);
}
}
constexpr uint32_t kSetTeleportingRva = 0x3D0A80;
constexpr float kFollowM = 10.0f;    // a sequence alignment moves Isaac a few centimetres
constexpr DWORD kSettleMs = 500;     // the movement component may apply the move a frame later
safetyhook::InlineHook hook;
bool installed = false;
volatile LONG calls = 0, localCalls = 0, followed = 0, small = 0, partnerEvents = 0;
bool pending = false;
DWORD pendingSince = 0;
uint32_t pendingCaller = 0;
float lastPos[3]{};
bool lastKnown = false;
uint32_t serial = 0;
float to[3]{};
uint32_t partnerToken[coop_peers::kMaxPeers]{}, partnerSerial[coop_peers::kMaxPeers]{};
bool partnerNew[coop_peers::kMaxPeers]{};
DWORD partnerNewAt[coop_peers::kMaxPeers]{};
constexpr DWORD kPartnerEventMs = 5000;
float partnerTo[coop_peers::kMaxPeers][3]{};

const char* CallerName(uint32_t rva) {
    if (rva >= 0xCB82B0 && rva < 0xCB8900) return "a SimpleTeleport";
    if (rva >= 0xAB87F0 && rva < 0xAB8C00) return "a scripted sequence";
    if (rva >= 0xCB8900 && rva < 0xCB8D00) return "Nicole's teleport";
    return "an unknown caller";
}
void __fastcall OnSetTeleporting(void* entity, void*) {
    hook.thiscall<void>(entity);
    InterlockedIncrement(&calls);
    const uint32_t e = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entity));
    if (traintrace::watching && e == traintrace::watchedPlayer) {
        uint32_t sequence = 0;
        Read(e + 0x238, &sequence, 4);
        traintrace::Snapshot("native-teleport", e, sequence);
    }
    if (!e || e != LocalSlotZeroEntity()) return;
    InterlockedIncrement(&localCalls);
    if (!pending) pendingSince = Stamp(GetTickCount());
    pending = true;
    pendingCaller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())) - static_cast<uint32_t>(base);
}
// Game thread, every frame. Position as the state packet reads it (Player+0xC0).
void Tick(DWORD now) {
    traintrace::Tick(now);
    if (!installed) return;
    const uint32_t me = inLevel ? LocalSlotZeroEntity() : 0;
    float pos[3]{};
    const bool have = me && Read(me + 0xC0, pos, 12) && std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]);
    if (pending) {
        const float d = have && lastKnown ? std::sqrt((pos[0] - lastPos[0]) * (pos[0] - lastPos[0]) + (pos[1] - lastPos[1]) * (pos[1] - lastPos[1]) +
                                                      (pos[2] - lastPos[2]) * (pos[2] - lastPos[2]))
                                          : 0.0f;
        if (d >= kFollowM) {
            pending = false;
            ++serial;
            std::memcpy(to, pos, 12);
            scenekind::sendNow = true;   // the VISI packet goes out this frame
            InterlockedIncrement(&followed);
            gate::Event("test teleport INFO: Isaac was teleported %.0f m to (%.1f %.1f %.1f) by %s (serial %u)", d, pos[0], pos[1], pos[2],
                        CallerName(pendingCaller), serial);
            lastPos[0] = pos[0]; lastPos[1] = pos[1]; lastPos[2] = pos[2];
            return;
        }
        if (now - pendingSince < kSettleMs) return;   // keep the position from before the move
        pending = false;
        InterlockedIncrement(&small);
    }
    if (have) std::memcpy(lastPos, pos, 12);
    lastKnown = have;
}
// From a partner's VISI packet. The first packet of a partner sets his serial without an event.
void NotePartner(unsigned slot, uint32_t token, uint32_t partner, const float* where) {
    if (slot >= coop_peers::kMaxPeers) return;
    if (partnerToken[slot] != token) { partnerToken[slot] = token; partnerSerial[slot] = partner; partnerNew[slot] = false; return; }
    if (partner == partnerSerial[slot]) return;
    partnerSerial[slot] = partner;
    std::memcpy(partnerTo[slot], where, 12);
    partnerNew[slot] = true;
    partnerNewAt[slot] = Stamp(GetTickCount());
    InterlockedIncrement(&partnerEvents);
}
bool TakePartner(unsigned slot, float* out) {
    if (slot >= coop_peers::kMaxPeers || !partnerNew[slot]) return false;
    partnerNew[slot] = false;
    if (TicksSince(GetTickCount(), partnerNewAt[slot]) > kPartnerEventMs) return false;
    std::memcpy(out, partnerTo[slot], 12);
    return true;
}

void Install(FILE* log, const std::string& ini) {
    traintrace::enabled = GetPrivateProfileIntA("Coop", "TrainTrace", 0, ini.c_str()) != 0;
    std::fprintf(log, "train_trace enabled=%d sample_ms=100 window_ms=20000 line_limit=600 (read-only, three train sequences)\n", traintrace::enabled ? 1 : 0);
    if (GetPrivateProfileIntA("Coop", "TeleportFollow", 1, ini.c_str()) == 0) {
        std::fprintf(log, "teleport follow off ([Coop] TeleportFollow=0)\n");
        return;
    }
    // mov eax,[0204D3BCh] / mov [ecx+178h],eax / ret
    static const unsigned char kBytes[] = {0xA1, 0xBC, 0xD3, 0x04, 0x02, 0x89, 0x81, 0x78, 0x01, 0x00, 0x00, 0xC3};
    installed = hook_site::Install(log, "teleport Entity::SetTeleporting", base + kSetTeleportingRva, kBytes, sizeof(kBytes),
                                   reinterpret_cast<void*>(OnSetTeleporting), hook);
    std::fprintf(log, "teleport follow %s: the local Isaac's scripted teleports of %.0f m or more travel with the VISI packet\n",
                 installed ? "on" : "NOT installed", kFollowM);
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG sum = calls + localCalls * 3 + followed * 7 + small * 11 + partnerEvents * 13;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "teleport calls=%ld local=%ld followed=%ld small=%ld serial=%u partner_events=%ld\n", calls, localCalls, followed, small, serial,
                 partnerEvents);
}
}  // namespace teleport
