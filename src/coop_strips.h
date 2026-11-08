// Dead Space 2 United by Richard Hafer.
#pragma once
// Xbox 360 triangle strips restart at index 0xFFFF; D3D9 strips cannot. The
// segments are joined into one strip with degenerate triangles: the last index
// of one and the first of the next, plus that first index once more when the
// output so far is odd, so every segment starts on an even position and keeps
// its winding. Segments shorter than 3 indices draw nothing and are dropped.
// Pure logic for runtime_coop_outbreakdraw.inc (after Mark de Rune's
// Adapter.dll, 0x0491B400 in his exe).
#include <cstdint>

namespace coop_strips {

constexpr uint16_t kRestart = 0xFFFF;

// Returns the output count; dst may be null (counting pass). minmax gets the
// smallest index in the low half and the largest in the high half (0 when
// nothing is drawn).
inline uint32_t Stitch(const uint16_t* src, uint32_t count, uint16_t* dst, uint32_t* minmax) {
    uint32_t out = 0, lo = 0xFFFF, hi = 0;
    uint16_t last = 0;
    uint32_t i = 0;
    while (i < count) {
        while (i < count && src[i] == kRestart) ++i;
        const uint32_t first = i;
        while (i < count && src[i] != kRestart) ++i;
        if (i - first < 3) continue;
        if (out) {
            const bool odd = (out & 1) != 0;
            if (dst) { dst[out] = last; dst[out + 1] = src[first]; }
            out += 2;
            if (odd) { if (dst) dst[out] = src[first]; ++out; }
        }
        for (uint32_t k = first; k < i; ++k) {
            const uint16_t v = src[k];
            if (dst) dst[out] = v;
            ++out;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        last = src[i - 1];
    }
    if (minmax) *minmax = out ? (lo | hi << 16) : 0;
    return out;
}

}  // namespace coop_strips
