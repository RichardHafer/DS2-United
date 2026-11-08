// Dead Space 2 United by Richard Hafer.
#include "../src/coop_respawn_rules.h"

#include <cassert>
#include <cstdio>

using namespace coop_respawn;

int main() {
    const Others none{}, oneUp{1, 0}, oneWaiting{0, 1};
    long lives = 1;

    // solo in a wave: the life goes, back after the delay
    assert(Decide(Wave, none, lives) == Mode::Timed && lives == 0);
    // solo, no life left: down until the run ends
    assert(Decide(Wave, none, lives) == Mode::Down && lives == 0);
    // outside a wave nothing is taken
    lives = 1;
    assert(Decide(Intermission, none, lives) == Mode::Free && lives == 1);
    assert(Decide(Off, none, lives) == Mode::Free && Decide(Arming, oneUp, lives) == Mode::Free && lives == 1);
    assert(Decide(GameOver, oneUp, lives) == Mode::Down);
    // a Mercenaries run: back after the delay, nobody waits, no life used
    assert(Decide(Clock, none, lives) == Mode::Free && Decide(Clock, oneUp, lives) == Mode::Free && lives == 1);
    // a partner still fighting: wait for the wave, no life used
    assert(Decide(Wave, oneUp, lives) == Mode::WaitWave && lives == 1);
    // the partner is down already: the team is down, lives do not matter
    assert(Decide(Wave, oneWaiting, lives) == Mode::Down && lives == 1);

    // Timed and Free count the delay
    Dead d{Mode::Timed, 1000, 0};
    assert(!Step(d, Wave, none, false, lives, 5999, 5000, 2000, 45000));
    assert(Step(d, Wave, none, false, lives, 6000, 5000, 2000, 45000));

    // WaitWave: the wave end opens the way back after the settle time
    d = Dead{Mode::WaitWave, 1000, 0};
    assert(!Step(d, Wave, oneUp, false, lives, 60000, 5000, 2000, 45000));
    assert(!Step(d, Wave, oneUp, true, lives, 61000, 5000, 2000, 45000) && d.clearedAt == 61000);
    assert(!Step(d, Intermission, oneUp, true, lives, 62999, 5000, 2000, 45000));
    assert(Step(d, Intermission, oneUp, true, lives, 63000, 5000, 2000, 45000));
    // ... and so does the director leaving the wave without a reward event
    d = Dead{Mode::WaitWave, 1000, 0};
    assert(!Step(d, Off, none, false, lives, 2000, 5000, 2000, 45000));
    assert(Step(d, Off, none, false, lives, 4000, 5000, 2000, 45000));
    // the partner goes down too: down, no life taken
    lives = 1;
    d = Dead{Mode::WaitWave, 1000, 0};
    assert(!Step(d, Wave, oneWaiting, false, lives, 2000, 5000, 2000, 45000) && d.mode == Mode::Down && lives == 1);
    // the partner left the session: solo from here, the life is used
    d = Dead{Mode::WaitWave, 1000, 0};
    assert(!Step(d, Wave, none, false, lives, 2000, 5000, 2000, 45000) && d.mode == Mode::Timed && lives == 0 && d.since == 2000);
    assert(Step(d, Wave, none, false, lives, 7000, 5000, 2000, 45000));
    // ... with no life left: down
    d = Dead{Mode::WaitWave, 1000, 0};
    assert(!Step(d, Wave, none, false, lives, 2000, 5000, 2000, 45000) && d.mode == Mode::Down);
    // the game over turns a wait into down
    d = Dead{Mode::WaitWave, 1000, 0};
    assert(!Step(d, GameOver, oneWaiting, false, lives, 2000, 5000, 2000, 45000) && d.mode == Mode::Down);

    // Down: waits through the game over, comes back when the next run arms
    d = Dead{Mode::Down, 1000, 0};
    assert(!Step(d, Wave, oneWaiting, false, lives, 20000, 5000, 2000, 45000));
    assert(!Step(d, GameOver, oneWaiting, false, lives, 30000, 5000, 2000, 45000));
    assert(!Step(d, Off, oneWaiting, false, lives, 31000, 5000, 2000, 45000) && d.clearedAt == 31000);
    assert(Step(d, Arming, oneWaiting, false, lives, 33000, 5000, 2000, 45000));
    // a stale view: somebody is up after all, so it is a wait for the wave
    d = Dead{Mode::Down, 1000, 0};
    assert(!Step(d, Wave, oneUp, false, lives, 2000, 5000, 2000, 45000) && d.mode == Mode::WaitWave);
    // no director ever answers: not down forever
    d = Dead{Mode::Down, 1000, 0};
    assert(!Step(d, Wave, none, false, lives, 45999, 5000, 2000, 45000));
    assert(Step(d, Wave, none, false, lives, 46000, 5000, 2000, 45000));

    // the host's game over
    assert(TeamDown(Waiting, none) && TeamDown(Waiting, oneWaiting));
    assert(!TeamDown(Waiting, oneUp) && !TeamDown(Returning, none) && !TeamDown(Up, oneWaiting));
    assert(LifeOf(Mode::Timed) == Returning && LifeOf(Mode::Free) == Returning && LifeOf(Mode::WaitWave) == Waiting &&
           LifeOf(Mode::Down) == Waiting && LifeOf(Mode::None) == Up);

    std::puts("coop_respawn_rules tests passed");
    return 0;
}
