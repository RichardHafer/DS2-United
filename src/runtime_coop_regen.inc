// Dead Space 2 United by Hafer.
// The Ubermorph's (Hunter's) regrown limbs reach the clients ([Coop] ShareRegeneration).
//
// A Hunter regrows what was cut off in CadaverousRegenerationSM: the regrowth
// animations (PlayLimbAnims 0x00FDA8B0), then Undismember (PC 0x00FEBC70,
// thiscall(sm), plain ret; Xbox 0x84488FC0): the NPC is sm+0x6C, its
// DismemberableHealthComponent sm+0x70 remembers the cut state at +0x240,
// SetDismemberableState(0) (0x00F64290, ret 4) puts every limb back,
// RagdollComponent::RestoreCollisionForDismemberableState(old) (0x00BB2610,
// ret 4; component type byte [0x020916DA]) gives the parts their collision
// again, and CadaverousNPC::UpdateLastDismemberState (0x00FEBC10, thiscall(npc))
// copies the state. The cuts reach a client's ghost through the damage events,
// the regrowth has no event at all: 25.09., the clients' Hunter stayed cut.
// The host (who owns the NPC) now reports each Undismember, and a client runs
// the same three steps on its ghost of that NPC.
namespace regen {
bool enabled = false, installed = false;
constexpr uint32_t kUndismemberRva = 0xBEBC70, kSetStateRva = 0xB64290, kRestoreCollisionRva = 0x7B2610, kUpdateLastRva = 0xBEBC10;
constexpr uint32_t kComponentLookupRva = 0x3CF080, kRagdollTypeRva = 0x1C916DA, kHealthTypeRva = 0x1CA4252;
constexpr uint32_t kSmNpc = 0x6C, kHealthState = 0x240;
constexpr uint32_t kMagic = 0x4E474552;   // 'REGN'
constexpr uint32_t kVersion = 1;
struct Packet { uint32_t magic, version, token, sequence, guid[4], before, reserved[10]; };
static_assert(sizeof(Packet) == 76, "Unexpected regeneration wire layout");
bool Valid(const Packet& p) { return p.magic == kMagic && p.version == kVersion && p.token; }

safetyhook::InlineHook hook;
uint32_t sequence = 0;
struct Outgoing { Packet packet; DWORD at; unsigned sends; };
Outgoing outbox[4]{};
struct Seen { uint32_t token, sequence; };
Seen seen[32]{};
unsigned seenNext = 0;
volatile LONG reported = 0, received = 0, applied = 0, notFound = 0, notGhost = 0, noComponent = 0, faults = 0, lines = 0;

uint32_t Component(uint32_t entity, uint32_t typeRva) {
    unsigned char type = 0;
    if (!entity || !Read(base + typeRva, &type, 1)) return 0;
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t)>(base + kComponentLookupRva)(reinterpret_cast<void*>(entity), type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void __fastcall OnUndismember(void* sm, void*) {
    uint32_t npc = 0, health = 0, before = 0;
    const auto self = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sm));
    if (enabled && IsHost() && peerTable.Count() && Read(self + kSmNpc, &npc, 4) && npc) {
        health = Component(npc, kHealthTypeRva);
        if (health) Read(health + kHealthState, &before, 4);
    }
    hook.thiscall<void>(sm);
    if (!npc || guidfix::IsGhostEntity(npc)) return;
    Packet p{kMagic, kVersion, peerReadiness.token, ++sequence, {}, before, {}};
    if (!Read(npc + 0x24, p.guid, 16) || !(p.guid[0] | p.guid[1] | p.guid[2] | p.guid[3])) return;
    unsigned slot = 0;
    for (unsigned i = 1; i < std::size(outbox); ++i)
        if (outbox[i].sends >= 3 || static_cast<LONG>(outbox[i].at - outbox[slot].at) < 0) slot = i;
    outbox[slot] = Outgoing{p, GetTickCount(), 0};
    InterlockedIncrement(&reported);
    if (InterlockedIncrement(&lines) <= 40)
        gate::Event("coop_regen %08X-%08X regrew its limbs (cut state %08X): sent to the clients", p.guid[0], p.guid[1], before);
}

// Sent at once and again after 100 and 400 ms; a client folds the copies.
void Tick(DWORD now) {
    if (!installed || !peerTable.Count()) return;
    static const DWORD kAfter[] = {0, 100, 400};
    for (auto& o : outbox) {
        if (!o.packet.magic || o.sends >= std::size(kAfter) || now - o.at < kAfter[o.sends]) continue;   // empty slot
        SendToPeers(&o.packet, sizeof(o.packet));
        ++o.sends;
    }
}

void Apply(uint32_t npc) {
    const uint32_t health = Component(npc, kHealthTypeRva);
    if (!health) { InterlockedIncrement(&noComponent); return; }
    uint32_t before = 0;
    Read(health + kHealthState, &before, 4);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kSetStateRva)(reinterpret_cast<void*>(health), 0u);
    if (const uint32_t ragdoll = Component(npc, kRagdollTypeRva))
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kRestoreCollisionRva)(reinterpret_cast<void*>(ragdoll), before);
    reinterpret_cast<void(__thiscall*)(void*)>(base + kUpdateLastRva)(reinterpret_cast<void*>(npc));
    InterlockedIncrement(&applied);
    if (InterlockedIncrement(&lines) <= 40)
        gate::Event("coop_regen the host's Hunter %08X regrew: its ghost here has every limb back (was %08X)", npc, before);
}

// Game thread, from the receive loop.
void OnPacket(const Packet& p) {
    if (!installed || !enabled || IsHost() || !inLevel) return;
    for (const auto& s : seen) if (s.token == p.token && s.sequence == p.sequence) return;
    seen[seenNext] = Seen{p.token, p.sequence};
    seenNext = (seenNext + 1) % std::size(seen);
    InterlockedIncrement(&received);
    const uint32_t npc = mirror::ObjectByGuid(p.guid);
    if (!npc) { InterlockedIncrement(&notFound); return; }
    if (!guidfix::IsGhostEntity(npc)) { InterlockedIncrement(&notGhost); return; }   // a local copy regrows by its own AI
    __try {
        Apply(npc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "ShareRegeneration", 0, ini.c_str()) != 0;
    if (enabled) {
        // push esi / mov esi,ecx / mov ecx,[esi+6Ch] / test ecx,ecx / je / movzx
        static const unsigned char kSig[] = {0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x6C, 0x85, 0xC9, 0x74, 0x37, 0x0F, 0xB6};
        installed = hook_site::Install(log, "coop_regen CadaverousRegenerationSM::Undismember", base + kUndismemberRva, kSig, sizeof(kSig),
                                       reinterpret_cast<void*>(OnUndismember), hook);
    }
    std::fprintf(log, "coop_regen share=%d installed=%d (a Hunter's regrown limbs reach the clients' ghosts)\n", enabled ? 1 : 0, installed ? 1 : 0);
    return installed;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG n = reported + received;
    if (n == last) return;
    last = n;
    std::fprintf(log, "coop_regen reported=%ld received=%ld applied=%ld not_found=%ld not_ghost=%ld no_component=%ld faults=%ld\n", reported, received,
                 applied, notFound, notGhost, noComponent, faults);
}
}  // namespace regen
