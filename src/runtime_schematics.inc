// Dead Space 2 United by Richard Hafer.
// Include at file scope. Install once before loading a campaign.
#include <windows.h>
#include <mutex>
#include <cstdio>
#include <optional>
#include <algorithm>
#include <array>
#include "generated_schematic_catalog.h"
#include "generated_schematic_blueprints.h"
#include "generated_schematic_store_levels.h"
#include "../third_party/safetyhook/safetyhook.hpp"
namespace ds2_schematic_runtime {
using namespace ds2::schematics;
constexpr size_t kCount = sizeof(kCampaignSources) / sizeof(kCampaignSources[0]);
constexpr uintptr_t kConstructorRva = 0x9D80A1;
constexpr uintptr_t kVtableRva = 0x174F560;
constexpr uintptr_t kDescriptorRva = 0x1C98DC8;
constexpr size_t kDetailCapacity = 128;
enum class Outcome { ReadFailed, WrongClass, UnknownIdentity, WrongDefinition,
    UnexpectedReward, UnexpectedVisual, ReadOnlyAttributes, ReadOnlyActor,
    AttributeWriteFailed, ActorWriteFailed, VerificationFailed, Applied, AlreadySelected, Count };
struct Detail {
    unsigned actor = 0, attributes = 0, thread = 0;
    Guid entity{}, original{}, selected{};
    size_t source = kCount, target = kCount;
    Outcome outcome = Outcome::ReadFailed;
    bool rollbackSucceeded = false;
};
std::mutex lock;
uintptr_t moduleBase = 0;
bool active = false;
unsigned seed = 1;
bool followsSessionSeed = true;
bool firstIsWeapon = true, secondIsSuit = true;
std::optional<Policy<kCount>> policy;
void BuildPolicy(FILE* log);
safetyhook::MidHook constructorHook;
std::array<unsigned long long, static_cast<size_t>(Outcome::Count)> counts{};
std::array<Detail, kDetailCapacity> details{};
size_t queued = 0;
unsigned long long dropped = 0, calls = 0, lastPrinted = 0;

bool ReadMemory(uintptr_t address, void* output, size_t bytes) {
    SIZE_T done = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
        output, bytes, &done) && done == bytes;
}
bool Writable(uintptr_t address, size_t bytes) {
    if (!address || !bytes || address + bytes < address) return false;
    const uintptr_t end = address + bytes;
    while (address < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) ||
            region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD mode = region.Protect & 0xFF;
        if (mode != PAGE_READWRITE && mode != PAGE_WRITECOPY &&
            mode != PAGE_EXECUTE_READWRITE && mode != PAGE_EXECUTE_WRITECOPY) return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (next <= address) return false;
        address = next;
    }
    return true;
}
bool WriteMemory(uintptr_t address, const void* input, size_t bytes) {
    SIZE_T done = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
        input, bytes, &done) && done == bytes;
}
void Record(Detail detail, Outcome outcome) {
    detail.outcome = outcome;
    ++counts[static_cast<size_t>(outcome)];
    if (queued < details.size()) details[queued++] = detail;
    else ++dropped;
}

void OnConstructed(SafetyHookContext& context) {
    std::lock_guard<std::mutex> guard(lock);
    if (!active) return;
    ++calls;
    Detail detail;
    detail.actor = context.esi;
    detail.thread = GetCurrentThreadId();
    unsigned vtable = 0, descriptor = 0;
    AttributeBytes original{};
    if (!ReadMemory(detail.actor, &vtable, 4) ||
        !ReadMemory(detail.actor + 0x0C, &detail.attributes, 4) || detail.attributes < 0x10 ||
        !ReadMemory(detail.attributes - 0x10, &descriptor, 4) ||
        !ReadMemory(detail.actor + 0x24, detail.entity.data(), 16) ||
        !ReadMemory(detail.actor + 0x350, detail.original.data(), 16) ||
        !ReadMemory(detail.attributes, original.data(), original.size())) {
        Record(detail, Outcome::ReadFailed); return;
    }
    if (vtable != moduleBase + kVtableRva || descriptor != moduleBase + kDescriptorRva) {
        Record(detail, Outcome::WrongClass); return;
    }
    auto changed = original;
    const auto result = PrepareAttributes(kCampaignSources, kCampaignBlueprints, *policy,
        detail.entity, detail.original, changed, detail.selected, detail.source, detail.target);
    switch (result) {
    case AttributeResult::UnknownIdentity: Record(detail, Outcome::UnknownIdentity); return;
    case AttributeResult::WrongDefinition: Record(detail, Outcome::WrongDefinition); return;
    case AttributeResult::UnexpectedReward: Record(detail, Outcome::UnexpectedReward); return;
    case AttributeResult::UnexpectedVisual: Record(detail, Outcome::UnexpectedVisual); return;
    case AttributeResult::AlreadySelected: Record(detail, Outcome::AlreadySelected); return;
    case AttributeResult::Prepared: break;
    }
    if (!Writable(detail.attributes, changed.size())) {
        Record(detail, Outcome::ReadOnlyAttributes); return;
    }
    if (!Writable(detail.actor + 0x350, 16)) {
        Record(detail, Outcome::ReadOnlyActor); return;
    }
    // The payload contains values and relative offsets; its owner header stays intact.
    if (!WriteMemory(detail.attributes, changed.data(), changed.size())) {
        detail.rollbackSucceeded = WriteMemory(detail.attributes, original.data(), original.size());
        Record(detail, Outcome::AttributeWriteFailed); return;
    }
    if (!WriteMemory(detail.actor + 0x350, detail.selected.data(), 16)) {
        const bool restoredPayload = WriteMemory(detail.attributes, original.data(), original.size());
        const bool restoredActor = WriteMemory(detail.actor + 0x350, detail.original.data(), 16);
        detail.rollbackSucceeded = restoredPayload && restoredActor;
        Record(detail, Outcome::ActorWriteFailed); return;
    }
    AttributeBytes checked{};
    Guid actual{};
    if (!ReadMemory(detail.attributes, checked.data(), checked.size()) || checked != changed ||
        !ReadMemory(detail.actor + 0x350, actual.data(), 16) || actual != detail.selected) {
        const bool restoredPayload = WriteMemory(detail.attributes, original.data(), original.size());
        const bool restoredActor = WriteMemory(detail.actor + 0x350, detail.original.data(), 16);
        detail.rollbackSucceeded = restoredPayload && restoredActor;
        Record(detail, Outcome::VerificationFailed); return;
    }
    Record(detail, Outcome::Applied);
}

// The store chapter follows the schematic.
//
// PlayerStore::AddItem lists an item once its CMD_SetMinLevelReq is <= the
// chapter ([PlayerStore+0x38]), found or not; a schematic pickup skips that
// test (flag at entry+0x19). So a reward whose schematic was moved still came
// back through the store at its authored chapter, usually one or two chapters
// after its old place. Here every reward takes the store chapter of the place
// its schematic now lies at: what was found at a place in vanilla, and when the
// store would have offered it anyway, now belongs to that place.
//
//   00BB5A1D  mov eax,[esi+1Ch]   ; entry MinLevelReq (cached from the template)
//   00BB5A20  cmp eax,[edi+38h]   ; chapter      <- hook, eax is replaced
//   00BB5A23  jg  00BB5A2D        ; not yet
// esi = store entry, its first 16 bytes the item GUID; edi = PlayerStore.
constexpr uintptr_t kStoreGateRva = 0x7B5A20;
bool CheckSignature(FILE* log, const char* name, uintptr_t rva, const unsigned char* expected, size_t size);
struct StoreShift { Guid reward{}; int vanilla = -1, shifted = -1; const char* name = ""; LONG loggedChapter = -1; };
std::array<StoreShift, kCount> storeShifts{};
size_t storeShiftCount = 0;
safetyhook::MidHook storeHook;
volatile LONG storeChecks = 0, storeShifted = 0, storeMismatch = 0;
struct StoreEvent { LONG ready; unsigned shift; int chapter; };
std::array<StoreEvent, 96> storeEvents{};
volatile LONG storeEventCount = 0;
LONG storeEventsWritten = 0;

int StoreLevelOf(const Guid& reward) {
    for (const auto& level : kStoreLevels) if (level.reward == reward) return level.minLevel;
    return -1;
}

void OnStoreGate(SafetyHookContext& context) {
    Guid item{};
    if (!ReadMemory(context.esi, item.data(), 16)) return;
    for (size_t i = 0; i < storeShiftCount; ++i) {
        auto& shift = storeShifts[i];
        if (shift.reward != item) continue;
        InterlockedIncrement(&storeChecks);
        // Only the authored value is moved; anything else was set by someone on purpose.
        if (static_cast<int>(context.eax) != shift.vanilla) { InterlockedIncrement(&storeMismatch); return; }
        context.eax = static_cast<uintptr_t>(shift.shifted);
        InterlockedIncrement(&storeShifted);
        int chapter = 0;
        ReadMemory(context.edi + 0x38, &chapter, 4);
        // one line per item and chapter, the gate runs on every kiosk refresh
        if (InterlockedExchange(&shift.loggedChapter, chapter) != chapter) {
            const LONG slot = InterlockedIncrement(&storeEventCount) - 1;
            if (slot >= 0 && slot < static_cast<LONG>(storeEvents.size())) {
                storeEvents[slot].shift = static_cast<unsigned>(i);
                storeEvents[slot].chapter = chapter;
                InterlockedExchange(&storeEvents[slot].ready, 1);
            }
        }
        return;
    }
}

bool InstallStoreGate(const char* ini, FILE* log) {
    if (GetPrivateProfileIntA("SchematicRandomizer", "StoreFollowsSchematics", 1, ini) == 0) {
        std::fprintf(log, "schematics store: StoreFollowsSchematics=0, items unlock at their authored chapter\n");
        return false;
    }
    for (size_t i = 0; i < kCount; ++i) {
        Guid selected{};
        if (policy->Select(kCampaignSources[i].entity, kCampaignSources[i].kind, kCampaignSources[i].reward, selected) != Result::Selected)
            continue;
        const int place = StoreLevelOf(kCampaignSources[i].reward), own = StoreLevelOf(selected);
        const char* name = "?";
        for (const auto& level : kStoreLevels) if (level.reward == selected) name = level.name;
        if (place < 0 || own < 0) {
            std::fprintf(log, "schematics store: no authored chapter for %s, left as it is\n", name);
            continue;
        }
        auto& shift = storeShifts[storeShiftCount++];
        shift.reward = selected; shift.vanilla = own; shift.shifted = place; shift.name = name;
        std::fprintf(log, "schematics store: %s chapter %d -> %d\n", name, own, place);
    }
    // test dl,dl / jne / mov eax,[esi+1Ch] / cmp eax,[edi+38h] / jg +8 / mov eax,[esi+20h] / cmp eax,[edi+3Ch] / jle +7
    const unsigned char gate[] = {0x84,0xD2,0x75,0x17,0x8B,0x46,0x1C,0x3B,0x47,0x38,0x7F,0x08,0x8B,0x46,0x20,0x3B,0x47,0x3C,0x7E,0x07};
    if (!CheckSignature(log, "store chapter gate", kStoreGateRva - 7, gate, sizeof(gate))) return false;
    storeHook = safetyhook::create_mid(reinterpret_cast<void*>(moduleBase + kStoreGateRva), OnStoreGate);
    std::fprintf(log, "schematics store gate: %s at %08X, %zu rewards moved with their schematics\n",
        storeHook ? "installed" : "FAILED", static_cast<unsigned>(moduleBase + kStoreGateRva), storeShiftCount);
    return static_cast<bool>(storeHook);
}

void WriteStoreDiagnostics(FILE* log) {
    while (storeEventsWritten < static_cast<LONG>(storeEvents.size()) &&
           InterlockedCompareExchange(&storeEvents[storeEventsWritten].ready, 1, 1) == 1) {
        const auto& e = storeEvents[storeEventsWritten++];
        const auto& shift = storeShifts[e.shift];
        std::fprintf(log, "schematics store: chapter=%d %s authored=%d now=%d %s\n", e.chapter, shift.name,
            shift.vanilla, shift.shifted, shift.shifted <= e.chapter ? "offered" : "held back");
    }
    static LONG last = -1;
    const LONG checks = InterlockedCompareExchange(&storeChecks, 0, 0);
    if (!storeHook || checks == last) return;
    last = checks;
    std::fprintf(log, "schematics store summary: checks=%ld moved=%ld mismatched=%ld events_dropped=%ld\n", checks,
        InterlockedCompareExchange(&storeShifted, 0, 0), InterlockedCompareExchange(&storeMismatch, 0, 0),
        std::max<LONG>(0, InterlockedCompareExchange(&storeEventCount, 0, 0) - static_cast<LONG>(storeEvents.size())));
}

bool CheckSignature(FILE* log, const char* name, uintptr_t rva,
    const unsigned char* expected, size_t size) {
    std::array<unsigned char, 64> actual{};
    const bool matched = size <= actual.size() && ReadMemory(moduleBase + rva, actual.data(), size) &&
        std::memcmp(actual.data(), expected, size) == 0;
    std::fprintf(log, "schematics signature: %s rva=%08X %s\n", name,
        static_cast<unsigned>(rva), matched ? "matched" : "MISMATCH");
    return matched;
}

bool Install(uintptr_t baseAddress, const char* ini, FILE* log) {
    std::lock_guard<std::mutex> guard(lock);
    if (constructorHook) return active;
    moduleBase = baseAddress;
    const bool enabled = GetPrivateProfileIntA("SchematicRandomizer", "Enabled", 0, ini) != 0;
    char explicitSeed[32]{};
    GetPrivateProfileStringA("SchematicRandomizer", "Seed", "", explicitSeed, sizeof(explicitSeed), ini);
    followsSessionSeed = !explicitSeed[0];
    seed = followsSessionSeed ? GetPrivateProfileIntA("DS2Randomizer", "Seed", 1, ini)
                              : GetPrivateProfileIntA("SchematicRandomizer", "Seed", 1, ini);
    std::fprintf(log, "schematics config: enabled=%u seed=%u follow_session_seed=%u campaignRewards=%zu completionGated=excluded hacker=excluded\n",
        enabled ? 1u : 0u, seed, followsSessionSeed ? 1u : 0u, kCount);
    if (!enabled) return true;
    const unsigned char ctor[] = {0x81,0x8E,0x10,0x03,0x00,0x00,0x00,0x00,0x01,0x00,0x8B,0xC6,0x5E,0xC2,0x04,0x00};
    const unsigned char identity[] = {0x8B,0x4B,0x0C,0x8B,0x11,0x89,0x56,0x24,0x8B,0x51,0x04,0x89,0x56,0x28,0x8B,0x51,0x08,0x89,0x56,0x2C,0x8B,0x49,0x0C,0x89,0x4E,0x30};
    const unsigned char save[] = {0x8B,0x46,0x0C,0x8B,0x90,0x38,0x01,0x00,0x00,0x89,0x57,0xFC,0x8B,0x88,0x3C,0x01,0x00,0x00,0x89,0x0F};
    const unsigned char pickup[] = {0x05,0x38,0x01,0x00,0x00,0x6A,0x00,0x50,0xE8,0x5D,0xA9,0xFF,0xFF,0x8B,0xF0,0x83,0xC4,0x08,0x85,0xF6,0x75,0x02,0x8B,0xF5};
    const unsigned char restore[] = {0x6A,0x00,0x83,0xC7,0xEC,0x57,0x8D,0x44,0x24,0x3C,0x50,0x8B,0xCD,0xE8,0xFA,0xCE,0x98,0xFF,0x68,0x00,0x20,0x00,0x00,0x8D,0x4C,0x24,0x38,0x51,0x8B,0xCD,0xE8,0x29,0xCF,0x98,0xFF};
    const unsigned char upload[] = {0x53,0x6A,0x01,0x81,0xC5,0x50,0x03,0x00,0x00,0x55,0x8B,0xCE,0x89,0x46,0x3C,0xE8,0x9E,0x95,0xCD,0xFF};
    bool verified = CheckSignature(log, "constructor", kConstructorRva, ctor, sizeof(ctor));
    verified = CheckSignature(log, "entity identity", 0x3CF28D, identity, sizeof(identity)) && verified;
    verified = CheckSignature(log, "inventory save", 0x9BFA33, save, sizeof(save)) && verified;
    verified = CheckSignature(log, "pickup shared archetype", 0x9ECBF6, pickup, sizeof(pickup)) && verified;
    verified = CheckSignature(log, "inventory restore", 0x9E9534, restore, sizeof(restore)) && verified;
    verified = CheckSignature(log, "store upload", 0xAC5A3E, upload, sizeof(upload)) && verified;
    if (!verified) return false;
    firstIsWeapon = GetPrivateProfileIntA("SchematicRandomizer", "FirstIsWeapon", 1, ini) != 0;
    secondIsSuit = GetPrivateProfileIntA("SchematicRandomizer", "SecondIsSuit", 1, ini) != 0;
    BuildPolicy(log);
    InstallStoreGate(ini, log);
    active = true;
    constructorHook = safetyhook::create_mid(reinterpret_cast<void*>(moduleBase + kConstructorRva), OnConstructed);
    if (!constructorHook) active = false;
    std::fprintf(log, "schematics hook: %s; fixed reward/presentation fields only; shared inventory identity retained\n",
        active ? "installed" : "FAILED");
    return active;
}

// A co-op client takes the host's seed. Blueprints built after this follow it;
// the ones already in the world keep what they were built with.
bool Reseed(unsigned newSeed, FILE* log) {
    std::lock_guard<std::mutex> guard(lock);
    if (!active || !policy || !followsSessionSeed || newSeed == seed) return false;
    const unsigned before = seed;
    seed = newSeed;
    BuildPolicy(log);
    if (log) std::fprintf(log, "schematics reseed: %u -> %u (the host's seed)\n", before, seed);
    return true;
}

void BuildPolicy(FILE* log) {
    policy.emplace(kCampaignSources, seed, true);
    // The story's first two blueprints: Javelin ammo in the public sector
    // courtyard and the power node in the hallway to the tram. The first
    // always turns into a weapon, the second into a suit one can afford
    // early (Security or Steampunk, not the Advanced Suit). Off with
    // FirstIsWeapon=0 / SecondIsSuit=0 in [SchematicRandomizer].
    {
        static constexpr Guid kFirstSource{{0x31CAD112u, 0xC2487E3Bu, 0x43524F57u, 0x31313048u}};
        static constexpr Guid kSecondSource{{0x31CB29D8u, 0x8859C534u, 0x43524F57u, 0x31313048u}};
        static constexpr Guid kWeapons[] = {
            {{0x36C8BC4Fu, 0x67BB5F38u, 0x44455257u, 0x34303245u}},   // Pulse Rifle
            {{0x45CB0685u, 0xDD95DCDAu, 0x43534147u, 0x4E47494Fu}},   // Seeker Rifle
            {{0x36C8BC40u, 0x0C67B69Au, 0x44455257u, 0x34303245u}},   // Flamethrower
            {{0x58CA30C7u, 0x78E54CEBu, 0x3045454Cu, 0x58373634u}},   // Detonator (Mine Gun)
            {{0x36C8BC4Fu, 0x6F9078E2u, 0x44455257u, 0x34303245u}},   // Ripper
            {{0x36C8BC48u, 0xED477CBAu, 0x44455257u, 0x34303245u}},   // Force Gun
            {{0x36C8BC3Fu, 0xF55615C8u, 0x44455257u, 0x34303245u}},   // Contact Beam
        };
        static constexpr Guid kAffordableSuits[] = {
            {{0x39CA8FDFu, 0x97CF9234u, 0x574F5242u, 0x3130314Eu}},   // Security Suit
            {{0x39CB3355u, 0x740F0370u, 0x4E415548u, 0x38343147u}},   // Steampunk Suit
        };
        uint64_t pinSeed = static_cast<uint64_t>(seed) ^ UINT64_C(0x50494E53);
        const bool pinnedFirst = firstIsWeapon && policy->Pin(kFirstSource, kWeapons, pinSeed);
        const bool pinnedSecond = secondIsSuit && policy->Pin(kSecondSource, kAffordableSuits, pinSeed);
        if (log) std::fprintf(log, "schematics openers: first_is_weapon=%d second_is_suit=%d\n", pinnedFirst ? 1 : 0, pinnedSecond ? 1 : 0);
    }
    for (size_t i = 0; log && i < kCount; ++i) {
        Guid selected{};
        policy->Select(kCampaignSources[i].entity, kCampaignSources[i].kind, kCampaignSources[i].reward, selected);
        size_t target = 0;
        while (target < kCount && kCampaignSources[target].reward != selected) ++target;
        std::fprintf(log, "schematics map: source=%zu %s -> %s shared=%08X-%08X-%08X-%08X\n",
            i, kCampaignBlueprints[i].name, target < kCount ? kCampaignBlueprints[target].name : "INVALID",
            kCampaignBlueprints[i].definition[0], kCampaignBlueprints[i].definition[1],
            kCampaignBlueprints[i].definition[2], kCampaignBlueprints[i].definition[3]);
    }
}

// The construction hook takes `lock` on the game thread, so the counters and
// details are copied out under it and written after it is released.
void WriteDiagnostics(FILE* log) {
    WriteStoreDiagnostics(log);
    std::array<unsigned long long, static_cast<size_t>(Outcome::Count)> countsCopy{};
    std::array<Detail, kDetailCapacity> detailsCopy;
    size_t detailCount = 0;
    unsigned long long callsCopy = 0, droppedCopy = 0;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (calls == lastPrinted) return;
        countsCopy = counts;
        for (; detailCount < queued; ++detailCount) detailsCopy[detailCount] = details[detailCount];
        queued = 0;
        callsCopy = calls;
        droppedCopy = dropped;
        lastPrinted = calls;
    }
    const char* names[] = {"read-failed", "wrong-class", "unknown-identity", "wrong-definition",
        "unexpected-reward", "unexpected-visual", "readonly-attributes", "readonly-actor",
        "attribute-write-failed", "actor-write-failed", "verification-failed", "applied", "already-selected"};
    std::fprintf(log, "schematics summary: calls=%llu detailOverflow=%llu", callsCopy, droppedCopy);
    for (size_t i = 0; i < countsCopy.size(); ++i) std::fprintf(log, " %s=%llu", names[i], countsCopy[i]);
    std::fputc('\n', log);
    for (size_t i = 0; i < detailCount; ++i) {
        const auto& d = detailsCopy[i];
        std::fprintf(log, "schematics detail: result=%s actor=%08X attributes=%08X thread=%u source=%zu target=%zu entity=%08X-%08X-%08X-%08X original=%08X-%08X-%08X-%08X selected=%08X-%08X-%08X-%08X rollback=%u\n",
            names[static_cast<size_t>(d.outcome)], d.actor, d.attributes, d.thread, d.source, d.target,
            d.entity[0], d.entity[1], d.entity[2], d.entity[3],
            d.original[0], d.original[1], d.original[2], d.original[3],
            d.selected[0], d.selected[1], d.selected[2], d.selected[3], d.rollbackSucceeded ? 1u : 0u);
    }
}
}  // namespace ds2_schematic_runtime
