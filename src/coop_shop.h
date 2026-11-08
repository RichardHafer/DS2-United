// Dead Space 2 United by Richard Hafer.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "coop_bot_bench.h"

// The supply terminal's logic without a game: catalogue, prices, tabs and
// cursor, key repeat, when it may open, the upgrade path to the next stat, and
// the text and geometry of the panel. runtime_coop_shop.inc reads the world and
// fills it in; runtime_coop_overlay.inc draws the View.
namespace coop_shop {

// --- tabs, keys ---
enum class Tab : uint8_t { Supplies, Weapons, Upgrades, Suits, Gadgets };
constexpr int kTabs = 5;
inline const char* TabName(int t) {
    static const char* const kNames[kTabs] = {"SUPPLIES", "WEAPONS", "UPGRADES", "SUITS", "GADGETS"};
    return t >= 0 && t < kTabs ? kNames[t] : "?";
}

// Modules that ask before reading a numpad key.
enum class Guard : uint8_t { Turret, StasisMine, HealField, BotCall, BotMark, BotOrder, BotToggle, Count };
constexpr int kGuards = static_cast<int>(Guard::Count);
inline const char* GuardName(int g) {
    static const char* const kNames[kGuards] = {"turret", "stasis_mine", "heal_field", "bot_call", "bot_mark", "bot_order", "bot_toggle"};
    return g >= 0 && g < kGuards ? kNames[g] : "?";
}
// A panel's open key as the hint line shows it.
inline const char* KeyName(int vk, char (&out)[16]) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { out[0] = static_cast<char>(vk); out[1] = 0; return out; }
    switch (vk) {
    case 0x2D: return "INS";
    case 0x24: return "HOME";
    case 0x23: return "END";
    case 0x09: return "TAB";
    case 0x6A: return "NUM *";
    default: break;
    }
    if (vk >= 0x70 && vk <= 0x7B) { std::snprintf(out, sizeof(out), "F%d", vk - 0x6F); return out; }
    std::snprintf(out, sizeof(out), "KEY %d", vk);
    return out;
}
// VK_NUMPAD0 .. VK_DIVIDE: the whole block belongs to the open shop.
inline bool IsNumpad(int vk) { return vk >= 0x60 && vk <= 0x6F; }

// Keys still held when the shop closed stay swallowed until they are let go,
// so the Numpad 0 that closed it does not call the bots on the next frame.
struct Swallow {
    uint32_t mask = 0;   // bit vk - 0x60
    void Take(int vk) { if (IsNumpad(vk)) mask |= 1u << (vk - 0x60); }
    bool Holds(int vk, bool down) {
        if (!IsNumpad(vk)) return false;
        const uint32_t bit = 1u << (vk - 0x60);
        if (!(mask & bit)) return false;
        if (down) return true;
        mask &= ~bit;
        return false;
    }
};

// A held navigation key: the press, then after delayMs one step every everyMs.
struct Repeat {
    bool down = false;
    uint32_t next = 0;
    bool Step(bool now, uint32_t ms, uint32_t delayMs = 380, uint32_t everyMs = 90) {
        if (!now) { down = false; return false; }
        if (!down) { down = true; next = ms + delayMs; return true; }
        if (static_cast<int32_t>(ms - next) >= 0) { next = ms + everyMs; return true; }
        return false;
    }
};
struct Edge {
    bool was = false;
    bool Pressed(bool down) { const bool p = down && !was; was = down; return p; }
};

// --- prices ---
constexpr uint32_t kMaxPrice = 9999999;
// A decimal price; anything else keeps the fallback. Too large is capped.
inline uint32_t ParsePrice(const char* text, uint32_t fallback) {
    if (!text) return fallback;
    while (*text == ' ' || *text == '\t') ++text;
    if (*text < '0' || *text > '9') return fallback;
    uint64_t v = 0;
    for (; *text >= '0' && *text <= '9'; ++text) {
        v = v * 10 + static_cast<uint64_t>(*text - '0');
        if (v > kMaxPrice) v = kMaxPrice + 1;
    }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    if (*text && *text != ';') return fallback;
    return v > kMaxPrice ? kMaxPrice : static_cast<uint32_t>(v);
}

// 12345 -> "12,345"
inline void FormatCredits(char* out, size_t size, int64_t value) {
    if (!out || !size) return;
    char digits[32];
    const bool negative = value < 0;
    uint64_t v = negative ? static_cast<uint64_t>(-value) : static_cast<uint64_t>(value);
    int n = 0;
    do { digits[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && n < 24);
    char text[40];
    int t = 0;
    if (negative) text[t++] = '-';
    for (int i = n - 1; i >= 0; --i) {
        text[t++] = digits[i];
        if (i && i % 3 == 0) text[t++] = ',';
    }
    text[t] = 0;
    std::snprintf(out, size, "%s", text);
}

// --- catalogue ---
// Gun vtables and proxy GUIDs as in the bots' gun catalogue (runtime_coop_bot.inc),
// spare ammo GUIDs as in the survival rewards (global_assets SpareAmmo).
struct GunEntry {
    const char* key;          // INI suffix: Price<key>, PriceAmmo<key>
    const char* name;
    uint32_t vtableRva;
    uint32_t proxy[4];
    uint32_t ammo[4];
    uint32_t gunPrice, ammoPrice;
};
inline const GunEntry kGuns[] = {
    {"PlasmaCutter", "Plasma Cutter", 0x17515D8, {0x30CAF6B7, 0xD12A955D, 0x54544950, 0x32303153}, {0x4BC78C36, 0x9D651322, 0x544E4543, 0x53574F4B}, 1000, 300},
    {"PulseRifle", "Pulse Rifle", 0x1751A00, {0x36C8BC4F, 0x67BB5F38, 0x44455257, 0x34303245}, {0x4BC78C36, 0x9D67757C, 0x544E4543, 0x53574F4B}, 2500, 350},
    {"LineGun", "Line Gun", 0x17511B8, {0x36C8BC49, 0x5F163CD0, 0x44455257, 0x34303245}, {0x4BC78CEC, 0xDFF02CCF, 0x544E4543, 0x53574F4B}, 2500, 450},
    {"Javelin", "Javelin Gun", 0x1750D98, {0x30C95FEE, 0xBBA303B8, 0x544E4543, 0x30574F4B}, {0x35C97DB8, 0x53ABA0A9, 0x44455257, 0x38343145}, 3000, 450},
    {"Ripper", "Ripper", 0x1752580, {0x36C8BC4F, 0x6F9078E2, 0x44455257, 0x34303245}, {0x30C8692D, 0x79E101DC, 0x4E484F4A, 0x324E4F53}, 2000, 400},
    {"Detonator", "Detonator", 0x1753100, {0x58CA30C7, 0x78E54CEB, 0x3045454C, 0x58373634}, {0x58CA30C7, 0xA9ADE7CB, 0x3045454C, 0x58373634}, 2500, 450},
    {"Flamethrower", "Flamethrower", 0x1753520, {0x36C8BC40, 0x0C67B69A, 0x44455257, 0x34303245}, {0x30C8692D, 0x6DBBF822, 0x4E484F4A, 0x324E4F53}, 2500, 400},
    {"SeekerRifle", "Seeker Rifle", 0x1773780, {0x45CB0685, 0xDD95DCDA, 0x43534147, 0x4E47494F}, {0x30C889E7, 0x7019FF4E, 0x4E484F4A, 0x324E4F53}, 3000, 450},
    {"ForceGun", "Force Gun", 0x1753948, {0x36C8BC48, 0xED477CBA, 0x44455257, 0x34303245}, {0x30C87D90, 0xE7570C3E, 0x4C454D50, 0x4B534544}, 4000, 500},
    {"ContactBeam", "Contact Beam", 0x1752CE0, {0x36C8BC3F, 0xF55615C8, 0x44455257, 0x34303245}, {0x30C87377, 0xE3BC55EA, 0x4C454D50, 0x4B534544}, 4000, 550},
    // the Foam Finger: class Gun (base vtable), no ammo use; profile level 60 (coop_progress.h)
    {"HandCannon", "Hand Cannon", 0x1750660, {0x38CB6750, 0x72BC9767, 0x54524542, 0x314D4152}, {}, 25000, 0},
};
constexpr int kGunCount = static_cast<int>(sizeof(kGuns) / sizeof(kGuns[0]));
constexpr int kHandCannon = 10;
inline bool HasAmmo(const GunEntry& g) { return (g.ammo[0] | g.ammo[1] | g.ammo[2] | g.ammo[3]) != 0; }
inline int GunByVtableRva(uint32_t rva) {
    for (int i = 0; i < kGunCount; ++i) if (kGuns[i].vtableRva == rva) return i;
    return -1;
}

// global_assets PowerUps.
constexpr uint32_t kMedSmall[4] = {0x4BC78C36, 0x9D5B89BA, 0x544E4543, 0x53574F4B};
constexpr uint32_t kMedMedium[4] = {0x4BC78C36, 0x9D5DEC14, 0x544E4543, 0x53574F4B};
constexpr uint32_t kMedLarge[4] = {0x4BC78C36, 0x9D604E6E, 0x544E4543, 0x53574F4B};
constexpr uint32_t kStasisPack[4] = {0x4BC806EA, 0x40D9D6F7, 0x544E4543, 0x53574F4B};

// Upgrade tree keys: the engine's name hash (h * 0x1003F + lower-case char).
constexpr uint32_t NameHash(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) {
        uint32_t c = static_cast<unsigned char>(*s);
        if (c >= 'A' && c <= 'Z') c += 0x20;
        h = h * 0x1003Fu + c;
    }
    return h;
}
constexpr uint32_t kRigTree = NameHash("RIG");                   // Player::CalcUpgradedValues, campaign body
constexpr uint32_t kMpHumanTree = NameHash("MP_Human");          // the same with the multiplayer flag
constexpr uint32_t kStasisTree = NameHash("StasisModule");

enum class Kind : uint8_t { Ammo, MedSmall, MedMedium, MedLarge, StasisPack, SupplyDrop, Gun, UpgradeGun, UpgradeRig, UpgradeStasis, Suit, TurretRefill, MineRecharge, FieldRecharge };
inline const char* KindName(Kind k) {
    switch (k) {
    case Kind::Ammo: return "ammo";
    case Kind::MedSmall: return "med_small";
    case Kind::MedMedium: return "med_medium";
    case Kind::MedLarge: return "med_large";
    case Kind::StasisPack: return "stasis_pack";
    case Kind::SupplyDrop: return "supply_drop";
    case Kind::Gun: return "gun";
    case Kind::UpgradeGun: return "upgrade_gun";
    case Kind::UpgradeRig: return "upgrade_rig";
    case Kind::UpgradeStasis: return "upgrade_stasis";
    case Kind::Suit: return "suit";
    case Kind::TurretRefill: return "turret_refill";
    case Kind::MineRecharge: return "mine_recharge";
    case Kind::FieldRecharge: return "field_recharge";
    }
    return "?";
}

struct Item {
    Kind kind = Kind::Ammo;
    int ref = -1;              // gun, suit or tree index, by kind
    int sub = 0;               // an upgrade's stat (node type)
    uint32_t price = 0;
    bool available = false;
    char label[48] = "";
    char detail[72] = "";
    char reason[20] = "";      // why not, instead of the price
};
enum class Verdict : uint8_t { Ok, Unavailable, TooPoor };
inline Verdict Check(const Item& item, int64_t credits) {
    if (!item.available) return Verdict::Unavailable;
    return credits >= 0 && static_cast<uint64_t>(credits) >= item.price ? Verdict::Ok : Verdict::TooPoor;
}
inline void SetItem(Item& it, Kind kind, int ref, const char* label, uint32_t price) {
    it = Item{};
    it.kind = kind;
    it.ref = ref;
    it.price = price;
    it.available = true;
    std::snprintf(it.label, sizeof(it.label), "%s", label);
}
inline void Refuse(Item& it, const char* reason) {
    it.available = false;
    std::snprintf(it.reason, sizeof(it.reason), "%s", reason);
}

// --- the cursor ---
struct Cursor {
    int tab = 0;
    int row[kTabs]{};
    int top[kTabs]{};
};
inline void StepTab(Cursor& c, int delta) { c.tab = ((c.tab + delta) % kTabs + kTabs) % kTabs; }
// Up and down wrap round the list.
inline void StepRow(Cursor& c, int delta, int count) {
    if (count <= 0) { c.row[c.tab] = 0; return; }
    c.row[c.tab] = ((c.row[c.tab] + delta) % count + count) % count;
}
// The row stays in the list and the window [top, top + visible) holds it.
inline void Fit(Cursor& c, int count, int visible) {
    int& row = c.row[c.tab];
    int& top = c.top[c.tab];
    if (visible < 1) visible = 1;
    if (count <= 0) { row = 0; top = 0; return; }
    if (row >= count) row = count - 1;
    if (row < 0) row = 0;
    if (row < top) top = row;
    if (row >= top + visible) top = row - visible + 1;
    if (top > count - visible) top = count - visible;
    if (top < 0) top = 0;
}

// --- when the terminal opens ---
// Survival director states from DS2Survival_State.
enum Survival : int { kSurvOff = 0, kSurvArming = 1, kSurvWave = 2, kSurvIntermission = 3, kSurvDead = 4 };
struct GateIn {
    bool player = false;          // a local Isaac stands
    bool worldReady = false;      // level settled, not the main menu, not loading
    bool cutscene = false;
    bool mpMap = false;           // an [MpMap] visit has arrived
    bool campaignAllowed = false; // [Shop] Campaign
    bool duringWaves = false;     // [Shop] DuringWaves
    int survival = kSurvOff;
};
enum class GateOut : uint8_t { Open, NoPlayer, NotReady, Cutscene, NotHere, WaveRunning, Dead };
inline GateOut Evaluate(const GateIn& g) {
    if (!g.worldReady) return GateOut::NotReady;
    if (!g.player) return g.mpMap && g.survival == kSurvDead ? GateOut::Dead : GateOut::NoPlayer;
    if (g.cutscene) return GateOut::Cutscene;
    if (!g.mpMap && !g.campaignAllowed) return GateOut::NotHere;
    if (g.mpMap && g.survival == kSurvDead) return GateOut::Dead;
    if (g.mpMap && g.survival == kSurvWave && !g.duringWaves) return GateOut::WaveRunning;
    return GateOut::Open;
}
inline const char* GateName(GateOut g) {
    switch (g) {
    case GateOut::Open: return "open";
    case GateOut::NoPlayer: return "no player";
    case GateOut::NotReady: return "the level is not ready";
    case GateOut::Cutscene: return "a cutscene runs";
    case GateOut::NotHere: return "not on a multiplayer map (Campaign=0)";
    case GateOut::WaveRunning: return "a wave is running (DuringWaves=0)";
    case GateOut::Dead: return "the run is over";
    }
    return "?";
}
// The RIG notice when it cannot open or has to close.
inline const char* GateNotice(GateOut g) {
    switch (g) {
    case GateOut::WaveRunning: return "SUPPLY TERMINAL: WAVE IN PROGRESS";
    case GateOut::NotHere: return "SUPPLY TERMINAL: SURVIVAL ONLY";
    case GateOut::Cutscene: return "SUPPLY TERMINAL: NOT NOW";
    case GateOut::Dead: return "SUPPLY TERMINAL: RUN OVER";
    default: return "SUPPLY TERMINAL: OFFLINE";
    }
}

// The wave line under the title. secondsLeft < 0: no estimate.
inline void WaveLine(char* out, size_t size, bool mpMap, bool exportFound, int survival, int wave, int secondsLeft) {
    if (!mpMap) { std::snprintf(out, size, "Campaign"); return; }
    if (!exportFound) { std::snprintf(out, size, "Survival director not loaded"); return; }
    switch (survival) {
    case kSurvArming:
    case kSurvIntermission: {
        const int next = survival == kSurvArming ? 1 : wave + 1;
        if (secondsLeft >= 0) std::snprintf(out, size, "Wave %d in %d s", next, secondsLeft);
        else std::snprintf(out, size, "Wave %d coming", next);
        return;
    }
    case kSurvWave: std::snprintf(out, size, "Wave %d in progress", wave); return;
    case kSurvDead: std::snprintf(out, size, "Run over at wave %d", wave); return;
    default: std::snprintf(out, size, "Survival not running"); return;
    }
}
// Seconds left of a pause that started at startMs and lasts lengthSeconds.
inline int SecondsLeft(uint32_t startMs, uint32_t nowMs, int lengthSeconds) {
    if (lengthSeconds <= 0) return -1;
    const uint32_t gone = nowMs - startMs;
    const int64_t left = static_cast<int64_t>(lengthSeconds) * 1000 - static_cast<int64_t>(gone);
    return left <= 0 ? 0 : static_cast<int>((left + 999) / 1000);
}

// --- upgrades ---
inline const char* NodeTypeName(int type) {
    switch (type) {
    case 1: return "DAMAGE";
    case 2: return "SPEED";
    case 3: return "CAPACITY";
    case 4: return "WIDTH";
    case 5: return "RELOAD";
    case 6: return "ALT DAMAGE";
    case 7: return "HP";
    case 8: return "AIR";
    case 9: return "ENERGY";
    case 10: return "DURATION";
    case 11: return "RANGE";
    case 12: return "CHARGE";
    case 13: return "SPECIAL";
    default: return "LINK";
    }
}
// The nodes one purchase fills: the bench pick (coop_bot_bench.h) again and
// again until a stat node is in, blanks on the way included. nodes == 0: maxed.
constexpr int kMaxPlan = 8;
struct Plan {
    int nodes = 0;
    int type = 0;            // the stat it ends on
    int order[kMaxPlan]{};
};
inline Plan PlanUpgrade(const coop_bot::BenchNode* tree, int count, int maxSteps = 6) {
    Plan p;
    if (!tree || count <= 0) return p;
    if (count > coop_bot::kMaxBenchNodes) count = coop_bot::kMaxBenchNodes;
    if (maxSteps > kMaxPlan) maxSteps = kMaxPlan;
    coop_bot::BenchNode work[coop_bot::kMaxBenchNodes];
    for (int i = 0; i < count; ++i) work[i] = tree[i];
    while (p.nodes < maxSteps) {
        const int pick = coop_bot::PickBenchNode(work, count);
        if (pick < 0) break;
        work[pick].filled = true;
        p.order[p.nodes++] = pick;
        if (work[pick].type != 0) { p.type = work[pick].type; return p; }
    }
    // Only blanks within reach of maxSteps: not worth a purchase.
    if (!p.type) p.nodes = 0;
    return p;
}
// One stat on its own (10.10.: the single row went for HP again and again
// before air was ever offered). The nearest empty node of that type, breadth
// first from every node that can be bought now over empty nodes; order[] runs
// from the bought node on, so each node lies next to one filled before it.
inline Plan PlanToType(const coop_bot::BenchNode* tree, int count, int type, int maxSteps = kMaxPlan) {
    Plan p;
    if (!tree || count <= 0 || type <= 0) return p;
    if (count > coop_bot::kMaxBenchNodes) count = coop_bot::kMaxBenchNodes;
    if (maxSteps > kMaxPlan) maxSteps = kMaxPlan;
    int dist[coop_bot::kMaxBenchNodes], from[coop_bot::kMaxBenchNodes], queue[coop_bot::kMaxBenchNodes];
    int head = 0, tail = 0, found = -1;
    for (int i = 0; i < count; ++i) { dist[i] = -1; from[i] = -1; }
    for (int i = 0; i < count; ++i)
        if (coop_bot::BenchReachable(tree, count, i)) { dist[i] = 0; queue[tail++] = i; }
    while (head < tail) {
        const int at = queue[head++];
        if (tree[at].type == type) { found = at; break; }
        for (uint32_t link : tree[at].links) {
            const int n = coop_bot::BenchFind(tree, count, link);
            if (n < 0 || tree[n].filled || dist[n] >= 0) continue;
            dist[n] = dist[at] + 1;
            from[n] = at;
            queue[tail++] = n;
        }
    }
    if (found < 0 || dist[found] + 1 > maxSteps) return p;
    int path[kMaxPlan], len = 0;
    for (int at = found; at >= 0 && len < kMaxPlan; at = from[at]) path[len++] = at;
    for (int k = 0; k < len; ++k) p.order[k] = path[len - 1 - k];
    p.nodes = len;
    p.type = type;
    return p;
}
// The stats a tree still has empty nodes of, ascending, each once.
inline int TypesLeft(const coop_bot::BenchNode* tree, int count, int* out, int max) {
    int n = 0;
    for (int type = 1; type <= 13 && n < max; ++type)
        for (int i = 0; i < count && i < coop_bot::kMaxBenchNodes; ++i)
            if (!tree[i].filled && tree[i].type == type) { out[n++] = type; break; }
    return n;
}
inline int EmptyOfType(const coop_bot::BenchNode* tree, int count, int type) {
    int n = 0;
    for (int i = 0; i < count && i < coop_bot::kMaxBenchNodes; ++i) n += !tree[i].filled && tree[i].type == type ? 1 : 0;
    return n;
}

// --- text and geometry ---
// Advances for the printable ASCII range 32..126, in pixels.
struct Metrics {
    int advance[95]{};
    int lineHeight = 0;
};
inline int Advance(const Metrics& m, char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return u >= 32 && u <= 126 ? m.advance[u - 32] : m.advance['?' - 32];
}
inline int Measure(const Metrics& m, const char* text) {
    int w = 0;
    for (; text && *text; ++text) w += Advance(m, *text);
    return w;
}
// The text cut to maxWidth with "..." when it does not fit.
inline void FitText(const Metrics& m, const char* text, int maxWidth, char* out, size_t size) {
    if (!out || !size) return;
    out[0] = 0;
    if (!text) return;
    if (Measure(m, text) <= maxWidth) { std::snprintf(out, size, "%s", text); return; }
    const int dots = Measure(m, "...");
    int w = 0;
    size_t n = 0;
    for (; text[n] && n + 4 < size; ++n) {
        const int a = Advance(m, text[n]);
        if (w + a + dots > maxWidth) break;
        w += a;
    }
    std::memcpy(out, text, n);
    if (dots <= maxWidth) std::memcpy(out + n, "...", 4); else out[n] = 0;
}
// Glyph height in pixels for a render target: 24 at 1080, 32 at 1440, 48 at 2160.
inline int GlyphPixels(int screenHeight, int scalePercent) {
    if (scalePercent < 50) scalePercent = 50;
    if (scalePercent > 250) scalePercent = 250;
    const int px = static_cast<int>((static_cast<int64_t>(screenHeight) * scalePercent + 2250) / 4500);
    return px < 12 ? 12 : px > 96 ? 96 : px;
}

// The panel: title, wave, tabs, rows, detail, status, hint.
struct Layout {
    int x = 0, y = 0, w = 0, h = 0;
    int pad = 0, line = 0;
    int titleY = 0, waveY = 0, tabsY = 0, rowsY = 0, detailY = 0, statusY = 0, hintY = 0;
    int rows = 0;
};
inline Layout PanelLayout(int screenW, int screenH, int px, int lineHeight, int wantRows) {
    Layout l;
    l.pad = px * 3 / 4;
    l.line = lineHeight + px / 4;
    l.rows = wantRows < 3 ? 3 : wantRows > 16 ? 16 : wantRows;
    // Rows give way first when the screen is short.
    for (;;) {
        l.h = l.line * (l.rows + 6) + l.pad * 7 / 2;
        if (l.h <= screenH - 2 * l.pad || l.rows <= 3) break;
        --l.rows;
    }
    l.w = px * 32;
    if (l.w > screenW - 4 * l.pad) l.w = screenW - 4 * l.pad;
    if (l.w < px * 12) l.w = px * 12;
    l.x = (screenW - l.w) / 2;
    l.y = (screenH - l.h) / 2;
    if (l.y < 0) l.y = 0;
    l.titleY = l.y + l.pad;
    l.waveY = l.titleY + l.line;
    l.tabsY = l.waveY + l.line + l.pad / 2;
    l.rowsY = l.tabsY + l.line + l.pad / 2;
    l.detailY = l.rowsY + l.line * l.rows + l.pad / 2;
    l.statusY = l.detailY + l.line;
    l.hintY = l.statusY + l.line;
    return l;
}

// --- the view the overlay draws ---
constexpr int kMaxRows = 16;
enum RowStyle : uint8_t { kRowNormal = 0, kRowTooPoor = 1, kRowUnavailable = 2, kRowSelected = 0x80 };
enum StatusKind : uint8_t { kStatusInfo = 0, kStatusGood = 1, kStatusBad = 2 };
struct Row {
    char label[48];
    char right[24];
    uint8_t style;
};
struct View {
    char title[24];
    char credits[32];
    char wave[64];
    int tab;
    int rowCount;
    int rowCapacity;
    bool above, below;
    Row rows[kMaxRows];
    char detail[96];
    char status[96];
    uint8_t statusKind;
    char hint[96];
    // another panel's own tabs (the debug menu); zero = the shop's
    int tabCount;
    char tabNames[8][16];
};
// The rows of one window of the list, styled for the credits at hand.
inline void BuildRows(const Item* items, int count, int selected, int top, int visible, int64_t credits, View& v) {
    if (visible > kMaxRows) visible = kMaxRows;
    if (visible < 0) visible = 0;
    v.rowCapacity = visible;
    v.rowCount = 0;
    for (int i = top; i < count && v.rowCount < visible; ++i) {
        Row& r = v.rows[v.rowCount++];
        const Item& it = items[i];
        std::snprintf(r.label, sizeof(r.label), "%s", it.label);
        const Verdict verdict = Check(it, credits);
        if (verdict == Verdict::Unavailable) {
            std::snprintf(r.right, sizeof(r.right), "%s", it.reason[0] ? it.reason : "N/A");
            r.style = kRowUnavailable;
        } else {
            FormatCredits(r.right, sizeof(r.right), it.price);
            r.style = verdict == Verdict::TooPoor ? kRowTooPoor : kRowNormal;
        }
        if (i == selected) r.style |= kRowSelected;
    }
    v.above = top > 0;
    v.below = top + v.rowCount < count;
}

}  // namespace coop_shop
