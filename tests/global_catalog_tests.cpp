// Dead Space 2 United by Richard Hafer.
#include "../src/selection.h"
#include "../src/generated_enemy_catalog.h"
#include "../src/generated_global_targets.h"
#include "../src/global_pool.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <set>

static uint32_t registrationHash(const ds2::Guid& id) {
    return ((id[0] * 33 + id[1]) * 33 + id[2]) * 33 + id[3];
}

int main() {
    std::set<uint32_t> occupied;
    for (const auto& row : ds2::kEnemyCatalog) occupied.insert(registrationHash(row.guid));
    unsigned count = 0;
    for (const auto& row : ds2::kGlobalTargets) {
        assert(row.guid != ds2::Guid{} && row.model != ds2::Guid{});
        assert(occupied.insert(registrationHash(row.guid)).second);
        bool sourceFound = false;
        for (const auto& source : ds2::kEnemyCatalog) {
            if (source.guid != row.source) continue;
            assert(!source.ambiguous);
            assert(source.model == row.model);
            assert(std::strcmp(source.family, row.family) == 0);
            assert(std::strcmp(source.context, row.context) == 0);
            sourceFound = true;
        }
        assert(sourceFound);
        ++count;
    }
    assert(count == 34);
    std::vector<ds2::EnemyRecord> pool;
    for (const auto& row : ds2::kEnemyCatalog) {
        ds2::EnemyRecord record{row.guid, row.model, row.family, row.context};
        record.ordinary = row.ordinary;
        record.ambiguous = row.ambiguous;
        record.weight = 73;
        record.targetEnabled = record.family != "Exploder";
        pool.push_back(record);
    }
    const auto original = pool;
    assert(ds2::appendGlobalTargets(pool));
    assert(pool.size() == original.size() + 34);
    assert(std::is_sorted(pool.begin(), pool.end(), [](const auto& a, const auto& b) { return a.guid < b.guid; }));
    unsigned privateCount = 0;
    for (const auto& record : pool) if (record.privateTemplate) {
        ++privateCount;
        assert(!record.sourceEnabled && record.weight == 73);
        assert(record.targetEnabled == (record.family != "Exploder"));
        assert(record.vent == ds2::Guid{});
    }
    assert(privateCount == 34);
    const auto size = pool.size();
    assert(!ds2::appendGlobalTargets(pool) && pool.size() == size);
    auto incomplete = original;
    incomplete.erase(std::remove_if(incomplete.begin(), incomplete.end(), [](const auto& row) {
        return row.guid == ds2::kGlobalTargets[0].source;
    }), incomplete.end());
    const auto before = incomplete.size();
    assert(!ds2::appendGlobalTargets(incomplete) && incomplete.size() == before);

    // Play-dead donors: the third arrival kind. Unlike a vent donor the record
    // keeps its scripted contract, because that key is what a swap is matched
    // on, and a row whose key disagrees with the catalog must be refused.
    std::vector<ds2::EnemyRecord> corpses;
    for (const auto& row : ds2::kEnemyCatalog) {
        ds2::EnemyRecord record{row.guid, row.model, row.family, row.context};
        record.ordinary = row.ordinary;
        record.ambiguous = row.ambiguous;
        record.scriptedKey = row.scriptedKey;
        corpses.push_back(record);
    }
    const auto plain = corpses;
    assert(ds2::appendPlaydeadDonors(corpses));
    const size_t donors = sizeof(ds2::kPlaydeadDonors) / sizeof(ds2::kPlaydeadDonors[0]);
    assert(donors >= 2);
    assert(corpses.size() == plain.size() + donors);
    unsigned playdead = 0;
    for (const auto& row : ds2::kPlaydeadDonors) {
        for (const auto& record : corpses) {
            if (record.guid != row.guid) continue;
            ++playdead;
            assert(record.privateTemplate && !record.sourceEnabled);
            assert(ds2::scriptedContext(record.context));
            // Kept, not cleared: compatibleScripted matches on exactly this.
            assert(record.scriptedKey == row.scripted);
            assert(record.arrivalKey.empty() && record.crossArrivalKey.empty());
            assert(record.vent == ds2::Guid{});
        }
    }
    assert(playdead == donors);
    // Every donor has a partner, or it could never be selected.
    for (const auto& row : ds2::kPlaydeadDonors) {
        unsigned sources = 0;
        for (const auto& record : plain)
            sources += record.scriptedKey == row.scripted && ds2::scriptedContext(record.context);
        assert(sources >= 2);
    }
    // A pool that never learned the contract must be refused, not guessed at.
    auto blank = plain;
    for (auto& record : blank) record.scriptedKey.clear();
    const auto blankSize = blank.size();
    assert(!ds2::appendPlaydeadDonors(blank) && blank.size() == blankSize);

    // A direct donor stands in for a root: the replacement is offered, the root
    // it supersedes stops being one, and both halves have to be present.
    auto direct = pool;   // roots already appended above
    const size_t directRows = sizeof(ds2::kDirectDonors) / sizeof(ds2::kDirectDonors[0]);
    assert(ds2::appendDirectDonors(direct));
    assert(direct.size() == pool.size() + directRows);
    for (const auto& row : ds2::kDirectDonors) {
        bool sawReplacement = false, sawSuperseded = false;
        for (const auto& record : direct) {
            if (record.guid == row.guid) {
                sawReplacement = true;
                assert(record.privateTemplate && !record.sourceEnabled && record.targetEnabled);
                assert(record.context == row.context);
            }
            if (record.guid == row.supersedes) {
                sawSuperseded = true;
                assert(!record.targetEnabled);
            }
        }
        assert(sawReplacement && sawSuperseded);
    }
    // Without the root there is nothing to stand in for, so it must refuse.
    auto rootless = direct;
    rootless.erase(std::remove_if(rootless.begin(), rootless.end(), [](const auto& record) {
        return record.guid == ds2::kDirectDonors[0].supersedes;
    }), rootless.end());
    const auto rootlessSize = rootless.size();
    assert(!ds2::appendDirectDonors(rootless) && rootless.size() == rootlessSize);

    // The Divider and the Ubermorph: every row has its source in the catalog
    // with the same family and a direct arrival, the copy is a private template,
    // and only the Ubermorph is marked as something that never dies.
    auto boss = direct;
    const size_t bossRows = sizeof(ds2::kBossDonors) / sizeof(ds2::kBossDonors[0]);
    assert(bossRows == 9);
    assert(ds2::appendBossDonors(boss));
    assert(boss.size() == direct.size() + bossRows);
    unsigned hunters = 0, cores = 0, parts = 0;
    for (const auto& row : ds2::kBossDonors) {
        assert(occupied.insert(registrationHash(row.guid)).second);
        bool seen = false;
        for (const auto& record : boss) {
            if (record.guid != row.guid) continue;
            seen = true;
            assert(record.privateTemplate && !record.sourceEnabled);
            assert(record.family == row.family && ds2::directContext(record.context));
            assert(record.unkillable == (record.family == "Hunter" || record.family == "DividerCore"));
            assert(record.arrivalKey.empty() && record.crossArrivalKey.empty());
            hunters += record.family == "Hunter";
            cores += record.family == "DividerCore";
            parts += record.family == "DividerPart";
        }
        assert(seen);
    }
    assert(hunters == 1 && cores == 1 && parts == 5);
    // Appending twice would duplicate identities, so it has to refuse.
    const auto bossSize = boss.size();
    assert(!ds2::appendBossDonors(boss) && boss.size() == bossSize);

    // Stat variants: each has its source in the catalog, keeps the family, gets
    // a model key of its own so it competes as its own entry, and carries the
    // chance from the spec.
    auto variants = boss;
    size_t variantRows = 0;
    for (const auto& row : ds2::kVariantDonors) if (!(row.guid == ds2::Guid{})) ++variantRows;
    // model mode expands one spec variant into a row per source record (13.09.);
    // a public build carries none (a single placeholder row, 15.09.)
    assert(ds2::appendVariantDonors(variants));
    assert(variants.size() == boss.size() + variantRows);
    for (const auto& row : ds2::kVariantDonors) {
        if (row.guid == ds2::Guid{}) continue;
        assert(occupied.insert(registrationHash(row.guid)).second);
        const ds2::EnemyRecord* copy = nullptr;
        const ds2::EnemyRecord* base = nullptr;
        for (const auto& record : variants) {
            if (record.guid == row.guid) copy = &record;
            if (record.guid == row.source) base = &record;
        }
        assert(copy && base);
        assert(copy->privateTemplate && !copy->sourceEnabled);
        assert(copy->family == row.family && copy->family == base->family);
        assert(copy->model != base->model && copy->model != ds2::Guid{});
        assert(copy->chance == row.chance && copy->chance > 0 && copy->chance <= 100);
        assert(copy->variantName == row.name);
    }
    const auto variantSize = variants.size();
    if (variantRows) assert(!ds2::appendVariantDonors(variants) && variants.size() == variantSize);

    std::cout << "Global catalog: " << count << " private roots, " << donors
              << " play-dead donors, " << directRows
              << " stand-ins, " << bossRows
              << " boss donors, " << variantRows
              << " variants, matching sources, no registration hash collisions.\n";
}
