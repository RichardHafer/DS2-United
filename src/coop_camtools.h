// Dead Space 2 United by Richard Hafer.
#pragma once
// Pure logic of the trailer camera (runtime_coop_camtools.inc): flying the free
// camera with a gamepad, and recording and replaying a camera path. No game
// memory is touched here.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop_camtools {
// The camera data the game keeps (CameraManager::GetActiveCamData, blender +0x00):
// position (y up), pitch (positive looks down), yaw (positive turns left), roll in
// radians, and the horizontal field of view authored for 16:9 (+0x30).
struct Pose { float pos[3]{}; float pitch = 0.0f, yaw = 0.0f, roll = 0.0f, fov = 1.2217305f; };

// One pad reading: sticks -1..1 after the dead zone, triggers 0..1, XInput button bits.
struct Pad { float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0; uint16_t buttons = 0; };
enum Button : uint16_t {
    kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010, kBack = 0x0020, kL3 = 0x0040, kR3 = 0x0080,
    kLb = 0x0100, kRb = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000,
};
inline float Stick(int16_t raw, int deadZone) {
    const float v = static_cast<float>(raw);
    const float dz = static_cast<float>(deadZone);
    if (std::fabs(v) <= dz) return 0.0f;
    const float sign = v < 0.0f ? -1.0f : 1.0f;
    const float out = (std::fabs(v) - dz) / (32767.0f - dz);
    return sign * (out > 1.0f ? 1.0f : out);
}
inline float Trigger(uint8_t raw, int threshold) { return raw <= threshold ? 0.0f : static_cast<float>(raw - threshold) / static_cast<float>(255 - threshold); }

struct Tuning {
    float moveSpeed = 3.0f;      // metres a second at full stick
    float lookSpeed = 1.4f;      // radians a second at full stick
    float rollSpeed = 0.6f, fovSpeed = 0.5f;
    float minFov = 0.17f, maxFov = 2.4f;
    float smoothing = 0.25f;     // seconds the motion takes to follow the stick
    bool invertLook = false;     // stick up looks down
};
// The game's camera-to-world rows (mat44flt_EulerAngles34 0x013C2870): left, up,
// forward; the projection flips x, so the screen's right is minus the left row.
inline void Rows(const Pose& p, float (&left)[3], float (&up)[3], float (&forward)[3]) {
    const float sp = std::sin(p.pitch), cp = std::cos(p.pitch), sy = std::sin(p.yaw), cy = std::cos(p.yaw), sr = std::sin(p.roll), cr = std::cos(p.roll);
    left[0] = sr * sp * sy + cr * cy;
    left[1] = sr * cp;
    left[2] = sr * sp * cy - cr * sy;
    up[0] = cr * sp * sy - sr * cy;
    up[1] = cr * cp;
    up[2] = cr * sp * cy + sr * sy;
    forward[0] = cp * sy;
    forward[1] = -sp;
    forward[2] = cp * cy;
}
// The blender's cached camera-to-world matrix (+0x50): rows left, up, forward, position.
inline void CameraToWorld(const Pose& p, float (&m)[16]) {
    float l[3], u[3], f[3];
    Rows(p, l, u, f);
    const float rows[4][4] = {{l[0], l[1], l[2], 0.0f}, {u[0], u[1], u[2], 0.0f}, {f[0], f[1], f[2], 0.0f}, {p.pos[0], p.pos[1], p.pos[2], 1.0f}};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m[r * 4 + c] = rows[r][c];
}
// The FOV the renderer gets (CameraManager::Update 0x007937A1): with conversion 2 (+0x40) it is
// widened from the authored aspect (+0x3C: 1 is 16:9, else 4:3) to the screen's.
inline float RenderFov(float fov, int authored16x9, int conversion, float screenAspect) {
    if (conversion != 2 || screenAspect <= 0.0f) return fov;
    const float authored = authored16x9 == 1 ? 1.7777778f : 1.3333334f;
    return 2.0f * std::atan(std::tan(fov * 0.5f) / authored * screenAspect);
}
// The flight state: velocities that follow the stick smoothly, so a recorded path needs no clean-up.
struct Flight { float move[3]{}; float look[2]{}; float roll = 0.0f, fov = 0.0f; };
inline float Follow(float current, float target, float dt, float smoothing) {
    if (smoothing <= 0.0f) return target;
    const float k = 1.0f - std::exp(-dt / smoothing);
    return current + (target - current) * k;
}
// One frame of flying: left stick moves along the view, triggers go down/up, right stick
// looks, the D-pad rolls (left/right), the shoulder buttons zoom; `speed` scales the movement.
inline void Fly(Pose& p, Flight& f, const Pad& pad, float speed, float dt, const Tuning& t) {
    if (dt <= 0.0f) return;
    if (dt > 0.1f) dt = 0.1f;
    float left[3], up[3], fwd[3];
    Rows(p, left, up, fwd);
    const float right[3] = {-std::cos(p.yaw), 0.0f, std::sin(p.yaw)};   // level, whatever the roll
    const float m = t.moveSpeed * speed;
    float want[3];
    for (int k = 0; k < 3; ++k) want[k] = (fwd[k] * pad.ly + right[k] * pad.lx) * m;
    want[1] += (pad.rt - pad.lt) * m;
    for (int k = 0; k < 3; ++k) {
        f.move[k] = Follow(f.move[k], want[k], dt, t.smoothing);
        p.pos[k] += f.move[k] * dt;
    }
    // stick right turns right (yaw down), stick up looks up (pitch down)
    f.look[0] = Follow(f.look[0], pad.rx * t.lookSpeed, dt, t.smoothing * 0.6f);
    f.look[1] = Follow(f.look[1], pad.ry * t.lookSpeed * (t.invertLook ? -1.0f : 1.0f), dt, t.smoothing * 0.6f);
    p.yaw -= f.look[0] * dt;
    p.pitch -= f.look[1] * dt;
    if (p.pitch > 1.5f) p.pitch = 1.5f;
    if (p.pitch < -1.5f) p.pitch = -1.5f;
    const float rollWant = ((pad.buttons & kRight) ? 1.0f : 0.0f) - ((pad.buttons & kLeft) ? 1.0f : 0.0f);
    f.roll = Follow(f.roll, rollWant * t.rollSpeed, dt, t.smoothing);
    p.roll += f.roll * dt;
    const float fovWant = ((pad.buttons & kLb) ? 1.0f : 0.0f) - ((pad.buttons & kRb) ? 1.0f : 0.0f);
    f.fov = Follow(f.fov, fovWant * t.fovSpeed, dt, t.smoothing);
    p.fov += f.fov * dt;
    if (p.fov < t.minFov) p.fov = t.minFov;
    if (p.fov > t.maxFov) p.fov = t.maxFov;
}

// --- the camera path ---
struct Sample { float t = 0.0f; Pose pose; };
struct Path {
    std::vector<Sample> samples;
    float Duration() const { return samples.empty() ? 0.0f : samples.back().t; }
};
constexpr size_t kMaxSamples = 60 * 60 * 20;   // twenty minutes at 60 a second
// A sample at time t (seconds from the start); closer than minGap to the last one it is left out.
inline bool Add(Path& path, float t, const Pose& pose, float minGap = 1.0f / 120.0f) {
    if (path.samples.size() >= kMaxSamples) return false;
    if (!path.samples.empty() && t - path.samples.back().t < minGap) return false;
    path.samples.push_back(Sample{t, pose});
    return true;
}
inline float Wrap(float a) {
    const float twoPi = 6.2831853f;
    while (a > 3.14159265f) a -= twoPi;
    while (a < -3.14159265f) a += twoPi;
    return a;
}
inline float LerpAngle(float a, float b, float s) { return a + Wrap(b - a) * s; }
// The pose at time t, linear between the two samples around it (clamped to the ends).
inline Pose At(const Path& path, float t) {
    if (path.samples.empty()) return Pose{};
    if (t <= path.samples.front().t) return path.samples.front().pose;
    if (t >= path.samples.back().t) return path.samples.back().pose;
    size_t lo = 0, hi = path.samples.size() - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (path.samples[mid].t <= t) lo = mid; else hi = mid;
    }
    const Sample& a = path.samples[lo];
    const Sample& b = path.samples[hi];
    const float span = b.t - a.t;
    const float s = span > 0.0f ? (t - a.t) / span : 0.0f;
    Pose p;
    for (int k = 0; k < 3; ++k) p.pos[k] = a.pose.pos[k] + (b.pose.pos[k] - a.pose.pos[k]) * s;
    p.pitch = a.pose.pitch + (b.pose.pitch - a.pose.pitch) * s;
    p.yaw = LerpAngle(a.pose.yaw, b.pose.yaw, s);
    p.roll = LerpAngle(a.pose.roll, b.pose.roll, s);
    p.fov = a.pose.fov + (b.pose.fov - a.pose.fov) * s;
    return p;
}
// Text form: one "t x y z pitch yaw roll fov" line per sample.
inline std::string Save(const Path& path) {
    std::string out = "# trailer camera path: t x y z pitch yaw roll fov\n";
    char line[160];
    for (const Sample& s : path.samples) {
        std::snprintf(line, sizeof(line), "%.4f %.4f %.4f %.4f %.5f %.5f %.5f %.5f\n", s.t, s.pose.pos[0], s.pose.pos[1], s.pose.pos[2], s.pose.pitch, s.pose.yaw,
                      s.pose.roll, s.pose.fov);
        out += line;
    }
    return out;
}
inline Path Load(const std::string& text, unsigned& skipped) {
    Path path;
    skipped = 0;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.empty() || line[0] == '#' || line[0] == '\r') continue;
        Sample s;
        if (sscanf_s(line.c_str(), "%f %f %f %f %f %f %f %f", &s.t, &s.pose.pos[0], &s.pose.pos[1], &s.pose.pos[2], &s.pose.pitch, &s.pose.yaw, &s.pose.roll,
                        &s.pose.fov) != 8 ||
            !std::isfinite(s.t) || (!path.samples.empty() && s.t < path.samples.back().t) || path.samples.size() >= kMaxSamples) {
            ++skipped;
            continue;
        }
        path.samples.push_back(s);
    }
    return path;
}
}  // namespace coop_camtools
