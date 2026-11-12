// Dead Space 2 United by Richard Hafer.
#pragma once
#include <cmath>
#include <cstdint>

// Whether an enemy is in a fight right now. The watchdogs judge an enemy by its age, its
// movement and its distance; none of that tells a stuck enemy from one that holds a player
// in a grab. A slasher killed in the middle of a QTE was the result (reported 13.11.2026).
//
// The grab: Sentient::m_pPairedCoordinator, PC +0x2A8 (Xbox +0x2C8, DeadSpaceNPC derives
// from Sentient at offset 0). PairedAttackCoordinator::SetSentients (0x010AA040) revokes
// and counts the coordinator on both of its sentients, so the attacker carries it as well
// as the victim. The coordinator (vtable 0x01B731D4) keeps the attacker as a SafePtr at
// +0x88 and the defender at +0x90; a SafePtr points 0x10 into its entity. The players are
// checked from their side as well, in case an enemy class leaves its own field empty.
namespace ds2::engaged {
constexpr uint32_t kPairedCoordinator = 0x2A8;
constexpr uint32_t kCoordinatorVtableRva = 0x17731D4, kCoordAttacker = 0x88, kCoordDefender = 0x90, kSafeToEntity = 0x10;
constexpr uint32_t kPlayerManagerRva = 0x1C1C0D4, kBasePlayer = 0x3C0, kPlayerVtableRva = 0x17294D0, kEntityPos = 0xC0;

enum class Reason { None, Grab, Held, PlayerGrab, Near };

inline const char* ReasonName(Reason r) {
    switch (r) {
    case Reason::Grab: return "grab";               // its own coordinator, as the attacker
    case Reason::Held: return "held";               // its own coordinator, as the defender
    case Reason::PlayerGrab: return "player_grab";  // a player's coordinator names it as the attacker
    case Reason::Near: return "near_player";
    default: return "none";
    }
}

struct Result {
    Reason reason = Reason::None;
    float meters = 1e9f;        // to the nearest player, 1e9 when no player could be read
    uint32_t coordinator = 0;
    bool Engaged() const { return reason != Reason::None; }
};

// The coordinator's attacker or defender, when ref holds a SafePtr to an entity.
template<class Reader>
uint32_t SideOf(uint32_t coordinator, uint32_t side, Reader& read) {
    uint32_t ref = 0;
    if (!read(coordinator + side, &ref, 4) || ref < 0x10000 + kSafeToEntity) return 0;
    return ref - kSafeToEntity;
}

template<class Reader>
bool IsCoordinator(uint32_t coordinator, uintptr_t base, Reader& read) {
    uint32_t vtable = 0;
    return coordinator >= 0x10000 && read(coordinator, &vtable, 4) && vtable == base + kCoordinatorVtableRva;
}

// nearMeters 0 leaves distance out. Reads only, safe on any thread with a
// failure-tolerant reader.
template<class Reader>
Result Check(uint32_t actor, uintptr_t base, float nearMeters, Reader& read) {
    Result r;
    if (!actor || !base) return r;
    uint32_t own = 0;
    if (read(actor + kPairedCoordinator, &own, 4) && IsCoordinator(own, base, read)) {
        if (SideOf(own, kCoordAttacker, read) == actor) { r.reason = Reason::Grab; r.coordinator = own; }
        else if (SideOf(own, kCoordDefender, read) == actor) { r.reason = Reason::Held; r.coordinator = own; }
    }
    float at[3]{};
    const bool havePos = read(actor + kEntityPos, at, sizeof(at)) && at[0] == at[0] && std::fabs(at[0]) < 1e6f;
    uint32_t manager = 0, slots = 0, capacity = 0;
    if (!read(static_cast<uint32_t>(base + kPlayerManagerRva), &manager, 4) || !manager || !read(manager + 4, &slots, 4) || !slots ||
        !read(manager + 8, &capacity, 4) || !capacity || capacity > 8)
        return r;
    for (uint32_t i = 0; i < capacity; ++i) {
        uint32_t bp = 0, vtable = 0, coord = 0;
        if (!read(slots + 4 * i, &bp, 4) || bp < 0x10000 + kBasePlayer) continue;
        const uint32_t player = bp - kBasePlayer;
        if (!read(player, &vtable, 4) || vtable != base + kPlayerVtableRva) continue;
        if (r.reason == Reason::None && read(player + kPairedCoordinator, &coord, 4) && IsCoordinator(coord, base, read) &&
            SideOf(coord, kCoordAttacker, read) == actor) {
            r.reason = Reason::PlayerGrab;
            r.coordinator = coord;
        }
        float p[3]{};
        if (havePos && read(player + kEntityPos, p, sizeof(p))) {
            const float dx = p[0] - at[0], dy = p[1] - at[1], dz = p[2] - at[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < r.meters) r.meters = d;
        }
    }
    if (r.reason == Reason::None && nearMeters > 0.0f && r.meters <= nearMeters) r.reason = Reason::Near;
    return r;
}
}  // namespace ds2::engaged
