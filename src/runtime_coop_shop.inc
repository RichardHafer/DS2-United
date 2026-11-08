// Dead Space 2 United by Hafer.
// The supply terminal for survival on the multiplayer maps ([Shop], logic in
// coop_shop.h, the panel in runtime_coop_overlay.inc).
//
// Numpad * opens it between waves on an [MpMap] visit; the survival state comes
// from the randomizer's export DS2Survival_State (0 off, 1 arming, 2 wave,
// 3 intermission, 4 dead). Numpad 4/6 change the tab, 8/2 the row, 5 buys,
// 0 or Escape close. While it is open the whole numpad is its own: the turret,
// the gadgets and the bots ask HoldsKey before they read a key. The game keeps
// running; the panel hides while the game is paused.
//
// Money is the local Isaac's credits (general inventory Player+0xBD0, credits
// +0x28). Delivery reuses what the bots and the survival rewards do:
//   supplies: CanFitItem, GetGlobalPickupItem, the item's Pickup (vtable +0x1F8)
//     into the inventory, then SubtractCurrency; without room the item is put
//     down in front of him with Item::Drop (vtable +0x1F0);
//   guns: a GunProxy into a free slot (Item::Pickup 0x00DEF170), then
//     Quartermaster::RequisitionGun; the real gun swaps in once its stream is
//     there, else the purchase is refunded after 20 s;
//   upgrades: the next nodes of the item's global tree filled as the bots'
//     bench does, then Gun::CalcUpgradedValues (vtable +0x284), or for the RIG
//     and stasis trees Player::CalcUpgradedValues (0x00879B80, thiscall(player,
//     true), ret 4, called the same way by ~PlayerUpgradeMainSM at 0x008A5802);
//   suits: ChangeToUniform with a uniform that is registered and loaded here;
//   gadgets: the turret's magazine refilled, a recharging mine or field ready.
// Every attempt is logged with price, credits before and after and the result;
// a delivery that fails is refunded.
#include "runtime_coop_overlay.inc"
#include "runtime_coop_panelinput.inc"
namespace escalation { unsigned PricePercent(); }   // runtime_coop_escalation.inc
namespace mpnative { bool InVersus(); }             // runtime_coop_mpnative.inc: no terminal in Versus
namespace gamemode { extern volatile LONG mode; }   // runtime_coop_gamemode.inc: 1 Mercenaries, 2 Objective, 3 Zone
namespace survmusic { extern volatile LONG state, wave; }   // runtime_coop_survmusic.inc: the host's director state on a client
namespace arena { bool RunningNow(); int ModeNow(int picked); }   // runtime_coop_arena.inc
int NetRole();   // runtime_coop.inc
namespace shop {
using coop_shop::Item;
using coop_shop::Kind;

bool wanted = false, installed = false;
bool deliveryOk = false, gunsOk = false, upgradesOk = false, suitsOk = false;
int key = 'V';
bool campaignAllowed = false, duringWaves = false, escapeCloses = true, notices = true, requestStreams = false, intoInventory = true, replaceHeld = true;
int rowsWanted = 10;
int pauseSeconds = 20, startDelaySeconds = 20;
uint32_t priceGun[coop_shop::kGunCount]{}, priceAmmo[coop_shop::kGunCount]{}, priceSuitOf[bot::kSuitKinds]{};
uint32_t priceMed[3] = {400, 900, 1800}, priceStasis = 500, priceDrop = 1500, priceNode = 800, priceRigNode = 1000, priceStasisNode = 700, priceSuit = 3000;
uint32_t priceTurret = 600, priceMine = 400, priceField = 500;

constexpr uint32_t kCredits = bot::kGeneralInventory + bot::kInventoryCredits;
constexpr uint32_t kPlayerCalcRva = 0x479B80, kPlayerCalcRetRva = 0x479EC8, kPlayerFlagWord = 0x3D8, kMpBodyBit = 1u << 13;
constexpr uint32_t kUniformManagerRva = 0x1C1C0DC, kUniformManagerVtableRva = 0x175CA78, kChangeToUniformRva = 0xAE0610;
constexpr uint32_t kItemAmount = 0x260, kPickupSlot = 0x1F8;
// a gun or suit whose stream is not loaded yet has this long to come in
constexpr DWORD kGunTimeoutMs = 30000, kSuitCheckMs = 1500, kSuitTimeoutMs = 20000, kStatusMs = 5000, kRefreshMs = 400;

// --- state -----------------------------------------------------------------------
bool open = false;
DWORD openedAt = 0;
coop_shop::Cursor cursor;
constexpr int kMaxItems = 48;
Item items[kMaxItems];
int itemCount = 0;
DWORD rebuiltAt = 0;
bool dirty = true;
char status[96] = "";
uint8_t statusKind = coop_shop::kStatusInfo;
DWORD statusAt = 0;
coop_shop::Edge toggleEdge, escapeEdge;
panelinput::Reader input;
coop_shop::Swallow swallow;
uint32_t guardDown[coop_shop::kGuards]{};
coop_shop::View lastView;
bool published = false;
uint32_t lastSim = 0;
DWORD simStillSince = 0;
bool paused = false;
LONG visitBuys = 0;

using SurvivalFn = int(__cdecl*)(int*);
SurvivalFn survivalFn = nullptr;
DWORD survivalLookAt = 0;
bool survivalMissingLogged = false;
int survState = coop_shop::kSurvOff, survWave = 0, survSeenState = -1;
DWORD survStateSince = 0;
// The picked mode on an MP map visit (arenas always play survival). Objective and
// Zone have no breaks, so the terminal stays open while they run.
LONG visitMode = 0;
bool ClockModeOpen() { return visitMode == 2 || visitMode == 3; }

struct PendingGun { int idx = -1; uint32_t proxy = 0, player = 0, price = 0; DWORD startedAt = 0; };
PendingGun pendingGun;
struct PendingSuit { int idx = -1; uint32_t player = 0, price = 0; DWORD startedAt = 0; };
PendingSuit pendingSuit;

volatile LONG opens = 0, closes = 0, autoCloses = 0, refusedOpens = 0, pausedTicks = 0;
volatile LONG attempts = 0, bought = 0, failed = 0, tooPoor = 0, unavailable = 0, refunds = 0, refundedCredits = 0, spent = 0, fallbackCharges = 0;
volatile LONG toInventory = 0, toFloor = 0, partial = 0, gunsOrdered = 0, gunsArrived = 0, gunTimeouts = 0, nodesFilled = 0, suitChanges = 0, gadgetBuys = 0, gunsReplaced = 0;
volatile LONG guardRefused[coop_shop::kGuards]{};
volatile LONG tickFaults = 0, eventLines = 0, buyLines = 0, survivalChanges = 0;

void Line(const char* format, ...) {
    if (InterlockedIncrement(&eventLines) > 200) return;
    char buffer[300];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    gate::Event("shop: %s", buffer);
}
void SetStatus(uint8_t kind, const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vsnprintf(status, sizeof(status), format, args);
    va_end(args);
    statusKind = kind;
    statusAt = GetTickCount();
}
// RIG banner, spaced by the notice queue so quick purchases do not overwrite each other.
void Announce(const char* format, ...) {
    if (!notices || !notice::enabled) return;
    char text[96];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    for (char* c = text; *c; ++c) *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
    notice::Enqueue(text);
}

// --- the numpad belongs to the open shop -------------------------------------------------
// Another panel on the overlay (the debug menu) owns the numpad the same way while it is open.
volatile LONG foreignPanel = 0;
bool HoldsKey(int vk, coop_shop::Guard who) {
    const bool panel = InterlockedCompareExchange(&foreignPanel, 0, 0) != 0;
    if ((!installed && !panel) || !vk || (!coop_shop::IsNumpad(vk) && vk != key)) return false;
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool held = open || panel || swallow.Holds(vk, down);
    const int g = static_cast<int>(who);
    const uint32_t bit = coop_shop::IsNumpad(vk) ? 1u << (vk - 0x60) : 1u << 16;
    if (g >= 0 && g < coop_shop::kGuards) {
        if (held && down) {
            if (!(guardDown[g] & bit)) { guardDown[g] |= bit; InterlockedIncrement(&guardRefused[g]); }
        } else if (!down) {
            guardDown[g] &= ~bit;
        }
    }
    return held;
}

// --- money -----------------------------------------------------------------------------
int32_t Credits(uint32_t player) {
    int32_t c = -1;
    return player && Read(player + kCredits, &c, 4) && c >= 0 && c <= 99999999 ? c : -1;
}
// SubtractCurrency, the store's own; a direct write if it did not take.
bool Charge(uint32_t player, uint32_t amount, int32_t& after) {
    const int32_t before = Credits(player);
    after = before;
    if (!amount) return true;
    if (before < 0 || static_cast<uint32_t>(before) < amount) return false;
    const bool called = bot::SubtractRaw(player + bot::kGeneralInventory, amount);
    after = Credits(player);
    if (called && after == before - static_cast<int32_t>(amount)) return true;
    const int32_t want = before - static_cast<int32_t>(amount);
    InterlockedIncrement(&fallbackCharges);
    if (!WriteBytes(player + kCredits, &want, 4)) return false;
    after = Credits(player);
    Line("SubtractCurrency %s: credits written directly (%d -> %d)", called ? "left them unchanged" : "faulted", before, after);
    return after == want;
}
void Refund(uint32_t player, uint32_t amount, const char* why) {
    if (!amount) return;
    const int32_t before = Credits(player);
    if (before < 0) { Line("refund of %u for %s lost: no credits to write to (player %08X)", amount, why, player); return; }
    const int32_t after = before + static_cast<int32_t>(amount);
    const bool ok = WriteBytes(player + kCredits, &after, 4);
    // Not a credits find: the bots' share watch (bot::ShareCredits) moves on with it.
    if (ok && bot::leaderCreditsSeen >= 0) bot::leaderCreditsSeen += static_cast<int32_t>(amount);
    InterlockedIncrement(&refunds);
    InterlockedExchangeAdd(&refundedCredits, static_cast<LONG>(amount));
    Line("refund %u for %s: credits %d -> %d%s", amount, why, before, Credits(player), ok ? "" : " (WRITE FAILED)");
}

// --- the survival director ---------------------------------------------------------------
void PollSurvival(DWORD now, bool mpMap) {
    if (!survivalFn && now - survivalLookAt > 5000) {
        survivalLookAt = now;
        const HMODULE module = ds2coop::PartnerModule("DS2RandomizerObserver.asi");
        if (module) survivalFn = reinterpret_cast<SurvivalFn>(GetProcAddress(module, "DS2Survival_State"));
        if (survivalFn) Line("survival export DS2Survival_State found at %p", reinterpret_cast<void*>(survivalFn));
        else if (mpMap && !survivalMissingLogged) {
            survivalMissingLogged = true;
            Line("survival export missing (DS2RandomizerObserver.asi %s): every moment counts as between waves", module ? "has no DS2Survival_State" : "not loaded");
        }
    }
    // Asked everywhere: survival also runs on the Outbreak maps and in the campaign arenas.
    int wave = 0;
    int state = survivalFn ? survivalFn(&wave) : coop_shop::kSurvOff;
    // a client's own director never runs: the host's state arrives with the music
    // ((wave << 8) | state), so DuringWaves and the dead lock hold there too (13.10.)
    if (NetRole() == 2) {
        state = static_cast<int>(InterlockedCompareExchange(&survmusic::state, 0, 0));
        wave = static_cast<int>(InterlockedCompareExchange(&survmusic::wave, 0, 0));
    }
    if (state != survSeenState) {
        if (InterlockedIncrement(&survivalChanges) <= 60) Line("survival state %d -> %d (wave %d)", survSeenState, state, wave);
        survSeenState = state;
        survStateSince = now;
    }
    survState = state;
    survWave = wave;
}

// Once per break between waves (and before the first), 3 s into it: the bots
// buy guns, med packs, ammo and power nodes with their own credits.
bool botShopping = true;
DWORD botShopKey = 0;
volatile LONG botShops = 0;
// The terminal's key, once per arrival when the first round arms (13.10.: players who
// read nothing wondered why they only had the plasma cutter).
LONG terminalHintEpoch = -1;
int terminalHintState = -1;
volatile LONG terminalHints = 0;
void TerminalHint(bool terminalHere) {
    const int state = survState;
    const bool arming = state == coop_shop::kSurvArming && terminalHintState != coop_shop::kSurvArming;
    terminalHintState = state;
    const LONG epoch = static_cast<LONG>(mpmap::visit.epochs);
    if (!installed || !terminalHere || !arming || epoch == terminalHintEpoch) return;
    terminalHintEpoch = epoch;
    char keyName[32] = "";
    const UINT scan = MapVirtualKeyA(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
    if (!scan || !GetKeyNameTextA(static_cast<LONG>(scan << 16), keyName, sizeof(keyName))) std::snprintf(keyName, sizeof(keyName), "key %d", key);
    char title[64], text[64];
    std::snprintf(title, sizeof(title), "Supply terminal: press %s", keyName);
    std::snprintf(text, sizeof(text), "SUPPLY TERMINAL: PRESS %s", keyName);
    for (char* c = text; *c; ++c) *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
    notice::Enqueue(text);
    overlay::ShowInfo(title, "Guns, ammo, med packs, suits and upgrades for your credits. You start with only the plasma cutter.",
                      duringWaves || ClockModeOpen() ? "It opens any time during the round." : "It opens between the waves and before the first one.", 7500);
    InterlockedIncrement(&terminalHints);
    Line("terminal hint shown (key %d '%s', arrival %ld)", key, keyName, epoch);
}
void BotShopping(DWORD now, bool survivalMap) {
    if (!botShopping || !survivalMap || (survState != coop_shop::kSurvArming && survState != coop_shop::kSurvIntermission)) return;
    if (now - survStateSince < 3000 || botShopKey == survStateSince + 1) return;
    botShopKey = survStateSince + 1;
    const unsigned percent = escalation::PricePercent();
    const uint32_t nodePrice = static_cast<uint32_t>((static_cast<uint64_t>(priceNode) * percent + 50) / 100);
    const int nextWave = survState == coop_shop::kSurvArming ? 1 : survWave + 1;
    const int shopped = bot::SurvivalShop(nodePrice, coop_shop::kMedMedium, nextWave);
    if (shopped > 0) InterlockedIncrement(&botShops);
    Line("bots shopping before wave %d: %d bot(s)%s, node price %u", nextWave, shopped < 0 ? 0 : shopped, shopped < 0 ? " (the bots' store hooks are off)" : "",
         nodePrice);
}

// --- what is on offer ----------------------------------------------------------------------
Item* Add(Kind kind, int ref, const char* label, uint32_t price) {
    if (itemCount >= kMaxItems) return nullptr;
    Item& it = items[itemCount++];
    coop_shop::SetItem(it, kind, ref, label, price);
    return &it;
}
// The kinds of gun a Player carries, as catalogue indices, each once.
int CarriedKinds(uint32_t player, int* out, uint32_t* gunAt) {
    uint32_t guns[8];
    const unsigned n = bot::GunsOf(player, guns, 8);
    int count = 0;
    for (unsigned g = 0; g < n; ++g) {
        uint32_t vtable = 0;
        if (!Read(guns[g], &vtable, 4)) continue;
        const int idx = coop_shop::GunByVtableRva(vtable - static_cast<uint32_t>(base));
        bool seen = idx < 0;
        for (int k = 0; k < count; ++k) seen = seen || out[k] == idx;
        if (seen) continue;
        if (gunAt) gunAt[count] = guns[g];
        out[count++] = idx;
    }
    return count;
}
void BuildSupplies(uint32_t player) {
    int kinds[8];
    const int n = CarriedKinds(player, kinds, nullptr);
    char label[48];
    for (int k = 0; k < n; ++k) {
        if (!coop_shop::HasAmmo(coop_shop::kGuns[kinds[k]])) continue;   // the Hand Cannon
        std::snprintf(label, sizeof(label), "%s ammo", coop_shop::kGuns[kinds[k]].name);
        if (Item* it = Add(Kind::Ammo, kinds[k], label, priceAmmo[kinds[k]])) std::snprintf(it->detail, sizeof(it->detail), "One ammo pack for the %s", coop_shop::kGuns[kinds[k]].name);
    }
    if (Item* it = Add(Kind::MedSmall, -1, "Small med pack", priceMed[0])) std::snprintf(it->detail, sizeof(it->detail), "Into the RIG inventory, or at your feet when it is full");
    if (Item* it = Add(Kind::MedMedium, -1, "Medium med pack", priceMed[1])) std::snprintf(it->detail, sizeof(it->detail), "Into the RIG inventory, or at your feet when it is full");
    if (Item* it = Add(Kind::MedLarge, -1, "Large med pack", priceMed[2])) std::snprintf(it->detail, sizeof(it->detail), "Into the RIG inventory, or at your feet when it is full");
    if (Item* it = Add(Kind::StasisPack, -1, "Stasis pack", priceStasis)) std::snprintf(it->detail, sizeof(it->detail), "Refills the stasis module");
    if (Item* it = Add(Kind::SupplyDrop, -1, "Supply drop", priceDrop))
        std::snprintf(it->detail, sizeof(it->detail), "Ammo for %d gun%s, a medium med pack and a stasis pack, put down in front of you", n, n == 1 ? "" : "s");
    if (!deliveryOk)
        for (int i = 0; i < itemCount; ++i) coop_shop::Refuse(items[i], "OFFLINE");
}
bool StreamLoaded(uint32_t qm, int idx, bool& known, bool& room) {
    const uint32_t entry = bot::GunEntryRaw(qm, reinterpret_cast<const unsigned char*>(coop_shop::kGuns[idx].proxy));
    uint32_t handle = 0;
    unsigned char cap = 0, loaded = 0;
    known = entry != 0;
    room = Read(qm + bot::kQmMaxStreams, &cap, 1) && Read(qm + bot::kQmLoadedStreams, &loaded, 1) && loaded + 1u <= cap;
    return entry && Read(entry + bot::kGunEntryHandle, &handle, 4) && handle;
}
// Every gun at once: the Quartermaster's weapon stream budget (the campaign's
// 4, multiplayer's 7) goes up to the ten guns plus room, whatever it costs in
// memory, and a gun whose stream is not loaded is requested when bought.
constexpr unsigned char kAllGunStreams = 12;
volatile LONG gunStreamRaises = 0;
void RaiseGunStreams(uint32_t qm) {
    unsigned char cap = 0;
    if (!qm || !Read(qm + bot::kQmMaxStreams, &cap, 1) || cap >= kAllGunStreams) return;
    const unsigned char want = kAllGunStreams;
    if (WriteBytes(qm + bot::kQmMaxStreams, &want, 1) && InterlockedIncrement(&gunStreamRaises) <= 20)
        Line("weapon streams allowed %u -> %u (every gun can be ordered)", cap, want);
}
// The gun in his hands, when it is a catalogue gun of his own in his weapon
// slots that Gun::MoveFromInventoryToSafe (vtable +0x288, 0x00DE0EB0) can take:
// a safe copy into owner+0xC38, out of its slot, the hand cleared, the gun
// deleted (the bots' FreeSlotFor does the same).
uint32_t HeldGun(uint32_t player, int& kind) {
    kind = -1;
    uint32_t ref = 0, vtable = 0, owner = 0, fn = 0, guns[8];
    if (!Read(player + bot::kActiveItem, &ref, 4) || ref < 0x10010) return 0;
    const uint32_t gun = ref - 0x10;
    if (!Read(gun, &vtable, 4) || (kind = coop_shop::GunByVtableRva(vtable - static_cast<uint32_t>(base))) < 0) return 0;
    if (!Read(gun + bot::kGunOwner, &owner, 4) || owner < 0x10010 || owner - 0x10 != player) return 0;
    if (!Read(vtable + bot::kMoveToSafeSlot, &fn, 4) || fn != base + bot::kMoveToSafeRva) return 0;
    const unsigned n = bot::GunsOf(player, guns, 8);
    for (unsigned i = 0; i < n; ++i) if (guns[i] == gun) return gun;
    return 0;
}
void BuildWeapons(uint32_t player) {
    uint32_t qm = 0;
    Read(base + bot::kQuartermasterRva, &qm, 4);
    if (requestStreams) RaiseGunStreams(qm);
    int carried[8], proxies = 0;
    const int n = CarriedKinds(player, carried, nullptr);
    bot::CarriedGuns(player, nullptr, &proxies);
    const bool full = gunsOk && bot::ThisBool0(base + bot::kInventoryIsFullRva, player + bot::kWeaponInventory) == 1;
    int heldKind = -1;
    const uint32_t held = full && replaceHeld ? HeldGun(player, heldKind) : 0;
    for (int i = 0; i < coop_shop::kGunCount; ++i) {
        Item* it = Add(Kind::Gun, i, coop_shop::kGuns[i].name, priceGun[i]);
        if (!it) break;
        bool owned = false;
        for (int k = 0; k < n; ++k) owned = owned || carried[k] == i;
        bool known = false, room = false;
        const bool loaded = gunsOk && qm && StreamLoaded(qm, i, known, room);
        std::snprintf(it->detail, sizeof(it->detail), "%s", loaded ? "Issued at once: its stream is loaded" : "Its weapon stream is not loaded on this map");
        // [Unlocks]: the profile level this gun is sold from
        const unsigned need = progress::UnlockLevel(coop_shop::kGuns[i].key);
        const bool levelLocked = !progress::Unlocked(coop_shop::kGuns[i].key);
        if (i == coop_shop::kHandCannon) std::snprintf(it->detail, sizeof(it->detail), "The Foam Finger: needs no ammo. Prestige reward of profile level %u", need);
        else if (levelLocked) std::snprintf(it->detail, sizeof(it->detail), "Unlocks at profile level %u (you are level %u)", need, progress::ProfileLevel());
        char levelText[20];
        std::snprintf(levelText, sizeof(levelText), "LEVEL %u", need);
        if (levelLocked) coop_shop::Refuse(*it, levelText);
        else if (!gunsOk || !qm) coop_shop::Refuse(*it, "OFFLINE");
        else if (owned) coop_shop::Refuse(*it, "OWNED");
        else if (pendingGun.idx >= 0 || proxies) coop_shop::Refuse(*it, "BUSY");
        else if (!known) coop_shop::Refuse(*it, "UNKNOWN");
        else if (!loaded && !requestStreams) coop_shop::Refuse(*it, "NOT LOADED");
        else if (!loaded && !room) coop_shop::Refuse(*it, "NO STREAM ROOM");
        else if (full && !held) coop_shop::Refuse(*it, replaceHeld ? "HOLD A GUN" : "SLOTS FULL");
        if (!loaded && requestStreams && it->available) std::snprintf(it->detail, sizeof(it->detail), "Its stream loads first; refunded if it is not here in 30 s");
        if (full && held && it->available)
            std::snprintf(it->detail, sizeof(it->detail), "Slots full: replaces the %s in your hands%s", coop_shop::kGuns[heldKind].name, loaded ? "" : " (stream loads first)");
        else if (full && !held && replaceHeld && it->reason[0] && std::strcmp(it->reason, "HOLD A GUN") == 0)
            std::snprintf(it->detail, sizeof(it->detail), "Slots full: hold the gun it should replace");
    }
}
// Player::CalcUpgradedValues reads the RIG and stasis trees, or MP_Human for both
// when bit 13 of the body's flags marks a multiplayer body.
bool MpBody(uint32_t player) {
    uint32_t flags = 0;
    return Read(player + kPlayerFlagWord, &flags, 4) && (flags & kMpBodyBit);
}
uint32_t RigTreeId(uint32_t player) { return MpBody(player) ? coop_shop::kMpHumanTree : coop_shop::kRigTree; }
// The tree an upgrade item works on, and for a gun the carried gun of that kind.
uint32_t TreeOf(Kind kind, int ref, uint32_t player) {
    if (kind == Kind::UpgradeRig) return bot::TreeFor(RigTreeId(player));
    if (kind == Kind::UpgradeStasis) return bot::TreeFor(MpBody(player) ? coop_shop::kMpHumanTree : coop_shop::kStasisTree);
    int kinds[8];
    uint32_t gunAt[8]{};
    const int n = CarriedKinds(player, kinds, gunAt);
    for (int k = 0; k < n; ++k)
        if (kinds[k] == ref) return bot::TreeFor(bot::GunUpgradeId(gunAt[k]));
    return 0;
}
uint32_t NodePrice(Kind kind) { return kind == Kind::UpgradeRig ? priceRigNode : kind == Kind::UpgradeStasis ? priceStasisNode : priceNode; }
// One row per stat the tree still has; a purchase goes the shortest way to the
// next node of that stat. Trees belong to the gun kind, not to one gun: what is
// filled stays when the gun is sold and bought again.
coop_shop::Plan PlanFor(const Item& it, const coop_bot::BenchNode* nodes, int count) {
    return it.sub > 0 ? coop_shop::PlanToType(nodes, count, it.sub) : coop_shop::PlanUpgrade(nodes, count);
}
void AddUpgrades(Kind kind, int ref, const char* name, uint32_t player) {
    const uint32_t tree = upgradesOk ? TreeOf(kind, ref, player) : 0;
    coop_bot::BenchNode nodes[coop_bot::kMaxBenchNodes];
    uint32_t where[coop_bot::kMaxBenchNodes]{};
    const int count = tree ? bot::ReadTree(tree, nodes, where) : -1;
    char label[48];
    std::snprintf(label, sizeof(label), "%s upgrades", name);
    if (!upgradesOk) { if (Item* it = Add(kind, ref, label, 0)) coop_shop::Refuse(*it, "OFFLINE"); return; }
    if (!tree || count <= 0) {
        Item* it = Add(kind, ref, label, 0);
        if (!it) return;
        coop_shop::Refuse(*it, "NO TREE");
        std::snprintf(it->detail, sizeof(it->detail), kind != Kind::UpgradeGun && MpBody(player) ? "A multiplayer body upgrades through MP_Human only" : "No upgrade tree found");
        return;
    }
    int types[16];
    const int typeCount = coop_shop::TypesLeft(nodes, count, types, 16);
    int listed = 0;
    for (int t = 0; t < typeCount; ++t) {
        const coop_shop::Plan plan = coop_shop::PlanToType(nodes, count, types[t]);
        if (!plan.nodes) continue;
        std::snprintf(label, sizeof(label), "%s: %s", name, coop_shop::NodeTypeName(types[t]));
        Item* it = Add(kind, ref, label, NodePrice(kind) * static_cast<uint32_t>(plan.nodes));
        if (!it) return;
        it->sub = types[t];
        const int left = coop_shop::EmptyOfType(nodes, count, types[t]);
        std::snprintf(it->detail, sizeof(it->detail), "%d bench node%s; %d %s node%s left", plan.nodes, plan.nodes == 1 ? "" : "s", left,
                      coop_shop::NodeTypeName(types[t]), left == 1 ? "" : "s");
        ++listed;
    }
    if (listed) return;
    std::snprintf(label, sizeof(label), "%s upgrades", name);
    if (Item* it = Add(kind, ref, label, 0)) {
        coop_shop::Refuse(*it, "MAXED");
        std::snprintf(it->detail, sizeof(it->detail), typeCount ? "The stat nodes left lie out of reach" : "Every stat node of this tree is filled");
    }
}
void BuildUpgrades(uint32_t player) {
    int kinds[8];
    const int n = CarriedKinds(player, kinds, nullptr);
    for (int k = 0; k < n; ++k) AddUpgrades(Kind::UpgradeGun, kinds[k], coop_shop::kGuns[kinds[k]].name, player);
    AddUpgrades(Kind::UpgradeRig, -1, "RIG", player);
    AddUpgrades(Kind::UpgradeStasis, -1, "Stasis", player);
}
// The UniformManager's list (as coop_uniform reads it): registered, and loaded with a stream.
bool SuitHere(const uint32_t* guid, bool& registered) {
    registered = false;
    uint32_t manager = 0, vtable = 0, list[3]{};
    if (!Read(base + kUniformManagerRva, &manager, 4) || !manager || !Read(manager, &vtable, 4) || vtable != base + kUniformManagerVtableRva ||
        !Read(manager + 0x10, list, sizeof(list)) || !list[0] || list[1] > list[2] || list[2] > 128)
        return false;
    for (uint32_t i = 0; i < list[1]; ++i) {
        unsigned char entry[24]{};
        if (!Read(list[0] + i * 24, entry, sizeof(entry)) || std::memcmp(entry + 4, guid, 16) != 0) continue;
        uint32_t stream = 0;
        std::memcpy(&stream, entry + 20, 4);
        registered = true;
        return entry[0] == 1 && stream;
    }
    return false;
}
void BuildSuits(uint32_t player) {
    uint32_t worn[4]{};
    const bool wearing = bot::WornSuit(player, worn);
    for (int i = 0; i < bot::kSuitKinds; ++i) {
        bool registered = false;
        const bool loaded = SuitHere(bot::kSuits[i].guid, registered);
        if (!registered) continue;   // every suit the UniformManager knows; ChangeToUniform streams one in
        Item* it = Add(Kind::Suit, i, bot::kSuits[i].name, priceSuitOf[i] ? priceSuitOf[i] : priceSuit);
        if (!it) break;
        std::snprintf(it->detail, sizeof(it->detail), "%s", loaded ? "Loaded here: the change is immediate" : "Its stream loads first: a few seconds");
        // [Unlocks]: the suit select's level for this suit
        const unsigned need = progress::SuitUnlockLevel(bot::kSuits[i].guid);
        const bool levelLocked = !progress::TestAll() && progress::ProfileLevel() < need;
        char levelText[20];
        std::snprintf(levelText, sizeof(levelText), "LEVEL %u", need);
        if (levelLocked) std::snprintf(it->detail, sizeof(it->detail), "Unlocks at profile level %u (you are level %u)", need, progress::ProfileLevel());
        if (levelLocked) coop_shop::Refuse(*it, levelText);
        else if (!suitsOk) coop_shop::Refuse(*it, "OFFLINE");
        else if (wearing && std::memcmp(worn, bot::kSuits[i].guid, 16) == 0) coop_shop::Refuse(*it, "WORN");
        else if (pendingSuit.idx >= 0) coop_shop::Refuse(*it, "BUSY");
        else if (!wearing) coop_shop::Refuse(*it, "CHANGING");
    }
}
void BuildGadgets() {
    if (Item* it = Add(Kind::TurretRefill, -1, "Turret: refill and cool", priceTurret)) {
        if (!turret::installed) coop_shop::Refuse(*it, "OFF");
        else if (!progress::Unlocked("Turret")) coop_shop::Refuse(*it, "LOCKED");
        else if (turret::mag.Unlimited(turret::cfg)) coop_shop::Refuse(*it, "UNLIMITED");
        else if (turret::mag.Rounds(turret::cfg) >= turret::cfg.ammo && !turret::mag.overheated) coop_shop::Refuse(*it, "FULL");
        std::snprintf(it->detail, sizeof(it->detail), "%d of %d rounds%s", turret::mag.Rounds(turret::cfg), turret::cfg.ammo, turret::mag.overheated ? ", overheated" : "");
    }
    const struct { Kind kind; const char* label; bool ready; const coop_gadgets::Charge* charge; uint32_t price; const char* key; } kGadgets[] = {
        {Kind::MineRecharge, "Stasis mine: recharge now", gadgets::installed && gadgets::mineWanted && gadgets::stasisReady, &gadgets::mineCharge, priceMine, "StasisMine"},
        {Kind::FieldRecharge, "Heal field: recharge now", gadgets::installed && gadgets::fieldWanted && gadgets::healReady, &gadgets::fieldCharge, priceField, "HealField"},
    };
    for (const auto& g : kGadgets) {
        Item* it = Add(g.kind, -1, g.label, g.price);
        if (!it) break;
        std::snprintf(it->detail, sizeof(it->detail), "Charge %s", coop_gadgets::ChargeName(g.charge->state));
        if (!g.ready) coop_shop::Refuse(*it, "OFF");
        else if (!progress::Unlocked(g.key)) coop_shop::Refuse(*it, "LOCKED");
        else if (g.charge->state == coop_gadgets::ChargeState::Ready) coop_shop::Refuse(*it, "READY");
        else if (g.charge->state == coop_gadgets::ChargeState::Out) coop_shop::Refuse(*it, "IN USE");
        else std::snprintf(it->detail, sizeof(it->detail), "Ready again in %d s without it", g.charge->SecondsLeft());
    }
}
void Rebuild(uint32_t player) {
    itemCount = 0;
    if (player) {
        switch (static_cast<coop_shop::Tab>(cursor.tab)) {
        case coop_shop::Tab::Supplies: BuildSupplies(player); break;
        case coop_shop::Tab::Weapons: BuildWeapons(player); break;
        case coop_shop::Tab::Upgrades: BuildUpgrades(player); break;
        case coop_shop::Tab::Suits: BuildSuits(player); break;
        case coop_shop::Tab::Gadgets: BuildGadgets(); break;
        }
    }
    // the survival escalation's prices
    if (const unsigned percent = escalation::PricePercent(); percent != 100)
        for (int i = 0; i < itemCount; ++i) items[i].price = static_cast<uint32_t>((static_cast<uint64_t>(items[i].price) * percent + 50) / 100);
    coop_shop::Fit(cursor, itemCount, rowsWanted);
    rebuiltAt = GetTickCount();
    dirty = false;
}

// --- delivery -------------------------------------------------------------------------------
// Put an unowned pickup down in front of the player; deleted when that fails.
bool DropNear(uint32_t player, uint32_t item, unsigned slot) {
    uint32_t vtable = 0, dropFn = 0, owner = 1;
    turret::V3 at, facing{0.0f, 0.0f, 1.0f};
    if (!Read(item, &vtable, 4) || !Read(vtable + turret::kItemDropSlot, &dropFn, 4) || dropFn != base + turret::kItemDropRva ||
        !Read(item + turret::kItemOwner, &owner, 4) || owner || !turret::Pos(player, at)) {
        turret::DeleteRaw(item);
        return false;
    }
    turret::Facing(player, facing);
    float fx = facing.x, fz = facing.z;
    const float len = std::sqrt(fx * fx + fz * fz);
    if (!(len > 0.01f)) { fx = 0.0f; fz = 1.0f; } else { fx /= len; fz /= len; }
    const float side = (static_cast<float>(slot % 5) - 2.0f) * 0.45f;
    alignas(16) float pos[4] = {at.x + fx * 1.3f - fz * side, at.y + 0.3f, at.z + fz * 1.3f + fx * side, 1.0f};
    if (!turret::DropRaw(item, pos)) { turret::DeleteRaw(item); return false; }
    return true;
}
enum class Got { Inventory, Floor, Partial, Failed };
const char* GotName(Got g) {
    switch (g) {
    case Got::Inventory: return "into the inventory";
    case Got::Floor: return "put down in front of him";
    case Got::Partial: return "partly into the inventory";
    default: return "FAILED";
    }
}
// One pickup the store's way; percent is how much of it he got.
Got Deliver(uint32_t player, const uint32_t* guid, unsigned slot, unsigned& percent, char* why, size_t whySize) {
    percent = 100;
    why[0] = 0;
    const unsigned char* g = reinterpret_cast<const unsigned char*>(guid);
    const int fits = bot::CanFitRaw(g, player + bot::kGeneralInventory);
    const uint32_t item = turret::SpawnRaw(guid);
    if (!item) { std::snprintf(why, whySize, "GetGlobalPickupItem gave nothing for %08X-%08X", guid[0], guid[1]); return Got::Failed; }
    uint32_t vtable = 0, fn = 0, amount = 0;
    if (!Read(item, &vtable, 4) || vtable < base + gate::kRdataLo || vtable >= base + gate::kRdataHi || !Read(vtable + kPickupSlot, &fn, 4) || fn < base + 0x1000 ||
        fn >= base + gate::kRdataLo) {
        turret::DeleteRaw(item);
        std::snprintf(why, whySize, "%08X is no pickup (vtable %08X)", item, vtable);
        return Got::Failed;
    }
    Read(item + kItemAmount, &amount, 4);
    if (fits == 1 && intoInventory) {
        const int whole = bot::PickupRaw(item, fn, player);
        if (whole == 1) return Got::Inventory;
        if (whole < 0) { std::snprintf(why, whySize, "Pickup faulted (item %08X left alone)", item); return Got::Failed; }
        uint32_t left = amount;
        Read(item + kItemAmount, &left, 4);
        if (left < amount && amount) {
            // Some went in; the rest goes down in front of him, or is lost and not charged.
            if (DropNear(player, item, slot)) return Got::Floor;
            percent = (amount - left) * 100 / amount;
            std::snprintf(why, whySize, "%u of %u fit, the rest could not be put down", amount - left, amount);
            return Got::Partial;
        }
    }
    if (DropNear(player, item, slot)) {
        std::snprintf(why, whySize, "%s", !intoInventory ? "Inventory=0" : fits == 0 ? "no room in the inventory" : fits < 0 ? "CanFitItem faulted" : "Pickup took nothing");
        return Got::Floor;
    }
    std::snprintf(why, whySize, "no room and Item::Drop failed");
    return Got::Failed;
}

// --- purchases ------------------------------------------------------------------------------
void LogBuy(const Item& it, uint32_t price, int32_t before, int32_t after, const char* result) {
    const LONG n = InterlockedIncrement(&buyLines);
    if (n <= 500)
        gate::Event("shop buy #%ld: %s '%s' (ref %d) price %u, credits %d -> %d: %s", n, coop_shop::KindName(it.kind), it.label, it.ref, price, before, after, result);
}
void Paid(uint32_t amount) {
    InterlockedIncrement(&bought);
    InterlockedExchangeAdd(&spent, static_cast<LONG>(amount));
    ++visitBuys;
}
void BuySupply(const Item& it, uint32_t player, int32_t before) {
    const uint32_t* guid = it.kind == Kind::Ammo ? coop_shop::kGuns[it.ref].ammo : it.kind == Kind::MedSmall ? coop_shop::kMedSmall
                         : it.kind == Kind::MedMedium ? coop_shop::kMedMedium : it.kind == Kind::MedLarge ? coop_shop::kMedLarge : coop_shop::kStasisPack;
    unsigned percent = 100;
    char why[120];
    const Got got = Deliver(player, guid, 0, percent, why, sizeof(why));
    int32_t after = before;
    char result[200];
    if (got == Got::Failed) {
        InterlockedIncrement(&failed);
        std::snprintf(result, sizeof(result), "not delivered (%s), nothing charged", why);
        SetStatus(coop_shop::kStatusBad, "%s could not be delivered", it.label);
    } else {
        const uint32_t price = got == Got::Partial ? it.price * percent / 100 : it.price;
        const bool charged = Charge(player, price, after);
        InterlockedIncrement(got == Got::Inventory ? &toInventory : got == Got::Floor ? &toFloor : &partial);
        Paid(price);
        std::snprintf(result, sizeof(result), "%s%s%s, charged %u%s", GotName(got), why[0] ? ": " : "", why, price, charged ? "" : " (CHARGE FAILED)");
        SetStatus(coop_shop::kStatusGood, got == Got::Floor ? "%s bought: it lies in front of you" : "%s bought", it.label);
        Announce("%s acquired", it.label);
    }
    LogBuy(it, it.price, before, after, result);
}
void BuySupplyDrop(const Item& it, uint32_t player, int32_t before) {
    const uint32_t* parts[10];
    const char* names[10];
    int n = 0;
    int kinds[8];
    const int guns = CarriedKinds(player, kinds, nullptr);
    for (int k = 0; k < guns && n < 8; ++k)
        if (coop_shop::HasAmmo(coop_shop::kGuns[kinds[k]])) { parts[n] = coop_shop::kGuns[kinds[k]].ammo; names[n++] = coop_shop::kGuns[kinds[k]].name; }
    parts[n] = coop_shop::kMedMedium; names[n++] = "medium med pack";
    parts[n] = coop_shop::kStasisPack; names[n++] = "stasis pack";
    int ok = 0;
    char lost[160] = "";
    size_t used = 0;
    for (int k = 0; k < n; ++k) {
        const uint32_t item = turret::SpawnRaw(parts[k]);
        if (item && DropNear(player, item, static_cast<unsigned>(k))) { ++ok; continue; }
        if (used < sizeof(lost)) used += std::snprintf(lost + used, sizeof(lost) - used, "%s%s", used ? ", " : "", names[k]);
    }
    int32_t after = before;
    char result[240];
    if (!ok) {
        InterlockedIncrement(&failed);
        std::snprintf(result, sizeof(result), "nothing could be put down (%s), nothing charged", lost);
        SetStatus(coop_shop::kStatusBad, "The supply drop failed");
    } else {
        const uint32_t price = it.price * static_cast<uint32_t>(ok) / static_cast<uint32_t>(n);
        const bool charged = Charge(player, price, after);
        InterlockedIncrement(&toFloor);
        Paid(price);
        std::snprintf(result, sizeof(result), "%d of %d parts put down%s%s, charged %u%s", ok, n, lost[0] ? ", missing: " : "", lost, price, charged ? "" : " (CHARGE FAILED)");
        SetStatus(coop_shop::kStatusGood, "Supply drop in front of you (%d of %d)", ok, n);
        Announce("Supply drop delivered");
    }
    LogBuy(it, it.price, before, after, result);
}
void BuyGun(const Item& it, uint32_t player, int32_t before) {
    const int idx = it.ref;
    const coop_shop::GunEntry& gun = coop_shop::kGuns[idx];
    uint32_t qm = 0;
    char result[200];
    int32_t after = before;
    const uint32_t weapons = player + bot::kWeaponInventory;
    if (Read(base + bot::kQuartermasterRva, &qm, 4) && qm && requestStreams) RaiseGunStreams(qm);
    const bool full = qm && bot::ThisBool0(base + bot::kInventoryIsFullRva, weapons) != 0;
    int heldKind = -1;
    const uint32_t held = full && replaceHeld ? HeldGun(player, heldKind) : 0;
    if (!qm || (full && !held)) {
        InterlockedIncrement(&failed);
        SetStatus(coop_shop::kStatusBad, !qm ? "No quartermaster here" : replaceHeld ? "Slots full: hold the gun to replace" : "No free weapon slot");
        LogBuy(it, it.price, before, after, !qm ? "no quartermaster" : replaceHeld ? "no free weapon slot and no own gun in hand to replace" : "no free weapon slot");
        return;
    }
    if (!Charge(player, it.price, after)) {
        InterlockedIncrement(&failed);
        SetStatus(coop_shop::kStatusBad, "The payment did not go through");
        LogBuy(it, it.price, before, after, "the charge failed, nothing ordered");
        return;
    }
    const uint32_t proxy = turret::SpawnRaw(gun.proxy);
    uint32_t vtable = 0;
    const char* fail = nullptr;
    const bool proxyOk = proxy && Read(proxy, &vtable, 4) && vtable == base + bot::kGunProxyVtableRva;
    // Paid and the placeholder is there: only now the gun in hand makes room.
    bool movedToSafe = false;
    if (proxyOk && held) {
        movedToSafe = bot::ThisVoid0(base + bot::kMoveToSafeRva, held);
        if (movedToSafe) InterlockedIncrement(&gunsReplaced);
    }
    if (!proxyOk) {
        if (proxy) turret::DeleteRaw(proxy);
        fail = "no GunProxy placeholder spawned";
    } else if (held && !movedToSafe) {
        turret::DeleteRaw(proxy);
        fail = "moving the gun in hand to the safe faulted";
    } else if (held && bot::ThisBool0(base + bot::kInventoryIsFullRva, weapons) != 0) {
        turret::DeleteRaw(proxy);
        fail = "the gun in hand went to the safe, the slots stayed full";
    } else if (bot::PickupRaw(proxy, base + bot::kItemPickupRva, player) != 1) {
        turret::DeleteRaw(proxy);
        fail = "the placeholder did not go into a slot";
    } else {
        const int ok = bot::RequisitionRaw(qm, player, gun.proxy);
        if (ok != 1) {
            bot::RemoveItemRaw(weapons, proxy);
            turret::DeleteRaw(proxy);
            fail = ok < 0 ? "RequisitionGun faulted" : "RequisitionGun refused (stream budget)";
        }
    }
    const char* heldName = heldKind >= 0 ? coop_shop::kGuns[heldKind].name : "?";
    if (fail) {
        InterlockedIncrement(&failed);
        Refund(player, it.price, gun.name);
        std::snprintf(result, sizeof(result), "%s%s%s, refunded", fail, movedToSafe ? "; the gun in hand is in the safe now: " : "", movedToSafe ? heldName : "");
        SetStatus(coop_shop::kStatusBad, "%s could not be issued", gun.name);
        LogBuy(it, it.price, before, Credits(player), result);
        return;
    }
    pendingGun = PendingGun{idx, proxy, player, it.price, GetTickCount()};
    InterlockedIncrement(&gunsOrdered);
    Paid(it.price);
    std::snprintf(result, sizeof(result), "placeholder %08X in a slot, gun requisitioned; waiting for it%s%s", proxy, movedToSafe ? "; replaced the " : "",
                  movedToSafe ? heldName : "");
    if (movedToSafe) SetStatus(coop_shop::kStatusGood, "%s ordered, the %s went to the safe", gun.name, heldName);
    else SetStatus(coop_shop::kStatusGood, "%s ordered", gun.name);
    LogBuy(it, it.price, before, after, result);
}
__declspec(noinline) bool PlayerRecalcRaw(uint32_t player) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, bool)>(base + kPlayerCalcRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(player)), true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
float MaxHealth(uint32_t player) {
    float max = -1.0f;
    const uint32_t c = hits::FindHealthComponent(player);
    return c && Read(c + hits::kHealthMax, &max, 4) && std::isfinite(max) ? max : -1.0f;
}
void BuyUpgrade(const Item& it, uint32_t player, int32_t before) {
    const uint32_t tree = TreeOf(it.kind, it.ref, player);
    coop_bot::BenchNode nodes[coop_bot::kMaxBenchNodes];
    uint32_t where[coop_bot::kMaxBenchNodes]{};
    const int count = tree ? bot::ReadTree(tree, nodes, where) : -1;
    const coop_shop::Plan plan = count > 0 ? PlanFor(it, nodes, count) : coop_shop::Plan{};
    int32_t after = before;
    char result[240];
    // The row was built a moment ago from the same tree: its price (with the escalation's percent) is the plan's.
    const uint32_t price = plan.nodes ? it.price : 0;
    if (!plan.nodes || before < 0 || static_cast<uint32_t>(before) < price) {
        InterlockedIncrement(&failed);
        std::snprintf(result, sizeof(result), "%s (tree %08X, %d nodes read)", !plan.nodes ? "nothing left to fill" : "too poor for the current plan", tree, count);
        SetStatus(coop_shop::kStatusBad, !plan.nodes ? "Nothing left to upgrade" : "Not enough credits");
        LogBuy(it, price, before, after, result);
        return;
    }
    const float healthBefore = it.kind == Kind::UpgradeRig ? MaxHealth(player) : -1.0f;
    int filled = 0;
    char keys[96] = "";
    size_t used = 0;
    for (int k = 0; k < plan.nodes; ++k) {
        const uint32_t one = 1;
        const int n = plan.order[k];
        if (!WriteBytes(where[n] + bot::kNodeFill, &one, 4)) break;
        ++filled;
        if (used < sizeof(keys)) used += std::snprintf(keys + used, sizeof(keys) - used, "%s%08X/%s", k ? " " : "", nodes[n].key, coop_shop::NodeTypeName(nodes[n].type));
    }
    if (!filled) {
        InterlockedIncrement(&failed);
        SetStatus(coop_shop::kStatusBad, "The upgrade could not be written");
        LogBuy(it, price, before, after, "the node write failed, nothing charged");
        return;
    }
    InterlockedExchangeAdd(&nodesFilled, filled);
    // Trees are global per item kind: every gun of that kind gains, the bots' too.
    bool recalced = true;
    if (it.kind == Kind::UpgradeGun) {
        const LONG before0 = InterlockedCompareExchange(&bot::gunRecalcs, 0, 0);
        bot::RecalcGuns(player, &tree, 1);
        for (unsigned i = 0; i < bot::wantedBots && i < kMaxBots; ++i)
            if (bot::units[i].entity) bot::RecalcGuns(bot::units[i].entity, &tree, 1);
        recalced = InterlockedCompareExchange(&bot::gunRecalcs, 0, 0) > before0;
    } else {
        recalced = PlayerRecalcRaw(player);
    }
    const uint32_t charged = price * static_cast<uint32_t>(filled) / static_cast<uint32_t>(plan.nodes);
    const bool paid = Charge(player, charged, after);
    Paid(charged);
    const char* stat = coop_shop::NodeTypeName(plan.type);
    std::snprintf(result, sizeof(result), "%d of %d node(s) filled in tree %08X [%s], %s%s, charged %u%s", filled, plan.nodes, tree, keys,
                  it.kind == Kind::UpgradeGun ? "guns recalculated" : "Player::CalcUpgradedValues", recalced ? "" : " FAILED", charged, paid ? "" : " (CHARGE FAILED)");
    if (it.kind == Kind::UpgradeRig) {
        const size_t len = std::strlen(result);
        std::snprintf(result + len, sizeof(result) - len, ", max health %.0f -> %.0f", healthBefore, MaxHealth(player));
    }
    char name[48];
    std::snprintf(name, sizeof(name), "%s", it.label);
    if (char* colon = std::strchr(name, ':')) *colon = 0;
    SetStatus(coop_shop::kStatusGood, "%s: %s upgraded", name, stat);
    Announce("%s: %s upgraded", name, stat);
    LogBuy(it, price, before, after, result);
}
void BuySuit(const Item& it, uint32_t player, int32_t before) {
    const auto& suit = bot::kSuits[it.ref];
    bool registered = false;
    int32_t after = before;
    const bool loaded = SuitHere(suit.guid, registered);
    if (pendingSuit.idx >= 0 || !registered) {
        InterlockedIncrement(&failed);
        SetStatus(coop_shop::kStatusBad, "%s is not available now", suit.name);
        LogBuy(it, it.price, before, after, pendingSuit.idx >= 0 ? "a suit change is under way" : "not registered");
        return;
    }
    if (!loaded) Line("suit %s: its stream is not loaded; ChangeToUniform streams it in", suit.name);
    if (!Charge(player, it.price, after)) {
        InterlockedIncrement(&failed);
        SetStatus(coop_shop::kStatusBad, "The payment did not go through");
        LogBuy(it, it.price, before, after, "the charge failed");
        return;
    }
    if (!::coop_uniform::BotDress(player, suit.guid)) {
        InterlockedIncrement(&failed);
        Refund(player, it.price, suit.name);
        SetStatus(coop_shop::kStatusBad, "%s could not be put on", suit.name);
        LogBuy(it, it.price, before, Credits(player), "ChangeToUniform faulted, refunded");
        return;
    }
    pendingSuit = PendingSuit{it.ref, player, it.price, GetTickCount()};
    Paid(it.price);
    SetStatus(coop_shop::kStatusGood, "Changing into the %s", suit.name);
    LogBuy(it, it.price, before, after, "ChangeToUniform called; checking the body");
}
void BuyGadget(const Item& it, uint32_t player, int32_t before) {
    int32_t after = before;
    const char* what = "";
    switch (it.kind) {
    case Kind::TurretRefill: turret::mag.Refill(turret::cfg); what = "turret magazine refilled and cooled"; break;
    case Kind::MineRecharge: gadgets::mineCharge.state = coop_gadgets::ChargeState::Ready; gadgets::mineCharge.recharge.left = 0; what = "stasis mine ready"; break;
    case Kind::FieldRecharge: gadgets::fieldCharge.state = coop_gadgets::ChargeState::Ready; gadgets::fieldCharge.recharge.left = 0; what = "heal field ready"; break;
    default: return;
    }
    const bool charged = Charge(player, it.price, after);
    InterlockedIncrement(&gadgetBuys);
    Paid(it.price);
    char result[120];
    std::snprintf(result, sizeof(result), "%s, charged %u%s", what, it.price, charged ? "" : " (CHARGE FAILED)");
    SetStatus(coop_shop::kStatusGood, "%s", it.kind == Kind::TurretRefill ? "Turret refilled" : it.kind == Kind::MineRecharge ? "Stasis mine ready" : "Heal field ready");
    Announce("%s", what);
    LogBuy(it, it.price, before, after, result);
}
void Buy(uint32_t player) {
    // The list as it is now: a purchase on a stale row would buy something else.
    const int tab = cursor.tab;
    const int row = cursor.row[tab];
    const Kind kind = row < itemCount ? items[row].kind : Kind::Ammo;
    const int ref = row < itemCount ? items[row].ref : -1, sub = row < itemCount ? items[row].sub : 0;
    Rebuild(player);
    InterlockedIncrement(&attempts);
    if (row >= itemCount || items[row].kind != kind || items[row].ref != ref || items[row].sub != sub) {
        SetStatus(coop_shop::kStatusInfo, "The list changed, check and try again");
        Line("purchase skipped: the row changed under the cursor");
        return;
    }
    const Item it = items[row];
    const int32_t before = Credits(player);
    switch (coop_shop::Check(it, before)) {
    case coop_shop::Verdict::Unavailable:
        InterlockedIncrement(&unavailable);
        SetStatus(coop_shop::kStatusBad, "%s: %s", it.label, it.reason);
        LogBuy(it, it.price, before, before, it.reason);
        return;
    case coop_shop::Verdict::TooPoor:
        InterlockedIncrement(&tooPoor);
        SetStatus(coop_shop::kStatusBad, "Not enough credits");
        LogBuy(it, it.price, before, before, "not enough credits");
        return;
    case coop_shop::Verdict::Ok: break;
    }
    switch (it.kind) {
    case Kind::Ammo: case Kind::MedSmall: case Kind::MedMedium: case Kind::MedLarge: case Kind::StasisPack: BuySupply(it, player, before); break;
    case Kind::SupplyDrop: BuySupplyDrop(it, player, before); break;
    case Kind::Gun: BuyGun(it, player, before); break;
    case Kind::UpgradeGun: case Kind::UpgradeRig: case Kind::UpgradeStasis: BuyUpgrade(it, player, before); break;
    case Kind::Suit: BuySuit(it, player, before); break;
    case Kind::TurretRefill: case Kind::MineRecharge: case Kind::FieldRecharge: BuyGadget(it, player, before); break;
    }
    dirty = true;
}

// Guns and suits on their way. An order placed in this very tick has a start a few ms after
// the tick's `now`; the plain difference wrapped to 49 days and every first gun "never came in
// 30 s" at once, refunded, while its stream went on loading (user, 17.10.: every gun had to be
// bought twice; shop log 16612/16613, the second order arrived after 0 ms).
DWORD Age(DWORD now, DWORD at) { return static_cast<LONG>(now - at) > 0 ? now - at : 0; }
void PollPending(DWORD now) {
    if (pendingGun.idx >= 0) {
        const PendingGun p = pendingGun;
        const DWORD age = Age(now, p.startedAt);
        const coop_shop::GunEntry& gun = coop_shop::kGuns[p.idx];
        const uint32_t player = LocalCampaignPlayer();
        bool waiting = false, arrived = false;
        uint32_t entries = 0, size = 0;
        if (player == p.player && Read(player + bot::kWeaponInventory + bot::kInventoryEntries, &entries, 4) && entries &&
            Read(player + bot::kWeaponInventory + bot::kInventorySize, &size, 4)) {
            for (uint32_t i = 0; i < size && i < 8; ++i) {
                uint32_t ref = 0, vtable = 0;
                if (!Read(entries + i * bot::kInventoryEntry + 4, &ref, 4) || ref < 0x10010) continue;
                if (ref - 0x10 == p.proxy) waiting = true;
                else if (Read(ref - 0x10, &vtable, 4) && vtable == base + gun.vtableRva) arrived = true;
            }
        }
        if (player != p.player) {
            pendingGun = PendingGun{};
            Line("gun %s: the body changed before it arrived; refunded to the new one", gun.name);
            if (player) Refund(player, p.price, gun.name);
        } else if (arrived && !waiting) {
            pendingGun = PendingGun{};
            InterlockedIncrement(&gunsArrived);
            Line("gun %s arrived after %lu ms", gun.name, static_cast<unsigned long>(age));
            SetStatus(coop_shop::kStatusGood, "%s acquired", gun.name);
            Announce("%s acquired", gun.name);
            dirty = true;
        } else if (!waiting && age > 1500) {
            pendingGun = PendingGun{};
            InterlockedIncrement(&failed);
            Line("gun %s: the placeholder is gone and no gun came", gun.name);
            Refund(player, p.price, gun.name);
            SetStatus(coop_shop::kStatusBad, "%s did not arrive: refunded", gun.name);
            dirty = true;
        } else if (age > kGunTimeoutMs) {
            pendingGun = PendingGun{};
            InterlockedIncrement(&gunTimeouts);
            uint32_t qm = 0;
            if (Read(base + bot::kQuartermasterRva, &qm, 4) && qm) bot::CancelRequisitionRaw(qm, player, gun.proxy);
            if (waiting && bot::RemoveItemRaw(player + bot::kWeaponInventory, p.proxy)) turret::DeleteRaw(p.proxy);
            Line("gun %s: its stream never came in %lu s; requisition cancelled", gun.name, static_cast<unsigned long>(kGunTimeoutMs / 1000));
            Refund(player, p.price, gun.name);
            SetStatus(coop_shop::kStatusBad, "%s did not arrive: refunded", gun.name);
            dirty = true;
        }
    }
    if (pendingSuit.idx >= 0 && Age(now, pendingSuit.startedAt) > kSuitCheckMs) {
        const PendingSuit p = pendingSuit;
        const DWORD age = Age(now, p.startedAt);
        const auto& suit = bot::kSuits[p.idx];
        const uint32_t player = LocalCampaignPlayer();
        uint32_t worn[4]{};
        if (player == p.player && bot::WornSuit(player, worn) && std::memcmp(worn, suit.guid, 16) == 0) {
            pendingSuit = PendingSuit{};
            InterlockedIncrement(&suitChanges);
            Line("suit %s on after %lu ms", suit.name, static_cast<unsigned long>(age));
            SetStatus(coop_shop::kStatusGood, "%s equipped", suit.name);
            Announce("%s equipped", suit.name);
            dirty = true;
        } else if (player != p.player || age > kSuitTimeoutMs) {
            pendingSuit = PendingSuit{};
            InterlockedIncrement(&failed);
            Line("suit %s: not on the body after %lu ms (%s); refunded", suit.name, static_cast<unsigned long>(age),
                 player != p.player ? "the body changed" : "the change did not take");
            if (player) Refund(player, p.price, suit.name);
            SetStatus(coop_shop::kStatusBad, "%s did not take: refunded", suit.name);
            dirty = true;
        }
    }
}

// --- opening, closing, the view ----------------------------------------------------------------
coop_shop::GateOut Gate(uint32_t player, bool mpMap) {
    coop_shop::GateIn g;
    g.worldReady = inLevel && turret::WorldReady();
    g.player = player != 0;
    g.cutscene = player && turret::CutsceneNow();
    g.mpMap = mpMap;
    g.campaignAllowed = campaignAllowed;
    g.duringWaves = duringWaves || ClockModeOpen();
    g.survival = survState;
    return coop_shop::Evaluate(g);
}
void Close(const char* why, coop_shop::GateOut gate, bool automatic) {
    open = false;
    overlay::Hide();
    overlay::SetPointer(false, 0, 0);
    published = false;
    focusrate::PanelHolds(1, false);
    for (int vk = 0x60; vk <= 0x6F; ++vk)
        if (GetAsyncKeyState(vk) & 0x8000) swallow.Take(vk);
    InterlockedIncrement(automatic ? &autoCloses : &closes);
    Line("closed (%s) after %lu s, %ld purchase(s)", why, static_cast<unsigned long>((GetTickCount() - openedAt) / 1000), visitBuys);
    if (automatic) Announce("%s", coop_shop::GateNotice(gate));
}
// An open panel mutes the game's input (focusrate::PanelHolds): one the overlay never draws
// is a frozen game (17.10., the debug menu). One second after the open it must have been drawn.
bool drawCheckDone = true;
LONG drawHooksAtOpen = 0, drawFramesAtOpen = 0, drawOffscreenAtOpen = 0;
volatile LONG notDrawn = 0;
bool CheckDrawn(DWORD now) {
    if (drawCheckDone || Age(now, openedAt) < 1000) return false;
    drawCheckDone = true;
    const LONG hooks = InterlockedCompareExchange(&overlay::hookCalls, 0, 0) - drawHooksAtOpen;
    const LONG frames = InterlockedCompareExchange(&overlay::panelFrames, 0, 0) - drawFramesAtOpen;
    const LONG offscreen = InterlockedCompareExchange(&overlay::offscreenFrames, 0, 0) - drawOffscreenAtOpen;
    if (frames > 0) {
        Line("open for 1 s: panel drawn in %ld frame(s), %ld frame(s) ended offscreen (back buffer draws %ld, failures %ld)", frames, offscreen,
             InterlockedCompareExchange(&overlay::backBufferDraws, 0, 0), InterlockedCompareExchange(&overlay::backBufferFailures, 0, 0));
        return false;
    }
    InterlockedIncrement(&notDrawn);
    Line("open for 1 s but NOT DRAWN: render hook calls +%ld, frames drawn +%ld, visible=%ld disabled=%ld camera_hidden=%ld draw_faults=%ld", hooks, frames,
         InterlockedCompareExchange(&overlay::visible, 0, 0), InterlockedCompareExchange(&overlay::disabled, 0, 0),
         InterlockedCompareExchange(&overlay::cameraHidden, 0, 0), InterlockedCompareExchange(&overlay::drawFaults, 0, 0));
    Close("nothing drawn: the input goes back to the game", coop_shop::GateOut::Open, false);
    return true;
}
void TryOpen(uint32_t player, bool mpMap, DWORD now) {
    const coop_shop::GateOut gate = Gate(player, mpMap);
    if (gate != coop_shop::GateOut::Open) {
        InterlockedIncrement(&refusedOpens);
        Line("not opened: %s (map %s, survival %d wave %d)", coop_shop::GateName(gate), mpMap ? "mp" : "campaign", survState, survWave);
        Announce("%s", coop_shop::GateNotice(gate));
        return;
    }
    open = true;
    openedAt = now;
    visitBuys = 0;
    dirty = true;
    status[0] = 0;
    input.Reset();
    focusrate::PanelHolds(1, true);
    InterlockedIncrement(&opens);
    drawCheckDone = false;
    drawHooksAtOpen = InterlockedCompareExchange(&overlay::hookCalls, 0, 0);
    drawFramesAtOpen = InterlockedCompareExchange(&overlay::panelFrames, 0, 0);
    drawOffscreenAtOpen = InterlockedCompareExchange(&overlay::offscreenFrames, 0, 0);
    Line("opened: credits %d, %s, survival %d wave %d, tab %s, overlay %s", Credits(player), mpMap ? "mp map" : "campaign", survState, survWave,
         coop_shop::TabName(cursor.tab), overlay::ready ? "ready" : "NOT READY");
}
void PublishView(uint32_t player, bool mpMap, DWORD now) {
    coop_shop::View v;
    std::memset(&v, 0, sizeof(v));
    std::snprintf(v.title, sizeof(v.title), "SUPPLY TERMINAL");
    const int32_t credits = Credits(player);
    char number[24];
    coop_shop::FormatCredits(number, sizeof(number), credits < 0 ? 0 : credits);
    std::snprintf(v.credits, sizeof(v.credits), "CREDITS %s", number);
    const bool counting = survState == coop_shop::kSurvArming || survState == coop_shop::kSurvIntermission;
    const int left = counting ? coop_shop::SecondsLeft(survStateSince, now, survState == coop_shop::kSurvArming ? startDelaySeconds : pauseSeconds) : -1;
    if (ClockModeOpen() && survState == coop_shop::kSurvWave) std::snprintf(v.wave, sizeof(v.wave), "%s in progress", visitMode == 2 ? "Objective" : "Hold the Zone");
    else coop_shop::WaveLine(v.wave, sizeof(v.wave), mpMap, survivalFn != nullptr, survState, survWave, left);
    v.tab = cursor.tab;
    const int row = cursor.row[cursor.tab];
    coop_shop::BuildRows(items, itemCount, row, cursor.top[cursor.tab], rowsWanted, credits, v);
    if (row < itemCount) std::snprintf(v.detail, sizeof(v.detail), "%s", items[row].detail);
    if (status[0] && Age(now, statusAt) < kStatusMs) {   // a purchase this tick stamps it after `now`
        std::snprintf(v.status, sizeof(v.status), "%s", status);
        v.statusKind = statusKind;
    }
    char keyName[16];
    std::snprintf(v.hint, sizeof(v.hint), "A/D TAB   W/S SELECT   SPACE OR CLICK BUY   %s%s CLOSE", coop_shop::KeyName(key, keyName), escapeCloses ? "/ESC" : "");
    if (published && std::memcmp(&v, &lastView, sizeof(v)) == 0) return;
    lastView = v;
    published = true;
    overlay::Publish(v);
}

void TickInner() {
    const DWORD now = GetTickCount();
    const uint32_t player = inLevel ? LocalCampaignPlayer() : 0;
    const bool visit = InterlockedCompareExchange(&mpmap::phaseMirror, 0, 0) == static_cast<LONG>(coop_mpmap::Phase::Arrived);
    PollSurvival(now, visit);
    // A survival map: an [MpMap] visit, or wherever a survival run is on (10.10.:
    // the arenas start as a campaign level and the terminal said SURVIVAL ONLY).
    // Versus and Mercenaries have none (the players' gear comes from the MP loadout
    // screens there): the open key goes to the game and the panel stays shut.
    const bool versus = play::VersusMode();   // a running match, not the picker's stale choice
    const LONG picked = InterlockedCompareExchange(&gamemode::mode, 0, 0);
    const LONG modeNow = visit ? picked : arena::RunningNow() ? arena::ModeNow(static_cast<int>(picked)) : 0;
    if (modeNow != visitMode) {
        Line("game mode on this map: %ld -> %ld (%s)", visitMode, modeNow,
             modeNow == 1 ? "no terminal" : modeNow == 2 || modeNow == 3 ? "terminal open during the run" : "terminal between waves");
        visitMode = modeNow;
    }
    const char* noTerminal = versus ? "Versus" : modeNow == 1 ? "Mercenaries" : nullptr;
    const bool mpMap = (visit || survState != coop_shop::kSurvOff) && !noTerminal;
    if (noTerminal && open) {
        char why[48];
        std::snprintf(why, sizeof(why), "%s has no supply terminal", noTerminal);
        Close(why, coop_shop::GateOut::Open, false);
    }
    BotShopping(now, mpMap);
    TerminalHint(mpMap);
    // Game time stands still in the pause menu: the panel hides meanwhile.
    const uint32_t sim = turret::SimMs();
    if (sim != lastSim) { lastSim = sim; simStillSince = now; paused = false; }
    else if (now - simStillSince > 300) paused = true;

    // The open key only where the terminal can open; the game never sees it there.
    focusrate::panelKey[key & 0xFF] = mpMap || campaignAllowed;
    const bool focused = necro::GameWindowFocused();
    auto down = [&](int vk) { return focused && vk && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const bool toggle = toggleEdge.Pressed(down(key));
    const bool escape = escapeEdge.Pressed(escapeCloses && down(VK_ESCAPE));

    if (toggle && noTerminal) {
        Line("not opened: %s has no supply terminal", noTerminal);
    } else if (toggle) {
        if (open) Close("the shop key", coop_shop::GateOut::Open, false);
        else if (InterlockedCompareExchange(&foreignPanel, 0, 0)) Line("not opened: the debug menu is open");
        else TryOpen(player, mpMap, now);
    }
    if (open) {
        const coop_shop::GateOut gate = Gate(player, mpMap);
        if (gate != coop_shop::GateOut::Open) {
            Close(coop_shop::GateName(gate), gate, true);
        } else if (paused) {
            // The pause menu needs the keyboard and the mouse back.
            InterlockedIncrement(&pausedTicks);
            Close("the game paused", gate, false);
        } else if (escape) {
            Close("Escape", gate, false);
        } else if (CheckDrawn(now)) {
            // closed: the overlay never showed it
        } else {
            const panelinput::Frame f = input.Read(now, focused);
            if (f.left) { coop_shop::StepTab(cursor, -1); dirty = true; }
            if (f.right) { coop_shop::StepTab(cursor, 1); dirty = true; }
            if (f.tab >= 0 && f.tab < coop_shop::kTabs && f.tab != cursor.tab) { cursor.tab = f.tab; dirty = true; }
            if (dirty || now - rebuiltAt > kRefreshMs) Rebuild(player);
            const int tab = cursor.tab;
            if (f.up) coop_shop::StepRow(cursor, -1, itemCount);
            if (f.down) coop_shop::StepRow(cursor, 1, itemCount);
            if (f.pageUp || f.pageDown) coop_shop::StepRow(cursor, f.pageDown ? rowsWanted : -rowsWanted, itemCount);
            if (f.scroll) coop_shop::StepRow(cursor, f.scroll, itemCount);
            // The view's rows start at the tab's top row.
            if (f.hoverRow >= 0 && cursor.top[tab] + f.hoverRow < itemCount) cursor.row[tab] = cursor.top[tab] + f.hoverRow;
            bool buy = f.use;
            if (f.clickRow >= 0 && cursor.top[tab] + f.clickRow < itemCount) {
                const int row = cursor.top[tab] + f.clickRow;
                if (row == cursor.row[tab]) buy = true;
                else cursor.row[tab] = row;
            }
            coop_shop::Fit(cursor, itemCount, rowsWanted);
            if (buy) Buy(player);
            if (dirty) Rebuild(player);
            PublishView(player, mpMap, now);
        }
    }
    PollPending(now);
}
int TickFilter(unsigned code) {
    InterlockedIncrement(&tickFaults);
    gate::Event("shop FAULT tick code %08X", code);
    return EXCEPTION_EXECUTE_HANDLER;
}
void Tick(DWORD) {
    if (!installed) return;
    __try {
        TickInner();
    } __except (TickFilter(GetExceptionCode())) {
        if (InterlockedCompareExchange(&tickFaults, 0, 0) >= 5) {
            installed = false;
            open = false;
            overlay::Hide();
            overlay::SetPointer(false, 0, 0);
            focusrate::PanelHolds(1, false);
            focusrate::panelKey[key & 0xFF] = false;
            gate::Event("shop: five faulted ticks, switched off for the session");
        }
    }
}

// --- setup -----------------------------------------------------------------------------------
uint32_t IniPrice(const std::string& ini, const char* name, uint32_t fallback) {
    char text[32]{};
    GetPrivateProfileStringA("Shop", name, "", text, sizeof(text), ini.c_str());
    if (!text[0]) return fallback;
    const uint32_t v = coop_shop::ParsePrice(text, fallback);
    if (v == fallback && std::strtoul(text, nullptr, 10) != fallback) Line("[Shop] %s='%s' is no price; %u stays", name, text, fallback);
    return v;
}
// A function start, or another plugin's 5-byte detour followed by its bytes.
bool Matches(uint32_t rva, const unsigned char* bytes, size_t size) {
    unsigned char actual[24]{};
    if (size > sizeof(actual) || !Read(base + rva, actual, size)) return false;
    return std::memcmp(actual, bytes, size) == 0 || (actual[0] == 0xE9 && size > 5 && std::memcmp(actual + 5, bytes + 5, size - 5) == 0);
}
bool Install(FILE* log, const std::string& ini) {
    wanted = GetPrivateProfileIntA("Shop", "Enabled", 0, ini.c_str()) != 0;
    if (!wanted) { std::fprintf(log, "shop off ([Shop] Enabled=0)\n"); return false; }
    key = GetPrivateProfileIntA("Shop", "Key", 'V', ini.c_str());
    if (key <= 0 || key > 255) key = 'V';
    campaignAllowed = GetPrivateProfileIntA("Shop", "Campaign", 0, ini.c_str()) != 0;
    duringWaves = GetPrivateProfileIntA("Shop", "DuringWaves", 0, ini.c_str()) != 0;
    escapeCloses = GetPrivateProfileIntA("Shop", "EscapeCloses", 1, ini.c_str()) != 0;
    notices = GetPrivateProfileIntA("Shop", "Notices", 1, ini.c_str()) != 0;
    requestStreams = GetPrivateProfileIntA("Shop", "RequestGunStreams", 1, ini.c_str()) != 0;
    intoInventory = GetPrivateProfileIntA("Shop", "Inventory", 1, ini.c_str()) != 0;
    replaceHeld = GetPrivateProfileIntA("Shop", "ReplaceHeldGun", 1, ini.c_str()) != 0;
    botShopping = GetPrivateProfileIntA("Shop", "BotShopping", 1, ini.c_str()) != 0;
    rowsWanted = GetPrivateProfileIntA("Shop", "Rows", 10, ini.c_str());
    if (rowsWanted < 3) rowsWanted = 3;
    if (rowsWanted > coop_shop::kMaxRows) rowsWanted = coop_shop::kMaxRows;
    char name[64];
    for (int i = 0; i < coop_shop::kGunCount; ++i) {
        std::snprintf(name, sizeof(name), "Price%s", coop_shop::kGuns[i].key);
        priceGun[i] = IniPrice(ini, name, coop_shop::kGuns[i].gunPrice);
        std::snprintf(name, sizeof(name), "PriceAmmo%s", coop_shop::kGuns[i].key);
        priceAmmo[i] = IniPrice(ini, name, coop_shop::kGuns[i].ammoPrice);
    }
    priceMed[0] = IniPrice(ini, "PriceMedPackSmall", priceMed[0]);
    priceMed[1] = IniPrice(ini, "PriceMedPackMedium", priceMed[1]);
    priceMed[2] = IniPrice(ini, "PriceMedPackLarge", priceMed[2]);
    priceStasis = IniPrice(ini, "PriceStasisPack", priceStasis);
    priceDrop = IniPrice(ini, "PriceSupplyDrop", priceDrop);
    priceNode = IniPrice(ini, "PriceUpgradeNode", priceNode);
    priceRigNode = IniPrice(ini, "PriceRigNode", priceRigNode);
    priceStasisNode = IniPrice(ini, "PriceStasisNode", priceStasisNode);
    priceSuit = IniPrice(ini, "PriceSuit", priceSuit);
    for (int i = 0; i < bot::kSuitKinds; ++i) {
        // "Hazard Suit" -> PriceSuitHazard
        char compact[48];
        size_t n = 0;
        for (const char* c = bot::kSuits[i].name; *c && n + 1 < sizeof(compact); ++c) if (*c != ' ') compact[n++] = *c;
        compact[n] = 0;
        if (n > 4 && std::strcmp(compact + n - 4, "Suit") == 0) compact[n - 4] = 0;
        std::snprintf(name, sizeof(name), "PriceSuit%s", compact);
        priceSuitOf[i] = IniPrice(ini, name, 0);
    }
    priceTurret = IniPrice(ini, "PriceTurretRefill", priceTurret);
    priceMine = IniPrice(ini, "PriceStasisMineRecharge", priceMine);
    priceField = IniPrice(ini, "PriceHealFieldRecharge", priceField);

    // The survival pause lengths, for the countdown on the panel.
    const size_t slash = ini.find_last_of('\\');
    const std::string survivalIni = ds2coop::ConfigFile((slash == std::string::npos ? std::string(".") : ini.substr(0, slash)) + "\\", "DS2Survival.ini");
    const bool survivalIniFound = GetFileAttributesA(survivalIni.c_str()) != INVALID_FILE_ATTRIBUTES;
    pauseSeconds = GetPrivateProfileIntA("Survival", "PauseSeconds", 20, survivalIni.c_str());
    startDelaySeconds = GetPrivateProfileIntA("Survival", "StartDelaySeconds", 20, survivalIni.c_str());

    // anyDetour: a short function start another plugin may have detoured whole (called through it).
    struct Site { uint32_t rva; unsigned char bytes[16]; unsigned size; const char* name; bool* group; bool anyDetour; };
    bool spawnOk = true, pickupOk = true, upgradeOk = true, suitOk = true;
    const uint32_t upgradeManager = static_cast<uint32_t>(base + bot::kUpgradeManagerRva);
    const Site sites[] = {
        {bot::kGlobalPickupRva, {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x28, 0x53, 0x8B, 0x1D}, 10, "GetGlobalPickupItem", &spawnOk, false},
        {bot::kCanFitRva, {0x83, 0xEC, 0x30, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x44, 0x57, 0x8B, 0xCE, 0xE8}, 14, "CanFitItem", &spawnOk, false},
        {bot::kSubtractCurrencyRva, {0x8B, 0x41, 0x28, 0x83, 0xEC, 0x20, 0x56, 0x8B, 0x74, 0x24, 0x28, 0x3B, 0xF0, 0x77, 0x07, 0x2B}, 16, "SubtractCurrency", &spawnOk, false},
        {bot::kBaseDeleteRva, {0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x06, 0x8B, 0x50, 0x08, 0x8B, 0xCE, 0xFF, 0xD2, 0x83, 0x3D}, 16, "Base::Delete", &spawnOk, false},
        {turret::kItemDropRva, {0x51, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x40, 0x02, 0x00, 0x00, 0x57, 0x85, 0xC0}, 14, "Item::Drop", &spawnOk, false},
        {bot::kItemPickupRva, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x14, 0x8B, 0x45, 0x08, 0x53, 0x56, 0x57, 0x8B}, 16, "Item::Pickup", &pickupOk, false},
        {bot::kRequisitionRva, {0x51, 0x53, 0x55, 0x56, 0x57, 0x8D, 0x44, 0x24, 0x10, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x1C, 0x50}, 16, "Quartermaster::RequisitionGun", &pickupOk, false},
        {bot::kCancelRequisitionRva, {0x53, 0x55, 0x8B, 0xE9, 0x33, 0xDB, 0x39, 0x5D, 0x24, 0x76, 0x69, 0x56, 0x57, 0x8D, 0x7D, 0x20}, 16, "Quartermaster::CancelRequisition", &pickupOk, false},
        {bot::kInventoryIsFullRva, {0xF6, 0x41, 0x30, 0x02, 0x8B, 0x51, 0x08, 0x57, 0x8B, 0xFA, 0x74, 0x29, 0x33, 0xFF, 0x85, 0xD2}, 16, "Inventory::IsFull", &pickupOk, false},
        {bot::kInventoryRemoveRva, {0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xF1, 0x85, 0xC0, 0x74, 0x13, 0x50, 0xE8}, 13, "Inventory::RemoveItem", &pickupOk, false},
        {bot::kFindGunEntryRva, {0x56, 0x33, 0xC0, 0x33, 0xF6, 0x39, 0x41, 0x18, 0x76, 0x49, 0x53, 0x57, 0x8B, 0x7C, 0x24, 0x10}, 16, "FindGunEntryByProxyGUID", &pickupOk, false},
        {bot::kNodeTypeRva, {0x8B, 0x01, 0x50, 0xE8}, 4, "UpgradeStruct::GetNodeType", &upgradeOk, true},
        // sub esp,10h / mov eax,[UpgradeManager] / push ebx..edi / mov edi,ecx, and its own ret 4
        {kPlayerCalcRva, {0x83, 0xEC, 0x10, 0xA1, static_cast<unsigned char>(upgradeManager), static_cast<unsigned char>(upgradeManager >> 8),
                          static_cast<unsigned char>(upgradeManager >> 16), static_cast<unsigned char>(upgradeManager >> 24), 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9}, 14,
         "Player::CalcUpgradedValues", &upgradeOk, false},
        {kPlayerCalcRetRva, {0xC2, 0x04, 0x00}, 3, "Player::CalcUpgradedValues ret 4", &upgradeOk, false},
        {kChangeToUniformRva, {0x51, 0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x56, 0x8B, 0xF1, 0x85, 0xDB, 0x0F, 0x84, 0x71, 0x01, 0x00}, 16, "ChangeToUniform", &suitOk, false},
    };
    for (const Site& s : sites) {
        unsigned char head = 0;
        const bool ok = Matches(s.rva, s.bytes, s.size) || (s.anyDetour && Read(base + s.rva, &head, 1) && head == 0xE9);
        if (!ok) { *s.group = false; std::fprintf(log, "shop: %s at %08X does not match\n", s.name, static_cast<unsigned>(base + s.rva)); }
    }
    uint32_t managerVtable = 0, manager = 0;
    const bool managerOk = Read(base + kUniformManagerRva, &manager, 4) && (!manager || (Read(manager, &managerVtable, 4) && managerVtable == base + kUniformManagerVtableRva));
    deliveryOk = spawnOk;
    gunsOk = spawnOk && pickupOk;
    upgradesOk = upgradeOk;
    suitsOk = suitOk && managerOk;
    const bool overlayOk = overlay::Install(log, ini);
    installed = overlayOk;
    std::fprintf(log, "shop %s key=%d campaign=%d during_waves=%d escape_closes=%d notices=%d rows=%d request_gun_streams=%d inventory=%d replace_held_gun=%d bot_shopping=%d | delivery=%d guns=%d upgrades=%d "
                      "suits=%d (uniform manager %s) | overlay=%d | survival export resolved at run time, pause %d s, start delay %d s (%s %s)\n",
                 installed ? "installed" : "OFF (the render hook did not go in)", key, campaignAllowed, duringWaves, escapeCloses, notices, rowsWanted, requestStreams,
                 intoInventory, replaceHeld, botShopping, deliveryOk, gunsOk, upgradesOk, suitsOk, managerOk ? (manager ? "ok" : "not created yet") : "VTABLE MISMATCH", overlayOk, pauseSeconds,
                 startDelaySeconds, survivalIni.c_str(), survivalIniFound ? "found" : "missing, defaults");
    char prices[600];
    int used = std::snprintf(prices, sizeof(prices), "shop prices: med %u/%u/%u stasis %u drop %u node %u rig_node %u stasis_node %u suit %u turret %u mine %u field %u | guns",
                             priceMed[0], priceMed[1], priceMed[2], priceStasis, priceDrop, priceNode, priceRigNode, priceStasisNode, priceSuit, priceTurret, priceMine, priceField);
    for (int i = 0; i < coop_shop::kGunCount && used > 0 && used < static_cast<int>(sizeof(prices)); ++i)
        used += std::snprintf(prices + used, sizeof(prices) - used, " %s %u/%u", coop_shop::kGuns[i].key, priceGun[i], priceAmmo[i]);
    std::fprintf(log, "%s (gun/ammo)\n", prices);
    return installed;
}

void WriteDiagnostics(FILE* log) {
    if (!wanted) return;
    static LONG last = -1;
    LONG guards = 0;
    for (LONG g : guardRefused) guards += g;
    const LONG sum = opens + closes * 3 + autoCloses * 5 + refusedOpens * 7 + attempts * 11 + bought * 13 + failed * 17 + refunds * 19 + guards * 23 + tickFaults * 29 +
                     survivalChanges * 31 + gunsArrived * 37 + suitChanges * 41 + gunsReplaced * 43 + botShops * 47 + notDrawn * 53;
    if (sum != last) {
        last = sum;
        std::fprintf(log, "shop open=%d tab=%s items=%d opens=%ld closes=%ld auto_closes=%ld refused_opens=%ld paused_ticks=%ld | buys attempts=%ld ok=%ld failed=%ld "
                          "too_poor=%ld unavailable=%ld refunds=%ld (%ld cr) spent=%ld fallback_charges=%ld | delivered inventory=%ld floor=%ld partial=%ld | guns ordered=%ld "
                          "arrived=%ld timeouts=%ld pending=%d replaced_in_hand=%ld | nodes=%ld suits=%ld gadgets=%ld | numpad guard turret=%ld mine=%ld field=%ld call=%ld mark=%ld order=%ld "
                          "toggle=%ld | survival export=%d state=%d wave=%d | tick_faults=%ld not_drawn=%ld\n",
                     open ? 1 : 0, coop_shop::TabName(cursor.tab), itemCount, opens, closes, autoCloses, refusedOpens, pausedTicks, attempts, bought, failed, tooPoor,
                     unavailable, refunds, refundedCredits, spent, fallbackCharges, toInventory, toFloor, partial, gunsOrdered, gunsArrived, gunTimeouts, pendingGun.idx, gunsReplaced,
                     nodesFilled, suitChanges, gadgetBuys, guardRefused[0], guardRefused[1], guardRefused[2], guardRefused[3], guardRefused[4], guardRefused[5],
                     guardRefused[6], survivalFn ? 1 : 0, survState, survWave, tickFaults, notDrawn);
    }
    overlay::WriteDiagnostics(log);
}
}  // namespace shop
