// Dead Space 2 United by Richard Hafer.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include "../src/coop_save_policy.h"

int main() {
    constexpr uint32_t player = 0x100000, proxy = 0x200000, vt = 0x1B294D0;
    std::map<uint32_t, uint32_t> memory{{player, vt}, {player + 0x68, proxy},
        {proxy + 0x30, 2}, {proxy + 0x40, player + 0x5C}};
    auto read = [&](uint32_t a, void* out, size_t bytes) {
        auto it = memory.find(a);
        if (it == memory.end() || bytes != 4) return false;
        std::memcpy(out, &it->second, 4);
        return true;
    };
    auto inspect = [&] { return coop_save::Inspect(player, vt, read).role; };
    using Role = coop_save::Role;
    assert(inspect() == Role::Ghost);
    memory[proxy + 0x30] = 8;
    assert(inspect() == Role::Local);
    memory[proxy + 0x30] = 2;
    memory[proxy + 0x40] = player + 0x100;
    assert(inspect() == Role::Unknown);
    memory.erase(proxy + 0x40);
    assert(inspect() == Role::Unknown);
    memory[player + 0x68] = 0;
    assert(inspect() == Role::Local);
    memory[player] = vt + 4;
    assert(inspect() == Role::Unknown);
    memory.erase(player);
    assert(inspect() == Role::Unknown);
    assert(coop_save::Inspect(0xFFFFFFF0, vt, read).role == Role::Unknown);
}
