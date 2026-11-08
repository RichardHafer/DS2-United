// The supply terminal's logic without a game: prices, catalogue, cursor, key
// repeat and swallowing, the open gate, the wave line, upgrade plans, text
// fitting, glyph sizes, panel geometry and the rows the overlay draws.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "../src/coop_shop.h"

namespace {
using namespace coop_shop;

void Prices() {
    assert(ParsePrice("1500", 7) == 1500);
    assert(ParsePrice("  200 ", 7) == 200);
    assert(ParsePrice("0", 7) == 0);
    assert(ParsePrice("300 ; comment", 7) == 300);
    assert(ParsePrice("", 7) == 7);
    assert(ParsePrice(nullptr, 7) == 7);
    assert(ParsePrice("abc", 7) == 7);
    assert(ParsePrice("-5", 7) == 7);
    assert(ParsePrice("12k", 7) == 7);
    assert(ParsePrice("1.5", 7) == 7);
    assert(ParsePrice("99999999999999999999", 7) == kMaxPrice);
    assert(ParsePrice("10000000", 7) == kMaxPrice);
    char text[32];
    FormatCredits(text, sizeof(text), 0); assert(std::strcmp(text, "0") == 0);
    FormatCredits(text, sizeof(text), 999); assert(std::strcmp(text, "999") == 0);
    FormatCredits(text, sizeof(text), 1000); assert(std::strcmp(text, "1,000") == 0);
    FormatCredits(text, sizeof(text), 1234567); assert(std::strcmp(text, "1,234,567") == 0);
    FormatCredits(text, sizeof(text), -2500); assert(std::strcmp(text, "-2,500") == 0);
    std::printf("prices ok\n");
}

void Catalogue() {
    assert(kGunCount == 11 && std::strcmp(kGuns[kHandCannon].key, "HandCannon") == 0);
    for (int i = 0; i < kGunCount; ++i) {
        assert(GunByVtableRva(kGuns[i].vtableRva) == i);
        assert(kGuns[i].proxy[0]);
        if (i == kHandCannon) assert(!HasAmmo(kGuns[i]) && kGuns[i].ammoPrice == 0 && kGuns[i].gunPrice > 0);
        else assert(kGuns[i].gunPrice > kGuns[i].ammoPrice && kGuns[i].ammoPrice > 0 && HasAmmo(kGuns[i]));
        for (int k = i + 1; k < kGunCount; ++k) {
            assert(std::strcmp(kGuns[i].key, kGuns[k].key) != 0);
            assert(std::memcmp(kGuns[i].proxy, kGuns[k].proxy, 16) != 0);
            if (HasAmmo(kGuns[i]) && HasAmmo(kGuns[k])) assert(std::memcmp(kGuns[i].ammo, kGuns[k].ammo, 16) != 0);
        }
    }
    assert(GunByVtableRva(0x1234) == -1);
    // The values Player::CalcUpgradedValues (0x00879B80) uses.
    assert(kRigTree == 0x388C01B0u);
    assert(kMpHumanTree == 0x6D7ACF11u && kMpHumanTree == kRigTree + 0x34EECD61u);
    assert(kStasisTree == 0xC9B05EA9u && kMpHumanTree == kStasisTree + 0xA3CA7068u);   // the stasis read, 0x00879C4B
    assert(NameHash("HP") == 0x681A08u);
    assert(std::strcmp(TabName(0), "SUPPLIES") == 0 && std::strcmp(TabName(4), "GADGETS") == 0 && std::strcmp(TabName(5), "?") == 0);
    Item it;
    SetItem(it, Kind::Gun, 1, "Pulse Rifle", 4000);
    assert(Check(it, 4000) == Verdict::Ok && Check(it, 3999) == Verdict::TooPoor && Check(it, -1) == Verdict::TooPoor);
    Refuse(it, "OWNED");
    assert(Check(it, 99999) == Verdict::Unavailable && std::strcmp(it.reason, "OWNED") == 0);
    std::printf("catalogue ok\n");
}

void Navigation() {
    Cursor c;
    StepTab(c, 1); assert(c.tab == 1);
    StepTab(c, -2); assert(c.tab == kTabs - 1);
    StepTab(c, 1); assert(c.tab == 0);
    StepRow(c, -1, 5); assert(c.row[0] == 4);
    StepRow(c, 1, 5); assert(c.row[0] == 0);
    StepRow(c, 3, 0); assert(c.row[0] == 0);
    // Each tab keeps its own row.
    c.row[0] = 2;
    StepTab(c, 1);
    StepRow(c, 1, 3); assert(c.row[1] == 1 && c.row[0] == 2);
    // The window follows the row.
    Cursor w;
    w.row[0] = 12;
    Fit(w, 20, 10); assert(w.top[0] == 3);
    w.row[0] = 1;
    Fit(w, 20, 10); assert(w.top[0] == 1);
    w.row[0] = 25;
    Fit(w, 20, 10); assert(w.row[0] == 19 && w.top[0] == 10);
    w.top[0] = 15; w.row[0] = 16;
    Fit(w, 18, 10); assert(w.top[0] == 8);   // no empty rows at the end
    Fit(w, 0, 10); assert(w.row[0] == 0 && w.top[0] == 0);
    Fit(w, 4, 10); assert(w.top[0] == 0);
    std::printf("navigation ok\n");
}

void Keys() {
    Repeat r;
    assert(r.Step(true, 1000));            // the press
    assert(!r.Step(true, 1200));
    assert(r.Step(true, 1380));            // after the delay
    assert(!r.Step(true, 1400));
    assert(r.Step(true, 1470));            // then every interval
    assert(!r.Step(false, 1500));
    assert(r.Step(true, 1510));            // a new press
    Repeat wrap;
    assert(wrap.Step(true, 0xFFFFFF00u));
    assert(wrap.Step(true, 0xFFFFFF00u + 400));   // across the tick counter wrap
    Edge e;
    assert(e.Pressed(true) && !e.Pressed(true) && !e.Pressed(false) && e.Pressed(true));
    Swallow s;
    s.Take(0x60);                           // Numpad 0 closed the shop
    s.Take(0x41);                           // not a numpad key: ignored
    assert(s.Holds(0x60, true));            // still held: swallowed
    assert(!s.Holds(0x61, true));           // never taken
    assert(!s.Holds(0x60, false));          // let go: released
    assert(!s.Holds(0x60, true));           // the next press is the module's again
    assert(!s.Holds(0x41, true));
    assert(IsNumpad(0x60) && IsNumpad(0x66) && IsNumpad(0x6A) && IsNumpad(0x6F) && !IsNumpad(0x70) && !IsNumpad(0x5F));
    assert(std::strcmp(GuardName(0), "turret") == 0 && std::strcmp(GuardName(kGuards), "?") == 0);
    std::printf("keys ok\n");
}

void Gate() {
    GateIn g;
    g.player = true; g.worldReady = true; g.mpMap = true; g.survival = kSurvIntermission;
    assert(Evaluate(g) == GateOut::Open);
    g.survival = kSurvArming; assert(Evaluate(g) == GateOut::Open);
    g.survival = kSurvOff; assert(Evaluate(g) == GateOut::Open);        // no director: nothing to protect
    g.survival = kSurvWave; assert(Evaluate(g) == GateOut::WaveRunning);
    g.duringWaves = true; assert(Evaluate(g) == GateOut::Open);
    g.survival = kSurvDead; assert(Evaluate(g) == GateOut::Dead);
    g.survival = kSurvIntermission;
    g.cutscene = true; assert(Evaluate(g) == GateOut::Cutscene);
    g.cutscene = false;
    g.player = false; assert(Evaluate(g) == GateOut::NoPlayer);
    g.survival = kSurvDead; assert(Evaluate(g) == GateOut::Dead);
    g.player = true; g.survival = kSurvOff;
    g.worldReady = false; assert(Evaluate(g) == GateOut::NotReady);
    g.worldReady = true;
    g.mpMap = false; assert(Evaluate(g) == GateOut::NotHere);
    g.campaignAllowed = true; assert(Evaluate(g) == GateOut::Open);
    g.survival = kSurvWave; g.duringWaves = false; assert(Evaluate(g) == GateOut::Open);   // waves only count on the MP map
    assert(std::strcmp(GateNotice(GateOut::WaveRunning), "SUPPLY TERMINAL: WAVE IN PROGRESS") == 0);
    std::printf("gate ok\n");
}

void Waves() {
    char text[64];
    WaveLine(text, sizeof(text), true, true, kSurvIntermission, 3, 14); assert(std::strcmp(text, "Wave 4 in 14 s") == 0);
    WaveLine(text, sizeof(text), true, true, kSurvArming, 0, 20); assert(std::strcmp(text, "Wave 1 in 20 s") == 0);
    WaveLine(text, sizeof(text), true, true, kSurvIntermission, 5, -1); assert(std::strcmp(text, "Wave 6 coming") == 0);
    WaveLine(text, sizeof(text), true, true, kSurvWave, 7, 0); assert(std::strcmp(text, "Wave 7 in progress") == 0);
    WaveLine(text, sizeof(text), true, true, kSurvDead, 9, 0); assert(std::strcmp(text, "Run over at wave 9") == 0);
    WaveLine(text, sizeof(text), true, true, kSurvOff, 0, 0); assert(std::strcmp(text, "Survival not running") == 0);
    WaveLine(text, sizeof(text), true, false, kSurvOff, 0, 0); assert(std::strcmp(text, "Survival director not loaded") == 0);
    WaveLine(text, sizeof(text), false, true, kSurvWave, 2, 0); assert(std::strcmp(text, "Campaign") == 0);
    assert(SecondsLeft(1000, 1000, 20) == 20);
    assert(SecondsLeft(1000, 1001, 20) == 20);    // rounds up
    assert(SecondsLeft(1000, 6000, 20) == 15);
    assert(SecondsLeft(1000, 30000, 20) == 0);
    assert(SecondsLeft(1000, 30000, 0) == -1);
    assert(SecondsLeft(0xFFFFF000u, 0x00000400u, 20) == 15);   // 5.12 s across the wrap
    std::printf("waves ok\n");
}

coop_bot::BenchNode Node(uint32_t key, int type, bool start, uint32_t a = 0, uint32_t b = 0) {
    coop_bot::BenchNode n;
    n.key = key;
    n.type = type;
    n.start = start;
    n.links[0] = a;
    n.links[1] = b;
    return n;
}

void Upgrades() {
    // start(blank) - blank - DAMAGE ; start also links a RELOAD one step away
    coop_bot::BenchNode tree[4] = {Node(1, 0, true, 2, 4), Node(2, 0, false, 1, 3), Node(3, 1, false, 2), Node(4, 5, false, 1)};
    Plan p = PlanUpgrade(tree, 4);
    // The start blank first, then the reload next to it is worth more per node than damage two steps on.
    assert(p.nodes == 2 && p.order[0] == 0 && p.order[1] == 3 && p.type == 5);
    tree[0].filled = true; tree[3].filled = true;
    p = PlanUpgrade(tree, 4);
    assert(p.nodes == 2 && p.order[0] == 1 && p.order[1] == 2 && p.type == 1);
    tree[1].filled = true; tree[2].filled = true;
    p = PlanUpgrade(tree, 4);
    assert(p.nodes == 0 && p.type == 0);   // maxed
    // Only blanks left within reach: nothing to sell.
    coop_bot::BenchNode blanks[2] = {Node(1, 0, true, 2), Node(2, 0, false, 1)};
    p = PlanUpgrade(blanks, 2);
    assert(p.nodes == 0);
    // A stat beyond maxSteps blanks is not offered either.
    coop_bot::BenchNode chain[5] = {Node(1, 0, true, 2), Node(2, 0, false, 1, 3), Node(3, 0, false, 2, 4), Node(4, 0, false, 3, 5), Node(5, 7, false, 4)};
    assert(PlanUpgrade(chain, 5, 4).nodes == 0);
    p = PlanUpgrade(chain, 5, 6);
    assert(p.nodes == 5 && p.type == 7);
    assert(PlanUpgrade(nullptr, 3).nodes == 0);
    assert(std::strcmp(NodeTypeName(1), "DAMAGE") == 0 && std::strcmp(NodeTypeName(0), "LINK") == 0 && std::strcmp(NodeTypeName(7), "HP") == 0);
    std::printf("upgrades ok\n");
}

void UpgradesByStat() {
    // start(HP) - blank - AIR ; start also links HP one step away
    coop_bot::BenchNode rig[4] = {Node(1, 7, true, 2, 4), Node(2, 0, false, 1, 3), Node(3, 8, false, 2), Node(4, 7, false, 1)};
    int types[8];
    assert(TypesLeft(rig, 4, types, 8) == 2 && types[0] == 7 && types[1] == 8);
    assert(EmptyOfType(rig, 4, 7) == 2 && EmptyOfType(rig, 4, 8) == 1);
    // Air at once, without HP bought first: start, blank, air.
    Plan p = PlanToType(rig, 4, 8);
    assert(p.nodes == 3 && p.order[0] == 0 && p.order[1] == 1 && p.order[2] == 2 && p.type == 8);
    p = PlanToType(rig, 4, 7);
    assert(p.nodes == 1 && p.order[0] == 0 && p.type == 7);
    // Bought in order: every node lies next to one filled before it.
    for (int k = 0; k < 3; ++k) {
        const int n = PlanToType(rig, 4, 8).order[0];
        assert(coop_bot::BenchReachable(rig, 4, n));
        rig[n].filled = true;
    }
    assert(rig[2].filled && PlanToType(rig, 4, 8).nodes == 0);
    p = PlanToType(rig, 4, 7);
    assert(p.nodes == 1 && p.order[0] == 3);
    rig[3].filled = true;
    assert(TypesLeft(rig, 4, types, 8) == 0);
    // Beyond maxSteps, unknown stat, no tree.
    coop_bot::BenchNode chain[5] = {Node(1, 0, true, 2), Node(2, 0, false, 1, 3), Node(3, 0, false, 2, 4), Node(4, 0, false, 3, 5), Node(5, 7, false, 4)};
    assert(PlanToType(chain, 5, 7, 4).nodes == 0 && PlanToType(chain, 5, 7).nodes == 5);
    assert(PlanToType(chain, 5, 1).nodes == 0 && PlanToType(nullptr, 5, 7).nodes == 0 && PlanToType(chain, 5, 0).nodes == 0);
    std::printf("upgrades by stat ok\n");
}

void Text() {
    Metrics m;
    for (int i = 0; i < 95; ++i) m.advance[i] = 10;
    m.advance['i' - 32] = 4;
    m.advance['.' - 32] = 3;
    assert(Measure(m, "abc") == 30);
    assert(Measure(m, "ii") == 8);
    assert(Measure(m, "") == 0 && Measure(m, nullptr) == 0);
    assert(Advance(m, '\x7F') == m.advance['?' - 32]);
    char out[32];
    FitText(m, "abcdef", 60, out, sizeof(out)); assert(std::strcmp(out, "abcdef") == 0);
    FitText(m, "abcdefgh", 60, out, sizeof(out)); assert(std::strcmp(out, "abcde...") == 0 && Measure(m, out) <= 60);
    FitText(m, "abcdefgh", 5, out, sizeof(out)); assert(std::strcmp(out, "") == 0);
    FitText(m, "abcdefghijklmnopqrstuvwxyz", 1000, out, 8); assert(std::strlen(out) < 8);
    assert(GlyphPixels(1080, 100) == 24);
    assert(GlyphPixels(1440, 100) == 32);
    assert(GlyphPixels(2160, 100) == 48);
    assert(GlyphPixels(720, 100) == 16);
    assert(GlyphPixels(1080, 150) == 36);
    assert(GlyphPixels(1080, 10) == 12);      // scale floor 50 %, size floor 12
    assert(GlyphPixels(8640, 250) == 96);
    std::printf("text ok\n");
}

void Geometry() {
    const int heights[] = {720, 1080, 1440, 2160};
    const int widths[] = {1280, 1920, 2560, 3840};
    for (int i = 0; i < 4; ++i) {
        const int px = GlyphPixels(heights[i], 100);
        const Layout l = PanelLayout(widths[i], heights[i], px, px + px / 5, 10);
        assert(l.rows == 10);
        assert(l.x >= 0 && l.y >= 0 && l.x + l.w <= widths[i] && l.y + l.h <= heights[i]);
        assert(l.hintY + l.line <= l.y + l.h);
        assert(l.rowsY > l.tabsY && l.detailY >= l.rowsY + l.rows * l.line);
    }
    // A short screen loses rows, never the frame.
    const Layout s = PanelLayout(800, 300, 24, 28, 16);
    assert(s.rows >= 3 && s.rows < 16);
    assert(s.y + s.h <= 300 || s.rows == 3);
    // A narrow one keeps a usable width.
    const Layout n = PanelLayout(400, 1080, 24, 28, 10);
    assert(n.w >= 24 * 12);
    std::printf("geometry ok\n");
}

void Rows() {
    Item items[5];
    SetItem(items[0], Kind::Ammo, 0, "Plasma Cutter ammo", 300);
    SetItem(items[1], Kind::Gun, 1, "Pulse Rifle", 4000);
    SetItem(items[2], Kind::Gun, 2, "Line Gun", 5000);
    Refuse(items[2], "NOT LOADED");
    SetItem(items[3], Kind::MedSmall, -1, "Small med pack", 400);
    SetItem(items[4], Kind::MedLarge, -1, "Large med pack", 1800);
    View v{};
    BuildRows(items, 5, 1, 0, 3, 1000, v);
    assert(v.rowCount == 3 && v.rowCapacity == 3 && !v.above && v.below);
    assert(v.rows[0].style == kRowNormal && std::strcmp(v.rows[0].right, "300") == 0);
    assert(v.rows[1].style == (kRowTooPoor | kRowSelected) && std::strcmp(v.rows[1].right, "4,000") == 0);
    assert(v.rows[2].style == kRowUnavailable && std::strcmp(v.rows[2].right, "NOT LOADED") == 0);
    BuildRows(items, 5, 4, 2, 3, 5000, v);
    assert(v.rowCount == 3 && v.above && !v.below);
    assert(v.rows[2].style == (kRowNormal | kRowSelected) && std::strcmp(v.rows[2].label, "Large med pack") == 0);
    BuildRows(items, 0, 0, 0, 3, 5000, v);
    assert(v.rowCount == 0 && !v.above && !v.below);
    BuildRows(items, 5, 0, 0, 99, 5000, v);
    assert(v.rowCapacity == kMaxRows && v.rowCount == 5);
    std::printf("rows ok\n");
}
}  // namespace

int main() {
    Prices();
    Catalogue();
    Navigation();
    Keys();
    Gate();
    Waves();
    Upgrades();
    UpgradesByStat();
    Text();
    Geometry();
    Rows();
    std::printf("all coop_shop tests passed\n");
    return 0;
}
