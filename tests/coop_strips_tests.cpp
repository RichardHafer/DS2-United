// Restart strips to plain D3D9 strips: fixed cases and a random check that the
// stitched strip draws exactly the segments' triangles with the same winding.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>
#include "../src/coop_strips.h"

namespace {
using coop_strips::kRestart;
using coop_strips::Stitch;
using Tri = std::array<uint16_t, 3>;

std::vector<uint16_t> Run(const std::vector<uint16_t>& in, uint32_t* minmax) {
    uint32_t mm = 0;
    const uint32_t n = Stitch(in.data(), static_cast<uint32_t>(in.size()), nullptr, &mm);
    std::vector<uint16_t> out(n + 1, 0xABCD);
    uint32_t mm2 = 0;
    assert(Stitch(in.data(), static_cast<uint32_t>(in.size()), out.data(), &mm2) == n && mm2 == mm);
    assert(out[n] == 0xABCD);   // nothing written past the counted end
    out.resize(n);
    if (minmax) *minmax = mm;
    return out;
}

// Triangles a strip draws, oriented (odd ones flipped back), rotated so the
// smallest index leads; degenerate ones skipped.
std::vector<Tri> Triangles(const std::vector<uint16_t>& s) {
    std::vector<Tri> out;
    for (size_t k = 0; k + 2 < s.size(); ++k) {
        Tri t = (k & 1) ? Tri{s[k + 1], s[k], s[k + 2]} : Tri{s[k], s[k + 1], s[k + 2]};
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
        while (t[0] > t[1] || t[0] > t[2]) t = Tri{t[1], t[2], t[0]};
        out.push_back(t);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void Fixed() {
    uint32_t mm = 0;
    assert((Run({0, 1, 2, 3}, &mm) == std::vector<uint16_t>{0, 1, 2, 3}) && mm == (0u | 3u << 16));
    // odd first segment: the next one gets an extra index to start on an even position
    assert((Run({0, 1, 2, kRestart, 3, 4, 5}, &mm) == std::vector<uint16_t>{0, 1, 2, 2, 3, 3, 3, 4, 5}) && mm == (0u | 5u << 16));
    // even first segment: just the two joining indices
    assert((Run({0, 1, 2, 3, kRestart, 4, 5, 6}, &mm) == std::vector<uint16_t>{0, 1, 2, 3, 3, 4, 4, 5, 6}));
    // segments under 3 indices and runs of restarts are dropped
    assert((Run({0, 1, kRestart, 7, 8, 9, kRestart, kRestart, 5}, &mm) == std::vector<uint16_t>{7, 8, 9}) && mm == (7u | 9u << 16));
    assert(Run({kRestart, kRestart}, &mm).empty() && mm == 0);
    assert(Run({}, &mm).empty() && mm == 0);
    assert((Run({kRestart, 10, 11, 12, kRestart}, &mm) == std::vector<uint16_t>{10, 11, 12}) && mm == (10u | 12u << 16));
    std::printf("fixed ok\n");
}

void Random() {
    std::mt19937 rng(12345);
    for (int round = 0; round < 2000; ++round) {
        std::vector<uint16_t> in;
        std::vector<Tri> expected;
        const int parts = 1 + static_cast<int>(rng() % 6);
        uint16_t next = static_cast<uint16_t>(rng() % 100);
        for (int p = 0; p < parts; ++p) {
            const int restarts = static_cast<int>(rng() % 3);
            for (int r = 0; r < restarts; ++r) in.push_back(kRestart);
            const int len = static_cast<int>(rng() % 9);
            std::vector<uint16_t> seg;
            for (int k = 0; k < len; ++k) seg.push_back(next++);   // distinct within a segment
            in.insert(in.end(), seg.begin(), seg.end());
            in.push_back(kRestart);
            const auto t = Triangles(seg);
            expected.insert(expected.end(), t.begin(), t.end());
        }
        std::sort(expected.begin(), expected.end());
        assert(Triangles(Run(in, nullptr)) == expected);
    }
    std::printf("random ok (2000 strips)\n");
}
}  // namespace

int main() {
    Fixed();
    Random();
    std::printf("all strip tests passed\n");
    return 0;
}
