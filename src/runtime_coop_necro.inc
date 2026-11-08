// Dead Space 2 United by Richard Hafer.
// Play as a necromorph in the campaign (2026-09-21).
//
// The multiplayer's playable necromorph is a complete class (NecroPlayer) whose
// four authored templates, together with the MPLevelSettings they hang off, ship
// in DS2DAT9's global_mp_assets stream. Nothing about them is gated on the game
// being a multiplayer match: the spawn filter only filters by class when its
// game mode is non-zero, and NecroPlayer's post-spawn init tolerates the
// absence of an online player. So the campaign can host one.
//
// What this does, on the game thread, when [Necro] Key is pressed:
//   1. keeps global_mp_assets\global_mp_assets resident (stream manager Load +
//      Lock, the same calls the randomizer pool uses),
//   2. makes sure the MPLevelSettings singleton exists (spawns the authored one
//      by GUID if the stream did not spawn it),
//   3. takes the local Player out of PlayerManager slot 0,
//   4. spawns the chosen NecroPlayer template at the player's transform and
//      registers it into slot 0 (the slot IS the controls and the camera),
//   5. registers the necro with the co-op session so the partner ghosts it.
// Pressing the key again gives slot 0 back to the parked Player.
//
// Everything below is the body of NecroPlayer::Spawn (0x010A5650) done by hand,
// because the native one only knows how to place a necro at a vent.
#include "generated_mp_autospawn_block.h"
namespace necro {
bool enabled = false, registerNet = true, wantPool = true, spawnSettings = true;
int key = VK_F11, type = 0;
bool keyDown = false, keyPressedByJoin = false;
// The stream spawns its own level logic on load (crash 28644: a UIPopup
// constructor with no text assets to read); only what the plugin asks for may exist.
bool allowMp = false;
volatile LONG blockedSpawns = 0;
// From OnCreator (0x777730), before the native creator runs. The request keeps
// the entity GUID at arg1+0x0C. A blocked request returns no entity, which is
// what the native spawn filter answers for anything it rejects.
bool (*versusPassesClass)(const char* cls) = nullptr;   // runtime_coop_versus.inc, [Versus] NativeAudio
bool ShouldBlock(uint32_t request) {
    if (!enabled || allowMp) return false;
    uint32_t guid[4]{};
    if (!Read(request + 0x0C, guid, sizeof(guid))) return false;
    for (const auto& entry : ds2::kMpAutoSpawnBlock) {
        if (entry.words[0] == guid[0] && entry.words[1] == guid[1] &&
            entry.words[2] == guid[2] && entry.words[3] == guid[3]) {
            if (versusPassesClass && entry.cls && versusPassesClass(entry.cls)) return false;
            InterlockedIncrement(&blockedSpawns);
            return true;
        }
    }
    return false;
}

// engine anchors (RVAs)
constexpr uint32_t kStreamManager = 0x1C1C098, kStreamLoad = 0x34BDD0, kStreamLock = 0x34AC80, kStreamStatus = 0xDAA0;
constexpr uint32_t kStreamUnlock = 0x34ACB0;   // the counterpart of kStreamLock; a lock held on a stream that never loads keeps the level loading forever
// The engine's file service, the thing that actually opens a TOC. It hands out
// slots from a small table (0x54 holds the count, 0x4C the array, one byte per
// slot at +4 marks it taken) and when none is free the TOC loader returns NO
// TOC AND NO ERROR. That is exactly what happened to the Leaper's stream:
// registry stable, 25 TOCs, the file opening fine from the plugin's own thread,
// and no descriptor. So the request has to wait for a free slot instead of
// being fired into a full table, the same rule the randomizer's probe follows.
constexpr uint32_t kFileService = 0x1C1C090;
uint32_t FileSlotsUsed(uint32_t& slots) {
    uint32_t service = 0, slotArray = 0, used = 0;
    slots = 0;
    if (!Read(base + kFileService, &service, 4) || !service || !Read(service + 0x54, &slots, 4) ||
        !Read(service + 0x4C, &slotArray, 4) || slots > 64) { slots = 0; return 0; }
    for (uint32_t i = 0; i < slots; ++i) {
        unsigned char taken = 0;
        if (Read(slotArray + 16 * i + 4, &taken, 1) && taken) ++used;
    }
    return used;
}
volatile LONG fileSlotWaits = 0;
bool FileSlotsBusy() {
    uint32_t slots = 0;
    const uint32_t used = FileSlotsUsed(slots);   // slot 0 is never handed out
    if (slots && used + 2 >= slots) { InterlockedIncrement(&fileSlotWaits); return true; }
    return false;
}
constexpr uint32_t kSimManager = 0x1C1C0A0, kGetSpawnInfo = 0x376440, kSetTransform = 0x44A5B0;
constexpr uint32_t kSpawnEntity = 0x376480, kFinalize = 0x3752A0, kDelete = 0x3568B0;
constexpr uint32_t kMPLevelSettings = 0x1C21624, kEntityDataManager = 0x1C21300, kSpawnFilterByte = 0x28;
constexpr uint32_t kPlayerManager = 0x1C1C0D4, kPmRegister = 0x37BB60, kPmUnregister = 0x37BC00;
constexpr uint32_t kRegisterWithManager = 0xC8C310, kNecroVtable = 0x17783A0;
constexpr uint32_t kCameraManager = 0x1C2131C, kGetActiveCamData = 0x392920, kSetFixedCamera = 0xB4F870, kPushCameraInfo = 0x392530;
constexpr uint32_t kBasePlayer = 0x3C0, kPlayerId = 0x3DC, kNecroFlags = 0x3D8, kNecroCamera = 0x400, kNecroType = 0x3CC;
constexpr uint32_t kPlayerIdOfPlayer = 0x420;   // Player keeps its manager id here (Player::RegisterWithManager writes [ebx+0x420])
constexpr uint32_t kEntityMatrix = 0x90, kEntitySubKeyWord = 0x38, kReflectable = 0x5C;
constexpr uint32_t kSettingsAttributes = 0xC, kSettingsNecroGuids = 0x78;
constexpr const char* kPoolName = "global_mp_assets\\global_mp_assets";
namespace leaper { void Restore(); void Forget(); void Apply(uint32_t attributes); extern bool enabled; }   // defined further down, beside the rest of the sixth type
namespace sixth { extern bool enabled; extern uint32_t slot; extern uint32_t guid[4]; bool Configured(); }   // likewise: the menu and the transform both have to know a sixth type exists
// The Leaper's own character stream, named the way the four playable ones are:
// the archive path without the container prefix and without the extension.
// Configurable ([Necro] SixthCharStream) since the Ubermorph joined the Leaper:
// the sixth type is whichever creature its record and this stream describe.
char kLeaperStreamName[160] = "global_assets\\global_assets\\ds_assets\\char_str\\npc\\leap_cct\\leaper";
// The plugin's own record, built by tools/build_necroplayer_leaper.py: a loose
// stream beside the randomizer pool, asked for by name the same way.
// Configurable, so "is it this file, or are loose streams broken in this process
// at all?" can be settled without a rebuild: point it at
// randomizer\ds2_randomizer_pool, a loose stream that is known to load, and see
// whether that one arrives either.
char leaperRecordStream[128] = "randomizer\\ds2_leaper";
uint32_t leaperModelHandle = 0, leaperRecordHandle = 0;
uint32_t leaperRequestedAt = 0;
constexpr uint32_t kLeaperGiveUpMs = 20000;   // measured in time: polls run at the frame rate, and 600 of them were ten seconds
// StreamManager::Unload(handle), PC RVA 0x2C1E0 (thiscall, ret 4): the public
// entry the game itself uses: StreamFromHandle, then Unload(Stream*) at
// 0x34B330, which is Cancel(stream, 0) at 0x34B060. Found 22.09. by its shape
// against the Xbox debug build (?Unload@StreamManager@@QAAXI@Z calls
// StreamFromHandle and ?Unload@...@AAAXPAVStream@@@Z, which calls Cancel).
// Unlock alone is not a way out: the second test on 22.09. gave the lock back
// after 20 s and the loading screen went on waiting, because the request
// itself (EXPLICIT_LOAD, still QUEUED) was still there.
constexpr uint32_t kStreamUnload = 0x2C1E0;
// The record is asked for from INSIDE the stream manager's update, the way the
// randomizer asks for every loose stream it has ever loaded (OnStreamUpdate on
// 0x34BBC0). Asked from the frame tick, a loose stream with a TOC of its own
// was accepted, got its TOC registered, and stayed QUEUED for good (two tests
// on 22.09., read live: status 0, flags 0x1E0, request id still at +0x28).
// Streams out of TOCs that were already there load either way, which is why
// the Leaper's character stream always arrived and the record never did.
safetyhook::MidHook leaperUpdateHook;
volatile LONG leaperRecordWanted = 0, leaperRecordCancel = 0, leaperUpdateServed = 0, leaperUpdateCancelled = 0;
void OnLeaperStreamUpdate(SafetyHookContext& ctx) {
    const bool wanted = InterlockedCompareExchange(&leaperRecordWanted, 0, 0) != 0;
    const bool cancel = InterlockedCompareExchange(&leaperRecordCancel, 0, 0) != 0;
    if (!wanted && !cancel) return;
    uint32_t manager = 0;
    if (!Read(base + kStreamManager, &manager, 4) || !manager || manager != ctx.ecx) return;
    void* const self = reinterpret_cast<void*>(manager);
    if (cancel) {
        const uint32_t handle = leaperRecordHandle;
        if (handle) {
            using Unlock = bool (__thiscall*)(void*, uint32_t);
            using Unload = void (__thiscall*)(void*, uint32_t);
            reinterpret_cast<Unlock>(base + kStreamUnlock)(self, handle);
            reinterpret_cast<Unload>(base + kStreamUnload)(self, handle);
            // Released: DropOurStreams must not unlock it a second time.
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&leaperRecordHandle), 0);
        }
        InterlockedExchange(&leaperRecordCancel, 0);
        InterlockedExchange(&leaperRecordWanted, 0);
        InterlockedIncrement(&leaperUpdateCancelled);
        return;
    }
    if (leaperRecordHandle) { InterlockedExchange(&leaperRecordWanted, 0); return; }
    using Load = uint32_t (__thiscall*)(void*, const char*, float, unsigned, const void*, const char*);
    using Lock = bool (__thiscall*)(void*, uint32_t);
    const uint32_t handle = reinterpret_cast<Load>(base + kStreamLoad)(self, leaperRecordStream, 0.0f, 2, nullptr, nullptr);
    if (handle) reinterpret_cast<Lock>(base + kStreamLock)(self, handle);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&leaperRecordHandle), static_cast<LONG>(handle));
    InterlockedExchange(&leaperRecordWanted, 0);
    InterlockedIncrement(&leaperUpdateServed);
}
// The Leaper has no ranged attack, and the Lurker's state machine it runs on
// does not know that. NecroPlayerLurkerSM::UpdateTentacles (PC 0x010C1FC0,
// thiscall(sm, unsigned, float), ret 8; found by the four tentacle animation
// hashes it plays, F744EA30 / 0C439644 / EA8E8438 / 899A6FE9) reads the fire
// and aim buttons, switches the necro into its aiming and tentacle states and
// asks for Lurker animations the Leaper's banks do not carry. The first test
// (22.09. 21:1x): after aiming the Leaper could no longer jump, attack or
// grab; the state never came back. So for a Leaper the whole function is
// skipped: no aim, no fire, the state stays where a necro without tentacles
// would have it. A Leaper is recognised by its model, CMD_SetModel at
// attributes+0 (Entity::Attributes, PDB), so its ghost on another machine is
// recognised the same way.
constexpr uint32_t kUpdateTentacles = 0xCC1FC0;
// The creatures built on another type's state machine that have no ranged
// attack of their own: the Leaper (Lurker SM) and the Ubermorph (Puker's
// Spitter SM). Recognised by CMD_SetModel at attributes+0.
constexpr uint32_t kNoRangedModels[][4] = {
    {0xACB61CC3u, 0xACB61CC3u, 0xACB61CC3u, 0x2F24BB5Bu},   // Leaper
    {0xA9337145u, 0xA9337145u, 0xA9337145u, 0xAD81D369u},   // Ubermorph ("Hunter")
};
safetyhook::InlineHook tentaclesHook;
volatile LONG tentaclesSkipped = 0, tentaclesPassed = 0;
bool IsLeaperNecro(uint32_t entity) {
    uint32_t attributes = 0, model[4]{};
    if (!entity || !Read(entity + 0xC, &attributes, 4) || !attributes || !Read(attributes, model, sizeof(model))) return false;
    for (const auto& known : kNoRangedModels)
        if (std::memcmp(model, known, sizeof(model)) == 0) return true;
    return false;
}
volatile LONG tentacleOwnerModel = 0, tentacleOwnerType = -1;
void __fastcall OnUpdateTentacles(void* sm, void*, uint32_t a, float dt) {
    uint32_t owner = 0;
    // What the first caller looks like, for the status line: whether a necro
    // runs the Lurker's state machine at all, and what it was spawned from.
    if (InterlockedCompareExchange(&tentacleOwnerType, 0, 0) == -1 && sm &&
        Read(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sm)) + 0x54, &owner, 4) && owner) {
        uint32_t attributes = 0, model = 0, necroType = 0;
        if (Read(owner + 0xC, &attributes, 4) && attributes) { Read(attributes, &model, 4); Read(attributes + 0x194, &necroType, 4); }
        InterlockedExchange(&tentacleOwnerModel, static_cast<LONG>(model));
        InterlockedExchange(&tentacleOwnerType, static_cast<LONG>(necroType));
        owner = 0;
    }
    if (sm && Read(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sm)) + 0x54, &owner, 4) && IsLeaperNecro(owner)) {
        InterlockedIncrement(&tentaclesSkipped);
        return;
    }
    InterlockedIncrement(&tentaclesPassed);
    tentaclesHook.thiscall<void>(sm, a, dt);
}
// The Spitter state machine's aim and fire, which the Puker (and so the
// Ubermorph built from it) runs on: NecroPlayerSpitterSM::UpdateAimMode, PC
// 0x0108DE60 (thiscall(sm), plain ret; found by the three controller events it
// polls, DD53A293 / E062D890 / 1A909F46, which only it and the Lurker's
// UpdateTentacles use together; it compares m_type with 3, the Puker, itself).
// Charging and vomiting would ask for the Puker's animations, which the
// Ubermorph does not have; the Leaper's tentacles showed what that does to a
// state machine. Skipped for the creatures above; sprint and melee are
// elsewhere and stay.
constexpr uint32_t kSpitterUpdateAimMode = 0xC8DE60;
safetyhook::InlineHook spitterAimHook;
volatile LONG spitterAimSkipped = 0;
void __fastcall OnSpitterUpdateAimMode(void* sm, void*) {
    uint32_t owner = 0;
    if (sm && Read(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sm)) + 0x54, &owner, 4) && IsLeaperNecro(owner)) {
        InterlockedIncrement(&spitterAimSkipped);
        return;
    }
    spitterAimHook.thiscall<void>(sm);
}
// The Ubermorph takes no damage at all while it holds someone. The gate is
// NecroPlayer::ShouldProcessDamage, PC 0x01071A60 (NecroPlayer vtable +0x20C,
// thiscall(this, const DamageData&), ret 4; Xbox reads Flags32 +0x3F8 bit 0x80,
// the PC the same bit at +0x3D8). Sentient's damage entry (0x00EE0D9C) asks it
// first and skips the health component's HandleDamage AND RespondToDamage when
// it says no: no health loss, no limbs, no flinch that could break the grab.
// "Holds someone" = Sentient::m_pPairedCoordinator, PC +0x2A8: Xbox +0x2C8,
// the PC 0x20 lower, read off RevokePairedCoordinatorControl (0x00E94000,
// mov ecx,[eax+0x2A8]) and SetPairedCoordinatorCount (0x00E94020, +0x2AC),
// which NecroPlayerSpitterSM::SetListeningForCoordinator (0x0108E0B0) calls.
constexpr uint32_t kNecroShouldProcessDamage = 0xC71A60, kPairedCoordinator = 0x2A8;
safetyhook::InlineHook grabImmuneHook;
volatile LONG grabHitsBlocked = 0;
bool IsUbermorphNecro(uint32_t entity) {
    uint32_t attributes = 0, model[4]{};
    return entity && Read(entity + 0xC, &attributes, 4) && attributes && Read(attributes, model, sizeof(model)) &&
           std::memcmp(model, kNoRangedModels[1], sizeof(model)) == 0;
}
bool __fastcall OnNecroShouldProcessDamage(void* self, void*, const void* damage) {
    const uint32_t entity = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    uint32_t coordinator = 0;
    if (IsUbermorphNecro(entity) && Read(entity + kPairedCoordinator, &coordinator, 4) && coordinator) {
        InterlockedIncrement(&grabHitsBlocked);
        return false;
    }
    return grabImmuneHook.thiscall<bool>(self, damage);
}
void InstallGrabImmunity(FILE* log) {
    // mov eax,[ecx+0x3D8] / shr eax,7 / not al / and al,1 / ret 4
    static const unsigned char expected[] = {0x8B, 0x81, 0xD8, 0x03, 0x00, 0x00, 0xC1, 0xE8, 0x07,
                                             0xF6, 0xD0, 0x24, 0x01, 0xC2, 0x04, 0x00};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kNecroShouldProcessDamage, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) {
        std::fprintf(log, "necro grab immunity: signature mismatch at %08X, not installed\n", static_cast<unsigned>(base + kNecroShouldProcessDamage));
        return;
    }
    grabImmuneHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kNecroShouldProcessDamage), reinterpret_cast<void*>(OnNecroShouldProcessDamage));
    std::fprintf(log, "necro grab immunity %s at %08X (Ubermorph takes no damage while its paired attack runs)\n",
                 grabImmuneHook ? "installed" : "FAILED", static_cast<unsigned>(base + kNecroShouldProcessDamage));
}
void InstallSpitterAimGuard(FILE* log) {
    // push ecx / push ebx / push ebp / push esi / mov esi,[0x0201C0AC] / mov ebp,ecx / movzx ecx,byte [ebp+0x304]
    static const unsigned char expected[] = {0x51, 0x53, 0x55, 0x56, 0x8B, 0x35, 0xAC, 0xC0, 0x01, 0x02, 0x8B, 0xE9,
                                             0x0F, 0xB6, 0x8D, 0x04, 0x03, 0x00, 0x00, 0x57};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kSpitterUpdateAimMode, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) {
        std::fprintf(log, "necro spitter aim guard: signature mismatch at %08X, not installed\n", static_cast<unsigned>(base + kSpitterUpdateAimMode));
        return;
    }
    spitterAimHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kSpitterUpdateAimMode), reinterpret_cast<void*>(OnSpitterUpdateAimMode));
    std::fprintf(log, "necro spitter aim guard %s at %08X\n", spitterAimHook ? "installed" : "FAILED", static_cast<unsigned>(base + kSpitterUpdateAimMode));
}
void InstallTentacleGuard(FILE* log) {
    // sub esp,0x14 / movss xmm0,[0x01AA5ED0] / push ebx / push ebp / push esi / mov esi,ecx / mov ebp,[esi+0x54]
    static const unsigned char expected[] = {0x83, 0xEC, 0x14, 0xF3, 0x0F, 0x10, 0x05, 0xD0, 0x5E, 0xAA, 0x01,
                                             0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x6E, 0x54};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kUpdateTentacles, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) {
        std::fprintf(log, "necro tentacle guard: signature mismatch at %08X, not installed\n", static_cast<unsigned>(base + kUpdateTentacles));
        return;
    }
    tentaclesHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kUpdateTentacles), reinterpret_cast<void*>(OnUpdateTentacles));
    std::fprintf(log, "necro tentacle guard %s at %08X (the Leaper skips the Lurker's aim and tentacle attack)\n",
                 tentaclesHook ? "installed" : "FAILED", static_cast<unsigned>(base + kUpdateTentacles));
}
void InstallLeaperStreamUpdate(FILE* log) {
    static const unsigned char expected[] = {0x83, 0xEC, 0x08, 0x53, 0x8B, 0xD9, 0xE8, 0x45, 0xCF, 0x0C, 0x00};
    static const unsigned char unloadExpected[] = {0x8B, 0x44, 0x24, 0x04, 0x56, 0x50, 0x8B, 0xF1};
    unsigned char actual[sizeof(expected)]{}, unload[sizeof(unloadExpected)]{};
    // The randomizer's pool request hooks the same stream update; a finished hook there is chained.
    hook_site::Lock siteLock(static_cast<uint32_t>(base + 0x34BBC0));
    if (!Read(base + 0x34BBC0, actual, sizeof(actual)) ||
        hook_site::Inspect(actual, expected, sizeof(expected)) == hook_site::State::Foreign ||
        !Read(base + kStreamUnload, unload, sizeof(unload)) || std::memcmp(unload, unloadExpected, sizeof(unload)) != 0) {
        std::fprintf(log, "necro leaper stream_update hook: signature mismatch (another plugin there?), requests stay on the frame tick\n");
        return;
    }
    const bool hooked = hook_site::InstallMid(log, "necro leaper stream_update", base + 0x34BBC0, expected, sizeof(expected),
                                              OnLeaperStreamUpdate, leaperUpdateHook);
    std::fprintf(log, "necro leaper stream_update hook %s at %08X\n", hooked ? "installed" : "FAILED",
                 static_cast<unsigned>(base + 0x34BBC0));
}
int leaperModelStatus = -1, leaperRecordStatus = -1;
// The authored MPLevelSettings in global_mp_assets (SimGroup entity #55).
constexpr uint32_t kSettingsGuid[4] = {0x58C9C3B1u, 0x0878CD53u, 0x3532554Cu, 0x58583337u};
// The four templates, in NecroPlayerType order (PACK, LURKER, SPITTER, PUKER);
// MPLevelSettings carries the same four at attributes+0x78, this is the fallback.
constexpr uint32_t kTemplateGuid[4][4] = {
    {0x58CA0CA9u, 0xDF9A22B2u, 0x3532554Cu, 0x58583337u},
    {0x58CA17A0u, 0xB16998A0u, 0x3532554Cu, 0x58583337u},
    {0x58CAC498u, 0x725F1CF9u, 0x3532554Cu, 0x58583337u},
    {0x58CB1317u, 0x5D04C30Bu, 0x3532554Cu, 0x58583337u},
};
const char* const kTypeNames[4] = {"pack", "lurker", "spitter", "puker"};
// kTypeNames only has the four the game ships; a sixth type has to be named
// without reading past the end of it.
const char* TypeName(int which) { return (which >= 0 && which < 4) ? kTypeNames[which] : "leaper"; }
// The type's character stream (mesh, skeleton, dismemberment). The root TOC
// only registers it; SpawnEntity's filter refuses a NecroPlayer whose model
// stream is not loaded (first live test: SpawnEntity returned 0 with the root
// loaded and MPLevelSettings spawned), so it is requested like the root.
const char* const kModelStreams[4] = {
    "global_mp_assets\\global_mp_assets\\ds_mp_assets\\char_streams\\npc\\pack_boy_cct\\pack_boy",
    "global_mp_assets\\global_mp_assets\\ds_mp_assets\\char_streams\\npc\\lurker_cct\\lurker",
    "global_mp_assets\\global_mp_assets\\ds_mp_assets\\char_streams\\npc\\necroplayer_spitter_cct\\necroplayer_spitter",
    "global_mp_assets\\global_mp_assets\\ds_mp_assets\\char_streams\\npc\\necroplayer_puker_cct\\necroplayer_puker",
};

// state
uint32_t poolManager = 0, poolHandle = 0, modelHandle = 0;
int poolStatus = -1, modelStatus = -1;
bool typeMenu = true;   // [Necro] TypeMenu: the MP12 necro type menu with all four types (needs the respawn manager, see EnsureRespawnManager)
// With a sixth type only the configured type's stream is loaded. A type picked in the menu that
// is not loaded is approved as the configured one this time and its stream asked for, so the
// next pick of it works (17.10.: the spitter, crash 39376). Only while TOC slots are left.
int menuWanted = -1;
volatile LONG menuStreamsAsked = 0, menuStreamsNoSlot = 0;
uint32_t extraModelHandle[4]{};
int extraModelStatus[4] = {-1, -1, -1, -1};
void ResetExtraModels() {
    for (int i = 0; i < 4; ++i) { extraModelHandle[i] = 0; extraModelStatus[i] = -1; }
    // The Leaper's two streams live and die with the same manager as the rest.
    leaperModelHandle = leaperRecordHandle = 0;
    leaperModelStatus = leaperRecordStatus = -1;
    leaperRequestedAt = 0;
    InterlockedExchange(&leaperRecordWanted, 0);
    InterlockedExchange(&leaperRecordCancel, 0);
}
bool poolLocked = false, modelLocked = false;
uint32_t settings = 0, necroEntity = 0, parkedPlayer = 0, necroProxy = 0, necroSubKey = 0;
// The human is not parked but removed, the way the multiplayer does it: a parked
// Player kept polling the controller next to the necro (fifth live test: Isaac
// walked and shot while the pack jumped). The level's Player template GUID is
// kept so the revert respawns Isaac from it, exactly like a checkpoint restart.
uint32_t playerGuid[4]{};
uint32_t playerSubKeyWord = 0;
bool playerGone = false;
// Where the necro last stood (refreshed every frame while it lives): the
// escape hatch from the ghost camera respawns Isaac there.
alignas(16) float lastNecroMatrix[16]{};
bool haveLastNecroMatrix = false;
uint32_t deadNecro = 0;   // the corpse the native respawn flow still owns; never adopted, never deleted here
uint32_t trackedIsaac = 0;   // the Isaac a transform removed (deletion tracer)
DWORD collisionPendingAt = 0;   // vent respawn: the player collision mode waits for the exit animation
DWORD ventRequestAt = 0;        // a vent accepted, the spawn not adopted yet
DWORD ghostProxySince = 0;      // the current ghost proxy appeared (death or vent re-entry)
DWORD necroDiedAt = 0;          // the necro died; its next vent waits for the respawn pace
bool necroByChoice = false;     // turned into a necro with the key or the join; a restart or level change brings him back
volatile LONG watchRejoins = 0;
void JoinAsNecro();             // below
uint32_t LocalGhost();          // below
void EscapeGhost(const float* at = nullptr);   // below
bool DirectSpawnPending();      // below, the direct spawn
void LifeExpect(int state, const char* why);   // below, the life state machine
void WriteLife(FILE* log);                     // below
enum : int { kLifeOff = 0, kLifeHuman, kLifeLiving, kLifeDying, kLifeGhost, kLifeWatching, kLifeSpawning, kLifeBroken, kLifeCount };
void WriteDirectSpawn(FILE* log);   // below
LONG exitDoneBaseline = 0;
// NecroRespawnProxySM::RefreshConsideredVentList asks VentMarker::FindNearestVents
// within MPLevelSettings' vent selection cast distance (attributes+0xD8, 70 m in
// the MP maps). Campaign rooms are larger and emptier: the radius is written
// from the INI ([Necro] VentSearchRadius, metres). Only loaded vents exist as
// entities, so a far vent is still in a loaded room.
constexpr uint32_t kSettingsVentRadius = 0xD8;
float ventSearchRadius = 150.0f;
uint32_t radiusAppliedTo = 0;
constexpr uint32_t kRespawnProxyVtableRva = 0x17596E0, kRespawnProxyBasePlayer = 0x180;
// Door collision hand-over (defined below Transform, used inside it).
uint32_t FindCmc(uint32_t entity);
void ApplyPlayerCollision(uint32_t entity);
extern uint32_t isaacCollisionMode;
constexpr uint32_t kCmcModeOffset = 0x224;
volatile LONG transforms = 0, reverts = 0, settingsSpawned = 0, lastError = 0;
// Where a caught fault happened: the step Transform was in and the faulting address.
volatile LONG stage = 0;
uint32_t faultAddress = 0;
DWORD FaultFilter(EXCEPTION_POINTERS* pointers) {
    faultAddress = pointers && pointers->ExceptionRecord
        ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointers->ExceptionRecord->ExceptionAddress)) : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}
// Written on the game thread, printed by the diagnostics thread: a small ring
// under a lock, so a note is neither torn nor overwritten before it is printed.
constexpr unsigned kNoteSlots = 32;   // a power of two (the indices wrap); 8 dropped 72 notes in one second on 17.10.
char notes[kNoteSlots][256]{};
unsigned noteHead = 0, noteTail = 0;
volatile LONG notesDropped = 0;
SRWLOCK noteLock = SRWLOCK_INIT;

void Note(const char* format, ...) {
    char text[256];
    va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    vlog::MirrorIfMatch("necro", text);
    AcquireSRWLockExclusive(&noteLock);
    if (noteHead - noteTail >= kNoteSlots) { ++noteTail; InterlockedIncrement(&notesDropped); }
    std::memcpy(notes[noteHead % kNoteSlots], text, sizeof(text));
    ++noteHead;
    ReleaseSRWLockExclusive(&noteLock);
}

// SpawnInfo is a thin wrapper over the InstanceData; 0x40 bytes is generous
// (the native Spawn keeps it in 0x20 bytes of stack).
struct SpawnInfo { uint32_t words[16]; };

bool GetSpawnInfo(uint32_t simManager, SpawnInfo& info, const uint32_t* guid) {
    std::memset(&info, 0, sizeof(info));
    using Get = void (__thiscall*)(void*, SpawnInfo*, const uint32_t*, int);
    reinterpret_cast<Get>(base + kGetSpawnInfo)(reinterpret_cast<void*>(simManager), &info, guid, 0);
    return info.words[0] != 0;   // InstanceData pointer; zero = InitializeInvalid
}

uint32_t SpawnByInfo(uint32_t simManager, SpawnInfo& info) {
    uint32_t edm = 0;
    Read(base + kEntityDataManager, &edm, 4);
    unsigned char off = 0, was = 1;
    if (edm) { Read(edm + kSpawnFilterByte, &was, 1); WriteBytes(edm + kSpawnFilterByte, &off, 1); }   // put back as found
    using Spawn = uint32_t (__thiscall*)(void*, SpawnInfo*, uint32_t);
    allowMp = true;   // the plugin's own request passes the creator gate
    const uint32_t entity = reinterpret_cast<Spawn>(base + kSpawnEntity)(reinterpret_cast<void*>(simManager), &info, 0x2000);
    allowMp = false;
    if (edm) WriteBytes(edm + kSpawnFilterByte, &was, 1);
    return entity;
}

// 1. the stream. Requested once per level; the level teardown drops it with
// everything else, so the manager pointer changing resets the request.
void WatchLevelExit();
void ForgetExitCameras();
extern bool releasedForExit;
// The pool on demand ([Necro] LoadPool=2). With the four pool streams resident
// from level start every debug-menu checkpoint load (24.09., dumps 18692,
// 28492, 5200, 28484) reloaded global_assets inside the load window: the load
// waited on the stopped render thread, and its entities outlived the world's
// close. Destroyed early they left a dangling resource user (linegun stream,
// 0x00790622); destroyed late, the persistent-item manager was gone
// (0x00DB43FA). Without the pool none of it happens (14:11 run). So the pool
// is loaded when somebody asks for a necro: F11 requests it here and tells
// every partner ('NPOL' kind 1), each machine answers once its copy is in
// (kind 2), and the transformation waits for all answers or kPoolWaitMs.
// The demand ends with the level.
int poolMode = 1;   // 0 never, 1 at every level start, 2 on demand
bool poolDemanded = false, transformWhenReady = false, readySent = false;
// The MP suit in a campaign arena (mpsuit, [Loadout] ArenaSuit): only the pool root, which holds
// its Uniform record and skin streams; no character streams, no MP UI, no respawn manager. Each
// machine asks for its own copy; the demand ends with the level like the necro's.
bool suitDemand = false;
DWORD suitDemandAt = 0;
volatile LONG suitDemands = 0;
void DemandForSuit() {
    if (suitDemand) return;
    suitDemand = true;
    suitDemandAt = GetTickCount();
    InterlockedIncrement(&suitDemands);
    Note("pool asked for the MP suit (pool mode %d, demanded by a necro %d)", poolMode, poolDemanded ? 1 : 0);
}
int PoolStatus() { return poolStatus; }
DWORD demandedAt = 0;
uint32_t readyPeers = 0;
constexpr DWORD kPoolWaitMs = 30000;
volatile LONG poolDemands = 0, poolSent = 0, poolReceived = 0, poolWaitTimeouts = 0;
void SendPool(uint32_t kind) {
    PoolPacket p{kPoolMagic, kPoolVersion, peerReadiness.token, kind};
    if (dataSock != INVALID_SOCKET && peerTable.Count() && SendToPeers(&p, sizeof(p))) InterlockedIncrement(&poolSent);
}
void Demand(bool announce) {
    if (!poolDemanded) {
        poolDemanded = true;
        demandedAt = GetTickCount();
        readySent = false;
        InterlockedIncrement(&poolDemands);
    }
    if (announce) SendPool(1);
}
// The sixth type's two streams (the Leaper or whatever SixthCharStream names).
void PollLeaper(uint32_t manager) {
    using Status = int (__thiscall*)(void*, uint32_t);
    // The Leaper is a campaign creature, so its character stream is only
    // resident in rooms that have leapers in them. A sixth type built out of
    // it has to ask for the stream the same way the four ask for theirs, or
    // the model and its animations are simply not there when it climbs out.
    // Tied to the sixth type, not to the runtime patch: with the record built
    // as a proper stream the patch is off, but the Leaper's model and banks
    // still have to be resident.
    using Load = uint32_t (__thiscall*)(void*, const char*, float, unsigned, const void*, const char*);
    using Lock = bool (__thiscall*)(void*, uint32_t);
    using Unlock = bool (__thiscall*)(void*, uint32_t);
    // Both Leaper streams are asked for in the SAME poll, early in the load.
    // Until 22.09. the record waited for the character stream to reach 3 and
    // was requested one poll later, and then stayed QUEUED for good, locked,
    // with the loading screen waiting on it (live: status 0, flags 0x1E0 =
    // EXPLICIT_LOAD|PARENTS_REFCOUNTED|LOCKED|IS_VISIBLE, never HAS_DISPATCHED,
    // request id still at +0x28). Every stream the loader did dispatch had been
    // asked for while it still had work queued; the randomizer asks for its
    // loose streams the same way. The record does not need the model to be
    // there to load, only to be spawned, so there is nothing to wait for.
    if (!leaperModelHandle && leaperModelStatus != -3) {
        leaperModelHandle = reinterpret_cast<Load>(base + kStreamLoad)(reinterpret_cast<void*>(manager), kLeaperStreamName, 0.0f, 2, nullptr, nullptr);
        if (!leaperModelHandle) { leaperModelStatus = -2; Note("leaper stream %s: Load gave no handle", kLeaperStreamName); return; }
        reinterpret_cast<Lock>(base + kStreamLock)(reinterpret_cast<void*>(manager), leaperModelHandle);
        leaperRequestedAt = GetTickCount();
    }
    // An empty LeaperRecordStream asks for no record stream at all. Every
    // session since the record was requested as a loose .str hung in the
    // loading screen, even after the request was withdrawn (22.09., three
    // tests), so this is the switch that shows whether that request is the
    // cause, with the character stream still loading as before.
    if (!leaperRecordStream[0]) leaperRecordStatus = -4;
    if (!leaperRecordHandle && leaperRecordStatus != -3 && leaperRecordStatus != -2 && leaperRecordStatus != -4) {
        if (!leaperRequestedAt) leaperRequestedAt = GetTickCount();
        if (leaperUpdateHook) {
            InterlockedExchange(&leaperRecordWanted, 1);   // served inside the next stream update
        } else {
            leaperRecordHandle = reinterpret_cast<Load>(base + kStreamLoad)(reinterpret_cast<void*>(manager), leaperRecordStream, 0.0f, 2, nullptr, nullptr);
            if (!leaperRecordHandle) { leaperRecordStatus = -2; Note("leaper record stream %s: Load gave no handle", leaperRecordStream); }
            else reinterpret_cast<Lock>(base + kStreamLock)(reinterpret_cast<void*>(manager), leaperRecordHandle);
            return;
        }
    }
    // Polled on every frame, both of them, for as long as either is short of 3.
    // (The first build returned as soon as the character stream reached 3, so
    // the record's status was never read again and its deadline never came.)
    if (leaperModelHandle && leaperModelStatus != 3 && leaperModelStatus != -3) {
        const int state = reinterpret_cast<Status>(base + kStreamStatus)(reinterpret_cast<void*>(manager), leaperModelHandle);
        if (state != leaperModelStatus) { leaperModelStatus = state; if (state == 3) Note("leaper stream loaded (status 3)"); }
    }
    if (leaperRecordHandle && leaperRecordStatus != 3 && leaperRecordStatus != -3) {
        const int record = reinterpret_cast<Status>(base + kStreamStatus)(reinterpret_cast<void*>(manager), leaperRecordHandle);
        if (record != leaperRecordStatus) { leaperRecordStatus = record; if (record == 3) Note("leaper record stream loaded (status 3)"); }
    }
    // A stream request has no deadline of its own, and a LOCKED request that
    // never dispatches holds the loading screen forever. So the deadline is
    // measured in time, not in polls, and giving up means giving the locks
    // back: the sixth type is then unavailable and the level simply goes on.
    const bool modelPending = leaperModelHandle && leaperModelStatus != 3 && leaperModelStatus != -3;
    const bool recordPending = leaperRecordStatus != 3 && leaperRecordStatus != -3 && leaperRecordStatus != -2 &&
                               (leaperRecordHandle || InterlockedCompareExchange(&leaperRecordWanted, 0, 0));
    if ((modelPending || recordPending) && leaperRequestedAt && GetTickCount() - leaperRequestedAt > kLeaperGiveUpMs) {
        using Unload = void (__thiscall*)(void*, uint32_t);
        if (recordPending) {
            // Given back AND withdrawn: the lock alone kept the level waiting.
            if (leaperUpdateHook) {
                InterlockedExchange(&leaperRecordCancel, 1);   // done inside the next stream update, where it was asked for
            } else if (leaperRecordHandle) {
                reinterpret_cast<Unlock>(base + kStreamUnlock)(reinterpret_cast<void*>(manager), leaperRecordHandle);
                reinterpret_cast<Unload>(base + kStreamUnload)(reinterpret_cast<void*>(manager), leaperRecordHandle);
            }
            Note("leaper record stream gave up after %u ms (status %d, handle %08X): unlocked and unloaded", kLeaperGiveUpMs, leaperRecordStatus, leaperRecordHandle);
            if (!leaperUpdateHook) leaperRecordHandle = 0;   // released above; the hook path clears it itself
            leaperRecordStatus = -3;
        }
        if (modelPending) {
            reinterpret_cast<Unlock>(base + kStreamUnlock)(reinterpret_cast<void*>(manager), leaperModelHandle);
            reinterpret_cast<Unload>(base + kStreamUnload)(reinterpret_cast<void*>(manager), leaperModelHandle);
            Note("leaper stream gave up after %u ms (status %d): unlocked and unloaded", kLeaperGiveUpMs, leaperModelStatus);
            leaperModelHandle = 0;   // released: DropOurStreams must not unlock it a second time
            leaperModelStatus = -3;
        }
    }
}
bool LeaperSettled() {
    const bool model = leaperModelStatus == 3 || leaperModelStatus == -3 || leaperModelStatus == -2;
    const bool record = leaperRecordStatus == 3 || leaperRecordStatus <= -2;
    return model && record;
}
// The other MP necro character streams: all four while a necro is in the game (user, 17.10.:
// with a sixth type only the configured one was loaded, so a type picked in the menu was
// refused the first time), else only the one picked in the menu. One at a time, each only
// while the stream manager keeps kExtraTocReserve of its 32 TOC slots free for the
// weapon and suit streams; one that does not fit stays on demand (TypeSpawnable keeps
// the menu from approving it, crash 52780).
constexpr uint32_t kExtraTocLimit = 28;
volatile LONG extraStreamsAsked = 0, extraStreamsNoSlot = 0;
void PollExtraModels(uint32_t manager, int which, bool sixthConfigured, bool allFour) {
    using Status = int (__thiscall*)(void*, uint32_t);
    using Load = uint32_t (__thiscall*)(void*, const char*, float, unsigned, const void*, const char*);
    using Lock = bool (__thiscall*)(void*, uint32_t);
    for (int other = 0; other < 4; ++other) {
        if (other == which || extraModelStatus[other] == 3 || extraModelStatus[other] == -2) continue;
        if (!allFour && other != menuWanted) continue;
        if (!extraModelHandle[other]) {
            uint32_t tocs = 0;
            if (!Read(manager + 0x130, &tocs, 4) || tocs >= kExtraTocLimit) {
                if (InterlockedIncrement(&extraStreamsNoSlot) <= 4)
                    Note("model stream %s: no TOC slot to spare (%u of 32, %u kept free), stays on demand", kTypeNames[other], tocs, 32u - kExtraTocLimit);
                if (other == menuWanted) menuWanted = -1;
                return;
            }
            extraModelHandle[other] = reinterpret_cast<Load>(base + kStreamLoad)(reinterpret_cast<void*>(manager), kModelStreams[other], 0.0f, 2, nullptr, nullptr);
            if (!extraModelHandle[other]) { extraModelStatus[other] = -2; Note("model stream %s: Load gave no handle", kTypeNames[other]); return; }
            reinterpret_cast<Lock>(base + kStreamLock)(reinterpret_cast<void*>(manager), extraModelHandle[other]);
            InterlockedIncrement(other == menuWanted && !allFour ? &menuStreamsAsked : &extraStreamsAsked);
            Note("model stream %s asked for: %s (%u of 32 TOC slots before)", kTypeNames[other],
                 allFour ? "a necro is in the game, all four are loaded" : "picked in the menu", tocs);
            return;
        }
        const int extra = reinterpret_cast<Status>(base + kStreamStatus)(reinterpret_cast<void*>(manager), extraModelHandle[other]);
        if (extra != extraModelStatus[other]) { extraModelStatus[other] = extra; if (extra == 3) Note("model stream %s loaded (status 3)", kTypeNames[other]); }
        return;
    }
    (void)sixthConfigured;
}

void PollPool() {
    if (!wantPool) return;
    WatchLevelExit();
    if (releasedForExit) return;
    uint32_t manager = 0;
    if (!Read(base + kStreamManager, &manager, 4) || !manager) {
        poolManager = 0; poolHandle = 0; poolStatus = -1; poolLocked = false;
        modelHandle = 0; modelStatus = -1; modelLocked = false; ResetExtraModels();
        return;
    }
    if (manager != poolManager) {
        poolManager = manager; poolHandle = 0; poolStatus = -1; poolLocked = false;
        modelHandle = 0; modelStatus = -1; modelLocked = false; ResetExtraModels();
    }
    if (!inLevel) return;
    if (poolMode == 2 && !poolDemanded && !suitDemand) return;
    if (!poolHandle) {
        uint32_t tocs = 0;
        // inLevel above says the level is up (the Outbreak maps have only 5 or 6 TOCs); 31 = no TOC slot left
        if (!Read(manager + 0x130, &tocs, 4) || tocs >= 31) return;
        using Load = uint32_t (__thiscall*)(void*, const char*, float, unsigned, const void*, const char*);
        poolHandle = reinterpret_cast<Load>(base + kStreamLoad)(reinterpret_cast<void*>(manager), kPoolName, 0.0f, 2, nullptr, nullptr);
        if (!poolHandle) { InterlockedExchange(&lastError, 1); return; }
        using Lock = bool (__thiscall*)(void*, uint32_t);
        poolLocked = reinterpret_cast<Lock>(base + kStreamLock)(reinterpret_cast<void*>(manager), poolHandle);
        Note("pool requested handle=%08X locked=%d tocs=%u", poolHandle, poolLocked ? 1 : 0, tocs);
        return;
    }
    using Status = int (__thiscall*)(void*, uint32_t);
    const int status = reinterpret_cast<Status>(base + kStreamStatus)(reinterpret_cast<void*>(manager), poolHandle);
    if (status != poolStatus) {
        poolStatus = status;
        if (status == 3) Note("pool loaded (status 3)%s", suitDemand && !poolDemanded ? ", for the MP suit" : "");
        if (status == 3 && suitDemand) Note("pool for the MP suit in after %lu ms", static_cast<unsigned long>(GetTickCount() - suitDemandAt));
    }
    if (poolStatus != 3) return;
    // the MP suit needs the root only
    if (poolMode == 2 && suitDemand && !poolDemanded && !transformWhenReady) return;
    // The root is in: now the chosen type's character stream, same calls.
    const int which = type < 0 || type > 3 ? 0 : type;
    if (!modelHandle) {
        using Load = uint32_t (__thiscall*)(void*, const char*, float, unsigned, const void*, const char*);
        modelHandle = reinterpret_cast<Load>(base + kStreamLoad)(reinterpret_cast<void*>(manager), kModelStreams[which], 0.0f, 2, nullptr, nullptr);
        if (!modelHandle) { InterlockedExchange(&lastError, 5); Note("model stream %s: Load gave no handle", kTypeNames[which]); return; }
        using Lock = bool (__thiscall*)(void*, uint32_t);
        modelLocked = reinterpret_cast<Lock>(base + kStreamLock)(reinterpret_cast<void*>(manager), modelHandle);
        Note("model stream %s requested handle=%08X locked=%d", kTypeNames[which], modelHandle, modelLocked ? 1 : 0);
        return;
    }
    const int model = reinterpret_cast<Status>(base + kStreamStatus)(reinterpret_cast<void*>(manager), modelHandle);
    if (model != modelStatus) { modelStatus = model; if (model == 3) Note("model stream %s loaded (status 3)", kTypeNames[which]); }
    if (modelStatus != 3) return;
    const bool sixthConfigured = sixth::enabled && sixth::Configured();
    // The sixth type first: its record once stayed queued for good when it was asked for late.
    if (sixthConfigured) {
        PollLeaper(manager);
        if (!LeaperSettled()) return;
    }
    if (!typeMenu) return;
    const bool necroInGame = poolDemanded || necroEntity || roles::AnyNecroPeer(GetTickCount());
    const bool allFour = !sixthConfigured || necroInGame;
    if (allFour || menuWanted >= 0) PollExtraModels(manager, which, sixthConfigured, allFour);
}

bool PoolReady() { return !wantPool || (poolStatus == 3 && modelStatus == 3); }
// Everything PollPool asks for is in, or given up on (the sixth type's two).
bool PoolDone() {
    if (!PoolReady()) return false;
    if (!(sixth::enabled && sixth::Configured())) return true;
    const bool model = leaperModelStatus == 3 || leaperModelStatus == -3 || leaperModelStatus == -2;
    const bool record = leaperRecordStatus == 3 || leaperRecordStatus <= -2;
    return model && record;
}
bool PartnersReady() {
    unsigned count = 0;
    // Only seats still taken count: a partner who left keeps no ready bit for somebody else.
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s)
        if ((readyPeers & (1u << s)) && peerTable.peers[s].used) ++count;
    return count >= peerTable.Count();
}
void OnPoolPacket(const PoolPacket& p, unsigned slot) {
    InterlockedIncrement(&poolReceived);
    if (!enabled || poolMode != 2) return;
    if (p.kind == 1) {
        Demand(false);
        if (PoolDone()) SendPool(2);   // already in here: answer again for the new asker
    } else if (p.kind == 2 && slot < 32) {
        readyPeers |= 1u << slot;
    }
}
void Transform(const float* at = nullptr);   // below; at: the necro's matrix instead of Isaac's
// From Poll: tell the partners once the local copy is in, and transform once theirs are.
void PollDemand() {
    if (poolMode != 2 || !poolDemanded || !inLevel) return;
    const bool done = PoolDone();
    if (done && !readySent) { readySent = true; SendPool(2); Note("necro pool loaded on demand, partners told"); }
    if (!transformWhenReady || !done) return;
    const bool all = PartnersReady();
    if (!all && GetTickCount() - demandedAt < kPoolWaitMs) return;
    transformWhenReady = false;
    if (!all) { InterlockedIncrement(&poolWaitTimeouts); Note("transform: not every partner reported the necro pool within %u ms, going ahead", kPoolWaitMs); }
    __try {
        Transform();
        // 17.10.: this way left by_choice=0, so a restart left him a human for good
        if (necroEntity || playerGone) necroByChoice = true;
    } __except (FaultFilter(GetExceptionInformation())) {
        InterlockedExchange(&lastError, 99);
        allowMp = false;
    }
}

// The world closes (GameMainLoop::CloseImpl, from runtime_coop_persistguard):
// the plugin's locked streams must not outlive it. A stream's entities normally
// go when the stream unloads (SimManager::DestroyEntities, 0x00776B10, thiscall
// (hStream), ret 4, with the handle StreamManager::Load returned); these are locked,
// so they stayed until the shutdown's DeleteAllEntities, which runs after the
// persistent-item manager and [0x0201C0A4] are gone (dumps 10436, 1404,
// 15288: 185 entities left on stream 0xB alone). So here, while everything
// still exists, each of these streams loses its entities, its lock and its
// request, and the next level asks for them again.
constexpr uint32_t kDestroyStreamEntities = 0x376B10;
volatile LONG worldCloses = 0, worldCloseStreams = 0, worldCloseFaults = 0;
int WorldCloseFilter(unsigned) { InterlockedIncrement(&worldCloseFaults); return EXCEPTION_EXECUTE_HANDLER; }
bool DropStream(uint32_t manager, uint32_t sim, uint32_t handle) {
    using Destroy = void (__thiscall*)(void*, uint32_t);
    using Unlock = bool (__thiscall*)(void*, uint32_t);
    using Unload = void (__thiscall*)(void*, uint32_t);
    __try {
        if (sim) reinterpret_cast<Destroy>(base + kDestroyStreamEntities)(reinterpret_cast<void*>(sim), handle);
        reinterpret_cast<Unlock>(base + kStreamUnlock)(reinterpret_cast<void*>(manager), handle);
        reinterpret_cast<Unload>(base + kStreamUnload)(reinterpret_cast<void*>(manager), handle);
        return true;
    } __except (WorldCloseFilter(GetExceptionCode())) {
        return false;
    }
}
// The plugin's streams hang below global_assets (the sixth type's character stream is
// global_assets\...\ubermorph_cct, its record names global_assets,
// global_assets_global and text_assets_global as parents), and a child keeps
// its parents loaded. Dump 15288: the leftover entities were global_assets'
// (185, stream 0xB, lock count 2) and global_assets_global's (14, stream 0xA);
// in the retail game both leave with the level. So the plugin's streams are
// given back as soon as a level change shows (WatchLevelExit, from PollPool),
// and at the world's close whatever is still left on its own streams
// (randomizer\, global_mp_assets) is destroyed while its managers exist.
// global_assets leftovers are destroyed only with [Necro] DestroyGlobalLeftovers=1
// (the default).
bool releasedForExit = false, wasInLevel = false;
volatile LONG exitReleases = 0, exitStreamsDropped = 0, leftoverStreams = 0, leftoverEntities = 0;
char leftoverLog[256] = "";
unsigned DropOurStreams(bool destroyEntities) {
    uint32_t manager = 0, sim = 0;
    const bool same = Read(base + kStreamManager, &manager, 4) && manager && manager == poolManager;
    if (destroyEntities) Read(base + kSimManager, &sim, 4);
    uint32_t* handles[] = {&leaperRecordHandle, &leaperModelHandle, &extraModelHandle[0], &extraModelHandle[1],
                           &extraModelHandle[2], &extraModelHandle[3], &modelHandle, &poolHandle};
    unsigned dropped = 0;
    for (uint32_t* handle : handles) {
        if (*handle && same && DropStream(manager, sim, *handle)) ++dropped;
        *handle = 0;
    }
    leaper::Forget();   // the patched attribute block goes with these streams: never written back into freed memory
    InterlockedExchange(&leaperRecordWanted, 0);
    InterlockedExchange(&leaperRecordCancel, 0);
    poolStatus = modelStatus = -1;
    poolLocked = modelLocked = false;
    for (int& status : extraModelStatus) status = -1;
    leaperModelStatus = leaperRecordStatus = -1;
    leaperRequestedAt = 0;
    return dropped;
}
// Every frame, from PollPool: the GameMainLoop's pending level change
// (+0x209, set by RequestLevelChange 0x0040F870, cleared at 0x0040EE47) or
// the level's own streams dropping away (inLevel). Nothing is asked for again
// until the next level is up. (RequestLevelChange itself is not hooked: the
// debug menu compares its first 8 bytes before every checkpoint load.)
// Only edges release: the pending byte can stay set after the level streams
// are gone, and the reset below would otherwise release again every frame.
bool wasChanging = false;
void WatchLevelExit() {
    const uint32_t loop = level::GameMainLoop();
    unsigned char pending = 0;
    const bool changing = loop && Read(loop + 0x209, &pending, 1) && pending;
    if (!releasedForExit) {
        if ((changing && !wasChanging) || (wasInLevel && !inLevel)) {
            releasedForExit = true;
            ForgetExitCameras();
            poolDemanded = transformWhenReady = readySent = suitDemand = false;
            readyPeers = 0;
            const unsigned dropped = DropOurStreams(false);
            InterlockedIncrement(&exitReleases);
            InterlockedExchangeAdd(&exitStreamsDropped, static_cast<LONG>(dropped));
            Note("level exit (%s): %u locked stream(s) given back and unloaded", changing ? "change pending" : "level streams gone", dropped);
        }
    }
    if (!inLevel) releasedForExit = false;
    wasInLevel = inLevel;
    wasChanging = changing;
}
// The stream a SimManager handle belongs to (StreamManager's handle table,
// +0x5C buckets / +0x60 count, nodes {handle, stream, next}, the walk of
// StreamFromHandle 0x0040DA50) and its name (stream+0x18 descriptor -> identity
// -> +4 name).
bool StreamName(uint32_t manager, uint32_t handle, char* out, size_t outSize) {
    uint32_t table = 0, buckets = 0, node = 0;
    out[0] = 0;
    if (!Read(manager + 0x5C, &table, 4) || !Read(manager + 0x60, &buckets, 4) || !table || !buckets ||
        !Read(table + 4 * (handle % buckets), &node, 4)) return false;
    for (unsigned hops = 0; node && hops < 64; ++hops) {
        uint32_t nodeHandle = 0, stream = 0, desc = 0, identity = 0, name = 0;
        if (!Read(node, &nodeHandle, 4)) return false;
        if (nodeHandle == handle) {
            if (!Read(node + 4, &stream, 4) || !stream || !Read(stream + 0x18, &desc, 4) || !desc ||
                !Read(desc, &identity, 4) || !identity || !Read(identity + 4, &name, 4) || !name) return false;
            char buffer[96]{};
            if (!Read(name, buffer, sizeof(buffer) - 1)) return false;
            std::snprintf(out, outSize, "%s", buffer);
            return true;
        }
        if (!Read(node + 8, &node, 4)) return false;
    }
    return false;
}
bool destroyGlobalLeftovers = true;   // [Necro] DestroyGlobalLeftovers
void DestroyLeftovers() {
    uint32_t sim = 0, manager = 0, node = 0;
    if (!Read(base + kSimManager, &sim, 4) || !sim || !Read(base + kStreamManager, &manager, 4) || !manager ||
        !Read(sim + 0x60, &node, 4)) return;
    // SimManager's stream-scoped lists (+0x60): {next, prev, handle +8, first entity +0xC, entity link +0x1C}.
    uint32_t handles[32]{}, counts[32]{};
    unsigned n = 0;
    for (unsigned hops = 0; node && hops < 256 && n < 32; ++hops) {
        uint32_t handle = 0, entity = 0, count = 0;
        Read(node + 8, &handle, 4);
        Read(node + 0xC, &entity, 4);
        for (; entity && count < 100000; ++count) if (!Read(entity + 0x1C, &entity, 4)) break;
        if (count) { handles[n] = handle; counts[n] = count; ++n; }
        if (!Read(node, &node, 4)) break;
    }
    size_t used = 0;
    leftoverLog[0] = 0;
    for (unsigned i = 0; i < n; ++i) {
        char name[96] = "";
        StreamName(manager, handles[i], name, sizeof(name));
        // global_assets is the engine's to take down: its unload first sends
        // iMsgStreamUnloading to the users of its children's resources, then
        // destroys the entities. Destroying them here reversed that; 24.09.
        // (dumps 18692, 28492, 5200) the linegun weapon stream's release then
        // called a destroyed user (_purecall / freed vtable at 0x00790622)
        // after every debug-menu checkpoint load with the necro pool resident.
        const bool global = _strnicmp(name, "global_assets", 13) == 0;
        const bool ours = (global && destroyGlobalLeftovers) || _strnicmp(name, "global_mp_assets", 16) == 0 ||
                          _strnicmp(name, "randomizer\\", 11) == 0;
        if (used < sizeof(leftoverLog) - 40)
            used += static_cast<size_t>(std::snprintf(leftoverLog + used, sizeof(leftoverLog) - used, " %s:%u%s", name[0] ? name : "?", counts[i], ours ? "" : "(kept)"));
        if (!ours) continue;
        using Destroy = void (__thiscall*)(void*, uint32_t);
        __try {
            reinterpret_cast<Destroy>(base + kDestroyStreamEntities)(reinterpret_cast<void*>(sim), handles[i]);
            InterlockedIncrement(&leftoverStreams);
            InterlockedExchangeAdd(&leftoverEntities, static_cast<LONG>(counts[i]));
        } __except (WorldCloseFilter(GetExceptionCode())) {
        }
    }
}
void OnWorldClose() {
    InterlockedIncrement(&worldCloses);
    const unsigned dropped = DropOurStreams(true);
    InterlockedExchangeAdd(&worldCloseStreams, static_cast<LONG>(dropped));
    DestroyLeftovers();
}

// 2. MPLevelSettings. The stream's SimGroup may have spawned it (spawn_flags=1);
// if not, spawn the authored one. Its own Singleton constructor fills the slot.
bool EnsureSettings(uint32_t simManager) {
    Read(base + kMPLevelSettings, &settings, 4);
    if (settings) return true;
    if (!spawnSettings) { InterlockedExchange(&lastError, 2); return false; }
    SpawnInfo info{};
    if (!GetSpawnInfo(simManager, info, kSettingsGuid)) { InterlockedExchange(&lastError, 3); return false; }
    const uint32_t entity = SpawnByInfo(simManager, info);
    using Finalize = void (__stdcall*)(uint32_t, int);
    if (entity) reinterpret_cast<Finalize>(base + kFinalize)(entity, 0);
    Read(base + kMPLevelSettings, &settings, 4);
    if (settings) InterlockedIncrement(&settingsSpawned);
    else InterlockedExchange(&lastError, 4);
    return settings != 0;
}

// The template GUID: MPLevelSettings' own list first, the shipped values as fallback.
const uint32_t* TemplateGuid(int which, uint32_t* scratch) {
    // A sixth type owns no authored slot. The index would land on the field
    // after the five, which belongs to the respawn proxy, so the record is
    // taken straight from the INI instead: the same GUID the spawn lends.
    if (sixth::enabled && which >= 0 && static_cast<uint32_t>(which) == sixth::slot && sixth::Configured())
        return sixth::guid;
    if (which < 0 || which > 3) which = 0;   // the shipped fallback holds four
    uint32_t attributes = 0;
    if (settings && Read(settings + kSettingsAttributes, &attributes, 4) && attributes &&
        Read(attributes + kSettingsNecroGuids + which * 16, scratch, 16) &&
        (scratch[0] | scratch[1] | scratch[2] | scratch[3])) return scratch;
    return kTemplateGuid[which];
}

void RegisterNecroWithSession(uint32_t entity) {
    necroProxy = necroSubKey = 0;
    if (!registerNet || !session) return;
    const uint32_t reflectable = entity + kReflectable;
    uint32_t vtable = 0, setSubKey = 0, netInit = 0, existing = 0;
    if (!Read(reflectable, &vtable, 4) || !Read(vtable + 8, &setSubKey, 4) || !Read(vtable + 0x30, &netInit, 4) ||
        setSubKey != base + 0x3D1D10 || netInit != base + 0x3D1DC0) { InterlockedExchange(&lastError, 10); return; }
    if (Read(reflectable + 0xC, &existing, 4) && existing) { necroProxy = existing; return; }
    if (!AllocatePlayerSubKey(session, machineIndex, necroSubKey)) { InterlockedExchange(&lastError, 11); return; }
    using SetSubKey = void (__thiscall*)(void*, uint32_t);
    using NetInit = void (__thiscall*)(void*);
    using AddToSession = void (__thiscall*)(void*, uint32_t);
    reinterpret_cast<SetSubKey>(setSubKey)(reinterpret_cast<void*>(reflectable), necroSubKey);
    reinterpret_cast<NetInit>(netInit)(reinterpret_cast<void*>(reflectable));
    reinterpret_cast<AddToSession>(base + kAddToSession)(reinterpret_cast<void*>(reflectable), 0);
    Read(reflectable + 0xC, &necroProxy, 4);
    if (!necroProxy) InterlockedExchange(&lastError, 12);
}

// 3.-5. the transformation itself.
void Transform(const float* at) {
    stage = 1;
    const uint32_t player = LocalCampaignPlayer();
    if (!player) { InterlockedExchange(&lastError, 20); Note("transform: no local player in slot 0"); return; }
    uint32_t simManager = 0, playerManager = 0;
    if (!Read(base + kSimManager, &simManager, 4) || !simManager ||
        !Read(base + kPlayerManager, &playerManager, 4) || !playerManager) { InterlockedExchange(&lastError, 21); return; }
    if (!PoolReady()) { InterlockedExchange(&lastError, 22); Note("transform: streams not loaded yet (root %d, model %d)", poolStatus, modelStatus); return; }
    stage = 2;
    if (!EnsureSettings(simManager)) { Note("transform: no MPLevelSettings (error %ld)", InterlockedCompareExchange(&lastError, 0, 0)); return; }

    stage = 3;
    // The sixth type is allowed through here too, or pressing the key would
    // quietly turn into a Pack instead.
    const bool sixthWanted = sixth::enabled && type >= 0 && static_cast<uint32_t>(type) == sixth::slot && sixth::Configured();
    const int which = sixthWanted ? static_cast<int>(sixth::slot) : (type < 0 || type > 3 ? 0 : type);
    uint32_t scratch[4]{};
    const uint32_t* guid = TemplateGuid(which, scratch);
    SpawnInfo info{};
    if (!GetSpawnInfo(simManager, info, guid)) {
        InterlockedExchange(&lastError, 23);
        Note("transform: template %s %08X-%08X-%08X-%08X has no instance data (stream not registered?)", TypeName(which), guid[0], guid[1], guid[2], guid[3]);
        return;
    }
    // Where the player stands: the entity matrix (right/up/forward/position rows).
    stage = 4;
    // Entity::SetTransform (0x13DB110) copies the four rows with movaps, so the
    // matrix must sit on a 16-byte boundary (third live test: C0000005 at
    // 0x013DB119 on the first movaps from a 4-byte-aligned stack array).
    alignas(16) float matrix[16]{};
    if (at) std::memcpy(matrix, at, sizeof(matrix));
    else if (!Read(player + kEntityMatrix, matrix, sizeof(matrix))) { InterlockedExchange(&lastError, 24); return; }
    using SetTransform = void (__thiscall*)(SpawnInfo*, const float*);
    reinterpret_cast<SetTransform>(base + kSetTransform)(&info, matrix);
    // SpawnInfo (0x24 bytes, ctor 0x84A530): +0 InstanceData, +4 matrix POINTER
    // (SetTransform stores the address; the float[16] above must outlive the
    // spawn), +8 the word NecroPlayer::Spawn overwrites with its second argument,
    // +0xC pointer to the entity guid. The second live test wrote the subkey to
    // +4 and SpawnEntity read it as the matrix (exception C0000005, caught).
    uint32_t subKeyWord = 0;
    Read(player + kEntitySubKeyWord, &subKeyWord, 4);
    info.words[2] = subKeyWord;

    stage = 5;
    const uint32_t entity = SpawnByInfo(simManager, info);
    stage = 6;
    uint32_t vtable = 0;
    if (!entity || !Read(entity, &vtable, 4) || vtable != base + kNecroVtable) {
        InterlockedExchange(&lastError, 25);
        Note("transform: SpawnEntity gave %08X (vtable %08X, wanted %08X)", entity, vtable, static_cast<unsigned>(base + kNecroVtable));
        return;
    }

    // Hand slot 0 over: the Player leaves the manager (pointer search, safe),
    // the necro registers into the slot the human had.
    stage = 7;
    // Isaac's collision mode, read while he still exists (mode 1 = player type).
    {
        const uint32_t isaacCmc = FindCmc(player);
        uint32_t mode = 0;
        if (isaacCmc && Read(isaacCmc + kCmcModeOffset, &mode, 4) && mode <= 3) isaacCollisionMode = mode;
    }
    clientprog::BeforeIsaacGoes(player);   // what he carries comes back with the next Isaac
    using Unregister = bool (__thiscall*)(void*, uint32_t, int);
    reinterpret_cast<Unregister>(base + kPmUnregister)(reinterpret_cast<void*>(playerManager), player + kBasePlayer, 1);
    // Remove Isaac the way NecroPlayer::Respawn removes a necro: unregistered,
    // the entity's own shutdown virtual (slot 7), then Base::Delete. His level
    // GUID and subkey word are what the revert needs to bring him back.
    Read(player + 0x24, playerGuid, sizeof(playerGuid));
    playerSubKeyWord = subKeyWord;
    // The deletion itself waits until the necro is finalized: with Isaac already
    // gone, the necro's post-spawn init faulted at 0x007CF020 on both machines
    // (sixth live test). Unregistered now, removed after stage 9.
    parkedPlayer = 0;
    using RegisterWithManager = void (__thiscall*)(uint32_t, uint32_t);
    reinterpret_cast<RegisterWithManager>(base + kRegisterWithManager)(entity, 0);

    // The camera, as the native Spawn does it: the type's fixed camera takes the
    // active camera's settings and is pushed for player 0.
    stage = 8;
    uint32_t cameraManager = 0, cameraRef = 0;
    if (Read(base + kCameraManager, &cameraManager, 4) && cameraManager &&
        Read(entity + kNecroCamera, &cameraRef, 4) && cameraRef) {
        const uint32_t fixedCamera = cameraRef - 0x10;
        using ActiveCamData = uint32_t (__thiscall*)(void*, uint32_t);
        using SetFixed = void (__thiscall*)(uint32_t, uint32_t);
        using Push = void (__thiscall*)(void*, uint32_t, uint32_t, float, int, int);
        const uint32_t data = reinterpret_cast<ActiveCamData>(base + kGetActiveCamData)(reinterpret_cast<void*>(cameraManager), 0);
        reinterpret_cast<SetFixed>(base + kSetFixedCamera)(fixedCamera, data);
        reinterpret_cast<Push>(base + kPushCameraInfo)(reinterpret_cast<void*>(cameraManager), 0, fixedCamera, 0.0f, 1, 0);
    }
    stage = 9;
    uint32_t flags = 0;
    if (Read(entity + kNecroFlags, &flags, 4)) { flags |= 0x10; WriteBytes(entity + kNecroFlags, &flags, 4); }
    using Finalize = void (__stdcall*)(uint32_t, int);
    reinterpret_cast<Finalize>(base + kFinalize)(entity, 0);
    ApplyPlayerCollision(entity);   // doors: the necro walks with Isaac's collision type

    // Now Isaac goes, the way NecroPlayer::Respawn removes a necro: the entity's
    // own shutdown virtual (slot 7), then Base::Delete. Level GUID and subkey
    // word (saved above) bring him back on the revert.
    stage = 11;
    {
        uint32_t playerVtable = 0, shutdown = 0;
        if (Read(player, &playerVtable, 4) && Read(playerVtable + 0x1C, &shutdown, 4) && shutdown) {
            using Shutdown = void (__thiscall*)(uint32_t);
            reinterpret_cast<Shutdown>(shutdown)(player);
        }
        using Delete = void (__cdecl*)(uint32_t);   // Base::Delete ends in a bare ret: the caller cleans (seventh live test: stdcall shifted esp by 4)
        reinterpret_cast<Delete>(base + kDelete)(player);
    }
    playerGone = true;
    tk::OnBodyReplaced();

    stage = 10;
    if (sixthWanted) {
        uint32_t necroAttributes = 0;
        if (Read(entity + kSettingsAttributes, &necroAttributes, 4) && necroAttributes) leaper::Apply(necroAttributes);
    }
    necroEntity = entity;
    RegisterNecroWithSession(entity);
    InterlockedIncrement(&transforms);
    uint32_t playerId = 0, necroType = 0;
    Read(entity + kPlayerId, &playerId, 4);
    Read(entity + kNecroType, &necroType, 4);
    uint32_t proxyFlags = 0;
    if (necroProxy) Read(necroProxy + 0x30, &proxyFlags, 4);
    trackedIsaac = player;
    LifeExpect(kLifeLiving, "the transform");
    Note("transform: %s necro %08X in slot (id %u, type %u), Isaac %08X removed (guid %08X-%08X-%08X-%08X), proxy %08X flags %08X subkey %08X, slot0 body=%08X",
         TypeName(which), entity, playerId, necroType, player, playerGuid[0], playerGuid[1], playerGuid[2], playerGuid[3], necroProxy, proxyFlags, necroSubKey, coop::LocalSlotZeroEntity());
}

// Cached pointers outlive a checkpoint restart (same level, same managers), so
// nothing acts on one without checking the object is still what it was. The
// fourth live test pressed F11 after a restart: Delete on the freed necro
// wrecked the listener lists and the next dispatch died at 0x013E3BBB.
bool IsNecro(uint32_t entity) {
    uint32_t vtable = 0;
    return entity >= 0x10000 && Read(entity, &vtable, 4) && vtable == base + kNecroVtable;
}
bool IsPlayer(uint32_t entity) {
    uint32_t vtable = 0;
    return entity >= 0x10000 && Read(entity, &vtable, 4) && vtable == base + placement::kPlayerPrimaryVtableRva;
}
// Whatever sits in PlayerManager slot 0 right now, if it is a NecroPlayer (the
// native respawn may have replaced ours).
uint32_t NecroInSlotZero() {
    uint32_t manager = 0, slots = 0, bp = 0;
    if (!Read(base + kPlayerManager, &manager, 4) || !manager || !Read(manager + 4, &slots, 4) || !slots ||
        !Read(slots, &bp, 4) || bp < 0x103C0) return 0;
    return IsNecro(bp - kBasePlayer) ? bp - kBasePlayer : 0;
}

uint32_t RespawnPlayer(uint32_t playerManager, const float* matrix);
// deleteNecro=false at a checkpoint restart: the restart's own teardown destroys
// every entity, and Base::Delete is deferred, so deleting here too unlinked the
// necro twice (host crash 35492 at 0x00737863, prev pointer already null).
void Revert(bool respawnHuman, bool deleteNecro, const float* at = nullptr) {
    necroDiedAt = 0;   // Isaac again: no pace for the next transform
    leaper::Restore();   // the borrowed template gets its own animations and model back
    uint32_t playerManager = 0;
    if (!Read(base + kPlayerManager, &playerManager, 4) || !playerManager) return;
    if (!IsNecro(necroEntity)) {
        const uint32_t current = NecroInSlotZero();
        Note("revert: cached necro %08X is gone%s", necroEntity, current ? ", taking the one in slot 0" : "");
        necroEntity = current;
    }
    // Where Isaac reappears: the necro's transform, read before it goes.
    alignas(16) float necroMatrix[16]{};
    bool haveMatrix = necroEntity && Read(necroEntity + kEntityMatrix, necroMatrix, sizeof(necroMatrix));
    if (!haveMatrix) { std::memset(necroMatrix, 0, sizeof(necroMatrix)); necroMatrix[0] = necroMatrix[5] = necroMatrix[10] = necroMatrix[15] = 1.0f; }
    if (!necroEntity) { necroProxy = necroSubKey = 0; }
    else {
    using Unregister = bool (__thiscall*)(void*, uint32_t, int);
    reinterpret_cast<Unregister>(base + kPmUnregister)(reinterpret_cast<void*>(playerManager), necroEntity + kBasePlayer, 1);
    // The player id stays: NecroPlayer's teardown (ShutdownCameras via the
    // master SM, run from the deferred delete) uses GetPlayerID() as the
    // viewport index into the camera table. With -1 written here it read the
    // entry before the table (client crash 22516 at 0x007429F0, a float where
    // a SafePtr target should be). Slot 0 is Isaac's again anyway.
    // NecroPlayer::Respawn's own teardown: vtable slot 7, then Base::Delete.
    if (deleteNecro) {
        uint32_t vtable = 0, shutdown = 0;
        if (Read(necroEntity, &vtable, 4) && Read(vtable + 0x1C, &shutdown, 4) && shutdown) {
            using Shutdown = void (__thiscall*)(uint32_t);
            reinterpret_cast<Shutdown>(shutdown)(necroEntity);
        }
        using Delete = void (__cdecl*)(uint32_t);   // Base::Delete: bare ret, caller cleans
        reinterpret_cast<Delete>(base + kDelete)(necroEntity);
    }
    }
    const uint32_t gone = necroEntity;
    necroEntity = necroProxy = necroSubKey = 0;
    uint32_t human = 0;
    tk::OnBodyReplaced();
    if (respawnHuman && playerGone) human = RespawnPlayer(playerManager, at ? at : necroMatrix);
    InterlockedIncrement(&reverts);
    Note("revert: necro %08X %s, Isaac %s%08X", gone, gone ? "deleted" : "(none)",
         !respawnHuman ? "left to the restart, was " : (human ? "respawned as " : "NOT respawned (error), was "),
         human ? human : trackedIsaac);   // "was": the Isaac the transform removed
    if (human || !respawnHuman) playerGone = false;
    parkedPlayer = 0;
    if (human) LifeExpect(kLifeHuman, "the revert");
}

// Isaac comes back from the level's own Player template, at the necro's last
// transform, registered into slot 0, as the checkpoint restart does.
const uint32_t* BirthSuit(const char*& why);   // below, next to the watch suit
uint32_t RespawnPlayer(uint32_t playerManager, const float* matrix) {
    uint32_t simManager = 0;
    if (!Read(base + kSimManager, &simManager, 4) || !simManager) { InterlockedExchange(&lastError, 30); return 0; }
    SpawnInfo info{};
    if (!GetSpawnInfo(simManager, info, playerGuid)) { InterlockedExchange(&lastError, 31); return 0; }
    using SetTransform = void (__thiscall*)(SpawnInfo*, const float*);
    reinterpret_cast<SetTransform>(base + kSetTransform)(&info, matrix);
    info.words[2] = playerSubKeyWord;
    const uint32_t player = SpawnByInfo(simManager, info);
    if (!IsPlayer(player)) { InterlockedExchange(&lastError, 32); return 0; }
    using Register = uint32_t (__thiscall*)(void*, uint32_t, uint32_t, int);
    const uint32_t id = reinterpret_cast<Register>(base + kPmRegister)(reinterpret_cast<void*>(playerManager), player + kBasePlayer, 0, 1);
    WriteBytes(player + kPlayerIdOfPlayer, &id, 4);
    const char* why = nullptr;
    const uint32_t* suit = BirthSuit(why);
    const bool armed = suit && coop_uniform::ArmBirth(suit, why);
    using Finalize = void (__stdcall*)(uint32_t, int);
    reinterpret_cast<Finalize>(base + kFinalize)(player, 0);
    if (armed) coop_uniform::DisarmBirth(true);
    return player;
}

// From the checkpoint restart hook (OnRestartImpl), before the restart runs:
// the world is about to be rebuilt, so Isaac takes slot 0 back now and the
// necro goes through its own teardown while everything it references is alive.
void DeleteGhostForRestart();   // below, with the MP screens
void ForgetGhost();             // below, next to LocalGhost
bool WatchActive();             // below, next to the watch
extern volatile LONG joinAsNecroPending;   // below, next to JoinAsNecro
void OnRestart() {
    if (!enabled) return;
    DeleteGhostForRestart();
    // 17.10.: a restart during a watch left the necro by choice a human for good
    if (necroByChoice && (necroEntity || playerGone || WatchActive())) {
        InterlockedIncrement(&watchRejoins);
        JoinAsNecro();   // after the restart, outside scenes, the necro and the vents again
    }
    if (necroEntity || parkedPlayer) {
        __try { Revert(false, false); } __except (FaultFilter(GetExceptionInformation())) {
            Note("revert at restart faulted: exception %08X at %08X", static_cast<unsigned>(GetExceptionCode()), faultAddress);
            necroEntity = parkedPlayer = necroProxy = necroSubKey = 0;
        }
    }
}

// With MPLevelSettings in the world (the stream spawns it), Player::CreateGhostNameVFX
// (PC 0x86B640) runs for every ghost Player and looks up an MP name VFX the
// campaign never loads: the client died at 0x0086AEDE the moment the host
// ghosted in (dump 19516). The function's own first check is
// DeadspaceGameManager::DisableNameplates() (flags at [0x020213FC]+0x1C, bit 4),
// so that bit is kept set while this module is on.
constexpr uint32_t kGameManager = 0x1C213FC, kGameManagerFlags = 0x1C, kNameplatesOff = 4;
bool nameplatesOff = true;
volatile LONG nameplateFlagSets = 0;
void KeepNameplatesOff() {
    if (!nameplatesOff) return;
    uint32_t manager = 0, flags = 0;
    if (!Read(base + kGameManager, &manager, 4) || !manager || !Read(manager + kGameManagerFlags, &flags, 4)) return;
    if (flags & kNameplatesOff) return;
    flags |= kNameplatesOff;
    if (WriteBytes(manager + kGameManagerFlags, &flags, 4)) InterlockedIncrement(&nameplateFlagSets);
}

// Doors: a NecroPlayer's character controller carries the NPC collision mode
// (CharacterMovementComponent mode 2 -> filter type 0x20000), and campaign doors
// keep a phantom field / blocking collision on that type so necromorphs cannot
// walk through a doorway. Isaac's controller runs mode 1 (type 0x10000). So the
// necro is given Isaac's mode through the engine's own
// CharacterMovementComponent::SetCollisionMode (PC 0x00F712A0, thiscall(mode,
// force), ret 8; mode at cmc+0x224, filter info at +0x228; PC layout verified
// against the Xbox one). The component is found the way the watchdog finds
// components: a pointer inside the entity to an object with the CMC vtable
// whose owner back-pointer (+0x2C, entity+0x10) leads back.
bool playerCollision = true;
constexpr uint32_t kCmcVtableRva = 0x1763FBC, kSetCollisionModeRva = 0xB712A0;   // mode at kCmcModeOffset
bool setCollisionModeOk = false;
uint32_t isaacCollisionMode = 1;
volatile LONG collisionApplied = 0, collisionMissing = 0;
uint32_t FindCmcScan(uint32_t entity);
// The scan reads up to 3000 words at random heap addresses and ran every frame; the sampler put
// it on the game thread's stack (02.11.). The last answers are kept while the component still
// carries the CMC vtable and sits at the same offset of the entity.
struct CmcMemo { uint32_t entity = 0, cmc = 0, offset = 0; };
CmcMemo cmcMemo[8];
unsigned cmcMemoNext = 0;
volatile LONG cmcMemoHits = 0, cmcScans = 0;
uint32_t FindCmc(uint32_t entity) {
    for (const CmcMemo& m : cmcMemo) {
        if (!m.entity || m.entity != entity) continue;
        uint32_t at = 0, vtable = 0;
        if (Read(entity + m.offset, &at, 4) && at == m.cmc && Read(m.cmc, &vtable, 4) && vtable == base + kCmcVtableRva) {
            InterlockedIncrement(&cmcMemoHits);
            return m.cmc;
        }
        break;
    }
    InterlockedIncrement(&cmcScans);
    const uint32_t cmc = FindCmcScan(entity);
    if (cmc) {
        for (uint32_t offset = 0; offset < 0x1800; offset += 4) {
            uint32_t at = 0;
            if (Read(entity + offset, &at, 4) && at == cmc) {
                cmcMemo[cmcMemoNext++ % 8] = CmcMemo{entity, cmc, offset};
                break;
            }
        }
    }
    return cmc;
}
uint32_t FindCmcScan(uint32_t entity) {
    // The CMC vtable is unique to that component class; a pointer to one inside
    // the entity's own bytes is the entity's component (the owner back-pointer
    // convention differs per component family, so it is only a tie-breaker:
    // sixth live test found none with it required).
    uint32_t fallback = 0;
    for (uint32_t offset = 0; offset < 0x1800; offset += 4) {
        uint32_t candidate = 0, vtable = 0, owner = 0;
        if (!Read(entity + offset, &candidate, 4) || candidate < 0x10000 || !Read(candidate, &vtable, 4)) continue;
        if (vtable != base + kCmcVtableRva) continue;
        if (Read(candidate + 0x2C, &owner, 4) && (owner == entity + 0x10 || owner == entity)) return candidate;
        if (Read(candidate + 0x10, &owner, 4) && (owner == entity + 0x10 || owner == entity)) return candidate;
        if (!fallback) fallback = candidate;
    }
    return fallback;
}

// The necro's death is the revert (multiplayer would open its vent-select
// screen and ask a NecroPlayerRespawnManager the campaign never has: crash
// 8484 two seconds after the pack died). Reported by the hits module's health
// hook on the game thread (installed for DeathRevert alone when ReportHits is
// off, see hits::InstallBodyWatch); acted on in the next Poll.
volatile LONG deathPending = 0;
void OnBodyHealth(uint32_t entity, float after) {
    if (!enabled || !necroEntity || entity != necroEntity) return;
    if (after <= 0.0f) InterlockedExchange(&deathPending, 1);
}
// PC 0x0108F590 is UINecroSelect_GetSelection(NecroPlayerType* out) (cdecl,
// bool in al): it asks the MP12 type-select screen for the player's pick
// (crash 8484: no screen loaded in the campaign) and then compares it with the
// NecroPlayerRespawnManager's approved type. NecroRespawnProxySM's transition
// respawnSelectType -> respawnSelectLocation is exactly this call, so answering
// "nothing" (the first stand-in) left the ghost stuck in the type-select state
// with no vent pick and no input. Now: the INI type, picked.
safetyhook::InlineHook mp12Hook;
volatile LONG mp12Suppressed = 0;
constexpr uint32_t kRespawnManager = 0x1C21628;   // [0x02021628] NecroPlayerRespawnManager singleton
bool RespawnManagerPresent() { uint32_t manager = 0; return Read(base + kRespawnManager, &manager, 4) && manager != 0; }
unsigned char __cdecl OnMp12Refresh(uint32_t out) {
    InterlockedIncrement(&mp12Suppressed);
    // With the respawn manager and the MP12 menu in place the engine's own
    // answer stands: the menu's pick, compared with the manager's approved type.
    // A sixth type has no row in the MP12 screen, so the menu can never answer
    // with it whatever the manager holds. When the sixth type is enabled and
    // chosen, the INI answer stands instead of the engine's.
    const bool sixthWanted = sixth::enabled && type >= 0 && static_cast<uint32_t>(type) == sixth::slot;
    if (!sixthWanted && RespawnManagerPresent() && mp12Hook) return mp12Hook.call<unsigned char>(out);
    const uint32_t highest = sixthWanted ? sixth::slot : 3u;
    const uint32_t picked = (type < 0 || static_cast<uint32_t>(type) > highest) ? 0u : static_cast<uint32_t>(type);
    if (!out || !WriteBytes(out, &picked, 4)) return 0;
    return 1;
}
void InstallMp12Guard(FILE* log) {
    constexpr uint32_t kMp12RefreshRva = 0xC8F590;
    static const unsigned char expected[] = {0x68, 0x32, 0x31, 0x50, 0x4D, 0xE8};   // push 'MP12'; call
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kMp12RefreshRva, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) {
        std::fprintf(log, "necro mp12 guard: signature mismatch at %08X, not installed\n", static_cast<unsigned>(base + kMp12RefreshRva)); return;
    }
    mp12Hook = safetyhook::create_inline(reinterpret_cast<void*>(base + kMp12RefreshRva), reinterpret_cast<void*>(OnMp12Refresh));
    std::fprintf(log, "necro mp12 guard %s at %08X\n", mp12Hook ? "installed" : "FAILED", static_cast<unsigned>(base + kMp12RefreshRva));
}
void ApplyPlayerCollision(uint32_t entity) {
    if (!playerCollision || !setCollisionModeOk) return;
    const uint32_t cmc = FindCmc(entity);
    if (!cmc) { InterlockedIncrement(&collisionMissing); return; }
    using SetMode = void (__thiscall*)(uint32_t, uint32_t, int);
    reinterpret_cast<SetMode>(base + kSetCollisionModeRva)(cmc, isaacCollisionMode, 1);
    InterlockedIncrement(&collisionApplied);
}

// The multiplayer respawn flow, native. NecroPlayerMasterSM's death state
// pushes the death camera, drops the item and, from the MPLevelSettings
// RespawnProxy GUID, spawns the ghost-camera proxy; NecroRespawnProxySM then
// lets the player fly, highlights vents (MPLevelSettings VFX) and, on confirm,
// QueueForRespawn -> NecroPlayer::Spawn at the vent with the exit animation.
// Two things the campaign lacks are answered here:
//  1. NecroPlayerRespawnManager ([0x02021628], the type approval/timers) is
//     null: the SM reads its approved type at [this+0x30] directly. The hook
//     at PC 0x010C2760 was meant as that getter, but the Xbox build (11.10.)
//     names it ReleaseNecroPlayer: it resets +0x30 to 5 and releases the type.
//     Its callers (~NecroRespawnProxySM, UINecroSelect_ReleaseSelection) ignore
//     the return value, so the replacement only keeps the approved type alive.
//  2. the Apt screens MP12 (type select) and MP13 (vent HUD) have no movie
//     loaded; their Show/Hide entry points (PC 0x0108F550/0x0108F570 show,
//     0x01085C90/0x01085CB0 hide) only run while the MP screen set exists
//     (EnsureMpUi). The world-side vent highlight VFX still runs (it comes
//     from MPLevelSettings), so the vent pick is visible.
bool nativeRespawn = true, deathRevert = false;
safetyhook::InlineHook approvedTypeHook, uiHooks[6];
volatile LONG approvedTypeAnswered = 0;
uint32_t __fastcall OnApprovedType(void* self, void*) {
    InterlockedIncrement(&approvedTypeAnswered);
    // With a respawn manager in place (EnsureRespawnManager) its own answer
    // counts: +0x30, the type the menu picked (5 = none picked yet).
    uint32_t approved = 5;
    // A sixth type has to pass here too, and it may not be 5: the engine uses
    // that value for "nothing picked yet", which is why the default slot is 6.
    const uint32_t sixthSlot = (sixth::enabled && sixth::Configured()) ? sixth::slot : 0xFFFFFFFFu;
    if (self && Read(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)) + 0x30, &approved, 4) &&
        (approved < 4 || approved == sixthSlot)) return approved;
    const uint32_t asked = type < 0 ? 0u : static_cast<uint32_t>(type);
    return (asked < 4 || asked == sixthSlot) ? asked : 0u;
}
// The SM polls IsShown_Multiplayer_NecroRespawn / _VentSelection (bytes in the
// UI component manager [0x02089C40] at +0xA03C / +0xA374) before it moves on,
// so the stand-ins keep those bytes honest: shown after Show, clear after Hide.
constexpr uint32_t kUiManager = 0x1C89C40, kShownMp12 = 0xA03C, kShownMp13 = 0xA374;
volatile LONG uiShows = 0, uiHides = 0;
void SetShown(uint32_t offset, unsigned char value) {
    uint32_t manager = 0;
    if (Read(base + kUiManager, &manager, 4) && manager) WriteBytes(manager + offset, &value, 1);
}
// With the multiplayer screen set built (EnsureMpUi below) the engine's own
// show/hide run for MP13, the vent-select HUD. MP12 (the necro type menu) stays
// hidden: its pick needs the NecroPlayerRespawnManager, and GetSelection above
// answers the INI type instead.
bool MpUiPresent() { uint32_t set = 0; return Read(base + kUiManager, &set, 4) && set != 0; }
// Bit 0 MP12, bit 1 MP13: shown by the engine and not hidden since.
volatile LONG hudShown = 0, hudOrphansHidden = 0, hudSweepsDone = 0;
// 17.10.: the hide before a restart is lost with the world (the vent HUD stood on Isaac after
// the reload), so it is done again twice once the next level is settled and no ghost is up.
int hudSweeps = 0;
DWORD hudSweepAt = 0;
void __cdecl OnShowMp12() {
    InterlockedIncrement(&uiShows);
    if (MpUiPresent() && RespawnManagerPresent() && uiHooks[0]) { uiHooks[0].call<void>(); InterlockedOr(&hudShown, 1); }
}
void __cdecl OnShowMp13() { InterlockedIncrement(&uiShows); if (MpUiPresent() && uiHooks[1]) { uiHooks[1].call<void>(); InterlockedOr(&hudShown, 2); } }
// 17.10. (live, both copies): 0x01085C90/B0 are byte for byte the show wrappers, GetUIManager
// 0x0070D2F0, the tag, then 0x0071C500 (make the screen if needed, virtual +0x4C, show). They were
// taken for hides since 06.10.; the screen still shows through them, so they count as shows here.
void __cdecl OnHideMp12() { InterlockedIncrement(&uiShows); if (MpUiPresent() && uiHooks[2]) { uiHooks[2].call<void>(); InterlockedOr(&hudShown, 1); } }
void __cdecl OnHideMp13() { InterlockedIncrement(&uiShows); if (MpUiPresent() && uiHooks[3]) { uiHooks[3].call<void>(); InterlockedOr(&hudShown, 2); } }
void __cdecl OnHideMp12Tag() { InterlockedIncrement(&uiHides); InterlockedAnd(&hudShown, ~1); if (MpUiPresent() && uiHooks[4]) uiHooks[4].call<void>(); }
void __cdecl OnHideMp13Tag() { InterlockedIncrement(&uiHides); InterlockedAnd(&hudShown, ~2); if (MpUiPresent() && uiHooks[5]) uiHooks[5].call<void>(); }
void __cdecl OnUiNoop() {}
// The ghost's type menu and vent HUD go with the ghost. Its own teardown leaves them up (crash
// 29076; 17.10.: Isaac back from the vents for a scene had both on screen and they took his input).
// The real hide is the by-tag one: 0x01072DE0/F0 push 'MP12'/'MP13' and call 0x00A38A40 (cdecl),
// which, with the Apt UI manager [0x02042830] up, calls 0x0071A3A0 on its +0x74 (find the screen
// by tag, virtual +0x50, hide). Until 17.10. this called the show copies above, so every "hide"
// showed the menus again: the HUD that stood on Isaac and on the living necro all day.
constexpr uint32_t kHideByTagRva = 0x638A40, kTagMp12 = 0x4D503132, kTagMp13 = 0x4D503133;
int hideByTagOk = -1;   // the routine's bytes checked once
void RealHide(uint32_t tag) {
    if (hideByTagOk < 0) {
        static const unsigned char kTail[] = {0x85, 0xC0, 0x74, 0x11, 0x8D, 0x48, 0x74, 0x85, 0xC9, 0x74, 0x0A};
        unsigned char head[16]{};
        hideByTagOk = Read(base + kHideByTagRva, head, sizeof(head)) && head[0] == 0xE8 && std::memcmp(head + 5, kTail, sizeof(kTail)) == 0 ? 1 : 0;
        Note("hud: the hide-by-tag routine at %08X %s", static_cast<unsigned>(base + kHideByTagRva), hideByTagOk ? "matches" : "does NOT match, no hide");
    }
    if (hideByTagOk == 1) reinterpret_cast<void(__cdecl*)(uint32_t)>(base + kHideByTagRva)(tag);
}
volatile LONG hudHides = 0;
void HideNecroHud() {
    __try {
        if (MpUiPresent()) {
            if (uiHooks[5]) uiHooks[5].call<void>(); else RealHide(kTagMp13);
            if (uiHooks[4]) uiHooks[4].call<void>(); else RealHide(kTagMp12);
            InterlockedIncrement(&hudHides);
        }
    } __except (FaultFilter(GetExceptionInformation())) {
        Note("hud: the hide faulted at %08X", faultAddress);
    }
    SetShown(kShownMp13, 0);
    SetShown(kShownMp12, 0);
    InterlockedExchange(&hudShown, 0);
}
// Whether the screens are up, from the screens themselves (17.10., live: an open screen has its
// state at +4 set, not -1, and its movie at +0x2C; the closed one on the host -1 and null). The
// bytes at +0x4C read before were 0 on both copies with the menus open. Bit 0 MP12, bit 1 MP13.
constexpr uint32_t kMp12Screen = 0x9FF0, kMp13Screen = 0xA328;
LONG EngineShown() {
    uint32_t block = 0;
    if (!Read(base + kUiManager, &block, 4) || !block) return 0;
    const auto open = [&](uint32_t screen) {
        uint32_t state = 0xFFFFFFFFu, movie = 0;
        return Read(block + screen + 4, &state, 4) && Read(block + screen + 0x2C, &movie, 4) && state != 0xFFFFFFFFu && movie != 0;
    };
    return (open(kMp12Screen) ? 1 : 0) | (open(kMp13Screen) ? 2 : 0);
}
// 17.10. (crash 29076 at 0x013E2D21): the RespawnProxy comes from global_mp_assets' template, so a
// checkpoint restart's teardown left it standing; its state machine (vtable 0x01B785EC) ran on the
// freed level in the loading screen with the vent HUD still up. It goes before the restart.
volatile LONG restartGhostsDeleted = 0;
void DeleteGhostForRestart() {
    const uint32_t proxy = LocalGhost();
    if (!proxy) return;
    HideNecroHud();
    hudSweeps = 2;
    uint32_t vtable = 0, shutdown = 0;
    __try {
        if (Read(proxy, &vtable, 4) && Read(vtable + 0x1C, &shutdown, 4) && shutdown) reinterpret_cast<void(__thiscall*)(uint32_t)>(shutdown)(proxy);
        reinterpret_cast<void(__cdecl*)(uint32_t)>(base + kDelete)(proxy);
        InterlockedIncrement(&restartGhostsDeleted);
    } __except (FaultFilter(GetExceptionInformation())) {
        Note("restart: deleting the ghost proxy %08X faulted at %08X", proxy, faultAddress);
    }
    ForgetGhost();
    Note("restart: the ghost proxy %08X deleted before the world goes (it outlives the teardown), vent HUD hidden", proxy);
}

// The multiplayer HUD screens (MP12 necro respawn, MP13 vent selection, MP10
// necro pause, MP14 scoring, MP17 spectator, ...) are C++ AptSubscriber objects
// living in one MPGameUIComponents block: PC UIComponentManager::Init(context)
// (0x00ABCFE0) allocates it with operator new (0x00403CB0, 0x17E30 bytes), runs
// the ctor (0x00AB4CA0, 'POGN' first) and stores it at [0x02089C40]; MP12 sits
// at +0x9FF0, MP13 at +0xA328. The campaign runs context 1 (SPGameUIComponents
// at [0x02089C3C]) and leaves the MP pointer null, so every MP screen call
// found nothing, although the Flash movies themselves (mp12necrorespawn,
// mp13ventselection) are in memory in a campaign session. So the block is
// built the engine's way once the level's own UI set exists, and
// UIComponentManager::Close (0x00ABA950: dtor 0x00AB51C0 + operator delete)
// tears it down with the level like any MP match would.
bool mpUi = true;
constexpr uint32_t kSpUiSet = 0x1C89C3C, kMpUiSetSize = 0x17E30, kOperatorNewRva = 0x3CB0, kMpUiSetCtorRva = 0x6B4CA0;
volatile LONG mpUiBuilt = 0, mpUiFaults = 0;
uint32_t mpUiBlock = 0;
void EnsureMpUi() {
    if (!mpUi || !nativeRespawn) return;
    uint32_t current = 0, sp = 0;
    if (!Read(base + kUiManager, &current, 4) || current) return;
    if (!Read(base + kSpUiSet, &sp, 4) || !sp) return;   // no level UI yet (front end / loading)
    static const unsigned char expected[] = {0x53, 0x55, 0x56, 0x57, 0x68, 0x50, 0x4F, 0x47, 0x4E, 0x8B, 0xF1};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kMpUiSetCtorRva, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) {
        mpUi = false;
        Note("mp ui: set ctor signature mismatch at %08X, MP screens stay off", static_cast<unsigned>(base + kMpUiSetCtorRva));
        return;
    }
    __try {
        using New = void* (__cdecl*)(uint32_t);
        void* block = reinterpret_cast<New>(base + kOperatorNewRva)(kMpUiSetSize);
        if (!block) { mpUi = false; Note("mp ui: operator new failed"); return; }
        std::memset(block, 0, kMpUiSetSize);
        using Ctor = void* (__thiscall*)(void*);
        // Not freed after a ctor fault: its subscribers may already be registered.
        // mpUi goes off, so at most one block per session.
        reinterpret_cast<Ctor>(base + kMpUiSetCtorRva)(block);
        const uint32_t address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block));
        WriteBytes(base + kUiManager, &address, 4);
        mpUiBlock = address;
        InterlockedIncrement(&mpUiBuilt);
        Note("mp ui: MPGameUIComponents built at %08X (MP12 %08X, MP13 %08X) beside the SP set %08X", address, address + 0x9FF0, address + 0xA328, sp);
    } __except (FaultFilter(GetExceptionInformation())) {
        mpUi = false;
        InterlockedIncrement(&mpUiFaults);
        Note("mp ui: set ctor faulted: exception %08X at %08X; MP screens stay off", static_cast<unsigned>(GetExceptionCode()), faultAddress);
    }
}
// CreateRespawnProxy ends with AutoSelectRespawnVent(proxy) and
// MoveProxyToRoomViewCamera(proxy, vent) (PC 0x01072A00, cdecl; both pointers
// null-checked by the engine itself). The mover puts the ghost at the vent's
// authored room-view camera position (VentMarker attributes+0x140, read from
// [vent+0xC]) looking at the vent centroid ([vent+0x330]), then
// Entity::SetMatrix (vtable +0x2C), CMC::SetMatrix, ResetPyr, VentMarker::OnSpawn.
// The MP maps author that position; campaign vents leave it at zero, so the
// ghost camera landed at the world origin (the "teleported away" death camera
// of the 23:20 test). Here a room-view position is synthesized next to the
// vent, 4.5 m towards where the necro fell and 2 m up, written into the vent's
// attributes, and the engine's own mover does the rest. RespawnProxy::Initialize
// does not touch the PlayerManager (the corpse keeps slot 0), so this call is
// also the first reliable sign that the local necro died.
safetyhook::InlineHook moveProxyHook;
volatile LONG moveProxySkipped = 0, moveProxyPassed = 0, moveProxySynthesized = 0, moveProxyParked = 0;
uint32_t ghostProxy = 0;   // the RespawnProxy of the current ghost phase
// Where a room view camera looks from (the human the ghost was put near), taken once by the
// next MoveProxyToRoomViewCamera instead of the corpse.
float roomViewAnchor[3]{};
bool roomViewAnchorSet = false;
// The auto pick's last pass took a vent far from every human (17.10.: ch11's insider scene, the Tiedemann fight, puts
// Isaac in Nicole's arena, no vent near): the camera goes to the human, the vent stays selected.
bool viewAtHuman = false;
float viewHuman[3]{};
unsigned viewHumanSlot = 0;
DWORD viewAtHumanAt = 0;
const char* viewHumanWhy = "";
constexpr DWORD kViewAtHumanMs = 3000;
volatile LONG viewsAtHuman = 0, viewsAtHumanExpired = 0;
constexpr uint32_t kProxyGetCmcRva = 0xAB5A50, kCmcSetMatrixRva = 0xB72DC0, kProxyResetPyrRva = 0xA928F0;
constexpr uint32_t kVentAttributes = 0xC, kVentCameraPos = 0x140, kVentCentroid = 0x330, kEntitySetMatrixSlot = 0x2C;
constexpr uint32_t kVentEdgeMatrixRva = 0xEC8C80;   // VentMarker::GetEdgeMidPointMatrix, thiscall(out, bool, bool, bool)
constexpr uint32_t kVentEdgePosRva = 0xEC8C10;   // VentMarker::GetEdgeMidPointPosition, thiscall(out, axisSel, positive), ret 0xC
constexpr uint32_t kGetGravityRva = 0xABB7E0;    // cdecl GetGravity(out, position, 0, 0), caller cleans 0x10
constexpr float kRoomViewUp = 1.8f;
// The vent the last request was granted for, and the necro that came out of
// it: a fresh necro that has not moved three seconds after the vent spawn is
// still inside the vent (some campaign vents have no exit animation for the
// MP necros; a puker stuck in the 12:37 test) and gets pushed into the room.
bool ventFallRescue = true, fallRescueDone = false;   // [Necro] VentFallRescue
volatile LONG fallRescues = 0;
// Where the necro last stood on the map (sampled every 500 ms while it is above the
// lowest spawn point and not falling): a fall through the world goes back there.
float safePos[3]{}, safeSample[3]{};
bool haveSafe = false;
DWORD safeSampleAt = 0, fallRescueAt = 0;
bool ventUnstick = false;   // the crude push-out; off, the native exit below handles the walk-in vents
// NecroPlayerExitVentSM knows three vent types: 0 FLOOR (launch anim, done),
// 1 WALL and 2 CEILING (launch, loop with velocity, land). The VentType enum
// also has 3 WALLWALKIN and 4 CEILINGWALKIN, the big vents necromorphs walk
// out of; for those none of the SM's transitions (type == 0/1/2) ever fires
// and the necro stays in the vent (the puker of the 12:37 test). The type
// lives in the vent's attributes (+0x134; the SM copies it at init, InitExitVent
// and GetVentExitAnim read it again), so for the duration of the respawn a
// walk-in vent is presented as its plain wall/ceiling counterpart and restored
// afterwards. [Necro] WalkInVentMap=1.
bool walkInVentMap = true;
constexpr uint32_t kVentTypeAttribute = 0x134;
uint32_t overriddenVentAttrs = 0, overriddenVentType = 0, lastGrantedVentType = 0xFFFFFFFFu;
DWORD ventOverrideAt = 0;
bool exitReportDone = false;
volatile LONG ventTypesMapped = 0;
void RestoreVentType() {
    if (!overriddenVentAttrs) return;
    WriteBytes(overriddenVentAttrs + kVentTypeAttribute, &overriddenVentType, 4);
    Note("vent type restored to %u (attributes %08X)", overriddenVentType, overriddenVentAttrs);
    overriddenVentAttrs = 0; overriddenVentType = 0; ventOverrideAt = 0;
}
// The exit camera (NecroPlayerExitVentSM::PushVentCamera -> VentMarker::
// GetRespawnCameraMatrix) is built from two attributes in the vent's own
// frame: m_necroPlayerRespawnCameraPositionOffset (+0x150) and
// m_necroPlayerRespawnCameraLookAtOffset (+0x160), both transformed by the
// vent's entity matrix. Every MP vent has them authored; no campaign vent
// does, so the exit camera sat inside the vent. At the grant the ghost
// camera's spot (already computed for that vent) and the vent centroid are
// written there, converted into the vent frame (rows of +0x90 as the axes,
// +0xC0 as the origin).
constexpr uint32_t kVentCameraOffset = 0x150, kVentLookAtOffset = 0x160;
float lastRoomCamera[3]{};
uint32_t lastRoomCameraVent = 0;
void WorldToVentLocal(const float* ventMatrix, const float* world, float* local) {
    const float d[3] = {world[0] - ventMatrix[12], world[1] - ventMatrix[13], world[2] - ventMatrix[14]};
    for (int axis = 0; axis < 3; ++axis) {
        const float* row = ventMatrix + axis * 4;
        const float len2 = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
        local[axis] = len2 > 1e-6f ? (d[0] * row[0] + d[1] * row[1] + d[2] * row[2]) / len2 : 0.0f;
    }
}
float grantedVentCentroid[3]{};
bool haveGrantedVent = false;
DWORD adoptedAt = 0;
float adoptedPos[3]{};
bool unstickDone = false;
volatile LONG unstuck = 0;
float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
bool Normalize3(float* v) {
    const float len = std::sqrt(Dot3(v, v));
    if (!(len > 1e-4f) || !std::isfinite(len)) return false;
    v[0] /= len; v[1] /= len; v[2] /= len;
    return true;
}
// --- cameras in front of a vent ------------------------------------------------------------------
// 17.10. (user, "the camera still spawns beside or behind the vent"): the ghost camera took the edge
// matrix's row 2 for "out of the vent", but that row lies ALONG the opening (towards the edge the
// necro is placed on), so a wall vent's camera went sideways along the wall or into it; the exit
// camera stood 2 to 4 m to the side, inside the wall of a narrow campaign corridor. Both now stand on
// the vent's normal (VentMarker+0x310, the way out of the opening, the edge matrices' row 1), checked
// against the level's collision: a normal that runs into geometry at once is turned round, and a
// camera behind a wall is pulled in to just short of it.
constexpr uint32_t kVentNormal = 0x310;
volatile LONG cameraNormalsFlipped = 0, cameraSpotsClamped = 0, cameraNoNormal = 0;
float CastFraction(const float* from, const float* to) {
    if (!bot_sense::losReady) return 1.0f;
    alignas(16) float a[4] = {from[0], from[1], from[2], 1.0f}, b[4] = {to[0], to[1], to[2], 1.0f};
    float fraction = 1.0f;
    uint32_t hit = 0;
    return bot_sense::CastRaw(a, b, 0, fraction, hit) && std::isfinite(fraction) ? fraction : 1.0f;
}
// The way out of the opening, pointing into the room.
bool VentNormal(uint32_t vent, const float* centroid, float* out) {
    if (!Read(vent + kVentNormal, out, 12) || !Normalize3(out)) { InterlockedIncrement(&cameraNoNormal); return false; }
    const float plusFrom[3] = {centroid[0] + out[0] * 0.15f, centroid[1] + out[1] * 0.15f, centroid[2] + out[2] * 0.15f};
    const float plusTo[3] = {centroid[0] + out[0] * 2.0f, centroid[1] + out[1] * 2.0f, centroid[2] + out[2] * 2.0f};
    const float minusFrom[3] = {centroid[0] - out[0] * 0.15f, centroid[1] - out[1] * 0.15f, centroid[2] - out[2] * 0.15f};
    const float minusTo[3] = {centroid[0] - out[0] * 2.0f, centroid[1] - out[1] * 2.0f, centroid[2] - out[2] * 2.0f};
    if (CastFraction(plusFrom, plusTo) < 0.25f && CastFraction(minusFrom, minusTo) > 0.9f) {
        out[0] = -out[0]; out[1] = -out[1]; out[2] = -out[2];
        InterlockedIncrement(&cameraNormalsFlipped);
    }
    return true;
}
// A spot `distance` out of the vent along `dir` and `lift` up, pulled in to 0.3 m short of anything
// between the opening and it.
void FrontSpot(const float* centroid, const float* dir, float distance, float lift, float* out) {
    const float from[3] = {centroid[0] + dir[0] * 0.4f, centroid[1] + dir[1] * 0.4f + 0.2f, centroid[2] + dir[2] * 0.4f};
    const float want[3] = {centroid[0] + dir[0] * distance, centroid[1] + dir[1] * distance + lift, centroid[2] + dir[2] * distance};
    const float fraction = CastFraction(from, want);
    if (fraction >= 0.999f) { std::memcpy(out, want, 12); return; }
    const float d[3] = {want[0] - from[0], want[1] - from[1], want[2] - from[2]};
    const float len = std::sqrt(Dot3(d, d));
    float keep = len > 0.01f ? fraction - 0.3f / len : 0.0f;
    if (keep < 0.0f) keep = 0.0f;
    for (int i = 0; i < 3; ++i) out[i] = from[i] + d[i] * keep;
    InterlockedIncrement(&cameraSpotsClamped);
}
// The edge the exit actually uses. NecroPlayerExitVentSM::InitExitVent
// (0x0108C5E0, read 22.09.) builds its matrix with GetEdgeMidPointMatrix(out,
// 1, flag, 0) and only a WALL vent (type 1) computes that flag: it takes both
// ends of axis B (GetEdgeMidPointPosition(out, 0, 1) and (out, 0, 0)), and
// when (endB - endA) points along gravity the flag is 1. Every other type
// (floor and ceiling) keeps flag 0. NecroPlayer::Spawn, though, is called with
// the edge NecroRespawnProxySM derived from the ghost camera, so on a floor
// vent the necro could be placed at one edge while the exit drove it from the
// other: it left through the wrong side or dropped through the level (the
// puker on the floor vent). The spawn now takes the exit's edge.
int EngineExitEdge(uint32_t vent, uint32_t ventType) {
    if (!vent || ventType != 1) return 0;
    alignas(16) float endA[4]{}, endB[4]{}, gravity[4]{}, centroid[4]{};
    if (!Read(vent + kVentCentroid, centroid, 16)) return 0;
    bool measured = false;
    __try {
        using EdgePos = void (__thiscall*)(uint32_t, float*, unsigned char, unsigned char);
        using GetGravity = void (__cdecl*)(float*, const float*, uint32_t, uint32_t);
        reinterpret_cast<GetGravity>(base + kGetGravityRva)(gravity, centroid, 0, 0);
        reinterpret_cast<EdgePos>(base + kVentEdgePosRva)(vent, endA, 0, 1);
        reinterpret_cast<EdgePos>(base + kVentEdgePosRva)(vent, endB, 0, 0);
        measured = true;
    } __except (FaultFilter(GetExceptionInformation())) {}
    if (!measured) return 0;
    const float delta[3] = {endB[0] - endA[0], endB[1] - endA[1], endB[2] - endA[2]};
    return Dot3(delta, gravity) < 0.0f ? 1 : 0;
}
// MP-faithful exit camera. Every MP vent authors the two exit-camera
// attributes; decoded from the 120 MP vents (mp_*.str, 22.09.) the rule is,
// in the vent's own frame (row Y of its matrix = the vent's normal, X/Z lie in
// the vent plane):
//   wall vent   camera (2.0 up, 0.5..1.5 out of the wall, 3..6 sideways along
//               the wall), look-at (0.5 up, 1.5 out of the wall, 0..1 sideways)
//   floor vent  camera (2.0 up, 3..6 back against the exit direction, 0.75
//               aside), look-at (floor height, 3.0 along the exit direction,
//               0.75 aside); the camera stands behind the necro's leap,
//               like the third-person camera that follows.
// No MP level has a ceiling vent; the floor rule is mirrored downwards for
// those. Everything is built in world space from the centroid (+0x330), the
// spawn edge's matrix (GetEdgeMidPointMatrix(out, 1, side, 0): its position
// is the spawn edge, its forward row the way the necro comes out) and world
// up, then converted into the vent frame for the attributes. Written before
// NecroPlayer::Spawn (the exit SM pushes the camera during the spawn) with
// the engine's own edge choice; at the vent grant the edge is guessed from
// the ghost camera the way NecroRespawnProxySM does (its forward against
// the edge forwards). Attributes written here are remembered so a second
// death at the same vent is not mistaken for authored data.
// The attribute blocks belong to the level's streams: forgotten at a level
// exit (ForgetExitCameras), and the oldest entry makes room when full.
uint32_t exitCameraAttrs[64]{};
unsigned exitCameraAttrCount = 0, exitCameraAttrNext = 0;
volatile LONG exitCamerasWritten = 0;
bool ExitCameraOurs(uint32_t attributes) {
    for (unsigned i = 0; i < exitCameraAttrCount; ++i) if (exitCameraAttrs[i] == attributes) return true;
    return false;
}
void RememberExitCamera(uint32_t attributes) {
    if (ExitCameraOurs(attributes)) return;
    exitCameraAttrs[exitCameraAttrNext] = attributes;
    exitCameraAttrNext = (exitCameraAttrNext + 1) % std::size(exitCameraAttrs);
    if (exitCameraAttrCount < std::size(exitCameraAttrs)) ++exitCameraAttrCount;
}
void ForgetExitCameras() { exitCameraAttrCount = exitCameraAttrNext = 0; }
// side: 0/1 = the spawn edge (GetEdgeMidPointMatrix's second bool), -1 = guess.
void WriteExitCamera(uint32_t vent, int side, const char* when) {
    uint32_t attributes = 0, ventType = 0xFFFFFFFFu;
    if (!vent || !Read(vent + kVentAttributes, &attributes, 4) || !attributes) return;
    float authored[3]{};
    if (!Read(attributes + kVentCameraOffset, authored, 12)) return;
    if (!ExitCameraOurs(attributes) && Dot3(authored, authored) > 0.01f) return;   // authored (MP vent): the engine's own
    Read(attributes + kVentTypeAttribute, &ventType, 4);
    alignas(16) float ventMatrix[16];
    float centroid[3]{};
    if (!Read(vent + kEntityMatrix, ventMatrix, sizeof(ventMatrix)) || !Read(vent + kVentCentroid, centroid, 12)) return;
    alignas(16) float edge[2][16];
    bool haveEdge[2] = {false, false};
    __try {
        using EdgeMatrix = void (__thiscall*)(uint32_t, float*, unsigned char, unsigned char, unsigned char);
        reinterpret_cast<EdgeMatrix>(base + kVentEdgeMatrixRva)(vent, edge[0], 1, 0, 0);
        haveEdge[0] = true;
        reinterpret_cast<EdgeMatrix>(base + kVentEdgeMatrixRva)(vent, edge[1], 1, 1, 0);
        haveEdge[1] = true;
    } __except (FaultFilter(GetExceptionInformation())) {}
    if (!haveEdge[0] || !haveEdge[1]) return;
    if (side != 0 && side != 1) {
        // The proxy SM's pick: the edge whose forward lines up with the ghost camera's.
        side = 1;
        if (lastRoomCameraVent == vent) {
            float look[3] = {centroid[0] - lastRoomCamera[0], centroid[1] - lastRoomCamera[1], centroid[2] - lastRoomCamera[2]};
            if (Normalize3(look)) {
                float f0[3] = {edge[0][8], edge[0][9], edge[0][10]}, f1[3] = {edge[1][8], edge[1][9], edge[1][10]};
                side = Dot3(f0, look) > Dot3(f1, look) ? 0 : 1;
            }
        }
    }
    const float* e = edge[side];
    const float up[3] = {0.0f, 1.0f, 0.0f};
    // GetEdgeMidPointMatrix's rows, decoded 22.09.: row 1 (+0x10) is the vent's
    // normal (+0x310), the way out of the opening, and row 2 (+0x20) lies
    // along the chosen axis, pointing at the edge the necro is placed on.
    // Row 0 is the remaining in-plane axis.
    float outward[3] = {e[4], 0.0f, e[6]};           // out of a wall, horizontal part
    float edgeDir[3] = {e[8], 0.0f, e[10]};          // along the opening, towards the spawn edge
    if (!(Dot3(edgeDir, edgeDir) > 0.0001f)) { edgeDir[0] = e[12] - centroid[0]; edgeDir[1] = 0.0f; edgeDir[2] = e[14] - centroid[2]; }
    float forward[3] = {outward[0], outward[1], outward[2]};
    const bool wall = ventType == 1 || ventType == 3, ceiling = ventType == 2 || ventType == 4;
    alignas(16) float cam[4] = {0, 0, 0, 1.0f}, lookAt[4] = {0, 0, 0, 1.0f};
    const char* rule = "floor";
    // How far to the side (wall) or behind (floor). MP authors 3 to 6 m per
    // room; 4 m is the middle and reproduced 117 of the 120 authored cameras
    // to within 2.5 m. Campaign corridors are narrower than MP arenas and
    // nothing here can test the space, so the reach is pulled in to the corpse:
    // the necro walked that far from the vent, so that much room exists.
    float span = 4.0f;
    if (haveLastNecroMatrix) {
        const float toCorpse[3] = {lastNecroMatrix[12] - centroid[0], 0.0f, lastNecroMatrix[14] - centroid[2]};
        const float reach = std::sqrt(Dot3(toCorpse, toCorpse));
        if (std::isfinite(reach) && reach < span) span = reach > 2.0f ? reach : 2.0f;
    }
    if (wall) {
        rule = "wall";
        // Out of the wall: the vent's own normal (checked against the collision), else the exit forward.
        float normal[3] = {forward[0], forward[1], forward[2]};
        float own[3]{};
        if (VentNormal(vent, centroid, own)) { normal[0] = own[0]; normal[1] = 0.0f; normal[2] = own[2]; }
        if (!Normalize3(normal)) { normal[0] = ventMatrix[4]; normal[1] = 0.0f; normal[2] = ventMatrix[6]; if (!Normalize3(normal)) return; }
        float lateral[3] = {up[1] * normal[2] - up[2] * normal[1], up[2] * normal[0] - up[0] * normal[2], up[0] * normal[1] - up[1] * normal[0]};
        if (!Normalize3(lateral)) return;
        // Sideways towards the corpse's side (the room the necro died in), else +.
        float sign = 1.0f;
        if (haveLastNecroMatrix) {
            const float toCorpse[3] = {lastNecroMatrix[12] - centroid[0], 0.0f, lastNecroMatrix[14] - centroid[2]};
            const float along = Dot3(toCorpse, lateral);
            if (along < -0.3f) sign = -1.0f;
        }
        // In front of the opening, slightly to the corpse's side, looking at where he comes out.
        const float slant[3] = {normal[0] * 0.97f + lateral[0] * sign * 0.25f, 0.0f, normal[2] * 0.97f + lateral[2] * sign * 0.25f};
        FrontSpot(centroid, slant, 2.6f, 1.3f, cam);
        for (int i = 0; i < 3; ++i) lookAt[i] = centroid[i] + up[i] * 0.4f + normal[i] * 1.0f;
        (void)span;
    } else {
        // Floor (and, mirrored, ceiling): behind the leap, looking along it.
        float exitDir[3] = {edgeDir[0], edgeDir[1], edgeDir[2]};
        if (!Normalize3(exitDir)) { exitDir[0] = forward[0]; exitDir[1] = forward[1]; exitDir[2] = forward[2]; if (!Normalize3(exitDir)) { exitDir[0] = ventMatrix[8]; exitDir[1] = 0.0f; exitDir[2] = ventMatrix[10]; if (!Normalize3(exitDir)) return; } }
        float aside[3] = {up[1] * exitDir[2] - up[2] * exitDir[1], up[2] * exitDir[0] - up[0] * exitDir[2], up[0] * exitDir[1] - up[1] * exitDir[0]};
        Normalize3(aside);
        if (ceiling) rule = "ceiling";
        for (int i = 0; i < 3; ++i) {
            if (ceiling) {
                cam[i] = centroid[i] - up[i] * 1.5f - exitDir[i] * span + aside[i] * 0.75f;
                lookAt[i] = centroid[i] - up[i] * 2.5f + exitDir[i] * 2.0f + aside[i] * 0.75f;
            } else {
                cam[i] = centroid[i] + up[i] * 2.0f - exitDir[i] * span + aside[i] * 0.75f;
                lookAt[i] = centroid[i] + exitDir[i] * 3.0f + aside[i] * 0.75f;
            }
        }
    }
    alignas(16) float camLocal[4] = {0, 0, 0, 1.0f}, lookAtLocal[4] = {0, 0, 0, 1.0f};
    WorldToVentLocal(ventMatrix, cam, camLocal);
    WorldToVentLocal(ventMatrix, lookAt, lookAtLocal);
    if (!WriteBytes(attributes + kVentCameraOffset, camLocal, 16) || !WriteBytes(attributes + kVentLookAtOffset, lookAtLocal, 16)) return;
    RememberExitCamera(attributes);
    InterlockedIncrement(&exitCamerasWritten);
    Note("exit camera (%s, MP %s rule, edge %d): world cam (%.1f %.1f %.1f) look-at (%.1f %.1f %.1f) = vent-local (%.2f %.2f %.2f) / (%.2f %.2f %.2f); centroid (%.1f %.1f %.1f), edge at (%.1f %.1f %.1f) forward (%.2f %.2f %.2f)",
         when, rule, side, cam[0], cam[1], cam[2], lookAt[0], lookAt[1], lookAt[2], camLocal[0], camLocal[1], camLocal[2], lookAtLocal[0], lookAtLocal[1], lookAtLocal[2],
         centroid[0], centroid[1], centroid[2], e[12], e[13], e[14], e[8], e[9], e[10]);
}
// Places the proxy (entity, character controller, camera pitch/yaw) at a matrix
// the way MoveProxyToRoomViewCamera does, without a vent.
void PlaceProxy(uint32_t proxy, const float* matrix16) {
    uint32_t vtable = 0, setMatrix = 0;
    if (!Read(proxy, &vtable, 4) || !Read(vtable + kEntitySetMatrixSlot, &setMatrix, 4) || !setMatrix) return;
    using MatrixCall = void (__thiscall*)(uint32_t, const float*);
    reinterpret_cast<MatrixCall>(setMatrix)(proxy, matrix16);
    using GetCmc = uint32_t (__thiscall*)(uint32_t);
    const uint32_t cmc = reinterpret_cast<GetCmc>(base + kProxyGetCmcRva)(proxy);
    if (cmc) reinterpret_cast<MatrixCall>(base + kCmcSetMatrixRva)(cmc, matrix16);
    reinterpret_cast<MatrixCall>(base + kProxyResetPyrRva)(proxy, matrix16);
}
volatile LONG moveProxyZeroViews = 0, paceFromProxy = 0, roomViewsOnNormal = 0;
void PaceStart(const char* how);   // below
void __cdecl OnMoveProxyToRoomView(uint32_t proxy, uint32_t vent) {
    // First sighting of a proxy: the local necro died. The corpse stays in slot 0 and
    // belongs to the native flow now; F11 means EscapeGhost.
    bool died = false;
    if (proxy && proxy != ghostProxy) {
        ghostProxy = proxy;
        ghostProxySince = Stamp(GetTickCount());
        LifeExpect(kLifeGhost, necroEntity ? "the necro's death or a vent re-entry" : "the ghost camera");
        if (necroEntity) {
            died = true;
            deadNecro = necroEntity;
            necroEntity = 0;
            necroProxy = necroSubKey = 0;
            InterlockedIncrement(&paceFromProxy);
            PaceStart("the ghost proxy came first");   // 17.10.: the second death never reached the pace
        }
    }
    float proxyPos[4]{};
    Read(proxy + kEntityMatrix + 0x30, proxyPos, 16);
    const float* corpse = haveLastNecroMatrix ? lastNecroMatrix + 12 : nullptr;
    if (viewAtHuman && TicksSince(GetTickCount(), viewAtHumanAt) > kViewAtHumanMs) {
        viewAtHuman = false;
        InterlockedIncrement(&viewsAtHumanExpired);
    }
    if (vent && proxy && viewAtHuman) {
        viewAtHuman = false;
        alignas(16) float where[16]{};
        where[0] = where[5] = where[10] = where[15] = 1.0f;
        where[12] = viewHuman[0];
        where[13] = viewHuman[1] + kRoomViewUp;
        where[14] = viewHuman[2];
        __try { PlaceProxy(proxy, where); InterlockedIncrement(&viewsAtHuman); }
        __except (FaultFilter(GetExceptionInformation())) { InterlockedExchange(&lastError, 61); }
        Note("ghost proxy %08X: vent %08X is %s, the camera goes to partner %u at (%.1f %.1f %.1f) instead", proxy, vent, viewHumanWhy, viewHumanSlot,
             viewHuman[0], viewHuman[1], viewHuman[2]);
        return;
    }
    if (!vent) {
        // No vent in reach: the proxy would stay at the death camera, wherever
        // that is. Hover it over the corpse instead.
        InterlockedIncrement(&moveProxySkipped);
        if (proxy && haveLastNecroMatrix) {
            alignas(16) float where[16];
            std::memcpy(where, lastNecroMatrix, sizeof(where));
            where[13] += kRoomViewUp;   // world up
            __try { PlaceProxy(proxy, where); InterlockedIncrement(&moveProxyParked); }
            __except (FaultFilter(GetExceptionInformation())) { InterlockedExchange(&lastError, 60); }
        }
        Note("%sghost proxy %08X: no vent auto-selected, proxy was at (%.1f %.1f %.1f), %s", died ? "necro died; " : "", proxy,
             proxyPos[0], proxyPos[1], proxyPos[2], corpse ? "parked over the corpse" : "corpse position unknown");
        return;
    }
    uint32_t attributes = 0;
    alignas(16) float authored[4]{}, centroid[4]{};
    if (!Read(vent + kVentAttributes, &attributes, 4) || !attributes || !Read(attributes + kVentCameraPos, authored, 16) || !Read(vent + kVentCentroid, centroid, 16)) {
        InterlockedIncrement(&moveProxyPassed);
        Note("%sghost proxy %08X: vent %08X unreadable, mover left alone", died ? "necro died; " : "", proxy, vent);
        moveProxyHook.call<void>(proxy, vent);
        return;
    }
    float offset[3] = {authored[0] - centroid[0], authored[1] - centroid[1], authored[2] - centroid[2]};
    const float authoredDistance = std::sqrt(Dot3(offset, offset));
    // A campaign vent carries no room view: the field is all zero, the world origin, which sat
    // within 60 m of a ch02 vent (17.10.: the ghost camera hung 46.8 m away and never moved).
    const bool authoredZero = std::fabs(authored[0]) < 0.01f && std::fabs(authored[1]) < 0.01f && std::fabs(authored[2]) < 0.01f;
    if (authoredZero) InterlockedIncrement(&moveProxyZeroViews);
    if (!authoredZero && std::isfinite(authoredDistance) && authoredDistance > 0.5f && authoredDistance < 30.0f) {
        InterlockedIncrement(&moveProxyPassed);
        Note("%sghost proxy %08X -> vent %08X authored room view (%.1f %.1f %.1f), %.1f m from the vent", died ? "necro died; " : "",
             proxy, vent, authored[0], authored[1], authored[2], authoredDistance);
        moveProxyHook.call<void>(proxy, vent);
        return;
    }
    // Synthesize: world up (the level's Y; the necro matrix's row 1 put the
    // camera at floor height, "under the map" in the 12:37 test), towards the
    // corpse (or the proxy) with the up component removed.
    float up[3] = {0.0f, 1.0f, 0.0f};
    const float* from = roomViewAnchorSet ? roomViewAnchor : corpse ? corpse : proxyPos;
    roomViewAnchorSet = false;
    float toward[3] = {from[0] - centroid[0], from[1] - centroid[1], from[2] - centroid[2]};
    const float along = Dot3(toward, up);
    toward[0] -= up[0] * along; toward[1] -= up[1] * along; toward[2] -= up[2] * along;
    if (!Normalize3(toward)) {
        // Degenerate (corpse right under the vent): any horizontal direction.
        toward[0] = up[1]; toward[1] = -up[0]; toward[2] = 0.0f;
        if (!Normalize3(toward)) { toward[0] = 0.0f; toward[1] = -up[2]; toward[2] = up[1]; Normalize3(toward); }
    }
    // In front of the vent, not merely towards the corpse: the corpse can lie
    // beyond a wall (the vent picked is the nearest within the radius), and
    // the camera then sat inside the wall behind the vent. The vent entity
    // matrix's rows were no use either (an axis pick from them put the camera
    // under the floor in the 13:21 test). The vent's own exit transform is used:
    // VentMarker::GetEdgeMidPointMatrix (PC 0x012C8C80, thiscall(matrix out,
    // bool, bool, bool), what NecroPlayer::Spawn places the necro with); its
    // position is the exit edge, its forward row points the way the necro comes
    // out. Wall vent: 3 m out along the forward and 2 m above the centroid;
    // floor/ceiling vent (forward near vertical): 2.5 m along the forward from
    // the lower edge and 1.5 m sideways towards the corpse.
    // A vent has two exit sides (GetEdgeMidPointMatrix's second bool);
    // NecroRespawnProxySM picks the one whose exit forward lines up with the
    // ghost proxy's forward when the respawn is queued, so where this camera
    // stands and looks decides which side the necro comes out of. The engine's
    // mover then aims the camera at the vent.
    float front[3] = {toward[0], toward[1], toward[2]};
    float anchor[3] = {centroid[0], centroid[1], centroid[2]};
    int sideChosen = -1;
    {
        alignas(16) float edge[2][16];
        bool haveEdge[2] = {false, false};
        __try {
            using EdgeMatrix = void (__thiscall*)(uint32_t, float*, unsigned char, unsigned char, unsigned char);
            reinterpret_cast<EdgeMatrix>(base + kVentEdgeMatrixRva)(vent, edge[0], 1, 0, 0);
            haveEdge[0] = true;
            reinterpret_cast<EdgeMatrix>(base + kVentEdgeMatrixRva)(vent, edge[1], 1, 1, 0);
            haveEdge[1] = true;
        } __except (FaultFilter(GetExceptionInformation())) {}
        // The two matrices are the vent's edges along its first axis (top and
        // bottom of a wall vent), not two faces: the lower one is the anchor,
        // and the exit direction is that edge matrix's forward row (the way the
        // necro comes out). Trusted as is; the earlier corpse-based guesses put the camera
        // behind walls and under floors.
        float lowest = 3.4e38f;
        for (int side = 0; side < 2; ++side) {
            if (!haveEdge[side]) continue;
            if (edge[side][13] < lowest) {
                lowest = edge[side][13]; sideChosen = side;
                float forward[3] = {edge[side][8], edge[side][9], edge[side][10]};
                if (Normalize3(forward)) std::memcpy(front, forward, sizeof(front));
                anchor[0] = edge[side][12]; anchor[1] = edge[side][13]; anchor[2] = edge[side][14];
            }
        }
    }
    // A wall vent's edge sits above the centroid as well as in front of it, so
    // the wall/vertical decision uses the horizontal part of the exit: a clear
    // horizontal component means a wall vent and the camera stays level.
    float horizontal[3] = {front[0], 0.0f, front[2]};
    const bool verticalVent = !(std::sqrt(Dot3(horizontal, horizontal)) > 0.35f);
    if (!verticalVent) { Normalize3(horizontal); std::memcpy(front, horizontal, sizeof(front)); }
    // Like an MP room camera: in front of the vent and above its centroid,
    // looking down at it. NecroRespawnProxySM picks the spawn edge by matching
    // this camera's forward against the edge matrices' forwards, and the MP
    // necro leaves a wall vent from the upper edge; a camera looking down at
    // the vent is what the authored MP cameras do.
    alignas(16) float synthesized[4];
    float own[3]{};
    const bool haveNormal = VentNormal(vent, centroid, own);
    const bool wallByNormal = haveNormal && std::fabs(own[1]) < 0.5f;
    if (wallByNormal) {
        float out[3] = {own[0], 0.0f, own[2]};
        Normalize3(out);
        FrontSpot(centroid, out, 3.0f, 1.6f, synthesized);
        front[0] = out[0]; front[1] = 0.0f; front[2] = out[2];
    } else if (verticalVent) {
        synthesized[0] = anchor[0] + front[0] * 2.5f + toward[0] * 1.5f;
        synthesized[1] = anchor[1] + front[1] * 2.5f + toward[1] * 1.5f;
        synthesized[2] = anchor[2] + front[2] * 2.5f + toward[2] * 1.5f;
    } else {
        synthesized[0] = centroid[0] + front[0] * 3.0f + up[0] * 2.0f;
        synthesized[1] = centroid[1] + front[1] * 3.0f + up[1] * 2.0f;
        synthesized[2] = centroid[2] + front[2] * 3.0f + up[2] * 2.0f;
    }
    synthesized[3] = authored[3];
    if (!WriteBytes(attributes + kVentCameraPos, synthesized, 16)) {
        InterlockedIncrement(&moveProxyPassed);
        moveProxyHook.call<void>(proxy, vent);
        return;
    }
    InterlockedIncrement(&moveProxySynthesized);
    if (wallByNormal) InterlockedIncrement(&roomViewsOnNormal);
    lastRoomCamera[0] = synthesized[0]; lastRoomCamera[1] = synthesized[1]; lastRoomCamera[2] = synthesized[2];
    lastRoomCameraVent = vent;
    Note("%sghost proxy %08X -> vent %08X centroid (%.1f %.1f %.1f): authored (%.1f %.1f %.1f) replaced by (%.1f %.1f %.1f) [exit side %d, front %.2f %.2f %.2f%s]; proxy was at (%.1f %.1f %.1f), corpse %s(%.1f %.1f %.1f)",
         died ? "necro died; " : "", proxy, vent, centroid[0], centroid[1], centroid[2], authored[0], authored[1], authored[2],
         synthesized[0], synthesized[1], synthesized[2], sideChosen, front[0], front[1], front[2], wallByNormal ? ", wall: the vent normal" : "", proxyPos[0], proxyPos[1],
         proxyPos[2], corpse ? "" : "unknown ",
         corpse ? corpse[0] : 0.0f, corpse ? corpse[1] : 0.0f, corpse ? corpse[2] : 0.0f);
    moveProxyHook.call<void>(proxy, vent);
}
// The vent pick's confirm and what follows, traced. NecroRespawnProxySM on PC:
// CheckTransition 0x010A8000 (thiscall(uint, float, id, data), ret 0x10; ids
// 5 cancel, 6 accept -> respawnAttempt, 7 refused -> back, 8 accepted or 5 s
// timeout -> respawnFinalize, 9 type picked), HandleStateMessage 0x010DD650
// (thiscall(state, float, msg, arg, data), ret 0x14; msg 12 = respawnFinalize
// enter, which calls NecroPlayer::Spawn 0x010A5650 directly as cdecl(playerId,
// subkey, VentMarker*, type, bool, bool)). The accept is a controller event
// (0x0FC4C211, or 0xCCACDEE7 with the swapped layout) from the necro's
// controller config; [Necro] ConfirmKey is a keyboard stand-in for it.
// --- invasion: the humans' scenes, the ghost camera near them, doors --------------------------
// Story co-op with necro players (user, 16.10.; analysis/NECRO_GHOST_MODE_2026-10-16.md).
// - A human in a scene: a living necro goes back into the vents the native way MP's "re-enter
//   a vent" does (NecroPlayerMasterSM::CheckTransition 0x0108D990, id 0xD, forced true), and a
//   ghost cannot accept a vent until 2 s after the last scene signal.
// - The ghost camera: AutoSelectRespawnVent (0x010C2B20, cdecl(proxy)) found no human team in
//   the campaign and took the vent nearest the death spot. It now takes the nearest spawnable
//   vent (FindNearestVents 0x012FFB20, CanSpawnAtVent 0x01072B40) within GhostFollowRadius of
//   a human, in a loaded stream; when a human's completed stream set (his VISI room) changes,
//   the selection is dropped (SetCurrentlySelectedVent 0x0108F230, 0) in the proxy SM's own
//   update (message 6) so the engine picks again and moves the camera, at most every 2 s.
// - Doors count only Players and NPCs (StandardDoor::UpdateEntitiesInRange): a living necro
//   right in front of a closed, unlocked door opens it (TryToActivateDoor, as a partner's
//   replayed opening does); worldcalls keeps a door open while it stands in it.
bool invasion = true;
float ghostFollowRadius = 25.0f;
DWORD recentreCooldownMs = 2000;
constexpr uint32_t kAutoSelectRva = 0xCC2B20, kFindNearestVentsRva = 0xEFFB20, kCanSpawnAtVentRva = 0xC72B40, kSetSelectedVentRva = 0xC8F230;
constexpr uint32_t kRefreshVentsRva = 0xCA8280, kStreamFromHandleRva = 0xDA50, kMasterCheckRva = 0xC8D990, kProxySmVtableRva = 0x17785EC;
constexpr uint32_t kNecroIsDeadSlot = 0x200, kNecroIsDeadRva = 0xAB8CF0, kVentStreamHandle = 0x38;
constexpr uint32_t kDoorListRva = 0x1C5B744, kDoorListOffset = 0x5C;   // the reflectable list bot_sense walks (included after this file)
DWORD humanSceneAt = 0;
volatile LONG forceGhost = 0;
bool joinGhostWanted = false;   // joined as a necromorph: straight into the vents after the transform
volatile LONG forcedGhosts = 0, forceRefusedLocal = 0, forceRefusedGrab = 0, forceRefusedDead = 0, acceptsHeld = 0;
volatile LONG recentres = 0, roomChanges = 0, autoPicks = 0, autoPicksFar = 0, autoFallbacks = 0, ventsRejectedSpawn = 0, ventsRejectedStream = 0, doorsOpened = 0, invasionFaults = 0;
uint32_t humanRoom[coop_peers::kMaxPeers]{};
bool recentreWanted = false;
DWORD lastRecentre = 0;
unsigned anchorSlot = 0xFF;
// Nicole's fight (user, 17.10.: "make an exception for this finale area, necros may spawn there"):
// a human in a playable sequence (scene kind 5: a sequence on Isaac while his master state is
// main, the strait jacket, dementia nav help). It is no scene for the necros: no hold, no forced
// vent re-entry, no watch. (Before the scene kinds: a timer of 40 s, then 40 s plus 25 m walked;
// both took the Tiedemann ending for the fight.)
bool finaleSeen = false;
volatile LONG finaleWatchSkips = 0, finaleLongScenes = 0;
bool FinaleArena(DWORD now) {
    bool playable = false;
    if (invasion && !play::VersusMode() && inLevel)
        for (unsigned s = 0; s < coop_peers::kMaxPeers && !playable; ++s)
            playable = peerTable.peers[s].used && !roles::IsNecro(s, now) && scenekind::PartnerKind(s, now) == scenekind::kPlayable;
    if (playable != finaleSeen) {
        if (playable) InterlockedIncrement(&finaleLongScenes);
        gate::Event("test finale INFO: %s", playable ? "a human plays a playable sequence: necros may spawn" : "the playable sequence ended");
        finaleSeen = playable;
    }
    return playable;
}
// A human's scene by its kind (runtime_coop_scenekind.inc): a cinematic, a short sequence, a QTE or
// an aiming part holds the necros back, a playable sequence does not. Without a kind from him
// (an older build) his sequence report decides, as before.
bool HumanInScene(DWORD now) {
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        if (!peer.used || !peer.token || roles::IsNecro(s, now)) continue;
        const uint32_t kind = scenekind::PartnerKind(s, now);
        if (kind != scenekind::kUnknown ? scenekind::HoldsNecros(kind) : cutscene::PartnerInScene(s, now)) return true;
    }
    return false;
}
// A QTE scene runs as several sequences with short gaps (16.10.: a ghost accepted a vent in one).
constexpr DWORD kSceneHoldMs = 4000;
bool SceneHold(DWORD now) { return invasion && humanSceneAt && now - humanSceneAt < kSceneHoldMs && !FinaleArena(now); }
// The respawn pace ([Necro] RespawnBaseSeconds): after a death the next vent opens
// base x necros / humans seconds later, at most RespawnMaxSeconds. One necro against
// three humans is back in a third of the time, two necros against one wait twice as long.
DWORD paceBaseMs = 8000, paceMaxMs = 30000, paceWaitMs = 0;
unsigned paceHumans = 0, paceNecros = 0;
volatile LONG paceHeld = 0, paceDeaths = 0, paceDone = 0;
void PaceCount(DWORD now, unsigned& humans, unsigned& necros) {
    humans = necros = 0;
    (LocalRole() == 0 ? humans : necros)++;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        if (!peerTable.peers[s].used || !peerTable.peers[s].token) continue;
        if (roles::Spectating(s, now)) continue;   // a dead human watching is neither
        (roles::IsNecro(s, now) ? necros : humans)++;
    }
}
DWORD PaceWait(DWORD now) {
    unsigned humans = 0, necros = 0;
    PaceCount(now, humans, necros);
    const DWORD wait = static_cast<DWORD>(static_cast<unsigned long long>(paceBaseMs) * (necros ? necros : 1) / (humans ? humans : 1));
    return wait < paceMaxMs ? wait : paceMaxMs;
}
DWORD PaceLeft(DWORD now) {
    if (!invasion || !paceBaseMs || !necroDiedAt || play::VersusMode()) return 0;
    const DWORD wait = PaceWait(now), gone = now - necroDiedAt;
    return gone < wait ? wait - gone : 0;
}
// Once per death, from whichever sign of it comes first.
void PaceStart(const char* how) {
    const DWORD now = GetTickCount();
    if (necroDiedAt && now - necroDiedAt < 5000) return;
    necroDiedAt = Stamp(now);
    PaceCount(necroDiedAt, paceHumans, paceNecros);
    paceWaitMs = PaceWait(necroDiedAt);
    InterlockedIncrement(&paceDeaths);
    if (paceBaseMs && invasion && !play::VersusMode())
        Note("respawn pace (%s): %u human(s), %u necro(s): the next vent opens in %lu ms", how, paceHumans, paceNecros, static_cast<unsigned long>(paceWaitMs));
}
void PaceTick(DWORD now) {
    const DWORD left = PaceLeft(now);
    if (!left || LocalRole() != 2) return;
    char line[48];
    std::snprintf(line, sizeof(line), "RESPAWN IN %lu", static_cast<unsigned long>((left + 999) / 1000));
    hud::SetOwn(line);
}
// The necro key (or a necro join) during a human's scene: the necromorph comes after it.
bool transformAfterScene = false;
volatile LONG transformsHeld = 0;
// A necro player watches the humans' scenes as Isaac (user, 16.10.; analysis/NECRO_SCENE_WATCH_2026-10-16.md):
// a partner's scene cue on this machine asks for the watch (the mirror queues the cue), Isaac
// comes back where the scene's author stood a second ago, follows the scene as a human
// follower, and at its real end the machine goes back into the vents. Role 3 the whole time,
// so the humans hide every body of this machine.
// Confirming: a partner's ScriptedSequence cue came, but most of them are world events (an enemy's
// entrance, a scripted animation) that never take his Isaac (17.10.: five watches, every one ended
// with no scene here). Only when his own scene signal (the VISI sequence age, cutscene::PartnerInScene)
// follows within kWatchConfirmMs does the necro turn into Isaac; otherwise he stays and the held
// cues are dropped, as before the watch existed.
enum class Watch { None, Confirming, Wanted, Watching, Returning };
Watch watch = Watch::None;
// A stamp taken later than the caller's now (a hook or a fresh GetTickCount) counts as 0 ms old.
DWORD Age(DWORD now, DWORD at) { return static_cast<LONG>(now - at) > 0 ? now - at : 0; }
constexpr DWORD kWatchConfirmMs = 2500;
DWORD watchConfirmSince = 0;
volatile LONG watchConfirmed = 0, watchUnconfirmed = 0, watchShortWaits = 0;
// A ghost at the vents beside a human who sits in a watchable scene without a cue (17.10.: the
// cinematic, QTE and aiming parts ran for minutes and the ghost waited at the vents) watches
// from the kind alone once it has lasted kKindWatchMs.
constexpr DWORD kKindWatchMs = 2000;
DWORD kindWatchSince[coop_peers::kMaxPeers]{};
volatile LONG watchFromKind = 0;
// A watch that saw no scene here (or failed) sits the rest of that scene out: no new watch while
// the humans' scene signal lasts, the old way back into the vents instead.
bool sitOut = false;
DWORD sceneSignalSince = 0, sceneQuietSince = 0;
volatile LONG watchSatOut = 0, watchWaitedForGhost = 0;
bool watchScenes = true;   // [Necro] WatchScenes
// [Necro] WatchSuit, off (17.10. ~19:05, user: "since the leader's suit the whole scene watch is
// broken"): the respawned Isaac's body never counted as whole, so every watch waited the full 2.5 s
// and the scene's cues played about 3 s late (2953 and 3203 ms after they came, 330 ms before).
bool watchSuit = false;
unsigned watchAuthor = 0xFF;
DWORD watchWantedAt = 0, watchSince = 0, watchCueAt = 0, watchFollowSeenAt = 0, returnSince = 0, returnTryAt = 0;
// 16.10. (Tormentor): a re-entry into the vents and the way back to Isaac 300 ms later faulted
// inside the respawn (0x0084A9D4) and the game died a moment later; the scene's gaps were longer
// than 4 s and its later cues started nothing on the watcher, so he went back and forth seven times.
constexpr DWORD kGhostSettleMs = 2500, kSitOutQuietMs = 3000, kForceAfterSignalMs = 3000;
constexpr DWORD kWatchSettleMs = 300, kWatchWaitMs = 6000, kWatchEndQuietMs = 8000, kWatchMinMs = 3000, kWatchNoSceneMs = 6000,
                kWatchMaxMs = 300000, kReturnGiveUpMs = 10000;
volatile LONG watchAsks = 0, watchStarts = 0, watchFromGhost = 0, watchFromLiving = 0, watchRefused = 0, watchRefusedGrab = 0,
              watchRefusedSpawning = 0, watchRefusedRestart = 0, watchRefusedNoSpot = 0, watchRefusedFault = 0, watchEnds = 0,
              watchEndsNoScene = 0, watchEndsTimeout = 0, watchReturns = 0, watchReturnFailures = 0, watchKeyIgnored = 0;
// Where each human stood, every 250 ms for two seconds: the watcher's Isaac takes a spot the
// author has just walked through (loaded, walkable, in his stream set).
struct Trail { float pos[8][3]; DWORD at[8]; unsigned next; };
Trail trails[coop_peers::kMaxPeers]{};
DWORD trailSampleAt = 0;
void SampleTrails(DWORD now) {
    if (now - trailSampleAt < 250) return;
    trailSampleAt = now;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& runtime = peerRuntime[s];
        if (!peerTable.peers[s].used || !runtime.lastState || now - runtime.lastState > 1000) continue;
        Trail& t = trails[s];
        std::memcpy(t.pos[t.next % 8], runtime.pos, 12);
        t.at[t.next % 8] = now;
        ++t.next;
    }
}
// The newest trail point at least 1 m from where he stands now, else where he stands.
bool WatchSpot(unsigned slot, DWORD now, float* out) {
    if (slot >= coop_peers::kMaxPeers) return false;
    const auto& runtime = peerRuntime[slot];
    if (!runtime.lastState || now - runtime.lastState > 2000) return false;
    const Trail& t = trails[slot];
    for (unsigned k = 1; k <= 8 && k <= t.next; ++k) {
        const unsigned i = (t.next - k) % 8;
        if (!t.at[i] || now - t.at[i] > 2500) break;
        const float dx = t.pos[i][0] - runtime.pos[0], dy = t.pos[i][1] - runtime.pos[1], dz = t.pos[i][2] - runtime.pos[2];
        if (dx * dx + dy * dy + dz * dz >= 1.0f && dy * dy < 4.0f) { std::memcpy(out, t.pos[i], 12); return true; }
    }
    std::memcpy(out, runtime.pos, 12);
    return true;
}
bool RestartOrLevelChange() {
    const uint32_t loop = level::GameMainLoop();
    unsigned char changing = 0;
    uint32_t restart = 0;
    return loop && ((Read(loop + 0x209, &changing, 1) && changing) || (Read(loop + 0x210, &restart, 4) && restart));
}
// The mirror asks with a partner's scene cue (game thread). True: the cue is queued and
// replayed once Isaac stands here; false: skipped, as before.
bool AskWatch(DWORD now, unsigned author) {
    if (!enabled || !invasion || !watchScenes || play::VersusMode() || !inLevel) return false;
    if (watch == Watch::Watching || watch == Watch::Wanted || watch == Watch::Confirming) { watchCueAt = Stamp(now); return true; }
    if (watch == Watch::Returning) return false;   // on the way back: the next cue asks again once the ghost is up
    if (!necroEntity && !playerGone) return false;
    if (FinaleArena(now)) { InterlockedIncrement(&finaleWatchSkips); return false; }   // every finale scene is its player's own
    InterlockedIncrement(&watchAsks);
    if (sitOut) { InterlockedIncrement(&watchSatOut); return false; }
    if (RestartOrLevelChange()) { InterlockedIncrement(&watchRefused); InterlockedIncrement(&watchRefusedRestart); return false; }
    watchAuthor = author;
    watchWantedAt = watchCueAt = Stamp(now);
    // His kind first, as in Confirming: a short sequence with a fresh VISI sequence age is no cinematic yet.
    const uint32_t kind = scenekind::PartnerKind(author, now);
    if (kind != scenekind::kUnknown ? !scenekind::Watchable(kind) : !cutscene::PartnerInScene(author, now)) {
        watch = Watch::Confirming;
        watchConfirmSince = Stamp(now);
        gate::Event("necro watch: partner %u sent a sequence cue (his kind: %s); waiting up to %lu ms for his own scene before turning into Isaac", author,
                    kind < scenekind::kKinds ? scenekind::kNames[kind] : "unknown", static_cast<unsigned long>(kWatchConfirmMs));
        return true;
    }
    watch = Watch::Wanted;
    gate::Event("necro watch: partner %u's scene (kind %s), %s turns back into Isaac to follow it", author, kind < scenekind::kKinds ? scenekind::kNames[kind] : "unknown",
                necroEntity ? "the living necro" : "the ghost at the vents");
    return true;
}
// Watching too (17.10. 17:40: three cues came while Isaac stood but was not ready yet, his net
// proxy still flagged or his suit changing, and were dropped at once; the watch then ended
// after 6 s without a scene and he went back into the vents mid-scene).
bool WatchCuesHeld() { return watch == Watch::Wanted || watch == Watch::Confirming || watch == Watch::Watching; }
// The scene's Isaac wears its author's suit (user, 17.10.: the necro watched every scene in the
// patient suit the respawned body comes in). One native ChangeToUniform once the body is whole;
// the scene's cues wait for it, 2.5 s per step at most.
enum class WatchDress { Done, Pending, Issued };
WatchDress watchDress = WatchDress::Done;
DWORD watchDressSince = 0;
volatile LONG watchDressed = 0, watchDressKept = 0, watchDressSkipped = 0, watchDressTimeouts = 0;
bool WatchReady() { return watch == Watch::Watching && IsPlayer(coop::LocalSlotZeroEntity()) && watchDress == WatchDress::Done; }
const char* WatchWaitReason() {
    if (watch != Watch::Watching) return watch == Watch::Returning ? "the watch is over" : "Isaac is not back yet";
    if (!IsPlayer(coop::LocalSlotZeroEntity())) return "Isaac stands but slot 0 is not his yet";
    if (watchDress != WatchDress::Done) return "his suit is changing";
    return "ready";
}
bool WatchBlocks() { return watch == Watch::Watching; }   // input and damage between the scene's parts
bool WatchActive() { return watch != Watch::None; }
void DropWatch(const char* why, volatile LONG* counter) {
    InterlockedIncrement(&watchRefused);
    if (counter) InterlockedIncrement(counter);
    gate::Event("necro watch: dropped (%s); the rest of this scene is sat out", why);
    watch = Watch::None;
    watchAuthor = 0xFF;
    sitOut = true;
}
bool HumanPos(unsigned slot, DWORD now, float* out) {
    if (slot >= coop_peers::kMaxPeers) return false;
    const auto& peer = peerTable.peers[slot];
    const auto& runtime = peerRuntime[slot];
    if (!peer.used || !peer.token || roles::IsNecro(slot, now) || !runtime.lastState || now - runtime.lastState > 5000) return false;
    std::memcpy(out, runtime.pos, 12);
    return true;
}
// A human's scripted teleport (runtime_coop_teleport.inc: his machine saw Entity::SetTeleporting on
// its Isaac; 17.10.: ch11's Tiedemann scene and Nicole's arena put Isaac far away and the vent
// camera stayed behind). The ghost camera follows at once, as for a room change, without the
// recentre cooldown, and aims at the destination until his state packets show him there.
float teleportAnchor[3]{};
DWORD teleportAnchorAt = 0;
volatile LONG humanTeleports = 0, teleportRecentres = 0, teleportsByEvent = 0, teleportsByJump = 0, teleportLines = 0;
// Without an event the old rule stands in (17.10.: the host's hook saw four SetTeleporting calls,
// none on his Isaac, and the vent camera stayed behind at Nicole's arena): a human more than
// kJumpM from where he stood at most kJumpWindowMs ago was moved. Sampled every 250 ms.
constexpr float kJumpM = 25.0f;
constexpr DWORD kJumpWindowMs = 1500, kJumpEventCoversMs = 3000;
float jumpFrom[coop_peers::kMaxPeers][3]{};
DWORD jumpFromAt[coop_peers::kMaxPeers]{}, teleportEventAt[coop_peers::kMaxPeers]{};
uint32_t jumpToken[coop_peers::kMaxPeers]{};
void WatchTeleports(DWORD now, bool ghost) {
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        float p[3]{};
        float jumped = 0.0f;
        const char* how = nullptr;
        const bool human = !roles::IsNecro(s, now);
        if (teleport::TakePartner(s, p)) {
            if (!human) continue;
            how = "his teleport event";
            teleportEventAt[s] = Stamp(now);
            InterlockedIncrement(&teleportsByEvent);
            if (jumpFromAt[s]) {
                const float dx = p[0] - jumpFrom[s][0], dy = p[1] - jumpFrom[s][1], dz = p[2] - jumpFrom[s][2];
                jumped = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
        } else {
            const auto& peer = peerTable.peers[s];
            const auto& runtime = peerRuntime[s];
            if (!peer.used || !peer.token || jumpToken[s] != peer.token) { jumpToken[s] = peer.used ? peer.token : 0; jumpFromAt[s] = 0; continue; }
            if (!human || !runtime.lastState || Age(now, runtime.lastState) > 1000) continue;
            if (jumpFromAt[s] && Age(now, jumpFromAt[s]) <= kJumpWindowMs) {
                const float dx = runtime.pos[0] - jumpFrom[s][0], dy = runtime.pos[1] - jumpFrom[s][1], dz = runtime.pos[2] - jumpFrom[s][2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > kJumpM * kJumpM && !(teleportEventAt[s] && Age(now, teleportEventAt[s]) < kJumpEventCoversMs)) {
                    std::memcpy(p, runtime.pos, 12);
                    jumped = std::sqrt(d2);
                    how = "a position jump, no teleport event";
                    InterlockedIncrement(&teleportsByJump);
                }
            }
            if (!jumpFromAt[s] || Age(now, jumpFromAt[s]) >= 250 || how) { std::memcpy(jumpFrom[s], runtime.pos, 12); jumpFromAt[s] = Stamp(now); }
            if (!how) continue;
        }
        InterlockedIncrement(&humanTeleports);
        if (ghost) {
            recentreWanted = true;
            anchorSlot = s;
            lastRecentre = 0;
            std::memcpy(teleportAnchor, p, 12);
            teleportAnchorAt = Stamp(now);
            InterlockedIncrement(&teleportRecentres);
        }
        if (InterlockedIncrement(&teleportLines) <= 60)
            gate::Event("necro invasion: partner %u was teleported to (%.1f %.1f %.1f), seen by %s (moved %.0f m)%s", s, p[0], p[1], p[2], how, jumped,
                        ghost ? ": the ghost camera follows" : "");
    }
}
// A human's room: the stream set his VISI packet says he completed last.
void WatchHumanRooms(DWORD now, bool ghost) {
    WatchTeleports(now, ghost);
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& v = streams::vis::peers[s];
        if (!v.at || TicksSince(now, v.at) > streams::vis::kFreshMs || roles::IsNecro(s, now)) continue;
        const uint32_t room = v.packet.area[0] ^ (v.packet.area[1] * 33u) ^ (v.packet.area[2] * 1089u) ^ v.packet.area[3];
        if (!room) continue;
        if (humanRoom[s] && humanRoom[s] != room && ghost) {
            recentreWanted = true;
            anchorSlot = s;
            InterlockedIncrement(&roomChanges);
            Note("invasion: partner %u went into another room (set %08X-%08X): the ghost camera follows", s, v.packet.area[0], v.packet.area[1]);
        }
        humanRoom[s] = room;
    }
}
__declspec(noinline) unsigned FindVentsRaw(uint32_t* out, unsigned max, const float* pos, float maxDistSq) {
    __try {
        using Find = unsigned(__cdecl*)(uint32_t*, unsigned, const float*, float, int, int);
        return reinterpret_cast<Find>(base + kFindNearestVentsRva)(out, max, pos, maxDistSq, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&invasionFaults);
        return 0;
    }
}
safetyhook::InlineHook canSpawnHook;
__declspec(noinline) bool CanSpawnRaw(uint32_t vent) {
    __try {
        if (canSpawnHook) return canSpawnHook.call<bool>(vent);
        return reinterpret_cast<bool(__cdecl*)(uint32_t)>(base + kCanSpawnAtVentRva)(vent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&invasionFaults);
        return false;
    }
}
// An entity's stream (+0x38, as on the vents): 1 loaded and shown, 0 loaded but hidden or not
// loaded, -1 unknown (no handle, or the manager does not know it).
__declspec(noinline) int EntityStreamShown(uint32_t entity) {
    uint32_t handle = 0, manager = 0, stream = 0, status = 0, flags = 0;
    if (!entity || !Read(entity + kVentStreamHandle, &handle, 4) || !handle || !Read(base + kStreamManager, &manager, 4) || !manager) return -1;
    __try {
        stream = reinterpret_cast<uint32_t(__thiscall*)(uint32_t, uint32_t)>(base + kStreamFromHandleRva)(manager, handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (!stream || !Read(stream + 0x24, &status, 4) || !Read(stream + 0x54, &flags, 4)) return -1;
    return status == 3 && (flags & 0x100) != 0 ? 1 : 0;
}
// The vent's stream is loaded and shown here (status 3, flag 0x100); unknown counts as yes.
__declspec(noinline) bool VentStreamShown(uint32_t vent) {
    uint32_t handle = 0, manager = 0, stream = 0, status = 0, flags = 0;
    if (!Read(vent + kVentStreamHandle, &handle, 4) || !handle || !Read(base + kStreamManager, &manager, 4) || !manager) return true;
    __try {
        stream = reinterpret_cast<uint32_t(__thiscall*)(uint32_t, uint32_t)>(base + kStreamFromHandleRva)(manager, handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    if (!stream || !Read(stream + 0x24, &status, 4) || !Read(stream + 0x54, &flags, 4)) return true;
    return status == 3 && (flags & 0x100) != 0;
}
// Every vent of a campaign level sits in the level's root stream (ch05_sol\ch05_sol, 17.10. dump
// 39376: both vents of the test there), so it exists whether its room is loaded or not and the
// stream test above passes them all (user: "vents in unloaded rooms can be picked"). A vent counts
// as in a room here when a human stands within VentHumanRange of it and the room's collision is
// around it: of six casts along the world axes from just above its centroid, three hit within
// 12 m. An unloaded room has no collision. Cached per vent for 1.5 s (the considered list asks often).
// anyRange: the room alone decides (the auto pick's last pass, when no vent is near a human).
float ventHumanRange = 45.0f;
struct VentProbe { uint32_t vent; DWORD at; float nearestSq; signed char room; };   // room: 1 shown, 0 not, -1 not probed
VentProbe ventProbes[32]{};
unsigned ventProbeNext = 0;
volatile LONG ventProbesRun = 0, ventProbesEmpty = 0, ventsTooFar = 0, ventProbeLines = 0, ventFrontHidden = 0, ventHitsHidden = 0, ventHitsUnknown = 0;
bool VentRoomHere(uint32_t vent, DWORD now, bool anyRange = false) {
    const float rangeSq = ventHumanRange * ventHumanRange;
    VentProbe* cached = nullptr;
    for (VentProbe& p : ventProbes)
        if (p.vent == vent && p.at && Age(now, p.at) < 1500) { cached = &p; break; }
    if (cached) {
        if (!anyRange && cached->nearestSq > rangeSq) return false;
        if (cached->room >= 0) return cached->room != 0;
    }
    float centroid[3]{};
    if (!Read(vent + kVentCentroid, centroid, 12) || !std::isfinite(centroid[0]) || !std::isfinite(centroid[1]) || !std::isfinite(centroid[2])) return true;
    float nearest = 3.4e38f;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        float h[3]{};
        if (!HumanPos(s, now, h)) continue;
        const float dx = h[0] - centroid[0], dy = h[1] - centroid[1], dz = h[2] - centroid[2], d = dx * dx + dy * dy + dz * dz;
        if (d < nearest) nearest = d;
    }
    const bool inRange = nearest <= rangeSq;
    bool ok = inRange || anyRange;
    bool probed = false;
    unsigned hits = 0;
    if (!ok) {
        InterlockedIncrement(&ventsTooFar);
    } else if (bot_sense::losReady) {
        probed = true;
        InterlockedIncrement(&ventProbesRun);
        // From 0.6 m out of the opening (the vent normal at +0x310): back onto the surface the
        // vent sits in (there only when the room is loaded), then the six world axes.
        float normal[3]{};
        Read(vent + 0x310, normal, 12);
        if (!Normalize3(normal)) { normal[0] = 0.0f; normal[1] = 1.0f; normal[2] = 0.0f; }
        alignas(16) float from[4] = {centroid[0] + normal[0] * 0.6f, centroid[1] + normal[1] * 0.6f, centroid[2] + normal[2] * 0.6f, 1.0f};
        // Collision is no proof (user, 17.10.: a room that was not shown still had it): a room
        // kept loaded but hidden collides as well. So every hit's own stream must be shown here,
        // and the room in front of the opening, where the necro climbs out, decides.
        unsigned shown = 0, hidden = 0, unknown = 0;
        const auto cast = [&](const float* start, const float* dir, float length, int& visible) {
            alignas(16) float a[4] = {start[0], start[1], start[2], 1.0f};
            alignas(16) float to[4] = {start[0] + dir[0] * length, start[1] + dir[1] * length, start[2] + dir[2] * length, 1.0f};
            float fraction = 1.0f;
            uint32_t hit = 0;
            visible = -1;
            if (!bot_sense::CastRaw(a, to, 0, fraction, hit) || fraction >= 0.999f) return false;
            visible = EntityStreamShown(hit);
            (visible == 1 ? shown : visible == 0 ? hidden : unknown)++;
            return true;
        };
        int visible = -1;
        const float back[3] = {-normal[0], -normal[1], -normal[2]};
        const bool mounted = cast(from, back, 1.5f, visible) && visible != 0;
        static const float kAxes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const auto& a : kAxes) if (cast(from, a, 12.0f, visible) && visible != 0) ++hits;
        // in front of the opening: the floor below the way out and the wall it faces
        const float front[3] = {centroid[0] + normal[0] * 1.2f, centroid[1] + normal[1] * 1.2f, centroid[2] + normal[2] * 1.2f};
        static const float kDown[3] = {0.0f, -1.0f, 0.0f};
        int frontDown = -1, frontAhead = -1;
        const bool downHit = cast(front, kDown, 6.0f, frontDown);
        const bool aheadHit = cast(front, normal, 8.0f, frontAhead);
        const bool frontHidden = (downHit && frontDown == 0) || (aheadHit && frontAhead == 0);
        const bool frontShown = (downHit && frontDown != 0) || (aheadHit && frontAhead != 0);
        ok = !frontHidden && (frontShown || mounted || hits >= 3);
        InterlockedExchangeAdd(&ventHitsHidden, static_cast<LONG>(hidden));
        InterlockedExchangeAdd(&ventHitsUnknown, static_cast<LONG>(unknown));
        if (frontHidden) InterlockedIncrement(&ventFrontHidden);
        if (mounted) hits += 10;   // in the log: 1x = the surface behind the vent was there
        if (frontShown) hits += 100;
        if (!ok) InterlockedIncrement(&ventProbesEmpty);
        if (InterlockedCompareExchange(&ventProbeLines, 0, 0) < 40)
            Note("vent %08X: hit streams shown=%u hidden=%u unknown=%u, in front of it down=%d ahead=%d (1 shown, 0 hidden, -1 unknown)%s", vent, shown, hidden,
                 unknown, downHit ? frontDown : 9, aheadHit ? frontAhead : 9, frontHidden ? ": the room in front is NOT shown here" : "");
    }
    if (InterlockedIncrement(&ventProbeLines) <= 40)
        Note("vent %08X at (%.1f %.1f %.1f): nearest human %.1f m, room collision %u (tens: the surface behind it, ones: of 6 axis casts) -> %s%s", vent, centroid[0], centroid[1], centroid[2],
             nearest < 3.0e38f ? std::sqrt(nearest) : -1.0f, hits, ok ? "selectable" : "NOT selectable", inRange ? "" : " (far from every human)");
    const VentProbe entry{vent, Stamp(now), nearest, static_cast<signed char>(probed ? (ok ? 1 : 0) : (inRange ? 1 : -1))};
    if (cached) *cached = entry;
    else ventProbes[ventProbeNext++ % std::size(ventProbes)] = entry;
    return ok;
}
// A vent whose room this machine does not have is no vent for a necro at the vents (user, 16.10.,
// 17.10.): CanSpawnAtVent (0x01072B40, cdecl(vent), plain ret) is what the vent cycling, the
// considered list and the game's own pick ask.
// 17.10. (user: "the necro can barely do anything"): with every vent refused, the vent the game
// had selected failed its own check every frame, AutoSelectRespawnVent ran again and again (23653
// fallbacks), the camera jumped to its pick and the spawn key found no vent. So the vent the auto
// pick handed out (ours, or the game's own when none of ours qualifies) stays allowed, and the
// game's own pick runs unfiltered.
volatile LONG ventsRefusedHidden = 0, keptVentPasses = 0, gamePicks = 0, gamePicksEmpty = 0;
uint32_t keptVent = 0;
bool gamePickRunning = false;
bool __cdecl OnCanSpawnAtVent(uint32_t vent) {
    const bool ok = canSpawnHook.call<bool>(vent);
    if (!ok || !invasion || play::VersusMode() || LocalRole() != 2 || gamePickRunning) return ok;
    if (vent == keptVent) { InterlockedIncrement(&keptVentPasses); return ok; }
    if (VentStreamShown(vent) && VentRoomHere(vent, GetTickCount())) return ok;
    InterlockedIncrement(&ventsRefusedHidden);
    return false;
}
safetyhook::InlineHook autoSelectHook;
// One auto pick note per two seconds: the game asks again whenever its vent fails.
DWORD autoNoteAt = 0;
bool AutoNoteQuiet(DWORD now) {
    if (autoNoteAt && now - autoNoteAt < 2000) return true;
    autoNoteAt = Stamp(now);
    return false;
}
// 17.10. (user): when the vent selection starts and the human's room has no vent, or no vent is
// within GhostHumanViewRange of him, the camera goes to the human; the vent stays selected. A room
// is the stream (+0x38) of the floor: under the human, and in front of the vent's opening, where
// the necro climbs out. 0 = off (the old rule: only a last-pass vent beyond VentHumanRange).
float humanViewRange = 50.0f;
volatile LONG humanViewsFar = 0, humanViewsOtherRoom = 0, humanRoomsUnknown = 0, ventRoomsSame = 0, ventRoomsOther = 0, ventRoomsUnknown = 0, ventRoomLines = 0,
              sameRoomPreferred = 0;
uint32_t FloorStream(const float* p, float up, uint32_t exclude) {
    if (!bot_sense::losReady) return 0;
    alignas(16) float a[4] = {p[0], p[1] + up, p[2], 1.0f};
    alignas(16) float b[4] = {p[0], p[1] + up - 6.0f, p[2], 1.0f};
    float fraction = 1.0f;
    uint32_t hit = 0, handle = 0;
    if (!bot_sense::CastRaw(a, b, exclude, fraction, hit) || fraction >= 0.999f || !hit || !Read(hit + kVentStreamHandle, &handle, 4)) return 0;
    return handle;
}
// 1 the vent's floor is in the human's stream, 0 in another, -1 unknown. Cached 1.5 s.
struct VentRoomKey { uint32_t vent, hisRoom; DWORD at; signed char same; };
VentRoomKey ventRoomKeys[16]{};
unsigned ventRoomNext = 0;
int VentInRoom(uint32_t vent, uint32_t hisRoom, DWORD now) {
    for (const VentRoomKey& k : ventRoomKeys)
        if (k.vent == vent && k.hisRoom == hisRoom && k.at && Age(now, k.at) < 1500) return k.same;
    float centroid[3]{}, normal[3]{};
    uint32_t ventRoom = 0;
    if (Read(vent + kVentCentroid, centroid, 12) && std::isfinite(centroid[0]) && std::isfinite(centroid[1]) && std::isfinite(centroid[2])) {
        Read(vent + 0x310, normal, 12);
        if (!Normalize3(normal)) { normal[0] = 0.0f; normal[1] = 1.0f; normal[2] = 0.0f; }
        const float front[3] = {centroid[0] + normal[0] * 1.2f, centroid[1] + normal[1] * 1.2f, centroid[2] + normal[2] * 1.2f};
        ventRoom = FloorStream(front, 0.0f, 0);
    }
    const int same = ventRoom ? (ventRoom == hisRoom ? 1 : 0) : -1;
    InterlockedIncrement(same == 1 ? &ventRoomsSame : same == 0 ? &ventRoomsOther : &ventRoomsUnknown);
    if (InterlockedIncrement(&ventRoomLines) <= 30)
        Note("vent %08X: floor stream %08X in front of it, the human's %08X -> %s", vent, ventRoom, hisRoom,
             same == 1 ? "his room" : same == 0 ? "ANOTHER room" : "unknown (counts as his room)");
    ventRoomKeys[ventRoomNext++ % std::size(ventRoomKeys)] = VentRoomKey{vent, hisRoom, Stamp(now), static_cast<signed char>(same)};
    return same;
}
uint32_t TakeAutoVent(uint32_t proxy, uint32_t vent, unsigned pass, unsigned slot, const float* at, unsigned n, int room, DWORD now) {
    float centroid[3]{};
    Read(vent + kVentCentroid, centroid, 12);
    const float dx = centroid[0] - at[0], dy = centroid[1] - at[1], dz = centroid[2] - at[2], d = std::sqrt(dx * dx + dy * dy + dz * dz);
    InterlockedIncrement(pass ? &autoPicksFar : &autoPicks);
    std::memcpy(roomViewAnchor, at, 12);
    roomViewAnchor[1] += 1.6f;
    roomViewAnchorSet = !pass;   // a far vent keeps its own room view
    const bool otherRoom = humanViewRange > 0.0f && room == 0;
    const bool tooFar = humanViewRange > 0.0f ? d > humanViewRange : pass == 2 && d > ventHumanRange;
    const bool toHuman = otherRoom || tooFar || (pass == 2 && d > ventHumanRange);
    if (vent != keptVent || !AutoNoteQuiet(now))
        Note("invasion: ghost proxy %08X -> vent %08X %.0f m from partner %u at (%.1f %.1f %.1f), %u candidate(s)%s%s", proxy, vent, d, slot, at[0], at[1], at[2], n,
             pass == 2 ? ", none near a human (the nearest with its room shown)" : pass ? ", none within the radius" : "",
             !toHuman ? "" : otherRoom ? "; not in his room: the camera goes to him" : "; far from him: the camera goes to him");
    keptVent = vent;
    if (toHuman) {
        viewAtHuman = true;
        viewAtHumanAt = Stamp(now);
        std::memcpy(viewHuman, at, 12);
        viewHumanSlot = slot;
        viewHumanWhy = otherRoom ? "not in the human's room" : "far from the human";
        InterlockedIncrement(otherRoom ? &humanViewsOtherRoom : &humanViewsFar);
    }
    return vent;
}
uint32_t __cdecl OnAutoSelectVent(uint32_t proxy) {
    const DWORD now = GetTickCount();
    if (invasion && !play::VersusMode() && peerTable.Count()) {
        float at[3]{};
        unsigned slot = anchorSlot;
        bool have = false;
        if (teleportAnchorAt && now - teleportAnchorAt < 3000) {
            std::memcpy(at, teleportAnchor, 12);
            have = true;
        } else {
            teleportAnchorAt = 0;
            have = HumanPos(slot, now, at);
        }
        if (!have) {
            float from[3]{}, best = 3.4e38f;
            Read(proxy + kEntityMatrix + 0x30, from, 12);
            for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
                float p[3]{};
                if (!HumanPos(s, now, p)) continue;
                const float d = (p[0] - from[0]) * (p[0] - from[0]) + (p[1] - from[1]) * (p[1] - from[1]) + (p[2] - from[2]) * (p[2] - from[2]);
                if (d < best) { best = d; slot = s; std::memcpy(at, p, 12); have = true; }
            }
        }
        if (have) {
            alignas(16) float pos[4] = {at[0], at[1] + 1.0f, at[2], 1.0f};
            // Within the radius first; a room without vents (16.10.) then takes the nearest shown
            // vent anywhere (the list comes sorted by distance) before the game's pick near the corpse.
            // Pass 3: no vent near any human (a room without vents and none within VentHumanRange),
            // so the nearest one whose own room is shown here, however far.
            const float reaches[3] = {ghostFollowRadius * ghostFollowRadius, 3.0e10f, 3.0e10f};
            // His room: the floor under him, past his ghost body on this machine.
            const uint32_t hisRoom = humanViewRange > 0.0f ? FloorStream(at, 0.5f, GhostForMachine(peerTable.peers[slot].machine)) : 0;
            if (humanViewRange > 0.0f && !hisRoom) InterlockedIncrement(&humanRoomsUnknown);
            uint32_t vents[32]{};
            unsigned n = 0, fallbackPass = 0, fallbackN = 0;
            uint32_t fallback = 0;
            int fallbackRoom = -1;
            for (unsigned pass = 0; pass < 3; ++pass) {
                if (pass < 2) n = FindVentsRaw(vents, pass ? 32u : 16u, pos, reaches[pass]);
                for (unsigned i = 0; i < n && i < 32; ++i) {
                    if (!vents[i]) continue;
                    if (!CanSpawnRaw(vents[i])) { if (pass < 2) InterlockedIncrement(&ventsRejectedSpawn); continue; }
                    if (!VentStreamShown(vents[i]) || !VentRoomHere(vents[i], now, pass == 2)) { if (pass < 2) InterlockedIncrement(&ventsRejectedStream); continue; }
                    const int room = hisRoom && pass < 2 ? VentInRoom(vents[i], hisRoom, now) : -1;
                    if (hisRoom && pass < 2) {
                        float centroid[3]{};
                        Read(vents[i] + kVentCentroid, centroid, 12);
                        const float dx = centroid[0] - at[0], dy = centroid[1] - at[1], dz = centroid[2] - at[2];
                        if (room == 0 || dx * dx + dy * dy + dz * dz > humanViewRange * humanViewRange) {
                            if (!fallback) { fallback = vents[i]; fallbackPass = pass; fallbackN = n; fallbackRoom = room; }
                            continue;   // one in his room may still come
                        }
                        if (fallback) InterlockedIncrement(&sameRoomPreferred);
                    }
                    return TakeAutoVent(proxy, vents[i], pass, slot, at, n, room, now);
                }
                if (pass == 1 && fallback) return TakeAutoVent(proxy, fallback, fallbackPass, slot, at, fallbackN, fallbackRoom, now);
            }
            if (!AutoNoteQuiet(now))
                Note("invasion: no spawnable vent in a shown room for partner %u (%u loaded vent(s)): the game's own pick, kept until the next pick (%ld so far)", slot, n,
                     InterlockedCompareExchange(&autoFallbacks, 0, 0) + 1);
        }
        InterlockedIncrement(&autoFallbacks);
        gamePickRunning = true;
        const uint32_t picked = autoSelectHook.call<uint32_t>(proxy);
        gamePickRunning = false;
        InterlockedIncrement(picked ? &gamePicks : &gamePicksEmpty);
        keptVent = picked;
        return picked;
    }
    return autoSelectHook.call<uint32_t>(proxy);
}
// The pause menu's suicide row for a living necro (user, 17.10.: "a suicide button as in MP, in
// place of the friends list"). MP's MP10PauseMenu sends lethal damage (CanCommitSuicide: the local
// player and not under a paired coordinator); here the necro takes MP's own way back into the
// vents (the forced re-entry below), with the same grab check and the respawn pace after it.
volatile LONG suicideWanted = 0, suicidesAsked = 0, suicidesDone = 0, suicidesRefused = 0;
DWORD suicideAt = 0;
bool SuicideRow() { return enabled && invasion && !play::VersusMode() && !mpmap::InVisit() && necroEntity && IsNecro(necroEntity); }
bool SuicideAvailable();   // below NecroDead: a paired attack or a dead necro refuses
bool CommitSuicide() {
    InterlockedIncrement(&suicidesAsked);
    if (!SuicideAvailable()) {
        InterlockedIncrement(&suicidesRefused);
        gate::Event("test suicide REFUSED: %s", SuicideRow() ? "the necro is in a paired attack or dead" : "no living necro");
        return false;
    }
    InterlockedExchange(&suicideWanted, 1);
    suicideAt = Stamp(GetTickCount());
    Note("suicide: asked from the pause menu, necro %08X goes into the vents at its next state check", necroEntity);
    return true;
}
// MP's "re-enter a vent" for a living necro, forced while a human is in a scene.
safetyhook::InlineHook masterCheckHook;
__declspec(noinline) bool NecroDead(uint32_t necro) {
    uint32_t vtable = 0, fn = 0;
    if (!Read(necro, &vtable, 4) || !Read(vtable + kNecroIsDeadSlot, &fn, 4) || fn != base + kNecroIsDeadRva) return false;
    __try {
        return reinterpret_cast<bool(__thiscall*)(uint32_t)>(fn)(necro);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SuicideAvailable() {
    if (!SuicideRow()) return false;
    uint32_t coordinator = 0;
    if (Read(necroEntity + kPairedCoordinator, &coordinator, 4) && coordinator) return false;
    return !NecroDead(necroEntity);
}
unsigned char __fastcall OnMasterCheck(void* self, void*, uint32_t a, float dt, uint32_t id, const void* data) {
    const unsigned char result = masterCheckHook.thiscall<unsigned char>(self, a, dt, id, data);
    if (result || id != 0xD || !InterlockedCompareExchange(&forceGhost, 0, 0)) return result;
    const uint32_t sm = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    uint32_t necro = 0, coordinator = 0;
    if (!Read(sm + 0x54, &necro, 4) || !necro || necro != necroEntity) { InterlockedIncrement(&forceRefusedLocal); return result; }
    if (Read(necro + kPairedCoordinator, &coordinator, 4) && coordinator) { InterlockedIncrement(&forceRefusedGrab); return result; }
    if (NecroDead(necro)) { InterlockedIncrement(&forceRefusedDead); return result; }
    InterlockedExchange(&forceGhost, 0);
    InterlockedIncrement(&forcedGhosts);
    LifeExpect(kLifeGhost, "forced into the vents");
    if (InterlockedExchange(&suicideWanted, 0)) {
        InterlockedIncrement(&suicidesDone);
        gate::Event("test suicide PASS: necro %08X went into the vents %lu ms after the pause menu", necro, static_cast<unsigned long>(GetTickCount() - suicideAt));
    }
    Note("invasion: %s: necro %08X goes into the vents", watch == Watch::Returning ? "back from watching the scene" : joinGhostWanted ? "joined as a necromorph" : "a human is in a scene", necro);
    joinGhostWanted = false;
    return 1;
}
// A living necro opens the closed, unlocked door it stands at: off by default ([Necro] OpensDoors).
// The user (17.10.): a necro opens no door; he only holds an open one open like a human does,
// from the room next to it (worldcalls KeepOpen and PlayersOnBothSides count him).
bool opensDoors = false;
DWORD doorAssistAt = 0;
__declspec(noinline) bool TryOpenDoor(uint32_t door, uint32_t actor) {
    __try {
        return reinterpret_cast<bool(__thiscall*)(void*, uint32_t)>(base + worldcalls::kTryActivateDoorRva)(reinterpret_cast<void*>(door), actor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&invasionFaults);
        return false;
    }
}
// An open door near the living necro: its open set (door attributes +0xC0 side 1, +0xE0 side 0,
// the side he stands on) is loaded here, both rooms of the doorway; the humans' opening reaches his
// machine only mirrored, which loads nothing.
uint32_t doorSetDoor = 0;
DWORD doorSetAt = 0;
volatile LONG doorSetsLoaded = 0;
void LoadDoorRooms(uint32_t door, DWORD now) {
    if (door == doorSetDoor && now - doorSetAt < 3000) return;
    bool side = true;
    if (!worldcalls::DoorSide(door, necroEntity, side)) side = true;
    uint32_t desc = 0, set[4]{};
    if (!Read(door + 0x0C, &desc, 4) || !desc || !Read(desc + (side ? 0xC0 : 0xE0), set, 16) || !(set[0] | set[1] | set[2] | set[3])) return;
    doorSetDoor = door;
    doorSetAt = now;
    if (streams::Covers(set)) return;
    if (streams::ActivateDoorSet(set)) {
        InterlockedIncrement(&doorSetsLoaded);
        if (doorSetsLoaded <= 40) Note("invasion: necro %08X at open door %08X: its rooms loaded (open set side %d %08X-%08X)", necroEntity, door, side ? 1 : 0, set[0], set[1]);
    }
}
void NecroDoorAssist(DWORD now) {
    if (now - doorAssistAt < 400) return;
    doorAssistAt = now;
    float me[3]{};
    if (!Read(necroEntity + kEntityMatrix + 0x30, me, 12)) return;
    uint32_t node = 0;
    if (!Read(base + kDoorListRva, &node, 4)) return;
    for (unsigned steps = 0; node && steps < 8192; ++steps) {
        const uint32_t entity = node - kDoorListOffset;
        uint32_t next = 0, vtable = 0;
        const bool more = Read(node + 4, &next, 4) && next != node;
        float at[3]{};
        if (Read(entity, &vtable, 4) && vtable == base + worldcalls::kStandardDoorVtableRva && Read(entity + 0xC0, at, 12)) {
            const float dx = at[0] - me[0], dz = at[2] - me[2], dy = at[1] - me[1];
            uint32_t state = 1;
            const bool readState = Read(entity + worldcalls::kDoorState, &state, 4);
            if (readState && (state == worldcalls::kDoorOpening || state == worldcalls::kDoorOpen) && dx * dx + dz * dz <= 6.0f * 6.0f && dy > -3.0f && dy < 3.0f) {
                LoadDoorRooms(entity, now);
                return;
            }
            if (opensDoors && dx * dx + dz * dz <= 2.5f * 2.5f && dy > -2.5f && dy < 2.5f) {
                unsigned char locked = 1;
                if (readState && state == 0 && Read(entity + 0x258, &locked, 1) && !locked && TryOpenDoor(entity, necroEntity)) {
                    if (InterlockedIncrement(&doorsOpened) <= 20) Note("invasion: necro %08X opened door %08X", necroEntity, entity);
                }
                return;
            }
        }
        if (!more) break;
        node = next;
    }
}
void InvasionTick() {
    if (!invasion || play::VersusMode() || !peerTable.Count() || !inLevel) { InterlockedExchange(&forceGhost, 0); return; }
    const DWORD now = GetTickCount();
    if (HumanInScene(now)) humanSceneAt = Stamp(now);
    SampleTrails(now);
    const bool alive = necroEntity && IsNecro(necroEntity);
    const bool signal = humanSceneAt && now - humanSceneAt < 500;
    if (signal) { if (!sceneSignalSince) sceneSignalSince = Stamp(now); sceneQuietSince = 0; }
    else { sceneSignalSince = 0; if (!sceneQuietSince) sceneQuietSince = Stamp(now); }
    if (sitOut && watch == Watch::None && sceneQuietSince && now - sceneQuietSince >= kSitOutQuietMs && !SceneHold(now)) {
        sitOut = false;
        Note("watch: the sat-out scene is over");
    }
    // With WatchScenes the cue decides; the scene signal alone forces the vents only when no watch
    // came for kForceAfterSignalMs, or the scene is sat out.
    const bool signalForce = signal && !FinaleArena(now) && (!watchScenes || sitOut || (sceneSignalSince && now - sceneSignalSince >= kForceAfterSignalMs));
    const bool sceneForce = watch == Watch::None && (signalForce || joinGhostWanted);
    if (InterlockedCompareExchange(&suicideWanted, 0, 0) && (!alive || now - suicideAt > 10000)) {
        InterlockedExchange(&suicideWanted, 0);
        gate::Event("test suicide FAIL: the necro %s within 10 s", alive ? "did not go into the vents" : "was gone");
    }
    InterlockedExchange(&forceGhost, alive && (sceneForce || watch == Watch::Returning || InterlockedCompareExchange(&suicideWanted, 0, 0)) ? 1 : 0);
    WatchHumanRooms(now, !alive && playerGone);
    if (alive && watch == Watch::None) NecroDoorAssist(now);   // open doors' rooms; closed ones only with OpensDoors
}
// Worn uniform at Player+0x1688 (vtable 0x0174FCB0 + base, GUID at +0x24), as botnet::WornSuit reads it.
bool WornSuitGuid(uint32_t player, uint32_t* out) {
    uint32_t uniform = 0, vtable = 0;
    return player && Read(player + 0x1688, &uniform, 4) && uniform && Read(uniform, &vtable, 4) && vtable == base + 0x174FCB0 && Read(uniform + 0x24, out, 16) &&
           (out[0] | out[1] | out[2] | out[3]);
}
// The suit the scene's author announced (suit sync), else any partner's.
const uint32_t* AuthorSuit(unsigned author, unsigned& from) {
    from = author;
    if (author < coop_peers::kMaxPeers && peerTable.peers[author].used && peerRuntime[author].suit.have) return peerRuntime[author].suit.guid;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s)
        if (peerTable.peers[s].used && peerRuntime[s].suit.have) { from = s; return peerRuntime[s].suit.guid; }
    return nullptr;
}
// The suit a body we spawn is born in ([Necro] BirthSuits): the watching Isaac in the scene
// author's suit, Isaac back from the necro in his own (the last one he wore as a human; the
// record gave him the patient suit). Without it the record's suit stays.
bool birthSuits = true;
uint32_t ownSuit[4]{};
bool ownSuitKnown = false;
DWORD ownSuitCheckAt = 0, ownPreloadAt = 0;
volatile LONG ownSuitChanges = 0, ownPreloads = 0, ownPreloadsResident = 0;
const uint32_t* BirthSuit(const char*& why) {
    if (!birthSuits) return nullptr;
    if (watch == Watch::Wanted || watch == Watch::Confirming || watch == Watch::Watching) {
        unsigned from = 0;
        why = "the watching Isaac (the scene author's suit)";
        return AuthorSuit(watchAuthor, from);
    }
    why = "Isaac back from the necro (his own suit)";
    return ownSuitKnown ? ownSuit : nullptr;
}
// As a human: remember the worn suit. As a necro: keep that suit resident for the way back
// (PreloadUniformStreams; it may have been unloaded by another suit's first activation).
void OwnSuitTick(DWORD now) {
    if (!birthSuits || now - ownSuitCheckAt < 1000) return;
    ownSuitCheckAt = Stamp(now);
    const uint32_t isaac = LocalCampaignPlayer();
    if (!necroEntity && !playerGone && watch == Watch::None && IsPlayer(isaac)) {
        uint32_t worn[4]{};
        if (WornSuitGuid(isaac, worn) && coop_uniform::SuitIndex(worn) >= 0 && (!ownSuitKnown || std::memcmp(worn, ownSuit, 16) != 0)) {
            std::memcpy(ownSuit, worn, 16);
            ownSuitKnown = true;
            InterlockedIncrement(&ownSuitChanges);
        }
        return;
    }
    if (!ownSuitKnown || !(necroEntity || playerGone) || now - ownPreloadAt < 10000) return;
    ownPreloadAt = Stamp(now);
    const int r = coop_uniform::Preload(ownSuit);
    if (r == 0) InterlockedIncrement(&ownPreloadsResident);
    if (r == 1) {
        InterlockedIncrement(&ownPreloads);
        Note("suit: own suit %08X-%08X was not resident; preloaded for the way back to Isaac", ownSuit[0], ownSuit[1]);
    }
}
void WatchDressTick(DWORD now) {
    if (watchDress == WatchDress::Done) return;
    const uint32_t isaac = LocalCampaignPlayer();
    uint32_t parts[3]{}, worn[4]{};
    const bool whole = isaac && coop_uniform::BotBody(isaac, parts);
    if (watchDress == WatchDress::Issued) {
        unsigned from = 0;
        const uint32_t* want = AuthorSuit(watchAuthor, from);
        if (whole && want && WornSuitGuid(isaac, worn) && std::memcmp(worn, want, 16) == 0) {
            watchDress = WatchDress::Done;
            Note("watch: Isaac %08X wears partner %u's suit %08X after %lu ms", isaac, from, worn[0], static_cast<unsigned long>(now - watchDressSince));
        } else if (now - watchDressSince > 2500) {
            watchDress = WatchDress::Done;
            InterlockedIncrement(&watchDressTimeouts);
            Note("watch: Isaac %08X's suit change did not show within 2.5 s (body whole %d, worn %08X): the scene goes on", isaac, whole ? 1 : 0, worn[0]);
        }
        return;
    }
    if (!whole) {
        if (now - watchDressSince > 2500) {
            watchDress = WatchDress::Done;
            InterlockedIncrement(&watchDressTimeouts);
            Note("watch: Isaac %08X's body was not whole within 2.5 s, no suit change", isaac);
        }
        return;
    }
    unsigned from = 0;
    const uint32_t* want = AuthorSuit(watchAuthor, from);
    const char* skip = !want ? "no partner announced a suit"
                     : coop_uniform::SuitIndex(want) < 0 ? "the partner's suit is not registered here"
                     : mpsuit::IsMpSuit(want) && !mpsuit::PartnersReady() ? "the partner's MP suit skin is not here yet" : nullptr;
    if (skip) {
        watchDress = WatchDress::Done;
        InterlockedIncrement(&watchDressSkipped);
        Note("watch: Isaac %08X keeps his suit: %s", isaac, skip);
        return;
    }
    const bool haveWorn = WornSuitGuid(isaac, worn);
    if (haveWorn && std::memcmp(worn, want, 16) == 0) {
        watchDress = WatchDress::Done;
        InterlockedIncrement(&watchDressKept);
        return;
    }
    if (!coop_uniform::BotDress(isaac, want)) {
        watchDress = WatchDress::Done;
        InterlockedIncrement(&watchDressSkipped);
        Note("watch: Isaac %08X's change into partner %u's suit %08X FAILED", isaac, from, want[0]);
        return;
    }
    watchDress = WatchDress::Issued;
    watchDressSince = now;
    InterlockedIncrement(&watchDressed);
    Note("watch: Isaac %08X changes from suit %08X into partner %u's %08X for the scene", isaac, haveWorn ? worn[0] : 0u, from, want[0]);
}
void Transform(const float* at);
// The kind alone starts a watch for a settled ghost at the vents (kKindWatchMs above).
void KindWatchTick(DWORD now) {
    unsigned best = 0xFF;
    uint32_t bestKind = scenekind::kUnknown;
    DWORD bestFor = 0;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        const uint32_t kind = peer.used && peer.token && !roles::IsNecro(s, now) ? scenekind::PartnerKind(s, now) : scenekind::kUnknown;
        if (!scenekind::Watchable(kind)) { kindWatchSince[s] = 0; continue; }
        if (!kindWatchSince[s]) kindWatchSince[s] = Stamp(now);
        const DWORD lasted = Age(now, kindWatchSince[s]);
        if (lasted >= kKindWatchMs && lasted >= bestFor) { best = s; bestKind = kind; bestFor = lasted; }
    }
    if (best == 0xFF || !enabled || !invasion || !watchScenes || play::VersusMode() || !inLevel || sitOut) return;
    if (!playerGone || necroEntity || !LocalGhost() || !ghostProxySince || Age(now, ghostProxySince) < kGhostSettleMs || DirectSpawnPending() ||
        (ventRequestAt && Age(now, ventRequestAt) < 8000))
        return;
    if (FinaleArena(now) || RestartOrLevelChange()) return;
    watchAuthor = best;
    watchWantedAt = watchCueAt = Stamp(now);
    watch = Watch::Wanted;
    InterlockedIncrement(&watchAsks);
    InterlockedIncrement(&watchFromKind);
    gate::Event("test watch INFO: partner %u has been in a %s for %lu ms and no cue came here: the ghost at the vents turns into Isaac to watch it", best,
                scenekind::kNames[bestKind < scenekind::kKinds ? bestKind : 0], static_cast<unsigned long>(bestFor));
}
void WatchTick(DWORD now) {
    if (watch == Watch::None) { KindWatchTick(now); return; }
    if (!inLevel || RestartOrLevelChange()) {
        gate::Event("necro watch: ended by %s%s", inLevel ? "a restart or level change" : "the level going", necroByChoice ? ", back to the necro team after it" : "");
        watch = Watch::None;
        if (necroByChoice && !InterlockedCompareExchange(&joinAsNecroPending, 0, 0)) {
            InterlockedIncrement(&watchRejoins);
            JoinAsNecro();
        }
        return;
    }
    if (watch == Watch::Confirming) {
        const uint32_t authorKind = scenekind::PartnerKind(watchAuthor, now);
        const char* kindName = authorKind < scenekind::kKinds ? scenekind::kNames[authorKind] : "unknown";
        // A short sequence often leads into the cinematic (17.10.: 313 ms of it, then the cinematic,
        // and the cancel left the necro a ghost for the whole scene): it keeps waiting. Only a
        // playable sequence or the end of the window without a watchable kind cancels.
        if (authorKind == scenekind::kPlayable) {
            InterlockedIncrement(&watchUnconfirmed);
            gate::Event("test watch INFO: partner %u's cue started a playable sequence, no cinematic: stays a necro", watchAuthor);
            watch = Watch::None;
            watchAuthor = 0xFF;
            return;
        }
        if (authorKind != scenekind::kUnknown ? scenekind::Watchable(authorKind) : (cutscene::PartnerInScene(watchAuthor, now) || HumanInScene(now))) {
            InterlockedIncrement(&watchConfirmed);
            gate::Event("necro watch: partner %u's %s confirmed %lu ms after the cue: %s turns back into Isaac to follow it", watchAuthor, kindName,
                        static_cast<unsigned long>(Age(now, watchConfirmSince)), necroEntity ? "the living necro" : "the ghost at the vents");
            watch = Watch::Wanted;
            watchWantedAt = Stamp(now);
        } else if (Age(now, watchConfirmSince) >= kWatchConfirmMs) {
            InterlockedIncrement(&watchUnconfirmed);
            gate::Event("test watch INFO: no watchable scene of partner %u followed his cue within %lu ms (his kind: %s): stays a necro", watchAuthor,
                        static_cast<unsigned long>(kWatchConfirmMs), kindName);
            watch = Watch::None;
            watchAuthor = 0xFF;
        } else if (authorKind == scenekind::kShort) {
            InterlockedIncrement(&watchShortWaits);
        }
        return;
    }
    if (watch == Watch::Wanted) {
        if (Age(now, watchWantedAt) < kWatchSettleMs) return;   // the role packet goes out first, so the humans hide what comes
        const bool late = Age(now, watchWantedAt) > kWatchWaitMs;
        uint32_t coordinator = 0;
        if (necroEntity && IsNecro(necroEntity) && Read(necroEntity + kPairedCoordinator, &coordinator, 4) && coordinator) {
            if (late) DropWatch("the necro is still in a paired attack", &watchRefusedGrab);
            return;
        }
        if ((necroEntity && collisionPendingAt) || (playerGone && ventRequestAt && Age(now, ventRequestAt) < 8000)) {
            if (late) DropWatch("the necro is still coming out of a vent", &watchRefusedSpawning);
            return;
        }
        if (!necroEntity && playerGone && (!LocalGhost() || !ghostProxySince || Age(now, ghostProxySince) < kGhostSettleMs)) {
            if (!LocalGhost() && late) DropWatch("no settled ghost proxy", &watchRefusedSpawning);
            else if (late) DropWatch("the ghost proxy did not settle", &watchRefusedSpawning);
            else InterlockedIncrement(&watchWaitedForGhost);
            return;
        }
        float spot[3]{};
        if (!WatchSpot(watchAuthor, now, spot)) {
            if (late) DropWatch("no position of the scene's author", &watchRefusedNoSpot);
            return;
        }
        alignas(16) float matrix[16]{};
        matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
        matrix[12] = spot[0]; matrix[13] = spot[1] + 0.1f; matrix[14] = spot[2];
        const bool fromGhost = !necroEntity;
        __try {
            if (necroEntity) Revert(true, true, matrix);
            else EscapeGhost(matrix);
        } __except (FaultFilter(GetExceptionInformation())) {
            Note("watch: the way back to Isaac faulted: exception %08X at %08X", static_cast<unsigned>(GetExceptionCode()), faultAddress);
        }
        if (!IsPlayer(coop::LocalSlotZeroEntity()) || playerGone) { DropWatch("Isaac did not come back", &watchRefusedFault); return; }
        watch = Watch::Watching;
        watchSince = Stamp(now);
        watchFollowSeenAt = 0;
        watchDress = watchSuit ? WatchDress::Pending : WatchDress::Done;
        watchDressSince = now;
        InterlockedIncrement(&watchStarts);
        InterlockedIncrement(fromGhost ? &watchFromGhost : &watchFromLiving);
        const uint32_t startKind = scenekind::PartnerKind(watchAuthor, now);
        gate::Event("necro watch: Isaac %08X stands at (%.1f %.1f %.1f) behind partner %u (kind %s), following his scene", coop::LocalSlotZeroEntity(), spot[0], spot[1],
                    spot[2], watchAuthor, startKind < scenekind::kKinds ? scenekind::kNames[startKind] : "unknown");
        return;
    }
    if (watch == Watch::Watching) {
        WatchDressTick(now);
        // The author's own scene kind decides (17.10.: his stationary aiming went on while nothing ran
        // here, and the watch ended after 6 s); the local follower only when no kind comes from him.
        const uint32_t authorKind = scenekind::PartnerKind(watchAuthor, now);
        const bool kindKnown = authorKind != scenekind::kUnknown;
        const bool localScene = cutscene::Following() || cutscene::SequenceRunningNow();
        const bool inScene = kindKnown ? authorKind != scenekind::kNone && authorKind != scenekind::kPlayable : localScene;
        if (inScene) watchFollowSeenAt = Stamp(now);
        const bool sceneSignal = SceneHold(now) || Age(now, watchCueAt) < kWatchEndQuietMs;
        const char* why = nullptr;
        if (Age(now, watchSince) > kWatchMaxMs) {
            why = "five minutes; the rest of the scene is sat out";
            InterlockedIncrement(&watchEndsTimeout);
            sitOut = true;   // or the kind would start the next watch at once
        }
        else if (authorKind == scenekind::kPlayable) { why = "the author's scene became playable, it is played, not watched"; InterlockedIncrement(&watchEndsTimeout); }
        else if (!watchFollowSeenAt && Age(now, watchSince) > kWatchNoSceneMs && !inScene) {
            why = "the scene never started (neither the author's kind nor here); the rest of it is sat out";
            InterlockedIncrement(&watchEndsNoScene);
            sitOut = true;
        }
        else if (watchFollowSeenAt && !inScene && !sceneSignal && Age(now, watchFollowSeenAt) >= kWatchEndQuietMs && Age(now, watchSince) >= kWatchMinMs)
            why = "the scene is over";
        if (!why) return;
        InterlockedIncrement(&watchEnds);
        gate::Event("necro watch: %s after %lu ms (author %u kind %s, here following %d), back to the necro team", why, static_cast<unsigned long>(Age(now, watchSince)),
                    watchAuthor, authorKind < scenekind::kKinds ? scenekind::kNames[authorKind] : "unknown", localScene ? 1 : 0);
        watch = Watch::Returning;
        returnSince = Stamp(now);
        returnTryAt = 0;
        return;
    }
    // Returning: the join's way (Transform, then the forced vent re-entry), role 3 until the ghost is up.
    // 17.10.: it ended with the transform, so the living necro stood beside the humans and only went
    // into the vents when a scene signal happened to force it.
    if (playerGone && !necroEntity && LocalGhost()) {
        InterlockedIncrement(&watchReturns);
        gate::Event("necro watch: back at the vents after %lu ms", static_cast<unsigned long>(Age(now, returnSince)));
        watch = Watch::None;
        return;
    }
    if (Age(now, returnSince) > kReturnGiveUpMs) {
        InterlockedIncrement(&watchReturnFailures);
        gate::Event("necro watch: the way back to the necro team failed for %lu ms (error %ld), staying %s", static_cast<unsigned long>(Age(now, returnSince)),
                    InterlockedCompareExchange(&lastError, 0, 0), necroEntity ? "a living necro" : "Isaac; the necro key turns him back");
        watch = Watch::None;
        return;
    }
    if (!necroEntity && (!returnTryAt || Age(now, returnTryAt) >= 500)) {
        returnTryAt = now;
        __try { Transform(); } __except (FaultFilter(GetExceptionInformation())) {
            Note("watch: transform back faulted: exception %08X at %08X", static_cast<unsigned>(GetExceptionCode()), faultAddress);
        }
    }
}
// A QTE of a human's scene on a machine whose slot 0 holds no Isaac (16.10., crash 9712,
// analysis/NECRO_QTE_CRASH_2026-10-16.md): TestInputEvent binds its player once, at
// construction (GetPlayerFromPlayerManager(0), NULL for a necro body), and
// TestInputEvent::Activate (0x010AECB0, thiscall(this), plain ret) uses the binding and the
// player's AlertVFX component without a check. The host ghost's animation event started it
// here. Without a player the QTE is not activated: TestInput and HandleEvents check for null.
safetyhook::InlineHook qteActivateHook;
volatile LONG qteActivations = 0, qteSkippedNoPlayer = 0, qteSkippedNoAlert = 0, qteRebound = 0, qteLines = 0;
constexpr uint32_t kQteActivateRva = 0xCAECB0, kAlertVfxTypeRva = 0x1CA3C02, kComponentLookupRva = 0x3CF080;
uint32_t AlertComponent(uint32_t entity) {
    unsigned char alertType = 0;
    if (!entity || !Read(base + kAlertVfxTypeRva, &alertType, 1)) return 0;
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t)>(base + kComponentLookupRva)(reinterpret_cast<void*>(entity), alertType);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
void __fastcall OnQteActivate(void* self, void*) {
    InterlockedIncrement(&qteActivations);
    const uint32_t qte = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    uint32_t ref = 0, attributes = 0, flags = 0, start = 0, guid[4]{};
    Read(qte + 0x5C, &ref, 4);
    const uint32_t player = ref > 0x10 ? ref - 0x10 : 0;
    const bool alertWanted = Read(qte + 0x0C, &attributes, 4) && attributes && Read(attributes + 0x30, &flags, 4) && (flags & 1);
    // A machine that is Isaac again (a necro watching the humans' scene): the QTE was built
    // while slot 0 held the necro, so it is bound here the way its constructor binds
    // (0x010C4E3D: SafePtr +0x5C -> player+0x10, linked in at [player+0x14]).
    if (!player && ref == 0) {
        const uint32_t isaac = LocalCampaignPlayer();
        uint32_t head = 0;
        if (isaac && IsPlayer(isaac) && Read(isaac + 0x14, &head, 4)) {
            const uint32_t target = isaac + 0x10, link = qte + 0x5C;
            if (WriteBytes(link, &target, 4) && WriteBytes(link + 4, &head, 4) && WriteBytes(isaac + 0x14, &link, 4)) {
                InterlockedIncrement(&qteRebound);
                if (InterlockedIncrement(&qteLines) <= 20) Note("qte %08X bound to Isaac %08X (role %d)", qte, isaac, LocalRole());
                ref = target;
            }
        }
    }
    const uint32_t bound = ref > 0x10 ? ref - 0x10 : 0;
    const char* why = !bound ? "no player bound" : alertWanted && !AlertComponent(bound) ? "the player has no alert component" : nullptr;
    if (!why) { qteActivateHook.thiscall<void>(self); return; }
    InterlockedIncrement(bound ? &qteSkippedNoAlert : &qteSkippedNoPlayer);
    if (InterlockedIncrement(&qteLines) <= 20) {
        Read(qte + 0x24, guid, 16);
        if (attributes) Read(attributes, &start, 4);
        Note("qte %08X (%08X-%08X, start %08X) not activated: %s (role %d, slot 0 %08X)", qte, guid[0], guid[3], start, why, LocalRole(),
             coop::LocalSlotZeroEntity());
    }
}
// A necro player's death drops MP loot (NecroPlayerMasterSM::DeathDropItem 0x0108DBD0,
// thiscall(sm), plain ret: list 37CA4774, large MultiAmmo 75 / Health Kit 25). In the story it
// fell on the necro's machine only, as MP ammo no campaign gun takes (16.10.).
safetyhook::InlineHook deathDropHook;
volatile LONG deathDropsSkipped = 0, deathDropsPassed = 0;
constexpr uint32_t kDeathDropRva = 0xC8DBD0;
void __fastcall OnNecroDeathDrop(void* sm, void*) {
    if (play::StoryCoop()) { InterlockedIncrement(&deathDropsSkipped); return; }
    InterlockedIncrement(&deathDropsPassed);
    deathDropHook.thiscall<void>(sm);
}
void InstallStoryGuards(FILE* log) {
    // push esi / mov esi,ecx / test byte [esi+64h],1 / jne / or byte [esi+64h],1 / push edi / push 0204D4D4
    static const unsigned char kQte[] = {0x56, 0x8B, 0xF1, 0xF6, 0x46, 0x64, 0x01, 0x75, 0x57, 0x80, 0x4E, 0x64, 0x01, 0x57, 0x68, 0xD4, 0xD4, 0x04, 0x02};
    // push ebp / mov ebp,esp / and esp,-16 / sub esp,44h / mov ecx,[ecx+54h] / mov eax,[ecx+0Ch] / add eax,1BCh
    static const unsigned char kDrop[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x44, 0x8B, 0x49, 0x54, 0x8B, 0x41, 0x0C, 0x05, 0xBC, 0x01, 0x00, 0x00};
    const bool q = hook_site::Install(log, "necro TestInputEvent::Activate", base + kQteActivateRva, kQte, sizeof(kQte), reinterpret_cast<void*>(OnQteActivate), qteActivateHook);
    const bool d = hook_site::Install(log, "necro NecroPlayerMasterSM::DeathDropItem", base + kDeathDropRva, kDrop, sizeof(kDrop), reinterpret_cast<void*>(OnNecroDeathDrop), deathDropHook);
    std::fprintf(log, "necro story guards: QTE without a player %s, necro death drops in the story %s\n", q ? "skipped" : "NOT GUARDED", d ? "dropped" : "STILL FALL");
}
void InstallInvasion(FILE* log) {
    if (!invasion) { std::fprintf(log, "necro invasion off ([Necro] Invasion=0)\n"); return; }
    // push ebp / mov ebp,esp / and esp,-16 / sub esp,94h / push ebx / push esi / push edi / push 4
    static const unsigned char kAutoSelect[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00, 0x53, 0x56, 0x57, 0x6A, 0x04};
    // push ebx / push esi / mov esi,ecx / mov ecx,[esp+14h] / mov eax,ecx / xor bl,bl / sub eax,0Bh / push edi
    static const unsigned char kMasterCheck[] = {0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x14, 0x8B, 0xC1, 0x32, 0xDB, 0x83, 0xE8, 0x0B, 0x57};
    const bool a = hook_site::Install(log, "necro invasion AutoSelectRespawnVent", base + kAutoSelectRva, kAutoSelect, sizeof(kAutoSelect), reinterpret_cast<void*>(OnAutoSelectVent), autoSelectHook);
    const bool b = hook_site::Install(log, "necro invasion NecroPlayerMasterSM::CheckTransition", base + kMasterCheckRva, kMasterCheck, sizeof(kMasterCheck), reinterpret_cast<void*>(OnMasterCheck), masterCheckHook);
    // push ebp / mov ebp,esp / and esp,-16 / sub esp,1Ch / push esi / mov esi,[ebp+8] / push ecx / mov eax,esp
    static const unsigned char kCanSpawn[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x1C, 0x56, 0x8B, 0x75, 0x08, 0x51, 0x8B, 0xC4};
    const bool c = hook_site::Install(log, "necro invasion CanSpawnAtVent", base + kCanSpawnAtVentRva, kCanSpawn, sizeof(kCanSpawn), reinterpret_cast<void*>(OnCanSpawnAtVent), canSpawnHook);
    std::fprintf(log, "necro invasion: vents in rooms this machine does not show %s for a necro at the vents\n", c ? "cannot be picked" : "CAN STILL BE PICKED (hook missing)");
    std::fprintf(log, "necro invasion: ghost camera near the humans %s (radius %.0f m, follow every %lu ms), back into the vents in their scenes %s, doors %s\n",
                 a ? "on" : "OFF", ghostFollowRadius, static_cast<unsigned long>(recentreCooldownMs), b ? "on" : "OFF", opensDoors ? "opened by a living necro ([Necro] OpensDoors=1)" : "never opened by a necro, only held open from the next room");
}
void WriteInvasion(FILE* log) {
    std::fprintf(log, "necro invasion forced_ghosts=%ld refused local=%ld grab=%ld dead=%ld accepts_held=%ld room_changes=%ld recentres=%ld auto_picks=%ld far=%ld fallbacks=%ld "
                      "vents_rejected spawn=%ld stream=%ld hidden_refused=%ld doors_opened=%ld faults=%ld scene_hold=%d transforms_held=%ld after_scene=%d\n",
                 forcedGhosts, forceRefusedLocal, forceRefusedGrab, forceRefusedDead, acceptsHeld, roomChanges, recentres, autoPicks, autoPicksFar, autoFallbacks, ventsRejectedSpawn,
                 ventsRejectedStream, ventsRefusedHidden, doorsOpened, invasionFaults, SceneHold(GetTickCount()) ? 1 : 0, transformsHeld,
                 transformAfterScene ? 1 : 0);
    WriteDirectSpawn(log);
    WriteLife(log);
    std::fprintf(log, "necro vent_pick kept=%08X kept_passes=%ld game_picks=%ld game_picks_empty=%ld hud_shown=%ld hud_orphans_hidden=%ld hud_sweeps=%ld "
                      "teleports=%ld (event %ld, jump %ld) teleport_recentres=%ld views_at_human=%ld\n", keptVent,
                 InterlockedCompareExchange(&keptVentPasses, 0, 0), InterlockedCompareExchange(&gamePicks, 0, 0), InterlockedCompareExchange(&gamePicksEmpty, 0, 0),
                 InterlockedCompareExchange(&hudShown, 0, 0), InterlockedCompareExchange(&hudOrphansHidden, 0, 0), InterlockedCompareExchange(&hudSweepsDone, 0, 0),
                 InterlockedCompareExchange(&humanTeleports, 0, 0), InterlockedCompareExchange(&teleportsByEvent, 0, 0), InterlockedCompareExchange(&teleportsByJump, 0, 0),
                 InterlockedCompareExchange(&teleportRecentres, 0, 0), InterlockedCompareExchange(&viewsAtHuman, 0, 0));
    if (paceBaseMs)
        std::fprintf(log, "necro respawn_pace base_ms=%lu max_ms=%lu deaths=%ld vents_held=%ld opened=%ld last_wait_ms=%lu humans=%u necros=%u left_ms=%lu\n",
                     static_cast<unsigned long>(paceBaseMs), static_cast<unsigned long>(paceMaxMs), paceDeaths, paceHeld, paceDone,
                     static_cast<unsigned long>(paceWaitMs), paceHumans, paceNecros, static_cast<unsigned long>(PaceLeft(GetTickCount())));
}

safetyhook::InlineHook checkTransitionHook, spawnHook, stateMessageHook;
volatile LONG transitionCalls[8]{}, transitionTrue[8]{}, stateMessages[19]{}, spawnCalls = 0, confirmKeyUsed = 0, spawnTypeRepairs = 0, restartResets = 0;
uint32_t lastSpawnResult = 0, lastSpawnSide = 0xFFFFFFFFu, lastSpawnFlagB2 = 0xFFFFFFFFu;
int confirmKey = VK_RETURN;
bool confirmKeyDown = false;
bool GameWindowFocused() {
    DWORD foregroundProcess = 0;
    const HWND foreground = GetForegroundWindow();
    if (foreground) GetWindowThreadProcessId(foreground, &foregroundProcess);
    return foregroundProcess == GetCurrentProcessId();
}
unsigned char __fastcall OnCheckTransition(void* self, void*, uint32_t a, float dt, uint32_t id, const void* data) {
    unsigned char result = checkTransitionHook.thiscall<unsigned char>(self, a, dt, id, data);
    if (result && id == 7) ventRequestAt = 0;
    // Nobody comes out of a vent while a human plays a scene (and 2 s after it).
    if (id == 6 && SceneHold(GetTickCount())) {
        if (result) InterlockedIncrement(&acceptsHeld);
        return 0;
    }
    if (id == 6 && PaceLeft(GetTickCount())) {
        if (result) InterlockedIncrement(&paceHeld);
        return 0;
    }
    if (id == 6 && result && necroDiedAt) {
        Note("respawn pace: vent open %lu ms after the death (wait %lu ms for %u human(s) and %u necro(s))",
             static_cast<unsigned long>(GetTickCount() - necroDiedAt), static_cast<unsigned long>(PaceWait(GetTickCount())), paceHumans, paceNecros);
        necroDiedAt = 0;
        InterlockedIncrement(&paceDone);
    }
    if (id >= 5 && id <= 12) {
        InterlockedIncrement(&transitionCalls[id - 5]);
        if (result) {
            if (id == 6) ventRequestAt = Stamp(GetTickCount());
            InterlockedIncrement(&transitionTrue[id - 5]);
            if (id == 6 || id == 7 || id == 8) {
                const uint32_t sm = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
                uint32_t ventRef = 0, ventFlags = 0, smFlags = 0; float inState = 0.0f;
                Read(sm + 0x44, &ventRef, 4); Read(sm + 0x54, &smFlags, 4); Read(sm + 0x30, &inState, 4);
                if (ventRef > 0x10) Read(ventRef - 0x10 + 0x2F0, &ventFlags, 4);
                Note("respawn transition %u (%s): vent %08X flags %08X, sm flags %08X, %.1f s in state", id,
                     id == 6 ? "vent accepted -> request" : id == 7 ? "request refused -> back to select" : "request granted or timed out -> finalize",
                     ventRef > 0x10 ? ventRef - 0x10 : 0, ventFlags, smFlags & 0xFF, inState);
            }
        }
    }
    if (id == 6 && confirmKey) {
        const bool down = GameWindowFocused() && (GetAsyncKeyState(confirmKey) & 0x8000) != 0;
        const bool pressed = down && !confirmKeyDown;
        confirmKeyDown = down;
        if (!result && pressed) {
            const uint32_t sm = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
            uint32_t ventRef = 0, ventFlags = 0;
            if (Read(sm + 0x44, &ventRef, 4) && ventRef > 0x10 && Read(ventRef - 0x10 + 0x2F0, &ventFlags, 4) && !(ventFlags & 0x60)) {
                InterlockedIncrement(&confirmKeyUsed);
                Note("vent confirm by key on vent %08X (flags %08X)", ventRef - 0x10, ventFlags);
                result = 1;
            } else Note("vent confirm by key: no selectable vent (ref %08X flags %08X)", ventRef, ventFlags);
        }
    }
    return result;
}
__declspec(noinline) void ProxySmCall(uint32_t rva, uint32_t sm, bool withVent) {
    __try {
        if (withVent) reinterpret_cast<void(__thiscall*)(uint32_t, uint32_t)>(base + rva)(sm, 0);
        else reinterpret_cast<void(__thiscall*)(uint32_t)>(base + rva)(sm);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&invasionFaults);
    }
}
unsigned char __fastcall OnStateMessage(void* self, void*, uint32_t state, float dt, uint32_t msg, uint32_t arg, const void* data) {
    // The switch is on the fourth argument ([ebp+0x14] in 0x010DD650), minus 3.
    if (arg >= 3 && arg <= 21) InterlockedIncrement(&stateMessages[arg - 3]);
    // Message 6 is the select states' update: with the selection dropped, the engine picks again
    // (OnAutoSelectVent, near the humans) and moves the camera there.
    bool recentred = false;
    const uint32_t sm = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    if (arg == 6 && recentreWanted && invasion) {
        const DWORD now = GetTickCount();
        uint32_t vtable = 0, proxyRef = 0;
        if (now - lastRecentre >= recentreCooldownMs && Read(sm, &vtable, 4) && vtable == base + kProxySmVtableRva && Read(sm + 0x3C, &proxyRef, 4) &&
            proxyRef > 0x10 && proxyRef - 0x10 == ghostProxy) {
            recentreWanted = false;
            lastRecentre = now;
            ProxySmCall(kSetSelectedVentRva, sm, true);
            recentred = true;
            InterlockedIncrement(&recentres);
        }
    }
    const unsigned char result = stateMessageHook.thiscall<unsigned char>(self, state, dt, msg, arg, data);
    if (recentred) {
        ProxySmCall(kRefreshVentsRva, sm, false);
        Note("invasion: the ghost camera moved to the humans' room");
    }
    return result;
}
// A sixth necro type, without touching the engine's five.
//
// NecroPlayer::Spawn does not switch on the type, it indexes. Read out of the
// PC binary on 22.09.:
//
//     mov eax, [0x02021624]        ; MPLevelSettings singleton
//     mov ecx, [ebp+0x14]          ; the type argument
//     mov eax, [eax+0x0C]          ; its attributes
//     shl ecx, 4                   ; type * 16
//     lea edx, [ecx + eax + 0x78]  ; the template GUID for that type
//
// The five authored slots are Pack +0x78, Lurker +0x88, Spitter +0x98, Puker
// +0xA8 and the never-filled DividerHead +0xB8. Index 5 therefore lands on
// +0xC8, which is CMD_SetRespawnProxy: a real field belonging to something
// else, and the reason a sixth type looked impossible.
//
// So the sixth type never reaches the engine as a sixth index. The respawn
// manager uses 5 for "nothing picked", which is why the plugin's own number for
// it is 6. But index 6 would land on +0xD8..+0xE7, and that is not free:
// VentSelectionCastDistance, m_ventSelectionCastDisplacement,
// m_ventCameraBlendTime and the start of m_ventHighlightActiveVFX. The spawn
// reads one of them: NecroPlayer::InitCameras loads m_ventCameraBlendTime
// (+0xE0) when the vent exit state is entered, which happens inside the spawn.
// A borrowed GUID word there (0x5041454C) is a blend of about 1.3e10 seconds.
// (Xbox debug build, 22.09.; the first build borrowed exactly that.)
//
// The one field nobody reads is the fifth: CMD_SetDividerHeadNecroPlayerGUID
// at +0xB8, never authored in retail. And NecroPlayer::Spawn uses its type
// argument for nothing but GetNecroPlayerGUID; the NecroPlayer's own m_type
// comes from its record (+0x194), not from the argument. So a sixth-type spawn
// lends the DividerHead field the configured template GUID for the length of the one call
// and asks the engine for type 4. Nothing is patched, no array is resized, and
// if the write fails the spawn simply runs as it would have.
namespace sixth {
constexpr uint32_t kSettingsSingleton = 0x1C21624;   // [0x02021624] MPLevelSettings
constexpr uint32_t kSettingsAttributes = 0xC, kTemplateBase = 0x78, kSlotStride = 0x10;
bool enabled = false;
uint32_t slot = 5;
uint32_t guid[4]{};
volatile LONG lends = 0, refused = 0;
constexpr uint32_t kLookupSlot = 4;   // the DividerHead field: never authored, never read

bool Configured() { return enabled && (guid[0] | guid[1] | guid[2] | guid[3]) != 0; }

uint32_t Attributes() {
    uint32_t levelSettings = 0, attributes = 0;
    if (!Read(base + kSettingsSingleton, &levelSettings, 4) || !levelSettings) return 0;
    return (Read(levelSettings + kSettingsAttributes, &attributes, 4) && attributes) ? attributes : 0;
}

// Borrowed for exactly as long as the call it wraps.
struct Borrow {
    uint32_t address = 0;
    uint32_t saved[4]{};
    bool active = false;
    explicit Borrow(uint32_t wanted) {
        if (!Configured() || wanted != slot) return;
        const uint32_t attributes = Attributes();
        if (!attributes) { InterlockedIncrement(&refused); return; }
        const uint32_t at = attributes + kTemplateBase + kLookupSlot * kSlotStride;
        if (!Read(at, saved, sizeof(saved)) || !WriteBytes(at, guid, sizeof(guid))) { InterlockedIncrement(&refused); return; }
        address = at;
        active = true;
        InterlockedIncrement(&lends);
    }
    ~Borrow() { if (active) WriteBytes(address, saved, sizeof(saved)); }
    // The type the engine is asked for: the lent field's own index.
    uint32_t EngineType(uint32_t wanted) const { return active ? kLookupSlot : wanted; }
    Borrow(const Borrow&) = delete;
    Borrow& operator=(const Borrow&) = delete;
};

void Configure(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Necro", "SixthType", 0, ini.c_str()) != 0;
    // 6, not 5: NecroPlayerRespawnManager uses 5 for "nothing picked yet", so a
    // sixth type sitting on 5 would read as no selection. It is only the
    // plugin's number: the engine is asked for type 4 (see Borrow).
    slot = static_cast<uint32_t>(GetPrivateProfileIntA("Necro", "SixthTypeSlot", 6, ini.c_str()));
    // 0-3 are the four real types and 5 means "nothing picked".
    if (slot < 4 || slot == 5 || slot > 255) {
        if (enabled) std::fprintf(log, "necro sixth_type: SixthTypeSlot=%u collides with a real type or is out of range, using 6\n", slot);
        slot = 6;
    }
    char text[64]{};
    GetPrivateProfileStringA("Necro", "SixthTypeGuid", "", text, sizeof(text), ini.c_str());
    // Four hex words separated by dashes, parsed by hand: sscanf is deprecated
    // here and the build carries no warnings.
    uint32_t parsed[4]{};
    const char* cursor = text;
    unsigned filled = 0;
    for (; filled < 4 && *cursor; ++filled) {
        char* end = nullptr;
        const unsigned long word = std::strtoul(cursor, &end, 16);
        if (end == cursor) break;
        parsed[filled] = static_cast<uint32_t>(word);
        cursor = (*end == '-') ? end + 1 : end;
    }
    if (filled == 4) std::memcpy(guid, parsed, sizeof(guid));
    if (!enabled) return;
    std::fprintf(log, "necro sixth_type enabled slot=%u guid=%08X-%08X-%08X-%08X (engine asked for type 4; borrows the DividerHead field at attributes+%02X for each spawn)\n",
                 slot, guid[0], guid[1], guid[2], guid[3], kTemplateBase + kLookupSlot * kSlotStride);
    if (!Configured()) std::fprintf(log, "necro sixth_type: no SixthTypeGuid given, so nothing will be lent\n");
}
}  // namespace sixth
// The Leaper, made playable without a new archive.
//
// A NecroPlayer is described entirely by its attribute block, and the block is
// not the authored record: the loader has already resolved it. Measured in a
// live session (dump 31064): MPLevelSettings' msg_ventSelectHighlight, authored
// as the text "aud_mp_vent_highlighted", reads 0x74050556 at attributes+0x130,
// exactly sdbm("aud_mp_vent_highlighted"). Every authored name is a name hash
// at run time, and every authored GUID is its sixteen bytes inline.
//
// So a sixth necromorph needs no record built, no stream packed and no TOC. It
// needs the template's attribute block to name the Leaper's model and the
// Leaper's animations instead of the Lurker's. Every animation below was read
// out of the Leaper's own banks, so nothing here is invented:
//
//   leaper_locomote_base.bnk   idle, walk, run_old, leaper_unaware_idle
//   leaper_scripted.bnk        Leaper_crawloutvent, leaper_vent_*
//   leaper_attack_paired.bnk   leaper_grab_attack*, and Isaac's pc_leaper_*
//   leaper_jumps.bnk           jump_attack_*, jump_leap_*
//   leaper_hit_reacts.bnk      knockdown, knockdown_getup, knockback_wall
//   leaper_death.bnk           leaper_die
//
// The one thing it has no animation for is strafing: the Lurker was given four
// directional runs for multiplayer and the Leaper only ever ran forwards, so
// all four names point at the same run and moving sideways slides. A deliberate
// trade, chosen on 22.09.
//
// The block is shared by every necro spawned from that template, so the change
// is remembered field by field and put back on revert. Only the fields listed
// here are touched; the block's size never has to be known.
namespace leaper {
bool enabled = false;
// Offsets inside a NecroPlayer attribute block, from the class descriptor's own
// attribute table (tools/dump_attribute_offsets.py).
constexpr uint32_t kModel = 0x000, kRcb = 0x020, kModelStream = 0x040;
constexpr uint32_t kIdleAnim = 0x0DC, kDeathAnim = 0x0E0;
constexpr uint32_t kLocoForwRight = 0x0E8, kLocoForwLeft = 0x0EC, kLocoBackRight = 0x0F0, kLocoBackLeft = 0x0F4;
constexpr uint32_t kRunForwRight = 0x0F8, kRunForwLeft = 0x0FC, kRunBackRight = 0x100, kRunBackLeft = 0x104;
constexpr uint32_t kExitFloorVent = 0x118;
constexpr uint32_t kExitWallLaunch = 0x130, kExitWallLoop = 0x134, kExitWallLand = 0x138;
constexpr uint32_t kExitCeilLaunch = 0x160, kExitCeilLoop = 0x164, kExitCeilLand = 0x168;

uint32_t NameHash(const char* text) {
    uint32_t hash = 0;
    for (const char* c = text; *c; ++c) {
        const char lower = (*c >= 'A' && *c <= 'Z') ? static_cast<char>(*c + 32) : *c;
        hash = hash * 0x1003F + static_cast<unsigned char>(lower);
    }
    return hash;
}

struct AnimPatch { uint32_t offset; const char* name; };
const AnimPatch kAnims[] = {
    {kIdleAnim, "leaper_unaware_idle"},
    {kDeathAnim, "leaper_die"},
    {kLocoForwRight, "walk"}, {kLocoForwLeft, "walk"},
    {kLocoBackRight, "walk"}, {kLocoBackLeft, "walk"},
    {kRunForwRight, "run_old"}, {kRunForwLeft, "run_old"},
    {kRunBackRight, "run_old"}, {kRunBackLeft, "run_old"},
    {kExitFloorVent, "Leaper_crawloutvent"},
    {kExitWallLaunch, "leaper_vent_wall_climb_out"},
    {kExitWallLoop, "leaper_vent_jump_up_loop"},
    {kExitWallLand, "leaper_vent_jump_up_end"},
    {kExitCeilLaunch, "leaper_vent_drop_down"},
    {kExitCeilLoop, "leaper_vent_drop_down_loop"},
    {kExitCeilLand, "leaper_vent_drop_down_land"},
};
// The Leaper's own resources, out of its character stream.
const uint32_t kLeaperModel[4] = {0xACB61CC3u, 0xACB61CC3u, 0xACB61CC3u, 0x2F24BB5Bu};
const uint32_t kLeaperRcb[4] = {0x48047A29u, 0x48047A29u, 0x48047A29u, 0xFBABFF51u};
const uint32_t kLeaperStream[4] = {0x36C83B5Fu, 0x95EB02FEu, 0x44455257u, 0x34303245u};

bool applied = false, patchModel = true;   // `enabled` is declared up top, the stream request needs it early
uint32_t patchedBlock = 0;
uint32_t savedAnims[std::size(kAnims)]{};
uint32_t savedModel[4]{}, savedRcb[4]{}, savedStream[4]{};
volatile LONG applications = 0;

void Apply(uint32_t attributes) {
    if (!enabled || applied || !attributes) return;
    for (unsigned i = 0; i < std::size(kAnims); ++i) {
        if (!Read(attributes + kAnims[i].offset, &savedAnims[i], 4)) return;
    }
    if (!Read(attributes + kModel, savedModel, sizeof(savedModel)) ||
        !Read(attributes + kRcb, savedRcb, sizeof(savedRcb)) ||
        !Read(attributes + kModelStream, savedStream, sizeof(savedStream))) return;
    unsigned written = 0;
    for (const auto& patch : kAnims) {
        const uint32_t hash = NameHash(patch.name);
        if (WriteBytes(attributes + patch.offset, &hash, 4)) ++written;
    }
    if (patchModel) {
        WriteBytes(attributes + kModel, kLeaperModel, sizeof(kLeaperModel));
        WriteBytes(attributes + kRcb, kLeaperRcb, sizeof(kLeaperRcb));
        WriteBytes(attributes + kModelStream, kLeaperStream, sizeof(kLeaperStream));
    }
    applied = true;
    patchedBlock = attributes;
    InterlockedIncrement(&applications);
    Note("leaper: attribute block %08X now describes a Leaper (%u of %u animation names, model %s)",
         attributes, written, static_cast<unsigned>(std::size(kAnims)), patchModel ? "swapped" : "left alone");
}

void Restore() {
    if (!applied || !patchedBlock) return;
    for (unsigned i = 0; i < std::size(kAnims); ++i) WriteBytes(patchedBlock + kAnims[i].offset, &savedAnims[i], 4);
    if (patchModel) {
        WriteBytes(patchedBlock + kModel, savedModel, sizeof(savedModel));
        WriteBytes(patchedBlock + kRcb, savedRcb, sizeof(savedRcb));
        WriteBytes(patchedBlock + kModelStream, savedStream, sizeof(savedStream));
    }
    Note("leaper: attribute block %08X put back", patchedBlock);
    applied = false;
    patchedBlock = 0;
}
// The block is gone with its stream: nothing to put back.
void Forget() {
    if (applied && patchedBlock) Note("leaper: attribute block %08X left with its stream, nothing put back", patchedBlock);
    applied = false;
    patchedBlock = 0;
}

void Configure(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Necro", "Leaper", 0, ini.c_str()) != 0;
    patchModel = GetPrivateProfileIntA("Necro", "LeaperModel", 1, ini.c_str()) != 0;
    if (enabled) std::fprintf(log, "necro leaper enabled (model swap %d, %u animation names)\n",
                              patchModel, static_cast<unsigned>(std::size(kAnims)));
}
}  // namespace leaper

// The spawn places the necro with VentMarker::GetEdgeMidPointMatrix(out, 1, b1,
// 0): the vent's first axis (half extent at vent+0x300), sign from b1; for a
// wall vent its top or bottom edge. NecroRespawnProxySM derives b1 from the
// ghost camera's facing (whichever edge's matrix forward lines up), which in
// the campaign gave the top edge and a necro dropping in from above the vent.
// NecroPlayerExitVentSM::InitExitVent picks its edge its own way, so the spawn
// takes that same edge (EngineExitEdge).
// b1/b2 arrive as bytes (only the low byte is the flag).
volatile LONG spawnEdgeOverrides = 0;
int spawnEdge = -1;   // [Necro] SpawnEdge: -1 = the lower edge by the vent axis, 0/1 = force that sign (experiment)
float lastVentAxisA[4]{}, lastVentAxisB[4]{}, lastSpawnPos[3]{};
uint32_t lastSpawnRequested = 0xFFFFFFFFu;
// Whether an engine type's template has instance data here (its character stream is resident).
// 17.10. (crash 39376): the type menu offered the spitter, whose stream a sixth-type setup never
// loads; GetSpawnInfo came back invalid, SpawnEntity returned null and NecroPlayer::Spawn handed
// that to RegisterWithManager (0x0108C313).
__declspec(noinline) bool SpawnInfoSafe(uint32_t sim, SpawnInfo& info, const uint32_t* guid) {
    __try {
        return GetSpawnInfo(sim, info, guid);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The template record of all four types sits in the pool root, so GetSpawnInfo answers for every
// one of them; only a type whose character stream is in spawns (17.10., crash 52780: the lurker
// passed the record test and SpawnEntity still gave null). Engine type 4 is the sixth type's lent
// slot, its character stream the Leaper's.
bool TypeModelLoaded(uint32_t engineType) {
    if (engineType == 4) return leaperModelStatus == 3;
    if (engineType > 3) return false;
    const int which = type < 0 || type > 3 ? 0 : type;
    return static_cast<int>(engineType) == which ? modelStatus == 3 : extraModelStatus[engineType] == 3;
}
bool TypeSpawnable(uint32_t engineType) {
    if (!TypeModelLoaded(engineType)) return false;
    uint32_t sim = 0, guid[4]{};
    const uint32_t attributes = sixth::Attributes();
    if (!attributes || engineType > 4 || !Read(attributes + sixth::kTemplateBase + engineType * sixth::kSlotStride, guid, 16) || !(guid[0] | guid[1] | guid[2] | guid[3]))
        return false;
    if (!Read(base + kSimManager, &sim, 4) || !sim) return false;
    SpawnInfo info{};
    return SpawnInfoSafe(sim, info, guid);
}
// The type to use instead: the configured one first, then any of the four. 0xFFFFFFFF: none.
uint32_t SpawnableFallback() {
    const uint32_t configured = type >= 0 && type <= 3 ? static_cast<uint32_t>(type) : 0u;
    if (TypeSpawnable(configured)) return configured;
    for (uint32_t t = 0; t < 4; ++t)
        if (TypeSpawnable(t)) return t;
    return 0xFFFFFFFFu;
}
volatile LONG spawnTypeFallbacks = 0, spawnsRefusedNoTemplate = 0, menuTypeFallbacks = 0, spawnNullCaught = 0;
// The last line of defence for the 0x0108C313 family (dumps 32388, 39376, 48976, 49984, 52780):
// should SpawnEntity still give null inside NecroPlayer::Spawn, RegisterWithManager reads
// [null+0x3DC] as its first access. Only that one fault is caught; nothing has changed by then.
int SpawnNullFilter(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* r = info ? info->ExceptionRecord : nullptr;
    return r && r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && reinterpret_cast<uintptr_t>(r->ExceptionAddress) == base + kRegisterWithManager + 3
               ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
__declspec(noinline) uint32_t NativeSpawnGuarded(uint32_t playerId, uint32_t subKey, uint32_t vent, uint32_t engineType, uint32_t side, uint32_t b2) {
    __try {
        return spawnHook.call<uint32_t>(playerId, subKey, vent, engineType, side, b2);
    } __except (SpawnNullFilter(GetExceptionInformation())) {
        InterlockedIncrement(&spawnNullCaught);
        return 0;
    }
}
uint32_t __cdecl OnNecroSpawn(uint32_t playerId, uint32_t subKey, uint32_t vent, uint32_t spawnType, uint32_t b1, uint32_t b2) {
    InterlockedIncrement(&spawnCalls);
    uint32_t side = b1 & 0xFF;
    lastSpawnRequested = side;
    std::memset(lastVentAxisA, 0, sizeof(lastVentAxisA)); std::memset(lastVentAxisB, 0, sizeof(lastVentAxisB));
    if (vent) { Read(vent + 0x300, lastVentAxisA, 16); Read(vent + 0x320, lastVentAxisB, 16); }
    // No "lower edge" rule: NecroPlayerExitVentSM::InitExitVent probes gravity
    // along the vent's second axis (the normal of a wall vent, which has no
    // component there) and the MP necro leaves a wall vent from the upper edge
    // with the launch / loop / land animation. The exit SM's own pick
    // (EngineExitEdge) stands unless [Necro] SpawnEdge forces one (experiment).
    uint32_t spawnAttributes = 0, spawnVentType = 0xFFFFFFFFu;
    if (vent && Read(vent + kVentAttributes, &spawnAttributes, 4) && spawnAttributes) Read(spawnAttributes + kVentTypeAttribute, &spawnVentType, 4);
    const uint32_t engineEdge = static_cast<uint32_t>(EngineExitEdge(vent, spawnVentType));
    if (side != engineEdge) InterlockedIncrement(&spawnEdgeOverrides);
    side = engineEdge;
    if (spawnEdge == 0 || spawnEdge == 1) side = static_cast<uint32_t>(spawnEdge);   // [Necro] SpawnEdge still wins, for experiments
    WriteExitCamera(vent, static_cast<int>(side), "spawn");
    // The engine's own respawn after a death can arrive with type 5 (the
    // respawn manager's "nothing picked yet"), and index 5 of the template
    // table is CMD_SetRespawnProxy. Spawn then builds a respawn proxy, treats
    // it as a NecroPlayer and faults at 0x007429F0 unlinking a "spawn point"
    // that is really a float (dump 26436, 22.09. 21:16: type 5 on the stack,
    // vtable 01B596E0 instead of the NecroPlayer's). Index 4, the never-filled
    // DividerHead slot, is empty and no better. Neither is ever a type anybody
    // asked for, so they become the type this machine plays.
    if (spawnType > 3 && !(sixth::Configured() && spawnType == sixth::slot)) {
        const uint32_t repaired = (sixth::Configured() && type == static_cast<int>(sixth::slot)) ? sixth::slot
                                : (type >= 0 && type <= 3) ? static_cast<uint32_t>(type) : 0u;
        InterlockedIncrement(&spawnTypeRepairs);
        Note("NecroPlayer::Spawn asked for type %u (no template of its own); spawning type %u instead", spawnType, repaired);
        spawnType = repaired;
    }
    sixth::Borrow borrowed(spawnType);   // a sixth type lends the neighbouring field its template GUID for this one call
    uint32_t engineType = borrowed.EngineType(spawnType);
    if (!TypeSpawnable(engineType)) {
        const uint32_t fallback = SpawnableFallback();
        if (fallback == 0xFFFFFFFFu) {
            InterlockedIncrement(&spawnsRefusedNoTemplate);
            Note("NecroPlayer::Spawn type %u (engine %u): no type has instance data here, NOT spawned (the native spawn would crash)", spawnType, engineType);
            gate::Event("necro: spawn of type %u refused, no necro template is loaded here", spawnType);
            return 0;
        }
        InterlockedIncrement(&spawnTypeFallbacks);
        gate::Event("necro: type %u (engine %u) has no instance data here (its stream is not loaded): spawning %s instead", spawnType, engineType, kTypeNames[fallback]);
        engineType = fallback;
    }
    const bool usedBorrow = borrowed.active && engineType == borrowed.EngineType(spawnType);
    const LONG caughtBefore = InterlockedCompareExchange(&spawnNullCaught, 0, 0);
    const uint32_t result = NativeSpawnGuarded(playerId, subKey, vent, engineType, side, b2 & 0xFF);
    if (InterlockedCompareExchange(&spawnNullCaught, 0, 0) != caughtBefore)
        gate::Event("test vent_spawn FAIL: NecroPlayer::Spawn type %u got no entity from SpawnEntity; its null RegisterWithManager was caught", engineType);
    lastSpawnResult = result;
    if (result && usedBorrow) {
        uint32_t attributes = 0;
        if (Read(result + kSettingsAttributes, &attributes, 4) && attributes) leaper::Apply(attributes);
    }
    lastSpawnSide = side; lastSpawnFlagB2 = b2 & 0xFF;
    gate::Event("test vent_spawn %s: NecroPlayer::Spawn type %u at vent %08X -> %08X", result ? "PASS" : "FAIL", engineType, vent, result);
    std::memset(lastSpawnPos, 0, sizeof(lastSpawnPos));
    if (result) Read(result + kEntityMatrix + 0x30, lastSpawnPos, 12);
    Note("NecroPlayer::Spawn(id %u, subkey %08X, vent %08X, type %u%s, edge %u (asked %u), %u) -> %08X at (%.1f %.1f %.1f); vent axis A %.2f %.2f %.2f, axis B %.2f %.2f %.2f",
         playerId, subKey, vent, spawnType, borrowed.active ? " [sixth slot, template lent]" : "", side, b1 & 0xFF, b2 & 0xFF, result, lastSpawnPos[0], lastSpawnPos[1], lastSpawnPos[2],
         lastVentAxisA[0], lastVentAxisA[1], lastVentAxisA[2], lastVentAxisB[0], lastVentAxisB[1], lastVentAxisB[2]);
    return result;
}
// NecroPlayerExitVentSM (PC vtable 0x01B7927C): CheckTransition 0x010A63F0
// (ids 0xB skip anim when the vent SafePtr [sm+0x304] is null, 0xC/0xD/0xE
// vent type [sm+0x30C] == 0/1/2 -> done/loop/loop, 0xF landed, 0x10 anim done)
// and HandleStateMessage 0x010D0C00 (msgs 0x12..0x1B: init exit vent, launch
// anim, ..., loop anim, ..., land). Counted, and the SM's vent and type noted.
safetyhook::InlineHook exitTransitionHook, exitMessageHook;
volatile LONG exitTransitionTrue[6]{}, exitMessages[10]{};
uint32_t exitSmVent = 0, exitSmType = 0xFFFFFFFFu, exitSmSeen = 0;
unsigned char __fastcall OnExitTransition(void* self, void*, uint32_t a, float dt, uint32_t id, const void* data) {
    const unsigned char result = exitTransitionHook.thiscall<unsigned char>(self, a, dt, id, data);
    const uint32_t sm = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    if (sm != exitSmSeen) {
        exitSmSeen = sm;
        uint32_t ventRef = 0;
        Read(sm + 0x304, &ventRef, 4); Read(sm + 0x30C, &exitSmType, 4);
        exitSmVent = ventRef > 0x10 ? ventRef - 0x10 : 0;
        Note("exit vent SM %08X: vent %08X, type %u", sm, exitSmVent, exitSmType);
    }
    if (result && id >= 0xB && id <= 0x10) InterlockedIncrement(&exitTransitionTrue[id - 0xB]);
    return result;
}
// MPLevelSettings gone before the exit vent SM (client crash 15156, 17.10.: a world teardown deleted
// the settings, the deferred delete flush reached NecroPlayer's SM shutdown later, and the SM's
// destructor 0x010D0B00 read [[0x02021624]+0xC]+0xE0, its vent camera blend time, at 0x010D0B24).
// The destructor (thiscall, no stack arguments, tail jump to the base destructor) and the exit SM's
// messages 0x18 (pop the vent camera, 0x0108C9C0) and 0x19 (InitCameras, 0x010C1270) read it
// without a check. Only while the singleton is null they run on a zeroed stand-in (blend time 0)
// and under SEH; the singleton is null again afterwards.
alignas(16) unsigned char settingsStandIn[0x200]{};
alignas(16) unsigned char settingsStandInAttributes[0x800]{};
volatile LONG standInUses = 0, standInFaults = 0;
bool PlantSettingsStandIn() {
    volatile uint32_t* const singleton = reinterpret_cast<volatile uint32_t*>(base + kMPLevelSettings);
    if (*singleton) return false;
    std::memset(settingsStandIn, 0, sizeof(settingsStandIn));
    std::memset(settingsStandInAttributes, 0, sizeof(settingsStandInAttributes));
    const uint32_t attributes = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(settingsStandInAttributes));
    std::memcpy(settingsStandIn + kSettingsAttributes, &attributes, 4);
    *singleton = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(settingsStandIn));
    return true;
}
void LiftSettingsStandIn(const char* where, uint32_t object, bool ok) {
    volatile uint32_t* const singleton = reinterpret_cast<volatile uint32_t*>(base + kMPLevelSettings);
    if (*singleton == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(settingsStandIn))) *singleton = 0;
    if (!ok) InterlockedIncrement(&standInFaults);
    if (InterlockedIncrement(&standInUses) <= 20)
        gate::Event("test teardown_guard INFO: %s %08X ran without MPLevelSettings, on the zeroed stand-in%s", where, object,
                    ok ? "" : ", and faulted anyway (caught)");
}
__declspec(noinline) bool ExitMessageGuarded(void* self, uint32_t a, float dt, uint32_t b, uint32_t msg, const void* data, unsigned char& result) {
    __try {
        result = exitMessageHook.thiscall<unsigned char>(self, a, dt, b, msg, data);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        result = 0;
        return false;
    }
}
unsigned char __fastcall OnExitMessage(void* self, void*, uint32_t a, float dt, uint32_t b, uint32_t msg, const void* data) {
    if (msg >= 0x12 && msg <= 0x1B) InterlockedIncrement(&exitMessages[msg - 0x12]);
    if ((msg == 0x18 || msg == 0x19) && PlantSettingsStandIn()) {
        unsigned char result = 0;
        const bool ok = ExitMessageGuarded(self, a, dt, b, msg, data, result);
        LiftSettingsStandIn(msg == 0x18 ? "exit vent SM message 0x18 (pop vent camera)" : "exit vent SM message 0x19 (InitCameras)",
                            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)), ok);
        return result;
    }
    return exitMessageHook.thiscall<unsigned char>(self, a, dt, b, msg, data);
}
constexpr uint32_t kExitVentDtorRva = 0xCD0B00;
safetyhook::InlineHook exitVentDtorHook;
__declspec(noinline) bool ExitDtorGuarded(void* self) {
    __try {
        exitVentDtorHook.thiscall<void>(self);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        return false;
    }
}
void __fastcall OnExitVentDtor(void* self, void*) {
    if (!PlantSettingsStandIn()) { exitVentDtorHook.thiscall<void>(self); return; }
    const bool ok = ExitDtorGuarded(self);
    LiftSettingsStandIn("NecroPlayerExitVentSM destructor", static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)), ok);
}
void InstallTeardownGuard(FILE* log) {
    // push esi / push 0 / push 0 / push ecx / mov esi,ecx / mov [esi],01B7927C / mov eax,esp / mov [eax],3 / mov eax,[02021624]
    static const unsigned char kDtor[] = {0x56, 0x6A, 0x00, 0x6A, 0x00, 0x51, 0x8B, 0xF1, 0xC7, 0x06, 0x7C, 0x92, 0xB7, 0x01,
                                          0x8B, 0xC4, 0xC7, 0x00, 0x03, 0x00, 0x00, 0x00, 0xA1, 0x24, 0x16, 0x02, 0x02};
    const bool ok = hook_site::Install(log, "necro NecroPlayerExitVentSM destructor", base + kExitVentDtorRva, kDtor, sizeof(kDtor),
                                       reinterpret_cast<void*>(OnExitVentDtor), exitVentDtorHook);
    std::fprintf(log, "necro teardown guard: exit vent SM destructor without MPLevelSettings %s; messages 0x18/0x19 %s\n", ok ? "on a stand-in" : "NOT GUARDED",
                 exitMessageHook ? "on a stand-in" : "not guarded (respawn trace off)");
}
void InstallRespawnTrace(FILE* log) {
    struct { uint32_t rva; const char* bytes; size_t n; void* handler; safetyhook::InlineHook* hook; const char* what; } sites[5] = {
        {0xCA8000, "\x53\x56\x8B\xF1\x8B\x4C\x24\x14\x8D\x41\xFB\x32\xDB", 13, reinterpret_cast<void*>(OnCheckTransition), &checkTransitionHook, "CheckTransition"},
        {0xCA5650, "\x55\x8B\xEC\x83\xE4\xF0\x83\xEC\x74\x53\x8B\x5D\x10\x56", 14, reinterpret_cast<void*>(OnNecroSpawn), &spawnHook, "NecroPlayer::Spawn"},
        {0xCDD650, "\x55\x8B\xEC\x83\xE4\xF0\x83\xEC\x74\x53\x56\x8B\xF1", 13, reinterpret_cast<void*>(OnStateMessage), &stateMessageHook, "HandleStateMessage"},
        {0xCA63F0, "\x56\x8B\xF1\x8B\x4C\x24\x10\x8D\x41\xF5\x83\xF8\x05", 13, reinterpret_cast<void*>(OnExitTransition), &exitTransitionHook, "ExitVentSM::CheckTransition"},
        {0xCD0C00, "\x8B\x54\x24\x10\x53\x8D\x42\xEE\x32\xDB\x56\x83\xF8\x09", 14, reinterpret_cast<void*>(OnExitMessage), &exitMessageHook, "ExitVentSM::HandleStateMessage"}};
    for (auto& s : sites) {
        unsigned char actual[16]{};
        if (!Read(base + s.rva, actual, s.n) || std::memcmp(actual, s.bytes, s.n) != 0) { std::fprintf(log, "necro respawn_trace %s: signature mismatch at %08X\n", s.what, static_cast<unsigned>(base + s.rva)); continue; }
        *s.hook = safetyhook::create_inline(reinterpret_cast<void*>(base + s.rva), s.handler);
        std::fprintf(log, "necro respawn_trace %s hook %s at %08X\n", s.what, *s.hook ? "installed" : "FAILED", static_cast<unsigned>(base + s.rva));
    }
    std::fprintf(log, "necro respawn_trace confirm_key=%d\n", confirmKey);
}

// Listener guard. The RWS message dispatch (loop at 0x00756CD0) walks the
// message's listener nodes, takes the listener object ([node+0xC]) and calls
// its vtable slot 1 (HandleEvents). A listener freed without unlinking leaves
// its node behind; once the memory is reused the call lands anywhere (client
// crashes 1856/20960 at 0x013E3A74/0x013E3BBB on the restart after a death:
// aud_global_unloader_lua_startup_complete reached a stale listener whose
// "Lua thread" was a reused heap object). The mid hook at 0x00756CEB (mov ecx,
// eax; mov eax,[ecx]; push edx; mov edx,[eax+4]; call edx) checks that the
// listener's vtable lies inside deadspace2.exe; otherwise the call goes to a
// no-op stand-in and the stale address is logged, so the log tells which
// teardown left it behind.
safetyhook::MidHook listenerGuardHook;
bool listenerGuard = true;
uint32_t imageEnd = 0;
volatile LONG listenerGuarded = 0;
struct StaleListener { uint32_t object, vtable, hash; LONG hits; };
// The restart crash, traced in full on 22.09. from the dumps of 14:01 to 14:32
// (three at 0x013E3A74, one at 0x013E3BBB, all in lua_pushstring and similar):
// a LuaAudioScript entity caches its lua_State at [listener+0x60]. The level
// restart builds a NEW state: dump 34144 (14:21) had [manager+0x1C] =
// 0x0A75AA18 and the listener agreed; dump 33928 (14:32) had the manager on
// 0x22C51868 while the listener still held 0x0A75AA18, whose memory was reused
// (its global-state pointer at +0x10 read 0x18). The next message that reached
// that listener walked into it. The vtable check above cannot see this: the
// listener itself is intact, only the state it cached is dead. So for exactly
// this handler the cached state is verified first.
constexpr uint32_t kLuaScriptHandleEventsRva = 0x4187D0;   // the vtable slot 1 of the Lua script listener (0x008187D0)
constexpr uint32_t kLuaStateCacheOffset = 0x60, kLuaGlobalStateOffset = 0x10;
volatile LONG deadLuaStates = 0;
StaleListener staleListeners[8]{};
void __fastcall NoopHandleEvents(void*, void*, void*) {}
void* noopVtable[8] = {reinterpret_cast<void*>(NoopHandleEvents), reinterpret_cast<void*>(NoopHandleEvents), reinterpret_cast<void*>(NoopHandleEvents), reinterpret_cast<void*>(NoopHandleEvents),
                       reinterpret_cast<void*>(NoopHandleEvents), reinterpret_cast<void*>(NoopHandleEvents), reinterpret_cast<void*>(NoopHandleEvents), reinterpret_cast<void*>(NoopHandleEvents)};
struct NoopListener { void** vtable; uint32_t pad[8]; } noopListener = {noopVtable, {}};
void OnListenerCall(SafetyHookContext& ctx) {
    const uint32_t object = static_cast<uint32_t>(ctx.eax);
    uint32_t vtable = 0;
    const char* why = "stale listener";
    if (object >= 0x10000 && Read(object, &vtable, 4) && vtable >= base + 0x1000 && vtable < imageEnd) {
        uint32_t handler = 0, luaState = 0, globalState = 0;
        if (!Read(vtable + 4, &handler, 4) || handler != base + kLuaScriptHandleEventsRva) return;
        if (!Read(object + kLuaStateCacheOffset, &luaState, 4) || luaState < 0x10000) return;   // nothing cached yet: the handler builds it itself
        if (Read(luaState + kLuaGlobalStateOffset, &globalState, 4) && globalState >= 0x10000) return;   // the state is alive
        why = "dead lua state";
        InterlockedIncrement(&deadLuaStates);
    }
    uint32_t hash = 0;
    Read(static_cast<uint32_t>(ctx.edx), &hash, 4);
    InterlockedIncrement(&listenerGuarded);
    for (auto& s : staleListeners) {
        if (s.object == object && s.hash == hash) { InterlockedIncrement(&s.hits); break; }
        if (!s.object) {
            s.object = object; s.vtable = vtable; s.hash = hash; s.hits = 1;
            Note("listener guard: %s %08X (vtable %08X) skipped for message %08X", why, object, vtable, hash);
            break;
        }
    }
    ctx.eax = reinterpret_cast<uintptr_t>(&noopListener);
}
// Nearest player to a point (PC 0x00FE59A0, cdecl(const float* point) ->
// Player*): walks the PlayerManager slots, QueryInterface(Player) on each,
// then reads the CMC position of the result without a null check. With a
// partner's necro in a slot the query fails and it crashes (host 16308 at
// 0x00FE5A39). Nine callers, all AI-side. Reimplemented: Players only, same
// answer, necros skipped.
safetyhook::InlineHook nearestPlayerHook;
volatile LONG nearestPlayerCalls = 0;
constexpr uint32_t kNearestPlayerRva = 0xBE59A0, kGetCmcOfSentientRva = 0xAB8D40;
uint32_t __cdecl OnNearestPlayer(const float* point) {
    InterlockedIncrement(&nearestPlayerCalls);
    uint32_t manager = 0, slots = 0, capacity = 0, best = 0;
    float bestDistance = 3.4e38f;
    if (!point || !Read(base + kPlayerManager, &manager, 4) || !manager || !Read(manager + 4, &slots, 4) || !slots ||
        !Read(manager + 8, &capacity, 4) || capacity > 8) return 0;
    for (uint32_t i = 0; i < capacity; ++i) {
        uint32_t bp = 0, vtable = 0;
        if (!Read(slots + 4 * i, &bp, 4) || bp <= kBasePlayer) continue;
        const uint32_t entity = bp - kBasePlayer;
        if (!Read(entity, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva) continue;   // a necro is not a Player
        using GetCmc = uint32_t (__thiscall*)(uint32_t);
        const uint32_t cmc = reinterpret_cast<GetCmc>(base + kGetCmcOfSentientRva)(entity);
        float position[3]{};
        if (!cmc || !Read(cmc + 0x100, position, 12)) continue;
        const float d[3] = {point[0] - position[0], point[1] - position[1], point[2] - position[2]};
        const float distance = Dot3(d, d);
        if (distance < bestDistance) { bestDistance = distance; best = entity; }
    }
    return best;
}
void InstallNearestPlayerStandIn(FILE* log) {
    static const unsigned char expected[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x18, 0xF3, 0x0F, 0x10, 0x05};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kNearestPlayerRva, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) {
        std::fprintf(log, "necro nearest_player stand-in: signature mismatch at %08X, not installed\n", static_cast<unsigned>(base + kNearestPlayerRva)); return;
    }
    nearestPlayerHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kNearestPlayerRva), reinterpret_cast<void*>(OnNearestPlayer));
    std::fprintf(log, "necro nearest_player stand-in %s at %08X\n", nearestPlayerHook ? "installed" : "FAILED", static_cast<unsigned>(base + kNearestPlayerRva));
}

// Deletion tracer. Base::Delete (PC 0x007568B0, cdecl(object)): vtable slot 2
// on the object, then the object is linked into the deferred-delete list
// (guarded against a second link). Every call on one of the objects this
// module put into the world (Isaac it removed, necros, the settings, the
// respawn manager, the MP UI block, ghost proxies) is recorded with the
// caller's return address, so the restart crashes (a freed LuaAudioScript
// target, a listener entry whose owner is gone) can be tied to a deletion.
safetyhook::InlineHook deleteTraceHook;
struct DeleteEvent { uint32_t object, caller; DWORD at; const char* what; };
DeleteEvent deleteEvents[16]{};
volatile LONG deleteEventCount = 0;
extern uint32_t respawnManager;   // defined with the respawn manager block below
const char* TrackedName(uint32_t object) {
    if (!object) return nullptr;
    if (object == necroEntity) return "necro";
    if (object == deadNecro) return "corpse";
    if (object == settings) return "settings";
    if (object == respawnManager) return "respawn-manager";
    if (object == mpUiBlock) return "mp-ui-block";
    if (object == ghostProxy) return "ghost-proxy";
    if (object == trackedIsaac) return "isaac";
    if (object == necroProxy) return "necro-proxy";
    return nullptr;
}
// Stale entries in the deferred delete. Base::Delete queues an object and the
// flush 0x007567B0 (cdecl(object), from the per-frame 0x00756880 walk) sends it
// iMsgNotifyDeleteEntity through vtable slot 1 and then destroys it through
// slot 0. 23.09., dumps 30880 and 30988 (20:27, 20:38, both on a level exit):
// the queued object was PlayerDecompressionInfo 58CA3E2F-8C8CD155 whose vtable
// was already the abstract base (slot 1 = _purecall 0x0142E5CC), so it had
// been destroyed elsewhere while still queued. That GUID exists twice while
// the necro pool is loaded (global_assets and global_mp_assets). Dumps 26564/27460:
// Base::Delete itself reached a freed object. Both paths now skip an object
// whose vtable is outside the image or whose slots are _purecall or outside it.
constexpr uint32_t kPureCallRva = 0x102E5CC, kDeleteFlushRva = 0x3567B0;
bool deleteGuard = true;
safetyhook::InlineHook deleteFlushHook;
volatile LONG staleFlushSkipped = 0, staleDeleteSkipped = 0;
struct StaleEntity { uint32_t object, vtable, guid0, guid1; const char* where; };
StaleEntity staleEntities[8]{};
volatile LONG staleEntityCount = 0;
// Code of another loaded module counts as live. 24.10.: a VR d3d9.dll patches the destructor of the
// per-player UI object (vtable 01B26D70); judged stale, its flush was skipped and the walk took the same
// node forever (hang dumps 37096, 32220, 37384, 37980).
volatile LONG foreignSlotsAccepted = 0;
bool ForeignCode(uint32_t fn) {
    static uint32_t knownStart[8]{}, knownEnd[8]{};
    static volatile LONG knownCount = 0;
    const LONG count = InterlockedCompareExchange(&knownCount, 0, 0);
    for (LONG i = 0; i < count && i < 8; ++i)
        if (fn >= knownStart[i] && fn < knownEnd[i]) return true;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(fn)), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || info.Type != MEM_IMAGE ||
        !(info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return false;
    const LONG slot = InterlockedIncrement(&knownCount) - 1;
    if (slot < 8) {
        knownStart[slot] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info.BaseAddress));
        knownEnd[slot] = knownStart[slot] + static_cast<uint32_t>(info.RegionSize);
    }
    return true;
}
bool LiveEntity(uint32_t object, std::initializer_list<uint32_t> slots) {
    uint32_t vtable = 0;
    if (object < 0x10000 || !Read(object, &vtable, 4) || vtable < base + 0x1000 || vtable >= imageEnd) return false;
    for (uint32_t slot : slots) {
        uint32_t fn = 0;
        if (!Read(vtable + slot * 4, &fn, 4) || !fn || fn == base + kPureCallRva) return false;
        if (fn >= base + 0x1000 && fn < base + hits::kCodeEnd) continue;
        if (fn >= base && fn < imageEnd) return false;   // the image but not its code
        if (!ForeignCode(fn)) return false;
        InterlockedIncrement(&foreignSlotsAccepted);
    }
    return true;
}
void NoteStaleEntity(uint32_t object, const char* where) {
    const LONG n = InterlockedIncrement(&staleEntityCount) - 1;
    if (n >= 8) return;
    StaleEntity& s = staleEntities[n];
    s.object = object; s.where = where;
    Read(object, &s.vtable, 4); Read(object + 0x24, &s.guid0, 4); Read(object + 0x28, &s.guid1, 4);
}
// The flush runs inside RWS::CEventHandler::DeleteEventHandlers (0x00756820), which
// always takes the first handler linked to the delete message and relies on the
// object's destructor (UnLinkMsg) to take it off. A stale object was destroyed
// without that: 11.10. 23:23, live on both copies, its node was the only one on
// the delete message (count 1), so skipping it alone kept the walk on that node
// forever (flush_skipped 2e9): the freeze on the way back to the menu. Its node
// comes off here as UnLinkMsg would do it: CRegisteredMsgs::RemoveLinkedMsg
// (0x00755E50, thiscall(node), ret 4), the message's link count, and the node
// back to its pool (0x00754D10, cdecl). The object's own link table is freed by
// then and stays untouched.
constexpr uint32_t kDeleteMsgRva = 0x1C4CDA4, kMsgTableRva = 0x1C4CD8C, kRemoveLinkedMsgRva = 0x355E50, kFreeLinkNodeRva = 0x354D10;
volatile LONG staleUnlinked = 0, staleNotLinked = 0, staleLocked = 0, staleUnlinkFaults = 0;
// The delete message's registration and the node that links this object to it.
bool FindDeleteNode(uint32_t object, uint32_t& reg, uint32_t& node) {
    uint32_t msg = 0, table = 0;
    reg = node = 0;
    if (!Read(base + kDeleteMsgRva, &msg, 4) || !msg || !Read(base + kMsgTableRva, &table, 4) || !table || !Read(table + 4 * (msg & 0xFFF), &reg, 4)) return false;
    for (unsigned guard = 0; reg && guard < 256; ++guard) {
        uint32_t id = 0;
        if (!Read(reg + 0x10, &id, 4)) return false;
        if (id == msg) break;
        if (!Read(reg, &reg, 4)) return false;
    }
    if (!reg) return false;
    // priority buckets (+0 next, +8 first node), nodes (+4 next, +0xC handler, +0x10 message)
    uint32_t bucket = 0;
    if (!Read(reg + 4, &bucket, 4)) return false;
    for (unsigned steps = 0; bucket && steps < 4096; ++steps) {
        uint32_t n = 0;
        if (!Read(bucket + 8, &n, 4)) return false;
        for (; n && steps < 4096; ++steps) {
            uint32_t handler = 0, id = 0;
            if (!Read(n + 0xC, &handler, 4) || !Read(n + 0x10, &id, 4)) return false;
            if (handler == object && id == msg) { node = n; return true; }
            if (!Read(n + 4, &n, 4)) return false;
        }
        if (!Read(bucket, &bucket, 4)) return false;
    }
    return false;
}
__declspec(noinline) void UnlinkStale(uint32_t object) {
    uint32_t reg = 0, node = 0;
    if (!FindDeleteNode(object, reg, node)) { InterlockedIncrement(&staleNotLinked); return; }
    uint16_t refs = 0, lock = 0;
    // a node pending its unlink, or a message being walked by a send: left to the engine
    if (!Read(node + 0x14, &refs, 2) || (refs & 0x8000) || !Read(reg + 0x16, &lock, 2) || (lock & 1)) { InterlockedIncrement(&staleLocked); return; }
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kRemoveLinkedMsgRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(reg)), node);
        uint16_t* count = reinterpret_cast<uint16_t*>(static_cast<uintptr_t>(base + kDeleteMsgRva + 4));
        if (*count) --*count;
        reinterpret_cast<void(__cdecl*)(uint32_t)>(base + kFreeLinkNodeRva)(node);
        InterlockedIncrement(&staleUnlinked);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&staleUnlinkFaults);
    }
}
// Loop breaker: the walk only moves on when the object's node leaves the delete message. One that
// comes back unlinked 256 times in a row is handed to the game's own flush, guarded, instead of
// freezing the game.
volatile LONG staleLoopsBroken = 0, staleLoopFaults = 0;
uint32_t staleRepeatObject = 0;
unsigned staleRepeats = 0;
void FlushGuarded(uint32_t object) {
    __try {
        deleteFlushHook.call<void>(object);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&staleLoopFaults);
    }
}
void __cdecl OnDeleteFlush(uint32_t object) {
    if (!LiveEntity(object, {0, 1})) {
        InterlockedIncrement(&staleFlushSkipped);
        NoteStaleEntity(object, "flush");
        UnlinkStale(object);
        staleRepeats = object == staleRepeatObject ? staleRepeats + 1 : 1;
        staleRepeatObject = object;
        if (staleRepeats >= 256) {
            InterlockedIncrement(&staleLoopsBroken);
            staleRepeats = 0;
            FlushGuarded(object);
        }
        return;
    }
    staleRepeatObject = 0;
    deleteFlushHook.call<void>(object);
}
void __cdecl OnBaseDelete(uint32_t object) {
    if (deleteGuard && imageEnd && !LiveEntity(object, {2})) {
        InterlockedIncrement(&staleDeleteSkipped);
        NoteStaleEntity(object, "delete");
        return;
    }
    story_objects::BeforeDelete(object);
    if (enabled) removals::OnBaseDelete(object);   // shared deletes have only ever run with the necro module
    const char* what = TrackedName(object);
    if (what) {
        const LONG n = InterlockedIncrement(&deleteEventCount) - 1;
        DeleteEvent& e = deleteEvents[n & 15];
        e.object = object; e.caller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())); e.at = GetTickCount(); e.what = what;
    }
    deleteTraceHook.call<void>(object);
}
void InstallDeleteTrace(FILE* log) {
    static const unsigned char expected[] = {0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x06, 0x8B, 0x50, 0x08, 0x8B, 0xCE, 0xFF, 0xD2};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kDelete, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) {
        std::fprintf(log, "necro delete_trace: signature mismatch at %08X, not installed\n", static_cast<unsigned>(base + kDelete)); return;
    }
    deleteTraceHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kDelete), reinterpret_cast<void*>(OnBaseDelete));
    std::fprintf(log, "necro delete_trace %s at %08X\n", deleteTraceHook ? "installed" : "FAILED", static_cast<unsigned>(base + kDelete));
    if (!deleteGuard) return;
    if (!imageEnd) {
        uint32_t peOffset = 0, sizeOfImage = 0;
        if (!Read(base + 0x3C, &peOffset, 4) || !Read(base + peOffset + 0x50, &sizeOfImage, 4) || !sizeOfImage) sizeOfImage = 0x3384000;
        imageEnd = base + sizeOfImage;
    }
    // sub esp,0x18 / push esi / mov esi,[esp+0x20] / test byte [esi+4],2 / je
    static const unsigned char kFlush[] = {0x83, 0xEC, 0x18, 0x56, 0x8B, 0x74, 0x24, 0x20, 0xF6, 0x46, 0x04, 0x02, 0x74};
    unsigned char flush[sizeof(kFlush)]{};
    if (!Read(base + kDeleteFlushRva, flush, sizeof(flush)) || std::memcmp(flush, kFlush, sizeof(kFlush)) != 0) {
        std::fprintf(log, "necro delete_guard: flush signature mismatch at %08X, flush not guarded\n", static_cast<unsigned>(base + kDeleteFlushRva)); return;
    }
    deleteFlushHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kDeleteFlushRva), reinterpret_cast<void*>(OnDeleteFlush));
    std::fprintf(log, "necro delete_guard %s (flush %08X, Base::Delete %08X, purecall %08X)\n", deleteFlushHook ? "installed" : "FAILED",
                 static_cast<unsigned>(base + kDeleteFlushRva), static_cast<unsigned>(base + kDelete), static_cast<unsigned>(base + kPureCallRva));
}

// Stream request callbacks. A pending resource request keeps a functor to
// call when its stream unloads (the walk at 0x00790560: item +0x18 -> table,
// table[0] = functor, vtable slot 2, thiscall(args*)). 25.09. 16:18, dump
// 31660: iMsgStreamUnloading during a checkpoint change from the debug menu
// reached a request for an effect texture (fire_ele_cpx) whose functor was
// already freed (vtable 000000A8), the same site as dump 27428 at 13:21.
// A functor whose vtable or slot is not the image's is swapped for a no-op.
struct NoopCallbackVtable { void* slots[4]; };
void __fastcall NoopCallback(void*, void*, void*) {}
NoopCallbackVtable noopCallbackVtable{{reinterpret_cast<void*>(NoopCallback), reinterpret_cast<void*>(NoopCallback),
                                       reinterpret_cast<void*>(NoopCallback), reinterpret_cast<void*>(NoopCallback)}};
struct NoopCallbackObject { NoopCallbackVtable* vtable; } noopCallback{&noopCallbackVtable};
safetyhook::MidHook callbackGuardHook;
volatile LONG callbacksGuarded = 0;
// The deleting callback 0x01391DE0 (vtable 0x01B93F20 slot 2): for its four
// message ids it deletes the payload, args[2] at [esp+0x18] (from [request+0x14]).
// Dump 49008 (09.10. 22:52, arena to the main menu): the payload was the raw
// matlib_catalog.xml (MNL, header 0x1D720001) behind a stale registration (see
// the registration keeper below), and the delete jumped into garbage. A payload
// whose vtable is not in the image's read-only data is not deleted.
constexpr uint32_t kDeletingCallbackRva = 0xF91DE0;
volatile LONG payloadsGuarded = 0;

// Loader registrations. Register 0x008489A0 allocates {loader, type name, type
// hash, channel, the loader's next, the registry's next}; Unregister 0x008487D0
// and the loader base dtor 0x00848750 unlink it and free it. Stream requests
// are cached by asset key (0x00790B40 hands out a live one again) and keep
// their registration (+0x18) past that free. Dumps 49008 and 48232 (09.10.,
// arena to the main menu): the freed block already held the HKT and the NAV
// registration, so the unload notice of matlib_catalog.xml went to the Havok
// and the NavPower callback. The block now stays allocated with the no-op as
// its loader (0x18 bytes per unregister; about a dozen loaders per level).
safetyhook::MidHook keepHooks[3], cacheReuseHook;
volatile LONG registrationsKept = 0, keptLines = 0, keptNotices = 0, staleReuses = 0;
void TypeName(uint32_t reg, char (&name)[16]) {
    std::memset(name, 0, sizeof(name));
    uint32_t text = 0;
    if (!reg || !Read(reg + 4, &text, 4) || !text || !Read(text, name, sizeof(name) - 1)) { name[0] = '?'; name[1] = 0; return; }
    name[sizeof(name) - 1] = 0;
    for (char* c = name; *c; ++c) if (*c < 32 || *c > 126) { *c = 0; break; }
}
bool KeepRegistration(uint32_t reg) {
    uint32_t loader = 0;
    if (reg < 0x10000 || !Read(reg, &loader, 4)) return false;
    const uint32_t noop = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&noopCallback));
    if (!WriteBytes(reg, &noop, 4)) return false;
    InterlockedIncrement(&registrationsKept);
    if (InterlockedIncrement(&keptLines) <= 40) {
        char name[16];
        TypeName(reg, name);
        Note("loader registration %08X (%s, loader %08X) kept with the no-op loader after its unregister", reg, name, loader);
    }
    return true;
}
void OnFreeRegistrationEdi(SafetyHookContext& ctx) { if (KeepRegistration(static_cast<uint32_t>(ctx.edi))) ctx.eip = base + 0x44881B; }
void OnFreeRegistrationEsi(SafetyHookContext& ctx) { if (KeepRegistration(static_cast<uint32_t>(ctx.esi))) ctx.eip = base + 0x44886B; }
void OnFreeRegistrationEax(SafetyHookContext& ctx) { if (KeepRegistration(static_cast<uint32_t>(ctx.eax))) ctx.eip = base + 0x4487BF; }
// Diagnostics only: the request cache handing out a request whose loader is gone.
void OnCachedRequest(SafetyHookContext& ctx) {
    uint32_t reg = 0, loader = 0;
    if (!Read(static_cast<uint32_t>(ctx.eax) + 0x18, &reg, 4) || !Read(reg, &loader, 4) || loader != reinterpret_cast<uintptr_t>(&noopCallback)) return;
    if (InterlockedIncrement(&staleReuses) <= 20) {
        char name[16];
        TypeName(reg, name);
        Note("stream request cache: request %08X for a %s loader that unregistered was handed out again; its resource reaches no loader", static_cast<uint32_t>(ctx.eax),
             name);
    }
}
void InstallRegistrationKeeper(FILE* log) {
    struct Site { const char* what; uint32_t rva; unsigned char bytes[9]; void (*fn)(SafetyHookContext&); };
    // push reg / call 0x00401FAA (free) / add esp,4
    static const Site sites[] = {
        {"Unregister, first", 0x448812, {0x57, 0xE8, 0x92, 0x97, 0xBB, 0xFF, 0x83, 0xC4, 0x04}, OnFreeRegistrationEdi},
        {"Unregister, chained", 0x448862, {0x56, 0xE8, 0x42, 0x97, 0xBB, 0xFF, 0x83, 0xC4, 0x04}, OnFreeRegistrationEsi},
        {"loader dtor", 0x4487B6, {0x50, 0xE8, 0xEE, 0x97, 0xBB, 0xFF, 0x83, 0xC4, 0x04}, OnFreeRegistrationEax},
    };
    for (unsigned i = 0; i < 3; ++i) {
        unsigned char seen[sizeof(sites[i].bytes)]{};
        if (!Read(base + sites[i].rva, seen, sizeof(seen)) || std::memcmp(seen, sites[i].bytes, sizeof(seen)) != 0) {
            std::fprintf(log, "necro registration_keeper %s: signature mismatch at %08X, registrations there are freed as before\n", sites[i].what,
                         static_cast<unsigned>(base + sites[i].rva));
            continue;
        }
        keepHooks[i] = safetyhook::create_mid(reinterpret_cast<void*>(base + sites[i].rva), sites[i].fn);
        std::fprintf(log, "necro registration_keeper %s %s at %08X\n", sites[i].what, keepHooks[i] ? "installed" : "FAILED", static_cast<unsigned>(base + sites[i].rva));
    }
    // inc dword ptr [eax] / pop edi / pop esi / pop ebx (cache hit in 0x00790B40)
    static const unsigned char cacheHit[] = {0xFF, 0x00, 0x5F, 0x5E, 0x5B, 0x83, 0xC4, 0x0C, 0xC2, 0x04, 0x00};
    unsigned char seen[sizeof(cacheHit)]{};
    if (Read(base + 0x390B84, seen, sizeof(seen)) && std::memcmp(seen, cacheHit, sizeof(seen)) == 0)
        cacheReuseHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x390B84), OnCachedRequest);
    std::fprintf(log, "necro request_cache_probe %s at %08X\n", cacheReuseHook ? "installed" : "NOT installed (signature)", static_cast<unsigned>(base + 0x390B84));
}

void OnStreamCallback(SafetyHookContext& ctx) {
    const uint32_t functor = static_cast<uint32_t>(ctx.eax);
    if (functor == reinterpret_cast<uintptr_t>(&noopCallback)) {
        // A request whose loader unregistered (registration keeper): nothing to tell.
        if (InterlockedIncrement(&keptNotices) <= 20) {
            uint32_t reg = 0;
            char name[16];
            Read(static_cast<uint32_t>(ctx.esi) + 0x18, &reg, 4);
            TypeName(reg, name);
            Note("stream callback: request %08X for a %s loader that unregistered; the notice goes to the no-op", static_cast<uint32_t>(ctx.esi), name);
        }
        return;
    }
    uint32_t vtable = 0, slot = 0;
    if (functor >= 0x10000 && Read(functor, &vtable, 4) && vtable >= base + 0x1000 && vtable < imageEnd && Read(vtable + 8, &slot, 4) &&
        slot >= base + 0x1000 && slot < base + hits::kCodeEnd && slot != base + kPureCallRva) {
        if (slot != base + kDeletingCallbackRva) return;
        uint32_t payload = 0, payloadVtable = 0, deleter = 0;
        if (!Read(static_cast<uint32_t>(ctx.esp) + 0x18, &payload, 4) || !payload) return;
        if (Read(payload, &payloadVtable, 4) && payloadVtable >= base + gate::kRdataLo && payloadVtable < base + gate::kRdataHi && Read(payloadVtable, &deleter, 4) &&
            deleter >= base + 0x1000 && deleter < base + hits::kCodeEnd) return;
        if (InterlockedIncrement(&payloadsGuarded) <= 20)
            Note("stream callback guard: payload %08X of the deleting callback is already gone (vtable %08X, slot 0 %08X); not deleted again", payload,
                 payloadVtable, deleter);
        ctx.eax = reinterpret_cast<uintptr_t>(&noopCallback);
        return;
    }
    if (InterlockedIncrement(&callbacksGuarded) <= 20)
        Note("stream callback guard: functor %08X (vtable %08X, slot %08X) is gone; its unload notice skipped", functor, vtable, slot);
    ctx.eax = reinterpret_cast<uintptr_t>(&noopCallback);
}
void InstallStreamCallbackGuard(FILE* log) {
    constexpr uint32_t kSiteRva = 0x390616;
    // mov edx,[eax] / mov edx,[edx+8] / lea ecx,[esp+10h] / push ecx / mov ecx,eax / call edx
    static const unsigned char expected[] = {0x8B, 0x10, 0x8B, 0x52, 0x08, 0x8D, 0x4C, 0x24, 0x10, 0x51, 0x8B, 0xC8, 0xFF, 0xD2};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kSiteRva, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) {
        std::fprintf(log, "necro stream_callback_guard: signature mismatch at %08X, not installed\n", static_cast<unsigned>(base + kSiteRva));
        return;
    }
    callbackGuardHook = safetyhook::create_mid(reinterpret_cast<void*>(base + kSiteRva), OnStreamCallback);
    std::fprintf(log, "necro stream_callback_guard %s at %08X\n", callbackGuardHook ? "installed" : "FAILED", static_cast<unsigned>(base + kSiteRva));
    InstallRegistrationKeeper(log);
}

void InstallListenerGuard(FILE* log) {
    if (!listenerGuard) return;
    constexpr uint32_t kSiteRva = 0x356CEB;
    static const unsigned char expected[] = {0x8B, 0xC8, 0x8B, 0x01, 0x52, 0x8B, 0x50, 0x04, 0xFF, 0xD2};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kSiteRva, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) {
        std::fprintf(log, "necro listener_guard: signature mismatch at %08X, not installed\n", static_cast<unsigned>(base + kSiteRva)); return;
    }
    uint32_t peOffset = 0, sizeOfImage = 0;
    if (!Read(base + 0x3C, &peOffset, 4) || !Read(base + peOffset + 0x50, &sizeOfImage, 4) || !sizeOfImage) sizeOfImage = 0x3384000;
    imageEnd = base + sizeOfImage;
    listenerGuardHook = safetyhook::create_mid(reinterpret_cast<void*>(base + kSiteRva), OnListenerCall);
    std::fprintf(log, "necro listener_guard %s at %08X (image %08X-%08X)\n", listenerGuardHook ? "installed" : "FAILED", static_cast<unsigned>(base + kSiteRva), static_cast<unsigned>(base), imageEnd);
    InstallStreamCallbackGuard(log);
}

// Respawn manager. NecroPlayerRespawnManager is the MP match's type
// bookkeeping: LevelManager::CheckpointLoadComplete builds it on the host for
// multiplayer only (operator new + ctor; PC ctor 0x010A7E00 sets the singleton
// [0x02021628], links the NetReflectable at +4 into the global reflectable
// list and, when authoritative, listens for the player-leave message);
// clients get theirs by NetCreate (0x010C26E0). The MP12 type menu talks to
// it: UINecroSelect_TrySelect -> RequestNecroPlayer (PC 0x0108EAE0: local
// OnlinePlayer -> approved type at +0x30 -> callback(0, ctx)), GetSelection
// compares the menu's pick with +0x30, the SM's respawn asks
// GetApprovedNecroPlayerType. So the host builds it the engine's way once the
// session is up and registers it like an NPC so partners get their ghost;
// RequestNecroPlayer is answered without the OnlinePlayer the campaign may lack.
constexpr uint32_t kNrmSize = 0xE0, kNrmCtorRva = 0xCA7E00, kRequestNecroPlayerRva = 0xC8EAE0;
uint32_t respawnManager = 0, respawnManagerSubKey = 0;
volatile LONG nrmBuilt = 0, nrmRegistered = 0, nrmRequests = 0;
safetyhook::InlineHook requestNecroPlayerHook;
void __fastcall OnRequestNecroPlayer(void* self, void*, uint32_t which, void* callback, void* context) {
    InterlockedIncrement(&nrmRequests);
    const uint32_t manager = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    uint32_t picked = which < 4 ? which : 0;
    // Only a type this machine can spawn is approved (its stream may not be loaded here).
    if (!TypeSpawnable(picked)) {
        const uint32_t fallback = SpawnableFallback();
        if (fallback != 0xFFFFFFFFu) {
            InterlockedIncrement(&menuTypeFallbacks);
            gate::Event("necro: type menu picked %s, which is not loaded here: %s approved instead, %s asked for next time", kTypeNames[picked], kTypeNames[fallback],
                        kTypeNames[picked]);
            if (menuWanted < 0) menuWanted = static_cast<int>(picked);
            picked = fallback;
        }
    }
    WriteBytes(manager + 0x30, &picked, 4);
    Note("type menu: %s picked (manager %08X)", kTypeNames[picked], manager);
    if (callback) { using Callback = void (__cdecl*)(uint32_t, void*); reinterpret_cast<Callback>(callback)(0, context); }
}
// A failed registration is retried from Poll; each try allocates a sub-key and
// runs SetSubKey/NetInit/AddToSession again, so tries are spaced and capped.
constexpr unsigned kNrmRegisterTries = 5;
constexpr DWORD kNrmRegisterSpacingMs = 1000;
unsigned nrmRegisterTries = 0;
DWORD nrmRegisterLastTry = 0;
volatile LONG nrmRegisterFaults = 0;
void RegisterRespawnManager(uint32_t manager) {
    if (!session) return;
    const uint32_t reflectable = manager + 4;
    uint32_t vtable = 0, setSubKey = 0, netInit = 0, proxy = 0;
    if (!Read(reflectable, &vtable, 4) || !Read(vtable + 8, &setSubKey, 4) || !Read(vtable + 0x30, &netInit, 4) || !setSubKey || !netInit) return;
    if (Read(reflectable + 0xC, &proxy, 4) && proxy) { InterlockedExchange(&nrmRegistered, 1); return; }
    if (nrmRegisterTries >= kNrmRegisterTries) return;
    const DWORD now = GetTickCount();
    if (nrmRegisterTries && now - nrmRegisterLastTry < kNrmRegisterSpacingMs) return;
    nrmRegisterLastTry = now;
    if (++nrmRegisterTries == kNrmRegisterTries)
        Note("respawn manager %08X: registration try %u of %u, no more after this one", manager, nrmRegisterTries, kNrmRegisterTries);
    if (!AllocatePlayerSubKey(session, machineIndex, respawnManagerSubKey)) return;
    using SetSubKey = void (__thiscall*)(void*, uint32_t);
    using NetInit = void (__thiscall*)(void*);
    __try {
        reinterpret_cast<SetSubKey>(setSubKey)(reinterpret_cast<void*>(reflectable), respawnManagerSubKey);
        reinterpret_cast<NetInit>(netInit)(reinterpret_cast<void*>(reflectable));
        reinterpret_cast<SetSubKey>(base + kAddToSession)(reinterpret_cast<void*>(reflectable), 0);
    } __except (FaultFilter(GetExceptionInformation())) {
        InterlockedIncrement(&nrmRegisterFaults);
        nrmRegisterTries = kNrmRegisterTries;
        Note("respawn manager %08X registration faulted at %08X; not tried again", manager, faultAddress);
        return;
    }
    if (Read(reflectable + 0xC, &proxy, 4) && proxy) {
        InterlockedExchange(&nrmRegistered, 1);
        Note("respawn manager %08X registered with the session (proxy %08X subkey %08X, try %u)", manager, proxy, respawnManagerSubKey, nrmRegisterTries);
    }
}
void EnsureRespawnManager() {
    if (!nativeRespawn || !typeMenu) return;
    uint32_t current = 0;
    if (!Read(base + kRespawnManager, &current, 4)) return;
    if (current) {
        if (current == respawnManager && !InterlockedCompareExchange(&nrmRegistered, 0, 0)) RegisterRespawnManager(current);
        return;
    }
    respawnManager = 0;
    InterlockedExchange(&nrmRegistered, 0);
    if (!IsHost() || !session) return;   // partners get the ghost through the session
    static const unsigned char expected[] = {0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8D, 0x6E, 0x04, 0xC7, 0x06, 0xFC, 0x51, 0xB7, 0x01};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kNrmCtorRva, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) {
        typeMenu = false;
        Note("respawn manager: ctor signature mismatch at %08X, type menu off", static_cast<unsigned>(base + kNrmCtorRva));
        return;
    }
    __try {
        using New = void* (__cdecl*)(uint32_t);
        void* block = reinterpret_cast<New>(base + kOperatorNewRva)(kNrmSize);
        if (!block) { typeMenu = false; Note("respawn manager: operator new failed"); return; }
        std::memset(block, 0, kNrmSize);
        using Ctor = void* (__thiscall*)(void*);
        // A ctor fault leaves the block allocated on purpose: the ctor may already
        // have linked it into the reflectable list. typeMenu goes off, so at most one.
        reinterpret_cast<Ctor>(base + kNrmCtorRva)(block);
        respawnManager = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block));
        nrmRegisterTries = 0;
        InterlockedIncrement(&nrmBuilt);
        Note("respawn manager built at %08X (host)", respawnManager);
        RegisterRespawnManager(respawnManager);
    } __except (FaultFilter(GetExceptionInformation())) {
        typeMenu = false;
        Note("respawn manager ctor faulted: exception %08X at %08X; type menu off", static_cast<unsigned>(GetExceptionCode()), faultAddress);
    }
}
// MP12NecroRespawn::Show (PC 0x00A97240) asks the online player list
// ([OnlineServices+0x40], vtable slot 0x18 = 0x0137DAC0) whether the killer's
// player ref is in the game; that reads JoinedGame ([OnlineServices+0x94]+0x498),
// null in the campaign (host crash 9244 at 0x0137DAD3). Answered "no" then.
safetyhook::InlineHook onlineRefGuardHook;
volatile LONG onlineRefNull = 0;
constexpr uint32_t kOnlineServices = 0x1C213DC, kJoinedGame = 0x94, kOnlineRefRva = 0xF7DAC0;
unsigned char __fastcall OnOnlineRefInGame(void* self, void*, uint32_t low, uint32_t high) {
    uint32_t services = 0, joined = 0;
    if (!Read(base + kOnlineServices, &services, 4) || !services || !Read(services + kJoinedGame, &joined, 4) || !joined) {
        InterlockedIncrement(&onlineRefNull);
        return 0;
    }
    return onlineRefGuardHook.thiscall<unsigned char>(self, low, high);
}
void InstallOnlineRefGuard(FILE* log) {
    if (!typeMenu) return;
    static const unsigned char expected[] = {0xA1, 0xDC, 0x13, 0x02, 0x02, 0x8B, 0x88, 0x94, 0x00, 0x00, 0x00, 0x8B, 0x54, 0x24, 0x08, 0x8B, 0x44, 0x24, 0x04};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kOnlineRefRva, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) {
        std::fprintf(log, "necro type_menu online-ref guard: signature mismatch at %08X, type menu off\n", static_cast<unsigned>(base + kOnlineRefRva));
        typeMenu = false;
        return;
    }
    onlineRefGuardHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kOnlineRefRva), reinterpret_cast<void*>(OnOnlineRefInGame));
    std::fprintf(log, "necro type_menu online-ref guard %s at %08X\n", onlineRefGuardHook ? "installed" : "FAILED", static_cast<unsigned>(base + kOnlineRefRva));
    if (!onlineRefGuardHook) typeMenu = false;
}
// VentMarker::RequestRespawn (PC 0x012F5CA0, thiscall(IVentRespawnHandler*),
// ret 4) is what the vent pick's confirm ends in: store the handler at
// vent+0x3D0, get the local OnlinePlayer, and return silently without one,
// which the campaign has none of (trace: transition 6 fires, the SM's own
// wait then refuses after five seconds, NecroPlayer::Spawn never runs). The
// rest of the function is what the campaign needs: authoritative vent (no net
// proxy) -> in-use bit set: handler->Response(false); else Response(true),
// SetInUse(true) (0x012D5840), SetCoverState(true, 0) (0x012F5AB0). The
// stand-in does exactly that, without the OnlinePlayer, and lets a handler
// re-register (the engine drops a second request while +0x3D0 is still set).
safetyhook::InlineHook requestRespawnHook;
volatile LONG ventRequests = 0, ventGranted = 0, ventRefused = 0;
constexpr uint32_t kRequestRespawnRva = 0xEF5CA0, kVentSetInUseRva = 0xED5840, kVentSetCoverStateRva = 0xEF5AB0, kIsAuthoritativeRva = 0x3E1130;
constexpr uint32_t kVentHandler = 0x3D0, kVentFlags = 0x2F0, kVentInUse = 0x20;
volatile LONG ventFaults = 0;
void GrantVentRequest(uint32_t vent, uint32_t handler);
void __fastcall OnRequestRespawn(void* self, void*, uint32_t handler) {
    InterlockedIncrement(&ventRequests);
    const uint32_t vent = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    if (!handler) return;
    uint32_t proxy = 0;
    if (Read(vent + 0x5C + 0xC, &proxy, 4) && proxy) {   // a networked vent: the engine's own path (authority check, RPC)
        requestRespawnHook.thiscall<void>(self, handler);
        return;
    }
    __try {
        GrantVentRequest(vent, handler);
    } __except (FaultFilter(GetExceptionInformation())) {
        if (InterlockedIncrement(&ventFaults) <= 5) Note("vent %08X request faulted at %08X; left unanswered", vent, faultAddress);
    }
}
void GrantVentRequest(uint32_t vent, uint32_t handler) {
    WriteBytes(vent + kVentHandler, &handler, 4);
    uint32_t flags = 0, handlerVtable = 0, response = 0;
    Read(vent + kVentFlags, &flags, 4);
    if (!Read(handler, &handlerVtable, 4) || !Read(handlerVtable + 4, &response, 4) || !response) return;
    using Response = void (__thiscall*)(uint32_t, unsigned char);
    if (flags & kVentInUse) {
        InterlockedIncrement(&ventRefused);
        Note("vent %08X request: in use (flags %08X), refused", vent, flags);
        reinterpret_cast<Response>(response)(handler, 0);
        return;
    }
    InterlockedIncrement(&ventGranted);
    reinterpret_cast<Response>(response)(handler, 1);
    using SetInUse = void (__thiscall*)(uint32_t, unsigned char);
    using SetCoverState = unsigned char (__thiscall*)(uint32_t, unsigned char, uint32_t);
    reinterpret_cast<SetInUse>(base + kVentSetInUseRva)(vent, 1);
    reinterpret_cast<SetCoverState>(base + kVentSetCoverStateRva)(vent, 1, 0);
    const uint32_t zero = 0;
    WriteBytes(vent + 0x3B8, &zero, 4); WriteBytes(vent + 0x3BC, &zero, 4);
    haveGrantedVent = Read(vent + kVentCentroid, grantedVentCentroid, 12);
    RestoreVentType();
    uint32_t attributes = 0, ventType = 0xFFFFFFFFu;
    if (Read(vent + kVentAttributes, &attributes, 4) && attributes) Read(attributes + kVentTypeAttribute, &ventType, 4);
    if (walkInVentMap && attributes && (ventType == 3 || ventType == 4)) {
        const uint32_t mapped = ventType - 2;   // WALLWALKIN -> WALL, CEILINGWALKIN -> CEILING
        if (WriteBytes(attributes + kVentTypeAttribute, &mapped, 4)) {
            overriddenVentAttrs = attributes; overriddenVentType = ventType; ventOverrideAt = GetTickCount();
            InterlockedIncrement(&ventTypesMapped);
        }
    }
    lastGrantedVentType = ventType;
    // Exit camera by the MP rule, the edge guessed from the ghost camera; the
    // spawn writes it again with the engine's own edge.
    // The effective type: the walk-in mapping above may already have rewritten
    // the attribute, and the exit uses what stands there, not what was authored.
    uint32_t effectiveType = ventType;
    if (attributes) Read(attributes + kVentTypeAttribute, &effectiveType, 4);
    WriteExitCamera(vent, EngineExitEdge(vent, effectiveType), "grant");
    Note("vent %08X request granted (flags %08X, type %u%s) for handler %08X", vent, flags, ventType, overriddenVentAttrs ? ", walk-in mapped for the exit" : "", handler);
}
void InstallRequestRespawnStandIn(FILE* log) {
    static const unsigned char expected[] = {0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xF1, 0x85, 0xC0, 0x0F, 0x84};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kRequestRespawnRva, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) {
        std::fprintf(log, "necro vent_request stand-in: signature mismatch at %08X, not installed\n", static_cast<unsigned>(base + kRequestRespawnRva)); return;
    }
    requestRespawnHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kRequestRespawnRva), reinterpret_cast<void*>(OnRequestRespawn));
    std::fprintf(log, "necro vent_request stand-in %s at %08X\n", requestRespawnHook ? "installed" : "FAILED", static_cast<unsigned>(base + kRequestRespawnRva));
}
void InstallRespawnManagerHook(FILE* log) {
    if (!typeMenu) return;
    static const unsigned char expected[] = {0x51, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14, 0x8B, 0xF1, 0xC7, 0x44, 0x24, 0x08, 0x00, 0x00, 0x00};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kRequestNecroPlayerRva, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) {
        typeMenu = false;
        std::fprintf(log, "necro type_menu RequestNecroPlayer: signature mismatch at %08X, type menu off\n", static_cast<unsigned>(base + kRequestNecroPlayerRva));
        return;
    }
    requestNecroPlayerHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kRequestNecroPlayerRva), reinterpret_cast<void*>(OnRequestNecroPlayer));
    std::fprintf(log, "necro type_menu RequestNecroPlayer stand-in %s at %08X\n", requestNecroPlayerHook ? "installed" : "FAILED", static_cast<unsigned>(base + kRequestNecroPlayerRva));
}

void InstallNativeRespawn(FILE* log) {
    if (!nativeRespawn) return;
    {
        constexpr uint32_t kMoveProxyRva = 0xC72A00;
        static const unsigned char expected[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x78, 0x56, 0x8B, 0x75, 0x08, 0x57, 0x85, 0xF6};
        unsigned char actual[sizeof(expected)]{};
        if (Read(base + kMoveProxyRva, actual, sizeof(actual)) && std::memcmp(actual, expected, sizeof(expected)) == 0) {
            moveProxyHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kMoveProxyRva), reinterpret_cast<void*>(OnMoveProxyToRoomView));
            std::fprintf(log, "necro native_respawn move-proxy guard %s at %08X\n", moveProxyHook ? "installed" : "FAILED", static_cast<unsigned>(base + kMoveProxyRva));
        } else std::fprintf(log, "necro native_respawn move-proxy signature mismatch at %08X\n", static_cast<unsigned>(base + kMoveProxyRva));
    }
    constexpr uint32_t kApprovedTypeRva = 0xCC2760;
    static const unsigned char expectedType[] = {0x51, 0x56, 0x8B, 0xF1, 0xB8, 0x05, 0x00, 0x00, 0x00, 0x39, 0x46, 0x30};
    unsigned char actual[16]{};
    if (Read(base + kApprovedTypeRva, actual, sizeof(expectedType)) && std::memcmp(actual, expectedType, sizeof(expectedType)) == 0) {
        approvedTypeHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kApprovedTypeRva), reinterpret_cast<void*>(OnApprovedType));
        std::fprintf(log, "necro native_respawn approved-type hook %s at %08X\n", approvedTypeHook ? "installed" : "FAILED", static_cast<unsigned>(base + kApprovedTypeRva));
    } else std::fprintf(log, "necro native_respawn approved-type signature mismatch at %08X\n", static_cast<unsigned>(base + kApprovedTypeRva));
    // Show/Hide pairs: call <ui manager>; push 'MP12'/'MP13'; lea ecx,[eax+0x74];
    // call <screen op> (PC 0x0108F550/70 show, 0x01085C90/B0 hide, adjacent like
    // their debug-build twins). The tiny by-tag hide wrappers (0x01072DE0/0x01072DF0:
    // push tag; call; pop ecx; ret) are silenced too (06.10.: they sat 0x10 higher and
    // the shape check refused both).
    struct { uint32_t rva; unsigned char tag; bool tiny; void* handler; const char* what; } screens[6] = {
        {0xC8F550, 0x32, false, reinterpret_cast<void*>(OnShowMp12), "show MP12"},
        {0xC8F570, 0x33, false, reinterpret_cast<void*>(OnShowMp13), "show MP13"},
        {0xC85C90, 0x32, false, reinterpret_cast<void*>(OnHideMp12), "hide MP12"},
        {0xC85CB0, 0x33, false, reinterpret_cast<void*>(OnHideMp13), "hide MP13"},
        {0xC72DE0, 0x32, true,  reinterpret_cast<void*>(OnHideMp12Tag), "hide-by-tag MP12"},
        {0xC72DF0, 0x33, true,  reinterpret_cast<void*>(OnHideMp13Tag), "hide-by-tag MP13"}};
    for (unsigned i = 0; i < 6; ++i) {
        unsigned char head[13]{};
        bool shape = false;
        if (Read(base + screens[i].rva, head, sizeof(head))) {
            if (screens[i].tiny) shape = head[0] == 0x68 && head[1] == screens[i].tag && head[2] == 0x31 && head[3] == 0x50 && head[4] == 0x4D && head[5] == 0xE8 && head[10] == 0x59 && head[11] == 0xC3;
            else shape = head[0] == 0xE8 && head[5] == 0x68 && head[6] == screens[i].tag && head[7] == 0x31 && head[8] == 0x50 && head[9] == 0x4D && head[10] == 0x8D && head[11] == 0x48 && head[12] == 0x74;
        }
        if (!shape) { std::fprintf(log, "necro native_respawn %s: signature mismatch at %08X, left alone\n", screens[i].what, static_cast<unsigned>(base + screens[i].rva)); continue; }
        uiHooks[i] = safetyhook::create_inline(reinterpret_cast<void*>(base + screens[i].rva), screens[i].handler);
        std::fprintf(log, "necro native_respawn %s stood in at %08X: %s\n", screens[i].what, static_cast<unsigned>(base + screens[i].rva), uiHooks[i] ? "ok" : "FAILED");
    }
}

void ApplyVentRadius() {
    uint32_t current = 0, attributes = 0;
    if (!Read(base + kMPLevelSettings, &current, 4) || !current) { radiusAppliedTo = 0; return; }
    if (current == radiusAppliedTo) return;
    if (Read(current + kSettingsAttributes, &attributes, 4) && attributes && WriteBytes(attributes + kSettingsVentRadius, &ventSearchRadius, 4)) {
        radiusAppliedTo = current;
        Note("vent search radius set to %.0f m (MPLevelSettings %08X)", ventSearchRadius, current);
    }
}

// F11 while the ghost camera is up (no vent in reach, or no patience): the
// corpse still holds slot 0 (RespawnProxy::Initialize never registers), so it
// is unregistered as at a revert, the ghost proxy goes through its own teardown
// (its bone-bound camera pops with it), and Isaac respawns where the necro fell.
void EscapeGhost(const float* at) {
    uint32_t vtable = 0, playerManager = 0;
    if (!Read(base + kPlayerManager, &playerManager, 4) || !playerManager) return;
    using Unregister = bool (__thiscall*)(void*, uint32_t, int);
    const uint32_t corpse = NecroInSlotZero();
    if (corpse) reinterpret_cast<Unregister>(base + kPmUnregister)(reinterpret_cast<void*>(playerManager), corpse + kBasePlayer, 1);
    uint32_t proxy = ghostProxy;
    if (proxy && (!Read(proxy, &vtable, 4) || vtable != base + kRespawnProxyVtableRva)) proxy = 0;
    ghostProxy = 0;
    HideNecroHud();
    if (proxy) {
        uint32_t shutdown = 0;
        if (Read(vtable + 0x1C, &shutdown, 4) && shutdown) {
            using Shutdown = void (__thiscall*)(uint32_t);
            reinterpret_cast<Shutdown>(shutdown)(proxy);
        }
        using Delete = void (__cdecl*)(uint32_t);
        reinterpret_cast<Delete>(base + kDelete)(proxy);
    }
    alignas(16) float where[16];
    std::memcpy(where, lastNecroMatrix, sizeof(where));
    if (!haveLastNecroMatrix) { std::memset(where, 0, sizeof(where)); where[0] = where[5] = where[10] = where[15] = 1.0f; }
    if (at) std::memcpy(where, at, sizeof(where));
    const uint32_t human = RespawnPlayer(playerManager, where);
    if (human) { playerGone = false; LifeExpect(kLifeHuman, "out of the ghost"); }
    InterlockedIncrement(&reverts);
    Note("ghost escape: corpse %08X %s, proxy %08X %s, Isaac %s%08X", corpse, corpse ? "unregistered" : "(none in slot 0)", proxy, proxy ? "deleted" : "(unknown)",
         human ? "respawned as " : "NOT respawned, error ", human ? human : static_cast<uint32_t>(InterlockedCompareExchange(&lastError, 0, 0)));
}

// Versus (runtime_coop_mpnative.inc): a necromorph player starts the round as the
// ghost, with no Isaac, the way an MP round starts one.
// NecroRespawnProxySM::CreateRespawnProxy (PC 0x010D2740, cdecl(const guid128*
// proxy template, NecroPlayer* = null), caller cleans) spawns the RespawnProxy
// from MPLevelSettings' CMD_SetRespawnProxy (attrs+0xC8), binds viewport 0 to it,
// starts its state machine (type menu, then vent) and moves it to a vent's room
// view through our MoveProxyToRoomViewCamera hook. Isaac then goes the way
// Transform removes him. 0 = ghost up, else the reason (logged by the caller).
constexpr uint32_t kCreateRespawnProxyRva = 0xCD2740, kSettingsRespawnProxy = 0xC8;
constexpr uint32_t kRespawnProxyGuid[4] = {0x58C9EA2Au, 0xCBDF5A07u, 0x3532554Cu, 0x58583337u};
volatile LONG ghostStarts = 0;
__declspec(noinline) bool CreateProxyRaw(const uint32_t* guid) {
    __try {
        reinterpret_cast<void(__cdecl*)(const uint32_t*, uint32_t)>(base + kCreateRespawnProxyRva)(guid, 0);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        return false;
    }
}
// The local ghost: the RespawnProxy the mover saw. It never registers with the
// PlayerManager (RespawnProxy::Initialize), so a Versus necro that starts as a
// ghost has an empty slot 0 until its first spawn (12.10. 00:26: "slot 0 proxy
// 00000000" and tick mode 1 throughout the ghost phase).
uint32_t LocalGhost() {
    uint32_t vtable = 0;
    return ghostProxy && Read(ghostProxy, &vtable, 4) && vtable == base + kRespawnProxyVtableRva ? ghostProxy : 0;
}
void ForgetGhost() { ghostProxy = 0; }
// withoutIsaac: a Versus necro starts as the ghost without ever having an Isaac
// (mpmap spawns none for it), as an MP round starts one.
volatile LONG ghostStartsWithoutIsaac = 0;
int GhostStart(bool withoutIsaac) {
    if (!enabled) return 1;
    if (playerGone || necroEntity || ghostProxy) return 2;   // already a necro or a ghost
    const uint32_t player = LocalCampaignPlayer();
    if (!player && !withoutIsaac) return 3;
    uint32_t simManager = 0, playerManager = 0;
    if (!Read(base + kSimManager, &simManager, 4) || !simManager || !Read(base + kPlayerManager, &playerManager, 4) || !playerManager) return 4;
    if (!PoolReady()) {
        if (poolMode == 2 && !poolDemanded) Demand(true);   // on demand: this is the demand
        return 5;
    }
    if (!EnsureSettings(simManager)) return 6;
    EnsureMpUi();
    EnsureRespawnManager();
    if (typeMenu && !RespawnManagerPresent()) return 7;   // the host builds it; a partner waits for its ghost
    static const unsigned char expected[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x45, 0x08, 0x83, 0xEC, 0x54, 0x53, 0x56, 0x57};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kCreateRespawnProxyRva, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) return 8;
    uint32_t attributes = 0, guid[4]{};
    if (!(settings && Read(settings + kSettingsAttributes, &attributes, 4) && attributes && Read(attributes + kSettingsRespawnProxy, guid, sizeof(guid)) &&
          (guid[0] | guid[1] | guid[2] | guid[3])))
        std::memcpy(guid, kRespawnProxyGuid, sizeof(guid));
    // Where Isaac stood: the ghost's fallback position and EscapeGhost's way back.
    if (player && Read(player + kEntityMatrix, lastNecroMatrix, sizeof(lastNecroMatrix))) haveLastNecroMatrix = true;
    const uint32_t before = ghostProxy;
    if (!CreateProxyRaw(guid)) { InterlockedExchange(&lastError, 70); return 9; }
    if (!ghostProxy || ghostProxy == before) return 10;   // the mover never saw a proxy
    if (!player) {
        playerGone = true;
        tk::OnBodyReplaced();
        InterlockedIncrement(&ghostStarts);
        InterlockedIncrement(&ghostStartsWithoutIsaac);
        Note("ghost start without an Isaac: RespawnProxy %08X from %08X-%08X-%08X-%08X, respawn manager %s, MP UI %s, local ghost %08X", ghostProxy, guid[0], guid[1],
             guid[2], guid[3], RespawnManagerPresent() ? "up" : "missing", MpUiPresent() ? "up" : "missing", LocalGhost());
        return 0;
    }
    // Isaac goes, as in Transform: out of the manager, his shutdown, Base::Delete.
    clientprog::BeforeIsaacGoes(player);
    Read(player + 0x24, playerGuid, sizeof(playerGuid));
    Read(player + kEntitySubKeyWord, &playerSubKeyWord, 4);
    using Unregister = bool (__thiscall*)(void*, uint32_t, int);
    reinterpret_cast<Unregister>(base + kPmUnregister)(reinterpret_cast<void*>(playerManager), player + kBasePlayer, 1);
    uint32_t playerVtable = 0, shutdown = 0;
    if (Read(player, &playerVtable, 4) && Read(playerVtable + 0x1C, &shutdown, 4) && shutdown) {
        using Shutdown = void (__thiscall*)(uint32_t);
        reinterpret_cast<Shutdown>(shutdown)(player);
    }
    using Delete = void (__cdecl*)(uint32_t);
    reinterpret_cast<Delete>(base + kDelete)(player);
    playerGone = true;
    trackedIsaac = player;
    tk::OnBodyReplaced();
    InterlockedIncrement(&ghostStarts);
    Note("ghost start: RespawnProxy %08X from %08X-%08X-%08X-%08X, Isaac %08X removed, respawn manager %s, MP UI %s", ghostProxy, guid[0], guid[1], guid[2], guid[3],
         player, RespawnManagerPresent() ? "up" : "missing", MpUiPresent() ? "up" : "missing");
    return 0;
}

// Joined as a necromorph (the story screen's join row, lobby::joinAsNecro): once the host's
// level runs here with the host connected and our Isaac in slot 0, the necro key's action
// runs once by itself. Dropped when the menu comes back without a level for two minutes.
volatile LONG joinAsNecroPending = 0, joinAsNecroDone = 0;
DWORD joinAsNecroSince = 0, joinAsNecroSettledAt = 0;
DWORD joinTestAt = 0;
void JoinAsNecro() {
    if (!enabled) return;
    joinTestAt = Stamp(GetTickCount());
    InterlockedExchange(&joinAsNecroPending, 1);
    joinAsNecroSince = GetTickCount();
    joinAsNecroSettledAt = 0;
    Note("join: as a necromorph; the transform waits for the host's level");
}
// What this machine plays, for the partners' doors and story triggers (roles in runtime_coop.inc).
int LocalRole() {
    if (!enabled) return 0;
    if (watch != Watch::None && watch != Watch::Confirming) return 3;
    if (DirectSpawnPending()) return 2;   // the moment Isaac stands at the spot before the transform
    if (necroEntity) return 1;
    return playerGone ? 2 : 0;
}
// From Poll, on the game thread.
void Poll();
// coop::Poll's entry: the stream, session and collision calls below run on the
// game thread, and a fault in one of them must not end the process.
volatile LONG pollFaults = 0;
void PollGuarded() {
    __try {
        Poll();
    } __except (FaultFilter(GetExceptionInformation())) {
        allowMp = false;
        InterlockedExchange(&lastError, 98);
        if (InterlockedIncrement(&pollFaults) <= 5)
            Note("poll faulted: exception %08X at %08X", static_cast<unsigned>(GetExceptionCode()), faultAddress);
    }
}
// The ghost outside the MP maps (user, 17.10.: faster, and through walls). RespawnProxy::Update
// sets its CMC's desired velocity from the stick times the template's walk or run speed
// (RespawnProxy::Attributes +0x60 / +0x64; the retail template has 4 and 7 m/s, the debug
// build's defaults 4 and 6, so 17.10. never sped anything up); the template is the pool's,
// gone with the pool at the level's end, so the MP maps keep theirs. Noclip is the CMC's
// COLLMODE_NONE (CollisionMode 0 NONE, 1 PLAYER, 2 NPC, 3 SMALL_NPC, 4 CUSTOM), only while its
// MovementMode (+0x268) has no gravity (2 or 6), else the ghost would fall out of the level.
constexpr uint32_t kProxyWalkSpeed = 0x60, kProxyRunSpeed = 0x64, kCmcMovementMode = 0x268;
float ghostSpeed = 6.0f;
// The speeds found on a template are its base; a tuned one (above 12/15 m/s) is never taken as one.
uint32_t speedAttrs = 0;
float baseWalk = 0.0f, baseRun = 0.0f;
bool ghostNoclip = true;
uint32_t tunedProxy = 0;
DWORD ghostTunedAt = 0;
volatile LONG ghostSpeedSets = 0, ghostNoclipSets = 0, ghostNoclipRefused = 0, ghostTuneSkips = 0;
void TuneGhost(DWORD now) {
    if (!ghostProxy || mpmap::InVisit() || now - ghostTunedAt < 250) return;
    ghostTunedAt = now;
    const bool fresh = ghostProxy != tunedProxy;
    tunedProxy = ghostProxy;
    uint32_t attrs = 0;
    if (fresh && ghostSpeed > 1.01f && Read(ghostProxy + 0x0C, &attrs, 4) && attrs) {
        float walk = 0.0f, run = 0.0f;
        Read(attrs + kProxyWalkSpeed, &walk, 4);
        Read(attrs + kProxyRunSpeed, &run, 4);
        const bool done = attrs == speedAttrs && walk == baseWalk * ghostSpeed && run == baseRun * ghostSpeed;
        const bool plain = walk >= 0.5f && walk <= 12.0f && run >= 0.5f && run <= 15.0f;
        if (!done && plain) {
            speedAttrs = attrs;
            baseWalk = walk;
            baseRun = run;
            const float wantWalk = walk * ghostSpeed, wantRun = run * ghostSpeed;
            const bool ok = WriteBytes(attrs + kProxyWalkSpeed, &wantWalk, 4) && WriteBytes(attrs + kProxyRunSpeed, &wantRun, 4);
            if (ok) InterlockedIncrement(&ghostSpeedSets);
            Note("ghost %08X: speed %.1f/%.1f -> %.1f/%.1f m/s (x%.1f)%s", ghostProxy, walk, run, wantWalk, wantRun, ghostSpeed, ok ? "" : " WRITE FAILED");
        } else if (!done) {
            InterlockedIncrement(&ghostTuneSkips);
            Note("ghost %08X: template speeds %.2f/%.2f look tuned already, left alone", ghostProxy, walk, run);
        }
    }
    if (!ghostNoclip || !setCollisionModeOk) return;
    const uint32_t cmc = FindCmc(ghostProxy);
    uint32_t mode = 0, movement = 0;
    if (!cmc || !Read(cmc + kCmcModeOffset, &mode, 4) || !Read(cmc + kCmcMovementMode, &movement, 4) || mode == 0) return;
    if (movement != 2 && movement != 6) {
        if (fresh || InterlockedIncrement(&ghostNoclipRefused) <= 3)
            Note("ghost %08X: movement mode %u has gravity, no noclip (it would fall through the floor)", ghostProxy, movement);
        return;
    }
    using SetMode = void (__thiscall*)(uint32_t, uint32_t, int);
    reinterpret_cast<SetMode>(base + kSetCollisionModeRva)(cmc, 0, 1);
    if (InterlockedIncrement(&ghostNoclipSets) <= 6) Note("ghost %08X: noclip (collision mode %u -> 0, movement mode %u)", ghostProxy, mode, movement);
}

// Versus-like direct spawn (user, 17.10.: "like L4D2 versus: spawn right at a spot around the
// corner, not in view, at least 7 m away"). The ghost at the vents flies to a spot and presses
// [Necro] DirectSpawnKey: the floor below the ghost is the spawn when its room is shown here, a
// necro fits there, no human is nearer than DirectSpawnMinDistance, one is within
// DirectSpawnMaxDistance and no human's eyes reach it (casts to its feet, chest and head). Isaac
// comes back there for a moment (EscapeGhost, as for a scene watch) and turns into the necro
// (Transform) on the next ticks: no vent, no exit animation. The type is the one the type menu
// approved (respawn manager +0x30), else [Necro] Type.
bool directSpawn = true;
int directSpawnKey = 'Q';
float directMinDistance = 7.0f, directMaxDistance = 60.0f;
enum class Direct { Idle, Escaped };
Direct directState = Direct::Idle;
DWORD directAt = 0, directTryAt = 0, directHintAt = 0, directRefusedAt = 0;
bool directKeyDown = false;
float directSpot[3]{};
alignas(16) float directMatrix[16]{};
DWORD directCheckAt = 0;
bool directCheckHealed = false;
int directType = 0;
unsigned lastDirectBots = 0;   // bots the last spot check counted
constexpr unsigned kDirectWhys = 9;
const char* const kDirectWhy[kDirectWhys] = {"the respawn timer runs", "a human plays a scene", "no floor below", "the room is not shown here", "no room to stand",
                                             "too close to a player", "a player sees it", "no human near", "no line of sight test"};
const char* const kDirectWhyHud[kDirectWhys] = {"TIMER", "SCENE", "NO FLOOR", "ROOM NOT HERE", "NO ROOM", "TOO CLOSE", "SEEN", "TOO FAR", "NO LOS TEST"};
volatile LONG directAsked = 0, directDone = 0, directFailed = 0, directRefused[kDirectWhys]{};
bool DirectSpawnPending() { return directState == Direct::Escaped; }
// A clear line from a human's eyes to the point (cast from 0.6 m out, so his own body is not hit).
bool EyeReaches(const float* eye, const float* point) {
    float d[3] = {point[0] - eye[0], point[1] - eye[1], point[2] - eye[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 0.7f) return true;
    alignas(16) float from[4] = {eye[0] + d[0] / len * 0.6f, eye[1] + d[1] / len * 0.6f, eye[2] + d[2] / len * 0.6f, 1.0f};
    alignas(16) float to[4] = {point[0], point[1], point[2], 1.0f};
    float fraction = 1.0f;
    uint32_t hit = 0;
    if (!bot_sense::CastRaw(from, to, 0, fraction, hit)) return true;   // no answer counts as seen
    return fraction >= 0.999f;
}
// -1: the spot is good; else the index into kDirectWhy.
int DirectSpotCheck(DWORD now, float* spot, float& nearest, unsigned& nearestSlot) {
    nearest = 3.4e38f;
    nearestSlot = 0xFF;
    lastDirectBots = 0;
    if (PaceLeft(now)) return 0;
    if (SceneHold(now)) return 1;
    if (!bot_sense::losReady) return 8;
    float cam[3]{};
    if (!Read(ghostProxy + kEntityMatrix + 0x30, cam, 12) || !std::isfinite(cam[0]) || !std::isfinite(cam[1]) || !std::isfinite(cam[2])) return 2;
    alignas(16) float from[4] = {cam[0], cam[1], cam[2], 1.0f};
    alignas(16) float to[4] = {cam[0], cam[1] - 20.0f, cam[2], 1.0f};
    float fraction = 1.0f;
    uint32_t floor = 0;
    if (!bot_sense::CastRaw(from, to, 0, fraction, floor) || fraction >= 0.999f) return 2;
    spot[0] = cam[0]; spot[1] = cam[1] - 20.0f * fraction + 0.05f; spot[2] = cam[2];
    if (EntityStreamShown(floor) == 0) return 3;
    {
        alignas(16) float a[4] = {spot[0], spot[1] + 0.2f, spot[2], 1.0f};
        alignas(16) float b[4] = {spot[0], spot[1] + 2.0f, spot[2], 1.0f};
        uint32_t hit = 0;
        fraction = 1.0f;
        if (bot_sense::CastRaw(a, b, 0, fraction, hit) && fraction < 0.999f) return 4;
        static const float kSides[4][2] = {{0.5f, 0.0f}, {-0.5f, 0.0f}, {0.0f, 0.5f}, {0.0f, -0.5f}};
        unsigned blocked = 0;
        for (const auto& side : kSides) {
            alignas(16) float c[4] = {spot[0], spot[1] + 1.0f, spot[2], 1.0f};
            alignas(16) float e[4] = {spot[0] + side[0], spot[1] + 1.0f, spot[2] + side[1], 1.0f};
            fraction = 1.0f;
            if (bot_sense::CastRaw(c, e, 0, fraction, hit) && fraction < 0.999f) ++blocked;
        }
        if (blocked >= 2) return 4;
    }
    bool seen = false;
    float nearestAny = 3.4e38f;   // humans and bots: "too close"
    static const float kHeights[3] = {0.4f, 1.1f, 1.7f};
    const auto look = [&](const float* h) {
        const float dx = h[0] - spot[0], dy = h[1] - spot[1], dz = h[2] - spot[2], d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < nearestAny) nearestAny = d;
        const float eye[3] = {h[0], h[1] + 1.6f, h[2]};
        for (float height : kHeights) {
            const float point[3] = {spot[0], spot[1] + height, spot[2]};
            if (!seen && EyeReaches(eye, point)) { seen = true; break; }
        }
        return d;
    };
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        float h[3]{};
        if (!HumanPos(s, now, h)) continue;
        const float d = look(h);
        if (d < nearest) { nearest = d; nearestSlot = s; }
    }
    // The host's bots see and crowd a spot like a human, but none makes it "near" (user, 17.10.).
    // Here as their ghosts (machine 0xF0 + n), on the bots' own machine as the bots.
    unsigned bots = 0;
    for (unsigned i = 0; i < kMaxBots; ++i) {
        const uint32_t body = coop::botEntities[i] ? coop::botEntities[i] : GhostForMachine(0xF0u + i);
        if (!body || !IsPlayer(body) || (!coop::botEntities[i] && MachineOf(body) != 0xF0u + i) || restsync::Dead(body)) continue;
        float b[3]{};
        if (!Read(body + 0xC0, b, 12) || !std::isfinite(b[0]) || !std::isfinite(b[1]) || !std::isfinite(b[2])) continue;
        look(b);
        ++bots;
    }
    lastDirectBots = bots;
    if (nearestSlot == 0xFF) return 7;
    if (nearestAny < directMinDistance) return 5;
    if (nearest > directMaxDistance) return 7;
    return seen ? 6 : -1;
}
int DirectSpawnType() {
    if (sixth::enabled && type >= 0 && static_cast<uint32_t>(type) == sixth::slot) return type;
    uint32_t manager = 0, approved = 5;
    if (!RespawnManagerPresent() || !Read(base + kRespawnManager, &manager, 4) || !manager || !Read(manager + 0x30, &approved, 4) || approved >= 4)
        approved = type >= 0 && type < 4 ? static_cast<uint32_t>(type) : 0u;
    if (!TypeSpawnable(approved)) {
        const uint32_t fallback = SpawnableFallback();   // its stream is not here: Transform would find no instance data
        if (fallback != 0xFFFFFFFFu) approved = fallback;
    }
    return static_cast<int>(approved);
}
void DirectSpawnTick(DWORD now) {
    if (!directSpawn || !invasion || play::VersusMode() || mpmap::InVisit() || !inLevel) { directState = Direct::Idle; directKeyDown = false; return; }
    if (directState == Direct::Escaped) {
        if (necroEntity) {
            directState = Direct::Idle;
            necroByChoice = true;
            InterlockedIncrement(&directDone);
            float stands[3]{};
            Read(necroEntity + kEntityMatrix + 0x30, stands, 12);
            Note("direct spawn: necro %08X (type %d) stands at (%.1f %.1f %.1f), wanted (%.1f %.1f %.1f), %lu ms after the key", necroEntity, directType, stands[0], stands[1],
                 stands[2], directSpot[0], directSpot[1], directSpot[2], static_cast<unsigned long>(now - directAt));
            directCheckAt = Stamp(now + 400);
            directCheckHealed = false;
            return;
        }
        if (now - directAt > 4000) {
            directState = Direct::Idle;
            InterlockedIncrement(&directFailed);
            gate::Event("test direct_spawn FAIL: no necro within 4 s (error %ld, player gone %d)", InterlockedCompareExchange(&lastError, 0, 0), playerGone ? 1 : 0);
            return;
        }
        // At once: the respawned Isaac never reached the spot (17.10.: he stood beside the host
        // 1.5 s later, in plain view), and the transform takes the spot's matrix anyway.
        if ((!directTryAt || now - directTryAt >= 300) && !playerGone && IsPlayer(coop::LocalSlotZeroEntity())) {
            directTryAt = Stamp(now);
            // 17.10.: the respawned Isaac still stood at the checkpoint's spawn point (RespawnPlayer places
            // him later), and the necro came there: the spot's matrix goes to the transform itself.
            float isaacAt[3]{};
            Read(coop::LocalSlotZeroEntity() + kEntityMatrix + 0x30, isaacAt, 12);
            const float dx = isaacAt[0] - directSpot[0], dz = isaacAt[2] - directSpot[2];
            if (dx * dx + dz * dz > 4.0f)
                Note("direct spawn: Isaac stands at (%.1f %.1f %.1f), not at the spot yet: the necro takes the spot's matrix", isaacAt[0], isaacAt[1], isaacAt[2]);
            const int saved = type;
            type = directType;
            __try {
                Transform(directMatrix);
            } __except (FaultFilter(GetExceptionInformation())) {
                Note("direct spawn: the transform faulted: exception %08X at %08X", static_cast<unsigned>(GetExceptionCode()), faultAddress);
            }
            type = saved;
        }
        return;
    }
    const bool ghost = LocalRole() == 2 && playerGone && !necroEntity && LocalGhost();
    const bool down = ghost && directSpawnKey > 0 && GameWindowFocused() && (GetAsyncKeyState(directSpawnKey) & 0x8000) != 0;
    const bool pressed = down && !directKeyDown;
    directKeyDown = down;
    if (!ghost) return;
    if (!pressed) {
        if (!PaceLeft(now) && now - directHintAt >= 500 && (!directRefusedAt || now - directRefusedAt >= 2000)) {
            directHintAt = Stamp(now);
            char hint[48];
            std::snprintf(hint, sizeof(hint), "[%c] SPAWN HERE (UNSEEN, %.0f M+)", directSpawnKey >= 0x30 && directSpawnKey <= 0x5A ? static_cast<char>(directSpawnKey) : '?',
                          directMinDistance);
            hud::SetOwn(hint);
        }
        return;
    }
    InterlockedIncrement(&directAsked);
    float spot[3]{}, nearest = 0.0f;
    unsigned who = 0xFF;
    const int why = DirectSpotCheck(now, spot, nearest, who);
    if (why >= 0) {
        InterlockedIncrement(&directRefused[why]);
        directRefusedAt = Stamp(now);
        char line[48];
        std::snprintf(line, sizeof(line), "NO SPAWN HERE: %s", kDirectWhyHud[why]);
        hud::SetOwn(line);
        gate::Event("test direct_spawn REFUSED: %s at (%.1f %.1f %.1f), nearest human %.1f m, %u bot(s) counted", kDirectWhy[why], spot[0], spot[1], spot[2],
                    nearest < 3.0e38f ? nearest : -1.0f, lastDirectBots);
        return;
    }
    // Facing the nearest human (a rotation either way: right = up x forward).
    float h[3]{}, f[3] = {0.0f, 0.0f, 1.0f};
    if (HumanPos(who, now, h)) {
        f[0] = h[0] - spot[0]; f[1] = 0.0f; f[2] = h[2] - spot[2];
        if (!Normalize3(f)) { f[0] = 0.0f; f[1] = 0.0f; f[2] = 1.0f; }
    }
    alignas(16) float matrix[16] = {f[2], 0.0f, -f[0], 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, f[0], 0.0f, f[2], 0.0f, spot[0], spot[1], spot[2], 1.0f};
    std::memcpy(directMatrix, matrix, sizeof(directMatrix));
    directType = DirectSpawnType();
    std::memcpy(directSpot, spot, 12);
    directState = Direct::Escaped;   // before EscapeGhost: the role stays "at the vents" for the humans and the checkpoint
    directAt = Stamp(now);
    directTryAt = 0;
    Note("direct spawn: from the ghost to (%.1f %.1f %.1f), nearest human %.1f m (partner %u), unseen; type %d", spot[0], spot[1], spot[2], nearest, who, directType);
    __try {
        EscapeGhost(matrix);
    } __except (FaultFilter(GetExceptionInformation())) {
        directState = Direct::Idle;
        InterlockedIncrement(&directFailed);
        Note("direct spawn: the way out of the ghost faulted: exception %08X at %08X", static_cast<unsigned>(GetExceptionCode()), faultAddress);
    }
}
// A loud cue on the other machines when a partner's necro comes out (user, 17.10.): his role turning
// "living necro" from the vents or from a human marks the spawn, and his necro body here roars
// through the entity's PlaySound(hash, opts) (vtable +0xA4, PC 0x007D1610 on Player and NecroPlayer,
// thiscall, ret 8; NULL opts = the engine's defaults). The roars live in the MP necro character
// banks, which are loaded while a necro is in the game, and the bank lookup plays any resident
// hash on any entity. Names (sdbm32 of the lowercase name): the lurker's "enemy spotted" roar
// from aud_global_assets.lua, the pack's grondo attack, the spitter's and puker's exit-vent vocals.
bool spawnCue = true;
char spawnSound[64] = "";   // [Necro] SpawnSound: one name for every type; empty = the type's own
const char* const kSpawnSounds[4] = {"pack_attack_grondo.emx", "vx_lurker_attack_grondo.emx", "vo_slasher_aggro_attack.emx", "puker_attack_01.emx"};
constexpr uint32_t kEntityPlaySoundHashRva = 0x3D1610;
DWORD spawnCueAt[coop_peers::kMaxPeers]{};
volatile LONG spawnCuesAsked = 0, spawnCuesPlayed = 0, spawnCuesSilent = 0, spawnCuesNoBody = 0, spawnCueFaults = 0;
uint32_t SoundHash(const char* name) {
    uint32_t h = 0;
    for (; *name; ++name) {
        const uint32_t c = static_cast<unsigned char>(*name >= 'A' && *name <= 'Z' ? *name + 32 : *name);
        h = c + (h << 6) + (h << 16) - h;
    }
    return h;
}
// From the role packets (runtime_coop.inc roles::OnBytes).
void NotePartnerRole(unsigned slot, int before, int after, DWORD now) {
    if (!spawnCue || !invasion || play::VersusMode() || slot >= coop_peers::kMaxPeers || after != 1 || (before != 0 && before != 2)) return;
    spawnCueAt[slot] = Stamp(now);
    InterlockedIncrement(&spawnCuesAsked);
}
__declspec(noinline) uint32_t PlayEntitySound(uint32_t entity, uint32_t hash) {
    uint32_t vtable = 0, fn = 0;
    if (!entity || !Read(entity, &vtable, 4) || !Read(vtable + 0xA4, &fn, 4) || fn != base + kEntityPlaySoundHashRva) return 0;
    __try {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(reinterpret_cast<void*(__thiscall*)(uint32_t, uint32_t, void*)>(fn)(entity, hash, nullptr)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&spawnCueFaults);
        return 0;
    }
}
void SpawnCueTick(DWORD now) {
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        if (!spawnCueAt[s]) continue;
        if (now - spawnCueAt[s] > 3000) {
            spawnCueAt[s] = 0;
            InterlockedIncrement(&spawnCuesNoBody);
            gate::Event("test spawn_cue FAIL: partner %u's necro body never showed here within 3 s, no roar", s);
            continue;
        }
        const uint32_t body = cutscene::NecroGhostFor(peerTable.peers[s].machine);
        if (!body) continue;
        spawnCueAt[s] = 0;
        uint32_t necroType = 0;
        Read(body + kNecroType, &necroType, 4);
        const char* name = spawnSound[0] ? spawnSound : kSpawnSounds[necroType < 4 ? necroType : 1];
        const uint32_t handle = PlayEntitySound(body, SoundHash(name));
        InterlockedIncrement(handle ? &spawnCuesPlayed : &spawnCuesSilent);
        gate::Event("test spawn_cue %s: partner %u's %s roars %s", handle ? "PASS" : "FAIL", s, necroType < 4 ? kTypeNames[necroType] : "necro", name);
        float at[3]{}, soundAt[3]{};
        Read(body + 0xC0, at, 12);
        Read(body + 0x100, soundAt, 12);
        Note("spawn cue: partner %u's necro %08X (type %u) at (%.1f %.1f %.1f, sound source %.1f %.1f %.1f) roars %s (%08X): %s", s, body, necroType, at[0], at[1], at[2],
             soundAt[0], soundAt[1], soundAt[2], name, SoundHash(name), handle ? "playing" : "no handle (bank not loaded here?)");
    }
}
void WriteDirectSpawn(FILE* log) {
    std::fprintf(log, "necro spawn_cue enabled=%d sound='%s' asked=%ld played=%ld silent=%ld no_body=%ld faults=%ld\n", spawnCue ? 1 : 0, spawnSound[0] ? spawnSound : "(by type)",
                 InterlockedCompareExchange(&spawnCuesAsked, 0, 0), InterlockedCompareExchange(&spawnCuesPlayed, 0, 0), InterlockedCompareExchange(&spawnCuesSilent, 0, 0),
                 InterlockedCompareExchange(&spawnCuesNoBody, 0, 0), InterlockedCompareExchange(&spawnCueFaults, 0, 0));
    std::fprintf(log, "necro direct_spawn enabled=%d key=%02X min=%.0f max=%.0f asked=%ld done=%ld failed=%ld refused timer=%ld scene=%ld floor=%ld room=%ld tight=%ld close=%ld seen=%ld far=%ld "
                      "no_los=%ld | finale=%d long_scenes=%ld watch_skips=%ld\n",
                 directSpawn ? 1 : 0, directSpawnKey, directMinDistance, directMaxDistance, InterlockedCompareExchange(&directAsked, 0, 0), InterlockedCompareExchange(&directDone, 0, 0),
                 InterlockedCompareExchange(&directFailed, 0, 0), directRefused[0], directRefused[1], directRefused[2], directRefused[3], directRefused[4], directRefused[5],
                 directRefused[6], directRefused[7], directRefused[8], finaleSeen ? 1 : 0, InterlockedCompareExchange(&finaleLongScenes, 0, 0),
                 InterlockedCompareExchange(&finaleWatchSkips, 0, 0));
}
// The necro life as one state machine (user, 17.10.: "one state machine instead of scattered
// transitions"). Every tick the state is read from the game itself (slot 0, the ghost proxy, our
// necro, the watch, the direct spawn), every change is logged in one place, the action paths
// announce what they expect (LifeExpect), and the rules below are checked: a broken combination
// is healed at once and reported as a "test ... FAIL" line for tools/coop_test_report.py.
const char* const kLifeNames[kLifeCount] = {"off", "human", "living necro", "dying necro", "ghost at the vents", "watching as Isaac", "direct spawning",
                                            "BROKEN (nothing to control)"};
int lifeNow = kLifeOff, lifeExpected = kLifeOff;
DWORD lifeSince = 0, lifeExpectAt = 0;
const char* lifeExpectWhy = "";
volatile LONG lifeChanges = 0, lifeExpectMisses = 0, healBroken = 0, healGhostWithIsaac = 0, healRejoin = 0, healWatch = 0, healStale = 0, healDirectMoved = 0;
DWORD sinceBroken = 0, sinceGhostWithIsaac = 0, sinceHumanByChoice = 0, sinceWatchNoIsaac = 0, sinceStale = 0, dyingFailAt = 0;
void LifeExpect(int state, const char* why) {
    lifeExpected = state;
    lifeExpectWhy = why ? why : "";
    lifeExpectAt = Stamp(GetTickCount());
}
int LifeObserve() {
    if (!enabled || !invasion || play::VersusMode() || mpmap::InVisit() || !inLevel || RestartOrLevelChange()) return kLifeOff;
    if (DirectSpawnPending()) return kLifeSpawning;
    if (watch != Watch::None && watch != Watch::Confirming) return kLifeWatching;
    if (necroEntity && IsNecro(necroEntity)) return NecroDead(necroEntity) ? kLifeDying : kLifeLiving;
    if (playerGone) return LocalGhost() ? kLifeGhost : levelSettled ? kLifeBroken : kLifeOff;
    return IsPlayer(coop::LocalSlotZeroEntity()) ? kLifeHuman : levelSettled ? kLifeBroken : kLifeOff;
}
// An expectation counts as met by the state it names or one it moves on to at once.
bool LifeSatisfies(int expected, int seen) {
    if (expected == seen) return true;
    if (expected == kLifeLiving) return seen == kLifeDying || seen == kLifeGhost;
    if (expected == kLifeHuman) return seen == kLifeWatching || seen == kLifeSpawning || seen == kLifeLiving;
    if (expected == kLifeGhost) return seen == kLifeLiving || seen == kLifeSpawning;
    return false;
}
// A held condition: true once it has stood for ms.
bool Held(bool condition, DWORD& since, DWORD now, DWORD ms) {
    if (!condition) { since = 0; return false; }
    if (!since) { since = Stamp(now); return false; }
    return now - since >= ms;
}
void PlaceBody(uint32_t entity, const float* matrix16) {
    uint32_t vtable = 0, setMatrix = 0;
    if (!Read(entity, &vtable, 4) || !Read(vtable + kEntitySetMatrixSlot, &setMatrix, 4) || !setMatrix) return;
    reinterpret_cast<void(__thiscall*)(uint32_t, const float*)>(setMatrix)(entity, matrix16);
    const uint32_t cmc = FindCmc(entity);
    if (cmc) reinterpret_cast<void(__thiscall*)(uint32_t, const float*)>(base + kCmcSetMatrixRva)(cmc, matrix16);
}
// The ghost proxy without the ghost: HUD hidden, the proxy's own teardown, forgotten.
void DropGhost(const char* why) {
    const uint32_t proxy = LocalGhost();
    HideNecroHud();
    if (proxy) {
        uint32_t vtable = 0, shutdown = 0;
        __try {
            if (Read(proxy, &vtable, 4) && Read(vtable + 0x1C, &shutdown, 4) && shutdown) reinterpret_cast<void(__thiscall*)(uint32_t)>(shutdown)(proxy);
            reinterpret_cast<void(__cdecl*)(uint32_t)>(base + kDelete)(proxy);
        } __except (FaultFilter(GetExceptionInformation())) {
            Note("life: dropping the ghost proxy %08X faulted at %08X", proxy, faultAddress);
        }
    }
    ForgetGhost();
    Note("life: ghost proxy %08X dropped (%s)", proxy, why);
}
void LifeTick(DWORD now) {
    OwnSuitTick(now);
    const int seen = LifeObserve();
    if (seen != lifeNow) {
        const bool met = lifeExpected != kLifeOff && LifeSatisfies(lifeExpected, seen);
        gate::Event("life: %s -> %s after %lu ms%s%s", kLifeNames[lifeNow], kLifeNames[seen], static_cast<unsigned long>(lifeSince ? now - lifeSince : 0),
                    met ? ", as expected: " : "", met ? lifeExpectWhy : "");
        if (lifeNow == kLifeDying && seen == kLifeGhost)
            gate::Event("test death_to_ghost PASS: the ghost came %lu ms after the death", static_cast<unsigned long>(now - lifeSince));
        if (met) lifeExpected = kLifeOff;
        lifeNow = seen;
        lifeSince = Stamp(now);
        InterlockedIncrement(&lifeChanges);
    }
    if (lifeExpected != kLifeOff && now - lifeExpectAt > 6000) {
        if (!LifeSatisfies(lifeExpected, seen)) {
            InterlockedIncrement(&lifeExpectMisses);
            gate::Event("test life FAIL: %s was expected 6 s ago (%s), it is %s", kLifeNames[lifeExpected], lifeExpectWhy, kLifeNames[seen]);
        }
        lifeExpected = kLifeOff;
    }
    if (joinTestAt) {
        if (seen == kLifeLiving || seen == kLifeGhost || seen == kLifeDying) {
            gate::Event("test rejoin PASS: a necro again %lu ms after the join was asked", static_cast<unsigned long>(now - joinTestAt));
            joinTestAt = 0;
        } else if (now - joinTestAt > 120000) {
            gate::Event("test rejoin FAIL: still %s 120 s after the join was asked", kLifeNames[seen]);
            joinTestAt = 0;
        }
    }
    if (seen == kLifeOff) { sinceBroken = sinceGhostWithIsaac = sinceHumanByChoice = sinceWatchNoIsaac = sinceStale = 0; return; }
    // Rule: a necro body that died stays dying no longer than 20 s (the death camera, then the ghost).
    if (seen == kLifeDying && now - lifeSince > 20000 && (!dyingFailAt || now - dyingFailAt > 30000)) {
        dyingFailAt = Stamp(now);
        gate::Event("test death_to_ghost FAIL: dead for %lu ms and no ghost", static_cast<unsigned long>(now - lifeSince));
    }
    // Rule: something to control. Gone Isaac, no ghost, no necro for 6 s: Isaac comes back where the
    // necro was, and a necro by choice joins again.
    if (Held(seen == kLifeBroken && levelSettled, sinceBroken, now, 6000)) {
        sinceBroken = 0;
        InterlockedIncrement(&healBroken);
        gate::Event("test life FAIL healed: nothing to control for 6 s (player gone %d, ghost %08X, necro %08X, slot 0 %08X)", playerGone ? 1 : 0, ghostProxy, necroEntity,
                    coop::LocalSlotZeroEntity());
        if (playerGone) {
            __try { EscapeGhost(); } __except (FaultFilter(GetExceptionInformation())) { Note("life: the way back to Isaac faulted at %08X", faultAddress); }
            if (necroByChoice && !InterlockedCompareExchange(&joinAsNecroPending, 0, 0)) JoinAsNecro();
        }
    }
    // Rule: no ghost proxy beside Isaac (it holds the camera and the vent HUD).
    if (Held(LocalGhost() && !playerGone && IsPlayer(coop::LocalSlotZeroEntity()) && seen != kLifeSpawning, sinceGhostWithIsaac, now, 2000)) {
        sinceGhostWithIsaac = 0;
        InterlockedIncrement(&healGhostWithIsaac);
        gate::Event("test life FAIL healed: a ghost proxy %08X beside Isaac for 2 s, dropped", ghostProxy);
        DropGhost("beside Isaac");
    }
    // Rule: a necro by choice is no human for long (a restart, a level change, a watch that ended).
    const bool humanByChoice = seen == kLifeHuman && levelSettled && necroByChoice && !InterlockedCompareExchange(&joinAsNecroPending, 0, 0) && !transformAfterScene && !SceneHold(now) &&
                               !spectate::Active() && !checkpoint::ReloadWaiting();
    if (Held(humanByChoice, sinceHumanByChoice, now, 10000)) {
        sinceHumanByChoice = 0;
        InterlockedIncrement(&healRejoin);
        gate::Event("test rejoin FAIL healed: a necro by choice stood as a human for 10 s, joining again");
        JoinAsNecro();
    }
    // Rule: a watch has Isaac in slot 0.
    if (Held(watch == Watch::Watching && !IsPlayer(coop::LocalSlotZeroEntity()), sinceWatchNoIsaac, now, 6000)) {
        sinceWatchNoIsaac = 0;
        InterlockedIncrement(&healWatch);
        gate::Event("test watch FAIL healed: watching for 6 s without Isaac in slot 0, the watch is dropped");
        DropWatch("Isaac never stood in slot 0", &watchRefusedFault);
    }
    // Rule: our necro pointer is a necro.
    if (Held(necroEntity && !IsNecro(necroEntity), sinceStale, now, 2000)) {
        sinceStale = 0;
        InterlockedIncrement(&healStale);
        gate::Event("test life FAIL healed: the cached necro %08X is no necro any more, forgotten", necroEntity);
        necroEntity = 0;
    }
    // The direct spawn's result 1.5 s later: the necro still stands at the spot.
    if (directCheckAt && static_cast<int32_t>(now - directCheckAt) >= 0) {
        directCheckAt = 0;
        float at[3]{};
        const bool have = necroEntity && IsNecro(necroEntity) && Read(necroEntity + kEntityMatrix + 0x30, at, 12);
        const float dx = at[0] - directSpot[0], dy = at[1] - directSpot[1], dz = at[2] - directSpot[2], d = std::sqrt(dx * dx + dy * dy + dz * dz);
        // 0.4 s after the spawn: a walk is at most 3 m away; the game's own placing jumped 10 m and more
        // (17.10.: the 1.5 s check put back a necro the player had walked 5 m).
        if (have && d <= 6.0f) {
            gate::Event("test direct_spawn PASS: the necro stands %.1f m from the spot (%.1f %.1f %.1f)", d, at[0], at[1], at[2]);
        } else if (have && !directCheckHealed) {
            directCheckHealed = true;
            InterlockedIncrement(&healDirectMoved);
            gate::Event("test direct_spawn FAIL healed: the necro was moved to (%.1f %.1f %.1f), %.1f m from the spot: put back", at[0], at[1], at[2], d);
            __try { PlaceBody(necroEntity, directMatrix); } __except (FaultFilter(GetExceptionInformation())) {}
            directCheckAt = Stamp(now + 400);
        } else {
            gate::Event("test direct_spawn FAIL: %s", have ? "moved away again after the fix" : "no living necro 1.5 s after the spawn");
        }
    }
}
void WriteLife(FILE* log) {
    std::fprintf(log, "necro life state=%s for %lu ms changes=%ld expect_misses=%ld | heals broken=%ld ghost_with_isaac=%ld rejoin=%ld watch=%ld stale=%ld direct_moved=%ld\n",
                 kLifeNames[lifeNow], static_cast<unsigned long>(lifeSince ? GetTickCount() - lifeSince : 0), InterlockedCompareExchange(&lifeChanges, 0, 0),
                 InterlockedCompareExchange(&lifeExpectMisses, 0, 0), InterlockedCompareExchange(&healBroken, 0, 0), InterlockedCompareExchange(&healGhostWithIsaac, 0, 0),
                 InterlockedCompareExchange(&healRejoin, 0, 0), InterlockedCompareExchange(&healWatch, 0, 0), InterlockedCompareExchange(&healStale, 0, 0),
                 InterlockedCompareExchange(&healDirectMoved, 0, 0));
}
// Any other way out of the ghost that leaves the HUD up: no ghost and no necro at the vents for
// 1.5 s, so it goes (it holds the input, and the MP maps run their own).
DWORD hudOrphanSince = 0;
void HudHeal(DWORD now) {
    if (hudSweeps > 0 && inLevel && levelSettled && !LocalGhost() && !play::VersusMode() && !mpmap::InVisit() && now - hudSweepAt >= 3000) {
        --hudSweeps;
        hudSweepAt = Stamp(now);
        HideNecroHud();
        InterlockedIncrement(&hudSweepsDone);
        Note("hud: type menu and vent HUD hidden after the load (pass %d of 2, role %d)", 2 - hudSweeps, LocalRole());
    }
    // 17.10.: the living necro of a direct spawn kept the vent HUD and its input; playerGone is true
    // for a living necro too, so only the ghost itself may have the HUD.
    const LONG mask = InterlockedCompareExchange(&hudShown, 0, 0), engine = EngineShown();
    const bool orphan = (mask || engine) && inLevel && !LocalGhost() && !play::VersusMode() && !mpmap::InVisit();
    if (!orphan) { hudOrphanSince = 0; return; }
    if (!hudOrphanSince) { hudOrphanSince = Stamp(now); return; }
    if (now - hudOrphanSince < 1500) return;
    hudOrphanSince = 0;
    const LONG shown = mask | engine;
    HideNecroHud();
    InterlockedIncrement(&hudOrphansHidden);
    gate::Event("test hud FAIL healed: the necro HUD (ours %ld, engine %ld) stood without a ghost for 1.5 s (role %d), hidden; afterwards ours %ld engine %ld", mask, engine,
                LocalRole(), InterlockedCompareExchange(&hudShown, 0, 0), EngineShown());
    Note("hud: %s%s stayed up without a ghost (role %d), hidden", shown & 1 ? "the type menu" : "", shown == 3 ? " and the vent HUD" : shown & 2 ? "the vent HUD" : "", LocalRole());
}

void Poll() {
    if (!enabled) return;
    KeepNameplatesOff();
    PollPool();
    TuneGhost(GetTickCount());
    HudHeal(GetTickCount());
    if (!inLevel) {
        joinGhostWanted = false;
        transformAfterScene = false;
        bool wasWatching = false;
        if (watch != Watch::None) { gate::Event("necro watch: the level went"); watch = Watch::None; wasWatching = true; }
        if (InterlockedCompareExchange(&hudShown, 0, 0) || ghostProxy || playerGone) hudSweeps = 2;
        sitOut = false;
        sceneSignalSince = sceneQuietSince = 0;
        std::memset(kindWatchSince, 0, sizeof(kindWatchSince));
        if (necroByChoice && !InterlockedCompareExchange(&joinAsNecroPending, 0, 0) && (necroEntity || playerGone || wasWatching)) {
            InterlockedIncrement(&watchRejoins);
            JoinAsNecro();   // a necro by choice is one again in the next level
        }
        if (InterlockedCompareExchange(&joinAsNecroPending, 0, 0) && !level::joinArmed && GetTickCount() - joinAsNecroSince > 120000) {
            InterlockedExchange(&joinAsNecroPending, 0);
            Note("join: no level within two minutes, the necro join is dropped");
        }
        if (necroEntity || parkedPlayer || playerGone || deadNecro || ghostProxy) { necroEntity = parkedPlayer = necroProxy = deadNecro = ghostProxy = 0; settings = 0; playerGone = false; haveLastNecroMatrix = false; }
        overriddenVentAttrs = 0; overriddenVentType = 0; ventOverrideAt = 0; adoptedAt = 0;   // the level took its vents with it
        return;
    }
    PollDemand();
    ApplyVentRadius();
    InvasionTick();
    if (invasion && !play::VersusMode()) {
        __try { WatchTick(GetTickCount()); } __except (FaultFilter(GetExceptionInformation())) {
            Note("watch: tick faulted: exception %08X at %08X", static_cast<unsigned>(GetExceptionCode()), faultAddress);
            watch = Watch::None;
        }
        PaceTick(GetTickCount());
        DirectSpawnTick(GetTickCount());
        SpawnCueTick(GetTickCount());
        __try { LifeTick(GetTickCount()); } __except (FaultFilter(GetExceptionInformation())) {
            Note("life: tick faulted: exception %08X at %08X", static_cast<unsigned>(GetExceptionCode()), faultAddress);
        }
    }
    if (InterlockedCompareExchange(&joinAsNecroPending, 0, 0)) {
        const DWORD now = GetTickCount();
        const bool ready = levelSettled && peerTable.Count() && !playerGone && !necroEntity && IsPlayer(coop::LocalSlotZeroEntity()) &&
                           !checkpoint::ReloadWaiting();
        if (!ready) joinAsNecroSettledAt = 0;
        else if (!joinAsNecroSettledAt) joinAsNecroSettledAt = Stamp(now);
        else if (now - joinAsNecroSettledAt >= 3000) {
            InterlockedExchange(&joinAsNecroPending, 0);
            InterlockedIncrement(&joinAsNecroDone);
            Note("join: the host's level runs, turning into a necromorph now");
            keyPressedByJoin = true;
            joinGhostWanted = invasion;   // then into the vents, the camera near the humans
        }
    }
    // The MP screens and the respawn manager only where a necro can be: a Versus
    // match or a transform asked for. 12.10.: both were built in every campaign
    // level, and the host registered the manager with the story session.
    if (play::VersusMode() || poolDemanded || transformWhenReady || playerGone || necroEntity || ghostProxy) {
        EnsureMpUi();
        EnsureRespawnManager();
    }
    if (necroEntity && IsNecro(necroEntity) && Read(necroEntity + kEntityMatrix, lastNecroMatrix, sizeof(lastNecroMatrix))) haveLastNecroMatrix = true;
    // The necro lost slot 0 while its body still exists: the RespawnProxy took
    // the slot, i.e. it died (the hits hook does not see a necro's own death).
    // From here the corpse belongs to the native flow; F11 means EscapeGhost.
    if (necroEntity && IsNecro(necroEntity) && NecroInSlotZero() != necroEntity) {
        Note("necro %08X left slot 0 (dead, ghost phase); native respawn flow owns it now", necroEntity);
        deadNecro = necroEntity;
        necroEntity = 0;
        necroProxy = necroSubKey = 0;
    }
    // The spawned necro can vanish without notice (its own death/respawn logic,
    // an unseen restart): follow slot 0 rather than trust the cache.
    if (necroEntity && !IsNecro(necroEntity)) {
        const uint32_t current = NecroInSlotZero();
        Note("necro %08X vanished; slot 0 holds %s%08X", necroEntity, current ? "necro " : "no necro ", current);
        necroEntity = current;
        necroProxy = necroSubKey = 0;
        if (!current && !IsPlayer(parkedPlayer)) parkedPlayer = 0;
    }
    // A checkpoint restart gives slot 0 a fresh Isaac without leaving the
    // level, so nothing above resets: the necro is gone, but playerGone still
    // says "playing a necro". The next F11 then took the ghost-phase branch and
    // spawned a SECOND Isaac next to the restart's own, and the F11 after that
    // removed only one of them; the other kept following the controller
    // (22.09.). When slot 0 holds a real Player again, the player is human again.
    if (playerGone && !necroEntity && IsPlayer(coop::LocalSlotZeroEntity())) {
        Note("slot 0 holds Isaac %08X again (checkpoint restart); necro state cleared", coop::LocalSlotZeroEntity());
        playerGone = false;
        deadNecro = ghostProxy = parkedPlayer = necroProxy = necroSubKey = 0;
        haveLastNecroMatrix = false;
        collisionPendingAt = 0; adoptedAt = 0;
        InterlockedIncrement(&restartResets);
    }
    // The native respawn put a fresh necro into slot 0 (vent exit): adopt it
    // (collision type, session registration) exactly as after a transform.
    if (playerGone && !necroEntity) {
        const uint32_t current = NecroInSlotZero();
        if (current && current != deadNecro) {
            deadNecro = 0;
            necroEntity = current;
            // The collision mode is not touched while NecroPlayerExitVentSM owns
            // the character controller (its exit animation runs with its own
            // modes and RestoreComponentSettings at the end): switching it
            // mid-exit dropped a puker through the floor (14:32). Applied once
            // the exit SM reports done or landed, or after six seconds.
            collisionPendingAt = GetTickCount();
            exitDoneBaseline = InterlockedCompareExchange(&exitTransitionTrue[1], 0, 0) + InterlockedCompareExchange(&exitTransitionTrue[5], 0, 0);
            RegisterNecroWithSession(current);
            InterlockedIncrement(&transforms);
            adoptedAt = GetTickCount();
            ventRequestAt = 0;
            unstickDone = !Read(current + kEntityMatrix + 0x30, adoptedPos, 12);
            fallRescueDone = false;
            haveSafe = false;   // a new body out of a new vent: its own spot counts, not the last life's
            safeSampleAt = 0;
            exitReportDone = unstickDone;
            Note("adopted respawned necro %08X (proxy %08X subkey %08X)", current, necroProxy, necroSubKey);
        }
    }
    // Vent respawn: the player collision mode goes on once the exit SM is done
    // (transition 0xC floor-done or 0x10 anim-done fired since adoption) or
    // after six seconds at the latest.
    if (collisionPendingAt && necroEntity && IsNecro(necroEntity)) {
        const LONG exitDone = InterlockedCompareExchange(&exitTransitionTrue[1], 0, 0) + InterlockedCompareExchange(&exitTransitionTrue[5], 0, 0);
        if (exitDone != exitDoneBaseline || GetTickCount() - collisionPendingAt > 6000) {
            collisionPendingAt = 0;
            ApplyPlayerCollision(necroEntity);
            Note("player collision applied to respawned necro %08X (%s)", necroEntity, exitDone != exitDoneBaseline ? "exit SM done" : "6 s timeout");
        }
    } else if (collisionPendingAt && !necroEntity) collisionPendingAt = 0;
    // The walk-in vent mapping ends once the exit is over: eight seconds after
    // the fresh necro was adopted (the land animation is long over by then).
    if (overriddenVentAttrs && adoptedAt && GetTickCount() - adoptedAt > 8000) RestoreVentType();
    // Four seconds after the vent spawn: how far the necro got, for the vent
    // exit diagnosis (type of the vent, distance from the spawn spot).
    if (necroEntity && adoptedAt && !exitReportDone && GetTickCount() - adoptedAt > 4000 && IsNecro(necroEntity)) {
        exitReportDone = true;
        float pos[3]{};
        if (Read(necroEntity + kEntityMatrix + 0x30, pos, 12)) {
            float moved[3] = {pos[0] - adoptedPos[0], pos[1] - adoptedPos[1], pos[2] - adoptedPos[2]};
            Note("vent exit report: type %u, edge %u (asked %u), necro %08X moved %.2f m in 4 s (placed at %.1f %.1f %.1f, adopted at %.1f %.1f %.1f, now %.1f %.1f %.1f, centroid %.1f %.1f %.1f, axis A %.2f %.2f %.2f, axis B %.2f %.2f %.2f)",
                 lastGrantedVentType, lastSpawnSide, lastSpawnRequested, necroEntity, std::sqrt(Dot3(moved, moved)), lastSpawnPos[0], lastSpawnPos[1], lastSpawnPos[2],
                 adoptedPos[0], adoptedPos[1], adoptedPos[2], pos[0], pos[1], pos[2],
                 grantedVentCentroid[0], grantedVentCentroid[1], grantedVentCentroid[2], lastVentAxisA[0], lastVentAxisA[1], lastVentAxisA[2], lastVentAxisB[0], lastVentAxisB[1], lastVentAxisB[2]);
        }
    }
    // Vent exit watchdog: a fresh necro still within half a metre of where it
    // spawned, three seconds on, is stuck in the vent. Moved two metres into
    // the room (away from the vent centroid), entity and character controller.
    if (ventUnstick && necroEntity && adoptedAt && !unstickDone && GetTickCount() - adoptedAt > 3000 && IsNecro(necroEntity)) {
        unstickDone = true;
        float pos[3]{};
        if (Read(necroEntity + kEntityMatrix + 0x30, pos, 12)) {
            float moved[3] = {pos[0] - adoptedPos[0], pos[1] - adoptedPos[1], pos[2] - adoptedPos[2]};
            if (Dot3(moved, moved) < 0.25f) {
                float dir[3] = {1.0f, 0.0f, 0.0f};
                if (haveGrantedVent) {
                    float away[3] = {pos[0] - grantedVentCentroid[0], 0.0f, pos[2] - grantedVentCentroid[2]};
                    if (Normalize3(away)) std::memcpy(dir, away, sizeof(dir));
                }
                alignas(16) float matrix[16];
                if (Read(necroEntity + kEntityMatrix, matrix, sizeof(matrix))) {
                    matrix[12] += dir[0] * 2.0f; matrix[13] += 0.2f; matrix[14] += dir[2] * 2.0f;
                    __try {
                        uint32_t vtable = 0, setMatrix = 0;
                        if (Read(necroEntity, &vtable, 4) && Read(vtable + kEntitySetMatrixSlot, &setMatrix, 4) && setMatrix) {
                            using MatrixCall = void (__thiscall*)(uint32_t, const float*);
                            reinterpret_cast<MatrixCall>(setMatrix)(necroEntity, matrix);
                            const uint32_t cmc = FindCmc(necroEntity);
                            if (cmc) reinterpret_cast<MatrixCall>(base + kCmcSetMatrixRva)(cmc, matrix);
                            InterlockedIncrement(&unstuck);
                            Note("vent exit watchdog: necro %08X had not left (%.1f %.1f %.1f), moved to (%.1f %.1f %.1f)", necroEntity, pos[0], pos[1], pos[2], matrix[12], matrix[13], matrix[14]);
                        }
                    } __except (FaultFilter(GetExceptionInformation())) { InterlockedExchange(&lastError, 61); }
                }
            }
        }
    }
    // Fall-through rescue. An exit animation that ends below the floor leaves the exit
    // SM's loop falling for ever: 27 m and 36 m out of wall vents on 22.09., the Leaper
    // gone for good. A necro under the map (below the lowest spawn point, or in the
    // origin's column) is put back where it last stood, else two metres out of its vent
    // at the adopted height, entity and character controller together. Under the map,
    // not just six metres below the vent: a jump down a storey right after the exit
    // was put back up (13.10.), and a fall later than six seconds was never caught.
    if (ventFallRescue && necroEntity && IsNecro(necroEntity) && mpmap::InVisit()) {
        float now3[3]{};
        const DWORD now = GetTickCount();
        if (Read(necroEntity + kEntityMatrix + 0x30, now3, 12) && mpmap::UsableAnchor(now3) && now - safeSampleAt >= 500) {
            if (safeSampleAt && std::fabs(now3[1] - safeSample[1]) < 0.3f) { std::memcpy(safePos, now3, sizeof(safePos)); haveSafe = true; }
            std::memcpy(safeSample, now3, sizeof(safeSample));
            safeSampleAt = now;
        }
    }
    if (ventFallRescue && necroEntity && IsNecro(necroEntity) && mpmap::InVisit() && GetTickCount() - fallRescueAt >= 2000) {
        float pos[3]{};
        if (Read(necroEntity + kEntityMatrix + 0x30, pos, 12) && !mpmap::UsableAnchor(pos) && (haveSafe || adoptedAt)) {
            fallRescueAt = GetTickCount();
            fallRescueDone = true;
            float dir[3] = {1.0f, 0.0f, 0.0f};
            if (haveGrantedVent) {
                float away[3] = {adoptedPos[0] - grantedVentCentroid[0], 0.0f, adoptedPos[2] - grantedVentCentroid[2]};
                if (Normalize3(away)) std::memcpy(dir, away, sizeof(dir));
            }
            alignas(16) float matrix[16];
            if (Read(necroEntity + kEntityMatrix, matrix, sizeof(matrix))) {
                if (haveSafe) { matrix[12] = safePos[0]; matrix[13] = safePos[1] + 0.3f; matrix[14] = safePos[2]; }
                else { matrix[12] = adoptedPos[0] + dir[0] * 2.0f; matrix[13] = adoptedPos[1] + 0.3f; matrix[14] = adoptedPos[2] + dir[2] * 2.0f; }
                __try {
                    uint32_t vtable = 0, setMatrix = 0;
                    if (Read(necroEntity, &vtable, 4) && Read(vtable + kEntitySetMatrixSlot, &setMatrix, 4) && setMatrix) {
                        using MatrixCall = void (__thiscall*)(uint32_t, const float*);
                        reinterpret_cast<MatrixCall>(setMatrix)(necroEntity, matrix);
                        const uint32_t cmc = FindCmc(necroEntity);
                        if (cmc) reinterpret_cast<MatrixCall>(base + kCmcSetMatrixRva)(cmc, matrix);
                        InterlockedIncrement(&fallRescues);
                        Note("fall rescue: necro %08X under the map at (%.1f %.1f %.1f), put back at (%.1f %.1f %.1f), %s",
                             necroEntity, pos[0], pos[1], pos[2], matrix[12], matrix[13], matrix[14], haveSafe ? "where it last stood" : "out of its vent");
                    }
                } __except (FaultFilter(GetExceptionInformation())) { InterlockedExchange(&lastError, 62); }
            }
        }
    }
    // The necro died. DeathRevert=1: Isaac comes straight back where it fell.
    // Otherwise the multiplayer's own flow runs: death camera, ghost proxy,
    // vent pick, respawn out of the vent (see InstallNativeRespawn).
    if (InterlockedCompareExchange(&deathPending, 0, 1) == 1 && necroEntity) {
        if (deathRevert) {
            Note("necro %08X died; reverting to Isaac", necroEntity);
            __try { Revert(true, true); } __except (FaultFilter(GetExceptionInformation())) {
                InterlockedExchange(&lastError, 99);
                Note("death revert faulted: exception %08X at %08X", static_cast<unsigned>(GetExceptionCode()), faultAddress);
            }
            return;
        }
        Note("necro %08X died; native respawn flow (ghost camera -> vent) takes over", necroEntity);
        PaceStart("the death");
        // The corpse stays until the SM respawns or the level goes; it is no
        // longer ours to delete (F11 on it while NecroRespawnProxySM still
        // pointed at it: crash 26060). F11 now means EscapeGhost.
        deadNecro = necroEntity;
        necroEntity = 0;
        necroProxy = necroSubKey = 0;
    }
    // Two instances on one machine both see the key (GetAsyncKeyState is
    // system-wide); only the game window in the foreground acts on it.
    DWORD foregroundProcess = 0;
    const HWND foreground = GetForegroundWindow();
    if (foreground) GetWindowThreadProcessId(foreground, &foregroundProcess);
    const bool focused = foregroundProcess == GetCurrentProcessId();
    const bool down = focused && key && (GetAsyncKeyState(key) & 0x8000) != 0;
    bool pressed = (down && !keyDown) || keyPressedByJoin;
    keyDown = down;
    keyPressedByJoin = false;
    if (pressed && watch != Watch::None) {
        InterlockedIncrement(&watchKeyIgnored);
        Note("transform: ignored while watching the humans' scene");
        return;
    }
    if (pressed && spectate::Active()) {
        InterlockedIncrement(&spectate::necroKeyRefused);
        Note("transform: refused, this Isaac is dead and watches a partner until the next checkpoint");
        return;
    }
    if (!necroEntity && !playerGone && !play::VersusMode()) {
        const DWORD tick = GetTickCount();
        if (pressed && SceneHold(tick)) {
            transformAfterScene = !transformAfterScene;   // a second press takes the wish back
            InterlockedIncrement(&transformsHeld);
            Note("transform: a human plays a scene, %s", transformAfterScene ? "the necromorph comes after it" : "the wish is taken back");
            return;
        }
        if (!pressed && transformAfterScene && !SceneHold(tick)) {
            transformAfterScene = false;
            pressed = true;
            Note("transform: the humans' scene is over, turning into a necromorph now");
        }
    } else {
        transformAfterScene = false;
    }
    if (!pressed) return;
    __try {
        if (necroEntity) { Revert(true, true); necroByChoice = false; clientprog::AfterIsaacReturns(LocalCampaignPlayer(), "the necro key"); }
        else if (playerGone) { EscapeGhost(); necroByChoice = false; clientprog::AfterIsaacReturns(LocalCampaignPlayer(), "the necro key"); }   // ghost camera phase: back to Isaac where the necro fell
        else if (poolMode == 2 && !PoolDone()) {
            Demand(true);
            transformWhenReady = true;
            Note("transform: necro pool requested on demand, transforming once every machine has it");
        }
        else { Transform(); if (necroEntity || playerGone) necroByChoice = true; }
    } __except (FaultFilter(GetExceptionInformation())) {
        InterlockedExchange(&lastError, 99);
        allowMp = false;   // a fault inside SpawnByInfo would leave the gate open
        Note("transform faulted at stage %ld: exception %08X at %08X (rva %08X)",
             InterlockedCompareExchange(&stage, 0, 0), static_cast<unsigned>(GetExceptionCode()), faultAddress,
             faultAddress >= base ? static_cast<unsigned>(faultAddress - base) : 0u);
    }
}

// NecroPlayer::HandlePostSpawnInit (PC 0x010A57C0) asks a non-authoritative
// necro (a ghost of the partner's) for its OnlinePlayer through
// GetOnlineGame()->GetPlayerByRef(): the campaign has no Blaze game, the inner
// pointer is null and GetPlayerByRef reads [0+0x1C0] (host crash 28484 when the
// client became a pack). The authoritative branch right above it uses
// GetLocalOnlinePlayer, which tolerates an invalid ref, and everything after
// the two branches is guarded. So the `je` that selects the ghost branch
// (0x010A57DD: 74 0F) becomes two nops: every necro takes the tolerant path.
constexpr uint32_t kGhostBranchRva = 0xCA57DD;
volatile LONG ghostBranchPatched = 0;
void PatchGhostPostSpawn(FILE* log) {
    unsigned char actual[2]{};
    static const unsigned char expected[2] = {0x74, 0x0F}, nops[2] = {0x90, 0x90};
    if (!Read(base + kGhostBranchRva, actual, 2)) { std::fprintf(log, "necro ghost_branch unreadable\n"); return; }
    if (std::memcmp(actual, nops, 2) == 0) { InterlockedExchange(&ghostBranchPatched, 1); return; }
    if (std::memcmp(actual, expected, 2) != 0) {
        std::fprintf(log, "necro ghost_branch signature mismatch at %08X: %02X %02X (wanted 74 0F); NOT patched, a partner necro will crash this host\n",
                     static_cast<unsigned>(base + kGhostBranchRva), actual[0], actual[1]);
        return;
    }
    if (WriteBytes(base + kGhostBranchRva, nops, 2)) { InterlockedExchange(&ghostBranchPatched, 1); std::fprintf(log, "necro ghost_branch patched at %08X (HandlePostSpawnInit takes the tolerant path for ghosts)\n", static_cast<unsigned>(base + kGhostBranchRva)); }
    else std::fprintf(log, "necro ghost_branch write failed\n");
}

// JoinedGame::GetPlayerByRef (PC 0x005F2CE0, thiscall(this, int64 ref), ret 8) is
// what every "which online player is this" question funnels into: NecroPlayer's
// ghost post-spawn init, Player::CreateGhostNameVFX and, on every damage event
// once MPLevelSettings exists, MultiplayerUIRelay::SetKillerInfo (from
// DismemberableHealthComponent::HandleDamage). Its `this` is the Blaze game
// inside the JoinedGame, null in the campaign, and the first thing it does is
// read [this+0x1C0]. A null `this` now answers "no such player" instead.
safetyhook::InlineHook playerByRefHook;
volatile LONG playerByRefNull = 0, playerByRefPassed = 0;
uint32_t __fastcall OnGetPlayerByRef(void* self, void*, uint32_t refLow, uint32_t refHigh) {
    if (!self) { InterlockedIncrement(&playerByRefNull); return 0; }
    InterlockedIncrement(&playerByRefPassed);
    return playerByRefHook.thiscall<uint32_t>(self, refLow, refHigh);
}
void InstallPlayerByRefGuard(FILE* log) {
    constexpr uint32_t kGetPlayerByRefRva = 0x1F2CE0;
    static const unsigned char expected[] = {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kGetPlayerByRefRva, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) != 0) {
        std::fprintf(log, "necro player_by_ref guard: signature mismatch at %08X, not installed\n", static_cast<unsigned>(base + kGetPlayerByRefRva));
        return;
    }
    playerByRefHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kGetPlayerByRefRva), reinterpret_cast<void*>(OnGetPlayerByRef));
    std::fprintf(log, "necro player_by_ref guard %s at %08X\n", playerByRefHook ? "installed" : "FAILED", static_cast<unsigned>(base + kGetPlayerByRefRva));
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Necro", "Enabled", 0, ini.c_str()) != 0;
    nativeRespawn = GetPrivateProfileIntA("Necro", "NativeRespawn", 1, ini.c_str()) != 0;
    ventSearchRadius = static_cast<float>(GetPrivateProfileIntA("Necro", "VentSearchRadius", 150, ini.c_str()));
    invasion = GetPrivateProfileIntA("Necro", "Invasion", 1, ini.c_str()) != 0;
    watchScenes = GetPrivateProfileIntA("Necro", "WatchScenes", 1, ini.c_str()) != 0;
    watchSuit = GetPrivateProfileIntA("Necro", "WatchSuit", 0, ini.c_str()) != 0;
    birthSuits = GetPrivateProfileIntA("Necro", "BirthSuits", 1, ini.c_str()) != 0;
    opensDoors = GetPrivateProfileIntA("Necro", "OpensDoors", 0, ini.c_str()) != 0;
    {
        const int pace = GetPrivateProfileIntA("Necro", "RespawnBaseSeconds", 8, ini.c_str()), most = GetPrivateProfileIntA("Necro", "RespawnMaxSeconds", 30, ini.c_str());
        paceBaseMs = static_cast<DWORD>(pace < 0 ? 0 : pace > 60 ? 60 : pace) * 1000;
        paceMaxMs = static_cast<DWORD>(most < 1 ? 1 : most > 120 ? 120 : most) * 1000;
    }
    {
        const int radius = GetPrivateProfileIntA("Necro", "GhostFollowRadius", 25, ini.c_str());
        ghostFollowRadius = static_cast<float>(radius < 8 ? 8 : radius > 80 ? 80 : radius);
        const int range = GetPrivateProfileIntA("Necro", "VentHumanRange", 45, ini.c_str());
        ventHumanRange = static_cast<float>(range < 10 ? 10 : range > 200 ? 200 : range);
        const int view = GetPrivateProfileIntA("Necro", "GhostHumanViewRange", 50, ini.c_str());
        humanViewRange = view <= 0 ? 0.0f : static_cast<float>(view < 10 ? 10 : view > 200 ? 200 : view);
        char text[32]{};
        GetPrivateProfileStringA("Necro", "GhostSpeed", "6", text, sizeof(text), ini.c_str());
        ghostSpeed = std::clamp(static_cast<float>(std::atof(text)), 1.0f, 10.0f);
        ghostNoclip = GetPrivateProfileIntA("Necro", "GhostNoclip", 1, ini.c_str()) != 0;
        directSpawn = GetPrivateProfileIntA("Necro", "DirectSpawn", 1, ini.c_str()) != 0;
        spawnCue = GetPrivateProfileIntA("Necro", "SpawnCue", 1, ini.c_str()) != 0;
        GetPrivateProfileStringA("Necro", "SpawnSound", "", spawnSound, sizeof(spawnSound), ini.c_str());
        directSpawnKey = GetPrivateProfileIntA("Necro", "DirectSpawnKey", 'Q', ini.c_str());
        const int nearMost = GetPrivateProfileIntA("Necro", "DirectSpawnMinDistance", 7, ini.c_str()), farMost = GetPrivateProfileIntA("Necro", "DirectSpawnMaxDistance", 60, ini.c_str());
        directMinDistance = static_cast<float>(nearMost < 3 ? 3 : nearMost > 40 ? 40 : nearMost);
        directMaxDistance = static_cast<float>(farMost < 15 ? 15 : farMost > 200 ? 200 : farMost);
    }
    if (ventSearchRadius < 10.0f) ventSearchRadius = 10.0f;
    deathRevert = GetPrivateProfileIntA("Necro", "DeathRevert", 0, ini.c_str()) != 0;
    mpUi = GetPrivateProfileIntA("Necro", "MpUi", 1, ini.c_str()) != 0;
    confirmKey = GetPrivateProfileIntA("Necro", "ConfirmKey", VK_RETURN, ini.c_str());
    listenerGuard = GetPrivateProfileIntA("Necro", "ListenerGuard", 1, ini.c_str()) != 0;
    deleteGuard = GetPrivateProfileIntA("Necro", "DeleteGuard", 1, ini.c_str()) != 0;
    typeMenu = GetPrivateProfileIntA("Necro", "TypeMenu", 1, ini.c_str()) != 0;
    ventUnstick = GetPrivateProfileIntA("Necro", "VentUnstick", 0, ini.c_str()) != 0;
    ventFallRescue = GetPrivateProfileIntA("Necro", "VentFallRescue", 1, ini.c_str()) != 0;
    walkInVentMap = GetPrivateProfileIntA("Necro", "WalkInVentMap", 1, ini.c_str()) != 0;
    spawnEdge = GetPrivateProfileIntA("Necro", "SpawnEdge", -1, ini.c_str());
    if (enabled) { InstallStoryGuards(log); PatchGhostPostSpawn(log); InstallPlayerByRefGuard(log); InstallMp12Guard(log); InstallNativeRespawn(log); if (nativeRespawn) { InstallRespawnTrace(log); InstallOnlineRefGuard(log); InstallRespawnManagerHook(log); InstallRequestRespawnStandIn(log); InstallInvasion(log); } InstallTeardownGuard(log); InstallNearestPlayerStandIn(log); InstallListenerGuard(log); InstallDeleteTrace(log); }
    // These guards are not about necromorphs: the freed stream callback at
    // 0x00790622 also ends level exits from the MP maps (dumps 28192, 29868,
    // 09.10., mp_solar) with the necro module off, and so does the stale
    // PlayerDecompressionInfo 58CA3E2F-8C8CD155 in the deferred delete (its GUID
    // is in global_assets and global_mp_assets; dumps 9608 and 42844, 09.10.,
    // back to the main menu from mp_bombRun, abort at 0x007567F7).
    if (!enabled) { InstallListenerGuard(log); InstallDeleteTrace(log); }
    if (enabled) std::fprintf(log, "necro native_respawn=%d death_revert=%d\n", nativeRespawn ? 1 : 0, deathRevert ? 1 : 0);
    // The death is seen by the health hooks; without ReportHits they are installed for this alone.
    if (enabled && deathRevert && !hits::InstallBodyWatch(log))
        std::fprintf(log, "necro death_revert: no health hooks, a necro death goes to the native respawn flow\n");
    key = GetPrivateProfileIntA("Necro", "Key", VK_F11, ini.c_str());
    type = GetPrivateProfileIntA("Necro", "Type", 0, ini.c_str());
    GetPrivateProfileStringA("Necro", "LeaperRecordStream", "randomizer\\ds2_leaper",
                             leaperRecordStream, sizeof(leaperRecordStream), ini.c_str());
    std::fprintf(log, "necro leaper record stream name: %s\n", leaperRecordStream);
    {
        char stream[sizeof(kLeaperStreamName)]{};
        GetPrivateProfileStringA("Necro", "SixthCharStream", kLeaperStreamName, stream, sizeof(stream), ini.c_str());
        std::memcpy(kLeaperStreamName, stream, sizeof(stream));
        std::fprintf(log, "necro sixth type character stream: %s\n", kLeaperStreamName);
    }
    sixth::Configure(log, ini);
    if (sixth::enabled) { InstallLeaperStreamUpdate(log); InstallTentacleGuard(log); InstallSpitterAimGuard(log); InstallGrabImmunity(log); }
    leaper::Configure(log, ini);
    registerNet = GetPrivateProfileIntA("Necro", "RegisterNet", 1, ini.c_str()) != 0;
    poolMode = static_cast<int>(GetPrivateProfileIntA("Necro", "LoadPool", 1, ini.c_str()));
    wantPool = poolMode != 0;
    destroyGlobalLeftovers = GetPrivateProfileIntA("Necro", "DestroyGlobalLeftovers", 1, ini.c_str()) != 0;
    std::fprintf(log, "necro destroy_global_leftovers=%d (0: global_assets entities left at a world close stay for the engine's own unload)\n", destroyGlobalLeftovers ? 1 : 0);
    spawnSettings = GetPrivateProfileIntA("Necro", "SpawnSettings", 1, ini.c_str()) != 0;
    nameplatesOff = GetPrivateProfileIntA("Necro", "NameplatesOff", 1, ini.c_str()) != 0;
    playerCollision = GetPrivateProfileIntA("Necro", "PlayerCollision", 1, ini.c_str()) != 0;
    {
        static const unsigned char expected[] = {0x56, 0x8B, 0x74, 0x24, 0x08, 0x57, 0x8B, 0xF9, 0x83, 0xFE, 0x04};
        unsigned char actual[sizeof(expected)]{};
        setCollisionModeOk = Read(base + kSetCollisionModeRva, actual, sizeof(actual)) && std::memcmp(actual, expected, sizeof(expected)) == 0;
        std::fprintf(log, "necro set_collision_mode %s at %08X (player_collision=%d)\n",
                     setCollisionModeOk ? "signature ok" : "SIGNATURE MISMATCH, door collision hand-over disabled",
                     static_cast<unsigned>(base + kSetCollisionModeRva), playerCollision ? 1 : 0);
    }
    if (key < 0 || key > 255) key = VK_F11;
    std::fprintf(log, "necro enabled=%d key=%d type=%d (%s) register_net=%d load_pool=%d spawn_settings=%d nameplates_off=%d\n",
                 enabled ? 1 : 0, key, type, type >= 0 && type <= 3 ? kTypeNames[type] : "?",
                 registerNet ? 1 : 0, wantPool ? 1 : 0, spawnSettings ? 1 : 0, nameplatesOff ? 1 : 0);
    return enabled;
}

void WriteDeleteGuard(FILE* log) {
    static LONG lastStale = -1, lastForeign = 0;
    const LONG staleNow = InterlockedCompareExchange(&staleEntityCount, 0, 0);
    const LONG foreignNow = InterlockedCompareExchange(&foreignSlotsAccepted, 0, 0);
    if (staleNow == lastStale && foreignNow == lastForeign) return;
    lastStale = staleNow;
    lastForeign = foreignNow;
    std::fprintf(log, "necro delete_guard flush_skipped=%ld delete_skipped=%ld | stale nodes taken off the delete message=%ld not_linked=%ld locked=%ld faults=%ld"
                      " | foreign_slots_accepted=%ld loops_broken=%ld loop_faults=%ld",
                 InterlockedCompareExchange(&staleFlushSkipped, 0, 0), InterlockedCompareExchange(&staleDeleteSkipped, 0, 0),
                 InterlockedCompareExchange(&staleUnlinked, 0, 0), InterlockedCompareExchange(&staleNotLinked, 0, 0), InterlockedCompareExchange(&staleLocked, 0, 0),
                 InterlockedCompareExchange(&staleUnlinkFaults, 0, 0), InterlockedCompareExchange(&foreignSlotsAccepted, 0, 0),
                 InterlockedCompareExchange(&staleLoopsBroken, 0, 0), InterlockedCompareExchange(&staleLoopFaults, 0, 0));
    for (LONG i = 0; i < staleNow && i < 8; ++i) {
        const StaleEntity& s = staleEntities[i];
        std::fprintf(log, " [%s %08X vt=%08X guid=%08X-%08X]", s.where, s.object, s.vtable, s.guid0, s.guid1);
    }
    std::fprintf(log, "\n");
}
void WriteDiagnostics(FILE* log) {
    if (!log) return;
    if (enabled && invasion) WriteInvasion(log);
    if (enabled) std::fprintf(log, "necro story_guards qte_activations=%ld rebound=%ld skipped no_player=%ld no_alert=%ld death_drops skipped=%ld passed=%ld | "
                                   "teardown stand_in uses=%ld faults=%ld | spawn null_caught=%ld\n",
                              qteActivations, qteRebound, qteSkippedNoPlayer, qteSkippedNoAlert, deathDropsSkipped, deathDropsPassed, standInUses, standInFaults,
                              spawnNullCaught);
    if (enabled && invasion)
        std::fprintf(log, "necro watch=%d scenes=%d by_choice=%d asks=%ld starts=%ld from living=%ld ghost=%ld refused=%ld (grab %ld spawning %ld restart %ld no_spot %ld fault %ld) "
                          "ends=%ld (no_scene %ld timeout %ld) returns=%ld return_failures=%ld key_ignored=%ld rejoins=%ld sit_out=%d sat_out=%ld waited_for_ghost=%ld "
                          "cues confirmed=%ld world_events=%ld short_waits=%ld from_kind=%ld\n",
                     static_cast<int>(watch), watchScenes ? 1 : 0, necroByChoice ? 1 : 0, watchAsks, watchStarts, watchFromLiving, watchFromGhost, watchRefused,
                     watchRefusedGrab, watchRefusedSpawning, watchRefusedRestart, watchRefusedNoSpot, watchRefusedFault, watchEnds, watchEndsNoScene, watchEndsTimeout,
                     watchReturns, watchReturnFailures, watchKeyIgnored, watchRejoins, sitOut ? 1 : 0, watchSatOut, watchWaitedForGhost, watchConfirmed,
                     watchUnconfirmed, watchShortWaits, watchFromKind);
    if (enabled && invasion)
        std::fprintf(log, "necro watch_suit state=%d dressed=%ld kept=%ld skipped=%ld timeouts=%ld\n", static_cast<int>(watchDress), InterlockedCompareExchange(&watchDressed, 0, 0),
                     InterlockedCompareExchange(&watchDressKept, 0, 0), InterlockedCompareExchange(&watchDressSkipped, 0, 0), InterlockedCompareExchange(&watchDressTimeouts, 0, 0));
    if (!enabled) {
        // only the crash guards run: their notes ("stream callback guard: ...")
        char pending[kNoteSlots][256];
        unsigned count = 0;
        AcquireSRWLockExclusive(&noteLock);
        for (; noteTail != noteHead && count < kNoteSlots; ++noteTail, ++count)
            std::memcpy(pending[count], notes[noteTail % kNoteSlots], sizeof(pending[count]));
        ReleaseSRWLockExclusive(&noteLock);
        for (unsigned i = 0; i < count; ++i) std::fprintf(log, "necro %s\n", pending[i]);
        WriteDeleteGuard(log);
        return;
    }
    {
        char pending[kNoteSlots][256];
        unsigned count = 0;
        AcquireSRWLockExclusive(&noteLock);
        for (; noteTail != noteHead && count < kNoteSlots; ++noteTail, ++count)
            std::memcpy(pending[count], notes[noteTail % kNoteSlots], sizeof(pending[count]));
        ReleaseSRWLockExclusive(&noteLock);
        for (unsigned i = 0; i < count; ++i) std::fprintf(log, "necro %s\n", pending[i]);
        static LONG lastDropped = 0;
        const LONG dropped = InterlockedCompareExchange(&notesDropped, 0, 0);
        if (dropped != lastDropped) { lastDropped = dropped; std::fprintf(log, "necro notes_dropped=%ld (more than %u notes in one second)\n", dropped, kNoteSlots); }
    }
    {
        static LONG lastCloses = -1, lastExits = -1;
        const LONG closes = InterlockedCompareExchange(&worldCloses, 0, 0), exits = InterlockedCompareExchange(&exitReleases, 0, 0);
        if (closes != lastCloses || exits != lastExits) {
            lastCloses = closes;
            lastExits = exits;
            std::fprintf(log, "necro world_close closes=%ld streams_dropped=%ld faults=%ld exit_releases=%ld exit_streams=%ld leftover_streams=%ld leftover_entities=%ld leftovers_seen:%s (our locked streams leave with the world)\n", closes,
                         InterlockedCompareExchange(&worldCloseStreams, 0, 0), InterlockedCompareExchange(&worldCloseFaults, 0, 0),
                         InterlockedCompareExchange(&exitReleases, 0, 0), InterlockedCompareExchange(&exitStreamsDropped, 0, 0),
                         InterlockedCompareExchange(&leftoverStreams, 0, 0), InterlockedCompareExchange(&leftoverEntities, 0, 0), leftoverLog);
        }
    }
    static LONG lastNull = -1;
    const LONG nullNow = InterlockedCompareExchange(&playerByRefNull, 0, 0);
    if (nullNow != lastNull) {
        lastNull = nullNow;
        std::fprintf(log, "necro player_by_ref null_answered=%ld passed=%ld ghost_branch_patched=%ld nameplate_flag_sets=%ld\n",
                     nullNow, InterlockedCompareExchange(&playerByRefPassed, 0, 0),
                     InterlockedCompareExchange(&ghostBranchPatched, 0, 0), InterlockedCompareExchange(&nameplateFlagSets, 0, 0));
    }
    static LONG lastCollision = -1;
    const LONG collisionNow = InterlockedCompareExchange(&collisionApplied, 0, 0) + InterlockedCompareExchange(&collisionMissing, 0, 0);
    if (collisionNow != lastCollision) {
        lastCollision = collisionNow;
        std::fprintf(log, "necro collision applied=%ld missing_cmc=%ld isaac_mode=%u\n",
                     InterlockedCompareExchange(&collisionApplied, 0, 0), InterlockedCompareExchange(&collisionMissing, 0, 0), isaacCollisionMode);
    }
    static LONG lastNrm = -1;
    const LONG nrmNow = InterlockedCompareExchange(&nrmBuilt, 0, 0) * 100 + InterlockedCompareExchange(&nrmRegistered, 0, 0) * 10 + InterlockedCompareExchange(&nrmRequests, 0, 0);
    if (nrmNow != lastNrm) {
        lastNrm = nrmNow;
        uint32_t current = 0, approved = 0;
        Read(base + kRespawnManager, &current, 4);
        if (current) Read(current + 0x30, &approved, 4);
        std::fprintf(log, "necro type_menu enabled=%d manager=%08X ours=%08X built=%ld registered=%ld register_tries=%u register_faults=%ld requests=%ld approved=%u models=%d/%d/%d/%d\n", typeMenu ? 1 : 0, current, respawnManager,
                     InterlockedCompareExchange(&nrmBuilt, 0, 0), InterlockedCompareExchange(&nrmRegistered, 0, 0), nrmRegisterTries,
                     InterlockedCompareExchange(&nrmRegisterFaults, 0, 0), InterlockedCompareExchange(&nrmRequests, 0, 0), approved,
                     extraModelStatus[0], extraModelStatus[1], extraModelStatus[2], extraModelStatus[3]);
    }
    static LONG lastDeletes = -1;
    const LONG deletesNow = InterlockedCompareExchange(&deleteEventCount, 0, 0);
    if (deletesNow != lastDeletes) {
        lastDeletes = deletesNow;
        std::fprintf(log, "necro delete_trace events=%ld", deletesNow);
        const LONG first = deletesNow > 16 ? deletesNow - 16 : 0;
        for (LONG i = first; i < deletesNow; ++i) { const DeleteEvent& e = deleteEvents[i & 15]; std::fprintf(log, " [%s %08X by %08X t=%lu]", e.what ? e.what : "?", e.object, e.caller, static_cast<unsigned long>(e.at)); }
        std::fprintf(log, "\n");
    }
    WriteDeleteGuard(log);
    static LONG lastGuarded = -1;
    const LONG guardedNow = InterlockedCompareExchange(&listenerGuarded, 0, 0) + InterlockedCompareExchange(&callbacksGuarded, 0, 0) * 1000 +
                            InterlockedCompareExchange(&payloadsGuarded, 0, 0) * 100000 + InterlockedCompareExchange(&registrationsKept, 0, 0) * 7 +
                            InterlockedCompareExchange(&keptNotices, 0, 0) * 131 + InterlockedCompareExchange(&staleReuses, 0, 0) * 3001;
    if (guardedNow != lastGuarded) {
        lastGuarded = guardedNow;
        std::fprintf(log, "necro listener_guard skipped=%ld dead_lua_states=%ld stream_callbacks_skipped=%ld freed_payloads_kept=%ld registrations_kept=%ld "
                          "kept_notices=%ld stale_cache_reuses=%ld",
                     InterlockedCompareExchange(&listenerGuarded, 0, 0), InterlockedCompareExchange(&deadLuaStates, 0, 0),
                     InterlockedCompareExchange(&callbacksGuarded, 0, 0), InterlockedCompareExchange(&payloadsGuarded, 0, 0),
                     InterlockedCompareExchange(&registrationsKept, 0, 0), InterlockedCompareExchange(&keptNotices, 0, 0),
                     InterlockedCompareExchange(&staleReuses, 0, 0));
        for (const auto& s : staleListeners) if (s.object) std::fprintf(log, " [%08X vt=%08X msg=%08X x%ld]", s.object, s.vtable, s.hash, InterlockedCompareExchange(const_cast<volatile LONG*>(&s.hits), 0, 0));
        std::fprintf(log, "\n");
    }
    static LONG lastTrace = -1;
    LONG traceNow = InterlockedCompareExchange(&spawnCalls, 0, 0) * 1000 + InterlockedCompareExchange(&confirmKeyUsed, 0, 0) * 100;
    for (int i = 0; i < 8; ++i) traceNow += InterlockedCompareExchange(&transitionTrue[i], 0, 0);
    for (int i = 0; i < 6; ++i) traceNow += InterlockedCompareExchange(&exitTransitionTrue[i], 0, 0) * 10;
    if (traceNow != lastTrace) {
        lastTrace = traceNow;
        char line[512]; size_t used = 0;
        used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, "necro respawn_trace spawns=%ld last=%08X key_confirms=%ld vent_requests=%ld granted=%ld refused=%ld transitions(id=calls/true):",
                                                  InterlockedCompareExchange(&spawnCalls, 0, 0), lastSpawnResult, InterlockedCompareExchange(&confirmKeyUsed, 0, 0),
                                                  InterlockedCompareExchange(&ventRequests, 0, 0), InterlockedCompareExchange(&ventGranted, 0, 0), InterlockedCompareExchange(&ventRefused, 0, 0)));
        for (int i = 0; i < 8 && used < sizeof(line); ++i)
            used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, " %d=%ld/%ld", i + 5, InterlockedCompareExchange(&transitionCalls[i], 0, 0), InterlockedCompareExchange(&transitionTrue[i], 0, 0)));
        used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, " msgs(3..21):"));
        for (int i = 0; i < 19 && used < sizeof(line); ++i)
            used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, " %ld", InterlockedCompareExchange(&stateMessages[i], 0, 0)));
        std::fprintf(log, "%s\n", line);
        used = 0;
        used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, "necro exit_sm vent=%08X type=%u transitions(B..10):", exitSmVent, exitSmType));
        for (int i = 0; i < 6 && used < sizeof(line); ++i)
            used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, " %ld", InterlockedCompareExchange(&exitTransitionTrue[i], 0, 0)));
        used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, " msgs(12..1B):"));
        for (int i = 0; i < 10 && used < sizeof(line); ++i)
            used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, " %ld", InterlockedCompareExchange(&exitMessages[i], 0, 0)));
        std::fprintf(log, "%s\n", line);
    }
    static LONG lastMpUi = -1;
    const LONG mpUiNow = InterlockedCompareExchange(&mpUiBuilt, 0, 0) * 2 + InterlockedCompareExchange(&mpUiFaults, 0, 0);
    if (mpUiNow != lastMpUi) {
        lastMpUi = mpUiNow;
        std::fprintf(log, "necro mp_ui enabled=%d built=%ld faults=%ld block=%08X\n", mpUi ? 1 : 0, InterlockedCompareExchange(&mpUiBuilt, 0, 0),
                     InterlockedCompareExchange(&mpUiFaults, 0, 0), mpUiBlock);
    }
    static LONG lastMove = -1;
    const LONG moveNow = InterlockedCompareExchange(&moveProxySkipped, 0, 0) + InterlockedCompareExchange(&moveProxyPassed, 0, 0)
                       + InterlockedCompareExchange(&moveProxySynthesized, 0, 0) + InterlockedCompareExchange(&moveProxyParked, 0, 0);
    if (moveNow != lastMove) {
        lastMove = moveNow;
        std::fprintf(log, "necro ghost proxy=%08X corpse=%08X move_passed=%ld no_vent=%ld synthesized=%ld parked=%ld zero_room_views=%ld pace_from_proxy=%ld "
                          "spawn_type_fallbacks=%ld spawns_refused_no_template=%ld menu_type_fallbacks=%ld\n", ghostProxy, deadNecro,
                     InterlockedCompareExchange(&moveProxyPassed, 0, 0), InterlockedCompareExchange(&moveProxySkipped, 0, 0),
                     InterlockedCompareExchange(&moveProxySynthesized, 0, 0), InterlockedCompareExchange(&moveProxyParked, 0, 0),
                     InterlockedCompareExchange(&moveProxyZeroViews, 0, 0), InterlockedCompareExchange(&paceFromProxy, 0, 0),
                     InterlockedCompareExchange(&spawnTypeFallbacks, 0, 0), InterlockedCompareExchange(&spawnsRefusedNoTemplate, 0, 0),
                     InterlockedCompareExchange(&menuTypeFallbacks, 0, 0));
        std::fprintf(log, "necro door_rooms loaded=%ld\n", InterlockedCompareExchange(&doorSetsLoaded, 0, 0));
        std::fprintf(log, "necro vent_cameras room_views_on_normal=%ld normals_flipped=%ld spots_clamped=%ld no_normal=%ld\n",
                     InterlockedCompareExchange(&roomViewsOnNormal, 0, 0), InterlockedCompareExchange(&cameraNormalsFlipped, 0, 0),
                     InterlockedCompareExchange(&cameraSpotsClamped, 0, 0), InterlockedCompareExchange(&cameraNoNormal, 0, 0));
        std::fprintf(log, "necro vent_rooms human_range=%.0f probes=%ld empty=%ld too_far=%ld refused=%ld front_hidden=%ld hits_hidden=%ld hits_unknown_stream=%ld\n", ventHumanRange,
                     InterlockedCompareExchange(&ventProbesRun, 0, 0), InterlockedCompareExchange(&ventProbesEmpty, 0, 0), InterlockedCompareExchange(&ventsTooFar, 0, 0),
                     InterlockedCompareExchange(&ventsRefusedHidden, 0, 0), InterlockedCompareExchange(&ventFrontHidden, 0, 0), InterlockedCompareExchange(&ventHitsHidden, 0, 0),
                     InterlockedCompareExchange(&ventHitsUnknown, 0, 0));
        std::fprintf(log, "necro ghost_human_view range=%.0f other_room=%ld far=%ld shown=%ld expired=%ld same_room_preferred=%ld | vent rooms same=%ld other=%ld "
                          "unknown=%ld, human room unknown=%ld\n",
                     humanViewRange, InterlockedCompareExchange(&humanViewsOtherRoom, 0, 0), InterlockedCompareExchange(&humanViewsFar, 0, 0),
                     InterlockedCompareExchange(&viewsAtHuman, 0, 0), InterlockedCompareExchange(&viewsAtHumanExpired, 0, 0),
                     InterlockedCompareExchange(&sameRoomPreferred, 0, 0), InterlockedCompareExchange(&ventRoomsSame, 0, 0),
                     InterlockedCompareExchange(&ventRoomsOther, 0, 0), InterlockedCompareExchange(&ventRoomsUnknown, 0, 0),
                     InterlockedCompareExchange(&humanRoomsUnknown, 0, 0));
        std::fprintf(log, "necro menu_streams wanted=%d asked=%ld all_four_asked=%ld no_slot=%ld models=%d/%d/%d/%d (pack/lurker/spitter/puker, 3 = loaded)\n", menuWanted,
                     InterlockedCompareExchange(&menuStreamsAsked, 0, 0), InterlockedCompareExchange(&extraStreamsAsked, 0, 0),
                     InterlockedCompareExchange(&extraStreamsNoSlot, 0, 0) + InterlockedCompareExchange(&menuStreamsNoSlot, 0, 0), extraModelStatus[0],
                     extraModelStatus[1], extraModelStatus[2], extraModelStatus[3]);
    }
    static int lastPool = -2; static uint32_t lastNecro = 0xFFFFFFFFu, lastSettings = 0xFFFFFFFFu; static LONG lastBlocked = -1;
    const LONG blockedNow = InterlockedCompareExchange(&blockedSpawns, 0, 0);
    static int lastModel = -2;
    static int lastLeaperModel = -9, lastLeaperRecord = -9;
    static LONG lastTentacleType = -2; static bool lastTentaclesActive = false;
    const LONG tentacleTypeNow = InterlockedCompareExchange(&tentacleOwnerType, 0, 0);
    const bool tentaclesActive = InterlockedCompareExchange(&tentaclesSkipped, 0, 0) > 0;
    if (poolStatus != lastPool || modelStatus != lastModel || necroEntity != lastNecro || settings != lastSettings || blockedNow != lastBlocked ||
        leaperModelStatus != lastLeaperModel || leaperRecordStatus != lastLeaperRecord ||
        tentacleTypeNow != lastTentacleType || tentaclesActive != lastTentaclesActive) {
        lastTentacleType = tentacleTypeNow; lastTentaclesActive = tentaclesActive;
        lastPool = poolStatus; lastModel = modelStatus; lastNecro = necroEntity; lastSettings = settings; lastBlocked = blockedNow;
        lastLeaperModel = leaperModelStatus; lastLeaperRecord = leaperRecordStatus;
        // leaper_stream and leaper_record belong in this recurring line, not in
        // a Note: the note ring drops its oldest entry when full (notes_dropped),
        // so a missing "loaded" line proves nothing. These two cannot be lost.
        uint32_t reportManager = 0, reportTocs = 0;
        if (Read(base + kStreamManager, &reportManager, 4) && reportManager) Read(reportManager + 0x130, &reportTocs, 4);
        std::fprintf(log, "necro pool_mode=%d demanded=%d ready_peers=%08X demands=%ld pool_sent=%ld pool_received=%ld wait_timeouts=%ld poll_faults=%ld vent_faults=%ld\n",
                     poolMode, poolDemanded ? 1 : 0, readyPeers, InterlockedCompareExchange(&poolDemands, 0, 0),
                     InterlockedCompareExchange(&poolSent, 0, 0), InterlockedCompareExchange(&poolReceived, 0, 0),
                     InterlockedCompareExchange(&poolWaitTimeouts, 0, 0), InterlockedCompareExchange(&pollFaults, 0, 0),
                     InterlockedCompareExchange(&ventFaults, 0, 0));
        std::fprintf(log, "necro pool=%d model=%d leaper_stream=%d leaper_record=%d tentacles_skipped=%ld spitter_aim_skipped=%ld tentacles_passed=%ld tentacle_owner_model=%08lX tentacle_owner_type=%ld spawn_type_repairs=%ld restart_resets=%ld fall_rescues=%ld grab_hits_blocked=%ld tocs=%u/32 handle=%08X settings=%08X necro=%08X parked=%08X proxy=%08X transforms=%ld reverts=%ld blocked=%ld error=%ld\n",
                     poolStatus, modelStatus, leaperModelStatus, leaperRecordStatus, InterlockedCompareExchange(&tentaclesSkipped, 0, 0), InterlockedCompareExchange(&spitterAimSkipped, 0, 0), InterlockedCompareExchange(&tentaclesPassed, 0, 0), InterlockedCompareExchange(&tentacleOwnerModel, 0, 0), InterlockedCompareExchange(&tentacleOwnerType, 0, 0), InterlockedCompareExchange(&spawnTypeRepairs, 0, 0), InterlockedCompareExchange(&restartResets, 0, 0), InterlockedCompareExchange(&fallRescues, 0, 0), InterlockedCompareExchange(&grabHitsBlocked, 0, 0), reportTocs, poolHandle, settings, necroEntity, parkedPlayer, necroProxy,
                     InterlockedCompareExchange(&transforms, 0, 0), InterlockedCompareExchange(&reverts, 0, 0),
                     blockedNow, InterlockedCompareExchange(&lastError, 0, 0));
    }
}
}  // namespace necro
