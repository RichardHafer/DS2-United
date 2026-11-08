// Dead Space 2 United by Hafer.
// Level follow: a client that joins plays where the host plays.
//
// Eight-player run, 2026-09-17: one instance had been started from a different
// save. It connected fine and stood in a different chapter for the whole
// session. The checkpoint module cannot help there: it applies a checkpoint
// GUID by looking the object up in the loaded level, and the object is in
// another level.
//
// The level script's own chapter change is the model. LevelChange::Activate
// (PC 0x00EA5F40, from the node's attributes CMD_SetMap +0xC, CMD_SetStream
// +0x10, CMD_SetCheckpoint +0x14, CMD_SetClearAllPersistentData +0x18) does:
//     Checkpoint::SetOverrideCheckpoint(&guid)        0x00B757B0, cdecl, writes 0x02091820
//     [0x02021300]->ClearPersistent(clearAll ? 7 : 1) 0x007B6950, thiscall(mask)
//     Checkpoint::StoreLevel()                         0x00B75880 (an autosave of kind 6)
//     GameMainLoop::RequestLevelChange(map, stream, clearCheckpoint=1, 0)
//                                                      0x0040F870, thiscall, ret 0x10
// where GameMainLoop is the service the registry at [0x0201C09C] returns for
// hash("GameMainLoop") (0x00417DB0: count at +0x88, 8-byte {hash, object}
// entries from +8). RequestLevelChange copies the names into the service
// (+4 map, +0x84 stream, +0x184), clears the death checkpoint, and refuses
// while a change is already pending (0x0042E6C0). After the load, Find
// returns the override GUID first, so the level starts at that checkpoint.
//
// So the host announces {map, stream, death checkpoint} once a second. A
// client whose map differs asks for the same change the script would. Only
// map changes are followed, even on joining: a different checkpoint in the
// same map is left to the checkpoint module, so the host crossing a
// checkpoint reloads nobody.
//
// From the main menu (12.10.): both players went back to it and the host
// started a new game. The client's session was still connected and his level
// packets kept arriving, but following only ran inside a level, and the menu's
// join refused ("one session per game start"). Now a client in the main menu
// follows when the host has been through the menu himself and loads a level
// again (the epoch moves on), with the menu lobby's campaign start; the menu's
// join item does the same at once.
namespace mpmap { bool StartCampaignFromMenu(const char* map, const char* stream, const uint32_t* checkpoint); }   // runtime_coop_mpmap.inc
namespace arena { bool RunningNow(); extern volatile LONG stateMirror; }   // runtime_coop_arena.inc: 1 launching, 2 running
namespace mpmap { extern bool campaignClientHold; bool InVisit(); }   // runtime_coop_mpmap.inc: set by every campaign launch from the menu
namespace loadsync { bool HostLoading(DWORD now, char* hostMap, size_t size); void ArmJoinSuitForCheckpoint(const uint32_t* checkpoint, const char* why); }   // runtime_coop_loadsync.inc
namespace level {
bool enabled = false, followHost = true, installed = false;
constexpr uint32_t kServiceRegistry = 0x1C1C09C;   // [0x0201C09C]
constexpr uint32_t kFindService = 0x17DB0;          // 0x00417DB0
constexpr uint32_t kRequestLevelChange = 0xF870;    // 0x0040F870
constexpr uint32_t kSetOverride = 0x7757B0;         // 0x00B757B0
constexpr uint32_t kPersistentStore = 0x1C21300;    // [0x02021300]
constexpr uint32_t kClearPersistent = 0x3B6950;     // 0x007B6950
constexpr uint32_t kDeathGuid = 0x1C8FCC4;          // 0x0208FCC4
constexpr uint32_t kOverrideGuid = 0x1C91820;
constexpr uint32_t kMapOffset = 4, kStreamOffset = 0x84;

constexpr uint32_t kMagic = 0x4C56454C;   // 'LEVL'
constexpr uint32_t kVersion = 1;
// flags: the host left the match; every partner goes back to the main menu (matchend)
constexpr uint32_t kFlagEnded = 1;
// A client in the main menu says so once a second (user, 17.10.: the host's door waited for a
// partner who had quit to the menu; his side channel still lived, so the gate counted him).
constexpr uint32_t kFlagInMenu = 2;
// The host plays a campaign arena: its death checkpoint is empty, the lobby launches the arena.
constexpr uint32_t kFlagArena = 4;
// With kFlagInMenu: this client chose join co-op and comes in at the host's next checkpoint restart.
constexpr uint32_t kFlagWantsJoin = 8;
// A story client comes in from the main menu only while the host is in a coordinated load (his
// checkpoint restart or level load), never into his running level ([Level] JoinOnHostRestart).
bool joinOnHostRestart = true;
volatile LONG joinWaits = 0, joinNotices = 0;
struct Packet {
    uint32_t magic, version, token, sequence, epoch, flags;
    char map[64];
    char stream[64];
    uint32_t checkpoint[4];
};
static_assert(sizeof(Packet) == 168, "Unexpected level wire layout");
bool Valid(const Packet& p) {
    return p.magic == kMagic && p.version == kVersion && p.token && p.map[63] == 0 && p.stream[63] == 0;
}

// Host side.
uint32_t sequence = 0, epoch = 0;
char lastMap[64]{};
DWORD lastSend = 0;
// Client side.
Packet wanted{};
uint32_t hostLevelToken = 0, hostLevelSequence = 0;
bool havePacket = false, joined = false;
uint32_t followedEpoch = 0;
// The main menu: the host counts a pass through it as a new epoch; a client
// there remembers the epoch it saw when it arrived.
bool followFromMenu = false;   // [Level] FollowFromMenu
bool hostSawMenu = false, inMenu = false;
uint32_t menuEpoch = 0, menuFollowedEpoch = 0;
// A client loads the host's level from the main menu only after "join co-op" (user, 17.10.: a
// copy started by a DS2COOP launcher was pulled into the host's story level on its own, before
// its intro had even begun, and could not choose to come as a necro). Once armed it follows
// whenever the host's level is known, also a host that starts later.
bool joinArmed = false;
volatile LONG unarmedSkips = 0, sameMapJoins = 0;
void ArmJoin() {
    joinArmed = true;
    if (inMenu) menuEpoch = 0;   // the host's current level counts as new
}
DWORD packetAt = 0, lastMenuTry = 0;
volatile LONG menuFollows = 0, menuFollowsRefused = 0;
DWORD lastRequest = 0;

volatile LONG sent = 0, received = 0, requests = 0, refused = 0, faults = 0, mismatches = 0, mpSkips = 0;
volatile LONG hostEnded = 0, endedSent = 0, endedIn = 0;

uint32_t GameMainLoop() {
    uint32_t registry = 0;
    if (!Read(base + kServiceRegistry, &registry, 4) || !registry) return 0;
    using Find = uint32_t(__thiscall*)(void*, uint32_t);
    __try {
        return reinterpret_cast<Find>(base + kFindService)(reinterpret_cast<void*>(registry), gate::NameHash("GameMainLoop"));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return 0;
    }
}
bool HostMpMapHere();
bool CurrentMap(char* map, size_t mapSize, char* stream, size_t streamSize) {
    const uint32_t loop = GameMainLoop();
    if (!loop) return false;
    char buffer[128]{};
    if (!Read(loop + kMapOffset, buffer, sizeof(buffer) - 1)) return false;
    std::snprintf(map, mapSize, "%s", buffer);
    std::memset(buffer, 0, sizeof(buffer));
    if (Read(loop + kStreamOffset, buffer, sizeof(buffer) - 1)) std::snprintf(stream, streamSize, "%s", buffer);
    else stream[0] = 0;
    return map[0] != 0;
}

// When each partner last said it sits in the main menu (by token), on every machine.
struct MenuMark { uint32_t token; DWORD at; bool wants; };
MenuMark menuMarks[8]{};
volatile LONG menuMarksIn = 0, menuMarksSent = 0;
bool PeerInMenu(uint32_t token, DWORD now) {
    for (const auto& m : menuMarks)
        if (m.token == token && m.at && TicksSince(now, m.at) < 3000) return true;
    return false;
}
void OnPacket(const Packet& p) {
    // A partner's "I sit in the main menu" reaches every machine of the mesh, the other clients
    // too: noted everywhere, never a level to follow (a client took it for the host's level
    // "frontend" and asked for that level change).
    if (p.flags & kFlagInMenu) {
        InterlockedIncrement(&menuMarksIn);
        MenuMark* slot = nullptr;
        for (auto& m : menuMarks) {
            if (m.token == p.token) { slot = &m; break; }
            if (!slot && (!m.token || GetTickCount() - m.at > 60000)) slot = &m;
        }
        if (slot) {
            if (slot->token != p.token || GetTickCount() - slot->at > 3000) gate::Event("coop_level partner %08X is in the main menu: doors and level changes do not wait for him", p.token);
            const bool wants = (p.flags & kFlagWantsJoin) != 0;
            if (IsHost() && wants && (!slot->wants || slot->token != p.token)) {
                InterlockedIncrement(&joinNotices);
                const auto* peer = peerTable.FindByToken(p.token);
                char text[96];
                // a story joiner comes in at our restart; a game mode played solo (start survival) has no
                // lobby for him (02.11.: the restart notice in a solo MP map match)
                const bool story = InterlockedCompareExchange(&arena::stateMirror, 0, 0) == 0 && !HostMpMapHere();
                const unsigned who = peer && peer->machine < 8 ? peer->machine + 1u : 0u;
                if (story) std::snprintf(text, sizeof(text), "PLAYER %u WANTS TO JOIN - RESTART A CHECKPOINT TO LET THEM IN", who);
                else std::snprintf(text, sizeof(text), "PLAYER %u WANTS TO JOIN - THIS MATCH IS SOLO: START IT WITH HOST CO-OP", who);
                notice::Show(text);
                gate::Event("coop_level partner %08X in the main menu chose join co-op: he comes in at our next checkpoint restart", p.token);
            }
            *slot = MenuMark{p.token, Stamp(GetTickCount()), wants};
        }
        return;
    }
    if (IsHost()) return;
    if (p.token == hostLevelToken && !coop_peers::NewerSerial(p.sequence, hostLevelSequence)) return;
    hostLevelToken = p.token;
    hostLevelSequence = p.sequence;
    if (p.flags & kFlagEnded) {
        InterlockedIncrement(&endedIn);
        if (!InterlockedExchange(&hostEnded, 1)) gate::Event("coop_level the host ended the match (token %08X, sequence %u)", p.token, p.sequence);
        return;
    }
    if (!followHost) return;
    InterlockedIncrement(&received);
    wanted = p;
    havePacket = true;
    packetAt = GetTickCount();
}

// The script's own sequence, step for step. The first version left out
// StoreLevel (0x00B75880), taking it for an autosave; it is not: it fires
// kind 6 on the object at [0x0208FC08], a notification the SetDeathRestart
// path fires the same way on its sibling at [0x0208FC04], and the reload
// that followed never produced a player (2026-09-17 14:00). Whether that
// was the missing step is not proven, so the whole recipe is kept.
constexpr uint32_t kStoreLevel = 0x775880;   // 0x00B75880
// One explicit story jump owns the next checkpoint room selection.
uint32_t requestedRoomCheckpoint[4]{};
void NoteRoomTarget(const char* map, const uint32_t* guid) {
    std::memset(requestedRoomCheckpoint, 0, sizeof(requestedRoomCheckpoint));
    if (map && !_stricmp(map, "ds_2") && guid)
        std::memcpy(requestedRoomCheckpoint, guid, sizeof(requestedRoomCheckpoint));
}
uint32_t __fastcall SelectCheckpointRooms(void* manager, void*) {
    const uint32_t native = reinterpret_cast<uint32_t(__thiscall*)(void*)>(base + 0x2658)(manager);
    if (checkpoint::GuidZero(requestedRoomCheckpoint)) return native;
    uint32_t target[4], overrideGuid[4]{}, actual[4]{};
    std::memcpy(target, requestedRoomCheckpoint, sizeof(target));
    NoteRoomTarget(nullptr, nullptr);
    if (!Read(base + kOverrideGuid, overrideGuid, 16) || !checkpoint::GuidEqual(target, overrideGuid)) {
        gate::Event("coop_level room target %08X-%08X superseded", target[0], target[1]);
        return native;
    }
    if (native && Read(native + 0x24, actual, 16) && checkpoint::GuidEqual(actual, target)) return native;
    const uint32_t selected = checkpoint::FindCheckpoint(target);
    uint32_t found[4]{};
    const bool valid = selected && Read(selected + 0x24, found, 16) && checkpoint::GuidEqual(found, target);
    gate::Event("coop_level checkpoint rooms native=%08X-%08X requested=%08X-%08X resolved=%d",
                actual[0], actual[1], target[0], target[1], valid);
    return valid ? selected : native;
}
bool InstallRoomSelection() {
    // LevelManager state 11 selects the checkpoint whose StreamSet it activates.
    constexpr unsigned char expected[] = {0x8B,0xCE,0xE8,0x2B,0x4C,0xFD,0xFF,0x8B,0xF8,0x85,0xFF};
    constexpr unsigned char thunk[] = {0xE9,0x23,0x75,0x02,0x00};
    unsigned char actual[sizeof(expected)]{};
    const uintptr_t call = base + 0x2DA28;
    if (!Read(call - 2, actual, sizeof(expected)) || std::memcmp(actual, expected, sizeof(expected)) ||
        !Read(base + 0x2658, actual, sizeof(thunk)) || std::memcmp(actual, thunk, sizeof(thunk))) return false;
    const uint32_t relative = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&SelectCheckpointRooms) - (call + 5));
    if (!WriteBytes(call + 1, &relative, 4)) return false;
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(call), 5);
    return true;
}
bool RequestChange(const Packet& p) {
    const uint32_t loop = GameMainLoop();
    uint32_t store = 0;
    if (!loop) return false;
    using SetOverride = void(__cdecl*)(const uint32_t*);
    using ClearPersistent = void(__thiscall*)(void*, uint32_t);
    using StoreLevel = void(__cdecl*)();
    using Request = bool(__thiscall*)(void*, const char*, const char*, bool, const char*);
    bool ok = false;
    __try {
        const bool haveCheckpoint = p.checkpoint[0] | p.checkpoint[1] | p.checkpoint[2] | p.checkpoint[3];
        if (haveCheckpoint) reinterpret_cast<SetOverride>(base + kSetOverride)(p.checkpoint);
        if (Read(base + kPersistentStore, &store, 4) && store)
            reinterpret_cast<ClearPersistent>(base + kClearPersistent)(reinterpret_cast<void*>(store), 1);
        reinterpret_cast<StoreLevel>(base + kStoreLevel)();
        ok = reinterpret_cast<Request>(base + kRequestLevelChange)(reinterpret_cast<void*>(loop), p.map,
                                                                  p.stream[0] ? p.stream : nullptr, true, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
    if (ok) NoteRoomTarget(p.map, p.checkpoint);
    return ok;
}

// 24.10.: inside a campaign arena the host's death checkpoint is all zeros. The lobby
// launched the arena on the client, the follow from the menu launched the host's map with
// that empty checkpoint 47 ms later, and the client came in at the level's default start
// ~550 m away, at every restart again. So an arena host's empty checkpoint is never followed
// and any other only after EmptyCheckpointWaitMs ([Level] SkipEmptyCheckpoint), and a campaign
// launch of somebody else (the lobby, an arena, the debug menu: mpmap's launch hold going up)
// within LaunchGraceMs, or an arena on its way, is never replaced.
bool skipEmptyCheckpoint = true, restartPointGuard = true;
DWORD launchGraceMs = 8000, emptyWaitMs = 10000;
bool launchHoldSeen = false, ownLaunch = false;
DWORD otherLaunchAt = 0;
volatile LONG launchesSeen = 0, menuSkipsEmpty = 0, menuSkipsLaunch = 0, menuSkipLines = 0;
void WatchLaunches(DWORD now) {
    const bool hold = mpmap::campaignClientHold;
    if (hold && !launchHoldSeen && !ownLaunch) {
        otherLaunchAt = Stamp(now);
        InterlockedIncrement(&launchesSeen);
        gate::Event("coop_level a campaign launch from the menu was requested here (lobby, arena or debug menu): the menu follow keeps off it for %lu ms",
                    static_cast<unsigned long>(launchGraceMs));
    }
    launchHoldSeen = hold;
}
// Why the host's level is not launched from the menu now, or nullptr.
const char* MenuFollowRefusal(DWORD now) {
    const LONG arenaState = InterlockedCompareExchange(&arena::stateMirror, 0, 0);
    if (arenaState != 0) return arenaState == 1 ? "a campaign arena is launching here" : "a campaign arena runs here";
    if (otherLaunchAt && TicksSince(now, otherLaunchAt) < launchGraceMs) return "another campaign launch was requested here moments ago";
    const bool empty = !(wanted.checkpoint[0] | wanted.checkpoint[1] | wanted.checkpoint[2] | wanted.checkpoint[3]);
    if (empty && joinOnHostRestart && joinArmed && !(wanted.flags & kFlagArena))
        return "waiting for the host's resolved story restart point";
    if (!skipEmptyCheckpoint || !empty) return nullptr;
    if (wanted.flags & kFlagArena) return "the host plays a campaign arena (empty checkpoint): the lobby's arena launch brings us";
    // A host at a fresh level start has no checkpoint yet either: after EmptyCheckpointWaitMs
    // without a launch or a checkpoint the default start is his too.
    static DWORD emptySince = 0;
    static uint32_t emptyEpoch = 0;
    if (!emptySince || emptyEpoch != wanted.epoch) { emptySince = Stamp(now); emptyEpoch = wanted.epoch; }
    if (TicksSince(now, emptySince) < emptyWaitMs) return "the host's checkpoint is empty: waiting for a lobby launch or his first checkpoint";
    gate::Event("coop_level the host's checkpoint stayed empty for %lu ms and no launch came: following to the level's own start", static_cast<unsigned long>(emptyWaitMs));
    return nullptr;
}

// The host's level, from the main menu, with the menu lobby's campaign start.
// True when the start was requested.
DWORD menuFollowedAt = 0;
bool FollowFromMenu(DWORD now, const char* why) {
    // one press reaches the menu's join several times: the start requested a moment ago stands
    if (menuFollowedAt && TicksSince(now, menuFollowedAt) < 5000) return true;
    if (now - lastMenuTry < 5000 || !havePacket || now - packetAt > 3000 || !wanted.map[0] || _strnicmp(wanted.map, "frontend", 8) == 0) return false;
    if (coop_mpmap::IsMpMap(wanted.map)) return false;   // an MP map comes through the menu lobby
    lastMenuTry = now;
    WatchLaunches(now);
    if (const char* no = MenuFollowRefusal(now)) {
        static const char* saidWhy = nullptr;
        static uint32_t saidEpoch = 0;
        InterlockedIncrement(otherLaunchAt && TicksSince(now, otherLaunchAt) < launchGraceMs ? &menuSkipsLaunch : &menuSkipsEmpty);
        const bool fresh = no != saidWhy || wanted.epoch != saidEpoch;   // retried every 5 s: said once per reason and epoch
        saidWhy = no;
        saidEpoch = wanted.epoch;
        if (fresh && InterlockedIncrement(&menuSkipLines) <= 100)
            gate::Event("coop_level follow from the main menu SKIPPED (%s): %s; host %s stream=%s checkpoint=%08X-%08X-%08X-%08X epoch=%u", why, no, wanted.map, wanted.stream,
                        wanted.checkpoint[0], wanted.checkpoint[1], wanted.checkpoint[2], wanted.checkpoint[3], wanted.epoch);
        return false;
    }
    ownLaunch = true;
    ::coop_uniform::Arm();   // the session starts only after the first settle: the join suit needs the hooks for this load
    loadsync::ArmJoinSuitForCheckpoint(wanted.checkpoint, "a story join from the menu");
    const bool ok = mpmap::StartCampaignFromMenu(wanted.map, wanted.stream, wanted.checkpoint);
    ownLaunch = false;
    launchHoldSeen = mpmap::campaignClientHold;
    InterlockedIncrement(ok ? &menuFollows : &menuFollowsRefused);
    gate::Event("coop_level follow from the main menu %s (%s): %s stream=%s checkpoint=%08X-%08X epoch=%u", ok ? "requested" : "REFUSED", why, wanted.map,
                wanted.stream, wanted.checkpoint[0], wanted.checkpoint[1], wanted.epoch);
    if (ok) { menuFollowedEpoch = wanted.epoch; followedEpoch = wanted.epoch; joined = true; menuFollowedAt = Stamp(now); }
    return ok;
}
// The host back in the main menu tells the partners the match is over (matchend).
bool SendEnded() {
    Packet p{};
    p.magic = kMagic; p.version = kVersion; p.token = peerReadiness.token; p.sequence = ++sequence; p.flags = kFlagEnded;
    std::snprintf(p.map, sizeof(p.map), "frontend");
    if (!p.token || !SendToPeers(&p, sizeof(p))) return false;
    InterlockedIncrement(&endedSent);
    return true;
}
// A new session starts with nothing to follow.
void ResetFollow() {
    wanted = Packet{};
    hostLevelToken = hostLevelSequence = 0;
    havePacket = joined = hostSawMenu = inMenu = false;
    followedEpoch = menuEpoch = menuFollowedEpoch = 0;
    lastMap[0] = 0;
    InterlockedExchange(&hostEnded, 0);
}
// For the menu's join item: a client whose session still runs follows the host now.
bool HostLevelKnown(DWORD now) { return !IsHost() && havePacket && now - packetAt <= 3000 && wanted.map[0] && !coop_mpmap::IsMpMap(wanted.map); }
// This machine plays an MP map (a game mode).
bool HostMpMapHere() { char map[64] = "", stream[64] = ""; return CurrentMap(map, sizeof(map), stream, sizeof(stream)) && coop_mpmap::IsMpMap(map); }
// Story only: the join waits for the host's checkpoint restart there, never in a game mode (user 02.11.).
// A campaign arena is a campaign map but a game mode.
bool HostPlaysStory(DWORD now) { return HostLevelKnown(now) && !(wanted.flags & kFlagArena) && _strnicmp(wanted.map, "frontend", 8) != 0; }
// The same for an MP map the host still plays: a client that left it may go back in.
bool HostMpMap(DWORD now, char* out, size_t size) {
    if (IsHost() || !havePacket || now - packetAt > 3000 || !wanted.map[0] || !coop_mpmap::IsMpMap(wanted.map)) return false;
    std::snprintf(out, size, "%s", wanted.map);
    return true;
}

// A client's restart point must not stay all zeros ([Level] RestartPointGuard): a restart
// then loads the level's default start. When the death checkpoint (0x0208FCC4) is empty and
// the launch left an override checkpoint (0x02091820, SetOverrideCheckpoint), that
// checkpoint becomes the restart point the way the checkpoint module applies the host's:
// Checkpoint::Find, then SetDeathRestart on it. A few tries per level, every 3 s.
DWORD guardAt = 0;
unsigned guardTries = 0;
bool guardWasInLevel = false;
volatile LONG guardApplied = 0, guardNoOverride = 0, guardNotFound = 0, guardFailed = 0, guardLines = 0;
void RestartPointTick(DWORD now) {
    if (inLevel != guardWasInLevel) { guardWasInLevel = inLevel; guardTries = 0; }
    if (!restartPointGuard || IsHost() || !checkpoint::installed || !inLevel || !levelSettled || !LocalCampaignPlayer() || mpmap::InVisit() ||
        guardTries >= 3 || TicksSince(now, guardAt) < 3000) return;
    guardAt = Stamp(now);
    uint32_t death[4]{}, override_[4]{};
    if (!Read(base + kDeathGuid, death, 16) || (death[0] | death[1] | death[2] | death[3])) return;
    ++guardTries;
    if (!Read(base + kOverrideGuid, override_, 16) || !(override_[0] | override_[1] | override_[2] | override_[3])) {
        InterlockedIncrement(&guardNoOverride);
        if (guardTries == 1 && InterlockedIncrement(&guardLines) <= 20)
            gate::Event("coop_level our restart point is empty and no override checkpoint is set: a restart would load the level's default start");
        return;
    }
    const uint32_t object = checkpoint::FindCheckpoint(override_);
    const bool ok = object && checkpoint::ApplyTo(object);
    uint32_t after[4]{};
    Read(base + kDeathGuid, after, 16);
    InterlockedIncrement(!object ? &guardNotFound : ok ? &guardApplied : &guardFailed);
    if (InterlockedIncrement(&guardLines) <= 20)
        gate::Event("coop_level our restart point was empty: override checkpoint %08X-%08X-%08X-%08X %s; restart point now %08X-%08X-%08X-%08X", override_[0],
                    override_[1], override_[2], override_[3], !object ? "NOT FOUND here" : ok ? "applied" : "apply FAILED", after[0], after[1], after[2], after[3]);
}

// The map for the report, read here on the game thread (CurrentMap asks the
// game's service registry, which the diagnostics thread must not).
char reportMap[64] = "", reportStream[64] = "";
DWORD reportMapAt = 0;
void Tick(DWORD now) {
    if (!installed) return;
    if (now - reportMapAt >= 2000) {
        reportMapAt = now;
        if (!CurrentMap(reportMap, sizeof(reportMap), reportStream, sizeof(reportStream))) { reportMap[0] = 0; reportStream[0] = 0; }
    }
    if (!enabled) return;
    WatchLaunches(now);
    RestartPointTick(now);
    if (IsHost()) {
        if (_strnicmp(reportMap, "frontend", 8) == 0) hostSawMenu = true;
        if (!peerTable.Count() || now - lastSend < 1000 || !LocalSlotZeroEntity()) return;
        lastSend = now;
        Packet p{};
        p.magic = kMagic; p.version = kVersion; p.token = peerReadiness.token; p.sequence = ++sequence;
        if (!CurrentMap(p.map, sizeof(p.map), p.stream, sizeof(p.stream))) return;
        if (_strnicmp(p.map, "frontend", 8) == 0) { hostSawMenu = true; return; }
        if (_stricmp(p.map, lastMap) != 0 || hostSawMenu) { std::snprintf(lastMap, sizeof(lastMap), "%s", p.map); ++epoch; hostSawMenu = false; }
        p.epoch = epoch;
        if (InterlockedCompareExchange(&arena::stateMirror, 0, 0) != 0) p.flags |= kFlagArena;
        Read(base + kDeathGuid, p.checkpoint, 16);
        if (!(p.flags & kFlagArena) && !coop_mpmap::IsMpMap(p.map) &&
            !(p.checkpoint[0] | p.checkpoint[1] | p.checkpoint[2] | p.checkpoint[3]) &&
            checkpoint::installed && checkpoint::LevelRunningNow()) {
            // The native restart resolves an empty death GUID via the override or map default.
            const uint32_t point = checkpoint::FindCheckpoint(nullptr);
            uint32_t resolved[4]{};
            if (point && Read(point + 0x24, resolved, sizeof(resolved)))
                std::memcpy(p.checkpoint, resolved, sizeof(resolved));
            static uint32_t loggedEpoch = ~0u, loggedGuid[4]{};
            if (loggedEpoch != epoch || std::memcmp(loggedGuid, p.checkpoint, sizeof(loggedGuid))) {
                gate::Event("coop_level host story restart target: map=%s epoch=%u object=%08X guid=%08X-%08X-%08X-%08X",
                            p.map, epoch, point, p.checkpoint[0], p.checkpoint[1], p.checkpoint[2], p.checkpoint[3]);
                loggedEpoch = epoch;
                std::memcpy(loggedGuid, p.checkpoint, sizeof(loggedGuid));
            }
        }
        if (SendToPeers(&p, sizeof(p))) InterlockedIncrement(&sent);
        return;
    }
    if (_strnicmp(reportMap, "frontend", 8) == 0) {
        if (!inMenu) { inMenu = true; menuEpoch = wanted.epoch; }
        if (peerTable.Count() && now - lastSend >= 1000) {
            lastSend = now;
            Packet m{};
            m.magic = kMagic; m.version = kVersion; m.token = peerReadiness.token; m.sequence = ++sequence; m.flags = kFlagInMenu | (joinArmed ? kFlagWantsJoin : 0u);
            std::snprintf(m.map, sizeof(m.map), "frontend");
            if (m.token && SendToPeers(&m, sizeof(m))) InterlockedIncrement(&menuMarksSent);
        }
        if (joinOnHostRestart && joinArmed && havePacket && !coop_mpmap::IsMpMap(wanted.map) && !(wanted.flags & kFlagArena)) {
            // The way in is the host's next coordinated load: his restart holds every machine at the
            // loading screen until we are in too (runtime_coop_loadsync.inc).
            char hostMap[48] = "";
            if (loadsync::HostLoading(now, hostMap, sizeof(hostMap))) FollowFromMenu(now, "the host restarts: joining with his load");
            else if (InterlockedIncrement(&joinWaits) == 1 || joinWaits % 600 == 0)
                gate::Event("coop_level join co-op chosen: waiting in the main menu for the host's next checkpoint restart (host plays %s)", wanted.map);
        } else if (havePacket && wanted.epoch != menuEpoch && wanted.epoch != menuFollowedEpoch) {
            if (joinArmed) FollowFromMenu(now, "join co-op was chosen");
            else if (InterlockedIncrement(&unarmedSkips) == 1)
                gate::Event("coop_level the host plays %s: not followed from the main menu until join co-op is chosen here", wanted.map);
        }
        return;
    }
    inMenu = false;
    if (!havePacket || now - lastRequest < 15000) return;
    // A launcher's client in its own game is not pulled into the host's either. One that
    // loaded the host's map by itself has joined (user, 19.10.: both loaded the same
    // checkpoint together and never saw each other, the session waited for join co-op).
    if (fromEnvironmentRole && !joinArmed && !joined) {
        char here[64]{}, hereStream[64]{};
        const bool hostPlays = now - packetAt <= 3000 && wanted.map[0] && _strnicmp(wanted.map, "frontend", 8) != 0;
        if (hostPlays && inLevel && LocalSlotZeroEntity() && CurrentMap(here, sizeof(here), hereStream, sizeof(hereStream)) &&
            _stricmp(here, wanted.map) == 0) {
            ArmJoin();
            InterlockedIncrement(&sameMapJoins);
            gate::Event("coop_level the host plays %s and so do we: joined without the menu", wanted.map);
        } else {
            if (InterlockedIncrement(&unarmedSkips) == 1)
                gate::Event("coop_level the host plays %s: not followed until join co-op is chosen in the main menu", wanted.map);
            return;
        }
    }
    // Not while this side is loading or has no player: RequestLevelChange
    // would refuse anyway, and a level without a player is mid-transition.
    if (!inLevel || !LocalSlotZeroEntity()) return;
    char map[64]{}, stream[64]{};
    if (!CurrentMap(map, sizeof(map), stream, sizeof(stream))) return;
    const bool sameMap = _stricmp(map, wanted.map) == 0;
    uint32_t mine[4]{};
    Read(base + kDeathGuid, mine, 16);
    const bool sameCheckpoint = std::memcmp(mine, wanted.checkpoint, 16) == 0;
    // Only a different map is followed, never a different checkpoint in the
    // same map. The first version reloaded the same map with the host's
    // checkpoint on joining (13:58, 2026-09-17: `ds_2 -> ds_2`), and that
    // reload never finished: the second player sat in a loading screen while
    // his ghost stood next to the host. Within one map the checkpoint module
    // aligns the restart point on its own.
    bool follow = false;
    if (!joined) {
        follow = !sameMap;
        joined = true;
        followedEpoch = wanted.epoch;
        if (!follow) gate::Event("coop_level joined in place map=%s checkpoint_same=%d", map, sameCheckpoint ? 1 : 0);
    } else if (wanted.epoch != followedEpoch && !sameMap) {
        follow = true;
        followedEpoch = wanted.epoch;
    }
    // An MP map is loaded by its visit ([MpMap]: saves held, MP logic blocked,
    // own player), never with this campaign recipe. The menu lobby launches
    // every machine itself; going from or to an MP map is not followed here.
    // a campaign arena is launched on every machine by the menu lobby, as an MP map is
    if (follow && arena::RunningNow() && _strnicmp(wanted.map, "frontend", 8) != 0) {
        InterlockedIncrement(&mpSkips);
        if (InterlockedCompareExchange(&mpSkips, 0, 0) <= 20) gate::Event("coop_level follow skipped: %s -> %s while a campaign arena runs here", map, wanted.map);
        return;
    }
    if (follow && (coop_mpmap::IsMpMap(wanted.map) || coop_mpmap::IsMpMap(map))) {
        InterlockedIncrement(&mpSkips);
        if (InterlockedCompareExchange(&mpSkips, 0, 0) <= 20)
            gate::Event("coop_level follow skipped: %s -> %s involves an MP map (the menu lobby or [MpMap] loads those)", map, wanted.map);
        return;
    }
    if (!follow) return;
    lastRequest = now;
    InterlockedIncrement(&mismatches);
    const bool ok = RequestChange(wanted);
    InterlockedIncrement(ok ? &requests : &refused);
    gate::Event("coop_level follow %s: %s -> %s stream=%s checkpoint=%08X-%08X-%08X-%08X epoch=%u",
                ok ? "requested" : "REFUSED", map, wanted.map, wanted.stream, wanted.checkpoint[0], wanted.checkpoint[1],
                wanted.checkpoint[2], wanted.checkpoint[3], wanted.epoch);
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Level", "Enabled", 1, ini.c_str()) != 0;
    followHost = GetPrivateProfileIntA("Level", "FollowHost", 1, ini.c_str()) != 0;
    followFromMenu = followHost && GetPrivateProfileIntA("Level", "FollowFromMenu", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop_level follow_from_menu=%d (a client in the main menu follows when the host loads a level again)\n", followFromMenu ? 1 : 0);
    skipEmptyCheckpoint = GetPrivateProfileIntA("Level", "SkipEmptyCheckpoint", 1, ini.c_str()) != 0;
    launchGraceMs = static_cast<DWORD>(GetPrivateProfileIntA("Level", "LaunchGraceMs", 8000, ini.c_str()));
    emptyWaitMs = static_cast<DWORD>(GetPrivateProfileIntA("Level", "EmptyCheckpointWaitMs", 10000, ini.c_str()));
    restartPointGuard = GetPrivateProfileIntA("Level", "RestartPointGuard", 1, ini.c_str()) != 0;
    joinOnHostRestart = GetPrivateProfileIntA("Level", "JoinOnHostRestart", 1, ini.c_str()) != 0;
    std::fprintf(log, "coop_level join_on_host_restart=%d (a story client in the main menu comes in only with the host's next checkpoint restart)\n",
                 joinOnHostRestart ? 1 : 0);
    std::fprintf(log, "coop_level skip_empty_checkpoint=%d empty_checkpoint_wait_ms=%lu launch_grace_ms=%lu restart_point_guard=%d (the menu follow never launches "
                      "an arena host's empty checkpoint or replaces another launch; an empty restart point takes the launch's checkpoint)\n",
                 skipEmptyCheckpoint ? 1 : 0, static_cast<unsigned long>(emptyWaitMs), static_cast<unsigned long>(launchGraceMs), restartPointGuard ? 1 : 0);
    if (!enabled) { std::fprintf(log, "coop_level off\n"); return false; }
    struct Sig { uint32_t rva; const unsigned char* bytes; size_t size; const char* name; };
    // mov eax,[esp+4] / push esi / push edi / mov edi,ecx / lea esi,[edi+4] / test eax,eax
    static const unsigned char kRequest[] = {0x8B,0x44,0x24,0x04,0x56,0x57,0x8B,0xF9,0x8D,0x77,0x04,0x85,0xC0};
    // mov eax,[esp+4] / mov ecx,[eax] / mov [0x02091820],ecx
    static const unsigned char kOverride[] = {0x8B,0x44,0x24,0x04,0x8B,0x08,0x89,0x0D,0x20,0x18,0x09,0x02};
    // push esi / mov esi,[ecx+0x88] / xor eax,eax / xor edx,edx
    static const unsigned char kFind[] = {0x56,0x8B,0xB1,0x88,0x00,0x00,0x00,0x33,0xC0,0x33,0xD2};
    // push ebx / push esi / mov ebx,ecx / mov esi,[ebx+0x18] / sub esi,1
    static const unsigned char kClear[] = {0x53,0x56,0x8B,0xD9,0x8B,0x73,0x18,0x83,0xEE,0x01};
    // push 6 / call / mov ecx,[0x0208FC08] / push eax / call
    static const unsigned char kStore[] = {0x6A,0x06,0xE8};
    const Sig sigs[] = {{kRequestLevelChange, kRequest, sizeof(kRequest), "RequestLevelChange"},
                        {kStoreLevel, kStore, sizeof(kStore), "StoreLevel"},
                        {kSetOverride, kOverride, sizeof(kOverride), "SetOverrideCheckpoint"},
                        {kFindService, kFind, sizeof(kFind), "FindService"},
                        {kClearPersistent, kClear, sizeof(kClear), "ClearPersistent"}};
    for (const auto& sig : sigs) {
        unsigned char actual[16]{};
        if (!Read(base + sig.rva, actual, sig.size) || std::memcmp(actual, sig.bytes, sig.size) != 0) {
            std::fprintf(log, "coop_level %s signature mismatch at %08X; level follow off\n", sig.name, static_cast<unsigned>(base + sig.rva));
            return false;
        }
    }
    std::fprintf(log, "coop_level directed checkpoint room selection: %s\n", InstallRoomSelection() ? "installed" : "native (site unavailable)");
    installed = true;
    std::fprintf(log, "coop_level follow_host=%d role=%s request=%08X override=%08X\n", followHost,
                 IsHost() ? "host (announces)" : "client (follows)", static_cast<unsigned>(base + kRequestLevelChange),
                 static_cast<unsigned>(base + kSetOverride));
    return true;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static DWORD lastLine = 0;
    const DWORD now = GetTickCount();
    if (now - lastLine < 10000) return;
    lastLine = now;
    char map[64]{}, stream[64]{};
    std::memcpy(map, reportMap, sizeof(map) - 1);
    std::memcpy(stream, reportStream, sizeof(stream) - 1);
    std::fprintf(log, "coop_level map=%s stream=%s sent=%ld received=%ld requests=%ld refused=%ld mismatches=%ld faults=%ld "
                      "joined=%d host_map=%s host_epoch=%u join_armed=%d same_map_joins=%ld\n",
                 map, stream, InterlockedCompareExchange(&sent, 0, 0), InterlockedCompareExchange(&received, 0, 0),
                 InterlockedCompareExchange(&requests, 0, 0), InterlockedCompareExchange(&refused, 0, 0),
                 InterlockedCompareExchange(&mismatches, 0, 0), InterlockedCompareExchange(&faults, 0, 0),
                 joined, havePacket ? wanted.map : "-", havePacket ? wanted.epoch : 0, joinArmed ? 1 : 0,
                 InterlockedCompareExchange(&sameMapJoins, 0, 0));
    std::fprintf(log, "coop_level menu_follows=%ld refused=%ld skipped_empty_or_arena=%ld skipped_other_launch=%ld other_launches_seen=%ld "
                      "restart_point applied=%ld no_override=%ld not_found=%ld failed=%ld join_waits=%ld join_notices=%ld\n",
                 InterlockedCompareExchange(&menuFollows, 0, 0), InterlockedCompareExchange(&menuFollowsRefused, 0, 0),
                 InterlockedCompareExchange(&menuSkipsEmpty, 0, 0), InterlockedCompareExchange(&menuSkipsLaunch, 0, 0),
                 InterlockedCompareExchange(&launchesSeen, 0, 0), InterlockedCompareExchange(&guardApplied, 0, 0),
                 InterlockedCompareExchange(&guardNoOverride, 0, 0), InterlockedCompareExchange(&guardNotFound, 0, 0),
                 InterlockedCompareExchange(&guardFailed, 0, 0), InterlockedCompareExchange(&joinWaits, 0, 0), InterlockedCompareExchange(&joinNotices, 0, 0));
}
}  // namespace level
