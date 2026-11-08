// Dead Space 2 United by Hafer.
#define NOMINMAX
#include <windows.h>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../src/coop_mpmap.h"
namespace ds2 { using Guid = std::array<uint32_t, 4>; }
#include "../src/generated_survival_vents.h"
namespace survival_rt { void Event(const char*, ...) {} }
#include "../src/runtime_vent_gate.inc"

unsigned detonate(bool keep) {
    unsigned count = 0;
    for (const auto& vent : ds2::kSurvivalVentMarkers)
        if (!std::strcmp(vent.map, "mp_detonate") && vent_gate::Open(vent, keep)) ++count;
    return count;
}
int main() {
    vent_gate::Watch();
    for (int mode = 0; mode <= 3; ++mode) {
        assert(vent_gate::KeepOpenedFor("mp_detonate", mode));
        assert(vent_gate::KeepOpenedFor("pdlc_mp_ship", mode));
        assert(!vent_gate::KeepOpenedFor("arena_ch04", mode));
    }
    assert(!vent_gate::KeepOpenedFor("mp_detonate", 4));
    assert(detonate(true) == 9 && detonate(false) == 9);
    for (auto msg : {0x4610EA6Eu, 0xDD68389Bu, 0xA1C6838Fu, 0x9BF00D2Bu}) vent_gate::Note(msg);
    assert(detonate(false) == 4 && detonate(true) > 9 && detonate(true) < 32);
    std::printf("Detonate after objective 2: %u retained, %u in versus\n", detonate(true), detonate(false));
    // A complete on/off pair between polls must count, including inherited group windows.
    auto check = [](const auto& vent) {
        vent_gate::Reset();
        const bool initial = !vent.selectOn || vent.onAtStart;
        assert(vent_gate::Open(vent, true) == initial);
        if (vent.selectOff) vent_gate::Note(vent.selectOff);
        assert(vent_gate::Open(vent, true) == initial);
        if (vent.selectOn) vent_gate::Note(vent.selectOn);
        if (vent.selectOff) vent_gate::Note(vent.selectOff);
        assert(vent_gate::Open(vent, true));
        if (vent.selectOff) assert(!vent_gate::Open(vent, false));
        vent_gate::Reset();
        assert(vent_gate::Open(vent, true) == initial);
    };
    for (const auto& marker : ds2::kSurvivalVentMarkers) check(marker);
    for (const auto& spawner : ds2::kSurvivalVents) check(spawner);
    std::puts("Survival vent window regression checks passed.");
}
