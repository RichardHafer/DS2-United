// Dead Space 2 United by Hafer.
#pragma once
// Who comes back when on the MP maps (runtime_coop_respawn.inc): pure logic, no
// game memory.
// - Solo, and the last human standing: [Respawn] Lives per wave, back after DelayMs.
// - Another human still up during a wave: back once that wave is cleared.
// - Nobody up and no life left: down until the host's director ends the run
//   (game over) and arms the next one. The level is never restarted.
// - Outside a wave (no run, arming, the break): back after DelayMs, no life used.
// Bots never count as humans.
#include <cstdint>

namespace coop_respawn {

// The director's state as survmusic hears it (the host's, on a client).
// Clock: a Mercenaries run; time is the only limit, so a death is back after the delay.
enum Survival : int { Off = 0, Arming = 1, Wave = 2, Intermission = 3, GameOver = 4, Clock = 5 };
enum class Mode : uint8_t { None, Timed, Free, WaitWave, Down };
// What a machine tells the others about its own Isaac.
enum Life : uint8_t { Absent = 0, Up = 1, Returning = 2, Waiting = 3 };

inline Life LifeOf(Mode m) {
    switch (m) {
    case Mode::Timed:
    case Mode::Free: return Returning;
    case Mode::WaitWave:
    case Mode::Down: return Waiting;
    default: return Up;
    }
}
inline const char* ModeName(Mode m) {
    switch (m) {
    case Mode::Timed: return "timed";
    case Mode::Free: return "free";
    case Mode::WaitWave: return "wait_wave";
    case Mode::Down: return "down";
    default: return "none";
    }
}

// The others as one machine sees them: up = up or back on a timer, known = on the map at all.
struct Others { unsigned up = 0, waiting = 0; bool Known() const { return up + waiting > 0; } };

// At the death. Takes a life only for Timed.
inline Mode Decide(int survival, const Others& others, long& lives) {
    if (survival == GameOver) return Mode::Down;
    if (survival != Wave) return Mode::Free;
    if (others.up) return Mode::WaitWave;
    if (others.Known() || lives <= 0) return Mode::Down;   // the rest of the team is down already
    --lives;
    return Mode::Timed;
}

struct Dead {
    Mode mode = Mode::None;
    uint32_t since = 0;      // when this mode began
    uint32_t clearedAt = 0;  // WaitWave/Down: when the way back opened
};

// One tick while dead: true when the respawn starts now. waveCleared: a reward
// event came since the death. stuckMs: Down that hears nothing from a director.
inline bool Step(Dead& d, int survival, const Others& others, bool waveCleared, long& lives, uint32_t now, uint32_t delayMs, uint32_t settleMs,
                 uint32_t stuckMs) {
    auto become = [&](Mode m) { d.mode = m; d.since = now; d.clearedAt = 0; };
    switch (d.mode) {
    case Mode::Timed:
    case Mode::Free: return now - d.since >= delayMs;
    case Mode::WaitWave:
        if (survival == GameOver) { become(Mode::Down); return false; }
        if (waveCleared || survival != Wave) {
            if (!d.clearedAt) d.clearedAt = now;
            return now - d.clearedAt >= settleMs;
        }
        if (others.up) return false;
        if (others.Known() || lives <= 0) { become(Mode::Down); return false; }   // the last one went down too
        --lives;   // the others left the session: solo from here
        become(Mode::Timed);
        return false;
    case Mode::Down:
        if (survival == Wave && others.up) { become(Mode::WaitWave); return false; }   // a stale view of the others
        if (survival == Off || survival == Arming || survival == Intermission) {
            if (!d.clearedAt) d.clearedAt = now;
            return now - d.clearedAt >= settleMs;
        }
        return now - d.since >= stuckMs;
    default: return false;
    }
}

// The host's game over: its own Isaac and every other human on the map are
// down, nobody comes back on a timer.
inline bool TeamDown(Life local, const Others& others) { return local == Waiting && !others.up; }

}  // namespace coop_respawn
