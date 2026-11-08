// Exercise the production probe without a game process or executable trampoline.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "../src/coop_weapon_lookup.h"

using LONG = long;
using DWORD = unsigned long;
LONG InterlockedIncrement(volatile LONG* value) { return ++*value; }
LONG InterlockedCompareExchange(volatile LONG* value, LONG replacement, LONG comparand) {
    const LONG old = *value;
    if (old == comparand) *value = replacement;
    return old;
}
DWORD clockNow = 0;
DWORD GetTickCount() { return clockNow; }
bool configRequested = false, installSucceeds = false;
int GetPrivateProfileIntA(const char*, const char*, int, const char*) { return configRequested; }
uintptr_t base = 0x400000;
namespace placement { constexpr uint32_t kPlayerPrimaryVtableRva = 0x17294D0; }
std::map<uint32_t, std::vector<unsigned char>> memory;
bool Read(uintptr_t address, void* out, size_t bytes) {
    auto it = memory.find(static_cast<uint32_t>(address));
    if (it == memory.end() || it->second.size() < bytes) return false;
    std::memcpy(out, it->second.data(), bytes); return true;
}
void Word(uint32_t address, uint32_t value) {
    auto& data = memory[address]; data.resize(4);
    std::memcpy(data.data(), &value, 4);
}
unsigned nativeCalls = 0, installCalls = 0;
void* lastSelf = nullptr;
std::array<uint32_t, 3> lastArgs{};
namespace safetyhook {
struct InlineHook {
    bool installed = false;
    explicit operator bool() const { return installed; }
    template<class T> T thiscall(void* self, uint32_t hash, uint32_t ammo, uint32_t plasma) {
        assert(installed);
        ++nativeCalls; lastSelf = self; lastArgs = {hash, ammo, plasma};
    }
    template<class T> T thiscall(void*, uint32_t) { assert(installed); }
};
InlineHook create_inline(void* target, void* callback) {
    assert(target == reinterpret_cast<void*>(0x86BAD0) && callback);
    ++installCalls; return {installSucceeds};
}
}
// hook_site.h: the gun owner hook goes through the shared installer.
unsigned ownerInstallCalls = 0;
namespace hook_site {
bool Install(FILE*, const char*, uintptr_t address, const unsigned char*, size_t, void* callback,
             safetyhook::InlineHook& hook) {
    assert(address == 0x400000 + 0x9C09B0 && callback);
    ++ownerInstallCalls;
    hook = {false};
    return false;
}
}
#include "../src/runtime_coop_weapon_probe.inc"

int main() {
    FILE* log = nullptr;
    assert(tmpfile_s(&log) == 0 && log);
    weapon_probe::Install(log, "unused");
    assert(!weapon_probe::hook && installCalls == 0);
    configRequested = true;
    weapon_probe::Install(log, "unused");
    assert(!weapon_probe::hook && installCalls == 0);
    memory[0x86BAD0] = {0x8B,0x44,0x24,0x04,0x53,0x55,0x56,0x57,
        0x8B,0xD9,0x50,0x8D,0x8B,0x04,0x0C,0x00,0x00};
    weapon_probe::Install(log, "unused");
    assert(!weapon_probe::hook && installCalls == 1);
    installSucceeds = true;
    weapon_probe::Install(log, "unused");
    assert(weapon_probe::hook && installCalls == 2 && ownerInstallCalls == 2);
    constexpr uint32_t p = 0x100000, entries = 0x200000, gun = 0x300000;
    Word(p, 0x1B294D0); Word(p+0xC08, entries); Word(p+0xC0C, 1); Word(p+0xC10, 4);
    Word(entries+4, gun+0x10); Word(gun+0x25C, 123); Word(gun+0x240, p+0x10);
    Word(p+0xCEC, gun+0x10); Word(p+0x424, 0);
    Word(gun, 0x1B51A00);
    for (unsigned i=0; i<4; ++i) {
        Word(gun+0x24+i*4, 0x10+i);
        Word(gun+0x404+i*4, 0x20+i);
    }
    auto invoke = [&](uint32_t player, uint32_t hash, uint32_t ammo, uint32_t plasma) {
        const auto before = memory;
        const auto callsBefore = nativeCalls;
        auto* self = reinterpret_cast<void*>(player);
        weapon_probe::OnUpdate(self, nullptr, hash, ammo, plasma);
        const std::array<uint32_t, 3> expected{hash, ammo, plasma};
        assert(nativeCalls == callsBefore+1 && lastSelf == self && lastArgs == expected);
        assert(memory == before);
    };
    for (unsigned i = 0; i < 40; ++i) invoke(p, 123, 0xABCD002A, 0x10001);
    assert(weapon_probe::count == 1 && weapon_probe::samples[0].readable == 3);
    assert(weapon_probe::samples[0].ammo == 42 && weapon_probe::samples[0].plasma == 1);
    assert(weapon_probe::samples[0].active == gun+0x10);
    assert(weapon_probe::samples[0].identity.readable == 7);
    assert(weapon_probe::samples[0].identity.proxyGuid[3] == 0x23);
    Word(gun+0x240, 0);
    invoke(p, 123, 1, 0);
    assert(weapon_probe::wrongOwner == 1);
    invoke(p, 999, 1, 0);
    assert(weapon_probe::missing == 1);
    memory.erase(p+0x424);
    invoke(p, 0, 0, 0);
    assert(weapon_probe::samples[0].readable == 1);
    for (unsigned i = 1; i <= 9; ++i) invoke(p+i*0x10000, 123, i, 0);
    assert(weapon_probe::count == 8 && weapon_probe::dropped == 2);
    assert(weapon_probe::invalid == 9 && weapon_probe::calls == 52);
    clockNow = 999; weapon_probe::Flush(log);
    assert(weapon_probe::count == 8);
    clockNow = 1000; weapon_probe::Flush(log);
    assert(weapon_probe::count == 0);
    invoke(p, 0, 0, 0);
    clockNow = 1999; weapon_probe::Flush(log);
    assert(weapon_probe::count == 1);
    clockNow = 2000; weapon_probe::Flush(log);
    assert(weapon_probe::count == 0);
    std::rewind(log);
    std::string output;
    char buffer[2048];
    while (std::fgets(buffer, sizeof(buffer), log)) output += buffer;
    for (const char* expected : {"config=0", "signature_mismatch", "hook=0", "hook=1",
         "readable_mask=1", "calls=52 missing=1 invalid=9 wrong_owner=1 dropped=2"})
        assert(output.find(expected) != std::string::npos);
    std::fclose(log);
    std::puts("Production weapon probe: forwarding, install gates, failure counters and bounded reports passed.");
}
