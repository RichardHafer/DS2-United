#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <thread>
#include <type_traits>
#include "../src/coop_peers.h"
#include "../src/coop_suit_protocol.h"
using Guid = std::array<unsigned, 4>;
uintptr_t base = 0x400000;
void (*g_uniformPoll)() = nullptr;
bool (*g_uniformPreparePose)(uint32_t) = nullptr;
bool (*g_uniformGhostDetached)(uint32_t) = nullptr;
namespace coop_uniform { int SuitIndex(const uint32_t*); }
namespace coop_mpmap { constexpr unsigned kRunning = 3; }
std::map<uintptr_t, unsigned char> memory;
bool Read(uintptr_t address, void* out, size_t bytes) {
    if (memory.count(address)) {
        auto dst = static_cast<unsigned char*>(out);
        for (size_t i = 0; i < bytes; ++i) {
            auto found = memory.find(address + i);
            if (found == memory.end()) return false;
            dst[i] = found->second;
        }
        return true;
    }
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out, bytes, &done) && done == bytes;
}
template<class T> void Put(uintptr_t address, const T& value) {
    auto bytes = reinterpret_cast<const unsigned char*>(&value);
    for (size_t i = 0; i < sizeof(T); ++i) memory[address + i] = bytes[i];
}
bool WriteBytes(uintptr_t address, const void* value, size_t size) {
    auto bytes = static_cast<const unsigned char*>(value);
    for (size_t i = 0; i < size; ++i) memory[address + i] = bytes[i];
    return true;
}
namespace safetyhook {
struct InlineHook {
    template<class T, class... Args> T thiscall(void*, Args...);
    explicit operator bool() const { return true; }
    void reset() {}
};
InlineHook create_inline(void*, void*) { return {}; }
}
namespace placement { constexpr uint32_t kPlayerPrimaryVtableRva = 0x17294D0; }
namespace frame_cap { bool installed = true; }
namespace coop {
DWORD Stamp(DWORD now) { return now ? now : 1; }
uint32_t LocalSlotZeroEntity() { return 0; }
namespace gate { void Event(const char*, ...) {} }
namespace lifecycle {
bool birthLoadOnly = false, birthPending = false;
uint32_t bornPlayer = 0;
bool RestartBodyWindow() { return false; }
}
namespace loadsync {
struct { bool armed, consumed; } joinSuit{};
int (*suitState)(char*, size_t) = nullptr;
bool JoinSuitPending() { return false; }
bool TakeJoinSuit(uint32_t, uint32_t*, const char**) { return false; }
void WriteJoinSuitDiagnostics(FILE*) {}
}
namespace play { bool StoryCoop() { return true; } }
namespace mpmap { unsigned LmState(void*) { return 0; } }
namespace mpsuit {
volatile LONG ghostRefusals = 0;
bool IsMpSuit(const uint32_t*) { return false; }
bool PartnersReady() { return true; }
}
safetyhook::InlineHook ghostAnimHook2;
uint32_t ghost = 0;
bool IsGhostPlayer(uint32_t player) { return player && player == ghost; }
// The rest of runtime_coop.inc the uniform module consults: no bots, no peers,
// suit sync off, and the donor counts as the local campaign player.
constexpr uint32_t kNoMachine = 0xFFFFFFFFu;
bool suitSync = false;
coop_suit::Sender suitSender;
coop_suit::Receiver suitReceiver;
struct PeerRuntime { coop_suit::Receiver suit; };
coop_peers::Table peerTable;
PeerRuntime peerRuntime[coop_peers::kMaxPeers]{};
uint32_t localPlayer = 0;
bool IsBotEntity(uint32_t) { return false; }
bool InPlayerManager(uint32_t player) { return player != 0; }
uint32_t LocalCampaignPlayer() { return localPlayer; }
uint32_t MachineOf(uint32_t) { return kNoMachine; }
uint32_t GhostForMachine(uint32_t) { return 0; }
constexpr unsigned kMaxBots = 3;
namespace botnet {
struct BotSuitState { uint32_t guid[4]{}; uint32_t generation = 0; bool have = false; };
BotSuitState botSuits[kMaxBots];
volatile LONG botSuitSent = 0, botSuitIn = 0, botSuitChanges = 0;
bool WornSuit(uint32_t, uint32_t*) { return false; }
}
}
#include "../src/runtime_coop_uniform.inc"

constexpr uint32_t manager = 0x100000, donorPlayer = 0x101000, targetPlayer = 0x103000;
unsigned nativeCalls = 0;
Guid received{};
bool receivedNull = false, stealDonor = false;

template<class T, class... Args> T safetyhook::InlineHook::thiscall(void*, Args... args) {
    if constexpr (std::is_same_v<T, bool>) return false;
    else {
        auto tuple = std::make_tuple(args...);
        const uint32_t player = std::get<0>(tuple);
        const Guid* guid = std::get<1>(tuple);
        ++nativeCalls;
        receivedNull = !guid;
        if (guid) {
            received = *guid;
            if (player == targetPlayer) {
                Put(player + 0x140, uint32_t(0x109000));
                Put(player + 0x1688, uint32_t(0x109100));
                Put(0x107000 + 0xB8, uint32_t(0x109200));
                if (stealDonor) Put(donorPlayer + 0x140, uint32_t(0));
            }
        }
    }
}
void Reset() {
    memory.clear(); nativeCalls = 0; received = {}; receivedNull = false; stealDonor = false;
    coop_uniform::installed = true; coop_uniform::stopped = 0; coop_uniform::nesting = 0;
    coop_uniform::messageCount = 0; coop::ghost = targetPlayer;
    Put(base + coop_uniform::kManager, manager);
    Put(manager, uint32_t(base + coop_uniform::kManagerVtable));
    Put(manager + 0x10, coop_uniform::Array{0x105000, 1, 16});
    Put(manager + 0x1C, coop_uniform::Array{0x105100, 1, 16});
    Put(0x105000, coop_uniform::UniformEntry{1, 0, 1, 0, {11, 22, 33, 44}, 0x1234});
    Put(0x105100, coop_uniform::PlayerEntry{0, 0, 0, donorPlayer + 0x10, 0});
    for (uint32_t p : {donorPlayer, targetPlayer}) {
        for (unsigned i = 0; i < 0x1800; ++i) memory[p + i] = 0;
        Put(p, uint32_t(base + placement::kPlayerPrimaryVtableRva));
        const uint32_t view = p == donorPlayer ? 0x106000 : 0x107000;
        Put(p + 0x194, view);
        Put(p + 0x188, uint32_t(0x108000));
        Put(view + 0xB0, uint32_t(132));
        Put(view + 0xB8, p == donorPlayer ? uint32_t(0x108100) : uint32_t(0));
    }
    Put(donorPlayer + 0x140, uint32_t(0x108200));
    Put(donorPlayer + 0x1688, uint32_t(0x108300));
    Put(donorPlayer + 0x54, targetPlayer);
    Put(targetPlayer + 0x50, donorPlayer + 0x54);
}
int main() {
    Guid remote{55, 66, 77, 88};
    const Guid local{11, 22, 33, 44};
    Reset();
    coop_uniform::OnChange(reinterpret_cast<void*>(manager), nullptr, targetPlayer, &remote);
    assert(nativeCalls == 1 && received == local && !coop_uniform::stopped);
    assert(coop_uniform::Complete(coop_uniform::ReadBody(targetPlayer)));

    Reset();
    coop_uniform::OnChange(reinterpret_cast<void*>(manager), nullptr, targetPlayer, nullptr);
    assert(nativeCalls == 1 && receivedNull && !coop_uniform::stopped);

    Reset(); Put(manager, uint32_t(0));
    coop_uniform::OnChange(reinterpret_cast<void*>(manager), nullptr, targetPlayer, &remote);
    assert(nativeCalls == 1 && received == remote);

    Reset(); coop::ghost = 0;
    coop_uniform::OnChange(reinterpret_cast<void*>(manager), nullptr, donorPlayer, &remote);
    assert(nativeCalls == 1 && received == remote);

    Reset();
    coop_uniform::Donor donor{};
    assert(coop_uniform::FindDonor(manager, targetPlayer, donor));
    Put(targetPlayer + 0x50, uint32_t(0));
    coop_uniform::Repair(manager, targetPlayer, donor);
    assert(nativeCalls == 0);
    Put(targetPlayer + 0x50, donorPlayer + 0x54);
    Put(targetPlayer + 0x188, uint32_t(0));
    coop_uniform::Repair(manager, targetPlayer, donor);
    assert(nativeCalls == 0);
    Put(targetPlayer + 0x188, uint32_t(0x108000));
    coop_uniform::Repair(manager, targetPlayer, donor);
    assert(nativeCalls == 1 && received == local && !coop_uniform::stopped);
    coop_uniform::Repair(manager, targetPlayer, donor);
    assert(nativeCalls == 1);

    Reset(); stealDonor = true;
    coop_uniform::OnChange(reinterpret_cast<void*>(manager), nullptr, targetPlayer, &remote);
    assert(coop_uniform::stopped);

    Reset();
    const uint32_t component = 0x10A000, skin = 0x10B000, indices = 0x10C000;
    Put(component + 0x2C, targetPlayer + 0x10);
    Put(component + 0x1C, skin);
    Put(skin + 0x120, uint32_t(0));
    Put(skin + 0xBC, uint32_t(2));
    Put(skin + 0x11C, indices);
    Put(indices, uint32_t(0)); Put(indices + 4, uint32_t(130));
    Put(targetPlayer + 0x1E0, uint32_t(0x109200));
    Put(targetPlayer + 0x1E4, uint32_t(131));
    assert(!coop_uniform::PreparePose(component));
    Put(targetPlayer + 0x140, uint32_t(0x109000));
    Put(targetPlayer + 0x1688, uint32_t(0x109100));
    coop_uniform::MarkDetached(targetPlayer, true);
    coop::ghost = 0;
    assert(!coop_uniform::PreparePose(component));
    coop_uniform::MarkDetached(targetPlayer, false);
    coop::ghost = targetPlayer;
    assert(coop_uniform::PreparePose(component));
    uint32_t pose = 0;
    assert(Read(skin + 0x120, &pose, 4) && pose == 0x109200);
    // A stale non-null pose must not read another player's allocation.
    Put(skin + 0x120, uint32_t(0xDEADBEEF));
    assert(coop_uniform::PreparePose(component));
    assert(Read(skin + 0x120, &pose, 4) && pose == 0x109200);
    // Replacement of this player's buffer invalidates the previous binding.
    Put(targetPlayer + 0x1E0, uint32_t(0x10D000));
    assert(coop_uniform::PreparePose(component));
    assert(Read(skin + 0x120, &pose, 4) && pose == 0x10D000);
    coop_uniform::stopped = 1;
    Put(skin + 0x120, uint32_t(0xDEADBEEF));
    assert(!coop_uniform::PreparePose(component));
    assert(Read(skin + 0x120, &pose, 4) && pose == 0xDEADBEEF);
    coop_uniform::stopped = 0;
    Put(indices + 4, uint32_t(131));
    assert(!coop_uniform::PreparePose(component));
    Put(targetPlayer + 0x1E0, uint32_t(0));
    assert(!coop_uniform::PreparePose(component));
    Reset();
    assert(coop_uniform::FindDonor(manager, targetPlayer, donor));
    Put(donorPlayer + 0x1E0, uint32_t(0x10D000));
    Put(targetPlayer + 0x1E0, uint32_t(0x10D000));
    coop_uniform::CheckResult(manager, targetPlayer, donor);
    assert(coop_uniform::stopped);
    std::puts("Uniform routing, cleanup, donor validation, lifecycle guards and ownership checks passed.");
    FILE* output = nullptr;
    assert(tmpfile_s(&output) == 0 && output);
    strcpy_s(coop_uniform::messages[0], "pending pose event");
    coop_uniform::messageCount = 1;
    _lock_file(output);
    std::thread writer([&] { coop_uniform::WriteDiagnostics(output); });
    bool drained = false;
    for (unsigned i = 0; i < 1000 && !drained; ++i) {
        if (coop_uniform::logMutex.try_lock()) {
            drained = coop_uniform::messageCount == 0;
            coop_uniform::logMutex.unlock();
        }
        if (!drained) Sleep(1);
    }
    _unlock_file(output);
    writer.join();
    std::fclose(output);
    assert(drained);
    std::puts("A blocked diagnostic stream leaves the game-thread pose queue available.");
}
