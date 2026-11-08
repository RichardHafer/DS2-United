// Dead Space 2 United by Richard Hafer.
// Scripted teleports of the local Isaac, from the engine's own marker (analysis/
// SIMGROUP_INTERCEPT_2026-10-17.md). Entity::SetTeleporting (0x007D0A80, thiscall, plain ret;
// it writes the frame number to entity+0x178) runs after every scripted move:
// SimpleTeleport::Teleport (0x010B82B0), ScriptedSequenceSM::TeleportTargetToDest (0x00EB87F0,
// two calls) and NicoleBossSM::TeleportNicole (0x010B8900). Each machine watches its own Isaac
// and sends a serial with the destination in its VISI packet; a necro's ghost camera follows
// that instead of guessing from a position jump.
namespace teleport {
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
    if (!e || e != LocalSlotZeroEntity()) return;
    InterlockedIncrement(&localCalls);
    if (!pending) pendingSince = Stamp(GetTickCount());
    pending = true;
    pendingCaller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())) - static_cast<uint32_t>(base);
}
// Game thread, every frame. Position as the state packet reads it (Player+0xC0).
void Tick(DWORD now) {
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
