#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// The plain CRT calls (fopen, sscanf, strncpy) are used on purpose; silences C4996.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <set>
#include <string>
#include <share.h>
#include "selection.h"
#include "spawner_rules.h"
#include "extra_spawns.h"
#include "more_enemies.h"
#include "generated_enemy_catalog.h"
#include "generated_corpse_templates.h"
#include "native_layout.h"
#include "stream_registry.h"
#include "stream_states.h"
#include "component_rebinding.h"
#include "construction_binding.h"
#include "generated_stream_requirements.h"
#include "generated_global_targets.h"
#include "global_stream_lifetime.h"
#include "global_pool.h"
#include "../third_party/safetyhook/safetyhook.hpp"
#include "hook_site.h"

// File scope on purpose: this adapter declares ds2::schematics.
#include "runtime_schematics.inc"
#include "runtime_bench.inc"
#include "death_message.h"
#include "runtime_service_guard.inc"
#include "runtime_watchdog.inc"

namespace {
using Guid = std::array<unsigned, 4>;
struct Event {
    LONG ready;
    unsigned source, attributes, actor, packetAttributes;
    Guid target;
    Guid selected;
    unsigned selection, attempts, tick;
    unsigned bindingOwner, bindingSource, bindingDonor, bindingResult;
    unsigned levelHandle, donorHandle;
    bool bindingApplied;
    bool readable;
};
constexpr LONG kCapacity = 4096;
Event events[kCapacity]{};
volatile LONG calls = 0;
volatile LONG frameOverflows = 0, detailCount = 0, created = 0, failed = 0, replacedCreated = 0;
volatile LONG bindingArmed = 0, bindingApplied = 0, bindingMissed = 0;
// Which families the watchdog may kill. Protected families are refused in
// code as well, so a stray 1 here cannot reach a boss.
std::map<std::string, bool> watchdogFamilies;
std::map<std::string, unsigned> watchdogFamilySeconds;
bool watchdogFamilyDefault = false;
// Donors that were seen still sitting in a vent long after they spawned. The
// file lives next to the log and is read at startup, so a donor that does not
// work at a vent is used once and then left alone. Which donors those are cannot
// be decided from the authored data (every check that could be made offline
// passes for them), so the plugin finds out by trying and remembers.
std::set<ds2::Guid> ventDonorBlocklist;
std::string ventBlocklistPath;

void LoadVentDonorBlocklist(FILE* log) {
    FILE* file = std::fopen(ventBlocklistPath.c_str(), "r");
    if (!file) return;
    char line[128];
    while (std::fgets(line, sizeof(line), file)) {
        ds2::Guid guid{};
        if (std::sscanf(line, "%8X-%8X-%8X-%8X", &guid[0], &guid[1], &guid[2], &guid[3]) == 4)
            ventDonorBlocklist.insert(guid);
    }
    std::fclose(file);
    if (log)
        std::fprintf(log, "vent_blocklist loaded=%zu file=%s\n",
                     ventDonorBlocklist.size(), ventBlocklistPath.c_str());
}

// Called from the watchdog when a replacement was still in a vent machine long
// after it spawned. Appending immediately means a crash later in the run does
// not lose what was learned.
void RememberBadVentDonor(const ds2::Guid& guid) {
    if (ventBlocklistPath.empty() || !ventDonorBlocklist.insert(guid).second) return;
    if (FILE* file = std::fopen(ventBlocklistPath.c_str(), "a")) {
        std::fprintf(file, "%08X-%08X-%08X-%08X\n", guid[0], guid[1], guid[2], guid[3]);
        std::fclose(file);
    }
}
// A shorter leash for arrivals through a rebound vent, 0 to use the family
// default. Those are the arrivals that strand enemies, and a quarantine
// waits for every one of them. It only ever shortens: the leash is the
// smaller of this and the family's own.
unsigned watchdogVentSeconds = 30;
unsigned deathMessagesWritten = 0, deathMessagesCleared = 0, deathMessagesRefused = 0;
unsigned deathMessagesUnregistered = 0;
const char* lastDeathMessageRefusal = "none";
// Attribute header of the last message field that was handled, for the summary line.
uint32_t lastDeathMessageHeader = 0;
volatile LONG statusCounts[static_cast<unsigned>(ds2::SelectionStatus::Count)]{};
bool observeDetails = false;
unsigned detailLimit = 256;
safetyhook::MidHook observer;
uintptr_t base;

bool Read(uintptr_t address, void* out, size_t bytes) {
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
                             out, bytes, &done) && done == bytes;
}

// Is this dword a pointer to that string? Used to recognise a message field
// whose value is the name rather than its hash.
bool StringAt(unsigned value, const char* name) {
    if (value < 0x10000) return false;
    char buffer[96]{};
    const size_t length = std::strlen(name);
    if (length + 1 > sizeof(buffer)) return false;
    return Read(value, buffer, length + 1) && std::memcmp(buffer, name, length + 1) == 0;
}

// A place to keep the source's message name so a pointer field can point at it.
// The arena is tiny, never freed, and every name is stored once.
std::map<std::string, unsigned> messageStrings;
unsigned MessageStringAddress(const char* name) {
    const auto found = messageStrings.find(name);
    if (found != messageStrings.end()) return found->second;
    const size_t size = std::strlen(name) + 1;
    void* memory = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory) return 0;
    std::memcpy(memory, name, size);
    const auto address = static_cast<unsigned>(reinterpret_cast<uintptr_t>(memory));
    messageStrings.emplace(name, address);
    return address;
}

std::recursive_mutex streamProbeMutex;
bool GlobalDonorAllowed(uint32_t handle);
#include "runtime_spawner_tools.inc"
#include "runtime_placement.inc"
#include "runtime_selection.inc"
#include "runtime_more_enemies.inc"
#include "runtime_stream_probe.inc"
#include "runtime_pool_ownership.inc"
#include "runtime_navpower.inc"
#include "runtime_loading_hold.inc"
#include "runtime_message_trace.inc"
#include "runtime_variants.inc"
#include "runtime_material_probe.inc"
#include "runtime_divider_probe.inc"
#include "runtime_listener_guard.inc"
#include "runtime_guid_redirect.inc"
#include "runtime_loot.inc"
#include "runtime_frame_cap.inc"
#include "runtime_texture_probe.inc"
#include "runtime_stream_watch.inc"
#include "runtime_zone_unload.inc"

void OnCreated(SafetyHookContext& ctx) {
    SelectionFrame saved{};
    for (auto& frame : selectionFrames) if (frame.stack == ctx.esp) {
        saved = frame;
        frame = {};
        break;
    }
    InterlockedIncrement(&calls);
    if (extraSpawnActive) lastCreatedActor = static_cast<uint32_t>(ctx.eax);
    if (saved.applied) {
        uint32_t payload = 0;
        saved.applied = ctx.eax && ctx.eax == saved.binding.owner && saved.bindingInstance &&
            Read(saved.bindingInstance + 0x0C, &payload, 4) && payload == saved.binding.sourceHeader + 0x10;
        if (!saved.applied) saved.bindResult = 102;
    }
    InterlockedIncrement(ctx.eax ? &created : &failed);
    if (saved.status < static_cast<unsigned>(ds2::SelectionStatus::Count))
        InterlockedIncrement(&statusCounts[saved.status]);
    const bool replaced = saved.status == static_cast<unsigned>(ds2::SelectionStatus::PacketReplaced) && ctx.eax;
    if (replaced) InterlockedIncrement(&replacedCreated);
    if (replaced) {
        auto reader = [](uint32_t address, void* output, size_t size) { return Read(address, output, size); };
        // The donor decides what this enemy now is, so its family decides
        // whether the watchdog may ever kill it. Bosses, anchored actors and
        // the parts another actor owns are never eligible.
        bool allowKill = false;
        const auto donor = std::lower_bound(pool.begin(), pool.end(), saved.guid,
            [](const auto& record, const Guid& guid) { return record.guid < guid; });
        if (donor != pool.end() && donor->guid == saved.guid &&
            !ds2::moreEnemiesProtectedFamily(donor->family))
            allowKill = watchdogFamilies.count(donor->family)
                ? watchdogFamilies.at(donor->family) : watchdogFamilyDefault;
        // A variant's speed and size are written now, while the actor is fresh.
        if (donor != pool.end() && donor->guid == saved.guid && !donor->variantName.empty())
            variant_runtime::OnVariantCreated(ctx.eax, *donor);
        unsigned leash = 0;
        if (allowKill && watchdogFamilySeconds.count(donor->family))
            leash = watchdogFamilySeconds.at(donor->family) * 1000u;
        // A rebound vent is the arrival most likely to strand an enemy: a
        // friend's session had 10 of 13 stranded enemies come out of one, and
        // two donor headers were handed to several enemies at once. A quarantine
        // waits for every one of them, so these get a shorter leash than the
        // family default rather than holding the room for the full 90 seconds.
        if (allowKill && saved.binding.packet && watchdogVentSeconds) {
            const unsigned family = leash ? leash : ds2_watchdog_runtime::timeoutMs;
            const unsigned vent = watchdogVentSeconds * 1000u;
            leash = vent < family ? vent : family;
        }
        ds2_watchdog_runtime::Track(ctx.eax, saved.guid, allowKill, leash, reader);
        // A scripted room waits for the messages the authored enemy would have
        // sent: its death above all, and in fewer places the end of its spawn,
        // a health change, or noticing the player. The replacement carries the
        // donor's instead, so the count never completes: that is what a stuck
        // quarantine has always been, and it has nothing to do with vents.
        //
        // The field is located through the component's class descriptor, and
        // when the donor carries a message of its own, exactly that value has
        // to be standing there. Anything else leaves the enemy alone.
        const auto sourceRecord = std::lower_bound(pool.begin(), pool.end(), saved.original,
            [](const auto& record, const ds2::Guid& guid) { return record.guid < guid; });
        const bool haveSource = sourceRecord != pool.end() && sourceRecord->guid == saved.original;
        const bool haveDonor = donor != pool.end() && donor->guid == saved.guid;
        // A swarmer or pack boy never comes alone: SpawnDone repeats this
        // spawner (horde). The source's death message goes along so the
        // sender can swallow the copies' deaths (runtime_more_enemies.inc).
        // Only when it replaces another kind: a swarmer slot keeps its
        // authored count, the room already has a dozen of them.
        if (haveSource && haveDonor && donor->family != sourceRecord->family)
            MarkHordeReplacement(ctx.edi, donor->family, sourceRecord->deathMessage.hash);
        // The multiplier counts what stands there, not what was authored.
        if (haveDonor) NoteReplacement(ctx.edi, donor->family);
        if (haveSource && haveDonor) {
            // Components, not the authored attribute header behind them: that
            // header is shared by every spawn of the same donor and is read at
            // construction anyway, so patching it changed nothing and would have
            // leaked into later spawns. One run proved both: four writes
            // reported written_as_hash and the door still did not open.
            auto readWord = [](uint32_t address, void* out, size_t size) { return Read(address, out, size); };
            auto writeWord = [](uint32_t address, const void* in, size_t size) {
                SIZE_T written = 0;
                return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
                                          in, size, &written) && written == size;
            };
            // Every component the actor owns; the authored header of each names
            // its class, which is how the right one is found without vtables.
            // Enemies carry 22-28 authored components, and ForEachComponent used
            // to report each one more than once, so 32 slots overflowed and real
            // components were dropped before the message search ran. The scan is
            // de-duplicated now; the larger bound keeps headroom.
            ds2::MemoryRegion components[64]{};
            size_t componentCount = 0;
            ds2_watchdog_runtime::ForEachComponent(ctx.eax, [&](uint32_t component) {
                if (componentCount < std::size(components))
                    components[componentCount++] = {component, 0};
            });
            const struct {
                const ds2::MessageField* fields;
                size_t count;
                const ds2::ScriptMessage& wanted;
                const ds2::ScriptMessage& carried;
            } work[] = {
                {ds2::kDeathFields, std::size(ds2::kDeathFields),
                 sourceRecord->deathMessage, donor->deathMessage},
                {ds2::kHealthModifiedFields, std::size(ds2::kHealthModifiedFields),
                 sourceRecord->healthModifiedMessage, donor->healthModifiedMessage},
                {ds2::kSpawnDoneFields, std::size(ds2::kSpawnDoneFields),
                 sourceRecord->spawnDoneMessage, donor->spawnDoneMessage},
                // MSG_AWARE (kAwareFields) is deliberately not here any more.
                // The three above are messages the actor sends; MSG_AWARE is one
                // it listens for: the AwarenessComponent (descriptor 0x020A4D98)
                // registers a listener node under that hash in its constructor
                // and unregisters with the field's value in its destructor.
                // Rewriting or clearing the field after creation leaves the node
                // registered under the donor's hash with nobody behind it. Dump
                // 48628 (19.09.2026): pack_boy_decompression_wave_1_set_aware
                // (934101C0) dispatched by a pack boy's wave timer six level
                // segments later, second listener at 24F880E0 freed, crash at
                // 0x00756CDE; the surviving listener's field already read 0.
                // A replacement now keeps the donor's MSG_AWARE, consistent from
                // constructor to destructor; it wakes on sight instead of on the
                // source's scripted "set aware".
            };
            for (const auto& item : work) {
                const ds2::ScriptMessage& wanted = item.wanted;
                // A source without a message is not a reason to skip. The donor
                // then keeps a name from its own chapter, and outside that
                // chapter the engine's message table does not know it, which is
                // a write to null the moment anything takes a reference. See
                // writeScriptMessage: clearing it is the whole point.
                if (!wanted.hash && !item.carried.hash) continue;
                uint32_t header = 0;
                const auto result = ds2::writeScriptMessage(components, componentCount,
                    item.fields, item.count, static_cast<uint32_t>(base), wanted,
                    item.carried, readWord, writeWord, &header);
                if (result == ds2::DeathMessageResult::WrittenAsHash) ++deathMessagesWritten;
                else if (result == ds2::DeathMessageResult::Cleared) ++deathMessagesCleared;
                else if (result == ds2::DeathMessageResult::ClearedUnregistered)
                    ++deathMessagesUnregistered;
                else if (result != ds2::DeathMessageResult::NotNeeded) {
                    ++deathMessagesRefused;
                    // A refused death message means the room waits for a name
                    // this enemy can no longer send. The watchdog says it for
                    // the enemy when it dies (death proxy).
                    if (item.fields == ds2::kDeathFields && wanted.hash) {
                        ds2_watchdog_runtime::ProxyDeath(ctx.eax, wanted.hash, wanted.name);
                    }
                }
                lastDeathMessageRefusal = ds2::deathMessageResultName(result);
                if (header) lastDeathMessageHeader = header;
            }
        }
    }
    if (saved.binding.packet) {
        InterlockedIncrement(saved.applied ? &bindingApplied : &bindingMissed);
    }
    RecordRecentSpawner(ctx.edi, ctx.ebp, ctx.eax, saved.status, saved.guid);
    if (!observeDetails || !detailLimit) return;
    // A ring, not a list that fills up. The crash that matters is usually late
    // in a session, and with a fixed list the spawn lines had stopped long
    // before it. The sequence number is the publish flag: the reader takes a
    // slot only when it carries the number it is waiting for.
    const LONG sequence = InterlockedIncrement(&detailCount);
    auto& event = events[static_cast<unsigned>(sequence - 1) % detailLimit];
    InterlockedExchange(&event.ready, 0);
    event.source = ctx.edi;
    event.attributes = ctx.ebp;
    event.actor = ctx.eax;
    event.selection = saved.status;
    event.selected = saved.guid;
    event.attempts = saved.attempts;
    event.tick = GetTickCount();
    event.bindingOwner = saved.binding.owner;
    event.bindingSource = saved.binding.sourceHeader;
    event.bindingDonor = saved.binding.donorHeader;
    event.bindingResult = saved.bindResult;
    event.levelHandle = saved.levelHandle;
    event.donorHandle = saved.donorHandle;
    event.bindingApplied = saved.applied;
    // At 0x00ECB2D0 the original lookup packet is still at ESP+30, its
    // attributes field (+0x10) at ESP+40.
    event.readable = Read(ctx.ebp + 0x0C, event.target.data(), 16) &&
        Read(ctx.esp + 0x40, &event.packetAttributes, 4);
    InterlockedExchange(&event.ready, sequence);
}

bool Signature() {
    constexpr unsigned char expected[] = {
        0xE8,0x8B,0xB1,0x8A,0xFF,0x8B,0x35,0,0,0,0,
        0x68,0,0x20,0,0,0x8D,0x4C,0x24,0x34,0x51,0x8B,0xCB,
        0xC6,0x46,0x28,0,0xE8,0xB0,0xB1,0x8A,0xFF,
        0x8B,0xD8,0x89,0x5C,0x24,0x10,0xC6,0x46,0x28,1
    };
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + 0xACB2B0, actual, sizeof(actual))) return false;
    for (size_t i = 0; i < sizeof(actual); ++i) {
        if (i >= 7 && i < 11) continue;
        if (actual[i] != expected[i]) return false;
    }
    unsigned global = 0;
    std::memcpy(&global, actual + 7, 4);
    return global == base + 0x1C21300;
}

std::map<Guid, std::string> LoadCatalog(const std::string& path) {
    std::map<Guid, std::string> result;
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    while (std::getline(file, line)) {
        std::istringstream row(line);
        std::array<std::string, 8> fields;
        bool valid = true;
        for (auto& field : fields) if (!std::getline(row, field, '\t')) valid = false;
        Guid guid{};
        if (!valid || std::sscanf(fields[3].c_str(), "%x-%x-%x-%x",
            &guid[0], &guid[1], &guid[2], &guid[3]) != 4) continue;
        std::string label = fields[5] + ":" + fields[4] + ":" + fields[7];
        auto [it, inserted] = result.emplace(guid, label);
        if (!inserted && it->second != label) it->second = "ambiguous_catalog_target";
    }
    return result;
}

DWORD WINAPI Worker(void* module) {
    char path[MAX_PATH]{};
    GetModuleFileNameA(static_cast<HMODULE>(module), path, MAX_PATH);
    std::string directory(path);
    directory.resize(directory.find_last_of("\\/") + 1);
    const std::string ini = directory + "DS2Randomizer.ini";
    FILE* log = nullptr;
    // Keep the last runs. Comparing two players means comparing two runs, and
    // starting the game once used to destroy the log that was being compared.
    // The most recent finished run is DS2Randomizer.1.log, the one before it
    // .2.log, and so on up to .5.log; the oldest falls off the end.
    const std::string logPath = directory + "DS2Randomizer.log";
    constexpr unsigned kKeptLogs = 5;
    auto kept = [&](unsigned index) {
        return directory + "DS2Randomizer." + std::to_string(index) + ".log";
    };
    std::remove(kept(kKeptLogs).c_str());
    for (unsigned index = kKeptLogs - 1; index >= 1; --index)
        std::rename(kept(index).c_str(), kept(index + 1).c_str());
    std::rename(logPath.c_str(), kept(1).c_str());
    // A leftover from the one-file scheme this replaces slots in behind it.
    const std::string previousPath = directory + "DS2Randomizer.previous.log";
    std::rename(previousPath.c_str(), kept(2).c_str());
    log = _fsopen(logPath.c_str(), "w", _SH_DENYWR);
    if (!log) return 0;
    base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    // First, before the catalog and the pool cost seconds: the NavPower pool
    // hook has to be in before the engine creates the system (runtime_navpower.inc).
    navpower::Load(ini);
    navpower::Install(log);
    std::fflush(log);
    observeDetails = GetPrivateProfileIntA("DS2Randomizer", "ObserveSpawns", 0, ini.c_str()) != 0;
    detailLimit = ConfigInt(ini, "DS2Randomizer", "DetailedSpawnLimit", 256, 0, kCapacity);
    spawnerToolsEnabled = ConfigInt(ini, "DS2Randomizer", "SpawnerTools", 0, 0, 1) != 0;
    spawnerRulesPath = directory + "DS2SpawnerRules.tsv";
    extraSpawnsPath = directory + "DS2ExtraSpawns.tsv";
    ventBlocklistPath = directory + "DS2VentDonorBlocklist.tsv";
    LoadVentDonorBlocklist(log);
    ds2_watchdog_runtime::reportBadVentDonor = RememberBadVentDonor;
    spawnerRecentPath = directory + "DS2SpawnerRecent.tsv";
    LoadEnemyPool(ini);
    if (ventBlocklistApplied)
        std::fprintf(log, "vent_blocklist applied=%u of %zu remembered\n",
                     ventBlocklistApplied, ventDonorBlocklist.size());
    if (!ventFamilyBlocked.empty()) {
        std::string names;
        for (const auto& family : ventFamilyBlocked) {
            if (!names.empty()) names += ',';
            names += family;
        }
        std::fprintf(log, "vent_families off=%s records_withheld=%u "
                          "(these still take part in ordinary spawns)\n",
                     names.c_str(), ventFamilyBlockedRecords);
    }
    {
        unsigned forced = 0, trimmed = 0;
        for (const auto& packaged : ds2::kPackagedFamilies) {
            if (packaged.resident) ++forced; else ++trimmed;
        }
        std::fprintf(log, "pool_packages forced=%u trimmed=%u records_disabled=%u passive_donors=%u\n",
                     forced, trimmed, poolFamilyUnpackaged, poolPassiveDonors);
    }
    LoadMoreEnemiesSettings(ini);
    bool streamProbe = globalTemplates || ConfigInt(ini, "DS2Randomizer", "GlobalStreamProbe", 0, 0, 1) != 0;
    // Experimental: hold a reference on the pool's shared character streams so a
    // room change cannot free the state tables of NPCs built from the pool.
    pinPrerequisites = ConfigInt(ini, "DS2Randomizer", "PinPoolPrerequisites", 0, 0, 1) != 0;
    releasePoolAtLevelRelease =
        ConfigInt(ini, "DS2Randomizer", "ReleasePoolAtLevelEnd", 1, 0, 1) != 0;
    poolHoldForever = ConfigInt(ini, "DS2Randomizer", "PoolHoldForever", 0, 0, 1) != 0;
    ds2_watchdog_runtime::probeSeconds = static_cast<unsigned>(
        ConfigInt(ini, "DS2Randomizer", "WatchdogStateProbeSeconds", 6, 0, 120));
    ds2_watchdog_runtime::ventStuckSeconds = static_cast<unsigned>(
        ConfigInt(ini, "DS2Randomizer", "WatchdogVentStuckSeconds", 8, 0, 120));
    watchdogVentSeconds = static_cast<unsigned>(
        ConfigInt(ini, "DS2Randomizer", "WatchdogVentSeconds", 30, 0, 600));
    watchdogFamilyDefault = ConfigInt(ini, "WatchdogFamilies", "Default", 0, 0, 1) != 0;
    for (const auto& record : pool) {
        if (watchdogFamilies.count(record.family)) continue;
        watchdogFamilies[record.family] =
            ConfigInt(ini, "WatchdogFamilies", record.family.c_str(), watchdogFamilyDefault, 0, 1) != 0;
        // 0 means "use the global WatchdogSeconds"; anything else is this
        // family's own leash.
        const int seconds = ConfigInt(ini, "WatchdogTimers", record.family.c_str(), 0, 0, 3600);
        watchdogFamilySeconds[record.family] = seconds ? static_cast<unsigned>(seconds < 10 ? 10 : seconds) : 0u;
    }
    std::fprintf(log, "DS2 randomizer experimental v0.4.5-stream-safe pid=%lu base=%08X observe=%d enabled=%d seed=%u chance=%u cross_family=%d pool=%zu\n",
                 GetCurrentProcessId(), static_cast<unsigned>(base), observeDetails, settings.enabled,
                 settings.seed, settings.chance, settings.crossFamily, pool.size());
    std::fprintf(log, "embedded_catalog=%s max_donor_lookups=%u global_templates=%d graph_gate=required\n", ds2::kCatalogHash, maxDonorLookups, globalTemplates);
    std::fprintf(log, "vent_variants=%d cross_family_vents=%d vent_rebinding_requested=%d\n", settings.ventVariants, settings.crossFamilyVents, settings.ventRebinding);
    std::fprintf(log, "scripted_arrivals=%d infected_arrivals=%d contract=exact_spawning_component same_family_only\n",
                 settings.scriptedArrivals, settings.infectedArrivals);
    for (const char* family : {"Infector", "Stalker", "SwarmEnemy"})
        std::fprintf(log, "experimental_class=%s enabled=%d\n", family,
            ConfigInt(ini, "ExperimentalClasses", family, 0, 0, 1));
    std::fprintf(log, "direct_donors=%d pack_boy_stand_in=%s withdrawn_donors=%u corpse_templates=%zu\n", directDonors,
                 directDonors && !withdrawnDonors ? "offered" : "withheld", withdrawnDonors,
                 std::size(ds2::kCorpseTemplates));
    std::fprintf(log, "variants enabled=%d offered=%u\n", variantsEnabled ? 1 : 0, variantsLoaded);
    if (!poolStageFailed.empty()) std::fprintf(log, "global_templates off at %s\n", poolStageFailed.c_str());
    if (!planDumpLine.empty()) std::fprintf(log, "%s\n", planDumpLine.c_str());
    for (const auto& record : pool)
        if (!record.variantName.empty())
            std::fprintf(log, "variant name=%s family=%s chance=%u enabled=%d\n",
                record.variantName.c_str(), record.family.c_str(), record.chance, record.targetEnabled ? 1 : 0);
    std::fprintf(log, "protected_classes=Hunter,DividerCore; encounter and dependent-part contracts unverified\n");
    WriteMoreEnemiesConfig(log);
    unsigned planCounts[static_cast<unsigned>(ds2::SelectionStatus::Count)]{};
    for (const auto& plan : plans) ++planCounts[static_cast<unsigned>(plan.status)];
    for (unsigned i = 0; i < static_cast<unsigned>(ds2::SelectionStatus::Count); ++i)
        if (planCounts[i]) std::fprintf(log, "plan reason=%s records=%u\n", ds2::statusName(i), planCounts[i]);
    if (!observeDetails && !settings.enabled && !streamProbe && !spawnerToolsEnabled &&
        !moreEnemies.enabled) {
        std::fprintf(log, "inactive: enable DS2Randomizer.Enabled or ObserveSpawns in %s\n", ini.c_str());
        std::fclose(log); return 0;
    }
    // Enabled=0 is the master switch. Until 2026-09-17 it only stopped the
    // enemy replacement: the pool still loaded, the loading screen was still
    // taken over, and hordes, loot and the watchdog still ran. With it off
    // only the frame cap is installed, for players who use it as their
    // limiter. ObserveSpawns=1 does not override it (on 2026-09-17 it did,
    // and the switch looked dead).
    if (!settings.enabled) {
        std::fprintf(log, "master off (Enabled=0): only the frame cap is installed (ObserveSpawns=%d ignored)\n", observeDetails ? 1 : 0);
        std::fflush(log);
        for (unsigned attempt = 0; attempt < 300 && !Signature(); ++attempt) Sleep(1000);
        if (Signature()) frame_cap::Install(log, ini);
        std::fflush(log);
        for (;;) {
            Sleep(1000);
            if (frame_cap::installed) frame_cap::WriteDiagnostics(log);
            std::fflush(log);
        }
    }
    const auto catalog = LoadCatalog(directory + "funcspawn_targets.tsv");
    std::fprintf(log, "optional_spawner_labels=%zu detail_limit=%u aggregate_counters=continuous\n", catalog.size(), detailLimit);
    std::fflush(log);
    // The executable is packed, so this code does not exist yet when the plugin
    // starts: it appears only once the unpacker has run. Thirty seconds proved
    // too short on a slow launch and the plugin then gave up for the whole
    // session, which looked like a broken hook rather than a slow start.
    constexpr unsigned kSignatureAttempts = 300;
    bool matched = false;
    for (unsigned attempt = 0; attempt < kSignatureAttempts; ++attempt) {
        if (Signature()) {
            if (attempt) std::fprintf(log, "signature appeared after %u s\n", attempt);
            matched = true;
            break;
        }
        if (attempt && attempt % 30 == 0) {
            std::fprintf(log, "still waiting for the unpacked image, %u s\n", attempt);
            std::fflush(log);
        }
        Sleep(1000);
    }
    if (!matched) {
        std::fprintf(log, "signature unavailable after %u checks; no hook installed\n",
                     kSignatureAttempts);
        std::fclose(log); return 0;
    }
    // A head start for plugins that verify retail bytes at their install
    // (the DS2 Debug Menu). [DS2Randomizer] InstallDelayMs, default 3 s.
    {
        const int delay = GetPrivateProfileIntA("DS2Randomizer", "InstallDelayMs", 3000, ini.c_str());
        if (delay > 0) {
            std::fprintf(log, "install_delay ms=%d (other plugins bind first)\n", delay);
            std::fflush(log);
            Sleep(static_cast<DWORD>(delay));
        }
    }
    const unsigned char lookupBytes[] = {0x8B,0x44,0x24,0x0C,0x8B,0x54,0x24,0x08,0x56,0x50,0x52,0xE8,0xC0,0xFE,0xFF,0xFF};
    unsigned char lookupActual[sizeof(lookupBytes)]{};
    if (settings.enabled && (!Read(base + 0x376440, lookupActual, sizeof(lookupActual)) ||
        std::memcmp(lookupActual, lookupBytes, sizeof(lookupBytes)) != 0)) {
        settings.enabled = false;
        std::fprintf(log, "lookup signature mismatch; mutation disabled\n");
    }
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCSTR>(&OnCreated), &pinned)) {
        std::fprintf(log, "module pin failed; no hook installed\n");
        std::fclose(log); return 0;
    }
    if (spawnerToolsEnabled) {
        VerifySpawnerIdentity(log);
        PollSpawnerTools(log);
    }
    observer = safetyhook::create_mid(reinterpret_cast<void*>(base + 0xACB2D0), OnCreated);
    if (!observer) {
        std::fprintf(log, "observation hook failed\n"); std::fclose(log); return 0;
    }
    if (streamProbe && !InstallStreamProbe(log)) {
        std::fprintf(log, "stream_probe unavailable; no global load will be requested\n");
        streamProbe = false;
        InterlockedExchange(&streamSelectionAllowed, 0);
    } else if (!streamProbe) {
        InterlockedExchange(&streamSelectionAllowed, 1);
    }
    if (streamProbe && observeDetails) InstallMaterialProbe(log);
    if (observeDetails) divider_probe::Install(log);
    listener_guard::Install(log);
    guid_redirect::Install(log);
    loot::Install(log, ini);
    frame_cap::Install(log, ini);
    placement::Install(log, ini);
    if (settings.enabled) {
        if (settings.ventRebinding && !InstallBindingHooks(log)) {
            settings.ventRebinding = false;
            // The relaxed vent contract is only safe with the rebind that
            // hands over the source's spawning component, so it goes with it.
            settings.ventMessageContract = false;
            plans.clear();
            allowedSpawnerPlans.clear();
            for (const auto& source : pool) {
                plans.push_back(ds2::buildPlan(settings, source, pool, maxDonorLookups));
                allowedSpawnerPlans.push_back(ds2::buildAllowedSpawnerPlan(settings, source, pool, maxDonorLookups));
            }
            std::fprintf(log, "vent rebinding unavailable; rebuilt authored-only plans\n");
        }
        selectionHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0xACB2B5), OnLookup);
        if (!selectionHook) {
            settings.enabled = false;
            std::fprintf(log, "selection hook failed; observation only\n");
        } else std::fprintf(log, "selection hook installed; experimental replacement enabled global_templates=%d\n", globalTemplates);
        if (selectionHook && settings.poolActorsFollowLevel) pool_ownership::Install(log);
        variant_runtime::enabled = variantsEnabled && ConfigInt(ini, "DS2Randomizer", "VariantRuntime", 0, 0, 1) != 0;
        if (variant_runtime::enabled && !variant_runtime::Install(log)) variant_runtime::enabled = false;
    }
    // An extra spawn is the donor forced through the selection hook; without
    // it the spawner builds its own enemy, so extras stay off.
    if (extraSpawnsEnabled && !selectionHook) {
        extraSpawnsEnabled = false;
        std::fprintf(log, "extra_spawns off: the selection hook is not installed (see the lookup or selection hook line above)\n");
    }
    // The repeat hook serves MoreEnemies and the swarm horde alike.
    if ((moreEnemies.enabled || HordesActive() || extraSpawnsEnabled) && !InstallMoreEnemies(log)) {
        moreEnemies.enabled = false;
        DisableHordes();
        std::fprintf(log, "more_enemies disabled; authored spawns are untouched\n");
    }
    std::fprintf(log, "signature verified; post-create observer installed at %08X\n",
                 static_cast<unsigned>(base + 0xACB2D0));
    std::fflush(log);
    LONG next = 1;   // the sequence number of the next spawn line to write
    LONG lastCalls = -1;
    DWORD lastSummary = GetTickCount();
    const bool registryDiagnostics = ConfigInt(ini, "DS2Randomizer", "StreamRegistryDiagnostics", 0, 0, 1) != 0;
    const bool streamWatch = ConfigInt(ini, "DS2Randomizer", "StreamWatchDiagnostics", 0, 0, 1) != 0;
    std::fprintf(log, "stream_watch enabled=%d interval_ms=5000 pending_interval_ms=30000 detail_limit=64 read_only=1\n", streamWatch);
    DWORD lastRegistry = 0;
    unsigned registryReports = 0;
    LONG lastProbeResult = -1;
    unsigned nextStreamTarget = 0;
    LONG lastPrerequisite = -1;
    std::fprintf(log, "stream_registry_diagnostics=%d read_only=1 report_limit=60\n", registryDiagnostics);
    // Disabled unless [SchematicRandomizer] Enabled=1. Validates its own six
    // retail signatures and refuses to hook if any of them moved.
    ds2_schematic_runtime::Install(base, ini.c_str(), log);
    ds2_bench_runtime::Install(base, ini.c_str(), log);
    ds2_service_guard::Install(base, ini.c_str(), log);
    ds2_watchdog_runtime::Configure(ini.c_str(), log);
    ds2_watchdog_runtime::InstallKill(base, log);
    // Diagnostic only, off unless asked for: one line per script message.
    message_trace::enabled = ConfigInt(ini, "DS2Randomizer", "MessageTrace", 0, 0, 1) != 0;
    if (message_trace::enabled) message_trace::Install(log);
    texture_probe::enabled = ConfigInt(ini, "DS2Randomizer", "TextureProbe", 0, 0, 1) != 0;
    if (texture_probe::enabled) { texture_probe::Install(log); texture_probe::InstallSkinProbe(log); }
    // The loading screen stays up for MinimumLoadingSeconds and, while the
    // pool is requested but not ready, up to MaximumLoadingSeconds.
    loading_hold::Install(log, ini.c_str(), streamProbe);
    // Only the pool's own references can hold a zone switch up.
    if (streamProbe) zone_unload::Install(log, ini);
    std::fflush(log);
    LONG moreEnemiesFlushed = 0;
    for (;;) {
        PollSpawnerTools(log);
        ds2_schematic_runtime::WriteDiagnostics(log);
        WriteMoreEnemiesDiagnostics(log, moreEnemiesFlushed);
        ds2_bench_runtime::WriteDiagnostics(log);
        ds2_service_guard::WriteDiagnostics(log);
        {
            auto reader = [](uint32_t address, void* output, size_t size) { return Read(address, output, size); };
            ds2_watchdog_runtime::Poll(log, reader);
        }
        ds2_watchdog_runtime::WriteDiagnostics(log);
        loading_hold::WriteDiagnostics(log);
        zone_unload::WriteDiagnostics(log);
        if (streamWatch) PollStreamWatch(log);
        const LONG prerequisite = InterlockedCompareExchange(&streamProbePrerequisite, 0, 0);
        if (prerequisite >= 0 && prerequisite != lastPrerequisite &&
            static_cast<unsigned>(prerequisite / 16) < std::size(ds2::kStreamRequirements)) {
            std::fprintf(log, "stream_probe prerequisite=%s result=%s\n",
                ds2::kStreamRequirements[prerequisite / 16].name,
                ds2::native::descriptorStatusName(static_cast<ds2::native::DescriptorStatus>(prerequisite % 16)));
        }
        lastPrerequisite = prerequisite;
        while (nextStreamTarget < std::size(streamTargetProbes) &&
            InterlockedCompareExchange(&streamTargetProbes[nextStreamTarget].ready, 1, 1) == 1) {
            const auto& entry = streamTargetProbes[nextStreamTarget++];
            const auto& target = ds2::kGlobalTargets[entry.index];
            std::fprintf(log, "stream_target package=%s family=%s guid=%08X-%08X-%08X-%08X manager=%08X attrs=%08X components=%08X handle=%08X status=%d identity_match=%d readiness=unverified\n",
                target.package, target.family, target.guid[0], target.guid[1], target.guid[2], target.guid[3],
                entry.manager, entry.attributes, entry.components, entry.handle, entry.status, entry.guidMatches);
        }
        if (streamProbe && observeDetails) FlushMaterialProbe(log);
        if (divider_probe::installed) divider_probe::Flush(log);
        if (listener_guard::installed) listener_guard::WriteDiagnostics(log);
        if (guid_redirect::installed) guid_redirect::WriteDiagnostics(log);
        if (loot::installed) loot::WriteDiagnostics(log);
        if (frame_cap::installed) frame_cap::WriteDiagnostics(log);
        if (placement::installed) placement::WriteDiagnostics(log);
        static LONG lastFileSlotWaits = 0;
        const LONG slotWaits = InterlockedCompareExchange(&fileSlotWaits, 0, 0);
        if (slotWaits != lastFileSlotWaits) { std::fprintf(log, "pool_request file_slot_waits=%ld\n", slotWaits); lastFileSlotWaits = slotWaits; }
        {
            static LONG printedNote = 0;
            char note[sizeof(poolRequestNote)]{};
            if (TakePoolRequestNote(note, sizeof(note), printedNote)) std::fprintf(log, "%s\n", note);
        }
        const LONG probeResult = InterlockedCompareExchange(&streamProbeResult, 0, 0);
        if (streamProbe && probeResult != lastProbeResult) {
            static const char* reasons[] = {"waiting_for_update", "registry_invalid_or_full", "prerequisite_missing",
                "private_root_already_registered", "load_failed", "lock_failed", "loading",
                "dispatched_resources_unverified", "stream_failed", "manager_destroyed", "thread_changed", "prerequisite_timeout", "manager_changed_without_teardown",
                "level_release_unlocked_paused", "level_release_unlock_failed", "resume_waiting_for_root_release", "service_retirement_unverified",
                "resume_waiting_for_service_retirement", "resume_waiting_for_service_recreation", "owned_graph_dispatched",
                "loading_timeout_unlocked", "file_slots_busy"};
            std::fprintf(log, "stream_probe result=%s calls=%ld handle=%08lX selection_enabled=%d\n",
                probeResult >= 0 && probeResult < static_cast<LONG>(std::size(reasons)) ? reasons[probeResult] : "unknown",
                InterlockedCompareExchange(&streamProbeCalls, 0, 0), InterlockedCompareExchange(&streamProbeHandle, 0, 0), settings.enabled);
            lastProbeResult = probeResult;
        }
        static LONG lastRecheckHits = 0;
        const LONG recheckHits = InterlockedCompareExchange(&streamRecheckHits, 0, 0);
        if (streamProbe && recheckHits != lastRecheckHits) {
            std::fprintf(log, "stream_graph_recheck hits=%ld calls=%ld thread_mismatch=%ld\n", recheckHits,
                InterlockedCompareExchange(&streamRecheckCalls, 0, 0),
                InterlockedCompareExchange(&streamRecheckThreadMismatch, 0, 0));
            lastRecheckHits = recheckHits;
        }
        static LONG lastLifecycleTotal = -1;
        static LONG lastGraphMissing = -99;
        const LONG graphMissing = InterlockedCompareExchange(&streamGraphMissing, 0, 0);
        if (streamProbe && graphMissing != lastGraphMissing) {
            std::fprintf(log, "stream_graph missing_index=%ld name=%s asset_behavior=unverified\n", graphMissing,
                graphMissing >= 0 && graphMissing < static_cast<LONG>(std::size(ds2::kStreamRequirements)) ?
                ds2::kStreamRequirements[graphMissing].name : "none_or_snapshot_invalid");
            lastGraphMissing = graphMissing;
        }
        const LONG releases = InterlockedCompareExchange(&streamReleaseCalls, 0, 0);
        const LONG resumes = InterlockedCompareExchange(&streamResumes, 0, 0);
        const LONG epochs = InterlockedCompareExchange(&streamServiceEpoch, 0, 0);
        if (lastLifecycleTotal != releases + resumes + epochs) {
            std::fprintf(log, "stream_lifecycle releases=%ld unlocked=%ld unlock_failures=%ld resumes=%ld service_retire_epoch=%ld held=%ld late=%ld swept=%ld\n",
                releases, InterlockedCompareExchange(&streamReleaseUnlocks, 0, 0),
                InterlockedCompareExchange(&streamReleaseFailures, 0, 0), resumes, epochs,
                InterlockedCompareExchange(&streamReleaseHeld, 0, 0),
                InterlockedCompareExchange(&streamReleaseLate, 0, 0),
                InterlockedCompareExchange(&streamReleaseSwept, 0, 0));
            lastLifecycleTotal = releases + resumes + epochs;
        }
        {
            static LONG lastModelStreams = -1;
            const LONG requested = InterlockedCompareExchange(&modelStreamsRequested, 0, 0);
            const LONG ready = InterlockedCompareExchange(&modelStreamsReady, 0, 0);
            const LONG released = InterlockedCompareExchange(&modelStreamsReleased, 0, 0);
            if (ModelStreamCount() && lastModelStreams != requested + ready + released) {
                std::fprintf(log, "model_streams count=%u requested=%ld locked=%ld failed=%ld ready=%ld released=%ld\n",
                    ModelStreamCount(), requested, InterlockedCompareExchange(&modelStreamsLocked, 0, 0),
                    InterlockedCompareExchange(&modelStreamsFailed, 0, 0), ready, released);
                for (unsigned i = 0; i < ModelStreamCount() && i < std::size(modelStreams); ++i)
                    std::fprintf(log, "model_stream name=%s handle=%08X locked=%d status=%d\n", ds2::kModelStreams[i],
                        modelStreams[i].handle, modelStreams[i].locked ? 1 : 0, modelStreams[i].status);
                lastModelStreams = requested + ready + released;
            }
        }
        static LONG lastPinTotal = -1;
        const LONG pinTotal = InterlockedCompareExchange(&streamPinned, 0, 0) +
            InterlockedCompareExchange(&streamUnpinned, 0, 0) +
            InterlockedCompareExchange(&streamPinFailures, 0, 0);
        if (pinTotal != lastPinTotal) {
            lastPinTotal = pinTotal;
            std::fprintf(log, "stream_pins enabled=%d pinned=%ld pin_failures=%ld unpinned=%ld unpin_failures=%ld\n",
                pinPrerequisites, InterlockedCompareExchange(&streamPinned, 0, 0),
                InterlockedCompareExchange(&streamPinFailures, 0, 0),
                InterlockedCompareExchange(&streamUnpinned, 0, 0),
                InterlockedCompareExchange(&streamUnpinFailures, 0, 0));
        }
        if (registryDiagnostics && registryReports < 60 && GetTickCount() - lastRegistry >= 5000) {
            uint32_t manager = 0;
            if (!Read(base + 0x1C1C098, &manager, 4)) manager = 0;
            const auto snapshot = ds2::native::inspectRegistry(manager, Read);
            const auto streamStates = ds2::native::inspectStreamStates(manager, Read);
            unsigned states[6]{};
            for (const auto& entry : streamStates.entries) ++states[entry.status];
            for (const auto& entry : streamStates.entries) {
                if (entry.handle != static_cast<uint32_t>(InterlockedCompareExchange(&streamProbeHandle, 0, 0))) continue;
                std::fprintf(log, "stream_root handle=%08X status=%u notification_bits=%08X wait_handle_count=%u resource_closure=unverified\n",
                    entry.handle, entry.status, entry.notificationBits, entry.waitHandleCount);
            }
            std::fprintf(log, "stream_states snapshot_result=%u sampled=%zu state0=%u state1=%u state2=%u state3=%u state4=%u state5=%u readiness=unverified\n",
                static_cast<unsigned>(streamStates.result), streamStates.entries.size(),
                states[0], states[1], states[2], states[3], states[4], states[5]);
            std::fprintf(log, "stream_registry status=%s manager=%08X count=%u registration_ready=unverified\n",
                ds2::native::registryStatusName(snapshot.status), manager, snapshot.count);
            if (snapshot.status == ds2::native::RegistryStatus::Stable) {
                for (unsigned i = 0; i < snapshot.count; ++i)
                    std::fprintf(log, "stream_toc slot=%u pointer=%08X refs_flags=%04X\n",
                        i, snapshot.tocs[i], snapshot.references[i]);
                const struct { uint32_t hash; const char* name; } prerequisites[] = {
                    {0x01A2021D, "global_assets\\global_assets"},
                    {0, "global_assets\\global_assets_global"},
                    {0, "text_assets\\text_assets_global"}
                };
                for (const auto& required : prerequisites) {
                    const auto result = ds2::native::findDescriptor(snapshot, required.hash, required.name, Read);
                    std::fprintf(log, "stream_prerequisite name=%s result=%s residency=unverified\n",
                        required.name, ds2::native::descriptorStatusName(result));
                }
                const auto after = ds2::native::inspectRegistry(manager, Read);
                std::fprintf(log, "stream_prerequisite_snapshot unchanged=%d\n",
                    after.status == ds2::native::RegistryStatus::Stable && after.tocs == snapshot.tocs);
            }
            lastRegistry = GetTickCount();
            if (++registryReports == 60)
                std::fprintf(log, "stream_registry report_limit_reached=60 no_further_snapshots=1\n");
        }
        while (detailLimit) {
            auto& event = events[static_cast<unsigned>(next - 1) % detailLimit];
            const LONG published = InterlockedCompareExchange(&event.ready, 0, 0);
            if (published < next) break;
            if (published > next) {
                // The game outran this loop by more than one ring. Say so and
                // catch up rather than wait forever for a number that is gone.
                std::fprintf(log, "spawn_detail_overrun skipped=%ld\n", published - next);
                next = published;
            }
            const auto found = catalog.find(event.target);
            const char* label = !event.readable ? "read_failed" :
                found == catalog.end() ? "unknown_target" : found->second.c_str();
            std::fprintf(log, "spawn=%ld source=%08X target=%08X-%08X-%08X-%08X actor=%08X packet_attrs=%08X result=%s catalog=%s\n",
                next, event.source, event.target[0], event.target[1], event.target[2], event.target[3],
                event.actor, event.packetAttributes, event.actor ? "created" : "creation_failed", label);
            const auto source = std::lower_bound(pool.begin(), pool.end(), event.target,
                [](const auto& record, const Guid& guid) { return record.guid < guid; });
            const auto target = std::lower_bound(pool.begin(), pool.end(), event.selected,
                [](const auto& record, const Guid& guid) { return record.guid < guid; });
            std::fprintf(log, "selection=%s attempts=%u tick=%u source_family=%s target_family=%s selected=%08X-%08X-%08X-%08X\n",
                ds2::statusName(event.selection), event.attempts, event.tick,
                source != pool.end() && source->guid == event.target ? source->family.c_str() : "unknown",
                target != pool.end() && target->guid == event.selected ? target->family.c_str() : "none",
                event.selected[0], event.selected[1], event.selected[2], event.selected[3]);
            if (target != pool.end() && target->guid == event.selected && !target->variantName.empty())
                std::fprintf(log, "variant=%s\n", target->variantName.c_str());
            // Equal handles mean the level owns this enemy and will tear it
            // down like a vanilla one.
            if (event.levelHandle || event.donorHandle)
                std::fprintf(log, "stream_owner level=%08X donor=%08X followed=%d\n",
                    event.levelHandle, event.donorHandle,
                    event.levelHandle && event.levelHandle == event.donorHandle ? 1 : 0);
            if (source != pool.end() && source->guid == event.target)
                std::fprintf(log, "arrival=%s exact_vent_contract=%d\n", source->context.c_str(),
                    !source->arrivalKey.empty());
            if (event.bindingSource || event.bindingResult)
                std::fprintf(log, "vent_binding owner=%08X source_header=%08X donor_header=%08X result=%u applied=%d\n",
                    event.bindingOwner, event.bindingSource, event.bindingDonor, event.bindingResult, event.bindingApplied);
            ++next;
        }
        const DWORD now = GetTickCount();
        const LONG total = InterlockedCompareExchange(&calls, 0, 0);
        if (now - lastSummary >= 5000 && total != lastCalls) {
            std::fprintf(log, "summary calls=%ld created=%ld failed=%ld replaced_created=%ld frame_overflows=%ld details=%ld/%u\n",
                total, InterlockedCompareExchange(&created, 0, 0), InterlockedCompareExchange(&failed, 0, 0),
                InterlockedCompareExchange(&replacedCreated, 0, 0), InterlockedCompareExchange(&frameOverflows, 0, 0), next - 1, detailLimit);
            for (unsigned i = 0; i < static_cast<unsigned>(ds2::SelectionStatus::Count); ++i) {
                const LONG count = InterlockedCompareExchange(&statusCounts[i], 0, 0);
                if (count) std::fprintf(log, "reason=%s count=%ld\n", ds2::statusName(i), count);
            }
            // `cleared` counts the foreign names taken off a replacement. Those
            // are the ones that crash the engine once their own level is gone.
            // `unregistered` counts the source names the engine's own table did
            // not know. Writing one would have armed the release function to
            // decrement a null entry, which is the 0x007564B9 crash.
            std::fprintf(log, "death_message written=%u cleared=%u unregistered=%u refused=%u last_result=%s header=%08X\n",
                deathMessagesWritten, deathMessagesCleared, deathMessagesUnregistered,
                deathMessagesRefused, lastDeathMessageRefusal, lastDeathMessageHeader);
            // `relaxed` counts the swaps that only became possible with the
            // spawn-done message left out of the contract. Those depend on the
            // rebind handing over the source's spawning component, so a `missed`
            // next to a non-zero `relaxed` is the one combination to watch: it
            // would mean a room is waiting for a message nobody sent.
            std::fprintf(log, "vent_binding armed=%ld applied=%ld missed=%ld relaxed=%ld\n",
                InterlockedCompareExchange(&bindingArmed, 0, 0), InterlockedCompareExchange(&bindingApplied, 0, 0),
                InterlockedCompareExchange(&bindingMissed, 0, 0),
                InterlockedCompareExchange(&relaxedVentSwaps, 0, 0));
            if (pool_ownership::createHook) pool_ownership::Summary(log);
            navpower::Summary(log);
            if (variant_runtime::speedHook) variant_runtime::Summary(log);
            lastCalls = total;
            lastSummary = now;
        }
        message_trace::Drain(log);
        if (variant_runtime::attachHook) variant_runtime::FlushAttach(log);
        if (variant_runtime::emxHook || variant_runtime::emxPlainHook) variant_runtime::FlushEmx(log);
        if (texture_probe::enabled) texture_probe::Flush(log);
        std::fflush(log);
        Sleep(1000);
    }
}
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, Worker, module, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
