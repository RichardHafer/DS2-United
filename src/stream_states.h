// Dead Space 2 United by Richard Hafer.
#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <vector>
#include <cstring>

namespace ds2::native {
struct StreamState {
    uint32_t pointer, descriptor, handle, status, identity, flags;
    uint16_t dependencyRefs;
    uint32_t notificationBits = 0, waitHandleCount = 0;
};
struct StreamStates {
    enum Status { Missing, Invalid, Cycle, Budget, Changed, Stable } result = Missing;
    std::vector<StreamState> entries;
};

// Stream notification state only; this does not validate individual assets.
inline bool dispatchedNotification(const StreamState& stream) {
    return stream.status == 3 && stream.notificationBits == 0x80000000u && stream.waitHandleCount == 0;
}

// The handle of a dispatched stream with that identity and name, 0 when
// there is none. Of two dispatched twins the newer one wins: handles are
// handed out in order, so the higher one belongs to the level that is
// loading now, and that is the one a caller wants to hold a reference on.
template<class Read>
uint32_t findDispatchedStream(const StreamStates& states, uint32_t identity, const char* name, Read read) {
    if (states.result != StreamStates::Stable) return 0;
    const size_t length = std::strlen(name);
    if (!length || length >= 160) return 0;
    uint32_t found = 0;
    for (const auto& stream : states.entries) {
        if (!stream.descriptor || stream.descriptor > UINT32_MAX - 8) continue;
        uint32_t header = 0, fields[2]{};
        if (!read(stream.descriptor, &header, 4) || !header || header > UINT32_MAX - 8 ||
            !read(header, fields, sizeof(fields)) || fields[0] != identity) continue;
        // A twin on its way out may already have lost its name block; that
        // is no reason to call the live one missing (30 s hold, 15.09. 13:00).
        if (!fields[1] || fields[1] > UINT32_MAX - 160) continue;
        char actual[160]{};
        if (!read(fields[1], actual, length + 1)) continue;
        if (std::memcmp(actual, name, length + 1)) continue;
        if (!dispatchedNotification(stream) || !stream.handle) continue;
        if (stream.handle > found) found = stream.handle;
    }
    return found;
}

// One stream of that identity and name may exist twice for a while: after a
// checkpoint restart the old level's copy stays in the list until the engine
// retires it, next to the new one. Measured on 2026-09-15 (three restarts):
// every package was at status 3 the moment the new level started loading,
// and the verdict still took 25 to 140 seconds, always stalling on the same
// entry, because "two matches" used to count as "missing". A dispatched
// stream is a dispatched stream; a retiring twin does not take that away.
template<class Read>
bool containsDispatchedStream(const StreamStates& states, uint32_t identity, const char* name, Read read) {
    return findDispatchedStream(states, identity, name, read) != 0;
}

// Read-only diagnostic. A stable sample is not a resource readiness fence.
template<class Read>
StreamStates inspectStreamStates(uint32_t manager, Read read) {
    StreamStates out;
    if (!manager) return out;
    out.result = StreamStates::Invalid;
    if (manager > UINT32_MAX - 0x90) return out;
    constexpr uint32_t offsets[] = {0x8C, 0x7C, 0x74, 0x84};
    std::array<uint32_t, 4> heads{};
    for (unsigned i = 0; i < heads.size(); ++i)
        if (!read(manager + offsets[i], &heads[i], 4)) return out;
    unsigned reads = 0;
    for (unsigned list = 0; list < heads.size(); ++list) {
        std::vector<uint32_t> seen;
        uint32_t current = heads[list];
        while (current) {
            if (++reads > 512) { out.result = StreamStates::Budget; return out; }
            if (std::find(seen.begin(), seen.end(), current) != seen.end()) {
                out.result = StreamStates::Cycle; return out;
            }
            seen.push_back(current);
            if ((current & 3) || current > UINT32_MAX - 0xF8) return out;
            std::array<uint32_t, 23> fields{};
            if (!read(current, fields.data(), sizeof(fields))) return out;
            if (!fields[7] || fields[9] > 5) return out;
            uint32_t notificationBits = 0, waitHandleCount = 0;
            if (!read(current + 0x68, &notificationBits, 4) ||
                !read(current + 0xF4, &waitHandleCount, 4)) return out;
            if (std::none_of(out.entries.begin(), out.entries.end(),
                [current](const auto& e) { return e.pointer == current; })) {
                out.entries.push_back({current, fields[6], fields[7], fields[9], fields[11], fields[21],
                    static_cast<uint16_t>(fields[22]), notificationBits, waitHandleCount});
            }
            if (list == 0) break;
            current = fields[0];
        }
    }
    for (unsigned i = 0; i < heads.size(); ++i) {
        uint32_t after = 0;
        if (!read(manager + offsets[i], &after, 4)) return out;
        if (after != heads[i]) { out.result = StreamStates::Changed; return out; }
    }
    out.result = StreamStates::Stable;
    return out;
}
}
