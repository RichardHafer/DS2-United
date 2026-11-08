// Dead Space 2 United by Hafer.
#pragma once
// Pure logic of the campaign arenas (runtime_coop_arena.inc): names, the
// arena area test and the member list merge. The arenas themselves are in
// generated_arenas.h (tools/build_arenas.py). No game memory is touched here.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "coop_mpmap.h"
#include "generated_arenas.h"
#include "generated_arena_points.h"

namespace coop_arena {
using ds2arena::Arena;
using ds2arena::ArenaPoints;
using ds2arena::kArenaCount;
using ds2arena::kArenas;

// "arena_<something>": the picker entry's map token and the survival map name.
inline bool IsArenaName(const char* name) { return coop_mpmap::StartsWithNoCase(name, "arena_") && name[6] != 0; }
inline int ArenaIndex(const char* name) {
    for (unsigned i = 0; i < kArenaCount; ++i) if (coop_mpmap::EqualNoCase(name, kArenas[i].name)) return static_cast<int>(i);
    return -1;
}

// Inside the arena: within the walk's box (plus a margin) and close to a walk
// sample on about the same floor. The walk covers every room the arena holds,
// so a spot that passes is in one of them and not behind a locked door.
// 5 m (was 7): tighter, so a spot next to the walk is less likely behind a wall.
struct Reach { float horizontal = 5.0f, vertical = 2.5f, margin = 1.5f; };
inline bool Inside(const Arena& a, const float* p, const Reach& r = Reach{}) {
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    for (unsigned k = 0; k < 3; ++k)
        if (p[k] < a.box[k] - r.margin - (k == 1 ? r.vertical : 0.0f) || p[k] > a.box[3 + k] + r.margin + (k == 1 ? r.vertical : 0.0f)) return false;
    const float h2 = r.horizontal * r.horizontal;
    for (unsigned i = 0; i < a.walkCount; ++i) {
        const float* w = a.walk[i];
        const float dx = p[0] - w[0], dz = p[2] - w[2];
        if (dx * dx + dz * dz <= h2 && std::fabs(p[1] - w[1]) <= r.vertical) return true;
    }
    return false;
}

inline bool SameGuid(const uint32_t* a, const uint32_t* b) { return std::memcmp(a, b, 16) == 0; }
inline int Find(const uint32_t (*list)[4], unsigned count, const uint32_t* guid) {
    for (unsigned i = 0; i < count; ++i) if (SameGuid(list[i], guid)) return static_cast<int>(i);
    return -1;
}
// A set switch inside the arena: every arena member joins the desired list and
// none of them loads hidden. Returns how many entries changed; `full` counts
// the members that found no room.
struct Merge { unsigned added = 0, unhidden = 0, full = 0; };
// selected: only those members (streaming=sets), null for all of them.
inline Merge MergeMembers(const Arena& a, uint32_t (*list)[4], unsigned char* hidden, unsigned& count, unsigned max, const bool* selected = nullptr) {
    Merge m;
    for (unsigned i = 0; i < a.memberCount; ++i) {
        if (selected && !selected[i]) continue;
        const int at = Find(list, count, a.members[i]);
        if (at >= 0) {
            if (hidden[at]) { hidden[at] = 0; ++m.unhidden; }
            continue;
        }
        if (count >= max) { ++m.full; continue; }
        std::memcpy(list[count], a.members[i], 16);
        hidden[count] = 0;
        ++count;
        ++m.added;
    }
    return m;
}

// The arena's story logic (sorted by GUID): 0 not listed, 1 not created, 2 the
// record stays and the spawn filter refuses the entity.
inline int BlockKind(const Arena& a, const uint32_t* guid, const char** cls = nullptr) {
    unsigned lo = 0, hi = a.blockCount;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2;
        const int c = coop_mpmap::Compare(a.blocks[mid].guid, guid);
        if (c == 0) {
            if (cls) *cls = a.blocks[mid].cls;
            return a.blocks[mid].kind;
        }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

// streaming=sets: the stream set that was active where the walk passed nearest to
// `p` (a metre of height counts three, so another storey is far); -1 for none.
inline int SetForPoint(const Arena& a, const float* p) {
    int best = -1;
    float bestCost = 1e30f;
    for (unsigned i = 0; a.walkSet && i < a.walkCount; ++i) {
        if (a.walkSet[i] >= a.setCount) continue;
        const float* w = a.walk[i];
        const float dx = p[0] - w[0], dy = (p[1] - w[1]) * 3.0f, dz = p[2] - w[2];
        const float cost = dx * dx + dy * dy + dz * dz;
        if (cost < bestCost) { bestCost = cost; best = a.walkSet[i]; }
    }
    return best;
}
// The members the sets in `mask` hold (bit = set index); returns how many.
inline unsigned SelectMembers(const Arena& a, uint32_t mask, bool* selected, unsigned max) {
    unsigned n = 0;
    for (unsigned i = 0; i < a.memberCount && i < max; ++i) selected[i] = false;
    for (unsigned s = 0; a.setStart && s < a.setCount && s < 32; ++s) {
        if (!(mask >> s & 1u)) continue;
        for (unsigned k = a.setStart[s]; k < a.setStart[s + 1]; ++k) {
            const unsigned m = a.setMember[k];
            if (m < a.memberCount && m < max && !selected[m]) { selected[m] = true; ++n; }
        }
    }
    return n;
}

// streaming=sets: each set's cost from `p`, the nearest walk sample labelled with it
// (squared metres on the floor plan, a metre of height counting three); 1e30 for none.
inline void SetCosts(const Arena& a, const float* p, float* cost, unsigned max) {
    for (unsigned s = 0; s < max; ++s) cost[s] = 1e30f;
    for (unsigned i = 0; a.walkSet && i < a.walkCount; ++i) {
        const unsigned s = a.walkSet[i];
        if (s >= a.setCount || s >= max) continue;
        const float* w = a.walk[i];
        const float dx = p[0] - w[0], dy = (p[1] - w[1]) * 3.0f, dz = p[2] - w[2];
        const float c = dx * dx + dy * dy + dz * dz;
        if (c < cost[s]) cost[s] = c;
    }
}
// The sets to keep: `first` (the local Isaac's own set, -1 none) and `forced` (a spawn spot's
// set that must load first, -1 none), then the others whose cost is below `reach2` or that
// are still held, nearest first, while their members fit in `roomLimit`. `held` bits stay
// wanted although out of reach.
inline uint32_t PickSets(const Arena& a, const float* cost, int first, float reach2, uint32_t held, unsigned roomLimit, int forced = -1) {
    bool scratch[256];
    uint32_t mask = first >= 0 && first < 32 ? 1u << first : 0u;
    if (forced >= 0 && forced < 32 && static_cast<unsigned>(forced) < a.setCount) mask |= 1u << forced;
    for (;;) {
        int best = -1;
        for (unsigned s = 0; s < a.setCount && s < 32; ++s) {
            if (mask >> s & 1u) continue;
            if (cost[s] > reach2 && !(held >> s & 1u)) continue;
            if (best < 0 || cost[s] < cost[best]) best = static_cast<int>(s);
        }
        if (best < 0) return mask;
        const uint32_t with = mask | 1u << best;
        if (mask && SelectMembers(a, with, scratch, 256) > roomLimit) return mask;
        mask = with;
    }
}
// The recorded spots (End key) are the players' spawn points (user, 16.10.): solo a random one
// at the start, in co-op the free one nearest to a partner already on a spot, and after a
// death the free one nearest to where he died. Distances count a metre of height as three.
inline float SpotCost(const float* s, const float* p) {
    const float dx = s[0] - p[0], dy = (s[1] - p[1]) * 3.0f, dz = s[2] - p[2];
    return dx * dx + dy * dy + dz * dz;
}
// The spot someone at `p` stands on (within `within` m on the floor plan, 2 m in height); -1 for none.
inline int SpotAt(const Arena& a, const float* p, float within) {
    for (unsigned i = 0; a.spots && i < a.spotCount; ++i) {
        const float* s = a.spots[i];
        const float dx = s[0] - p[0], dz = s[2] - p[2];
        if (dx * dx + dz * dz <= within * within && std::fabs(s[1] - p[1]) <= 2.0f) return static_cast<int>(i);
    }
    return -1;
}
// The spot nearest to `p` that nobody in `taken` (x, y, z triples) stands on within `clear` m;
// with every spot taken the nearest one and `shared` set. -1 without spots.
inline int FreeSpotNear(const Arena& a, const float* p, const float* taken, unsigned takenCount, float clear, bool& shared) {
    shared = false;
    int best = -1, nearest = -1;
    float bestCost = 1e30f, nearestCost = 1e30f;
    for (unsigned i = 0; a.spots && i < a.spotCount; ++i) {
        const float* s = a.spots[i];
        const float cost = SpotCost(s, p);
        if (cost < nearestCost) { nearestCost = cost; nearest = static_cast<int>(i); }
        bool used = false;
        for (unsigned k = 0; k < takenCount && !used; ++k) {
            const float* t = taken + 3 * k;
            const float dx = s[0] - t[0], dz = s[2] - t[2];
            used = dx * dx + dz * dz <= clear * clear && std::fabs(s[1] - t[1]) <= 2.0f;
        }
        if (!used && cost < bestCost) { bestCost = cost; best = static_cast<int>(i); }
    }
    if (best < 0 && nearest >= 0) shared = true;
    return best >= 0 ? best : nearest;
}

// The spot after `last`, so respawns and seeds take turns.
inline unsigned NextSpot(const Arena& a, unsigned last) { return a.spotCount ? (last + 1) % a.spotCount : 0; }

// The game modes' points of an arena (generated_arena_points.h); null when it has none.
inline const ArenaPoints* PointsFor(const char* name) {
    for (unsigned i = 0; name && i < ds2arena::kArenaPointsCount; ++i)
        if (coop_mpmap::EqualNoCase(name, ds2arena::kArenaPoints[i].name)) return &ds2arena::kArenaPoints[i];
    return nullptr;
}
// The mode an arena plays when `picked` was chosen: picked if the arena has its data, else survival (0).
inline int ModeFor(const ArenaPoints* p, int picked) {
    return p && picked > 0 && picked < 8 && (p->modes >> picked & 1u) ? picked : 0;
}
// The next mode the arena can play after `from`, stepping up (+1) or down (-1) through `count` modes.
inline int StepMode(const ArenaPoints* p, int from, int step, int count) {
    int m = from;
    for (int i = 0; i < count; ++i) {
        m = ((m + step) % count + count) % count;
        if (m == 0 || ModeFor(p, m) == m) return m;
    }
    return 0;
}
// A door's mark from the game (recorder DoorKey): 0 none (the door table decides), 1 inner, 2 locked, 3 held open.
enum DoorState : uint8_t { kDoorAuto = 0, kDoorInner = 1, kDoorLocked = 2, kDoorHeld = 3 };
inline const char* DoorStateName(int s) { return s == kDoorInner ? "inner" : s == kDoorLocked ? "locked" : s == kDoorHeld ? "held" : "auto"; }
inline int DoorMarkFor(const ArenaPoints* p, const uint32_t* guid) {
    unsigned lo = 0, hi = p ? p->doorMarkCount : 0;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2;
        const int c = coop_mpmap::Compare(p->doorMarks[mid].guid, guid);
        if (c == 0) return p->doorMarks[mid].state;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return kDoorAuto;
}
// The next mark when the door key is pressed again: auto, inner, locked, held, auto ...
inline int NextDoorState(int s) { return (s + 1) % 4; }
// The marks a running arena saved (plugins\DS2Arenas\doors\<arena>.txt, a
// "doormark <GUID> state=<name> ..." line per key press): the last line of a door wins.
struct SavedMark { uint32_t guid[4]; int state; };
inline unsigned ParseSavedMarks(const char* text, SavedMark* out, unsigned max) {
    unsigned n = 0;
    for (const char* line = text; line && *line;) {
        const char* end = std::strchr(line, '\n');
        unsigned g[4]{};
        char name[16] = "";
        if (std::strncmp(line, "doormark ", 9) == 0 &&
            sscanf_s(line + 9, "%8x-%8x-%8x-%8x state=%15[a-z]", &g[0], &g[1], &g[2], &g[3], name, static_cast<unsigned>(sizeof(name))) == 5) {
            const int state = std::strcmp(name, "inner") == 0    ? kDoorInner
                              : std::strcmp(name, "locked") == 0 ? kDoorLocked
                              : std::strcmp(name, "held") == 0   ? kDoorHeld
                              : std::strcmp(name, "auto") == 0   ? kDoorAuto
                                                                 : -1;
            unsigned i = 0;
            while (i < n && !(out[i].guid[0] == g[0] && out[i].guid[1] == g[1] && out[i].guid[2] == g[2] && out[i].guid[3] == g[3])) ++i;
            if (state >= 0 && (i < n || n < max)) {
                for (int k = 0; k < 4; ++k) out[i].guid[k] = g[k];
                out[i].state = state;
                if (i == n) ++n;
            }
        }
        line = end ? end + 1 : nullptr;
    }
    return n;
}
// The rule a door follows: its mark, else the door table; doors=open holds every door that is not locked.
inline int DoorRule(const Arena& a, int mark, bool tableLock) {
    const int rule = mark != kDoorAuto ? mark : tableLock ? kDoorLocked : kDoorInner;
    return rule == kDoorInner && a.doorsOpen ? kDoorHeld : rule;
}
// exclude= in the arena file: a survival family the director never spawns there.
inline bool Excludes(const Arena& a, const char* family) {
    const size_t n = family ? std::strlen(family) : 0;
    for (const char* p = a.exclude; p && *p && n;) {
        const char* end = std::strchr(p, ',');
        const size_t len = end ? static_cast<size_t>(end - p) : std::strlen(p);
        if (len == n && coop_mpmap::StartsWithNoCase(p, family)) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}
enum PointKind { kZones = 0, kCapsules = 1, kRespawns = 2 };
inline const ds2arena::Point* PointList(const ArenaPoints& p, int kind, unsigned& count) {
    count = kind == kZones ? p.zoneCount : kind == kCapsules ? p.capsuleCount : kind == kRespawns ? p.respawnCount : 0;
    return kind == kZones ? p.zones : kind == kCapsules ? p.capsules : kind == kRespawns ? p.respawns : nullptr;
}
// The respawn point nearest to `toward` (a metre of height counts as three on the floor
// plan, so another storey is far); the first one without a target. -1 when there is none.
inline int NearestRespawn(const ArenaPoints& p, const float* toward) {
    if (!p.respawnCount) return -1;
    if (!toward) return 0;
    int best = 0;
    float bestCost = 1e30f;
    for (unsigned i = 0; i < p.respawnCount; ++i) {
        const float* q = p.respawns[i].pos;
        const float dx = q[0] - toward[0], dy = q[1] - toward[1], dz = q[2] - toward[2];
        const float cost = dx * dx + dz * dz + 9.0f * dy * dy;
        if (cost < bestCost) { bestCost = cost; best = static_cast<int>(i); }
    }
    return best;
}
// Behind a locked door (user, 16.10.: enemies came from behind the mining hall's locked
// door): within 6 m of a door that stays locked (its mark, else the door table) and on
// the side the arena is not on. Doors whose side is unknown count for nothing.
inline bool BehindLockedDoor(const Arena& a, const ArenaPoints* points, const float* p) {
    for (unsigned i = 0; a.doors && i < a.doorCount; ++i) {
        const ds2arena::Door& d = a.doors[i];
        if (!d.inside || DoorRule(a, DoorMarkFor(points, d.guid), d.lock) != kDoorLocked) continue;
        const float dx = p[0] - d.pos[0], dz = p[2] - d.pos[2];
        if (dx * dx + dz * dz > 36.0f || std::fabs(p[1] - d.pos[1]) > 3.0f) continue;
        if ((dx * d.axis[0] + dz * d.axis[1]) * static_cast<float>(d.inside) < -0.3f) return true;
    }
    return false;
}
}  // namespace coop_arena
