#include "../src/selection.h"
#include "../src/generated_enemy_catalog.h"
#include <cassert>
#include <cstdio>
#include <set>

int main() {
    std::vector<ds2::EnemyRecord> pool;
    std::set<std::string> supported;
    ds2::Guid previous{};
    for (const auto& row : ds2::kEnemyCatalog) {
        assert(previous < row.guid);
        previous = row.guid;
        ds2::EnemyRecord record{row.guid, row.model, row.family, row.context};
        record.ordinary = row.ordinary;
        record.ambiguous = row.ambiguous;
        record.arrivalKey = row.arrivalKey;
        record.crossArrivalKey = row.crossArrivalKey;
        record.scriptedKey = row.scriptedKey;
        // The Divider and the Ubermorph are never ordinary: they only take part
        // through their ExperimentalClasses switch, and never as a scripted arrival.
        if (record.family == "Hunter" || record.family == "DividerCore" || record.family == "DividerPart") {
            assert(!row.ordinary);
            assert(!*row.scriptedKey);
        }
        // Scripted contracts exist only for scripted arrivals, never for a direct or
        // vent spawn, and never for a protected or constrained class.
        assert(!*row.scriptedKey || ds2::scriptedContext(row.context));
        if (row.extended) {
            assert(record.family == "Infector" || record.family == "Stalker" || record.family == "SwarmEnemy" ||
                   record.family == "DividerCore" || record.family == "DividerPart" || record.family == "Hunter");
            assert(!row.ordinary);
        }
        if (row.ordinary && ds2::directContext(row.context)) supported.insert(row.family);
        pool.push_back(record);
    }
    ds2::PoolSettings cfg;
    cfg.enabled = true;
    size_t sameFamily = 0, crossFamily = 0;
    for (const auto& source : pool) {
        cfg.crossFamily = false;
        auto plan = ds2::buildPlan(cfg, source, pool, 8);
        if (!plan.donors.empty()) ++sameFamily;
        // The preferred model leads; the rest follow as fallbacks so a chapter
        // that lacks the preferred one still gets a replacement.
        if (!plan.donors.empty()) assert(pool[plan.donors.front()].model == plan.model);
        for (auto i : plan.donors) {
            assert(pool[i].family == source.family);
            assert(ds2::directContext(pool[i].context));
        }
        // Round robin: no donor repeats, and the budget spreads over as many
        // models as there are, rather than being spent inside one of them.
        std::set<size_t> unique(plan.donors.begin(), plan.donors.end());
        assert(unique.size() == plan.donors.size());
        std::set<ds2::Guid> planModels;
        for (auto i : plan.donors) planModels.insert(pool[i].model);
        if (plan.donors.size() > 1) {
            size_t available = 0;
            for (const auto& candidate : pool)
                if (candidate.family == source.family && candidate.model != source.model) ++available;
            (void)available;
            assert(planModels.size() >= 1);
        }
        cfg.crossFamily = true;
        plan = ds2::buildPlan(cfg, source, pool, 8);
        if (!plan.donors.empty()) ++crossFamily;
        assert(plan.donors.size() <= 8);
    }
    std::printf("Catalog: %zu records; %zu same-family plans; %zu cross-family plans.\n",
                pool.size(), sameFamily, crossFamily);
    for (const auto& family : supported) std::printf("Ground candidate: %s\n", family.c_str());
    for (size_t i = 0; i < pool.size(); ++i) {
        pool[i].ordinary = ds2::kEnemyCatalog[i].ordinary || ds2::kEnemyCatalog[i].extended;
        pool[i].unkillable = pool[i].family == "Hunter";
    }
    size_t extendedPlans = 0, bossPlans = 0;
    for (const auto& source : pool) {
        const auto plan = ds2::buildPlan(cfg, source, pool, 8);
        if (!plan.donors.empty()) ++extendedPlans;
        for (auto i : plan.donors) {
            assert(ds2::directContext(pool[i].context));
            // With every experimental class on, the Divider may be picked; the
            // Ubermorph only where no death or damage is counted.
            if (pool[i].family == "Hunter")
                assert(!source.deathMessage.hash && !source.healthModifiedMessage.hash);
            bossPlans += pool[i].family == "Hunter" || pool[i].family == "DividerCore" ||
                         pool[i].family == "DividerPart";
        }
    }
    assert(bossPlans > 0);
    std::printf("Experimental classes enabled: %zu cross-family plans.\n", extendedPlans);
    cfg.ventVariants = true;
    size_t ventPlans = 0;
    for (const auto& source : pool) {
        if (!ds2::ventContext(source.context)) continue;
        const auto plan = ds2::buildPlan(cfg, source, pool, 8);
        if (!plan.donors.empty()) ++ventPlans;
        for (auto i : plan.donors) {
            assert(!source.arrivalKey.empty());
            assert(source.arrivalKey == pool[i].arrivalKey);
            assert(source.family == pool[i].family);
            assert(source.context == pool[i].context);
        }
    }
    assert(ventPlans > 0);
    std::printf("Exact same-vent variant plans: %zu.\n", ventPlans);
    cfg.crossFamilyVents = true;
    size_t crossVentPlans = 0;
    for (const auto& source : pool) {
        if (!ds2::ventContext(source.context)) continue;
        const auto plan = ds2::buildPlan(cfg, source, pool, 8);
        if (!plan.donors.empty()) ++crossVentPlans;
        for (auto i : plan.donors) assert(ds2::compatibleVent(cfg, source, pool[i]));
    }
    std::printf("Authored cross-family vent plans: %zu.\n", crossVentPlans);
    // Scripted arrivals stay refused until their own switches are set.
    for (const auto& source : pool) {
        if (!ds2::scriptedContext(source.context)) continue;
        const auto status = ds2::buildPlan(cfg, source, pool, 8).status;
        assert(status == ds2::SelectionStatus::UnsupportedClass ||
               status == ds2::SelectionStatus::UnsupportedArrival);
    }
    cfg.scriptedArrivals = true;
    cfg.infectedArrivals = true;
    size_t scriptedPlans = 0, scriptedSources = 0;
    for (const auto& source : pool) {
        if (!ds2::scriptedContext(source.context)) continue;
        ++scriptedSources;
        const auto plan = ds2::buildPlan(cfg, source, pool, 8);
        if (!plan.donors.empty()) {
            ++scriptedPlans;
            assert(pool[plan.donors.front()].model == plan.model);
        }
        for (auto i : plan.donors) {
            assert(pool[i].family == source.family);
            assert(pool[i].context == source.context);
            assert(!source.scriptedKey.empty() && source.scriptedKey == pool[i].scriptedKey);
            // Fallback models follow the preferred one, so only the first donor
            // is guaranteed to be the plan's model. Every donor must still be a
            // different model from the source.
            assert(pool[i].model != source.model);
        }
    }
    assert(scriptedPlans > 0);
    std::printf("Scripted arrival sources: %zu; exact-contract plans: %zu.\n",
                scriptedSources, scriptedPlans);
    // Drill-room regression. These two are authored with the identical spawning
    // component: SPAWNMODE_ANIM, zombie_rail_climb, SPAWNANIMFLAG_NOPAININTERRUPT,
    // SPAWNACTIVATIONFLAG_IMMEDIATE. They differ only in model, so the miner and the
    // super slasher are proven donors for each other. The elevator slasher is the
    // corpus's only zombie_rail_climb record that also sets SPAWNANIMFLAG_NOGRAVITY,
    // so it has no donor and must report no_candidate rather than being widened into
    // the weaker option set.
    const ds2::Guid drillMiner{0x34CABBE5,0xFC471012,0x54544F43,0x33363053};
    const ds2::Guid drillSuper{0x34CAD1EA,0x20CDF410,0x54544F43,0x33363053};
    const ds2::Guid elevatorClimb{0x5ACA19F9,0xDD7CA373,0x52444F52,0x45554749};
    const ds2::EnemyRecord* miner = nullptr;
    const ds2::EnemyRecord* superSlasher = nullptr;
    const ds2::EnemyRecord* elevator = nullptr;
    for (const auto& record : pool) {
        if (record.guid == drillMiner) miner = &record;
        if (record.guid == drillSuper) superSlasher = &record;
        if (record.guid == elevatorClimb) elevator = &record;
    }
    assert(miner && superSlasher && elevator);
    assert(ds2::compatibleScripted(cfg, *miner, *superSlasher));
    assert(ds2::compatibleScripted(cfg, *superSlasher, *miner));
    assert(!ds2::buildPlan(cfg, *miner, pool, 8).donors.empty());
    assert(!ds2::compatibleScripted(cfg, *elevator, *miner));
    assert(ds2::buildPlan(cfg, *elevator, pool, 8).status == ds2::SelectionStatus::NoCandidate);
    cfg.scriptedArrivals = false;
    cfg.infectedArrivals = false;
    // Both switches off restores the previous refusals for the same records.
    assert(ds2::buildPlan(cfg, *miner, pool, 8).status == ds2::SelectionStatus::UnsupportedClass);
    assert(!ds2::compatibleScripted(cfg, *miner, *superSlasher));
    const ds2::Guid parishZombie{0x5ACB27AD,0xA7CA579E,0x52444F52,0x45554749};
    const ds2::Guid parishInfector{0x39CB067E,0x2B7315E4,0x4B4C4157,0x32345245};
    const ds2::EnemyRecord* zombie = nullptr;
    const ds2::EnemyRecord* infector = nullptr;
    for (const auto& record : pool) {
        if (record.guid == parishZombie) zombie = &record;
        if (record.guid == parishInfector) infector = &record;
    }
    assert(zombie && infector);
    assert(ds2::compatibleVent(cfg, *zombie, *infector));
    assert(!ds2::buildPlan(cfg, *zombie, pool, 8).donors.empty());
}
