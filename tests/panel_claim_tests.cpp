#include "../src/coop_panel_claims.h"
#include <algorithm>
#include <cassert>
#include <random>
#include <vector>

int main() {
    struct Event { uint32_t arrival, sequence, panel; bool held; };
    for (unsigned seed = 0; seed < 200; ++seed) {
        std::mt19937 random(seed);
        std::vector<Event> received;
        for (unsigned seq = 1; seq <= 7; ++seq) {
            for (unsigned copy = 0; copy < 2; ++copy) {
                if (random() % 5 == 0 && seq != 7) continue;
                const auto arrival = static_cast<uint32_t>(seq * 150 + 180 + random() % 900);
                received.push_back({arrival, seq, seq <= 3 ? 25u : 26u, seq != 3 && seq != 7});
            }
        }
        std::sort(received.begin(), received.end(), [](auto a, auto b) { return a.arrival < b.arrival; });
        coop_panel::Claim claim;
        for (const auto& event : received) {
            const uint32_t guid[4]{event.panel};
            claim.Update(17, event.sequence, 2, guid, event.held, event.arrival);
        }
        assert(claim.sequence == 7 && !claim.at && claim.guid[0] == 26);
    }
    coop_panel::Claim claim;
    const uint32_t guid[4]{2};
    assert(claim.Update(1, UINT32_MAX, 0, guid, true, 3));
    assert(claim.Update(1, 0, 0, guid, false, 5));
    assert(!claim.Update(1, UINT32_MAX, 0, guid, true, 6));
    assert(claim.Update(2, 0, 0, guid, true, 7));
}
