// Placement readout: where the player stands, which way they look, and the
// spawners around them, on one key.
//
// The DS1 patch grew a designer's workflow out of this: stand where a
// Guardian should sit, press the key, copy the numbers into a rule. The DS2
// player object is found through its constructor (0x008A6CA0, the target of
// the "Player" class factory 0x0041A530; vtable 0x01B29768), and two dumps of
// 15.09. agree on the fields: the feet position at +0x380 (x, y, z, 1) and
// the facing at +0x300 (a unit vector, +x/+z), both live while the player is
// standing still (dump 33076 shows +0x380, +0x3B0 and +0x3E0 all equal).
// Spawners keep their own transform like every entity: rotation rows at
// +0x60/+0x70/+0x80, position at +0xB0 (read in dump 17912 on six live
// FuncSpawn objects), identity GUID at +0x24. The yaw printed here is the
// angle the extra-spawn rule takes: forward = (sin yaw, 0, cos yaw).
namespace placement {
safetyhook::MidHook playerHook;
bool installed = false;
int hotkey = VK_F10;
volatile LONG playerObject = 0;
volatile LONG readouts = 0;
uint32_t playerVtable = 0;
// The Player is an entity with its transform 0x30 further in than a
// FuncSpawn's: rotation rows at +0x90/+0xA0/+0xB0, position at +0xC0. Both
// dumps of 15.09. (33076 and 17912, objects with the primary vtable 01B294D0)
// show a unit forward row there that agrees with the first row, and +0xC0
// within two metres of the feet capsule at +0x740. The first readout used
// +0x6C0 for the facing, which is a constant (0, 0, 1) and printed yaw 0.
constexpr uint32_t kPlayerPrimaryVtableRva = 0x17294D0;
constexpr uint32_t kPlayerPosition = 0xC0, kPlayerForward = 0xB0;
constexpr uint32_t kEntityPosition = 0xB0, kEntityForwardRow = 0x80, kEntityGuid = 0x24;
char pending[8][640]{};
volatile LONG pendingReady[8]{};
volatile LONG pendingCount = 0;
bool keyWasDown = false;

// Every Player the game constructs, in order. The campaign builds exactly one;
// a second entry means something else went through the same constructor.
volatile LONG playerConstructions = 0;
volatile LONG playerObjects[4]{};
// Capture the spawn call chain the first few times a Player is constructed, so
// SimManager::SpawnEntity/CreateEntity and the SpawnInfo that drives
// Player::Init can be found from the log. Read-only stack walk on the existing
// ctor hook, no new hook, no crash risk. Queued to the log, analysed offline.
volatile LONG spawnChainCaptured = 0;
void Queue(const char* text);   // defined below
void CaptureSpawnChain(SafetyHookContext& ctx) {
    const LONG n = InterlockedIncrement(&spawnChainCaptured);
    if (n > 2) return;
    char line[640];
    int used = std::snprintf(line, sizeof(line), "spawn_chain #%ld ctor_ecx=%08X esp=%08X callers=",
                             n, static_cast<unsigned>(ctx.ecx), static_cast<unsigned>(ctx.esp));
    // walk the stack for return addresses (a code dword preceded by a call op)
    for (uint32_t off = 0; off < 0x240 && used < static_cast<int>(sizeof(line)) - 12; off += 4) {
        uint32_t v = 0;
        if (!Read(ctx.esp + off, &v, 4)) break;
        if (v <= base + 0x1000 || v >= base + 0x1AA1000) continue;
        unsigned char pre5 = 0, pre6 = 0;
        Read(v - 5, &pre5, 1);   // E8 rel32 call
        Read(v - 6, &pre6, 1);   // FF /2 call (opcode two bytes before modrm)
        if (pre5 == 0xE8 || pre6 == 0xFF)
            used += std::snprintf(line + used, sizeof(line) - used, "%08X ", v);
    }
    Queue(line);
    // Walk the true ebp frame chain: each frame gives its return address and
    // its first args. The frame whose arg is a SpawnInfo (arg -> +4 transform,
    // +0x20 small flags) is SimManager::CreateEntity/SpawnEntity -- exactly what
    // we call to spawn a second Isaac.
    char line2[640];
    int u2 = std::snprintf(line2, sizeof(line2), "spawn_chain #%ld frames=", n);
    uint32_t fp = static_cast<uint32_t>(ctx.ebp);
    for (int depth = 0; depth < 12 && u2 < static_cast<int>(sizeof(line2)) - 64; ++depth) {
        uint32_t nextfp = 0, ret = 0, a1 = 0, a2 = 0, a3 = 0;
        if (!Read(fp, &nextfp, 4) || !Read(fp + 4, &ret, 4)) break;
        Read(fp + 8, &a1, 4); Read(fp + 0xC, &a2, 4); Read(fp + 0x10, &a3, 4);
        // is any arg a SpawnInfo? mark it
        auto isSpawnInfo = [&](uint32_t p) {
            if (p < 0x02000000 || p >= 0x60000000) return false;
            uint32_t tform = 0, flags = 0;
            return Read(p + 0x04, &tform, 4) && tform >= 0x02000000 && tform < 0x60000000 &&
                   Read(p + 0x20, &flags, 4) && flags < 0x10000;
        };
        char si = (isSpawnInfo(a1) ? '1' : (isSpawnInfo(a2) ? '2' : (isSpawnInfo(a3) ? '3' : '-')));
        u2 += std::snprintf(line2 + u2, sizeof(line2) - u2, "[ret=%08X a1=%08X a2=%08X si=%c] ", ret, a1, a2, si);
        if (nextfp <= fp || nextfp - fp > 0x4000) break;
        fp = nextfp;
    }
    Queue(line2);
}

void OnPlayerConstructed(SafetyHookContext& ctx) {
    // Ring buffer: playerConstructions only ever increments (checkpoint reloads keep
    // building new players), so a plain index overflows the array after a few
    // reloads and stops recording the CURRENT players. Modulo keeps the most recent.
    const LONG count = InterlockedIncrement(&playerConstructions) - 1;
    const LONG index = count % static_cast<LONG>(std::size(playerObjects));
    if (index >= 0) InterlockedExchange(&playerObjects[index], static_cast<LONG>(ctx.ecx));
    // the newest one drives the readout (a checkpoint restart builds a new local player)
    InterlockedExchange(&playerObject, static_cast<LONG>(ctx.ecx));
    CaptureSpawnChain(ctx);
}

bool PlayerTransform(float* position, float* forward) {
    const uint32_t player = static_cast<uint32_t>(InterlockedCompareExchange(&playerObject, 0, 0));
    uint32_t vtable = 0;
    // The live object's vtable is not the base Player's (the readout of
    // 16:45 saw the constructor fire and then refused the object), so any
    // vtable inside the module's read-only data passes; the value is printed.
    if (!player || !Read(player, &vtable, 4) || vtable < base + 0x16A1000 || vtable > base + 0x1880000) return false;
    playerVtable = vtable;
    return Read(player + kPlayerPosition, position, 12) && Read(player + kPlayerForward, forward, 12);
}

float YawDegrees(const float* forward) {
    return static_cast<float>(std::atan2(static_cast<double>(forward[0]), static_cast<double>(forward[2])) * 180.0 / 3.14159265358979);
}

void Queue(const char* text) {
    const LONG slot = InterlockedIncrement(&pendingCount) - 1;
    if (slot < 0 || slot >= static_cast<LONG>(std::size(pending))) return;
    std::snprintf(pending[slot], sizeof(pending[slot]), "%s", text);
    InterlockedExchange(&pendingReady[slot], 1);
}

// Game thread, once per tick from the more-enemies drain.
void Poll() {
    if (!installed || !hotkey) return;
    const bool down = (GetAsyncKeyState(hotkey) & 0x8000) != 0;
    const bool pressed = down && !keyWasDown;
    keyWasDown = down;
    if (!pressed) return;
    float position[3]{}, forward[3]{};
    char text[640]{};
    if (!PlayerTransform(position, forward)) {
        std::snprintf(text, sizeof(text), "placement: no player yet (object %08X)",
                      static_cast<unsigned>(InterlockedCompareExchange(&playerObject, 0, 0)));
        Queue(text);
        return;
    }
    const float yaw = YawDegrees(forward);
    int used = std::snprintf(text, sizeof(text), "placement: x=%.2f y=%.2f z=%.2f yaw=%.0f forward=(%.2f %.2f %.2f) vtable=%08X  (rule fields: %.2f, %.2f, %.2f, %.0f)",
                             position[0], position[1], position[2], yaw, forward[0], forward[1], forward[2], playerVtable, position[0], position[1], position[2], yaw);
    // the spawners this session has seen, nearest first
    struct Near { double distance; Guid guid; float yaw; };
    Near best[6]{};
    unsigned found = 0;
    {
        const std::lock_guard<std::mutex> guard(spawnerToolsMutex);
        uint32_t seen[64]{};
        unsigned seenCount = 0;
        for (const auto& entry : recentSpawners) {
            if (!entry.pointer) continue;
            bool dup = false;
            for (unsigned i = 0; i < seenCount; ++i) if (seen[i] == entry.pointer) dup = true;
            if (dup || seenCount >= std::size(seen)) continue;
            seen[seenCount++] = entry.pointer;
            uint32_t vtable = 0;
            Guid guid{};
            float where[3]{}, row[3]{};
            if (!Read(entry.pointer, &vtable, 4) || vtable != base + 0x1759C58 ||
                !Read(entry.pointer + kEntityGuid, guid.data(), 16) || guid != entry.source ||
                !Read(entry.pointer + kEntityPosition, where, 12) || !Read(entry.pointer + kEntityForwardRow, row, 12)) continue;
            const double dx = where[0] - position[0], dy = where[1] - position[1], dz = where[2] - position[2];
            const Near candidate{std::sqrt(dx * dx + dy * dy + dz * dz), guid, YawDegrees(row)};
            unsigned slot = found < std::size(best) ? found : static_cast<unsigned>(std::size(best));
            for (unsigned probe = 0; probe < found && probe < std::size(best); ++probe)
                if (candidate.distance < best[probe].distance) { slot = probe; break; }
            if (slot >= std::size(best)) continue;
            for (unsigned move = found < std::size(best) ? found : static_cast<unsigned>(std::size(best) - 1); move > slot; --move) best[move] = best[move - 1];
            best[slot] = candidate;
            if (found < std::size(best)) ++found;
        }
    }
    for (unsigned i = 0; i < found && used < static_cast<int>(sizeof(text)) - 80; ++i)
        used += std::snprintf(text + used, sizeof(text) - used, "\n placement spawner %08X-%08X-%08X-%08X distance=%.1f yaw=%.0f",
                              best[i].guid[0], best[i].guid[1], best[i].guid[2], best[i].guid[3], best[i].distance, best[i].yaw);
    if (!found) used += std::snprintf(text + used, sizeof(text) - used, "\n placement spawner none seen yet in this session");
    if (used < static_cast<int>(sizeof(text)) - 120)
        std::snprintf(text + used, sizeof(text) - used, "\n placement area row (fires when you come within 20 m, no spawner needed): near:20\tGuardian\t1\t100\t%.2f\t%.2f\t%.2f\t%.0f\tdirect",
                      position[0], position[1], position[2], yaw);
    Queue(text);
    InterlockedIncrement(&readouts);
}

bool Install(FILE* log, const std::string& ini) {
    hotkey = static_cast<int>(GetPrivateProfileIntA("DS2Randomizer", "PlacementHotkey", VK_F10, ini.c_str()));
    if (hotkey < 0 || hotkey > 255) hotkey = VK_F10;
    // mov eax,[esp+4] / push ebx / push ebp / push esi / push edi / push -1 / push 8 / push eax / mov esi,ecx
    static const unsigned char kCtor[] = {0x8B,0x44,0x24,0x04,0x53,0x55,0x56,0x57,0x6A,0xFF,0x6A,0x08,0x50,0x8B,0xF1,0xE8};
    unsigned char actual[sizeof(kCtor)]{};
    if (!Read(base + 0x4A6CA0, actual, sizeof(actual)) || std::memcmp(actual, kCtor, sizeof(kCtor)) != 0) {
        std::fprintf(log, "placement signature_mismatch at %08X; readout off\n", static_cast<unsigned>(base + 0x4A6CA0));
        return false;
    }
    playerHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x4A6CA0), OnPlayerConstructed);
    if (!playerHook) {
        std::fprintf(log, "placement hook_failed at %08X\n", static_cast<unsigned>(base + 0x4A6CA0));
        return false;
    }
    installed = true;
    std::fprintf(log, "placement installed; hotkey=%d prints position, yaw and the nearest spawners\n", hotkey);
    return true;
}

void WriteDiagnostics(FILE* log) {
    static LONG flushed = 0;
    while (flushed < static_cast<LONG>(std::size(pending)) && InterlockedCompareExchange(&pendingReady[flushed], 1, 1) == 1) {
        std::fprintf(log, "%s\n", pending[flushed]);
        std::fflush(log);
        ++flushed;
    }
    // the ring is small: once full, start over so the key keeps working
    if (flushed >= static_cast<LONG>(std::size(pending))) {
        for (auto& ready : pendingReady) InterlockedExchange(&ready, 0);
        InterlockedExchange(&pendingCount, 0);
        flushed = 0;
    }
}
}  // namespace placement
