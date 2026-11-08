// Dead Space 2 United by Hafer.
// Retail PlayerManager slot zero is the local campaign BasePlayer.
constexpr uint32_t kPlayerManagerRva = 0x1C1C0D4;
uint32_t LocalCampaignPlayer() {
    uint32_t manager = 0, slots = 0, capacity = 0, bp = 0, vt = 0, getter = 0;
    if (!Read(base + kPlayerManagerRva, &manager, 4) || !manager ||
        !Read(manager + 4, &slots, 4) || !slots ||
        !Read(manager + 8, &capacity, 4) || !capacity || capacity > 8 ||
        !Read(slots, &bp, 4) || bp < 0x103C0 ||
        !Read(bp, &vt, 4) || !Read(vt + 12, &getter, 4) || getter != base + 0x4A7960) return 0;
    const uint32_t player = bp - 0x3C0;
    uint32_t proxy = 0, flags = 0;
    if (!Read(player, &vt, 4) || vt != base + placement::kPlayerPrimaryVtableRva ||
        !Read(player + 0x68, &proxy, 4)) return 0;
    if (proxy && (!Read(proxy + 0x30, &flags, 4) || (flags & 2))) return 0;
    return player;
}

// The same slot, but the body may be a NecroPlayer (runtime_coop_necro.inc puts
// one into slot 0 while the human plays as a necromorph). Only for consumers
// that touch entity-level fields (+0x24 guid, +0x40 subkey, +0x5C reflectable,
// +0x90 matrix, +0xB0/+0xC0 forward/position): the Player-only layout
// (+0x420 id, +0xC08 inventory, +0x3DC flag words, which on a NecroPlayer is
// its manager id) stays behind LocalCampaignPlayer().
constexpr uint32_t kNecroPrimaryVtableRva = 0x17783A0;
uint32_t LocalSlotZeroEntity() {
    uint32_t manager = 0, slots = 0, capacity = 0, bp = 0, vt = 0;
    if (!Read(base + kPlayerManagerRva, &manager, 4) || !manager ||
        !Read(manager + 4, &slots, 4) || !slots ||
        !Read(manager + 8, &capacity, 4) || !capacity || capacity > 8 ||
        !Read(slots, &bp, 4) || bp < 0x103C0) return 0;
    const uint32_t entity = bp - 0x3C0;
    if (!Read(entity, &vt, 4) || (vt != base + placement::kPlayerPrimaryVtableRva && vt != base + kNecroPrimaryVtableRva)) return 0;
    // A necro in the local slot 0 is local whatever its net proxy says (a client's
    // necro carried the ghost bit and the state packets stopped at the
    // transform: the host saw "partner0=no position" at every door).
    if (vt == base + kNecroPrimaryVtableRva) return entity;
    uint32_t proxy = 0, flags = 0;
    if (!Read(entity + 0x68, &proxy, 4)) return 0;
    if (proxy && (!Read(proxy + 0x30, &flags, 4) || (flags & 2))) return 0;
    return entity;
}
bool IsNecroEntity(uint32_t entity) {
    uint32_t vt = 0;
    return entity >= 0x10000 && Read(entity, &vt, 4) && vt == base + kNecroPrimaryVtableRva;
}
// Is this entity registered in a PlayerManager slot right now? A deleted Player
// keeps its vtable bytes for a while; the manager does not keep the pointer.
bool InPlayerManager(uint32_t entity) {
    uint32_t manager = 0, slots = 0, capacity = 0;
    if (!entity || !Read(base + kPlayerManagerRva, &manager, 4) || !manager ||
        !Read(manager + 4, &slots, 4) || !slots || !Read(manager + 8, &capacity, 4) || capacity > 8) return false;
    for (uint32_t i = 0; i < capacity; ++i) {
        uint32_t bp = 0;
        if (Read(slots + 4 * i, &bp, 4) && bp == entity + 0x3C0) return true;
    }
    return false;
}

// A partner's NecroPlayer ghost: necro vtable and a proxy flagged as ghost.
bool IsGhostNecro(uint32_t entity) {
    uint32_t proxy = 0, flags = 0;
    return IsNecroEntity(entity) && Read(entity + 0x5C + 0xC, &proxy, 4) && proxy && Read(proxy + 0x30, &flags, 4) && (flags & 2);
}
bool IsPlayerOrNecroEntity(uint32_t entity) {
    uint32_t vt = 0;
    return entity >= 0x10000 && Read(entity, &vt, 4) && (vt == base + placement::kPlayerPrimaryVtableRva || vt == base + kNecroPrimaryVtableRva);
}

bool HasConnectedPeer(uint32_t currentSession) {
    uint32_t iface = 0, count = 0, array = 0;
    if (!currentSession || !Read(currentSession + 0x1C, &iface, 4) || !iface ||
        !Read(iface + 0xC, &count, 4) || count > 16 ||
        !Read(iface + 0x14, &array, 4) || !array) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t connection = 0, state = 0;
        if (Read(array + 4 * i, &connection, 4) && connection &&
            Read(connection + 0x1B0, &state, 4) && state == 7) return true;
    }
    return false;
}

// Share the engine's counter; refuse exhaustion rather than reuse an ID.
bool AllocatePlayerSubKey(uint32_t currentSession, uint32_t machine, uint32_t& key) {
    uint32_t counter = 0;
    if (!currentSession || machine > 255 || !Read(currentSession + 0x18, &counter, 4) ||
        counter >= 0xFFFFFF) return false;
    ++counter;
    if (!WriteBytes(currentSession + 0x18, &counter, 4)) return false;
    key = (machine << 24) | counter;
    return true;
}

template<class... Args>
void AppendReport(char* buffer, size_t capacity, size_t& used, const char* format, Args... args) {
    if (!capacity || used >= capacity - 1) return;
    const int result = std::snprintf(buffer + used, capacity - used, format, args...);
    if (result < 0) { buffer[used] = 0; return; }
    const size_t available = capacity - used - 1;
    used += static_cast<size_t>(result) < available ? static_cast<size_t>(result) : available;
}
