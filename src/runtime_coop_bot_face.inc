// Dead Space 2 United by Hafer.
// --- facing open space ([Bot] FaceOpenSpace) -------------------------------------------
// A bot that has come to rest (no walk, no fight, no kiosk, lift or scene)
// with a wall or the end of the walkable mesh right before its face turns toward
// the most open way once per rest (opposite first), takes a short step and then
// stands facing the room. The wall test is the cameras' own sphere cast
// (CollisionCamera::CastingTest, bot_sense::CastRaw) from eye height; the mesh
// test asks NavPower (bot_nav::PathEnd) how far a walk that way really gets.
bool faceOpen = false;
volatile LONG faceChecks = 0, faceWalls = 0, faceTurns = 0, faceSteps = 0, faceSkips = 0, faceAborts = 0, faceLines = 0;
constexpr DWORD kFaceRestMs = 1000, kFaceCooldownMs = 8000, kFaceTurnMs = 1500, kFaceStepMs = 300;
constexpr float kFaceEye = 1.5f, kFaceWallCast = 1.5f, kFaceProbe = 4.0f, kFaceNavAhead = 1.5f, kFaceNavProbe = 3.0f;

// Free metres from the eye along yaw, up to len; -1 when nothing is hit. A
// Player in the way (the leader, a bot) is no wall.
float FaceCast(const Vec3& eye, float yaw, float len) {
    if (!bot_sense::losReady) return -1.0f;
    const Vec3 d = coop_bot::FromYaw(yaw);
    alignas(16) float from[4] = {eye.x, eye.y, eye.z, 1.0f}, to[4] = {eye.x + d.x * len, eye.y, eye.z + d.z * len, 1.0f};
    float fraction = 1.0f;
    uint32_t hit = 0;
    if (!bot_sense::CastRaw(from, to, u->entity, fraction, hit)) { bot_sense::NoteLosFault(); return -1.0f; }
    if (fraction >= 0.999f || (hit && (hit == Leader() || IsBotEntity(hit)))) return -1.0f;
    return fraction * len;
}
// How far NavPower lets it walk along yaw (up to len); -1 unknown (no path from here).
float FaceNav(const Vec3& self, float yaw, float len) {
    if (!bot_nav::ready) return -1.0f;
    const Vec3 goal = coop_bot::Add(self, coop_bot::Scale(coop_bot::FromYaw(yaw), len));
    Vec3 end;
    int n = 0;
    if (!bot_nav::PathEnd(self, goal, end, n)) return -1.0f;
    return coop_bot::Flat(end, goal) < 0.5f ? len : coop_bot::Flat(self, end);
}
bool FaceLog() { return InterlockedIncrement(&faceLines) <= 120; }
float FaceHeading(const coop_bot::Perception& p) {
    float heading = 0.0f, pitch = 0.0f;
    if (AimAngles(u->entity, kCamAim, heading, pitch)) return heading;
    return p.selfYawKnown ? p.selfYaw : bot_sense::Yaw(u->entity);
}
void FaceEvaluate(const Vec3& self, const Vec3& leaderPos, const coop_bot::Perception& p, DWORD now) {
    InterlockedIncrement(&faceChecks);
    const float heading = FaceHeading(p);
    const Vec3 eye = {self.x, self.y + kFaceEye, self.z};
    coop_bot::FaceOpenInput in;
    in.wallAhead = FaceCast(eye, heading, kFaceWallCast);
    const float navAhead = FaceNav(self, heading, kFaceNavAhead);
    in.navShort = navAhead >= 0.0f && navAhead < kFaceNavAhead - 0.6f;
    if (!((in.wallAhead >= 0.0f && in.wallAhead < 1.2f) || in.navShort)) return;
    InterlockedIncrement(&faceWalls);
    in.selfYaw = heading;
    in.count = coop_bot::FaceCandidates(heading, in.yaws);
    for (int i = 0; i < in.count; ++i) {
        const float cast = FaceCast(eye, in.yaws[i], kFaceProbe);
        const float nav = FaceNav(self, in.yaws[i], kFaceNavProbe);
        float clear = cast < 0.0f ? kFaceProbe : cast;
        if (nav >= 0.0f && nav < clear) clear = nav;
        in.clear[i] = clear;
    }
    in.self = self;
    in.leader = leaderPos;
    in.holding = u->holding;
    in.holdSpot = u->holdSpot;
    const coop_bot::FaceOpenOutput out = coop_bot::DecideFaceOpen(in);
    if (!out.turn) {
        InterlockedIncrement(&faceSkips);
        if (FaceLog())
            gate::Event("bot %u: faces a wall (%.1f m ahead%s) but stays: %s", u->index + 1, in.wallAhead, in.navShort ? ", the mesh ends" : "", out.why);
        return;
    }
    InterlockedIncrement(&faceTurns);
    u->face.step = FaceStep::Turn;
    u->face.stepAt = now;
    u->face.yaw = out.yaw;
    u->face.walk = out.step;
    u->face.lastAt = now;
    if (FaceLog())
        gate::Event("bot %u: faces a wall %.1f m ahead%s: turns to %.0f deg (was %.0f), %.1f m clear that way%s", u->index + 1, in.wallAhead,
                    in.navShort ? " (the mesh ends)" : "", Wrap(out.yaw) * 57.29578f, Wrap(heading) * 57.29578f, out.clear,
                    out.step ? ", then a short step" : ", no step (its hold spot)");
}
void FaceTick(const Vec3& self, const Vec3& leaderPos, const coop_bot::Perception& p, DWORD now) {
    if (!faceOpen) return;
    FaceUnit& f = u->face;
    coop_bot::Command& c = u->command;
    const bool calm = !c.move && !c.aim && !c.use && !c.fire && !c.melee && (c.mode == coop_bot::Mode::Idle || c.mode == coop_bot::Mode::Follow) &&
                      p.enemyCount == 0 && !p.grabbed && !u->flying && !u->zeroG && u->kioskVisit == KioskStep::None &&
                      !(u->liftHoldUntil && now < u->liftHoldUntil) && !u->sceneSince && u->lowerState != 30 && u->tkStep == TkStep::None &&
                      !u->storeWaiting && u->brain.AnticNow() == coop_bot::Antic::None;
    if (!calm) {
        if (f.step != FaceStep::None) {
            InterlockedIncrement(&faceAborts);
            if (FaceLog()) gate::Event("bot %u: turn from the wall broken off (it has something to do)", u->index + 1);
        }
        const DWORD last = f.lastAt;
        f = FaceUnit{};
        f.lastAt = last;
        return;
    }
    if (!f.restSince || (f.step == FaceStep::None && coop_bot::Flat(self, f.restFrom) > 0.3f)) {
        f.restSince = now;
        f.restFrom = self;
        f.done = f.keepLook = false;
    }
    const Vec3 eye = {self.x, self.y + kEye, self.z};
    const Vec3 ahead = coop_bot::Add(eye, coop_bot::Scale(coop_bot::FromYaw(f.yaw), 5.0f));
    switch (f.step) {
    case FaceStep::None:
        if (!f.done && now - f.restSince >= kFaceRestMs && (!f.lastAt || now - f.lastAt >= kFaceCooldownMs)) {
            f.done = true;
            FaceEvaluate(self, leaderPos, p, now);
        }
        break;
    case FaceStep::Turn: {
        c.look = true;
        c.lookAt = ahead;
        float heading = 0.0f, pitch = 0.0f;
        const bool known = AimAngles(u->entity, kCamAim, heading, pitch);
        if ((known && std::fabs(Wrap(heading - f.yaw)) < 0.3f) || now - f.stepAt > kFaceTurnMs) {
            f.step = f.walk ? FaceStep::Step : FaceStep::None;
            f.stepAt = now;
            if (!f.walk) { f.keepLook = true; f.restFrom = self; }
        }
        break;
    }
    case FaceStep::Step:
        c.look = true;
        c.lookAt = ahead;
        if (now - f.stepAt >= kFaceStepMs) {
            InterlockedIncrement(&faceSteps);
            f.step = FaceStep::None;
            f.keepLook = true;
            f.restFrom = self;
        }
        break;
    }
    // Done: it stands facing the room for the rest of this rest.
    if (f.keepLook && f.step == FaceStep::None) {
        c.look = true;
        c.lookAt = ahead;
    }
}
// The short step: straight ahead, the way the camera now looks.
void FaceAfterAct(DWORD now) {
    (void)now;
    if (!faceOpen || u->face.step != FaceStep::Step) return;
    u->pad.lx = 0.0f;
    u->pad.ly = u->cal.forwardSign * 0.6f;
    u->padLive = true;
}
void WriteFaceDiagnostics(FILE* log) {
    if (!faceOpen) return;
    static LONG last = -1;
    const LONG sum = faceChecks + faceWalls + faceTurns + faceSteps + faceSkips + faceAborts;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "bot face_open checks=%ld walls=%ld turns=%ld steps=%ld skips=%ld aborts=%ld\n", faceChecks, faceWalls, faceTurns, faceSteps, faceSkips,
                 faceAborts);
}
