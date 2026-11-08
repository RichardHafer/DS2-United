#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <string>

uintptr_t base = 0x400000;
std::map<uintptr_t, unsigned char> memory;
bool Read(uintptr_t address, void* out, size_t size) {
    auto bytes = static_cast<unsigned char*>(out);
    for (size_t i = 0; i < size; ++i) {
        auto found = memory.find(address + i);
        if (found == memory.end()) return false;
        bytes[i] = found->second;
    }
    return true;
}
void Put(uintptr_t address, uint32_t value) {
    auto bytes = reinterpret_cast<unsigned char*>(&value);
    for (size_t i = 0; i < 4; ++i) memory[address + i] = bytes[i];
}
void Class(uint32_t address, uint32_t type, uint32_t parent = 0) {
    Put(address, static_cast<uint32_t>(base + 0x171E648)); Put(address + 8, type);
    memory[address + 0x49] = parent ? 1 : 0;
    Put(address + 0x1C, address + 0x100); Put(address + 0x100, parent);
}
bool originalAllowed = true;
unsigned originalCalls = 0;
namespace safetyhook {
struct InlineHook {
    template<class T> T thiscall(void*, bool prior, uint32_t) {
        ++originalCalls; return static_cast<T>(prior && originalAllowed);
    }
    template<class T> T thiscall(void*) { assert(false); }
    template<class T> T thiscall(void*, uint32_t) { assert(false); }
    explicit operator bool() const { return true; }
};
InlineHook create_inline(void*, void*) { return {}; }
struct MidHook { explicit operator bool() const { return false; } };
}
struct SafetyHookContext { uintptr_t eax, ebx, ecx, edx, esi, edi, ebp, esp; };
namespace safetyhook {
MidHook create_mid(void*, void (*)(SafetyHookContext&)) { return {}; }
}
// The rest of runtime_coop*.inc that runtime_coop_npcs.inc consults.
namespace hook_site {
bool Install(FILE*, const char*, uintptr_t, const unsigned char*, size_t, void*, safetyhook::InlineHook&) { return false; }
}
namespace gate {
constexpr uint32_t kRdataLo = 0x1600000, kRdataHi = 0x1880000;
volatile LONG lastSequenceTick = 0;
void Event(const char*, ...) {}
}
namespace hits {
constexpr uint32_t kCodeEnd = 0x16A1000;
}
namespace guidfix {
constexpr uint32_t kReflectableListRva = 0x1C5B744, kEntityReflectable = 0x5C;
bool IsGhostEntity(uint32_t) { return false; }
}
struct PeerTableStub { unsigned Count() const { return 0; } } peerTable;
bool inLevel = false;
bool IsBotEntity(uint32_t) { return false; }
bool AnyBot() { return false; }
bool IsGhostPlayer(uint32_t) { return false; }
bool IsGhostNecro(uint32_t) { return false; }
bool IsPlayerOrNecroEntity(uint32_t) { return false; }
bool WriteBytes(uintptr_t, const void*, size_t) { return true; }
uint32_t session = 1, machineIndex = 0;
constexpr uint32_t kAddToSession = 0x3E10E0;
bool replicationReady = true, faulted = false;
int tickMode = 2;
bool AllocatePlayerSubKey(uint32_t, uint32_t, uint32_t&) { return false; }
int TickFilter(unsigned, EXCEPTION_POINTERS*) { return EXCEPTION_EXECUTE_HANDLER; }
#include "../src/runtime_coop_npc_policy.inc"
#include "../src/runtime_coop_npcs.inc"

int main() {
    using npc_policy::Classification;
    Class(0x100000, 0x71BC20FF);
    Class(0x200000, 10, 0x100000);
    Class(0x300000, 11, 0x200000);
    Class(0x400000, 12);
    assert(npc_policy::Classify(0x300000) == Classification::NPC);
    assert(npc_policy::Classify(0x400000) == Classification::Other);
    assert(npc_policy::Classify(0x900000) == Classification::Unknown);
    memory[0x300049] = 17;
    assert(npc_policy::Classify(0x300000) == Classification::Unknown);
    memory[0x300049] = 1;
    // Native refusal always wins; client NPC suppression leaves other classes alone.
    Put(0x500008, 0); Put(0x600010, 0x610000); Put(0x610000, 0x300000);
    npcs::client = true;
    assert(!npcs::OnShouldSpawn(reinterpret_cast<void*>(0x500000), nullptr, true, 0x600000));
    assert(npcs::suppressed == 1 && originalCalls == 1);
    Put(0x610000, 0x400000);
    assert(npcs::OnShouldSpawn(reinterpret_cast<void*>(0x500000), nullptr, true, 0x600000));
    assert(!npcs::OnShouldSpawn(reinterpret_cast<void*>(0x500000), nullptr, false, 0x600000));
    Put(0x610000, 0x900000);
    assert(npcs::OnShouldSpawn(reinterpret_cast<void*>(0x500000), nullptr, true, 0x600000));
    assert(npcs::unknown == 1);
    Put(0x610000, 0x300000); Put(0x500008, 2);
    assert(npcs::OnShouldSpawn(reinterpret_cast<void*>(0x500000), nullptr, true, 0x600000));
    Put(0x500008, 0); npcs::client = false;
    assert(npcs::OnShouldSpawn(reinterpret_cast<void*>(0x500000), nullptr, true, 0x600000));

    // Registration never replaces an existing proxy or calls unknown callbacks.
    // The grab coordinator is recognised by its vtable, so no class rep getter runs here.
    const uint32_t coordinatorVt = static_cast<uint32_t>(base + npcs::kCoordinatorReflectableVtableRva);
    Put(0x700000, coordinatorVt);
    Put(0x70000C, 0x720000); npcs::Register(0x700000, false);
    assert(npcs::existing == 1 && npcs::coordinatorsSeen == 1);
    Put(0x70000C, 0); Put(coordinatorVt + 8, 1); Put(coordinatorVt + 0x30, 2);
    npcs::Register(0x700000, false); assert(npcs::invalid == 1 && npcs::rejectSlots == 1);
    Put(coordinatorVt + 8, static_cast<uint32_t>(base + 0x3D1D10));
    Put(coordinatorVt + 0x30, static_cast<uint32_t>(base + 0x3D1DC0));
    npcs::Register(0x700000, false);
    assert(npcs::invalid == 2 && npcs::rejectKey == 1 && npcs::registered == 0);
    // Anything else needs a readable plain class rep getter before it is even considered.
    Put(0x800000, 0x810000); Put(0x810078, static_cast<uint32_t>(base + 0xBDB2B0));
    npcs::Register(0x800000, true); assert(npcs::rejectGetter == 1 && npcs::registered == 0);
    std::puts("NPC ancestry, original spawn decisions, role/mode isolation and registration guards passed.");
}
