// Dead Space 2 United by Richard Hafer.
#pragma once
#include <cmath>
#include <cstdint>

namespace survival_capsules {
constexpr uint32_t kMagic = 0x31504353; // SCP1
constexpr unsigned kMax = 20;

#pragma pack(push, 1)
struct Item { uint32_t id; float pos[3]; uint8_t golden; };
struct Snapshot {
    uint32_t magic = kMagic, token = 0, seq = 0, count = 0;
    Item items[kMax]{};
    unsigned Find(uint32_t id) const {
        for (unsigned i = 0; i < count; ++i) if (items[i].id == id) return i;
        return count;
    }
    void Apply(int kind, const Item& item) {
        if (kind == 6 && !item.id) { count = 0; return; }
        const unsigned i = Find(item.id);
        if (kind == 6) {
            if (i < count) { --count; items[i] = items[count]; }
        } else if (item.id && i < kMax) {
            if (i == count) ++count;
            items[i] = item;
        }
    }
    bool Valid() const {
        if (magic != kMagic || !token || !seq || count > kMax) return false;
        for (unsigned i = 0; i < count; ++i) {
            const auto& entry = items[i];
            if (!entry.id || entry.golden > 1 || Find(entry.id) != i) return false;
            for (const float v : entry.pos) if (!std::isfinite(v)) return false;
        }
        return true;
    }
};
#pragma pack(pop)
static_assert(sizeof(Snapshot) == 356, "capsule snapshot wire size");

struct Replica {
    Snapshot desired{}, offered{};
    unsigned cursor = 0;
    bool clearCopies = false;
    bool Receive(const Snapshot& packet) {
        if (!packet.Valid() || (desired.token == packet.token && static_cast<int32_t>(packet.seq - desired.seq) <= 0)) return false;
        if (desired.token && desired.token != packet.token) clearCopies = true;
        desired = packet;
        cursor = 0;
        return true;
    }
    bool Take(int& kind, Item& item) {
        if (!desired.seq) return false;
        if (clearCopies) { kind = 6; item = {}; offered.count = 0; clearCopies = false; return true; }
        for (unsigned i = 0; i < offered.count; ++i) {
            if (desired.Find(offered.items[i].id) < desired.count) continue;
            kind = 6;
            item = offered.items[i];
            offered.Apply(kind, item);
            return true;
        }
        if (cursor >= desired.count) return false;
        kind = 5;
        item = desired.items[cursor++];
        offered.Apply(kind, item);
        return true;
    }
};
}
