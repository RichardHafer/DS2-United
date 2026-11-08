// Dead Space 2 United by Richard Hafer.
// Respawn on the MP maps instead of reloading the level ([Respawn] in DS2United.ini).
//
// The native core of the MP respawn, without the MP-only parts (spawn point
// selector, spectator, killer card): the death sequence plays as in single
// player; in ProcessDeath the local Isaac with lives left skips the SP branch
// (the level restart); after DelayMs the MP branch of ProcessDeath runs for him
// (bit 13 of Player+0x3D8 set for that one call), which schedules
// Player::RespawnPlayer (0x00891D70) on the next pre-tick: a new body in the
// same PlayerManager slot. We then put it on a spawn point and give the map's
// loadout. No Blaze or name lookups are on this path (in SP the killer info
// object does not exist). Evidence: analysis/DS2_MP_RESPAWN_FEASIBILITY_2026-10-09.md.
// Who comes back when (coop_respawn_rules.h): solo a life per wave; with another
// human up, after the wave; everybody down, after the director's game over. The
// machines tell each other about their Isaac in a 18-byte LIFE packet. Only a
// failed respawn or an unresolvable GUID falls back to the native ProcessDeath,
// i.e. the restart that the co-op death sync spreads to the partners.
namespace mpnative { bool InVersus(); }   // runtime_coop_mpnative.inc
namespace loadout { int ApplyMode(); const uint32_t* SuitRow(const char** name); }   // runtime_coop_loadout.inc: 1 Mercenaries, 4 Versus, 0 neither
namespace arena {                         // runtime_coop_arena.inc ([Arena] Modes)
bool RespawnHere();
bool RespawnPoint(const float* toward, float* out);
bool Usable(const float* point);
bool RunningNow();
bool FacingFor(const float* pos, float* dir);
extern volatile LONG starts, readyArrivals;
}
namespace lifecycle {                     // runtime_coop_lifecycle.inc, included at the end of this file
bool Install(FILE* log, const std::string& ini);
void Tick(DWORD now);
void WriteDiagnostics(FILE* log);
bool OnBytes(const void* data, int size, DWORD now);
void OnRestartLanded();
bool HoldReturns();
void ArmPlacement(uint32_t player, const float* matrix, const char* why);
uint32_t DisarmPlacement();
bool PlacementReady();
}
namespace respawn {
bool enabled = false, useMpBranch = true, restoreLoadout = true;
// A Versus match respawns its players natively (runtime_coop_versus.inc): this
// module's banner, countdown and death message filter only doubled the wait there.
volatile LONG versusSkips = 0;
bool OffForVersus() { return mpnative::InVersus() && mpmap::InVisit(); }   // the picked mode outlives the visit
// On an MP map, or in a campaign arena with [Arena] Modes: a death here is ours.
bool OnRespawnMap() { return mpmap::visit.phase == coop_mpmap::Phase::Arrived || arena::RespawnHere(); }
int livesPerRun = 1;
DWORD delayMs = 4000, settleMs = 1000, downStuckMs = 45000;
coop_respawn::Dead dead;
float deathPos[3]{};
bool haveDeathPos = false;
LONG rewardsAtDeath = 0;
volatile LONG modeCounts[5]{}, modeChanges = 0, waveReturns = 0, runReturns = 0, stuckReturns = 0, teamDownSeen = 0;
volatile LONG humanDeaths = 0;   // this machine's and the partners' (DS2Coop_HumanDeaths)
constexpr uint32_t kRespawnPlayerRva = 0x491D70, kLinkMsgRva = 0x356BE0, kPreTickTable = 0x1C4D504;
constexpr uint32_t kNetSession = 0x1C216DC, kRemoveDeleteListenerRva = 0x42AA30;
constexpr uint32_t kFlagsMp = 0x2000, kFlagsPending = 0x800, kFlagsProxyWait = 0x1;
// ProcessDeath's SP branch reads damage+0x90 (0x00878C2F) on some flag sets: a
// zeroed stand-in instead of null for the calls we make later.
alignas(16) unsigned char noDamage[0x100]{};
safetyhook::InlineHook respawnHook;
volatile LONG lives = 0, deaths = 0, taken = 0, refusedNoLives = 0, refusedGuid = 0, triggers = 0, respawned = 0, fallbacks = 0, gaveUp = 0;
volatile LONG pending = 0, epoch = 0;   // epoch: one more per completed respawn (the survival director adopts the new body)
volatile LONG armed = 0;                // a death now would respawn: the survival director keeps its waves going
volatile LONG duplicates = 0, nativeFallbacks = 0, listenersRemoved = 0, directCalls = 0, keptDown = 0, freshSpawns = 0;
bool passThrough = false, holdLogged = false;
volatile LONG restartHolds = 0;
uint32_t deadPlayer = 0, newPlayer = 0;
// The host's "respawn the downed now" (debug menu HOST tab): a dead Isaac waiting here,
// or dying within the next 5 s, comes back at once. The tick of the ask, 0 when none.
volatile LONG reviveAskedAt = 0, revivesForced = 0;
DWORD diedAt = 0, triggeredAt = 0, respawnedAt = 0;
// The way back under the black "LOADING" curtain: it rises at the trigger, the new
// body is placed on its point inside RespawnPlayer's hook, and the curtain falls
// once the body stands there (two samples 150 ms apart), 4 s after RespawnPlayer
// at the latest. Before, the new body fell from the map origin on camera.
bool curtainUp = false, placedOnPoint = false;
float placedAt[3]{}, gateSample[3]{};
DWORD gateSampleAt = 0;
int gateReplaces = 0;
volatile LONG curtainsLifted = 0, curtainTimeouts = 0, placedInHook = 0, putOnMapSkipped = 0, gateReplaced = 0;
void LiftRespawnCurtain(const char* why, DWORD now) {
    if (!curtainUp) return;
    curtainUp = false;
    overlay::LiftCurtain();
    InterlockedIncrement(&curtainsLifted);
    gate::Event("respawn: curtain down %lu ms after the trigger (%s)", static_cast<unsigned long>(now - triggeredAt), why);
}
bool TeamAnchor(uint32_t self, float (&out)[3]);   // below
int stage = 0;   // 0 idle, 1 waiting the delay, 2 triggered, 3 respawned (placing), 4 fallback asked
// The death camera hides the HUD, so the countdown is the overlay's banner;
// the RIG notices stay for the moment after.
char deathTitle[48] = "YOU DIED";
DWORD bannerAt = 0;
// The death sequence (PlayerDeathSM) runs animation, DropItemsOnDeath and the
// death screen before it calls ProcessDeath: the countdown starts when the
// health reaches zero, and ProcessDeath may then respawn at once.
uint32_t dyingPlayer = 0;
DWORD dyingAt = 0;

// --- the other machines' Isaacs ---
constexpr uint32_t kLifeMagic = 0x4546494Cu;   // 'LIFE'
constexpr uint8_t kLifeVersion = 1;
#pragma pack(push, 1)
struct LifePacket { uint32_t magic; uint8_t version, machine, life, survival; uint32_t token, seq; uint16_t wave; };
#pragma pack(pop)
static_assert(sizeof(LifePacket) == 18, "life packet size (the receive loop tests for 18)");
constexpr bool LifeSizeFree() {
    for (size_t s : kPacketSizes) if (s == sizeof(LifePacket)) return false;
    return sizeof(LifePacket) != sizeof(survnet::Packet) && sizeof(LifePacket) != sizeof(cutscene::leadaim::Packet) &&
           sizeof(LifePacket) != sizeof(cutscene::leadinput::Packet) && sizeof(LifePacket) != sizeof(hits::WorldPacket) &&
           sizeof(LifePacket) != sizeof(necro::PoolPacket) && sizeof(LifePacket) != sizeof(nameplate::NamePacket);
}
static_assert(LifeSizeFree(), "the life packet shares a size with another co-op packet");
// A checkpoint restart rebuilds the world: a death waiting for its respawn belongs to the old
// one (audit 16.10.: in an arena RespawnHere stays true through a restart, and a stale stage
// spawned a second Isaac or ran the death on the new body).
volatile LONG restartsDropped = 0;
void OnRestart() {
    lifecycle::OnRestartLanded();
    if (stage || dyingPlayer || curtainUp) {
        InterlockedIncrement(&restartsDropped);
        gate::Event("respawn: a checkpoint restart drops stage %d (dead %08X, dying %08X)", stage, deadPlayer, dyingPlayer);
    }
    if (curtainUp) LiftRespawnCurtain("a checkpoint restart", GetTickCount());
    stage = 0;
    dead = coop_respawn::Dead{};
    deadPlayer = 0;
    dyingPlayer = 0;
    InterlockedExchange(&pending, 0);
    overlay::HideBanner();
}
struct OtherIsaac { uint8_t life = 0, survival = 0; DWORD at = 0; uint32_t seq = 0; uint16_t wave = 0; };
OtherIsaac others[16]{};   // by machine index; game thread
constexpr DWORD kLifeFreshMs = 3000;
uint32_t lifeSeq = 0;
DWORD lifeSentAt = 0;
uint8_t lifeSent = 0xFF;
volatile LONG lifeOut = 0, lifeIn = 0, lifeRejected = 0, lifeChanges = 0, teamDownFlag = 0;
unsigned lifeLines = 0;
coop_respawn::Others OthersNow(DWORD now) {
    coop_respawn::Others o;
    if (!network) return o;
    const DWORD fresh = kLifeFreshMs + netclock::OneWay();
    for (unsigned m = 0; m < 16; ++m) {
        if (m == machineIndex || !others[m].at || now - others[m].at > fresh) continue;
        if (others[m].life == coop_respawn::Up || others[m].life == coop_respawn::Returning) ++o.up;
        else if (others[m].life == coop_respawn::Waiting) ++o.waiting;
    }
    return o;
}
// Up through the dying seconds: the death is not decided before ProcessDeath.
coop_respawn::Life LocalLife(bool onMap) {
    if (!onMap) return coop_respawn::Absent;
    if (stage == 0) return coop_respawn::Up;
    if (stage == 1) return coop_respawn::LifeOf(dead.mode);
    return coop_respawn::Returning;   // the respawn is under way
}
// A client's survmusic hears the host's state with survnet kind 3, repeated every 10 s; until then a
// late joiner reads Off. The host's LIFE packet carries the same state every 500 ms.
volatile LONG survivalFromHost = 0;
int SurvivalNow() {
    const int own = static_cast<int>(InterlockedCompareExchange(&survmusic::state, 0, 0));
    if (own || IsHost() || !network || !others[0].at || GetTickCount() - others[0].at > kLifeFreshMs) return own;
    if (others[0].survival) InterlockedIncrement(&survivalFromHost);
    return others[0].survival;
}
void SendLife(DWORD now, bool onMap) {
    if (!network) return;
    const uint8_t life = static_cast<uint8_t>(LocalLife(onMap));
    // In a story level "absent" is said once, not every 5 s (12.10.: 525 each way).
    if (!onMap && life == coop_respawn::Absent && lifeSent == life && lifeSentAt && play::StoryCoop()) return;
    if (life == lifeSent && now - lifeSentAt < (life == coop_respawn::Absent ? 5000u : 500u)) return;
    LifePacket p{kLifeMagic, kLifeVersion, static_cast<uint8_t>(machineIndex), life, static_cast<uint8_t>(SurvivalNow()), peerReadiness.token, ++lifeSeq,
                 static_cast<uint16_t>(InterlockedCompareExchange(&survmusic::wave, 0, 0))};
    if (SendToPeers(&p, sizeof(p)) > 0) InterlockedIncrement(&lifeOut);
    lifeSent = life;
    lifeSentAt = now;
}
bool OnLifeBytes(const void* data, int size, uint32_t now) {
    if (size != static_cast<int>(sizeof(LifePacket))) return false;
    if (lifecycle::OnBytes(data, size, now)) return true;   // 'LFCY' shares the size
    LifePacket p{};
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != kLifeMagic) return false;
    if (p.version != kLifeVersion || !KnownPeerToken(p.token) || p.machine >= 16 || p.machine == machineIndex || p.life > coop_respawn::Waiting) {
        InterlockedIncrement(&lifeRejected);
        return true;
    }
    OtherIsaac& o = others[p.machine];
    if (o.at && now - o.at < 10000 && p.seq <= o.seq && o.seq - p.seq < 1000) return true;   // late and out of order
    InterlockedIncrement(&lifeIn);
    // a partner going down (Mercenaries takes time off for it)
    if (o.at && o.life == coop_respawn::Up && (p.life == coop_respawn::Returning || p.life == coop_respawn::Waiting)) InterlockedIncrement(&humanDeaths);
    if (o.life != p.life) {
        InterlockedIncrement(&lifeChanges);
        if (lifeLines < 80) {
            ++lifeLines;
            static const char* const kNames[] = {"absent", "up", "returning", "waiting"};
            gate::Event("respawn: machine %u's Isaac %s -> %s (its survival state %u, wave %u)", p.machine, kNames[o.life & 3], kNames[p.life & 3], p.survival,
                        p.wave);
        }
    }
    o.life = p.life;
    o.survival = p.survival;
    o.wave = p.wave;
    o.at = now;
    o.seq = p.seq;
    return true;
}

void Countdown(DWORD now) {
    if (now - bannerAt < 200) return;
    bannerAt = now;
    char line[96];
    const char* title = deathTitle;
    const coop_respawn::Others o = OthersNow(now);
    long left = InterlockedCompareExchange(&lives, 0, 0);
    // before ProcessDeath: what the death will be
    const coop_respawn::Mode mode = stage == 1 ? dead.mode : stage == 0 ? coop_respawn::Decide(SurvivalNow(), o, left) : coop_respawn::Mode::None;
    const DWORD from = stage == 1 ? dead.since : dyingAt;
    const DWORD gone = now - from, wait = gone < delayMs ? delayMs - gone : 0;
    const unsigned long seconds = static_cast<unsigned long>((wait + 999) / 1000);
    const bool opened = stage == 1 && dead.clearedAt;
    if (stage == 1 && lifecycle::HoldReturns()) {
        overlay::ShowBanner("GAME OVER", "THE MAP RESTARTS   -   EVERYBODY COMES BACK TOGETHER", 1500);
        return;
    }
    switch (mode) {
    case coop_respawn::Mode::Timed:
        if (wait) std::snprintf(line, sizeof(line), "RESPAWN IN %lu   -   %ld %s LEFT THIS WAVE", seconds, left, left == 1 ? "LIFE" : "LIVES");
        else std::snprintf(line, sizeof(line), "RESPAWNING ...   -   %ld %s LEFT THIS WAVE", left, left == 1 ? "LIFE" : "LIVES");
        break;
    case coop_respawn::Mode::Free:
        if (wait) std::snprintf(line, sizeof(line), "RESPAWN IN %lu", seconds);
        else std::snprintf(line, sizeof(line), "RESPAWNING ...");
        break;
    case coop_respawn::Mode::WaitWave:
        if (opened) std::snprintf(line, sizeof(line), "WAVE CLEARED   -   RESPAWNING ...");
        else std::snprintf(line, sizeof(line), "BACK WHEN THE WAVE IS CLEARED   -   %u STILL FIGHTING", o.up);
        break;
    case coop_respawn::Mode::Down:
        title = o.Known() ? "ALL PLAYERS DOWN" : "OUT OF LIVES";
        if (opened) std::snprintf(line, sizeof(line), "A NEW RUN   -   RESPAWNING ...");
        else std::snprintf(line, sizeof(line), "GAME OVER   -   THE RUN STARTS OVER");
        break;
    default: std::snprintf(line, sizeof(line), "RESPAWNING ..."); break;
    }
    overlay::ShowBanner(title, line, 1500);
}
LONG refillSeen = 0;
unsigned lines = 0;

const char* FamilyOf(uint32_t entity) {
    uint32_t vt = 0;
    if (!entity || !Read(entity, &vt, 4)) return "something";
    struct F { uint32_t vt; const char* name; };
    static const F kFamilies[] = {{0x01B83948, "A SLASHER"}, {0x01B6C400, "A LEAPER"}, {0x01B6CA40, "A LURKER"}, {0x01B8CCA8, "A STALKER"},
                                  {0x01B88B10, "A CRAWLER"}, {0x01B89018, "A PUKER"}, {0x01B84410, "A DIVIDER"}};
    for (const auto& f : kFamilies) if (vt == f.vt) return f.name;
    return "A NECROMORPH";
}
__declspec(noinline) bool ResolvesSafe(uint32_t manager, const uint32_t* guid) {
    necro::SpawnInfo info{};
    __try {
        return necro::GetSpawnInfo(manager, info, guid);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// From PlayerDeathSM::HandleStateMessage (bot::OnDeathSm): on the map every death
// is ours, so the local Isaac keeps his items (0x20 DropItemsOnDeath, which
// emptied his weapon slots before ProcessDeath on 09.10.) and is spared the red
// death screen (0x1E, [Respawn] HideDeathScreen); the respawn puts him back anyway.
bool hideDeathScreen = true;
volatile LONG dropsKept = 0, deathScreensHidden = 0;
bool SwallowDeathMessage(uint32_t owner, uint32_t message) {
    if (!enabled || OffForVersus() || (message != 0x20 && message != 0x1E) || (message == 0x1E && !hideDeathScreen)) return false;
    if (!owner || owner != LocalCampaignPlayer() || IsBotEntity(owner) || !OnRespawnMap()) return false;
    InterlockedIncrement(message == 0x20 ? &dropsKept : &deathScreensHidden);
    gate::Event("respawn: death state message %02X of Isaac %08X swallowed (%s)", message, owner,
                message == 0x20 ? "his items stay with him" : "no red death screen");
    return true;
}
// For the checkpoint module: a death screen's restart while Isaac lives is not wanted here.
bool SuppressDeathRestart() { return enabled && OnRespawnMap(); }
bool Eligible(uint32_t player) {
    return enabled && player && player == LocalCampaignPlayer() && !IsBotEntity(player) && OnRespawnMap();
}

// What the dead body carried. The new body starts from the template (09.10.:
// credits 4000 -> 0, seven inventory stacks -> one), so it gets this back.
struct Stack { uint32_t guid[4]; uint32_t amount; };
struct Saved {
    int32_t credits = -1;
    uint32_t guns = 0;
    Stack items[16]{};
    unsigned count = 0;
    uint32_t player = 0;
    DWORD at = 0;
    uint32_t suit[4]{};   // the suit he wore (a bought one too); the new body follows the suit rule instead
    bool haveSuit = false;
} saved, alive;
volatile LONG restoredGuns = 0, restoredStacks = 0, restoreFailures = 0, restoredCredits = 0;
unsigned ReadStacks(uint32_t player, Stack* out, unsigned cap) {
    uint32_t entries = 0, size = 0;
    if (!Read(player + bot::kGeneralInventory + bot::kInventoryEntries, &entries, 4) || !entries ||
        !Read(player + bot::kGeneralInventory + bot::kInventorySize, &size, 4) || size > 32) return 0;
    unsigned n = 0;
    for (uint32_t i = 0; i < size && n < cap; ++i) {
        uint32_t ref = 0, attrs = 0;
        Stack s{};
        if (!Read(entries + i * bot::kInventoryEntry + 4, &ref, 4) || ref < 0x10010) continue;
        const uint32_t item = ref - 0x10;
        if (!Read(item + 0xC, &attrs, 4) || !attrs || !Read(attrs + bot::kItemSharedGuid, s.guid, 16) || !Read(item + bot::kItemAmount, &s.amount, 4)) continue;
        if (!(s.guid[0] | s.guid[1] | s.guid[2] | s.guid[3]) || !s.amount || s.amount > 100000) continue;
        out[n++] = s;
    }
    return n;
}
// The guns a Player carries, a placeholder whose stream is still loading included.
uint32_t GunsOf(uint32_t player) {
    coop_loadout::Entry entries[coop_loadout::kMaxSlots]{};
    const unsigned n = ReadLocalLoadout(player, entries, coop_loadout::kMaxSlots);
    uint32_t bits = bot::CarriedGuns(player, nullptr, nullptr);
    for (unsigned i = 0; i < n; ++i)
        for (int k = 0; entries[i].have && k < bot::kGunKinds; ++k)
            if (std::memcmp(entries[i].guid, bot::kGunCatalogue[k].guid, 16) == 0) bits |= 1u << k;
    return bits;
}
Saved Take(uint32_t player) {
    Saved s;
    s.player = player;
    s.at = GetTickCount();
    s.credits = shop::Credits(player);
    s.guns = GunsOf(player);
    s.count = ReadStacks(player, s.items, 16);
    const int worn = bot::WornSuit(player, s.suit) ? bot::SuitOfGuid(s.suit) : -1;
    s.haveSuit = worn >= 0 && worn != bot::PatientSuit();
    return s;
}
// At ProcessDeath the death sequence has already dropped the inventory (09.10.:
// guns=0 at every death), so what Isaac carried while alive counts; the credits
// are read again (they are not dropped and may have been spent).
void Snapshot(uint32_t player) {
    const Saved now = Take(player);
    saved = alive.player == player ? alive : now;
    if (now.credits >= 0) saved.credits = now.credits;
    saved.guns |= now.guns;
    if (now.count > saved.count) { std::memcpy(saved.items, now.items, sizeof(saved.items)); saved.count = now.count; }
    if (now.haveSuit) { std::memcpy(saved.suit, now.suit, sizeof(saved.suit)); saved.haveSuit = true; }
}
// A gun the way the map loadout gives the cutter: a GunProxy placeholder into a
// weapon slot, then the Quartermaster's requisition. nullptr when it took.
const char* GiveGun(uint32_t player, int kind) {
    uint32_t qm = 0;
    if (!Read(base + bot::kQuartermasterRva, &qm, 4) || !qm) return "no Quartermaster";
    const uint32_t* guid = bot::kGunCatalogue[kind].guid;
    if (bot::ThisBool0(base + bot::kInventoryIsFullRva, player + bot::kWeaponInventory) != 0) return "weapon inventory full";
    const uint32_t proxy = bot::SpawnPickupRaw(reinterpret_cast<const unsigned char*>(guid));
    uint32_t vtable = 0;
    if (!proxy || !Read(proxy, &vtable, 4) || vtable != base + bot::kGunProxyVtableRva) {
        if (proxy) bot::DeleteRaw(proxy);
        return "no placeholder spawned";
    }
    if (bot::PickupRaw(proxy, base + bot::kItemPickupRva, player) != 1) { bot::DeleteRaw(proxy); return "the placeholder did not go into a slot"; }
    const int ok = bot::RequisitionRaw(qm, player, guid);
    if (ok == 1) return nullptr;
    bot::RemoveItemRaw(player + bot::kWeaponInventory, proxy);
    bot::DeleteRaw(proxy);
    return ok < 0 ? "requisition FAULTED" : "requisition refused";
}
// A stack the store's way, with the amount it had. nullptr when all of it went in.
const char* GiveStack(uint32_t player, const Stack& s, unsigned slot) {
    const uint32_t item = bot::SpawnPickupRaw(reinterpret_cast<const unsigned char*>(s.guid));
    uint32_t vtable = 0, fn = 0;
    if (!item) return "nothing spawned";
    if (!Read(item, &vtable, 4) || vtable < base + gate::kRdataLo || vtable >= base + gate::kRdataHi || !Read(vtable + shop::kPickupSlot, &fn, 4) ||
        fn < base + 0x1000 || fn >= base + gate::kRdataLo) {
        bot::DeleteRaw(item);
        return "no pickup";
    }
    WriteBytes(item + bot::kItemAmount, &s.amount, 4);
    const int whole = bot::PickupRaw(item, fn, player);
    if (whole == 1) return nullptr;
    if (whole < 0) return "pickup faulted";
    return shop::DropNear(player, item, slot) ? "partly put down in front of him" : "lost";
}
// Credits, guns and stacks back on the new body; what its template already
// carries is not given twice.
bool Restore(uint32_t player) {
    if (saved.credits < 0 && !saved.guns && !saved.count) return false;
    if (saved.credits >= 0 && shop::Credits(player) >= 0 && shop::Credits(player) < saved.credits) {
        const int32_t was = shop::Credits(player);
        if (WriteBytes(player + shop::kCredits, &saved.credits, 4)) {
            InterlockedIncrement(&restoredCredits);
            if (bot::leaderCreditsSeen >= 0) bot::leaderCreditsSeen = saved.credits;   // no credits find for the bots' share
            gate::Event("respawn: credits %d -> %d", was, saved.credits);
        }
    }
    const uint32_t has = GunsOf(player);
    for (int k = 0; k < bot::kGunKinds; ++k) {
        if (!(saved.guns & 1u << k) || (has & 1u << k)) continue;
        const char* why = GiveGun(player, k);
        InterlockedIncrement(why ? &restoreFailures : &restoredGuns);
        gate::Event("respawn: %s %s%s", bot::kGunCatalogue[k].name, why ? "NOT given back: " : "given back", why ? why : "");
    }
    Stack now[16]{};
    const unsigned present = ReadStacks(player, now, 16);
    for (unsigned i = 0; i < saved.count; ++i) {
        Stack want = saved.items[i];
        for (unsigned j = 0; j < present && want.amount; ++j) {
            if (std::memcmp(now[j].guid, want.guid, 16) != 0 || !now[j].amount) continue;
            const uint32_t take = now[j].amount < want.amount ? now[j].amount : want.amount;
            now[j].amount -= take;
            want.amount -= take;
        }
        if (!want.amount) continue;
        const char* why = GiveStack(player, want, i);
        InterlockedIncrement(why ? &restoreFailures : &restoredStacks);
        if (why) gate::Event("respawn: stack %08X-%08X x%u: %s", want.guid[0], want.guid[1], want.amount, why);
    }
    gate::Event("respawn: restored credits=%d guns=%X stacks=%u (failures so far %ld)", saved.credits, saved.guns, saved.count,
                InterlockedCompareExchange(&restoreFailures, 0, 0));
    // never without a gun: the map's own loadout when nothing came back
    if (!GunsOf(player)) {
        gate::Event("respawn: the new Isaac has no gun after the restore: the map loadout");
        mpmap::GiveLoadout(player);
    }
    return true;
}
// From OnProcessDeath (game thread): true when this death is ours to handle.
bool TakeDeath(uint32_t player, uint32_t damage) {
    if (passThrough || OffForVersus() || !Eligible(player)) return false;
    InterlockedIncrement(&deaths);
    holdLogged = false;
    if (stage != 0) {
        // our own calls go to the trampoline; a second death message for the same body is swallowed
        if (player == deadPlayer) { InterlockedIncrement(&duplicates); return true; }
        return false;
    }
    // RespawnPlayer spawns from the body's own GUID without a null check: a GUID
    // the SimManager cannot resolve would crash inside native code.
    uint32_t guid[4]{}, manager = 0;
    bool valid = Read(player + 0x24, guid, 16) && Read(base + necro::kSimManager, &manager, 4) && manager && ResolvesSafe(manager, guid);
    if (!valid) {
        InterlockedIncrement(&refusedGuid);
        gate::Event("respawn: Isaac %08X GUID %08X-%08X does not resolve to a spawn info: the native restart instead", player, guid[0], guid[1]);
        return false;
    }
    uint32_t attacker = 0, flags = 0;
    if (damage) Read(damage + 0x80, &attacker, 4);
    // a SafePtr too (entity+0x10); on 10.10. it was Isaac himself as often as not
    attacker = attacker > 0x10010 ? attacker - 0x10 : 0;
    if (!attacker || attacker == player) {
        attacker = 0;
        // the health component's last hit (attacker SafePtr = entity+0x10), as for the corpses in coop_drops
        const uint32_t health = drops::ComponentOf(player, drops::kHealthTypeRva);
        uint32_t last = 0;
        if (health && Read(health + drops::kLastAttacker, &last, 4) && last > 0x10010) attacker = last - 0x10;
    }
    Read(player + 0x3D8, &flags, 4);
    Snapshot(player);
    haveDeathPos = Read(player + mpmap::kPlayerPos, deathPos, sizeof(deathPos));
    deadPlayer = player;
    const DWORD tookAt = GetTickCount();
    diedAt = dyingPlayer == player && tookAt - dyingAt < 60000 ? dyingAt : tookAt;
    if (dyingPlayer == player)
        gate::Event("respawn: ProcessDeath came %lu ms after the health reached zero (alive snapshot %lu ms old)",
                    static_cast<unsigned long>(tookAt - dyingAt), static_cast<unsigned long>(alive.at ? tookAt - alive.at : 0));
    dyingPlayer = 0;
    // who comes back when: the lives, the others, the director's state
    const int survival = SurvivalNow();
    const coop_respawn::Others o = OthersNow(tookAt);
    long left = InterlockedCompareExchange(&lives, 0, 0);
    dead = coop_respawn::Dead{coop_respawn::Decide(survival, o, left), diedAt, 0};
    InterlockedExchange(&lives, left);
    rewardsAtDeath = InterlockedCompareExchange(&survnet::rewardEvents, 0, 0);
    InterlockedIncrement(&modeCounts[static_cast<int>(dead.mode)]);
    if (dead.mode == coop_respawn::Mode::Down) InterlockedIncrement(&refusedNoLives);
    stage = 1;
    InterlockedExchange(&pending, 1);
    InterlockedIncrement(&taken);
    InterlockedIncrement(&humanDeaths);
    const bool known = attacker > 0x10010;
    gate::Event("respawn: Isaac %08X died (flags3D8=%08X, attacker %08X = %s); the level stays, mode %s (survival %d, others up=%u waiting=%u), lives left %ld; "
                "carried credits=%d guns=%X stacks=%u",
                player, flags, attacker, known ? FamilyOf(attacker) : "unknown", coop_respawn::ModeName(dead.mode), survival, o.up, o.waiting, left, saved.credits,
                saved.guns, saved.count);
    if (known) std::snprintf(deathTitle, sizeof(deathTitle), "KILLED BY %s", FamilyOf(attacker));
    else std::snprintf(deathTitle, sizeof(deathTitle), "YOU DIED");
    if (dead.mode == coop_respawn::Mode::Down) notice::Enqueue(o.Known() ? "ALL PLAYERS DOWN" : "OUT OF LIVES");
    bannerAt = 0;
    Countdown(GetTickCount());
    return true;
}

// The suit the new body is born in, by the suit rule (mpmap::RuleSuit): Engineering in survival,
// Objective and Hold the Zone (the punishment for dying) whatever he
// wore or bought; the setup's suit in Mercenaries and Versus.
const uint32_t* BirthSuit(const char*& why) { return mpmap::RuleSuit(why); }
volatile LONG placedByInit = 0, bornDressed = 0;
bool bornInSaved = false, placedNatively = false;
// The point and the suit are handed to the game's own Player::Init inside RespawnPlayer
// ([Lifecycle] NativePlacement), so the new body never stands at the checkpoint or bare.
bool ArmRespawn(uint32_t player, float* matrix, uint32_t& pointObject, int& pointIndex) {
    float toward[3]{};
    if (mpmap::visit.phase == coop_mpmap::Phase::Arrived) {
        const bool haveNear = TeamAnchor(player, toward);
        pointIndex = mpmap::RespawnMatrix(haveNear ? toward : nullptr, matrix, &pointObject);
        return pointIndex >= 0;
    }
    if (!arena::RespawnHere()) return false;
    float point[3]{};
    bool haveNear = haveDeathPos && arena::Usable(deathPos);
    if (haveNear) std::memcpy(toward, deathPos, sizeof(toward));
    else haveNear = TeamAnchor(player, toward);
    if (!arena::RespawnPoint(haveNear ? toward : nullptr, point)) return false;
    // no facing in the arena points: toward the arena's middle (forward row +8/+10, right = up x forward)
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, point[0], point[1] + 0.2f, point[2], 1};
    float face[2]{};
    if (arena::FacingFor(point, face)) {
        m[8] = face[0];
        m[10] = face[1];
        m[0] = face[1];
        m[2] = -face[0];
    }
    std::memcpy(matrix, m, sizeof(m));
    return true;
}
// Player::RespawnPlayer, cdecl(player), plain ret: traced.
void __cdecl OnRespawnPlayer(uint32_t player) {
    const uint32_t before = LocalSlotZeroEntity();
    const bool ours = player == deadPlayer && stage >= 2;
    alignas(16) float matrix[16]{};
    uint32_t pointObject = 0;
    int pointIndex = -1;
    bool pointArmed = false, birth = false;
    const char* suitWhy = "none";
    const uint32_t* suit = nullptr;
    if (ours && lifecycle::PlacementReady()) {
        pointArmed = ArmRespawn(player, matrix, pointObject, pointIndex);
        if (pointArmed) lifecycle::ArmPlacement(0, matrix, "a respawn");
        // a story join's suit (loadsync) owns that same dress call: never both
        if (!play::StoryCoop() && !loadsync::JoinSuitPending()) {
            suit = BirthSuit(suitWhy);
            birth = suit && coop_uniform::ArmBirth(suit, "a respawn");
        }
    }
    respawnHook.ccall<void>(player);
    const uint32_t placedBody = pointArmed ? lifecycle::DisarmPlacement() : 0;
    if (birth) coop_uniform::DisarmBirth(true);
    const uint32_t after = LocalSlotZeroEntity();
    bool adopted = false;
    if (ours) {
        newPlayer = after != player ? after : 0;
        respawnedAt = GetTickCount();
        stage = 3;
        InterlockedIncrement(&respawned);
        // the level stays: no four unsettled seconds (bots letting go, the session pausing for everybody)
        adopted = newPlayer && localSettle.Adopt(newPlayer);
        uint32_t worn[4]{};
        bornInSaved = birth && newPlayer && bot::WornSuit(newPlayer, worn) && std::memcmp(worn, suit, 16) == 0;
        if (bornInSaved) InterlockedIncrement(&bornDressed);
        placedNatively = newPlayer && placedBody == newPlayer;
        if (placedNatively) {
            placedOnPoint = true;
            placedAt[0] = matrix[12];
            placedAt[1] = matrix[13];
            placedAt[2] = matrix[14];
            InterlockedIncrement(&placedInHook);
            InterlockedIncrement(&placedByInit);
            if (pointIndex >= 0) mpmap::SendSpawnUsage(pointObject, pointIndex);
            char facing[96];
            mpmap::FacingText(newPlayer, facing, sizeof(facing));
            gate::Event("respawn: Player::Init placed %08X on %s (%.1f %.1f %.1f), point facing (%.2f %.2f), body %s; birth suit %s (%s); curtain not raised, kit on the next tick",
                        newPlayer, pointIndex >= 0 ? "a spawn point" : "the arena point", matrix[12], matrix[13], matrix[14], matrix[8], matrix[10], facing,
                        bornInSaved ? "worn" : birth ? "NOT worn" : "none", suitWhy);
        } else if (newPlayer && mpmap::visit.phase == coop_mpmap::Phase::Arrived) {
            float toward[3]{};
            const bool haveNear = TeamAnchor(newPlayer, toward);
            placedOnPoint = mpmap::PlaceRespawned(newPlayer, haveNear ? toward : nullptr, placedAt) >= 0;
            if (placedOnPoint) InterlockedIncrement(&placedInHook);
        } else if (newPlayer && arena::RespawnHere()) {
            // the spot nearest to where he died (user, 16.10.); a fall's place of death: the team
            float toward[3]{}, point[3]{};
            bool haveNear = haveDeathPos && arena::Usable(deathPos);
            if (haveNear) std::memcpy(toward, deathPos, sizeof(toward));
            else haveNear = TeamAnchor(newPlayer, toward);
            float face[2]{};
            const bool picked = arena::RespawnPoint(haveNear ? toward : nullptr, point);
            placedOnPoint = picked && mpmap::PlaceAt(newPlayer, point, placedAt, arena::FacingFor(point, face) ? face : nullptr);
            if (placedOnPoint) InterlockedIncrement(&placedInHook);
        }
    }
    else if (spectate::OnRespawned(player, after)) return;
    gate::Event("respawn: RespawnPlayer old=%08X slot0 before=%08X after=%08X settle_adopted=%d placed_by_init=%d", player, before, after, adopted ? 1 : 0,
                placedNatively && ours ? 1 : 0);
}

__declspec(noinline) bool CallOriginal(uint32_t player, bool mpBranch) {
    uint32_t flags = 0;
    Read(player + 0x3D8, &flags, 4);
    if (mpBranch) { const uint32_t set = flags | kFlagsMp; WriteBytes(player + 0x3D8, &set, 4); }
    bool ok = true;
    __try {
        deathHook.thiscall<uint32_t>(reinterpret_cast<void*>(static_cast<uintptr_t>(player)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(noDamage)), 0u, 0u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (mpBranch) {
        uint32_t now = 0;
        Read(player + 0x3D8, &now, 4);
        now &= ~kFlagsMp;
        WriteBytes(player + 0x3D8, &now, 4);
    }
    return ok;
}
// The respawn-pending bit and the pre-tick listener, as ProcessDeath's MP branch
// sets them without a session (0x00878BD9..0x00878BEA). A ghost deletion still
// waited for (bit 0) is dropped first, as the Player cleanup at 0x008AB6AD does.
__declspec(noinline) bool ScheduleDirect(uint32_t player) {
    uint32_t flags = 0, netSession = 0;
    if (!Read(player + 0x3D8, &flags, 4)) return false;
    if (flags & kFlagsPending) return true;   // OnProxyDeleted got there first
    if ((flags & kFlagsProxyWait) && Read(base + kNetSession, &netSession, 4) && netSession) {
        __try {
            reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t)>(base + kRemoveDeleteListenerRva)(
                reinterpret_cast<void*>(static_cast<uintptr_t>(netSession)), player + 0x5C, player + 0x16E4, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        flags &= ~kFlagsProxyWait;
        InterlockedIncrement(&listenersRemoved);
    }
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t)>(base + kLinkMsgRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(player)),
                                                                                       base + kPreTickTable, 0xFFFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    flags |= kFlagsPending;
    return WriteBytes(player + 0x3D8, &flags, 4);
}

// The call Player::HandleEvents makes on the pre-tick when the pending bit is set
// (0x008AC008), made here through our hook. 13.10.: a client dead for two minutes
// through a wave got both schedules and no RespawnPlayer; the pre-tick never
// reached the old body. The pending bit goes first, so a late pre-tick does not
// respawn it a second time.
__declspec(noinline) bool RespawnDirect(uint32_t player) {
    uint32_t flags = 0;
    if (Read(player + 0x3D8, &flags, 4)) {
        flags &= ~kFlagsPending;
        WriteBytes(player + 0x3D8, &flags, 4);
    }
    __try {
        reinterpret_cast<void(__cdecl*)(uint32_t)>(base + kRespawnPlayerRva)(player);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Out of options: the native death through our own hook, so the death sync
// tells the partners as for any other restart.
__declspec(noinline) void NativeDeath(uint32_t player) {
    InterlockedIncrement(&nativeFallbacks);
    passThrough = true;
    __try {
        reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t, uint32_t, uint32_t)>(base + kProcessDeathRva)(
            reinterpret_cast<void*>(static_cast<uintptr_t>(player)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(noDamage)), 0u, 0u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gate::Event("respawn: the native death call FAULTED");
    }
    passThrough = false;
}

// Where to come back: the living teammate (partner or bot) nearest to the place
// of death, else that place. A partner whose machine says it waits is no anchor.
bool TeamAnchor(uint32_t self, float (&out)[3]) {
    uint32_t players[8]{};
    const unsigned n = nameplate::Targets(players, 8);
    float best = 1e30f;
    bool found = false;
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t p = players[i];
        if (p == self || p == deadPlayer) continue;
        const uint32_t health = hits::FindHealthComponent(p);
        float left = 0.0f;
        if (health && Read(health + hits::kHealthCurrent, &left, 4) && left <= 0.0f) continue;
        const uint32_t machine = MachineOf(p);
        if (machine < 16 && others[machine].life == coop_respawn::Waiting) continue;
        float at[3]{};
        if (!Read(p + mpmap::kPlayerPos, at, sizeof(at)) || !mpmap::UsableAnchor(at)) continue;   // a falling partner is no anchor
        float d = 0.0f;
        if (haveDeathPos) {
            const float dx = at[0] - deathPos[0], dz = at[2] - deathPos[2];
            d = dx * dx + dz * dz;
        }
        if (!found || d < best) { best = d; found = true; std::memcpy(out, at, sizeof(out)); }
    }
    // a fall's place of death lies far below the map (13.10.: y -442): no anchor
    if (!found && haveDeathPos && mpmap::UsableAnchor(deathPos)) { std::memcpy(out, deathPos, sizeof(out)); found = true; }
    return found;
}

// A body off its place: onto the arena point nearest to `toward`, or an MP spawn point.
void PutBack(uint32_t player, const float* toward) {
    float point[3]{};
    if (arena::RespawnHere()) {
        const bool moved = arena::RespawnPoint(toward, point) && bot::MoveEntity(player, coop_bot::Vec3{point[0], point[1], point[2]});
        gate::Event("respawn: %08X %s the arena point (%.1f %.1f %.1f)", player, moved ? "moved to" : "NOT moved to", point[0], point[1], point[2]);
        return;
    }
    mpmap::PutOnMap(player, true, true, toward);
}

void Finish(uint32_t player, DWORD now) {
    float at[3]{};
    const bool stays = placedOnPoint && Read(player + mpmap::kPlayerPos, at, sizeof(at)) &&
                       (at[0] - placedAt[0]) * (at[0] - placedAt[0]) + (at[2] - placedAt[2]) * (at[2] - placedAt[2]) <= 1.5f * 1.5f &&
                       std::fabs(at[1] - placedAt[1]) < 3.0f;
    if (stays) {
        InterlockedIncrement(&putOnMapSkipped);
        gate::Event("respawn: %08X still stands on the point it was placed on in RespawnPlayer's hook (%.1f %.1f %.1f)", player, at[0], at[1], at[2]);
    } else {
        float toward[3]{};
        const bool haveNear = TeamAnchor(player, toward);
        gate::Event("respawn: back near %s (%.1f %.1f %.1f)%s", haveNear ? "the team or the place of death" : "nothing known", toward[0], toward[1], toward[2],
                    placedOnPoint ? "; the body left the point it was placed on" : "");
        PutBack(player, haveNear ? toward : nullptr);
    }
    gateSampleAt = 0;
    if (restoreLoadout && !Restore(player)) mpmap::GiveLoadout(player, true);
    const char* ruleWhy = "";
    const uint32_t* ruleSuit = mpmap::RuleSuit(ruleWhy);
    if (bornInSaved) {
        gate::Event("respawn: %08X was born in its suit (%s); no change", player, ruleWhy);
    } else if (loadout::ApplyMode() && !ruleSuit) {
        gate::Event("respawn: the character setup's MP colour again on %08X", player);
        mpmap::GiveLoadoutSuit(player);
    } else if (ruleSuit) {
        uint32_t worn[4]{};
        const bool already = bot::WornSuit(player, worn) && std::memcmp(worn, ruleSuit, 16) == 0;
        const bool dressed = !already && coop_uniform::BotDress(player, ruleSuit);
        gate::Event("respawn: %08X suit by %s: %s (the birth suit did not take)", player, ruleWhy, already ? "already worn" : dressed ? "changed now" : "change FAILED");
    }
    if (mpmap::enableModules) mpmap::EnableModules(player);
    uint32_t body[3]{};
    const bool complete = coop_uniform::BotBody(player, body);
    if (!complete && mpmap::dressIfBare) coop_uniform::BotDress(player, mpmap::dressSuit);
    if (mpmap::ourPlayer == deadPlayer) mpmap::ourPlayer = player;
    InterlockedIncrement(&epoch);
    InterlockedExchange(&pending, 0);
    stage = 0;
    bornInSaved = placedNatively = false;
    // only a death that took a life has a count to tell (13.10.: the clock modes and the
    // breaks respawn for free and said "1 LIFE LEFT" every time)
    const bool counted = dead.mode == coop_respawn::Mode::Timed;
    dead = coop_respawn::Dead{};
    overlay::HideBanner();
    char text[64];
    if (counted)
        std::snprintf(text, sizeof(text), "RESPAWNED - %ld %s LEFT", InterlockedCompareExchange(&lives, 0, 0),
                      InterlockedCompareExchange(&lives, 0, 0) == 1 ? "LIFE" : "LIVES");
    else
        std::snprintf(text, sizeof(text), "RESPAWNED");
    notice::Enqueue(text);
    gate::Event("respawn: new Isaac %08X placed and equipped %lu ms after the death (epoch %ld, body %s)", player, static_cast<unsigned long>(now - diedAt),
                InterlockedCompareExchange(&epoch, 0, 0), complete ? "complete" : "was bare");
}

// Game thread, from Poll.
// The engine deletes a body that fell out of the world (13.10.: the client's,
// about two minutes before his wave ended; the death handler then ran on freed
// memory and found no new Isaac). Whatever is still to be done for that body is
// done for a fresh one from the player template instead.
bool BodyGone() {
    uint32_t vtable = 0;
    return !deadPlayer || !Read(deadPlayer, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva;
}
void SpawnFresh(DWORD now, const char* when) {
    const uint32_t fresh = mpmap::SpawnPlayer();
    gate::Event("respawn: the dead body %08X is gone (%s): a fresh Isaac from the player template: %08X", deadPlayer, when, fresh);
    placedOnPoint = false;   // SpawnPlayer placed it; Finish checks it
    if (!fresh) {
        stage = 0;
        InterlockedExchange(&pending, 0);
        LiftRespawnCurtain("no fresh Isaac", now);
        overlay::ShowBanner("RESPAWN FAILED", "RESTART THE CHECKPOINT FROM THE PAUSE MENU", 10000);
        return;
    }
    InterlockedIncrement(&freshSpawns);
    InterlockedIncrement(&respawned);
    newPlayer = fresh;
    respawnedAt = now;
    localSettle.Adopt(fresh);
    Finish(fresh, now);
}

// Under the curtain: down once the new body stands on the map; a body found falling or
// in the origin's column is put back (twice at most).
void CurtainTick(DWORD now, bool onMap) {
    if (!onMap) { LiftRespawnCurtain("the map went away", now); return; }
    if (stage == 1 || stage == 2 || stage == 4 || stage == 5) return;   // until RespawnPlayer the curtain keeps its own 8 s limit
    // signed: RespawnPlayer's hook stamps a fresh tick
    if (respawnedAt && static_cast<LONG>(now - respawnedAt) >= 4000) { InterlockedIncrement(&curtainTimeouts); LiftRespawnCurtain("4 s after RespawnPlayer, standing or not", now); return; }
    if (stage != 0) return;   // Finish has not run yet
    const uint32_t body = LocalCampaignPlayer();
    float at[3]{};
    if (!body || body == deadPlayer || !Read(body + mpmap::kPlayerPos, at, sizeof(at))) return;
    if (!mpmap::UsableAnchor(at)) {
        gateSampleAt = 0;
        if (gateReplaces < 2) {
            ++gateReplaces;
            InterlockedIncrement(&gateReplaced);
            float toward[3]{};
            const bool haveNear = TeamAnchor(body, toward);
            gate::Event("respawn: the new body %08X fell to (%.1f %.1f %.1f) under the curtain: put back (%d)", body, at[0], at[1], at[2], gateReplaces);
            PutBack(body, haveNear ? toward : nullptr);
        }
        return;
    }
    if (!gateSampleAt) { std::memcpy(gateSample, at, sizeof(at)); gateSampleAt = now; return; }
    if (now - gateSampleAt < 150) return;
    const float dx = at[0] - gateSample[0], dy = at[1] - gateSample[1], dz = at[2] - gateSample[2];
    std::memcpy(gateSample, at, sizeof(at));
    gateSampleAt = now;
    if (dx * dx + dz * dz < 0.09f && std::fabs(dy) < 0.5f) LiftRespawnCurtain("the new body stands", now);
}

void Tick(DWORD now) {
    lifecycle::Tick(now);
    if (!enabled) return;
    if (OffForVersus()) {
        if (stage || dyingPlayer) { stage = 0; dyingPlayer = 0; InterlockedExchange(&pending, 0); overlay::HideBanner(); InterlockedIncrement(&versusSkips); }
        return;
    }
    // a new visit fills the lives; so does every cleared wave (the reward events)
    static uint32_t visitEpoch = 0xFFFFFFFFu;
    static LONG arenaEpoch = -1;
    const LONG arenaStarts = InterlockedCompareExchange(&arena::starts, 0, 0);
    if (mpmap::visit.epochs != visitEpoch || arenaStarts != arenaEpoch) {
        visitEpoch = mpmap::visit.epochs;
        arenaEpoch = arenaStarts;
        InterlockedExchange(&lives, livesPerRun);
        refillSeen = InterlockedCompareExchange(&survnet::rewardEvents, 0, 0);
    }
    const LONG refills = InterlockedCompareExchange(&survnet::rewardEvents, 0, 0);
    if (refills != refillSeen) {
        refillSeen = refills;
        if (InterlockedExchange(&lives, livesPerRun) != livesPerRun && lines < 40) { ++lines; gate::Event("respawn: wave cleared, lives back to %d", livesPerRun); }
    }
    // and so does a new run (after a game over)
    const int survival = SurvivalNow();
    static int survivalSeen = 0;
    if (survival != survivalSeen) {
        if (survival == coop_respawn::Arming && InterlockedExchange(&lives, livesPerRun) != livesPerRun && lines < 40) {
            ++lines;
            gate::Event("respawn: a new run arms, lives back to %d", livesPerRun);
        }
        survivalSeen = survival;
    }
    const bool onMap = OnRespawnMap();
    if (curtainUp) CurtainTick(now, onMap);
    // every death on the map is ours: the director keeps its run through it
    InterlockedExchange(&armed, onMap ? 1 : 0);
    SendLife(now, onMap);
    const coop_respawn::Others o = OthersNow(now);
    const bool teamDown = onMap && coop_respawn::TeamDown(LocalLife(onMap), o);
    if (InterlockedExchange(&teamDownFlag, teamDown ? 1 : 0) != (teamDown ? 1 : 0) && teamDown) {
        InterlockedIncrement(&teamDownSeen);
        gate::Event("respawn: every human on the map is down (others waiting=%u, survival %d): the host's director ends the run", o.waiting, survival);
    }
    if (stage == 0) {
        const uint32_t local = onMap ? LocalCampaignPlayer() : 0;
        if (!local || IsBotEntity(local)) { if (dyingPlayer) { dyingPlayer = 0; overlay::HideBanner(); } return; }
        const bool living = checkpoint::LocalIsaacAlive();
        if (living) {
            if (dyingPlayer) { dyingPlayer = 0; overlay::HideBanner(); }   // not dead after all
            static DWORD aliveAt = 0;
            if (now - aliveAt >= 500 || alive.player != local) { aliveAt = now; alive = Take(local); }
            return;
        }
        if (alive.player != local) return;   // never seen alive: an unreadable health is no death
        if (dyingPlayer != local) {
            dyingPlayer = local;
            dyingAt = now;
            std::snprintf(deathTitle, sizeof(deathTitle), "YOU DIED");
            bannerAt = 0;
            gate::Event("respawn: Isaac %08X has no health left: countdown from now (alive snapshot guns=%X stacks=%u credits=%d)", local, alive.guns,
                        alive.count, alive.credits);
        }
        Countdown(now);
        return;
    }
    if (!onMap) { stage = 0; dead = coop_respawn::Dead{}; InterlockedExchange(&pending, 0); overlay::HideBanner(); return; }   // the level went away
    Countdown(now);
    if (stage == 1) {
        const coop_respawn::Mode before = dead.mode;
        const DWORD openedBefore = dead.clearedAt;
        long left = InterlockedCompareExchange(&lives, 0, 0);
        const bool cleared = InterlockedCompareExchange(&survnet::rewardEvents, 0, 0) != rewardsAtDeath;
        bool go = coop_respawn::Step(dead, survival, o, cleared, left, now, delayMs, settleMs, downStuckMs);
        InterlockedExchange(&lives, left);
        const LONG askedAt = InterlockedExchange(&reviveAskedAt, 0);
        if (askedAt && static_cast<LONG>(now - static_cast<DWORD>(askedAt)) < 5000 && !go) {   // signed: a stamp newer than now is fresh
            go = true;
            InterlockedIncrement(&revivesForced);
            gate::Event("respawn: the host brings %08X back now (%s)", deadPlayer, coop_respawn::ModeName(dead.mode));
        }
        // the host restarts the map: the reload brings everybody back at once
        if (go && lifecycle::HoldReturns()) {
            go = false;
            if (!holdLogged) {
                holdLogged = true;
                InterlockedIncrement(&restartHolds);
                gate::Event("respawn: %08X stays down for the map restart (%s, survival %d)", deadPlayer, coop_respawn::ModeName(dead.mode), survival);
            }
        }
        if (dead.mode != before) {
            InterlockedIncrement(&modeChanges);
            gate::Event("respawn: dead Isaac %08X %s -> %s (survival %d, others up=%u waiting=%u, lives %ld)", deadPlayer, coop_respawn::ModeName(before),
                        coop_respawn::ModeName(dead.mode), survival, o.up, o.waiting, left);
            bannerAt = 0;
        }
        if (dead.clearedAt && !openedBefore)
            gate::Event("respawn: the way back is open for %08X (%s, survival %d, reward event %d): respawn in %lu ms", deadPlayer, coop_respawn::ModeName(dead.mode),
                        survival, cleared ? 1 : 0, static_cast<unsigned long>(settleMs));
        if (!go) return;
        if (BodyGone()) { SpawnFresh(now, "the way back opened"); return; }
        if (dead.mode == coop_respawn::Mode::WaitWave) InterlockedIncrement(&waveReturns);
        if (dead.mode == coop_respawn::Mode::Down) InterlockedIncrement(dead.clearedAt ? &runReturns : &stuckReturns);
        if (dead.mode == coop_respawn::Mode::Down && !dead.clearedAt)
            gate::Event("respawn: %08X was down %lu s and no director ended the run (survival %d): back anyway", deadPlayer,
                        static_cast<unsigned long>((now - dead.since) / 1000), survival);
        stage = 2;
        triggeredAt = now;
        respawnedAt = 0;
        InterlockedIncrement(&triggers);
        // the curtain only for the old placement; with native placement the body appears on its point
        if (!lifecycle::PlacementReady()) {
            overlay::Curtain(8000);
            curtainUp = true;
        }
        placedOnPoint = false;
        gateSampleAt = 0;
        gateReplaces = 0;
        const bool ok = useMpBranch ? CallOriginal(deadPlayer, true) : ScheduleDirect(deadPlayer);
        gate::Event("respawn: %s for %08X %s", useMpBranch ? "ProcessDeath MP branch" : "pending bit + pre-tick", deadPlayer, ok ? "called" : "FAULTED");
        return;
    }
    if ((stage == 2 || stage == 4 || stage == 5) && BodyGone()) { SpawnFresh(now, "while waiting for RespawnPlayer"); return; }
    if (stage == 2 && now - triggeredAt >= 3000) {
        // co-op waits for the partners to drop the old ghost; solo should be next frame
        stage = 4;
        triggeredAt = now;
        InterlockedIncrement(&fallbacks);
        const bool ok = ScheduleDirect(deadPlayer);
        gate::Event("respawn: no RespawnPlayer 3 s after the trigger; pending bit + pre-tick set directly: %s", ok ? "ok" : "FAILED");
        return;
    }
    if (stage == 4 && now - triggeredAt >= 3000) {
        stage = 5;
        triggeredAt = now;
        InterlockedIncrement(&directCalls);
        const bool ok = RespawnDirect(deadPlayer);
        gate::Event("respawn: still no RespawnPlayer; Player::RespawnPlayer called directly for %08X: %s", deadPlayer, ok ? "called" : "FAULTED");
        return;
    }
    if (stage == 5 && now - triggeredAt >= 3000) {
        InterlockedIncrement(&gaveUp);
        stage = 0;
        InterlockedExchange(&pending, 0);
        // With partners the native restart reloads every machine (the death sync
        // spreads it): everybody fell through the map origin afterwards (13.10.).
        LiftRespawnCurtain("the respawn failed", now);
        if (peerTable.Count()) {
            InterlockedIncrement(&keptDown);
            gate::Event("respawn: no new Isaac even after the direct call; no native restart with partners, %08X stays down", deadPlayer);
            overlay::ShowBanner("RESPAWN FAILED", "RESTART THE CHECKPOINT FROM THE PAUSE MENU", 10000);
            return;
        }
        gate::Event("respawn: no new Isaac even after the direct call; giving up, the native restart");
        overlay::ShowBanner("RESPAWN FAILED", "THE LEVEL RESTARTS", 8000);
        NativeDeath(deadPlayer);
        return;
    }
    if (stage == 3) {
        const uint32_t slot0 = LocalCampaignPlayer();
        const LONG since = static_cast<LONG>(now - respawnedAt);   // signed: the hook's stamp may be newer than now
        // a body Init placed itself is equipped on the next tick; the old placement waits 300 ms
        if (slot0 && slot0 != deadPlayer && since >= (placedNatively ? 0 : 300)) Finish(slot0, now);
        else if (since > 5000) {
            stage = 0;
            InterlockedExchange(&pending, 0);
            overlay::HideBanner();
            LiftRespawnCurtain("no new Isaac in slot 0", now);
            gate::Event("respawn: no new Isaac in slot 0 5 s after RespawnPlayer");
        }
    }
}

// The death and respawn hooks, for this module and the story spectator (runtime_coop_spectate.inc).
bool hooksTried = false, hooksOk = false, respawnOk = false, linkOk = false, removeOk = false;
bool InstallHooks(FILE* log);
void Install(FILE* log, const std::string& ini) {
    lifecycle::Install(log, ini);
    enabled = GetPrivateProfileIntA("Respawn", "Enabled", 0, ini.c_str()) != 0;
    livesPerRun = static_cast<int>(GetPrivateProfileIntA("Respawn", "Lives", 1, ini.c_str()));
    if (livesPerRun < 0 || livesPerRun > 99) livesPerRun = 1;
    // the game's own death sequence takes 3953 ms of it (13.10., four deaths)
    delayMs = static_cast<DWORD>(GetPrivateProfileIntA("Respawn", "DelayMs", 4000, ini.c_str()));
    settleMs = static_cast<DWORD>(GetPrivateProfileIntA("Respawn", "WaveClearedDelayMs", 1000, ini.c_str()));
    downStuckMs = static_cast<DWORD>(GetPrivateProfileIntA("Respawn", "DownTimeoutMs", 45000, ini.c_str()));
    if (delayMs > 60000) delayMs = 4000;
    if (settleMs > 30000) settleMs = 1000;
    if (downStuckMs < 15000 || downStuckMs > 600000) downStuckMs = 45000;
    useMpBranch = GetPrivateProfileIntA("Respawn", "UseMpBranch", 1, ini.c_str()) != 0;
    restoreLoadout = GetPrivateProfileIntA("Respawn", "RestoreLoadout", 1, ini.c_str()) != 0;
    hideDeathScreen = GetPrivateProfileIntA("Respawn", "HideDeathScreen", 1, ini.c_str()) != 0;
    if (!enabled) { std::fprintf(log, "respawn off ([Respawn] Enabled=0)\n"); return; }
    // the countdown banner needs the overlay, which otherwise only the shop installs
    if (!overlay::ready) overlay::Install(log, ini);
    enabled = InstallHooks(log);
    std::fprintf(log, "respawn %s: lives=%d delay_ms=%lu mp_branch=%d restore_loadout=%d respawn_player=%d link_msg=%d remove_listener=%d process_death_hook=%d\n",
                 enabled ? "installed" : "NOT installed", livesPerRun, static_cast<unsigned long>(delayMs), useMpBranch ? 1 : 0, restoreLoadout ? 1 : 0,
                 respawnOk ? 1 : 0, linkOk ? 1 : 0, removeOk ? 1 : 0, deathHook ? 1 : 0);
    std::fprintf(log, "respawn rules: lives per wave solo, a partner up = back after the wave, all down = the director's game over; wave_cleared_delay_ms=%lu "
                      "down_timeout_ms=%lu\n",
                 static_cast<unsigned long>(settleMs), static_cast<unsigned long>(downStuckMs));
}
bool InstallHooks(FILE* log) {
    if (hooksTried) return hooksOk;
    hooksTried = true;
    // the death state guard lives in the bot module; without bots it is laid here
    if (!bot::deathSmHook) {
        // push ebp / mov ebp,esp / and esp,-16 / sub esp,114h / push ebx / mov ebx,[ebp+14h] / push esi / lea eax,[ebx-17h]
        static const unsigned char kDeathSm[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x14, 0x01, 0x00, 0x00, 0x53, 0x8B, 0x5D, 0x14, 0x56, 0x8D, 0x43, 0xE9};
        hook_site::Install(log, "respawn PlayerDeathSM::HandleStateMessage", base + 0x4A12D0, kDeathSm, sizeof(kDeathSm),
                           reinterpret_cast<void*>(bot::OnDeathSm), bot::deathSmHook);
    }
    static const unsigned char kRespawnHead[] = {0x83, 0xEC, 0x34, 0x53, 0x56, 0x8B, 0x74, 0x24, 0x40};
    static const unsigned char kLinkHead[] = {0x56, 0x8B, 0x74, 0x24, 0x08, 0x83, 0x3E, 0x00};
    static const unsigned char kRemoveHead[] = {0x56, 0x8B, 0x71, 0x58, 0x33, 0xC0, 0x85, 0xF6};
    static const unsigned char kDeathHead[] = {0x83, 0xEC, 0x3C, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0xD8, 0x03, 0x00, 0x00};
    unsigned char a[16]{}, b[16]{}, c[16]{}, d[16]{};
    respawnOk = Read(base + kRespawnPlayerRva, a, sizeof(kRespawnHead)) && std::memcmp(a, kRespawnHead, sizeof(kRespawnHead)) == 0;
    linkOk = Read(base + kLinkMsgRva, b, sizeof(kLinkHead)) && std::memcmp(b, kLinkHead, sizeof(kLinkHead)) == 0;
    removeOk = Read(base + kRemoveDeleteListenerRva, d, sizeof(kRemoveHead)) && std::memcmp(d, kRemoveHead, sizeof(kRemoveHead)) == 0;
    if (!deathHook && Read(base + kProcessDeathRva, c, sizeof(kDeathHead)) && std::memcmp(c, kDeathHead, sizeof(kDeathHead)) == 0)
        deathHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kProcessDeathRva), reinterpret_cast<void*>(OnProcessDeath));
    if (respawnOk) respawnHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kRespawnPlayerRva), reinterpret_cast<void*>(OnRespawnPlayer));
    // the online lookups that MP death code may reach: guarded as in the necro module
    if (!necro::enabled) { necro::InstallPlayerByRefGuard(log); necro::InstallOnlineRefGuard(log); }
    hooksOk = respawnOk && linkOk && removeOk && static_cast<bool>(deathHook) && static_cast<bool>(respawnHook);
    std::fprintf(log, "respawn hooks %s: respawn_player=%d link_msg=%d remove_listener=%d process_death_hook=%d death_sm=%d\n", hooksOk ? "ready" : "MISSING",
                 respawnOk ? 1 : 0, linkOk ? 1 : 0, removeOk ? 1 : 0, deathHook ? 1 : 0, bot::deathSmHook ? 1 : 0);
    return hooksOk;
}
void WriteDiagnostics(FILE* log) {
    lifecycle::WriteDiagnostics(log);
    if (!enabled) return;
    std::fprintf(log, "respawn lifecycle placed_by_init=%ld born_dressed=%ld restart_holds=%ld survival_from_host=%ld\n", InterlockedCompareExchange(&placedByInit, 0, 0),
                 InterlockedCompareExchange(&bornDressed, 0, 0), InterlockedCompareExchange(&restartHolds, 0, 0), InterlockedCompareExchange(&survivalFromHost, 0, 0));
    const coop_respawn::Others o = OthersNow(GetTickCount());
    std::fprintf(log, "respawn rules mode=%s modes timed=%ld free=%ld wait_wave=%ld down=%ld changes=%ld returns wave=%ld run=%ld stuck=%ld team_down=%ld/%ld "
                      "| life out=%ld in=%ld rejected=%ld changes=%ld others up=%u waiting=%u\n",
                 coop_respawn::ModeName(dead.mode), InterlockedCompareExchange(&modeCounts[1], 0, 0), InterlockedCompareExchange(&modeCounts[2], 0, 0),
                 InterlockedCompareExchange(&modeCounts[3], 0, 0), InterlockedCompareExchange(&modeCounts[4], 0, 0), InterlockedCompareExchange(&modeChanges, 0, 0),
                 InterlockedCompareExchange(&waveReturns, 0, 0), InterlockedCompareExchange(&runReturns, 0, 0), InterlockedCompareExchange(&stuckReturns, 0, 0),
                 InterlockedCompareExchange(&teamDownFlag, 0, 0), InterlockedCompareExchange(&teamDownSeen, 0, 0), InterlockedCompareExchange(&lifeOut, 0, 0),
                 InterlockedCompareExchange(&lifeIn, 0, 0), InterlockedCompareExchange(&lifeRejected, 0, 0), InterlockedCompareExchange(&lifeChanges, 0, 0), o.up,
                 o.waiting);
    std::fprintf(log, "respawn lives=%ld armed=%ld stage=%d deaths=%ld taken=%ld duplicates=%ld no_lives=%ld bad_guid=%ld triggers=%ld respawned=%ld "
                      "fallbacks=%ld listeners_removed=%ld gave_up=%ld native=%ld epoch=%ld\n",
                 InterlockedCompareExchange(&lives, 0, 0), InterlockedCompareExchange(&armed, 0, 0), stage, InterlockedCompareExchange(&deaths, 0, 0),
                 InterlockedCompareExchange(&taken, 0, 0), InterlockedCompareExchange(&duplicates, 0, 0), InterlockedCompareExchange(&refusedNoLives, 0, 0),
                 InterlockedCompareExchange(&refusedGuid, 0, 0), InterlockedCompareExchange(&triggers, 0, 0), InterlockedCompareExchange(&respawned, 0, 0),
                 InterlockedCompareExchange(&fallbacks, 0, 0), InterlockedCompareExchange(&listenersRemoved, 0, 0), InterlockedCompareExchange(&gaveUp, 0, 0),
                 InterlockedCompareExchange(&nativeFallbacks, 0, 0), InterlockedCompareExchange(&epoch, 0, 0));
    std::fprintf(log, "respawn death_messages drops_kept=%ld death_screens_hidden=%ld hook=%d\n", InterlockedCompareExchange(&dropsKept, 0, 0),
                 InterlockedCompareExchange(&deathScreensHidden, 0, 0), bot::deathSmHook ? 1 : 0);
    std::fprintf(log, "respawn restored guns=%ld stacks=%ld credits=%ld failures=%ld death_screen_restarts_refused=%ld\n",
                 InterlockedCompareExchange(&restoredGuns, 0, 0), InterlockedCompareExchange(&restoredStacks, 0, 0),
                 InterlockedCompareExchange(&restoredCredits, 0, 0), InterlockedCompareExchange(&restoreFailures, 0, 0),
                 InterlockedCompareExchange(&checkpoint::respawnRestartsRefused, 0, 0));
}
}  // namespace respawn

#include "runtime_coop_lifecycle.inc"
