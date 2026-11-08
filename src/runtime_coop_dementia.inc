// Dead Space 2 United by Richard Hafer.
// The Dementia mutator ([Dementia]): a sanity meter for this machine's Isaac. Fighting alone
// (no partner or bot within AloneMeters) fills it, staying together drains it, and the stage
// it reaches brings hallucinations: whispers and a faint vignette first, then flicker, glitch
// bands, Nicole's lines and a lying RIG, then heavy flicker, the partner "lost" and a
// heartbeat, at last a freakout. Nothing is sent: a partner never sees or hears another
// player's hallucinations. The sounds are WAV files in plugins\dementia (made by
// tools\export_dementia_audio.py), picked by their name: whisper_*, nicole_*, amb_*;
// a <name>.txt next to a file holds its subtitle.
namespace dementia {
bool enabled = false, installed = false, campaignToo = false, botsCount = true, flashes = true;
float aloneM = 15.0f, pace = 1.0f;
coop_dementia::Rates rates;
int forceStage = -1;
using coop_dementia::kStageAt;
constexpr DWORD kTickMs = 200;

// --- the sounds on disk ---
struct File { std::string path, subtitle; };
std::string folder;
std::vector<File> whispers, nicole, ambience;
// The subtitle file: one "ms|text" line per part (tools\build_dementia_pack.py), or plain text.
std::string ReadSubtitle(const std::string& wav) {
    std::string txt = wav.substr(0, wav.size() - 4) + ".txt";
    FILE* f = nullptr;
    char buffer[1024]{};
    if (fopen_s(&f, txt.c_str(), "rb") != 0 || !f) return "";
    const size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, f);
    std::fclose(f);
    std::string s(buffer, got);
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}
void ScanFiles(FILE* log) {
    WIN32_FIND_DATAA fd{};
    const HANDLE h = FindFirstFileA((folder + "\\*.wav").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { std::fprintf(log, "dementia: no sounds in %s (tools\\export_dementia_audio.py makes them)\n", folder.c_str()); return; }
    do {
        std::string name = fd.cFileName;
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        File f{folder + "\\" + fd.cFileName, ""};
        f.subtitle = ReadSubtitle(f.path);
        if (name.rfind("whisper", 0) == 0) whispers.push_back(f);
        else if (name.rfind("nicole", 0) == 0) nicole.push_back(f);
        else if (name.rfind("amb", 0) == 0) ambience.push_back(f);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::fprintf(log, "dementia: sounds in %s: whispers=%zu nicole=%zu ambience=%zu\n", folder.c_str(), whispers.size(), nicole.size(), ambience.size());
}

}  // namespace dementia

// The registry every module asks (declared at the top of runtime_coop.inc).
namespace phantom {
volatile LONG spawning = 0, allowedOnClient = 0, keptOnHost = 0, worldHitsKept = 0, hitsZeroed = 0, dropsRefused = 0, messagesDropped = 0, botsIgnored = 0,
              restSkipped = 0;
constexpr unsigned kMax = 4;
volatile LONG entities[kMax]{};
bool Is(uint32_t entity) {
    if (!entity) return false;
    for (const volatile LONG& e : entities)
        if (static_cast<uint32_t>(e) == entity) return true;
    return false;
}
bool Any() {
    for (const volatile LONG& e : entities)
        if (e) return true;
    return false;
}
}  // namespace phantom

namespace dementia {
// --- the meter ---
float meter = 0.0f, lastHealth = -1.0f;
int stage = 0;
bool activeNow = false, wasPaused = false;
DWORD lastTick = 0, aloneSince = 0, nextEventAt = 0;
float nearestM = -1.0f;
unsigned companyNow = 0;
const char* pauseWhy = "";
volatile LONG ticks = 0, pausedScene = 0, pausedDead = 0, pausedOff = 0, pausedSolo = 0, medDrops = 0, stageUps = 0, stageDowns = 0, eventLines = 0;
unsigned rng = 0x5EED1234u;
float Roll() { rng = rng * 1664525u + 1013904223u; return static_cast<float>(rng >> 8) / 16777216.0f; }

bool PositionOf(uint32_t entity, float* out) {
    return entity && Read(entity + placement::kPlayerPosition, out, 12) && std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}
// Human partners in the game. Solo, with bots or without, is never a Dementia game (user, 19.10.).
unsigned HumanPartners() {
    unsigned n = 0;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        if (peer.used && peer.token && peer.machine != 0xFFFFFFFFu) ++n;
    }
    return n;
}
// The nearest partner or bot, in metres; -1 when there is none at all.
float NearestCompany(const float* me, unsigned& count) {
    float best = -1.0f;
    count = 0;
    auto consider = [&](uint32_t e) {
        float p[3];
        if (!PositionOf(e, p)) return;
        ++count;
        const float dx = p[0] - me[0], dy = p[1] - me[1], dz = p[2] - me[2], d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (best < 0.0f || d < best) best = d;
    };
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        if (peer.used && peer.token && peer.machine != 0xFFFFFFFFu) consider(GhostForMachine(peer.machine));
    }
    if (botsCount)
        for (unsigned i = 0; i < kMaxBots; ++i) consider(botEntities[i] ? botEntities[i] : GhostForMachine(0xF0u + i));
    return best;
}
bool HealthOf(uint32_t player, float& fraction, float& current, float& max) {
    const uint32_t hc = hits::FindHealthComponent(player);
    current = max = 0.0f;
    if (!hc || !Read(hc + hits::kHealthCurrent, &current, 4) || !Read(hc + hits::kHealthMax, &max, 4) || !(max > 0.0f) || !std::isfinite(current)) return false;
    fraction = current / max;
    return true;
}

// --- the hallucinations ---
enum Kind : int { kWhisper, kFlickerMild, kGlitch, kFlicker, kNicoleLine, kRigWave, kRigVitals, kFlickerHeavy, kRigPartnerLost, kAmbience, kFreakout,
                  kScreen, kNativeFlicker, kPackChild, kKinds };
struct KindRule { const char* name; int minStage; DWORD cooldownMs; float weight; };
constexpr KindRule kRules[kKinds] = {
    {"whisper", 1, 15000, 3.0f},         {"flicker_mild", 1, 20000, 2.0f}, {"glitch", 2, 25000, 1.5f},
    {"flicker", 2, 25000, 1.5f},         {"nicole_line", 2, 60000, 1.5f},  {"rig_wave", 2, 90000, 0.7f},
    {"rig_vitals", 2, 90000, 0.7f},      {"flicker_heavy", 3, 30000, 1.2f}, {"rig_partner_lost", 3, 120000, 0.8f},
    {"ambience", 3, 70000, 0.6f},        {"freakout", 4, 40000, 1.0f},      {"dementia_screen", 2, 30000, 1.4f},
    {"dementia_flicker", 3, 35000, 1.0f}, {"pack_child", 2, 22000, 2.5f}};
DWORD lastOf[kKinds]{};
// The pack children grow with the stage (19.10. test: two in a whole run were too few).
float WeightOf(int kind, int s) { return kRules[kind].weight * (kind == kPackChild && s > 2 ? static_cast<float>(s - 1) : 1.0f); }
volatile LONG fired[kKinds]{}, refused[kKinds]{};
void Line(const char* fmt, ...);   // below

// --- the campaign's own dementia effects (analysis/DEMENTIA_SCREEN_EFFECTS_2026-10-19.md) ---
// dementia_screen (the LUT, a blur pulse, the distortion, glitch) and dementia_flicker_on are
// resident on every survival map, for the necro win. Created like the turret's effects
// (Create_ByGuid, no parent) at our Isaac; their camera bit (header 0x4000) keeps them in front
// of the camera. The instance points back at its handle (+8), so the handles live here for good.
struct Native {
    const char* name;
    uint32_t guid[4];
    uint32_t slot = 0;
    DWORD createdAt = 0, killAt = 0, endedAt = 0;
    bool live = false;
    LONG created = 0, absent = 0, refused = 0, ended = 0, killed = 0;
    DWORD lastLifeMs = 0;
};
Native natives[2] = {{"dementia_screen", {0x793C565Du, 0x793C565Du, 0x793C565Du, 0xF2E421ADu}},
                     {"dementia_flicker_on", {0x75296F97u, 0x75296F97u, 0x75296F97u, 0x799A0B39u}}};
enum : int { kNativeScreen = 0, kNativeFlickerOn = 1 };
bool nativeOk = false;
bool Live(Native& n) {
    uint32_t back = 0;
    return n.slot && Read(n.slot + 8, &back, 4) && back == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&n.slot));
}
// holdMs 0: until killed (or until it ends by itself)
bool StartNative(int i, uint32_t player, DWORD now, DWORD holdMs) {
    Native& n = natives[i];
    if (!nativeOk || !turret::VfxRunning()) { ++n.refused; return false; }
    if (Live(n)) { n.killAt = holdMs ? Stamp(now + holdMs) : 0; return true; }
    if (turret::Resident(n.guid) != 1) { ++n.absent; return false; }
    alignas(16) float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Read(player + placement::kPlayerPosition, &m[12], 12);
    n.slot = 0;
    if (!turret::CreateRaw(&n.slot, n.guid, m) || !n.slot) { ++n.refused; n.slot = 0; return false; }
    ++n.created;
    n.live = true;
    n.createdAt = Stamp(now);
    n.killAt = holdMs ? Stamp(now + holdMs) : 0;
    return true;
}
void KillNative(int i, const char* why) {
    Native& n = natives[i];
    if (!Live(n)) { n.live = false; return; }
    turret::SoftKillRaw(&n.slot);
    ++n.killed;
    n.live = false;
    Line("%s killed after %lu ms (%s)", n.name, static_cast<unsigned long>(GetTickCount() - n.createdAt), why);
}
void KillNatives(const char* why) {
    for (int i = 0; i < 2; ++i) KillNative(i, why);
}
// Every tick: note an effect that ended by itself (its life answers how long they last), end
// timed ones, and keep the screen effect up at the last stage.
void NativeTick(uint32_t player, int stageNow, DWORD now) {
    for (int i = 0; i < 2; ++i) {
        Native& n = natives[i];
        const bool live = Live(n);
        if (n.live && !live) {
            ++n.ended;
            n.lastLifeMs = now - n.createdAt;
            n.endedAt = Stamp(now);
            Line("%s ended by itself after %lu ms", n.name, static_cast<unsigned long>(n.lastLifeMs));
        }
        n.live = live;
        if (live && n.killAt && static_cast<LONG>(now - n.killAt) >= 0) KillNative(i, "its time is up");
    }
    if (stageNow >= 4 && !natives[kNativeScreen].live && (!natives[kNativeScreen].endedAt || now - natives[kNativeScreen].endedAt > 1500))
        StartNative(kNativeScreen, player, now, 0);
}

void Line(const char* fmt, ...) {
    if (InterlockedIncrement(&eventLines) > 400) return;
    char text[300];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    gate::Event("dementia %s", text);
}
// --- stage 2: the phantom Pack children (analysis/DEMENTIA_PHANTOM_PACK_2026-10-19.md) ---
// The hallucinated Pack of Dementia Land, from the pool copies (tools\append_dementia_records.py),
// spawned the way the bots spawn their body. It lives on this machine only: the host does not
// ghost it, a client's spawn filter lets it through (phantom::spawning), its target is our Isaac
// alone, its execution grab is refused, and every system that would share or count it asks
// phantom::Is. Its hits on our Isaac are real.
uint32_t Seed();   // below
bool packChildren = true;
constexpr uint32_t kPackVtableRva = 0x1786DF0, kSpawningVtableRva = 0x176400C, kAwarenessVtableRva = 0x17646B0;
constexpr uint32_t kFilterSlotRva = 0x17870F0, kFilterOriginalRva = 0xBE3050;   // PackEnemy vtable +0x300 FilterTarget, thiscall(candidate), ret 4
constexpr uint32_t kGrappleSlotRva = 0x178703C, kGrappleOriginalRva = 0xEEE2F0;   // +0x24C CheckInitGrapple, thiscall(), plain ret, al
constexpr uint32_t kFadeOutRva = 0x3D3220;                                       // Entity::FadeOut thiscall(ms, deleteWhenDone, bool), ret 0xC
constexpr uint32_t kCmcRva = 0x476E00, kCmcOrientRva = 0xB63540, kCmcPositionRva = 0xB638A0;
constexpr uint32_t kLevelManagerRva = 0x1C1C0A4;                                  // +0x18: 16 while the level runs
constexpr uint32_t kAwareness = 0x71C, kForcedAware = 0x724, kSpawnMode = 0x14;
bool filterSwapped = false, grappleSwapped = false, fadeOk = false;
uint32_t filterOriginal = 0, grappleOriginal = 0;
volatile LONG targetsOurs = 0, targetsRefused = 0, grabsRefused = 0;
struct Child {
    uint32_t entity = 0;
    unsigned record = 0;
    DWORD bornAt = 0, deadAt = 0, goneAt = 0;
    bool fading = false;
    float health = 0.0f;
};
Child children[phantom::kMax];
volatile LONG spawned = 0, spawnRefused = 0, noRecord = 0, noSpot = 0, spawnFaults = 0, died = 0, faded = 0, deleted = 0, forgotten = 0;

bool __fastcall OnFilterTarget(uint32_t npc, void*, uint32_t candidate) {
    using Fn = bool(__thiscall*)(uint32_t, uint32_t);
    if (!phantom::Is(npc)) return reinterpret_cast<Fn>(filterOriginal)(npc, candidate);
    const bool ours = candidate && candidate == LocalCampaignPlayer();
    InterlockedIncrement(ours ? &targetsOurs : &targetsRefused);
    return ours && reinterpret_cast<Fn>(filterOriginal)(npc, candidate);
}
bool __fastcall OnCheckInitGrapple(uint32_t npc, void*) {
    if (phantom::Is(npc)) { InterlockedIncrement(&grabsRefused); return false; }   // a hallucination never executes
    return reinterpret_cast<bool(__thiscall*)(uint32_t)>(grappleOriginal)(npc);
}
bool SwapSlot(FILE* log, const char* name, uint32_t slotRva, uint32_t expectedRva, uint32_t ours, uint32_t& original) {
    const uint32_t slot = base + slotRva;
    uint32_t current = 0;
    if (!Read(slot, &current, 4) || current != base + expectedRva) {
        std::fprintf(log, "dementia: PackEnemy %s slot %08X holds %08X, expected %08X; phantoms OFF\n", name, slot, current, base + expectedRva);
        return false;
    }
    DWORD previous = 0;
    void* at = reinterpret_cast<void*>(static_cast<uintptr_t>(slot));
    if (!VirtualProtect(at, 4, PAGE_READWRITE, &previous)) return false;
    original = current;
    std::memcpy(at, &ours, 4);
    VirtualProtect(at, 4, previous, &previous);
    std::fprintf(log, "dementia: PackEnemy %s slot %08X: %08X -> ours\n", name, slot, current);
    return true;
}
bool LevelRunning() {
    uint32_t manager = 0, state = 0;
    return inLevel && Read(base + kLevelManagerRva, &manager, 4) && manager && Read(manager + 0x18, &state, 4) && state == 16;
}
uint32_t FindComponent(uint32_t actor, uint32_t vtableRva) {
    for (uint32_t offset = 0; offset < hits::kActorScanBytes; offset += 4) {
        uint32_t candidate = 0, vtable = 0, owner = 0;
        if (!Read(actor + offset, &candidate, 4) || candidate < 0x10000 || !Read(candidate, &vtable, 4) || vtable != base + vtableRva) continue;
        if (Read(candidate + hits::kComponentOwner, &owner, 4) && owner == actor) return candidate;
    }
    return 0;
}
// Still our child: the same PackEnemy with the record's GUID.
bool StillThere(const Child& c) {
    uint32_t vtable = 0, guid[4]{};
    return c.entity && Read(c.entity, &vtable, 4) && vtable == base + kPackVtableRva && Read(c.entity + 0x24, guid, 16) &&
           std::memcmp(guid, coop_dementia_data::kDementiaRecords[c.record].guid, 16) == 0;
}
__declspec(noinline) uint32_t SpawnRaw(uint32_t sim, necro::SpawnInfo& info) {
    __try {
        return necro::SpawnByInfo(sim, info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
__declspec(noinline) bool PlaceAndFinalize(uint32_t entity, const float* rotation, const float* position, uint32_t& modeBefore) {
    __try {
        uint32_t vtable = 0, setRotation = 0, setPosition = 0;
        if (!Read(entity, &vtable, 4) || !Read(vtable + 0x24, &setRotation, 4) || !Read(vtable + 0x20, &setPosition, 4)) return false;
        reinterpret_cast<void(__thiscall*)(uint32_t, const float*)>(setRotation)(entity, rotation);
        reinterpret_cast<void(__thiscall*)(uint32_t, const float*)>(setPosition)(entity, position);
        if (void* movement = reinterpret_cast<void*(__cdecl*)(uint32_t)>(base + kCmcRva)(entity)) {
            reinterpret_cast<void(__thiscall*)(void*, const float*, int)>(base + kCmcOrientRva)(movement, rotation, 0);
            reinterpret_cast<void(__thiscall*)(void*, const float*)>(base + kCmcPositionRva)(movement, position);
        }
        // spawn mode NONE: no rail climb from Dementia Land, it stands there
        if (const uint32_t spawning = FindComponent(entity, kSpawningVtableRva)) {
            Read(spawning + kSpawnMode, &modeBefore, 4);
            const uint32_t none = 0;
            WriteBytes(spawning + kSpawnMode, &none, 4);
        }
        reinterpret_cast<void(__stdcall*)(uint32_t, int)>(base + necro::kFinalize)(entity, 0);
        uint32_t awareness = 0, awVtable = 0, owner = 0;
        if (Read(entity + kAwareness, &awareness, 4) && awareness && Read(awareness, &awVtable, 4) && awVtable == base + kAwarenessVtableRva &&
            Read(awareness + hits::kComponentOwner, &owner, 4) && owner == entity) {
            uint8_t flags = 0;
            if (Read(awareness + kForcedAware, &flags, 1)) { flags |= 4; WriteBytes(awareness + kForcedAware, &flags, 1); }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool FadeOut(uint32_t entity, unsigned ms) {
    if (!fadeOk) return false;
    __try {
        reinterpret_cast<void(__thiscall*)(uint32_t, unsigned, bool, bool)>(base + kFadeOutRva)(entity, ms, true, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool DeleteRaw(uint32_t entity) {
    __try {
        reinterpret_cast<void(__cdecl*)(uint32_t)>(base + necro::kDelete)(entity);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// A spot 8 to 15 m away on the mesh, out of view, preferably behind.
bool SpotFor(uint32_t player, float* out, float& yaw) {
    float me[3]{}, fwd[3]{0.0f, 0.0f, 1.0f};
    if (!PositionOf(player, me)) return false;
    Read(player + placement::kPlayerForward, fwd, 12);
    const float base0 = std::atan2(fwd[0], fwd[2]);
    for (int attempt = 0; attempt < 10; ++attempt) {
        const float spread = (Roll() - 0.5f) * (attempt < 5 ? 2.0f : 5.0f);   // behind first, then anywhere
        const float angle = base0 + 3.14159f + spread, distance = 8.0f + Roll() * 7.0f;
        const coop_bot::Vec3 from{me[0], me[1], me[2]}, want{me[0] + std::sin(angle) * distance, me[1], me[2] + std::cos(angle) * distance};
        coop_bot::Vec3 end{};
        int count = 0;
        if (!bot_nav::PathEnd(from, want, end, count)) continue;
        const float dx = end.x - me[0], dz = end.z - me[2], d = std::sqrt(dx * dx + dz * dz);
        if (d < 6.0f || std::fabs(end.y - me[1]) > 3.0f) continue;
        const float head[3] = {end.x, end.y + 0.8f, end.z};
        if (voices::InView(head)) continue;
        out[0] = end.x; out[1] = end.y; out[2] = end.z;
        yaw = std::atan2(me[0] - end.x, me[2] - end.z);
        return true;
    }
    return false;
}
unsigned AliveChildren() {
    unsigned n = 0;
    for (const Child& c : children)
        if (c.entity && !c.deadAt && !c.fading) ++n;
    return n;
}
bool SpawnChild(uint32_t player, DWORD now) {
    if (!packChildren || !filterSwapped || !grappleSwapped || !LevelRunning() || !levelSettled) { InterlockedIncrement(&spawnRefused); return false; }
    int slot = -1;
    for (unsigned i = 0; i < phantom::kMax && slot < 0; ++i)
        if (!children[i].entity) slot = static_cast<int>(i);
    if (slot < 0) { InterlockedIncrement(&spawnRefused); return false; }
    // a record no living child uses (the slot's own one)
    const unsigned record = static_cast<unsigned>(slot) % static_cast<unsigned>(std::size(coop_dementia_data::kDementiaRecords));
    uint32_t sim = 0;
    necro::SpawnInfo info{};
    if (!Read(base + necro::kSimManager, &sim, 4) || !sim || !necro::GetSpawnInfo(sim, info, coop_dementia_data::kDementiaRecords[record].guid)) {
        InterlockedIncrement(&noRecord);
        Line("pack child: record %08X-%08X not resident (the pool with the Dementia records is not installed or not loaded)",
             coop_dementia_data::kDementiaRecords[record].guid[0], coop_dementia_data::kDementiaRecords[record].guid[1]);
        return false;
    }
    float at[3]{}, yaw = 0.0f;
    if (!SpotFor(player, at, yaw)) { InterlockedIncrement(&noSpot); Line("pack child: no spot out of view on the mesh"); return false; }
    alignas(16) float rotation[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float s = std::sin(yaw), c = std::cos(yaw);
    rotation[0] = c; rotation[2] = -s; rotation[8] = s; rotation[10] = c;
    alignas(16) float matrix[16];
    std::memcpy(matrix, rotation, sizeof(matrix));
    matrix[12] = at[0]; matrix[13] = at[1]; matrix[14] = at[2];
    alignas(16) const float position[4] = {at[0], at[1], at[2], 1.0f};
    reinterpret_cast<void(__thiscall*)(necro::SpawnInfo*, const float*)>(base + necro::kSetTransform)(&info, matrix);
    InterlockedExchange(&phantom::spawning, 1);
    const uint32_t entity = SpawnRaw(sim, info);
    if (entity) InterlockedExchange(&phantom::entities[slot], static_cast<LONG>(entity));   // before the next Scan can see it
    InterlockedExchange(&phantom::spawning, 0);
    uint32_t vtable = 0;
    if (!entity || !Read(entity, &vtable, 4) || vtable != base + kPackVtableRva) {
        InterlockedExchange(&phantom::entities[slot], 0);
        InterlockedIncrement(&spawnFaults);
        Line("pack child: the spawn returned %08X (vtable %08X), not a PackEnemy", entity, vtable);
        return false;
    }
    uint32_t modeBefore = 0xFFFFFFFFu, owner = 0;
    const bool placed = PlaceAndFinalize(entity, rotation, position, modeBefore);
    Read(entity + 0x38, &owner, 4);
    Child& ch = children[slot];
    ch = Child{};
    ch.entity = entity;
    ch.record = record;
    ch.bornAt = Stamp(now);
    InterlockedIncrement(&spawned);
    Line("pack child %08X born at (%.1f %.1f %.1f), stage %d, record %u, spawn mode %u -> 0, owner %08X, placed %d, %s", entity, at[0], at[1], at[2], stage, record,
         modeBefore, owner, placed ? 1 : 0, IsHost() ? "host (kept out of the ghosting)" : "client (its own spawn)");
    if (!placed) { InterlockedIncrement(&spawnFaults); DeleteRaw(entity); InterlockedExchange(&phantom::entities[slot], 0); ch = Child{}; }
    return placed;
}
void Forget(unsigned i, const char* why) {
    Child& c = children[i];
    if (!c.entity) return;
    InterlockedIncrement(&forgotten);
    Line("pack child %08X gone (%s) after %lu ms", c.entity, why, static_cast<unsigned long>(GetTickCount() - c.bornAt));
    InterlockedExchange(&phantom::entities[i], 0);
    c = Child{};
}
// Every child away now: faded out while the level runs, else only forgotten (the level takes them).
void ClearChildren(const char* why) {
    for (unsigned i = 0; i < phantom::kMax; ++i) {
        Child& c = children[i];
        if (!c.entity) continue;
        if (LevelRunning() && StillThere(c)) {
            if (!c.fading && FadeOut(c.entity, 250)) { c.fading = true; c.goneAt = Stamp(GetTickCount()); InterlockedIncrement(&faded); continue; }
            if (!c.fading && DeleteRaw(c.entity)) InterlockedIncrement(&deleted);
        }
        if (!c.fading) Forget(i, why);
    }
}
// Each tick: a dying child flickers and fades; a fade that stalls is deleted.
void ChildTick(uint32_t player, DWORD now) {
    for (unsigned i = 0; i < phantom::kMax; ++i) {
        Child& c = children[i];
        if (!c.entity) continue;
        if (!StillThere(c)) { Forget(i, "the entity is gone"); continue; }
        if (c.fading) {
            if (now - c.goneAt > 2500) {
                if (LevelRunning() && DeleteRaw(c.entity)) InterlockedIncrement(&deleted);
                Forget(i, "faded");
            }
            continue;
        }
        const uint32_t hc = hits::FindHealthComponent(c.entity);
        float health = 1.0f;
        if (hc) Read(hc + hits::kHealthCurrent, &health, 4);
        // it knows where our Isaac is, as the director's enemies do (refreshed once a second)
        if (health > 0.0f && (now / 1000) != (c.bornAt / 1000)) {
            uint32_t awareness = 0, vtable = 0, owner = 0;
            uint8_t flags = 0;
            if (Read(c.entity + kAwareness, &awareness, 4) && awareness && Read(awareness, &vtable, 4) && vtable == base + kAwarenessVtableRva &&
                Read(awareness + hits::kComponentOwner, &owner, 4) && owner == c.entity && Read(awareness + kForcedAware, &flags, 1) && !(flags & 4)) {
                flags |= 4;
                WriteBytes(awareness + kForcedAware, &flags, 1);
            }
        }
        if (!c.deadAt && !(health > 0.0f)) {
            c.deadAt = Stamp(now);
            InterlockedIncrement(&died);
            StartNative(kNativeFlickerOn, player, now, 600);
            overlay::VeilBurstStart(overlay::kVeilGlitch, 0.8f, 700, Seed());
        }
        if (c.deadAt && now - c.deadAt > 300) {
            if (FadeOut(c.entity, 400)) { c.fading = true; c.goneAt = Stamp(now); InterlockedIncrement(&faded); }
            else if (DeleteRaw(c.entity)) { InterlockedIncrement(&deleted); Forget(i, "deleted at death"); }
        }
    }
}

// A spot next to the head, behind or to one side: where a whisper seems to come from.
void BesideHead(uint32_t player, float* out) {
    float pos[3]{}, fwd[3]{0.0f, 0.0f, 1.0f};
    PositionOf(player, pos);
    Read(player + placement::kPlayerForward, fwd, 12);
    const float side = Roll() < 0.5f ? -1.0f : 1.0f, back = 0.6f + Roll() * 0.8f;
    out[0] = pos[0] - fwd[0] * back + fwd[2] * side * 0.9f;
    out[1] = pos[1] + 1.6f;
    out[2] = pos[2] - fwd[2] * back - fwd[0] * side * 0.9f;
}
const File* Pick(const std::vector<File>& list) { return list.empty() ? nullptr : &list[static_cast<size_t>(Roll() * list.size()) % list.size()]; }
bool PlaySound(const std::vector<File>& list, uint32_t player, float gain, const char* who, bool beside) {
    const File* f = Pick(list);
    if (!f) return false;
    float at[3]{};
    if (beside) BesideHead(player, at);
    else { PositionOf(player, at); at[1] += 1.6f; }
    // Nicole speaks on the game's Voice slider; whispers and the ambience are effects
    return voices::PlayPhantom(f->path, at, gain, who, f->subtitle.empty() ? nullptr : f->subtitle.c_str(), &list != &nicole);
}
uint32_t Seed() { return static_cast<uint32_t>(Roll() * 4294967040.0f) | 1; }
bool Fire(int kind, uint32_t player, DWORD now) {
    bool ok = true;
    switch (kind) {
    case kWhisper: ok = PlaySound(whispers, player, 0.45f + 0.15f * stage, nullptr, true); break;
    case kFlickerMild: overlay::VeilBurstStart(overlay::kVeilFlickerMild, 1.0f, 900, Seed()); break;
    case kGlitch: overlay::VeilBurstStart(overlay::kVeilGlitch, 0.7f + 0.1f * stage, 1600, Seed()); break;
    case kFlicker: overlay::VeilBurstStart(overlay::kVeilFlicker, 1.0f, 1500, Seed()); break;
    case kNicoleLine:
        ok = PlaySound(nicole, player, 0.9f, "Nicole", false);
        if (ok) overlay::VeilBurstStart(overlay::kVeilFlickerMild, 0.8f, 700, Seed());
        break;
    case kRigWave: ok = notice::Show(Roll() < 0.5f ? "WARNING: LARGE NECROMORPH INCOMING" : "RIG: MULTIPLE CONTACTS CLOSING IN"); break;
    case kRigVitals:
        ok = notice::Show("RIG: VITALS CRITICAL");
        overlay::VeilBurstStart(overlay::kVeilGlitch, 0.5f, 900, Seed());
        break;
    case kFlickerHeavy: overlay::VeilBurstStart(overlay::kVeilFlickerHeavy, 1.0f, 2500, Seed()); break;
    case kRigPartnerLost: ok = peerTable.Count() > 0 && notice::Show("RIG: PARTNER SIGNAL LOST"); break;
    case kAmbience: ok = PlaySound(ambience, player, 0.35f, nullptr, false); break;
    case kFreakout:
        overlay::VeilBurstStart(overlay::kVeilFreakout, 1.0f, 4000, Seed());
        StartNative(kNativeFlickerOn, player, now, 2500);
        StartNative(kNativeScreen, player, now, 6000);
        PlaySound(whispers, player, 0.9f, nullptr, true);
        break;
    // the game's own effects; the veil stands in where they are not resident
    case kScreen:
        if (!StartNative(kNativeScreen, player, now, 5000)) overlay::VeilBurstStart(overlay::kVeilGlitch, 1.0f, 2000, Seed());
        break;
    case kNativeFlicker:
        if (!StartNative(kNativeFlickerOn, player, now, 2500)) overlay::VeilBurstStart(overlay::kVeilFlicker, 1.0f, 1500, Seed());
        break;
    case kPackChild: {
        // stage 2 one child at a time, stage 3 two, stage 4 three
        const unsigned limit = static_cast<unsigned>(stage - 1), want = stage >= 4 ? 3u : stage >= 3 ? 2u : 1u;
        unsigned born = 0;
        for (unsigned n = 0; n < want && AliveChildren() < limit; ++n)
            if (SpawnChild(player, now)) ++born;
        ok = born > 0;
        if (ok) PlaySound(whispers, player, 0.5f, nullptr, true);
        break;
    }
    default: return false;
    }
    InterlockedIncrement(ok ? &fired[kind] : &refused[kind]);
    if (ok) lastOf[kind] = Stamp(now);
    Line("%s %s at stage %d (meter %.0f, alone %lus, nearest %.1f m)", kRules[kind].name, ok ? "fired" : "REFUSED (no sound, voices off or notice off)", stage, meter,
         aloneSince ? static_cast<unsigned long>((now - aloneSince) / 1000) : 0ul, nearestM);
    return ok;
}
DWORD GapFor(int s) {
    static const DWORD kLow[5] = {0, 40000, 22000, 14000, 9000}, kHigh[5] = {0, 70000, 38000, 24000, 15000};
    const DWORD low = kLow[s], high = kHigh[s];
    return static_cast<DWORD>((low + Roll() * (high - low)) / (pace > 0.05f ? pace : 0.05f));
}
void MaybeFire(uint32_t player, DWORD now) {
    if (stage < 1 || static_cast<LONG>(now - nextEventAt) < 0) return;
    float total = 0.0f;
    bool eligible[kKinds]{};
    for (int k = 0; k < kKinds; ++k) {
        if (stage < kRules[k].minStage || (lastOf[k] && now - lastOf[k] < kRules[k].cooldownMs)) continue;
        if ((k == kWhisper && whispers.empty()) || (k == kNicoleLine && nicole.empty()) || (k == kAmbience && ambience.empty())) continue;
        if (k == kRigPartnerLost && !peerTable.Count()) continue;
        if (k == kPackChild && (!packChildren || !filterSwapped || !grappleSwapped || AliveChildren() >= static_cast<unsigned>(stage - 1))) continue;
        eligible[k] = true;
        total += WeightOf(k, stage);
    }
    nextEventAt = Stamp(now + GapFor(stage));
    if (total <= 0.0f) return;
    float pick = Roll() * total;
    for (int k = 0; k < kKinds; ++k) {
        if (!eligible[k]) continue;
        pick -= WeightOf(k, stage);
        if (pick <= 0.0f) { Fire(k, player, now); return; }
    }
}
void ApplyAmbient() {
    static const float kLevel[5] = {0.0f, 0.15f, 0.35f, 0.6f, 0.85f};
    overlay::VeilAmbient(activeNow ? kLevel[stage] : 0.0f, stage >= 4 ? 0x601010 : 0x20A0A0, !flashes);
}
void SetStage(int s, DWORD now, const char* why) {
    if (s == stage) return;
    const int was = stage;
    stage = s;
    InterlockedIncrement(s > was ? &stageUps : &stageDowns);
    Line("stage %d -> %d (meter %.0f, %s)", was, s, meter, why);
    ApplyAmbient();
    if (s < 2) { KillNatives("the stage fell"); ClearChildren("the stage fell"); }
    // a rising stage is felt at once
    if (s > was) {
        static const int kCue[5] = {-1, kWhisper, kGlitch, kFlicker, kFreakout};
        const uint32_t player = LocalCampaignPlayer();
        if (player && kCue[s] >= 0) Fire(kCue[s], player, now);
        nextEventAt = Stamp(now + GapFor(s) / 2);
    }
}
void Reset(const char* why) {
    if (meter > 0.0f || stage) Line("reset (%s)", why);
    KillNatives(why);
    ClearChildren(why);
    meter = 0.0f;
    stage = 0;
    aloneSince = 0;
    lastHealth = -1.0f;
    ApplyAmbient();
}

// Game thread, from coop::Poll.
void Tick(DWORD now) {
    if (!installed || now - lastTick < kTickMs) return;
    const float dt = lastTick ? std::min(1.0f, (now - lastTick) / 1000.0f) : 0.0f;
    lastTick = now;
    InterlockedIncrement(&ticks);
    const bool modeOk = inLevel && (play::SurvivalMode() || (campaignToo && play::StoryCoop())) && necro::LocalRole() == 0;
    const bool solo = modeOk && HumanPartners() == 0;
    const uint32_t player = modeOk && !solo ? LocalCampaignPlayer() : 0;
    if (!player) {
        if (activeNow) { activeNow = false; Reset(solo ? "solo: no human partner" : inLevel ? "not a Dementia game or no Isaac" : "the level is gone"); }
        InterlockedIncrement(solo ? &pausedSolo : &pausedOff);
        return;
    }
    if (!activeNow) { activeNow = true; ApplyAmbient(); Line("on: %s", play::SurvivalMode() ? "survival" : "story co-op"); }
    float me[3]{};
    if (!PositionOf(player, me)) return;
    float fraction = 1.0f, current = 0.0f, max = 0.0f;
    const bool haveHealth = HealthOf(player, fraction, current, max);
    // a med pack (a jump of health) clears the head a little
    if (haveHealth && coop_dementia::Healed(lastHealth, current, max)) {
        meter = std::max(0.0f, meter - 20.0f);
        InterlockedIncrement(&medDrops);
        Line("health %.0f -> %.0f: meter -20", lastHealth, current);
    }
    lastHealth = haveHealth ? current : -1.0f;
    pauseWhy = scenekind::local != scenekind::kNone ? "scene" : haveHealth && current <= 0.0f ? "dead" : "";
    if (pauseWhy[0]) {
        InterlockedIncrement(pauseWhy[0] == 's' ? &pausedScene : &pausedDead);
        if (!wasPaused) { KillNatives(pauseWhy); ClearChildren(pauseWhy); }
        wasPaused = true;
        ChildTick(player, now);   // the fades finish
        return;
    }
    if (shop::open && (natives[0].live || natives[1].live)) KillNatives("the store is open");
    if (shop::open && AliveChildren()) ClearChildren("the store is open");
    // a level change or restart on its way: away while the level still runs (pool children outlive a restart)
    if (phantom::Any() && !LevelRunning()) ClearChildren("the level is changing");
    ChildTick(player, now);
    if (wasPaused) { wasPaused = false; nextEventAt = Stamp(now + GapFor(stage < 1 ? 1 : stage) / 2); }
    nearestM = NearestCompany(me, companyNow);
    const bool alone = nearestM < 0.0f || nearestM > aloneM;
    if (alone && !shop::open) { if (!aloneSince) aloneSince = Stamp(now); }
    else aloneSince = 0;
    meter = coop_dementia::Step(meter, alone, companyNow > 0, haveHealth ? fraction : -1.0f, shop::open, dt, rates);
    if (forceStage >= 0 && forceStage <= 4) meter = forceStage ? kStageAt[forceStage] + 1.0f : 0.0f;
    const int want = coop_dementia::StageFor(meter, stage);
    if (want != stage) SetStage(want, now, alone ? (companyNow ? "alone" : "nobody else here") : "company");
    if (!shop::open) NativeTick(player, stage, now);
    MaybeFire(player, now);
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Dementia", "Enabled", 0, ini.c_str()) != 0;
    if (!enabled) { std::fprintf(log, "dementia off ([Dementia] Enabled=0)\n"); return; }
    char text[64]{};
    GetPrivateProfileStringA("Dementia", "Modes", "survival", text, sizeof(text), ini.c_str());
    campaignToo = _stricmp(text, "all") == 0;
    botsCount = GetPrivateProfileIntA("Dementia", "BotsCount", 1, ini.c_str()) != 0;
    flashes = GetPrivateProfileIntA("Dementia", "Flashes", 1, ini.c_str()) != 0;
    const int alone = GetPrivateProfileIntA("Dementia", "AloneMeters", 15, ini.c_str());
    aloneM = static_cast<float>(alone < 3 ? 3 : alone > 100 ? 100 : alone);
    const int rise = GetPrivateProfileIntA("Dementia", "RiseSeconds", 150, ini.c_str());
    rates.risePerS = 100.0f / static_cast<float>(rise < 20 ? 20 : rise > 1800 ? 1800 : rise);
    const int fall = GetPrivateProfileIntA("Dementia", "FallSeconds", 40, ini.c_str());
    rates.fallPerS = 100.0f / static_cast<float>(fall < 5 ? 5 : fall > 600 ? 600 : fall);
    const int pacePercent = GetPrivateProfileIntA("Dementia", "Pace", 100, ini.c_str());
    pace = static_cast<float>(pacePercent < 25 ? 25 : pacePercent > 400 ? 400 : pacePercent) / 100.0f;
    forceStage = GetPrivateProfileIntA("Dementia", "ForceStage", -1, ini.c_str());
    const size_t slash = ini.find_last_of("\\/");
    folder = (slash == std::string::npos ? std::string(".") : ini.substr(0, slash)) + "\\dementia";
    ScanFiles(log);
    const bool veil = overlay::Install(log, ini);
    bool createOk = false, findOk = false, softOk = false;
    nativeOk = turret::FxCoreOk(createOk, findOk, softOk);
    std::fprintf(log, "dementia: the game's dementia effects %s (Create_ByGuid %s, FindInitByGuid %s, SoftKill %s)\n", nativeOk ? "usable" : "OFF, the veil stands in",
                 createOk ? "ok" : "BYTES DIFFER", findOk ? "ok" : "BYTES DIFFER", softOk ? "ok" : "BYTES DIFFER");
    packChildren = GetPrivateProfileIntA("Dementia", "PackChildren", 1, ini.c_str()) != 0;
    if (packChildren) {
        // cmp byte [esp+8],0 / jz / or dword [ecx+168h],4
        static const unsigned char kFade[] = {0x80, 0x7C, 0x24, 0x08, 0x00, 0x74, 0x07, 0x83, 0x89, 0x68, 0x01, 0x00, 0x00, 0x04};
        unsigned char head[sizeof(kFade)]{};
        fadeOk = Read(base + kFadeOutRva, head, sizeof(head)) && std::memcmp(head, kFade, sizeof(kFade)) == 0;
        filterSwapped = SwapSlot(log, "FilterTarget", kFilterSlotRva, kFilterOriginalRva, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&OnFilterTarget)),
                                 filterOriginal);
        grappleSwapped = filterSwapped && SwapSlot(log, "CheckInitGrapple", kGrappleSlotRva, kGrappleOriginalRva,
                                                   static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&OnCheckInitGrapple)), grappleOriginal);
        std::fprintf(log, "dementia: pack children %s (target filter %s, execution grab refused %s, FadeOut %s; records %08X-%08X.. x%zu from the pool)\n",
                     filterSwapped && grappleSwapped ? "ready" : "OFF", filterSwapped ? "ours" : "NOT swapped", grappleSwapped ? "yes" : "NO",
                     fadeOk ? "ok" : "BYTES DIFFER (deleted instead)", coop_dementia_data::kDementiaRecords[0].guid[0], coop_dementia_data::kDementiaRecords[0].guid[1],
                     std::size(coop_dementia_data::kDementiaRecords));
    } else {
        std::fprintf(log, "dementia: pack children off ([Dementia] PackChildren=0)\n");
    }
    rng ^= GetTickCount();
    installed = true;
    std::fprintf(log, "dementia on: modes %s, alone beyond %.0f m (bots %s), 0->100 in %.0f s alone, only with a human partner, 100->0 in %.0f s together, pace %.0f%%, "
                      "flashes %d, force stage %d; screen veil %s, voices %s, notices %s\n",
                 campaignToo ? "survival+story" : "survival", aloneM, botsCount ? "count" : "do not count", 100.0f / rates.risePerS,
                 100.0f / rates.fallPerS,
                 pace * 100.0f, flashes ? 1 : 0, forceStage, veil ? "ready" : "MISSING (overlay)", voices::installed ? "ready" : "OFF (no sounds)",
                 notice::installed && notice::enabled ? "ready" : "OFF (no RIG lies)");
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    LONG sum = stageUps * 3 + stageDowns * 5 + medDrops * 7 + static_cast<LONG>(meter) * 11 + stage * 13;
    for (int k = 0; k < kKinds; ++k) sum += fired[k] * (17 + k) + refused[k] * (41 + k);
    for (const Native& n : natives) sum += n.created * 67 + n.absent * 71 + n.refused * 73 + n.ended * 79 + n.killed * 83;
    sum += spawned * 89 + spawnRefused * 97 + noRecord * 101 + noSpot * 103 + spawnFaults * 107 + died * 109 + faded * 113 + deleted * 127 + forgotten * 131 +
           targetsRefused * 137 + grabsRefused * 139 + phantom::hitsZeroed * 149 + phantom::worldHitsKept * 151 + phantom::dropsRefused * 157 +
           phantom::messagesDropped * 163 + phantom::keptOnHost * 167 + phantom::allowedOnClient * 173 + targetsOurs / 50;
    if (sum == last) return;
    last = sum;
    std::string kinds;
    for (int k = 0; k < kKinds; ++k) {
        if (!fired[k] && !refused[k]) continue;
        char part[48];
        std::snprintf(part, sizeof(part), "%s%s=%ld/%ld", kinds.empty() ? "" : " ", kRules[k].name, fired[k], refused[k]);
        kinds += part;
    }
    std::fprintf(log, "dementia meter=%.0f stage=%d active=%d nearest=%.1fm company=%u ups=%ld downs=%ld med_drops=%ld paused scene=%ld dead=%ld off=%ld solo=%ld | "
                      "events fired/refused: %s | veil bursts=%ld frames=%ld\n",
                 meter, stage, activeNow ? 1 : 0, nearestM, companyNow, stageUps, stageDowns, medDrops, pausedScene, pausedDead, pausedOff, pausedSolo,
                 kinds.empty() ? "none" : kinds.c_str(), InterlockedCompareExchange(&overlay::veilBursts, 0, 0), InterlockedCompareExchange(&overlay::veilFrames, 0, 0));
    for (const Native& n : natives)
        std::fprintf(log, "dementia %s created=%ld not_resident=%ld refused=%ld ended_by_itself=%ld (last after %lu ms) killed=%ld live=%d\n", n.name, n.created,
                     n.absent, n.refused, n.ended, static_cast<unsigned long>(n.lastLifeMs), n.killed, n.live ? 1 : 0);
    if (packChildren)
        std::fprintf(log, "dementia pack children alive=%u spawned=%ld refused=%ld no_record=%ld no_spot=%ld faults=%ld died=%ld faded=%ld deleted=%ld gone=%ld | "
                          "targets ours=%ld refused=%ld grabs_refused=%ld | host kept_out=%ld client let_through=%ld | hits zeroed=%ld world_hits_kept=%ld "
                          "drops_refused=%ld messages_dropped=%ld corpses_unshared=%ld\n",
                     AliveChildren(), spawned, spawnRefused, noRecord, noSpot, spawnFaults, died, faded, deleted, forgotten, targetsOurs, targetsRefused, grabsRefused,
                     phantom::keptOnHost, phantom::allowedOnClient, phantom::hitsZeroed, phantom::worldHitsKept, phantom::dropsRefused, phantom::messagesDropped,
                     phantom::restSkipped);
}
}  // namespace dementia
