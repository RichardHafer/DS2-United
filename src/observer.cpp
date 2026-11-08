// Dead Space 2 United by Richard Hafer.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// The plain CRT calls (fopen, sscanf, strncpy) are used on purpose; silences C4996.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include "ds2coop_config.h"
#include <array>
#include <cstdarg>
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
#include "generated_scene_actors.h"
#include "native_layout.h"
#include "stream_registry.h"
#include "stream_states.h"
#include "log_rotation.h"
#include "component_rebinding.h"
#include "construction_binding.h"
#include "generated_stream_requirements.h"
#include "generated_global_targets.h"
#include "global_stream_lifetime.h"
#include "global_pool.h"
#include "survival.h"
#include "coop_progress.h"
#include "coop_mpmap.h"
#include "coop_arena.h"
#include "coop_difficulty.h"   // the difficulty's names on the survival HUD
#include "generated_survival_vents.h"
#include "generated_slasher_skins.h"      // the Versus slashers' skins per pool model
#include "generated_objective_table.h"   // the Objective mode's messages per MP map
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
// The survival director (runtime_survival.inc) rides on the frame tick and the
// state machine tick; set once it is installed.
void (*survivalFrameTick)() = nullptr;
void (*survivalMachineTick)() = nullptr;
// Called at the level's release points before the pool goes: the director ends
// its run and puts template values back while the pool is still there.
void (*survivalLevelEnding)(const char*) = nullptr;
// Pool enemies the director made in the current level, reset with the service.
volatile LONG survivalLevelSpawns = 0;
volatile LONG streamReleaseHeldSurvival = 0;
// [Survival] Enabled in DS2Survival.ini, read before the hooks go in.
bool survivalWanted = false;
// Master switch off with survival on: the randomizer runs for survival alone. The
// campaign keeps its own enemies (chance 0); MoreEnemies, hordes, extra spawns, loot,
// the bench and schematic randomizers and the watchdog stay off.
bool survivalOnly = false;
// The selection lookup moved: survival names that instead of the master switch.
bool lookupMismatch = false;
// A co-op client (DS2Coop_NetRole() == 2): the host's enemies reach it as
// ghosts and its own NPC spawns are suppressed, so it repeats nothing.
volatile LONG coopClientSkips = 0;
bool CoopClient() {
    using RoleFn = int(__cdecl*)();
    static RoleFn role = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (HMODULE coop = ds2coop::PartnerModule("DS2United.asi")) role = reinterpret_cast<RoleFn>(GetProcAddress(coop, "DS2Coop_NetRole"));
    }
    return role && role() == 2;
}
#include "runtime_spawner_tools.inc"
#include "runtime_placement.inc"
#include "runtime_selection.inc"
#include "runtime_vent_gate.inc"
#include "runtime_objective_watch.inc"
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
#include "runtime_swarm_guard.inc"
#include "runtime_guid_redirect.inc"
#include "runtime_loot.inc"
#include "runtime_exit_guard.inc"
#include "runtime_perfdiag.inc"
#include "runtime_frame_cap.inc"
#include "runtime_texture_probe.inc"
#include "runtime_sound_probe.inc"
#include "runtime_stream_watch.inc"
#include "runtime_zone_unload.inc"
#include "runtime_survival.inc"

// Some templates have a DSNavRoutingComponent without nav routing settings (21
// walkers in the catalog, among them the ch09 elevator ride's slasher_miner): with
// none the component builds no mover and the enemy never paths, it stood in
// survival until a player came within a few metres (13.10.). The component reads
// the settings GUID from its attributes once, on its first post-spawn message
// (NavRoutingComponent::HandleEvents 0x0136B6B0: attributes +0x00, the resolved
// SafePtr +0x70, flag +0x84 bit 5), so a GUID written into the template right
// after the spawn reaches this copy and every later one. The GUID is the one a
// navigating enemy of the same family came with (the slashers' own as a start).
namespace navrepair {
constexpr uint32_t kNavTypeRva = 0x1CA8062, kNavAttributes = 0x0C, kNavSettings = 0x70, kNavFlags = 0x84, kNavResolved = 0x20;
const Guid kSlasherRouting = {0x33CA6342u, 0x04D85EE8u, 0x4B4C4546u, 0x30315245u};
std::map<std::string, Guid> learned;
SRWLOCK lock = SRWLOCK_INIT;
volatile LONG learnedCount = 0, repaired = 0, repairedLate = 0, unknownFamily = 0, writeFailed = 0, lines = 0;
uint32_t Component(uint32_t entity) {
    unsigned char type = 0, index = 0;
    uint32_t types = 0, list = 0, component = 0, owner = 0;
    if (!entity || !Read(base + kNavTypeRva, &type, 1) || !type || !Read(entity + 0x44, &types, 4) || !types || !Read(entity + 0x48, &list, 4) || !list) return 0;
    if (!Read(types + type + 0xC, &index, 1) || !Read(list + index * 4u, &component, 4) || !component) return 0;
    return Read(component + 0x10, &owner, 4) && owner == entity ? component : 0;
}
// Game thread, from OnCreated.
void Check(uint32_t actor, const std::string& family) {
    const uint32_t component = Component(actor);
    uint32_t attributes = 0;
    Guid routing{};
    if (!component || !Read(component + kNavAttributes, &attributes, 4) || !attributes || !Read(attributes, routing.data(), 16)) return;
    AcquireSRWLockExclusive(&lock);
    if (routing != Guid{}) {
        if (!family.empty() && learned.emplace(family, routing).second) InterlockedIncrement(&learnedCount);
        ReleaseSRWLockExclusive(&lock);
        return;
    }
    const auto known = learned.find(family);
    const Guid use = known != learned.end() ? known->second : family == "Zombie" ? kSlasherRouting : Guid{};
    ReleaseSRWLockExclusive(&lock);
    if (use == Guid{}) {
        if (InterlockedIncrement(&unknownFamily) <= 10) survival_rt::Event("navrepair actor=%08X family=%s: no nav routing settings and none known for the family yet", actor,
                                                                    family.c_str());
        return;
    }
    uint32_t flags = 0;
    Read(component + kNavFlags, &flags, 4);
    const bool late = (flags & kNavResolved) != 0;
    if (!WriteBytes(attributes, use.data(), 16)) { InterlockedIncrement(&writeFailed); return; }
    InterlockedIncrement(late ? &repairedLate : &repaired);
    if (InterlockedIncrement(&lines) <= 30)
        survival_rt::Event("navrepair actor=%08X family=%s: its template had no nav routing settings; %08X-%08X-%08X-%08X written into %08X%s", actor, family.c_str(),
                     use[0], use[1], use[2], use[3], attributes, late ? " (this copy already looked: the next copies path)" : "");
}
}  // namespace navrepair

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
    // What a forced spawn's caller needs to know beyond the actor.
    if (extraSpawnActive) {
        lastCreatedStatus = saved.status;
        lastCreatedArmed = saved.binding.packet != 0;
        lastCreatedApplied = saved.applied;
        lastCreatedBind = saved.bindResult;
    }
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
        // The survival director watches its own enemies (a wave fight is long,
        // the leash would cut it short).
        if (!survivalSpawnActive) ds2_watchdog_runtime::Track(ctx.eax, saved.guid, allowKill, leash, reader);
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
        // A survival spawn has no catalog source (an MP slasher or a prop), so it
        // is treated as a source without messages: the donor's own, named for a
        // campaign room, are cleared and nothing on the MP map hears them.
        static const ds2::EnemyRecord kNoSource{};
        const ds2::EnemyRecord& sourceOrNone = haveSource ? *sourceRecord : kNoSource;
        if (haveDonor && (haveSource || survivalSpawnActive)) {
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
                 sourceOrNone.deathMessage, donor->deathMessage},
                {ds2::kHealthModifiedFields, std::size(ds2::kHealthModifiedFields),
                 sourceOrNone.healthModifiedMessage, donor->healthModifiedMessage},
                {ds2::kSpawnDoneFields, std::size(ds2::kSpawnDoneFields),
                 sourceOrNone.spawnDoneMessage, donor->spawnDoneMessage},
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
    NoteSpawnerSpawn(ctx.edi);
    ventq::NoteArrival(ctx.edi, ctx.ebp, ctx.eax);
    if (ctx.eax) {
        // the family of what was spawned: the donor when one was chosen, else the authored target
        Guid spawned = saved.guid;
        if (spawned == Guid{}) Read(ctx.ebp + 0x0C, spawned.data(), 16);
        const auto record = std::lower_bound(pool.begin(), pool.end(), spawned, [](const auto& r, const Guid& g) { return r.guid < g; });
        navrepair::Check(ctx.eax, record != pool.end() && record->guid == spawned ? record->family : std::string());
    }
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

// The host's seed on a co-op client, see DS2Randomizer_AdoptSeed.
namespace seed_adopt {
uint32_t iniSeed = 0;
volatile LONG ready = 0, active = 0, changes = 0;
volatile LONG current = 0, adoptions = 0, reverts = 0, logged = -1, lastMs = 0;
}

DWORD WINAPI Worker(void* module) {
    char path[MAX_PATH]{};
    GetModuleFileNameA(static_cast<HMODULE>(module), path, MAX_PATH);
    std::string directory(path);
    directory.resize(directory.find_last_of("\\/") + 1);
    const std::string ini = ds2coop::ConfigFile(directory, "DS2Randomizer.ini");
    FILE* log = nullptr;
    // Keep the last runs. Comparing two players means comparing two runs, and
    // starting the game once used to destroy the log that was being compared.
    // The most recent finished run is DS2Randomizer.1.log, the one before it
    // .2.log, and so on up to .5.log; the oldest falls off the end.
    // A second copy of the game on this PC (DS2COOP / DS2COOP_PORT) writes its own
    // log and leaves the rotation alone: the first copy holds DS2Randomizer.log,
    // and a failed open used to switch the whole randomizer off in the second.
    char copyPort[64]{};
    if (!GetEnvironmentVariableA("DS2COOP_PORT", copyPort, sizeof(copyPort)) &&
        GetEnvironmentVariableA("DS2COOP", copyPort, sizeof(copyPort)))
        copyPort[std::strcspn(copyPort, ",")] = 0;
    std::string logPath = directory + "DS2Randomizer.log";
    if (copyPort[0]) {
        // A co-op copy's log was overwritten at every start; it keeps three runs now.
        log_rotation::Rotate(directory + "DS2Randomizer.coop" + copyPort);
        logPath = directory + "DS2Randomizer.coop" + copyPort + ".log";
    } else {
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
    }
    log = _fsopen(logPath.c_str(), "w", _SH_DENYWR);
    if (!log) {
        logPath = directory + "DS2Randomizer.pid" + std::to_string(GetCurrentProcessId()) + ".log";
        log = _fsopen(logPath.c_str(), "w", _SH_DENYWR);
    }
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
    survivalWanted = GetPrivateProfileIntA("Survival", "Enabled", 0, ds2coop::ConfigFile(directory, "DS2Survival.ini").c_str()) != 0;
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
    if (survivalOnly) {
        moreEnemies.enabled = false;
        extraSpawnsEnabled = false;
        DisableHordes();
        std::fprintf(log, "master off (Enabled=0) with [Survival] Enabled=1: survival only. The campaign keeps its own enemies (chance 0); "
                          "MoreEnemies, hordes, extra spawns, loot, the bench and schematic randomizers and the watchdog stay off\n");
    }
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
    std::fprintf(log, "direct_donors=%d pack_boy_stand_in=%s withdrawn_donors=%u (offered to the survival director) corpse_templates=%zu\n", directDonors,
                 directDonors && !withdrawnDonors ? "offered" : "withheld in the campaign", withdrawnDonors,
                 std::size(ds2::kCorpseTemplates));
    std::fprintf(log, "variants enabled=%d offered=%u\n", variantsEnabled ? 1 : 0, variantsLoaded);
    std::fprintf(log, "twitchers enabled=%d severed rows=%u (oracle %u, for the %u enhanced sources) skipped=%u campaign_chance twitcher=%u pregnant=%u "
                      "campaign_plans=%u (no candidate %u)%s%s\n",
                 twitchersEnabled ? 1 : 0, twitcherRowsLoaded, oracleRowsLoaded, enhancedSources, ds2::retargetRowsSkipped, settings.twitcherChance,
                 settings.pregnantChance, twitcherPlans,
                 twitcherPlansEmpty,
                 ds2::retargetSkipReason.empty() ? "" : " first skip: ", ds2::retargetSkipReason.c_str());
    if (twitchersRefused[0]) std::fprintf(log, "twitchers REFUSED: %s\n", twitchersRefused);
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
    // Enabled=0 is the master switch. Until 2026-09-17 it only stopped the
    // enemy replacement: the pool still loaded, the loading screen was still
    // taken over, and hordes, loot and the watchdog still ran. With it off
    // only the frame cap is installed, for players who use it as their
    // limiter. ObserveSpawns=1 does not override it (on 2026-09-17 it did,
    // and the switch looked dead).
    if (!settings.enabled) {
        std::fprintf(log, "master off (Enabled=0) and survival off: only the frame cap is installed (ObserveSpawns=%d ignored)\n", observeDetails ? 1 : 0);
        survival_rt::NotReady("survival is off ([Survival] Enabled=0)", "turn on Survival in DS2UnitedConfig.exe, or set [Survival] Enabled=1");
        std::fflush(log);
        for (unsigned attempt = 0; attempt < 300 && !Signature(); ++attempt) Sleep(1000);
        if (Signature()) {
            frame_cap::Install(log, ini);
            perfdiag::Install(log, ini);
            exit_guard::Install(log, ini, directory, frame_cap::installed);
        }
        std::fflush(log);
        for (;;) {
            Sleep(1000);
            exit_guard::Poll(log);
            if (frame_cap::installed) { frame_cap::WriteDiagnostics(log); perfdiag::WriteDiagnostics(log); }
            if (exit_guard::installed) exit_guard::WriteDiagnostics(log);
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
        survival_rt::NotReady("the randomizer never found the game code it needs (another game build)", "use the supported Steam build of the game");
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
        lookupMismatch = true;
        std::fprintf(log, "lookup signature mismatch; mutation disabled\n");
    }
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCSTR>(&OnCreated), &pinned)) {
        std::fprintf(log, "module pin failed; no hook installed\n");
        survival_rt::NotReady("the randomizer could not pin itself in memory", "restart the game; DS2Randomizer.log has the details");
        std::fclose(log); return 0;
    }
    if (spawnerToolsEnabled) {
        VerifySpawnerIdentity(log);
        PollSpawnerTools(log);
    }
    observer = safetyhook::create_mid(reinterpret_cast<void*>(base + 0xACB2D0), OnCreated);
    if (!observer) {
        std::fprintf(log, "observation hook failed\n");
        survival_rt::NotReady("the randomizer could not hook the spawn function", "another plugin may hook it; see DS2Randomizer.log");
        std::fclose(log); return 0;
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
    swarm_guard::Install(log);
    swarm_guard::InstallCollisionGuard(log);
    guid_redirect::Install(log);
    if (!survivalOnly) loot::Install(log, ini);
    frame_cap::Install(log, ini);
    perfdiag::Install(log, ini);
    exit_guard::Install(log, ini, directory, frame_cap::installed);
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
    if ((moreEnemies.enabled || HordesActive() || extraSpawnsEnabled || survivalWanted) && !InstallMoreEnemies(log)) {
        moreEnemies.enabled = false;
        DisableHordes();
        std::fprintf(log, "more_enemies disabled; authored spawns are untouched\n");
    }
    // Survival waves on a multiplayer map: needs the selection hook, the pool
    // and the FuncSpawn registry above, so it goes in after them.
    survival_rt::Install(log, directory);
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
    if (!survivalOnly) {
        ds2_schematic_runtime::Install(base, ini.c_str(), log);
        ds2_bench_runtime::Install(base, ini.c_str(), log);
    }
    ds2_service_guard::Install(base, ini.c_str(), log);
    ds2_watchdog_runtime::Configure(ini.c_str(), log);
    if (survivalOnly && ds2_watchdog_runtime::enabled) {
        ds2_watchdog_runtime::enabled = false;
        std::fprintf(log, "watchdog off: survival only (the master switch is off)\n");
    }
    ds2_watchdog_runtime::InstallKill(base, log);
    // Diagnostic only, off unless asked for: one line per script message.
    message_trace::enabled = ConfigInt(ini, "DS2Randomizer", "MessageTrace", 0, 0, 1) != 0;
    if (message_trace::enabled) message_trace::Install(log);
    texture_probe::enabled = ConfigInt(ini, "DS2Randomizer", "TextureProbe", 0, 0, 1) != 0;
    if (texture_probe::enabled) { texture_probe::Install(log); texture_probe::InstallSkinProbe(log); }
    // Diagnostic: which enemies ask for sounds and which are found.
    sound_probe::enabled = ConfigInt(ini, "DS2Randomizer", "SoundProbe", 0, 0, 1) != 0;
    if (sound_probe::enabled) sound_probe::Install(log);
    // The loading screen stays up for MinimumLoadingSeconds and, while the
    // pool is requested but not ready, up to MaximumLoadingSeconds.
    loading_hold::Install(log, ini.c_str(), streamProbe);
    // Only the pool's own references can hold a zone switch up.
    if (streamProbe) zone_unload::Install(log, ini);
    std::fflush(log);
    seed_adopt::iniSeed = settings.seed;
    InterlockedExchange(&seed_adopt::ready, 1);
    LONG moreEnemiesFlushed = 0;
    for (;;) {
        PollSpawnerTools(log);
        ds2_schematic_runtime::WriteDiagnostics(log);
        if (const LONG changes = InterlockedCompareExchange(&seed_adopt::changes, 0, 0); changes != seed_adopt::logged) {
            seed_adopt::logged = changes;
            const LONG adopted = InterlockedCompareExchange(&seed_adopt::current, 0, 0);
            if (InterlockedCompareExchange(&seed_adopt::active, 0, 0))
                std::fprintf(log, "seed: the host's seed %lu is used (INI seed %u), enemy plans and blueprints rebuilt in %ld ms; the bench keeps the INI seed; adoptions=%ld\n",
                             static_cast<unsigned long>(adopted), seed_adopt::iniSeed, InterlockedCompareExchange(&seed_adopt::lastMs, 0, 0),
                             InterlockedCompareExchange(&seed_adopt::adoptions, 0, 0));
            else if (InterlockedCompareExchange(&seed_adopt::reverts, 0, 0))
                std::fprintf(log, "seed: back to the INI seed %u; reverts=%ld\n", seed_adopt::iniSeed, InterlockedCompareExchange(&seed_adopt::reverts, 0, 0));
        }
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
        text_packs::FollowUp(log);
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
        if (swarm_guard::installed || swarm_guard::collisionInstalled) swarm_guard::WriteDiagnostics(log);
        if (guid_redirect::installed) guid_redirect::WriteDiagnostics(log);
        if (loot::installed) loot::WriteDiagnostics(log);
        if (frame_cap::installed) { frame_cap::WriteDiagnostics(log); perfdiag::WriteDiagnostics(log); }
        exit_guard::Poll(log);
        if (exit_guard::installed) exit_guard::WriteDiagnostics(log);
        if (placement::installed) placement::WriteDiagnostics(log);
        survival_rt::WriteDiagnostics(log);
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
            std::fprintf(log, "stream_lifecycle releases=%ld unlocked=%ld unlock_failures=%ld resumes=%ld service_retire_epoch=%ld held=%ld late=%ld swept=%ld "
                         "held_for_survival=%ld survival_level_spawns=%ld spawn_record_unloads=%ld (prerequisites %ld, of them with a newer twin %ld; still held by another stream %ld) spawn_record_unload_faults=%ld\n",
                releases, InterlockedCompareExchange(&streamReleaseUnlocks, 0, 0),
                InterlockedCompareExchange(&streamReleaseFailures, 0, 0), resumes, epochs,
                InterlockedCompareExchange(&streamReleaseHeld, 0, 0),
                InterlockedCompareExchange(&streamReleaseLate, 0, 0),
                InterlockedCompareExchange(&streamReleaseSwept, 0, 0),
                InterlockedCompareExchange(&streamReleaseHeldSurvival, 0, 0), InterlockedCompareExchange(&survivalLevelSpawns, 0, 0),
                InterlockedCompareExchange(&spawnRecordUnloads, 0, 0), InterlockedCompareExchange(&prerequisiteRecordUnloads, 0, 0),
                InterlockedCompareExchange(&prerequisiteRecordsKept, 0, 0), InterlockedCompareExchange(&prerequisiteRecordsShared, 0, 0),
                InterlockedCompareExchange(&spawnRecordUnloadFaults, 0, 0));
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
        sound_probe::Summary(log);
        std::fflush(log);
        Sleep(1000);
    }
}
}

// For the co-op plugin's survival shop: 0 off, 1 arming, 2 wave, 3 intermission,
// 4 dead; *wave gets the current wave. Any thread.
extern "C" __declspec(dllexport) int __cdecl DS2Survival_State(int* wave) {
    if (wave) *wave = static_cast<int>(InterlockedCompareExchange(&survival_rt::waveMirror, 0, 0));
    return static_cast<int>(InterlockedCompareExchange(&survival_rt::stateMirror, 0, 0));
}

// A co-op client plays the host's seed (0 = back to the INI seed). Game thread only:
// the spawn hooks read the plans there too. Enemy picks and blueprints built from
// now on follow it; the bench keeps the INI seed, its trees are read at start.
// Returns 0 when nothing changed, else 1 enemy plans rebuilt | 2 blueprints reseeded | 4 seed set.
extern "C" __declspec(dllexport) int __cdecl DS2Randomizer_SetSessionSeed(unsigned seed, int active) {
    if (!InterlockedCompareExchange(&seed_adopt::ready, 0, 0)) return -1;
    const uint32_t wanted = active ? seed : seed_adopt::iniSeed;
    InterlockedExchange(&seed_adopt::active, active != 0);
    const bool changed = wanted != settings.seed;
    int result = changed ? 4 : 0;
    const DWORD started = GetTickCount();
    settings.seed = wanted;
    if (changed && settings.enabled && !pool.empty()) {
        std::vector<ds2::SelectionPlan> fresh, freshAllowed;
        fresh.reserve(pool.size());
        freshAllowed.reserve(pool.size());
        for (const auto& source : pool) {
            fresh.push_back(ds2::buildPlan(settings, source, pool, maxDonorLookups));
            freshAllowed.push_back(ds2::buildAllowedSpawnerPlan(settings, source, pool, maxDonorLookups));
        }
        plans.swap(fresh);
        allowedSpawnerPlans.swap(freshAllowed);
        result |= 1;
    }
    if (changed && ds2_schematic_runtime::Reseed(wanted, nullptr)) result |= 2;
    InterlockedExchange(&seed_adopt::current, static_cast<LONG>(seed));
    InterlockedIncrement(active ? &seed_adopt::adoptions : &seed_adopt::reverts);
    InterlockedExchange(&seed_adopt::lastMs, static_cast<LONG>(GetTickCount() - started));
    InterlockedIncrement(&seed_adopt::changes);
    return result;
}

extern "C" __declspec(dllexport) int __cdecl DS2Randomizer_AdoptSeed(unsigned seed) {
    return DS2Randomizer_SetSessionSeed(seed, seed != 0);
}

// For the co-op plugin's arena stream sets: where the living enemies and the spawners they
// came out of are (x y z each), so their rooms stay loaded. Any thread.
extern "C" __declspec(dllexport) int __cdecl DS2Survival_KeepPoints(float* xyz, int max) { return survival_rt::KeepPoints(xyz, max); }

// For the co-op menu, before a launch: can the director play `map` in game `mode`?
// 1 yes, 2 yes with a warning, 0 no, -1 still starting; why and fix are filled for
// everything but 1. Any thread (the settings are fixed once readiness is set).
extern "C" __declspec(dllexport) int __cdecl DS2Survival_Ready(const char* map, int mode, char* why, int whySize, char* fix, int fixSize) {
    auto say = [&](int result, const char* w, const char* f) {
        if (why && whySize > 0) std::snprintf(why, static_cast<size_t>(whySize), "%s", w);
        if (fix && fixSize > 0) std::snprintf(fix, static_cast<size_t>(fixSize), "%s", f);
        return result;
    };
    const LONG state = InterlockedCompareExchange(&survival_rt::readiness, 0, 0);
    const bool mp = map && coop_mpmap::IsMpMap(map);
    if (mode == 4 && mp) {
        char w[240];
        if (state != 1) {
            std::snprintf(w, sizeof(w), "versus without AI slashers: %s", state ? survival_rt::notReadyWhy : "the randomizer is still starting");
            return say(2, w, state ? survival_rt::notReadyFix : "wait a few seconds and pick the map again");
        }
        if (!survival_rt::cfg.versus.enabled)
            return say(2, "versus without AI slashers ([VersusSlashers] Enabled=0)", "turn on Versus slashers in DS2UnitedConfig.exe for AI enemies");
        return say(1, "", "");
    }
    if (state == 0) return say(-1, "the randomizer is still starting", "wait a few seconds and pick the map again");
    if (state != 1) return say(0, survival_rt::notReadyWhy, survival_rt::notReadyFix);
    if (map && map[0] && !survival_rt::MapAllowed(map)) {
        char w[160];
        std::snprintf(w, sizeof(w), "survival plays on %s only ([Survival] Map=%s)", survival_rt::cfg.map.c_str(), survival_rt::cfg.map.c_str());
        return say(0, w, "set Map to any in DS2UnitedConfig.exe (Survival page), or [Survival] Map=any");
    }
    return say(1, "", "");
}

// For the co-op plugin: a message it replayed for a partner. Its replays call the
// dispatch behind OnDispatchMessage, so the MP maps' vent windows and objective
// count would never see them. Game thread.
extern "C" __declspec(dllexport) void __cdecl DS2Survival_NoteMessage(uint32_t hash) {
    if (!hash) return;
    vent_gate::Note(hash);
    objective_watch::Note(hash);
}

// The co-op debug menu's HOST tab: a host tool for the director (survival_rt::admin,
// kStartNow 1 .. kRestartRun 7). 1 when queued for the director's tick. Any thread.
extern "C" __declspec(dllexport) int __cdecl DS2Survival_Admin(int command, int value) {
    return survival_rt::admin::Queue(command, value) ? 1 : 0;
}
// The director's line for the panel and the newest tool result ("!" in front: refused).
// Returns the result's sequence number. Any thread.
extern "C" __declspec(dllexport) int __cdecl DS2Survival_AdminInfo(char* info, int infoSize, char* result, int resultSize) {
    AcquireSRWLockShared(&survival_rt::admin::lock);
    if (info && infoSize > 0) std::snprintf(info, static_cast<size_t>(infoSize), "%s", survival_rt::admin::info);
    if (result && resultSize > 0) std::snprintf(result, static_cast<size_t>(resultSize), "%s", survival_rt::admin::result);
    ReleaseSRWLockShared(&survival_rt::admin::lock);
    return static_cast<int>(InterlockedCompareExchange(&survival_rt::admin::resultSeq, 0, 0));
}
// The kinds a wave may ask for, for the HOST tab's spawn row: the name of `index`;
// returns how many there are.
extern "C" __declspec(dllexport) int __cdecl DS2Survival_AdminFamily(int index, char* out, int size) {
    const int count = static_cast<int>(std::size(survival::kFamilies));
    if (out && size > 0) std::snprintf(out, static_cast<size_t>(size), "%s", index >= 0 && index < count ? survival::kFamilies[index].family : "");
    return count;
}

// The escalation for wave n, for the co-op plugin (bot hits, shop prices, boss
// stasis): out[0] level, [1] bot damage %, [2] price %, [3] boss stasis resist %,
// [4] enhanced chance %. Returns how many were written.
extern "C" __declspec(dllexport) int __cdecl DS2Survival_EscalationFor(int wave, int* out, int count) {
    if (!out || count <= 0 || wave < 0) return 0;
    const unsigned n = static_cast<unsigned>(wave);
    const auto& c = survival_rt::cfg;
    const int values[] = {static_cast<int>(survival::EscLevel(n, c)), static_cast<int>(survival::BotDamagePercent(n, c)), static_cast<int>(survival::PricePercent(n, c)),
                          static_cast<int>(survival::BossStasisResist(n, c)), static_cast<int>(survival::EnhancedChance(n, c))};
    int written = 0;
    for (; written < count && written < static_cast<int>(std::size(values)); ++written) out[written] = values[written];
    return written;
}

// In DS2United.asi (DS2COOP_MODULE) ds2coop_main.cpp's DllMain calls this half.
#ifdef DS2COOP_MODULE
extern "C" BOOL WINAPI DS2Coop_RandomizerMain(HINSTANCE module, DWORD reason, LPVOID) {
#else
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
#endif
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, Worker, module, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
