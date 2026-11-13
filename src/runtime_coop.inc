// Dead Space 2 United by Richard Hafer.
// Experimental campaign co-op through the retail TNL session.
// Uniform loading, player replication and whole-world replication are separate
// options. MPUniformStreams has a successful local body test; network behavior
// still requires a two-peer test. All experimental options default to off.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <intrin.h>
#include <cmath>
#include "coop_peer_readiness.h"
#include "coop_local_lifecycle.h"
#include "coop_save_policy.h"
#include "coop_weapon_lookup.h"
#include "coop_template_protocol.h"
#include "coop_death_protocol.h"
#include "coop_ping_protocol.h"
#include "coop_hit_protocol.h"
#include "coop_peers.h"
#include "coop_lobby.h"
#include "coop_progress.h"
#include "coop_survmusic.h"
#include "coop_respawn_rules.h"
#include "generated_mp_spawn_windows.h"
#include "coop_suit_protocol.h"
#include "coop_loadout_sync.h"
#include "coop_bot_brain.h"
#include "coop_bot_steer.h"
#include "coop_bot_bench.h"
#include "coop_bot_save.h"
#include "coop_bot_progress.h"
#include "coop_turret.h"
#include "coop_gadgets.h"
#include "coop_zone.h"
#include "coop_configcheck.h"
#include "coop_marks.h"
#include "coop_mpnative.h"
#include "coop_versus.h"
#include "coop_shop.h"
#include "generated_mpmap_block.h"
#include "generated_versus_templates.h"
#include "generated_helmet_guids.h"
#include "coop_strips.h"
#include "coop_arena.h"
#include "coop_camtools.h"
#include "coop_debug.h"
#include "coop_voices.h"
#include "coop_dementia.h"
#include "generated_dementia_records.h"
#include "coop_client_progress.h"
#include "coop_difficulty.h"
#include <algorithm>
#include <memory>
#include <xaudio2.h>
#include <x3daudio.h>
#undef small   // rpcndr.h, pulled in by xaudio2.h, makes it "char"
#pragma comment(lib, "ws2_32.lib")

namespace coop_uniform { bool Arm(); bool BotBody(uint32_t player, uint32_t* out); bool BotSuit(uint32_t* out); bool BotDress(uint32_t player, const uint32_t* guid); int BotOtherSuit(unsigned n, uint32_t* out); int SuitIndex(const uint32_t* guid);
                         bool ArmBirth(const uint32_t* guid, const char* why); void DisarmBirth(bool countMiss); int Preload(const uint32_t* guid); }   // runtime_coop_uniform.inc
namespace coop {
// The companion bots' Isaacs (runtime_coop_bot.inc), known to every hook that has
// to tell them from the player: saving, damage, death, GUID lookups, lifts. At
// most three: the CameraManager has four viewports and Isaac holds the first.
// botInit is set while the engine initializes one (Player::Init in Finalize).
constexpr unsigned kMaxBots = 3;
uint32_t botEntities[kMaxBots]{};
bool botHolding[kMaxBots]{};   // told to hold a spot: lifts do not wait for it
bool botInit = false, botWanted = false;
DWORD botDiedAt = 0;   // when a bot's death state last started (runtime_coop_bot.inc OnDeathSm)
bool IsBotEntity(uint32_t entity) {
    if (!entity) return false;
    for (uint32_t b : botEntities) if (b == entity) return true;
    return false;
}
bool AnyBot() {
    for (uint32_t b : botEntities) if (b) return true;
    return false;
}
#include "runtime_coop_vlog.inc"
#include "runtime_coop_mpquiet.inc"
#include "runtime_coop_state.inc"
#include "runtime_coop_ownership.inc"
#include "runtime_coop_npc_policy.inc"
#include "runtime_coop_ghost_probe.inc"
#include "runtime_coop_save.inc"
#include "runtime_coop_weapon_probe.inc"
// What kind of session the level is (defined after SurvivalSession). Level code
// asks these; gamemode::mode is only the map picker's choice and stays at
// Survival through a whole campaign (12.10.: "gamemode mode=survival" in story).
namespace play { bool StoryCoop(); bool SurvivalMode(); bool VersusMode(); }
namespace voices { void OnHit(const void* damage, uint32_t component, float before, float after); }   // runtime_coop_voices.inc: multi_kill
namespace progress {   // runtime_coop_progress.inc
float HitFactor(const void* damage, uint32_t component);
void OnHit(const void* damage, uint32_t component, float before, float after);
void FromHost(uint32_t run, uint32_t wave, uint32_t xp, uint32_t flags);
void Taint(const char* why);
void StartBonus(uint32_t player, const char* where);
extern bool enabled;
extern coop_progress::Rules rules;
extern volatile LONG kills[coop_progress::kGunCount];
unsigned WeaponLevelOf(int gun);
bool TestAll();
unsigned ProfileLevel();
unsigned UnlockLevel(const char* key, int fallback = -1);
bool Unlocked(const char* key, int fallback = -1);
unsigned SuitUnlockLevel(const uint32_t* uniform);
bool GadgetAllowed(const char* key, const char* label);
}
namespace mpsuit {   // runtime_coop_mpsuit.inc
bool Wanted();
bool KeepRecord(const uint32_t* guid);
bool IsMpSuit(const uint32_t* guid);
bool AskDress(uint32_t player);
bool PartnersReady();
extern volatile LONG ghostRefusals;
}
bool enabled = false, installed = false, autoStart = true, network = false;
int startKey = VK_F9;
unsigned localPort = 27015;
std::string peerText;
unsigned peerIp = 0, peerPort = 0;
bool keyWasDown = false;

// engine anchors (RVAs)
constexpr uint32_t kServices = 0x1C213DC, kSessionSingleton = 0x1C216DC, kClassTableReady = 0x1D38AA4;
constexpr uint32_t kSessionCtor = 0x42A520, kSessionSize = 0x90, kSessionInterface = 0x1C;
constexpr uint32_t kCreateConnection = 0x4578C0, kGameNew = 0x3CB0;
constexpr uint32_t kSocketCtor = 0x1017EA0, kSocketDtor = 0x1017EE0, kSocketSendto = 0x1017F00, kSocketRecvfrom = 0x1017F50;
constexpr uint32_t kInterfaceConnectionCount = 0xC, kInterfaceConnectionArray = 0x14, kConnectionState = 0x1B0;
constexpr uint32_t kInterfacePendingCount = 0x24, kInterfacePendingArray = 0x2C;
// TNL::NetInterface::checkIncomingPackets and processConnections, the two
// calls NetSession::Tick (0x0082B300) makes on its interface. The engine's own
// tick did not run in the first test (one connect packet, no retries, no
// receive), so the plugin drives them itself once per frame; TickMode=2 calls
// the whole NetSession::Tick instead.
constexpr uint32_t kCheckIncomingPackets = 0x101A340, kProcessConnections = 0x101B400, kSessionTick = 0x42B300;
int tickMode = 2;
volatile LONG receiveCalls = 0, receiveSkipped = 0, ticks = 0, idleProcessSkips = 0;
// Ghosting fields of a TNL::GhostConnection, read for the log only: the ghost
// array pointer (+0x1F8, set once the connection ghosts to the peer) and the
// ghosting sequence (+0x208, bumped by activateGhosting).
constexpr uint32_t kConnectionGhostArray = 0x1F8, kConnectionGhostingSequence = 0x208, kConnectionLocalGhosts = 0x20C;
constexpr uint32_t kStreamManager = 0x1C1C098;  // level detection: a level registers more than 8 TOCs
bool inLevel = false;
// True once the level has settled (player stable for a few seconds). The gate
// holds nothing before that: the stream set that brings the starting area in
// fires during the load, and holding it is a loading screen that never ends
// (host, 2026-09-17 14:23).
bool levelSettled = false;
DWORD levelSettledSince = 0;   // when the local level last settled, 0 outside one
// The session must not sync objects while the level is still being built
// (client crash 41460 of 21:50: the first sync ran during the loading screen
// and an entity's animation data was not there yet), and the client must not
// sync before it is connected: with machine index 1 its level objects become
// ghosts of the host's, so their data has to arrive from the host first. So:
// start only once the loading screen has been gone for a few seconds and the
// player exists; tick TNL only (mode 1) until a connection is established,
// then the full session tick (mode 2).
coop_local::SettleState localSettle;
bool connected = false, replicationReady = false;
int requestedTickMode = 2;
// Player IDs belong to PlayerManager slots, not network machine IDs.
constexpr uint32_t kPlayerId = 0x420;
DWORD connectStarted = 0;
volatile LONG connectAttempts = 0, partnerRedials = 0, frontendTickSkips = 0;

// TNL::Address as this plugin encodes it
struct Address { uint32_t ip; uint32_t port; };

// The OnlinePlayer fake: the engine dereferences the 4-byte handle to the
// impl and calls slot 3 of the object embedded at impl+0x50.
struct AddressObject { void** vtable; Address address; };
struct FakeImpl { unsigned char pad[0x50]; AddressObject address; };
struct FakePlayer { FakeImpl* impl; };
unsigned long long __fastcall GetAddress64(AddressObject* self, void*) {
    return (static_cast<unsigned long long>(self->address.port) << 32) | self->address.ip;
}
unsigned long long __fastcall Slot0(void*, void*) { return 0; }
// Slot 1 of the same object is the machine index NetObjSync folds into the
// sub key of every dynamic object it registers (top byte). Distinct per
// machine, so the two local players never share a NetID.
unsigned machineIndex = 0;
// -1 means "decide from the role", which is the old two-player rule. Anything
// else is an explicit number from the launcher or the INI, and is what lets
// three or more instances mint sub-keys that cannot collide.
int machineOverride = -1;

// Who owns the enemies, and who relays: machine 0 is the host.
//
// This used to be "is a peer configured?", which only held while the players
// were a star. Once every instance dials every other one, every instance has a
// peer, so the host took itself for a client and suppressed its own NPC spawns
// (2026-09-17: role=client machine=0, npc_suppressed=4 and npc_registered=0 on
// all four, no enemies in the level). The machine number is the launcher's
// convention, explicit in DS2COOP, and does not change with the topology.
bool IsHost() { return machineIndex == 0; }
// A tick that is never 0 (0 means "not set"). Not `now | 1`: that makes an even tick now+1, and
// `now - stamp` in the same tick is then 0xFFFFFFFF, every window long past (17.10.: the survival
// director's frozen watchdog killed moving slashers "frozen for 4294967 s").
inline DWORD Stamp(DWORD now) { return now ? now : 1; }
// now - stamp for a stamp that may come from a fresher GetTickCount() than the caller's now: 0, not 49 days.
inline DWORD TicksSince(DWORD now, DWORD stamp) { return static_cast<LONG>(now - stamp) < 0 ? 0 : now - stamp; }
unsigned __fastcall GetMachineIndex(void*, void*) { return machineIndex; }
void* fakeVtable[16]{};
FakeImpl localImpl{}, peerImpl{};
FakePlayer localPlayer{&localImpl}, peerPlayer{&peerImpl};
// One of these per dialled partner.
//
// TNL does not forward ghosts. Measured 2026-09-17: with three clients on one
// host the host had connections=3 and three distinct ghost bodies, one per
// machine, while every client had connections=1 and saw only the host. The side
// channel could be relayed (the clients knew about each other, peers=3), but a
// body is not a datagram: a ghost the host holds belongs to somebody else, and
// TNL will not hand it on to a third connection.
//
// So the players have to be a mesh even though the side channel is a star. Each
// instance dials every partner it was given; the first to speak wins and the
// other side accepts, exactly as it already does for the host.
constexpr unsigned kMaxDialled = coop_peers::kMaxPeers;
FakeImpl dialledImpl[kMaxDialled]{};
FakePlayer dialledPlayer[kMaxDialled]{};
unsigned dialledIps[kMaxDialled]{}, dialledPorts[kMaxDialled]{};
unsigned dialledCount = 0;

// NetObjSync (0x00829940) and the socket path ask the online manager
// (services+0x24, vtable slot 26 = 0x013778E0) for the local game player: an
// object whose +0x210 holds the local player's impl (NetObjSync reads
// [+0x210]+0x50 straight away, one dereference fewer than CreateConnection,
// which takes the 4-byte handle; dump 38660 of 21:44 was that mix-up). The
// real object comes from Blaze's game at services+0x94, null in the campaign,
// so the getter returns null and the sync registers nothing. While the
// plugin's session exists the getter is answered with a fake instead.
constexpr uint32_t kLocalGamePlayerGetter = 0xF778E0;
struct FakeGamePlayer { unsigned char pad[0x210]; FakeImpl* player; unsigned char tail[0x100]; };
FakeGamePlayer fakeGamePlayer{};
safetyhook::InlineHook gamePlayerHook;
volatile LONG gamePlayerAnswers = 0, gamePlayerPassed = 0;
uint32_t session = 0;
// Only NetObjSync gets the fake. The getter is also asked thirty times a
// second by ordinary game code (600 answers in 20 s on 15.09. 22:10 with no
// session tick at all), and a non-null answer there sent the animation
// transform (0x0106B1D0) through a path meant for a Blaze player: both
// instances died the same way before any connection existed. The call site
// inside NetObjSync is 0x00829982, so its return address is 0x00829984.
constexpr uint32_t kNetObjSyncGetterReturn = 0x429984;
// 22:12: with the fake, NetObjSync registered every level object (subkey,
// netInit at reflectable slot 12, proxy) and both instances died in the
// animation transform without any connection. The whole-level sync is off
// (SyncAllObjects=0); the plugin registers the local player by hand instead.
bool syncAllObjects = false;
void Poll();  // defined below

// Fallback driver. The preferred periodic call is the plugin's frame tick, but
// that site can already be taken by another plugin (the randomizer's frame cap
// hooks the same 0x3701C0). Rather than fight over it, the co-op plugin then
// drives itself from a hook it owns: this getter runs on the game thread often
// enough, and the work is throttled to roughly one frame.
// Self-drive runs Poll from hooks that can fire inside a message dispatch
// (gate::OnDispatch, a listener asking for the local game player). Poll
// replays held messages itself, so it never runs nested in a dispatch or in itself.
thread_local int gateDispatchDepth = 0;
// the getter's caller while it drives Poll: the match end must not delete the session from inside network code
thread_local uint32_t driveCaller = 0;
volatile LONG driveSelfNested = 0;
void DriveSelf() {
    static bool driving = false;
    if (driving || gateDispatchDepth > 0) { InterlockedIncrement(&driveSelfNested); return; }
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 16) return;
    last = now;
    driving = true;
    Poll();
    if (g_uniformPoll) g_uniformPoll();
    driving = false;
}

void** __fastcall OnLocalGamePlayer(void* self, void*, void** out) {
    const uint32_t caller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    if (g_selfDrive) {
        driveCaller = caller;
        DriveSelf();
        driveCaller = 0;
    }
    if (syncAllObjects && replicationReady && session && out && caller == base + kNetObjSyncGetterReturn) {
        *out = &fakeGamePlayer;
        InterlockedIncrement(&gamePlayerAnswers);
        return out;
    }
    InterlockedIncrement(&gamePlayerPassed);
    return gamePlayerHook.thiscall<void**>(self, out);
}

// The partner Player arrives through Player::NetCreate as a bare Player: an
// animation/pose component (vtable 0x01B78668) whose owner has no skeleton
// yet. Two of its virtuals drive the per-frame pose build: slot 15
// (0x01091790) is the update that ran on both instances (dump 21080 of 22:30,
// reached via 0x01091740 -> slot 15 -> 0x01092150), slot 14 (0x01091A00) is a
// second variant. Both funnel into 0x01092150 (fills [animState+0x120] from
// the skeleton, faults at 0x01092252 reading the null bone table) and
// 0x0106B1D0 (multiplies the unset matrices). Guarding only a sub-step left
// the next with unset matrices (the 22:00-22:31 loop), and guarding only slot
// 14 missed the live path (slot 15). So both whole updates are skipped for a
// ghost Player whose AnimView (+0x194) has no bone table (+0xB8); once the
// model attaches, the table fills and they run normally.
constexpr uint32_t kGhostUpdateSlot15 = 0xC91790, kGhostUpdateSlot14 = 0xC91A00;
constexpr uint32_t kComponentOwner = 0x2C, kAnimViewOffset = 0x194, kBoneTableOffset = 0xB8;
constexpr uint32_t kPlayerReflectableOffset = 0x5C;
safetyhook::InlineHook ghostAnimHook, ghostAnimHook2;
volatile LONG ghostAnimSkips = 0;

// When on, a detected bare ghost is given the local Isaac's resident model so
// the skeleton fills and the animation runs (see AttachGhostSkeleton below).
bool ghostSkeleton = false;
volatile LONG ghostAttachEntity = 0;   // set here, consumed on the main thread in Poll

// Partner transform from the data channel (defined fully in the data-channel
// section below; declared here so the ghost/stand-in code above can use it).
float partnerPos[3]{}, partnerYaw = 0.0f;
bool partnerAddrKnown = false;

// True when this animation component belongs to a ghost Player without a
// skeleton yet. Every read is a checked ReadProcessMemory, so a bad pointer
// makes it return false (run the original) rather than fault here.
bool BareGhostComponent(uint32_t component) {
    uint32_t owner = 0, entityVtable = 0, proxy = 0, flags = 0, view = 0, bones = 0;
    if (!Read(component + kComponentOwner, &owner, 4) || !owner) return false;
    const uint32_t entity = owner - 0x10;
    // A partner's NecroPlayer ghost (runtime_coop_necro.inc) shares the Animated
    // layout (+0x194 view, +0xB8 bone table); without a bone table it must be
    // skipped exactly like a bare Player ghost, or slot 14/15 walk a null table.
    const bool bare = Read(entity, &entityVtable, 4) &&
           (entityVtable == base + placement::kPlayerPrimaryVtableRva || entityVtable == base + kNecroPrimaryVtableRva) &&
           Read(entity + kPlayerReflectableOffset + 0xC, &proxy, 4) && proxy &&
           Read(proxy + 0x30, &flags, 4) && (flags & 2) &&
           Read(entity + kAnimViewOffset, &view, 4) && view &&
           Read(view + kBoneTableOffset, &bones, 4) && !bones;
    // Remember the ghost so the frame loop can attach the model off the
    // animation tick (attaching from inside the tick would re-enter the engine
    // mid-update). The newest one wins; Poll retries until the bones appear.
    if (bare && ghostSkeleton) InterlockedExchange(&ghostAttachEntity, static_cast<LONG>(entity));
    return bare;
}
void __fastcall OnGhostUpdate15(void* self, void*) {
    if (g_uniformGhostDetached && g_uniformGhostDetached(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)))) {
        InterlockedIncrement(&ghostAnimSkips);
        return;
    }
    if (BareGhostComponent(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)))) { InterlockedIncrement(&ghostAnimSkips); return; }
    ghostAnimHook.thiscall<void>(self);
}
void __fastcall OnGhostUpdate14(void* self, void*) {
    if (g_uniformPreparePose && !g_uniformPreparePose(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)))) {
        InterlockedIncrement(&ghostAnimSkips);
        return;
    }
    if (BareGhostComponent(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)))) { InterlockedIncrement(&ghostAnimSkips); return; }
    ghostAnimHook2.thiscall<void>(self);
}

// Real second Isaac: give the bare ghost the local player's resident model.
// The engine creates the second Player ghost, but the campaign path attaches no
// uniform, so its AnimView (+0x194) has no bone table (+0xB8) and the animation
// tick faults (0x1092252 / 0x106B1D0 family). The local Isaac's model IS
// resident. Attaching it with the proven Entity::AttachModel (0x7D2230, ecx =
// entity, [esp+4] = resource entry) fills the skeleton, after which the guards
// above step aside on their own (they only skip while +0xB8 is null) and the
// engine's ghost prediction animates the body. The entry is what the resolver
// 0x790650(guid*) returns for the model GUID, which sits at MetaModel+0x10
// (entity+0x140 -> +0x10 -> +0x10; verified live 15.09.: that GUID is the one a
// resource entry carries at +0x1C). Cross-engine confirmation of the +0xB8
// crash and the resident-asset requirement: DS3's Carver suit, fault 0x62C74F.
constexpr uint32_t kMetaModelInstance = 0x140, kMetaModelPtr = 0x10, kModelGuidInMeta = 0x10;
constexpr uint32_t kResolveEntryRva = 0x390650, kAttachModelRva = 0x3D2230;
Guid localModelGuid{};
bool localModelKnown = false;
uint32_t attachedGhosts[8]{};
unsigned attachedGhostCount = 0;
volatile LONG ghostAttachTries = 0, ghostAttachDone = 0, ghostAttachMissing = 0, ghostAttachFaults = 0, ghostBonesFilled = 0;

// Read the model GUID from a player that actually has a skeleton: that is the
// local Isaac, never the bare ghost (whose bone table is null). playerObjects
// holds every constructed Player, so the ghost never shadows the real one here.
void CaptureLocalModel() {
    if (localModelKnown) return;
    for (unsigned i = 0; i < std::size(placement::playerObjects); ++i) {
        const uint32_t player = static_cast<uint32_t>(InterlockedCompareExchange(&placement::playerObjects[i], 0, 0));
        if (!player) continue;
        uint32_t vtable = 0, view = 0, bones = 0, mmi = 0, meta = 0;
        if (!Read(player, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva) continue;
        if (!Read(player + kAnimViewOffset, &view, 4) || !view) continue;
        if (!Read(view + kBoneTableOffset, &bones, 4) || !bones) continue;
        if (!Read(player + kMetaModelInstance, &mmi, 4) || !mmi) continue;
        if (!Read(mmi + kMetaModelPtr, &meta, 4) || !meta) continue;
        Guid g{};
        if (Read(meta + kModelGuidInMeta, g.data(), 16) && (g[0] || g[1] || g[2] || g[3])) {
            localModelGuid = g;
            localModelKnown = true;
            return;
        }
    }
}

bool AlreadyAttached(uint32_t entity) {
    for (unsigned i = 0; i < attachedGhostCount; ++i)
        if (attachedGhosts[i] == entity) return true;
    return false;
}

bool IsGhostPlayer(uint32_t entity);   // defined below

// A bare ghost is a Player-vtable entity flagged as a ghost (proxy+0x30 & 2)
// whose AnimView (+0x194) has no bone table (+0xB8). The local Isaac is never
// this: it is authoritative (ghost bit clear) and carries a skeleton. Checked
// reads, so a stale pointer aborts to false. Out-params give the caller the
// AnimView to re-check after the attach.
bool IsBareGhostEntity(uint32_t entity, uint32_t* outView) {
    uint32_t vtable = 0, proxy = 0, flags = 0, view = 0, bones = 0;
    if (!Read(entity, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva) return false;
    if (!Read(entity + kPlayerReflectableOffset + 0xC, &proxy, 4) || !proxy) return false;
    if (!Read(proxy + 0x30, &flags, 4) || !(flags & 2)) return false;
    if (!Read(entity + kAnimViewOffset, &view, 4) || !view) return false;
    if (!Read(view + kBoneTableOffset, &bones, 4) || bones) return false;
    if (outView) *outView = view;
    return true;
}

// Attach the local resident model to one bare ghost. Guarded with SEH so a bad
// attach counts a fault instead of crashing; if it does not take, the ghost is
// still bare next frame and gets flagged again.
void AttachOneGhost(uint32_t entity) {
    if (!entity || AlreadyAttached(entity)) return;
    // Only attach when the ghost has NO skeleton yet. Forcing Entity::AttachModel
    // over an already-modelled ghost corrupts it (dumps 42708/5988, 16.09.:
    // crash moved to a garbage vtable call, eip=0 / eip=eax). So a ghost the
    // engine already gave a (malformed) model is left to the skinning guard, not
    // re-attached here.
    uint32_t view = 0;
    if (!IsBareGhostEntity(entity, &view)) return;
    using Resolve = uint32_t (__cdecl*)(const void*);
    const uint32_t entry = reinterpret_cast<Resolve>(base + kResolveEntryRva)(localModelGuid.data());
    if (!entry) { InterlockedIncrement(&ghostAttachMissing); return; }
    InterlockedIncrement(&ghostAttachTries);
    __try {
        using Attach = void (__thiscall*)(void*, uint32_t);
        reinterpret_cast<Attach>(base + kAttachModelRva)(reinterpret_cast<void*>(entity), entry);
        InterlockedIncrement(&ghostAttachDone);
        if (attachedGhostCount < std::size(attachedGhosts)) attachedGhosts[attachedGhostCount++] = entity;
        // Success signal: does the AnimView now carry a bone table?
        uint32_t bones = 0;
        if (Read(view + kBoneTableOffset, &bones, 4) && bones)
            InterlockedIncrement(&ghostBonesFilled);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&ghostAttachFaults);
    }
}

// Runs on the main thread from Poll. Scans every constructed Player for a bare
// ghost; this catches the ghost the frame it is created (its NetCreate goes
// through the same player-construction hook that fills playerObjects), so the
// model attaches before the other bone-table readers (0x1092150 / 0x106B1D0 /
// 0x1076765 / 0x1073E6D) run on it. The anim guard's flagged entity is handled
// too, as a backstop for a ghost that never entered playerObjects.
void AttachGhostSkeleton() {
    // The ghosts go with the level; a new one may get an old address.
    if (!inLevel) attachedGhostCount = 0;
    if (!ghostSkeleton || !localModelKnown) return;
    for (unsigned i = 0; i < std::size(placement::playerObjects); ++i) {
        const uint32_t player = static_cast<uint32_t>(InterlockedCompareExchange(&placement::playerObjects[i], 0, 0));
        if (player) AttachOneGhost(player);
    }
    const uint32_t flagged = static_cast<uint32_t>(InterlockedExchange(&ghostAttachEntity, 0));
    if (flagged) AttachOneGhost(flagged);
}

// Lighter check than IsBareGhostEntity, for the attach-hook moment: the ghost's
// AnimView may not exist yet, so only require a Player-vtable entity with the
// ghost flag set. The local player is authoritative (flag clear) and is never
// matched. Returns the local resident model entry to substitute, or 0.
volatile LONG ghostModelSwaps = 0;
bool IsGhostPlayer(uint32_t entity) {
    uint32_t vtable = 0, proxy = 0, flags = 0;
    if (!Read(entity, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva) return false;
    if (!Read(entity + kPlayerReflectableOffset + 0xC, &proxy, 4) || !proxy) return false;
    return Read(proxy + 0x30, &flags, 4) && (flags & 2);
}
uint32_t GhostModelEntryForAttach(uint32_t entity) {
    if (!ghostSkeleton || !localModelKnown || !entity) return 0;
    if (!IsGhostPlayer(entity)) return 0;
    using Resolve = uint32_t (__cdecl*)(const void*);
    const uint32_t entry = reinterpret_cast<Resolve>(base + kResolveEntryRva)(localModelGuid.data());
    if (entry) InterlockedIncrement(&ghostModelSwaps);
    return entry;
}

// Skinning guard: skip a sub-mesh whose bone-index array is garbage.
// The engine-built ghost carries a valid body skeleton but one malformed
// sub-mesh (dump 40276: skin instance 0x3CEBAC00, count 6, index array at
// +0x11C full of huge junk like 0x843094B2). The skinning routine 0x01076450
// (cdecl, one arg = the skin instance at [ebp+8]) writes each skinned matrix to
// [skin+0x120] + boneIndex*64, so a junk index writes far out of bounds and
// faults (0x1076765). This inline hook runs the routine normally for valid
// meshes and skips it only when its index array is junk, leaving the rest of
// the ghost (the body) to render. Only installed when GhostSkeleton is on.
constexpr uint32_t kSkinBuildRva = 0xC76450;
constexpr uint32_t kSkinCount = 0xBC, kSkinIndexArray = 0x11C, kSkinMaxBone = 0x1000;
safetyhook::InlineHook skinHook;
volatile LONG skinChecks = 0, skinSkips = 0;

bool SkinIndicesGarbage(uint32_t skin) {
    if (!skin) return false;
    __try {
        const uint32_t count = *reinterpret_cast<const uint32_t*>(skin + kSkinCount);
        if (count == 0) return false;
        if (count > 4096) return true;   // an absurd bone count is itself junk
        const uint32_t idxArr = *reinterpret_cast<const uint32_t*>(skin + kSkinIndexArray);
        if (!idxArr) return false;
        const uint32_t* idx = reinterpret_cast<const uint32_t*>(idxArr);
        const uint32_t n = count < 64 ? count : 64;
        for (uint32_t i = 0; i < n; ++i)
            if (idx[i] >= kSkinMaxBone) return true;   // no real skeleton has 4096+ bones
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;   // unreadable skinning data -> skip rather than fault
    }
}

// --- Stand-in partner: a real, healthy actor puppeted to the partner ---
// The engine ghost is a render dead end (crash history above). Instead this adopts
// a fully-constructed local creature (any Animated actor with a real skeleton),
// reskin it to Isaac's resident model, freeze it so it is harmless, and teleport
// it to the partner's live position every frame. Only proven addresses are used
// (AttachModel 0x7D2230, resolver 0x790650, the AnimView speed field). The actor
// pointers come from the shared AttachModel hook (enemies reskin through it too).
// Off by default (INI [Coop] Standin). Independent of the ghost path.
bool standin = false;
uint32_t standinEntity = 0;
bool standinReskinned = false;
volatile LONG standinAdopted = 0, standinDrives = 0, standinReskins = 0;
constexpr uint32_t kAnimViewInner = 0x40, kAnimSpeedInner = 0x134;

uint32_t recordedActors[64]{};
volatile LONG recordedActorHead = 0;
void RecordActor(uint32_t entity) {
    if (!standin || !entity) return;
    // skip the player/ghost (same primary vtable): only a creature is puppeted
    uint32_t vtable = 0;
    if (!Read(entity, &vtable, 4) || vtable == base + placement::kPlayerPrimaryVtableRva) return;
    const LONG slot = InterlockedIncrement(&recordedActorHead) - 1;
    recordedActors[slot & 63] = entity;
}

bool ActorHasSkeleton(uint32_t entity, uint32_t* outView) {
    uint32_t view = 0, bones = 0;
    if (!Read(entity + kAnimViewOffset, &view, 4) || !view) return false;
    if (!Read(view + kBoneTableOffset, &bones, 4) || !bones) return false;
    if (outView) *outView = view;
    return true;
}

// Build a rotation from yaw (forward = sin,0,cos) and write the actor transform:
// right +0x90, up +0xA0, forward +0xB0, position +0xC0 (the FuncSpawn/actor
// layout). Written every frame so the creature is pinned at the partner.
void DriveStandin(uint32_t entity) {
    const double rad = static_cast<double>(partnerYaw) * 3.14159265358979 / 180.0;
    const float s = static_cast<float>(std::sin(rad)), c = static_cast<float>(std::cos(rad));
    const float right[3] = {c, 0.0f, -s};
    const float up[3]    = {0.0f, 1.0f, 0.0f};
    const float fwd[3]   = {s, 0.0f, c};
    WriteBytes(entity + 0x90, right, 12);
    WriteBytes(entity + 0xA0, up, 12);
    WriteBytes(entity + 0xB0, fwd, 12);
    WriteBytes(entity + 0xC0, partnerPos, 12);
    // freeze: AnimView (+0x194) -> inner (+0x40) -> speed (+0x134) = 0, so no
    // attack/locomotion animation advances -> harmless, and it holds its pose.
    uint32_t view = 0, inner = 0;
    if (Read(entity + kAnimViewOffset, &view, 4) && view && Read(view + kAnimViewInner, &inner, 4) && inner) {
        const float zero = 0.0f;
        WriteBytes(inner + kAnimSpeedInner, &zero, 4);
    }
    InterlockedIncrement(&standinDrives);
}

void ReskinStandin(uint32_t entity) {
    if (standinReskinned || !localModelKnown) return;
    using Resolve = uint32_t (__cdecl*)(const void*);
    const uint32_t entry = reinterpret_cast<Resolve>(base + kResolveEntryRva)(localModelGuid.data());
    if (!entry) return;
    __try {
        using Attach = void (__thiscall*)(void*, uint32_t);
        reinterpret_cast<Attach>(base + kAttachModelRva)(reinterpret_cast<void*>(entity), entry);
        standinReskinned = true;
        InterlockedIncrement(&standinReskins);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Adopt the nearest recorded creature (valid skeleton) to the local player.
void AdoptStandin() {
    const uint32_t player = static_cast<uint32_t>(InterlockedCompareExchange(&placement::playerObject, 0, 0));
    float me[3]{};
    if (!player || !Read(player + placement::kPlayerPosition, me, 12)) return;
    uint32_t best = 0; double bestDist = 1e18;
    for (unsigned i = 0; i < 64; ++i) {
        const uint32_t e = recordedActors[i];
        if (!e || e == player) continue;
        uint32_t vtable = 0;
        if (!Read(e, &vtable, 4) || vtable == base + placement::kPlayerPrimaryVtableRva) continue;
        if (!ActorHasSkeleton(e, nullptr)) continue;
        float p[3]{};
        if (!Read(e + 0xC0, p, 12)) continue;
        const double dx = p[0]-me[0], dy = p[1]-me[1], dz = p[2]-me[2];
        const double d = dx*dx + dy*dy + dz*dz;
        if (d < bestDist) { bestDist = d; best = e; }
    }
    if (best) { standinEntity = best; standinReskinned = false; InterlockedIncrement(&standinAdopted); }
}

void PollStandin() {
    if (!standin || !connected || !partnerAddrKnown) return;
    CaptureLocalModel();
    if (!standinEntity) { AdoptStandin(); return; }
    // if the adopted actor died/was freed its skeleton check fails; drop it
    if (!ActorHasSkeleton(standinEntity, nullptr)) { standinEntity = 0; standinReskinned = false; return; }
    ReskinStandin(standinEntity);
    DriveStandin(standinEntity);
}

void __cdecl OnSkinBuild(uint32_t skin) {
    InterlockedIncrement(&skinChecks);
    if (SkinIndicesGarbage(skin)) {
        InterlockedIncrement(&skinSkips);
        // Neutralise this sub-mesh for EVERY consumer, not just this routine: a
        // second, differently-shaped function (0x106B1B0, called from the
        // thiscall 0x10769D0 -> client crash 0x106B1D0) loops over the same
        // instance's bone count. Zeroing the count (+0xBC) makes all their loops
        // no-ops, and it persists across frames: one guard, every reader safe.
        // The malformed sub-mesh just does not render.
        __try { *reinterpret_cast<volatile uint32_t*>(skin + kSkinCount) = 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }
    skinHook.call<void>(skin);
}

// What crosses the wire. NetObjSync gives every reflectable of the level a
// sub key with the machine byte in it, so nothing on the peer ever matches
// by NetID: without a filter the peer creates ghost copies of the whole
// level (dumps 41200/37236 of 22:04, both sides at once, a ghost entity
// without matrices in the animation transform). The multiplayer maps avoid
// that with the spawn filter (clients spawn no world objects); the campaign
// cannot. So TNL::GhostConnection::objectInScope (0x014160A0) is filtered:
// of the session's NetProxies only those whose class is Player pass, the
// scope proxies always pass. INI SyncOnlyPlayers=0 lifts the filter.
constexpr uint32_t kObjectInScope = 0x10160A0, kNetProxyVtable = 0x1722C0C;
bool syncOnlyPlayers = true, ghostPlayer = false, checkEnemyTemplates = false, hostNPCs = false;
bool TemplateScopeAllowed(uint32_t object);
bool TemplateScopeAllowedFor(uint32_t connection, uint32_t object);
safetyhook::InlineHook scopeHook;
volatile LONG scopeAllowed = 0, scopeDropped = 0;
volatile LONG scopeAllowedPlayer = 0, scopeAllowedNpc = 0;
volatile LONG scopeDroppedNpc = 0, scopeDroppedOther = 0;
volatile LONG scopeDroppedTemplate = 0, scopeUnreadable = 0;
void __fastcall OnObjectInScope(void* connection, void*, uint32_t object) {
    if (syncOnlyPlayers && object) {
        uint32_t vtable = 0, classInfo = 0;
        char name[32]{};   // long enough for "PairedAttackCoordinator" (23 chars); a 16-byte buffer made that compare read past its end
        if (Read(object, &vtable, 4) && vtable == base + kNetProxyVtable) {
            // A NecroPlayer proxy only exists when somebody plays as a necromorph
            // (runtime_coop_necro.inc); the partner must ghost it or never sees it.
            bool player = Read(object + 0x44, &classInfo, 4) && classInfo && Read(classInfo + 0x20, name, sizeof(name)) &&
                          (std::memcmp(name, "Player", 7) == 0 || std::memcmp(name, "NecroPlayer", 12) == 0 ||
                           std::memcmp(name, "PairedAttackCoordinator", 24) == 0 ||   // the grab: created on the victim's side, ghosted to the attacker
                           std::memcmp(name, "NecroPlayerRespawnManager", 26) == 0);   // the necro type menu's bookkeeping, built by the host (necro::EnsureRespawnManager)
            if (!player) { InterlockedIncrement(&scopeDropped); return; }
            InterlockedIncrement(&scopeAllowed);
        }
    }
    if ((checkEnemyTemplates || hostNPCs) && !syncOnlyPlayers && object) {
        uint32_t vt = 0, classInfo = 0;
        char name[32]{};
        if (Read(object, &vt, 4) && vt == base + kNetProxyVtable) {
            if (!Read(object + 0x44, &classInfo, 4) || !classInfo || !Read(classInfo + 0x20, name, sizeof(name))) {
                InterlockedIncrement(&scopeUnreadable); return;
            }
            const bool npc = std::memcmp(name, "DeadSpaceNPC", 13) == 0;
            // NecroPlayer counts as a player here too (HostNPCs=1 is the real
            // configuration; the SyncOnlyPlayers branch above never runs then).
            const bool player = std::memcmp(name, "Player", 7) == 0 || std::memcmp(name, "NecroPlayer", 12) == 0 ||
                                std::memcmp(name, "PairedAttackCoordinator", 24) == 0 ||   // grab coordinator: victim side creates, attacker side needs the ghost
                                std::memcmp(name, "NecroPlayerRespawnManager", 26) == 0;   // type menu bookkeeping, host-built (necro::EnsureRespawnManager)
            if (hostNPCs && ((!npc && !player) || (npc && !IsHost()))) {
                InterlockedIncrement(npc ? &scopeDroppedNpc : &scopeDroppedOther);
                return;
            }
            // per partner: a late joiner must not ride on the first partner's template answer
            if (npc && checkEnemyTemplates && !TemplateScopeAllowedFor(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(connection)), object)) {
                InterlockedIncrement(&scopeDroppedTemplate); return;
            }
            // Count what gets through, per kind. scope_allowed is only counted
            // in the SyncOnlyPlayers branch above, which is off in every real
            // configuration, so it read 0 next to fifteen thousand drops.
            InterlockedIncrement(player ? &scopeAllowedPlayer : &scopeAllowedNpc);
        }
    }
    scopeHook.thiscall<void>(connection, object);
}

// UDP socket shared by every TNL::Socket the engine constructs (there is one)
SOCKET udp = INVALID_SOCKET;
unsigned socketUsers = 0;
volatile LONG packetsSent = 0, packetsReceived = 0, sendErrors = 0, lastSendError = 0, bindError = 0;
safetyhook::InlineHook ctorHook, dtorHook, sendHook, recvHook;

// A side channel independent of TNL and the engine's Player ghosting: each
// plugin sends its own player transform on its own UDP socket (localPort+1)
// to the peer's data port, and reads the partner's. This carries the coop
// payload (where the other Isaac is, which way it faces, its move state)
// without instantiating a skeleton-less ghost that the animation pipeline
// would crash on. It is the backbone for a phantom model or a partner marker,
// and it works with GhostPlayer=0, so it never destabilises the game.
// The data port is localPort + kDataPortOffset. +1 collided with the other
// instance's TNL port on one machine (host data 27016 == client TNL 27016,
// bind_error 10048, no connection); +1000 keeps both instances' four ports
// distinct (27015/28015 host, 27016/28016 client).
constexpr unsigned kDataPortOffset = 1000;
SOCKET dataSock = INVALID_SOCKET;
// The only packet that never carried a sender. With two machines that was fine,
// because there was only one other end; with four it is the difference between
// three players and one player teleporting. 52 bytes so the receiver's
// length dispatch still tells it apart from everything else.
// The sequence is not padding. Position is the one thing sent often enough for
// UDP reordering to show, and applying an older sample after a newer one is a
// visible stutter rather than a lost packet. On a LAN it will never fire; that
// is exactly why it has to be in before the first remote test, not after.
// flags: bit 0 = helmet open (Player flag bit 0x17, see helmet:: below). The
// word used to be an unfilled moveState; the body itself moves through TNL.
struct PlayerState {
    uint32_t magic, token, machine, sequence;
    float x, y, z, yaw, forward[3];
    uint32_t flags, tick;
};
constexpr uint32_t kStateFlagHelmetOpen = 1;
static_assert(sizeof(PlayerState) == 52, "Unexpected player state wire layout");
constexpr uint32_t kStateMagic = 0x32504B44;  // "DKP2"

// Everything that has to exist once per partner rather than once.
struct PeerRuntime {
    float pos[3]{};
    float yaw = 0.0f;
    uint32_t flags = 0, appliedFlags = 0xFFFFFFFFu, lastTick = 0, lastSequence = 0, stateToken = 0;
    uint32_t reordered = 0;
    DWORD lastState = 0;
    coop_ping::Monitor link;
    // Readiness is per partner. One shared coop_peer::State locks extra
    // machines out: Accept refuses a second token while the first is alive,
    // and Ready requires the peer to have acknowledged this machine's token
    // and epoch, so a host with one State only acknowledges the first client.
    // 2026-09-17: the third instance sat at peer_ready=0 with 715 readiness
    // packets received and registration_attempts=0.
    coop_peer::State readiness;
    // Everything that describes this player rather than the link. One shared
    // set meant the last packet to arrive overwrote the one before, whoever
    // sent it.
    coop_suit::Receiver suit;
    coop_loadout::Receiver loadout;
    coop_death::Receiver death;
};
coop_peers::Table peerTable;
PeerRuntime peerRuntime[coop_peers::kMaxPeers]{};
volatile LONG peerStatesReceived = 0;
uint32_t stateSequence = 0;
// Top bit of the machine number marks a position the host is passing on for
// somebody else. Machine numbers are 0..255, so the bit is free, and this
// keeps the packet the same size, which matters because the receiver
// dispatches by length.
constexpr uint32_t kRelayBit = 0x80000000u;
volatile LONG relayForwarded = 0, relayDropped = 0;

unsigned PeerSlot(const coop_peers::Peer* peer) {
    return static_cast<unsigned>(peer - &peerTable.peers[0]);
}
// The lowest occupied slot. Whoever sits there is "the partner" for everything
// that has not been taught to count past one yet.
unsigned FirstPeerSlot() {
    for (unsigned i = 0; i < coop_peers::kMaxPeers; ++i)
        if (peerTable.peers[i].used) return i;
    return 0;
}

// What the side channel sends, by packet kind (the first word of every packet
// is its magic). Written as rates every ten seconds, so a session log shows
// which module loads the host.
namespace traffic {
constexpr unsigned kKinds = 24;
struct Kind { uint32_t magic; volatile LONG packets, bytes; };
Kind kinds[kKinds]{};
volatile LONG other = 0;
DWORD windowStart = 0;
void Note(const void* data, int length, int copies) {
    if (!data || length < 4 || copies <= 0) return;
    uint32_t magic = 0;
    std::memcpy(&magic, data, 4);
    for (auto& k : kinds) {
        uint32_t owner = k.magic;
        if (!owner) {
            // Claim a free row; a thread that lost the race reads the winner's magic.
            owner = static_cast<uint32_t>(InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&k.magic), static_cast<LONG>(magic), 0));
            if (!owner) owner = magic;
        }
        if (owner != magic) continue;
        InterlockedExchangeAdd(&k.packets, copies);
        InterlockedExchangeAdd(&k.bytes, copies * length);
        return;
    }
    InterlockedExchangeAdd(&other, copies);
}
void WriteDiagnostics(FILE* log) {
    const DWORD now = GetTickCount();
    if (!windowStart) { windowStart = now; return; }
    const DWORD span = now - windowStart;
    if (span < 10000) return;
    windowStart = now;
    char line[1024];
    int used = std::snprintf(line, sizeof(line), "coop_traffic window=%u ms", static_cast<unsigned>(span));
    LONG totalPackets = 0, totalBytes = 0;
    for (auto& k : kinds) {
        if (!k.magic) continue;
        const LONG packets = InterlockedExchange(&k.packets, 0), bytes = InterlockedExchange(&k.bytes, 0);
        if (!packets) continue;
        totalPackets += packets; totalBytes += bytes;
        char tag[5] = {};
        std::memcpy(tag, &k.magic, 4);
        for (char& c : tag) if (c && (c < 0x20 || c > 0x7E)) c = '?';
        if (used > 0 && used < static_cast<int>(sizeof(line)) - 48)
            used += std::snprintf(line + used, sizeof(line) - used, " %s=%.1f/s,%.0fB/s", tag, packets * 1000.0 / span, bytes * 1000.0 / span);
    }
    std::fprintf(log, "%s total=%.1f/s,%.0fB/s unknown=%ld\n", line, totalPackets * 1000.0 / span, totalBytes * 1000.0 / span,
                 InterlockedExchange(&other, 0));
}
}  // namespace traffic

// One datagram to every peer in the table. Returns how many went out.
int SendToPeers(const void* data, int length) {
    if (dataSock == INVALID_SOCKET) return 0;
    int sent = 0;
    for (const auto& peer : peerTable.peers) {
        if (!peer.Addressable()) continue;   // forwarded players have no address
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = peer.ip;
        to.sin_port = peer.port;
        if (::sendto(dataSock, static_cast<const char*>(data), length, 0,
                     reinterpret_cast<sockaddr*>(&to), sizeof(to)) == length)
            ++sent;
    }
    traffic::Note(data, length, sent);
    return sent;
}
// The slot a packet belongs to, by the token it carries rather than by where
// it arrived from. A client only ever talks to the host, so a suit or a loadout
// that started life on another client reaches it wearing the host's return
// address; filing it by address would dress the host's Isaac in somebody else's
// suit. Unknown tokens get a slot of their own, with no address, exactly like a
// forwarded position.
unsigned SlotForToken(uint32_t token, unsigned fallback, uint32_t now) {
    if (!token) return fallback;
    if (auto* known = peerTable.FindByToken(token)) return PeerSlot(known);
    if (auto* made = peerTable.AcceptRelayed(token, 0xFFFFFFFFu, now)) return PeerSlot(made);
    return fallback;
}

// Pass a packet on to everybody else. Only the host does this, and only for
// packets that arrived from a partner it talks to directly, so a forwarded
// packet is never forwarded again and cannot loop.
void RelayToOthers(const void* data, int length, unsigned fromSlot) {
    if (!IsHost() || dataSock == INVALID_SOCKET) return;
    for (const auto& other : peerTable.peers) {
        if (!other.Addressable() || PeerSlot(&other) == fromSlot) continue;
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = other.ip;
        to.sin_port = other.port;
        if (::sendto(dataSock, static_cast<const char*>(data), length, 0,
                     reinterpret_cast<sockaddr*>(&to), sizeof(to)) == length) {
            InterlockedIncrement(&relayForwarded);
            traffic::Note(data, length, 1);
        }
    }
}

// Suit, loadout, death and the template exchange still speak to one partner:
// there is one ghost body, and the template request/response is a conversation
// with the host by construction. Accepting them from any known peer
// keeps them working while the table carries more than one.
namespace phantom {   // runtime_coop_dementia.inc: the Dementia mutator's hallucinated enemies, on this machine only
extern volatile LONG spawning, allowedOnClient, keptOnHost, worldHitsKept, hitsZeroed, dropsRefused, messagesDropped, botsIgnored, restSkipped;
bool Is(uint32_t entity);
bool Any();
}
namespace necro { int LocalRole(); }   // runtime_coop_necro.inc: 0 human, 1 living necro, 2 necro at the vents, 3 a necro watching a scene as Isaac
namespace necro { bool AskWatch(DWORD now, unsigned author); bool WatchCuesHeld(); bool WatchReady(); bool WatchBlocks(); const char* WatchWaitReason(); }   // runtime_coop_necro.inc
namespace spectate {   // runtime_coop_spectate.inc: a dead human watching a partner (role 4)
bool Active(); unsigned SubjectSlot(); bool SwallowDeathMessage(uint32_t owner, uint32_t message); bool TakeDeath(uint32_t player, uint32_t damage);
bool OnRespawned(uint32_t old, uint32_t after); void OnRestart(); void OnLevelExit(); void OnPartnerReached(const uint32_t* guid, uint32_t token, uint32_t seq);
void OnPartnerVote(uint32_t hash, const float* at); bool AskScene(DWORD now, unsigned author); bool SceneCuesHeld(); bool SceneReady();
extern volatile LONG restartsRefused, catchupRefused, necroKeyRefused;
}
namespace clientprog {   // runtime_coop_clientprogress.inc
uint64_t OwnHostId(); uint32_t HostSaveKey(); void OnHost(uint32_t token, uint32_t saveKey, uint32_t idHi, uint32_t idLo); void NoteSaveAttempt(const char* why);
void BeforeIsaacGoes(uint32_t player); void AfterIsaacReturns(uint32_t player, const char* why);
}
// Who a machine plays. A necro player in the campaign is no voter: the humans
// never wait for it at a door or a story trigger (16.10., from the code: the door
// wait and the progression quorum counted every partner, so a necro's machine,
// which reaches neither doors nor triggers, and at the vents sends no position at
// all, held the humans 180 s at every door). Every machine says its role once a
// second in a 12-byte 'NROL' packet; the host passes the clients' on. Silent for
// 10 s: a human again.
namespace roles {
constexpr uint32_t kMagic = 0x4C4F524Eu;   // 'NROL'
constexpr uint8_t kVersion = 1;
#pragma pack(push, 1)
struct Packet { uint32_t magic; uint8_t version, role; uint16_t reserved; uint32_t token; };
#pragma pack(pop)
static_assert(sizeof(Packet) == 12, "role packet size");
uint8_t role[coop_peers::kMaxPeers]{};
DWORD heardAt[coop_peers::kMaxPeers]{};
uint32_t roleToken[coop_peers::kMaxPeers]{};   // a reused slot does not inherit a role
int lastSentRole = -1;
DWORD sentAt = 0;
volatile LONG sent = 0, received = 0, changes = 0;
bool IsNecro(unsigned slot, DWORD now) {
    // signed age: heardAt comes from the receive loop and may be newer than a caller's earlier now
    return slot < coop_peers::kMaxPeers && role[slot] && heardAt[slot] && TicksSince(now, heardAt[slot]) < 10000 && roleToken[slot] &&
           peerTable.peers[slot].token == roleToken[slot];
}
bool AnyNecroPeer(DWORD now) {
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) if (IsNecro(s, now)) return true;
    return false;
}
int RoleOf(unsigned slot, DWORD now) { return IsNecro(slot, now) ? role[slot] : 0; }
// Role 4: a dead human watching a partner until the next checkpoint (runtime_coop_spectate.inc).
// He counts as no voter and no target like a necro at the vents, but never as a necro.
bool Spectating(unsigned slot, DWORD now) { return RoleOf(slot, now) == 4; }
bool SpectatingMachine(uint32_t machine, DWORD now) {
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s)
        if (peerTable.peers[s].used && peerTable.peers[s].machine == machine && Spectating(s, now)) return true;
    return false;
}
unsigned Spectators(DWORD now) {
    unsigned n = 0;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) if (peerTable.peers[s].used && Spectating(s, now)) ++n;
    return n;
}
DWORD spectatorBackAt[coop_peers::kMaxPeers]{};   // a partner's role 4 -> 0: his new body is no new world
DWORD roleChangedAt[coop_peers::kMaxPeers]{};     // any change of a partner's role (a team switch: no new world either)
// A partner machine whose necro player watches a scene as Isaac: every body of it stays hidden here.
bool Watching(unsigned slot, DWORD now) { return RoleOf(slot, now) == 3; }
bool WatchingMachine(uint32_t machine, DWORD now) {
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s)
        if (peerTable.peers[s].used && peerTable.peers[s].machine == machine && Watching(s, now)) return true;
    return false;
}
bool LocalNecro() { return necro::LocalRole() == 1; }
void Send(DWORD now, uint32_t token);
bool OnBytes(const void* data, int size, unsigned slot, DWORD now);
}
bool KnownPeerToken(uint32_t token) {
    return token && peerTable.FindByToken(token) != nullptr;
}
volatile LONG statePacketsSent = 0, statePacketsReceived = 0;
uint32_t partnerLastTick = 0;
DWORD partnerLastSeen = 0, lastStateSend = 0;
// The host has no configured peer, so it learns the partner's data address
// from the first packet it receives and replies there. The client sends to
// its configured peer (host) and updates the learned address to match.
sockaddr_in partnerAddr{};
coop_peer::State peerReadiness;

// --- survival over the side channel ---
// The host runs the waves (the randomizer's director). Every notice it shows goes
// to the partners too, three times 100 ms apart; a client shows each sequence
// number once. Clients run no director of their own (DS2Coop_NetRole).
namespace hud { void SetRemote(const char* text); void SetOwn(const char* line); }   // runtime_coop_hud.inc
namespace zone { void SetRemote(const coop_zone::Wire& w); }   // runtime_coop_zone.inc
namespace mpnative { bool OnBytes(const void* data, int size); }   // runtime_coop_mpnative.inc
namespace versus { bool OnBytes(const void* data, int size); }     // runtime_coop_versus.inc
namespace botnet { constexpr int kBotSuitBytes = 32; bool OnBotSuitBytes(const void* data, int size); }   // bot suits, below
namespace nameplate { bool OnNameBytes(const void* data, int size); }   // runtime_coop_nameplate.inc
namespace voices { bool OnPacket(const coop_voices::Packet& p, bool fromHost); bool OnEventPacket(const void* data, int size); }   // runtime_coop_voices.inc
namespace respawn { bool OnLifeBytes(const void* data, int size, uint32_t now); }   // runtime_coop_respawn.inc
namespace uselock { bool RefuseLocal(uint32_t player); void Started(uint32_t player, bool started); bool OnBytes(const void*, int, unsigned, DWORD, bool); void ResetClaims(); }   // runtime_coop_uselock.inc
namespace marks { void SetEnemies(const float* xyz, int count); }   // runtime_coop_marks.inc
namespace debugmenu { void OnAdmin(int command, int value, const float* pos); }   // runtime_coop_debugmenu.inc: the host's tools
namespace bugreport { bool OnBytes(const void* data, int size); }   // runtime_coop_bugreport.inc
namespace difficulty {   // runtime_coop_difficulty.inc
bool OnSurvnet(uint8_t kind, uint32_t token, const void* text, uint32_t value);
void BeforeLevelRequest(const char* why);
void NoteCampaignOrigin();
void RestoreCampaignOrigin(const char* why);
uint8_t LobbyByte();
uint16_t CampaignField();
void SetLaunch(int d, const char* source);
bool OpenPicker(uint32_t entry, const char* map);
void OnCampaignScreen();
// for any picker UI: hold a pick (0..4, -1 clears), the pick and its HUD name (CASUAL ... HARD CORE)
bool SetPick(int row);
int PickValue();
const char* PickName();
}
namespace survnet {
constexpr uint32_t kMagic = 0x544E5653;   // 'SVNT'
constexpr uint8_t kVersion = 1;
#pragma pack(push, 1)
struct Packet { uint32_t magic; uint8_t version, kind; uint16_t repeat; uint32_t token, seq; char text[140]; };
#pragma pack(pop)
static_assert(sizeof(Packet) == 156, "survival packet size");
struct Pending { Packet packet; DWORD at; int left; };
Pending pending[16]{};
uint32_t seqOut = 0;
coop_versus::SeqWindow seen;
uint32_t seenToken = 0;
survival_capsules::Snapshot hostCapsules;
survival_capsules::Replica remoteCapsules;
DWORD capsulesSentAt = 0;
unsigned capsuleRepeats = 0;
bool hadCapsules = false;
SRWLOCK lock = SRWLOCK_INIT;
volatile LONG queued = 0, sentPackets = 0, received = 0, shown = 0, duplicates = 0;
// Kind 2: the host's wave ended and every machine drops its own rewards next to
// its own Isaac (pickups are not ghosted). Counted here, taken by the director.
volatile LONG rewardEvents = 0, lastRewardWave = 0, rewardOverflow = 0;
survival::RewardQueue receivedRewards;
survival::RewardPartner rewardPartners[coop_peers::kMaxPeers]{};
survival::RewardReceiver rewardReceiver;
uint32_t rewardRun = 1;
DWORD rewardHelloAt = 0;
volatile LONG rewardGrants = 0, rewardAcked = 0;

void NewRewardRun() {
    rewardRun = survival::NextRewardSerial(rewardRun);
    for (auto& p : rewardPartners) p.Restart();
}
void ResetRewards() {
    AcquireSRWLockExclusive(&lock);
    receivedRewards.Clear();
    rewardReceiver.Restart();
    NewRewardRun();
    rewardHelloAt = 0;
    ReleaseSRWLockExclusive(&lock);
}
void SendReward(const survival::RewardWire& p, const coop_peers::Peer& peer) {
    if (!peer.Addressable() || dataSock == INVALID_SOCKET) return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = peer.ip;
    to.sin_port = peer.port;
    if (::sendto(dataSock, reinterpret_cast<const char*>(&p), sizeof(p), 0,
                 reinterpret_cast<sockaddr*>(&to), sizeof(to)) == sizeof(p)) traffic::Note(&p, sizeof(p), 1);
}
bool ReceiveReward(const survival::RewardWire& p, unsigned slot, bool hostEndpoint) {
    const auto& source = peerTable.peers[slot];
    if (!p.sender || source.token != p.sender || p.target != peerReadiness.token || !p.generation) return false;
    AcquireSRWLockExclusive(&lock);
    bool ok = false;
    if (IsHost()) {
        auto& partner = rewardPartners[slot];
        partner.Bind(source.token);
        if (p.kind == survival::RewardKind::Hello) ok = partner.Hello(p.generation);
        else if (p.kind == survival::RewardKind::Ack && partner.Ack(p, rewardRun)) { ok = true; InterlockedIncrement(&rewardAcked); }
    } else if (hostEndpoint) {
        bool added = false;
        ok = rewardReceiver.Accept(p, peerReadiness.token, receivedRewards, added);
        if (added) {
            InterlockedExchange(&lastRewardWave, static_cast<LONG>(p.wave));
            InterlockedIncrement(&rewardEvents);
        }
        if (ok) {
            auto ack = p;
            ack.kind = survival::RewardKind::Ack;
            ack.sender = peerReadiness.token;
            ack.target = p.sender;
            SendReward(ack, source);
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return ok;
}
void TickRewards(DWORD now) {
    AcquireSRWLockExclusive(&lock);
    if (IsHost()) {
        for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
            const auto& peer = peerTable.peers[s];
            auto& partner = rewardPartners[s];
            partner.Bind(peer.Addressable() ? peer.token : 0);
            survival::RewardWire p;
            if (partner.Grant(peerReadiness.token, rewardRun, now, p)) {
                SendReward(p, peer);
                InterlockedIncrement(&rewardGrants);
            }
        }
    } else if (!rewardHelloAt || now - rewardHelloAt >= 1000) {
        for (const auto& peer : peerTable.peers) {
            if (!peer.token || !peer.Is(peerIp, htons(static_cast<u_short>(peerPort + kDataPortOffset)))) continue;
            survival::RewardWire p{survival::kRewardMagic, survival::RewardKind::Hello, peerReadiness.token, peer.token, rewardReceiver.generation};
            SendReward(p, peer);
            rewardHelloAt = now;
            break;
        }
    }
    ReleaseSRWLockExclusive(&lock);
}
// Kind 3: the host's survival state for the music, (wave << 8) | state.
volatile LONG musicEvents = 0, musicValue = 0;
void ShowLocal(const char* text);   // notice::Enqueue, below
// Any thread (the randomizer's director calls through DS2Coop_QueueNotice).
// The latest director state (3) and HUD line (4), sent again at once to a partner who just arrived.
struct Latest { bool have = false; char text[140]{}; uint32_t value = 0; };
Latest latest[2]{};
volatile LONG newPartnerResends = 0;
void Queue(uint8_t kind, const char* text, uint32_t value) {
    if (!network || !IsHost()) return;
    AcquireSRWLockExclusive(&lock);
    if (kind == 2) {
        for (auto& p : rewardPartners)
            if (p.token && !p.pending.Push(value)) InterlockedIncrement(&rewardOverflow);
    }
    if (kind == 3 && (value & 0xffu) == 1u && (!latest[0].have || (latest[0].value & 0xffu) != 1u)) NewRewardRun();
    if (kind == 3 || kind == 4) {
        Latest& l = latest[kind - 3];
        l.have = true;
        std::snprintf(l.text, sizeof(l.text), "%s", text ? text : "");
        l.value = value;
    }
    for (auto& p : pending) {
        if (p.left > 0) continue;
        p.packet = Packet{kMagic, kVersion, kind, 0, 0, ++seqOut, {}};
        std::snprintf(p.packet.text, sizeof(p.packet.text), "%s", text ? text : "");
        if (kind == 2) std::memcpy(p.packet.text, &survival::kRewardMagic, 4);
        if (kind == 3) std::memcpy(p.packet.text, &rewardRun, 4);
        std::memcpy(p.packet.text + sizeof(p.packet.text) - 4, &value, 4);
        p.at = 0;
        p.left = 3;
        InterlockedIncrement(&queued);
        break;
    }
    ReleaseSRWLockExclusive(&lock);
}
void Broadcast(const char* text) {
    if (text && text[0]) Queue(1, text, 0);
}
// Host: a partner just arrived; he gets the wave state now, not at the next 10 s repeat.
void ResendLatest() {
    if (!network || !IsHost()) return;
    Latest copy[2];
    AcquireSRWLockExclusive(&lock);
    if (hadCapsules) capsuleRepeats = 3;
    copy[0] = latest[0];
    copy[1] = latest[1];
    ReleaseSRWLockExclusive(&lock);
    for (unsigned i = 0; i < 2; ++i)
        if (copy[i].have) { Queue(static_cast<uint8_t>(3 + i), copy[i].text, copy[i].value); InterlockedIncrement(&newPartnerResends); }
}
// Kinds 5 and 6: a Mercenaries capsule the host placed or that is gone (id 0: all).
// Pickups are not ghosted, so every partner drops its own copy of each capsule
// and deletes it again on the host's word. {id, x, y, z, golden} in the text.
using CapsuleWire = survival_capsules::Item;
struct CapsuleEvent { int kind; CapsuleWire wire; };
CapsuleEvent capsuleEvents[16]{};
volatile LONG capsuleHead = 0, capsuleTail = 0, capsuleSent = 0, capsuleReceived = 0, capsuleOverflow = 0;
void QueueCapsule(uint8_t kind, const CapsuleWire& wire) {
    if (!network || !IsHost()) return;
    AcquireSRWLockExclusive(&lock);
    hostCapsules.Apply(kind, wire);
    hadCapsules = true;
    capsuleRepeats = 3;
    for (auto& p : pending) {
        if (p.left > 0) continue;
        p.packet = Packet{kMagic, kVersion, kind, 0, 0, ++seqOut, {}};
        std::memcpy(p.packet.text, &wire, sizeof(wire));
        p.at = 0;
        p.left = 3;
        InterlockedIncrement(&queued);
        InterlockedIncrement(&capsuleSent);
        break;
    }
    ReleaseSRWLockExclusive(&lock);
}
// Kind 7: Hold the Zone, where the zone is and how it stands (coop_zone::Wire).
// A change goes out three times, the once-a-second refresh once.
volatile LONG zoneSent = 0, zoneReceived = 0;
void QueueZone(const coop_zone::Wire& wire, int repeats) {
    if (!network || !IsHost()) return;
    AcquireSRWLockExclusive(&lock);
    for (auto& p : pending) {
        if (p.left > 0) continue;
        p.packet = Packet{kMagic, kVersion, 7, 0, 0, ++seqOut, {}};
        std::memcpy(p.packet.text, &wire, sizeof(wire));
        p.at = 0;
        p.left = repeats < 1 ? 1 : repeats;
        InterlockedIncrement(&queued);
        InterlockedIncrement(&zoneSent);
        break;
    }
    ReleaseSRWLockExclusive(&lock);
}
// Kind 8: the last enemies of a wave, {count, count x xyz}. Sent when the count
// changes and once a second while any are marked; the marks fade without it.
volatile LONG enemySent = 0, enemyReceived = 0;
int enemyLastCount = -1;
DWORD enemyLastAt = 0;
void QueueEnemies(const float* xyz, int count) {
    if (!network || !IsHost()) return;
    if (count < 0) count = 0;
    if (count > 8) count = 8;
    const DWORD now = GetTickCount();
    if (count == enemyLastCount && (!count || now - enemyLastAt < 1000)) return;
    enemyLastCount = count;
    enemyLastAt = now;
    AcquireSRWLockExclusive(&lock);
    for (auto& p : pending) {
        if (p.left > 0) continue;
        p.packet = Packet{kMagic, kVersion, 8, 0, 0, ++seqOut, {}};
        p.packet.text[0] = static_cast<char>(count);
        if (count && xyz) std::memcpy(p.packet.text + 4, xyz, static_cast<size_t>(count) * 12);
        p.at = 0;
        p.left = 1;
        InterlockedIncrement(&queued);
        InterlockedIncrement(&enemySent);
        break;
    }
    ReleaseSRWLockExclusive(&lock);
}
// Kind 12: a host tool every machine applies (debug menu HOST tab), {command, value,
// host position}. Three sends 100 ms apart; the sequence check shows each once.
#pragma pack(push, 1)
struct AdminWire { int32_t command, value; float pos[3]; };
#pragma pack(pop)
volatile LONG adminSent = 0, adminReceived = 0;
void QueueAdmin(int command, int value, const float* pos) {
    if (!network || !IsHost()) return;
    AdminWire wire{command, value, {}};
    if (pos) std::memcpy(wire.pos, pos, sizeof(wire.pos));
    AcquireSRWLockExclusive(&lock);
    for (auto& p : pending) {
        if (p.left > 0) continue;
        p.packet = Packet{kMagic, kVersion, 12, 0, 0, ++seqOut, {}};
        std::memcpy(p.packet.text, &wire, sizeof(wire));
        p.at = 0;
        p.left = 3;
        InterlockedIncrement(&queued);
        InterlockedIncrement(&adminSent);
        break;
    }
    ReleaseSRWLockExclusive(&lock);
}
// Kind 13: the host's difficulty (coop_difficulty::HostWire, value (generation << 8) | d),
// three times on a change and once every 2 s. Kind 14 is a client's request the other way;
// the difficulty module sends that one itself.
void QueueDifficulty(const void* wire, size_t size, uint32_t value, int repeats) {
    if (!network || !IsHost() || size > sizeof(Packet::text) - 4) return;
    AcquireSRWLockExclusive(&lock);
    for (auto& p : pending) {
        if (p.left > 0) continue;
        p.packet = Packet{kMagic, kVersion, 13, 0, 0, ++seqOut, {}};
        std::memcpy(p.packet.text, wire, size);
        std::memcpy(p.packet.text + sizeof(p.packet.text) - 4, &value, 4);
        p.at = 0;
        p.left = repeats < 1 ? 1 : repeats;
        InterlockedIncrement(&queued);
        break;
    }
    ReleaseSRWLockExclusive(&lock);
}
// Kind 9: a profile XP payment of the host's director (coop_progress.h). The text
// starts with a 0 byte, so an older build shows nothing.
#pragma pack(push, 1)
struct ProgressWire { uint8_t zero, pad[3]; uint32_t run, wave, xp, flags; };
#pragma pack(pop)
volatile LONG progressSent = 0, progressReceived = 0;
void QueueProgress(uint32_t run, uint32_t wave, uint32_t xp, uint32_t flags) {
    if (!network || !IsHost()) return;
    const ProgressWire wire{0, {}, run, wave, xp, flags};
    AcquireSRWLockExclusive(&lock);
    for (auto& p : pending) {
        if (p.left > 0) continue;
        p.packet = Packet{kMagic, kVersion, 9, 0, 0, ++seqOut, {}};
        std::memcpy(p.packet.text, &wire, sizeof(wire));
        p.at = 0;
        p.left = 3;
        InterlockedIncrement(&queued);
        InterlockedIncrement(&progressSent);
        break;
    }
    ReleaseSRWLockExclusive(&lock);
}
// The partner's director takes them one at a time (DS2Coop_SurvivalCapsule).
bool TakeCapsule(CapsuleEvent& out) {
    AcquireSRWLockExclusive(&lock);
    if (remoteCapsules.desired.seq) {
        const bool ready = remoteCapsules.Take(out.kind, out.wire);
        ReleaseSRWLockExclusive(&lock);
        return ready;
    }
    const bool any = capsuleTail != capsuleHead;
    if (any) {
        out = capsuleEvents[capsuleTail % 16];
        remoteCapsules.offered.Apply(out.kind, out.wire);
        InterlockedIncrement(&capsuleTail);
    }
    ReleaseSRWLockExclusive(&lock);
    return any;
}
// Game thread, with the side channel's poll.
void Tick(DWORD now) {
    if (!network) return;
    TickRewards(now);
    if (!IsHost()) return;
    AcquireSRWLockExclusive(&lock);
    const bool clockMode = latest[0].have && (latest[0].value & 0xff) == 5;
    if (hadCapsules && (capsuleRepeats || hostCapsules.count || clockMode) &&
        now - capsulesSentAt >= (capsuleRepeats ? 100u : 2000u)) {
        capsulesSentAt = now;
        hostCapsules.token = peerReadiness.token;
        if (!++hostCapsules.seq) ++hostCapsules.seq;
        SendToPeers(&hostCapsules, sizeof(hostCapsules));
        if (capsuleRepeats) --capsuleRepeats;
    }
    for (auto& p : pending) {
        if (p.left <= 0 || (p.at && now - p.at < 100)) continue;
        p.packet.token = peerReadiness.token;
        p.packet.repeat = static_cast<uint16_t>(3 - p.left);
        if (SendToPeers(&p.packet, sizeof(p.packet)) > 0) InterlockedIncrement(&sentPackets);
        p.at = now;
        --p.left;
    }
    ReleaseSRWLockExclusive(&lock);
}
bool OnPacket(const Packet& p) {
    // the difficulty goes both ways and folds its own copies by generation and request
    if ((p.kind == 13 || p.kind == 14) && p.version == kVersion) {
        uint32_t value = 0;
        std::memcpy(&value, p.text + sizeof(p.text) - 4, 4);
        return difficulty::OnSurvnet(p.kind, p.token, p.text, value);
    }
    if (p.version != kVersion || IsHost()) return false;
    InterlockedIncrement(&received);
    if (seenToken != p.token) { seen = {}; seenToken = p.token; }
    if (!seen.Fresh(p.seq)) { InterlockedIncrement(&duplicates); return true; }
    if (p.kind == 3) {
        uint32_t value = 0, run = 0;
        std::memcpy(&run, p.text, 4);
        if (run) {
            AcquireSRWLockExclusive(&lock);
            const bool current = rewardReceiver.ObserveRun(p.token, run, receivedRewards);
            ReleaseSRWLockExclusive(&lock);
            if (!current) return true;
        }
        std::memcpy(&value, p.text + sizeof(p.text) - 4, 4);
        InterlockedExchange(&musicValue, static_cast<LONG>(value));
        InterlockedIncrement(&musicEvents);
        return true;
    }
    if (p.kind == 2) {
        uint32_t format = 0;
        std::memcpy(&format, p.text, 4);
        if (format == survival::kRewardMagic) return true; // a per-peer grant carries the award
        uint32_t wave = 0;
        std::memcpy(&wave, p.text + sizeof(p.text) - 4, 4);
        if (wave > INT32_MAX) return false;
        AcquireSRWLockExclusive(&lock);
        if (!receivedRewards.Push(wave)) InterlockedIncrement(&rewardOverflow);
        ReleaseSRWLockExclusive(&lock);
        InterlockedExchange(&lastRewardWave, static_cast<LONG>(wave));
        InterlockedIncrement(&rewardEvents);
        return true;
    }
    if (p.kind == 5 || p.kind == 6) {
        CapsuleEvent e{p.kind, {}};
        std::memcpy(&e.wire, p.text, sizeof(e.wire));
        AcquireSRWLockExclusive(&lock);
        if (remoteCapsules.desired.seq) { ReleaseSRWLockExclusive(&lock); return true; }
        if (capsuleHead - capsuleTail >= 16) InterlockedIncrement(&capsuleOverflow);
        else {
            capsuleEvents[capsuleHead % 16] = e;
            InterlockedIncrement(&capsuleHead);
        }
        ReleaseSRWLockExclusive(&lock);
        InterlockedIncrement(&capsuleReceived);
        return true;
    }
    if (p.kind == 8) {
        const int count = static_cast<unsigned char>(p.text[0]) <= 8 ? static_cast<unsigned char>(p.text[0]) : 0;
        float xyz[8 * 3]{};
        if (count) std::memcpy(xyz, p.text + 4, static_cast<size_t>(count) * 12);
        marks::SetEnemies(xyz, count);
        InterlockedIncrement(&enemyReceived);
        return true;
    }
    if (p.kind == 9) {
        ProgressWire w{};
        std::memcpy(&w, p.text, sizeof(w));
        progress::FromHost(w.run, w.wave, w.xp, w.flags);
        InterlockedIncrement(&progressReceived);
        return true;
    }
    if (p.kind == 12) {
        AdminWire w{};
        std::memcpy(&w, p.text, sizeof(w));
        debugmenu::OnAdmin(w.command, w.value, w.pos);
        InterlockedIncrement(&adminReceived);
        return true;
    }
    if (p.kind == 7) {
        coop_zone::Wire w{};
        std::memcpy(&w, p.text, sizeof(w));
        zone::SetRemote(w);
        InterlockedIncrement(&zoneReceived);
        return true;
    }
    if (p.kind == 4) {
        // The host's HUD line and event line, "line\x1Fflash".
        char hudText[sizeof(p.text) - 3]{};
        std::memcpy(hudText, p.text, sizeof(p.text) - 4);
        hud::SetRemote(hudText);
        return true;
    }
    char text[sizeof(p.text) + 1]{};
    std::memcpy(text, p.text, sizeof(p.text) - 4);
    ShowLocal(text);
    InterlockedIncrement(&shown);
    return true;
}
}  // namespace survnet


// The local half of the readiness handshake is per machine, not per partner,
// so it is kept in one place and pushed into every peer's state. The peer half
// (their token, their epoch, whether they acknowledged this machine) is theirs.
void SyncLocalReadiness() {
    for (auto& runtime : peerRuntime) {
        runtime.readiness.token = peerReadiness.token;
        runtime.readiness.epoch = peerReadiness.epoch;
        runtime.readiness.player = peerReadiness.player;
        runtime.readiness.localReady = peerReadiness.localReady;
    }
}

// The template exchange and a handful of report lines still read the old single
// object, so the primary partner's half is copied back into it. Everything that
// counts past one reads the per-peer states directly.
void MirrorPrimaryReadiness() {
    const auto& source = peerRuntime[FirstPeerSlot()].readiness;
    peerReadiness.peerToken = source.peerToken;
    peerReadiness.peerEpoch = source.peerEpoch;
    peerReadiness.peerSequence = source.peerSequence;
    peerReadiness.peerSeen = source.peerSeen;
    peerReadiness.peerAckToken = source.peerAckToken;
    peerReadiness.peerAckEpoch = source.peerAckEpoch;
    peerReadiness.peerReady = source.peerReady;
    peerReadiness.havePeer = source.havePeer;
}

// Registration waits for somebody, not for everybody. A player who is still
// loading must not hold the others out of the session.
bool AnyPeerReady(uint32_t now) {
    for (unsigned i = 0; i < coop_peers::kMaxPeers; ++i)
        if (peerTable.peers[i].used && peerRuntime[i].readiness.Ready(now)) return true;
    return false;
}
unsigned ReadyPeerCount(uint32_t now) {
    unsigned n = 0;
    for (unsigned i = 0; i < coop_peers::kMaxPeers; ++i)
        if (peerTable.peers[i].used && peerRuntime[i].readiness.Ready(now)) ++n;
    return n;
}

bool requirePeerReady = false;
DWORD lastReadinessSend = 0;
volatile LONG readinessSent = 0, readinessReceived = 0, dataRejected = 0;
#include "runtime_coop_templates.inc"

void OpenDataSocket() {
    if (dataSock != INVALID_SOCKET) return;
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
    dataSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dataSock == INVALID_SOCKET) return;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(static_cast<u_short>(localPort + kDataPortOffset));
    if (::bind(dataSock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        closesocket(dataSock); dataSock = INVALID_SOCKET; return;
    }
    u_long nonBlocking = 1;
    ioctlsocket(dataSock, FIONBIO, &nonBlocking);
    if (peerPort) {
        partnerAddr.sin_family = AF_INET;
        partnerAddr.sin_addr.s_addr = peerIp;
        partnerAddr.sin_port = htons(static_cast<u_short>(peerPort + kDataPortOffset));
        partnerAddrKnown = true;
        // A configured peer goes into the table straight away and is never
        // recycled out of it: it is meant to be there even while it is
        // still loading and has not said a word.
        peerTable.Configure(partnerAddr.sin_addr.s_addr, partnerAddr.sin_port, GetTickCount());
    }
    // Every dialled partner, not only the first. The bodies are a mesh because
    // TNL will not forward a ghost; a side channel that stayed a star would
    // route every position through the host and measure the round trip to the
    // wrong machine. The host's relay stays as the fallback for anyone this
    // instance was not told about. Data port = game port + kDataPortOffset.
    for (unsigned i = 0; i < dialledCount; ++i) {
        if (!dialledIps[i] || !dialledPorts[i]) continue;
        peerTable.Configure(dialledIps[i],
                            htons(static_cast<u_short>(dialledPorts[i] + kDataPortOffset)),
                            GetTickCount());
    }
}

// --- Death mirror, observation stage ---
// Retail Player::ProcessDeath is thiscall with three stack arguments (ret 0xC).
// Prologue verified identical in dumps 46012 and 46076:
//   sub esp,3C / push ebx,ebp,esi / mov esi,ecx / mov eax,[esi+3D8]
// The following branch tests Player+3D8 bit 13 (multiplayer game); with that bit
// clear the campaign path continues at 0x878BFE and runs the normal death
// sequence, which is what ends in a checkpoint restart.
//
// Observation: when the local campaign Player enters that path, the death is
// announced to the peers over the side channel and the arguments are recorded.
// Replaying it locally (MirrorDeath, TriggerLocalDeath below) passes a zeroed
// argument 0, which some paths dereference (test byte [arg0+0x90],2).
// Suit replication state. The uniform module publishes the local suit into
// suitSender and reads the peer suit from suitReceiver; this file only moves
// them across the side channel.
bool suitSync = false;
coop_suit::Sender suitSender;
coop_suit::Receiver suitReceiver;
volatile LONG suitSent = 0, suitReceived = 0;

// Loadout replication. Reads the local inventory with the tested reader in
// coop_weapon_lookup.h and publishes the FULL proxy GUID of every carried item,
// because the native ghost update only carries a name hash and one hash maps to
// two different weapons (DS2_COOP_GUN_NETWORK_2026-09-16.md).
bool loadoutSync = false;
coop_loadout::Sender loadoutSender;
coop_loadout::Receiver loadoutReceiver;
volatile LONG loadoutSent = 0, loadoutReceived = 0, loadoutSlotsRead = 0, loadoutUnreadable = 0, loadoutUnknownClass = 0, loadoutUnknownVtable = 0;
// The host's bots: what each carries, so a client can arm its ghost (sub-key 0xF0 + bot).
static_assert(kMaxBots <= coop_loadout::kMaxBots, "bot loadout index range");
coop_loadout::Sender botLoadoutSenders[kMaxBots];
coop_loadout::Receiver botLoadouts[kMaxBots];
volatile LONG botLoadoutSent = 0, botLoadoutReceived = 0, botLoadoutRejected = 0;

// Inventory layout, verified in coop_weapon_lookup.h: entries at Player+0xC08,
// count at +0xC0C, allocated at +0xC10, 12-byte entries whose item reference is
// at +4 and points at item+0x10.
unsigned ReadLocalLoadout(uint32_t player, coop_loadout::Entry* out, unsigned capacity) {
    uint32_t vt = 0, entries = 0, count = 0, allocated = 0;
    if (!player || !Read(player, &vt, 4) || vt != base + placement::kPlayerPrimaryVtableRva) return 0;
    if (!Read(player + 0xC08, &entries, 4) || !Read(player + 0xC0C, &count, 4) ||
        !Read(player + 0xC10, &allocated, 4)) return 0;
    if (count > allocated || allocated > 16 || (count && (!entries || entries > 0xFFFFFF3F))) return 0;
    if (count > capacity) count = capacity;
    auto reader = [](uint32_t address, void* buffer, size_t size) { return Read(address, buffer, size); };
    unsigned filled = 0;
    for (uint32_t i = 0; i < count; ++i) {
        coop_loadout::Entry entry;
        uint32_t ref = 0;
        if (Read(entries + i * 12 + 4, &ref, 4) && ref >= 0x10010 && ref <= 0xFFFFFD00) {
            const auto identity = coop_weapon::InspectIdentity(ref - 0x10, static_cast<uint32_t>(base), reader);
            if (identity.readable & 4) {
                for (unsigned w = 0; w < 4; ++w) entry.guid[w] = identity.proxyGuid[w];
                entry.have = (entry.guid[0] | entry.guid[1] | entry.guid[2] | entry.guid[3]) != 0;
            } else if (identity.kind != coop_weapon::Kind::Unknown) {
                InterlockedIncrement(&loadoutUnreadable);
            } else if (identity.readable & 1) {
                // a class coop_weapon_lookup.h does not know: the partner's ghost never gets it
                InterlockedIncrement(&loadoutUnknownClass);
                InterlockedExchange(&loadoutUnknownVtable, static_cast<LONG>(identity.vtable - static_cast<uint32_t>(base)));
            }
        }
        out[filled++] = entry;
    }
    return filled;
}

constexpr uint32_t kProcessDeathRva = 0x478B40;
bool deathSync = false;
coop_death::Sender deathSender;
coop_death::Receiver deathReceiver;
volatile LONG pingSent = 0, pingReplied = 0, pingRepliesIn = 0;
safetyhook::InlineHook deathHook;
volatile LONG deathLocalSeen = 0, deathOtherSeen = 0, deathSent = 0, deathReceived = 0;
volatile LONG deathAcksIn = 0, deathAcksOut = 0, deathAbandoned = 0;
volatile LONG deathLastArgs[3]{}, deathLastFlags = 0;

// --- Weapon provisioning survey (read-only) ---
// Answers, for every weapon the peer carries, whether this machine could hand it
// to the ghost at all. Verified from the retail entry scanner at 0x00DB28D0:
// the Quartermaster keeps its entries pointer at manager+0x14 and their count at
// manager+0x18, entries are 0x3C bytes and the proxy GUID sits at entry+0.
//
// This only reads. Provisioning has to go through RequisitionGun (0x00DDD5F0,
// thiscall on the manager), whose bool return is not a completion guarantee, and
// whose IssueGun step needs an existing GunProxy placeholder in the inventory,
// which is what the report's ghost= field shows.
// The Quartermaster singleton is [0x0201C0D0]: that is the `this` every retail
// caller of RequisitionGun and of the entry finder 0x00DB28D0 loads (dump
// 45856, 2026-09-17: 37 entries of 0x3C bytes, the peers' GUIDs among them).
// 0x02021300, used until then, is a different manager with 0x1550 entries,
// which is why every lookup came back "count > 128" and nothing was ever
// provisioned (qm=-2, tries=0 in every log).
constexpr uint32_t kQuartermaster = 0x1C1C0D0, kRequisitionGun = 0x9DD5F0;
// thiscall(qm, Player* owner, const GUID* proxy), ret 8: drops the owner's queued form
// and its reference, leaves the stream load running.
constexpr uint32_t kCancelRequisitionGun = 0x9D6C40;
constexpr uint32_t kQuartermasterForms = 0x24;
constexpr uint32_t kGunEntryStride = 0x3C, kGunEntryStream = 0x34, kGunEntryRefs = 0x39;

int FindQuartermasterEntry(const uint32_t guid[4], uint32_t& stream, uint32_t& refs) {
    uint32_t manager = 0, entries = 0, count = 0;
    stream = refs = 0;
    if (!Read(base + kQuartermaster, &manager, 4) || !manager) return -2;
    if (!Read(manager + 0x14, &entries, 4) || !Read(manager + 0x18, &count, 4) ||
        !entries || count > 128) return -2;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t entry = entries + i * kGunEntryStride;
        uint32_t words[4]{};
        if (!Read(entry, words, 16)) return -2;
        if (std::memcmp(words, guid, 16) != 0) continue;
        Read(entry + kGunEntryStream, &stream, 4);
        unsigned char count8 = 0;
        if (Read(entry + kGunEntryRefs, &count8, 1)) refs = count8;
        return static_cast<int>(i);
    }
    return -1;
}

// A gun for a ghost that never got a placeholder for it.
//
// The game hands a ghost GunProxy placeholders only for the weapons its owner
// had when it was first ghosted; a weapon bought later never reaches it, so
// UpdateGhostGunState finds nothing to hold (23.09.: the host's two starting
// guns showed on the client's ghost, his two new ones were "missing").
// Quartermaster::IssueGun (PC 0x00DD6CC0) only swaps an existing placeholder:
// without one it spawns the gun and leaves it lying there, which is what the
// RequisitionGun fallback did, eleven times for one bought gun.
//
// This is IssueGun without the swap, ending the way IssueGun itself ends,
// with Gun::Pickup (vtable +0x1F8, thiscall(gun, Sentient* owner, bool vfx),
// ret 8). Pickup checks the gun is free, the owner a Player, the inventory not
// full and the weapon not already held; it sends nothing over the network and
// expects a non-authoritative owner (it only picks other VFX hints for one).
// Found against the Xbox debug build, 23.09.; the steps:
//   SimManager [0x0201C0A0]: GetSpawnInfo 0x00776440 (SpawnInfo* out, guid*,
//     bool) ret 0xC, SpawnEntity 0x00776480 (SpawnInfo*, flags) ret 8, with the
//     spawn filter byte [[0x02021300]+0x28] cleared around it
//   the template is GunEntry+0x24, the proxy GUID GunEntry+0x00 goes to
//     gun+0x404 (Gun::m_gunProxyGUID; SetGunProxyID is inlined on PC), and the
//     entry's refcount (+0x39) keeps the stream resident under the new gun
//   Pickup fails -> Base::Delete 0x007568B0 (cdecl) and the refcount back
constexpr uint32_t kGunEntryTemplate = 0x24, kGunProxyId = 0x404, kGunPickupSlot = 0x1F8;
constexpr uint32_t kSimManagerPtr = 0x1C1C0A0, kGetSpawnInfoRva = 0x376440, kSpawnEntityRva = 0x376480;
constexpr uint32_t kSpawnFilterOwner = 0x1C21300, kBaseDeleteRva = 0x3568B0;
constexpr uint32_t kStreamManagerPtr = 0x1C1C098, kStreamStatusRva = 0xDAA0;
volatile LONG ghostGunsGiven = 0, ghostGunsRefused = 0, ghostGunFaults = 0, ghostGunStreamWaits = 0;

bool GhostGunStreamResident(uint32_t entry) {
    uint32_t handle = 0, streams = 0;
    if (!Read(entry + kGunEntryStream, &handle, 4) || !handle) return false;
    if (!Read(base + kStreamManagerPtr, &streams, 4) || !streams) return false;
    using Status = int (__thiscall*)(void*, uint32_t);
    return reinterpret_cast<Status>(base + kStreamStatusRva)(reinterpret_cast<void*>(streams), handle) == 3;
}

// 1 given, 0 not ready yet (stream still loading, no manager), -1 refused or faulted.
int GiveGhostGun(uint32_t ghost, const uint32_t guid[4]) {
    uint32_t manager = 0, entries = 0, sim = 0, filterOwner = 0;
    uint32_t stream = 0, refs = 0;
    const int index = FindQuartermasterEntry(guid, stream, refs);
    if (index < 0 || !Read(base + kQuartermaster, &manager, 4) || !manager || !Read(manager + 0x14, &entries, 4) || !entries) return 0;
    const uint32_t entry = entries + static_cast<uint32_t>(index) * kGunEntryStride;
    if (!GhostGunStreamResident(entry)) { InterlockedIncrement(&ghostGunStreamWaits); return 0; }
    if (!Read(base + kSimManagerPtr, &sim, 4) || !sim || !Read(base + kSpawnFilterOwner, &filterOwner, 4) || !filterOwner) return 0;
    uint32_t gun = 0;
    const unsigned char filterOff = 0;
    unsigned char filterWas = 1;
    Read(filterOwner + 0x28, &filterWas, 1);   // put back as found, not forced to 1
    __try {
        alignas(16) unsigned char info[0x40]{};
        WriteBytes(filterOwner + 0x28, &filterOff, 1);
        using GetSpawnInfo = void* (__thiscall*)(void*, void*, uint32_t, bool);
        using SpawnEntity = uint32_t (__thiscall*)(void*, void*, uint32_t);
        reinterpret_cast<GetSpawnInfo>(base + kGetSpawnInfoRva)(reinterpret_cast<void*>(sim), info, entry + kGunEntryTemplate, false);
        uint32_t infoPacket = 0;   // the spawn packet at +0x10, what the spawn filter reads first
        std::memcpy(&infoPacket, info + 0x10, 4);
        if (infoPacket) gun = reinterpret_cast<SpawnEntity>(base + kSpawnEntityRva)(reinterpret_cast<void*>(sim), info, 0);
        WriteBytes(filterOwner + 0x28, &filterWas, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        WriteBytes(filterOwner + 0x28, &filterWas, 1);
        InterlockedIncrement(&ghostGunFaults);
        return -1;
    }
    if (!gun) { InterlockedIncrement(&ghostGunsRefused); return -1; }
    uint32_t proxy[4]{};
    unsigned char count = 0;
    if (Read(entry, proxy, 16)) WriteBytes(gun + kGunProxyId, proxy, 16);
    if (Read(entry + kGunEntryRefs, &count, 1) && count < 0xFF) { ++count; WriteBytes(entry + kGunEntryRefs, &count, 1); }
    bool taken = false;
    __try {
        uint32_t vtable = 0, pickup = 0;
        if (Read(gun, &vtable, 4) && Read(vtable + kGunPickupSlot, &pickup, 4) && pickup) {
            using Pickup = bool (__thiscall*)(void*, uint32_t, bool);
            taken = reinterpret_cast<Pickup>(pickup)(reinterpret_cast<void*>(gun), ghost, false);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&ghostGunFaults);
    }
    if (taken) { InterlockedIncrement(&ghostGunsGiven); return 1; }
    // Refused (inventory full, weapon already held): no stray gun is left behind.
    __try {
        using Delete = void (__cdecl*)(uint32_t);
        reinterpret_cast<Delete>(base + kBaseDeleteRva)(gun);
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&ghostGunFaults); }
    if (Read(entry + kGunEntryRefs, &count, 1) && count) { --count; WriteBytes(entry + kGunEntryRefs, &count, 1); }
    InterlockedIncrement(&ghostGunsRefused);
    return -1;
}

// The remote player's body, found through the intrusive player list rather than
// a scan: the local player comes from the native PlayerManager, so anything else
// in that list flagged as a ghost is the peer.
// A partner's body straight out of the PlayerManager's slots, the way the
// ownership report finds it (slot 1, ghost=1, every session). The list walk
// below it never reached that body in the sessions of 22.09.: every weapon
// report said ghost=none, the provisioning never looked at a single slot
// (tries=0, no_placeholder=0), and nobody saw a partner's newly bought gun.
// machine == 0xFFFFFFFF (kNoMachine, defined further down) takes any ghost.
uint32_t MachineOf(uint32_t player);   // defined below
uint32_t GhostFromPlayerManager(uint32_t machine) {
    constexpr uint32_t kNoMachine = 0xFFFFFFFFu;
    uint32_t manager = 0, slots = 0, capacity = 0;
    if (!Read(base + kPlayerManagerRva, &manager, 4) || !manager || !Read(manager + 4, &slots, 4) || !slots ||
        !Read(manager + 8, &capacity, 4) || !capacity || capacity > 8) return 0;
    for (uint32_t i = 0; i < capacity; ++i) {
        uint32_t bp = 0, vtable = 0;
        if (!Read(slots + 4 * i, &bp, 4) || bp < 0x103C0) continue;
        const uint32_t entity = bp - 0x3C0;
        if (!Read(entity, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva) continue;
        if (!IsGhostPlayer(entity)) continue;
        if (machine == kNoMachine || MachineOf(entity) == machine) return entity;
    }
    return 0;
}

uint32_t FindGhostPlayer() {
    if (const uint32_t slotted = GhostFromPlayerManager(0xFFFFFFFFu)) return slotted;
    // Seeded from slot 0 whatever its class, and a NecroPlayer node (ours or a
    // partner's) is stepped over rather than ending the walk: the peers' Isaacs
    // behind it are still there.
    const uint32_t local = LocalSlotZeroEntity();
    if (!local) return 0;
    for (unsigned direction = 0; direction < 2; ++direction) {
        uint32_t entity = local;
        for (unsigned hops = 0; hops < 8; ++hops) {
            uint32_t next = 0;
            if (direction == 0) {
                if (!Read(entity + 0x54, &next, 4)) break;
            } else {
                uint32_t pprev = 0;
                if (!Read(entity + 0x50, &pprev, 4) || pprev < 0x10054) break;
                next = pprev - 0x54;
            }
            uint32_t vtable = 0;
            if (!next || !Read(next, &vtable, 4)) break;
            if (vtable == base + kNecroPrimaryVtableRva) { entity = next; continue; }
            if (vtable != base + placement::kPlayerPrimaryVtableRva) break;
            if (IsGhostPlayer(next)) return next;
            entity = next;
        }
    }
    return 0;
}

// Which machine owns this player object. AllocatePlayerSubKey builds every key
// as (machine << 24) | counter so that instances cannot mint the same identity,
// and SetSubKey stores it at reflectable-0x1C, which is entity+0x40: the top
// byte of a ghost's sub-key is the machine whose player it is.
//
// Returns 0xFFFFFFFF when the object has no sub-key yet, which means it has not
// been registered and belongs to nobody.
constexpr uint32_t kEntitySubKey = 0x40, kNoMachine = 0xFFFFFFFFu;
uint32_t MachineOf(uint32_t player) {
    uint32_t key = 0;
    if (!player || !Read(player + kEntitySubKey, &key, 4) || !key) return kNoMachine;
    return key >> 24;
}

// The ghost body belonging to one particular partner, rather than whichever
// ghost happens to come first. With two players those were the same thing and
// FindGhostPlayer was enough; with four it would dress the wrong Isaac.
uint32_t GhostForMachine(uint32_t machine) {
    if (machine != kNoMachine)
        if (const uint32_t slotted = GhostFromPlayerManager(machine)) return slotted;
    const uint32_t local = LocalSlotZeroEntity();
    if (!local || machine == kNoMachine) return 0;
    for (unsigned direction = 0; direction < 2; ++direction) {
        uint32_t entity = local;
        for (unsigned hops = 0; hops < 16; ++hops) {
            uint32_t next = 0;
            if (direction == 0) {
                if (!Read(entity + 0x54, &next, 4)) break;
            } else {
                uint32_t pprev = 0;
                if (!Read(entity + 0x50, &pprev, 4) || pprev < 0x10054) break;
                next = pprev - 0x54;
            }
            uint32_t vtable = 0;
            if (!next || !Read(next, &vtable, 4)) break;
            if (vtable == base + kNecroPrimaryVtableRva) { entity = next; continue; }   // a necro body is not a Player ghost; keep walking
            if (vtable != base + placement::kPlayerPrimaryVtableRva) break;
            if (IsGhostPlayer(next) && MachineOf(next) == machine) return next;
            entity = next;
        }
    }
    return 0;
}

// What the ghost's inventory holds for one weapon GUID: nothing, a GunProxy
// placeholder (the replicated stand-in every ghost gets), or a real Gun (the
// model that is actually shown). The 2026-09-17 logs said ghost_has=1 for
// every peer weapon while nobody ever saw a gun on a ghost: those were the
// placeholders. IssueGun turns a placeholder into a gun; that is the step.
enum class GhostGun { None, Placeholder, Real };
GhostGun GhostWeaponState(uint32_t ghost, const uint32_t guid[4]) {
    uint32_t entries = 0, count = 0, allocated = 0;
    if (!ghost || !Read(ghost + 0xC08, &entries, 4) || !Read(ghost + 0xC0C, &count, 4) ||
        !Read(ghost + 0xC10, &allocated, 4) || count > allocated || allocated > 16) return GhostGun::None;
    auto reader = [](uint32_t address, void* buffer, size_t size) { return Read(address, buffer, size); };
    GhostGun state = GhostGun::None;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t ref = 0;
        if (!Read(entries + i * 12 + 4, &ref, 4) || ref < 0x10010 || ref > 0xFFFFFD00) continue;
        const auto identity = coop_weapon::InspectIdentity(ref - 0x10, static_cast<uint32_t>(base), reader);
        if (!(identity.readable & 4) || std::memcmp(identity.proxyGuid, guid, 16) != 0) continue;
        if (identity.kind == coop_weapon::Kind::Gun) return GhostGun::Real;
        state = GhostGun::Placeholder;
    }
    return state;
}

// Provisioning stage: hand the ghost a weapon it does not carry.
//
// RequisitionGun (0x00DDD5F0, thiscall on the Quartermaster, three stack
// arguments, ret 0xC) is the sanctioned entry point: it resolves the entry by
// FULL proxy GUID, issues immediately when the stream is already loaded and
// otherwise queues the request and starts the load. Its bool result is NOT a
// completion guarantee, so nothing here treats it as one.
//
// Deliberately conservative, because IssueGun skips its ownership sequence when
// the inventory has no matching placeholder:
//   * only when the Quartermaster actually knows the GUID (entry index >= 0),
//   * a missing stream is loaded by RequisitionGun itself and taken back when
//     the level ends first (WatchStartedLoads),
//   * never for a weapon the ghost already has,
//   * at most eight requests per peer loadout generation, SEH-guarded.
bool provisionWeapons = false;
bool provisionCreate = false;   // experimental: also stream-preload weapons the ghost lacks entirely
volatile LONG provisionTries = 0, provisionAccepted = 0, provisionRefused = 0, provisionFaults = 0,
              provisionNoPlaceholder = 0, provisionLoads = 0, provisionCreateTries = 0, provisionCreateCancelled = 0,
              provisionCreateCancelFaults = 0, provisionCreateEvents = 0;
// Per partner: the loadout generation last acted on, and how many requests
// were spent on it.
// Partners first, then the host's bots (kMaxPeers + bot).
constexpr unsigned kProvisionSlots = coop_peers::kMaxPeers + kMaxBots;
uint32_t provisionGeneration[kProvisionSlots]{};
unsigned provisionAttempts[kProvisionSlots]{};
// 16.10.: every role change on the client rebuilt the host's ghost, often at the same
// address, the game deleted the guns we had given the old body, and the eight attempts
// were spent on the first body. A body we had armed that holds no gun of ours any more
// counts as new.
bool provisionArmed[kProvisionSlots]{};
volatile LONG provisionNewBodies = 0, provisionRebuiltInPlace = 0, provisionBudgetStops = 0, provisionBodyLines = 0;

// Each partner's own weapons on each partner's own ghost.
//
// This used to take the single loadout and hand it to FindGhostPlayer, which
// returns whichever ghost comes first. With two players those were the same
// body; with four it would have armed one Isaac with everybody's guns and left
// the others empty-handed. The owner of a body is the top byte of its sub-key,
// so each peer's ghost is found the same way its suit is.
// Only while the level stands, and never across its end. 25.09. 16:18 (dump
// 31660): the ghost appeared, the host picked a checkpoint in the debug menu a
// moment later, and in between this asked the Quartermaster for the partner's
// pulse rifle. RequisitionGun loads a weapon stream through 0x0074C4D0, which
// requests its parents first: handles 2E-31 were text_assets_global,
// global_assets_global, global_assets and weapn_str\pulserifl. That request
// kept global_assets coming back while the level went down; its completion
// waited on the stopped render thread (coop_renderstall), 245 global_assets
// entities were respawned, the world close had to tear them down out of
// order, and iMsgStreamUnloading then called a freed user at 0x00790622. The
// linegun crashes of 24.09. (dumps 18692, 28492, 5200, 27536) were the same
// chain. So a load is only started in a settled level, with no level change
// pending and a ghost that has stood for a while; and a load started here that
// has not finished when the level ends, restarts or loses the ghost is taken
// back through the Quartermaster's own release (0x00DD6E60, thiscall(entry,
// unload=1), ret 8; its destructor does the same for every entry).
namespace level { uint32_t GameMainLoop(); }
namespace gate { void Event(const char* format, ...); }
constexpr uint32_t kQmReleaseEntry = 0x9D6E60;
constexpr DWORD kGhostSteadyMs = 5000;
// A gun whose stream is already loaded needs no load, so it goes on a new body
// sooner (Versus respawns, 13.10.: 5-6 s empty-handed each time).
constexpr DWORD kGhostResidentMs = 1500;
struct StartedLoad { int entry; uint32_t ghost; DWORD at; };
StartedLoad startedLoads[8]{};
unsigned startedLoadCount = 0;
uint32_t steadyGhost[kProvisionSlots]{};
DWORD steadySince[kProvisionSlots]{};
volatile LONG provisionHeldBack = 0, provisionCancelled = 0, provisionCancelFaults = 0;
bool LevelChangePending() {
    const uint32_t loop = level::GameMainLoop();
    unsigned char pending = 0;
    return loop && Read(loop + 0x209, &pending, 1) && pending;
}
// A stream handle's state (StreamManager handle table +0x5C/+0x60, nodes {handle, stream, next}; stream +0x24, 3 = loaded).
int StreamStatus(uint32_t handle) {
    uint32_t manager = 0, table = 0, buckets = 0, node = 0;
    if (!handle || !Read(base + 0x1C1C098, &manager, 4) || !manager || !Read(manager + 0x5C, &table, 4) || !Read(manager + 0x60, &buckets, 4) ||
        !table || !buckets || !Read(table + 4 * (handle % buckets), &node, 4)) return -1;
    for (unsigned hops = 0; node && hops < 64; ++hops) {
        uint32_t nodeHandle = 0, stream = 0, status = 0;
        if (!Read(node, &nodeHandle, 4)) return -1;
        if (nodeHandle == handle) return Read(node + 4, &stream, 4) && stream && Read(stream + 0x24, &status, 4) ? static_cast<int>(status) : -1;
        if (!Read(node + 8, &node, 4)) return -1;
    }
    return -1;
}
void NoteStartedLoad(int entry, uint32_t ghost) {
    for (unsigned i = 0; i < startedLoadCount; ++i) if (startedLoads[i].entry == entry) return;
    if (startedLoadCount < std::size(startedLoads)) startedLoads[startedLoadCount++] = StartedLoad{entry, ghost, GetTickCount()};
}
// Takes back every weapon load started here that is still in flight.
void CancelStartedLoads(const char* why) {
    uint32_t manager = 0, entries = 0, count = 0;
    const bool qm = Read(base + kQuartermaster, &manager, 4) && manager && Read(manager + 0x14, &entries, 4) && Read(manager + 0x18, &count, 4) && entries;
    for (unsigned i = 0; i < startedLoadCount; ++i) {
        const StartedLoad& l = startedLoads[i];
        uint32_t handle = 0;
        if (!qm || l.entry < 0 || static_cast<uint32_t>(l.entry) >= count || !Read(entries + l.entry * kGunEntryStride + kGunEntryStream, &handle, 4) || !handle)
            continue;
        const int status = StreamStatus(handle);
        if (status == 3) continue;   // loaded: its gun is the game's now, it goes with the level
        __try {
            reinterpret_cast<void(__thiscall*)(void*, int, int)>(base + kQmReleaseEntry)(reinterpret_cast<void*>(manager), l.entry, 1);
            InterlockedIncrement(&provisionCancelled);
            gate::Event("coop weapon_provision: load of Quartermaster entry %d (handle %X, status %d) taken back: %s", l.entry, handle, status, why);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&provisionCancelFaults);
        }
    }
    startedLoadCount = 0;
}
// Every frame: a level change, a level gone or a ghost gone ends the loads started here.
void WatchStartedLoads() {
    if (!startedLoadCount) return;
    if (LevelChangePending()) { CancelStartedLoads("a level change is pending"); return; }
    if (!inLevel || !levelSettled) { CancelStartedLoads("the level is not standing"); return; }
    for (unsigned i = 0; i < startedLoadCount; ++i) {
        uint32_t vtable = 0;
        if (!Read(startedLoads[i].ghost, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva) {
            CancelStartedLoads("the ghost it was for is gone");
            return;
        }
    }
}
// A gun the partner no longer carries leaves the ghost's slots the way the bots
// trim theirs: Gun::MoveFromInventoryToSafe (0x00DE0EB0, thiscall(gun), vtable
// +0x288) moves it into its owner's safe, clears the hand if it held it and
// deletes it. 13.10.: the partner's new seeker rifle was refused seven times, the
// ghost's four slots still held the guns he had swapped out.
constexpr uint32_t kGunMoveToSafeSlot = 0x288, kGunMoveToSafeRva = 0x9E0EB0;
volatile LONG ghostGunsTrimmed = 0, ghostGunTrimFaults = 0;
__declspec(noinline) bool MoveGunToSafe(uint32_t fn, uint32_t gun) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(gun)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool TrimGhostGuns(uint32_t ghost, const coop_loadout::Receiver& incoming, uint32_t machine) {
    uint32_t entries = 0, count = 0;
    if (!Read(ghost + 0xC08, &entries, 4) || !entries || !Read(ghost + 0xC0C, &count, 4) || count > 16) return false;
    auto reader = [](uint32_t address, void* buffer, size_t size) { return Read(address, buffer, size); };
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t ref = 0;
        if (!Read(entries + i * 12 + 4, &ref, 4) || ref < 0x10010 || ref > 0xFFFFFD00) continue;
        const uint32_t gun = ref - 0x10;
        const auto identity = coop_weapon::InspectIdentity(gun, static_cast<uint32_t>(base), reader);
        if (identity.kind != coop_weapon::Kind::Gun || !(identity.readable & 4)) continue;
        bool wanted = false;
        for (unsigned s = 0; s < incoming.slots && s < coop_loadout::kMaxSlots && !wanted; ++s)
            wanted = incoming.items[s].have && std::memcmp(incoming.items[s].guid, identity.proxyGuid, 16) == 0;
        if (wanted) continue;
        uint32_t fn = 0;
        if (!Read(identity.vtable + kGunMoveToSafeSlot, &fn, 4) || fn != base + kGunMoveToSafeRva) continue;
        const bool ok = MoveGunToSafe(fn, gun);
        InterlockedIncrement(ok ? &ghostGunsTrimmed : &ghostGunTrimFaults);
        gate::Event("coop weapon_provision: machine %u's ghost %08X no longer carries proxy %08X-%08X (vtable RVA %08X): out of its slots %s", machine, ghost,
                    identity.proxyGuid[0], identity.proxyGuid[3], identity.vtable - static_cast<uint32_t>(base), ok ? "done" : "FAULTED");
        return true;   // one change per poll
    }
    return false;
}
unsigned provisionSlot = 0;
void ProvisionGhostWeapons() {
    if (!provisionWeapons || !loadoutSync) return;
    WatchStartedLoads();
    if (!inLevel || !levelSettled || LevelChangePending()) { InterlockedIncrement(&provisionHeldBack); return; }
    // One partner (or host bot) per poll, round robin, so no single peer can
    // starve the rest and the eight-attempt budget below stays per partner.
    provisionSlot = (provisionSlot + 1) % kProvisionSlots;
    uint32_t machine = kNoMachine;
    const coop_loadout::Receiver* source = nullptr;
    if (provisionSlot < coop_peers::kMaxPeers) {
        const auto& partner = peerTable.peers[provisionSlot];
        if (!partner.used || partner.machine == kNoMachine) return;
        machine = partner.machine;
        source = &peerRuntime[provisionSlot].loadout;
    } else {
        if (IsHost()) return;   // the bots are the host's own
        const unsigned bot = provisionSlot - coop_peers::kMaxPeers;
        machine = 0xF0u + bot;
        source = &botLoadouts[bot];
    }
    const auto& incoming = *source;
    if (!incoming.complete) return;
    if (provisionGeneration[provisionSlot] != incoming.generation) {
        provisionGeneration[provisionSlot] = incoming.generation;
        provisionAttempts[provisionSlot] = 0;
    }
    const uint32_t ghost = GhostForMachine(machine);
    if (!ghost) { steadyGhost[provisionSlot] = 0; return; }
    // A ghost that has only just appeared is often about to go again (a restart, a checkpoint pick).
    // Every new body gets its own eight attempts: a respawned ghost starts empty-handed.
    const DWORD now = GetTickCount();
    bool realGuns = false;
    for (unsigned i = 0; i < incoming.slots && i < coop_loadout::kMaxSlots && !realGuns; ++i)
        realGuns = incoming.items[i].have && GhostWeaponState(ghost, incoming.items[i].guid) == GhostGun::Real;
    const bool rebuiltInPlace = steadyGhost[provisionSlot] == ghost && provisionArmed[provisionSlot] && !realGuns;
    if (steadyGhost[provisionSlot] != ghost || rebuiltInPlace) {
        InterlockedIncrement(rebuiltInPlace ? &provisionRebuiltInPlace : &provisionNewBodies);
        if (InterlockedIncrement(&provisionBodyLines) <= 40)
            gate::Event("coop_ghost_gun slot %u: %s body %08X (was %08X, %u attempt(s) spent), its guns are given again", provisionSlot,
                        rebuiltInPlace ? "rebuilt" : "new", ghost, steadyGhost[provisionSlot], provisionAttempts[provisionSlot]);
        steadyGhost[provisionSlot] = ghost;
        steadySince[provisionSlot] = now;
        provisionAttempts[provisionSlot] = 0;
        provisionArmed[provisionSlot] = false;
    }
    if (realGuns) provisionArmed[provisionSlot] = true;
    if (provisionAttempts[provisionSlot] >= 8) { InterlockedIncrement(&provisionBudgetStops); return; }   // stop rather than retry forever
    if (now - steadySince[provisionSlot] < kGhostResidentMs) return;
    const bool steadyForLoads = now - steadySince[provisionSlot] >= kGhostSteadyMs;
    if (TrimGhostGuns(ghost, incoming, machine)) return;
    uint32_t manager = 0;
    if (!Read(base + kQuartermaster, &manager, 4) || !manager) return;
    for (unsigned i = 0; i < incoming.slots && i < coop_loadout::kMaxSlots; ++i) {
        const auto& item = incoming.items[i];
        if (!item.have) continue;
        // Only a placeholder is upgraded: with no placeholder IssueGun skips
        // its ownership sequence, and a real gun is already there.
        const GhostGun state = GhostWeaponState(ghost, item.guid);
        if (state == GhostGun::Real) continue;
        if (state == GhostGun::None) {
            InterlockedIncrement(&provisionNoPlaceholder);
            // ProvisionCreate=1: a weapon the ghost never had has no GunProxy
            // placeholder, so IssueGun cannot complete. Bounded per generation
            // like the rest.
            if (!provisionCreate || provisionAttempts[provisionSlot] >= 8) continue;
            // Loaded: hand the ghost the gun itself (GiveGhostGun above).
            // Not loaded yet: RequisitionGun streams it in. Only then, since
            // for a loaded stream it spawns a gun it cannot place and leaves
            // it lying (refs=11 and eleven stray guns for one purchase, 23.09.).
            {
                uint32_t stream = 0, refs = 0;
                if (FindQuartermasterEntry(item.guid, stream, refs) < 0) continue;
                if (stream) {
                    // A stream still loading costs no attempt; the next poll asks again.
                    if (GiveGhostGun(ghost, item.guid) < 0) ++provisionAttempts[provisionSlot];
                    return;   // one weapon per poll
                }
            }
            if (!steadyForLoads) continue;   // a stream load waits for a body that stays
            ++provisionAttempts[provisionSlot];
            InterlockedIncrement(&provisionCreateTries);
            {
                uint32_t stream = 0, refs = 0;
                const int entry = FindQuartermasterEntry(item.guid, stream, refs);
                // The form RequisitionGun queues for the ghost is taken back at once: when the
                // load finished, IssueGun spawned the gun at the origin, found no placeholder
                // on the ghost and left it there (13.10.: guns floating at 0,0,0 on a necro
                // client). The load goes on and GiveGhostGun hands the gun over once resident.
                uint32_t formsBefore = 0, formsQueued = 0, formsAfter = 0;
                bool asked = false, cancelled = false;
                Read(manager + kQuartermasterForms, &formsBefore, 4);
                __try {
                    using Requisition = bool(__thiscall*)(void*, uint32_t, const void*, uint32_t);
                    asked = reinterpret_cast<Requisition>(base + kRequisitionGun)(
                        reinterpret_cast<void*>(manager), ghost, item.guid, 0u);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    InterlockedIncrement(&provisionFaults);
                }
                Read(manager + kQuartermasterForms, &formsQueued, 4);
                if (formsQueued > formsBefore) {
                    __try {
                        reinterpret_cast<void(__thiscall*)(void*, uint32_t, const void*)>(base + kCancelRequisitionGun)(
                            reinterpret_cast<void*>(manager), ghost, item.guid);
                        cancelled = true;
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        InterlockedIncrement(&provisionCreateCancelFaults);
                    }
                }
                Read(manager + kQuartermasterForms, &formsAfter, 4);
                if (cancelled && formsAfter < formsQueued) InterlockedIncrement(&provisionCreateCancelled);
                if (InterlockedIncrement(&provisionCreateEvents) <= 24)
                    gate::Event("coop_ghost_gun create load: slot %u ghost %08X entry %d stream %08X requisition=%d forms %u -> %u -> %u (%s)",
                                provisionSlot, ghost, entry, stream, asked ? 1 : 0, formsBefore, formsQueued, formsAfter,
                                formsQueued <= formsBefore ? "no form queued" : formsAfter < formsQueued ? "form taken back" : "CANCEL MISSED: a stray gun follows");
                if (entry >= 0 && !stream) NoteStartedLoad(entry, ghost);   // a load started here: taken back if the level ends first
            }
            return;   // one request per poll; stream load is asynchronous
        }
        uint32_t stream = 0, refs = 0;
        const int entry = FindQuartermasterEntry(item.guid, stream, refs);
        // RequisitionGun starts the stream load itself when the entry has none,
        // within the Quartermaster's own budget, the same path the game's
        // multiplayer takes. Insisting on a loaded stream meant a weapon the
        // host never held could never appear on a ghost.
        if (entry < 0) { InterlockedIncrement(&provisionRefused); continue; }
        if (!stream && !steadyForLoads) continue;
        if (!stream) { InterlockedIncrement(&provisionLoads); NoteStartedLoad(entry, ghost); }
        ++provisionAttempts[provisionSlot];
        InterlockedIncrement(&provisionTries);
        __try {
            using Requisition = bool(__thiscall*)(void*, uint32_t, const void*, uint32_t);
            if (reinterpret_cast<Requisition>(base + kRequisitionGun)(
                    reinterpret_cast<void*>(manager), ghost, item.guid, 0u))
                InterlockedIncrement(&provisionAccepted);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&provisionFaults);
        }
        return;  // one request per poll; the load is asynchronous
    }
}

// Mirror stage: when the peer died, put the local player through the same
// native campaign death, so both players restart at the checkpoint together.
//
// Verified live on 2026-09-16 (death_flags=001500FC): bit 13, the multiplayer
// flag, is CLEAR in the campaign, so ProcessDeath takes the campaign branch that
// ends in the loading/checkpoint path. The captured arguments were
// (001AF520, 0005EB10, 3C83126F): argument 0 is a pointer to damage data on the
// stack and argument 2 is the frame delta (0x3C83126F is ~0.0167).
//
// Argument 0 is dereferenced on one guarded branch (test byte [arg0+0x90],2),
// so a zeroed buffer makes that test false rather than reading wild memory.
// The call goes through the hook's trampoline to the original function, so the
// hook is not re-entered and a mirrored death can never be relayed back, by
// construction rather than by a flag.
bool mirrorDeath = false;
volatile LONG deathMirrored = 0, deathMirrorFaults = 0, pendingPeerDeath = 0, deathsIgnoredStale = 0;
DWORD restartBookkeepingAt = 0;   // checkpoint::RestartBookkeeping: the world was just rebuilt

void TriggerLocalDeath() {
    if (!InterlockedCompareExchange(&pendingPeerDeath, 0, 0)) return;
    if (!mirrorDeath || !deathSync || !deathHook) { InterlockedExchange(&pendingPeerDeath, 0); return; }
    const uint32_t player = LocalCampaignPlayer();
    if (!player) {
        // A necromorph body has no Player death path; drop the partner's death
        // rather than replaying it on Isaac the moment he is respawned.
        if (LocalSlotZeroEntity()) InterlockedExchange(&pendingPeerDeath, 0);
        return;  // otherwise no local player yet: keep the request pending
    }
    InterlockedExchange(&pendingPeerDeath, 0);
    alignas(16) unsigned char damage[0x200]{};
    InterlockedIncrement(&deathMirrored);
    __try {
        deathHook.thiscall<uint32_t>(reinterpret_cast<void*>(player),
                                     reinterpret_cast<uint32_t>(damage), 0u,
                                     0x3C83126Fu);  // the observed frame delta
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&deathMirrorFaults);
    }
}

namespace bot { void NoteDeathRefused(); }   // runtime_coop_bot.inc
namespace respawn { bool TakeDeath(uint32_t player, uint32_t damage); }   // runtime_coop_respawn.inc
namespace versus { bool NativeDeath(uint32_t player); void NoteDeath(uint32_t player); }   // runtime_coop_versus.inc
uint32_t __fastcall OnProcessDeath(uint32_t ecx, uint32_t, uint32_t a0, uint32_t a1, uint32_t a2) {
    // The companion bot never dies: its PlayerDeathSM would restart the level for everybody.
    if (IsBotEntity(ecx)) { bot::NoteDeathRefused(); return 0; }
    // Versus with the MP bits: the native MP branch respawns him; no restart, no death packet.
    if (versus::NativeDeath(ecx)) {
        versus::NoteDeath(ecx);
        return deathHook.thiscall<uint32_t>(reinterpret_cast<void*>(ecx), a0, a1, a2);
    }
    // On an MP map with lives left the level stays and Isaac respawns; the
    // partners hear nothing of it (no death packet, no mirrored restart).
    if (respawn::TakeDeath(ecx, a0)) return 0;
    // Story co-op with another human alive ([Spectate]): he watches until the next checkpoint.
    if (spectate::TakeDeath(ecx, a0)) return 0;
    if (deathSync && ecx) {
        if (ecx == LocalSlotZeroEntity()) {   // the local body, Isaac or necro: the partners get the death packet either way
            uint32_t flags = 0;
            Read(ecx + 0x3D8, &flags, 4);
            InterlockedExchange(&deathLastFlags, static_cast<LONG>(flags));
            InterlockedExchange(&deathLastArgs[0], static_cast<LONG>(a0));
            InterlockedExchange(&deathLastArgs[1], static_cast<LONG>(a1));
            InterlockedExchange(&deathLastArgs[2], static_cast<LONG>(a2));
            InterlockedIncrement(&deathLocalSeen);
            {
                // Who has to confirm it. Taken at the moment of death, so a
                // partner who joins afterwards is not waited for.
                uint32_t waitFor[coop_death::kMaxAckPeers]{};
                unsigned count = 0;
                for (const auto& peer : peerTable.peers) {
                    if (!peer.used || !peer.token) continue;
                    if (count < coop_death::kMaxAckPeers) waitFor[count++] = peer.token;
                }
                deathSender.Record(GetTickCount(), waitFor, count);
            }
        } else {
            InterlockedIncrement(&deathOtherSeen);
        }
    }
    return deathHook.thiscall<uint32_t>(reinterpret_cast<void*>(ecx), a0, a1, a2);
}

namespace mirror { uint32_t ObjectByGuid(const uint32_t* guid); }   // runtime_coop_mirror.inc
namespace gate { extern bool trace; void Event(const char* format, ...); }   // runtime_coop_gate.inc
namespace mirror { bool Included(uint32_t hash); bool StoryActorCue(uint32_t hash); bool RoutesStoryScene(uint32_t hash, DWORD now); bool HoldNestedStory(uint32_t hash, const void* message, int flags, DWORD now); }
namespace loadsync { bool SceneSyncCapable(); uint32_t SceneMapHash(); uint32_t SceneGeneration(); }
namespace nameplate { bool Dead(uint32_t player); }   // runtime_coop_mirror.inc
namespace necro { void OnBodyHealth(uint32_t entity, float after); }   // runtime_coop_necro.inc: the necro's death is the revert
// Link-aware time windows. Every "fresh", "grace" and "arrives within" in the
// co-op modules was tuned on one machine or a LAN, where a packet takes a
// millisecond. Across the internet one takes half the round trip plus the
// spread, and a window that is shorter than that turns a slow link into a
// broken feature (a leader's aim "stale" after 300 ms at 400 ms ping). A
// window keeps its LAN value and stretches with the worst partner link.
namespace netclock {
constexpr DWORD kPeerFreshMs = 10000;
// Worst smoothed round trip and jitter among partners heard from recently.
uint32_t Rtt() {
    const DWORD now = GetTickCount();
    uint32_t worst = 0;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        if (!peer.used || !peer.token || now - peer.lastSeen > kPeerFreshMs) continue;
        const uint32_t rtt = peerRuntime[s].link.smoothedRtt;
        if (rtt > worst) worst = rtt;
    }
    return worst > 2000 ? 2000 : worst;
}
uint32_t Jitter() {
    const DWORD now = GetTickCount();
    uint32_t worst = 0;
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        if (!peer.used || !peer.token || now - peer.lastSeen > kPeerFreshMs) continue;
        const uint32_t jitter = peerRuntime[s].link.jitter;
        if (jitter > worst) worst = jitter;
    }
    return worst > 1000 ? 1000 : worst;
}
// How late a packet sent "now" by a partner may still reasonably arrive.
DWORD OneWay() { return Rtt() / 2 + 2 * Jitter(); }
// base on a LAN; base plus two round trips plus four jitters on a slow link,
// never more than base + cap.
DWORD Window(DWORD lan, DWORD cap = 3000) {
    const DWORD extra = 2 * Rtt() + 4 * Jitter();
    return lan + (extra > cap ? cap : extra);
}
}  // namespace netclock
namespace npcs { struct LocalClass; const LocalClass* LocalByVtable(uint32_t vtable); const LocalClass* LocalByDescriptor(uint32_t descriptor); bool LocalNeutral(uint32_t vtable); }   // runtime_coop_npcs.inc
namespace bot { bool ApplyKongStasis(uint32_t entity, float factor, float duration); }
#include "runtime_coop_hits.inc"
namespace mirror { extern bool actionOutcomes; }
#include "runtime_coop_storyobjects.inc"
namespace notice { bool Show(const char* text); }
namespace streams { void OnActivation(uint32_t hash, uint32_t flags); void OnLoadComplete(uint32_t source); }   // runtime_coop_streams.inc
namespace ghosttrigger { bool GhostOnlyTouch(uint32_t trigger, DWORD now); bool BotOnlyTouch(uint32_t trigger, DWORD now); extern bool blockBotLevelChange, cullGuard; }   // runtime_coop_ghosttrigger.inc
namespace catchup { bool PullTo(const float* at, const char* why); }            // runtime_coop_catchup.inc
namespace catchup { const char* StageChurchIsaac(uint32_t player, const float* at); }
namespace bot { void WritePad(); bool TakesDamage(uint32_t entity, const void* damage); void NoteDeathRefused(); void OnRestart(); bool HidesInThisScene();
                bool RefuseHitOnHuman(uint32_t target, const void* damage, const char* where); void SetNoDamageToHumans(bool on); }   // runtime_coop_bot.inc
#include "runtime_coop_spawnwin.inc"
namespace arena { bool RunningNow(); bool BlocksHallucination(uint32_t hash); }   // runtime_coop_arena.inc
#include "runtime_coop_gate.inc"
namespace overlay { void ShowHint(const char* what, const char* why, const char* fix, DWORD ms);
                    bool ShowPage(const char* title, const char* const* lines, int count, DWORD ms); }   // runtime_coop_overlay.inc
namespace lobby { int SurvivalCheck(const char* map, int mode, char (&why)[240], char (&fix)[200]); }   // runtime_coop_lobby.inc
#include "runtime_coop_notice.inc"
namespace mirror { void OnLocalDirect(uint32_t hash, const void* message, uint32_t target); void OnLocalCall(const uint32_t* guid, uint32_t callId, uint32_t a0, uint32_t a1, const char* what, bool anyMachine, bool journal); }
#include "runtime_coop_ghosttrigger.inc"
#include "runtime_coop_guidfix.inc"
namespace level { uint32_t GameMainLoop(); }   // defined in runtime_coop_level.inc
namespace mirror { void OnRestart(); }         // runtime_coop_mirror.inc
namespace necro { void OnRestart(); }          // runtime_coop_necro.inc: Isaac back in slot 0 before the restart tears down
namespace worldcalls { void OnRestart(); bool DoorSide(uint32_t door, uint32_t actor, bool& out); extern float doorReach; extern bool doorSideOk; }     // runtime_coop_worldcalls.inc
namespace cutscene { void OnWorldReset(); bool Following(); bool FollowsSlot(unsigned slot); bool InFollowedChain(DWORD now); bool Leading(); DWORD FollowingSince(); void NoteSuppressed(uint32_t hash); bool SequenceRunningNow(); void NoteReplay(unsigned author, bool started, bool direct, DWORD now); void NotePartnerScene(unsigned slot, uint32_t sequenceAgoMs, DWORD now); bool PartnerInScene(unsigned slot, DWORD now); }   // runtime_coop_cutscene.inc
#include "runtime_coop_checkpoint.inc"
extern bool fromEnvironmentRole;   // below: DS2COOP set the role at start
#include "runtime_coop_level.inc"
namespace mpmap { bool InVisit(); constexpr DWORD kGuardMs = 10000; }
#include "runtime_coop_scenekind.inc"
#include "runtime_coop_teleport.inc"
#include "runtime_coop_streams.inc"
namespace worldcalls { bool ReplayCall(uint32_t callId, const uint32_t* guid, const uint32_t* args, bool fromJournal, unsigned authorSlot); }   // runtime_coop_worldcalls.inc
#include "runtime_coop_mirror.inc"
#include "runtime_coop_lights.inc"
#include "runtime_coop_worldcalls.inc"
#include "runtime_coop_cutscene.inc"
#include "runtime_coop_scenepartner.inc"
namespace necro { void OnWorldClose(); }   // runtime_coop_necro.inc
namespace necro {
constexpr uint32_t kPoolMagic = 0x4C4F504E, kPoolVersion = 1;   // 'NPOL': 1 = load the necro pool, 2 = it is loaded here
struct PoolPacket { uint32_t magic, version, token, kind; };
static_assert(sizeof(PoolPacket) == 16, "Unexpected necro pool wire layout");
void OnPoolPacket(const PoolPacket& p, unsigned slot);
}
#include "runtime_coop_persistguard.inc"
#include "runtime_coop_tk.inc"
namespace mpmap { bool HoldStartForEarlySpawn(DWORD now); }   // runtime_coop_mpmap.inc
#include "runtime_coop_loadsync.inc"
#include "runtime_coop_ghostclock.inc"
#include "runtime_coop_crashdump.inc"
#include "runtime_coop_crashguards.inc"
#include "runtime_coop_rigcolor.inc"
#include "runtime_coop_fxcolor.inc"
#include "runtime_coop_pause.inc"
namespace necro { bool SuicideRow(); bool SuicideAvailable(); bool CommitSuicide(); }   // runtime_coop_necro.inc
#include "runtime_coop_pausemenu.inc"
#include "runtime_coop_focus.inc"
#include "runtime_coop_removals.inc"
#include "runtime_coop_restsync.inc"
#include "runtime_coop_regen.inc"
#include "runtime_coop_drops.inc"
#include "runtime_coop_reghost.inc"
namespace lifecycle { void PartnerLastPhase(unsigned machine, char* out, size_t size); }   // runtime_coop_lifecycle.inc
#include "runtime_coop_disconnect.inc"
#include "runtime_coop_aimguard.inc"
#include "runtime_coop_netrate.inc"
#include "runtime_coop_helmet.inc"
#include "runtime_coop_authority.inc"
#include "runtime_coop_configcheck.inc"

// Reads the local player transform the way the placement readout does
// (Player+0xC0 position, +0xB0 forward row) and sends it to the peer; drains
// whatever the partner sent. Called from Poll on the game thread.
// Packets are told apart by size alone.
constexpr size_t kPacketSizes[] = {
    sizeof(PlayerState), sizeof(checkpoint::Packet), sizeof(coop_death::Ack), sizeof(coop_death::Packet), sizeof(coop_gate_wire::Packet),
    sizeof(coop_hit::Packet), sizeof(coop_loadout::Packet), sizeof(coop_peer::Packet), sizeof(coop_ping::Packet), sizeof(coop_suit::Packet),
    sizeof(coop_template::Packet), sizeof(level::Packet), sizeof(mirror::Packet), sizeof(streams::Packet), sizeof(streams::vis::Packet),
    sizeof(tk::Packet), sizeof(tk::PosePacket), sizeof(loadsync::Packet), sizeof(removals::Packet), sizeof(restsync::Packet), sizeof(regen::Packet),
    sizeof(coop_loadout::BotPacket), sizeof(lights::Packet)};
namespace mesh { constexpr int kPacketBytes = 24 + 16 * static_cast<int>(coop_peers::kMaxPeers - 1); bool OnBytes(const void* data, int size, unsigned slot, DWORD now); void Tick(DWORD now); void OnPartnersReset(); }   // runtime_coop_mesh.inc
constexpr bool PacketSizesDistinct() {
    for (size_t i = 0; i < std::size(kPacketSizes); ++i)
        for (size_t j = i + 1; j < std::size(kPacketSizes); ++j)
            if (kPacketSizes[i] == kPacketSizes[j]) return false;
    return true;
}
static_assert(PacketSizesDistinct(), "Two co-op packets share a size");
volatile LONG receiveBacklogs = 0;
void ExchangePlayerState() {
    if (dataSock == INVALID_SOCKET) return;
    const uint32_t player = LocalCampaignPlayer();
    const DWORD now = GetTickCount();
    // Receive first, so the host learns the partner address before replying.
    sockaddr_in from{};
    unsigned drained = 0;
    for (; drained < 16; ++drained) {
        unsigned char buffer[512]{};   // the Versus roster is 272 bytes, the level announcement 168; a larger datagram fails as WSAEMSGSIZE
        int fromLen = sizeof(from);
        const int got = ::recvfrom(dataSock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (got <= 0) {
            // An ICMP "port unreachable" for an earlier send (a partner not up yet) and an
            // oversized datagram each fail one call; the datagrams behind them are still queued.
            const int error = got < 0 ? WSAGetLastError() : 0;
            if (error == WSAECONNRESET || error == WSAEMSGSIZE) continue;
            break;
        }
        // A machine that was given a peer address only ever talks to peers it
        // was told about; one that was not (the host) learns them from their
        // first packet.
        coop_peers::Peer* source = nullptr;
        if (from.sin_family == AF_INET)
            source = peerTable.ReceiveSource(from.sin_addr.s_addr, from.sin_port, now, IsHost(),
                                             peerReadiness.token, buffer, got);
        if (!source) { InterlockedIncrement(&dataRejected); continue; }
        const unsigned slot = PeerSlot(source);
        const bool fromHostEndpoint = !IsHost() && peerPort &&
            source->Is(peerIp, htons(static_cast<u_short>(peerPort + kDataPortOffset)));
        bool accepted = false;
        uint32_t rewardMagic = 0, capsuleMagic = 0, leadMagic = 0, worldMagic = 0, poolMagic = 0, aimMagic = 0, survMagic = 0, voiceMagic = 0, voiceEventMagic = 0;
        if (got == static_cast<int>(sizeof(survival::RewardWire))) std::memcpy(&rewardMagic, buffer, 4);
        if (got == static_cast<int>(sizeof(survival_capsules::Snapshot))) std::memcpy(&capsuleMagic, buffer, 4);
        if (got == static_cast<int>(sizeof(coop_voices::Packet))) std::memcpy(&voiceMagic, buffer, 4);
        if (got == 12) std::memcpy(&voiceEventMagic, buffer, 4);   // also the role packet's magic
        if (got == static_cast<int>(sizeof(survnet::Packet))) std::memcpy(&survMagic, buffer, 4);
        if (got == static_cast<int>(sizeof(cutscene::leadaim::Packet))) std::memcpy(&aimMagic, buffer, 4);
        if (got == static_cast<int>(sizeof(cutscene::leadinput::Packet))) std::memcpy(&leadMagic, buffer, 4);
        if (got == static_cast<int>(sizeof(hits::WorldPacket)) || got == static_cast<int>(sizeof(hits::terminalprop::Packet))) std::memcpy(&worldMagic, buffer, 4);
        if (got == static_cast<int>(sizeof(necro::PoolPacket))) std::memcpy(&poolMagic, buffer, 4);
        if (story_objects::Receive(buffer, got, slot)) {
            accepted = true;
        } else if (got == mesh::kPacketBytes && fromHostEndpoint && mesh::OnBytes(buffer, got, slot, now)) {
            accepted = true;   // the host's session table and seed (told apart by the magic)
        } else if (rewardMagic == survival::kRewardMagic) {
            survival::RewardWire reward{};
            std::memcpy(&reward, buffer, sizeof(reward));
            accepted = survnet::ReceiveReward(reward, slot, fromHostEndpoint);
        } else if (capsuleMagic == survival_capsules::kMagic && fromHostEndpoint) {
            survival_capsules::Snapshot snapshot{};
            std::memcpy(&snapshot, buffer, sizeof(snapshot));
            if (snapshot.token && source->token == snapshot.token) {
                AcquireSRWLockExclusive(&survnet::lock);
                accepted = survnet::remoteCapsules.Receive(snapshot);
                if (accepted) survnet::capsuleTail = survnet::capsuleHead;
                ReleaseSRWLockExclusive(&survnet::lock);
            }
        } else if (survMagic == survnet::kMagic) {
            survnet::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = (packet.kind == 14 || (fromHostEndpoint && packet.token && source->token == packet.token)) && survnet::OnPacket(packet);
        } else if (aimMagic == cutscene::leadaim::kMagic) {
            cutscene::leadaim::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = cutscene::installed && packet.version == cutscene::leadaim::kVersion && KnownPeerToken(packet.token);
            if (accepted) {
                cutscene::OnAimPacket(packet, SlotForToken(packet.token, slot, static_cast<uint32_t>(now)));
                RelayToOthers(buffer, got, slot);
            }
        } else if (poolMagic == necro::kPoolMagic) {
            necro::PoolPacket packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = packet.version == necro::kPoolVersion && KnownPeerToken(packet.token);
            if (accepted) {
                necro::OnPoolPacket(packet, SlotForToken(packet.token, slot, static_cast<uint32_t>(now)));
                RelayToOthers(buffer, got, slot);
            }
        } else if (got == sizeof(hits::terminalprop::Packet) && worldMagic == hits::terminalprop::kMagic) {
            hits::terminalprop::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            const auto* owner = peerTable.FindByToken(packet.hit.token);
            accepted = owner && (owner == source || fromHostEndpoint) && hits::terminalprop::Receive(packet, PeerSlot(owner));
            if (accepted) RelayToOthers(buffer, got, slot);
        } else if (got == sizeof(hits::WorldPacket) && worldMagic == hits::kWorldMagic) {
            hits::WorldPacket packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = hits::enabled && packet.version == hits::kWorldVersion && KnownPeerToken(packet.token) &&
                       (source->token == packet.token || fromHostEndpoint);
            if (accepted) {
                hits::OnWorldPacket(packet);
                RelayToOthers(buffer, got, slot);
            }
        } else if (leadMagic == cutscene::leadinput::kMagic) {
            cutscene::leadinput::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = cutscene::installed && packet.version == cutscene::leadinput::kVersion && KnownPeerToken(packet.token);
            if (accepted) {
                cutscene::OnInputPacket(packet, SlotForToken(packet.token, slot, static_cast<uint32_t>(now)));
                RelayToOthers(buffer, got, slot);
            }
        } else if (voiceMagic == coop_voices::kMagic) {
            // the voice packs' picks; told apart by the magic, whatever else has this size
            coop_voices::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = voices::OnPacket(packet, !IsHost() && packet.token == peerReadiness.peerToken);
            if (accepted && IsHost()) RelayToOthers(buffer, got, slot);
        } else if (voiceEventMagic == roles::kMagic && roles::OnBytes(buffer, got, slot, now)) {
            accepted = true;   // a partner says whether he plays a necro
            if (IsHost()) RelayToOthers(buffer, got, slot);
        } else if (voiceEventMagic == 0x45584F56u && voices::OnEventPacket(buffer, got)) {
            accepted = true;   // a partner's death or reload, for his voice pack
            if (IsHost()) RelayToOthers(buffer, got, slot);
        } else if (voiceEventMagic == 0x52475542u && bugreport::OnBytes(buffer, got)) {
            accepted = true;   // a partner pressed the bug key: this machine saves its own report
            if (IsHost()) RelayToOthers(buffer, got, slot);
        } else if (got == sizeof(cutscene::sceneaim::Packet) && cutscene::sceneaim::Receive(buffer, slot, now)) {
            accepted = true;
            RelayToOthers(buffer, got, slot);
        } else if (got == 32 && uselock::OnBytes(buffer, got, slot, now, fromHostEndpoint)) {
            accepted = true;   // a partner at a hacking panel, node door or eye poke machine, or leaving it
            if (IsHost()) RelayToOthers(buffer, got, slot);
        } else if (got == static_cast<int>(sizeof(coop_mpnative::Packet)) && mpnative::OnBytes(buffer, got)) {
            accepted = true;   // a Versus roster entry or the host's roster
        } else if (got == static_cast<int>(sizeof(coop_versus::Packet)) && versus::OnBytes(buffer, got)) {
            accepted = true;   // a Versus match event (scoring, rounds, respawn points)
        } else if (got == sizeof(hits::KongStasis) && hits::OnKongStasis(buffer, slot)) {
            accepted = true;
            RelayToOthers(buffer, got, slot);
        } else if (got == hits::kKongBytes && hits::OnKongBytes(buffer, got)) {
            accepted = true;   // a partner's Tormentor arm
            if (IsHost()) RelayToOthers(buffer, got, slot);
        } else if (got == botnet::kBotSuitBytes && botnet::OnBotSuitBytes(buffer, got)) {
            accepted = true;   // the suit one of the host's bots wears
        } else if (got == 132 && nameplate::OnNameBytes(buffer, got)) {
            accepted = true;
            RelayToOthers(buffer, got, slot);
        } else if (got == 18 && respawn::OnLifeBytes(buffer, got, static_cast<uint32_t>(now))) {
            accepted = true;
            RelayToOthers(buffer, got, slot);
        } else if (got == sizeof(coop_peer::Packet)) {
            coop_peer::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            // Identity first: this is the packet that says who is on the other
            // end, and every other packet is routed by the token it carries. Only
            // a readiness packet: a refused 32-byte uselock or bot suit packet
            // lands here too and gave the slot the token at its offset 8 (audit 16.10.).
            // A rejected heartbeat must not change the identity used by other protocols.
            // Into this peer's own state. Every partner has its own handshake,
            // and none of them can shut another out.
            accepted = packet.magic == coop_peer::kMagic && peerRuntime[slot].readiness.Accept(packet, now);
            if (accepted) {
                if (source->token != packet.token) source->machine = 0xFFFFFFFFu;
                peerTable.Identify(*source, packet.token, 0xFFFFFFFFu);
                InterlockedIncrement(&readinessReceived);
                MirrorPrimaryReadiness();
            }
        } else if (got == sizeof(coop_loadout::Packet)) {
            coop_loadout::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = loadoutSync && peerTable.Count() && coop_loadout::Valid(packet);
            if (accepted) {
                const unsigned owner = SlotForToken(packet.token, slot, static_cast<uint32_t>(now));
                // Accept counts a COMPLETE loadout, not every slot packet.
                if (peerRuntime[owner].loadout.Accept(packet, packet.token, now))
                    InterlockedIncrement(&loadoutReceived);
                if (owner == FirstPeerSlot()) loadoutReceiver = peerRuntime[owner].loadout;
                RelayToOthers(buffer, got, slot);
            }
        } else if (got == sizeof(coop_loadout::BotPacket)) {
            coop_loadout::BotPacket packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = loadoutSync && !IsHost() && coop_loadout::ValidBot(packet) && packet.bot < kMaxBots;
            if (!accepted) InterlockedIncrement(&botLoadoutRejected);
            else if (botLoadouts[packet.bot].Accept(coop_loadout::FromBot(packet), 0, now)) InterlockedIncrement(&botLoadoutReceived);
        } else if (got == sizeof(coop_suit::Packet)) {
            coop_suit::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = suitSync && peerTable.Count() && coop_suit::Valid(packet);
            if (accepted) {
                const unsigned owner = SlotForToken(packet.token, slot, static_cast<uint32_t>(now));
                if (peerRuntime[owner].suit.Accept(packet, packet.token, now))
                    InterlockedIncrement(&suitReceived);
                if (owner == FirstPeerSlot()) suitReceiver = peerRuntime[owner].suit;
                RelayToOthers(buffer, got, slot);
            }
        } else if (got == sizeof(coop_death::Packet)) {
            coop_death::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = deathSync && peerTable.Count() && coop_death::Valid(packet);
            // Death is per player too, but its consequence is not: whoever dies,
            // everybody restarts at the checkpoint, so the mirror below stays a
            // single request no matter which partner it came from.
            const unsigned deathOwner = accepted
                ? SlotForToken(packet.token, slot, static_cast<uint32_t>(now)) : slot;
            if (accepted) RelayToOthers(buffer, got, slot);
            // Retransmissions of the same epoch stay accepted but are not counted
            // again, so one death reports once.
            if (accepted) {
                // Confirm every copy, not just the first: the sender keeps
                // retransmitting until it hears back, so an ack only for the
                // one that happened to arrive first would leave it shouting.
                const auto ack = coop_death::MakeAck(packet, peerReadiness.token);
                if (::sendto(dataSock, reinterpret_cast<const char*>(&ack), sizeof(ack), 0,
                             reinterpret_cast<sockaddr*>(&from), sizeof(from)) == sizeof(ack))
                    InterlockedIncrement(&deathAcksOut);
            }
            if (accepted && peerRuntime[deathOwner].death.Accept(packet, packet.token, now)) {
                deathReceiver = peerRuntime[deathOwner].death;
                InterlockedIncrement(&deathReceived);
                // Acted on from Poll, on the game thread; Accept already folds
                // the retransmissions, so this is one request per real death.
                // Not in a level, or within 6 s of our own restart, it is a copy
                // that belongs to the world that went: played now it would kill
                // the new Isaac (audit 16.10.). The ack above still went out.
                const DWORD at = static_cast<DWORD>(now);
                if (!inLevel || (restartBookkeepingAt && TicksSince(at, restartBookkeepingAt) < 6000)) {
                    InterlockedIncrement(&deathsIgnoredStale);
                    gate::Event("coop death of %08X ignored: %s", packet.token, inLevel ? "within 6 s of our own restart" : "not in a level");
                } else {
                    InterlockedExchange(&pendingPeerDeath, 1);
                }
            }
        } else if (got == sizeof(coop_death::Ack)) {
            coop_death::Ack ack{};
            std::memcpy(&ack, buffer, sizeof(ack));
            accepted = deathSync && coop_death::ValidAck(ack);
            if (accepted && deathSender.Acknowledge(ack))
                InterlockedIncrement(&deathAcksIn);
            else if (accepted && ack.owner && ack.owner != peerReadiness.token)
                RelayToOthers(buffer, got, slot);   // a client's confirmation of another client's death
        } else if (got == sizeof(mirror::Packet)) {
            uint32_t magic = 0;
            std::memcpy(&magic, buffer, sizeof(magic));
            if (magic == mirror::agreement::kMagic) {
                mirror::agreement::Packet packet{};
                std::memcpy(&packet, buffer, sizeof(packet));
                accepted = mirror::installed && mirror::agreement::OnPacket(packet, slot, GetTickCount());
            } else if (magic == mirror::ghostseq::kSceneMagic) {
                mirror::ghostseq::ScenePacket packet{};
                std::memcpy(&packet, buffer, sizeof(packet));
                accepted = mirror::installed && mirror::ghostseq::OnScenePacket(packet, slot, GetTickCount());
            } else {
                mirror::Packet packet{};
                std::memcpy(&packet, buffer, sizeof(packet));
                const bool hostOnly = (packet.flags & (mirror::kFlagRelayed | mirror::kFlagJournal)) != 0;
                accepted = mirror::installed && mirror::Valid(packet) &&
                           (hostOnly ? fromHostEndpoint : packet.token == source->token);
                if (accepted) mirror::OnPacket(packet, slot);
            }
        } else if (got == sizeof(level::Packet)) {
            level::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = level::installed && level::Valid(packet) && packet.token == source->token &&
                       ((packet.flags & level::kFlagInMenu) || fromHostEndpoint);
            if (accepted) level::OnPacket(packet);
        } else if (got == sizeof(tk::PosePacket)) {
            tk::PosePacket packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = tk::installed && tk::ValidPose(packet) && packet.token == source->token;
            if (accepted) tk::OnPose(packet, slot);
        } else if (got == sizeof(regen::Packet)) {
            regen::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = regen::installed && regen::Valid(packet) && KnownPeerToken(packet.token);
            if (accepted) regen::OnPacket(packet);
        } else if (got == sizeof(restsync::Packet)) {
            restsync::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = restsync::installed && restsync::Valid(packet) && KnownPeerToken(packet.token);
            if (accepted) restsync::OnPacket(packet);
        } else if (got == sizeof(removals::Packet)) {
            removals::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = removals::installed && removals::Valid(packet) && KnownPeerToken(packet.token);
            if (accepted) removals::OnPacket(packet);
        } else if (got == sizeof(loadsync::Packet)) {
            loadsync::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = loadsync::installed && loadsync::Valid(packet) && packet.token == source->token;
            if (accepted) loadsync::OnPacket(packet);
        } else if (got == sizeof(tk::Packet)) {
            tk::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = tk::installed && tk::Valid(packet) && packet.token == source->token;
            if (accepted) tk::OnPacket(packet);
        } else if (got == sizeof(lights::Packet)) {
            lights::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = lights::installed && lights::Valid(packet) && KnownPeerToken(packet.token);
            if (accepted) lights::OnPacket(packet, slot);
        } else if (got == sizeof(streams::vis::Packet)) {
            streams::vis::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = streams::installed && streams::vis::Valid(packet) && KnownPeerToken(packet.token);
            if (accepted) streams::OnVisPacket(packet, slot);
        } else if (got == sizeof(streams::Packet)) {
            streams::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = streams::installed && streams::Valid(packet) && KnownPeerToken(packet.token);
            if (accepted) streams::OnPacket(packet, slot);
        } else if (got == sizeof(checkpoint::Packet)) {
            checkpoint::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = checkpoint::installed && checkpoint::Valid(packet) && packet.token == source->token &&
                       (IsHost() ? (packet.flags == checkpoint::kFlagRestartRequest || packet.flags == checkpoint::kFlagReached)
                                 : (fromHostEndpoint && !packet.flags));
            if (accepted) checkpoint::OnPacket(packet, slot);
        } else if (got == sizeof(coop_gate_wire::Packet)) {
            coop_gate_wire::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            const bool relayed = (packet.flags & coop_gate_wire::kFlagRelayed) != 0;
            accepted = gate::installed && coop_gate_wire::Valid(packet) &&
                       (relayed ? fromHostEndpoint : packet.token == source->token);
            if (accepted) gate::OnVote(packet, slot, now);
        } else if (got == sizeof(coop_hit::Packet)) {
            coop_hit::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = hits::enabled && peerTable.Count() && coop_hit::Valid(packet);
            // A world hit (a button, Nicole) counts on every machine: the host
            // passes every copy of a client's on, or the other clients never
            // see it. They fold the copies like the sender's own repeats.
            if (accepted && (packet.flags & coop_hit::kFlagWorld)) RelayToOthers(buffer, got, slot);
            if (accepted && hits::receiver.Accept(packet, KnownPeerToken(packet.token) ? packet.token : peerReadiness.peerToken))
                (packet.flags & coop_hit::kFlagWorld) ? hits::OnWorldReport(packet) : hits::OnReport(packet);
        } else if (got == sizeof(coop_ping::Packet)) {
            coop_ping::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = coop_ping::Valid(packet);
            if (accepted && packet.kind == coop_ping::kRequest) {
                // Answer immediately and from here, inside the receive loop.
                // Anything that defers the reply to the next poll measures the
                // local frame time as if it were the peer's distance.
                const auto reply = coop_ping::MakeReply(packet, peerReadiness.token, now);
                if (::sendto(dataSock, reinterpret_cast<const char*>(&reply), sizeof(reply), 0,
                             reinterpret_cast<sockaddr*>(&from), sizeof(from)) == sizeof(reply))
                    InterlockedIncrement(&pingReplied);
            } else if (accepted && peerRuntime[slot].link.OnReply(packet, now)) {
                InterlockedIncrement(&pingRepliesIn);
            }
        } else if (got == sizeof(coop_template::Packet)) {
            coop_template::Packet packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            accepted = peerTable.Count() && templates::Receive(packet, now);
        } else if (got == static_cast<int>(sizeof(coop_config::Packet))) {
            // dispatched by size: no other packet may have this one's
            static_assert(sizeof(coop_config::Packet) != sizeof(survnet::Packet) && sizeof(coop_config::Packet) != sizeof(PlayerState) &&
                              sizeof(coop_config::Packet) != sizeof(coop_peer::Packet) && sizeof(coop_config::Packet) != sizeof(coop_loadout::Packet) &&
                              sizeof(coop_config::Packet) != sizeof(coop_loadout::BotPacket) && sizeof(coop_config::Packet) != sizeof(coop_suit::Packet) &&
                              sizeof(coop_config::Packet) != sizeof(coop_death::Packet) && sizeof(coop_config::Packet) != sizeof(coop_death::Ack) &&
                              sizeof(coop_config::Packet) != sizeof(mirror::Packet) && sizeof(coop_config::Packet) != sizeof(level::Packet) &&
                              sizeof(coop_config::Packet) != sizeof(tk::PosePacket) && sizeof(coop_config::Packet) != sizeof(regen::Packet) &&
                              sizeof(coop_config::Packet) != sizeof(restsync::Packet) && sizeof(coop_config::Packet) != sizeof(removals::Packet) &&
                              sizeof(coop_config::Packet) != sizeof(loadsync::Packet) && sizeof(coop_config::Packet) != sizeof(tk::Packet) &&
                              sizeof(coop_config::Packet) != sizeof(streams::vis::Packet) && sizeof(coop_config::Packet) != sizeof(streams::Packet) &&
                              sizeof(coop_config::Packet) != sizeof(checkpoint::Packet) && sizeof(coop_config::Packet) != sizeof(coop_gate_wire::Packet) &&
                              sizeof(coop_config::Packet) != sizeof(coop_hit::Packet) && sizeof(coop_config::Packet) != sizeof(coop_ping::Packet) &&
                              sizeof(coop_config::Packet) != sizeof(coop_template::Packet) && sizeof(coop_config::Packet) != sizeof(coop_mpnative::Packet) &&
                              sizeof(coop_config::Packet) != sizeof(coop_versus::Packet) && sizeof(coop_config::Packet) != sizeof(cutscene::leadaim::Packet) &&
                              sizeof(coop_config::Packet) != sizeof(cutscene::leadinput::Packet) && sizeof(coop_config::Packet) != sizeof(hits::WorldPacket) &&
                              sizeof(coop_config::Packet) != sizeof(necro::PoolPacket) && sizeof(coop_config::Packet) != 132 && sizeof(coop_config::Packet) != 18,
                          "the config packet's size is taken");
            accepted = configcheck::OnBytes(buffer, got);
        } else if (got == sizeof(PlayerState)) {
            PlayerState in{};
            std::memcpy(&in, buffer, sizeof(in));
            const bool relayed = (in.machine & kRelayBit) != 0;
            accepted = in.magic == kStateMagic && in.token && in.token != peerReadiness.token &&
                       (relayed ? fromHostEndpoint : in.token == source->token) && std::isfinite(in.x) && std::isfinite(in.y) &&
                       std::isfinite(in.z) && std::isfinite(in.yaw) && std::isfinite(in.forward[0]) &&
                       std::isfinite(in.forward[1]) && std::isfinite(in.forward[2]);
            if (accepted) {
                // The sender rides in the packet, so a position is never filed
                // under the wrong player even when several arrive in one pass of
                // this loop, and a position the host forwarded on somebody else's
                // behalf is filed under that somebody rather than the host.
                const uint32_t machine = in.machine & ~kRelayBit;
                unsigned target = slot;
                if (relayed) {
                    auto* owner = peerTable.AcceptRelayed(in.token, machine,
                                                          static_cast<uint32_t>(now));
                    if (!owner) { InterlockedIncrement(&relayDropped); continue; }
                    target = PeerSlot(owner);
                } else {
                    source->machine = machine;
                    // Everyone else needs to hear about this too. Clients are
                    // configured against the host alone and refuse packets from
                    // anywhere else, so without this the host is the only
                    // machine that ever sees more than one partner.
                    if (IsHost()) {
                        PlayerState forward = in;
                        forward.machine = machine | kRelayBit;
                        for (const auto& other : peerTable.peers) {
                            if (!other.Addressable() || PeerSlot(&other) == slot) continue;
                            sockaddr_in to{};
                            to.sin_family = AF_INET;
                            to.sin_addr.s_addr = other.ip;
                            to.sin_port = other.port;
                            if (::sendto(dataSock, reinterpret_cast<const char*>(&forward),
                                         sizeof(forward), 0, reinterpret_cast<sockaddr*>(&to),
                                         sizeof(to)) == sizeof(forward))
                                InterlockedIncrement(&relayForwarded);
                        }
                    }
                }
                PeerRuntime& runtime = peerRuntime[target];
                // A restarted partner (same address, new token) or a recycled
                // slot starts its sequence again at 1.
                if (runtime.stateToken != in.token) { runtime.stateToken = in.token; runtime.lastSequence = 0; }
                // An older sample arriving after a newer one is dropped rather
                // than applied, which would show as the other Isaac snapping
                // backwards. Unsigned difference, so the counter may wrap.
                if (runtime.lastSequence &&
                    static_cast<int32_t>(in.sequence - runtime.lastSequence) <= 0) {
                    ++runtime.reordered;
                    continue;
                }
                runtime.lastSequence = in.sequence;
                runtime.pos[0] = in.x; runtime.pos[1] = in.y; runtime.pos[2] = in.z;
                runtime.yaw = in.yaw; runtime.flags = in.flags;
                runtime.lastTick = in.tick; runtime.lastState = now;
                InterlockedIncrement(&peerStatesReceived);
                // The ghost body and the stand-in still follow one partner, so
                // the long-standing globals keep meaning what they always did:
                // the first peer in the table.
                if (target == FirstPeerSlot()) {
                    partnerPos[0] = in.x; partnerPos[1] = in.y; partnerPos[2] = in.z;
                    partnerYaw = in.yaw;
                    partnerLastTick = in.tick; partnerLastSeen = now;
                    InterlockedIncrement(&statePacketsReceived);
                }
            }
        }
        if (!accepted) { InterlockedIncrement(&dataRejected); continue; }
        source->lastSeen = now;
        if (!partnerAddrKnown) { partnerAddr = from; partnerAddrKnown = true; }
    }
    if (drained == 16) {
        u_long pendingBytes = 0;
        if (::ioctlsocket(dataSock, FIONREAD, &pendingBytes) == 0 && pendingBytes)
            InterlockedIncrement(&receiveBacklogs);
    }
    // A partner heard for the first time (a joiner, or one back with a new token) gets the
    // suit, the loadout and the host's wave state at once instead of at the next refresh.
    {
        static uint32_t knownToken[coop_peers::kMaxPeers]{};
        for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
            const auto& peer = peerTable.peers[s];
            const uint32_t token = peer.used ? peer.token : 0;
            if (token == knownToken[s]) continue;
            knownToken[s] = token;
            if (!token) continue;
            if (suitSync && suitSender.have) { suitSender.pending = 5; suitSender.lastSend = now - 1000; }
            if (loadoutSync && loadoutSender.slots) { loadoutSender.sweeps = 3; loadoutSender.cursor = 0; loadoutSender.lastSend = now - 1000; }
            survnet::ResendLatest();
            gate::Event("coop new partner token %08X (slot %u, machine %d): suit, loadout%s sent now", token, s,
                        peer.machine == 0xFFFFFFFFu ? -1 : static_cast<int>(peer.machine), IsHost() ? " and the wave state" : "");
        }
    }
    // Publish the local loadout; Publish ignores an unchanged set, so a steady
    // inventory is silent and picking a weapon up produces one sweep.
    if (loadoutSync && player) {
        coop_loadout::Entry carried[coop_loadout::kMaxSlots]{};
        const unsigned slots = ReadLocalLoadout(player, carried, coop_loadout::kMaxSlots);
        if (slots) {
            InterlockedExchange(&loadoutSlotsRead, static_cast<LONG>(slots));
            loadoutSender.Publish(carried, slots, now);
        }
    }
    const bool anyPeer = peerTable.Count() != 0;
    if (loadoutSync && anyPeer && loadoutSender.Due(now)) {
        const auto packet = loadoutSender.MakePacket(now);
        if (SendToPeers(&packet, sizeof(packet))) InterlockedIncrement(&loadoutSent);
    }
    // The local suit is only sent when it actually changed; Publish ignores repeats.
    if (suitSync && anyPeer && suitSender.Due(now)) {
        const auto packet = suitSender.MakePacket(now);
        if (SendToPeers(&packet, sizeof(packet))) InterlockedIncrement(&suitSent);
    }
    // A local death is retransmitted a few times: the side channel is plain UDP
    // and a single lost datagram would leave a peer at a different checkpoint.
    if (deathSync && deathSender.Expire(now))
        InterlockedIncrement(&deathAbandoned);
    if (deathSync && anyPeer && deathSender.Due(now)) {
        const auto packet = deathSender.MakePacket(now);
        if (SendToPeers(&packet, sizeof(packet))) InterlockedIncrement(&deathSent);
    }
    // Twice a second, per peer. One shared monitor would average four different
    // distances into one meaningless number, so each partner gets its own.
    peerTable.ExpireRelayed(static_cast<uint32_t>(now));
    for (auto& peer : peerTable.peers) {
        if (!peer.Addressable()) continue;
        auto& monitor = peerRuntime[PeerSlot(&peer)].link;
        monitor.Expire(now);
        if (!monitor.Due(now)) continue;
        monitor.token = peerReadiness.token;
        const auto packet = monitor.MakeRequest(now);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = peer.ip;
        to.sin_port = peer.port;
        if (::sendto(dataSock, reinterpret_cast<const char*>(&packet), sizeof(packet), 0,
                     reinterpret_cast<sockaddr*>(&to), sizeof(to)) == sizeof(packet))
            InterlockedIncrement(&pingSent);
    }
    if (anyPeer && now - lastReadinessSend >= 250) {
        lastReadinessSend = now;
        // One packet per partner, because each one carries the acknowledgement
        // of that partner's token and epoch.
        for (auto& peer : peerTable.peers) {
            if (!peer.Addressable()) continue;
            auto& state = peerRuntime[PeerSlot(&peer)].readiness;
            // The local half, set here every time. Relying on the tick to have
            // pushed it first was a race the send won: a packet built from a
            // fresh peer state carries token 0, and the far side drops every
            // packet with no token.
            state.token = peerReadiness.token;
            state.epoch = peerReadiness.epoch;
            state.player = peerReadiness.player;
            state.localReady = peerReadiness.localReady;
            const auto packet = state.MakePacket();
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = peer.ip;
            to.sin_port = peer.port;
            if (::sendto(dataSock, reinterpret_cast<const char*>(&packet), sizeof(packet), 0,
                         reinterpret_cast<sockaddr*>(&to), sizeof(to)) == sizeof(packet))
                InterlockedIncrement(&readinessSent);
        }
        MirrorPrimaryReadiness();
    }
    templates::Poll(now);
    if (anyPeer) roles::Send(now, peerReadiness.token);
    // The transform goes out for whatever body is driven here: Isaac, or the necro
    // (entity-level +0xB0/+0xC0). Only the helmet bit is Player-specific.
    const uint32_t body = player ? player : LocalSlotZeroEntity();
    if (body && anyPeer && now - lastStateSend >= 33) {  // ~30 Hz
        lastStateSend = now;
        float position[3]{}, forward[3]{};
        if (Read(body + placement::kPlayerPosition, position, 12) && Read(body + placement::kPlayerForward, forward, 12)) {
            PlayerState state{};
            state.magic = kStateMagic;
            state.token = peerReadiness.token;   // who this is, so four cannot be confused for one
            state.machine = machineIndex;
            state.sequence = ++stateSequence;
            state.x = position[0]; state.y = position[1]; state.z = position[2];
            state.yaw = placement::YawDegrees(forward);
            state.forward[0] = forward[0]; state.forward[1] = forward[1]; state.forward[2] = forward[2];
            state.tick = now;
            state.flags = (player && helmet::LocalOpen(player)) ? kStateFlagHelmetOpen : 0;
            if (SendToPeers(&state, sizeof(state))) InterlockedIncrement(&statePacketsSent);
        }
    }
}

void* __fastcall OnSocketCtor(void* self, void*, const Address* bind) {
    std::memcpy(self, bind, sizeof(Address));
    if (udp == INVALID_SOCKET) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = htons(static_cast<u_short>(bind->port));
        if (udp == INVALID_SOCKET || ::bind(udp, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            InterlockedExchange(&bindError, static_cast<LONG>(WSAGetLastError()));
            if (udp != INVALID_SOCKET) { closesocket(udp); udp = INVALID_SOCKET; }
        } else {
            u_long nonBlocking = 1;
            ioctlsocket(udp, FIONBIO, &nonBlocking);
        }
    }
    ++socketUsers;
    return self;
}

void __fastcall OnSocketDtor(void*, void*) {
    if (socketUsers && --socketUsers == 0 && udp != INVALID_SOCKET) { closesocket(udp); udp = INVALID_SOCKET; }
}

// TNL::NetError: 0 ok, 2 would block, 3 not open
int __fastcall OnSocketSendto(void*, void*, const Address* to, const unsigned char* buffer, int length, int) {
    if (udp == INVALID_SOCKET || !to || !buffer || length <= 0) return 3;
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_addr.s_addr = to->ip;
    remote.sin_port = htons(static_cast<u_short>(to->port));
    const int sent = ::sendto(udp, reinterpret_cast<const char*>(buffer), length, 0, reinterpret_cast<sockaddr*>(&remote), sizeof(remote));
    if (sent < 0) { InterlockedExchange(&lastSendError, static_cast<LONG>(WSAGetLastError())); InterlockedIncrement(&sendErrors); return 3; }
    InterlockedIncrement(&packetsSent);
    return 0;
}

int __fastcall OnSocketRecvfrom(void*, void*, Address* from, unsigned char* buffer, int size, int* received) {
    InterlockedIncrement(&receiveCalls);
    if (udp == INVALID_SOCKET || !from || !buffer || size <= 0) return 3;
    sockaddr_in remote{};
    int got = -1;
    // TNL's receive loop (NetInterface::checkIncomingPackets, PC 0x0141A3BE:
    // test eax,eax / jne out) stops at any non-zero answer. An ICMP "port
    // unreachable" for an earlier send (WSAECONNRESET) or a truncated datagram
    // (WSAEMSGSIZE) fails only this call; the datagrams behind it are skipped to,
    // or one dead peer would cut every tick's reading short.
    for (int tries = 0; tries < 16; ++tries) {
        int remoteSize = sizeof(remote);
        got = ::recvfrom(udp, reinterpret_cast<char*>(buffer), size, 0, reinterpret_cast<sockaddr*>(&remote), &remoteSize);
        if (got >= 0) break;
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) return 2;
        if (error != WSAECONNRESET && error != WSAEMSGSIZE) return 3;
        InterlockedIncrement(&receiveSkipped);
    }
    if (got < 0) return 3;
    from->ip = remote.sin_addr.s_addr;
    from->port = ntohs(remote.sin_port);
    if (received) *received = got;
    InterlockedIncrement(&packetsReceived);
    return 0;
}

bool ParsePeer(const std::string& text, unsigned& ip, unsigned& port) {
    const auto colon = text.find(':');
    if (colon == std::string::npos) return false;
    const std::string host = text.substr(0, colon);
    port = static_cast<unsigned>(std::strtoul(text.c_str() + colon + 1, nullptr, 10));
    if (!port || port > 65535) return false;
    in_addr parsed{};
    if (inet_pton(AF_INET, host.c_str(), &parsed) != 1) return false;
    ip = parsed.s_addr;
    return true;
}

// A second instance on one machine dies in the first seconds: the game
// looks for its own window class "DeadSpace2WndClass" (helper 0x0044AF80 via
// the FindWindowW import at 0x02158F34) and gives up when one exists. With
// DS2COOP set, the import is pointed at a stub that never finds anything.
constexpr uint32_t kFindWindowSlot = 0x1D58F34;
HWND WINAPI FindWindowNone(LPCWSTR, LPCWSTR) { return nullptr; }
// Redirect the FindWindowW import so the game cannot find an instance of itself
// and quit.
//
// The import table is not filled in by the Windows loader here, because the exe
// is packed: the packer resolves imports itself after it has unpacked, so at
// DllMain the slot holds a packer thunk (2026-09-17: 6113FCC5, which belongs to
// no loaded module). The game's own instance check runs shortly after the
// imports become real, and an instance that loses that race exits silently
// before its plugin has written a line. So the slot is watched and taken the
// moment it turns real.
//
// NotYet: the slot is not ours and not yet the real import (keep waiting);
// Redirected: it has just been redirected; AlreadyDone: it was already ours.
enum class InstanceCheck { NotYet, Redirected, AlreadyDone };

InstanceCheck TryDisableInstanceCheck() {
    uint32_t current = 0;
    if (!Read(base + kFindWindowSlot, &current, 4) || !current) return InstanceCheck::NotYet;
    if (current == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(FindWindowNone)))
        return InstanceCheck::AlreadyDone;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    const uint32_t expected = user32
        ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(GetProcAddress(user32, "FindWindowW")))
        : 0;
    if (!expected || current != expected) return InstanceCheck::NotYet;
    void* slot = reinterpret_cast<void*>(base + kFindWindowSlot);
    DWORD previous = 0;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &previous)) return InstanceCheck::NotYet;
    *reinterpret_cast<void**>(slot) = reinterpret_cast<void*>(FindWindowNone);
    VirtualProtect(slot, 4, previous, &previous);
    return InstanceCheck::Redirected;
}

// Spins on the slot: one dword read per turn, only until it turns real (within
// the first seconds). Silent while it waits.
bool WaitAndDisableInstanceCheck(FILE* log, unsigned milliseconds) {
    const DWORD started = GetTickCount();
    for (;;) {
        const auto result = TryDisableInstanceCheck();
        if (result == InstanceCheck::Redirected) {
            std::fprintf(log, "coop instance_check disabled after %u ms "
                              "(FindWindowW import redirected)\n",
                         static_cast<unsigned>(GetTickCount() - started));
            std::fflush(log);
            return true;
        }
        if (result == InstanceCheck::AlreadyDone) return true;
        if (GetTickCount() - started >= milliseconds) {
            uint32_t current = 0;
            Read(base + kFindWindowSlot, &current, 4);
            std::fprintf(log, "coop instance_check slot=%08X still holds %08X after %u ms; "
                              "a second instance may refuse to start\n",
                         static_cast<unsigned>(base + kFindWindowSlot), current, milliseconds);
            std::fflush(log);
            return false;
        }
        Sleep(2);
    }
}

bool DisableInstanceCheck(FILE* log) {
    return WaitAndDisableInstanceCheck(log, 0);
}

volatile LONG startRequests = 0, startResult = 0;  // 1 started, 2 no allocator, 3 no memory, 4 already
char report[4096]{};
volatile LONG reportReady = 0;
DWORD lastReport = 0;

// Client only: one connection attempt to the host. TNL retries the
// challenge a few times on its own and then gives up (state 5, timed out);
// Poll() calls this again when that happens, so the host may start later.
void Connect() {
    if (!session || !dialledCount) return;
    uint32_t iface = 0;
    if (!Read(session + kSessionInterface, &iface, 4) || !iface) return;
    using Create = void (__thiscall*)(void*, FakePlayer*);
    // Every partner, not just the one that happens to be first. A client that
    // only dials the host can never receive anybody else's body, because TNL
    // will not pass a ghost on to a third connection.
    for (unsigned i = 0; i < dialledCount; ++i) {
        reinterpret_cast<Create>(base + kCreateConnection)(
            reinterpret_cast<void*>(iface), &dialledPlayer[i]);
        InterlockedIncrement(&connectAttempts);
    }
    connectStarted = GetTickCount();
}

bool InLevel() {
    uint32_t manager = 0, tocs = 0;
    if (!Read(base + kStreamManager, &manager, 4) || !manager || !Read(manager + 0x130, &tocs, 4) || tocs > 32) return false;
    if (tocs > 8) return true;
    // A small map has fewer TOCs: Concourse (pdlc_mp_mall) had 6 with the
    // randomizer pool, so no zone and no HUD until more of it streamed in
    // (12.10.). There the level manager running (state 16), a body in player
    // slot 0 and a map that is not the front end say the same.
    uint32_t lm = 0, state = 0;
    if (tocs < 3 || !Read(base + 0x1C1C0A4, &lm, 4) || !lm || !Read(lm + 0x18, &state, 4) || state != 16 || !LocalSlotZeroEntity()) return false;
    char map[16]{};
    const uint32_t loop = level::GameMainLoop();
    return loop && Read(loop + level::kMapOffset, map, sizeof(map) - 1) && map[0] && _strnicmp(map, "frontend", 8) != 0;
}

namespace mpmap { bool InVisit(); bool Arrived(); DWORD SettleMs(); bool UsableAnchor(const float* p); }   // runtime_coop_mpmap.inc
namespace necro { uint32_t LocalGhost(); }                            // runtime_coop_necro.inc
// The local body the settle and the peer readiness follow. On an MP map a necro
// between lives is its ghost (a RespawnProxy, never in slot 0); not counting it kept the session
// from ever replicating fully while a necro picked its type and vent (11.10.:
// tick_mode 1 the whole match on both machines).
uint32_t SettleBody() {
    const uint32_t body = LocalSlotZeroEntity();
    return body ? body : mpmap::InVisit() ? necro::LocalGhost() : 0;
}
volatile LONG settleAdopted = 0;
// Require a stable local player after loading and after body replacement.
bool LevelSettled(bool levelAvailable) {
    const LONG released = InterlockedCompareExchange(&loading_hold::released, 0, 0);
    const bool holding = InterlockedCompareExchange(&loading_hold::holding, 0, 0) != 0;
    const DWORD now = GetTickCount();
    // A NecroPlayer in slot 0 is a settled body too; otherwise the first
    // transform unsettles the level, replicationReady drops and the session
    // stops ticking for everybody (audit 2026-09-21).
    uint32_t player = SettleBody();
    // A native MP death takes the body out of slot 0 at once and the respawn comes
    // ~10 s later: in between the level stands as it was. Unsettled, the bots let
    // go of their bodies (12.10. 00:58:44) and the session stopped replicating.
    static DWORD bodyLostAt = 0;
    if (mpmap::InVisit() && !player && localSettle.tracking && localSettle.player) {
        if (!bodyLostAt) bodyLostAt = now;
        if (now - bodyLostAt < 30000) player = localSettle.player;
    } else {
        bodyLostAt = 0;
    }
    // On an MP map the bodies change all match long (Isaac, ghost, necro, ghost):
    // the level stays as settled as it was, as after a respawn.
    if (mpmap::InVisit() && localSettle.Adopt(player)) InterlockedIncrement(&settleAdopted);
    return localSettle.Update(now, levelAvailable, holding, static_cast<uint32_t>(released), player, mpmap::InVisit() ? mpmap::SettleMs() : 4000);
}

// The session tick runs engine code that never ran in the campaign. A fault
// inside it is caught here, logged with its address, and the ticking stops,
// so the game keeps running and the log says where it went wrong instead of
// a dump per attempt. The engine state after a caught fault is not trusted:
// subsequent plugin Poll calls stop. Native session callbacks remain installed.
volatile LONG tickFaultCode = 0, tickFaultAddress = 0;
bool faulted = false;
int TickFilter(unsigned code, EXCEPTION_POINTERS* info) {
    InterlockedExchange(&tickFaultCode, static_cast<LONG>(code));
    InterlockedExchange(&tickFaultAddress, static_cast<LONG>(reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress)));
    return EXCEPTION_EXECUTE_HANDLER;
}
void SafeCall(void (__thiscall* function)(void*), void* object) {
    __try {
        function(object);
    } __except (TickFilter(GetExceptionCode(), GetExceptionInformation())) {
        faulted = true;
        replicationReady = false;
        tickMode = 0;
    }
}

// The local Player's NetReflectable sits at Player+0x5C (dump 41716). Its
// vtable: slot 1 GetNetID (entity GUID hash + sub key at reflectable-0x1C),
// slot 2 SetSubKey(id), slot 12 netInit (allocates the 0x70-byte net state
// at reflectable+0x118 once), and NetReflectable::AddToSession is
// 0x007E10E0(this, existingProxy) which builds the NetProxy and registers it.
// This is what NetObjSync would do for it, done for the player alone.
constexpr uint32_t kPlayerReflectable = 0x5C, kAddToSession = 0x3E10E0;
uint32_t registeredPlayer = 0, registeredSubKey = 0, registeredProxy = 0;
volatile LONG registrationAttempts = 0, registrationRejected = 0, connectionTransitions = 0;
volatile LONG registrationStatus = 0;
void RegisterLocalPlayer() {
    const uint32_t player = LocalCampaignPlayer();
    if (player != registeredPlayer) {
        registeredPlayer = registeredProxy = registeredSubKey = 0;
    }
    uint32_t vtable = 0, existing = 0;
    if (!player || !Read(player, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva) {
        InterlockedExchange(&registrationStatus, 1); return;
    }
    const uint32_t reflectable = player + kPlayerReflectable;
    uint32_t reflectableVtable = 0, setSubKey = 0, netInit = 0;
    if (!Read(reflectable, &reflectableVtable, 4) || !Read(reflectableVtable + 8, &setSubKey, 4) || !Read(reflectableVtable + 0x30, &netInit, 4)) { InterlockedExchange(&registrationStatus, 2); return; }
    if (!Read(reflectable + 0xC, &existing, 4)) { InterlockedExchange(&registrationStatus, 3); InterlockedIncrement(&registrationRejected); return; }
    if (existing) {
        registeredPlayer = player;
        registeredProxy = existing;
        Read(player + 0x40, &registeredSubKey, 4);
        InterlockedExchange(&registrationStatus, 6);
        return;
    }
    if (setSubKey != base + 0x3D1D10 || netInit != base + 0x3D1DC0) {
        InterlockedExchange(&registrationStatus, 4);
        InterlockedIncrement(&registrationRejected);
        return;
    }
    using SetSubKey = void (__thiscall*)(void*, uint32_t);
    using NetInit = void (__thiscall*)(void*);
    using AddToSession = void (__thiscall*)(void*, uint32_t);
    if (!AllocatePlayerSubKey(session, machineIndex, registeredSubKey)) {
        InterlockedExchange(&registrationStatus, 5);
        InterlockedIncrement(&registrationRejected);
        return;
    }
    InterlockedIncrement(&registrationAttempts);
    reinterpret_cast<SetSubKey>(setSubKey)(reinterpret_cast<void*>(reflectable), registeredSubKey);
    reinterpret_cast<NetInit>(netInit)(reinterpret_cast<void*>(reflectable));
    reinterpret_cast<AddToSession>(base + kAddToSession)(reinterpret_cast<void*>(reflectable), 0);
    Read(reflectable + 0xC, &registeredProxy, 4);
    registeredPlayer = registeredProxy ? player : 0;
    InterlockedExchange(&registrationStatus, registeredProxy ? 7 : 8);
    if (!registeredProxy) {
        InterlockedIncrement(&registrationRejected);
        faulted = true;
        replicationReady = false;
        tickMode = 0;
    }
}

void RegisterLocalPlayerSafe() {
    __try { RegisterLocalPlayer(); }
    __except (TickFilter(GetExceptionCode(), GetExceptionInformation())) {
        faulted = true;
        replicationReady = false;
        tickMode = 0;
    }
}

#include "runtime_coop_npcs.inc"
namespace bot_sense { extern bool losReady; bool CastRaw(const float* from, const float* to, uint32_t exclude, float& fraction, uint32_t& hit); }   // runtime_coop_bot_sense.inc
#include "runtime_coop_necro.inc"
namespace roles {
const char* RoleName(int r) {
    return r == 1 ? "a living necro" : r == 2 ? "a necro at the vents" : r == 3 ? "a necro watching a scene as Isaac" : r == 4 ? "a dead human watching a partner" : "a human";
}
void Send(DWORD now, uint32_t token) {
    const int mine = spectate::Active() ? 4 : necro::LocalRole();
    if (!token || (mine == lastSentRole && now - sentAt < 1000)) return;
    if (mine != lastSentRole) gate::Event("roles: this machine plays %s", RoleName(mine));
    lastSentRole = mine;
    sentAt = now;
    const Packet p{kMagic, kVersion, static_cast<uint8_t>(mine), 0, token};
    if (SendToPeers(&p, sizeof(p)) > 0) InterlockedIncrement(&sent);
}
bool OnBytes(const void* data, int size, unsigned slot, DWORD now) {
    if (size != static_cast<int>(sizeof(Packet))) return false;
    Packet p{};
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != kMagic || p.version != kVersion || p.role > 4 || !KnownPeerToken(p.token)) return false;
    InterlockedIncrement(&received);
    const unsigned s = SlotForToken(p.token, slot, static_cast<uint32_t>(now));
    if (s >= coop_peers::kMaxPeers) return true;
    if (role[s] != p.role || !heardAt[s]) {
        InterlockedIncrement(&changes);
        gate::Event("roles: partner %u (token %08X) plays %s", s, p.token, RoleName(p.role));
        if (role[s] == 4 && p.role == 0) spectatorBackAt[s] = Stamp(now);
        if (heardAt[s] && role[s] != p.role) {
            roleChangedAt[s] = Stamp(now);
            necro::NotePartnerRole(s, role[s], p.role, now);   // a necro came out: the loud cue
        }
    }
    role[s] = p.role;
    heardAt[s] = Stamp(now);
    roleToken[s] = p.token;
    return true;
}
}  // namespace roles
#include "runtime_coop_catchup.inc"
#include "runtime_coop_bot_sense.inc"
#include "runtime_coop_bot_nav.inc"
#include "runtime_coop_bot_aim.inc"
namespace shop { bool HoldsKey(int vk, coop_shop::Guard who); }   // runtime_coop_shop.inc: the open shop owns the numpad
namespace versus { void CreditBotKill(unsigned bot, uint64_t ref, uint16_t flags, DWORD ageMs); }   // runtime_coop_versus.inc
#include "runtime_coop_bot.inc"

// --- the host's bots for the partners ([Bot] GhostToClients) ---
// A bot is a local Player like Isaac. Registered with the session the way
// RegisterLocalPlayer does it, it reaches every partner as a ghost Player. Its
// sub-key carries machine byte 0xF0+index, so MachineOf and GhostForMachine never
// take it for the host's Isaac (machine 0). A respawned bot is a new body and
// is registered again; a deleted body leaves the session with the entity.
namespace botnet {
bool enabled = false;
uint32_t registered[kMaxBots]{};
volatile LONG attempts = 0, done = 0, refused = 0, faults = 0;
int Register(uint32_t entity, unsigned index) {
    uint32_t vtable = 0, reflectableVtable = 0, setSubKey = 0, netInit = 0, existing = 0, subKey = 0, proxy = 0;
    if (!Read(entity, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva) return 1;
    const uint32_t reflectable = entity + kPlayerReflectable;
    if (!Read(reflectable, &reflectableVtable, 4) || !Read(reflectableVtable + 8, &setSubKey, 4) || !Read(reflectableVtable + 0x30, &netInit, 4)) return 2;
    if (!Read(reflectable + 0xC, &existing, 4)) return 3;
    if (existing) return 6;   // already in the session
    if (setSubKey != base + 0x3D1D10 || netInit != base + 0x3D1DC0) return 4;
    if (!AllocatePlayerSubKey(session, 0xF0u + index, subKey)) return 5;
    using SetSubKey = void(__thiscall*)(void*, uint32_t);
    using NetInit = void(__thiscall*)(void*);
    using AddToSession = void(__thiscall*)(void*, uint32_t);
    __try {
        reinterpret_cast<SetSubKey>(setSubKey)(reinterpret_cast<void*>(reflectable), subKey);
        reinterpret_cast<NetInit>(netInit)(reinterpret_cast<void*>(reflectable));
        reinterpret_cast<AddToSession>(base + kAddToSession)(reinterpret_cast<void*>(reflectable), 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return 9;
    }
    Read(reflectable + 0xC, &proxy, 4);
    gate::Event("bot %u: registered with the session for the partners: entity=%08X sub_key=%08X proxy=%08X", index + 1, entity, subKey, proxy);
    return proxy ? 7 : 8;
}
// Game thread, from Poll, on the host once replication runs.
void Tick() {
    if (!enabled || !session || !replicationReady || !IsHost() || faulted) return;
    for (unsigned i = 0; i < kMaxBots; ++i) {
        const auto& unit = bot::units[i];
        if (!unit.entity || unit.state != bot::State::Active) { registered[i] = 0; continue; }
        if (registered[i] == unit.entity) continue;
        InterlockedIncrement(&attempts);
        const int status = Register(unit.entity, i);
        if (status == 6 || status == 7) { registered[i] = unit.entity; InterlockedIncrement(&done); continue; }
        InterlockedIncrement(&refused);
        if (InterlockedCompareExchange(&refused, 0, 0) <= 10)
            gate::Event("bot %u: session registration refused, status %d (1 class, 2 layout, 4 functions, 5 sub-key, 8 no proxy, 9 fault)", i + 1, status);
        registered[i] = unit.entity;   // one try per body
    }
}
// Host: each registered bot's carried weapons to the partners. A new body starts
// a new epoch, so a client arms the new ghost; every 5 s one more sweep reaches a
// partner who joined after the last change.
void SendLoadouts(DWORD now) {
    if (!enabled || !loadoutSync || !session || !replicationReady || !IsHost() || faulted) return;
    static uint32_t publishedFor[kMaxBots]{};
    for (unsigned i = 0; i < kMaxBots; ++i) {
        const auto& unit = bot::units[i];
        auto& sender = botLoadoutSenders[i];
        if (!unit.entity || unit.state != bot::State::Active || registered[i] != unit.entity) continue;
        if (publishedFor[i] != unit.entity) {
            const uint32_t epoch = sender.epoch;
            sender = coop_loadout::Sender{};
            sender.epoch = epoch;
            publishedFor[i] = unit.entity;
        }
        sender.token = loadoutSender.token;
        coop_loadout::Entry carried[coop_loadout::kMaxSlots]{};
        const unsigned slots = ReadLocalLoadout(unit.entity, carried, coop_loadout::kMaxSlots);
        if (slots) sender.Publish(carried, slots, now);
        if (!sender.sweeps && sender.slots && now - sender.lastSend >= 5000) { sender.sweeps = 1; sender.cursor = 0; }
        if (!peerTable.Count() || !sender.Due(now)) continue;
        const auto packet = coop_loadout::ToBot(sender.MakePacket(now), i);
        if (SendToPeers(&packet, sizeof(packet))) InterlockedIncrement(&botLoadoutSent);
    }
}
// Host: the suit each registered bot wears (Player+0x1688, a Uniform, its GUID at
// +0x24), on a change and every 2 s, so a partner dresses the bot's ghost in it
// (12.10.: every bot ghost wore the template's Engineering suit). The partners'
// own suits travel with the suit sync; bots are no peers.
constexpr uint32_t kBotSuitMagic = 0x54555342, kBotSuitVersion = 1;   // 'BSUT'
struct BotSuitPacket { uint32_t magic, version, token, index, guid[4]; };
static_assert(sizeof(BotSuitPacket) == kBotSuitBytes, "bot suit packet");
struct BotSuitState { uint32_t guid[4]{}; uint32_t generation = 0; bool have = false; };
BotSuitState botSuits[kMaxBots];   // a partner: what the host's bots wear
volatile LONG botSuitSent = 0, botSuitIn = 0, botSuitChanges = 0;
bool WornSuit(uint32_t player, uint32_t* out) {
    uint32_t uniform = 0, vtable = 0;
    return Read(player + 0x1688, &uniform, 4) && uniform && Read(uniform, &vtable, 4) && vtable == base + 0x174FCB0 && Read(uniform + 0x24, out, 16) &&
           (out[0] | out[1] | out[2] | out[3]);
}
void SendBotSuits(DWORD now) {
    if (!enabled || !session || !replicationReady || !IsHost() || faulted || !peerTable.Count()) return;
    static uint32_t sentGuid[kMaxBots][4]{};
    static DWORD sentAt[kMaxBots]{};
    for (unsigned i = 0; i < kMaxBots; ++i) {
        const auto& unit = bot::units[i];
        uint32_t guid[4]{};
        if (!unit.entity || unit.state != bot::State::Active || registered[i] != unit.entity || !WornSuit(unit.entity, guid)) continue;
        if (std::memcmp(guid, sentGuid[i], 16) == 0 && now - sentAt[i] < 2000) continue;
        const BotSuitPacket packet{kBotSuitMagic, kBotSuitVersion, peerReadiness.token, i, {guid[0], guid[1], guid[2], guid[3]}};
        if (SendToPeers(&packet, sizeof(packet)) > 0) InterlockedIncrement(&botSuitSent);
        std::memcpy(sentGuid[i], guid, 16);
        sentAt[i] = now;
    }
}
bool OnBotSuitBytes(const void* data, int size) {
    if (size != static_cast<int>(sizeof(BotSuitPacket))) return false;
    BotSuitPacket p{};
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != kBotSuitMagic || p.version != kBotSuitVersion || p.index >= kMaxBots || !KnownPeerToken(p.token) || IsHost()) return false;
    InterlockedIncrement(&botSuitIn);
    BotSuitState& s = botSuits[p.index];
    if (!s.have || std::memcmp(s.guid, p.guid, 16) != 0) {
        std::memcpy(s.guid, p.guid, 16);
        s.have = true;
        ++s.generation;
        if (InterlockedIncrement(&botSuitChanges) <= 30) gate::Event("bot %u suit from the host: %08X-%08X", p.index + 1, p.guid[0], p.guid[1]);
    }
    return true;
}
}  // namespace botnet

#include "runtime_coop_uselock.inc"
#include "runtime_coop_turret.inc"
#include "runtime_coop_gadgets.inc"
#include "runtime_coop_mpmap.inc"
#include "runtime_coop_shop.inc"
#include "runtime_coop_hud.inc"
#include "runtime_coop_nameplate.inc"
#include "runtime_coop_laserdiag.inc"
#include "runtime_coop_voices.inc"
#include "runtime_coop_dementia.inc"
#include "runtime_coop_survmusic.inc"
#include "runtime_coop_respawn.inc"
#include "runtime_coop_spectate.inc"
#include "runtime_coop_clientprogress.inc"
#include "runtime_coop_bugreport.inc"
#include "runtime_coop_objectives.inc"
#include "runtime_coop_zone.inc"
#include "runtime_coop_marks.inc"
#include "runtime_coop_npcwatch.inc"
#include "runtime_coop_patrolhelmet.inc"
#include "runtime_coop_credits.inc"
#include "runtime_coop_escalation.inc"
#include "runtime_coop_pcmcodec.inc"
#include "runtime_coop_bankheap.inc"
#include "runtime_coop_outbreakdraw.inc"

// --- the role, chosen in the main menu (runtime_coop_lobby.inc) ---
// Install read the role once, from DS2COOP or the INI. The lobby sets it again
// before the level that starts the session; nothing here touches a live session.
volatile LONG roleChanges = 0;
bool fromEnvironmentRole = false;
void ApplyRole(const char* why) {
    npcs::client = !IsHost();
    peerText.clear();
    InterlockedIncrement(&roleChanges);
    gate::Event("coop role %s (%s): network=%d machine=%u dialled=%u peer=%08X:%u session=%08X", IsHost() ? "host" : "client", why, network ? 1 : 0,
                machineIndex, dialledCount, peerIp, peerPort, session);
}
namespace matchend { bool Reusable(); }   // runtime_coop_matchend.inc: an idle session kept after the match ended
// A copy started by a DS2COOP launcher keeps the role, machine and partners it started with.
bool LauncherHost() { return fromEnvironmentRole && IsHost(); }
bool LauncherClient() { return fromEnvironmentRole && !IsHost(); }
// The launcher comes first: it is the reason a launcher copy never changes role, running session or not.
bool RoleChangeAllowed(const char* what) {
    if (fromEnvironmentRole) {
        gate::Event("coop role change to %s REFUSED: the launcher (DS2COOP) started this copy as %s machine %u", what, IsHost() ? "the host," : "a client,", machineIndex);
        return false;
    }
    if (session && !matchend::Reusable()) { gate::Event("coop role change to %s REFUSED: a session is running", what); return false; }
    return true;
}
// A host back in the menu hosts the next match in the session it already runs
// (13.10.: the second "host co-op" was refused). Partners still connected stay;
// new joiners go into the dial list and the redial loop connects them.
volatile LONG hostSessionReuses = 0, hostSessionDials = 0, launcherHostKeeps = 0, launcherClientKeeps = 0;
bool HostSessionRunning() { return session && network && IsHost() && !fromEnvironmentRole; }
// A role picked in the menu starts from an empty partner list: configured
// peers are never recycled out of the table, so a stale one would survive.
void ResetPartners() {
    uselock::ResetClaims();
    AcquireSRWLockExclusive(&survnet::lock);
    survnet::receivedRewards.Clear();
    survnet::rewardReceiver.Restart();
    survnet::NewRewardRun();
    for (auto& partner : survnet::rewardPartners) partner.Bind(0);
    survnet::rewardHelloAt = 0;
    survnet::remoteCapsules = {};
    survnet::hostCapsules = {};
    survnet::hadCapsules = false;
    survnet::capsuleRepeats = 0;
    survnet::latest[0] = {};
    survnet::latest[1] = {};
    for (auto& slot : survnet::pending) slot.left = 0;
    survnet::capsuleTail = survnet::capsuleHead;
    ReleaseSRWLockExclusive(&survnet::lock);
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) streams::ForgetPeer(s);
    peerTable = coop_peers::Table{};
    for (auto& runtime : peerRuntime) runtime = PeerRuntime{};
    dialledCount = 0;
    peerIp = peerPort = 0;
    peerImpl.address.address = {0, 0};
    partnerAddrKnown = false;
    mesh::OnPartnersReset();
}
// Every machine dials every other one (as DS2_Coop_4P_Local.cmd does): TNL does
// not forward a ghost, so the bodies need a full mesh. The side channel learns
// each partner's data port too.
void AddDial(uint32_t ip, unsigned port) {
    if (!ip || !port || dialledCount >= kMaxDialled) return;
    for (unsigned i = 0; i < dialledCount; ++i) if (dialledIps[i] == ip && dialledPorts[i] == port) return;
    // filled before it is counted: with a running session the redial loop may read it
    const unsigned i = dialledCount;
    dialledIps[i] = ip;
    dialledPorts[i] = port;
    dialledImpl[i].address.vtable = fakeVtable;
    dialledImpl[i].address.address = {ip, port};
    dialledPlayer[i].impl = &dialledImpl[i];
    if (!i) {
        peerIp = ip;
        peerPort = port;
        peerImpl.address.vtable = fakeVtable;
        peerImpl.address.address = {ip, port};
    }
    MemoryBarrier();
    dialledCount = i + 1;
    peerTable.Configure(ip, htons(static_cast<u_short>(port + kDataPortOffset)), GetTickCount());
}
bool ConfigureSolo() {
    if (!RoleChangeAllowed("solo")) return false;
    network = false;
    ResetPartners();
    machineIndex = 0;
    ApplyRole("solo from the menu");
    return true;
}
bool ConfigureAsHost() {
    if (HostSessionRunning()) {
        InterlockedIncrement(&hostSessionReuses);
        gate::Event("coop role host kept: session %08X from the last match keeps running, %u partner(s) dialled, connected=%d", session, dialledCount,
                    connected ? 1 : 0);
        return true;
    }
    // A launcher's host hosts as it is: its partners, machine and session stay (17.10.: refused on both copies).
    if (LauncherHost()) {
        static DWORD loggedAt = 0;
        const DWORD now = GetTickCount();
        if (InterlockedIncrement(&launcherHostKeeps) == 1 || now - loggedAt > 2000)
            gate::Event("coop role host kept: the launcher (DS2COOP) started this copy as the host; network=%d session=%08X, %u partner(s) known, %u dialled",
                        network ? 1 : 0, session, peerTable.Count(), dialledCount);
        loggedAt = now;
        return network;
    }
    if (!RoleChangeAllowed("host")) return false;
    network = true;
    ResetPartners();
    machineIndex = 0;
    ApplyRole("host from the menu");
    return true;
}
// At the host's launch: dial every joiner from the roster.
bool ConfigureHostDials(const coop_lobby::RosterEntry* roster, unsigned count) {
    if (HostSessionRunning() || (LauncherHost() && network)) {
        const unsigned before = dialledCount;
        for (unsigned i = 0; i < count; ++i) if (roster[i].machine) AddDial(roster[i].ip, roster[i].gamePort);
        InterlockedExchangeAdd(&hostSessionDials, static_cast<LONG>(dialledCount - before));
        gate::Event("coop host dials %s %08X: %u new, %u dialled; the redial loop connects them", LauncherHost() ? "as the launcher's host, session" : "in the running session",
                    session, dialledCount - before, dialledCount);
        return true;
    }
    if (!RoleChangeAllowed("host dials") || !network || !IsHost()) return false;
    for (unsigned i = 0; i < count; ++i) if (roster[i].machine) AddDial(roster[i].ip, roster[i].gamePort);
    ApplyRole("host dials the roster");
    return true;
}
bool ConfigureAsClient(unsigned hostIp, unsigned hostPort, unsigned machine, const coop_lobby::RosterEntry* roster, unsigned count) {
    // A launcher's client keeps its machine number and its dial to the host; the
    // lobby's other joiners are dialled as well.
    if (LauncherClient() && network) {
        InterlockedIncrement(&launcherClientKeeps);
        const unsigned before = dialledCount;
        for (unsigned i = 0; i < count; ++i)
            if (roster[i].machine && roster[i].machine != machine) AddDial(roster[i].ip, roster[i].gamePort);
        gate::Event("coop role client kept: the launcher (DS2COOP) started this copy as machine %u (the lobby numbered it %u); host %08X:%u, %u new dial(s), %u dialled",
                    machineIndex, machine, static_cast<unsigned>(ntohl(hostIp)), hostPort, dialledCount - before, dialledCount);
        return true;
    }
    if (!RoleChangeAllowed("client")) return false;
    network = true;
    ResetPartners();
    machineIndex = machine ? machine : 1;
    AddDial(hostIp, hostPort);
    // the template exchange talks to the host
    partnerAddr.sin_family = AF_INET;
    partnerAddr.sin_addr.s_addr = hostIp;
    partnerAddr.sin_port = htons(static_cast<u_short>(hostPort + kDataPortOffset));
    partnerAddrKnown = true;
    for (unsigned i = 0; i < count; ++i)
        if (roster[i].machine && roster[i].machine != machineIndex) AddDial(roster[i].ip, roster[i].gamePort);
    ApplyRole("client from the menu lobby");
    return true;
}
char joinAddress[64] = "";
void survnet::ShowLocal(const char* text) { notice::Enqueue(text); }
// 0 no network (solo or bots), 1 host, 2 client: the randomizer's director runs on 0 and 1 only.
int NetRole() { return !network ? 0 : IsHost() ? 1 : 2; }
#include "runtime_coop_mesh.inc"
#include "runtime_coop_lobby.inc"
#include "runtime_coop_arena.inc"
#include "runtime_coop_debugmenu.inc"
#include "runtime_coop_menu.inc"
#include "runtime_coop_gamemode.inc"
#include "runtime_coop_mpnative.inc"
#include "runtime_coop_nativelobby.inc"
#include "runtime_coop_difficulty.inc"
#include "runtime_coop_mappictures.inc"
#include "runtime_coop_loadout.inc"
#include "runtime_coop_matchend.inc"
#include "runtime_coop_versus.inc"
#include "runtime_coop_progress.inc"
#include "runtime_coop_mpsuit.inc"
#include "runtime_coop_arenarec.inc"
#include "runtime_coop_camtools.inc"
#include "runtime_coop_lifetime.inc"
#include "runtime_coop_zoneobj.inc"
#include "runtime_coop_objonce.inc"

// Game thread. Builds the session the way 0x015EFDD0 does, then connects.
void StartSession() {
    if (session) { InterlockedExchange(&startResult, 4); return; }
    uint32_t services = 0, allocator = 0;
    if (!Read(base + kServices, &services, 4) || !services || !Read(services + 0x8C, &allocator, 4) || !allocator) {
        InterlockedExchange(&startResult, 2); return;
    }
    using GameNew = void* (__cdecl*)(unsigned);
    void* memory = reinterpret_cast<GameNew>(base + kGameNew)(kSessionSize);
    if (!memory) { InterlockedExchange(&startResult, 3); return; }
    using SessionCtor = void* (__thiscall*)(void*, uint32_t, FakePlayer*);
    reinterpret_cast<SessionCtor>(base + kSessionCtor)(memory, allocator, &localPlayer);
    session = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(memory));
    InterlockedExchange(&startResult, 1);
    Connect();
}

// The partner's gun as his ghost carries it, once a second, logged when it
// changes. 24.09. (dump 24692): the ghost's active weapon (+0xCEC) was empty
// and its pending one (+0x424) pointed at a plasma cutter lying next to it
// without owner, out of its inventory (+0xC04). UpdateGhostGunState
// (0x0086BAD0) only runs when the partner's weapon hash changes (16 calls in a
// whole session), so nothing ever put the gun back into his hands.
// GhostGunReequip=1: a ghost with empty hands whose last hash is in its own
// inventory, owned by it, is handed that hash again through the same native
// call (its equip gate, [ghost+0x194]+0xB8, is checked first as it does).
bool ghostGunReequip = false;
DWORD ghostGunWatchAt = 0;
struct GhostGunSeen { uint32_t ghost, signature; DWORD emptySince, reequipAt; };
GhostGunSeen ghostGunSeen[coop_peers::kMaxPeers + kMaxBots]{};
volatile LONG ghostGunLines = 0, ghostGunEmpty = 0, ghostGunReequips = 0, ghostGunReequipFaults = 0, ghostGunNotHeld = 0;
void WatchGhostGuns() {
    if (!weapon_probe::hook) return;
    const DWORD now = GetTickCount();
    if (now - ghostGunWatchAt < 1000) return;
    ghostGunWatchAt = now;
    for (unsigned s = 0; s < coop_peers::kMaxPeers + kMaxBots; ++s) {
        GhostGunSeen& seen = ghostGunSeen[s];
        const bool isBot = s >= coop_peers::kMaxPeers;
        uint32_t machine = kNoMachine;
        if (isBot) {
            if (IsHost()) continue;
            machine = 0xF0u + (s - coop_peers::kMaxPeers);
        } else {
            const auto& partner = peerTable.peers[s];
            if (!partner.used || partner.machine == kNoMachine) continue;
            machine = partner.machine;
        }
        const uint32_t ghost = GhostForMachine(machine);
        if (!ghost) continue;
        const char* who = isBot ? "bot" : "partner";
        const unsigned whoIndex = isBot ? s - coop_peers::kMaxPeers + 1 : s;
        uint32_t view = 0, mapping = 0, active = 0, pending = 0, entries = 0, count = 0;
        Read(ghost + 0x194, &view, 4);
        if (view) Read(view + 0xB8, &mapping, 4);
        Read(ghost + 0xCEC, &active, 4);
        Read(ghost + 0x424, &pending, 4);
        if (!Read(ghost + 0xC08, &entries, 4) || !Read(ghost + 0xC0C, &count, 4) || count > 16) count = 0;
        const uint32_t activeGun = active ? active - 0x10 : 0, pendingGun = pending ? pending - 0x10 : 0;
        weapon_probe::Held last{};
        const bool haveHash = weapon_probe::HeldBy(ghost, last);
        char items[320] = "";
        size_t used = 0;
        uint32_t signature = activeGun * 31u ^ pendingGun * 17u ^ count ^ (mapping ? 0x100u : 0u) ^ (haveHash ? last.hash : 0u);
        bool pendingInInventory = false, hashInInventory = false;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t ref = 0, name = 0, owner = 0;
            unsigned char hide = 0;
            Read(entries + i * 12 + 4, &ref, 4);
            const uint32_t gun = ref ? ref - 0x10 : 0;
            if (gun) { Read(gun + 0x25C, &name, 4); Read(gun + 0x240, &owner, 4); Read(gun + 0x16D, &hide, 1); }
            const uint32_t ownerEntity = owner ? owner - 0x10 : 0;
            if (gun && gun == pendingGun) pendingInInventory = true;
            if (gun && haveHash && name == last.hash && ownerEntity == ghost) hashInInventory = true;
            signature = signature * 33u ^ gun ^ ownerEntity ^ hide;
            if (used < sizeof(items))
                used += std::snprintf(items + used, sizeof(items) - used, " [%u]=%08X/%08X/%s/hide%u", i, gun, name,
                                      !gun ? "-" : !ownerEntity ? "no_owner" : ownerEntity == ghost ? "ghost" : "OTHER_OWNER", hide);
        }
        if (!activeGun) {
            if (!seen.emptySince || seen.ghost != ghost) seen.emptySince = now;
        } else {
            seen.emptySince = 0;
        }
        if (seen.ghost != ghost || seen.signature != signature) {
            seen.ghost = ghost; seen.signature = signature;
            if (InterlockedIncrement(&ghostGunLines) <= 200) {
                uint32_t activeOwner = 0, pendingOwner = 0;
                unsigned char activeHide = 0;
                float ghostPos[3]{}, gunPos[3]{};
                if (activeGun) { Read(activeGun + 0x240, &activeOwner, 4); Read(activeGun + 0x16D, &activeHide, 1); Read(activeGun + 0xC0, gunPos, 12); }
                if (pendingGun) Read(pendingGun + 0x240, &pendingOwner, 4);
                Read(ghost + 0xC0, ghostPos, 12);
                const float dx = gunPos[0] - ghostPos[0], dy = gunPos[1] - ghostPos[1], dz = gunPos[2] - ghostPos[2];
                gate::Event("coop_ghost_gun %s=%u ghost=%08X equip_gate=%d last_hash=%08X active=%08X(owner=%08X hide=%u dist=%.1f) pending=%08X(owner=%08X in_inventory=%d) items=%u%s",
                            who, whoIndex, ghost, view && mapping ? 1 : 0, haveHash ? last.hash : 0u, activeGun, activeOwner ? activeOwner - 0x10 : 0u,
                            activeHide, activeGun ? std::sqrt(dx * dx + dy * dy + dz * dz) : -1.0f, pendingGun,
                            pendingOwner ? pendingOwner - 0x10 : 0u, pendingInInventory ? 1 : 0, count, items);
            }
        }
        if (activeGun || !seen.emptySince || now - seen.emptySince < 1500 || !haveHash || !last.hash) continue;
        InterlockedIncrement(&ghostGunEmpty);
        if (!hashInInventory) { InterlockedIncrement(&ghostGunNotHeld); continue; }
        if (!ghostGunReequip || !view || !mapping || now - seen.reequipAt < 2000) continue;
        seen.reequipAt = now;
        __try {
            weapon_probe::hook.thiscall<void>(reinterpret_cast<void*>(ghost), last.hash, last.ammo, last.plasma);
            InterlockedIncrement(&ghostGunReequips);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&ghostGunReequipFaults);
        }
        uint32_t after = 0;
        Read(ghost + 0xCEC, &after, 4);
        if (InterlockedCompareExchange(&ghostGunReequips, 0, 0) <= 32)
            gate::Event("coop_ghost_gun %s=%u ghost=%08X empty hands for %u ms: hash %08X handed again, active now %08X",
                    who, whoIndex, ghost, static_cast<unsigned>(now - seen.emptySince), last.hash, after ? after - 0x10 : 0u);
    }
}

// The partners' Isaacs where their last state packets put them (fresh ones only),
// count x {x, y, z}: the host's director lets them take a Mercenaries capsule.
int PartnerPositions(float* out, int max) {
    if (!network || !out || max <= 0) return 0;
    const DWORD now = GetTickCount();
    int n = 0;
    for (unsigned i = 0; i < coop_peers::kMaxPeers && n < max; ++i) {
        if (!peerTable.peers[i].used || !peerTable.peers[i].token) continue;
        const auto& r = peerRuntime[i];
        if (!r.lastState || now - r.lastState > 2000) continue;
        if (mpmap::InVisit() && !mpmap::UsableAnchor(r.pos)) continue;   // a falling partner is no place to come back to
        out[n * 3] = r.pos[0];
        out[n * 3 + 1] = r.pos[1];
        out[n * 3 + 2] = r.pos[2];
        ++n;
    }
    return n;
}

// Survival in any mode: an MP map visit, a campaign arena or the director's run.
// Partners come in through the lobby there, so only campaign co-op announces a
// connection (12.10.). A lost partner is announced everywhere.
bool SurvivalSession() {
    return mpmap::InVisit() || arena::state != arena::State::Off || shop::survState != coop_shop::kSurvOff;
}
namespace play {
enum class Kind { Menu, Story, Survival, Versus };
Kind kind = Kind::Menu;
bool VersusMode() { return InterlockedCompareExchange(&mpnative::active, 0, 0) != 0; }
bool SurvivalMode() { return !VersusMode() && SurvivalSession(); }
// A campaign level that is neither (solo story too; with partners = story co-op).
bool StoryCoop() { return inLevel && !SurvivalSession() && !VersusMode(); }
const char* Name(Kind k) { return k == Kind::Story ? "story" : k == Kind::Survival ? "survival" : k == Kind::Versus ? "versus" : "menu"; }
void Update() {
    const Kind now = !inLevel ? Kind::Menu : VersusMode() ? Kind::Versus : SurvivalSession() ? Kind::Survival : Kind::Story;
    if (now == kind) return;
    gate::Event("coop session kind %s -> %s", Name(kind), Name(now));
    kind = now;
}
}  // namespace play
volatile LONG connectNoticesSkipped = 0;

void Poll() {
    if (!installed || faulted) return;
    ExchangePlayerState();
    gate::Tick(GetTickCount());   // repeat votes, release what everybody reached
    laserdiag::Tick(GetTickCount());
    checkpoint::Tick(GetTickCount());
    level::Tick(GetTickCount());
    gamemode::PollRestart();
    streams::Tick(GetTickCount());
    tk::Tick(GetTickCount());
    hits::Tick(GetTickCount());
    loadsync::Tick(GetTickCount());
    story_objects::Tick(GetTickCount());
    removals::Tick(GetTickCount());
    restsync::Tick(GetTickCount());
    regen::Tick(GetTickCount());
    mirror::Tick(GetTickCount());
    worldcalls::DoorWaitTick(GetTickCount());
    ghosttrigger::LiftTick(GetTickCount());
    scenekind::Tick(GetTickCount());   // before the cutscene module and the necro: both ask it
    teleport::Tick(GetTickCount());
    lights::Tick(GetTickCount());
    scenepartner::Tick(GetTickCount());   // before the cutscene module hides the partners
    cutscene::Tick(GetTickCount());
    reghost::Poll();
    aimguard::Tick(GetTickCount());   // drop aiming entries whose objects were freed without their destructor
    disconnect::Poll(GetTickCount());   // a partner silent past the deadline loses its seat, and the engine its connection
    netrate::Poll();
    necro::PollGuarded();   // play as a necromorph: pool, MPLevelSettings, slot-0 hand-over on the key
    catchup::Poll();
    bot::Tick();   // the companion bot: spawn, senses, brain, pad
    botnet::Tick();   // the host's bots registered with the session, so the partners see them
    botnet::SendLoadouts(GetTickCount());   // and the guns they carry, so their ghosts hold them
    botnet::SendBotSuits(GetTickCount());   // and the suits they wear
    turret::Tick();   // the placeable turret
    uselock::Tick();  // machines one player at a time
    gadgets::Tick();   // stasis mine and heal field
    lobby::Tick(GetTickCount());   // the main menu lobby: host announces, joiners follow
    matchend::Tick(GetTickCount());   // the host leaving the match ends it for everybody
    nativelobby::Tick(GetTickCount());   // the game's own lobby screen in front of it ([NativeLobby])
    loadout::Tick(GetTickCount());       // the character setup's unlock data and its saves ([Loadout])
    progress::Tick(GetTickCount());      // XP bookings and weapon kills ([Progress])
    mpsuit::Tick(GetTickCount());        // the MP suits of the suit select ([Loadout] ApplySuit)
    survnet::Tick(GetTickCount()); // the host's survival notices to the partners
    configcheck::Tick(GetTickCount());   // the INI and build compared with every peer
    mesh::Tick(GetTickCount());          // every machine dials every other one; the host's seed
    menu::Tick(GetTickCount());    // a map picked in the main menu's survival entry
    difficulty::Tick(GetTickCount());   // the difficulty screen's pick, the host's value on every machine
    arenarec::Tick(GetTickCount());   // the arena recorder ([Arena] RecordKey)
    arena::Tick(GetTickCount());      // a running campaign arena: doors locked, rooms held
    debugmenu::Tick();                // the debug menu ([Debug] Key, Insert)
    lifetime::Tick(GetTickCount());   // how many corpses and body parts stay ([Lifetime])
    if (!IsHost()) {               // a client's music follows the host's state
        static LONG musicSeen = 0;
        const LONG events = InterlockedCompareExchange(&survnet::musicEvents, 0, 0);
        if (events != musicSeen) {
            musicSeen = events;
            const LONG value = InterlockedCompareExchange(&survnet::musicValue, 0, 0);
            survmusic::SetState(value & 0xFF, value >> 8, "host");
        }
    }
    survmusic::Tick(GetTickCount());
    respawn::Tick(GetTickCount());   // respawn on the MP maps instead of a level restart
    spectate::Tick(GetTickCount());  // story co-op: a dead human watches a partner until the next checkpoint
    clientprog::Tick(GetTickCount());  // a client's own progress per host, Isaac's state across a necro transform
    bugreport::Tick(GetTickCount());   // the bug key's picture of the game thread
    escalation::Tick(GetTickCount());   // the survival escalation's factors for the wave
    mpmap::Tick(GetTickCount());   // a multiplayer map in single player, on the double key press
    shop::Tick(GetTickCount());    // the supply terminal: keys, purchases, the panel's View
    zone::Tick(GetTickCount());    // Hold the Zone: ring, locator line, the partners' copy
    marks::Tick(GetTickCount());   // the zone and the capsules as waypoints on the screen
    npcwatch::Tick(GetTickCount());   // a line for every enemy that appears, tripods followed
    zoneobj::Tick(GetTickCount()); // Hold the Zone: the map's objectives opened, its end dropped
    objonce::Tick(GetTickCount()); // the MP maps' objective chain once per round; a late joiner's catch-up
    mpnative::Tick(GetTickCount());   // Versus: the roster, the local team
    versus::Tick(GetTickCount());     // Versus: MP bits, scores, rounds, respawn points, team groups
    hud::Tick(GetTickCount());     // the survival status line at the top centre
    nameplate::Tick(GetTickCount());   // names over the partners' and bots' heads
    voices::Tick(GetTickCount());      // the voice packs' lines, in 3D
    dementia::Tick(GetTickCount());    // the Dementia mutator: sanity meter and hallucinations, this machine only
    notice::DrainQueued(GetTickCount());   // notices other plugins queued (DS2Coop_QueueNotice)
    helmet::Mirror();
    TriggerLocalDeath();  // acts on a peer death announced by ExchangePlayerState
    ProvisionGhostWeapons();
    WatchGhostGuns();
    npcs::GhostOcclusionTick(GetTickCount());
    if (ghostSkeleton) { CaptureLocalModel(); AttachGhostSkeleton(); }
    if (standin) PollStandin();
    const bool down = startKey && (GetAsyncKeyState(startKey) & 0x8000) != 0;
    const bool pressed = down && !keyWasDown;
    keyWasDown = down;
    inLevel = InLevel();
    static bool wasInLevel = false;
    if (wasInLevel && !inLevel) {
        // What waited for the old world leaves with it: queues, lift holds and the
        // host's journal (mirror::OnRestart), the gate, a partner's death not played
        // yet and restarts asked for there (audit 16.10.).
        mirror::OnRestart();
        mirror::finaleActive = false;
        gate::OnRestart("a level exit");
        InterlockedExchange(&pendingPeerDeath, 0);
        checkpoint::OnLevelExit();
        spectate::OnLevelExit();
    }
    wasInLevel = inLevel;
    const bool settled = LevelSettled(inLevel);
    levelSettled = settled;
    play::Update();
    if (!settled) levelSettledSince = 0;
    else if (!levelSettledSince) levelSettledSince = Stamp(GetTickCount());
    // on an MP map the session starts at the arrival: a necro there has no body
    // until the host's respawn manager reaches it through this very session
    const bool mayStart = IsHost() || !fromEnvironmentRole || level::joinArmed || pressed;
    if (network && mayStart && (pressed || (autoStart && !session && (settled || (inLevel && mpmap::Arrived()))))) {
        InterlockedIncrement(&startRequests);
        StartSession();
    }
    // reconnect per partner, 12 s after the last attempt: in a mesh one live
    // connection must not stop the redials to everybody else
    if (session && dialledCount && GetTickCount() - connectStarted > 12000) {
        uint32_t iface = 0;
        if (Read(session + kSessionInterface, &iface, 4) && iface) {
            using Create = void (__thiscall*)(void*, FakePlayer*);
            unsigned redials = 0;
            for (unsigned i = 0; i < dialledCount; ++i) {
                if (reghost::AnyConnectionTo(dialledIps[i], dialledPorts[i])) continue;
                reinterpret_cast<Create>(base + kCreateConnection)(reinterpret_cast<void*>(iface), &dialledPlayer[i]);
                InterlockedIncrement(&connectAttempts);
                InterlockedIncrement(&partnerRedials);
                ++redials;
            }
            if (redials && InterlockedCompareExchange(&partnerRedials, 0, 0) <= 40)
                gate::Event("coop redial: %u of %u partner(s) without a connection dialled again", redials, dialledCount);
        }
        connectStarted = GetTickCount();
    }
    // The drain that calls this runs many times per frame (17000 calls in
    // 20 s on 15.09.), so the session is ticked at most once every 10 ms.
    static DWORD lastTick = 0;
    const DWORD tickNow = GetTickCount();
    const bool peerConnected = HasConnectedPeer(session);
    if (connected != peerConnected) {
        connected = peerConnected;
        InterlockedIncrement(&connectionTransitions);
        if (!peerConnected) notice::Show("CO-OP PARTNER LOST");
        else if (!SurvivalSession()) notice::Show("CO-OP PARTNER CONNECTED");
        else if (InterlockedIncrement(&connectNoticesSkipped) <= 20) gate::Event("coop partner connected in a survival session: no notice");
        if (peerConnected) ::coop_uniform::Arm();
    }
    // the necro body and its ghost count as ours; the epoch tracks the real replacement, not a death's empty slot
    peerReadiness.UpdateLocal(connected && settled, settled ? localSettle.player : SettleBody());
    SyncLocalReadiness();
    replicationReady = connected && settled && (!requirePeerReady || AnyPeerReady(tickNow));
    tickMode = requestedTickMode == 0 ? 0 : (replicationReady ? requestedTickMode : 1);
    if (session && replicationReady) {
        if (ghostPlayer && !syncAllObjects) RegisterLocalPlayerSafe();
        npcs::Scan();
    }
    if (faulted) return;
    // No TNL in the main menu: a partner still in a level would push its ghosts
    // into the frontend world. The Apt director only exists while the frontend runs.
    uint32_t frontendDirector = 0;
    if (session && Read(base + 0x1C8BE90, &frontendDirector, 4) && frontendDirector) {
        InterlockedIncrement(&frontendTickSkips);
        return;
    }
    if (session && tickMode && tickNow - lastTick >= 10) {
        lastTick = tickNow;
        uint32_t iface = 0;
        using Tick = void (__thiscall*)(void*);
        if (tickMode == 2) {
            SafeCall(reinterpret_cast<Tick>(base + kSessionTick), reinterpret_cast<void*>(session));
            InterlockedIncrement(&ticks);
        } else if (Read(session + kSessionInterface, &iface, 4) && iface) {
            SafeCall(reinterpret_cast<Tick>(base + kCheckIncomingPackets), reinterpret_cast<void*>(iface));
            // Without a connection or a pending one there is nothing to process, and the
            // game's override still runs entity ticks from here, mid-frame. That sent
            // iMsgRunningTick to a Slasher whose state machine was not linked yet (dump
            // 22484, 08.10.: crash at 0x00E93CCF). Incoming connects arrive through
            // CheckIncomingPackets above and show up as pending. On an MP map these
            // are the only entity ticks there are: 12.10. the last partner left a
            // survival run on mp_escape and the world stood still.
            uint32_t open = 0, pending = 0;
            Read(iface + kInterfaceConnectionCount, &open, 4);
            Read(iface + kInterfacePendingCount, &pending, 4);
            if (open || pending || mpmap::InVisit()) SafeCall(reinterpret_cast<Tick>(base + kProcessConnections), reinterpret_cast<void*>(iface));
            else InterlockedIncrement(&idleProcessSkips);
            InterlockedIncrement(&ticks);
        }
        // The ghost is created inside the session tick just run. The engine's
        // per-frame event dispatch (0x756C60, from the main loop after this
        // returns) builds its skinned mesh and faults on the null bones
        // (0x1076765 / 0x106B1D0) before the next Poll could attach. So attach
        // here, same frame, right after creation and before that dispatch.
        if (faulted) return;
        if (ghostSkeleton) AttachGhostSkeleton();
    }
    const DWORD now = GetTickCount();
    if (!session || now - lastReport < 1000 || InterlockedCompareExchange(&reportReady, 0, 0)) return;
    lastReport = now;
    uint32_t singleton = 0, iface = 0, count = 0, array = 0, pending = 0, pendingArray = 0;
    unsigned char ready = 0;
    Read(base + kSessionSingleton, &singleton, 4);
    Read(base + kClassTableReady, &ready, 1);
    Read(session + kSessionInterface, &iface, 4);
    if (iface) {
        Read(iface + kInterfaceConnectionCount, &count, 4); Read(iface + kInterfaceConnectionArray, &array, 4);
        Read(iface + kInterfacePendingCount, &pending, 4); Read(iface + kInterfacePendingArray, &pendingArray, 4);
    }
    size_t used = 0;
    AppendReport(report, sizeof(report), used, "coop session=%08X singleton=%08X class_table=%u iface=%08X tick_mode=%d connected=%d connections=%u pending=%u ticks=%ld idle_process_skips=%ld frontend_skips=%ld redials=%ld sent=%ld received=%ld recv_calls=%ld recv_skipped=%ld send_errors=%ld last_send_error=%ld bind_error=%ld",
                             session, singleton, ready, iface, tickMode, connected, count, pending, InterlockedCompareExchange(&ticks, 0, 0), InterlockedCompareExchange(&idleProcessSkips, 0, 0),
                             InterlockedCompareExchange(&frontendTickSkips, 0, 0), InterlockedCompareExchange(&partnerRedials, 0, 0), InterlockedCompareExchange(&packetsSent, 0, 0),
                             InterlockedCompareExchange(&packetsReceived, 0, 0), InterlockedCompareExchange(&receiveCalls, 0, 0), InterlockedCompareExchange(&receiveSkipped, 0, 0), InterlockedCompareExchange(&sendErrors, 0, 0),
                             InterlockedCompareExchange(&lastSendError, 0, 0), InterlockedCompareExchange(&bindError, 0, 0));
    for (uint32_t i = 0; i < pending && i < 2 && used < sizeof(report) - 40; ++i) {
        uint32_t connection = 0, state = 0;
        if (!Read(pendingArray + 4 * i, &connection, 4) || !connection || !Read(connection + kConnectionState, &state, 4)) break;
        AppendReport(report, sizeof(report), used, " pending%u=%08X state=%u", i, connection, state);
    }
    for (uint32_t i = 0; i < count && i < 4 && used < sizeof(report) - 80; ++i) {
        uint32_t connection = 0, state = 0, ghosts = 0, sequence = 0, local = 0;
        if (!Read(array + 4 * i, &connection, 4) || !connection || !Read(connection + kConnectionState, &state, 4)) break;
        Read(connection + kConnectionGhostArray, &ghosts, 4);
        Read(connection + kConnectionGhostingSequence, &sequence, 4);
        Read(connection + kConnectionLocalGhosts, &local, 4);
        AppendReport(report, sizeof(report), used, " conn%u=%08X state=%u ghost_array=%08X ghosting_seq=%u local_ghosts=%08X",
                              i, connection, state, ghosts, sequence, local);
    }
    // how many NetReflectables the level holds (intrusive list, head at 0x0205B744, next at node+4)
    uint32_t reflectables = 0, node = 0;
    if (Read(base + 0x1C5B744, &node, 4)) {
        uint32_t first = node;
        while (node && reflectables < 4096) {
            ++reflectables;
            if (!Read(node + 4, &node, 4) || node == first) break;
        }
    }
    AppendReport(report, sizeof(report), used, " reflectables=%u game_player_answers=%ld game_player_passed=%ld machine=%u anim_skips=%ld scope_player=%ld scope_npc=%ld scope_drop_npc=%ld scope_drop_other=%ld scope_drop_template=%ld scope_unreadable=%ld scope_allowed=%ld scope_dropped=%ld",
                          reflectables, InterlockedCompareExchange(&gamePlayerAnswers, 0, 0), InterlockedCompareExchange(&gamePlayerPassed, 0, 0), machineIndex, InterlockedCompareExchange(&ghostAnimSkips, 0, 0),
                          InterlockedCompareExchange(&scopeAllowedPlayer, 0, 0),
                          InterlockedCompareExchange(&scopeAllowedNpc, 0, 0),
                          InterlockedCompareExchange(&scopeDroppedNpc, 0, 0),
                          InterlockedCompareExchange(&scopeDroppedOther, 0, 0),
                          InterlockedCompareExchange(&scopeDroppedTemplate, 0, 0),
                          InterlockedCompareExchange(&scopeUnreadable, 0, 0),
                          InterlockedCompareExchange(&scopeAllowed, 0, 0), InterlockedCompareExchange(&scopeDropped, 0, 0));
    AppendReport(report, sizeof(report), used, " ghost_skeleton=%d local_model=%d model_guid=%08X-%08X-%08X-%08X attach_tries=%ld attach_done=%ld attach_missing=%ld attach_faults=%ld bones_filled=%ld model_swaps=%ld ghosts_attached=%u skin_checks=%ld skin_skips=%ld standin=%d standin_entity=%08X adopted=%ld reskins=%ld drives=%ld",
                          ghostSkeleton, localModelKnown, localModelGuid[0], localModelGuid[1], localModelGuid[2], localModelGuid[3],
                          InterlockedCompareExchange(&ghostAttachTries, 0, 0), InterlockedCompareExchange(&ghostAttachDone, 0, 0),
                          InterlockedCompareExchange(&ghostAttachMissing, 0, 0), InterlockedCompareExchange(&ghostAttachFaults, 0, 0),
                          InterlockedCompareExchange(&ghostBonesFilled, 0, 0), InterlockedCompareExchange(&ghostModelSwaps, 0, 0), attachedGhostCount,
                          InterlockedCompareExchange(&skinChecks, 0, 0), InterlockedCompareExchange(&skinSkips, 0, 0),
                          standin, standinEntity, InterlockedCompareExchange(&standinAdopted, 0, 0),
                          InterlockedCompareExchange(&standinReskins, 0, 0), InterlockedCompareExchange(&standinDrives, 0, 0));
    uint32_t playerId = 0;
    const uint32_t player = LocalCampaignPlayer();
    if (player) Read(player + kPlayerId, &playerId, 4);
    AppendReport(report, sizeof(report), used, " players_constructed=%ld attempts=%ld in_level=%d local_player=%08X player_id=%u registered=%08X subkey=%08X proxy=%08X",
                          InterlockedCompareExchange(&placement::playerConstructions, 0, 0), InterlockedCompareExchange(&connectAttempts, 0, 0), inLevel,
                          player, playerId, registeredPlayer, registeredSubKey, registeredProxy);
    AppendReport(report, sizeof(report), used, " connection_transitions=%ld registration_attempts=%ld registration_rejected=%ld settled=%d settle_player=%08X settle_tracking=%d settle_age_ms=%lu load_release=%u settle_adopted=%ld",
                 InterlockedCompareExchange(&connectionTransitions, 0, 0),
                 InterlockedCompareExchange(&registrationAttempts, 0, 0),
                 InterlockedCompareExchange(&registrationRejected, 0, 0), settled,
                 localSettle.player, localSettle.tracking,
                 static_cast<unsigned long>(now - localSettle.since), localSettle.released, InterlockedCompareExchange(&settleAdopted, 0, 0));
    AppendReport(report, sizeof(report), used, " peer_ready_required=%d peer_ready=%d local_epoch=%u remote_epoch=%u ready_sent=%ld ready_recv=%ld data_rejected=%ld receive_backlogs=%ld",
                 requirePeerReady, peerReadiness.Ready(now), peerReadiness.epoch, peerReadiness.peerEpoch,
                 InterlockedCompareExchange(&readinessSent, 0, 0), InterlockedCompareExchange(&readinessReceived, 0, 0),
                 InterlockedCompareExchange(&dataRejected, 0, 0), InterlockedCompareExchange(&receiveBacklogs, 0, 0));
    AppendReport(report, sizeof(report), used, " templates_enabled=%d queued=%ld matched=%ld rejected=%ld missing=%ld full=%ld held=%ld queries_sent=%ld template_log_dropped=%ld",
                 checkEnemyTemplates, InterlockedCompareExchange(&templates::queued, 0, 0),
                 InterlockedCompareExchange(&templates::matched, 0, 0), InterlockedCompareExchange(&templates::rejected, 0, 0),
                 InterlockedCompareExchange(&templates::missing, 0, 0), InterlockedCompareExchange(&templates::full, 0, 0),
                 InterlockedCompareExchange(&templates::held, 0, 0), InterlockedCompareExchange(&templates::sent, 0, 0), InterlockedCompareExchange(&templates::logDropped, 0, 0));
    AppendReport(report, sizeof(report), used, " host_npcs=%d npc_spawn_calls=%ld npc_suppressed=%ld npc_unknown=%ld npc_registered=%ld npc_existing=%ld npc_invalid=%ld npc_log_dropped=%ld npc_scripted_local=%ld npc_coordinators=%ld npc_neutral_local=%ld npc_neutral_not_ghosted=%ld",
                 hostNPCs, InterlockedCompareExchange(&npcs::spawnCalls, 0, 0), InterlockedCompareExchange(&npcs::suppressed, 0, 0),
                 InterlockedCompareExchange(&npcs::unknown, 0, 0), InterlockedCompareExchange(&npcs::registered, 0, 0),
                 InterlockedCompareExchange(&npcs::existing, 0, 0), InterlockedCompareExchange(&npcs::invalid, 0, 0), InterlockedCompareExchange(&npcs::logDropped, 0, 0),
                 InterlockedCompareExchange(&npcs::scriptedAllowed, 0, 0), InterlockedCompareExchange(&npcs::coordinatorsSeen, 0, 0),
                 InterlockedCompareExchange(&npcs::neutralSpawnsLocal, 0, 0), InterlockedCompareExchange(&npcs::neutralsNotGhosted, 0, 0));
    AppendReport(report, sizeof(report), used, " npc_scans=%ld npc_nodes=%ld npc_reject_vtable=%ld npc_reject_getter=%ld npc_reject_rep=%ld npc_reject_body=%ld npc_reject_proxy=%ld npc_reject_slots=%ld npc_reject_key=%ld npc_reps=",
                 InterlockedCompareExchange(&npcs::scanRuns, 0, 0), InterlockedCompareExchange(&npcs::nodesWalked, 0, 0),
                 InterlockedCompareExchange(&npcs::rejectVtable, 0, 0), InterlockedCompareExchange(&npcs::rejectGetter, 0, 0),
                 InterlockedCompareExchange(&npcs::rejectRep, 0, 0), InterlockedCompareExchange(&npcs::rejectBody, 0, 0),
                 InterlockedCompareExchange(&npcs::rejectProxy, 0, 0), InterlockedCompareExchange(&npcs::rejectSlots, 0, 0),
                 InterlockedCompareExchange(&npcs::rejectKey, 0, 0));
    AppendReport(report, sizeof(report), used, " pair_available_calls=%ld pair_defender_waits_fixed=%ld",
                 InterlockedCompareExchange(&npcs::pairAvailableCalls, 0, 0), InterlockedCompareExchange(&npcs::pairDefenderWaitsFixed, 0, 0));
    AppendReport(report, sizeof(report), used, " npc_coordinator_changes=%ld npc_local_class_spawns=%ld npc_local_class_not_ghosted=%ld twins_hidden=%ld twins_shown=%ld twins_gone=%ld twins_full=%ld twin_faults=%ld twins_rehidden=%ld twin_walks_cut=%ld pair_null_skipped=%ld pair_enter_skipped=%ld pair_enter_faults=%ld ghost_gun_lines=%ld ghost_gun_empty=%ld ghost_gun_not_held=%ld ghost_gun_reequips=%ld ghost_gun_reequip_faults=%ld",
                 InterlockedCompareExchange(&npcs::coordinatorChanges, 0, 0),
                 InterlockedCompareExchange(&npcs::localClassSpawns, 0, 0), InterlockedCompareExchange(&npcs::localClassNotGhosted, 0, 0),
                 InterlockedCompareExchange(&npcs::twinsHidden, 0, 0), InterlockedCompareExchange(&npcs::twinsShown, 0, 0),
                 InterlockedCompareExchange(&npcs::twinsGone, 0, 0), InterlockedCompareExchange(&npcs::twinsFull, 0, 0),
                 InterlockedCompareExchange(&npcs::twinFaults, 0, 0), InterlockedCompareExchange(&npcs::twinsRehidden, 0, 0), InterlockedCompareExchange(&npcs::twinWalksCut, 0, 0), InterlockedCompareExchange(&npcs::pairNullSkipped, 0, 0), InterlockedCompareExchange(&npcs::pairEnterSkipped, 0, 0),
                 InterlockedCompareExchange(&npcs::pairEnterFaults, 0, 0), InterlockedCompareExchange(&ghostGunLines, 0, 0),
                 InterlockedCompareExchange(&ghostGunEmpty, 0, 0), InterlockedCompareExchange(&ghostGunNotHeld, 0, 0),
                 InterlockedCompareExchange(&ghostGunReequips, 0, 0), InterlockedCompareExchange(&ghostGunReequipFaults, 0, 0));
    for (unsigned i = 0; i < npcs::seenRepCount; ++i) AppendReport(report, sizeof(report), used, "%s%08X", i ? "," : "", npcs::seenReps[i]);
    if (!npcs::seenRepCount) AppendReport(report, sizeof(report), used, "none");
    const DWORD partnerAge = partnerLastSeen ? now - partnerLastSeen : 0;
    AppendReport(report, sizeof(report), used, " loadout_sync=%d loadout_sent=%ld loadout_recv=%ld local_slots=%ld unreadable=%ld unknown_class=%ld (last vtable RVA %08lX) "
                 "peer_slots=%u peer_filled=%u peer_complete=%d peer_gen=%u",
                 loadoutSync, InterlockedCompareExchange(&loadoutSent, 0, 0),
                 InterlockedCompareExchange(&loadoutReceived, 0, 0),
                 InterlockedCompareExchange(&loadoutSlotsRead, 0, 0),
                 InterlockedCompareExchange(&loadoutUnreadable, 0, 0), InterlockedCompareExchange(&loadoutUnknownClass, 0, 0),
                 static_cast<unsigned long>(InterlockedCompareExchange(&loadoutUnknownVtable, 0, 0)),
                 loadoutReceiver.slots, loadoutReceiver.filled,
                 loadoutReceiver.complete ? 1 : 0, loadoutReceiver.generation);
    AppendReport(report, sizeof(report), used, " bot_loadout_sent=%ld bot_loadout_recv=%ld bot_loadout_rejected=%ld",
                 InterlockedCompareExchange(&botLoadoutSent, 0, 0), InterlockedCompareExchange(&botLoadoutReceived, 0, 0),
                 InterlockedCompareExchange(&botLoadoutRejected, 0, 0));
    for (unsigned i = 0; i < kMaxBots; ++i) {
        const auto& r = botLoadouts[i];
        if (!r.slots && !botLoadoutSenders[i].slots) continue;
        const uint32_t botGhost = IsHost() ? 0 : GhostForMachine(0xF0u + i);
        unsigned real = 0, placeholders = 0;
        for (unsigned k = 0; botGhost && k < r.slots && k < coop_loadout::kMaxSlots; ++k) {
            if (!r.items[k].have) continue;
            const GhostGun g = GhostWeaponState(botGhost, r.items[k].guid);
            if (g == GhostGun::Real) ++real; else if (g == GhostGun::Placeholder) ++placeholders;
        }
        AppendReport(report, sizeof(report), used, " bot%u[sent_slots=%u epoch=%u recv_slots=%u filled=%u gen=%u ghost=%08X real=%u placeholders=%u attempts=%u]",
                     i + 1, botLoadoutSenders[i].slots, botLoadoutSenders[i].epoch, r.slots, r.filled, r.generation, botGhost, real, placeholders,
                     provisionAttempts[coop_peers::kMaxPeers + i]);
    }
    const uint32_t ghostBody = loadoutSync ? FindGhostPlayer() : 0;
    for (unsigned i = 0; i < loadoutReceiver.slots && i < coop_loadout::kMaxSlots; ++i) {
        const auto& entry = loadoutReceiver.items[i];
        if (!entry.have) continue;   // empty slots carry no weapon identity
        uint32_t stream = 0, refs = 0;
        const int quartermaster = FindQuartermasterEntry(entry.guid, stream, refs);
        AppendReport(report, sizeof(report), used,
                     " peer[%u]=%08X-%08X-%08X-%08X qm=%d stream=%08X refs=%u ghost=%s",
                     i, entry.guid[0], entry.guid[1], entry.guid[2], entry.guid[3],
                     quartermaster, stream, refs,
                     !ghostBody ? "none" : GhostWeaponState(ghostBody, entry.guid) == GhostGun::Real ? "gun"
                                : GhostWeaponState(ghostBody, entry.guid) == GhostGun::Placeholder ? "placeholder" : "missing");
    }
    AppendReport(report, sizeof(report), used, " suit_sync=%d suit_sent=%ld suit_recv=%ld suit_local=%08X-%08X-%08X-%08X suit_peer=%08X-%08X-%08X-%08X peer_gen=%u",
                 suitSync, InterlockedCompareExchange(&suitSent, 0, 0),
                 InterlockedCompareExchange(&suitReceived, 0, 0),
                 suitSender.guid[0], suitSender.guid[1], suitSender.guid[2], suitSender.guid[3],
                 suitReceiver.guid[0], suitReceiver.guid[1], suitReceiver.guid[2], suitReceiver.guid[3],
                 suitReceiver.generation);
    AppendReport(report, sizeof(report), used, " create_cancelled=%ld create_cancel_faults=%ld",
                 InterlockedCompareExchange(&provisionCreateCancelled, 0, 0), InterlockedCompareExchange(&provisionCreateCancelFaults, 0, 0));
    AppendReport(report, sizeof(report), used, " provision=%d create=%d tries=%ld accepted=%ld refused=%ld faults=%ld no_placeholder=%ld loads=%ld create_tries=%ld ghost_guns_given=%ld ghost_guns_refused=%ld ghost_gun_faults=%ld ghost_guns_trimmed=%ld trim_faults=%ld held_back=%ld loads_in_flight=%u loads_taken_back=%ld take_back_faults=%ld",
                 provisionWeapons, provisionCreate, InterlockedCompareExchange(&provisionTries, 0, 0),
                 InterlockedCompareExchange(&provisionAccepted, 0, 0),
                 InterlockedCompareExchange(&provisionRefused, 0, 0),
                 InterlockedCompareExchange(&provisionFaults, 0, 0),
                 InterlockedCompareExchange(&provisionNoPlaceholder, 0, 0),
                 InterlockedCompareExchange(&provisionLoads, 0, 0),
                 InterlockedCompareExchange(&provisionCreateTries, 0, 0), InterlockedCompareExchange(&ghostGunsGiven, 0, 0),
                 InterlockedCompareExchange(&ghostGunsRefused, 0, 0), InterlockedCompareExchange(&ghostGunFaults, 0, 0),
                 InterlockedCompareExchange(&ghostGunsTrimmed, 0, 0), InterlockedCompareExchange(&ghostGunTrimFaults, 0, 0),
                 InterlockedCompareExchange(&provisionHeldBack, 0, 0), startedLoadCount, InterlockedCompareExchange(&provisionCancelled, 0, 0),
                 InterlockedCompareExchange(&provisionCancelFaults, 0, 0));
    AppendReport(report, sizeof(report), used, " bodies new=%ld rebuilt_in_place=%ld budget_stops=%ld stream_waits=%ld",
                 InterlockedCompareExchange(&provisionNewBodies, 0, 0), InterlockedCompareExchange(&provisionRebuiltInPlace, 0, 0),
                 InterlockedCompareExchange(&provisionBudgetStops, 0, 0), InterlockedCompareExchange(&ghostGunStreamWaits, 0, 0));
    // The link, per partner: rtt is the smoothed round trip, jitter its
    // smoothed deviation; min and max cover the whole session, so a single
    // spike stays visible instead of being averaged away.
    AppendReport(report, sizeof(report), used,
                 " peers=%u ready_peers=%u learned=%u rejected=%u recycled=%u relay_out=%ld relay_dropped=%ld ping_sent=%ld ping_replied=%ld peer_states=%ld",
                 peerTable.Count(), ReadyPeerCount(GetTickCount()),
                 peerTable.learned, peerTable.rejected, peerTable.recycled,
                 InterlockedCompareExchange(&relayForwarded, 0, 0),
                 InterlockedCompareExchange(&relayDropped, 0, 0),
                 InterlockedCompareExchange(&pingSent, 0, 0),
                 InterlockedCompareExchange(&pingReplied, 0, 0),
                 InterlockedCompareExchange(&peerStatesReceived, 0, 0));
    for (unsigned i = 0; i < coop_peers::kMaxPeers; ++i) {
        const auto& peer = peerTable.peers[i];
        if (!peer.used) continue;
        const auto& runtime = peerRuntime[i];
        const auto& monitor = runtime.link;
        AppendReport(report, sizeof(report), used,
                     " peer%u[%u.%u.%u.%u:%u machine=%d token=%08X cfg=%u relay=%u rtt=%ums jitter=%ums "
                     "min=%ums max=%ums loss=%u%% ready=%d ghost=%08X suit=%08X-%08X gen=%u seq=%u reorder=%u age_ms=%u pos=(%.1f %.1f %.1f) yaw=%.0f]",
                     i, peer.ip & 0xFF, (peer.ip >> 8) & 0xFF, (peer.ip >> 16) & 0xFF,
                     (peer.ip >> 24) & 0xFF, ntohs(peer.port),
                     peer.machine == 0xFFFFFFFFu ? -1 : static_cast<int>(peer.machine),
                     peer.token, peer.configured, peer.relayed,
                     monitor.smoothedRtt, monitor.jitter, monitor.minRtt, monitor.maxRtt,
                     monitor.LossPercent(), runtime.readiness.Ready(GetTickCount()) ? 1 : 0,
                     peer.machine == 0xFFFFFFFFu ? 0u : GhostForMachine(peer.machine),
                     runtime.suit.guid[0], runtime.suit.guid[1], runtime.suit.generation,
                     runtime.lastSequence, runtime.reordered,
                     runtime.lastState ? static_cast<unsigned>(GetTickCount() - runtime.lastState) : 0u,
                     runtime.pos[0], runtime.pos[1], runtime.pos[2], runtime.yaw);
    }
    AppendReport(report, sizeof(report), used, " death_mirror=%d mirrored=%ld mirror_faults=%ld pending=%ld",
                 mirrorDeath, InterlockedCompareExchange(&deathMirrored, 0, 0),
                 InterlockedCompareExchange(&deathMirrorFaults, 0, 0),
                 InterlockedCompareExchange(&pendingPeerDeath, 0, 0));
    AppendReport(report, sizeof(report), used, " death_sync=%d death_local=%ld death_other=%ld death_sent=%ld death_recv=%ld ack_in=%ld ack_out=%ld ack_outstanding=%u abandoned=%ld death_flags=%08X death_args=%08X,%08X,%08X",
                 deathSync, InterlockedCompareExchange(&deathLocalSeen, 0, 0),
                 InterlockedCompareExchange(&deathOtherSeen, 0, 0),
                 InterlockedCompareExchange(&deathSent, 0, 0),
                 InterlockedCompareExchange(&deathReceived, 0, 0),
                 InterlockedCompareExchange(&deathAcksIn, 0, 0),
                 InterlockedCompareExchange(&deathAcksOut, 0, 0),
                 deathSender.Outstanding(),
                 InterlockedCompareExchange(&deathAbandoned, 0, 0),
                 static_cast<unsigned>(InterlockedCompareExchange(&deathLastFlags, 0, 0)),
                 static_cast<unsigned>(InterlockedCompareExchange(&deathLastArgs[0], 0, 0)),
                 static_cast<unsigned>(InterlockedCompareExchange(&deathLastArgs[1], 0, 0)),
                 static_cast<unsigned>(InterlockedCompareExchange(&deathLastArgs[2], 0, 0)));
    AppendReport(report, sizeof(report), used, " state_sent=%ld state_recv=%ld partner=(%.1f %.1f %.1f) partner_yaw=%.0f partner_age_ms=%lu",
                          InterlockedCompareExchange(&statePacketsSent, 0, 0), InterlockedCompareExchange(&statePacketsReceived, 0, 0),
                          partnerPos[0], partnerPos[1], partnerPos[2], partnerYaw, partnerLastSeen ? partnerAge : 0);
    ownership::Capture();
    InterlockedExchange(&reportReady, 1);
}

// --- SimManager's entity creator ---
// Every entity is built by the class-dispatch creator 0x777730
// (thiscall(ecx=SimManager, arg1=spawn request, arg2, arg3), ret 0xC): it reads
// the class descriptor at [[arg1+8]] and calls the class factory at
// descriptor+0x18; for a Player that factory is 0x41A530. Hooked for the
// necromorph mode's MP autospawn block and the read-only necromorph recon.
// The SecondIsaac experiment that also lived here was removed on 07.10.: it
// detected the player spawn through placement::playerObject, which only the
// randomizer's Player ctor hook writes, so in this plugin it could never fire
// (and the 16.09. tests showed a single resident Isaac model instance anyway).
constexpr uint32_t kCreatorRva = 0x377730, kPlayerFactoryRva = 0x1A530;
safetyhook::InlineHook creatorHook;
volatile LONG creatorCalls = 0;

// Necromorph recon (2026-09-21): the creator 0x377730 builds an entity from a
// spawn request (arg1). Spawning a playable NecroPlayer needs to know where in
// that request the class descriptor sits, to clone a Player request and
// swap Player's descriptor (0x02087900) for NecroPlayer's (0x020AAEB0). This logs
// the request layout for the first few Player spawns; it does not spawn anything.
bool necroRecon = false;
volatile LONG necroReconCount = 0;       // how many player-spawn requests captured
volatile LONG necroReconPending = 0;     // a fresh capture waits to be written
constexpr uint32_t kPlayerDescriptor = 0x1C87900;   // base + this = 0x02087900
uint32_t reconCreatorArgs[4]{};          // arg1, arg2, arg3, result of the last player spawn
unsigned char reconCreatorSnap[0x80]{};  // snapshot of arg1[0..0x80], taken on the game thread
// Called on the game thread from OnCreator: snapshot the spawn request so the log
// thread can print it even after the transient request is gone.
void CaptureNecroRecon(uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t result) {
    reconCreatorArgs[0] = arg1; reconCreatorArgs[1] = arg2; reconCreatorArgs[2] = arg3; reconCreatorArgs[3] = result;
    for (uint32_t off = 0; off < sizeof(reconCreatorSnap); off += 4) {
        uint32_t w = 0;
        if (!Read(arg1 + off, &w, 4)) w = 0;
        std::memcpy(reconCreatorSnap + off, &w, 4);
    }
    InterlockedExchange(&necroReconPending, 1);
}
// Called from the diagnostics writer (has the log). Prints the captured request
// and marks any dword that is the Player descriptor or points at a ClassDesc.
void WriteNecroRecon(FILE* log) {
    if (!log || InterlockedCompareExchange(&necroReconPending, 0, 1) != 1) return;
    std::fprintf(log, "necro_recon creator arg1=%08X arg2=%08X arg3=%08X result=%08X (capture %ld)\n",
                 reconCreatorArgs[0], reconCreatorArgs[1], reconCreatorArgs[2], reconCreatorArgs[3],
                 InterlockedCompareExchange(&necroReconCount, 0, 0));
    for (uint32_t off = 0; off < sizeof(reconCreatorSnap); off += 4) {
        uint32_t w = 0; std::memcpy(&w, reconCreatorSnap + off, 4);
        char note[96] = "";
        if (w == base + kPlayerDescriptor) std::snprintf(note, sizeof(note), " <- Player descriptor (swap here for NecroPlayer)");
        else if (w >= base && w < base + 0x3400000) {
            uint32_t vt = 0, np = 0; char nm[40]{};
            if (Read(w, &vt, 4) && vt == base + 0x171E648 && Read(w + 4, &np, 4) && Read(np, nm, sizeof(nm) - 1)) {
                nm[sizeof(nm) - 1] = 0; std::snprintf(note, sizeof(note), " <- ClassDesc \"%s\"", nm);
            }
        }
        std::fprintf(log, "  arg1+0x%02X = %08X%s\n", off, w, note);
    }
}

uint32_t __fastcall OnCreator(uint32_t ecx, uint32_t, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    InterlockedIncrement(&creatorCalls);
    // MP stream level logic the campaign must not host (see generated_mp_autospawn_block.h):
    // answered with no entity, exactly what the native spawn filter does for a rejected request.
    if (necro::ShouldBlock(arg1) || mpmap::ShouldBlock(arg1)) return 0;
    const uint32_t result = creatorHook.thiscall<uint32_t>(reinterpret_cast<void*>(ecx), arg1, arg2, arg3);
    // Necromorph recon: snapshot the first few player-spawn requests to learn
    // where the class descriptor sits in arg1, recognised by the result's vtable.
    if (necroRecon && result && InterlockedCompareExchange(&necroReconCount, 0, 0) < 3) {
        uint32_t rvt = 0;
        if (Read(result, &rvt, 4) && rvt == base + placement::kPlayerPrimaryVtableRva) {
            InterlockedIncrement(&necroReconCount);
            __try { CaptureNecroRecon(arg1, arg2, arg3, result); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    return result;
}

bool InstallCreatorHook(FILE* log, const std::string& ini) {
    necroRecon = GetPrivateProfileIntA("Coop", "NecroRecon", 0, ini.c_str()) != 0;
    if (GetPrivateProfileIntA("Coop", "SecondIsaac", 0, ini.c_str()))
        std::fprintf(log, "coop SecondIsaac=1 ignored: the experiment was removed (it could not trigger in this plugin)\n");
    // necro::ShouldBlock lives here: without the hook the necro pool's MP level
    // logic would spawn freely (crash 28644).
    if (!necro::enabled && !necroRecon && !mpmap::enabled) return false;   // mpmap: the MP map's online logic
    static const unsigned char kHead[] = {0x83,0xEC,0x24,0x56,0x57,0x8B,0x7C,0x24,0x30,0x8B,0x47,0x08,0x8B,0x00,0x8B,0xF1};
    unsigned char actual[sizeof(kHead)]{};
    if (!Read(base + kCreatorRva, actual, sizeof(actual)) || std::memcmp(actual, kHead, sizeof(actual)) != 0) {
        std::fprintf(log, "creator hook signature mismatch at %08X; necro MP autospawn block and necro_recon off\n", static_cast<unsigned>(base + kCreatorRva));
        return false;
    }
    creatorHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kCreatorRva), reinterpret_cast<void*>(OnCreator));
    std::fprintf(log, "creator hook %s at %08X (necro_block=%d necro_recon=%d mpmap_block=%d player_factory=%08X)\n",
                 creatorHook ? "installed" : "FAILED", static_cast<unsigned>(base + kCreatorRva),
                 necro::enabled ? 1 : 0, necroRecon ? 1 : 0, mpmap::enabled ? 1 : 0, static_cast<unsigned>(base + kPlayerFactoryRva));
    return static_cast<bool>(creatorHook);
}

void WriteCreatorDiagnostics(FILE* log) {
    WriteNecroRecon(log);
}

bool Install(FILE* log, const std::string& ini) {
    char env[512]{};   // eight players: seven peers of 16 characters each, plus port and machine
    const bool fromEnvironment = GetEnvironmentVariableA("DS2COOP", env, sizeof(env)) > 0;
    fromEnvironmentRole = fromEnvironment;
    enabled = fromEnvironment || GetPrivateProfileIntA("Coop", "Enabled", 0, ini.c_str()) != 0;
    if (!enabled) return false;
    requirePeerReady = GetPrivateProfileIntA("Coop", "RequirePeerReady", 0, ini.c_str()) != 0;
    checkEnemyTemplates = GetPrivateProfileIntA("Coop", "CheckEnemyTemplates", 0, ini.c_str()) != 0;
    hostNPCs = GetPrivateProfileIntA("Coop", "HostNPCs", 0, ini.c_str()) != 0;
    npcs::enabled = hostNPCs;
    npcs::scriptedLocal = GetPrivateProfileIntA("Coop", "ScriptedSpawnsLocal", 1, ini.c_str()) != 0;
    npcs::scriptedWindowMs = static_cast<DWORD>(GetPrivateProfileIntA("Coop", "ScriptedSpawnWindowMs", 3000, ini.c_str()));
    std::fprintf(log, "coop scripted_spawns_local=%d window_ms=%lu (a client spawns a sequence's actor itself)\n",
                 npcs::scriptedLocal ? 1 : 0, static_cast<unsigned long>(npcs::scriptedWindowMs));
    npcs::neutralsLocal = GetPrivateProfileIntA("Coop", "NeutralNPCsLocal", 0, ini.c_str()) != 0;
    npcs::pairDefenderFix = GetPrivateProfileIntA("Coop", "PairDefenderWaitFix", 1, ini.c_str()) != 0;
    npcs::ghostOcclusion = GetPrivateProfileIntA("Coop", "GhostOcclusion", 0, ini.c_str());
    std::fprintf(log, "coop ghost_occlusion=%d (1: ghost NPCs test sound occlusion by line of sight only, 2: never occluded)\n", npcs::ghostOcclusion);
    std::fprintf(log, "coop neutral_npcs_local=%d (1: every machine keeps its own NeutralNPCs and the host does not ghost them)\n", npcs::neutralsLocal ? 1 : 0);
    {
        char list[256]{};
        GetPrivateProfileStringA("Coop", "LocalNPCClasses", "", list, sizeof(list), ini.c_str());
        char* next = nullptr;
        for (char* token = strtok_s(list, ",; ", &next); token; token = strtok_s(nullptr, ",; ", &next)) {
            bool known = false;
            for (auto& c : npcs::localClasses) if (_stricmp(c.name, token) == 0) { c.on = true; known = true; }
            if (!known) std::fprintf(log, "coop local_npc_classes: unknown class '%s' ignored\n", token);
        }
        for (const auto& c : npcs::localClasses)
            std::fprintf(log, "coop local_npc_class %s=%d (1: every machine spawns and runs its own, the host does not ghost it)\n", c.name, c.on ? 1 : 0);
        hits::nicoleShared = npcs::localClasses[0].on;
        npcs::botFilter = GetPrivateProfileIntA("Bot", "Enabled", 0, ini.c_str()) != 0 && GetPrivateProfileIntA("Bot", "NicoleIgnoresBots", 0, ini.c_str()) != 0;
        std::fprintf(log, "coop nicole_ignores_bots=%d (Nicole never takes a bot as her target: her only attack is a grab the bots refuse)\n",
                     npcs::botFilter ? 1 : 0);
        npcs::InstallLocalTargetFilter(log);
    }
    npcs::InstallNearestTarget(log, ini);
    npcs::hideGhostTwins = GetPrivateProfileIntA("Coop", "HideNeutralGhostTwins", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop hide_neutral_ghost_twins=%d (a client hides the host's NeutralNPC ghost while it has its own copy)\n", npcs::hideGhostTwins ? 1 : 0);
    if (hostNPCs) {
        if (!GetPrivateProfileIntA("Coop", "MPUniformStreams", 0, ini.c_str()) ||
            GetPrivateProfileIntA("Coop", "SyncAllObjects", 0, ini.c_str())) {
            std::fprintf(log, "coop HostNPCs requires MPUniformStreams=1, SyncAllObjects=0; not installed\n");
            return false;
        }
        checkEnemyTemplates = true;
    }
    templates::enabled = checkEnemyTemplates;
    if (checkEnemyTemplates) requirePeerReady = true;
    std::fprintf(log, "coop enemy_template_check=%d capacity=64 reply_ttl_ms=2000\n", checkEnemyTemplates);
    peerReadiness.token = GetTickCount() ^ (GetCurrentProcessId() * 2654435761U);
    if (!peerReadiness.token) peerReadiness.token = 1;
    std::fprintf(log, "coop readiness protocol=1 required=%d token=%08X timeout_ms=3000\n", requirePeerReady, peerReadiness.token);
    // Suit replication. Shares the readiness token so the receiver can pin the
    // source. Only useful together with the uniform module, which does the
    // actual dressing; this flag just enables the transport.
    suitSync = GetPrivateProfileIntA("Coop", "SuitSync", 0, ini.c_str()) != 0;
    if (suitSync && !GetPrivateProfileIntA("Coop", "MPUniformStreams", 0, ini.c_str())) {
        std::fprintf(log, "coop suit_sync requires MPUniformStreams=1; disabled\n");
        suitSync = false;
    }
    suitSender.token = peerReadiness.token;
    std::fprintf(log, "coop suit_sync=%d\n", suitSync);
    // Loadout replication: transport and diagnostics only. Provisioning a
    // missing weapon through RequisitionGun is a separate, later step.
    loadoutSync = GetPrivateProfileIntA("Coop", "LoadoutSync", 0, ini.c_str()) != 0;
    loadoutSender.token = peerReadiness.token;
    std::fprintf(log, "coop loadout_sync=%d slots<=%u\n", loadoutSync, coop_loadout::kMaxSlots);
    // Needs the loadout transport: it arms a ghost from what that ghost's owner
    // carries. (This assignment was missing once; ProvisionWeapons=1 then did
    // nothing and the report said provision=0.)
    provisionWeapons = loadoutSync &&
                       GetPrivateProfileIntA("Coop", "ProvisionWeapons", 0, ini.c_str()) != 0;
    provisionCreate = provisionWeapons &&
                      GetPrivateProfileIntA("Coop", "ProvisionCreate", 0, ini.c_str()) != 0;
    ghostGunReequip = GetPrivateProfileIntA("Coop", "GhostGunReequip", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop ghost_gun_reequip=%d (a ghost with empty hands is handed its last weapon again; needs GhostWeaponProbe=1)\n", ghostGunReequip ? 1 : 0);
    std::fprintf(log, "coop weapon_provision=%d create=%d requisition=%08X quartermaster=%08X\n",
                 provisionWeapons, provisionCreate, static_cast<unsigned>(base + kRequisitionGun),
                 static_cast<unsigned>(base + kQuartermaster));
    // Death mirror, observation stage. Shares the readiness token so the receiver
    // can pin the source. Refuses to install unless the retail prologue matches.
    deathSync = GetPrivateProfileIntA("Coop", "DeathSync", 0, ini.c_str()) != 0;
    if (deathSync) {
        deathSender.token = peerReadiness.token;
        static const unsigned char kDeathHead[] = {0x83,0xEC,0x3C,0x53,0x55,0x56,0x8B,0xF1,0x8B,0x86,0xD8,0x03,0x00,0x00};
        unsigned char actual[sizeof(kDeathHead)]{};
        if (!Read(base + kProcessDeathRva, actual, sizeof(actual)) ||
            std::memcmp(actual, kDeathHead, sizeof(actual)) != 0) {
            std::fprintf(log, "coop death_sync signature mismatch at %08X; observation not installed\n",
                         static_cast<unsigned>(base + kProcessDeathRva));
            deathSync = false;
        } else {
            deathHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kProcessDeathRva),
                                                  reinterpret_cast<void*>(OnProcessDeath));
            if (!deathHook) { std::fprintf(log, "coop death_sync hook FAILED; observation not installed\n"); deathSync = false; }
        }
        std::fprintf(log, "coop death_sync observe=%d process_death=%08X\n",
                     deathSync, static_cast<unsigned>(base + kProcessDeathRva));
    }
    // Opt-in: actually put the local player through the native campaign death
    // when the peer dies, so both restart at the same checkpoint.
    mirrorDeath = deathSync && GetPrivateProfileIntA("Coop", "MirrorDeath", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop death_mirror=%d\n", mirrorDeath);
    // Tell the host about hits it would otherwise never hear about. Observation
    // only: ApplyHits is reserved and the receiver refuses to act on it.
    hits::enabled = GetPrivateProfileIntA("Coop", "ReportHits", 0, ini.c_str()) != 0;
    hits::applyHits = GetPrivateProfileIntA("Coop", "ApplyHits", 0, ini.c_str()) != 0;
    hits::worldHits = GetPrivateProfileIntA("Coop", "WorldHits", 1, ini.c_str()) != 0;
    hits::propHits = GetPrivateProfileIntA("Coop", "WorldPropHits", 0, ini.c_str()) != 0;
    hits::kongShared = GetPrivateProfileIntA("Coop", "ShareTormentorArm", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop share_tormentor_arm=%d (the Tormentor's arm health after a hit reaches every machine's Tormentor)\n", hits::kongShared ? 1 : 0);
    std::fprintf(log, "coop world_prop_hits=%d (hits on props, vents and containers are replayed on every machine)\n", hits::propHits ? 1 : 0);
    localPort = static_cast<unsigned>(GetPrivateProfileIntA("Coop", "Port", 27015, ini.c_str()));
    // DS2COOP_PORT: a second copy of the game on this PC (menu lobby tests) gets
    // its own ports and leaves the role to the main menu, unlike DS2COOP.
    {
        char portText[16]{};
        if (!fromEnvironment && GetEnvironmentVariableA("DS2COOP_PORT", portText, sizeof(portText)) > 0) {
            const unsigned value = static_cast<unsigned>(std::strtoul(portText, nullptr, 10));
            if (value > 1024 && value < 60000) localPort = value;
            DisableInstanceCheck(log);
            std::fprintf(log, "coop DS2COOP_PORT=%s: port %u, the role comes from the main menu\n", portText, localPort);
        }
    }
    startKey = static_cast<int>(GetPrivateProfileIntA("Coop", "StartKey", VK_F9, ini.c_str()));
    autoStart = GetPrivateProfileIntA("Coop", "AutoStart", 1, ini.c_str()) != 0;
    // Off: companion bots only. No session is started, so TNL never runs.
    network = fromEnvironment || GetPrivateProfileIntA("Coop", "Network", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop network=%d%s\n", network ? 1 : 0, network ? "" : " (bots only: no session, no TNL ticks; AutoStart and StartKey ignored)");
    requestedTickMode = static_cast<int>(GetPrivateProfileIntA("Coop", "TickMode", 2, ini.c_str()));
    if (requestedTickMode < 0 || requestedTickMode > 2) requestedTickMode = 2;
    tickMode = requestedTickMode == 0 ? 0 : 1;  // TNL only until connected
    if (static_cast<int>(GetPrivateProfileIntA("Coop", "ClientPlayerId", -1, ini.c_str())) >= 0)
        std::fprintf(log, "coop ClientPlayerId ignored; native PlayerManager owns local slot zero\n");
    syncOnlyPlayers = GetPrivateProfileIntA("Coop", "SyncOnlyPlayers", 1, ini.c_str()) != 0;
    syncAllObjects = GetPrivateProfileIntA("Coop", "SyncAllObjects", 0, ini.c_str()) != 0;
    // Off by default: a ghosted campaign Player has no skeleton on the peer,
    // and the engine's session processing animates it through many paths and
    // faults (the 15.09. 22:xx crash family in 0x0107xxxx). The connection
    // stays up either way; GhostPlayer=1 is the experimental path once the
    // ghost gets a model via onGhostAdd/AttachModel.
    ghostPlayer = GetPrivateProfileIntA("Coop", "GhostPlayer", 0, ini.c_str()) != 0;
    // GhostSkeleton is the real second Isaac: let the engine make the ghost
    // (GhostPlayer), then attach the local resident model so it gets a skeleton
    // and the engine animates it. It needs the ghost to exist, so it implies
    // GhostPlayer=1. Off by default: the connection and data channel are stable
    // without it.
    if (hostNPCs) { ghostPlayer = true; syncOnlyPlayers = false; }
    ghostSkeleton = GetPrivateProfileIntA("Coop", "GhostSkeleton", 0, ini.c_str()) != 0;
    if (GetPrivateProfileIntA("Coop", "MPUniformStreams", 0, ini.c_str())) {
        ghostSkeleton = false;
        std::fprintf(log, "coop MPUniformStreams owns player models; legacy GhostSkeleton bypass disabled\n");
    }
    if (ghostSkeleton) ghostPlayer = true;
    // Stand-in partner: adopt a real creature, reskin to Isaac, freeze, and
    // puppet it to the partner. Independent of the ghost path; needs no ghost.
    standin = GetPrivateProfileIntA("Coop", "Standin", 0, ini.c_str()) != 0;
    // Let the shared Entity::AttachModel hook swap a bare ghost's model for the
    // local resident one at the engine's own attach (see g_ghostModelEntry), and
    // record actors for the stand-in to adopt.
    ::g_ghostModelEntry = GhostModelEntryForAttach;
    ::g_recordActor = RecordActor;
    char peer[64]{};
    GetPrivateProfileStringA("Coop", "Peer", "", peer, sizeof(peer), ini.c_str());
    peerText = peer;
    // the host the main menu's "join co-op" says hello to (ip:port, the host's [Coop] Port)
    GetPrivateProfileStringA("Coop", "JoinAddress", "", joinAddress, sizeof(joinAddress), ini.c_str());
    lobby::build = lobby::BuildId();
    lobby::randomizer = lobby::RandomizerId();   // the randomizer loads before or after us; read again at the first lobby use
    if (fromEnvironment) {
        DisableInstanceCheck(log);
        // port,peer:port[,machine]
        //
        // The third field allows more than two players. Sub-keys are built as
        // (machine << 24) | counter, so every instance needs a machine number of
        // its own; with the old rule (host 0, everybody else 1) three clients
        // would mint the same identity for different objects. Left out, the old
        // rule still applies, so a two-player launcher keeps working.
        const std::string text(env);
        const auto comma = text.find(',');
        localPort = static_cast<unsigned>(std::strtoul(text.c_str(), nullptr, 10));
        std::string rest = comma == std::string::npos ? "" : text.substr(comma + 1);
        const auto second = rest.find(',');
        if (second != std::string::npos) {
            machineOverride = static_cast<int>(std::strtol(rest.c_str() + second + 1, nullptr, 10));
            rest = rest.substr(0, second);
        }
        peerText = rest;
    }
    if (machineOverride < 0)
        machineOverride = GetPrivateProfileIntA("Coop", "MachineIndex", -1, ini.c_str());
    // A list, separated by semicolons: every partner this instance dials.
    // peerIp and peerPort keep meaning "the first one", because the role test
    // (client when there is a peer, host when there is none) and a handful of
    // log lines still read them.
    if (!peerText.empty()) {
        size_t start = 0;
        for (;;) {
            const auto semicolon = peerText.find(';', start);
            const std::string one = semicolon == std::string::npos
                ? peerText.substr(start) : peerText.substr(start, semicolon - start);
            if (!one.empty() && dialledCount < kMaxDialled) {
                unsigned ip = 0, port = 0;
                if (!ParsePeer(one, ip, port)) {
                    std::fprintf(log, "coop peer '%s' is not ip:port; not installed\n", one.c_str());
                    return false;
                }
                dialledIps[dialledCount] = ip;
                dialledPorts[dialledCount] = port;
                if (!dialledCount) { peerIp = ip; peerPort = port; }
                ++dialledCount;
            }
            if (semicolon == std::string::npos) break;
            start = semicolon + 1;
        }
    }
    const unsigned char playerGetterExpected[] = {0x8D,0x81,0x40,0xFC,0xFF,0xFF,0xC3};
    unsigned char playerGetterActual[sizeof(playerGetterExpected)]{};
    if (!Read(base + 0x4A7960, playerGetterActual, sizeof(playerGetterActual)) ||
        std::memcmp(playerGetterActual, playerGetterExpected, sizeof(playerGetterActual))) {
        std::fprintf(log, "coop local_player_layout signature_mismatch; not installed\n");
        return false;
    }
    std::fprintf(log, "coop local_player_layout signature_ok manager_slot=0 baseplayer_offset=3C0\n");
    if (ghostPlayer && !syncAllObjects) {
        struct RegistrationSite { uint32_t rva; const char* head; };
        const RegistrationSite registrationSites[] = {
            {0x3D1D10, "\x56\x57\x8B\x7C\x24\x0C\x57\x8B\xF1\xE8\x22\xF3\x00\x00\x89\x7E"},
            {0x3D1DC0, "\x83\xEC\x0C\x53\x56\x8B\xF1\x33\xDB\x39\x9E\x18\x01\x00\x00\x75"},
            {0x3E10E0, "\x8B\x44\x24\x04\x56\x8B\xF1\x85\xC0\x75\x21\x6A\x54\xE8\xBE\x2B"},
        };
        for (const auto& site : registrationSites) {
            unsigned char bytes[16]{};
            if (!Read(base + site.rva, bytes, sizeof(bytes)) || std::memcmp(bytes, site.head, sizeof(bytes))) {
                std::fprintf(log, "coop registration signature_mismatch va=%08X; not installed\n", static_cast<unsigned>(base + site.rva));
                return false;
            }
            std::fprintf(log, "coop registration signature_ok va=%08X\n", static_cast<unsigned>(base + site.rva));
        }
    }
    struct Site { uint32_t rva; const char* bytes; size_t size; void* fn; safetyhook::InlineHook* hook; };
    const Site sites[] = {
        {kSocketCtor, "\x83\x3D\x38\x8D\x13\x02\x00\x8B\x44\x24\x04\x56\x8B\xF1\x8B\x08", 16, reinterpret_cast<void*>(OnSocketCtor), &ctorHook},
        {kSocketDtor, "\x83\x2D\x38\x8D\x13\x02\x01\x75\x12\x8B\x0D\x50\x8E\x13\x02\x85", 16, reinterpret_cast<void*>(OnSocketDtor), &dtorHook},
        {kSocketSendto, "\x83\xEC\x08\x8B\x44\x24\x0C\x8B\x08\x8B\x50\x04\x8B\x44\x24\x18", 16, reinterpret_cast<void*>(OnSocketSendto), &sendHook},
        {kSocketRecvfrom, "\x8B\x4C\x24\x0C\x8B\x54\x24\x08\x83\xEC\x08\x8D\x04\x24\x50\x51", 16, reinterpret_cast<void*>(OnSocketRecvfrom), &recvHook},
    };
    static const unsigned char kCtorHead[] = {0x8B,0x44,0x24,0x08,0x53,0x55,0x56,0x8B,0xF1,0x89,0x35,0xDC,0x16,0x02,0x02,0x57};
    static const unsigned char kCreateHead[] = {0x83,0xEC,0x08,0x56,0x8B,0xF1,0x57,0x8D,0x4C,0x24,0x08,0xE8,0xE0,0x04,0xBC,0x00};
    unsigned char actual[16]{};
    if (!Read(base + kSessionCtor, actual, 16) || std::memcmp(actual, kCtorHead, 16) != 0 ||
        !Read(base + kCreateConnection, actual, 16) || std::memcmp(actual, kCreateHead, 16) != 0) {
        std::fprintf(log, "coop signature_mismatch at session ctor or CreateConnection; not installed\n");
        return false;
    }
    for (const auto& site : sites) {
        if (!Read(base + site.rva, actual, site.size) || std::memcmp(actual, site.bytes, site.size) != 0) {
            std::fprintf(log, "coop signature_mismatch at %08X; not installed\n", static_cast<unsigned>(base + site.rva));
            return false;
        }
    }
    for (const auto& site : sites) {
        *site.hook = safetyhook::create_inline(reinterpret_cast<void*>(base + site.rva), site.fn);
        if (!*site.hook) {
            for (const auto& undo : sites) undo.hook->reset();
            std::fprintf(log, "coop hook_failed at %08X; not installed\n", static_cast<unsigned>(base + site.rva));
            return false;
        }
    }
    static const unsigned char kSlot15Head[] = {0x56,0x8B,0xF1,0x8B,0x46,0x2C,0x85,0xC0,0x74,0x05,0x83,0xC0,0xF0,0xEB,0x02,0x33};
    static const unsigned char kSlot14Head[] = {0x83,0xEC,0x08,0x56,0x8B,0xF1,0x8B,0x46,0x20,0xC1,0xE8,0x05,0xA8,0x01,0x0F,0x84};
    bool guard15 = false, guard14 = false;
    if (Read(base + kGhostUpdateSlot15, actual, 16) && std::memcmp(actual, kSlot15Head, 16) == 0) {
        ghostAnimHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kGhostUpdateSlot15), reinterpret_cast<void*>(OnGhostUpdate15));
        guard15 = static_cast<bool>(ghostAnimHook);
    }
    if (Read(base + kGhostUpdateSlot14, actual, 16) && std::memcmp(actual, kSlot14Head, 16) == 0) {
        ghostAnimHook2 = safetyhook::create_inline(reinterpret_cast<void*>(base + kGhostUpdateSlot14), reinterpret_cast<void*>(OnGhostUpdate14));
        guard14 = static_cast<bool>(ghostAnimHook2);
    }
    std::fprintf(log, "coop ghost_anim_guard slot15=%d slot14=%d\n", guard15, guard14);
    if (!guard15 || !guard14)
        std::fprintf(log, "coop ghost_anim_guard incomplete; a bare ghost may crash the animation update\n");
    // Skinning guard: only when GhostSkeleton is on (it is a hot path). Verify
    // the function head before hooking.
    bool skinGuard = false;
    if (ghostSkeleton) {
        static const unsigned char kSkinHead[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x0F,0x57,0xC0,0x81,0xEC,0x94,0x00,0x00,0x00,0x53};
        if (Read(base + kSkinBuildRva, actual, 16) && std::memcmp(actual, kSkinHead, 16) == 0) {
            skinHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kSkinBuildRva), reinterpret_cast<void*>(OnSkinBuild));
            skinGuard = static_cast<bool>(skinHook);
        }
        std::fprintf(log, "coop skin_guard=%d (0x%X)\n", skinGuard, static_cast<unsigned>(base + kSkinBuildRva));
    }
    static const unsigned char kScopeHead[] = {0x57,0x8B,0xF9,0x80,0xBF,0x05,0x02,0x00,0x00,0x00,0x0F,0x84,0xD5,0x00,0x00,0x00};
    if (Read(base + kObjectInScope, actual, 16) && std::memcmp(actual, kScopeHead, 16) == 0) {
        scopeHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kObjectInScope), reinterpret_cast<void*>(OnObjectInScope));
        std::fprintf(log, "coop scope_filter %s sync_only_players=%d\n", scopeHook ? "installed" : "hook_failed", syncOnlyPlayers);
    } else {
        std::fprintf(log, "coop scope_filter signature_mismatch; every object would be ghosted\n");
    }
    if ((syncOnlyPlayers || requirePeerReady || checkEnemyTemplates || hostNPCs) && !scopeHook) {
        faulted = true;
        replicationReady = false;
        tickMode = 0;
        std::fprintf(log, "coop required_scope_hook_unavailable players_only=%d peer_ready=%d templates=%d host_npcs=%d; plugin polling disabled\n",
                     syncOnlyPlayers, requirePeerReady, checkEnemyTemplates, hostNPCs);
    }
    for (auto& slot : fakeVtable) slot = reinterpret_cast<void*>(Slot0);
    fakeVtable[1] = reinterpret_cast<void*>(GetMachineIndex);
    fakeVtable[3] = reinterpret_cast<void*>(GetAddress64);
    machineIndex = machineOverride >= 0 && machineOverride <= 255
                       ? static_cast<unsigned>(machineOverride)
                       : (peerPort ? 1u : 0u);
    fakeGamePlayer.player = &localImpl;
    static const unsigned char kGetterHead[] = {0xA1,0xDC,0x13,0x02,0x02,0x8B,0x80,0x94,0x00,0x00,0x00,0x85,0xC0,0x74,0x2C,0x8B};
    if (Read(base + kLocalGamePlayerGetter, actual, 16) && std::memcmp(actual, kGetterHead, 16) == 0) {
        gamePlayerHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kLocalGamePlayerGetter), reinterpret_cast<void*>(OnLocalGamePlayer));
        std::fprintf(log, "coop local_game_player getter %s\n", gamePlayerHook ? "hooked" : "hook_failed");
    } else {
        std::fprintf(log, "coop local_game_player getter signature_mismatch; object sync will register nothing\n");
    }
    localImpl.address.vtable = fakeVtable;
    localImpl.address.address = {htonl(INADDR_LOOPBACK), localPort};
    peerImpl.address.vtable = fakeVtable;
    peerImpl.address.address = {peerIp, peerPort};
    // One dial-out object per partner. They have to survive for the life of the
    // process, because the engine keeps the pointer it is given.
    for (unsigned i = 0; i < dialledCount; ++i) {
        dialledImpl[i].address.vtable = fakeVtable;
        dialledImpl[i].address.address = {dialledIps[i], dialledPorts[i]};
        dialledPlayer[i].impl = &dialledImpl[i];
    }
    botWanted = GetPrivateProfileIntA("Bot", "Enabled", 0, ini.c_str()) != 0;   // before the modules whose hooks the bot needs
    npcs::client = !IsHost();
    if (!faulted && !npcs::Install(log)) {
        faulted = true; replicationReady = false; tickMode = 0;
        std::fprintf(log, "coop HostNPCs installation failed; plugin polling disabled\n");
    }
    hits::Install(log);
    gate::Install(log, ini);
    checkpoint::Install(log, ini);
    level::Install(log, ini);
    teleport::Install(log, ini);
    streams::Install(log, ini);
    mirror::Install(log, ini);
    lights::Install(log, ini);
    reghost::Install(log, ini);
    disconnect::Install(log, ini);
    matchend::Install(log, ini);
    aimguard::Install(log, ini);
    netrate::Install(log, ini);
    notice::Install(log, ini);
    ghosttrigger::Install(log, ini);
    guidfix::Install(log, ini);
    worldcalls::Install(log, ini);
    cutscene::Install(log, ini);
    scenepartner::Install(log, ini);
    persistguard::Install(log, ini);
    renderstall::Install(log, ini);
    tk::Install(log, ini);
    loadsync::Install(log, ini);
    ghostclock::Install(log, ini);
    crashdump::Install(log, ini);
    crashguards::Install(log, ini);
    pausefree::Install(log, ini);
    pausemenu::Install(log, ini);
    focusrate::Install(log, ini);
    removals::Install(log, ini);
    restsync::Install(log, ini);
    regen::Install(log, ini);
    drops::Install(log, ini);
    helmet::Install(log, ini);
    authority::Install(log, ini);
    ghost_probe::Install(log, ini);
    save_guard::Install(log, ini);
    weapon_probe::Install(log, ini);
    necro::Install(log, ini);
    catchup::Install(log, ini);
    bot::Install(botlog::Or(log), ini);
    if (botlog::side.file) std::fprintf(log, "bot lines go to DS2Bots.log (install, events, counters)\n");
    botnet::enabled = GetPrivateProfileIntA("Bot", "GhostToClients", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop bot_ghosts=%d (the host's bots reach the partners as ghost Players, machine byte 0xF0+n)\n", botnet::enabled ? 1 : 0);
    turret::Install(log, ini);
    uselock::Install(log, ini);
    story_objects::Install(log);
    gadgets::Install(log, ini);
    shop::Install(log, ini);
    hud::Install(log, ini);
    nameplate::Install(log, ini);
    voices::Install(log, ini);
    dementia::Install(log, ini);
    mpmap::Install(log, ini);
    menu::Install(log, ini);
    gamemode::Install(log, ini);
    difficulty::Install(log, ini);
    arenarec::Install(log, ini);
    camtools::Install(log, ini);
    lifetime::Install(log, ini);
    arena::Install(log, ini);
    debugmenu::Install(log, ini);
    survmusic::Install(log, ini);
    respawn::Install(log, ini);
    spectate::Install(log, ini);
    clientprog::Install(log, ini);
    bugreport::Install(log, ini);
    objectives::Install(log, ini);
    zone::Install(log, ini);
    marks::Install(log, ini);
    npcwatch::Install(log, ini);
    patrolhelmet::Install(log, ini);
    modcredits::Install(log, ini);
    configcheck::Install(log, ini);
    mesh::Install(log, ini);
    laserdiag::Install(log, ini);
    zoneobj::Install(log, ini);
    objonce::Install(log, ini);
    mpnative::Install(log, ini);
    nativelobby::Install(log, ini);
    mappictures::Install(log, ini);
    loadout::Install(log, ini);
    versus::Install(log, ini);
    progress::Install(log, ini);
    mpsuit::Install(log, ini);
    escalation::Install(log);
    pcmcodec::Install(log, ini);
    bankheap::Install(log, ini);
    outbreakdraw::Install(log, ini);
    InstallCreatorHook(log, ini);   // creator hook: necro MP autospawn block + necromorph recon (read-only)
    OpenDataSocket();
    installed = true;
    std::fprintf(log, "coop data_socket=%s on port %u\n", dataSock != INVALID_SOCKET ? "open" : "FAILED", localPort + kDataPortOffset);
    // Logged loudly, because the failure is otherwise silent. 2026-09-17: two
    // instances were started as host on the same port; the second could not
    // bind its data port and had no side channel (peers=0, ready_sent=0) while
    // the client sent 575 readiness packets to the first host.
    if (dataSock == INVALID_SOCKET) {
        std::fprintf(log,
            "coop ==== NO SIDE CHANNEL ====\n"
            "coop   UDP port %u could not be bound, so this instance will not see any\n"
            "coop   partner: no positions, no suit, no loadout, no death, no ping.\n"
            "coop   Almost always another instance already owns that port - two of them\n"
            "coop   started on the same Port= or the same DS2COOP port. Give each one its\n"
            "coop   own, and exactly one of them should be the host.\n"
            "coop ==========================\n",
            localPort + kDataPortOffset);
    }
    std::fprintf(log, "coop installed port=%u peer=%s role=%s machine=%u (%s) start_key=%d "
                      "auto_start=%d tick_mode=%d ghost_player=%d; TNL socket routed to UDP\n",
                 localPort, peerText.empty() ? "(none, host)" : peerText.c_str(),
                 IsHost() ? "host" : "client", machineIndex,
                 machineOverride >= 0 ? "explicit" : "from role",
                 startKey, autoStart, tickMode, ghostPlayer);
    return true;
}

void FinalizeNPCSetup(FILE* log) {
    if (hostNPCs && (!g_uniformPoll || !g_uniformPreparePose || !scopeHook)) {
        npcs::spawnHook.reset();
        npcs::enabled = false;
        faulted = true; replicationReady = false; tickMode = 0;
        std::fprintf(log, "coop HostNPCs missing uniform/pose/scope dependency; spawn suppression removed, polling disabled\n");
    }
}

void WriteDiagnostics(FILE* log) {
    necro::WriteDiagnostics(log);
    catchup::WriteDiagnostics(log);
    bot::WriteDiagnostics(botlog::Or(log));
    botlog::side.Flush();
    turret::WriteDiagnostics(log);
    uselock::WriteDiagnostics(log);
    gadgets::WriteDiagnostics(log);
    shop::WriteDiagnostics(log);
    hud::WriteDiagnostics(log);
    nameplate::WriteDiagnostics(log);
    voices::WriteDiagnostics(log);
    dementia::WriteDiagnostics(log);
    mpmap::WriteDiagnostics(log);
    menu::WriteDiagnostics(log);
    gamemode::WriteDiagnostics(log);
    difficulty::WriteDiagnostics(log);
    arenarec::WriteDiagnostics(log);
    camtools::WriteDiagnostics(log);
    lifetime::WriteDiagnostics(log);
    arena::WriteDiagnostics(log);
    debugmenu::WriteDiagnostics(log);
    survmusic::WriteDiagnostics(log);
    respawn::WriteDiagnostics(log);
    spectate::WriteDiagnostics(log);
    clientprog::WriteDiagnostics(log);
    bugreport::WriteDiagnostics(log);
    objectives::WriteDiagnostics(log);
    zone::WriteDiagnostics(log);
    marks::WriteDiagnostics(log);
    npcwatch::WriteDiagnostics(log);
    patrolhelmet::WriteDiagnostics(log);
    modcredits::WriteDiagnostics(log);
    configcheck::WriteDiagnostics(log);
    mesh::WriteDiagnostics(log);
    laserdiag::WriteDiagnostics(log);
    zoneobj::WriteDiagnostics(log);
    objonce::WriteDiagnostics(log);
    mpnative::WriteDiagnostics(vlog::Or(log));
    nativelobby::WriteDiagnostics(vlog::Or(log));
    mappictures::WriteDiagnostics(log);
    loadout::WriteDiagnostics(vlog::Or(log));
    versus::WriteDiagnostics(vlog::Or(log));
    progress::WriteDiagnostics(log);
    mpsuit::WriteDiagnostics(log);
    escalation::WriteDiagnostics(log);
    pcmcodec::WriteDiagnostics(log);
    bankheap::WriteDiagnostics(log);
    outbreakdraw::WriteDiagnostics(log);
    lobby::WriteDiagnostics(vlog::Or(log));
    if (network)
        std::fprintf(log, "survnet queued=%ld sent=%ld received=%ld shown=%ld duplicates=%ld reward_events=%ld grants=%ld acknowledged=%ld overflow=%ld last_reward_wave=%ld capsules sent=%ld received=%ld "
                          "taken=%ld overflow=%ld zone sent=%ld received=%ld\n",
                     InterlockedCompareExchange(&survnet::queued, 0, 0), InterlockedCompareExchange(&survnet::sentPackets, 0, 0),
                     InterlockedCompareExchange(&survnet::received, 0, 0), InterlockedCompareExchange(&survnet::shown, 0, 0),
                     InterlockedCompareExchange(&survnet::duplicates, 0, 0), InterlockedCompareExchange(&survnet::rewardEvents, 0, 0),
                     InterlockedCompareExchange(&survnet::rewardGrants, 0, 0), InterlockedCompareExchange(&survnet::rewardAcked, 0, 0),
                     InterlockedCompareExchange(&survnet::rewardOverflow, 0, 0), InterlockedCompareExchange(&survnet::lastRewardWave, 0, 0), InterlockedCompareExchange(&survnet::capsuleSent, 0, 0),
                     InterlockedCompareExchange(&survnet::capsuleReceived, 0, 0), InterlockedCompareExchange(&survnet::capsuleTail, 0, 0),
                     InterlockedCompareExchange(&survnet::capsuleOverflow, 0, 0), InterlockedCompareExchange(&survnet::zoneSent, 0, 0),
                     InterlockedCompareExchange(&survnet::zoneReceived, 0, 0));
    WriteCreatorDiagnostics(log);   // necromorph recon captures
    ghost_probe::Flush(log);
    save_guard::Flush(log);
    weapon_probe::Flush(log);
    templates::Flush(log);
    npcs::Flush(log);
    if (npcs::ghostOcclusion)
        std::fprintf(log, "coop_npcs ghost_occlusion visits=%ld with_component=%ld without=%ld changed=%ld occluded_seen=%ld\n",
                     InterlockedCompareExchange(&npcs::occlusionGhosts, 0, 0), InterlockedCompareExchange(&npcs::occlusionFound, 0, 0),
                     InterlockedCompareExchange(&npcs::occlusionMissing, 0, 0), InterlockedCompareExchange(&npcs::occlusionChanged, 0, 0),
                     InterlockedCompareExchange(&npcs::occlusionOccludedSeen, 0, 0));
    hits::WriteDiagnostics(log);
    gate::WriteDiagnostics(log);
    checkpoint::WriteDiagnostics(log);
    level::WriteDiagnostics(log);
    streams::WriteDiagnostics(log);
    mirror::WriteDiagnostics(log);
    traffic::WriteDiagnostics(log);
    reghost::WriteDiagnostics(log);
    disconnect::WriteDiagnostics(log);
    matchend::WriteDiagnostics(log);
    aimguard::WriteDiagnostics(log);
    netrate::WriteDiagnostics(log);
    notice::WriteDiagnostics(log);
    ghosttrigger::WriteDiagnostics(log);
    guidfix::WriteDiagnostics(log);
    worldcalls::WriteDiagnostics(log);
    cutscene::WriteDiagnostics(log);
    scenepartner::WriteDiagnostics(log);
    persistguard::WriteDiagnostics(log);
    renderstall::WriteDiagnostics(log);
    tk::WriteDiagnostics(log);
    npcs::WriteTargetFilterDiagnostics(log);
    loadsync::WriteDiagnostics(log);
    ghostclock::WriteDiagnostics(log);
    crashdump::WriteDiagnostics(log);
    crashguards::WriteDiagnostics(log);
    rigcolor::WriteDiagnostics(log);
    fxcolor::WriteDiagnostics(log);
    pausefree::WriteDiagnostics(log);
    pausemenu::WriteDiagnostics(log);
    scenekind::WriteDiagnostics(log);
    teleport::WriteDiagnostics(log);
    lights::WriteDiagnostics(log);
    focusrate::WriteDiagnostics(log);
    removals::WriteDiagnostics(log);
    restsync::WriteDiagnostics(log);
    regen::WriteDiagnostics(log);
    drops::WriteDiagnostics(log);
    helmet::WriteDiagnostics(log);
    authority::WriteDiagnostics(log);
    static LONG previousRegistrationStatus = 0;
    const LONG state = InterlockedCompareExchange(&registrationStatus, 0, 0);
    if (state != previousRegistrationStatus) {
        const char* names[] = {"idle", "no_local_player", "unreadable_callbacks", "unreadable_proxy",
                              "unexpected_callbacks", "subkey_allocation_failed", "existing_proxy", "registered", "missing_proxy_stopped"};
        std::fprintf(log, "coop registration state=%s attempts=%ld rejected=%ld\n",
                     state >= 0 && state < 9 ? names[state] : "unknown",
                     InterlockedCompareExchange(&registrationAttempts, 0, 0),
                     InterlockedCompareExchange(&registrationRejected, 0, 0));
        previousRegistrationStatus = state;
    }
    static LONG lastFault = 0;
    const LONG fault = InterlockedCompareExchange(&tickFaultAddress, 0, 0);
    if (fault != lastFault) {
        std::fprintf(log, "coop tick FAULT code=%08lX address=%08lX (rva %08lX); coop ticking stopped\n",
                     InterlockedCompareExchange(&tickFaultCode, 0, 0), fault, fault - static_cast<LONG>(base));
        std::fflush(log);
        lastFault = fault;
    }
    static LONG lastResult = 0;
    const LONG result = InterlockedCompareExchange(&startResult, 0, 0);
    if (result != lastResult) {
        static const char* names[] = {"none", "started", "no_allocator", "no_memory", "already_running"};
        std::fprintf(log, "coop start result=%s session=%08X\n", result >= 0 && result < 5 ? names[result] : "?", session);
        lastResult = result;
    }
    if (InterlockedCompareExchange(&reportReady, 1, 1) == 1) {
        std::fprintf(log, "%s\n", report);
        std::fprintf(log, "%s\n", ownership::report);
        InterlockedExchange(&reportReady, 0);
    }
}
}  // namespace coop
