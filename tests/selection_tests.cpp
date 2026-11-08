// Dead Space 2 United by Richard Hafer.
#include "../src/selection.h"
#include <cassert>
#include <cstdio>
int main() {
    using namespace ds2;
    EnemyRecord source{{1,2,3,4},{4,5,6,7},"Zombie","default_NONE_descriptor",true,true,true,false};
    EnemyRecord donor{{9,10,11,12},{8,9,10,11},"Zombie","SPAWNMODE_NONE",true,true,true,false};
    std::vector<EnemyRecord> pool{donor};
    PoolSettings cfg;
    // The gating rules, exercised through the plan the runtime actually uses.
    auto offers = [&]() { return !buildPlan(cfg, source, pool, 8).donors.empty(); };
    assert(!offers());
    cfg.enabled=true;
    assert(offers());
    cfg.chance=0; assert(!offers()); cfg.chance=100;
    source.sourceEnabled=false; assert(!offers()); source.sourceEnabled=true;
    pool[0].targetEnabled=false; assert(!offers()); pool[0].targetEnabled=true;
    pool[0].family="Puker"; assert(!offers());
    cfg.crossFamily=true; assert(offers());
    pool[0].context="SPAWNMODE_VENT|Wall"; assert(!offers());
    pool[0]=donor; source.context="SPAWNMODE_ANIM"; assert(!offers());
    source.context="default_NONE_descriptor";
    pool[0].ambiguous=true; assert(!offers()); pool[0]=donor;
    pool[0].ordinary=false; assert(!offers()); pool[0]=donor;
    pool[0].model=source.model; assert(!offers()); pool[0]=donor;
    // A blocklisted vent donor is still fine as a walk-in.
    pool[0].ventBlocked=true; assert(offers()); pool[0]=donor;
    // An enemy that never dies is refused wherever the source's death is
    // awaited, and offered where nothing is.
    {
        auto waited = source;
        waited.deathMessage = {"room_npc_dead", 0x1234u};
        pool[0].unkillable = true;
        assert(buildPlan(cfg, waited, pool, 8).donors.empty());
        auto counted = source;
        counted.healthModifiedMessage = {"room_npc_hurt", 0x5678u};
        assert(buildPlan(cfg, counted, pool, 8).donors.empty());
        assert(offers());
        pool[0] = donor;
    }
    // A variant's chance is its share of its family's spawns: a seeded roll
    // under the chance puts it in front of the family's plain models, a lost
    // roll puts it behind them. Chance 0 keeps it out entirely (13.09.).
    {
        EnemyRecord plain{{13,14,15,16},{12,13,14,15},"Zombie","SPAWNMODE_NONE",true,true,true,false};
        pool = {donor, plain};
        pool[0].variantName = "Test";
        pool[0].chance = 0;
        assert(offers());
        assert(buildPlan(cfg, source, pool, 8).model == plain.model);
        pool[0].chance = 100;
        assert(buildPlan(cfg, source, pool, 8).model == donor.model);
        pool[0].chance = 50;
        unsigned granted = 0;
        for (unsigned seed = 1; seed <= 200; ++seed) {
            cfg.seed = seed;
            const bool now = buildPlan(cfg, source, pool, 8).model == donor.model;
            for (int i = 0; i < 3; ++i) assert((buildPlan(cfg, source, pool, 8).model == donor.model) == now);
            granted += now;
        }
        assert(granted > 60 && granted < 140);
        cfg.seed = 1;
        pool = {donor};
    }
    const auto first=buildPlan(cfg,source,pool,8).model;
    for (int i=0;i<100;++i) assert(buildPlan(cfg,source,pool,8).model==first);
    auto plan = buildPlan(cfg, source, pool, 8);
    assert(plan.donors.size() == 1 && plan.model == donor.model);
    pool[0].weight = 0;
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    pool[0] = donor;
    auto duplicate = donor;
    duplicate.guid = {99,98,97,96};
    pool.push_back(duplicate);
    auto other = donor;
    other.guid = {88,87,86,85};
    other.model = {55,54,53,52};
    pool.push_back(other);
    const auto withDuplicate = buildPlan(cfg, source, pool, 8);
    std::vector<EnemyRecord> withoutDuplicate{donor, other};
    assert(withDuplicate.model == buildPlan(cfg, source, withoutDuplicate, 8).model);
    std::reverse(pool.begin(), pool.end());
    const auto reordered = buildPlan(cfg, source, pool, 8);
    assert(withDuplicate.model == reordered.model);
    assert(buildPlan(cfg, source, pool, 1).donors.size() == 1);
    assert(buildPlan(cfg, source, pool, 0).donors.empty());
    cfg.chance = 0;
    assert(buildPlan(cfg, source, pool, 8).status == SelectionStatus::ChanceSkipped);
    cfg.chance = 100;
    source.context = "SPAWNMODE_VENT|Wall";
    assert(buildPlan(cfg, source, pool, 8).status == SelectionStatus::UnsupportedArrival);
    source.context = "SPAWNMODE_NONE";
    source.ambiguous = true;
    assert(buildPlan(cfg, source, pool, 8).status == SelectionStatus::AmbiguousSource);
    source.ambiguous = false;
    source.context = "SPAWNMODE_VENT|Wall";
    source.arrivalKey = "same-vent-and-parameters";
    pool = {donor};
    pool[0].context = source.context;
    pool[0].arrivalKey = source.arrivalKey;
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    cfg.ventVariants = true;
    assert(buildPlan(cfg, source, pool, 8).donors.size() == 1);
    pool[0].arrivalKey = "another-vent";
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    pool[0].arrivalKey = source.arrivalKey;
    pool[0].context = "SPAWNMODE_VENT|Ceiling";
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    pool[0].context = source.context;
    pool[0].family = "Puker";
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    source.crossArrivalKey = "same-spawning-settings";
    pool[0].crossArrivalKey = source.crossArrivalKey;
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    cfg.crossFamilyVents = true;
    assert(buildPlan(cfg, source, pool, 8).donors.size() == 1);
    pool[0].crossArrivalKey = "different-vent-or-messages";
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    pool[0].crossArrivalKey = source.crossArrivalKey;
    cfg.crossFamily = false;
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    cfg.crossFamily = true;
    pool[0].family = source.family;
    source.arrivalKey.clear();
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    // Rebinding: two vent arrivals on different vents. Everything else about the
    // arrival has to be identical, which is what ventFreeKey carries.
    source.arrivalKey = "source-contract";
    source.crossArrivalKey = "source-spawn";
    source.ventFreeKey = "same-arrival-different-vent";
    source.vent = {1,2,3,4};
    pool[0].arrivalKey = "another-contract";
    pool[0].crossArrivalKey = "another-spawn";
    pool[0].ventFreeKey = source.ventFreeKey;
    pool[0].vent = {5,6,7,8};
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    cfg.ventRebinding = true;
    assert(buildPlan(cfg, source, pool, 8).donors.size() == 1);
    pool[0].family = "Exploder";
    assert(buildPlan(cfg, source, pool, 8).donors.size() == 1);
    // A different activation contract is refused even with rebinding on.
    pool[0].ventFreeKey = "different-activation";
    assert(buildPlan(cfg, source, pool, 8).donors.empty());

    // Two spawns that differ only in their spawn-done message. Refused by
    // default, because that message is what a level script waits for; allowed
    // with VentMessageContract, because the rebind hands the replacement the
    // source's own spawning component and the message comes with it.
    source.ventOpenKey = "same-arrival-without-the-message";
    pool[0].ventOpenKey = source.ventOpenKey;
    assert(!ventMessageRelaxed(cfg, source, pool[0]));
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    cfg.ventMessageContract = true;
    assert(ventMessageRelaxed(cfg, source, pool[0]));
    assert(buildPlan(cfg, source, pool, 8).donors.size() == 1);
    // It never outranks the rest of the contract: no rebinding, no relaxation.
    cfg.ventRebinding = false;
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    cfg.ventRebinding = true;
    // And an open key of its own is not enough when the vents are missing.
    pool[0].vent = {};
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    pool[0].vent = {5,6,7,8};
    // A pair that already agrees on the strict key is not counted as relaxed.
    pool[0].ventFreeKey = source.ventFreeKey;
    assert(!ventMessageRelaxed(cfg, source, pool[0]));
    cfg.ventMessageContract = false;
    source.ventOpenKey.clear();
    pool[0].ventOpenKey.clear();
    cfg.crossFamilyVents = false;
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    cfg.crossFamilyVents = true;
    pool[0].context = "SPAWNMODE_VENT|Ceiling";
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    pool[0].context = source.context;
    pool[0].vent = {};
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    // A donor that walks in has no way out of a vent, whatever its ventMask
    // claims and whether or not it comes from the private pool.
    pool[0].vent = {5,6,7,8};
    pool[0].privateTemplate = true;
    pool[0].context = "default_NONE_descriptor";
    pool[0].ventMask = 7;
    assert(buildPlan(cfg, source, pool, 8).donors.empty());
    pool[0].privateTemplate = false;
    pool[0].context = source.context;
    assert(buildPlan(cfg, source, pool, 8).donors.size() == 1);
    source.context = "default_NONE_descriptor";
    pool[0].context = "default_NONE_descriptor";
    pool.resize(2, pool[0]);
    pool[1].guid = {99,98,97,96};
    pool[1].privateTemplate = true;
    const auto preferred = buildPlan(cfg, source, pool, 1);
    assert(preferred.donors.size() == 1 && preferred.donors[0] == 1);

    // Scripted arrivals: authored animation, play-dead and infected contracts.
    PoolSettings scriptedCfg;
    scriptedCfg.enabled = true;
    EnemyRecord animated{{1,2,3,4},{4,5,6,7},"Zombie","SPAWNMODE_ANIM",true,true,false,false};
    EnemyRecord animatedDonor{{9,10,11,12},{8,9,10,11},"Zombie","SPAWNMODE_ANIM",true,true,false,false};
    std::vector<EnemyRecord> animatedPool{animatedDonor};
    // Off by default: an animated class keeps reporting unsupported_class.
    assert(buildPlan(scriptedCfg, animated, animatedPool, 8).status == SelectionStatus::UnsupportedClass);
    scriptedCfg.scriptedArrivals = true;
    // Enabling the switch alone proves nothing; both sides need an authored contract.
    assert(buildPlan(scriptedCfg, animated, animatedPool, 8).status == SelectionStatus::UnsupportedClass);
    animated.scriptedKey = "zombie_rail_climb|nopain|immediate";
    assert(buildPlan(scriptedCfg, animated, animatedPool, 8).donors.empty());
    animatedPool[0].scriptedKey = animated.scriptedKey;
    assert(buildPlan(scriptedCfg, animated, animatedPool, 8).donors.size() == 1);
    // A different authored animation, option set or message is a different contract.
    animatedPool[0].scriptedKey = "zombie_rail_climb_right|nopain|immediate";
    assert(buildPlan(scriptedCfg, animated, animatedPool, 8).donors.empty());
    animatedPool[0].scriptedKey = animated.scriptedKey;
    // Cross-family scripted arrivals stay refused even with both cross switches on.
    scriptedCfg.crossFamily = true;
    scriptedCfg.crossFamilyVents = true;
    animatedPool[0].family = "Puker";
    assert(buildPlan(scriptedCfg, animated, animatedPool, 8).donors.empty());
    animatedPool[0].family = "Zombie";
    // A scripted source never falls back to the direct or vent pools.
    scriptedCfg.ventVariants = true;
    scriptedCfg.ventRebinding = true;
    animatedPool[0].context = "default_NONE_descriptor";
    animatedPool[0].ordinary = true;
    assert(buildPlan(scriptedCfg, animated, animatedPool, 8).donors.empty());
    animatedPool[0].context = "SPAWNMODE_VENT|Wall";
    animatedPool[0].arrivalKey = "vent";
    animatedPool[0].crossArrivalKey = "vent";
    animatedPool[0].vent = {1,2,3,4};
    assert(buildPlan(scriptedCfg, animated, animatedPool, 8).donors.empty());
    animatedPool[0] = animatedDonor;
    animatedPool[0].scriptedKey = animated.scriptedKey;
    assert(buildPlan(scriptedCfg, animated, animatedPool, 8).donors.size() == 1);
    // A direct or vent source is never routed into the scripted pool.
    EnemyRecord ground = animated;
    ground.context = "default_NONE_descriptor";
    ground.ordinary = false;
    assert(buildPlan(scriptedCfg, ground, animatedPool, 8).status == SelectionStatus::UnsupportedClass);
    // Play-dead uses the same gate; infected has its own.
    animated.context = animatedPool[0].context = "SPAWNMODE_ZOMBIE_PLAYDEAD";
    assert(buildPlan(scriptedCfg, animated, animatedPool, 8).donors.size() == 1);
    animated.context = animatedPool[0].context = "SPAWNMODE_INFECTED";
    assert(buildPlan(scriptedCfg, animated, animatedPool, 8).status == SelectionStatus::UnsupportedClass);
    scriptedCfg.infectedArrivals = true;
    assert(buildPlan(scriptedCfg, animated, animatedPool, 8).donors.size() == 1);
    scriptedCfg.scriptedArrivals = false;
    animated.context = animatedPool[0].context = "SPAWNMODE_ANIM";
    assert(buildPlan(scriptedCfg, animated, animatedPool, 8).status == SelectionStatus::UnsupportedClass);
    // Contexts must still agree on both sides.
    scriptedCfg.scriptedArrivals = true;
    animatedPool[0].context = "SPAWNMODE_ZOMBIE_PLAYDEAD";
    assert(buildPlan(scriptedCfg, animated, animatedPool, 8).donors.empty());
    // The Severed tiers (18.10.): a twitcher roll on a plain slasher offers the security and
    // classic copies, on an enhanced slasher only the oracle copy; an ordinary draw keeps the
    // tiers apart in the family as well.
    {
        PoolSettings severed;
        severed.enabled = true;
        severed.chance = 0;
        severed.twitcherChance = 100;
        EnemyRecord plainSource{{30,31,32,33},{40,41,42,43},"Zombie","default_NONE_descriptor",true,true,true,false};
        EnemyRecord enhancedSource{{34,35,36,37},{44,45,46,47},"Zombie","default_NONE_descriptor",true,true,true,false};
        enhancedSource.enhanced = true;
        EnemyRecord security{{50,51,52,53},{60,61,62,63},"Zombie","SPAWNMODE_NONE",true,true,true,false};
        security.retargeted = true;
        EnemyRecord oracle{{54,55,56,57},{64,65,66,67},"Zombie","SPAWNMODE_NONE",true,true,true,false};
        oracle.retargeted = oracle.enhanced = true;
        EnemyRecord slasher{{58,59,60,61},{68,69,70,71},"Zombie","SPAWNMODE_NONE",true,true,true,false};
        const std::vector<EnemyRecord> tiers{security, oracle, slasher};
        const auto plainPlan = buildPlan(severed, plainSource, tiers, 8);
        assert(plainPlan.twitcherOnly && plainPlan.donors.size() == 1 && tiers[plainPlan.donors[0]].guid == security.guid);
        const auto enhancedPlan = buildPlan(severed, enhancedSource, tiers, 8);
        assert(enhancedPlan.twitcherOnly && enhancedPlan.donors.size() == 1 && tiers[enhancedPlan.donors[0]].guid == oracle.guid);
        severed.chance = 100;
        for (const size_t i : buildPlan(severed, plainSource, tiers, 8).donors) assert(tiers[i].guid != oracle.guid);
        for (const size_t i : buildPlan(severed, enhancedSource, tiers, 8).donors) assert(tiers[i].guid != security.guid);
    }
    std::puts("Selection tests passed.");
}
