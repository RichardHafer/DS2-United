// Dead Space 2 United by Hafer.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Watchdog for enemies the randomizer put into the world.
//
// A replaced enemy that never dies can hold a room closed, so each one is
// tracked from its creation and reported once it outlives the timeout.
//
// The hard part is not the timer, it is knowing whether the recorded pointer
// still refers to the same object. An actor that died is freed and its memory
// reused, so a stale pointer can look like anything. Every entry therefore
// carries the actor's vtable and its entity GUID, and both must still match
// before the entry is believed. A mismatch means the object is gone, which is
// the ordinary outcome and is not reported.
namespace ds2::watchdog {
using Guid = std::array<uint32_t, 4>;

enum class Verdict {
    Empty,        // slot unused
    Waiting,      // inside the timeout
    Released,     // vtable or GUID no longer match: the actor is gone
    Unreadable,   // memory could not be read: treat as gone
    Expired,      // still the same actor after the timeout
};

struct Entry {
    uint32_t actor = 0;
    uint32_t vtable = 0;
    Guid guid{};
    uint32_t bornTick = 0;
    Guid selected{};      // the donor GUID this actor was created from
    bool used = false;
    bool reported = false;
    // A replaced enemy is probed once, a few seconds in, to see whether it is
    // doing something or still sitting in its arrival.
    bool probed = false;
    // Whether this kind of enemy may be killed at all. Decided at creation,
    // where the donor's family is known, so the watchdog itself stays free of
    // the catalog.
    bool allowKill = false;
    // Per-family leash, decided at creation. Zero means use the global value.
    // Some kinds get stuck more readily than others and deserve a shorter one.
    uint32_t timeoutMs = 0;
};

// State machine layout, from the Xbox PDB and confirmed on live objects:
// +0x08 m_stateTableID, +0x0C m_curStateIdx, +0x10 m_pStateArr, +0x18 m_pTree,
// +0x1C m_pParent, +0x20 m_pChildHead, +0x28 m_pNext. A State is 20 bytes with
// m_transArr at +0x00 and m_pUpdateMessage at +0x0C, and the update walks both
// without a null check.
namespace machine {
constexpr uint32_t kTableId = 0x08, kCurState = 0x0C, kStateArr = 0x10;
constexpr uint32_t kChildHead = 0x20, kNext = 0x28, kAllocNumber = 0x2C;
constexpr uint32_t kStateTransArr = 0x00, kStateUpdateMessage = 0x0C;
// EARS::StateMachineSys::State is 20 bytes and holds its name at +0x04.
constexpr uint32_t kStateName = 0x04, kStateSize = 20;

// A state table id is hash(class name) + 7, with the engine's own name hash.
// Verified against the two ids the crash dumps carried: DeadSpaceNPCMainSM is
// 0x43AE7E39 and DeadSpaceNPCDeadSM is 0x12AD6E04, both reproduced exactly.
// These five are the machines the Xbox symbols name for vent behaviour, and an
// enemy that still has one of them running has not come out.
struct NamedTable { uint32_t id; const char* name; };
constexpr NamedTable kVentTables[] = {
    {0x0685D635u, "DeadSpaceNPCVentSM"},
    {0xDA9D292Fu, "DeadSpaceNPCEnterVentSM"},
    {0x3CD8A453u, "DeadSpaceNPCExitVentSM"},
    {0xBF6D0FE9u, "DeadSpaceNPCVentIdleSM"},
    {0xB24F03F3u, "NavVentLinkSM"},
};
inline const char* ventTableName(uint32_t id) {
    for (const auto& entry : kVentTables)
        if (entry.id == id) return entry.name;
    return nullptr;
}
// An actor running this machine is a corpse the player already made. The object
// stays in the world, so the leash cannot tell it apart from an enemy that
// never engaged, and "killed" then counts bodies, not stranded enemies.
constexpr uint32_t kDeadTable = 0x12AD6E04u;
// The actor holds its state machine tree here, and the tree holds the root
// machine. Both offsets were read off matching pairs in a crash dump.
constexpr uint32_t kActorTree = 0x248, kTreeRoot = 0x04;
}

// Which state table array a class is currently using, keyed by table id.
//
// The generation is decided by m_allocNumber, which the engine hands out in
// order. In the dump the surviving machines carried 55 to 642 and pointed at a
// freed block, while everything from 645 on pointed at the new one, so the
// highest allocation number wins. Without that rank an old machine ticking
// first after a rebuild would teach the registry the dead table and drag the
// new machines onto it.
template<size_t N>
class TableRegistry {
public:
    void Reset() { count_ = 0; }
    void Observe(uint32_t tableId, uint32_t array, uint32_t rank) {
        if (!tableId || !array) return;
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].tableId != tableId) continue;
            if (rank >= entries_[i].rank) { entries_[i].array = array; entries_[i].rank = rank; }
            return;
        }
        if (count_ < N) entries_[count_++] = {tableId, array, rank};
    }
    uint32_t Lookup(uint32_t tableId) const {
        for (size_t i = 0; i < count_; ++i)
            if (entries_[i].tableId == tableId) return entries_[i].array;
        return 0;
    }
    uint32_t RankOf(uint32_t tableId) const {
        for (size_t i = 0; i < count_; ++i)
            if (entries_[i].tableId == tableId) return entries_[i].rank;
        return 0;
    }
    size_t Count() const { return count_; }

private:
    struct Entry { uint32_t tableId = 0, array = 0, rank = 0; };
    std::array<Entry, N> entries_{};
    size_t count_ = 0;
};

// A wrapping ring: the oldest entry is dropped rather than losing a new one.
// Enemies are created in bursts, so a full ring is expected, not an error.
template<size_t N>
class Tracker {
public:
    void Reset() { entries_ = {}; next_ = 0; dropped_ = 0; }

    // Ignores an actor without a readable vtable or a null identity: nothing
    // could be verified about it later.
    bool Track(uint32_t actor, uint32_t vtable, const Guid& guid, const Guid& selected,
               uint32_t tick, bool allowKill, uint32_t timeoutMs = 0) {
        if (!actor || !vtable || guid == Guid{}) return false;
        const Entry fresh{actor, vtable, guid, tick, selected, true, false, false,
                          allowKill, timeoutMs};
        for (auto& entry : entries_)
            if (entry.used && entry.actor == actor) {
                entry = fresh;
                return true;
            }
        auto& slot = entries_[next_];
        if (slot.used && !slot.reported) ++dropped_;
        slot = fresh;
        next_ = (next_ + 1) % N;
        return true;
    }

    // Reader must be a bounded, failure-tolerant memory read.
    // timeoutMs is the fallback for entries that carry no leash of their own.
    template<class Reader>
    Verdict Inspect(size_t index, uint32_t now, uint32_t timeoutMs, Reader& read) const {
        const auto& entry = entries_[index];
        if (!entry.used) return Verdict::Empty;
        const uint32_t leash = entry.timeoutMs ? entry.timeoutMs : timeoutMs;
        if (now - entry.bornTick < leash) return Verdict::Waiting;
        uint32_t vtable = 0;
        Guid guid{};
        if (!read(entry.actor, &vtable, 4) || !read(entry.actor + 0x24, guid.data(), sizeof(guid)))
            return Verdict::Unreadable;
        if (vtable != entry.vtable || guid != entry.guid) return Verdict::Released;
        return Verdict::Expired;
    }

    void Retire(size_t index) { entries_[index] = {}; }
    void MarkReported(size_t index) { entries_[index].reported = true; }
    void MarkProbed(size_t index) { entries_[index].probed = true; }
    const Entry& At(size_t index) const { return entries_[index]; }
    static constexpr size_t Capacity() { return N; }
    size_t Dropped() const { return dropped_; }
    size_t Live() const {
        size_t count = 0;
        for (const auto& entry : entries_) count += entry.used;
        return count;
    }

private:
    std::array<Entry, N> entries_{};
    size_t next_ = 0;
    size_t dropped_ = 0;
};
}  // namespace ds2::watchdog
