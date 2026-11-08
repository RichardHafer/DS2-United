// Dead Space 2 United by Hafer.
namespace npc_policy {
enum class Classification { Other, NPC, Unknown };
Classification Classify(uint32_t descriptor) {
    uint32_t pending[64]{descriptor}, seen[64]{};
    unsigned count = 1, visited = 0;
    while (count) {
        const uint32_t current = pending[--count];
        bool duplicate = false;
        for (unsigned i = 0; i < visited; ++i) if (seen[i] == current) duplicate = true;
        if (duplicate) continue;
        if (visited == std::size(seen)) return Classification::Unknown;
        seen[visited++] = current;
        uint32_t vt = 0, type = 0, parents = 0;
        unsigned char size = 0;
        if (!current || !Read(current, &vt, 4) || vt != base + 0x171E648 ||
            !Read(current + 8, &type, 4)) return Classification::Unknown;
        if (type == 0x71BC20FF) return Classification::NPC;
        if (!Read(current + 0x49, &size, 1) || size > 16 || count + size > std::size(pending)) return Classification::Unknown;
        if (size && (!Read(current + 0x1C, &parents, 4) || !parents ||
                     !Read(parents, pending + count, size * 4))) return Classification::Unknown;
        count += size;
    }
    return Classification::Other;
}
} // namespace npc_policy
