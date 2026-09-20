#include "../src/bench_policy.h"
#include "../src/generated_bench_catalog.h"
#include <cassert>
#include <iostream>
#include <map>
#include <set>

using namespace ds2::bench;

constexpr size_t kCount = sizeof(kBenchNodes) / sizeof(kBenchNodes[0]);
static NodeInput inputs[kCount];

// The game supplies real types at install time. Here a deterministic stand-in
// exercises the permutation: some nodes blank, the rest spread over the stats.
static void BuildInputs() {
    for (size_t i = 0; i < kCount; ++i) {
        const auto& node = kBenchNodes[i];
        const uint8_t type = (node.nameHash % 3u == 0u)
            ? kNodeBlank
            : static_cast<uint8_t>(1u + node.nameHash % (kNodeTypeCount - 1));
        inputs[i] = makeInput(node, type);
    }
}

static std::map<uint32_t, uint8_t> Collect(const Policy<kCount>& policy) {
    std::map<uint32_t, uint8_t> out;
    for (size_t i = 0; i < kCount; ++i) {
        uint8_t type = 0xFF;
        const bool known = policy.Lookup(kBenchNodes[i].nameHash, type);
        assert(known);
        // Single valued: the game's mapping is global by name, so two trees
        // must never disagree about the same hash.
        const auto found = out.find(kBenchNodes[i].nameHash);
        if (found != out.end()) assert(found->second == type);
        out[kBenchNodes[i].nameHash] = type;
    }
    return out;
}

static std::multiset<uint8_t> TreeTypes(const std::map<uint32_t, uint8_t>& map, uint16_t tree) {
    std::multiset<uint8_t> out;
    for (size_t i = 0; i < kCount; ++i)
        if (kBenchNodes[i].tree == tree) out.insert(map.at(kBenchNodes[i].nameHash));
    return out;
}

int main() {
    BuildInputs();
    const uint16_t trees = sizeof(kTrees) / sizeof(kTrees[0]);

    Policy<kCount> policy;
    assert(policy.Build(inputs, 1, Options{}) == BuildResult::Disabled);
    uint8_t ignored = 0;
    assert(!policy.Lookup(kBenchNodes[0].nameHash, ignored));
    assert(policy.Count() == 0);

    // Untouched baseline for comparison.
    std::map<uint32_t, uint8_t> original;
    for (size_t i = 0; i < kCount; ++i) original[kBenchNodes[i].nameHash] = inputs[i].type;

    // Stat shuffle permutes a tree's own stats and never brings in a foreign one.
    //
    // It used to relabel across the whole bench, which put stats where they do
    // nothing: Width exists only on the Line Gun, and a global map could hand it
    // to the stasis tree, where nothing uses it.
    Options stat; stat.statShuffle = true;
    assert(policy.Build(inputs, 12345, stat) == BuildResult::Built);
    const auto relabelled = Collect(policy);
    for (size_t i = 0; i < kCount; ++i) {
        const uint8_t before = inputs[i].type;
        const uint8_t after = relabelled.at(kBenchNodes[i].nameHash);
        // A base-value name is where the tree registers its authored base, so
        // it stays put; a filler cell stays a filler cell either way.
        if (kBenchNodes[i].carriesBaseValue) assert(after == before);
        if (before == kNodeBlank) assert(after == kNodeBlank);
        else assert(after != kNodeBlank);
    }
    // The point of the change: every tree awards exactly the stats it awarded
    // before, no more and no less. Width cannot turn up on stasis any more, and
    // the Line Gun cannot lose it either. Only the counts move.
    for (uint16_t tree = 0; tree < trees; ++tree)
        assert(TreeTypes(relabelled, tree) == TreeTypes(original, tree));
    // Something actually traded, or the safety check would pass on a no-op.
    size_t tradedNames = 0;
    for (const auto& entry : original) tradedNames += relabelled.at(entry.first) != entry.second;
    assert(tradedNames > 0);
    // And safety holds for every seed, not just the one that was looked at.
    for (uint64_t trial = 1; trial <= 32; ++trial) {
        assert(policy.Build(inputs, trial, stat) == BuildResult::Built);
        const auto attempt = Collect(policy);
        for (uint16_t tree = 0; tree < trees; ++tree)
            assert(TreeTypes(attempt, tree) == TreeTypes(original, tree));
    }
    assert(policy.Build(inputs, 12345, stat) == BuildResult::Built);

    // Node shuffle moves types between cells of one tree and preserves that
    // tree's multiset, so nothing is created or destroyed.
    Options node; node.nodeShuffle = true;
    assert(policy.Build(inputs, 777, node) == BuildResult::Built);
    const auto moved = Collect(policy);
    for (uint16_t tree = 0; tree < trees; ++tree)
        assert(TreeTypes(moved, tree) == TreeTypes(original, tree));
    // Shared names are never moved: the mapping is global, so a shared name
    // cannot differ between trees.
    for (size_t i = 0; i < kCount; ++i)
        if (!kBenchNodes[i].uniqueToTree)
            assert(moved.at(kBenchNodes[i].nameHash) == original.at(kBenchNodes[i].nameHash));
    // A protected start point never ends up blank if it did not start blank.
    for (size_t i = 0; i < kCount; ++i)
        if (kBenchNodes[i].startPoint && inputs[i].type != kNodeBlank)
            assert(moved.at(kBenchNodes[i].nameHash) != kNodeBlank);

    // Something actually moved, or the test would pass on a no-op.
    size_t changed = 0;
    for (const auto& entry : original) changed += moved.at(entry.first) != entry.second;
    assert(changed > 0);

    // Both dials together. The stat shuffle keeps each tree's set of stats and
    // the node shuffle keeps each tree's multiset, so the combination still
    // neither brings a foreign stat into a tree nor empties one out of it.
    Options both; both.statShuffle = true; both.nodeShuffle = true;
    assert(policy.Build(inputs, 99, both) == BuildResult::Built);
    const auto combined = Collect(policy);
    for (uint16_t tree = 0; tree < trees; ++tree)
        assert(TreeTypes(combined, tree) == TreeTypes(original, tree));

    // Seed stability and seed sensitivity.
    Policy<kCount> again;
    assert(again.Build(inputs, 777, node) == BuildResult::Built);
    assert(Collect(again) == moved);
    size_t distinct = 0;
    std::set<std::map<uint32_t, uint8_t>> results;
    for (uint64_t seed = 1; seed <= 64; ++seed) {
        Policy<kCount> run;
        assert(run.Build(inputs, seed, both) == BuildResult::Built);
        if (results.insert(Collect(run)).second) ++distinct;
    }
    assert(distinct > 32);

    // An unknown name keeps the game's own answer.
    Policy<kCount> ready;
    assert(ready.Build(inputs, 5, both) == BuildResult::Built);
    uint8_t unused = 0;
    assert(!ready.Lookup(0xDEADBEEF, unused));

    // A base-value name never moves, whatever the dials say: it is where the
    // tree's authored base for that stat is registered. Everything else may.
    for (const auto& opts : {stat, node, both}) {
        Policy<kCount> vital;
        assert(vital.Build(inputs, 4242, opts) == BuildResult::Built);
        size_t movedFree = 0;
        for (size_t i = 0; i < kCount; ++i) {
            uint8_t after = 0;
            assert(vital.Lookup(kBenchNodes[i].nameHash, after));
            const uint8_t before = inputs[i].type;
            if (kBenchNodes[i].carriesBaseValue) { assert(after == before); continue; }
            movedFree += after != before;
        }
        assert(movedFree > 0);
        // A vital kind may move inside the tree that owns it, which is what
        // makes the RIG randomisable, but it must never appear in a tree that
        // had none: no weapon starts awarding air.
        for (uint16_t tree = 0; tree < trees; ++tree) {
            std::set<uint8_t> beforeVital, afterVital;
            for (size_t i = 0; i < kCount; ++i) {
                if (kBenchNodes[i].tree != tree) continue;
                uint8_t after = 0;
                assert(vital.Lookup(kBenchNodes[i].nameHash, after));
                if (vitalStat(inputs[i].type)) beforeVital.insert(inputs[i].type);
                if (vitalStat(after)) afterVital.insert(after);
            }
            for (const uint8_t kind : afterVital) assert(beforeVital.count(kind));
        }
    }

    std::cout << "Bench policy: " << kCount << " nodes over " << trees
              << " trees, every tree keeps the stats it awards, "
              << distinct << "/64 seeds distinct\n";
}
