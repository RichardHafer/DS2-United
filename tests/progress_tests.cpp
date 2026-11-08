// Dead Space 2 United by Richard Hafer.
// Profile and weapon progression without a game: gun lookup, weapon levels and
// tiers, the profile rows, XP amounts, payment dedup and the progress percent.
#include <cassert>
#include <cmath>
#include <cstdio>
#include "../src/coop_progress.h"
#include "../src/coop_mpnative.h"

using namespace coop_progress;

namespace {
void Guns() {
    assert(GunByInflictor(0x20 | 0x2) == 0);           // Plasma Cutter, RANGED bit beside it
    assert(GunByInflictor(0x400) == GunByKey("Ripper"));
    assert(GunByInflictor(0x8000) == GunByKey("SeekerRifle"));
    assert(GunByInflictor(0x2 | 0x1) == -1);           // necro melee and ranged, no gun
    assert(GunByKey("nope") == -1 && GunByKey(nullptr) == -1);
    for (int i = 0; i < kGunCount; ++i)
        for (int j = i + 1; j < kGunCount; ++j) assert(!(kGuns[i].inflictor & kGuns[j].inflictor));
    std::printf("guns ok\n");
}

void Levels() {
    const Rules r;
    assert(WeaponLevel(r, 0) == 1 && WeaponLevel(r, 24) == 1 && WeaponLevel(r, 25) == 2);
    assert(WeaponLevel(r, 1499) == 9 && WeaponLevel(r, 1500) == 10 && WeaponLevel(r, 99999) == 10);
    assert(KillsForNext(r, 1) == 25 && KillsForNext(r, 9) == 1500 && KillsForNext(r, 10) == 0);
    assert(ClipTier(r, 2) == 0 && ClipTier(r, 3) == 1 && ClipTier(r, 7) == 2);
    assert(DamageTier(r, 4) == 0 && DamageTier(r, 5) == 1 && DamageTier(r, 10) == 2);
    assert(ClipFactor(r, 1) == 1.0f && std::fabs(ClipFactor(r, 3) - 1.25f) < 1e-6f && std::fabs(ClipFactor(r, 9) - 1.5f) < 1e-6f);
    assert(std::fabs(DamageFactor(r, 10) - 1.2f) < 1e-6f);
    assert(BoostedClip(10, 1.25f) == 12 && BoostedClip(1, 1.25f) == 2 && BoostedClip(10, 1.0f) == 10 && BoostedClip(0, 1.5f) == 0);
    assert(BoostedClip(25, 1.5f) == 37);
    uint32_t kills[9]{};
    assert(ParseRising("10, 20,30,40,50,60,70,80,90", kills, 9) && kills[0] == 10 && kills[8] == 90);
    assert(!ParseRising("10,20", kills, 9) && kills[0] == 10);
    assert(!ParseRising("10,10,30,40,50,60,70,80,90", kills, 9));
    assert(!ParseRising("x", kills, 9));
    std::printf("levels ok\n");
}

void Rows() {
    assert(ProfileUpgrade(5, 60) == 1.0f && ProfileUpgrade(13, 60) == 1.0f);   // gun rows: weapon levels
    assert(ProfileUpgrade(29, 1) == 1.5f);                                      // stasis for everybody
    assert(ProfileUpgrade(30, 4) == 1.0f && ProfileUpgrade(30, 5) == 2.0f);
    assert(ProfileUpgrade(25, 42) == 1.0f && std::fabs(ProfileUpgrade(25, 43) - 1.2f) < 1e-6f);
    assert(ProfileUpgrade(15, 6) > 1.0f && ProfileUpgrade(33, 60) == 1.0f && ProfileUpgrade(-1, 60) == 1.0f);
    std::printf("rows ok\n");
}

void Xp() {
    const XpRules r;
    assert(WaveXp(r, 1, false) == 100 && WaveXp(r, 10, true) == 3000 && WaveXp(r, 60, false) == 4000);
    assert(KillXp(r, KillSize::Normal, false) == 100 && KillXp(r, KillSize::Heavy, true) == 600 && KillXp(r, KillSize::Swarm, false) == 25);
    assert(EventOf(Flags(kZoneHeld, true)) == kZoneHeld && (Flags(kZoneHeld, true) & kTaintHost));
    assert(!(Flags(kWaveEnd, false) & kTaintHost));
    Paid p;
    assert(p.Take(3, 1, kWaveEnd) && !p.Take(3, 1, kWaveEnd) && p.Take(3, 1, kRunEnd) && p.Take(4, 1, kWaveEnd));
    for (unsigned w = 2; w < 70; ++w) assert(p.Take(5, w, kWaveEnd));
    assert(p.Take(3, 1, kWaveEnd));   // fell out of the 64
    std::printf("xp ok\n");
}

void Percent() {
    using coop_mpnative::kLevelScore;
    assert(PercentToNext(0, kLevelScore, 60) == 0 && PercentToNext(2500, kLevelScore, 60) == 50);
    assert(PercentToNext(5000, kLevelScore, 60) == 0 && PercentToNext(2224750, kLevelScore, 60) == 100);
    std::printf("percent ok\n");
}

void Unlocks() {
    assert(DefaultUnlock("HandCannon") == 60 && DefaultUnlock("LineGun") == 7 && DefaultUnlock("Turret") == 8 && DefaultUnlock("nope") == 0);
    assert(!std::strcmp(BackendKey(0), "PulseRifle") && !std::strcmp(BackendKey(11), "HandCannon") && !BackendKey(12) && !BackendKey(-1));
    for (int b = 1; b < kBackendRows; ++b) assert(GunByKey(BackendKey(b)) >= 0 || !std::strcmp(BackendKey(b), "HandCannon"));
    std::printf("unlocks ok\n");
}
}  // namespace

int main() {
    Guns();
    Levels();
    Rows();
    Xp();
    Percent();
    Unlocks();
    std::printf("progress tests passed\n");
    return 0;
}
