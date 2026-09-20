#include "../src/global_stream_lifetime.h"
#include <cassert>
#include <iostream>

int main() {
    ds2::StreamTransitionGate gate;
    gate.pause(7);
    assert(!gate.canResume(7, true, true));
    assert(!gate.canResume(8, false, true));
    assert(!gate.canResume(8, true, false));
    assert(gate.canResume(8, true, true));
    gate.reset();
    assert(!gate.canResume(9, true, true));
    ds2::GlobalStreamLifetime stream;
    assert(!stream.attach(0));
    assert(stream.attach(0x1000));
    const auto epoch = stream.generation;
    assert(stream.beginRequest() && !stream.beginRequest());
    assert(stream.acceptHandle(epoch, 7));
    stream.sample(0x1000, 7, 3, false);
    assert(!stream.usable(0x1000));
    stream.sample(0x1000, 7, 3, true);
    assert(stream.usable(0x1000) && !stream.usable(0x2000));
    assert(stream.beginTeardown(0x1000));
    assert(!stream.usable(0x1000) && !stream.attach(0x1000));
    stream.finishTeardown();
    assert(stream.attach(0x1000) && stream.generation != epoch);
    assert(stream.beginRequest());
    assert(!stream.acceptHandle(epoch, 7));
    assert(stream.acceptHandle(stream.generation, 8));
    stream.sample(0x1000, 7, 3, true);
    assert(!stream.usable(0x1000));
    assert(!stream.beginRequest());
    for (int status : {-1, 2, 4, 5, 6}) {
        ds2::GlobalStreamLifetime s;
        assert(s.attach(0x1000) && s.beginRequest());
        assert(s.acceptHandle(s.generation, 7));
        s.sample(0x1000, 7, status, true);
        assert(s.state == ds2::GlobalStreamLifetime::State::Failed);
    }
    std::cout << "Global lifetime tests passed; no native calls.\n";
}
