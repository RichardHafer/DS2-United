// Dead Space 2 United by Richard Hafer.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>
#include "../src/enemy_speed.h"
using namespace enemy_speed;
int main() {
    assert(ModeFor(false, 3) == kStory && ModeFor(true, 0) == kSurvival && ModeFor(true, 1) == kMercenaries && ModeFor(true, 4) == kVersus && ModeFor(true, 9) == kSurvival);
    assert(ClampPercent(0) == kMinPercent && ClampPercent(1000) == kMaxPercent && ClampPercent(140) == 140);
    const Tables plain = Load([](const char*, const char*, int f) { return f; });
    assert(!plain.Any() && plain.Factor(kStory, kSlasher) == 1.0f && plain.Factor(kStory, kNone) == 1.0f && plain.Factor(99, 0) == 1.0f);
    const Tables t = Load([](const char* s, const char* k, int f) {
        if (std::string(s) == "EnemySpeedStory" && std::string(k) == "All") return 150;
        if (std::string(s) == "EnemySpeedStory" && std::string(k) == "Stalker") return 60;
        if (std::string(s) == "EnemySpeedZone" && std::string(k) == "Brute") return 5000;
        return f;
    });
    assert(t.Any() && t.percent[kStory][kSlasher] == 150 && t.percent[kStory][kStalker] == 60 && t.percent[kSurvival][kSlasher] == 100 && t.percent[kZone][kBrute] == kMaxPercent && t.percent[kStory][kBoss] == 100);
    for (int k = 0; k < kKindCount; ++k) assert(KindKey(k) && KindKey(k)[0] != '?');
    assert(KeepsOwnSpeed("scriptedSequence") && KeepsOwnSpeed("exitVentWalkOut") && KeepsOwnSpeed("initGrapple") && !KeepsOwnSpeed("walk") && !KeepsOwnSpeed("charge") && !KeepsOwnSpeed("gravity") && !KeepsOwnSpeed("exit"));
    assert(TimedCaller(0xA93447) && TimedCaller(0xCD3551) && TimedCaller(0xAB7687) && !TimedCaller(0x11EC9B3) && !TimedCaller(0xB42F));
    std::printf("enemy_speed_tests: all passed\n");
    return 0;
}
