// Dead Space 2 United by Richard Hafer.
#pragma once
// Hold the Zone on the co-op side: the wire format the host sends its partners
// and the direction text each machine shows for its own Isaac.
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace coop_zone {
// What the director says about the zone (DS2Coop_SurvivalZone).
enum State : int { None = 0, Empty = 1, Capturing = 2, Contested = 3, Losing = 4, kStates };
inline const char* StateName(int s) {
    static const char* const kNames[] = {"none", "empty", "capturing", "contested", "losing"};
    return s >= 0 && s < kStates ? kNames[s] : "?";
}

#pragma pack(push, 1)
struct Wire { uint32_t id; float pos[3]; float radius; uint8_t state, percent; };
#pragma pack(pop)
static_assert(sizeof(Wire) == 22, "zone wire size");

// Where `to` lies for someone at `from` facing `forward` ({x, y, z}): AHEAD,
// LEFT, RIGHT or BEHIND, and UP or DOWN past 3 m. y is up and left = up x forward,
// as in the body frame the bots read (CMC +0xD0 left, +0xF0 forward): facing +z,
// +x is left.
inline const char* Bearing(const float* from, const float* forward, const float* to, const char** vertical) {
    const float dx = to[0] - from[0], dz = to[2] - from[2], dy = to[1] - from[1];
    *vertical = dy > 3.0f ? "UP" : dy < -3.0f ? "DOWN" : "";
    const float fl = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]), dl = std::sqrt(dx * dx + dz * dz);
    if (!(fl > 1e-3f) || !(dl > 1e-3f)) return "";
    const float fx = forward[0] / fl, fz = forward[2] / fl, tx = dx / dl, tz = dz / dl;
    const float ahead = fx * tx + fz * tz, side = fx * tz - fz * tx;
    if (ahead >= 0.7071f) return "AHEAD";
    if (ahead <= -0.7071f) return "BEHIND";
    return side < 0.0f ? "LEFT" : "RIGHT";
}
// The director's cylinder (survival::InZone): 0.5 m below the spot to 1 m above.
inline bool Inside(const float* center, float radius, const float* p) {
    const float dx = p[0] - center[0], dz = p[2] - center[2], dy = p[1] - center[1];
    return dx * dx + dz * dz <= radius * radius && dy >= -0.5f && dy <= 1.0f;
}
// The overlay's zone part for one Isaac: "IN THE ZONE" or "ZONE 45 M AHEAD UP".
inline void Text(char* out, size_t size, const float* self, const float* forward, const float* center, float radius) {
    if (Inside(center, radius, self)) {
        std::snprintf(out, size, "IN THE ZONE");
        return;
    }
    const float dx = center[0] - self[0], dy = center[1] - self[1], dz = center[2] - self[2];
    const float meters = std::sqrt(dx * dx + dy * dy + dz * dz);
    const char* vertical = "";
    const char* way = Bearing(self, forward, center, &vertical);
    std::snprintf(out, size, "ZONE %.0f M%s%s%s%s", meters, way[0] ? " " : "", way, vertical[0] ? " " : "", vertical);
}
}  // namespace coop_zone
