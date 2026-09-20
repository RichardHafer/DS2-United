ds2::PoolSettings settings;
std::vector<ds2::EnemyRecord> pool;
std::vector<ds2::SelectionPlan> plans;
std::vector<ds2::SelectionPlan> allowedSpawnerPlans;
safetyhook::MidHook selectionHook;
unsigned maxDonorLookups = 8;
bool globalTemplates = false;
std::string poolStageFailed;   // which append step switched the global templates off
std::string planDumpLine;      // PlanDump=<guid>: that source's donor order
// Vent swaps that only qualified with the spawn-done message left out.
volatile LONG relaxedVentSwaps = 0;
// The stream handle of the last packet a spawner looked up. Every spawner
// packet names the stream the level spawns from, and that is the handle the
// create hook in runtime_pool_ownership.inc gives to the packets a pool
// enemy builds on its own.
volatile LONG lastLevelHandle = 0;
extern volatile LONG streamSelectionAllowed;
struct SelectionFrame {
    uintptr_t stack = 0;
    unsigned status = 0;
    Guid guid{};
    unsigned attempts = 0, bindResult = 0, bindingInstance = 0;
    unsigned levelHandle = 0, donorHandle = 0;
    Guid original{};
    ds2::native::ConstructionBinding binding{};
    bool applied = false;
};
thread_local SelectionFrame selectionFrames[16];
// Extra spawns (runtime_more_enemies.inc) drive a spawner once more with the
// donor already decided: OnLookup takes it instead of the plan, OnCreated
// reports the actor so the caller can place it.
thread_local size_t forcedDonorIndex = SIZE_MAX;
thread_local bool extraSpawnActive = false;
thread_local uint32_t lastCreatedActor = 0;
safetyhook::MidHook bindingOwnerHook, bindingConstructorHook;

void OnBindingOwner(SafetyHookContext& ctx) {
    uint32_t args[4]{};
    if (!Read(ctx.esp, args, sizeof(args))) return;
    for (auto& frame : selectionFrames) {
        if (!frame.binding.packet || frame.binding.packet != ctx.ebp) continue;
        frame.bindResult = static_cast<unsigned>(frame.binding.bindOwner(args[0] - base, ctx.ebp, args[3], args[1]));
        return;
    }
}

void OnBindingConstructor(SafetyHookContext& ctx) {
    uint32_t args[3]{};
    if (!Read(ctx.esp, args, sizeof(args))) return;
    for (auto& frame : selectionFrames) {
        if (!frame.binding.owner || frame.binding.owner != args[1]) continue;
        uint16_t refs = 0;
        if (!Read(frame.binding.sourceHeader + 0x0C, &refs, 2) || !refs || refs == UINT16_MAX) {
            frame.bindResult = 100; return;
        }
        uint32_t replacement = 0;
        const auto result = frame.binding.replacement(args[0] - base, args[1], args[2], replacement);
        frame.bindResult = static_cast<unsigned>(result);
        if (result == ds2::native::BindResult::Matched) {
            frame.bindingInstance = ctx.ecx;
            SIZE_T written = 0;
            frame.applied = WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(ctx.esp + 8),
                &replacement, 4, &written) && written == 4;
            if (!frame.applied) frame.bindResult = 101;
        }
        return;
    }
}

bool InstallBindingHooks(FILE* log) {
    const struct { uint32_t rva; unsigned char bytes[5]; } callers[] = {
        {0x3CF408, {0xE8,0x53,0x9B,0x04,0x00}},
        {0x1E415, {0xE8,0x06,0x67,0xB4,0x00}},
        {0x1E454, {0xE8,0xC7,0x66,0xB4,0x00}}
    };
    for (const auto& caller : callers) {
        unsigned char bytes[5]{};
        if (!Read(base + caller.rva, bytes, sizeof(bytes)) || std::memcmp(bytes, caller.bytes, sizeof(bytes))) {
            std::fprintf(log, "vent binding caller signature mismatch rva=%08X\n", caller.rva); return false;
        }
    }
    const unsigned char ownerBytes[] = {0x51,0xFF,0x41,0x04,0x53,0x56,0x8B,0x74,0x24,0x14};
    const unsigned char ctorBytes[] = {0x8B,0x44,0x24,0x08,0x56,0x8B,0xF1,0x83,0xC0,0x10,0x56};
    unsigned char actual[sizeof(ctorBytes)]{};
    if (!Read(base + 0x418F60, actual, sizeof(ownerBytes)) || std::memcmp(actual, ownerBytes, sizeof(ownerBytes)) ||
        !Read(base + 0xB64B20, actual, sizeof(ctorBytes)) || std::memcmp(actual, ctorBytes, sizeof(ctorBytes))) {
        std::fprintf(log, "vent binding signature mismatch\n"); return false;
    }
    bindingOwnerHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x418F60), OnBindingOwner);
    bindingConstructorHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0xB64B20), OnBindingConstructor);
    if (!bindingOwnerHook || !bindingConstructorHook) {
        bindingOwnerHook.reset(); bindingConstructorHook.reset();
        std::fprintf(log, "vent binding hook creation failed\n"); return false;
    }
    std::fprintf(log, "vent binding hooks installed; constructor argument transfer experimental\n");
    return true;
}

int ConfigInt(const std::string& ini, const char* section, const char* key, int fallback, int low, int high) {
    return std::clamp(static_cast<int>(GetPrivateProfileIntA(section, key, fallback, ini.c_str())), low, high);
}

// How many donors the blocklist took out of play this run, reported by the
// observer once the pool is built.
unsigned ventBlocklistApplied = 0;
// Families [VentFamilies] keeps out of vents, and how many records that cost.
std::set<std::string> ventFamilyBlocked;
unsigned ventFamilyBlockedRecords = 0;
unsigned poolFamilyUnpackaged = 0;
unsigned poolPassiveDonors = 0;
bool directDonors = false;
bool variantsEnabled = false;
unsigned variantsLoaded = 0;

void LoadEnemyPool(const std::string& ini) {
    settings.enabled = GetPrivateProfileIntA("DS2Randomizer", "Enabled", 0, ini.c_str()) != 0;
    settings.crossFamily = GetPrivateProfileIntA("DS2Randomizer", "CrossFamily", 0, ini.c_str()) != 0;
    settings.ventVariants = ConfigInt(ini, "DS2Randomizer", "VentVariants", 0, 0, 1) != 0;
    settings.crossFamilyVents = ConfigInt(ini, "DS2Randomizer", "CrossFamilyVents", 0, 0, 1) != 0;
    settings.ventRebinding = ConfigInt(ini, "DS2Randomizer", "VentRebinding", 0, 0, 1) != 0;
    settings.ventMessageContract =
        ConfigInt(ini, "DS2Randomizer", "VentMessageContract", 0, 0, 1) != 0 && settings.ventRebinding;
    settings.poolActorsFollowLevel =
        ConfigInt(ini, "DS2Randomizer", "PoolActorsFollowLevel", 1, 0, 1) != 0;
    settings.scriptedArrivals = ConfigInt(ini, "DS2Randomizer", "ScriptedArrivals", 0, 0, 1) != 0;
    settings.infectedArrivals = ConfigInt(ini, "DS2Randomizer", "InfectedArrivals", 0, 0, 1) != 0;
    settings.seed = GetPrivateProfileIntA("DS2Randomizer", "Seed", 1, ini.c_str());
    settings.chance = ConfigInt(ini, "DS2Randomizer", "Chance", 100, 0, 100);
    {
        // VariantBias=<name>: that variant wins every draw it is a candidate in (test aid)
        char bias[64]{};
        GetPrivateProfileStringA("DS2Randomizer", "VariantBias", "", bias, sizeof(bias), ini.c_str());
        settings.preferredVariant = bias;
    }
    maxDonorLookups = ConfigInt(ini, "DS2Randomizer", "MaxDonorLookups", 8, 1, 16);
    for (const auto& row : ds2::kEnemyCatalog) {
        ds2::EnemyRecord record{row.guid, row.model, row.family, row.context};
        record.ordinary = row.ordinary || (row.extended &&
            ConfigInt(ini, "ExperimentalClasses", row.family, 0, 0, 1) != 0);
        record.ambiguous = row.ambiguous;
        record.arrivalKey = row.arrivalKey;
        record.crossArrivalKey = row.crossArrivalKey;
        record.ventFreeKey = row.ventFreeKey;
        record.ventOpenKey = row.ventOpenKey;
        record.archive = row.archive;
        record.chapter = row.chapter;
        record.scriptedKey = row.scriptedKey;
        record.passive = row.passive;
        const auto messages = ds2::scriptMessagesFor(row.guid);
        record.deathMessage = messages.death;
        record.healthModifiedMessage = messages.healthModified;
        record.spawnDoneMessage = messages.spawnDone;
        record.awareMessage = messages.aware;
        for (const auto& entry : ds2::kVentGuids) if (entry.target == row.guid) { record.vent = entry.vent; break; }
        record.sourceEnabled = GetPrivateProfileIntA("Sources", record.family.c_str(), 1, ini.c_str()) != 0;
        record.targetEnabled = GetPrivateProfileIntA("Targets", record.family.c_str(), 1, ini.c_str()) != 0;
        record.weight = ConfigInt(ini, "Weights", row.family, 100, 0, 1000);
        pool.push_back(std::move(record));
    }
    if (ConfigInt(ini, "DS2Randomizer", "GlobalTemplates", 0, 0, 1))
        globalTemplates = ds2::appendGlobalTargets(pool);
    if (globalTemplates && !ds2::appendVentDonors(pool)) { globalTemplates = false; poolStageFailed = "vent_donors"; }
    if (globalTemplates && !ds2::appendPlaydeadDonors(pool)) { globalTemplates = false; poolStageFailed = "playdead_donors"; }
    // The pack boy stand-in. It is the only donor that was added by hand rather
    // than derived, and the only one that supersedes a root. Two of them were on
    // screen when a player's game went down, and for both of them all four
    // message fields came back `ambiguous`: the component walk saw two of every
    // class on one actor, which is not what the authored entity holds. Until
    // that is understood it stays off unless asked for.
    directDonors = ConfigInt(ini, "DS2Randomizer", "DirectDonors", 0, 0, 1) != 0;
    if (globalTemplates && directDonors && !ds2::appendDirectDonors(pool)) { globalTemplates = false; poolStageFailed = "direct_donors"; }
    // The Divider and the Ubermorph. Their records are in the pool either way;
    // whether they are ever picked is the ExperimentalClasses switch of their
    // family, which is what decides `ordinary` for the copied source above.
    if (globalTemplates && !ds2::appendBossDonors(pool)) { globalTemplates = false; poolStageFailed = "boss_donors"; }
    // Stat variants. Their chance can be tuned per name in [Variants]; 0 takes
    // one out without touching the pool.
    variantsEnabled = ConfigInt(ini, "DS2Randomizer", "Variants", 0, 0, 1) != 0;
    if (globalTemplates && variantsEnabled && !ds2::appendVariantDonors(pool)) { globalTemplates = false; poolStageFailed = "variant_donors: " + ds2::variantDonorFailure; }
    unsigned variants = 0;
    for (auto& record : pool) {
        if (record.variantName.empty()) continue;
        record.chance = static_cast<unsigned>(ConfigInt(ini, "Variants", record.variantName.c_str(),
            static_cast<int>(record.chance), 0, 100));
        if (!record.chance) record.targetEnabled = false;
        else ++variants;
    }
    variantsLoaded = variants;
    // The pool forces one set of character packages resident for the whole game,
    // and every one of them is a stream the engine may no longer evict. That is
    // what makes trimming the TOC worth doing. But a package that is gone takes
    // its models with it, and a donor without models arrives as nothing at all.
    // So a family whose package is no longer forced stops being a candidate here
    // rather than at the spawn site.
    unsigned unpackaged = 0;
    for (auto& record : pool) {
        if (!record.targetEnabled) continue;
        for (const auto& packaged : ds2::kPackagedFamilies) {
            if (packaged.resident || record.family != packaged.family) continue;
            record.targetEnabled = false;
            ++unpackaged;
            break;
        }
    }
    poolFamilyUnpackaged = unpackaged;
    // An enemy authored never to attack is no replacement for one that
    // does. The pool copies inherit the flag from their source, so this
    // catches the copy as well as the original. Sources are untouched.
    unsigned passive = 0;
    for (auto& record : pool)
        if (record.passive && record.targetEnabled) {
            record.targetEnabled = false;
            ++passive;
        }
    poolPassiveDonors = passive;
    // A donor that was seen sitting in a vent is not offered for vents again.
    // It used to be taken out of the pool altogether, which also removed it
    // from ordinary walk-in spawns it had never failed at, and since the list is
    // private to one machine that made two players on the same seed diverge in
    // rooms that have nothing to do with vents. Now it only leaves the vent
    // candidates, and the direct plans stay the same on every machine.
    unsigned blocked = 0;
    for (auto& record : pool)
        if (ventDonorBlocklist.count(record.guid) && record.targetEnabled) {
            record.ventBlocked = true;
            ++blocked;
        }
    ventBlocklistApplied = blocked;
    // A whole family kept out of vents, for the cases where the blocklist would
    // only ever learn one record at a time and each lesson costs a playthrough.
    // The Lurker is why this exists: one of them held the hospital lobby shut on
    // 2026-09-16 with no state machines at all, and there are thirteen more
    // vent-capable Lurker records behind it that nothing has tested. A family
    // switched off here still takes part in every ordinary walk-in spawn, so the
    // cost is variety in vents and nothing else.
    //
    // It rides on the same ventBlocked flag the blocklist sets, so there is no
    // second gate to keep in step with compatibleVent - and no second way for
    // the two to disagree.
    unsigned familyBlocked = 0;
    ventFamilyBlocked.clear();
    for (auto& record : pool) {
        // Only records that could ever have been a vent donor in the first place.
        // Setting the flag on the other 44 of the 57 Lurker records would change
        // nothing - compatibleVent never sees them - but it would make the log
        // claim a cost four times the real one.
        if (record.ventBlocked || !record.targetEnabled || !ds2::ventContext(record.context))
            continue;
        if (GetPrivateProfileIntA("VentFamilies", record.family.c_str(), 1, ini.c_str()) != 0)
            continue;
        record.ventBlocked = true;
        ventFamilyBlocked.insert(record.family);
        ++familyBlocked;
    }
    ventFamilyBlockedRecords = familyBlocked;
    for (const auto& source : pool) {
        plans.push_back(ds2::buildPlan(settings, source, pool, maxDonorLookups));
        allowedSpawnerPlans.push_back(ds2::buildAllowedSpawnerPlan(settings, source, pool, maxDonorLookups));
    }
    // PlanDump=<guid>: the donor order the plan holds for that source, for the log
    {
        char text[64]{};
        GetPrivateProfileStringA("DS2Randomizer", "PlanDump", "", text, sizeof(text), ini.c_str());
        unsigned w[4]{};
        if (std::sscanf(text, "%8x-%8x-%8x-%8x", &w[0], &w[1], &w[2], &w[3]) == 4) {
            const Guid wanted{w[0], w[1], w[2], w[3]};
            for (size_t i = 0; i < pool.size(); ++i) {
                if (pool[i].guid != wanted) continue;
                char line[1400]{};
                size_t used = static_cast<size_t>(std::snprintf(line, sizeof(line), "plan_dump source=%s status=%s family=%s variant_candidates=%u winners=%u model=%08X-%08X donors=",
                    text, ds2::statusName(static_cast<unsigned>(plans[i].status)), plans[i].chosenFamily.c_str(), plans[i].variantCandidates, plans[i].variantWinners, plans[i].model[0], plans[i].model[1]));
                for (size_t d = 0; d < plans[i].donors.size() && used < sizeof(line) - 120; ++d) {
                    const auto& donor = pool[plans[i].donors[d]];
                    used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, " %08X-%08X(%s%s%s)", donor.guid[0], donor.guid[1],
                        donor.family.c_str(), donor.variantName.empty() ? "" : ":", donor.variantName.c_str()));
                }
                planDumpLine = line;
            }
        }
    }
}

// Called after the original lookup, before the original creation call.
void OnLookup(SafetyHookContext& ctx) {
    if (!settings.enabled) return;
    SelectionFrame* state = nullptr;
    for (auto& frame : selectionFrames) if (!frame.stack) { state = &frame; break; }
    if (!state) { InterlockedIncrement(&frameOverflows); return; }
    *state = {};
    state->stack = ctx.esp;
    // Never block here. The stream thread holds this mutex while it calls into the
    // manager, and this path calls into the manager too; waiting for it with the
    // engine's own lock held is how a level ends up loading forever.
    std::unique_lock<std::recursive_mutex> guard(streamProbeMutex, std::try_to_lock);
    if (!guard.owns_lock()) {
        state->status = static_cast<unsigned>(ds2::SelectionStatus::StreamProbeBusy);
        return;
    }
    if (InterlockedCompareExchange(&streamSelectionAllowed, 1, 1) == 0) {
        state->status = static_cast<unsigned>(ds2::SelectionStatus::GlobalPoolUnavailable);
        return;
    }
    auto& selectionStatus = state->status;
    auto& selectedGuid = state->guid;
    const auto spawnerRule = RuleForSpawner(ctx.edi, ctx.ebp);
    if (spawnerRule.mode == ds2::SpawnerMode::Block) {
        selectionStatus = static_cast<unsigned>(ds2::SelectionStatus::SpawnerBlocked);
        return;
    }
    Guid original{};
    if (!Read(ctx.ebp + 0x0C, original.data(), 16)) { selectionStatus = static_cast<unsigned>(ds2::SelectionStatus::SourceReadFailed); return; }
    if (ctx.eax == ctx.esp + 0x30) {
        uint32_t levelHandle = 0;
        if (Read(ctx.eax + 0x08, &levelHandle, 4) && levelHandle)
            InterlockedExchange(&lastLevelHandle, static_cast<LONG>(levelHandle));
    }
    auto found = std::lower_bound(pool.begin(), pool.end(), original,
        [](const auto& record, const Guid& guid) { return record.guid < guid; });
    const bool knownSource = found != pool.end() && found->guid == original;
    const bool forced = forcedDonorIndex < pool.size();
    // An extra spawn (placement) drives whatever spawner it was bound to, and
    // that spawner's authored target may be no enemy at all: a prop, a
    // container, a pickup. The catalog has no row for it and the plan no
    // entry, but the donor is forced, so the packet is replaced like any
    // other and only the source-dependent parts (plan, family check, vent
    // contract) are skipped. Until 20.09.2026 this returned unknown_source
    // and the "Guardian" came out as a second copy of the prop.
    if (!knownSource && !forced) { selectionStatus = static_cast<unsigned>(ds2::SelectionStatus::UnknownSource); return; }
    state->original = original;
    const ds2::SelectionPlan* plan = knownSource
        ? &(spawnerRule.mode == ds2::SpawnerMode::Allow ? allowedSpawnerPlans : plans)[static_cast<size_t>(found - pool.begin())]
        : nullptr;
    selectionStatus = static_cast<unsigned>(plan ? plan->status : ds2::SelectionStatus::UnknownSource);
    if (!forced && plan->donors.empty()) return;
    // A spawner with its own family list (DS2SpawnerRules.tsv) still lets the
    // authored enemy through now and then: one chance in (families + 1), by
    // the seed, so a list of one kind keeps the original half the time and a
    // long list rarely. When the original wins, a second seeded coin decides
    // whether it comes as its own kind (no replacement at all) or as the
    // super variant of that kind, if the pool holds one (super_slasher,
    // super_puker, super_leaper, super_infector, super_brute, super_lurker,
    // super_stalker). Variety I wanted, 15.09.
    std::vector<size_t> superOnly;
    const std::vector<size_t>* donors = plan ? &plan->donors : &superOnly;
    if (forced) {
        superOnly.push_back(forcedDonorIndex);
        donors = &superOnly;
    } else if (!spawnerRule.families.empty()) {
        const unsigned kinds = static_cast<unsigned>(spawnerRule.families.size());
        if (ds2::stableHash(settings.seed, original, original, 0x4F524947u) % (kinds + 1) == 0) {
            size_t superIndex = pool.size();
            if (ds2::stableHash(settings.seed, original, original, 0x53555052u) % 2 == 0) {
                for (const auto& row : ds2::kGlobalTargets) {
                    if (std::strncmp(row.package, "super_", 6) != 0 || found->family != row.family) continue;
                    const auto candidate = std::lower_bound(pool.begin(), pool.end(), row.guid,
                        [](const auto& record, const Guid& guid) { return record.guid < guid; });
                    if (candidate == pool.end() || candidate->guid != row.guid || !candidate->targetEnabled ||
                        !candidate->ordinary || candidate->guid == original) continue;
                    superIndex = static_cast<size_t>(candidate - pool.begin());
                    break;
                }
            }
            if (superIndex == pool.size()) {
                selectionStatus = static_cast<unsigned>(ds2::SelectionStatus::OriginalKept);
                return;
            }
            superOnly.push_back(superIndex);
            donors = &superOnly;
        }
    }
    ds2::native::SpawnInfo32 originalPacket{};
    Guid originalResolved{};
    unsigned originalDescriptor = 0;
    if (ctx.eax != ctx.esp + 0x30 ||
        !Read(ctx.eax, &originalPacket, sizeof(originalPacket)) || !originalPacket.record || !originalPacket.attributes ||
        !Read(originalPacket.recordField0C, originalResolved.data(), 16) || originalResolved != original ||
        !Read(originalPacket.attributes, &originalDescriptor, 4) || !originalDescriptor) {
        selectionStatus = static_cast<unsigned>(ds2::SelectionStatus::InvalidOriginal); return;
    }
    unsigned manager = 0;
    if (!Read(base + 0x1C1C0A0, &manager, 4) || !manager || manager != ctx.ebx) {
        selectionStatus = static_cast<unsigned>(ds2::SelectionStatus::InvalidManager); return;
    }
    using Lookup = void* (__thiscall*)(void*, ds2::native::SpawnInfo32*, const Guid*, unsigned);
    for (const auto index : *donors) {
        const auto& chosen = pool[index];
        // The plan says which enemies fit this source. The rule says which of
        // them fit this one spawner, which is knowledge that only playing the
        // encounter produces. It can only take candidates away. The super
        // variant of the authored kind is the one exception: it is the
        // original in all but name.
        if (plan && donors == &plan->donors && !spawnerRule.allowsFamily(chosen.family)) {
            selectionStatus = static_cast<unsigned>(ds2::SelectionStatus::FamilyNotAllowed);
            continue;
        }
        selectedGuid = chosen.guid;
        ++state->attempts;
        if (chosen.privateTemplate && !GlobalDonorAllowed(0)) {
            selectionStatus = static_cast<unsigned>(ds2::SelectionStatus::GlobalPoolUnavailable); continue;
        }
        ds2::native::SpawnInfo32 donor{};
        reinterpret_cast<Lookup>(base + 0x376440)(reinterpret_cast<void*>(manager), &donor, &chosen.guid, 0);
        if (!donor.record || !donor.attributes) continue;
        if (chosen.privateTemplate && (!donor.field08 || !GlobalDonorAllowed(donor.field08))) {
            selectionStatus = static_cast<unsigned>(ds2::SelectionStatus::GlobalPoolUnavailable); continue;
        }
        Guid resolved{};
        unsigned descriptor = 0;
        if (!Read(donor.recordField0C, resolved.data(), 16) || resolved != chosen.guid ||
            !Read(donor.attributes, &descriptor, 4) || !descriptor) { selectionStatus = static_cast<unsigned>(ds2::SelectionStatus::InvalidDonor); continue; }
        if (knownSource && (!settings.crossFamily || chosen.family == found->family) && descriptor != originalDescriptor) { selectionStatus = static_cast<unsigned>(ds2::SelectionStatus::DescriptorMismatch); continue; }
        if (knownSource && settings.ventRebinding && ds2::ventContext(found->context)) {
            ds2::native::VentHeaders headers{};
            auto reader = [](uint32_t address, void* output, size_t size) { return Read(address, output, size); };
            const auto result = ds2::native::prepareVentHeaders(originalPacket.componentSet, donor.componentSet,
                static_cast<uint32_t>(base + 0x1CA4078), found->vent, reader, headers, chosen.privateTemplate);
            state->bindResult = 200 + static_cast<unsigned>(result);
            if (result != ds2::native::RebindResult::Ready) { selectionStatus = static_cast<unsigned>(ds2::SelectionStatus::InvalidDonor); continue; }
            if (headers.source != headers.donor) {
                if (!state->binding.arm(ctx.eax, donor.componentSet, headers.source, headers.donor)) {
                    selectionStatus = static_cast<unsigned>(ds2::SelectionStatus::InvalidDonor); continue;
                }
                InterlockedIncrement(&bindingArmed);
            }
            // Only here is a relaxed pair committed, once the rebind is settled:
            // either the two share the one header, so the message is the same by
            // definition, or the swap is armed and the replacement will run on
            // the source's spawning component: vent, activation flags and
            // spawn-done message alike. Anything else took the `continue` above.
            if (ds2::ventMessageRelaxed(settings, *found, chosen))
                InterlockedIncrement(&relaxedVentSwaps);
        }
        donor.transform = originalPacket.transform;
        // Keep the level's stream handle, not the donor's.
        //
        // The create path at 0x00776480 reads the packet's handle at +0x08, asks
        // the stream manager for its status, and if the stream is ready registers
        // the new entity against it. Vanilla enemies are therefore owned by the
        // level's stream and are destroyed when it is torn down. A replacement
        // carrying the donor's handle is owned by the global pool instead, which
        // stays resident, so it survives a checkpoint restart and then ticks
        // against state tables the teardown already freed. That is the crash in
        // analysis/STATE_TABLE_GENERATIONS.md.
        //
        // The donor handle is still what GlobalDonorAllowed checked above; only
        // the ownership recorded in the packet changes.
        if (settings.poolActorsFollowLevel && originalPacket.field08)
            donor.field08 = originalPacket.field08;
        state->levelHandle = originalPacket.field08;
        state->donorHandle = donor.field08;
        // Binding state is cleared at the matching post-create callback.
        std::memcpy(reinterpret_cast<void*>(ctx.eax), &donor, sizeof(donor));
        selectionStatus = static_cast<unsigned>(ds2::SelectionStatus::PacketReplaced);
        return;
    }
    // An extra spawn asked for exactly one enemy and did not get it: the
    // spawner must not build its own instead (it would then be moved to the
    // rule's spot and counted as the extra). A packet without a record makes
    // the create path (0x00776480) return null, which the caller reports as
    // failed. Ordinary arrivals are untouched: `forced` is only ever set
    // around an extra spawn.
    if (forced) {
        std::memset(reinterpret_cast<void*>(ctx.eax), 0, sizeof(ds2::native::SpawnInfo32));
        if (selectionStatus == static_cast<unsigned>(ds2::SelectionStatus::UnknownSource))
            selectionStatus = static_cast<unsigned>(ds2::SelectionStatus::InvalidDonor);
    }
}
