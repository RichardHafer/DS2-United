// Dead Space 2 United by Richard Hafer.
#pragma once
constexpr uint32_t kWorldMagic = 0x474D4457; // WDMG
constexpr uint32_t kWorldVersion = 1;
constexpr unsigned kNetDamageSize = 0xA8;
struct WorldPacket {
    uint32_t magic, version, token, sequence;
    uint32_t guid[4];
    unsigned char net[kNetDamageSize];
};
static_assert(sizeof(WorldPacket) == 200);
