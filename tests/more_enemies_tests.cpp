#include "../src/more_enemies.h"
#include "../src/generated_enemy_catalog.h"
#include <cassert>
#include <cstdio>
#include <set>

int main() {
    using namespace ds2;
    MoreEnemiesSettings cfg;
    std::map<std::string, unsigned> totals{
        {"Zombie", 3}, {"PackEnemy", 4}, {"Hunter", 6}, {"DividerPart", 5},
        {"Guardian", 8}, {"Puker", 2}};
    MoreEnemiesBudget budget;

    // Disabled is the default and nothing else is evaluated.
    assert(decideMoreEnemies(cfg, totals, "Zombie", "default_NONE_descriptor",
                             budget).status == MoreEnemiesStatus::Disabled);
    cfg.enabled = true;

    // Direct arrivals multiply, and the extra count is the total minus the
    // authored one that already happened.
    auto direct = decideMoreEnemies(cfg, totals, "Zombie", "default_NONE_descriptor",
                                    budget);
    assert(direct.status == MoreEnemiesStatus::Multiplied && direct.extras == 2);

    // Vents are off by default and refused as an arrival, not as a family.
    assert(decideMoreEnemies(cfg, totals, "Zombie", "SPAWNMODE_VENT|Wall", budget)
               .status == MoreEnemiesStatus::UnsupportedArrival);
    // Scripted animation, play-dead and infection arrivals stay refused even
    // when vents are allowed.
    cfg.vents = true;
    for (const char* context : {"SPAWNMODE_ANIM", "SPAWNMODE_ZOMBIE_PLAYDEAD",
                                "SPAWNMODE_INFECTED", "no_spawning_component"})
        assert(decideMoreEnemies(cfg, totals, "Zombie", context, budget).status ==
               MoreEnemiesStatus::UnsupportedArrival);
    auto vent = decideMoreEnemies(cfg, totals, "Zombie", "SPAWNMODE_VENT|Ceiling",
                                  budget);
    assert(vent.status == MoreEnemiesStatus::Multiplied && vent.extras == 2);
    cfg.vents = false;

    // Protected families stay at one whatever the configuration asks for.
    for (const char* family : {"Hunter", "DividerCore", "DividerPart", "Nest",
                               "NestProjectile", "Guardian", "GuardianPod",
                               "GuardianTentacle", "TripodProjectile", "Landmine",
                               "Tripod", "WindowTripod", "BossTripod", "Kong",
                               "NicoleBoss", "Polyp", "DragTentacle",
                               "SlamDragTentacle", "LeviathanTentacle",
                               "SlugTentacle", "StabTentacle"}) {
        assert(moreEnemiesProtectedFamily(family));
        assert(moreEnemiesTotalFor(cfg, totals, family) == 1);
        assert(decideMoreEnemies(cfg, totals, family, "default_NONE_descriptor",
                                 budget).status == MoreEnemiesStatus::ProtectedFamily);
    }
    // The two the project names explicitly are covered by that list.
    assert(moreEnemiesProtectedFamily("Hunter") && moreEnemiesProtectedFamily("DividerCore"));

    // A family left at one is reported as such rather than silently skipped.
    assert(decideMoreEnemies(cfg, totals, "Lurker", "default_NONE_descriptor",
                             budget).status == MoreEnemiesStatus::MultiplierOne);

    // The per-trigger ceiling clamps configuration, and the absolute ceiling
    // clamps the per-trigger ceiling.
    assert(moreEnemiesTotalFor(cfg, totals, "PackEnemy") == 3);
    cfg.perTrigger = 99;
    assert(moreEnemiesTotalFor(cfg, totals, "PackEnemy") == 4);
    std::map<std::string, unsigned> huge{{"Zombie", 100}};
    assert(moreEnemiesTotalFor(cfg, huge, "Zombie") == kMoreEnemiesMaxPerTrigger);
    cfg.perTrigger = 3;

    // Unlisted families fall back to the default total.
    cfg.defaultTotal = 2;
    assert(moreEnemiesTotalFor(cfg, totals, "SomethingNew") == 2);
    cfg.defaultTotal = 1;
    assert(decideMoreEnemies(cfg, totals, "SomethingNew", "default_NONE_descriptor",
                             budget).status == MoreEnemiesStatus::MultiplierOne);

    // The rolling budget hands out at most its limit inside one window, and
    // opens again in the next one.
    MoreEnemiesBudget rolling;
    // The window counts arrivals, not seconds: five per sixteen arrivals here.
    assert(moreEnemiesGrant(rolling, 5, 3) == 3);
    assert(moreEnemiesGrant(rolling, 5, 3) == 2);
    assert(moreEnemiesGrant(rolling, 5, 3) == 0);
    for (int i = 0; i < 13; ++i) moreEnemiesGrant(rolling, 5, 0);
    assert(moreEnemiesGrant(rolling, 5, 3) == 3);
    // GetTickCount rollover must not open an unbounded window.
    MoreEnemiesBudget wrap;
    // Nothing wraps any more, because nothing reads a clock.
    assert(moreEnemiesGrant(wrap, 4, 4) == 4);
    assert(moreEnemiesGrant(wrap, 4, 4) == 0);
    // Zero disables the feature through the budget alone.
    MoreEnemiesBudget none;
    assert(moreEnemiesGrant(none, 0, 1) == 0);
    MoreEnemiesSettings starved = cfg;
    starved.budgetPerSecond = 0;
    MoreEnemiesBudget starvedBudget;
    assert(decideMoreEnemies(starved, totals, "Zombie", "default_NONE_descriptor",
                             starvedBudget).status == MoreEnemiesStatus::BudgetExhausted);
    // The absolute rate ceiling cannot be raised by configuration.
    MoreEnemiesBudget capped;
    assert(moreEnemiesGrant(capped, 1000, 1000) == kMoreEnemiesMaxPerSecond);

    // Every status has a distinct name, so one log line can tell the paths apart.
    std::set<std::string> names;
    for (unsigned i = 0; i < static_cast<unsigned>(MoreEnemiesStatus::Count); ++i)
        names.insert(moreEnemiesStatusName(i));
    assert(names.size() == static_cast<unsigned>(MoreEnemiesStatus::Count));
    assert(std::string(moreEnemiesStatusName(
        static_cast<unsigned>(MoreEnemiesStatus::Count))) == "invalid_status");

    // Against the real catalog: every accepted row is a direct or vent arrival
    // of an unprotected family, and no protected family ever gets through.
    unsigned accepted = 0, protectedRows = 0, refusedArrival = 0;
    MoreEnemiesSettings live;
    live.enabled = true;
    live.vents = true;
    live.perTrigger = 3;
    live.budgetPerSecond = kMoreEnemiesMaxPerSecond;
    {
        // replacements: the family that stands there decides
        ds2::MoreEnemiesSettings cross;
        cross.enabled = true; cross.perTrigger = 8; cross.budgetPerSecond = 112; cross.defaultTotal = 1;
        std::map<std::string, unsigned> t{{"Zombie", 4}, {"Puker", 2}, {"SwarmEnemy", 4}, {"Stalker", 4}};
        ds2::MoreEnemiesBudget b;
        auto d = ds2::decideMoreEnemies(cross, t, "Zombie", "default_NONE_descriptor", b, "Puker");
        assert(d.status == ds2::MoreEnemiesStatus::Multiplied && d.extras == 1);   // min(4, 2) - 1
        b = ds2::MoreEnemiesBudget{};
        d = ds2::decideMoreEnemies(cross, t, "Zombie", "default_NONE_descriptor", b, "Zombie");
        assert(d.status == ds2::MoreEnemiesStatus::Multiplied && d.extras == 3);   // same family: authored total
        b = ds2::MoreEnemiesBudget{};
        d = ds2::decideMoreEnemies(cross, t, "SwarmEnemy", "default_NONE_descriptor", b, "Stalker");
        assert(d.status == ds2::MoreEnemiesStatus::SwarmSlot && d.extras == 0);
        b = ds2::MoreEnemiesBudget{};
        d = ds2::decideMoreEnemies(cross, t, "SwarmEnemy", "default_NONE_descriptor", b, "SwarmEnemy");
        assert(d.status == ds2::MoreEnemiesStatus::SwarmSlot && d.extras == 0);
        b = ds2::MoreEnemiesBudget{};
        d = ds2::decideMoreEnemies(cross, t, "SwarmEnemy", "default_NONE_descriptor", b);
        assert(d.status == ds2::MoreEnemiesStatus::SwarmSlot && d.extras == 0);
        b = ds2::MoreEnemiesBudget{};
        d = ds2::decideMoreEnemies(cross, t, "Zombie", "default_NONE_descriptor", b, "Guardian");
        assert(d.status == ds2::MoreEnemiesStatus::ProtectedFamily);
        cross.crossFamily = false;
        b = ds2::MoreEnemiesBudget{};
        d = ds2::decideMoreEnemies(cross, t, "Zombie", "default_NONE_descriptor", b, "Puker");
        assert(d.status == ds2::MoreEnemiesStatus::CrossFamilyRefused);
        assert(std::string(ds2::moreEnemiesStatusName(static_cast<unsigned>(ds2::MoreEnemiesStatus::CrossFamilyRefused))) == "cross_family_refused");
        assert(std::string(ds2::moreEnemiesStatusName(static_cast<unsigned>(ds2::MoreEnemiesStatus::Multiplied))) == "multiplied");
    }
    std::map<std::string, unsigned> everything;
    for (const auto& row : kEnemyCatalog) everything[row.family] = 3;
    MoreEnemiesBudget liveBudget;
    for (const auto& row : kEnemyCatalog) {
        liveBudget = MoreEnemiesBudget{};
        const auto decision = decideMoreEnemies(live, everything, row.family,
                                                row.context, liveBudget);
        if (decision.status == MoreEnemiesStatus::Multiplied) {
            ++accepted;
            assert(!moreEnemiesProtectedFamily(row.family));
            assert(directContext(row.context) || ventContext(row.context));
            assert(decision.extras == 2);
        } else if (decision.status == MoreEnemiesStatus::ProtectedFamily) {
            ++protectedRows;
        } else if (decision.status == MoreEnemiesStatus::UnsupportedArrival) {
            ++refusedArrival;
            assert(!directContext(row.context) && !ventContext(row.context));
        }
    }
    assert(accepted && protectedRows && refusedArrival);
    std::printf("More enemies: %u catalog rows accepted, %u protected, %u refused arrivals.\n",
                accepted, protectedRows, refusedArrival);
    std::printf("More enemies tests passed; no native calls.\n");
    return 0;
}
