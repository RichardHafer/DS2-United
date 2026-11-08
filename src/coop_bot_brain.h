// Dead Space 2 United by Richard Hafer.
// The companion bot's decisions, without the game.
//
// The runtime (runtime_coop_bot.inc) fills a Perception every tick from the
// game (the bot's own Isaac, the players it follows, enemies with the points
// on their limbs, pickups, a progression point the group is heading for) and
// turns the Command it gets back into movement, look direction and buttons.
// Everything here is plain arithmetic on that snapshot, so it is tested
// offline (tests/coop_bot_brain_tests.cpp).
#pragma once
#include <cmath>
#include <cstdint>

namespace coop_bot {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
inline Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 Scale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline float Distance(Vec3 a, Vec3 b) { return Length(Sub(a, b)); }
// Distance on the ground plane (the game is Y-up).
inline float Flat(Vec3 a, Vec3 b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}
// Yaw in radians with forward = (sin, 0, cos), the Player's convention.
inline Vec3 FromYaw(float yaw) { return {std::sin(yaw), 0.0f, std::cos(yaw)}; }

constexpr int kMaxEnemies = 16, kMaxLimbs = 4, kMaxItems = 16, kGunSlots = 4, kMaxPlayers = 8, kMaxMates = 3, kMaxLoot = 12;

enum class LimbKind : uint8_t { Arm, Leg };
enum class Gun : uint8_t { None, PlasmaCutter, LineGun, Javelin, PulseRifle, Ripper, Detonator, SeekerRifle, Flamethrower, ForceGun, ContactBeam, Other };

// One weapon slot of the bot's Isaac. Rounds -1 = unknown.
struct GunSlot {
    Gun kind = Gun::None;
    int clip = -1, reserve = -1;
    bool Usable() const { return kind != Gun::None && (clip != 0 || reserve != 0); }
};

// The kind decides how many bots may shoot one enemy at a time (FocusCap).
enum class EnemyKind : uint8_t { Unknown, Slasher, Pack, Swarm, TubeBeetle, Leaper, Lurker, Exploder, Infector, Puker,
                                 Stalker, Brute, Hunter, DividerCore, DividerPart, Guardian, Boss, Nest, GuardianTentacle };
struct Enemy {
    uint32_t id = 0;
    EnemyKind kind = EnemyKind::Unknown;
    Vec3 pos;                  // feet
    Vec3 vel;                  // metres per second, flat, from the last scans
    Vec3 chest;                // where stasis and the fallback shot go
    Vec3 limbs[kMaxLimbs];     // arms and legs still attached, best first
    LimbKind limbKind[kMaxLimbs] = {LimbKind::Arm, LimbKind::Arm, LimbKind::Leg, LimbKind::Leg};
    int limbCount = 0;
    float health = 0.0f, maxHealth = 0.0f;
    bool stasised = false;     // already slowed
    bool visible = true;       // line of sight from the bot, when the runtime knows
    bool inLine = true;        // the clear ray alone; with human senses visible also needs the view cone (the brain sets both)
    int limbsLost = -1;        // arms and legs shot off (0..4), -1 = unknown
    float height = 0.0f;       // feet to the highest bone, 0 = unknown
    uint32_t victim = 0;       // the entity it goes for, 0 = none or unknown
    bool objective = false;    // not an enemy but the thing the fight is about (the Marker's heart): first, and from farther
    bool regenerating = false; // down and regrowing its limbs (the Ubermorph): aimed at, never shot, others first
    bool dodger = false;       // dodges an aim within a quarter second (the Severed twitcher)
};

// Another bot of the squad, as it stood on its last tick.
struct Mate {
    uint32_t id = 0;
    Vec3 pos;
    Vec3 goal;                 // where it walks, when moving
    bool moving = false;
    uint32_t target = 0;       // the enemy it fights
    uint32_t item = 0;         // the item it goes for
    uint32_t loot = 0;         // the corpse or container it loots
    bool antic = false;        // runs an antic: only one bot of the squad at a time
    bool aiming = false;       // gun up in a fight: its shot line runs from its eye to aimAt
    Vec3 aimAt;
};

struct Item {
    uint32_t id = 0;
    Vec3 pos;
    bool botLoot = false;      // dropped by a corpse or container a bot just worked: the bots may take it beside the leader
};

// Something to loot when no fight is on: a fresh corpse or a supply crate to
// stomp, or a locker to open with use.
enum class LootKind : uint8_t { Corpse, Crate, Locker };
struct LootSpot {
    uint32_t id = 0;
    LootKind kind = LootKind::Corpse;
    Vec3 pos;                  // where to stomp (the corpse's middle) or stand
    int stomps = 0;            // stomps (or swings, shots) the squad already gave it
    bool botKill = false;      // a bot dealt its last blow
    bool inSight = false;      // a crate the bot looking at it has a clear shot at
};

// The leader's orders to the squad ([Bot] order keys, runtime_coop_bot.inc).
struct Orders {
    bool hold = false;          // keep holdSpot instead of following the leader (Hold, Go there)
    Vec3 holdSpot;
    uint32_t focus = 0;         // the enemy to fight first, until focusUntil
    uint32_t focusUntil = 0;
    bool holdFire = false;      // no attacks unless something attacks the bot or the leader
    bool vanguard = false;      // attack formation: ahead of the leader, closer and pushier
};

struct Perception {
    uint32_t now = 0;
    Orders orders;
    float leaderHealth01 = 1.0f;
    bool leaderGrabbed = false;
    uint32_t leaderGrabber = 0;   // the enemy holding the leader, when known: every bot in reach goes for it
    // The bot's Isaac.
    Vec3 self;
    float selfYaw = 0.0f;        // where the body faces (atan2(x, z)); it only turns while walking forward
    bool selfYawKnown = false;
    float viewYaw = 0.0f;        // where its eyes point (the camera, or the gun while aiming), atan2(x, z)
    bool viewYawKnown = false;
    float health01 = 1.0f;       // current / max
    int medpacks = -1;           // -1 = unknown
    float stasis01 = 1.0f;       // stasis energy / max
    int clip = -1, reserve = -1; // plasma cutter rounds, -1 = unknown
    bool reloading = false;
    bool pickupInReach = false;  // the game offers the bot's Isaac a pickup right now
    bool grabbed = false;        // an enemy holds the bot (button mashing)
    bool chase = false;          // the leader flees (the straitjacket run): right behind him, running
    // The player it keeps close to.
    Vec3 leader;
    float leaderYaw = 0.0f;
    bool leaderMoving = false;   // walked more than a little in the last seconds
    bool leaderShooting = false; // the human fired or swung in the last seconds
    Vec3 leaderVel;              // metres per second, flat, over the last half second
    // Somewhere the level goes on (a door, a lift, a checkpoint) the group waits at.
    bool progression = false;
    Vec3 progressionAt;
    Enemy enemies[kMaxEnemies];
    int enemyCount = 0;
    Item items[kMaxItems];
    int itemCount = 0;
    LootSpot loot[kMaxLoot];
    int lootCount = 0;
    uint32_t lootOffered = 0;    // the container the game offers the bot's Isaac to use right now
    float aimErrorDeg = 180.0f;  // how far the current aim is off the last look target
    // Weapons. All None: the runtime does not know them, the bot fights with what it holds.
    GunSlot guns[kGunSlots];
    int activeSlot = -1;         // the slot in hand, -1 = unknown or none
    bool switching = false;      // a weapon change is under way
    int cutterHorizontal = -1;   // plasma cutter blade: 1 horizontal, 0 vertical, -1 unknown
    bool cutterTurning = false;  // the blade is still turning (0.2 s)
    uint32_t spearHitAt = 0;     // when the bot's newest javelin spear stuck in an enemy (0 = none)
    uint32_t mineFuseMs = 0;     // the line gun mine's fuse, 0 = unknown
    int seekerZoomed = -1;       // seeker rifle scope: 1 on, 0 off, -1 unknown
    bool ripperBladeOut = false; // a ripper blade hangs in front of the bot
    int detonatorMines = -1;     // detonator mines in the level (everyone's), -1 unknown
    int contactReady = -1;       // contact beam fully charged: 1, charging or empty: 0, -1 unknown
    bool gunsKnown = false;      // the weapon slots were read (all empty then means no gun at all)
    // The other players (the leader is always counted too): no stasis and nothing
    // that bursts where one of them stands.
    Vec3 players[kMaxPlayers];
    int playerCount = 0;
    // The humans among them (not the bots), the leader too: a desperate blast holds for these only.
    Vec3 humans[kMaxPlayers];
    int humanCount = 0;
    // The squad: this bot, the leader's entity, and the other bots.
    uint32_t selfId = 0, leaderId = 0;
    int squadIndex = 0, squadSize = 1;
    Mate mates[kMaxMates];
    int mateCount = 0;
    // Antics. busy: the runtime has the bot in something of its own (a scene,
    // a kiosk, zero-G, a wall turn...), the reason; no antic then.
    const char* busy = nullptr;
    uint32_t killId = 0, killAt = 0;   // the last enemy this bot itself killed, and when
    Vec3 killPos;                      // where its corpse lies
    int leaderLine = -1;               // a clear line from the bot's eye to the leader's chest: 1 yes, 0 no, -1 unknown
    uint32_t squadPrankAt = 0;         // when any bot of the squad last started a stasis prank (0 = never)
};

// Antics: a playful moment of a bot now and then, only when all is calm
// (DS2BotProfiles.ini Antics, HarmfulAntics; runtime_coop_bot_antics.inc).
enum class Antic : uint8_t { None, AimSpam, Stare, VictoryStomp, ShadowBoxing, StasisPrank, Count };
inline const char* AnticName(Antic a) {
    switch (a) {
    case Antic::AimSpam: return "aim_spam";
    case Antic::Stare: return "stare";
    case Antic::VictoryStomp: return "victory_stomp";
    case Antic::ShadowBoxing: return "shadow_boxing";
    case Antic::StasisPrank: return "stasis_prank";
    default: return "none";
    }
}
// Why a due antic, or one kind of it, did not start.
enum class AnticSkip : uint8_t { Cooldown, NotCalm, MateBusy, NoKill, NoStasis, NoLine, LeaderClose, HoldFire, NoHeading, Count };
inline const char* AnticSkipName(AnticSkip s) {
    static const char* const kNames[] = {"cooldown", "not_calm", "mate_busy", "no_kill", "no_stasis", "no_line", "leader_close", "hold_fire", "no_heading"};
    return static_cast<unsigned>(s) < static_cast<unsigned>(AnticSkip::Count) ? kNames[static_cast<unsigned>(s)] : "?";
}
constexpr int kAntics = static_cast<int>(Antic::Count), kAnticSkips = static_cast<int>(AnticSkip::Count);
struct AnticEvent {
    enum Kind : uint8_t { Start, Finish, Abort };
    Antic antic = Antic::None;
    Kind kind = Start;
    const char* why = "";
    uint32_t ms = 0;            // how long it ran
    uint32_t eligible = 0;      // start: the kinds it could have picked (bit per Antic)
    int taps = 0, stomps = 0, swings = 0, swingsSkipped = 0, spins = 0;
    bool stasisFired = false;
};
struct AnticStats {
    uint32_t started[kAntics]{}, finished[kAntics]{}, aborted[kAntics]{};
    uint32_t rejected[kAnticSkips]{};
    uint32_t taps = 0, stomps = 0, swings = 0, swingsSkipped = 0, stasisShots = 0;
};

enum class Mode : uint8_t { Idle, Follow, Rush, Combat, Pickup, Heal, Loot };

// Why the bot walks this frame in a fight (diagnostics only).
enum class MoveWhy : uint8_t { None, Follow, Retreat, Chase, Relocate, Step, Lane, Count };
struct Command {
    Mode mode = Mode::Idle;
    bool move = false;
    Vec3 moveTo;
    bool run = false;
    bool look = false;
    Vec3 lookAt;
    bool aim = false;      // hold lock_on
    bool fire = false;     // press use_item while aiming
    bool melee = false;    // press use_item without aiming
    bool reload = false;
    bool stasis = false;
    bool heal = false;
    bool use = false;      // pick up / use
    bool mash = false;     // shake off a grab
    bool altFire = false;  // the weapon's second function (press it while aiming)
    bool holdFire = false; // keep the fire button down this frame (automatic fire, charging)
    bool holdAlt = false;  // keep the alt fire button down this frame (charged alt fire)
    bool stomp = false;    // press the alt fire button without aiming
    int slot = -1;         // take the gun in this slot
    uint32_t target = 0;
    bool laneBlocked = false;  // a shot held back: a player stood in the line of fire
    bool desperate = false;    // a crowd-clearing shot that ignores the mates (the runtime spares the bots its blast)
    bool regenHold = false;    // a shot held back: the target lies regrowing its limbs
    bool shotAtLimb = false, shotAtChest = false;   // where this frame's shot goes (diagnostics)
    bool holdFireActive = false;   // hold fire in force and nothing provoked it: no kinesis throws either
    bool fireHeld = false;         // an attack this frame was dropped by hold fire
    bool returnFire = false;       // hold fire is on, but something attacks: fighting back
    bool focused = false;          // the target is the leader's forced one
    bool rescue = false;           // the target holds the leader
    uint32_t loot = 0;             // the loot spot this frame works on
    MoveWhy why = MoveWhy::None;
    Antic antic = Antic::None;     // the antic this frame belongs to
    bool direct = false;           // walk straight at moveTo, no route (a short turning step)
    Vec3 lookVel;                  // how fast lookAt itself moves (m/s), for the look to lead it
    bool backOff = false;          // backing off enemies close by (diagnostics)
};

struct Tuning {
    float engageRange = 22.0f;     // enemies farther than this are ignored
    float leash = 10.0f;           // never more than this from the leader while fighting
    // Where it stands in a fight: within combatRadius of the leader, on its side
    // of him and a step back from the enemy; it shoots from there (walking
    // backwards or sideways while aiming is fine) instead of chasing targets.
    float combatRadius = 6.0f, combatSide = 2.2f, combatBack = 1.0f, combatArrive = 1.0f, combatCrowd = 1.2f;
    float closeWeaponChase = 2.0f;  // a close-range weapon may take it this far beyond the radius
    float closeWeaponRange = 5.0f;  // a weapon that wants its target nearer than this is a close-range one
    float dangerPath = 2.0f;        // an enemy this close to the way there makes the way too dangerous
    // Its own pace in a fight (28.09.: the place followed the leader frame by frame,
    // so the bots moved exactly when he did): it reacts after a moment, keeps a
    // chosen place until it is there (or a while has passed), and now and then
    // takes a step aside of its own while it shoots.
    uint32_t combatReactMinMs = 400, combatReactMaxMs = 900, combatMoveMaxMs = 3000;
    uint32_t combatStepMinMs = 5000, combatStepMaxMs = 9000;
    float combatFollow = 6.0f;      // farther than this from the leader in a fight: back to him at once, shooting on the way
    float unseenLookRange = 0.5f;   // an unseen target farther than this is not looked at through the wall
    float combatRejoin = 3.5f;      // ... until this close to him again
    // Farther than this in a fight: gun down and run until back within combatFollow.
    // Aimed it only strafes; the leader's input used to drag it along in aim (the
    // engine bug fixed 06.10.), which hid how slow that was.
    float combatCatchUp = 10.0f;
    // Far behind in a fight, it comes back before anything else, even with an enemy on the
    // bot, unless one holds it (20.10., ch09 mining arena: Pack children kept the bots swinging
    // and stomping 40 m behind the leader).
    // Between combatCatchUp and kiteFar it comes back aimed: a step toward the leader and to a
    // side that changes every second or two, shooting all the while; beyond kiteFar it runs
    // with the gun down until kiteFar - kiteFarBand (user 20.10.: "shoot while running back,
    // backwards, sideways").
    float kiteFar = 22.0f, kiteFarBand = 6.0f, kiteStep = 3.0f, kiteSide = 1.4f;
    uint32_t kiteFlipMinMs = 900, kiteFlipMaxMs = 1800;
    uint32_t followHoldMs = 2500;   // the rejoin point is picked anew this often, not every frame
    // Enemies this close to the bot or the leader: a bot without a target of its own
    // holds where it stands instead of following him.
    float alertHoldRange = 12.0f;
    // Only enemies it knows of hold it: seen within threatMemoryMs, moving faster
    // than threatMoveSpeed within threatHearRange, or going for someone. A necromorph
    // playing dead behind a wall is none of these (06.10.: the bots aimed through
    // the wall at one and stood frozen).
    uint32_t threatMemoryMs = 6000;
    float threatHearRange = 12.0f, threatMoveSpeed = 0.6f;
    // Human senses ([Bot] HumanSenses): a clear ray is not enough, the enemy must be
    // inside the view cone or this close. One seen within trackMemoryMs stays tracked
    // outside the cone; one heard (moving, attacking, shot at) or one that hurt the
    // bot is turned to after its reaction, but only with a clear ray.
    bool humanSenses = false;
    float viewAngleDeg = 120.0f, peripheral = 2.0f;
    uint32_t trackMemoryMs = 3000;
    uint32_t hearReactMinMs = 250, hearReactMaxMs = 550;   // when the profile sets no reaction
    float hitRange = 4.0f;   // what hurt it: one going for it, else the nearest it knows of within this
    // Human aim ([Bot] HumanAim): a new target gets a short pause, then a flick that
    // lands off by an error which swings back past the target and settles (a
    // slightly underdamped spring); small wander while tracking, worse far off and
    // on fast movers; the lead is a little off. aimSkill 0..1 scales all of it.
    bool humanAim = false;
    float aimSkill = 0.85f;
    uint32_t aimDelayMinMs = 60, aimDelayMaxMs = 150;
    float flickErrorDeg = 2.5f, flickErrorPerDeg = 0.04f, flickErrorMaxDeg = 10.0f;
    float aimFreq = 18.0f, aimDamping = 0.55f;   // rad/s at full skill; below 1 it overshoots
    float aimSettleDeg = 0.6f;                   // the first shot once the error is this small
    uint32_t aimSettleMaxMs = 650;               // ... or after this long anyway
    float aimJitterDeg = 0.5f;
    uint32_t aimJitterMs = 250;
    float leadError = 0.25f;                     // share the lead may be off, per target
    // A dodger (the Severed twitcher) dodges 0.25 s into an aim, nine times in ten, and not
    // again for 1.5 to 3 s. Against it: no pause before the flick, no settling and a wider
    // shot cone, so the shots land in that pause ([Bot] TwitcherSnapShots).
    bool dodgerSnap = true;
    float dodgerTolerance = 1.6f;
    float leaderLookMin = 1.5f;   // closer than this, a standing bot does not look at the leader
    // The leader flees: the first bot this far behind him, each next one this much
    // farther, a little to alternate sides; "there" within chaseArrive.
    float chaseGap = 1.4f, chaseSpacing = 1.1f, chaseSide = 0.35f, chaseArrive = 0.4f;
    // The leader in its line of fire this long before it steps out of it (a leader
    // walking through the line moves nobody).
    uint32_t laneStepAfterMs = 1500;
    float followDanger = 1.2f;      // ... unless the way runs this close past an enemy
    float onUs = 2.5f;              // an enemy this close is on the bot anyway: no reason to stay
    float combatStepChance = 0.35f, combatStep = 1.3f;
    float keepMin = 2.8f, keepMax = 6.0f;
    float retreatStep = 2.0f;      // how far one step back goes
    // Backing off while it shoots: any enemy it knows of within backOffRange, or
    // crowdCount of them within crowdRange (07.10.: with many enemies it only ever
    // backed off its own target). Close-range weapons and stomps are exempt.
    float backOffRange = 3.0f, crowdRange = 4.5f;
    // The squad fights an enemy this bot cannot see: it comes over, by the mesh,
    // when that enemy is this close to the leader, and stops this far from it.
    float joinRange = 20.0f, joinStop = 4.0f;
    int crowdCount = 2;
    float backOffClear = 1.5f;     // the step back keeps this far from any other enemy
    float meleeRange = 1.3f;        // closer than this a swing beats a shot (packs come this close)
    uint32_t meleeMinMs = 1200, meleeMaxMs = 1800;
    // A swing goes where the body faces, and Isaac's body turns only while he
    // walks forward: it faces the enemy's middle first (a short step at it), swings,
    // and stands through the swing before it backs off (29.09.: backing off while
    // swinging turned it away, the swings went past).
    float meleeFaceDeg = 35.0f, meleeFaceStep = 0.4f;
    uint32_t meleeHoldMs = 650;
    // Small ones at its feet (a lurker, a crouched pack): a swing goes over them, a stomp does not.
    float stompReach = 1.4f, stompHeight = 1.2f, stompBelow = 0.8f, stompAbove = 0.4f;
    uint32_t stompIntervalMs = 900;
    float fireToleranceDeg = 3.0f;  // the widest a shot may be off; nearer targets allow it, far ones less
    float aimHalfWidth = 0.18f;     // metres a shot may pass beside the aim point and still hit a limb
    float chestHalfWidth = 0.25f;   // the same for a shot at the body
    float minToleranceDeg = 0.8f;
    float aimJump = 0.6f;           // the aim point moving farther than this at once is another point, not motion
    float aimLeadMaxSpeed = 8.0f;   // m/s: faster aim point motion is not believed
    float spearSpeed = 90.0f, ripperThrowSpeed = 35.0f;   // m/s: lead moving targets by the flight time
    uint32_t fireIntervalMs = 450;
    float healBelow = 0.6f;
    uint32_t healCooldownMs = 4000;
    float stasisMinEnergy = 0.25f;
    uint32_t stasisCooldownMs = 8000;
    float dangerousHealth = 200.0f; // max health from which an enemy counts as dangerous
    float immortalHealth = 50000.0f; // the Ubermorph (100000): it only ever loses limbs, which grow back
    int immortalLimbsOff = 3;        // an immortal with this many limbs gone is left alone until they regrow
    float immortalClose = 4.0f, immortalPenalty = 12.0f;   // beyond this from bot and leader, any other enemy comes first
    float stasisCloseRange = 3.5f;
    // Stasis first on twitchers and charging stalkers ([Bot] StasisPriority; user 18.10.:
    // "definitely more stasis"): within this range, with any energy for a cast, after this
    // cooldown, and such a target is picked this many metres earlier while the bot can cast.
    bool stasisPriority = true;
    float stasisPriorityRange = 14.0f, stalkerChargeSpeed = 2.5f, stasisPriorityBonus = 5.0f;
    uint32_t stasisPriorityCooldownMs = 2500;
    // A twitcher dodges the aim a stasis waits for: past this long on one target with the
    // aim never close enough, stasis leaves it alone for a while and the gun takes over.
    uint32_t stasisAimGiveUpMs = 600, stasisGiveUpMs = 4000;
    float followStart = 6.5f, followStop = 3.0f, runFrom = 4.0f;
    float idleMin = 2.0f, idleMax = 4.5f;
    uint32_t idleMoveMinMs = 10000, idleMoveMaxMs = 22000;
    float idleMoveChance = 0.35f;              // when the timer runs out: move on, or stay a while longer
    float arriveStart = 1.6f, arriveStop = 0.7f;  // walk off beyond the first, stop within the second
    uint32_t settleMinMs = 500, settleMaxMs = 3000;  // standing this long before it looks at anyone
    uint32_t lookMinMs = 2500, lookMaxMs = 6000;
    float glanceChance = 0.3f;                 // a look somewhere else instead of the leader
    float itemRange = 8.0f, itemLeash = 12.0f;
    float itemLeaderOwns = 4.0f;   // what lies this close to the leader is his to take (the loot he just stomped out)
    uint32_t pickupPulseMs = 350;
    uint32_t itemGiveUpMs = 6000;   // walking at one item this long without getting it: leave it
    uint32_t itemTries = 3;         // use presses in reach before it counts as not ours to take
    uint32_t itemIgnoreMs = 60000;  // how long a given-up item is left alone
    float itemLookRange = 2.0f;     // look at the item only this close; walking, eyes on the way
    // Weapons.
    float groupRadius = 3.5f;       // enemies this close to the target count as a group
    float lineLateral = 1.0f, lineReach = 14.0f;  // how wide and far the line gun's line counts
    uint32_t lineFireIntervalMs = 1100, javelinFireIntervalMs = 1300;
    float lineToleranceDeg = 4.0f;
    float mineMin = 5.0f, mineMax = 14.0f;
    uint32_t mineCooldownMs = 6000, mineFuseMs = 5000;   // fuse: 5 s, 2.5 s with the Duration upgrade
    float lineBoltSpeed = 30.0f;    // m/s: the line gun's line is not instant
    float mineLeadMax = 4.0f;       // never lead a mine further than this ahead of the group
    uint32_t shockWindowMs = 4000;  // the newest spear can shock until the next shot; give it this long
    float javelinMinRange = 3.5f;
    uint32_t rotateCooldownMs = 700;
    uint32_t gunHoldMs = 5000;      // a weapon choice stands at least this long
    uint32_t switchCooldownMs = 3000; // after a change, no other one this long unless it is urgent
    // Surrounded, a gun in hand that can clear the ring stays, and any change waits this long
    // (20.10.: Harlan, three twitchers on him, swapped contact beam and force gun 1000 times
    // for slam and push in turn, and the change itself never let him shoot).
    uint32_t crowdSwitchGapMs = 2500;
    float pushToleranceDeg = 20.0f;   // the push is a wide cone; a twitcher dodges a narrow aim
    uint32_t healRetryMs = 30000;   // a med pack press that healed nothing: none left, stop trying a while
    float closeRadius = 3.0f;       // "right on top of the bot"
    int surroundedCount = 3;        // this many that close: make room
    // Desperate: this many within desperateRadius and no human within desperateHumanClear.
    // The rules for mates near the blast fall away: the grenade at its feet, the detonator
    // into the one in front, the slam and the push first (user 20.10.: Novak buried in slashers).
    float desperateRadius = 3.5f, desperateHumanClear = 5.0f;
    int desperateCount = 3;
    float farRange = 12.0f;
    // Pulse rifle (DS2_BOT_WEAPONS_BATCH_A): automatic, 0.1 s a round; the grenade
    // needs 25 rounds in the clip, 1.3 s apart (primary blocked as long), flies
    // at 18 m/s under gravity and goes off on impact.
    uint32_t pulseBurstMs = 900, pulsePauseMs = 300;
    int grenadeRounds = 25;
    uint32_t grenadeCooldownMs = 1500;
    float grenadeMin = 5.0f, grenadeMax = 16.0f, grenadeSpeed = 18.0f, gravity = 9.81f;
    float grenadeFeetAhead = 1.8f;   // the desperate grenade's floor point, kept inside the aim's pitch range
    // Ripper: primary hangs a blade 2.25 m out along the aim (5-9 s, one press,
    // 2 s apart, a new shot destroys it); alt throws one (20 damage, 1.2 s apart).
    float ripperReach = 2.25f;
    uint32_t ripperPressMs = 2100, ripperThrowMs = 1300;
    // Seeker rifle: 0.84 s between shots; the scope toggles on alt fire while aiming.
    float seekerMin = 6.0f, seekerToleranceDeg = 1.5f;
    uint32_t seekerFireMs = 900;
    // Detonator: one mine a press, 1.5 s apart; five in the whole level (the
    // leader's count too), so the bot keeps it to a few, and never recalls one.
    float detonatorMin = 7.0f, detonatorMax = 15.0f, detonatorClosing = 0.8f, detonatorFromUs = 3.5f;
    int detonatorMinesMax = 2;
    uint32_t detonatorCooldownMs = 6000;
    // Flamethrower (DS2_BOT_WEAPONS_BATCH_B): hold for a stream (a flame every 0.08 s,
    // 30 m/s, 1 s, under gravity); the canister costs 25 fuel, 2.4 s apart, flies
    // at 12 m/s on an arc and bursts 2.5 m wide (it can burn the bot: never close).
    float flameRange = 5.0f, flameBlastMin = 3.5f, flameBlastMax = 8.0f, flameBlastSpeed = 12.0f;
    int flameBlastFuel = 25;
    uint32_t flameBlastCooldownMs = 2500;
    // Force gun: primary a fresh press 1.1 s apart (a shove, next to no damage); the
    // alt a 100 m/s bolt that leaves after the wind-up animation along the aim of
    // that moment, so the aim stays on the target meanwhile.
    float pushRange = 4.0f, forceBeamMin = 5.0f;
    uint32_t pushIntervalMs = 1150, forceAltCooldownMs = 1150, forceWindupMs = 1300;
    // Contact beam: hold to charge (0.8 s base, the gun says when it is full), the
    // beam leaves on release; the slam (alt) is 6 m around the bot, 1 s apart.
    float contactMin = 4.0f, contactToleranceDeg = 2.0f;
    uint32_t contactChargeMs = 900, contactCooldownMs = 600, contactHoldMaxMs = 5000;
    uint32_t slamCooldownMs = 1200;
    // Keep bursts off the players: the blast radius plus this much.
    float friendlyMargin = 1.5f;
    float stasisBurst = 1.5f, mineBurst = 4.0f, grenadeBurst = 3.0f, canisterBurst = 2.5f, shockBurst = 2.25f, detonatorBurst = 3.0f;
    // The squad. A target a mate already fights counts sharedTargetCost metres
    // farther per mate, so each bot takes its own enemy while there are enough
    // (28.09.: no ganging up on big ones); one going for a mate within coverRange
    // is worth coverBonus more. Mates keep mateSpace between their places, and no
    // shot goes where a player stands within laneWidth of its line.
    float sharedTargetCost = 8.0f, coverRange = 4.0f, coverBonus = 2.0f;
    // A target already shot by as many bots as its kind allows costs this much
    // more: the bot takes another one while there is one with room, and only
    // doubles up when every enemy is full (two slashers, three bots: all shoot).
    float overCapCost = 60.0f;
    float mateSpace = 1.6f, laneWidth = 0.6f, laneStep = 1.5f;
    // Standing in a mate's line of fire it steps out and holds the new place a
    // moment; a mate in its own line gets laneStepAfterMs to step out first.
    uint32_t mateLaneHoldMs = 700;
    uint32_t blockedGoalMs = 6000;  // a goal it could not get closer to stays off limits this long
    float objectiveRange = 80.0f, objectiveBonus = 100.0f;  // the Marker's heart (it opens some 50 m off): seen from far off, before any enemy
    // Partner pose: with nothing to fight or pick up and the leader standing, the
    // first bot of the squad keeps one place by him and watches him (like Sheva
    // in Resident Evil 5); the second one its own place. The places are in the
    // leader's own frame, set live on 28.09. in play: the
    // first 0.97 m in front of him and 0.85 m to his right, the second 0.65 m
    // behind him and 0.98 m to his right. Right is his right as he sees it: the
    // entity's first row (+0x90) points to his left (a right-handed frame).
    bool partnerPose = false;
    float partnerForward = 0.97f, partnerRight = 0.85f;
    float partner2Forward = -0.65f, partner2Right = 0.98f;
    float partnerArriveStart = 1.0f, partnerArriveStop = 0.35f;
    float partnerMove = 1.0f, partnerTurnDeg = 45.0f;   // the place follows the leader past these
    // Isaac's body turns only while he walks forward, so looking at the leader
    // does not face him: a short step toward him does. Tried a few times per place.
    float partnerFaceDeg = 25.0f, partnerFaceStep = 0.6f, partnerFaceNear = 1.0f;
    uint32_t partnerFaceMs = 350, partnerFaceGapMs = 1200;
    // It comes in from this far outside its place, the last stretch walking at the
    // leader, so it arrives facing him (28.09.: coming from behind it stood with
    // its back or side to him, and the face steps walked it ever closer).
    float partnerApproach = 0.6f, partnerFaceCloser = 0.3f;
    unsigned partnerFaceTries = 3;
    // Personality (DS2BotProfiles.ini, one per bot). The defaults keep the old,
    // instant behaviour the tests pin down.
    // Reaction: a new enemy is noticed, following or a door run begins, only after
    // a moment of its own (a third of it for an enemy on the bot), so the bots do
    // not all switch in the same frame.
    uint32_t reactMinMs = 0, reactMaxMs = 0;
    uint32_t postCombatMinMs = 0, postCombatMaxMs = 0;   // eyes open this long after the last enemy is gone
    uint32_t aimAcquireMs = 0;                           // a new target: aim first, shoot after this
    float aimSway = 0.0f;                                // metres the aim point wanders around the target
    uint32_t aimSwayMs = 450;                            // a new wander point this often
    float pressureSway = 1.6f;                           // hurt or crowded: the wander grows this much
    uint32_t doorStaggerMs = 350;                        // per squad place, on top of the reaction, before a door run
    // How much of the leader's walking moves it (29.09.: the bots copied every step
    // forward and back he took). Its follow place is moved once he is this far from
    // where it was set; it keeps following a walking leader only this far off; the
    // partner place is moved after this much walking or turning.
    float followReanchor = 1.5f, followKeep = 0.0f;
    // Making way for the leader: from this walking speed, this far ahead of him,
    // this close to his line; the step aside and how long it holds.
    float yieldSpeed = 1.2f, yieldAhead = 2.2f, yieldWidth = 0.9f, yieldStep = 0.8f;
    uint32_t yieldHoldMs = 900;
    // Its own places instead of the squad's default ones: beside the leader at an
    // angle from his facing (degrees, + to his right), its side in a fight
    // (+1 or -1) and how far back from him, and where it keeps an eye.
    bool ownPlaces = false;
    float followAngleDeg = 90.0f, followDistance = 2.5f;
    float combatSideSign = 1.0f, combatBackBy = 1.0f;
    float watchAngleDeg = 0.0f;
    // Orders. Hold: back to its spot beyond holdArriveStart, a retreat bent back
    // toward it beyond holdLeash, items only this close to it.
    float holdArriveStart = 1.2f, holdArriveStop = 0.6f, holdLeash = 4.0f, holdItemRange = 3.0f, holdRunGunDown = 5.0f;
    float focusDefend = 2.5f;      // an enemy this close to the bot beats the leader's target
    // Hold fire: attacks only while something attacks the bot or the leader (this
    // close, a hold, a hit within provokeHurtMs), and returnFireMs after that.
    float provokeRange = 3.5f;
    uint32_t provokeHurtMs = 1500, returnFireMs = 4000;
    // Attack formation: its place this far ahead of the leader and apart sideways,
    // moved after he walked or turned this much. In a fight its place is
    // vanguardFight toward the enemy and it prefers vanguardKeep of its usual
    // distance; a target it sees is never walked at.
    float vanguardAhead = 3.5f, vanguardSide = 1.6f, vanguardSolo = 0.9f, vanguardMove = 1.5f, vanguardTurnDeg = 40.0f;
    float vanguardFight = 2.5f, vanguardKeep = 0.7f;
    uint32_t vanguardRelocateMs = 1500;
    // Looting after a fight: only once no live enemy has been about for lootCalmMs.
    // Spots within lootRange of the bot and lootLeash of the leader; a new one only
    // while he stands or within lootStart of him. What lies within lootLeaderClear
    // of him is his own stomp. A corpse gets at most corpseStompsMax stomps from the
    // squad (its loot drops on the first hit that lands; the rest are misses).
    bool lootWanted = false;
    float lootRange = 9.0f, lootLeash = 12.0f, lootStart = 6.0f, lootLeaderClear = 1.8f;
    uint32_t lootCalmMs = 1500, lootGiveUpMs = 9000;
    int corpseStompsMax = 3;
    // A crate this far over its feet hangs on a wall: it walks to wallCrateStand
    // off its middle and swings from within wallCrateReach, at most wallCrateHitsMax times.
    float wallCrateHeight = 0.5f, wallCrateStand = 1.0f, wallCrateReach = 1.4f;
    uint32_t wallCrateSwingMs = 1100;
    int wallCrateHitsMax = 5;
    // This share of the crates (by crate, the same for every bot) is shot open from
    // up to crateShootRange instead, with a plain gun in hand, crateShotsMax shots.
    float crateShootChance = 0.5f, crateShootRange = 9.0f, crateShootMin = 1.5f, crateToleranceDeg = 3.0f;
    uint32_t crateShotMs = 700;
    int crateShotsMax = 3;
    float stompLootReach = 0.8f, botKillBonus = 2.0f;
    uint32_t lockerPressMs = 700;
    int lockerTries = 4;
    // The leader held by an enemy: every bot this close to the grabber goes for it.
    float rescueRange = 25.0f;
    // Antics: 0 never, 1 rare, 2 now and then, 3 often (the gap between two of a
    // bot); harmful ones (a stasis shot at the leader) only with harmfulAntics.
    int antics = 0;
    bool harmfulAntics = false;
    uint32_t anticGapMinMs[3] = {120000, 60000, 25000}, anticGapMaxMs[3] = {240000, 120000, 50000};
    uint32_t anticCalmMs = 8000;        // no enemy about for this long
    uint32_t anticRareMs = 600000;      // a victory stomp or a stasis prank at most this often per bot
    uint32_t anticRetryMs = 3000;       // due but not possible: tried again after this
    uint32_t anticKillWindowMs = 30000; // its own kill counts as fresh this long
    float anticKillReach = 10.0f;       // and only this close to the bot
    uint32_t anticMaxMs = 20000;        // given up when one takes longer
    uint32_t anticWalkMaxMs = 8000;     // getting to its place may take this long
    float anticLeaderMove = 1.0f, anticLeaderTurnDeg = 50.0f;   // in front of the leader: over when he moves or turns this much
    float anticApproach = 0.8f, anticArrive = 0.35f, anticFaceDeg = 40.0f;
    uint32_t anticSettleMs = 400;
    float anticAimSpamGap = 1.5f, anticStareGap = 1.2f;
    uint32_t anticTapMs = 150, anticTapDownMs = 75, anticAimSpamMs = 2000, anticStareMs = 3000, anticLookBackMs = 1500;
    // Shadow boxing: spins on a small circle, swings only with nobody near or before it.
    uint32_t anticSpinMinMs = 1500, anticSpinMaxMs = 2000, anticTurnMs = 1200;
    float anticSpinLead = 0.7f, anticSpinReach = 0.6f, anticTurnDoneDeg = 25.0f;
    float anticSwingClear = 2.5f, anticSwingFrontDeg = 75.0f;
    uint32_t anticSwingGapMinMs = 400, anticSwingGapMaxMs = 1100;
    // Stasis prank: from a few metres, at the chest, then a look elsewhere.
    float anticPrankMin = 2.5f, anticPrankMax = 7.0f, anticPrankStand = 4.0f, anticPrankAimDeg = 4.0f, anticChest = 1.1f;
    uint32_t anticPrankAimMs = 600, anticPrankAimMaxMs = 3000, anticInnocentMs = 2000;
};

class Brain {
public:
    explicit Brain(uint32_t seed = 0x2545F491u) : rng_(seed ? seed : 1u) {}
    Tuning tune;

    Command Tick(const Perception& seen) {
        Perception p = seen;
        Senses(p);
        anticRan_ = false;
        desperate_ = false;
        Command c = Decide(p);
        if (antic_ != Antic::None && !anticRan_) EndAntic(p.now, false, BusyWith(c.mode));
        if (!c.direct) Spread(p, c);
        if (c.mode == Mode::Combat && (c.fire || c.holdFire)) {
            for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
                const Enemy& e = p.enemies[i];
                if (e.id != c.target) continue;
                for (int l = 0; l < e.limbCount && l < kMaxLimbs; ++l) c.shotAtLimb = c.shotAtLimb || Distance(c.lookAt, e.limbs[l]) < 0.4f;
                c.shotAtChest = !c.shotAtLimb && Distance(c.lookAt, e.chest) < 0.4f;
            }
        }
        if (c.move && p.now < blockedUntil_ && Flat(c.moveTo, blockedGoal_) < 1.0f) c.move = false;
        MakeWay(p, c);
        AimPointSpeed(p, c);
        HumanAim(p, c);
        if (c.mode == Mode::Combat) ClearLane(p, c);
        StepOutOfMateLanes(p, c);
        HoldOnRegenerating(p, c);
        HoldFire(p, c);
        if (desperate_ && c.mode == Mode::Combat && (c.fire || c.altFire || c.holdFire || c.stasis)) c.desperate = true;
        Commit(c);
        if (c.mode == Mode::Combat && c.target) CountTargetKind(p, c);
        if (flickOf_ && !flickShot_ && flickOf_ == c.target && (c.fire || c.holdFire)) {
            flickShot_ = true;
            ++acquisitions;
            acquireMsSum += p.now - flickStart_;
        }
        return c;
    }
    // Human aim: how far the aim point is off the target now (degrees).
    float AimOffsetDeg() const { return std::sqrt((flickH_ + jitH_) * (flickH_ + jitH_) + (flickV_ + jitV_) * (flickV_ + jitV_)); }
    uint32_t Provoker() const { return provoker_; }
    uint32_t ItemWanted() const { return pickId_; }
    uint32_t LootWanted() const { return lootId_; }
    // Antics.
    Antic AnticNow() const { return antic_; }
    int AnticPhase() const { return anticPhase_; }
    uint32_t AnticSince() const { return anticSince_; }
    const AnticStats& AnticCounts() const { return anticStats_; }
    // Starts, ends and aborts since the last call, oldest first.
    int TakeAnticEvents(AnticEvent* out, int max) {
        const int n = anticEventCount_ < max ? anticEventCount_ : max;
        for (int i = 0; i < n; ++i) out[i] = anticEvents_[i];
        anticEventCount_ = 0;
        return n;
    }
    // The runtime takes the bot elsewhere (a scene, a kiosk, a new body).
    void StopAntic(uint32_t now, const char* why) {
        if (antic_ != Antic::None) EndAntic(now, false, why);
    }
    // This antic at once, when it fits now (tests; the schedule picks at random).
    bool ForceAntic(const Perception& p, Antic kind) {
        if (tune.antics <= 0 || antic_ != Antic::None || kind == Antic::None || kind == Antic::Count || AnticUnrest(p) || !AnticFits(p, kind, true)) return false;
        StartAntic(p, kind, "forced", 1u << static_cast<unsigned>(kind));
        return true;
    }

    Command Decide(const Perception& p) {
        Command c;
        const uint32_t now = p.now;
        commitCount_ = 0;
        planResetOn_ = 0;
        pendingHorizontal_ = -1;
        RememberSightings(p);
        if (Hostile(p)) lastHostileAt_ = now ? now : 1u;
        if (antic_ != Antic::None)
            if (const char* why = AnticUnrest(p)) EndAntic(now, false, why);
        if (p.grabbed) {                       // mash out whatever else is going on
            c.mode = Mode::Combat;
            c.mash = true;
            return c;
        }
        // The leader held: his grabber before anything else, whatever the orders,
        // the focus caps or the forced target say. Over once he is free.
        if (p.leaderGrabber) {
            for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
                const Enemy& e = p.enemies[i];
                if (e.id != p.leaderGrabber || e.health <= 0.0f) continue;
                const float dist = Distance(p.self, e.pos);
                if (dist > tune.rescueRange) break;
                // No line to the grabber (07.10.: every rescue shot met the same rail or
                // pane a metre out while a slasher held the leader): run at him instead,
                // swing once there, shoot again as soon as he is in sight.
                if (!e.visible) {
                    ++rescueCloseIns;
                    c.mode = Mode::Combat;
                    c.target = e.id;
                    c.look = true;
                    c.lookAt = e.chest;
                    if (dist < tune.meleeRange) {
                        MeleeAt(p, e, c);
                    } else {
                        c.move = true;
                        c.moveTo = e.pos;
                        c.run = true;
                        c.why = MoveWhy::Chase;
                    }
                    c.rescue = true;
                    c.focused = false;
                    lootId_ = 0;
                    return c;
                }
                Fight(p, e, dist, c);
                c.rescue = true;
                c.focused = false;
                lootId_ = 0;
                return c;
            }
        }
        const bool hold = p.orders.hold;
        if (p.chase && !hold) {                // he runs for his life: so does the squad, right behind him
            Chase(p, c);
            return c;
        }
        // 1. Health first, if nothing is on top of the bot.
        const Enemy* target = PickTarget(p);
        const float targetDist = target ? Distance(p.self, target->pos) : 1e9f;
        // A press that healed nothing means no med packs left: leave it a while.
        if (healCheckAt_ && now - healCheckAt_ >= 2500) {
            if (p.health01 <= healedFrom_ + 0.01f) noMedsUntil_ = now + tune.healRetryMs;
            healCheckAt_ = 0;
        }
        if (p.health01 < tune.healBelow && p.medpacks != 0 && now - lastHeal_ >= tune.healCooldownMs && now >= noMedsUntil_ &&
            targetDist > tune.meleeRange) {
            lastHeal_ = now;
            healCheckAt_ = now;
            healedFrom_ = p.health01;
            c.mode = Mode::Heal;
            c.heal = true;
            if (!Threat(p)) {   // with enemies about it heals where it stands
                if (hold) HoldPlace(p, c);
                else FollowLeader(p, c);
            }
            return c;
        }
        // 2. The level goes on: run there (a holding squad stays).
        if (!p.progression || hold) rushAt_ = 0;
        if (p.progression && !target && !hold) {
            // Not all at once: its reaction, and a little more for each squad place.
            if (!rushAt_) rushAt_ = now + React(false) + (tune.reactMaxMs ? tune.doorStaggerMs * static_cast<uint32_t>(p.squadIndex) : 0u);
            c.mode = Mode::Rush;
            c.move = now >= rushAt_ && Flat(p.self, p.progressionAt) > 1.2f;
            c.moveTo = p.progressionAt;
            c.run = true;
            c.look = true;
            c.lookAt = Add(p.progressionAt, {0.0f, 1.5f, 0.0f});
            return c;
        }
        // 3. Fight.
        if (target) {
            Fight(p, *target, targetDist, c);
            // A target it remembers or hears but cannot see: no turning toward it
            // through the wall unless it is about to come round (08.10.).
            if (!target->visible && !c.rescue && targetDist > tune.unseenLookRange) { c.look = false; ++unseenLookSkips; }
            return c;
        }
        // Enemies about, none of them its target (out of sight, not noticed yet, a
        // mate's): it holds where it stands with the gun up toward the nearest, and
        // the leader's steps do not move it (27.09.: a bot without a target of its
        // own followed his every move with four necromorphs near). Only left far
        // behind does it go back to him, as in a fight.
        // Holding, it walks back to its spot meanwhile, gun up.
        // One behind a wall (heard or seen a moment ago) never holds the bot: it goes
        // on as usual, only alert (08.10.: standing for one it could not see looked
        // like the bot froze).
        const Enemy* threat = Threat(p);
        bool alert = false;
        if (threat && !threat->visible && !hold) {
            ++unseenThreatFollows;
            alert = true;
            threat = nullptr;
        }
        if (threat) {
            if (hold || Flat(p.self, p.leader) <= tune.combatFollow) {
                c.mode = Mode::Combat;
                c.aim = true;
                // Only one it can see: turned toward one behind a wall it looked like aiming
                // through it (08.10.); the gun stays up all the same.
                if (Noticed(p, *threat) && threat->visible) {
                    c.look = true;
                    c.lookAt = Length(threat->chest) > 0.01f ? threat->chest : Add(threat->pos, {0.0f, 1.3f, 0.0f});
                    ++threatAims;
                }
                // Out of sight but close: back off it all the same, gun up.
                if (!hold) BackOff(p, c);
                if (hold && Flat(p.self, p.orders.holdSpot) > tune.holdArriveStart) {
                    c.move = true;
                    c.moveTo = p.orders.holdSpot;
                    c.why = MoveWhy::Follow;
                }
                return c;
            }
        }
        // 3b. The squad fights and this bot sees nothing: it comes over to help, by the
        // mesh and so round corners too, until it sees an enemy (then it fights above).
        if (!hold && !p.orders.holdFire) {
            if (const Enemy* fought = FoughtEnemy(p)) {
                alert = true;
                if (Flat(p.self, fought->pos) > tune.joinStop) {
                    ++joinFrames;
                    c.mode = Mode::Rush;
                    c.move = true;
                    c.run = true;
                    c.moveTo = fought->pos;
                    c.why = MoveWhy::Chase;
                    return c;
                }
            }
        }
        // The fight just ended: the gun stays up a moment longer toward where the last enemy was.
        if (lastTarget_ && tune.postCombatMaxMs) scanUntil_ = now + Between(tune.postCombatMinMs, tune.postCombatMaxMs);
        lastTarget_ = 0;
        // Out of a fight with a part-empty clip: top it up.
        if (p.clip >= 0 && p.clip < 4 && p.reserve != 0 && !p.reloading && now - lastReload_ > 2000) {
            lastReload_ = now;
            c.reload = true;
        }
        const bool home = hold ? Flat(p.self, p.orders.holdSpot) <= tune.holdArriveStart : Flat(p.self, p.leader) <= tune.combatFollow;
        if (scanUntil_ && now < scanUntil_ && home) {
            c.mode = Mode::Combat;
            c.aim = true;
            c.look = true;
            // Steady on where the last one was (29.09.: no looking left and right).
            Vec3 to = {lastEnemyAt_.x - p.self.x, 0.0f, lastEnemyAt_.z - p.self.z};
            if (Length(to) < 0.5f) to = FromYaw(p.leaderYaw);
            c.lookAt = Add(p.self, Add(Scale(FromYaw(std::atan2(to.x, to.z)), 6.0f), {0.0f, 1.3f, 0.0f}));
            return c;
        }
        scanUntil_ = 0;
        // 4. Pick things up close to the group, while the leader stands. One it
        // cannot get (full, off the mesh, not meant for it) is left alone a while
        // instead of being walked at again and again.
        if (const Item* item = (p.leaderMoving && !hold) || alert ? nullptr : PickItem(p)) {
            if (item->id != pickId_) { pickId_ = item->id; pickSince_ = now; pickUses_ = 0; }
            if (now - pickSince_ >= tune.itemGiveUpMs || pickUses_ >= tune.itemTries) {
                Ignore(item->id, now);
                pickId_ = 0;
            } else {
                c.mode = Mode::Pickup;
                const float d = Flat(p.self, item->pos);
                c.move = d > 0.9f;
                c.moveTo = item->pos;
                if (!c.move || d < tune.itemLookRange) { c.look = true; c.lookAt = item->pos; }
                if (p.pickupInReach && now - lastUse_ >= tune.pickupPulseMs) { lastUse_ = now; c.use = true; ++pickUses_; }
                return c;
            }
        } else {
            pickId_ = 0;
        }
        // 4b. Corpses, crates and lockers close by, once things are calm.
        if (const LootSpot* spot = alert ? nullptr : PickLoot(p)) {
            if (Loot(p, *spot, c)) return c;
        } else {
            lootId_ = 0;
        }
        // 5. Its spot (Hold, Go there), its place ahead of him (attack formation),
        // or keep up and loiter. Following goes on until the bot is close again
        // and the leader stands, so it does not start and stop at one edge.
        if (hold) { HoldPlace(p, c); return c; }
        if (p.orders.vanguard) { VanguardPlace(p, c); return c; }
        // 5b. Calm, with nothing to do: now and then an antic of its own.
        if (!alert && Antics(p, c)) return c;
        const float gap = Flat(p.self, p.leader);
        if (!following_) {
            // Wanting to follow, it starts after its own moment (at once when far behind).
            const bool want = gap > tune.followStart || (p.leaderMoving && gap > tune.idleMax + 1.5f);
            if (!want) followWantAt_ = 0;
            else if (!followWantAt_) followWantAt_ = now + React(false);
            following_ = want && (now >= followWantAt_ || gap > tune.followStart * 2.0f);
            if (following_) followWantAt_ = 0;
        } else {
            following_ = gap > tune.followStop || (p.leaderMoving && gap > tune.followKeep);
        }
        if (following_) {
            FollowLeader(p, c);
            c.run = gap > tune.runFrom;
            idleUntil_ = 0;
            return c;
        }
        if (tune.partnerPose && p.squadIndex <= 1) PartnerPose(p, c);
        else Loiter(p, c);
        if (alert && !c.move) { c.aim = true; ++alertFrames; }   // standing, the gun is up
        return c;
    }

    // The enemy the squad is fighting that this bot should come over to: one a mate
    // aims at, or, while the leader shoots, any near him. Alive and near the leader.
    const Enemy* FoughtEnemy(const Perception& p) const {
        const Enemy* best = nullptr;
        float bestDist = 1e9f;
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
            const Enemy& e = p.enemies[i];
            if (e.health <= 0.0f || Flat(e.pos, p.leader) > tune.joinRange) continue;
            bool fought = p.leaderShooting;
            for (int m = 0; m < p.mateCount && m < kMaxMates && !fought; ++m)
                fought = p.mates[m].aiming && p.mates[m].target == e.id;
            if (!fought) continue;
            const float d = Flat(p.self, e.pos);
            if (d < bestDist) { bestDist = d; best = &e; }
        }
        return best;
    }

    // The spot it walks to is off the mesh; the closest reachable point stands in for it.
    // An item there is out of reach: it is left alone for a while.
    void Unreachable(const Vec3& goal, const Vec3& closest, uint32_t now) {
        if (pickId_ && Flat(pickAt_, goal) < 0.3f) { Ignore(pickId_, now); pickId_ = 0; }
        if (Flat(idleSpot_, goal) < 0.3f) idleSpot_ = closest;
        if (haveAnchor_ && Flat(followSpot_, goal) < 0.3f) followSpot_ = closest;
        // The partner place in a wall: the mirrored side first, then as close as it gets
        // (28.09.: behind-left was in the corridor wall; it stood short of it for good,
        // still "walking", so it never looked at the leader).
        if (havePartner_ && partnerStage_ == 0 && Flat(partnerApproach_, goal) < 0.3f) partnerStage_ = 1;   // the way in is blocked: straight to the place
        if (havePartner_ && !partnerFallback_ && Flat(partnerSpot_, goal) < 0.3f) {
            if (partnerSide_ > 0.0f) partnerSide_ = -1.0f;
            else { partnerFallback_ = true; partnerClosest_ = closest; }
        }
        // Spots worked out anew every tick (in a fight) would send it straight back:
        // the place is off limits a while.
        blockedGoal_ = goal;
        blockedUntil_ = now + tune.blockedGoalMs;
    }

private:
    enum class Plan : uint8_t { Held, Cutter, Line, Mine, Javelin, Pulse, Grenade, RipperHold, RipperThrow, Seeker, Detonator, Flame, FlameBlast,
                                ForcePush, ForceBeam, ContactCharge, ContactSlam, GrenadeFeet, DetonatorClose };
    uint32_t rng_;
    uint32_t lastStomp_ = 0;
    uint32_t lastHeal_ = 0, lastMelee_ = 0, nextMeleeGap_ = 3500, lastFire_ = 0, lastStasis_ = 0, lastReload_ = 0, lastUse_ = 0;
    uint32_t idleUntil_ = 0, lookUntil_ = 0;
    uint32_t lastTarget_ = 0, limbCursor_ = 0, limbSince_ = 0, limbCursorSeen_ = 0, limbOf_ = 0;
    Vec3 limbOffset_;   // the limb aimed at, from the enemy's feet
    uint32_t lastMine_ = 0, lastRotate_ = 0, shockedFor_ = 0, planSince_ = 0;
    uint32_t stasisAimId_ = 0, stasisAimSince_ = 0, stasisSkipId_ = 0, stasisSkipUntil_ = 0;
    uint32_t lastGrenade_ = 0, lastDetonator_ = 0, lastFlameBlast_ = 0, lastPush_ = 0, lastSlam_ = 0, lastAlt_ = 0, lastZoom_ = 0;
    uint32_t burstStart_ = 0, chargeStart_ = 0, forceWindupUntil_ = 0, lastSwitch_ = 0, healCheckAt_ = 0, noMedsUntil_ = 0;
    bool combatMoving_ = false;
    Vec3 combatSpot_;
    uint32_t combatUntil_ = 0, reactAt_ = 0, stepAt_ = 0;
    Vec3 blockedGoal_, partnerAnchor_;
    float partnerYaw_ = 0.0f, partnerSide_ = 1.0f;
    bool havePartner_ = false, partnerFallback_ = false;
    Vec3 partnerSpot_, partnerClosest_, partnerApproach_;
    int partnerStage_ = 0;   // 0 to the point outside, 1 the last stretch in, 2 at its place
    Vec3 faceTo_;
    uint32_t faceUntil_ = 0, faceAt_ = 0;
    unsigned faceTries_ = 0;
    uint32_t blockedUntil_ = 0;
    int askedSlot_ = -1;
    float healedFrom_ = 1.0f;
    bool cutterHorizontal_ = false;   // its own count of the cutter's turns when the runtime cannot read it
    Plan plan_ = Plan::Held;
    Vec3 idleSpot_, lookSpot_, followAnchor_, followSpot_;
    bool lookAtLeader_ = false, following_ = false, walking_ = false, haveAnchor_ = false;
    uint32_t stillSince_ = 0, settleMs_ = 0, lastArrive_ = 0;
    // Personality state: enemies noticed, the scan after a fight, the door run,
    // following, and the aim's acquire pause and wander.
    struct Seen { uint32_t id = 0, readyAt = 0, seenAt = 0; };
    Seen noticed_[kMaxEnemies * 2];
    uint32_t scanUntil_ = 0, rushAt_ = 0, followWantAt_ = 0, partnerWantAt_ = 0;
    Vec3 lastEnemyAt_;
    uint32_t aimTarget_ = 0, acquireUntil_ = 0, swayAt_ = 0;
    uint32_t yieldUntil_ = 0;
    Vec3 yieldTo_;
    // Out of the leader's way: he walks at it (up to yieldAhead in front of him,
    // less than yieldWidth off his line): a step to the side, held a moment. It
    // keeps aiming and shooting in a fight.
    void MakeWay(const Perception& p, Command& c) {
        if (p.grabbed || c.mode == Mode::Rush || p.orders.vanguard) return;   // ahead of him on purpose in the attack formation
        if (c.mode == Mode::Combat) { yieldUntil_ = 0; return; }   // in a fight it holds, whatever he walks
        if (p.now < yieldUntil_) {
            if (Flat(p.self, yieldTo_) > 0.3f) { c.move = true; c.moveTo = yieldTo_; c.run = false; c.why = MoveWhy::Step; }
            return;
        }
        const Vec3 vel = {p.leaderVel.x, 0.0f, p.leaderVel.z};
        const float speed = Length(vel);
        if (speed < tune.yieldSpeed) return;
        const Vec3 dir = Scale(vel, 1.0f / speed);
        const Vec3 rel = {p.self.x - p.leader.x, 0.0f, p.self.z - p.leader.z};
        const float along = Dot(rel, dir);
        const Vec3 side = {-dir.z, 0.0f, dir.x};
        const float lateral = Dot(rel, side);
        if (along < -0.3f || along > tune.yieldAhead || std::fabs(lateral) > tune.yieldWidth) return;
        // To the side it already leans to; straight on his line, the squad's own side.
        const float sign = std::fabs(lateral) > 0.1f ? (lateral > 0.0f ? 1.0f : -1.0f) : (p.squadIndex % 2 == 0 ? 1.0f : -1.0f);
        yieldTo_ = Add(p.self, Add(Scale(side, sign * (tune.yieldStep + tune.yieldWidth - std::fabs(lateral))), Scale(dir, 0.3f)));
        yieldTo_.y = p.self.y;
        yieldUntil_ = p.now + tune.yieldHoldMs;
        c.move = true;
        c.moveTo = yieldTo_;
        c.run = false;
        c.why = MoveWhy::Step;
    }
    Vec3 sway_, swayGoal_;

    uint32_t React(bool urgent) {
        if (!tune.reactMaxMs) return 0;
        const uint32_t ms = Between(tune.reactMinMs, tune.reactMaxMs);
        return urgent ? ms / 3u : ms;
    }
    // An enemy counts from the moment it has been seen for the bot's reaction time
    // (forgotten after 4 s out of sight).
    bool Noticed(const Perception& p, const Enemy& e) {
        if (!tune.reactMaxMs) return true;
        Seen* slot = nullptr;
        Seen* oldest = &noticed_[0];
        for (Seen& s : noticed_) {
            if (s.id == e.id && p.now - s.seenAt <= 4000u) { slot = &s; break; }
            if (s.seenAt < oldest->seenAt) oldest = &s;
        }
        if (!slot) {
            slot = oldest;
            const bool urgent = (e.victim && e.victim == p.selfId) || Distance(p.self, e.pos) < tune.closeRadius || e.objective ||
                                (e.dodger && tune.dodgerSnap);
            *slot = Seen{e.id, p.now + React(urgent), p.now};
        }
        slot->seenAt = p.now;
        return p.now >= slot->readyAt;
    }
    // A target lying down regrowing its limbs (the Ubermorph) is aimed at but not
    // shot, slowed or kicked until it stands again: every hit there is wasted
    // (05.10.: the bots shot it all through its regrowth).
    void HoldOnRegenerating(const Perception& p, Command& c) {
        if (!c.target) return;
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
            const Enemy& e = p.enemies[i];
            if (e.id != c.target || !e.regenerating) continue;
            c.regenHold = c.fire || c.holdFire || c.altFire || c.holdAlt || c.melee || c.stasis || c.stomp;
            c.fire = c.holdFire = c.altFire = c.holdAlt = c.melee = c.stasis = c.stomp = false;
            c.shotAtLimb = c.shotAtChest = false;
            return;
        }
    }
    // Hold fire: no shot, swing, stomp or stasis unless something attacks the bot
    // or the leader right now; then it fights back until that enemy is gone (and
    // returnFireMs after the last attack). The leader's forced target is always fair game.
    uint32_t provoker_ = 0, returnUntil_ = 0, hurtAt_ = 0, leaderHurtAt_ = 0;
    float lastHealth_ = 1.0f, lastLeaderHealth_ = 1.0f;
    void HoldFire(const Perception& p, Command& c) {
        const uint32_t now = p.now;
        if (p.health01 < lastHealth_ - 0.005f) hurtAt_ = now;
        if (p.leaderHealth01 < lastLeaderHealth_ - 0.005f) leaderHurtAt_ = now;
        lastHealth_ = p.health01;
        lastLeaderHealth_ = p.leaderHealth01;
        if (!p.orders.holdFire) { provoker_ = returnUntil_ = 0; return; }
        if (c.rescue || c.mode == Mode::Loot) { c.returnFire = c.rescue; return; }   // freeing the leader, or stomping loot with nothing about
        const Enemy* attacker = nullptr;
        float nearest = tune.provokeRange;
        bool provokerAlive = false;
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
            const Enemy& e = p.enemies[i];
            if (e.health <= 0.0f || e.objective) continue;
            if (e.id == provoker_) provokerAlive = true;
            const float d = Distance(p.self, e.pos) < Distance(p.leader, e.pos) ? Distance(p.self, e.pos) : Distance(p.leader, e.pos);
            if (d < nearest) { nearest = d; attacker = &e; }
        }
        const bool hurt = (hurtAt_ && now - hurtAt_ < tune.provokeHurtMs) || (leaderHurtAt_ && now - leaderHurtAt_ < tune.provokeHurtMs);
        if (attacker || hurt || p.grabbed || p.leaderGrabbed) {
            returnUntil_ = now + tune.returnFireMs;
            if (attacker) { provoker_ = attacker->id; provokerAlive = true; }
            else if (!provoker_ && c.target) { provoker_ = c.target; provokerAlive = true; }
        }
        if (!provokerAlive) provoker_ = 0;
        if (c.focused || now < returnUntil_ || provoker_) { c.returnFire = !c.focused; return; }
        c.holdFireActive = true;
        c.fireHeld = c.fire || c.holdFire || c.altFire || c.holdAlt || c.melee || c.stasis || c.stomp;
        c.fire = c.holdFire = c.altFire = c.holdAlt = c.melee = c.stasis = c.stomp = false;
        c.shotAtLimb = c.shotAtChest = false;
    }
    // Twitchers against plain slashers as targets: frames, frames with a shot, in sight, moving fast.
    void CountTargetKind(const Perception& p, const Command& c) {
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
            const Enemy& e = p.enemies[i];
            if (e.id != c.target) continue;
            const bool shot = c.fire || c.holdFire || c.altFire;
            if (e.dodger) {
                ++dodgerFrames;
                if (shot) ++dodgerShots;
                if (e.visible) ++dodgerSeenFrames;
                if (Length(e.vel) > 3.0f) ++dodgerFastFrames;
            } else if (e.kind == EnemyKind::Slasher) {
                ++slasherFrames;
                if (shot) ++slasherShots;
                if (e.visible) ++slasherSeenFrames;
            }
            return;
        }
    }
    void CastStasis(const Perception& p, const Enemy& e, bool priority, Command& c) {
        c.aim = true;
        c.look = true;
        c.lookAt = e.chest;
        ++stasisAimFrames;
        const float widen = e.dodger && tune.dodgerSnap ? tune.dodgerTolerance : 1.0f;
        if (p.aimErrorDeg > tune.fireToleranceDeg * 2.0f * widen) {
            if (stasisAimId_ != e.id) {
                stasisAimId_ = e.id;
                stasisAimSince_ = p.now;
            } else if (p.now - stasisAimSince_ >= tune.stasisAimGiveUpMs) {
                stasisSkipId_ = e.id;
                stasisSkipUntil_ = p.now + tune.stasisGiveUpMs;
                stasisAimId_ = 0;
                ++stasisAimGiveUps;
            }
            return;
        }
        stasisAimId_ = 0;
        Defer(kOnStasis, &Brain::lastStasis_, p.now);
        c.stasis = true;
        if (priority) ++(e.dodger ? stasisOnTwitchers : stasisOnStalkers);
    }
    // A twitcher, or a stalker coming on fast or already close, within the priority range.
    bool StasisPriority(const Perception& p, const Enemy& e, float dist) const {
        if (!tune.stasisPriority || e.health <= 0.0f || e.objective || dist > tune.stasisPriorityRange) return false;
        if (e.dodger) return true;
        if (e.kind != EnemyKind::Stalker) return false;
        if (dist < tune.stasisCloseRange * 2.0f) return true;
        const Vec3 toUs = {p.self.x - e.pos.x, 0.0f, p.self.z - e.pos.z}, toLeader = {p.leader.x - e.pos.x, 0.0f, p.leader.z - e.pos.z};
        return Length(e.vel) >= tune.stalkerChargeSpeed && (Dot(e.vel, toUs) > 0.0f || Dot(e.vel, toLeader) > 0.0f);
    }
    // ... and this bot could cast at it now.
    bool StasisSkipped(const Enemy& e, uint32_t now) const { return e.id == stasisSkipId_ && now < stasisSkipUntil_; }
    bool StasisReady(const Perception& p, const Enemy& e, float dist) const {
        return !e.stasised && !StasisSkipped(e, p.now) && p.stasis01 > 0.0f && p.now - lastStasis_ >= tune.stasisPriorityCooldownMs && StasisPriority(p, e, dist) &&
               !PlayerNear(p, e.pos, tune.stasisBurst);
    }
    bool DodgerTarget(const Perception& p, uint32_t id) const {
        if (!tune.dodgerSnap || !id) return false;
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i)
            if (p.enemies[i].id == id) return p.enemies[i].dodger;
        return false;
    }
    // The leader's forced target, while it lasts, lives and is in sight.
    const Enemy* Forced(const Perception& p) const {
        if (!p.orders.focus || p.now >= p.orders.focusUntil) return nullptr;
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
            const Enemy& e = p.enemies[i];
            if (e.id == p.orders.focus) return e.health > 0.0f && e.visible ? &e : nullptr;
        }
        return nullptr;
    }
    // Aiming like a person: a new target is aimed at a moment before the first
    // shot, and the aim point wanders a little around it (more when hurt or crowded).
    void HumanAim(const Perception& p, Command& c) {
        if (c.mode != Mode::Combat || !c.target) { aimTarget_ = 0; ResetFlick(); return; }
        if (c.target != aimTarget_) { aimTarget_ = c.target; acquireUntil_ = c.rescue || DodgerTarget(p, c.target) ? p.now : p.now + tune.aimAcquireMs; }
        if (p.now < acquireUntil_) {
            if (c.fire || c.holdFire) ++acquireHolds;
            c.fire = false;
            c.holdFire = false;
        }
        HumanFlick(p, c);
        if (!(tune.aimSway > 0.0f) || !c.look) return;
        bool pressed = p.health01 < 0.4f;
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies && !pressed; ++i)
            pressed = p.enemies[i].health > 0.0f && Distance(p.self, p.enemies[i].pos) < tune.closeRadius;
        const float r = tune.aimSway * (pressed ? tune.pressureSway : 1.0f);
        if (!swayAt_ || p.now >= swayAt_) {
            const float a = Unit() * 6.2831853f, d = std::sqrt(Unit()) * r;
            swayGoal_ = {std::cos(a) * d, (Unit() - 0.5f) * r, std::sin(a) * d};
            swayAt_ = p.now + tune.aimSwayMs;
        }
        sway_ = Add(sway_, Scale(Sub(swayGoal_, sway_), 0.12f));
        c.lookAt = Add(c.lookAt, sway_);
    }

    // Timers a shot starts, written by Commit only when the shot is still in the
    // command after every filter of Tick.
    enum : uint8_t { kOnFire = 1, kOnAlt = 2, kOnStasis = 4, kOnMelee = 8, kOnStomp = 16 };
    struct Pending { uint32_t Brain::*field; uint32_t value; uint8_t on; };
    Pending commits_[6]{};
    int commitCount_ = 0;
    uint8_t planResetOn_ = 0;
    int pendingHorizontal_ = -1;
    void Defer(uint8_t on, uint32_t Brain::*field, uint32_t value) {
        if (commitCount_ < static_cast<int>(sizeof(commits_) / sizeof(commits_[0]))) commits_[commitCount_++] = Pending{field, value, on};
    }
    void DeferPlanReset(uint8_t on) { planResetOn_ = on; }
    static bool Live(const Command& c, uint8_t on) {
        return ((on & kOnFire) && c.fire) || ((on & kOnAlt) && c.altFire) || ((on & kOnStasis) && c.stasis) || ((on & kOnMelee) && c.melee) ||
               ((on & kOnStomp) && c.stomp);
    }
    void Commit(const Command& c) {
        bool dropped = false;
        for (int i = 0; i < commitCount_; ++i) {
            if (Live(c, commits_[i].on)) this->*commits_[i].field = commits_[i].value;
            else dropped = true;
        }
        if (planResetOn_ && Live(c, planResetOn_)) plan_ = Plan::Held;
        if (pendingHorizontal_ >= 0 && c.altFire) cutterHorizontal_ = pendingHorizontal_ == 1;
        if (dropped) ++heldShots;
        commitCount_ = 0;
        planResetOn_ = 0;
        pendingHorizontal_ = -1;
    }
    // A shot due by its cadence but the aim not on the point yet is counted.
    bool Ready(bool aimed, bool cadence) {
        if (cadence && !aimed) ++aimWaits;
        return aimed && cadence;
    }
    // How fast the aim point itself moves: a limb point moves with the animation
    // (a climb is mostly vertical), and the look, which closes the gap at a fixed
    // rate, trails a moving point by that speed over its rate. The runtime leads by
    // it, so the aim sits on the point and the shot is not held back for good.
    uint32_t aimVelOf_ = 0, aimVelAt_ = 0;
    Vec3 aimVelFrom_, aimVel_;
    void AimPointSpeed(const Perception& p, Command& c) {
        if (c.mode != Mode::Combat || !c.look || !c.target) { aimVelOf_ = 0; return; }
        if (c.target != aimVelOf_) {
            aimVelOf_ = c.target;
            aimVelFrom_ = c.lookAt;
            aimVelAt_ = p.now;
            aimVel_ = {};
            return;
        }
        const Vec3 d = Sub(c.lookAt, aimVelFrom_);
        const float moved = Length(d);
        const uint32_t dt = p.now - aimVelAt_;
        if (moved > tune.aimJump) {   // another point (the next limb, another plan): no motion
            aimVelFrom_ = c.lookAt;
            aimVelAt_ = p.now;
            aimVel_ = {};
        } else if (moved >= 0.01f && dt >= 30) {
            const Vec3 v = Scale(d, 1000.0f / static_cast<float>(dt));
            aimVel_ = Length(v) > tune.aimLeadMaxSpeed ? Vec3{} : Add(Scale(aimVel_, 0.5f), Scale(v, 0.5f));
            aimVelFrom_ = c.lookAt;
            aimVelAt_ = p.now;
        } else if (dt > 300) {
            aimVel_ = {};   // it stands
        }
        c.lookVel = aimVel_;
    }

    uint32_t Next() {   // xorshift32
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        return rng_;
    }
    float Unit() { return static_cast<float>(Next() & 0xFFFFFF) / static_cast<float>(0x1000000); }
    uint32_t Between(uint32_t lo, uint32_t hi) { return hi > lo ? lo + Next() % (hi - lo + 1) : lo; }

    // How many bots may shoot this enemy while others still have room: small and
    // quick ones one each, tough ones two, what takes the whole squad three.
    int FocusCap(const Enemy& e) const {
        switch (e.kind) {
        case EnemyKind::Hunter: case EnemyKind::Brute: case EnemyKind::Guardian: case EnemyKind::Boss: case EnemyKind::Nest:
        case EnemyKind::GuardianTentacle: return 3;
        case EnemyKind::Puker: case EnemyKind::Stalker: case EnemyKind::DividerCore: return 2;
        case EnemyKind::Unknown: return e.maxHealth >= tune.dangerousHealth ? 2 : 1;
        default: return 1;
        }
    }
    // The enemy that matters most: the closest one near the bot or the leader.
    const Enemy* PickTarget(const Perception& p) {
        // The leader's target comes first unless something else is right on the bot.
        if (const Enemy* forced = Forced(p)) {
            bool onUs = false;
            for (int i = 0; i < p.enemyCount && i < kMaxEnemies && !onUs; ++i) {
                const Enemy& e = p.enemies[i];
                onUs = e.id != forced->id && e.health > 0.0f && e.visible && !e.objective && Distance(p.self, e.pos) < tune.focusDefend;
            }
            if (!onUs) return forced;
        }
        const Enemy* best = nullptr;
        float bestScore = 1e9f;
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
            const Enemy& e = p.enemies[i];
            if (e.health <= 0.0f || !e.visible) continue;
            // Down and regrowing (the Ubermorph): no target at all, not even to aim at (user 20.10.)
            if (e.regenerating) { ++regenSkips; continue; }
            if (!Noticed(p, e)) continue;   // not yet: its own reaction time
            // Shooting what cannot die is ammo thrown away once it is crippled.
            if (e.maxHealth >= tune.immortalHealth && e.limbsLost >= tune.immortalLimbsOff) continue;
            const float mine = Distance(p.self, e.pos), theirs = Distance(p.leader, e.pos);
            const float reach = e.objective ? tune.objectiveRange : tune.engageRange;
            if (mine > reach && theirs > reach) continue;
            float score = mine < theirs ? mine : theirs * 0.9f;   // one going for the leader counts a little closer
            if (e.id == lastTarget_) score -= 2.0f;               // no dithering between two
            // The squad: every bot on an enemy of its own while there are enough,
            // and help a mate something goes for.
            int shared = 0;
            for (int m = 0; m < p.mateCount && m < kMaxMates; ++m) {
                const Mate& mate = p.mates[m];
                if (mate.target == e.id) ++shared;
                if (e.victim && e.victim == mate.id && Flat(mate.pos, e.pos) < tune.coverRange) score -= tune.coverBonus;
            }
            // What cannot die (the Ubermorph) comes after the others unless it is
            // right on the bot or the leader (28.09.: both bots fixed on it, the
            // rest left alone). Alone it may take all three (FocusCap).
            const bool immortal = e.maxHealth >= tune.immortalHealth;
            if (immortal && mine > tune.immortalClose && theirs > tune.immortalClose) score += tune.immortalPenalty;
            if (shared) score += tune.sharedTargetCost * static_cast<float>(shared);
            if (shared >= FocusCap(e)) score += tune.overCapCost;
            if (e.victim && e.victim == p.leaderId && p.leaderId) score -= 1.0f;
            if (e.objective) score -= tune.objectiveBonus;
            // The Guardian dies by its tentacles (40 health each); its body has 30000.
            if (e.kind == EnemyKind::GuardianTentacle) score -= tune.objectiveBonus;
            if (e.kind == EnemyKind::Guardian) score += tune.immortalPenalty;
            if (StasisReady(p, e, mine)) score -= tune.stasisPriorityBonus;
            if (p.orders.holdFire && e.id == provoker_) score -= 4.0f;   // the one that attacked comes first
            if (score < bestScore) { bestScore = score; best = &e; }
        }
        return best;
    }

    // How many others stand within the group radius of this enemy.
    int GroupAround(const Perception& p, const Enemy& e) const {
        int n = 0;
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
            const Enemy& o = p.enemies[i];
            if (o.id == e.id || o.health <= 0.0f || !o.visible) continue;
            if (Flat(o.pos, e.pos) <= tune.groupRadius) ++n;
        }
        return n;
    }
    // How many others stand on the line from the bot through this enemy, within reach.
    int InLine(const Perception& p, const Enemy& e) const {
        Vec3 dir = Sub(e.pos, p.self);
        dir.y = 0.0f;
        const float len = Length(dir);
        if (len < 0.5f) return 0;
        dir = Scale(dir, 1.0f / len);
        int n = 0;
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
            const Enemy& o = p.enemies[i];
            if (o.id == e.id || o.health <= 0.0f || !o.visible) continue;
            const Vec3 rel = {o.pos.x - p.self.x, 0.0f, o.pos.z - p.self.z};
            const float along = Dot(rel, dir);
            if (along < 1.0f || along > tune.lineReach) continue;
            const Vec3 side = Sub(rel, Scale(dir, along));
            if (Length(side) <= tune.lineLateral) ++n;
        }
        return n;
    }
    int SlotOf(const Perception& p, Gun kind) const {
        for (int i = 0; i < kGunSlots; ++i) if (p.guns[i].kind == kind && p.guns[i].Usable()) return i;
        return -1;
    }
    // A player within this burst (plus the margin) of the point.
    bool PlayerNear(const Perception& p, const Vec3& point, float burst) const {
        const float r = burst + tune.friendlyMargin;
        if (Distance(p.leader, point) <= r) return true;
        for (int i = 0; i < p.playerCount && i < kMaxPlayers; ++i) if (Distance(p.players[i], point) <= r) return true;
        return false;
    }
    // How far off the aim may be at this distance to still put the shot on a limb
    // (or the body, with its own half width).
    float Tolerance(float dist) const { return Tolerance(dist, tune.aimHalfWidth); }
    float Tolerance(float dist, float halfWidth) const {
        const float deg = std::atan2(halfWidth, dist > 0.5f ? dist : 0.5f) * 57.2957795f;
        return deg < tune.minToleranceDeg ? tune.minToleranceDeg : (deg > tune.fireToleranceDeg ? tune.fireToleranceDeg : deg);
    }
    // A point on a moving enemy, moved on by the time a shot at this speed takes to get there.
    static Vec3 Lead(const Perception& p, const Enemy& e, const Vec3& point, float speed) {
        return Add(point, Scale(e.vel, Distance(p.self, point) / speed));
    }
    int CloseCount(const Perception& p, float radius) const {
        int n = 0;
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
            const Enemy& o = p.enemies[i];
            if (o.health > 0.0f && o.visible && Flat(o.pos, p.self) <= radius) ++n;
        }
        return n;
    }
    // Closing in: the flat speed toward the bot, m/s.
    static float Closing(const Perception& p, const Enemy& e) {
        Vec3 to = Sub(p.self, e.pos);
        to.y = 0.0f;
        const float len = Length(to);
        return len > 0.1f ? Dot(e.vel, Scale(to, 1.0f / len)) : 0.0f;
    }
    int ClipOf(const Perception& p, Gun kind) const {
        const int slot = SlotOf(p, kind);
        return slot >= 0 ? p.guns[slot].clip : -1;
    }
    bool Has(const Perception& p, Gun kind) const { return SlotOf(p, kind) >= 0; }
    // The weapon for this enemy. A choice stands gunHoldMs, so the bot does not
    // flick between two guns while the numbers wobble; one-shot plans (a grenade,
    // a mine, a slam) cut in at once and hand back afterwards.
    Plan ChoosePlan(const Perception& p, const Enemy& e, float dist) {
        const uint32_t now = p.now;
        bool known = false;
        for (const GunSlot& g : p.guns) known = known || g.kind != Gun::None;
        if (!known) return Plan::Held;
        // Mid-charge or mid-wind-up: letting go now would waste it.
        if ((plan_ == Plan::ContactCharge && chargeStart_) || (plan_ == Plan::ForceBeam && now < forceWindupUntil_)) return plan_;
        const int group = GroupAround(p, e), inLine = InLine(p, e), close = CloseCount(p, tune.closeRadius);
        const bool danger = e.maxHealth >= tune.dangerousHealth, distant = dist > tune.farRange;
        Plan want = Plan::Held;
        auto pick = [&](Plan plan) { if (want == Plan::Held) want = plan; };
        // Desperate: whatever clears the ring around it, mates near or not (no human near, Fight).
        if (desperate_) {
            if (Has(p, Gun::ContactBeam) && now - lastSlam_ >= tune.slamCooldownMs) pick(Plan::ContactSlam);
            if (Has(p, Gun::ForceGun) && now - lastPush_ >= tune.pushIntervalMs) pick(Plan::ForcePush);
            if (Has(p, Gun::PulseRifle) && ClipOf(p, Gun::PulseRifle) >= tune.grenadeRounds && now - lastGrenade_ >= tune.grenadeCooldownMs) pick(Plan::GrenadeFeet);
            if (Has(p, Gun::Detonator) && p.detonatorMines >= 0 && p.detonatorMines <= tune.detonatorMinesMax && now - lastDetonator_ >= tune.detonatorCooldownMs)
                pick(Plan::DetonatorClose);
            if (Has(p, Gun::Flamethrower)) pick(Plan::Flame);
        }
        // Surrounded: room first.
        if (close >= tune.surroundedCount) {
            if (Has(p, Gun::ContactBeam) && now - lastSlam_ >= tune.slamCooldownMs) pick(Plan::ContactSlam);
            if (Has(p, Gun::ForceGun) && now - lastPush_ >= tune.pushIntervalMs) pick(Plan::ForcePush);
            if (Has(p, Gun::Flamethrower)) pick(Plan::Flame);
        }
        // Right on top of the bot.
        if (dist < tune.closeRadius) {
            if (Has(p, Gun::ForceGun) && close >= 2 && now - lastPush_ >= tune.pushIntervalMs) pick(Plan::ForcePush);
            if (Has(p, Gun::Flamethrower)) pick(Plan::Flame);
            if (Has(p, Gun::Ripper) && !danger) pick(Plan::RipperHold);
        }
        // A group further out.
        if (group >= 1) {
            if (Has(p, Gun::Detonator) && Closing(p, e) >= tune.detonatorClosing && dist >= tune.detonatorMin && dist <= tune.detonatorMax &&
                p.detonatorMines >= 0 && p.detonatorMines <= tune.detonatorMinesMax && now - lastDetonator_ >= tune.detonatorCooldownMs &&
                !PlayerNear(p, DetonatorPoint(p, e), tune.detonatorBurst))
                pick(Plan::Detonator);
            if (Has(p, Gun::LineGun) && dist >= tune.mineMin && dist <= tune.mineMax && now - lastMine_ >= tune.mineCooldownMs &&
                !PlayerNear(p, MinePoint(p, e), tune.mineBurst))
                pick(Plan::Mine);
            if (Has(p, Gun::PulseRifle) && ClipOf(p, Gun::PulseRifle) >= tune.grenadeRounds && dist >= tune.grenadeMin && dist <= tune.grenadeMax &&
                now - lastGrenade_ >= tune.grenadeCooldownMs && !PlayerNear(p, e.pos, tune.grenadeBurst))
                pick(Plan::Grenade);
            if (Has(p, Gun::Flamethrower) && ClipOf(p, Gun::Flamethrower) >= tune.flameBlastFuel && dist >= tune.flameBlastMin &&
                dist <= tune.flameBlastMax && now - lastFlameBlast_ >= tune.flameBlastCooldownMs && !PlayerNear(p, e.pos, tune.canisterBurst))
                pick(Plan::FlameBlast);
            if (Has(p, Gun::Javelin) && dist >= tune.javelinMinRange) pick(Plan::Javelin);
        }
        if (inLine >= 1 && Has(p, Gun::LineGun)) pick(Plan::Line);
        // Big, or far away: the heavy single shots.
        if (danger || distant) {
            if (Has(p, Gun::SeekerRifle) && dist >= tune.seekerMin) pick(Plan::Seeker);
            if (Has(p, Gun::ContactBeam) && dist >= tune.contactMin) pick(Plan::ContactCharge);
            if (Has(p, Gun::ForceGun) && dist >= tune.forceBeamMin) pick(Plan::ForceBeam);
            if (Has(p, Gun::Javelin)) pick(Plan::Javelin);
        }
        // One at a time: limbs.
        if (Has(p, Gun::PlasmaCutter)) pick(Plan::Cutter);
        if (Has(p, Gun::Ripper)) pick(Plan::RipperThrow);
        if (Has(p, Gun::PulseRifle)) pick(Plan::Pulse);
        if (Has(p, Gun::LineGun)) pick(Plan::Line);
        if (Has(p, Gun::Javelin)) pick(Plan::Javelin);
        if (Has(p, Gun::SeekerRifle)) pick(Plan::Seeker);
        if (Has(p, Gun::ContactBeam)) pick(Plan::ContactCharge);
        if (Has(p, Gun::Flamethrower) && dist <= tune.flameRange + 2.0f) pick(Plan::Flame);
        if (Has(p, Gun::ForceGun)) pick(Plan::ForceBeam);
        bool oneShot = want == Plan::Mine || want == Plan::Grenade || want == Plan::Detonator || want == Plan::ContactSlam ||
                       want == Plan::FlameBlast || want == Plan::ForcePush || want == Plan::GrenadeFeet || want == Plan::DetonatorClose;
        // Stay with the gun in hand when it can do the job: every change costs an
        // animation, and in a crowd a bot that keeps changing never shoots. A
        // change goes ahead when it is urgent (surrounded, the gun is empty, an
        // area shot into three or more) or when the last one is long enough ago.
        const Gun held = p.activeSlot >= 0 ? p.guns[p.activeSlot].kind : Gun::None;
        const bool heldUsable = p.activeSlot >= 0 && p.guns[p.activeSlot].Usable();
        const bool crowded = desperate_ || close >= tune.surroundedCount;
        if (crowded && heldUsable && GunFor(want) != held) {
            const Plan inHand = CrowdPlan(p, held, dist);
            if (inHand != Plan::Held) {
                want = inHand;
                ++crowdStays;
                oneShot = want == Plan::ContactSlam || want == Plan::ForcePush || want == Plan::GrenadeFeet || want == Plan::DetonatorClose;
            }
        }
        const bool urgent = !heldUsable || ((crowded || (oneShot && group >= 2)) && now - lastSwitch_ >= tune.crowdSwitchGapMs);
        if (GunFor(want) != held && heldUsable && !urgent) {
            const Plan stay = DefaultPlan(held, dist);
            if (stay != Plan::Held && (now - lastSwitch_ < tune.switchCooldownMs || oneShot || now - planSince_ < tune.gunHoldMs)) {
                want = stay;
                oneShot = false;
            }
        }
        if (want == plan_ || plan_ == Plan::Held || now - planSince_ >= tune.gunHoldMs || oneShot) {
            if (want != plan_) planSince_ = now;
            plan_ = want;
        }
        return plan_;
    }
    // What the gun in hand does about a crowd, Held when it has nothing for one.
    Plan CrowdPlan(const Perception& p, Gun g, float dist) const {
        const uint32_t now = p.now;
        switch (g) {
        case Gun::ContactBeam: return now - lastSlam_ >= tune.slamCooldownMs ? Plan::ContactSlam : Plan::ContactCharge;
        case Gun::ForceGun: return now - lastPush_ >= tune.pushIntervalMs ? Plan::ForcePush : Plan::ForceBeam;
        case Gun::Flamethrower: return Plan::Flame;
        case Gun::PulseRifle:
            return desperate_ && ClipOf(p, Gun::PulseRifle) >= tune.grenadeRounds && now - lastGrenade_ >= tune.grenadeCooldownMs ? Plan::GrenadeFeet : Plan::Pulse;
        case Gun::Detonator:
            return desperate_ && p.detonatorMines >= 0 && p.detonatorMines <= tune.detonatorMinesMax && now - lastDetonator_ >= tune.detonatorCooldownMs
                       ? Plan::DetonatorClose : Plan::Held;
        case Gun::Ripper: return dist < tune.closeRadius ? Plan::RipperHold : Plan::Held;
        default: return Plan::Held;
        }
    }
    // What a gun does on its own, when it is the one in hand.
    Plan DefaultPlan(Gun g, float dist) const {
        switch (g) {
        case Gun::PlasmaCutter: return Plan::Cutter;
        case Gun::LineGun: return Plan::Line;
        case Gun::Javelin: return Plan::Javelin;
        case Gun::PulseRifle: return Plan::Pulse;
        case Gun::Ripper: return dist < tune.closeRadius ? Plan::RipperHold : Plan::RipperThrow;
        case Gun::SeekerRifle: return Plan::Seeker;
        case Gun::Flamethrower: return dist <= tune.flameRange + 2.0f ? Plan::Flame : Plan::Held;
        case Gun::ForceGun: return Plan::ForceBeam;
        case Gun::ContactBeam: return Plan::ContactCharge;
        default: return Plan::Held;
        }
    }
    // How close the bot wants to be for this plan.
    void PlanRange(Plan plan, float& lo, float& hi) const {
        switch (plan) {
        case Plan::Line: lo = 3.0f; hi = 8.0f; break;
        case Plan::Mine: lo = tune.mineMin; hi = tune.mineMax; break;
        case Plan::Javelin: lo = tune.javelinMinRange; hi = 10.0f; break;
        case Plan::Pulse: lo = 4.0f; hi = 12.0f; break;
        case Plan::Grenade: lo = tune.grenadeMin; hi = tune.grenadeMax; break;
        case Plan::RipperHold: lo = tune.ripperReach - 0.5f; hi = tune.ripperReach + 0.4f; break;
        case Plan::RipperThrow: lo = 3.0f; hi = 9.0f; break;
        case Plan::Seeker: lo = tune.seekerMin; hi = 30.0f; break;
        case Plan::Detonator: lo = tune.detonatorMin - 1.0f; hi = tune.detonatorMax; break;
        case Plan::Flame: lo = 1.0f; hi = tune.flameRange - 0.5f; break;
        case Plan::FlameBlast: lo = tune.flameBlastMin; hi = tune.flameBlastMax; break;
        case Plan::ForcePush: lo = 0.8f; hi = tune.pushRange - 0.5f; break;
        case Plan::ForceBeam: lo = tune.forceBeamMin; hi = 15.0f; break;
        case Plan::ContactCharge: lo = tune.contactMin; hi = 14.0f; break;
        case Plan::ContactSlam: lo = 0.0f; hi = tune.closeRadius; break;
        case Plan::GrenadeFeet: lo = 0.0f; hi = tune.desperateRadius + 1.0f; break;
        case Plan::DetonatorClose: lo = 0.0f; hi = tune.desperateRadius + 1.0f; break;
        default: break;   // keepMin .. keepMax
        }
    }
    static Gun GunFor(Plan plan) {
        switch (plan) {
        case Plan::Cutter: return Gun::PlasmaCutter;
        case Plan::Line: case Plan::Mine: return Gun::LineGun;
        case Plan::Javelin: return Gun::Javelin;
        case Plan::Pulse: case Plan::Grenade: case Plan::GrenadeFeet: return Gun::PulseRifle;
        case Plan::RipperHold: case Plan::RipperThrow: return Gun::Ripper;
        case Plan::Seeker: return Gun::SeekerRifle;
        case Plan::Detonator: case Plan::DetonatorClose: return Gun::Detonator;
        case Plan::Flame: case Plan::FlameBlast: return Gun::Flamethrower;
        case Plan::ForcePush: case Plan::ForceBeam: return Gun::ForceGun;
        case Plan::ContactCharge: case Plan::ContactSlam: return Gun::ContactBeam;
        default: return Gun::None;
        }
    }
    // The point to aim at so a lobbed shot (launch speed v, gravity g) comes down
    // on the target: the flat direction, raised to the low-arc launch angle.
    static Vec3 LobPoint(const Vec3& eye, const Vec3& target, float v, float g) {
        Vec3 flat = {target.x - eye.x, 0.0f, target.z - eye.z};
        const float d = Length(flat), h = target.y - eye.y;
        if (d < 0.5f) return target;
        const float v2 = v * v, root = v2 * v2 - g * (g * d * d + 2.0f * h * v2);
        if (root < 0.0f) return {target.x, eye.y + d, target.z};   // out of reach: 45 degrees
        const float angle = std::atan((v2 - std::sqrt(root)) / (g * d));
        return {target.x, eye.y + std::tan(angle) * d, target.z};
    }
    // Where a detonator mine goes: on the floor in the enemy's way, part of the
    // way from it toward the bot, never close to the bot or the leader.
    Vec3 DetonatorPoint(const Perception& p, const Enemy& e) const {
        Vec3 to = Sub(p.self, e.pos);
        to.y = 0.0f;
        const float len = Length(to);
        const float along = len > 0.1f ? (len - tune.detonatorFromUs) : 0.0f;
        Vec3 spot = len > 0.1f ? Add(e.pos, Scale(to, (along > 0.0f ? along : 0.0f) * 0.5f / len)) : e.pos;
        spot.y = e.pos.y + 0.05f;
        return spot;
    }
    // Where the line gun's horizontal line should cross: between the legs at
    // knee height takes both at once; one leg, else low on the body.
    static Vec3 LinePoint(const Enemy& e) {
        Vec3 legs[2];
        int n = 0;
        for (int i = 0; i < e.limbCount && n < 2; ++i) if (e.limbKind[i] == LimbKind::Leg) legs[n++] = e.limbs[i];
        if (n == 2) return Scale(Add(legs[0], legs[1]), 0.5f);
        if (n == 1) return legs[0];
        return {e.pos.x, e.pos.y + 0.5f, e.pos.z};
    }
    // Where a mine should land: where the group will be when it goes off, not
    // further ahead than mineLeadMax, on the floor.
    Vec3 MinePoint(const Perception& p, const Enemy& e) const {
        Vec3 centre = e.pos, vel = e.vel;
        int n = 1;
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
            const Enemy& o = p.enemies[i];
            if (o.id == e.id || o.health <= 0.0f || !o.visible || Flat(o.pos, e.pos) > tune.groupRadius) continue;
            centre = Add(centre, o.pos);
            vel = Add(vel, o.vel);
            ++n;
        }
        centre = Scale(centre, 1.0f / static_cast<float>(n));
        vel = Scale(vel, 1.0f / static_cast<float>(n));
        vel.y = 0.0f;
        const uint32_t fuse = p.mineFuseMs ? p.mineFuseMs : tune.mineFuseMs;
        Vec3 lead = Scale(vel, static_cast<float>(fuse) / 1000.0f);
        const float l = Length(lead);
        if (l > tune.mineLeadMax) lead = Scale(lead, tune.mineLeadMax / l);
        return {centre.x + lead.x, e.pos.y + 0.1f, centre.z + lead.z};
    }

    void Fight(const Perception& p, const Enemy& e, float dist, Command& c) {
        const uint32_t now = p.now;
        c.mode = Mode::Combat;
        c.target = e.id;
        c.focused = p.orders.focus && e.id == p.orders.focus && now < p.orders.focusUntil;
        lastEnemyAt_ = e.pos;
        scanUntil_ = 0;
        if (e.id != lastTarget_) { lastTarget_ = e.id; limbCursor_ = 0; limbSince_ = now; }
        if (now - fightAt_ > 1000) catchingUp_ = kiting_ = false;   // a new fight starts without the last one's run back
        fightAt_ = now;
        // Buried in enemies with no human near: the crowd blasts come first (ChoosePlan),
        // before any run back to the leader.
        desperate_ = CloseCount(p, tune.desperateRadius) >= tune.desperateCount;
        if (desperate_ && HumanNear(p, p.self, tune.desperateHumanClear)) { desperate_ = false; ++desperateHumanHolds; }
        if (desperate_) ++desperateFrames;
        const float leaderGap = Flat(p.self, p.leader);
        const bool free = !desperate_ && !p.orders.hold && !p.grabbed;
        if (free && (leaderGap > tune.kiteFar || (catchingUp_ && leaderGap > tune.kiteFar - tune.kiteFarBand))) {
            ++breakAwayFrames;
            catchingUp_ = true;
            kiting_ = false;
            combatMoving_ = false;
            rejoining_ = false;
            c.move = true;
            c.moveTo = p.leader;
            c.run = true;
            c.why = MoveWhy::Follow;
            return;
        }
        catchingUp_ = false;
        kiting_ = free && (leaderGap > tune.combatCatchUp || (kiting_ && leaderGap > tune.combatFollow));
        FightBody(p, e, dist, c);
        if (kiting_) Kite(p, e, c);
    }
    // Back toward the leader on foot with the gun up: a step along the way to him and to one
    // side, the side changing every second or two, away from the target first and around any
    // other enemy on the way. The plan and the shots come from FightBody as in any fight.
    void Kite(const Perception& p, const Enemy& e, Command& c) {
        ++kiteFrames;
        const uint32_t now = p.now;
        const Vec3 to = {p.leader.x - p.self.x, 0.0f, p.leader.z - p.self.z};
        const float len = Length(to);
        if (len < 0.5f) return;
        const Vec3 dir = Scale(to, 1.0f / len);
        const Vec3 side = {-dir.z, 0.0f, dir.x};
        if (!kiteSide_ || now >= kiteFlipAt_) {
            const Vec3 away = {p.self.x - e.pos.x, 0.0f, p.self.z - e.pos.z};
            kiteSide_ = kiteSide_ ? -kiteSide_ : (Dot(away, side) >= 0.0f ? 1 : -1);
            kiteFlipAt_ = now + Between(tune.kiteFlipMinMs, tune.kiteFlipMaxMs);
        }
        const float step = len < tune.kiteStep ? len : tune.kiteStep;
        const Vec3 ahead = Add(p.self, Scale(dir, step));
        Vec3 spot = Add(ahead, Scale(side, tune.kiteSide * static_cast<float>(kiteSide_)));
        if (WayNearEnemy(p, p.self, spot, tune.followDanger, tune.onUs)) {
            const Vec3 other = Add(ahead, Scale(side, -tune.kiteSide * static_cast<float>(kiteSide_)));
            if (!WayNearEnemy(p, p.self, other, tune.followDanger, tune.onUs)) {
                spot = other;
                kiteSide_ = -kiteSide_;
            } else {
                spot = ahead;
            }
        }
        c.move = true;
        c.moveTo = spot;
        c.run = false;
        c.why = MoveWhy::Follow;
    }
    void FightBody(const Perception& p, const Enemy& e, float dist, Command& c) {
        const uint32_t now = p.now;
        // The weapon for the situation first: it decides how close the bot wants to be.
        const Plan plan = ChoosePlan(p, e, dist);
        float keepMin = tune.keepMin, keepMax = tune.keepMax;
        PlanRange(plan, keepMin, keepMax);
        // Attack formation: nearer. Only the plain shots come closer at the near
        // edge too; area weapons keep theirs.
        if (p.orders.vanguard && !p.orders.hold) {
            if (plan == Plan::Held || plan == Plan::Cutter) keepMin *= tune.vanguardKeep;
            keepMax = keepMax * tune.vanguardKeep > keepMin + 1.0f ? keepMax * tune.vanguardKeep : keepMin + 1.0f;
        }
        // Small and underfoot: stomp it where the bot stands. Backing off would not
        // help, it crawls after the bot and stays below the swing.
        const bool underfoot = e.height > 0.0f && e.height < tune.stompHeight && Flat(p.self, e.pos) < tune.stompReach &&
                               e.pos.y > p.self.y - tune.stompBelow && e.pos.y < p.self.y + tune.stompAbove;
        if (underfoot) {
            c.look = true;
            c.lookAt = e.chest;
            if (now - lastStomp_ >= tune.stompIntervalMs) { Defer(kOnStomp, &Brain::lastStomp_, now); c.stomp = true; }
            return;
        }
        // No gun at all (a new game: the bots start with empty hands like the
        // player): nothing to aim with. It swings at what is on top of it and
        // otherwise keeps away, close to the leader.
        bool armed = !p.gunsKnown;
        for (const GunSlot& g : p.guns) armed = armed || g.kind != Gun::None;
        if (!armed) {
            c.look = true;
            c.lookAt = e.chest;
            if (dist < tune.meleeRange) {
                MeleeAt(p, e, c);
            } else if (dist < tune.keepMin + 1.0f) {
                Retreat(p, e, c);
            } else if (Flat(p.self, Anchor(p)) > (p.orders.hold ? tune.holdArriveStart : tune.combatFollow)) {
                c.move = true;
                c.moveTo = Anchor(p);
                c.why = MoveWhy::Follow;
            }
            return;
        }
        // A twitcher or a stalker on the bot: stasis before any swing.
        if (dist < tune.meleeRange && StasisReady(p, e, dist)) {
            CastStasis(p, e, true, c);
            return;
        }
        // Too close to shoot: step back and swing now and then, without aiming
        // (unless the weapon in mind is made for this distance).
        if (dist < tune.meleeRange && keepMin >= tune.meleeRange) {
            MeleeAt(p, e, c);
            return;
        }
        CombatMove(p, e, dist, keepMin, keepMax, c);
        if (catchingUp_) return;   // running back to the leader, the gun waits
        // Empty: reload while there is ammo for it, eyes on the target. With none
        // left, on to the weapon change below (28.09.: both bots pressed reload on
        // an empty plasma cutter for good, a full pulse rifle in the next slot).
        const bool heldEmpty = p.clip == 0 && p.reserve == 0;
        if (p.reloading || (p.clip == 0 && !heldEmpty)) {
            c.look = true;
            c.lookAt = e.chest;
            if (!p.reloading && now - lastReload_ > 1500) { lastReload_ = now; c.reload = true; }
            return;
        }
        // Stasis on something big, or on anything closing in, while there is energy.
        // Twitchers and charging stalkers first: any energy for a cast, a short cooldown.
        const bool priority = StasisPriority(p, e, dist);
        const bool dangerous = priority || e.maxHealth >= tune.dangerousHealth || dist < tune.stasisCloseRange;
        const bool energy = priority ? p.stasis01 > 0.0f : p.stasis01 >= tune.stasisMinEnergy;
        const bool cooled = now - lastStasis_ >= (priority ? tune.stasisPriorityCooldownMs : tune.stasisCooldownMs);
        const bool clear = desperate_ ? !HumanNear(p, e.pos, tune.stasisBurst + tune.friendlyMargin) : !PlayerNear(p, e.pos, tune.stasisBurst);
        if (dangerous && !e.stasised && energy && cooled && clear && !StasisSkipped(e, now)) {
            CastStasis(p, e, priority, c);
            return;
        }
        if (priority && !e.stasised) ++(!energy ? stasisNoEnergy : (!cooled ? stasisCooling : stasisPlayerNear));
        c.aim = true;
        c.look = true;
        // Take the weapon first, shoot once it is in hand.
        const Gun gun = GunFor(plan);
        if (gun != Gun::None) {
            const int slot = SlotOf(p, gun);
            if (slot >= 0 && slot != p.activeSlot) {
                if (slot != askedSlot_) { askedSlot_ = slot; lastSwitch_ = now; }
                c.slot = slot;
                c.lookAt = gun == Gun::LineGun ? LinePoint(e) : LimbPoint(e, now);
                return;
            }
            if (p.switching) { c.lookAt = LimbPoint(e, now); return; }
        }
        // Every timer a shot starts is deferred (Defer): Tick may still hold the shot
        // back (a player in the line, hold fire), and a held shot must not use up the
        // cooldown of a stasis, a mine or the fire interval.
        switch (plan) {
        case Plan::Mine: {
            c.lookAt = MinePoint(p, e);
            if (p.aimErrorDeg <= tune.lineToleranceDeg) {
                Defer(kOnAlt, &Brain::lastMine_, now);
                DeferPlanReset(kOnAlt);   // one throw; the next choice starts fresh
                c.altFire = true;
            }
            return;
        }
        case Plan::Line:
            // Lead the target by the line's flight time.
            c.lookAt = Add(LinePoint(e), Scale(e.vel, Distance(p.self, e.pos) / tune.lineBoltSpeed));
            if (Ready(p.aimErrorDeg <= tune.lineToleranceDeg, now - lastFire_ >= tune.lineFireIntervalMs)) { Defer(kOnFire, &Brain::lastFire_, now); c.fire = true; }
            return;
        case Plan::Pulse: {
            // Bursts at a limb: hold the trigger a while, let go a moment, as a person would.
            // (28.09.: at the chest, which on a slasher or the Ubermorph is ammo thrown away.)
            c.lookAt = LimbPoint(e, now);
            const float pulseTolerance = e.limbCount > 0 ? Tolerance(dist) * 2.0f : 5.0f;
            if (p.aimErrorDeg > pulseTolerance) { burstStart_ = 0; return; }
            if (!burstStart_) burstStart_ = now;
            const uint32_t phase = (now - burstStart_) % (tune.pulseBurstMs + tune.pulsePauseMs);
            c.holdFire = phase < tune.pulseBurstMs;
            return;
        }
        case Plan::Grenade: {
            const Vec3 at = {e.pos.x, e.pos.y + 0.3f, e.pos.z};
            c.lookAt = LobPoint(p.self, at, tune.grenadeSpeed, tune.gravity);
            if (p.aimErrorDeg <= tune.lineToleranceDeg) { Defer(kOnAlt, &Brain::lastGrenade_, now); DeferPlanReset(kOnAlt); c.altFire = true; }
            return;
        }
        case Plan::RipperHold: {
            // One press hangs the blade; then the aim alone holds it into a limb.
            const int limb = StickyLimb(e, now);
            c.lookAt = limb >= 0 ? e.limbs[limb] : e.chest;
            if (!p.ripperBladeOut && p.aimErrorDeg <= 8.0f && now - lastFire_ >= tune.ripperPressMs) { Defer(kOnFire, &Brain::lastFire_, now); c.fire = true; }
            return;
        }
        case Plan::RipperThrow: {
            const int limb = StickyLimb(e, now);
            c.lookAt = Lead(p, e, limb >= 0 ? e.limbs[limb] : e.chest, tune.ripperThrowSpeed);
            if (Ready(p.aimErrorDeg <= Tolerance(dist, limb >= 0 ? tune.aimHalfWidth : tune.chestHalfWidth), now - lastAlt_ >= tune.ripperThrowMs)) {
                Defer(kOnAlt, &Brain::lastAlt_, now);
                Defer(kOnAlt, &Brain::limbCursor_, limbCursor_ + 1);   // the next blade at the next limb
                c.altFire = true;
            }
            return;
        }
        case Plan::Seeker: {
            const int limb = StickyLimb(e, now);
            c.lookAt = limb >= 0 ? e.limbs[limb] : e.chest;
            // The scope first (it stays on), a shot after, 0.84 s apart.
            if (p.seekerZoomed == 0) {
                if (now - lastFire_ >= tune.seekerFireMs && now - lastZoom_ >= 1000) { Defer(kOnAlt, &Brain::lastZoom_, now); c.altFire = true; }
                return;
            }
            if (Ready(p.aimErrorDeg <= tune.seekerToleranceDeg, now - lastFire_ >= tune.seekerFireMs)) {
                Defer(kOnFire, &Brain::lastFire_, now);
                c.fire = true;
            }
            return;
        }
        case Plan::GrenadeFeet: {
            // The floor a little ahead: the blast takes the ring around it (the runtime spares the bot).
            // 0.8 m ahead was about 75 deg down from the muzzle, past the aim's pitch limit, so the aim
            // never came within the tolerance and the grenade never left (24.10.). At 1.8 m it is 45-50 deg.
            Vec3 fwd = Sub(e.pos, p.self);
            fwd.y = 0.0f;
            const float len = Length(fwd);
            const Vec3 dir = len > 0.1f ? Scale(fwd, 1.0f / len) : Vec3{0.0f, 0.0f, 1.0f};
            c.lookAt = {p.self.x + dir.x * tune.grenadeFeetAhead, p.self.y + 0.05f, p.self.z + dir.z * tune.grenadeFeetAhead};
            if (p.aimErrorDeg <= 12.0f) { Defer(kOnAlt, &Brain::lastGrenade_, now); DeferPlanReset(kOnAlt); c.altFire = true; ++desperateBlasts; }
            return;
        }
        case Plan::DetonatorClose: {
            // At the feet of the one in front: it goes off as soon as anything steps through its line.
            c.lookAt = {e.pos.x, e.pos.y + 0.05f, e.pos.z};
            if (p.aimErrorDeg <= 10.0f) { Defer(kOnFire, &Brain::lastDetonator_, now); DeferPlanReset(kOnFire); c.fire = true; ++desperateBlasts; }
            return;
        }
        case Plan::Detonator: {
            c.lookAt = DetonatorPoint(p, e);
            if (p.aimErrorDeg <= tune.lineToleranceDeg) { Defer(kOnFire, &Brain::lastDetonator_, now); DeferPlanReset(kOnFire); c.fire = true; }
            return;
        }
        case Plan::Flame:
            c.lookAt = e.chest;
            c.holdFire = dist <= tune.flameRange && p.aimErrorDeg <= 8.0f;
            return;
        case Plan::FlameBlast: {
            c.lookAt = LobPoint(p.self, {e.pos.x, e.pos.y + 0.3f, e.pos.z}, tune.flameBlastSpeed, tune.gravity);
            if (p.aimErrorDeg <= tune.lineToleranceDeg) { Defer(kOnAlt, &Brain::lastFlameBlast_, now); DeferPlanReset(kOnAlt); c.altFire = true; }
            return;
        }
        case Plan::ForcePush:
            c.lookAt = e.chest;
            if (p.aimErrorDeg <= tune.pushToleranceDeg) { Defer(kOnFire, &Brain::lastPush_, now); DeferPlanReset(kOnFire); c.fire = true; }
            return;
        case Plan::ForceBeam:
            // One press starts the wind-up; the bolt leaves along the aim at its end.
            c.lookAt = e.chest;
            if (now < forceWindupUntil_) return;
            if (p.aimErrorDeg <= tune.fireToleranceDeg * 2.0f && now - lastAlt_ >= tune.forceAltCooldownMs) {
                Defer(kOnAlt, &Brain::lastAlt_, now);
                Defer(kOnAlt, &Brain::forceWindupUntil_, now + tune.forceWindupMs);
                c.altFire = true;
            }
            return;
        case Plan::ContactCharge: {
            // Hold the trigger to charge; the beam leaves when it is let go, so let go on target.
            c.lookAt = e.chest;
            if (!chargeStart_) {
                if (now - lastFire_ < tune.contactCooldownMs) return;
                chargeStart_ = now;
            }
            const bool ready = p.contactReady >= 0 ? p.contactReady == 1 : now - chargeStart_ >= tune.contactChargeMs;
            if ((!ready || p.aimErrorDeg > tune.contactToleranceDeg || LaneBlocked(p, c.lookAt)) && now - chargeStart_ < tune.contactHoldMaxMs) {
                c.holdFire = true;
                return;
            }
            chargeStart_ = 0;   // let go: the beam leaves (or, not full, nothing does)
            lastFire_ = now;
            return;
        }
        case Plan::ContactSlam:
            c.lookAt = e.chest;
            Defer(kOnAlt, &Brain::lastSlam_, now);
            DeferPlanReset(kOnAlt);
            c.altFire = true;
            return;
        case Plan::Javelin:
            c.lookAt = e.chest;
            // A spear stuck in an enemy: the shock, once per spear. It costs no ammo and
            // burns the impaled one and everything within 2.25 m, so it is worth it even alone.
            // signed: the stamp comes from the gun read this tick and may be a few ms past `now`
            if (p.spearHitAt && p.spearHitAt != shockedFor_ && static_cast<int32_t>(now - p.spearHitAt) <= static_cast<int32_t>(tune.shockWindowMs) &&
                !PlayerNear(p, e.pos, tune.shockBurst)) {
                Defer(kOnAlt, &Brain::shockedFor_, p.spearHitAt);
                c.altFire = true;
                return;
            }
            c.lookAt = Lead(p, e, e.chest, tune.spearSpeed);
            if (Ready(p.aimErrorDeg <= Tolerance(dist, tune.chestHalfWidth) * 1.5f, now - lastFire_ >= tune.javelinFireIntervalMs)) {
                Defer(kOnFire, &Brain::lastFire_, now);
                c.fire = true;
            }
            return;
        default:
            break;
        }
        // A detonator in hand with no place for a mine: no shots (it would lay mines at their feet).
        if (plan == Plan::Held && p.activeSlot >= 0 && p.guns[p.activeSlot].kind == Gun::Detonator) return;
        // Plasma cutter (or whatever is in hand): a limb at a time, the next one when a limb has taken a while.
        const int limb = StickyLimb(e, now);
        c.lookAt = limb >= 0 ? e.limbs[limb] : e.chest;
        // A nest dies by its three pods (20 health each, 1000 damage when one bursts);
        // its body has 3000. No pod in sight: aimed at, not shot.
        if ((e.kind == EnemyKind::Nest || e.kind == EnemyKind::Guardian) && limb < 0) { ++nestHolds; return; }
        // The blade across the limb: vertical for an arm, horizontal for a leg. Not for a
        // dodger: the turn would cost the pause between its dodges.
        const bool dodger = e.dodger && tune.dodgerSnap;
        if (plan == Plan::Cutter && limb >= 0 && !dodger) {
            const bool wantHorizontal = e.limbKind[limb] == LimbKind::Leg;
            const bool isHorizontal = p.cutterHorizontal >= 0 ? p.cutterHorizontal == 1 : cutterHorizontal_;
            if (wantHorizontal != isHorizontal) {
                if (now - lastRotate_ >= tune.rotateCooldownMs) {
                    Defer(kOnAlt, &Brain::lastRotate_, now);
                    pendingHorizontal_ = wantHorizontal ? 1 : 0;   // its own count of the turns, kept once the press goes out
                    c.altFire = true;
                }
                return;
            }
        }
        if (plan == Plan::Cutter && p.cutterTurning) return;   // the blade is still on its way round
        const float widen = dodger ? tune.dodgerTolerance : 1.0f;
        if (Ready(p.aimErrorDeg <= Tolerance(dist, limb >= 0 ? tune.aimHalfWidth : tune.chestHalfWidth) * widen, now - lastFire_ >= tune.fireIntervalMs)) {
            Defer(kOnFire, &Brain::lastFire_, now);
            c.fire = true;
        }
    }

    // The limb being worked on, else the body.
    Vec3 LimbPoint(const Enemy& e, uint32_t now) {
        const int limb = StickyLimb(e, now);
        return limb >= 0 ? e.limbs[limb] : e.chest;
    }
    // Which of the enemy's limbs that is now. The limb list is sorted anew by how
    // open each limb is on every refresh, so a list index jumped between arm and
    // leg from one frame to the next and the aim never settled for a shot (07.10.).
    // The limb is kept by where it sits on the body instead; the next one is taken
    // after a while, or when limbCursor_ moves on (a ripper blade per limb).
    int StickyLimb(const Enemy& e, uint32_t now) {
        if (e.limbCount <= 0) return -1;
        const int count = e.limbCount < kMaxLimbs ? e.limbCount : kMaxLimbs;
        bool next = limbCursor_ != limbCursorSeen_;
        if (now - limbSince_ > 2500) { next = true; limbSince_ = now; }
        limbCursorSeen_ = limbCursor_;
        int current = -1;
        if (limbOf_ == e.id) {
            float best = tune.aimJump;
            for (int i = 0; i < count; ++i) {
                const float d = Distance(Sub(e.limbs[i], e.pos), limbOffset_);
                if (d < best) { best = d; current = i; }
            }
        }
        int pick = current >= 0 ? current : 0;   // the most open one to start with
        if (next && current >= 0 && count > 1) { pick = (current + 1) % count; ++limbSwitches; }
        limbOf_ = e.id;
        limbOffset_ = Sub(e.limbs[pick], e.pos);
        return pick;
    }
    // Any enemy within `radius` of the straight way from a to b.
    // skipWithin: enemies already this close to the start do not count (they are on
    // the bot wherever it goes).
    bool WayNearEnemy(const Perception& p, const Vec3& a, const Vec3& b, float radius, float skipWithin = 0.0f) const {
        Vec3 ab = {b.x - a.x, 0.0f, b.z - a.z};
        const float len2 = Dot(ab, ab);
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
            const Enemy& o = p.enemies[i];
            if (o.health <= 0.0f || (skipWithin > 0.0f && Flat(a, o.pos) < skipWithin)) continue;
            const Vec3 ao = {o.pos.x - a.x, 0.0f, o.pos.z - a.z};
            float t = len2 > 0.01f ? Dot(ao, ab) / len2 : 0.0f;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            if (Length(Sub(ao, Scale(ab, t))) < radius) return true;
        }
        return false;
    }
    // The bot's place in a fight: beside the leader on the bot's side of him, a
    // step back from the enemy, and not closer to it than the weapon wants.
    Vec3 CombatSpot(const Perception& p, const Enemy& e, float keepMin) const {
        Vec3 back = {p.leader.x - e.pos.x, 0.0f, p.leader.z - e.pos.z};
        float len = Length(back);
        back = len > 0.1f ? Scale(back, 1.0f / len) : FromYaw(p.leaderYaw + 3.14159265f);
        const Vec3 side = {-back.z, 0.0f, back.x};
        const Vec3 fromLeader = {p.self.x - p.leader.x, 0.0f, p.self.z - p.leader.z};
        float sign = Dot(fromLeader, side) >= 0.0f ? 1.0f : -1.0f, backBy = tune.combatBack;
        // A squad splits: the first to one side, the second to the other, the third behind.
        if (p.squadSize > 1) {
            sign = p.squadIndex == 0 ? 1.0f : (p.squadIndex == 1 ? -1.0f : 0.0f);
            if (p.squadIndex >= 2) backBy += 1.8f;
        }
        if (tune.ownPlaces) { sign = tune.combatSideSign; backBy = tune.combatBackBy; }
        Vec3 spot = Add(p.leader, Add(Scale(side, sign * tune.combatSide), Scale(back, backBy)));
        const float d = Flat(spot, e.pos);
        if (d < keepMin) spot = Add(spot, Scale(back, keepMin - d));
        spot.y = p.leader.y;
        return spot;
    }
    // Moving in a fight: stay by the leader and shoot from there. Walks when it is
    // too far from him or on his feet, stops once at its place; backs off an enemy
    // that comes too close; only a close-range weapon goes after the target, and
    // not far. A way past an enemy is not taken: it holds and fights instead.
    void CombatMove(const Perception& p, const Enemy& e, float dist, float keepMin, float keepMax, Command& c) {
        if (p.orders.hold) { HoldCombatMove(p, e, dist, keepMin, c); return; }
        const uint32_t now = p.now;
        const float gap = Flat(p.self, p.leader);
        // The run back with the gun down and the aimed way back belong to Fight (kiteFar, Kite).
        if (desperate_ || kiting_) catchingUp_ = false;
        else if (gap > tune.kiteFar) catchingUp_ = true;
        else if (gap <= tune.combatFollow) catchingUp_ = false;
        if (catchingUp_) {
            combatMoving_ = false;
            reactAt_ = 0;
            rejoining_ = false;
            c.move = true;
            c.moveTo = p.leader;
            c.run = true;
            c.why = MoveWhy::Follow;
            return;
        }
        // Enemies close by, or several near: step back while it shoots (bent back
        // toward the leader once the leash runs out, see Retreat).
        if (!CloseRangePlan(keepMax) && BackOff(p, c)) { combatMoving_ = false; reactAt_ = 0; rejoining_ = false; return; }
        // Left behind: to the leader now, no pause first; only a way that runs
        // right past another enemy is not taken (the one already on the bot does
        // not count). The aim and the gun keep working while it walks (28.09.: the
        // bots stayed far off too long in fights; with the Ubermorph at arm's
        // length they only backed away from it, 20 m from the leader).
        // It walks to its place by him as it was a moment ago, picked anew every
        // 2.5 s, not to a spot tied to his every step: tied, it copied each step
        // forward and back he took while aiming (28.09.). It stops once back in range.
        if (gap > tune.combatFollow || (rejoining_ && gap > tune.combatRejoin + 0.5f && Flat(p.self, rejoinSpot_) > tune.combatArrive)) {
            combatMoving_ = false;
            reactAt_ = 0;
            if (dist < keepMin) { rejoining_ = false; Retreat(p, e, c); return; }
            if (!rejoining_ || now >= rejoinUntil_) {
                rejoinSpot_ = CombatSpot(p, e, keepMin);   // its squad side, held for a while
                rejoinUntil_ = now + tune.followHoldMs;
                rejoining_ = true;
            }
            if (WayNearEnemy(p, p.self, rejoinSpot_, tune.followDanger, tune.onUs)) return;
            c.move = true;
            c.moveTo = rejoinSpot_;
            c.why = MoveWhy::Follow;
            return;
        }
        rejoining_ = false;
        if (dist < keepMin) { Retreat(p, e, c); combatMoving_ = false; reactAt_ = 0; return; }
        // Only the close-range plans chase (a profile's short favourite distance is no reason to).
        const bool closeWeapon = CloseRangePlan(keepMax);
        if (closeWeapon && dist > keepMax && gap < tune.closeWeaponRange + tune.closeWeaponChase) {
            Vec3 to = Sub(e.pos, p.self);
            to.y = 0.0f;
            const float len = Length(to);
            c.move = true;
            c.moveTo = len > 0.1f ? Add(p.self, Scale(to, (dist - (keepMin + keepMax) * 0.5f) / len)) : p.self;
            c.why = MoveWhy::Chase;
            combatMoving_ = false;
            return;
        }
        // On its way: the place chosen stays, until it is there or the time is up.
        if (combatMoving_) {
            if (Flat(p.self, combatSpot_) <= tune.combatArrive || now >= combatUntil_ || WayNearEnemy(p, p.self, combatSpot_, tune.dangerPath)) {
                combatMoving_ = false;
            } else {
                c.move = true;
                c.moveTo = combatSpot_;
                c.why = stepping_ ? MoveWhy::Step : MoveWhy::Relocate;
                return;
            }
        }
        if (p.orders.vanguard && VanguardCombat(p, e, dist, keepMin, keepMax, gap, c)) return;
        // Where the leader walks moves nobody in a fight: no new place because he
        // came close or went a few steps off (27.09.: every step of his moved the
        // bots). It holds and fights; only left behind past combatFollow (above)
        // does it go back to him.
        reactAt_ = 0;
        // Now and then a step aside of its own, eyes on the target.
        if (!stepAt_) stepAt_ = now + Between(tune.combatStepMinMs, tune.combatStepMaxMs);
        if (now < stepAt_) return;
        stepAt_ = 0;
        if (Unit() >= tune.combatStepChance) return;
        Vec3 across = {-(e.pos.z - p.self.z), 0.0f, e.pos.x - p.self.x};
        const float len = Length(across);
        if (len < 0.1f) return;
        const Vec3 spot = Add(p.self, Scale(across, (Unit() < 0.5f ? -1.0f : 1.0f) * tune.combatStep / len));
        const float fromLeader = Flat(spot, p.leader);
        if (fromLeader > tune.combatRadius || fromLeader < tune.combatCrowd + 0.3f || WayNearEnemy(p, p.self, spot, tune.dangerPath) ||
            NearMate(p, spot, tune.mateSpace))
            return;
        StartCombatMove(spot, now, c);
        stepping_ = true;
        c.why = MoveWhy::Step;
    }
    bool rejoining_ = false, stepping_ = false, catchingUp_ = false;
    uint32_t fightAt_ = 0;
    bool desperate_ = false, kiting_ = false;
    int kiteSide_ = 0;
    uint32_t kiteFlipAt_ = 0;
    Vec3 rejoinSpot_;
    uint32_t rejoinUntil_ = 0, leaderInLaneSince_ = 0;

    // What it keeps close to: its spot while holding, else the leader.
    static Vec3 Anchor(const Perception& p) { return p.orders.hold ? p.orders.holdSpot : p.leader; }
    // Holding in a fight: it shoots from its spot, backs off what comes too close
    // and walks back once there is room; far from the spot (a Go there under
    // fire) it runs there with the gun down.
    void HoldCombatMove(const Perception& p, const Enemy& e, float dist, float keepMin, Command& c) {
        combatMoving_ = rejoining_ = stepping_ = false;
        reactAt_ = 0;
        const Vec3& spot = p.orders.holdSpot;
        const float away = Flat(p.self, spot);
        if (away > tune.combatCatchUp) catchingUp_ = true;
        else if (away <= tune.holdRunGunDown) catchingUp_ = false;
        if (catchingUp_ && dist >= tune.closeRadius) {
            c.move = true;
            c.moveTo = spot;
            c.run = true;
            c.why = MoveWhy::Follow;
            return;
        }
        catchingUp_ = false;
        if (dist < keepMin) { Retreat(p, e, c); return; }
        if (!CloseRangePlan(0.0f) && BackOff(p, c)) return;
        if (away > tune.holdArriveStart) {
            c.move = true;
            c.moveTo = spot;
            c.why = MoveWhy::Follow;
        }
    }
    // Holding with nothing to fight: back to the spot, then it stands and looks about.
    void HoldPlace(const Perception& p, Command& c) {
        const bool healing = c.mode == Mode::Heal;
        haveAnchor_ = following_ = false;
        idleUntil_ = 0;
        const float away = Flat(p.self, p.orders.holdSpot);
        const bool walking = Arrive(p, c, p.orders.holdSpot, tune.holdArriveStart, tune.holdArriveStop);
        if (!healing) c.mode = walking ? Mode::Follow : Mode::Idle;
        if (walking) { c.run = away > tune.runFrom; return; }
        StandingLook(p, c);
    }
    // Attack formation, no fight: a place ahead of the leader (where he walks, else
    // where he faces), the squad spread sideways, the eyes on the way ahead.
    Vec3 vanguardAnchor_, vanguardDir_;
    bool haveVanguard_ = false;
    uint32_t vanguardCheckAt_ = 0;
    Vec3 VanguardSpot(const Perception& p) {
        const float speed = Length(p.leaderVel);
        const Vec3 dir = speed > 0.8f ? Scale(Vec3{p.leaderVel.x, 0.0f, p.leaderVel.z}, 1.0f / speed) : FromYaw(p.leaderYaw);
        const float turn = haveVanguard_ ? std::acos(std::fmax(-1.0f, std::fmin(1.0f, Dot(dir, vanguardDir_)))) : 10.0f;
        if (!haveVanguard_ || Flat(vanguardAnchor_, p.leader) > tune.vanguardMove || turn > tune.vanguardTurnDeg * 0.01745329f) {
            vanguardAnchor_ = p.leader;
            vanguardDir_ = dir;
            haveVanguard_ = true;
        }
        const Vec3 side = {-vanguardDir_.z, 0.0f, vanguardDir_.x};
        float lateral = tune.vanguardSolo, ahead = tune.vanguardAhead;
        if (p.squadSize > 1) {
            lateral = p.squadIndex == 0 ? tune.vanguardSide : (p.squadIndex == 1 ? -tune.vanguardSide : 0.0f);
            if (p.squadIndex >= 2) ahead += 1.5f;
        }
        Vec3 spot = Add(vanguardAnchor_, Add(Scale(vanguardDir_, ahead), Scale(side, lateral)));
        spot.y = vanguardAnchor_.y;
        return spot;
    }
    void VanguardPlace(const Perception& p, Command& c) {
        haveAnchor_ = following_ = false;
        idleUntil_ = 0;
        const Vec3 spot = VanguardSpot(p);
        const float away = Flat(p.self, spot);
        const bool walking = Arrive(p, c, spot);
        c.mode = walking ? Mode::Follow : Mode::Idle;
        if (walking) { c.run = away > tune.runFrom; return; }
        if (!Settled(p.now)) return;
        c.look = true;
        c.lookAt = Add(spot, Add(Scale(vanguardDir_, 8.0f), {0.0f, 1.4f, 0.0f}));
    }
    // Its place in a fight in the attack formation: between the leader and the
    // enemy, ahead of him on its side, not closer to the enemy than the weapon wants.
    Vec3 VanguardFightSpot(const Perception& p, const Enemy& e, float keepMin) const {
        Vec3 dir = {e.pos.x - p.leader.x, 0.0f, e.pos.z - p.leader.z};
        const float len = Length(dir);
        dir = len > 0.1f ? Scale(dir, 1.0f / len) : FromYaw(p.leaderYaw);
        const Vec3 side = {-dir.z, 0.0f, dir.x};
        float sign = p.squadIndex == 0 ? 1.0f : (p.squadIndex == 1 ? -1.0f : 0.0f);
        if (tune.ownPlaces && p.squadSize <= 1) sign = tune.combatSideSign;
        Vec3 spot = Add(p.leader, Add(Scale(dir, tune.vanguardFight), Scale(side, sign * tune.combatSide)));
        const float d = Flat(spot, e.pos);
        if (d < keepMin) spot = Sub(spot, Scale(dir, keepMin - d));
        spot.y = p.leader.y;
        return spot;
    }
    // Attack formation in a fight: it takes its place ahead of the leader. A target
    // it sees is not walked at (08.10.: the bots closed in to 3 m on enemies they
    // already had in clear sight at 6 m and more).
    bool VanguardCombat(const Perception& p, const Enemy& e, float dist, float keepMin, float keepMax, float gap, Command& c) {
        (void)dist; (void)keepMax; (void)gap;
        const uint32_t now = p.now;
        if (now < vanguardCheckAt_) return false;
        vanguardCheckAt_ = now + tune.vanguardRelocateMs;
        const Vec3 spot = VanguardFightSpot(p, e, keepMin);
        if (Flat(p.self, spot) <= tune.combatArrive * 1.5f || WayNearEnemy(p, p.self, spot, tune.dangerPath, tune.onUs) || NearMate(p, spot, tune.mateSpace))
            return false;
        StartCombatMove(spot, now, c);
        stepping_ = false;
        c.why = MoveWhy::Relocate;
        return true;
    }
    // The nearest live enemy within alertHoldRange of the bot or the leader, seen or not.
    const Enemy* Threat(const Perception& p) const {
        const Enemy* best = nullptr;
        float bestDist = 1e9f;
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
            const Enemy& e = p.enemies[i];
            if (e.health <= 0.0f) continue;
            if (e.maxHealth >= tune.immortalHealth && e.limbsLost >= tune.immortalLimbsOff) continue;   // a crippled Ubermorph holds nobody
            if (e.regenerating) continue;   // nor one regrowing: no gun raised at it either
            const float mine = Distance(p.self, e.pos);
            if (mine > tune.alertHoldRange && Distance(p.leader, e.pos) > tune.alertHoldRange) continue;
            if (!Aware(p, e)) { ++unawareSkips; continue; }
            if (mine < bestDist) { bestDist = mine; best = &e; }
        }
        return best;
    }
    // Enemies seen lately, by id; Aware() asks it.
    struct Sighting { uint32_t id = 0, at = 0; };
    Sighting sightings_[kMaxEnemies * 2];
    void RememberSightings(const Perception& p) {
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
            const Enemy& e = p.enemies[i];
            if (!e.visible || e.health <= 0.0f) continue;
            Sighting* slot = &sightings_[0];
            for (Sighting& s : sightings_) {
                if (s.id == e.id) { slot = &s; break; }
                if (s.at < slot->at) slot = &s;
            }
            *slot = Sighting{e.id, p.now ? p.now : 1u};
        }
    }
    bool Aware(const Perception& p, const Enemy& e) const {
        if (e.visible || e.objective || e.victim) return true;
        if (Length(e.vel) >= tune.threatMoveSpeed && (Distance(p.self, e.pos) <= tune.threatHearRange || Distance(p.leader, e.pos) <= tune.threatHearRange))
            return true;
        if (tune.humanSenses && SquadShoots(p, e)) return true;
        for (const Sighting& s : sightings_)
            if (s.id == e.id && s.at && p.now - s.at <= tune.threatMemoryMs) return true;
        return false;
    }
    // A mate aims at it, or the leader shoots its way (within 30 deg of where he faces).
    bool SquadShoots(const Perception& p, const Enemy& e) const {
        for (int m = 0; m < p.mateCount && m < kMaxMates; ++m)
            if (p.mates[m].aiming && p.mates[m].target == e.id) return true;
        if (!p.leaderShooting || Distance(p.leader, e.pos) > tune.threatHearRange) return false;
        const Vec3 to = {e.pos.x - p.leader.x, 0.0f, e.pos.z - p.leader.z};
        return Length(to) < 1.0f || std::fabs(WrapAngle(std::atan2(to.x, to.z) - p.leaderYaw)) <= 0.5236f;
    }
    static float WrapAngle(float a) {
        while (a > 3.14159265f) a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        return a;
    }

    // Human senses: what the bot sees of what has a clear ray, and what it hears.
    struct Heard { uint32_t id = 0, at = 0, readyAt = 0; bool counted = false; };
    struct InView { uint32_t id = 0, at = 0; };
    Heard heard_[kMaxEnemies * 2];
    InView inView_[kMaxEnemies * 2];
    uint32_t hitId_ = 0, hitReadyAt_ = 0;
    bool hitCounted_ = false;
    float senseHealth_ = 1.0f;
    uint32_t SenseReact(bool urgent) {
        const uint32_t ms = tune.reactMaxMs ? Between(tune.reactMinMs, tune.reactMaxMs) : Between(tune.hearReactMinMs, tune.hearReactMaxMs);
        return urgent ? ms / 3u : ms;
    }
    bool Hears(const Perception& p, const Enemy& e) const {
        if (e.health <= 0.0f || e.objective) return false;
        if (e.id == p.leaderGrabber && p.leaderGrabber) return true;
        if (Distance(p.self, e.pos) > tune.threatHearRange) return false;
        return Length(e.vel) >= tune.threatMoveSpeed || e.victim || SquadShoots(p, e);
    }
    // Skips its reaction in PickTarget: it was spent turning to the sound or the hit.
    void NoticeNow(uint32_t id, uint32_t now) {
        Seen* slot = &noticed_[0];
        for (Seen& s : noticed_) {
            if (s.id == id) { slot = &s; break; }
            if (s.seenAt < slot->seenAt) slot = &s;
        }
        *slot = Seen{id, now, now};
    }
    void Senses(Perception& p) {
        const int n = p.enemyCount < kMaxEnemies ? p.enemyCount : kMaxEnemies;
        for (int i = 0; i < n; ++i) p.enemies[i].inLine = p.enemies[i].visible;
        if (!tune.humanSenses) return;
        const uint32_t now = p.now ? p.now : 1u;
        // Hurt: the one going for it, else the nearest live enemy within hitRange.
        if (p.health01 < senseHealth_ - 0.005f) {
            const Enemy* by = nullptr;
            float best = 1e9f;
            for (int i = 0; i < n; ++i) {
                const Enemy& e = p.enemies[i];
                if (e.health <= 0.0f || e.objective) continue;
                const bool onMe = p.selfId && e.victim == p.selfId;
                const float d = Distance(p.self, e.pos);
                if (!onMe && d > tune.hitRange) continue;
                const float score = onMe ? d - 100.0f : d;
                if (score < best) { best = score; by = &e; }
            }
            if (by && (by->id != hitId_ || now - hitReadyAt_ > tune.trackMemoryMs)) {
                hitId_ = by->id;
                hitReadyAt_ = now + SenseReact(true);
                hitCounted_ = false;
            }
        }
        senseHealth_ = p.health01;
        for (int i = 0; i < n; ++i) {
            const Enemy& e = p.enemies[i];
            if (!Hears(p, e)) continue;
            Heard* slot = nullptr;
            Heard* oldest = &heard_[0];
            for (Heard& h : heard_) {
                if (h.id == e.id) { slot = &h; break; }
                if (h.at < oldest->at) oldest = &h;
            }
            if (!slot || now - slot->at > 1500u) {
                const bool urgent = (p.selfId && e.victim == p.selfId) || e.id == p.leaderGrabber || Distance(p.self, e.pos) < tune.closeRadius;
                if (!slot) slot = oldest;
                *slot = Heard{e.id, now, now + SenseReact(urgent), false};
            }
            slot->at = now;
        }
        const float half = tune.viewAngleDeg * 0.5f * 0.01745329f;
        for (int i = 0; i < n; ++i) {
            Enemy& e = p.enemies[i];
            if (!e.inLine || e.objective || !p.viewYawKnown) continue;
            bool inCone = Distance(p.self, e.pos) <= tune.peripheral;
            if (!inCone) {
                const Vec3 to = {e.chest.x - p.self.x, 0.0f, e.chest.z - p.self.z};
                inCone = Length(to) < 0.1f || std::fabs(WrapAngle(std::atan2(to.x, to.z) - p.viewYaw)) <= half;
            }
            InView* view = nullptr;
            InView* oldest = &inView_[0];
            for (InView& v : inView_) {
                if (v.id == e.id) { view = &v; break; }
                if (v.at < oldest->at) oldest = &v;
            }
            if (inCone) {
                if (!view) view = oldest;
                *view = InView{e.id, now};
                continue;
            }
            if (view && now - view->at <= tune.trackMemoryMs) continue;   // seen a moment ago: still tracked
            bool known = false;
            for (Heard& h : heard_) {
                if (h.id != e.id || now - h.at > 1500u || now < h.readyAt) continue;
                known = true;
                if (!h.counted) { h.counted = true; ++heardTurns; NoticeNow(e.id, now); }
            }
            if (!known && e.id == hitId_ && now >= hitReadyAt_ && now - hitReadyAt_ <= tune.trackMemoryMs) {
                known = true;
                if (!hitCounted_) { hitCounted_ = true; ++hitTurns; NoticeNow(e.id, now); }
            }
            if (!known) { e.visible = false; ++fovHidden; }
        }
    }

    // Human aim, per target: the pause, the flick's error and its spring, the wander.
    uint32_t flickOf_ = 0, flickStart_ = 0, flickAimAt_ = 0, flickTick_ = 0, jitAt_ = 0;
    bool flickShot_ = false, flickSettled_ = false;
    float flickH_ = 0.0f, flickV_ = 0.0f, flickVelH_ = 0.0f, flickVelV_ = 0.0f;
    float jitH_ = 0.0f, jitV_ = 0.0f, jitGoalH_ = 0.0f, jitGoalV_ = 0.0f, leadScale_ = 1.0f;
    void ResetFlick() {
        flickOf_ = 0;
        flickH_ = flickV_ = flickVelH_ = flickVelV_ = jitH_ = jitV_ = jitGoalH_ = jitGoalV_ = 0.0f;
        jitAt_ = 0;
        leadScale_ = 1.0f;
    }
    void HumanFlick(const Perception& p, Command& c) {
        if (!tune.humanAim || c.rescue) { ResetFlick(); return; }
        if (!c.look) return;
        const float skill = tune.aimSkill < 0.0f ? 0.0f : (tune.aimSkill > 1.0f ? 1.0f : tune.aimSkill);
        const float slack = 1.5f - skill;
        const Vec3 eye = Add(p.self, {0.0f, 1.5f, 0.0f});
        const Vec3 to = Sub(c.lookAt, eye);
        const float dist = Length(to);
        if (dist < 0.3f) return;
        float speed = 0.0f;
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i)
            if (p.enemies[i].id == c.target) speed = Length(p.enemies[i].vel);
        if (speed > 6.0f) speed = 6.0f;
        const bool snap = DodgerTarget(p, c.target);
        if (c.target != flickOf_) {
            flickOf_ = c.target;
            flickStart_ = flickTick_ = p.now;
            flickShot_ = flickSettled_ = false;
            flickAimAt_ = p.now + (snap ? tune.aimDelayMinMs : static_cast<uint32_t>(static_cast<float>(Between(tune.aimDelayMinMs, tune.aimDelayMaxMs)) * slack));
            if (snap) ++dodgerSnaps;
            // Overshoot or fall short along the way the eyes turn; any way for a small turn.
            float dirH = Unit() * 2.0f - 1.0f, dirV = (Unit() * 2.0f - 1.0f) * 0.4f, flickDeg = 0.0f;
            if (p.viewYawKnown) {
                const float d = WrapAngle(std::atan2(to.x, to.z) - p.viewYaw);
                flickDeg = std::fabs(d) * 57.2957795f;
                if (flickDeg > 2.0f) { dirH = d > 0.0f ? 1.0f : -1.0f; dirV = (Unit() * 2.0f - 1.0f) * 0.3f; }
            }
            const float len = std::sqrt(dirH * dirH + dirV * dirV);
            if (len > 0.001f) { dirH /= len; dirV /= len; }
            float err = (tune.flickErrorDeg + tune.flickErrorPerDeg * flickDeg) * slack * (1.0f + dist / 30.0f) * (1.0f + speed / 5.0f);
            if (err > tune.flickErrorMaxDeg) err = tune.flickErrorMaxDeg;
            err *= 0.6f + 0.4f * Unit();
            const bool over = Unit() < 0.65f;
            if (over) ++overshoots; else ++undershoots;
            flickH_ = dirH * err * (over ? 1.0f : -1.0f);
            flickV_ = dirV * err * (over ? 1.0f : -1.0f);
            flickVelH_ = flickVelV_ = 0.0f;
            leadScale_ = 1.0f + (Unit() * 2.0f - 1.0f) * tune.leadError * slack;
        }
        if (p.now < flickAimAt_) {   // still reacting: the eyes stay where they are
            if (c.fire || c.holdFire) ++acquireHolds;
            c.fire = c.holdFire = false;
            c.look = false;
            flickTick_ = p.now;
            return;
        }
        uint32_t dtMs = p.now - flickTick_;
        flickTick_ = p.now;
        if (dtMs > 100) dtMs = 100;
        const float w = tune.aimFreq * (0.6f + 0.4f * skill), z = tune.aimDamping;
        for (uint32_t t = 0; t < dtMs; t += 5) {
            const float h = static_cast<float>(dtMs - t < 5 ? dtMs - t : 5) / 1000.0f;
            flickVelH_ += (-w * w * flickH_ - 2.0f * z * w * flickVelH_) * h;
            flickVelV_ += (-w * w * flickV_ - 2.0f * z * w * flickVelV_) * h;
            flickH_ += flickVelH_ * h;
            flickV_ += flickVelV_ * h;
        }
        if (!flickSettled_ && (snap || std::sqrt(flickH_ * flickH_ + flickV_ * flickV_) <= tune.aimSettleDeg || p.now - flickAimAt_ >= tune.aimSettleMaxMs)) {
            flickSettled_ = true;
            ++settles;
            settleMsSum += p.now - flickStart_;
        }
        if (!flickSettled_) {
            if (c.fire || c.holdFire) ++acquireHolds;
            c.fire = c.holdFire = false;
        }
        // The wander while tracking, wider far off and on a fast mover.
        const float sigma = tune.aimJitterDeg * slack * (1.0f + dist / 25.0f) * (1.0f + speed / 6.0f);
        if (!jitAt_ || p.now >= jitAt_) {
            const float a = Unit() * 6.2831853f, r = std::sqrt(Unit()) * sigma;
            jitGoalH_ = std::cos(a) * r;
            jitGoalV_ = std::sin(a) * r * 0.6f;
            jitAt_ = p.now + tune.aimJitterMs;
        }
        const float k = dtMs >= 120 ? 1.0f : static_cast<float>(dtMs) / 120.0f;
        jitH_ += (jitGoalH_ - jitH_) * k;
        jitV_ += (jitGoalV_ - jitV_) * k;
        const float flat = std::sqrt(to.x * to.x + to.z * to.z);
        const Vec3 right = flat > 0.1f ? Vec3{to.z / flat, 0.0f, -to.x / flat} : Vec3{1.0f, 0.0f, 0.0f};
        const float H = (flickH_ + jitH_) * 0.01745329f, V = (flickV_ + jitV_) * 0.01745329f;
        c.lookAt = Add(c.lookAt, Add(Scale(right, dist * std::tan(H)), {0.0f, dist * std::tan(V), 0.0f}));
        c.lookVel = Scale(c.lookVel, leadScale_);
    }
public:
    // Human senses and aim (counts; frames x enemies for fovHidden).
    uint32_t fovHidden = 0;       // a clear ray but outside the view cone, neither tracked, heard nor hit
    uint32_t heardTurns = 0;      // heard enemies with a clear ray turned to after the reaction
    uint32_t hitTurns = 0;        // turned to what hurt it
    uint32_t acquisitions = 0, acquireMsSum = 0;   // first shot at a new target, ms from picking it
    uint32_t overshoots = 0, undershoots = 0;      // how a flick landed
    uint32_t settles = 0, settleMsSum = 0;         // the flick's error settled, ms from picking the target
    mutable uint32_t unawareSkips = 0;   // enemies passed over by Threat/Hostile, unseen and quiet (frames x enemies)
    // Fight diagnostics (frames unless named otherwise).
    uint32_t aimWaits = 0;        // a shot due by its cadence, the aim not on the point yet
    uint32_t acquireHolds = 0;    // a shot held while a new target is first aimed at
    uint32_t heldShots = 0;       // shots held back after the brain chose them; their timers did not start
    // Twitchers (dodgers) against plain slashers as the target, frames: all, with a shot, in sight, fast (dodgers only).
    uint32_t dodgerFrames = 0, dodgerShots = 0, dodgerSeenFrames = 0, dodgerFastFrames = 0, dodgerSnaps = 0;
    uint32_t slasherFrames = 0, slasherShots = 0, slasherSeenFrames = 0;
    // Priority stasis: casts chosen at twitchers and stalkers; frames a priority target waited for energy, cooldown, a player in the burst.
    uint32_t stasisOnTwitchers = 0, stasisOnStalkers = 0, stasisNoEnergy = 0, stasisCooling = 0, stasisPlayerNear = 0;
    uint32_t stasisAimFrames = 0, stasisAimGiveUps = 0;   // frames aiming a stasis; aims given up as the target kept dodging
    uint32_t threatAims = 0;      // turned toward an enemy it sees but has not taken as its target
    uint32_t breakAwayFrames = 0; // frames it ran back to a leader beyond kiteFar in a fight, gun down
    uint32_t kiteFrames = 0;      // frames it came back aimed and shooting (Kite)
    uint32_t regenSkips = 0;      // a regrowing Ubermorph passed over as a target
    uint32_t crowdStays = 0;      // surrounded, kept the crowd gun in hand instead of a change
    uint32_t desperateFrames = 0, desperateBlasts = 0, desperateHumanHolds = 0;   // buried in enemies; its blasts; held for a human near
    uint32_t unseenLookSkips = 0; // frames its unseen target was not looked at through a wall
    uint32_t unseenThreatFollows = 0; // frames it kept following past an enemy it cannot see
    uint32_t joinFrames = 0;      // frames it walked over to a fight it could not see
    uint32_t alertFrames = 0;     // frames it stood alert, gun up, for an enemy out of sight
    uint32_t backOffs = 0;        // backing off enemies close by
    uint32_t limbSwitches = 0;    // the aim moved on to the next limb
    uint32_t mateLaneHolds = 0;   // a shot held back for a mate in the line
    uint32_t mateLaneSteps = 0;   // steps out of a mate's line of fire (each step, not frames)
private:
    // Face its middle, swing, stand through the swing, then back off.
    void MeleeAt(const Perception& p, const Enemy& e, Command& c) {
        const uint32_t now = p.now;
        c.look = true;
        c.lookAt = e.chest;
        const Vec3 to = {e.chest.x - p.self.x, 0.0f, e.chest.z - p.self.z};
        const float len = Length(to);
        if (p.selfYawKnown && len > 0.05f) {
            float off = std::atan2(to.x, to.z) - p.selfYaw;
            while (off > 3.14159265f) off -= 6.2831853f;
            while (off < -3.14159265f) off += 6.2831853f;
            if (std::fabs(off) > tune.meleeFaceDeg * 0.01745329f && now - lastMelee_ >= tune.meleeHoldMs) {
                c.move = true;   // a short step at it turns the body
                c.moveTo = Add(p.self, Scale(to, tune.meleeFaceStep / len));
                return;
            }
        }
        if (now - lastMelee_ >= nextMeleeGap_) {
            Defer(kOnMelee, &Brain::lastMelee_, now);
            Defer(kOnMelee, &Brain::nextMeleeGap_, Between(tune.meleeMinMs, tune.meleeMaxMs));
            c.melee = true;
            return;
        }
        if (now - lastMelee_ < tune.meleeHoldMs) return;   // mid-swing: stand
        Retreat(p, e, c);
    }
    void StartCombatMove(const Vec3& spot, uint32_t now, Command& c) {
        combatSpot_ = spot;
        combatMoving_ = true;
        combatUntil_ = now + tune.combatMoveMaxMs;
        c.move = true;
        c.moveTo = spot;
    }

    // A mate stands, or is headed, within `radius` of the point.
    bool NearMate(const Perception& p, const Vec3& point, float radius) const {
        for (int m = 0; m < p.mateCount && m < kMaxMates; ++m) {
            const Mate& mate = p.mates[m];
            if (Flat(mate.pos, point) < radius || (mate.moving && Flat(mate.goal, point) < radius)) return true;
        }
        return false;
    }
    // Teammates keep their own room: a goal on a mate's place (or where it is
    // headed) moves off it; standing on top of one, the bot steps aside.
    void Spread(const Perception& p, Command& c) const {
        if (c.mode == Mode::Pickup || c.mode == Mode::Rush || c.mode == Mode::Heal || c.mode == Mode::Loot) return;
        for (int m = 0; m < p.mateCount && m < kMaxMates; ++m) {
            const Mate& mate = p.mates[m];
            if (c.move) {
                for (int k = 0; k < (mate.moving ? 2 : 1); ++k) {
                    const Vec3& at = k == 0 ? mate.pos : mate.goal;
                    Vec3 off = {c.moveTo.x - at.x, 0.0f, c.moveTo.z - at.z};
                    const float d = Length(off);
                    if (d >= tune.mateSpace) continue;
                    if (d < 0.05f) off = FromYaw(static_cast<float>(p.squadIndex) * 2.1f + 0.4f);
                    else off = Scale(off, 1.0f / d);
                    c.moveTo = {at.x + off.x * tune.mateSpace, c.moveTo.y, at.z + off.z * tune.mateSpace};
                }
            } else if (Flat(p.self, mate.pos) < tune.mateSpace * 0.55f) {
                Vec3 away = {p.self.x - mate.pos.x, 0.0f, p.self.z - mate.pos.z};
                const float d = Length(away);
                away = d > 0.05f ? Scale(away, 1.0f / d) : FromYaw(static_cast<float>(p.squadIndex) * 2.1f + 0.4f);
                c.move = true;
                c.moveTo = Add(p.self, Scale(away, tune.mateSpace));
            }
        }
    }
    // A player (the leader, a partner, a mate) in the way of a shot from the bot's eye to the point.
    bool LaneBlocked(const Perception& p, const Vec3& to) const { return LaneBlocker(p, to) != 0; }
    // The humans only: 1 the leader, 2 another human.
    int HumanLaneBlocker(const Perception& p, const Vec3& to) const {
        const Vec3 eye = {p.self.x, p.self.y + 1.5f, p.self.z};
        if (BodyInLine(eye, to, p.leader)) return 1;
        for (int i = 0; i < p.humanCount && i < kMaxPlayers; ++i) if (BodyInLine(eye, to, p.humans[i])) return 2;
        return 0;
    }
    bool HumanNear(const Perception& p, const Vec3& point, float r) const {
        if (Distance(p.leader, point) <= r) return true;
        for (int i = 0; i < p.humanCount && i < kMaxPlayers; ++i) if (Distance(p.humans[i], point) <= r) return true;
        return false;
    }
    // Whether a body standing at feet is in the line from eye to `to`.
    bool BodyInLine(const Vec3& eye, const Vec3& to, const Vec3& feet) const {
        const Vec3 d = Sub(to, eye);
        const float len2 = Dot(d, d);
        if (len2 < 0.25f) return false;
        const Vec3 body = {feet.x, feet.y + 1.0f, feet.z};
        const float t = Dot(Sub(body, eye), d) / len2;
        if (t <= 0.0f || t >= 1.0f) return false;
        Vec3 off = Sub(body, Add(eye, Scale(d, t)));
        const float up = std::fabs(off.y);
        off.y = up > 0.8f ? up - 0.8f : 0.0f;   // a body is tall: anything from the knees to the head counts
        return Length(off) < tune.laneWidth;
    }
    // Who is in the way of a shot: 0 nobody, 1 the leader, 2 another player, 3 a mate.
    int LaneBlocker(const Perception& p, const Vec3& to) const {
        const Vec3 eye = {p.self.x, p.self.y + 1.5f, p.self.z};
        if (BodyInLine(eye, to, p.leader)) return 1;
        for (int m = 0; m < p.mateCount && m < kMaxMates; ++m) if (BodyInLine(eye, to, p.mates[m].pos)) return 3;
        for (int i = 0; i < p.playerCount && i < kMaxPlayers; ++i) {
            bool mate = false;   // the players list has the mates too, as the last scan saw them
            for (int m = 0; m < p.mateCount && m < kMaxMates; ++m) mate = mate || Flat(p.mates[m].pos, p.players[i]) < 0.5f;
            if (!mate && BodyInLine(eye, to, p.players[i])) return 2;
        }
        return 0;
    }
    // No shot through a player: hold it and, standing, step out of the line; for
    // the leader and a mate only once they have stood in it a while (one walking
    // through it moves nobody, and a mate steps out of it on its own).
    void ClearLane(const Perception& p, Command& c) {
        const bool laneAlt = plan_ == Plan::Mine || plan_ == Plan::Grenade || plan_ == Plan::RipperThrow || plan_ == Plan::Detonator ||
                             plan_ == Plan::FlameBlast || plan_ == Plan::ForceBeam || plan_ == Plan::GrenadeFeet || plan_ == Plan::DetonatorClose;
        const bool shooting = c.fire || c.stasis || (c.holdFire && plan_ != Plan::ContactCharge) || (c.altFire && laneAlt);
        // The leader's time in the line counts between the shots too. Desperate, a mate in the line is no reason.
        const int blocker = c.look ? (desperate_ ? HumanLaneBlocker(p, c.lookAt) : LaneBlocker(p, c.lookAt)) : 0;
        if (blocker != 1) leaderInLaneSince_ = 0;
        else if (!leaderInLaneSince_) leaderInLaneSince_ = p.now;
        if (blocker != 3) mateInLaneSince_ = 0;
        else if (!mateInLaneSince_) mateInLaneSince_ = p.now;
        if (!shooting || !blocker) return;
        c.fire = c.stasis = false;
        if (plan_ != Plan::ContactCharge) c.holdFire = false;
        if (laneAlt) c.altFire = false;
        c.laneBlocked = true;
        if (blocker == 3) ++mateLaneHolds;
        if (c.move) return;
        // Freeing him, the bot steps out of his line at once: he stands still in the grab anyway.
        if (blocker == 1 && !c.rescue && p.now - leaderInLaneSince_ < tune.laneStepAfterMs) return;
        if (blocker == 3 && p.now - mateInLaneSince_ < tune.laneStepAfterMs) return;
        Vec3 dir = {c.lookAt.x - p.self.x, 0.0f, c.lookAt.z - p.self.z};
        const float len = Length(dir);
        if (len < 0.1f) return;
        dir = Scale(dir, 1.0f / len);
        const Vec3 side = {-dir.z, 0.0f, dir.x};
        const float sign = p.squadIndex % 2 == 0 ? 1.0f : -1.0f;
        c.move = true;
        c.moveTo = Add(p.self, Scale(side, sign * tune.laneStep));
        c.why = MoveWhy::Lane;
    }
    // Standing in a mate's line of fire: a step out of it, to the side it already
    // leans to, held a moment so it does not step back in at once. It keeps aiming
    // and shooting meanwhile.
    uint32_t mateLaneUntil_ = 0, mateInLaneSince_ = 0;
    Vec3 mateLaneTo_;
    void StepOutOfMateLanes(const Perception& p, Command& c) {
        if (p.grabbed || c.antic != Antic::None || (c.mode != Mode::Combat && c.mode != Mode::Idle && c.mode != Mode::Follow)) {
            mateLaneUntil_ = 0;
            return;
        }
        if (p.now < mateLaneUntil_) {
            if (!c.move && Flat(p.self, mateLaneTo_) > 0.25f) { c.move = true; c.moveTo = mateLaneTo_; c.run = false; c.why = MoveWhy::Lane; }
            return;
        }
        if (c.move) return;   // walking off anyway
        for (int m = 0; m < p.mateCount && m < kMaxMates; ++m) {
            const Mate& mate = p.mates[m];
            if (!mate.aiming) continue;
            const Vec3 eye = {mate.pos.x, mate.pos.y + 1.5f, mate.pos.z};
            if (!BodyInLine(eye, mate.aimAt, p.self)) continue;
            Vec3 dir = {mate.aimAt.x - mate.pos.x, 0.0f, mate.aimAt.z - mate.pos.z};
            const float len = Length(dir);
            if (len < 0.5f) continue;
            dir = Scale(dir, 1.0f / len);
            const Vec3 side = {-dir.z, 0.0f, dir.x};
            const float lateral = Dot(Vec3{p.self.x - mate.pos.x, 0.0f, p.self.z - mate.pos.z}, side);
            float sign = std::fabs(lateral) > 0.1f ? (lateral > 0.0f ? 1.0f : -1.0f) : (p.squadIndex % 2 == 0 ? 1.0f : -1.0f);
            Vec3 spot = Add(p.self, Scale(side, sign * (tune.laneWidth + 0.5f - std::fabs(lateral))));
            if (WayNearEnemy(p, p.self, spot, tune.dangerPath, tune.onUs)) {   // the other side, a little farther
                sign = -sign;
                spot = Add(p.self, Scale(side, sign * (tune.laneWidth + 0.5f + std::fabs(lateral))));
                if (WayNearEnemy(p, p.self, spot, tune.dangerPath, tune.onUs)) continue;
            }
            spot.y = p.self.y;
            mateLaneTo_ = spot;
            mateLaneUntil_ = p.now + tune.mateLaneHoldMs;
            c.move = true;
            c.moveTo = spot;
            c.run = false;
            c.why = MoveWhy::Lane;
            ++mateLaneSteps;
            return;
        }
    }

    // A weapon made for the distance it has to the enemy (a flame, a ripper blade, a shove).
    bool CloseRangePlan(float keepMax) const {
        return keepMax <= tune.closeWeaponRange &&
               (plan_ == Plan::Flame || plan_ == Plan::RipperHold || plan_ == Plan::ForcePush || plan_ == Plan::ContactSlam);
    }
    // Enemies it knows of close by (one within backOffRange, or crowdCount within
    // crowdRange): a step back from them all, weighted by how close each is, to
    // whichever side keeps it farthest from every one of them. False when none is that close.
    bool BackOff(const Perception& p, Command& c) {
        if (tune.backOffRange <= 0.0f && tune.crowdCount <= 0) return false;
        const float range = tune.crowdRange > tune.backOffRange ? tune.crowdRange : tune.backOffRange;
        Vec3 away{};
        int crowd = 0, counted = 0;
        bool close = false;
        const Enemy* closeBy[kMaxEnemies];
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
            const Enemy& e = p.enemies[i];
            if (e.health <= 0.0f || e.objective || e.regenerating) continue;
            if (e.maxHealth >= tune.immortalHealth && e.limbsLost >= tune.immortalLimbsOff) continue;
            if (std::fabs(e.pos.y - p.self.y) > 2.5f || !Aware(p, e)) continue;   // another floor
            const float d = Flat(p.self, e.pos);
            if (d > range) continue;
            closeBy[counted++] = &e;
            if (d <= tune.crowdRange) ++crowd;
            close = close || d < tune.backOffRange;
            Vec3 from = {p.self.x - e.pos.x, 0.0f, p.self.z - e.pos.z};
            const float len = Length(from);
            from = len > 0.05f ? Scale(from, 1.0f / len) : FromYaw(p.leaderYaw);
            away = Add(away, Scale(from, (range - d) / range + 0.1f));
        }
        if (!close && (tune.crowdCount <= 0 || crowd < tune.crowdCount)) return false;
        float len = Length(away);
        if (len < 0.05f) {   // evenly around it: toward the leader
            away = {p.leader.x - p.self.x, 0.0f, p.leader.z - p.self.z};
            len = Length(away);
            if (len < 0.05f) { away = FromYaw(p.leaderYaw + 3.14159265f); len = 1.0f; }
        }
        away = Scale(away, 1.0f / len);
        // Straight back, or turned up to 90 degrees: whichever ends farthest from the nearest of them.
        static const float kTurns[] = {0.0f, 0.785f, -0.785f, 1.571f, -1.571f};
        Vec3 best = away;
        float bestClear = -1.0f;
        for (float t : kTurns) {
            const float cs = std::cos(t), sn = std::sin(t);
            const Vec3 dir = {away.x * cs - away.z * sn, 0.0f, away.x * sn + away.z * cs};
            const Vec3 spot = Add(p.self, Scale(dir, tune.retreatStep));
            float clear = 1e9f;
            for (int k = 0; k < counted; ++k) { const float d = Flat(spot, closeBy[k]->pos); if (d < clear) clear = d; }
            if (clear > bestClear + 0.2f) { bestClear = clear; best = dir; }
            if (bestClear >= tune.backOffClear + tune.retreatStep) break;   // straight back is clear enough
        }
        RetreatAlong(p, best, c);
        c.backOff = true;
        ++backOffs;
        return true;
    }
    // Away from the enemy, but bent back toward the leader when the leash runs out.
    // Left behind, the step goes mostly toward the leader, bent away from the enemy
    // (around it when it stands in the way).
    void Retreat(const Perception& p, const Enemy& e, Command& c) {
        Vec3 away = Sub(p.self, e.pos);
        away.y = 0.0f;
        RetreatAlong(p, away, c);
    }
    void RetreatAlong(const Perception& p, Vec3 away, Command& c) {
        away.y = 0.0f;
        float len = Length(away);
        if (len < 0.01f) { away = FromYaw(p.leaderYaw); len = 1.0f; }
        // Holding, the spot takes the leader's part, on a shorter leash.
        const Vec3 anchor = Anchor(p);
        const float follow = p.orders.hold ? tune.holdLeash : tune.combatFollow, leash = p.orders.hold ? tune.holdLeash : tune.leash;
        Vec3 home = {anchor.x - p.self.x, 0.0f, anchor.z - p.self.z};
        const float homeLen = Length(home);
        if (homeLen > follow) {
            Vec3 dir = Add(Scale(home, 1.0f / homeLen), Scale(away, 0.6f / len));
            float dirLen = Length(dir);
            if (dirLen < 0.2f) { dir = {-away.z, 0.0f, away.x}; dirLen = len; }   // the enemy right on the way: sideways first
            c.move = true;
            c.moveTo = Add(p.self, Scale(dir, tune.retreatStep / dirLen));
            c.why = MoveWhy::Retreat;
            return;
        }
        Vec3 spot = Add(p.self, Scale(away, tune.retreatStep / len));
        if (Flat(spot, anchor) > leash) {
            const Vec3 toAnchor = Sub(anchor, spot);
            spot = Add(spot, Scale(toAnchor, 0.5f));
        }
        c.move = true;
        c.moveTo = spot;
        c.why = MoveWhy::Retreat;
    }

    const Item* PickItem(const Perception& p) {
        const Item* best = nullptr;
        float bestDist = tune.itemRange;
        for (int i = 0; i < p.itemCount && i < kMaxItems; ++i) {
            if (Ignored(p.items[i].id, p.now)) continue;
            bool mates = false;
            for (int m = 0; m < p.mateCount && m < kMaxMates; ++m) mates = mates || p.mates[m].item == p.items[i].id;
            if (mates) continue;   // a mate goes for it
            if (!p.items[i].botLoot && Distance(p.leader, p.items[i].pos) < tune.itemLeaderOwns) continue;
            if (p.orders.hold && Flat(p.orders.holdSpot, p.items[i].pos) > tune.holdItemRange) continue;
            const float d = Distance(p.self, p.items[i].pos);
            if (d < bestDist && (p.orders.hold || Distance(p.leader, p.items[i].pos) < tune.itemLeash)) { bestDist = d; best = &p.items[i]; }
        }
        if (best) pickAt_ = best->pos;
        return best;
    }
    struct IgnoredItem { uint32_t id = 0, until = 0; };
    IgnoredItem ignored_[16];
    unsigned ignoredNext_ = 0;
    uint32_t pickId_ = 0, pickSince_ = 0, pickUses_ = 0;
    Vec3 pickAt_;
    void Ignore(uint32_t id, uint32_t now) {
        ignored_[ignoredNext_++ % 16] = IgnoredItem{id, now + tune.itemIgnoreMs};
    }

    // A live enemy close enough to matter: no looting while one is about.
    bool Hostile(const Perception& p) const {
        for (int i = 0; i < p.enemyCount && i < kMaxEnemies; ++i) {
            const Enemy& e = p.enemies[i];
            if (e.health <= 0.0f || e.objective || !Aware(p, e)) continue;
            if (Distance(p.self, e.pos) <= tune.engageRange || Distance(p.leader, e.pos) <= tune.engageRange) return true;
        }
        return false;
    }
    uint32_t lastHostileAt_ = 0, lootId_ = 0, lootSince_ = 0, lootPressAt_ = 0;
    int lootStomps_ = 0, lootPresses_ = 0;
    // The nearest spot worth a visit; the bots' own kills a little before the leader's.
    const LootSpot* PickLoot(const Perception& p) const {
        if (!tune.lootWanted || !p.lootCount) return nullptr;
        if (lastHostileAt_ && p.now - lastHostileAt_ < tune.lootCalmMs) return nullptr;
        const LootSpot* best = nullptr;
        float bestScore = 1e9f;
        for (int i = 0; i < p.lootCount && i < kMaxLoot; ++i) {
            const LootSpot& s = p.loot[i];
            if (!s.id || Ignored(s.id, p.now)) continue;
            if (s.kind != LootKind::Locker && s.stomps >= tune.corpseStompsMax) continue;
            if (Flat(p.leader, s.pos) < tune.lootLeaderClear) continue;   // his to stomp or open
            bool mates = false;
            for (int m = 0; m < p.mateCount && m < kMaxMates; ++m) mates = mates || p.mates[m].loot == s.id;
            if (mates) continue;
            const float mine = Distance(p.self, s.pos), theirs = Distance(p.leader, s.pos);
            if (mine > tune.lootRange || theirs > tune.lootLeash) continue;
            const bool current = s.id == lootId_;
            if (!current && p.leaderMoving && Flat(p.leader, s.pos) > tune.lootStart) continue;
            if (p.orders.hold && Flat(p.orders.holdSpot, s.pos) > tune.holdItemRange) continue;
            float score = mine - (s.botKill ? tune.botKillBonus : 0.0f) - (current ? 1.0f : 0.0f);
            if (score < bestScore) { bestScore = score; best = &s; }
        }
        return best;
    }
    // One frame at a spot: walk there, then stomp (corpse, crate) or press use (locker).
    // False when the spot is given up.
    bool Loot(const Perception& p, const LootSpot& s, Command& c) {
        const uint32_t now = p.now;
        if (s.id != lootId_) { lootId_ = s.id; lootSince_ = now; lootStomps_ = 0; lootPresses_ = 0; lootPressAt_ = 0; }
        const int stomps = s.stomps > lootStomps_ ? s.stomps : lootStomps_;
        // A crate up on a wall: a stomp passes under it, a swing breaks it (06.10.:
        // its middle 1.0 m over the bots' feet, they stomped from 2 m off for ages).
        const bool wallBox = s.kind == LootKind::Crate && s.pos.y - p.self.y > tune.wallCrateHeight;
        const bool shoot = s.kind == LootKind::Crate && ShootsCrate(s.id) && CrateGun(p) && !p.orders.holdFire;
        const int most = shoot ? tune.crateShotsMax : (wallBox ? tune.wallCrateHitsMax : tune.corpseStompsMax);
        if (now - lootSince_ >= tune.lootGiveUpMs || (s.kind != LootKind::Locker && stomps >= most) ||
            (s.kind == LootKind::Locker && lootPresses_ >= tune.lockerTries)) {
            Ignore(s.id, now);
            lootId_ = 0;
            return false;
        }
        c.mode = Mode::Loot;
        c.loot = s.id;
        const float d = Flat(p.self, s.pos);
        if (s.kind == LootKind::Locker) {
            c.look = true;
            c.lookAt = Add(s.pos, {0.0f, 1.0f, 0.0f});
            if (p.lootOffered == s.id) {
                if (!lootPressAt_ || now - lootPressAt_ >= tune.lockerPressMs) { lootPressAt_ = now; c.use = true; ++lootPresses_; }
                return true;
            }
            c.move = d > 0.5f;
            c.moveTo = s.pos;
            c.run = d > 4.0f;
            return true;
        }
        // Shot open from where it stands, once in range with a clear line; till
        // then it walks on toward the crate.
        if (shoot && s.inSight && d <= tune.crateShootRange && d >= tune.crateShootMin) {
            c.aim = true;
            c.look = true;
            c.lookAt = wallBox ? s.pos : Add(s.pos, {0.0f, 0.3f, 0.0f});
            if (p.aimErrorDeg <= tune.crateToleranceDeg && now - lastFire_ >= tune.crateShotMs) { lastFire_ = now; c.fire = true; ++lootStomps_; }
            return true;
        }
        if (wallBox) {
            c.look = true;
            c.lookAt = s.pos;
            const Vec3 out = {p.self.x - s.pos.x, 0.0f, p.self.z - s.pos.z};
            const float len = Length(out);
            if (d > tune.wallCrateReach) {
                c.move = true;
                c.moveTo = len > 0.05f ? Add(Vec3{s.pos.x, p.self.y, s.pos.z}, Scale(out, tune.wallCrateStand / len)) : s.pos;
                c.run = d > 4.0f;
                return true;
            }
            if (p.selfYawKnown && len > 0.05f) {
                float off = std::atan2(-out.x, -out.z) - p.selfYaw;
                while (off > 3.14159265f) off -= 6.2831853f;
                while (off < -3.14159265f) off += 6.2831853f;
                if (std::fabs(off) > tune.meleeFaceDeg * 0.01745329f && now - lastStomp_ >= tune.meleeHoldMs) {
                    c.move = true;   // a short step at it turns the body
                    c.moveTo = Add(p.self, Scale(out, -tune.meleeFaceStep / len));
                    return true;
                }
            }
            if (now - lastStomp_ >= tune.wallCrateSwingMs) { lastStomp_ = now; c.melee = true; ++lootStomps_; }
            return true;
        }
        if (d > tune.stompLootReach) {
            c.move = true;
            c.moveTo = s.pos;
            c.run = d > 4.0f;
            if (d < 2.5f) { c.look = true; c.lookAt = s.pos; }
            return true;
        }
        c.look = true;
        c.lookAt = s.pos;
        if (now - lastStomp_ >= tune.stompIntervalMs) { lastStomp_ = now; c.stomp = true; ++lootStomps_; }
        return true;
    }
    // Half the crates, by id: every bot agrees on which.
    bool ShootsCrate(uint32_t id) const {
        if (tune.crateShootChance <= 0.0f) return false;
        const uint32_t roll = ((id >> 4) * 2654435761u) >> 16;
        return static_cast<float>(roll % 1000u) < tune.crateShootChance * 1000.0f;
    }
    // A gun that puts one shot where it aims, loaded, in hand.
    static bool CrateGun(const Perception& p) {
        if (p.activeSlot < 0 || p.activeSlot >= kGunSlots || p.reloading) return false;
        const GunSlot& g = p.guns[p.activeSlot];
        const bool plain = g.kind == Gun::PlasmaCutter || g.kind == Gun::PulseRifle || g.kind == Gun::SeekerRifle || g.kind == Gun::LineGun;
        return plain && g.clip > 0;
    }
    bool Ignored(uint32_t id, uint32_t now) const {
        for (const auto& i : ignored_) if (i.id == id && now < i.until) return true;
        return false;
    }

    // Walks to a spot with some slack: off beyond arriveStart, stopped within
    // arriveStop. Remembers since when it stands, and draws how long it stands
    // before it looks at anyone. Returns true while it walks.
    bool Arrive(const Perception& p, Command& c, const Vec3& spot) { return Arrive(p, c, spot, tune.arriveStart, tune.arriveStop); }
    bool Arrive(const Perception& p, Command& c, const Vec3& spot, float start, float stop) {
        if (p.now - lastArrive_ > 500) { walking_ = false; stillSince_ = 0; }   // back from a fight or a pickup: settle anew
        lastArrive_ = p.now;
        const float d = Flat(p.self, spot);
        walking_ = walking_ ? d > stop : d > start;
        c.move = walking_;
        c.moveTo = spot;
        if (walking_) {
            stillSince_ = 0;
        } else if (!stillSince_) {
            stillSince_ = p.now;
            settleMs_ = Between(tune.settleMinMs, tune.settleMaxMs);
            lookUntil_ = 0;
        }
        return walking_;
    }
    bool Settled(uint32_t now) const { return stillSince_ && now - stillSince_ >= settleMs_; }

    // While it walks it looks where it goes (no look command: the body turns
    // along its route). Standing, after a moment, mostly at the leader.
    void StandingLook(const Perception& p, Command& c) {
        const uint32_t now = p.now;
        if (walking_ || !Settled(now)) return;
        if (!lookUntil_ || now >= lookUntil_) {
            lookAtLeader_ = Unit() >= tune.glanceChance;
            // With its own place it mostly keeps an eye on its own direction.
            const float yaw = tune.ownPlaces && Unit() < 0.6f ? p.leaderYaw - (tune.watchAngleDeg + (Unit() - 0.5f) * 50.0f) * 0.01745329f
                                                               : Unit() * 6.2831853f;
            lookSpot_ = Add(p.self, Add(Scale(FromYaw(yaw), 8.0f), {0.0f, 1.2f + (Unit() - 0.5f) * 1.0f, 0.0f}));
            if (!lookAtLeader_ && p.mateCount > 0 && Unit() < 0.5f)
                lookSpot_ = Add(p.mates[Next() % static_cast<uint32_t>(p.mateCount)].pos, {0.0f, 1.6f, 0.0f});
            lookUntil_ = now + Between(tune.lookMinMs, tune.lookMaxMs);
        }
        c.look = true;
        // Right beside him it looks past him: from there his every small step
        // swung its head back and forth (06.10.).
        const bool tooClose = lookAtLeader_ && Flat(p.self, p.leader) < tune.leaderLookMin;
        if (tooClose) ++leaderLookSkips;
        c.lookAt = lookAtLeader_ && !tooClose ? Add(p.leader, {0.0f, 1.6f, 0.0f}) : lookSpot_;
    }
public:
    uint32_t LastPrankAt() const { return lastPrankAt_; }
    uint32_t rescueCloseIns = 0;    // rescue frames run at a grabber out of sight instead of shooting
    uint32_t leaderLookSkips = 0;   // standing looks at the leader turned away, him too close
    uint32_t nestHolds = 0;         // frames on a nest with no pod to shoot at
private:

    void FollowLeader(const Perception& p, Command& c) {
        c.mode = c.mode == Mode::Heal ? Mode::Heal : Mode::Follow;
        // Beside and a little behind the leader, never on top of him. The spot
        // stays put until the leader has gone a little way, so every turn of
        // his does not send the bot somewhere new.
        if (!haveAnchor_ || Flat(followAnchor_, p.leader) > tune.followReanchor) {
            // Alone: back and to one side. A squad: one each side, the third straight behind.
            float angle = 0.75f, distance = 2.5f;
            if (p.squadSize > 1) {
                angle = p.squadIndex == 0 ? 0.75f : (p.squadIndex == 1 ? -0.75f : 1.0f);
                if (p.squadIndex >= 2) distance = 3.5f;
            }
            Vec3 back = FromYaw(p.leaderYaw + 3.14159265f * angle);
            // Its own place from its profile: the angle is to his right as he sees it
            // (the yaw grows toward his left, see partnerRight).
            if (tune.ownPlaces) {
                back = FromYaw(p.leaderYaw - tune.followAngleDeg * 0.01745329f);
                distance = tune.followDistance;
            }
            followSpot_ = Add(p.leader, Scale(back, distance));
            followAnchor_ = p.leader;
            haveAnchor_ = true;
        }
        Arrive(p, c, followSpot_);
        StandingLook(p, c);
    }

    // The leader flees (the straitjacket run at the start): each bot runs right
    // behind him in a line, whatever else is about; no fight, no pickups, no stop
    // of its own. The line runs back along his walk, or his facing when he stands.
    void Chase(const Perception& p, Command& c) {
        c.mode = Mode::Rush;
        const float speed = Length(p.leaderVel);
        const Vec3 dir = speed > 0.5f ? Scale(p.leaderVel, 1.0f / speed) : FromYaw(p.leaderYaw);
        const Vec3 side = {-dir.z, 0.0f, dir.x};
        const float gap = tune.chaseGap + tune.chaseSpacing * static_cast<float>(p.squadIndex);
        const float lateral = p.squadIndex == 0 ? 0.0f : (p.squadIndex % 2 == 0 ? 1.0f : -1.0f) * tune.chaseSide;
        Vec3 spot = Add(p.leader, Add(Scale(dir, -gap), Scale(side, lateral)));
        spot.y = p.leader.y;
        c.move = Flat(p.self, spot) > tune.chaseArrive;
        c.moveTo = spot;
        c.run = true;
        c.why = MoveWhy::Follow;
        c.look = true;
        c.lookAt = Add(p.leader, {0.0f, 1.5f, 0.0f});
    }

    // The partner's place and look. The place moves with the leader only once he
    // has gone a step or turned well away, so it does not shuffle at every turn.
    void PartnerPose(const Perception& p, Command& c) {
        c.mode = Mode::Idle;
        idleUntil_ = 0;
        haveAnchor_ = false;
        float turn = p.leaderYaw - partnerYaw_;
        while (turn > 3.14159265f) turn -= 6.2831853f;
        while (turn < -3.14159265f) turn += 6.2831853f;
        const bool moved = !havePartner_ || Flat(partnerAnchor_, p.leader) > tune.partnerMove || std::fabs(turn) > tune.partnerTurnDeg * 0.01745329f;
        // A real move of the leader moves the place, after the bot's own moment;
        // his few steps back and forth do not.
        if (!moved) partnerWantAt_ = 0;
        else if (havePartner_ && !partnerWantAt_) partnerWantAt_ = p.now + React(false);
        if (moved && (!havePartner_ || p.now >= partnerWantAt_)) {
            partnerWantAt_ = 0;
            partnerAnchor_ = p.leader;
            partnerYaw_ = p.leaderYaw;
            havePartner_ = true;
            faceTries_ = 0;
            faceUntil_ = 0;
            partnerSide_ = 1.0f;
            partnerFallback_ = false;
            partnerStage_ = 0;
        }
        const Vec3 fwd = FromYaw(partnerYaw_), right = {-fwd.z, 0.0f, fwd.x};
        const bool second = p.squadIndex == 1;
        const float ahead = second ? tune.partner2Forward : tune.partnerForward, aside = second ? tune.partner2Right : tune.partnerRight;
        Vec3 spot = Add(partnerAnchor_, Add(Scale(fwd, ahead), Scale(right, aside * partnerSide_)));
        spot.y = partnerAnchor_.y;
        partnerSpot_ = spot;
        if (partnerFallback_) spot = partnerClosest_;
        const uint32_t now = p.now;
        const Vec3 head = Add(p.leader, {0.0f, 1.6f, 0.0f});
        // Mid face step: keep walking at him a moment, eyes on him.
        if (now < faceUntil_) {
            c.move = true;
            c.moveTo = faceTo_;
            c.look = true;
            c.lookAt = head;
            return;
        }
        // In from outside: first a point beyond the place (seen from the leader),
        // then the last stretch at him with the eyes on him.
        Vec3 outward = {spot.x - partnerAnchor_.x, 0.0f, spot.z - partnerAnchor_.z};
        const float reach = Length(outward);
        const bool approachOn = tune.partnerApproach > 0.0f && reach > 0.1f && !partnerFallback_;
        partnerApproach_ = approachOn ? Add(spot, Scale(outward, tune.partnerApproach / reach)) : spot;
        if (partnerStage_ == 0 && (!approachOn || Flat(p.self, partnerApproach_) < 0.4f)) partnerStage_ = 1;
        if (partnerStage_ == 1 && Flat(p.self, spot) <= tune.partnerArriveStop) partnerStage_ = 2;
        if (partnerStage_ == 0) {
            walking_ = true;
            stillSince_ = 0;
            c.move = true;
            c.moveTo = partnerApproach_;
            return;
        }
        if (partnerStage_ == 1) {
            walking_ = true;
            stillSince_ = 0;
            c.move = true;
            c.moveTo = spot;
            c.look = true;
            c.lookAt = head;
            return;
        }
        if (Arrive(p, c, spot, tune.partnerArriveStart, tune.partnerArriveStop)) {
            partnerStage_ = 0;   // pushed off its place: come in again
            return;
        }
        if (!Settled(now)) return;
        c.look = true;
        c.lookAt = head;
        if (!p.selfYawKnown || faceTries_ >= tune.partnerFaceTries || now - faceAt_ < tune.partnerFaceGapMs) return;
        Vec3 to = {p.leader.x - p.self.x, 0.0f, p.leader.z - p.self.z};
        const float gap = Length(to);
        // A face step never takes it much closer than its place is.
        float nearest = reach - tune.partnerFaceCloser;
        if (nearest < tune.partnerFaceNear) nearest = tune.partnerFaceNear;
        if (gap - nearest < 0.15f) return;
        float off = std::atan2(to.x, to.z) - p.selfYaw;
        while (off > 3.14159265f) off -= 6.2831853f;
        while (off < -3.14159265f) off += 6.2831853f;
        if (std::fabs(off) <= tune.partnerFaceDeg * 0.01745329f) return;
        to = Scale(to, 1.0f / gap);
        const float step = gap - tune.partnerFaceStep < nearest ? gap - nearest : tune.partnerFaceStep;
        faceTo_ = Add(p.self, Scale(to, step));
        faceUntil_ = now + tune.partnerFaceMs;
        faceAt_ = now;
        ++faceTries_;
        c.move = true;
        c.moveTo = faceTo_;
    }

    void Loiter(const Perception& p, Command& c) {
        const uint32_t now = p.now;
        c.mode = Mode::Idle;
        haveAnchor_ = false;
        const float spotGap = Flat(idleSpot_, p.leader);
        bool pick = false;
        if (!idleUntil_) {
            // Just stopped following: where it stands is fine when it is a sensible distance off.
            const float here = Flat(p.self, p.leader);
            if (here >= tune.idleMin - 0.5f && here <= tune.idleMax + 1.0f) idleSpot_ = p.self;
            else pick = true;
            idleUntil_ = now + Between(tune.idleMoveMinMs, tune.idleMoveMaxMs);
        } else if (spotGap > tune.idleMax + 2.0f || spotGap < tune.idleMin - 1.0f) {
            pick = true;   // the leader moved away from it, or onto it
        } else if (now >= idleUntil_) {
            pick = Unit() < tune.idleMoveChance;
            idleUntil_ = now + Between(tune.idleMoveMinMs, tune.idleMoveMaxMs);
        }
        if (pick) {
            for (int attempt = 0; attempt < 8; ++attempt) {
                const float angle = Unit() * 6.2831853f;
                const float radius = tune.idleMin + Unit() * (tune.idleMax - tune.idleMin);
                idleSpot_ = Add(p.leader, {std::sin(angle) * radius, 0.0f, std::cos(angle) * radius});
                if (!NearMate(p, idleSpot_, tune.mateSpace * 1.5f)) break;
            }
        }
        Arrive(p, c, idleSpot_);
        StandingLook(p, c);
    }

    // --- antics ---
    Antic antic_ = Antic::None;
    int anticPhase_ = 0;
    uint32_t anticSince_ = 0, anticPhaseAt_ = 0, nextAnticAt_ = 0, anticTryAt_ = 0, lastVictoryAt_ = 0, lastPrankAt_ = 0;
    uint32_t anticFaceUntil_ = 0, anticLastStomp_ = 0, nextSwingAt_ = 0, anticSpinMs_ = 1700, anticKill_ = 0;
    int anticTaps_ = 0, anticLastTap_ = -1, anticStomps_ = 0, anticStompsWanted_ = 3, anticSwings_ = 0, anticSwingsSkipped_ = 0;
    int anticSwingsWanted_ = 3, anticSpins_ = 0, anticSpinsWanted_ = 1, anticFaceTries_ = 0;
    float anticLeaderYaw_ = 0.0f, anticSpinDir_ = 1.0f, anticSpinYaw_ = 0.0f, anticTurnYaw_ = 0.0f, anticLookYaw_ = 0.0f;
    Vec3 anticLeaderAt_, anticCorpse_, anticFaceTo_;
    bool anticRan_ = false, anticFired_ = false;
    AnticStats anticStats_;
    AnticEvent anticEvents_[6];
    int anticEventCount_ = 0;

    static float WrapPi(float a) {
        while (a > 3.14159265f) a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        return a;
    }
    static float YawTo(const Vec3& from, const Vec3& to) { return std::atan2(to.x - from.x, to.z - from.z); }
    void Skip(AnticSkip s) { ++anticStats_.rejected[static_cast<int>(s)]; }
    void PushAntic(const AnticEvent& e) {
        if (anticEventCount_ < static_cast<int>(sizeof(anticEvents_) / sizeof(anticEvents_[0]))) anticEvents_[anticEventCount_++] = e;
    }
    void AnticStep(int phase, uint32_t now) { anticPhase_ = phase; anticPhaseAt_ = now; }
    uint32_t AnticGap() {
        const int i = tune.antics < 1 ? 0 : (tune.antics > 3 ? 2 : tune.antics - 1);
        return Between(tune.anticGapMinMs[i], tune.anticGapMaxMs[i]);
    }
    static const char* BusyWith(Mode m) {
        switch (m) {
        case Mode::Pickup: return "pickup";
        case Mode::Loot: return "looting";
        case Mode::Heal: return "healing";
        case Mode::Combat: return "fight";
        case Mode::Rush: return "the level goes on";
        case Mode::Follow: return "following";
        default: return "something else to do";
        }
    }
    // Whatever keeps the bot from an antic now; nullptr when all is calm.
    const char* AnticUnrest(const Perception& p) const {
        if (p.busy) return p.busy;
        if (p.grabbed) return "grabbed";
        if (p.leaderGrabbed || p.leaderGrabber) return "leader grabbed";
        if (Hostile(p)) return "enemy about";
        if (lastHostileAt_ && p.now - lastHostileAt_ < tune.anticCalmMs) return "fight too recent";
        if (p.orders.hold) return "hold order";
        if (p.orders.focus && p.now < p.orders.focusUntil) return "focus order";
        if (p.orders.vanguard) return "attack formation";
        if (p.progression) return "the level goes on";
        if (p.chase) return "chase";
        if (p.leaderMoving) return "leader moving";
        if (Flat(p.self, p.leader) > tune.followStart) return "leader far";
        if (p.health01 < tune.healBelow && p.medpacks != 0) return "needs healing";
        return nullptr;
    }
    // Whether this kind may start now; counts why not when asked.
    bool AnticFits(const Perception& p, Antic kind, bool count) {
        auto no = [&](AnticSkip s) { if (count) Skip(s); return false; };
        switch (kind) {
        case Antic::AimSpam: case Antic::Stare: return true;
        case Antic::ShadowBoxing:
            if (p.orders.holdFire) return no(AnticSkip::HoldFire);
            if (!p.selfYawKnown) return no(AnticSkip::NoHeading);
            if (Flat(p.self, p.leader) <= tune.anticSwingClear + 0.3f) return no(AnticSkip::LeaderClose);
            return true;
        case Antic::VictoryStomp:
            if (!p.killId || p.killId == anticKill_ || p.now - p.killAt > tune.anticKillWindowMs || Flat(p.self, p.killPos) > tune.anticKillReach ||
                Flat(p.leader, p.killPos) > tune.followStart + 4.0f)
                return no(AnticSkip::NoKill);
            if (lastVictoryAt_ && p.now - lastVictoryAt_ < tune.anticRareMs) return no(AnticSkip::Cooldown);
            if (p.orders.holdFire) return no(AnticSkip::HoldFire);
            return true;
        case Antic::StasisPrank:
            if (!tune.harmfulAntics) return false;   // off in its profile: not a rejection
            // Squad-wide: one stasis on the leader per anticRareMs, whichever bot it is.
            if ((lastPrankAt_ && p.now - lastPrankAt_ < tune.anticRareMs) || (p.squadPrankAt && p.now - p.squadPrankAt < tune.anticRareMs))
                return no(AnticSkip::Cooldown);
            if (p.orders.holdFire) return no(AnticSkip::HoldFire);
            if (p.stasis01 < tune.stasisMinEnergy) return no(AnticSkip::NoStasis);
            if (p.leaderLine != 1) return no(AnticSkip::NoLine);
            return true;
        default: return false;
        }
    }
    static int AnticWeight(Antic kind) { return kind == Antic::VictoryStomp || kind == Antic::StasisPrank ? 1 : 3; }
    // Due, calm and no mate busy with one: a random kind among those that fit.
    bool MaybeStartAntic(const Perception& p) {
        if (tune.antics <= 0) return false;
        const uint32_t now = p.now;
        if (!nextAnticAt_) { nextAnticAt_ = now + AnticGap(); return false; }
        if (now < nextAnticAt_ || now < anticTryAt_) return false;
        anticTryAt_ = now + tune.anticRetryMs;
        if (following_ || AnticUnrest(p)) { Skip(AnticSkip::NotCalm); return false; }
        for (int m = 0; m < p.mateCount && m < kMaxMates; ++m)
            if (p.mates[m].antic) { Skip(AnticSkip::MateBusy); return false; }
        static constexpr Antic kKinds[] = {Antic::AimSpam, Antic::Stare, Antic::ShadowBoxing, Antic::VictoryStomp, Antic::StasisPrank};
        int weights[5]{}, total = 0;
        uint32_t eligible = 0;
        for (int i = 0; i < 5; ++i) {
            if (!AnticFits(p, kKinds[i], true)) continue;
            weights[i] = AnticWeight(kKinds[i]);
            total += weights[i];
            eligible |= 1u << static_cast<unsigned>(kKinds[i]);
        }
        if (!total) return false;
        int roll = static_cast<int>(Next() % static_cast<uint32_t>(total));
        for (int i = 0; i < 5; ++i) {
            if (roll < weights[i]) { StartAntic(p, kKinds[i], "due", eligible); return true; }
            roll -= weights[i];
        }
        return false;
    }
    void StartAntic(const Perception& p, Antic kind, const char* why, uint32_t eligible) {
        const uint32_t now = p.now ? p.now : 1u;
        antic_ = kind;
        anticSince_ = now;
        AnticStep(0, now);
        anticLeaderAt_ = p.leader;
        anticLeaderYaw_ = p.leaderYaw;
        anticTaps_ = anticStomps_ = anticSwings_ = anticSwingsSkipped_ = anticSpins_ = anticFaceTries_ = 0;
        anticLastTap_ = -1;
        anticFaceUntil_ = anticLastStomp_ = 0;
        anticFired_ = false;
        if (kind == Antic::VictoryStomp) {
            lastVictoryAt_ = now;
            anticKill_ = p.killId;
            anticCorpse_ = p.killPos;
            anticStompsWanted_ = 3 + static_cast<int>(Next() % 2u);
        } else if (kind == Antic::StasisPrank) {
            lastPrankAt_ = now;
        } else if (kind == Antic::ShadowBoxing) {
            anticSpinsWanted_ = Unit() < 0.35f ? 2 : 1;
            anticSpinDir_ = Unit() < 0.5f ? 1.0f : -1.0f;
            anticSpinMs_ = Between(tune.anticSpinMinMs, tune.anticSpinMaxMs);
            anticSwingsWanted_ = static_cast<int>(Between(2, 4));
            anticSpinYaw_ = p.selfYaw;
        }
        ++anticStats_.started[static_cast<int>(kind)];
        AnticEvent e;
        e.antic = kind;
        e.kind = AnticEvent::Start;
        e.why = why;
        e.eligible = eligible;
        PushAntic(e);
    }
    void EndAntic(uint32_t now, bool finished, const char* why) {
        AnticEvent e;
        e.antic = antic_;
        e.kind = finished ? AnticEvent::Finish : AnticEvent::Abort;
        e.why = why;
        e.ms = now - anticSince_;
        e.taps = anticTaps_;
        e.stomps = anticStomps_;
        e.swings = anticSwings_;
        e.swingsSkipped = anticSwingsSkipped_;
        e.spins = anticSpins_;
        e.stasisFired = anticFired_;
        PushAntic(e);
        const int k = static_cast<int>(antic_);
        ++(finished ? anticStats_.finished[k] : anticStats_.aborted[k]);
        anticStats_.taps += static_cast<uint32_t>(anticTaps_);
        anticStats_.stomps += static_cast<uint32_t>(anticStomps_);
        anticStats_.swings += static_cast<uint32_t>(anticSwings_);
        anticStats_.swingsSkipped += static_cast<uint32_t>(anticSwingsSkipped_);
        if (anticFired_) ++anticStats_.stasisShots;
        antic_ = Antic::None;
        nextAnticAt_ = now + AnticGap();
        anticTryAt_ = 0;
    }
    // The hook in Decide: true while an antic owns this frame.
    bool Antics(const Perception& p, Command& c) {
        if (antic_ == Antic::None && !MaybeStartAntic(p)) return false;
        anticRan_ = true;
        if (StepAntic(p, c)) return true;
        c = Command{};   // just over: the usual behaviour takes this frame
        return false;
    }
    bool StepAntic(const Perception& p, Command& c) {
        const uint32_t now = p.now;
        if (now - anticSince_ > tune.anticMaxMs) { EndAntic(now, false, "took too long"); return false; }
        if (p.orders.holdFire && antic_ != Antic::AimSpam && antic_ != Antic::Stare) { EndAntic(now, false, "hold fire order"); return false; }
        c = Command{};
        c.mode = Mode::Idle;
        c.antic = antic_;
        switch (antic_) {
        case Antic::AimSpam: case Antic::Stare: return StepInFront(p, c);
        case Antic::VictoryStomp: return StepVictoryStomp(p, c);
        case Antic::ShadowBoxing: return StepShadowBoxing(p, c);
        case Antic::StasisPrank: return StepStasisPrank(p, c);
        default: EndAntic(now, false, "unknown"); return false;
        }
    }
    // Into place `gap` in front of the leader (as he stood when it began), the
    // last stretch walking at him so it arrives facing him: 1 there, 0 on its way,
    // -1 given up.
    int ComeInFront(const Perception& p, Command& c, float gap, const Vec3& head) {
        const uint32_t now = p.now;
        const Vec3 fwd = FromYaw(anticLeaderYaw_), side = {fwd.z, 0.0f, -fwd.x};
        Vec3 spot = Add(anticLeaderAt_, Scale(fwd, gap));
        spot.y = anticLeaderAt_.y;
        const Vec3 outer = Add(spot, Scale(fwd, tune.anticApproach));
        if (anticPhase_ <= 1 && now - anticPhaseAt_ > tune.anticWalkMaxMs) { EndAntic(now, false, "no way to its place in front of the leader"); return -1; }
        // A goal the runtime found off the mesh stays off limits a while (Tick), and the
        // place is right beside the outer point: either one off the mesh ends it.
        if (anticPhase_ <= 1 && now < blockedUntil_ && (Flat(outer, blockedGoal_) < 1.0f || Flat(spot, blockedGoal_) < 1.0f)) {
            EndAntic(now, false, "its place in front of the leader is off the mesh");
            return -1;
        }
        if (anticPhase_ == 0) {
            const Vec3 rel = {p.self.x - anticLeaderAt_.x, 0.0f, p.self.z - anticLeaderAt_.z};
            if (Flat(p.self, outer) < 0.5f || (Dot(rel, fwd) >= gap + 0.3f && std::fabs(Dot(rel, side)) < 0.6f)) {
                AnticStep(1, now);
            } else {
                c.move = true;
                c.moveTo = outer;
                c.run = Flat(p.self, outer) > 4.0f;
                return 0;
            }
        }
        if (anticPhase_ == 1) {
            if (Flat(p.self, spot) > tune.anticArrive) {
                c.move = true;
                c.moveTo = spot;
                c.look = true;
                c.lookAt = head;
                return 0;
            }
            AnticStep(2, now);
        }
        // There: one short step at him when the body faces elsewhere.
        c.look = true;
        c.lookAt = head;
        if (now < anticFaceUntil_) {
            c.move = true;
            c.direct = true;
            c.moveTo = anticFaceTo_;
            return 0;
        }
        if (p.selfYawKnown && anticFaceTries_ < 1) {
            const Vec3 to = {p.leader.x - p.self.x, 0.0f, p.leader.z - p.self.z};
            const float len = Length(to);
            if (len > 0.5f && std::fabs(WrapPi(std::atan2(to.x, to.z) - p.selfYaw)) > tune.anticFaceDeg * 0.01745329f) {
                ++anticFaceTries_;
                anticFaceTo_ = Add(p.self, Scale(to, 0.25f / len));
                anticFaceUntil_ = now + 300;
                c.move = true;
                c.direct = true;
                c.moveTo = anticFaceTo_;
                return 0;
            }
        }
        return now - anticPhaseAt_ >= tune.anticSettleMs ? 1 : 0;
    }
    // Aim spam: taps the aim at the leader's face. Stare: just looks at it.
    bool StepInFront(const Perception& p, Command& c) {
        const uint32_t now = p.now;
        if (Flat(p.leader, anticLeaderAt_) > tune.anticLeaderMove || std::fabs(WrapPi(p.leaderYaw - anticLeaderYaw_)) > tune.anticLeaderTurnDeg * 0.01745329f) {
            EndAntic(now, false, "leader moved");
            return false;
        }
        const bool spam = antic_ == Antic::AimSpam;
        const Vec3 head = Add(p.leader, {0.0f, 1.6f, 0.0f});
        if (anticPhase_ < 3) {
            const int r = ComeInFront(p, c, spam ? tune.anticAimSpamGap : tune.anticStareGap, head);
            if (r < 0) return false;
            if (r == 0) return true;
            c.move = false;
            AnticStep(3, now);
        }
        const uint32_t t = now - anticPhaseAt_;
        if (t >= (spam ? tune.anticAimSpamMs : tune.anticStareMs)) { EndAntic(now, true, "done"); return false; }
        c.look = true;
        c.lookAt = head;
        if (spam) {
            const int tap = static_cast<int>(t / tune.anticTapMs);
            c.aim = t % tune.anticTapMs < tune.anticTapDownMs;
            if (c.aim && tap != anticLastTap_) { anticLastTap_ = tap; ++anticTaps_; }
        }
        return true;
    }
    // Victory stomp: to its own fresh kill, three or four stomps, then a look at the leader.
    bool StepVictoryStomp(const Perception& p, Command& c) {
        const uint32_t now = p.now;
        if (p.killId == anticKill_) anticCorpse_ = p.killPos;
        if (anticPhase_ == 0) {
            const float d = Flat(p.self, anticCorpse_);
            if (d > tune.stompLootReach) {
                if (now - anticPhaseAt_ > tune.anticWalkMaxMs) { EndAntic(now, false, "the corpse is out of reach"); return false; }
                c.move = true;
                c.moveTo = anticCorpse_;
                c.run = d > 4.0f;
                if (d < 2.5f) { c.look = true; c.lookAt = anticCorpse_; }
                return true;
            }
            AnticStep(1, now);
        }
        if (anticPhase_ == 1) {
            c.look = true;
            c.lookAt = anticCorpse_;
            if (anticStomps_ < anticStompsWanted_) {
                if (!anticLastStomp_ || now - anticLastStomp_ >= tune.stompIntervalMs) { anticLastStomp_ = now; c.stomp = true; ++anticStomps_; }
                return true;
            }
            if (now - anticLastStomp_ < 700) return true;
            AnticStep(2, now);
        }
        c.look = true;
        c.lookAt = Add(p.leader, {0.0f, 1.6f, 0.0f});
        if (now - anticPhaseAt_ >= tune.anticLookBackMs) { EndAntic(now, true, "done"); return false; }
        return true;
    }
    // Within anticSwingFrontDeg of where the body faces.
    bool Before(const Perception& p, const Vec3& who) const {
        const Vec3 to = {who.x - p.self.x, 0.0f, who.z - p.self.z};
        if (Length(to) < 0.05f) return true;
        return std::fabs(WrapPi(std::atan2(to.x, to.z) - p.selfYaw)) < tune.anticSwingFrontDeg * 0.01745329f;
    }
    // A swing at the air only with the leader (and any other player but a mate,
    // whom it cannot hurt) farther than anticSwingClear and not before it.
    bool SwingSafe(const Perception& p) const {
        if (!p.selfYawKnown) return false;
        auto clear = [&](const Vec3& who) { return Flat(p.self, who) > tune.anticSwingClear && !Before(p, who); };
        if (!clear(p.leader)) return false;
        for (int i = 0; i < p.playerCount && i < kMaxPlayers; ++i) {
            bool mate = false;
            for (int m = 0; m < p.mateCount && m < kMaxMates; ++m) mate = mate || Flat(p.mates[m].pos, p.players[i]) < 0.3f;
            if (!mate && !clear(p.players[i])) return false;
        }
        return true;
    }
    // A way to face that is well away from the leader.
    float AwayYaw(const Perception& p) {
        const float leaderDir = YawTo(p.self, p.leader);
        const float yaw = Unit() * 6.2831853f;
        if (std::fabs(WrapPi(yaw - leaderDir)) >= 100.0f * 0.01745329f) return yaw;
        return leaderDir + 3.14159265f + (Unit() - 0.5f) * 1.4f;
    }
    // Shadow boxing: spins where it stands (a small circle, the body only turns
    // while it walks), then a few swings at the air with pauses, sometimes
    // turned some other way first.
    bool StepShadowBoxing(const Perception& p, Command& c) {
        const uint32_t now = p.now;
        switch (anticPhase_) {
        case 0: {
            const uint32_t t = now - anticPhaseAt_;
            if (t >= anticSpinMs_) {
                ++anticSpins_;
                if (anticSpins_ < anticSpinsWanted_) {
                    anticSpinYaw_ = p.selfYawKnown ? p.selfYaw : anticSpinYaw_;
                    AnticStep(0, now);
                } else {
                    AnticStep(1, now);
                    nextSwingAt_ = now + Between(250, 600);
                }
                return true;
            }
            const float yaw = anticSpinYaw_ + anticSpinDir_ * (6.2831853f * static_cast<float>(t) / static_cast<float>(anticSpinMs_) + tune.anticSpinLead);
            c.move = true;
            c.direct = true;
            c.moveTo = Add(p.self, Scale(FromYaw(yaw), tune.anticSpinReach));
            return true;
        }
        case 1:
            if (now < nextSwingAt_) return true;
            // Always turned away first when the leader stands before it; else now and then.
            if (Before(p, p.leader) || Unit() < 0.5f) { anticTurnYaw_ = AwayYaw(p); AnticStep(2, now); }
            else AnticStep(3, now);
            return true;
        case 2: {
            const bool there = p.selfYawKnown && std::fabs(WrapPi(p.selfYaw - anticTurnYaw_)) < tune.anticTurnDoneDeg * 0.01745329f;
            if (!there && now - anticPhaseAt_ < tune.anticTurnMs) {
                c.move = true;
                c.direct = true;
                c.moveTo = Add(p.self, Scale(FromYaw(anticTurnYaw_), 0.5f));
                return true;
            }
            AnticStep(3, now);
            return true;
        }
        case 3:
            if (SwingSafe(p)) { c.melee = true; ++anticSwings_; }
            else { ++anticSwingsSkipped_; Skip(AnticSkip::LeaderClose); }
            AnticStep(4, now);
            return true;
        default:
            if (now - anticPhaseAt_ < tune.meleeHoldMs) return true;   // through the swing
            if (anticSwings_ + anticSwingsSkipped_ >= anticSwingsWanted_) { EndAntic(now, true, "done"); return false; }
            nextSwingAt_ = now + Between(tune.anticSwingGapMinMs, tune.anticSwingGapMaxMs);
            AnticStep(1, now);
            return true;
        }
    }
    // Stasis prank: a few metres off, one stasis shot at the leader's chest, then
    // a look elsewhere as if nothing happened.
    bool StepStasisPrank(const Perception& p, Command& c) {
        const uint32_t now = p.now;
        const Vec3 chest = Add(p.leader, {0.0f, tune.anticChest, 0.0f});
        const float d = Flat(p.self, p.leader);
        switch (anticPhase_) {
        case 0: {
            if (d >= tune.anticPrankMin && d <= tune.anticPrankMax) { AnticStep(1, now); return true; }
            if (now - anticPhaseAt_ > tune.anticWalkMaxMs) { EndAntic(now, false, "no place a few metres off the leader"); return false; }
            Vec3 away = {p.self.x - p.leader.x, 0.0f, p.self.z - p.leader.z};
            const float len = Length(away);
            away = len > 0.3f ? Scale(away, 1.0f / len) : FromYaw(p.leaderYaw + 3.14159265f);
            c.move = true;
            c.moveTo = Add(p.leader, Scale(away, tune.anticPrankStand));
            return true;
        }
        case 1:
            c.aim = true;
            c.look = true;
            c.lookAt = chest;
            if (d < tune.anticPrankMin - 0.5f) { EndAntic(now, false, "leader came too close"); return false; }
            if (now - anticPhaseAt_ < tune.anticPrankAimMs || p.aimErrorDeg > tune.anticPrankAimDeg) {
                if (now - anticPhaseAt_ > tune.anticPrankAimMaxMs) { EndAntic(now, false, "its aim did not settle"); return false; }
                return true;
            }
            if (p.leaderLine != 1) { Skip(AnticSkip::NoLine); EndAntic(now, false, "no clear line"); return false; }
            if (p.stasis01 < tune.stasisMinEnergy) { Skip(AnticSkip::NoStasis); EndAntic(now, false, "no stasis energy"); return false; }
            c.stasis = true;
            anticFired_ = true;
            AnticStep(2, now);
            return true;
        case 2:
            c.aim = true;
            c.look = true;
            c.lookAt = chest;
            if (now - anticPhaseAt_ >= 350) {
                anticLookYaw_ = YawTo(p.self, p.leader) + (Unit() < 0.5f ? 1.0f : -1.0f) * (1.9f + Unit() * 0.9f);
                AnticStep(3, now);
            }
            return true;
        default:
            c.look = true;
            c.lookAt = Add(p.self, Add(Scale(FromYaw(anticLookYaw_), 6.0f), {0.0f, 2.4f, 0.0f}));
            if (now - anticPhaseAt_ >= tune.anticInnocentMs) { EndAntic(now, true, "done"); return false; }
            return true;
        }
    }
};

// Power node doors and stasis stations (runtime_coop_bot_usables.inc): whether a
// bot may go and use one, from what the runtime read off the game. The first
// reason that holds wins, so the log names exactly one.
struct UsableTune {
    float doorLeaderMeters = 6.0f;   // the leader this close to the door panel
    float doorReachMeters = 20.0f;   // the bot this close to it
    uint32_t doorWaitMs = 1500;      // the leader near it this long without using it
    float stationBelow = 0.5f;       // stasis energy below this share of its max
    float stationMeters = 12.0f;     // the station this close to the bot and to the leader
    float floorMeters = 2.5f;        // farther apart in height is another floor
    int maxFailures = 2;             // failed tries before a panel or station is left alone
    uint32_t retryMs = 60000;        // the wait after a failed try
};

enum class DoorVerdict : uint8_t {
    Go, Busy, Disabled, AlreadyOn, InUse, LeaderAtIt, WaitingOut, GaveUp, Cooldown, NoNodes, Enemies, OtherBot, TooFar, LeaderFar, LeaderNotSettled, Count
};
inline const char* DoorVerdictName(DoorVerdict v) {
    static const char* const kNames[] = {"go", "bot busy", "panel disabled", "already open", "in use", "leader at it", "denial time runs", "gave up",
                                         "cooldown", "not enough nodes", "enemies near", "another bot on a panel", "too far", "leader not near",
                                         "leader not settled"};
    const unsigned i = static_cast<unsigned>(v);
    return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}
struct NodeDoorFacts {
    bool busy = false;         // the bot is at a kiosk, holding a spot or flying
    bool enabled = true;       // Switch::m_IsEnabled
    bool on = false;           // switched on already: the room is open
    bool inUse = false;        // the panel has a user (m_pUser)
    bool leaderAtIt = false;   // the game offers the panel to the leader
    bool waitingOut = false;   // its denial time (m_fAllowUsageTime) has not run out
    bool calm = true;          // no enemy seen, not in a fight
    bool otherBot = false;     // another bot is on a panel
    int nodes = 0, required = 1;
    float selfDist = 0.0f, selfDy = 0.0f, leaderDist = 0.0f, leaderDy = 0.0f;
    uint32_t leaderNearMs = 0;
    int failures = 0;
    uint32_t sinceFailureMs = 0xFFFFFFFFu;
};
inline DoorVerdict JudgeNodeDoor(const NodeDoorFacts& f, const UsableTune& t) {
    if (f.busy) return DoorVerdict::Busy;
    if (!f.enabled) return DoorVerdict::Disabled;
    if (f.on) return DoorVerdict::AlreadyOn;
    if (f.inUse) return DoorVerdict::InUse;
    if (f.leaderAtIt) return DoorVerdict::LeaderAtIt;
    if (f.waitingOut) return DoorVerdict::WaitingOut;
    if (f.failures >= t.maxFailures) return DoorVerdict::GaveUp;
    if (f.failures > 0 && f.sinceFailureMs < t.retryMs) return DoorVerdict::Cooldown;
    if (f.required < 1 || f.nodes < f.required) return DoorVerdict::NoNodes;
    if (!f.calm) return DoorVerdict::Enemies;
    if (f.otherBot) return DoorVerdict::OtherBot;
    if (f.selfDist > t.doorReachMeters || std::fabs(f.selfDy) > t.floorMeters) return DoorVerdict::TooFar;
    if (f.leaderDist > t.doorLeaderMeters || std::fabs(f.leaderDy) > t.floorMeters) return DoorVerdict::LeaderFar;
    if (f.leaderNearMs < t.doorWaitMs) return DoorVerdict::LeaderNotSettled;
    return DoorVerdict::Go;
}

enum class StationVerdict : uint8_t {
    Go, Busy, NoModule, EnergyOk, AirStation, Locked, LeaderUsing, Charging, Downtime, Empty, GaveUp, Cooldown, Enemies, OtherBot, TooFar, LeaderFar, Count
};
inline const char* StationVerdictName(StationVerdict v) {
    static const char* const kNames[] = {"go", "bot busy", "no stasis module", "energy high enough", "air station", "locked", "leader using it",
                                         "charging someone", "downtime", "empty", "gave up", "cooldown", "enemies near", "another bot on a station",
                                         "too far", "leader not near"};
    const unsigned i = static_cast<unsigned>(v);
    return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}
struct StationFacts {
    bool busy = false;
    bool stasisModule = true;    // ability bit 0x08 of the bot
    bool energyKnown = true;
    float energy01 = 1.0f;       // the bot's stasis energy over its max
    bool airStation = false;     // attrs CMD_SetIsAirStation: refills air, not stasis
    // 0 locked, 1 ready, 2 activating, 3 charging, 4 deactivating, 5 downtime, 6 back to ready
    int state = 1;
    bool targetSet = false;      // m_pChargeTarget is someone
    bool leaderTarget = false;   // ... and that someone is the leader
    bool leaderAtIt = false;     // the game offers the station to the leader and his stasis is not full
    float charge = 1.0f, downtime = 0.0f;
    bool calm = true;
    bool otherBot = false;
    float selfDist = 0.0f, selfDy = 0.0f, leaderDist = 0.0f, leaderDy = 0.0f;
    int failures = 0;
    uint32_t sinceFailureMs = 0xFFFFFFFFu;
};
inline StationVerdict JudgeStation(const StationFacts& f, const UsableTune& t) {
    if (f.busy) return StationVerdict::Busy;
    if (!f.stasisModule) return StationVerdict::NoModule;
    if (!f.energyKnown || f.energy01 >= t.stationBelow) return StationVerdict::EnergyOk;
    if (f.airStation) return StationVerdict::AirStation;
    if (f.state == 0) return StationVerdict::Locked;
    if (f.leaderTarget || f.leaderAtIt) return StationVerdict::LeaderUsing;
    if (f.targetSet || f.state == 2 || f.state == 3 || f.state == 4) return StationVerdict::Charging;
    if (f.state != 1 || f.downtime > 0.0f) return StationVerdict::Downtime;
    if (!(f.charge > 0.0f)) return StationVerdict::Empty;
    if (f.failures >= t.maxFailures) return StationVerdict::GaveUp;
    if (f.failures > 0 && f.sinceFailureMs < t.retryMs) return StationVerdict::Cooldown;
    if (!f.calm) return StationVerdict::Enemies;
    if (f.otherBot) return StationVerdict::OtherBot;
    if (f.selfDist > t.stationMeters || std::fabs(f.selfDy) > t.floorMeters) return StationVerdict::TooFar;
    if (f.leaderDist > t.stationMeters || std::fabs(f.leaderDy) > t.floorMeters) return StationVerdict::LeaderFar;
    return StationVerdict::Go;
}

}  // namespace coop_bot
