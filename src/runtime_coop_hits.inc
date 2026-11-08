// Dead Space 2 United by Hafer.
// Client hits on host-owned objects.
//
// The engine refuses to broadcast damage from a peer that does not own the
// target. DirectSendDamageEvent (PC 0x010BD030) reads the target's net proxy at
// reflectable+0x0C, then IsAuthoritative, and returns having done nothing if
// either fails. With HostNPCs the host owns every enemy, so every shot the
// client fires is applied locally and never sent. That is why the two machines
// disagree about which limbs came off.
//
// The hook sits on that function and changes nothing about what the engine
// does (the original runs either way). It only notices the case the engine
// drops and tells the host about it.
//
// Observation first. `ApplyHits` is off by default and the receiver honours an
// observe-only flag (no sender sets it at present), because the 168-byte
// NetDamageData_t payload has not been laid out and applying data on a guessed
// layout crashed the game twice before. What this answers on its own is the
// question everything else depends on: does a sub-key sent by one machine name
// the same object on the other.
namespace escalation { float BotHitFactor(const void* damage, uint32_t component); }   // runtime_coop_escalation.inc
namespace bot { bool MortalNow(); }   // runtime_coop_bot.inc
namespace hits {
// 0x010BD030 with the usual base.
constexpr uint32_t kDirectSendDamageRva = 0xCBD030;
// A NetReflectable sits inside its Entity, and the pieces around it are all
// measured, not guessed:
//   entity+0x24  the authored GUID              (used all over the randomizer)
//   entity+0x40  the net sub-key                (SetSubKey writes reflectable-0x1C)
//   entity+0x5C  the NetReflectable sub-object  (the receive handler does lea ecx,[eax-0x5c])
//   +0x0C on it  the net proxy                  (AddToSession writes it; npcs::Register checks it)
constexpr int32_t kSubKeyFromReflectable = -0x1C;
constexpr uint32_t kEntityReflectable = 0x5C, kEntityGuid = 0x24;
constexpr uint32_t kReflectableProxy = 0x0C, kProxyFlags = 0x30;
// The intrusive list of every NetReflectable, the one npcs::Scan already walks.
constexpr uint32_t kReflectableListRva = 0x1C5B744;

// The two health components, and where they keep health. Read out of SetHealth
// itself (PC 0x00F84C60), which clamps the new value against +0xE4 and skips the
// write when it already equals +0xE8:
//   +0xE4  maximum health
//   +0xE8  current health
//   +0x10  the owning entity   (kComponentOwner, same as the watchdog uses)
//   vtable +0x20  SetHealth    (the watchdog has been calling this in production)
constexpr uint32_t kHealthVtableRva = 0x1764760, kDismemberableVtableRva = 0x1764C68;
constexpr uint32_t kHandleDamageHcRva = 0xB65B80, kHandleDamageDhcRva = 0xB73600;
constexpr uint32_t kComponentOwner = 0x10, kHealthCurrent = 0xE8, kHealthMax = 0xE4;
constexpr uint32_t kSetHealthSlot = 0x20, kSetHealthRva = 0xB84C60;
constexpr uint32_t kActorScanBytes = 0x1000;

bool enabled = false;      // [Coop] ReportHits
bool bodyWatch = false;    // health hooks installed for necro DeathRevert alone (InstallBodyWatch)
bool applyHits = false;    // [Coop] ApplyHits
bool worldHits = true;     // [Coop] WorldHits: damage on GUID-named world objects goes to everybody
volatile LONG worldReported = 0, worldApplied = 0, worldNotFound = 0, worldSkipped = 0, worldSolo = 0, gateRefused = 0,
              worldProxyTarget = 0, watcherHitsKept = 0;
safetyhook::InlineHook sendHook, damageHookHc, damageHookDhc;

// The final boss never dies. NicoleBoss::ApplyDamageModifiers (0x0107D930,
// vtable +0x210, thiscall(DamageData&), ret 4) turns a hit that would reach
// zero into amount 0 plus the flag byte +0x870, and her state machine reads
// that flag (TestAndClearHealthReachedZero) to fade her out and bring in the
// marker heart. So when every machine runs its own Nicole (LocalNPCClasses),
// her hits are shared from this entry, and a lethal one sets the flag on the
// partner's copy instead of taking her health to zero.
bool nicoleShared = false;   // set when NicoleBoss is a local class
constexpr uint32_t kNicoleVtableRva = 0x1774B78, kNicoleModifiersRva = 0xC7D930, kNicoleZeroFlag = 0x870;
constexpr uint32_t kNicoleEpsilonRva = 0x17625F0, kEntityHiddenCount = 0x16D;
safetyhook::InlineHook nicoleHook;
volatile LONG nicoleSent = 0, nicoleLethalSent = 0, nicoleNoComponent = 0, nicoleApplied = 0, nicoleFlagged = 0, nicoleHidden = 0, nicoleGhostZeroed = 0, nicoleFaults = 0;
// World props. DamageableProp, BreakableContainer, Destructible and the vent
// covers keep their health in IDamageable (+0x44 health, +0x40 max, +0xC the
// owning entity), not in a HealthComponent, so the HandleDamage hooks above
// never saw them: world_reported stayed 0 in every session and a vent shot on
// one machine broke only there (24.09.). IDamageable::ProcessDamage
// (0x00DC5260, thiscall(DamageData&, bool reflected), ret 8) is where every hit
// on them lands. The engine's own MP path for such a hit is the damage RPC
// receiver (0x010A0F10): QueryInterface INetDamage (0x388C5A03), DamageData
// ctor 0x00ECFD30, NetDamageData_t::ToDamageData 0x00ECFF90, then
// HandleReflectedDamage (INetDamage +4), which calls ProcessDamage with
// reflected=true. The hit goes out as the NetDamageData_t the engine would have sent
// (ctor 0x00EAE280 from the DamageData; its only caller, the cdecl wrapper
// 0x010CD7B0, builds it that way and passes it to DirectSendDamageEvent
// 0x010BD030, checked in the 28.09. dump) and is replayed through that same
// receiver sequence; the replay is reflected, so it is never sent back.
bool propHits = false;     // [Coop] WorldPropHits
constexpr uint32_t kProcessDamageRva = 0x9C5260, kNetFromDamageRva = 0xAAE280;
constexpr uint32_t kDamageCtorRva = 0xACFD30, kNetToDamageRva = 0xACFF90;
constexpr uint32_t kSafePtrReleaseRva = 0x3429F0, kDamageFreeRva = 0x3E22, kDamageBaseVtableRva = 0x16A5D18;
constexpr uint32_t kNetDamageInterface = 0x388C5A03;
// End of .text (section table: RVA 0x1000, size 0x16A0000); .rdata follows.
// Shared by every "is this a code pointer" check in the coop modules.
constexpr uint32_t kCodeEnd = 0x16A1000;
constexpr uint32_t kDamageableOwner = 0x0C, kDamageableMax = 0x40, kDamageableHealth = 0x44;
#include "coop_prop_wire.h"
safetyhook::InlineHook propHook;
uint32_t propSequence = 0;
struct SeenHit { uint32_t token, sequence; };
SeenHit propSeen[64]{};
unsigned propSeenNext = 0;
volatile LONG propAbsorbed = 0, propAbsorbedInvincible = 0, propAbsorbedLogged = 0;
volatile LONG propSent = 0, propReceived = 0, propApplied = 0, propDuplicates = 0, propNotFound = 0,
              propGhostTarget = 0, propNoInterface = 0, propSkippedOwner = 0, propFaults = 0,
              propCleanupFaults = 0, propSentLogged = 0, propReplayLogged = 0;
constexpr LONG kPropLogLines = 20;
void OnWorldPacket(const WorldPacket& p);
#include "runtime_coop_terminalprop.inc"
bool IsNicole(uint32_t entity) {
    uint32_t vt = 0;
    return nicoleShared && entity && Read(entity, &vt, 4) && vt == base + kNicoleVtableRva;
}
coop_hit::Sender sender;
coop_hit::Receiver receiver;

// The Tormentor (class Kong, ch03, 12.10.). Its four grabs (Pin1, Pin2, Hang1,
// Hang2) end when the right arm is shot down to the phase's threshold. Every
// machine runs its own Kong (it is an Animated, never ghosted), and the
// follower's took only what its streamed input happened to hit: 25.09. its arm
// stayed whole and its world played the kill. Kong::HandleDamage (0x010AD0A0,
// thiscall(DamageData*, bool*), ret 8) clamps a hit on body part 1 to the
// phase threshold (0x010796A0), lets the DismemberableHealthComponent take it,
// copies part 1's health (component +0x1F8 + 4) to +0x294, and when
// IsArmDamagedEnoughThisPhase (0x01079100, phase at +0x24C) says so raises
// flag 1 on its state machine ([+0x248], 0x015ED280). The arm health after each
// hit goes to the partners with the phase; a Kong here in the same phase takes
// the lower value and runs the same tail. The flag is raised again for a few
// seconds, since a Kong still in its grab animation is not listening yet.
// Shots of a partner's ghost on our Kong count nothing: his machine sends the result.
bool kongShared = false;   // [Coop] ShareTormentorArm
constexpr uint32_t kKongVtableRva = 0x17733E8, kKongHandleDamageRva = 0xCAD0A0, kKongArmCheckRva = 0xC79100, kKongRaiseRva = 0x11ED280;
constexpr uint32_t kKongPhase = 0x24C, kKongArm = 0x294, kKongMachine = 0x248, kPartHealth = 0x1F8, kArmPart = 1;
constexpr uint32_t kKongMagic = 0x474E4F4B, kKongVersion = 1;   // 'KONG'
constexpr int kKongBytes = 40;
struct KongPacket { uint32_t magic, version, token, sequence, guid[4]; int32_t phase; float arm; };
static_assert(sizeof(KongPacket) == kKongBytes, "Unexpected Tormentor wire layout");
safetyhook::InlineHook kongHook;
uint32_t kongSequence = 0;
struct KongRepeat { KongPacket packet; DWORD at; unsigned sent; };
KongRepeat kongRepeat{};
// A partner's result still being raised here (the Kong may not listen yet).
struct KongPending { uint32_t guid[4]; int32_t phase; DWORD until, lastRaise; bool used; };
KongPending kongPending{};
volatile LONG kongHits = 0, kongSent = 0, kongReceived = 0, kongApplied = 0, kongNotLower = 0, kongPhaseMismatch = 0, kongNotFound = 0,
              kongRaised = 0, kongReraised = 0, kongGhostZeroed = 0, kongFaults = 0, kongLines = 0;
bool IsKong(uint32_t entity) {
    uint32_t vt = 0;
    return entity && Read(entity, &vt, 4) && vt == base + kKongVtableRva;
}
#include "runtime_coop_kong_stasis.inc"
// IsArmDamagedEnoughThisPhase, then the flag on the state machine, as the tail of HandleDamage does.
bool KongCheckAndRaise(uint32_t kong) {
    __try {
        const bool enough = reinterpret_cast<bool(__thiscall*)(void*)>(base + kKongArmCheckRva)(reinterpret_cast<void*>(kong));
        if (!enough) return false;
        uint32_t machine = 0;
        if (!Read(kong + kKongMachine, &machine, 4) || !machine) return false;
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kKongRaiseRva)(reinterpret_cast<void*>(machine), 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&kongFaults);
        return false;
    }
}

volatile LONG damageCalls = 0, withProxy = 0, authoritative = 0, foreign = 0;
volatile LONG reported = 0, sendFailed = 0, faults = 0, clampedReports = 0, notOwner = 0;
volatile LONG resolved = 0, unresolved = 0, guidMismatch = 0, applyRefused = 0;
volatile LONG applied = 0, applyNoComponent = 0, applyFaults = 0;
// A hit can only ever take health away, and never more than this much in one
// message. A corrupt or replayed packet then costs a scratch, not a kill.
constexpr float kMaxSingleHit = 100000.0f;

// A few lines of real data, once, so the payload layout can be worked out from
// a session instead of from a guess.
constexpr unsigned kSampleCapacity = 12;
struct Sample { uint32_t entity, proxy, subKey, guid[4]; int authoritative; uint32_t words[6]; volatile LONG ready; };
Sample samples[kSampleCapacity]{};
volatile LONG sampleCount = 0;

// Bit 1 of proxy+0x30 is the GHOST bit, not an authority bit: measured in
// the 16:28 client dump, local Player 0x8, the partner's ghost 0x2 (and
// IsGhostPlayer has tested exactly that bit all along). Until 20:00 this read
// the bit the other way round, so a client's hits on the host's enemies (the
// one case worth reporting) were dropped as local, and reports went out for
// the client's own objects instead. DirectSendDamageEvent's third gate
// (0x007E1150) therefore reads: send only when the target IS a ghost, i.e.
// the engine does try to tell the owner; whether that arrives is still open.
bool Authoritative(uint32_t proxy) {
    uint32_t flags = 0;
    return proxy && Read(proxy + kProxyFlags, &flags, 4) && !((flags >> 1) & 1);
}

// The host's copy of whatever the client says it hit. Walks the same list the
// NPC scan walks; a sub-key match is the answer and the GUID is the check on it.
bool ResolveSubKey(uint32_t subKey, const uint32_t wanted[4], uint32_t& entityOut,
                   bool& guidMatched) {
    entityOut = 0;
    guidMatched = false;
    if (!subKey) return false;
    uint32_t node = 0;
    if (!Read(base + kReflectableListRva, &node, 4)) return false;
    for (unsigned steps = 0; node && steps < 4096; ++steps) {
        uint32_t key = 0;
        if (Read(static_cast<uint32_t>(node + kSubKeyFromReflectable), &key, 4) && key == subKey) {
            const uint32_t entity = node - kEntityReflectable;
            uint32_t guid[4]{};
            if (Read(entity + kEntityGuid, guid, sizeof(guid))) {
                guidMatched = wanted && guid[0] == wanted[0] && guid[1] == wanted[1] &&
                              guid[2] == wanted[2] && guid[3] == wanted[3];
            }
            entityOut = entity;
            return true;
        }
        uint32_t next = 0;
        if (!Read(node + 4, &next, 4) || next == node) break;
        node = next;
    }
    return false;
}

// The actor's health component, recognised the way the watchdog recognises it:
// a pointer inside the actor to an object carrying one of the two health
// vtables whose owner field points back here. The back-pointer is what rules
// out a coincidence.
// The pointer sits at the same offset in every actor of a class, so the offset is
// remembered per vtable and checked again on each use; the 4 KB scan (a Read of
// every pointer-like value in it, floats included) runs once per class instead of
// on every call (05.10.: three bots asked for their own health every frame and
// for every enemy within 30 m on every scan).
struct HealthOffset { uint32_t vtable, offset; };
HealthOffset healthOffsets[64]{};
volatile LONG healthScans = 0, healthCacheHits = 0;
uint32_t FindHealthComponent(uint32_t actor) {
    const uint32_t health = static_cast<uint32_t>(base + kHealthVtableRva);
    const uint32_t dismemberable = static_cast<uint32_t>(base + kDismemberableVtableRva);
    uint32_t actorVtable = 0;
    HealthOffset* cached = Read(actor, &actorVtable, 4) && actorVtable ? &healthOffsets[(actorVtable >> 2) & 63] : nullptr;
    if (cached && cached->vtable == actorVtable) {
        uint32_t candidate = 0, vtable = 0, owner = 0;
        if (Read(actor + cached->offset, &candidate, 4) && candidate >= 0x10000 && Read(candidate, &vtable, 4) &&
            (vtable == health || vtable == dismemberable) && Read(candidate + kComponentOwner, &owner, 4) && owner == actor) {
            InterlockedIncrement(&healthCacheHits);
            return candidate;
        }
    }
    InterlockedIncrement(&healthScans);
    for (uint32_t offset = 0; offset < kActorScanBytes; offset += 4) {
        uint32_t candidate = 0, vtable = 0, owner = 0;
        if (!Read(actor + offset, &candidate, 4) || candidate < 0x10000) continue;
        if (!Read(candidate, &vtable, 4)) continue;
        if (vtable != health && vtable != dismemberable) continue;
        if (!Read(candidate + kComponentOwner, &owner, 4) || owner != actor) continue;
        if (cached) *cached = HealthOffset{actorVtable, offset};
        return candidate;
    }
    return 0;
}

// SEH and C++ objects do not share a frame, so the engine call lives alone.
__declspec(noinline) bool CallSetHealth(uint32_t component, float value) {
    uint32_t vtable = 0, slot = 0;
    if (!Read(component, &vtable, 4) ||
        !Read(vtable + kSetHealthSlot, &slot, 4) || slot != base + kSetHealthRva)
        return false;
    __try {
        using SetHealth = void (__thiscall*)(void*, float);
        reinterpret_cast<SetHealth>(slot)(reinterpret_cast<void*>(component), value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Host side. Takes the reported amount off the target's health through the
// engine's own zero path, so the death events, the drop and any door counter
// fire exactly as they would have. Only ever downwards, never past zero, and
// only through a vtable slot that still holds the expected SetHealth: the same
// three checks the watchdog's kill has been running in production.
// keepBotFloor: a bot that cannot die keeps a quarter of its health, as its own HandleDamage
// floor does (runtime_coop_bot.inc OnHealthApply).
volatile LONG botHitsFloored = 0, botHitsRefused = 0, botHitLines = 0;
void ApplyDamage(uint32_t entity, float amount, bool keepBotFloor = false) {
    if (!(amount > 0.0f) || amount > kMaxSingleHit) { InterlockedIncrement(&applyRefused); return; }
    const uint32_t component = FindHealthComponent(entity);
    if (!component) { InterlockedIncrement(&applyNoComponent); return; }
    float current = 0.0f;
    if (!Read(component + kHealthCurrent, &current, 4) || !(current > 0.0f)) {
        // Already dead here. Nothing to do, and certainly nothing to revive.
        InterlockedIncrement(&applyRefused);
        return;
    }
    float target = current - amount;
    if (!(target > 0.0f)) target = 0.0f;
    float max = 0.0f;
    if (keepBotFloor && Read(component + kHealthMax, &max, 4) && max > 0.0f) {
        const float quarter = max * 0.25f;
        float keep = current < quarter ? current : quarter;
        if (keep < 1.0f) keep = current < 1.0f ? current : 1.0f;
        if (target < keep) { target = keep; InterlockedIncrement(&botHitsFloored); }
        if (!(target < current)) return;
    }
    if (CallSetHealth(component, target)) InterlockedIncrement(&applied);
    else InterlockedIncrement(&applyFaults);
}

// Called from ExchangePlayerState when a hit report arrives. Host side.
void OnReport(const coop_hit::Packet& packet) {
    uint32_t entity = 0;
    bool guidMatched = false;
    if (!ResolveSubKey(packet.subKey, packet.guid, entity, guidMatched)) {
        InterlockedIncrement(&unresolved);
        ++receiver.unresolved;
        return;
    }
    InterlockedIncrement(&resolved);
    ++receiver.resolved;
    if (!guidMatched) {
        // Resolved something, but not the thing the client meant. Never act on
        // that: a sub-key that has been recycled onto a different object would
        // otherwise let one machine kill something the other never shot.
        InterlockedIncrement(&guidMismatch);
        ++receiver.mismatched;
        return;
    }
    // Only the owner acts on a report. Reports go to every machine, and one
    // that is not the owner resolves the same sub-key to its ghost copy: a
    // third player's copy of Isaac lost health there, an enemy on a second
    // client died before the host's did.
    uint32_t targetProxy = 0;
    if (Read(entity + kEntityReflectable + kReflectableProxy, &targetProxy, 4) && targetProxy && !Authoritative(targetProxy)) {
        InterlockedIncrement(&notOwner);
        return;
    }
    if (!applyHits || (packet.flags & coop_hit::kFlagObserveOnly)) {
        InterlockedIncrement(&applyRefused);
        return;
    }
    // A partner's hit on one of our bots skipped the bot's own damage gate (17.10., bots review):
    // a human's is refused (same side), a living necro's comes off down to the bot's floor while
    // bots cannot die.
    if (IsBotEntity(entity)) {
        const auto* from = peerTable.FindByToken(packet.token);
        const unsigned slot = from ? PeerSlot(from) : 0xFFu;
        const int role = slot < coop_peers::kMaxPeers ? roles::RoleOf(slot, GetTickCount()) : 0;
        const bool mortal = bot::MortalNow();
        if (role != 1) {
            InterlockedIncrement(&botHitsRefused);
            if (InterlockedIncrement(&botHitLines) <= 20)
                gate::Event("coop_hits partner %u's hit of %.1f on our bot %08X refused: %s (mortal %d)", slot, packet.amount, entity,
                            role == 0 ? "a human, same side" : "not a living necro", mortal ? 1 : 0);
            return;
        }
        ApplyDamage(entity, packet.amount, !mortal);
        return;
    }
    ApplyDamage(entity, packet.amount);
}

// A partner's hit on his Nicole, applied to ours the way her own modifier
// would: below her health it comes off, at or above it only the flag is set.
// While she is hidden (faded out between phases) nothing is taken, so a late
// hit cannot end a phase that has not started here yet.
void ApplyNicole(uint32_t entity, float amount) {
    if (!(amount > 0.0f) || amount > kMaxSingleHit) { InterlockedIncrement(&applyRefused); return; }
    unsigned char hiddenCount = 0;
    if (Read(entity + kEntityHiddenCount, &hiddenCount, 1) && hiddenCount) { InterlockedIncrement(&nicoleHidden); return; }
    const uint32_t component = FindHealthComponent(entity);
    float current = 0.0f, epsilon = 0.0f;
    if (!component || !Read(component + kHealthCurrent, &current, 4) || !Read(base + kNicoleEpsilonRva, &epsilon, 4)) {
        InterlockedIncrement(&applyNoComponent);
        return;
    }
    if (!(current > 0.0f)) { InterlockedIncrement(&applyRefused); return; }
    if (amount >= current - epsilon) {
        const unsigned char one = 1;
        if (WriteBytes(entity + kNicoleZeroFlag, &one, 1)) InterlockedIncrement(&nicoleFlagged);
        else InterlockedIncrement(&nicoleFaults);
        gate::Event("coop_hits nicole: partner's hit %.1f reaches her health %.1f here, zero flag set", amount, current);
        return;
    }
    if (CallSetHealth(component, current - amount)) InterlockedIncrement(&nicoleApplied);
    else InterlockedIncrement(&nicoleFaults);
}

// A partner damaged a world object: the same amount comes off the local copy,
// found by GUID. Runs on every machine, host or not, since nobody owns a button.
void OnWorldReport(const coop_hit::Packet& packet) {
    if (!worldHits) { InterlockedIncrement(&worldSkipped); return; }
    const uint32_t object = mirror::ObjectByGuid(packet.guid);
    if (!object) { InterlockedIncrement(&worldNotFound); return; }
    if (phantom::Is(object)) { InterlockedIncrement(&phantom::worldHitsKept); return; }   // ours alone, nobody else hits it
    if (IsNicole(object)) { ApplyNicole(object, packet.amount); return; }
    // Pool enemies can share a GUID with a world object; a networked object
    // takes its damage through OnReport only.
    uint32_t proxy = 0;
    if (!Read(object + kEntityReflectable + kReflectableProxy, &proxy, 4) || proxy) {
        InterlockedIncrement(&worldProxyTarget);
        return;
    }
    const LONG before = InterlockedCompareExchange(&applied, 0, 0);
    ApplyDamage(object, packet.amount);
    if (InterlockedCompareExchange(&applied, 0, 0) != before) {
        if (InterlockedIncrement(&worldApplied) <= 20 && gate::trace)
            gate::Event("coop_hits world hit applied: %.1f on %08X guid=%08X-%08X-%08X-%08X", packet.amount, object,
                                     packet.guid[0], packet.guid[1], packet.guid[2], packet.guid[3]);
    }
}

void Record(uint32_t entity, uint32_t proxy, uint32_t subKey, const uint32_t guid[4],
            bool isAuthoritative, const void* damage) {
    const LONG slot = InterlockedIncrement(&sampleCount) - 1;
    if (slot < 0 || slot >= static_cast<LONG>(kSampleCapacity)) return;
    Sample& s = samples[slot];
    s.entity = entity; s.proxy = proxy; s.subKey = subKey;
    for (int i = 0; i < 4; ++i) s.guid[i] = guid[i];
    s.authoritative = isAuthoritative ? 1 : 0;
    // The first words of the payload, so its layout can be read off a real
    // session rather than invented.
    Read(reinterpret_cast<uintptr_t>(damage), s.words, sizeof(s.words));
    InterlockedExchange(&s.ready, 1);   // the diagnostics thread prints only filled samples
}

// Client side.
//
// The amount is measured, not parsed. The 168-byte NetDamageData_t has never
// been laid out, and guessing its fields crashed the game twice. The client
// applies the damage locally anyway (that is the bug), so the reliable number
// is how much health went away while the engine's own handler ran. It is
// exactly what the host has to subtract to end up in the same place.
// A report is sent three times (now, +60 ms, +180 ms): a hit is an event, and
// one lost datagram was damage that never happened on the host. The receiver
// folds the copies by sender and sequence.
struct Repeat { coop_hit::Packet packet; DWORD at; unsigned sent; };
constexpr unsigned kRepeatSlots = 32, kCopies = 3;
constexpr DWORD kCopyDelayMs[kCopies] = {0, 60, 180};
Repeat repeats[kRepeatSlots]{};
unsigned repeatHead = 0;
volatile LONG copiesSent = 0, repeatsOverwritten = 0;
void SendReport(uint32_t subKey, const uint32_t guid[4], float amount, uint32_t flags) {
    if (dataSock == INVALID_SOCKET || peerTable.Count() == 0) return;
    sender.token = peerReadiness.token;
    const auto packet = sender.MakePacket(subKey, guid, amount, 0, flags);
    // To everyone: with more than two machines the owner of the target is not
    // necessarily the configured peer. Only the owner acts on it; the others
    // resolve a ghost copy and drop it (OnReport).
    if (SendToPeers(&packet, sizeof(packet))) InterlockedIncrement(&reported);
    else InterlockedIncrement(&sendFailed);
    Repeat& r = repeats[repeatHead];
    repeatHead = (repeatHead + 1) % kRepeatSlots;
    if (r.sent && r.sent < kCopies) InterlockedIncrement(&repeatsOverwritten);
    r = Repeat{packet, GetTickCount(), 1};
}
// From Poll: the remaining copies.
void KongTick(DWORD now);   // below
void Tick(DWORD now) {
    terminalprop::Tick(now);
    if (dataSock == INVALID_SOCKET || peerTable.Count() == 0) return;
    if (kongShared) KongTick(now);
    for (auto& r : repeats) {
        if (!r.sent || r.sent >= kCopies || now - r.at < kCopyDelayMs[r.sent]) continue;
        if (SendToPeers(&r.packet, sizeof(r.packet))) InterlockedIncrement(&copiesSent);
        ++r.sent;
    }
}

// One health component took damage. Runs on both peers; only the one that does
// not own the target has anything to say.
void OnHealthDamage(uint32_t component, const void* damage, float before, float after) {
    float delta = before - after;
    // The local copy of a partner's enemy ran out of health before the owner's
    // did (a lost report, a third player's hit never seen here): clamped at zero it
    // reported less than was dealt, or nothing at all, and the enemy lay dead
    // here and stood on over there. The amount the hit carried goes out
    // instead; the owner's ApplyDamage refuses anything already dead.
    float carried = 0.0f;
    if (!(after > 0.0f) && damage && Read(reinterpret_cast<uintptr_t>(damage) + 0x50, &carried, 4) &&
        carried > delta && carried <= kMaxSingleHit) {
        delta = carried;
        InterlockedIncrement(&clampedReports);
    }
    if (!(delta > 0.0f)) return;              // healed, or nothing happened
    if (necro::LocalRole() == 3) { InterlockedIncrement(&watcherHitsKept); return; }   // a necro watching a scene as Isaac is a spectator
    uint32_t entity = 0;
    if (!Read(component + kComponentOwner, &entity, 4) || !entity) return;
    necro::OnBodyHealth(entity, after);
    const uint32_t reflectable = entity + kEntityReflectable;
    uint32_t proxy = 0, subKey = 0, guid[4]{};
    if (!Read(reflectable + kReflectableProxy, &proxy, 4)) return;
    if (!proxy) {
        if (IsNicole(entity)) return;   // shared from her modifier hook, lethal hits included
        if (phantom::Is(entity)) { InterlockedIncrement(&phantom::worldHitsKept); return; }   // a hallucination: the partners have none
        // No proxy: a world object. If it has an authored GUID, everybody has
        // a copy and everybody should take the same damage.
        if (!worldHits || IsPlayerOrNecroEntity(entity) || !Read(entity + kEntityGuid, guid, sizeof(guid)) || !(guid[0] | guid[1] | guid[2] | guid[3])) return;   // a not-yet-registered body shares its template GUID with every machine's copy
        // Nobody to tell (solo, the bots' game): SendReport would drop it anyway. A prop
        // burning or under a flamethrower takes a hit every frame: 05.10. logged ~2200
        // lines in one session and pushed other events out of the ring. The count
        // goes to the props line; only the first lines are logged.
        if (dataSock == INVALID_SOCKET || peerTable.Count() == 0) { InterlockedIncrement(&worldSolo); return; }
        SendReport(coop_hit::kWorldSubKey, guid, delta, coop_hit::kFlagWorld);
        if (InterlockedIncrement(&worldReported) <= 20 && gate::trace)
            gate::Event("coop_hits world hit sent: %.1f guid=%08X-%08X-%08X-%08X entity=%08X", delta, guid[0], guid[1], guid[2], guid[3], entity);
        return;
    }
    if (Authoritative(proxy)) return;          // owned here: the engine broadcasts it itself
    if (!Read(static_cast<uint32_t>(reflectable + kSubKeyFromReflectable), &subKey, 4) || !subKey)
        return;
    Read(entity + kEntityGuid, guid, sizeof(guid));
    Record(entity, proxy, subKey, guid, false, damage);
    SendReport(subKey, guid, delta, 0);
}

// A local NPC class (every machine fights its own Nicole) that hits the
// partner's ghost Isaac: that swing belongs to this machine's Nicole and the
// partner's body is not here. 25.09.: the target filter keeps her from picking
// the ghost, but a swing aimed at the local Isaac still lands on a ghost
// standing next to him, and the report would have hurt the partner on the
// partner's machine, where that machine's Nicole never swung. The amount (DamageData+0x50) is zeroed
// before the component sees it, and nothing is reported.
volatile LONG localOnGhostZeroed = 0;
// DamageData+0x80 is m_attacker, a SafePtr holding the attacker's +0x10 (GhostAttacker below, the
// spectator's and the respawn's death lines). Read raw, the ghost tests here never matched:
// local_npc_on_ghost_zeroed and nicole ghost_hits_zeroed were 0 in every log. Both readings are
// tried, the SafePtr's first, as bot::BotBehind does.
uint32_t AttackerEntity(const void* damage) {
    uint32_t ref = 0, vtable = 0;
    if (!damage || !Read(reinterpret_cast<uintptr_t>(damage) + 0x80, &ref, 4) || ref < 0x10010 || ref >= 0xFFFFFD00) return 0;
    for (const uint32_t who : {ref - 0x10, ref})
        if (Read(who, &vtable, 4) && vtable >= base + kCodeEnd && vtable < base + 0x1880000) return who;
    return 0;
}
bool LocalNpcHitsGhost(uint32_t component, const void* damage) {
    uint32_t owner = 0, attackerVtable = 0;
    if (!damage || !Read(component + kComponentOwner, &owner, 4) || !owner) return false;
    // a Dementia phantom hurts nobody but the Isaac who sees it: no ghost, bot or enemy
    if (phantom::Any() && owner != LocalCampaignPlayer()) {
        const uint32_t striker = AttackerEntity(damage);
        if (striker && phantom::Is(striker)) { InterlockedIncrement(&phantom::hitsZeroed); return true; }
    }
    if (!IsGhostPlayer(owner) && !IsGhostNecro(owner)) return false;
    const uint32_t attacker = AttackerEntity(damage);
    if (!attacker || !Read(attacker, &attackerVtable, 4)) return false;
    return npcs::LocalByVtable(attackerVtable) != nullptr || npcs::LocalNeutral(attackerVtable);
}
// Slot +0x2C is the damage gate, bool ShouldHandleDamage(DamageData const&)
// (PDB; the caller at 0x00EE0DB9 tests al and drops the hit on 0). Until 06.10.
// these detours were void and handed back whatever eax held: with the bots
// running every hit on a corpse was refused, so no corpse dropped its loot.
// The original's answer goes back unchanged. The Dismemberable gate calls the
// HealthComponent one for body parts 10 and up (0x00F736B7), both hooked: only the
// outer call observes, or a hit on a dead body was reported twice.
thread_local int healthDepth = 0;
__declspec(noinline) bool HealthDamageThunk(safetyhook::InlineHook& hook, void* component,
                                            const void* damage) {
    float before = 0.0f, after = 0.0f;
    const uint32_t object = reinterpret_cast<uint32_t>(component);
    if (enabled && LocalNpcHitsGhost(object, damage)) {
        const float zero = 0.0f;
        if (WriteBytes(reinterpret_cast<uintptr_t>(damage) + 0x50, &zero, 4)) InterlockedIncrement(&localOnGhostZeroed);
        return hook.thiscall<bool>(component, damage);
    }
    const bool readBefore = Read(object + kHealthCurrent, &before, 4);
    // the survival escalation: a companion bot's hit counts less (outer call only;
    // the amount is put back, an explosion hands the same data to the next body)
    float amount = 0.0f;
    bool scaled = false;
    if (!healthDepth) {
        // and the local Isaac's gun by its weapon level (runtime_coop_progress.inc)
        const float factor = escalation::BotHitFactor(damage, object) * progress::HitFactor(damage, object);
        if (factor != 1.0f && std::isfinite(factor) && Read(reinterpret_cast<uintptr_t>(damage) + 0x50, &amount, 4) && std::isfinite(amount)) {
            const float less = amount * factor;
            scaled = WriteBytes(reinterpret_cast<uintptr_t>(damage) + 0x50, &less, 4);
        }
    }
    ++healthDepth;
    const bool handled = hook.thiscall<bool>(component, damage);
    --healthDepth;
    if (scaled) WriteBytes(reinterpret_cast<uintptr_t>(damage) + 0x50, &amount, 4);
    if (!handled) InterlockedIncrement(&gateRefused);
    if (readBefore && !healthDepth) {
        float now = 0.0f;
        if (Read(object + kHealthCurrent, &now, 4)) {
            progress::OnHit(damage, object, before, now);   // weapon kill credit
            voices::OnHit(damage, object, before, now);     // three kills in a row
        }
    }
    if (!(enabled || bodyWatch) || !readBefore || healthDepth) return handled;
    __try {
        if (Read(object + kHealthCurrent, &after, 4)) {
            if (enabled) {
                OnHealthDamage(object, damage, before, after);
            } else if (after < before) {
                uint32_t entity = 0;
                if (Read(object + kComponentOwner, &entity, 4) && entity) necro::OnBodyHealth(entity, after);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
    return handled;
}

bool __fastcall OnHandleDamageHc(void* component, void*, const void* damage) {
    return HealthDamageThunk(damageHookHc, component, damage);
}
bool __fastcall OnHandleDamageDhc(void* component, void*, const void* damage) {
    return HealthDamageThunk(damageHookDhc, component, damage);
}

bool __fastcall OnKongDamage(void* self, void*, void* damage, void* out) {
    const uint32_t kong = reinterpret_cast<uint32_t>(self);
    const uint32_t attacker = AttackerEntity(damage);
    if (attacker && (IsGhostPlayer(attacker) || IsGhostNecro(attacker))) {
        const float zero = 0.0f;
        if (WriteBytes(reinterpret_cast<uintptr_t>(damage) + 0x50, &zero, 4)) InterlockedIncrement(&kongGhostZeroed);
        return kongHook.thiscall<bool>(self, damage, out);
    }
    float before = 0.0f, after = 0.0f;
    const bool readBefore = Read(kong + kKongArm, &before, 4);
    const bool result = kongHook.thiscall<bool>(self, damage, out);
    int32_t phase = 0;
    KongPacket p{kKongMagic, kKongVersion, peerReadiness.token, 0, {}, 0, 0.0f};
    if (!readBefore || !Read(kong + kKongArm, &after, 4) || !(after < before) || !Read(kong + kKongPhase, &phase, 4) ||
        !Read(kong + kEntityGuid, p.guid, sizeof(p.guid)))
        return result;
    InterlockedIncrement(&kongHits);
    if (dataSock == INVALID_SOCKET || peerTable.Count() == 0) return result;
    p.sequence = ++kongSequence;
    p.phase = phase;
    p.arm = after;
    if (SendToPeers(&p, sizeof(p))) InterlockedIncrement(&kongSent);
    kongRepeat = KongRepeat{p, GetTickCount(), 1};
    if (InterlockedIncrement(&kongLines) <= 40)
        gate::Event("coop_hits tormentor: our hit took its arm %.1f -> %.1f in phase %d, sent", before, after, static_cast<int>(phase));
    return result;
}
// A partner's arm health (receive loop, game thread).
bool OnKongBytes(const void* data, int size) {
    if (size != kKongBytes) return false;
    KongPacket p{};
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != kKongMagic || p.version != kKongVersion || !KnownPeerToken(p.token) || p.token == peerReadiness.token) return false;
    InterlockedIncrement(&kongReceived);
    if (!kongShared || !kongHook || !(p.arm >= 0.0f) || p.arm > kMaxSingleHit) return true;
    const uint32_t kong = mirror::ObjectByGuid(p.guid);
    if (!IsKong(kong)) { InterlockedIncrement(&kongNotFound); return true; }
    int32_t phase = -1;
    float arm = 0.0f;
    if (!Read(kong + kKongPhase, &phase, 4) || !Read(kong + kKongArm, &arm, 4)) return true;
    if (phase != p.phase) {
        if (InterlockedIncrement(&kongPhaseMismatch) <= 20)
            gate::Event("coop_hits tormentor: partner's arm %.1f in phase %d, ours is in phase %d: not taken", p.arm, static_cast<int>(p.phase),
                        static_cast<int>(phase));
        return true;
    }
    if (!(p.arm < arm - 0.01f)) { InterlockedIncrement(&kongNotLower); return true; }
    const uint32_t component = FindHealthComponent(kong);
    if (!component) { InterlockedIncrement(&kongNotFound); return true; }
    if (!WriteBytes(component + kPartHealth + kArmPart * 4, &p.arm, 4) || !WriteBytes(kong + kKongArm, &p.arm, 4)) {
        InterlockedIncrement(&kongFaults);
        return true;
    }
    InterlockedIncrement(&kongApplied);
    const bool raised = KongCheckAndRaise(kong);
    if (raised) {
        InterlockedIncrement(&kongRaised);
        std::memcpy(kongPending.guid, p.guid, 16);
        kongPending.phase = phase;
        kongPending.until = GetTickCount() + 6000;
        kongPending.lastRaise = GetTickCount();
        kongPending.used = true;
    }
    if (InterlockedIncrement(&kongLines) <= 40)
        gate::Event("coop_hits tormentor: partner's arm %.1f taken here (ours was %.1f, phase %d)%s", p.arm, arm, static_cast<int>(phase),
                    raised ? ", enough for this phase: raised" : "");
    return true;
}
// From Tick: the two repeats of our last result, and the flag raised again while
// a partner's result waits for our Kong to listen.
void KongTick(DWORD now) {
    RepeatKongStasis(now);
    if (kongRepeat.sent && kongRepeat.sent < 3 && now - kongRepeat.at >= (kongRepeat.sent == 1 ? 60u : 180u)) {
        if (SendToPeers(&kongRepeat.packet, sizeof(kongRepeat.packet))) InterlockedIncrement(&kongSent);
        ++kongRepeat.sent;
    }
    if (!kongPending.used) return;
    if (now - kongPending.until < 0x80000000u) { kongPending.used = false; return; }   // past its end
    if (now - kongPending.lastRaise < 500) return;
    kongPending.lastRaise = now;
    const uint32_t kong = mirror::ObjectByGuid(kongPending.guid);
    int32_t phase = -1;
    if (!IsKong(kong) || !Read(kong + kKongPhase, &phase, 4) || phase != kongPending.phase) { kongPending.used = false; return; }
    if (KongCheckAndRaise(kong)) InterlockedIncrement(&kongReraised);
}

// One of Nicole's states (vtable 0x01B74EC8, HandleStateMessage 0x010C9080: thiscall,
// five stack arguments, ret 0x14, message id the fourth, owner at +0x50) places her by
// her target's camera on message 0x22 and reads the target (NicoleBoss+0x6F8, SafePtr,
// set by 0x00FEDFA0) without a null check. A machine whose Isaac is a necro leaves its own
// Nicole without a target (dump 11920, 17.10. 20:01: the client crashed at 0x0136EC40
// reading 0x100). Without a target that message is skipped; she waits for one.
constexpr uint32_t kNicoleStateRva = 0xCC9080, kNicoleStateVtableRva = 0x1774EC8, kNicoleTarget = 0x6F8, kStateOwner = 0x50;
safetyhook::InlineHook nicoleStateHook;
volatile LONG nicoleNoTarget = 0;
unsigned char __fastcall OnNicoleState(void* sm, void*, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t message, uint32_t a4) {
    uint32_t owner = 0, vt = 0, target = 1;
    if (message == 0x22 && Read(reinterpret_cast<uintptr_t>(sm) + kStateOwner, &owner, 4) && owner && Read(owner, &vt, 4) &&
        vt == base + kNicoleVtableRva && Read(owner + kNicoleTarget, &target, 4) && !target) {
        if (InterlockedIncrement(&nicoleNoTarget) == 1)
            gate::Event("test nicole_target INFO: her state %08X has no target (owner %08X); message 22 skipped until she has one", reinterpret_cast<uintptr_t>(sm), owner);
        return 0;
    }
    return nicoleStateHook.thiscall<unsigned char>(sm, a0, a1, a2, message, a4);
}

// A local hit on the local Nicole. The amount is read after the original ran, with
// the weak spot and weapon modifiers applied. A hit that set the flag had its
// amount zeroed; it goes out as kMaxSingleHit, which is lethal on any copy.
void __fastcall OnNicoleModifiers(void* self, void*, void* damage) {
    const uint32_t entity = reinterpret_cast<uint32_t>(self);
    unsigned char flagBefore = 0;
    float health = 0.0f;
    const bool readBefore = Read(entity + kNicoleZeroFlag, &flagBefore, 1);
    const uint32_t component = readBefore ? FindHealthComponent(entity) : 0;
    if (component) Read(component + kHealthCurrent, &health, 4);
    // A partner's ghost hitting the local copy: the partner's machine counts
    // that hit and sends it, so it must not count a second time here. DamageData+0x80 is
    // m_attacker (a SafePtr, see AttackerEntity), +0x50 m_amount.
    const uint32_t attacker = enabled && worldHits ? AttackerEntity(damage) : 0;
    if (attacker && (IsGhostPlayer(attacker) || IsGhostNecro(attacker))) {
        const float zero = 0.0f;
        if (WriteBytes(reinterpret_cast<uintptr_t>(damage) + 0x50, &zero, 4)) InterlockedIncrement(&nicoleGhostZeroed);
        nicoleHook.thiscall<void>(self, damage);
        return;
    }
    nicoleHook.thiscall<void>(self, damage);
    if (!enabled || !worldHits) return;
    if (!component) { InterlockedIncrement(&nicoleNoComponent); return; }
    __try {
        unsigned char flagAfter = 0;
        float amount = 0.0f;
        uint32_t guid[4]{};
        if (!Read(entity + kNicoleZeroFlag, &flagAfter, 1) || !Read(reinterpret_cast<uintptr_t>(damage) + 0x50, &amount, 4) ||
            !Read(entity + kEntityGuid, guid, sizeof(guid))) return;
        const bool lethal = !flagBefore && flagAfter;
        const float report = lethal ? kMaxSingleHit : amount;
        if (!(report > 0.0f)) return;
        SendReport(coop_hit::kWorldSubKey, guid, report, coop_hit::kFlagWorld);
        InterlockedIncrement(lethal ? &nicoleLethalSent : &nicoleSent);
        if (lethal) gate::Event("coop_hits nicole: our hit reached her health (%.1f), partners get it as lethal", health);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&nicoleFaults);
    }
}

// A local, non-reflected hit on a world prop took health: send it as the
// engine's wire form. The attacker's net id is cleared, it names nothing on
// the other machine; extra hits are sent by their own ProcessDamage calls.
void SendPropHit(uint32_t damageable, const void* damage, float before) {
    float after = 0.0f;
    uint32_t owner = 0, proxy = 0;
    if (!Read(damageable + kDamageableHealth, &after, 4) || !(before - after > 0.0f)) return;
    if (!Read(damageable + kDamageableOwner, &owner, 4) || owner < 0x10000) return;
    WorldPacket p{};
    if (!Read(owner + kEntityReflectable + kReflectableProxy, &proxy, 4) || proxy || IsPlayerOrNecroEntity(owner) ||
        !Read(owner + kEntityGuid, p.guid, sizeof(p.guid)) || !(p.guid[0] | p.guid[1] | p.guid[2] | p.guid[3])) {
        InterlockedIncrement(&propSkippedOwner);
        return;
    }
    using NetFromDamage = void* (__thiscall*)(void*, const void*);
    alignas(16) unsigned char net[kNetDamageSize]{};
    reinterpret_cast<NetFromDamage>(base + kNetFromDamageRva)(net, damage);
    std::memset(net, 0, 8);   // m_attackerNetId
    net[0x2C] = 0; net[0x2D] = 0;   // m_numAdditionalHits, m_numAdditionalZeroDamageHits
    p.magic = kWorldMagic; p.version = kWorldVersion; p.token = peerReadiness.token; p.sequence = ++propSequence;
    std::memcpy(p.net, net, sizeof(net));
    if (dataSock == INVALID_SOCKET || peerTable.Count() == 0) return;
    if (terminalprop::Queue(p, owner, damageable, before, after, GetTickCount())) return;
    if (SendToPeers(&p, sizeof(p))) InterlockedIncrement(&propSent);
    else InterlockedIncrement(&sendFailed);
    // A burning prop is hit every frame; the counters carry the rest.
    if (gate::trace && InterlockedIncrement(&propSentLogged) <= kPropLogLines) gate::Event("coop_hits prop hit sent: %.1f (%.1f -> %.1f) guid=%08X-%08X-%08X-%08X", before - after, before, after,
                                 p.guid[0], p.guid[1], p.guid[2], p.guid[3]);
}

// The attacker of a DamageData (SafePtr at +0x80, the entity 0x10 below) is a
// partner's ghost: his own machine sends that hit as its prop packet, so a copy
// of it sent from here comes back to him twice (13.10., Marker Lab's power node).
bool GhostAttacker(const void* damage) {
    uint32_t ref = 0;
    return damage && Read(reinterpret_cast<uint32_t>(damage) + 0x80, &ref, 4) && ref > 0x10010 && ref < 0xFFFFFD00 && IsGhostPlayer(ref - 0x10);
}
volatile LONG propGhostHitsKept = 0;

void __fastcall OnProcessDamage(void* self, void*, void* damage, bool reflected) {
    const uint32_t damageable = reinterpret_cast<uint32_t>(self);
    float before = 0.0f;
    bool measure = propHits && enabled && !reflected && Read(damageable + kDamageableHealth, &before, 4) && before > 0.0f;
    if (measure && GhostAttacker(damage)) { measure = false; InterlockedIncrement(&propGhostHitsKept); }
    propHook.thiscall<void>(self, damage, reflected);
    if (!measure) return;
    __try {
        SendPropHit(damageable, damage, before);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&propFaults);
    }
}

// The DamageData cleanup of 0x010A0F10: attacker SafePtr at +0x80, the buffer at +4.
__declspec(noinline) bool ReleaseDamageData(unsigned char* data) {
    __try {
        uint32_t* attacker = reinterpret_cast<uint32_t*>(data + 0x80);
        if (*attacker) reinterpret_cast<void (__thiscall*)(void*, void*)>(base + kSafePtrReleaseRva)(reinterpret_cast<void*>(*attacker), attacker);
        *reinterpret_cast<uint32_t*>(data) = static_cast<uint32_t>(base + kDamageBaseVtableRva);
        reinterpret_cast<void (__cdecl*)(uint32_t)>(base + kDamageFreeRva)(*reinterpret_cast<uint32_t*>(data + 4));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The receiver sequence of 0x010A0F10 for one hit. Once the DamageData is
// constructed it is released on every path, a fault included.
__declspec(noinline) int ReplayPropHit(uint32_t entity, const unsigned char* netIn) {
    alignas(16) unsigned char net[kNetDamageSize];
    alignas(16) unsigned char data[0xB0];
    std::memcpy(net, netIn, sizeof(net));
    std::memset(data, 0, sizeof(data));
    uint32_t vtable = 0, query = 0;
    if (!Read(entity, &vtable, 4) || !Read(vtable + 0x10, &query, 4) || query < base + 0x1000 || query >= base + kCodeEnd) return 1;
    volatile int result = 2;
    volatile bool constructed = false;
    __try {
        void* iface = nullptr;
        using Query = bool (__thiscall*)(void*, uint32_t, void**);
        if (!reinterpret_cast<Query>(query)(reinterpret_cast<void*>(entity), kNetDamageInterface, &iface) || !iface) {
            result = 1;
        } else {
            using Ctor = void* (__thiscall*)(void*);
            using ToDamage = void (__thiscall*)(void*, void*);
            using Handle = void (__thiscall*)(void*, void*);
            reinterpret_cast<Ctor>(base + kDamageCtorRva)(data);
            constructed = true;
            reinterpret_cast<ToDamage>(base + kNetToDamageRva)(net, data);
            const uint32_t handle = (*reinterpret_cast<uint32_t**>(iface))[1];
            if (handle < base + 0x1000 || handle >= base + kCodeEnd) {
                result = 1;
            } else {
                reinterpret_cast<Handle>(handle)(iface, data);
                result = 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = 2;
    }
    if (constructed && !ReleaseDamageData(data)) InterlockedIncrement(&propCleanupFaults);
    return result;
}

void OnWorldPacket(const WorldPacket& p) {
    InterlockedIncrement(&propReceived);
    if (!propHits || !worldHits) { InterlockedIncrement(&worldSkipped); return; }
    for (const auto& seen : propSeen)
        if (seen.token == p.token && seen.sequence == p.sequence) { InterlockedIncrement(&propDuplicates); return; }
    propSeen[propSeenNext++ % std::size(propSeen)] = SeenHit{p.token, p.sequence};
    const uint32_t object = mirror::ObjectByGuid(p.guid);
    if (!object) { InterlockedIncrement(&propNotFound); return; }
    uint32_t proxy = 0;
    if (!Read(object + kEntityReflectable + kReflectableProxy, &proxy, 4) || proxy) { InterlockedIncrement(&propGhostTarget); return; }
    // A DamageableProp's IDamageable (+0x274, owner at +0x0C): its health and the invincible
    // bit ProcessDamage returns on, to tell a replay that took nothing from one that hit
    constexpr uint32_t kPropDamageable = 0x274;
    uint32_t owner = 0, flagsBefore = 0;
    float healthBefore = 0.0f, healthAfter = 0.0f;
    const bool measured = Read(object + kPropDamageable + kDamageableOwner, &owner, 4) && owner == object &&
                          Read(object + kPropDamageable + 4, &flagsBefore, 4) && Read(object + kPropDamageable + kDamageableHealth, &healthBefore, 4);
    const int result = ReplayPropHit(object, p.net);
    if (measured && result == 0 && Read(object + kPropDamageable + kDamageableHealth, &healthAfter, 4) && !(healthBefore - healthAfter > 0.0f)) {
        InterlockedIncrement((flagsBefore & 1) ? &propAbsorbedInvincible : &propAbsorbed);
        if (InterlockedIncrement(&propAbsorbedLogged) <= 40)
            gate::Event("coop_hits prop hit on %08X guid=%08X-%08X took nothing: health %.1f -> %.1f, %s", object, p.guid[0], p.guid[1], healthBefore, healthAfter,
                        (flagsBefore & 1) ? "INVINCIBLE here (its chain has not made it vulnerable on this machine)" : "not invincible");
    }
    if (result == 0) InterlockedIncrement(&propApplied);
    else if (result == 1) InterlockedIncrement(&propNoInterface);
    else InterlockedIncrement(&propFaults);
    if (gate::trace && InterlockedIncrement(&propReplayLogged) <= kPropLogLines) gate::Event("coop_hits prop hit replayed on %08X guid=%08X-%08X-%08X-%08X result=%d", object,
                                 p.guid[0], p.guid[1], p.guid[2], p.guid[3], result);
}

void __cdecl OnDirectSendDamage(void* entity, const void* damage) {
    InterlockedIncrement(&damageCalls);
    __try {
        const uint32_t object = reinterpret_cast<uint32_t>(entity);
        if (object) {
            const uint32_t reflectable = object + kEntityReflectable;
            uint32_t proxy = 0, subKey = 0, guid[4]{};
            Read(reflectable + kReflectableProxy, &proxy, 4);
            Read(static_cast<uint32_t>(reflectable + kSubKeyFromReflectable), &subKey, 4);
            Read(object + kEntityGuid, guid, sizeof(guid));
            if (proxy) {
                InterlockedIncrement(&withProxy);
                const bool mine = Authoritative(proxy);
                if (mine) {
                    InterlockedIncrement(&authoritative);
                } else {
                    // The case the engine drops: a hit on something another
                    // machine owns. This is every shot the client fires at an enemy.
                    // Counted here, reported from the health hook, which is the
                    // only place the amount is knowable.
                    InterlockedIncrement(&foreign);
                }
                Record(object, proxy, subKey, guid, mine, damage);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
    // The engine's own behaviour is untouched, whatever happened above.
    sendHook.call<void>(entity, damage);
}

// The two HandleDamage bodies, where the amount is measured as the health
// that went away. Both must match, or neither is hooked: half of this is
// worse than none, because enemies would then converge only sometimes.
bool InstallHealthHooks(FILE* log, const char* consequence) {
    if (damageHookHc && damageHookDhc) return true;
    struct Site { uint32_t rva; const unsigned char* bytes; size_t size;
                  safetyhook::InlineHook* hook; void* detour; const char* name; };
    static const unsigned char kHc[] = {0x83,0xB9,0xCC,0x00,0x00,0x00,0x02,0x53,0x56,0x57};
    static const unsigned char kDhc[] = {0x83,0xEC,0x08,0x56,0x8B,0xF1,0x83,0xBE,0xCC,0x00,0x00,0x00,0x02};
    const Site sites[] = {
        {kHandleDamageHcRva, kHc, sizeof(kHc), &damageHookHc,
         reinterpret_cast<void*>(OnHandleDamageHc), "HealthComponent"},
        {kHandleDamageDhcRva, kDhc, sizeof(kDhc), &damageHookDhc,
         reinterpret_cast<void*>(OnHandleDamageDhc), "DismemberableHealthComponent"},
    };
    for (const auto& site : sites) {
        unsigned char seen[16]{};
        if (site.size > sizeof(seen) || !Read(base + site.rva, seen, site.size) ||
            std::memcmp(seen, site.bytes, site.size) != 0) {
            std::fprintf(log, "coop_hits %s HandleDamage signature mismatch at %08X; %s\n",
                         site.name, static_cast<unsigned>(base + site.rva), consequence);
            return false;
        }
    }
    for (const auto& site : sites) {
        *site.hook = safetyhook::create_inline(reinterpret_cast<void*>(base + site.rva), site.detour);
        if (!*site.hook) {
            std::fprintf(log, "coop_hits %s HandleDamage hook failed; %s\n", site.name, consequence);
            return false;
        }
    }
    return true;
}

// [Necro] DeathRevert learns of the necro's death from the health hooks. With
// [Coop] ReportHits=0 they are installed for that alone: nothing is reported.
bool InstallBodyWatch(FILE* log) {
    if (enabled) {
        std::fprintf(log, "coop_hits body watch: served by the hit report hooks\n");
        return true;
    }
    bodyWatch = InstallHealthHooks(log, "necro DeathRevert cannot see the necro die");
    std::fprintf(log, "coop_hits body watch %s (health hooks for necro DeathRevert only, no hit reports)\n",
                 bodyWatch ? "installed" : "FAILED");
    return bodyWatch;
}

bool Install(FILE* log) {
    if (!enabled) return true;
    // push ebp / mov ebp,esp / and esp,0xF8 / sub esp,0xB4 / push ebx / push esi
    static const unsigned char kSite[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF8,0x81,0xEC,
                                          0xB4,0x00,0x00,0x00,0x53,0x56};
    unsigned char actual[sizeof(kSite)]{};
    if (!Read(base + kDirectSendDamageRva, actual, sizeof(actual)) ||
        std::memcmp(actual, kSite, sizeof(actual)) != 0) {
        std::fprintf(log, "coop_hits DirectSendDamageEvent signature mismatch at %08X; hit reports off\n",
                     static_cast<unsigned>(base + kDirectSendDamageRva));
        enabled = false;
        return false;
    }
    sendHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kDirectSendDamageRva),
                                         reinterpret_cast<void*>(OnDirectSendDamage));
    if (!sendHook) {
        std::fprintf(log, "coop_hits hook failed at %08X; hit reports off\n",
                     static_cast<unsigned>(base + kDirectSendDamageRva));
        enabled = false;
        return false;
    }
    if (!InstallHealthHooks(log, "hit reports off")) {
        enabled = false;
        return false;
    }
    if (propHits) {
        // push esi / mov esi,ecx / test byte [esi+4],1 / jne
        static const unsigned char kProcess[] = {0x56,0x8B,0xF1,0xF6,0x46,0x04,0x01,0x0F,0x85};
        static const unsigned char kNetCtor[] = {0x0F,0x57,0xC0,0x33,0xC0,0x89,0x01};
        static const unsigned char kToDamage[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x81,0xEC,0xA4,0x00,0x00,0x00};
        unsigned char a[sizeof(kProcess)]{}, b[sizeof(kNetCtor)]{}, c[sizeof(kToDamage)]{};
        // The randomizer's listener guard wraps ProcessDamage too; a finished hook there is chained.
        hook_site::Lock siteLock(static_cast<uint32_t>(base + kProcessDamageRva));
        if (!Read(base + kProcessDamageRva, a, sizeof(a)) ||
            hook_site::Inspect(a, kProcess, sizeof(a)) == hook_site::State::Foreign ||
            !Read(base + kNetFromDamageRva, b, sizeof(b)) || std::memcmp(b, kNetCtor, sizeof(b)) ||
            !Read(base + kNetToDamageRva, c, sizeof(c)) || std::memcmp(c, kToDamage, sizeof(c))) {
            std::fprintf(log, "coop_hits world prop signatures mismatch (ProcessDamage %08X); prop hits stay local\n",
                         static_cast<unsigned>(base + kProcessDamageRva));
            propHits = false;
        } else {
            if (!hook_site::Install(log, "coop_hits ProcessDamage", base + kProcessDamageRva, kProcess, sizeof(kProcess),
                                    reinterpret_cast<void*>(OnProcessDamage), propHook))
                propHits = false;
            std::fprintf(log, "coop_hits world prop hits %s at %08X\n", propHook ? "installed" : "hook failed",
                         static_cast<unsigned>(base + kProcessDamageRva));
        }
    }
    if (nicoleShared) {
        static const unsigned char kNicole[] = {0x56,0x57,0x8B,0x7C,0x24,0x0C,0x57,0x8B,0xF1,0xE8};
        unsigned char seen[sizeof(kNicole)]{};
        uint32_t slot = 0;
        if (!Read(base + kNicoleModifiersRva, seen, sizeof(seen)) || std::memcmp(seen, kNicole, sizeof(seen)) != 0 ||
            !Read(base + kNicoleVtableRva + 0x210, &slot, 4) || slot != base + kNicoleModifiersRva) {
            std::fprintf(log, "coop_hits NicoleBoss::ApplyDamageModifiers signature mismatch at %08X (vtable slot %08X); her hits stay local\n",
                         static_cast<unsigned>(base + kNicoleModifiersRva), slot);
            nicoleShared = false;
        } else {
            nicoleHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kNicoleModifiersRva), reinterpret_cast<void*>(OnNicoleModifiers));
            if (!nicoleHook) nicoleShared = false;
            std::fprintf(log, "coop_hits NicoleBoss damage sharing %s at %08X\n", nicoleHook ? "installed" : "hook failed",
                         static_cast<unsigned>(base + kNicoleModifiersRva));
        }
    }
    {
        // push ebp / mov ebp,esp / and esp,-16 / sub esp,184h / push ebx / push esi / mov esi,ecx / mov ecx,[ebp+14h]
        static const unsigned char kState[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x84, 0x01, 0x00, 0x00, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x4D, 0x14};
        uint32_t slot = 0;
        if (!Read(base + kNicoleStateVtableRva + 8, &slot, 4) || slot != base + kNicoleStateRva)
            std::fprintf(log, "coop_hits nicole target guard: state vtable slot %08X is not %08X; not installed\n", slot,
                         static_cast<unsigned>(base + kNicoleStateRva));
        else if (!hook_site::Install(log, "coop_hits nicole target guard (state message 22)", base + kNicoleStateRva, kState, sizeof(kState),
                                     reinterpret_cast<void*>(OnNicoleState), nicoleStateHook))
            std::fprintf(log, "coop_hits: without the nicole target guard a necro machine can crash in her camera state\n");
    }
    if (kongShared) {
        // sub esp,8 / push ebx / push ebp / push esi / mov esi,[esp+0x18] / xor al,al / push edi / mov edi,ecx
        static const unsigned char kKong[] = {0x83, 0xEC, 0x08, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x18, 0x32, 0xC0, 0x57, 0x8B, 0xF9};
        // mov edx,[ecx+0x24C] / xor al,al / cmp edx,5
        static const unsigned char kCheck[] = {0x8B, 0x91, 0x4C, 0x02, 0x00, 0x00, 0x32, 0xC0, 0x83, 0xFA, 0x05};
        // push esi / mov edx,ecx / call
        static const unsigned char kRaise[] = {0x56, 0x8B, 0xD1, 0xE8};
        unsigned char a[sizeof(kKong)]{}, b[sizeof(kCheck)]{}, c[sizeof(kRaise)]{};
        uint32_t slot = 0;
        if (!Read(base + kKongHandleDamageRva, a, sizeof(a)) || std::memcmp(a, kKong, sizeof(a)) != 0 ||
            !Read(base + kKongArmCheckRva, b, sizeof(b)) || std::memcmp(b, kCheck, sizeof(b)) != 0 ||
            !Read(base + kKongRaiseRva, c, sizeof(c)) || std::memcmp(c, kRaise, sizeof(c)) != 0 ||
            !Read(base + kKongVtableRva + 0x1F4, &slot, 4) || slot != base + kKongHandleDamageRva) {
            std::fprintf(log, "coop_hits Kong::HandleDamage signature mismatch at %08X (vtable slot %08X); the Tormentor's arm stays local\n",
                         static_cast<unsigned>(base + kKongHandleDamageRva), slot);
            kongShared = false;
        } else {
            kongHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kKongHandleDamageRva), reinterpret_cast<void*>(OnKongDamage));
            if (!kongHook) kongShared = false;
            std::fprintf(log, "coop_hits Tormentor arm sharing %s at %08X\n", kongHook ? "installed" : "hook failed",
                         static_cast<unsigned>(base + kKongHandleDamageRva));
        }
    }
    std::fprintf(log, "coop_hits installed: broadcast=%08X damage_hc=%08X damage_dhc=%08X apply=%d\n",
                 static_cast<unsigned>(base + kDirectSendDamageRva),
                 static_cast<unsigned>(base + kHandleDamageHcRva),
                 static_cast<unsigned>(base + kHandleDamageDhcRva), applyHits ? 1 : 0);
    return true;
}

void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    if (!enabled) return;
    if (kongShared) {
        static LONG lastKong = -1;
        const LONG kongTotal = kongHits + kongReceived + kongReraised + kongGhostZeroed + kongFaults;
        if (kongTotal != lastKong) {
            lastKong = kongTotal;
            std::fprintf(log, "coop_hits tormentor hits=%ld sent=%ld | received=%ld applied=%ld not_lower=%ld phase_mismatch=%ld not_found=%ld raised=%ld "
                              "reraised=%ld | ghost_zeroed=%ld faults=%ld\n",
                         kongHits, kongSent, kongReceived, kongApplied, kongNotLower, kongPhaseMismatch, kongNotFound, kongRaised, kongReraised,
                         kongGhostZeroed, kongFaults);
        }
    }
    static LONG terminalLast = -1;
    const LONG terminalTotal = terminalprop::queued + terminalprop::retried + terminalprop::missing + terminalprop::mismatched + terminalprop::duplicates + terminalprop::delivered;
    if (terminalTotal != terminalLast) {
        terminalLast = terminalTotal;
        std::fprintf(log, "coop_hits terminal props queued=%ld overwritten=%ld retries=%ld absent=%ld mismatched=%ld duplicates=%ld dispatched=%ld\n",
            terminalprop::queued, terminalprop::overwritten, terminalprop::retried, terminalprop::missing, terminalprop::mismatched, terminalprop::duplicates, terminalprop::delivered);
    }
    const LONG total = InterlockedCompareExchange(&damageCalls, 0, 0) +
                       InterlockedCompareExchange(&resolved, 0, 0) +
                       InterlockedCompareExchange(&unresolved, 0, 0) +
                       InterlockedCompareExchange(&worldReported, 0, 0) + InterlockedCompareExchange(&worldApplied, 0, 0) +
                       InterlockedCompareExchange(&nicoleSent, 0, 0) + InterlockedCompareExchange(&nicoleLethalSent, 0, 0) +
                       InterlockedCompareExchange(&nicoleApplied, 0, 0) + InterlockedCompareExchange(&nicoleFlagged, 0, 0) +
                       InterlockedCompareExchange(&nicoleHidden, 0, 0) + InterlockedCompareExchange(&nicoleNoComponent, 0, 0) +
                       InterlockedCompareExchange(&nicoleGhostZeroed, 0, 0) +
                       InterlockedCompareExchange(&propSent, 0, 0) + InterlockedCompareExchange(&propReceived, 0, 0) +
                       InterlockedCompareExchange(&propSkippedOwner, 0, 0) + InterlockedCompareExchange(&worldProxyTarget, 0, 0);
    if (total == last) return;
    last = total;
    std::fprintf(log, "coop_hits calls=%ld with_proxy=%ld mine=%ld foreign=%ld sent=%ld "
                      "send_failed=%ld faults=%ld | recv=%u resolved=%ld unresolved=%ld "
                      "guid_mismatch=%ld repeat_copies=%u apply=%d applied=%ld no_component=%ld "
                      "apply_faults=%ld apply_refused=%ld world_reported=%ld world_applied=%ld world_not_found=%ld world_skipped=%ld world_solo=%ld "
                      "health_scans=%ld health_cache_hits=%ld\n",
                 InterlockedCompareExchange(&damageCalls, 0, 0),
                 InterlockedCompareExchange(&withProxy, 0, 0),
                 InterlockedCompareExchange(&authoritative, 0, 0),
                 InterlockedCompareExchange(&foreign, 0, 0),
                 InterlockedCompareExchange(&reported, 0, 0),
                 InterlockedCompareExchange(&sendFailed, 0, 0),
                 InterlockedCompareExchange(&faults, 0, 0),
                 receiver.received,
                 InterlockedCompareExchange(&resolved, 0, 0),
                 InterlockedCompareExchange(&unresolved, 0, 0),
                 InterlockedCompareExchange(&guidMismatch, 0, 0),
                 receiver.stale, applyHits ? 1 : 0,
                 InterlockedCompareExchange(&applied, 0, 0),
                 InterlockedCompareExchange(&applyNoComponent, 0, 0),
                 InterlockedCompareExchange(&applyFaults, 0, 0),
                 InterlockedCompareExchange(&applyRefused, 0, 0),
                 InterlockedCompareExchange(&worldReported, 0, 0), InterlockedCompareExchange(&worldApplied, 0, 0),
                 InterlockedCompareExchange(&worldNotFound, 0, 0), InterlockedCompareExchange(&worldSkipped, 0, 0),
                 InterlockedCompareExchange(&worldSolo, 0, 0), InterlockedCompareExchange(&healthScans, 0, 0),
                 InterlockedCompareExchange(&healthCacheHits, 0, 0));
    std::fprintf(log, "coop_hits local_npc_on_ghost_zeroed=%ld damage_gate_refused=%ld (the game's own no to a hit, passed on) watcher_hits_kept=%ld\n",
                 InterlockedCompareExchange(&localOnGhostZeroed, 0, 0), InterlockedCompareExchange(&gateRefused, 0, 0),
                 InterlockedCompareExchange(&watcherHitsKept, 0, 0));
    std::fprintf(log, "coop_hits lag copies_sent=%ld copies_overwritten=%ld clamped_reports=%ld not_owner=%ld world_proxy_target=%ld | partner hits on our bots refused=%ld floored=%ld\n",
                 InterlockedCompareExchange(&copiesSent, 0, 0), InterlockedCompareExchange(&repeatsOverwritten, 0, 0),
                 InterlockedCompareExchange(&clampedReports, 0, 0), InterlockedCompareExchange(&notOwner, 0, 0),
                 InterlockedCompareExchange(&worldProxyTarget, 0, 0), InterlockedCompareExchange(&botHitsRefused, 0, 0),
                 InterlockedCompareExchange(&botHitsFloored, 0, 0));
    if (propHits)
        std::fprintf(log, "coop_hits props sent=%ld received=%ld applied=%ld (took nothing %ld, invincible %ld) duplicates=%ld not_found=%ld ghost_target=%ld no_interface=%ld "
                     "skipped_owner=%ld ghost_hits_not_sent=%ld faults=%ld cleanup_faults=%ld\n",
                     InterlockedCompareExchange(&propSent, 0, 0), InterlockedCompareExchange(&propReceived, 0, 0),
                     InterlockedCompareExchange(&propApplied, 0, 0), InterlockedCompareExchange(&propAbsorbed, 0, 0),
                     InterlockedCompareExchange(&propAbsorbedInvincible, 0, 0), InterlockedCompareExchange(&propDuplicates, 0, 0),
                     InterlockedCompareExchange(&propNotFound, 0, 0), InterlockedCompareExchange(&propGhostTarget, 0, 0),
                     InterlockedCompareExchange(&propNoInterface, 0, 0), InterlockedCompareExchange(&propSkippedOwner, 0, 0),
                     InterlockedCompareExchange(&propGhostHitsKept, 0, 0), InterlockedCompareExchange(&propFaults, 0, 0),
                     InterlockedCompareExchange(&propCleanupFaults, 0, 0));
    if (nicoleShared)
        std::fprintf(log, "coop_hits nicole sent=%ld lethal_sent=%ld no_component=%ld applied=%ld flagged=%ld skipped_hidden=%ld ghost_hits_zeroed=%ld faults=%ld\n",
                     InterlockedCompareExchange(&nicoleSent, 0, 0), InterlockedCompareExchange(&nicoleLethalSent, 0, 0),
                     InterlockedCompareExchange(&nicoleNoComponent, 0, 0), InterlockedCompareExchange(&nicoleApplied, 0, 0),
                     InterlockedCompareExchange(&nicoleFlagged, 0, 0), InterlockedCompareExchange(&nicoleHidden, 0, 0),
                     InterlockedCompareExchange(&nicoleGhostZeroed, 0, 0), InterlockedCompareExchange(&nicoleFaults, 0, 0));
    if (nicoleStateHook)
        std::fprintf(log, "coop_hits nicole target guard skips=%ld (state message 22 without a target)\n", InterlockedCompareExchange(&nicoleNoTarget, 0, 0));
    const LONG count = InterlockedCompareExchange(&sampleCount, 0, 0);
    static LONG flushed = 0;
    for (; flushed < count && flushed < static_cast<LONG>(kSampleCapacity); ++flushed) {
        Sample& s = samples[flushed];
        if (!InterlockedCompareExchange(&s.ready, 0, 0)) break;
        std::fprintf(log, "coop_hit_sample entity=%08X proxy=%08X subkey=%08X mine=%d "
                          "guid=%08X-%08X-%08X-%08X payload=%08X,%08X,%08X,%08X,%08X,%08X\n",
                     s.entity, s.proxy, s.subKey, s.authoritative,
                     s.guid[0], s.guid[1], s.guid[2], s.guid[3],
                     s.words[0], s.words[1], s.words[2], s.words[3], s.words[4], s.words[5]);
    }
}
}  // namespace hits
