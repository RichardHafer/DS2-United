// Dead Space 2 United by Richard Hafer.
#pragma once
// Survival music on the MP maps (runtime_coop_survmusic.inc): pure logic, no
// game memory. The original MP round music: eight PlayAudioStreams in
// global_mp_assets, started by message and cut after the time the MP cue
// MultiManagers use. A wave starts a combat track, then action and combat
// alternate as in an MP round; a cleared wave stops it after a short delay.
// analysis/DS2_SURVIVAL_MUSIC_2026-10-09.md
#include <cstdint>

namespace coop_survmusic {

// aud_play_stream_mus_01..08: 0-3 mx_mp_action_01..04, 4-7 mx_mp_combat_01..04
constexpr uint32_t kPlay[8] = {0x9160B958u, 0x9160B959u, 0x9160B95Au, 0x9160B95Bu, 0x9160B95Cu, 0x9160B95Du, 0x9160B95Eu, 0x9160B95Fu};
constexpr uint32_t kStop = 0xBB5EE091u;   // aud_stop_current_music
constexpr uint32_t kCutMs[8] = {118000, 99000, 123000, 114000, 117000, 101000, 117000, 117000};

constexpr uint32_t NameHash(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) h = h * 0x1003Fu + static_cast<unsigned char>(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
    return h;
}
// The campaign's music sections (aud_global_music.lua in global_assets, alive on
// every MP map): one message per section, the conductor stops the current one and
// starts this one on its other music voice, and the layers follow the fear level.
// The source when the eight MP PlayAudioStreams are not on the map: they never
// spawned in a visit (13.10.: every play in every log had no listener). There is
// no stop message, so a calm section replaces the wave's between the waves.
constexpr uint32_t kSectionWave[4] = {NameHash("mus_action_01"), NameHash("mus_action_02"), NameHash("mus_action_03"), NameHash("mus_action_04")};
constexpr const char* kSectionWaveNames[4] = {"mus_action_01", "mus_action_02", "mus_action_03", "mus_action_04"};
constexpr uint32_t kSectionCalm[6] = {NameHash("mus_tension_01"), NameHash("mus_tension_02"), NameHash("mus_tension_03"),
                                      NameHash("mus_tension_04"), NameHash("mus_tension_05"), NameHash("mus_tension_06")};
constexpr const char* kSectionCalmNames[6] = {"mus_tension_01", "mus_tension_02", "mus_tension_03", "mus_tension_04", "mus_tension_05", "mus_tension_06"};
static_assert(kSectionWave[0] != kPlay[0] && kSectionCalm[0] != kStop, "distinct names");

// Every name this module sends: the co-op mirror keeps them on each machine.
inline bool IsMusicHash(uint32_t hash) {
    if ((hash >= kPlay[0] && hash <= kPlay[7]) || hash == kStop) return true;
    for (uint32_t h : kSectionWave) if (h == hash) return true;
    for (uint32_t h : kSectionCalm) if (h == hash) return true;
    return false;
}

struct Action { enum Kind { None, Play, Stop } kind = None; int track = -1; };

struct Player {
    bool playing = false;
    int track = -1;
    int lastAction = -1, lastCombat = -1;
    bool combatNext = true;
    uint32_t trackAt = 0, stopAt = 0, rng = 0x9E3779B9u;

    int Pick(bool combat) {
        rng = rng * 1664525u + 1013904223u;
        const int first = combat ? 4 : 0;
        const int avoid = combat ? lastCombat : lastAction;
        int pick = first + static_cast<int>((rng >> 16) % 4u);
        if (pick == avoid) pick = first + (pick - first + 1) % 4;
        (combat ? lastCombat : lastAction) = pick;
        return pick;
    }
    // One step: what to send now. A Play is always preceded by the caller's
    // stop of whatever plays (the MP cues do the same).
    Action Update(bool wave, uint32_t now, uint32_t stopDelayMs) {
        Action a;
        if (wave) {
            stopAt = 0;
            if (!playing) {
                combatNext = true;
                track = Pick(true);
                combatNext = false;
                playing = true;
                trackAt = now;
                a.kind = Action::Play;
                a.track = track;
                return a;
            }
            if (track >= 0 && now - trackAt >= kCutMs[track]) {
                track = Pick(combatNext);
                combatNext = !combatNext;
                trackAt = now;
                a.kind = Action::Play;
                a.track = track;
            }
            return a;
        }
        if (!playing) return a;
        if (!stopAt) { stopAt = now + stopDelayMs; if (!stopAt) stopAt = 1; }
        if (static_cast<int32_t>(now - stopAt) >= 0) {
            playing = false;
            track = -1;
            stopAt = 0;
            a.kind = Action::Stop;
        }
        return a;
    }
    // The map went away: its streams stop with their entities.
    void Forget() { playing = false; track = -1; stopAt = 0; }
};

}  // namespace coop_survmusic
