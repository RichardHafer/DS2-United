// Dead Space 2 United by Richard Hafer.
// Catch up with the partner on a key ([Coop] CatchUpKey, 0 = off).
//
// 25.09., ch11 marker test bed: the host shot the glowing fuse next to the
// barrier (DamageableProp 4FCB381D-2901560A), the Mover 4FCB6693-9B5EC04B came
// down on both machines alike and the client was left on the far side. The
// level is built for one Isaac, so nothing on the client's side opens it again.
// The key puts the local Isaac where the partner stood a moment ago: a spot a player
// really occupied is floor, not wall. Entity and character controller are
// moved together, the way the necro's vent fall rescue does it.
namespace catchup {
int key = 0;
bool keyDown = false;
constexpr DWORD kSampleMs = 250, kTrailMs = 4000, kCooldownMs = 1000;
constexpr float kMinGap = 3.0f, kBehind = 1.0f;
struct Sample { float pos[3]; DWORD at; };
// One trail per partner. With three or four players each press goes to the
// next partner who is not already close by, so a player cut off together
// with another reaches the group with a second press.
Sample trails[coop_peers::kMaxPeers][16]{};
unsigned trailNext[coop_peers::kMaxPeers]{};
DWORD lastSample = 0, lastJump = 0;
unsigned lastTarget = coop_peers::kMaxPeers - 1;
volatile LONG jumps = 0, refused = 0, faults = 0;

float Distance(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void Refuse(const char* why) {
    InterlockedIncrement(&refused);
    gate::Event("coop_catchup refused: %s", why);
}

// A partner known to be on another map has coordinates of another world
// (load sync's map hash; unknown counts as the same map, as before).
volatile LONG otherMap = 0;
bool Fresh(unsigned slot, DWORD now) {
    if (!peerTable.peers[slot].used || !peerRuntime[slot].lastState || now - peerRuntime[slot].lastState > 2000) return false;
    if (loadsync::PartnerOnOurMap(peerTable.peers[slot].token, now) == 0) { InterlockedIncrement(&otherMap); return false; }
    return true;
}
void Reset() {
    for (auto& trail : trails) for (auto& s : trail) s = Sample{};
    for (auto& next : trailNext) next = 0;
    lastSample = lastJump = 0;
    lastTarget = coop_peers::kMaxPeers - 1;
}

// Moves the local Isaac to a spot, entity and character controller together.
bool MoveIsaac(const float* target, float& gap, uint32_t& player, uint32_t& cmc, bool exact = false) {
    player = LocalSlotZeroEntity();
    cmc = 0;
    alignas(16) float matrix[16];
    if (!player || !Read(player + necro::kEntityMatrix, matrix, sizeof(matrix))) return false;
    const float dx = matrix[12] - target[0], dy = matrix[13] - target[1], dz = matrix[14] - target[2];
    gap = std::sqrt(dx * dx + dy * dy + dz * dz);
    matrix[12] = target[0]; matrix[13] = target[1] + (exact ? 0.0f : 0.2f); matrix[14] = target[2];
    __try {
        uint32_t vtable = 0, setMatrix = 0;
        if (!Read(player, &vtable, 4) || !Read(vtable + necro::kEntitySetMatrixSlot, &setMatrix, 4) || !setMatrix) return false;
        using MatrixCall = void (__thiscall*)(uint32_t, const float*);
        if (exact && !(cmc = necro::FindCmc(player))) return false;
        reinterpret_cast<MatrixCall>(setMatrix)(player, matrix);
        if (!exact) cmc = necro::FindCmc(player);
        if (cmc) reinterpret_cast<MatrixCall>(base + necro::kCmcSetMatrixRva)(cmc, matrix);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}
const char* StageChurchIsaac(uint32_t player, const float* at) {
    static constexpr uint32_t sequenceGuid[] = {0x39CB17CF, 0xFDEDBB6C, 0x4B4C4157, 0x32345245};
    if (player != LocalCampaignPlayer() || IsGhostPlayer(player)) return "local Isaac unavailable";
    if (!mirror::ObjectByGuid(sequenceGuid)) return "church pull sequence unavailable";
    float gap = 0;
    uint32_t cmc = 0;
    if (!MoveIsaac(at, gap, player, cmc, true)) return "church placement failed";
    gate::Event("church opener staged Isaac=%08X cmc=%08X distance=%.2f m", player, cmc, gap);
    return nullptr;
}
// A move to a partner whose rooms are not loaded here drops our Isaac into the void (they follow the
// body only once it is there). His set is asked for first and the move waits for it.
struct Pending { bool used = false, pull = false; unsigned slot = 0; float at[3]{}; char why[128]{}; DWORD since = 0, askedAt = 0; };
Pending pending;
constexpr DWORD kRoomsMs = 15000;
volatile LONG roomWaits = 0, roomsCame = 0, roomsMissing = 0;
unsigned SlotNear(const float* at, DWORD now) {
    unsigned best = coop_peers::kMaxPeers;
    float bestD = 0.0f;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        if (!Fresh(s, now)) continue;
        const float d = Distance(peerRuntime[s].pos, at);
        if (best == coop_peers::kMaxPeers || d < bestD) { best = s; bestD = d; }
    }
    return best;
}
// True: the rooms are in (or nothing is known about them) and the move may go ahead now.
bool RoomsFirst(unsigned slot, bool pull, const float* at, const char* why, DWORD now) {
    if (slot >= coop_peers::kMaxPeers || streams::PartnerRooms(slot, now, true) != 0) return true;
    pending = Pending{};
    pending.used = true;
    pending.pull = pull;
    pending.slot = slot;
    pending.since = pending.askedAt = now ? now : 1;
    if (at) std::memcpy(pending.at, at, 12);
    std::snprintf(pending.why, sizeof(pending.why), "%s", why);
    InterlockedIncrement(&roomWaits);
    gate::Event("coop_catchup move to partner %u waits: his rooms are not loaded here (%s)", slot, why);
    notice::Show("LOADING PARTNER'S ROOMS");
    return false;
}
// From the gate: the group waits for this player at a progression point.
volatile LONG pulled = 0;
bool roomsIn = false;   // the wait is over, the move comes now
bool PullTo(const float* at, const char* why) {
    if (!inLevel) return false;
    if (spectate::Active()) { InterlockedIncrement(&spectate::catchupRefused); return false; }   // the corpse stays where he died
    if (necro::LocalRole() != 0) { Refuse("a necro player is not pulled to the group"); return false; }   // slot 0 holds the necro or nothing
    if (!roomsIn && !RoomsFirst(SlotNear(at, GetTickCount()), true, at, why, GetTickCount())) return true;
    float gap = 0.0f;
    uint32_t player = 0, cmc = 0;
    if (!MoveIsaac(at, gap, player, cmc)) {
        gate::Event("coop_catchup pull failed (%s): our Isaac could not be moved", why);
        return false;
    }
    InterlockedIncrement(&pulled);
    lastJump = GetTickCount();
    gate::Event("coop_catchup Isaac %08X pulled %.1f m to (%.1f %.1f %.1f): %s (cmc %08X)", player, gap, at[0], at[1], at[2], why, cmc);
    notice::Show("PULLED TO PARTNER");
    return true;
}

void Jump(DWORD now) {
    if (cutscene::Following() || cutscene::SequenceRunningNow()) { Refuse("a scene is running"); return; }
    if (necro::LocalRole() != 0) { Refuse("a necro player (slot 0 holds the necro, not Isaac)"); return; }
    const uint32_t player = LocalSlotZeroEntity();
    alignas(16) float matrix[16];
    if (!player || !Read(player + necro::kEntityMatrix, matrix, sizeof(matrix))) { Refuse("no local Isaac"); return; }
    // The next partner after the last target, skipping whoever is already close.
    unsigned slot = coop_peers::kMaxPeers, fresh = 0;
    for (unsigned step = 1; step <= coop_peers::kMaxPeers && slot == coop_peers::kMaxPeers; ++step) {
        const unsigned s = (lastTarget + step) % coop_peers::kMaxPeers;
        if (!Fresh(s, now)) continue;
        ++fresh;
        if (Distance(matrix + 12, peerRuntime[s].pos) >= kMinGap) slot = s;
    }
    if (slot == coop_peers::kMaxPeers) { Refuse(fresh ? "every partner is already next to us" : "no partner with a current position"); return; }
    if (!roomsIn && !RoomsFirst(slot, false, nullptr, "the catch-up key", now)) return;
    if (roomsIn) slot = pending.slot;   // the one we waited for
    const auto& runtime = peerRuntime[slot];
    const float gap = Distance(matrix + 12, runtime.pos);
    // The newest trail point a little behind that partner, else his own spot.
    float target[3] = {runtime.pos[0], runtime.pos[1], runtime.pos[2]};
    const char* source = "the partner's own spot";
    DWORD best = 0;
    for (const auto& s : trails[slot]) {
        if (!s.at || now - s.at > kTrailMs || s.at <= best) continue;
        if (Distance(s.pos, runtime.pos) < kBehind || std::fabs(s.pos[1] - runtime.pos[1]) > 1.0f) continue;
        best = s.at;
        std::memcpy(target, s.pos, sizeof(target));
        source = "where the partner stood";
    }
    matrix[12] = target[0]; matrix[13] = target[1] + 0.2f; matrix[14] = target[2];
    __try {
        uint32_t vtable = 0, setMatrix = 0;
        if (!Read(player, &vtable, 4) || !Read(vtable + necro::kEntitySetMatrixSlot, &setMatrix, 4) || !setMatrix) { Refuse("no SetMatrix"); return; }
        using MatrixCall = void (__thiscall*)(uint32_t, const float*);
        reinterpret_cast<MatrixCall>(setMatrix)(player, matrix);
        const uint32_t cmc = necro::FindCmc(player);
        if (cmc) reinterpret_cast<MatrixCall>(base + necro::kCmcSetMatrixRva)(cmc, matrix);
        lastJump = now;
        lastTarget = slot;
        InterlockedIncrement(&jumps);
        gate::Event("coop_catchup Isaac %08X moved %.1f m to partner %u of %u: (%.1f %.1f %.1f), %s (partner at %.1f %.1f %.1f, cmc %08X)", player, gap,
                    slot, fresh, matrix[12], matrix[13], matrix[14], source, runtime.pos[0], runtime.pos[1], runtime.pos[2], cmc);
        char text[48];
        std::snprintf(text, sizeof(text), "CAUGHT UP WITH PARTNER %u", slot + 1);
        notice::Show(text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        gate::Event("coop_catchup fault while moving Isaac %08X", player);
    }
}

void PendingTick(DWORD now) {
    if (!pending.used) return;
    if (!inLevel || !Fresh(pending.slot, now) || spectate::Active() || necro::LocalRole() != 0) { pending.used = false; return; }
    if (now - pending.askedAt < 250) return;
    pending.askedAt = now;
    if (streams::PartnerRooms(pending.slot, now, now - pending.since < 3000) != 0) {
        InterlockedIncrement(&roomsCame);
        gate::Event("coop_catchup partner %u's rooms are loaded after %lu ms", pending.slot, static_cast<unsigned long>(now - pending.since));
        pending.used = false;
        roomsIn = true;
        if (pending.pull) PullTo(pending.at, pending.why);
        else Jump(now);
        roomsIn = false;
        return;
    }
    if (now - pending.since < kRoomsMs) return;
    InterlockedIncrement(&roomsMissing);
    gate::Event("coop_catchup partner %u's rooms did not load in %lu ms: not moved (%s)", pending.slot, static_cast<unsigned long>(kRoomsMs), pending.why);
    notice::Show("PARTNER'S ROOMS DID NOT LOAD");
    pending.used = false;
}
// Game thread, every frame.
bool trailsHeld = false;
void Poll() {
    PendingTick(GetTickCount());
    // The trails are positions in the old level: dropped when it goes.
    if (!inLevel) { if (trailsHeld) { Reset(); trailsHeld = false; } return; }
    if (!key || !peerTable.Count()) return;
    trailsHeld = true;
    const DWORD now = GetTickCount();
    if (now - lastSample >= kSampleMs) {
        lastSample = now;
        for (unsigned slot = 0; slot < coop_peers::kMaxPeers; ++slot) {
            if (!peerTable.peers[slot].used || !peerRuntime[slot].lastState || now - peerRuntime[slot].lastState > 1000) continue;
            if (loadsync::PartnerOnOurMap(peerTable.peers[slot].token, now) == 0) continue;
            Sample& s = trails[slot][trailNext[slot]++ % std::size(trails[slot])];
            std::memcpy(s.pos, peerRuntime[slot].pos, sizeof(s.pos));
            s.at = now;
        }
    }
    // Two instances on one machine both see the key; only the focused one acts.
    const bool down = necro::GameWindowFocused() && (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool pressed = down && !keyDown;
    keyDown = down;
    if (!pressed) return;
    if (spectate::Active()) { InterlockedIncrement(&spectate::catchupRefused); Refuse("dead, watching a partner"); return; }
    if (lastJump && now - lastJump < kCooldownMs) { Refuse("pressed again too soon"); return; }
    Jump(now);
}

void Install(FILE* log, const std::string& ini) {
    key = GetPrivateProfileIntA("Coop", "CatchUpKey", 0, ini.c_str());
    if (key < 0 || key > 255) key = 0;
    std::fprintf(log, "coop_catchup key=%d (0 = off; puts our Isaac next to the partner when a one-player barrier cut us off)\n", key);
}

void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG n = jumps + pulled + refused + faults + otherMap + roomWaits + roomsCame + roomsMissing;
    if (n == last) return;
    last = n;
    std::fprintf(log, "coop_catchup jumps=%ld pulled=%ld refused=%ld faults=%ld other_map_skips=%ld room_waits=%ld rooms_came=%ld rooms_missing=%ld\n", jumps, pulled, refused, faults, otherMap,
                 roomWaits, roomsCame, roomsMissing);
}
}  // namespace catchup
