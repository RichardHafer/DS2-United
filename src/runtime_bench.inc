// Dead Space 2 United by Hafer.
// Bench upgrade randomizer. Include at file scope; install once at startup.
//
// Retail resolves an upgrade node's type from its name hash every time it is
// needed, through one pure function. Redirecting that function moves what a
// node awards while the authored grid, the connections, the navigation keys and
// the save data stay byte for byte as shipped.
#include <windows.h>
#include <cstdio>
#include <mutex>
#include <optional>
#include "bench_policy.h"
#include "generated_bench_catalog.h"
#include "../third_party/safetyhook/safetyhook.hpp"

namespace ds2_bench_runtime {
using namespace ds2::bench;
constexpr size_t kCount = sizeof(kBenchNodes) / sizeof(kBenchNodes[0]);
constexpr size_t kTreeCount = sizeof(kTrees) / sizeof(kTrees[0]);

// Retail 0x00B76990: __cdecl, one dword name hash, returns UpgradeNodeType.
// Retail 0x007423C0: __cdecl, one string pointer, returns the name hash.
constexpr uintptr_t kTypeFromHashRva = 0x776990;
constexpr uintptr_t kNameHashRva = 0x3423C0;

using TypeFromHash = uint32_t(__cdecl*)(uint32_t);

// The callers keep live values in ECX and EDX across this call. UpgradeTypeTab
// holds its own `this` in ECX while calling here and dereferences it one
// instruction later, so a plain C hook, which uses both registers freely,
// destroys the caller. Everything below therefore runs behind a naked thunk
// that preserves them.
uint32_t __cdecl BenchLookup(uint32_t nameHash);

std::mutex lock;
uintptr_t moduleBase = 0;
bool active = false;
safetyhook::InlineHook typeHook;
std::optional<Policy<kCount>> policy;
NodeInput inputs[kCount]{};
volatile LONG calls = 0, remapped = 0, passthrough = 0;

uint32_t __cdecl BenchLookup(uint32_t nameHash) {
    InterlockedIncrement(&calls);
    if (active && policy) {
        uint8_t type = 0;
        if (policy->Lookup(nameHash, type)) {
            InterlockedIncrement(&remapped);
            return type;
        }
    }
    InterlockedIncrement(&passthrough);
    return typeHook.call<uint32_t>(nameHash);
}

// A node's amount is authored by name, not by stat: um:UpgradeValue(item,
// "Damage2", 2) lands on that cell through UpgradeTree::SetUpgradeValue
// (retail 0x00B77000, __thiscall(tree, nameHash, float), node array at
// tree+4, count at tree+0x170, value at node+0x30). Only the type moves with
// the permutation, so a filler cell that became Damage awarded "+0" and a
// Reload cell that took a Damage name awarded a Damage-sized amount to reload
// (friend's New Game+, 13.09.). The value has to follow the stat: every time
// the game sets a value, the tree is rewritten so that a cell whose stat
// moved carries the amount the game authored for that stat in this tree.
const char* TypeName(uint8_t type);
FILE* benchLog = nullptr;
constexpr uintptr_t kSetUpgradeValueRva = 0x777000;
constexpr size_t kTreeNodeCapacity = 30;
constexpr size_t kValueRecordLimit = 512;
struct ValueRecord { uintptr_t tree; uint32_t nameHash; float value; };
ValueRecord valueRecords[kValueRecordLimit]{};
size_t valueRecordCount = 0;
volatile LONG valuesRewritten = 0;
safetyhook::InlineHook valueHook;

uint8_t AuthoredType(uint32_t nameHash) {
    for (size_t i = 0; i < kCount; ++i)
        if (kBenchNodes[i].nameHash == nameHash) return inputs[i].type;
    return kNodeBlank;
}

uint8_t MappedType(uint32_t nameHash) {
    uint8_t type = AuthoredType(nameHash);
    if (policy) policy->Lookup(nameHash, type);
    return type;
}

void RewriteTreeValues(uintptr_t tree, FILE* log) {
    uint32_t count = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(tree + 0x170), &count, 4, nullptr)) return;
    if (count > kTreeNodeCapacity) return;
    uintptr_t nodes[kTreeNodeCapacity]{};
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(tree + 4), nodes, count * sizeof(uintptr_t), nullptr)) return;
    for (uint32_t n = 0; n < count; ++n) {
        if (!nodes[n]) continue;
        uint32_t key = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(nodes[n]), &key, 4, nullptr)) continue;
        const uint8_t authored = AuthoredType(key), mapped = MappedType(key);
        if (mapped == authored || mapped == kNodeBlank) continue;
        // the amount this tree authored for the stat the cell now awards; a
        // nonzero one before a zero one (RPGGun authors Damage and Capacity
        // at 0 next to real values elsewhere), and when this tree never set
        // the stat at all, the amount another tree set for it (the Line Gun's
        // Duration2 and the kinesis Range have no authored value, 15.09.)
        bool found = false;
        float value = 0.0f;
        for (int pass = 0; pass < 2 && !found; ++pass) {
            for (size_t r = 0; r < valueRecordCount; ++r) {
                if ((pass == 0 && valueRecords[r].tree != tree) || AuthoredType(valueRecords[r].nameHash) != mapped) continue;
                if (!found || (value == 0.0f && valueRecords[r].value != 0.0f)) { value = valueRecords[r].value; found = true; }
                if (value != 0.0f) break;
            }
        }
        if (!found) {
            // Diagnostic (15.09.): a RIG cell that took Air showed +0 while
            // the tree had Air values set; say which cells find no amount.
            static LONG missing = 0;
            if (log && InterlockedIncrement(&missing) <= 200)
                std::fprintf(log, "bench value: tree=%08X hash=%08X %s -> %s no amount recorded (records=%zu)\n",
                             static_cast<unsigned>(tree), key, TypeName(authored), TypeName(mapped), valueRecordCount);
            continue;
        }
        float current = 0.0f;
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(nodes[n] + 0x30), &current, 4, nullptr);
        if (current == value) continue;
        if (WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(nodes[n] + 0x30), &value, 4, nullptr)) {
            InterlockedIncrement(&valuesRewritten);
            if (log) std::fprintf(log, "bench value: tree=%08X hash=%08X %s -> %s amount %g -> %g\n",
                                  static_cast<unsigned>(tree), key, TypeName(authored), TypeName(mapped), current, value);
        }
    }
}

// The trees are usually built before this plugin's worker gets to install
// the hook above (the game runs player_upgrades.lua at start-up; 20.09.2026:
// a session without a single `bench set` line, and the Pulse Rifle's start
// cell offered "RELOAD by 25 sec": Capacity2's authored 25 rounds under its
// new stat). So on install the live trees are read once: every cell's
// authored amount (node+0x30) becomes a record, exactly what the hook would
// have recorded, and every tree is rewritten right away. A later reload of
// the upgrades (New Game+) goes through the hook as before.
constexpr uintptr_t kUpgradeManagerRva = 0x1C21438;   // [0x02021438]
constexpr uint32_t kManagerSet = 0x18, kSetTreeCount = 0xC8, kTreeNodeCount = 0x170;
void SeedValuesFromLiveTrees(FILE* log) {
    auto read = [](uintptr_t address, void* out, size_t size) {
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out, size, nullptr) != 0;
    };
    // The set is embedded in the manager at +0x18 (`add ecx,0x18` in the
    // Upgrade binding), not pointed to: read live on 20.09.2026, 18 trees,
    // the tree pointers from set+0 and the count at set+0xC8.
    uint32_t manager = 0, set = 0, treeCount = 0;
    if (!read(moduleBase + kUpgradeManagerRva, &manager, 4) || !manager) {
        std::fprintf(log, "bench values: no upgrade manager yet; amounts follow the stats as the game sets them\n");
        return;
    }
    set = manager + kManagerSet;
    if (!read(set + kSetTreeCount, &treeCount, 4) || !treeCount || treeCount > 64) {
        std::fprintf(log, "bench values: no live trees yet; amounts follow the stats as the game sets them\n");
        return;
    }
    size_t seeded = 0, trees = 0;
    for (uint32_t t = 0; t < treeCount; ++t) {
        uint32_t tree = 0, nodeCount = 0;
        if (!read(set + 4 * t, &tree, 4) || !tree || !read(tree + kTreeNodeCount, &nodeCount, 4) || nodeCount > kTreeNodeCapacity) continue;
        uintptr_t nodes[kTreeNodeCapacity]{};
        if (!read(tree + 4, nodes, nodeCount * sizeof(uintptr_t))) continue;
        ++trees;
        for (uint32_t n = 0; n < nodeCount; ++n) {
            uint32_t key = 0;
            float value = 0.0f;
            if (!nodes[n] || !read(nodes[n], &key, 4) || !read(nodes[n] + 0x30, &value, 4)) continue;
            bool known = false;
            for (size_t r = 0; r < valueRecordCount; ++r)
                if (valueRecords[r].tree == tree && valueRecords[r].nameHash == key) { known = true; break; }
            if (!known && valueRecordCount < kValueRecordLimit) { valueRecords[valueRecordCount++] = {tree, key, value}; ++seeded; }
        }
    }
    const LONG before = InterlockedCompareExchange(&valuesRewritten, 0, 0);
    for (uint32_t t = 0; t < treeCount; ++t) {
        uint32_t tree = 0;
        if (read(set + 4 * t, &tree, 4) && tree) RewriteTreeValues(tree, log);
    }
    std::fprintf(log, "bench values: %zu amounts read from %zu live trees, %ld cells rewritten to follow their stat\n",
                 seeded, trees, InterlockedCompareExchange(&valuesRewritten, 0, 0) - before);
}

void __fastcall OnSetUpgradeValue(uintptr_t tree, void*, uint32_t nameHash, float value) {
    {
        static LONG logged = 0;
        if (benchLog && InterlockedIncrement(&logged) <= 400)
            std::fprintf(benchLog, "bench set: tree=%08X hash=%08X type=%s value=%g\n",
                         static_cast<unsigned>(tree), nameHash, TypeName(AuthoredType(nameHash)), value);
        std::lock_guard<std::mutex> guard(lock);
        bool known = false;
        for (size_t r = 0; r < valueRecordCount; ++r)
            if (valueRecords[r].tree == tree && valueRecords[r].nameHash == nameHash) { valueRecords[r].value = value; known = true; break; }
        if (!known && valueRecordCount < kValueRecordLimit) valueRecords[valueRecordCount++] = {tree, nameHash, value};
    }
    valueHook.thiscall<void>(reinterpret_cast<void*>(tree), nameHash, value);
    std::lock_guard<std::mutex> guard(lock);
    RewriteTreeValues(tree, benchLog);
}

// At entry: [esp] return address, [esp+4] the name hash. After the two pushes
// the argument sits at [esp+12].
__declspec(naked) void OnTypeFromHash() {
    __asm {
        push ecx
        push edx
        push dword ptr [esp + 12]
        call BenchLookup
        add  esp, 4
        pop  edx
        pop  ecx
        ret
    }
}

bool CheckSignature(FILE* log, const char* name, uintptr_t rva,
                    const unsigned char* expected, size_t size) {
    unsigned char actual[16]{};
    SIZE_T done = 0;
    const bool matched = size <= sizeof(actual) &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(moduleBase + rva),
                          actual, size, &done) && done == size &&
        std::memcmp(actual, expected, size) == 0;
    std::fprintf(log, "bench signature: %s rva=%08X %s\n", name,
                 static_cast<unsigned>(rva), matched ? "matched" : "MISMATCH");
    return matched;
}

const char* TypeName(uint8_t type) {
    static const char* names[] = {"Blank", "Damage", "Speed", "Capacity", "Width", "Reload",
        "AltDamage", "HP", "Air", "Energy", "Duration", "Range", "Charge", "Special"};
    return type < kNodeTypeCount ? names[type] : "invalid";
}

bool Install(uintptr_t baseAddress, const char* ini, FILE* log) {
    std::lock_guard<std::mutex> guard(lock);
    if (typeHook) return active;
    moduleBase = baseAddress;

    Options options;
    options.statShuffle = GetPrivateProfileIntA("BenchRandomizer", "StatShuffle", 0, ini) != 0;
    options.nodeShuffle = GetPrivateProfileIntA("BenchRandomizer", "NodeShuffle", 0, ini) != 0;
    options.protectStartPoints =
        GetPrivateProfileIntA("BenchRandomizer", "ProtectStartPoints", 1, ini) != 0;
    const unsigned seed = GetPrivateProfileIntA("BenchRandomizer", "Seed",
        GetPrivateProfileIntA("DS2Randomizer", "Seed", 1, ini), ini);
    std::fprintf(log, "bench config: statShuffle=%u nodeShuffle=%u protectStartPoints=%u seed=%u nodes=%zu trees=%zu\n",
                 options.statShuffle ? 1u : 0u, options.nodeShuffle ? 1u : 0u,
                 options.protectStartPoints ? 1u : 0u, seed, kCount, kTreeCount);
    if (!options.statShuffle && !options.nodeShuffle) return true;

    const unsigned char typeBytes[] = {0x8B,0x44,0x24,0x04,0x3D,0x34,0xCB,0x15,0x7A,0x0F,0x87,0xCB,0x00,0x00,0x00,0x0F};
    const unsigned char hashBytes[] = {0x8B,0x54,0x24,0x04,0x33,0xC0,0x85,0xD2,0x74,0x25,0x8A,0x0A,0x84,0xC9,0x74,0x1F};
    const unsigned char ctorBytes[] = {0x56,0x57,0x8B,0x7C,0x24,0x0C,0x57,0x8B,0xF1,0xE8,0x92,0xBA,0xBC,0xFF,0x8B,0x4C};
    const unsigned char addBytes[]  = {0x53,0x55,0x56,0x57,0x8B,0x7C,0x24,0x14,0x57,0x8B,0xF1,0xE8,0xE0,0xB4,0xBC,0xFF};
    bool verified = CheckSignature(log, "type from hash", kTypeFromHashRva, typeBytes, sizeof(typeBytes));
    verified = CheckSignature(log, "name hash", kNameHashRva, hashBytes, sizeof(hashBytes)) && verified;
    verified = CheckSignature(log, "upgrade constructor", 0x776920, ctorBytes, sizeof(ctorBytes)) && verified;
    verified = CheckSignature(log, "tree add upgrade", 0x776ED0, addBytes, sizeof(addBytes)) && verified;
    if (!verified) return false;

    // Ask the game for the authored type of every catalogued name, before the
    // hook exists. A wrong offline guess can therefore never become a mapping.
    const auto original = reinterpret_cast<TypeFromHash>(moduleBase + kTypeFromHashRva);
    unsigned unknown = 0;
    for (size_t i = 0; i < kCount; ++i) {
        const uint32_t type = original(kBenchNodes[i].nameHash);
        if (type >= kNodeTypeCount) {
            std::fprintf(log, "bench abort: hash %08X reported type %u, outside 0..%u\n",
                         kBenchNodes[i].nameHash, type, kNodeTypeCount - 1);
            return false;
        }
        inputs[i] = makeInput(kBenchNodes[i], static_cast<uint8_t>(type));
        if (!type) ++unknown;
    }
    std::fprintf(log, "bench types: read %zu nodes from the game, %u blank\n", kCount, unknown);

    policy.emplace();
    if (policy->Build(inputs, seed, options) != BuildResult::Built) {
        std::fprintf(log, "bench abort: permutation not built\n");
        policy.reset();
        return false;
    }

    // The guarantee, measured on the game's own types rather than assumed: a
    // tree must award exactly the stats it awarded before, each as often as
    // before. Width belongs to the Line Gun, so no other tree may offer it and
    // the Line Gun may not lose it; and no tree gains a fifth Damage cell.
    uint8_t authoredStats[kTreeCount][kNodeTypeCount]{}, mappedStats[kTreeCount][kNodeTypeCount]{};
    for (size_t i = 0; i < kCount; ++i) {
        const uint16_t tree = kBenchNodes[i].tree;
        if (tree >= kTreeCount) continue;
        uint8_t mapped = inputs[i].type;
        policy->Lookup(kBenchNodes[i].nameHash, mapped);
        ++authoredStats[tree][inputs[i].type];
        ++mappedStats[tree][mapped];
    }
    for (size_t tree = 0; tree < kTreeCount; ++tree) {
        for (uint8_t type = 0; type < kNodeTypeCount; ++type) {
            if (authoredStats[tree][type] == mappedStats[tree][type]) continue;
            std::fprintf(log, "bench abort: %s would award %s %u times instead of %u\n",
                         kTrees[tree].name, TypeName(type), mappedStats[tree][type], authoredStats[tree][type]);
            policy.reset();
            return false;
        }
    }
    std::fprintf(log, "bench trees: %zu checked, each awards the stats it awarded before, as often as before\n",
                 kTreeCount);

    for (size_t i = 0; i < kCount; ++i) {
        uint8_t after = 0;
        if (!policy->Lookup(kBenchNodes[i].nameHash, after) || after == inputs[i].type) continue;
        std::fprintf(log, "bench map: tree=%s hash=%08X %s -> %s%s\n",
                     kBenchNodes[i].tree < kTreeCount ? kTrees[kBenchNodes[i].tree].name : "?",
                     kBenchNodes[i].nameHash, TypeName(inputs[i].type), TypeName(after),
                     kBenchNodes[i].startPoint ? " startPoint" : "");
    }

    const unsigned char valueBytes[] = {0x8B,0x81,0x70,0x01,0x00,0x00,0x56,0x33,0xF6,0x85,0xC0,0x76,0x1F,0x57,0x8B,0x7C};
    if (!CheckSignature(log, "tree set upgrade value", kSetUpgradeValueRva, valueBytes, sizeof(valueBytes))) {
        policy.reset();
        return false;
    }
    benchLog = log;
    valueHook = safetyhook::create_inline(reinterpret_cast<void*>(moduleBase + kSetUpgradeValueRva),
                                          reinterpret_cast<void*>(OnSetUpgradeValue));
    if (!valueHook) {
        std::fprintf(log, "bench hook: value hook FAILED; types stay retail\n");
        policy.reset();
        return false;
    }
    active = true;
    typeHook = safetyhook::create_inline(reinterpret_cast<void*>(moduleBase + kTypeFromHashRva),
                                         reinterpret_cast<void*>(OnTypeFromHash));
    if (!typeHook) {
        active = false;
        policy.reset();
        valueHook = {};
    }
    std::fprintf(log, "bench hook: %s; node types and their amounts, grid and connections untouched\n",
                 active ? "installed" : "FAILED");
    if (active) SeedValuesFromLiveTrees(log);
    return active;
}

// Counters only, all interlocked: no `lock` here, which the value hook takes
// on the game thread.
void WriteDiagnostics(FILE* log) {
    static LONG lastPrinted = -1;
    const LONG total = InterlockedCompareExchange(&calls, 0, 0);
    if (!active || total == lastPrinted) return;
    lastPrinted = total;
    std::fprintf(log, "bench summary: calls=%ld remapped=%ld passthrough=%ld\n", total,
                 InterlockedCompareExchange(&remapped, 0, 0),
                 InterlockedCompareExchange(&passthrough, 0, 0));
}
}  // namespace ds2_bench_runtime
