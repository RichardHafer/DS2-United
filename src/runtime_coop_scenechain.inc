// Dead Space 2 United by Hafer.
// Direct actor and camera steps verified in the initiating scene's retail MultiManager.
namespace scenechain {
// Mixed listeners in Ellie's street encounter were verified separately in the level scripts.
bool ActorOnlyCue(uint32_t hash) {
    switch (hash) {
        case 0x345FD344: // ss_ellie_idle_fire
        case 0x102BCDE5: // msg_se_04_02_ellie_fire
        case 0x4888E9B4: // msg_cct_holster_cutter
        case 0xDA5307E5: // slasher_se_04_02_left_ready
        case 0x35507026: // slasher_se_04_02_right_ready
        case 0x3D5724C2: // interrupt_ss_attack_gate_init_1
        case 0x3D5724C3: // interrupt_ss_attack_gate_init_2
        case 0x67FA8B27: // interrupt_ss_attack_gate_left
        case 0x73E6C89C: // interrupt_ss_attack_gate_right
            return true;
        default:
            return scene_catalog::IsActorCue(hash);
    }
}
constexpr uint32_t kMark = 0x31484353;   // SCH1
uint32_t root = 0, owner = 0, attempt = 0;
DWORD lastActive = 0;
bool churchHandoff = false, promptBranches = false;
uint32_t playedBranch = 0;
std::vector<uint32_t> played;
struct Early { Packet packet; DWORD at; };
std::vector<Early> early;
template<class Cue>
bool PendingRoot(const Packet& step, const Cue* queue, unsigned count, uint32_t rootMark) {
    for (unsigned i = 0; i < count; ++i) {
        const Packet& p = queue[i].packet;
        if (p.reserved[5] == rootMark && p.hash == step.reserved[7] &&
            p.token == step.token && p.reserved[6] == step.reserved[6]) return true;
    }
    return false;
}
void QueueEarly(const Packet& p, DWORD now) {
    for (const auto& e : early)
        if (e.packet.hash == p.hash && e.packet.token == p.token &&
            e.packet.reserved[6] == p.reserved[6] && e.packet.reserved[7] == p.reserved[7]) return;
    if (early.size() < 64) early.push_back({p, now});
}
void Clear() { root = owner = attempt = playedBranch = 0; churchHandoff = promptBranches = false; played.clear(); }
void Start(uint32_t hash, uint32_t actor, uint32_t occurrence, DWORD now, bool handoff, bool shareBranches = false) {
    Clear();
    churchHandoff = handoff;
    promptBranches = shareBranches;
    root = hash; owner = actor; attempt = occurrence; lastActive = now;

}
constexpr uint32_t kChurch = 0x39C2AFA6, kShaftExit = 0x994388F4, kKongSpawned = 0x13922E52;
bool ChurchTransition(uint32_t hash) {
    return root == kChurch && (hash == kShaftExit || hash == kKongSpawned);
}
bool Owns(uint32_t hash) {
    if (promptBranches && scene_catalog::IsPromptBranch(root, hash)) return true;
    return root && scene_catalog::IsCompanion(root, hash) && (!ChurchTransition(hash) || churchHandoff);
}
bool Matches(const Packet& p) {
    return root == p.reserved[7] && attempt == p.reserved[6] && owner == p.token && Owns(p.hash);
}
bool WaitsForExit(uint32_t hash) {
    return root == kChurch && hash == kKongSpawned &&
           std::find(played.begin(), played.end(), kShaftExit) == played.end();
}
bool HasPlayed(uint32_t hash) {
    if (playedBranch && scene_catalog::IsPromptBranch(root, hash)) return true;
    return std::find(played.begin(), played.end(), hash) != played.end();
}
bool First(uint32_t hash) {
    if (HasPlayed(hash)) return false;
    if (promptBranches && scene_catalog::IsPromptBranch(root, hash)) {
        playedBranch = hash;
    }
    played.push_back(hash);
    return true;
}
void Tick(DWORD now) {
    if (!root) return;
    if (!inLevel || !peerTable.Count()) { Clear(); return; }
    if (cutscene::SequenceRunningNow()) lastActive = now;
    else if (TicksSince(now, lastActive) > 3000) Clear();
}
}
