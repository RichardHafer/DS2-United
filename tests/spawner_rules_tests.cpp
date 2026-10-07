#include "../src/spawner_rules.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace ds2;
    const Guid sourceGuid{1, 2, 3, 4}, otherGuid{5, 6, 7, 8};
    SpawnerRules rules;
    assert(parseSpawnerRules("source_guid\tmode\r\n00000001-00000002-00000003-00000004\tblock\r\n", rules));
    // One row per spawner is the common case; `only` reads it.
    auto only = [&](const Guid& guid) -> const SpawnerRule& {
        assert(rules.at(guid).rows.size() == 1);
        return rules.at(guid).rows.front();
    };
    assert(only(sourceGuid).mode == SpawnerMode::Block && !rules.count(otherGuid));
    assert(only(sourceGuid).families.empty());
    const auto previous = rules;
    for (const auto* invalid : {
        "", "wrong\tmode\n", "source_guid\tmode\n00000000-00000000-00000000-00000000\tblock\n",
        "source_guid\tmode\n00000001-00000002-00000003-00000004\tforce\n",
        "source_guid\tmode\n00000001-00000002-00000003-00000004\tblock\n00000001-00000002-00000003-00000004\tallow\n",
        "source_guid\tmode\n00000001-00000002-00000003-00000004\tblock\textra\n",
        // A family list needs the header that declares one.
        "source_guid\tmode\n00000001-00000002-00000003-00000004\tallow\tZombie\n",
        // Nothing to say: neither a mode nor a restriction.
        "source_guid\tmode\tfamilies\n00000001-00000002-00000003-00000004\tdefault\t\n",
        // A blocked spawner selects nothing, so a list on it contradicts itself.
        "source_guid\tmode\tfamilies\n00000001-00000002-00000003-00000004\tblock\tZombie\n",
        // Empty entries, duplicates and anything that cannot be a family name.
        "source_guid\tmode\tfamilies\n00000001-00000002-00000003-00000004\tallow\tZombie;\n",
        "source_guid\tmode\tfamilies\n00000001-00000002-00000003-00000004\tallow\tZombie;Zombie\n",
        "source_guid\tmode\tfamilies\n00000001-00000002-00000003-00000004\tallow\tZom bie\n"}) {
        assert(!parseSpawnerRules(invalid, rules));
        assert(rules == previous);
    }
    assert(parseSpawnerRules("source_guid\tmode\n", rules) && rules.empty());

    // A spawner that only copes with some enemies: the families it may receive
    // are written next to the mode, and both spellings of the file are read.
    assert(parseSpawnerRules("source_guid\tmode\tfamilies\n"
        "00000001-00000002-00000003-00000004\tdefault\tZombie;Puker\n"
        "00000005-00000006-00000007-00000008\tallow\tLeaper\n", rules));
    const auto& restricted = only(sourceGuid);
    assert(restricted.mode == SpawnerMode::Default);
    assert((restricted.families == std::vector<std::string>{"Zombie", "Puker"}));
    assert(restricted.allowsFamily("Zombie") && restricted.allowsFamily("Puker"));
    assert(!restricted.allowsFamily("Leaper") && !restricted.allowsFamily("zombie"));
    assert(only(otherGuid).mode == SpawnerMode::Allow);
    assert(!only(otherGuid).allowsFamily("Zombie"));
    // No list means no restriction, which is what every rule written before this
    // feature existed carries.
    assert(SpawnerRule{}.allowsFamily("Puker"));
    // Exactly what the editor writes, empty family column and all.
    assert(parseSpawnerRules("source_guid\tmode\tfamilies\n"
        "00000001-00000002-00000003-00000004\tblock\t\n"
        "00000005-00000006-00000007-00000008\tdefault\tZombie;Leaper\n", rules));
    assert(only(sourceGuid).mode == SpawnerMode::Block && only(sourceGuid).families.empty());
    assert(only(otherGuid).families.size() == 2);
    assert(only(sourceGuid).general() && only(sourceGuid).difficulties == kEveryDifficulty);
    assert(only(sourceGuid).appliesTo(3) && only(sourceGuid).appliesTo(kAnyDifficulty));
    assert(rules.at(sourceGuid).forDifficulty(3) == &only(sourceGuid));

    // A rule for one difficulty only. The game writes the player's choice into
    // its own settings file, so on every other difficulty the rule is simply not
    // there: a Zealot-only block leaves Normal untouched.
    assert(parseSpawnerRules("source_guid\tmode\tfamilies\tdifficulty\n"
        "00000001-00000002-00000003-00000004\tblock\t\tzealot\n"
        "00000005-00000006-00000007-00000008\tdefault\tZombie\tnormal\n", rules));
    const auto& zealot = only(sourceGuid);
    assert(zealot.difficulties == 0x08 && !zealot.general() && std::string(difficultyName(3)) == "zealot");
    assert(zealot.appliesTo(3) && !zealot.appliesTo(1) && !zealot.appliesTo(kAnyDifficulty));
    assert(rules.at(sourceGuid).forDifficulty(3) == &zealot && !rules.at(sourceGuid).forDifficulty(1));
    assert(only(otherGuid).difficulties == 0x02 && only(otherGuid).allowsFamily("Zombie"));
    assert(difficultyValue("zealot") == 3 && difficultyValue("casual") == 0);
    assert(difficultyValue("nightmare") == kAnyDifficulty);
    const auto scoped = rules;
    for (const auto* invalid : {
        // A difficulty needs the header that declares one.
        "source_guid\tmode\tfamilies\n00000001-00000002-00000003-00000004\tblock\t\tzealot\n",
        // And it has to be one the game actually has.
        "source_guid\tmode\tfamilies\tdifficulty\n"
        "00000001-00000002-00000003-00000004\tblock\t\tnightmare\n",
        // Five columns are not a rule.
        "source_guid\tmode\tfamilies\tdifficulty\n"
        "00000001-00000002-00000003-00000004\tblock\t\tzealot\textra\n"}) {
        assert(!parseSpawnerRules(invalid, rules));
        assert(rules == scoped);
    }

    // Several rows for one spawner. The intro chase is blocked on Zealot and
    // Hard Core, limited to two families everywhere else. Each difficulty gets
    // exactly one answer: the row written for it, or else the general one.
    assert(parseSpawnerRules("source_guid\tmode\tfamilies\tdifficulty\n"
        "00000001-00000002-00000003-00000004\tdefault\tZombie;Puker\t\n"
        "00000001-00000002-00000003-00000004\tblock\t\tzealot;hardcore\n"
        "00000001-00000002-00000003-00000004\tallow\t\tcasual\n", rules));
    const auto& set = rules.at(sourceGuid);
    assert(set.rows.size() == 3 && spawnerRuleCount(rules) == 3);
    assert(set.forDifficulty(3)->mode == SpawnerMode::Block && set.forDifficulty(4)->mode == SpawnerMode::Block);
    assert(set.forDifficulty(3)->difficulties == 0x18 && difficultyList(0x18) == "zealot;hardcore");
    assert(set.forDifficulty(0)->mode == SpawnerMode::Allow);
    assert(set.forDifficulty(1)->mode == SpawnerMode::Default && set.forDifficulty(1)->allowsFamily("Puker"));
    assert(set.forDifficulty(2) == set.forDifficulty(1) && set.forDifficulty(1)->general());
    // An unreadable settings file leaves only the general row in force.
    assert(set.forDifficulty(kAnyDifficulty)->general());
    // Without a general row the other difficulties have no rule at all.
    assert(parseSpawnerRules("source_guid\tmode\tfamilies\tdifficulty\n"
        "00000001-00000002-00000003-00000004\tblock\t\tzealot;hardcore\n", rules));
    assert(rules.at(sourceGuid).forDifficulty(4) && !rules.at(sourceGuid).forDifficulty(1));
    const auto multi = rules;
    for (const auto* invalid : {
        // Two rows answering the same difficulty, directly or through overlap.
        "source_guid\tmode\tfamilies\tdifficulty\n"
        "00000001-00000002-00000003-00000004\tblock\t\tzealot\n"
        "00000001-00000002-00000003-00000004\tallow\t\tzealot;hardcore\n",
        // Two general rows.
        "source_guid\tmode\tfamilies\tdifficulty\n"
        "00000001-00000002-00000003-00000004\tblock\t\t\n"
        "00000001-00000002-00000003-00000004\tallow\t\t\n",
        // A repeated or empty name in the list.
        "source_guid\tmode\tfamilies\tdifficulty\n"
        "00000001-00000002-00000003-00000004\tblock\t\tzealot;zealot\n",
        "source_guid\tmode\tfamilies\tdifficulty\n"
        "00000001-00000002-00000003-00000004\tblock\t\tzealot;\n"}) {
        assert(!parseSpawnerRules(invalid, rules));
        assert(rules == multi);
    }
    uint8_t mask = 0;
    assert(parseDifficulties("hardcore;casual", mask) && mask == 0x11 && difficultyList(mask) == "casual;hardcore");
    assert(!parseDifficulties("", mask) && !parseDifficulties("zealot;nightmare", mask));
    EnemyRecord source{sourceGuid, {4,5,6,7}, "Zombie", "SPAWNMODE_NONE", false, true, true, false};
    EnemyRecord donor{otherGuid, {8,9,10,11}, "Zombie", "SPAWNMODE_NONE", true, true, true, false};
    std::vector<EnemyRecord> pool{donor};
    PoolSettings settings;
    settings.enabled = true;
    settings.chance = 0;
    assert(buildPlan(settings, source, pool, 8).status == SelectionStatus::SourceDisabled);
    assert(!buildAllowedSpawnerPlan(settings, source, pool, 8).donors.empty());
    assert(!source.sourceEnabled && settings.chance == 0);
    settings.enabled = false;
    assert(buildAllowedSpawnerPlan(settings, source, pool, 8).status == SelectionStatus::Disabled);
    settings.enabled = true;
    source.ordinary = false;
    assert(buildAllowedSpawnerPlan(settings, source, pool, 8).status == SelectionStatus::UnsupportedClass);
    source.ordinary = true;
    source.context = "SPAWNMODE_ANIM";
    assert(buildAllowedSpawnerPlan(settings, source, pool, 8).status == SelectionStatus::UnsupportedArrival);
    source.context = "SPAWNMODE_NONE";
    source.ambiguous = true;
    assert(buildAllowedSpawnerPlan(settings, source, pool, 8).status == SelectionStatus::AmbiguousSource);
    source.ambiguous = false;
    pool[0].targetEnabled = false;
    assert(buildAllowedSpawnerPlan(settings, source, pool, 8).donors.empty());
    pool[0].targetEnabled = true;
    pool[0].family = "Puker";
    assert(buildAllowedSpawnerPlan(settings, source, pool, 8).donors.empty());
    settings.crossFamily = true;
    assert(!buildAllowedSpawnerPlan(settings, source, pool, 8).donors.empty());

    // The list narrows, it never widens: a family the plan already refused stays
    // refused, and a plan that is empty for other reasons stays empty.
    SpawnerRule narrow;
    narrow.families = {"Puker"};
    const auto plan = buildAllowedSpawnerPlan(settings, source, pool, 8);
    size_t usable = 0;
    for (const auto index : plan.donors) usable += narrow.allowsFamily(pool[index].family);
    assert(usable == 1);
    narrow.families = {"Brute"};
    usable = 0;
    for (const auto index : plan.donors) usable += narrow.allowsFamily(pool[index].family);
    assert(usable == 0);
    pool[0].targetEnabled = false;
    assert(buildAllowedSpawnerPlan(settings, source, pool, 8).donors.empty());
    std::puts("Spawner rules passed: strict parsing, per-spawner families, preserved safety gates.");
}
