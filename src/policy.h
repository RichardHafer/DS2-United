// Dead Space 2 United by Hafer.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace ds2 {
using Guid = std::array<uint32_t, 4>;
inline uint32_t mix(uint32_t hash, uint32_t value) {
    hash ^= value;
    hash *= 0x85EBCA6Bu;
    return hash ^ (hash >> 13);
}
inline uint32_t stableHash(uint32_t seed, const Guid& source,
                           const Guid& target, uint32_t salt) {
    uint32_t hash = mix(0x9E3779B9u, seed ^ salt);
    for (auto word : source) hash = mix(hash, word);
    for (auto word : target) hash = mix(hash, word);
    hash ^= hash >> 16;
    hash *= 0x7FEB352Du;
    return hash ^ (hash >> 15);
}
struct Settings {
    bool enabled = false;
    uint32_t seed = 0x00D5A11u;
    unsigned chance = 100;
    unsigned spawnsPerTrigger = 1;
};
struct Candidate {
    Guid target{};
    uint32_t family = 0;
    uint32_t context = 0;
    bool selected = true;
    bool resident = false;
    bool donorValidated = false;
    bool compatibilityVerified = false;
};
struct Spawn {
    Guid source{}, authoredTarget{};
    uint32_t family = 0, context = 0;
    bool lifecycleVerified = false;
    bool protectedEncounter = true;
};
enum class Outcome { Disabled, UnverifiedSource, Protected, ChanceSkipped,
                     NoCandidate, Selected };
struct Diagnostics {
    uint64_t calls = 0, disabled = 0, unverified = 0, protectedSource = 0;
    uint64_t chanceSkipped = 0, noCandidate = 0, selected = 0;
    uint64_t filtered = 0, notResident = 0, invalidDonor = 0;
    uint64_t incompatible = 0, authored = 0;
};
struct Decision { Outcome outcome; Guid target; unsigned totalSpawns = 1; };
inline unsigned spawnCount(unsigned global, int overrideCount = -1) {
    const unsigned count = overrideCount < 0 ? global : unsigned(overrideCount);
    return std::clamp(count, 1u, 16u);
}
// Runtime adapters must supply verified, current-lifecycle facts, never PDB guesses.
inline Decision decide(const Settings& cfg, const Spawn& spawn,
                       const std::vector<Candidate>& candidates,
                       Diagnostics& stats, int perEnemyCount = -1) {
    ++stats.calls;
    auto unchanged = [&](Outcome reason) {
        return Decision{reason, spawn.authoredTarget, 1};
    };
    if (!cfg.enabled) { ++stats.disabled; return unchanged(Outcome::Disabled); }
    if (!spawn.lifecycleVerified) {
        ++stats.unverified; return unchanged(Outcome::UnverifiedSource);
    }
    if (spawn.protectedEncounter) {
        ++stats.protectedSource; return unchanged(Outcome::Protected);
    }
    if (stableHash(cfg.seed, spawn.source, spawn.authoredTarget, 0x43484E43u)
            % 100u >= std::min(cfg.chance, 100u)) {
        ++stats.chanceSkipped; return unchanged(Outcome::ChanceSkipped);
    }
    std::vector<Guid> eligible;
    for (const auto& candidate : candidates) {
        if (!candidate.selected) { ++stats.filtered; continue; }
        if (!candidate.resident) { ++stats.notResident; continue; }
        if (!candidate.donorValidated) { ++stats.invalidDonor; continue; }
        if (!candidate.compatibilityVerified || !spawn.context ||
            candidate.context != spawn.context || candidate.family != spawn.family) {
            ++stats.incompatible; continue;
        }
        if (candidate.target == Guid{} || candidate.target == spawn.authoredTarget) {
            ++stats.authored; continue;
        }
        eligible.push_back(candidate.target);
    }
    std::sort(eligible.begin(), eligible.end());
    eligible.erase(std::unique(eligible.begin(), eligible.end()), eligible.end());
    if (eligible.empty()) { ++stats.noCandidate; return unchanged(Outcome::NoCandidate); }
    const auto index = stableHash(cfg.seed, spawn.source, spawn.authoredTarget,
                                  0x4C4F434Cu) % eligible.size();
    ++stats.selected;
    return {Outcome::Selected, eligible[index], spawnCount(cfg.spawnsPerTrigger, perEnemyCount)};
}
}
