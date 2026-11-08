// Dead Space 2 United by Richard Hafer.
// Trailer camera: stick shaping, flying, the path's samples, interpolation and its text form.
#include <cassert>
#include <cmath>
#include <cstdio>
#include "../src/coop_camtools.h"

using namespace coop_camtools;

namespace {
bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

void Sticks() {
    assert(Stick(0, 7849) == 0.0f && Stick(7000, 7849) == 0.0f && Stick(-7849, 7849) == 0.0f);
    assert(Near(Stick(32767, 7849), 1.0f) && Near(Stick(-32768, 7849), -1.0f));
    const float half = Stick(20308, 7849);
    assert(half > 0.45f && half < 0.55f);
    assert(Trigger(0, 30) == 0.0f && Trigger(30, 30) == 0.0f && Near(Trigger(255, 30), 1.0f));
}

void Flying() {
    Tuning t;
    t.smoothing = 0.0f;   // no lag: the stick is the velocity
    Pose p;
    Flight f;
    Pad pad;
    pad.ly = 1.0f;
    Fly(p, f, pad, 1.0f, 0.05f, t);
    // yaw 0, pitch 0: forward is +z
    assert(Near(p.pos[2], t.moveSpeed * 0.05f) && Near(p.pos[0], 0.0f) && Near(p.pos[1], 0.0f));
    Pad up;
    up.rt = 1.0f;
    Fly(p, f, up, 2.0f, 0.05f, t);
    assert(Near(p.pos[1], t.moveSpeed * 2.0f * 0.05f));
    Pad look;
    look.rx = 1.0f;
    look.ry = 1.0f;
    const float yawBefore = p.yaw, pitchBefore = p.pitch;
    Fly(p, f, look, 1.0f, 0.05f, t);
    assert(p.yaw < yawBefore && p.pitch < pitchBefore);   // stick right turns right (yaw down), stick up looks up
    // strafing right at yaw 0 goes to -x: the projection flips x
    Pose s;
    Flight sf;
    Pad strafe;
    strafe.lx = 1.0f;
    Fly(s, sf, strafe, 1.0f, 0.05f, t);
    assert(s.pos[0] < 0.0f && Near(s.pos[2], 0.0f));
    // pitch is clamped short of straight up and down
    for (int i = 0; i < 100; ++i) Fly(p, f, look, 1.0f, 0.1f, t);
    assert(p.pitch >= -1.5f && p.pitch <= 1.5f);
    // zoom and roll
    Pad zoom;
    zoom.buttons = kRb | kRight;
    const float fovBefore = p.fov;
    Fly(p, f, zoom, 1.0f, 0.1f, t);
    assert(p.fov < fovBefore && p.roll > 0.0f);
    for (int i = 0; i < 200; ++i) Fly(p, f, zoom, 1.0f, 0.1f, t);
    assert(Near(p.fov, t.minFov));
    // with smoothing the motion builds up instead of jumping
    Tuning soft;
    Pose q;
    Flight g;
    Fly(q, g, pad, 1.0f, 0.016f, soft);
    assert(q.pos[2] > 0.0f && q.pos[2] < soft.moveSpeed * 0.016f * 0.5f);
    // no time, no motion; a long hitch counts as 0.1 s
    Pose r;
    Flight h;
    Fly(r, h, pad, 1.0f, 0.0f, t);
    assert(r.pos[2] == 0.0f);
    Fly(r, h, pad, 1.0f, 5.0f, t);
    assert(Near(r.pos[2], t.moveSpeed * 0.1f));
}

void Matrix() {
    Pose p;
    float m[16];
    CameraToWorld(p, m);
    // level, yaw 0: left +x, up +y, forward +z, the position in the last row
    assert(Near(m[0], 1) && Near(m[5], 1) && Near(m[10], 1) && Near(m[15], 1) && Near(m[3], 0) && Near(m[7], 0) && Near(m[11], 0));
    p.pos[0] = 4.0f;
    p.yaw = 1.5707963f;   // a quarter turn left: forward is +x
    CameraToWorld(p, m);
    assert(Near(m[8], 1.0f) && Near(m[10], 0.0f) && Near(m[12], 4.0f));
    p.pitch = 0.5f;       // positive pitch looks down
    CameraToWorld(p, m);
    assert(m[9] < 0.0f);
    // the rows stay orthonormal with roll
    p.roll = 0.7f;
    CameraToWorld(p, m);
    for (int a = 0; a < 3; ++a) {
        float len = 0.0f;
        for (int k = 0; k < 3; ++k) len += m[a * 4 + k] * m[a * 4 + k];
        assert(Near(len, 1.0f));
        for (int b = a + 1; b < 3; ++b) {
            float dot = 0.0f;
            for (int k = 0; k < 3; ++k) dot += m[a * 4 + k] * m[b * 4 + k];
            assert(Near(dot, 0.0f));
        }
    }
    // the FOV is widened from 16:9 to the screen, and left alone without conversion
    assert(Near(RenderFov(1.2217305f, 1, 2, 1.7777778f), 1.2217305f));
    assert(RenderFov(1.2217305f, 1, 2, 2.3333333f) > 1.2217305f);
    assert(RenderFov(1.2217305f, 0, 2, 1.7777778f) > 1.2217305f);   // authored for 4:3, shown wider
    assert(Near(RenderFov(1.0f, 1, 0, 2.3333333f), 1.0f));
}

void Paths() {
    Path path;
    assert(path.Duration() == 0.0f);
    Pose a, b;
    a.pos[0] = 0.0f;
    a.yaw = 3.0f;
    b.pos[0] = 10.0f;
    b.yaw = -3.0f;   // across the wrap: the short way round
    b.fov = 0.5f;
    assert(Add(path, 0.0f, a));
    assert(!Add(path, 0.001f, a));   // too close to the last sample
    assert(Add(path, 2.0f, b));
    assert(Near(path.Duration(), 2.0f));
    const Pose mid = At(path, 1.0f);
    assert(Near(mid.pos[0], 5.0f));
    assert(std::fabs(Wrap(mid.yaw - 3.14159265f)) < 0.01f);   // halfway between 3 and -3 the short way is pi
    assert(Near(mid.fov, (a.fov + b.fov) * 0.5f));
    assert(Near(At(path, -1.0f).pos[0], 0.0f) && Near(At(path, 9.0f).pos[0], 10.0f));
    unsigned skipped = 0;
    const Path back = Load(Save(path) + "garbage line\n1.0 1 2 3 4 5 6 7\n", skipped);
    assert(back.samples.size() == 2 && skipped == 2);   // the garbage, and a time going backwards
    assert(Near(back.samples[1].pose.pos[0], 10.0f) && Near(back.samples[1].pose.yaw, -3.0f));
    assert(Load("", skipped).samples.empty() && skipped == 0);
}
}  // namespace

int main() {
    Sticks();
    Flying();
    Matrix();
    Paths();
    std::printf("coop_camtools tests passed\n");
    return 0;
}
