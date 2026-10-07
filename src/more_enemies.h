#pragma once
#include "selection.h"
#include <cstdint>
#include <map>
#include <string>

// Spawn multiplier. It repeats an authored FuncSpawn arrival that has already
// completed, which is the same operation the engine performs for its own
// queued retry: build a message carrying Attributes+0x00 and dispatch it
// through the object's own HandleEvents. Nothing here selects a donor, so the
// multiplier never depends on the private global pool.
namespace ds2 {

struct MoreEnemiesSettings {
    bool enabled = false;
    // Vent arrivals play an authored climb-out animation. Repeating one starts
    // a second animation in the same vent, which is why it is separate.
    bool vents = false;
    // Every repeat runs SpawnDone, which resends the authored per-spawn
    // completion message. Level logic counts those, so they are hidden by
    // default and the multiplier refuses to run when they cannot be hidden.
    bool suppressCompletion = true;
    unsigned defaultTotal = 1;
    unsigned perTrigger = 3;
    unsigned budgetPerSecond = 12;
    // A replacement from another family: multiplied up to the smaller of the
    // two families' totals (true), or never (false). A swarmer slot is never
    // multiplied into another family either way, see decideMoreEnemies.
    bool crossFamily = true;
};

// Absolute limits. Configuration cannot raise these.
inline constexpr unsigned kMoreEnemiesMaxPerTrigger = 8;
// A window is sixteen arrivals (kMoreEnemiesWindowArrivals), so this is the
// budget for every arrival in the window at the largest total: 16 x (8 - 1).
// 19.09.: with the old 64 and a total of 4 (three extras each) only four of
// sixteen arrivals were multiplied ("MoreEnemies does nothing").
inline constexpr unsigned kMoreEnemiesMaxPerSecond = 112;

enum class MoreEnemiesStatus : unsigned {
    Disabled, Reentrant, AttributesUnreadable, UnknownTarget, UnsupportedArrival,
    ProtectedFamily, MultiplierOne, BudgetExhausted, CompletionNotWritable,
    CountNotWritable, VtableMismatch, CrossFamilyRefused, SwarmSlot, Multiplied, CorpseTemplate, Count
};

inline const char* moreEnemiesStatusName(unsigned value) {
    static constexpr const char* names[] = {
        "disabled", "reentrant", "attributes_unreadable", "unknown_target",
        "unsupported_arrival", "protected_family", "multiplier_one",
        "budget_exhausted", "completion_message_not_writable",
        "spawn_count_not_writable", "handle_events_vtable_mismatch",
        "cross_family_refused", "swarm_slot", "multiplied", "corpse_template"
    };
    return value < static_cast<unsigned>(MoreEnemiesStatus::Count) ? names[value]
                                                                  : "invalid_status";
}

// Families that stay at one no matter what the configuration asks for. Boss and
// encounter actors, actors anchored to a wall or a floor plate, and the parts
// another actor owns and counts.
inline bool moreEnemiesProtectedFamily(const std::string& family) {
    static const char* const kProtected[] = {
        "Hunter", "DividerCore", "DividerPart", "Nest", "NestProjectile",
        "Guardian", "GuardianPod", "GuardianTentacle", "Landmine",
        "Tripod", "WindowTripod", "BossTripod", "TripodProjectile",
        "Kong", "NicoleBoss", "Polyp", "DragTentacle", "SlamDragTentacle",
        "LeviathanTentacle", "SlugTentacle", "StabTentacle"
    };
    for (const char* entry : kProtected)
        if (family == entry) return true;
    return false;
}

// Only arrivals whose placement is repeatable. A scripted animation arrival,
// the play-dead lifecycle and the infection lifecycle each own one authored
// spot and one authored animation, so a second copy would overlap the first.
inline bool moreEnemiesSupportedContext(const MoreEnemiesSettings& settings,
                                        const std::string& context) {
    if (directContext(context)) return true;
    return settings.vents && ventContext(context);
}

inline unsigned moreEnemiesTotalFor(const MoreEnemiesSettings& settings,
                                    const std::map<std::string, unsigned>& perFamily,
                                    const std::string& family) {
    if (moreEnemiesProtectedFamily(family)) return 1;
    const auto found = perFamily.find(family);
    unsigned total = found == perFamily.end() ? settings.defaultTotal : found->second;
    if (total < 1) total = 1;
    const unsigned ceiling = std::min(settings.perTrigger, kMoreEnemiesMaxPerTrigger);
    return std::min(total, ceiling < 1 ? 1u : ceiling);
}

// A window is a run of arrivals, not a second of wall clock.
//
// It used to be a rolling second off GetTickCount, and that made the result
// depend on the machine: whether two arrivals fall inside the same second is a
// question of load times and frame rate, so two players on the same seed could
// get a different number of extra enemies in the same room. Counting arrivals
// instead keeps the same protection (a burst still runs into the ceiling) and
// two runs that reach the same arrivals in the same order now agree.
constexpr unsigned kMoreEnemiesWindowArrivals = 16;
struct MoreEnemiesBudget {
    unsigned arrivals = 0;
    unsigned used = 0;
    bool started = false;
};

inline unsigned moreEnemiesGrant(MoreEnemiesBudget& budget, unsigned perWindow, unsigned wanted) {
    const unsigned limit = std::min(perWindow, kMoreEnemiesMaxPerSecond);
    if (!budget.started || budget.arrivals >= kMoreEnemiesWindowArrivals) {
        budget.started = true;
        budget.arrivals = 0;
        budget.used = 0;
    }
    ++budget.arrivals;
    if (budget.used >= limit) return 0;
    const unsigned room = limit - budget.used;
    const unsigned granted = std::min(wanted, room);
    budget.used += granted;
    return granted;
}

struct MoreEnemiesDecision {
    MoreEnemiesStatus status = MoreEnemiesStatus::Disabled;
    unsigned extras = 0;
};

// One authored arrival has just completed. Returns how many repeats it earns.
// `donorFamily` is the family that actually stands there when the randomizer
// replaced the packet; empty (or equal to `family`) means the authored enemy.
//
// The family that decides the total is the one that will stand there. A room
// authored for a dozen swarmers, each slot replaced by a stalker or a puker
// and each replacement repeated four times, is how a session ends: dump 42512
// (19.09.2026) died inside a stalker's spawn-finalize with twelve stalkers and
// pukers standing in one swarmer room. So a replacement of another family is
// held to the smaller of the two families' totals, and a swarmer slot is
// never multiplied into anything but swarmers.
inline MoreEnemiesDecision decideMoreEnemies(const MoreEnemiesSettings& settings,
                                             const std::map<std::string, unsigned>& perFamily,
                                             const std::string& family,
                                             const std::string& context,
                                             MoreEnemiesBudget& budget,
                                             const std::string& donorFamily = std::string()) {
    if (!settings.enabled) return {MoreEnemiesStatus::Disabled, 0};
    if (!moreEnemiesSupportedContext(settings, context))
        return {MoreEnemiesStatus::UnsupportedArrival, 0};
    if (moreEnemiesProtectedFamily(family))
        return {MoreEnemiesStatus::ProtectedFamily, 0};
    // A swarmer slot is never multiplied, whatever stands there: the room
    // authored them by the dozen already (19.09.2026). Replacing them
    // is untouched; a swarmer that replaces something else gets its horde.
    if (family == "SwarmEnemy") return {MoreEnemiesStatus::SwarmSlot, 0};
    unsigned total = moreEnemiesTotalFor(settings, perFamily, family);
    const bool replaced = !donorFamily.empty() && donorFamily != family;
    if (replaced) {
        if (moreEnemiesProtectedFamily(donorFamily))
            return {MoreEnemiesStatus::ProtectedFamily, 0};
        if (!settings.crossFamily)
            return {MoreEnemiesStatus::CrossFamilyRefused, 0};
        total = std::min(total, moreEnemiesTotalFor(settings, perFamily, donorFamily));
    }
    if (total <= 1) return {MoreEnemiesStatus::MultiplierOne, 0};
    const unsigned granted = moreEnemiesGrant(budget, settings.budgetPerSecond, total - 1);
    if (!granted) return {MoreEnemiesStatus::BudgetExhausted, 0};
    return {MoreEnemiesStatus::Multiplied, granted};
}
}
