// Dead Space 2 United by Hafer.
#pragma once
#include "policy.h"
#include <string>
#include <map>

namespace ds2 {
// One authored message an enemy sends, carried as the name and the engine's hash
// of it. The field is a string in the archives, so the name is what survives;
// the hash is what a runtime header is likely to hold. See death_message.h.
struct ScriptMessage { const char* name = ""; unsigned hash = 0; };
// The catalog loader and the tests build records positionally up to
// `ambiguous`, so the first fields keep their order.
struct EnemyRecord {
    Guid guid{}, model{};
    std::string family, context;
    bool sourceEnabled = true, targetEnabled = true;
    bool ordinary = false, ambiguous = false;
    unsigned weight = 100;
    std::string arrivalKey;
    std::string crossArrivalKey;
    // The same contract without the vent it points at, so a donor can be
    // rebound to the source's vent when everything else about the arrival
    // is identical.
    std::string ventFreeKey;
    // The same contract once more without the spawn-done message. See
    // compatibleVent: that message rides along with the rebound component, so it
    // does not have to be part of what makes two vent spawns interchangeable.
    std::string ventOpenKey;
    std::string scriptedKey;
    Guid vent{};
    bool privateTemplate = false;
    unsigned ventMask = 0;
    // Where this record was authored. Entities of one archive are loaded
    // together, so a donor sharing the source's archive is the one that is there
    // when the spawn fires; a donor from the same chapter is the next best
    // guess, because a chapter's NPC design streams come in together.
    unsigned archive = 0, chapter = 0;
    // The messages this enemy sends that a level may listen for. A scripted room
    // counts those, so a replacement has to send the source's and not its own.
    ScriptMessage deathMessage{}, healthModifiedMessage{}, spawnDoneMessage{}, awareMessage{};
    // Seen stuck in a vent, or inert after a vent arrival, in an earlier run on
    // this machine. Such a donor is still fine walking in through a door, so it
    // only stops being a vent candidate.
    bool ventBlocked = false;
    // Authored so that it never attacks the player (the intro's psych hall
    // infector has both grapple chances at zero). Still a source, never a donor.
    bool passive = false;
    // Cannot be killed by the player (the Ubermorph regenerates without limit).
    // Such a donor is only offered where nothing waits for the source's death:
    // a source with no authored death message. A room that counts deaths would
    // otherwise wait forever.
    bool unkillable = false;
    // A stat variant: the percentage of spawns it competes in at all, decided
    // per spawn by the seed. 100 for every ordinary record. The name is what
    // the log shows next to the family.
    unsigned chance = 100;
    std::string variantName;
    // Built from another character package (the three Severed twitcher looks, 17.10.).
    bool retargeted = false;
    // The enhanced (super) look of its family, or a copy of such a record (the oracle
    // twitcher is the enhanced slasher's Severed counterpart, 18.10.).
    bool enhanced = false;
    // Runtime factors of a variant, in percent: animation playback (movement
    // follows it) and visual size. 100 leaves the base enemy as it is.
    unsigned speedPercent = 100, scalePercent = 100;
};
enum class SelectionStatus : unsigned {
    Disabled, SourceReadFailed, UnknownSource, NoCandidate, InvalidOriginal,
    DonorUnavailable, PacketReplaced, SourceDisabled, AmbiguousSource,
    UnsupportedClass, UnsupportedArrival, ChanceSkipped, FrameLimit,
    InvalidDonor, DescriptorMismatch, InvalidManager, GlobalPoolUnavailable, SpawnerBlocked,
    StreamProbeBusy, FamilyNotAllowed, OriginalKept, CorpseTemplate, SceneActor, Count
};
inline const char* statusName(unsigned value) {
    static constexpr const char* names[] = {
        "disabled", "source_read_failed", "unknown_source", "no_candidate",
        "invalid_original_packet", "donor_unavailable", "packet_replaced",
        "source_disabled", "ambiguous_source", "unsupported_class",
        "unsupported_arrival", "chance_skipped", "frame_limit",
        "invalid_donor", "descriptor_mismatch", "invalid_manager", "global_pool_unavailable", "spawner_blocked",
        "stream_probe_busy", "family_not_allowed_here", "original_kept", "corpse_template", "scene_actor"
    };
    return value < static_cast<unsigned>(SelectionStatus::Count) ? names[value] : "invalid_status";
}
struct SelectionPlan {
    SelectionStatus status = SelectionStatus::NoCandidate;
    Guid model{};
    std::vector<size_t> donors;
    // diagnostics for the plan dump: the family the draw landed on and how
    // many variant models rolled themselves to the front
    std::string chosenFamily;
    unsigned variantWinners = 0, variantCandidates = 0;
    bool twitcherOnly = false;   // a Severed roll (twitcher or pregnant), not the ordinary chance, made this plan
};
struct PoolSettings {
    bool enabled = false, crossFamily = false;
    uint32_t seed = 1;
    unsigned chance = 100;
    bool ventVariants = false;
    bool crossFamilyVents = false;
    bool ventRebinding = false;
    // Let two vent spawns match even when only their spawn-done messages differ.
    // Requires ventRebinding, which is what makes it safe. See compatibleVent.
    bool ventMessageContract = false;
    bool scriptedArrivals = false;
    bool infectedArrivals = false;
    // Register a replaced enemy against the level's stream rather than the pool
    // it was built from, so a checkpoint restart destroys it like any other.
    bool poolActorsFollowLevel = true;
    // Test aid: a variant by name that wins every draw it is a candidate in
    // (INI VariantBias). Empty for the normal seeded draw.
    std::string preferredVariant;
    // Severed enemies (17.10./18.10.): the percentage of slasher (twitcher) and pregnant
    // sources replaced by a Severed copy of their own family when Chance leaves them alone
    // (0 = only the ordinary draw).
    unsigned twitcherChance = 0, pregnantChance = 0;
};
inline bool directContext(const std::string& context) {
    return context == "default_NONE_descriptor" || context == "SPAWNMODE_NONE";
}
inline bool ventContext(const std::string& context) {
    return context == "SPAWNMODE_VENT|Wall" || context == "SPAWNMODE_VENT|Ceiling" ||
        context == "SPAWNMODE_VENT|default_FLOOR_descriptor" || context == "SPAWNMODE_VENT|Floor";
}
// Mode 4. The corpse takes part in the infection lifecycle, so it carries donor
// requirements that a pure animation contract does not: its own gate.
inline bool infectedContext(const std::string& context) {
    return context == "SPAWNMODE_INFECTED";
}
// Arrivals driven by an authored animation or by the play-dead lifecycle instead of
// a vent. The native constructor rewrites mode 3 to mode 2 with fixed animations, so
// play-dead belongs with the animation contract rather than with direct spawns.
inline bool scriptedContext(const std::string& context) {
    return context == "SPAWNMODE_ANIM" || context == "SPAWNMODE_ZOMBIE_PLAYDEAD" ||
        infectedContext(context);
}
// scriptedKey is the hash of the whole authored DSNPCSpawningComponent: mode, both
// animation names, animation options, activation flags, dismemberment exit zones and
// every activation/completion message. It is only generated for classes that are
// ordinary swap candidates, and scripted records never reference a spawning vent, so
// the key is placement independent.
inline bool scriptedArrival(const PoolSettings& settings, const EnemyRecord& record) {
    return !record.scriptedKey.empty() && scriptedContext(record.context) &&
        (infectedContext(record.context) ? settings.infectedArrivals : settings.scriptedArrivals);
}
// Equal authored contracts prove the donor model was authored to play exactly these
// spawn animations. Cross-family scripted arrivals stay refused: every authored spawn
// animation name in the corpus is species specific, so no donor evidence exists.
inline bool compatibleScripted(const PoolSettings& settings, const EnemyRecord& source,
                               const EnemyRecord& target) {
    return scriptedArrival(settings, source) && scriptedArrival(settings, target) &&
        source.family == target.family && source.context == target.context &&
        source.scriptedKey == target.scriptedKey;
}
// What a vent arrival needs, in the order the evidence supports it.
//
// The climb-out belongs to the species, not to the vent: a wall vent carries no
// animation, the entity does, through its own <Species>VentComponent. A donor
// can therefore only ever come out of a vent if it is itself an authored vent
// arrival. Every donor accepted below is one, which is why the direct-arrival
// branch that used to live here is gone: it accepted a walk-in model whenever
// its ventMask carried the orientation, and that mask only says the model
// appears at vents somewhere in the game. Ten of thirteen enemies stranded in a
// friend's session had arrived that way.
//
// What is left of the contract is small. A vent spawn's DSNPCSpawningComponent
// holds the mode, the activation flags and the GUID of the vent it uses, and
// nothing else. So two spawns of different species differ in which vent they sit
// on and in nothing else, which is exactly what rebinding fixes, by pointing
// the donor at the source's vent. `ventFreeKey` is that contract with the vent
// pointer removed, and comparing it is the honest test.
inline bool compatibleVent(const PoolSettings& settings, const EnemyRecord& source,
                           const EnemyRecord& target) {
    if (!settings.ventVariants || !ventContext(source.context) || source.context != target.context)
        return false;
    if (target.ventBlocked) return false;
    const bool familyAllowed = source.family == target.family ||
        (settings.crossFamily && settings.crossFamilyVents);
    if (settings.ventRebinding && familyAllowed && source.vent != Guid{} && target.vent != Guid{} &&
        !source.ventFreeKey.empty() && source.ventFreeKey == target.ventFreeKey)
        return true;
    // The same contract with the spawn-done message left out of the comparison.
    //
    // That message is what a level script waits for: a door, the next wave, the
    // end of a quarantine. So it was part of the contract: only a donor authored
    // with the very same message could be trusted to send it. It is also the only
    // message field a spawning component ever carries, and it alone holds back 26
    // of the 35 vent spawns no donor can reach. Three leapers stand together in
    // the med storage and two of them are unreachable for this single field.
    //
    // Leaving it out is safe precisely because the rebind hands the replacement
    // the *source's* spawning component: prepareVentRebinding puts the source's
    // reference into the donor's component list, so the vent, the activation
    // flags and the message are the level's own by construction, not by a write
    // that could be refused. Hence the ventRebinding requirement below. Without
    // it the donor would arrive through its own component and send its own
    // message, which is the hang this key was invented to prevent.
    if (settings.ventMessageContract && settings.ventRebinding && familyAllowed &&
        source.vent != Guid{} && target.vent != Guid{} &&
        !source.ventOpenKey.empty() && source.ventOpenKey == target.ventOpenKey)
        return true;
    // Without rebinding the donor arrives through its own authored vent, so the
    // contract has to match including which vent that is.
    if (source.family == target.family)
        return !source.arrivalKey.empty() && source.arrivalKey == target.arrivalKey;
    return settings.crossFamily && settings.crossFamilyVents && !source.crossArrivalKey.empty() &&
        source.crossArrivalKey == target.crossArrivalKey;
}
// True when the pair only qualifies with the message left out, which is what the
// runtime checks the rebind for before it commits the packet.
inline bool ventMessageRelaxed(const PoolSettings& settings, const EnemyRecord& source,
                               const EnemyRecord& target) {
    return settings.ventMessageContract && !source.ventOpenKey.empty() &&
        source.ventOpenKey == target.ventOpenKey &&
        source.ventFreeKey != target.ventFreeKey;
}
inline SelectionPlan buildPlan(const PoolSettings& settings, const EnemyRecord& source,
                              const std::vector<EnemyRecord>& pool, unsigned maxDonors) {
    SelectionPlan plan;
    auto reject = [&](SelectionStatus status) { plan.status = status; return plan; };
    if (!settings.enabled) return reject(SelectionStatus::Disabled);
    if (!source.sourceEnabled) return reject(SelectionStatus::SourceDisabled);
    if (source.ambiguous) return reject(SelectionStatus::AmbiguousSource);
    const bool scripted = scriptedArrival(settings, source);
    if (!source.ordinary && !scripted) return reject(SelectionStatus::UnsupportedClass);
    const bool vent = settings.ventVariants && ventContext(source.context) && !source.arrivalKey.empty();
    if (!directContext(source.context) && !vent && !scripted) return reject(SelectionStatus::UnsupportedArrival);
    const bool chanceHit = stableHash(settings.seed, source.guid, source.model, 0x43484E43u) % 100 < std::min(settings.chance, 100u);
    // A slasher or pregnant the ordinary draw leaves alone may still become its Severed
    // counterpart; then only retargeted copies of its own family are candidates.
    const unsigned severedChance = source.family == "Zombie" ? settings.twitcherChance : source.family == "Pregnant" ? settings.pregnantChance : 0;
    const bool twitcherOnly = !chanceHit && severedChance &&
        stableHash(settings.seed, source.guid, source.model, 0x54574954u) % 100 < std::min(severedChance, 100u);
    if (!chanceHit && !twitcherOnly) return reject(SelectionStatus::ChanceSkipped);
    plan.twitcherOnly = twitcherOnly;
    std::map<Guid, std::vector<size_t>> variants;
    for (size_t i = 0; i < pool.size(); ++i) {
        const auto& candidate = pool[i];
        // A scripted source never draws from the direct or vent pools; compatibleScripted
        // carries its own class gate through the donor's own scriptedKey.
        const bool arrival = scripted ? compatibleScripted(settings, source, candidate) :
            candidate.ordinary && (vent ? compatibleVent(settings, source, candidate) :
                                          directContext(candidate.context));
        // A variant's chance is applied inside its family below, not here: as a
        // candidate gate it left the variant one model among twenty (13.09.).
        // A Severed copy stands only where its own tier stood: the oracle twitcher for an
        // enhanced slasher, the security and classic ones for a plain slasher.
        const bool tierMismatch = candidate.retargeted && candidate.family == source.family && candidate.enhanced != source.enhanced;
        const bool variantSkipped = candidate.chance == 0 || tierMismatch ||
            (twitcherOnly && (!candidate.retargeted || candidate.family != source.family));
        // An enemy that never dies may not stand where a death is counted.
        const bool deathAwaited = source.deathMessage.hash != 0 || source.healthModifiedMessage.hash != 0;
        if (candidate.targetEnabled && candidate.weight && arrival && !candidate.ambiguous &&
            candidate.model != Guid{} &&
            candidate.model != source.model && candidate.guid != source.guid &&
            (settings.crossFamily || candidate.family == source.family) &&
            !(candidate.unkillable && deathAwaited) && !variantSkipped)
            variants[candidate.model].push_back(i);
    }
    if (variants.empty() || !maxDonors) return plan;
    // The species is chosen first, then a model of that species.
    //
    // Weighting the models directly made the roster decide the outcome: the pool
    // carries 31 Zombie vent donors against 5 Puker and 2 Lurker, and a slasher
    // has far more variants than anything else, so a seeded pick over models
    // landed on another slasher most of the time. A run showed it plainly: 17 of
    // 23 replacements were Zombie for Zombie. Picking the family first makes a
    // Puker as likely as a slasher, and [Weights] still says how likely each
    // family is.
    std::map<std::string, std::vector<Guid>> byFamily;
    for (const auto& variant : variants)
        byFamily[pool[variant.second.front()].family].push_back(variant.first);
    uint64_t totalWeight = 0;
    for (const auto& family : byFamily)
        totalWeight += std::min(pool[variants.at(family.second.front()).front()].weight, 1000u);
    if (!totalWeight) return plan;
    uint64_t choice = stableHash(settings.seed, source.guid, source.model, 0x46414D31u) % totalWeight;
    const std::string* preferredFamily = nullptr;
    for (const auto& family : byFamily) {
        const unsigned weight = std::min(pool[variants.at(family.second.front()).front()].weight, 1000u);
        if (choice >= weight) { choice -= weight; continue; }
        preferredFamily = &family.first;
        break;
    }
    if (!preferredFamily) return plan;

    // Inside a family the seed decides which variant leads, and the families
    // behind it stay as fallbacks in a seeded order, so a chapter that cannot
    // supply the preferred species still gets a replacement.
    auto seededLess = [&](const Guid& a, const Guid& b) {
        const auto ha = stableHash(settings.seed, source.guid, a, 0x4D4F4432u);
        const auto hb = stableHash(settings.seed, source.guid, b, 0x4D4F4432u);
        return ha != hb ? ha < hb : a < b;
    };
    std::vector<std::string> families;
    families.reserve(byFamily.size());
    for (const auto& family : byFamily)
        if (family.first != *preferredFamily) families.push_back(family.first);
    std::sort(families.begin(), families.end(), [&](const std::string& a, const std::string& b) {
        const auto ha = stableHash(settings.seed, source.guid, byFamily.at(a).front(), 0x46414D32u);
        const auto hb = stableHash(settings.seed, source.guid, byFamily.at(b).front(), 0x46414D32u);
        return ha != hb ? ha < hb : a < b;
    });
    families.insert(families.begin(), *preferredFamily);

    std::vector<Guid> models;
    models.reserve(variants.size());
    for (const auto& family : families) {
        auto& entries = byFamily.at(family);
        std::sort(entries.begin(), entries.end(), seededLess);
        models.insert(models.end(), entries.begin(), entries.end());
    }
    // A variant's chance is its share of its family's spawns: inside every
    // family's block, a variant model that rolls under its chance leads that
    // family (chance 100 = every spawn that becomes this family, 25 = one in
    // four). Done per family, because the draw may land on another family
    // first and fall back to this one when its donors are away (13.09.: a
    // pack spawn drew Stalker, fell back to Zombie, and the Berserker sat
    // behind two plain slashers).
    const std::string chosenFamily = pool[variants.at(models.front()).front()].family;
    plan.chosenFamily = chosenFamily;
    {
        std::vector<Guid> reordered;
        reordered.reserve(models.size());
        for (const auto& family : families) {
            std::vector<Guid> block, winners;
            for (const auto& model : models)
                if (pool[variants.at(model).front()].family == family) block.push_back(model);
            for (const auto& model : block) {
                const auto& candidate = pool[variants.at(model).front()];
                if (candidate.variantName.empty()) continue;
                ++plan.variantCandidates;
                if (stableHash(settings.seed, source.guid, candidate.guid, 0x56415249u) % 100 < candidate.chance)
                    winners.push_back(model);
            }
            std::sort(winners.begin(), winners.end(), seededLess);
            plan.variantWinners += static_cast<unsigned>(winners.size());
            for (const auto& model : winners) reordered.push_back(model);
            // A variant that lost its roll goes behind the family's plain
            // models, so the chance is its exact share whatever the family's
            // size; keeping it in the seeded order made a 50 read as 75 in a
            // two-model family.
            std::vector<Guid> losers;
            for (const auto& model : block) {
                if (std::find(winners.begin(), winners.end(), model) != winners.end()) continue;
                if (pool[variants.at(model).front()].variantName.empty()) reordered.push_back(model);
                else losers.push_back(model);
            }
            for (const auto& model : losers) reordered.push_back(model);
        }
        models = reordered;
    }
    // VariantBias: the named variant leads the list when it is a candidate of
    // the source's own family. Not across families: with the bias every swarm
    // spawn became a Berserker that never targeted the player (13.09., 33 of
    // 35 came from SwarmEnemy sources).
    if (!settings.preferredVariant.empty()) {
        for (const auto& variant : variants) {
            if (pool[variant.second.front()].variantName != settings.preferredVariant ||
                pool[variant.second.front()].family != chosenFamily) continue;
            auto it = std::find(models.begin(), models.end(), variant.first);
            if (it != models.end()) { models.erase(it); models.insert(models.begin(), variant.first); }
            break;
        }
    }
    plan.model = models.front();

    // A donor is worth a lookup when it can be loaded at all: the private pool
    // is resident by construction, and an entity from the source's own archive
    // comes with it. Everything else is authored in some other room and the
    // native lookup returns nothing, which is what burned all eight lookups per
    // vent spawn before this: five vent spawns in a row reported
    // donor_unavailable with attempts=8 and donors from four other chapters.
    // 0 is certain, 1 is likely, 2 is somewhere else in the game. Ordering by
    // this is what makes the lookup budget land on a donor that exists: across
    // the 392 authored vent spawns, 282 have a donor in their own archive and
    // 339 have one in their chapter, against 35 with none at all.
    const auto locality = [&](size_t index) -> unsigned {
        const auto& candidate = pool[index];
        if (candidate.privateTemplate) return 0;
        if (source.archive && candidate.archive == source.archive) return 0;
        if (source.chapter && candidate.chapter == source.chapter) return 1;
        return 2;
    };
    for (const auto& model : models) {
        auto& entries = variants.at(model);
        std::sort(entries.begin(), entries.end(), [&](size_t a, size_t b) {
            if (locality(a) != locality(b)) return locality(a) < locality(b);
            if (pool[a].privateTemplate != pool[b].privateTemplate) return pool[a].privateTemplate;
            const auto ha = stableHash(settings.seed, source.guid, pool[a].guid, 0x444F4E33u);
            const auto hb = stableHash(settings.seed, source.guid, pool[b].guid, 0x444F4E33u);
            return ha != hb ? ha < hb : pool[a].guid < pool[b].guid;
        });
    }
    // Models whose best donor can be reached go first. The seeded order inside
    // each group survives, so the same seed still prefers the same species where
    // it has the choice.
    std::stable_sort(models.begin(), models.end(), [&](const Guid& a, const Guid& b) {
        const auto& ea = variants.at(a);
        const auto& eb = variants.at(b);
        const unsigned la = ea.empty() ? 3u : locality(ea.front());
        const unsigned lb = eb.empty() ? 3u : locality(eb.front());
        return la < lb;
    });
    // Round robin across models, best donor of each first. Donors of one model
    // share its resources, so if that model is not resident every one of them
    // fails the same way; spending the lookup budget on a second model instead
    // is what makes a run recover.
    for (size_t depth = 0; plan.donors.size() < maxDonors; ++depth) {
        bool progressed = false;
        for (const auto& model : models) {
            if (plan.donors.size() >= maxDonors) break;
            const auto& entries = variants.at(model);
            if (depth >= entries.size()) continue;
            plan.donors.push_back(entries[depth]);
            progressed = true;
        }
        if (!progressed) break;
    }
    // Donor availability is evaluated at the native spawn call, not cached here.
    plan.status = SelectionStatus::DonorUnavailable;
    return plan;
}
}
