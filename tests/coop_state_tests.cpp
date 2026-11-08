// Dead Space 2 United by Richard Hafer.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>

uintptr_t base = 0x400000;
std::map<uintptr_t, unsigned char> memory;
bool denyWrites = false;
bool Read(uintptr_t address, void* out, size_t bytes) {
    auto dst = static_cast<unsigned char*>(out);
    for (size_t i = 0; i < bytes; ++i) {
        const auto found = memory.find(address + i);
        if (found == memory.end()) return false;
        dst[i] = found->second;
    }
    return true;
}
bool WriteBytes(uintptr_t address, const void* value, size_t size) {
    if (denyWrites) return false;
    auto bytes = static_cast<const unsigned char*>(value);
    for (size_t i = 0; i < size; ++i) memory[address + i] = bytes[i];
    return true;
}
void Put(uintptr_t address, uint32_t value) { assert(WriteBytes(address, &value, 4)); }
namespace placement { constexpr uint32_t kPlayerPrimaryVtableRva = 0x17294D0; }
namespace coop {
#include "../src/runtime_coop_state.inc"
}

int main() {
    using namespace coop;
    const uint32_t pm = 0x100000, slots = 0x110000, local = 0x200000, ghost = 0x300000;
    const uint32_t bpVt = 0x500000, proxy = 0x600000;
    assert(!LocalCampaignPlayer());
    Put(base + kPlayerManagerRva, pm); Put(pm + 4, slots); Put(pm + 8, 8);
    Put(slots, local + 0x3C0); Put(slots + 4, ghost + 0x3C0);
    Put(local + 0x3C0, bpVt); Put(bpVt + 12, static_cast<uint32_t>(base + 0x4A7960));
    Put(local, static_cast<uint32_t>(base + placement::kPlayerPrimaryVtableRva)); Put(local + 0x68, 0);
    assert(LocalCampaignPlayer() == local);
    Put(local + 0x68, proxy); Put(proxy + 0x30, 2);
    assert(!LocalCampaignPlayer());
    Put(proxy + 0x30, 8); assert(LocalCampaignPlayer() == local);
    Put(slots, 0); assert(!LocalCampaignPlayer());
    Put(slots, ghost + 0x3C0); Put(ghost + 0x3C0, bpVt);
    Put(ghost, static_cast<uint32_t>(base + placement::kPlayerPrimaryVtableRva)); Put(ghost + 0x68, 0);
    assert(LocalCampaignPlayer() == ghost);
    Put(pm + 8, 0xFFFFFFFF); assert(!LocalCampaignPlayer());
    Put(pm + 8, 8); Put(bpVt + 12, 0); assert(!LocalCampaignPlayer());

    const uint32_t session = 0x700000, iface = 0x710000, array = 0x720000, first = 0x730000, second = 0x740000;
    assert(!HasConnectedPeer(session));
    Put(session + 0x1C, iface); Put(iface + 0xC, 2); Put(iface + 0x14, array);
    Put(array, first); Put(array + 4, second); Put(first + 0x1B0, 5); Put(second + 0x1B0, 7);
    assert(HasConnectedPeer(session));
    Put(second + 0x1B0, 5); assert(!HasConnectedPeer(session));
    Put(first + 0x1B0, 7); assert(HasConnectedPeer(session));
    Put(iface + 0xC, 17); assert(!HasConnectedPeer(session));

    uint32_t key = 0;
    Put(session + 0x18, 12);
    assert(AllocatePlayerSubKey(session, 1, key) && key == 0x0100000D);
    assert(AllocatePlayerSubKey(session, 1, key) && key == 0x0100000E);
    assert(!AllocatePlayerSubKey(session, 256, key));
    denyWrites = true; assert(!AllocatePlayerSubKey(session, 1, key)); denyWrites = false;
    Put(session + 0x18, 0xFFFFFF); assert(!AllocatePlayerSubKey(session, 1, key));

    struct Guarded { char text[12]; char guard[4]; } buffer{};
    std::memset(buffer.guard, 'G', sizeof(buffer.guard));
    size_t used = 0;
    AppendReport(buffer.text, sizeof(buffer.text), used, "%s", "abcdefghijklmnopqrstuvwxyz");
    assert(used == 11 && buffer.text[11] == 0);
    AppendReport(buffer.text, sizeof(buffer.text), used, "%u", 123U);
    assert(std::memcmp(buffer.guard, "GGGG", 4) == 0);
    used = 0; AppendReport(buffer.text, sizeof(buffer.text), used, "%s", "abc");
    AppendReport(buffer.text, sizeof(buffer.text), used, "%u", 42U);
    assert(used == 5 && std::strcmp(buffer.text, "abc42") == 0);
    std::puts("coop state tests passed");
}
