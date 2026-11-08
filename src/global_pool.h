// Dead Space 2 United by Hafer.
#pragma once
#include <cstring>
#include "selection.h"
#include "generated_global_targets.h"
#include "generated_vent_donors.h"
#include "generated_playdead_donors.h"
#include "generated_direct_donors.h"
#include "generated_boss_donors.h"
#include "generated_variant_donors.h"

namespace ds2 {
// The enhanced looks: the models of the pool's super_* targets (super_slasher EC703B0F and the rest).
inline bool enhancedModel(const Guid& model) {
    if (model == Guid{}) return false;
    for (const auto& row : kGlobalTargets)
        if (row.model == model && std::strncmp(row.package, "super_", 6) == 0) return true;
    return false;
}
// Build once before hooks are installed. Plan indices require a stable pool.
inline bool appendGlobalTargets(std::vector<EnemyRecord>& pool) {
    auto combined = pool;
    for (const auto& row : kGlobalTargets) {
        const EnemyRecord* source = nullptr;
        for (const auto& record : pool) {
            if (record.guid == row.guid) return false;
            if (record.guid != row.source) continue;
            if (source) return false;
            source = &record;
        }
        if (!source || source->ambiguous || source->model != row.model ||
            source->family != row.family || source->context != row.context || row.authored)
            return false;
        EnemyRecord target = *source;
        target.guid = row.guid;
        target.sourceEnabled = false;
        target.privateTemplate = true;
        target.ventMask = row.ventMask;
        target.vent = {};
        target.arrivalKey.clear();
        target.crossArrivalKey.clear();
        target.scriptedKey.clear();
        combined.push_back(std::move(target));
    }
    std::sort(combined.begin(), combined.end(), [](const auto& a, const auto& b) {
        return a.guid < b.guid;
    });
    for (size_t i = 1; i < combined.size(); ++i)
        if (combined[i - 1].guid == combined[i].guid) return false;
    pool.swap(combined);
    return true;
}
// The pool's vent arrivals. A vent climb-out belongs to the species, through its
// own <Species>VentComponent, so a donor that walks in cannot serve a vent no
// matter what its ventMask says. These entities are authored vent arrivals,
// copied into the pool with their contract intact, which is what lets a vent
// spawn anywhere in the game draw from the whole roster instead of from the two
// or three species the current chapter happens to carry.
//
// The record is built from the entity it was copied from, so model, family and
// orientation are the catalog's and not a second source of truth. Only the
// identity, the vent it names and the contract come from the row.
inline bool appendVentDonors(std::vector<EnemyRecord>& pool) {
    auto combined = pool;
    for (const auto& row : kVentDonors) {
        const EnemyRecord* source = nullptr;
        for (const auto& record : pool) {
            if (record.guid == row.guid) return false;
            if (record.guid != row.source) continue;
            if (source) return false;
            source = &record;
        }
        if (!source || source->ambiguous || !ventContext(source->context) ||
            source->ventFreeKey != row.ventFree)
            return false;
        EnemyRecord target = *source;
        target.guid = row.guid;
        target.sourceEnabled = false;
        target.privateTemplate = true;
        target.vent = row.vent;
        // Its own vent is not the one it will use, so the vent-bound keys are
        // cleared: this donor only ever qualifies through a rebind. The two
        // rebind keys stay, they are what the rebind is matched on.
        target.arrivalKey.clear();
        target.crossArrivalKey.clear();
        target.scriptedKey.clear();
        combined.push_back(std::move(target));
    }
    std::sort(combined.begin(), combined.end(), [](const auto& a, const auto& b) {
        return a.guid < b.guid;
    });
    for (size_t i = 1; i < combined.size(); ++i)
        if (combined[i - 1].guid == combined[i].guid) return false;
    pool.swap(combined);
    return true;
}
// The pool's play-dead arrivals: enemies that lie on the floor as a corpse and
// get up when the player comes close. The pool never carried one, so a corpse
// that got up had no candidate at all and stayed whatever the level authored.
//
// A vent donor has its contract cleared, because the vent it names is not the
// one it will use. Here the opposite is true: `compatibleScripted` matches on
// the scriptedKey, the hash over the whole authored spawning component, both
// animation names, the options, the flags and every message. That key is the
// only evidence that the donor's model can play exactly these animations, so it
// is kept and checked against the row.
inline bool appendPlaydeadDonors(std::vector<EnemyRecord>& pool) {
    auto combined = pool;
    for (const auto& row : kPlaydeadDonors) {
        const EnemyRecord* source = nullptr;
        for (const auto& record : pool) {
            if (record.guid == row.guid) return false;
            if (record.guid != row.source) continue;
            if (source) return false;
            source = &record;
        }
        if (!source || source->ambiguous || !scriptedContext(source->context) ||
            source->scriptedKey != row.scripted)
            return false;
        EnemyRecord target = *source;
        target.guid = row.guid;
        target.sourceEnabled = false;
        target.privateTemplate = true;
        target.vent = {};
        // The vent-bound keys never applied to a corpse; the scripted one stays.
        target.arrivalKey.clear();
        target.crossArrivalKey.clear();
        combined.push_back(std::move(target));
    }
    std::sort(combined.begin(), combined.end(), [](const auto& a, const auto& b) {
        return a.guid < b.guid;
    });
    for (size_t i = 1; i < combined.size(); ++i)
        if (combined[i - 1].guid == combined[i].guid) return false;
    pool.swap(combined);
    return true;
}
// A direct arrival that stands in for one of the pool's own roots.
//
// The roots were picked one per character stream, without asking what kind of
// encounter each was authored for, and one of them turned out to be a
// scripted-event actor: the pack boy from the crash room hallway, the only
// scripted-event donor among all 108. Its level script does not exist around it
// any more, and it walks about without ever attacking.
//
// A stream can only be appended to, so the root is not removed. It stays in the
// file and stops being offered, which is what `supersedes` names.
inline bool appendDirectDonors(std::vector<EnemyRecord>& pool) {
    auto combined = pool;
    for (const auto& row : kDirectDonors) {
        const EnemyRecord* source = nullptr;
        EnemyRecord* superseded = nullptr;
        for (auto& record : combined) {
            if (record.guid == row.guid) return false;
            if (record.guid == row.supersedes) {
                if (superseded) return false;
                superseded = &record;
            }
            if (record.guid != row.source) continue;
            if (source) return false;
            source = &record;
        }
        // Both ends have to be there: the entity to copy and the root it stands
        // in for. Half of the exchange would leave the pool worse than before.
        if (!source || !superseded || source->ambiguous ||
            source->context != row.context || source->family != superseded->family ||
            !superseded->privateTemplate)
            return false;
        superseded->targetEnabled = false;
        EnemyRecord target = *source;
        target.guid = row.guid;
        target.sourceEnabled = false;
        target.privateTemplate = true;
        target.ventMask = superseded->ventMask;
        target.vent = {};
        target.arrivalKey.clear();
        target.crossArrivalKey.clear();
        target.scriptedKey.clear();
        combined.push_back(std::move(target));
    }
    std::sort(combined.begin(), combined.end(), [](const auto& a, const auto& b) {
        return a.guid < b.guid;
    });
    for (size_t i = 1; i < combined.size(); ++i)
        if (combined[i - 1].guid == combined[i].guid) return false;
    pool.swap(combined);
    return true;
}

// The Divider and the Ubermorph, appended with their dependencies.
//
// The core spawns its five parts from templates that were copied and remapped
// next to it, so a part born from a pool core is a pool entity too and the
// create hook hands it the level's handle like any other child. The Ubermorph
// cannot be killed, so its record is marked as such and the plan keeps it away
// from any source whose death a level might count.
inline bool appendBossDonors(std::vector<EnemyRecord>& pool) {
    auto combined = pool;
    for (const auto& row : kBossDonors) {
        const EnemyRecord* source = nullptr;
        for (const auto& record : pool) {
            if (record.guid == row.guid) return false;
            if (record.guid != row.source) continue;
            if (source) return false;
            source = &record;
        }
        if (!source || source->ambiguous || source->family != row.family ||
            !directContext(source->context))
            return false;
        EnemyRecord target = *source;
        target.guid = row.guid;
        target.sourceEnabled = false;
        target.privateTemplate = true;
        // Hunter: never dies. DividerCore: its "death" is the split into
        // five parts, and in the field the parts did not always come out
        // (15.09.), so a door waiting for that kill stayed shut. Both
        // are kept away from any source whose death a script waits for.
        target.unkillable = target.family == "Hunter" || target.family == "DividerCore";
        target.vent = {};
        target.arrivalKey.clear();
        target.crossArrivalKey.clear();
        target.scriptedKey.clear();
        combined.push_back(std::move(target));
    }
    std::sort(combined.begin(), combined.end(), [](const auto& a, const auto& b) {
        return a.guid < b.guid;
    });
    for (size_t i = 1; i < combined.size(); ++i)
        if (combined[i - 1].guid == combined[i].guid) return false;
    pool.swap(combined);
    return true;
}

// Stat variants: a copy of a source with authored numbers changed. The copy
// shares the base enemy's model, so for the family draw it gets a model key of
// its own, folded from the base model and the variant name; otherwise it would
// vanish into the base model's slot and never be picked on its own.
// Why the last appendVariantDonors said no, for the log.
inline std::string variantDonorFailure;
// A retargeted variant is built from another character package (the twitcher on
// slasher records, 17.10.). All its copies share that package's MetaModel as the
// model key, so the family draw rolls its chance once, not once per copy. One
// that cannot be appended is left out on its own instead of failing the pool.
inline bool rowRetargeted(const VariantDonorRow& row) { return row.package && row.package[0]; }
inline unsigned retargetRowsSkipped = 0;
inline std::string retargetSkipReason;
inline bool appendVariantDonors(std::vector<EnemyRecord>& pool, bool statVariants = true, bool retargets = true) {
    auto combined = pool;
    variantDonorFailure.clear();
    retargetRowsSkipped = 0;
    retargetSkipReason.clear();
    for (const auto& row : kVariantDonors) {
        if (row.guid == Guid{}) continue;   // the placeholder of a build without variants
        const bool retarget = rowRetargeted(row);
        if (retarget ? !retargets : !statVariants) continue;
        const EnemyRecord* source = nullptr;
        for (const auto& record : pool) {
            if (record.guid == row.guid) { variantDonorFailure = std::string(row.name) + ": variant guid already in the pool"; return false; }
            if (record.guid != row.source) continue;
            if (source) { variantDonorFailure = std::string(row.name) + ": source twice in the pool"; return false; }
            source = &record;
        }
        // a vent spawn record works as a source too: the copy keeps its
        // spawning component and vent contract, so it competes for vent
        // spawns the way a direct copy competes for direct ones (13.09.)
        if (!source || source->ambiguous || source->family != row.family ||
            !(directContext(source->context) || ventContext(source->context) || scriptedContext(source->context)) || row.chance > 100 ||
            (retarget && row.model == Guid{})) {
            const std::string why = std::string(row.name) + (!source ? ": source not in the pool" : source->ambiguous ? ": source ambiguous" :
                source->family != row.family ? ": family mismatch" : row.chance > 100 ? ": chance" : retarget && row.model == Guid{} ? ": no model" :
                ": context " + source->context);
            if (retarget) {
                ++retargetRowsSkipped;
                if (retargetSkipReason.empty()) retargetSkipReason = why;
                continue;
            }
            variantDonorFailure = why;
            return false;
        }
        EnemyRecord target = *source;
        target.guid = row.guid;
        target.model = retarget ? row.model : Guid{source->model[0] ^ row.guid[1], source->model[1] ^ row.guid[1], source->model[2], source->model[3]};
        target.sourceEnabled = false;
        target.privateTemplate = true;
        target.chance = row.chance;
        target.variantName = row.name;
        target.retargeted = retarget;
        target.speedPercent = row.speed;
        target.scalePercent = row.scale;
        // the copy keeps the source's vent and scripted contracts: a variant
        // of a vent record competes for vent spawns with that contract
        // (clearing them left the copy out of every vent draw, 13.09.)
        combined.push_back(std::move(target));
    }
    std::sort(combined.begin(), combined.end(), [](const auto& a, const auto& b) {
        return a.guid < b.guid;
    });
    for (size_t i = 1; i < combined.size(); ++i)
        if (combined[i - 1].guid == combined[i].guid) { variantDonorFailure = "duplicate guid after append"; return false; }
    pool.swap(combined);
    return true;
}
}
