// Dead Space 2 United by Richard Hafer.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Seed-stable permutation of bench upgrade node types.
//
// Retail derives a node's type at runtime from its name hash, through the pure
// function at 0x00B76990. Every consumer does this: the stat effect, the node
// counters and the UI. Nothing caches a type on the node. Redirecting that one
// function therefore moves what a node awards while the grid, the authored
// connections, the navigation keys and the save data all stay untouched.
//
// The type for each catalogued name is not decided here. The runtime asks the
// game for it and passes the answers in, so an offline mistake cannot become a
// permutation.
namespace ds2::bench {

enum : uint8_t { kNodeBlank = 0, kNodeTypeCount = 14 };
enum : uint8_t { kNodeHP = 7, kNodeAir = 8, kNodeEnergy = 9 };

// Two separate hazards, and they need different guards.
//
// A name used by um:BaseValue carries that tree's authored base for the stat.
// Only those exact names, three per tree or so, must never be remapped; the
// numbered siblings are free. Moving "HP" left the RIG with no health and the
// player died on load, while "HP2" and "HP3" were never the problem.
//
// Separately, health, air and stasis energy stay out of the global relabel, so
// a weapon never starts awarding air and the RIG never loses its own kinds.
constexpr bool vitalStat(uint8_t type) {
    return type == kNodeHP || type == kNodeAir || type == kNodeEnergy;
}

// A tree and a stat each fit in one bit, which is what makes "does this tree
// still award the same stats" a single comparison. Retail has 14 trees; a tree
// beyond the limit simply takes no part in the shuffle.
constexpr uint16_t kTreeLimit = 32;
constexpr uint32_t typeBit(uint8_t type) { return 1u << (type & 31u); }

inline uint64_t Next(uint64_t& state) {
    uint64_t value = (state += UINT64_C(0x9E3779B97F4A7C15));
    value = (value ^ (value >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31);
}

// Rejection sampling, so the result does not depend on a library RNG.
inline size_t Below(uint64_t& state, size_t bound) {
    const uint64_t limit = static_cast<uint64_t>(bound);
    const uint64_t threshold = (uint64_t{0} - limit) % limit;
    uint64_t value;
    do { value = Next(state); } while (value < threshold);
    return static_cast<size_t>(value % limit);
}

template<class T>
void Shuffle(uint64_t& state, T* items, size_t count) {
    for (size_t remaining = count; remaining > 1; --remaining) {
        const size_t selected = Below(state, remaining);
        const T saved = items[remaining - 1];
        items[remaining - 1] = items[selected];
        items[selected] = saved;
    }
}

struct Options {
    // Trade stats between node names, keeping only the trades that leave every
    // affected tree awarding exactly the stats it awarded before. A tree keeps
    // its authored shape and the stats it offers; which of its cells gives what
    // moves. Width stays on the Line Gun because no other tree has it.
    bool statShuffle = false;
    // Permute types among the nodes whose name belongs to exactly one tree.
    // Filler nodes take part, so a stat can land on a cell that used to be a
    // plain connector. Restricted to unique names because the game's mapping is
    // global by name: a shared name cannot differ between two trees.
    bool nodeShuffle = false;
    // Keep a real stat on start-point cells, so a tree never opens on a blank.
    bool protectStartPoints = true;
    // Leave health, air and stasis energy where they are. See vitalStat().
    bool preserveVitalStats = true;
};

enum class BuildResult { Disabled, NoNodes, Built };

// One entry per catalogued node identity.
struct NodeInput {
    uint16_t tree;
    uint32_t nameHash;
    bool uniqueToTree;
    bool startPoint;
    bool carriesBaseValue;
    uint8_t type;      // as reported by the game for nameHash
};

// Build the policy input from a catalog row and the type the game reported.
// Both the runtime and the tests must go through here.
template<class CatalogNode>
NodeInput makeInput(const CatalogNode& node, uint8_t type) {
    NodeInput input;
    input.tree = node.tree;
    input.nameHash = node.nameHash;
    input.uniqueToTree = node.uniqueToTree;
    input.startPoint = node.startPoint;
    input.carriesBaseValue = node.carriesBaseValue;
    input.type = type;
    return input;
}

template<size_t N>
class Policy {
public:
    BuildResult Build(const NodeInput (&nodes)[N], uint64_t seed, const Options& options) {
        count_ = 0;
        if (!options.statShuffle && !options.nodeShuffle) return BuildResult::Disabled;
        if constexpr (N == 0) return BuildResult::NoNodes;

        uint16_t highestTree = 0;
        for (size_t i = 0; i < N; ++i) if (nodes[i].tree > highestTree) highestTree = nodes[i].tree;

        for (size_t i = 0; i < N; ++i) {
            entries_[i].nameHash = nodes[i].nameHash;
            entries_[i].type = nodes[i].type;
        }
        count_ = N;

        // Stats trade places between names, and a trade is only kept when every
        // tree it touches still awards exactly the stats it awarded before.
        //
        // This used to relabel the types themselves: every Damage node in the
        // game became whatever Damage mapped to. That put stats where they do
        // nothing: Width exists only on the Line Gun, and a global relabel
        // could hand it to the stasis tree, which has no width to widen.
        //
        // Grouping the types by the trees they occur in does not help: no two
        // types are spread over the same trees, so nothing is ever
        // interchangeable and the shuffle collapses to a no-op. Names are the
        // right unit. The runtime resolves a node from its name alone, so
        // trading the types of two names is exactly as global as it has to be,
        // and one name reaches far fewer trees than one type does.
        //
        // Rather than derive which trades are safe, each one is tried and
        // measured: swap two names, rebuild the set of stats for every tree
        // either name appears in, and keep the swap only if all of them come out
        // unchanged. Equality in both directions, so a tree can neither pick up
        // a stat it never had nor lose one entirely. Only the counts move.
        // `Width2` may become Damage because the Line Gun has Damage, and
        // something else in the Line Gun becomes Width in exchange.
        //
        // Blank is not a stat and stays blank, or filler cells would start
        // awarding upgrades on their own. Base-value names and the vital stats
        // are held fixed for the reasons given at vitalStat(), though the check
        // above would refuse to move a vital stat out of its tree anyway.
        if (options.statShuffle) {
            // One name occurs in several trees, so collapse the rows onto the
            // first row that carries each name and work on those.
            std::array<size_t, N> owner{};
            std::array<uint32_t, N> reach{};
            std::array<bool, N> beyondReach{};
            std::array<bool, N> pinned{};
            for (size_t i = 0; i < N; ++i) {
                owner[i] = i;
                for (size_t j = 0; j < i; ++j)
                    if (nodes[j].nameHash == nodes[i].nameHash) { owner[i] = j; break; }
            }
            for (size_t i = 0; i < N; ++i) {
                const size_t name = owner[i];
                if (nodes[i].tree < kTreeLimit) reach[name] |= 1u << nodes[i].tree;
                else beyondReach[name] = true;
                if (nodes[i].carriesBaseValue) pinned[name] = true;
            }

            std::array<uint8_t, N> current{};
            for (size_t i = 0; i < N; ++i) current[i] = nodes[i].type;
            std::array<size_t, N> candidates{};
            size_t candidateCount = 0;
            for (size_t i = 0; i < N; ++i) {
                if (owner[i] != i || beyondReach[i] || !reach[i]) continue;
                const uint8_t type = nodes[i].type;
                if (type == kNodeBlank || type >= kNodeTypeCount) continue;
                if (options.preserveVitalStats && (pinned[i] || vitalStat(type))) continue;
                candidates[candidateCount++] = i;
            }

            if (candidateCount >= 2) {
                // The guarantee is the count of every stat per tree, not only
                // the set. Keeping just the set let a tree end up with six
                // Damage cells instead of four (New Game+ with every node
                // bought: 13.09., far above the authored maximum). With the
                // counts held, a fully upgraded weapon is exactly as strong
                // as retail; only where on the grid each stat sits moves.
                uint8_t authored[kTreeLimit][kNodeTypeCount]{};
                for (size_t i = 0; i < N; ++i)
                    if (nodes[i].tree < kTreeLimit)
                        ++authored[nodes[i].tree][nodes[i].type];

                uint64_t state = seed;
                // Enough attempts that every name is offered a partner many
                // times over; the refused ones cost nothing but a rebuild.
                const size_t attempts = candidateCount * 64;
                for (size_t attempt = 0; attempt < attempts; ++attempt) {
                    const size_t left = candidates[Below(state, candidateCount)];
                    const size_t right = candidates[Below(state, candidateCount)];
                    if (left == right || current[left] == current[right]) continue;
                    const uint8_t saved = current[left];
                    current[left] = current[right];
                    current[right] = saved;

                    const uint32_t touched = reach[left] | reach[right];
                    uint8_t rebuilt[kTreeLimit][kNodeTypeCount]{};
                    for (size_t i = 0; i < N; ++i) {
                        const uint16_t tree = nodes[i].tree;
                        if (tree >= kTreeLimit || !((touched >> tree) & 1u)) continue;
                        ++rebuilt[tree][current[owner[i]]];
                    }
                    bool safe = true;
                    for (uint16_t tree = 0; tree < kTreeLimit && safe; ++tree) {
                        if (!((touched >> tree) & 1u)) continue;
                        for (uint8_t type = 0; type < kNodeTypeCount && safe; ++type)
                            safe = rebuilt[tree][type] == authored[tree][type];
                    }
                    if (!safe) {
                        current[right] = current[left];
                        current[left] = saved;
                    }
                }
            }
            for (size_t i = 0; i < N; ++i) entries_[i].type = current[owner[i]];
        }

        if (options.nodeShuffle) {
            for (uint16_t tree = 0; tree <= highestTree; ++tree) {
                std::array<size_t, N> slots{};
                size_t slotCount = 0;
                for (size_t i = 0; i < N; ++i) {
                    if (nodes[i].tree != tree) continue;
                    if (!nodes[i].uniqueToTree) continue;
                    // A protected start point keeps whatever stat it now has.
                    if (options.protectStartPoints && nodes[i].startPoint &&
                        entries_[i].type != kNodeBlank) continue;
                    // A base-value name must not move: it is where the tree's
                    // authored base for that stat is registered.
                    if (options.preserveVitalStats && nodes[i].carriesBaseValue) continue;
                    slots[slotCount++] = i;
                }
                if (slotCount < 2) continue;
                std::array<uint8_t, N> types{};
                for (size_t i = 0; i < slotCount; ++i) types[i] = entries_[slots[i]].type;
                Shuffle(seed, types.data(), slotCount);
                for (size_t i = 0; i < slotCount; ++i) entries_[slots[i]].type = types[i];
            }
        }
        return BuildResult::Built;
    }

    // Fail closed: an unknown hash keeps the game's own answer.
    bool Lookup(uint32_t nameHash, uint8_t& type) const {
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].nameHash != nameHash) continue;
            type = entries_[i].type;
            return true;
        }
        return false;
    }

    size_t Count() const { return count_; }

private:
    struct Entry { uint32_t nameHash; uint8_t type; };
    std::array<Entry, N> entries_{};
    size_t count_ = 0;
};
}  // namespace ds2::bench
