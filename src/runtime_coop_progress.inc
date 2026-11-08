// Dead Space 2 United by Hafer.
// Profile XP, weapon levels and the MP upgrades they unlock ([Progress];
// coop_progress.h, analysis/MP_UPGRADES_EFFECTS_2026-10-15.md).
//
// XP. The host's survival director pays each wave, the unfinished wave at a run
// end and the clock modes' results (DS2Coop_SurvivalProgress); the host books it
// and sends it as SVNT kind 9, every client books its own. A run that used a host
// tool pays nothing; a machine that used a cheat or got a host tool pays nothing
// until the next run. Booking happens on the game thread (profile file, notice).
//
// Weapon levels. Each machine counts its own kills: a hit from the local Isaac
// that takes an enemy's health to zero, credited to the gun named by the hit's
// inflictor bit (DamageData+0x94). Kept in DS2Profile.ini [WeaponKills].
//
// Effects, in Survival, Mercenaries and Versus (never in the story):
//   - clip: Gun::CalcUpgradedValues 0x00DBAFF0 (thiscall(Gun*), plain ret), after
//     the original, for the local Isaac's guns. The original refills the clip when
//     the capacity changed during the call (0x00DBB167); the same rule is kept.
//   - damage: in the health thunk (runtime_coop_hits.inc) for the local Isaac's
//     hits, scaled for the call and put back, because an explosion hands the same
//     DamageData to the next body.
//   - the non-gun MP rows (stasis, necro): DGM::GetMultiplayerUpgrade 0x00A6C140
//     (cdecl(Entity*, row), float in st0) in a Versus visit, by the body's profile
//     level. Without it the local body gets level 0, so stasis never fired in Versus
//     (row 29 must be != 1.0 with the MP bit, 0x00873450).
namespace progress {
namespace cp = coop_progress;
bool enabled = true, effects = true, versusRows = true;
unsigned startCreditsPerLevel = 50;
cp::Rules rules;

// --- the books ---------------------------------------------------------------------------
volatile LONG kills[cp::kGunCount]{};
LONG savedKills[cp::kGunCount]{};
volatile LONG killsDirty = 0;
DWORD killsSavedAt = 0;
bool TestAll() { return loadout::unlockAll; }
unsigned WeaponLevelOf(int gun) {
    if (gun < 0 || gun >= cp::kGunCount) return 1;
    return TestAll() ? cp::kMaxWeaponLevel : cp::WeaponLevel(rules, static_cast<uint32_t>(InterlockedCompareExchange(&kills[gun], 0, 0)));
}
unsigned ProfileLevel() { return TestAll() ? 60 : coop_mpnative::LevelForXp(mpnative::ProfileXp()); }

// [Unlocks]: the profile level of each gun, suit and gadget, read when asked so an edited
// INI counts at the next menu or shop visit. fallback < 0: the coop_progress.h default.
std::string unlocksIni;
unsigned UnlockLevel(const char* key, int fallback) {
    const int def = fallback >= 0 ? fallback : static_cast<int>(cp::DefaultUnlock(key));
    if (!key || unlocksIni.empty()) return static_cast<unsigned>(def);
    const int v = static_cast<int>(GetPrivateProfileIntA("Unlocks", key, def, unlocksIni.c_str()));
    return v < 0 ? 0u : v > static_cast<int>(cp::kMaxUnlockLevel) ? cp::kMaxUnlockLevel : static_cast<unsigned>(v);
}
bool Unlocked(const char* key, int fallback) { return TestAll() || ProfileLevel() >= UnlockLevel(key, fallback); }
// A campaign suit by its Uniform GUID: its code name in the suit select, and that row's level.
unsigned SuitUnlockLevel(const uint32_t* uniform) {
    for (const auto& s : loadout_data::kSuits)
        if (uniform && !std::memcmp(s.uniform, uniform, 16)) return UnlockLevel(s.code, s.level);
    return 0;
}
// The gadget keys: a locked gadget says so (at most every 3 s) and stays put. Only where the
// progression counts; the story keeps every gadget.
volatile LONG gadgetRefusals = 0;
bool GadgetAllowed(const char* key, const char* label) {
    if (!enabled || !inLevel || play::StoryCoop() || Unlocked(key)) return true;
    InterlockedIncrement(&gadgetRefusals);
    static DWORD saidAt = 0;
    const DWORD now = GetTickCount();
    if (now - saidAt >= 3000) {
        saidAt = now;
        char text[80];
        std::snprintf(text, sizeof(text), "%s UNLOCKS AT LEVEL %u", label, UnlockLevel(key));
        notice::Enqueue(text);
    }
    return false;
}

void LoadKills() {
    const char* f = mpnative::profilePath.c_str();
    for (int i = 0; i < cp::kGunCount; ++i) {
        const LONG n = static_cast<LONG>(GetPrivateProfileIntA("WeaponKills", cp::kGuns[i].key, 0, f));
        InterlockedExchange(&kills[i], n < 0 ? 0 : n);
        savedKills[i] = kills[i];
    }
}
void SaveKills() {
    const char* f = mpnative::profilePath.c_str();
    for (int i = 0; i < cp::kGunCount; ++i) {
        const LONG n = InterlockedCompareExchange(&kills[i], 0, 0);
        if (n == savedKills[i]) continue;
        char text[16];
        std::snprintf(text, sizeof(text), "%ld", n);
        WritePrivateProfileStringA("WeaponKills", cp::kGuns[i].key, text, f);
        savedKills[i] = n;
    }
}

// --- XP booking (any thread in, game thread out) -------------------------------------------------
struct Booking { int32_t xp; char why[40]; };
SRWLOCK bookLock = SRWLOCK_INIT;
Booking book[16]{};
unsigned bookHead = 0, bookTail = 0;
volatile LONG booked = 0, bookedXp = 0, bookDropped = 0, levelUps = 0;
void AddXp(int32_t xp, const char* why) {
    if (!enabled || xp <= 0) return;
    AcquireSRWLockExclusive(&bookLock);
    if (bookHead - bookTail < 16) {
        Booking& b = book[bookHead++ % 16];
        b.xp = xp;
        std::snprintf(b.why, sizeof(b.why), "%s", why ? why : "");
    } else {
        InterlockedIncrement(&bookDropped);
    }
    ReleaseSRWLockExclusive(&bookLock);
}
void BookPending() {
    for (;;) {
        Booking b{};
        AcquireSRWLockExclusive(&bookLock);
        const bool any = bookTail != bookHead;
        if (any) b = book[bookTail++ % 16];
        ReleaseSRWLockExclusive(&bookLock);
        if (!any) return;
        const unsigned before = coop_mpnative::LevelForXp(mpnative::profile.xp);
        const uint64_t sum = static_cast<uint64_t>(mpnative::profile.xp) + static_cast<uint32_t>(b.xp);
        mpnative::profile.xp = sum > 0xFFFFFFF0ull ? 0xFFFFFFF0u : static_cast<uint32_t>(sum);
        mpnative::SaveProfile();
        const unsigned after = coop_mpnative::LevelForXp(mpnative::profile.xp);
        InterlockedIncrement(&booked);
        InterlockedExchangeAdd(&bookedXp, b.xp);
        char text[96];
        if (after > before) {
            InterlockedIncrement(&levelUps);
            std::snprintf(text, sizeof(text), "+%d XP - LEVEL UP! LEVEL %u", b.xp, after);
        } else if (after < 60) {
            std::snprintf(text, sizeof(text), "+%d XP (%s) - LEVEL %u, %u XP TO LEVEL %u", b.xp, b.why, after,
                          coop_mpnative::kLevelScore[after] - mpnative::profile.xp, after + 1);
        } else {
            std::snprintf(text, sizeof(text), "+%d XP (%s) - LEVEL %u", b.xp, b.why, after);
        }
        notice::Enqueue(text);
        gate::Event("progress: +%d XP for %s, now %u (level %u -> %u)", b.xp, b.why, mpnative::profile.xp, before, after);
    }
}

// --- taint ---------------------------------------------------------------------------------
// Set by a cheat or a host tool on this machine; it lasts until a payment from
// another run arrives.
volatile LONG tainted = 0, taintRun = 0, refusedTainted = 0, refusedHost = 0;
char taintWhy[48] = "";
void Taint(const char* why) {
    if (!InterlockedExchange(&tainted, 1)) {
        std::snprintf(taintWhy, sizeof(taintWhy), "%s", why ? why : "?");
        gate::Event("progress: no XP until the next run (%s)", taintWhy);
    }
}
// true when this payment may be booked
bool TaintAllows(uint32_t run) {
    if (!InterlockedCompareExchange(&tainted, 0, 0)) return true;
    const LONG seen = InterlockedCompareExchange(&taintRun, 0, 0);
    if (!seen) { InterlockedExchange(&taintRun, static_cast<LONG>(run)); return false; }
    if (static_cast<uint32_t>(seen) == run) return false;
    InterlockedExchange(&tainted, 0);
    InterlockedExchange(&taintRun, 0);
    gate::Event("progress: run %u is a new run, XP counts again", run);
    return true;
}

// --- survival payments ------------------------------------------------------------------------
cp::Paid paid;
SRWLOCK paidLock = SRWLOCK_INIT;
volatile LONG payments = 0, paymentsSent = 0, paymentsReceived = 0, paymentDuplicates = 0;
void Pay(uint32_t run, uint32_t wave, uint32_t xp, uint32_t flags, const char* source) {
    const unsigned event = cp::EventOf(flags);
    AcquireSRWLockExclusive(&paidLock);
    const bool fresh = paid.Take(run, wave, event);
    ReleaseSRWLockExclusive(&paidLock);
    if (!fresh) { InterlockedIncrement(&paymentDuplicates); return; }
    InterlockedIncrement(&payments);
    if (flags & cp::kTaintHost) {
        InterlockedIncrement(&refusedHost);
        gate::Event("progress: %s %s run %u wave %u: %u XP refused, the host's run used a host tool", source, cp::EventName(event), run, wave, xp);
        return;
    }
    if (!TaintAllows(run)) {
        InterlockedIncrement(&refusedTainted);
        gate::Event("progress: %s %s run %u wave %u: %u XP refused here (%s)", source, cp::EventName(event), run, wave, xp, taintWhy);
        return;
    }
    char why[40];
    if (event == cp::kWaveEnd || event == cp::kRunEnd) std::snprintf(why, sizeof(why), "%s %u", cp::EventName(event), wave);
    else std::snprintf(why, sizeof(why), "%s", cp::EventName(event));
    AddXp(static_cast<int32_t>(xp), why);
}
// The host's director (DS2Coop_SurvivalProgress), any thread.
void FromDirector(uint32_t run, uint32_t wave, uint32_t xp, uint32_t flags) {
    if (!enabled) return;
    // a cheat or host tool on the host taints the run for everybody
    if (!(flags & cp::kTaintHost) && !TaintAllows(run)) flags |= cp::kTaintHost;
    Pay(run, wave, xp, flags, "director");
    survnet::QueueProgress(run, wave, xp, flags);
    InterlockedIncrement(&paymentsSent);
}
// A client: SVNT kind 9.
void FromHost(uint32_t run, uint32_t wave, uint32_t xp, uint32_t flags) {
    InterlockedIncrement(&paymentsReceived);
    if (enabled) Pay(run, wave, xp, flags, "host");
}

// --- weapon kills (game thread, the health thunk) ----------------------------------------------------
volatile LONG localHits = 0, scaledHits = 0, killCredits = 0, killsNoGun = 0, weaponLevelUps = 0, recalcAsked = 0, recalcDone = 0;
uint32_t LocalIsaac() { return LocalSlotZeroEntity(); }
bool EffectsHere() { return enabled && effects && inLevel && !play::StoryCoop(); }
// The local Isaac behind a hit and the gun it names, or -1.
int LocalGun(const void* damage, bool* local) {
    *local = false;
    const uint32_t isaac = LocalIsaac();
    if (!damage || !isaac || bot::HumanBehind(damage) != isaac) return -1;
    *local = true;
    uint32_t inflictor = 0;
    if (!Read(reinterpret_cast<uintptr_t>(damage) + 0x94, &inflictor, 4)) return -1;
    return cp::GunByInflictor(inflictor);
}
bool VictimCounts(uint32_t component, uint32_t* entity) {
    uint32_t vtable = 0;
    if (!Read(component + hits::kComponentOwner, entity, 4) || !*entity || !Read(*entity, &vtable, 4)) return false;
    if (vtable == base + placement::kPlayerPrimaryVtableRva) return false;   // no friendly fire bonus, no kill credit
    if (phantom::Is(*entity)) return false;   // a hallucination is worth nothing
    return bot_sense::KindOf(vtable) != coop_bot::EnemyKind::Unknown || vtable == base + kNecroPrimaryVtableRva;
}
float HitFactor(const void* damage, uint32_t component) {
    if (!EffectsHere()) return 1.0f;
    bool local = false;
    const int gun = LocalGun(damage, &local);
    if (gun < 0) return 1.0f;
    uint32_t entity = 0;
    if (!VictimCounts(component, &entity)) return 1.0f;
    InterlockedIncrement(&localHits);
    const float f = cp::DamageFactor(rules, WeaponLevelOf(gun));
    if (f > 1.0f) InterlockedIncrement(&scaledHits);
    return f;
}
void OnHit(const void* damage, uint32_t component, float before, float after) {
    if (!enabled || !(before > 0.0f) || after > 0.0f || !inLevel) return;
    bool local = false;
    const int gun = LocalGun(damage, &local);
    if (!local) return;
    uint32_t entity = 0;
    if (!VictimCounts(component, &entity)) return;
    if (gun < 0) { InterlockedIncrement(&killsNoGun); return; }
    const unsigned levelBefore = cp::WeaponLevel(rules, static_cast<uint32_t>(InterlockedCompareExchange(&kills[gun], 0, 0)));
    const LONG n = InterlockedIncrement(&kills[gun]);
    InterlockedIncrement(&killCredits);
    InterlockedExchange(&killsDirty, 1);
    const unsigned levelAfter = cp::WeaponLevel(rules, static_cast<uint32_t>(n));
    if (levelAfter <= levelBefore) return;
    InterlockedIncrement(&weaponLevelUps);
    char text[96];
    const unsigned clip = cp::ClipTier(rules, levelAfter), dmg = cp::DamageTier(rules, levelAfter);
    if (clip > cp::ClipTier(rules, levelBefore)) InterlockedExchange(&recalcAsked, 1);   // the gun in hand, from Tick
    if (clip > cp::ClipTier(rules, levelBefore))
        std::snprintf(text, sizeof(text), "%s LEVEL %u - MAGAZINE X%.2f", cp::kGuns[gun].name, levelAfter, rules.clipFactor[clip - 1]);
    else if (dmg > cp::DamageTier(rules, levelBefore))
        std::snprintf(text, sizeof(text), "%s LEVEL %u - DAMAGE X%.2f", cp::kGuns[gun].name, levelAfter, rules.damageFactor[dmg - 1]);
    else
        std::snprintf(text, sizeof(text), "%s LEVEL %u", cp::kGuns[gun].name, levelAfter);
    notice::Enqueue(text);
    gate::Event("progress: %s level %u -> %u at %ld kills", cp::kGuns[gun].name, levelBefore, levelAfter, n);
}

// --- clip: Gun::CalcUpgradedValues -----------------------------------------------------------------
constexpr uint32_t kCalcUpgradedRva = 0x9BAFF0, kGunCapacity = 0x3E4, kGunLoaded = 0x39C, kGunOwner = 0x240;
const unsigned char kCalcUpgradedBytes[] = {0x83, 0xEC, 0x0C, 0xA1, 0x38, 0x14, 0x02, 0x02, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x0C};
safetyhook::InlineHook calcHook;
volatile LONG clipCalls = 0, clipBoosted = 0, clipRefills = 0, clipNotOurs = 0, clipUnknownGun = 0, clipLines = 0;
// Gun vtables (analysis/ds2_classmap_live_2026-09-17.json, coop_shop.h) by kGuns index.
int GunByVtableRva(uint32_t rva) {
    static constexpr struct { uint32_t rva; int gun; } kVtables[] = {
        {0x17515D8, 0}, {0x1751A00, 1}, {0x17511B8, 2}, {0x1753520, 3}, {0x1752CE0, 4}, {0x1752580, 5},
        {0x1753948, 6}, {0x1750D98, 7}, {0x1753100, 8}, {0x174C9A0, 8}, {0x1778A78, 9}, {0x1773780, 10},
    };
    for (const auto& v : kVtables) if (v.rva == rva) return v.gun;
    return -1;
}
void __fastcall OnCalcUpgraded(void* self, void*) {
    const uint32_t gun = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    int32_t cap0 = 0, loaded0 = 0;
    const bool readBefore = Read(gun + kGunCapacity, &cap0, 4) && Read(gun + kGunLoaded, &loaded0, 4);
    calcHook.thiscall<void>(self);
    InterlockedIncrement(&clipCalls);
    if (!readBefore || !EffectsHere()) return;
    uint32_t ownerRef = 0, vtable = 0;
    const uint32_t isaac = LocalIsaac();
    if (!isaac || !Read(gun + kGunOwner, &ownerRef, 4) || ownerRef - 0x10 != isaac) { InterlockedIncrement(&clipNotOurs); return; }
    const int kind = Read(gun, &vtable, 4) ? GunByVtableRva(vtable - static_cast<uint32_t>(base)) : -1;
    if (kind < 0) { InterlockedIncrement(&clipUnknownGun); return; }
    int32_t cap1 = 0;
    if (!Read(gun + kGunCapacity, &cap1, 4) || cap1 <= 0 || cap1 > 100000) return;
    const unsigned level = WeaponLevelOf(kind);
    const int32_t boosted = static_cast<int32_t>(cp::BoostedClip(static_cast<uint32_t>(cap1), cp::ClipFactor(rules, level)));
    if (boosted != cap1 && WriteBytes(gun + kGunCapacity, &boosted, 4)) InterlockedIncrement(&clipBoosted);
    // the original's rule: a changed capacity refills; an unchanged one keeps the rounds
    const int32_t loaded = boosted != cap0 ? boosted : loaded0;
    int32_t loadedNow = 0;
    if (Read(gun + kGunLoaded, &loadedNow, 4) && loadedNow != loaded && WriteBytes(gun + kGunLoaded, &loaded, 4) && boosted != cap0)
        InterlockedIncrement(&clipRefills);
    if (InterlockedIncrement(&clipLines) <= 40)
        gate::Event("progress clip: %s gun %08X level %u: capacity %d -> %d -> %d, loaded %d -> %d", cp::kGuns[kind].name, gun, level, cap0, cap1,
                    boosted, loaded0, loaded);
}

// --- Versus: the non-gun MP rows ------------------------------------------------------------------
constexpr uint32_t kMpUpgradeRva = 0x66C140, kPlayerRef = 0x1710, kNecroRef = 0x538;
const unsigned char kMpUpgradeBytes[] = {0x83, 0xEC, 0x0C, 0xA1, 0xE4, 0xC0, 0x01, 0x02, 0x8B, 0x48, 0x38, 0xF3, 0x0F, 0x10, 0x05, 0xD0, 0x5E, 0xAA, 0x01};
safetyhook::InlineHook mpUpgradeHook;
volatile LONG rowAsks = 0, rowAnswered = 0, rowNotBody = 0, rowGun = 0, rowUnknownRef = 0, rowBelowLevel = 0;
volatile LONG rowCounts[cp::kRows]{};
float __cdecl OnMpUpgrade(uint32_t entity, int row) {
    if (!versusRows || !InterlockedCompareExchange(&mpnative::active, 0, 0)) return mpUpgradeHook.ccall<float>(entity, row);
    InterlockedIncrement(&rowAsks);
    if (row >= 0 && row < cp::kRows) InterlockedIncrement(&rowCounts[row]);
    if (row < 14) { InterlockedIncrement(&rowGun); return 1.0f; }   // guns: the weapon levels, applied by our own hooks
    uint32_t vtable = 0;
    if (!entity || !Read(entity, &vtable, 4)) { InterlockedIncrement(&rowNotBody); return 1.0f; }
    const bool human = vtable == base + placement::kPlayerPrimaryVtableRva, necroBody = vtable == base + kNecroPrimaryVtableRva;
    if (!human && !necroBody) { InterlockedIncrement(&rowNotBody); return 1.0f; }   // AI necros carry the same type flags
    unsigned level = 0;
    uint64_t ref = 0;
    Read(entity + (human ? kPlayerRef : kNecroRef), &ref, 8);
    if (entity == LocalIsaac() || entity == necro::necroEntity || (ref && ref == coop_mpnative::RefFor(machineIndex))) {
        level = ProfileLevel();
    } else {
        coop_mpnative::Entry e{};
        if (ref && mpnative::Lookup(ref, e)) level = coop_mpnative::LevelForXp(e.xp);
        else if (IsBotEntity(entity)) level = 1;
        else { InterlockedIncrement(&rowUnknownRef); level = 1; }
    }
    const float v = cp::ProfileUpgrade(row, level);
    InterlockedIncrement(v != 1.0f ? &rowAnswered : &rowBelowLevel);
    return v;
}

// --- the start of a visit -----------------------------------------------------------------------
volatile LONG startBonuses = 0, startCredits = 0;
void StartBonus(uint32_t player, const char* where) {
    if (!enabled || !startCreditsPerLevel || !player || play::VersusMode() || gamemode::mode == gamemode::Mercenaries) return;
    const unsigned level = ProfileLevel();
    const uint32_t credits = level * startCreditsPerLevel;
    if (!credits) return;
    shop::Refund(player, credits, "level start bonus");
    InterlockedIncrement(&startBonuses);
    InterlockedExchangeAdd(&startCredits, static_cast<LONG>(credits));
    char text[80];
    std::snprintf(text, sizeof(text), "LEVEL %u: +%u CREDITS", level, credits);
    notice::Enqueue(text);
    gate::Event("progress: start bonus at %s, level %u, %u credits", where, level, credits);
}

// A new magazine tier: Gun::CalcUpgradedValues (vtable +0x284) on the gun in hand, so the
// capacity grows now and not at the next pickup; our hook refills it, as the original
// does when the capacity changes.
constexpr uint32_t kCalcSlot = 0x284;
__declspec(noinline) bool CallCalc(uint32_t gun, uint32_t fn) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(gun)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void RecalcHeldGun() {
    const uint32_t isaac = LocalIsaac();
    uint32_t ref = 0, vtable = 0, fn = 0;
    if (!isaac || !Read(isaac + bot::kActiveItem, &ref, 4) || ref < 0x10010) return;
    const uint32_t gun = ref - 0x10;
    if (!Read(gun, &vtable, 4) || GunByVtableRva(vtable - static_cast<uint32_t>(base)) < 0 || !Read(vtable + kCalcSlot, &fn, 4) || !fn) return;
    if (CallCalc(gun, fn)) InterlockedIncrement(&recalcDone);
}

// --- tick, install, log -----------------------------------------------------------------------------
void Tick(DWORD now) {
    if (!enabled) return;
    BookPending();
    if (InterlockedExchange(&recalcAsked, 0) && EffectsHere()) RecalcHeldGun();
    if (InterlockedCompareExchange(&killsDirty, 0, 0) && now - killsSavedAt >= 5000) {
        InterlockedExchange(&killsDirty, 0);
        killsSavedAt = now;
        SaveKills();
    }
}
void ReadRules(const char* f, FILE* log) {
    char text[160];
    uint32_t list[cp::kMaxWeaponLevel - 1]{};
    GetPrivateProfileStringA("Progress", "WeaponLevelKills", "", text, sizeof(text), f);
    if (text[0] && cp::ParseRising(text, list, cp::kMaxWeaponLevel - 1)) std::memcpy(rules.levelKills, list, sizeof(list));
    else if (text[0]) std::fprintf(log, "progress: WeaponLevelKills='%s' is not 9 rising numbers; defaults kept\n", text);
    uint32_t two[2]{};
    GetPrivateProfileStringA("Progress", "ClipLevels", "", text, sizeof(text), f);
    if (text[0] && cp::ParseRising(text, two, 2) && two[1] <= cp::kMaxWeaponLevel) { rules.clipLevel[0] = two[0]; rules.clipLevel[1] = two[1]; }
    GetPrivateProfileStringA("Progress", "DamageLevels", "", text, sizeof(text), f);
    if (text[0] && cp::ParseRising(text, two, 2) && two[1] <= cp::kMaxWeaponLevel) { rules.damageLevel[0] = two[0]; rules.damageLevel[1] = two[1]; }
    GetPrivateProfileStringA("Progress", "ClipBonus", "", text, sizeof(text), f);
    if (text[0] && cp::ParseRising(text, two, 2) && two[1] <= 300) { rules.clipFactor[0] = 1.0f + two[0] / 100.0f; rules.clipFactor[1] = 1.0f + two[1] / 100.0f; }
    GetPrivateProfileStringA("Progress", "DamageBonus", "", text, sizeof(text), f);
    if (text[0] && cp::ParseRising(text, two, 2) && two[1] <= 300) { rules.damageFactor[0] = 1.0f + two[0] / 100.0f; rules.damageFactor[1] = 1.0f + two[1] / 100.0f; }
}
void Install(FILE* log, const std::string& ini) {
    const char* f = ini.c_str();
    unlocksIni = ini;
    enabled = GetPrivateProfileIntA("Progress", "Enabled", 1, f) != 0;
    effects = GetPrivateProfileIntA("Progress", "Upgrades", 1, f) != 0;
    versusRows = GetPrivateProfileIntA("Progress", "VersusRows", 1, f) != 0;
    startCreditsPerLevel = static_cast<unsigned>(GetPrivateProfileIntA("Progress", "StartCreditsPerLevel", 50, f));
    if (startCreditsPerLevel > 1000) startCreditsPerLevel = 1000;
    ReadRules(f, log);
    if (!enabled) { std::fprintf(log, "progress off ([Progress] Enabled=0): no XP, no weapon levels, no upgrades\n"); return; }
    LoadKills();
    const bool clip = effects && hook_site::Install(log, "progress Gun::CalcUpgradedValues", base + kCalcUpgradedRva, kCalcUpgradedBytes,
                                                    sizeof(kCalcUpgradedBytes), reinterpret_cast<void*>(OnCalcUpgraded), calcHook);
    const bool rows = versusRows && hook_site::Install(log, "progress DGM::GetMultiplayerUpgrade", base + kMpUpgradeRva, kMpUpgradeBytes,
                                                       sizeof(kMpUpgradeBytes), reinterpret_cast<void*>(OnMpUpgrade), mpUpgradeHook);
    std::fprintf(log, "progress on: profile %s xp=%u level=%u%s; upgrades=%d (clip hook %s, damage in the health thunk) versus_rows=%s start_credits=%u/level\n",
                 mpnative::profilePath.c_str(), mpnative::ProfileXp(), coop_mpnative::LevelForXp(mpnative::ProfileXp()),
                 TestAll() ? " (Unlocks=1: everything counts as level 60 and weapon level 10)" : "", effects ? 1 : 0, clip ? "in" : "MISSING",
                 rows ? "in" : versusRows ? "MISSING" : "off", startCreditsPerLevel);
    std::fprintf(log, "progress weapon levels at kills %u,%u,%u,%u,%u,%u,%u,%u,%u; clip +%.0f%%/+%.0f%% at %u/%u; damage +%.0f%%/+%.0f%% at %u/%u\n",
                 rules.levelKills[0], rules.levelKills[1], rules.levelKills[2], rules.levelKills[3], rules.levelKills[4], rules.levelKills[5],
                 rules.levelKills[6], rules.levelKills[7], rules.levelKills[8], (rules.clipFactor[0] - 1) * 100, (rules.clipFactor[1] - 1) * 100,
                 rules.clipLevel[0], rules.clipLevel[1], (rules.damageFactor[0] - 1) * 100, (rules.damageFactor[1] - 1) * 100, rules.damageLevel[0],
                 rules.damageLevel[1]);
    char line[400];
    int used = std::snprintf(line, sizeof(line), "progress weapon kills:");
    for (int i = 0; i < cp::kGunCount && used > 0 && used < static_cast<int>(sizeof(line)); ++i)
        used += std::snprintf(line + used, sizeof(line) - static_cast<size_t>(used), " %s=%ld(L%u)", cp::kGuns[i].key, kills[i],
                              cp::WeaponLevel(rules, static_cast<uint32_t>(kills[i])));
    std::fprintf(log, "%s\n", line);
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    static LONG last = -1;
    const LONG sum = booked + payments * 3 + paymentDuplicates * 5 + killCredits * 7 + clipBoosted * 11 + rowAsks + startBonuses * 13 + refusedTainted * 17 +
                     refusedHost * 19 + bookDropped * 23 + clipUnknownGun * 29 + killsNoGun * 31;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "progress xp=%u level=%u booked=%ld (+%ld XP, %ld level ups, %ld dropped) payments=%ld sent=%ld received=%ld duplicates=%ld "
                      "refused_host=%ld refused_tainted=%ld tainted=%ld start_bonuses=%ld (%ld credits) gadget_refusals=%ld\n",
                 mpnative::ProfileXp(), coop_mpnative::LevelForXp(mpnative::ProfileXp()), booked, bookedXp, levelUps, bookDropped, payments,
                 paymentsSent, paymentsReceived, paymentDuplicates, refusedHost, refusedTainted, tainted, startBonuses, startCredits, gadgetRefusals);
    std::fprintf(log, "progress weapons local_hits=%ld scaled=%ld kill_credits=%ld kills_no_gun=%ld level_ups=%ld recalcs=%ld | clip calls=%ld boosted=%ld refills=%ld "
                      "not_ours=%ld unknown_gun=%ld | versus rows asks=%ld answered=%ld below_level=%ld gun_rows=%ld not_body=%ld unknown_ref=%ld "
                      "stasis_row29=%ld energy_rows=%ld/%ld\n",
                 localHits, scaledHits, killCredits, killsNoGun, weaponLevelUps, recalcDone, clipCalls, clipBoosted, clipRefills, clipNotOurs, clipUnknownGun,
                 rowAsks, rowAnswered, rowBelowLevel, rowGun, rowNotBody, rowUnknownRef, rowCounts[29], rowCounts[30], rowCounts[31]);
}
}  // namespace progress
