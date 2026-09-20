#include "../src/policy.h"
#include <cassert>
#include <cstdio>
int main() {
    using namespace ds2;
    Settings cfg;
    Spawn spawn{{1,2,3,4}, {5,6,7,8}, 1, 1, true, false};
    std::vector<Candidate> candidates{
        {{9,10,11,12}, 1, 1, true, true, true, true},
        {{13,14,15,16}, 1, 1, true, true, true, true}};
    Diagnostics stats;
    assert(decide(cfg, spawn, candidates, stats).outcome == Outcome::Disabled);
    cfg.enabled = true;
    auto chosen = decide(cfg, spawn, candidates, stats).target;
    std::reverse(candidates.begin(), candidates.end());
    candidates.push_back(candidates.front());
    assert(decide(cfg, spawn, candidates, stats).target == chosen);
    cfg.chance = 0;
    assert(decide(cfg, spawn, candidates, stats).outcome == Outcome::ChanceSkipped);
    cfg.chance = 100;
    spawn.lifecycleVerified = false;
    assert(decide(cfg, spawn, candidates, stats).outcome == Outcome::UnverifiedSource);
    spawn.lifecycleVerified = true;
    spawn.protectedEncounter = true;
    assert(decide(cfg, spawn, candidates, stats).outcome == Outcome::Protected);
    spawn.protectedEncounter = false;
    for (auto& candidate : candidates) candidate.resident = false;
    assert(decide(cfg, spawn, candidates, stats).outcome == Outcome::NoCandidate);
    assert(stats.notResident == 3);
    for (auto& candidate : candidates) {
        candidate.resident = true;
        candidate.compatibilityVerified = false;
    }
    assert(decide(cfg, spawn, candidates, stats).outcome == Outcome::NoCandidate);
    for (auto& candidate : candidates) candidate.compatibilityVerified = true;
    cfg.spawnsPerTrigger = 32;
    assert(decide(cfg, spawn, candidates, stats).totalSpawns == 16);
    assert(decide(cfg, spawn, candidates, stats, 3).totalSpawns == 3);
    assert(spawnCount(0) == 1 && spawnCount(5, 0) == 1);
    assert(spawnCount(5, -1) == 5 && spawnCount(5, 99) == 16);
    assert(stableHash(0x00D5A11u, spawn.source, spawn.authoredTarget,
                      0x43484E43u) == 0x63B00C9Du);
    for (auto& candidate : candidates) candidate.selected = false;
    assert(decide(cfg, spawn, candidates, stats).outcome == Outcome::NoCandidate);
    assert(stats.filtered == 3);
    for (auto& candidate : candidates) {
        candidate.selected = true;
        candidate.donorValidated = false;
    }
    assert(decide(cfg, spawn, candidates, stats).outcome == Outcome::NoCandidate);
    assert(stats.invalidDonor == 3);
    for (auto& candidate : candidates) {
        candidate.donorValidated = true;
        candidate.target = spawn.authoredTarget;
    }
    assert(decide(cfg, spawn, candidates, stats).outcome == Outcome::NoCandidate);
    assert(stats.authored == 3);
    spawn.context = 0;
    assert(decide(cfg, spawn, candidates, stats).outcome == Outcome::NoCandidate);
    std::printf("Policy tests passed; DS1 hash vector: %08X\n",
        stableHash(0x00D5A11u, spawn.source, spawn.authoredTarget, 0x43484E43u));
}
#include "../src/native_layout.h"
