// Dead Space 2 United by Richard Hafer.
// Exercise the production callback with a counted original and no game process.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "../src/coop_save_policy.h"

using LONG = long;
LONG InterlockedIncrement(volatile LONG* value) { return ++*value; }
LONG InterlockedCompareExchange(volatile LONG* value, LONG replacement, LONG comparand) {
    const LONG old = *value;
    if (old == comparand) *value = replacement;
    return old;
}
bool configRequested = false, installSucceeds = false;
int GetPrivateProfileIntA(const char*, const char*, int, const char*) { return configRequested; }
uintptr_t base = 0x400000;
namespace placement { constexpr uint32_t kPlayerPrimaryVtableRva = 0x17294D0; }
std::map<uint32_t, std::vector<unsigned char>> memory;
bool Read(uintptr_t address, void* out, size_t bytes) {
    auto it = memory.find(static_cast<uint32_t>(address));
    if (it == memory.end() || it->second.size() < bytes) return false;
    std::memcpy(out, it->second.data(), bytes);
    return true;
}
void Word(uint32_t address, uint32_t value) {
    auto& data = memory[address]; data.resize(4);
    std::memcpy(data.data(), &value, 4);
}
unsigned nativeCalls = 0, installCalls = 0;
void* lastSelf = nullptr;
namespace safetyhook {
struct InlineHook {
    bool installed = false;
    explicit operator bool() const { return installed; }
    template<class T> T thiscall(void* self) {
        assert(installed);
        ++nativeCalls; lastSelf = self;
    }
};
InlineHook create_inline(void*, void*) { ++installCalls; return {installSucceeds}; }
}
// runtime_coop.inc: the companion bot registry the save hook consults.
bool botWanted = false;
uint32_t botEntity = 0;
bool IsBotEntity(uint32_t entity) { return entity && entity == botEntity; }
#include "../src/runtime_coop_save.inc"

int main() {
    FILE* log = nullptr;
    assert(tmpfile_s(&log) == 0 && log);
    save_guard::Install(log, "unused");
    assert(!save_guard::hook && installCalls == 0);
    configRequested = true;
    save_guard::Install(log, "unused");
    assert(!save_guard::hook && installCalls == 0);
    memory[0x87C7F0] = {0xA1,0xA4,0xC0,0x01,0x02,0x83,0xEC,0x10,0x57,0x8B,0xF9,
        0x8A,0x48,0x1C,0xD0,0xE9,0xF6,0xC1,0x01,0x0F,0x85,0xCA,0x02,0x00,0x00};
    save_guard::Install(log, "unused");
    assert(!save_guard::hook && installCalls == 1);
    installSucceeds = true;
    save_guard::Install(log, "unused");
    assert(save_guard::hook && installCalls == 2);
    constexpr uint32_t p = 0x100000, proxy = 0x200000;
    Word(p, 0x1B294D0); Word(p + 0x68, proxy);
    Word(proxy + 0x30, 2); Word(proxy + 0x40, p + 0x5C);
    auto* self = reinterpret_cast<void*>(p);
    const auto before = memory;
    for (unsigned i = 0; i < 40; ++i) save_guard::OnStore(self, nullptr);
    assert(nativeCalls == 0 && save_guard::blocked == 40);
    assert(memory == before && save_guard::count == 32 && save_guard::dropped == 8);
    save_guard::Flush(log);
    assert(save_guard::count == 0);
    Word(proxy + 0x30, 8);
    save_guard::OnStore(self, nullptr);
    assert(nativeCalls == 1 && lastSelf == self && save_guard::passed == 1);
    Word(proxy + 0x40, p + 0x100);
    save_guard::OnStore(self, nullptr);
    assert(nativeCalls == 2 && save_guard::unknown == 1);
    Word(p + 0x68, 0);
    save_guard::OnStore(self, nullptr);
    assert(nativeCalls == 3 && save_guard::passed == 3);
    save_guard::Flush(log);
    std::rewind(log);
    std::string output;
    char buffer[2048];
    while (std::fgets(buffer, sizeof(buffer), log)) output += buffer;
    for (const char* expected : {"config=0", "signature_mismatch", "hook=0", "hook=1",
         "action=blocked", "action=native", "blocked=40 returned=3 unknown=1 log_dropped=8"})
        assert(output.find(expected) != std::string::npos);
    // A companion bot's Isaac never stores, whatever its proxy says.
    botEntity = p;
    save_guard::OnStore(self, nullptr);
    assert(nativeCalls == 3 && save_guard::blocked == 41);
    // The flush log names the bot block, not "native".
    {
        std::fseek(log, 0, SEEK_END);   // reading switches to writing only through a seek
        const long at = std::ftell(log);
        save_guard::Flush(log);
        std::fseek(log, at, SEEK_SET);
        std::string botLines;
        while (std::fgets(buffer, sizeof(buffer), log)) botLines += buffer;
        assert(botLines.find("action=blocked_bot") != std::string::npos);
        assert(botLines.find("action=native") == std::string::npos);
    }
    // Bots alone request the guard even when ProtectLocalSave is off.
    configRequested = false; botWanted = true;
    const unsigned installsBefore = installCalls;
    save_guard::Install(log, "unused");
    assert(installCalls == installsBefore + 1);
    std::fclose(log);
    std::puts("Production save callback: install gates, ghost suppression, native forwarding, bounded logs passed.");
}
