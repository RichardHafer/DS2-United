// Dead Space 2 United by Richard Hafer.
// The game's own character setup offline ([Loadout]): FE16 with its weapon tab
// FE11 and its suit tab FE12, as in the original multiplayer. The Multiplayer
// screen's item 4 opens it (the native item 2 did, PushScreen 0xB00F4B85), and in a
// Versus or Mercenaries lobby the lobby's own tabs 3 and 4 show the same two screens.
// Write-up: analysis/DS2_MP_LOADOUT_SCREENS_2026-10-12.md.
//
// Offline the screens need three things:
//   - the settings seam: leaving FE16 after a pick saves "<weapon>,<skin>" through
//     BlazeSettingsManager::SaveUserSetting (slot 8 at 0x01B927D4, 0x01382540,
//     thiscall(key, value) ret 8, void), which asks the null hub first. With no hub
//     the slot stores the pair in DS2Profile.ini instead; SavePlayerSetup
//     (0x00A537C0) clears the dirty bit DGM+0x1C 0x200 itself afterwards;
//   - an XP answer for the local player (id 0 with no hub) from the stats seam in
//     runtime_coop_mpnative.inc, so level and locks follow [Loadout] Unlocks;
//   - the local unlock data, fed on the game thread each time the frontend comes up:
//     UnlockedContent::ForceUnlocked (0x01354180, ret 4) for the Rivet Gun,
//     DGM::ReadSpecialSuitsUnlocked (0x00A53B40, thiscall(dgm, csv) ret 4) for the
//     20 special suits, DGM::ReadPlayerSetup (0x00A6C420, ret 4) for the stored pick.
// Never call DGM::AddSpecialSuitUnlock: it marks the list dirty for a Blaze save.
#include "generated_loadout_rows.h"
namespace loadout {
bool enabled = false, menuItem = true, lobbyTabs = true, unlockAll = false, specialSuits = true, rivetGun = true;
bool settingsIn = false, sitesOk = false;
constexpr uint32_t kSettingsSlot = 0x17927D4, kSaveUserSetting = 0xF82540;
constexpr uint32_t kSavePlayerSetup = 0x6537C0, kReadPlayerSetup = 0x66C420, kReadSpecialSuits = 0x653B40;
constexpr uint32_t kUnlockedContent = 0x1C213B0, kForceUnlocked = 0xF54180, kIsUnlocked = 0xF54020, kUnlockCount = 0x91C;
constexpr uint32_t kSkinDb = 0x1C21428, kSkinCount = 0x18;
// PlayerSkinDB entries (0xD0): +0 code name (String), +0x10 its hash, +0x1C unlock level
constexpr uint32_t kSkinArray = 0x14, kSkinEntrySize = 0xD0, kSkinNameHash = 0x10, kSkinUnlockLevel = 0x1C;
std::string iniPath;   // [Unlocks] is read at every frontend feed
constexpr uint32_t kSettingsKeyPtr = 0x1B601BC;   // char* DSGM_PLAYER_SETTINGS_KEY
constexpr uint32_t kFe16Hash = 0xB00F4B85, kCharacterSetupLabel = 0x172D648;   // "$UIMenu_Character_Setup_mc"
constexpr uint32_t kDgmFlags = 0x1C, kDgmSkin = 0xF0, kDgmWeapon = 0xF1, kDgmSpecialCount = 0xA0, kSetupDirty = 0x200;
constexpr uint32_t kRivetHash = 0x9954AE8F, kMaxXp = 2224750;   // level 60, the last mp_levels row
const char kSpecialSuits[] =
    "ce,maroon,urban_maroon,tiger_maroon,solid_viral_blue,urban_viral_blue,tiger_viral_blue,solid_viral_yellow,urban_viral_yellow,"
    "tiger_viral_yellow,solid_viral_white,urban_viral_white,tiger_viral_white,matchett,gascoigne,probst,woldman,papoutsis,murphy,lu";

struct Site { const char* name; uint32_t rva; unsigned char bytes[16]; size_t size; };
const Site kSites[] = {
    {"BlazeSettingsManager::SaveUserSetting", kSaveUserSetting, {0x83, 0xEC, 0x48, 0xA1, 0xDC, 0x13, 0x02, 0x02, 0x56, 0x57, 0x8B, 0xF1, 0x8B, 0x88, 0x94, 0x00}, 16},
    {"DGM::SavePlayerSetup", kSavePlayerSetup, {0x81, 0xEC, 0xB8, 0x0B, 0x00, 0x00, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x1C, 0xC1, 0xE8, 0x09, 0xA8}, 16},
    {"DGM::ReadPlayerSetup", kReadPlayerSetup, {0x83, 0xEC, 0x08, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x18, 0x57, 0x8D, 0x91, 0xB8}, 16},
    {"DGM::ReadSpecialSuitsUnlocked", kReadSpecialSuits, {0x81, 0xEC, 0xC0, 0x0B, 0x00, 0x00, 0x55, 0x8B, 0xE9, 0x56, 0xC7, 0x85, 0xA0, 0x00, 0x00, 0x00}, 16},
    {"UnlockedContent::ForceUnlocked", kForceUnlocked, {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x57, 0x8B, 0xF1, 0xE8, 0x02, 0x2D, 0x04, 0x00, 0x83, 0xF8}, 16},
};

volatile LONG lines = 0;
void Line(const char* format, ...) {
    if (InterlockedIncrement(&lines) > 200) return;
    char buffer[300];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    vlog::WriteText("loadout", buffer);
}
uint32_t Ptr(uint32_t address) {
    uint32_t v = 0;
    Read(address, &v, 4);
    return v;
}
// A game string, copied with every byte read checked.
bool CopyString(uint32_t at, char* out, size_t size) {
    if (!at || !size) return false;
    for (size_t i = 0; i + 1 < size; ++i) {
        char c = 0;
        if (!Read(at + static_cast<uint32_t>(i), &c, 1)) return false;
        out[i] = c;
        if (!c) return true;
    }
    out[size - 1] = 0;
    return true;
}
bool HubPresent() {
    const uint32_t services = Ptr(base + 0x1C213DC);
    return services && Ptr(services + 0x94);
}
bool Frontend() { return Ptr(base + menu::kDirector) != 0; }
uint32_t Dgm() { return Ptr(base + menu::kGameManager); }

// The weapon rows MP never had (backend rows 7..11, DGM+0xF1 = 6..10): four campaign
// guns and the Hand Cannon (the Foam Finger, profile level 60). Their icons are
// frame6..10 of the FE11 movie (tools/build_loadout_frontend.py). The Hand Cannon has
// no text id in the tables, so its name is plain text, as the descriptions are.
struct ExtraGun { const char* nameId; const char* name; };
const ExtraGun kExtraGuns[] = {
    {"$online_weapon_ripper", "Ripper"}, {"$online_weapon_flamethrower", "Flamethrower"},
    {"$online_weapon_contactbeam", "Contact Beam"}, {"$online_weapon_detonator", "Detonator"}, {"hand cannon", "Hand Cannon"},
};
constexpr int kExtraCount = static_cast<int>(sizeof(kExtraGuns) / sizeof(kExtraGuns[0]));
bool extrasIn = false;   // the four FE11 detours are in

// --- the settings seam --------------------------------------------------------------------------
volatile LONG saves = 0, savesStored = 0, savesOnline = 0, setupSaves = 0;
char settingsKey[64] = "";
using SaveFn = void(__thiscall*)(void*, const char*, const char*);
void __fastcall OnSaveUserSetting(void* self, void*, const char* key, const char* value) {
    InterlockedIncrement(&saves);
    // 16.10.: the lobby's picks came this way offline and were never stored here, so the next
    // menu visit showed the old suit; the game's own save runs, and ours as well
    const bool hub = HubPresent();
    if (hub) {
        InterlockedIncrement(&savesOnline);
        reinterpret_cast<SaveFn>(base + kSaveUserSetting)(self, key, value);
    }
    char k[96] = "", v[256] = "";
    if (!CopyString(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(key)), k, sizeof(k)) || !k[0]) return;
    CopyString(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value)), v, sizeof(v));
    WritePrivateProfileStringA("UserSettings", k, v, mpnative::profilePath.c_str());
    InterlockedIncrement(&savesStored);
    const uint32_t dgm = Dgm();
    uint8_t weapon = 0, skin = 0;
    uint32_t flags = 0;
    if (dgm) { Read(dgm + kDgmWeapon, &weapon, 1); Read(dgm + kDgmSkin, &skin, 1); Read(dgm + kDgmFlags, &flags, 4); }
    if (settingsKey[0] && std::strcmp(k, settingsKey) == 0) {
        InterlockedIncrement(&setupSaves);
        char weaponName[64] = "", suitName[64] = "";
        const char* comma = std::strchr(v, ',');
        if (comma) {
            std::snprintf(weaponName, sizeof(weaponName), "%.*s", static_cast<int>(comma - v), v);
            std::snprintf(suitName, sizeof(suitName), "%s", comma + 1);
        }
        // an extra row saves an empty weapon name (the native name table holds 6): its name comes from DGM+0xF1
        if (weapon >= 6 && weapon < 6 + kExtraCount) std::snprintf(weaponName, sizeof(weaponName), "%s", kExtraGuns[weapon - 6].name);
        WritePrivateProfileStringA("Profile", "WeaponName", weaponName, mpnative::profilePath.c_str());
        WritePrivateProfileStringA("Profile", "SuitName", suitName, mpnative::profilePath.c_str());
        mpnative::profile.weapon = weapon;
        mpnative::profile.skin = skin;
        mpnative::SaveProfile();
    }
    Line("setting %s = \"%s\" kept in DS2Profile.ini (%s; weapon %u skin %u, DGM flags %08X)", k, v, hub ? "the hub's save ran too" : "no hub", weapon, skin, flags);
}

// --- the unlock data ------------------------------------------------------------------------------
volatile LONG feeds = 0, feedFaults = 0, rivetUnlocks = 0, setupReads = 0, xpAnswers = 0;
__declspec(noinline) bool CallUnlock(uint32_t content, uint32_t fn, uint32_t hash, bool& result) {
    __try {
        result = reinterpret_cast<bool(__thiscall*)(void*, uint32_t)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(content)), hash);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool CallDgmText(uint32_t dgm, uint32_t fn, const char* text) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, const char*)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(dgm)), text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool CallSavePlayerSetup(uint32_t dgm) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(base + kSavePlayerSetup)(reinterpret_cast<void*>(static_cast<uintptr_t>(dgm)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Every time the frontend comes up and before FE16 opens: idempotent.
void Feed(const char* why) {
    const uint32_t dgm = Dgm(), skinDb = Ptr(base + kSkinDb), content = Ptr(base + kUnlockedContent);
    const uint32_t skins = skinDb ? Ptr(skinDb + kSkinCount) : 0;
    if (!dgm) { Line("feed (%s): no game manager yet", why); return; }
    InterlockedIncrement(&feeds);
    char rivet[48] = "off";
    if (rivetGun && content) {
        bool unlocked = false;
        const uint32_t before = Ptr(content + kUnlockCount);
        if (!CallUnlock(content, base + kIsUnlocked, kRivetHash, unlocked)) { InterlockedIncrement(&feedFaults); std::snprintf(rivet, sizeof(rivet), "IsUnlocked faulted"); }
        else if (unlocked) std::snprintf(rivet, sizeof(rivet), "already unlocked");
        else if (before >= 256) std::snprintf(rivet, sizeof(rivet), "list full (%u)", before);
        else {
            bool ignored = false;
            const bool ok = CallUnlock(content, base + kForceUnlocked, kRivetHash, ignored);
            CallUnlock(content, base + kIsUnlocked, kRivetHash, unlocked);
            if (ok && unlocked) InterlockedIncrement(&rivetUnlocks);
            else InterlockedIncrement(&feedFaults);
            std::snprintf(rivet, sizeof(rivet), "%s (list %u -> %u)", ok && unlocked ? "unlocked" : "FAILED", before, Ptr(content + kUnlockCount));
        }
    }
    // [Unlocks] by the rows' code names into the skin DB's unlock level (+0x1C), which the suit
    // select reads (GetSuitUnlock 0x00A52A80); the lua's own levels stay where no key is set
    unsigned levelsSet = 0;
    const uint32_t skinArray = skinDb ? Ptr(skinDb + kSkinArray) : 0;
    for (uint32_t i = 0; skinArray && i < skins && i < 128; ++i) {
        char code[48] = "";
        if (!CopyString(Ptr(skinArray + i * kSkinEntrySize), code, sizeof(code)) || !code[0]) continue;
        const int level = static_cast<int>(GetPrivateProfileIntA("Unlocks", code, -1, iniPath.c_str()));
        int32_t now = 0;
        if (level < 0 || !Read(skinArray + i * kSkinEntrySize + kSkinUnlockLevel, &now, 4) || now < 0 || now == level) continue;   // special rows keep their way
        const int32_t want = level > 60 ? 60 : level;
        if (WriteBytes(skinArray + i * kSkinEntrySize + kSkinUnlockLevel, &want, 4)) ++levelsSet;
    }
    if (levelsSet) Line("feed (%s): %u suit unlock level(s) from [Unlocks]", why, levelsSet);
    uint32_t special = 0;
    if (specialSuits && skins) {
        if (!CallDgmText(dgm, base + kReadSpecialSuits, kSpecialSuits)) InterlockedIncrement(&feedFaults);
        special = Ptr(dgm + kDgmSpecialCount);
    }
    char stored[256] = "";
    uint8_t weaponBefore = 0, skinBefore = 0, weaponAfter = 0, skinAfter = 0;
    Read(dgm + kDgmWeapon, &weaponBefore, 1);
    Read(dgm + kDgmSkin, &skinBefore, 1);
    if (settingsKey[0]) GetPrivateProfileStringA("UserSettings", settingsKey, "", stored, sizeof(stored), mpnative::profilePath.c_str());
    if (stored[0] && skins) {
        if (CallDgmText(dgm, base + kReadPlayerSetup, stored)) InterlockedIncrement(&setupReads);
        else InterlockedIncrement(&feedFaults);
    }
    // an extra row: ReadPlayerSetup knows only the six native names and leaves the plasma cutter
    char weaponName[64] = "";
    GetPrivateProfileStringA("Profile", "WeaponName", "", weaponName, sizeof(weaponName), mpnative::profilePath.c_str());
    for (int k = 0; k < kExtraCount && extrasIn; ++k)
        if (_stricmp(weaponName, kExtraGuns[k].name) == 0) {
            const uint8_t f1 = static_cast<uint8_t>(6 + k);
            WriteBytes(dgm + kDgmWeapon, &f1, 1);
        }
    Read(dgm + kDgmWeapon, &weaponAfter, 1);
    Read(dgm + kDgmSkin, &skinAfter, 1);
    Line("feed (%s): skins %u, rivet gun %s, special suits %u%s, setup \"%s\" weapon %u -> %u skin %u -> %u, flags %08X", why, skins, rivet, special,
         specialSuits ? "" : " (off)", stored[0] ? stored : "(none stored)", weaponBefore, weaponAfter, skinBefore, skinAfter, Ptr(dgm + kDgmFlags));
}

// The stats seam asks for the local player's XP with no hub (runtime_coop_mpnative.inc).
bool FrontendXp(uint64_t id, uint32_t* xp) {
    if (!enabled || !settingsIn || id != 0 || !xp || !Frontend()) return false;
    *xp = unlockAll ? kMaxXp : mpnative::ProfileXp();
    InterlockedIncrement(&xpAnswers);
    return true;
}

// --- opening ------------------------------------------------------------------------------------
volatile LONG menuOpens = 0, menuRefusals = 0, lobbyTabAsks = 0;
// FE03 item 4 (runtime_coop_menu.inc): the native item 2 did the same push.
bool OpenFromMenu() {
    if (!enabled || !menuItem || !settingsIn) {
        InterlockedIncrement(&menuRefusals);
        Line("character setup refused from the Multiplayer screen: %s", !enabled ? "[Loadout] Enabled=0" : !menuItem ? "MenuItem=0" : "the settings seam is not in");
        return false;
    }
    Feed("character setup from the menu");
    const bool pushed = menu::PushScreen(kFe16Hash);
    InterlockedIncrement(pushed ? &menuOpens : &menuRefusals);
    Line("push FE16 from the Multiplayer screen: director %08X -> %s", Ptr(base + menu::kDirector), pushed ? "ok" : "FAILED");
    return pushed;
}
// The lobby's weapon and suit tabs (runtime_coop_nativelobby.inc), Versus and Mercenaries only.
// `mode` is the lobby's mode: a joiner's own gamemode::mode is the host's only after the launch.
bool LobbyTabsOpen(LONG mode) {
    if (!enabled || !lobbyTabs || !settingsIn) return false;
    InterlockedIncrement(&lobbyTabAsks);
    return mode == gamemode::Mercenaries || mode == gamemode::Versus;
}

// --- applying the pick: the guns ----------------------------------------------------------------
// The weapon rows of the setup (DGM+0xF1 = row - 1; row 0, the Pulse Rifle, is
// always given). Versus gives the MP guns (their records pass the map's gate there),
// Mercenaries the campaign twin of the pick with its campaign values and bench
// upgrades (user, 12.10.). A gun MP never had is the campaign gun in both; the
// Rivet Gun has no campaign twin, so Mercenaries gives the plasma cutter for it.
// mpmap::GiveLoadout falls back to the campaign gun when an MP gun is refused.
struct GunRow { const char* name; uint32_t mp[4]; int kind; };   // kind: bot::kGunCatalogue index, -1 the Rivet twin, -2 the Hand Cannon
// The campaign Rivet Gun (unlock id 0x9954AE8F, as the FE11 row); registered with the Quartermaster in every level.
const uint32_t kRivetTwin[4] = {0x33CB4867, 0x256213EC, 0x4E4F4F4E, 0x32304E41};
// "Finger Gun Plasma Cutter" in global_assets: no ammo use (CMD_SetAmmoUsage 0), store gate achievement 46.
const uint32_t kHandCannon[4] = {0x38CB6750, 0x72BC9767, 0x54524542, 0x314D4152};
const GunRow kGunRows[] = {
    {"pulse rifle", {0x58C9B246, 0x436790D5, 0x3532554C, 0x58583337}, 1},
    {"plasma cutter", {0x58C9B97E, 0x6F595DA9, 0x3532554C, 0x58583337}, 0},
    {"rivet gun", {0x58CB44A3, 0xCEB46ED1, 0x3532554C, 0x58583337}, -1},
    {"line gun", {0x58C9B246, 0x260A3DFE, 0x3532554C, 0x58583337}, 2},
    {"force gun", {0x58CAF628, 0x88C7B4B5, 0x3532554C, 0x58583337}, 8},
    {"seeker rifle", {0x58CB449C, 0x8A7AAF4F, 0x3532554C, 0x58583337}, 7},
    {"javelin gun", {0x58CB0833, 0x525558CF, 0x3532554C, 0x58583337}, 3},
    {"ripper", {}, 4},
    {"flamethrower", {}, 6},
    {"contact beam", {}, 9},
    {"detonator", {}, 5},
    {"hand cannon", {}, -2},
};
constexpr int kGunRowCount = static_cast<int>(sizeof(kGunRows) / sizeof(kGunRows[0]));
volatile LONG gunPicks = 0;
// 4 Versus, 1 Mercenaries, 0 neither (only on the MP map itself: the picked mode outlives the visit).
int ApplyMode() {
    if (!enabled) return 0;
    const LONG mode = InterlockedCompareExchange(&gamemode::mode, 0, 0);
    if (arena::RunningNow()) return arena::ModeNow(static_cast<int>(mode)) == gamemode::Mercenaries ? 1 : 0;
    if (!mpmap::InVisit()) return 0;
    if (mode == gamemode::Versus && mpnative::InVersus()) return 4;
    if (mode == gamemode::Mercenaries) return 1;
    return 0;
}
int Guns(GunPick* out, int max) {
    const int mode = ApplyMode();
    if (!mode || max < 2) return 0;
    const uint32_t dgm = Dgm();
    uint8_t f1 = 0;
    if (!dgm || !Read(dgm + kDgmWeapon, &f1, 1)) return 0;
    int row = static_cast<int>(f1) + 1;
    if (row < 1 || row >= kGunRowCount) row = 1;
    const bool versus = mode == 4;
    // a pick above the profile level ([Unlocks] changed after the pick), or the Hand Cannon in Versus
    const char* key = coop_progress::BackendKey(row);
    const bool cannonInVersus = kGunRows[row].kind == -2 && versus;
    if (cannonInVersus || (!unlockAll && key && !progress::Unlocked(key))) {
        if (InterlockedIncrement(&gunPicks) <= 20) Line("%s picked: %s, the plasma cutter instead", kGunRows[row].name, cannonInVersus ? "never in Versus" : "above the profile level");
        if (cannonInVersus) notice::Enqueue("NO HAND CANNON IN VERSUS - PLASMA CUTTER");
        row = 1;
    }
    auto fill = [&](GunPick& p, int r) {
        const GunRow& g = kGunRows[r];
        const bool mp = versus && g.mp[0] != 0;
        const uint32_t* campaign = g.kind >= 0 ? bot::kGunCatalogue[g.kind].guid : g.kind == -2 ? kHandCannon : kRivetTwin;
        p.name = g.name;
        p.kind = g.kind;
        p.guid = mp ? g.mp : campaign;
        p.fallback = mp ? campaign : g.kind < 0 ? bot::kGunCatalogue[0].guid : campaign;   // the twin or the cannon refused: the plasma cutter
    };
    fill(out[0], 0);
    fill(out[1], row);
    if (InterlockedIncrement(&gunPicks) <= 20)
        Line("guns for %s: %s + %s (%s), weapon byte %u", versus ? "Versus" : "Mercenaries", out[0].name, out[1].name,
             versus && kGunRows[row].mp[0] ? "MP gun, campaign twin if refused" : kGunRows[row].kind < 0 ? "campaign Rivet Gun, plasma cutter if refused" : "campaign gun", f1);
    return 2;
}

// --- Versus: no armour, no suit bonuses (user, 12.10.) -----------------------------------------
// Player::ApplyDamageModifiers (0x0087CF70) multiplies the damage by the worn suit's
// factor ([[Player+0x1688]+0xC]+0x2F4) at 0x0087D0DC, mulss xmm0,[esi+0x50] with
// xmm0 the factor, on whichever copy of a body takes the hit on this machine. The
// MP suit leaves it at 1.0, so in a Versus visit every suit does. The suits' gun
// bonuses come from Gun::GetBonusDamageModifier (0x00DBA1D0, thiscall, float in st0,
// 0.0 without a suit, added): 0.0 there too. Mercenaries and the campaign keep both.
constexpr uint32_t kArmourSite = 0x47D0DC, kBonusDamage = 0x9BA1D0;
safetyhook::MidHook armourHook;
safetyhook::InlineHook bonusHook;
volatile LONG armourHits = 0, armourNeutralised = 0, bonusZeroed = 0;
float lastArmour = 1.0f;
bool VersusVisit() { return mpnative::InVersus() && mpmap::InVisit(); }
void OnArmour(safetyhook::Context& ctx) {
    InterlockedIncrement(&armourHits);
    if (!VersusVisit() || ctx.xmm0.f32[0] == 1.0f) return;
    lastArmour = ctx.xmm0.f32[0];
    ctx.xmm0.f32[0] = 1.0f;
    InterlockedIncrement(&armourNeutralised);
}
float __fastcall OnBonusDamage(void* gun, void*) {
    if (VersusVisit()) { InterlockedIncrement(&bonusZeroed); return 0.0f; }
    return bonusHook.thiscall<float>(gun);
}

// --- Versus: the MP guns with the Quartermaster ----------------------------------------------
// The MP GunProxyRegistry (34CA0BEE-2FA1A162) stays blocked with the rest of the MP
// player setup, so the seven MP guns have no Quartermaster entry and their
// requisition fails. Each Versus arrival registers them the registry's way:
// Quartermaster::RegisterGunProxy (0x00DD6B50, thiscall(guid*) ret 4) for each proxy
// whose record the map keeps (it adds the entry before reading the record). The
// weapon stream budget goes up as for the store.
constexpr uint32_t kRegisterGunProxy = 0x9D6B50, kQmEntryCount = 0x18;
uint32_t gunsEpoch = 0;
volatile LONG gunsRegistered = 0, gunsKnown = 0, gunsMissing = 0, gunsFaults = 0;
__declspec(noinline) bool SpawnInfoFor(uint32_t sim, const uint32_t* guid) {
    __try {
        necro::SpawnInfo info{};
        return necro::GetSpawnInfo(sim, info, guid);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) int RegisterGunRaw(uint32_t qm, const uint32_t* guid) {
    __try {
        return reinterpret_cast<bool(__thiscall*)(void*, const uint32_t*)>(base + kRegisterGunProxy)(reinterpret_cast<void*>(static_cast<uintptr_t>(qm)), guid) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
void RegisterMpGuns() {
    if (!VersusVisit() || !mpmap::Arrived() || mpmap::visit.epochs == gunsEpoch) return;
    const uint32_t qm = Ptr(base + bot::kQuartermasterRva), sim = Ptr(base + necro::kSimManager);
    const uint32_t before = qm ? Ptr(qm + kQmEntryCount) : 0;
    if (!qm || !sim || !before) return;   // the level's own registries come first
    gunsEpoch = mpmap::visit.epochs;
    int added = 0, known = 0, missing = 0;
    for (int i = 0; i < 7; ++i) {
        const uint32_t* guid = kGunRows[i].mp;
        if (!SpawnInfoFor(sim, guid)) {
            ++missing;
            InterlockedIncrement(&gunsMissing);
            Line("MP gun %s %08X-%08X: no record on this map, not registered (the campaign twin is given)", kGunRows[i].name, guid[0], guid[1]);
            continue;
        }
        const int r = RegisterGunRaw(qm, guid);
        if (r < 0) InterlockedIncrement(&gunsFaults);
        else if (r) { ++added; InterlockedIncrement(&gunsRegistered); }
        else { ++known; InterlockedIncrement(&gunsKnown); }
    }
    shop::RaiseGunStreams(qm);
    Line("MP guns with the Quartermaster (visit %u): %d registered, %d already there, %d without a record; entries %u -> %u, weapon streams %u",
         gunsEpoch, added, known, missing, before, Ptr(qm + kQmEntryCount), static_cast<unsigned>(static_cast<uint8_t>(Ptr(qm + bot::kQmMaxStreams))));
}

// --- the weapon list: four more rows ------------------------------------------------------------
// FE11's list is the static 7-row table 0x01B2DCF0 (row 0, the Pulse Rifle, never
// listed). Four FE11-only functions take the extra rows (backend 7..10) after the
// native ones: FE11GetNumWeapons 0x00A7C800 (no args, int), UIIndexToBackendIndex
// 0x00A7C8D0 and BackendIndexToUIIndex 0x00A7C9D0 (thiscall(screen, i) ret 4),
// GetWeaponUnlock 0x00A6BF10 (cdecl(idx, level, name*, desc*, unlockLevel*, clip*,
// damage*, hash*), no bounds check, so it answers every index above 6 itself).
constexpr uint32_t kNumWeapons = 0x67C800, kUiToBackend = 0x67C8D0, kBackendToUi = 0x67C9D0, kWeaponUnlock = 0x66BF10;
safetyhook::InlineHook numHook, uiHook, backendHook, unlockHook;
volatile LONG extraRowsShown = 0, extraUnlocks = 0;
int NativeWeaponCount() { return numHook.call<int>(); }
int __cdecl OnNumWeapons() { return numHook.call<int>() + kExtraCount; }
int __fastcall OnUiToBackend(void* screen, void*, int ui) {
    const int n = NativeWeaponCount();
    if (ui < n) return uiHook.thiscall<int>(screen, ui);
    const int k = ui - n;
    if (k >= kExtraCount) return -1;
    InterlockedIncrement(&extraRowsShown);
    return 7 + k;
}
int __fastcall OnBackendToUi(void* screen, void*, int backend) {
    if (backend <= 6) return backendHook.thiscall<int>(screen, backend);
    const int k = backend - 7;
    return k < kExtraCount ? NativeWeaponCount() + k : -1;
}
// The progress gun (coop_progress.h kGuns) of a backend row; -1 for the Hand Cannon.
int ProgressGun(int backend) {
    static constexpr int kMap[] = {1, 0, 9, 2, 6, 10, 7, 5, 3, 4, 8, -1};
    return backend >= 0 && backend < static_cast<int>(sizeof(kMap) / sizeof(kMap[0])) ? kMap[backend] : -1;
}
// The description under the name: the weapon level and what it gives.
char rowText[12][128];
const char* WeaponText(int backend) {
    char* out = rowText[backend >= 0 && backend < 12 ? backend : 0];
    const int gun = ProgressGun(backend);
    if (backend == 11) {
        std::snprintf(out, 128, "prestige reward of level %u. needs no ammo and drops almost anything; never in versus",
                      progress::UnlockLevel("HandCannon"));
        return out;
    }
    if (gun < 0) return out;
    const auto& r = progress::rules;
    const unsigned lvl = progress::WeaponLevelOf(gun);
    const LONG kills = InterlockedCompareExchange(&progress::kills[gun], 0, 0);
    const unsigned clip = coop_progress::ClipTier(r, lvl), dmg = coop_progress::DamageTier(r, lvl);
    char now[64];
    if (clip || dmg)
        std::snprintf(now, sizeof(now), "magazine x%.2f, damage x%.2f", clip ? r.clipFactor[clip - 1] : 1.0f, dmg ? r.damageFactor[dmg - 1] : 1.0f);
    else
        std::snprintf(now, sizeof(now), "bigger magazine at level %u", r.clipLevel[0]);
    const uint32_t next = coop_progress::KillsForNext(r, lvl);
    if (progress::TestAll()) std::snprintf(out, 128, "weapon level %u (unlocks=1). %s", lvl, now);
    else if (next) std::snprintf(out, 128, "weapon level %u, %ld kills (level %u at %u). %s", lvl, kills, lvl + 1, next, now);
    else std::snprintf(out, 128, "weapon level %u (max), %ld kills. %s", lvl, kills, now);
    return out;
}
volatile LONG rowBackend = -1;   // the row InitWeaponsData sets up next (0x00A7CC10, then 0x00A7CCF3)
void __cdecl OnWeaponUnlock(int idx, int level, const char** name, const char** desc, int* unlockLevel, int* clip, int* damage, uint32_t* hash) {
    InterlockedExchange(&rowBackend, idx);
    if (idx <= 6) {
        unlockHook.call<void>(idx, level, name, desc, unlockLevel, clip, damage, hash);
        if (desc && progress::enabled) *desc = WeaponText(idx);
        if (unlockLevel && idx >= 1 && !unlockAll) *unlockLevel = static_cast<int>(progress::UnlockLevel(coop_progress::BackendKey(idx)));
        return;
    }
    InterlockedIncrement(&extraUnlocks);
    const int k = idx - 7 < kExtraCount ? idx - 7 : 0;
    if (name) *name = kExtraGuns[k].nameId;
    if (desc) *desc = WeaponText(idx);
    if (unlockLevel) *unlockLevel = unlockAll ? 0 : static_cast<int>(progress::UnlockLevel(coop_progress::BackendKey(idx)));
    if (clip) *clip = 0;
    if (damage) *damage = 0;
    if (hash) *hash = 0;
}

// FE11 HandleInput 0x00A889F0 (thiscall(code) ret 4): up and down (8, 9) move the scroll
// iterator at +0x5C, which scrolls past the 10 visible rows by itself (the arrow keys
// worked, 15.10.). FE11 has no PC list helper at +0x1F8: only FE12's Init makes one
// (0x00A63F5C), and the base mouse handler (0x00A6AFC0, thiscall(screen, a, mouse) ret 8)
// gives the wheel (float at mouse+8) only to that helper. A wheel notch on FE11 is sent
// to HandleInput as up or down, under the same checks the base handler makes.
constexpr uint32_t kFe11Input = 0x6889F0, kInputHelper = 0x1F8, kFe11Vtable = 0x173B4D8;
constexpr uint32_t kMouseInput = 0x66AFC0, kMouseEnabled = 0x1F4, kMouseWheel = 8, kInputMode = 0x1C1C0A8, kInputModeByte = 0x578;
const unsigned char kFe11InputBytes[] = {0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0xF8, 0x01, 0x00, 0x00, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x85, 0xC9};
const unsigned char kMouseInputBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x64, 0x53, 0x56, 0x33, 0xDB, 0x8B, 0xF1, 0x57};
safetyhook::InlineHook fe11InputHook, mouseInputHook;
volatile LONG fe11Keys = 0, fe11Wheel = 0, fe11WheelSkipped = 0;
int __fastcall OnFe11Input(void* screen, void*, int code) {
    if (code == 8 || code == 9) InterlockedIncrement(&fe11Keys);
    return fe11InputHook.thiscall<int>(screen, code);
}
int __fastcall OnMouseInput(void* screen, void*, int a, void* mouse) {
    const uint32_t s = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(screen));
    uint32_t vtable = 0, helper = 1, mouseOn = 0, mode = 0;
    uint8_t modeByte = 0;
    float wheel = 0.0f;
    if (mouse && Read(s, &vtable, 4) && vtable == base + kFe11Vtable &&
        Read(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mouse)) + kMouseWheel, &wheel, 4) && wheel != 0.0f) {
        const bool ok = Read(s + kInputHelper, &helper, 4) && !helper && Read(s + kMouseEnabled, &mouseOn, 4) && mouseOn &&
                        Read(base + kInputMode, &mode, 4) && mode && Read(mode + kInputModeByte, &modeByte, 1) && modeByte == 0x12;
        if (ok) {
            reinterpret_cast<int(__thiscall*)(void*, int)>(base + kFe11Input)(screen, wheel < 0.0f ? 9 : 8);
            InterlockedIncrement(&fe11Wheel);
        } else {
            InterlockedIncrement(&fe11WheelSkipped);
        }
    }
    return mouseInputHook.thiscall<int>(screen, a, mouse);
}

// FE11 row setup 0x00A48240 (thiscall(row, text*, unlockLevel, clipLevel, damageLevel,
// clipOn, damageOn, locked, selected) ret 0x20; its one caller is InitWeaponsData right
// after GetWeaponUnlock). The upgrade marks follow the weapon level: the level a tier
// needs and whether this gun has it (mClipBonusUnlocked / mDamageBonusUnlocked).
constexpr uint32_t kRowSetup = 0x648240;
const unsigned char kRowSetupBytes[] = {0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xF1, 0x8B, 0x48, 0x04, 0x8B, 0x00, 0x85, 0xC0, 0x75, 0x05};
safetyhook::InlineHook rowSetupHook;
volatile LONG rowSetups = 0, rowMarks = 0;
void __fastcall OnRowSetup(void* row, void*, void* text, int unlockLevel, int clipLevel, int damageLevel, int clipOn, int damageOn, int locked, int selected) {
    InterlockedIncrement(&rowSetups);
    const int backend = static_cast<int>(InterlockedExchange(&rowBackend, -1));
    const int gun = ProgressGun(backend);
    if (progress::enabled && backend == 11) {
        clipLevel = damageLevel = 0;
        clipOn = damageOn = 1;
    } else if (progress::enabled && gun >= 0) {
        const auto& r = progress::rules;
        const unsigned lvl = progress::WeaponLevelOf(gun);
        clipLevel = static_cast<int>(r.clipLevel[0]);
        damageLevel = static_cast<int>(r.damageLevel[0]);
        clipOn = coop_progress::ClipTier(r, lvl) ? 1 : 0;
        damageOn = coop_progress::DamageTier(r, lvl) ? 1 : 0;
        InterlockedIncrement(&rowMarks);
    }
    rowSetupHook.thiscall<void>(row, text, unlockLevel, clipLevel, damageLevel, clipOn, damageOn, locked, selected);
}

// --- applying the pick: the suit -------------------------------------------------------------
// The pick is the PlayerSkinDB row in DGM+0xF0, matched by the row's code-name hash
// (entry +0x10) to the campaign suits of randomizer\ds2coop_mpskins.lua
// (generated_loadout_rows.h); the row index is the fallback. The MP suit's colour
// rows stand for the MP suit itself, which the campaign Isaac cannot wear yet: the
// campaign Security suit stands in (the MP colours are for later, user 12.10.).
const uint32_t kMpSuitStandIn[4] = {0x39CA8FDF, 0x97CF9234, 0x574F5242, 0x3130314E};
volatile LONG suitPicks = 0;
// An MP row goes to mpsuit (registered MP suit, its skin): *handled then, and no GUID.
const uint32_t* Suit(const char** name, uint32_t player, bool* handled) {
    if (handled) *handled = false;
    if (!ApplyMode()) return nullptr;
    const uint32_t dgm = Dgm(), db = Ptr(base + kSkinDb);
    uint8_t f0 = 0;
    if (!dgm || !Read(dgm + kDgmSkin, &f0, 1)) return nullptr;
    const uint32_t array = db ? Ptr(db + kSkinArray) : 0, count = db ? Ptr(db + kSkinCount) : 0;
    uint32_t hash = 0;
    if (array && f0 < count) Read(array + f0 * kSkinEntrySize + kSkinNameHash, &hash, 4);
    const loadout_data::SuitRow* row = nullptr;
    for (const auto& s : loadout_data::kSuits)
        if ((hash && s.nameHash == hash) || (!hash && s.skinIndex == f0)) { row = &s; break; }
    if (InterlockedIncrement(&suitPicks) <= 20)
        Line("suit pick: skin row %u of %u (name hash %08X) -> %s", f0, count, hash, row ? row->code : "an MP colour: the campaign Security suit stands in");
    if (!row && f0 < 36 && mpsuit::AskDress(player)) {
        if (name) *name = "MP suit";
        if (handled) *handled = true;
        return nullptr;
    }
    if (name) *name = row ? row->code : "MP suit (Security stands in)";
    return row ? row->uniform : kMpSuitStandIn;
}
// The same pick for a body that is about to be born (mpmap's arrival spawn): only a campaign suit
// row, nothing asked of mpsuit and nothing logged; an MP colour keeps the old dress path.
const uint32_t* SuitRow(const char** name) {
    if (!ApplyMode()) return nullptr;
    const uint32_t dgm = Dgm(), db = Ptr(base + kSkinDb);
    uint8_t f0 = 0;
    if (!dgm || !Read(dgm + kDgmSkin, &f0, 1)) return nullptr;
    const uint32_t array = db ? Ptr(db + kSkinArray) : 0, count = db ? Ptr(db + kSkinCount) : 0;
    uint32_t hash = 0;
    if (array && f0 < count) Read(array + f0 * kSkinEntrySize + kSkinNameHash, &hash, 4);
    for (const auto& s : loadout_data::kSuits)
        if ((hash && s.nameHash == hash) || (!hash && s.skinIndex == f0)) {
            if (name) *name = s.code;
            return s.uniform;
        }
    return nullptr;
}

// --- the game thread ----------------------------------------------------------------------------
bool frontendSeen = false;
DWORD dirtyAt = 0;
volatile LONG dirtySaves = 0;
void Tick(DWORD now) {
    if (!enabled) return;
    RegisterMpGuns();
    if (!settingsIn) return;
    const bool frontend = Frontend();
    if (frontend && !frontendSeen) Feed("the frontend is up");
    frontendSeen = frontend;
    // A pick on the lobby's tabs has no FE16 hide to save it: the dirty bit does.
    if (!frontend || now - dirtyAt < 500) return;
    dirtyAt = now;
    const uint32_t dgm = Dgm();
    uint32_t flags = 0;
    if (!dgm || !Read(dgm + kDgmFlags, &flags, 4) || !(flags & kSetupDirty)) return;
    const bool ok = CallSavePlayerSetup(dgm);
    InterlockedIncrement(&dirtySaves);
    Line("the setup was changed (flags %08X): SavePlayerSetup %s, flags now %08X", flags, ok ? "called" : "FAULTED", Ptr(dgm + kDgmFlags));
}

void Install(FILE* log, const std::string& ini) {
    const char* f = ini.c_str();
    iniPath = ini;
    enabled = GetPrivateProfileIntA("Loadout", "Enabled", 0, f) != 0;
    menuItem = GetPrivateProfileIntA("Loadout", "MenuItem", 1, f) != 0;
    lobbyTabs = GetPrivateProfileIntA("Loadout", "LobbyTabs", 1, f) != 0;
    unlockAll = GetPrivateProfileIntA("Loadout", "Unlocks", 0, f) != 0;
    specialSuits = GetPrivateProfileIntA("Loadout", "SpecialSuits", 1, f) != 0;
    rivetGun = GetPrivateProfileIntA("Loadout", "RivetGun", 1, f) != 0;
    std::fprintf(log, "loadout config enabled=%d menu_item=%d lobby_tabs=%d unlocks=%s special_suits=%d rivet_gun=%d profile %s\n", enabled ? 1 : 0, menuItem ? 1 : 0,
                 lobbyTabs ? 1 : 0, unlockAll ? "all (level 60)" : "own XP", specialSuits ? 1 : 0, rivetGun ? 1 : 0, mpnative::profilePath.c_str());
    if (!enabled) return;
    // The character setup's picks only count in Mercenaries and Versus (user, 16.10.: make it obvious).
    // LCH2 ids from deadspace2_global.en.lh2: "character setup" twice, "weapon select", "suit select".
    notice::AddOverride(0x0C9541E1, "character setup - mercs & versus");
    notice::AddOverride(0xA491E93A, "character setup - mercs & versus");
    notice::AddOverride(0x9EEAD300, "weapon select - mercs & versus only");
    notice::AddOverride(0xD2ECD356, "suit select - mercs & versus only");
    sitesOk = true;
    for (const Site& s : kSites) {
        unsigned char actual[16]{};
        const bool ok = Read(base + s.rva, actual, s.size) && std::memcmp(actual, s.bytes, s.size) == 0;
        sitesOk = sitesOk && ok;
        std::fprintf(log, "loadout sig %-38s %08X %s\n", s.name, static_cast<unsigned>(base + s.rva), ok ? "ok" : "MISMATCH");
    }
    if (!sitesOk) { std::fprintf(log, "loadout off: a site did not match\n"); return; }
    CopyString(Ptr(base + kSettingsKeyPtr), settingsKey, sizeof(settingsKey));
    settingsIn = menu::SwapSlot(log, "BlazeSettingsManager::SaveUserSetting", kSettingsSlot, kSaveUserSetting, reinterpret_cast<void*>(OnSaveUserSetting));
    if (settingsIn && menuItem && menu::installed)
        menu::SetLabel(log, "multiplayer item 4", menu::kFe03Labels + 16, 0, reinterpret_cast<const char*>(static_cast<uintptr_t>(base + kCharacterSetupLabel)));
    {
        // the extra weapon rows: all four functions or none
        static const unsigned char kNum[] = {0x83, 0xEC, 0x0C, 0x53, 0x55, 0x33, 0xED, 0x56, 0x57, 0x89, 0x6C, 0x24, 0x14};
        static const unsigned char kUi[] = {0x83, 0xEC, 0x0C, 0x53, 0x55, 0x56, 0x33, 0xED, 0x57, 0xC7, 0x44, 0x24, 0x14, 0xFF, 0xFF, 0xFF, 0xFF};
        static const unsigned char kBack[] = {0x83, 0xEC, 0x08, 0x8B, 0x44, 0x24, 0x0C, 0x55, 0x33, 0xED, 0x89, 0x44, 0x24, 0x04, 0x85, 0xC0};
        static const unsigned char kUnlock[] = {0x51, 0xA1, 0xE4, 0xC0, 0x01, 0x02, 0x8B, 0x48, 0x38, 0x8B, 0x44, 0x24, 0x10, 0xC1, 0xE9, 0x09};
        extrasIn = settingsIn && hook_site::Install(log, "loadout FE11GetNumWeapons", base + kNumWeapons, kNum, sizeof(kNum), reinterpret_cast<void*>(OnNumWeapons), numHook) &&
                   hook_site::Install(log, "loadout UIIndexToBackendIndex", base + kUiToBackend, kUi, sizeof(kUi), reinterpret_cast<void*>(OnUiToBackend), uiHook) &&
                   hook_site::Install(log, "loadout BackendIndexToUIIndex", base + kBackendToUi, kBack, sizeof(kBack), reinterpret_cast<void*>(OnBackendToUi), backendHook) &&
                   hook_site::Install(log, "loadout GetWeaponUnlock", base + kWeaponUnlock, kUnlock, sizeof(kUnlock), reinterpret_cast<void*>(OnWeaponUnlock), unlockHook);
        if (!extrasIn) { numHook = {}; uiHook = {}; backendHook = {}; unlockHook = {}; }
        // the mouse wheel scrolls FE11 too (the eleventh row); up and down are only counted
        const bool keysIn = extrasIn && hook_site::Install(log, "loadout FE11 HandleInput (count)", base + kFe11Input, kFe11InputBytes,
                                                           sizeof(kFe11InputBytes), reinterpret_cast<void*>(OnFe11Input), fe11InputHook);
        const bool wheelIn = extrasIn && hook_site::Install(log, "loadout menu mouse input (FE11 wheel)", base + kMouseInput, kMouseInputBytes,
                                                            sizeof(kMouseInputBytes), reinterpret_cast<void*>(OnMouseInput), mouseInputHook);
        if (extrasIn && !wheelIn) std::fprintf(log, "loadout: the mouse wheel does not scroll FE11 (the arrow keys still do)\n");
        (void)keysIn;
        // the upgrade marks by weapon level (optional: without it they follow the profile level)
        const bool marksIn = extrasIn && hook_site::Install(log, "loadout FE11 row setup", base + kRowSetup, kRowSetupBytes, sizeof(kRowSetupBytes),
                                                            reinterpret_cast<void*>(OnRowSetup), rowSetupHook);
        std::fprintf(log, "loadout weapon rows: upgrade marks by weapon level %s; unlock levels %s from [Unlocks] (Line Gun %u, Ripper %u, Hand Cannon %u, ...)\n",
                     marksIn ? "in" : "OFF", unlockAll ? "off (Unlocks=1)" : "on", progress::UnlockLevel("LineGun"), progress::UnlockLevel("Ripper"),
                     progress::UnlockLevel("HandCannon"));
        // Versus: no armour, no suit bonuses
        static const unsigned char kArmour[] = {0xF3, 0x0F, 0x59, 0x46, 0x50, 0xF3, 0x0F, 0x11, 0x46, 0x50};
        static const unsigned char kBonus[] = {0x51, 0x56, 0x8B, 0xF1, 0x85, 0xF6, 0x74, 0x0F, 0x0F, 0xB6, 0x05, 0x5A, 0xB7, 0x0A, 0x02, 0x50};
        const bool armourIn = hook_site::InstallMid(log, "loadout suit armour (Versus)", base + kArmourSite, kArmour, sizeof(kArmour), OnArmour, armourHook);
        const bool bonusIn = hook_site::Install(log, "loadout suit gun bonus (Versus)", base + kBonusDamage, kBonus, sizeof(kBonus), reinterpret_cast<void*>(OnBonusDamage), bonusHook);
        std::fprintf(log, "loadout extra weapon rows %s; Versus armour %s, suit gun bonus %s\n", extrasIn ? "in (Ripper, Flamethrower, Contact Beam, Detonator, Hand Cannon)" : "OFF",
                     armourIn ? "neutral" : "NOT hooked (suits keep their armour)", bonusIn ? "off" : "NOT hooked");
    }
    std::fprintf(log, "loadout %s: settings key \"%s\", FE16 from the Multiplayer screen's item 4 %s, lobby tabs %s\n", settingsIn ? "installed" : "NOT installed",
                 settingsKey, settingsIn && menuItem && menu::installed ? "on" : "off", settingsIn && lobbyTabs ? "on in Versus and Mercenaries lobbies" : "off");
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    static LONG last = -1;
    const LONG sum = saves + feeds * 3 + menuOpens * 5 + menuRefusals * 7 + dirtySaves * 11 + feedFaults * 13 + xpAnswers / 50 + gamemode::native::skinsOpens * 17 +
                     armourNeutralised / 10 + gunsRegistered * 19 + gunsMissing * 23 + gunPicks * 29 + extraUnlocks / 20 + fe11Keys * 31 + fe11Wheel * 37 + rowSetups / 10;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "loadout settings saves=%ld stored=%ld online=%ld setup=%ld dirty_saves=%ld | feeds=%ld faults=%ld rivet_unlocks=%ld setup_reads=%ld xp_answers=%ld | "
                      "menu opens=%ld refusals=%ld lobby_tab_asks=%ld | suit list opens=%ld redirected=%ld\n",
                 saves, savesStored, savesOnline, setupSaves, dirtySaves, feeds, feedFaults, rivetUnlocks, setupReads, xpAnswers, menuOpens, menuRefusals, lobbyTabAsks,
                 gamemode::native::skinsOpens, gamemode::native::skinsRedirects);
    std::fprintf(log, "loadout extra rows shown=%ld unlock_answers=%ld | Versus armour hits=%ld neutralised=%ld (last factor %.2f) gun_bonus_zeroed=%ld | "
                      "MP guns registered=%ld known=%ld missing=%ld faults=%ld | gun picks=%ld\n",
                 extraRowsShown, extraUnlocks, armourHits, armourNeutralised, static_cast<double>(lastArmour), bonusZeroed, gunsRegistered, gunsKnown, gunsMissing,
                 gunsFaults, gunPicks);
    std::fprintf(log, "loadout suit picks=%ld | FE11 up/down=%ld wheel notches=%ld wheel skipped=%ld row setups=%ld marks=%ld\n", suitPicks, fe11Keys, fe11Wheel,
                 fe11WheelSkipped, rowSetups, rowMarks);
}
}  // namespace loadout
