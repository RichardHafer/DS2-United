// Dead Space 2 United by Hafer.
// The MP objectives' damage on our MP-map visits ([MpMap] ObjectiveDamage).
// The objectives are DamageableProps with an MPDamageFilter GUID at attrs+0x124
// and a fixed authored health. In an MP round MPDamageFilter::ApplyFilter
// (0x01084F60) multiplies every human hit by m_numTargetHumans (4 in all nine
// filters) / the human team's Blaze count, so one human does four times the
// damage. That path sits behind DamageableProp flag 0x20000 (+0x2C0, set at
// spawn only with the LevelManager's MP bit), which is clear here: every hit
// counted once, as if four humans shot. Evidence: scratchpad objectives/,
// pc_dp_handleevents.txt and pc_applyfilter.txt (10.10.).
//
// DamageableProp::HandleEvents (0x00ED9750, thiscall(this, CMsg&), ret 4) is
// hooked at its entry: on iMsgDamage ([0x0209EE68]) for a prop with a filter
// GUID and the flag clear, the hit's DamageData (handle [msg+8] in the table at
// [0x0201C0F4], 8-byte entries indexed by handle >> 23, [entry+4] == handle)
// gets amount +0x50 times 4 / team. The team is this machine and its fresh
// partners, plus the bots with [MpMap] ObjectiveTeamCountsBots=1 (off since 12.10.). Only hits
// from a Player (attacker SafePtr +0x80 minus 0x10) or with the human team bits
// (+0x90 & 5) are scaled. A partner's hit comes in already scaled through
// coop_hits and is replayed past HandleEvents, so nothing is scaled twice.
//
// The Objective mode's tuning ([Objective] in DS2Survival.ini, read on every
// machine, only while an MP-map visit plays Objective):
// - PropHealthFactor: every hit on an objective prop (filter GUID set) is divided
//   by it in the same hook, so the prop lasts that many times as long and its
//   health bar and 75/50/25 % messages stay true. The only other ways in are
//   IDamageable::Kill (0x00DC5370, a scripted kill, left alone), the partner's
//   replay through INetDamage (0x00E92A30, already divided by his machine) and
//   the defend hits below (divided there).
// - HoldPerPlayerPercent: see hold below.
// - ContactBeamPercent: a Contact Beam hit (DamageData+0x94 inflictor bit 0x200, as
//   coop_progress' weapon kills read it) on an objective prop counts this percent in
//   Objective and Versus visits (user, 16.10.: the beam took the objectives too fast).
namespace gamemode { bool PlaysObjective(); extern volatile LONG mode; }   // runtime_coop_gamemode.inc
namespace objectives {
bool enabled = false, countBots = true;
constexpr uint32_t kHandleEventsRva = 0xAD9750, kMsgDamage = 0x1C9EE68, kDamageTable = 0x1C1C0F4;
constexpr uint32_t kVtableSlots[] = {0x175E67C, 0x16A8D34};   // DamageableProp vtable slot 1, and its other user
constexpr uint32_t kFlags = 0x2C0, kFlagMpGame = 0x20000, kAttrs = 0x0C, kFilterGuid = 0x124, kInitialHealth = 0x2B4, kHealth = 0x2B8;
constexpr unsigned kTargetHumans = 4;
safetyhook::InlineHook hook;
volatile LONG damageMsgs = 0, scaled = 0, noFilter = 0, mpFlag = 0, staleHandle = 0, otherAttacker = 0, faults = 0;
unsigned lines = 0, otherLines = 0;
float lastFactor = 1.0f;

namespace tuning {
float healthFactor = 3.0f;   // [Objective] PropHealthFactor, 1 = off
unsigned holdPercent = 50;   // [Objective] HoldPerPlayerPercent, 0 = off
unsigned beamPercent = 50;   // [Objective] ContactBeamPercent, 100 = off
constexpr uint32_t kInflictorFlags = 0x94, kInflictorContactBeam = 0x200;
bool Active() { return mpmap::Arrived() && gamemode::PlaysObjective(); }
// Objective or Versus (gamemode 4) on an MP-map visit.
bool BeamActive() {
    return beamPercent != 100 && mpmap::Arrived() && (gamemode::PlaysObjective() || InterlockedCompareExchange(&gamemode::mode, 0, 0) == 4);
}
volatile LONG beamHits = 0;
// What a hit on an objective prop is divided by right now.
float HealthDivisor() { return healthFactor > 1.0f && Active() ? healthFactor : 1.0f; }

// One line per objective prop and arrival run; the counters carry the rest.
struct Seen { uint32_t guid[4]; };
Seen seen[24]{};
unsigned seenCount = 0, seenRun = 0xFFFFFFFFu;
volatile LONG propHits = 0, defendHits = 0, propsSeen = 0;
float damageIn = 0.0f, damageOut = 0.0f;   // game thread only, read for the log
void NoteProp(uint32_t prop, const uint32_t* filter, float in, float out, float divisor) {
    InterlockedIncrement(&propHits);
    damageIn += in;
    damageOut += out;
    if (mpmap::Run() != seenRun) { seenRun = mpmap::Run(); seenCount = 0; }
    uint32_t guid[4]{};
    Read(prop + 0x24, guid, 16);
    for (unsigned i = 0; i < seenCount; ++i)
        if (std::memcmp(seen[i].guid, guid, 16) == 0) return;
    if (seenCount >= std::size(seen)) return;
    std::memcpy(seen[seenCount++].guid, guid, 16);
    InterlockedIncrement(&propsSeen);
    float initial = 0.0f, health = 0.0f;
    Read(prop + kInitialHealth, &initial, 4);
    Read(prop + kHealth, &health, 4);
    gate::Event("objectives health: prop %08X %08X-%08X-%08X-%08X (filter %08X-%08X) health %.0f of %.0f: hits / %.2f in Objective mode, lasts like %.0f "
                "(first hit %.1f -> %.1f)",
                prop, guid[0], guid[1], guid[2], guid[3], filter[0], filter[1], health, initial, divisor, initial * divisor, in, out);
}
}  // namespace tuning

unsigned Team(unsigned& humans, unsigned& bots) {
    humans = 1;
    if (network) {
        const DWORD now = GetTickCount();
        for (const auto& peer : peerTable.peers)
            if (peer.used && peer.token && now - peer.lastSeen <= netclock::kPeerFreshMs) ++humans;
    }
    bots = 0;
    for (uint32_t b : botEntities) if (b) ++bots;
    const unsigned team = humans + (countBots ? bots : 0);
    return team < 1 ? 1 : team > kTargetHumans ? kTargetHumans : team;
}

// The DamageData belongs to the message's handle, not to the prop, so the amount
// is put back once the prop has taken its hit: whatever else that data reaches
// (another target of the same blast) gets it as the game made it.
struct Written { uint32_t data = 0, handle = 0; float before = 0.0f, after = 0.0f; };
volatile LONG amountsRestored = 0, restoreGone = 0, restoreChanged = 0;
void Restore(const Written& w) {
    uint32_t table = 0, entryHandle = 0, data = 0;
    float now = 0.0f;
    if (!Read(base + kDamageTable, &table, 4) || !table) { InterlockedIncrement(&restoreGone); return; }
    const uint32_t entry = table + (w.handle >> 23) * 8;
    if (!Read(entry + 4, &entryHandle, 4) || entryHandle != w.handle || !Read(entry, &data, 4) || data != w.data) { InterlockedIncrement(&restoreGone); return; }
    if (!Read(data + 0x50, &now, 4) || now != w.after) { InterlockedIncrement(&restoreChanged); return; }
    if (WriteBytes(data + 0x50, &w.before, 4)) InterlockedIncrement(&amountsRestored);
}

__declspec(noinline) void Scale(uint32_t prop, uint32_t msg, Written& w) {
    uint32_t id = 0, damageId = 0;
    if (!Read(msg, &id, 4) || !Read(base + kMsgDamage, &damageId, 4) || id != damageId) return;
    InterlockedIncrement(&damageMsgs);
    uint32_t flags = 0, attrs = 0, guid[4]{};
    if (!Read(prop + kFlags, &flags, 4) || !Read(prop + kAttrs, &attrs, 4) || !attrs || !Read(attrs + kFilterGuid, guid, 16)) return;
    if (!(guid[0] | guid[1] | guid[2] | guid[3])) { InterlockedIncrement(&noFilter); return; }
    if (flags & kFlagMpGame) InterlockedIncrement(&mpFlag);   // the native filter does the team part
    const bool teamWanted = enabled && !(flags & kFlagMpGame);
    const float divisor = tuning::HealthDivisor();
    const bool beamRule = tuning::BeamActive();
    if (!teamWanted && divisor == 1.0f && !beamRule) return;
    uint32_t handle = 0, table = 0, entryHandle = 0, data = 0;
    if (!Read(msg + 8, &handle, 4) || !Read(base + kDamageTable, &table, 4) || !table) { InterlockedIncrement(&staleHandle); return; }
    const uint32_t entry = table + (handle >> 23) * 8;
    if (!Read(entry + 4, &entryHandle, 4) || entryHandle != handle || !Read(entry, &data, 4) || !data) { InterlockedIncrement(&staleHandle); return; }
    float amount = 0.0f;
    uint32_t attackerRef = 0, attacker = 0, attackerVtable = 0;
    uint16_t team = 0;
    if (!Read(data + 0x50, &amount, 4) || !std::isfinite(amount) || amount <= 0.0f || amount > 100000.0f) return;
    Read(data + 0x90, &team, 2);
    uint32_t inflictor = 0;
    Read(data + tuning::kInflictorFlags, &inflictor, 4);
    const float beam = beamRule && (inflictor & tuning::kInflictorContactBeam) ? static_cast<float>(tuning::beamPercent) / 100.0f : 1.0f;
    if (beam != 1.0f) InterlockedIncrement(&tuning::beamHits);
    if (Read(data + 0x80, &attackerRef, 4) && attackerRef > 0x10010) {
        attacker = attackerRef - 0x10;
        Read(attacker, &attackerVtable, 4);
    }
    const bool fromPlayer = attackerVtable == base + placement::kPlayerPrimaryVtableRva;
    const bool humanTeam = (team & 5) == 5;
    float factor = 1.0f;
    unsigned humans = 1, bots = 0, size = 0;
    if (teamWanted && !fromPlayer && !humanTeam) {
        InterlockedIncrement(&otherAttacker);
        if (otherLines < 12) {
            ++otherLines;
            gate::Event("objectives: hit on %08X not team scaled: attacker %08X vtable %08X team bits %04X amount %.1f%s", prop, attacker, attackerVtable, team,
                        amount, divisor != 1.0f ? " (still divided by the health factor)" : "");
        }
    } else if (teamWanted) {
        size = Team(humans, bots);
        factor = static_cast<float>(kTargetHumans) / static_cast<float>(size);
        lastFactor = factor;
    }
    // the health factor applies to every hit on an objective prop, whoever made it
    const float total = factor / divisor * beam;
    const float after = amount * total;
    if (total != 1.0f) {
        if (!WriteBytes(data + 0x50, &after, 4)) return;
        w = Written{data, handle, amount, after};
    }
    if (size) InterlockedIncrement(&scaled);
    if (divisor != 1.0f) tuning::NoteProp(prop, guid, amount * factor, after, divisor);
    if (lines < 40) {
        ++lines;
        uint32_t id24[4]{};
        float initial = 0.0f, health = 0.0f;
        Read(prop + 0x24, id24, 16);
        Read(prop + kInitialHealth, &initial, 4);
        Read(prop + kHealth, &health, 4);
        gate::Event("objectives: hit on %08X (%08X-%08X filter %08X-%08X) health %.0f/%.0f: %.1f x %.2f / %.2f x %.2f = %.1f (humans %u bots %u team %u, attacker %08X %s, "
                    "team bits %04X, weapon bits %04X, flags %08X)",
                    prop, id24[0], id24[1], guid[0], guid[1], health, initial, amount, factor, divisor, beam, after, humans, bots, size, attacker,
                    fromPlayer ? "a Player" : humanTeam ? "by team bits" : "other", team, inflictor & 0xFFFF, flags);
    }
}

// A partner's ghost hitting a prop here: his own machine applies that hit and
// sends it as a prop packet (coop_hits), so the copy his ghost lands here counted
// twice and was scaled and sent back to him (13.10., Marker Lab's power node).
volatile LONG ghostHitsZeroed = 0;
unsigned ghostLines = 0;
bool ZeroGhostHit(uint32_t prop, uint32_t msg) {
    uint32_t id = 0, damageId = 0, handle = 0, table = 0, entryHandle = 0, data = 0, attackerRef = 0;
    if (!Read(msg, &id, 4) || !Read(base + kMsgDamage, &damageId, 4) || id != damageId) return false;
    if (!Read(msg + 8, &handle, 4) || !Read(base + kDamageTable, &table, 4) || !table) return false;
    const uint32_t entry = table + (handle >> 23) * 8;
    if (!Read(entry + 4, &entryHandle, 4) || entryHandle != handle || !Read(entry, &data, 4) || !data) return false;
    if (!Read(data + 0x80, &attackerRef, 4) || attackerRef <= 0x10010 || attackerRef >= 0xFFFFFD00 || !IsGhostPlayer(attackerRef - 0x10)) return false;
    float amount = 0.0f;
    const float zero = 0.0f;
    Read(data + 0x50, &amount, 4);
    if (!WriteBytes(data + 0x50, &zero, 4)) return false;
    InterlockedIncrement(&ghostHitsZeroed);
    if (ghostLines < 12) {
        ++ghostLines;
        uint32_t id24[2]{};
        Read(prop + 0x24, id24, 8);
        gate::Event("objectives: hit on %08X (%08X-%08X) by the partner's ghost %08X zeroed (%.1f): his machine sends it", prop, id24[0], id24[1], attackerRef - 0x10,
                    amount);
    }
    return true;
}

uint32_t __fastcall OnHandleEvents(uint32_t prop, uint32_t, uint32_t msg) {
    Written w;
    if (msg && mpmap::visit.phase == coop_mpmap::Phase::Arrived) {
        __try {
            if (!(hits::propHits && hits::enabled && hits::worldHits && peerTable.Count() && ZeroGhostHit(prop, msg)))
                Scale(prop, msg, w);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&faults);
        }
    }
    const uint32_t result = hook.thiscall<uint32_t>(reinterpret_cast<void*>(static_cast<uintptr_t>(prop)), msg);
    if (w.data) {
        __try {
            Restore(w);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&faults);
        }
    }
    return result;
}

// Defend phases (the Objective mode). AI enemies never hit a prop; in an MP round
// only the necromorph players do. So the survival director hands over where its
// enemies are, and those near a defend target the map has made vulnerable damage
// it. The prop's IDamageable (+0x274: flags +0x04, bit 0 invincible; initial
// health +0x40; health +0x44) takes the hit through IDamageable::ProcessDamage
// (0x00DC5260, thiscall(const DamageData&, bool reflected), ret 8). Reflected
// skips the damage type and inflictor checks but still runs the health, the
// 75/50/25 % messages and the death message the map's chain listens to.
namespace defend {
constexpr uint32_t kDamageable = 0x274, kDamageableFlags = 0x04, kDamageableInitial = 0x40, kDamageableHealth = 0x44, kEntityPos = 0xC0;
constexpr uint32_t kProcessDamageRva = 0x9C5260, kDamageCtorRva = 0xACFDE0, kDamageDtorRva = 0x477540, kModGun = 4;
// The only props the humans defend: Titan Mines' bench and Fuel Core's three fuses
// and detonator (each one's invincibility is switched by its defend phase).
constexpr uint32_t kTargets[][4] = {
    {0x39CA6A29, 0x61D05A7C, 0x5052554D, 0x33315948},   // mp_bombRun bench, 1200
    {0x39CADBF6, 0xEFA0228A, 0x5052554D, 0x33315948},   // mp_detonate fuse 1, 500
    {0x39CB5AB5, 0x493E1757, 0x5052554D, 0x33315948},   // fuse 2, 550
    {0x39CB5B5E, 0x092236E4, 0x5052554D, 0x33315948},   // fuse 3, 600
    {0x39CADC4B, 0x6413D1C9, 0x5052554D, 0x33315948},   // detonator, 1000
};
bool ready = false;
volatile LONG ticks = 0, hitsDone = 0, vulnerable = 0, refused = 0, faults = 0;
float damageDone = 0.0f;
unsigned lines = 0;

bool IsProp(uint32_t object) {
    uint32_t vtable = 0, slot = 0;
    return object && Read(object, &vtable, 4) && Read(vtable + 4, &slot, 4) && slot == base + kHandleEventsRva;
}
int Hit(uint32_t prop, float amount, const float* at) {
    using Ctor = void*(__thiscall*)(void*, uint32_t, uint32_t, float, uint32_t, const float*, int, uint32_t, uint32_t, float);
    using Process = void(__thiscall*)(void*, const void*, bool);
    using Dtor = void(__thiscall*)(void*);
    alignas(16) unsigned char data[0xB0];
    std::memset(data, 0, sizeof(data));
    alignas(16) float where[4] = {at[0], at[1], at[2], 1.0f};
    volatile bool built = false;
    int result = 0;
    __try {
        reinterpret_cast<Ctor>(base + kDamageCtorRva)(data, 0, 0, amount, kModGun, where, 0, 0, 0, 0.0f);
        built = true;
        reinterpret_cast<Process>(base + kProcessDamageRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(prop + kDamageable)), data, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = 1;
    }
    if (built) {
        __try {
            reinterpret_cast<Dtor>(base + kDamageDtorRva)(data);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            result |= 2;
        }
    }
    if (result) InterlockedIncrement(&faults);
    return result;
}
// enemies: count x {x, y, z}. Returns how many defend targets are vulnerable now
// (0 none, -1 not ready); lowest gets the weakest one's health fraction.
int Tick(const float* enemies, int count, float seconds, float dps, float radius, float* lowest) {
    if (lowest) *lowest = -1.0f;
    if (!ready || !mpmap::InVisit()) return -1;
    InterlockedIncrement(&ticks);
    int open = 0;
    for (const auto& guid : kTargets) {
        const uint32_t prop = removals::FindBase(guid, 0);
        if (!IsProp(prop)) continue;
        uint32_t flags = 1;
        float health = 0.0f, initial = 0.0f;
        alignas(16) float pos[3]{};
        if (!Read(prop + kDamageable + kDamageableFlags, &flags, 4) || (flags & 1) || !Read(prop + kDamageable + kDamageableHealth, &health, 4) ||
            !(health > 0.0f) || !Read(prop + kDamageable + kDamageableInitial, &initial, 4) || !Read(prop + kEntityPos, pos, 12))
            continue;
        ++open;
        if (lowest && initial > 0.0f && (*lowest < 0.0f || health / initial < *lowest)) *lowest = health / initial;
        int close = 0;
        for (int i = 0; i < count && enemies; ++i) {
            const float dx = enemies[i * 3] - pos[0], dy = enemies[i * 3 + 1] - pos[1], dz = enemies[i * 3 + 2] - pos[2];
            if (dx * dx + dy * dy + dz * dz <= radius * radius) ++close;
        }
        if (!close || !(seconds > 0.0f) || !(dps > 0.0f)) continue;
        // ProcessDamage directly, past the hook: the health factor is applied here
        const float divisor = tuning::HealthDivisor();
        const float amount = close * dps * (seconds > 2.0f ? 2.0f : seconds) / divisor;
        if (Hit(prop, amount, pos) == 0) {
            InterlockedIncrement(&hitsDone);
            if (divisor != 1.0f) InterlockedIncrement(&tuning::defendHits);
            damageDone += amount;
            if (lines < 40) {
                ++lines;
                mpmap::Event("defend: %08X-%08X %u enemies within %.1f m hit it for %.1f (/ %.2f health factor; health %.0f of %.0f)", guid[0], guid[1], close,
                             radius, amount, divisor, health, initial);
            }
        }
    }
    if (open) InterlockedIncrement(&vulnerable);
    return open;
}
void Install(FILE* log) {
    // the damage path the turret uses (ctor with its vtable store, destructor) and ProcessDamage, ours or chained
    static const unsigned char kCtor[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x08, 0x53, 0x56, 0x8B, 0xF1, 0xE8};
    static const unsigned char kDtor[] = {0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0x80, 0x00, 0x00, 0x00, 0x8D, 0x86, 0x80, 0x00, 0x00, 0x00, 0x85, 0xC9};
    static const unsigned char kProcess[] = {0x56, 0x8B, 0xF1, 0xF6, 0x46, 0x04, 0x01, 0x0F, 0x85, 0xFC, 0x00, 0x00, 0x00, 0x57, 0x8B, 0x7C};
    unsigned char ctor[sizeof(kCtor)]{}, dtor[sizeof(kDtor)]{}, process[sizeof(kProcess)]{};
    const bool ctorOk = Read(base + kDamageCtorRva, ctor, sizeof(ctor)) && std::memcmp(ctor, kCtor, sizeof(ctor)) == 0;
    const bool dtorOk = Read(base + kDamageDtorRva, dtor, sizeof(dtor)) && std::memcmp(dtor, kDtor, sizeof(dtor)) == 0;
    const hook_site::State processState = Read(base + kProcessDamageRva, process, sizeof(process)) ? hook_site::Inspect(process, kProcess, sizeof(process))
                                                                                                    : hook_site::State::Foreign;
    ready = ctorOk && dtorOk && processState != hook_site::State::Foreign;
    std::fprintf(log, "objectives defend %s: DamageData ctor %s, dtor %s, ProcessDamage %08X %s; %zu defend targets\n", ready ? "ready" : "NOT ready",
                 ctorOk ? "verified" : "MISMATCH", dtorOk ? "verified" : "MISMATCH", static_cast<unsigned>(base + kProcessDamageRva),
                 hook_site::StateName(processState), std::size(kTargets));
}
void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&ticks, 0, 0) / 20 + InterlockedCompareExchange(&hitsDone, 0, 0) + InterlockedCompareExchange(&faults, 0, 0) * 1000;
    if (now == last || !InterlockedCompareExchange(&ticks, 0, 0)) return;
    last = now;
    std::fprintf(log, "objectives defend ticks=%ld vulnerable_ticks=%ld hits=%ld damage=%.0f faults=%ld\n", InterlockedCompareExchange(&ticks, 0, 0),
                 InterlockedCompareExchange(&vulnerable, 0, 0), InterlockedCompareExchange(&hitsDone, 0, 0), damageDone, InterlockedCompareExchange(&faults, 0, 0));
}
}  // namespace defend

// Hold switches in the Objective mode ([Objective] HoldPerPlayerPercent): each real
// player beyond the first makes a SimplePressAndHoldSwitch take that much longer;
// bots never count. The switch (vtable 0x01B5AC30, IUsable at +0x230, attrs +0x0C)
// keeps the hold in ms: RequiredUseTime at attrs+0x7C (uint, the authored seconds
// through Time::SecondsToMilliseconds) and the elapsed time at +0x254, to which
// HandleEvents (0x00ECF240) adds the frame's ms each tick while a user holds.
// GetProgressImpl (0x00EACE50) fills the meter with elapsed / required, the
// Player's ShouldExitUseObject ends the use once GetProgress turns -1 (past 1.0),
// and ExitUse (0x00ECF5F0) completes the switch only when elapsed >= required.
// So the scaled time goes into attrs+0x7C when a use starts: IUsable::EnterUse
// (0x00E994B0, only in slot +0x2C of the switch's IUsable vtable 0x01B5AAB8;
// thiscall(time, arg), ret 8, void), before its decay of an earlier hold reads it.
// Attributes may be shared by switches and outlive a level restart, so the
// authored value is remembered per attribute block and never scaled twice.
namespace hold {
constexpr uint32_t kEnterUseRva = 0xA994B0, kExitUseRva = 0xACF5F0, kProgressRva = 0xAACE50, kTickRva = 0xACF322, kSwitchEventsRva = 0xACF240;
constexpr uint32_t kVtableRva = 0x175AC30, kUsableVtableRva = 0x175AAB8, kUsable = 0x230, kRequiredMs = 0x7C, kElapsed = 0x254, kFrameMsRva = 0x1C4D400;
constexpr uint32_t kMinMs = 500, kMaxMs = 120000;   // the maps author 6..11 s
struct Entry { uint32_t attrs, authored, written; };
Entry entries[64]{};
unsigned used = 0, lines = 0;
safetyhook::InlineHook hook;
bool ready = false;
volatile LONG uses = 0, written = 0, kept = 0, restored = 0, outside = 0, notSwitch = 0, rejected = 0, full = 0, faults = 0, lastHumans = 0;
float lastFactor = 1.0f;

__declspec(noinline) void Apply(uint32_t sw) {
    uint32_t vtable = 0, attrs = 0, current = 0;
    if (!Read(sw, &vtable, 4) || vtable != base + kVtableRva) { InterlockedIncrement(&notSwitch); return; }
    if (!Read(sw + kAttrs, &attrs, 4) || !attrs || !Read(attrs + kRequiredMs, &current, 4)) { InterlockedIncrement(&rejected); return; }
    Entry* e = nullptr;
    for (unsigned i = 0; i < used; ++i)
        if (entries[i].attrs == attrs) { e = &entries[i]; break; }
    const bool active = tuning::holdPercent && tuning::Active();
    if (!active && !e) { InterlockedIncrement(&outside); return; }
    // a value written since by somebody else (a stream reloaded at the same address) is authored again
    const uint32_t authored = e && current == e->written ? e->authored : current;
    uint32_t guid[4]{};
    Read(sw + 0x24, guid, 16);
    if (authored < kMinMs || authored > kMaxMs) {
        InterlockedIncrement(&rejected);
        if (lines < 48) {
            ++lines;
            gate::Event("objectives hold: switch %08X %08X-%08X required %u ms is no hold time, left alone", sw, guid[0], guid[1], current);
        }
        return;
    }
    if (!e) {
        if (used >= std::size(entries)) {
            if (InterlockedIncrement(&full) <= 3) gate::Event("objectives hold: switch %08X %08X-%08X not scaled, %u attribute blocks known already", sw, guid[0], guid[1], used);
            return;
        }
        e = &entries[used++];
        e->attrs = attrs;
    }
    e->authored = authored;
    unsigned humans = 1, bots = 0;
    float factor = 1.0f;
    if (active) {
        Team(humans, bots);
        if (humans > kTargetHumans) humans = kTargetHumans;
        factor = 1.0f + static_cast<float>(tuning::holdPercent) / 100.0f * static_cast<float>(humans - 1);
        InterlockedExchange(&lastHumans, static_cast<LONG>(humans));
        lastFactor = factor;
    }
    const uint32_t target = static_cast<uint32_t>(static_cast<float>(authored) * factor + 0.5f);
    uint32_t elapsed = 0, elapsedAfter = 0;
    Read(sw + kElapsed, &elapsed, 4);
    elapsedAfter = elapsed;
    if (target != current) {
        if (!WriteBytes(attrs + kRequiredMs, &target, 4)) { InterlockedIncrement(&faults); return; }
        // an earlier hold's progress keeps its share of the meter
        if (elapsed && current) {
            elapsedAfter = static_cast<uint32_t>(static_cast<uint64_t>(elapsed) * target / current);
            WriteBytes(sw + kElapsed, &elapsedAfter, 4);
        }
        InterlockedIncrement(active ? &written : &restored);
    } else {
        InterlockedIncrement(&kept);
    }
    e->written = target;
    if (lines < 48) {
        ++lines;
        if (active)
            gate::Event("objectives hold: switch %08X %08X-%08X-%08X-%08X use starts: %.1f s authored -> %.1f s (x%.2f, %u human(s): this machine and %u partner(s), "
                        "%u bot(s) not counted; was %.1f s, progress %u -> %u ms)",
                        sw, guid[0], guid[1], guid[2], guid[3], authored / 1000.0, target / 1000.0, factor, humans, humans - 1, bots, current / 1000.0, elapsed,
                        elapsedAfter);
        else
            gate::Event("objectives hold: switch %08X %08X-%08X back to its authored %.1f s (was %.1f s): not an Objective visit", sw, guid[0], guid[1],
                        authored / 1000.0, current / 1000.0);
    }
}

void __fastcall OnEnterUse(uint32_t usable, uint32_t, uint32_t time, uint32_t arg) {
    InterlockedIncrement(&uses);
    __try {
        Apply(usable - kUsable);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
    hook.thiscall<void>(reinterpret_cast<void*>(static_cast<uintptr_t>(usable)), time, arg);
}

void Install(FILE* log) {
    if (!tuning::holdPercent) { std::fprintf(log, "objectives hold off ([Objective] HoldPerPlayerPercent=0)\n"); return; }
    // xorps xmm0,xmm0 / sub esp,8 / push esi / mov esi,[ecx-0x224] (attrs) / movss xmm1,[esi+0x80] (CooldownFraction) / comiss xmm1,xmm0
    static const unsigned char kEnter[] = {0x0F, 0x57, 0xC0, 0x83, 0xEC, 0x08, 0x56, 0x8B, 0xB1, 0xDC, 0xFD, 0xFF,
                                           0xFF, 0xF3, 0x0F, 0x10, 0x8E, 0x80, 0x00, 0x00, 0x00, 0x0F, 0x2F, 0xC8};
    // mov eax,[esi-0x224] / mov ecx,[esi+0x24] (elapsed) / xor bl,bl / push edi / cmp ecx,[eax+0x7C] / jb (not complete)
    static const unsigned char kExit[] = {0x8B, 0x86, 0xDC, 0xFD, 0xFF, 0xFF, 0x8B, 0x4E, 0x24, 0x32, 0xDB, 0x57, 0x3B, 0x48, 0x7C, 0x72, 0x4A};
    // mov eax,[ecx+0x254] / fild [ecx+0x254], then mov ecx,[ecx+0xC] / mov edx,[ecx+0x7C] / fild [ecx+0x7C]
    static const unsigned char kProgressA[] = {0x8B, 0x81, 0x54, 0x02, 0x00, 0x00, 0xDB, 0x81, 0x54, 0x02, 0x00, 0x00};
    static const unsigned char kProgressB[] = {0x8B, 0x49, 0x0C, 0x8B, 0x51, 0x7C, 0xDB, 0x41, 0x7C};
    // mov ecx,[frame ms] / add [esi+0x254],ecx
    unsigned char tick[] = {0x8B, 0x0D, 0, 0, 0, 0, 0x01, 0x8E, 0x54, 0x02, 0x00, 0x00};
    const uint32_t frameMs = static_cast<uint32_t>(base + kFrameMsRva);
    std::memcpy(tick + 2, &frameMs, 4);
    struct Check { const char* name; uint32_t rva; const unsigned char* bytes; size_t size; };
    const Check checks[] = {
        {"ExitUse elapsed >= RequiredUseTime", kExitUseRva + 5, kExit, sizeof(kExit)},
        {"GetProgressImpl elapsed", kProgressRva + 0x26, kProgressA, sizeof(kProgressA)},
        {"GetProgressImpl RequiredUseTime", kProgressRva + 0x3C, kProgressB, sizeof(kProgressB)},
        {"HandleEvents tick adds the frame ms", kTickRva, tick, sizeof(tick)},
    };
    unsigned checksOk = 0;
    for (const Check& c : checks) {
        unsigned char have[32]{};
        if (Read(base + c.rva, have, c.size) && std::memcmp(have, c.bytes, c.size) == 0) ++checksOk;
        else std::fprintf(log, "objectives hold: %s at %08X MISMATCH\n", c.name, static_cast<unsigned>(base + c.rva));
    }
    uint32_t enterSlot = 0, exitSlot = 0, eventsSlot = 0;
    Read(base + kUsableVtableRva + 0x2C, &enterSlot, 4);
    Read(base + kUsableVtableRva + 0x34, &exitSlot, 4);
    Read(base + kVtableRva + 4, &eventsSlot, 4);
    const bool slotsOk = enterSlot == base + kEnterUseRva && exitSlot == base + kExitUseRva && eventsSlot == base + kSwitchEventsRva;
    if (checksOk == std::size(checks) && slotsOk)
        ready = hook_site::Install(log, "objectives hold SimplePressAndHoldSwitch::EnterUse", base + kEnterUseRva, kEnter, sizeof(kEnter),
                                   reinterpret_cast<void*>(OnEnterUse), hook);
    std::fprintf(log, "objectives hold %s: checks %u/%zu verified, slots (IUsable +0x2C %08X, +0x34 %08X, HandleEvents %08X) %s; +%u %% hold per human beyond "
                      "the first, Objective visits only\n",
                 ready ? "installed" : "NOT installed", checksOk, std::size(checks), enterSlot, exitSlot, eventsSlot, slotsOk ? "ok" : "MISMATCH",
                 tuning::holdPercent);
}
void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG sum = uses + written * 3 + kept * 5 + restored * 7 + outside * 11 + notSwitch * 13 + rejected * 17 + full * 19 + faults * 23;
    if (!ready || sum == last) return;
    last = sum;
    std::fprintf(log, "objectives hold uses=%ld written=%ld kept=%ld restored=%ld outside=%ld not_switch=%ld rejected=%ld table_full=%ld faults=%ld switches=%u "
                      "last_humans=%ld last_factor=%.2f\n",
                 InterlockedCompareExchange(&uses, 0, 0), InterlockedCompareExchange(&written, 0, 0), InterlockedCompareExchange(&kept, 0, 0),
                 InterlockedCompareExchange(&restored, 0, 0), InterlockedCompareExchange(&outside, 0, 0), InterlockedCompareExchange(&notSwitch, 0, 0),
                 InterlockedCompareExchange(&rejected, 0, 0), InterlockedCompareExchange(&full, 0, 0), InterlockedCompareExchange(&faults, 0, 0), used,
                 InterlockedCompareExchange(&lastHumans, 0, 0), lastFactor);
}
}  // namespace hold

// The blue icon on the RIG next to the stasis bar while a Player carries an
// encumbering KeyItem (CMD_DisableRun, attrs+0x310). CheckDirtyFlags (0x0087B260)
// calls Player::EnableCarryingKeyVFX (0x0087ABD0, thiscall(bool), ret 4) on each
// change, but the game builds the effect only with MPLevelSettings ([0x02021624]),
// which our MP-map visits block. Without it the local half is done here the way
// the game does it: bone 0x6CB25065 (Player vtable +0x94, thiscall(out*, hash),
// out = index * 4), Create_ByGuid with m_humanCarryingKeyVFX into Player+0x1708,
// VFXHandle::Kill when the item is gone. Only our own Isaac gets it: the
// partner's client lost the game the moment the host picked up a key item
// (13.10.), the one time a ghost's carry bit arrived; until a client log shows
// otherwise, a ghost (and the key holder marker over it) is left out. [MpMap] CarryIcon.
namespace carry {
bool enabled = true, ready = false;
constexpr uint32_t kEnableRva = 0x47ABD0, kSettingsRva = 0x1C21624, kHandle = 0x1708, kBoneSlot = 0x94, kBoneLookupRva = 0x34D7A0, kBoneHash = 0x6CB25065;
constexpr uint32_t kCreateRva = 0x2A04A0, kAssignRva = 0x2D2130, kHandleDtorRva = 0x29F700, kKillRva = 0x29DF60;
// global_mp_assets MPLevelSettings m_humanCarryingKeyVFX (the NecroVision variant is the same)
constexpr uint32_t kVfxGuid[4] = {0xB37C9E87u, 0xB37C9E87u, 0xB37C9E87u, 0x0DF146D4u};
safetyhook::InlineHook hook;
volatile LONG calls = 0, native = 0, shown = 0, hidden = 0, notResident = 0, outsideVisit = 0, notPlayer = 0, boneMissing = 0, faults = 0, ghostsSkipped = 0;
unsigned lines = 0;

struct alignas(16) Matrix { float m[16]; };
// 0 shown, 1 not resident, 2 bone lookup failed (shown on the root like the game would).
__declspec(noinline) int CreateRaw(uint32_t player) {
    uint32_t bone = 0;
    using Lookup = bool(__thiscall*)(void*, uint32_t*, uint32_t);
    const bool found = reinterpret_cast<Lookup>(base + kBoneLookupRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(player)), &bone, kBoneHash);
    Matrix identity{};
    identity.m[0] = identity.m[5] = identity.m[10] = identity.m[15] = 1.0f;
    uint32_t tmp = 0;
    using Create = uint32_t*(__cdecl*)(uint32_t*, const uint32_t*, const Matrix*, uint32_t, uint32_t, uint32_t, uint32_t, const void*);
    reinterpret_cast<Create>(base + kCreateRva)(&tmp, kVfxGuid, &identity, 0xFFFFu, 0u, player, bone >> 2, nullptr);
    if (!tmp) return 1;
    reinterpret_cast<void(__thiscall*)(void*, uint32_t*)>(base + kAssignRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(player + kHandle)), &tmp);
    reinterpret_cast<void(__thiscall*)(uint32_t*)>(base + kHandleDtorRva)(&tmp);
    return found ? 0 : 2;
}
__declspec(noinline) void KillRaw(uint32_t player) {
    reinterpret_cast<void(__thiscall*)(void*)>(base + kKillRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(player + kHandle)));
}

void Apply(uint32_t player, bool on) {
    uint32_t vtable = 0, handle = 0;
    if (!Read(player, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva || !Read(player + kHandle, &handle, 4)) {
        InterlockedIncrement(&notPlayer);
        return;
    }
    if (!on) {
        if (!handle) return;
        KillRaw(player);
        InterlockedIncrement(&hidden);
        if (lines < 24) { ++lines; mpmap::Event("carry icon: Player %08X put the key item down, icon %08X killed", player, handle); }
        return;
    }
    if (handle) return;
    if (IsGhostPlayer(player)) {
        if (InterlockedIncrement(&ghostsSkipped) <= 3) mpmap::Event("carry icon: the partner's ghost %08X carries a key item, no icon on it", player);
        return;
    }
    if (!mpmap::InVisit()) {
        if (InterlockedIncrement(&outsideVisit) <= 3) mpmap::Event("carry icon: Player %08X carries a key item outside an MP-map visit, no icon", player);
        return;
    }
    const int result = CreateRaw(player);
    Read(player + kHandle, &handle, 4);
    if (result == 1 || !handle) {
        if (InterlockedIncrement(&notResident) <= 3) mpmap::Event("carry icon: Player %08X carries a key item but %08X-%08X is not resident", player, kVfxGuid[0], kVfxGuid[3]);
        return;
    }
    InterlockedIncrement(&shown);
    if (result == 2) InterlockedIncrement(&boneMissing);
    if (lines < 24) { ++lines; mpmap::Event("carry icon: Player %08X picked up a key item, icon %08X%s", player, handle, result == 2 ? " (bone not found, on the root)" : ""); }
}

void __fastcall OnEnable(uint32_t player, uint32_t, uint32_t on) {
    InterlockedIncrement(&calls);
    uint32_t settings = 0;
    if (!ready || (Read(base + kSettingsRva, &settings, 4) && settings)) {
        InterlockedIncrement(&native);
        hook.thiscall<void>(reinterpret_cast<void*>(static_cast<uintptr_t>(player)), on);
        return;
    }
    __try {
        Apply(player, (on & 0xFF) != 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("MpMap", "CarryIcon", 1, ini.c_str()) != 0;
    if (!enabled) { std::fprintf(log, "carry icon off ([MpMap] CarryIcon=0)\n"); return; }
    // push ebp / mov ebp,esp / and esp,-10h / sub esp,64h / push ebx / mov ebx,[MPLevelSettings] / push esi / push edi / mov edi,ecx
    unsigned char head[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x64, 0x53, 0x8B, 0x1D, 0, 0, 0, 0, 0x56, 0x57, 0x8B, 0xF9};
    const uint32_t settingsAt = static_cast<uint32_t>(base + kSettingsRva);
    std::memcpy(head + 12, &settingsAt, 4);
    struct Check { const char* name; uint32_t rva; unsigned char bytes[12]; unsigned size; };
    static const Check kChecks[] = {
        {"Alchemy::Create_ByGuid", kCreateRva, {0x83, 0xEC, 0x10, 0x8B, 0x44, 0x24, 0x28, 0x8B, 0x4C, 0x24, 0x30, 0x8B}, 12},
        {"VFXHandle::operator=", kAssignRva, {0x8B, 0xC1, 0x8B, 0x08, 0x85, 0xC9, 0x74, 0x0D, 0xC7, 0x41, 0x08, 0x00}, 12},
        {"VFXHandle::~VFXHandle", kHandleDtorRva, {0x8B, 0x01, 0x85, 0xC0, 0x74, 0x0D, 0xC7, 0x40, 0x08, 0x00, 0x00, 0x00}, 12},
        {"VFXHandle::Kill", kKillRva, {0x8B, 0x09, 0x85, 0xC9, 0x74, 0x05, 0xE9}, 7},
        // push ebx / push ebp / mov ebp,[esp+0Ch] / push esi / mov esi,ecx / mov eax,[esi+194h]
        {"Player bone lookup", kBoneLookupRva, {0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x94}, 12},
    };
    unsigned checksOk = 0;
    for (const Check& c : kChecks) {
        unsigned char have[12]{};
        const bool ok = Read(base + c.rva, have, c.size) && std::memcmp(have, c.bytes, c.size) == 0;
        if (ok) ++checksOk;
        else std::fprintf(log, "carry icon: %s at %08X MISMATCH\n", c.name, static_cast<unsigned>(base + c.rva));
    }
    uint32_t slot = 0;
    const bool slotOk = Read(base + placement::kPlayerPrimaryVtableRva + kBoneSlot, &slot, 4) && slot == base + kBoneLookupRva;
    bool hooked = false;
    if (checksOk == std::size(kChecks) && slotOk)
        hooked = hook_site::Install(log, "carry icon Player::EnableCarryingKeyVFX", base + kEnableRva, head, sizeof(head), reinterpret_cast<void*>(OnEnable), hook);
    ready = hooked;
    std::fprintf(log, "carry icon %s: calls %u/%zu verified, Player vtable +0x94 %08X %s, VFX %08X-%08X-%08X-%08X\n", ready ? "installed" : "NOT installed", checksOk,
                 std::size(kChecks), slot, slotOk ? "ok" : "MISMATCH", kVfxGuid[0], kVfxGuid[1], kVfxGuid[2], kVfxGuid[3]);
}
void WriteDiagnostics(FILE* log) {
    if (!ready || !InterlockedCompareExchange(&calls, 0, 0)) return;
    std::fprintf(log, "carry icon calls=%ld native=%ld shown=%ld hidden=%ld not_resident=%ld outside_visit=%ld not_player=%ld bone_missing=%ld faults=%ld "
                      "ghosts_skipped=%ld\n",
                 InterlockedCompareExchange(&calls, 0, 0), InterlockedCompareExchange(&native, 0, 0), InterlockedCompareExchange(&shown, 0, 0),
                 InterlockedCompareExchange(&hidden, 0, 0), InterlockedCompareExchange(&notResident, 0, 0), InterlockedCompareExchange(&outsideVisit, 0, 0),
                 InterlockedCompareExchange(&notPlayer, 0, 0), InterlockedCompareExchange(&boneMissing, 0, 0), InterlockedCompareExchange(&faults, 0, 0),
                 InterlockedCompareExchange(&ghostsSkipped, 0, 0));
}
}  // namespace carry

// [Objective] lives in DS2Survival.ini (DS2United.ini when merged), next to the
// director's own Objective settings.
void ReadTuning(FILE* log, const std::string& ini) {
    const size_t slash = ini.find_last_of("\\/");
    const std::string file = ds2coop::ConfigFile(slash == std::string::npos ? std::string() : ini.substr(0, slash + 1), "DS2Survival.ini");
    char text[32]{};
    GetPrivateProfileStringA("Objective", "PropHealthFactor", "3", text, sizeof(text), file.c_str());
    const float factor = static_cast<float>(std::atof(text));
    tuning::healthFactor = std::isfinite(factor) && factor >= 1.0f ? (factor > 20.0f ? 20.0f : factor) : 1.0f;
    const int percent = static_cast<int>(GetPrivateProfileIntA("Objective", "HoldPerPlayerPercent", 50, file.c_str()));
    tuning::holdPercent = percent < 0 ? 0u : percent > 400 ? 400u : static_cast<unsigned>(percent);
    const int beam = static_cast<int>(GetPrivateProfileIntA("Objective", "ContactBeamPercent", 50, file.c_str()));
    tuning::beamPercent = beam < 0 ? 0u : beam > 100 ? 100u : static_cast<unsigned>(beam);
    std::fprintf(log, "objectives tuning: ContactBeamPercent=%u (Contact Beam hits on objective props in Objective and Versus visits)\n", tuning::beamPercent);
    std::fprintf(log, "objectives tuning from %s (%s): PropHealthFactor=%.2f (\"%s\"; objective props last %.2fx as long), HoldPerPlayerPercent=%u "
                      "(x1 solo, x%.2f with 2, x%.2f with 3, x%.2f with 4 humans); Objective visits only\n",
                 file.c_str(), ds2coop::FileExists(file) ? "found" : "missing, defaults", tuning::healthFactor, text, tuning::healthFactor, tuning::holdPercent,
                 1.0f + tuning::holdPercent / 100.0f, 1.0f + tuning::holdPercent / 50.0f, 1.0f + tuning::holdPercent * 3 / 100.0f);
}

void Install(FILE* log, const std::string& ini) {
    defend::Install(log);
    carry::Install(log, ini);
    ReadTuning(log, ini);
    hold::Install(log);
    enabled = GetPrivateProfileIntA("MpMap", "ObjectiveDamage", 0, ini.c_str()) != 0;
    countBots = GetPrivateProfileIntA("MpMap", "ObjectiveTeamCountsBots", 0, ini.c_str()) != 0;
    if (!enabled && !(tuning::healthFactor > 1.0f) && tuning::beamPercent == 100) {
        std::fprintf(log, "objectives off ([MpMap] ObjectiveDamage=0, [Objective] PropHealthFactor=1, ContactBeamPercent=100)\n");
        return;
    }
    // the prologue, then the MP-game gate: mov eax,[esi+2C0h] / shr eax,11h / push edi / mov edi,[esi+0Ch] / test al,1 / jz
    static const unsigned char kHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xA4, 0x00, 0x00, 0x00, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0xC0,
                                          0x02, 0x00, 0x00, 0xC1, 0xE8, 0x11, 0x57, 0x8B, 0x7E, 0x0C, 0xA8, 0x01, 0x0F, 0x84, 0xC4, 0x00, 0x00, 0x00};
    unsigned char have[sizeof(kHead)]{};
    const bool headOk = Read(base + kHandleEventsRva, have, sizeof(have)) && std::memcmp(have, kHead, sizeof(have)) == 0;
    unsigned slotsOk = 0;
    for (uint32_t slot : kVtableSlots) {
        uint32_t fn = 0;
        if (Read(base + slot, &fn, 4) && fn == base + kHandleEventsRva) ++slotsOk;
    }
    if (headOk && slotsOk == std::size(kVtableSlots))
        hook = safetyhook::create_inline(reinterpret_cast<void*>(base + kHandleEventsRva), reinterpret_cast<void*>(OnHandleEvents));
    const bool hookIn = static_cast<bool>(hook);
    enabled = enabled && hookIn;
    if (!hookIn) tuning::healthFactor = 1.0f;   // the defend hits follow the hook, so a prop never gets two rules
    std::fprintf(log, "objectives %s: DamageableProp::HandleEvents %08X %s, vtable slots %u/%zu; team scaling %s (damage x %u / team, bots %s); "
                      "objective props / %.2f in Objective visits\n",
                 hookIn ? "installed" : "NOT installed", static_cast<unsigned>(base + kHandleEventsRva), headOk ? "verified" : "MISMATCH", slotsOk,
                 std::size(kVtableSlots), enabled ? "on" : "off", kTargetHumans, countBots ? "count" : "do not count", tuning::healthFactor);
}
void WriteDiagnostics(FILE* log) {
    defend::WriteDiagnostics(log);
    carry::WriteDiagnostics(log);
    hold::WriteDiagnostics(log);
    if (!hook) return;
    if (tuning::beamPercent != 100)
        std::fprintf(log, "objectives contact_beam_hits=%ld at %u%% (Objective and Versus visits)\n", InterlockedCompareExchange(&tuning::beamHits, 0, 0),
                     tuning::beamPercent);
    std::fprintf(log, "objectives damage_msgs=%ld scaled=%ld no_filter=%ld mp_flag=%ld stale_handle=%ld other_attacker=%ld ghost_hits_zeroed=%ld faults=%ld last_factor=%.2f "
                      "health_factor=%.2f health_hits=%ld health_props=%ld health_damage=%.0f->%.0f defend_divided=%ld amounts_restored=%ld restore_gone=%ld "
                      "restore_changed=%ld\n",
                 InterlockedCompareExchange(&damageMsgs, 0, 0), InterlockedCompareExchange(&scaled, 0, 0), InterlockedCompareExchange(&noFilter, 0, 0),
                 InterlockedCompareExchange(&mpFlag, 0, 0), InterlockedCompareExchange(&staleHandle, 0, 0), InterlockedCompareExchange(&otherAttacker, 0, 0),
                 InterlockedCompareExchange(&ghostHitsZeroed, 0, 0), InterlockedCompareExchange(&faults, 0, 0), lastFactor, tuning::healthFactor,
                 InterlockedCompareExchange(&tuning::propHits, 0, 0), InterlockedCompareExchange(&tuning::propsSeen, 0, 0), tuning::damageIn, tuning::damageOut,
                 InterlockedCompareExchange(&tuning::defendHits, 0, 0), InterlockedCompareExchange(&amountsRestored, 0, 0),
                 InterlockedCompareExchange(&restoreGone, 0, 0), InterlockedCompareExchange(&restoreChanged, 0, 0));
}
}  // namespace objectives
