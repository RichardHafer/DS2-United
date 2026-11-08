// Dead Space 2 United by Hafer.
// How many corpses, body parts and trash pieces stay in the world ([Lifetime]). The game keeps
// them in EnemyLifetimeManager ([0x02021350], vtable 0x01B6A840): Bucket[5] at +0x14, stride
// 0x18, the limit at +0x10 of each (eLifetime 0 CORPSE, 1 BODYPART, 2 TRASH, 3 and 4 the MP
// items). ResetPopLimit 0x01003BF0 (thiscall(bucket, size), ret 8) fades the oldest above the
// limit and sets it; the game asks 5, 10 and 20 (0x01012B37). MarkerPatch's DINPUT8.dll detours
// that entry and swaps in its own IncreasedEntityPersistence values (25 and 96 here, 17.10.: 25
// bodies in a survival break, 55 fps against 143 without them). Our calls go to the original
// past any detour: the entry's 7 bytes rebuilt in a stub that jumps to 0x01003BF7. Whenever a
// limit differs from the wanted one (a level start, MarkerPatch's next call) it is set again.
namespace lifetime {
constexpr uint32_t kManagerPtr = 0x1C21350, kManagerVtableRva = 0x176A840, kResetRva = 0xC03BF0, kBodyRva = 0xC03BF7;
constexpr uint32_t kBuckets = 0x14, kBucketStride = 0x18, kPopLimit = 0x10, kQueueCount = 0x0C;
constexpr unsigned kTuned = 3;   // corpse, body part, trash; the MP item buckets stay the game's
// mov eax,[esp+4] / sub esp,14h, then push ebp / push esi / lea eax,[eax+eax*2] / lea ebp,[ecx+eax*8] / push edi / lea esi,[ebp+10h]
const unsigned char kPrologue[] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x14};
const unsigned char kBody[] = {0x55, 0x56, 0x8D, 0x04, 0x40, 0x8D, 0x2C, 0xC1, 0x57, 0x8D, 0x75, 0x10};
const char* const kNames[kTuned] = {"corpses", "body parts", "trash"};
bool enabled = false, ready = false, detoured = false;
int story[kTuned]{}, modes[kTuned]{10, 32, 0};   // 0 leaves the bucket alone
using ResetFn = void(__thiscall*)(void*, int, int);
ResetFn reset = nullptr;
DWORD checkedAt = 0;
int lastSet[kTuned] = {-1, -1, -1};
volatile LONG sets = 0, faults = 0, lines = 0, reaped = 0;

void Line(const char* format, ...) {
    if (InterlockedIncrement(&lines) > 60) return;
    char text[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    gate::Event("lifetime %s", text);
}
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Lifetime", "Enabled", 1, ini.c_str()) != 0;
    const char* const storyKeys[kTuned] = {"StoryCorpses", "StoryBodyParts", "StoryTrash"};
    const char* const modeKeys[kTuned] = {"ModeCorpses", "ModeBodyParts", "ModeTrash"};
    for (unsigned b = 0; b < kTuned; ++b) {
        story[b] = std::clamp(static_cast<int>(GetPrivateProfileIntA("Lifetime", storyKeys[b], story[b], ini.c_str())), 0, 200);
        modes[b] = std::clamp(static_cast<int>(GetPrivateProfileIntA("Lifetime", modeKeys[b], modes[b], ini.c_str())), 0, 200);
    }
    if (!enabled) { std::fprintf(log, "lifetime off ([Lifetime] Enabled=0)\n"); return; }
    unsigned char entry[sizeof(kPrologue)]{}, body[sizeof(kBody)]{};
    const bool bodyOk = Read(base + kBodyRva, body, sizeof(body)) && std::memcmp(body, kBody, sizeof(body)) == 0;
    const bool entryOk = Read(base + kResetRva, entry, sizeof(entry)) && std::memcmp(entry, kPrologue, sizeof(entry)) == 0;
    if (bodyOk && entryOk) {
        reset = reinterpret_cast<ResetFn>(base + kResetRva);
    } else if (bodyOk) {
        // somebody's detour sits on the entry: our own copy of the 7 bytes, then the rest of the original
        auto* stub = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (stub) {
            std::memcpy(stub, kPrologue, sizeof(kPrologue));
            stub[sizeof(kPrologue)] = 0xE9;
            const int32_t rel = static_cast<int32_t>((base + kBodyRva) - (reinterpret_cast<uintptr_t>(stub) + sizeof(kPrologue) + 5));
            std::memcpy(stub + sizeof(kPrologue) + 1, &rel, 4);
            DWORD old = 0;
            VirtualProtect(stub, 64, PAGE_EXECUTE_READ, &old);
            FlushInstructionCache(GetCurrentProcess(), stub, 64);
            reset = reinterpret_cast<ResetFn>(stub);
            detoured = true;
        }
    }
    ready = reset != nullptr;
    std::fprintf(log, "lifetime %s: ResetPopLimit %08X entry %s, body %s; story corpses=%d body_parts=%d trash=%d, game modes corpses=%d body_parts=%d trash=%d (0 = left alone)\n",
                 ready ? "ready" : "OFF", static_cast<unsigned>(base + kResetRva), entryOk ? "original" : "detoured (MarkerPatch?), called past it",
                 bodyOk ? "as expected" : "DIFFERENT", story[0], story[1], story[2], modes[0], modes[1], modes[2]);
}
__declspec(noinline) bool CallReset(uint32_t manager, int bucket, int size) {
    __try {
        reset(reinterpret_cast<void*>(static_cast<uintptr_t>(manager)), bucket, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Game thread, from Poll.
void Tick(DWORD now) {
    if (!enabled || !ready || now - checkedAt < 1000) return;
    checkedAt = now;
    if (mpmap::LmState(nullptr) != coop_mpmap::kRunning) return;
    uint32_t manager = 0, vtable = 0;
    if (!Read(base + kManagerPtr, &manager, 4) || !manager || !Read(manager, &vtable, 4) || vtable != base + kManagerVtableRva) return;
    const bool gameMode = mpmap::InVisit() || arena::RunningNow();
    const int* wanted = gameMode ? modes : story;
    for (unsigned b = 0; b < kTuned; ++b) {
        if (wanted[b] <= 0) continue;
        const uint32_t bucket = manager + kBuckets + kBucketStride * b;
        int32_t limit = 0, count = 0;
        if (!Read(bucket + kPopLimit, &limit, 4) || !Read(bucket + kQueueCount, &count, 4) || limit == wanted[b]) continue;
        const bool ok = CallReset(manager, static_cast<int>(b), wanted[b]);
        int32_t after = -1, left = -1;
        Read(bucket + kPopLimit, &after, 4);
        Read(bucket + kQueueCount, &left, 4);
        InterlockedIncrement(ok ? &sets : &faults);
        if (ok && count > left && left >= 0) InterlockedExchangeAdd(&reaped, count - left);
        if (!ok) { ready = false; Line("ResetPopLimit FAULTED on the %s, the limits are left alone from now on", kNames[b]); return; }
        if (lastSet[b] != wanted[b] || after != wanted[b])
            Line("%s: limit %d -> %d (%s, %d in the world before, %d after)%s", kNames[b], limit, after, gameMode ? "game mode" : "story", count, left,
                 after == wanted[b] ? "" : " NOT TAKEN");
        lastSet[b] = wanted[b];
    }
}
void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG v = sets + faults * 7 + reaped * 13;
    if (v == last) return;
    last = v;
    std::fprintf(log, "lifetime sets=%ld faults=%ld reaped=%ld ready=%d detoured=%d\n", sets, faults, reaped, ready ? 1 : 0, detoured ? 1 : 0);
}
}  // namespace lifetime
