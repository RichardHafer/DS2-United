// Dead Space 2 United by Hafer.
#pragma once
#include <cmath>

// On-screen waypoints: where a world point lands on the screen, or where on the
// edge its arrow goes. The camera is the game's camera-to-world matrix (rows x
// left, y up, z ahead, then the position) and the projection's half angles, the
// way UIMPObjectiveIndicator::CheckObjOnScreen tests its objective: -x/z within
// tan(fov/2), y/z within tan(fov/2)/aspect.
namespace coop_marks {
struct Camera { float axis[3][3]; float pos[3]; float tanX, tanY; };
struct Placed { float x, y; bool onScreen; float dirX, dirY; };   // dir: unit, from the screen's middle toward the target

inline void ToView(const Camera& c, const float* p, float& x, float& y, float& z) {
    const float d[3] = {p[0] - c.pos[0], p[1] - c.pos[1], p[2] - c.pos[2]};
    x = d[0] * c.axis[0][0] + d[1] * c.axis[0][1] + d[2] * c.axis[0][2];
    y = d[0] * c.axis[1][0] + d[1] * c.axis[1][1] + d[2] * c.axis[1][2];
    z = d[0] * c.axis[2][0] + d[1] * c.axis[2][1] + d[2] * c.axis[2][2];
}
// margin: pixels kept free at every edge; a point closer to an edge goes to it.
inline Placed Place(const Camera& c, const float* p, float w, float h, float margin) {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    ToView(c, p, x, y, z);
    const float hw = w * 0.5f, hh = h * 0.5f;
    Placed out{hw, hh, false, 0.0f, 1.0f};
    float dx = 0.0f, dy = 0.0f;
    if (z > 0.05f) {
        out.x = hw * (1.0f - x / z / c.tanX);
        out.y = hh * (1.0f - y / z / c.tanY);
        if (out.x >= margin && out.x <= w - margin && out.y >= margin && out.y <= h - margin) {
            out.onScreen = true;
            return out;
        }
        dx = out.x - hw;
        dy = out.y - hh;
    } else {
        dx = -x;   // behind: still left or right as seen, up or down by height
        dy = -y;
    }
    const float length = std::sqrt(dx * dx + dy * dy);
    if (!(length > 1e-4f)) { dx = 0.0f; dy = 1.0f; }
    else { dx /= length; dy /= length; }
    const float ax = hw - margin, ay = hh - margin;
    float k = 1e9f;
    if (std::fabs(dx) > 1e-6f) k = ax / std::fabs(dx);
    if (std::fabs(dy) > 1e-6f && ay / std::fabs(dy) < k) k = ay / std::fabs(dy);
    out.x = hw + dx * k;
    out.y = hh + dy * k;
    out.dirX = dx;
    out.dirY = dy;
    return out;
}
}  // namespace coop_marks
