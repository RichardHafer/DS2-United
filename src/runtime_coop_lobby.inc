// Dead Space 2 United by Richard Hafer.
// The main menu lobby for online survival (coop_lobby.h has the protocol).
//
// "host co-op" opens the lobby on [Coop] Port + 2000 and then the map picker;
// every joiner that says hello gets a machine number. When the host picks a map
// it launches it and announces it; "join co-op" says hello to [Coop] JoinAddress
// until that announcement arrives and then launches the same map. In the level
// the co-op session starts as usual (AutoStart): the host listens, a joiner
// dials the host's game port with the machine number it was given.
// The launch carries the host's game mode; a joiner takes it before loading.
namespace gamemode { extern volatile LONG mode; }   // runtime_coop_gamemode.inc
namespace mpnative { uint32_t ProfileXp(); }        // runtime_coop_mpnative.inc
namespace arena { bool Start(const char* name, DWORD now); extern volatile LONG stateMirror; }   // runtime_coop_arena.inc
namespace matchend { bool Busy(); void EndNow(); bool Reusable(); }   // runtime_coop_matchend.inc
namespace lobby {
// Seats for joiners: eight players at most, the host's bots included.
unsigned JoinerSeats() {
    const unsigned bots = bot::enabled ? (bot::wantedBots < coop_lobby::kMaxMembers ? bot::wantedBots : coop_lobby::kMaxMembers) : 0;
    return coop_lobby::kMaxMembers - bots;
}
uint8_t LaunchFlags() { return coop_lobby::ModeFlags(InterlockedCompareExchange(&gamemode::mode, 0, 0)); }
// Machines of partners the host's running session still hears (bit n = machine n):
// a new joiner must not get one of their numbers, the ghost subkeys carry it.
uint32_t HeldMachines(DWORD now) {
    if (!HostSessionRunning() && !LauncherHost()) return 0;   // a launcher's partners hold their numbers without a session too
    uint32_t mask = 0;
    for (const auto& p : peerTable.peers)
        if (p.used && p.machine >= 1 && p.machine <= coop_lobby::kMaxMembers && now - p.lastSeen < 15000) mask |= 1u << p.machine;
    return mask;
}
enum class Role { None, Host, Join };
Role role = Role::None;
// A separate presence bit lets hosts share seed zero.
uint32_t HostSeed() { return mesh::seedSync ? mesh::ownSeed : 0u; }
// A joiner plays the host's seed from the first level on: the side channel's table comes too late for it.
void AdoptLaunchSeed(uint32_t seed, bool present) {
    if (!present || !mesh::seedSync) return;
    mesh::AdoptSeed(seed, "lobby launch");
}
// Survival: the host's map picker launches everybody. Campaign: the host plays;
// waiting joiners load his map and checkpoint at his next restart.
enum class Mode { Survival, Campaign };
Mode mode = Mode::Survival;
coop_lobby::CampaignPacket campaignOut;
bool campaignArmed = false, wasRunning = false, announceOver = false;
DWORD campaignAt = 0, notRunningAt = 0;
unsigned campaignSends = 0;
volatile LONG campaignLaunches = 0, lateJoins = 0;
SOCKET sock = INVALID_SOCKET;
coop_lobby::Host table;
coop_lobby::Joiner joiner;
uint32_t token = 0, seq = 0, build = 0, randomizer = 0;
uint32_t hostIp = 0;
unsigned hostGamePort = 0;
int launchedMap = -1;
DWORD launchedAt = 0, lastSend = 0, startedAt = 0;
volatile LONG sent = 0, received = 0, rejected = 0, admitted = 0, launchesSent = 0, sendErrors = 0, versionRefusals = 0;
char status[96] = "join co-op";   // the "join co-op" item's label
// The same on the gamemodes screen (FE03), without the human/necromorph choice: that
// choice is the story screen's, and on FE03 left/right never reached it (02.11.).
char modeStatus[96] = "join co-op";
// Join as a human or as a necromorph ([Necro] Enabled=1, user 16.10.): left/right on the
// "join co-op" row of the story screen (menu::OnSp01Input) shows "< human >" or
// "< necromorph >"; kept in DS2Profile.ini [Coop] JoinAs. A necro joiner turns into one
// once the host's level runs (necro::JoinAsNecro).
bool joinAsNecro = false, joinRoleLoaded = false;
void SetIdleStatus();
volatile LONG rejoins = 0, hintsShown = 0, survivalWarnings = 0, launcherJoins = 0, launcherRefusals = 0;
bool noAnswerHinted = false;
// A launcher's client (DS2COOP) in the join lobby: it also follows the host's story level
// (level::joinArmed), so its lobby closes once it leaves the main menu.
bool launcherJoin = false;
DWORD idleStatusAt = 0;

// The lobby list (runtime_coop_nativelobby.inc fills and reads it). Host: the
// packet it sends every joiner while no map is launched. Joiner: the host's last
// one, its round trip, and a count of launches so the lobby screen can tell a
// launch from a goodbye.
coop_lobby::StatePacket stateOut;
bool stateOutReady = false;
DWORD stateSentAt = 0;
coop_lobby::StatePacket hostState;
DWORD hostStateAt = 0;
volatile LONG statesSent = 0, statesIn = 0, byesSent = 0, byesIn = 0, joinLaunches = 0;
uint16_t pingMs = 0;
uint32_t helloSeq[8]{};
DWORD helloAt[8]{};
coop_lobby::Profile OwnProfile(unsigned machine) {
    coop_lobby::Profile p;
    if (nameplate::ownName[0]) std::snprintf(p.name, sizeof(p.name), "%s", nameplate::ownName);
    else std::snprintf(p.name, sizeof(p.name), "PLAYER %u", machine + 1);
    p.xp = mpnative::ProfileXp();
    p.pingMs = pingMs;
    return p;
}

// The build id both sides compare: this plugin's link time stamp.
uint32_t ModuleStamp(const void* module) {
    if (!module) return 0;
    const auto* dos = static_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(dos) + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
}
// The randomizer and its pool must match too: the host's enemies are only
// ghosted to a client that has the same templates resident.
uint32_t RandomizerId() { return ModuleStamp(ds2coop::PartnerModule("DS2RandomizerObserver.asi")); }
uint32_t BuildId() {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(&__ImageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(dos) + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
}
bool Open(unsigned port) {
    if (sock != INVALID_SOCKET) return true;
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return false;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(static_cast<u_short>(port));
    if (::bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        mpmap::Event("lobby: UDP port %u could not be bound (error %d); another instance owns it?", port, WSAGetLastError());
        closesocket(sock);
        sock = INVALID_SOCKET;
        return false;
    }
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
    return true;
}
void Close() {
    if (sock != INVALID_SOCKET) closesocket(sock);
    sock = INVALID_SOCKET;
}
void Send(const coop_lobby::Packet& p, uint32_t ip, uint16_t portNet) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = ip;
    to.sin_port = portNet;
    if (::sendto(sock, reinterpret_cast<const char*>(&p), sizeof(p), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == sizeof(p))
        InterlockedIncrement(&sent);
    else
        InterlockedIncrement(&sendErrors);
}
// Leaving says goodbye: a host's joiners leave its lobby screen, a joiner's seat frees up.
void SendByes() {
    if (sock == INVALID_SOCKET) return;
    const coop_lobby::Packet bye = coop_lobby::Make(coop_lobby::Type::Bye, role == Role::Host ? token : joiner.token, ++seq, static_cast<uint16_t>(localPort), build, randomizer);
    if (role == Role::Host) {
        for (const auto& m : table.members)
            if (m.used) { Send(bye, m.ip, m.port); InterlockedIncrement(&byesSent); }
    } else if (role == Role::Join) {
        Send(bye, hostIp, htons(static_cast<u_short>(hostGamePort + coop_lobby::kLobbyPortOffset)));
        InterlockedIncrement(&byesSent);
    }
}
void Stop() {
    if (role != Role::None) mpmap::Event("lobby: closed (%s)", role == Role::Host ? "host" : "join");
    SendByes();
    Close();
    role = Role::None;
    launcherJoin = false;
    stateOutReady = false;
    hostStateAt = 0;
    SetIdleStatus();
}
const char* MapLabel(int index) { return coop_mpmap::MapLabel(index); }
void SetStatus(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vsnprintf(status, sizeof(status), format, args);
    va_end(args);
    std::snprintf(modeStatus, sizeof(modeStatus), "%s", status);
    if (char* choice = std::strstr(modeStatus, "   < ")) *choice = 0;
}
}  // namespace lobby
namespace mpnative { extern std::string profilePath; }   // runtime_coop_mpnative.inc: DS2Profile.ini
namespace lobby {
bool JoinRoleOffered() { return necro::enabled; }
void SetIdleStatus() {
    if (JoinRoleOffered()) SetStatus("join co-op   < %s >", joinAsNecro ? "necromorph" : "human");
    else SetStatus("join co-op");
}
// The row shows the choice, nothing is under way.
bool ShowsJoinChoice() { return _strnicmp(status, "join co-op", 10) == 0 && !std::strchr(status, ':'); }
void LoadJoinRole() {
    if (joinRoleLoaded || mpnative::profilePath.empty()) return;
    joinRoleLoaded = true;
    char value[16] = "";
    GetPrivateProfileStringA("Coop", "JoinAs", "human", value, sizeof(value), mpnative::profilePath.c_str());
    joinAsNecro = _stricmp(value, "necro") == 0;
    if (ShowsJoinChoice()) SetIdleStatus();
}
void SetJoinRole(bool necroRole) {
    joinAsNecro = necroRole;
    if (!mpnative::profilePath.empty()) WritePrivateProfileStringA("Coop", "JoinAs", necroRole ? "necro" : "human", mpnative::profilePath.c_str());
    SetIdleStatus();
    mpmap::Event("lobby: join as %s", necroRole ? "a necromorph" : "a human");
}
// The item's label as the native hint, the overlay card with why and the fix.
void Hint(const char* why, const char* fix) {
    InterlockedIncrement(&hintsShown);
    overlay::ShowHint(status, why, fix, 15000);
}
// The randomizer's survival director, asked before a launch (DS2Survival_Ready): 1 ready,
// 2 ready with a warning, 0 no, -1 still starting; why and fix say what is missing.
using SurvivalReadyFn = int(__cdecl*)(const char*, int, char*, int, char*, int);
SurvivalReadyFn survivalReadyFn = nullptr;
bool survivalReadyLooked = false;
int SurvivalCheck(const char* map, int gameMode, char (&why)[240], char (&fix)[200]) {
    why[0] = fix[0] = 0;
    if (!survivalReadyLooked) {
        survivalReadyLooked = true;
        if (const HMODULE module = ds2coop::PartnerModule("DS2RandomizerObserver.asi"))
            survivalReadyFn = reinterpret_cast<SurvivalReadyFn>(GetProcAddress(module, "DS2Survival_Ready"));
    }
    if (!survivalReadyFn) {
        std::snprintf(why, sizeof(why), "the randomizer half of Dead Space 2 United is not loaded");
        std::snprintf(fix, sizeof(fix), "put DS2United.asi back into the plugins folder; DS2Randomizer.log says more");
        return 0;
    }
    return survivalReadyFn(map, gameMode, why, static_cast<int>(sizeof(why)), fix, static_cast<int>(sizeof(fix)));
}
void HintRefusal(const char* refusal) {
    const char* r = refusal ? refusal : "";
    if (std::strstr(r, "full")) Hint("the host's lobby is full", "wait for a free seat, or ask the host to raise the player count");
    else if (std::strstr(r, "closed")) Hint("the host closed the lobby", "ask the host to pick host co-op again, then join");
    else if (std::strstr(r, "build")) Hint(r, "both PCs need the same DS2United.asi from the same update");
    else Hint(r[0] ? r : "the host refused the join", "DS2United.log on both PCs has the reason");
}

// From the Multiplayer screen.
bool StartHost(Mode wanted = Mode::Survival) {
    if (matchend::Busy()) matchend::EndNow();
    randomizer = RandomizerId();
    if (role == Role::Host && mode == wanted) return true;
    // A launcher's client never hosts; the story screen's own start still runs natively.
    if (LauncherClient()) {
        if (InterlockedIncrement(&launcherRefusals) <= 20)
            mpmap::Event("lobby: host REFUSED: the launcher (DS2COOP) started this copy as client machine %u; no lobby opened", machineIndex);
        return false;
    }
    // one TNL session per game start: a client's session cannot turn into a host's
    if (session && !IsHost() && !matchend::Reusable()) {
        mpmap::Event("lobby: host REFUSED: this game start already ran a co-op session as a client");
        SetStatus("host co-op: restart the game first");
        Hint("this game start already joined a session as a client; each start hosts or joins once", "quit to the desktop and start the game again to host");
        return false;
    }
    mode = wanted;
    campaignArmed = false;
    wasRunning = false;
    Close();
    role = Role::None;
    if (!Open(localPort + coop_lobby::kLobbyPortOffset)) {
        SetStatus("host co-op: port %u busy", localPort + coop_lobby::kLobbyPortOffset);
        Hint("another program or a second copy of the game uses this UDP port", "close the other copy, or give this PC another [Coop] Port (the partners must dial that one)");
        return false;
    }
    role = Role::Host;
    table = coop_lobby::Host{};
    token = (GetTickCount() * 2654435761u) ^ static_cast<uint32_t>(GetCurrentProcessId());
    if (!token) token = 1;
    launchedMap = -1;
    startedAt = GetTickCount();
    if (mode == Mode::Campaign) SetIdleStatus();
    else SetStatus("hosting - 0 joined");
    mpmap::Event("lobby: hosting on UDP %u (game port %u), token %08X, build %08X randomizer %08X, up to %u joiners", localPort + coop_lobby::kLobbyPortOffset,
                 localPort, token, build, randomizer, coop_lobby::kMaxMembers);
    return true;
}
bool StartJoin(const char* address) {
    if (matchend::Busy()) matchend::EndNow();
    randomizer = RandomizerId();
    unsigned ip = 0, port = 0;
    if (LauncherHost()) {
        mpmap::Event("lobby: join REFUSED: the launcher (DS2COOP) started this copy as the host");
        SetStatus("join co-op: this copy is the host");
        Hint("the launcher started this copy as the host; the partner's copy joins it", "pick host co-op here, or start a story game; the partner picks join co-op");
        return false;
    }
    // A launcher's client says hello to the host it was started with, not to JoinAddress.
    if (LauncherClient() && peerIp && peerPort) {
        ip = peerIp;
        port = peerPort;
    } else if (!address || !address[0] || !ParsePeer(address, ip, port)) {
        mpmap::Event("lobby: join REFUSED: [Coop] JoinAddress '%s' is not ip:port", address ? address : "");
        SetStatus("join co-op: set JoinAddress");
        Hint("[Coop] JoinAddress is empty or not ip:port", "enter the host's address in DS2UnitedConfig.exe (Co-op page), for example 25.12.34.56:27015");
        return false;
    }
    level::ArmJoin();
    // The session from before still runs (back in the main menu), or a launcher set the role: follow the host.
    // a host in a game mode (MP map or campaign arena) takes joiners through his lobby below
    if ((session || fromEnvironmentRole) && !IsHost() && level::HostPlaysStory(GetTickCount())) {
        // Story: in with the host's next checkpoint restart, never into his running level.
        if (level::joinOnHostRestart && !loadsync::HostLoading(GetTickCount(), nullptr, 0)) {
            SetStatus("joining - the host restarts a checkpoint to let you in");
            mpmap::Event("lobby: join co-op armed; the host plays: waiting for his next checkpoint restart (he gets a notice)");
            return true;
        }
        const bool started = level::FollowFromMenu(GetTickCount(), "the menu's join item");
        SetStatus("%s", started ? "loading the host's game" : "join co-op: the host's level could not be started");
        if (!started) Hint("the host's level could not be loaded from the menu", "restart the game and join again; DS2United.log has the reason");
        return started;
    }
    // A client that left the host's MP match goes back into it: the session still runs,
    // only the lobby's launch is over (13.10.: "restart the game first" instead).
    {
        char map[64] = "";
        if (session && !IsHost() && level::HostMpMap(GetTickCount(), map, sizeof(map))) {
            const bool started = mpmap::StartVisitFromMenu(map, GetTickCount());
            InterlockedIncrement(&rejoins);
            mpmap::Event("lobby: rejoin of the host's %s from the main menu %s", map, started ? "started" : "REFUSED (see the menu start line)");
            SetStatus("%s %s", started ? "loading" : "rejoin refused:", MapLabel(coop_mpmap::MapIndex(map)));
            if (!started) Hint("the host's map could not be started from the menu", "restart the game and join again; DS2United.log has the 'menu start' line");
            return started;
        }
    }
    // A launcher's client (DS2COOP) hears the host's story level on the side channel and follows
    // it (armed above); a survival launch only reaches it through the host's lobby, so it says
    // hello there too, keeping its own role and machine (ConfigureAsClient).
    if (LauncherClient()) {
        if (role == Role::Join && launcherJoin && sock != INVALID_SOCKET) return true;   // the same press again
        InterlockedIncrement(&launcherJoins);
        Close();
        role = Role::None;
        if (!ip || !port || !Open(localPort + coop_lobby::kLobbyPortOffset)) {
            SetStatus("joining - waiting for the host's story level");
            mpmap::Event("lobby: launcher join armed (machine %u): the host's story level is followed once it is known; %s", machineIndex,
                         ip && port ? "the lobby port is busy, so a survival launch cannot reach this copy" : "no host address to say hello to");
            return true;
        }
        role = Role::Join;
        launcherJoin = true;
        hostIp = ip;
        hostGamePort = port;
        joiner.Start();
        noAnswerHinted = false;
        startedAt = GetTickCount();
        SetStatus("joining - waiting for the host's game");
        mpmap::Event("lobby: launcher join (machine %u): following the host's story level once it is known, and saying hello to its lobby at %08X:%u for a survival launch",
                     machineIndex, static_cast<unsigned>(ntohl(ip)), port + coop_lobby::kLobbyPortOffset);
        return true;
    }
    if (session && !matchend::Reusable()) {
        mpmap::Event("lobby: join REFUSED: this game start already ran a co-op session (one per start)");
        SetStatus("join co-op: restart the game first");
        Hint("this game start already ran a co-op session; each start joins once", "quit to the desktop and start the game again");
        return false;
    }
    Close();
    role = Role::None;
    if (!Open(localPort + coop_lobby::kLobbyPortOffset)) {
        SetStatus("join co-op: port %u busy", localPort + coop_lobby::kLobbyPortOffset);
        Hint("another program or a second copy of the game uses this UDP port", "close the other copy, or give this PC another [Coop] Port");
        return false;
    }
    role = Role::Join;
    hostIp = ip;
    hostGamePort = port;
    joiner.Start();
    noAnswerHinted = false;
    startedAt = GetTickCount();
    SetStatus("joining - waiting for host");
    mpmap::Event("lobby: joining %s (lobby port %u), our game port %u, build %08X", address, port + coop_lobby::kLobbyPortOffset, localPort, build);
    return true;
}
// The maps a joiner cannot load (the Outbreak ones without DS2DAT12.DAT), from its hello.
uint8_t MissingAny() {
    for (unsigned i = 0; i < coop_mpmap::kMapCount; ++i)
        if (const uint8_t m = table.MissingMap(static_cast<int>(i))) return m;
    return 0;
}
void LogMissingMaps(uint8_t machine, uint32_t mask) {
    char names[160] = "";
    size_t used = 0;
    for (unsigned i = 0; i < coop_mpmap::kMapCount && used < sizeof(names) - 24; ++i)
        if (!(mask >> i & 1u)) used += std::snprintf(names + used, sizeof(names) - used, "%s%s", used ? ", " : "", coop_mpmap::MapLabel(static_cast<int>(i)));
    mpmap::Event("lobby: machine %u can load maps %08X%s%s", machine, mask, used ? "; missing: " : "", names);
    if (used) {
        char text[120];
        std::snprintf(text, sizeof(text), "PLAYER %u CANNOT LOAD: %s", machine + 1u, names);
        notice::Enqueue(text);
    }
}
// For the map picker: the first joiner who cannot load this map (0: everybody can).
uint8_t MissingFor(int mapIndex) { return role == Role::Host ? table.MissingMap(mapIndex) : 0; }

// From the map picker, after the host's own launch was accepted.
void HostLaunched(int mapIndex) {
    if (role != Role::Host) return;
    coop_lobby::Packet roster;
    table.FillRoster(roster);
    ConfigureHostDials(roster.roster, coop_lobby::kMaxMembers);
    launchedMap = mapIndex;
    launchedAt = GetTickCount();
    announceOver = false;
    lastSend = 0;
    const int arenaIndex = coop_lobby::ArenaIndexOf(mapIndex);
    mpmap::Event("lobby: host launched %s (%s %d), difficulty byte %u; announcing to %u joiner(s) for 60 s",
                 arenaIndex >= 0 && arenaIndex < static_cast<int>(coop_arena::kArenaCount) ? coop_arena::kArenas[arenaIndex].name : coop_mpmap::MapName(mapIndex),
                 arenaIndex >= 0 ? "arena code" : "index", mapIndex, difficulty::LobbyByte(), table.Count());
}

// Campaign host: a restart (the level manager leaves RUNNING in a level) with
// joiners waiting sends them the map and the checkpoint being loaded, every
// 300 ms until 10 s after this machine runs again.
void CampaignHostTick(DWORD now) {
    uint32_t director = 0;
    const bool frontend = Read(base + 0x1C8BE90, &director, 4) && director;
    const bool running = mpmap::LmState(nullptr) == coop_mpmap::kRunning;
    if (!campaignArmed) {
        if (table.Expire(now, 6000)) mpmap::Event("lobby: a campaign joiner went quiet; %u waiting", table.Count());
        if (running && !frontend) { wasRunning = true; notRunningAt = 0; return; }
        if (!running && !notRunningAt) notRunningAt = now;
        if (!table.Count() || running) return;
        // a load from the main menu or a restart: wait until the map being loaded is known
        char map[64]{}, stream[64]{};
        if (!level::CurrentMap(map, sizeof(map), stream, sizeof(stream)) || _strnicmp(map, "frontend", 8) == 0) return;
        campaignOut = coop_lobby::CampaignPacket{};
        campaignOut.token = token;
        campaignOut.build = build;
        campaignOut.randomizer = randomizer;
        campaignOut.gamePort = static_cast<uint16_t>(localPort);
        campaignOut.members = static_cast<uint8_t>(table.Count());
        std::snprintf(campaignOut.map, sizeof(campaignOut.map), "%s", map);
        std::snprintf(campaignOut.stream, sizeof(campaignOut.stream), "%s", stream);
        // a save's load sets the override; a restart goes to the death checkpoint
        uint32_t override_[4]{}, death[4]{};
        Read(base + mpmap::kOverrideGuid, override_, 16);
        Read(base + level::kDeathGuid, death, 16);
        std::memcpy(campaignOut.checkpoint, (override_[0] | override_[1] | override_[2] | override_[3]) ? override_ : death, 16);
        coop_lobby::Packet roster;
        table.FillRoster(roster);
        std::memcpy(campaignOut.roster, roster.roster, sizeof(campaignOut.roster));
        campaignOut.difficulty = difficulty::CampaignField();
        campaignOut.seed = HostSeed();
        campaignOut.flags = mesh::seedSync ? 1 : 0;
        campaignArmed = true;
        campaignAt = now;
        campaignSends = 0;
        InterlockedIncrement(&campaignLaunches);
        mpmap::Event("lobby: host %s %s (stream %s) checkpoint %08X-%08X difficulty field %u; pulling in %u joiner(s)", wasRunning ? "restarts" : "loads", map, stream,
                     campaignOut.checkpoint[0], campaignOut.checkpoint[1], campaignOut.difficulty, table.Count());
        wasRunning = false;
    }
    if (running && notRunningAt == 0 && now - campaignAt > 10000) {
        mpmap::Event("lobby: campaign launch announced %u time(s); the joiners are on their way (more need the next restart)", campaignSends);
        // the lobby stays open for the next restart; the joiners already sent are forgotten
        table = coop_lobby::Host{};
        campaignArmed = false;
        return;
    }
    if (running) notRunningAt = 0;
    if (now - lastSend < 300) return;
    lastSend = now;
    campaignOut.difficulty = difficulty::CampaignField();   // a save's load sets it while the level loads
    for (const auto& m : table.members) {
        if (!m.used) continue;
        campaignOut.seq = ++seq;
        campaignOut.machine = m.machine;
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = m.ip;
        to.sin_port = m.port;
        if (::sendto(sock, reinterpret_cast<const char*>(&campaignOut), sizeof(campaignOut), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == sizeof(campaignOut))
            InterlockedIncrement(&sent);
        else
            InterlockedIncrement(&sendErrors);
        ++campaignSends;
    }
}

void HostTick(DWORD now) {
    char buffer[512];
    sockaddr_in from{};
    int fromLen = sizeof(from);
    for (int i = 0; i < 32; ++i) {
        const int got = ::recvfrom(sock, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (got <= 0) break;
        coop_lobby::Packet p;
        if (!coop_lobby::Parse(buffer, got, p)) {
            InterlockedIncrement(&rejected);
            // an older build's hello: the build refusal in its own layout, so it stops waiting
            const int other = coop_lobby::OtherVersion(buffer, got);
            const int bytes = coop_lobby::RefusalBytes(other);
            if (other >= 0) {
                coop_lobby::Packet refusal = coop_lobby::Make(coop_lobby::Type::Mismatch, token, ++seq, static_cast<uint16_t>(localPort), build, randomizer);
                refusal.version = static_cast<uint8_t>(other);
                const bool replied = bytes && ::sendto(sock, reinterpret_cast<const char*>(&refusal), bytes, 0, reinterpret_cast<sockaddr*>(&from), sizeof(from)) == bytes;
                if (InterlockedIncrement(&versionRefusals) <= 10)
                    mpmap::Event("lobby: a packet of lobby version %d (ours %u) from %08X:%u REFUSED: another DS2Coop build; %s", other, coop_lobby::kVersion,
                                 ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), replied ? "the build refusal went back" : "no refusal it can read");
            }
            continue;
        }
        if (p.type == static_cast<uint8_t>(coop_lobby::Type::Bye)) {
            const unsigned before = table.Count();
            table.Forget(from.sin_addr.s_addr, from.sin_port);
            InterlockedIncrement(&byesIn);
            if (table.Count() != before) mpmap::Event("lobby: joiner %08X:%u left the lobby; %u joined", ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), table.Count());
            continue;
        }
        if (p.type != static_cast<uint8_t>(coop_lobby::Type::Hello)) { InterlockedIncrement(&rejected); continue; }
        InterlockedIncrement(&received);
        if (p.build != build || p.randomizer != randomizer) {
            Send(coop_lobby::Make(coop_lobby::Type::Mismatch, token, ++seq, static_cast<uint16_t>(localPort), build, randomizer), from.sin_addr.s_addr, from.sin_port);
            mpmap::Event("lobby: hello from %08X:%u REFUSED: build %08X randomizer %08X, ours %08X %08X", ntohl(from.sin_addr.s_addr), ntohs(from.sin_port),
                         p.build, p.randomizer, build, randomizer);
            continue;
        }
        const unsigned before = table.Count();
        const uint32_t held = HeldMachines(now);
        const uint8_t machine = table.Admit(from.sin_addr.s_addr, from.sin_port, p.gamePort, now, JoinerSeats(), held);
        if (!machine) {
            Send(coop_lobby::Make(coop_lobby::Type::Full, token, ++seq, static_cast<uint16_t>(localPort), build, randomizer), from.sin_addr.s_addr, from.sin_port);
            continue;
        }
        const uint8_t lackedBefore = MissingAny();
        table.NoteMaps(from.sin_addr.s_addr, from.sin_port, p.reserved);
        if (table.NoteProfile(from.sin_addr.s_addr, from.sin_port, p.profile) && table.Count() != before)
            mpmap::Event("lobby: machine %u is '%s', XP %u", machine, p.profile.name, p.profile.xp);
        if (table.Count() != before || MissingAny() != lackedBefore) LogMissingMaps(machine, p.reserved);
        if (table.Count() != before) {
            InterlockedIncrement(&admitted);
            mpmap::Event("lobby: joiner %08X:%u (game port %u) is machine %u; %u joined", ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), p.gamePort, machine,
                         table.Count());
            if (held)
                mpmap::Event("lobby: the running session still holds machines %08X from the last match; the new joiner is not in their roster", held);
            if (mode == Mode::Campaign) {
                // the session starts at the next settle; nothing is pulled in before a restart
                if (!network) ConfigureAsHost();
                char text[120];
                std::snprintf(text, sizeof(text), "PLAYER %u WANTS TO JOIN - RESTART A CHECKPOINT TO LET THEM IN", machine + 1u);
                notice::Enqueue(text);
            } else if (launchedMap >= 0) {
                // a survival lobby: the late joiner is logged above, no notice (12.10.)
                InterlockedIncrement(&lateJoins);
            }
        }
        coop_lobby::Packet reply = coop_lobby::Make(launchedMap >= 0 ? coop_lobby::Type::Launch : coop_lobby::Type::Welcome, token, ++seq,
                                                    static_cast<uint16_t>(localPort), build, randomizer);
        table.FillRoster(reply);
        reply.machine = machine;
        reply.map = launchedMap >= 0 ? static_cast<uint8_t>(launchedMap) : coop_lobby::kNoMap;
        reply.flags = LaunchFlags();
        reply.difficulty = difficulty::LobbyByte();
        reply.seed = HostSeed();
        reply.profile.pad = mesh::seedSync ? 1 : 0;
        reply.members = static_cast<uint8_t>(table.Count());
        reply.reserved = p.seq;
        Send(reply, from.sin_addr.s_addr, from.sin_port);
    }
    if (mode == Mode::Campaign) { CampaignHostTick(now); return; }
    if (launchedMap < 0) {
        if (table.Expire(now, 6000)) mpmap::Event("lobby: a joiner went quiet; %u joined", table.Count());
        SetStatus("hosting - %u joined", table.Count());
        if (stateOutReady && now - stateSentAt >= 250) {
            stateSentAt = now;
            stateOut.token = token;
            for (const auto& m : table.members) {
                if (!m.used) continue;
                stateOut.seq = ++seq;
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_addr.s_addr = m.ip;
                to.sin_port = m.port;
                if (::sendto(sock, reinterpret_cast<const char*>(&stateOut), sizeof(stateOut), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == sizeof(stateOut))
                    InterlockedIncrement(&statesSent);
                else
                    InterlockedIncrement(&sendErrors);
            }
        }
        return;
    }
    // after the launch: announce to the roster for a minute; the lobby stays open
    // for joiners in mid-game (they get the launch as their welcome above) until
    // the host leaves the map
    if (now - launchedAt > 60000) {
        if (mpmap::visit.phase != coop_mpmap::Phase::Idle || InterlockedCompareExchange(&arena::stateMirror, 0, 0) != 0) {
            if (!announceOver) {
                announceOver = true;
                mpmap::Event("lobby: announcement over (%u joiner(s)); still open for joiners while the map runs", table.Count());
            }
            SetStatus("hosting - %u joined (in game)", table.Count());
            return;
        }
        // Stop: the joiners hear a bye and the join item no longer says "hosting" (16.10.)
        mpmap::Event("lobby: the host left the map (%u joiner(s)); lobby closed", table.Count());
        Stop();
        return;
    }
    if (now - lastSend < 300) return;
    lastSend = now;
    for (const auto& m : table.members) {
        if (!m.used) continue;
        coop_lobby::Packet p = coop_lobby::Make(coop_lobby::Type::Launch, token, ++seq, static_cast<uint16_t>(localPort), build, randomizer);
        table.FillRoster(p);
        p.machine = m.machine;
        p.map = static_cast<uint8_t>(launchedMap);
        p.members = static_cast<uint8_t>(table.Count());
        p.flags = LaunchFlags();
        p.difficulty = difficulty::LobbyByte();
        p.seed = HostSeed();
        p.profile.pad = mesh::seedSync ? 1 : 0;
        Send(p, m.ip, m.port);
        InterlockedIncrement(&launchesSent);
    }
}

// The host launched a campaign arena: the same arena here (its campaign launch at the
// arena checkpoint, doors, rooms and story block on this machine too); the host runs the waves.
void JoinArena(DWORD now) {
    const int index = coop_lobby::ArenaIndexOf(joiner.map);
    if (index < 0 || index >= static_cast<int>(coop_arena::kArenaCount)) {
        mpmap::Event("lobby: the host launched arena code %02X, which this build does not know", static_cast<unsigned>(joiner.map));
        SetStatus("join refused: host arena missing here");
        Hint("the host runs another build with arenas this one does not know", "both PCs need the same DS2United.asi");
        Close();
        role = Role::None;
        return;
    }
    const coop_arena::Arena& a = coop_arena::kArenas[index];
    mpmap::Event("lobby: the host launched arena %s (\"%s\"); joining as machine %u, dialling the host's game port %u", a.name, a.label, joiner.machine,
                 joiner.hostGamePort);
    if (!ConfigureAsClient(hostIp, joiner.hostGamePort ? joiner.hostGamePort : hostGamePort, joiner.machine, joiner.roster, coop_lobby::kMaxMembers)) {
        SetStatus("join refused - restart the game");
        Hint("the co-op network could not be set up for this game start", "quit to the desktop, start the game again and join");
        Close();
        role = Role::None;
        return;
    }
    if (joiner.hostMode >= 0) {
        const LONG was = InterlockedExchange(&gamemode::mode, joiner.hostMode);
        mpmap::Event("lobby: the host plays game mode %d (ours was %ld)", joiner.hostMode, was);
    }
    char why[240], fix[200];
    if (SurvivalCheck(a.name, joiner.hostMode >= 0 ? joiner.hostMode : 0, why, fix) == 0) {
        InterlockedIncrement(&survivalWarnings);
        mpmap::Event("lobby: this PC's survival is not ready (%s): no wave rewards or capsules here", why);
        overlay::ShowHint("Your survival is off: no wave rewards or capsules on this PC", why, fix, 15000);
    }
    difficulty::SetLaunch(joiner.hostDifficulty, "the host's lobby launch (arena)");
    AdoptLaunchSeed(joiner.hostSeed, joiner.hasHostSeed);
    const bool started = arena::Start(a.name, now);
    InterlockedIncrement(&joinLaunches);
    mpmap::Event("lobby: arena %s %s", a.name, started ? "started" : "REFUSED (see the arena line)");
    SetStatus("%s arena %s", started ? "loading" : "launch refused:", a.label);
    if (!started) Hint("the host's arena could not be started from the menu", "restart the game and join again; DS2United.log has the arena line");
    Close();
    role = Role::None;
}

void JoinTick(DWORD now) {
    char buffer[512];
    sockaddr_in from{};
    int fromLen = sizeof(from);
    for (int i = 0; i < 32; ++i) {
        const int got = ::recvfrom(sock, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (got <= 0) break;
        if (from.sin_addr.s_addr != hostIp) { InterlockedIncrement(&rejected); continue; }
        coop_lobby::StatePacket state;
        if (coop_lobby::ParseState(buffer, got, state)) {
            if (joiner.token && state.token != joiner.token) { InterlockedIncrement(&rejected); continue; }
            hostState = state;
            hostStateAt = now;
            InterlockedIncrement(&statesIn);
            continue;
        }
        coop_lobby::CampaignPacket c;
        if (coop_lobby::ParseCampaign(buffer, got, c)) {
            InterlockedIncrement(&received);
            if (!joiner.OnCampaign(c, build, randomizer)) {
                if (joiner.state == coop_lobby::Joiner::State::Refused) {
                    mpmap::Event("lobby: campaign join REFUSED: %s", joiner.refusal ? joiner.refusal : "?");
                    SetStatus("join co-op: %s", joiner.refusal ? joiner.refusal : "refused");
                    HintRefusal(joiner.refusal);
                    Close();
                    role = Role::None;
                    return;
                }
                continue;
            }
            mpmap::Event("lobby: the host loads %s (stream %s) checkpoint %08X-%08X; joining as machine %u", c.map, c.stream, c.checkpoint[0], c.checkpoint[1],
                         c.machine);
            if (!ConfigureAsClient(hostIp, c.gamePort ? c.gamePort : hostGamePort, c.machine, c.roster, coop_lobby::kMaxMembers)) {
                SetStatus("join refused - restart the game");
                Hint("the co-op network could not be set up for this game start", "quit to the desktop, start the game again and join");
                Close();
                role = Role::None;
                return;
            }
            difficulty::SetLaunch(coop_difficulty::FromCampaignField(c.difficulty), "the host's campaign packet");
            AdoptLaunchSeed(c.seed, c.seed != 0 || (c.flags & 1) != 0);
            // Born in the right suit: the suit hooks go in now (the session only starts after the
            // first settle, too late for this level's Player::Init), the chapter's suit is armed,
            // and the client progress file may replace it once the host's id arrives (CKPT).
            ::coop_uniform::Arm();
            loadsync::ArmJoinSuitForCheckpoint(c.checkpoint, "a story join");
            const bool started = mpmap::StartCampaignFromMenu(c.map, c.stream, c.checkpoint);
            SetStatus("%s", started ? "loading the host's game" : "join refused - see DS2United.log");
            if (!started) Hint("the host's campaign level could not be started here", "both PCs need the same game files; DS2United.log has the reason");
            Close();
            role = Role::None;
            return;
        }
        coop_lobby::Packet p;
        if (!coop_lobby::Parse(buffer, got, p)) {
            InterlockedIncrement(&rejected);
            const int other = coop_lobby::OtherVersion(buffer, got);
            if (other >= 0 && joiner.state != coop_lobby::Joiner::State::Idle) {
                mpmap::Event("lobby: join REFUSED: the host speaks lobby version %d, ours is %u (another DS2Coop build)", other, coop_lobby::kVersion);
                joiner.state = coop_lobby::Joiner::State::Refused;
                joiner.refusal = "the host runs a different DS2Coop build";
                SetStatus("join co-op: %s", joiner.refusal);
                HintRefusal(joiner.refusal);
                Close();
                role = Role::None;
                return;
            }
            continue;
        }
        InterlockedIncrement(&received);
        if (p.reserved && (p.type == static_cast<uint8_t>(coop_lobby::Type::Welcome) || p.type == static_cast<uint8_t>(coop_lobby::Type::Launch)) &&
            helloSeq[p.reserved & 7] == p.reserved) {
            const DWORD rtt = now - helloAt[p.reserved & 7];
            helloSeq[p.reserved & 7] = 0;
            if (rtt < 5000) pingMs = static_cast<uint16_t>(pingMs ? (pingMs * 3u + rtt) / 4u : (rtt ? rtt : 1u));
        }
        const auto before = joiner.state;
        const bool launch = joiner.OnPacket(p, build, randomizer);
        if (joiner.state == coop_lobby::Joiner::State::Refused) {
            mpmap::Event("lobby: join REFUSED by the host: %s", joiner.refusal ? joiner.refusal : "?");
            SetStatus("join co-op: %s", joiner.refusal ? joiner.refusal : "refused");
            HintRefusal(joiner.refusal);
            Close();
            role = Role::None;
            return;
        }
        if (before == coop_lobby::Joiner::State::Hello && joiner.state != before)
            mpmap::Event("lobby: welcomed by the host as machine %u (%u joined, token %08X)", joiner.machine, joiner.members, joiner.token);
        if (joiner.state == coop_lobby::Joiner::State::Welcomed)
            SetStatus("joined as player %u - waiting for host", launcherJoin ? machineIndex + 1 : joiner.machine + 1u);
        if (launch && coop_lobby::IsArenaCode(joiner.map)) {
            JoinArena(now);
            return;
        }
        if (launch) {
            if (joiner.map < 0 || joiner.map >= static_cast<int>(coop_mpmap::kMapCount) || !mpmap::MapAvailable(joiner.map)) {
                const bool known = joiner.map >= 0 && joiner.map < static_cast<int>(coop_mpmap::kMapCount);
                mpmap::Event("lobby: the host launched map index %d (%s), which this machine %s", joiner.map, coop_mpmap::MapName(joiner.map),
                             known ? "has in no mounted archive (Outbreak: DS2DAT12.DAT)" : "does not know (another build)");
                SetStatus("join refused: host map %s missing here", known ? coop_mpmap::MapLabel(joiner.map) : "?");
                Hint(known ? "the host picked an Outbreak map and this game has no DS2DAT12.DAT" : "the host runs another build with maps this one does not know",
                     known ? "install the Outbreak maps (DS2DAT12.DAT), or ask the host for another map" : "both PCs need the same DS2United.asi");
                Close();
                role = Role::None;
                return;
            }
            const char* map = coop_mpmap::MapName(joiner.map);
            mpmap::Event("lobby: the host launched %s; joining as machine %u, dialling the host's game port %u", map, joiner.machine, joiner.hostGamePort);
            if (!ConfigureAsClient(hostIp, joiner.hostGamePort ? joiner.hostGamePort : hostGamePort, joiner.machine, joiner.roster, coop_lobby::kMaxMembers)) {
                SetStatus("join refused - restart the game");
                Hint("the co-op network could not be set up for this game start", "quit to the desktop, start the game again and join");
                Close();
                role = Role::None;
                return;
            }
            for (const auto& r : joiner.roster)
                if (r.machine) mpmap::Event("lobby: roster machine %u at %08X game port %u%s", r.machine, ntohl(r.ip), r.gamePort, r.machine == joiner.machine ? " (us)" : "");
            if (joiner.hostMode >= 0) {
                const LONG was = InterlockedExchange(&gamemode::mode, joiner.hostMode);
                mpmap::Event("lobby: the host plays game mode %d (ours was %ld)", joiner.hostMode, was);
            }
            // our own director only drops our rewards and capsule copies; the host runs the waves
            if (joiner.hostMode != 4) {
                char why[240], fix[200];
                const int ready = SurvivalCheck(map, joiner.hostMode >= 0 ? joiner.hostMode : 0, why, fix);
                if (ready == 0) {
                    InterlockedIncrement(&survivalWarnings);
                    mpmap::Event("lobby: this PC's survival is not ready (%s): no wave rewards or capsules here", why);
                    overlay::ShowHint("Your survival is off: no wave rewards or capsules on this PC", why, fix, 15000);
                }
            }
            difficulty::SetLaunch(joiner.hostDifficulty, "the host's lobby launch");
            AdoptLaunchSeed(joiner.hostSeed, joiner.hasHostSeed);
            const bool started = mpmap::StartVisitFromMenu(map, now);
            InterlockedIncrement(&joinLaunches);
            mpmap::Event("lobby: launch of %s %s", map, started ? "started" : "REFUSED (see the menu start line)");
            SetStatus("%s %s", started ? "loading" : "launch refused:", MapLabel(joiner.map));
            if (!started) Hint("the host's map could not be started from the menu", "restart the game and join again; DS2United.log has the 'menu start' line");
            Close();
            role = Role::None;
            return;
        }
    }
    if (now - lastSend >= 500) {
        lastSend = now;
        coop_lobby::Packet hello = coop_lobby::Make(coop_lobby::Type::Hello, joiner.token, ++seq, static_cast<uint16_t>(localPort), build, randomizer);
        hello.reserved = mpmap::AvailableMask();   // the host offers only maps every joiner can load
        hello.profile = OwnProfile(joiner.machine);
        helloSeq[seq & 7] = seq;
        helloAt[seq & 7] = now;
        Send(hello, hostIp, htons(static_cast<u_short>(hostGamePort + coop_lobby::kLobbyPortOffset)));
        // a launcher's client waits for whatever the host starts: no lobby answer is no fault
        if (joiner.state == coop_lobby::Joiner::State::Hello && now - startedAt > 3000)
            SetStatus(launcherJoin ? "joining - waiting for the host's game (%lu s)" : "joining - no answer for %lu s", static_cast<unsigned long>((now - startedAt) / 1000));
        if (joiner.state == coop_lobby::Joiner::State::Hello && now - startedAt > 10000 && !noAnswerHinted && !launcherJoin) {
            noAnswerHinted = true;
            Hint("the host does not answer on its lobby port",
                 "check [Coop] JoinAddress (the host's Hamachi or LAN address), that the host picked host co-op first, and that the firewall lets the game through");
        }
    }
}

// Back in the main menu with no lobby open: the join item says what it does now. A
// "loading <map>" from the last launch stayed on it after the match was left (13.10.).
void IdleStatus(DWORD now) {
    if (now - idleStatusAt < 500) return;
    idleStatusAt = now;
    uint32_t director = 0;
    if (!Read(base + 0x1C8BE90, &director, 4) || !director) return;   // the Apt director exists in the main menu only
    char map[64] = "";
    if (session && !IsHost() && level::HostMpMap(now, map, sizeof(map))) SetStatus("rejoin co-op: %s", MapLabel(coop_mpmap::MapIndex(map)));
    else if (_strnicmp(status, "loading", 7) == 0 || _strnicmp(status, "rejoin co-op", 12) == 0) SetIdleStatus();
}
void Tick(DWORD now) {
    LoadJoinRole();
    if (role == Role::None) IdleStatus(now);
    if (role == Role::None || sock == INVALID_SOCKET) return;
    // The launcher's client went into the host's story level by the level follow: a lobby
    // launch must not start a second load behind it.
    if (role == Role::Join && launcherJoin) {
        uint32_t director = 0;
        if (Read(base + 0x1C8BE90, &director, 4) && !director) {
            mpmap::Event("lobby: launcher join closed: this copy left the main menu (status '%s')", status);
            Stop();
            return;
        }
    }
    if (role == Role::Host) HostTick(now);
    else JoinTick(now);
}
// For the loading barrier (runtime_coop_loadsync.inc): the machines a campaign restart or a
// survival launch is pulling in right now, bit n = machine n, and since when.
uint32_t PendingJoiners(DWORD now, DWORD* since) {
    if (since) *since = 0;
    if (role != Role::Host || !IsHost()) return 0;
    const bool campaign = mode == Mode::Campaign && campaignArmed;
    const bool survival = mode == Mode::Survival && launchedMap >= 0 && !announceOver && TicksSince(now, launchedAt) < 60000;
    if (!campaign && !survival) return 0;
    uint32_t mask = 0;
    for (const auto& m : table.members)
        if (m.used && m.machine >= 1 && m.machine < 32) mask |= 1u << m.machine;
    if (since) *since = campaign ? campaignAt : launchedAt;
    return mask;
}
void WriteDiagnostics(FILE* log) {
    if (!build) return;
    std::fprintf(log, "lobby mode=%s campaign_launches=%ld late_joins=%ld rejoins=%ld hints=%ld survival_warnings=%ld\n", mode == Mode::Campaign ? "campaign" : "survival",
                 InterlockedCompareExchange(&campaignLaunches, 0, 0), InterlockedCompareExchange(&lateJoins, 0, 0), InterlockedCompareExchange(&rejoins, 0, 0),
                 InterlockedCompareExchange(&hintsShown, 0, 0), InterlockedCompareExchange(&survivalWarnings, 0, 0));
    std::fprintf(log, "lobby list states sent=%ld in=%ld byes sent=%ld in=%ld ping=%u ms join_launches=%ld\n", InterlockedCompareExchange(&statesSent, 0, 0),
                 InterlockedCompareExchange(&statesIn, 0, 0), InterlockedCompareExchange(&byesSent, 0, 0), InterlockedCompareExchange(&byesIn, 0, 0), pingMs,
                 InterlockedCompareExchange(&joinLaunches, 0, 0));
    std::fprintf(log, "lobby host session reused=%ld dials_added=%ld running=%d | launcher (DS2COOP=%d host=%d) host_kept=%ld client_kept=%ld joins=%ld host_refusals=%ld join_open=%d\n",
                 InterlockedCompareExchange(&hostSessionReuses, 0, 0), InterlockedCompareExchange(&hostSessionDials, 0, 0), HostSessionRunning() ? 1 : 0,
                 fromEnvironmentRole ? 1 : 0, IsHost() ? 1 : 0, InterlockedCompareExchange(&launcherHostKeeps, 0, 0),
                 InterlockedCompareExchange(&launcherClientKeeps, 0, 0), InterlockedCompareExchange(&launcherJoins, 0, 0),
                 InterlockedCompareExchange(&launcherRefusals, 0, 0), launcherJoin && role == Role::Join ? 1 : 0);
    std::fprintf(log, "lobby role=%s token=%08X build=%08X joined=%u admitted=%ld sent=%ld received=%ld rejected=%ld launches_sent=%ld send_errors=%ld status='%s'\n",
                 role == Role::Host ? "host" : role == Role::Join ? "join" : "none", token, build, table.Count(), InterlockedCompareExchange(&admitted, 0, 0),
                 InterlockedCompareExchange(&sent, 0, 0), InterlockedCompareExchange(&received, 0, 0), InterlockedCompareExchange(&rejected, 0, 0),
                 InterlockedCompareExchange(&launchesSent, 0, 0), InterlockedCompareExchange(&sendErrors, 0, 0), status);
}
}  // namespace lobby
