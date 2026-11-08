// Dead Space 2 United by Hafer.
#pragma once
#include <cstdint>
#include <cstring>

namespace coop_panel {
struct Claim {
    uint32_t guid[4]{}, slot = 0, at = 0, token = 0, sequence = 0;

    bool Update(uint32_t sender, uint32_t serial, unsigned from, const uint32_t* target, bool held, uint32_t stamp) {
        const uint32_t advance = serial - sequence;
        if (token == sender && (!advance || advance >= 0x80000000u)) return false;
        token = sender;
        sequence = serial;
        slot = from;
        at = held ? stamp : 0;
        std::memcpy(guid, target, sizeof(guid));
        return true;
    }
};
} // namespace coop_panel
