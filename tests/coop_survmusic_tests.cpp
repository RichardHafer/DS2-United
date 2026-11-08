// Dead Space 2 United by Richard Hafer.
#include "../src/coop_survmusic.h"

#include <cassert>
#include <cstdio>

using namespace coop_survmusic;

int main() {
    Player p;
    Action a = p.Update(false, 0, 2000);
    assert(a.kind == Action::None);                       // nothing plays outside a wave
    a = p.Update(true, 1000, 2000);
    assert(a.kind == Action::Play && a.track >= 4 && a.track <= 7);   // a wave opens with a combat track
    const int first = a.track;
    a = p.Update(true, 2000, 2000);
    assert(a.kind == Action::None);
    a = p.Update(true, 1000 + kCutMs[first], 2000);
    assert(a.kind == Action::Play && a.track >= 0 && a.track <= 3);   // then an action track
    const int second = a.track;
    a = p.Update(true, 1000 + kCutMs[first] + kCutMs[second], 2000);
    assert(a.kind == Action::Play && a.track >= 4 && a.track <= 7 && a.track != first);   // combat again, not the same one
    a = p.Update(false, 500000, 2000);
    assert(a.kind == Action::None);                       // the stop waits for the delay
    a = p.Update(false, 501999, 2000);
    assert(a.kind == Action::None);
    a = p.Update(false, 502000, 2000);
    assert(a.kind == Action::Stop && !p.playing);
    a = p.Update(false, 503000, 2000);
    assert(a.kind == Action::None);
    a = p.Update(true, 600000, 2000);
    assert(a.kind == Action::Play && a.track >= 4);        // the next wave starts fresh
    p.Update(false, 601000, 2000);
    a = p.Update(true, 601500, 2000);                     // back into a wave before the stop: keeps playing
    assert(a.kind == Action::None && p.playing);
    p.Forget();
    assert(!p.playing && p.Update(false, 700000, 2000).kind == Action::None);
    // the picker never repeats the last track of its group twice in a row
    Player q;
    for (int i = 0; i < 200; ++i) {
        const int before = q.lastCombat;
        const int t = q.Pick(true);
        assert(t >= 4 && t <= 7 && t != before);
    }
    std::printf("survival music tests passed\n");
    return 0;
}
