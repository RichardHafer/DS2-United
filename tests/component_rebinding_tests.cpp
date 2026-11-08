// Dead Space 2 United by Richard Hafer.
#include "../src/component_rebinding.h"
#include <cassert>
#include <cstring>
#include <cstdio>

int main() {
    using namespace ds2::native;
    std::array<unsigned char, 4096> memory{};
    auto put = [&](size_t at, uint32_t value) { std::memcpy(memory.data() + at, &value, 4); };
    auto read = [&](uint32_t at, void* out, size_t size) {
        if (at > memory.size() || size > memory.size() - at) return false;
        std::memcpy(out, memory.data() + at, size); return true;
    };
    const ds2::Guid vent{1,2,3,4};
    put(100, 11); put(104, 1); put(108, 300); put(300, 500); put(500, 999);
    put(516, 1); std::memcpy(memory.data() + 524, vent.data(), 16);
    put(200, 22); put(204, 2); put(208, 320); put(212, 340);
    put(320, 700); put(700, 888);
    put(340, 900); put(900, 999); put(916, 1);
    put(512, 1); put(912, 1);
    VentHeaders headers{};
    assert(prepareVentHeaders(100, 200, 999, vent, read, headers) == RebindResult::Ready);
    assert(headers.source == 500 && headers.donor == 900);
    put(512, UINT16_MAX);
    assert(prepareVentHeaders(100, 200, 999, vent, read, headers) == RebindResult::InvalidReferenceCount);
    assert(!headers.source && !headers.donor);
    put(512, 0);
    assert(prepareVentHeaders(100, 200, 999, vent, read, headers) == RebindResult::InvalidReferenceCount);
    put(512, 1);
    ComponentListCopy output{};
    const auto untouched = memory;
    assert(prepareVentRebinding(100, 200, 999, vent, read, output) == RebindResult::Ready);
    assert(output.layoutKey == 22 && output.count == 2);
    assert(output.references[0] == 320 && output.references[1] == 300);
    assert(memory == untouched);
    assert(prepareVentRebinding(100, 200, 999, ds2::Guid{}, read, output) == RebindResult::WrongVent);
    put(916, 0);
    assert(prepareVentRebinding(100, 200, 999, vent, read, output) == RebindResult::WrongMode);
    assert(prepareVentHeaders(100, 200, 999, vent, read, headers, true) == RebindResult::Ready);
    put(916, 2);
    assert(prepareVentHeaders(100, 200, 999, vent, read, headers, true) == RebindResult::WrongMode);
    put(916, 1); put(700, 999);
    assert(prepareVentRebinding(100, 200, 999, vent, read, output) == RebindResult::DuplicateSpawning);
    put(700, 888); put(900, 888);
    assert(prepareVentRebinding(100, 200, 999, vent, read, output) == RebindResult::MissingSpawning);
    put(204, 65);
    assert(prepareVentRebinding(100, 200, 999, vent, read, output) == RebindResult::InvalidList);
    assert(prepareVentRebinding(UINT32_MAX, 200, 999, vent, read, output) == RebindResult::InvalidList);
    std::puts("Component rebinding tests passed; no live game writes.");
}
