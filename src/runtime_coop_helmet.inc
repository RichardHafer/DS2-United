// Dead Space 2 United by Hafer.
// Helmet open or closed, mirrored onto the ghost bodies.
//
// A level's PlayerHelmetCommand node (vtable 0x01B2686C) executes on a given
// Player through its slot 8 (0x00864300): it calls
//     Player::SetFlag(0x17, open)          PC 0x0085F7E0, thiscall(int bit, bool), ret 8
// which sets or clears bit 0x17 in the flag words at Player+0x3DC and marks
// the NetReflectable dirty (mask 0x200). PlayerHelmetSM reads that bit and
// runs the fold animation. So the local bit rides in PlayerState, and each
// ghost gets the same call with its owner's value. No message, no node, no
// audio: the ghost just folds its helmet the way its owner did.
namespace helmet {
bool enabled = false, installed = false;
constexpr uint32_t kSetFlagRva = 0x45F7E0;
constexpr uint32_t kFlagWords = 0x3DC;
constexpr uint32_t kHelmetOpenBit = 0x17;

volatile LONG applied = 0, faults = 0, mismatches = 0;

bool ReadBit(uint32_t player, bool& open) {
    uint32_t word = 0;
    if (!Read(player + kFlagWords + (kHelmetOpenBit >> 5) * 4, &word, 4)) return false;
    open = (word & (1u << (kHelmetOpenBit & 31))) != 0;
    return true;
}
bool LocalOpen(uint32_t player) {
    bool open = false;
    return installed && player && ReadBit(player, open) && open;
}

int Filter(unsigned) { InterlockedIncrement(&faults); return EXCEPTION_EXECUTE_HANDLER; }
void SetOpen(uint32_t ghost, bool open) {
    using SetFlag = void(__thiscall*)(void*, int, bool);
    __try {
        reinterpret_cast<SetFlag>(base + kSetFlagRva)(reinterpret_cast<void*>(ghost), static_cast<int>(kHelmetOpenBit), open);
        InterlockedIncrement(&applied);
    } __except (Filter(GetExceptionCode())) {
    }
}

// From Poll, on the game thread: every partner's ghost follows its owner's bit.
void Mirror() {
    if (!installed || !enabled) return;
    for (unsigned slot = 0; slot < coop_peers::kMaxPeers; ++slot) {
        const auto& peer = peerTable.peers[slot];
        auto& runtime = peerRuntime[slot];
        if (!peer.used || peer.machine == 0xFFFFFFFFu || !runtime.lastState) continue;
        const bool wantOpen = (runtime.flags & kStateFlagHelmetOpen) != 0;
        const uint32_t ghost = GhostForMachine(peer.machine);
        if (!ghost) continue;
        bool isOpen = false;
        if (!ReadBit(ghost, isOpen)) continue;
        if (isOpen == wantOpen) { runtime.appliedFlags = runtime.flags; continue; }
        InterlockedIncrement(&mismatches);
        SetOpen(ghost, wantOpen);
        runtime.appliedFlags = runtime.flags;
    }
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "HelmetSync", 1, ini.c_str()) != 0;
    // mov eax,ecx / push esi / mov esi,[esp+8] / mov ecx,esi / and ecx,0x1F / mov edx,1
    static const unsigned char kSig[] = {0x8B,0xC1,0x56,0x8B,0x74,0x24,0x08,0x8B,0xCE,0x83,0xE1,0x1F,0xBA,0x01,0x00,0x00,0x00};
    unsigned char actual[sizeof(kSig)]{};
    if (!Read(base + kSetFlagRva, actual, sizeof(kSig)) || std::memcmp(actual, kSig, sizeof(kSig)) != 0) {
        std::fprintf(log, "coop_helmet Player::SetFlag signature mismatch at %08X; helmet sync off\n",
                     static_cast<unsigned>(base + kSetFlagRva));
        return false;
    }
    installed = true;
    std::fprintf(log, "coop_helmet sync=%d set_flag=%08X bit=0x%X words=+0x%X\n", enabled,
                 static_cast<unsigned>(base + kSetFlagRva), kHelmetOpenBit, kFlagWords);
    return true;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&applied, 0, 0) + InterlockedCompareExchange(&faults, 0, 0);
    if (now == last) return;
    last = now;
    std::fprintf(log, "coop_helmet applied=%ld mismatches=%ld faults=%ld\n", InterlockedCompareExchange(&applied, 0, 0),
                 InterlockedCompareExchange(&mismatches, 0, 0), InterlockedCompareExchange(&faults, 0, 0));
}
}  // namespace helmet
