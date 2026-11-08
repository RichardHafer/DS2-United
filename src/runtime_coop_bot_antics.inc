// Dead Space 2 United by Richard Hafer.
// --- antics (DS2BotProfiles.ini Antics, HarmfulAntics; [Bot] AnticsEnabled) ----------------
// Now and then, with nothing about, a bot does something playful: taps its aim at
// the leader's face, stares at him, stomps its own fresh kill, shadow boxes, or
// (HarmfulAntics=1) puts a stasis shot on him. coop_bot_brain.h decides what and
// when; this part tells it what keeps the bot busy, which kills were its own and
// whether the line to the leader is clear, and logs every start, end and abort.
bool anticsEnabled = true;
volatile LONG anticLines = 0, ownKills = 0, anticLineRays = 0, anticLinesClear = 0, anticDirectFrames = 0;
bool AnticLog() { return InterlockedIncrement(&anticLines) <= 300; }

// The target died: its health component names the last attacker (entity + 0x10).
void NoteKill(const coop_bot::Enemy& e, DWORD now) {
    if (u->brain.tune.antics <= 0 || !e.id || u->killId == e.id) return;
    uint32_t health = drops::ComponentOf(e.id, drops::kHealthTypeRva), attacker = 0;
    if (!health) health = u->targetHealth;
    if (!health || !Read(health + drops::kLastAttacker, &attacker, 4) || attacker != u->entity + 0x10) return;
    u->killId = e.id;
    u->killAt = now;
    u->killPos = e.pos;
    u->killPosAt = now;
    InterlockedIncrement(&ownKills);
}

// What the runtime has the bot in that rules an antic out (nullptr: nothing).
const char* AnticBusy(DWORD now) {
    if (holdActive) return "restart hold";
    if (u->sceneSince || u->hiddenBody || cutscene::SequenceRunningNow()) return "scene";
    if (u->flying || u->zeroG || leaderFlying || leaderZeroG) return "zero-G";
    if (u->kioskVisit != KioskStep::None) return "kiosk";
    if (u->storeWaiting) return "store wait";
    {
        // The leader at a store or bench: nobody steps between him and it (07.10.).
        int32_t state = -1;
        bool store = false;
        const uint32_t leader = Leader();
        if (leader && KioskMachine(leader, state, store)) return "leader at a kiosk";
    }
    if (u->face.step != FaceStep::None) return "wall turn";
    if (u->tkStep != TkStep::None) return "kinesis";
    if (u->liftHoldUntil && now < u->liftHoldUntil) return "lift hold";
    if (leaderInCabin) return "lift";
    if (leaderBusy) return "leader climbing";
    if (leaderChase) return "chase";
    if (u->holding || u->goingThere) return "hold order";
    return nullptr;
}

void AnticsBefore(uint32_t leader, const Vec3& self, const Vec3& leaderPos, DWORD now, coop_bot::Perception& p) {
    const coop_bot::Tuning& t = u->brain.tune;
    if (t.antics <= 0) return;
    p.busy = AnticBusy(now);
    p.killId = u->killId;
    p.killAt = u->killAt;
    // Where its corpse lies now (the ragdoll slides), twice a second while it is stomped.
    if (u->killId && u->brain.AnticNow() == coop_bot::Antic::VictoryStomp && now - u->killPosAt >= 500) {
        Vec3 center;
        bot_sense::CorpseCenter(u->killId, u->killPos, center);
        u->killPos = center;
        u->killPosAt = now;
    }
    p.killPos = u->killPos;
    // The line to the leader's chest, only for the stasis prank. A faulted cast counts as unknown.
    if (t.harmfulAntics) {
        if (!u->lineAt || now - u->lineAt >= 300) {
            u->lineAt = now;
            u->leaderLine = -1;
            if (bot_sense::losReady) {
                const Vec3 chest = {leaderPos.x, leaderPos.y + t.anticChest, leaderPos.z};
                const bool clear = bot_sense::Clear(CameraEye(self), chest, u->entity, leader);
                if (bot_sense::losReady) u->leaderLine = clear ? 1 : 0;
                InterlockedIncrement(&anticLineRays);
                if (u->leaderLine == 1) InterlockedIncrement(&anticLinesClear);
            }
        }
        p.leaderLine = u->leaderLine;
    }
    for (const Unit& other : units) {
        const uint32_t at = other.brain.LastPrankAt();
        if (at && (!p.squadPrankAt || static_cast<LONG>(at - p.squadPrankAt) > 0)) p.squadPrankAt = at;
    }
}

void LogAntics(float leaderGap) {
    coop_bot::AnticEvent ev[6];
    const int n = u->brain.TakeAnticEvents(ev, 6);
    for (int i = 0; i < n; ++i) {
        const coop_bot::AnticEvent& e = ev[i];
        if (!AnticLog()) continue;
        const char* kind = coop_bot::AnticName(e.antic);
        if (e.kind == coop_bot::AnticEvent::Start) {
            char could[96] = "";
            size_t used = 0;
            for (int k = 1; k < coop_bot::kAntics; ++k) {
                if (!(e.eligible & (1u << k)) || used >= sizeof(could)) continue;
                const int w = std::snprintf(could + used, sizeof(could) - used, "%s%s", used ? " " : "", coop_bot::AnticName(static_cast<coop_bot::Antic>(k)));
                if (w > 0) used += static_cast<size_t>(w);
            }
            gate::Event("bot %u (%s): antic %s starts (%s; could pick %s), %.1f m from the leader", u->index + 1, u->name, kind, e.why, could, leaderGap);
            continue;
        }
        char more[96] = "";
        switch (e.antic) {
        case coop_bot::Antic::AimSpam: std::snprintf(more, sizeof(more), ", aim taps %d", e.taps); break;
        case coop_bot::Antic::VictoryStomp: std::snprintf(more, sizeof(more), ", stomps %d", e.stomps); break;
        case coop_bot::Antic::ShadowBoxing:
            std::snprintf(more, sizeof(more), ", spins %d, swings %d, held back %d (leader near or before it)", e.spins, e.swings, e.swingsSkipped);
            break;
        case coop_bot::Antic::StasisPrank:
            std::snprintf(more, sizeof(more), ", stasis shot %s, energy %.2f%s", e.stasisFired ? "fired" : "not fired", u->stasis01,
                          u->stasisKnown ? "" : " (unknown)");
            break;
        default: break;
        }
        gate::Event("bot %u (%s): antic %s %s (%s) after %lu ms%s", u->index + 1, u->name, kind,
                    e.kind == coop_bot::AnticEvent::Finish ? "finished" : "aborted", e.why, static_cast<unsigned long>(e.ms), more);
    }
}

// The runtime takes the bot elsewhere (a scene, a kiosk, a new body).
void StopAntic(const char* why) {
    if (u->brain.AnticNow() == coop_bot::Antic::None) return;
    u->brain.StopAntic(GetTickCount(), why);
    LogAntics(-1.0f);
}

// After the brain: the store wait takes the frame over; then the log.
void AnticsAfter(const Vec3& self, const Vec3& leaderPos, DWORD now) {
    if (u->storeWaiting) u->brain.StopAntic(now, "store wait");
    if (u->command.direct && u->command.move) InterlockedIncrement(&anticDirectFrames);
    LogAntics(coop_bot::Flat(self, leaderPos));
}

void WriteAnticDiagnostics(FILE* log) {
    bool on = false;
    coop_bot::AnticStats s;
    for (const Unit& one : units) {
        if (one.brain.tune.antics <= 0) continue;
        on = true;
        const coop_bot::AnticStats& o = one.brain.AnticCounts();
        for (int k = 0; k < coop_bot::kAntics; ++k) { s.started[k] += o.started[k]; s.finished[k] += o.finished[k]; s.aborted[k] += o.aborted[k]; }
        for (int k = 0; k < coop_bot::kAnticSkips; ++k) s.rejected[k] += o.rejected[k];
        s.taps += o.taps;
        s.stomps += o.stomps;
        s.swings += o.swings;
        s.swingsSkipped += o.swingsSkipped;
        s.stasisShots += o.stasisShots;
    }
    if (!on) return;
    // Retries while not calm or a mate is busy tick on by themselves: they do not trigger a line.
    uint32_t sig = static_cast<uint32_t>(ownKills) * 7u + s.taps + s.stomps + s.swings + s.swingsSkipped + s.stasisShots;
    for (int k = 0; k < coop_bot::kAntics; ++k) sig += s.started[k] * 31u + s.finished[k] * 37u + s.aborted[k] * 41u;
    for (int k = 0; k < coop_bot::kAnticSkips; ++k)
        if (k != static_cast<int>(coop_bot::AnticSkip::NotCalm) && k != static_cast<int>(coop_bot::AnticSkip::MateBusy)) sig += s.rejected[k] * 43u;
    static uint32_t last = ~0u;
    static DWORD at = 0;
    const DWORD now = GetTickCount();
    if (sig == last || (at && now - at < 10000)) return;
    last = sig;
    at = now;
    const uint32_t* r = s.rejected;
    std::fprintf(log, "bot antics (aim_spam,stare,victory_stomp,shadow_boxing,stasis_prank) started=%u,%u,%u,%u,%u finished=%u,%u,%u,%u,%u "
                      "aborted=%u,%u,%u,%u,%u rejected cooldown=%u not_calm=%u mate_busy=%u no_kill=%u no_stasis=%u no_line=%u leader_close=%u "
                      "hold_fire=%u no_heading=%u aim_taps=%u stomps=%u swings=%u swings_held_back=%u stasis_shots=%u own_kills=%ld line_rays=%ld "
                      "line_clear=%ld direct_frames=%ld event_lines=%ld active=%s,%s,%s\n",
                 s.started[1], s.started[2], s.started[3], s.started[4], s.started[5], s.finished[1], s.finished[2], s.finished[3], s.finished[4],
                 s.finished[5], s.aborted[1], s.aborted[2], s.aborted[3], s.aborted[4], s.aborted[5], r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8],
                 s.taps, s.stomps, s.swings, s.swingsSkipped, s.stasisShots, ownKills, anticLineRays, anticLinesClear, anticDirectFrames, anticLines,
                 coop_bot::AnticName(units[0].brain.AnticNow()), coop_bot::AnticName(units[1].brain.AnticNow()), coop_bot::AnticName(units[2].brain.AnticNow()));
}
