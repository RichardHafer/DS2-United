// Dead Space 2 United by Hafer.
// Which upgrade node a companion bot buys next, without the game.
//
// One tree is a grid of nodes linked to their four neighbours. A node can be
// bought when it is empty and either a start point or next to a filled node
// (PlayerUpgradeMainSM::CanReach);
// every node costs one power node, blank fillers included. The bot goes for the
// stats that help it most, and buys a blank only on the way to one.
#pragma once
#include <cstdint>

namespace coop_bot {

constexpr int kMaxBenchNodes = 48;

struct BenchNode {
    uint32_t key = 0;
    bool filled = false, start = false;
    int type = 0;              // EARS::Modules::UpgradeNodeType, 0 = blank
    uint32_t links[4]{};       // neighbour keys, 0 = none
};

// How much a stat is worth to a bot. Blank (0) is worth nothing by itself.
inline float BenchWorth(int type) {
    switch (type) {
    case 1: return 6.0f;    // damage
    case 3: return 5.0f;    // capacity
    case 5: return 4.0f;    // reload
    case 2: return 4.0f;    // speed
    case 6: return 3.0f;    // alt damage
    case 4: return 3.0f;    // width
    case 7: return 3.0f;    // HP
    case 13: return 3.0f;   // special
    case 9: case 10: case 11: case 12: return 2.0f;   // energy, duration, range, charge
    case 8: return 1.0f;    // air
    default: return 0.0f;
    }
}

inline int BenchFind(const BenchNode* nodes, int count, uint32_t key) {
    if (!key) return -1;
    for (int i = 0; i < count; ++i) if (nodes[i].key == key) return i;
    return -1;
}

inline bool BenchReachable(const BenchNode* nodes, int count, int i) {
    if (nodes[i].filled) return false;
    if (nodes[i].start) return true;
    for (uint32_t link : nodes[i].links) {
        const int n = BenchFind(nodes, count, link);
        if (n >= 0 && nodes[n].filled) return true;
    }
    return false;
}

// The node to buy next, or -1: of the nodes that can be bought now, the one
// whose best stat (itself, or one reachable over empty nodes behind it) is
// worth most per node spent on the way.
inline int PickBenchNode(const BenchNode* nodes, int count) {
    if (count > kMaxBenchNodes) count = kMaxBenchNodes;
    int best = -1;
    float bestScore = 0.0f;
    for (int i = 0; i < count; ++i) {
        if (!BenchReachable(nodes, count, i)) continue;
        // Breadth first over empty nodes: distance from this candidate.
        int dist[kMaxBenchNodes];
        for (int k = 0; k < count; ++k) dist[k] = -1;
        int queue[kMaxBenchNodes], head = 0, tail = 0;
        dist[i] = 0;
        queue[tail++] = i;
        float score = 0.0f;
        while (head < tail) {
            const int at = queue[head++];
            const float worth = BenchWorth(nodes[at].type) / static_cast<float>(1 + dist[at]);
            if (worth > score) score = worth;
            for (uint32_t link : nodes[at].links) {
                const int n = BenchFind(nodes, count, link);
                if (n < 0 || nodes[n].filled || dist[n] >= 0) continue;
                dist[n] = dist[at] + 1;
                queue[tail++] = n;
            }
        }
        if (score > bestScore) { bestScore = score; best = i; }
    }
    return best;
}

}  // namespace coop_bot
