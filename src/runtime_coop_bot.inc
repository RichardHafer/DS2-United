// Dead Space 2 United by Hafer.
// A companion bot: a second, local Isaac the game itself runs ([Bot]).
//
// The bot is a full, non-ghost Player spawned from the level's own Player
// template on the machine that stands in the level (solo or host). Every
// action (walking, turning, aiming, the plasma cutter, melee, reload, stasis,
// med packs, pickups) is the game's own Player code, driven through a virtual
// controller: pad 14, whose raw state ([0x0201C0A8] + 0x2C + 14 * 0x44, sticks
// as floats at +0..+0xC, buttons at +0x10) is written once per
// ControllerManager update from the pad filter hook. What the bot wants comes
// from coop_bot_brain.h, how it gets there from coop_bot_steer.h and NavPower
// (runtime_coop_bot_nav.inc), what it sees from runtime_coop_bot_sense.inc.
//
// Making a second local Player behave:
//  * id 1 in the PlayerManager (Register 0x0077BB60 with an explicit id; left
//    at -1, Player::Init would register it into slot 0 and take over Isaac).
//  * Its cameras go to CameraManager viewport 1 ([0x0202131C]): the manager is
//    told there are two viewports (+0x164 = 2) so every lookup for id 1 finds
//    blender 1 instead of reading past the array, and SetRenderCamera
//    (0x00793470) returns at once for a viewport without a render context, so
//    nothing is drawn twice.
//  * Its controller config is moved from pad 15 (Isaac's, which the ctor gives
//    every Player) to pad 14: PopConfiguration(15), m_controllerID (+0x41C) = 14,
//    SetControllerEventConfig (0x008694E0) pushes it onto 14; the Player's own
//    destructor pops it again.
//  * Player::Init restores the saved record into every SP Player, clearing the
//    global UpgradeManager first: Clear (0x01354CC0) and Restore (0x01354F60)
//    are skipped while the bot initializes, so Isaac's upgrades stay. Its own
//    Player::Store never runs (save guard), its death never runs (damage floor,
//    no scripted deaths, ProcessDeath refused: the death path restarts the
//    level for whichever Player dies), NO_PAIRED keeps grabs off it, and GUID
//    lookups of "the player" get Isaac (guidfix).
//  * A checkpoint restart deletes every registered Player, the bot with them;
//    the module only lets go (OnRestart) and spawns a new one once the level
//    stands again.
namespace respawn { bool SwallowDeathMessage(uint32_t owner, uint32_t message); }   // runtime_coop_respawn.inc
namespace escalation { float StasisFactor(uint32_t entity); }   // runtime_coop_escalation.inc
namespace mpnative { bool InVersus(); int HumanSideHumans(); }   // runtime_coop_mpnative.inc
namespace arena { bool RunningNow(); int ModeNow(int picked); extern volatile LONG starts; }   // runtime_coop_arena.inc
namespace mpmap { bool RespawnSpot(const float* toward, float* out); }   // runtime_coop_mpmap.inc
namespace mpmap { unsigned Run(); }                                     // runtime_coop_mpmap.inc: the arrival count of a visit
namespace gamemode { extern volatile LONG mode; }                       // runtime_coop_gamemode.inc: 0 survival, 1 mercenaries, 2 objective, 3 zone, 4 versus
namespace turret { uint32_t SimMs(); }                                  // runtime_coop_turret.inc: game time, stands still in the pause
namespace shop { extern uint32_t priceGun[coop_shop::kGunCount]; }      // runtime_coop_shop.inc: the supply terminal's gun prices
namespace overlay { bool CameraNow(coop_marks::Camera& c, int W, int H); }   // runtime_coop_overlay.inc
namespace bot {
using coop_bot::Vec3;
bool enabled = false;
int toggleKey = 0, callKey = 0;
bool keyDown = false, callDown = false, userOff = false, markDown = false;
DWORD leaderShotAt = 0;
// [Bot] MarkKey: writes a marked line and every bot's state at once, for "it just happened" reports.
int markKey = 105;
volatile LONG marks = 0;
volatile LONG calls = 0;
DWORD spawnDelayMs = 4000;
constexpr uint32_t kPlayerManagerRva = 0x1C1C0D4, kPmRegisterRva = 0x37BB60, kPmUnregisterRva = 0x37BC00;
constexpr uint32_t kCameraManagerRva = 0x1C2131C, kCmBlenders = 0x10, kCmRenderCtx = 0x14, kCmNumViewports = 0x164, kCmPlayerIds = 0x168,
                   kBlenderSize = 0x2A0, kBlenderStackIndex = 0xD4;
constexpr uint32_t kSetRenderCameraRva = 0x393470;
constexpr uint32_t kControllerManagerRva = 0x1C1C0AC, kCmiBase = 0x10, kCmiSize = 0x168, kPushConfigRva = 0x37E610, kPopConfigRva = 0x37E6D0;
constexpr uint32_t kSetControllerConfigRva = 0x4694E0, kDeviceManagerRva = 0x1C1C0A8, kPadBase = 0x2C, kPadSize = 0x44;
constexpr uint32_t kUpgradeClearRva = 0xF54CC0, kUpgradeRestoreRva = 0xF54F60;
constexpr uint32_t kPlayerController = 0x41C, kPlayerId = 0x420, kPlayerFlags = 0x3DC, kPlayerConfigGuid = 0x3C8, kClosestPickup = 0x40C,
                   kPairedControl = 0x2A8, kAimingData = 0x6A0, kCamAim = 0x20, kGunAim = 0xC0, kBasePlayer = 0x3C0;
constexpr uint32_t kFlagNoPaired = 1u << 30;
constexpr uint32_t kActiveItem = 0xCEC, kPlasmaCutterVtableRva = 0x17515D8;
constexpr uint16_t kNextItem = 0x0002;
constexpr uint16_t kBackButton = 0x0080;   // a kiosk menu's Back, read raw from the pad
constexpr uint32_t kPadConnected = 0x3E;   // the pad record's "connected" byte
constexpr uint16_t kAltFire = 0x0800;   // the stomp button; while aiming it is the weapon's alt fire
constexpr uint16_t kAim = 0x0100, kFire = 0x0400, kReload = 0x0040, kStasis = 0x0010, kMedpack = 0x0080, kUse = 0x0020, kRun = 0x0200;
constexpr float kEye = 1.6f, kPi = 3.14159265f;

safetyhook::InlineHook renderHook, clearHook, restoreHook, deathSmHook, evaluateHook;
bool renderGuard = false, upgradeGuard = false;

enum class State : uint8_t { Off, Waiting, Calibrating, Active, Failed };
coop_bot::Trail trail;
DWORD lastCabinCheck = 0;
uint16_t lastSeen15 = 0;
bool leaderInCabin = false;
Vec3 leaderAt;
// A bot's stasis cast holds its target for the squad while the shot flies; whether it
// hit shows in the enemy's time multiplier (+0x84, the one the stasis mine checks):
// slowed below kStasisSlowBelow counts as stasised for as long as it lasts. A missed
// cast (a twitcher's dodge) no longer blocks the next one for 6 s.
struct Stasised { uint32_t npc; DWORD until; };
Stasised stasised[8]{};
constexpr uint32_t kNpcTimeMultiplier = 0x84, kStasisPendingMs = 1500;
constexpr float kStasisSlowBelow = 0.5f;   // speed variants run at 0.9 and above
volatile LONG stasisSlowSeen = 0;
Vec3 leaderHistory[8];
DWORD leaderHistoryAt[8]{};
unsigned leaderHistoryNext = 0;

// The pad, computed on the tick and written by the filter hook (both on the game thread).
struct Pad { float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f; uint16_t buttons = 0; };

// Calibration: which way each stick turns or moves this Isaac, and how fast.
struct Calibration {
    // The left stick's signs: up is negative on a DirectInput pad (the keyboard's W
    // gives -1 in its slot), and every measurement of the side said -1. Measuring
    // them at spawn was flaky (26.09.: forward came out +1 once and the bot walked
    // backwards away from its goals), so they are fixed.
    float yawSign = 1.0f, pitchSign = 1.0f, forwardSign = -1.0f, strafeSign = -1.0f;
    float yawRate = 2.5f, pitchRate = 1.5f;   // rad/s at full stick
    unsigned step = 0;
    DWORD stepAt = 0;
    float startHeading = 0.0f, startPitch = 0.0f;
    Vec3 startPos;
    bool done = false;
};

// One bot. Up to kMaxBots of them, each its own Player slot (1..3), virtual pad
// (14, 13, 12) and CameraManager viewport (the slot's number). `u` is the one
// the code below works on; the tick sets it for each in turn.
enum class KioskStep : uint8_t { None, Approach, Align, Press, Inside, Leave, Browse };
enum class TkStep : uint8_t { None, Raise, Grab, Hold, Throw };
// Turning away from a wall at rest ([Bot] FaceOpenSpace).
enum class FaceStep : uint8_t { None, Turn, Step };
struct FaceUnit {
    FaceStep step = FaceStep::None;
    Vec3 restFrom;
    DWORD restSince = 0, lastAt = 0, stepAt = 0;
    bool done = false, keepLook = false, walk = false;
    float yaw = 0.0f;
};
// Its kit outside the campaign ("the kit outside the campaign", below the weapons).
struct ProgressUnit {
    unsigned run = 0;   // the run this plan belongs to, 0 = none
    uint32_t seed = 0;
    int item[coop_bot_progress::kSteps] = {-1, -1, -1, -1};   // per step a gun (kGunCatalogue) or the suit (kSuits)
    float due[coop_bot_progress::kSteps]{};                    // completed waves or round seconds, 0 = never
    uint8_t reached = 0, done = 0, failed = 0, waitLogged = 0;
    uint8_t tries[coop_bot_progress::kSteps]{};
    DWORD reachedAt[coop_bot_progress::kSteps]{}, retryAt[coop_bot_progress::kSteps]{}, startedAt[coop_bot_progress::kSteps]{};
    int pendingStep = -1;
    uint32_t pendingEntity = 0;
    uint32_t granted = 0;   // guns this run put in its hands
    int suitOrder[coop_bot_progress::kMaxSuits]{};
    int suitCount = 0, suitPos = 0;
    DWORD checkAt = 0;
    char gunWhy[64] = "";   // its last gun outcome (GunEvent)
    // Since its first plan outside the campaign: what it owned before, and what it was left at.
    bool stretch = false;
    uint32_t ownedBefore = 0, ownedSeen = 0;
    int suitBefore = -1, suitSeen = -1;
};
struct Unit {
    unsigned index = 0, controller = 14, slotId = 1;
    uint32_t entity = 0;
    // Versus: the last hit that got through (DamageData+0x88 the attacker's ref, +0x90 its flags)
    uint64_t lastHitRef = 0;
    uint16_t lastHitFlags = 0;
    DWORD lastHitAt = 0;
    DWORD lastWeaponSwitch = 0;
    unsigned weaponSwitches = 0;
    bool noCutter = false;
    State state = State::Off;
    DWORD settledSince = 0, spawnedAt = 0, dressCheckAt = 0, lastScan = 0, lastQuery = 0, lastStatus = 0;
    Vec3 spawnTarget{}, spawnLast{};
    DWORD spawnSampleAt = 0;
    unsigned spawnGuardTries = 0;
    bool guardSpawn = false;
    unsigned dressTries = 0;
    uint32_t isaacBody[3]{};   // Isaac's mmi, bones, uniform when its body check started
    int isaacSuit = -1;        // the suit Isaac wore then (kSuits index)
    unsigned isaacWaits = 0;   // checks put off while Isaac's own suit streams in
    bool failedForGood = false;   // out until the level changes or the key calls it back
    coop_bot::Brain brain;
    coop_bot::Steer steer;
    bot_sense::Snapshot seen;
    bot_sense::Snapshot scanNext;   // the scan under way ("Sense"); replaces seen once every enemy in it is sighted
    bool scanActive = false;
    uint32_t scanEntity = 0;
    DWORD scanStartedAt = 0;
    coop_bot::Command command;
    Vec3 self;
    Pad pad;
    bool padLive = false;
    DWORD pressUntil[16]{};
    Calibration cal;
    uint16_t lastWritten = 0;
    bool haveWritten = false;
    DWORD lastLookAt = 0;
    bool storeWaiting = false, storeWaitArrived = false, storeWaitClear = false, keepingOut = false;
    DWORD storeWaitSince = 0, storeWaitRayAt = 0, storeWaitAimUntil = 0;
    int storeWaitTaps = 0;
    uint32_t crateSightId = 0;
    DWORD crateSightAt = 0;
    bool crateSightClear = false;
    LONGLONG lastLookQpc = 0;
    float lastTurnSign = 0.0f;
    uint32_t lastSpear = 0;
    DWORD spearHitAt = 0, altWantedAt = 0, stompWantedAt = 0;
    uint32_t slotGun[coop_bot::kGunSlots]{};
    DWORD wishSince = 0;
    uint32_t wishGun = 0;
    // Power nodes: the bot's own count, kept across new bodies (respawns, levels).
    int32_t nodeWallet = 0, nodesSeen = -1;
    bool nodesSet = false;
    // Credits, the same way. walletFresh: nothing was ever put in (TrackCredits gives it a start).
    int32_t creditWallet = 0, creditsSeen = -1;
    bool creditsSet = false, walletFresh = true;
    // A kiosk visit (runtime_coop_bot.inc, "using the kiosks").
    KioskStep kioskVisit = KioskStep::None;
    uint32_t kiosk = 0;
    bool kioskStore = false, kioskBought = false, kioskInward = false;
    int kioskSide = 0, kioskPresses = 0;
    DWORD kioskStepAt = 0, kioskLeaveAt = 0, kioskBackAt = 0;
    unsigned backStep = 0;   // a Back press on the pad: 1 a clear frame, 2..3 pressed, 4 released
    unsigned rawStep = 0;    // the same for buttons the game reads raw (the kinesis throw), over the normal pad
    uint16_t rawBits = 0;
    // Where the muzzle sits beside the bot, in the aim's own frame (right, up,
    // forward of the gun's heading), from the last muzzle matrix.
    float muzzleRight = 0.0f, muzzleUp = 0.0f, muzzleForward = 0.0f;
    DWORD muzzleAt = 0;
    DWORD routeBlockedAt = 0; // the steering last said the goal is out of reach
    DWORD fireAt = 0, shotMeasuredAt = 0;   // the trigger, and the last shot measured
    DWORD ammoCheckAt = 0;
    bool died = false;   // its death state started: the body is replaced by a new one
    DWORD diedAt = 0;    // Versus: when it died; the body lies until the new one comes
    DWORD zeroSince = 0, zeroCheckAt = 0;   // its health at zero without a death message (the fail-safe in TickUnit)
    uint32_t blockedBy = 0;   // whoever sits in its Player slot while it has no body
    DWORD blockedSince = 0;
    bool farSkipped = false;   // far from the leader on a survival map, no catch-up (counted once per stretch)
    bool wasGrabbed = false;
    DWORD suitAt = 0;        // when its own suit is put on ([Bot] Suit=1)
    bool suitDone = false;
    // A kinesis throw ([Bot] Kinesis=1, "kinesis").
    TkStep tkStep = TkStep::None;
    uint32_t tkObject = 0, tkBody = 0, tkTarget = 0;
    int tkPresses = 0, tkMedpacks = -1;
    bool tkCanister = false;
    bool alert = false, tkWants = false;   // an enemy within 10 m; minded to take a kinesis object this alert
    DWORD alertSeenAt = 0, tkRerollAt = 0, tkCloseSince = 0;
    DWORD tkLagSince = 0, tkHiddenSince = 0, tkSightAt = 0;   // the held object hanging back or out of sight (snagged)
    bool tkHidden = false;
    bool tkLaunchWanted = false;
    const char* tkWaitWhy = nullptr;   // what last kept a held object from being thrown (KinesisTick, Hold)
    // Hidden for the leader's scene (the hide count of each, and what it was).
    uint32_t hiddenBody = 0, hiddenItem = 0, hiddenHelmet = 0, hiddenBodyVt = 0, hiddenItemVt = 0, hiddenHelmetVt = 0;
    DWORD sceneSince = 0, farSince = 0, cabinMissSince = 0, cabinCheckAt = 0;
    bool sceneHides = false;   // the running scene is of a kind that hides the bots
    bool catchUpPending = false;   // a catch-up held back while the leader climbed
    DWORD desperateAt = 0;         // its last desperate shot: its own blast spares it for a while
    DWORD liftHoldUntil = 0;       // put down in a lift cabin: stands still until then
    coop_bot::Enemy refreshed;     // the target as last measured
    DWORD refreshAt = 0;   // the next launch transition of its kinesis machine goes through
    DWORD tkHeldSince = 0, tkMoveCheckAt = 0;   // carrying since; the last check that it gets anywhere
    // Its profile (DS2BotProfiles.ini): name, turn speed, walking pace, kinesis mood, favourite suit.
    char name[32] = "bot";
    char favoriteSuit[64] = "";
    int suitFavorites[8]{};   // kSuits indices from its FavoriteSuits, best first
    int suitFavoriteCount = 0;
    int suitForced = -1;       // kSuits index from its ForceSuit: always worn, owned or not
    int suitChosen = -1;      // the suit it picked, kept across bodies
    DWORD suitUrgentAt = 0;
    float aimSpeed = 1.0f, walkPace = 1.0f;
    int kinesisChance = 50;
    int favorites[10]{};   // catalogue indices from its FavoriteWeapons, best first
    int favoriteCount = 0;
    int medpacks = -1;   // carried med packs, read every 500 ms
    DWORD medpacksAt = 0;
    // The leader's Hold or Go there ("the leader's orders"): its spot, and whether it is still on its way.
    bool holding = false, goingThere = false;
    Vec3 holdSpot;
    DWORD holdSince = 0;
    // Guns it bought (catalogue bits, kept across bodies) and a purchase or hand-back under way.
    uint32_t gunsOwned = 0, gunProxy = 0, gunPrice = 0;
    int gunPending = -1;
    DWORD gunStartedAt = 0, gunCheckAt = 0;
    Vec3 tkMoveFrom;
    DWORD tkStepAt = 0, tkNextAt = 0, tkMedpackUntil = 0, tkReleasedAt = 0, tkAimAt = 0;
    bool tkRedirect = false;   // its kinesis machine aims at tkRedirectTo, not the human's crosshair
    Vec3 tkRedirectTo, tkAim;
    // Zero-G ("zero-G flight"): its machines, its body frame, the flight.
    int32_t lowerState = -1, zgState = -1;
    uint32_t zgSm = 0;
    bool zeroG = false, flying = false, bodyKnown = false, zgClear = true, zgWantsCatchUp = false;
    coop_bot::BodyFrame body, signFrame;
    DWORD flightStart = 0, landingSince = 0, takeoffAt = 0, landPulseAt = 0, landHoldUntil = 0, otherSurfaceSince = 0, zgClearAt = 0, zgLogAt = 0,
          noSpotLogAt = 0, signFrameAt = 0;
    float flightYaw = 0.0f, flightPitch = 0.0f, flightYawSign = 1.0f, flightPitchSign = 1.0f, signYaw = 0.0f, signPitch = 0.0f;
    int yawContrary = 0, pitchContrary = 0;
    // Moved with its stick still ("CheckDrift").
    DWORD stickStillSince = 0, driftAt = 0, noPairedAt = 0;
    uint32_t targetHealthOf = 0, targetHealth = 0;   // the target's health component, looked up once per target
    Vec3 driftFrom, driftLeaderFrom;
    // Kinesis and stasis ("kinesis and stasis modules"): its module bits and stasis energy.
    uint32_t modulesSeen = 0;
    bool modulesKnown = false, tkModule = true, stasisKnown = false;
    float stasis01 = 1.0f;
    // Looting and the rescue ("corpses, crates and lockers", "the leader in a grab").
    uint32_t lootLast = 0;
    bool rescuing = false;
    coop_bot::LootSpot lootSpots[coop_bot::kMaxLoot];
    int lootSpotCount = 0;
    DWORD lootScanAt = 0;
    FaceUnit face;
    // Antics (runtime_coop_bot_antics.inc): its own last kill, the line to the leader's chest.
    uint32_t killId = 0;
    DWORD killAt = 0, killPosAt = 0, lineAt = 0;
    Vec3 killPos;
    int leaderLine = -1;
    ProgressUnit prog;
};
Unit units[kMaxBots];
Unit* u = &units[0];
uint32_t pastBots[8]{};
// The bots' frame budget: time spent in the bot tick and its costly parts,
// logged every 10 s with the memory reads per frame.
struct PerfSlot { LONGLONG ticks = 0, maxTicks = 0; LONG calls = 0; };
PerfSlot perfTick, perfScan, perfSight, perfRefresh, perfCabins, perfThrowables, perfNav, perfBrain, perfKinesis, perfAct, perfLeader;
unsigned botFrame = 0, scannedFrame = ~0u;   // one bot's entity scan a frame
// A scan is a cheap walk over the entity chain, then each enemy's limbs and the
// rays to them; those take turns, kSightsPerFrame a frame for all bots together.
constexpr DWORD kScanMs = 250;
constexpr int kSightsPerFrame = 4;
int sightsLeft = 0;
unsigned sightFrame = ~0u;
volatile LONG scanWalks = 0, scanSights = 0, scanDone = 0, scanDropped = 0, scanLagMsMax = 0, scanLagMsSum = 0;
LONGLONG perfFreq = 0;
struct Perf {
    PerfSlot& slot;
    LARGE_INTEGER start{};
    explicit Perf(PerfSlot& s) : slot(s) { QueryPerformanceCounter(&start); }
    ~Perf() {
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        const LONGLONG d = end.QuadPart - start.QuadPart;
        slot.ticks += d;
        if (d > slot.maxTicks) slot.maxTicks = d;
        ++slot.calls;
    }
};
constexpr DWORD kRefreshMs = 100;
// Lift cabins: the Movers found by the SimManager walk (4096 buckets) are kept
// for 10 s; their boxes are read fresh each time. The walk is every entity of the
// level and took 11-21 ms in one frame (05.10.), so it goes kCabinBuckets a frame
// (StepCabinWalk) and the last complete list stays in use meanwhile.
constexpr uint32_t kCabinBuckets = 256;
uint32_t cabinMovers[16]{}, cabinWalkFound[16]{};
unsigned cabinMoverCount = 0, cabinWalkCount = 0;
uint32_t cabinWalkBucket = 0;
bool cabinWalking = false;
DWORD cabinMoversAt = 0;
volatile LONG cabinWalks = 0;
void StepCabinWalk(DWORD now) {
    if (!cabinWalking) {
        if (cabinMoversAt && now - cabinMoversAt <= 10000) return;
        cabinWalking = true;
        cabinWalkBucket = 0;
        cabinWalkCount = 0;
    }
    const Perf timer(perfCabins);
    const uint32_t to = cabinWalkBucket + kCabinBuckets;
    cabinWalkCount += ghosttrigger::MoversIn(cabinWalkBucket, to, cabinWalkFound + cabinWalkCount, 16 - cabinWalkCount);
    cabinWalkBucket = to;
    if (cabinWalkBucket < 4096 && cabinWalkCount < 16) return;
    std::memcpy(cabinMovers, cabinWalkFound, sizeof(cabinMovers));
    cabinMoverCount = cabinWalkCount;
    cabinMoversAt = now;
    cabinWalking = false;
    InterlockedIncrement(&cabinWalks);
}
unsigned CachedCabins(ghosttrigger::Cabin* out, unsigned cap, DWORD) {
    unsigned n = 0;
    for (unsigned i = 0; i < cabinMoverCount && n < cap; ++i)
        if (ghosttrigger::CabinBox(cabinMovers[i], out[n])) { out[n].mover = cabinMovers[i]; ++n; }
    return n;
}
bool leaderBusy = false;   // the leader climbs or rides ("scenes and catching up"): no teleports
bool chaseFollow = false, leaderChase = false;   // [Bot] ChaseFollow; the straitjacket run ("scenes and catching up")
volatile LONG deadTargetsDropped = 0;   // targets found dead between two scans (Sense)
DWORD liftHoldMs = 6000;   // [Bot] LiftHoldMs
volatile LONG liftHolds = 0, liftHoldFrames = 0;
void HoldInLift(const Vec3& spot, DWORD now, const char* why, bool inCabin);   // "scenes and catching up"
// Zero-G ("zero-G flight"): the switch and the leader's side, read once a frame.
bool zeroGFlight = false;   // [Bot] ZeroGFlight
int32_t leaderLower = -1;
bool leaderFlying = false, leaderZeroG = false, leaderBodyKnown = false;
DWORD leaderFlyingSince = 0;
coop_bot::BodyFrame leaderBody;
Vec3 ZeroGSpot(const Vec3& leaderPos);
DWORD leaderBusyUntil = 0;   // bodies let go of, still told apart from the human's (see "using the kiosks")
unsigned pastBotNext = 0;
unsigned wantedBots = 1;
// The engine lists of bots (coop::botEntities) follow the units.
void Publish() {
    for (unsigned i = 0; i < kMaxBots; ++i) {
        botEntities[i] = units[i].entity;
        botHolding[i] = units[i].entity && units[i].holding;
    }
}
void SnapIsaac(Unit& unit, uint32_t leader);   // "suits", below
Unit* UnitOf(uint32_t entity) {
    if (!entity) return nullptr;
    for (Unit& unit : units) if (unit.entity == entity) return &unit;
    return nullptr;
}

volatile LONG spawns = 0, spawnFailures = 0, drops = 0, dressings = 0, isaacRepairs = 0, teleports = 0, deathsRefused = 0, damageBlocked = 0,
              shots = 0, melees = 0, heals = 0, stasisCasts = 0, pickups = 0, reloads = 0, navAsked = 0, navFound = 0, shadowTicks = 0,
              unreachable = 0, laneHolds = 0, limbShots = 0, chestShots = 0, botDeaths = 0, struggles = 0, mashFrames = 0, regenHolds = 0;
// Frames a bot walked in a fight, by reason (coop_bot::MoveWhy: other, follow, retreat, chase, relocate, step, lane).
volatile LONG combatMoves[static_cast<unsigned>(coop_bot::MoveWhy::Count)]{};

// --- engine helpers ---------------------------------------------------------

uint32_t Leader() {
    uint32_t leader = LocalCampaignPlayer();
    if (!leader) leader = LocalSlotZeroEntity();
    return IsBotEntity(leader) ? 0 : leader;
}
bool Alive(uint32_t entity) {
    uint32_t vtable = 0;
    return entity && Read(entity, &vtable, 4) && vtable == base + placement::kPlayerPrimaryVtableRva;
}
uint32_t CameraManager() { uint32_t cm = 0; Read(base + kCameraManagerRva, &cm, 4); return cm; }
uint32_t PadSlot() {
    uint32_t dm = 0;
    return Read(base + kDeviceManagerRva, &dm, 4) && dm ? dm + kPadBase + u->controller * kPadSize : 0;
}
uint32_t ConfigCount(unsigned index) {
    uint32_t cm = 0, count = 0;
    if (!Read(base + kControllerManagerRva, &cm, 4) || !cm || !Read(cm + kCmiBase + index * kCmiSize, &count, 4)) return 0xFFFFFFFFu;
    return count;
}
// Whatever sits in PlayerManager slot id (its BasePlayer pointer), 0 when free.
uint32_t SlotOccupant(unsigned id) {
    uint32_t pm = 0, slots = 0, capacity = 0, bp = 0;
    if (!Read(base + kPlayerManagerRva, &pm, 4) || !pm || !Read(pm + 4, &slots, 4) || !slots || !Read(pm + 8, &capacity, 4) || id >= capacity) return 0;
    Read(slots + id * 4, &bp, 4);
    return bp;
}
// Four bodies at most (four viewports): every human in the session takes one
// bot's seat. Solo, or before a partner is ready, all configured bots play. In
// Versus only the humans' side counts (12.10.: a necromorph player cost a bot). In an
// invasion a necro partner takes no seat either; a dead human watching keeps his
// (17.10.: each necro spawn dropped bot 3 and the next vent visit brought it back).
volatile LONG seatsGiven = 0, slotBusy = 0;
unsigned BotSeats(DWORD now) {
    if (!network || !session) return kMaxBots;
    const int side = mpnative::HumanSideHumans();
    unsigned humans = 1;
    if (side >= 0) humans = static_cast<unsigned>(side);
    else
        for (unsigned i = 0; i < coop_peers::kMaxPeers; ++i)
            if (peerTable.peers[i].used && peerRuntime[i].readiness.Ready(now) && (!roles::IsNecro(i, now) || roles::Spectating(i, now))) ++humans;
    return humans >= 4 ? 0 : (4 - humans < kMaxBots ? 4 - humans : kMaxBots);
}
bool InSlot(uint32_t entity, unsigned id) {
    uint32_t pm = 0, slots = 0, capacity = 0, bp = 0;
    return Read(base + kPlayerManagerRva, &pm, 4) && pm && Read(pm + 4, &slots, 4) && slots && Read(pm + 8, &capacity, 4) && id < capacity &&
           Read(slots + id * 4, &bp, 4) && bp == entity + kBasePlayer;
}
float Health01(uint32_t entity) {
    const uint32_t component = hits::FindHealthComponent(entity);
    float current = 0.0f, max = 0.0f;
    if (!component || !Read(component + hits::kHealthCurrent, &current, 4) || !Read(component + hits::kHealthMax, &max, 4) || !(max > 0.0f)) return 1.0f;
    return current / max;
}

// Where the bot's camera stands: the shot goes where the camera's centre ray
// hits, so aiming and line of sight start there, not at the head. The blender
// of the bot's viewport keeps the camera position at +0x00 and its look point
// at +0x10 (measured 26.09.: 1.6 m up, about 1.4 m behind and to the side of
// the chase camera's player). The head stands in when that is not readable.
constexpr uint32_t kBlenderCameraPos = 0x00;
Vec3 CameraEyeOf(const Vec3& self, unsigned slot) {
    const Vec3 head = {self.x, self.y + kEye, self.z};
    const uint32_t cm = CameraManager();
    uint32_t blenders = 0, count = 0;
    if (!cm || !Read(cm + kCmBlenders, &blenders, 4) || !blenders || !Read(cm + kCmNumViewports, &count, 4) || count < 2 || count > 4) return head;
    for (uint32_t viewport = 0; viewport < count; ++viewport) {
        uint32_t player = 0;
        if (!Read(cm + kCmPlayerIds + viewport * 4, &player, 4) || player != slot) continue;
        float at[3]{};
        if (!Read(blenders + viewport * kBlenderSize + kBlenderCameraPos, at, sizeof(at))) return head;
        const Vec3 eye = {at[0], at[1], at[2]};
        if (!std::isfinite(at[0]) || !std::isfinite(at[1]) || !std::isfinite(at[2]) || coop_bot::Distance(eye, head) > 4.0f) return head;
        return eye;
    }
    return head;
}
Vec3 CameraEye(const Vec3& self) { return CameraEyeOf(self, u->slotId); }

// The force gun's cone. ForceGun::GetFiringMatrix (PC 0x00DBA000, thiscall(gun,
// matrix out), ret 4) starts it at CameraManager::GetActiveCamPos(0), the host's
// camera, moved along the owner's aim to the depth of the owner's body; for the
// bot that is the wrong camera (DS2_BOT_WEAPONS_BATCH_B). Afterwards the bot's
// gets its own: from its camera along the aim (row 2) to the depth of its chest.
constexpr uint32_t kFiringMatrixRva = 0x9BA000;
safetyhook::InlineHook firingMatrixHook;
volatile LONG firingFixes = 0;
uint32_t __fastcall OnFiringMatrix(void* gun, void*, float* out) {
    const uint32_t result = firingMatrixHook.thiscall<uint32_t>(gun, out);
    uint32_t owner = 0;
    if (!out || !Read(reinterpret_cast<uintptr_t>(gun) + 0x240, &owner, 4) || owner < 0x10010) return result;
    const Unit* shooter = UnitOf(owner - 0x10);
    Vec3 self;
    if (!shooter || !bot_sense::Pos(shooter->entity, self)) return result;
    const Vec3 eye = CameraEyeOf(self, shooter->slotId), fwd = {out[8], out[9], out[10]}, chest = {self.x, self.y + 1.1f, self.z};
    if (!std::isfinite(fwd.x) || std::fabs(coop_bot::Length(fwd) - 1.0f) > 0.1f) return result;
    const float depth = coop_bot::Dot(coop_bot::Sub(chest, eye), fwd);
    const Vec3 at = coop_bot::Add(eye, coop_bot::Scale(fwd, depth > 0.0f ? depth : 0.0f));
    out[12] = at.x;
    out[13] = at.y;
    out[14] = at.z;
    InterlockedIncrement(&firingFixes);
    return result;
}

// "Centered" aiming. With the option on (the byte at 0x01F5E088, the Xbox's
// Player::ShouldUseCenterPointAiming), Gun::GetMuzzleAim (PC 0x00DB07E0,
// thiscall(gun, matrix out), ret 4) and the stasis shot (PlayerFireStasisSM::
// ExecuteAttack, PC 0x008892D0, thiscall(sm), owner at sm+0x50) aim every
// Player's shot at the CenterPointAiming point ([0x020213AC]+0x10), which is
// where the human's crosshair lands: the bot's shots went there (27.09.: with
// Classic aiming it hit, with Centered never). For the bot the option is off
// while those two run.
constexpr uint32_t kCenterAimingRva = 0x1B5E088, kMuzzleAimRva = 0x9B07E0, kStasisAttackRva = 0x4892D0;
safetyhook::InlineHook muzzleAimHook, stasisAttackHook;
volatile LONG centeredBypasses = 0;
struct ClassicAim {
    unsigned char saved = 0;
    bool active = false;
    explicit ClassicAim(bool forBot) {
        if (!forBot) return;
        unsigned char* flag = reinterpret_cast<unsigned char*>(base + kCenterAimingRva);
        saved = *flag;
        if (!saved) return;
        *flag = 0;
        active = true;
        InterlockedIncrement(&centeredBypasses);
    }
    ~ClassicAim() {
        if (active) *reinterpret_cast<unsigned char*>(base + kCenterAimingRva) = saved;
    }
};
// Where a bot's shots really go: the muzzle matrix the gun fires along (row 2
// forward, row 3 the muzzle), checked against the point the bot aimed at and
// cast into the world to see what it meets. One sample per trigger press.
volatile LONG shotRays = 0, shotsOnTarget = 0, shotsWide = 0, shotMissCm = 0, shotParallaxCm = 0;
void MeasureShot(Unit& unit, const float* m) {
    const DWORD now = GetTickCount();
    if (!unit.entity || now - unit.fireAt > 150 || now - unit.shotMeasuredAt < 250) return;
    unit.shotMeasuredAt = now;
    const Vec3 fwd = {m[8], m[9], m[10]}, from = {m[12], m[13], m[14]};
    if (!std::isfinite(fwd.x) || std::fabs(coop_bot::Length(fwd) - 1.0f) > 0.05f || !std::isfinite(from.x)) return;
    const Vec3 aim = unit.command.lookAt;
    const Vec3 to = coop_bot::Sub(aim, from);
    const float along = coop_bot::Dot(to, fwd);
    if (along < 0.5f) return;
    const float miss = coop_bot::Length(coop_bot::Sub(to, coop_bot::Scale(fwd, along)));
    // The camera the bot turns: how far the muzzle sits off that line at the target.
    Vec3 self;
    float parallax = -1.0f;
    if (bot_sense::Pos(unit.entity, self)) {
        const Vec3 eye = CameraEyeOf(self, unit.slotId), eyeTo = coop_bot::Sub(aim, eye);
        const float eyeLen = coop_bot::Length(eyeTo);
        if (eyeLen > 0.5f) {
            const Vec3 eyeDir = coop_bot::Scale(eyeTo, 1.0f / eyeLen), rel = coop_bot::Sub(from, eye);
            parallax = coop_bot::Length(coop_bot::Sub(rel, coop_bot::Scale(eyeDir, coop_bot::Dot(rel, eyeDir))));
        }
    }
    alignas(16) float a[4] = {from.x, from.y, from.z, 1.0f}, b[4] = {from.x + fwd.x * 60.0f, from.y + fwd.y * 60.0f, from.z + fwd.z * 60.0f, 1.0f};
    float fraction = 1.0f;
    uint32_t hit = 0;
    bool cast = false;
    if (bot_sense::losReady) {   // never into CastingTest unverified
        cast = bot_sense::CastRaw(a, b, unit.entity, fraction, hit);
        if (!cast) bot_sense::NoteLosFault();
    }
    const bool onTarget = cast && hit && hit == unit.command.target;
    InterlockedIncrement(&shotRays);
    if (onTarget) InterlockedIncrement(&shotsOnTarget);
    if (miss > 0.25f) InterlockedIncrement(&shotsWide);
    InterlockedExchangeAdd(&shotMissCm, static_cast<LONG>(miss * 100.0f));
    if (parallax >= 0.0f) InterlockedExchangeAdd(&shotParallaxCm, static_cast<LONG>(parallax * 100.0f));
    if (shotRays <= 60)
        gate::Event("bot %u shot: %.2f m beside its aim point at %.1f m (muzzle %.2f m off the camera line), ray meets %08X at %.1f m, target %08X%s",
                    unit.index + 1, miss, along, parallax, hit, fraction * 60.0f, unit.command.target, onTarget ? " (hit)" : "");
}
// The shot leaves the muzzle along the aim, parallel to the camera's line and
// 0.2 to 0.55 m beside it (27.09.: 11 of 171 shots on target, 39 cm off on
// average, nearly all of it this offset). So the aim is taken from the muzzle.
volatile LONG muzzleNotes = 0;
void NoteMuzzle(Unit& unit, const float* m) {
    Vec3 self;
    const float fx = m[8], fz = m[10], flat = std::sqrt(fx * fx + fz * fz);
    if (!bot_sense::Pos(unit.entity, self) || flat < 0.2f || !std::isfinite(m[12]) || !std::isfinite(m[13]) || !std::isfinite(m[14])) return;
    const Vec3 off = {m[12] - self.x, m[13] - self.y, m[14] - self.z};
    if (coop_bot::Length(off) > 2.5f) return;
    const float sx = fx / flat, sz = fz / flat;   // forward (sin h, cos h); right (cos h, -sin h)
    unit.muzzleRight = off.x * sz - off.z * sx;
    unit.muzzleForward = off.x * sx + off.z * sz;
    unit.muzzleUp = off.y;
    unit.muzzleAt = GetTickCount();
    InterlockedIncrement(&muzzleNotes);
}
uint32_t __fastcall OnMuzzleAim(void* gun, void*, void* out) {
    uint32_t owner = 0;
    const bool bot = Read(reinterpret_cast<uintptr_t>(gun) + 0x240, &owner, 4) && owner > 0x10010 && IsBotEntity(owner - 0x10);
    uint32_t result = 0;
    {
        ClassicAim classic(bot);
        result = muzzleAimHook.thiscall<uint32_t>(gun, out);
    }
    if (bot && out) {
        Unit* unit = UnitOf(owner - 0x10);
        float m[16];
        if (unit && Read(reinterpret_cast<uintptr_t>(out), m, sizeof(m))) {
            MeasureShot(*unit, m);
            NoteMuzzle(*unit, m);
        }
    }
    return result;
}
uint32_t __fastcall OnStasisAttack(void* sm, void*) {
    uint32_t owner = 0;
    const bool bot = Read(reinterpret_cast<uintptr_t>(sm) + 0x50, &owner, 4) && IsBotEntity(owner);
    ClassicAim classic(bot);
    return stasisAttackHook.thiscall<uint32_t>(sm);
}

// Heading (atan2(x, z)) and pitch (positive looks down) of an aim data block:
// the source matrix forward row (+0x50, row 2 at +0x70) turned by its yaw.
bool AimAngles(uint32_t entity, uint32_t block, float& heading, float& pitch) {
    uint32_t adc = 0;
    float pyr[2]{}, forward[3]{};
    if (!Read(entity + kAimingData, &adc, 4) || !adc || !Read(adc + block, pyr, 8) || !Read(adc + block + 0x50 + 0x20, forward, 12)) return false;
    if (!std::isfinite(pyr[0]) || !std::isfinite(pyr[1]) || !std::isfinite(forward[0]) || !std::isfinite(forward[2])) return false;
    heading = std::atan2(forward[0], forward[2]) + pyr[1];
    pitch = pyr[0];
    return true;
}
float Wrap(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}
Vec3 Direction(float heading, float pitch) {
    return {std::sin(heading) * std::cos(pitch), -std::sin(pitch), std::cos(heading) * std::cos(pitch)};
}
float AngleDeg(const Vec3& a, const Vec3& b) {
    const float la = coop_bot::Length(a), lb = coop_bot::Length(b);
    if (la < 1e-4f || lb < 1e-4f) return 180.0f;
    float c = coop_bot::Dot(a, b) / (la * lb);
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    return std::acos(c) * 180.0f / kPi;
}

// Entity and character controller moved together (the necro vent rescue's way).
__declspec(noinline) bool MoveEntity(uint32_t entity, const Vec3& to, float lift = 0.2f) {
    alignas(16) float matrix[16];
    if (!Read(entity + necro::kEntityMatrix, matrix, sizeof(matrix))) return false;
    matrix[12] = to.x; matrix[13] = to.y + lift; matrix[14] = to.z;
    __try {
        uint32_t vtable = 0, setMatrix = 0;
        if (!Read(entity, &vtable, 4) || !Read(vtable + necro::kEntitySetMatrixSlot, &setMatrix, 4) || !setMatrix) return false;
        using MatrixCall = void (__thiscall*)(uint32_t, const float*);
        reinterpret_cast<MatrixCall>(setMatrix)(entity, matrix);
        if (const uint32_t cmc = necro::FindCmc(entity)) reinterpret_cast<MatrixCall>(base + necro::kCmcSetMatrixRva)(cmc, matrix);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The CharacterMovementComponent (vtable 0x01B63FBC) behind entity+0x17F4.
constexpr uint32_t kCmcVtableRva = 0x1763FBC, kCmcPointer = 0x17F4, kCmcFrame = 0xD0, kCmcFlags = 0x144;
uint32_t CmcOf(uint32_t entity) {
    uint32_t cmc = 0, vtable = 0;
    if (Read(entity + kCmcPointer, &cmc, 4) && cmc && Read(cmc, &vtable, 4) && vtable == base + kCmcVtableRva) return cmc;
    return 0;
}
// Its body frame (left +0xD0, up +0xE0, forward +0xF0, as the zero-G code reads
// them) at the entity's own position; flags +0x144, bit 0 = in gravity.
bool BodyOf(uint32_t entity, const Vec3& pos, coop_bot::BodyFrame& b, uint32_t& flags) {
    const uint32_t cmc = entity ? CmcOf(entity) : 0;
    float rows[12]{};
    if (!cmc || !Read(cmc + kCmcFrame, rows, sizeof(rows))) return false;
    for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10}) if (!std::isfinite(rows[i])) return false;
    b.left = {rows[0], rows[1], rows[2]};
    b.up = {rows[4], rows[5], rows[6]};
    b.forward = {rows[8], rows[9], rows[10]};
    if (std::fabs(coop_bot::Length(b.forward) - 1.0f) > 0.1f || std::fabs(coop_bot::Length(b.up) - 1.0f) > 0.1f) return false;
    b.pos = pos;
    return Read(cmc + kCmcFlags, &flags, 4);
}

// A teleport lands right by the leader (29.09.: the trail spot a few metres back
// was not where he stood): just behind him, bot 1 straight behind, 2 behind left,
// 3 behind right, 0.9 m out, closer when a wall is in the way, else on his spot.
// Out of the local camera's view (or no camera to ask).
bool OutOfView(const Vec3& spot) {
    coop_marks::Camera cam{};
    if (!overlay::CameraNow(cam, 0, 0)) return true;
    const float p[3] = {spot.x, spot.y + 1.0f, spot.z};
    float x = 0.0f, y = 0.0f, z = 0.0f;
    coop_marks::ToView(cam, p, x, y, z);
    return z <= 0.2f || std::fabs(x) > z * cam.tanX * 1.1f || std::fabs(y) > z * cam.tanY * 1.1f;
}
volatile LONG catchUpsHidden = 0, catchUpsSeen = 0, catchUpsOffMesh = 0;
// A catch-up spot near the leader, on the mesh and out of the camera's view when there is one:
// a spot beside him off the mesh got the bot stuck again at once, and it popped up in plain
// sight (20.10., ch09 mining arena: 8 catch-ups in 4 minutes, most beside him on a platform).
Vec3 SpotAtLeader(const Vec3& leader);
Vec3 CatchUpSpot(const Vec3& leader) {
    if (zeroGFlight && (leaderZeroG || leaderFlying)) return ZeroGSpot(leader);
    const uint32_t leaderEntity = Leader();
    const float yaw = leaderEntity ? bot_sense::Yaw(leaderEntity) : 0.0f;
    const Vec3 chest = {leader.x, leader.y + 1.0f, leader.z};
    static const float kAngles[] = {180.0f, 150.0f, -150.0f, 120.0f, -120.0f, 90.0f, -90.0f};
    static const float kRadii[] = {3.0f, 2.0f, 1.2f};
    bool haveSeen = false;
    Vec3 seen{};
    for (unsigned k = 0; k < std::size(kAngles); ++k) {
        const float a = yaw + kAngles[(k + u->index) % std::size(kAngles)] * 0.01745329f;
        for (const float r : kRadii) {
            Vec3 spot = {leader.x + std::sin(a) * r, leader.y, leader.z + std::cos(a) * r};
            Vec3 snapped{};
            if (!bot_nav::Snap(spot, snapped) || coop_bot::Flat(snapped, spot) > 1.0f) continue;
            spot = snapped;
            if (bot_sense::losReady && !bot_sense::Clear(chest, {spot.x, spot.y + 1.0f, spot.z}, leaderEntity, 0)) continue;
            if (OutOfView(spot)) { InterlockedIncrement(&catchUpsHidden); return spot; }
            if (!haveSeen) { haveSeen = true; seen = spot; }
        }
    }
    if (haveSeen) { InterlockedIncrement(&catchUpsSeen); return seen; }
    InterlockedIncrement(&catchUpsOffMesh);
    return SpotAtLeader(leader);
}
Vec3 SpotAtLeader(const Vec3& leader) {
    if (zeroGFlight && (leaderZeroG || leaderFlying)) return ZeroGSpot(leader);   // he may stand on a wall or fly
    const uint32_t leaderEntity = Leader();
    const float yaw = leaderEntity ? bot_sense::Yaw(leaderEntity) : 0.0f;
    static const float kAngles[] = {180.0f, 135.0f, -135.0f};
    const float a = yaw + kAngles[u->index % 3] * 0.01745329f;
    const Vec3 chest = {leader.x, leader.y + 1.0f, leader.z};
    static const float kRadii[] = {0.9f, 0.5f};
    for (const float r : kRadii) {
        const Vec3 spot = {leader.x + std::sin(a) * r, leader.y, leader.z + std::cos(a) * r};
        if (!bot_sense::losReady || bot_sense::Clear(chest, {spot.x, spot.y + 1.0f, spot.z}, leaderEntity, 0)) return spot;
    }
    return leader;
}
// Where to put the bot near the leader: a spot the leader stood on 2-5 m back, else his own.
// Each bot its own stretch of the trail, so a call does not stack them.
Vec3 SpotNearLeader(const Vec3& leader) {
    const float from = 2.0f + 1.5f * static_cast<float>(u->index), to = from + 3.0f;
    Vec3 best = leader;
    for (int i = trail.count - 1; i >= 0; --i) {
        const Vec3 p = trail.Get(i);
        const float d = coop_bot::Distance(p, leader);
        if (d >= from && d <= to && std::fabs(p.y - leader.y) < 1.0f) return p;
    }
    return best;
}

// Camera fades. Every Player
// has a CameraInteractionComponent that fades its body when a camera comes too
// close; NotifyInteraction (PC 0x00F5AAC0, thiscall(component), ret) starts it
// and the alpha lives on the entity (+0x158), one for every viewport. The callers
// are the cameras themselves: a bot's own chase camera pushed against a wall
// faded the bot on the human's screen (28.09.), and a bot's camera sphere
// would fade the human or props near it too. CameraManager::Update calls each
// viewport's CameraBlender::Update (0x0084D640, thiscall(blender), ret) at
// 0x00793735; that call is rerouted here to tell whose camera is working.
// Notifications while a bot's viewport (1..3) updates are dropped, and outside
// any camera update (the store and zero-G states) a bot's own component is left alone.
constexpr uint32_t kBlenderCallRva = 0x393735, kBlenderUpdateRva = 0x44D640, kCamNotifyRva = 0xB5AAC0, kCamInteractionOwner = 0x18;
bool camFadeFix = false;
int camViewport = -1;
safetyhook::InlineHook camNotifyHook;
volatile LONG camFadeDropped = 0, camFadePassed = 0, camFadeBotSelf = 0;
void __fastcall OnBlenderUpdate(void* blender, void*) {
    const uint32_t cm = CameraManager();
    uint32_t blenders = 0;
    const int saved = camViewport;
    camViewport = cm && Read(cm + kCmBlenders, &blenders, 4) && blenders && reinterpret_cast<uint32_t>(blender) >= blenders
                      ? static_cast<int>((reinterpret_cast<uint32_t>(blender) - blenders) / kBlenderSize)
                      : -1;
    reinterpret_cast<void(__thiscall*)(void*)>(base + kBlenderUpdateRva)(blender);
    camViewport = saved;
}
void __fastcall OnCamNotify(void* component, void*) {
    if (camViewport >= 1) { InterlockedIncrement(&camFadeDropped); return; }
    if (camViewport < 0) {
        uint32_t owner = 0;
        if (Read(reinterpret_cast<uintptr_t>(component) + kCamInteractionOwner, &owner, 4) && IsBotEntity(owner)) { InterlockedIncrement(&camFadeBotSelf); return; }
    }
    InterlockedIncrement(&camFadePassed);
    camNotifyHook.thiscall<void>(component);
}
bool InstallCamFade(FILE* log) {
    if (!camFadeFix) { std::fprintf(log, "bot camera_fade_fix=0\n"); return false; }
    // mov ecx,[esi+10h] / add ecx,ebx / call CameraBlender::Update / mov eax,[esi+164h]
    unsigned char want[16] = {0x8B, 0x4E, 0x10, 0x03, 0xCB, 0xE8, 0, 0, 0, 0, 0x8B, 0x86, 0x64, 0x01, 0x00, 0x00};
    const int32_t rel = static_cast<int32_t>(kBlenderUpdateRva - (kBlenderCallRva + 5));
    std::memcpy(want + 6, &rel, 4);
    unsigned char have[16]{};
    const uintptr_t site = base + kBlenderCallRva - 5;
    const bool pristine = Read(site, have, sizeof(have)) && std::memcmp(have, want, sizeof(want)) == 0;
    bool routed = false;
    if (pristine) {
        DWORD previous = 0;
        unsigned char* call = reinterpret_cast<unsigned char*>(base + kBlenderCallRva);
        if (VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &previous)) {
            const int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&OnBlenderUpdate) - (base + kBlenderCallRva + 5));
            std::memcpy(call + 1, &to, 4);
            VirtualProtect(call, 5, previous, &previous);
            FlushInstructionCache(GetCurrentProcess(), call, 5);
            routed = true;
        }
    }
    // push esi / mov esi,ecx / mov eax,[esi+1Ch] / test eax,eax / jz
    static const unsigned char kNotify[] = {0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x1C, 0x85, 0xC0, 0x74, 0x1C};
    const bool notify = routed && hook_site::Install(log, "bot CameraInteractionComponent::NotifyInteraction", base + kCamNotifyRva, kNotify, sizeof(kNotify),
                                                     reinterpret_cast<void*>(OnCamNotify), camNotifyHook);
    std::fprintf(log, "bot camera_fade_fix=%d (blender_call=%s notify_hook=%d: a bot's camera fades nobody, its own body is not faded by it)\n",
                 routed && notify ? 1 : 0, routed ? "routed" : (pristine ? "protect failed" : "bytes differ"), notify ? 1 : 0);
    return routed && notify;
}

// Pickup prompts. Every
// Player runs its own PlayerItemPickupSM (owner at +0x50), and each one spawns
// its own "Pick Up" hologram and name plate on the item it targets, turned to
// its own Player: near a pickup the human saw one per bot, some mirrored. The
// three VFX entries return at once for a bot's machine; its targeting is left
// as it is. PlayLargeVFX 0x0088BB90 (thiscall(sm, x), ret 4), PlayNewSmallVFX
// 0x0088BEC0 and PlayCollapseVFX 0x00873E00 (thiscall(sm), ret).
constexpr uint32_t kPlayLargeVfxRva = 0x48BB90, kPlaySmallVfxRva = 0x48BEC0, kPlayCollapseVfxRva = 0x473E00, kPickupSmOwner = 0x50;
safetyhook::InlineHook largeVfxHook, smallVfxHook, collapseVfxHook;
volatile LONG promptsSkipped = 0;
bool BotsPickupMachine(void* sm) {
    uint32_t owner = 0;
    return Read(reinterpret_cast<uintptr_t>(sm) + kPickupSmOwner, &owner, 4) && IsBotEntity(owner);
}
uint32_t __fastcall OnPlayLargeVfx(void* sm, void*, uint32_t x) {
    if (BotsPickupMachine(sm)) { InterlockedIncrement(&promptsSkipped); return 0; }
    return largeVfxHook.thiscall<uint32_t>(sm, x);
}
uint32_t __fastcall OnPlaySmallVfx(void* sm, void*) {
    if (BotsPickupMachine(sm)) { InterlockedIncrement(&promptsSkipped); return 0; }
    return smallVfxHook.thiscall<uint32_t>(sm);
}
uint32_t __fastcall OnPlayCollapseVfx(void* sm, void*) {
    if (BotsPickupMachine(sm)) return 0;
    return collapseVfxHook.thiscall<uint32_t>(sm);
}
bool InstallPrompts(FILE* log) {
    // sub esp,10h / push ebx / push esi / mov esi,ecx / cmp dword [esi+0D8h],0 / push edi / lea
    static const unsigned char kLarge[] = {0x83, 0xEC, 0x10, 0x53, 0x56, 0x8B, 0xF1, 0x83, 0xBE, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x57, 0x8D};
    // push ebp / mov ebp,esp / and esp,-16 / sub esp,134h
    static const unsigned char kSmall[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x34, 0x01, 0x00, 0x00};
    // sub esp,10h / push esi / mov esi,ecx / cmp dword [esi+0D8h],0 / push edi / lea edi,[esi+...]
    static const unsigned char kCollapse[] = {0x83, 0xEC, 0x10, 0x56, 0x8B, 0xF1, 0x83, 0xBE, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x57, 0x8D, 0xBE};
    const bool large = hook_site::Install(log, "bot PlayerItemPickupSM::PlayLargeVFX", base + kPlayLargeVfxRva, kLarge, sizeof(kLarge),
                                          reinterpret_cast<void*>(OnPlayLargeVfx), largeVfxHook);
    const bool smallOk = hook_site::Install(log, "bot PlayerItemPickupSM::PlayNewSmallVFX", base + kPlaySmallVfxRva, kSmall, sizeof(kSmall),
                                          reinterpret_cast<void*>(OnPlaySmallVfx), smallVfxHook);
    const bool collapse = hook_site::Install(log, "bot PlayerItemPickupSM::PlayCollapseVFX", base + kPlayCollapseVfxRva, kCollapse, sizeof(kCollapse),
                                             reinterpret_cast<void*>(OnPlayCollapseVfx), collapseVfxHook);
    std::fprintf(log, "bot pickup_prompts_hidden=%d (large=%d small=%d collapse=%d: only the player's pickup holograms show)\n",
                 large && smallOk && collapse ? 1 : 0, large ? 1 : 0, smallOk ? 1 : 0, collapse ? 1 : 0);
    return large && smallOk && collapse;
}

// --- hooks --------------------------------------------------------------------

// CameraManager::Update calls this for every viewport below the count; viewport 1
// is the bot's and has no render context.
void __fastcall OnSetRenderCamera(void* cm, void*, uint32_t viewport, void* pos, void* rot, float fov, float nearPlane, float farPlane) {
    uint32_t context = 0;
    if (viewport >= 1 && viewport < 4 && Read(reinterpret_cast<uintptr_t>(cm) + kCmRenderCtx + viewport * 4, &context, 4) && !context) return;
    renderHook.thiscall<void>(cm, viewport, pos, rot, fov, nearPlane, farPlane);
}
volatile LONG upgradeSkips = 0;
void __fastcall OnUpgradeClear(void* self, void*) {
    if (botInit) { InterlockedIncrement(&upgradeSkips); return; }
    clearHook.thiscall<void>(self);
}
void __fastcall OnUpgradeRestore(void* self, void*) {
    if (botInit) { InterlockedIncrement(&upgradeSkips); return; }
    restoreHook.thiscall<void>(self);
}

// A host bot's ghost on a partner's machine (machine byte 0xF0 + bot). The host owns
// its life: there the bot is floored and its death swallowed, so the ghost must not die
// here either (17.10.: a necro's spit took a bot ghost's 25 health to 0 on the client and
// the ghost vanished there, while the host's bot was put back to 25 and lived on).
bool IsBotGhost(uint32_t entity) {
    const uint32_t machine = MachineOf(entity);
    return machine >= 0xF0u && machine < 0xF0u + kMaxBots && IsGhostPlayer(entity);
}
volatile LONG ghostDeathMessages = 0, ghostFloorCatches = 0, ghostFloorLines = 0;
bool MortalNow();      // below
bool VersusMortal();   // below

// The last net: should the bot's health reach zero anyway, its PlayerDeathSM
// (HandleStateMessage 0x008A12D0, thiscall, ret 0x14, message id the fourth
// stack argument, owner Player at +0x50) would announce a player death to the
// whole level (0x18), push the human's death screen (0x1E), drop its items
// (0x20) and restart the level (0x1D, ProcessDeath). Those four are swallowed
// for the bot.
volatile LONG deathMessages = 0;
unsigned char __fastcall OnDeathSm(void* sm, void*, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t message, uint32_t a4) {
    uint32_t owner = 0;
    const bool ownerRead = Read(reinterpret_cast<uintptr_t>(sm) + 0x50, &owner, 4);
    if (ownerRead && respawn::SwallowDeathMessage(owner, message)) return 1;
    if (ownerRead && spectate::SwallowDeathMessage(owner, message)) return 1;
    if (ownerRead && IsBotEntity(owner) &&
        (message == 0x18 || message == 0x1D || message == 0x1E || message == 0x20)) {
        botDiedAt = GetTickCount();
        if (Unit* dead = UnitOf(owner)) dead->died = true;
        if (InterlockedIncrement(&deathMessages) <= 20) {
            float pos[3]{};
            Read(owner + 0xC0, pos, 12);
            gate::Event("bot: death state message %02X of the bot %08X swallowed (health %.0f%%, at %.1f %.1f %.1f)", message, owner,
                        Health01(owner) * 100.0f, pos[0], pos[1], pos[2]);
        }
        return 1;
    }
    // A host bot's ghost, mortal or not: outside Versus these four would run the campaign
    // death path (death screen, restart) on this machine. The body still dies and lies.
    if (ownerRead && (message == 0x18 || message == 0x1D || message == 0x1E || message == 0x20) && IsBotGhost(owner) && !VersusMortal()) {
        const LONG n = InterlockedIncrement(&ghostDeathMessages);
        if (n == 1)
            gate::Event("test bot_ghost INFO: death state message %02X of the bot ghost %08X (machine %02X) swallowed, health %.0f%%", message, owner,
                        MachineOf(owner), Health01(owner) * 100.0f);
        else if (n <= 20)
            gate::Event("bot ghost: death state message %02X of the bot ghost %08X (machine %02X) swallowed, health %.0f%%", message, owner,
                        MachineOf(owner), Health01(owner) * 100.0f);
        return 1;
    }
    return deathSmHook.thiscall<unsigned char>(sm, a0, a1, a2, message, a4);
}
// The PC keyboard hands its actions (stomp on space, ...) to the ControllerManager
// as up to four event ids at +0x18BC; every controller's event pass
// (0x0077E2F0, thiscall(manager, controller), ret 4) counts them as its own and clears
// them afterwards. Controllers run 0..15, so pad 14 took the human's keys before
// Isaac's pad 15 saw them (26.09.: space made the bot stomp, never Isaac). The
// bot's pass runs without them and hands them on untouched.
constexpr uint32_t kEvaluateRva = 0x37E2F0, kInjectedEvents = 0x18BC;
volatile LONG keysKept = 0, keysKeptBodiless = 0, keysTakenByOther = 0, keysTakenLines = 0;
// A bot's pad stays plugged in while the bot has no body (dead, given up, between bodies), and
// its pass took the human's keys then: space and kinesis did nothing now and then for months
// (02.11.). So every bot pad is skipped, body or not. Keys any other controller than Isaac's
// takes are logged: that is the same fault from somewhere else.
uint32_t __fastcall OnEvaluate(void* manager, void*, uint32_t index) {
    bool botPad = false, bodiless = false;
    const uint32_t human = [] { uint32_t c = 0xFF; const uint32_t p = LocalCampaignPlayer(); if (p) Read(p + kPlayerController, &c, 1); return c & 0xFF; }();
    for (const Unit& unit : units)
        if ((index & 0xFF) == unit.controller && unit.controller != human) { botPad = true; bodiless = !unit.entity; }
    const uintptr_t injected = reinterpret_cast<uintptr_t>(manager) + kInjectedEvents;
    if (!botPad) {
        uint32_t before[4]{};
        std::memcpy(before, reinterpret_cast<const void*>(injected), sizeof(before));
        // A follower plays a scene with the leader's controller: its own keyboard actions do not
        // count (02.11.: the client mashed through the host's failed prompt and lived).
        if ((before[0] | before[1] | before[2] | before[3]) && (index & 0xFF) == human && cutscene::leadinput::injecting) {
            std::memset(reinterpret_cast<void*>(injected), 0, sizeof(before));
            InterlockedIncrement(&cutscene::leadinput::ownKeysDropped);
            return evaluateHook.thiscall<uint32_t>(manager, index);
        }
        const uint32_t result = evaluateHook.thiscall<uint32_t>(manager, index);
        if ((before[0] | before[1] | before[2] | before[3]) && human != 0xFF && (index & 0xFF) != human) {
            uint32_t after[4]{};
            std::memcpy(after, reinterpret_cast<const void*>(injected), sizeof(after));
            if (!(after[0] | after[1] | after[2] | after[3])) {
                InterlockedIncrement(&keysTakenByOther);
                if (InterlockedIncrement(&keysTakenLines) <= 40)
                    gate::Event("input: keyboard events %08X %08X %08X %08X taken by controller %u, not Isaac's %u (pad 15 configs %u)", before[0], before[1], before[2],
                                before[3], index & 0xFF, human, ConfigCount(15));
            }
        }
        return result;
    }
    if (bodiless) InterlockedIncrement(&keysKeptBodiless);
    uint32_t keys[4]{};
    std::memcpy(keys, reinterpret_cast<const void*>(injected), sizeof(keys));
    std::memset(reinterpret_cast<void*>(injected), 0, sizeof(keys));
    const uint32_t result = evaluateHook.thiscall<uint32_t>(manager, index);
    std::memcpy(reinterpret_cast<void*>(injected), keys, sizeof(keys));
    if (keys[0] | keys[1] | keys[2] | keys[3]) InterlockedIncrement(&keysKept);
    return result;
}
// Each Isaac's own walk direction. PlayerMovement::Update (0x0086D1D0,
// thiscall(pm, Player*), ret 4) turns the given Player's left stick into the
// direction at pm+8 (Player+0x6B0) that the aim state's strafe reads. The aim
// state's UpdateMovement (0x00882BD0, update message 0x1D, run right before
// ProcessGroundMovement 0x1C), PlayerMovement::Enable (0x0086D640) and two
// state constructors pass player 0 instead of the owner, so an aiming bot
// strafed with the human's keys (05.10., drift in lower body 17). A bot's
// PlayerMovement (owner+0x6A8) gets its own Player. Enable(false) wrote the
// bot's strafe values into player 0's animation variables; for a bot it only
// resets the fields (its own variables already hold those values).
constexpr uint32_t kMoveUpdateRva = 0x46D1D0, kMoveEnableRva = 0x46D640, kPlayerMovement = 0x6A8, kMoveDirection = 0x6B0;
bool ownMovement = false, ownMovementEnable = false;
safetyhook::InlineHook moveUpdateHook, moveEnableHook;
// Swaps by caller: aim UpdateMovement, Enable, constructor at 0x00881330, constructor at 0x00881C30, other.
volatile LONG moveSwaps[5]{}, moveOwnCalls = 0, moveEnableBot[2]{};
uint32_t moveOtherCaller = 0;
void __fastcall OnMoveUpdate(void* pm, void*, uint32_t player) {
    const uint32_t owner = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pm)) - kPlayerMovement;
    if (ownMovement && player != owner && IsBotEntity(owner)) {
        const uint32_t caller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())) - static_cast<uint32_t>(base);
        const unsigned slot = caller == 0x482C34 ? 0 : caller == 0x46D746 ? 1 : caller == 0x4815C0 ? 2 : caller == 0x481CC7 ? 3 : 4;
        InterlockedIncrement(&moveSwaps[slot]);
        if (slot == 4) moveOtherCaller = caller;
        player = owner;
    } else if (player == owner && IsBotEntity(owner)) {
        InterlockedIncrement(&moveOwnCalls);
    }
    moveUpdateHook.thiscall<void>(pm, player);
}
void __fastcall OnMoveEnable(void* pmp, void*, uint32_t enable) {
    const uint32_t pm = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pmp)), owner = pm - kPlayerMovement;
    if (!ownMovement || !ownMovementEnable || !IsBotEntity(owner)) { moveEnableHook.thiscall<void>(pmp, enable); return; }
    // The retail body with the owner in place of player 0.
    const unsigned char on = (enable & 0xFF) ? 1 : 0;
    InterlockedIncrement(&moveEnableBot[on]);
    WriteBytes(pm + 0x1C, &on, 1);
    if (on) {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kMoveUpdateRva)(pmp, owner);
        const int32_t full = 2;
        WriteBytes(pm + 0x18, &full, 4);
    } else {
        int32_t direction = 0;
        const int32_t zero = 0;
        Read(pm + 0x8, &direction, 4);
        WriteBytes(pm + 0xC, &direction, 4);
        WriteBytes(pm + 0x8, &zero, 4);
        WriteBytes(pm + 0x10, &zero, 4);
    }
}
bool InstallOwnMovement(FILE* log, bool wanted) {
    if (!wanted) { std::fprintf(log, "bot own_movement=0 (OwnMovementInput=0: an aiming bot strafes with the human's keys)\n"); return false; }
    // push ebp / mov ebp,esp / and esp,-16 / sub esp,124h / push ebx / push esi / push edi / mov edi,[ebp+8]
    static const unsigned char kUpdate[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x24, 0x01, 0x00, 0x00, 0x53, 0x56, 0x57, 0x8B, 0x7D, 0x08};
    ownMovement = hook_site::Install(log, "bot PlayerMovement::Update", base + kMoveUpdateRva, kUpdate, sizeof(kUpdate),
                                     reinterpret_cast<void*>(OnMoveUpdate), moveUpdateHook);
    // mov eax,[PlayerManager] / push esi / xor esi,esi / push edi / mov edi,ecx / test eax,eax / je +7
    const uint32_t pmGlobal = static_cast<uint32_t>(base) + kPlayerManagerRva;
    const unsigned char kEnable[] = {0xA1, static_cast<unsigned char>(pmGlobal), static_cast<unsigned char>(pmGlobal >> 8), static_cast<unsigned char>(pmGlobal >> 16),
                                     static_cast<unsigned char>(pmGlobal >> 24), 0x56, 0x33, 0xF6, 0x57, 0x8B, 0xF9, 0x85, 0xC0, 0x74, 0x07};
    ownMovementEnable = ownMovement && hook_site::Install(log, "bot PlayerMovement::Enable", base + kMoveEnableRva, kEnable, sizeof(kEnable),
                                                          reinterpret_cast<void*>(OnMoveEnable), moveEnableHook);
    std::fprintf(log, "bot own_movement=%d enable=%d (an aiming bot walks by its own stick, not the human's keys)\n", ownMovement ? 1 : 0,
                 ownMovementEnable ? 1 : 0);
    return ownMovement;
}
void NoteDeathRefused() {
    if (InterlockedIncrement(&deathsRefused) <= 20) gate::Event("bot: a death of the bot's Isaac refused (it would restart the level)");
}
// The bot behind a hit, or 0: DamageData+0x80 is the attacker (the Isaac for a
// stomp or swing, maybe his gun for a shot: then the gun's owner at +0x240).
// Both the object and its reference (object + 0x10) are taken.
uint32_t BotBehind(const void* damage) {
    uint32_t attacker = 0;
    if (!Read(reinterpret_cast<uintptr_t>(damage) + 0x80, &attacker, 4) || attacker < 0x10010) return 0;
    for (const uint32_t who : {attacker, attacker - 0x10}) {
        if (IsBotEntity(who)) return who;
        uint32_t ownerRef = 0;
        if (Read(who + 0x240, &ownerRef, 4) && ownerRef > 0x10010 && IsBotEntity(ownerRef - 0x10)) return ownerRef - 0x10;
    }
    return 0;
}
// The human Isaac behind a hit, or 0 (the same attacker reading; a NecroPlayer has its own vtable).
uint32_t HumanBehind(const void* damage) {
    uint32_t attacker = 0;
    if (!Read(reinterpret_cast<uintptr_t>(damage) + 0x80, &attacker, 4) || attacker < 0x10010) return 0;
    for (const uint32_t who : {attacker, attacker - 0x10}) {
        uint32_t vtable = 0, ownerRef = 0;
        if (!IsBotEntity(who) && Read(who, &vtable, 4) && vtable == base + placement::kPlayerPrimaryVtableRva) return who;
        if (Read(who + 0x240, &ownerRef, 4) && ownerRef > 0x10010 && !IsBotEntity(ownerRef - 0x10) && Read(ownerRef - 0x10, &vtable, 4) &&
            vtable == base + placement::kPlayerPrimaryVtableRva)
            return ownerRef - 0x10;
    }
    return 0;
}

// --- no harm to humans ([Bot] NoDamageToHumans, user 18.10.) ---------------------------------
// Nothing a bot does may hurt a human's Isaac: no shot, swing, stomp, blast, mine or kinesis
// throw, on the host (the bots) and on a partner's machine (their ghosts, machine 0xF0 + bot).
// Refused where the player's damage gate decides (Player::ShouldProcessDamage, on every
// machine) and again where health is taken (HealthComponent::HandleDamage, with the bots);
// a bot's stasis on a human is dropped as well. Necro players are no human Isaacs.
bool noDamageToHumans = true;
void SetNoDamageToHumans(bool on) { noDamageToHumans = on; }
constexpr DWORD kBotThrownMs = 6000, kBotActionMs = 2500, kBotStasisFlightMs = 2500;
struct BotThrownObject { uint32_t entity = 0; DWORD until = 0; };
BotThrownObject botThrown[8]{};
unsigned botThrownNext = 0;
DWORD lastBotActionAt = 0, lastBotStasisAt = 0;
volatile LONG humanHitsRefused = 0, humanHitLines = 0, humanHitsUnattributed = 0, humanUnattributedLines = 0, humanStasisRefused = 0, humanStasisLines = 0;
// Held or thrown with kinesis: the impact names the object as the attacker, not the bot.
void NoteBotThrown(uint32_t object, DWORD now) {
    if (!object) return;
    for (BotThrownObject& t : botThrown)
        if (t.entity == object) { t.until = now + kBotThrownMs; return; }
    botThrown[botThrownNext++ % std::size(botThrown)] = BotThrownObject{object, now + kBotThrownMs};
}
bool BotThrown(uint32_t entity, DWORD now) {
    for (const BotThrownObject& t : botThrown)
        if (t.entity && t.entity == entity && static_cast<LONG>(t.until - now) > 0) return true;
    return false;
}
// A human's Isaac: the local one or a partner's ghost, never a bot nor a bot's ghost.
bool HumanIsaac(uint32_t entity) {
    uint32_t vtable = 0;
    return entity && !IsBotEntity(entity) && !IsBotGhost(entity) && Read(entity, &vtable, 4) && vtable == base + placement::kPlayerPrimaryVtableRva;
}
// The bot behind a hit on any machine, or 0: the bot or its gun (BotBehind), a bot's ghost or
// its gun on a partner's machine, or an object a bot held or threw.
uint32_t BotSource(const void* damage) {
    if (const uint32_t bot = BotBehind(damage)) return bot;
    uint32_t attacker = 0;
    if (!Read(reinterpret_cast<uintptr_t>(damage) + 0x80, &attacker, 4) || attacker < 0x10010) return 0;
    const DWORD now = GetTickCount();
    for (const uint32_t who : {attacker, attacker - 0x10}) {
        if (IsBotGhost(who) || BotThrown(who, now)) return who;
        uint32_t ownerRef = 0;
        if (Read(who + 0x240, &ownerRef, 4) && ownerRef > 0x10010 && IsBotGhost(ownerRef - 0x10)) return ownerRef - 0x10;
    }
    return 0;
}
// A hit on a human shortly after a bot acted that names no bot: logged with what its attacker
// is, so a path the guard misses (a mine, a blade, a blast) shows in the next test.
void NoteUnattributed(uint32_t target, const void* damage, const char* where) {
    const DWORD now = GetTickCount();
    if (!lastBotActionAt || now - lastBotActionAt > kBotActionMs) return;
    uint32_t attacker = 0, vtable = 0, ownerRef = 0;
    Read(reinterpret_cast<uintptr_t>(damage) + 0x80, &attacker, 4);
    const uint32_t entity = attacker > 0x10010 ? attacker - 0x10 : 0;
    if (entity == target || (entity && npcs::IsNpcReflectable(entity + hits::kEntityReflectable))) return;   // its own or an enemy's
    InterlockedIncrement(&humanHitsUnattributed);
    if (InterlockedIncrement(&humanUnattributedLines) > 30) return;
    float amount = 0.0f;
    int32_t type = 0;
    Read(reinterpret_cast<uintptr_t>(damage) + 0x50, &amount, 4);
    Read(reinterpret_cast<uintptr_t>(damage) + 0x54, &type, 4);
    if (entity) { Read(entity, &vtable, 4); Read(entity + 0x240, &ownerRef, 4); }
    gate::Event("bot: a hit of %.1f (type %d) on the human %08X at %s, %lu ms after a bot acted, names no bot: attacker ref %08X (vtable RVA %08X, +0x240 %08X); it went through",
                amount, type, target, where, static_cast<unsigned long>(now - lastBotActionAt), attacker,
                vtable ? vtable - static_cast<uint32_t>(base) : 0u, ownerRef);
}
bool RefuseHitOnHuman(uint32_t target, const void* damage, const char* where) {
    if (!noDamageToHumans || !damage || !HumanIsaac(target)) return false;
    const uint32_t bot = BotSource(damage);
    if (!bot) { NoteUnattributed(target, damage, where); return false; }
    InterlockedIncrement(&humanHitsRefused);
    if (InterlockedIncrement(&humanHitLines) <= 30) {
        float amount = 0.0f;
        int32_t type = 0;
        Read(reinterpret_cast<uintptr_t>(damage) + 0x50, &amount, 4);
        Read(reinterpret_cast<uintptr_t>(damage) + 0x54, &type, 4);
        gate::Event("bot: a hit of %.1f (type %d) on the human %08X by %08X%s refused at %s (bots never hurt a human)", amount, type, target, bot,
                    IsBotGhost(bot) ? " (a bot's ghost)" : "", where);
    }
    return true;
}

bool noBotFriendlyFire = true;
volatile LONG botFriendlyBlocked = 0, botFriendlyLines = 0, humanFriendlyBlocked = 0;
// Versus ([Bot] VersusDeaths): the bots can die there and come back VersusRespawnMs
// after the death, next to the leader. Outside Versus they die only while a necro
// player is in the game (user, 17.10., no INI switch); otherwise never.
bool versusDeaths = true;
DWORD versusRespawnMs = 5000;
volatile LONG versusBotDeaths = 0, versusBotRespawns = 0, versusSpawnPoints = 0;
// the picked mode outlives the visit: only on the MP map itself
bool VersusMortal() { return versusDeaths && mpnative::InVersus() && mpmap::InVisit(); }
// A necro player here or at a partner: roles 1..3 (role 4 is a dead human watching).
// Host and clients see the same set: their own role and the partners' NROL packets.
bool NecroInGame() {
    const int own = necro::LocalRole();
    if (own >= 1 && own <= 3) return true;
    const DWORD now = GetTickCount();
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const int role = roles::RoleOf(s, now);
        if (role >= 1 && role <= 3) return true;
    }
    return false;
}
// A Versus round on its map keeps its own rule (VersusDeaths); InVersus alone outlives the visit.
bool MortalNow() { return VersusMortal() || (!(mpnative::InVersus() && mpmap::InVisit()) && NecroInGame()); }
const char* MortalWhy() { return VersusMortal() ? "in Versus" : "with a necro player in the game"; }
int mortalState = -1;   // the tick's last answer, for the diagnostics thread
volatile LONG mortalChanges = 0, zeroHealthBodies = 0;
void NoteMortal() {
    const int mortal = MortalNow() ? 1 : 0;
    if (mortal == mortalState) return;
    mortalState = mortal;
    if (InterlockedIncrement(&mortalChanges) <= 40)
        gate::Event("bot: mortal=%d (versus=%d necro_in_game=%d local_role=%d): the bots %s", mortal, VersusMortal() ? 1 : 0, NecroInGame() ? 1 : 0,
                    necro::LocalRole(), mortal ? "can die now" : "never die now");
}
// On the survival maps (an MP map visit, a campaign arena) a bot is only put by the
// leader when it is stuck, never for being far off ([Bot] SurvivalDistanceTeleport).
bool survivalDistanceTeleport = false;
volatile LONG distanceTeleportsSkipped = 0;
bool OnlyStuckTeleports() { return !survivalDistanceTeleport && (mpmap::InVisit() || arena::RunningNow()); }
// The bot takes damage down to a floor: healing is its business, dying is not allowed.
// A mate's shots and stomps do not hurt it at all ([Bot] NoBotFriendlyFire; a bot's
// hits on a human are refused before this, RefuseHitOnHuman).
volatile LONG desperateSelfSpared = 0;
bool TakesDamage(uint32_t entity, const void* damage) {
    float amount = 0.0f;
    Read(reinterpret_cast<uintptr_t>(damage) + 0x50, &amount, 4);
    if (!std::isfinite(amount) || amount < 0.0f) amount = 0.0f;
    // A bot's own desperate blast at its feet does not hurt it (the mates are spared above).
    if (const uint32_t shooter = BotBehind(damage); shooter && shooter == entity) {
        for (const Unit& one : units)
            if (one.entity == entity && one.desperateAt && GetTickCount() - one.desperateAt < 4000) {
                InterlockedIncrement(&desperateSelfSpared);
                return false;
            }
    }
    if (noBotFriendlyFire) {
        if (const uint32_t mate = BotBehind(damage); mate && mate != entity) {
            InterlockedIncrement(&botFriendlyBlocked);
            if (InterlockedIncrement(&botFriendlyLines) <= 30)
                gate::Event("bot: a hit of %.1f on the bot %08X by its mate %08X refused (no friendly fire among bots)", amount, entity, mate);
            return false;
        }
    }
    if (MortalNow()) {
        // the bots are on the human side: the humans' hits do nothing to them, as in an MP round
        if (const uint32_t human = HumanBehind(damage)) {
            InterlockedIncrement(&humanFriendlyBlocked);
            if (InterlockedIncrement(&botFriendlyLines) <= 30)
                gate::Event("bot: a hit of %.1f on the bot %08X by the human %08X refused (same side, %s)", amount, entity, human, MortalWhy());
            return false;
        }
        // who hit last: a bot's death is credited to that necro (versus::CreditBotKill)
        uint64_t ref = 0;
        uint16_t flags = 0;
        if (Read(reinterpret_cast<uintptr_t>(damage) + 0x88, &ref, 8) && ref) {
            Read(reinterpret_cast<uintptr_t>(damage) + 0x90, &flags, 2);
            for (unsigned i = 0; i < kMaxBots; ++i)
                if (units[i].entity == entity) { units[i].lastHitRef = ref; units[i].lastHitFlags = flags; units[i].lastHitAt = GetTickCount(); break; }
        }
        return true;
    }
    const uint32_t component = hits::FindHealthComponent(entity);
    float current = 0.0f, max = 0.0f;
    if (!component || !Read(component + hits::kHealthCurrent, &current, 4) || !Read(component + hits::kHealthMax, &max, 4) || !(max > 0.0f)) return true;
    if (current - amount < max * 0.25f) { InterlockedIncrement(&damageBlocked); return false; }
    return true;
}

// The floor where health really drops. HealthComponent::HandleDamage (0x00F74D60,
// thiscall(component, damage), ret 4; both health component kinds end here) is
// where the amount is final: an exploder's blast passed the Player's gate small
// and killed the bot outright here (07.10.: "death state message 18 ... health 0%",
// then a new body). For a bot the health is put back to its floor before
// anything reads it.
constexpr uint32_t kHealthApplyRva = 0xB74D60;
safetyhook::InlineHook healthApplyHook;
volatile LONG healthFloorCatches = 0, healthFloorLines = 0;
void __fastcall OnHealthApply(void* component, void*, const void* damage) {
    const uint32_t c = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(component));
    uint32_t owner = 0;
    float before = 0.0f, max = 0.0f;
    const bool ownerRead = Read(c + hits::kComponentOwner, &owner, 4) && owner;
    if (ownerRead && RefuseHitOnHuman(owner, damage, "HealthComponent::HandleDamage")) return;   // the second net behind the player's gate
    const bool ghost = ownerRead && !IsBotEntity(owner) && IsBotGhost(owner);
    const bool bot = ownerRead && (IsBotEntity(owner) || ghost) && Read(c + hits::kHealthCurrent, &before, 4) &&
                     Read(c + hits::kHealthMax, &max, 4) && max > 0.0f && std::isfinite(before);
    healthApplyHook.thiscall<void>(component, damage);
    if (!bot || MortalNow()) return;
    float after = 0.0f;
    if (!Read(c + hits::kHealthCurrent, &after, 4)) return;
    const float floorAt = std::min(before, max * 0.25f);
    const float keep = floorAt > 1.0f ? floorAt : 1.0f;
    if (std::isfinite(after) && after >= keep) return;
    WriteBytes(c + hits::kHealthCurrent, &keep, 4);
    if (ghost) {
        InterlockedIncrement(&ghostFloorCatches);
        const LONG n = InterlockedIncrement(&ghostFloorLines);
        if (n == 1)
            gate::Event("test bot_ghost INFO: a hit took the bot ghost %08X (machine %02X) from %.0f to %.0f of %.0f health; put back to %.0f",
                        owner, MachineOf(owner), before, after, max, keep);
        else if (n <= 30)
            gate::Event("bot ghost: a hit took the bot ghost %08X (machine %02X) from %.0f to %.0f of %.0f health; put back to %.0f (the host owns its life)",
                        owner, MachineOf(owner), before, after, max, keep);
        return;
    }
    InterlockedIncrement(&healthFloorCatches);
    if (InterlockedIncrement(&healthFloorLines) <= 30)
        gate::Event("bot: a hit took the bot %08X from %.0f to %.0f of %.0f health past its damage gate; put back to %.0f", owner, before, after, max, keep);
}
// Once per ControllerManager update, from the pad filter hook.
// Input trace, to find how the human's keys reach the bot (26.09.: space made
// the bot stomp, not Isaac): button changes of pad 15 and anything that wrote
// pad 14 between two of the bot's own writes. Bounded.
volatile LONG inputTraces = 0, foreignPad14 = 0;
void TracePads(uint32_t slot) {
    if (!u->entity) { u->haveWritten = false; return; }
    uint16_t mine = 0, now15 = 0;
    Read(slot + 0x10, &mine, 2);
    Read(slot + (15 - u->controller) * kPadSize + 0x10, &now15, 2);
    if (u->haveWritten && mine != u->lastWritten) {
        InterlockedIncrement(&foreignPad14);
        if (InterlockedIncrement(&inputTraces) <= 40)
            gate::Event("bot input: pad %u buttons %04X, we wrote %04X (pad 15 now %04X)", u->controller, mine, u->lastWritten, now15);
    }
    if (now15 != lastSeen15) {
        if (InterlockedIncrement(&inputTraces) <= 40) gate::Event("bot input: pad 15 buttons %04X -> %04X (pad %u %04X)", lastSeen15, now15, u->controller, mine);
        lastSeen15 = now15;
    }
}

// The Back pulse is over or cut short: the bot's pad no longer counts as connected.
void Unplug() {
    u->backStep = 0;
    u->rawStep = 0;
    const uint32_t slot = PadSlot();
    const unsigned char off = 0;
    if (slot) WriteBytes(slot + kPadConnected, &off, 1);
}
// DeviceManager's rumble pass (PC 0x0077F9F0, thiscall(dm), ret; run on a few
// messages, e.g. when a store closes) asks the input device behind every pad
// slot that counts as connected. The bot's pads have none: with the Back pulse
// on, it read garbage and crashed at 0x0140E72A (27.09., a bot leaving the store).
// For that pass the bots' pads are unplugged.
constexpr uint32_t kRumblePassRva = 0x37F9F0;
safetyhook::InlineHook rumbleHook;
volatile LONG rumbleGuards = 0;
void __fastcall OnRumblePass(void* dm, void*) {
    unsigned char saved[kMaxBots]{};
    const uint32_t manager = reinterpret_cast<uint32_t>(dm);
    const unsigned char off = 0;
    for (unsigned i = 0; i < kMaxBots; ++i) {
        const uint32_t slot = manager + kPadBase + units[i].controller * kPadSize;
        if (Read(slot + kPadConnected, &saved[i], 1) && saved[i]) {
            WriteBytes(slot + kPadConnected, &off, 1);
            InterlockedIncrement(&rumbleGuards);
        }
    }
    rumbleHook.thiscall<void>(dm);
    for (unsigned i = 0; i < kMaxBots; ++i)
        if (saved[i]) WriteBytes(manager + kPadBase + units[i].controller * kPadSize + kPadConnected, &saved[i], 1);
}
void WritePadOf(Unit& unit) {
    u = &unit;
    const uint32_t slot = PadSlot();
    if (!slot) return;
    TracePads(slot);
    Pad p = u->padLive && u->entity ? u->pad : Pad{};
    // A kiosk menu reads Back straight from the pad, only while it counts as
    // connected: on for the frames of the press, a clear frame first for the edge.
    if (u->backStep) {
        p = Pad{};
        p.buttons = u->backStep == 2 || u->backStep == 3 ? kBackButton : 0;
        const unsigned char plugged = u->backStep <= 3 ? 1 : 0;
        WriteBytes(slot + kPadConnected, &plugged, 1);
        u->backStep = u->backStep >= 4 ? 0 : u->backStep + 1;
    }
    // A button the game reads raw (the kinesis throw) the same way, over the
    // normal pad: a frame without it for the edge, two with it, then unplugged.
    if (u->rawStep && !u->backStep) {
        p.buttons = static_cast<uint16_t>(u->rawStep == 2 || u->rawStep == 3 ? (p.buttons | u->rawBits) : (p.buttons & ~u->rawBits));
        const unsigned char plugged = u->rawStep <= 3 ? 1 : 0;
        WriteBytes(slot + kPadConnected, &plugged, 1);
        u->rawStep = u->rawStep >= 4 ? 0 : u->rawStep + 1;
    }
    u->lastWritten = p.buttons;
    u->haveWritten = u->entity != 0;
    float sticks[4] = {p.lx, p.ly, p.rx, p.ry};
    WriteBytes(slot, sticks, sizeof(sticks));
    WriteBytes(slot + 0x10, &p.buttons, 2);
}
void WritePad() {
    if (!enabled) return;
    Unit* const saved = u;
    for (unsigned i = 0; i < wantedBots; ++i) WritePadOf(units[i]);
    u = saved;
}

// --- lifecycle ------------------------------------------------------------------

void StopAntic(const char* why);   // "antics", runtime_coop_bot_antics.inc
void Let(const char* why) {
    StopAntic("its body was let go");
    if (u->gunPending >= 0) {
        u->creditWallet += static_cast<int32_t>(u->gunPrice);
        u->gunPending = -1;
        u->gunProxy = 0;
        u->gunPrice = 0;
    }
    if (u->entity) {
        InterlockedIncrement(&drops);
        gate::Event("bot %u: let go of %08X (%s)", u->index + 1, u->entity, why);
    }
    if (u->entity) pastBots[pastBotNext++ % 8] = u->entity;
    u->entity = 0;
    u->died = false;   // a death message of the old body never reaches the next one
    u->zeroSince = u->zeroCheckAt = 0;
    Publish();
    u->padLive = false;
    u->pad = Pad{};
    u->state = enabled && !userOff && !u->failedForGood ? State::Waiting : State::Off;
    u->settledSince = 0;
    u->steer.Reset();
    u->cal = Calibration{};
    u->weaponSwitches = 0;
    u->noCutter = false;
    u->nodesSet = false;
    u->nodesSeen = -1;
    u->creditsSet = false;
    u->creditsSeen = -1;
    u->kioskVisit = KioskStep::None;
    // A new body starts without the old one's throw or half-done scan.
    u->tkStep = TkStep::None;
    u->tkRedirect = u->tkLaunchWanted = false;
    u->tkMedpackUntil = u->tkHeldSince = 0;
    u->tkWaitWhy = nullptr;
    u->scanActive = false;
    u->killId = 0;
    u->leaderLine = -1;
    Unplug();
    u->suitAt = 0;
    u->suitDone = false;
    // A body let go took its hide count with it; nothing to show.
    u->hiddenBody = u->hiddenItem = u->hiddenHelmet = 0;
    u->sceneSince = u->farSince = u->cabinMissSince = 0;
    u->liftHoldUntil = 0;
    u->flying = u->zeroG = u->bodyKnown = u->zgWantsCatchUp = false;
    u->lowerState = u->zgState = -1;
    u->zgSm = 0;
    u->flightStart = u->landingSince = u->landHoldUntil = u->otherSurfaceSince = u->signFrameAt = 0;
    u->modulesKnown = u->stasisKnown = false;
    u->tkModule = true;
    u->stasis01 = 1.0f;
    u->holding = u->goingThere = false;   // a new body follows
    u->face = FaceUnit{};
    // Nothing the old body saw or measured carries over to the next one.
    u->seen = bot_sense::Snapshot{};
    u->refreshed = coop_bot::Enemy{};
    u->refreshAt = 0;
    u->targetHealthOf = u->targetHealth = 0;
}
void ResetOrders(const char* why);   // "the leader's orders", below
// After a checkpoint restart, and while the main menu is up, no bot does anything
// until the level manager starts the level again (loadsync::levelStarts) and a
// moment has passed. 06.10.: the leader's body outlives a restart, so the level
// looked settled and the bots spawned again mid-restart; the restore point never
// ran and the loading screen stayed up for good.
constexpr DWORD kHoldGraceMs = 1500, kHoldNoSignalMs = 8000, kHoldMaxMs = 180000;
// From the main menu the level is new and must already stand (settled): the bots come
// right after, in time for the 3 s start of the clock modes (13.10.).
constexpr DWORD kMenuHoldGraceMs = 300;
bool holdActive = false;
const char* holdWhy = "";
DWORD holdSince = 0;
LONG holdStartsSeen = 0;
volatile LONG holdsEntered = 0, holdsReleased = 0, holdsWithoutSignal = 0, holdsTimedOut = 0, holdFrames = 0;
void EnterHold(const char* why) {
    const DWORD now = GetTickCount();
    holdStartsSeen = InterlockedCompareExchange(&loadsync::levelStarts, 0, 0);
    holdSince = now;
    if (!holdActive) {
        InterlockedIncrement(&holdsEntered);
        gate::Event("bot hold: %s, every bot stays out until the level starts again", why);
    }
    holdActive = true;
    holdWhy = why;
}
bool FrontendNow() {
    char name[16]{};
    const uint32_t loop = level::GameMainLoop();
    return loop && Read(loop + level::kMapOffset, name, sizeof(name) - 1) && _strnicmp(name, "frontend", 8) == 0;
}
// True while the bots must stay out.
bool HoldTick(bool standing, DWORD now) {
    if (FrontendNow()) {
        if (!holdActive || std::strcmp(holdWhy, "the main menu") != 0) EnterHold("the main menu");
        else holdSince = now;   // released only by a start after the menu
    }
    if (!holdActive) return false;
    InterlockedIncrement(&holdFrames);
    const LONG starts = InterlockedCompareExchange(&loadsync::levelStarts, 0, 0);
    const char* how = nullptr;
    if (starts != holdStartsSeen) {
        const DWORD grace = std::strcmp(holdWhy, "the main menu") == 0 ? kMenuHoldGraceMs : kHoldGraceMs;
        if (standing && TicksSince(now, loadsync::lastStartAt) >= grace) { how = "the level started"; InterlockedIncrement(&holdsReleased); }
    } else if (!loadsync::installed && standing && now - holdSince >= kHoldNoSignalMs) {
        how = "no start signal (the level manager hook is off), standing a while";
        InterlockedIncrement(&holdsWithoutSignal);
    } else if (now - holdSince >= kHoldMaxMs) {
        how = "TIMEOUT without a level start";
        InterlockedIncrement(&holdsTimedOut);
    }
    if (!how) return true;
    gate::Event("bot hold over after %lu ms (%s): %s", static_cast<unsigned long>(now - holdSince), holdWhy, how);
    holdActive = false;
    return false;
}
void OnRestart() {
    EnterHold("a checkpoint restart");
    Unit* const saved = u;
    for (Unit& unit : units) {
        u = &unit;
        if (u->entity) Let("a checkpoint restart deletes it with the other Players");
    }
    u = saved;
    ResetOrders("a checkpoint restart");
}

// Viewport v belongs to Player id v; the count covers the highest slot in use.
bool SetViewports(uint32_t count) {
    const uint32_t cm = CameraManager();
    uint32_t max = 0, now = 0;
    if (!cm || !Read(cm + kCmNumViewports - 4, &max, 4) || max < 2 || max > 4 || count > max) return false;
    for (uint32_t v = 1; v < count; ++v) WriteBytes(cm + kCmPlayerIds + v * 4, &v, 4);
    if (Read(cm + kCmNumViewports, &now, 4) && now >= count) return true;
    return WriteBytes(cm + kCmNumViewports, &count, 4);
}
// Fewer viewports once the bots above are gone and their blenders hold nothing of them.
void ReleaseViewport() {
    const uint32_t cm = CameraManager();
    uint32_t count = 0, blenders = 0, want = 1;
    if (!cm || !Read(cm + kCmNumViewports, &count, 4) || count < 2 || count > 4 || !Read(cm + kCmBlenders, &blenders, 4) || !blenders) return;
    for (const Unit& unit : units) if (unit.entity && unit.slotId + 1 > want) want = unit.slotId + 1;
    if (want >= count) return;
    for (uint32_t v = want; v < count; ++v) {
        int32_t index = 0;
        if (!Read(blenders + v * kBlenderSize + kBlenderStackIndex, &index, 4) || index != -1) return;
    }
    WriteBytes(cm + kCmNumViewports, &want, 4);
}

// --- attack slots ------------------------------------------------------------------
// Enemies took a bot as their target and never attacked it (05.10.). An enemy asks
// its target for an attack slot (HasAttackSlot 0x0086A2F0, RequestAttackPermission
// 0x0086A3F0), and Player::ComputeAttackerSlots (0x0086A980, every frame) fills the
// three slots (+0x750 + i*0x30 valid) only when the phase at +0xB48 is not -1
// (cmp ebx,-1 / je at 0x0086A9C9). The phase is set by Player::RegisterWithManager
// alone (0x0086C1C0: id % 15 at 0x0086C23B), which returns at once once +0x420 is
// set, and Player::Init calls it only while +0x420 is -1. The bot spawn registers
// directly and writes +0x420 first, so a bot kept the ctor's -1 (0x008A76A2): dump
// 27388 has Isaac at phase 0, the bots at -1 with no valid slot. Without a slot an
// enemy stands by the bot (CadaverousMainSM UpdateApproachTarget 0x00FFDA44).
constexpr uint32_t kAttackPhase = 0xB48, kAttackers = 0x940, kAttackSlotValid = 0x750, kAttackSlotStride = 0x30, kAttackGranted = 0xB44;
constexpr uint32_t kSlotsPhaseCheckRva = 0x46A9C9, kCtorPhaseRva = 0x4A76A2, kRegisterWithManagerRva = 0x46C1C0;
bool attackPhaseReady = false;
volatile LONG attackPhaseSets = 0, attackPhaseRestored = 0, attackPhaseFaults = 0;
bool InstallAttackPhase(FILE* log) {
    // mov ebx,[esi+0B48h] / cmp ebx,-1 ; mov dword [esi+0B48h],-1 ; push ebx / mov ebx,ecx / cmp dword [ebx+420h],-1 / jne
    static const unsigned char kCheck[] = {0x8B, 0x9E, 0x48, 0x0B, 0x00, 0x00, 0x83, 0xFB, 0xFF};
    static const unsigned char kCtor[] = {0xC7, 0x86, 0x48, 0x0B, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    static const unsigned char kRegister[] = {0x53, 0x8B, 0xD9, 0x83, 0xBB, 0x20, 0x04, 0x00, 0x00, 0xFF, 0x75, 0x75};
    unsigned char check[sizeof(kCheck)]{}, ctor[sizeof(kCtor)]{}, reg[sizeof(kRegister)]{};
    const bool checkOk = Read(base + kSlotsPhaseCheckRva, check, sizeof(check)) && std::memcmp(check, kCheck, sizeof(kCheck)) == 0;
    const bool ctorOk = Read(base + kCtorPhaseRva, ctor, sizeof(ctor)) && std::memcmp(ctor, kCtor, sizeof(kCtor)) == 0;
    const bool regOk = Read(base + kRegisterWithManagerRva, reg, sizeof(reg)) && std::memcmp(reg, kRegister, sizeof(kRegister)) == 0;
    attackPhaseReady = checkOk && ctorOk && regOk;
    std::fprintf(log, "bot attack_phase=%d (slots_check=%d ctor=%d register=%d: a bot gets the attack-slot phase id %% 15 the game gives a Player it "
                      "registers itself, so enemies attack it)\n",
                 attackPhaseReady ? 1 : 0, checkOk ? 1 : 0, ctorOk ? 1 : 0, regOk ? 1 : 0);
    return attackPhaseReady;
}
bool SetAttackPhase(uint32_t entity, uint32_t id) {
    if (!attackPhaseReady) return false;
    const int32_t phase = static_cast<int32_t>(id % 15u);
    if (!WriteBytes(entity + kAttackPhase, &phase, 4)) { InterlockedIncrement(&attackPhaseFaults); return false; }
    InterlockedIncrement(&attackPhaseSets);
    return true;
}

// A partner's ghost registers in the first free Player slot from 1 up, and a bot's
// slot (1..3, it picks the bot's pad and viewport) is free while the bot has no body:
// a respawning necromorph took slot 3 for good on 12.10. and the host had two bots.
// The ghost is registered again in a free slot from 4 up (capacity 8); a ghost has
// no viewport, its id only names the slot. Player id +0x420, NecroPlayer id +0x3DC.
bool moveGhostsOut = true;   // [Bot] MoveGhostsOut
volatile LONG ghostsMovedOut = 0, ghostMoveRefused = 0, ghostMoveFaults = 0;
bool MoveGhostOut(unsigned slot, uint32_t bp) {
    if (!moveGhostsOut || bp < 0x103C0) return false;
    const uint32_t entity = bp - kBasePlayer;
    const bool isPlayer = IsGhostPlayer(entity), isNecro = !isPlayer && IsGhostNecro(entity);
    if ((!isPlayer && !isNecro) || IsBotEntity(entity)) {
        if (InterlockedIncrement(&ghostMoveRefused) <= 10) gate::Event("bot slot %u: %08X is no partner's ghost, left there", slot, entity);
        return false;
    }
    uint32_t pm = 0, capacity = 0;
    if (!Read(base + kPlayerManagerRva, &pm, 4) || !pm || !Read(pm + 8, &capacity, 4) || capacity > 8) return false;
    unsigned to = 0;
    for (unsigned id = kMaxBots + 1; id < capacity && !to; ++id)
        if (!SlotOccupant(id)) to = id;
    if (!to) {
        if (InterlockedIncrement(&ghostMoveRefused) <= 10) gate::Event("bot slot %u: ghost %08X stays, no free slot from %u to %u", slot, entity, kMaxBots + 1, capacity - 1);
        return false;
    }
    uint32_t got = 0xFFFFFFFFu;
    __try {
        reinterpret_cast<bool(__thiscall*)(void*, uint32_t, int)>(base + kPmUnregisterRva)(reinterpret_cast<void*>(pm), bp, 1);
        got = reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t, uint32_t, int)>(base + kPmRegisterRva)(reinterpret_cast<void*>(pm), bp, to, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&ghostMoveFaults);
        gate::Event("bot slot %u: FAULT moving ghost %08X to slot %u", slot, entity, to);
        return false;
    }
    WriteBytes(entity + (isPlayer ? kPlayerId : necro::kPlayerId), &got, 4);
    if (isPlayer) SetAttackPhase(entity, got);
    const bool ok = got == to && SlotOccupant(to) == bp && !SlotOccupant(slot);
    if (ok) InterlockedIncrement(&ghostsMovedOut);
    gate::Event("bot slot %u: %s ghost %08X (machine %d) registered again in slot %u: %s (returned %d, slot %u now %08X)", slot, isPlayer ? "Isaac" : "necromorph",
                entity, static_cast<int>(MachineOf(entity)), to, ok ? "done" : "NOT AS ASKED", static_cast<int>(got), slot, SlotOccupant(slot));
    return ok;
}
// Phase, attacker count, the three slots' valid bytes and the granted count of one Player, for the log.
int DescribeSlots(uint32_t player, char* out, size_t size) {
    int32_t phase = -2, granted = -1;
    unsigned char valid[3]{}, waiting = 0;
    Read(player + kAttackPhase, &phase, 4);
    Read(player + kAttackers, &waiting, 1);   // the waiting attackers' count is one byte (dump 8396)
    const int attackers = waiting;
    Read(player + kAttackGranted, &granted, 4);
    for (unsigned i = 0; i < 3; ++i) Read(player + kAttackSlotValid + i * kAttackSlotStride, &valid[i], 1);
    return std::snprintf(out, size, "phase %d attackers %d slots %u%u%u granted %d", phase, attackers, valid[0] & 1, valid[1] & 1, valid[2] & 1, granted);
}

// Every 2 s, when anything changed: the leader's and each bot's slots in one line.
DWORD slotsReportAt = 0;
char slotsLast[256] = "";
volatile LONG slotsLines = 0;
void SlotsReport(uint32_t leader, const uint32_t* bots, unsigned count, DWORD now) {
    if (now - slotsReportAt < 2000) return;
    slotsReportAt = now;
    char line[256];
    int n = std::snprintf(line, sizeof(line), "bot attack_slots leader: ");
    n += DescribeSlots(leader, line + n, sizeof(line) - n);
    for (unsigned i = 0; i < count && n > 0 && n < static_cast<int>(sizeof(line)) - 1; ++i) {
        if (!bots[i]) continue;
        n += std::snprintf(line + n, sizeof(line) - n, " | bot %u: ", i + 1);
        if (n < static_cast<int>(sizeof(line)) - 1) n += DescribeSlots(bots[i], line + n, sizeof(line) - n);
    }
    if (std::strcmp(line, slotsLast) == 0) return;
    std::memcpy(slotsLast, line, sizeof(slotsLast));
    if (InterlockedIncrement(&slotsLines) <= 300) gate::Event("%s", line);
}

__declspec(noinline) uint32_t SpawnRaw(uint32_t sim, necro::SpawnInfo& info) {
    __try {
        return necro::SpawnByInfo(sim, info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
const uint32_t* BirthSuitGuid();   // below, with the suits
bool WornSuit(uint32_t player, uint32_t* guid);   // below, with the suits
__declspec(noinline) bool EngineSteps(uint32_t entity, uint32_t pad15Before, bool& padMoved, bool& registered) {
    uint32_t pm = 0, cm = 0;
    if (!Read(base + kPlayerManagerRva, &pm, 4) || !pm || !Read(base + kControllerManagerRva, &cm, 4) || !cm) return false;
    __try {
        // Controller: off Isaac's pad 15, onto 14. Popped only if the spawn pushed one
        // on top of Isaac's; otherwise the one there is Isaac's own.
        if (pad15Before != 0xFFFFFFFFu && ConfigCount(15) == pad15Before + 1)
            reinterpret_cast<bool(__thiscall*)(void*, uint32_t)>(base + kPopConfigRva)(reinterpret_cast<void*>(cm), 15);
        WriteBytes(entity + kPlayerController, &u->controller, 4);
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kSetControllerConfigRva)(reinterpret_cast<void*>(entity), entity + kPlayerConfigGuid);
        padMoved = true;
        // PlayerManager id 1, set before Init so Init does not register it into slot 0.
        const uint32_t id = reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t, uint32_t, int)>(base + kPmRegisterRva)(
            reinterpret_cast<void*>(pm), entity + kBasePlayer, u->slotId, 1);
        WriteBytes(entity + kPlayerId, &id, 4);
        registered = id == u->slotId;
        if (id != 0xFFFFFFFFu) SetAttackPhase(entity, id);   // what RegisterWithManager would have set ("attack slots")
        uint32_t flags = 0;
        Read(entity + kPlayerFlags, &flags, 4);
        flags |= kFlagNoPaired;
        WriteBytes(entity + kPlayerFlags, &flags, 4);
        // Player::Init, with the upgrade manager left alone; a body after the first is born in
        // the bot's own suit (coop_uniform::ArmBirth), so it needs no change afterwards.
        const uint32_t* suit = BirthSuitGuid();
        const bool birth = suit && coop_uniform::ArmBirth(suit, "a bot (its own suit again)");
        botInit = true;
        reinterpret_cast<void(__stdcall*)(uint32_t, int)>(base + necro::kFinalize)(entity, 0);
        botInit = false;
        if (birth) coop_uniform::DisarmBirth(true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        botInit = false;
        coop_uniform::DisarmBirth(false);
        return false;
    }
}

void Despawn(const char* why);   // below
volatile LONG necroLeaderWaits = 0;
void Spawn(uint32_t leader, const Vec3& leaderPos, DWORD now) {
    // The bot is spawned from its leader's own template. A necromorph leader (the
    // host on the necro side of a Versus round) has no Player template to lend:
    // 12.10. 00:31 the spawn made a stray NecroPlayer ("not a Player") and the
    // bots never came back. They wait for a human leader instead.
    if (!Alive(leader)) {
        if (InterlockedIncrement(&necroLeaderWaits) <= 3)
            gate::Event("bot %u: the leader %08X is no Player (a necromorph); no bot spawns from it", u->index + 1, leader);
        return;
    }
    uint32_t sim = 0, guid[4]{};
    if (!Read(base + necro::kSimManager, &sim, 4) || !sim || !Read(leader + 0x24, guid, 16)) return;
    const uint32_t pad15Before = ConfigCount(15);
    SnapIsaac(*u, leader);
    u->isaacWaits = 0;
    if (!SetViewports(u->slotId + 1)) {
        gate::Event("bot %u: the camera manager has no room for viewport %u; no bot there", u->index + 1, u->slotId);
        u->state = State::Failed;
        return;
    }
    necro::SpawnInfo info{};
    if (!necro::GetSpawnInfo(sim, info, guid)) { InterlockedIncrement(&spawnFailures); gate::Event("bot: no spawn info for the level's Player template"); return; }
    Vec3 at = SpotNearLeader(leaderPos);
    // Versus: back at a spawn point the way a player respawns, near the team; the
    // leader's side when the map has none in use (and always in the campaign)
    bool atPoint = false;
    if (u->diedAt && VersusMortal()) {
        const float toward[3] = {leaderPos.x, leaderPos.y, leaderPos.z};
        float point[3]{};
        if (mpmap::RespawnSpot(toward, point)) {
            const float a = static_cast<float>(u->index) * 2.1f;   // two bots back at once do not share a spot
            at = {point[0] + std::sin(a) * 0.9f, point[1], point[2] + std::cos(a) * 0.9f};
            atPoint = true;
            InterlockedIncrement(&versusSpawnPoints);
        }
    }
    alignas(16) float matrix[16];
    if (!Read(leader + necro::kEntityMatrix, matrix, sizeof(matrix))) return;
    matrix[12] = at.x; matrix[13] = at.y; matrix[14] = at.z;
    reinterpret_cast<void(__thiscall*)(necro::SpawnInfo*, const float*)>(base + necro::kSetTransform)(&info, matrix);
    const uint32_t entity = SpawnRaw(sim, info);
    if (!Alive(entity)) {
        InterlockedIncrement(&spawnFailures);
        gate::Event("bot: the spawn returned %08X, not a Player", entity);
        return;
    }
    bool padMoved = false, registered = false;
    const uint32_t pad15Spawned = ConfigCount(15);
    const bool steps = EngineSteps(entity, pad15Before, padMoved, registered);
    // Born in its own suit that streams in: no Uniform until it is here. The first body check
    // waits as long as after a change, or it dressed the bot in Isaac's suit meanwhile.
    uint32_t bornIn[4]{};
    const bool birthStreams = steps && BirthSuitGuid() && !WornSuit(entity, bornIn);
    u->entity = entity;
    Publish();
    u->spawnedAt = now;
    u->dressCheckAt = now + (birthStreams ? 12000 : 1500);
    u->dressTries = 0;
    MoveEntity(entity, at);   // Player::Init put it at the level checkpoint
    u->guardSpawn = play::SurvivalMode() && mpmap::InVisit();
    u->spawnTarget = u->spawnLast = at;
    u->spawnSampleAt = now;
    u->spawnGuardTries = 0;
    InterlockedIncrement(&spawns);
    gate::Event("bot %u: spawned %08X at (%.1f %.1f %.1f) %s %08X: engine_steps=%d registered=%d id=%u pad15 %u->%u->%u pad%u=%u", u->index + 1,
                entity, at.x, at.y, at.z, atPoint ? "at an MP spawn point near" : "next to", leader, steps ? 1 : 0, registered ? 1 : 0, registered ? u->slotId : 0u, pad15Before, pad15Spawned, ConfigCount(15),
                u->controller, ConfigCount(u->controller));
    {
        int32_t phase = -2, id = -2;
        Read(entity + kAttackPhase, &phase, 4);
        Read(entity + kPlayerId, &id, 4);
        gate::Event("bot %u: attack-slot phase %d with PlayerManager id %d (fix %s)", u->index + 1, phase, id, attackPhaseReady ? "on" : "off");
    }
    if (!steps || !registered || ConfigCount(u->controller) == 0 || ConfigCount(u->controller) == 0xFFFFFFFFu) {
        gate::Event("bot %u: setup incomplete (steps=%d registered=%d pad%u=%u); it stays out until the level changes", u->index + 1, steps ? 1 : 0,
                    registered ? 1 : 0, u->controller, ConfigCount(u->controller));
        u->failedForGood = true;
        Despawn("setup incomplete");
        return;
    }
    u->state = State::Calibrating;
    u->cal = Calibration{};
    u->cal.stepAt = now;
}

// Deliberate removal (the toggle key, a failed body): the engine's own teardown.
__declspec(noinline) void Despawn(const char* why) {
    const uint32_t entity = u->entity;
    Let(why);
    if (!Alive(entity)) return;
    uint32_t pm = 0;
    __try {
        if (Read(base + kPlayerManagerRva, &pm, 4) && pm)
            reinterpret_cast<bool(__thiscall*)(void*, uint32_t, int)>(base + kPmUnregisterRva)(reinterpret_cast<void*>(pm), entity + kBasePlayer, 1);
        uint32_t vtable = 0, shutdown = 0;
        if (Read(entity, &vtable, 4) && Read(vtable + 0x1C, &shutdown, 4) && shutdown)
            reinterpret_cast<void(__thiscall*)(uint32_t)>(shutdown)(entity);
        reinterpret_cast<void(__cdecl*)(uint32_t)>(base + necro::kDelete)(entity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gate::Event("bot: fault while removing %08X", entity);
    }
}

// The room the bot is drawn in. The level's SetOcclusion objects set the scene
// mask of their targets when a door closes behind the player (which zones show);
// they find their target by GUID through SimManager::FindBase, which answers with
// Isaac alone, so the bot kept the mask of the room behind the door and was not
// drawn in the new one (26.09.). The mask is a dword at Entity+0x88; the virtual
// at +0x58 (PC 0x007D0C10, thiscall(set, clear), ret 8: new = set | (old & ~clear))
// stores it and tells the model. The bot takes the leader's whenever they differ.
constexpr uint32_t kSceneMask = 0x88, kSetSceneMaskSlot = 0x58, kSetSceneMaskRva = 0x3D0C10;
bool sceneMaskReady = false;
// [Bot] Suit=1: each bot changes into a suit of its own once its body stands,
// through the same native ChangeToUniform (a suit not loaded is streamed in, as
// for a store change). The body check runs again afterwards: should the bot
// hold Isaac's old model (or Isaac have lost his), he is dressed again and the
// bot leaves; should the bot's body not come together, it is dressed in the
// local suit again.
bool ownSuits = false;
volatile LONG suitChanges = 0, suitFailures = 0;
void PutOnOwnSuit(uint32_t leader, DWORD now);   // "suits", below the weapons
volatile LONG sceneMaskSyncs = 0, sceneMaskFaults = 0;
__declspec(noinline) bool SetSceneMaskRaw(uint32_t entity, uint32_t fn, uint32_t mask) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(entity)), mask, 0xFFFFFFFFu);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void SyncSceneMask(uint32_t leader) {
    uint32_t want = 0, have = 0, vtable = 0, fn = 0;
    if (!sceneMaskReady || !Read(leader + kSceneMask, &want, 4) || !Read(u->entity + kSceneMask, &have, 4) || want == have) return;
    if (!Read(u->entity, &vtable, 4) || !Read(vtable + kSetSceneMaskSlot, &fn, 4) || fn != base + kSetSceneMaskRva) return;
    if (!SetSceneMaskRaw(u->entity, fn, want)) {
        if (InterlockedIncrement(&sceneMaskFaults) <= 5) gate::Event("bot: setting its scene mask faulted");
        return;
    }
    if (InterlockedIncrement(&sceneMaskSyncs) <= 20) gate::Event("bot: scene mask %08X -> %08X (the leader's room)", have, want);
}

// Isaac's body as it is now, for the next check of this unit.
void SnapIsaac(Unit& unit, uint32_t leader);   // "suits", below
// Every pending check against Isaac's body as it is now: after his own suit
// change or a repair, the old snapshots would blame the next bot (05.10.: one
// repair made a new model instance and the other two bots were let go for it).
volatile LONG isaacOwnChanges = 0, isaacSnapRefreshes = 0, isaacWaitsTotal = 0;
void RefreshIsaacSnaps(uint32_t leader) {
    for (Unit& unit : units) {
        if (!unit.entity) continue;
        SnapIsaac(unit, leader);
        InterlockedIncrement(&isaacSnapRefreshes);
    }
}
const char* SuitName(int idx);   // "suits", below
int WornSuitIndex(uint32_t player);

// A second Isaac needs a model instance of its own: the native ChangeToUniform
// with the local player's resident suit makes one (as for a partner's ghost).
// Isaac's own body must come through untouched.
void CheckBodies(uint32_t leader, DWORD now) {
    if (!u->dressCheckAt || now < u->dressCheckAt) return;
    uint32_t mine[3]{}, isaac[3]{};
    const bool botComplete = coop_uniform::BotBody(u->entity, mine);
    const bool isaacComplete = coop_uniform::BotBody(leader, isaac);
    const bool isaacSame = isaac[0] == u->isaacBody[0] && isaac[1] == u->isaacBody[1];
    uint32_t suit[4]{};
    const bool haveSuit = coop_uniform::BotSuit(suit);
    if (!isaacComplete || !isaacSame) {
        // Theft: the bot holds Isaac's old model instance, or Isaac lost his model
        // or skeleton. Isaac with model and bones but no Uniform is his own suit
        // still streaming in: looked at again shortly.
        const bool lost = !isaac[0] || !isaac[1];
        const bool holdsOld = u->isaacBody[0] && (mine[0] == u->isaacBody[0] || (u->isaacBody[1] && mine[1] == u->isaacBody[1]));
        const bool streaming = !isaacComplete && !lost && !isaac[2];
        if (streaming && !holdsOld && u->isaacWaits < 8) {
            ++u->isaacWaits;
            InterlockedIncrement(&isaacWaitsTotal);
            u->dressCheckAt = now + 1500;
            return;
        }
        if (!lost && !holdsOld) {
            InterlockedIncrement(&isaacOwnChanges);
            gate::Event("bot %u: Isaac's body changed on its own (mmi %08X->%08X, suit %s->%s, bot mmi %08X); the bot stays", u->index + 1,
                        u->isaacBody[0], isaac[0], SuitName(u->isaacSuit), SuitName(WornSuitIndex(leader)), mine[0]);
            RefreshIsaacSnaps(leader);
            u->isaacWaits = 0;
        } else {
            InterlockedIncrement(&isaacRepairs);
            gate::Event("bot %u: took Isaac's model (mmi %08X->%08X bones %08X->%08X, bot mmi %08X bones %08X, lost=%d): dressing Isaac again, the bot leaves",
                        u->index + 1, u->isaacBody[0], isaac[0], u->isaacBody[1], isaac[1], mine[0], mine[1], lost ? 1 : 0);
            if (haveSuit) coop_uniform::BotDress(leader, suit);
            u->failedForGood = true;
            Despawn("it took Isaac's model");
            RefreshIsaacSnaps(leader);   // the repair gave Isaac a new instance
            return;
        }
    }
    if (botComplete) {
        u->dressCheckAt = 0;
        u->isaacWaits = 0;
        if (ownSuits && !u->suitDone && !u->suitAt) u->suitAt = now + 2000;
        gate::Event("bot: own body ready (mmi %08X bones %08X uniform %08X), Isaac's untouched (mmi %08X)", mine[0], mine[1], mine[2], isaac[0]);
        return;
    }
    if (u->dressTries >= 2 || !haveSuit) {
        gate::Event("bot: no body of its own after %u tries (suit known=%d); the bot leaves", u->dressTries, haveSuit ? 1 : 0);
        u->failedForGood = true;
        Despawn("no model");
        return;
    }
    ++u->dressTries;
    InterlockedIncrement(&dressings);
    const bool dressed = coop_uniform::BotDress(u->entity, suit);
    gate::Event("bot: dressing the bot in the local suit %08X-%08X (try %u, call %s)", suit[0], suit[1], u->dressTries, dressed ? "ok" : "faulted");
    u->dressCheckAt = now + 1500;
}

// --- control --------------------------------------------------------------------

void Press(uint16_t bit, DWORD now, DWORD ms = 110) {
    for (unsigned i = 0; i < 16; ++i) if (bit == (1u << i)) u->pressUntil[i] = now + ms;
}
uint16_t Pressed(DWORD now) {
    uint16_t b = 0;
    for (unsigned i = 0; i < 16; ++i) if (u->pressUntil[i] && now < u->pressUntil[i]) b = static_cast<uint16_t>(b | (1u << i));
    return b;
}
float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Turns toward a look point, straight on the aim data: the camera's, or the
// gun's while aiming. Closes the gap at kGain per second, at most kRate rad/s.
// (The right stick cannot do it: a mouse fix replaces the look input of every
// Player, see runtime_coop_bot_aim.inc.)
volatile LONG lookCloseHolds = 0, lookReversals = 0, lookTurns = 0;
// Seconds since the last look, from the performance counter: GetTickCount steps
// in 15.6 ms, so at high frame rates most frames saw dt 0 and the next a jump.
float LookDt() {
    static LONGLONG freq = 0;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart ? f.QuadPart : 1; }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const LONGLONG last = u->lastLookQpc;
    u->lastLookQpc = t.QuadPart;
    return last ? Clamp(static_cast<float>(t.QuadPart - last) / static_cast<float>(freq), 0.0f, 0.05f) : 0.0f;
}
// [Bot] AimLead: a moving look point is led by its speed over the look's gain, the
// lag the look keeps behind it, so the aim sits on the point. Without it a slasher
// crossing at 3 m/s 5 m off keeps the aim about 4 deg behind, twice the shot
// tolerance there, and the brain holds the shot.
bool aimLead = true;
constexpr float kLookGain = 8.0f;   // the look closes its gap at this rate (1/s) times the profile's AimSpeed
constexpr float kLeadMax = 0.6f;   // metres
volatile LONG leadFrames = 0, leadCmSum = 0;
void Look(const Vec3& from, const Vec3& target, uint32_t block, DWORD now, const Vec3& pointVel = {}) {
    u->lastLookAt = now;
    const float dt = LookDt();
    float heading = 0.0f, pitch = 0.0f;
    if (!AimAngles(u->entity, block, heading, pitch)) return;
    Vec3 to = target;
    if (aimLead && block == kGunAim && coop_bot::Length(pointVel) > 0.05f) {
        Vec3 lead = coop_bot::Scale(pointVel, 1.0f / (kLookGain * u->aimSpeed));
        const float l = coop_bot::Length(lead);
        if (l > kLeadMax) lead = coop_bot::Scale(lead, kLeadMax / l);
        to = coop_bot::Add(target, lead);
        InterlockedIncrement(&leadFrames);
        InterlockedExchangeAdd(&leadCmSum, static_cast<LONG>(coop_bot::Length(lead) * 100.0f));
    }
    const Vec3 d = coop_bot::Sub(to, from);
    const float flat = std::sqrt(d.x * d.x + d.z * d.z);
    if (flat < 0.05f && std::fabs(d.y) < 0.05f) return;
    // A point this close flat swings the heading wildly with every small step of
    // either (06.10.: idle bots stood 0.3 m off the leader looking at him, heads
    // jittering). Not aiming, the heading holds.
    const bool holdHeading = flat < 1.0f && block == kCamAim;
    if (holdHeading) InterlockedIncrement(&lookCloseHolds);
    const float wantHeading = std::atan2(d.x, d.z), wantPitch = std::atan2(-d.y, flat < 0.3f ? 0.3f : flat);
    const float eh = holdHeading ? 0.0f : Wrap(wantHeading - heading), ep = Clamp(wantPitch, -1.2f, 1.2f) - pitch;
    constexpr float kGain = kLookGain, kRate = 6.0f;
    const float step = Clamp(kGain * u->aimSpeed * dt, 0.0f, 1.0f), cap = kRate * u->aimSpeed * dt;
    const float dy = std::fabs(eh) < 0.002f ? 0.0f : Clamp(eh * step, -cap, cap);
    const float dp = std::fabs(ep) < 0.002f ? 0.0f : Clamp(ep * step, -cap, cap);
    if (dy != 0.0f) {
        const float sign = dy > 0.0f ? 1.0f : -1.0f;
        if (u->lastTurnSign != 0.0f && sign != u->lastTurnSign) InterlockedIncrement(&lookReversals);
        u->lastTurnSign = sign;
        InterlockedIncrement(&lookTurns);
    }
    if (dy != 0.0f || dp != 0.0f) bot_aim::Turn(u->entity, block, dp, dy);
}
// The left stick toward a world point, in the frame the Isaac moves in.
// Not aiming, it walks the way it faces: the stick goes straight forward, and
// only once the camera has turned close to the way (Isaac's body turns only
// while he walks forward; 28.09.: a stick toward a point beside the camera made
// the bots walk sideways and backwards and never face anyone). Aiming, it may
// strafe and back off as a fighter does.
constexpr float kForwardOnlyDeg = 70.0f, kFullForwardDeg = 25.0f;
volatile LONG turnsInPlace = 0;
void Walk(const Vec3& from, const Vec3& to, float magnitude, uint32_t block, bool forwardOnly, Pad& p) {
    float heading = 0.0f, pitch = 0.0f;
    if (!AimAngles(u->entity, block, heading, pitch) && !AimAngles(u->entity, kCamAim, heading, pitch)) return;
    const float want = std::atan2(to.x - from.x, to.z - from.z);
    const float rel = Wrap(want - heading);
    if (forwardOnly) {
        const float off = std::fabs(rel) * 57.2957795f;
        if (off >= kForwardOnlyDeg) { InterlockedIncrement(&turnsInPlace); return; }   // turn first
        const float scale = off <= kFullForwardDeg ? 1.0f : (kForwardOnlyDeg - off) / (kForwardOnlyDeg - kFullForwardDeg);
        p.lx = 0.0f;
        p.ly = u->cal.forwardSign * magnitude * (scale < 0.3f ? 0.3f : scale);
        return;
    }
    p.lx = u->cal.strafeSign * std::sin(rel) * magnitude;
    p.ly = u->cal.forwardSign * std::cos(rel) * magnitude;
}

// The left stick once each way, a moment each, to learn its signs on this Isaac.
// (Looking needs no calibration: it turns the aim data directly.)
void Calibrate(const Vec3& self, DWORD now) {
    float heading = 0.0f, pitch = 0.0f;
    if (!AimAngles(u->entity, kCamAim, heading, pitch)) { if (now - u->cal.stepAt > 5000) u->cal.done = true; return; }
    constexpr DWORD kSettle = 1500, kLook = 300, kMove = 450, kGap = 250;
    Pad p;
    auto begin = [&](unsigned next) { u->cal.step = next; u->cal.stepAt = now; u->cal.startHeading = heading; u->cal.startPitch = pitch; u->cal.startPos = self; };
    switch (u->cal.step) {
    case 0: if (now - u->cal.stepAt >= kSettle) begin(7); break;   // the signs are fixed; only settle
    case 1: p.rx = 0.6f; if (now - u->cal.stepAt >= kLook) { const float dh = Wrap(heading - u->cal.startHeading);
                if (std::fabs(dh) > 0.02f) { u->cal.yawSign = dh > 0.0f ? 1.0f : -1.0f; u->cal.yawRate = Clamp(std::fabs(dh) / (kLook / 1000.0f) / 0.6f, 0.5f, 12.0f); }
                begin(2); } break;
    case 2: if (now - u->cal.stepAt >= kGap) begin(3); break;
    case 3: p.ry = 0.6f; if (now - u->cal.stepAt >= kLook) { const float dp = pitch - u->cal.startPitch;
                if (std::fabs(dp) > 0.02f) { u->cal.pitchSign = dp > 0.0f ? 1.0f : -1.0f; u->cal.pitchRate = Clamp(std::fabs(dp) / (kLook / 1000.0f) / 0.6f, 0.3f, 12.0f); }
                begin(4); } break;
    case 4: p.ry = -0.6f * 1.0f; if (now - u->cal.stepAt >= kLook) begin(5); break;   // back to level
    case 5: p.ly = 1.0f; if (now - u->cal.stepAt >= kMove) { const Vec3 moved = coop_bot::Sub(self, u->cal.startPos);
                const float along = moved.x * std::sin(u->cal.startHeading) + moved.z * std::cos(u->cal.startHeading);
                if (std::fabs(along) > 0.15f) u->cal.forwardSign = along > 0.0f ? 1.0f : -1.0f;
                begin(6); } break;
    case 6: p.lx = 1.0f; if (now - u->cal.stepAt >= kMove) { const Vec3 moved = coop_bot::Sub(self, u->cal.startPos);
                const float side = moved.x * std::cos(u->cal.startHeading) - moved.z * std::sin(u->cal.startHeading);
                if (std::fabs(side) > 0.15f) u->cal.strafeSign = side > 0.0f ? 1.0f : -1.0f;
                begin(7); } break;
    default:
        u->cal.done = true;
        gate::Event("bot: controls calibrated: yaw sign %+.0f rate %.2f rad/s, pitch sign %+.0f rate %.2f rad/s, forward %+.0f, strafe %+.0f", u->cal.yawSign,
                    u->cal.yawRate, u->cal.pitchSign, u->cal.pitchRate, u->cal.forwardSign, u->cal.strafeSign);
        break;
    }
    u->pad = p;
    u->padLive = true;
}

// --- weapons ----------------------------------------------------------------------
// The weapon inventory
// sits at Player+0xC04 (entries at +4, 0xC each, +4 = item+0x10 or 0; size at
// +8); slot order is the d-pad: up, left, right, down. The gun class is its
// vtable (the name hash follows the display name, which skins change). Rounds
// in the clip at gun+0x39C; spare rounds from Player::CountSpareAmmo (0x0086C0C0,
// thiscall(player, ammo type), ret 4) with the type at [[gun+0xC]+0x300].
// Player::SetDesiredItem (0x00869EB0, thiscall(player, item), ret 4) links the
// wish into Player+0x424 (item+0x10); PlayerSwitchItemSM then plays the change
// by itself. Never ask for the gun already in hand: no change starts and the
// wish would stay; a wish that does not come true in a few seconds is dropped.
constexpr uint32_t kWeaponInventory = 0xC04, kInventoryEntries = 0x4, kInventorySize = 0x8, kInventoryEntry = 0xC, kDesiredItem = 0x424;
constexpr uint32_t kGunClip = 0x39C, kGunAttributes = 0xC, kGunAmmoType = 0x300;
// Gun mechanics: the cutter's blade, target
// byte +0x424 (1 = horizontal) and animated float +0x420 (0.2 s a turn); the javelin
// gun keeps its newest spear at +0x420 (spear+0x10), the spear the entity it is
// stuck in at +0x588; the line gun's mine fuse is the float at +0x448 (seconds).
// Alt fire counts only in the aiming state (Player+0x298 bit 7) and on a fresh press.
constexpr uint32_t kCutterTarget = 0x424, kCutterBlend = 0x420, kJavelinSpear = 0x420, kSpearStuckIn = 0x588, kMineDuration = 0x448;
constexpr uint32_t kBehaviourFlags = 0x298, kAimingFlag = 1u << 7;
constexpr uint32_t kSetDesiredItemRva = 0x469EB0, kCountSpareAmmoRva = 0x46C0C0;
constexpr uint32_t kLineGunVtableRva = 0x17511B8, kJavelinGunVtableRva = 0x1750D98;
// The other guns (DS2_BOT_WEAPONS_INVENTORY): the Ripper's class is Yoyo, the Detonator's DetonatorGun.
constexpr uint32_t kPulseRifleVtableRva = 0x1751A00, kRipperVtableRva = 0x1752580, kDetonatorVtableRva = 0x1753100, kSeekerVtableRva = 0x1773780,
                   kFlamethrowerVtableRva = 0x1753520, kForceGunVtableRva = 0x1753948, kContactBeamVtableRva = 0x1752CE0;
// DS2_BOT_WEAPONS_BATCH_A: the seeker's scope byte +0x420 (stays as it was left),
// the ripper's hanging blade +0x374, and the level's detonator mines, one list
// for every player (64 slots at 0x020976B8, count at 0x020979B8).
constexpr uint32_t kSeekerZoom = 0x420, kRipperHeld = 0x374, kDetonatorCountRva = 0x1C979B8;
// DS2_BOT_WEAPONS_BATCH_B: the contact beam's charge needed +0x420 and charge now +0x424.
constexpr uint32_t kContactNeed = 0x420, kContactCharge = 0x424;
bool gunsReady = false;
volatile LONG gunSwitches = 0, gunWishesDropped = 0, gunFaults = 0, altFires = 0, stomps = 0, stompsLost = 0;
coop_bot::Gun GunKind(uint32_t gun) {
    uint32_t vtable = 0;
    if (!gun || !Read(gun, &vtable, 4)) return coop_bot::Gun::None;
    if (vtable == base + 0x1751E20) return coop_bot::Gun::None;   // a GunProxy placeholder waiting for its gun (the weapon economy)
    // The flashlight of the first chapter is a plain Gun (vtable 0x01B50660, named
    // "Flashlight", name hash A8969626 at +0x25C): no weapon (29.09.: the bots aimed with it).
    uint32_t nameHash = 0;
    if (vtable == base + 0x1750660 || (Read(gun + 0x25C, &nameHash, 4) && nameHash == 0xA8969626u)) return coop_bot::Gun::None;
    if (vtable == base + kPlasmaCutterVtableRva) return coop_bot::Gun::PlasmaCutter;
    if (vtable == base + kLineGunVtableRva) return coop_bot::Gun::LineGun;
    if (vtable == base + kJavelinGunVtableRva) return coop_bot::Gun::Javelin;
    if (vtable == base + kPulseRifleVtableRva) return coop_bot::Gun::PulseRifle;
    if (vtable == base + kRipperVtableRva) return coop_bot::Gun::Ripper;
    if (vtable == base + kDetonatorVtableRva) return coop_bot::Gun::Detonator;
    if (vtable == base + kSeekerVtableRva) return coop_bot::Gun::SeekerRifle;
    if (vtable == base + kFlamethrowerVtableRva) return coop_bot::Gun::Flamethrower;
    if (vtable == base + kForceGunVtableRva) return coop_bot::Gun::ForceGun;
    if (vtable == base + kContactBeamVtableRva) return coop_bot::Gun::ContactBeam;
    return coop_bot::Gun::Other;
}
__declspec(noinline) int SpareRaw(uint32_t player, uint32_t type) {
    __try {
        return reinterpret_cast<int(__thiscall*)(void*, uint32_t)>(base + kCountSpareAmmoRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(player)), type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
__declspec(noinline) bool WishRaw(uint32_t player, uint32_t gun) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kSetDesiredItemRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(player)), gun);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint32_t ActiveGun() {
    uint32_t ref = 0;
    return Read(u->entity + kActiveItem, &ref, 4) && ref > 0x10010 ? ref - 0x10 : 0;
}
void ReadGuns(coop_bot::Perception& p) {
    for (uint32_t& g : u->slotGun) g = 0;
    uint32_t entries = 0, size = 0;
    if (!gunsReady || !Read(u->entity + kWeaponInventory + kInventoryEntries, &entries, 4) || !entries ||
        !Read(u->entity + kWeaponInventory + kInventorySize, &size, 4)) return;
    p.gunsKnown = true;
    const uint32_t active = ActiveGun();
    for (uint32_t i = 0; i < size && i < static_cast<uint32_t>(coop_bot::kGunSlots); ++i) {
        uint32_t ref = 0;
        if (!Read(entries + i * kInventoryEntry + 4, &ref, 4) || ref < 0x10010) continue;
        const uint32_t gun = ref - 0x10;
        coop_bot::GunSlot& slot = p.guns[i];
        slot.kind = GunKind(gun);
        if (slot.kind == coop_bot::Gun::None) continue;
        if (slot.kind == coop_bot::Gun::ForceGun && !firingMatrixHook) slot.kind = coop_bot::Gun::Other;   // its cone would start at the host's camera
        u->slotGun[i] = gun;
        int clip = -1;
        if (Read(gun + kGunClip, &clip, 4) && (clip < 0 || clip > 1000)) clip = -1;
        slot.clip = clip;
        uint32_t attributes = 0, type = 0;
        if (Read(gun + kGunAttributes, &attributes, 4) && attributes && Read(attributes + kGunAmmoType, &type, 4)) {
            const int spare = SpareRaw(u->entity, type);
            if (spare < 0) { if (InterlockedIncrement(&gunFaults) <= 5) gate::Event("bot: counting spare ammo faulted"); }
            else slot.reserve = spare;
        }
        if (gun == active) {
            p.activeSlot = static_cast<int>(i);
            p.clip = slot.clip;
            p.reserve = slot.reserve;
            if (slot.kind == coop_bot::Gun::SeekerRifle) {
                unsigned char zoom = 0;
                if (Read(gun + kSeekerZoom, &zoom, 1) && zoom <= 1) p.seekerZoomed = zoom;
            }
            if (slot.kind == coop_bot::Gun::ContactBeam) {
                float need = 0.0f, charge = 0.0f;
                if (Read(gun + kContactNeed, &need, 4) && Read(gun + kContactCharge, &charge, 4) && std::isfinite(need) && std::isfinite(charge) &&
                    need > 0.0f && need < 10.0f)
                    p.contactReady = charge >= need ? 1 : 0;
            }
            if (slot.kind == coop_bot::Gun::Ripper) {
                uint32_t held = 0;
                p.ripperBladeOut = Read(gun + kRipperHeld, &held, 4) && held;
            }
            if (slot.kind == coop_bot::Gun::PlasmaCutter) {
                unsigned char target = 0;
                float blend = 0.0f;
                if (Read(gun + kCutterTarget, &target, 1) && target <= 1 && Read(gun + kCutterBlend, &blend, 4) && std::isfinite(blend)) {
                    p.cutterHorizontal = target;
                    p.cutterTurning = std::fabs(blend - static_cast<float>(target)) > 0.05f;
                }
            }
        }
        if (slot.kind == coop_bot::Gun::Javelin) {
            // The newest spear, once it sticks in someone: the moment the shock pays.
            uint32_t spearRef = 0, stuck = 0;
            const uint32_t spear = Read(gun + kJavelinSpear, &spearRef, 4) && spearRef > 0x10010 ? spearRef - 0x10 : 0;
            if (spear && spear != u->lastSpear && Read(spear + kSpearStuckIn, &stuck, 4) && stuck) {
                u->lastSpear = spear;
                u->spearHitAt = GetTickCount();
            }
        }
        if (slot.kind == coop_bot::Gun::LineGun) {
            float fuse = 0.0f;
            if (Read(gun + kMineDuration, &fuse, 4) && std::isfinite(fuse) && fuse >= 0.5f && fuse <= 10.0f)
                p.mineFuseMs = static_cast<uint32_t>(fuse * 1000.0f);
        }
    }
    uint32_t wish = 0;
    p.switching = Read(u->entity + kDesiredItem, &wish, 4) && wish && wish != active + 0x10;
    p.spearHitAt = u->spearHitAt;
    uint32_t mines = 0;
    if (Read(base + kDetonatorCountRva, &mines, 4) && mines <= 64) p.detonatorMines = static_cast<int>(mines);
}
bool Aiming() {
    uint32_t flags = 0;
    return Read(u->entity + kBehaviourFlags, &flags, 4) && (flags & kAimingFlag);
}
// Takes the gun in this slot; drops a wish that has not come true in 4 s.
void TakeSlot(int slot, DWORD now) {
    uint32_t wish = 0;
    Read(u->entity + kDesiredItem, &wish, 4);
    const uint32_t active = ActiveGun();
    if (wish && (wish == active + 0x10 || (u->wishSince && now - u->wishSince > 4000))) {
        WishRaw(u->entity, 0);
        if (wish != active + 0x10) InterlockedIncrement(&gunWishesDropped);
        u->wishSince = 0;
        u->wishGun = 0;
        wish = 0;
    }
    if (slot < 0 || slot >= coop_bot::kGunSlots || !gunsReady) return;
    const uint32_t gun = u->slotGun[slot];
    if (!gun || gun == active || gun == u->wishGun) return;
    if (!WishRaw(u->entity, gun)) { if (InterlockedIncrement(&gunFaults) <= 5) gate::Event("bot: asking for a gun faulted"); return; }
    u->wishSince = now;
    u->wishGun = gun;
    InterlockedIncrement(&gunSwitches);
    static const char* const kNames[] = {"none", "plasma cutter", "line gun", "javelin gun", "pulse rifle", "ripper", "detonator", "seeker rifle",
                                         "flamethrower", "force gun", "contact beam", "other gun"};
    if (gunSwitches <= 40) gate::Event("bot: takes the %s in slot %d", kNames[static_cast<unsigned>(GunKind(gun)) % 12], slot);
}

// Defined further down: the kiosk visits take over from the direct purchases.
bool QueueKiosk(uint32_t sm, bool store);
void Act(const Vec3& self, const Vec3& leaderPos, DWORD now);

// Stasis on a player (the human or a bot, from a bot's or the human's shot) is
// cut short ([Bot] PlayerStasisSeconds): the full length on a teammate felt
// like ten seconds. Every stasis goes through StasisManager::StasisEntityInternal
// (PC 0x00BB1B40, thiscall(manager, entity, time factor, duration), ret 0xC;
// found 28.09. from the manager's constructor 0x00B86D90 (singleton
// [0x02021348]) and StasisEntity 0x00BB6D00, which calls it; the Xbox build
// stores the second float as the record's duration). Enemies keep theirs.
constexpr uint32_t kStasisInternalRva = 0x7B1B40;
safetyhook::InlineHook stasisEntityHook;
float playerStasisSeconds = 0.0f;
volatile LONG playerStasisCuts = 0;
bool ApplyKongStasis(uint32_t entity, float factor, float duration) {
    uint32_t manager = 0;
    if (!stasisEntityHook || !Read(base + 0x1C21348, &manager, 4) || !manager) return false;
    stasisEntityHook.thiscall<void>(reinterpret_cast<void*>(manager), entity, factor, duration);
    return true;
}
void __fastcall OnStasisEntity(void* manager, void*, uint32_t entity, float factor, float duration) {
    uint32_t vtable = 0;
    // The stasis carries no shooter: one on a human while a bot's cast is in flight is the bot's.
    const DWORD now = GetTickCount();
    if (noDamageToHumans && lastBotStasisAt && now - lastBotStasisAt < kBotStasisFlightMs && HumanIsaac(entity)) {
        InterlockedIncrement(&humanStasisRefused);
        if (InterlockedIncrement(&humanStasisLines) <= 20)
            gate::Event("bot: stasis (%.2f s) on the human %08X %lu ms after a bot's cast dropped (bots never slow a human)", duration, entity,
                        static_cast<unsigned long>(now - lastBotStasisAt));
        return;
    }
    if (playerStasisSeconds > 0.0f && std::isfinite(duration) && duration > playerStasisSeconds && entity && Read(entity, &vtable, 4) &&
        vtable == base + placement::kPlayerPrimaryVtableRva) {
        if (InterlockedIncrement(&playerStasisCuts) <= 20)
            gate::Event("bot: stasis on the player %08X cut from %.2f to %.2f (factor %.2f)%s", entity, duration, playerStasisSeconds, factor,
                        IsBotEntity(entity) ? " (a bot)" : "");
        duration = playerStasisSeconds;
    }
    if (std::isfinite(duration) && duration > 0.0f) duration *= escalation::StasisFactor(entity);   // boss stasis resistance
    stasisEntityHook.thiscall<void>(manager, entity, factor, duration);
    hits::SendKongStasis(entity, factor, duration);
}

// --- suits --------------------------------------------------------------------------------
// A bot wears a suit the
// leader owns: there is no unlocked list, a suit is owned when it is worn
// ([player+0x1688], an EARS::Modules::Uniform, vtable 0x01B4FCB0, its GUID at
// +0x24) or lies in the safe (player+0xC38, Uniform items). That GUID is what
// ChangeToUniform takes; it is checked against the UniformManager's list first
// (an unregistered one leaves the body bare). Its profile's FavoriteSuits come
// first, then any other owned one, and none that the leader or another bot
// already wears if it can be helped. It dresses when its body first stands, at
// each store visit (a suit the leader got since may be better), and again in a
// new body. While a suit streams in the bot has no Uniform: no med pack then
// (PowerUp::GiftHealth reads it unchecked).
constexpr uint32_t kUniformVtableRva = 0x174FCB0, kWornUniform = 0x1688, kUniformGuid = 0x24, kSafeInventory = 0xC38;
constexpr DWORD kSuitStreamMs = 12000;   // the body check after a change waits this long for the suit's stream
struct SuitInfo { const char* name; uint32_t guid[4]; };
// The suits a player can own in the campaign (the English store names are unique among them).
static const SuitInfo kSuits[] = {
    {"Arctic Security Suit", {0x38CB666F, 0xE989A6F7, 0x54524542, 0x314D4152}},
    {"Riot Security Suit", {0x38CB666F, 0xF99BFFA1, 0x54524542, 0x314D4152}},
    {"Soldier Suit", {0x38CB666F, 0xF2B9B9FB, 0x54524542, 0x314D4152}},
    {"Patrol Suit", {0x31CB7561, 0x1B516549, 0x534F5254, 0x30314E49}},
    {"Zealot Suit", {0x58CB5F60, 0x4BF6F5A0, 0x574F4843, 0x39323031}},
    {"Shockpoint Suit", {0x38CB63F1, 0xFA62DA20, 0x54524542, 0x314D4152}},
    {"Bloody Vintage Suit", {0x38CB63F2, 0x5D6D2A6A, 0x54524542, 0x314D4152}},
    {"Forged Engineering Suit", {0x38CB63F2, 0x508D4590, 0x54524542, 0x314D4152}},
    {"EarthGov Security Suit", {0x38CB63F2, 0x42052D6A, 0x54524542, 0x314D4152}},
    {"Heavy Duty Vintage Suit", {0x38CB63F2, 0x1C0C50E0, 0x54524542, 0x314D4152}},
    {"Hazard Suit", {0x38CB63F1, 0xDECD957E, 0x54524542, 0x314D4152}},
    {"Agility Advanced Suit", {0x38CB63F2, 0x2BE46F24, 0x54524542, 0x314D4152}},
    {"Triage Suit", {0x38CB63F1, 0xED1EDDAA, 0x54524542, 0x314D4152}},
    {"Advanced Suit", {0x39CAC0A5, 0x686ED487, 0x4E415548, 0x38343147}},
    {"Patient Suit", {0x30C9FF4E, 0x44D00498, 0x54544950, 0x32303153}},
    {"Engineering Suit", {0x30C9B946, 0xE243D95F, 0x54544950, 0x32303153}},
    {"Security Suit", {0x39CA8FDF, 0x97CF9234, 0x574F5242, 0x3130314E}},
    {"Vintage Suit", {0x39CB3355, 0x740F0370, 0x4E415548, 0x38343147}},
    {"Elite Engineering Suit", {0x38CB37E4, 0xC671D1BE, 0x54524542, 0x314D4152}},
    {"Elite Security Suit", {0x38CB37E6, 0x424A40DE, 0x54524542, 0x314D4152}},
    {"Elite Vintage Suit", {0x38CB37E7, 0x2058CA16, 0x54524542, 0x314D4152}},
    {"Elite Advanced Suit", {0x38CB37E7, 0xAD1B5F9C, 0x54524542, 0x314D4152}},
    {"Hacker Suit", {0x58CB43ED, 0xEDE44FA8, 0x4E4F574B, 0x35373230}},
};
constexpr int kSuitKinds = static_cast<int>(sizeof(kSuits) / sizeof(kSuits[0]));
const uint32_t* BirthSuitGuid() {
    const int idx = u->suitForced >= 0 ? u->suitForced : u->suitChosen;
    return ownSuits && idx >= 0 && idx < kSuitKinds ? kSuits[idx].guid : nullptr;
}
volatile LONG suitPicks = 0, suitStoreChanges = 0, suitNoneOwned = 0, healsHeldBack = 0;

// "Vintage", "vintage suit" and "Vintage Suit" all name the Vintage Suit.
int SuitByName(const char* raw) {
    char name[64];
    size_t n = 0;
    while (*raw == ' ' || *raw == '\t') ++raw;
    for (; *raw && n + 1 < sizeof(name); ++raw) name[n++] = *raw;
    while (n && (name[n - 1] == ' ' || name[n - 1] == '\t')) --n;
    name[n] = 0;
    if (!n) return -1;
    for (int i = 0; i < kSuitKinds; ++i) {
        if (_stricmp(name, kSuits[i].name) == 0) return i;
        char withSuit[72];
        std::snprintf(withSuit, sizeof(withSuit), "%s Suit", name);
        if (_stricmp(withSuit, kSuits[i].name) == 0) return i;
    }
    return -1;
}
int SuitOfGuid(const uint32_t* guid) {
    for (int i = 0; i < kSuitKinds; ++i) if (std::memcmp(kSuits[i].guid, guid, 16) == 0) return i;
    return -1;
}
// The suit a Player wears (false while a change streams in: no Uniform then).
bool WornSuit(uint32_t player, uint32_t* guid) {
    uint32_t uniform = 0, vtable = 0;
    return player && Read(player + kWornUniform, &uniform, 4) && uniform && Read(uniform, &vtable, 4) && vtable == base + kUniformVtableRva &&
           Read(uniform + kUniformGuid, guid, 16);
}
const char* SuitName(int idx) { return idx >= 0 && idx < kSuitKinds ? kSuits[idx].name : "?"; }
int WornSuitIndex(uint32_t player) {
    uint32_t guid[4]{};
    return WornSuit(player, guid) ? SuitOfGuid(guid) : -1;
}
void SnapIsaac(Unit& unit, uint32_t leader) {
    coop_uniform::BotBody(leader, unit.isaacBody);
    unit.isaacSuit = WornSuitIndex(leader);
}
// What the leader owns, as indices into kSuits: worn plus the Uniforms in the safe, registered only.
unsigned OwnedSuits(uint32_t leader, int* out, unsigned cap) {
    unsigned n = 0;
    auto add = [&](const uint32_t* guid) {
        const int idx = SuitOfGuid(guid);
        if (idx < 0 || coop_uniform::SuitIndex(guid) < 0 || n >= cap) return;
        for (unsigned k = 0; k < n; ++k) if (out[k] == idx) return;
        out[n++] = idx;
    };
    uint32_t guid[4]{};
    if (WornSuit(leader, guid)) add(guid);
    uint32_t entries = 0, size = 0;
    if (!Read(leader + kSafeInventory + kInventoryEntries, &entries, 4) || !entries || !Read(leader + kSafeInventory + kInventorySize, &size, 4)) return n;
    for (uint32_t i = 0; i < size && i < 256; ++i) {
        uint32_t ref = 0, vtable = 0;
        if (Read(entries + i * kInventoryEntry + 4, &ref, 4) && ref > 0x10010 && Read(ref - 0x10, &vtable, 4) && vtable == base + kUniformVtableRva &&
            Read(ref - 0x10 + kUniformGuid, guid, 16))
            add(guid);
    }
    return n;
}
// The suit for this bot now, or -1: favourites the leader owns and nobody else
// wears, then any such suit (the same pick for the same bot every time), then a
// favourite someone else wears too, then any owned one even if taken (29.09.:
// with only the leader's Engineering Suit owned, a bot without it among its
// favourites stayed in the patient suit). The patient suit only when it is all there is.
int PatientSuit() {
    static int idx = -2;
    if (idx == -2) idx = SuitByName("Patient Suit");
    return idx;
}
int PickSuit(uint32_t leader, char* ownedText, size_t ownedTextSize) {
    if (u->suitForced >= 0) return u->suitForced;   // the player's choice, owned or not
    int owned[kSuitKinds];
    const unsigned n = OwnedSuits(leader, owned, kSuitKinds);
    if (ownedText && ownedTextSize) {
        size_t used = 0;
        ownedText[0] = 0;
        for (unsigned k = 0; k < n && used + 2 < ownedTextSize; ++k) {
            const int wrote = std::snprintf(ownedText + used, ownedTextSize - used, "%s%s", k ? ", " : "", kSuits[owned[k]].name);
            if (wrote < 0) break;
            used += static_cast<size_t>(wrote);
        }
    }
    if (!n) return -1;
    auto takenBy = [&](int idx) {
        uint32_t guid[4]{};
        if (WornSuit(leader, guid) && SuitOfGuid(guid) == idx) return true;
        for (const Unit& other : units) {
            if (&other == u || !other.entity) continue;
            if (other.suitChosen == idx || (WornSuit(other.entity, guid) && SuitOfGuid(guid) == idx)) return true;
        }
        return false;
    };
    auto isOwned = [&](int idx) {
        for (unsigned k = 0; k < n; ++k) if (owned[k] == idx) return true;
        return false;
    };
    const int patient = PatientSuit();
    for (int k = 0; k < u->suitFavoriteCount; ++k)
        if (u->suitFavorites[k] != patient && isOwned(u->suitFavorites[k]) && !takenBy(u->suitFavorites[k])) return u->suitFavorites[k];
    for (unsigned k = 0; k < n; ++k) {
        const int idx = owned[(k + u->index) % n];
        if (idx != patient && !takenBy(idx)) return idx;
    }
    for (int k = 0; k < u->suitFavoriteCount; ++k)
        if (u->suitFavorites[k] != patient && isOwned(u->suitFavorites[k])) return u->suitFavorites[k];
    for (unsigned k = 0; k < n; ++k)
        if (owned[(k + u->index) % n] != patient) return owned[(k + u->index) % n];
    return -1;
}
// Out of the patient suit at once, as soon as the leader has another one (the
// game makes him change early on; 29.09.: the third bot waited for its turn at
// the store for that). Other changes wait for a store visit.
volatile LONG suitUrgentChanges = 0;
bool DressIn(uint32_t leader, int idx, const char* why, DWORD now);   // below
void UrgentSuit(uint32_t leader, DWORD now) {
    if (!ownSuits || !u->entity || u->dressCheckAt || now - u->suitUrgentAt < 1500) return;
    u->suitUrgentAt = now;
    if (u->kioskVisit != KioskStep::None || u->command.mode == coop_bot::Mode::Combat || u->tkStep != TkStep::None) return;
    uint32_t worn[4]{};
    if (!WornSuit(u->entity, worn) || SuitOfGuid(worn) != PatientSuit()) return;
    const int idx = PickSuit(leader, nullptr, 0);
    if (idx < 0 || idx == PatientSuit()) return;
    if (DressIn(leader, idx, "out of the patient suit at once", now)) InterlockedIncrement(&suitUrgentChanges);
}
// Into suit idx, unless it wears it already. The body check waits for the stream.
bool DressIn(uint32_t leader, int idx, const char* why, DWORD now) {
    uint32_t worn[4]{};
    u->suitChosen = idx;
    if (WornSuit(u->entity, worn) && std::memcmp(worn, kSuits[idx].guid, 16) == 0) {
        gate::Event("bot %u (%s): keeps the %s (%s)", u->index + 1, u->name, kSuits[idx].name, why);
        return false;
    }
    if (coop_uniform::SuitIndex(kSuits[idx].guid) < 0) {
        InterlockedIncrement(&suitFailures);
        gate::Event("bot %u (%s): the %s is not registered here; stays as it is", u->index + 1, u->name, kSuits[idx].name);
        return false;
    }
    SnapIsaac(*u, leader);   // Isaac's body as it is now, for the check after
    u->isaacWaits = 0;
    const bool dressed = coop_uniform::BotDress(u->entity, kSuits[idx].guid);
    if (dressed) InterlockedIncrement(&suitChanges); else InterlockedIncrement(&suitFailures);
    gate::Event("bot %u (%s): changes into the %s (%s), call %s", u->index + 1, u->name, kSuits[idx].name, why, dressed ? "ok" : "faulted");
    u->dressCheckAt = now + kSuitStreamMs;
    u->dressTries = 0;
    return dressed;
}
// A new body: its own suit back, or its first pick.
void PutOnOwnSuit(uint32_t leader, DWORD now) {
    if (!u->suitAt || now < u->suitAt || u->suitDone) return;
    u->suitDone = true;
    u->suitAt = 0;
    if (u->suitForced >= 0) u->suitChosen = u->suitForced;
    if (u->suitChosen >= 0) { DressIn(leader, u->suitChosen, "its own suit again", now); return; }
    char owned[256];
    const int idx = PickSuit(leader, owned, sizeof(owned));
    InterlockedIncrement(&suitPicks);
    if (idx < 0) {
        InterlockedIncrement(&suitNoneOwned);
        gate::Event("bot %u (%s): the leader owns no suit it could wear (owned: %s); keeps the one it has", u->index + 1, u->name, owned[0] ? owned : "none");
        return;
    }
    gate::Event("bot %u (%s): the leader owns: %s", u->index + 1, u->name, owned[0] ? owned : "none");
    DressIn(leader, idx, "its first pick", now);
}
// At a store visit: a better suit, if the leader got one since.
void StoreSuit(uint32_t leader, DWORD now) {
    if (!ownSuits || !u->entity || u->dressCheckAt) return;
    uint32_t worn[4]{};
    if (!WornSuit(u->entity, worn)) return;   // a change is still streaming in
    char owned[256];
    const int idx = PickSuit(leader, owned, sizeof(owned));
    InterlockedIncrement(&suitPicks);
    if (idx < 0 || std::memcmp(worn, kSuits[idx].guid, 16) == 0) { if (idx >= 0) u->suitChosen = idx; return; }
    gate::Event("bot %u (%s): at the store, the leader owns: %s", u->index + 1, u->name, owned[0] ? owned : "none");
    if (DressIn(leader, idx, "picked at the store", now)) InterlockedIncrement(&suitStoreChanges);
}

// --- the bench ------------------------------------------------------------------------
// The upgrade trees are global, one
// per item kind: UpgradeManager [0x02021438], the set inline at +0x18, tree
// pointers from set+0, their count at set+0xC8; a tree's key (the item's name
// hash) at tree+0, node pointers from tree+0x04, count at tree+0x170. A node:
// key +0, neighbour keys +0x0C..+0x18, filled +0x2C (0/1), start point byte +0x34;
// its kind comes from UpgradeStruct::GetNodeType (PC 0x00B76B80, thiscall, ret),
// which also honours the bench randomizer. Power nodes are per Player at +0x42C;
// a node costs one. A gun reads its tree through the upgrade id at
// [[gun+0xC]+0x2E8] in Gun::CalcUpgradedValues (vtable +0x284, thiscall, ret).
// The bench's own state machine applies values when it closes
// (~PlayerUpgradeMainSM, PC 0x008A5620, thiscall, ret; owner Player at +0x50):
// right after the leader leaves a bench, each bot spends its nodes on the trees
// of the guns it carries, and the guns of that kind are recalculated. The trees
// being shared, the leader's guns of that kind gain as well.
constexpr uint32_t kUpgradeManagerRva = 0x1C21438, kUpgradeSet = 0x18, kTreeCount = 0xC8, kTreeNodes = 0x04, kTreeNodeCount = 0x170;
constexpr uint32_t kNodeLinks = 0x0C, kNodeFill = 0x2C, kNodeStart = 0x34, kPlayerNodes = 0x42C, kGunUpgradeId = 0x2E8, kGunRecalcSlot = 0x284;
constexpr uint32_t kNodeTypeRva = 0x776B80, kBenchCloseRva = 0x4A5620, kBenchOwner = 0x50;
bool benchWanted = false, benchReady = false;
safetyhook::InlineHook benchCloseHook;
DWORD benchClosedAt = 0;
volatile LONG benchVisits = 0, nodesBought = 0, nodesPicked = 0, benchFaults = 0, gunRecalcs = 0;
void __fastcall OnBenchClose(void* sm, void*) {
    uint32_t owner = 0;
    Read(reinterpret_cast<uintptr_t>(sm) + kBenchOwner, &owner, 4);
    const bool queued = owner && !IsBotEntity(owner) && QueueKiosk(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sm)), false);
    benchCloseHook.thiscall<void>(sm);
    if (owner && !IsBotEntity(owner) && !queued) benchClosedAt = GetTickCount();
}
__declspec(noinline) int NodeTypeRaw(uint32_t node) {
    __try {
        return reinterpret_cast<int(__thiscall*)(void*)>(base + kNodeTypeRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(node)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
__declspec(noinline) bool RecalcRaw(uint32_t gun, uint32_t fn) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(gun)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint32_t TreeFor(uint32_t id) {
    uint32_t manager = 0, count = 0;
    if (!id || !Read(base + kUpgradeManagerRva, &manager, 4) || !manager || !Read(manager + kUpgradeSet + kTreeCount, &count, 4) || count > 64) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t tree = 0, key = 0;
        if (Read(manager + kUpgradeSet + i * 4, &tree, 4) && tree && Read(tree, &key, 4) && key == id) return tree;
    }
    return 0;
}
uint32_t GunUpgradeId(uint32_t gun) {
    uint32_t attributes = 0, id = 0;
    return Read(gun + kGunAttributes, &attributes, 4) && attributes && Read(attributes + kGunUpgradeId, &id, 4) ? id : 0;
}
// The guns in a Player's weapon slots.
unsigned GunsOf(uint32_t player, uint32_t* out, unsigned cap) {
    uint32_t entries = 0, size = 0;
    unsigned n = 0;
    if (!Read(player + kWeaponInventory + kInventoryEntries, &entries, 4) || !entries || !Read(player + kWeaponInventory + kInventorySize, &size, 4)) return 0;
    for (uint32_t i = 0; i < size && i < 8 && n < cap; ++i) {
        uint32_t ref = 0;
        if (Read(entries + i * kInventoryEntry + 4, &ref, 4) && ref > 0x10010 && GunKind(ref - 0x10) != coop_bot::Gun::None) out[n++] = ref - 0x10;
    }
    return n;
}
// One tree as the picker sees it; node pointers alongside.
int ReadTree(uint32_t tree, coop_bot::BenchNode* nodes, uint32_t* where) {
    uint32_t count = 0;
    if (!Read(tree + kTreeNodeCount, &count, 4) || count > static_cast<uint32_t>(coop_bot::kMaxBenchNodes)) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t node = 0, fill = 0;
        unsigned char start = 0;
        coop_bot::BenchNode& n = nodes[i];
        n = coop_bot::BenchNode{};
        if (!Read(tree + kTreeNodes + i * 4, &node, 4) || !node || !Read(node, &n.key, 4) || !Read(node + kNodeLinks, n.links, sizeof(n.links)) ||
            !Read(node + kNodeFill, &fill, 4) || !Read(node + kNodeStart, &start, 1))
            return -1;
        n.filled = fill != 0;
        n.start = start != 0;
        n.type = NodeTypeRaw(node);
        if (n.type < 0) { InterlockedIncrement(&benchFaults); return -1; }
        where[i] = node;
    }
    return static_cast<int>(count);
}
// Recalculates every gun of these trees that a Player (the leader or a bot) holds.
void RecalcGuns(uint32_t player, const uint32_t* trees, unsigned treeCount) {
    uint32_t guns[8];
    const unsigned n = GunsOf(player, guns, 8);
    for (unsigned g = 0; g < n; ++g) {
        const uint32_t tree = TreeFor(GunUpgradeId(guns[g]));
        bool touched = false;
        for (unsigned t = 0; t < treeCount; ++t) touched = touched || trees[t] == tree;
        uint32_t vtable = 0, fn = 0;
        if (!touched || !Read(guns[g], &vtable, 4) || !Read(vtable + kGunRecalcSlot, &fn, 4) || fn < base + 0x1000 || fn >= base + gate::kRdataLo) continue;
        if (RecalcRaw(guns[g], fn)) InterlockedIncrement(&gunRecalcs);
        else InterlockedIncrement(&benchFaults);
    }
}
// The bot's own power nodes. A new body carries the bot's count, whatever
// Player::Init put there; nodes it picks up are added.
void TrackNodes() {
    if (!benchReady) return;
    int32_t have = 0;
    if (!Read(u->entity + kPlayerNodes, &have, 4) || have < 0 || have > 100000) return;
    if (!u->nodesSet) {
        if (have != u->nodeWallet) gate::Event("bot %u: power nodes %d -> %d (its own count)", u->index + 1, have, u->nodeWallet);
        WriteBytes(u->entity + kPlayerNodes, &u->nodeWallet, 4);
        u->nodesSeen = u->nodeWallet;
        u->nodesSet = true;
        return;
    }
    if (have > u->nodesSeen) {
        InterlockedExchangeAdd(&nodesPicked, have - u->nodesSeen);
        gate::Event("bot %u: picked up %d power node(s), %d now", u->index + 1, have - u->nodesSeen, have);
    }
    u->nodeWallet = have;
    u->nodesSeen = have;
}
// Spends the bot's nodes, a node at a time, round the trees of its guns.
void Invest(uint32_t leader) {
    if (!benchReady || !u->entity || !u->nodesSet || u->nodeWallet <= 0) return;
    uint32_t guns[8], trees[8];
    unsigned treeCount = 0;
    const unsigned gunCount = GunsOf(u->entity, guns, 8);
    for (unsigned g = 0; g < gunCount; ++g) {
        const uint32_t tree = TreeFor(GunUpgradeId(guns[g]));
        bool known = !tree;
        for (unsigned t = 0; t < treeCount; ++t) known = known || trees[t] == tree;
        if (!known) trees[treeCount++] = tree;
    }
    uint32_t touched[8];
    unsigned touchedCount = 0, open = treeCount, turn = 0;
    bool done[8]{};
    int bought = 0;
    while (u->nodeWallet > 0 && open > 0 && bought < 100) {
        const unsigned t = turn++ % treeCount;
        if (done[t]) continue;
        coop_bot::BenchNode nodes[coop_bot::kMaxBenchNodes];
        uint32_t where[coop_bot::kMaxBenchNodes]{};
        const int count = ReadTree(trees[t], nodes, where);
        const int pick = count > 0 ? coop_bot::PickBenchNode(nodes, count) : -1;
        if (pick < 0) { done[t] = true; --open; continue; }
        const uint32_t filled = 1;
        if (!WriteBytes(where[pick] + kNodeFill, &filled, 4)) { InterlockedIncrement(&benchFaults); done[t] = true; --open; continue; }
        --u->nodeWallet;
        WriteBytes(u->entity + kPlayerNodes, &u->nodeWallet, 4);
        u->nodesSeen = u->nodeWallet;
        ++bought;
        InterlockedIncrement(&nodesBought);
        bool listed = false;
        for (unsigned k = 0; k < touchedCount; ++k) listed = listed || touched[k] == trees[t];
        if (!listed) touched[touchedCount++] = trees[t];
        uint32_t treeKey = 0;
        Read(trees[t], &treeKey, 4);
        if (nodesBought <= 200)
            gate::Event("bot %u: bench: tree %08X node %08X (type %d) filled, %d node(s) left", u->index + 1, treeKey, nodes[pick].key, nodes[pick].type,
                        u->nodeWallet);
    }
    if (!touchedCount) return;
    RecalcGuns(u->entity, touched, touchedCount);
    if (leader) RecalcGuns(leader, touched, touchedCount);   // the leader's guns of that kind gain too
    gate::Event("bot %u: bench: %d node(s) spent on %u tree(s), %d left", u->index + 1, bought, touchedCount, u->nodeWallet);
}
bool InstallBench(FILE* log) {
    if (!benchWanted) return false;
    // push ecx / push ebx / push ebp / push esi / mov esi,ecx / mov eax,[esi+50h] / push edi / movzx edi,byte [esi+58h] / lea ecx,[eax+440h]
    static const unsigned char kClose[] = {0x51, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x50, 0x57, 0x0F, 0xB6, 0x7E, 0x58, 0x8D, 0x88, 0x40, 0x04, 0x00, 0x00};
    // mov eax,[ecx] / push eax / call rel32 / add esp,4 / ret
    static const unsigned char kType[] = {0x8B, 0x01, 0x50, 0xE8};
    // mov eax,[ecx+42Ch] / ret (Player::GetUpgradeNodes)
    static const unsigned char kNodes[] = {0x8B, 0x81, 0x2C, 0x04, 0x00, 0x00, 0xC3};
    unsigned char typeSite[sizeof(kType)]{}, nodeSite[sizeof(kNodes)]{};
    uint32_t playerVtable = static_cast<uint32_t>(base + placement::kPlayerPrimaryVtableRva), getter = 0;
    const bool type = Read(base + kNodeTypeRva, typeSite, sizeof(typeSite)) && std::memcmp(typeSite, kType, sizeof(kType)) == 0;
    const bool nodes = Read(playerVtable + 0x248, &getter, 4) && Read(getter, nodeSite, sizeof(nodeSite)) && std::memcmp(nodeSite, kNodes, sizeof(kNodes)) == 0;
    const bool close = type && nodes &&
                       hook_site::Install(log, "bot PlayerUpgradeMainSM destructor", base + kBenchCloseRva, kClose, sizeof(kClose),
                                          reinterpret_cast<void*>(OnBenchClose), benchCloseHook);
    benchReady = type && nodes && close;
    std::fprintf(log, "bot bench=%d (node_type=%d node_count=%d close_hook=%d: bots spend their own power nodes when the leader leaves a bench)\n",
                 benchReady ? 1 : 0, type ? 1 : 0, nodes ? 1 : 0, close ? 1 : 0);
    return benchReady;
}

// --- the store ------------------------------------------------------------------------
// Credits are per Player in the
// general inventory (Player+0xBD0, its credits at +0x28 = Player+0xBF8); a
// credits pickup goes to whoever picks it up. The store's own purchase, without
// its UI: PickupItem::GetGlobalPickupItem(guid*, false) (PC 0x00DE7560, cdecl)
// spawns the item, its Pickup(owner, false) (vtable +0x1F8, thiscall, ret 8)
// adds it to the owner's inventory (true when all of it fit), then
// Inventory::SubtractCurrency (0x00DCDB70, thiscall, ret 4). The price is the
// unit price [[item+0xC]+0x2E4] times the amount (+0x260), less the suit's
// discount [[[player+0x1688]+0xC]+0x3C8]. Ammo for a gun: the Quartermaster
// ([0x0201C0D0]) entry of the gun's proxy GUID (gun+0x404) via
// FindGunEntryByProxyGUID (0x00DB28D0, thiscall, ret 8), its drop GUID at +0x10.
// Med packs are PowerUps with bonus health (attrs+0x334 > 0); their GUID
// (attrs+0x138) is learned from the med packs the leader or a bot carries.
// Right after the leader leaves a store (~PlayerStoreSM, PC 0x00ED3CE0, found by
// its vtable 0x01B5E360: thiscall, owner at +0x50), each bot buys med packs and
// ammo for its guns with its own credits.
constexpr uint32_t kGeneralInventory = 0xBD0, kInventoryCredits = 0x28, kItemAmount = 0x260, kItemPrice = 0x2E4, kItemSharedGuid = 0x138;
constexpr uint32_t kPlayerUniform = 0x1688, kUniformDiscount = 0x3C8, kGunProxyGuid = 0x404, kGunCapacity = 0x3E4, kBonusHealth = 0x334;
constexpr uint32_t kQuartermasterRva = 0x1C1C0D0, kFindGunEntryRva = 0x9B28D0, kGunEntryDrop = 0x10;
constexpr uint32_t kGlobalPickupRva = 0x9E7560, kCanFitRva = 0x9CDD20, kSubtractCurrencyRva = 0x9CDB70, kBaseDeleteRva = 0x3568B0;
constexpr uint32_t kPickupSlot = 0x1F8, kPowerUpVtableRva = 0x174F1B8, kStoreCloseRva = 0xAD3CE0, kStoreOwner = 0x50;
constexpr int kWantMedPacks = 2, kAmmoClips = 3, kBuysPerGun = 12;
bool storeWanted = false, storeReady = false;
safetyhook::InlineHook storeCloseHook;
DWORD storeClosedAt = 0;
volatile LONG storeVisits = 0, purchases = 0, creditsSpent = 0, creditsPicked = 0, storeFaults = 0;
struct MedPackKind { unsigned char guid[16]; uint32_t price = 0; bool known = false; };
MedPackKind medPack;
void __fastcall OnStoreClose(void* sm, void*) {
    uint32_t owner = 0;
    Read(reinterpret_cast<uintptr_t>(sm) + kStoreOwner, &owner, 4);
    const bool queued = owner && !IsBotEntity(owner) && QueueKiosk(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sm)), true);
    storeCloseHook.thiscall<void>(sm);
    if (owner && !IsBotEntity(owner) && !queued) storeClosedAt = GetTickCount();
}
__declspec(noinline) uint32_t SpawnPickupGuarded(const unsigned char* guid) {
    __try {
        return reinterpret_cast<uint32_t(__cdecl*)(const void*, int)>(base + kGlobalPickupRva)(guid, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&mpquiet::faults);
        return 0;
    }
}
uint32_t SpawnPickupRaw(const unsigned char* guid) {
    mpquiet::Scope quiet;
    return SpawnPickupGuarded(guid);
}
__declspec(noinline) int CanFitRaw(const unsigned char* guid, uint32_t inventory) {
    __try {
        return reinterpret_cast<bool(__cdecl*)(const void*, uint32_t)>(base + kCanFitRva)(guid, inventory) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
__declspec(noinline) int PickupGuarded(uint32_t item, uint32_t fn, uint32_t owner) {
    __try {
        return reinterpret_cast<bool(__thiscall*)(void*, uint32_t, int)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(item)), owner, 0) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&mpquiet::faults);
        return -1;
    }
}
int PickupRaw(uint32_t item, uint32_t fn, uint32_t owner) {
    mpquiet::Scope quiet;
    return PickupGuarded(item, fn, owner);
}
__declspec(noinline) bool DeleteGuarded(uint32_t object) {
    __try {
        reinterpret_cast<void(__cdecl*)(uint32_t)>(base + kBaseDeleteRva)(object);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&mpquiet::faults);
        return false;
    }
}
bool DeleteRaw(uint32_t object) {
    mpquiet::Scope quiet;
    return DeleteGuarded(object);
}
__declspec(noinline) bool SubtractRaw(uint32_t inventory, uint32_t amount) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kSubtractCurrencyRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(inventory)), amount);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) uint32_t GunEntryRaw(uint32_t qm, const unsigned char* guid) {
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*, const void*, uint32_t*)>(base + kFindGunEntryRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(qm)), guid, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// The items of a Player's general inventory.
unsigned ItemsOf(uint32_t player, uint32_t* out, unsigned cap) {
    uint32_t entries = 0, size = 0;
    unsigned n = 0;
    const uint32_t inventory = player + kGeneralInventory;
    if (!Read(inventory + kInventoryEntries, &entries, 4) || !entries || !Read(inventory + kInventorySize, &size, 4) || size > 256) return 0;
    for (uint32_t i = 0; i < size && n < cap; ++i) {
        uint32_t ref = 0;
        if (Read(entries + i * kInventoryEntry + 4, &ref, 4) && ref > 0x10010) out[n++] = ref - 0x10;
    }
    return n;
}
bool IsMedPack(uint32_t item, uint32_t& attributes) {
    uint32_t vtable = 0;
    float health = 0.0f;
    return Read(item, &vtable, 4) && vtable == base + kPowerUpVtableRva && Read(item + kGunAttributes, &attributes, 4) && attributes &&
           Read(attributes + kBonusHealth, &health, 4) && std::isfinite(health) && health > 0.0f;
}
int MedPacks(uint32_t player) {
    uint32_t items[64];
    const unsigned n = ItemsOf(player, items, 64);
    int count = 0;
    for (unsigned i = 0; i < n; ++i) {
        uint32_t attributes = 0, amount = 0;
        if (IsMedPack(items[i], attributes) && Read(items[i] + kItemAmount, &amount, 4) && amount < 100) count += static_cast<int>(amount);
    }
    return count;
}
// The cheapest med pack anyone carries is the one the bots buy.
void LearnMedPacks(uint32_t player) {
    uint32_t items[64];
    const unsigned n = ItemsOf(player, items, 64);
    for (unsigned i = 0; i < n; ++i) {
        uint32_t attributes = 0, price = 0;
        unsigned char guid[16]{};
        if (!IsMedPack(items[i], attributes) || !Read(attributes + kItemPrice, &price, 4) || !price || price > 1000000 ||
            !Read(attributes + kItemSharedGuid, guid, 16))
            continue;
        if (medPack.known && price >= medPack.price) continue;
        std::memcpy(medPack.guid, guid, 16);
        medPack.price = price;
        medPack.known = true;
        gate::Event("bot: med pack kind learned from %08X: price %u", player, price);
    }
}
// One purchase the store's way. True when something was bought.
bool Buy(const unsigned char* guid, const char* what) {
    const uint32_t inventory = u->entity + kGeneralInventory;
    const int fits = CanFitRaw(guid, inventory);
    if (fits <= 0) { if (fits < 0) InterlockedIncrement(&storeFaults); return false; }
    const uint32_t item = SpawnPickupRaw(guid);
    if (!item) { InterlockedIncrement(&storeFaults); return false; }
    uint32_t amount = 0, attributes = 0, unit = 0, credits = 0, suit = 0, suitAttributes = 0, vtable = 0, fn = 0;
    float discount = 0.0f;
    if (!Read(item + kItemAmount, &amount, 4) || !Read(item + kGunAttributes, &attributes, 4) || !attributes || !Read(attributes + kItemPrice, &unit, 4) ||
        !Read(u->entity + kGeneralInventory + kInventoryCredits, &credits, 4) || !Read(item, &vtable, 4) || !Read(vtable + kPickupSlot, &fn, 4) ||
        fn < base + 0x1000 || fn >= base + gate::kRdataLo || !amount || amount > 10000) {
        DeleteRaw(item);
        InterlockedIncrement(&storeFaults);
        return false;
    }
    if (Read(u->entity + kPlayerUniform, &suit, 4) && suit && Read(suit + kGunAttributes, &suitAttributes, 4) && suitAttributes)
        Read(suitAttributes + kUniformDiscount, &discount, 4);
    if (!std::isfinite(discount) || discount < 0.0f || discount > 1.0f) discount = 0.0f;
    const uint32_t full = unit * amount;
    uint32_t price = full - static_cast<uint32_t>(std::ceil(static_cast<double>(full) * discount));
    if (credits < price) { DeleteRaw(item); return false; }
    const int whole = PickupRaw(item, fn, u->entity);
    if (whole < 0) { InterlockedIncrement(&storeFaults); return false; }
    if (!whole) {
        // Not all of it fit: what is left stays with nobody; charge for what went in.
        uint32_t left = amount;
        Read(item + kItemAmount, &left, 4);
        DeleteRaw(item);
        if (left >= amount) return false;
        price = price * (amount - left) / amount;
    }
    if (price && !SubtractRaw(inventory, price)) InterlockedIncrement(&storeFaults);
    InterlockedIncrement(&purchases);
    InterlockedExchangeAdd(&creditsSpent, static_cast<LONG>(price));
    uint32_t after = 0;
    Read(u->entity + kGeneralInventory + kInventoryCredits, &after, 4);
    u->creditWallet = static_cast<int32_t>(after);
    u->creditsSeen = static_cast<int32_t>(after);
    if (purchases <= 200)
        gate::Event("bot %u: store: %s x%u for %u credits (unit %u, discount %.2f, %s), %u -> %u credits", u->index + 1, what, amount, price, unit,
                    discount, whole ? "all in" : "part", credits, after);
    return true;
}
void GiveLeaderShare(int32_t found);   // the weapon economy, below
extern int creditShare;
extern volatile LONG creditsSeeded;
// The bot's own credits, kept across new bodies like its power nodes. A wallet
// that never had anything (a bot new to this game or save) starts with CreditShare
// per cent of the leader's credits: 06.10. the bots began a long session with 0,
// the leader's 23230 never changed (no find to share), so they bought nothing.
void TrackCredits() {
    if (!storeReady) return;
    int32_t have = 0;
    if (!Read(u->entity + kGeneralInventory + kInventoryCredits, &have, 4) || have < 0 || have > coop_bot_save::kMaxCredits) return;
    if (!u->creditsSet) {
        int32_t leaderHas = -1;
        const uint32_t leader = Leader();
        if (u->walletFresh && creditShare > 0 && leader && Read(leader + kGeneralInventory + kInventoryCredits, &leaderHas, 4) && leaderHas >= 0 &&
            leaderHas <= coop_bot_save::kMaxCredits) {
            const int64_t stake = static_cast<int64_t>(leaderHas) * creditShare / 100;
            u->creditWallet += static_cast<int32_t>(stake > coop_bot_save::kMaxCredits ? coop_bot_save::kMaxCredits : stake);
            if (u->creditWallet > coop_bot_save::kMaxCredits) u->creditWallet = coop_bot_save::kMaxCredits;
            u->walletFresh = false;
            InterlockedIncrement(&creditsSeeded);
            gate::Event("bot %u: starting credits %lld (CreditShare %d%% of the leader's %d)", u->index + 1, static_cast<long long>(stake), creditShare, leaderHas);
        }
        if (have != u->creditWallet) gate::Event("bot %u: credits %d -> %d (its own)", u->index + 1, have, u->creditWallet);
        WriteBytes(u->entity + kGeneralInventory + kInventoryCredits, &u->creditWallet, 4);
        u->creditsSeen = u->creditWallet;
        u->creditsSet = true;
        return;
    }
    if (have > u->creditsSeen) {
        InterlockedExchangeAdd(&creditsPicked, have - u->creditsSeen);
        gate::Event("bot %u: picked up %d credits, %d now", u->index + 1, have - u->creditsSeen, have);
        GiveLeaderShare(have - u->creditsSeen);
    }
    u->creditWallet = have;
    u->creditsSeen = have;
}
void ShopGun(uint32_t leader);   // the weapon economy, below
// After the leader's store visit: a gun (the weapon economy), med packs, then ammo for every gun it carries.
void Shop(uint32_t leader) {
    if (!storeReady || !u->entity || !u->creditsSet) return;
    ShopGun(leader);
    LearnMedPacks(u->entity);
    if (leader) LearnMedPacks(leader);
    int bought = 0;
    for (int i = 0; i < kWantMedPacks && medPack.known && MedPacks(u->entity) < kWantMedPacks; ++i)
        if (Buy(medPack.guid, "med pack")) ++bought; else break;
    uint32_t qm = 0;
    Read(base + kQuartermasterRva, &qm, 4);
    uint32_t guns[8];
    const unsigned n = qm ? GunsOf(u->entity, guns, 8) : 0;
    for (unsigned g = 0; g < n; ++g) {
        unsigned char proxy[16]{}, drop[16]{};
        uint32_t attributes = 0, type = 0;
        int32_t capacity = 0;
        const uint32_t entry = Read(guns[g] + kGunProxyGuid, proxy, 16) ? GunEntryRaw(qm, proxy) : 0;
        if (!entry || !Read(entry + kGunEntryDrop, drop, 16) || !Read(guns[g] + kGunAttributes, &attributes, 4) || !attributes ||
            !Read(attributes + kGunAmmoType, &type, 4))
            continue;
        if (!Read(guns[g] + kGunCapacity, &capacity, 4) || capacity <= 0 || capacity > 500) capacity = 20;
        const int want = capacity * kAmmoClips;
        for (int k = 0; k < kBuysPerGun; ++k) {
            const int spare = SpareRaw(u->entity, type);
            if (spare < 0 || spare >= want) break;
            if (Buy(drop, "ammo")) ++bought; else break;
        }
    }
    uint32_t credits = 0;
    Read(u->entity + kGeneralInventory + kInventoryCredits, &credits, 4);
    gate::Event("bot %u: store: %d purchase(s), %u credits left, med packs %d%s", u->index + 1, bought, credits, MedPacks(u->entity),
                medPack.known ? "" : " (no med pack kind known yet)");
}
bool InstallStore(FILE* log) {
    if (!storeWanted) return false;
    // push ecx / push ebx / push ebp / push esi / mov esi,ecx / mov eax,[esi+0AF4h] / shr eax,4 / push edi
    static const unsigned char kClose[] = {0x51, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0xF4, 0x0A, 0x00, 0x00, 0xC1, 0xE8, 0x04, 0x57};
    struct Site { uint32_t rva; unsigned char bytes[16]; unsigned size; const char* name; };
    static const Site kSites[] = {
        {kGlobalPickupRva, {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x28, 0x53, 0x8B, 0x1D}, 10, "GetGlobalPickupItem"},
        {kCanFitRva, {0x83, 0xEC, 0x30, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x44, 0x57, 0x8B, 0xCE, 0xE8}, 14, "CanFitItem"},
        {kSubtractCurrencyRva, {0x8B, 0x41, 0x28, 0x83, 0xEC, 0x20, 0x56, 0x8B, 0x74, 0x24, 0x28, 0x3B, 0xF0, 0x77, 0x07, 0x2B}, 16, "SubtractCurrency"},
        {kBaseDeleteRva, {0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x06, 0x8B, 0x50, 0x08, 0x8B, 0xCE, 0xFF, 0xD2, 0x83, 0x3D}, 16, "Base::Delete"},
        {kFindGunEntryRva, {0x56, 0x33, 0xC0, 0x33, 0xF6, 0x39, 0x41, 0x18, 0x76, 0x49, 0x53, 0x57, 0x8B, 0x7C, 0x24, 0x10}, 16, "FindGunEntryByProxyGUID"},
    };
    bool sites = gunsReady;
    for (const Site& s : kSites) {
        unsigned char actual[16]{};
        // Another plugin's detour at the entry (a 5-byte jmp, 28.09.: Base::Delete) still
        // leads to the same function: the bytes after it have to match.
        const bool read = Read(base + s.rva, actual, s.size);
        const bool ok = read && (std::memcmp(actual, s.bytes, s.size) == 0 || (actual[0] == 0xE9 && std::memcmp(actual + 5, s.bytes + 5, s.size - 5) == 0));
        if (read && actual[0] == 0xE9 && ok) std::fprintf(log, "bot store: %s at %08X is detoured by someone else; called through the detour\n", s.name,
                                                           static_cast<unsigned>(base + s.rva));
        if (!ok) std::fprintf(log, "bot store: %s at %08X does not match\n", s.name, static_cast<unsigned>(base + s.rva));
        sites = sites && ok;
    }
    const bool close = sites && hook_site::Install(log, "bot PlayerStoreSM destructor", base + kStoreCloseRva, kClose, sizeof(kClose),
                                                   reinterpret_cast<void*>(OnStoreClose), storeCloseHook);
    storeReady = sites && close;
    std::fprintf(log, "bot store=%d (functions=%d close_hook=%d: bots buy med packs and ammo with their own credits when the leader leaves a store)\n",
                 storeReady ? 1 : 0, sites ? 1 : 0, close ? 1 : 0);
    return storeReady;
}

// --- guns (the weapon economy) ----------------------------------------------------------
// At a store visit a bot buys
// one gun whose chapter has come (the store's own rule, MinLevelReq <= the
// leader's chapter [player+0xFC8]) with its own credits, the store's way replayed
// step by step: a GunProxy placeholder (GetGlobalPickupItem with the proxy GUID;
// every gun's proxy is resident), Item::Pickup (0x00DEF170, thiscall(item,
// owner, vfx), ret 8) into a free weapon slot, then Quartermaster::RequisitionGun
// (0x00DDD5F0, thiscall(qm, player, guid*, force), ret 0xC). With the gun's stream
// loaded IssueGun swaps the real gun in at once; else the stream is requested and
// the swap comes later (the placeholder waits in the slot). The Quartermaster
// keeps only 4 weapon streams in the campaign ([qm+0x11]); the bots raise it and
// always leave room for the leader's own new guns. Four slots full: the gun it
// likes least (its profile's FavoriteWeapons first) goes to its safe
// (Gun::MoveFromInventoryToSafe, vtable +0x288, 0x00DE0EB0), never the one in hand.
// A new body (respawn, level) starts with the template's guns again: what a bot
// bought is handed back to it free.
constexpr uint32_t kGunProxyVtableRva = 0x1751E20, kItemPickupRva = 0x9EF170, kRequisitionRva = 0x9DD5F0, kCancelRequisitionRva = 0x9D6C40;
constexpr uint32_t kInventoryIsFullRva = 0x9C4C70, kInventoryRemoveRva = 0x9D7180, kMoveToSafeSlot = 0x288, kMoveToSafeRva = 0x9E0EB0;
constexpr uint32_t kChapter = 0xFC8, kQmMaxStreams = 0x11, kQmLoadedStreams = 0x12, kGunEntryHandle = 0x34;
constexpr DWORD kGunTimeoutMs = 20000, kReissueEveryMs = 3000;
constexpr unsigned kLeaderStreamRoom = 2;   // weapon streams always left for the leader's own new guns
struct GunInfo { const char* name; coop_bot::Gun kind; uint32_t vtableRva; uint32_t guid[4]; uint32_t price; int minChapter; };
static const GunInfo kGunCatalogue[] = {
    {"PlasmaCutter", coop_bot::Gun::PlasmaCutter, 0x17515D8, {0x30CAF6B7, 0xD12A955D, 0x54544950, 0x32303153}, 2000, 0},
    {"PulseRifle", coop_bot::Gun::PulseRifle, 0x1751A00, {0x36C8BC4F, 0x67BB5F38, 0x44455257, 0x34303245}, 7000, 4},
    {"LineGun", coop_bot::Gun::LineGun, 0x17511B8, {0x36C8BC49, 0x5F163CD0, 0x44455257, 0x34303245}, 9000, 2},
    {"Javelin", coop_bot::Gun::Javelin, 0x1750D98, {0x30C95FEE, 0xBBA303B8, 0x544E4543, 0x30574F4B}, 11000, 2},
    {"Ripper", coop_bot::Gun::Ripper, 0x1752580, {0x36C8BC4F, 0x6F9078E2, 0x44455257, 0x34303245}, 8000, 6},
    {"Detonator", coop_bot::Gun::Detonator, 0x1753100, {0x58CA30C7, 0x78E54CEB, 0x3045454C, 0x58373634}, 8000, 7},
    {"Flamethrower", coop_bot::Gun::Flamethrower, 0x1753520, {0x36C8BC40, 0x0C67B69A, 0x44455257, 0x34303245}, 11000, 7},
    {"SeekerRifle", coop_bot::Gun::SeekerRifle, 0x1773780, {0x45CB0685, 0xDD95DCDA, 0x43534147, 0x4E47494F}, 11000, 8},
    {"ForceGun", coop_bot::Gun::ForceGun, 0x1753948, {0x36C8BC48, 0xED477CBA, 0x44455257, 0x34303245}, 11000, 10},
    {"ContactBeam", coop_bot::Gun::ContactBeam, 0x1752CE0, {0x36C8BC3F, 0xF55615C8, 0x44455257, 0x34303245}, 9000, 11},
};
constexpr int kGunKinds = static_cast<int>(sizeof(kGunCatalogue) / sizeof(kGunCatalogue[0]));
uint32_t KioskMachine(uint32_t player, int32_t& state, bool& store);   // "using the kiosks" below
bool gunsWanted = false, gunsReadyToBuy = false;
unsigned gunStreamCap = 9;
int creditShare = 0;   // percent of every credits find of the leader that each bot gets
int32_t leaderCreditsSeen = -1;
volatile LONG gunBuys = 0, gunReissues = 0, gunArrivals = 0, gunFails = 0, gunRefunds = 0, gunSwaps = 0, gunTimeouts = 0, gunFallbacks = 0, gunLogLines = 0, creditsShared = 0,
              streamRaises = 0;

int GunIndexOfVtable(uint32_t vtable) {
    for (int i = 0; i < kGunKinds; ++i) if (vtable == base + kGunCatalogue[i].vtableRva) return i;
    return -1;
}
// The catalogue bit of a gun object, 0 for anything else.
uint32_t GunBit(uint32_t gun) {
    uint32_t vtable = 0;
    const int idx = gun && Read(gun, &vtable, 4) ? GunIndexOfVtable(vtable) : -1;
    return idx >= 0 ? 1u << idx : 0u;
}
int GunIndexByName(const char* name) {
    for (int i = 0; i < kGunKinds; ++i) if (_stricmp(name, kGunCatalogue[i].name) == 0) return i;
    return -1;
}
// Lower is better: its favourites in their order, then the later (stronger) guns.
int GunRank(const Unit& unit, int idx) {
    for (int k = 0; k < unit.favoriteCount; ++k) if (unit.favorites[k] == idx) return k;
    return 10 + (12 - kGunCatalogue[idx].minChapter);
}
// The catalogue bits of the guns a Player carries, and where each sits.
uint32_t CarriedGuns(uint32_t player, uint32_t* gunAt, int* proxies) {
    uint32_t bits = 0, entries = 0, size = 0;
    if (proxies) *proxies = 0;
    if (!Read(player + kWeaponInventory + kInventoryEntries, &entries, 4) || !entries || !Read(player + kWeaponInventory + kInventorySize, &size, 4)) return 0;
    for (uint32_t i = 0; i < size && i < 8; ++i) {
        uint32_t ref = 0, vtable = 0;
        if (!Read(entries + i * kInventoryEntry + 4, &ref, 4) || ref < 0x10010 || !Read(ref - 0x10, &vtable, 4)) continue;
        if (vtable == base + kGunProxyVtableRva) { if (proxies) ++*proxies; continue; }
        const int idx = GunIndexOfVtable(vtable);
        if (idx < 0) continue;
        bits |= 1u << idx;
        if (gunAt) gunAt[idx] = ref - 0x10;
    }
    return bits;
}
int LeaderChapter() {
    const uint32_t leader = Leader();
    int32_t chapter = 0;
    return leader && Read(leader + kChapter, &chapter, 4) && chapter >= 1 && chapter <= 30 ? chapter : 0;
}
__declspec(noinline) int ThisBool0(uint32_t fn, uint32_t self) {
    __try {
        return reinterpret_cast<bool(__thiscall*)(void*)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(self))) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
__declspec(noinline) bool ThisVoid0(uint32_t fn, uint32_t self) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(self)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool RemoveItemRaw(uint32_t inventory, uint32_t item) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kInventoryRemoveRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(inventory)), item);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) int RequisitionGuarded(uint32_t qm, uint32_t player, const uint32_t* guid) {
    __try {
        return reinterpret_cast<bool(__thiscall*)(void*, uint32_t, const void*, int)>(base + kRequisitionRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(qm)), player, guid, 0) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&mpquiet::faults);
        return -1;
    }
}
int RequisitionRaw(uint32_t qm, uint32_t player, const uint32_t* guid) {
    mpquiet::Scope quiet;
    return RequisitionGuarded(qm, player, guid);
}
__declspec(noinline) bool CancelRequisitionGuarded(uint32_t qm, uint32_t player, const uint32_t* guid) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t, const void*)>(base + kCancelRequisitionRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(qm)), player, guid);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&mpquiet::faults);
        return false;
    }
}
bool CancelRequisitionRaw(uint32_t qm, uint32_t player, const uint32_t* guid) {
    mpquiet::Scope quiet;
    return CancelRequisitionGuarded(qm, player, guid);
}
void GunEvent(const char* how, int idx, uint32_t price) {
    std::snprintf(u->prog.gunWhy, sizeof(u->prog.gunWhy), "%s", how);   // for the progression's own lines
    if (InterlockedIncrement(&gunLogLines) <= 150)
        gate::Event("bot %u (%s): gun %s: %s (%u credits)", u->index + 1, u->name, idx >= 0 ? kGunCatalogue[idx].name : "?", how, price);
}
void AddCredits(int32_t amount) {
    if (!amount) return;
    u->creditWallet += amount;
    if (u->entity && u->creditsSet) {
        int32_t have = 0;
        if (Read(u->entity + kGeneralInventory + kInventoryCredits, &have, 4)) {
            have += amount;
            if (have < 0) have = 0;
            WriteBytes(u->entity + kGeneralInventory + kInventoryCredits, &have, 4);
            u->creditWallet = have;
            u->creditsSeen = have;
        }
    }
}
// The campaign's four weapon streams would stop at the fifth gun type: raised once
// per level (the retail multiplayer value is 7; the ten base guns need at most 10).
void RaiseStreamBudget(uint32_t qm) {
    unsigned char cap = 0;
    if (!Read(qm + kQmMaxStreams, &cap, 1) || cap >= gunStreamCap) return;
    const unsigned char want = static_cast<unsigned char>(gunStreamCap);
    if (WriteBytes(qm + kQmMaxStreams, &want, 1) && InterlockedIncrement(&streamRaises) <= 20)
        gate::Event("bot guns: weapon streams allowed %u -> %u", cap, want);
}
// This gun's stream is loaded (or loading), or one more fits and still leaves room for the leader.
bool StreamRoom(uint32_t qm, int idx) {
    const uint32_t entry = GunEntryRaw(qm, reinterpret_cast<const unsigned char*>(kGunCatalogue[idx].guid));
    uint32_t handle = 0;
    unsigned char cap = 0, loaded = 0;
    if (!entry) return false;
    if (Read(entry + kGunEntryHandle, &handle, 4) && handle) return true;
    return Read(qm + kQmMaxStreams, &cap, 1) && Read(qm + kQmLoadedStreams, &loaded, 1) && loaded + 1u + kLeaderStreamRoom <= cap;
}
// One gun into the bot's free weapon slot, paid or handed back free.
bool StartGun(int idx, bool paid, DWORD now) {
    uint32_t qm = 0;
    if (!Read(base + kQuartermasterRva, &qm, 4) || !qm) { GunEvent("no quartermaster", idx, 0); return false; }
    RaiseStreamBudget(qm);
    const GunInfo& info = kGunCatalogue[idx];
    const uint32_t weapons = u->entity + kWeaponInventory;
    if (ThisBool0(base + kInventoryIsFullRva, weapons) != 0) { GunEvent("no free slot", idx, 0); return false; }
    if (!StreamRoom(qm, idx)) { InterlockedIncrement(&gunFails); GunEvent("no room for its weapon stream", idx, 0); return false; }
    const uint32_t proxy = SpawnPickupRaw(reinterpret_cast<const unsigned char*>(info.guid));
    uint32_t vtable = 0;
    if (!proxy || !Read(proxy, &vtable, 4) || vtable != base + kGunProxyVtableRva) {
        if (proxy) DeleteRaw(proxy);
        InterlockedIncrement(&gunFails);
        GunEvent("no placeholder spawned", idx, 0);
        return false;
    }
    uint32_t price = 0;
    if (paid) {
        uint32_t attributes = 0, unit = 0, amount = 1, suit = 0, suitAttributes = 0;
        float discount = 0.0f;
        if (!Read(proxy + kGunAttributes, &attributes, 4) || !attributes || !Read(attributes + kItemPrice, &unit, 4) || !unit || unit > 1000000) unit = info.price;
        if (!Read(proxy + kItemAmount, &amount, 4) || !amount || amount > 10) amount = 1;
        if (Read(u->entity + kPlayerUniform, &suit, 4) && suit && Read(suit + kGunAttributes, &suitAttributes, 4) && suitAttributes)
            Read(suitAttributes + kUniformDiscount, &discount, 4);
        if (!std::isfinite(discount) || discount < 0.0f || discount > 1.0f) discount = 0.0f;
        price = unit * amount - static_cast<uint32_t>(std::ceil(static_cast<double>(unit * amount) * discount));
        int32_t credits = 0;
        Read(u->entity + kGeneralInventory + kInventoryCredits, &credits, 4);
        if (credits < 0 || static_cast<uint32_t>(credits) < price) { DeleteRaw(proxy); GunEvent("too poor", idx, price); return false; }
    }
    if (PickupRaw(proxy, base + kItemPickupRva, u->entity) != 1) {
        DeleteRaw(proxy);
        InterlockedIncrement(&gunFails);
        GunEvent("the placeholder did not go into a slot", idx, price);
        return false;
    }
    const int ok = RequisitionRaw(qm, u->entity, info.guid);
    if (ok != 1) {
        RemoveItemRaw(weapons, proxy);
        DeleteRaw(proxy);
        InterlockedIncrement(&gunFails);
        GunEvent(ok < 0 ? "requisition faulted" : "requisition refused (stream budget)", idx, price);
        return false;
    }
    if (paid && price) {
        if (!SubtractRaw(u->entity + kGeneralInventory, price)) InterlockedIncrement(&storeFaults);
        uint32_t after = 0;
        Read(u->entity + kGeneralInventory + kInventoryCredits, &after, 4);
        u->creditWallet = static_cast<int32_t>(after);
        u->creditsSeen = static_cast<int32_t>(after);
        InterlockedExchangeAdd(&creditsSpent, static_cast<LONG>(price));
    }
    InterlockedIncrement(paid ? &gunBuys : &gunReissues);
    u->gunPending = idx;
    u->gunProxy = proxy;
    u->gunPrice = paid ? price : 0;
    u->gunStartedAt = now;
    GunEvent(paid ? "bought, waiting for the gun" : "handed back, waiting for the gun", idx, price);
    return true;
}
// The purchase under way: the real gun in the placeholder's slot, still waiting, or given up.
void EndGun(const char* how, bool refund) {
    if (refund && u->gunPrice) { AddCredits(static_cast<int32_t>(u->gunPrice)); InterlockedIncrement(&gunRefunds); }
    GunEvent(how, u->gunPending, refund ? u->gunPrice : 0);
    u->gunPending = -1;
    u->gunProxy = 0;
    u->gunPrice = 0;
}
void PollGun(DWORD now) {
    if (u->gunPending < 0) return;
    const int idx = u->gunPending;
    uint32_t entries = 0, size = 0;
    bool waiting = false, arrived = false;
    uint32_t gun = 0;
    if (Read(u->entity + kWeaponInventory + kInventoryEntries, &entries, 4) && entries && Read(u->entity + kWeaponInventory + kInventorySize, &size, 4)) {
        for (uint32_t i = 0; i < size && i < 8; ++i) {
            uint32_t ref = 0, vtable = 0;
            if (!Read(entries + i * kInventoryEntry + 4, &ref, 4) || ref < 0x10010) continue;
            if (ref - 0x10 == u->gunProxy) waiting = true;
            else if (Read(ref - 0x10, &vtable, 4) && vtable == base + kGunCatalogue[idx].vtableRva) { arrived = true; gun = ref - 0x10; }
        }
    }
    if (arrived && !waiting) {
        u->gunsOwned |= 1u << idx;
        InterlockedIncrement(&gunArrivals);
        int32_t clip = -1;
        Read(gun + kGunClip, &clip, 4);
        char how[64];
        std::snprintf(how, sizeof(how), "in hand after %lu ms (clip %d)", static_cast<unsigned long>(now - u->gunStartedAt), clip);
        EndGun(how, false);
        return;
    }
    if (!waiting && now - u->gunStartedAt > 1500) { InterlockedIncrement(&gunFails); EndGun("placeholder gone, no gun: given up", true); return; }
    if (now - u->gunStartedAt > kGunTimeoutMs) {
        uint32_t qm = 0;
        InterlockedIncrement(&gunTimeouts);
        if (Read(base + kQuartermasterRva, &qm, 4) && qm) CancelRequisitionRaw(qm, u->entity, kGunCatalogue[idx].guid);
        if (waiting && RemoveItemRaw(u->entity + kWeaponInventory, u->gunProxy)) DeleteRaw(u->gunProxy);
        EndGun("the stream never came: given up", true);
    }
}
// Make room: the carried gun it likes least goes to its safe, never the one in
// hand, and only for a better one.
bool FreeSlotFor(int idx, const uint32_t* gunAt, uint32_t carried) {
    const uint32_t active = ActiveGun();
    int worst = -1;
    for (int k = 0; k < kGunKinds; ++k)
        if ((carried >> k) & 1u && gunAt[k] && gunAt[k] != active && (worst < 0 || GunRank(*u, k) > GunRank(*u, worst))) worst = k;
    if (worst < 0 || GunRank(*u, worst) <= GunRank(*u, idx)) return false;
    uint32_t vtable = 0, fn = 0;
    if (!Read(gunAt[worst], &vtable, 4) || !Read(vtable + kMoveToSafeSlot, &fn, 4) || fn != base + kMoveToSafeRva) return false;
    if (!ThisVoid0(fn, gunAt[worst])) return false;
    InterlockedIncrement(&gunSwaps);
    GunEvent("put in its safe to make room", worst, 0);
    return true;
}
// At a store visit: one gun, the best ranked one it can have and afford.
int survivalChapter = 0;   // set by SurvivalShop: every gun is on offer
bool ProgressOwnsGuns();   // "the kit outside the campaign", below
void ShopGun(uint32_t leader) {
    if (!gunsReadyToBuy || !u->entity || !u->creditsSet || u->gunPending >= 0) return;
    if (ProgressOwnsGuns()) return;   // outside the campaign its guns come by schedule
    (void)leader;
    const int chapter = survivalChapter ? survivalChapter : LeaderChapter();
    uint32_t gunAt[kGunKinds]{};
    int proxies = 0;
    const uint32_t carried = CarriedGuns(u->entity, gunAt, &proxies);
    if (proxies) return;
    uint32_t mates = 0;
    for (const Unit& other : units) if (&other != u && other.entity) mates |= CarriedGuns(other.entity, nullptr, nullptr);
    int32_t credits = 0;
    Read(u->entity + kGeneralInventory + kInventoryCredits, &credits, 4);
    uint32_t qm = 0;
    Read(base + kQuartermasterRva, &qm, 4);
    if (!qm) return;
    RaiseStreamBudget(qm);
    // Best first; the price check is a guess (a suit discount may apply), so a
    // gun the real price rules out passes to the next (06.10.: Vasquez wanted a
    // pulse rifle at 7000 with 6000, bought nothing and fought unarmed).
    int order[kGunKinds], score[kGunKinds], n = 0;
    for (int i = 0; i < kGunKinds; ++i) {
        if ((carried >> i) & 1u || kGunCatalogue[i].minChapter > chapter || static_cast<uint32_t>(credits) < kGunCatalogue[i].price * 8u / 10u) continue;
        if (!StreamRoom(qm, i)) continue;
        order[n] = i;
        score[n++] = GunRank(*u, i) + ((mates >> i) & 1u ? 3 : 0);   // something the others do not carry, when it is all the same
    }
    for (int a = 1; a < n; ++a)
        for (int b = a; b > 0 && score[b] < score[b - 1]; --b) { std::swap(order[b], order[b - 1]); std::swap(score[b], score[b - 1]); }
    const bool full = ThisBool0(base + kInventoryIsFullRva, u->entity + kWeaponInventory) == 1;
    for (int k = 0; k < n; ++k) {
        if (full && !FreeSlotFor(order[k], gunAt, carried)) continue;
        if (StartGun(order[k], true, GetTickCount())) return;
        if (k + 1 < n) InterlockedIncrement(&gunFallbacks);
        if (full) return;   // the slot it freed holds no gun now: no second swap
    }
}
// Survival has no kiosk (10.10.: the bots never bought anything there). Once
// per break every active bot does what it does after the leader's store visit
// (a better gun, whatever the chapter; med packs; ammo), then buys power nodes
// for its gun trees with half of what is left, at most four, at the store's node
// price. Trees belong to the gun kind: the leader's guns of that kind gain too.
// Returns how many bots shopped, -1 without the store hooks.
constexpr int kSurvivalNodesPerBreak = 4;
volatile LONG survivalShops = 0, survivalNodesBought = 0;
int SurvivalShop(uint32_t nodePrice, const uint32_t* medGuid, int nextWave) {
    if (!storeReady) return -1;
    Unit* const saved = u;
    const uint32_t leader = Leader();
    if (!medPack.known && medGuid) {
        // Learned from a carried med pack later, if one is cheaper.
        std::memcpy(medPack.guid, medGuid, 16);
        medPack.price = 0xFFFFFFFFu;
        medPack.known = true;
    }
    survivalChapter = 30;
    int shopped = 0;
    for (unsigned i = 0; i < wantedBots; ++i) {
        u = &units[i];
        if (!u->entity || u->state != State::Active || !u->creditsSet) continue;
        int32_t before = 0, mid = 0, after = 0;
        Read(u->entity + kGeneralInventory + kInventoryCredits, &before, 4);
        Shop(leader);
        Read(u->entity + kGeneralInventory + kInventoryCredits, &mid, 4);
        after = mid;
        int nodes = 0;
        if (benchReady && u->nodesSet && nodePrice && mid > 0) {
            nodes = static_cast<int>(static_cast<uint32_t>(mid / 2) / nodePrice);
            if (nodes > kSurvivalNodesPerBreak) nodes = kSurvivalNodesPerBreak;
            if (nodes > 0 && SubtractRaw(u->entity + kGeneralInventory, static_cast<uint32_t>(nodes) * nodePrice)) {
                Read(u->entity + kGeneralInventory + kInventoryCredits, &after, 4);
                u->creditWallet = after;
                u->creditsSeen = after;
                u->nodeWallet += nodes;
                WriteBytes(u->entity + kPlayerNodes, &u->nodeWallet, 4);
                u->nodesSeen = u->nodeWallet;
                InterlockedExchangeAdd(&survivalNodesBought, nodes);
                Invest(leader);
            } else {
                nodes = 0;
            }
        }
        ++shopped;
        InterlockedIncrement(&survivalShops);
        gate::Event("bot %u: survival break before wave %d: credits %d -> %d at the store, %d power node(s) for %u each -> %d credits", u->index + 1, nextWave,
                    before, mid, nodes, nodePrice, after);
    }
    survivalChapter = 0;
    u = saved;
    return shopped;
}
// A new body is a copy of the leader's loadout ([Bot] OwnGunsOnly): the guns it
// never bought go to its own safe, one a pass, and a plasma cutter is its base
// (06.10.: after a load the bots carried every gun of the leader's).
// Gun::MoveFromInventoryToSafe (0x00DE0EB0, thiscall(gun), ret) works on the
// gun's owner at +0x240: a safe copy into owner+0xC38, out of its slots, the
// hand cleared if it held it, the gun deleted. Only guns the bot owns go there.
constexpr uint32_t kGunOwner = 0x240;
bool ownGunsOnly = true;
volatile LONG gunsTrimmed = 0, gunTrimRefused = 0, cutterGrants = 0, trimSwitches = 0;
uint32_t ProgressKeep();                                         // "the kit outside the campaign", below
uint32_t ProgressBaseGuns(uint32_t carried, uint32_t cutterBit);
extern volatile LONG progressCutterTrims;
// True when it did something this pass.
bool TrimTemplateGuns(DWORD now, const uint32_t* gunAt, uint32_t carried) {
    if (!ownGunsOnly) return false;
    static const int cutter = GunIndexByName("PlasmaCutter");
    const uint32_t cutterBit = cutter >= 0 ? 1u << cutter : 0u;
    // Mercenaries and Versus: the cutter only until its kit for the mode is in its hands.
    // Its hand-back marked it owned (PollGun); dropped again, or ReissueGuns would bring it back.
    const uint32_t baseGuns = ProgressBaseGuns(carried, cutterBit), kitKeep = ProgressKeep();
    if (!(baseGuns & cutterBit) && !(kitKeep & cutterBit)) u->gunsOwned &= ~cutterBit;
    const uint32_t keep = u->gunsOwned | baseGuns | kitKeep, extra = carried & ~keep;
    const uint32_t active = ActiveGun();
    for (int k = 0; k < kGunKinds; ++k) {
        if (!((extra >> k) & 1u) || !gunAt[k] || gunAt[k] == active) continue;
        uint32_t ownerRef = 0, vtable = 0, fn = 0;
        const bool mine = Read(gunAt[k] + kGunOwner, &ownerRef, 4) && ownerRef > 0x10010 && ownerRef - 0x10 == u->entity;
        if (!mine || !Read(gunAt[k], &vtable, 4) || !Read(vtable + kMoveToSafeSlot, &fn, 4) || fn != base + kMoveToSafeRva) {
            if (InterlockedIncrement(&gunTrimRefused) <= 20)
                gate::Event("bot %u: the %s it never bought stays (%s)", u->index + 1, kGunCatalogue[k].name, mine ? "unexpected vtable slot" : "owned by someone else");
            continue;
        }
        if (!ThisVoid0(fn, gunAt[k])) { InterlockedIncrement(&gunTrimRefused); GunEvent("putting a gun it never bought in its safe faulted", k, 0); continue; }
        InterlockedIncrement(&gunsTrimmed);
        const bool kitTrim = k == cutter && !(baseGuns & cutterBit);
        if (kitTrim) InterlockedIncrement(&progressCutterTrims);
        GunEvent(kitTrim ? "not in its kit for this mode: put in its own safe" : "never bought (a copy of the leader's): put in its own safe", k, 0);
        return true;
    }
    // Its base gun, once the leader carries a gun at all (none in the straitjacket run).
    const uint32_t leader = Leader();
    if ((baseGuns & cutterBit) && !(carried & cutterBit) && leader && CarriedGuns(leader, nullptr, nullptr) &&
        ThisBool0(base + kInventoryIsFullRva, u->entity + kWeaponInventory) == 0) {
        if (StartGun(cutter, false, now)) InterlockedIncrement(&cutterGrants);
        return true;
    }
    // Only the gun in hand is left over: take a kept one, then it goes next pass.
    if (active && GunBit(active) && !(keep & GunBit(active))) {
        for (int i = 0; i < coop_bot::kGunSlots; ++i) {
            const uint32_t gun = u->slotGun[i];
            if (!gun || gun == active || !(keep & GunBit(gun))) continue;
            TakeSlot(i, now);
            InterlockedIncrement(&trimSwitches);
            return true;
        }
    }
    return false;
}
// A new body carries the template's guns: what it bought (the best four by its
// ranking) is handed back, one at a time, outside fights.
void ReissueGuns(DWORD now) {
    if (!gunsReadyToBuy || u->gunPending >= 0 || now - u->gunCheckAt < kReissueEveryMs) return;
    u->gunCheckAt = now;
    if (u->state != State::Active || u->kioskVisit != KioskStep::None || u->command.mode == coop_bot::Mode::Combat || u->tkStep != TkStep::None) return;
    uint32_t gunAt[kGunKinds]{};
    int proxies = 0;
    const uint32_t carried = CarriedGuns(u->entity, gunAt, &proxies);
    if (proxies) return;
    if (TrimTemplateGuns(now, gunAt, carried) || !u->gunsOwned) return;
    // The loadout it wants: the four best of what it carries and what it bought.
    int order[kGunKinds], n = 0;
    for (int i = 0; i < kGunKinds; ++i) if (((carried | u->gunsOwned) >> i) & 1u) order[n++] = i;
    for (int a = 1; a < n; ++a)
        for (int b = a; b > 0 && GunRank(*u, order[b]) < GunRank(*u, order[b - 1]); --b) std::swap(order[b], order[b - 1]);
    for (int k = 0; k < n && k < 4; ++k) {
        const int idx = order[k];
        if ((carried >> idx) & 1u) continue;
        if (ThisBool0(base + kInventoryIsFullRva, u->entity + kWeaponInventory) == 1 && !FreeSlotFor(idx, gunAt, carried)) return;
        StartGun(idx, false, now);
        return;
    }
}
// Every credits find of the leader (outside the store: selling there is no find)
// gives each bot its own share, like its own loot in co-op.
// The leader's balance: finds, spending (stores, restarts) and finds not shared, for the log.
volatile LONG leaderFinds = 0, leaderFindsUnshared = 0, leaderCreditDrops = 0, creditsSeeded = 0;
int32_t leaderCreditsNow = -1;
void ShareCredits(uint32_t leader) {
    int32_t have = 0;
    if (!creditShare || !Read(leader + kGeneralInventory + kInventoryCredits, &have, 4) || have < 0 || have > 9999999) return;
    leaderCreditsNow = have;
    if (leaderCreditsSeen < 0) { leaderCreditsSeen = have; return; }
    const int32_t found = have - leaderCreditsSeen;
    leaderCreditsSeen = have;
    if (found < 0) {
        if (InterlockedIncrement(&leaderCreditDrops) <= 40) gate::Event("bot credits: the leader's credits went down by %d to %d (spent or restored)", -found, have);
        return;
    }
    if (!found) return;
    InterlockedIncrement(&leaderFinds);
    int32_t state = -1;
    bool store = false;
    const bool kiosk = KioskMachine(leader, state, store) != 0;
    if (found > 20000 || kiosk) {
        InterlockedIncrement(&leaderFindsUnshared);
        if (InterlockedIncrement(&gunLogLines) <= 150)
            gate::Event("bot credits: the leader's credits went up by %d to %d, not shared (%s)", found, have, kiosk ? "at a kiosk: a sale" : "too much for a find");
        return;
    }
    const int32_t share = found * creditShare / 100;
    if (share <= 0) return;
    Unit* const saved = u;
    for (unsigned i = 0; i < wantedBots; ++i) {
        u = &units[i];
        AddCredits(share);
    }
    u = saved;
    InterlockedExchangeAdd(&creditsShared, share);
    if (InterlockedIncrement(&gunLogLines) <= 150) gate::Event("bot credits: the leader found %d credits, each bot gets %d", found, share);
}
// The other way round: credits a bot picks up give the leader the same share.
// His count is moved on with it, so ShareCredits does not hand it out again.
volatile LONG creditsToLeader = 0;
void GiveLeaderShare(int32_t found) {
    const uint32_t leader = Leader();
    int32_t have = 0;
    if (!creditShare || found <= 0 || found > 20000 || !leader || !Read(leader + kGeneralInventory + kInventoryCredits, &have, 4) || have < 0) return;
    const int32_t share = found * creditShare / 100;
    if (share <= 0) return;
    const int32_t now = have + share;
    if (!WriteBytes(leader + kGeneralInventory + kInventoryCredits, &now, 4)) return;
    if (leaderCreditsSeen >= 0) leaderCreditsSeen += share;
    InterlockedExchangeAdd(&creditsToLeader, share);
    if (InterlockedIncrement(&gunLogLines) <= 150) gate::Event("bot guns: bot %u found %d credits, the leader gets %d", u->index + 1, found, share);
}
bool InstallGuns(FILE* log) {
    if (!gunsWanted) { std::fprintf(log, "bot buy_guns=0\n"); return false; }
    struct Site { uint32_t rva; unsigned char bytes[16]; unsigned size; const char* name; };
    static const Site kSites[] = {
        {kItemPickupRva, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x14, 0x8B, 0x45, 0x08, 0x53, 0x56, 0x57, 0x8B}, 16, "Item::Pickup"},
        {kRequisitionRva, {0x51, 0x53, 0x55, 0x56, 0x57, 0x8D, 0x44, 0x24, 0x10, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x1C, 0x50}, 16, "Quartermaster::RequisitionGun"},
        {kCancelRequisitionRva, {0x53, 0x55, 0x8B, 0xE9, 0x33, 0xDB, 0x39, 0x5D, 0x24, 0x76, 0x69, 0x56, 0x57, 0x8D, 0x7D, 0x20}, 16, "Quartermaster::CancelRequisition"},
        {kInventoryIsFullRva, {0xF6, 0x41, 0x30, 0x02, 0x8B, 0x51, 0x08, 0x57, 0x8B, 0xFA, 0x74, 0x29, 0x33, 0xFF, 0x85, 0xD2}, 16, "Inventory::IsFull"},
        {kInventoryRemoveRva, {0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xF1, 0x85, 0xC0, 0x74, 0x13, 0x50, 0xE8}, 13, "Inventory::RemoveItem"},
        {kMoveToSafeRva, {0x83, 0xEC, 0x28, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x87, 0x40, 0x02, 0x00, 0x00, 0x85}, 16, "Gun::MoveFromInventoryToSafe"},
    };
    bool sites = storeReady && gunsReady;
    for (const Site& s : kSites) {
        unsigned char actual[16]{};
        const bool read = Read(base + s.rva, actual, s.size);
        const bool ok = read && (std::memcmp(actual, s.bytes, s.size) == 0 || (actual[0] == 0xE9 && std::memcmp(actual + 5, s.bytes + 5, s.size - 5) == 0));
        if (!ok) std::fprintf(log, "bot guns: %s at %08X does not match\n", s.name, static_cast<unsigned>(base + s.rva));
        sites = sites && ok;
    }
    gunsReadyToBuy = sites;
    std::fprintf(log, "bot buy_guns=%d (store=%d functions=%d weapon_streams=%u credit_share=%d%%: at a store visit each bot buys one gun whose chapter has come)\n",
                 gunsReadyToBuy ? 1 : 0, storeReady ? 1 : 0, sites ? 1 : 0, gunStreamCap, creditShare);
    std::fprintf(log, "bot own_guns_only=%d (a new body keeps only the guns the bot bought, plus a plasma cutter; needs buy_guns)\n", ownGunsOnly ? 1 : 0);
    return gunsReadyToBuy;
}

// --- the kit outside the campaign ([Bot] Progression, ModeLoadout) ----------------------
// 13.10.: outside the campaign the bots kept the plasma cutter all match. Mercenaries,
// Versus, Objective and Hold the Zone have no store break for them, and in survival
// the shared credits paid for a first gun at the campaign's prices only after wave 5.
// Now they get a player's kit by schedule (coop_bot_progress.h), free, through
// StartGun and DressIn: Mercenaries and Versus at once, the wave modes after so many
// completed waves, Objective and Hold the Zone after so many minutes of game time.
using coop_bot_progress::Kind;
bool progressionWanted = true, modeLoadoutWanted = true;
float progressWaves[coop_bot_progress::kSteps] = {3, 5, 7, 9}, progressMinutes[coop_bot_progress::kSteps] = {2, 4, 6, 8};
int progressStagger = 2, progressStaggerSeconds = 45;
uint32_t progressSessionSeed = 0;
constexpr DWORD kProgressCheckMs = 500, kProgressRetryMs = 10000, kProgressGunGraceMs = 15000, kProgressSuitGraceMs = 30000, kProgressSuitWaitMs = 25000;
constexpr int kProgressTries = 6;
static_assert(coop_bot_progress::kGuns == kGunKinds, "the plan indexes the gun catalogue");
static_assert(kSuitKinds <= coop_bot_progress::kMaxSuits, "every suit fits the suit order");
extern bool perSaveActive;   // "the bots' state per campaign save", below
struct ProgressRun {
    Kind kind = Kind::Off;
    int modeId = -1;
    const char* mode = "menu";
    unsigned number = 0;   // runs this session
    unsigned epoch = 0;
    int state = 0, wave = 0, completed = 0;
    bool sawWave = false;
    uint64_t clockMs = 0;   // game time with a wave running
    uint32_t simAt = 0;
    DWORD tickAt = 0;
    coop_bot_progress::Tiers tiers;
    uint32_t prices[coop_bot_progress::kGuns]{};
    int rotation[coop_bot_progress::kSteps]{};
};
ProgressRun prun;
using SurvivalStateFn = int(__cdecl*)(int*);
SurvivalStateFn progressSurvival = nullptr;
DWORD progressLookAt = 0;
bool progressExportMissing = false;
volatile LONG progressLines = 0, progressRuns = 0, progressReached = 0, progressStarted = 0, progressArrived = 0, progressRetries = 0, progressGivenUp = 0,
              progressAlternatives = 0, progressNoStream = 0, progressNoEntry = 0, progressSlotFull = 0, progressFightWaits = 0, progressBusyWaits = 0,
              progressSuitStarts = 0, progressSuitsInPlace = 0, progressUnregistered = 0, progressCutterTrims = 0, progressOwnedCleared = 0;
template<class... A> void ProgressEvent(const char* format, A... args) {
    if (InterlockedIncrement(&progressLines) <= 800) gate::Event(format, args...);
}
const char* const kProgressSteps[2][coop_bot_progress::kSteps] = {{"cheaper gun", "suit", "expensive gun", "fourth gun"},
                                                                    {"pulse rifle", "suit", "extra gun", "-"}};
const char* ProgressStepName(int s) { return kProgressSteps[prun.kind == Kind::Loadout ? 1 : 0][s]; }
const char* ProgressItemName(int s, int item) {
    if (item < 0) return "none";
    return coop_bot_progress::GunStep(s) ? kGunCatalogue[item].name : SuitName(item);
}

struct ProgressModeInfo { Kind kind; int id; const char* name; };
// The picked mode on an MP map; anywhere else survival when a run is on (the arenas).
ProgressModeInfo ProgressModeNow() {
    if (!inLevel) return {Kind::Off, -1, "menu"};
    const bool visit = mpmap::InVisit();
    if (visit && mpnative::InVersus()) return {Kind::Loadout, 4, "versus"};
    if (visit && !arena::RunningNow()) {
        switch (InterlockedCompareExchange(&gamemode::mode, 0, 0)) {
            case 1: return {Kind::Loadout, 1, "mercenaries"};
            case 2: return {Kind::Clock, 2, "objective"};
            case 3: return {Kind::Clock, 3, "zone"};
            default: return {Kind::Waves, 0, "survival"};
        }
    }
    if (arena::RunningNow()) {
        switch (arena::ModeNow(static_cast<int>(InterlockedCompareExchange(&gamemode::mode, 0, 0)))) {
            case 1: return {Kind::Loadout, 1, "arena mercenaries"};
            case 3: return {Kind::Clock, 3, "arena zone"};
            default: return {Kind::Waves, 5, "arena"};
        }
    }
    if (play::SurvivalMode()) return {Kind::Waves, 5, "survival"};
    return {Kind::Story, -1, "story"};
}
// The director's state (1 arming, 2 wave, 3 intermission, 4 dead) and wave; bots run on the host only.
int ProgressSurvivalState(int* wave, DWORD now) {
    *wave = 0;
    if (!progressSurvival && now - progressLookAt > 5000) {
        progressLookAt = now;
        if (const HMODULE module = ds2coop::PartnerModule("DS2RandomizerObserver.asi"))
            progressSurvival = reinterpret_cast<SurvivalStateFn>(GetProcAddress(module, "DS2Survival_State"));
        if (!progressSurvival && !progressExportMissing) {
            progressExportMissing = true;
            ProgressEvent("bot progression: the survival export DS2Survival_State is missing: waves and the round clock stay at 0");
        }
    }
    return progressSurvival ? progressSurvival(wave) : 0;
}
// Shop price per catalogue gun: the terminal's [Shop] price, its default without the shop.
uint32_t ProgressPrice(int idx) {
    const int k = coop_shop::GunByVtableRva(kGunCatalogue[idx].vtableRva);
    if (k < 0) return kGunCatalogue[idx].price;
    return shop::priceGun[k] ? shop::priceGun[k] : coop_shop::kGuns[k].gunPrice;
}
void ProgressGunList(uint32_t bits, char* out, size_t size) {
    size_t used = 0;
    out[0] = 0;
    for (int i = 0; i < kGunKinds && used + 2 < size; ++i) {
        if (!((bits >> i) & 1u)) continue;
        const int wrote = std::snprintf(out + used, size - used, "%s%s", used ? " " : "", kGunCatalogue[i].name);
        if (wrote < 0) break;
        used += static_cast<size_t>(wrote);
    }
}
uint32_t ProgressStreams(char* out, size_t size) {
    uint32_t qm = 0;
    unsigned char cap = 0, loaded = 0;
    if (Read(base + kQuartermasterRva, &qm, 4) && qm) { Read(qm + kQmMaxStreams, &cap, 1); Read(qm + kQmLoadedStreams, &loaded, 1); }
    std::snprintf(out, size, "streams %u/%u, %u kept for the leader", loaded, cap, kLeaderStreamRoom);
    return qm;
}

// Back in the campaign without per-save state: what it owned before, unless something changed it since.
void ProgressBackToStory() {
    for (Unit& unit : units) {
        ProgressUnit& p = unit.prog;
        if (!p.stretch) continue;
        if (!perSaveActive) {
            if (unit.gunsOwned == p.ownedSeen) unit.gunsOwned = p.ownedBefore;
            if (unit.suitChosen == p.suitSeen) unit.suitChosen = p.suitBefore;
        }
        p = ProgressUnit{};
    }
}
// Once a frame, before the bots: the mode, the run, the waves and the round clock.
void ProgressFrame(DWORD now) {
    if (!progressionWanted && !modeLoadoutWanted) return;
    const ProgressModeInfo m = ProgressModeNow();
    int wave = 0;
    const int state = coop_bot_progress::Active(m.kind) ? ProgressSurvivalState(&wave, now) : 0;
    const unsigned epoch = mpmap::InVisit() ? mpmap::Run() : arena::RunningNow() ? 0x10000u + static_cast<unsigned>(arena::starts) : 0;
    const int before = prun.state;
    const char* why = nullptr;
    if (m.kind != prun.kind || m.id != prun.modeId) why = "the mode changed";
    else if (coop_bot_progress::Active(m.kind) && epoch != prun.epoch) why = "the map was loaded again";
    else if (coop_bot_progress::Active(m.kind) && state == 1 && before != 1 && prun.sawWave) why = "a new round arms";
    if (why) {
        const Kind was = prun.kind;
        prun.kind = m.kind;
        prun.modeId = m.id;
        prun.mode = m.name;
        prun.epoch = epoch;
        prun.clockMs = 0;
        prun.sawWave = false;
        if (coop_bot_progress::Active(m.kind)) {
            ++prun.number;
            InterlockedIncrement(&progressRuns);
            uint32_t candidates = 0;
            for (int i = 0; i < kGunKinds; ++i) {
                prun.prices[i] = ProgressPrice(i);
                if (i != coop_bot_progress::kCutter) candidates |= 1u << i;
            }
            prun.tiers = coop_bot_progress::SplitTiers(prun.prices, candidates);
            coop_bot_progress::Rng rng(coop_bot_progress::Mix(progressSessionSeed ^ (prun.number * 0x85EBCA6Bu)));
            for (int s = 0; s < coop_bot_progress::kSteps; ++s) prun.rotation[s] = rng.Below(progressStagger + 1);
            char streams[64], cheap[120], expensive[120];
            ProgressStreams(streams, sizeof(streams));
            ProgressGunList(prun.tiers.cheap, cheap, sizeof(cheap));
            ProgressGunList(prun.tiers.expensive, expensive, sizeof(expensive));
            ProgressEvent("bot progression run %u: %s (%s, %s), survival state %d wave %d, %s", prun.number, m.name, coop_bot_progress::KindName(m.kind), why, state, wave,
                          streams);
            if (m.kind != Kind::Loadout)
                ProgressEvent("bot progression run %u tiers: cheaper (up to %u cr) %s | expensive %s", prun.number, prun.tiers.median, cheap, expensive);
        } else if (coop_bot_progress::Active(was)) {
            ProgressEvent("bot progression off: %s (run %u is over)", m.name, prun.number);
            ProgressBackToStory();
        }
    }
    prun.mode = m.name;   // an arena launch turns from "survival" into "arena" within one run
    prun.state = state;
    prun.wave = wave;
    if (state == 2 || state == 3) prun.sawWave = true;
    prun.completed = coop_bot_progress::CompletedWaves(state, wave);
    const uint32_t sim = turret::SimMs();
    if (state == 2) {
        uint32_t add = 0;
        if (sim && prun.simAt && sim >= prun.simAt && sim - prun.simAt < 5000) add = sim - prun.simAt;
        else if (!sim) add = now - prun.tickAt < 250 ? now - prun.tickAt : 250;
        prun.clockMs += add;
    }
    prun.simAt = sim;
    prun.tickAt = now;
}
bool ProgressOwnsGuns() {
    if (!gunsReadyToBuy) return false;
    if (prun.kind == Kind::Loadout) return modeLoadoutWanted;
    return (prun.kind == Kind::Waves || prun.kind == Kind::Clock) && progressionWanted;
}
// The guns of the steps whose time has come: never trimmed to the safe.
uint32_t ProgressKeep() {
    const ProgressUnit& p = u->prog;
    if (!ProgressOwnsGuns() || p.run != prun.number) return 0;
    uint32_t bits = 0;
    for (int s = 0; s < coop_bot_progress::kSteps; ++s)
        if (coop_bot_progress::GunStep(s) && p.item[s] >= 0 && ((p.reached >> s) & 1u) && !((p.failed >> s) & 1u)) bits |= 1u << p.item[s];
    return bits;
}
uint32_t ProgressBaseGuns(uint32_t carried, uint32_t cutterBit) {
    const ProgressUnit& p = u->prog;
    if (prun.kind != Kind::Loadout || !ProgressOwnsGuns() || p.run != prun.number) return cutterBit;
    uint32_t kit = 0;
    if (p.item[0] >= 0) kit |= 1u << p.item[0];
    if (p.item[2] >= 0) kit |= 1u << p.item[2];
    return kit && (carried & kit) == kit ? 0u : cutterBit;
}
void ProgressDueText(const ProgressUnit& p, int s, char* out, size_t size) {
    if (prun.kind == Kind::Loadout) std::snprintf(out, size, "at once");
    else if (p.due[s] <= 0.0f) std::snprintf(out, size, "never");
    else if (prun.kind == Kind::Waves) std::snprintf(out, size, "after wave %d", static_cast<int>(p.due[s]));
    else std::snprintf(out, size, "at %u:%02u", static_cast<unsigned>(p.due[s]) / 60u, static_cast<unsigned>(p.due[s]) % 60u);
}
void ProgressNowText(char* out, size_t size) {
    if (prun.kind == Kind::Loadout) std::snprintf(out, size, "start");
    else if (prun.kind == Kind::Waves) std::snprintf(out, size, "wave %d done", prun.completed);
    else std::snprintf(out, size, "at %u:%02u", static_cast<unsigned>(prun.clockMs / 60000u), static_cast<unsigned>(prun.clockMs / 1000u % 60u));
}
// A plan for this run: guns and the suit picked with its own seed, the steps staggered.
void ProgressPlan() {
    ProgressUnit& p = u->prog;
    const bool loadout = prun.kind == Kind::Loadout;
    const bool stretch = p.stretch;
    const uint32_t ownedBefore = stretch ? p.ownedBefore : u->gunsOwned, lastGranted = p.granted;
    const int suitBefore = stretch ? p.suitBefore : u->suitChosen;
    if (!stretch && u->gunsOwned) {
        // A player starts this mode with nothing bought: its own guns go to its safe.
        InterlockedIncrement(&progressOwnedCleared);
        ProgressEvent("bot %u (%s): starts %s without the guns it owned (%X); they go to its safe", u->index + 1, u->name, prun.mode, u->gunsOwned);
        u->gunsOwned = 0;
    } else if (stretch && !loadout && lastGranted) {
        u->gunsOwned &= ~lastGranted;   // a new round starts from the cutter again
    }
    p = ProgressUnit{};
    p.stretch = true;
    p.ownedBefore = ownedBefore;
    p.suitBefore = suitBefore;
    p.run = prun.number;
    // The kit for Mercenaries and Versus stays the same all session; the progression picks anew each run.
    p.seed = coop_bot_progress::Mix(progressSessionSeed ^ coop_bot_progress::HashName(u->name) ^ ((u->index + 1) * 0x9E3779B9u) ^
                                    (loadout ? 0u : coop_bot_progress::Mix(prun.number)));
    coop_bot_progress::Rng rng(p.seed);
    uint32_t taken[3]{}, suitMates = 0;
    for (const Unit& other : units) {
        if (&other == u || other.prog.run != prun.number) continue;
        if (other.prog.item[0] >= 0) taken[0] |= 1u << other.prog.item[0];
        if (other.prog.item[2] >= 0) taken[1] |= 1u << other.prog.item[2];
        if (other.prog.item[3] >= 0) taken[2] |= 1u << other.prog.item[3];
        if (other.prog.item[1] >= 0) suitMates |= 1u << other.prog.item[1];
    }
    const coop_bot_progress::GunPlan g = loadout ? coop_bot_progress::PlanLoadout(rng, u->favorites, u->favoriteCount, taken[1])
                                                 : coop_bot_progress::PlanProgression(rng, prun.tiers, u->favorites, u->favoriteCount, taken);
    p.item[0] = g.gun[0];
    p.item[2] = g.gun[1];
    p.item[3] = loadout ? -1 : g.gun[2];
    if (u->suitForced >= 0) {
        p.suitOrder[0] = u->suitForced;
        p.suitCount = 1;
    } else {
        p.suitCount = coop_bot_progress::SuitOrder(rng, kSuitKinds, u->suitFavorites, u->suitFavoriteCount, PatientSuit(), suitMates, p.suitOrder,
                                                   coop_bot_progress::kMaxSuits);
    }
    p.item[1] = ownSuits && p.suitCount ? p.suitOrder[0] : -1;
    if (!loadout) {
        const bool waves = prun.kind == Kind::Waves;
        float start[coop_bot_progress::kSteps];
        int slots[coop_bot_progress::kSteps];
        for (int s = 0; s < coop_bot_progress::kSteps; ++s) {
            start[s] = waves ? progressWaves[s] : progressMinutes[s] * 60.0f;
            slots[s] = coop_bot_progress::StaggerSlot(u->index, prun.rotation[s], progressStagger);
        }
        coop_bot_progress::DueTimes(start, slots, waves ? 1.0f : static_cast<float>(progressStaggerSeconds), p.due);
        for (int s = 0; s < coop_bot_progress::kSteps; ++s)
            if (p.due[s] <= 0.0f) p.done |= static_cast<uint8_t>(1u << s);   // that step never comes
    }
    char when[4][24];
    for (int s = 0; s < coop_bot_progress::kSteps; ++s) ProgressDueText(p, s, when[s], sizeof(when[s]));
    if (loadout)
        ProgressEvent("bot %u (%s): %s kit (seed %08X): suit %s, %s + %s", u->index + 1, u->name, prun.mode, p.seed, ProgressItemName(1, p.item[1]),
                      ProgressItemName(0, p.item[0]), ProgressItemName(2, p.item[2]));
    else
        ProgressEvent("bot %u (%s): %s plan run %u (seed %08X): %s %s %s, suit %s %s, %s %s %s, %s %s %s", u->index + 1, u->name, prun.mode, prun.number, p.seed,
                      ProgressStepName(0), ProgressItemName(0, p.item[0]), when[0], ProgressItemName(1, p.item[1]), when[1], ProgressStepName(2),
                      ProgressItemName(2, p.item[2]), when[2], ProgressStepName(3), ProgressItemName(3, p.item[3]), when[3]);
}
void ProgressFail(int s, DWORD now, const char* why) {
    ProgressUnit& p = u->prog;
    const uint8_t bit = static_cast<uint8_t>(1u << s);
    ++p.tries[s];
    InterlockedIncrement(&progressRetries);
    if (p.tries[s] >= kProgressTries) {
        p.failed |= bit;
        InterlockedIncrement(&progressGivenUp);
        ProgressEvent("bot %u (%s): %s %s: given up after %d tries (last: %s)", u->index + 1, u->name, ProgressStepName(s), ProgressItemName(s, p.item[s]),
                      kProgressTries, why);
        return;
    }
    p.retryAt[s] = now + kProgressRetryMs;
    if (p.tries[s] <= 3)
        ProgressEvent("bot %u (%s): %s %s: %s; again in %lu s (try %d of %d)", u->index + 1, u->name, ProgressStepName(s), ProgressItemName(s, p.item[s]), why,
                      static_cast<unsigned long>(kProgressRetryMs / 1000), p.tries[s], kProgressTries);
}
// Not now: a kiosk, a throw, a suit or body check under way, or a fight (until the grace is over).
bool ProgressBusy(int s, DWORD grace, DWORD now) {
    ProgressUnit& p = u->prog;
    uint32_t worn[4]{};
    if (u->kioskVisit != KioskStep::None || u->tkStep != TkStep::None || u->dressCheckAt || !WornSuit(u->entity, worn)) {
        InterlockedIncrement(&progressBusyWaits);
        return true;
    }
    // A suit change leaves it without med packs while the suit streams: not when hurt in a fight.
    const bool hurt = s == coop_bot_progress::kSuitStep && Health01(u->entity) < 0.5f;
    if (u->command.mode == coop_bot::Mode::Combat && (now - p.reachedAt[s] < grace || hurt)) {
        InterlockedIncrement(&progressFightWaits);
        if (!((p.waitLogged >> s) & 1u)) {
            p.waitLogged |= static_cast<uint8_t>(1u << s);
            ProgressEvent("bot %u (%s): %s %s waits for a quiet moment (in a fight; at most %lu s)", u->index + 1, u->name, ProgressStepName(s),
                          ProgressItemName(s, p.item[s]), static_cast<unsigned long>(grace / 1000));
        }
        return true;
    }
    return false;
}
// Another gun of the same tier whose stream is already in, when this one cannot come.
int ProgressAlternative(int s, uint32_t carried, uint32_t qm) {
    const ProgressUnit& p = u->prog;
    uint32_t pool = 0;
    if (prun.kind == Kind::Loadout) pool = s == 2 ? coop_bot_progress::kAllGuns & ~(1u << coop_bot_progress::kPulseRifle) : 0u;
    else pool = (s == 0 ? prun.tiers.cheap : s == 2 ? prun.tiers.expensive : prun.tiers.cheap | prun.tiers.expensive) & ~(1u << coop_bot_progress::kCutter);
    pool &= ~carried;
    for (int k = 0; k < coop_bot_progress::kSteps; ++k)
        if (k != s && coop_bot_progress::GunStep(k) && p.item[k] >= 0) pool &= ~(1u << p.item[k]);
    uint32_t loaded = 0;
    for (int i = 0; i < kGunKinds; ++i) {
        if (!((pool >> i) & 1u)) continue;
        const uint32_t entry = GunEntryRaw(qm, reinterpret_cast<const unsigned char*>(kGunCatalogue[i].guid));
        uint32_t handle = 0;
        if (entry && Read(entry + kGunEntryHandle, &handle, 4) && handle) loaded |= 1u << i;
    }
    coop_bot_progress::Rng rng(coop_bot_progress::Mix(p.seed ^ (static_cast<uint32_t>(s) << 8) ^ p.tries[s]));
    return coop_bot_progress::Pick(rng, loaded, u->favorites, u->favoriteCount, 0);
}
// One gun step: in hand already, or a free grant the store's way. True when one started.
bool ProgressGun(int s, uint32_t carried, int proxies, DWORD now) {
    ProgressUnit& p = u->prog;
    const uint8_t bit = static_cast<uint8_t>(1u << s);
    int idx = p.item[s];
    if ((carried >> idx) & 1u) {
        p.done |= bit;
        p.granted |= 1u << idx;
        ProgressEvent("bot %u (%s): %s %s: already in its hands", u->index + 1, u->name, ProgressStepName(s), ProgressItemName(s, idx));
        return false;
    }
    if (u->gunPending >= 0 || proxies) return false;   // another gun is on its way
    if (ProgressBusy(s, kProgressGunGraceMs, now)) return false;
    uint32_t qm = 0;
    if (Read(base + kQuartermasterRva, &qm, 4) && qm) RaiseStreamBudget(qm);   // the campaign arenas start at 4
    char streams[64];
    qm = ProgressStreams(streams, sizeof(streams));
    if (!qm) { ProgressFail(s, now, "no Quartermaster"); return false; }
    const bool entry = GunEntryRaw(qm, reinterpret_cast<const unsigned char*>(kGunCatalogue[idx].guid)) != 0;
    if (!entry || !StreamRoom(qm, idx)) {
        InterlockedIncrement(entry ? &progressNoStream : &progressNoEntry);
        const int alt = ProgressAlternative(s, carried, qm);
        if (alt < 0) {
            char why[128];
            std::snprintf(why, sizeof(why), "%s (%s), no other gun of its kind loaded", entry ? "no stream slot" : "no Quartermaster entry on this map", streams);
            ProgressFail(s, now, why);
            return false;
        }
        InterlockedIncrement(&progressAlternatives);
        ProgressEvent("bot %u (%s): %s %s: %s (%s); takes the %s, its stream is in", u->index + 1, u->name, ProgressStepName(s), ProgressItemName(s, idx),
                      entry ? "no stream slot" : "no Quartermaster entry on this map", streams, kGunCatalogue[alt].name);
        p.item[s] = idx = alt;
    }
    if (ThisBool0(base + kInventoryIsFullRva, u->entity + kWeaponInventory) != 0) {
        InterlockedIncrement(&progressSlotFull);
        ProgressFail(s, now, "slot full (four weapon slots)");
        return false;
    }
    p.gunWhy[0] = 0;
    if (!StartGun(idx, false, now)) { ProgressFail(s, now, p.gunWhy[0] ? p.gunWhy : "StartGun refused"); return false; }
    p.pendingStep = s;
    p.pendingEntity = u->entity;
    p.startedAt[s] = now;
    InterlockedIncrement(&progressStarted);
    char at[32];
    ProgressNowText(at, sizeof(at));
    ProgressEvent("bot %u (%s): %s %s, %s %s: free grant started (GunProxy into a slot, RequisitionGun), %s", u->index + 1, u->name, ProgressStepName(s),
                  ProgressItemName(s, idx), prun.mode, at, streams);
    return true;
}
// The suit step: its pick, or the next one that is registered here. True when a change started.
bool ProgressSuit(uint32_t leader, int s, DWORD now) {
    ProgressUnit& p = u->prog;
    const uint8_t bit = static_cast<uint8_t>(1u << s);
    const int target = p.item[s];
    if (WornSuitIndex(u->entity) == target) {
        p.done |= bit;
        u->suitChosen = target;
        ProgressEvent("bot %u (%s): suit %s: already worn", u->index + 1, u->name, ProgressItemName(s, target));
        return false;
    }
    if (coop_uniform::SuitIndex(kSuits[target].guid) < 0) {
        InterlockedIncrement(&progressUnregistered);
        ++p.suitPos;
        const int next = p.suitPos < p.suitCount ? p.suitOrder[p.suitPos] : -1;
        ProgressEvent("bot %u (%s): suit %s is not registered on this map; %s %s", u->index + 1, u->name, ProgressItemName(s, target), next >= 0 ? "next" : "none left",
                      next >= 0 ? SuitName(next) : "");
        if (next < 0) { p.failed |= bit; InterlockedIncrement(&progressGivenUp); }
        else p.item[s] = next;
        return false;
    }
    // Its body is not dressed yet: its first dressing (PutOnOwnSuit) takes this suit.
    if (!u->suitDone) {
        u->suitChosen = target;
        p.pendingStep = s;
        p.pendingEntity = u->entity;
        p.startedAt[s] = now;
        InterlockedIncrement(&progressSuitStarts);
        ProgressEvent("bot %u (%s): suit %s: its first dressing takes it", u->index + 1, u->name, ProgressItemName(s, target));
        return true;
    }
    if (!leader || u->gunPending >= 0) return false;
    if (ProgressBusy(s, kProgressSuitGraceMs, now)) return false;
    char at[32];
    ProgressNowText(at, sizeof(at));
    const bool dressed = DressIn(leader, target, prun.kind == Kind::Loadout ? "its kit for this mode" : "progression", now);
    InterlockedIncrement(&progressSuitStarts);
    ProgressEvent("bot %u (%s): suit %s, %s %s: ChangeToUniform %s", u->index + 1, u->name, ProgressItemName(s, target), prun.mode, at, dressed ? "called" : "FAILED");
    if (!dressed) { ProgressFail(s, now, "ChangeToUniform faulted"); return false; }
    p.pendingStep = s;
    p.pendingEntity = u->entity;
    p.startedAt[s] = now;
    return true;
}
// A gun or suit on its way: arrived, still coming, or given up. True when settled.
bool ProgressPending(uint32_t carried, DWORD now) {
    ProgressUnit& p = u->prog;
    const int s = p.pendingStep;
    const uint8_t bit = static_cast<uint8_t>(1u << s);
    if (p.pendingEntity != u->entity) { p.pendingStep = -1; return true; }   // a new body: it tries again
    const int item = p.item[s];
    char at[32];
    ProgressNowText(at, sizeof(at));
    if (coop_bot_progress::GunStep(s)) {
        if ((carried >> item) & 1u) {
            p.done |= bit;
            p.granted |= 1u << item;
            p.pendingStep = -1;
            InterlockedIncrement(&progressArrived);
            ProgressEvent("bot %u (%s): %s %s: in its hands after %lu ms (%s %s)", u->index + 1, u->name, ProgressStepName(s), ProgressItemName(s, item),
                          static_cast<unsigned long>(now - p.startedAt[s]), prun.mode, at);
            return true;
        }
        if (u->gunPending >= 0) return false;   // PollGun waits for the stream (20 s at most)
        p.pendingStep = -1;
        ProgressFail(s, now, p.gunWhy[0] ? p.gunWhy : "the gun did not arrive");
        return true;
    }
    if (WornSuitIndex(u->entity) == item) {
        p.done |= bit;
        p.pendingStep = -1;
        InterlockedIncrement(&progressSuitsInPlace);
        ProgressEvent("bot %u (%s): suit %s: worn after %lu ms (%s %s)", u->index + 1, u->name, ProgressItemName(s, item), static_cast<unsigned long>(now - p.startedAt[s]),
                      prun.mode, at);
        return true;
    }
    if (now - p.startedAt[s] < kProgressSuitWaitMs) return false;
    p.pendingStep = -1;
    ProgressFail(s, now, WornSuitIndex(u->entity) < 0 ? "still no suit on after 25 s (streaming)" : "another suit went on (the body check)");
    return true;
}
// Per bot, after its gun poll: due steps one at a time.
void ProgressTick(uint32_t leader, DWORD now) {
    const Kind kind = prun.kind;
    if (!coop_bot_progress::Active(kind) || !gunsReadyToBuy || (kind == Kind::Loadout ? !modeLoadoutWanted : !progressionWanted)) return;
    if (!u->entity || u->state != State::Active) return;
    ProgressUnit& p = u->prog;
    if (p.run != prun.number) ProgressPlan();
    if (now - p.checkAt < kProgressCheckMs) return;
    p.checkAt = now;
    uint32_t gunAt[kGunKinds]{};
    int proxies = 0;
    const uint32_t carried = CarriedGuns(u->entity, gunAt, &proxies);
    if (p.pendingStep >= 0 && !ProgressPending(carried, now)) return;
    bool started = false;
    for (int s = 0; s < coop_bot_progress::kSteps && !started && p.pendingStep < 0; ++s) {
        const uint8_t bit = static_cast<uint8_t>(1u << s);
        const int item = p.item[s];
        if (p.failed & bit) continue;
        if (p.done & bit) {
            // Kept for its next body (ReissueGuns, PutOnOwnSuit), whatever reset the owned list.
            if (item >= 0 && (p.reached & bit)) {
                if (coop_bot_progress::GunStep(s)) u->gunsOwned |= 1u << item;
                else u->suitChosen = item;
            }
            continue;
        }
        if (item < 0) { p.done |= bit; continue; }
        if (!(p.reached & bit)) {
            const bool due = kind == Kind::Loadout || (kind == Kind::Waves ? static_cast<float>(prun.completed) >= p.due[s]
                                                                            : static_cast<double>(prun.clockMs) >= static_cast<double>(p.due[s]) * 1000.0);
            if (!due) continue;
            p.reached |= bit;
            p.reachedAt[s] = now;
            InterlockedIncrement(&progressReached);
            char at[32];
            ProgressNowText(at, sizeof(at));
            ProgressEvent("bot %u (%s): %s %s: %s %s is due", u->index + 1, u->name, prun.mode, at, ProgressStepName(s), ProgressItemName(s, item));
        }
        if (static_cast<int32_t>(now - p.retryAt[s]) < 0) continue;
        started = coop_bot_progress::GunStep(s) ? ProgressGun(s, carried, proxies, now) : ProgressSuit(leader, s, now);
    }
    p.ownedSeen = u->gunsOwned;
    p.suitSeen = u->suitChosen;
}
void InstallProgression(FILE* log, const std::string& ini) {
    const char* f = ini.c_str();
    progressionWanted = GetPrivateProfileIntA("Bot", "Progression", 1, f) != 0;
    modeLoadoutWanted = GetPrivateProfileIntA("Bot", "ModeLoadout", 1, f) != 0;
    char text[96] = "";
    GetPrivateProfileStringA("Bot", "ProgressionWaves", "", text, sizeof(text), f);
    coop_bot_progress::ParseList(text, progressWaves, coop_bot_progress::kSteps);
    GetPrivateProfileStringA("Bot", "ProgressionMinutes", "", text, sizeof(text), f);
    coop_bot_progress::ParseList(text, progressMinutes, coop_bot_progress::kSteps);
    const int stagger = static_cast<int>(GetPrivateProfileIntA("Bot", "ProgressionStagger", 2, f));
    progressStagger = stagger < 0 ? 0 : (stagger > 5 ? 5 : stagger);
    const int seconds = static_cast<int>(GetPrivateProfileIntA("Bot", "ProgressionStaggerSeconds", 45, f));
    progressStaggerSeconds = seconds < 0 ? 0 : (seconds > 600 ? 600 : seconds);
    const uint32_t seed = static_cast<uint32_t>(GetPrivateProfileIntA("Bot", "ProgressionSeed", 0, f));
    progressSessionSeed = coop_bot_progress::Mix(seed ? seed : GetTickCount() ^ (GetCurrentProcessId() << 16));
    const bool catalogue = GunIndexByName("PlasmaCutter") == coop_bot_progress::kCutter && GunIndexByName("PulseRifle") == coop_bot_progress::kPulseRifle;
    if (!catalogue) {
        progressionWanted = modeLoadoutWanted = false;
        std::fprintf(log, "bot progression OFF: the gun catalogue no longer starts with the plasma cutter and the pulse rifle\n");
    }
    std::fprintf(log, "bot progression=%d mode_loadout=%d waves=%g,%g,%g,%g minutes=%g,%g,%g,%g stagger=%d waves or %d s seed=%08X (%s) needs buy_guns=%d suits=%d\n",
                 progressionWanted ? 1 : 0, modeLoadoutWanted ? 1 : 0, progressWaves[0], progressWaves[1], progressWaves[2], progressWaves[3], progressMinutes[0],
                 progressMinutes[1], progressMinutes[2], progressMinutes[3], progressStagger, progressStaggerSeconds, progressSessionSeed,
                 seed ? "fixed" : "new each session", gunsReadyToBuy ? 1 : 0, ownSuits ? 1 : 0);
    std::fprintf(log, "bot progression steps: Mercenaries and Versus at once (suit, Pulse Rifle, one more gun); survival, arenas, Objective and Hold the Zone: "
                      "cheaper gun, suit, expensive gun, fourth gun; never in the campaign\n");
}

// [Bot] InfiniteAmmo=1: a bot never runs dry. Its reserve for each gun it
// carries is kept at three clips: the stack it has is topped up, a missing one
// is handed over the store's way (spawn, Pickup) without a price, and only
// without those functions is an empty clip filled straight. It still reloads.
bool infiniteAmmo = false;
volatile LONG ammoTopUps = 0, ammoGifts = 0, clipRefills = 0;
constexpr uint32_t kSpareAmmoVtableRva = 0x174F908, kItemAmmoType = 0x2F0;
bool GiveFree(const unsigned char* guid) {
    if (CanFitRaw(guid, u->entity + kGeneralInventory) <= 0) return false;
    const uint32_t item = SpawnPickupRaw(guid);
    uint32_t vtable = 0, fn = 0;
    if (!item) return false;
    if (!Read(item, &vtable, 4) || !Read(vtable + kPickupSlot, &fn, 4) || fn < base + 0x1000 || fn >= base + gate::kRdataLo) { DeleteRaw(item); return false; }
    const int whole = PickupRaw(item, fn, u->entity);
    if (whole == 0) DeleteRaw(item);
    return whole > 0;
}
void KeepAmmo(DWORD now) {
    if (!infiniteAmmo || now - u->ammoCheckAt < 1000) return;
    u->ammoCheckAt = now;
    uint32_t guns[8], items[64], qm = 0;
    const unsigned gunCount = GunsOf(u->entity, guns, 8), itemCount = ItemsOf(u->entity, items, 64);
    Read(base + kQuartermasterRva, &qm, 4);
    for (unsigned g = 0; g < gunCount; ++g) {
        uint32_t attributes = 0, type = 0;
        int32_t capacity = 0;
        if (!Read(guns[g] + kGunAttributes, &attributes, 4) || !attributes || !Read(attributes + kGunAmmoType, &type, 4)) continue;
        if (!Read(guns[g] + kGunCapacity, &capacity, 4) || capacity <= 0 || capacity > 500) capacity = 20;
        const uint32_t want = static_cast<uint32_t>(capacity) * 3u;
        uint32_t stack = 0;
        for (unsigned i = 0; i < itemCount && !stack; ++i) {
            uint32_t vtable = 0, itemAttributes = 0, itemType = 0;
            if (Read(items[i], &vtable, 4) && vtable == base + kSpareAmmoVtableRva && Read(items[i] + kGunAttributes, &itemAttributes, 4) && itemAttributes &&
                Read(itemAttributes + kItemAmmoType, &itemType, 4) && itemType == type)
                stack = items[i];
        }
        uint32_t amount = 0;
        if (stack && Read(stack + kItemAmount, &amount, 4)) {
            if (amount < want) { WriteBytes(stack + kItemAmount, &want, 4); InterlockedIncrement(&ammoTopUps); }
            continue;
        }
        unsigned char proxy[16]{}, drop[16]{};
        const uint32_t entry = storeReady && qm && Read(guns[g] + kGunProxyGuid, proxy, 16) ? GunEntryRaw(qm, proxy) : 0;
        if (entry && Read(entry + kGunEntryDrop, drop, 16) && GiveFree(drop)) {
            if (InterlockedIncrement(&ammoGifts) <= 20) gate::Event("bot %u: handed a stack of ammo for its gun in slot %u", u->index + 1, g);
            continue;
        }
        int32_t clip = 0;
        if (Read(guns[g] + kGunClip, &clip, 4) && clip == 0) { WriteBytes(guns[g] + kGunClip, &capacity, 4); InterlockedIncrement(&clipRefills); }
    }
}

// --- using the kiosks --------------------------------------------------------------
// After the leader leaves a bench or a store, the bots use it too, one at a time,
// the way he does: to the kiosk's dock point, facing it, until the game names it
// the bot's nearby usable (Player+0x3F4 == kiosk+0x230: its own range, facing and
// sight test), one press of use (0x0020), and the game pushes its own
// PlayerUpgradeMainSM / PlayerStoreSM for the bot, docks it and plays the real
// animation. Inside, at the menu, the bot does its purchases (Invest / Shop
// above), lingers a moment and backs out: the menus read Back (0x0080) straight
// from the pad record, and only when its "connected" byte (+0x3E) is set, so that
// byte is on for the few frames of each press. Kiosks are found in the
// TargetingManager's usable list ([0x020212EC]: lists at +0xC, node target +8 =
// entity+0x10, next +0x28). The kiosk SM is a child in the Player's machine tree
// (root [player+0x24C], child +0x20, sibling +0x28, state index +0x0C).
// Both SMs switch the global input context of devices 4 and 5 (keyboard and
// mouse: store 7, bench 9) through push 0x0043D380 / pop 0x0043D3A0 (cdecl(id));
// for a bot's SM those five calls are skipped, so the human's controls stay his.
constexpr uint32_t kBenchVtableRva = 0x1741F08, kStoreVtableRva = 0x1742DA0, kTargetingRva = 0x1C212EC, kUsable = 0x230;
constexpr uint32_t kBenchInUse = 0x291, kBenchOutro = 0x292, kStoreInUse = 0x271, kNearbyUsable = 0x3F4;
constexpr uint32_t kStoreSmVtableRva = 0x175E360, kBenchSmVtableRva = 0x1729188, kStoreSmKiosk = 0xAF8, kBenchSmKiosk = 0x1294;
constexpr uint32_t kMachineRoot = 0x24C, kMachineChild = 0x20, kMachineSibling = 0x28, kMachineState = 0x0C;
constexpr uint32_t kOriginalUniform = 0x176C, kWeaponSwitchMsgSent = 0xCCC;
constexpr uint32_t kContextPushRva = 0x3D380, kContextPopRva = 0x3D3A0, kContextStackRva = 0x1C21D34;
// KioskEnter=1 lets the bot really open the kiosk (experimental). Twice on 28.09.
// a bot's store never let it out again and took the human's keyboard and mouse
// with it; by default the bot walks up, looks at the kiosk a few seconds ("browses")
// and buys without a menu.
bool kioskWanted = false, kioskReady = false, kioskEnter = false;
safetyhook::InlineHook contextPushHook, contextPopHook;
uintptr_t contextPushTrampoline = 0, contextPopTrampoline = 0;
uint32_t contextEsi = 0, contextEbx = 0, contextReturn = 0, contextId = 0;
unsigned char contextIsPush = 0;
// Pushes skipped for bots, per context id: a pop is only skipped against one, so
// the human's stack can never lose an entry. Bodies let go of stay "bots" here,
// since a kiosk SM's destructor may run after the body left the bot list.
LONG skippedPushes[16]{};
bool WasBot(uint32_t entity) {
    if (!entity) return false;
    if (IsBotEntity(entity)) return true;
    for (uint32_t b : pastBots) if (b == entity) return true;
    return false;
}
volatile LONG contextSkips = 0, kioskVisits = 0, kioskEntries = 0, kioskExits = 0, kioskAborts = 0, kioskFallbacks = 0;
// The calls of the kiosk code into the context push and pop (return addresses;
// all callers of the thunks 0x004036C0 and 0x0040280B in the bench and store
// code). The first five were known from the start (store push with esi = owner,
// bench push, bench exit pop, bench and store destructor pops); 28.09. a bot's
// store stayed in its menu (state 51), no push was skipped (context_skips=0) and
// the human's keyboard and mouse went to the bot's store. The store's menus push
// more contexts (3, 8, 14) from more places, and esi is not always the owner.
// So a call from the kiosk code is also skipped while a bot is at a kiosk and
// the leader is not; pops only against pushes held back, per id.
constexpr uint32_t kContextPushSites[] = {0xAE4B20, 0x4AAE6F, 0xAC4527, 0xAD6A20, 0xAE1C10};
constexpr uint32_t kContextPopSites[] = {0x4AAE0E, 0x4A5683, 0xAD3E81, 0xAC4671, 0xACE5FF, 0xAD6A90, 0xAD6C24, 0xAD6C42, 0xADC7D1, 0x49E766};
volatile LONG contextLines = 0, contextKept = 0, contextByBot = 0, contextBotLines = 0;
uint32_t KioskMachine(uint32_t player, int32_t& state, bool& store);
bool BotAtKiosk() {
    for (const Unit& unit : units)
        if (unit.entity && (unit.kioskVisit == KioskStep::Press || unit.kioskVisit == KioskStep::Inside || unit.kioskVisit == KioskStep::Leave)) return true;
    return false;
}
extern "C" bool __cdecl SkipBotContext() {
    const uint32_t site = contextReturn - static_cast<uint32_t>(base);
    bool push = false, pop = false;
    for (uint32_t k : kContextPushSites) push = push || k == site;
    for (uint32_t k : kContextPopSites) pop = pop || k == site;
    // Any push or pop from a bot's code, kiosk site or not: the stacks are shared
    // with the human's keyboard and mouse, and a pop takes whatever is on top.
    // Owner checked in esi and ebx, as the entity or a machine's owner at +0x50.
    {
        uint32_t who = 0;
        for (const uint32_t reg : {contextEsi, contextEbx}) {
            uint32_t owner = 0;
            if (!who && reg && WasBot(reg)) who = reg;
            else if (!who && reg && Read(reg + kBenchOwner, &owner, 4) && WasBot(owner)) who = owner;
        }
        if (who) {
            InterlockedIncrement(&contextByBot);
            if (!push && !pop && InterlockedIncrement(&contextBotLines) <= 40)
                gate::Event("bot input context %s %u from %08X by bot %08X (not a kiosk site): passed, kiosk_bot=%d", contextIsPush ? "push" : "pop",
                            contextId, site, who, BotAtKiosk() ? 1 : 0);
        }
    }
    if ((!push && !pop) || contextId >= 16) return false;
    uint32_t owner = 0;
    bool bot = contextEsi && (WasBot(contextEsi) || (Read(contextEsi + kBenchOwner, &owner, 4) && WasBot(owner)));
    const bool atKiosk = BotAtKiosk();
    if (!bot && atKiosk) {
        int32_t state = -1;
        bool store = false;
        const uint32_t leader = Leader();
        bot = !leader || !KioskMachine(leader, state, store);   // the leader is not at a kiosk himself
    }
    bool skip = false;
    if (bot && push) { ++skippedPushes[contextId]; skip = true; }
    else if (bot && pop && skippedPushes[contextId] > 0) { --skippedPushes[contextId]; skip = true; }
    if (skip) InterlockedIncrement(&contextSkips);
    else if (atKiosk) InterlockedIncrement(&contextKept);
    if (atKiosk && InterlockedIncrement(&contextLines) <= 80)
        gate::Event("bot input context %s %u from %08X (esi %08X): %s", push ? "push" : "pop", contextId, site, contextEsi, skip ? "skipped (a bot's)" : "passed");
    return skip;
}
__declspec(naked) void OnContextPush() {
    __asm {
        mov contextIsPush, 1
        mov contextEbx, ebx
        mov contextEsi, esi
        mov eax, [esp]
        mov contextReturn, eax
        mov eax, [esp + 4]
        mov contextId, eax
        call SkipBotContext
        test al, al
        jnz skip
        jmp dword ptr [contextPushTrampoline]
    skip:
        ret
    }
}
__declspec(naked) void OnContextPop() {
    __asm {
        mov contextIsPush, 0
        mov contextEbx, ebx
        mov contextEsi, esi
        mov eax, [esp]
        mov contextReturn, eax
        mov eax, [esp + 4]
        mov contextId, eax
        call SkipBotContext
        test al, al
        jnz skip
        jmp dword ptr [contextPopTrampoline]
    skip:
        ret
    }
}

// The kiosk SM in a Player's machine tree, and its state.
uint32_t KioskMachine(uint32_t player, int32_t& state, bool& store) {
    uint32_t stack[64], rootSm = 0;
    int n = 0, visited = 0;
    if (!Read(player + kMachineRoot, &rootSm, 4) || !rootSm) return 0;
    stack[n++] = rootSm;
    while (n && visited < 256) {
        const uint32_t sm = stack[--n];
        ++visited;
        uint32_t vtable = 0, child = 0, sibling = 0;
        if (!Read(sm, &vtable, 4)) continue;
        if (vtable == base + kStoreSmVtableRva || vtable == base + kBenchSmVtableRva) {
            store = vtable == base + kStoreSmVtableRva;
            if (!Read(sm + kMachineState, &state, 4)) state = -1;
            return sm;
        }
        if (Read(sm + kMachineChild, &child, 4) && child && n < 64) stack[n++] = child;
        if (Read(sm + kMachineSibling, &sibling, 4) && sibling && n < 64) stack[n++] = sibling;
    }
    return 0;
}
// The kiosk an SM of the leader works at (its SafePtr, object = value - 0x10).
uint32_t KioskOf(uint32_t sm, bool store) {
    uint32_t ref = 0, vtable = 0;
    if (!Read(sm + (store ? kStoreSmKiosk : kBenchSmKiosk), &ref, 4) || ref < 0x10010) return 0;
    const uint32_t kiosk = ref - 0x10;
    return Read(kiosk, &vtable, 4) && vtable == base + (store ? kStoreVtableRva : kBenchVtableRva) ? kiosk : 0;
}
bool KioskBusy(uint32_t kiosk, bool store) {
    unsigned char a = 0, b = 0;
    if (store) return Read(kiosk + kStoreInUse, &a, 1) && a;
    return (Read(kiosk + kBenchInUse, &a, 1) && a) || (Read(kiosk + kBenchOutro, &b, 1) && b);
}
// Where to stand: the kiosk's docking offset (bench attrs +0x9C/+0xA0, store
// +0xDC/+0xE0) in its matrix (rows at +0xD0 .. +0x100); without one, a step in
// front along its third row, or behind it on the second try.
Vec3 DockPoint(uint32_t kiosk, bool store, int side) {
    float m[16]{};
    uint32_t attributes = 0;
    unsigned char use = 0;
    Read(kiosk + 0xD0, m, sizeof(m));
    const Vec3 r0 = {m[0], m[1], m[2]}, r1 = {m[4], m[5], m[6]}, r2 = {m[8], m[9], m[10]}, at = {m[12], m[13], m[14]};
    if (side == 0 && Read(kiosk + kGunAttributes, &attributes, 4) && attributes && Read(attributes + (store ? 0xDC : 0x9C), &use, 1) && use) {
        float off[16]{};
        if (Read(attributes + (store ? 0xE0 : 0xA0), off, sizeof(off)) && std::isfinite(off[12]) && std::fabs(off[12]) + std::fabs(off[14]) < 5.0f)
            return coop_bot::Add(at, coop_bot::Add(coop_bot::Add(coop_bot::Scale(r0, off[12]), coop_bot::Scale(r1, off[13])), coop_bot::Scale(r2, off[14])));
    }
    return coop_bot::Add(at, coop_bot::Scale(r2, side == 1 ? -1.1f : 1.1f));
}
// Where the bot stands at the kiosk: straight in front of it, 1.25 m out along
// its facing (the third row of its matrix), not toward the dock point: that lies
// a little to one side (29.09.: the bot stood beside the store, not before it).
// Which end of the facing is the front, the dock point tells (its part along the
// facing); the second try is the other end, the third the front again.
constexpr float kKioskStand = 1.25f;
Vec3 StandPoint(uint32_t kiosk, bool store, int side) {
    float m[16]{};
    Read(kiosk + 0xD0, m, sizeof(m));
    const Vec3 at = {m[12], m[13], m[14]};
    Vec3 front = {m[8], 0.0f, m[10]};
    float len = coop_bot::Length(front);
    if (len < 0.05f) { front = {0.0f, 0.0f, 1.0f}; len = 1.0f; }
    front = coop_bot::Scale(front, 1.0f / len);
    const Vec3 dock = coop_bot::Sub(DockPoint(kiosk, store, 0), at);
    float sign = coop_bot::Dot({dock.x, 0.0f, dock.z}, front) < 0.0f ? -1.0f : 1.0f;
    if (side == 1) sign = -sign;
    return coop_bot::Add(at, coop_bot::Scale(front, sign * kKioskStand));
}

struct KioskCall { uint32_t kiosk = 0; bool store = false; DWORD at = 0; unsigned done = 0; };
KioskCall kioskCall;   // the kiosk the leader last left; each bot visits it once
// The store room the leader is in ("store rooms", below): its kiosks and which bots had their turn there.
struct StoreRoom {
    uint32_t store = 0, bench = 0;
    Vec3 storeAt, benchAt;
    bool in = false, sight = true;
    DWORD scanAt = 0, leftAt = 0;
    unsigned storeDone = 0, benchDone = 0;
};
StoreRoom room;
bool storeRooms = false;   // [Bot] StoreRooms
volatile LONG roomEntries = 0, roomExits = 0, roomVisits = 0, instantStores = 0, instantBenches = 0, roomScanFaults = 0;
// The bot in turn had its turn at this kiosk (0: at whichever of that kind its room has).
void NoteKioskTurn(uint32_t kiosk, bool store) {
    const unsigned bit = 1u << u->index;
    if (store && (!kiosk || kiosk == room.store)) room.storeDone |= bit;
    if (!store && (!kiosk || kiosk == room.bench)) room.benchDone |= bit;
}
// The leader leaves a kiosk (from the SM destructor hooks, before the SM goes).
bool QueueKiosk(uint32_t sm, bool store) {
    if (!kioskReady) return false;
    const uint32_t kiosk = KioskOf(sm, store);
    if (!kiosk) return false;
    kioskCall = KioskCall{kiosk, store, GetTickCount(), 0};
    return true;
}
void EndVisit(const char* how, bool fallback) {
    if (u->kioskVisit != KioskStep::None)
        gate::Event("bot %u: kiosk %08X (%s): %s", u->index + 1, u->kiosk, u->kioskStore ? "store" : "bench", how);
    if (fallback) {
        InterlockedIncrement(&kioskFallbacks);
        if (u->kioskStore) { Shop(Leader()); StoreSuit(Leader(), GetTickCount()); } else Invest(Leader());
        NoteKioskTurn(u->kiosk, u->kioskStore);
    }
    if (!u->kiosk || u->kiosk == kioskCall.kiosk) kioskCall.done |= 1u << u->index;
    u->kioskVisit = KioskStep::None;
    u->kiosk = 0;
    Unplug();
}
// A bot's turn at a kiosk: the one the leader just left, else its own turn in the
// store room he is in; when it is calm and no mate is at that kiosk.
void MaybeStartVisit(DWORD now) {
    if (!kioskReady || u->kioskVisit != KioskStep::None || u->holding) return;
    if (u->flying || u->zeroG) return;
    const unsigned bit = 1u << u->index;
    uint32_t kiosk = 0;
    bool store = false, fromRoom = false;
    if (kioskCall.kiosk && !(kioskCall.done & bit)) {
        const bool hadTurn = (kioskCall.kiosk == room.store && (room.storeDone & bit)) || (kioskCall.kiosk == room.bench && (room.benchDone & bit));
        if (now - kioskCall.at > 120000 || hadTurn) kioskCall.done |= bit;
        else { kiosk = kioskCall.kiosk; store = kioskCall.store; }
    }
    if (!kiosk && storeRooms && room.in) {
        if (room.store && !(room.storeDone & bit)) { kiosk = room.store; store = true; fromRoom = true; }
        else if (room.bench && !(room.benchDone & bit)) { kiosk = room.bench; fromRoom = true; }
    }
    if (!kiosk) return;
    if (u->command.mode == coop_bot::Mode::Combat || u->state != State::Active) return;
    for (const Unit& other : units)
        if (&other != u && other.kioskVisit != KioskStep::None && (kioskEnter || other.kiosk == kiosk)) return;   // one bot per kiosk
    {
        uint32_t worn[4]{};
        if (ownSuits && !WornSuit(u->entity, worn)) return;   // a suit still streams in
    }
    {
        // Not while the leader stands at it or uses it (their menus got mixed up, 28.09.);
        // browsing, the other kind of kiosk is fine meanwhile.
        Vec3 at;
        int32_t state = -1;
        bool leaderStore = false;
        const uint32_t leader = Leader();
        const uint32_t leaderSm = leader ? KioskMachine(leader, state, leaderStore) : 0;
        if (!leader || (leaderSm && (kioskEnter || leaderStore == store)) || KioskBusy(kiosk, store) ||
            (bot_sense::Pos(kiosk, at) && bot_sense::Pos(leader, leaderAt) && coop_bot::Flat(at, leaderAt) < 3.0f))
            return;
    }
    uint32_t vtable = 0;
    unsigned char guid[16]{};
    if (!Read(kiosk, &vtable, 4) || vtable != base + (store ? kStoreVtableRva : kBenchVtableRva)) {
        if (kiosk == kioskCall.kiosk) kioskCall.kiosk = 0;
        return;
    }
    if (store && Read(u->entity + kOriginalUniform, guid, 16)) {
        bool clear = true;
        for (unsigned char b : guid) clear = clear && !b;
        if (!clear) { u->kiosk = kiosk; u->kioskStore = true; u->kioskVisit = KioskStep::Approach; EndVisit("a suit change is pending: not sent in", true); return; }
    }
    if (fromRoom) InterlockedIncrement(&roomVisits);
    u->kiosk = kiosk;
    u->kioskStore = store;
    u->kioskSide = 0;
    u->kioskVisit = KioskStep::Approach;
    u->kioskStepAt = now;
    u->kioskPresses = 0;
    u->kioskBought = false;
    u->kioskInward = false;
    InterlockedIncrement(&kioskVisits);
    const Vec3 dock = DockPoint(u->kiosk, u->kioskStore, 0), stand = StandPoint(u->kiosk, u->kioskStore, 0);
    gate::Event("bot %u: goes to the %s %08X, dock (%.1f %.1f %.1f), stands at (%.1f %.1f %.1f)", u->index + 1, u->kioskStore ? "store" : "bench",
                u->kiosk, dock.x, dock.y, dock.z, stand.x, stand.y, stand.z);
}
// One frame of a visit. Returns false when the brain should run instead.
bool KioskTick(const Vec3& self, const Vec3& leaderPos, DWORD now) {
    if (u->kioskVisit == KioskStep::None) return false;
    int32_t state = -1;
    bool store = false;
    const uint32_t sm = KioskMachine(u->entity, state, store);
    Vec3 kioskAt;
    if (!bot_sense::Pos(u->kiosk, kioskAt)) { EndVisit("the kiosk is gone", !sm); return false; }
    const Vec3 head = {kioskAt.x, kioskAt.y + 1.3f, kioskAt.z};
    coop_bot::Command c;
    c.mode = coop_bot::Mode::Idle;
    switch (u->kioskVisit) {
    case KioskStep::Approach: {
        if (u->seen.enemyCount > 0) { EndVisit("a fight came first", false); return false; }
        if (now - u->kioskStepAt > 25000) { EndVisit("never got there", true); return false; }
        // In from outside, the last stretch at the kiosk, so the body faces it.
        // There when at the spot, when the way ends short of it close to the kiosk,
        // or as soon as the game offers the kiosk anyway.
        const Vec3 stand = StandPoint(u->kiosk, u->kioskStore, u->kioskSide);
        Vec3 out = {stand.x - kioskAt.x, 0.0f, stand.z - kioskAt.z};
        const float len = coop_bot::Length(out);
        const Vec3 outside = len > 0.1f ? coop_bot::Add(stand, coop_bot::Scale(out, 0.8f / len)) : stand;
        uint32_t offered = 0;
        Read(u->entity + kNearbyUsable, &offered, 4);
        const bool shortOfIt = now - u->routeBlockedAt < 500 && coop_bot::Flat(self, kioskAt) < 2.3f;
        const bool blockedNow = now - u->routeBlockedAt < 500;
        if (!u->kioskInward && (coop_bot::Flat(self, outside) < 0.5f || coop_bot::Flat(self, kioskAt) < kKioskStand + 0.3f || blockedNow)) {
            u->kioskInward = true;
            u->routeBlockedAt = 0;   // that was the way out; the way in starts fresh
        }
        if (offered == u->kiosk + kUsable || (u->kioskInward && (coop_bot::Flat(self, stand) < 0.6f || shortOfIt))) {
            gate::Event("bot %u: at the %s, %.1f m from it (%s)", u->index + 1, u->kioskStore ? "store" : "bench", coop_bot::Flat(self, kioskAt),
                        offered == u->kiosk + kUsable ? "offered" : (shortOfIt ? "as close as the way goes" : "on the spot"));
            u->kioskVisit = KioskStep::Align;
            u->kioskStepAt = now;
            break;
        }
        c.move = true;
        c.moveTo = u->kioskInward ? stand : outside;
        if (u->kioskInward) { c.look = true; c.lookAt = head; }
        break;
    }
    case KioskStep::Align: {
        uint32_t nearby = 0;
        Read(u->entity + kNearbyUsable, &nearby, 4);
        c.look = true;
        c.lookAt = head;
        if (KioskBusy(u->kiosk, u->kioskStore)) { if (now - u->kioskStepAt > 10000) { EndVisit("it stayed busy", true); return false; } break; }
        if (!kioskEnter) {
            // Facing it a moment, then "browsing": no menu, the purchases at the end.
            if (now - u->kioskStepAt >= 400) {
                u->kioskVisit = KioskStep::Browse;
                u->kioskStepAt = now;
                u->kioskLeaveAt = now + 1200 + (now * 2654435761u >> 20) % 1300u;
                InterlockedIncrement(&kioskEntries);
                gate::Event("bot %u: browses the %s", u->index + 1, u->kioskStore ? "store" : "bench");
            }
            break;
        }
        if (nearby == u->kiosk + kUsable) {
            u->kioskVisit = KioskStep::Press;
            u->kioskStepAt = now;
            break;
        }
        if (now - u->kioskStepAt > 3500) {
            gate::Event("bot %u: the %s does not offer itself (nearby usable %08X, want %08X)", u->index + 1, u->kioskStore ? "store" : "bench", nearby,
                        u->kiosk + kUsable);
            if (u->kioskSide < 2) { ++u->kioskSide; u->kioskInward = false; u->kioskVisit = KioskStep::Approach; u->kioskStepAt = now; break; }
            EndVisit("it never offered itself", true);
            return false;
        }
        break;
    }
    case KioskStep::Press:
        if (sm) {
            u->kioskVisit = KioskStep::Inside;
            u->kioskStepAt = now;
            InterlockedIncrement(&kioskEntries);
            gate::Event("bot %u: in the %s (SM %08X, state %d)", u->index + 1, store ? "store" : "bench", sm, state);
            break;
        }
        if (now - u->kioskStepAt > 1500 || !u->kioskPresses) {
            if (u->kioskPresses >= 3) { EndVisit("three presses of use did nothing", true); return false; }
            ++u->kioskPresses;
            u->kioskStepAt = now;
            if (u->kioskStore) { const unsigned char one = 1; WriteBytes(u->entity + kWeaponSwitchMsgSent, &one, 1); }
            c.use = true;
        }
        c.look = true;
        c.lookAt = head;
        break;
    case KioskStep::Inside: {
        u->pad = Pad{};
        u->padLive = true;
        if (!sm) { EndVisit("the kiosk let it go early", !u->kioskBought); return true; }
        const bool menu = store ? (state == 8 || state == 51) : (state == 4 || state == 11);
        if (menu && !u->kioskBought) {
            u->kioskBought = true;
            if (store) Shop(Leader()); else Invest(Leader());
            NoteKioskTurn(u->kiosk, store);
            u->kioskLeaveAt = now + 1500 + (GetTickCount() % 1500);
        }
        if ((u->kioskBought && now >= u->kioskLeaveAt) || now - u->kioskStepAt > 15000) {
            u->kioskVisit = KioskStep::Leave;
            u->kioskStepAt = now;
            u->kioskBackAt = 0;
        }
        return true;
    }
    case KioskStep::Browse:
        c.look = true;
        c.lookAt = head;
        if (u->seen.enemyCount > 0) { EndVisit("a fight came first", true); return false; }
        if (now >= u->kioskLeaveAt) {
            InterlockedIncrement(&kioskExits);
            EndVisit("done browsing, bought what it needs", true);
            return false;
        }
        break;
    case KioskStep::Leave: {
        u->pad = Pad{};
        u->padLive = true;
        if (!sm) {
            InterlockedIncrement(&kioskExits);
            EndVisit(u->kioskBought ? "done, left the kiosk" : "left the kiosk without buying", !u->kioskBought);
            return true;
        }
        const bool leaving = store ? state >= 55 : state >= 28;
        if (!leaving && !u->backStep && now - u->kioskBackAt >= 400) { u->kioskBackAt = now; u->backStep = 1; }
        if (now - u->kioskStepAt > 12000) {
            InterlockedIncrement(&kioskAborts);
            gate::Event("bot %u: cannot back out of the %s (state %d); left to the game", u->index + 1, store ? "store" : "bench", state);
            u->kioskStepAt = now;   // keep pressing, but report every 12 s
        }
        return true;
    }
    default:
        return false;
    }
    // Act carries out u->command: the visit's own command replaces the brain's for
    // this frame (28.09.: it ran the brain's last one, so use was never pressed).
    u->command = c;
    Act(self, leaderPos, now);
    return true;
}

// --- store rooms --------------------------------------------------------------------------
// A store or a bench within 14 m of the leader and in his sight makes his room a
// store room. While he is there each bot takes its turn at them on its own (walks
// up, browses, buys, upgrades, changes its suit), one bot per kiosk at a time,
// without waiting for him to use them first. Whoever has not had its turn when he
// leaves the room (20 m from its kiosks, 10 m with a wall between, or another
// floor) does it all at once, wherever it is: its nodes at the bench, its buys and
// guns at the store, and its suit. The kiosks come from the TargetingManager's
// usable list ([0x020212EC]: the lists by type from +0xC, type 0 the usables; a
// node's vtable 0x01B4139C, target SafePtr +8 = entity+0x10, next +0x28).
constexpr uint32_t kTargetInfoVtableRva = 0x174139C, kTargetLists = 0xC, kTargetNext = 0x28, kTargetEntity = 8;
constexpr float kRoomEnter = 14.0f, kRoomSpan = 20.0f, kRoomUnseen = 10.0f, kRoomFloor = 3.5f;
int usableLayout = -1;   // how +0xC leads to the first node: 0 via a list of lists, 1 via the list, 2 inline
bool FirstUsable(uint32_t tm, uint32_t& node) {
    uint32_t lists = 0, list = 0, candidates[3]{};
    if (!Read(tm + kTargetLists, &lists, 4) || !lists) return false;
    if (Read(lists, &list, 4) && list) Read(list, &candidates[0], 4);
    Read(lists, &candidates[1], 4);
    candidates[2] = lists;
    for (int i = 0; i < 3; ++i) {
        if (usableLayout >= 0 && i != usableLayout) continue;
        uint32_t vtable = 0;
        if (candidates[i] && Read(candidates[i], &vtable, 4) && vtable == base + kTargetInfoVtableRva) {
            if (usableLayout < 0) {
                usableLayout = i;
                gate::Event("bot: store rooms: the usable list starts %s", i == 0 ? "through a list of lists" : (i == 1 ? "at the list" : "inline"));
            }
            node = candidates[i];
            return true;
        }
    }
    return false;
}
// The nearest store and bench to the leader on his floor.
void FindKiosks(const Vec3& leaderPos, uint32_t& store, Vec3& storeAt, uint32_t& bench, Vec3& benchAt) {
    store = bench = 0;
    float bestStore = 1e9f, bestBench = 1e9f;
    uint32_t tm = 0, node = 0;
    if (!Read(base + kTargetingRva, &tm, 4) || !tm || !FirstUsable(tm, node)) return;
    for (unsigned steps = 0; node && steps < 512; ++steps) {
        uint32_t vtable = 0, ref = 0, next = 0;
        if (!Read(node, &vtable, 4) || vtable != base + kTargetInfoVtableRva) { InterlockedIncrement(&roomScanFaults); return; }
        if (Read(node + kTargetEntity, &ref, 4) && ref > 0x10010) {
            const uint32_t kiosk = ref - 0x10;
            uint32_t kind = 0;
            Vec3 at;
            if (Read(kiosk, &kind, 4) && (kind == base + kStoreVtableRva || kind == base + kBenchVtableRva) && bot_sense::Pos(kiosk, at) &&
                std::fabs(at.y - leaderPos.y) < kRoomFloor) {
                const float d = coop_bot::Flat(at, leaderPos);
                if (kind == base + kStoreVtableRva && d < bestStore) { bestStore = d; store = kiosk; storeAt = at; }
                if (kind == base + kBenchVtableRva && d < bestBench) { bestBench = d; bench = kiosk; benchAt = at; }
            }
        }
        if (!Read(node + kTargetNext, &next, 4) || next == node) break;
        node = next;
    }
}
bool KioskStands(uint32_t kiosk, bool store) {
    uint32_t vtable = 0;
    return kiosk && Read(kiosk, &vtable, 4) && vtable == base + (store ? kStoreVtableRva : kBenchVtableRva);
}
// The leader left: every bot that has not had its turn does it all at once.
void FinishRoom(DWORD now, const char* why) {
    Unit* const saved = u;
    const uint32_t leader = Leader();
    for (unsigned i = 0; i < wantedBots; ++i) {
        u = &units[i];
        if (!u->entity || u->state != State::Active) continue;
        const unsigned bit = 1u << i;
        // A visit under way ends here, unless a real menu is open (KioskEnter=1): that one finishes itself.
        if (u->kioskVisit != KioskStep::None) {
            if (kioskEnter && (u->kioskVisit == KioskStep::Press || u->kioskVisit == KioskStep::Inside || u->kioskVisit == KioskStep::Leave)) continue;
            EndVisit(why, false);
        }
        const bool bench = room.bench && !(room.benchDone & bit), store = room.store && !(room.storeDone & bit);
        if (bench) { Invest(leader); room.benchDone |= bit; InterlockedIncrement(&instantBenches); }
        if (store) { Shop(leader); StoreSuit(leader, now); room.storeDone |= bit; InterlockedIncrement(&instantStores); }
        if (bench || store)
            gate::Event("bot %u: %s: %s%s%s at once", u->index + 1, why, bench ? "its nodes at the bench" : "", bench && store ? ", " : "",
                        store ? "its buys, guns and suit at the store" : "");
    }
    u = saved;
}
void RoomTick(uint32_t leader, const Vec3& leaderPos, DWORD now) {
    if (!storeRooms || !kioskReady || now - room.scanAt < 500) return;
    room.scanAt = now;
    const Vec3 chest = {leaderPos.x, leaderPos.y + 1.2f, leaderPos.z};
    auto inSight = [&](uint32_t kiosk, const Vec3& at) {
        return !bot_sense::losReady || bot_sense::Clear({at.x, at.y + 1.3f, at.z}, chest, kiosk, leader);
    };
    if (!room.in) {
        uint32_t store = 0, bench = 0;
        Vec3 storeAt, benchAt;
        FindKiosks(leaderPos, store, storeAt, bench, benchAt);
        const float ds = store ? coop_bot::Flat(storeAt, leaderPos) : 1e9f, db = bench ? coop_bot::Flat(benchAt, leaderPos) : 1e9f;
        const bool nearStore = ds < kRoomEnter && inSight(store, storeAt), nearBench = db < kRoomEnter && inSight(bench, benchAt);
        if (!nearStore && !nearBench) return;
        // Back in the room it left a moment ago: the turns taken there still count.
        const bool again = ((store && store == room.store) || (bench && bench == room.bench)) && now - room.leftAt < 300000;
        if (!again) room.storeDone = room.benchDone = 0;
        room.store = store && ds < kRoomSpan ? store : 0;
        room.bench = bench && db < kRoomSpan ? bench : 0;
        room.storeAt = storeAt;
        room.benchAt = benchAt;
        room.in = true;
        room.sight = true;
        InterlockedIncrement(&roomEntries);
        gate::Event("bot: the leader is in a store room (store %08X at %.1f m, bench %08X at %.1f m%s)", room.store, room.store ? ds : 0.0f, room.bench,
                    room.bench ? db : 0.0f, again ? ", back again" : "");
        return;
    }
    const bool storeHere = KioskStands(room.store, true), benchHere = KioskStands(room.bench, false);
    float d = 1e9f, dy = 0.0f;
    uint32_t nearest = 0;
    Vec3 nearestAt;
    if (storeHere && coop_bot::Flat(room.storeAt, leaderPos) < d) { d = coop_bot::Flat(room.storeAt, leaderPos); nearest = room.store; nearestAt = room.storeAt; }
    if (benchHere && coop_bot::Flat(room.benchAt, leaderPos) < d) { d = coop_bot::Flat(room.benchAt, leaderPos); nearest = room.bench; nearestAt = room.benchAt; }
    if (nearest) {
        dy = std::fabs(nearestAt.y - leaderPos.y);
        room.sight = d < kRoomUnseen || inSight(nearest, nearestAt);
    }
    const bool gone = !nearest, away = d > kRoomSpan || (d > kRoomUnseen && !room.sight) || dy > kRoomFloor + 1.0f;
    if (!gone && !away) return;
    room.in = false;
    room.leftAt = now;
    InterlockedIncrement(&roomExits);
    gate::Event("bot: the leader left the store room (%s, %.1f m from its kiosks); turns taken: store %X, bench %X", gone ? "its kiosks are gone" : "out of it",
                gone ? 0.0f : d, room.storeDone, room.benchDone);
    FinishRoom(now, "the leader left the store room");
}
// The input context push and pop; also wanted by the alt fire trace without kiosk use.
bool contextHooked = false;
bool InstallContextHooks(FILE* log, bool& push, bool& pop) {
    static bool tried = false, pushOk = false, popOk = false;
    if (!tried) {
        tried = true;
        const uint32_t stack = static_cast<uint32_t>(base + kContextStackRva);
        // mov eax,[esp+4] / mov ecx,[stack] / push -1 / push eax / call
        unsigned char kPush[] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x0D, 0, 0, 0, 0, 0x6A, 0xFF, 0x50, 0xE8};
        std::memcpy(kPush + 6, &stack, 4);
        pushOk = hook_site::Install(log, "bot input context push", base + kContextPushRva, kPush, sizeof(kPush),
                                    reinterpret_cast<void*>(OnContextPush), contextPushHook);
        popOk = pushOk && hook_site::Install(log, "bot input context pop", base + kContextPopRva, kPush, sizeof(kPush),
                                             reinterpret_cast<void*>(OnContextPop), contextPopHook);
        if (pushOk) contextPushTrampoline = contextPushHook.trampoline().address();
        if (popOk) contextPopTrampoline = contextPopHook.trampoline().address();
        contextHooked = pushOk && popOk && contextPushTrampoline && contextPopTrampoline;
    }
    push = pushOk;
    pop = popOk;
    return contextHooked;
}
bool InstallKiosk(FILE* log) {
    if (!kioskWanted || (!benchReady && !storeReady)) return false;
    if (!kioskEnter) {
        kioskReady = true;
        std::fprintf(log, "bot kiosks=1 enter=0 (bots walk up to the bench and the store after the leader, browse a moment and buy without a menu)\n");
        return true;
    }
    bool push = false, pop = false;
    InstallContextHooks(log, push, pop);
    // sub esp,14h / push ebx / push ebp / push esi / push edi / mov edi,ecx / mov [esp+10h],edi
    static const unsigned char kRumble[] = {0x83, 0xEC, 0x14, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9, 0x89, 0x7C, 0x24, 0x10};
    const bool rumble = pop && hook_site::Install(log, "bot DeviceManager rumble pass", base + kRumblePassRva, kRumble, sizeof(kRumble),
                                                  reinterpret_cast<void*>(OnRumblePass), rumbleHook);
    kioskReady = push && pop && rumble && contextPushTrampoline && contextPopTrampoline;
    std::fprintf(log, "bot kiosks=%d (context_push=%d context_pop=%d rumble_guard=%d: bots use the bench and the store themselves after the leader)\n",
                 kioskReady ? 1 : 0, push ? 1 : 0, pop ? 1 : 0, rumble ? 1 : 0);
    return kioskReady;
}

// The lower body machine's state (the machine tree as in "using the kiosks"), -1
// unknown, and while it flies (state 20, zgJump) the zero-G machine (vtable
// 0x01B27E1C) with its state; only then is the rest of the tree walked.
constexpr uint32_t kLowerBodySmVtableRva = 0x1727000, kZeroGSmVtableRva = 0x1727E1C;
struct Machines { int32_t lower = -1, zeroG = -1; uint32_t zeroGSm = 0; };
Machines MachinesOf(uint32_t player) {
    Machines m;
    uint32_t stack[64], rootSm = 0;
    int n = 0, visited = 0;
    bool lowerFound = false;
    if (!Read(player + kMachineRoot, &rootSm, 4) || !rootSm) return m;
    stack[n++] = rootSm;
    while (n && visited < 128) {
        const uint32_t sm = stack[--n];
        ++visited;
        uint32_t vtable = 0, child = 0, sibling = 0;
        if (!Read(sm, &vtable, 4)) continue;
        if (vtable == base + kLowerBodySmVtableRva && !lowerFound) {
            lowerFound = true;
            if (!Read(sm + kMachineState, &m.lower, 4)) m.lower = -1;
            if (m.lower != 20) { m.zeroG = -1; m.zeroGSm = 0; return m; }
            if (m.zeroGSm) return m;
        } else if (vtable == base + kZeroGSmVtableRva && !m.zeroGSm) {
            m.zeroGSm = sm;
            if (!Read(sm + kMachineState, &m.zeroG, 4)) m.zeroG = -1;
            if (lowerFound) return m;
        }
        if (Read(sm + kMachineChild, &child, 4) && child && n < 64) stack[n++] = child;
        if (Read(sm + kMachineSibling, &sibling, 4) && sibling && n < 64) stack[n++] = sibling;
    }
    if (m.lower != 20) { m.zeroG = -1; m.zeroGSm = 0; }
    return m;
}
int32_t LowerBodyState(uint32_t player) { return MachinesOf(player).lower; }

// --- kinesis ---------------------------------------------------------------------------
// In a fight a bot now and then
// grabs a loose prop with kinesis and throws it at its target. The bot's own
// PlayerFireTKSM does the work (aim held, the med pack button once to grab, fire
// to throw); only two points in it are player 0's: the grab cast
// (PlayerFireTKSM::ExecuteAttack, PC 0x008A48A0, thiscall(sm), ret) and the
// launch velocity (ApplyKeyframeToTarget, PC 0x008A3DE0, thiscall(sm, type, dt),
// ret 8, type 1 = launch) both aim at the CenterPointAiming point
// ([0x020213AC]+0x10), the human's crosshair. For a bot's machine that point is
// swapped for the bot's own while the call runs; the game recomputes it every frame.
//
// Throwables come from the Havok world (HavokManager [0x02021318]+0x24, active
// and inactive islands at +0x28/+0x34, bodies at island+0x48; a body's entity is
// property 0x2001 of the array at body+0x78), filtered the way CanTK does: a
// TKComponent (type byte [0x020A7B22], vtable 0x01B6A430) that is not immune,
// not held, on a body that is neither keyframed nor fixed. ScriptableProp and
// DismemberableProp are taken, and the canisters: DamageableProps with a grab
// priority of 69 (explosive and stasis alike, read live 28.09.; launch damage 0,
// launch power x2). No teammate may stand within 2 m of a canister's target.
// Other dynamic kinesis classes are logged.
constexpr uint32_t kTkAttackRva = 0x4A48A0, kTkKeyframeRva = 0x4A3DE0, kCenterPointRva = 0x1C213AC, kCenterPoint = 0x10;
constexpr uint32_t kHavokManagerRva = 0x1C21318, kHavokWorld = 0x24, kActiveIslands = 0x28, kInactiveIslands = 0x34, kIslandBodies = 0x48;
constexpr uint32_t kBodyProperties = 0x78, kBodyPropertyCount = 0x7C, kEntityProperty = 0x2001;
constexpr uint32_t kTkTypeRva = 0x1CA7B22, kTkComponentVtableRva = 0x176A430, kComponentTypes = 0x44, kComponents = 0x48, kComponentOwner = 0x10;
constexpr uint32_t kTkDataRef = 0x1A0, kTkDataInline = 0x30, kTkFlags = 0x1A8, kTkGrabbedBit = 2, kTkImmune = 0x58, kTkLaunchDamage = 0x18;
constexpr uint32_t kScriptablePropVtableRva = 0x175B268, kDismemberablePropVtableRva = 0x175CCB0, kTkGrabPriority = 0x88;
constexpr float kCanisterPriority = 50.0f, kCanisterValue = 30.0f, kCanisterClear = 2.0f;
constexpr float kAlertRange = 10.0f, kTkTooClose = 1.0f;
constexpr DWORD kAlertLingerMs = 3000, kRerollMs = 5000, kTkSnagMs = 600;
constexpr DWORD kTkCarryMs = 12000, kTkStuckMs = 2000, kTkCloseDropMs = 600;   // an enemy within a metre this long: drop it and fight
constexpr float kTkStuckMove = 0.4f;
constexpr float kTkSnagLag = 3.0f;   // metres from the bot's chest; held, it sits about 1.5 m out
constexpr uint32_t kGameFlagsRva = 0x1C1C0A4, kGameFlags = 0x1C;
constexpr float kTkReach = 9.0f, kTkMinDamage = 10.0f, kTkThrowMin = 3.5f, kTkThrowMax = 18.0f, kTkLeaderClear = 1.3f;
constexpr int kMaxThrowables = 48, kTkSightTries = 4;
bool kinesisWanted = false, kinesisReady = false;
safetyhook::InlineHook tkAttackHook, tkKeyframeHook;
struct Throwable { uint32_t entity = 0, body = 0, vtable = 0; Vec3 pos; float damage = 0.0f; bool canister = false; };
Throwable throwables[kMaxThrowables];
int throwableCount = 0;
DWORD throwablesAt = 0;
uint32_t otherTkClasses[16]{};
unsigned otherTkClassCount = 0;
struct Shunned { uint32_t entity; DWORD until; };
Shunned shunned[8]{};
volatile LONG tkStarts = 0, tkGrabs = 0, tkThrows = 0, tkMisses = 0, tkLost = 0, tkNoLaunch = 0, tkRedirects = 0, tkAborts = 0, tkMedpacksLost = 0,
              tkScanFaults = 0, tkLogLines = 0, alerts = 0, tkRollsYes = 0, tkRollsNo = 0, tkCalmDrops = 0, tkCloseDrops = 0, tkHealDrops = 0, tkSnagDrops = 0, tkMaxLagCm = 0, tkFirePulses = 0, tkCarryDrops = 0, tkStuckDrops = 0,
              tkWaitNoTarget = 0, tkWaitRegrow = 0, tkWaitRange = 0, tkWaitCrowded = 0, tkWaitLane = 0, tkWaitBlast = 0, tkWaitAim = 0;

// The bot whose kinesis machine this is, when that bot wants the point redirected.
Unit* RedirectFor(void* sm) {
    uint32_t owner = 0;
    if (!Read(reinterpret_cast<uintptr_t>(sm) + tk::kSmOwner, &owner, 4)) return nullptr;
    Unit* unit = UnitOf(owner);
    return unit && unit->tkRedirect ? unit : nullptr;
}
struct CenterPointSwap {
    uint32_t point = 0;
    float saved[3]{};
    explicit CenterPointSwap(const Vec3& to) {
        uint32_t cpa = 0;
        if (!Read(base + kCenterPointRva, &cpa, 4) || !cpa || !Read(cpa + kCenterPoint, saved, sizeof(saved))) return;
        const float want[3] = {to.x, to.y, to.z};
        if (!WriteBytes(cpa + kCenterPoint, want, sizeof(want))) return;
        point = cpa + kCenterPoint;
        InterlockedIncrement(&tkRedirects);
    }
    ~CenterPointSwap() {
        if (point) WriteBytes(point, saved, sizeof(saved));
    }
};
uint32_t __fastcall OnTkAttack(void* sm, void*) {
    Unit* unit = RedirectFor(sm);
    if (!unit) return tkAttackHook.thiscall<uint32_t>(sm);
    CenterPointSwap swap(unit->tkRedirectTo);
    return tkAttackHook.thiscall<uint32_t>(sm);
}
uint32_t __fastcall OnTkKeyframe(void* sm, void*, int type, float dt) {
    Unit* unit = type == 1 ? RedirectFor(sm) : nullptr;
    if (!unit) return tkKeyframeHook.thiscall<uint32_t>(sm, type, dt);
    CenterPointSwap swap(unit->tkRedirectTo);
    return tkKeyframeHook.thiscall<uint32_t>(sm, type, dt);
}

uint32_t EntityOfBody(uint32_t body) {
    uint32_t props = 0, count = 0;
    if (!Read(body + kBodyProperties, &props, 4) || !props || !Read(body + kBodyPropertyCount, &count, 4) || count > 32) return 0;
    uint32_t entries[32 * 4];
    if (!count || !Read(props, entries, count * 16)) return 0;
    for (uint32_t i = 0; i < count; ++i)
        if (entries[i * 4] == kEntityProperty) return entries[i * 4 + 2];
    return 0;
}
uint32_t TkComponentOf(uint32_t entity, unsigned char type) {
    uint32_t types = 0, components = 0, comp = 0, vtable = 0, owner = 0;
    unsigned char index = 0;
    if (!Read(entity + kComponentTypes, &types, 4) || !types || !Read(entity + kComponents, &components, 4) || !components ||
        !Read(types + type + 0xC, &index, 1) || !Read(components + index * 4u, &comp, 4) || !comp)
        return 0;
    return Read(comp, &vtable, 4) && vtable == base + kTkComponentVtableRva && Read(comp + kComponentOwner, &owner, 4) && owner == entity ? comp : 0;
}
bool Shuns(uint32_t entity, DWORD now) {
    for (const Shunned& s : shunned) if (s.entity == entity && now < s.until) return true;
    return false;
}
void Shun(uint32_t entity, DWORD now) {
    Shunned* slot = &shunned[0];
    for (Shunned& s : shunned) if (s.until < slot->until) slot = &s;
    *slot = Shunned{entity, now + 20000};
}
// Twice a second while a bot looks for one, shared by all bots: the loose props
// that kinesis could take. The walk over the Havok bodies goes kTkWalkBodies a
// frame (05.10.: up to 13 ms in one frame); the last complete list is used meanwhile.
constexpr unsigned kTkWalkBodies = 256;
struct ThrowableWalk { bool active = false; unsigned list = 0; uint32_t island = 0, body = 0; int count = 0; Throwable found[kMaxThrowables]; };
ThrowableWalk tkWalk;
volatile LONG tkWalks = 0;
void PublishThrowables(DWORD now) {
    throwableCount = tkWalk.count;
    for (int i = 0; i < tkWalk.count; ++i) throwables[i] = tkWalk.found[i];
    throwablesAt = now;
    tkWalk.active = false;
    InterlockedIncrement(&tkWalks);
}
void ScanThrowables(DWORD now) {
    if (!tkWalk.active) {
        if (throwablesAt && now - throwablesAt < 500) return;
        tkWalk.active = true;
        tkWalk.list = 0;
        tkWalk.island = tkWalk.body = 0;
        tkWalk.count = 0;
    }
    const Perf timer(perfThrowables);
    unsigned char type = 0;
    uint32_t manager = 0, world = 0;
    if (!Read(base + kTkTypeRva, &type, 1) || !type) { tkWalk.count = 0; PublishThrowables(now); return; }   // no kinesis component type registered yet
    if (!Read(base + kHavokManagerRva, &manager, 4) || !manager || !Read(manager + kHavokWorld, &world, 4) || !world) {
        InterlockedIncrement(&tkScanFaults);
        tkWalk.count = 0;
        PublishThrowables(now);
        return;
    }
    static const uint32_t kIslandLists[] = {kActiveIslands, kInactiveIslands};
    unsigned budget = kTkWalkBodies;
    for (; tkWalk.list < 2; ++tkWalk.list, tkWalk.island = 0, tkWalk.body = 0) {
        const uint32_t list = kIslandLists[tkWalk.list];
        uint32_t islands = 0, islandCount = 0;
        if (!Read(world + list, &islands, 4) || !islands || !Read(world + list + 4, &islandCount, 4) || islandCount > 512) continue;
        for (; tkWalk.island < islandCount && tkWalk.count < kMaxThrowables; ++tkWalk.island, tkWalk.body = 0) {
            const uint32_t i = tkWalk.island;
            uint32_t island = 0, bodies = 0, bodyCount = 0;
            if (!Read(islands + i * 4, &island, 4) || !island || !Read(island + kIslandBodies, &bodies, 4) || !bodies ||
                !Read(island + kIslandBodies + 4, &bodyCount, 4) || bodyCount > 4096)
                continue;
            for (; tkWalk.body < bodyCount && tkWalk.count < kMaxThrowables; ++tkWalk.body) {
                if (!budget--) return;   // on in the next frame, from this body
                const uint32_t j = tkWalk.body;
                uint32_t body = 0, vtable = 0;
                unsigned char motion = 0;
                if (!Read(bodies + j * 4, &body, 4) || !body || !Read(body + tk::kBodyMotionType, &motion, 1) || motion == tk::kMotionKeyframed ||
                    motion == tk::kMotionFixed)
                    continue;
                const uint32_t entity = EntityOfBody(body);
                if (!entity || !Read(entity, &vtable, 4)) continue;
                const uint32_t comp = TkComponentOf(entity, type);
                if (!comp) continue;
                uint32_t data = 0, flags = 0;
                unsigned char immune = 1, e16a = 1, e16d = 1;
                float damage = 0.0f;
                if (!Read(comp + kTkDataRef, &data, 4)) continue;
                if (!data) data = comp + kTkDataInline;
                if (!Read(data + kTkImmune, &immune, 1) || immune || !Read(comp + kTkFlags, &flags, 4) || (flags & kTkGrabbedBit) ||
                    !Read(data + kTkLaunchDamage, &damage, 4) || !std::isfinite(damage) || !Read(entity + 0x16A, &e16a, 1) || (e16a & 1) ||
                    !Read(entity + 0x16D, &e16d, 1) || e16d)
                    continue;
                float priority = 0.0f;
                const bool canister = vtable == base + bot_sense::kDamageablePropVtableRva && Read(data + kTkGrabPriority, &priority, 4) &&
                                      std::isfinite(priority) && priority >= kCanisterPriority;
                if (!canister && vtable != base + kScriptablePropVtableRva && vtable != base + kDismemberablePropVtableRva) {
                    // Unknown kinesis class on a loose body: named once, for the next list.
                    bool known = false;
                    for (unsigned k = 0; k < otherTkClassCount; ++k) known = known || otherTkClasses[k] == vtable;
                    if (!known && otherTkClassCount < std::size(otherTkClasses)) {
                        otherTkClasses[otherTkClassCount++] = vtable;
                        gate::Event("bot kinesis: loose kinesis object of another class, vtable RVA %08X (entity %08X, launch damage %.0f, motion %u); not taken",
                                    vtable - static_cast<uint32_t>(base), entity, damage, motion);
                    }
                    continue;
                }
                if (!canister && damage < kTkMinDamage) continue;
                bool dup = false;
                for (int k = 0; k < tkWalk.count; ++k) dup = dup || tkWalk.found[k].entity == entity;
                float at[3]{};
                if (dup || !Read(body + tk::kBodyPos, at, sizeof(at)) || !std::isfinite(at[0]) || !std::isfinite(at[1]) || !std::isfinite(at[2])) continue;
                tkWalk.found[tkWalk.count++] = Throwable{entity, body, vtable, {at[0], at[1], at[2]}, damage, canister};
            }
        }
    }
    PublishThrowables(now);
}
// The bot's kinesis machine (a child of its upper body machine while it is in 'fireTK').
uint32_t TkMachine(uint32_t player) {
    uint32_t stack[64], rootSm = 0;
    int n = 0, visited = 0;
    if (!Read(player + kMachineRoot, &rootSm, 4) || !rootSm) return 0;
    stack[n++] = rootSm;
    while (n && visited < 256) {
        const uint32_t sm = stack[--n];
        ++visited;
        uint32_t vtable = 0, child = 0, sibling = 0, owner = 0;
        if (!Read(sm, &vtable, 4)) continue;
        if (vtable == base + tk::kSmVtableRva) return Read(sm + tk::kSmOwner, &owner, 4) && owner == player ? sm : 0;
        if (Read(sm + kMachineChild, &child, 4) && child && n < 64) stack[n++] = child;
        if (Read(sm + kMachineSibling, &sibling, 4) && sibling && n < 64) stack[n++] = sibling;
    }
    return 0;
}
uint32_t TkHeld(uint32_t player) {
    const uint32_t sm = TkMachine(player);
    uint32_t body = 0;
    return sm && Read(sm + tk::kSmBody, &body, 4) ? body : 0;
}
float SegmentGap(const Vec3& a, const Vec3& b, const Vec3& p) {
    const Vec3 ab = coop_bot::Sub(b, a);
    const float len2 = coop_bot::Dot(ab, ab);
    const float t = len2 > 1e-4f ? Clamp(coop_bot::Dot(coop_bot::Sub(p, a), ab) / len2, 0.0f, 1.0f) : 0.0f;
    return coop_bot::Distance(p, coop_bot::Add(a, coop_bot::Scale(ab, t)));
}
// A canister bursts where it lands: no teammate (the leader, a bot, this one)
// within kCanisterClear of the enemy it goes for.
bool BlastClear(const Vec3& at, const Vec3& self, const Vec3& leaderPos) {
    if (coop_bot::Distance(at, self) < kCanisterClear || coop_bot::Distance(at, leaderPos) < kCanisterClear) return false;
    for (const Unit& other : units)
        if (&other != u && other.entity && coop_bot::Distance(at, other.self) < kCanisterClear) return false;
    return true;
}
// No teammate in the flight path.
bool LaneClear(const Vec3& from, const Vec3& to, const Vec3& leaderPos) {
    const Vec3 chest = {leaderPos.x, leaderPos.y + 1.0f, leaderPos.z};
    if (coop_bot::Distance(from, leaderPos) < kTkLeaderClear || SegmentGap(from, to, chest) < kTkLeaderClear) return false;
    for (const Unit& other : units)
        if (&other != u && other.entity && SegmentGap(from, to, {other.self.x, other.self.y + 1.0f, other.self.z}) < kTkLeaderClear) return false;
    return true;
}
void EndKinesis(DWORD now, DWORD pause, const char* how) {
    if (u->tkStep != TkStep::None && InterlockedIncrement(&tkLogLines) <= 120) {
        if (u->tkHeldSince && (u->tkStep == TkStep::Hold || u->tkStep == TkStep::Throw))
            gate::Event("bot %u kinesis: %s (object %08X, target %08X; held %.1f s, last wait: %s)", u->index + 1, how, u->tkObject, u->tkTarget,
                        static_cast<float>(now - u->tkHeldSince) / 1000.0f, u->tkWaitWhy ? u->tkWaitWhy : "none");
        else
            gate::Event("bot %u kinesis: %s (object %08X, target %08X)", u->index + 1, how, u->tkObject, u->tkTarget);
    }
    u->tkStep = TkStep::None;
    u->tkRedirect = false;
    u->tkMedpackUntil = 0;
    u->tkLaunchWanted = false;
    u->tkWaitWhy = nullptr;
    u->tkHeldSince = 0;
    u->tkNextAt = now + pause + (now * 7919u) % 2000u;
}
// The fight's point for the throw: the brain's aim point on its target, else that enemy's chest.
bool ThrowPoint(const coop_bot::Perception& p, const coop_bot::Command& brain, Vec3& at, Vec3& enemyAt, uint32_t& target, bool& regrowing) {
    regrowing = false;
    for (int i = 0; i < p.enemyCount; ++i) {
        const coop_bot::Enemy& e = p.enemies[i];
        if (!brain.target || e.id != brain.target || e.health <= 0.0f || !e.visible) continue;
        if (e.regenerating) { regrowing = true; continue; }   // nothing thrown at it either (the brain's HoldOnRegenerating)
        target = e.id;
        at = brain.look ? brain.lookAt : e.chest;
        enemyAt = e.pos;
        return true;
    }
    return false;
}
// A throw of the dice (percent from the bot's profile), a little different for each bot.
bool Toss(DWORD now, int percent) {
    uint32_t x = static_cast<uint32_t>(now) * 2654435761u ^ (u->index + 1) * 0x9E3779B9u;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    return static_cast<int>(x % 100u) < percent;
}
void Roll(DWORD now, const char* why) {
    u->tkWants = Toss(now, u->kinesisChance);
    InterlockedIncrement(u->tkWants ? &tkRollsYes : &tkRollsNo);
    if (InterlockedIncrement(&tkLogLines) <= 120)
        gate::Event("bot %u: %s; %s", u->index + 1, why, u->tkWants ? "looks for something to take with kinesis" : "keeps its gun");
}
// Alert: an enemy within kAlertRange, seen or not (heard). It means nothing but
// that the bot may want a kinesis object in hand: heads or tails once per alert,
// and again a while after each throw while the alert lasts.
void UpdateAlert(const Vec3& self, const coop_bot::Perception& p, DWORD now) {
    bool nearby = false;
    for (int i = 0; i < p.enemyCount && !nearby; ++i) {
        const coop_bot::Enemy& e = p.enemies[i];
        nearby = e.health > 0.0f && !e.objective && coop_bot::Distance(self, e.pos) <= kAlertRange;
    }
    if (nearby) u->alertSeenAt = now;
    const bool was = u->alert;
    u->alert = nearby || (was && now - u->alertSeenAt < kAlertLingerMs);
    if (u->alert && !was) {
        InterlockedIncrement(&alerts);
        u->tkRerollAt = 0;
        Roll(now, "alert, an enemy within 10 m");
    } else if (!u->alert && was) {
        u->tkWants = false;
        u->tkRerollAt = 0;
    } else if (u->alert && u->tkRerollAt && now >= u->tkRerollAt && u->tkStep == TkStep::None) {
        u->tkRerollAt = 0;
        Roll(now, "still alert after a throw");
    }
}
// One frame of kinesis over the brain's command: when alert and minded to, take
// the best object in reach and hold it (walking with the others, the gun quiet);
// throw it at the brain's target once no teammate is in the way (and none within
// 2 m of that enemy for a canister) and no enemy stands within a metre.
void KinesisTick(const Vec3& self, const Vec3& leaderPos, const coop_bot::Perception& p, DWORD now) {
    if (!kinesisReady) return;
    if (u->flying || !u->tkModule) {   // no kinesis in flight, none before the leader has it
        if (u->tkStep != TkStep::None) EndKinesis(now, 1500, u->flying ? "zero-G flight" : "no kinesis module");
        return;
    }
    UpdateAlert(self, p, now);
    coop_bot::Command& c = u->command;
    // Hold fire: no throws either; one under way is dropped.
    if (c.holdFireActive) {
        if (u->tkStep != TkStep::None) EndKinesis(now, 1500, "hold fire");
        return;
    }
    if (u->tkStep == TkStep::None) {
        if (u->tkRedirect && now - u->tkReleasedAt > 300) u->tkRedirect = false;
        if (!u->alert || !u->tkWants || now < u->tkNextAt || p.grabbed || u->kioskVisit != KioskStep::None) return;
        for (const Unit& other : units)
            if (&other != u && (other.tkStep == TkStep::Raise || other.tkStep == TkStep::Grab)) return;   // one grab at a time
        ScanThrowables(now);
        if (!throwablesAt) return;   // the first walk is still under way
        // Best first; the line of sight (a cast each) only for the best few.
        int order[kMaxThrowables];
        float score[kMaxThrowables];
        int candidates = 0;
        for (int i = 0; i < throwableCount; ++i) {
            const Throwable& t = throwables[i];
            const float d = coop_bot::Distance(self, t.pos);
            if (d > kTkReach || d < 1.0f || std::fabs(t.pos.y - self.y) > 3.0f || coop_bot::Distance(leaderPos, t.pos) < kTkLeaderClear || Shuns(t.entity, now))
                continue;
            bool claimed = false;
            for (const Unit& other : units) claimed = claimed || (&other != u && other.tkObject == t.entity && other.tkStep != TkStep::None);
            for (int k = 0; k < p.enemyCount && !claimed; ++k) claimed = p.enemies[k].id == t.entity;   // the Marker's heart is a DamageableProp too
            if (claimed) continue;
            score[i] = (t.canister ? kCanisterValue : t.damage * 2.0f) - d;
            int at = candidates++;
            for (; at > 0 && score[order[at - 1]] < score[i]; --at) order[at] = order[at - 1];
            order[at] = i;
        }
        const Vec3 eye = CameraEye(self);
        int best = -1;
        for (int k = 0; k < candidates && k < kTkSightTries && best < 0; ++k)
            if (bot_sense::Clear(eye, throwables[order[k]].pos, u->entity, throwables[order[k]].entity)) best = order[k];
        if (best < 0) { u->tkNextAt = now + 1500; return; }
        const Throwable& t = throwables[best];
        u->tkStep = TkStep::Raise;
        u->tkObject = t.entity;
        u->tkBody = t.body;
        u->tkCanister = t.canister;
        u->tkTarget = 0;
        u->tkStepAt = now;
        u->tkPresses = 0;
        u->tkCloseSince = 0;
        u->tkMedpacks = MedPacks(u->entity);
        InterlockedIncrement(&tkStarts);
        if (InterlockedIncrement(&tkLogLines) <= 120) {
            uint32_t flags = 0, gameFlags = 0;
            if (Read(base + kGameFlagsRva, &gameFlags, 4) && gameFlags) Read(gameFlags + kGameFlags, &flags, 4);
            gate::Event("bot %u kinesis: goes for %08X (vtable RVA %08X%s, launch damage %.0f, %.1f m away; %d throwables, mp flag %u)", u->index + 1,
                        t.entity, t.vtable - static_cast<uint32_t>(base), t.canister ? ", a canister" : "", t.damage, coop_bot::Distance(self, t.pos),
                        throwableCount, (flags >> 1) & 1u);
        }
    }
    // A struggle or a kiosk: let go (releasing the aim drops what it holds).
    if (p.grabbed || u->kioskVisit != KioskStep::None) { InterlockedIncrement(&tkAborts); EndKinesis(now, 4000, "broken off"); return; }
    Vec3 objectAt;
    float at[3]{};
    if (u->tkBody && Read(u->tkBody + tk::kBodyPos, at, sizeof(at)) && std::isfinite(at[0])) objectAt = {at[0], at[1], at[2]};
    else { EndKinesis(now, 3000, "the object is gone"); return; }
    // The brain keeps thinking, but the gun stays quiet and the aim held. Taking
    // the object it stands still; holding it, it walks where the brain goes.
    const coop_bot::Command brain = c;
    c.move = false;
    c.run = false;
    c.fire = c.holdFire = c.melee = c.reload = c.stasis = c.heal = c.use = c.mash = c.altFire = c.holdAlt = c.stomp = false;
    c.slot = -1;
    c.aim = true;
    c.look = true;
    const DWORD inStep = now - u->tkStepAt;
    const uint32_t held = TkHeld(u->entity);
    // An enemy within a metre: no throw now.
    bool crowded = false;
    for (int i = 0; i < p.enemyCount && !crowded; ++i) {
        const coop_bot::Enemy& e = p.enemies[i];
        crowded = e.health > 0.0f && !e.objective && (coop_bot::Flat(self, e.pos) < kTkTooClose || coop_bot::Distance(self, e.chest) < kTkTooClose);
    }
    switch (u->tkStep) {
    case TkStep::Raise:
        c.lookAt = objectAt;
        if (Aiming() && inStep >= 250) { u->tkStep = TkStep::Grab; u->tkStepAt = now; }
        else if (inStep > 1500) { InterlockedIncrement(&tkMisses); Shun(u->tkObject, now); EndKinesis(now, 2000, "never came to aim"); }
        break;
    case TkStep::Grab:
        c.lookAt = objectAt;
        u->tkRedirect = true;
        u->tkRedirectTo = objectAt;
        if (held == u->tkBody || (held && inStep > 50)) {
            if (held != u->tkBody) u->tkBody = held;   // the game may pick another body of it (or a limb part)
            u->tkStep = TkStep::Hold;
            u->tkStepAt = now;
            u->tkRedirect = false;
            u->tkWants = false;
            u->tkLagSince = u->tkHiddenSince = u->tkSightAt = 0;
            u->tkHeldSince = now;
            u->tkMoveCheckAt = 0;
            u->tkHidden = false;
            NoteBotThrown(u->tkObject, now);
            NoteBotThrown(u->tkBody, now);
            InterlockedIncrement(&tkGrabs);
            break;
        }
        if (!u->tkPresses || (inStep > 900 && !held)) {
            if (u->tkPresses >= 2) { InterlockedIncrement(&tkMisses); Shun(u->tkObject, now); EndKinesis(now, 2000, "two grabs did not take it"); break; }
            ++u->tkPresses;
            u->tkStepAt = now;
            u->tkMedpackUntil = now + 70;
        }
        break;
    case TkStep::Hold: {
        if (!held) { InterlockedIncrement(&tkLost); EndKinesis(now, 3000, "lost its hold"); break; }
        if (!u->alert) { InterlockedIncrement(&tkCalmDrops); EndKinesis(now, 3000, "the alert is over, put it down"); c.aim = false; break; }
        if (brain.heal) { InterlockedIncrement(&tkHealDrops); EndKinesis(now, 3000, "has to heal, dropped it"); c.aim = false; break; }
        // Snagged: the hold only pulls the object toward a point in front of the
        // bot, it does not pass through walls, so at a corner or a door frame it
        // hangs back. Too far behind or out of sight for a moment: drop it and go on.
        {
            const Vec3 chest = {self.x, self.y + 1.1f, self.z};
            const float lag = coop_bot::Distance(objectAt, chest);
            const LONG lagCm = static_cast<LONG>(lag * 100.0f);
            if (lagCm > tkMaxLagCm) InterlockedExchange(&tkMaxLagCm, lagCm);
            u->tkLagSince = lag > kTkSnagLag ? (u->tkLagSince ? u->tkLagSince : now) : 0;
            if (now - u->tkSightAt >= 100) {
                u->tkSightAt = now;
                u->tkHidden = !bot_sense::Clear(CameraEye(self), objectAt, u->entity, u->tkObject);
            }
            u->tkHiddenSince = u->tkHidden ? (u->tkHiddenSince ? u->tkHiddenSince : now) : 0;
            const bool lagging = u->tkLagSince && now - u->tkLagSince > kTkSnagMs, hidden = u->tkHiddenSince && now - u->tkHiddenSince > kTkSnagMs;
            if (lagging || hidden) {
                InterlockedIncrement(&tkSnagDrops);
                if (InterlockedIncrement(&tkLogLines) <= 120)
                    gate::Event("bot %u kinesis: the object is stuck (%.1f m from its chest%s), dropped it", u->index + 1, lag, hidden ? ", out of sight" : "");
                EndKinesis(now, 3000, "snagged");
                Shun(u->tkObject, now);
                c = brain;   // back to the brain's frame at once: walk on, the gun free
                c.aim = false;
                c.fire = false;
                break;
            }
        }
        u->tkCloseSince = crowded ? (u->tkCloseSince ? u->tkCloseSince : now) : 0;
        if (u->tkCloseSince && now - u->tkCloseSince > kTkCloseDropMs) {
            InterlockedIncrement(&tkCloseDrops);
            EndKinesis(now, 3000, "an enemy at arm's length, dropped it to fight");
            c.aim = false;
            break;
        }
        c.move = brain.move;
        c.moveTo = brain.moveTo;
        // Carried too long without a throw, or it wants to walk and does not get
        // anywhere with the thing in hand (a corner, a door frame; 27.09. a bot
        // stood in a corner with a stasis canister): put it down and go on.
        const char* giveUp = nullptr;
        if (now - u->tkHeldSince > kTkCarryMs) {
            giveUp = "carried it too long without a throw";
            InterlockedIncrement(&tkCarryDrops);
        } else if (c.move && coop_bot::Flat(self, c.moveTo) > 1.0f) {
            if (!u->tkMoveCheckAt) { u->tkMoveCheckAt = now; u->tkMoveFrom = self; }
            else if (now - u->tkMoveCheckAt >= kTkStuckMs) {
                if (coop_bot::Flat(self, u->tkMoveFrom) < kTkStuckMove) { giveUp = "does not get anywhere with it"; InterlockedIncrement(&tkStuckDrops); }
                u->tkMoveCheckAt = now;
                u->tkMoveFrom = self;
            }
        } else {
            u->tkMoveCheckAt = 0;
        }
        if (giveUp) {
            EndKinesis(now, 4000, giveUp);
            Shun(u->tkObject, now);
            c = brain;
            c.aim = false;
            c.fire = false;
            break;
        }
        Vec3 aim, enemyAt;
        uint32_t target = 0;
        bool regrowing = false;
        if (!ThrowPoint(p, brain, aim, enemyAt, target, regrowing)) {
            // Nothing to throw at yet: carry it, looking where the brain looks.
            c.look = brain.look;
            c.lookAt = brain.lookAt;
            u->tkTarget = 0;
            u->tkWaitWhy = regrowing ? "target regrowing" : "no target in sight";
            InterlockedIncrement(regrowing ? &tkWaitRegrow : &tkWaitNoTarget);
            break;
        }
        if (target != u->tkTarget) { u->tkTarget = target; u->tkAimAt = now; }
        u->tkAim = aim;
        c.lookAt = aim;
        const float range = coop_bot::Distance(self, aim);
        float heading = 0.0f, pitch = 0.0f, off = 180.0f;
        if (AimAngles(u->entity, kGunAim, heading, pitch)) off = AngleDeg(Direction(heading, pitch), coop_bot::Sub(aim, CameraEye(self)));
        // The first rule that holds the throw back, counted per frame (tk_wait_*).
        const char* wait = nullptr;
        volatile LONG* waitCount = nullptr;
        if (inStep < 350) wait = "settling";
        else if (range > kTkThrowMax) { wait = "target out of range"; waitCount = &tkWaitRange; }
        else if (crowded) { wait = "an enemy within a metre"; waitCount = &tkWaitCrowded; }
        else if (!LaneClear(objectAt, aim, leaderPos)) { wait = "a teammate in the flight path"; waitCount = &tkWaitLane; }
        else if (u->tkCanister && !BlastClear(enemyAt, self, leaderPos)) { wait = "a teammate by the canister's target"; waitCount = &tkWaitBlast; }
        else if (!(off < (range < 4.0f ? 12.0f : 6.0f) || now - u->tkAimAt >= 1200)) { wait = "aiming"; waitCount = &tkWaitAim; }
        if (wait) u->tkWaitWhy = wait;
        if (waitCount) InterlockedIncrement(waitCount);
        const bool ready = !wait;
        if (ready) {
            u->tkStep = TkStep::Throw;
            u->tkStepAt = now;
            u->tkPresses = 0;
            u->tkRedirect = true;
            u->tkRedirectTo = aim;
        }
        break;
    }
    case TkStep::Throw:
        c.move = brain.move;
        c.moveTo = brain.moveTo;
        c.lookAt = u->tkAim;
        u->tkRedirectTo = u->tkAim;
        if (!held) {
            InterlockedIncrement(&tkThrows);
            NoteBotThrown(u->tkObject, now);
            NoteBotThrown(u->tkBody, now);
            u->tkReleasedAt = now;
            EndKinesis(now, 3000, "thrown");
            u->tkRedirect = true;   // the launch runs in this frame or the next; the swap stays 300 ms
            u->tkRerollAt = now + kRerollMs;
            const int medpacks = MedPacks(u->entity);
            if (u->tkMedpacks >= 0 && medpacks >= 0 && medpacks < u->tkMedpacks) InterlockedExchangeAdd(&tkMedpacksLost, u->tkMedpacks - medpacks);
            break;
        }
        if (!u->tkPresses || inStep > 600) {
            if (u->tkPresses >= 2) { InterlockedIncrement(&tkNoLaunch); EndKinesis(now, 4000, "fire did not throw, dropped it"); c.aim = false; break; }
            ++u->tkPresses;
            u->tkStepAt = now;
            // The throw is the machine's launch transition, let through for the bot
            // (OnTkTransition): the fire button it wants is read raw from a pad
            // that counts as connected, and a plugged-in pad did not do it either.
            u->tkLaunchWanted = true;
            InterlockedIncrement(&tkFirePulses);
        }
        break;
    default:
        break;
    }
}
// PlayerFireTKSM::CheckTransition (PC 0x0089B2E0, thiscall(sm, a, b, transition,
// c), ret 0x10). Transition 12 (LAUNCH_VALID) is IsLaunchButtonPressed
// (0x00873BD0, fire read raw through DeviceManager::CheckButtonsANY 0x0077FE90,
// nothing on an unplugged pad) and flag 8 of sm+0x100 (the launch target is
// valid) and not flag 2 (snapped into a receptacle). For a bot that means to
// throw, a refused launch goes through; why it was refused is logged.
// A throw is two launch checks in one Update (state table 0x0089AF10, the state
// at sm+0x0C, written before the new state's checks): hold (4) goes to
// holdToRelease (6) on transition 12, and holdToRelease to launchObject (11) on
// 12 again. A human's press is a rising edge in both; the bot's pass used to end
// after the first, and the machine sat in holdToRelease, where nothing steers
// the object, until the retry 600 ms later (05.10.: 14 forced passes for 7
// throws). So both go through; a pass in any other state is refused and counted.
constexpr uint32_t kTkTransitionRva = 0x49B2E0, kTkLaunchTransition = 12, kTkSmFlags = 0x100, kIsLaunchPressedRva = 0x473BD0;
constexpr uint32_t kTkSmState = 0x0C, kTkSmStateTime = 0x30;
constexpr int32_t kTkStateHold = 4, kTkStateHoldToRelease = 6;
safetyhook::InlineHook tkTransitionHook;
volatile LONG tkForcedLaunches = 0, tkForcedHold = 0, tkForcedRelease = 0, tkLaunchOtherState = 0, tkLaunchLines = 0;
uint32_t __fastcall OnTkTransition(void* sm, void*, uint32_t a, uint32_t b, uint32_t transition, uint32_t d) {
    const uint32_t result = tkTransitionHook.thiscall<uint32_t>(sm, a, b, transition, d);
    if (transition != kTkLaunchTransition || (result & 0xFF)) return result;
    uint32_t owner = 0, body = 0;
    const uintptr_t machine = reinterpret_cast<uintptr_t>(sm);
    if (!Read(machine + tk::kSmOwner, &owner, 4)) return result;
    Unit* unit = UnitOf(owner);
    if (!unit || !unit->tkLaunchWanted || unit->tkStep != TkStep::Throw) return result;
    int32_t state = -1;
    float inState = 0.0f;
    Read(machine + kTkSmState, &state, 4);
    Read(machine + kTkSmStateTime, &inState, 4);
    if (state != kTkStateHold && state != kTkStateHoldToRelease) {
        if (InterlockedIncrement(&tkLaunchOtherState) <= 10)
            gate::Event("bot %u kinesis: launch check in machine state %d, not let through", unit->index + 1, state);
        return result;
    }
    unsigned char flags = 0;
    Read(machine + kTkSmFlags, &flags, 1);
    if ((flags & 2) || !Read(machine + tk::kSmBody, &body, 4) || !body) return result;
    if (state == kTkStateHoldToRelease) unit->tkLaunchWanted = false;   // the launch itself; one in hold only starts it
    InterlockedIncrement(&tkForcedLaunches);
    InterlockedIncrement(state == kTkStateHold ? &tkForcedHold : &tkForcedRelease);
    if (InterlockedIncrement(&tkLaunchLines) <= 30) {
        bool button = false;
        __try { button = reinterpret_cast<bool(__thiscall*)(void*)>(base + kIsLaunchPressedRva)(sm); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        gate::Event("bot %u kinesis: launch let through in state %d (%s, %.2f s in it; button %d, flags %02X: launch target %s)", unit->index + 1, state,
                    state == kTkStateHold ? "hold" : "holdToRelease", std::isfinite(inState) ? inState : -1.0f, button ? 1 : 0, flags,
                    (flags & 8) ? "valid" : "not valid");
    }
    return (result & ~0xFFu) | 1u;
}
bool InstallKinesis(FILE* log) {
    if (!kinesisWanted) { std::fprintf(log, "bot kinesis=0\n"); return false; }
    unsigned char type = 0;
    Read(base + kTkTypeRva, &type, 1);
    // push ebp / mov ebp,esp / and esp,-16 / sub esp,18h / push esi / mov esi,ecx
    static const unsigned char kAttack[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x18, 0x56, 0x8B, 0xF1};
    // push ebp / mov ebp,esp / and esp,-16 / sub esp,0F4h / push ebx / push esi / push edi
    static const unsigned char kKeyframe[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xF4, 0x00, 0x00, 0x00, 0x53, 0x56, 0x57};
    const bool attack = hook_site::Install(log, "bot PlayerFireTKSM::ExecuteAttack", base + kTkAttackRva, kAttack, sizeof(kAttack),
                                           reinterpret_cast<void*>(OnTkAttack), tkAttackHook);
    const bool keyframe = attack && hook_site::Install(log, "bot PlayerFireTKSM::ApplyKeyframeToTarget", base + kTkKeyframeRva, kKeyframe, sizeof(kKeyframe),
                                                       reinterpret_cast<void*>(OnTkKeyframe), tkKeyframeHook);
    // push ebp / mov ebp,esp / and esp,-16 / sub esp,2Ch / push esi / mov esi,ecx / mov ecx,[ebp+10h]
    static const unsigned char kTransition[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x2C, 0x56, 0x8B, 0xF1, 0x8B, 0x4D, 0x10};
    const bool transition = keyframe && hook_site::Install(log, "bot PlayerFireTKSM::CheckTransition", base + kTkTransitionRva, kTransition,
                                                           sizeof(kTransition), reinterpret_cast<void*>(OnTkTransition), tkTransitionHook);
    // The component type is still 0 when the game starts at its menu; the scan
    // reads it again each time (28.09.: kinesis stayed off for a whole new game).
    kinesisReady = attack && keyframe && transition;
    std::fprintf(log, "bot kinesis=%d (grab_hook=%d launch_hook=%d launch_transition=%d tk_component_type=%u, read again at each scan: bots throw loose props at their target now and then)\n",
                 kinesisReady ? 1 : 0, attack ? 1 : 0, keyframe ? 1 : 0, transition ? 1 : 0, type);
    return kinesisReady;
}

// Where the aim is measured from: the muzzle, turned with the gun's heading now,
// while a recent muzzle matrix is known; else the camera.
Vec3 AimOrigin(const Vec3& self, DWORD now) {
    float heading = 0.0f, pitch = 0.0f;
    if (!u->muzzleAt || now - u->muzzleAt > 3000 || !AimAngles(u->entity, kGunAim, heading, pitch)) return CameraEye(self);
    const float sx = std::sin(heading), sz = std::cos(heading);
    return {self.x + u->muzzleRight * sz + u->muzzleForward * sx, self.y + u->muzzleUp, self.z - u->muzzleRight * sx + u->muzzleForward * sz};
}
// --- zero-G flight -----------------------------------------------------------------------
// Zero-G is per Isaac and per spot:
// its CharacterMovementComponent asks the gravity fields every tick (+0x144 bit 0
// = in gravity). Flying is lower body state 20 (zgJump) with the zero-G machine
// below it (1 quickTakeoff, 2 launch, 3 suspendedTakeoff, 4 float, 5 realign, 6
// land, 11 touchDown, 12 exit). One edge of pad bit 0x4000 takes off from a
// surface and, in float with a landing spot found (+0x8C == 0, the spot at +0x110,
// searched along the gravity alignment within 4 m), lands; nothing else lands a
// flying Isaac. The bots take off after the leader, fly to their places behind him
// (the look through bot_aim's zero-G stub, thrust on the left stick) and land by
// him once he stands. Before this they only floated where the game lifted them.
constexpr uint32_t kZgLanding = 0x8C, kZgTouchPoint = 0x110;
constexpr uint16_t kZgJump = 0x4000;
constexpr DWORD kTakeoffDelayMs = 450, kLandRetryMs = 900, kLandGiveUpMs = 10000, kPulseMs = 70;
volatile LONG zgTakeoffs = 0, zgSurfaceTakeoffs = 0, zgLandPulses = 0, zgLandings = 0, zgBackToFloat = 0, zgNoSpot = 0, zgTeleports = 0,
              zgSignFlips = 0, zgFlyingFrames = 0, zgTrailFrames = 0, zgUnknownBody = 0, zgLogLines = 0, zgAimFrames = 0, zgFightFrames = 0,
              zgFightOnTarget = 0;
constexpr float kZgFightTurnGain = 4.0f;   // the travel gain is 1.6 (FlightTuning)
bool ZgLog() { return InterlockedIncrement(&zgLogLines) <= 400; }
coop_bot::BodyFrame FallbackFrame(const Vec3& pos) {
    coop_bot::BodyFrame f;
    f.pos = pos;
    f.forward = {0.0f, 0.0f, 1.0f};
    f.left = {1.0f, 0.0f, 0.0f};
    f.up = {0.0f, 1.0f, 0.0f};
    return f;
}
// The leader's side of it, once a frame.
void UpdateLeaderFlight(uint32_t leader, const Vec3& leaderPos, const Machines& m, DWORD now) {
    leaderLower = m.lower;
    const bool flyingNow = m.lower == 20;
    if (flyingNow != leaderFlying && zeroGFlight && ZgLog())
        gate::Event(flyingNow ? "bot: the leader flies (zero-G, lower body state %d)" : "bot: the leader is on his feet again (lower body state %d)", m.lower);
    if (flyingNow && !leaderFlying) leaderFlyingSince = now;
    if (!flyingNow) leaderFlyingSince = 0;
    leaderFlying = flyingNow;
    uint32_t flags = 1;
    leaderBodyKnown = BodyOf(leader, leaderPos, leaderBody, flags);
    const bool zeroGNow = leaderBodyKnown && !(flags & 1);
    if (zeroGNow != leaderZeroG && zeroGFlight && ZgLog()) gate::Event("bot: the leader %s zero-G (gravity flags %X)", zeroGNow ? "is in" : "left", flags);
    leaderZeroG = zeroGNow;
}
// The bot's side, from Sense.
void NoteFlight(const Machines& m, const Vec3& self, DWORD now) {
    u->lowerState = m.lower;
    u->zgSm = m.zeroGSm;
    const bool flyingNow = m.lower == 20;
    if (m.zeroG != u->zgState) {
        if (u->zgState == 6 && m.zeroG == 4) InterlockedIncrement(&zgBackToFloat);
        if (zeroGFlight && ZgLog()) gate::Event("bot %u: zero-G machine %d -> %d (lower body %d)", u->index + 1, u->zgState, m.zeroG, m.lower);
        u->zgState = m.zeroG;
    }
    if (u->flying && !flyingNow) {
        if (u->landPulseAt && now - u->landPulseAt < 8000) InterlockedIncrement(&zgLandings);
        if (zeroGFlight && ZgLog())
            gate::Event("bot %u: on its feet again (lower body %d) after %lu ms of flight", u->index + 1, m.lower,
                        static_cast<unsigned long>(u->flightStart ? now - u->flightStart : 0));
        u->zgWantsCatchUp = false;
    }
    u->flying = flyingNow;
    uint32_t flags = 1;
    u->bodyKnown = BodyOf(u->entity, self, u->body, flags);
    u->zeroG = u->bodyKnown && !(flags & 1);
}
// The look signs of the zero-G stub, checked against the body in float: a
// command that turned it the other way six times running flips that sign.
void LearnFlightSigns(DWORD now) {
    if (u->zgState != 4) { u->signFrameAt = 0; return; }
    const coop_bot::BodyFrame& b = u->body;
    if (u->signFrameAt && now - u->signFrameAt >= 100) {
        const coop_bot::BodyFrame& was = u->signFrame;
        const float along = coop_bot::Dot(b.forward, was.forward);
        const float turnedLeft = std::atan2(coop_bot::Dot(b.forward, was.left), along), turnedUp = std::atan2(coop_bot::Dot(b.forward, was.up), along);
        if (std::fabs(u->signYaw) >= 0.3f && (u->flightYaw > 0.0f) == (u->signYaw > 0.0f) && std::fabs(turnedLeft) > 0.01f) {
            u->yawContrary = (turnedLeft > 0.0f) != (u->signYaw > 0.0f) ? u->yawContrary + 1 : 0;
            if (u->yawContrary >= 6) {
                u->flightYawSign = -u->flightYawSign;
                u->yawContrary = 0;
                InterlockedIncrement(&zgSignFlips);
                gate::Event("bot %u: its body turned against the zero-G yaw command six times: yaw sign now %+.0f", u->index + 1, u->flightYawSign);
            }
        }
        if (std::fabs(u->signPitch) >= 0.3f && (u->flightPitch > 0.0f) == (u->signPitch > 0.0f) && std::fabs(turnedUp) > 0.01f) {
            u->pitchContrary = (turnedUp < 0.0f) != (u->signPitch > 0.0f) ? u->pitchContrary + 1 : 0;
            if (u->pitchContrary >= 6) {
                u->flightPitchSign = -u->flightPitchSign;
                u->pitchContrary = 0;
                InterlockedIncrement(&zgSignFlips);
                gate::Event("bot %u: its body pitched against the zero-G pitch command six times: pitch sign now %+.0f", u->index + 1, u->flightPitchSign);
            }
        }
        u->signFrameAt = 0;
    }
    if (!u->signFrameAt) {
        u->signFrame = b;
        u->signFrameAt = now;
        u->signYaw = u->flightYaw;
        u->signPitch = u->flightPitch;
    }
}
// Where a catch-up puts a bot while the leader is in zero-G: its place behind the
// flying leader, or a metre above his feet beside him (it floats there and lands).
Vec3 ZeroGSpot(const Vec3& leaderPos) {
    const coop_bot::BodyFrame lead = leaderBodyKnown ? leaderBody : FallbackFrame(leaderPos);
    const Vec3 chest = coop_bot::Add(leaderPos, lead.up);
    // Standing: the first to his left, the second to his right, the third behind him
    // (bots 1 and 3 shared the left spot before).
    static const float kSide[] = {0.9f, -0.9f, 0.0f}, kBack[] = {0.0f, 0.0f, 0.9f};
    const unsigned i = u->index % 3;
    Vec3 spot = leaderFlying ? coop_bot::FlightGoal(lead, true, static_cast<int>(u->index))
                             : coop_bot::Add(chest, coop_bot::Add(coop_bot::Scale(lead.left, kSide[i]), coop_bot::Scale(lead.forward, -kBack[i])));
    if (bot_sense::losReady && !bot_sense::Clear(chest, spot, Leader(), 0)) spot = chest;
    return spot;
}
// One frame of it. On its feet it may take off (false: the ground logic goes on);
// flying it owns the stick and the look (true).
bool FlightTick(const Vec3& self, const Vec3& leaderPos, DWORD now, Pad& p, uint16_t& buttons) {
    if (!zeroGFlight) return false;
    const coop_bot::Command& c = u->command;
    if (!u->flying) {
        u->landingSince = u->flightStart = 0;
        if (!u->zeroG || !u->bodyKnown || !leaderBodyKnown || now - u->takeoffAt < 1500 || u->holding) return false;   // a holding bot stays put
        const int32_t s = u->lowerState;
        if (s != 1 && s != 3 && s != 8 && s != 17) return false;
        const float apart = coop_bot::Distance(self, leaderPos);
        bool go = false, surface = false;
        if (leaderFlying) {
            u->otherSurfaceSince = 0;
            go = leaderFlyingSince && now - leaderFlyingSince >= kTakeoffDelayMs + 300 * u->index;
        } else if (leaderZeroG && apart > 3.0f && coop_bot::OtherSurface(u->body, leaderBody)) {
            if (!u->otherSurfaceSince) u->otherSurfaceSince = now;
            go = surface = now - u->otherSurfaceSince >= 1200;
        } else {
            u->otherSurfaceSince = 0;
        }
        if (!go) return false;
        u->takeoffAt = now;
        u->otherSurfaceSince = 0;
        Press(kZgJump, now, kPulseMs);
        buttons = static_cast<uint16_t>((buttons | kZgJump) & ~(kAim | kFire | kAltFire));
        InterlockedIncrement(surface ? &zgSurfaceTakeoffs : &zgTakeoffs);
        if (ZgLog())
            gate::Event("bot %u: takes off %s (zero-G, %.1f m from him, lower body %d)", u->index + 1,
                        surface ? "toward the leader on another surface" : "after the flying leader", apart, s);
        return false;
    }
    InterlockedIncrement(&zgFlyingFrames);
    u->steer.Reset();
    if (!u->flightStart) u->flightStart = now;
    if (!u->bodyKnown) {
        InterlockedIncrement(&zgUnknownBody);
        p = Pad{};
        buttons = static_cast<uint16_t>(buttons & ~(kAim | kFire | kAltFire | kRun));
        bot_aim::SetFloatLook(u->entity, 0.0f, 0.0f, now);
        return true;
    }
    LearnFlightSigns(now);
    // Where to: its place by the leader; the leader's trail when a wall is in the way.
    const coop_bot::BodyFrame lead = leaderBodyKnown ? leaderBody : FallbackFrame(leaderPos);
    const Vec3 goal = coop_bot::FlightGoal(lead, leaderFlying, static_cast<int>(u->index));
    const Vec3 up = u->body.up;
    const Vec3 eye = coop_bot::Add(self, coop_bot::Scale(up, 1.3f));
    if (now - u->zgClearAt >= 250) {
        u->zgClearAt = now;
        u->zgClear = !bot_sense::losReady || bot_sense::Clear(eye, coop_bot::Add(goal, coop_bot::Scale(up, 1.3f)), u->entity, 0);
    }
    Vec3 flyTo = goal;
    bool onTrail = false;
    if (!u->zgClear) {
        Vec3 next;
        if (trail.NextFrom(self, next)) { flyTo = next; onTrail = true; InterlockedIncrement(&zgTrailFrames); }
    }
    // The look: the brain's target in a fight near its place, else the way.
    const bool fight = c.look && (c.aim || c.fire || c.holdFire || c.altFire) && coop_bot::Distance(self, goal) < 7.0f;
    if (!fight) buttons = static_cast<uint16_t>(buttons & ~(kAim | kFire | kAltFire));
    coop_bot::BodyFrame head = u->body;
    head.pos = eye;
    const Vec3 wayAt = coop_bot::Add(flyTo, coop_bot::Scale(up, 1.3f));
    // On an enemy it turns faster: at the travel gain a target 10 deg off got a
    // look command near the curve's dead zone and the aim trailed (06.10.).
    coop_bot::FlightTuning tuning;
    if (fight) tuning.turnGain = kZgFightTurnGain;
    const coop_bot::FlightStick s = coop_bot::FlyToward(head, wayAt, fight ? c.lookAt : wayAt, fight, (buttons & kAim) != 0, tuning);
    if (fight) {
        InterlockedIncrement(&zgFightFrames);
        if (s.lookOffDeg <= 5.0f) InterlockedIncrement(&zgFightOnTarget);
    }
    u->flightYaw = s.yaw;
    u->flightPitch = s.pitch;
    bot_aim::SetFloatLook(u->entity, s.yaw * u->flightYawSign, s.pitch * u->flightPitchSign, now);
    p.lx = s.lx;
    p.ly = s.ly;
    buttons = static_cast<uint16_t>(s.boost ? (buttons | kRun) : (buttons & ~kRun));
    if (!(buttons & kAim)) buttons = static_cast<uint16_t>(buttons & ~kAltFire);   // in flight that button realigns
    // Down by the leader once he stands: the pulse over a landing spot near his feet.
    int32_t noSpot = -1;
    float touch[3]{};
    if (leaderFlying) {
        u->landingSince = 0;
    } else {
        if (!u->landingSince) u->landingSince = now;
        const bool spotRead = u->zgSm && Read(u->zgSm + kZgLanding, &noSpot, 4) && Read(u->zgSm + kZgTouchPoint, touch, sizeof(touch));
        const Vec3 touchAt = {touch[0], touch[1], touch[2]};
        if (u->zgState == 4 && coop_bot::Distance(self, goal) < 3.0f && now - u->landPulseAt >= kLandRetryMs) {
            if (spotRead && noSpot == 0 && coop_bot::Distance(touchAt, leaderPos) < 3.0f) {
                u->landPulseAt = now;
                u->landHoldUntil = now + 900;
                Press(kZgJump, now, kPulseMs);
                buttons = static_cast<uint16_t>(buttons | kZgJump);
                InterlockedIncrement(&zgLandPulses);
                if (ZgLog())
                    gate::Event("bot %u: lands by the leader (spot %.1f m from his feet, %lu ms after he stood)", u->index + 1,
                                coop_bot::Distance(touchAt, leaderPos), static_cast<unsigned long>(now - u->landingSince));
            } else if (now - u->noSpotLogAt >= 3000) {
                u->noSpotLogAt = now;
                InterlockedIncrement(&zgNoSpot);
                if (ZgLog())
                    gate::Event("bot %u: no landing spot by the leader yet (flag %d, spot %.1f m from his feet)", u->index + 1, noSpot,
                                spotRead ? coop_bot::Distance(touchAt, leaderPos) : -1.0f);
            }
        }
        if (now - u->landingSince >= kLandGiveUpMs) {
            u->landingSince = now;
            u->zgWantsCatchUp = true;   // CatchUpTick puts it by him
        }
    }
    if (u->landHoldUntil && now < u->landHoldUntil) {
        p.lx = p.ly = 0.0f;
        buttons = static_cast<uint16_t>(buttons & ~(kAim | kFire | kAltFire | kRun));
    }
    if (now - u->zgLogAt >= 2000 && ZgLog()) {
        u->zgLogAt = now;
        gate::Event("bot %u zero-G: machine %d, %.1f m to its place%s, look off %.0f deg, look %.2f/%.2f (signs %+.0f/%+.0f), stick %.2f/%.2f%s, "
                    "leader %s, landing flag %d",
                    u->index + 1, u->zgState, coop_bot::Distance(self, goal), onTrail ? " (on his trail)" : "", s.lookOffDeg, s.yaw, s.pitch,
                    u->flightYawSign, u->flightPitchSign, p.lx, p.ly, s.boost ? " boost" : "", leaderFlying ? "flying" : (leaderZeroG ? "on a zero-G surface" : "in gravity"),
                    noSpot);
    }
    return true;
}
// Vacuum air: every Isaac counts down together (the environment message reaches
// all), but an air canister refills only the one who picks it up. A bot with less
// air than the leader gets PlayerAir::RefillMax (PC 0x0086E3D0, thiscall(air),
// ret): PlayerAir = [player+0xFC0] - 0x10, max +0x70, left +0x74, flags +0x7C
// (bit 0 counting down, bit 1 refilling).
constexpr uint32_t kPlayerAirRef = 0xFC0, kAirMax = 0x70, kAirFlags = 0x7C, kRefillMaxRva = 0x46E3D0;
bool shareAir = false;
DWORD airAt = 0;
volatile LONG airRefills = 0, airFaults = 0;
bool AirOf(uint32_t player, uint32_t& air, float& left, uint8_t& flags) {
    uint32_t ref = 0;
    float values[2]{};
    if (!player || !Read(player + kPlayerAirRef, &ref, 4) || ref < 0x10010) return false;
    air = ref - 0x10;
    if (!Read(air + kAirMax, values, sizeof(values)) || !Read(air + kAirFlags, &flags, 1)) return false;
    if (!std::isfinite(values[0]) || values[0] < 5.0f || values[0] > 3600.0f || !std::isfinite(values[1])) return false;
    left = values[1];
    return true;
}
bool RefillAir(uint32_t air) {
    __try {
        reinterpret_cast<void(__thiscall*)(uint32_t)>(base + kRefillMaxRva)(air);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&airFaults);
        return false;
    }
}
void ShareAir(uint32_t leader, DWORD now) {
    if (!shareAir || now - airAt < 500) return;
    airAt = now;
    uint32_t leaderAir = 0;
    float leaderLeft = 0.0f;
    uint8_t leaderFlags = 0;
    if (!AirOf(leader, leaderAir, leaderLeft, leaderFlags)) return;
    const bool leaderBreathes = !(leaderFlags & 1);
    for (const Unit& unit : units) {
        uint32_t air = 0;
        float left = 0.0f;
        uint8_t flags = 0;
        if (!unit.entity || !AirOf(unit.entity, air, left, flags) || !(flags & 1) || (flags & 2)) continue;
        if (!leaderBreathes && left >= leaderLeft - 2.0f) continue;
        if (!RefillAir(air)) continue;
        if (InterlockedIncrement(&airRefills) <= 40)
            gate::Event("bot %u: air refilled (it had %.0f s, the leader %s %.0f s)", unit.index + 1, left, leaderBreathes ? "breathes, at" : "has", leaderLeft);
    }
}

// --- the leader's orders ---------------------------------------------------------------
// [Bot] FollowHoldKey, GoThereKey, AttackTargetKey, FireHoldKey, VanguardKey: one key
// each, acting on its press, only in a level with a bot standing and the game window
// focused. The squad's state lives here; each bot's brain gets it through
// Perception::orders (coop_bot_brain.h) and decides what it means there.
//  * Follow / Hold: each bot holds the spot it stands on. No catch-up moves a holding
//    bot (the call key ends the hold), and lifts do not wait for it.
//  * Go there: the point under the leader's crosshair, his camera ray against the
//    world (CollisionCamera::CastingTest, 40 m), dropped to the floor and checked
//    against the NavPower mesh; the bots spread around it and hold there.
//  * Attack target: the necromorph under his crosshair is every bot's first target
//    for 20 s or until it dies.
//  * Hold fire, and the attack formation: switches the brain reads.
// A level change or a checkpoint restart ends Hold, Go there and the target; hold
// fire and the formation stay as set.
enum class Stance : uint8_t { Follow, Hold, GoThere };
struct OrderKey { const char* name; int vk; int fallback; bool down; };
OrderKey orderKeys[] = {{"FollowHoldKey", 0, VK_NUMPAD1, false}, {"GoThereKey", 0, VK_NUMPAD2, false}, {"AttackTargetKey", 0, VK_NUMPAD3, false},
                        {"FireHoldKey", 0, VK_NUMPAD4, false}, {"VanguardKey", 0, VK_NUMPAD5, false}};
constexpr unsigned kOrderKeys = sizeof(orderKeys) / sizeof(orderKeys[0]);
Stance stance = Stance::Follow;
bool holdFire = false, vanguard = false;
uint32_t focusTarget = 0, focusHealth = 0, focusVtable = 0;
DWORD focusUntil = 0;
float leaderHealth01 = 1.0f;
bool leaderGrabbed = false;
uint32_t leaderGrabber = 0;   // "the leader in a grab"
constexpr DWORD kFocusMs = 20000;
constexpr float kOrderReach = 40.0f, kTargetReach = 45.0f, kReachFlat = 2.5f, kReachHeight = 2.5f;
volatile LONG orderPresses[kOrderKeys]{}, ordersIgnored = 0, goThereFails = 0, goThereNearest = 0, focusMisses = 0, focusKills = 0, focusExpiries = 0,
              focusLosses = 0, holdTeleportsSkipped = 0, holdCatchUpsSkipped = 0, holdSpotsMoved = 0, holdArrivals = 0, fireHeldFrames = 0,
              returnFireFrames = 0, focusFrames = 0, orderResets = 0, orderLines = 0;
bool OrderLog() { return InterlockedIncrement(&orderLines) <= 200; }
const char* StanceName() { return stance == Stance::Hold ? "hold" : (stance == Stance::GoThere ? "go there" : "follow"); }

void FillOrders(coop_bot::Perception& p) {
    p.orders.hold = u->holding;
    p.orders.holdSpot = u->holdSpot;
    p.orders.focus = focusTarget;
    p.orders.focusUntil = focusUntil;
    p.orders.holdFire = holdFire;
    p.orders.vanguard = vanguard;
    p.leaderHealth01 = leaderHealth01;
    p.leaderGrabbed = leaderGrabbed;
    p.leaderGrabber = leaderGrabber;
}
void ClearFocus(const char* why, volatile LONG* counter) {
    if (!focusTarget) return;
    if (counter) InterlockedIncrement(counter);
    if (OrderLog()) gate::Event("bot order: target %08X dropped (%s)", focusTarget, why);
    focusTarget = focusHealth = focusVtable = 0;
    focusUntil = 0;
    bot_sense::forcedEnemy = 0;
}
void SetFollow(const char* why) {
    for (Unit& unit : units) unit.holding = unit.goingThere = false;
    stance = Stance::Follow;
    Publish();
    if (OrderLog()) gate::Event("bot order: follow (%s)", why);
}
void ResetOrders(const char* why) {
    if (stance == Stance::Follow && !focusTarget) return;
    InterlockedIncrement(&orderResets);
    SetFollow(why);
    ClearFocus(why, nullptr);
}
// The forced target dies, goes, or its time is up.
void CheckFocus(DWORD now) {
    if (!focusTarget) return;
    uint32_t vtable = 0;
    float health = 0.0f;
    if (static_cast<LONG>(now - focusUntil) >= 0) ClearFocus("20 s are up", &focusExpiries);
    else if (!Read(focusTarget, &vtable, 4) || vtable != focusVtable) ClearFocus("the entity is gone", &focusLosses);
    else if (focusHealth && Read(focusHealth + hits::kHealthCurrent, &health, 4) && health <= 0.0f) ClearFocus("it died", &focusKills);
}

// The leader's crosshair: from his camera (viewport 0's blender) along his camera
// aim data (+0x20, the block the bots' own look turns). The view's own look point
// and his gun's aim are compared for the log; all three should agree closely.
bool LeaderRay(uint32_t leader, const Vec3& leaderPos, Vec3& eye, Vec3& dir, float& viewDeg, float& gunDeg) {
    eye = CameraEyeOf(leaderPos, 0);
    viewDeg = gunDeg = -1.0f;
    Vec3 view;
    bool haveView = false;
    const uint32_t cm = CameraManager();
    uint32_t blenders = 0, count = 0;
    if (cm && Read(cm + kCmBlenders, &blenders, 4) && blenders && Read(cm + kCmNumViewports, &count, 4) && count >= 1 && count <= 4)
        for (uint32_t viewport = 0; viewport < count && !haveView; ++viewport) {
            uint32_t player = 0;
            float at[8]{};
            if (!Read(cm + kCmPlayerIds + viewport * 4, &player, 4) || player != 0 || !Read(blenders + viewport * kBlenderSize, at, sizeof(at))) continue;
            const Vec3 d = {at[4] - at[0], at[5] - at[1], at[6] - at[2]};
            const float len = coop_bot::Length(d);
            if (std::isfinite(len) && len > 0.01f) { view = coop_bot::Scale(d, 1.0f / len); haveView = true; }
        }
    float heading = 0.0f, pitch = 0.0f;
    if (AimAngles(leader, kCamAim, heading, pitch)) {
        dir = Direction(heading, pitch);
        if (haveView) viewDeg = AngleDeg(dir, view);
        float gunHeading = 0.0f, gunPitch = 0.0f;
        if (AimAngles(leader, kGunAim, gunHeading, gunPitch)) gunDeg = AngleDeg(dir, Direction(gunHeading, gunPitch));
        return true;
    }
    if (!haveView) return false;
    dir = view;
    return true;
}
bool CastRay(const Vec3& from, const Vec3& to, uint32_t exclude, float& fraction, uint32_t& hit) {
    alignas(16) float a[4] = {from.x, from.y, from.z, 1.0f}, b[4] = {to.x, to.y, to.z, 1.0f};
    fraction = 1.0f;
    hit = 0;
    if (!bot_sense::losReady) return false;
    InterlockedIncrement(&bot_sense::losCasts);
    if (!bot_sense::CastRaw(a, b, exclude, fraction, hit)) { bot_sense::NoteLosFault(); return false; }
    fraction = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
    return true;
}
Vec3 DropToFloor(const Vec3& at, uint32_t exclude) {
    const Vec3 from = {at.x, at.y + 0.3f, at.z}, to = {at.x, at.y - 6.0f, at.z};
    float fraction = 1.0f;
    uint32_t hit = 0;
    if (!CastRay(from, to, exclude, fraction, hit) || fraction >= 0.999f) return at;
    return {at.x, from.y + (to.y - from.y) * fraction, at.z};
}
bool Standing(const Unit& unit) { return unit.entity && unit.state == State::Active; }

void OrderFollowHold(DWORD now) {
    if (stance != Stance::Follow) {
        SetFollow("the key");
        notice::Show("BOTS: FOLLOW");
        return;
    }
    char spots[160] = "";
    size_t used = 0;
    for (unsigned i = 0; i < wantedBots; ++i) {
        Unit& unit = units[i];
        if (!Standing(unit)) continue;
        unit.holding = true;
        unit.goingThere = false;
        unit.holdSpot = unit.self;
        unit.holdSince = now;
        if (used < sizeof(spots))
            used += static_cast<size_t>(std::snprintf(spots + used, sizeof(spots) - used, " bot %u (%.1f %.1f %.1f)", i + 1, unit.self.x, unit.self.y, unit.self.z));
    }
    stance = Stance::Hold;
    Publish();
    notice::Show("BOTS: HOLD");
    if (OrderLog()) gate::Event("bot order: hold, each where it stands:%s", spots);
}

void OrderGoThere(uint32_t leader, const Vec3& leaderPos, DWORD now) {
    Vec3 eye, dir;
    float viewDeg = -1.0f, gunDeg = -1.0f;
    if (!LeaderRay(leader, leaderPos, eye, dir, viewDeg, gunDeg)) {
        InterlockedIncrement(&goThereFails);
        notice::Show("BOTS: NO AIM");
        if (OrderLog()) gate::Event("bot order: go there: the leader's aim is not readable (aim data and view both failed)");
        return;
    }
    float fraction = 1.0f;
    uint32_t hitEntity = 0;
    const bool cast = CastRay(eye, coop_bot::Add(eye, coop_bot::Scale(dir, kOrderReach)), leader, fraction, hitEntity);
    const bool hit = cast && fraction < 0.999f;
    const float reach = kOrderReach * fraction;
    Vec3 end, floor;
    int points = 0;
    bool ok = false, nearest = false;
    const char* verdict = "no path to it";
    if (hit) {
        // A little short of the wall it met, so the point is in front of it.
        for (const float back : {0.6f, 2.0f}) {
            if (reach - back < 0.5f) break;
            floor = DropToFloor(coop_bot::Add(eye, coop_bot::Scale(dir, reach - back)), leader);
            if (!bot_nav::PathEnd(leaderPos, floor, end, points)) continue;
            ok = coop_bot::Flat(end, floor) <= kReachFlat && std::fabs(end.y - floor.y) <= kReachHeight;
            verdict = ok ? "reachable" : "the mesh ends short of it";
            break;
        }
    } else {
        // Nothing within reach: the nearest reachable point to the far end, else nearer along the ray.
        for (const float share : {1.0f, 0.75f, 0.5f, 0.25f}) {
            floor = DropToFloor(coop_bot::Add(eye, coop_bot::Scale(dir, kOrderReach * share)), leader);
            if (!bot_nav::PathEnd(leaderPos, floor, end, points)) continue;
            ok = true;
            nearest = coop_bot::Flat(end, floor) > kReachFlat || std::fabs(end.y - floor.y) > kReachHeight;
            verdict = nearest ? "nothing hit: the nearest reachable point" : "nothing hit: reachable";
            break;
        }
    }
    if (OrderLog())
        gate::Event("bot order: go there: eye (%.1f %.1f %.1f) dir (%.2f %.2f %.2f), view off %.1f deg, gun off %.1f deg, cast %d hit %d at %.1f m "
                    "entity %08X, floor (%.1f %.1f %.1f), path end (%.1f %.1f %.1f) %d points: %s",
                    eye.x, eye.y, eye.z, dir.x, dir.y, dir.z, viewDeg, gunDeg, cast ? 1 : 0, hit ? 1 : 0, reach, hitEntity, floor.x, floor.y, floor.z,
                    end.x, end.y, end.z, points, verdict);
    if (!ok) {
        InterlockedIncrement(&goThereFails);
        notice::Show("BOTS: CAN'T GET THERE");
        return;
    }
    if (nearest) InterlockedIncrement(&goThereNearest);
    // Spread around the point: the first on it, the next ones to either side.
    Vec3 forward = {end.x - leaderPos.x, 0.0f, end.z - leaderPos.z};
    const float len = coop_bot::Length(forward);
    forward = len > 0.1f ? coop_bot::Scale(forward, 1.0f / len) : coop_bot::FromYaw(bot_sense::Yaw(leader));
    const Vec3 side = {-forward.z, 0.0f, forward.x};
    static const float kSide[] = {0.0f, -1.6f, 1.6f};
    char spots[200] = "";
    size_t used = 0;
    unsigned k = 0;
    for (unsigned i = 0; i < wantedBots; ++i) {
        Unit& unit = units[i];
        if (!Standing(unit)) continue;
        Vec3 spot = end;
        if (k > 0) {
            const Vec3 want = coop_bot::Add(end, coop_bot::Scale(side, kSide[k % 3]));
            Vec3 at;
            int n = 0;
            if (bot_nav::PathEnd(end, want, at, n) && coop_bot::Flat(at, want) <= 0.8f && std::fabs(at.y - want.y) <= 1.5f) spot = at;
        }
        unit.holding = unit.goingThere = true;
        unit.holdSpot = spot;
        unit.holdSince = now;
        unit.steer.Reset();
        ++k;
        if (used < sizeof(spots))
            used += static_cast<size_t>(std::snprintf(spots + used, sizeof(spots) - used, " bot %u (%.1f %.1f %.1f) %.1f m off", i + 1, spot.x, spot.y,
                                                      spot.z, coop_bot::Distance(unit.self, spot)));
    }
    stance = Stance::GoThere;
    Publish();
    notice::Show(nearest ? "BOTS: GO THERE (NEAREST POINT)" : "BOTS: GO THERE");
    if (OrderLog()) gate::Event("bot order: go there, %.1f m from the leader:%s", coop_bot::Distance(leaderPos, end), spots);
}

// The necromorph under the leader's crosshair: the one his ray meets, else the
// nearest to the ray within 4 degrees (or 0.7 m) that he can see.
void OrderAttack(uint32_t leader, const Vec3& leaderPos, DWORD now) {
    Vec3 eye, dir;
    float viewDeg = -1.0f, gunDeg = -1.0f;
    if (!LeaderRay(leader, leaderPos, eye, dir, viewDeg, gunDeg)) {
        InterlockedIncrement(&focusMisses);
        notice::Show("BOTS: NO TARGET");
        if (OrderLog()) gate::Event("bot order: attack: the leader's aim is not readable");
        return;
    }
    float fraction = 1.0f;
    uint32_t hitEntity = 0;
    CastRay(eye, coop_bot::Add(eye, coop_bot::Scale(dir, kTargetReach)), leader, fraction, hitEntity);
    struct Pick { uint32_t entity, vtable, health; Vec3 at; float deg, dist; };
    Pick picks[4]{};
    int count = 0;
    unsigned npcs = 0, alive = 0;
    uint32_t node = 0;
    Read(base + bot_sense::kReflectableListRva, &node, 4);
    for (unsigned steps = 0; node && steps < 8192; ++steps) {
        const uint32_t entity = node - bot_sense::kEntityReflectable;
        uint32_t next = 0, vtable = 0;
        const bool more = Read(node + 4, &next, 4) && next != node;
        const uint32_t current = node;
        node = more ? next : 0;
        if (!npcs::IsNpcReflectable(current) || IsPlayerOrNecroEntity(entity) || IsBotEntity(entity) || phantom::Is(entity) || !Read(entity, &vtable, 4) ||
            vtable < base + gate::kRdataLo || vtable >= base + gate::kRdataHi)
            continue;
        ++npcs;
        Vec3 at;
        if (!bot_sense::Pos(entity, at) || coop_bot::Distance(eye, at) > kTargetReach || bot_sense::Hidden(entity) || guidfix::IsGhostEntity(entity) ||
            bot_sense::Neutral(entity, vtable))
            continue;
        const uint32_t health = hits::FindHealthComponent(entity);
        float left = 0.0f;
        if (!health || !Read(health + hits::kHealthCurrent, &left, 4) || !(left > 0.0f)) continue;
        ++alive;
        float best = 180.0f;
        for (const float h : {0.4f, 1.0f, 1.6f}) {
            const Vec3 v = coop_bot::Sub({at.x, at.y + h, at.z}, eye);
            const float t = coop_bot::Dot(v, dir);
            if (t < 1.0f) continue;
            const float off = coop_bot::Length(coop_bot::Sub(v, coop_bot::Scale(dir, t)));
            const float deg = std::atan2(off, t) * 57.2957795f, tolerance = std::fmax(4.0f, std::atan2(0.7f, t) * 57.2957795f);
            if (deg <= tolerance && deg < best) best = deg;
        }
        if (best >= 180.0f && entity != hitEntity) continue;
        if (entity == hitEntity) best = 0.0f;
        const Pick pick{entity, vtable, health, at, best, coop_bot::Distance(eye, at)};
        if (count < 4) picks[count++] = pick;
        else if (best < picks[3].deg) picks[3] = pick;
        else continue;
        for (int s = count - 1; s > 0 && picks[s - 1].deg > picks[s].deg; --s) std::swap(picks[s], picks[s - 1]);
    }
    const Pick* chosen = nullptr;
    bool seen = false;
    for (int i = 0; i < count && !chosen; ++i) {
        seen = picks[i].entity == hitEntity || bot_sense::Clear(eye, {picks[i].at.x, picks[i].at.y + 1.0f, picks[i].at.z}, leader, picks[i].entity);
        if (seen) chosen = &picks[i];
    }
    if (OrderLog())
        gate::Event("bot order: attack: ray hit %08X at %.1f m, view off %.1f deg, gun off %.1f deg; necromorphs %u alive in reach %u, near the ray %d%s",
                    hitEntity, kTargetReach * fraction, viewDeg, gunDeg, npcs, alive, count, chosen ? "" : ", none in sight");
    if (!chosen) {
        InterlockedIncrement(&focusMisses);
        notice::Show("BOTS: NO TARGET");
        return;
    }
    focusTarget = chosen->entity;
    focusVtable = chosen->vtable;
    focusHealth = chosen->health;
    focusUntil = now + kFocusMs;
    bot_sense::forcedEnemy = focusTarget;
    notice::Show("BOTS: ATTACK TARGET");
    if (OrderLog())
        gate::Event("bot order: attack %08X (vtable RVA %08X) at %.1f m, %.1f deg off the crosshair%s, for %lu s", chosen->entity, chosen->vtable - static_cast<uint32_t>(base),
                    chosen->dist, chosen->deg, chosen->entity == hitEntity ? " (the ray met it)" : "", static_cast<unsigned long>(kFocusMs / 1000));
}

// The keys, once a frame. A press outside a level with a bot standing does nothing.
void PollOrders(uint32_t leader, const Vec3& leaderPos, DWORD now) {
    const bool focused = necro::GameWindowFocused();
    bool pressed[kOrderKeys]{};
    bool any = false;
    for (unsigned k = 0; k < kOrderKeys; ++k) {
        OrderKey& key = orderKeys[k];
        const bool down = key.vk && focused && !shop::HoldsKey(key.vk, coop_shop::Guard::BotOrder) && (GetAsyncKeyState(key.vk) & 0x8000) != 0;
        pressed[k] = down && !key.down;
        key.down = down;
        any = any || pressed[k];
    }
    if (!any) return;
    bool standing = false;
    for (unsigned i = 0; i < wantedBots; ++i) standing = standing || Standing(units[i]);
    if (!standing || !leader || cutscene::SequenceRunningNow()) {
        InterlockedIncrement(&ordersIgnored);
        if (OrderLog()) gate::Event("bot order: key ignored (%s)", !standing ? "no bot standing" : (!leader ? "no leader" : "a scene runs"));
        return;
    }
    for (unsigned k = 0; k < kOrderKeys; ++k) {
        if (!pressed[k]) continue;
        InterlockedIncrement(&orderPresses[k]);
        switch (k) {
        case 0: OrderFollowHold(now); break;
        case 1: OrderGoThere(leader, leaderPos, now); break;
        case 2: OrderAttack(leader, leaderPos, now); break;
        case 3:
            holdFire = !holdFire;
            notice::Show(holdFire ? "BOTS: HOLD FIRE" : "BOTS: FIRE AT WILL");
            if (OrderLog()) gate::Event("bot order: %s", holdFire ? "hold fire (only fighting back)" : "fire at will");
            break;
        default:
            vanguard = !vanguard;
            notice::Show(vanguard ? "BOTS: ATTACK FORMATION" : "BOTS: COVER FORMATION");
            if (OrderLog()) gate::Event("bot order: %s", vanguard ? "attack formation (ahead of the leader)" : "cover formation (beside and behind the leader)");
            break;
        }
    }
}

// --- corpses, crates and lockers ([Bot] StompCorpses, LootContainers) ------------------
// A dead necromorph drops its loot once, on the first damaging hit after its
// death: DropItem 0x010B44E0
// sets HAS_DROPPED_ITEM (ItemDropListComponent +0x20 bit 31, type byte
// [0x020AC71A]) even for an empty roll and returns at once when it is set
// (0x010B44F0); bit 0 is DROP_ON_DEATH. That bit is the game's "looted" mark:
// the bots stomp a corpse until it is set, a few stomps at most. The bots' own
// corpse hits are refused a drop (KeepCorpseLoot), so a stomp on purpose names
// the corpse to runtime_coop_drops.inc first. Supply crates (BreakableContainer,
// vtable 0x01B402C0, same component) break under a stomp. Lockers
// (StandardContainer, vtable 0x01B425F8) open with use: state +0x23C (0 closed,
// 1 access granted, 2 opening, 3 open, 4 closing; CanTryToActivateContainer
// 0x00B7A640 refuses 2-4), locked byte +0x248 (HandleEvents 0x00BB7AC2), usable
// interface +0x230 as on the kiosks. Containers whose data sends messages when
// they open or break belong to level logic and stay shut
// (tools/build_scripted_containers.py). Both kinds are found by a walk over the
// SimManager's GUID table, spread over frames like the lift walk.
constexpr uint32_t kDropListTypeRva = 0x1CAC71A, kDropFlags = 0x20, kHasDropped = 0x80000000u, kDropOnDeath = 1u;
constexpr uint32_t kStandardContainerVtableRva = 0x17425F8, kBreakableContainerVtableRva = 0x17402C0, kContainerState = 0x23C, kContainerLocked = 0x248;
constexpr float kContainerRange = 14.0f;
// A crate this far over the leader's feet hangs on a wall: swung at, a few more times (brain wallCrate*).
constexpr float kWallCrateHeight = 0.5f;
constexpr int kWallCrateHitsMax = 5;
bool stompCorpses = false, lootContainers = false;
int corpseStompMax = 3;
struct LootMemo {
    uint32_t entity = 0;
    coop_bot::LootKind kind = coop_bot::LootKind::Corpse;
    int stomps = 0, presses = 0, triedWork = 0;
    DWORD seenAt = 0, workedAt = 0, centerAt = 0, triedSince = 0;
    Vec3 center;
    bool done = false, botKill = false, skip = false;
};
LootMemo lootMemo[64];
struct LootMark { Vec3 at; DWORD time = 0; };
LootMark lootMarks[8];
unsigned lootMarkNext = 0;
struct ItemSeen { uint32_t entity = 0; DWORD at = 0; bool flagged = false; };
ItemSeen itemSeen[64];
uint32_t containerList[96]{}, containerWalkFound[96]{};
unsigned containerCount = 0, containerWalkCount = 0;
uint32_t containerWalkBucket = 0;
bool containerWalking = false;
DWORD containersAt = 0;
volatile LONG containerWalks = 0, corpsesNoted = 0, cratesNoted = 0, lockersNoted = 0, scriptedSkips = 0, lockedSkips = 0, noDropListSkips = 0,
              lootGivenUp = 0, corpseStompsGiven = 0, crateStompsGiven = 0, crateSwingsGiven = 0, crateShotsGiven = 0, crateSightRays = 0, crateSightClear = 0, lockerPresses = 0, corpsesLooted = 0, cratesBroken = 0, lockersOpened = 0, lootItemsFlagged = 0,
              lootVisits = 0, lootLines = 0;
bool LootLog() { return InterlockedIncrement(&lootLines) <= 150; }
const char* LootName(coop_bot::LootKind kind) {
    return kind == coop_bot::LootKind::Corpse ? "corpse" : (kind == coop_bot::LootKind::Crate ? "crate" : "locker");
}
void ResetLoot() {
    for (LootMemo& m : lootMemo) m = LootMemo{};
    for (LootMark& m : lootMarks) m = LootMark{};
    for (ItemSeen& s : itemSeen) s = ItemSeen{};
    for (Unit& unit : units) unit.lootSpotCount = 0;
    containerCount = containerWalkCount = 0;
    containerWalking = false;
    containersAt = 0;
}
// Only containers within kWalkKeep of the leader are kept: a level may hold more than the list.
constexpr float kWalkKeep = 40.0f;
void StepLootWalk(DWORD now, const Vec3& leaderPos) {
    if (!lootContainers) return;
    if (!containerWalking) {
        if (containersAt && now - containersAt <= 6000) return;
        containerWalking = true;
        containerWalkBucket = 0;
        containerWalkCount = 0;
    }
    uint32_t sim = 0;
    if (!Read(base + ghosttrigger::kSimManagerRva, &sim, 4) || !sim) { containerWalking = false; return; }
    const uint32_t to = containerWalkBucket + kCabinBuckets;
    for (uint32_t b = containerWalkBucket; b < to && b < 4096 && containerWalkCount < std::size(containerWalkFound); ++b) {
        uint32_t node = 0;
        if (!Read(sim + 0x70 + b * 4, &node, 4)) break;
        for (unsigned guard = 0; node && guard < 64 && containerWalkCount < std::size(containerWalkFound); ++guard) {
            uint32_t object = 0, vtable = 0;
            Vec3 at;
            if (Read(node + 0x40, &object, 4) && object && Read(object, &vtable, 4) &&
                (vtable == base + kStandardContainerVtableRva || vtable == base + kBreakableContainerVtableRva) && bot_sense::Pos(object, at) &&
                coop_bot::Flat(at, leaderPos) < kWalkKeep)
                containerWalkFound[containerWalkCount++] = object;
            if (!Read(node + 0x34, &node, 4)) break;
        }
    }
    containerWalkBucket = to;
    if (containerWalkBucket < 4096 && containerWalkCount < std::size(containerWalkFound)) return;
    std::memcpy(containerList, containerWalkFound, sizeof(containerList));
    containerCount = containerWalkCount;
    containersAt = now;
    containerWalking = false;
    InterlockedIncrement(&containerWalks);
}
LootMemo& Memo(uint32_t entity, coop_bot::LootKind kind, DWORD now, bool& fresh) {
    LootMemo* oldest = &lootMemo[0];
    for (LootMemo& m : lootMemo) {
        if (m.entity == entity && m.kind == kind) { fresh = false; m.seenAt = now; return m; }
        if (!m.entity || m.seenAt < oldest->seenAt) oldest = &m;
        if (!m.entity) break;
    }
    *oldest = LootMemo{};
    oldest->entity = entity;
    oldest->kind = kind;
    oldest->seenAt = now;
    fresh = true;
    return *oldest;
}
LootMemo* FindMemo(uint32_t entity) {
    for (LootMemo& m : lootMemo) if (m.entity == entity && entity) return &m;
    return nullptr;
}
bool Scripted(uint32_t entity, const char*& why) {
    uint32_t guid[4]{};
    if (!Read(entity + 0x24, guid, sizeof(guid))) return false;
    for (const auto& row : ::ds2::kScriptedContainers)
        if (guid[0] == row.guid[0] && guid[1] == row.guid[1] && guid[2] == row.guid[2] && guid[3] == row.guid[3]) { why = row.why; return true; }
    return false;
}
uint32_t DropFlags(uint32_t entity, bool& have) {
    uint32_t flags = 0;
    const uint32_t list = drops::ComponentOf(entity, kDropListTypeRva);
    have = list && Read(list + kDropFlags, &flags, 4);
    return flags;
}
DWORD ItemFirstSeen(uint32_t entity, DWORD now, ItemSeen*& slot) {
    ItemSeen* oldest = &itemSeen[0];
    for (ItemSeen& s : itemSeen) {
        if (s.entity == entity) { slot = &s; return s.at; }
        if (s.at < oldest->at) oldest = &s;
    }
    *oldest = ItemSeen{entity, now, false};
    slot = oldest;
    return now;
}
// The corpses, crates and lockers this bot may visit, with what the squad did to them so far.
void GatherLoot(const Vec3& leaderPos, DWORD now) {
    int& count = u->lootSpotCount;
    count = 0;
    auto add = [&](const LootMemo& m) {
        if (count >= coop_bot::kMaxLoot) return;
        coop_bot::LootSpot& s = u->lootSpots[count++];
        s.id = m.entity;
        s.kind = m.kind;
        s.pos = m.center;
        s.stomps = m.stomps;
        s.botKill = m.botKill;
    };
    for (int i = 0; stompCorpses && i < u->seen.corpseCount; ++i) {
        const bot_sense::Corpse& c = u->seen.corpses[i];
        bool have = false, fresh = false;
        const uint32_t flags = DropFlags(c.entity, have);
        LootMemo& m = Memo(c.entity, coop_bot::LootKind::Corpse, now, fresh);
        if (fresh) {
            InterlockedIncrement(&corpsesNoted);
            uint32_t attacker = 0;
            const uint32_t health = drops::ComponentOf(c.entity, drops::kHealthTypeRva);
            m.botKill = health && Read(health + drops::kLastAttacker, &attacker, 4) && attacker > 0x10010 && IsBotEntity(attacker - 0x10);
            if (!have) InterlockedIncrement(&noDropListSkips);
        }
        if (!have || (flags & (kHasDropped | kDropOnDeath))) {
            if (have && m.stomps && !m.done) {
                InterlockedIncrement(&corpsesLooted);
                if (LootLog()) gate::Event("bot: corpse %08X looted after %d stomp(s) (drop flags %08X)", c.entity, m.stomps, flags);
            }
            m.done = true;
            continue;
        }
        if (m.done || m.stomps >= corpseStompMax) continue;
        if (!m.centerAt || now - m.centerAt >= 500) {
            bot_sense::CorpseCenter(c.entity, c.at, m.center);
            m.centerAt = now;
        }
        add(m);
    }
    for (unsigned i = 0; lootContainers && i < containerCount; ++i) {
        const uint32_t entity = containerList[i];
        uint32_t vtable = 0;
        Vec3 at;
        unsigned char hidden = 0;
        if (!Read(entity, &vtable, 4) || !bot_sense::Pos(entity, at) || (Read(entity + bot_sense::kHiddenCount, &hidden, 1) && hidden)) continue;
        const bool locker = vtable == base + kStandardContainerVtableRva;
        if (!locker && vtable != base + kBreakableContainerVtableRva) continue;
        if (coop_bot::Flat(at, leaderPos) > kContainerRange || std::fabs(at.y - leaderPos.y) > 3.0f) continue;
        bool fresh = false;
        LootMemo& m = Memo(entity, locker ? coop_bot::LootKind::Locker : coop_bot::LootKind::Crate, now, fresh);
        if (fresh) {
            InterlockedIncrement(locker ? &lockersNoted : &cratesNoted);
            const char* why = nullptr;
            if (Scripted(entity, why)) {
                m.skip = true;
                InterlockedIncrement(&scriptedSkips);
                if (LootLog()) gate::Event("bot: %s %08X is part of the level's logic (%s): left alone", locker ? "locker" : "crate", entity, why);
            }
        }
        if (m.skip || m.done) continue;
        m.center = at;
        if (locker) {
            int32_t state = -1;
            unsigned char locked = 0;
            if (!Read(entity + kContainerState, &state, 4) || !Read(entity + kContainerLocked, &locked, 1)) continue;
            if (state != 0) {
                if (m.presses) {
                    InterlockedIncrement(&lockersOpened);
                    if (LootLog()) gate::Event("bot: locker %08X opened (state %d) after %d press(es)", entity, state, m.presses);
                }
                m.done = true;
                continue;
            }
            if (locked) {
                if (fresh) InterlockedIncrement(&lockedSkips);
                continue;
            }
        } else {
            bool have = false;
            const uint32_t flags = DropFlags(entity, have);
            if (!have || (flags & kHasDropped)) {
                if (have && m.stomps) {
                    InterlockedIncrement(&cratesBroken);
                    if (LootLog()) gate::Event("bot: crate %08X broken after %d stomp(s) or swing(s) (drop flags %08X)", entity, m.stomps, flags);
                }
                if (!have && fresh) InterlockedIncrement(&noDropListSkips);
                m.done = true;
                continue;
            }
            if (m.stomps >= (at.y - leaderPos.y > kWallCrateHeight ? kWallCrateHitsMax : corpseStompMax)) continue;
        }
        add(m);
    }
}
// Every frame: the spots gathered at most 250 ms ago with the squad's stomps as
// they are now, the container on offer, and the items that came out of looting.
void FillLoot(coop_bot::Perception& p, const Vec3& self, const Vec3& leaderPos, DWORD now) {
    if (!stompCorpses && !lootContainers) return;
    uint32_t offered = 0;
    if (Read(u->entity + kNearbyUsable, &offered, 4) && offered > kUsable) p.lootOffered = offered - kUsable;
    if (now - u->lootScanAt >= 250) { u->lootScanAt = now; GatherLoot(leaderPos, now); }
    for (int i = 0; i < u->lootSpotCount && p.lootCount < coop_bot::kMaxLoot; ++i) {
        coop_bot::LootSpot s = u->lootSpots[i];
        if (const LootMemo* m = FindMemo(s.id)) {
            if (m->done) continue;
            s.stomps = m->stomps;
        }
        // A clear shot at the crate it works now, a ray every 250 ms.
        if (s.kind == coop_bot::LootKind::Crate && s.id == u->command.loot) {
            if (s.id != u->crateSightId || now - u->crateSightAt >= 250) {
                u->crateSightId = s.id;
                u->crateSightAt = now;
                const bool wall = s.pos.y - self.y > u->brain.tune.wallCrateHeight;
                const Vec3 at = wall ? s.pos : coop_bot::Add(s.pos, {0.0f, 0.3f, 0.0f});
                u->crateSightClear = bot_sense::losReady && coop_bot::Distance(self, s.pos) <= u->brain.tune.crateShootRange + 1.0f &&
                                     bot_sense::Clear(CameraEye(self), at, u->entity, s.id);
                InterlockedIncrement(&crateSightRays);
                if (u->crateSightClear) InterlockedIncrement(&crateSightClear);
            }
            s.inSight = u->crateSightClear;
        }
        p.loot[p.lootCount++] = s;
    }
    // What turned up beside a spot a bot just worked is its loot, the leader's closeness aside.
    for (int i = 0; i < p.itemCount; ++i) {
        ItemSeen* slot = nullptr;
        const DWORD first = ItemFirstSeen(p.items[i].id, now, slot);
        for (const LootMark& mark : lootMarks) {
            if (!mark.time || now - mark.time > 20000 || first + 500 < mark.time || coop_bot::Distance(p.items[i].pos, mark.at) > 3.0f) continue;
            p.items[i].botLoot = true;
            if (slot && !slot->flagged) {
                slot->flagged = true;
                InterlockedIncrement(&lootItemsFlagged);
                if (LootLog()) gate::Event("bot: item %08X at (%.1f %.1f %.1f) came out of the squad's looting", p.items[i].id, p.items[i].pos.x, p.items[i].pos.y,
                                           p.items[i].pos.z);
            }
            break;
        }
    }
    (void)self;
}
// After the brain: count what it did to its spot, and let a stomped corpse drop its loot.
constexpr DWORD kLootFruitlessMs = 15000;
void NoteLoot(const coop_bot::Perception& p, DWORD now) {
    const coop_bot::Command& c = u->command;
    if (c.mode != coop_bot::Mode::Loot || !c.loot) { u->lootLast = 0; return; }
    LootMemo* m = FindMemo(c.loot);
    const coop_bot::LootSpot* spot = nullptr;
    for (int i = 0; i < p.lootCount; ++i) if (p.loot[i].id == c.loot) spot = &p.loot[i];
    if (!m || !spot) return;
    // A spot the squad keeps visiting without a stomp or a press is out of its reach: a
    // locker that never offers its prompt to a bot, a corpse it cannot step on. 15 s of
    // that, over all visits, and it is left alone for the level (17.10.: one corpse 13
    // visits, two lockers 6 each, nothing done; each bot came back after its own minute).
    const int work = m->stomps + m->presses;
    if (!m->triedSince || work != m->triedWork) {
        m->triedSince = now;
        m->triedWork = work;
    } else if (now - m->triedSince >= kLootFruitlessMs) {
        m->skip = true;
        InterlockedIncrement(&lootGivenUp);
        gate::Event("bot: %s %08X given up for this level: %lu s of visits without a stomp or a press (%d before)", LootName(m->kind), c.loot,
                    static_cast<unsigned long>((now - m->triedSince) / 1000), work);
        return;
    }
    if (u->lootLast != c.loot) {
        u->lootLast = c.loot;
        InterlockedIncrement(&lootVisits);
        if (LootLog())
            gate::Event("bot %u: goes to the %s %08X at (%.1f %.1f %.1f), %.1f m off%s", u->index + 1, LootName(spot->kind), c.loot, spot->pos.x, spot->pos.y,
                        spot->pos.z, coop_bot::Flat(u->self, spot->pos), spot->kind == coop_bot::LootKind::Corpse ? (spot->botKill ? ", a bot's kill" : ", the leader's kill") : "");
    }
    const bool shot = c.fire && c.mode == coop_bot::Mode::Loot;
    if (c.stomp || (c.melee && c.mode == coop_bot::Mode::Loot) || shot) {
        ++m->stomps;
        m->workedAt = now;
        if (m->kind == coop_bot::LootKind::Corpse) { drops::AllowBotLoot(c.loot, now + 2500); InterlockedIncrement(&corpseStompsGiven); }
        else InterlockedIncrement(shot ? &crateShotsGiven : (c.melee ? &crateSwingsGiven : &crateStompsGiven));
        lootMarks[lootMarkNext++ % std::size(lootMarks)] = LootMark{spot->pos, now};
        if (LootLog()) gate::Event("bot %u: %s the %s %08X (%d of %d), %.2f m from its middle, %.2f m over its feet", u->index + 1, shot ? "shoots" : (c.melee ? "swings at" : "stomps"),
                                   LootName(m->kind), c.loot, m->stomps, shot ? u->brain.tune.crateShotsMax : (c.melee ? u->brain.tune.wallCrateHitsMax : corpseStompMax), coop_bot::Flat(u->self, spot->pos),
                                   spot->pos.y - u->self.y);
    }
    if (c.use) {
        ++m->presses;
        m->workedAt = now;
        InterlockedIncrement(&lockerPresses);
        lootMarks[lootMarkNext++ % std::size(lootMarks)] = LootMark{spot->pos, now};
        if (LootLog()) gate::Event("bot %u: presses use at the locker %08X (%d), offered %08X", u->index + 1, c.loot, m->presses, p.lootOffered);
    }
}

// --- the leader in a grab ([Bot] RescueLeader) ----------------------------------------
// Held: his lower body machine in a grab state (27 swarm shake-off, 29 dragged,
// 33 paired animation, 40 eye poke; read live 28.09. on the bots), or a paired
// attack on him. Sentient +0x2A8 is m_pPairedCoordinator (PC
// SentientPairedAttackSM 0x01092D3C/0x01092DCC test it; Xbox +0x2C8,
// IsUnderPairedCoordinatorControl, cleared by RevokePairedCoordinatorControl).
// The PairedAttackCoordinator (vtable 0x01B731D4, ctor 0x01092440) keeps the
// attacker as a SafePtr at +0x88 and the defender at +0x90 (PC 0x01092AD0 and
// 0x01092B0E read them; Xbox GetAttackerGrapplerComponent +0x9C, the defender
// attachment +0xA4). The attacker is the one to shoot; without a coordinator the
// nearest enemy on him going for him stands in.
constexpr uint32_t kCoordinatorVtableRva = 0x17731D4, kCoordAttacker = 0x88, kCoordDefender = 0x90;
bool rescueLeader = false;
DWORD rescueSince = 0;
volatile LONG rescues = 0, rescueByCoordinator = 0, rescueByNearest = 0, rescueNoGrabber = 0, rescueCoordOnly = 0, rescueFrames = 0, rescueAttacks = 0,
              rescueInjected = 0, rescueLines = 0;
uint32_t SafeEntity(uint32_t safe) {
    uint32_t vtable = 0;
    if (safe <= 0x10010) return 0;
    const uint32_t entity = safe - 0x10;
    return Read(entity, &vtable, 4) && vtable >= base + gate::kRdataLo && vtable < base + gate::kRdataHi ? entity : 0;
}
uint32_t FindGrabber(uint32_t leader, const Vec3& leaderPos, int32_t lower, DWORD now) {
    const bool grabState = lower == 27 || lower == 29 || lower == 33 || lower == 40;
    uint32_t coord = 0, vtable = 0, attackerRef = 0, defenderRef = 0, attacker = 0;
    if (Read(leader + kPairedControl, &coord, 4) && coord > 0x10000 && Read(coord, &vtable, 4) && vtable == base + kCoordinatorVtableRva &&
        Read(coord + kCoordAttacker, &attackerRef, 4) && Read(coord + kCoordDefender, &defenderRef, 4) && SafeEntity(defenderRef) == leader)
        attacker = SafeEntity(attackerRef);
    float hp = 0.0f;
    const uint32_t health = attacker ? hits::FindHealthComponent(attacker) : 0;
    if (attacker && (attacker == leader || IsBotEntity(attacker) || !health || !Read(health + hits::kHealthCurrent, &hp, 4) || hp <= 0.0f)) attacker = 0;
    const char* source = "coordinator";
    if (attacker && !grabState) {
        Vec3 at;
        if (!bot_sense::Pos(attacker, at) || coop_bot::Distance(at, leaderPos) > 3.0f) attacker = 0;
        else source = "coordinator without a grab state";
    }
    if (!attacker && grabState) {
        // The nearest live enemy on him, one that goes for him first.
        float best = 1e9f;
        for (unsigned i = 0; i < wantedBots; ++i) {
            const bot_sense::Snapshot& s = units[i].seen;
            for (int k = 0; k < s.enemyCount; ++k) {
                const coop_bot::Enemy& e = s.enemies[k];
                if (e.health <= 0.0f || e.objective) continue;
                const float d = coop_bot::Distance(e.pos, leaderPos) - (e.victim == leader ? 1.0f : 0.0f);
                if (d < best && coop_bot::Distance(e.pos, leaderPos) < 3.0f) { best = d; attacker = e.id; }
            }
        }
        source = "the nearest enemy on him";
    }
    const bool held = grabState || attacker;
    if (held && !leaderGrabber && attacker) {
        rescueSince = now;
        InterlockedIncrement(&rescues);
        InterlockedIncrement(std::strcmp(source, "the nearest enemy on him") ? &rescueByCoordinator : &rescueByNearest);
        if (!grabState) InterlockedIncrement(&rescueCoordOnly);
        uint32_t avt = 0;
        Read(attacker, &avt, 4);
        Vec3 at;
        bot_sense::Pos(attacker, at);
        if (InterlockedIncrement(&rescueLines) <= 60)
            gate::Event("bot rescue: the leader is held (lower body state %d, coordinator %08X) by %08X (vtable RVA %08X, %s), %.1f m from him: every bot goes for it",
                        lower, coord, attacker, avt ? avt - static_cast<uint32_t>(base) : 0u, source, coop_bot::Distance(at, leaderPos));
    } else if (held && !attacker && !leaderGrabber && !rescueSince) {
        rescueSince = now;
        InterlockedIncrement(&rescueNoGrabber);
        if (InterlockedIncrement(&rescueLines) <= 60)
            gate::Event("bot rescue: the leader is held (lower body state %d, coordinator %08X) but no grabber is known", lower, coord);
    } else if (!held && rescueSince) {
        if (InterlockedIncrement(&rescueLines) <= 60)
            gate::Event("bot rescue: the leader is free after %lu ms (rescue frames %ld, attacks %ld so far)", static_cast<unsigned long>(now - rescueSince), rescueFrames,
                        rescueAttacks);
        rescueSince = 0;
    } else if (held && attacker && leaderGrabber && attacker != leaderGrabber && InterlockedIncrement(&rescueLines) <= 60) {
        gate::Event("bot rescue: the grabber changed from %08X to %08X (%s)", leaderGrabber, attacker, source);
    }
    return held ? attacker : 0;
}
// The grabber may be missing from a bot's last scan (scanned before it came, or
// the scan was full): it is put in, measured like a target.
void AddGrabber(coop_bot::Perception& p, const Vec3& self) {
    if (!p.leaderGrabber) return;
    for (int i = 0; i < p.enemyCount; ++i) if (p.enemies[i].id == p.leaderGrabber) return;
    if (p.enemyCount >= coop_bot::kMaxEnemies) return;
    coop_bot::Enemy e;
    e.id = p.leaderGrabber;
    const uint32_t health = hits::FindHealthComponent(e.id);
    float hp = 0.0f, max = 0.0f;
    if (!health || !Read(health + hits::kHealthCurrent, &hp, 4) || hp <= 0.0f) return;
    Read(health + hits::kHealthMax, &max, 4);
    const DWORD now = GetTickCount();
    if (u->refreshed.id == e.id && now - u->refreshAt < kRefreshMs) {
        e = u->refreshed;
    } else {
        if (!bot_sense::Refresh(e, u->entity, CameraEye(self))) return;
        u->refreshed = e;
        u->refreshAt = now;
    }
    e.health = hp;
    e.maxHealth = max;
    e.victim = p.leaderId;
    p.enemies[p.enemyCount++] = e;
    InterlockedIncrement(&rescueInjected);
}

void NoteKill(const coop_bot::Enemy& e, DWORD now);   // "antics", runtime_coop_bot_antics.inc
volatile LONG viewFromBody = 0;   // frames the view cone took the body's facing: no aim block readable
void Sense(uint32_t leader, const Vec3& self, const Vec3& leaderPos, DWORD now, coop_bot::Perception& p) {
    // Every 250 ms, and never two bots' walks in the same frame: their scans lined
    // up in one frame made the spikes. The walk's enemies are sighted afterwards,
    // a few a frame; the last complete scan is what the bot knows meanwhile.
    if (sightFrame != botFrame) { sightFrame = botFrame; sightsLeft = kSightsPerFrame; }
    if (u->scanActive && u->scanEntity != u->entity) { u->scanActive = false; InterlockedIncrement(&scanDropped); }
    auto finish = [&]() {
        u->seen = u->scanNext;
        u->scanActive = false;
        const LONG lag = static_cast<LONG>(now - u->scanStartedAt);
        InterlockedIncrement(&scanDone);
        InterlockedExchangeAdd(&scanLagMsSum, lag);
        if (lag > scanLagMsMax) InterlockedExchange(&scanLagMsMax, lag);
    };
    if (!u->scanActive && now - u->lastScan >= kScanMs && scannedFrame != botFrame) {
        u->lastScan = now;
        scannedFrame = botFrame;
        {
            const Perf timer(perfScan);
            bot_sense::Scan(u->scanNext, u->entity, self, leaderPos);
        }
        InterlockedIncrement(&scanWalks);
        u->scanActive = true;
        u->scanEntity = u->entity;
        u->scanStartedAt = now;
        if (!u->scanNext.enemyCount) finish();
    } else if (u->scanActive && sightsLeft > 0) {
        const Perf timer(perfSight);
        const Vec3 eye = CameraEye(self);
        while (sightsLeft > 0 && u->scanNext.sighted < u->scanNext.enemyCount) {
            bot_sense::Sight(u->scanNext, u->scanNext.sighted++, u->entity, self, eye);
            --sightsLeft;
            InterlockedIncrement(&scanSights);
        }
        if (u->scanNext.sighted >= u->scanNext.enemyCount) finish();
    }
    if (now - lastCabinCheck >= 500) {
        lastCabinCheck = now;
        ghosttrigger::Cabin cabins[16]{};
        const unsigned n = CachedCabins(cabins, 16, now);
        const float at[3] = {leaderPos.x, leaderPos.y, leaderPos.z};
        leaderInCabin = false;
        for (unsigned i = 0; i < n && !leaderInCabin; ++i) leaderInCabin = ghosttrigger::InCabin(cabins[i], at);
    }
    p.now = now;
    p.self = self;
    FillOrders(p);
    {
        float body[3]{};
        p.selfYawKnown = Read(u->entity + 0xB0, body, sizeof(body)) && std::isfinite(body[0]) && std::isfinite(body[2]) &&
                         std::fabs(body[0]) + std::fabs(body[2]) > 0.1f;
        if (p.selfYawKnown) p.selfYaw = std::atan2(body[0], body[2]);   // the body's forward row, as bot_sense::Yaw reads it
        // Its eyes for the view cone: the aim block the last look turned, the body in flight.
        float viewHeading = 0.0f, viewPitch = 0.0f;
        if (u->flying && u->bodyKnown && std::fabs(u->body.forward.x) + std::fabs(u->body.forward.z) > 0.1f) {
            p.viewYaw = std::atan2(u->body.forward.x, u->body.forward.z);
            p.viewYawKnown = true;
        } else if (AimAngles(u->entity, u->command.aim ? kGunAim : kCamAim, viewHeading, viewPitch) || AimAngles(u->entity, kCamAim, viewHeading, viewPitch)) {
            p.viewYaw = viewHeading;
            p.viewYawKnown = true;
        } else if (p.selfYawKnown) {
            p.viewYaw = p.selfYaw;
            p.viewYawKnown = true;
            InterlockedIncrement(&viewFromBody);
        }
    }
    p.health01 = Health01(u->entity);
    if (!u->medpacksAt || now - u->medpacksAt >= 500) {
        u->medpacksAt = now;
        u->medpacks = MedPacks(u->entity);
    }
    p.medpacks = u->medpacks;
    p.stasis01 = u->stasisKnown ? u->stasis01 : 1.0f;   // 0 without the module or below one cast
    uint32_t pickup = 0, paired = 0;
    p.pickupInReach = Read(u->entity + kClosestPickup, &pickup, 4) && pickup;
    ReadGuns(p);
    // Held or in a struggle: the lower body machine says so by its state (read
    // live 28.09., PlayerLowerBodySM vtable 0x01B27000, state index +0x0C):
    // 27 swarmShakeOff, 28 decompressionPulled, 29 dragged, 33 pairedAnim (an
    // enemy's grab), 40 eyePoke. The way out is mashing use ("shakeoff" is bound
    // to the same button). Player+0x2A8 was no good (set on a free Isaac).
    (void)paired;
    const Machines machines = MachinesOf(u->entity);
    const int32_t lower = machines.lower;
    NoteFlight(machines, self, now);
    p.grabbed = lower == 27 || lower == 28 || lower == 29 || lower == 33 || lower == 40;
    if (p.grabbed && !u->wasGrabbed) {
        InterlockedIncrement(&struggles);
        gate::Event("bot %u: held (lower body state %d): mashing use", u->index + 1, lower);
    }
    u->wasGrabbed = p.grabbed;
    p.leader = leaderPos;
    p.leaderYaw = bot_sense::Yaw(leader);
    p.leaderMoving = false;
    p.leaderShooting = leaderShotAt && now - leaderShotAt < 4000;
    for (unsigned i = 0; i < 8; ++i)
        if (leaderHistoryAt[i] && now - leaderHistoryAt[i] <= 2000 && coop_bot::Flat(leaderHistory[i], leaderPos) > 1.5f) p.leaderMoving = true;
    {
        // His walk over the last half second, from the samples a quarter second apart.
        int older = -1;
        for (unsigned i = 0; i < 8; ++i)
            if (leaderHistoryAt[i] && now - leaderHistoryAt[i] >= 400 && now - leaderHistoryAt[i] <= 800 &&
                (older < 0 || leaderHistoryAt[i] > leaderHistoryAt[older]))
                older = static_cast<int>(i);
        if (older >= 0) {
            const float dt = static_cast<float>(now - leaderHistoryAt[older]) / 1000.0f;
            p.leaderVel = {(leaderPos.x - leaderHistory[older].x) / dt, 0.0f, (leaderPos.z - leaderHistory[older].z) / dt};
        }
    }
    p.chase = leaderChase;
    p.progression = leaderInCabin || u->seen.doorBusy;
    p.progressionAt = leaderInCabin ? leaderPos : u->seen.doorAt;
    p.enemyCount = u->seen.enemyCount;
    for (int i = 0; i < u->seen.enemyCount; ++i) {
        p.enemies[i] = u->seen.enemies[i];
        if (p.enemies[i].id == u->command.target && u->command.target) {
            // Its limbs and the rays to them, at most every 100 ms (29.09.: every
            // frame for every bot cost frames); in between the last measurement.
            if (u->refreshed.id == p.enemies[i].id && now - u->refreshAt < kRefreshMs) {
                p.enemies[i] = u->refreshed;
            } else {
                const Perf timer(perfRefresh);
                bot_sense::Refresh(p.enemies[i], u->entity, CameraEye(self));
                u->refreshed = p.enemies[i];
                u->refreshAt = now;
            }
            // Its health every frame: a target that died since the last scan gets no
            // more shots (27.09.: shots into the corpse popped its loot for the bots).
            if (u->targetHealthOf != p.enemies[i].id) {
                u->targetHealthOf = p.enemies[i].id;
                u->targetHealth = hits::FindHealthComponent(p.enemies[i].id);
            }
            float left = 1.0f;
            if (u->targetHealth && Read(u->targetHealth + hits::kHealthCurrent, &left, 4) && left <= 0.0f) {
                p.enemies[i].health = 0.0f;
                InterlockedIncrement(&deadTargetsDropped);
                NoteKill(p.enemies[i], now);
            }
        }
        for (const auto& s : stasised) if (s.npc == u->seen.enemies[i].id && now < s.until) p.enemies[i].stasised = true;
        float tm = 1.0f;
        if (!p.enemies[i].stasised && !p.enemies[i].objective && Read(p.enemies[i].id + kNpcTimeMultiplier, &tm, 4) && std::isfinite(tm) && tm >= 0.0f &&
            tm < kStasisSlowBelow) {
            p.enemies[i].stasised = true;
            InterlockedIncrement(&stasisSlowSeen);
        }
    }
    p.playerCount = u->seen.playerCount;
    for (int i = 0; i < u->seen.playerCount; ++i) p.players[i] = u->seen.players[i];
    p.humanCount = u->seen.humanCount;
    for (int i = 0; i < u->seen.humanCount; ++i) p.humans[i] = u->seen.humans[i];
    p.itemCount = u->seen.itemCount;
    for (int i = 0; i < u->seen.itemCount; ++i) p.items[i] = u->seen.items[i];
    // The squad: the other bots as they stood on their last tick.
    p.selfId = u->entity;
    p.leaderId = leader;
    p.squadSize = 0;
    for (unsigned i = 0; i < wantedBots; ++i) {
        const Unit& other = units[i];
        if (!other.entity || other.state != State::Active) continue;
        if (&other == u) { p.squadIndex = p.squadSize; ++p.squadSize; continue; }
        ++p.squadSize;
        if (p.mateCount >= coop_bot::kMaxMates) continue;
        coop_bot::Mate& m = p.mates[p.mateCount++];
        m.id = other.entity;
        m.pos = other.self;
        m.moving = other.command.move;
        m.goal = other.command.move ? other.command.moveTo : other.self;
        m.target = other.command.target;
        m.item = other.brain.ItemWanted();
        m.loot = other.brain.LootWanted();
        m.antic = other.brain.AnticNow() != coop_bot::Antic::None;
        m.aiming = other.command.mode == coop_bot::Mode::Combat && other.command.aim && other.command.look;
        m.aimAt = other.command.lookAt;
    }
    if (!p.squadSize) p.squadSize = 1;
    FillLoot(p, self, leaderPos, now);
    AddGrabber(p, self);
    // How far the aim is off the last look target (the gun's when aiming).
    float heading = 0.0f, pitch = 0.0f;
    // In flight the body turns to the look point, and the flat heading of the aim
    // data means nothing there (06.10.: 70-95 deg "off" with the body 1-8 deg off
    // the enemy, so no shot ever left): the body's own forward from its eye.
    if (u->command.look && u->flying && u->bodyKnown) {
        p.aimErrorDeg = AngleDeg(u->body.forward, coop_bot::Sub(u->command.lookAt, coop_bot::Add(self, coop_bot::Scale(u->body.up, 1.3f))));
        InterlockedIncrement(&zgAimFrames);
    } else if (u->command.look && AimAngles(u->entity, u->command.aim ? kGunAim : kCamAim, heading, pitch))
        p.aimErrorDeg = AngleDeg(Direction(heading, pitch), coop_bot::Sub(u->command.lookAt, u->command.aim ? AimOrigin(self, now) : Vec3{self.x, self.y + kEye, self.z}));
}

void Act(const Vec3& self, const Vec3& leaderPos, DWORD now) {
    Pad p;
    const coop_bot::Command& c = u->command;
    // Buttons first: pulses with their own spacing (the brain already paces them).
    if (c.fire) { Press(kFire, now); InterlockedIncrement(&shots); }
    if (c.fire || c.holdFire) u->fireAt = now;
    if (c.fire || c.holdFire || c.melee || c.stasis || c.altFire || c.stomp) lastBotActionAt = now;   // for the human guard's log
    if (c.stasis) lastBotStasisAt = now;
    if (c.melee) { Press(kFire, now); InterlockedIncrement(&melees); }
    if (c.reload) { Press(kReload, now); InterlockedIncrement(&reloads); }
    if (c.heal) { Press(kMedpack, now); InterlockedIncrement(&heals); }
    if (c.use) { Press(kUse, now); InterlockedIncrement(&pickups); }
    if (c.stasis) {
        Press(kStasis, now);
        InterlockedIncrement(&stasisCasts);
        Stasised& slot = stasised[static_cast<unsigned>(now / 7) % std::size(stasised)];
        slot = Stasised{c.target, now + kStasisPendingMs};
    }
    // Weapons: the brain's slot through the game's own wish; without the inventory
    // (signature mismatch) the old way, next item until the plasma cutter is in hand.
    TakeSlot(c.slot, now);
    // Alt fire waits for the aiming state (the same button is a stomp outside it),
    // at most 800 ms, and goes out as a fresh press.
    if (c.altFire) u->altWantedAt = now;
    if (u->altWantedAt && now - u->altWantedAt > 800) u->altWantedAt = 0;
    if (u->altWantedAt && Aiming()) { Press(kAltFire, now); InterlockedIncrement(&altFires); u->altWantedAt = 0; }
    // A stomp is the same button outside the aiming state: let go of the aim first.
    if (c.stomp) u->stompWantedAt = now;
    if (u->stompWantedAt && now - u->stompWantedAt > 800) { u->stompWantedAt = 0; InterlockedIncrement(&stompsLost); }
    if (u->stompWantedAt && !Aiming()) { Press(kAltFire, now); InterlockedIncrement(&stomps); u->stompWantedAt = 0; }
    if (!gunsReady && !u->noCutter && !c.fire && !c.melee && now - u->lastWeaponSwitch >= 900) {
        uint32_t item = 0, vtable = 0;
        const bool held = Read(u->entity + kActiveItem, &item, 4) && item > 0x10010 && Read(item - 0x10, &vtable, 4);
        if (held && vtable != base + kPlasmaCutterVtableRva) {
            u->lastWeaponSwitch = now;
            if (++u->weaponSwitches > 8) {
                u->noCutter = true;
                gate::Event("bot: no plasma cutter found among its items after 8 switches; it keeps what it holds");
            } else {
                Press(kNextItem, now);
            }
        }
    }
    uint16_t buttons = Pressed(now);
    if (c.aim) buttons = static_cast<uint16_t>(buttons | kAim);
    if (c.melee) buttons = static_cast<uint16_t>(buttons & ~kAim);   // melee is the fire button without aiming
    if (u->stompWantedAt || c.stomp) buttons = static_cast<uint16_t>(buttons & ~kAim);
    if (c.altFire || u->altWantedAt) buttons = static_cast<uint16_t>(buttons | kAim);   // alt fire only counts while aiming (else it is a stomp)
    if (c.holdFire) buttons = static_cast<uint16_t>(buttons | kFire);
    if (c.holdAlt && Aiming()) buttons = static_cast<uint16_t>(buttons | kAltFire | kAim);
    // The med pack button while aiming is kinesis: while it is down, nothing else is,
    // unless the bot means kinesis (then it is one short press with the aim held).
    if (u->tkStep == TkStep::None && (c.heal || (buttons & kMedpack))) buttons = static_cast<uint16_t>(buttons & ~(kAim | kFire | kAltFire));
    if (u->tkStep != TkStep::None) {
        buttons = static_cast<uint16_t>(buttons & ~kMedpack);
        if (u->tkMedpackUntil && now < u->tkMedpackUntil) buttons = static_cast<uint16_t>(buttons | kMedpack | kAim);
    }
    // Six presses a second: firm, but not a machine gun.
    if (c.mash) { buttons = static_cast<uint16_t>((now / 83) % 2 ? (buttons | kUse) : (buttons & ~kUse)); InterlockedIncrement(&mashFrames); }
    // Zero-G: flying, the flight owns the stick and the look; on its feet it may take off.
    if (FlightTick(self, leaderPos, now, p, buttons)) {
        p.buttons = buttons;
        u->pad = p;
        u->padLive = true;
        return;
    }
    // Where to go. Just put into a lift, nowhere: the ride starts under it, and a
    // bot that walked on toward its follow spot stepped off the platform.
    const bool holding = u->liftHoldUntil && now < u->liftHoldUntil;
    if (holding) { u->steer.Reset(); InterlockedIncrement(&liftHoldFrames); }
    // Holding a spot, the spot stands in for the leader: no catch-up to him, and
    // not his trail either when NavPower has no way.
    static const coop_bot::Trail kNoTrail;
    // A short turning step of an antic goes straight at its point, past the route.
    const bool direct = !holding && c.move && c.direct;
    if (direct) u->steer.Reset();
    u->steer.distanceTeleport = !OnlyStuckTeleports();
    // A fight slows any walk: more patience before it counts as stuck (20.10.: catch-ups mid-fight)
    const bool fighting = c.mode == coop_bot::Mode::Combat;
    u->steer.giveUpMs = fighting ? 8000u : 4000u;
    u->steer.teleportAfterMs = fighting ? 10000u : 7000u;
    coop_bot::SteerOutput route = holding || direct ? coop_bot::SteerOutput{}
                                                    : u->steer.Tick(self, c.moveTo, c.move, u->holding ? u->holdSpot : leaderPos, u->holding ? kNoTrail : trail, now);
    if (direct) {
        route.move = true;
        route.toward = c.moveTo;
        route.route = coop_bot::Route::Direct;
    }
    if (route.teleport && u->holding) {
        InterlockedIncrement(&holdTeleportsSkipped);
        if (OrderLog())
            gate::Event("bot %u: no way on toward its spot (%.1f %.1f %.1f), %.1f m off: holds where it stands instead of a catch-up", u->index + 1,
                        u->holdSpot.x, u->holdSpot.y, u->holdSpot.z, coop_bot::Flat(self, u->holdSpot));
        u->holdSpot = self;
        u->goingThere = false;
    } else if (route.teleport && leaderBusy) {
        u->catchUpPending = true;   // it waits below until he is out
    } else if (route.teleport) {
        const Vec3 spot = CatchUpSpot(leaderPos);
        if (MoveEntity(u->entity, spot)) {
            InterlockedIncrement(&teleports);
            gate::Event("bot: caught up with the leader (%.1f m behind or stuck), put at (%.1f %.1f %.1f)", coop_bot::Distance(self, leaderPos), spot.x,
                        spot.y, spot.z);
            HoldInLift(spot, now, "caught up", false);
        }
    } else if (route.askPath && now - u->lastQuery >= 250) {
        u->lastQuery = now;
        InterlockedIncrement(&navAsked);
        Vec3 points[coop_bot::kMaxWaypoints];
        int n = 0;
        bool found = false;
        {
            const Perf timer(perfNav);
            found = bot_nav::Query(self, c.moveTo, points, coop_bot::kMaxWaypoints, n);
        }
        if (found) { u->steer.SetPath(points, n, now); InterlockedIncrement(&navFound); }
        else u->steer.NoPath(now);
    }
    if (route.blocked) {
        u->routeBlockedAt = now;
        // Its spot is off the mesh or out of reach: it holds the closest point instead.
        if (u->holding && coop_bot::Flat(c.moveTo, u->holdSpot) < 1.0f && coop_bot::Flat(route.reachable, u->holdSpot) > 0.3f) {
            InterlockedIncrement(&holdSpotsMoved);
            if (OrderLog())
                gate::Event("bot %u: its spot (%.1f %.1f %.1f) is out of reach, holds (%.1f %.1f %.1f) instead", u->index + 1, u->holdSpot.x, u->holdSpot.y,
                            u->holdSpot.z, route.reachable.x, route.reachable.y, route.reachable.z);
            u->holdSpot = route.reachable;
            u->goingThere = false;
        }
        u->brain.Unreachable(c.moveTo, route.reachable, now);
        const bool gaveUp = coop_bot::Flat(route.reachable, self) < 0.01f;
        if (InterlockedIncrement(&unreachable) <= 40)
            gate::Event(gaveUp ? "bot %u: no closer to (%.1f %.1f %.1f) in 4 s (a wall or a corner), stops at (%.1f %.1f %.1f)"
                               : "bot %u: (%.1f %.1f %.1f) is off the mesh, stays at the closest point (%.1f %.1f %.1f)",
                        u->index + 1, c.moveTo.x, c.moveTo.y, c.moveTo.z, route.reachable.x, route.reachable.y, route.reachable.z);
    }
    if (route.move && !route.teleport) {
        if (route.route == coop_bot::Route::Shadow) InterlockedIncrement(&shadowTicks);
        const float dist = coop_bot::Flat(self, route.toward);
        const float magnitude = c.run ? 1.0f : (dist < 1.5f ? 0.45f : 0.75f) * u->walkPace;
        Walk(self, route.toward, magnitude, c.aim ? kGunAim : kCamAim, !c.aim, p);
        if (c.run && !c.aim) buttons = static_cast<uint16_t>(buttons | kRun);
    }
    // Where to look: the brain's point, else the way it walks. Walking without aiming,
    // the brain's point only when it lies along the way; else the way comes first.
    const Vec3 eye = {self.x, self.y + kEye, self.z};
    bool lookAlong = false;
    if (route.move && !route.teleport && !c.aim) {
        lookAlong = !c.look;
        if (c.look) {
            const Vec3 way = {route.toward.x - self.x, 0.0f, route.toward.z - self.z}, at = {c.lookAt.x - self.x, 0.0f, c.lookAt.z - self.z};
            lookAlong = AngleDeg(way, at) > 30.0f;
        }
    }
    if (lookAlong) Look(eye, {route.toward.x, eye.y, route.toward.z}, kCamAim, now);
    // Not aiming, the look is measured from the head, not the camera: the chase
    // camera circles the bot as it turns, so a look taken from the camera moved
    // its own starting point and swung back and forth every frame (29.09.: the
    // bots' heads jittered, their camera yaw flipped between two values 0.2 rad apart).
    else if (c.look) Look(c.aim ? AimOrigin(self, now) : eye, c.lookAt, c.aim ? kGunAim : kCamAim, now, c.lookVel);
    else { u->lastLookAt = now; u->lastLookQpc = 0; }
    p.buttons = buttons;
    u->pad = p;
    u->padLive = true;
}

// --- scenes and catching up --------------------------------------------------------
// While the leader's Isaac runs a scripted sequence (a cutscene, a scripted
// kill, a scene on a lift) the bots are hidden: body, the gun in hand and the
// helmet, through the game's own balanced hide count (Player vtable +0x68 and
// +0x6C, the way co-op hides partners, runtime_coop_cutscene.inc). A short one
// (a lever) does not hide them. When it ends they are shown again, and one that
// is far off is put beside the leader: scenes often carry him somewhere else.
// Outside scenes a bot that stays far from the leader (another floor, the far
// side of a closed door) is put by him too, and one left outside the lift the
// leader rides in is put into its cabin.
constexpr DWORD kSceneHideMs = 600, kFarOutMs = 3000, kFarOutCombatMs = 6000, kCabinMissMs = 2500;
constexpr float kSceneCatchUp = 6.0f, kFarOut = 20.0f, kFarOutCombat = 30.0f, kOtherFloor = 3.5f, kOtherFloorFlat = 4.0f;
bool sceneHide = false, autoTeleport = false;
// Which kinds of Isaac's scripted sequences hide the bots ([Bot] HideIn*):
// cinematics by default; scripted events, player actions and visions keep them.
bool hideKind[5] = {false, true, false, false, false};   // Unknown, Cinematic, Event, Action, Vision
volatile LONG sceneKindSeen[5] = {}, sceneUnknownLogged = 0;
coop_scenes::Kind SceneKindNow() {
    const uint32_t player = LocalCampaignPlayer();
    uint32_t sequence = 0, guid[4]{};
    if (!player || !Read(player + 0x238, &sequence, 4) || !sequence || !Read(sequence + 0x24, guid, 16)) return coop_scenes::Kind::Unknown;
    for (const auto& s : coop_scenes::kSequences)
        if (s.guid[0] == guid[0] && s.guid[1] == guid[1] && s.guid[2] == guid[2] && s.guid[3] == guid[3]) return s.kind;
    if (InterlockedIncrement(&sceneUnknownLogged) <= 20)
        gate::Event("bot: Isaac's sequence %08X-%08X-%08X-%08X is not in the scene table (kind unknown)", guid[0], guid[1], guid[2], guid[3]);
    return coop_scenes::Kind::Unknown;
}
volatile LONG sceneHides = 0, sceneShows = 0, sceneCatchUps = 0, farTeleports = 0, cabinTeleports = 0, cabinMisses = 0;
// For a client's ghosts of the host's bots: hidden in the same scenes the
// host hides the bots in, by the local Isaac's sequence.
bool HidesInThisScene() {
    const auto kind = static_cast<unsigned>(SceneKindNow());
    return sceneHide && kind < 5 && hideKind[kind];
}

void HideUnit() {
    if (u->hiddenBody || !u->entity) return;
    uint32_t vtable = 0;
    if (!cutscene::Hide(u->entity, vtable)) return;
    u->hiddenBody = u->entity;
    u->hiddenBodyVt = vtable;
    const uint32_t item = cutscene::ActiveItem(u->entity), helmet = cutscene::Helmet(u->entity);
    if (item && cutscene::Hide(item, u->hiddenItemVt)) u->hiddenItem = item;
    if (helmet && cutscene::Hide(helmet, u->hiddenHelmetVt)) u->hiddenHelmet = helmet;
    InterlockedIncrement(&sceneHides);
}
// Shown only while it is still the same living body (a new body after a respawn
// never had the hide; the old one is gone with its count).
void ShowUnit() {
    if (!u->hiddenBody) return;
    if (u->entity == u->hiddenBody && Alive(u->entity)) {
        if (u->hiddenItem && cutscene::ActiveItem(u->entity) == u->hiddenItem) cutscene::Show(u->hiddenItem, u->hiddenItemVt);
        if (u->hiddenHelmet && cutscene::Helmet(u->entity) == u->hiddenHelmet) cutscene::Show(u->hiddenHelmet, u->hiddenHelmetVt);
        cutscene::Show(u->hiddenBody, u->hiddenBodyVt);
        InterlockedIncrement(&sceneShows);
    }
    u->hiddenBody = u->hiddenItem = u->hiddenHelmet = 0;
}
// The leader climbs (PlayerLowerBodySM 39 kong: a ladder or shaft), rides a
// tube (30) or hangs (31): no bot is put by him until he is out and 2 s more
// (29.09.: the bots were put into the shaft he climbed). They wait below; the
// catch-up follows once he is out.
int32_t leaderBusyState = -1;
volatile LONG busyHolds = 0;
// The leader's root machine (PlayerRootSM, vtable 0x01B27598, at [player+0x24C]) in
// state 3, straitJacketMode: the run from the necromorphs at the start (the psych
// hall). The bots then run right behind him ([Bot] ChaseFollow), and one that
// falls behind is put by him after 1.5 s at 10 m.
constexpr uint32_t kRootSmVtableRva = 0x1727598;
constexpr int32_t kStraitJacketMode = 3;
constexpr float kChaseFarOut = 10.0f;
constexpr DWORD kChaseFarOutMs = 1500;
volatile LONG chaseStarts = 0, chaseTeleports = 0;
void UpdateLeaderChase(uint32_t leader) {
    uint32_t root = 0, vtable = 0;
    int32_t state = -1;
    const bool chaseNow = chaseFollow && Read(leader + kMachineRoot, &root, 4) && root && Read(root, &vtable, 4) &&
                          vtable == base + kRootSmVtableRva && Read(root + kMachineState, &state, 4) && state == kStraitJacketMode;
    if (chaseNow != leaderChase) {
        if (chaseNow) InterlockedIncrement(&chaseStarts);
        gate::Event(chaseNow ? "bot: the leader runs in the straitjacket: the bots stay right behind him, running"
                             : "bot: the leader is out of the straitjacket run");
    }
    leaderChase = chaseNow;
}
int32_t LowerBodyState(uint32_t player);   // after "using the kiosks", above
void UpdateLeaderBusy(int32_t state, DWORD now) {
    const bool busyNow = state == 30 || state == 31 || state == 39;
    if (busyNow) {
        if (!leaderBusy && InterlockedIncrement(&busyHolds) <= 40)
            gate::Event("bot: the leader climbs or rides (lower body state %d): no bot is put by him until he is out", state);
        leaderBusyUntil = now + 2000;
        leaderBusyState = state;
    }
    leaderBusy = now < leaderBusyUntil;
}
bool PutByLeader(const Vec3& self, const Vec3& leaderPos, const char* why) {
    const Vec3 spot = CatchUpSpot(leaderPos);
    if (!MoveEntity(u->entity, spot)) return false;
    u->steer.Reset();
    gate::Event("bot %u: put by the leader (%s): %.1f m from him before, now at (%.1f %.1f %.1f)", u->index + 1, why,
                coop_bot::Distance(self, leaderPos), spot.x, spot.y, spot.z);
    HoldInLift(spot, GetTickCount(), "put by the leader", false);
    return true;
}
// The scene part of a frame. True while the leader is in a scene (the bot waits).
bool SceneTick(const Vec3& self, const Vec3& leaderPos, DWORD now) {
    if (cutscene::SequenceRunningNow()) {
        if (!u->sceneSince) {
            u->sceneSince = now;
            const auto kind = static_cast<unsigned>(SceneKindNow());
            u->sceneHides = kind < 5 && hideKind[kind];
            if (u->index == 0 && kind < 5 && InterlockedIncrement(&sceneKindSeen[kind]) <= 40) {
                static const char* kNames[] = {"unknown", "cinematic", "scripted event", "player action", "vision"};
                gate::Event("bot: Isaac's scene starts (%s): the bots %s", kNames[kind], u->sceneHides ? "hide" : "stay visible");
            }
        }
        if (sceneHide && u->sceneHides && now - u->sceneSince >= kSceneHideMs) HideUnit();
        return true;
    }
    if (!u->sceneSince) return false;
    const bool wasHidden = u->hiddenBody != 0;
    ShowUnit();
    u->sceneSince = 0;
    if (autoTeleport && wasHidden && coop_bot::Distance(self, leaderPos) > kSceneCatchUp) {
        if (u->holding) InterlockedIncrement(&holdCatchUpsSkipped);
        else if (leaderBusy) u->catchUpPending = true;   // after his climb
        else if (PutByLeader(self, leaderPos, "after a scene")) InterlockedIncrement(&sceneCatchUps);
    }
    return false;
}
// Into the lift cabin the leader stands in: a free spot around him inside its box.
bool PutInCabin(const Vec3& self, const Vec3& leaderPos) {
    ghosttrigger::Cabin cabins[16]{};
    const unsigned n = CachedCabins(cabins, 16, GetTickCount());
    const float at[3] = {leaderPos.x, leaderPos.y, leaderPos.z};
    for (unsigned i = 0; i < n; ++i) {
        if (!ghosttrigger::InCabin(cabins[i], at)) continue;
        for (unsigned k = 0; k < 8; ++k) {
            const float a = static_cast<float>(k + u->index * 3) * (kPi / 4.0f);
            const Vec3 spot = {leaderPos.x + std::sin(a) * 0.9f, leaderPos.y, leaderPos.z + std::cos(a) * 0.9f};
            const float p[3] = {spot.x, spot.y, spot.z};
            if (!ghosttrigger::InCabin(cabins[i], p)) continue;
            bool mate = false;
            for (const Unit& other : units) mate = mate || (&other != u && other.entity && coop_bot::Flat(other.self, spot) < 0.6f);
            if (mate || !MoveEntity(u->entity, spot)) continue;
            u->steer.Reset();
            gate::Event("bot %u: put into the leader's lift (it was %.1f m from him)", u->index + 1, coop_bot::Distance(self, leaderPos));
            HoldInLift(spot, GetTickCount(), "put into the leader's lift", true);
            return true;
        }
    }
    return false;
}
bool InAnyCabin(const Vec3& p) {
    ghosttrigger::Cabin cabins[16]{};
    const unsigned n = CachedCabins(cabins, 16, GetTickCount());
    const float at[3] = {p.x, p.y, p.z};
    for (unsigned i = 0; i < n; ++i) if (ghosttrigger::InCabin(cabins[i], at)) return true;
    return false;
}
// Put down inside a lift cabin (any teleport, the call key too): it stands still
// for LiftHoldMs, no walking and no catch-up; it still looks, aims and shoots.
void HoldInLift(const Vec3& spot, DWORD now, const char* why, bool inCabin) {
    if (!liftHoldMs || !(inCabin || InAnyCabin(spot))) return;
    u->liftHoldUntil = now + liftHoldMs;
    InterlockedIncrement(&liftHolds);
    gate::Event("bot %u: %s, in a lift cabin: stands still for %lu ms", u->index + 1, why, static_cast<unsigned long>(liftHoldMs));
}
// Far from the leader for a while, or left outside his lift: to him.
void CatchUpTick(const Vec3& self, const Vec3& leaderPos, DWORD now) {
    if (u->holding && autoTeleport && (u->catchUpPending || u->zgWantsCatchUp || u->farSince || u->cabinMissSince)) InterlockedIncrement(&holdCatchUpsSkipped);
    if (u->holding) u->catchUpPending = u->zgWantsCatchUp = false;   // told to stay: no catch-up of any kind
    if (!autoTeleport || u->holding || u->kioskVisit != KioskStep::None || u->tkStep != TkStep::None || leaderBusy ||
        (u->liftHoldUntil && now < u->liftHoldUntil)) {
        u->farSince = u->cabinMissSince = 0;
        return;
    }
    if (u->catchUpPending) {
        u->catchUpPending = false;
        if (coop_bot::Distance(self, leaderPos) > kSceneCatchUp && PutByLeader(self, leaderPos, "after the leader's climb")) InterlockedIncrement(&sceneCatchUps);
        return;
    }
    if (u->zgWantsCatchUp) {
        u->zgWantsCatchUp = false;
        if (PutByLeader(self, leaderPos, "no landing by him in 10 s")) InterlockedIncrement(&zgTeleports);
        return;
    }
    const bool combat = u->command.mode == coop_bot::Mode::Combat;
    // The survival maps: far behind, another floor, the chase and the lift are no reason.
    if (OnlyStuckTeleports()) {
        const bool farOff = coop_bot::Flat(self, leaderPos) > (combat ? kFarOutCombat : kFarOut);
        if (farOff && !u->farSkipped) InterlockedIncrement(&distanceTeleportsSkipped);
        u->farSkipped = farOff;
        u->farSince = u->cabinMissSince = 0;
        return;
    }
    const float flat = coop_bot::Flat(self, leaderPos), dy = std::fabs(self.y - leaderPos.y);
    if (leaderChase) {   // the run: whoever falls behind is put right behind him at once
        if (coop_bot::Distance(self, leaderPos) <= kChaseFarOut) { u->farSince = 0; return; }
        if (!u->farSince) u->farSince = now;
        if (now - u->farSince >= kChaseFarOutMs) {
            u->farSince = 0;
            if (PutByLeader(self, leaderPos, "fell behind in the chase")) InterlockedIncrement(&chaseTeleports);
        }
        return;
    }
    // Zero-G: he walks on walls and flies, so no "other floor" there; far counts in 3D.
    const bool zeroGArea = zeroGFlight && (leaderZeroG || leaderFlying || u->zeroG || u->flying);
    const bool distant = zeroGArea ? coop_bot::Distance(self, leaderPos) > (combat ? kFarOutCombat : kFarOut) + 5.0f
                                   : flat > (combat ? kFarOutCombat : kFarOut) || (dy > kOtherFloor && flat > kOtherFloorFlat);
    if (!distant) {
        u->farSince = 0;
    } else if (!u->farSince) {
        u->farSince = now;
    } else if (now - u->farSince >= (combat ? kFarOutCombatMs : kFarOutMs)) {
        u->farSince = 0;
        if (PutByLeader(self, leaderPos, dy > kOtherFloor ? "another floor" : "far behind")) InterlockedIncrement(&farTeleports);
    }
    // The lift: checked every half second (the cabin list walks the SimManager's table).
    if (!leaderInCabin) { u->cabinMissSince = 0; return; }
    if (now - u->cabinCheckAt < 500) return;
    u->cabinCheckAt = now;
    if (InAnyCabin(self)) { u->cabinMissSince = 0; return; }
    if (!u->cabinMissSince) { u->cabinMissSince = now; InterlockedIncrement(&cabinMisses); return; }
    if (now - u->cabinMissSince >= kCabinMissMs) {
        u->cabinMissSince = 0;
        if (PutInCabin(self, leaderPos)) InterlockedIncrement(&cabinTeleports);
    }
}

void Status(const Vec3& self, const Vec3& leaderPos, DWORD now, const coop_bot::Perception& perception) {
    if (now - u->lastStatus < 5000) return;
    u->lastStatus = now;
    static const char* const kModes[] = {"idle", "follow", "rush", "combat", "pickup", "heal", "loot"};
    gate::Event("bot %u (%s): %s at (%.1f %.1f %.1f), %.1f m from the leader, health %.0f%%, enemies %d (limb points %u; necromorphs %u, near and alive %u, "
                "chain %u), unaware skips %u, items %d, loot spots %d, corpses %d, waypoints %d, power nodes %d, credits %d%s%s%s",
                u->index + 1, u->name, kModes[static_cast<unsigned>(u->command.mode) % std::size(kModes)], self.x, self.y, self.z, coop_bot::Distance(self, leaderPos),
                Health01(u->entity) * 100.0f, u->seen.enemyCount, u->seen.limbs, u->seen.npcs, u->seen.alive, u->seen.scanned, u->brain.unawareSkips, u->seen.itemCount, perception.lootCount,
                u->seen.corpseCount, u->steer.Waypoints(), u->nodeWallet, u->creditWallet,
                u->steer.Stuck() ? ", stuck" : "", leaderInCabin ? ", leader in a lift" : "", u->flying ? ", flying" : (u->zeroG ? ", in zero-G" : ""));
    if (u->holding || focusTarget || holdFire || vanguard)
        gate::Event("bot %u orders: %s%s, spot (%.1f %.1f %.1f) %.1f m off, target %08X%s, %s, %s formation", u->index + 1, u->holding ? "hold" : "follow",
                    u->goingThere ? " (on its way)" : "", u->holdSpot.x, u->holdSpot.y, u->holdSpot.z, coop_bot::Flat(self, u->holdSpot), focusTarget,
                    u->command.focused ? " (on it)" : "", !holdFire ? "fire at will" : (u->command.returnFire ? "hold fire, fighting back" : "hold fire"),
                    vanguard ? "attack" : "cover");
    if (u->brain.AnticNow() != coop_bot::Antic::None)
        gate::Event("bot %u: antic %s, phase %d, %lu ms in", u->index + 1, coop_bot::AnticName(u->brain.AnticNow()), u->brain.AnticPhase(),
                    static_cast<unsigned long>(now - u->brain.AnticSince()));
    for (int i = 0; i < perception.enemyCount; ++i) {
        const coop_bot::Enemy& e = perception.enemies[i];
        if (e.id != u->command.target || !u->command.target) continue;
        uint32_t vtable = 0;
        Read(e.id, &vtable, 4);
        gate::Event("bot %u: target %08X vtable %08X at %.1f m (flat %.1f, %+.1f m high), height %.2f, limbs %d, seen %d, aim off %.1f deg",
                    u->index + 1, e.id, vtable ? static_cast<unsigned>(vtable - base) : 0u, coop_bot::Distance(self, e.pos), coop_bot::Flat(self, e.pos),
                    e.pos.y - self.y, e.height, e.limbCount, e.visible ? 1 : 0, perception.aimErrorDeg);
    }
}

// --- players pass through one another -----------------------------------------------
// [Bot] PlayerCollision=0: the human and the bots walk through one another. Every
// Isaac's CharacterMovementComponent (vtable 0x01B63FBC, pointer at entity+0x17F4)
// keeps up to 8 entity ids it does not collide with (mFilterExcludeUIDs +0x248,
// count +0x240; its own id is mFilter.m_id +0x230), and
// EARS::Havok::CollisionFilter::IsWorldCollisionEnabled checks both lists, for the
// character proxy and the inner body alike. The engine's AddCollisionExcludeUID
// (0x00F61BE0) and RemoveCollisionExcludeUID (0x00F61C20), both thiscall(cmc, id)
// ret 4, also refresh the Havok filters. Checked twice a second: the game adds and
// drops ids of its own (use objects, paired attacks, drags).
constexpr uint32_t kCmcOwnId = 0x230, kCmcExcludeCount = 0x240, kCmcExcludeIds = 0x248, kCmcExcludeMax = 8, kAddExcludeRva = 0xB61BE0,
                   kRemoveExcludeRva = 0xB61C20;
bool passThrough = false;
DWORD apartAt = 0;
struct Apart { uint32_t cmc = 0, id = 0, ownerId = 0; };
Apart apartAdded[16];   // the ids this module put into which list
volatile LONG apartAdds = 0, apartRemoves = 0, apartFull = 0, apartLost = 0, apartFaults = 0;
bool Excludes(uint32_t cmc, uint32_t id, uint32_t* countOut = nullptr) {
    uint32_t count = 0, ids[kCmcExcludeMax]{};
    if (!Read(cmc + kCmcExcludeCount, &count, 4) || count > kCmcExcludeMax) return false;
    if (count && !Read(cmc + kCmcExcludeIds, ids, count * 4)) return false;
    if (countOut) *countOut = count;
    for (uint32_t i = 0; i < count; ++i) if (ids[i] == id) return true;
    return false;
}
bool CallExclude(uint32_t rva, uint32_t cmc, uint32_t id) {
    __try {
        reinterpret_cast<void(__thiscall*)(uint32_t, uint32_t)>(base + rva)(cmc, id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&apartFaults);
        return false;
    }
}
void KeepPlayersApart(uint32_t leader, DWORD now, bool ghosts = false) {
    if (!passThrough || now - apartAt < 500) return;
    apartAt = now;
    uint32_t cmcs[1 + kMaxBots]{}, ids[1 + kMaxBots]{};
    unsigned n = 0;
    auto take = [&](uint32_t entity) {
        const uint32_t cmc = entity ? CmcOf(entity) : 0;
        uint32_t id = 0;
        if (cmc && Read(cmc + kCmcOwnId, &id, 4) && id) { cmcs[n] = cmc; ids[n] = id; ++n; }
    };
    take(leader);
    if (ghosts) {
        for (unsigned slot = 0; slot < kMaxBots; ++slot) {
            const uint32_t entity = GhostForMachine(0xF0u + slot);
            if (entity && IsBotGhost(entity)) take(entity);
        }
    } else {
        for (const Unit& unit : units) take(unit.entity);
    }
    // An id put in for a body that is gone comes out again (ids are handed out
    // anew); a list whose own body is gone is not touched.
    for (Apart& a : apartAdded) {
        if (!a.cmc) continue;
        bool listHere = false, idHere = false;
        for (unsigned i = 0; i < n; ++i) { listHere = listHere || (cmcs[i] == a.cmc && ids[i] == a.ownerId); idHere = idHere || ids[i] == a.id; }
        if (listHere && idHere) continue;
        if (listHere && Excludes(a.cmc, a.id) && CallExclude(kRemoveExcludeRva, a.cmc, a.id)) InterlockedIncrement(&apartRemoves);
        a = Apart{};
    }
    for (unsigned i = 0; i < n; ++i)
        for (unsigned k = 0; k < n; ++k) {
            uint32_t count = kCmcExcludeMax;
            if (i == k || ids[i] == ids[k] || Excludes(cmcs[i], ids[k], &count)) continue;
            if (count >= kCmcExcludeMax) { InterlockedIncrement(&apartFull); continue; }
            if (!CallExclude(kAddExcludeRva, cmcs[i], ids[k])) continue;
            if (!Excludes(cmcs[i], ids[k], &count)) { InterlockedIncrement(&apartLost); continue; }
            bool noted = false;
            for (const Apart& a : apartAdded) noted = noted || (a.cmc == cmcs[i] && a.id == ids[k]);
            if (!noted)
                for (Apart& a : apartAdded)
                    if (!a.cmc) { a.cmc = cmcs[i]; a.id = ids[k]; a.ownerId = ids[i]; break; }
            if (InterlockedIncrement(&apartAdds) <= 40)
                gate::Event("bot: players pass through: the movement %08X (id %X) now ignores id %X (%u in its list)", cmcs[i], ids[i], ids[k], count);
        }
}
bool InstallPassThrough(FILE* log, bool wanted) {
    passThrough = false;
    if (!wanted) {
        std::fprintf(log, "bot player_collision=1 (the human and the bots collide as usual)\n");
        return false;
    }
    // push esi / push edi / mov edi,[esp+0Ch] / push edi / mov esi,ecx / call IsUIDCollisionExcluded
    static const unsigned char kAdd[] = {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x57, 0x8B, 0xF1, 0xE8};
    // test al,al / jne / mov eax,[esi+240h] / cmp eax,8
    static const unsigned char kAddCheck[] = {0x84, 0xC0, 0x75};
    static const unsigned char kAddCount[] = {0x8B, 0x86, 0x40, 0x02, 0x00, 0x00, 0x83, 0xF8, 0x08};
    // push esi / mov esi,ecx / mov edx,[esi+240h] / xor eax,eax / test edx,edx / jbe
    static const unsigned char kRemove[] = {0x56, 0x8B, 0xF1, 0x8B, 0x96, 0x40, 0x02, 0x00, 0x00, 0x33, 0xC0, 0x85, 0xD2, 0x0F, 0x86};
    unsigned char add[28]{}, remove[sizeof(kRemove)]{};
    const bool readable = Read(base + kAddExcludeRva, add, sizeof(add)) && Read(base + kRemoveExcludeRva, remove, sizeof(remove));
    const bool addOk = readable && std::memcmp(add, kAdd, sizeof(kAdd)) == 0 && std::memcmp(add + 14, kAddCheck, sizeof(kAddCheck)) == 0 &&
                       std::memcmp(add + 18, kAddCount, sizeof(kAddCount)) == 0;
    const bool removeOk = readable && std::memcmp(remove, kRemove, sizeof(kRemove)) == 0;
    passThrough = addOk && removeOk;
    std::fprintf(log, "bot player_collision=0 pass_through=%d (add=%d remove=%d: the human and the bots walk through one another, through the "
                      "movement components' own exclude lists)\n",
                 passThrough ? 1 : 0, addOk ? 1 : 0, removeOk ? 1 : 0);
    return passThrough;
}

// --- kinesis and stasis modules ----------------------------------------------------------
// The level gives Isaac the
// modules by broadcasting player_enable_tk / player_enable_stasis (the disable pair
// at level start takes them away); every Player's HandleEvents then sets or clears
// bit 0x10 (kinesis) or 0x08 (stasis) of +0x3D8. The bots hear it too, but one
// built after the grant restores Isaac's older saved record (chapter 1: no module
// after a restart), and the stasis energy fill that follows reaches "the player" by
// GUID only. So twice a second each bot takes Isaac's two bits and his 0x10000
// "stasis picked up" bit (set only: no first-pickup tutorial for a bot), and a bot
// whose stasis just came on gets his energy through HealthComponent::AddEnergy.
// The brain's stasis energy comes from here as well (it cast with an empty pack).
constexpr uint32_t kAbilities = 0x3D8, kAbilityStasis = 0x08, kAbilityTk = 0x10, kStasisPickedUp = 0x10000, kInitDone = 1u << 28;
constexpr uint32_t kGetHealthRva = 0xAB8D80, kAddEnergyRva = 0xB55D30, kStasisEnergy = 0xF0, kPlayerAttributes = 0xC, kStasisCostAttr = 0x45C;
bool mirrorModules = false, energyReady = false;
DWORD modulesAt = 0;
volatile LONG moduleChanges = 0, energyTopUps = 0, moduleFaults = 0;
uint32_t HealthOf(uint32_t entity) {   // GetDHC, PC 0x00EB8D80, thiscall(entity), ret
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(uint32_t)>(base + kGetHealthRva)(entity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&moduleFaults);
        return 0;
    }
}
bool AddEnergy(uint32_t health, float amount) {   // PC 0x00F55D30, thiscall(health, float), ret 4, clamps to its max
    __try {
        reinterpret_cast<void(__thiscall*)(uint32_t, float)>(base + kAddEnergyRva)(health, amount);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&moduleFaults);
        return false;
    }
}
bool EnergyOf(uint32_t health, float& energy, float& max) {   // stasis energy +0xF0, its max +0xF4
    float values[2]{};
    if (!health || !Read(health + kStasisEnergy, values, sizeof(values))) return false;
    if (!std::isfinite(values[0]) || !std::isfinite(values[1]) || values[1] <= 0.0f || values[1] > 10000.0f) return false;
    energy = values[0];
    max = values[1];
    return true;
}
void ModulesTick(uint32_t leader, DWORD now) {
    if (!energyReady || now - modulesAt < 500) return;
    modulesAt = now;
    uint32_t leaderBits = 0, leaderFlags = 0;
    if (!Read(leader + kAbilities, &leaderBits, 4) || !Read(leader + kPlayerFlags, &leaderFlags, 4) || !(leaderFlags & kInitDone)) return;
    float cost = 20.0f;
    {
        uint32_t attributes = 0;
        float c = 0.0f;
        if (Read(leader + kPlayerAttributes, &attributes, 4) && attributes && Read(attributes + kStasisCostAttr, &c, 4) && std::isfinite(c) && c > 0.0f && c < 1000.0f)
            cost = c;
    }
    float leaderEnergy = 0.0f, leaderMax = 0.0f;
    const bool leaderEnergyKnown = EnergyOf(HealthOf(leader), leaderEnergy, leaderMax);
    for (Unit& unit : units) {
        if (!unit.entity || unit.state != State::Active) continue;
        uint32_t bits = 0;
        if (!Read(unit.entity + kAbilities, &bits, 4)) continue;
        if (mirrorModules) {
            constexpr uint32_t mask = kAbilityStasis | kAbilityTk;
            const uint32_t want = (bits & ~mask) | (leaderBits & mask) | (leaderBits & kStasisPickedUp);
            if (want != bits && WriteBytes(unit.entity + kAbilities, &want, 4)) {
                InterlockedIncrement(&moduleChanges);
                gate::Event("bot %u: modules as the leader's: kinesis %d, stasis %d (bits %08X -> %08X, leader %08X)", unit.index + 1,
                            (want & kAbilityTk) ? 1 : 0, (want & kAbilityStasis) ? 1 : 0, bits, want, leaderBits);
                bits = want;
            }
        }
        const uint32_t health = HealthOf(unit.entity);
        float energy = 0.0f, max = 0.0f;
        const bool energyKnown = EnergyOf(health, energy, max);
        // Stasis just came on (mirrored here or by the level's broadcast): the leader's energy, as the level fills his.
        if (mirrorModules && (bits & kAbilityStasis) && (!unit.modulesKnown || !(unit.modulesSeen & kAbilityStasis)) && energyKnown && leaderEnergyKnown) {
            const float target = leaderEnergy < max ? leaderEnergy : max;
            if (energy + 0.5f < target && AddEnergy(health, target - energy)) {
                InterlockedIncrement(&energyTopUps);
                const float before = energy;
                EnergyOf(health, energy, max);
                gate::Event("bot %u: stasis energy %.0f -> %.0f of %.0f (the leader has %.0f)", unit.index + 1, before, energy, max, leaderEnergy);
            }
        }
        unit.modulesSeen = bits;
        unit.modulesKnown = true;
        unit.tkModule = !mirrorModules || (bits & kAbilityTk) != 0;
        unit.stasisKnown = energyKnown || !(bits & kAbilityStasis);
        unit.stasis01 = (bits & kAbilityStasis) && energyKnown && energy >= cost ? energy / max : 0.0f;
    }
}

// A bot that moves while the left stick of its pad has stood still a while:
// something other than its own stick moves it (the leader's keys reaching its pad,
// a push, a scripted move). Logged with the leader's walk at the same moment, so
// one test tells whether his input moves the bots or their brain does.
volatile LONG driftFrames = 0, driftWithLeader = 0, driftWithDirection = 0, driftLogs = 0, noPairedRestored = 0;
void CheckDrift(const Vec3& self, const Vec3& leaderPos, DWORD now) {
    const bool still = std::fabs(u->pad.lx) < 0.05f && std::fabs(u->pad.ly) < 0.05f;   // what the pad got last frame
    if (!still) u->stickStillSince = 0;
    else if (!u->stickStillSince) u->stickStillSince = now;
    if (u->driftAt && now - u->driftAt < 100) return;
    const float dt = static_cast<float>(now - u->driftAt) / 1000.0f;
    const bool had = u->driftAt != 0 && dt < 0.3f;
    const Vec3 was = u->driftFrom, leaderWas = u->driftLeaderFrom;
    u->driftFrom = self;
    u->driftLeaderFrom = leaderPos;
    u->driftAt = now;
    if (!had || !u->stickStillSince || now - u->stickStillSince < 300 || u->flying || u->kioskVisit != KioskStep::None) return;
    // Scenes, lifts and enemies' grabs move a bot on their own.
    const int32_t s = u->lowerState;
    if (cutscene::SequenceRunningNow() || leaderInCabin || (u->liftHoldUntil && now < u->liftHoldUntil) || s == 27 || s == 28 || s == 29 || s == 33 || s == 40) return;
    const Vec3 moved = {self.x - was.x, 0.0f, self.z - was.z}, leaderMoved = {leaderPos.x - leaderWas.x, 0.0f, leaderPos.z - leaderWas.z};
    const float speed = coop_bot::Length(moved) / dt, leaderSpeed = coop_bot::Length(leaderMoved) / dt;
    if (speed < 1.0f || speed > 8.0f) return;   // standing, or a teleport
    InterlockedIncrement(&driftFrames);
    int32_t direction = -1;
    Read(u->entity + kMoveDirection, &direction, 4);
    if (direction > 0) InterlockedIncrement(&driftWithDirection);
    const float same = leaderSpeed > 0.5f ? coop_bot::Dot(moved, leaderMoved) / (coop_bot::Length(moved) * coop_bot::Length(leaderMoved)) : 0.0f;
    if (leaderSpeed > 1.0f && same > 0.7f) InterlockedIncrement(&driftWithLeader);
    if (InterlockedIncrement(&driftLogs) <= 30) {
        // Its walk direction (0 none) and the human's stick: a direction with both still is not the leader's keys.
        float stick15[2]{};
        uint32_t dm = 0;
        if (Read(base + kDeviceManagerRva, &dm, 4) && dm) Read(dm + kPadBase + 15 * kPadSize, stick15, sizeof(stick15));
        gate::Event("bot %u: moved %.1f m/s with its stick still for %lu ms (leader %.1f m/s, same way %.2f, lower body %d, mode %d, dir %d, pad15 %.2f/%.2f)",
                    u->index + 1, speed, static_cast<unsigned long>(now - u->stickStillSince), leaderSpeed, same, u->lowerState,
                    static_cast<int>(u->command.mode), direction, stick15[0], stick15[1]);
    }
}

// --- the alt fire trace ([Bot] AltFireTrace) ---------------------------------------
// 05.10.: the human's alt fire (Space while aiming: pad 15 button 0x0800, event
// 0xF12F9141) sometimes did nothing. Its gate (0x008730B0, cdecl(Player*), bool:
// active item +0xCEC, no wish at +0x424, item vtable+0x250 ready, alt type
// [[item+0xC]+0x1D4] == 2, behaviour bits 4/5/13 clear, the event just triggered
// on the Player's own pad) is watched while pad 15 holds 0x0800. Each press while
// aiming is logged 300 ms later with both context-stack tops and the bots' states.
constexpr uint32_t kAltGateRva = 0x4730B0, kAltFireEvent = 0xF12F9141u, kBehaviour = 0x298;
bool altTrace = false;
safetyhook::InlineHook altGateHook;
struct AltGate {
    bool open = false, first = false;
    DWORD openAt = 0;
    LONG calls = 0, passes = 0;
    uint32_t item = 0, wish = 0, behaviour = 0;
    int ready = -1, altType = -1, just = -1;
};
AltGate altGate;
uint32_t altLeader = 0;
struct AltPress {
    bool pending = false;
    DWORD at = 0;
    uint32_t behaviour = 0;
    int32_t lower = -1;
    unsigned long long lowerSeen = 0;
    int ctx[2]{-2, -2};
    uint32_t depth[2]{};
    char bots[200] = "";
};
AltPress altPress;
volatile LONG altPresses = 0, altLogged = 0, altGatePasses = 0;
uint16_t altLast15 = 0;
uint16_t Pad15Buttons() {
    uint32_t dm = 0;
    uint16_t b = 0;
    if (Read(base + kDeviceManagerRva, &dm, 4) && dm) Read(dm + kPadBase + 15 * kPadSize + 0x10, &b, 2);
    return b;
}
// The top context of a device group of the global input context stacks, its depth in depth.
int ContextTop(unsigned group, uint32_t& depth) {
    uint32_t sys = 0, defs = 0, groups = 0, arr = 0, def = 0;
    depth = 0;
    if (!Read(base + kContextStackRva, &sys, 4) || !sys || !Read(sys + 0x1C, &defs, 4) || !Read(sys + 0x10, &groups, 4) || !groups) return -2;
    if (!Read(groups + group * 16 + 4, &arr, 4) || !Read(groups + group * 16 + 0xC, &depth, 4) || depth > 64) return -2;
    if (!depth) return -1;
    if (!Read(arr + (depth - 1) * 8 + 4, &def, 4) || def < defs) return -2;
    return static_cast<int>((def - defs) / 12);
}
__declspec(noinline) void AltGateInputs(uint32_t player, AltGate& g) {
    __try {
        uint32_t ref = 0;
        Read(player + kActiveItem, &ref, 4);
        g.item = ref ? ref - 0x10 : 0;
        Read(player + kDesiredItem, &g.wish, 4);
        Read(player + kBehaviour, &g.behaviour, 4);
        uint32_t vt = 0, fn = 0, desc = 0;
        int32_t type = -1;
        if (g.item && Read(g.item, &vt, 4) && Read(vt + 0x250, &fn, 4) && fn)
            g.ready = reinterpret_cast<bool(__thiscall*)(void*)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(g.item))) ? 1 : 0;
        if (g.item && Read(g.item + 0xC, &desc, 4) && desc && Read(desc + 0x1D4, &type, 4)) g.altType = type;
        uint32_t cm = 0, ctrl = 15;
        Read(player + kPlayerController, &ctrl, 4);
        if (Read(base + kControllerManagerRva, &cm, 4) && cm && Read(cm, &vt, 4) && Read(vt + 0x14, &fn, 4) && fn)
            g.just = reinterpret_cast<bool(__thiscall*)(void*, uint32_t, uint32_t)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(cm)), ctrl & 0xFF,
                                                                                          kAltFireEvent) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
bool __cdecl OnAltGate(uint32_t player) {
    const bool result = altGateHook.ccall<bool>(player);
    if (altTrace && player && player == altLeader) {
        if (Pad15Buttons() & kAltFire) {
            if (!altGate.open) { altGate = AltGate{}; altGate.open = true; altGate.openAt = GetTickCount(); }
            ++altGate.calls;
            if (result) { ++altGate.passes; InterlockedIncrement(&altGatePasses); }
            if (!altGate.first) { altGate.first = true; AltGateInputs(player, altGate); }
        } else {
            altGate.open = false;
        }
    }
    return result;
}
bool InstallAltTrace(FILE* log) {
    // push ecx / push esi / mov esi,[esp+0Ch] / mov eax,[esi+0CECh] / mov byte [esp+7],0
    static const unsigned char kGate[] = {0x51, 0x56, 0x8B, 0x74, 0x24, 0x0C, 0x8B, 0x86, 0xEC, 0x0C, 0x00, 0x00, 0xC6, 0x44, 0x24, 0x07, 0x00};
    const bool gate = hook_site::Install(log, "bot alt fire gate", base + kAltGateRva, kGate, sizeof(kGate), reinterpret_cast<void*>(OnAltGate), altGateHook);
    bool push = false, pop = false;
    InstallContextHooks(log, push, pop);
    altTrace = gate;
    std::fprintf(log, "bot alt_fire_trace=%d gate=%d context_push=%d context_pop=%d (each aiming press of the human's alt fire is logged, first 40)\n",
                 altTrace ? 1 : 0, gate ? 1 : 0, push ? 1 : 0, pop ? 1 : 0);
    return altTrace;
}
void AltFireTick(uint32_t leader, int32_t lowerNow, DWORD now) {
    if (!altTrace) return;
    altLeader = leader;
    const uint16_t b = Pad15Buttons();
    const bool rising = (b & kAltFire) && !(altLast15 & kAltFire);
    if (!(b & kAltFire)) altGate.open = false;
    altLast15 = b;
    if (altPress.pending) {
        if (lowerNow >= 0 && lowerNow < 64) altPress.lowerSeen |= 1ull << lowerNow;
        if (now - altPress.at >= 300) {
            altPress.pending = false;
            const AltGate& g = altGate;
            const LONG n = InterlockedIncrement(&altLogged);
            gate::Event("bot alt_fire %ld: gate calls %ld passes %ld (item %08X ready %d type %d wish %08X behaviour %08X just %d), lower %d seen %llX",
                        n, g.calls, g.passes, g.item, g.ready, g.altType, g.wish, g.behaviour, g.just, altPress.lower, altPress.lowerSeen);
            gate::Event("bot alt_fire %ld: behaviour %08X, contexts %d(%u)/%d(%u), bots %s", n, altPress.behaviour, altPress.ctx[0], altPress.depth[0],
                        altPress.ctx[1], altPress.depth[1], altPress.bots);
        }
    }
    if (!rising) return;
    uint32_t behaviour = 0;
    if (!Read(leader + kBehaviour, &behaviour, 4) || !(behaviour & 0x80)) return;
    InterlockedIncrement(&altPresses);
    if (altPress.pending || altLogged >= 40) return;
    if (!altGate.open) altGate = AltGate{};   // a gate call this frame already belongs to this press
    altPress = AltPress{};
    altPress.pending = true;
    altPress.at = now;
    altPress.behaviour = behaviour;
    altPress.lower = lowerNow;
    if (lowerNow >= 0 && lowerNow < 64) altPress.lowerSeen = 1ull << lowerNow;
    for (unsigned g = 0; g < 2; ++g) altPress.ctx[g] = ContextTop(g, altPress.depth[g]);
    size_t used = 0;
    for (const Unit& unit : units) {
        if (!unit.entity || used + 48 >= sizeof(altPress.bots)) continue;
        uint32_t flags = 0;
        Read(unit.entity + kBehaviour, &flags, 4);
        const int wrote = std::snprintf(altPress.bots + used, sizeof(altPress.bots) - used, "%s%u:l%d a%u p%04X k%d t%d z%d m%d", used ? " " : "", unit.index + 1,
                                        unit.lowerState, (flags >> 7) & 1u, unit.lastWritten, static_cast<int>(unit.kioskVisit), static_cast<int>(unit.tkStep),
                                        unit.zgState, static_cast<int>(unit.command.mode));
        if (wrote > 0) used += static_cast<size_t>(wrote);
    }
}

#include "runtime_coop_bot_wheel.inc"
#include "runtime_coop_bot_light.inc"
#include "runtime_coop_bot_face.inc"
#include "runtime_coop_bot_antics.inc"
#include "runtime_coop_bot_usables.inc"
#include "runtime_coop_bot_tube.inc"   // after the usables: the use guard knows their classes

// While the leader works a store, the squad waits behind him in fixed places, out
// of his view of the menu ([Bot] StoreWaitPlaces). Each place is taken from the
// store's dock point (where the shopper stands) in the shopper's frame: metres
// back from the store, metres to the shopper's right, and the turn from the
// shopper's facing (positive = to his right). The first one was measured on
// 06.10. at the first store, where a player stood watching a suit change.
struct WaitPlace { float back, right, turnDeg; };
// 07.10.: 2.2 m right behind the shopper stood in the store camera, which looks
// over his shoulder; the places moved out to the sides, and any place between that
// camera and him is pushed off its line (kCamClear).
constexpr WaitPlace kStoreWait[3] = {{3.00f, 1.40f, 57.5f}, {3.00f, -1.40f, -57.5f}, {4.20f, 0.00f, 20.0f}};
constexpr float kCamClear = 1.3f;
volatile LONG storeWaitCamPushes = 0;
// The ground around a kiosk is often off the bots' navmesh (07.10.: they stopped
// at its edge, 2 m short, and never turned): the last kWaitDirect metres in sight
// go straight at the place, the turn comes within kWaitLook, and a bot still short
// of it after kWaitSettleMs settles where it is if that is within kWaitNear.
constexpr float kWaitArrive = 0.35f, kWaitDirect = 4.0f, kWaitLook = 1.0f, kWaitNear = 2.5f;
constexpr DWORD kWaitSettleMs = 6000;
bool storeWaitWanted = true;
volatile LONG storeWaitFrames = 0, storeWaitArrivals = 0, storeWaitLines = 0, storeWaitDirectFrames = 0, storeWaitSettledShort = 0, storeWaitAimTaps = 0;
void StoreWait(const Vec3& self, uint32_t leader) {
    const bool was = u->storeWaiting;
    u->storeWaiting = false;
    if (!storeWaitWanted || !leader || u->holding || u->kioskVisit != KioskStep::None) return;
    coop_bot::Command& c = u->command;
    if (c.mode != coop_bot::Mode::Idle && c.mode != coop_bot::Mode::Follow && c.mode != coop_bot::Mode::Loot) return;
    int32_t state = -1;
    bool store = false;
    const uint32_t sm = KioskMachine(leader, state, store);
    const uint32_t kiosk = sm && store ? KioskOf(sm, true) : 0;
    if (!kiosk) return;
    float m[16]{};
    if (!Read(kiosk + 0xD0, m, sizeof(m))) return;
    const Vec3 at = {m[12], m[13], m[14]};
    const Vec3 dock = DockPoint(kiosk, true, 0);
    Vec3 front = {m[8], 0.0f, m[10]};
    const float len = coop_bot::Length(front);
    if (len < 0.05f) return;
    front = coop_bot::Scale(front, 1.0f / len);
    if (coop_bot::Dot({dock.x - at.x, 0.0f, dock.z - at.z}, front) < 0.0f) front = coop_bot::Scale(front, -1.0f);
    const Vec3 facing = coop_bot::Scale(front, -1.0f);   // the shopper looks at the store
    const Vec3 right = {facing.z, 0.0f, -facing.x};
    const WaitPlace& w = kStoreWait[u->index % 3];
    Vec3 spot = coop_bot::Add(dock, coop_bot::Add(coop_bot::Scale(front, w.back), coop_bot::Scale(right, w.right)));
    // Out of the leader's camera: from him toward his camera, anything nearer than
    // the camera (and a little past it) within kCamClear of that line goes sideways.
    Vec3 shopperAt;
    if (bot_sense::Pos(leader, shopperAt)) {
        const Vec3 head = {shopperAt.x, shopperAt.y + kEye, shopperAt.z};
        const Vec3 cam = CameraEyeOf(shopperAt, 0);
        Vec3 axis = {cam.x - head.x, 0.0f, cam.z - head.z};
        const float camDist = coop_bot::Length(axis);
        if (camDist > 0.3f) {
            axis = coop_bot::Scale(axis, 1.0f / camDist);
            const Vec3 rel = {spot.x - shopperAt.x, 0.0f, spot.z - shopperAt.z};
            const float along = coop_bot::Dot(rel, axis);
            Vec3 side = coop_bot::Sub(rel, coop_bot::Scale(axis, along));
            float lateral = coop_bot::Length(side);
            if (along > -0.5f && along < camDist + 0.8f && lateral < kCamClear) {
                Vec3 out = {axis.z, 0.0f, -axis.x};   // straight on the line: its own side by its index
                if (u->index % 2) out = coop_bot::Scale(out, -1.0f);
                if (lateral >= 0.05f) out = coop_bot::Scale(side, 1.0f / lateral);
                const Vec3 push = coop_bot::Scale(out, kCamClear - (lateral >= 0.05f ? lateral : 0.0f));
                spot = coop_bot::Add(spot, {push.x, 0.0f, push.z});
                InterlockedIncrement(&storeWaitCamPushes);
            }
        }
    }
    const float yaw = std::atan2(facing.x, facing.z) + w.turnDeg * 0.01745329f;
    u->storeWaiting = true;
    InterlockedIncrement(&storeWaitFrames);
    c = coop_bot::Command{};
    c.mode = coop_bot::Mode::Idle;
    const float off = coop_bot::Flat(self, spot);
    const DWORD now = GetTickCount();
    if (!was) { u->storeWaitSince = now; u->storeWaitRayAt = 0; u->storeWaitArrived = false; u->storeWaitTaps = 0; u->storeWaitAimUntil = 0; }
    const bool shortSettle = off > kWaitArrive && off <= kWaitNear && now - u->storeWaitSince > kWaitSettleMs;
    if (off > kWaitArrive && !shortSettle) {
        c.move = true;
        c.moveTo = spot;
        c.run = off > 4.0f;
        if (off <= kWaitDirect) {
            if (!u->storeWaitRayAt || now - u->storeWaitRayAt >= 300) {
                u->storeWaitRayAt = now;
                u->storeWaitClear = !bot_sense::losReady ||
                                    bot_sense::Clear({self.x, self.y + 1.0f, self.z}, {spot.x, spot.y + 1.0f, spot.z}, u->entity, 0);
            }
            c.direct = u->storeWaitClear;
            if (c.direct) InterlockedIncrement(&storeWaitDirectFrames);
        }
    }
    if (off <= kWaitLook || shortSettle) {
        c.look = true;
        c.lookAt = {spot.x + std::sin(yaw) * 8.0f, spot.y + 1.6f, spot.z + std::cos(yaw) * 8.0f};
        // Standing, the body does not follow the camera; a short aim does, all the
        // way (a trick found in play, 07.10.). Once the camera points the right way and
        // the body is still off, a 150 ms tap, at most three per wait.
        float heading = 0.0f, pitch = 0.0f;
        if (u->storeWaitAimUntil && now < u->storeWaitAimUntil) {
            c.aim = true;
        } else if (!c.move && u->storeWaitTaps < 3 && now - u->storeWaitAimUntil >= 600 && AimAngles(u->entity, kCamAim, heading, pitch) &&
                   std::fabs(Wrap(yaw - heading)) < 0.1f && std::fabs(Wrap(yaw - bot_sense::Yaw(u->entity))) > 0.26f) {
            u->storeWaitAimUntil = now + 150;
            ++u->storeWaitTaps;
            InterlockedIncrement(&storeWaitAimTaps);
            c.aim = true;
        }
    }
    if ((off <= kWaitArrive || shortSettle) && !u->storeWaitArrived) {
        u->storeWaitArrived = true;
        InterlockedIncrement(shortSettle ? &storeWaitSettledShort : &storeWaitArrivals);
        if (InterlockedIncrement(&storeWaitLines) <= 60)
            gate::Event("bot %u: %s its store place after %lu ms (%.2f m off it)", u->index + 1, shortSettle ? "settles short of" : "is at",
                        static_cast<unsigned long>(now - u->storeWaitSince), off);
    }
    if (!was && InterlockedIncrement(&storeWaitLines) <= 60)
        gate::Event("bot %u: waits for the leader at the store %08X: place %u, %.2f m back, %.2f m right, turned %.0f deg (%.1f m off it)", u->index + 1, kiosk,
                    u->index % 3 + 1, w.back, w.right, w.turnDeg, off);
}
// While the leader works a store or a bench, no bot comes within kioskKeepOut of
// it ([Bot] KioskKeepOut, metres, 0 = off): a goal inside is moved out along the
// line from the kiosk, and a bot already inside walks out to the edge plus half a
// metre. Fights, heals and orders are left alone (07.10.: the bots stood at the
// counter and in the way of the store camera).
float kioskKeepOut = 2.0f;
volatile LONG keepOutGoals = 0, keepOutWalks = 0, keepOutLines = 0;
void KioskKeepOut(const Vec3& self, uint32_t leader) {
    if (kioskKeepOut <= 0.0f || !leader || u->holding || u->kioskVisit != KioskStep::None) return;
    coop_bot::Command& c = u->command;
    if (c.mode != coop_bot::Mode::Idle && c.mode != coop_bot::Mode::Follow && c.mode != coop_bot::Mode::Loot) return;
    int32_t state = -1;
    bool store = false;
    const uint32_t sm = KioskMachine(leader, state, store);
    const uint32_t kiosk = sm ? KioskOf(sm, store) : 0;
    float m[16]{};
    if (!kiosk || !Read(kiosk + 0xD0, m, sizeof(m))) return;
    const Vec3 at = {m[12], m[13], m[14]};
    if (!std::isfinite(at.x) || !std::isfinite(at.z)) return;
    // The way out: from the kiosk through the point, else the kiosk's own facing.
    auto outward = [&](const Vec3& from) {
        Vec3 d = {from.x - at.x, 0.0f, from.z - at.z};
        const float len = coop_bot::Length(d);
        if (len > 0.05f) return coop_bot::Scale(d, 1.0f / len);
        Vec3 f = {m[8], 0.0f, m[10]};
        const float fl = coop_bot::Length(f);
        return fl > 0.05f ? coop_bot::Scale(f, 1.0f / fl) : Vec3{0.0f, 0.0f, 1.0f};
    };
    const float edge = kioskKeepOut + 0.5f;
    if (c.move && coop_bot::Flat(c.moveTo, at) < kioskKeepOut) {
        const Vec3 out = outward(c.moveTo);
        c.moveTo = {at.x + out.x * edge, c.moveTo.y, at.z + out.z * edge};
        c.direct = false;
        InterlockedIncrement(&keepOutGoals);
    }
    if (coop_bot::Flat(self, at) < kioskKeepOut && (!c.move || coop_bot::Flat(c.moveTo, at) < kioskKeepOut)) {
        const Vec3 out = outward(self);
        c.move = true;
        c.moveTo = {at.x + out.x * edge, self.y, at.z + out.z * edge};
        c.run = false;
        c.look = false;
        InterlockedIncrement(&keepOutWalks);
        if (!u->keepingOut && InterlockedIncrement(&keepOutLines) <= 60)
            gate::Event("bot %u: steps back from the %s %08X the leader works (%.1f m from it)", u->index + 1, store ? "store" : "bench", kiosk,
                        coop_bot::Flat(self, at));
        u->keepingOut = true;
    } else {
        u->keepingOut = false;
    }
}
// Game thread, every frame (from coop::Poll).
// One bot's frame: spawn it when its turn comes, else senses, brain, pad.
void TickUnit(uint32_t leader, const Vec3& leaderPos, DWORD now, bool callPressed) {
    if (callPressed && u->entity && !cutscene::SequenceRunningNow()) {
        const Vec3 spot = SpotAtLeader(leaderPos);
        if (MoveEntity(u->entity, spot)) {
            InterlockedIncrement(&calls);
            u->steer.Reset();
            gate::Event("bot %u: called over by the key, put at (%.1f %.1f %.1f)", u->index + 1, spot.x, spot.y, spot.z);
            HoldInLift(spot, now, "called over", false);
        }
    }
    if (!u->entity) {
        // Only on the machine that owns the world: solo or the host. A client's own bot
        // would be one more Isaac nobody else sees. One spawn at a time: the ones
        // before this have to stand first.
        if (userOff || u->failedForGood || u->state == State::Off || u->state == State::Failed || !IsHost()) return;
        if (!leader) return;   // spawned from the leader's own template
        if (u->index >= BotSeats(now)) return;   // a human has this seat
        // a partner's ghost may have taken the slot first; the explicit
        // registration would overwrite it. After a second there it moves up.
        if (const uint32_t other = SlotOccupant(u->slotId)) {
            if (other != u->blockedBy) { u->blockedBy = other; u->blockedSince = now; }
            if (now - u->blockedSince < 1000 || !MoveGhostOut(u->slotId, other)) {
                if (InterlockedIncrement(&slotBusy) <= 10)
                    gate::Event("bot %u: Player slot %u is taken (%08X), no spawn there", u->index + 1, u->slotId, other);
                u->settledSince = now;
                return;
            }
        }
        u->blockedBy = 0;
        // The ones before this stand first; one whose slot stays someone else's
        // for 3 s holds nobody up.
        for (unsigned i = 0; i < u->index; ++i) {
            const Unit& before = units[i];
            if (before.state == State::Failed || before.failedForGood || (before.entity && before.state == State::Active)) continue;
            if (!before.entity && before.blockedBy && now - before.blockedSince >= 3000) continue;
            u->settledSince = 0;
            return;
        }
        if (!u->settledSince) u->settledSince = now;
        if (now - u->settledSince < spawnDelayMs || cutscene::SequenceRunningNow()) return;
        u->state = State::Waiting;
        Spawn(leader, leaderPos, now);
        if (!u->entity) { u->settledSince = now; return; }   // try again after the delay
        if (u->diedAt) {
            InterlockedIncrement(&versusBotRespawns);
            gate::Event("bot %u: back %lu ms after its death (%s)", u->index + 1, static_cast<unsigned long>(now - u->diedAt), MortalWhy());
            u->diedAt = 0;
        }
        return;
    }
    // Dead after all (something that kills outright): a new body in a moment. In
    // Versus the body lies until shortly before the new one comes.
    // Before the slot check: a death may take the body out of its slot first.
    if (u->died && Alive(u->entity)) {
        if (MortalNow()) {
            if (!u->diedAt) {
                u->diedAt = Stamp(now);
                InterlockedIncrement(&versusBotDeaths);
                gate::Event("bot %u: died %s; back in %lu ms", u->index + 1, MortalWhy(), static_cast<unsigned long>(versusRespawnMs));
                // the hit stamp comes from the damage hook and may be a few ms newer than this tick
                const DWORD hitAge = !u->lastHitAt ? 0xFFFFFFFFu : static_cast<LONG>(now - u->lastHitAt) > 0 ? now - u->lastHitAt : 0;
                if (VersusMortal()) versus::CreditBotKill(u->index, u->lastHitRef, u->lastHitFlags, hitAge);
                u->lastHitRef = 0;
                u->lastHitAt = 0;
            }
            const DWORD lie = versusRespawnMs > spawnDelayMs ? versusRespawnMs - spawnDelayMs : 0;
            if (now - u->diedAt < lie) return;
        }
        u->died = false;
        InterlockedIncrement(&botDeaths);
        Despawn(u->diedAt ? "it died as a mortal bot; the new one follows" : "it died; a new one follows");
        return;
    }
    // Still the bot's body: the Player class, and still its id in the PlayerManager.
    if (!Alive(u->entity) || !InSlot(u->entity, u->slotId)) { Let("it is no longer the registered Player in its slot"); return; }
    // Fail-safe: a body at zero health whose death state never spoke (no death guard
    // installed, a refused scripted death) would stand there for good. After 8 s it counts as dead.
    if (now - u->zeroCheckAt >= 500) {
        u->zeroCheckAt = now;
        const uint32_t hc = hits::FindHealthComponent(u->entity);
        float hp = 1.0f;
        if (hc && Read(hc + hits::kHealthCurrent, &hp, 4) && std::isfinite(hp) && hp <= 0.0f) {
            if (!u->zeroSince) {
                u->zeroSince = Stamp(now);
            } else if (now - u->zeroSince >= 8000) {
                u->zeroSince = 0;
                u->died = true;
                if (InterlockedIncrement(&zeroHealthBodies) <= 10)
                    gate::Event("bot %u: FAIL-SAFE its body %08X has had no health for 8 s without a death message; treated as dead (mortal=%d)", u->index + 1,
                                u->entity, MortalNow() ? 1 : 0);
                return;
            }
        } else {
            u->zeroSince = 0;
        }
    }
    if (u->index >= BotSeats(now)) {
        InterlockedIncrement(&seatsGiven);
        Despawn("a human partner took its seat");
        return;
    }
    Vec3 self;
    if (!bot_sense::Pos(u->entity, self)) return;
    if (u->guardSpawn && now - u->spawnSampleAt >= 100) {
        u->spawnSampleAt = now;
        if (now - u->spawnedAt > mpmap::kGuardMs || !mpmap::InVisit() || !play::SurvivalMode()) {
            u->guardSpawn = false;
        } else if (std::fabs(self.x) < 2.0f && std::fabs(self.z) < 2.0f &&
                   coop_bot::Flat(u->spawnTarget, Vec3{}) > 8.0f && coop_bot::Flat(self, u->spawnLast) > 8.0f) {
            const bool moved = MoveEntity(u->entity, u->spawnTarget);
            ++u->spawnGuardTries;
            gate::Event("bot %u: spawn guard %u at %lu ms: (%.1f %.1f %.1f) to (%.1f %.1f %.1f), moved=%d",
                        u->index + 1, u->spawnGuardTries, now - u->spawnedAt, self.x, self.y, self.z,
                        u->spawnTarget.x, u->spawnTarget.y, u->spawnTarget.z, moved);
            u->spawnLast = u->spawnTarget;
            if (u->spawnGuardTries >= 3) u->guardSpawn = false;
            return;
        } else {
            u->spawnLast = self;
        }
    }
    u->self = self;
    CheckDrift(self, leaderPos, now);
    // NO_PAIRED again when the game cleared it (some player states clear it on their way out).
    if (now - u->noPairedAt >= 500) {
        u->noPairedAt = now;
        uint32_t flags = 0;
        if (Read(u->entity + kPlayerFlags, &flags, 4) && !(flags & kFlagNoPaired)) {
            flags |= kFlagNoPaired;
            if (WriteBytes(u->entity + kPlayerFlags, &flags, 4) && InterlockedIncrement(&noPairedRestored) <= 10)
                gate::Event("bot %u: its no-grab flag was cleared by the game; set again", u->index + 1);
        }
        uint32_t id = 0;
        int32_t phase = 0;
        if (attackPhaseReady && Read(u->entity + kPlayerId, &id, 4) && id != 0xFFFFFFFFu && Read(u->entity + kAttackPhase, &phase, 4) &&
            phase != static_cast<int32_t>(id % 15u) && SetAttackPhase(u->entity, id) && InterlockedIncrement(&attackPhaseRestored) <= 10)
            gate::Event("bot %u: attack-slot phase was %d, set to %u again", u->index + 1, phase, id % 15u);
    }
    // Without a leader (he is dead) nothing that needs his body runs.
    if (leader) CheckBodies(leader, now);
    if (leader && u->entity && u->state == State::Active) PutOnOwnSuit(leader, now);
    if (leader && u->entity && u->state == State::Active && u->suitDone) UrgentSuit(leader, now);
    if (leader && u->entity) SyncSceneMask(leader);
    if (!u->entity) return;
    if (!u->cal.done) { Calibrate(self, now); if (!u->cal.done) return; u->state = State::Active; }
    if (SceneTick(self, leaderPos, now)) { StopAntic("scene"); u->pad = Pad{}; u->padLive = true; u->steer.Reset(); return; }
    coop_bot::Perception p;
    Sense(leader, self, leaderPos, now, p);
    if (leader && UsablesTick(leader, self, leaderPos, now)) { StopAntic("usable"); u->face = FaceUnit{}; TrackNodes(); TrackCredits(); Status(self, leaderPos, now, p); return; }
    if (leader) MaybeStartVisit(now);
    KeepAmmo(now);
    if (gunsReadyToBuy) { PollGun(now); ReissueGuns(now); ProgressTick(leader, now); }
    if (KioskTick(self, leaderPos, now)) { StopAntic("kiosk"); TrackNodes(); TrackCredits(); Status(self, leaderPos, now, p); return; }
    if (leader) AnticsBefore(leader, self, leaderPos, now, p);
    {
        const Perf timer(perfBrain);
        u->command = u->brain.Tick(p);
    }
    if (u->command.desperate) u->desperateAt = now;
    StoreWait(self, leader);
    KioskKeepOut(self, leader);
    AnticsAfter(self, leaderPos, now);
    if (u->command.heal && ownSuits) {
        uint32_t worn[4]{};
        if (!WornSuit(u->entity, worn)) { u->command.heal = false; InterlockedIncrement(&healsHeldBack); }   // no body yet: the heal would crash
    }
    NoteLoot(p, now);
    if (u->command.rescue) {
        InterlockedIncrement(&rescueFrames);
        const coop_bot::Command& c = u->command;
        if (c.fire || c.holdFire || c.melee || c.altFire || c.stasis) InterlockedIncrement(&rescueAttacks);
        if (!u->rescuing && InterlockedIncrement(&rescueLines) <= 60)
            gate::Event("bot %u: goes for the leader's grabber %08X, %.1f m off", u->index + 1, c.target, coop_bot::Distance(self, leaderPos));
    }
    u->rescuing = u->command.rescue;
    if (u->command.laneBlocked) InterlockedIncrement(&laneHolds);
    if (u->command.regenHold) InterlockedIncrement(&regenHolds);
    if (u->command.fireHeld) InterlockedIncrement(&fireHeldFrames);
    if (u->command.returnFire) InterlockedIncrement(&returnFireFrames);
    if (u->command.focused) InterlockedIncrement(&focusFrames);
    if (u->goingThere && coop_bot::Flat(self, u->holdSpot) <= u->brain.tune.holdArriveStart) {
        u->goingThere = false;
        InterlockedIncrement(&holdArrivals);
        if (OrderLog())
            gate::Event("bot %u: at its spot (%.1f %.1f %.1f) after %lu ms, holds there", u->index + 1, u->holdSpot.x, u->holdSpot.y, u->holdSpot.z,
                        static_cast<unsigned long>(now - u->holdSince));
    }
    if (u->command.shotAtLimb) InterlockedIncrement(&limbShots);
    if (u->command.shotAtChest) InterlockedIncrement(&chestShots);
    if (u->command.move && u->command.mode == coop_bot::Mode::Combat)
        InterlockedIncrement(&combatMoves[static_cast<unsigned>(u->command.why) % static_cast<unsigned>(coop_bot::MoveWhy::Count)]);
    {
        const Perf timer(perfKinesis);
        KinesisTick(self, leaderPos, p, now);
    }
    TrackNodes();
    TrackCredits();
    FaceTick(self, leaderPos, p, now);
    {
        const Perf timer(perfAct);
        Act(self, leaderPos, now);
    }
    FaceAfterAct(now);
    CatchUpTick(self, leaderPos, now);
    Status(self, leaderPos, now, p);
}

// Grabbed by a necro player. PairedAttackCoordinator::Update (PC 0x010C36E0, thiscall,
// ret) counts the defender's shakeoff presses only when PlayerManager slot 0 defends:
// flag 4 at +0x68 and +0xA4 = slot 0's pad, both set by SetSentients (0x010AA040). So a
// bot never broke free (13.10.: held at 522.8, dead at 534.3). For the Update call alone
// the bot stands in as the local defender. Flag 4 stays off otherwise: the coordinator's
// machine pushes the grab camera and the DoF VFX for the local defender, which would be
// the host's screen.
constexpr uint32_t kPacUpdateRva = 0xCC36E0;
constexpr uint32_t kPacNet = 0x64;   // the reflectable at +0x58, its NetObject at +0x0C
constexpr uint32_t kPacFlags = 0x68, kPacStage = 0x6C, kPacDefender = 0x90, kPacPad = 0xA4, kPacMeter = 0xA8, kPacDecay = 0xAC, kPacThreshold = 0xB0,
                   kPacStageAt = 0xC0;
constexpr uint8_t kPacLocalRoles = 6, kPacLocalAttacker = 2, kPacLocalDefender = 4, kPacBreak = 8;
constexpr int32_t kPacStruggle = 3;
constexpr uint32_t kInputManagerRva = 0x1C1C0AC, kSimNowRva = 0x1C4D3C0, kSimDtRva = 0x1C4D3E4, kShakeoffHash = 0x9F3F7949u;
safetyhook::InlineHook pacUpdateHook;
bool grabEscape = true, pacHooked = false;   // [Bot] NecroGrabEscape
volatile LONG pacStruggles = 0, pacUpdates = 0, pacPresses = 0, pacBreaks = 0, pacGhostSkips = 0, pacFaults = 0, pacOwnCounts = 0;
struct PacTrack {
    uint32_t pac = 0, pressAt = 0;
    long presses = 0;
    float meterPeak = 0.0f, threshold = 0.0f;
    bool broke = false, pending = false, hostNecro = false;
    DWORD seenAt = 0, lastAt = 0;
};
PacTrack pacTracks[kMaxBots];
// The coordinator runs on the victim's machine; a ghost copy only mirrors it (0x007E1130).
bool PacAuthoritative(uint32_t pac) {
    uint32_t net = 0, netFlags = 0;
    uint8_t remote = 0;
    if (!Read(pac + kPacNet, &net, 4)) return false;
    if (!net) return true;
    return Read(net + 0x30, &netFlags, 4) && Read(net + 0x3C, &remote, 1) && !(netFlags & 2) && !(remote & 1);
}
int PacBotDefender(uint32_t pac) {
    int32_t stage = 0;
    uint8_t flags = 0;
    uint32_t defender = 0;
    if (!Read(pac + kPacStage, &stage, 4) || stage != kPacStruggle || !Read(pac + kPacFlags, &flags, 1) || (flags & kPacLocalDefender) ||
        !Read(pac + kPacDefender, &defender, 4) || defender <= 0x10)
        return -1;
    for (unsigned i = 0; i < kMaxBots; ++i)
        if (units[i].entity && units[i].entity == defender - 0x10) return static_cast<int>(i);
    return -1;
}
__declspec(noinline) int ShakeoffPressed(uint32_t input, uint32_t pad) {
    __try {
        uint32_t vtable = 0, fn = 0;
        if (!Read(input, &vtable, 4) || !vtable || !Read(vtable + 0x14, &fn, 4) || !fn) return -1;
        return reinterpret_cast<bool(__thiscall*)(void*, uint32_t, uint32_t)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(input)), pad, kShakeoffHash) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// The host's own necro holds the bot: slot 0 mashes as the attacker in Update, so the
// bot's presses are counted here, the way Update's defender branch does (0x010C3825).
void CountDefenderPress(uint32_t pac, PacTrack& t, uint32_t pad) {
    uint32_t input = 0, simNow = 0, stageAt = 0;
    float dt = 0.0f, meter = 0.0f, decay = 0.0f, threshold = 0.0f;
    if (!Read(base + kInputManagerRva, &input, 4) || !input || !Read(base + kSimNowRva, &simNow, 4) || !Read(base + kSimDtRva, &dt, 4) ||
        !Read(pac + kPacMeter, &meter, 4) || !Read(pac + kPacDecay, &decay, 4) || !Read(pac + kPacThreshold, &threshold, 4) || !Read(pac + kPacStageAt, &stageAt, 4)) {
        InterlockedIncrement(&pacFaults);
        return;
    }
    const int pressed = ShakeoffPressed(input, pad);
    if (pressed < 0) { InterlockedIncrement(&pacFaults); return; }
    if (pressed) t.pending = true;
    float value = 0.0f;
    if (t.pending && simNow - t.pressAt >= 66) {
        t.pending = false;
        t.pressAt = simNow;
        value = dt > 0.03333f ? dt * 30.0f : 1.0f;
    }
    meter = meter + value - decay * dt;
    if (meter < 0.0f) meter = 0.0f;
    WriteBytes(pac + kPacMeter, &meter, 4);
    InterlockedIncrement(&pacOwnCounts);
    uint8_t flags = 0;
    if (threshold >= 0.0f && meter >= threshold && simNow - stageAt >= 500 && Read(pac + kPacFlags, &flags, 1) && !(flags & kPacBreak)) {
        flags |= kPacBreak;
        WriteBytes(pac + kPacFlags, &flags, 1);
    }
}
void __fastcall OnPacUpdate(void* self, void*) {
    const uint32_t pac = reinterpret_cast<uint32_t>(self);
    const int bot = grabEscape ? PacBotDefender(pac) : -1;
    if (bot < 0) { pacUpdateHook.thiscall<void>(self); return; }
    if (!PacAuthoritative(pac)) { InterlockedIncrement(&pacGhostSkips); pacUpdateHook.thiscall<void>(self); return; }
    uint8_t flags = 0;
    uint32_t pad = 0;
    float before = 0.0f, after = 0.0f;
    if (!Read(pac + kPacFlags, &flags, 1) || !Read(pac + kPacPad, &pad, 4) || !Read(pac + kPacMeter, &before, 4)) {
        InterlockedIncrement(&pacFaults);
        pacUpdateHook.thiscall<void>(self);
        return;
    }
    const uint32_t botPad = units[bot].controller;
    PacTrack& t = pacTracks[bot];
    const bool hostNecro = (flags & kPacLocalAttacker) != 0;
    const DWORD tick = GetTickCount();
    if (t.pac != pac) {
        t = PacTrack{};
        t.pac = pac;
        t.seenAt = tick;
        t.hostNecro = hostNecro;
        float decay = 0.0f;
        Read(pac + kPacThreshold, &t.threshold, 4);
        Read(pac + kPacDecay, &decay, 4);
        InterlockedIncrement(&pacStruggles);
        gate::Event("bot %u: grabbed by %s necro (coordinator %08X, break at %.1f, decay %.2f/s): %s", bot + 1, hostNecro ? "the host's" : "a partner's", pac,
                    t.threshold, decay, hostNecro ? "its presses counted by the plugin" : "its pad stands in for slot 0's");
    }
    t.lastAt = tick;
    if (hostNecro) {
        pacUpdateHook.thiscall<void>(self);
        Read(pac + kPacMeter, &before, 4);
        CountDefenderPress(pac, t, botPad);
    } else {
        const uint8_t standIn = static_cast<uint8_t>(flags | kPacLocalDefender);
        WriteBytes(pac + kPacFlags, &standIn, 1);
        WriteBytes(pac + kPacPad, &botPad, 4);
        pacUpdateHook.thiscall<void>(self);
        // Update may set the break (8) and the pending press (0x40); only the role goes back.
        uint8_t now8 = 0;
        if (Read(pac + kPacFlags, &now8, 1)) {
            const uint8_t back = static_cast<uint8_t>((now8 & ~kPacLocalRoles) | (flags & kPacLocalRoles));
            WriteBytes(pac + kPacFlags, &back, 1);
        }
        WriteBytes(pac + kPacPad, &pad, 4);
    }
    uint8_t now8 = 0;
    Read(pac + kPacFlags, &now8, 1);
    InterlockedIncrement(&pacUpdates);
    Read(pac + kPacMeter, &after, 4);
    if (after > before) { ++t.presses; InterlockedIncrement(&pacPresses); }
    if (after > t.meterPeak) t.meterPeak = after;
    if ((now8 & kPacBreak) && !t.broke) {
        t.broke = true;
        InterlockedIncrement(&pacBreaks);
        gate::Event("bot %u: breaks free after %.1f s (%ld presses, meter %.1f of %.1f)", bot + 1, (tick - t.seenAt) / 1000.0f, t.presses, after, t.threshold);
    }
}
void PacTick(DWORD now) {
    for (unsigned i = 0; i < kMaxBots; ++i) {
        PacTrack& t = pacTracks[i];
        if (!t.pac || now - t.lastAt < 1000) continue;
        gate::Event("bot %u: struggle over after %.1f s (%s necro): %ld presses counted, meter peak %.1f of %.1f, %s", i + 1, (t.lastAt - t.seenAt) / 1000.0f,
                    t.hostNecro ? "host's" : "partner's", t.presses, t.meterPeak, t.threshold, t.broke ? "broke free" : "no break (dead, or the necro let go)");
        t = PacTrack{};
    }
}

// Game thread, every frame (from coop::Poll).
void NewGameCheck();   // below, before Install
void TickInner();
volatile LONG perfFrames = 0;
LONG perfReadsAt = 0, perfQueriesAt = 0, perfFaultsAt = 0;
LONGLONG perfQueryTicksAt = 0;
DWORD perfLogAt = 0;
void Tick() {
    NoteMortal();   // also on a machine without bots: its bot ghosts follow the answer
    if (passThrough && network && !IsHost() && inLevel && levelSettled && !LevelChangePending()) {
        const DWORD now = GetTickCount();
        if (now - apartAt >= 500) {
            const uint32_t local = LocalSlotZeroEntity();
            if (local) KeepPlayersApart(local, now, true);
        }
    }
    if (!enabled) return;
    {
        const Perf timer(perfTick);
        TickInner();
    }
    ++perfFrames;
    const DWORD now = GetTickCount();
    if (!perfLogAt) { perfLogAt = now; perfReadsAt = readCalls; perfQueriesAt = readQueries; perfFaultsAt = readFaults; perfQueryTicksAt = readQueryTicks; if (!perfFreq) { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); perfFreq = f.QuadPart; } }
    if (now - perfLogAt >= 10000 && perfFrames > 0 && perfFreq) {
        const double ms = 1000.0 / static_cast<double>(perfFreq);
        auto avg = [&](const PerfSlot& s) { return s.calls ? static_cast<double>(s.ticks) * ms / static_cast<double>(perfFrames) : 0.0; };
        auto top = [&](const PerfSlot& s) { return static_cast<double>(s.maxTicks) * ms; };
        // Two lines: the event ring cuts a line at 255 characters (one line was cut at 191 before).
        gate::Event("bot_perf %ld frames in %lu ms: tick %.2f ms a frame (max %.2f), walks %.2f (max %.2f), sights %.2f (max %.2f), refresh %.2f, "
                    "lifts %.2f (max %.2f), throwables %.2f (max %.2f)",
                    perfFrames, static_cast<unsigned long>(now - perfLogAt), avg(perfTick), top(perfTick), avg(perfScan), top(perfScan), avg(perfSight),
                    top(perfSight), avg(perfRefresh), avg(perfCabins), top(perfCabins), avg(perfThrowables), top(perfThrowables));
        const LONGLONG queryTicks = readQueryTicks;
        gate::Event("bot_perf2 paths %.2f (max %.2f), brain %.2f (max %.2f), kinesis %.2f (max %.2f), act %.2f (max %.2f), leader %.2f; %ld reads a frame, "
                    "%ld region queries %.1f ms, %ld read faults",
                    avg(perfNav), top(perfNav), avg(perfBrain), top(perfBrain), avg(perfKinesis), top(perfKinesis), avg(perfAct), top(perfAct), avg(perfLeader),
                    (readCalls - perfReadsAt) / perfFrames, readQueries - perfQueriesAt, static_cast<double>(queryTicks - perfQueryTicksAt) * ms,
                    readFaults - perfFaultsAt);
        perfQueryTicksAt = queryTicks;
        perfQueriesAt = readQueries;
        perfFaultsAt = readFaults;
        perfTick = PerfSlot{}; perfScan = PerfSlot{}; perfSight = PerfSlot{}; perfRefresh = PerfSlot{}; perfCabins = PerfSlot{}; perfThrowables = PerfSlot{};
        perfNav = PerfSlot{}; perfBrain = PerfSlot{}; perfKinesis = PerfSlot{}; perfAct = PerfSlot{}; perfLeader = PerfSlot{};
        perfFrames = 0;
        perfReadsAt = readCalls;
        perfLogAt = now;
    }
}
// Once per level change, checkpoint restart or main menu (the hold below covers
// all three): what points at the old level goes. A store room the leader stood in
// made RoomTick see room.in with dead kiosk pointers in the next level, and
// FinishRoom then let every bot shop and invest at once; kinesis could take a body
// of the old level from the last throwable list.
bool levelForgotten = false;
volatile LONG levelResets = 0;
// The last spot of the leader, for the time he is dead ([Bot] Leaderless).
bool leaderless = true;
Vec3 lastLeaderPos{};
DWORD lastLeaderAt = 0, leaderlessSince = 0;
volatile LONG leaderlessEpisodes = 0, leaderlessFrames = 0;
void ForgetLevel(const char* why) {
    if (levelForgotten) return;
    levelForgotten = true;
    InterlockedIncrement(&levelResets);
    const bool inRoom = room.in;
    const uint32_t call = kioskCall.kiosk;
    room = StoreRoom{};
    kioskCall = KioskCall{};
    tkWalk = ThrowableWalk{};
    throwableCount = 0;
    throwablesAt = 0;
    for (Shunned& s : shunned) s = Shunned{};
    bot_sense::ForgetLevel();
    bot_sense::ReenableLineOfSight(why);
    gate::Event("bot: %s: store room%s, kiosk call %08X, throwables and the play-dead memory reset", why, inRoom ? " (the leader was in one)" : "", call);
}
void TickInner() {
    const DWORD now = GetTickCount();
    ++botFrame;
    bot_aim::Tick(now);
    PacTick(now);
    // The toggle key: the bots leave, and come back on the next press.
    const bool down = toggleKey && necro::GameWindowFocused() && !shop::HoldsKey(toggleKey, coop_shop::Guard::BotToggle) && (GetAsyncKeyState(toggleKey) & 0x8000) != 0;
    const bool pressed = down && !keyDown;
    keyDown = down;
    if (pressed && inLevel) {
        userOff = !userOff;
        gate::Event("bot: %s by the key", userOff ? "sent away" : "called back");
        if (userOff) ResetOrders("the bots were sent away");
        for (unsigned i = 0; i < wantedBots; ++i) {
            u = &units[i];
            if (userOff && u->entity) Despawn("the key");
            if (!userOff && u->failedForGood) {
                u->failedForGood = false;
                gate::Event("bot %u: called back after it left for good", i + 1);
            }
            if (!userOff && (u->state == State::Off || u->state == State::Failed)) u->state = State::Waiting;
        }
    }
    const bool levelStands = inLevel && levelSettled && !LevelChangePending();
    const bool standing = !HoldTick(levelStands, now) && levelStands;
    if (!standing) {
        for (unsigned i = 0; i < wantedBots; ++i) {
            u = &units[i];
            if (u->entity) Let(inLevel ? "the level is changing" : "the level is gone");
            u->settledSince = 0;
            u->diedAt = 0;
            // A new level, a new try for a bot that left for good.
            if (u->failedForGood) {
                u->failedForGood = false;
                if (u->state == State::Off && enabled && !userOff) u->state = State::Waiting;
                gate::Event("bot %u: may come back in the next level", i + 1);
            }
            // No viewport room last time: the next level tries again.
            if (u->state == State::Failed && enabled && !userOff) u->state = State::Waiting;
        }
        ResetOrders(inLevel ? "the level is changing" : "the level is gone");
        trail.Clear();
        leaderCreditsSeen = -1;   // a level load or restart sets the leader's credits anew: no find
        ResetLoot();
        leaderGrabber = 0;
        rescueSince = 0;
        cabinMoversAt = 0;        // the next level's lifts are walked anew
        cabinWalking = false;
        cabinMoverCount = 0;
        ForgetLevel(holdActive ? holdWhy : (inLevel ? "the level is changing" : "the level is gone"));
        lastLeaderAt = 0;
        leaderlessSince = 0;
        return;
    }
    levelForgotten = false;
    ReleaseViewport();
    // The leader left a bench a moment ago: the bots spend their nodes.
    if (benchClosedAt && static_cast<LONG>(now - benchClosedAt) >= 200) {   // stamped in a hook, maybe after `now`
        benchClosedAt = 0;
        InterlockedIncrement(&benchVisits);
        const uint32_t visitor = Leader();
        for (unsigned i = 0; i < wantedBots; ++i) {
            u = &units[i];
            if (u->entity && u->state == State::Active) { Invest(visitor); NoteKioskTurn(0, false); }
        }
        u = &units[0];
    }
    // The leader left a store a moment ago: the bots shop.
    if (storeClosedAt && static_cast<LONG>(now - storeClosedAt) >= 200) {
        storeClosedAt = 0;
        InterlockedIncrement(&storeVisits);
        const uint32_t visitor = Leader();
        for (unsigned i = 0; i < wantedBots; ++i) {
            u = &units[i];
            if (u->entity && u->state == State::Active) { Shop(visitor); NoteKioskTurn(0, true); }
        }
        u = &units[0];
    }
    uint32_t leader = Leader();
    Vec3 leaderPos;
    if (leader && bot_sense::Pos(leader, leaderPos)) {
        lastLeaderPos = leaderPos;
        lastLeaderAt = now;
        if (leaderlessSince) {
            gate::Event("bot: the leader %08X is back after %lu ms without one", leader, static_cast<unsigned long>(now - leaderlessSince));
            leaderlessSince = 0;
        }
    } else {
        // The leader is dead (a Versus or survival respawn) or between bodies: the bots
        // stay around his last spot and go on fighting (12.10.: they froze until he was
        // back). Nothing that needs his body runs meanwhile, and no bot spawns.
        bool any = false;
        for (unsigned i = 0; i < wantedBots; ++i) any = any || (units[i].entity && units[i].state == State::Active);
        if (!leaderless || !lastLeaderAt || !any) return;
        leader = 0;
        leaderPos = lastLeaderPos;
        if (!leaderlessSince) {
            leaderlessSince = Stamp(now);
            InterlockedIncrement(&leaderlessEpisodes);
            gate::Event("bot: no leader (slot 0 holds %08X); the bots go on around (%.1f %.1f %.1f)", SlotOccupant(0), leaderPos.x, leaderPos.y, leaderPos.z);
        }
        InterlockedIncrement(&leaderlessFrames);
        leaderGrabbed = false;
        leaderGrabber = 0;
        leaderHealth01 = 1.0f;
        leaderChase = false;
    }
    if (leader) {
        trail.Note(leaderPos, now);
        StepCabinWalk(now);
    }
    if (leader) {
        const Perf timer(perfLeader);
        NewGameCheck();
        const Machines leaderMachines = MachinesOf(leader);
        const int32_t lower = leaderMachines.lower;
        leaderGrabbed = lower == 27 || lower == 28 || lower == 29 || lower == 33 || lower == 40;   // as the bots' own (Sense)
        leaderGrabber = rescueLeader ? FindGrabber(leader, leaderPos, lower, now) : 0;
        leaderHealth01 = Health01(leader);
        UpdateLeaderBusy(leaderMachines.lower, now);
        AltFireTick(leader, leaderMachines.lower, now);
        UpdateLeaderChase(leader);
        UpdateLeaderFlight(leader, leaderPos, leaderMachines, now);
        if (!network || IsHost()) KeepPlayersApart(leader, now);
        ShareAir(leader, now);
        ModulesTick(leader, now);
        RoomTick(leader, leaderPos, now);
        UsablesScan(leader, leaderPos, now);
        if (storeReady) ShareCredits(leader);
    }
    StepLootWalk(now, leaderPos);
    // Moving: more than 1.5 m in the last two seconds, from a sample every quarter second.
    if (now - leaderHistoryAt[(leaderHistoryNext + 7) % 8] >= 250) {
        leaderHistory[leaderHistoryNext % 8] = leaderPos;
        leaderHistoryAt[leaderHistoryNext % 8] = now;
        ++leaderHistoryNext;
    }
    // The call key: the bots come to the player at once (the catch-up the steering does
    // when it is stuck for good, on demand). Not during a scene.
    const bool callHeld = callKey && necro::GameWindowFocused() && !shop::HoldsKey(callKey, coop_shop::Guard::BotCall) && (GetAsyncKeyState(callKey) & 0x8000) != 0;
    // The human's fire (left mouse button, which also swings): the bots come over to his fight.
    if (necro::GameWindowFocused() && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) leaderShotAt = now;
    const bool callPressed = callHeld && !callDown;
    callDown = callHeld;
    if (callPressed && stance != Stance::Follow && !cutscene::SequenceRunningNow()) {
        SetFollow("the call key brings them over");
        notice::Show("BOTS: FOLLOW");
    }
    const bool markHeld = markKey && necro::GameWindowFocused() && !shop::HoldsKey(markKey, coop_shop::Guard::BotMark) && (GetAsyncKeyState(markKey) & 0x8000) != 0;
    if (markHeld && !markDown) {
        const LONG n = InterlockedIncrement(&marks);
        gate::Event("bot MARK %ld: the player marked this moment", n);
        for (unsigned i = 0; i < wantedBots; ++i) {
            const Unit& m = units[i];
            gate::Event("bot MARK %ld: bot %u entity %08X kiosk %08X kinesis %s, antic %d, holding %d, flying %d",
                        n, i + 1, m.entity, m.kiosk, m.tkHeldSince ? "carrying" : "idle",
                        static_cast<int>(m.brain.AnticNow()), m.holding ? 1 : 0, m.flying ? 1 : 0);
            units[i].lastStatus = 0;   // the full status lines follow this tick
        }
        notice::Show("MARKED");
    }
    markDown = markHeld;
    CheckFocus(now);
    PollOrders(leader, leaderPos, now);
    ProgressFrame(now);
    for (unsigned i = 0; i < wantedBots; ++i) {
        u = &units[i];
        TickUnit(leader, leaderPos, now, callPressed);
    }
    u = &units[0];
    {
        uint32_t bots[kMaxBots]{};
        bool any = false;
        for (unsigned i = 0; i < kMaxBots; ++i) { bots[i] = units[i].entity; any = any || bots[i]; }
        if (any && leader) SlotsReport(leader, bots, kMaxBots, now);
    }
}

// --- personalities --------------------------------------------------------------------
// DS2BotProfiles.ini next to DS2United.ini: one section per bot ([Bot1] .. [Bot3])
// with its name, reaction, fighting distance and courage, aim, pace, places and
// the rest. A missing file or key falls back to the built-in profile of that bot.
struct Profile {
    char name[32] = "";
    char suit[64] = "";        // one favourite suit (the older key)
    char suits[192] = "";      // FavoriteSuits, best first
    char forceSuit[64] = "";   // ForceSuit: this suit for good, empty = pick as usual
    char weapons[96] = "";     // favourite guns, for the weapon economy
    uint32_t reactMin = 250, reactMax = 650, postMin = 1000, postMax = 3000, acquire = 250;
    float courage = 0.5f, distance = 6.0f, aimSpeed = 1.0f, aimSway = 0.06f, walkPace = 0.9f;
    float aimSkill = 0.85f;   // AimSkill: 0 sloppy .. 1 sharp, scales the human aim's errors
    int healBelow = -1, kinesisChance = 50;
    float followAngle = 90.0f, followDistance = 2.5f, watchAngle = 0.0f, combatSide = 1.0f, combatSideMeters = 2.2f, combatBack = 1.0f;
    int antics = 0;               // Antics: 0 never .. 3 often
    bool harmfulAntics = false;   // HarmfulAntics: the stasis prank too
};
Profile BuiltInProfile(unsigned i) {
    Profile p;
    if (i == 0) {
        std::snprintf(p.name, sizeof(p.name), "Vasquez");
        p.reactMin = 180; p.reactMax = 450; p.postMin = 1000; p.postMax = 2000; p.acquire = 180;
        p.courage = 0.8f; p.distance = 4.5f; p.aimSpeed = 1.15f; p.aimSway = 0.06f; p.walkPace = 1.0f; p.healBelow = 45; p.kinesisChance = 60;
        p.followAngle = 60.0f; p.followDistance = 2.4f; p.watchAngle = 35.0f; p.combatSide = 1.0f; p.combatSideMeters = 2.0f; p.combatBack = 0.3f;
        p.aimSkill = 0.85f;
    } else if (i == 1) {
        std::snprintf(p.name, sizeof(p.name), "Novak");
        p.reactMin = 300; p.reactMax = 650; p.postMin = 1500; p.postMax = 3000; p.acquire = 320;
        p.courage = 0.5f; p.distance = 7.0f; p.aimSpeed = 0.9f; p.aimSway = 0.04f; p.walkPace = 0.9f; p.healBelow = 60; p.kinesisChance = 35;
        p.followAngle = -70.0f; p.followDistance = 2.8f; p.watchAngle = -40.0f; p.combatSide = -1.0f; p.combatSideMeters = 2.6f; p.combatBack = 0.8f;
        p.aimSkill = 0.92f;
    } else {
        std::snprintf(p.name, sizeof(p.name), "Harlan");
        p.reactMin = 350; p.reactMax = 800; p.postMin = 2000; p.postMax = 3000; p.acquire = 280;
        p.courage = 0.3f; p.distance = 6.0f; p.aimSpeed = 0.95f; p.aimSway = 0.09f; p.walkPace = 0.85f; p.healBelow = 70; p.kinesisChance = 50;
        p.followAngle = -125.0f; p.followDistance = 3.2f; p.watchAngle = 180.0f; p.combatSide = 1.0f; p.combatSideMeters = 3.4f; p.combatBack = 1.2f;
        p.aimSkill = 0.78f;
    }
    return p;
}
Profile LoadProfile(const std::string& file, unsigned i) {
    Profile p = BuiltInProfile(i);
    char section[8];
    std::snprintf(section, sizeof(section), "Bot%u", i + 1);
    const char* f = file.c_str();
    auto text = [&](const char* key, char* out, size_t size) {
        char v[128]{};
        GetPrivateProfileStringA(section, key, "\x01", v, sizeof(v), f);
        if (v[0] != '\x01') std::snprintf(out, size, "%s", v);
    };
    auto number = [&](const char* key, float fallback, float lo, float hi) {
        char v[32]{};
        GetPrivateProfileStringA(section, key, "", v, sizeof(v), f);
        if (!v[0]) return fallback;
        const float x = static_cast<float>(std::atof(v));
        return std::isfinite(x) ? (x < lo ? lo : (x > hi ? hi : x)) : fallback;
    };
    auto ms = [&](const char* key, uint32_t fallback) { return static_cast<uint32_t>(number(key, static_cast<float>(fallback), 0.0f, 10000.0f)); };
    text("Name", p.name, sizeof(p.name));
    text("Suit", p.suit, sizeof(p.suit));
    text("FavoriteSuits", p.suits, sizeof(p.suits));
    text("ForceSuit", p.forceSuit, sizeof(p.forceSuit));
    if (!p.suits[0] && p.suit[0]) std::snprintf(p.suits, sizeof(p.suits), "%s", p.suit);
    text("FavoriteWeapons", p.weapons, sizeof(p.weapons));
    p.reactMin = ms("ReactionMinMs", p.reactMin);
    p.reactMax = ms("ReactionMaxMs", p.reactMax);
    if (p.reactMax < p.reactMin) p.reactMax = p.reactMin;
    p.postMin = ms("PostCombatMinMs", p.postMin);
    p.postMax = ms("PostCombatMaxMs", p.postMax);
    if (p.postMax < p.postMin) p.postMax = p.postMin;
    p.acquire = ms("AimAcquireMs", p.acquire);
    p.courage = number("Courage", p.courage, 0.0f, 1.0f);
    p.distance = number("PreferredDistance", p.distance, 3.0f, 15.0f);
    p.aimSpeed = number("AimSpeed", p.aimSpeed, 0.4f, 2.0f);
    p.aimSway = number("AimSway", p.aimSway, 0.0f, 0.5f);
    p.aimSkill = number("AimSkill", p.aimSkill, 0.0f, 1.0f);
    p.walkPace = number("WalkPace", p.walkPace, 0.5f, 1.0f);
    p.healBelow = static_cast<int>(number("HealBelowPercent", static_cast<float>(p.healBelow), -1.0f, 95.0f));
    p.kinesisChance = static_cast<int>(number("KinesisChance", static_cast<float>(p.kinesisChance), 0.0f, 100.0f));
    p.followAngle = number("FollowAngle", p.followAngle, -180.0f, 180.0f);
    p.followDistance = number("FollowDistance", p.followDistance, 1.5f, 6.0f);
    p.watchAngle = number("WatchAngle", p.watchAngle, -180.0f, 180.0f);
    p.combatSide = number("CombatSide", p.combatSide, -1.0f, 1.0f) < 0.0f ? -1.0f : 1.0f;
    p.combatSideMeters = number("CombatSideMeters", p.combatSideMeters, 1.0f, 5.0f);
    p.combatBack = number("CombatBack", p.combatBack, -1.0f, 4.0f);
    p.antics = static_cast<int>(number("Antics", static_cast<float>(p.antics), 0.0f, 3.0f));
    p.harmfulAntics = number("HarmfulAntics", p.harmfulAntics ? 1.0f : 0.0f, 0.0f, 1.0f) >= 0.5f;
    return p;
}
void ApplyProfile(Unit& unit, const Profile& p, float globalHeal) {
    coop_bot::Tuning& t = unit.brain.tune;
    t.reactMinMs = p.reactMin;
    t.reactMaxMs = p.reactMax;
    t.postCombatMinMs = p.postMin;
    t.postCombatMaxMs = p.postMax;
    t.aimAcquireMs = p.acquire;
    t.aimSway = p.aimSway;
    t.aimSkill = p.aimSkill;
    // Where it likes to fight: the far edge is its preferred distance, the near
    // edge (where it backs off) comes closer the braver it is.
    t.keepMax = p.distance;
    float keepMin = p.distance * (0.65f - 0.3f * p.courage);
    keepMin = keepMin < 1.8f ? 1.8f : (keepMin > p.distance - 1.0f ? p.distance - 1.0f : keepMin);
    t.keepMin = keepMin;
    t.healBelow = p.healBelow >= 10 ? static_cast<float>(p.healBelow) / 100.0f : globalHeal;
    t.ownPlaces = true;
    t.followAngleDeg = p.followAngle;
    t.followDistance = p.followDistance;
    t.watchAngleDeg = p.watchAngle;
    t.combatSideSign = p.combatSide;
    t.combatSide = p.combatSideMeters;
    t.combatBackBy = p.combatBack;
    unit.aimSpeed = p.aimSpeed;
    unit.walkPace = p.walkPace;
    unit.kinesisChance = p.kinesisChance;
    t.antics = anticsEnabled ? p.antics : 0;
    t.harmfulAntics = p.harmfulAntics;
    std::snprintf(unit.name, sizeof(unit.name), "%s", p.name[0] ? p.name : "bot");
    std::snprintf(unit.favoriteSuit, sizeof(unit.favoriteSuit), "%s", p.suits);
    unit.suitFavoriteCount = 0;
    unit.suitForced = p.forceSuit[0] ? SuitByName(p.forceSuit) : -1;
    if (p.forceSuit[0] && unit.suitForced < 0)
        gate::Event("bot %u (%s): ForceSuit '%s' is no suit name; it picks as usual", unit.index + 1, unit.name, p.forceSuit);
    else if (unit.suitForced >= 0)
        gate::Event("bot %u (%s): ForceSuit %s, worn for good", unit.index + 1, unit.name, kSuits[unit.suitForced].name);
    {
        char suitList[192];
        std::snprintf(suitList, sizeof(suitList), "%s", p.suits);
        char* rest = nullptr;
        for (char* token = strtok_s(suitList, ",", &rest); token && unit.suitFavoriteCount < 8; token = strtok_s(nullptr, ",", &rest)) {
            const int idx = SuitByName(token);
            bool twice = false;
            for (int k = 0; k < unit.suitFavoriteCount; ++k) twice = twice || unit.suitFavorites[k] == idx;
            if (idx >= 0 && !twice) unit.suitFavorites[unit.suitFavoriteCount++] = idx;
        }
    }
    unit.favoriteCount = 0;
    char list[96];
    std::snprintf(list, sizeof(list), "%s", p.weapons);
    char* rest = nullptr;
    for (char* token = strtok_s(list, ", ", &rest); token && unit.favoriteCount < 10; token = strtok_s(nullptr, ", ", &rest)) {
        const int idx = GunIndexByName(token);
        bool twice = false;
        for (int k = 0; k < unit.favoriteCount; ++k) twice = twice || unit.favorites[k] == idx;
        if (idx >= 0 && !twice) unit.favorites[unit.favoriteCount++] = idx;
    }
}

// The leader's chapter went back (a new game from the menu, or a much older
// save): what the bots earned and bought belongs to the other game. Credits,
// power nodes, bought guns and the picked suit start over; a new game starts
// the bots with empty hands like the player (their bodies copy his inventory).
int lastChapter = 0;
volatile LONG progressResets = 0;
void NewGameCheck() {
    const int chapter = LeaderChapter();
    if (chapter <= 0) return;
    if (lastChapter && chapter < lastChapter) {
        InterlockedIncrement(&progressResets);
        gate::Event("bot: the chapter went back from %d to %d (a new game or an older save): the bots' credits, nodes, guns and suits start over", lastChapter,
                    chapter);
        Unit* const saved = u;
        for (Unit& unit : units) {
            u = &unit;
            unit.creditWallet = 0;
            unit.creditsSet = false;
            unit.creditsSeen = -1;
            unit.walletFresh = true;
            unit.nodeWallet = 0;
            unit.nodesSet = false;
            unit.nodesSeen = -1;
            unit.gunsOwned = 0;
            unit.gunPending = -1;
            unit.gunProxy = 0;
            unit.gunPrice = 0;
            unit.suitChosen = -1;
        }
        u = saved;
        leaderCreditsSeen = -1;
    }
    lastChapter = chapter;
}

// --- the bots' state per campaign save ---------------------------------------------
// The game keeps its restart state in save points (Checkpoint statics, PC):
// [0x0208FC04] is stored by SetDeathRestart (SavePoint::Store 0x015F0440 called
// from 0x00BA20AA), copied into every save file (AllocAndFillSavedGame 0x00B75980)
// and rebuilt from a loaded one (RestoreFromSavedGame 0x00B75A00). [0x0208FC08] is
// stored by StoreLevel 0x00B75880 at a level change. RestoreSave 0x00B75900, at
// every level start, restores the first when a death restart GUID is set
// (0x0208FCC4), else the second. The bots do the same: a snapshot at each store,
// applied when the game restores that very store, and the death restart snapshot
// written beside each save file. A save file is known by the hash the game keeps in
// its header (SaveManager [0x0201C0E8] +0x2C, SDBM over header and body) and its
// body size (+0x94): ChooseSaveSlot 0x00B99FE0 fills them (slot 0 is also the
// autosave), PostLoadFinished 0x00B8BF60 still holds them from the loaded file.
constexpr uint32_t kSaveManagerRva = 0x1C1C0E8, kDeathPointRva = 0x1C8FC04, kLevelPointRva = 0x1C8FC08, kDeathRestartIdRva = 0x1C8FCC4;
constexpr uint32_t kPointStoreRva = 0x11F0440, kRestoreSaveRva = 0x775900, kChooseSaveSlotRva = 0x799FE0, kPostLoadFinishedRva = 0x78BF60;
constexpr uint32_t kSmState = 0xC, kSmHash = 0x2C, kSmMap = 0x30, kSmBodyUsed = 0x94, kSmSaveSlot = 0x69E78, kSmSaving = 3;
constexpr uint32_t kPointSize = 0x18, kPointBuffer = 0x1C, kMaxBodyUsed = 0x64000;
constexpr unsigned kSidecarsPerSlot = 3;
static_assert(coop_bot_save::kMaxBots == kMaxBots, "one sidecar entry per unit");
static_assert(kGunKinds <= coop_bot_save::kMaxGuns, "gun bits fit the sidecar");

struct PointId { uint32_t point = 0, buffer = 0, size = 0; };
struct BotSnapshot {
    bool valid = false;
    PointId at;   // the save point right after the store
    coop_bot_save::Bot bots[kMaxBots];
};
bool perSaveWanted = false, perSaveActive = false, fileApplyPending = false;
BotSnapshot deathSnap, levelSnap;
std::string sidecarDir;
const char* gunNames[kGunKinds]{};
safetyhook::InlineHook pointStoreHook, restoreSaveHook, chooseSaveHook, postLoadHook;
volatile LONG snapDeath = 0, snapLevel = 0, snapOther = 0, restoreFile = 0, restoreSnap = 0, restoreFresh = 0, restoreUnknown = 0, sidecarsWritten = 0,
              sidecarFaults = 0, savesNotStarted = 0, loadsMatched = 0, loadsFresh = 0, loadsRejected = 0, loadsUnkeyed = 0, sidecarsPruned = 0,
              perSaveLines = 0;

template<class... A> void SaveEvent(const char* format, A... args) {
    if (InterlockedIncrement(&perSaveLines) <= 400) gate::Event(format, args...);
}
PointId PointAt(uint32_t rva) {
    PointId id;
    if (Read(base + rva, &id.point, 4) && id.point && !(Read(id.point + kPointSize, &id.size, 4) && Read(id.point + kPointBuffer, &id.buffer, 4)))
        id.size = id.buffer = 0;
    return id;
}
bool SamePoint(const BotSnapshot& s, const PointId& id) {
    return s.valid && id.point && id.size && s.at.point == id.point && s.at.buffer == id.buffer && s.at.size == id.size;
}
void Capture(coop_bot_save::Bot* out) {
    for (unsigned i = 0; i < kMaxBots; ++i) {
        const Unit& unit = units[i];
        coop_bot_save::Bot& b = out[i];
        b = coop_bot_save::Bot{};
        b.present = true;
        std::snprintf(b.name, sizeof(b.name), "%s", unit.name);
        // A purchase under way is not its gun yet; the price still belongs to the bot (as in Let).
        int32_t credits = unit.creditWallet + (unit.gunPending >= 0 ? static_cast<int32_t>(unit.gunPrice) : 0);
        b.credits = credits < 0 ? 0 : (credits > coop_bot_save::kMaxCredits ? coop_bot_save::kMaxCredits : credits);
        b.nodes = unit.nodeWallet < 0 ? 0 : (unit.nodeWallet > coop_bot_save::kMaxNodes ? coop_bot_save::kMaxNodes : unit.nodeWallet);
        b.guns = unit.gunsOwned;
        b.seeded = !unit.walletFresh;
        if (unit.suitChosen >= 0 && unit.suitChosen < kSuitKinds) {
            b.hasSuit = true;
            std::memcpy(b.suit, kSuits[unit.suitChosen].guid, 16);
        }
    }
}
void Fresh(coop_bot_save::Bot* out) {
    for (unsigned i = 0; i < kMaxBots; ++i) {
        out[i] = coop_bot_save::Bot{};
        out[i].present = true;
        out[i].seeded = false;
    }
}
// "credits 1200/0/0 nodes 3/0/0 guns 6/0/0 suits 4/-1/-1"
void Describe(const coop_bot_save::Bot* bots, char* out, size_t size) {
    int suit[kMaxBots];
    for (unsigned i = 0; i < kMaxBots; ++i) suit[i] = bots[i].hasSuit ? SuitOfGuid(bots[i].suit) : -1;
    std::snprintf(out, size, "credits %ld/%ld/%ld (started %d%d%d) nodes %ld/%ld/%ld guns %X/%X/%X suits %d/%d/%d", static_cast<long>(bots[0].credits),
                  static_cast<long>(bots[1].credits), static_cast<long>(bots[2].credits), bots[0].seeded ? 1 : 0, bots[1].seeded ? 1 : 0,
                  bots[2].seeded ? 1 : 0, static_cast<long>(bots[0].nodes), static_cast<long>(bots[1].nodes), static_cast<long>(bots[2].nodes), bots[0].guns,
                  bots[1].guns, bots[2].guns, suit[0], suit[1], suit[2]);
}
// The bots take this state over: a new body gets the wallets (TrackNodes/TrackCredits),
// the bought guns (ReissueGuns) and the suit.
void ApplySaved(const coop_bot_save::Bot* bots, const char* why) {
    unsigned live = 0, unknownSuits = 0;
    for (unsigned i = 0; i < kMaxBots; ++i) {
        Unit& unit = units[i];
        const coop_bot_save::Bot& b = bots[i];
        unit.creditWallet = b.present ? b.credits : 0;
        unit.walletFresh = !(b.present && b.seeded);
        unit.nodeWallet = b.present ? b.nodes : 0;
        unit.gunsOwned = b.present ? b.guns & ((1u << kGunKinds) - 1u) : 0;
        unit.suitChosen = b.present && b.hasSuit ? SuitOfGuid(b.suit) : -1;
        if (b.present && b.hasSuit && unit.suitChosen < 0) ++unknownSuits;
        unit.creditsSet = unit.nodesSet = false;
        unit.creditsSeen = unit.nodesSeen = -1;
        unit.gunPrice = 0;   // a purchase under way is no refund into the restored wallet
        if (unit.entity) ++live;
        else { unit.gunPending = -1; unit.gunProxy = 0; }
    }
    leaderCreditsSeen = -1;
    lastChapter = 0;   // the restored state is the game's own; no "chapter went back" reset after it
    char text[160];
    Describe(bots, text, sizeof(text));
    SaveEvent("bot_save applied (%s): %s; live bodies %u, unknown suits %u", why, text, live, unknownSuits);
}

bool WriteSidecar(const coop_bot_save::File& f, std::string& path) {
    CreateDirectoryA(sidecarDir.c_str(), nullptr);
    path = sidecarDir + coop_bot_save::FileName(f.slot, f.key);
    const std::string tmp = path + ".tmp";
    const std::string text = coop_bot_save::Write(f, gunNames, kGunKinds);
    const HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileA(tmp.c_str());
    return false;
}
// Keeps the newest few per slot: a failed write leaves the older save on disk, and its sidecar with it.
void PruneSlot(int slot) {
    char pattern[32];
    std::snprintf(pattern, sizeof(pattern), "slot%02d_*.ds2bots", slot);
    struct Entry { char name[MAX_PATH]; FILETIME at; };
    Entry found[32];
    unsigned n = 0;
    WIN32_FIND_DATAA data{};
    const HANDLE find = FindFirstFileA((sidecarDir + pattern).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (n < std::size(found) && !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::snprintf(found[n].name, sizeof(found[n].name), "%s", data.cFileName);
            found[n++].at = data.ftLastWriteTime;
        }
    } while (FindNextFileA(find, &data));
    FindClose(find);
    for (unsigned a = 1; a < n; ++a)
        for (unsigned b = a; b > 0 && CompareFileTime(&found[b].at, &found[b - 1].at) > 0; --b) std::swap(found[b], found[b - 1]);
    for (unsigned k = kSidecarsPerSlot; k < n; ++k)
        if (DeleteFileA((sidecarDir + found[k].name).c_str())) {
            InterlockedIncrement(&sidecarsPruned);
            SaveEvent("bot_save pruned %s (older than the newest %u of slot %d)", found[k].name, kSidecarsPerSlot, slot);
        }
}
// The sidecar of exactly this save, by name and by the key inside it; the newest when several slots hold it.
bool FindSidecar(const coop_bot_save::Key& key, coop_bot_save::File& out, char* chosen, size_t chosenSize) {
    char pattern[48];
    std::snprintf(pattern, sizeof(pattern), "slot*_%08X_%08X.ds2bots", static_cast<unsigned>(key.hash), static_cast<unsigned>(key.bodyUsed));
    WIN32_FIND_DATAA data{};
    const HANDLE find = FindFirstFileA((sidecarDir + pattern).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    bool have = false;
    FILETIME best{};
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || data.nFileSizeHigh || data.nFileSizeLow > 65536) continue;
        const std::string path = sidecarDir + data.cFileName;
        std::string text(data.nFileSizeLow, '\0');
        FILE* file = nullptr;
        size_t got = 0;
        if (fopen_s(&file, path.c_str(), "rb") == 0 && file) {
            got = text.empty() ? 0 : std::fread(&text[0], 1, text.size(), file);
            std::fclose(file);
        }
        text.resize(got);
        coop_bot_save::File parsed;
        unsigned unknownGuns = 0;
        const coop_bot_save::Error e = coop_bot_save::Parse(text, gunNames, kGunKinds, parsed, &unknownGuns);
        if (e != coop_bot_save::Error::None || parsed.key.hash != key.hash || parsed.key.bodyUsed != key.bodyUsed) {
            InterlockedIncrement(&loadsRejected);
            SaveEvent("bot_save rejected %s: %s (key inside %08X/%X)", data.cFileName,
                      e != coop_bot_save::Error::None ? coop_bot_save::ErrorName(e) : "key differs", parsed.key.hash, parsed.key.bodyUsed);
            continue;
        }
        if (unknownGuns) SaveEvent("bot_save %s: %u gun name(s) unknown to this build, skipped", data.cFileName, unknownGuns);
        if (!have || CompareFileTime(&data.ftLastWriteTime, &best) > 0) {
            out = parsed;
            best = data.ftLastWriteTime;
            std::snprintf(chosen, chosenSize, "%s", data.cFileName);
            have = true;
        }
    } while (FindNextFileA(find, &data));
    FindClose(find);
    return have;
}
bool ReadSaveKey(uint32_t sm, coop_bot_save::Key& key) {
    char map[sizeof(key.map)]{};
    if (!Read(sm + kSmHash, &key.hash, 4) || !Read(sm + kSmBodyUsed, &key.bodyUsed, 4) || !Read(sm + kSmMap, map, sizeof(map) - 1)) return false;
    std::snprintf(key.map, sizeof(key.map), "%s", map);
    return key.hash && key.bodyUsed && key.bodyUsed <= kMaxBodyUsed;
}

// Both save points run through here (SetDeathRestart and StoreLevel); thiscall(point, allocator, kind), ret 8.
bool __fastcall OnPointStore(void* self, void*, uint32_t allocator, uint32_t kind) {
    const bool stored = pointStoreHook.thiscall<bool>(self, allocator, kind);
    if (!perSaveActive) return stored;
    const uint32_t point = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    uint32_t death = 0, level = 0;
    Read(base + kDeathPointRva, &death, 4);
    Read(base + kLevelPointRva, &level, 4);
    BotSnapshot* snap = point == death ? &deathSnap : (point == level ? &levelSnap : nullptr);
    InterlockedIncrement(snap == &deathSnap ? &snapDeath : (snap ? &snapLevel : &snapOther));
    if (!snap) return stored;
    snap->valid = stored;
    snap->at = PointAt(snap == &deathSnap ? kDeathPointRva : kLevelPointRva);
    // Before the loaded save's level start the units still hold the old game.
    if (fileApplyPending) {
        if (snap != &deathSnap) std::copy_n(deathSnap.bots, kMaxBots, snap->bots);
    } else {
        Capture(snap->bots);
    }
    char text[160];
    Describe(snap->bots, text, sizeof(text));
    SaveEvent("bot_save snapshot %s point=%08X size=%X kind=%u stored=%d: %s", snap == &deathSnap ? "death_restart" : "level", point, snap->at.size, kind,
              stored ? 1 : 0, text);
    return stored;
}
// cdecl, no arguments: the level start restores one of the two points.
void OnRestoreSave() {
    if (perSaveActive) {
        uint32_t id[4]{};
        Read(base + kDeathRestartIdRva, id, sizeof(id));
        const bool death = id[0] || id[1] || id[2] || id[3];
        const PointId at = PointAt(death ? kDeathPointRva : kLevelPointRva);
        const BotSnapshot& snap = death ? deathSnap : levelSnap;
        const char* which = death ? "death_restart" : "level";
        if (fileApplyPending) {
            fileApplyPending = false;
            InterlockedIncrement(&restoreFile);
            SaveEvent("bot_save restore %s point=%08X size=%X: the loaded save's state", which, at.point, at.size);
            ApplySaved(deathSnap.bots, "loaded save, level start");
        } else if (SamePoint(snap, at)) {
            InterlockedIncrement(&restoreSnap);
            SaveEvent("bot_save restore %s point=%08X size=%X: back to its snapshot", which, at.point, at.size);
            ApplySaved(snap.bots, death ? "death restart" : "level start");
        } else if (!at.point || !at.size) {
            InterlockedIncrement(&restoreFresh);
            coop_bot_save::Bot fresh[kMaxBots];
            Fresh(fresh);
            SaveEvent("bot_save restore %s: the save point is empty (a new game): fresh bots", which);
            ApplySaved(fresh, "new game");
        } else {
            InterlockedIncrement(&restoreUnknown);
            SaveEvent("bot_save restore %s point=%08X size=%X: no snapshot of it (snapshot %08X size %X valid %d); bots keep their state", which, at.point,
                      at.size, snap.at.point, snap.at.size, snap.valid ? 1 : 0);
        }
    }
    restoreSaveHook.ccall<void>();
}
// thiscall(SaveManager, slot, guid&, guid&, const char*, const char*, bool), ret 0x18.
void __fastcall OnChooseSaveSlot(void* self, void*, uint32_t slot, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
    chooseSaveHook.thiscall<void>(self, slot, a1, a2, a3, a4, a5);
    if (!perSaveActive) return;
    const uint32_t sm = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    uint32_t state = 0, saving = 0;
    coop_bot_save::File f;
    if (!Read(sm + kSmState, &state, 4) || !Read(sm + kSmSaveSlot, &saving, 4) || state != kSmSaving || saving != slot || slot >= 20 ||
        !ReadSaveKey(sm, f.key)) {
        InterlockedIncrement(&savesNotStarted);
        SaveEvent("bot_save slot %u: no save under way (state %u, slot %u, hash %08X, body %X); no sidecar", slot + 1, state, saving, f.key.hash, f.key.bodyUsed);
        return;
    }
    f.slot = static_cast<int>(slot) + 1;
    const PointId at = PointAt(kDeathPointRva);
    const bool snapped = SamePoint(deathSnap, at);
    if (snapped) std::copy_n(deathSnap.bots, kMaxBots, f.bots);
    else Capture(f.bots);
    std::string path;
    char text[160];
    Describe(f.bots, text, sizeof(text));
    if (!WriteSidecar(f, path)) {
        InterlockedIncrement(&sidecarFaults);
        SaveEvent("bot_save slot %d hash %08X: writing %s failed (error %lu)", f.slot, f.key.hash, path.c_str(), GetLastError());
        return;
    }
    InterlockedIncrement(&sidecarsWritten);
    SaveEvent("bot_save wrote slot %d hash %08X body %X map %s from %s: %s", f.slot, f.key.hash, f.key.bodyUsed, f.key.map,
              snapped ? "the death restart snapshot" : "the current state (no snapshot of this save point)", text);
    PruneSlot(f.slot);
}
// thiscall(SaveManager), no arguments: the header still holds the loaded file's key.
void __fastcall OnPostLoadFinished(void* self, void*) {
    coop_bot_save::Key key;
    const bool keyed = perSaveActive && ReadSaveKey(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)), key);
    postLoadHook.thiscall<void>(self);
    if (!perSaveActive) return;
    coop_bot_save::File f;
    char chosen[MAX_PATH] = "";
    const bool found = keyed && FindSidecar(key, f, chosen, sizeof(chosen));
    if (found) {
        InterlockedIncrement(&loadsMatched);
        std::copy_n(f.bots, kMaxBots, deathSnap.bots);
        SaveEvent("bot_save load hash %08X body %X map %s: %s (slot %d when written)", key.hash, key.bodyUsed, key.map, chosen, f.slot);
    } else {
        InterlockedIncrement(keyed ? &loadsFresh : &loadsUnkeyed);
        Fresh(deathSnap.bots);
        SaveEvent("bot_save load hash %08X body %X map %s: %s; fresh bots", key.hash, key.bodyUsed, key.map,
                  keyed ? "no sidecar for this save" : "no save key in the header");
    }
    // RestoreFromSavedGame rebuilt both points; the death restart one now holds the file's body.
    // The units take it over at the level start (RestoreSave), once the old bodies are gone.
    deathSnap.valid = true;
    deathSnap.at = PointAt(kDeathPointRva);
    levelSnap.valid = false;
    fileApplyPending = true;
}

void InstallPerSave(FILE* log, const std::string& ini) {
    perSaveWanted = GetPrivateProfileIntA("Bot", "PerSaveInventory", 1, ini.c_str()) != 0;
    const size_t slash = ini.find_last_of("\\/");
    sidecarDir = (slash == std::string::npos ? std::string() : ini.substr(0, slash + 1)) + "DS2BotSaves\\";
    for (int i = 0; i < kGunKinds; ++i) gunNames[i] = kGunCatalogue[i].name;
    if (!perSaveWanted) {
        std::fprintf(log, "bot per_save_inventory=0 (the bots' credits, nodes, guns and suit live for the game session only)\n");
        return;
    }
    // push ebp / mov ebp,esp / sub esp,28h / push ebx / push esi / mov esi,ecx / mov ecx,[esi+14h] / push edi / xor edi,edi
    static const unsigned char kStore[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x28, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x14, 0x57, 0x33, 0xFF};
    // cmp dword [death restart id],0 / jne / cmp dword [id+4],0
    unsigned char restore[] = {0x83, 0x3D, 0, 0, 0, 0, 0x00, 0x75, 0x2D, 0x83, 0x3D, 0, 0, 0, 0, 0x00};
    const uint32_t id = static_cast<uint32_t>(base + kDeathRestartIdRva), id4 = id + 4;
    std::memcpy(restore + 2, &id, 4);
    std::memcpy(restore + 11, &id4, 4);
    // sub esp,418h / push esi / push edi / mov edi,[esp+424h]
    static const unsigned char kChoose[] = {0x81, 0xEC, 0x18, 0x04, 0x00, 0x00, 0x56, 0x57, 0x8B, 0xBC, 0x24, 0x24, 0x04, 0x00, 0x00};
    // push esi / mov esi,ecx / mov ecx,[esi+70h] / push edi / sub esp,10h / mov eax,esp / mov [eax],ecx
    static const unsigned char kPostLoad[] = {0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x70, 0x57, 0x83, 0xEC, 0x10, 0x8B, 0xC4, 0x89, 0x08};
    const bool store = hook_site::Install(log, "bot SavePoint::Store", base + kPointStoreRva, kStore, sizeof(kStore), reinterpret_cast<void*>(OnPointStore),
                                          pointStoreHook);
    const bool restoreOk = hook_site::Install(log, "bot Checkpoint::RestoreSave", base + kRestoreSaveRva, restore, sizeof(restore),
                                              reinterpret_cast<void*>(OnRestoreSave), restoreSaveHook);
    const bool choose = hook_site::Install(log, "bot SaveManager::ChooseSaveSlot", base + kChooseSaveSlotRva, kChoose, sizeof(kChoose),
                                           reinterpret_cast<void*>(OnChooseSaveSlot), chooseSaveHook);
    const bool load = hook_site::Install(log, "bot SaveManager::PostLoadFinished", base + kPostLoadFinishedRva, kPostLoad, sizeof(kPostLoad),
                                         reinterpret_cast<void*>(OnPostLoadFinished), postLoadHook);
    uint32_t sm = 0;
    Read(base + kSaveManagerRva, &sm, 4);
    perSaveActive = store && restoreOk && choose && load;
    std::fprintf(log, "bot per_save_inventory=1 active=%d store=%d restore=%d save=%d load=%d save_manager=%08X dir=%s (each campaign save keeps the "
                      "bots' credits, nodes, bought guns and suit; a death restart rolls them back with the player's)\n",
                 perSaveActive ? 1 : 0, store ? 1 : 0, restoreOk ? 1 : 0, choose ? 1 : 0, load ? 1 : 0, sm, sidecarDir.c_str());
    if (!perSaveActive) std::fprintf(log, "bot per_save_inventory: a hook is missing; the bots keep their state for the game session only\n");
}

void Install(FILE* log, const std::string& ini) {
    enabled = botWanted;
    InstallPassThrough(log, GetPrivateProfileIntA("Bot", "PlayerCollision", 1, ini.c_str()) == 0);
    toggleKey = GetPrivateProfileIntA("Bot", "Key", 0, ini.c_str());
    if (toggleKey < 0 || toggleKey > 255) toggleKey = 0;
    callKey = GetPrivateProfileIntA("Bot", "CallKey", 0, ini.c_str());
    markKey = GetPrivateProfileIntA("Bot", "MarkKey", 105, ini.c_str());
    if (markKey < 0 || markKey > 255) markKey = 0;
    if (callKey < 0 || callKey > 255) callKey = 0;
    orderKeys[0].vk = GetPrivateProfileIntA("Bot", "FollowHoldKey", orderKeys[0].fallback, ini.c_str());
    orderKeys[1].vk = GetPrivateProfileIntA("Bot", "GoThereKey", orderKeys[1].fallback, ini.c_str());
    orderKeys[2].vk = GetPrivateProfileIntA("Bot", "AttackTargetKey", orderKeys[2].fallback, ini.c_str());
    orderKeys[3].vk = GetPrivateProfileIntA("Bot", "FireHoldKey", orderKeys[3].fallback, ini.c_str());
    orderKeys[4].vk = GetPrivateProfileIntA("Bot", "VanguardKey", orderKeys[4].fallback, ini.c_str());
    for (OrderKey& key : orderKeys) {
        if (key.vk < 0 || key.vk > 255) key.vk = 0;
        if (key.vk && (key.vk == toggleKey || key.vk == callKey)) {
            std::fprintf(log, "bot %s=%d is the toggle or call key already; off\n", key.name, key.vk);
            key.vk = 0;
        }
    }
    std::fprintf(log, "bot order_keys follow_hold=%d go_there=%d attack_target=%d fire_hold=%d vanguard=%d (0 = off; the numpad keys need Num Lock on)\n",
                 orderKeys[0].vk, orderKeys[1].vk, orderKeys[2].vk, orderKeys[3].vk, orderKeys[4].vk);
    const int spawnDelay = GetPrivateProfileIntA("Bot", "SpawnDelayMs", 4000, ini.c_str());
    spawnDelayMs = static_cast<DWORD>(spawnDelay < 0 ? 0 : spawnDelay);   // negative would wrap to 49 days
    const int heal = GetPrivateProfileIntA("Bot", "HealBelowPercent", 60, ini.c_str());
    // Bots at once: 1 (the default) to 3, the room the CameraManager's four viewports leave.
    const int count = GetPrivateProfileIntA("Bot", "Count", 1, ini.c_str());
    wantedBots = static_cast<unsigned>(count < 1 ? 1 : (count > static_cast<int>(kMaxBots) ? static_cast<int>(kMaxBots) : count));
    // The first bot's place by the resting leader, in metres of his own frame.
    const bool partner = GetPrivateProfileIntA("Bot", "PartnerPose", 0, ini.c_str()) != 0;
    auto metres = [&](const char* key, float fallback) {
        char text[32]{};
        GetPrivateProfileStringA("Bot", key, "", text, sizeof(text), ini.c_str());
        const float v = text[0] ? static_cast<float>(std::atof(text)) : fallback;
        return std::isfinite(v) && std::fabs(v) <= 6.0f ? v : fallback;
    };
    const float partnerForward = metres("PartnerForward", 0.97f), partnerRight = metres("PartnerRight", 0.85f);
    const float partner2Forward = metres("Partner2Forward", -0.65f), partner2Right = metres("Partner2Right", 0.98f);
    // In a fight a bot farther than this from the leader walks back toward him,
    // until it is within CombatFollowMeters - 3 m, at most 6 m (28.09.: at 6 m every
    // few steps of the aiming leader moved the bots too). Inside it his steps move
    // nobody (27.09.).
    float combatFollow = 9.0f;
    {
        char text[32]{};
        GetPrivateProfileStringA("Bot", "CombatFollowMeters", "9", text, sizeof(text), ini.c_str());
        const float v = static_cast<float>(std::atof(text));
        if (std::isfinite(v) && v >= 4.0f && v <= 25.0f) combatFollow = v;
    }
    // Backing off in a fight: from any enemy within BackOffMeters, or from CrowdCount
    // of them within CrowdMeters (0 turns either rule off). FireIntervalMs paces the
    // plain shots (cutter and whatever else has no rule of its own); AimLead leads a
    // moving aim point by the look's own lag (runtime Look).
    auto setting = [&](const char* key, float fallback, float lo, float hi) {
        char text[32]{};
        GetPrivateProfileStringA("Bot", key, "", text, sizeof(text), ini.c_str());
        const float v = text[0] ? static_cast<float>(std::atof(text)) : fallback;
        return std::isfinite(v) ? (v < lo ? lo : (v > hi ? hi : v)) : fallback;
    };
    const float backOffMeters = setting("BackOffMeters", 3.0f, 0.0f, 8.0f), crowdMeters = setting("CrowdMeters", 4.5f, 0.0f, 10.0f);
    const int crowdCount = static_cast<int>(setting("CrowdCount", 2.0f, 0.0f, 8.0f));
    const uint32_t fireIntervalMs = static_cast<uint32_t>(setting("FireIntervalMs", 450.0f, 150.0f, 2000.0f));
    aimLead = GetPrivateProfileIntA("Bot", "AimLead", 1, ini.c_str()) != 0;
    std::fprintf(log, "bot back_off=%.1f m crowd=%d within %.1f m fire_interval=%u ms aim_lead=%d (a bot backs off close enemies while it shoots; "
                      "the aim leads a moving point by the look's lag)\n",
                 backOffMeters, crowdCount, crowdMeters, fireIntervalMs, aimLead ? 1 : 0);
    // Human senses and aim (coop_bot_brain.h Senses, HumanFlick).
    const bool humanSenses = GetPrivateProfileIntA("Bot", "HumanSenses", 1, ini.c_str()) != 0;
    const float viewAngle = setting("ViewAngle", 120.0f, 30.0f, 360.0f), peripheralMeters = setting("PeripheralMeters", 2.0f, 0.0f, 6.0f);
    const float hearMeters = setting("HearMeters", 12.0f, 0.0f, 40.0f);
    const bool humanAim = GetPrivateProfileIntA("Bot", "HumanAim", 1, ini.c_str()) != 0;
    const float aimErrorDeg = setting("AimErrorDeg", 2.5f, 0.0f, 10.0f), aimJitterDeg = setting("AimJitterDeg", 0.5f, 0.0f, 3.0f);
    const uint32_t aimSettleMaxMs = static_cast<uint32_t>(setting("AimSettleMaxMs", 650.0f, 0.0f, 2000.0f));
    const float aimLeadError = setting("AimLeadError", 0.25f, 0.0f, 1.0f);
    std::fprintf(log, "bot human_senses=%d view_angle=%.0f deg peripheral=%.1f m hear=%.1f m human_aim=%d aim_error=%.1f deg jitter=%.2f deg settle_max=%u ms "
                      "lead_error=%.2f (senses: a clear ray counts only inside the view cone or within the peripheral range; aim: pause, flick, settle)\n",
                 humanSenses ? 1 : 0, viewAngle, peripheralMeters, hearMeters, humanAim ? 1 : 0, aimErrorDeg, aimJitterDeg, aimSettleMaxMs, aimLeadError);
    // Twitchers dodge an aim after 0.25 s (Player::UpdateAimingTarget threatens what any Player aims at, bots too).
    const bool twitcherSnap = GetPrivateProfileIntA("Bot", "TwitcherSnapShots", 1, ini.c_str()) != 0;
    std::fprintf(log, "bot twitcher_snap_shots=%d (a twitcher target: no aim pause, no settling, a wider shot cone, no blade turn)\n", twitcherSnap ? 1 : 0);
    const bool stasisPriority = GetPrivateProfileIntA("Bot", "StasisPriority", 1, ini.c_str()) != 0;
    std::fprintf(log, "bot stasis_priority=%d (twitchers and charging stalkers within 14 m: stasis first, with any energy for a cast, 2.5 s apart; "
                      "a stasised enemy is the one whose time multiplier is below %.1f)\n", stasisPriority ? 1 : 0, kStasisSlowBelow);
    stompCorpses = GetPrivateProfileIntA("Bot", "StompCorpses", 1, ini.c_str()) != 0;
    lootContainers = GetPrivateProfileIntA("Bot", "LootContainers", 1, ini.c_str()) != 0;
    const int crateShootPercent = GetPrivateProfileIntA("Bot", "CrateShootPercent", 50, ini.c_str());
    rescueLeader = GetPrivateProfileIntA("Bot", "RescueLeader", 1, ini.c_str()) != 0;
    {
        const int perCorpse = GetPrivateProfileIntA("Bot", "CorpseStomps", 3, ini.c_str());
        corpseStompMax = perCorpse < 1 ? 1 : (perCorpse > 6 ? 6 : perCorpse);
    }
    std::fprintf(log, "bot stomp_corpses=%d loot_containers=%d corpse_stomps=%d scripted_containers=%zu keep_corpse_loot=%d (after a fight the bots stomp fresh "
                      "corpses and supply crates and open lockers near the leader, then pick up what drops; a corpse is done once its drop flag is set)\n",
                 stompCorpses ? 1 : 0, lootContainers ? 1 : 0, corpseStompMax, std::size(::ds2::kScriptedContainers), drops::keepBotLoot ? 1 : 0);
    std::fprintf(log, "bot rescue_leader=%d (an enemy holding the leader is every bot's target at once: his paired-attack coordinator's attacker, "
                      "else the nearest enemy on him)\n",
                 rescueLeader ? 1 : 0);
    for (unsigned i = 0; i < kMaxBots; ++i) {
        Unit& unit = units[i];
        unit.index = i;
        unit.slotId = 1 + i;
        unit.controller = 14 - i;
        unit.brain = coop_bot::Brain(0x2545F491u + i * 0x9E3779B9u);
        unit.brain.tune.healBelow = static_cast<float>(heal < 10 ? 10 : (heal > 95 ? 95 : heal)) / 100.0f;
        unit.brain.tune.partnerPose = partner;
        unit.brain.tune.partnerForward = partnerForward;
        unit.brain.tune.partnerRight = partnerRight;
        unit.brain.tune.partner2Forward = partner2Forward;
        unit.brain.tune.partner2Right = partner2Right;
        unit.brain.tune.combatFollow = combatFollow;
        unit.brain.tune.combatRadius = combatFollow;
        unit.brain.tune.combatRejoin = combatFollow - 3.0f < 3.5f ? 3.5f : (combatFollow - 3.0f > 6.0f ? 6.0f : combatFollow - 3.0f);
        // The leader's few steps do not move the bots (29.09.: they copied every step).
        unit.brain.tune.followReanchor = 3.5f;
        unit.brain.tune.followKeep = 4.5f;
        unit.brain.tune.partnerMove = 3.0f;
        unit.brain.tune.partnerTurnDeg = 100.0f;
        unit.brain.tune.lootWanted = stompCorpses || lootContainers;
        unit.brain.tune.crateShootChance = static_cast<float>(crateShootPercent < 0 ? 0 : (crateShootPercent > 100 ? 100 : crateShootPercent)) / 100.0f;
        unit.brain.tune.corpseStompsMax = corpseStompMax;
        unit.brain.tune.backOffRange = backOffMeters;
        unit.brain.tune.crowdRange = crowdMeters;
        unit.brain.tune.crowdCount = crowdCount;
        unit.brain.tune.fireIntervalMs = fireIntervalMs;
        coop_bot::Tuning& human = unit.brain.tune;
        human.humanSenses = humanSenses;
        human.viewAngleDeg = viewAngle;
        human.peripheral = peripheralMeters;
        human.threatHearRange = hearMeters;
        human.humanAim = humanAim;
        human.flickErrorDeg = aimErrorDeg;
        human.aimJitterDeg = aimJitterDeg;
        human.aimSettleMaxMs = aimSettleMaxMs;
        human.leadError = aimLeadError;
        human.dodgerSnap = twitcherSnap;
        human.stasisPriority = stasisPriority;
        human.aimSkill = BuiltInProfile(i).aimSkill;   // the profile may set its own
    }
    anticsEnabled = GetPrivateProfileIntA("Bot", "AnticsEnabled", 1, ini.c_str()) != 0;
    {
        // The personalities, when [Bot] Profiles=1 (DS2BotProfiles.ini next to this INI).
        const bool profiles = GetPrivateProfileIntA("Bot", "Profiles", 0, ini.c_str()) != 0;
        const size_t slash = ini.find_last_of("\\/");
        const std::string file = ds2coop::ConfigFile(slash == std::string::npos ? std::string() : ini.substr(0, slash + 1), "DS2BotProfiles.ini");
        const bool found = GetFileAttributesA(file.c_str()) != INVALID_FILE_ATTRIBUTES;
        for (unsigned i = 0; i < kMaxBots && profiles; ++i) {
            const Profile prof = LoadProfile(file, i);
            ApplyProfile(units[i], prof, units[i].brain.tune.healBelow);
            const coop_bot::Tuning& t = units[i].brain.tune;
            std::fprintf(log, "bot %u profile \"%s\": reaction %u-%u ms, scan after a fight %u-%u ms, aim first %u ms, sway %.2f m, aim skill %.2f, turn x%.2f, pace x%.2f, "
                              "fights at %.1f-%.1f m, heals below %.0f%%, kinesis %d%%, place %.0f deg %.1f m, watches %.0f deg, fight side %+.0f %.1f m back %.1f m, suit \"%s\"\n",
                         i + 1, units[i].name, t.reactMinMs, t.reactMaxMs, t.postCombatMinMs, t.postCombatMaxMs, t.aimAcquireMs, t.aimSway, t.aimSkill, units[i].aimSpeed,
                         units[i].walkPace, t.keepMin, t.keepMax, t.healBelow * 100.0f, units[i].kinesisChance, t.followAngleDeg, t.followDistance, t.watchAngleDeg,
                         t.combatSideSign, t.combatSide, t.combatBackBy, units[i].favoriteSuit);
        }
        std::fprintf(log, "bot profiles=%d file=%s (%s)\n", profiles ? 1 : 0, file.c_str(), found ? "found" : "missing: built-in profiles");
        for (unsigned i = 0; i < kMaxBots; ++i)
            std::fprintf(log, "bot %u (%s) antics=%d harmful_antics=%d (AnticsEnabled=%d profiles=%d; 0 never, 1 rare, 3 often; harmful = the stasis prank)\n",
                         i + 1, units[i].name, units[i].brain.tune.antics, units[i].brain.tune.harmfulAntics ? 1 : 0, anticsEnabled ? 1 : 0, profiles ? 1 : 0);
    }
    u = &units[0];
    if (enabled) {
        // push ebp / mov ebp,esp / and esp,-16 / sub esp,8Ch / cmp dword [ecx+164h],0 / push esi / jbe
        static const unsigned char kRender[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x8C, 0x00, 0x00, 0x00, 0x83, 0xB9, 0x64, 0x01, 0x00, 0x00, 0x00};
        // push esi / push edi / mov edi,ecx / xor esi,esi / cmp [edi+0E0h],esi / jbe
        static const unsigned char kClear[] = {0x56, 0x57, 0x8B, 0xF9, 0x33, 0xF6, 0x39, 0xB7, 0xE0, 0x00, 0x00, 0x00, 0x76, 0x1D, 0x53, 0x8D, 0x5F, 0x18};
        // sub esp,14h / push ebx / push ebp / push esi / push edi / push -1 / xor esi,esi / push 0B8B0FBC3h
        static const unsigned char kRestore[] = {0x83, 0xEC, 0x14, 0x53, 0x55, 0x56, 0x57, 0x6A, 0xFF, 0x33, 0xF6, 0x68, 0xC3, 0xFB, 0xB0, 0xB8};
        renderGuard = hook_site::Install(log, "bot CameraManager::SetRenderCamera", base + kSetRenderCameraRva, kRender, sizeof(kRender),
                                         reinterpret_cast<void*>(OnSetRenderCamera), renderHook);
        upgradeGuard = hook_site::Install(log, "bot UpgradeManager::Clear", base + kUpgradeClearRva, kClear, sizeof(kClear),
                                          reinterpret_cast<void*>(OnUpgradeClear), clearHook) &&
                       hook_site::Install(log, "bot UpgradeManager::Restore", base + kUpgradeRestoreRva, kRestore, sizeof(kRestore),
                                          reinterpret_cast<void*>(OnUpgradeRestore), restoreHook);
        // push ebp / mov ebp,esp / and esp,-16 / sub esp,114h / push ebx / mov ebx,[ebp+14h] / push esi / lea eax,[ebx-17h]
        static const unsigned char kDeathSm[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x14, 0x01, 0x00, 0x00, 0x53, 0x8B, 0x5D, 0x14, 0x56, 0x8D, 0x43, 0xE9};
        const bool deathSm = hook_site::Install(log, "bot PlayerDeathSM::HandleStateMessage", base + 0x4A12D0, kDeathSm, sizeof(kDeathSm),
                                                reinterpret_cast<void*>(OnDeathSm), deathSmHook);
        if (!deathSm) std::fprintf(log, "bot: without the death state guard the bot relies on its damage floor alone\n");
        grabEscape = GetPrivateProfileIntA("Bot", "NecroGrabEscape", 1, ini.c_str()) != 0;
        if (grabEscape) {
            // push ebp / mov ebp,esp / and esp,-16 / sub esp,54h / push ebx / push esi / mov esi,ecx / cmp dword [esi+74h],0 / push edi
            static const unsigned char kPacUpdate[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x54, 0x53, 0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x74, 0x00, 0x57};
            pacHooked = hook_site::Install(log, "bot PairedAttackCoordinator::Update", base + kPacUpdateRva, kPacUpdate, sizeof(kPacUpdate),
                                           reinterpret_cast<void*>(OnPacUpdate), pacUpdateHook);
        }
        std::fprintf(log, "bot necro_grab_escape=%d hooked=%d (a bot held by a necro player mashes its way out like a human)\n", grabEscape ? 1 : 0, pacHooked ? 1 : 0);
        // push ebp / mov ebp,esp / and esp,-16 / xorps xmm2,xmm2 / sub esp,74h / push ebx / push esi / mov esi,ecx
        static const unsigned char kHealthApply[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x0F, 0x57, 0xD2, 0x83, 0xEC, 0x74, 0x53, 0x56, 0x8B, 0xF1};
        if (!hook_site::Install(log, "bot HealthComponent::HandleDamage floor", base + kHealthApplyRva, kHealthApply, sizeof(kHealthApply),
                                reinterpret_cast<void*>(OnHealthApply), healthApplyHook))
            std::fprintf(log, "bot: without the health floor a blast can still kill a bot (a new body follows)\n");
        // push ecx / push ebp / mov ebp,ecx / movzx ecx,byte [esp+0Ch] / imul ecx,ecx,168h / push edi
        static const unsigned char kEvaluate[] = {0x51, 0x55, 0x8B, 0xE9, 0x0F, 0xB6, 0x4C, 0x24, 0x0C, 0x69, 0xC9, 0x68, 0x01, 0x00, 0x00, 0x57};
        if (!hook_site::Install(log, "bot ControllerManager event pass", base + kEvaluateRva, kEvaluate, sizeof(kEvaluate),
                                reinterpret_cast<void*>(OnEvaluate), evaluateHook))
            std::fprintf(log, "bot: without the event pass guard the bot takes the human's keyboard actions (stomp)\n");
        bot_wheel::Install(log, GetPrivateProfileIntA("Bot", "WheelGuard", 1, ini.c_str()) != 0);
        bot_light::Install(log, GetPrivateProfileIntA("Bot", "OwnAimLight", 1, ini.c_str()) != 0);
        const bool nav = bot_nav::Install(log);
        bot_sense::InstallLineOfSight(log);
        // push ebp / mov ebp,esp / and esp,-16 / sub esp,84h / push ebx / push esi / push edi / mov edi,[ebp+8]
        static const unsigned char kMuzzle[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x84, 0x00, 0x00, 0x00, 0x53, 0x56, 0x57, 0x8B, 0x7D, 0x08};
        // push ebp / mov ebp,esp / and esp,-16 / sub esp,1D4h / push ebx / push esi / mov ebx,ecx
        static const unsigned char kStasisSite[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xD4, 0x01, 0x00, 0x00, 0x53, 0x56, 0x8B, 0xD9};
        const bool muzzle = hook_site::Install(log, "bot Gun::GetMuzzleAim", base + kMuzzleAimRva, kMuzzle, sizeof(kMuzzle),
                                               reinterpret_cast<void*>(OnMuzzleAim), muzzleAimHook);
        const bool stasisAim = hook_site::Install(log, "bot PlayerFireStasisSM::ExecuteAttack", base + kStasisAttackRva, kStasisSite, sizeof(kStasisSite),
                                                  reinterpret_cast<void*>(OnStasisAttack), stasisAttackHook);
        std::fprintf(log, "bot own_aim_with_centered=%d stasis=%d (the bot's shots go where the bot aims, not the human's crosshair)\n",
                     muzzle ? 1 : 0, stasisAim ? 1 : 0);
        // push ebp / mov ebp,esp / and esp,-16 / sub esp,14h / push ebx / mov ebx,ecx / mov eax,[ebx+240h]
        static const unsigned char kFiring[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x14, 0x53, 0x8B, 0xD9, 0x8B, 0x83, 0x40, 0x02, 0x00, 0x00};
        if (!hook_site::Install(log, "bot ForceGun::GetFiringMatrix", base + kFiringMatrixRva, kFiring, sizeof(kFiring),
                                reinterpret_cast<void*>(OnFiringMatrix), firingMatrixHook))
            std::fprintf(log, "bot: without the firing matrix fix the force gun is left alone\n");
        // mov eax,[esp+4] / push esi / push edi / lea edi,[ecx+424h] / test eax,eax / je +5
        static const unsigned char kWish[] = {0x8B, 0x44, 0x24, 0x04, 0x56, 0x57, 0x8D, 0xB9, 0x24, 0x04, 0x00, 0x00, 0x85, 0xC0, 0x74, 0x05};
        // push ecx / push ebx / push ebp / mov ebp,[ecx+0BD8h] / push esi / push edi / lea edi,[ecx+0BD0h]
        static const unsigned char kSpare[] = {0x51, 0x53, 0x55, 0x8B, 0xA9, 0xD8, 0x0B, 0x00, 0x00, 0x56, 0x57, 0x8D, 0xB9, 0xD0, 0x0B, 0x00};
        unsigned char wishSite[sizeof(kWish)]{}, spareSite[sizeof(kSpare)]{};
        gunsReady = Read(base + kSetDesiredItemRva, wishSite, sizeof(wishSite)) && std::memcmp(wishSite, kWish, sizeof(kWish)) == 0 &&
                    Read(base + kCountSpareAmmoRva, spareSite, sizeof(spareSite)) && std::memcmp(spareSite, kSpare, sizeof(kSpare)) == 0;
        std::fprintf(log, "bot guns=%d (weapon slots, ammo and the game's own gun change)\n", gunsReady ? 1 : 0);
        // mov eax,[esp+8] / mov edx,ecx / mov ecx,[edx+88h] / not eax
        static const unsigned char kSetMask[] = {0x8B, 0x44, 0x24, 0x08, 0x8B, 0xD1, 0x8B, 0x8A, 0x88, 0x00, 0x00, 0x00, 0xF7, 0xD0};
        unsigned char maskSite[sizeof(kSetMask)]{};
        sceneMaskReady = Read(base + kSetSceneMaskRva, maskSite, sizeof(maskSite)) && std::memcmp(maskSite, kSetMask, sizeof(kSetMask)) == 0;
        std::fprintf(log, "bot scene_mask_sync=%d (the bot is drawn in the leader's room)\n", sceneMaskReady ? 1 : 0);
        bot_aim::Install(log, GetPrivateProfileIntA("Bot", "OwnLookInput", 1, ini.c_str()) != 0);
        InstallOwnMovement(log, GetPrivateProfileIntA("Bot", "OwnMovementInput", 1, ini.c_str()) != 0);
        benchWanted = GetPrivateProfileIntA("Bot", "Bench", 0, ini.c_str()) != 0;
        InstallBench(log);
        storeWanted = GetPrivateProfileIntA("Bot", "Store", 0, ini.c_str()) != 0;
        InstallStore(log);
        gunsWanted = GetPrivateProfileIntA("Bot", "BuyGuns", 0, ini.c_str()) != 0;
        ownGunsOnly = GetPrivateProfileIntA("Bot", "OwnGunsOnly", 1, ini.c_str()) != 0;
        storeWaitWanted = GetPrivateProfileIntA("Bot", "StoreWaitPlaces", 0, ini.c_str()) != 0;
        {
            char text[32] = "";
            GetPrivateProfileStringA("Bot", "KioskKeepOut", "2.0", text, sizeof(text), ini.c_str());
            const float keepOut = static_cast<float>(std::atof(text));
            kioskKeepOut = std::isfinite(keepOut) ? (keepOut < 0.0f ? 0.0f : (keepOut > 6.0f ? 6.0f : keepOut)) : 2.0f;
        }
        noBotFriendlyFire = GetPrivateProfileIntA("Bot", "NoBotFriendlyFire", 1, ini.c_str()) != 0;
        {
            const int streams = GetPrivateProfileIntA("Bot", "WeaponStreams", 9, ini.c_str());
            gunStreamCap = static_cast<unsigned>(streams < 4 ? 4 : (streams > 12 ? 12 : streams));
            const int share = GetPrivateProfileIntA("Bot", "CreditShare", 0, ini.c_str());
            creditShare = share < 0 ? 0 : (share > 200 ? 200 : share);
        }
        InstallGuns(log);
        InstallPerSave(log, ini);
        kioskWanted = GetPrivateProfileIntA("Bot", "UseKiosks", 0, ini.c_str()) != 0;
        kioskEnter = GetPrivateProfileIntA("Bot", "KioskEnter", 0, ini.c_str()) != 0;
        ownSuits = GetPrivateProfileIntA("Bot", "Suit", 0, ini.c_str()) != 0;
        infiniteAmmo = GetPrivateProfileIntA("Bot", "InfiniteAmmo", 0, ini.c_str()) != 0;
        {
            char text[32]{};
            GetPrivateProfileStringA("Bot", "PlayerStasisSeconds", "0", text, sizeof(text), ini.c_str());
            const float seconds = static_cast<float>(std::atof(text));
            playerStasisSeconds = std::isfinite(seconds) && seconds > 0.0f && seconds <= 30.0f ? seconds : 0.0f;
            // push ebx / mov ebx,ecx / movss xmm0,[ebx+58h] / comiss xmm0,[esp+0Ch] / push esi / jbe +6
            static const unsigned char kStasisEntity[] = {0x53, 0x8B, 0xD9, 0xF3, 0x0F, 0x10, 0x43, 0x58, 0x0F, 0x2F, 0x44, 0x24, 0x0C, 0x56, 0x76, 0x06};
            const bool cut = playerStasisSeconds > 0.0f &&
                             hook_site::Install(log, "bot StasisManager::StasisEntityInternal", base + kStasisInternalRva, kStasisEntity, sizeof(kStasisEntity),
                                                reinterpret_cast<void*>(OnStasisEntity), stasisEntityHook);
            std::fprintf(log, "bot player_stasis_seconds=%.1f hooked=%d (stasis on the player or a bot lasts at most this long)\n",
                         playerStasisSeconds, cut ? 1 : 0);
        }
        std::fprintf(log, "bot infinite_ammo=%d (the reserve of every gun a bot carries stays at three clips)\n", infiniteAmmo ? 1 : 0);
        std::fprintf(log, "bot own_suits=%d (each bot wears a suit the player owns, its FavoriteSuits first; picked when its body stands and at store visits)\n",
                     ownSuits ? 1 : 0);
        InstallProgression(log, ini);
        InstallKiosk(log);
        InstallTubeGuard(log);
        if (!useGuard) InstallUseGuard(log);   // the panel lock may have put it in first
        faceOpen = GetPrivateProfileIntA("Bot", "FaceOpenSpace", 1, ini.c_str()) != 0;
        std::fprintf(log, "bot face_open_space=%d line_of_sight=%d nav=%d (a bot at rest facing a wall turns toward open space once and steps off it)\n",
                     faceOpen ? 1 : 0, bot_sense::losReady ? 1 : 0, bot_nav::ready ? 1 : 0);
        if (GetPrivateProfileIntA("Bot", "AltFireTrace", 0, ini.c_str()) != 0) InstallAltTrace(log);
        else std::fprintf(log, "bot alt_fire_trace=0\n");
        storeRooms = kioskReady && GetPrivateProfileIntA("Bot", "StoreRooms", 0, ini.c_str()) != 0;
        std::fprintf(log, "bot store_rooms=%d (a store or bench within %.0f m of the leader makes a store room: the bots take their turns while he is there, "
                          "and whoever has not had one when he leaves it buys, upgrades and changes its suit at once)\n",
                     storeRooms ? 1 : 0, kRoomEnter);
        InstallPrompts(log);
        {
            // test ecx,ecx / je / movzx eax,byte [the component type] / push eax / call (the address moves with the image)
            static const unsigned char kGetHealth[] = {0x85, 0xC9, 0x74, 0x0E, 0x0F, 0xB6, 0x05, 0x00, 0x00, 0x00, 0x00, 0x50, 0xE8};
            // mov al,[ecx+0C4h] / test al,2 / jne / xorps xmm0,xmm0 / comiss xmm0,[ecx+0E8h]
            static const unsigned char kAddEnergy[] = {0x8A, 0x81, 0xC4, 0x00, 0x00, 0x00, 0xA8, 0x02, 0x75, 0x36, 0x0F, 0x57, 0xC0, 0x0F, 0x2F, 0x81, 0xE8, 0x00, 0x00, 0x00};
            unsigned char health[sizeof(kGetHealth)]{}, energy[sizeof(kAddEnergy)]{};
            bool healthOk = Read(base + kGetHealthRva, health, sizeof(health));
            for (size_t i = 0; healthOk && i < sizeof(kGetHealth); ++i) healthOk = (i >= 7 && i <= 10) || health[i] == kGetHealth[i];
            energyReady = healthOk && Read(base + kAddEnergyRva, energy, sizeof(energy)) && std::memcmp(energy, kAddEnergy, sizeof(energy)) == 0;
            mirrorModules = energyReady && GetPrivateProfileIntA("Bot", "MirrorModules", 0, ini.c_str()) != 0;
            std::fprintf(log, "bot mirror_modules=%d energy_calls=%d (each bot has kinesis and stasis exactly when the leader has them, its stasis energy "
                              "topped up when stasis arrives; the brain casts stasis only with energy for one cast)\n",
                         mirrorModules ? 1 : 0, energyReady ? 1 : 0);
        }
        {
            // Power node doors and stasis stations (runtime_coop_bot_usables.inc), after the energy calls.
            auto number = [](const char* text, float fallback, float lo, float hi) {
                const float value = text[0] ? static_cast<float>(std::atof(text)) : fallback;
                return std::isfinite(value) ? (value < lo ? lo : (value > hi ? hi : value)) : fallback;
            };
            char text[32] = "";
            GetPrivateProfileStringA("Bot", "NodeDoorLeaderMeters", "", text, sizeof(text), ini.c_str());
            usableTune.doorLeaderMeters = number(text, 6.0f, 1.0f, 20.0f);
            GetPrivateProfileStringA("Bot", "NodeDoorWaitMs", "", text, sizeof(text), ini.c_str());
            usableTune.doorWaitMs = static_cast<uint32_t>(number(text, 1500.0f, 0.0f, 30000.0f));
            GetPrivateProfileStringA("Bot", "StasisStationBelow", "", text, sizeof(text), ini.c_str());
            usableTune.stationBelow = number(text, 50.0f, 1.0f, 100.0f) / 100.0f;
            GetPrivateProfileStringA("Bot", "StasisStationMeters", "", text, sizeof(text), ini.c_str());
            usableTune.stationMeters = number(text, 12.0f, 2.0f, 30.0f);
            InstallUsables(log, GetPrivateProfileIntA("Bot", "UseNodeDoors", 1, ini.c_str()) != 0,
                           GetPrivateProfileIntA("Bot", "UseStasisStations", 1, ini.c_str()) != 0);
        }
        {
            const bool wanted = GetPrivateProfileIntA("Bot", "ZeroGFlight", 0, ini.c_str()) != 0;
            zeroGFlight = wanted && bot_aim::enabled && bot_aim::floatLaid;
            std::fprintf(log, "bot zero_g_flight=%d (wanted=%d look_stub=%d: bots take off after the leader, fly behind him and land by him; "
                              "pad bit 0x4000 pulses, look through the zero-G stub)\n",
                         zeroGFlight ? 1 : 0, wanted ? 1 : 0, bot_aim::floatLaid ? 1 : 0);
            // and byte [ecx+7Ch],0FAh / movss xmm0,[ecx+70h] / or byte [ecx+7Ch],2 / xorps xmm1,xmm1
            static const unsigned char kRefill[] = {0x80, 0x61, 0x7C, 0xFA, 0xF3, 0x0F, 0x10, 0x41, 0x70, 0x80, 0x49, 0x7C, 0x02, 0x0F, 0x57, 0xC9};
            unsigned char actual[sizeof(kRefill)]{};
            const bool airWanted = GetPrivateProfileIntA("Bot", "ShareAir", 0, ini.c_str()) != 0;
            const bool refillOk = Read(base + kRefillMaxRva, actual, sizeof(actual)) && std::memcmp(actual, kRefill, sizeof(kRefill)) == 0;
            shareAir = airWanted && refillOk;
            std::fprintf(log, "bot share_air=%d (wanted=%d refill_signature=%d: a bot with less vacuum air than the leader is refilled)\n", shareAir ? 1 : 0,
                         airWanted ? 1 : 0, refillOk ? 1 : 0);
        }
        kinesisWanted = GetPrivateProfileIntA("Bot", "Kinesis", 0, ini.c_str()) != 0;
        sceneHide = GetPrivateProfileIntA("Bot", "HideInScenes", 0, ini.c_str()) != 0;
        hideKind[1] = GetPrivateProfileIntA("Bot", "HideInCinematics", 1, ini.c_str()) != 0;
        hideKind[2] = GetPrivateProfileIntA("Bot", "HideInScriptedEvents", 0, ini.c_str()) != 0;
        hideKind[3] = GetPrivateProfileIntA("Bot", "HideInPlayerActions", 0, ini.c_str()) != 0;
        hideKind[4] = GetPrivateProfileIntA("Bot", "HideInVisions", 0, ini.c_str()) != 0;
        hideKind[0] = GetPrivateProfileIntA("Bot", "HideInUnknownScenes", 0, ini.c_str()) != 0;
        std::fprintf(log, "bot hide_in_scenes=%d cinematics=%d events=%d actions=%d visions=%d unknown=%d (%zu player sequences known)\n",
                     sceneHide ? 1 : 0, hideKind[1] ? 1 : 0, hideKind[2] ? 1 : 0, hideKind[3] ? 1 : 0, hideKind[4] ? 1 : 0, hideKind[0] ? 1 : 0,
                     std::size(coop_scenes::kSequences));
        autoTeleport = GetPrivateProfileIntA("Bot", "AutoTeleport", 0, ini.c_str()) != 0;
        chaseFollow = GetPrivateProfileIntA("Bot", "ChaseFollow", 0, ini.c_str()) != 0;
        std::fprintf(log, "bot chase_follow=%d (in the straitjacket run the bots run right behind the leader, %.1f m and more apart; put by him after %.1f s at %.0f m)\n",
                     chaseFollow ? 1 : 0, 1.4f, kChaseFarOutMs / 1000.0f, kChaseFarOut);
        liftHoldMs = static_cast<DWORD>(GetPrivateProfileIntA("Bot", "LiftHoldMs", 6000, ini.c_str()));
        if (liftHoldMs > 30000) liftHoldMs = 30000;
        std::fprintf(log, "bot hide_in_scenes=%d auto_teleport=%d lift_hold_ms=%lu (hidden while the leader runs a scripted sequence; put by him after it, when far off or left outside his lift; "
                          "one put down in a lift cabin stands still that long)\n",
                     sceneHide ? 1 : 0, autoTeleport ? 1 : 0, static_cast<unsigned long>(liftHoldMs));
        survivalDistanceTeleport = GetPrivateProfileIntA("Bot", "SurvivalDistanceTeleport", 0, ini.c_str()) != 0;
        versusDeaths = GetPrivateProfileIntA("Bot", "VersusDeaths", 1, ini.c_str()) != 0;
        const int respawnMs = GetPrivateProfileIntA("Bot", "VersusRespawnMs", 5000, ini.c_str());
        versusRespawnMs = static_cast<DWORD>(respawnMs < 1000 ? 1000 : respawnMs > 60000 ? 60000 : respawnMs);
        std::fprintf(log, "bot survival_distance_teleport=%d (0: on the MP maps and arenas only a stuck bot is put by the leader) versus_deaths=%d respawn_ms=%lu\n",
                     survivalDistanceTeleport ? 1 : 0, versusDeaths ? 1 : 0, static_cast<unsigned long>(versusRespawnMs));
        moveGhostsOut = GetPrivateProfileIntA("Bot", "MoveGhostsOut", 1, ini.c_str()) != 0;
        leaderless = GetPrivateProfileIntA("Bot", "Leaderless", 1, ini.c_str()) != 0;
        std::fprintf(log, "bot move_ghosts_out=%d (a partner's ghost in a bot's Player slot is registered again from slot 4 up) leaderless=%d (the bots go on "
                          "while the leader is dead)\n",
                     moveGhostsOut ? 1 : 0, leaderless ? 1 : 0);
        InstallKinesis(log);
        if (GetPrivateProfileIntA("Bot", "AttackSlots", 1, ini.c_str()) != 0) InstallAttackPhase(log);
        else std::fprintf(log, "bot attack_phase=0 (AttackSlots=0: a bot keeps the phase -1, no enemy gets an attack slot on it)\n");
        camFadeFix =GetPrivateProfileIntA("Bot", "CameraFadeFix", 0, ini.c_str()) != 0;
        InstallCamFade(log);
        if (!renderGuard || !upgradeGuard) {
            std::fprintf(log, "bot: a required hook is missing (render=%d upgrades=%d); no bot\n", renderGuard ? 1 : 0, upgradeGuard ? 1 : 0);
            enabled = false;
            perSaveActive = false;
        }
        (void)nav;   // without NavPower the bot walks the leader's trail
    }
    for (unsigned i = 0; i < kMaxBots; ++i) units[i].state = enabled && i < wantedBots ? State::Waiting : State::Off;
    std::fprintf(log, "bot combat_follow=%.1f m rejoin=%.1f m (in a fight a bot walks back toward the leader only beyond this)\n", units[0].brain.tune.combatFollow,
                 units[0].brain.tune.combatRejoin);
    std::fprintf(log, "bot partner_pose=%d forward=%.2f right=%.2f (the first bot keeps this place by the resting leader and watches him)\n",
                 units[0].brain.tune.partnerPose ? 1 : 0, units[0].brain.tune.partnerForward, units[0].brain.tune.partnerRight);
    std::fprintf(log, "bot enabled=%d count=%u key=%d call_key=%d spawn_delay_ms=%lu heal_below=%.0f%% pads=14..%u ids=1..%u (more Isaacs the game runs, on virtual pads)\n",
                 enabled ? 1 : 0, wantedBots, toggleKey, callKey, static_cast<unsigned long>(spawnDelayMs), u->brain.tune.healBelow * 100.0f,
                 15 - wantedBots, wantedBots);
}

void WriteDiagnostics(FILE* log) {
    std::fprintf(log, "bot necro_grabs hooked=%d struggles=%ld updates=%ld own_counts=%ld presses=%ld breaks=%ld ghost_skips=%ld faults=%ld\n", pacHooked ? 1 : 0,
                 InterlockedCompareExchange(&pacStruggles, 0, 0), InterlockedCompareExchange(&pacUpdates, 0, 0), InterlockedCompareExchange(&pacOwnCounts, 0, 0),
                 InterlockedCompareExchange(&pacPresses, 0, 0), InterlockedCompareExchange(&pacBreaks, 0, 0), InterlockedCompareExchange(&pacGhostSkips, 0, 0),
                 InterlockedCompareExchange(&pacFaults, 0, 0));
    std::fprintf(log, "bot ghosts floor_catches=%ld death_messages_swallowed=%ld mortal=%d changes=%ld zero_health_bodies=%ld (the host's bots as ghosts here keep their "
                      "life unless mortal)\n",
                 InterlockedCompareExchange(&ghostFloorCatches, 0, 0), InterlockedCompareExchange(&ghostDeathMessages, 0, 0), mortalState,
                 InterlockedCompareExchange(&mortalChanges, 0, 0), InterlockedCompareExchange(&zeroHealthBodies, 0, 0));
    if (!enabled) return;
    bot_aim::WriteDiagnostics(log);
    WriteTubeDiagnostics(log);
    WriteUsableDiagnostics(log);
    bot_wheel::WriteDiagnostics(log);
    bot_light::WriteDiagnostics(log);
    WriteFaceDiagnostics(log);
    WriteAnticDiagnostics(log);
    if (chaseFollow) {
        static LONG lastChase = -1;
        if (chaseStarts + chaseTeleports != lastChase) {
            lastChase = chaseStarts + chaseTeleports;
            std::fprintf(log, "bot chase starts=%ld teleports=%ld active=%d\n", chaseStarts, chaseTeleports, leaderChase ? 1 : 0);
        }
    }
    {
        // Its own line: it used to print only with ChaseFollow on and a chase count changed.
        static LONG lastMortal = -1;
        const LONG mortal = distanceTeleportsSkipped + versusBotDeaths + versusBotRespawns + versusSpawnPoints + humanFriendlyBlocked + bot_sense::necroTargetsSeen;
        if (mortal != lastMortal) {
            lastMortal = mortal;
            std::fprintf(log, "bot survival maps: distance catch-ups skipped=%ld; mortal deaths=%ld respawns=%ld at_spawn_points=%ld human_hits_refused=%ld; "
                              "necro targets seen=%ld\n",
                         distanceTeleportsSkipped, versusBotDeaths, versusBotRespawns, versusSpawnPoints, humanFriendlyBlocked, bot_sense::necroTargetsSeen);
        }
    }
    {
        static LONG lastSeats = -1;
        const LONG seats = ghostsMovedOut + ghostMoveRefused + ghostMoveFaults + leaderlessEpisodes + seatsGiven + slotBusy;
        if (seats != lastSeats) {
            lastSeats = seats;
            std::fprintf(log, "bot seats: given to humans=%ld slot busy=%ld | ghosts moved out of bot slots=%ld refused=%ld faults=%ld | leaderless stretches=%ld "
                              "frames=%ld\n",
                         seatsGiven, slotBusy, ghostsMovedOut, ghostMoveRefused, ghostMoveFaults, leaderlessEpisodes, leaderlessFrames);
        }
    }
    {
        static LONG lastDrift = -1;
        if (driftFrames + noPairedRestored + npcs::nicoleBotRefused != lastDrift) {
            lastDrift = driftFrames + noPairedRestored + npcs::nicoleBotRefused;
            std::fprintf(log, "bot drift frames=%ld with_leader=%ld with_direction=%ld (moved with its stick still) no_paired_restored=%ld nicole_bot_refused=%ld "
                              "dead_targets_dropped=%ld\n",
                         driftFrames, driftWithLeader, driftWithDirection, noPairedRestored, npcs::nicoleBotRefused, deadTargetsDropped);
        }
    }
    if (ownMovement) {
        static LONG lastMove = -1;
        const LONG m = moveSwaps[0] + moveSwaps[1] + moveSwaps[2] + moveSwaps[3] + moveSwaps[4] + moveEnableBot[0] + moveEnableBot[1];
        if (m != lastMove) {
            lastMove = m;
            std::fprintf(log, "bot own_movement swaps aim=%ld enable=%ld ctor_881330=%ld ctor_881C30=%ld other=%ld (last other caller %08X) own_calls=%ld "
                              "bot_enable on=%ld off=%ld\n",
                         moveSwaps[0], moveSwaps[1], moveSwaps[2], moveSwaps[3], moveSwaps[4], static_cast<unsigned>(moveOtherCaller ? base + moveOtherCaller : 0),
                         moveOwnCalls, moveEnableBot[1], moveEnableBot[0]);
        }
    }
    if (altTrace) {
        static LONG lastAlt = -1;
        const LONG a = altPresses + altLogged + altGatePasses + contextByBot;
        if (a != lastAlt) {
            lastAlt = a;
            std::fprintf(log, "bot alt_fire presses=%ld logged=%ld gate_passes=%ld context_by_bot=%ld\n", altPresses, altLogged, altGatePasses, contextByBot);
        }
    }
    {
        static LONG lastBodies = -1;
        const LONG b = isaacRepairs + isaacOwnChanges + isaacSnapRefreshes + isaacWaitsTotal;
        if (b != lastBodies) {
            lastBodies = b;
            std::fprintf(log, "bot isaac_body thefts=%ld own_changes=%ld snapshot_refreshes=%ld streaming_waits=%ld out=%d%d%d\n", isaacRepairs, isaacOwnChanges,
                         isaacSnapRefreshes, isaacWaitsTotal, units[0].failedForGood ? 1 : 0, units[1].failedForGood ? 1 : 0, units[2].failedForGood ? 1 : 0);
        }
    }
    if (storeRooms) {
        static LONG lastRooms = -1;
        const LONG r = roomEntries + roomExits + roomVisits + instantStores + instantBenches + roomScanFaults;
        if (r != lastRooms) {
            lastRooms = r;
            std::fprintf(log, "bot store_rooms entries=%ld exits=%ld room_visits=%ld instant_stores=%ld instant_benches=%ld scan_faults=%ld list_layout=%d "
                              "in=%d store=%08X bench=%08X turns=%X/%X\n",
                         roomEntries, roomExits, roomVisits, instantStores, instantBenches, roomScanFaults, usableLayout, room.in ? 1 : 0, room.store, room.bench,
                         room.storeDone, room.benchDone);
        }
    }
    if (energyReady) {
        static LONG lastModules = -1;
        const LONG m = moduleChanges + energyTopUps + moduleFaults;
        if (m != lastModules) {
            lastModules = m;
            std::fprintf(log, "bot modules changes=%ld energy_top_ups=%ld faults=%ld tk=%d,%d,%d stasis01=%.2f,%.2f,%.2f\n", moduleChanges, energyTopUps,
                         moduleFaults, units[0].tkModule ? 1 : 0, units[1].tkModule ? 1 : 0, units[2].tkModule ? 1 : 0, units[0].stasis01, units[1].stasis01,
                         units[2].stasis01);
        }
    }
    if (zeroGFlight || shareAir) {
        static LONG lastZg = -1;
        const LONG z = zgTakeoffs + zgSurfaceTakeoffs + zgLandPulses + zgLandings + zgBackToFloat + zgNoSpot + zgTeleports + zgSignFlips + zgFlyingFrames / 256 +
                       zgUnknownBody + airRefills + airFaults + zgFightFrames / 256;
        if (z != lastZg) {
            lastZg = z;
            std::fprintf(log, "bot zero_g takeoffs=%ld surface_takeoffs=%ld land_pulses=%ld landings=%ld back_to_float=%ld no_spot=%ld teleports=%ld "
                              "sign_flips=%ld flying_frames=%ld trail_frames=%ld unknown_body=%ld signs=%+.0f/%+.0f,%+.0f/%+.0f,%+.0f/%+.0f "
                              "air_refills=%ld air_faults=%ld aim_frames=%ld fight_frames=%ld fight_on_target=%ld\n",
                         zgTakeoffs, zgSurfaceTakeoffs, zgLandPulses, zgLandings, zgBackToFloat, zgNoSpot, zgTeleports, zgSignFlips, zgFlyingFrames,
                         zgTrailFrames, zgUnknownBody, units[0].flightYawSign, units[0].flightPitchSign, units[1].flightYawSign, units[1].flightPitchSign,
                         units[2].flightYawSign, units[2].flightPitchSign, airRefills, airFaults, zgAimFrames, zgFightFrames, zgFightOnTarget);
        }
    }
    if (passThrough) {
        static LONG lastApart = -1;
        const LONG a = apartAdds + apartRemoves + apartFull + apartLost + apartFaults;
        if (a != lastApart) {
            lastApart = a;
            std::fprintf(log, "bot players_apart adds=%ld removes=%ld full=%ld lost=%ld faults=%ld\n", apartAdds, apartRemoves, apartFull, apartLost, apartFaults);
        }
    }
    if (attackPhaseReady) {
        static LONG lastPhase = -1;
        if (attackPhaseSets + attackPhaseRestored + attackPhaseFaults != lastPhase) {
            lastPhase = attackPhaseSets + attackPhaseRestored + attackPhaseFaults;
            std::fprintf(log, "bot attack_phase sets=%ld restored=%ld faults=%ld\n", attackPhaseSets, attackPhaseRestored, attackPhaseFaults);
        }
    }
    if (kinesisReady) {
        static LONG lastTk = -1;
        const LONG waits = tkWaitNoTarget + tkWaitRegrow + tkWaitRange + tkWaitCrowded + tkWaitLane + tkWaitBlast + tkWaitAim;
        const LONG k = tkStarts + tkThrows + tkForcedLaunches + tkLaunchOtherState + tkNoLaunch + waits / 64;
        if (k != lastTk) {
            lastTk = k;
            std::fprintf(log, "bot kinesis_throws starts=%ld grabs=%ld throws=%ld forced=%ld in_hold=%ld in_hold_to_release=%ld other_state=%ld no_launch=%ld "
                              "wait_frames no_target=%ld regrowing=%ld range=%ld crowded=%ld lane=%ld blast=%ld aim=%ld drops carry=%ld stuck=%ld snag=%ld "
                              "close=%ld lost=%ld throwable_walks=%ld\n",
                         tkStarts, tkGrabs, tkThrows, tkForcedLaunches, tkForcedHold, tkForcedRelease, tkLaunchOtherState, tkNoLaunch, tkWaitNoTarget, tkWaitRegrow,
                         tkWaitRange, tkWaitCrowded, tkWaitLane, tkWaitBlast, tkWaitAim, tkCarryDrops, tkStuckDrops, tkSnagDrops, tkCloseDrops, tkLost, tkWalks);
        }
    }
    {
        // The scans and the lift walk, every 10 s at most (they change every frame).
        static DWORD senseAt = 0;
        static LONG lastSense = -1;
        const DWORD now = GetTickCount();
        if (scanWalks != lastSense && now - senseAt >= 10000) {
            senseAt = now;
            lastSense = scanWalks;
            std::fprintf(log, "bot sense walks=%ld done=%ld dropped=%ld sights=%ld lag_avg_ms=%ld lag_max_ms=%ld zone_skips=%ld bone_exceptions=%ld bones_from_volume=%ld kind_aim_fallbacks=%ld cabin_walks=%ld "
                              "cabins=%u regen_hold_frames=%ld regen_reads flag_only=%ld machine_only=%ld both=%ld\n",
                         scanWalks, scanDone, scanDropped, scanSights, scanDone ? scanLagMsSum / scanDone : 0, scanLagMsMax, bot_sense::zoneSkips,
                         bot_sense::boneExceptions, bot_sense::bonesFromVolume, bot_sense::kindAimFallbacks, cabinWalks, cabinMoverCount, regenHolds, bot_sense::regenFlagOnly, bot_sense::regenSmOnly,
                         bot_sense::regenBoth);
        }
    }
    if (perSaveWanted) {
        static LONG lastSave = -1;
        const LONG v = snapDeath + snapLevel + snapOther + restoreFile + restoreSnap + restoreFresh + restoreUnknown + sidecarsWritten + sidecarFaults +
                       savesNotStarted + loadsMatched + loadsFresh + loadsRejected + loadsUnkeyed + sidecarsPruned;
        if (v != lastSave) {
            lastSave = v;
            std::fprintf(log, "bot per_save active=%d snapshots death=%ld level=%ld other=%ld restores file=%ld snapshot=%ld fresh=%ld unknown=%ld "
                              "sidecars written=%ld faults=%ld not_started=%ld pruned=%ld loads matched=%ld fresh=%ld rejected=%ld unkeyed=%ld\n",
                         perSaveActive ? 1 : 0, snapDeath, snapLevel, snapOther, restoreFile, restoreSnap, restoreFresh, restoreUnknown, sidecarsWritten,
                         sidecarFaults, savesNotStarted, sidecarsPruned, loadsMatched, loadsFresh, loadsRejected, loadsUnkeyed);
        }
    }
    if (storeReady && creditShare) {
        static LONG lastCredits = -1;
        const LONG v = leaderFinds + leaderFindsUnshared + leaderCreditDrops + creditsSeeded + creditsShared + creditsToLeader + creditsPicked + creditsSpent +
                       units[0].creditWallet + units[1].creditWallet + units[2].creditWallet + leaderCreditsNow;
        if (v != lastCredits) {
            lastCredits = v;
            std::fprintf(log, "bot credits leader=%ld finds=%ld unshared=%ld drops=%ld shared_each=%ld to_leader=%ld picked=%ld spent=%ld starting_stakes=%ld "
                              "wallets=%ld/%ld/%ld fresh=%d%d%d\n",
                         static_cast<long>(leaderCreditsNow), leaderFinds, leaderFindsUnshared, leaderCreditDrops, creditsShared, creditsToLeader, creditsPicked,
                         creditsSpent, creditsSeeded, static_cast<long>(units[0].creditWallet), static_cast<long>(units[1].creditWallet),
                         static_cast<long>(units[2].creditWallet), units[0].walletFresh ? 1 : 0, units[1].walletFresh ? 1 : 0, units[2].walletFresh ? 1 : 0);
        }
    }
    if (progressionWanted || modeLoadoutWanted) {
        static LONG lastProgress = -1;
        const LONG v = progressRuns + progressReached * 3 + progressStarted * 5 + progressArrived * 7 + progressRetries * 11 + progressGivenUp * 13 +
                       progressAlternatives * 17 + progressSuitStarts * 19 + progressSuitsInPlace * 23 + progressUnregistered * 29 + progressCutterTrims * 31 +
                       progressOwnedCleared * 37 + prun.completed * 41 + static_cast<LONG>(prun.clockMs / 60000u) * 43 + static_cast<LONG>(prun.kind) * 47;
        if (v != lastProgress) {
            lastProgress = v;
            std::fprintf(log, "bot progression mode=%s kind=%s run=%u state=%d wave=%d done_waves=%d clock=%us | due=%ld guns started=%ld arrived=%ld retries=%ld "
                              "given_up=%ld alternatives=%ld no_stream=%ld no_entry=%ld slot_full=%ld waits fight=%ld busy=%ld | suits started=%ld worn=%ld "
                              "unregistered=%ld | cutter_trims=%ld owned_cleared=%ld | done %X/%X/%X failed %X/%X/%X\n",
                         prun.mode, coop_bot_progress::KindName(prun.kind), prun.number, prun.state, prun.wave, prun.completed,
                         static_cast<unsigned>(prun.clockMs / 1000u), progressReached, progressStarted, progressArrived, progressRetries, progressGivenUp,
                         progressAlternatives, progressNoStream, progressNoEntry, progressSlotFull, progressFightWaits, progressBusyWaits, progressSuitStarts,
                         progressSuitsInPlace, progressUnregistered, progressCutterTrims, progressOwnedCleared, units[0].prog.done, units[1].prog.done,
                         units[2].prog.done, units[0].prog.failed, units[1].prog.failed, units[2].prog.failed);
        }
    }
    if (stompCorpses || lootContainers) {
        static LONG lastLoot = -1;
        const LONG v = lootGivenUp + corpsesNoted + cratesNoted + lockersNoted + corpseStompsGiven + crateStompsGiven + crateSwingsGiven + crateShotsGiven + lockerPresses + corpsesLooted + cratesBroken +
                       lockersOpened + lootItemsFlagged + containerWalks / 8;
        if (v != lastLoot) {
            lastLoot = v;
            std::fprintf(log, "bot loot visits=%ld given_up=%ld corpses seen=%ld stomps=%ld looted=%ld crates seen=%ld stomps=%ld swings=%ld shots=%ld (sight rays %ld, clear %ld) broken=%ld lockers seen=%ld presses=%ld opened=%ld "
                              "skips scripted=%ld locked=%ld no_drop_list=%ld items_flagged=%ld leader_items_skipped=%ld drops_by_bot_stomp=%ld "
                              "corpse_centers=%ld misses=%ld gap_max_cm=%ld container_walks=%ld containers=%u\n",
                         lootVisits, lootGivenUp, corpsesNoted, corpseStompsGiven, corpsesLooted, cratesNoted, crateStompsGiven, crateSwingsGiven, crateShotsGiven, crateSightRays, crateSightClear, cratesBroken, lockersNoted, lockerPresses,
                         lockersOpened, scriptedSkips, lockedSkips, noDropListSkips, lootItemsFlagged, bot_sense::leaderItemsSkipped, drops::botStompDrops,
                         bot_sense::corpseCenters, bot_sense::corpseCenterMisses, bot_sense::corpseGapMaxCm, containerWalks, containerCount);
        }
    }
    if (rescueLeader) {
        static LONG lastRescue = -1;
        const LONG v = rescues + rescueNoGrabber + rescueFrames / 32 + rescueInjected;
        if (v != lastRescue) {
            lastRescue = v;
            std::fprintf(log, "bot rescue grabs=%ld by_coordinator=%ld by_nearest=%ld coordinator_without_grab_state=%ld no_grabber=%ld frames=%ld attacks=%ld "
                              "injected=%ld close_ins=%lu (close_ins: frames run at a grabber out of sight)\n",
                         rescues, rescueByCoordinator, rescueByNearest, rescueCoordOnly, rescueNoGrabber, rescueFrames, rescueAttacks, rescueInjected,
                         static_cast<unsigned long>(units[0].brain.rescueCloseIns + units[1].brain.rescueCloseIns + units[2].brain.rescueCloseIns));
        }
    }
    static LONG lastHold = -1;
    const LONG holdActivity = holdsEntered + holdsReleased + holdsWithoutSignal + holdsTimedOut;
    if (holdActivity != lastHold) {
        lastHold = holdActivity;
        std::fprintf(log, "bot hold entered=%ld released_on_start=%ld released_without_signal=%ld timed_out=%ld frames=%ld active=%d (%s) level_starts=%ld\n",
                     holdsEntered, holdsReleased, holdsWithoutSignal, holdsTimedOut, holdFrames, holdActive ? 1 : 0, holdActive ? holdWhy : "-",
                     loadsync::levelStarts);
    }
    static LONG lastLying = -1;
    if (bot_sense::playdeadWakes * 100000 + bot_sense::playdeadSkips / 64 != lastLying) {
        lastLying = bot_sense::playdeadWakes * 100000 + bot_sense::playdeadSkips / 64;
        std::fprintf(log, "bot playdead skipped_scans=%ld got_up=%ld (a necromorph playing dead is a corpse to the bots until it gets up)\n",
                     bot_sense::playdeadSkips, bot_sense::playdeadWakes);
    }
    static LONG lastKeepOut = -1;
    if (keepOutGoals / 64 + keepOutWalks / 64 + keepOutLines != lastKeepOut) {
        lastKeepOut = keepOutGoals / 64 + keepOutWalks / 64 + keepOutLines;
        std::fprintf(log, "bot kiosk_keep_out metres=%.1f goals_moved=%ld walks_out=%ld (frames; bots kept off the store or bench the leader works)\n",
                     kioskKeepOut, keepOutGoals, keepOutWalks);
    }
    static LONG lastWait = -1;
    if (storeWaitArrivals + storeWaitFrames / 256 != lastWait) {
        lastWait = storeWaitArrivals + storeWaitFrames / 256;
        std::fprintf(log, "bot store_wait frames=%ld arrivals=%ld settled_short=%ld direct_frames=%ld aim_taps=%ld cam_pushes=%ld (bots in their waiting places while the leader shops)\n",
                     storeWaitFrames, storeWaitArrivals, storeWaitSettledShort, storeWaitDirectFrames, storeWaitAimTaps, storeWaitCamPushes);
    }
    static LONG lastNest = -1;
    if (bot_sense::nestSights + bot_sense::tentaclesSeen / 64 != lastNest) {
        lastNest = bot_sense::nestSights + bot_sense::tentaclesSeen / 64;
        unsigned long holds = 0;
        for (const Unit& one : units) holds += one.brain.nestHolds;
        std::fprintf(log, "bot nest seen=%ld sights=%ld with_pods=%ld holds=%lu guardian_tentacles_seen=%ld sights=%ld points=%ld no_ragdoll=%ld (holds: aimed at a nest or Guardian body with no point to shoot)\n",
                     bot_sense::nestsSeen, bot_sense::nestSights, bot_sense::nestPods, holds, bot_sense::tentaclesSeen, bot_sense::tentacleSights,
                     bot_sense::tentaclePoints, bot_sense::tentacleNoRagdoll);
    }
    static LONG lastLook = -1;
    if (lookTurns != lastLook) {
        lastLook = lookTurns;
        unsigned long leaderTooClose = 0;
        for (const Unit& one : units) leaderTooClose += one.brain.leaderLookSkips;
        std::fprintf(log, "bot look turns=%ld reversals=%ld close_holds=%ld leader_too_close=%lu (reversals near turns = a head swinging back and forth)\n",
                     lookTurns, lookReversals, lookCloseHolds, leaderTooClose);
    }
    {
        // The fight in numbers (07.10. changes): why shots wait, backing off, lanes, sight by body parts.
        unsigned long unseenSkips = 0, unseenFollows = 0, joins = 0, alertsUp = 0, aimWaits = 0, acquireHolds = 0, heldShots = 0, threatAims = 0, backOffs = 0, limbSwitches = 0, mateHolds = 0, mateSteps = 0;
        for (const Unit& one : units) {
            aimWaits += one.brain.aimWaits;
            acquireHolds += one.brain.acquireHolds;
            heldShots += one.brain.heldShots;
            threatAims += one.brain.threatAims; unseenSkips += one.brain.unseenLookSkips; unseenFollows += one.brain.unseenThreatFollows; joins += one.brain.joinFrames; alertsUp += one.brain.alertFrames;
            backOffs += one.brain.backOffs;
            limbSwitches += one.brain.limbSwitches;
            mateHolds += one.brain.mateLaneHolds;
            mateSteps += one.brain.mateLaneSteps;
        }
        static unsigned long lastFight = ~0ul;
        const unsigned long v = aimWaits / 64 + heldShots + threatAims / 64 + backOffs / 16 + mateSteps + static_cast<unsigned long>(bot_sense::partSights) +
                                static_cast<unsigned long>(bot_sense::losDisables + bot_sense::losReenables + levelResets) +
                                static_cast<unsigned long>(bot_sense::tentacleRefreshes / 64);
        if (v != lastFight) {
            lastFight = v;
            std::fprintf(log, "bot fight unseen_look_skips=%lu unseen_threat_follows=%lu join_frames=%lu alert_frames=%lu aim_waits=%lu acquire_holds=%lu held_shots=%lu threat_aims=%lu back_offs=%lu limb_switches=%lu mate_lane_holds=%lu "
                              "mate_lane_steps=%lu lead_frames=%ld lead_cm_avg=%ld part_tries=%ld part_sights=%ld part_rays=%ld tentacle_refreshes=%ld "
                              "los_disables=%ld los_reenables=%ld level_resets=%ld sense_forgets=%ld (frames unless named; held_shots: shots held back "
                              "after the choice, their cooldowns kept)\n",
                         unseenSkips, unseenFollows, joins, alertsUp, aimWaits, acquireHolds, heldShots, threatAims, backOffs, limbSwitches, mateHolds, mateSteps, leadFrames,
                         leadFrames ? leadCmSum / leadFrames : 0L, bot_sense::partTries, bot_sense::partSights, bot_sense::partRays,
                         bot_sense::tentacleRefreshes, bot_sense::losDisables, bot_sense::losReenables, levelResets, bot_sense::levelForgets);
        }
    }
    {
        // Human senses and aim, all bots together.
        unsigned long fovHidden = 0, heardTurns = 0, hitTurns = 0, acquisitions = 0, acquireMs = 0, overshoots = 0, undershoots = 0, settles = 0, settleMs = 0;
        for (const Unit& one : units) {
            fovHidden += one.brain.fovHidden; heardTurns += one.brain.heardTurns; hitTurns += one.brain.hitTurns;
            acquisitions += one.brain.acquisitions; acquireMs += one.brain.acquireMsSum;
            overshoots += one.brain.overshoots; undershoots += one.brain.undershoots;
            settles += one.brain.settles; settleMs += one.brain.settleMsSum;
        }
        static unsigned long lastSenses = ~0ul;
        const unsigned long v = fovHidden / 64 + heardTurns + hitTurns + acquisitions + settles + static_cast<unsigned long>(viewFromBody / 64);
        if (v != lastSenses) {
            lastSenses = v;
            std::fprintf(log, "bot senses fov_hidden=%lu heard_turns=%lu hit_turns=%lu view_from_body=%ld acquisitions=%lu acquire_ms_avg=%lu overshoots=%lu "
                              "undershoots=%lu settles=%lu settle_ms_avg=%lu (fov_hidden: frames x enemies with a clear ray outside the cone; acquire: "
                              "new target to first shot)\n",
                         fovHidden, heardTurns, hitTurns, viewFromBody, acquisitions, acquisitions ? acquireMs / acquisitions : 0ul, overshoots, undershoots,
                         settles, settles ? settleMs / settles : 0ul);
        }
    }
    {
        // Twitchers against plain slashers as targets, all bots together (frames).
        unsigned long tFrames = 0, tShots = 0, tSeen = 0, tFast = 0, tSnaps = 0, sFrames = 0, sShots = 0, sSeen = 0;
        unsigned long onTwitchers = 0, onStalkers = 0, noEnergy = 0, cooling = 0, playerNear = 0, aimFrames = 0, aimGiveUps = 0;
        for (const Unit& one : units) {
            tFrames += one.brain.dodgerFrames; tShots += one.brain.dodgerShots; tSeen += one.brain.dodgerSeenFrames;
            tFast += one.brain.dodgerFastFrames; tSnaps += one.brain.dodgerSnaps;
            sFrames += one.brain.slasherFrames; sShots += one.brain.slasherShots; sSeen += one.brain.slasherSeenFrames;
            onTwitchers += one.brain.stasisOnTwitchers; onStalkers += one.brain.stasisOnStalkers;
            noEnergy += one.brain.stasisNoEnergy; cooling += one.brain.stasisCooling; playerNear += one.brain.stasisPlayerNear;
            aimFrames += one.brain.stasisAimFrames; aimGiveUps += one.brain.stasisAimGiveUps;
        }
        static unsigned long lastTwitchers = ~0ul;
        const unsigned long v = tFrames / 32 + tShots + tSnaps + sFrames / 32 + sShots + onTwitchers + onStalkers + (noEnergy + cooling + playerNear) / 32 + aimFrames / 32 + aimGiveUps +
                                static_cast<unsigned long>(stasisSlowSeen / 64);
        if (v != lastTwitchers) {
            lastTwitchers = v;
            std::fprintf(log, "bot twitchers rows=%ld scanned=%ld targeted=%lu shots=%lu in_sight=%lu fast=%lu snaps=%lu | slashers targeted=%lu shots=%lu in_sight=%lu "
                              "(frames as the target; a twitcher dodges an aim after 0.25 s)\n",
                         bot_sense::twitcherRows, bot_sense::twitcherScans, tFrames, tShots, tSeen, tFast, tSnaps, sFrames, sShots, sSeen);
            std::fprintf(log, "bot stasis priority casts twitchers=%lu stalkers=%lu waits no_energy=%lu cooldown=%lu player_in_burst=%lu | all casts=%ld "
                              "slowed_seen=%ld aim_frames=%lu aim_given_up=%lu (waits: frames a twitcher or charging stalker could not get stasis; "
                              "given up: the aim never closed in 0.6 s, the gun for 4 s)\n",
                         onTwitchers, onStalkers, noEnergy, cooling, playerNear, stasisCasts, stasisSlowSeen, aimFrames, aimGiveUps);
        }
        unsigned long breakAway = 0, regenSkips = 0, desperateFrames = 0, desperateBlasts = 0, desperateHolds = 0, crowdStays = 0, kiteFrames = 0;
        for (const Unit& one : units) {
            breakAway += one.brain.breakAwayFrames;
            regenSkips += one.brain.regenSkips;
            desperateFrames += one.brain.desperateFrames;
            desperateBlasts += one.brain.desperateBlasts;
            desperateHolds += one.brain.desperateHumanHolds;
            crowdStays += one.brain.crowdStays;
            kiteFrames += one.brain.kiteFrames;
        }
        static unsigned long lastLeash = ~0ul;
        const unsigned long leash = breakAway / 32 + regenSkips / 32 + static_cast<unsigned long>(catchUpsHidden + catchUpsSeen * 3 + catchUpsOffMesh * 5) +
                                    desperateFrames / 32 + desperateBlasts * 7 + desperateHolds / 32 + static_cast<unsigned long>(desperateSelfSpared) * 11 + crowdStays / 32 + kiteFrames / 32;
        if (leash != lastLeash) {
            lastLeash = leash;
            std::fprintf(log, "bot leash run_back_frames=%lu kite_frames=%lu regen_target_skips=%lu | catch_up spots hidden=%ld in_view=%ld off_mesh_fallback=%ld | desperate frames=%lu blasts=%lu held_for_human=%lu self_spared=%ld | crowd_gun_kept=%lu\n",
                         breakAway, kiteFrames, regenSkips, catchUpsHidden, catchUpsSeen, catchUpsOffMesh, desperateFrames, desperateBlasts, desperateHolds, desperateSelfSpared, crowdStays);
        }
        static LONG lastGuard = -1;
        const LONG g = humanHitsRefused * 3 + humanHitsUnattributed * 5 + humanStasisRefused * 7;
        if (g != lastGuard) {
            lastGuard = g;
            std::fprintf(log, "bot human_guard on=%d hits_refused=%ld unattributed_after_bot_action=%ld stasis_dropped=%ld (no bot, bot ghost, bot gun or "
                              "bot-thrown object hurts a human's Isaac)\n",
                         noDamageToHumans ? 1 : 0, humanHitsRefused, humanHitsUnattributed, humanStasisRefused);
        }
    }
    {
        static LONG lastKeys = -1;
        const LONG k = keysKept + keysKeptBodiless * 3 + keysTakenByOther * 7;
        if (k != lastKeys) {
            lastKeys = k;
            std::fprintf(log, "bot keyboard keys_kept=%ld of them for a bot without a body=%ld | taken by a controller other than Isaac's=%ld\n", keysKept, keysKeptBodiless,
                         keysTakenByOther);
        }
    }
    static LONG last = -1;
    const LONG n = orderPresses[0] + orderPresses[1] + orderPresses[2] + orderPresses[3] + orderPresses[4] + orderResets + spawns + drops + shots + melees + stomps + heals + stasisCasts + pickups + reloads + teleports + deathsRefused + navFound + tkStarts + tkThrows;
    if (n == last) return;
    last = n;
    std::fprintf(log,
                 "bot spawns=%ld failures=%ld drops=%ld dressings=%ld isaac_repairs=%ld teleports=%ld deaths_refused=%ld damage_blocked=%ld shots=%ld melees=%ld "
                 "heals=%ld stasis=%ld pickups=%ld reloads=%ld nav_asked=%ld nav_found=%ld nav_queries=%ld nav_empty=%ld nav_faults=%ld shadow_ticks=%ld "
                 "upgrade_skips=%ld death_messages=%ld foreign_pad14=%ld keys_kept=%ld calls=%ld mask_syncs=%ld firing_fixes=%ld centered_bypasses=%ld gun_switches=%ld gun_wishes_dropped=%ld gun_faults=%ld alt_fires=%ld stomps=%ld stomps_lost=%ld unreachable=%ld los_casts=%ld los_blocked=%ld los_faults=%ld zone_reads=%ld guessed_reads=%ld bone_faults=%ld lane_holds=%ld limb_frames=%ld chest_frames=%ld limbs_kept=%ld head_skips=%ld hit_volumes=%ld volume_misses=%ld neutral_skips=%ld neutral_faults=%ld hidden_skips=%ld limbs_near=%ld limbs_far=%ld limbs_behind_body=%ld shot_rays=%ld shots_on_target=%ld shots_wide=%ld shot_miss_cm=%ld shot_parallax_cm=%ld turns_in_place=%ld bench_visits=%ld nodes_bought=%ld nodes_picked=%ld gun_recalcs=%ld bench_faults=%ld store_visits=%ld purchases=%ld credits_spent=%ld credits_picked=%ld store_faults=%ld kiosk_visits=%ld kiosk_entries=%ld kiosk_exits=%ld kiosk_aborts=%ld cam_fade_dropped=%ld cam_fade_bot_self=%ld cam_fade_passed=%ld gun_buys=%ld gun_reissues=%ld gun_arrivals=%ld gun_fails=%ld gun_refunds=%ld gun_swaps=%ld gun_timeouts=%ld gun_fallbacks=%ld mate_hits_blocked=%ld health_floor_catches=%ld guns_trimmed=%ld trim_refused=%ld cutter_grants=%ld trim_switches=%ld credits_shared=%ld credits_to_leader=%ld stream_raises=%ld busy_holds=%ld progress_resets=%ld suit_picks=%ld suit_urgent_changes=%ld suit_store_changes=%ld suit_none_owned=%ld heals_held_back=%ld prompts_skipped=%ld kiosk_fallbacks=%ld context_kept=%ld scene_hides=%ld scene_shows=%ld scene_catch_ups=%ld far_teleports=%ld cabin_misses=%ld cabin_teleports=%ld lift_holds=%ld lift_hold_frames=%ld context_skips=%ld rumble_guards=%ld suit_changes=%ld suit_failures=%ld bot_deaths=%ld restarts_refused=%ld player_stasis_cuts=%ld struggles=%ld mash_frames=%ld combat_move_frames=other:%ld,follow:%ld,retreat:%ld,chase:%ld,relocate:%ld,step:%ld,lane:%ld tk_starts=%ld tk_grabs=%ld tk_throws=%ld tk_misses=%ld tk_lost=%ld tk_no_launch=%ld tk_aborts=%ld tk_redirects=%ld tk_medpacks_lost=%ld tk_scan_faults=%ld tk_throwables=%d alerts=%ld tk_rolls_yes=%ld tk_rolls_no=%ld tk_calm_drops=%ld tk_close_drops=%ld tk_heal_drops=%ld tk_snag_drops=%ld tk_max_lag_cm=%ld tk_throw_tries=%ld tk_forced_launches=%ld tk_carry_drops=%ld tk_stuck_drops=%ld muzzle_notes=%ld ammo_top_ups=%ld ammo_gifts=%ld clip_refills=%ld orders=%ld,%ld,%ld,%ld,%ld orders_ignored=%ld go_there_fails=%ld go_there_nearest=%ld hold_arrivals=%ld hold_spots_moved=%ld hold_teleport_skips=%ld hold_catch_up_skips=%ld target_misses=%ld target_kills=%ld target_expiries=%ld target_losses=%ld focus_frames=%ld fire_held_frames=%ld return_fire_frames=%ld order_resets=%ld stance=%s hold_fire=%d vanguard=%d entities=%08X,%08X,%08X states=%d,%d,%d\n",
                 spawns, spawnFailures, drops, dressings, isaacRepairs, teleports, deathsRefused, damageBlocked, shots, melees, heals, stasisCasts, pickups,
                 reloads, navAsked, navFound, bot_nav::queries, bot_nav::empty, bot_nav::faults, shadowTicks, upgradeSkips, deathMessages, foreignPad14, keysKept, calls, sceneMaskSyncs, firingFixes, centeredBypasses, gunSwitches, gunWishesDropped, gunFaults, altFires, stomps, stompsLost, unreachable, bot_sense::losCasts, bot_sense::losBlocked,
                 bot_sense::losFaults, bot_sense::zoneReads, bot_sense::guessedReads, bot_sense::boneFaults, laneHolds, limbShots, chestShots, bot_sense::limbsKept, bot_sense::headSkips, bot_sense::volumeReads, bot_sense::volumeMisses, bot_sense::neutralSkips, bot_sense::neutralFaults, bot_sense::hiddenSkips, bot_sense::limbsNear, bot_sense::limbsFar, bot_sense::limbsBehindBody, shotRays, shotsOnTarget, shotsWide, shotMissCm, shotParallaxCm, turnsInPlace, benchVisits, nodesBought, nodesPicked, gunRecalcs, benchFaults, storeVisits, purchases, creditsSpent, creditsPicked, storeFaults, kioskVisits, kioskEntries, kioskExits, kioskAborts, camFadeDropped, camFadeBotSelf, camFadePassed, gunBuys, gunReissues, gunArrivals, gunFails, gunRefunds, gunSwaps, gunTimeouts, gunFallbacks, botFriendlyBlocked, healthFloorCatches, gunsTrimmed, gunTrimRefused, cutterGrants, trimSwitches, creditsShared, creditsToLeader, streamRaises, busyHolds, progressResets, suitPicks, suitUrgentChanges, suitStoreChanges, suitNoneOwned, healsHeldBack, promptsSkipped, kioskFallbacks, contextKept, sceneHides, sceneShows, sceneCatchUps, farTeleports, cabinMisses, cabinTeleports, liftHolds, liftHoldFrames, contextSkips, rumbleGuards, suitChanges, suitFailures, botDeaths, checkpoint::botRestartsRefused, playerStasisCuts, struggles, mashFrames, combatMoves[0], combatMoves[1], combatMoves[2], combatMoves[3], combatMoves[4], combatMoves[5], combatMoves[6], tkStarts, tkGrabs, tkThrows, tkMisses, tkLost, tkNoLaunch, tkAborts, tkRedirects, tkMedpacksLost, tkScanFaults, throwableCount, alerts, tkRollsYes, tkRollsNo, tkCalmDrops, tkCloseDrops, tkHealDrops, tkSnagDrops, tkMaxLagCm, tkFirePulses, tkForcedLaunches, tkCarryDrops, tkStuckDrops, muzzleNotes, ammoTopUps, ammoGifts, clipRefills, orderPresses[0], orderPresses[1], orderPresses[2], orderPresses[3], orderPresses[4], ordersIgnored, goThereFails, goThereNearest, holdArrivals, holdSpotsMoved, holdTeleportsSkipped, holdCatchUpsSkipped, focusMisses, focusKills, focusExpiries, focusLosses, focusFrames, fireHeldFrames, returnFireFrames, orderResets, StanceName(), holdFire ? 1 : 0, vanguard ? 1 : 0, units[0].entity, units[1].entity, units[2].entity,
                 static_cast<int>(units[0].state), static_cast<int>(units[1].state), static_cast<int>(units[2].state));
}
}  // namespace bot
