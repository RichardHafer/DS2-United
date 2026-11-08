// Dead Space 2 United by Richard Hafer.
#include "../src/spawn_watchdog.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>

using namespace ds2::watchdog;

// A stand-in for process memory that can fail a read, so the unreadable path is
// exercised the same way a freed page would behave.
struct Memory {
    std::map<uint32_t, unsigned char> bytes;
    bool blocked = false;
    bool operator()(uint32_t address, void* out, size_t size) const {
        if (blocked) return false;
        for (size_t i = 0; i < size; ++i) {
            const auto found = bytes.find(static_cast<uint32_t>(address + i));
            if (found == bytes.end()) return false;
            static_cast<unsigned char*>(out)[i] = found->second;
        }
        return true;
    }
    void put(uint32_t address, const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i)
            bytes[static_cast<uint32_t>(address + i)] = static_cast<const unsigned char*>(data)[i];
    }
    void erase(uint32_t address, size_t size) {
        for (size_t i = 0; i < size; ++i) bytes.erase(static_cast<uint32_t>(address + i));
    }
};

static void place(Memory& memory, uint32_t actor, uint32_t vtable, const Guid& guid) {
    memory.put(actor, &vtable, 4);
    memory.put(actor + 0x24, guid.data(), sizeof(guid));
}

int main() {
    constexpr uint32_t kTimeout = 90000;
    Memory memory;
    Tracker<4> tracker;

    const Guid guidA{1, 2, 3, 4}, guidB{5, 6, 7, 8}, donor{9, 9, 9, 9};
    place(memory, 0x1000, 0xAABB, guidA);

    // Nothing verifiable is ever tracked.
    assert(!tracker.Track(0, 0xAABB, guidA, donor, 0, true));
    assert(!tracker.Track(0x1000, 0, guidA, donor, 0, true));
    assert(!tracker.Track(0x1000, 0xAABB, Guid{}, donor, 0, true));
    assert(tracker.Live() == 0);

    assert(tracker.Track(0x1000, 0xAABB, guidA, donor, 1000, true));
    assert(tracker.Live() == 1);
    assert(tracker.Inspect(0, 1000 + kTimeout - 1, kTimeout, memory) == Verdict::Waiting);
    assert(tracker.Inspect(0, 1000 + kTimeout, kTimeout, memory) == Verdict::Expired);

    // The actor died and its memory was reused by something else.
    place(memory, 0x1000, 0xCCDD, guidA);
    assert(tracker.Inspect(0, 1000 + kTimeout, kTimeout, memory) == Verdict::Released);
    // Same vtable, different entity: a different actor in the same slot.
    place(memory, 0x1000, 0xAABB, guidB);
    assert(tracker.Inspect(0, 1000 + kTimeout, kTimeout, memory) == Verdict::Released);
    // Freed and unmapped.
    memory.erase(0x1000, 0x34);
    assert(tracker.Inspect(0, 1000 + kTimeout, kTimeout, memory) == Verdict::Unreadable);
    memory.blocked = true;
    assert(tracker.Inspect(0, 1000 + kTimeout, kTimeout, memory) == Verdict::Unreadable);
    memory.blocked = false;

    // A tick counter that wrapped past 2^32 must not read as expired.
    Tracker<4> wrapping;
    place(memory, 0x2000, 0xAABB, guidA);
    const uint32_t late = 0xFFFFFF00u;
    assert(wrapping.Track(0x2000, 0xAABB, guidA, donor, late, true));
    assert(wrapping.Inspect(0, late + 1000u, kTimeout, memory) == Verdict::Waiting);
    assert(wrapping.Inspect(0, late + kTimeout, kTimeout, memory) == Verdict::Expired);

    // Re-tracking the same actor refreshes it instead of taking a second slot.
    Tracker<4> reuse;
    assert(reuse.Track(0x2000, 0xAABB, guidA, donor, 100, true));
    assert(reuse.Track(0x2000, 0xAABB, guidA, donor, 200, true));
    assert(reuse.Live() == 1);
    assert(reuse.At(0).bornTick == 200);

    // The ring wraps and counts what it had to drop unreported.
    Tracker<4> ring;
    for (uint32_t i = 0; i < 6; ++i) {
        place(memory, 0x3000 + i * 0x100, 0xAABB, Guid{i + 1, 0, 0, 0});
        assert(ring.Track(0x3000 + i * 0x100, 0xAABB, Guid{i + 1, 0, 0, 0}, donor, 0, true));
    }
    assert(ring.Live() == 4);
    assert(ring.Dropped() == 2);
    assert(ring.At(0).actor == 0x3000 + 4 * 0x100);

    // A reported entry that is retired frees its slot.
    ring.MarkReported(0);
    ring.Retire(0);
    assert(ring.Live() == 3);
    assert(ring.Inspect(0, 0, kTimeout, memory) == Verdict::Empty);

    // The kill permission travels with the entry and is not invented later.
    Tracker<4> gated;
    place(memory, 0x4000, 0xAABB, guidA);
    assert(gated.Track(0x4000, 0xAABB, guidA, donor, 0, false));
    assert(gated.Inspect(0, kTimeout, kTimeout, memory) == Verdict::Expired);
    assert(!gated.At(0).allowKill);
    assert(gated.Track(0x4000, 0xAABB, guidA, donor, 0, true));
    assert(gated.At(0).allowKill);

    // A per-family leash overrides the global one in both directions.
    Tracker<4> leash;
    place(memory, 0x5000, 0xAABB, guidA);
    assert(leash.Track(0x5000, 0xAABB, guidA, donor, 0, true, 30000));
    assert(leash.Inspect(0, 29999, kTimeout, memory) == Verdict::Waiting);
    assert(leash.Inspect(0, 30000, kTimeout, memory) == Verdict::Expired);
    assert(leash.Track(0x5000, 0xAABB, guidA, donor, 0, true, 120000));
    assert(leash.Inspect(0, kTimeout, kTimeout, memory) == Verdict::Waiting);
    assert(leash.Inspect(0, 120000, kTimeout, memory) == Verdict::Expired);
    // Zero falls back to the global value rather than expiring immediately.
    assert(leash.Track(0x5000, 0xAABB, guidA, donor, 0, true, 0));
    assert(leash.Inspect(0, 1, kTimeout, memory) == Verdict::Waiting);
    assert(leash.Inspect(0, kTimeout, kTimeout, memory) == Verdict::Expired);

    // The table registry: one live array per class, latest observation wins,
    // and an unknown class yields nothing rather than a wrong donor.
    TableRegistry<4> registry;
    assert(registry.Count() == 0);
    assert(registry.Lookup(0x12AD6E04) == 0);
    registry.Observe(0, 0x1000, 1);          // a null id is not a class
    registry.Observe(0x12AD6E04, 0, 1);      // and a null array is not a table
    assert(registry.Count() == 0);
    registry.Observe(0x12AD6E04, 0x0C53AE60, 642);
    registry.Observe(0x43AE7E39, 0x0C53D4E8, 245);
    assert(registry.Count() == 2);
    assert(registry.Lookup(0x12AD6E04) == 0x0C53AE60);
    assert(registry.Lookup(0x43AE7E39) == 0x0C53D4E8);
    assert(registry.Lookup(0xDEADBEEF) == 0);
    // A newer generation wins: the engine hands out allocation numbers in order.
    registry.Observe(0x12AD6E04, 0x47EC58A8, 645);
    assert(registry.Count() == 2 && registry.Lookup(0x12AD6E04) == 0x47EC58A8);
    assert(registry.RankOf(0x12AD6E04) == 645);
    // An older machine ticking afterwards must not drag the class back onto the
    // block that is about to be freed. This is the case that made the naive
    // version repair the wrong way round.
    registry.Observe(0x12AD6E04, 0x0C53AE60, 117);
    assert(registry.Lookup(0x12AD6E04) == 0x47EC58A8 && registry.RankOf(0x12AD6E04) == 645);
    // It never grows past its capacity.
    for (uint32_t i = 1; i <= 8; ++i) registry.Observe(0x1000 + i, 0x2000 + i, i);
    assert(registry.Count() == 4);
    registry.Reset();
    assert(registry.Count() == 0 && registry.Lookup(0x12AD6E04) == 0);

    std::cout << "Spawn watchdog: identity re-checked on every verdict, wrap-safe timing, "
              << "ring capacity " << Tracker<4>::Capacity() << " with drop accounting\n";
}
