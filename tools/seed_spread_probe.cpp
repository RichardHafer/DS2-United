// Offline probe: how much does the seed change what a run can replace?
//
// buildPlan picks exactly one target model per source and keeps only that
// model's donors. This measures how many models a source could have had, and
// how much the chosen set moves between seeds.
#include "../src/selection.h"
#include "../src/generated_enemy_catalog.h"
#include <cstdio>
#include <map>
#include <set>

int main(int argc, char** argv) {
    std::vector<ds2::EnemyRecord> pool;
    for (const auto& row : ds2::kEnemyCatalog) {
        ds2::EnemyRecord record{row.guid, row.model, row.family, row.context};
        record.ordinary = row.ordinary;
        record.ambiguous = row.ambiguous;
        record.arrivalKey = row.arrivalKey;
        record.crossArrivalKey = row.crossArrivalKey;
        record.scriptedKey = row.scriptedKey;
        pool.push_back(record);
    }

    ds2::PoolSettings cfg;
    cfg.enabled = true;
    cfg.chance = 100;
    cfg.crossFamily = argc > 1 && argv[1][0] == '1';
    cfg.ventVariants = true;
    cfg.crossFamilyVents = cfg.crossFamily;
    cfg.ventRebinding = true;

    // How many distinct models could each source have drawn from, ignoring the
    // seed's single pick? Recomputed here with the same gates buildPlan uses.
    std::map<size_t, size_t> choiceHistogram;
    size_t withChoice = 0, sources = 0;
    for (const auto& source : pool) {
        auto plan = ds2::buildPlan(cfg, source, pool, 8);
        if (plan.donors.empty()) continue;
        ++sources;
        std::set<ds2::Guid> models;
        for (const auto& candidate : pool) {
            const bool vent = cfg.ventVariants && ds2::ventContext(source.context) && !source.arrivalKey.empty();
            const bool arrival = vent ? ds2::compatibleVent(cfg, source, candidate)
                                      : ds2::directContext(candidate.context) && candidate.ordinary;
            if (candidate.targetEnabled && candidate.weight && arrival && !candidate.ambiguous &&
                candidate.model != ds2::Guid{} && candidate.model != source.model &&
                candidate.guid != source.guid &&
                (cfg.crossFamily || candidate.family == source.family))
                models.insert(candidate.model);
        }
        ++choiceHistogram[models.size()];
        if (models.size() > 1) ++withChoice;
    }

    std::printf("cross_family=%d  sources with a plan: %zu\n", cfg.crossFamily ? 1 : 0, sources);
    std::printf("sources whose target model the seed alone decides (>1 candidate): %zu (%.1f%%)\n",
                withChoice, sources ? 100.0 * withChoice / sources : 0.0);
    std::printf("candidate models per source:\n");
    for (const auto& entry : choiceHistogram)
        std::printf("   %2zu model(s): %4zu sources\n", entry.first, entry.second);

    // How deep is a plan now? Before the fallback change a plan carried donors
    // of exactly one model, so an absent model meant the enemy never changed.
    size_t totalDonors = 0, plansWithFallback = 0, modelsInPlans = 0;
    for (const auto& source : pool) {
        auto plan = ds2::buildPlan(cfg, source, pool, 8);
        if (plan.donors.empty()) continue;
        totalDonors += plan.donors.size();
        std::set<ds2::Guid> models;
        for (const size_t donor : plan.donors) models.insert(pool[donor].model);
        modelsInPlans += models.size();
        if (models.size() > 1) ++plansWithFallback;
    }
    std::printf("\naverage donors per plan: %.2f\n", sources ? double(totalDonors) / sources : 0.0);
    std::printf("average models per plan: %.2f\n", sources ? double(modelsInPlans) / sources : 0.0);
    std::printf("plans that carry a fallback model: %zu (%.1f%%)\n",
                plansWithFallback, sources ? 100.0 * plansWithFallback / sources : 0.0);

    // How far apart are two seeds? Compare the chosen model per source.
    std::printf("\nseed  planned  same model as seed 1\n");
    std::map<ds2::Guid, ds2::Guid> reference;
    for (uint32_t seed = 1; seed <= 10; ++seed) {
        cfg.seed = seed;
        size_t planned = 0, agree = 0;
        for (const auto& source : pool) {
            auto plan = ds2::buildPlan(cfg, source, pool, 8);
            if (plan.donors.empty()) continue;
            ++planned;
            if (seed == 1) reference[source.guid] = plan.model;
            else if (reference.count(source.guid) && reference[source.guid] == plan.model) ++agree;
        }
        if (seed == 1) std::printf("%4u  %7zu  (reference)\n", seed, planned);
        else std::printf("%4u  %7zu  %zu (%.1f%%)\n", seed, planned, agree,
                         planned ? 100.0 * agree / planned : 0.0);
    }
    return 0;
}
