// Dead Space 2 United by Richard Hafer.
#include "../src/extra_spawns.h"
#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
    using namespace ds2;
    const std::string header = "source_guid\tkind\tcount\tchance\tx\ty\tz\tyaw\tarrival\n";
    ExtraSpawnRules rules;
    // a full row, a minimal row, a comment and a blank line
    assert(parseExtraSpawns(header +
        "35CAAF64-AB812255-44455257-38343145\tGuardian\t1\t100\t132.79\t-31.98\t-267.46\t90\tdirect\n"
        "# a note\n\n"
        "35CAAF64-AB812255-44455257-38343145\tsuper_slasher\t2\t50\n"
        "35CAAF65-0DF7E020-44455257-38343145\tStalker\t\t\t1\t2\t3\n", rules));
    assert(extraSpawnCount(rules) == 3 && rules.size() == 2);
    Guid first{{0x35CAAF64u, 0xAB812255u, 0x44455257u, 0x38343145u}};
    const auto& rows = rules.at(first);
    assert(rows[0].kind == "Guardian" && rows[0].count == 1 && rows[0].chance == 100);
    assert(rows[0].hasPosition && std::fabs(rows[0].position[0] - 132.79f) < 1e-3f && rows[0].hasYaw && rows[0].yaw == 90.0f);
    assert(rows[0].arrival == ExtraArrival::Direct);
    assert(rows[1].kind == "super_slasher" && rows[1].count == 2 && rows[1].chance == 50 && !rows[1].hasPosition);
    assert(rows[1].arrival == ExtraArrival::Here);
    // a position without an explicit arrival means direct
    Guid second{{0x35CAAF65u, 0x0DF7E020u, 0x44455257u, 0x38343145u}};
    assert(rules.at(second)[0].arrival == ExtraArrival::Direct && rules.at(second)[0].count == 1 && !rules.at(second)[0].hasYaw);
    // an explicit "here" keeps the spawner's own spot even when the row carries a position
    {
        ExtraSpawnRules here;
        assert(parseExtraSpawns(header +
            "35CAAF64-AB812255-44455257-38343145\tZombie\t1\t100\t1\t2\t3\t90\there\n", here));
        assert(here.at(first)[0].arrival == ExtraArrival::Here && here.at(first)[0].hasPosition);
    }
    // area rows: bound to no spawner, fire near a spot
    {
        ExtraSpawnRules byGuid;
        ExtraAreaRules areas;
        assert(parseExtraSpawns(header +
            "near:12.5\tGuardian\t1\t100\t10\t20\t30\t45\tdirect\n"
            "near\tTripod\t2\t\t1\t2\t3\n"
            "35CAAF64-AB812255-44455257-38343145\tZombie\t1\t100\n", byGuid, &areas));
        assert(areas.size() == 2 && extraSpawnCount(byGuid) == 1);
        assert(areas[0].rule.kind == "Guardian" && std::fabs(areas[0].radius - 12.5f) < 1e-4f && areas[0].rule.arrival == ExtraArrival::Direct);
        assert(areas[1].rule.kind == "Tripod" && areas[1].radius == 20.0f && areas[1].rule.count == 2 && areas[1].rule.hasPosition);
        // an area row needs a spot, and the radius has bounds
        ExtraAreaRules none;
        assert(!parseExtraSpawns(header + "near\tGuardian\t1\t100\n", byGuid, &none));
        assert(!parseExtraSpawns(header + "near:500\tGuardian\t1\t100\t1\t2\t3\n", byGuid, &none));
        assert(!parseExtraSpawns(header + "near:x\tGuardian\t1\t100\t1\t2\t3\n", byGuid, &none));
    }
    // refusals: wrong header, bad guid, count out of range, direct without a spot, odd kind
    ExtraSpawnRules bad;
    assert(!parseExtraSpawns("source_guid\tkind\n35CAAF64-AB812255-44455257-38343145\tZombie\n", bad));
    assert(!parseExtraSpawns(header + "not-a-guid\tZombie\n", bad));
    assert(!parseExtraSpawns(header + "35CAAF64-AB812255-44455257-38343145\tZombie\t9\n", bad));
    assert(!parseExtraSpawns(header + "35CAAF64-AB812255-44455257-38343145\tZombie\t1\t100\t\t\t\t\tdirect\n", bad));
    assert(!parseExtraSpawns(header + "35CAAF64-AB812255-44455257-38343145\tZom bie\n", bad));
    assert(!parseExtraSpawns(header + "35CAAF64-AB812255-44455257-38343145\tZombie\t1\t100\t1\tx\t3\n", bad));
    assert(!parseExtraSpawns(header + "35CAAF64-AB812255-44455257-38343145\tZombie\t1\t100\t1\t2\t3\t0\tvent\n", bad));
    std::puts("Extra spawn rules passed: header, defaults, direct/here, refusals");
    return 0;
}
