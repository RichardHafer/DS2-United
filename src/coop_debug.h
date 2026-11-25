// Dead Space 2 United by Richard Hafer.
#pragma once
// Pure logic of the debug menu (runtime_coop_debugmenu.inc): tabs, the cursor,
// the checkpoint rows from map.txt (generated_checkpoints.h), the cheat rows and
// the second-press confirmation. No game memory is touched here.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "generated_checkpoints.h"

namespace coop_debug {
using ds2checkpoints::kChapterCount;
using ds2checkpoints::kChapters;
using ds2checkpoints::kCheckpointCount;
using ds2checkpoints::kCheckpoints;

enum Tab : int { kLevels, kWeapons, kSupplies, kSuits, kCheats, kHost, kTabCount };
inline const char* TabName(int t) {
    static const char* const kNames[kTabCount] = {"LEVELS", "WEAPONS", "SUPPLIES", "SUITS", "CHEATS", "HOST"};
    return t >= 0 && t < kTabCount ? kNames[t] : "?";
}

// --- the HOST tab: the director's tools, then the ones every machine applies ---
enum HostRow : int {
    kStartRun, kJumpWave, kEndWave, kSpawnEnemy, kHoldDirector, kMaxAlive, kRestartRun, kKillAll,
    kHealAll, kReviveAll, kSupplyAll, kCreditsAll, kGatherAll, kGodAll, kRestartCheckpoint, kHostRowCount
};
inline const char* HostRowName(int r) {
    static const char* const kNames[kHostRowCount] = {
        "Start the run now", "Jump to wave", "End this wave / skip the break", "Spawn next", "Director hold", "Max alive", "Restart the run",
        "Kill all enemies", "Heal everyone", "Respawn the downed now", "Supply drop for everyone", "Credits for everyone", "Everyone to me",
        "God mode for everyone", "Restart at the checkpoint"};
    return r >= 0 && r < kHostRowCount ? kNames[r] : "?";
}
// The director's half goes through DS2Survival_Admin; the rest is applied on every machine.
inline bool HostRowIsDirector(int r) { return r >= kStartRun && r <= kRestartRun; }
inline bool HostRowIsEveryone(int r) { return r >= kHealAll && r <= kGodAll; }
inline bool HostRowNeedsConfirm(int r) { return r == kJumpWave || r == kRestartRun || r == kKillAll || r == kRestartCheckpoint; }
// The rows Q/E change instead of paging.
inline bool HostRowHasValue(int r) { return r == kJumpWave || r == kSpawnEnemy || r == kMaxAlive; }
struct HostValues {
    int wave = 5, family = 0, maxAlive = 0;
};
// Q/E on a value row; `families` is how many kinds the director offers.
inline void StepHostValue(HostValues& v, int row, int d, int families) {
    if (row == kJumpWave) {
        v.wave += d;
        if (v.wave < 1) v.wave = 1;
        if (v.wave > 999) v.wave = 999;
    } else if (row == kSpawnEnemy && families > 0) {
        v.family = ((v.family + d) % families + families) % families;
    } else if (row == kMaxAlive) {
        v.maxAlive += d;
        if (v.maxAlive < 0) v.maxAlive = 0;
        if (v.maxAlive > 40) v.maxAlive = 40;
    }
}
// The wire commands of the tools every machine applies (SVNT kind 12).
// kCmdJump: the host's LEVELS jump, value = checkpoint row, pos = its GUID's first three words.
enum EveryoneCommand : int { kCmdHeal = 1, kCmdRevive, kCmdSupply, kCmdCredits, kCmdGather, kCmdGod, kCmdJump };
inline int EveryoneCommandFor(int row) {
    switch (row) {
    case kHealAll: return kCmdHeal;
    case kReviveAll: return kCmdRevive;
    case kSupplyAll: return kCmdSupply;
    case kCreditsAll: return kCmdCredits;
    case kGatherAll: return kCmdGather;
    case kGodAll: return kCmdGod;
    default: return 0;
    }
}

enum Cheat : int { kGodMode, kInfiniteStasis, kShowEnemies, kZeroG, kFullHealth, kRefillStasis, kAddCredits, kAddNodes, kVoiceTest, kCheatCount };
inline const char* CheatName(int c) {
    static const char* const kNames[kCheatCount] = {"God mode", "Infinite stasis", "Show all enemies", "Zero G", "Full health", "Refill stasis", "Add credits", "Add power nodes",
                                                    "Voice test"};
    return c >= 0 && c < kCheatCount ? kNames[c] : "?";
}
inline bool CheatIsToggle(int c) { return c == kGodMode || c == kInfiniteStasis || c == kShowEnemies; }
// Zero G steps AUTO (the map's own gravity) -> ON -> OFF (gravity in zero-G rooms) -> AUTO.
enum ZeroGMode : int { kZeroGAuto, kZeroGOn, kZeroGOff, kZeroGModeCount };
inline int NextZeroG(int m) { return m >= 0 && m + 1 < kZeroGModeCount ? m + 1 : kZeroGAuto; }
inline const char* ZeroGName(int m) { return m == kZeroGOn ? "ON" : m == kZeroGOff ? "OFF" : "AUTO"; }
// What the movement component gets: whether its vector is replaced by zero or by `gravity`.
enum ZeroGAction : int { kZeroGLeave, kZeroGClear, kZeroGWeight };
inline ZeroGAction ZeroGFor(int mode, float fieldLength) {
    if (mode == kZeroGOn) return kZeroGClear;
    if (mode == kZeroGOff && !(fieldLength > 0.1f)) return kZeroGWeight;
    return kZeroGLeave;
}
// Usable without a local Isaac of one's own.
inline bool CheatNeedsNoIsaac(int c) { return CheatIsToggle(c) || c == kVoiceTest; }

struct Cursor {
    int tab = kLevels;
    int row[kTabCount]{};
    int top[kTabCount]{};
};
inline void StepTab(Cursor& c, int d) { c.tab = ((c.tab + d) % kTabCount + kTabCount) % kTabCount; }
// Rows wrap around at both ends.
inline void StepRow(Cursor& c, int d, int count) {
    if (count <= 0) { c.row[c.tab] = 0; return; }
    c.row[c.tab] = ((c.row[c.tab] + d) % count + count) % count;
}
// The window of `visible` rows keeps the selection in view.
inline void Fit(Cursor& c, int count, int visible) {
    int& row = c.row[c.tab];
    int& top = c.top[c.tab];
    if (count <= 0) { row = top = 0; return; }
    if (row >= count) row = count - 1;
    if (row < 0) row = 0;
    if (visible < 1) visible = 1;
    if (row < top) top = row;
    if (row >= top + visible) top = row - visible + 1;
    if (top > count - visible) top = count - visible > 0 ? count - visible : 0;
    if (top < 0) top = 0;
}

// --- checkpoints ---
// "CH04  stalker room debug": the chapter label and the name with spaces.
inline void CheckpointLabel(unsigned index, char* out, size_t size) {
    if (!out || !size) return;
    if (index >= kCheckpointCount) { std::snprintf(out, size, "?"); return; }
    const auto& cp = kCheckpoints[index];
    const char* chapter = cp.chapter < kChapterCount ? kChapters[cp.chapter].label : "?";
    size_t n = static_cast<size_t>(std::snprintf(out, size, "%s  ", chapter));
    if (n >= size) return;
    for (const char* s = cp.name; *s && n + 1 < size; ++s) out[n++] = *s == '_' ? ' ' : *s;
    out[n] = 0;
}
// The first row of the next (dir > 0) or the previous chapter.
inline int ChapterJump(int row, int dir) {
    if (row < 0 || static_cast<unsigned>(row) >= kCheckpointCount) return 0;
    const unsigned here = kCheckpoints[row].chapter;
    if (dir > 0) {
        for (unsigned i = static_cast<unsigned>(row) + 1; i < kCheckpointCount; ++i)
            if (kCheckpoints[i].chapter != here) return static_cast<int>(i);
        return 0;
    }
    const unsigned first = kChapters[here].first;
    if (static_cast<unsigned>(row) != first) return static_cast<int>(first);
    const unsigned prev = here ? here - 1 : kChapterCount - 1;
    return static_cast<int>(kChapters[prev].first);
}

// A second press on the same row within the window confirms.
struct Confirm {
    int tab = -1, row = -1;
    uint32_t at = 0;
    bool Press(int t, int r, uint32_t now, uint32_t windowMs) {
        const bool second = t == tab && r == row && now - at <= windowMs;
        if (second) { tab = row = -1; at = 0; return true; }
        tab = t;
        row = r;
        at = now;
        return false;
    }
    bool Armed(int t, int r, uint32_t now, uint32_t windowMs) const { return t == tab && r == row && now - at <= windowMs; }
};
}  // namespace coop_debug
