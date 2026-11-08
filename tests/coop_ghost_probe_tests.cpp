// Dead Space 2 United by Richard Hafer.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cassert>
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
    const auto bytes = reinterpret_cast<unsigned char*>(&value);
    for (size_t i = 0; i < 4; ++i) memory[address + i] = bytes[i];
}
uint32_t originalResult = 0, originalCalls = 0;
namespace safetyhook {
struct InlineHook {
    template<class T> T ccall(uint32_t hash, uint32_t subkey, uint32_t stream) {
        assert(hash == 7 && subkey == 8 && stream == 9);
        ++originalCalls;
        return static_cast<T>(originalResult);
    }
    explicit operator bool() const { return true; }
};
InlineHook create_inline(void*, void*) { return {}; }
}
constexpr uint32_t kNecroPrimaryVtableRva = 0x17783A0;   // runtime_coop_state.inc
#include "../src/runtime_coop_ghost_probe.inc"

int main() {
    using namespace ghost_probe;
    Event event{}; event.hash = 7;
    Lookup(event); assert(event.lookup == 0);
    Put(base + 0x1C1C0A0, 0x100000);
    Put(0x100000 + 0x70 + 28, 0);
    Lookup(event); assert(event.lookup == 1);
    Put(0x100000 + 0x70 + 28, 0x200000);
    Lookup(event); assert(event.lookup == 3);
    Put(0x200000 + 0x1C, 7); Put(0x200000 + 8, 0x300000);
    Put(0x300000, 0x310000); Put(0x310000, 0x320000); Put(0x320008, 0x04B57466);
    for (int i = 0; i < 4; ++i) Put(0x20000C + i * 4, i);
    Lookup(event); assert(event.lookup == 2 && event.classID == 0x04B57466);
    assert(OnCreate(7, 8, 9) == 0 && originalCalls == 1);
    assert(count == 2 && events[0].phase == 0 && events[1].phase == 1);
    assert(failed == 1 && returned == 0);
    originalResult = 0x40005C;
    Put(0x400000, static_cast<uint32_t>(base + 0x1783948));
    Put(0x400194, 0x500000); Put(0x5000B8, 0x600000); Put(0x400140, 0x700000);
    assert(OnCreate(7, 8, 9) == originalResult && originalCalls == 2);
    assert(events[3].bodyInspected && events[3].bones == 0x600000);
    assert(returned == 1);
    Put(0x400000, 0x1);
    OnCreate(7, 8, 9);
    assert(events[5].bodyInspected == 0);
    Put(0x20001C, 6); Put(0x200034, 0x200000);
    event = {}; event.hash = 7; Lookup(event); assert(event.lookup == 4);
    count = 64;
    OnCreate(7, 8, 9);
    assert(count == 64 && dropped == 2 && originalCalls == 4);
    std::puts("Ghost probe lookup, argument forwarding, results, layout guards and queue bounds passed.");
}
