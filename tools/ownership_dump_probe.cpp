// Run the plugin's read-only ownership report against extracted dump ranges.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>
#include "../src/coop_save_policy.h"

uintptr_t base = 0x400000;
std::map<uint32_t, std::vector<unsigned char>> ranges;
bool Read(uintptr_t address, void* out, size_t bytes) {
    auto it = ranges.upper_bound(static_cast<uint32_t>(address));
    if (it == ranges.begin()) return false;
    --it;
    const size_t offset = address - it->first;
    if (offset > it->second.size() || bytes > it->second.size() - offset) return false;
    std::memcpy(out, it->second.data() + offset, bytes);
    return true;
}
bool WriteBytes(uintptr_t, const void*, size_t) { return false; }
namespace placement { constexpr uint32_t kPlayerPrimaryVtableRva = 0x17294D0; }
namespace coop {
#include "../src/runtime_coop_state.inc"
#include "../src/runtime_coop_ownership.inc"
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 3;
    for (;;) {
        uint32_t address = 0, size = 0;
        input.read(reinterpret_cast<char*>(&address), 4);
        if (input.eof() && input.gcount() == 0) break;
        if (!input.read(reinterpret_cast<char*>(&size), 4) || !size || size > 65536) return 4;
        auto& data = ranges[address];
        data.resize(size);
        if (!input.read(reinterpret_cast<char*>(data.data()), size)) return 5;
    }
    coop::ownership::Capture();
    std::puts(coop::ownership::report);
    for (const auto& entry : ranges) {
        if (entry.second.size() < 0x1000) continue;
        const auto evidence = coop_save::Inspect(entry.first, 0x1B294D0, Read);
        std::printf("save_policy player=%08X role=%d\n", entry.first,
                    static_cast<int>(evidence.role));
    }
}
