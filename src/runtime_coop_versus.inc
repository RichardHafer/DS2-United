// Dead Space 2 United by Richard Hafer.
// Versus matches on the game's own round systems ([Versus] and [VersusPoints] in
// DS2United.ini; spec: analysis/mp_infrastructure/spec_ingame.md). The roster, the
// player refs and the team seams live in runtime_coop_mpnative.inc; this file
// runs the match on top of them, each part behind its own switch:
//   - MpBits: the level manager's multiplayer (0x2) and host (0x4) bits while a
//     round runs, cleared before a quick restart rebuilds the level, and the
//     20 s "no online game" timer kept at zero;
//   - Scoring: a native ScoreManager with an entry per roster player on every
//     machine; the host applies every scoring event and the others replay it;
//   - KillerCard: the killer relay, named from the roster;
//   - Rounds: the host's end of round reaches every machine, the sides swap
//     after round 1 through a quick restart, round 2 ends the match;
//   - NativeRespawn: the spawn point selector on every machine, the host queues
//     the dead and hands out the points;
//   - TeamGroups: refs and team groups for every Player and NecroPlayer.
namespace versus {
namespace vs = coop_versus;
namespace mp = coop_mpnative;
struct Config { bool mpBits = true, scoring = true, killerCard = true, rounds = true, nativeRespawn = true, teamGroups = true, inventory = false, music = true, nativeAudio = true, combatText = true; DWORD swapMs = 12000, resultsMs = 20000; int scoreKey = VK_TAB, roundWin = 1000, bothRounds = 2000; };
Config cfg;
bool installed = false;

// RVAs (image base 0x400000)
constexpr uint32_t kLevelManager = 0x1C1C0A4, kGmlHolder = 0x1C1C09C, kNoGameTimer = 0x1C2155C, kSimTime = 0x1C4D3C0;
constexpr uint32_t kScoreManager = 0x1C8BEA8, kRelay = 0x1C21398, kSequencer = 0x1C8BEB0, kGroupManager = 0x1C50E18, kCombatText = 0x1CAD0BC;
constexpr uint32_t kScoringIndex = 0x1CAB75A, kRespawnManager = 0x1C21628, kSelector = 0x1C21620, kMpUiBlock = 0x1C89C40, kKillerTable = 0x1B603A8;
constexpr uint32_t kLevelSettings = 0x1C21624;
constexpr uint32_t kInflictorTable = 0x1B60400, kSuicideText = 0x17318D4, kEmptyText = 0x16A6AD4;   // {mask, text} x15; "$ui_kill_text_suicide"; ""
constexpr uint32_t kResetForRestart = 0x153F0, kCheckpointLoadComplete = 0x2D5D0, kEntryEvent = 0xCD9F00, kVictimInfo = 0xC86670, kAttackerInfo = 0xC865A0;
constexpr uint32_t kTuning = 0x2CAA0, kCombatTextFn = 0xCCD8D0, kSetKillerInfo = 0x668BC0, kActivate = 0x7936C0, kRequestPoint = 0x33F20, kIssuePoint = 0x331F0;
constexpr uint32_t kScoreListenerOpen = 0xCBD320, kEntryCtor = 0xC868A0, kFindEntry = 0xC870D0, kTeamChange = 0xC87060, kTranslate = 0xC7A4D0;
constexpr uint32_t kRegisterCombatText = 0xCC5990, kHideScoring = 0x64E5A0, kHideEndRound = 0x6381F0, kShutdownCamera = 0x793650, kRelayCtor = 0x64D130;
constexpr uint32_t kStringAssign = 0x33AAE0, kAddToGroup = 0x3A8030, kRemoveFromGroup = 0x3A8130, kGetComponent = 0x3CF080, kOperatorNew = 0x3CB0;
constexpr uint32_t kSelectorCtor = 0x33630, kResetInitialPoints = 0x31D80, kQueueRespawn = 0x33C60, kSafePtrAssign = 0x31400, kPointResponse = 0x32C00;
constexpr uint32_t kScoreForRound = 0x652D10;
constexpr uint32_t kLifetimeAdd = 0xBF22A0;   // EnemyLifetimeManager::AddEntity
constexpr uint32_t kDropListSpawn = 0xC970D0, kFindIndexForGun = 0x9A8930;   // ItemDropListComponent::HandlePostSpawnInit, MultiAmmo::FindIndexForGun
constexpr uint32_t kCheckStartTest = 0xCBF10B;
constexpr uint32_t kShowScoring = 0x64E4F0, kCanShowScoring = 0x66A630, kIsShownScoring = 0x6381C0;
// MP10PauseMenu (the online pause menu): OnInput_Select slot and its quit popup's result slot
constexpr uint32_t kPauseSelectSlot = 0x17377F0, kPauseSelect = 0x697120, kPauseQuitSlot = 0x172EBEC, kPauseQuit = 0x6666E0, kHidePause = 0x64E700;   // PlayerUIMainSM::CheckStart 0x010BF100: the MP bit test
constexpr uint32_t kPlayerRef = 0x1710, kNecroRef = 0x538, kPlayerFlags = 0x3D8, kFlagMpBody = 0x2000;
constexpr uint32_t kEntrySize = 0xF8, kRelaySize = 0x50, kSelectorSize = 0x90;

struct Sig { const char* name; uint32_t rva; const char* hex; };
const Sig kSigs[] = {
    {"GameServices_ResetForQuickLevelRestart", kResetForRestart, "8B 0D 34 C1 01 02 E9 97 10 FF FF"},
    {"LM::CheckpointLoadComplete", kCheckpointLoadComplete, "56 8B F1 8A 46 1C D0 E8 A8 01 74 7E 8B 0D 18 16"},
    {"ScoreBoardEntryImpl::LogScoringEvent", kEntryEvent, "53 8B D9 E8 28 72 70 FF 84 C0 74 4E 8B 44 24 10"},
    {"GetVictimInfo", kVictimInfo, "8B 44 24 14 8B 4C 24 10 53 33 DB 55 8B 6C 24 14"},
    {"GetAttackerInfo", kAttackerInfo, "8B 4C 24 04 53 56 33 DB 57 8B 7C 24 14 89 1F 89"},
    {"LM::GetMPTuningValue", kTuning, "56 8B 74 24 08 33 D2 8B C6 F7 B1 80 00 00 00 8B"},
    {"ScoreBoardEntryImpl::HandleCombatText", kCombatTextFn, "83 EC 08 56 8B F1 E8 55 38 71 FF 84 C0 0F 84 FF"},
    {"UIKilledByInfo::SetKillerInfo", kSetKillerInfo, "83 EC 08 53 55 56 8D 44 24 0C 57 50 8B F1 C7 44"},
    {"MPEndOfGameSequencer::Activate", kActivate, "83 EC 44 53 55 56 8B F1 F6 46 6C 01 57 0F 85 EE"},
    {"MPSpawnPointSelector::RequestRespawnPoint", kRequestPoint, "83 EC 10 56 8B F1 C7 44 24 04 00 00 00 00 F6 46"},
    {"MPSpawnPointSelector::IssueSpawnPoint", kIssuePoint, "51 53 56 8D 44 24 08 50 8B F1 32 DB E8 7F 24 75"},
    {"ScoreListener_Open", kScoreListenerOpen, "83 3D A8 BE 08 02 00 75 28 6A 4C E8 80 69 34 FF"},
    {"ScoreBoardEntryImpl ctor", kEntryCtor, "56 8B F1 E8 F8 AE 75 FF 8B 44 24 08 C7 46 10 E0"},
    {"ScoreManager::FindPlayerEntry", kFindEntry, "56 57 8D 79 04 8B 4F 40 8B F7 8D 14 8F 33 C0 3B"},
    {"ScoreManager::PlayerTeamChange", kTeamChange, "8B 44 24 04 3B 41 44 73 3B 8B 44 81 04 85 C0 74"},
    {"TranslateScoreEvent", kTranslate, "0F B6 44 24 04 8B 4C 24 08 8D 04 40 8D 14 C1 8B"},
    {"EnemyLifetimeManager::AddEntity", kLifetimeAdd, "8B 44 24 04 83 EC 28 53 8D 04 40 56 8D 74 C1 10"},
    {"ItemDropListComponent::HandlePostSpawnInit list spawn", kDropListSpawn, "56 8B 35 A0 C0 01 02 6A 00 50"},
    {"MultiAmmo::FindIndexForGun", kFindIndexForGun, "8B 54 24 04 33 C0 85 D2 0F 84 E3 00 00 00 8B 49 0C 56 57 8B BA 04 04 00 00"},
    {"UIMPCombatTextManager::RegisterCombatTextEvent", kRegisterCombatText, "83 EC 08 55 8B 6C 24 10 8D 45 FF 56 8B F1 83 F8"},
    {"HideScreen_Multiplayer_Scoring", kHideScoring, "A1 40 9C 08 02 33 C9 83 EC 0C 3B C1 0F 84 88 00"},
    {"HideScreen_Multiplayer_EndRound", kHideEndRound, "A1 40 9C 08 02 85 C0 74 2C 80 B8 A4 F2 00 00 00"},
    {"MPEndOfGameSequencer::ShutdownCamera", kShutdownCamera, "8B 41 7C 56 8D 71 7C 85 C0 74 5B 83 C0 F0 74 56"},
    {"UIKilledByInfo ctor", kRelayCtor, "33 C0 56 8B F1 89 35 98 13 02 02 C7 06 50 FF B2"},
    {"String::assign", kStringAssign, "53 8B 5C 24 08 56 8B F1 85 DB 74 69 80 3B 00 74"},
    {"GroupManager::AddToGroupByGroupID", kAddToGroup, "8B 54 24 08 56 83 CE FF 57 8B F9 83 FA FF 74 16"},
    {"GroupManager::RemoveFromGroupByGroupID", kRemoveFromGroup, "57 8B 7C 24 0C 83 FF FF 74 34 8B 51 04 56 83 CE"},
    {"Base::GetComponent", kGetComponent, "8B 51 48 33 C0 85 D2 74 0F 8B 41 44 8B 4C 24 04"},
    {"MPSpawnPointSelector ctor", kSelectorCtor, "53 55 56 8B F1 57 8D 7E 04 C7 06 78 C6 AA 01 8B"},
    {"MPSpawnPointSelector::ResetInitialSpawnPoints", kResetInitialPoints, "33 C9 39 0D 20 19 02 02 76 1D 8D 9B 00 00 00 00"},
    {"MPSpawnPointSelector::QueuePlayerForRespawn", kQueueRespawn, "55 8B EC 83 E4 F0 A1 24 16 02 02 8B 15 C0 D3 04"},
    {"SafePtr assign", kSafePtrAssign, "8B 44 24 04 56 57 8B F1 85 C0 74 05 8D 78 10 EB"},
    {"RPC_SpawnPointResponse_remote", kPointResponse, "51 55 8B E9 F6 45 24 01 89 6C 24 04 0F 84 C3 00"},
    {"GetPlayerScoreForRound", kScoreForRound, "8B 0D A8 BE 08 02 53 8B 1D A4 C0 01 02 56 57 33"},
    {"PlayerUIMainSM::CheckStart MP test", kCheckStartTest, "32 C0 F6 C2 01 0F 85 E3 00 00 00"},
    {"ShowScreen_Multiplayer_Scoring", kShowScoring, "A1 40 9C 08 02 85 C0 0F 84 8F 00 00 00 80 B8 A4"},
    {"CanShowScreen_Multiplayer_Scoring", kCanShowScoring, "A1 40 9C 08 02 56 85 C0 74 65 8D B0 18 53 00 00"},
    {"IsShown_Multiplayer_Scoring", kIsShownScoring, "A1 40 9C 08 02 85 C0 74 18 80 B8 64 53 00 00 00"},
    {"MP10PauseMenu::OnInput_Select", kPauseSelect, "8B 54 24 04 32 C0 83 FA 04 77 4A FF 24 95 78 71 A9 00"},
    {"MP10PauseMenu quit popup result", kPauseQuit, "8B 44 24 04 83 78 08 00 75 10 E8 11 80 FE FF 8B 0D CC C0 01 02"},
};
bool sigOk[std::size(kSigs)]{};
bool SigOk(uint32_t rva) {
    for (size_t i = 0; i < std::size(kSigs); ++i)
        if (kSigs[i].rva == rva) return sigOk[i];
    return false;
}

volatile LONG faults = 0;
unsigned lines = 0;
void Line(const char* format, ...) {
    if (++lines > 2000) return;
    char buffer[320];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    vlog::WriteText("match", buffer);
}
uint32_t Ptr(uint32_t address) {
    uint32_t v = 0;
    Read(address, &v, 4);
    return v;
}
uint8_t Byte(uint32_t address) {
    uint8_t v = 0;
    Read(address, &v, 1);
    return v;
}
void* P(uint32_t address) { return reinterpret_cast<void*>(static_cast<uintptr_t>(address)); }
bool OnMap() { return InterlockedCompareExchange(&mpnative::active, 0, 0) && mpmap::visit.phase == coop_mpmap::Phase::Arrived; }
bool Hosting() { return !network || IsHost(); }
uint64_t OwnRef() { return mp::RefFor(machineIndex); }
uint8_t TeamOf(unsigned machine) {
    mp::Entry e{};
    if (!mpnative::Lookup(mp::RefFor(machine), e)) return mp::kNoTeam;
    return mp::EffectiveTeam(e.team, mpnative::roundNo);
}
const char* NameOf(uint64_t ref, char (&out)[24]) {
    mp::Entry e{};
    if (!mpnative::Lookup(ref, e) || !e.name[0]) return nullptr;
    std::memcpy(out, e.name, sizeof(out));
    out[sizeof(out) - 1] = 0;
    return out;
}

// --- traffic ----------------------------------------------------------------------------------
struct Outgoing { vs::Packet p; int left = 0; DWORD next = 0; };
Outgoing outbox[24];
uint32_t seq = 0;
vs::SeqWindow seen[16];
uint32_t seenToken[16]{};   // a sender that restarted its game starts its sequence over
volatile LONG sent = 0, received = 0, refused = 0, duplicates = 0, kindsIn[9]{};
// Each packet goes out three times, 150 ms apart; the receivers take it once.
void Queue(vs::Packet p, DWORD now) {
    p.magic = vs::kMagic;
    p.version = vs::kVersion;
    p.machine = static_cast<uint8_t>(machineIndex);
    p.token = peerReadiness.token;
    p.seq = ++seq;
    p.round = mpnative::roundNo;
    for (auto& o : outbox)
        if (o.left <= 0) { o.p = p; o.left = 3; o.next = now; return; }
    InterlockedIncrement(&refused);   // full: dropped
}
void Flush(DWORD now) {
    if (!network) { for (auto& o : outbox) o.left = 0; return; }
    for (auto& o : outbox) {
        if (o.left <= 0 || static_cast<LONG>(now - o.next) < 0) continue;
        if (SendToPeers(&o.p, sizeof(o.p)) > 0) InterlockedIncrement(&sent);
        --o.left;
        o.next = now + 150;
    }
}
vs::Packet Make(uint8_t kind, uint64_t ref = 0, uint64_t other = 0, int32_t a = 0, int32_t b = 0) {
    vs::Packet p;
    p.kind = kind;
    p.ref = ref;
    p.other = other;
    p.a = a;
    p.b = b;
    return p;
}

// Received packets wait here for the game thread.
SRWLOCK inboxLock = SRWLOCK_INIT;
vs::Packet inbox[32];
unsigned inboxCount = 0;
bool OnBytes(const void* data, int size) {
    vs::Packet p;
    if (!vs::Parse(data, size, p)) return false;
    if (p.version != vs::kVersion || !KnownPeerToken(p.token) || p.machine >= 16 || p.kind == 0 || p.kind > 8) { InterlockedIncrement(&refused); return true; }
    if (vs::FromHostOnly(p.kind) ? p.machine != 0 || IsHost() : !IsHost()) return true;   // not for us
    AcquireSRWLockExclusive(&inboxLock);
    if (seenToken[p.machine] != p.token) {
        seenToken[p.machine] = p.token;
        seen[p.machine] = vs::SeqWindow{};
    }
    const bool fresh = seen[p.machine].Fresh(p.seq);
    if (fresh && inboxCount < std::size(inbox)) inbox[inboxCount++] = p;
    ReleaseSRWLockExclusive(&inboxLock);
    InterlockedIncrement(fresh ? &received : &duplicates);
    if (fresh) InterlockedIncrement(&kindsIn[p.kind]);
    return true;
}

// --- MP bits ----------------------------------------------------------------------------------
uint32_t bitsLm = 0;
bool bitsOurs = false;
volatile LONG bitsSet = 0, bitsCleared = 0, lateClears = 0, timerZeroes = 0, bodyFlags = 0;
safetyhook::InlineHook resetHook, loadCompleteHook;
bool Restarting(uint32_t* loopOut = nullptr) {
    const uint32_t loop = Ptr(Ptr(base + kGmlHolder) + 4);
    if (loopOut) *loopOut = loop;
    return loop && (Byte(loop + 0x208) || Ptr(loop + 0x210) || Byte(loop + 0x209));
}
void ClearBits(uint32_t lm, const char* why) {
    uint16_t flags = 0;
    if (!lm || !Read(lm + 0x1C, &flags, 2)) return;
    const uint16_t cleared = static_cast<uint16_t>(flags & ~0x16);
    if (cleared != flags) WriteBytes(lm + 0x1C, &cleared, 2);
    if (bitsOurs) InterlockedIncrement(&bitsCleared);
    Line("MP bits cleared (%s): level manager %08X flags %04X -> %04X", why, lm, flags, cleared);
    bitsOurs = false;
}
// Before entities are rebuilt in a quick restart (state 4): the rebuild runs
// like round 1, and CheckpointLoadComplete never sees the bit (state 0xD would
// spawn a second Isaac through MPSpawnInitialPlayer).
void __cdecl OnResetForRestart() {
    if (bitsOurs) ClearBits(Ptr(base + kLevelManager), "quick restart");
    resetHook.ccall<void>();
}
void __fastcall OnCheckpointLoadComplete(void* lm, void*) {
    uint16_t flags = 0;
    if ((bitsOurs || InterlockedCompareExchange(&mpnative::active, 0, 0)) && Read(reinterpret_cast<uintptr_t>(lm) + 0x1C, &flags, 2) && (flags & 0x2)) {
        InterlockedIncrement(&lateClears);
        ClearBits(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lm)), "late, at CheckpointLoadComplete");
    }
    loadCompleteHook.thiscall<void>(lm);
}

// --- respawn: the selector ---------------------------------------------------------------------
volatile LONG selectorsBuilt = 0, selectorsDeleted = 0, asksQueued = 0, asksSent = 0, pointsLocal = 0, pointsSent = 0, pointsIn = 0, delaysIn = 0;
__declspec(noinline) uint32_t BuildSelector() {
    __try {
        void* block = reinterpret_cast<void*(__cdecl*)(uint32_t)>(base + kOperatorNew)(kSelectorSize);
        if (!block) return 0;
        std::memset(block, 0, kSelectorSize);
        reinterpret_cast<void(__thiscall*)(void*)>(base + kSelectorCtor)(block);
        if (Hosting()) reinterpret_cast<void(__thiscall*)(void*)>(base + kResetInitialPoints)(block);
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return 0;
    }
}
void EnsureSelector() {
    if (!cfg.nativeRespawn || Ptr(base + kSelector)) return;
    const uint32_t sel = BuildSelector();
    if (sel) InterlockedIncrement(&selectorsBuilt);
    Line("spawn point selector %s at %08X (%s; singleton now %08X)", sel ? "built" : "NOT built", sel, Hosting() ? "host: queues and ticks" : "client: mailbox",
         Ptr(base + kSelector));
}
__declspec(noinline) void DeleteSelector() {
    const uint32_t sel = Ptr(base + kSelector);
    if (!sel) return;
    __try {
        const uint32_t dtor = Ptr(Ptr(sel));
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(dtor)(P(sel), 1);
        InterlockedIncrement(&selectorsDeleted);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
    Line("spawn point selector %08X deleted for the next round (singleton now %08X)", sel, Ptr(base + kSelector));
}

// The local Isaac built before the bits were set lacks the body bit the MP
// branch of ProcessDeath needs (the Player ctor sets it with the bit on).
void MarkBody() {
    const uint32_t player = LocalCampaignPlayer();
    uint32_t flags = 0;
    if (!player || IsBotEntity(player) || !Read(player + kPlayerFlags, &flags, 4) || (flags & kFlagMpBody)) return;
    if (Ptr(player) != base + placement::kPlayerPrimaryVtableRva) return;
    flags |= kFlagMpBody;
    if (WriteBytes(player + kPlayerFlags, &flags, 4)) InterlockedIncrement(&bodyFlags);
}

void PollBits(DWORD now) {
    (void)now;
    const uint32_t lm = Ptr(base + kLevelManager);
    if (bitsOurs && lm != bitsLm) {
        Line("MP bits: the level manager changed (%08X -> %08X); the new one starts without them", bitsLm, lm);
        bitsOurs = false;
    }
    if (!lm || !cfg.mpBits) return;
    const bool want = OnMap();
    uint16_t flags = 0;
    Read(lm + 0x1C, &flags, 2);
    const uint32_t state = Ptr(lm + 0x18);
    uint32_t loop = 0;
    const bool restarting = Restarting(&loop);
    // without MPLevelSettings the respawn queue (and the round clock) has nothing to read
    if (want && !bitsOurs && state == 0x10 && !restarting && Ptr(base + kLevelSettings)) {
        if (flags & 0x2) {
            static bool told = false;
            if (!told) { told = true; Line("MP bits: the level manager already has the multiplayer bit (%04X); left alone", flags); }
            return;
        }
        const uint16_t set = static_cast<uint16_t>(flags | 0x2 | (Hosting() ? 0x4 : 0));
        if (!WriteBytes(lm + 0x1C, &set, 2)) return;
        bitsOurs = true;
        bitsLm = lm;
        InterlockedIncrement(&bitsSet);
        Line("MP bits set: level manager %08X flags %04X -> %04X (%s), game loop %08X", lm, flags, set, Hosting() ? "host" : "client", loop);
        EnsureSelector();
    }
    if (!bitsOurs) return;
    if (!want && state == 0x10 && !restarting) { ClearBits(lm, "the Versus visit is over"); return; }
    const uint32_t zero = 0;
    if (Ptr(base + kNoGameTimer)) { WriteBytes(base + kNoGameTimer, &zero, 4); InterlockedIncrement(&timerZeroes); }
    if (cfg.nativeRespawn) MarkBody();
}

// The MP bit closes the RIG inventory (CheckStart returns at once); Inventory keeps it.
safetyhook::MidHook checkStartHook;
volatile LONG inventoryOpens = 0;
void OnCheckStart(safetyhook::Context& ctx) {
    if (!installed || !cfg.inventory || !bitsOurs || !(ctx.edx & 1)) return;
    ctx.edx &= ~1u;
    InterlockedIncrement(&inventoryOpens);
}

// For OnProcessDeath and the respawn module: the native MP death chain owns this death.
// The death state has taken the body out of slot 0 by the time ProcessDeath runs
// (test 11.10. 22:05: slot0 before=00000000), so ours is the local, non-ghost Player.
volatile LONG foreignDeaths = 0;
bool NativeDeath(uint32_t player) {
    if (!installed || !cfg.nativeRespawn || !bitsOurs || !player || IsBotEntity(player)) return false;
    if (Ptr(player) != base + placement::kPlayerPrimaryVtableRva || IsGhostPlayer(player)) return false;
    const uint32_t local = LocalCampaignPlayer();
    if (local && local != player) return false;   // another body holds slot 0
    // With slot 0 empty the body must be ours by name: 12.10. 00:59 the bots the
    // bot module had let go died, each was taken for the host and replaced him.
    if (!local && player != mpmap::ourPlayer) { InterlockedIncrement(&foreignDeaths); return false; }
    uint32_t flags = 0;
    return Ptr(base + kSelector) && Read(player + kPlayerFlags, &flags, 4) && (flags & kFlagMpBody);
}
volatile LONG nativeDeaths = 0, nativeRespawns = 0;
uint32_t deadBody = 0;
DWORD deadAt = 0;
void NoteDeath(uint32_t player) {
    InterlockedIncrement(&nativeDeaths);
    deadBody = player;
    deadAt = GetTickCount();
    const uint32_t sel = Ptr(base + kSelector);
    Line("death of Isaac %08X goes the native MP way (selector %08X flags %02X, respawn at %u, sim now %u)", player, sel, Byte(sel + 0x24), Ptr(sel + 0x28),
         Ptr(base + kSimTime));
}
// After Player::RespawnPlayer built the new body on the native point: what the
// respawn module's own path adds (the session adopts it, the map's loadout, a suit).
void AfterRespawn(uint32_t oldPlayer, uint32_t newPlayer) {
    InterlockedIncrement(&nativeRespawns);
    const bool adopted = newPlayer && localSettle.Adopt(newPlayer);
    if (newPlayer) {
        mpmap::GiveLoadout(newPlayer);
        if (mpmap::enableModules) mpmap::EnableModules(newPlayer);
        // The setup's suit streams in after GiveLoadout: a second change on top kept the
        // body hidden for good (12.10., the client after a respawn).
        uint32_t body[3]{};
        if (!mpmap::suitFromLoadout && !coop_uniform::BotBody(newPlayer, body) && mpmap::dressIfBare) coop_uniform::BotDress(newPlayer, mpmap::dressSuit);
        if (mpmap::ourPlayer == oldPlayer) mpmap::ourPlayer = newPlayer;
    }
    float pos[3]{};
    Read(newPlayer + mpmap::kPlayerPos, pos, sizeof(pos));
    Line("native respawn: Isaac %08X -> %08X at (%.1f %.1f %.1f), adopted %d", oldPlayer, newPlayer, pos[0], pos[1], pos[2], adopted ? 1 : 0);
}

safetyhook::InlineHook requestHook, issueHook;
DWORD askedAt = 0;   // a client's last ask, until the host's point arrives
volatile LONG askTimeouts = 0, foreignAsks = 0;
// The selector serves this machine's Isaac, but a bot's MP body calls it as well
// when it dies (12.10.: the host was queued while alive and stood on the respawn
// screen). Our Isaac still standing with health left means the ask is not ours.
bool OwnIsaacAlive() {
    const uint32_t me = LocalCampaignPlayer();
    if (!me || IsBotEntity(me)) return false;
    const uint32_t health = hits::FindHealthComponent(me);
    float current = 0.0f;
    return health && Read(health + hits::kHealthCurrent, &current, 4) && current > 0.0f;
}
using QueueFn = uint32_t(__thiscall*)(void*, const uint64_t*, const float*);
// RequestRespawnPoint: the host queues itself, a client asks the host (offline
// the native code finds no OnlinePlayer and never queues anybody).
void __fastcall OnRequestPoint(void* self, void*, const float* pos) {
    if (!installed || !cfg.nativeRespawn || !bitsOurs) { requestHook.thiscall<void>(self, pos); return; }
    const uint32_t sel = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    uint8_t flags = Byte(sel + 0x24);
    if (flags & 1) return;
    if (OwnIsaacAlive()) {
        alignas(16) float from[4]{};
        if (pos) Read(reinterpret_cast<uintptr_t>(pos), from, sizeof(from));
        if (InterlockedIncrement(&foreignAsks) <= 30)
            Line("respawn: an ask from (%.1f %.1f %.1f) while our Isaac lives (a bot's MP body died): not queued", from[0], from[1], from[2]);
        return;
    }
    // cleared before the queue call and re-read after it: the queue sets bit 0x8
    // (tick linked), which the selector's destructor needs to unlink the tick
    flags = static_cast<uint8_t>(flags & ~6);
    WriteBytes(sel + 0x24, &flags, 1);
    const uint64_t ref = OwnRef();
    alignas(16) float at[4]{};   // QueuePlayerForRespawn reads it with movaps
    if (pos) Read(reinterpret_cast<uintptr_t>(pos), at, sizeof(at));
    if (Hosting()) {
        uint32_t when = 0;
        __try {
            when = reinterpret_cast<QueueFn>(base + kQueueRespawn)(self, &ref, at);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&faults);
        }
        WriteBytes(sel + 0x28, &when, 4);
        flags = static_cast<uint8_t>(Byte(sel + 0x24) | 4);
        InterlockedIncrement(&asksQueued);
        Line("respawn: queued ourselves at (%.1f %.1f %.1f), back at %u (sim now %u), selector flags %02X", at[0], at[1], at[2], when, Ptr(base + kSimTime), flags);
    } else {
        vs::Packet p = Make(vs::kRespawnAsk, ref);
        std::memcpy(p.pos, at, sizeof(p.pos));
        Queue(p, GetTickCount());
        flags = static_cast<uint8_t>(Byte(sel + 0x24) | 1);
        askedAt = Stamp(GetTickCount());
        InterlockedIncrement(&asksSent);
        Line("respawn: asked the host for a point (from %.1f %.1f %.1f)", at[0], at[1], at[2]);
    }
    WriteBytes(sel + 0x24, &flags, 1);
}
// IssueSpawnPoint, from the host's selector tick: ours directly, a client's by packet.
bool __fastcall OnIssuePoint(void* self, void*, const uint64_t* refIn, uint32_t point) {
    if (!installed || !cfg.nativeRespawn || !bitsOurs || !refIn) return issueHook.thiscall<bool>(self, refIn, point);
    uint64_t ref = 0;
    Read(reinterpret_cast<uintptr_t>(refIn), &ref, 8);
    const int machine = mp::MachineOfRef(ref);
    if (machine < 0) return issueHook.thiscall<bool>(self, refIn, point);
    const uint32_t sel = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    if (machine == static_cast<int>(machineIndex)) {
        __try {
            reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kSafePtrAssign)(P(sel + 0x2C), point);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&faults);
            return false;
        }
        uint8_t flags = Byte(sel + 0x24);
        flags = static_cast<uint8_t>((flags & ~1) | 2);
        WriteBytes(sel + 0x24, &flags, 1);
        InterlockedIncrement(&pointsLocal);
        Line("respawn: point %08X for us", point);
        return true;
    }
    vs::Packet p = Make(vs::kSpawnPoint, ref);
    if (point) Read(point + 0x24, p.guid, 16);
    Queue(p, GetTickCount());
    InterlockedIncrement(&pointsSent);
    Line("respawn: point %08X (%08X-%08X) for machine %d", point, p.guid[0], p.guid[1], machine);
    return true;
}

// --- scoring ----------------------------------------------------------------------------------
safetyhook::InlineHook entryEventHook, victimHook, attackerHook, tuningHook, combatTextHook;
bool replaying = false;
uint32_t namesSerial = ~0u;
volatile LONG listenerOpens = 0, entriesBuilt = 0, renames = 0, eventsLocal = 0, eventsForwarded = 0, eventsApplied = 0, eventsReplayed = 0, eventsDropped = 0, eventsRefused = 0;
volatile LONG victimFixes = 0, attackerFixes = 0, tuningAnswers = 0, combatTexts = 0, combatSkipped = 0;
bool ScoringOn() { return installed && cfg.scoring && InterlockedCompareExchange(&mpnative::active, 0, 0) && Ptr(base + kScoreManager); }
__declspec(noinline) void OpenScoreListener() {
    __try {
        reinterpret_cast<void(__cdecl*)()>(base + kScoreListenerOpen)();
        InterlockedIncrement(&listenerOpens);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}
uint32_t FindEntry(uint32_t sm, uint64_t ref) {
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t, uint32_t)>(base + kFindEntry)(P(sm), static_cast<uint32_t>(ref), static_cast<uint32_t>(ref >> 32));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return 0;
    }
}
__declspec(noinline) bool BuildEntry(uint32_t sm, const char* name, uint8_t team, uint64_t ref) {
    const uint32_t count = Ptr(sm + 0x44);
    if (count >= 16) return false;
    __try {
        void* block = reinterpret_cast<void*(__cdecl*)(uint32_t)>(base + kOperatorNew)(kEntrySize);
        if (!block) return false;
        std::memset(block, 0, kEntrySize);
        reinterpret_cast<void(__thiscall*)(void*, const char*, uint32_t, uint32_t, uint32_t)>(base + kEntryCtor)(block, name, team, static_cast<uint32_t>(ref),
                                                                                                            static_cast<uint32_t>(ref >> 32));
        const uint32_t iface = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block)) + 0x10;
        const uint32_t next = count + 1;
        WriteBytes(sm + 4 + 4 * count, &iface, 4);
        WriteBytes(sm + 0x44, &next, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}
__declspec(noinline) void AssignString(uint32_t string, const char* text) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, const char*)>(base + kStringAssign)(P(string), text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}
// One entry per roster player on every machine, unregistered: each stays
// authoritative locally and the replay keeps them equal.
void SyncEntries() {
    if (!Ptr(base + kScoreManager)) {
        OpenScoreListener();
        Line("score listener %s: score manager %08X, listener %08X, combat text manager %08X, MP UI block %08X", Ptr(base + kScoreManager) ? "open" : "NOT open",
             Ptr(base + kScoreManager), Ptr(base + 0x1CAD0B8), Ptr(base + kCombatText), Ptr(base + kMpUiBlock));
    }
    const uint32_t sm = Ptr(base + kScoreManager);
    if (!sm) return;
    AcquireSRWLockShared(&mpnative::lock);
    const mp::Roster r = mpnative::roster;
    const uint8_t round = mpnative::roundNo;
    ReleaseSRWLockShared(&mpnative::lock);
    const bool rename = r.serial != namesSerial;
    namesSerial = r.serial;
    for (unsigned i = 0; i < r.count; ++i) {
        const mp::Entry& e = r.entries[i];
        const uint8_t team = mp::EffectiveTeam(e.team, round);
        if (team > mp::kNecro || !e.name[0]) continue;
        const uint64_t ref = mp::RefFor(e.machine);
        const uint32_t iface = FindEntry(sm, ref);
        if (!iface) {
            const bool built = BuildEntry(sm, e.name, team, ref);
            if (built) InterlockedIncrement(&entriesBuilt);
            Line("score entry for machine %u '%s' team %u %s (count %u)", e.machine, e.name, team, built ? "built" : "NOT built", Ptr(sm + 0x44));
        } else if (rename) {
            AssignString(iface - 0x10 + 0xD4, e.name);
            InterlockedIncrement(&renames);
        }
    }
}
using EntryEventFn = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
__declspec(noinline) void ApplyEvent(uint32_t impl, uint32_t type, uint32_t n, uint64_t other, bool replay) {
    const bool was = replaying;
    replaying = replay;
    __try {
        entryEventHook.thiscall<void>(P(impl), type, n, static_cast<uint32_t>(other), static_cast<uint32_t>(other >> 32));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
    replaying = was;
}
// The host applies an event and tells the others; streaks come out of the
// replay on every machine by themselves, so they are never sent.
// Player kills of the current round by team (entry +0xF0: 0 humans, 1 necros),
// counted on the host for the survival director's Versus slashers.
volatile LONG roundKills[2] = {};
void HostApply(uint32_t impl, uint64_t ref, uint32_t type, uint32_t n, uint64_t other) {
    ApplyEvent(impl, type, n, other, false);
    InterlockedIncrement(&eventsApplied);
    if (type == vs::kTypeEnemyKill) {
        const uint8_t team = Byte(impl + 0xF0);
        if (team <= mp::kNecro) InterlockedExchangeAdd(&roundKills[team], static_cast<LONG>(n));
    }
    if (network && !vs::IsStreak(type)) Queue(Make(vs::kScore, ref, other, static_cast<int32_t>(type), static_cast<int32_t>(n)), GetTickCount());
}
// A necro killing a bot: the bot has no ref, so the victim id stays 0 and the native
// code books an NPC kill straight onto the host's copy of the killer's entry, past
// LogScoringEvent (ProcessKills 0x010E1DAE): no points, no combat text, nothing sent
// (13.10.). The host books the enemy kill itself, with no victim (a victim without an
// entry is dropped by 0x010D9F00), and the replay gives the necro his points.
volatile LONG botKillsCredited = 0, botKillsNoHit = 0, botKillsStale = 0, botKillsNotNecro = 0, botKillsNoEntry = 0, botKillsNotHost = 0;
void CreditBotKill(unsigned bot, uint64_t ref, uint16_t flags, DWORD ageMs) {
    if (!ScoringOn()) return;
    const char* why = nullptr;
    volatile LONG* counter = nullptr;
    const int machine = ref ? mp::MachineOfRef(ref) : -1;
    if (!Hosting()) { why = "not hosting"; counter = &botKillsNotHost; }
    else if (!ref || machine < 0) { why = "no player hit it last"; counter = &botKillsNoHit; }
    else if (ageMs > 5000) { why = "the last hit is too old"; counter = &botKillsStale; }
    else if (TeamOf(static_cast<unsigned>(machine)) != mp::kNecro) { why = "the last hitter is no necro"; counter = &botKillsNotNecro; }
    const uint32_t sm = Ptr(base + kScoreManager);
    const uint32_t iface = !why && sm ? FindEntry(sm, ref) : 0;
    if (!why && !iface) { why = "the killer has no score entry"; counter = &botKillsNoEntry; }
    if (why) {
        InterlockedIncrement(counter);
        Line("bot %u died: no kill credited (%s; ref %016llX flags %04X, hit %lu ms before)", bot + 1, why, static_cast<unsigned long long>(ref), flags,
             static_cast<unsigned long>(ageMs));
        return;
    }
    HostApply(iface - 0x10, ref, vs::kTypeEnemyKill, 1, 0);
    InterlockedIncrement(&botKillsCredited);
    Line("bot %u died: enemy kill credited to machine %d (ref %016llX flags %04X, hit %lu ms before)", bot + 1, machine,
         static_cast<unsigned long long>(ref), flags, static_cast<unsigned long>(ageMs));
}
void __fastcall OnEntryEvent(void* self, void*, uint32_t type, uint32_t n, uint32_t otherLo, uint32_t otherHi) {
    const uint32_t impl = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    uint64_t ref = 0;
    Read(impl + 0xE8, &ref, 8);
    if (replaying || !ScoringOn() || mp::MachineOfRef(ref) < 0) { entryEventHook.thiscall<void>(self, type, n, otherLo, otherHi); return; }
    const uint64_t other = (static_cast<uint64_t>(otherHi) << 32) | otherLo;
    InterlockedIncrement(&eventsLocal);
    // objective completion runs on every machine: each counts only its own player's
    if (vs::IsObjective(type) && mp::MachineOfRef(ref) != static_cast<int>(machineIndex)) { InterlockedIncrement(&eventsDropped); return; }
    if (Hosting()) { HostApply(impl, ref, type, n, other); return; }
    if (vs::IsStreak(type)) { InterlockedIncrement(&eventsDropped); return; }
    Queue(Make(vs::kScoreAsk, ref, other, static_cast<int32_t>(type), static_cast<int32_t>(n)), GetTickCount());
    InterlockedIncrement(&eventsForwarded);
}
// The victim and the attacker as refs: the native code takes the local one from
// the Blaze player (none offline); ghosts carry theirs (TeamGroups writes them).
bool LocalBody(uint32_t entity) { return entity && (entity == LocalSlotZeroEntity() || entity == LocalCampaignPlayer() || entity == necro::necroEntity); }
void __stdcall OnVictimInfo(uint32_t victim, uint32_t* id, uint8_t* auth, uint8_t* keyItem, uint8_t* paired) {
    victimHook.stdcall<void>(victim, id, auth, keyItem, paired);
    if (!ScoringOn() || !id || !auth || !*auth || id[0] || id[1] || !LocalBody(victim)) return;
    const uint64_t ref = OwnRef();
    std::memcpy(id, &ref, 8);
    InterlockedIncrement(&victimFixes);
}
void __stdcall OnAttackerInfo(uint32_t attacker, uint32_t* id, uint8_t* paired) {
    attackerHook.stdcall<void>(attacker, id, paired);
    if (!ScoringOn() || !id || id[0] || id[1] || !LocalBody(attacker)) return;
    const uint64_t ref = OwnRef();
    std::memcpy(id, &ref, 8);
    InterlockedIncrement(&attackerFixes);
}
// The points: Blaze title settings natively, ours in a Versus visit.
float __fastcall OnTuning(void* lm, void*, uint32_t hash, float fallback) {
    if (installed && cfg.scoring && InterlockedCompareExchange(&mpnative::active, 0, 0))
        if (const vs::Points* p = vs::FindPoints(hash)) {
            InterlockedIncrement(&tuningAnswers);
            return static_cast<float>(p->value);
        }
    return tuningHook.thiscall<float>(lm, hash, fallback);
}
// The native combat text needs the Blaze player; each machine shows its own player's.
void __fastcall OnCombatText(void* self, void*, uint32_t type, int32_t n) {
    if (!ScoringOn()) { combatTextHook.thiscall<void>(self, type, n); return; }
    const uint32_t impl = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)), manager = Ptr(base + kCombatText);
    uint64_t ref = 0;
    Read(impl + 0xE8, &ref, 8);
    if (n <= 0 || !manager || ref != OwnRef()) { InterlockedIncrement(&combatSkipped); return; }
    __try {
        int32_t event = 0;
        const bool known = reinterpret_cast<bool(__stdcall*)(uint32_t, uint32_t, int32_t*)>(base + kTranslate)(Byte(impl + 0xF0), type, &event);
        if (known) {
            reinterpret_cast<void(__thiscall*)(void*, int32_t, int32_t)>(base + kRegisterCombatText)(P(manager), event, n);
            InterlockedIncrement(&combatTexts);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}

// --- killer card ------------------------------------------------------------------------------
safetyhook::InlineHook killerHook;
volatile LONG relaysBuilt = 0, killerCards = 0, killerNamed = 0, killerSelf = 0, killerAi = 0;
char killerName[24] = "";
__declspec(noinline) void EnsureRelay() {
    if (!cfg.killerCard || Ptr(base + kRelay)) return;
    __try {
        void* block = reinterpret_cast<void*(__cdecl*)(uint32_t)>(base + kOperatorNew)(kRelaySize);
        if (!block) return;
        std::memset(block, 0, kRelaySize);
        reinterpret_cast<void(__thiscall*)(void*)>(base + kRelayCtor)(block);
        InterlockedIncrement(&relaysBuilt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
    Line("killer relay %s (%08X)", Ptr(base + kRelay) ? "built" : "NOT built", Ptr(base + kRelay));
}
// Offline the local ref is 0, so a kill by the world reads as a suicide and our
// own as friendly fire; the roster gives the killer's name.
void __fastcall OnKillerInfo(void* self, void*, uint32_t attackerFlags, const uint32_t* killerRef, uint32_t inflictorFlags) {
    killerHook.thiscall<void>(self, attackerFlags, killerRef, inflictorFlags);
    if (!installed || !cfg.killerCard || !InterlockedCompareExchange(&mpnative::active, 0, 0)) return;
    InterlockedIncrement(&killerCards);
    const uint32_t relay = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    uint64_t ref = 0;
    if (killerRef) Read(reinterpret_cast<uintptr_t>(killerRef), &ref, 8);
    const int machine = mp::MachineOfRef(ref);
    if (!ref) {
        const uint32_t kind = 1;
        WriteBytes(relay + 4, &kind, 4);
        const uint16_t flags = static_cast<uint16_t>(Ptr(relay + 0x1A) & 0xFFFF);
        for (uint32_t row = 0; row < 7; ++row) {
            const uint32_t at = base + kKillerTable + row * 12;
            const uint16_t mask = static_cast<uint16_t>(Ptr(at) & 0xFFFF);
            if ((flags & mask) != mask) continue;
            const uint32_t text = Ptr(at + 8);
            if (text) { AssignString(relay + 0x1C, reinterpret_cast<const char*>(static_cast<uintptr_t>(text))); AssignString(relay + 8, reinterpret_cast<const char*>(static_cast<uintptr_t>(text))); }
            break;
        }
        // the weapon line the suicide branch cleared, from the inflictor flags as natively
        const uint32_t inflictor = Ptr(relay + 0x2C);
        for (uint32_t row = 0; row < 15; ++row) {
            const uint32_t at = base + kInflictorTable + row * 8, mask = Ptr(at);
            if ((inflictor & mask) != mask) continue;
            if (const uint32_t text = Ptr(at + 4)) AssignString(relay + 0x30, reinterpret_cast<const char*>(static_cast<uintptr_t>(text)));
            break;
        }
        InterlockedIncrement(&killerAi);
    } else if (machine == static_cast<int>(machineIndex)) {
        const uint32_t kind = 4;
        WriteBytes(relay + 4, &kind, 4);
        const char* empty = reinterpret_cast<const char*>(static_cast<uintptr_t>(base + kEmptyText));
        AssignString(relay + 8, reinterpret_cast<const char*>(static_cast<uintptr_t>(base + kSuicideText)));
        AssignString(relay + 0x1C, empty);
        AssignString(relay + 0x30, empty);
        InterlockedIncrement(&killerSelf);
    } else if (machine >= 0 && NameOf(ref, killerName)) {
        AssignString(relay + 8, killerName);
        InterlockedIncrement(&killerNamed);
    }
    Line("killer card: ref %08X%08X (machine %d) kind %u flags %04X name '%s'", static_cast<uint32_t>(ref >> 32), static_cast<uint32_t>(ref), machine, Ptr(relay + 4),
         Ptr(relay + 0x1A) & 0xFFFF, machine >= 0 && machine != static_cast<int>(machineIndex) ? killerName : "");
}

// --- rounds -----------------------------------------------------------------------------------
safetyhook::InlineHook activateHook;
vs::Match match;
bool roundReplay = false, restartWanted = false;
DWORD restartSince = 0;
bool sawIdle = true;   // the next arrival is a new match
volatile LONG activations = 0, localEndsIgnored = 0, roundEnds = 0, swaps = 0, restartsPulled = 0, matchOvers = 0;
const char* TeamName(int team) { return team == 0 ? "HUMANS" : team == 1 ? "NECROMORPHS" : "NOBODY"; }
void __fastcall OnActivate(void* self, void*, uint32_t team) {
    InterlockedIncrement(&activations);
    if (!installed || !cfg.rounds || !InterlockedCompareExchange(&mpnative::active, 0, 0) || roundReplay) { activateHook.thiscall<void>(self, team); return; }
    const uint8_t winner = static_cast<uint8_t>(team);
    if (!Hosting()) {
        InterlockedIncrement(&localEndsIgnored);
        Line("round end here (winner team %u) ignored: the host ends the rounds", winner);
        return;
    }
    activateHook.thiscall<void>(self, team);
    const DWORD now = GetTickCount();
    if (!match.End(mpnative::roundNo, winner, now)) return;
    InterlockedIncrement(&roundEnds);
    Queue(Make(vs::kRoundEnd, 0, 0, mpnative::roundNo, winner), now);
    char text[64];
    std::snprintf(text, sizeof(text), "ROUND %u: %s WIN", mpnative::roundNo, TeamName(winner));
    notice::Enqueue(text);
    Line("round %u over, winner team %u (host); %s in %lu s", mpnative::roundNo, winner, mpnative::roundNo == 1 ? "sides swap" : "match over",
         static_cast<unsigned long>((mpnative::roundNo == 1 ? cfg.swapMs : cfg.swapMs + cfg.resultsMs) / 1000));
}
__declspec(noinline) void ReplayActivate(uint8_t winner) {
    const uint32_t sequencer = Ptr(base + kSequencer);
    if (!sequencer || (Byte(sequencer + 0x6C) & 1)) { Line("round end from the host: sequencer %08X %s", sequencer, sequencer ? "already active" : "missing"); return; }
    roundReplay = true;
    __try {
        activateHook.thiscall<void>(P(sequencer), static_cast<uint32_t>(winner));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
    roundReplay = false;
}
__declspec(noinline) void HideRoundScreens() {
    __try {
        reinterpret_cast<void(__cdecl*)()>(base + kHideEndRound)();
        reinterpret_cast<void(__cdecl*)()>(base + kHideScoring)();
        if (const uint32_t sequencer = Ptr(base + kSequencer)) reinterpret_cast<void(__thiscall*)(void*)>(base + kShutdownCamera)(P(sequencer));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}
__declspec(noinline) void ChangeTeam(uint32_t sm, uint32_t index, uint8_t team) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t)>(base + kTeamChange)(P(sm), index, team);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}
// The scoreboard (MP11/MP14 team boards) while ScoreKey is held. The native
// trigger sits in DGM::HandleEvents on BUTTON_SELECT and stays silent offline.
bool scoreDown = false, scoreOurs = false;
volatile LONG scoreShows = 0, scoreRefused = 0, scoreHides = 0;
using BoolFn = bool(__cdecl*)();
__declspec(noinline) bool ScoringCall(uint32_t rva, bool* result) {
    __try {
        const bool r = reinterpret_cast<BoolFn>(base + rva)();
        if (result) *result = r;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}
void PollScoreKey() {
    if (!cfg.scoring || !cfg.scoreKey || !SigOk(kShowScoring) || !SigOk(kCanShowScoring) || !SigOk(kIsShownScoring)) return;
    const bool down = necro::GameWindowFocused() && (GetAsyncKeyState(cfg.scoreKey) & 0x8000) != 0;
    if (down == scoreDown) return;
    scoreDown = down;
    bool shown = false, can = false;
    ScoringCall(kIsShownScoring, &shown);
    if (down && !shown) {
        ScoringCall(kCanShowScoring, &can);
        if (!can) {
            InterlockedIncrement(&scoreRefused);
            Line("scoreboard refused: MP UI block %08X, score manager %08X (%u entries), MPLevelSettings %08X", Ptr(base + kMpUiBlock), Ptr(base + kScoreManager),
                 Ptr(Ptr(base + kScoreManager) + 0x44), Ptr(base + kLevelSettings));
            return;
        }
        ScoringCall(kShowScoring, nullptr);
        ScoringCall(kIsShownScoring, &shown);
        scoreOurs = shown;
        InterlockedIncrement(&scoreShows);
        if (InterlockedCompareExchange(&scoreShows, 0, 0) <= 3)
            Line("scoreboard %s (team %u, %u entries)", shown ? "shown" : "NOT shown after the call", Byte(Ptr(base + menu::kGameManager) + 0xF2), Ptr(Ptr(base + kScoreManager) + 0x44));
    } else if (!down && shown && scoreOurs) {
        ScoringCall(kHideScoring, nullptr);
        scoreOurs = false;
        InterlockedIncrement(&scoreHides);
    }
}

// The online pause menu: "friends list" opens Nucleus's friends screen and
// "quit" ends the map through GameFlowManager::CompleteMap, both for a Blaze
// game. In our match the first is refused and the second leaves the way the
// match end does.
volatile LONG pauseFriendsRefused = 0, pauseQuits = 0, leaveWanted = 0;
bool __fastcall OnPauseSelect(void* self, void*, int index) {
    if (installed && bitsOurs && index == 3) {
        InterlockedIncrement(&pauseFriendsRefused);
        notice::Enqueue("FRIENDS LIST NEEDS EA ONLINE - NOT AVAILABLE");
        return true;
    }
    return reinterpret_cast<bool(__thiscall*)(void*, int)>(base + kPauseSelect)(self, index);
}
void __fastcall OnPauseQuit(void* self, void*, const unsigned char* result) {
    uint32_t value = 1;
    Read(reinterpret_cast<uintptr_t>(result) + 8, &value, 4);
    if (!installed || !InterlockedCompareExchange(&mpnative::active, 0, 0) || value) {
        reinterpret_cast<void(__thiscall*)(void*, const void*)>(base + kPauseQuit)(self, result);
        return;
    }
    __try {
        reinterpret_cast<void(__cdecl*)()>(base + kHidePause)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
    InterlockedIncrement(&pauseQuits);
    InterlockedExchange(&leaveWanted, 1);   // on the game tick, outside the menu's input
}

// Every machine, on the host's word: screens down, round 2 in the score
// tables, the teams flipped, the selector gone; the host then restarts.
void Swap(DWORD now) {
    InterlockedIncrement(&swaps);
    HideRoundScreens();
    const uint32_t dgm = Ptr(base + menu::kGameManager), sm = Ptr(base + kScoreManager);
    const uint8_t one = 1;
    if (dgm) WriteBytes(dgm + 0x244, &one, 1);
    unsigned flipped = 0;
    if (sm) {
        WriteBytes(sm + 0x48, &one, 1);
        WriteBytes(sm + 0x49, &one, 1);
        const uint32_t count = Ptr(sm + 0x44);
        for (uint32_t i = 0; i < count && i < 16; ++i) {
            const uint32_t iface = Ptr(sm + 4 + 4 * i);
            if (!iface) continue;
            const uint8_t team = Byte(iface - 0x10 + 0xF0);
            if (team > 1) continue;
            ChangeTeam(sm, i, static_cast<uint8_t>(1 - team));
            ++flipped;
        }
    }
    AcquireSRWLockExclusive(&mpnative::lock);
    mpnative::roundNo = 2;
    ++mpnative::roster.serial;
    ReleaseSRWLockExclusive(&mpnative::lock);
    if (cfg.nativeRespawn) DeleteSelector();
    if (const uint32_t nrm = Ptr(base + kRespawnManager)) { const uint32_t five = 5; WriteBytes(nrm + 0x30, &five, 4); }
    match.Swap();
    InterlockedExchange(&roundKills[0], 0);
    InterlockedExchange(&roundKills[1], 0);
    restartWanted = Hosting();
    restartSince = GetTickCount();
    notice::Enqueue("SIDES SWAP - ROUND 2");
    Line("sides swap: round 2, %u score entries flipped, DGM %08X SM %08X; %s", flipped, dgm, sm, restartWanted ? "pulling the restart" : "following the host's restart");
    (void)now;
}
int ScoreFor(uint64_t ref, uint8_t round, uint8_t team) {
    __try {
        return reinterpret_cast<int(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t)>(base + kScoreForRound)(static_cast<uint32_t>(ref), static_cast<uint32_t>(ref >> 32),
                                                                                                            round, team);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return 0;
    }
}
void Results() {
    AcquireSRWLockShared(&mpnative::lock);
    const mp::Roster r = mpnative::roster;
    ReleaseSRWLockShared(&mpnative::lock);
    struct Line_ { char name[24]; int score; } rows[mp::kMaxPlayers]{};
    unsigned n = 0;
    for (unsigned i = 0; i < r.count && n < mp::kMaxPlayers; ++i) {
        const mp::Entry& e = r.entries[i];
        if (e.team > mp::kNecro) continue;
        const uint64_t ref = mp::RefFor(e.machine);
        const int score = ScoreFor(ref, 0, mp::EffectiveTeam(e.team, 1)) + ScoreFor(ref, 1, mp::EffectiveTeam(e.team, 2));
        std::memcpy(rows[n].name, e.name, sizeof(rows[n].name));
        rows[n].score = score;
        ++n;
    }
    for (unsigned i = 1; i < n; ++i)
        for (unsigned j = i; j > 0 && rows[j].score > rows[j - 1].score; --j) std::swap(rows[j], rows[j - 1]);
    char text[96];
    std::snprintf(text, sizeof(text), "MATCH OVER - ROUND 1: %s, ROUND 2: %s", TeamName(match.winner[1]), TeamName(match.winner[2]));
    notice::Enqueue(text);
    for (unsigned i = 0; i < n && i < 4; ++i) {
        std::snprintf(text, sizeof(text), "%u. %s  %d", i + 1, rows[i].name, rows[i].score);
        notice::Enqueue(text);
    }
    for (unsigned i = 0; i < n; ++i) Line("result %u: '%s' %d", i + 1, rows[i].name, rows[i].score);
    // the match score is the XP, as natively; the lobby shows the level from it
    const uint64_t own = OwnRef();
    const mp::Entry* me = r.Find(machineIndex);
    if (!me || me->team > mp::kNecro) return;
    // and the retail round bonus: 1000 per round your team won, 2000 more for both
    const unsigned won = (match.winner[1] == static_cast<int8_t>(mp::EffectiveTeam(me->team, 1))) +
                         (match.winner[2] == static_cast<int8_t>(mp::EffectiveTeam(me->team, 2)));
    const int bonus = static_cast<int>(won) * cfg.roundWin + (won == 2 ? cfg.bothRounds : 0);
    const int earned = ScoreFor(own, 0, mp::EffectiveTeam(me->team, 1)) + ScoreFor(own, 1, mp::EffectiveTeam(me->team, 2)) + bonus;
    if (bonus) Line("profile: %u round(s) won, +%d XP bonus", won, bonus);
    if (earned <= 0) return;
    const unsigned before = mp::LevelForXp(mpnative::profile.xp);
    mpnative::profile.xp += static_cast<uint32_t>(earned);
    mpnative::SaveProfile();
    const unsigned after = mp::LevelForXp(mpnative::profile.xp);
    std::snprintf(text, sizeof(text), after > before ? "+%d XP - LEVEL %u!" : "+%d XP (LEVEL %u)", earned, after);
    notice::Enqueue(text);
    Line("profile: +%d XP, now %u (level %u -> %u)", earned, mpnative::profile.xp, before, after);
}
void MatchOver(DWORD now) {
    InterlockedIncrement(&matchOvers);
    match.Over();
    HideRoundScreens();
    Line("match over: back to the menu");
    mpmap::ReturnToCampaign(now, "versus match over");
}
void PollRounds(DWORD now) {
    if (!cfg.rounds) return;
    if (mpmap::visit.phase == coop_mpmap::Phase::Idle) sawIdle = true;
    else if (sawIdle && mpmap::visit.phase == coop_mpmap::Phase::Arrived) {
        sawIdle = false;
        match = vs::Match{};
        restartWanted = false;
        InterlockedExchange(&roundKills[0], 0);
        InterlockedExchange(&roundKills[1], 0);
        AcquireSRWLockExclusive(&mpnative::lock);
        mpnative::roundNo = 1;
        ReleaseSRWLockExclusive(&mpnative::lock);
    }
    if (!OnMap()) return;
    if (restartWanted && checkpoint::PullRestart()) {
        restartWanted = false;
        InterlockedIncrement(&restartsPulled);
        Line("round 2: quick restart pulled");
    } else if (restartWanted && now - restartSince > 10000) {
        restartWanted = false;
        Line("round 2: the quick restart was refused for 10 s (checkpoint module off or not in the level); round 2 plays on without it");
    }
    if (!Hosting()) return;
    static bool resultsShown = false;
    if (match.stage != vs::Stage::RoundOver) resultsShown = false;
    if (match.round == 2 && match.stage == vs::Stage::RoundOver && !resultsShown && now - match.endedAt >= cfg.swapMs) {
        resultsShown = true;
        Results();
    }
    switch (match.Due(now, cfg.swapMs, cfg.resultsMs)) {
    case 1:
        Queue(Make(vs::kSwap, 0, 0, 2), now);
        Swap(now);
        break;
    case 2:
        Queue(Make(vs::kMatchOver), now);
        Flush(now);   // out before the level goes
        MatchOver(now);
        break;
    default: break;
    }
}

// For the survival director's slashers (DS2Coop_VersusRound): out[0] the stage
// (0 playing, 1 round over, 2 match over), out[1] the round, out[2] and out[3]
// the round's player kills by the humans and by the necros (host only).
volatile LONG roundInfoAsks = 0;
int RoundInfo(int* out, int count) {
    InterlockedIncrement(&roundInfoAsks);
    if (!installed || !OnMap()) return 0;
    const int values[4] = {static_cast<int>(match.stage), match.round, static_cast<int>(InterlockedCompareExchange(&roundKills[0], 0, 0)),
                           static_cast<int>(InterlockedCompareExchange(&roundKills[1], 0, 0))};
    for (int i = 0; out && i < count && i < 4; ++i) out[i] = values[i];
    return 1;
}

// --- music ------------------------------------------------------------------------------------
// The map's round music through the survival music module: combat tracks while a
// round runs, the calm ones between the rounds, silence after the match. The
// director that drives it in survival idles in Versus (11.10.: no music). Also
// with NativeAudio: the map's own music chain never started in a visit (13.10.:
// music changes=0 all match, no music outside survival).
int musicState = -1;
volatile LONG musicChanges = 0;
void PollMusic() {
    if (!cfg.music || !survmusic::enabled) return;
    const int want = !OnMap() ? -1 : match.stage == vs::Stage::Playing ? 2 : match.stage == vs::Stage::RoundOver ? 3 : 0;
    if (want == musicState) return;
    musicState = want;
    InterlockedIncrement(&musicChanges);
    survmusic::SetState(want < 0 ? 0 : want, match.round, "versus");
}

// What Versus lets through of global_mp_assets: the MP audio layer ([Versus]
// NativeAudio: the announcer, the stingers, the MP music logic) and the combat
// text manager ([Versus] CombatText: the points over each event). The UI popups
// (the generic yes/no and continue dialogs, a second copy of the campaign's,
// crash 28644), the registries, the player templates and the pickups stay out.
bool PassesClass(const char* cls) {
    if (!installed || !InterlockedCompareExchange(&mpnative::active, 0, 0)) return false;
    // No LoadMixPatch: global_assets' submix patch is already active, and a second
    // one from global_mp_assets crashed both machines on a Versus exit (12.10.).
    static const char* const kAudio[] = {"PlaySound", "PlayAudioStream", "SetAudioMix", "RASAnnouncement", "MultiManager", "LuaAudioScript",
                                         "TriggerRandom", "TriggerAuto", "OrEventGate", "RelayMessageByTeam"};
    if (cfg.nativeAudio)
        for (const char* c : kAudio)
            if (std::strcmp(cls, c) == 0) return true;
    return cfg.combatText && std::strcmp(cls, "UIMPCombatTextManager") == 0;
}

// --- team groups ------------------------------------------------------------------------------
struct Joined { uint32_t entity = 0, key = 0; uint64_t ref = 0; uint8_t team = 0xFF; };
Joined joined[16];
volatile LONG refWrites = 0, groupJoins = 0;
DWORD groupsAt = 0;
__declspec(noinline) void Regroup(uint32_t gm, uint32_t entity, uint32_t leave, uint32_t join) {
    __try {
        using GroupFn = void(__thiscall*)(void*, uint32_t, uint32_t);
        if (leave) reinterpret_cast<GroupFn>(base + kRemoveFromGroup)(P(gm), entity, leave);
        reinterpret_cast<GroupFn>(base + kAddToGroup)(P(gm), entity, join);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}
uint32_t ScoringComponent(uint32_t entity) {
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t)>(base + kGetComponent)(P(entity), Byte(base + kScoringIndex));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return 0;
    }
}
void PollGroups(DWORD now) {
    if (!cfg.teamGroups || now - groupsAt < 500) return;
    groupsAt = now;
    const uint32_t manager = Ptr(base + kPlayerManagerRva), gm = Ptr(base + kGroupManager), dgm = Ptr(base + menu::kGameManager);
    const uint32_t slots = manager ? Ptr(manager + 4) : 0, capacity = manager ? Ptr(manager + 8) : 0;
    if (!slots || !gm || !dgm || capacity > 8) return;
    const uint32_t ids[2] = {Ptr(dgm + 0x238), Ptr(dgm + 0x23C)};
    for (uint32_t i = 0; i < capacity; ++i) {
        const uint32_t bp = Ptr(slots + 4 * i);
        if (bp < 0x103C0) continue;
        const uint32_t entity = bp - 0x3C0, vtable = Ptr(entity);
        const bool isPlayer = vtable == base + placement::kPlayerPrimaryVtableRva, isNecro = vtable == base + kNecroPrimaryVtableRva;
        if ((!isPlayer && !isNecro) || IsBotEntity(entity)) continue;
        const uint32_t machine = LocalBody(entity) ? machineIndex : MachineOf(entity);
        if (machine >= 16) continue;
        const uint8_t team = TeamOf(machine);
        if (team > mp::kNecro) continue;
        const uint64_t ref = mp::RefFor(machine);
        const uint32_t key = Ptr(entity + 0x40);
        WriteBytes(entity + (isPlayer ? kPlayerRef : kNecroRef), &ref, 8);
        if (const uint32_t component = ScoringComponent(entity)) WriteBytes(component + 0x18, &ref, 8);
        Joined* slot = nullptr;
        for (auto& j : joined) if (j.entity == entity) { slot = &j; break; }
        if (slot && slot->key == key && slot->ref == ref && slot->team == team) continue;
        if (!slot) for (auto& j : joined) if (!j.entity) { slot = &j; break; }
        if (!slot) { joined[0] = Joined{}; slot = &joined[0]; }
        Regroup(gm, entity, ids[team ^ 1], ids[team]);
        InterlockedIncrement(&refWrites);
        InterlockedIncrement(&groupJoins);
        Line("team group: %s %08X machine %u ref %08X team %u (group %08X)%s", isPlayer ? "Player" : "NecroPlayer", entity, machine, static_cast<uint32_t>(ref), team, ids[team],
             LocalBody(entity) ? " (ours)" : "");
        *slot = Joined{entity, key, ref, team};
    }
}

// --- the game tick ----------------------------------------------------------------------------
// The game's own caller (0x010D9F80) refuses types from 0x18 on before LogScoringEvent indexes an array
// with it; a peer's packet skipped that check (security review 2026-10-24).
constexpr uint32_t kScoreTypeLimit = 0x18, kScoreCountLimit = 1000;
void Handle(const vs::Packet& p, DWORD now) {
    if ((p.kind == vs::kScoreAsk || p.kind == vs::kScore) &&
        (static_cast<uint32_t>(p.a) >= kScoreTypeLimit || static_cast<uint32_t>(p.b) > kScoreCountLimit)) {
        if (InterlockedIncrement(&eventsRefused) <= 8)
            Line("score event from machine %u refused: type %u, count %u out of range", static_cast<unsigned>(p.machine),
                 static_cast<uint32_t>(p.a), static_cast<uint32_t>(p.b));
        return;
    }
    switch (p.kind) {
    case vs::kScoreAsk: {
        const uint32_t sm = Ptr(base + kScoreManager);
        const uint32_t iface = sm ? FindEntry(sm, p.ref) : 0;
        if (!ScoringOn() || !iface) { InterlockedIncrement(&eventsDropped); break; }
        if (vs::IsObjective(static_cast<uint32_t>(p.a)) && mp::MachineOfRef(p.ref) != p.machine) { InterlockedIncrement(&eventsDropped); break; }
        HostApply(iface - 0x10, p.ref, static_cast<uint32_t>(p.a), static_cast<uint32_t>(p.b), p.other);
        break;
    }
    case vs::kScore: {
        const uint32_t sm = Ptr(base + kScoreManager);
        const uint32_t iface = sm ? FindEntry(sm, p.ref) : 0;
        if (!ScoringOn() || !iface) { InterlockedIncrement(&eventsDropped); break; }
        ApplyEvent(iface - 0x10, static_cast<uint32_t>(p.a), static_cast<uint32_t>(p.b), p.other, true);
        InterlockedIncrement(&eventsReplayed);
        break;
    }
    case vs::kRoundEnd:
        if (!cfg.rounds || !match.End(static_cast<uint8_t>(p.a), p.b, now)) break;
        Line("round %d over from the host, winner team %d", p.a, p.b);
        ReplayActivate(static_cast<uint8_t>(p.b));
        {
            char text[64];
            std::snprintf(text, sizeof(text), "ROUND %d: %s WIN", p.a, TeamName(p.b));
            notice::Enqueue(text);
        }
        break;
    case vs::kSwap:
        if (cfg.rounds && match.round == 1) Swap(now);
        break;
    case vs::kMatchOver:
        if (cfg.rounds && match.stage != vs::Stage::MatchOver) { Results(); MatchOver(now); }
        break;
    case vs::kRespawnAsk: {
        const uint32_t sel = Ptr(base + kSelector);
        if (!cfg.nativeRespawn || !sel || !bitsOurs) {
            Queue(Make(vs::kSpawnPoint, p.ref), now);   // zero GUID: back where it is
            Line("respawn ask from machine %u refused (selector %08X, bits %d); it comes back in place", p.machine, sel, bitsOurs ? 1 : 0);
            break;
        }
        uint32_t when = 0;
        const uint64_t ref = p.ref;
        alignas(16) float at[4];   // movaps in the queue; the packet's copy is not aligned
        std::memcpy(at, p.pos, sizeof(at));
        __try {
            when = reinterpret_cast<QueueFn>(base + kQueueRespawn)(P(sel), &ref, at);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&faults);
        }
        InterlockedIncrement(&asksQueued);
        const int32_t delay = static_cast<int32_t>(when - Ptr(base + kSimTime));
        Queue(Make(vs::kRespawnDelay, p.ref, 0, delay > 0 ? delay : 0), now);
        Line("respawn: machine %u queued, back in %d ms", p.machine, delay);
        break;
    }
    case vs::kRespawnDelay: {
        const uint32_t sel = Ptr(base + kSelector);
        if (p.ref != OwnRef() || !sel) break;
        const uint32_t when = Ptr(base + kSimTime) + static_cast<uint32_t>(p.a > 0 ? p.a : 0);
        WriteBytes(sel + 0x28, &when, 4);
        const uint8_t flags = static_cast<uint8_t>(Byte(sel + 0x24) | 4);
        WriteBytes(sel + 0x24, &flags, 1);
        InterlockedIncrement(&delaysIn);
        break;
    }
    case vs::kSpawnPoint: {
        const uint32_t sel = Ptr(base + kSelector);
        if (p.ref != OwnRef() || !sel) break;
        const uint8_t flags = static_cast<uint8_t>(Byte(sel + 0x24) | 1);
        WriteBytes(sel + 0x24, &flags, 1);
        __try {
            reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t)>(base + kPointResponse)(P(sel), p.guid[0], p.guid[1], p.guid[2], p.guid[3]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&faults);
        }
        InterlockedIncrement(&pointsIn);
        askedAt = 0;
        Line("respawn: the host's point %08X-%08X; selector flags %02X point %08X", p.guid[0], p.guid[1], Byte(sel + 0x24), Ptr(sel + 0x2C));
        break;
    }
    default: break;
    }
}
// --- head watch -------------------------------------------------------------------------------
// 12.10.: on the host the heads of every player vanished now and then. Per Player
// body: its helmet (its own prop, SafePtr at +0x168C), the hide counters of helmet
// and body (+0x16D, Entity::Hide/Show), the helmet-open bit (+0x3DC bit 23) and the
// camera fade alpha (+0x158), logged whenever one of them changes.
struct HeadState { uint32_t body = 0, helmet = 0; uint8_t helmetHidden = 0, bodyHidden = 0; bool open = false; float alpha = 1.0f; };
HeadState heads[8];
volatile LONG headChanges = 0;
// A partner's (or a bot's) ghost body is created hidden and shown by its spawn. A
// necro client that joins while they already stand never sees that spawn, so they
// stayed invisible until their next death (12.10.). A live ghost Player hidden for
// 1.5 s in a Versus visit is shown through Entity::Show (vtable +0x6C); our own body
// after 3 s (two suit changes on top of each other left it hidden). Only a complete
// body (model, bones, suit): a suit change hides it while the model is rebuilt, and
// a half-built model must never reach the renderer.
DWORD hiddenSince[8]{};
volatile LONG ghostsShown = 0, ghostShowFaults = 0;
__declspec(noinline) bool ShowRaw(uint32_t entity) {
    __try {
        const uint32_t fn = Ptr(Ptr(entity) + 0x6C);
        if (fn < base + 0x1000 || fn >= base + gate::kRdataLo) return false;
        reinterpret_cast<void(__thiscall*)(void*)>(fn)(P(entity));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&ghostShowFaults);
        return false;
    }
}
void ShowStuckGhost(uint32_t slot, uint32_t body, uint8_t hidden, DWORD now) {
    const bool own = body && body == LocalCampaignPlayer() && !IsBotEntity(body);
    if (!body || !hidden || !(own || IsGhostPlayer(body)) || !OnMap()) { hiddenSince[slot] = 0; return; }
    if (!hiddenSince[slot]) { hiddenSince[slot] = Stamp(now); return; }
    if (now - hiddenSince[slot] < (own ? 3000u : 1500u)) return;
    const uint32_t health = hits::FindHealthComponent(body);
    float current = 0.0f;
    if (!health || !Read(health + hits::kHealthCurrent, &current, 4) || !(current > 0.0f)) return;
    uint32_t parts[3]{};
    if (!coop_uniform::BotBody(body, parts)) return;   // still being rebuilt
    hiddenSince[slot] = 0;
    for (uint8_t k = 0; k < hidden && k < 4; ++k)
        if (!ShowRaw(body)) break;
    InterlockedIncrement(&ghostsShown);
    Line("%s body %08X in slot %u was hidden (count %u) while alive and complete: shown, count now %u", own ? "our" : "ghost", body, slot, hidden,
         Byte(body + 0x16D));
}
DWORD headsAt = 0;
void WatchHeads(DWORD now) {
    if (now - headsAt < 200) return;
    headsAt = now;
    const uint32_t manager = Ptr(base + kPlayerManagerRva);
    const uint32_t slots = manager ? Ptr(manager + 4) : 0, capacity = manager ? Ptr(manager + 8) : 0;
    if (!slots || capacity > 8) return;
    for (uint32_t i = 0; i < capacity; ++i) {
        const uint32_t bp = Ptr(slots + 4 * i);
        const uint32_t body = bp >= 0x103C0 ? bp - 0x3C0 : 0;
        HeadState cur{};
        if (body && Ptr(body) == base + placement::kPlayerPrimaryVtableRva) {
            cur.body = body;
            const uint32_t held = Ptr(body + 0x168C);
            cur.helmet = held > 0x10010 ? held - 0x10 : 0;
            if (cur.helmet) cur.helmetHidden = Byte(cur.helmet + 0x16D);
            cur.bodyHidden = Byte(body + 0x16D);
            cur.open = ((Ptr(body + 0x3DC) >> 23) & 1) != 0;
            const uint32_t raw = Ptr(body + 0x158);
            std::memcpy(&cur.alpha, &raw, 4);
        }
        ShowStuckGhost(i, cur.body, cur.bodyHidden, now);
        HeadState& was = heads[i];
        const float fade = cur.alpha > was.alpha ? cur.alpha - was.alpha : was.alpha - cur.alpha;
        if (cur.body == was.body && cur.helmet == was.helmet && cur.helmetHidden == was.helmetHidden && cur.bodyHidden == was.bodyHidden && cur.open == was.open &&
            !(fade >= 0.25f))
            continue;
        if (cur.body && InterlockedIncrement(&headChanges) <= 400)
            Line("head watch slot %u: body %08X%s helmet %08X (hidden %u) body hidden %u helmet %s alpha %.2f%s", i, cur.body,
                 IsBotEntity(cur.body) ? " (bot)" : IsGhostPlayer(cur.body) ? " (ghost)" : "", cur.helmet, cur.helmetHidden, cur.bodyHidden, cur.open ? "open" : "closed",
                 cur.alpha, cur.body != was.body ? " (new body)" : "");
        was = cur;
    }
}

DWORD entriesAt = 0;
// --- necro vision -----------------------------------------------------------------------------
// The necro players see the humans through walls as glowing nerves. Every Player has a
// NecroVisionComponent (vtable 0x01B76DE0), but the campaign Isaac's holds the class
// defaults (every effect the invalid BADBADBA GUID), so on a necro machine PlayVFX
// (0x010A9210) passed its one gate, DGM+0xF2 == 1, and played nothing (13.10.). The MP
// human template 34CA0FEE-ECDB4E92 (global_mp_assets, the killaura*.vfx resources the
// Versus pool brings) fills it; its values go into a block of our own under every human
// body on a necro machine. A body set up already is set up again: KillVFXAll(0) first,
// or the old handles would be dropped without being killed, then Initialize. Before the
// level or the side changes everything is killed and the old attributes come back.
// analysis/VERSUS_NECROVISION_BOTXP_2026-10-13.md
namespace necrovision {
constexpr uint32_t kIndexByte = 0x1CACE52, kVtable = 0x1776DE0, kInitialize = 0xCD2B10, kKillAll = 0xC72D50, kIsLinked = 0x349D60, kPostSpawnMsg = 0x1C4D5E4;
constexpr uint32_t kAttrs = 0x0C, kAttrSize = 0x1E0, kHandles = 0x30;   // handles: const[3], state[3], bone[15]
constexpr uint32_t kGuidConst[2][4] = {{0xF74C140Fu, 0xF74C140Fu, 0xF74C140Fu, 0x52706EB2u}, {0x29419093u, 0x29419093u, 0x29419093u, 0x68748C38u}};
constexpr uint32_t kGuidBone[13][4] = {
    {0x27DD4E91u, 0x27DD4E91u, 0x27DD4E91u, 0xD96AADADu}, {0x0134D6D0u, 0x0134D6D0u, 0x0134D6D0u, 0x1AE3BF25u}, {0x0134D6D0u, 0x0134D6D0u, 0x0134D6D0u, 0x1AE3BF25u},
    {0x33C47704u, 0x33C47704u, 0x33C47704u, 0xB6818CA1u}, {0xD3E374C8u, 0xD3E374C8u, 0xD3E374C8u, 0x51296760u}, {0x10A6A5BEu, 0x10A6A5BEu, 0x10A6A5BEu, 0xF6523F4Du},
    {0x10A6A5BEu, 0x10A6A5BEu, 0x10A6A5BEu, 0xF6523F4Du}, {0xB71DA623u, 0xB71DA623u, 0xB71DA623u, 0x8455A7F3u}, {0x4C15A3A6u, 0x4C15A3A6u, 0x4C15A3A6u, 0xD5EA3555u},
    {0x4C15A3A6u, 0x4C15A3A6u, 0x4C15A3A6u, 0xD5EA3555u}, {0x2E9F0A63u, 0x2E9F0A63u, 0x2E9F0A63u, 0x4E88A742u}, {0x9E9A3C5Eu, 0x9E9A3C5Eu, 0x9E9A3C5Eu, 0x05A829D2u},
    {0x9E9A3C5Eu, 0x9E9A3C5Eu, 0x9E9A3C5Eu, 0x05A829D2u}};
// chest, l/r elbow, head, hips, l/r knee, shoulders, l/r hip, torso, l/r shoulder (the engine's bone name hashes)
constexpr uint32_t kBoneHash[13] = {0x4E418B41u, 0x32674BF0u, 0x3768E2F6u, 0x26ABEA81u, 0xDD290F82u, 0x38594A76u, 0xBD6391F0u,
                                    0xD5A991F1u, 0x1F5EF1FCu, 0xB6E92C02u, 0x6CB25065u, 0xE4540033u, 0x0C5C8DADu};
constexpr uint32_t kHealthColor[3] = {0xD90000FFu, 0xFFE77BFFu, 0x6FFF8AFFu}, kColorSpecial = 0x3D3DFFFFu;
constexpr uint32_t kHealthFracBits[3] = {0x3E800000u, 0x3F2B851Fu, 0x3F800000u};   // 0.25, 0.67, 1.0
bool enabled = true;
// An attribute set sits behind a 16-byte header: type, two dwords, a 16-bit reference
// count at +0x0C and flags at +0x0E (AddRef 0x007CEA40, Release 0x007CEB60). A dying
// component releases [comp+0x0C] - 0x10, so the block carries the defaults' header with
// a count that never reaches 1. 13.10. 18:28: a bare block made Release take versus
// counters for the header and crash at 0x007CE939 when a uniform change rebuilt a body.
constexpr uint32_t kHeader = 0x10, kTypeSize = 0x4A, kRefs = 0x0C, kFlags = 0x0E;
constexpr uint16_t kPinnedRefs = 0x4000, kRefFloor = 0x2000;
constexpr uint8_t kReleaseFlags = 0x06;   // 2: free hook, 4: field walk; neither may run on our block
struct alignas(16) AttrSet { unsigned char header[kHeader]; unsigned char data[kAttrSize]; };
AttrSet attrSet{};
unsigned char (&block)[kAttrSize] = attrSet.data;
bool built = false, refused = false;
uint32_t headerType = 0;
uint16_t headerRefs = 0, headerFlags = 0;
struct Swap { uint32_t entity = 0, comp = 0, oldAttrs = 0; };
Swap swaps[16]{};
volatile LONG swapsDone = 0, preInit = 0, reinits = 0, restored = 0, notOurs = 0, noComponent = 0, faults = 0, liveHandles = 0, lines = 0;
volatile LONG released = 0, topUps = 0;
DWORD scanAt = 0;
bool active = false;

uint16_t Refs() {
    uint16_t refs = 0;
    std::memcpy(&refs, attrSet.header + kRefs, 2);
    return refs;
}
// The defaults' header, checked: a type inside the image whose size word is ours.
bool CopyHeader(uint32_t defaults) {
    unsigned char header[kHeader]{};
    uint16_t typeSize = 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const uint32_t imageEnd = static_cast<uint32_t>(base + nt->OptionalHeader.SizeOfImage);
    uint32_t type = 0;
    const bool read = defaults > kHeader && Read(defaults - kHeader, header, kHeader);
    if (read) std::memcpy(&type, header, 4);
    const bool typeOk = read && type >= base && type < imageEnd && Read(type + kTypeSize, &typeSize, 2) && typeSize == kAttrSize;
    std::memcpy(&headerRefs, header + kRefs, 2);
    std::memcpy(&headerFlags, header + kFlags, 2);
    headerType = type;
    if (!typeOk || !headerRefs) {
        refused = true;
        Line("necro vision OFF: the defaults at %08X have no usable header (read %d, type %08X, type size %04X, want %04X, refs %u, flags %04X)", defaults,
             read ? 1 : 0, type, static_cast<unsigned>(typeSize), kAttrSize, static_cast<unsigned>(headerRefs), static_cast<unsigned>(headerFlags));
        return false;
    }
    std::memcpy(attrSet.header, header, kHeader);
    std::memcpy(attrSet.header + kRefs, &kPinnedRefs, 2);
    attrSet.header[kFlags] = static_cast<unsigned char>(attrSet.header[kFlags] & ~kReleaseFlags);
    return true;
}
// Called before every scan: components that died with our block took a reference each.
void PinRefs() {
    const uint16_t refs = Refs();
    if (refs >= kRefFloor) return;
    std::memcpy(attrSet.header + kRefs, &kPinnedRefs, 2);
    InterlockedIncrement(&topUps);
    Line("necro vision: block references fell to %u, set back to %u", static_cast<unsigned>(refs), static_cast<unsigned>(kPinnedRefs));
}

bool Build(uint32_t defaults) {
    if (!CopyHeader(defaults)) return false;
    Read(defaults, block, sizeof(block));   // the class defaults keep every slot not set here invalid
    block[0] = 1;   // UseColor
    // a colour is four bytes R, G, B, A (UpdateProperty 0x010A8FC0 reads them one by one): the
    // catalogue's values are 0xRRGGBBAA, so they go in big-endian (13.10.: written as they
    // were, full health glowed magenta instead of green)
    const uint32_t special = _byteswap_ulong(kColorSpecial);
    std::memcpy(block + 0x04, &special, 4);
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t colour = _byteswap_ulong(kHealthColor[i]);
        std::memcpy(block + 0x08 + 4 * i, &colour, 4);
    }
    std::memcpy(block + 0x14, kHealthFracBits, sizeof(kHealthFracBits));
    std::memcpy(block + 0x24, kGuidConst, sizeof(kGuidConst));
    std::memcpy(block + 0xB4, kGuidBone, sizeof(kGuidBone));
    std::memcpy(block + 0x1A4, kBoneHash, sizeof(kBoneHash));
    built = true;
    Line("necro vision: attribute block %08X built from the defaults at %08X (2 constant and 13 bone effects of the MP human template); "
         "header type %08X refs %u flags %04X -> refs pinned at %u, flags %02X",
         static_cast<unsigned>(reinterpret_cast<uintptr_t>(block)), defaults, headerType, static_cast<unsigned>(headerRefs),
         static_cast<unsigned>(headerFlags), static_cast<unsigned>(kPinnedRefs), static_cast<unsigned>(attrSet.header[kFlags]));
    return true;
}
uint32_t Component(uint32_t entity) {
    uint32_t comp = 0;
    __try {
        comp = reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t)>(base + kGetComponent)(P(entity), Byte(base + kIndexByte));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return 0;
    }
    return comp && Ptr(comp) == base + kVtable ? comp : 0;
}
__declspec(noinline) bool Reinit(uint32_t comp, bool start) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, bool)>(base + kKillAll)(P(comp), false);
        if (start) reinterpret_cast<void(__thiscall*)(void*)>(base + kInitialize)(P(comp));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}
__declspec(noinline) bool Linked(uint32_t comp) {
    __try {
        return reinterpret_cast<bool(__thiscall*)(void*, uint32_t)>(base + kIsLinked)(P(comp), base + kPostSpawnMsg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return true;
    }
}
unsigned Handles(uint32_t comp) {
    unsigned n = 0;
    for (uint32_t i = 0; i < 21; ++i) n += Ptr(comp + kHandles + 4 * i) ? 1u : 0u;
    return n;
}
// Every body back to its own attributes, its effects killed. Only bodies that still
// carry our block are touched.
void RestoreAll(const char* why) {
    unsigned n = 0;
    for (Swap& s : swaps) {
        if (!s.comp) continue;
        if (Ptr(s.entity) == base + placement::kPlayerPrimaryVtableRva && Component(s.entity) == s.comp &&
            Ptr(s.comp + kAttrs) == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block))) {
            Reinit(s.comp, false);
            WriteBytes(s.comp + kAttrs, &s.oldAttrs, 4);
            ++n;
            InterlockedIncrement(&restored);
        }
        s = Swap{};
    }
    if (n || active) Line("necro vision off (%s): %u body(ies) back to their own attributes", why, n);
    active = false;
}
// A uniform change or a deletion destroys the component; its destructor released our block.
void Recycle() {
    for (Swap& s : swaps) {
        if (!s.comp) continue;
        if (Ptr(s.entity) == base + placement::kPlayerPrimaryVtableRva && Component(s.entity) == s.comp) continue;
        if (InterlockedIncrement(&released) <= 24)
            Line("necro vision: body %08X lost component %08X (rebuilt or deleted); block references now %u", s.entity, s.comp, static_cast<unsigned>(Refs()));
        s = Swap{};
    }
}
void Poll(DWORD now, bool running) {
    if (!enabled) return;
    const bool necro = running && Byte(Ptr(base + menu::kGameManager) + 0xF2) == mp::kNecro;
    if (!necro) { if (active) RestoreAll(running ? "this machine is no necro any more" : "the round or the level is changing"); return; }
    if (refused || now - scanAt < 500) return;
    scanAt = now;
    active = true;
    if (built) {
        PinRefs();
        Recycle();
    }
    uint32_t bodies[8];
    const unsigned n = nameplate::Targets(bodies, 8);
    const uint32_t mine = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block));
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t comp = Component(bodies[i]);
        if (!comp) { InterlockedIncrement(&noComponent); continue; }
        const uint32_t attrs = Ptr(comp + kAttrs);
        if (attrs == mine || !attrs) continue;
        if (!built && !Build(attrs)) return;
        Swap* slot = nullptr;
        for (Swap& s : swaps) if (!s.comp || s.comp == comp) { slot = &s; break; }
        if (!slot) { InterlockedIncrement(&notOurs); continue; }
        // still waiting for its post-spawn message: the native Initialize reads our block
        const bool pending = Linked(comp);
        if (!WriteBytes(comp + kAttrs, &mine, 4)) continue;
        *slot = Swap{bodies[i], comp, attrs};
        InterlockedIncrement(&swapsDone);
        if (pending) {
            InterlockedIncrement(&preInit);
        } else if (Reinit(comp, true)) {
            InterlockedIncrement(&reinits);
        }
        const unsigned handles = pending ? 0u : Handles(comp);
        InterlockedExchangeAdd(&liveHandles, static_cast<LONG>(handles));
        if (InterlockedIncrement(&lines) <= 24)
            Line("necro vision on body %08X (component %08X, own attributes %08X): %s, %u effect handle(s) live", bodies[i], comp, attrs,
                 pending ? "set before its own setup" : "set up again", handles);
    }
}
}  // namespace necrovision

void Tick(DWORD now) {
    if (!installed) return;
    vs::Packet batch[32];
    unsigned n = 0;
    AcquireSRWLockExclusive(&inboxLock);
    n = inboxCount;
    std::memcpy(batch, inbox, n * sizeof(vs::Packet));
    inboxCount = 0;
    ReleaseSRWLockExclusive(&inboxLock);
    for (unsigned i = 0; i < n; ++i) Handle(batch[i], now);
    // a client whose ask the host never answered comes back in place after 30 s
    if (askedAt && static_cast<LONG>(now - askedAt) > 30000) {   // signed: the hook stamps a fresh tick
        askedAt = 0;
        InterlockedIncrement(&askTimeouts);
        if (const uint32_t sel = Ptr(base + kSelector)) {
            __try {
                reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t)>(base + kPointResponse)(P(sel), 0u, 0u, 0u, 0u);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedIncrement(&faults);
            }
            Line("respawn: no answer from the host in 30 s; back in place (selector flags %02X)", Byte(sel + 0x24));
        }
    }
    // the native respawn put a new body in slot 0
    if (deadBody) {
        const uint32_t body = LocalCampaignPlayer();
        if (body && body != deadBody) { AfterRespawn(deadBody, body); deadBody = 0; }
        else if (now - deadAt > 120000) { Line("no new body 120 s after the native death of %08X", deadBody); deadBody = 0; }
    }
    PollBits(now);
    PollMusic();
    if (InterlockedExchange(&leaveWanted, 0)) {
        Line("quit from the online pause menu: back to the menu");
        mpmap::ReturnToCampaign(now, "quit from the online pause menu");
    }
    PollRounds(now);
    necrovision::Poll(now, OnMap() && Ptr(Ptr(base + kLevelManager) + 0x18) == 0x10 && !Restarting() && !LevelChangePending());
    if (OnMap() && Ptr(Ptr(base + kLevelManager) + 0x18) == 0x10 && !Restarting()) {
        if (cfg.killerCard) EnsureRelay();
        if (cfg.scoring && now - entriesAt >= 1000) { entriesAt = now; SyncEntries(); }
        PollScoreKey();
        PollGroups(now);
        WatchHeads(now);
    } else {
        // a restart or a new level rebuilds every body: join them afresh
        for (auto& j : joined) j = Joined{};
    }
    Flush(now);
}

// --- pickups under the MP bit ------------------------------------------------------------------
// With the MP bit a dropped pickup registers with EnemyLifetimeManager::AddEntity
// (PickupItem::UpdatePickupRegistrationState, PC 0x00DB439A; thiscall(type, entity,
// a, b), ret 0x10). Its queues are sized from tuning that never comes offline, so
// capacity 0 counts as full and the oldest entry is popped off a null array (11.10.
// 23:34 and 23:38, host: a Versus slasher's loot). A queue never sized keeps
// nothing, as in the campaign; RemoveEntity walks the count and stays safe.
safetyhook::InlineHook lifetimeHook;
volatile LONG lifetimeAdds = 0, lifetimeSkipped = 0;
void __fastcall OnLifetimeAdd(void* self, void*, uint32_t type, uint32_t entity, uint32_t a, uint32_t b) {
    InterlockedIncrement(&lifetimeAdds);
    uint32_t capacity = 0;
    const uint32_t queue = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)) + type * 24 + 0x10;
    if (type < 16 && Read(queue + 4, &capacity, 4) && !capacity) {
        if (InterlockedIncrement(&lifetimeSkipped) <= 5) Line("pickup lifetime: queue %u was never sized, entity %08X kept out of it", type, entity);
        return;
    }
    lifetimeHook.thiscall<void>(self, type, entity, a, b);
}

// --- MP loot ----------------------------------------------------------------------------------
// [Versus] MpLoot: the pool slashers drop what the maps' own slashers do (list
// 37CA2B52: 70 %, MultiAmmo 70 / Health Kit 25 / Stasis Battery 5), not their
// campaign lists with credits and gun ammo. ItemDropListComponent::
// HandlePostSpawnInit (0x010970B0, thiscall, ret 0) spawns the list from the GUID
// eax points to at 0x010970D0 (edi the component, [edi+0x14] its owner); the mid
// hook points eax at the MP list. Under the MP bit a list has no sub-lists and no
// gun ammo path, so the odds are MP's. On every machine: a client's copy of a dead
// enemy runs DropItem too (13.10.: 43 calls on the client) and drops its own loot,
// as pickups are not ghosted; with the campaign list it dropped nothing.
bool mpLoot = true;
safetyhook::MidHook dropListHook;
const uint32_t kMpDropList[4] = {0x37CA2B52, 0x2F7A9A31, 0x52524143, 0x4F4C4C49};
const uint32_t kNecroDropList[4] = {0x37CA4774, 0x36E82F90, 0x52524143, 0x4F4C4C49};
const uint32_t kUnauthoredList[4] = {0xBADBADBA, 0xBEEFBEEF, 0xEAC15A55, 0x91100911};
volatile LONG dropLists = 0, dropListsSwapped = 0, dropListsKept = 0, dropListLines = 0;
void OnDropListSpawn(safetyhook::Context& ctx) {
    InterlockedIncrement(&dropLists);
    if (!mpLoot || !mpnative::InVersus() || !mpmap::InVisit()) return;
    const uint32_t at = static_cast<uint32_t>(ctx.eax), component = static_cast<uint32_t>(ctx.edi);
    uint32_t guid[4]{}, owner = 0, vtable = 0;
    if (!Read(at, guid, sizeof(guid))) return;
    Read(component + 0x14, &owner, 4);
    if (owner) Read(owner, &vtable, 4);
    const char* keep = std::memcmp(guid, kUnauthoredList, 16) == 0 ? "unauthored"
                       : std::memcmp(guid, kMpDropList, 16) == 0 || std::memcmp(guid, kNecroDropList, 16) == 0 ? "already MP"
                       : IsPlayerOrNecroEntity(owner) ? "a player's" : nullptr;
    if (keep) {
        InterlockedIncrement(&dropListsKept);
        if (InterlockedIncrement(&dropListLines) <= 30)
            Line("loot: drop list %08X-%08X of %08X (vtable RVA %08X) kept (%s)", guid[0], guid[3], owner, vtable ? vtable - static_cast<uint32_t>(base) : 0u, keep);
        return;
    }
    ctx.eax = reinterpret_cast<uintptr_t>(kMpDropList);
    InterlockedIncrement(&dropListsSwapped);
    if (InterlockedIncrement(&dropListLines) <= 30)
        Line("loot: drop list %08X-%08X of %08X (vtable RVA %08X) -> the MP slashers' list 37CA2B52", guid[0], guid[3], owner,
             vtable ? vtable - static_cast<uint32_t>(base) : 0u);
}
// The MP universal ammo matches a gun by its proxy GUID (Gun+0x404) against the MP
// guns' proxies only, and answers 8 (none) for the campaign guns our players carry.
// MultiAmmo::FindIndexForGun (0x00DA8930, thiscall(gun), ret 4) is asked again by
// the gun's class; CanPickup and Pickup then give the gun's own ammo up to the MP
// amount, looked up by its own proxy. The guns MP never had take a stand-in slot
// ([Versus] AmmoSlot..., -1 none).
struct GunSlot { uint32_t vtableRva; const char* key; const char* name; int slot; };
GunSlot gunSlots[] = {
    {0x17515D8, nullptr, "plasma cutter", 0}, {0x1751A00, nullptr, "pulse rifle", 1}, {0x17511B8, nullptr, "line gun", 2},
    {0x1753948, nullptr, "force gun", 3},     {0x1750D98, nullptr, "javelin", 4},     {0x1773780, nullptr, "seeker rifle", 5},
    {0x1752580, "AmmoSlotRipper", "ripper", 2}, {0x1753520, "AmmoSlotFlamethrower", "flamethrower", 1},
    {0x1753100, "AmmoSlotDetonator", "detonator", 2}, {0x1752CE0, "AmmoSlotContactBeam", "contact beam", 4},
};
safetyhook::InlineHook ammoSlotHook;
volatile LONG ammoAsks = 0, ammoMapped = 0, ammoUnmapped = 0, ammoEmptySlot = 0, ammoLines = 0;
uint32_t __fastcall OnFindIndexForGun(void* self, void*, uint32_t gun) {
    const uint32_t slot = ammoSlotHook.thiscall<uint32_t>(self, gun);
    InterlockedIncrement(&ammoAsks);
    if (slot != 8 || !mpLoot || !gun || !mpnative::InVersus() || !mpmap::InVisit()) return slot;
    uint32_t vtable = 0;
    Read(gun, &vtable, 4);
    const GunSlot* g = nullptr;
    for (const GunSlot& s : gunSlots)
        if (vtable == base + s.vtableRva) { g = &s; break; }
    uint32_t proxy[4]{};
    Read(gun + 0x404, proxy, sizeof(proxy));
    if (!g || g->slot < 0 || g->slot > 7) {
        InterlockedIncrement(&ammoUnmapped);
        if (InterlockedIncrement(&ammoLines) <= 20)
            Line("loot: universal ammo has no slot for gun %08X (vtable RVA %08X, %s, proxy %08X-%08X)", gun, vtable - static_cast<uint32_t>(base),
                 g ? g->name : "unknown class", proxy[0], proxy[1]);
        return slot;
    }
    // only a slot the pack fills: a proxy GUID there and a quantity
    uint32_t attrs = 0, entry[5]{};
    const uint32_t me = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    if (!Read(me + 0xC, &attrs, 4) || !attrs || !Read(attrs + 0x1B0 + static_cast<uint32_t>(g->slot) * 0x18, entry, sizeof(entry)) ||
        !(entry[0] | entry[1] | entry[2] | entry[3]) || static_cast<int32_t>(entry[4]) <= 0) {
        InterlockedIncrement(&ammoEmptySlot);
        if (InterlockedIncrement(&ammoLines) <= 20) Line("loot: universal ammo slot %d for the %s is empty in this pack (%08X)", g->slot, g->name, me);
        return slot;
    }
    InterlockedIncrement(&ammoMapped);
    if (InterlockedIncrement(&ammoLines) <= 20)
        Line("loot: universal ammo for the %s (proxy %08X-%08X) through MP slot %d (quantity word %08X)", g->name, proxy[0], proxy[1], g->slot, entry[4]);
    return static_cast<uint32_t>(g->slot);
}

// --- install ----------------------------------------------------------------------------------
size_t Hex(const char* hex, unsigned char* out, size_t cap) { return nativelobby::Hex(hex, out, cap); }
bool Hook(FILE* log, const char* name, uint32_t rva, void* fn, safetyhook::InlineHook& hook) {
    for (const Sig& s : kSigs) {
        if (s.rva != rva) continue;
        unsigned char bytes[32]{};
        const size_t n = Hex(s.hex, bytes, sizeof(bytes));
        char who[96];
        std::snprintf(who, sizeof(who), "versus %s", name);
        return hook_site::Install(log, who, base + rva, bytes, n, fn, hook);
    }
    return false;
}
void Install(FILE* log, const std::string& ini) {
    const char* f = ini.c_str();
    cfg.mpBits = GetPrivateProfileIntA("Versus", "MpBits", 1, f) != 0;
    cfg.scoring = GetPrivateProfileIntA("Versus", "Scoring", 1, f) != 0;
    cfg.killerCard = GetPrivateProfileIntA("Versus", "KillerCard", 1, f) != 0;
    cfg.rounds = GetPrivateProfileIntA("Versus", "Rounds", 1, f) != 0;
    cfg.nativeRespawn = GetPrivateProfileIntA("Versus", "NativeRespawn", 1, f) != 0;
    cfg.teamGroups = GetPrivateProfileIntA("Versus", "TeamGroups", 1, f) != 0;
    cfg.inventory = GetPrivateProfileIntA("Versus", "Inventory", 0, f) != 0;   // 0: the scoreboard owns Tab (12.10.)
    cfg.scoreKey = GetPrivateProfileIntA("Versus", "ScoreKey", VK_TAB, f);
    cfg.music = GetPrivateProfileIntA("Versus", "Music", 1, f) != 0;
    necrovision::enabled = GetPrivateProfileIntA("Versus", "NecroVision", 1, f) != 0;
    cfg.nativeAudio = GetPrivateProfileIntA("Versus", "NativeAudio", 1, f) != 0;
    cfg.combatText = GetPrivateProfileIntA("Versus", "CombatText", 1, f) != 0;
    mpLoot = GetPrivateProfileIntA("Versus", "MpLoot", 1, f) != 0;
    for (GunSlot& g : gunSlots)
        if (g.key) g.slot = GetPrivateProfileIntA("Versus", g.key, g.slot, f);
    if (cfg.nativeAudio || cfg.combatText) { mpmap::versusPassesClass = PassesClass; necro::versusPassesClass = PassesClass; }
    cfg.swapMs = static_cast<DWORD>(GetPrivateProfileIntA("Versus", "SwapSeconds", 12, f)) * 1000;
    cfg.resultsMs = static_cast<DWORD>(GetPrivateProfileIntA("Versus", "ResultsSeconds", 20, f)) * 1000;
    if (cfg.swapMs < 3000) cfg.swapMs = 3000;
    if (cfg.resultsMs < 3000) cfg.resultsMs = 3000;
    for (vs::Points& p : vs::kPoints) p.value = GetPrivateProfileIntA("VersusPoints", p.key, p.value, f);
    cfg.roundWin = GetPrivateProfileIntA("VersusPoints", "RoundWin", 1000, f);
    cfg.bothRounds = GetPrivateProfileIntA("VersusPoints", "BothRounds", 2000, f);
    if (!mpnative::enabled) { std::fprintf(log, "versus match off ([Versus] Enabled=0)\n"); return; }
    bool all = true;
    for (size_t i = 0; i < std::size(kSigs); ++i) {
        unsigned char want[32]{}, have[32]{};
        const size_t n = Hex(kSigs[i].hex, want, sizeof(want));
        sigOk[i] = n && Read(base + kSigs[i].rva, have, n) && std::memcmp(have, want, n) == 0;
        if (!sigOk[i]) {
            all = false;
            std::fprintf(log, "versus sig %-46s %08X MISMATCH:", kSigs[i].name, static_cast<unsigned>(base + kSigs[i].rva));
            for (size_t k = 0; k < n; ++k) std::fprintf(log, " %02X", have[k]);
            std::fprintf(log, "\n");
        }
    }
    std::fprintf(log, "versus sigs: %s\n", all ? "all match" : "MISMATCHES above; the parts that need them stay off");
    // a part runs only with every function it calls in place
    if (cfg.mpBits && !(SigOk(kResetForRestart) && SigOk(kCheckpointLoadComplete))) cfg.mpBits = false;
    if (cfg.nativeRespawn && !(cfg.mpBits && SigOk(kRequestPoint) && SigOk(kIssuePoint) && SigOk(kSelectorCtor) && SigOk(kResetInitialPoints) && SigOk(kQueueRespawn) &&
                               SigOk(kSafePtrAssign) && SigOk(kPointResponse)))
        cfg.nativeRespawn = false;
    if (cfg.scoring && !(SigOk(kEntryEvent) && SigOk(kScoreListenerOpen) && SigOk(kEntryCtor) && SigOk(kFindEntry) && SigOk(kVictimInfo) && SigOk(kAttackerInfo) &&
                         SigOk(kTuning) && SigOk(kCombatTextFn) && SigOk(kTranslate) && SigOk(kRegisterCombatText) && SigOk(kStringAssign)))
        cfg.scoring = false;
    if (cfg.killerCard && !(SigOk(kSetKillerInfo) && SigOk(kRelayCtor) && SigOk(kStringAssign))) cfg.killerCard = false;
    if (cfg.rounds && !(SigOk(kActivate) && SigOk(kHideScoring) && SigOk(kHideEndRound) && SigOk(kShutdownCamera) && SigOk(kTeamChange) && SigOk(kScoreForRound)))
        cfg.rounds = false;
    if (cfg.teamGroups && !(SigOk(kAddToGroup) && SigOk(kRemoveFromGroup) && SigOk(kGetComponent))) cfg.teamGroups = false;
    if (cfg.inventory && !(cfg.mpBits && SigOk(kCheckStartTest))) cfg.inventory = false;
    if (cfg.inventory) {
        unsigned char bytes[16]{};
        const size_t n = Hex("32 C0 F6 C2 01 0F 85 E3 00 00 00", bytes, sizeof(bytes));
        cfg.inventory = hook_site::InstallMid(log, "versus CheckStart MP test", base + kCheckStartTest, bytes, n, OnCheckStart, checkStartHook);
    }
    if (cfg.mpBits) {
        cfg.mpBits = Hook(log, "ResetForQuickLevelRestart", kResetForRestart, reinterpret_cast<void*>(OnResetForRestart), resetHook) &&
                     Hook(log, "CheckpointLoadComplete", kCheckpointLoadComplete, reinterpret_cast<void*>(OnCheckpointLoadComplete), loadCompleteHook);
        if (cfg.mpBits && SigOk(kLifetimeAdd)) Hook(log, "EnemyLifetimeManager::AddEntity", kLifetimeAdd, reinterpret_cast<void*>(OnLifetimeAdd), lifetimeHook);
    }
    if (mpLoot && SigOk(kDropListSpawn) && SigOk(kFindIndexForGun)) {
        unsigned char bytes[16]{};
        const size_t n = Hex("56 8B 35 A0 C0 01 02", bytes, sizeof(bytes));
        mpLoot = hook_site::InstallMid(log, "versus drop list spawn", base + kDropListSpawn, bytes, n, OnDropListSpawn, dropListHook) &&
                 Hook(log, "MultiAmmo::FindIndexForGun", kFindIndexForGun, reinterpret_cast<void*>(OnFindIndexForGun), ammoSlotHook);
    } else {
        mpLoot = false;
    }
    if (cfg.nativeRespawn) {
        cfg.nativeRespawn = Hook(log, "RequestRespawnPoint", kRequestPoint, reinterpret_cast<void*>(OnRequestPoint), requestHook) &&
                            Hook(log, "IssueSpawnPoint", kIssuePoint, reinterpret_cast<void*>(OnIssuePoint), issueHook);
    }
    if (cfg.scoring) {
        cfg.scoring = Hook(log, "LogScoringEvent", kEntryEvent, reinterpret_cast<void*>(OnEntryEvent), entryEventHook) &&
                      Hook(log, "GetVictimInfo", kVictimInfo, reinterpret_cast<void*>(OnVictimInfo), victimHook) &&
                      Hook(log, "GetAttackerInfo", kAttackerInfo, reinterpret_cast<void*>(OnAttackerInfo), attackerHook) &&
                      Hook(log, "GetMPTuningValue", kTuning, reinterpret_cast<void*>(OnTuning), tuningHook) &&
                      Hook(log, "HandleCombatText", kCombatTextFn, reinterpret_cast<void*>(OnCombatText), combatTextHook);
    }
    if (cfg.killerCard) cfg.killerCard = Hook(log, "SetKillerInfo", kSetKillerInfo, reinterpret_cast<void*>(OnKillerInfo), killerHook);
    if (cfg.rounds) cfg.rounds = Hook(log, "MPEndOfGameSequencer::Activate", kActivate, reinterpret_cast<void*>(OnActivate), activateHook);
    if (cfg.mpBits && SigOk(kPauseSelect) && SigOk(kPauseQuit)) {
        menu::SwapSlot(log, "MP10PauseMenu OnInput_Select", kPauseSelectSlot, kPauseSelect, reinterpret_cast<void*>(OnPauseSelect));
        menu::SwapSlot(log, "MP10PauseMenu quit result", kPauseQuitSlot, kPauseQuit, reinterpret_cast<void*>(OnPauseQuit));
    }
    installed = true;
    std::fprintf(log, "versus match: mp_bits %s, scoring %s, killer card %s, rounds %s (swap %lu s, results %lu s), native respawn %s, team groups %s, inventory %s\n",
                 cfg.mpBits ? "on" : "off", cfg.scoring ? "on" : "off", cfg.killerCard ? "on" : "off", cfg.rounds ? "on" : "off",
                 static_cast<unsigned long>(cfg.swapMs / 1000), static_cast<unsigned long>(cfg.resultsMs / 1000), cfg.nativeRespawn ? "on" : "off",
                 cfg.teamGroups ? "on" : "off", cfg.inventory ? "kept" : "MP rule (closed)");
    std::fprintf(log, "versus points:");
    for (const vs::Points& p : vs::kPoints) std::fprintf(log, " %s=%d", p.key, p.value);
    std::fprintf(log, "\n");
    std::fprintf(log, "versus mp_loot %s (drop list hook %d, universal ammo hook %d) stand-in slots:", mpLoot ? "on" : "off", dropListHook ? 1 : 0, ammoSlotHook ? 1 : 0);
    for (const GunSlot& g : gunSlots)
        if (g.key) std::fprintf(log, " %s=%d", g.name, g.slot);
    std::fprintf(log, "\n");
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG sum = InterlockedCompareExchange(&bitsSet, 0, 0) * 3 + InterlockedCompareExchange(&bitsCleared, 0, 0) * 5 + InterlockedCompareExchange(&received, 0, 0) +
                     InterlockedCompareExchange(&sent, 0, 0) + InterlockedCompareExchange(&eventsLocal, 0, 0) * 7 + InterlockedCompareExchange(&killerCards, 0, 0) * 11 +
                     InterlockedCompareExchange(&activations, 0, 0) * 13 + InterlockedCompareExchange(&groupJoins, 0, 0) * 17 +
                     InterlockedCompareExchange(&nativeDeaths, 0, 0) * 19 + InterlockedCompareExchange(&faults, 0, 0) * 23 + InterlockedCompareExchange(&entriesBuilt, 0, 0) * 29 +
                     InterlockedCompareExchange(&dropListsSwapped, 0, 0) * 31 + InterlockedCompareExchange(&ammoMapped, 0, 0) * 37 +
                     InterlockedCompareExchange(&ammoUnmapped, 0, 0) * 41;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "versus bits ours=%d set=%ld cleared=%ld late_clears=%ld timer_zeroes=%ld body_flags=%ld inventory_opens=%ld faults=%ld\n", bitsOurs ? 1 : 0,
                 InterlockedCompareExchange(&bitsSet, 0, 0), InterlockedCompareExchange(&bitsCleared, 0, 0), InterlockedCompareExchange(&lateClears, 0, 0),
                 InterlockedCompareExchange(&timerZeroes, 0, 0), InterlockedCompareExchange(&bodyFlags, 0, 0), InterlockedCompareExchange(&inventoryOpens, 0, 0),
                 InterlockedCompareExchange(&faults, 0, 0));
    std::fprintf(log, "versus traffic sent=%ld received=%ld duplicates=%ld refused=%ld in:", InterlockedCompareExchange(&sent, 0, 0),
                 InterlockedCompareExchange(&received, 0, 0), InterlockedCompareExchange(&duplicates, 0, 0), InterlockedCompareExchange(&refused, 0, 0));
    for (uint8_t k = 1; k <= 8; ++k) std::fprintf(log, " %s=%ld", vs::KindName(k), InterlockedCompareExchange(&kindsIn[k], 0, 0));
    std::fprintf(log, "\n");
    std::fprintf(log, "versus scoring listener_opens=%ld entries=%ld renames=%ld events local=%ld forwarded=%ld applied=%ld replayed=%ld dropped=%ld victim_fixes=%ld "
                      "attacker_fixes=%ld tuning=%ld combat_text=%ld skipped=%ld scoreboard shows=%ld refused=%ld hides=%ld\n",
                 InterlockedCompareExchange(&listenerOpens, 0, 0), InterlockedCompareExchange(&entriesBuilt, 0, 0), InterlockedCompareExchange(&renames, 0, 0),
                 InterlockedCompareExchange(&eventsLocal, 0, 0), InterlockedCompareExchange(&eventsForwarded, 0, 0), InterlockedCompareExchange(&eventsApplied, 0, 0),
                 InterlockedCompareExchange(&eventsReplayed, 0, 0), InterlockedCompareExchange(&eventsDropped, 0, 0), InterlockedCompareExchange(&victimFixes, 0, 0),
                 InterlockedCompareExchange(&attackerFixes, 0, 0), InterlockedCompareExchange(&tuningAnswers, 0, 0), InterlockedCompareExchange(&combatTexts, 0, 0),
                 InterlockedCompareExchange(&combatSkipped, 0, 0), InterlockedCompareExchange(&scoreShows, 0, 0), InterlockedCompareExchange(&scoreRefused, 0, 0),
                 InterlockedCompareExchange(&scoreHides, 0, 0));
    std::fprintf(log, "versus killer relays=%ld cards=%ld named=%ld self=%ld world=%ld | rounds activations=%ld ignored_local=%ld ends=%ld swaps=%ld restarts=%ld overs=%ld "
                      "round=%u stage=%u winners=%d/%d\n",
                 InterlockedCompareExchange(&relaysBuilt, 0, 0), InterlockedCompareExchange(&killerCards, 0, 0), InterlockedCompareExchange(&killerNamed, 0, 0),
                 InterlockedCompareExchange(&killerSelf, 0, 0), InterlockedCompareExchange(&killerAi, 0, 0), InterlockedCompareExchange(&activations, 0, 0),
                 InterlockedCompareExchange(&localEndsIgnored, 0, 0), InterlockedCompareExchange(&roundEnds, 0, 0), InterlockedCompareExchange(&swaps, 0, 0),
                 InterlockedCompareExchange(&restartsPulled, 0, 0), InterlockedCompareExchange(&matchOvers, 0, 0), match.round, static_cast<unsigned>(match.stage),
                 match.winner[1], match.winner[2]);
    std::fprintf(log, "versus bot_kills credited=%ld no_hit=%ld stale=%ld not_necro=%ld no_entry=%ld not_host=%ld\n", InterlockedCompareExchange(&botKillsCredited, 0, 0),
                 InterlockedCompareExchange(&botKillsNoHit, 0, 0), InterlockedCompareExchange(&botKillsStale, 0, 0), InterlockedCompareExchange(&botKillsNotNecro, 0, 0),
                 InterlockedCompareExchange(&botKillsNoEntry, 0, 0), InterlockedCompareExchange(&botKillsNotHost, 0, 0));
    std::fprintf(log, "versus necro_vision enabled=%d active=%d block=%d header_refused=%d swaps=%ld before_setup=%ld set_up_again=%ld restored=%ld no_component=%ld table_full=%ld "
                      "handles_seen=%ld faults=%ld | header type=%08X refs=%u (pinned %u) components_released=%ld top_ups=%ld\n",
                 necrovision::enabled ? 1 : 0, necrovision::active ? 1 : 0, necrovision::built ? 1 : 0, necrovision::refused ? 1 : 0,
                 InterlockedCompareExchange(&necrovision::swapsDone, 0, 0),
                 InterlockedCompareExchange(&necrovision::preInit, 0, 0), InterlockedCompareExchange(&necrovision::reinits, 0, 0),
                 InterlockedCompareExchange(&necrovision::restored, 0, 0), InterlockedCompareExchange(&necrovision::noComponent, 0, 0),
                 InterlockedCompareExchange(&necrovision::notOurs, 0, 0), InterlockedCompareExchange(&necrovision::liveHandles, 0, 0),
                 InterlockedCompareExchange(&necrovision::faults, 0, 0), necrovision::headerType, static_cast<unsigned>(necrovision::Refs()),
                 static_cast<unsigned>(necrovision::kPinnedRefs),
                 InterlockedCompareExchange(&necrovision::released, 0, 0), InterlockedCompareExchange(&necrovision::topUps, 0, 0));
    std::fprintf(log, "versus pause menu friends_refused=%ld quits=%ld | music changes=%ld state=%d | native audio %s combat text %s passed=%ld | "
                      "round kills humans=%ld necros=%ld (director asks %ld) | pickup lifetime adds=%ld kept_out=%ld hook=%d\n",
                 InterlockedCompareExchange(&pauseFriendsRefused, 0, 0), InterlockedCompareExchange(&pauseQuits, 0, 0), InterlockedCompareExchange(&musicChanges, 0, 0),
                 musicState, cfg.nativeAudio ? "on" : "off", cfg.combatText ? "on" : "off", InterlockedCompareExchange(&mpmap::versusAudioPassed, 0, 0),
                 InterlockedCompareExchange(&roundKills[0], 0, 0), InterlockedCompareExchange(&roundKills[1], 0, 0), InterlockedCompareExchange(&roundInfoAsks, 0, 0),
                 InterlockedCompareExchange(&lifetimeAdds, 0, 0), InterlockedCompareExchange(&lifetimeSkipped, 0, 0), lifetimeHook ? 1 : 0);
    std::fprintf(log, "versus respawn deaths=%ld respawns=%ld selectors built=%ld deleted=%ld asks queued=%ld sent=%ld points local=%ld sent=%ld in=%ld delays_in=%ld | "
                      "ask_timeouts=%ld groups joins=%ld foreign_deaths_refused=%ld respawn_module_skips=%ld\n",
                 InterlockedCompareExchange(&nativeDeaths, 0, 0), InterlockedCompareExchange(&nativeRespawns, 0, 0), InterlockedCompareExchange(&selectorsBuilt, 0, 0),
                 InterlockedCompareExchange(&selectorsDeleted, 0, 0), InterlockedCompareExchange(&asksQueued, 0, 0), InterlockedCompareExchange(&asksSent, 0, 0),
                 InterlockedCompareExchange(&pointsLocal, 0, 0), InterlockedCompareExchange(&pointsSent, 0, 0), InterlockedCompareExchange(&pointsIn, 0, 0),
                 InterlockedCompareExchange(&delaysIn, 0, 0), InterlockedCompareExchange(&askTimeouts, 0, 0), InterlockedCompareExchange(&groupJoins, 0, 0),
                 InterlockedCompareExchange(&foreignDeaths, 0, 0), InterlockedCompareExchange(&respawn::versusSkips, 0, 0));
    std::fprintf(log, "versus foreign respawn asks dropped=%ld | hidden live ghosts shown=%ld faults=%ld\n", InterlockedCompareExchange(&foreignAsks, 0, 0),
                 InterlockedCompareExchange(&ghostsShown, 0, 0), InterlockedCompareExchange(&ghostShowFaults, 0, 0));
    std::fprintf(log, "versus loot drop_lists=%ld swapped=%ld kept=%ld | universal ammo asks=%ld mapped=%ld no_slot=%ld empty_slot=%ld\n",
                 InterlockedCompareExchange(&dropLists, 0, 0), InterlockedCompareExchange(&dropListsSwapped, 0, 0), InterlockedCompareExchange(&dropListsKept, 0, 0),
                 InterlockedCompareExchange(&ammoAsks, 0, 0), InterlockedCompareExchange(&ammoMapped, 0, 0), InterlockedCompareExchange(&ammoUnmapped, 0, 0),
                 InterlockedCompareExchange(&ammoEmptySlot, 0, 0));
}
}  // namespace versus
