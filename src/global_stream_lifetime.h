// Dead Space 2 United by Richard Hafer.
#pragma once
#include <cstdint>

namespace ds2 {
struct StreamTransitionGate {
    bool paused = false;
    uint32_t epoch = 0;
    void pause(uint32_t current) { paused = true; epoch = current; }
    bool canResume(uint32_t current, bool serviceAlive, bool rootAbsent) const {
        return paused && current != epoch && serviceAlive && rootAbsent;
    }
    void reset() { paused = false; }
};
// Owned by the native loading thread, never by the logging worker.
class GlobalStreamLifetime {
public:
    enum class State { Idle, Waiting, Loading, Ready, Failed, Destroying };
    State state = State::Idle;
    uint32_t manager = 0, handle = 0;
    uint64_t generation = 0;

    bool attach(uint32_t current) {
        if (!current || state == State::Destroying) return false;
        if (manager == current) return true;
        manager = current;
        handle = 0;
        ++generation;
        state = State::Waiting;
        return true;
    }
    bool beginRequest() {
        if (state != State::Waiting) return false;
        state = State::Loading;
        return true;
    }
    bool acceptHandle(uint64_t requestGeneration, uint32_t value) {
        if (generation != requestGeneration || state != State::Loading || handle) return false;
        handle = value;
        if (!handle) state = State::Failed;
        return handle != 0;
    }
    void sample(uint32_t current, uint32_t checkedHandle, int status, bool resourcesReady) {
        if ((state != State::Loading && state != State::Ready) || !handle) return;
        if (current != manager || checkedHandle != handle || status < 0 || status > 5 ||
            status == 2 || status == 4 || status == 5) {
            state = State::Failed;
            return;
        }
        state = status == 3 && resourcesReady ? State::Ready : State::Loading;
    }
    bool usable(uint32_t current) const {
        return state == State::Ready && current == manager && handle != 0;
    }
    bool beginTeardown(uint32_t current) {
        if (!current || current != manager || state == State::Destroying) return false;
        state = State::Destroying;
        ++generation;
        return true;
    }
    void finishTeardown() {
        if (state != State::Destroying) return;
        manager = handle = 0;
        state = State::Idle;
    }
};
}
