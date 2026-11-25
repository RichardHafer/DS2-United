// Dead Space 2 United by Richard Hafer.
#pragma once
// Pure logic of the enemy speed tables ([EnemySpeed] in DS2United.ini): modes, enemy
// kinds, the percent of a table cell and the states an enemy keeps its own speed in.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace enemy_speed {
enum Mode : int { kStory, kSurvival, kMercenaries, kObjective, kZone, kVersus, kModeCount };
inline const char* ModeSection(int m) {
    static const char* const kNames[kModeCount] = {"EnemySpeedStory", "EnemySpeedSurvival", "EnemySpeedMercenaries", "EnemySpeedObjective",
                                                   "EnemySpeedZone", "EnemySpeedVersus"};
    return m >= 0 && m < kModeCount ? kNames[m] : "?";
}
// gameMode: the picked game mode (0 survival .. 4 versus), used on an MP map or in an arena only.
inline int ModeFor(bool modeMap, int gameMode) {
    if (!modeMap) return kStory;
    return gameMode >= 0 && gameMode + 1 < kModeCount ? gameMode + 1 : kSurvival;
}

enum Kind : int {
    kSlasher, kPack, kSwarm, kTubeBeetle, kLeaper, kLurker, kExploder, kInfector, kPuker, kPregnant, kStalker, kBrute, kUbermorph, kDivider, kDividerPart,
    kTripod, kGuardian, kNest, kBoss, kKindCount, kNone = -1
};
inline const char* KindKey(int k) {
    static const char* const kNames[kKindCount] = {"Slasher", "Pack", "Swarmer", "TubeBeetle", "Leaper", "Lurker", "Exploder", "Infector", "Puker",
                                                   "Pregnant", "Stalker", "Brute", "Ubermorph", "Divider", "DividerPart", "Tripod", "Guardian", "Nest", "Boss"};
    return k >= 0 && k < kKindCount ? kNames[k] : "?";
}
// Bosses and set pieces keep the game's speed unless they have a line of their own.
inline bool FollowsAll(int k) { return k < kTripod; }
constexpr int kMinPercent = 25, kMaxPercent = 300;
inline int ClampPercent(int v) { return v < kMinPercent ? kMinPercent : v > kMaxPercent ? kMaxPercent : v; }

struct Tables {
    int percent[kModeCount][kKindCount];
    Tables() { for (auto& m : percent) for (int& p : m) p = 100; }
    bool Any() const {
        for (const auto& m : percent) for (int p : m) if (p != 100) return true;
        return false;
    }
    float Factor(int mode, int kind) const {
        return mode >= 0 && mode < kModeCount && kind >= 0 && kind < kKindCount ? static_cast<float>(percent[mode][kind]) / 100.0f : 1.0f;
    }
};
// int(section, key, fallback): a kind without its own line takes the table's All.
template <class ReadInt> Tables Load(ReadInt&& read) {
    Tables t;
    for (int m = 0; m < kModeCount; ++m) {
        const int all = ClampPercent(read(ModeSection(m), "All", 100));
        for (int k = 0; k < kKindCount; ++k) t.percent[m][k] = ClampPercent(read(ModeSection(m), KindKey(k), FollowsAll(k) ? all : 100));
    }
    return t;
}

// State names of the enemy's machines (DeadSpaceNPCMainSM, SpawnSM, ExitVentSM, GrappleSM,
// InfectorSM) in which the animation has to fit something else: a scripted sequence, a vent,
// the way into the room, a grab, a corpse being infected, the death.
inline bool KeepsOwnSpeed(const char* state) {
    static const char* const kExact[] = {"scriptedSequence", "onSpawn", "vent", "dead", "playDead", "decompressionPulled", "gravTrap", "standbyForActivate",
                                         "ventSpawn", "ventSpawnExitVent", "animSpawn", "gotoEnterVent", "gotoExitVent", "ventTraverse", "initGrapple",
                                         "postGrappleAnim", "postGrapplePlayerDead", "infectReel", "infectStab", "infectLoopPre", "infectLoopPost", "infectDrop"};
    for (const char* e : kExact) if (std::strcmp(state, e) == 0) return true;
    return std::strncmp(state, "exitVent", 8) == 0 || std::strncmp(state, "docking", 7) == 0;
}
// SetAnimSpeed callers that time an animation against somebody else's (RVA of the return address):
// ScriptedEventSequence::UpdateAnimTime, ResetTargetAnimSpeed and Stop, the paired attack's stages.
inline bool TimedCaller(uint32_t rva) {
    return (rva >= 0xA93350 && rva < 0xA93600) || (rva >= 0xAA4230 && rva < 0xAA4310) || (rva >= 0xAB7540 && rva < 0xAB7780) ||
           (rva >= 0xCD33E0 && rva < 0xCD3600);
}
}  // namespace enemy_speed
