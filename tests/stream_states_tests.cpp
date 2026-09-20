#include "../src/stream_states.h"
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    unsigned char memory[4096]{};
    auto put = [&](unsigned offset, uint32_t value) { std::memcpy(memory + offset, &value, 4); };
    auto read = [&](uint32_t address, void* out, size_t length) {
        if (address > sizeof(memory) || length > sizeof(memory) - address) return false;
        std::memcpy(out, memory + address, length); return true;
    };
    using namespace ds2::native;
    assert(inspectStreamStates(0, read).result == StreamStates::Missing);
    put(0x100 + 0x74, 0x300);
    put(0x300 + 0x1C, 7);
    put(0x300 + 0x24, 3);
    put(0x300 + 0x58, 4);
    put(0x300 + 0x68, 0x80000000);
    put(0x300 + 0xF4, 0);
    auto result = inspectStreamStates(0x100, read);
    assert(result.result == StreamStates::Stable && result.entries.size() == 1);
    assert(result.entries[0].status == 3 && result.entries[0].dependencyRefs == 4);
    assert(result.entries[0].notificationBits == 0x80000000 && result.entries[0].waitHandleCount == 0);
    auto shortRead = [&](uint32_t address, void* out, size_t length) {
        return address != 0x3F4 && read(address, out, length);
    };
    assert(inspectStreamStates(0x100, shortRead).result == StreamStates::Invalid);
    put(0x300 + 0x18, 0x600);
    put(0x600, 0x650);
    put(0x650, 123);
    put(0x654, 0x700);
    std::memcpy(memory + 0x700, "test_stream", 12);
    result = inspectStreamStates(0x100, read);
    assert(containsDispatchedStream(result, 123, "test_stream", read));
    assert(!containsDispatchedStream(result, 123, "wrong_name", read));
    assert(!containsDispatchedStream(result, 124, "test_stream", read));
    result.entries[0].notificationBits = 0;
    assert(!containsDispatchedStream(result, 123, "test_stream", read));
    result.entries[0].notificationBits = 0x80000000;
    result.entries[0].waitHandleCount = 1;
    assert(!containsDispatchedStream(result, 123, "test_stream", read));
    result.entries[0].waitHandleCount = 0;
    // A retiring twin next to the dispatched stream (checkpoint restart) does
    // not take the verdict away, whether the twin is still dispatched or not.
    result.entries.push_back(result.entries[0]);
    assert(containsDispatchedStream(result, 123, "test_stream", read));
    result.entries[1].notificationBits = 0;
    assert(containsDispatchedStream(result, 123, "test_stream", read));
    result.entries[0].notificationBits = 0;
    assert(!containsDispatchedStream(result, 123, "test_stream", read));
    result.entries.pop_back();
    result.entries[0].notificationBits = 0x80000000;
    // A twin whose descriptor points at an unreadable name block is skipped.
    result.entries.push_back(result.entries[0]);
    put(0x800, 0x850);
    put(0x850, 123);
    put(0x854, 0x3000);
    result.entries[1].descriptor = 0x800;
    assert(containsDispatchedStream(result, 123, "test_stream", read));
    result.entries.pop_back();
    // findDispatchedStream applies the same match and reports the newest handle.
    auto handles = inspectStreamStates(0x100, read);
    assert(handles.entries.size() == 1 && handles.entries[0].handle);
    assert(findDispatchedStream(handles, 123, "test_stream", read) == handles.entries[0].handle);
    assert(!findDispatchedStream(handles, 123, "wrong_name", read));
    assert(!findDispatchedStream(handles, 124, "test_stream", read));
    handles.entries.push_back(handles.entries[0]);
    handles.entries[1].handle = handles.entries[0].handle + 1;
    assert(findDispatchedStream(handles, 123, "test_stream", read) == handles.entries[1].handle);
    handles.entries[1].notificationBits = 0;
    assert(findDispatchedStream(handles, 123, "test_stream", read) == handles.entries[0].handle);
    handles.entries.pop_back();
    handles.entries[0].notificationBits = 0;
    assert(!findDispatchedStream(handles, 123, "test_stream", read));
    handles.entries[0].notificationBits = 0x80000000;
    handles.entries[0].handle = 0;
    assert(!findDispatchedStream(handles, 123, "test_stream", read));
    put(0x100 + 0x8C, 0x300);
    assert(inspectStreamStates(0x100, read).entries.size() == 1);
    put(0x300, 0x300);
    assert(inspectStreamStates(0x100, read).result == StreamStates::Cycle);
    put(0x300, 0);
    put(0x300 + 0x24, 99);
    assert(inspectStreamStates(0x100, read).result == StreamStates::Invalid);
    std::cout << "Stream state tests passed; no native calls.\n";
}
