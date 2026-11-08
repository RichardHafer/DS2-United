#include <cassert>
#include <cstdio>
#include "../src/coop_local_lifecycle.h"
#include "../src/coop_peer_readiness.h"

int main() {
    coop_local::SettleState state;
    assert(!state.Update(0, true, false, 0, 100));
    assert(!state.Update(4000, true, false, 0, 100));
    assert(state.Update(4001, true, false, 0, 100));
    // Replacement without a loading screen must not inherit readiness.
    assert(!state.Update(5000, true, false, 0, 200));
    assert(!state.Update(9000, true, false, 0, 200));
    assert(state.Update(9001, true, false, 0, 200));
    // A removed body may later reuse the same heap address.
    assert(!state.Update(10000, true, false, 0, 0));
    assert(!state.Update(20000, true, false, 0, 200));
    assert(state.Update(24001, true, false, 0, 200));
    assert(!state.Update(25000, false, false, 0, 200));
    assert(!state.Update(35000, true, false, 0, 200));
    assert(state.Update(39001, true, false, 0, 200));
    // A release serial catches a load that occurred between samples.
    assert(!state.Update(40000, true, false, 1, 200));
    assert(state.Update(44001, true, false, 1, 200));
    assert(!state.Update(45000, true, true, 1, 200));
    assert(!state.Update(55000, true, false, 1, 200));
    assert(state.Update(59001, true, false, 1, 200));
    // An MP map visit asks for a shorter stand.
    coop_local::SettleState mp;
    assert(!mp.Update(0, true, false, 0, 100, 1500));
    assert(!mp.Update(1500, true, false, 0, 100, 1500));
    assert(mp.Update(1501, true, false, 0, 100, 1500));

    coop_peer::State ready;
    ready.token = 1;
    ready.UpdateLocal(true, 200);
    assert(ready.Accept({coop_peer::kMagic, 1, 2, 1, 1, ready.epoch, 1, 1}, 59001));
    assert(ready.Ready(59001));
    ready.UpdateLocal(state.Update(60000, true, false, 1, 300), 300);
    assert(!ready.Ready(60000));
    ready.UpdateLocal(state.Update(64001, true, false, 1, 300), 300);
    assert(!ready.Ready(64001));
    assert(ready.Accept({coop_peer::kMagic, 1, 2, 1, 1, ready.epoch, 1, 2}, 64001));
    assert(ready.Ready(64001));

    // A respawn in a level that stays: the new body keeps the settled level.
    coop_local::SettleState respawn;
    assert(!respawn.Update(0, true, false, 0, 100));
    assert(respawn.Update(4001, true, false, 0, 100));
    assert(respawn.Adopt(300));
    assert(respawn.Update(4100, true, false, 0, 300));
    assert(!respawn.Adopt(300));
    // Nothing to adopt while nothing is tracked.
    coop_local::SettleState idle;
    assert(!idle.Adopt(300));
    assert(!idle.Update(0, true, false, 0, 300));

    coop_local::SettleState wrap;
    assert(!wrap.Update(0xFFFFFF00U, true, false, 0, 1));
    assert(!wrap.Update(0xEA0U, true, false, 0, 1));
    assert(wrap.Update(0xEA1U, true, false, 0, 1));
    std::puts("Local lifecycle and peer acknowledgement checks passed.");
}
